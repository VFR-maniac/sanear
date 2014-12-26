#include "pch.h"
#include "DeviceManager.h"

#include "DspMatrix.h"

namespace SaneAudioRenderer
{
    namespace
    {
        const auto WindowClass = L"SaneAudioRenderer::DeviceManager";
        const auto WindowTitle = L"";

        enum
        {
            WM_CHECK_BITSTREAM_FORMAT = WM_USER + 100,
            WM_CREATE_DEVICE,
        };

        template <class T>
        bool IsLastInstance(T& smartPointer)
        {
            bool ret = (smartPointer.GetInterfacePtr()->AddRef() == 2);
            smartPointer.GetInterfacePtr()->Release();
            return ret;
        }

        WAVEFORMATEXTENSIBLE BuildFormat(GUID formatGuid, uint32_t formatBits, WORD formatExtProps,
                                         uint32_t rate, uint32_t channelCount, DWORD channelMask)
        {
            WAVEFORMATEXTENSIBLE ret;
            ret.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
            ret.Format.nChannels = channelCount;
            ret.Format.nSamplesPerSec = rate;
            ret.Format.nAvgBytesPerSec = formatBits / 8 * rate * channelCount;
            ret.Format.nBlockAlign = formatBits / 8 * channelCount;
            ret.Format.wBitsPerSample = formatBits;
            ret.Format.cbSize = 22;
            ret.Samples.wValidBitsPerSample = formatExtProps;
            ret.dwChannelMask = channelMask;
            ret.SubFormat = formatGuid;
            return ret;
        }

        std::shared_ptr<std::wstring> GetDevicePropertyString(IPropertyStore* pStore, REFPROPERTYKEY key)
        {
            assert(pStore);

            PROPVARIANT prop;
            PropVariantInit(&prop);
            ThrowIfFailed(pStore->GetValue(key, &prop));
            auto ret = std::make_shared<std::wstring>(prop.pwszVal);
            PropVariantClear(&prop);

            return ret;
        }

        void CreateAudioClient(AudioDevice& output, ISettings* pSettings)
        {
            assert(pSettings);

            std::unique_ptr<wchar_t, CoTaskMemFreeDeleter> deviceName;

            {
                output.settingsSerial = pSettings->GetSerial();

                LPWSTR pDeviceName = nullptr;
                BOOL exclusive;
                ThrowIfFailed(pSettings->GetOuputDevice(&pDeviceName, &exclusive));

                deviceName.reset(pDeviceName);
                output.exclusive = !!exclusive;
            }

            IMMDeviceEnumeratorPtr enumerator;
            ThrowIfFailed(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                           CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&enumerator)));

            IMMDevicePtr device;
            IPropertyStorePtr devicePropertyStore;

            if (!deviceName || !*deviceName)
            {
                output.default = true;
                ThrowIfFailed(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device));
                ThrowIfFailed(device->OpenPropertyStore(STGM_READ, &devicePropertyStore));
                output.friendlyName = GetDevicePropertyString(devicePropertyStore, PKEY_Device_FriendlyName);
            }
            else
            {
                output.default = false;

                IMMDeviceCollectionPtr collection;
                ThrowIfFailed(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection));

                UINT count = 0;
                ThrowIfFailed(collection->GetCount(&count));

                for (UINT i = 0; i < count; i++)
                {
                    ThrowIfFailed(collection->Item(i, &device));
                    ThrowIfFailed(device->OpenPropertyStore(STGM_READ, &devicePropertyStore));
                    output.friendlyName = GetDevicePropertyString(devicePropertyStore, PKEY_Device_FriendlyName);

                    if (wcscmp(deviceName.get(), output.friendlyName->c_str()))
                    {
                        device = nullptr;
                        devicePropertyStore = nullptr;
                        output.friendlyName = nullptr;
                    }
                }
            }

            if (!device)
                return;

            output.adapterName = GetDevicePropertyString(devicePropertyStore, PKEY_DeviceInterface_FriendlyName);
            output.endpointName = GetDevicePropertyString(devicePropertyStore, PKEY_Device_DeviceDesc);

            ThrowIfFailed(device->Activate(__uuidof(IAudioClient),
                                           CLSCTX_INPROC_SERVER, nullptr, (void**)&output.audioClient));
        }
    }

    DeviceManager::DeviceManager(HRESULT& result)
    {
        if (FAILED(result))
            return;

        m_hThread = (HANDLE)_beginthreadex(nullptr, 0, StaticThreadProc<DeviceManager>, this, 0, nullptr);

        if (m_hThread == NULL || !m_windowInitialized.get_future().get())
            result = E_FAIL;
    }

    DeviceManager::~DeviceManager()
    {
        m_queuedDestroy = true;
        PostMessage(m_hWindow, WM_DESTROY, 0, 0);
        WaitForSingleObject(m_hThread, INFINITE);
        CloseHandle(m_hThread);
        UnregisterClass(WindowClass, GetModuleHandle(nullptr));
    }

    bool DeviceManager::BitstreamFormatSupported(SharedWaveFormat format, ISettings* pSettings)
    {
        assert(format);
        assert(pSettings);

        m_checkBitstreamFormat = format;
        m_checkBitstreamSettings = pSettings;
        m_queuedCheckBitstream = true;

        bool ret = (SendMessage(m_hWindow, WM_CHECK_BITSTREAM_FORMAT, 0, 0) == 0);

        m_checkBitstreamFormat = nullptr;
        m_checkBitstreamSettings = nullptr;
        assert(m_queuedCheckBitstream == false);

        return ret;
    }

    bool DeviceManager::CreateDevice(AudioDevice& device, SharedWaveFormat format, ISettings* pSettings)
    {
        assert(format);
        assert(pSettings);

        m_createDeviceFormat = format;
        m_createDeviceSettings = pSettings;
        m_queuedCreateDevice = true;

        bool ret = (SendMessage(m_hWindow, WM_CREATE_DEVICE, 0, 0) == 0);

        m_createDeviceFormat = nullptr;
        m_createDeviceSettings = nullptr;
        assert(m_queuedCreateDevice == false);

        device = m_device;

        return ret;
    }

    void DeviceManager::ReleaseDevice()
    {
        auto areLastInstances = [this]
        {
            if (m_device.audioClock && !IsLastInstance(m_device.audioClock))
                return false;

            m_device.audioClock = nullptr;

            if (m_device.audioRenderClient && !IsLastInstance(m_device.audioRenderClient))
                return false;

            m_device.audioRenderClient = nullptr;

            if (m_device.audioClient && !IsLastInstance(m_device.audioClient))
                return false;

            return true;
        };
        assert(areLastInstances());

        m_device = {};
    }

    LRESULT DeviceManager::OnCheckBitstreamFormat()
    {
        if (!m_queuedCheckBitstream)
            return 1;

        assert(m_checkBitstreamFormat);
        assert(m_checkBitstreamSettings);
        m_queuedCheckBitstream = false;

        try
        {
            AudioDevice device = {};

            CreateAudioClient(device, m_checkBitstreamSettings);

            if (!m_device.audioClient)
                return 1;

            return SUCCEEDED(m_device.audioClient->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE,
                                                                     &(*m_checkBitstreamFormat), nullptr)) ? 0 : 1;
        }
        catch (HRESULT)
        {
            return 1;
        }
    }

    LRESULT DeviceManager::OnCreateDevice()
    {
        if (!m_queuedCreateDevice)
            return 1;

        assert(m_createDeviceFormat);
        assert(m_createDeviceSettings);
        m_queuedCreateDevice = false;

        ReleaseDevice();
        try
        {
            CreateAudioClient(m_device, m_createDeviceSettings);

            if (!m_device.audioClient)
                return 1;

            WAVEFORMATEX* pFormat;
            ThrowIfFailed(m_device.audioClient->GetMixFormat(&pFormat));
            SharedWaveFormat mixFormat(pFormat, CoTaskMemFreeDeleter());

            m_device.bufferDuration = 200;

            if (DspFormatFromWaveFormat(*m_createDeviceFormat) == DspFormat::Unknown)
            {
                // Exclusive bitstreaming.
                if (!m_device.exclusive)
                    return 1;

                m_device.dspFormat = DspFormat::Unknown;
                m_device.format = m_createDeviceFormat;
            }
            else if (m_device.exclusive)
            {
                // Exclusive.
                auto priorities = make_array(
                    std::make_pair(DspFormat::Float, BuildFormat(KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, 32, 32, m_createDeviceFormat->nSamplesPerSec, mixFormat->nChannels, DspMatrix::GetChannelMask(*mixFormat))),
                    std::make_pair(DspFormat::Pcm32, BuildFormat(KSDATAFORMAT_SUBTYPE_PCM,        32, 32, m_createDeviceFormat->nSamplesPerSec, mixFormat->nChannels, DspMatrix::GetChannelMask(*mixFormat))),
                    std::make_pair(DspFormat::Pcm24, BuildFormat(KSDATAFORMAT_SUBTYPE_PCM,        24, 24, m_createDeviceFormat->nSamplesPerSec, mixFormat->nChannels, DspMatrix::GetChannelMask(*mixFormat))),
                    std::make_pair(DspFormat::Pcm32, BuildFormat(KSDATAFORMAT_SUBTYPE_PCM,        32, 24, m_createDeviceFormat->nSamplesPerSec, mixFormat->nChannels, DspMatrix::GetChannelMask(*mixFormat))),
                    std::make_pair(DspFormat::Pcm16, BuildFormat(KSDATAFORMAT_SUBTYPE_PCM,        16, 16, m_createDeviceFormat->nSamplesPerSec, mixFormat->nChannels, DspMatrix::GetChannelMask(*mixFormat))),

                    std::make_pair(DspFormat::Float, BuildFormat(KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, 32, 32, mixFormat->nSamplesPerSec, mixFormat->nChannels, DspMatrix::GetChannelMask(*mixFormat))),
                    std::make_pair(DspFormat::Pcm32, BuildFormat(KSDATAFORMAT_SUBTYPE_PCM,        32, 32, mixFormat->nSamplesPerSec, mixFormat->nChannels, DspMatrix::GetChannelMask(*mixFormat))),
                    std::make_pair(DspFormat::Pcm24, BuildFormat(KSDATAFORMAT_SUBTYPE_PCM,        24, 24, mixFormat->nSamplesPerSec, mixFormat->nChannels, DspMatrix::GetChannelMask(*mixFormat))),
                    std::make_pair(DspFormat::Pcm32, BuildFormat(KSDATAFORMAT_SUBTYPE_PCM,        32, 24, mixFormat->nSamplesPerSec, mixFormat->nChannels, DspMatrix::GetChannelMask(*mixFormat))),
                    std::make_pair(DspFormat::Pcm16, BuildFormat(KSDATAFORMAT_SUBTYPE_PCM,        16, 16, mixFormat->nSamplesPerSec, mixFormat->nChannels, DspMatrix::GetChannelMask(*mixFormat)))
                );

                for (const auto& f : priorities)
                {
                    if (SUCCEEDED(m_device.audioClient->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &f.second.Format, nullptr)))
                    {
                        m_device.dspFormat = f.first;
                        m_device.format = CopyWaveFormat(f.second.Format);
                        break;
                    }
                }
            }
            else
            {
                // Shared.
                m_device.dspFormat = DspFormat::Float;
                m_device.format = mixFormat;
            }

            ThrowIfFailed(m_device.audioClient->Initialize(m_device.exclusive ? AUDCLNT_SHAREMODE_EXCLUSIVE : AUDCLNT_SHAREMODE_SHARED,
                                                           0, MILLISECONDS_TO_100NS_UNITS(m_device.bufferDuration),
                                                           0, &(*m_device.format), nullptr));

            ThrowIfFailed(m_device.audioClient->GetService(IID_PPV_ARGS(&m_device.audioRenderClient)));

            ThrowIfFailed(m_device.audioClient->GetService(IID_PPV_ARGS(&m_device.audioClock)));

            return 0;
        }
        catch (std::bad_alloc&)
        {
            ReleaseDevice();
            return 1;
        }
        catch (HRESULT)
        {
            ReleaseDevice();
            return 1;
        }
    }

    DWORD DeviceManager::ThreadProc()
    {
        CoInitializeHelper coInitializeHelper(COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

        HINSTANCE hInstance = GetModuleHandle(nullptr);

        WNDCLASSEX windowClass{sizeof(windowClass), 0, StaticWindowProc<DeviceManager>, 0, 0, hInstance,
                               NULL, NULL, NULL, nullptr, WindowClass, NULL};

        m_hWindow = NULL;
        if (coInitializeHelper.Initialized() && RegisterClassEx(&windowClass))
            m_hWindow = CreateWindowEx(0, WindowClass, WindowTitle, 0, 0, 0, 0, 0, 0, NULL, hInstance, this);

        if (m_hWindow != NULL)
        {
            m_windowInitialized.set_value(true);
            RunMessageLoop();
            ReleaseDevice();
        }
        else
        {
            m_windowInitialized.set_value(false);
        }

        return 0;
    }

    LRESULT DeviceManager::WindowProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        switch (msg)
        {
            case WM_DESTROY:
                if (m_queuedDestroy)
                    PostQuitMessage(0);
                return 0;

            case WM_CHECK_BITSTREAM_FORMAT:
                return OnCheckBitstreamFormat();

            case WM_CREATE_DEVICE:
                return OnCreateDevice();

            default:
                return DefWindowProc(hWnd, msg, wParam, lParam);
        }
    }
}

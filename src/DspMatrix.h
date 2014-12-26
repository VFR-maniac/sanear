#pragma once

#include "DspChunk.h"

namespace SaneAudioRenderer
{
    class DspMatrix final
    {
    public:

        DspMatrix() = default;
        DspMatrix(const DspMatrix&) = delete;
        DspMatrix& operator=(const DspMatrix&) = delete;

        std::wstring Name() { return L"Matrix"; }

        void Initialize(uint32_t inputChannels, DWORD inputMask,
                        uint32_t outputChannels, DWORD outputMask);
        bool Active();

        void Process(DspChunk& chunk);
        void Finish(DspChunk& chunk);

        static DWORD GetChannelMask(const WAVEFORMATEX& format);

    private:

        std::unique_ptr<float[]> m_matrix;
        uint32_t m_inputChannels = 0;
        uint32_t m_outputChannels = 0;
    };
}

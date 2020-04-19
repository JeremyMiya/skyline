// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include <kernel/types/KProcess.h>
#include "voice.h"

namespace skyline::service::audio::IAudioRenderer {
    void Voice::SetWaveBufferIndex(u8 index) {
        bufferIndex = static_cast<u8>(index & 3);
        bufferReload = true;
    }

    Voice::Voice(const DeviceState &state) : state(state) {}

    void Voice::ProcessInput(const VoiceIn &input) {
        // Voice no longer in use, reset it
        if (acquired && !input.acquired) {
            bufferReload = true;
            bufferIndex = 0;
            sampleOffset = 0;

            output.playedSamplesCount = 0;
            output.playedWaveBuffersCount = 0;
            output.voiceDropsCount = 0;
        }

        acquired = input.acquired;

        if (!acquired)
            return;

        if (input.firstUpdate) {
            if (input.format != skyline::audio::AudioFormat::Int16)
                throw exception("Unsupported voice PCM format: {}", input.format);

            format = input.format;
            sampleRate = input.sampleRate;

            if (input.channelCount > 2)
                throw exception("Unsupported voice channel count: {}", input.channelCount);

            channelCount = static_cast<u8>(input.channelCount);

            SetWaveBufferIndex(static_cast<u8>(input.baseWaveBufferIndex));
        }

        waveBuffers = input.waveBuffers;
        volume = input.volume;
        playbackState = input.playbackState;
    }

    void Voice::UpdateBuffers() {
        const auto &currentBuffer = waveBuffers.at(bufferIndex);

        if (currentBuffer.size == 0)
            return;

        switch (format) {
            case skyline::audio::AudioFormat::Int16:
                samples.resize(currentBuffer.size / sizeof(i16));
                state.process->ReadMemory(samples.data(), currentBuffer.address, currentBuffer.size);
                break;
            default:
                throw exception("Unsupported PCM format used by Voice: {}", format);
        }

        if (sampleRate != constant::SampleRate)
            samples = resampler.ResampleBuffer(samples, static_cast<double>(sampleRate) / constant::SampleRate, channelCount);

        if (channelCount == 1 && constant::ChannelCount != channelCount) {
            auto originalSize = samples.size();
            samples.resize((originalSize / channelCount) * constant::ChannelCount);

            for (auto monoIndex = originalSize - 1, targetIndex = samples.size(); monoIndex > 0; monoIndex--) {
                auto sample = samples[monoIndex];
                for (auto i = 0; i < constant::ChannelCount; i++)
                    samples[--targetIndex] = sample;
            }
        }
    }

    std::vector<i16> &Voice::GetBufferData(u32 maxSamples, u32 &outOffset, u32 &outSize) {
        WaveBuffer &currentBuffer = waveBuffers.at(bufferIndex);

        if (!acquired || playbackState != skyline::audio::AudioOutState::Started) {
            outSize = 0;
            return samples;
        }

        if (bufferReload) {
            bufferReload = false;
            UpdateBuffers();
        }

        outOffset = sampleOffset;
        outSize = std::min(maxSamples * constant::ChannelCount, static_cast<u32>(samples.size() - sampleOffset));

        output.playedSamplesCount += outSize / constant::ChannelCount;
        sampleOffset += outSize;

        if (sampleOffset == samples.size()) {
            sampleOffset = 0;

            if (currentBuffer.lastBuffer)
                playbackState = skyline::audio::AudioOutState::Paused;

            if (!currentBuffer.looping)
                SetWaveBufferIndex(static_cast<u8>(bufferIndex + 1));

            output.playedWaveBuffersCount++;
        }

        return samples;
    }
}
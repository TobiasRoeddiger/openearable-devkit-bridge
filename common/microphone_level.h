/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>

// Adapter-only microphone conditioning. The gain is calibrated digitally,
// not in acoustic SPL. A short look-ahead block prevents hard clipping.
class MicrophoneLevel {
    float previousInput = 0, highpass = 0, gain = 0;
public:
    void reset() { previousInput = highpass = gain = 0; }
    static float gainForVolume(unsigned volume) {
        if (!volume) return 0;
        if (volume > 15) volume = 15;
        // Nominal volume 8: +12 dB. Maximum: +24 dB.
        return std::pow(10.0f, (12.0f + (static_cast<int>(volume) - 8) * 12.0f / 7) / 20.0f);
    }
    bool process(int16_t *samples, size_t count, unsigned rate, float requestedGain) {
        if (!count) return false;
        float filtered[160];
        if (count > 160 || (rate != 8000 && rate != 16000)) return false;
        const float pole = std::exp(-2.0f * 3.14159265359f * 80.0f / rate);
        float peak = 0;
        for (size_t i = 0; i < count; i++) {
            float x = samples[i];
            highpass = pole * (highpass + x - previousInput);
            previousInput = x;
            filtered[i] = highpass;
            peak = std::fmax(peak, std::fabs(highpass));
        }
        float allowed = peak > 0 ? std::fmin(requestedGain, 30000.0f / peak) : requestedGain;
        // Instant attenuation, 150 ms recovery. One block of look-ahead keeps
        // every sample within headroom even on the first loud transient.
        float release = 1.0f - std::exp(-static_cast<float>(count) / (rate * .15f));
        gain = allowed < gain ? allowed : gain + (allowed - gain) * release;
        for (size_t i = 0; i < count; i++)
            samples[i] = static_cast<int16_t>(std::lround(filtered[i] * gain));
        return allowed < requestedGain;
    }
};

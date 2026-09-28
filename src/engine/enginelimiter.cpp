#include "engine/enginelimiter.h"

#include <algorithm>
#include <cmath>

namespace {
constexpr int kChannels = 2;
} // namespace

EngineLimiter::EngineLimiter()
        : m_gain(CSAMPLE_GAIN_ONE),
          m_minGainOfLastBuffer(CSAMPLE_GAIN_ONE),
          m_releaseCoefficient(0) {
}

void EngineLimiter::reset() {
    m_gain = CSAMPLE_GAIN_ONE;
    m_minGainOfLastBuffer = CSAMPLE_GAIN_ONE;
}

void EngineLimiter::process(CSAMPLE* pBuffer,
        int numSamples,
        mixxx::audio::SampleRate sampleRate) {
    if (sampleRate != m_releaseSampleRate && sampleRate.isValid()) {
        // One pole smoothing that covers ~63% of the way back in kReleaseSeconds
        m_releaseCoefficient = static_cast<CSAMPLE_GAIN>(
                1.0 - std::exp(-1.0 / (kReleaseSeconds * sampleRate.value())));
        m_releaseSampleRate = sampleRate;
    }

    CSAMPLE_GAIN gain = m_gain;
    CSAMPLE_GAIN minGain = CSAMPLE_GAIN_ONE;
    for (int i = 0; i + 1 < numSamples; i += kChannels) {
        const CSAMPLE peak = std::max(std::abs(pBuffer[i]), std::abs(pBuffer[i + 1]));
        const CSAMPLE_GAIN targetGain = peak > kCeiling ? kCeiling / peak : CSAMPLE_GAIN_ONE;
        if (targetGain < gain) {
            // Instant attack: never let a sample pass above the ceiling
            gain = targetGain;
        } else {
            gain += (targetGain - gain) * m_releaseCoefficient;
        }
        if (gain < CSAMPLE_GAIN_ONE) {
            pBuffer[i] *= gain;
            pBuffer[i + 1] *= gain;
            minGain = std::min(minGain, gain);
        }
    }
    // Avoid denormals and snap back to unity when the release is complete
    if (gain > 0.9999f) {
        gain = CSAMPLE_GAIN_ONE;
    }
    m_gain = gain;
    m_minGainOfLastBuffer = minGain;
}

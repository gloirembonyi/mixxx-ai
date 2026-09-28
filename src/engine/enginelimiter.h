#pragma once

#include "audio/types.h"
#include "util/types.h"

/// Zero latency peak limiter that protects the main output against digital
/// clipping. It reacts instantly when a stereo frame would exceed the ceiling
/// and releases the gain reduction smoothly afterwards.
///
/// This is a safety net and not meant as a creative effect, so it has no
/// parameters apart from being enabled or not.
class EngineLimiter {
  public:
    /// The ceiling is slightly below 0 dBFS to leave headroom for the
    /// inter-sample peaks created by the sound card's DAC and lossy encoders.
    static constexpr CSAMPLE kCeiling = 0.966f; // -0.3 dBFS
    static constexpr double kReleaseSeconds = 0.15;

    EngineLimiter();

    /// Processes an interleaved stereo buffer in place
    void process(CSAMPLE* pBuffer, int numSamples, mixxx::audio::SampleRate sampleRate);

    /// Resets the gain reduction, e.g. when the limiter is enabled again
    void reset();

    /// The lowest gain that was applied in the last processed buffer (1 = none)
    CSAMPLE_GAIN minGainOfLastBuffer() const {
        return m_minGainOfLastBuffer;
    }

  private:
    CSAMPLE_GAIN m_gain;
    CSAMPLE_GAIN m_minGainOfLastBuffer;
    mixxx::audio::SampleRate m_releaseSampleRate;
    CSAMPLE_GAIN m_releaseCoefficient;
};

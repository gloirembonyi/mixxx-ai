// Tests for the performance features: beat/bar/phrase counter, Pitch Play,
// slow tempo reset and the main output limiter.

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include "control/controlproxy.h"
#include "engine/controls/pitchplaycontrol.h"
#include "engine/enginelimiter.h"
#include "test/signalpathtest.h"
#include "widget/wphasemeter.h"

namespace {

// The fake track is 10 s long, 240 BPM gives 40 beats: more than one phrase
constexpr double kBpm = 240.0;
constexpr double kSampleRate = 44100.0;
constexpr double kBeatLengthFrames = 60.0 / kBpm * kSampleRate;

} // namespace

TEST(EngineLimiterTest, QuietSignalIsUntouched) {
    EngineLimiter limiter;
    std::vector<CSAMPLE> buffer(1024, 0.5f);
    limiter.process(buffer.data(), static_cast<int>(buffer.size()),
            mixxx::audio::SampleRate(44100));
    for (const CSAMPLE sample : buffer) {
        EXPECT_FLOAT_EQ(0.5f, sample);
    }
    EXPECT_FLOAT_EQ(CSAMPLE_GAIN_ONE, limiter.minGainOfLastBuffer());
}

TEST(EngineLimiterTest, LoudSignalNeverExceedsCeiling) {
    EngineLimiter limiter;
    std::vector<CSAMPLE> buffer(2048);
    for (std::size_t i = 0; i < buffer.size(); ++i) {
        // Alternating +2 / -1.5 peaks, way above 0 dBFS
        buffer[i] = (i % 4 < 2) ? 2.0f : -1.5f;
    }
    limiter.process(buffer.data(), static_cast<int>(buffer.size()),
            mixxx::audio::SampleRate(44100));
    for (const CSAMPLE sample : buffer) {
        EXPECT_LE(std::abs(sample), EngineLimiter::kCeiling + 1e-6f);
    }
    EXPECT_LT(limiter.minGainOfLastBuffer(), 0.5f);
}

TEST(EngineLimiterTest, GainRecoversAfterPeak) {
    EngineLimiter limiter;
    const auto sampleRate = mixxx::audio::SampleRate(44100);
    std::vector<CSAMPLE> peak(64, 2.0f);
    limiter.process(peak.data(), static_cast<int>(peak.size()), sampleRate);

    // Two seconds (stereo) of quiet signal are more than 13 release time constants
    std::vector<CSAMPLE> quiet(44100 * 2 * 2, 0.25f);
    limiter.process(quiet.data(), static_cast<int>(quiet.size()), sampleRate);
    EXPECT_NEAR(0.25f, quiet.back(), 0.0001f);
}

TEST(PhaseMeterTest, PhaseOffsetWrapsAround) {
    EXPECT_DOUBLE_EQ(0.0, WPhaseMeter::phaseOffset(0.25, 0.25));
    // Deck ahead of the reference
    EXPECT_DOUBLE_EQ(0.1, WPhaseMeter::phaseOffset(0.35, 0.25));
    // Deck behind the reference
    EXPECT_DOUBLE_EQ(-0.1, WPhaseMeter::phaseOffset(0.15, 0.25));
    // 0.9 beats ahead is 0.1 beats behind
    EXPECT_NEAR(-0.1, WPhaseMeter::phaseOffset(0.95, 0.05), 1e-12);
    EXPECT_NEAR(0.1, WPhaseMeter::phaseOffset(0.05, 0.95), 1e-12);
}

TEST(PitchPlayOffsetTest, Ranges) {
    // Low: -7 .. 0
    EXPECT_EQ(-7, PitchPlayControl::padOffset(0, 0));
    EXPECT_EQ(0, PitchPlayControl::padOffset(0, 7));
    // Mid: -3 .. +4
    EXPECT_EQ(-3, PitchPlayControl::padOffset(1, 0));
    EXPECT_EQ(0, PitchPlayControl::padOffset(1, 3));
    EXPECT_EQ(4, PitchPlayControl::padOffset(1, 7));
    // Up: 0 .. +7
    EXPECT_EQ(0, PitchPlayControl::padOffset(2, 0));
    EXPECT_EQ(7, PitchPlayControl::padOffset(2, 7));
}

class ProFeaturesTest : public BaseSignalPathTest {
  protected:
    void SetUp() override {
        BaseSignalPathTest::SetUp();
        m_pMixerDeck1->loadFakeTrack(false, kBpm);
        ProcessBuffer();
    }

    void seekToFrame(double frame) {
        m_pChannel1->getEngineBuffer()->queueNewPlaypos(
                mixxx::audio::FramePos(frame), EngineBuffer::SEEK_STANDARD);
        ProcessBuffer();
    }

    double get(const QString& item) const {
        return ControlObject::get(ConfigKey(m_sGroup1, item));
    }

    void set(const QString& item, double value) {
        ControlObject::set(ConfigKey(m_sGroup1, item), value);
    }

    void press(const QString& item) {
        set(item, 1.0);
        set(item, 0.0);
    }
};

TEST_F(ProFeaturesTest, BeatCounterCountsBeatsBarsAndPhrases) {
    // The fake track is too short for a whole phrase, use a real 30 s file
    const auto pTrack = Track::newTemporary(mixxx::FileAccess(mixxx::FileInfo(
            getTestDir().filePath(QStringLiteral("sine-30.wav")))));
    // The beatgrid needs the sample rate
    pTrack->setAudioProperties(mixxx::audio::ChannelCount(2),
            mixxx::audio::SampleRate(44100),
            mixxx::audio::Bitrate(),
            mixxx::Duration::fromSeconds(30));
    pTrack->trySetBpm(kBpm);
    loadTrack(m_pMixerDeck1.get(), pTrack);
    ProcessBuffer();

    // In the middle of the 6th beat (index 5): bar 2, beat 2
    seekToFrame(5.5 * kBeatLengthFrames);
    EXPECT_EQ(6, get(QStringLiteral("beat_number")));
    EXPECT_EQ(2, get(QStringLiteral("beat_in_bar")));
    EXPECT_EQ(2, get(QStringLiteral("bar_number")));
    EXPECT_EQ(2, get(QStringLiteral("bar_in_phrase")));
    EXPECT_EQ(1, get(QStringLiteral("phrase_number")));
    EXPECT_EQ(27, get(QStringLiteral("beats_until_phrase")));
    EXPECT_EQ(0, get(QStringLiteral("phrase_last_bar")));

    // Beat 30 of 32 is in the last bar of the phrase
    seekToFrame(29.5 * kBeatLengthFrames);
    EXPECT_EQ(3, get(QStringLiteral("beats_until_phrase")));
    EXPECT_EQ(1, get(QStringLiteral("phrase_last_bar")));

    // First beat of the second 32 beat phrase
    seekToFrame(32.25 * kBeatLengthFrames);
    EXPECT_EQ(33, get(QStringLiteral("beat_number")));
    EXPECT_EQ(1, get(QStringLiteral("beat_in_bar")));
    EXPECT_EQ(9, get(QStringLiteral("bar_number")));
    EXPECT_EQ(1, get(QStringLiteral("bar_in_phrase")));
    EXPECT_EQ(2, get(QStringLiteral("phrase_number")));
    EXPECT_EQ(32, get(QStringLiteral("beats_until_phrase")));
}

TEST_F(ProFeaturesTest, PitchPlayTransposesAndPlaysHotcue) {
    seekToFrame(4 * kBeatLengthFrames);
    // Pressing an empty hotcue pad creates the cue and focuses it
    press(QStringLiteral("hotcue_1_activate"));
    ASSERT_EQ(1, get(QStringLiteral("hotcue_focus")));
    seekToFrame(10 * kBeatLengthFrames);

    // Mid range (default): pad 5 is +1 semitone
    press(QStringLiteral("pitchplay_5_activate"));
    ProcessBuffer();
    EXPECT_DOUBLE_EQ(1.0, get(QStringLiteral("pitch")));
    EXPECT_DOUBLE_EQ(1.0, get(QStringLiteral("pitchplay_offset")));
    EXPECT_EQ(1.0, get(QStringLiteral("pitchplay_5_enabled")));
    EXPECT_EQ(0.0, get(QStringLiteral("pitchplay_4_enabled")));
    EXPECT_EQ(1.0, get(QStringLiteral("play")));

    // Pad 1 in mid range is -3 semitones, relative to the original pitch
    press(QStringLiteral("pitchplay_1_activate"));
    EXPECT_DOUBLE_EQ(-3.0, get(QStringLiteral("pitch")));
    EXPECT_EQ(0.0, get(QStringLiteral("pitchplay_5_enabled")));
    EXPECT_EQ(1.0, get(QStringLiteral("pitchplay_1_enabled")));

    press(QStringLiteral("pitchplay_reset"));
    EXPECT_DOUBLE_EQ(0.0, get(QStringLiteral("pitch")));
    EXPECT_EQ(0.0, get(QStringLiteral("pitchplay_1_enabled")));
}

TEST_F(ProFeaturesTest, PitchPlayKeepsUserPitchAsBase) {
    set(QStringLiteral("pitch"), 2.0);
    // Up range: pad 3 is +2 semitones
    set(QStringLiteral("pitchplay_range"), 2.0);
    press(QStringLiteral("pitchplay_3_activate"));
    EXPECT_DOUBLE_EQ(4.0, get(QStringLiteral("pitch")));
    press(QStringLiteral("pitchplay_reset"));
    EXPECT_DOUBLE_EQ(2.0, get(QStringLiteral("pitch")));
}

TEST_F(ProFeaturesTest, SlowRateResetGlidesBackToZero) {
    set(QStringLiteral("rate"), 0.5);
    ProcessBuffer();
    press(QStringLiteral("rate_reset_slow"));
    ProcessBuffer();
    EXPECT_EQ(1.0, get(QStringLiteral("rate_reset_slow_active")));
    const double rateAfterOneBuffer = get(QStringLiteral("rate"));
    EXPECT_LT(rateAfterOneBuffer, 0.5);
    EXPECT_GT(rateAfterOneBuffer, 0.0);

    // Process a bit more than the glide duration
    const double bufferSeconds = kProcessBufferSize / (2.0 * kSampleRate);
    const int buffers = static_cast<int>(5.0 / bufferSeconds);
    for (int i = 0; i < buffers; ++i) {
        ProcessBuffer();
    }
    EXPECT_DOUBLE_EQ(0.0, get(QStringLiteral("rate")));
    EXPECT_EQ(0.0, get(QStringLiteral("rate_reset_slow_active")));
}

TEST_F(ProFeaturesTest, SlowRateResetIsCancelledByUser) {
    set(QStringLiteral("rate"), 0.5);
    press(QStringLiteral("rate_reset_slow"));
    ProcessBuffer();
    set(QStringLiteral("rate"), 0.3);
    ProcessBuffer();
    ProcessBuffer();
    EXPECT_DOUBLE_EQ(0.3, get(QStringLiteral("rate")));
    EXPECT_EQ(0.0, get(QStringLiteral("rate_reset_slow_active")));
}

TEST_F(ProFeaturesTest, MainLimiterIsEnabledByDefault) {
    EXPECT_EQ(1.0, ControlObject::get(ConfigKey(m_sMainGroup, "limiter_enabled")));
    EXPECT_EQ(0.0, ControlObject::get(ConfigKey(m_sMainGroup, "limiter_reduction_db")));
}

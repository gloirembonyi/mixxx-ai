#include "engine/controls/clockcontrol.h"

#include <limits>

#include "control/controlobject.h"
#include "control/controlproxy.h"
#include "engine/controls/enginecontrol.h"
#include "moc_clockcontrol.cpp"
#include "preferences/usersettings.h"
#include "track/track.h"

namespace {
constexpr double kBlinkInterval = 0.20; // LED is on 20% of the beat period
constexpr double kStandStillTolerance =
        0.005; // (seconds) Minimum change, to he last evaluated position
constexpr double kSignificiantRateThreshold =
        0.1; // If rate is significiant, update indicator also inside standstill tolerance
constexpr int kBeatsPerBar = 4;
constexpr int kBarsPerPhrase = 8;
constexpr int kBeatsPerPhrase = kBeatsPerBar * kBarsPerPhrase;
constexpr int kNoBeatIndex = std::numeric_limits<int>::min();

/// Modulo that is always positive, also for beats before the first downbeat
int positiveModulo(int value, int divisor) {
    const int result = value % divisor;
    return result < 0 ? result + divisor : result;
}

/// Division that rounds towards negative infinity
int floorDivide(int value, int divisor) {
    return (value - positiveModulo(value, divisor)) / divisor;
}

std::unique_ptr<ControlObject> makeReadOnlyCounter(const QString& group, const QString& item) {
    auto pControl = std::make_unique<ControlObject>(ConfigKey(group, item));
    pControl->setReadOnly();
    pControl->forceSet(0.0);
    return pControl;
}
} // namespace

ClockControl::ClockControl(const QString& group, UserSettingsPointer pConfig)
        : EngineControl(group, pConfig),
          m_pCOBeatActive(std::make_unique<ControlObject>(ConfigKey(group, "beat_active"))),
          m_pCOBeatNumber(makeReadOnlyCounter(group, "beat_number")),
          m_pCOBeatInBar(makeReadOnlyCounter(group, "beat_in_bar")),
          m_pCOBarNumber(makeReadOnlyCounter(group, "bar_number")),
          m_pCOBarInPhrase(makeReadOnlyCounter(group, "bar_in_phrase")),
          m_pCOPhraseNumber(makeReadOnlyCounter(group, "phrase_number")),
          m_pCOBeatsUntilPhrase(makeReadOnlyCounter(group, "beats_until_phrase")),
          m_pCOPhraseLastBar(makeReadOnlyCounter(group, "phrase_last_bar")),
          m_lastCountedBeatIndex(kNoBeatIndex),
          m_pCountedBeats(nullptr),
          m_pLoopEnabled(std::make_unique<ControlProxy>(group, "loop_enabled", this)),
          m_pLoopStartPosition(std::make_unique<ControlProxy>(group, "loop_start_position", this)),
          m_pLoopEndPosition(std::make_unique<ControlProxy>(group, "loop_end_position", this)),
          m_lastPlayDirectionWasForwards(true),
          m_lastEvaluatedPosition(mixxx::audio::kStartFramePos),
          m_prevBeatPosition(mixxx::audio::kStartFramePos),
          m_nextBeatPosition(mixxx::audio::kStartFramePos),
          m_blinkIntervalFrames(0.0),
          m_internalState(StateMachine::outsideIndicationArea) {
    m_pCOBeatActive->setReadOnly();
    m_pCOBeatActive->forceSet(0.0);
}

ClockControl::~ClockControl() = default;

// called from an engine worker thread
void ClockControl::trackLoaded(TrackPointer pNewTrack) {
    mixxx::BeatsPointer pBeats;
    if (pNewTrack) {
        pBeats = pNewTrack->getBeats();
    }
    trackBeatsUpdated(pBeats);
}

void ClockControl::trackBeatsUpdated(mixxx::BeatsPointer pBeats) {
    // Clear on-beat control
    m_pCOBeatActive->forceSet(0.0);
    m_pBeats = pBeats;
}

void ClockControl::clearBeatCounter() {
    m_lastCountedBeatIndex = kNoBeatIndex;
    m_pCOBeatNumber->forceSet(0.0);
    m_pCOBeatInBar->forceSet(0.0);
    m_pCOBarNumber->forceSet(0.0);
    m_pCOBarInPhrase->forceSet(0.0);
    m_pCOPhraseNumber->forceSet(0.0);
    m_pCOBeatsUntilPhrase->forceSet(0.0);
    m_pCOPhraseLastBar->forceSet(0.0);
}

void ClockControl::updateBeatCounter(int beatIndex) {
    if (beatIndex == m_lastCountedBeatIndex) {
        return;
    }
    m_lastCountedBeatIndex = beatIndex;
    // Beat, bar and phrase numbers are 1-based like on a DJ's count
    // "1-2-3-4". Beats before the first downbeat get numbers <= 0.
    const int beatInPhrase = positiveModulo(beatIndex, kBeatsPerPhrase);
    const int barIndex = floorDivide(beatIndex, kBeatsPerBar);
    m_pCOBeatNumber->forceSet(beatIndex + 1);
    m_pCOBeatInBar->forceSet(positiveModulo(beatIndex, kBeatsPerBar) + 1);
    m_pCOBarNumber->forceSet(barIndex + 1);
    m_pCOBarInPhrase->forceSet(positiveModulo(barIndex, kBarsPerPhrase) + 1);
    m_pCOPhraseNumber->forceSet(floorDivide(beatIndex, kBeatsPerPhrase) + 1);
    m_pCOBeatsUntilPhrase->forceSet(kBeatsPerPhrase - beatInPhrase);
    m_pCOPhraseLastBar->forceSet(
            beatInPhrase >= kBeatsPerPhrase - kBeatsPerBar ? 1.0 : 0.0);
}

void ClockControl::updateIndicators(const double dRate,
        mixxx::audio::FramePos currentPosition,
        mixxx::audio::SampleRate sampleRate) {
    /* This method sets the control beat_active is set to the following values:
    *  0.0 --> No beat indication (outside 20% area or play direction changed while indication was on)
    *  1.0 --> Forward playing, set at the beat and set back to 0.0 at 20% of beat distance
    *  2.0 --> Reverse playing, set at the beat and set back to 0.0 at -20% of beat distance
    */

    // No position change since last indicator update (e.g. deck stopped) -> No indicator update needed
    // The kSignificiantRateThreshold condition ensures an immediate indicator update, when the play/cue button is pressed
    if ((currentPosition <= (m_lastEvaluatedPosition + kStandStillTolerance * sampleRate)) &&
            (currentPosition >= (m_lastEvaluatedPosition - kStandStillTolerance * sampleRate)) &&
            (fabs(dRate) <= kSignificiantRateThreshold)) {
        return;
    }

    // Position change more significiantly, but rate is zero. Occurs when pressing a cue point
    // The m_internalState needs to be taken into account here to prevent unnecessary events (state 0 -> state 0)
    if ((dRate == 0.0) && (m_internalState != StateMachine::outsideIndicationArea)) {
        m_internalState = StateMachine::outsideIndicationArea;
        m_pCOBeatActive->forceSet(0.0);
    }

    mixxx::audio::FramePos prevIndicatorPosition;
    mixxx::audio::FramePos nextIndicatorPosition;

    const mixxx::BeatsPointer pBeats = m_pBeats;
    // A new track or an edited beatgrid invalidates the cached beats
    const bool beatsChanged = pBeats.get() != m_pCountedBeats;
    m_pCountedBeats = pBeats.get();
    if (pBeats) {
        if (beatsChanged || !m_prevBeatPosition.isValid() || !m_nextBeatPosition.isValid() ||
                currentPosition >= m_nextBeatPosition ||
                currentPosition <= m_prevBeatPosition) {
            pBeats->findPrevNextBeats(currentPosition,
                    &m_prevBeatPosition,
                    &m_nextBeatPosition,
                    false); // Precise compare without tolerance needed
            if (m_prevBeatPosition.isValid()) {
                // Only evaluated when a beat is crossed, not on every callback
                updateBeatCounter(pBeats->iteratorFrom(m_prevBeatPosition) -
                        pBeats->cfirstmarker());
            } else {
                clearBeatCounter();
            }
        }
    } else {
        m_prevBeatPosition = mixxx::audio::kInvalidFramePos;
        m_nextBeatPosition = mixxx::audio::kInvalidFramePos;
        if (m_lastCountedBeatIndex != kNoBeatIndex) {
            clearBeatCounter();
        }
    }

    // Loops need special handling
    if (m_pLoopEnabled->toBool()) {
        const auto loopStartPosition =
                mixxx::audio::FramePos::fromEngineSamplePosMaybeInvalid(
                        m_pLoopStartPosition->get());
        const auto loopEndPosition =
                mixxx::audio::FramePos::fromEngineSamplePosMaybeInvalid(
                        m_pLoopEndPosition->get());

        if (m_prevBeatPosition.isValid() && m_nextBeatPosition.isValid() &&
                loopStartPosition.isValid() && loopEndPosition.isValid() &&
                m_prevBeatPosition < loopStartPosition &&
                m_nextBeatPosition >= loopEndPosition) {
            // No beat inside loop -> show beat indication at loop_start_position
            prevIndicatorPosition = loopStartPosition;
            nextIndicatorPosition = loopEndPosition;
        } else {
            prevIndicatorPosition = m_prevBeatPosition;
            nextIndicatorPosition = m_nextBeatPosition;
        }

        if (m_prevBeatPosition.isValid() && m_nextBeatPosition.isValid()) {
            // Don't overwrite interval at begin/end of track
            if ((loopEndPosition - loopStartPosition) < (m_nextBeatPosition - m_prevBeatPosition)) {
                // Loops smaller than beat distance -> Set m_blinkIntervalFrames based on loop period
                m_blinkIntervalFrames = (loopEndPosition - loopStartPosition) * kBlinkInterval;
            } else {
                m_blinkIntervalFrames =
                        (nextIndicatorPosition - prevIndicatorPosition) *
                        kBlinkInterval;
            }
        }
    } else {
        prevIndicatorPosition = m_prevBeatPosition;
        nextIndicatorPosition = m_nextBeatPosition;

        if (prevIndicatorPosition.isValid() && nextIndicatorPosition.isValid()) {
            // Don't overwrite interval at begin/end of track
            m_blinkIntervalFrames =
                    (nextIndicatorPosition - prevIndicatorPosition) *
                    kBlinkInterval;
        }
    }

    // The m_internalState needs to be taken into account, to show a reliable beat indication for loops
    if (dRate > 0.0) {
        if (m_lastPlayDirectionWasForwards) {
            if (prevIndicatorPosition.isValid() &&
                    currentPosition > prevIndicatorPosition &&
                    currentPosition <
                            (prevIndicatorPosition + m_blinkIntervalFrames) &&
                    (m_internalState != StateMachine::afterBeatActive) &&
                    (m_internalState !=
                            StateMachine::afterBeatDirectionChanged)) {
                m_internalState = StateMachine::afterBeatActive;
                m_pCOBeatActive->forceSet(1.0);
            } else if (prevIndicatorPosition.isValid() &&
                    currentPosition >
                            (prevIndicatorPosition + m_blinkIntervalFrames) &&
                    (m_internalState == StateMachine::afterBeatActive ||
                            m_internalState ==
                                    StateMachine::afterBeatDirectionChanged)) {
                m_internalState = StateMachine::outsideIndicationArea;
                m_pCOBeatActive->forceSet(0.0);
            }
        } else {
            // Play direction changed while beat indicator was on and forward playing
            if (nextIndicatorPosition.isValid() &&
                    currentPosition < nextIndicatorPosition &&
                    currentPosition >=
                            nextIndicatorPosition - m_blinkIntervalFrames &&
                    m_internalState !=
                            StateMachine::beforeBeatDirectionChanged) {
                m_internalState = StateMachine::beforeBeatDirectionChanged;
                m_pCOBeatActive->forceSet(0.0);
            }
        }
        m_lastPlayDirectionWasForwards = true;
    } else if (dRate < 0.0) {
        if (!m_lastPlayDirectionWasForwards) {
            if (nextIndicatorPosition.isValid() &&
                    currentPosition < nextIndicatorPosition &&
                    currentPosition >
                            (nextIndicatorPosition - m_blinkIntervalFrames) &&
                    m_internalState != StateMachine::beforeBeatActive &&
                    m_internalState !=
                            StateMachine::beforeBeatDirectionChanged) {
                m_internalState = StateMachine::beforeBeatActive;
                m_pCOBeatActive->forceSet(2.0);
            } else if (nextIndicatorPosition.isValid() &&
                    currentPosition <
                            (nextIndicatorPosition - m_blinkIntervalFrames) &&
                    (m_internalState == StateMachine::beforeBeatActive ||
                            m_internalState ==
                                    StateMachine::beforeBeatDirectionChanged)) {
                m_internalState = StateMachine::outsideIndicationArea;
                m_pCOBeatActive->forceSet(0.0);
            }
        } else {
            // Play direction changed while beat indicator was on and reverse playing
            if (prevIndicatorPosition.isValid() &&
                    currentPosition > prevIndicatorPosition &&
                    currentPosition <=
                            (prevIndicatorPosition + m_blinkIntervalFrames) &&
                    m_internalState !=
                            StateMachine::afterBeatDirectionChanged) {
                m_internalState = StateMachine::afterBeatDirectionChanged;
                m_pCOBeatActive->forceSet(0.0);
            }
        }
        m_lastPlayDirectionWasForwards = false;
    }
    m_lastEvaluatedPosition = currentPosition;
}

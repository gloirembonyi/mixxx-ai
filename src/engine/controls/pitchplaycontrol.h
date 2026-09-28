#pragma once

#include <QObject>
#include <memory>
#include <vector>

#include "control/pollingcontrolproxy.h"
#include "engine/controls/enginecontrol.h"
#include "preferences/usersettings.h"

class ControlObject;
class ControlPushButton;

/// Pitch Play: play the focused hotcue (or the main cue) transposed by a
/// number of semitones, one pad per semitone, like Serato's Pitch Play.
///
/// Controls per deck:
///  - pitchplay_N_activate (N = 1..8): jump to the cue and play it with the
///    pitch offset of pad N
///  - pitchplay_N_enabled: 1 if pad N is the currently played offset (for LEDs)
///  - pitchplay_range: 0 = low (-7..0), 1 = mid (-3..+4), 2 = up (0..+7)
///  - pitchplay_reset: return to the pitch before Pitch Play was used
///  - pitchplay_offset: currently applied offset in semitones (read-only)
class PitchPlayControl : public EngineControl {
    Q_OBJECT
  public:
    static constexpr int kNumPads = 8;

    PitchPlayControl(const QString& group, UserSettingsPointer pConfig);
    ~PitchPlayControl() override;

    /// Semitone offset of a pad (0-based index) for a given range setting
    static int padOffset(int rangeSetting, int padIndex);

  private slots:
    void slotReset(double value);
    void slotRangeChanged(double value);

  private:
    void padActivate(int padIndex, double value);
    void jumpToCueAndPlay();
    void updatePadIndicators();

    std::vector<std::unique_ptr<ControlPushButton>> m_padActivate;
    std::vector<std::unique_ptr<ControlObject>> m_padEnabled;
    std::unique_ptr<ControlPushButton> m_pRange;
    std::unique_ptr<ControlPushButton> m_pReset;
    std::unique_ptr<ControlObject> m_pOffset;

    PollingControlProxy m_pitch;
    PollingControlProxy m_hotcueFocus;

    // The pitch before the first pad was pressed
    double m_basePitch;
    bool m_active;
    int m_activeOffset;
};

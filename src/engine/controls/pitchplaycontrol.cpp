#include "engine/controls/pitchplaycontrol.h"

#include <cmath>

#include "control/controlobject.h"
#include "control/controlpushbutton.h"
#include "moc_pitchplaycontrol.cpp"

namespace {

enum class PitchPlayRange : int {
    Low = 0,
    Mid = 1,
    Up = 2,
};
constexpr int kNumRanges = 3;

// Tolerance when checking if someone else changed the pitch meanwhile
constexpr double kPitchTolerance = 0.001;

ConfigKey padKey(const QString& group, int padIndex, const QString& suffix) {
    return ConfigKey(group,
            QStringLiteral("pitchplay_") + QString::number(padIndex + 1) +
                    QChar('_') + suffix);
}

} // namespace

PitchPlayControl::PitchPlayControl(const QString& group, UserSettingsPointer pConfig)
        : EngineControl(group, pConfig),
          m_pRange(std::make_unique<ControlPushButton>(
                  ConfigKey(group, QStringLiteral("pitchplay_range")))),
          m_pReset(std::make_unique<ControlPushButton>(
                  ConfigKey(group, QStringLiteral("pitchplay_reset")))),
          m_pOffset(std::make_unique<ControlObject>(
                  ConfigKey(group, QStringLiteral("pitchplay_offset")))),
          m_pitch(group, QStringLiteral("pitch")),
          m_hotcueFocus(group, QStringLiteral("hotcue_focus")),
          m_basePitch(0.0),
          m_active(false),
          m_activeOffset(0) {
    m_pRange->setButtonMode(ControlPushButton::TOGGLE);
    m_pRange->setStates(kNumRanges);
    m_pRange->set(static_cast<double>(PitchPlayRange::Mid));
    connect(m_pRange.get(),
            &ControlObject::valueChanged,
            this,
            &PitchPlayControl::slotRangeChanged,
            Qt::DirectConnection);

    connect(m_pReset.get(),
            &ControlObject::valueChanged,
            this,
            &PitchPlayControl::slotReset,
            Qt::DirectConnection);

    m_pOffset->setReadOnly();

    m_padActivate.reserve(kNumPads);
    m_padEnabled.reserve(kNumPads);
    for (int i = 0; i < kNumPads; ++i) {
        auto pActivate = std::make_unique<ControlPushButton>(
                padKey(group, i, QStringLiteral("activate")));
        connect(pActivate.get(),
                &ControlObject::valueChanged,
                this,
                [this, i](double value) { padActivate(i, value); },
                Qt::DirectConnection);
        m_padActivate.push_back(std::move(pActivate));

        auto pEnabled = std::make_unique<ControlObject>(
                padKey(group, i, QStringLiteral("enabled")));
        pEnabled->setReadOnly();
        m_padEnabled.push_back(std::move(pEnabled));
    }
}

PitchPlayControl::~PitchPlayControl() = default;

// static
int PitchPlayControl::padOffset(int rangeSetting, int padIndex) {
    switch (static_cast<PitchPlayRange>(rangeSetting)) {
    case PitchPlayRange::Low:
        return padIndex - (kNumPads - 1); // -7 .. 0
    case PitchPlayRange::Up:
        return padIndex; // 0 .. +7
    case PitchPlayRange::Mid:
    default:
        return padIndex - 3; // -3 .. +4
    }
}

void PitchPlayControl::padActivate(int padIndex, double value) {
    if (value <= 0.0) {
        return;
    }
    const double currentPitch = m_pitch.get();
    // Remember the pitch before Pitch Play was used, unless it is still
    // active and nobody changed the pitch in the meantime.
    if (!m_active ||
            std::abs(currentPitch - (m_basePitch + m_activeOffset)) > kPitchTolerance) {
        m_basePitch = currentPitch;
    }
    m_active = true;
    m_activeOffset = padOffset(static_cast<int>(m_pRange->get()), padIndex);
    m_pitch.set(m_basePitch + m_activeOffset);
    m_pOffset->forceSet(m_activeOffset);
    updatePadIndicators();
    jumpToCueAndPlay();
}

void PitchPlayControl::jumpToCueAndPlay() {
    const int focusedHotcue = static_cast<int>(m_hotcueFocus.get());
    ConfigKey key;
    if (focusedHotcue > 0 &&
            ControlObject::get(ConfigKey(getGroup(),
                    QStringLiteral("hotcue_%1_status").arg(focusedHotcue))) >
                    0.0) {
        key = ConfigKey(getGroup(),
                QStringLiteral("hotcue_%1_gotoandplay").arg(focusedHotcue));
    } else {
        key = ConfigKey(getGroup(), QStringLiteral("cue_gotoandplay"));
    }
    ControlObject::set(key, 1.0);
    ControlObject::set(key, 0.0);
}

void PitchPlayControl::slotReset(double value) {
    if (value <= 0.0 || !m_active) {
        return;
    }
    m_pitch.set(m_basePitch);
    m_active = false;
    m_activeOffset = 0;
    m_pOffset->forceSet(0.0);
    updatePadIndicators();
}

void PitchPlayControl::slotRangeChanged(double value) {
    Q_UNUSED(value);
    updatePadIndicators();
}

void PitchPlayControl::updatePadIndicators() {
    const int range = static_cast<int>(m_pRange->get());
    for (int i = 0; i < kNumPads; ++i) {
        const bool enabled = m_active && padOffset(range, i) == m_activeOffset;
        m_padEnabled[i]->forceSet(enabled ? 1.0 : 0.0);
    }
}

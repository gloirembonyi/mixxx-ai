#include "widget/wphasemeter.h"

#include <QPainter>
#include <QStyle>
#include <QStyleOption>
#include <cmath>

#include "moc_wphasemeter.cpp"
#include "skin/legacy/skincontext.h"

namespace {

// ~30 fps is smooth enough for a meter and cheap
constexpr int kUpdateIntervalMs = 33;
// Ignore changes that would not move the bar by a visible amount
constexpr double kRepaintThreshold = 0.002;

QString deckGroup(int deckIndex) {
    return QStringLiteral("[Channel%1]").arg(deckIndex + 1);
}

} // namespace

WPhaseMeter::DeckControls::DeckControls(const QString& deckGroup)
        : group(deckGroup),
          play(deckGroup, QStringLiteral("play"), ControlFlag::AllowMissingOrInvalid),
          beatDistance(deckGroup,
                  QStringLiteral("beat_distance"),
                  ControlFlag::AllowMissingOrInvalid),
          syncLeader(deckGroup,
                  QStringLiteral("sync_leader"),
                  ControlFlag::AllowMissingOrInvalid),
          bpm(deckGroup, QStringLiteral("bpm"), ControlFlag::AllowMissingOrInvalid) {
}

WPhaseMeter::WPhaseMeter(QWidget* pParent, const QString& group)
        : WWidget(pParent),
          m_group(group),
          m_numDecks(QStringLiteral("[App]"),
                  QStringLiteral("num_decks"),
                  ControlFlag::AllowMissingOrInvalid),
          m_ownDeckIndex(-1),
          m_inPhaseColor(0x3c, 0xc8, 0x5a),
          m_offPhaseColor(0xf0, 0x8a, 0x24),
          m_centerColor(0x80, 0x80, 0x80) {
    connect(&m_timer, &QTimer::timeout, this, &WPhaseMeter::slotUpdate);
}

void WPhaseMeter::setup(const QDomNode& node, const SkinContext& context) {
    Q_UNUSED(node);
    Q_UNUSED(context);
    updateDeckList();
    m_timer.start(kUpdateIntervalMs);
}

// static
double WPhaseMeter::phaseOffset(double deckBeatDistance, double referenceBeatDistance) {
    double offset = deckBeatDistance - referenceBeatDistance;
    // Wrap to [-0.5, 0.5): being 0.9 beats ahead is being 0.1 beats behind
    offset -= std::floor(offset + 0.5);
    return offset;
}

void WPhaseMeter::updateDeckList() {
    const int numDecks = static_cast<int>(m_numDecks.get());
    if (numDecks == static_cast<int>(m_decks.size())) {
        return;
    }
    m_decks.clear();
    m_ownDeckIndex = -1;
    m_decks.reserve(numDecks);
    for (int i = 0; i < numDecks; ++i) {
        m_decks.emplace_back(deckGroup(i));
        if (m_decks.back().group == m_group) {
            m_ownDeckIndex = i;
        }
    }
}

int WPhaseMeter::findReferenceDeck() const {
    int firstPlaying = -1;
    for (int i = 0; i < static_cast<int>(m_decks.size()); ++i) {
        if (i == m_ownDeckIndex) {
            continue;
        }
        const DeckControls& deck = m_decks[i];
        if (!deck.play.toBool() || deck.bpm.get() <= 0.0) {
            continue;
        }
        if (deck.syncLeader.toBool()) {
            return i;
        }
        if (firstPlaying < 0) {
            firstPlaying = i;
        }
    }
    return firstPlaying;
}

void WPhaseMeter::slotUpdate() {
    if (!isVisible()) {
        return;
    }
    updateDeckList();

    std::optional<double> offset;
    if (m_ownDeckIndex >= 0) {
        const DeckControls& ownDeck = m_decks[m_ownDeckIndex];
        const int referenceIndex = findReferenceDeck();
        if (referenceIndex >= 0 && ownDeck.bpm.get() > 0.0) {
            offset = phaseOffset(ownDeck.beatDistance.get(),
                    m_decks[referenceIndex].beatDistance.get());
        }
    }

    const bool changed = offset.has_value() != m_offset.has_value() ||
            (offset && std::abs(*offset - *m_offset) > kRepaintThreshold);
    if (changed) {
        m_offset = offset;
        update();
    }
}

void WPhaseMeter::paintEvent(QPaintEvent* pEvent) {
    Q_UNUSED(pEvent);
    QStyleOption option;
    option.initFrom(this);
    QPainter painter(this);
    // Draws the background and border from the style sheet
    style()->drawPrimitive(QStyle::PE_Widget, &option, &painter, this);

    const QRectF area = rect();
    const double centerX = area.width() / 2.0;

    if (m_offset) {
        const double offset = *m_offset;
        const bool inPhase = std::abs(offset) < kInPhaseTolerance;
        // An offset of +-0.5 beats fills one half of the meter
        const double barWidth = offset * area.width();
        QRectF bar(centerX, area.top() + 1, barWidth, area.height() - 2);
        painter.fillRect(bar.normalized(), inPhase ? m_inPhaseColor : m_offPhaseColor);
    }

    painter.setPen(m_centerColor);
    painter.drawLine(QPointF(centerX, area.top()), QPointF(centerX, area.bottom()));
}

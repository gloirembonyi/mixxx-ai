#pragma once

#include <QColor>
#include <QTimer>
#include <optional>
#include <vector>

#include "control/pollingcontrolproxy.h"
#include "widget/wwidget.h"

class QDomNode;
class SkinContext;

/// Shows the beat phase offset of a deck relative to a reference deck, like
/// the phase meters of Traktor or Serato. The reference is the sync leader if
/// there is one, otherwise the first other deck that is playing.
///
/// A bar grows from the center to the left when the deck is behind (late) and
/// to the right when it is ahead (early) of the reference deck.
class WPhaseMeter : public WWidget {
    Q_OBJECT
    Q_PROPERTY(QColor inPhaseColor MEMBER m_inPhaseColor DESIGNABLE true)
    Q_PROPERTY(QColor offPhaseColor MEMBER m_offPhaseColor DESIGNABLE true)
    Q_PROPERTY(QColor centerColor MEMBER m_centerColor DESIGNABLE true)
  public:
    WPhaseMeter(QWidget* pParent, const QString& group);

    void setup(const QDomNode& node, const SkinContext& context);

    /// Phase offset in beats in the range [-0.5, 0.5), positive if the deck
    /// is ahead of the reference.
    static double phaseOffset(double deckBeatDistance, double referenceBeatDistance);

    /// Offsets below this are shown in the "in phase" color
    static constexpr double kInPhaseTolerance = 0.02;

  protected:
    void paintEvent(QPaintEvent* pEvent) override;

  private slots:
    void slotUpdate();

  private:
    struct DeckControls {
        explicit DeckControls(const QString& deckGroup);
        QString group;
        PollingControlProxy play;
        PollingControlProxy beatDistance;
        PollingControlProxy syncLeader;
        PollingControlProxy bpm;
    };

    void updateDeckList();
    /// Index into m_decks, or -1
    int findReferenceDeck() const;

    const QString m_group;
    PollingControlProxy m_numDecks;
    std::vector<DeckControls> m_decks;
    // Index of this widget's deck in m_decks, or -1
    int m_ownDeckIndex;
    QTimer m_timer;

    // The displayed offset, std::nullopt if there is nothing to compare with
    std::optional<double> m_offset;

    QColor m_inPhaseColor;
    QColor m_offPhaseColor;
    QColor m_centerColor;
};

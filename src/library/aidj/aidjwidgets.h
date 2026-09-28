#pragma once

#include <QColor>
#include <QStyledItemDelegate>
#include <QWidget>
#include <optional>

#include "proto/keys.pb.h"

class QLabel;
class QPushButton;

namespace mixxx {
namespace aidj {

/// Object name of the AI DJ pages, the style sheet is scoped to it
extern const char* const kPageObjectName;

/// Gives a page (or dialog) of the AI DJ the dark "pro" look. The look does
/// not depend on the skin, so the pages look the same in every skin.
void applyPageStyle(QWidget* pPage);

/// Style roles for labels, frames and buttons, used by the style sheet
enum class Role {
    Title,   // page title
    Section, // small caps section header
    Muted,   // secondary text
    Chip,    // small rounded badge
    Card,    // raised panel (QFrame)
    Primary, // main action button
    Ai,      // button that uses an AI service
    Tip,     // highlighted advice
};
void setRole(QWidget* pWidget, Role role);

QLabel* makeLabel(const QString& text, Role role, QWidget* pParent);

/// Color of a key on the Camelot wheel, similar keys get similar colors
QColor keyColor(track::io::key::ChromaticKey key);

/// Green for good, yellow for ok, orange/red for poor values (0..1)
QColor scoreColor(double value);

/// Item data role of the value (0..1) that ScoreBarDelegate draws as a bar
constexpr int kBarValueRole = Qt::UserRole + 10;

/// Draws a table cell as a small meter: a rounded bar of the value in
/// kBarValueRole behind the cell text. Cells without a value are drawn as
/// usual.
class ScoreBarDelegate : public QStyledItemDelegate {
    Q_OBJECT
  public:
    /// fixedColor: draw all bars in this color instead of scoreColor()
    explicit ScoreBarDelegate(QObject* pParent, QColor fixedColor = QColor());

    void paint(QPainter* pPainter,
            const QStyleOptionViewItem& option,
            const QModelIndex& index) const override;

  private:
    QColor m_fixedColor;
};

/// Shows the energy curve of the event over the set, where the set is now,
/// and the energy of the playing track.
class EnergyCurveWidget : public QWidget {
    Q_OBJECT
  public:
    explicit EnergyCurveWidget(QWidget* pParent = nullptr);

    void setCurve(double startEnergy, double middleEnergy, double endEnergy);
    /// 0 = start, 1 = end, nullopt = the set has not started
    void setProgress(std::optional<double> progress);
    /// Energy of the playing track
    void setCurrentEnergy(std::optional<double> energy);
    /// Fixed energy chosen by the DJ instead of the curve
    void setTargetOverride(std::optional<double> energy);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

  protected:
    void paintEvent(QPaintEvent* pEvent) override;

  private:
    double curveAt(double progress) const;

    double m_start;
    double m_middle;
    double m_end;
    std::optional<double> m_progress;
    std::optional<double> m_currentEnergy;
    std::optional<double> m_targetOverride;
};

} // namespace aidj
} // namespace mixxx

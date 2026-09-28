#include "library/aidj/aidjwidgets.h"

#include <QApplication>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QStyle>
#include <algorithm>

#include "library/aidj/djrecommender.h"
#include "moc_aidjwidgets.cpp"
#include "track/keyutils.h"

namespace mixxx {
namespace aidj {

const char* const kPageObjectName = "AiDjPage";

namespace {

constexpr const char* kRoleProperty = "aidjRole";

// Colors of the look, close to the dark Mixxx skins
const QColor kAccent(0xf0, 0x7a, 0x1a);
const QColor kTrack(0x2a, 0x2a, 0x30);
const QColor kCurveLine(0x5b, 0x9b, 0xd5);
const QColor kTextColor(0xdc, 0xdc, 0xdc);
const QColor kMutedColor(0x8c, 0x8c, 0x94);

// Scoped to the page, it wins over the skin's style sheet
const QString kStyleSheet = QStringLiteral(R"(
#AiDjPage {
  background-color: #151517;
  color: #dcdcdc;
}
#AiDjPage QLabel {
  color: #dcdcdc;
  background: transparent;
}
#AiDjPage QLabel[aidjRole="title"] {
  color: #ffffff;
  font-size: 15px;
  font-weight: bold;
}
#AiDjPage QLabel[aidjRole="section"] {
  color: #f07a1a;
  font-size: 10px;
  font-weight: bold;
}
#AiDjPage QLabel[aidjRole="muted"] {
  color: #8c8c94;
}
#AiDjPage QLabel[aidjRole="chip"] {
  background-color: #26262a;
  border: 1px solid #34343a;
  border-radius: 3px;
  padding: 0px 6px;
  color: #dcdcdc;
}
#AiDjPage QLabel[aidjRole="tip"] {
  background-color: #1f1a14;
  border: 1px solid #5a3a18;
  border-left: 3px solid #f07a1a;
  border-radius: 4px;
  padding: 5px 8px;
  color: #f0e0cc;
}
#AiDjPage QLabel a {
  color: #5b9bd5;
}
#AiDjPage QFrame[aidjRole="card"] {
  background-color: #1d1d20;
  border: 1px solid #2e2e34;
  border-radius: 6px;
}
#AiDjPage QPushButton {
  background-color: #26262a;
  color: #dcdcdc;
  border: 1px solid #3a3a40;
  border-radius: 4px;
  padding: 3px 12px;
  min-height: 20px;
}
#AiDjPage QPushButton:hover {
  background-color: #303036;
  border-color: #56565e;
}
#AiDjPage QPushButton:pressed {
  background-color: #1a1a1d;
}
#AiDjPage QPushButton:disabled {
  color: #56565c;
  background-color: #1c1c1f;
  border-color: #2a2a2e;
}
#AiDjPage QPushButton[aidjRole="primary"] {
  background-color: #c9621a;
  border-color: #f07a1a;
  color: #ffffff;
  font-weight: bold;
}
#AiDjPage QPushButton[aidjRole="primary"]:hover {
  background-color: #f07a1a;
}
#AiDjPage QPushButton[aidjRole="primary"]:disabled {
  background-color: #3a2a1e;
  border-color: #4a3424;
  color: #8a7060;
}
#AiDjPage QPushButton[aidjRole="ai"] {
  border-color: #6c5fc7;
  color: #d4ceff;
}
#AiDjPage QPushButton[aidjRole="ai"]:hover {
  background-color: #2c2840;
}
#AiDjPage QPushButton[aidjRole="ai"]:disabled {
  border-color: #2e2b3a;
  color: #5a5670;
}
#AiDjPage QLineEdit,
#AiDjPage QComboBox,
#AiDjPage QSpinBox {
  background-color: #0f0f11;
  color: #e6e6e6;
  border: 1px solid #3a3a40;
  border-radius: 4px;
  padding: 2px 6px;
  min-height: 20px;
  selection-background-color: #2c454f;
  selection-color: #ffffff;
}
#AiDjPage QLineEdit:focus,
#AiDjPage QComboBox:focus,
#AiDjPage QSpinBox:focus {
  border-color: #f07a1a;
}
#AiDjPage QComboBox QAbstractItemView {
  background-color: #1d1d20;
  color: #dcdcdc;
  border: 1px solid #3a3a40;
  selection-background-color: #2c454f;
  selection-color: #ffffff;
  outline: none;
}
#AiDjPage QCheckBox {
  color: #dcdcdc;
  spacing: 6px;
  background: transparent;
}
#AiDjPage QCheckBox::indicator {
  width: 13px;
  height: 13px;
  border: 1px solid #5a5a62;
  border-radius: 3px;
  background-color: #0f0f11;
}
#AiDjPage QCheckBox::indicator:hover {
  border-color: #f07a1a;
}
#AiDjPage QCheckBox::indicator:checked {
  background-color: #f07a1a;
  border-color: #f07a1a;
}
#AiDjPage QCheckBox:disabled {
  color: #56565c;
}
#AiDjPage QGroupBox {
  background-color: #1d1d20;
  border: 1px solid #2e2e34;
  border-radius: 6px;
  margin-top: 12px;
  padding: 10px 8px 6px 8px;
  color: #dcdcdc;
}
#AiDjPage QGroupBox::title {
  subcontrol-origin: margin;
  left: 10px;
  padding: 0px 4px;
  color: #f07a1a;
  font-weight: bold;
}
#AiDjPage QTableWidget {
  background-color: #111113;
  alternate-background-color: #17171a;
  color: #d6d6d6;
  gridline-color: #222226;
  border: 1px solid #2e2e34;
  border-radius: 4px;
  selection-background-color: #2c454f;
  selection-color: #ffffff;
  outline: none;
}
#AiDjPage QTableWidget::item {
  padding: 0px 6px;
  border: none;
}
#AiDjPage QTableWidget::item:selected {
  background-color: #2c454f;
  color: #ffffff;
}
#AiDjPage QHeaderView {
  background-color: #1d1d20;
  border: none;
}
#AiDjPage QHeaderView::section {
  background-color: #1d1d20;
  color: #9a9aa2;
  border: none;
  border-bottom: 1px solid #34343a;
  border-right: 1px solid #26262a;
  padding: 3px 6px;
  font-weight: bold;
}
#AiDjPage QTableCornerButton::section {
  background-color: #1d1d20;
  border: none;
}
#AiDjPage QProgressBar {
  background-color: #0f0f11;
  border: 1px solid #34343a;
  border-radius: 3px;
  max-height: 6px;
  min-height: 6px;
  text-align: center;
  color: transparent;
}
#AiDjPage QProgressBar::chunk {
  background-color: #f07a1a;
  border-radius: 2px;
}
#AiDjPage QScrollBar:vertical {
  background: #111113;
  width: 10px;
  margin: 0px;
}
#AiDjPage QScrollBar:horizontal {
  background: #111113;
  height: 10px;
  margin: 0px;
}
#AiDjPage QScrollBar::handle {
  background: #3a3a40;
  border-radius: 4px;
  min-height: 24px;
  min-width: 24px;
}
#AiDjPage QScrollBar::handle:hover {
  background: #505058;
}
#AiDjPage QScrollBar::add-line,
#AiDjPage QScrollBar::sub-line {
  width: 0px;
  height: 0px;
}
#AiDjPage QScrollBar::add-page,
#AiDjPage QScrollBar::sub-page {
  background: none;
}
)");

QString roleName(Role role) {
    switch (role) {
    case Role::Title:
        return QStringLiteral("title");
    case Role::Section:
        return QStringLiteral("section");
    case Role::Muted:
        return QStringLiteral("muted");
    case Role::Chip:
        return QStringLiteral("chip");
    case Role::Card:
        return QStringLiteral("card");
    case Role::Primary:
        return QStringLiteral("primary");
    case Role::Ai:
        return QStringLiteral("ai");
    case Role::Tip:
        return QStringLiteral("tip");
    }
    return QString();
}

} // namespace

void applyPageStyle(QWidget* pPage) {
    pPage->setObjectName(QString::fromLatin1(kPageObjectName));
    // Without this the page background is not painted for a plain QWidget
    pPage->setAttribute(Qt::WA_StyledBackground, true);
    pPage->setStyleSheet(kStyleSheet);
}

void setRole(QWidget* pWidget, Role role) {
    pWidget->setProperty(kRoleProperty, roleName(role));
    // Apply the style again if the role changes after the widget was shown
    pWidget->style()->unpolish(pWidget);
    pWidget->style()->polish(pWidget);
}

QLabel* makeLabel(const QString& text, Role role, QWidget* pParent) {
    auto* pLabel = new QLabel(text, pParent);
    setRole(pLabel, role);
    return pLabel;
}

QColor keyColor(track::io::key::ChromaticKey key) {
    if (key == track::io::key::INVALID) {
        return kMutedColor;
    }
    // Neighbors on the wheel get neighboring hues
    const int number = KeyUtils::keyToOpenKeyNumber(key); // 1..12
    const int hue = ((number - 1) * 30) % 360;
    // Minor keys a bit darker than their relative major keys
    return KeyUtils::keyIsMajor(key) ? QColor::fromHsv(hue, 150, 255)
                                     : QColor::fromHsv(hue, 190, 225);
}

QColor scoreColor(double value) {
    if (value >= 0.75) {
        return QColor(0x3c, 0xc8, 0x5a);
    }
    if (value >= 0.55) {
        return QColor(0xb8, 0xc8, 0x30);
    }
    if (value >= 0.4) {
        return QColor(0xe6, 0xa2, 0x29);
    }
    return QColor(0xe0, 0x50, 0x3a);
}

ScoreBarDelegate::ScoreBarDelegate(QObject* pParent, QColor fixedColor)
        : QStyledItemDelegate(pParent),
          m_fixedColor(fixedColor) {
}

void ScoreBarDelegate::paint(QPainter* pPainter,
        const QStyleOptionViewItem& option,
        const QModelIndex& index) const {
    const QVariant valueData = index.data(kBarValueRole);
    if (!valueData.isValid()) {
        QStyledItemDelegate::paint(pPainter, option, index);
        return;
    }
    const double value = std::clamp(valueData.toDouble(), 0.0, 1.0);

    // Background and selection as usual, but without the text
    QStyleOptionViewItem opt(option);
    initStyleOption(&opt, index);
    const QString text = opt.text;
    opt.text.clear();
    const QWidget* pWidget = opt.widget;
    QStyle* pStyle = pWidget ? pWidget->style() : QApplication::style();
    pStyle->drawControl(QStyle::CE_ItemViewItem, &opt, pPainter, pWidget);

    pPainter->save();
    pPainter->setRenderHint(QPainter::Antialiasing, true);
    const QRectF track = QRectF(opt.rect).adjusted(4, 4, -4, -4);
    const QColor color = m_fixedColor.isValid() ? m_fixedColor : scoreColor(value);
    QColor trackColor = kTrack;
    pPainter->setPen(Qt::NoPen);
    pPainter->setBrush(trackColor);
    pPainter->drawRoundedRect(track, 3, 3);
    if (value > 0.0) {
        QRectF bar = track;
        bar.setWidth(std::max(track.height(), track.width() * value));
        QColor barColor = color;
        barColor.setAlpha(opt.state & QStyle::State_Selected ? 200 : 150);
        pPainter->setBrush(barColor);
        pPainter->drawRoundedRect(bar, 3, 3);
    }
    pPainter->setPen(Qt::white);
    QFont font = opt.font;
    font.setBold(true);
    pPainter->setFont(font);
    pPainter->drawText(track, Qt::AlignCenter, text);
    pPainter->restore();
}

EnergyCurveWidget::EnergyCurveWidget(QWidget* pParent)
        : QWidget(pParent),
          m_start(0.5),
          m_middle(0.5),
          m_end(0.5) {
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setToolTip(tr(
            "Energy curve of the event over the set. The marker shows "
            "where the set is now, the dot the energy of the playing "
            "track."));
}

QSize EnergyCurveWidget::sizeHint() const {
    return QSize(260, 54);
}

QSize EnergyCurveWidget::minimumSizeHint() const {
    return QSize(120, 54);
}

void EnergyCurveWidget::setCurve(double startEnergy, double middleEnergy, double endEnergy) {
    m_start = startEnergy;
    m_middle = middleEnergy;
    m_end = endEnergy;
    update();
}

void EnergyCurveWidget::setProgress(std::optional<double> progress) {
    m_progress = progress;
    update();
}

void EnergyCurveWidget::setCurrentEnergy(std::optional<double> energy) {
    m_currentEnergy = energy;
    update();
}

void EnergyCurveWidget::setTargetOverride(std::optional<double> energy) {
    m_targetOverride = energy;
    update();
}

double EnergyCurveWidget::curveAt(double progress) const {
    EventProfile profile;
    profile.startEnergy = m_start;
    profile.middleEnergy = m_middle;
    profile.endEnergy = m_end;
    return targetEnergy(profile, progress);
}

void EnergyCurveWidget::paintEvent(QPaintEvent* pEvent) {
    Q_UNUSED(pEvent);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    QFont labelFont = font();
    labelFont.setPixelSize(9);
    painter.setFont(labelFont);
    const int labelHeight = 11;
    const QRectF plot = QRectF(rect()).adjusted(2, 3, -2, -labelHeight - 2);
    auto pointAt = [&plot](double progress, double energy) {
        return QPointF(plot.left() + plot.width() * progress,
                plot.bottom() - plot.height() * std::clamp(energy, 0.0, 1.0));
    };

    // Grid
    painter.setPen(QPen(QColor(0x2a, 0x2a, 0x30), 1));
    for (double level : {0.25, 0.5, 0.75}) {
        const double y = plot.bottom() - plot.height() * level;
        painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
    }

    // Area under the curve
    QPainterPath line;
    constexpr int kSteps = 40;
    for (int i = 0; i <= kSteps; ++i) {
        const double progress = static_cast<double>(i) / kSteps;
        const QPointF point = pointAt(progress, curveAt(progress));
        if (i == 0) {
            line.moveTo(point);
        } else {
            line.lineTo(point);
        }
    }
    QPainterPath area = line;
    area.lineTo(plot.bottomRight());
    area.lineTo(plot.bottomLeft());
    area.closeSubpath();
    QLinearGradient gradient(plot.topLeft(), plot.bottomLeft());
    QColor top = kCurveLine;
    top.setAlpha(110);
    QColor bottom = kCurveLine;
    bottom.setAlpha(10);
    gradient.setColorAt(0.0, top);
    gradient.setColorAt(1.0, bottom);
    painter.setPen(Qt::NoPen);
    painter.setBrush(gradient);
    painter.drawPath(area);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(kCurveLine, 1.6));
    painter.drawPath(line);

    // Fixed energy chosen by the DJ
    if (m_targetOverride) {
        const double y = pointAt(0.0, *m_targetOverride).y();
        QPen pen(kAccent, 1.2, Qt::DashLine);
        painter.setPen(pen);
        painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
    }

    // Where the set is now
    if (m_progress) {
        const double x = pointAt(*m_progress, 0.0).x();
        painter.setPen(QPen(QColor(0xff, 0xff, 0xff, 140), 1, Qt::DotLine));
        painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
        const double wanted = m_targetOverride.value_or(curveAt(*m_progress));
        painter.setPen(Qt::NoPen);
        painter.setBrush(kCurveLine.lighter(130));
        painter.drawEllipse(pointAt(*m_progress, wanted), 3.0, 3.0);
        if (m_currentEnergy) {
            painter.setBrush(kAccent);
            painter.setPen(QPen(Qt::white, 1));
            painter.drawEllipse(pointAt(*m_progress, *m_currentEnergy), 4.0, 4.0);
        }
    }

    // Labels
    painter.setPen(kMutedColor);
    const QRectF labels(plot.left(), plot.bottom() + 2, plot.width(), labelHeight);
    painter.drawText(labels, Qt::AlignLeft | Qt::AlignVCenter, tr("Start"));
    painter.drawText(labels, Qt::AlignRight | Qt::AlignVCenter, tr("End"));
    painter.setPen(kTextColor);
    painter.drawText(labels, Qt::AlignHCenter | Qt::AlignVCenter, tr("Energy over the set"));
}

} // namespace aidj
} // namespace mixxx

#include "library/aidj/dlgaidj.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>

#include "control/controlobject.h"
#include "library/aidj/aidjwidgets.h"
#include "library/aidj/aiservice.h"
#include "library/dao/playlistdao.h"
#include "library/dao/trackschema.h"
#include "library/library.h"
#include "library/trackcollection.h"
#include "library/trackcollectionmanager.h"
#include "mixer/playerinfo.h"
#include "mixer/playermanager.h"
#include "moc_dlgaidj.cpp"
#include "track/track.h"

namespace mixxx {
namespace aidj {

namespace {

const QString kConfigGroup = QStringLiteral("[AiDj]");
const ConfigKey kEventKey(kConfigGroup, QStringLiteral("Event"));
const ConfigKey kSetMinutesKey(kConfigGroup, QStringLiteral("SetMinutes"));
const ConfigKey kMoodKey(kConfigGroup, QStringLiteral("Mood"));
const ConfigKey kEnergyKey(kConfigGroup, QStringLiteral("Energy"));
const ConfigKey kHarmonicOnlyKey(kConfigGroup, QStringLiteral("HarmonicOnly"));
const ConfigKey kFollowDeckKey(kConfigGroup, QStringLiteral("FollowDeck"));
const ConfigKey kAutoFillKey(kConfigGroup, QStringLiteral("AutoFillAutoDj"));

constexpr int kMaxSuggestions = 30;
// Candidates offered to Gemini for a playlist, best matches first
constexpr int kGeminiCandidates = 300;
// Candidates offered to Jev or Gemini for the next track
constexpr int kPickCandidates = 20;
// Mood tagging per click, keeps within the free daily quota
constexpr int kMaxMoodTagsPerClick = 300;
// Auto DJ is topped up to this many tracks
constexpr int kAutoFillQueueSize = 3;
constexpr int kAutoFillIntervalMs = 10000;
// Artists that count as recently played
constexpr int kRecentArtistCount = 4;

enum Column {
    kColumnScore,
    kColumnArtist,
    kColumnTitle,
    kColumnBpm,
    kColumnKey,
    kColumnEnergy,
    kColumnMoods,
    kColumnReasons,
    kColumnCount,
};

// Energy choices: follow the event curve or a fixed level
const double kEnergyLevels[] = {-1.0, 0.25, 0.5, 0.75, 0.95};
constexpr int kEnergyLevelCount = 5;

const QColor kEnergyBarColor(0x5b, 0x9b, 0xd5);

QTableWidgetItem* readOnlyItem(const QString& text) {
    auto* pItem = new QTableWidgetItem(text);
    pItem->setFlags(pItem->flags() & ~Qt::ItemIsEditable);
    return pItem;
}

QString formatClock(qint64 seconds) {
    return QStringLiteral("%1:%2")
            .arg(seconds / 3600)
            .arg((seconds / 60) % 60, 2, 10, QChar('0'));
}

} // namespace

DlgAiDjSettings::DlgAiDjSettings(QWidget* pParent, AiService* pService)
        : QDialog(pParent),
          m_pService(pService) {
    setWindowTitle(tr("AI DJ settings"));
    AiSettings& settings = m_pService->settings();

    auto* pLayout = new QVBoxLayout(this);
    pLayout->setSpacing(10);
    pLayout->addWidget(makeLabel(tr("AI DJ settings"), Role::Title, this));
    auto* pIntro = makeLabel(
            tr("The suggestions work offline without any account. Gemini "
               "builds playlists from your wishes, tags moods, picks the next "
               "track and gives transition tips. Jev picks the next track in a "
               "fraction of a second. Keys are stored in the system's "
               "credential store, never in files."),
            Role::Muted,
            this);
    pIntro->setWordWrap(true);
    pLayout->addWidget(pIntro);

    auto* pGemini = new QGroupBox(tr("Google Gemini"), this);
    auto* pGeminiForm = new QFormLayout(pGemini);
    m_pGeminiKey = new QLineEdit(settings.geminiKey(), pGemini);
    m_pGeminiKey->setEchoMode(QLineEdit::Password);
    m_pGeminiKey->setPlaceholderText(tr("Paste your API key"));
    m_pGeminiModel = new QComboBox(pGemini);
    m_pGeminiModel->setEditable(true);
    m_pGeminiModel->addItems(AiService::suggestedGeminiModels());
    m_pGeminiModel->setCurrentText(settings.geminiModel());
    m_pGeminiModel->setToolTip(
            tr("%1 has the largest free daily quota. Larger models give "
               "better sets but allow fewer free requests.")
                    .arg(AiSettings::defaultGeminiModel()));
    auto* pTestRow = new QHBoxLayout();
    m_pTestGemini = new QPushButton(tr("Test connection"), pGemini);
    setRole(m_pTestGemini, Role::Ai);
    m_pGeminiTestResult = new QLabel(pGemini);
    m_pGeminiTestResult->setWordWrap(true);
    pTestRow->addWidget(m_pTestGemini);
    pTestRow->addWidget(m_pGeminiTestResult, 1);
    auto* pGeminiLink = makeLabel(
            tr("<a href=\"https://aistudio.google.com/apikey\">Get a free key in "
               "Google AI Studio</a>. The free tier has a daily limit; "
               "Flash-Lite models allow the most requests. In the EU, UK and "
               "Switzerland Google only allows paid keys for apps."),
            Role::Muted,
            pGemini);
    pGeminiLink->setOpenExternalLinks(true);
    pGeminiLink->setWordWrap(true);
    pGeminiForm->addRow(tr("API key"), m_pGeminiKey);
    pGeminiForm->addRow(tr("Model"), m_pGeminiModel);
    pGeminiForm->addRow(pTestRow);
    pGeminiForm->addRow(pGeminiLink);
    pLayout->addWidget(pGemini);

    auto* pJev = new QGroupBox(tr("Jev by TypeSafe AI (optional)"), this);
    auto* pJevForm = new QFormLayout(pJev);
    m_pJevKey = new QLineEdit(settings.jevKey(), pJev);
    m_pJevKey->setEchoMode(QLineEdit::Password);
    m_pJevKey->setPlaceholderText(tr("Paste your API key"));
    m_pJevEndpoint = new QLineEdit(settings.jevEndpoint(), pJev);
    auto* pJevLink = makeLabel(
            tr("<a href=\"https://console.typesafe.ai\">Get a key from TypeSafe "
               "AI</a>. Jev bills only input tokens (about $0.04 per million), "
               "one choice costs a tiny fraction of a cent. Without Jev, "
               "\"Ask AI to pick\" uses Gemini."),
            Role::Muted,
            pJev);
    pJevLink->setOpenExternalLinks(true);
    pJevLink->setWordWrap(true);
    pJevForm->addRow(tr("API key"), m_pJevKey);
    pJevForm->addRow(tr("Endpoint"), m_pJevEndpoint);
    pJevForm->addRow(pJevLink);
    pLayout->addWidget(pJev);

    m_pConsent = new QCheckBox(
            tr("Allow sending artist, title, genre, BPM, key and moods of my "
               "tracks to these services"),
            this);
    m_pConsent->setChecked(settings.dataSharingAllowed());
    pLayout->addWidget(m_pConsent);
    auto* pPrivacy = makeLabel(
            tr("No audio and no file names are sent. On free tiers the "
               "providers may use the requests to improve their services."),
            Role::Muted,
            this);
    pPrivacy->setWordWrap(true);
    pLayout->addWidget(pPrivacy);

    auto* pButtons = new QDialogButtonBox(
            QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    if (QPushButton* pSave = pButtons->button(QDialogButtonBox::Save)) {
        setRole(pSave, Role::Primary);
    }
    connect(pButtons, &QDialogButtonBox::accepted, this, &DlgAiDjSettings::slotAccept);
    connect(pButtons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    pLayout->addWidget(pButtons);

    connect(m_pTestGemini, &QPushButton::clicked, this, &DlgAiDjSettings::slotTestGemini);
    connect(m_pService,
            &AiService::connectionTested,
            this,
            &DlgAiDjSettings::slotConnectionTested);

    applyPageStyle(this);
    resize(600, sizeHint().height());
}

void DlgAiDjSettings::slotTestGemini() {
    m_pTestGemini->setEnabled(false);
    m_pGeminiTestResult->setStyleSheet(QString());
    m_pGeminiTestResult->setText(tr("Testing…"));
    m_pService->testGemini(m_pGeminiKey->text(), m_pGeminiModel->currentText());
}

void DlgAiDjSettings::slotConnectionTested(
        const QString& service, bool ok, const QString& message) {
    Q_UNUSED(service);
    m_pTestGemini->setEnabled(true);
    m_pGeminiTestResult->setStyleSheet(
            ok ? QStringLiteral("color: #3cc85a;") : QStringLiteral("color: #e0503a;"));
    m_pGeminiTestResult->setText(message);
}

void DlgAiDjSettings::slotAccept() {
    AiSettings& settings = m_pService->settings();
    settings.setGeminiKey(m_pGeminiKey->text());
    settings.setGeminiModel(m_pGeminiModel->currentText());
    settings.setJevKey(m_pJevKey->text());
    settings.setJevEndpoint(m_pJevEndpoint->text());
    settings.setDataSharingAllowed(m_pConsent->isChecked());
    accept();
}

DlgAiDj::DlgAiDj(QWidget* pParent,
        UserSettingsPointer pConfig,
        Library* pLibrary,
        KeyboardEventFilter* pKeyboard)
        : QWidget(pParent),
          m_pConfig(pConfig),
          m_pLibrary(pLibrary),
          m_pService(new AiService(pConfig, this)),
          m_pMoodStore(std::make_unique<MoodStore>(MoodStore::defaultFilePath(pConfig))),
          m_pendingMoodRequests(0) {
    Q_UNUSED(pKeyboard);
    m_pMoodStore->load();

    auto* pLayout = new QVBoxLayout(this);
    pLayout->setContentsMargins(6, 6, 6, 6);
    pLayout->setSpacing(6);

    // Card: now playing, energy curve of the event, set clock
    auto* pCard = new QFrame(this);
    setRole(pCard, Role::Card);
    auto* pCardLayout = new QHBoxLayout(pCard);
    pCardLayout->setContentsMargins(10, 6, 10, 6);
    pCardLayout->setSpacing(14);

    auto* pNowColumn = new QVBoxLayout();
    pNowColumn->setSpacing(3);
    pNowColumn->addWidget(makeLabel(tr("NOW PLAYING"), Role::Section, pCard));
    m_pNowTitle = makeLabel(QString(), Role::Title, pCard);
    m_pNowTitle->setTextFormat(Qt::PlainText);
    // Long titles must not widen the page
    m_pNowTitle->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_pNowTitle->setMinimumWidth(180);
    pNowColumn->addWidget(m_pNowTitle);
    auto* pChips = new QHBoxLayout();
    pChips->setSpacing(4);
    m_pNowBpm = makeLabel(QString(), Role::Chip, pCard);
    m_pNowKey = makeLabel(QString(), Role::Chip, pCard);
    m_pNowEnergy = makeLabel(QString(), Role::Chip, pCard);
    m_pNowGenre = makeLabel(QString(), Role::Chip, pCard);
    m_pNowGenre->setTextFormat(Qt::PlainText);
    m_pNowHint = makeLabel(QString(), Role::Muted, pCard);
    for (QLabel* pChip : {m_pNowBpm, m_pNowKey, m_pNowEnergy, m_pNowGenre, m_pNowHint}) {
        pChips->addWidget(pChip);
    }
    pChips->addStretch();
    pNowColumn->addLayout(pChips);
    pCardLayout->addLayout(pNowColumn, 3);

    m_pEnergyCurve = new EnergyCurveWidget(pCard);
    pCardLayout->addWidget(m_pEnergyCurve, 2);

    auto* pSetColumn = new QVBoxLayout();
    pSetColumn->setSpacing(3);
    pSetColumn->addWidget(makeLabel(tr("SET"), Role::Section, pCard));
    m_pSetClock = new QLabel(pCard);
    pSetColumn->addWidget(m_pSetClock);
    m_pSetProgress = new QProgressBar(pCard);
    m_pSetProgress->setRange(0, 1000);
    m_pSetProgress->setTextVisible(false);
    pSetColumn->addWidget(m_pSetProgress);
    auto* pNewSet = new QPushButton(tr("Start new set"), pCard);
    pNewSet->setToolTip(tr("Restart the set clock: the energy curve starts again"));
    pSetColumn->addWidget(pNewSet);
    pCardLayout->addLayout(pSetColumn, 1);
    pLayout->addWidget(pCard);

    // Row: event, mood, energy, set length and options
    auto* pContextRow = new QHBoxLayout();
    pContextRow->setSpacing(6);
    pContextRow->addWidget(makeLabel(tr("Event"), Role::Muted, this));
    m_pEvent = new QComboBox(this);
    for (const EventProfile& profile : eventProfiles()) {
        m_pEvent->addItem(profile.name, static_cast<int>(profile.type));
    }
    m_pEvent->setCurrentIndex(std::max(0, m_pEvent->findData(m_pConfig->getValue(kEventKey, 0))));
    pContextRow->addWidget(m_pEvent);

    pContextRow->addWidget(makeLabel(tr("Mood"), Role::Muted, this));
    m_pMood = new QComboBox(this);
    m_pMood->addItem(tr("Any"), QString());
    for (const QString& mood : moodVocabulary()) {
        m_pMood->addItem(mood, mood);
    }
    m_pMood->setCurrentIndex(std::max(0, m_pMood->findData(m_pConfig->getValueString(kMoodKey))));
    m_pMood->setToolTip(tr("Moods come from \"Tag moods with Gemini\""));
    pContextRow->addWidget(m_pMood);

    pContextRow->addWidget(makeLabel(tr("Energy"), Role::Muted, this));
    m_pEnergy = new QComboBox(this);
    m_pEnergy->addItems({tr("Follow the event"),
            tr("Low"),
            tr("Medium"),
            tr("High"),
            tr("Peak")});
    m_pEnergy->setCurrentIndex(
            std::clamp(m_pConfig->getValue(kEnergyKey, 0), 0, kEnergyLevelCount - 1));
    pContextRow->addWidget(m_pEnergy);

    pContextRow->addWidget(makeLabel(tr("Set length"), Role::Muted, this));
    m_pSetMinutes = new QSpinBox(this);
    m_pSetMinutes->setRange(10, 720);
    m_pSetMinutes->setSingleStep(15);
    m_pSetMinutes->setSuffix(tr(" min"));
    m_pSetMinutes->setValue(m_pConfig->getValue(kSetMinutesKey, 120));
    pContextRow->addWidget(m_pSetMinutes);

    pContextRow->addSpacing(8);
    m_pHarmonicOnly = new QCheckBox(tr("Harmonic only"), this);
    m_pHarmonicOnly->setToolTip(
            tr("Hide tracks whose key clashes with the playing track"));
    m_pHarmonicOnly->setChecked(m_pConfig->getValue(kHarmonicOnlyKey, false));
    pContextRow->addWidget(m_pHarmonicOnly);
    m_pAutoRefresh = new QCheckBox(tr("Follow the playing deck"), this);
    m_pAutoRefresh->setToolTip(tr("Update the suggestions when the playing track changes"));
    m_pAutoRefresh->setChecked(m_pConfig->getValue(kFollowDeckKey, true));
    pContextRow->addWidget(m_pAutoRefresh);
    pContextRow->addStretch();
    m_pAiState = makeLabel(QString(), Role::Chip, this);
    pContextRow->addWidget(m_pAiState);
    auto* pSettings = new QPushButton(tr("AI settings…"), this);
    pContextRow->addWidget(pSettings);
    pLayout->addLayout(pContextRow);

    // Suggestions
    m_pTable = new QTableWidget(0, kColumnCount, this);
    m_pTable->setHorizontalHeaderLabels({tr("Match"),
            tr("Artist"),
            tr("Title"),
            tr("BPM"),
            tr("Key"),
            tr("Energy"),
            tr("Moods"),
            tr("Why it fits")});
    m_pTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_pTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_pTable->verticalHeader()->hide();
    m_pTable->verticalHeader()->setDefaultSectionSize(24);
    m_pTable->horizontalHeader()->setStretchLastSection(true);
    m_pTable->horizontalHeader()->setHighlightSections(false);
    m_pTable->setAlternatingRowColors(true);
    m_pTable->setShowGrid(false);
    m_pTable->setWordWrap(false);
    m_pTable->setItemDelegateForColumn(kColumnScore, new ScoreBarDelegate(m_pTable));
    m_pTable->setItemDelegateForColumn(
            kColumnEnergy, new ScoreBarDelegate(m_pTable, kEnergyBarColor));
    m_pTable->setToolTip(tr("Double-click a suggestion to load it to a free deck"));
    pLayout->addWidget(m_pTable, 1);

    // Actions for the selected suggestions
    auto* pActions = new QHBoxLayout();
    pActions->setSpacing(6);
    auto* pLoad = new QPushButton(tr("Load to free deck"), this);
    setRole(pLoad, Role::Primary);
    auto* pQueue = new QPushButton(tr("Add to Auto DJ"), this);
    m_pAutoFill = new QCheckBox(tr("Keep Auto DJ filled"), this);
    m_pAutoFill->setToolTip(
            tr("AI Auto DJ: while this is on, the best next tracks are added "
               "to the Auto DJ queue so it never runs dry. Turn on Auto DJ to "
               "let Mixxx mix them."));
    m_pAutoFill->setChecked(m_pConfig->getValue(kAutoFillKey, false));
    auto* pRefresh = new QPushButton(tr("Refresh"), this);
    pRefresh->setToolTip(tr("Read the library again, e.g. after new tracks were analyzed"));
    m_pAskAi = new QPushButton(tr("Ask AI to pick"), this);
    setRole(m_pAskAi, Role::Ai);
    auto* pTip = new QPushButton(tr("Transition tip"), this);
    pActions->addWidget(pLoad);
    pActions->addWidget(pQueue);
    pActions->addSpacing(6);
    pActions->addWidget(m_pAutoFill);
    pActions->addStretch();
    pActions->addWidget(m_pAskAi);
    pActions->addWidget(pTip);
    pActions->addWidget(pRefresh);
    pLayout->addLayout(pActions);

    m_pTip = makeLabel(QString(), Role::Tip, this);
    m_pTip->setWordWrap(true);
    m_pTip->setTextFormat(Qt::PlainText);
    m_pTip->hide();
    pLayout->addWidget(m_pTip);

    // Set builder
    auto* pBuilderRow = new QHBoxLayout();
    pBuilderRow->setSpacing(6);
    pBuilderRow->addWidget(makeLabel(tr("BUILD A SET"), Role::Section, this));
    m_pPrompt = new QLineEdit(this);
    m_pPrompt->setClearButtonEnabled(true);
    m_pPrompt->setPlaceholderText(
            tr("Describe the set, e.g. \"romantic entrance for the bride, then "
               "uplifting songs for the exit\""));
    pBuilderRow->addWidget(m_pPrompt, 1);
    m_pPlaylistSize = new QSpinBox(this);
    m_pPlaylistSize->setRange(3, 100);
    m_pPlaylistSize->setValue(15);
    m_pPlaylistSize->setSuffix(tr(" tracks"));
    pBuilderRow->addWidget(m_pPlaylistSize);
    auto* pOffline = new QPushButton(tr("Build offline"), this);
    pOffline->setToolTip(tr(
            "Chains the best next tracks along the event's energy "
            "curve, instantly and without an account"));
    m_pGeminiPlaylist = new QPushButton(tr("Build with Gemini"), this);
    setRole(m_pGeminiPlaylist, Role::Ai);
    m_pTagMoods = new QPushButton(tr("Tag moods with Gemini"), this);
    setRole(m_pTagMoods, Role::Ai);
    pBuilderRow->addWidget(pOffline);
    pBuilderRow->addWidget(m_pGeminiPlaylist);
    pBuilderRow->addWidget(m_pTagMoods);
    pLayout->addLayout(pBuilderRow);

    m_pStatus = makeLabel(QString(), Role::Muted, this);
    m_pStatus->setWordWrap(true);
    m_pStatus->setTextFormat(Qt::PlainText);
    pLayout->addWidget(m_pStatus);

    connect(m_pEvent, &QComboBox::currentIndexChanged, this, [this]() {
        m_pConfig->setValue(kEventKey, m_pEvent->currentData().toInt());
        slotRefreshSuggestions();
    });
    connect(m_pMood, &QComboBox::currentIndexChanged, this, [this]() {
        m_pConfig->setValue(kMoodKey, m_pMood->currentData().toString());
        slotRefreshSuggestions();
    });
    connect(m_pEnergy, &QComboBox::currentIndexChanged, this, [this](int index) {
        m_pConfig->setValue(kEnergyKey, index);
        slotRefreshSuggestions();
    });
    connect(m_pSetMinutes, &QSpinBox::valueChanged, this, [this](int minutes) {
        m_pConfig->setValue(kSetMinutesKey, minutes);
        slotUpdateSetClock();
    });
    connect(m_pHarmonicOnly, &QCheckBox::toggled, this, [this](bool checked) {
        m_pConfig->setValue(kHarmonicOnlyKey, checked);
        slotRefreshSuggestions();
    });
    connect(m_pAutoRefresh, &QCheckBox::toggled, this, [this](bool checked) {
        m_pConfig->setValue(kFollowDeckKey, checked);
    });
    connect(m_pAutoFill, &QCheckBox::toggled, this, [this](bool checked) {
        m_pConfig->setValue(kAutoFillKey, checked);
        if (checked) {
            m_autoFillTimer.start(kAutoFillIntervalMs);
            slotAutoFillAutoDj();
        } else {
            m_autoFillTimer.stop();
        }
    });
    connect(pSettings, &QPushButton::clicked, this, &DlgAiDj::slotShowSettings);
    connect(pNewSet, &QPushButton::clicked, this, &DlgAiDj::slotStartNewSet);
    connect(pLoad, &QPushButton::clicked, this, &DlgAiDj::slotLoadSelectedToFreeDeck);
    connect(pQueue, &QPushButton::clicked, this, &DlgAiDj::slotAddSelectedToAutoDj);
    connect(pRefresh, &QPushButton::clicked, this, &DlgAiDj::slotReloadLibrary);
    connect(m_pAskAi, &QPushButton::clicked, this, &DlgAiDj::slotAskAi);
    connect(pTip, &QPushButton::clicked, this, &DlgAiDj::slotTransitionTip);
    connect(pOffline, &QPushButton::clicked, this, &DlgAiDj::slotBuildOfflinePlaylist);
    connect(m_pPrompt, &QLineEdit::returnPressed, this, [this]() {
        if (m_pGeminiPlaylist->isEnabled()) {
            slotBuildGeminiPlaylist();
        } else {
            slotBuildOfflinePlaylist();
        }
    });
    connect(m_pGeminiPlaylist, &QPushButton::clicked, this, &DlgAiDj::slotBuildGeminiPlaylist);
    connect(m_pTagMoods, &QPushButton::clicked, this, &DlgAiDj::slotTagMoods);
    connect(m_pTable, &QTableWidget::cellDoubleClicked, this, &DlgAiDj::slotLoadSelectedToFreeDeck);

    connect(m_pService, &AiService::playlistReady, this, &DlgAiDj::slotPlaylistReady);
    connect(m_pService, &AiService::moodTagsReady, this, &DlgAiDj::slotMoodTagsReady);
    connect(m_pService, &AiService::transitionTipReady, this, &DlgAiDj::slotTransitionTipReady);
    connect(m_pService, &AiService::nextTrackChosen, this, &DlgAiDj::slotNextTrackChosen);
    connect(m_pService, &AiService::failed, this, &DlgAiDj::slotAiFailed);

    connect(&PlayerInfo::instance(),
            &PlayerInfo::currentPlayingTrackChanged,
            this,
            &DlgAiDj::slotCurrentPlayingTrackChanged);

    connect(&m_clockTimer, &QTimer::timeout, this, &DlgAiDj::slotUpdateSetClock);
    m_clockTimer.start(1000);
    connect(&m_autoFillTimer, &QTimer::timeout, this, &DlgAiDj::slotAutoFillAutoDj);
    if (m_pAutoFill->isChecked()) {
        m_autoFillTimer.start(kAutoFillIntervalMs);
    }

    applyPageStyle(this);
    updateAiButtons();
    updateNowPlaying(currentContext());
    slotUpdateSetClock();
}

DlgAiDj::~DlgAiDj() = default;

void DlgAiDj::onShow() {
    slotReloadLibrary();
}

bool DlgAiDj::hasFocus() const {
    return m_pTable->hasFocus();
}

void DlgAiDj::setStatus(const QString& text) {
    m_pStatus->setText(text);
}

void DlgAiDj::setTip(const QString& text) {
    m_pTip->setText(text);
    m_pTip->setVisible(!text.isEmpty());
}

void DlgAiDj::updateAiButtons() {
    AiSettings& settings = m_pService->settings();
    const bool gemini = settings.geminiConfigured();
    const bool jev = settings.jevConfigured();
    m_pGeminiPlaylist->setEnabled(gemini);
    m_pTagMoods->setEnabled(gemini && m_pendingMoodRequests == 0);
    m_pAskAi->setEnabled(jev || gemini);
    const QString hint = tr("Set up a key in the AI settings to use this.");
    m_pGeminiPlaylist->setToolTip(gemini ? tr("Gemini picks and orders tracks of "
                                              "your library for the described set")
                                         : hint);
    m_pTagMoods->setToolTip(gemini ? tr("Gemini adds moods to tracks without moods, "
                                        "used by the mood filter")
                                   : hint);
    m_pAskAi->setToolTip(jev ? tr("Jev picks the best of the top suggestions")
                    : gemini ? tr("Gemini picks the best of the top suggestions")
                             : hint);
    if (gemini && jev) {
        m_pAiState->setText(tr("AI: Gemini + Jev"));
    } else if (gemini) {
        m_pAiState->setText(tr("AI: Gemini"));
    } else if (jev) {
        m_pAiState->setText(tr("AI: Jev"));
    } else {
        m_pAiState->setText(tr("Offline mode"));
    }
    m_pAiState->setToolTip(tr(
            "Suggestions, tips and the offline set builder "
            "always work offline"));
}

void DlgAiDj::slotShowSettings() {
    DlgAiDjSettings dialog(this, m_pService);
    if (dialog.exec() == QDialog::Accepted) {
        updateAiButtons();
        setStatus(tr("AI settings saved."));
    }
}

void DlgAiDj::slotReloadLibrary() {
    AiDjLibrary library(
            m_pLibrary->trackCollectionManager()->internalCollection()->database());
    m_candidates = library.loadCandidates();
    m_pMoodStore->apply(&m_candidates);
    slotRefreshSuggestions();
    // BPM and key are what the suggestions depend on most
    const auto notAnalyzed = std::count_if(m_candidates.cbegin(),
            m_candidates.cend(),
            [](const TrackCandidate& track) {
                return track.bpm <= 0.0 || track.key == track::io::key::INVALID;
            });
    if (notAnalyzed > 0 && notAnalyzed * 5 >= m_candidates.size()) {
        setStatus(tr(
                "%1 of %2 tracks have no BPM or key yet, so the "
                "suggestions are rough. Select them in Tracks and choose "
                "Analyze (or use Analyze in the sidebar) for "
                "harmonic, tempo-matched suggestions.")
                        .arg(notAnalyzed)
                        .arg(m_candidates.size()));
    }
}

std::optional<TrackCandidate> DlgAiDj::candidateForId(TrackId id) const {
    if (!id.isValid()) {
        return std::nullopt;
    }
    for (const TrackCandidate& candidate : m_candidates) {
        if (candidate.id == id) {
            return candidate;
        }
    }
    return std::nullopt;
}

std::optional<TrackCandidate> DlgAiDj::candidateForTrack(const TrackPointer& pTrack) const {
    if (!pTrack) {
        return std::nullopt;
    }
    if (auto candidate = candidateForId(pTrack->getId())) {
        return candidate;
    }
    // Not in the library (yet), use the loaded track's data
    TrackCandidate candidate;
    candidate.id = pTrack->getId();
    candidate.artist = pTrack->getArtist();
    candidate.title = pTrack->getTitle();
    candidate.genre = pTrack->getGenre();
    candidate.bpm = pTrack->getBpm();
    candidate.key = pTrack->getKey();
    candidate.durationSeconds = pTrack->getDuration();
    candidate.rating = pTrack->getRating();
    candidate.moods = m_pMoodStore->moods(candidate.id);
    return candidate;
}

QList<TrackCandidate> DlgAiDj::availableCandidates(const QSet<TrackId>& excludedIds) const {
    QSet<TrackId> loaded;
    const QMap<QString, TrackPointer> loadedTracks = PlayerInfo::instance().getLoadedTracks();
    for (const TrackPointer& pTrack : loadedTracks) {
        if (pTrack) {
            loaded.insert(pTrack->getId());
        }
    }
    QList<TrackCandidate> available;
    available.reserve(m_candidates.size());
    for (const TrackCandidate& candidate : m_candidates) {
        if (m_sessionPlayed.contains(candidate.id) || excludedIds.contains(candidate.id)) {
            continue;
        }
        TrackCandidate track = candidate;
        // Tracks in a deck are about to be played or are playing
        if (loaded.contains(track.id)) {
            track.playedThisSession = true;
        }
        available.append(std::move(track));
    }
    return available;
}

std::optional<double> DlgAiDj::setProgress() const {
    if (!m_setStart.isValid()) {
        return std::nullopt;
    }
    const double elapsedMinutes = m_setStart.secsTo(QDateTime::currentDateTime()) / 60.0;
    return std::clamp(elapsedMinutes / m_pSetMinutes->value(), 0.0, 1.0);
}

MixContext DlgAiDj::currentContext() const {
    MixContext context;
    context.event = static_cast<EventType>(m_pEvent->currentData().toInt());
    context.current = candidateForTrack(PlayerInfo::instance().getCurrentPlayingTrack());
    context.setProgress = setProgress().value_or(0.0);
    const double energy =
            kEnergyLevels[std::clamp(m_pEnergy->currentIndex(), 0, kEnergyLevelCount - 1)];
    if (energy >= 0.0) {
        context.targetEnergy = energy;
    }
    const QString mood = m_pMood->currentData().toString();
    if (!mood.isEmpty()) {
        context.moods << mood;
    }
    context.recentArtists = m_recentArtists;
    context.harmonicOnly = m_pHarmonicOnly->isChecked();
    return context;
}

void DlgAiDj::updateNowPlaying(const MixContext& context) {
    const bool playing = context.current.has_value();
    for (QLabel* pChip : {m_pNowBpm, m_pNowKey, m_pNowEnergy}) {
        pChip->setVisible(playing);
    }
    m_pNowHint->setVisible(!playing);
    if (!playing) {
        m_pNowTitle->setText(tr("Nothing is playing"));
        m_pNowGenre->hide();
        m_pNowHint->setText(tr("The suggestions fit the start of the event."));
        return;
    }
    const TrackCandidate& track = *context.current;
    const QString title = track.artist.isEmpty()
            ? track.title
            : track.artist + QStringLiteral(" – ") + track.title;
    m_pNowTitle->setText(title);
    m_pNowTitle->setToolTip(title);
    m_pNowBpm->setText(track.bpm > 0.0 ? tr("%1 BPM").arg(track.bpm, 0, 'f', 1)
                                       : tr("BPM ?"));
    m_pNowKey->setText(tr("Key %1").arg(keyName(track.key)));
    m_pNowKey->setStyleSheet(QStringLiteral("color: %1; font-weight: bold;")
                    .arg(keyColor(track.key).name()));
    m_pNowEnergy->setText(tr("Energy %1/10").arg(qRound(estimateEnergy(track) * 10)));
    m_pNowGenre->setText(track.genre);
    m_pNowGenre->setVisible(!track.genre.isEmpty());
}

void DlgAiDj::updateEnergyCurve(const MixContext& context) {
    const EventProfile& profile = eventProfile(context.event);
    m_pEnergyCurve->setCurve(profile.startEnergy, profile.middleEnergy, profile.endEnergy);
    // Before the set starts, the marker is at the start
    m_pEnergyCurve->setProgress(setProgress().value_or(0.0));
    m_pEnergyCurve->setTargetOverride(context.targetEnergy);
    m_pEnergyCurve->setCurrentEnergy(context.current
                    ? std::optional<double>(estimateEnergy(*context.current))
                    : std::nullopt);
}

void DlgAiDj::slotRefreshSuggestions() {
    const MixContext context = currentContext();
    m_suggestions = recommend(availableCandidates(), context, kMaxSuggestions);
    updateNowPlaying(context);
    updateEnergyCurve(context);

    m_pTable->setSortingEnabled(false);
    m_pTable->clearContents();
    m_pTable->setRowCount(static_cast<int>(m_suggestions.size()));
    for (int row = 0; row < m_suggestions.size(); ++row) {
        const Recommendation& suggestion = m_suggestions[row];
        const TrackCandidate& track = suggestion.track;
        auto* pScore = readOnlyItem(QString::number(qRound(suggestion.score * 100)) + QChar('%'));
        pScore->setData(Qt::UserRole, track.id.toVariant());
        pScore->setData(kBarValueRole, suggestion.score);
        m_pTable->setItem(row, kColumnScore, pScore);
        m_pTable->setItem(row, kColumnArtist, readOnlyItem(track.artist));
        auto* pTitle = readOnlyItem(track.title);
        QFont bold = pTitle->font();
        bold.setBold(true);
        pTitle->setFont(bold);
        m_pTable->setItem(row, kColumnTitle, pTitle);
        auto* pBpm = readOnlyItem(
                track.bpm > 0 ? QString::number(track.bpm, 'f', 1) : QStringLiteral("–"));
        if (track.bpm <= 0) {
            pBpm->setToolTip(tr("Not analyzed yet"));
        }
        pBpm->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        if (context.current) {
            pBpm->setForeground(scoreColor(suggestion.bpmScore));
        }
        m_pTable->setItem(row, kColumnBpm, pBpm);
        auto* pKey = readOnlyItem(keyName(track.key));
        pKey->setForeground(keyColor(track.key));
        pKey->setFont(bold);
        pKey->setTextAlignment(Qt::AlignCenter);
        if (context.current) {
            pKey->setToolTip(suggestion.keyScore >= 0.9 ? tr("Harmonic with the playing track")
                            : suggestion.keyScore < 0.5 ? tr("Key clash with the playing track")
                                                        : QString());
        }
        m_pTable->setItem(row, kColumnKey, pKey);
        auto* pEnergy = readOnlyItem(QString::number(qRound(suggestion.estimatedEnergy * 10)));
        pEnergy->setData(kBarValueRole, suggestion.estimatedEnergy);
        m_pTable->setItem(row, kColumnEnergy, pEnergy);
        m_pTable->setItem(row, kColumnMoods, readOnlyItem(track.moods.join(QStringLiteral(", "))));
        const QString reasons = suggestion.reasons.join(QStringLiteral("  ·  "));
        auto* pReasons = readOnlyItem(reasons);
        pReasons->setToolTip(reasons);
        m_pTable->setItem(row, kColumnReasons, pReasons);
    }
    m_pTable->resizeColumnsToContents();
    // Bars need some room, long names should not push "Why" out of sight
    m_pTable->setColumnWidth(kColumnScore, std::max(m_pTable->columnWidth(kColumnScore), 70));
    m_pTable->setColumnWidth(kColumnEnergy, std::max(m_pTable->columnWidth(kColumnEnergy), 70));
    for (int column : {kColumnArtist, kColumnTitle, kColumnMoods}) {
        m_pTable->setColumnWidth(column, std::min(m_pTable->columnWidth(column), 260));
    }
    if (m_candidates.isEmpty()) {
        setStatus(tr(
                "The library is empty: add music folders in the preferences "
                "or download free music in the Free Music Finder."));
    } else if (m_suggestions.isEmpty()) {
        setStatus(m_pHarmonicOnly->isChecked()
                        ? tr("No harmonic match in the library: uncheck \"Harmonic only\".")
                        : tr("No more tracks to suggest."));
    }
}

void DlgAiDj::slotCurrentPlayingTrackChanged(TrackPointer pTrack) {
    if (pTrack) {
        if (!m_setStart.isValid()) {
            // The set starts with the first track that plays
            m_setStart = QDateTime::currentDateTime();
        }
        m_sessionPlayed.insert(pTrack->getId());
        const QString artist = normalizedArtist(pTrack->getArtist());
        if (!artist.isEmpty()) {
            m_recentArtists.removeAll(artist);
            m_recentArtists.prepend(artist);
            while (m_recentArtists.size() > kRecentArtistCount) {
                m_recentArtists.removeLast();
            }
        }
    }
    if (m_pAutoRefresh->isChecked() && isVisible()) {
        slotRefreshSuggestions();
        setTip(QString());
    }
    if (m_pAutoFill->isChecked()) {
        slotAutoFillAutoDj();
    }
}

void DlgAiDj::slotStartNewSet() {
    m_setStart = QDateTime::currentDateTime();
    slotUpdateSetClock();
    slotRefreshSuggestions();
    setStatus(tr("New set started: the energy curve starts again."));
}

void DlgAiDj::slotUpdateSetClock() {
    const std::optional<double> progress = setProgress();
    if (!progress) {
        m_pSetClock->setText(tr("Starts with the first track"));
        m_pSetProgress->setValue(0);
        return;
    }
    const qint64 elapsed = m_setStart.secsTo(QDateTime::currentDateTime());
    const qint64 total = m_pSetMinutes->value() * 60;
    const qint64 remaining = std::max<qint64>(0, total - elapsed);
    m_pSetClock->setText(tr("%1 of %2 · %3 left")
                    .arg(formatClock(elapsed), formatClock(total), formatClock(remaining)));
    m_pSetProgress->setValue(qRound(*progress * 1000));
    m_pEnergyCurve->setProgress(progress);
}

QList<TrackId> DlgAiDj::selectedTrackIds() const {
    QList<TrackId> ids;
    const QList<QTableWidgetSelectionRange> ranges = m_pTable->selectedRanges();
    QList<int> rows;
    for (const auto& range : ranges) {
        for (int row = range.topRow(); row <= range.bottomRow(); ++row) {
            rows << row;
        }
    }
    std::sort(rows.begin(), rows.end());
    for (int row : rows) {
        const QTableWidgetItem* pItem = m_pTable->item(row, kColumnScore);
        if (pItem) {
            ids << TrackId(pItem->data(Qt::UserRole));
        }
    }
    return ids;
}

QString DlgAiDj::freeDeckGroup() const {
    const int numDecks = static_cast<int>(
            ControlObject::get(ConfigKey(QStringLiteral("[App]"), QStringLiteral("num_decks"))));
    for (int i = 0; i < numDecks; ++i) {
        const QString group = PlayerManager::groupForDeck(i);
        if (ControlObject::get(ConfigKey(group, QStringLiteral("play"))) == 0.0) {
            return group;
        }
    }
    return QString();
}

void DlgAiDj::slotLoadSelectedToFreeDeck() {
    const QList<TrackId> ids = selectedTrackIds();
    if (ids.isEmpty()) {
        setStatus(tr("Select a suggestion first."));
        return;
    }
    const QString group = freeDeckGroup();
    if (group.isEmpty()) {
        setStatus(tr(
                "All decks are playing: stop a deck first or add the "
                "track to Auto DJ."));
        return;
    }
    TrackPointer pTrack = m_pLibrary->trackCollectionManager()->getTrackById(ids.first());
    if (!pTrack) {
        setStatus(tr("The track could not be loaded."));
        return;
    }
    emit loadTrackToPlayer(pTrack, group, false);
    setStatus(tr("Loaded %1 - %2 to %3.").arg(pTrack->getArtist(), pTrack->getTitle(), group));
}

void DlgAiDj::slotAddSelectedToAutoDj() {
    const QList<TrackId> ids = selectedTrackIds();
    if (ids.isEmpty()) {
        setStatus(tr("Select one or more suggestions first."));
        return;
    }
    m_pLibrary->trackCollectionManager()
            ->internalCollection()
            ->getPlaylistDAO()
            .addTracksToAutoDJQueue(ids, PlaylistDAO::AutoDJSendLoc::BOTTOM);
    setStatus(tr("Added %n track(s) to the Auto DJ queue.", "", static_cast<int>(ids.size())));
}

void DlgAiDj::slotAutoFillAutoDj() {
    if (!m_pAutoFill->isChecked()) {
        return;
    }
    PlaylistDAO& playlistDao =
            m_pLibrary->trackCollectionManager()->internalCollection()->getPlaylistDAO();
    const int autoDjId = playlistDao.getPlaylistIdFromName(QStringLiteral(AUTODJ_TABLE));
    if (autoDjId < 0) {
        return;
    }
    const QList<TrackId> queue = playlistDao.getTrackIds(autoDjId);
    const int missing = kAutoFillQueueSize - static_cast<int>(queue.size());
    if (missing <= 0) {
        return;
    }
    if (m_candidates.isEmpty()) {
        AiDjLibrary library(
                m_pLibrary->trackCollectionManager()->internalCollection()->database());
        m_candidates = library.loadCandidates();
        m_pMoodStore->apply(&m_candidates);
    }
    MixContext context = currentContext();
    // Continue from the last queued track, so the queue flows
    if (!queue.isEmpty()) {
        if (auto last = candidateForId(queue.last())) {
            context.current = last;
        }
    }
    const QSet<TrackId> queued(queue.begin(), queue.end());
    const QList<Recommendation> next = buildSet(
            availableCandidates(queued), context, m_pSetMinutes->value(), missing);
    QList<TrackId> ids;
    QStringList titles;
    for (const Recommendation& item : next) {
        ids << item.track.id;
        titles << item.track.artist + QStringLiteral(" - ") + item.track.title;
    }
    if (ids.isEmpty()) {
        return;
    }
    playlistDao.addTracksToAutoDJQueue(ids, PlaylistDAO::AutoDJSendLoc::BOTTOM);
    setStatus(tr("Auto DJ queue filled up: %1").arg(titles.join(QStringLiteral(", "))));
}

void DlgAiDj::createPlaylist(const QList<TrackId>& trackIds, const QString& name) {
    PlaylistDAO& playlistDao =
            m_pLibrary->trackCollectionManager()->internalCollection()->getPlaylistDAO();
    QString uniqueName = name;
    const int playlistId = playlistDao.createUniquePlaylist(&uniqueName);
    if (playlistId < 0 || !playlistDao.appendTracksToPlaylist(trackIds, playlistId)) {
        setStatus(tr("The playlist could not be created."));
        return;
    }
    setStatus(tr(
            "Created the playlist \"%1\" with %n track(s). Find it under "
            "Playlists.",
            "",
            static_cast<int>(trackIds.size()))
                    .arg(uniqueName));
}

void DlgAiDj::slotBuildOfflinePlaylist() {
    MixContext context = currentContext();
    context.setProgress = 0.0;
    // A set is built for later: the played history does not matter, the
    // playing track is the starting point if there is one
    context.harmonicOnly = false;
    const int trackCount = m_pPlaylistSize->value();
    const QList<Recommendation> set =
            buildSet(m_candidates, context, m_pSetMinutes->value(), trackCount);
    QList<TrackId> ids;
    for (const Recommendation& item : set) {
        ids << item.track.id;
    }
    if (ids.isEmpty()) {
        setStatus(tr("No tracks to build a set from."));
        return;
    }
    createPlaylist(ids,
            tr("AI DJ - %1 - %2")
                    .arg(m_pEvent->currentText(),
                            QDate::currentDate().toString(Qt::ISODate)));
}

void DlgAiDj::slotBuildGeminiPlaylist() {
    MixContext context = currentContext();
    context.setProgress = 0.0;
    context.harmonicOnly = false;
    // Offer the best matching tracks for the event, not the whole library
    context.current.reset();
    const QList<Recommendation> candidates = recommend(m_candidates, context, kGeminiCandidates);
    if (candidates.isEmpty()) {
        setStatus(tr("No tracks to build a set from."));
        return;
    }
    setStatus(tr("Asking Gemini to build the set…"));
    m_pGeminiPlaylist->setEnabled(false);
    m_pService->requestPlaylist(m_pPrompt->text(),
            eventProfile(context.event),
            candidates,
            m_pPlaylistSize->value());
}

void DlgAiDj::slotPlaylistReady(const QList<TrackId>& trackIds, const QString& explanation) {
    updateAiButtons();
    createPlaylist(trackIds,
            tr("AI DJ (Gemini) - %1 - %2")
                    .arg(m_pEvent->currentText(),
                            QDate::currentDate().toString(Qt::ISODate)));
    setTip(explanation.isEmpty() ? QString() : tr("Gemini: %1").arg(explanation));
}

void DlgAiDj::slotTagMoods() {
    QList<TrackCandidate> untagged;
    for (const TrackCandidate& track : std::as_const(m_candidates)) {
        if (!m_pMoodStore->hasMoods(track.id)) {
            untagged << track;
            if (untagged.size() >= kMaxMoodTagsPerClick) {
                break;
            }
        }
    }
    if (untagged.isEmpty()) {
        setStatus(tr("All tracks already have moods."));
        return;
    }
    // One request per 60 tracks, see AiService
    m_pendingMoodRequests = static_cast<int>((untagged.size() + 59) / 60);
    updateAiButtons();
    setStatus(tr("Asking Gemini for the moods of %n track(s)…", "", static_cast<int>(untagged.size())));
    m_pService->requestMoodTags(untagged);
}

void DlgAiDj::slotMoodTagsReady(const QHash<TrackId, QStringList>& moods) {
    for (auto it = moods.begin(); it != moods.end(); ++it) {
        m_pMoodStore->setMoods(it.key(), it.value());
    }
    m_pMoodStore->save();
    m_pMoodStore->apply(&m_candidates);
    m_pendingMoodRequests = std::max(0, m_pendingMoodRequests - 1);
    updateAiButtons();
    setStatus(tr("Moods saved for %1 track(s), %2 tracks tagged in total.")
                    .arg(moods.size())
                    .arg(m_pMoodStore->size()));
    slotRefreshSuggestions();
}

void DlgAiDj::slotAskAi() {
    if (m_suggestions.isEmpty()) {
        setStatus(tr("There are no suggestions to choose from."));
        return;
    }
    const QList<Recommendation> candidates = m_suggestions.mid(0, kPickCandidates);
    if (m_pService->settings().jevConfigured()) {
        setStatus(tr("Asking Jev…"));
        m_pService->requestNextTrackChoice(currentContext(), candidates);
    } else {
        setStatus(tr("Asking Gemini…"));
        m_pService->requestNextTrackChoiceGemini(currentContext(), candidates);
    }
}

void DlgAiDj::slotNextTrackChosen(TrackId trackId, double confidence, const QString& service) {
    for (int row = 0; row < m_pTable->rowCount(); ++row) {
        const QTableWidgetItem* pItem = m_pTable->item(row, kColumnScore);
        if (pItem && TrackId(pItem->data(Qt::UserRole)) == trackId) {
            m_pTable->selectRow(row);
            m_pTable->scrollToItem(pItem);
            setStatus(tr(
                    "%1 picks: %2 - %3 (confidence %4%). Press \"Load to "
                    "free deck\" to use it.")
                            .arg(service,
                                    m_pTable->item(row, kColumnArtist)->text(),
                                    m_pTable->item(row, kColumnTitle)->text())
                            .arg(qRound(confidence * 100)));
            return;
        }
    }
    setStatus(tr("%1 picked a track that is no longer in the list.").arg(service));
}

void DlgAiDj::slotTransitionTip() {
    const QList<TrackId> ids = selectedTrackIds();
    const MixContext context = currentContext();
    if (ids.isEmpty() || !context.current) {
        setStatus(tr("Play a track and select the next one to get a tip."));
        return;
    }
    std::optional<TrackCandidate> next;
    for (const Recommendation& suggestion : std::as_const(m_suggestions)) {
        if (suggestion.track.id == ids.first()) {
            next = suggestion.track;
            break;
        }
    }
    if (!next) {
        return;
    }
    // The offline tip is instant, Gemini's tip replaces it when it arrives
    setTip(transitionTip(*context.current, *next));
    if (m_pService->settings().geminiConfigured()) {
        setStatus(tr("Asking Gemini for a detailed tip…"));
        m_pService->requestTransitionTip(*context.current, *next, eventProfile(context.event));
    }
}

void DlgAiDj::slotTransitionTipReady(const QString& tip) {
    setTip(tr("Gemini: %1").arg(tip));
    setStatus(QString());
}

void DlgAiDj::slotAiFailed(const QString& service, const QString& message) {
    Q_UNUSED(service);
    if (m_pendingMoodRequests > 0) {
        m_pendingMoodRequests = std::max(0, m_pendingMoodRequests - 1);
    }
    updateAiButtons();
    setStatus(message);
}

} // namespace aidj
} // namespace mixxx

#pragma once

#include <QDateTime>
#include <QDialog>
#include <QList>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <QWidget>
#include <memory>

#include "library/aidj/aidjlibrary.h"
#include "library/aidj/djrecommender.h"
#include "library/libraryview.h"
#include "preferences/usersettings.h"
#include "track/track_decl.h"

class Library;
class KeyboardEventFilter;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QTableWidget;

namespace mixxx {
namespace aidj {

class AiService;
class EnergyCurveWidget;

/// Settings of the AI services: API keys, model and consent
class DlgAiDjSettings : public QDialog {
    Q_OBJECT
  public:
    DlgAiDjSettings(QWidget* pParent, AiService* pService);

  private slots:
    void slotAccept();
    void slotTestGemini();
    void slotConnectionTested(const QString& service, bool ok, const QString& message);

  private:
    AiService* m_pService;
    QLineEdit* m_pGeminiKey;
    QComboBox* m_pGeminiModel;
    QPushButton* m_pTestGemini;
    QLabel* m_pGeminiTestResult;
    QLineEdit* m_pJevKey;
    QLineEdit* m_pJevEndpoint;
    QCheckBox* m_pConsent;
};

/// Library view of the AI DJ assistant: suggests the next track, builds sets
/// and playlists for the event, keeps the Auto DJ queue filled and asks
/// Gemini and Jev for help.
class DlgAiDj : public QWidget, public virtual LibraryView {
    Q_OBJECT
  public:
    DlgAiDj(QWidget* pParent,
            UserSettingsPointer pConfig,
            Library* pLibrary,
            KeyboardEventFilter* pKeyboard);
    ~DlgAiDj() override;

    void onShow() override;
    bool hasFocus() const override;

  signals:
    void loadTrack(TrackPointer pTrack);
    void loadTrackToPlayer(TrackPointer pTrack, const QString& group, bool play);

  private slots:
    void slotRefreshSuggestions();
    void slotReloadLibrary();
    void slotCurrentPlayingTrackChanged(TrackPointer pTrack);
    void slotLoadSelectedToFreeDeck();
    void slotAddSelectedToAutoDj();
    void slotBuildOfflinePlaylist();
    void slotBuildGeminiPlaylist();
    void slotTagMoods();
    void slotAskAi();
    void slotTransitionTip();
    void slotStartNewSet();
    void slotShowSettings();
    void slotUpdateSetClock();
    void slotAutoFillAutoDj();

    void slotPlaylistReady(const QList<TrackId>& trackIds, const QString& explanation);
    void slotMoodTagsReady(const QHash<TrackId, QStringList>& moods);
    void slotTransitionTipReady(const QString& tip);
    void slotNextTrackChosen(TrackId trackId, double confidence, const QString& service);
    void slotAiFailed(const QString& service, const QString& message);

  private:
    MixContext currentContext() const;
    std::optional<TrackCandidate> candidateForTrack(const TrackPointer& pTrack) const;
    std::optional<TrackCandidate> candidateForId(TrackId id) const;
    /// Library tracks without the ones played in this session, loaded in a
    /// deck or in excludedIds
    QList<TrackCandidate> availableCandidates(const QSet<TrackId>& excludedIds = {}) const;
    QList<TrackId> selectedTrackIds() const;
    /// Group of the first deck that is not playing, empty if all play
    QString freeDeckGroup() const;
    void createPlaylist(const QList<TrackId>& trackIds, const QString& name);
    void setStatus(const QString& text);
    void setTip(const QString& text);
    void updateAiButtons();
    void updateNowPlaying(const MixContext& context);
    void updateEnergyCurve(const MixContext& context);
    std::optional<double> setProgress() const;

    UserSettingsPointer m_pConfig;
    Library* m_pLibrary;
    AiService* m_pService;
    std::unique_ptr<MoodStore> m_pMoodStore;

    QList<TrackCandidate> m_candidates;
    QList<Recommendation> m_suggestions;
    QDateTime m_setStart;
    QTimer m_clockTimer;
    QTimer m_autoFillTimer;
    // Tracks that played since Mixxx started
    QSet<TrackId> m_sessionPlayed;
    // Artists of the last played tracks (lower case), newest first
    QStringList m_recentArtists;
    // Number of mood tagging requests that are still running
    int m_pendingMoodRequests;

    QComboBox* m_pEvent;
    QComboBox* m_pMood;
    QComboBox* m_pEnergy;
    QSpinBox* m_pSetMinutes;
    QCheckBox* m_pAutoRefresh;
    QCheckBox* m_pHarmonicOnly;
    QCheckBox* m_pAutoFill;
    QLabel* m_pAiState;
    QLabel* m_pNowTitle;
    QLabel* m_pNowBpm;
    QLabel* m_pNowKey;
    QLabel* m_pNowEnergy;
    QLabel* m_pNowGenre;
    QLabel* m_pNowHint;
    EnergyCurveWidget* m_pEnergyCurve;
    QLabel* m_pSetClock;
    QProgressBar* m_pSetProgress;
    QTableWidget* m_pTable;
    QLabel* m_pTip;
    QLineEdit* m_pPrompt;
    QSpinBox* m_pPlaylistSize;
    QPushButton* m_pAskAi;
    QPushButton* m_pGeminiPlaylist;
    QPushButton* m_pTagMoods;
    QLabel* m_pStatus;
};

} // namespace aidj
} // namespace mixxx

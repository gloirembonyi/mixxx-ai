#pragma once

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QObject>
#include <QPointer>
#include <QString>
#include <functional>

#include "library/aidj/djrecommender.h"
#include "preferences/usersettings.h"
#include "track/trackid.h"

class QNetworkReply;

namespace mixxx {
namespace aidj {

/// Settings of the AI services. The API keys are kept in the operating
/// system's credential store (QtKeychain) when available.
class AiSettings {
  public:
    explicit AiSettings(UserSettingsPointer pConfig);

    QString geminiKey() const;
    void setGeminiKey(const QString& key);
    QString geminiModel() const;
    void setGeminiModel(const QString& model);

    QString jevKey() const;
    void setJevKey(const QString& key);
    QString jevEndpoint() const;
    void setJevEndpoint(const QString& url);

    /// The user agreed that track information is sent to the AI services
    bool dataSharingAllowed() const;
    void setDataSharingAllowed(bool allowed);

    bool geminiConfigured() const {
        return dataSharingAllowed() && !geminiKey().isEmpty();
    }
    bool jevConfigured() const {
        return dataSharingAllowed() && !jevKey().isEmpty();
    }

    static QString defaultGeminiModel();
    static QString defaultJevEndpoint();

  private:
    QString readSecret(const QString& name) const;
    void writeSecret(const QString& name, const QString& value);

    UserSettingsPointer m_pConfig;
    // Secrets are read once and cached, the keychain may be slow
    mutable QHash<QString, QString> m_secretCache;
};

/// Talks to Google Gemini (playlists, moods, transition tips) and to the Jev
/// decision model by TypeSafe AI (fast choice of the next track).
///
/// All requests are asynchronous. Every request emits either its result
/// signal or failed().
class AiService : public QObject {
    Q_OBJECT
  public:
    AiService(UserSettingsPointer pConfig, QObject* pParent = nullptr);

    AiSettings& settings() {
        return m_settings;
    }

    /// Gemini selects tracks for the described set from the candidates
    void requestPlaylist(const QString& description,
            const EventProfile& event,
            const QList<Recommendation>& candidates,
            int trackCount);

    /// Gemini assigns moods from moodVocabulary() to the tracks
    void requestMoodTags(const QList<TrackCandidate>& tracks);

    /// Gemini explains how to mix from one track into the next
    void requestTransitionTip(const TrackCandidate& from,
            const TrackCandidate& to,
            const EventProfile& event);

    /// Jev picks the best next track from the candidates
    void requestNextTrackChoice(const MixContext& context,
            const QList<Recommendation>& candidates);

    /// Gemini picks the best next track from the candidates (used when Jev
    /// is not set up), emits nextTrackChosen()
    void requestNextTrackChoiceGemini(const MixContext& context,
            const QList<Recommendation>& candidates);

    /// Sends a tiny request with the given key and model (before they are
    /// saved), emits connectionTested()
    void testGemini(const QString& key, const QString& model);

    /// Known Gemini models for the settings, the default first
    static QStringList suggestedGeminiModels();

    /// Generation config adjusted to the model: Gemini 3 models should keep
    /// the default temperature and get a low thinking level (public for tests)
    static QJsonObject adjustGeminiConfig(QJsonObject config, const QString& model);

    /// Parses the JSON object of a Gemini text response, also if the model
    /// wrapped it in a Markdown code block (public for tests)
    static QJsonObject parseGeminiJson(const QJsonObject& response);

    /// Why a Gemini response has no text, e.g. blocked by the safety filter
    static QString geminiEmptyReason(const QJsonObject& response);

    /// Compact one-line description of a track for prompts
    static QString describeTrack(const TrackCandidate& track, double energy);

    /// Builds the Jev "choice" request body (public for tests)
    static QJsonObject buildJevChoiceRequest(const MixContext& context,
            const QList<Recommendation>& candidates);
    /// Parses the choice of a Jev response, returns an invalid id on errors
    static TrackId parseJevChoice(const QJsonObject& response, double* pConfidence);

    /// Extracts the JSON text of a Gemini generateContent response
    static QString geminiResponseText(const QJsonObject& response);

  signals:
    void playlistReady(const QList<TrackId>& trackIds, const QString& explanation);
    void moodTagsReady(const QHash<TrackId, QStringList>& moods);
    void transitionTipReady(const QString& tip);
    void nextTrackChosen(TrackId trackId, double confidence, const QString& service);
    void connectionTested(const QString& service, bool ok, const QString& message);
    void failed(const QString& service, const QString& message);

  private:
    using JsonHandler = std::function<void(const QJsonObject&)>;
    using ErrorHandler = std::function<void(const QString&)>;

    void postGemini(const QJsonObject& generationConfig,
            const QString& prompt,
            JsonHandler onResult);
    void postGeminiWith(const QString& key,
            const QString& model,
            const QJsonObject& generationConfig,
            const QString& prompt,
            JsonHandler onResult,
            ErrorHandler onError);
    /// onError defaults to emitting failed()
    void postJson(QNetworkRequest request,
            const QJsonObject& body,
            const QString& service,
            JsonHandler onResult,
            ErrorHandler onError = nullptr);

    AiSettings m_settings;
    QNetworkAccessManager m_network;
};

} // namespace aidj
} // namespace mixxx

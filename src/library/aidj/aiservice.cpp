#include "library/aidj/aiservice.h"

#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QSet>
#include <QUrl>
#include <algorithm>

#include "library/aidj/aidjlibrary.h"
#include "moc_aiservice.cpp"
#include "util/logger.h"

#ifdef __QTKEYCHAIN__
#include <qt6keychain/keychain.h>
#endif

namespace mixxx {
namespace aidj {

namespace {

const Logger kLogger("AiService");

const QString kConfigGroup = QStringLiteral("[AiDj]");
const QString kKeychainService = QStringLiteral("Mixxx AI DJ");
const QString kGeminiKeyName = QStringLiteral("GeminiApiKey");
const QString kJevKeyName = QStringLiteral("JevApiKey");

const QString kGeminiService = QStringLiteral("Gemini");
const QString kJevService = QStringLiteral("Jev");

const QString kGeminiUrl = QStringLiteral(
        "https://generativelanguage.googleapis.com/v1beta/models/%1:generateContent");

// Requests to free tiers can take a while, but not forever
constexpr int kTimeoutMs = 60000;
// Jev allows up to 255 options, fewer keep the request small and fast
constexpr int kMaxJevOptions = 25;
// Tracks per mood tagging request, keeps responses well below the limits
constexpr int kMaxMoodTracksPerRequest = 60;
// Candidates sent for a playlist, about 30 tokens each
constexpr int kMaxPlaylistCandidates = 400;

QString jevOptionKey(TrackId id) {
    return QStringLiteral("t") + id.toString();
}

QString friendlyHttpError(const QString& service, int httpStatus, const QString& details) {
    switch (httpStatus) {
    case 400:
        return QObject::tr("%1 rejected the request: %2").arg(service, details);
    case 401:
    case 403:
        return QObject::tr(
                "%1: the API key is invalid or not allowed. Check it in the AI "
                "settings.")
                .arg(service);
    case 404:
        return QObject::tr(
                "%1: the model or endpoint was not found. Check the model name "
                "in the AI settings (%2).")
                .arg(service, details);
    case 429:
        return QObject::tr(
                "%1: the rate limit or the free daily quota is used up. Wait a "
                "minute or try again tomorrow. The offline suggestions keep "
                "working.")
                .arg(service);
    case 500:
    case 503:
    case 529:
        return QObject::tr("%1 is overloaded right now, try again in a moment.")
                .arg(service);
    default:
        return QObject::tr("%1 error %2: %3").arg(service).arg(httpStatus).arg(details);
    }
}

QJsonObject stringSchema() {
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}};
}

} // namespace

AiSettings::AiSettings(UserSettingsPointer pConfig)
        : m_pConfig(std::move(pConfig)) {
}

// static
QString AiSettings::defaultGeminiModel() {
    // Flash-Lite has the largest free daily quota
    return QStringLiteral("gemini-3.5-flash-lite");
}

// static
QString AiSettings::defaultJevEndpoint() {
    return QStringLiteral("https://api.typesafe.ai/v1/systemone");
}

QString AiSettings::readSecret(const QString& name) const {
    const auto cached = m_secretCache.constFind(name);
    if (cached != m_secretCache.constEnd()) {
        return cached.value();
    }
    QString value;
#ifdef __QTKEYCHAIN__
    QKeychain::ReadPasswordJob job(kKeychainService);
    job.setAutoDelete(false);
    job.setKey(name);
    QEventLoop loop;
    QObject::connect(&job, &QKeychain::Job::finished, &loop, &QEventLoop::quit);
    job.start();
    loop.exec();
    if (job.error() == QKeychain::NoError) {
        value = job.textData();
    } else if (job.error() != QKeychain::EntryNotFound) {
        kLogger.warning() << "Failed to read" << name << "from the keychain:"
                          << job.errorString();
    }
#else
    value = m_pConfig->getValueString(ConfigKey(kConfigGroup, name));
#endif
    m_secretCache.insert(name, value);
    return value;
}

void AiSettings::writeSecret(const QString& name, const QString& value) {
    m_secretCache.insert(name, value);
#ifdef __QTKEYCHAIN__
    QEventLoop loop;
    if (value.isEmpty()) {
        QKeychain::DeletePasswordJob job(kKeychainService);
        job.setAutoDelete(false);
        job.setKey(name);
        QObject::connect(&job, &QKeychain::Job::finished, &loop, &QEventLoop::quit);
        job.start();
        loop.exec();
        return;
    }
    QKeychain::WritePasswordJob job(kKeychainService);
    job.setAutoDelete(false);
    job.setKey(name);
    job.setTextData(value);
    QObject::connect(&job, &QKeychain::Job::finished, &loop, &QEventLoop::quit);
    job.start();
    loop.exec();
    if (job.error() != QKeychain::NoError) {
        kLogger.warning() << "Failed to store" << name << "in the keychain:"
                          << job.errorString();
    }
#else
    m_pConfig->setValue(ConfigKey(kConfigGroup, name), value);
#endif
}

QString AiSettings::geminiKey() const {
    return readSecret(kGeminiKeyName);
}

void AiSettings::setGeminiKey(const QString& key) {
    writeSecret(kGeminiKeyName, key.trimmed());
}

QString AiSettings::geminiModel() const {
    const QString model = m_pConfig->getValueString(
            ConfigKey(kConfigGroup, QStringLiteral("GeminiModel")));
    return model.isEmpty() ? defaultGeminiModel() : model;
}

void AiSettings::setGeminiModel(const QString& model) {
    m_pConfig->setValue(ConfigKey(kConfigGroup, QStringLiteral("GeminiModel")), model.trimmed());
}

QString AiSettings::jevKey() const {
    return readSecret(kJevKeyName);
}

void AiSettings::setJevKey(const QString& key) {
    writeSecret(kJevKeyName, key.trimmed());
}

QString AiSettings::jevEndpoint() const {
    const QString url = m_pConfig->getValueString(
            ConfigKey(kConfigGroup, QStringLiteral("JevEndpoint")));
    return url.isEmpty() ? defaultJevEndpoint() : url;
}

void AiSettings::setJevEndpoint(const QString& url) {
    m_pConfig->setValue(ConfigKey(kConfigGroup, QStringLiteral("JevEndpoint")), url.trimmed());
}

bool AiSettings::dataSharingAllowed() const {
    return m_pConfig->getValue(ConfigKey(kConfigGroup, QStringLiteral("DataSharingAllowed")), false);
}

void AiSettings::setDataSharingAllowed(bool allowed) {
    m_pConfig->setValue(ConfigKey(kConfigGroup, QStringLiteral("DataSharingAllowed")), allowed);
}

AiService::AiService(UserSettingsPointer pConfig, QObject* pParent)
        : QObject(pParent),
          m_settings(std::move(pConfig)) {
    m_network.setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
    m_network.setTransferTimeout(kTimeoutMs);
}

// static
QString AiService::describeTrack(const TrackCandidate& track, double energy) {
    QStringList parts;
    parts << track.artist + QStringLiteral(" - ") + track.title;
    if (!track.genre.isEmpty()) {
        parts << track.genre;
    }
    if (track.bpm > 0.0) {
        parts << QString::number(track.bpm, 'f', 0) + QStringLiteral(" BPM");
    }
    if (track.key != track::io::key::INVALID) {
        parts << QStringLiteral("key ") + keyName(track.key);
    }
    parts << QStringLiteral("energy ") + QString::number(energy, 'f', 2);
    if (!track.moods.isEmpty()) {
        parts << QStringLiteral("moods ") + track.moods.join(QChar('/'));
    }
    if (track.durationSeconds > 0.0) {
        parts << QString::number(track.durationSeconds / 60.0, 'f', 1) + QStringLiteral(" min");
    }
    return parts.join(QStringLiteral(", "));
}

void AiService::postJson(QNetworkRequest request,
        const QJsonObject& body,
        const QString& service,
        JsonHandler onResult,
        ErrorHandler onError) {
    if (!onError) {
        onError = [this, service](const QString& message) {
            emit failed(service, message);
        };
    }
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Mixxx AI DJ"));
    QNetworkReply* pReply = m_network.post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(pReply, &QNetworkReply::finished, this, [pReply, service, onResult, onError]() {
        pReply->deleteLater();
        const int status = pReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray data = pReply->readAll();
        if (pReply->error() != QNetworkReply::NoError && status == 0) {
            onError(tr(
                    "%1 could not be reached: %2. Check the internet "
                    "connection.")
                            .arg(service, pReply->errorString()));
            return;
        }
        const QJsonObject response = QJsonDocument::fromJson(data).object();
        if (status < 200 || status >= 300) {
            QString details = response.value(QStringLiteral("error"))
                                      .toObject()
                                      .value(QStringLiteral("message"))
                                      .toString();
            if (details.isEmpty()) {
                details = QString::fromUtf8(data.left(300));
            }
            kLogger.warning() << service << "HTTP" << status << details;
            onError(friendlyHttpError(service, status, details));
            return;
        }
        onResult(response);
    });
}

void AiService::postGemini(const QJsonObject& generationConfig,
        const QString& prompt,
        JsonHandler onResult) {
    if (!m_settings.geminiConfigured()) {
        emit failed(kGeminiService,
                tr("Add a Gemini API key and allow sending track information "
                   "in the AI settings."));
        return;
    }
    postGeminiWith(m_settings.geminiKey(),
            m_settings.geminiModel(),
            generationConfig,
            prompt,
            std::move(onResult),
            nullptr);
}

void AiService::postGeminiWith(const QString& key,
        const QString& model,
        const QJsonObject& generationConfig,
        const QString& prompt,
        JsonHandler onResult,
        ErrorHandler onError) {
    QNetworkRequest request(QUrl(kGeminiUrl.arg(model)));
    // The header keeps the key out of URLs and logs
    request.setRawHeader("x-goog-api-key", key.toUtf8());
    const QJsonObject body{
            {QStringLiteral("contents"),
                    QJsonArray{QJsonObject{
                            {QStringLiteral("role"), QStringLiteral("user")},
                            {QStringLiteral("parts"),
                                    QJsonArray{QJsonObject{
                                            {QStringLiteral("text"), prompt}}}}}}},
            {QStringLiteral("generationConfig"), adjustGeminiConfig(generationConfig, model)},
    };
    postJson(request, body, kGeminiService, std::move(onResult), std::move(onError));
}

// static
QStringList AiService::suggestedGeminiModels() {
    return {AiSettings::defaultGeminiModel(),
            QStringLiteral("gemini-3.1-flash-lite"),
            QStringLiteral("gemini-3.5-flash"),
            QStringLiteral("gemini-3.8-flash")};
}

// static
QJsonObject AiService::adjustGeminiConfig(QJsonObject config, const QString& model) {
    if (!model.startsWith(QStringLiteral("gemini-3"))) {
        return config;
    }
    // Google: lower temperatures can make Gemini 3 loop or perform worse
    config.remove(QStringLiteral("temperature"));
    if (!config.contains(QStringLiteral("thinkingConfig"))) {
        // "low" is supported by all Gemini 3 text models and keeps the
        // answers fast; thinking tokens also count against maxOutputTokens
        config.insert(QStringLiteral("thinkingConfig"),
                QJsonObject{{QStringLiteral("thinkingLevel"), QStringLiteral("low")}});
    }
    return config;
}

// static
QJsonObject AiService::parseGeminiJson(const QJsonObject& response) {
    QString text = geminiResponseText(response).trimmed();
    if (text.startsWith(QStringLiteral("```"))) {
        // ```json ... ```
        const int firstNewline = text.indexOf(QChar('\n'));
        text = firstNewline >= 0 ? text.mid(firstNewline + 1) : QString();
        text = text.trimmed();
        if (text.endsWith(QStringLiteral("```"))) {
            text.chop(3);
        }
    }
    return QJsonDocument::fromJson(text.toUtf8()).object();
}

// static
QString AiService::geminiEmptyReason(const QJsonObject& response) {
    const QString blockReason = response.value(QStringLiteral("promptFeedback"))
                                        .toObject()
                                        .value(QStringLiteral("blockReason"))
                                        .toString();
    if (!blockReason.isEmpty()) {
        return tr("Gemini blocked the request (%1).").arg(blockReason);
    }
    const QJsonArray candidates = response.value(QStringLiteral("candidates")).toArray();
    const QString finishReason = candidates.isEmpty()
            ? QString()
            : candidates.first()
                      .toObject()
                      .value(QStringLiteral("finishReason"))
                      .toString();
    if (finishReason == QStringLiteral("MAX_TOKENS")) {
        return tr(
                "Gemini's answer was cut off. Try a smaller request or a "
                "Flash-Lite model.");
    }
    if (finishReason == QStringLiteral("SAFETY") ||
            finishReason == QStringLiteral("PROHIBITED_CONTENT") ||
            finishReason == QStringLiteral("BLOCKLIST")) {
        return tr(
                "Gemini's safety filter stopped the answer. Try other "
                "wording.");
    }
    if (!finishReason.isEmpty() && finishReason != QStringLiteral("STOP")) {
        return tr("Gemini stopped without an answer (%1).").arg(finishReason);
    }
    return tr("Gemini returned no usable answer, try again.");
}

void AiService::testGemini(const QString& key, const QString& model) {
    if (key.trimmed().isEmpty()) {
        emit connectionTested(kGeminiService, false, tr("Paste an API key first."));
        return;
    }
    const QString usedModel = model.trimmed().isEmpty()
            ? AiSettings::defaultGeminiModel()
            : model.trimmed();
    const QJsonObject config{{QStringLiteral("maxOutputTokens"), 256}};
    postGeminiWith(key.trimmed(), usedModel, config, QStringLiteral("Reply with the single word OK."), [this, usedModel](const QJsonObject& response) {
                Q_UNUSED(response);
                emit connectionTested(kGeminiService,
                        true,
                        tr("Gemini works with the model %1.").arg(usedModel)); }, [this](const QString& message) { emit connectionTested(kGeminiService, false, message); });
}

// static
QString AiService::geminiResponseText(const QJsonObject& response) {
    const QJsonArray candidates = response.value(QStringLiteral("candidates")).toArray();
    if (candidates.isEmpty()) {
        return QString();
    }
    const QJsonArray parts = candidates.first()
                                     .toObject()
                                     .value(QStringLiteral("content"))
                                     .toObject()
                                     .value(QStringLiteral("parts"))
                                     .toArray();
    QString text;
    for (const QJsonValue& part : parts) {
        text += part.toObject().value(QStringLiteral("text")).toString();
    }
    return text;
}

void AiService::requestPlaylist(const QString& description,
        const EventProfile& event,
        const QList<Recommendation>& candidates,
        int trackCount) {
    QStringList lines;
    const int count = std::min(static_cast<int>(candidates.size()), kMaxPlaylistCandidates);
    for (int i = 0; i < count; ++i) {
        const Recommendation& candidate = candidates[i];
        lines << candidate.track.id.toString() + QStringLiteral(": ") +
                        describeTrack(candidate.track, candidate.estimatedEnergy);
    }
    const QString prompt = QStringLiteral(
            "You are an experienced professional DJ. Build a playlist of about "
            "%1 tracks for %2.\n"
            "Request of the DJ: %3\n"
            "Rules: only use tracks from the list below, refer to them by their "
            "numeric id. Order them for smooth transitions: compatible keys "
            "(Camelot wheel), gradual BPM changes, and an energy curve that "
            "fits the event. Avoid repeating the same artist back to back.\n"
            "Explain the idea of the set in two short sentences.\n"
            "Tracks (id: artist - title, genre, BPM, key, energy 0-1, moods, "
            "length):\n%4")
                                   .arg(trackCount)
                                   .arg(event.description,
                                           description.isEmpty()
                                                   ? QStringLiteral("(no extra wishes)")
                                                   : description,
                                           lines.join(QChar('\n')));

    const QJsonObject schema{
            {QStringLiteral("type"), QStringLiteral("object")},
            {QStringLiteral("properties"),
                    QJsonObject{
                            {QStringLiteral("trackIds"),
                                    QJsonObject{
                                            {QStringLiteral("type"), QStringLiteral("array")},
                                            {QStringLiteral("items"),
                                                    QJsonObject{{QStringLiteral("type"),
                                                            QStringLiteral("integer")}}},
                                    }},
                            {QStringLiteral("explanation"), stringSchema()},
                    }},
            {QStringLiteral("required"),
                    QJsonArray{QStringLiteral("trackIds"), QStringLiteral("explanation")}},
    };
    const QJsonObject config{
            {QStringLiteral("responseMimeType"), QStringLiteral("application/json")},
            {QStringLiteral("responseJsonSchema"), schema},
            {QStringLiteral("temperature"), 0.7},
    };

    // Only accept ids that were offered, the model may invent others
    QSet<QString> offeredIds;
    for (int i = 0; i < count; ++i) {
        offeredIds.insert(candidates[i].track.id.toString());
    }
    postGemini(config, prompt, [this, offeredIds](const QJsonObject& response) {
        const QJsonObject result = parseGeminiJson(response);
        if (result.isEmpty()) {
            emit failed(kGeminiService, geminiEmptyReason(response));
            return;
        }
        QList<TrackId> ids;
        QSet<QString> seen;
        const QJsonArray array = result.value(QStringLiteral("trackIds")).toArray();
        for (const QJsonValue& value : array) {
            const QString id = QString::number(value.toInt());
            if (offeredIds.contains(id) && !seen.contains(id)) {
                seen.insert(id);
                ids.append(TrackId(QVariant(value.toInt())));
            }
        }
        if (ids.isEmpty()) {
            emit failed(kGeminiService, tr("Gemini did not return usable tracks."));
            return;
        }
        emit playlistReady(ids, result.value(QStringLiteral("explanation")).toString());
    });
}

void AiService::requestMoodTags(const QList<TrackCandidate>& tracks) {
    for (int start = 0; start < tracks.size(); start += kMaxMoodTracksPerRequest) {
        QStringList lines;
        QSet<QString> offeredIds;
        const int end = std::min(static_cast<int>(tracks.size()),
                start + kMaxMoodTracksPerRequest);
        for (int i = start; i < end; ++i) {
            const TrackCandidate& track = tracks[i];
            offeredIds.insert(track.id.toString());
            lines << track.id.toString() + QStringLiteral(": ") +
                            describeTrack(track, estimateEnergy(track));
        }
        const QString prompt = QStringLiteral(
                "You are a music expert. For each track, choose 1 to 3 moods that "
                "describe how the song feels, based on what you know about the "
                "song and its metadata. If you do not know the song, judge by "
                "artist, genre, tempo, key (A = minor, B = major) and energy.\n"
                "Tracks:\n%1")
                                       .arg(lines.join(QChar('\n')));
        QJsonArray moodEnum;
        for (const QString& mood : moodVocabulary()) {
            moodEnum.append(mood);
        }
        const QJsonObject schema{
                {QStringLiteral("type"), QStringLiteral("object")},
                {QStringLiteral("properties"),
                        QJsonObject{{QStringLiteral("tracks"),
                                QJsonObject{
                                        {QStringLiteral("type"), QStringLiteral("array")},
                                        {QStringLiteral("items"),
                                                QJsonObject{
                                                        {QStringLiteral("type"),
                                                                QStringLiteral("object")},
                                                        {QStringLiteral("properties"),
                                                                QJsonObject{
                                                                        {QStringLiteral("id"),
                                                                                QJsonObject{{QStringLiteral("type"),
                                                                                        QStringLiteral("integer")}}},
                                                                        {QStringLiteral("moods"),
                                                                                QJsonObject{
                                                                                        {QStringLiteral("type"),
                                                                                                QStringLiteral("array")},
                                                                                        {QStringLiteral("items"),
                                                                                                QJsonObject{
                                                                                                        {QStringLiteral("type"),
                                                                                                                QStringLiteral("string")},
                                                                                                        {QStringLiteral("enum"),
                                                                                                                moodEnum}}}}}}},
                                                        {QStringLiteral("required"),
                                                                QJsonArray{QStringLiteral("id"),
                                                                        QStringLiteral("moods")}}}}}}}},
                {QStringLiteral("required"), QJsonArray{QStringLiteral("tracks")}},
        };
        const QJsonObject config{
                {QStringLiteral("responseMimeType"), QStringLiteral("application/json")},
                {QStringLiteral("responseJsonSchema"), schema},
                {QStringLiteral("temperature"), 0.2},
        };
        postGemini(config, prompt, [this, offeredIds](const QJsonObject& response) {
            const QJsonObject result = parseGeminiJson(response);
            if (result.isEmpty()) {
                emit failed(kGeminiService, geminiEmptyReason(response));
                return;
            }
            QHash<TrackId, QStringList> moods;
            const QJsonArray array = result.value(QStringLiteral("tracks")).toArray();
            for (const QJsonValue& value : array) {
                const QJsonObject entry = value.toObject();
                const int id = entry.value(QStringLiteral("id")).toInt();
                if (!offeredIds.contains(QString::number(id))) {
                    continue;
                }
                QStringList trackMoods;
                const QJsonArray moodArray = entry.value(QStringLiteral("moods")).toArray();
                for (const QJsonValue& mood : moodArray) {
                    if (moodVocabulary().contains(mood.toString())) {
                        trackMoods << mood.toString();
                    }
                }
                moods.insert(TrackId(QVariant(id)), trackMoods);
            }
            emit moodTagsReady(moods);
        });
    }
}

void AiService::requestTransitionTip(const TrackCandidate& from,
        const TrackCandidate& to,
        const EventProfile& event) {
    const QString prompt = QStringLiteral(
            "You are a DJ coach. In at most 3 short sentences, explain how to mix "
            "from track A into track B at %1 with Mixxx (EQ, filter, echo, "
            "reverb, loops, key shift or tempo). Be concrete, e.g. when to swap "
            "the bass. Plain text only.\n"
            "A: %2\nB: %3")
                                   .arg(event.description,
                                           describeTrack(from, estimateEnergy(from)),
                                           describeTrack(to, estimateEnergy(to)));
    const QJsonObject config{
            {QStringLiteral("temperature"), 0.4},
            // Room for thinking tokens, they count against this limit
            {QStringLiteral("maxOutputTokens"), 1024},
    };
    postGemini(config, prompt, [this](const QJsonObject& response) {
        const QString tip = geminiResponseText(response).trimmed();
        if (tip.isEmpty()) {
            emit failed(kGeminiService, geminiEmptyReason(response));
            return;
        }
        emit transitionTipReady(tip);
    });
}

// static
QJsonObject AiService::buildJevChoiceRequest(const MixContext& context,
        const QList<Recommendation>& candidates) {
    const EventProfile& event = eventProfile(context.event);
    QJsonObject criteria;
    const int count = std::min(static_cast<int>(candidates.size()), kMaxJevOptions);
    for (int i = 0; i < count; ++i) {
        const Recommendation& candidate = candidates[i];
        criteria.insert(jevOptionKey(candidate.track.id),
                describeTrack(candidate.track, candidate.estimatedEnergy));
    }
    QJsonObject state{
            {QStringLiteral("event"), event.description},
            {QStringLiteral("set_progress_percent"), qRound(context.setProgress * 100.0)},
            {QStringLiteral("target_energy"),
                    context.targetEnergy.value_or(targetEnergy(event, context.setProgress))},
    };
    if (!context.moods.isEmpty()) {
        state.insert(QStringLiteral("wanted_moods"), context.moods.join(QStringLiteral(", ")));
    }
    if (context.current) {
        state.insert(QStringLiteral("now_playing"),
                describeTrack(*context.current, estimateEnergy(*context.current)));
    }
    const QJsonObject question{
            {QStringLiteral("type"), QStringLiteral("choice")},
            {QStringLiteral("instructions"),
                    QStringLiteral("You are a professional DJ. Which track should be "
                                   "played next? Prefer a smooth transition "
                                   "(compatible key, close BPM), the target energy "
                                   "and moods, and what fits the event and the "
                                   "audience.")},
            {QStringLiteral("criteria"), criteria},
    };
    return QJsonObject{
            {QStringLiteral("model"), QStringLiteral("jev-latest")},
            {QStringLiteral("state"), state},
            {QStringLiteral("questions"), QJsonObject{{QStringLiteral("next_track"), question}}},
    };
}

// static
TrackId AiService::parseJevChoice(const QJsonObject& response, double* pConfidence) {
    const QJsonObject answer = response.value(QStringLiteral("answers"))
                                       .toObject()
                                       .value(QStringLiteral("next_track"))
                                       .toObject();
    const QString choice = answer.value(QStringLiteral("choice")).toString();
    if (pConfidence) {
        *pConfidence = answer.value(QStringLiteral("confidence")).toDouble();
    }
    if (!choice.startsWith(QChar('t'))) {
        return TrackId();
    }
    bool ok = false;
    const int id = choice.mid(1).toInt(&ok);
    return ok ? TrackId(QVariant(id)) : TrackId();
}

void AiService::requestNextTrackChoice(const MixContext& context,
        const QList<Recommendation>& candidates) {
    if (!m_settings.jevConfigured()) {
        emit failed(kJevService,
                tr("Add a Jev API key and allow sending track information in "
                   "the AI settings."));
        return;
    }
    if (candidates.isEmpty()) {
        emit failed(kJevService, tr("There are no tracks to choose from."));
        return;
    }
    QNetworkRequest request{QUrl(m_settings.jevEndpoint())};
    request.setRawHeader("Authorization", "Bearer " + m_settings.jevKey().toUtf8());
    postJson(request,
            buildJevChoiceRequest(context, candidates),
            kJevService,
            [this](const QJsonObject& response) {
                double confidence = 0.0;
                const TrackId id = parseJevChoice(response, &confidence);
                if (!id.isValid()) {
                    emit failed(kJevService, tr("Jev returned no valid choice."));
                    return;
                }
                emit nextTrackChosen(id, confidence, kJevService);
            });
}

void AiService::requestNextTrackChoiceGemini(const MixContext& context,
        const QList<Recommendation>& candidates) {
    if (candidates.isEmpty()) {
        emit failed(kGeminiService, tr("There are no tracks to choose from."));
        return;
    }
    const EventProfile& event = eventProfile(context.event);
    QStringList lines;
    QSet<QString> offeredIds;
    const int count = std::min(static_cast<int>(candidates.size()), kMaxJevOptions);
    for (int i = 0; i < count; ++i) {
        const Recommendation& candidate = candidates[i];
        offeredIds.insert(candidate.track.id.toString());
        lines << candidate.track.id.toString() + QStringLiteral(": ") +
                        describeTrack(candidate.track, candidate.estimatedEnergy);
    }
    const QString nowPlaying = context.current
            ? describeTrack(*context.current, estimateEnergy(*context.current))
            : QStringLiteral("nothing yet, this is the first track");
    const QString moods = context.moods.isEmpty()
            ? QString()
            : QStringLiteral(" Wanted moods: ") +
                    context.moods.join(QStringLiteral(", ")) + QChar('.');
    const double energy = context.targetEnergy.value_or(
            targetEnergy(event, context.setProgress));
    const QString prompt = QStringLiteral(
            "You are a professional DJ playing %1. The set is %2% done and the "
            "target energy is %3 (0-1).%4\nNow playing: %5\n"
            "Which track should be played next? Prefer a smooth transition "
            "(compatible key on the Camelot wheel, close BPM), the target "
            "energy, and what the audience of this event wants to hear. "
            "Answer with the id of one track from this list and a short "
            "reason:\n%6")
                                   .arg(event.description,
                                           QString::number(qRound(context.setProgress * 100.0)),
                                           QString::number(energy, 'f', 2),
                                           moods,
                                           nowPlaying,
                                           lines.join(QChar('\n')));
    const QJsonObject schema{
            {QStringLiteral("type"), QStringLiteral("object")},
            {QStringLiteral("properties"),
                    QJsonObject{
                            {QStringLiteral("trackId"),
                                    QJsonObject{{QStringLiteral("type"),
                                            QStringLiteral("integer")}}},
                            {QStringLiteral("confidence"),
                                    QJsonObject{{QStringLiteral("type"),
                                            QStringLiteral("number")}}},
                            {QStringLiteral("reason"), stringSchema()},
                    }},
            {QStringLiteral("required"),
                    QJsonArray{QStringLiteral("trackId"),
                            QStringLiteral("confidence"),
                            QStringLiteral("reason")}},
    };
    const QJsonObject config{
            {QStringLiteral("responseMimeType"), QStringLiteral("application/json")},
            {QStringLiteral("responseJsonSchema"), schema},
            {QStringLiteral("temperature"), 0.3},
    };
    postGemini(config, prompt, [this, offeredIds](const QJsonObject& response) {
        const QJsonObject result = parseGeminiJson(response);
        const int id = result.value(QStringLiteral("trackId")).toInt(-1);
        if (!offeredIds.contains(QString::number(id))) {
            emit failed(kGeminiService,
                    result.isEmpty() ? geminiEmptyReason(response)
                                     : tr("Gemini picked a track that was not offered."));
            return;
        }
        const QString reason = result.value(QStringLiteral("reason")).toString().trimmed();
        if (!reason.isEmpty()) {
            emit transitionTipReady(reason);
        }
        emit nextTrackChosen(TrackId(QVariant(id)),
                std::clamp(result.value(QStringLiteral("confidence")).toDouble(), 0.0, 1.0),
                kGeminiService);
    });
}

} // namespace aidj
} // namespace mixxx

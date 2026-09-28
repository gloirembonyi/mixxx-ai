#include "library/aidj/musicsources.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QSaveFile>
#include <QUrlQuery>

#include "moc_musicsources.cpp"
#include "util/logger.h"

namespace mixxx {
namespace aidj {

namespace {

const Logger kLogger("MusicSources");

// Descriptive user agent as asked for by the Internet Archive
const QByteArray kUserAgent = QByteArrayLiteral(
        "Mixxx AI DJ music finder (https://mixxx.org)");

constexpr int kSearchTimeoutMs = 30000;
// Downloads can be large (FLAC), only abort if nothing arrives
constexpr int kDownloadTimeoutMs = 60000;
// Items of the Internet Archive whose file lists are fetched per search
constexpr int kInternetArchiveItems = 12;
// The ccMixter server answers with an empty body for more than about 20
// results, and sometimes even for 20: then it is asked again for fewer
constexpr int kCcMixterResults = 20;
constexpr int kCcMixterRetryResults = 10;

QString forceHttps(QString url) {
    if (url.startsWith(QStringLiteral("http://"))) {
        url.replace(0, 7, QStringLiteral("https://"));
    }
    return url;
}

QString jsonString(const QJsonValue& value) {
    if (value.isArray()) {
        // Internet Archive fields may be arrays of strings
        const QJsonArray array = value.toArray();
        return array.isEmpty() ? QString() : array.first().toString();
    }
    if (value.isDouble()) {
        return QString::number(value.toDouble());
    }
    return value.toString();
}

qint64 parseSize(const QJsonValue& value) {
    if (value.isDouble()) {
        return static_cast<qint64>(value.toDouble());
    }
    const QString text = value.toString().trimmed();
    bool ok = false;
    const qint64 bytes = text.toLongLong(&ok);
    if (ok) {
        return bytes;
    }
    // e.g. "7.3MB" or "512KB"
    static const QRegularExpression kSizeRegex(
            QStringLiteral("^([0-9.]+)\\s*([KMG]?)B$"),
            QRegularExpression::CaseInsensitiveOption);
    const auto match = kSizeRegex.match(text);
    if (!match.hasMatch()) {
        return 0;
    }
    double size = match.captured(1).toDouble();
    const QString unit = match.captured(2).toUpper();
    if (unit == QStringLiteral("K")) {
        size *= 1024;
    } else if (unit == QStringLiteral("M")) {
        size *= 1024 * 1024;
    } else if (unit == QStringLiteral("G")) {
        size *= 1024 * 1024 * 1024;
    }
    return static_cast<qint64>(size);
}

QString sanitizeFileName(QString name) {
    static const QRegularExpression kInvalid(QStringLiteral("[\\\\/:*?\"<>|\\x00-\\x1f]"));
    name.replace(kInvalid, QStringLiteral("_"));
    name = name.simplified();
    // Windows does not allow trailing dots and spaces
    while (name.endsWith(QChar('.')) || name.endsWith(QChar(' '))) {
        name.chop(1);
    }
    if (name.size() > 150) {
        name.truncate(150);
    }
    return name.isEmpty() ? QStringLiteral("Track") : name;
}

} // namespace

LicenseInfo classifyLicense(const QString& licenseUrlOrName) {
    LicenseInfo info;
    const QString text = licenseUrlOrName.toLower();
    if (text.isEmpty()) {
        info.name = QObject::tr("Unknown license");
        info.summary = QObject::tr("License unknown: check the source page before playing it in public.");
        return info;
    }
    if (text.contains(QStringLiteral("publicdomain")) || text.contains(QStringLiteral("cc0")) ||
            text.contains(QStringLiteral("public domain"))) {
        info.known = true;
        info.name = text.contains(QStringLiteral("zero")) || text.contains(QStringLiteral("cc0"))
                ? QStringLiteral("CC0")
                : QObject::tr("Public domain");
        info.paidGigsOk = true;
        info.publishMixesOk = true;
        info.summary = QObject::tr("Free for any use, also paid gigs and published mixes.");
        return info;
    }
    // e.g. creativecommons.org/licenses/by-nc-sa/3.0/ or "Attribution Noncommercial"
    static const QRegularExpression kCcUrl(
            QStringLiteral("licenses/([a-z-]+)/([0-9.]+)"));
    QString terms;
    QString version;
    const auto match = kCcUrl.match(text);
    if (match.hasMatch()) {
        terms = match.captured(1);
        version = match.captured(2);
    } else if (text.contains(QStringLiteral("attribution"))) {
        terms = QStringLiteral("by");
        if (text.contains(QStringLiteral("noncommercial")) ||
                text.contains(QStringLiteral("non-commercial"))) {
            terms += QStringLiteral("-nc");
        }
        if (text.contains(QStringLiteral("noderiv")) || text.contains(QStringLiteral("no deriv"))) {
            terms += QStringLiteral("-nd");
        }
        if (text.contains(QStringLiteral("sharealike")) || text.contains(QStringLiteral("share alike"))) {
            terms += QStringLiteral("-sa");
        }
    } else {
        info.name = licenseUrlOrName;
        info.summary = QObject::tr("License not recognized: check the source page before playing it in public.");
        return info;
    }
    info.known = true;
    info.name = QStringLiteral("CC ") + terms.toUpper();
    if (!version.isEmpty()) {
        info.name += QChar(' ') + version;
    }
    const bool nonCommercial = terms.contains(QStringLiteral("nc"));
    const bool noDerivatives = terms.contains(QStringLiteral("nd"));
    info.paidGigsOk = !nonCommercial;
    info.publishMixesOk = !noDerivatives && !nonCommercial;
    if (nonCommercial && noDerivatives) {
        info.summary = QObject::tr("Non-commercial and no derivatives: no paid gigs, do not publish mixes with it. Credit the artist.");
    } else if (nonCommercial) {
        info.summary = QObject::tr("Non-commercial: fine for free events, not for paid gigs. Credit the artist.");
    } else if (noDerivatives) {
        info.summary = QObject::tr("OK for paid gigs, but do not publish recorded mixes with it. Credit the artist.");
    } else {
        info.summary = QObject::tr("OK for paid gigs and published mixes. Credit the artist.");
    }
    return info;
}

QString musicSourceName(MusicSource source) {
    switch (source) {
    case MusicSource::CcMixter:
        return QStringLiteral("ccMixter");
    case MusicSource::All:
        return QObject::tr("All sources");
    case MusicSource::InternetArchive:
    default:
        return QObject::tr("Internet Archive (netlabels)");
    }
}

double parseDurationSeconds(const QString& text) {
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        return 0.0;
    }
    if (!trimmed.contains(QChar(':'))) {
        return trimmed.toDouble();
    }
    double seconds = 0.0;
    const QStringList parts = trimmed.split(QChar(':'));
    for (const QString& part : parts) {
        seconds = seconds * 60.0 + part.toDouble();
    }
    return seconds;
}

QList<MusicSearchResult> parseCcMixterResults(const QByteArray& json) {
    QList<MusicSearchResult> results;
    const QJsonArray uploads = QJsonDocument::fromJson(json).array();
    for (const QJsonValue& uploadValue : uploads) {
        const QJsonObject upload = uploadValue.toObject();
        const QJsonArray files = upload.value(QStringLiteral("files")).toArray();
        // Pick the first audio file (usually the MP3)
        for (const QJsonValue& fileValue : files) {
            const QJsonObject file = fileValue.toObject();
            const QJsonObject formatInfo = file.value(QStringLiteral("file_format_info")).toObject();
            const QString mime = formatInfo.value(QStringLiteral("mime_type")).toString();
            const QString url = file.value(QStringLiteral("download_url")).toString();
            if (!mime.startsWith(QStringLiteral("audio/")) || url.isEmpty()) {
                continue;
            }
            MusicSearchResult result;
            result.source = MusicSource::CcMixter;
            result.title = upload.value(QStringLiteral("upload_name")).toString();
            result.artist = upload.value(QStringLiteral("user_real_name")).toString();
            if (result.artist.isEmpty()) {
                result.artist = upload.value(QStringLiteral("user_name")).toString();
            }
            result.pageUrl = QUrl(forceHttps(upload.value(QStringLiteral("file_page_url")).toString()));
            result.licenseUrl = upload.value(QStringLiteral("license_url")).toString();
            result.license = classifyLicense(result.licenseUrl.isEmpty()
                            ? upload.value(QStringLiteral("license_name")).toString()
                            : result.licenseUrl);
            result.bpm = jsonString(upload.value(QStringLiteral("upload_extra"))
                                            .toObject()
                                            .value(QStringLiteral("bpm")))
                                 .toDouble();
            result.durationSeconds = parseDurationSeconds(
                    formatInfo.value(QStringLiteral("ps")).toString());
            result.sizeBytes = parseSize(file.value(QStringLiteral("file_rawsize")));
            if (result.sizeBytes == 0) {
                result.sizeBytes = parseSize(file.value(QStringLiteral("file_filesize")));
            }
            result.downloadUrl = QUrl(forceHttps(url));
            result.fileExtension = QFileInfo(result.downloadUrl.path()).suffix().toLower();
            if (result.fileExtension.isEmpty()) {
                result.fileExtension = QStringLiteral("mp3");
            }
            result.format = result.fileExtension.toUpper();
            results.append(result);
            break;
        }
    }
    return results;
}

QStringList parseInternetArchiveSearch(const QByteArray& json) {
    QStringList identifiers;
    const QJsonArray docs = QJsonDocument::fromJson(json)
                                    .object()
                                    .value(QStringLiteral("response"))
                                    .toObject()
                                    .value(QStringLiteral("docs"))
                                    .toArray();
    for (const QJsonValue& doc : docs) {
        const QString identifier = doc.toObject().value(QStringLiteral("identifier")).toString();
        if (!identifier.isEmpty()) {
            identifiers << identifier;
        }
    }
    return identifiers;
}

QList<MusicSearchResult> parseInternetArchiveItem(const QByteArray& json, bool preferLossless) {
    QList<MusicSearchResult> results;
    const QJsonObject root = QJsonDocument::fromJson(json).object();
    const QJsonObject metadata = root.value(QStringLiteral("metadata")).toObject();
    const QString identifier = jsonString(metadata.value(QStringLiteral("identifier")));
    if (identifier.isEmpty()) {
        return results;
    }
    const QString itemArtist = jsonString(metadata.value(QStringLiteral("creator")));
    const QString album = jsonString(metadata.value(QStringLiteral("title")));
    const QString licenseUrl = jsonString(metadata.value(QStringLiteral("licenseurl")));
    const LicenseInfo license = classifyLicense(licenseUrl);
    const QUrl pageUrl(QStringLiteral("https://archive.org/details/") + identifier);

    // Group the files of a track by base name: "01 Track.flac", "01 Track.mp3"
    struct Candidate {
        QJsonObject file;
        QString format;
        QString extension;
        int preference;
    };
    QMap<QString, Candidate> bestByTrack;
    QStringList trackOrder;
    const QJsonArray files = root.value(QStringLiteral("files")).toArray();
    for (const QJsonValue& fileValue : files) {
        const QJsonObject file = fileValue.toObject();
        const QString format = file.value(QStringLiteral("format")).toString();
        const QString name = file.value(QStringLiteral("name")).toString();
        QString extension;
        QString displayFormat;
        int preference = 0;
        if (format == QStringLiteral("Flac")) {
            extension = QStringLiteral("flac");
            displayFormat = QStringLiteral("FLAC");
            preference = preferLossless ? 3 : 1;
        } else if (format == QStringLiteral("VBR MP3") || format.endsWith(QStringLiteral("Kbps MP3"))) {
            extension = QStringLiteral("mp3");
            displayFormat = QStringLiteral("MP3");
            preference = preferLossless ? 2 : 3;
        } else if (format == QStringLiteral("Ogg Vorbis")) {
            extension = QStringLiteral("ogg");
            displayFormat = QStringLiteral("OGG");
            preference = preferLossless ? 1 : 2;
        } else {
            continue;
        }
        const QString baseName = QFileInfo(name).completeBaseName();
        const auto existing = bestByTrack.constFind(baseName);
        if (existing == bestByTrack.constEnd()) {
            trackOrder << baseName;
            bestByTrack.insert(baseName, Candidate{file, displayFormat, extension, preference});
        } else if (preference > existing->preference) {
            bestByTrack.insert(baseName, Candidate{file, displayFormat, extension, preference});
        }
    }
    for (const QString& baseName : std::as_const(trackOrder)) {
        const Candidate& candidate = bestByTrack[baseName];
        const QJsonObject& file = candidate.file;
        MusicSearchResult result;
        result.source = MusicSource::InternetArchive;
        result.title = file.value(QStringLiteral("title")).toString();
        if (result.title.isEmpty()) {
            result.title = baseName;
        }
        result.artist = file.value(QStringLiteral("creator")).toString();
        if (result.artist.isEmpty()) {
            result.artist = file.value(QStringLiteral("artist")).toString();
        }
        if (result.artist.isEmpty()) {
            result.artist = itemArtist;
        }
        result.album = album;
        result.durationSeconds = parseDurationSeconds(jsonString(file.value(QStringLiteral("length"))));
        result.sizeBytes = parseSize(file.value(QStringLiteral("size")));
        result.format = candidate.format;
        result.fileExtension = candidate.extension;
        result.pageUrl = pageUrl;
        result.licenseUrl = licenseUrl;
        result.license = license;
        QUrl url(QStringLiteral("https://archive.org/download/") + identifier + QChar('/'));
        url.setPath(url.path() + file.value(QStringLiteral("name")).toString());
        result.downloadUrl = url;
        results.append(result);
    }
    return results;
}

MusicSearch::MusicSearch(QObject* pParent)
        : QObject(pParent),
          m_generation(0),
          m_pendingSources(0),
          m_pendingItems(0) {
    m_network.setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
    m_network.setTransferTimeout(kSearchTimeoutMs);
}

QNetworkReply* MusicSearch::get(const QUrl& url) {
    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", kUserAgent);
    return m_network.get(request);
}

void MusicSearch::cancel() {
    ++m_generation;
    m_pendingResults.clear();
    m_errors.clear();
    m_pendingSources = 0;
    m_archiveResults.clear();
    m_pendingItems = 0;
}

void MusicSearch::sourceDone(int generation,
        const QList<MusicSearchResult>& results,
        const QString& error) {
    if (generation != m_generation) {
        return;
    }
    m_pendingResults.append(results);
    if (!error.isEmpty()) {
        m_errors << error;
    }
    if (--m_pendingSources > 0) {
        return;
    }
    const QList<MusicSearchResult> allResults = m_pendingResults;
    const QStringList errors = m_errors;
    m_pendingResults.clear();
    m_errors.clear();
    if (allResults.isEmpty() && !errors.isEmpty()) {
        emit failed(errors.join(QChar(' ')));
        return;
    }
    // A source that failed while others worked is only logged
    for (const QString& message : errors) {
        kLogger.warning() << message;
    }
    emit resultsReady(allResults);
}

void MusicSearch::search(const QString& query,
        MusicSource source,
        bool preferLossless,
        bool paidGigsOnly) {
    cancel();
    if (query.trimmed().isEmpty()) {
        emit resultsReady({});
        return;
    }
    switch (source) {
    case MusicSource::CcMixter:
        m_pendingSources = 1;
        searchCcMixter(query.trimmed(), kCcMixterResults);
        break;
    case MusicSource::All:
        m_pendingSources = 2;
        searchInternetArchive(query.trimmed(), preferLossless, paidGigsOnly);
        searchCcMixter(query.trimmed(), kCcMixterResults);
        break;
    case MusicSource::InternetArchive:
    default:
        m_pendingSources = 1;
        searchInternetArchive(query.trimmed(), preferLossless, paidGigsOnly);
        break;
    }
}

void MusicSearch::searchCcMixter(const QString& query, int limit) {
    QUrl url(QStringLiteral("https://ccmixter.org/api/query"));
    QUrlQuery params;
    params.addQueryItem(QStringLiteral("f"), QStringLiteral("json"));
    params.addQueryItem(QStringLiteral("search"), query);
    params.addQueryItem(QStringLiteral("search_type"), QStringLiteral("all"));
    params.addQueryItem(QStringLiteral("sort"), QStringLiteral("rank"));
    params.addQueryItem(QStringLiteral("limit"), QString::number(limit));
    url.setQuery(params);
    const int generation = m_generation;
    QNetworkReply* pReply = get(url);
    connect(pReply, &QNetworkReply::finished, this, [this, pReply, generation, query, limit]() {
        pReply->deleteLater();
        if (generation != m_generation) {
            return;
        }
        if (pReply->error() != QNetworkReply::NoError) {
            sourceDone(generation,
                    {},
                    tr("ccMixter search failed: %1").arg(pReply->errorString()));
            return;
        }
        const QByteArray data = pReply->readAll();
        if (data.trimmed().isEmpty() && limit > kCcMixterRetryResults) {
            kLogger.info() << "ccMixter returned nothing, asking for fewer results";
            searchCcMixter(query, kCcMixterRetryResults);
            return;
        }
        sourceDone(generation, parseCcMixterResults(data), QString());
    });
}

// static
QString MusicSearch::internetArchiveQuery(const QString& text, bool paidGigsOnly) {
    // Quotes and brackets in the user's text would break the query
    QString words = text;
    words.remove(QRegularExpression(QStringLiteral("[\"()]")));
    QString query = QStringLiteral("collection:netlabels AND mediatype:audio AND (%1)")
                            .arg(words.simplified());
    if (paidGigsOnly) {
        // Most netlabel releases are non-commercial (NC), without this
        // filter the few that allow paid gigs are rarely among the results
        query += QStringLiteral(
                " AND licenseurl:*creativecommons* AND NOT licenseurl:*nc*");
    }
    return query;
}

void MusicSearch::searchInternetArchive(
        const QString& query, bool preferLossless, bool paidGigsOnly) {
    QUrl url(QStringLiteral("https://archive.org/advancedsearch.php"));
    QUrlQuery params;
    params.addQueryItem(QStringLiteral("q"), internetArchiveQuery(query, paidGigsOnly));
    params.addQueryItem(QStringLiteral("fl[]"), QStringLiteral("identifier"));
    params.addQueryItem(QStringLiteral("sort[]"), QStringLiteral("downloads desc"));
    params.addQueryItem(QStringLiteral("rows"), QString::number(kInternetArchiveItems));
    params.addQueryItem(QStringLiteral("page"), QStringLiteral("1"));
    params.addQueryItem(QStringLiteral("output"), QStringLiteral("json"));
    url.setQuery(params);
    const int generation = m_generation;
    QNetworkReply* pReply = get(url);
    connect(pReply, &QNetworkReply::finished, this, [this, pReply, generation, preferLossless]() {
        pReply->deleteLater();
        if (generation != m_generation) {
            return;
        }
        if (pReply->error() != QNetworkReply::NoError) {
            sourceDone(generation,
                    {},
                    tr("Internet Archive search failed: %1").arg(pReply->errorString()));
            return;
        }
        const QStringList identifiers = parseInternetArchiveSearch(pReply->readAll());
        if (identifiers.isEmpty()) {
            sourceDone(generation, {}, QString());
            return;
        }
        // Fetch the file lists of all items in parallel
        m_pendingItems = static_cast<int>(identifiers.size());
        for (const QString& identifier : identifiers) {
            QNetworkReply* pItemReply = get(QUrl(
                    QStringLiteral("https://archive.org/metadata/") + identifier));
            connect(pItemReply,
                    &QNetworkReply::finished,
                    this,
                    [this, pItemReply, generation, preferLossless]() {
                        pItemReply->deleteLater();
                        if (generation != m_generation) {
                            return;
                        }
                        if (pItemReply->error() == QNetworkReply::NoError) {
                            m_archiveResults.append(parseInternetArchiveItem(
                                    pItemReply->readAll(), preferLossless));
                        } else {
                            kLogger.warning() << "Item request failed" << pItemReply->url()
                                              << pItemReply->errorString();
                        }
                        if (--m_pendingItems == 0) {
                            const QList<MusicSearchResult> results = m_archiveResults;
                            m_archiveResults.clear();
                            sourceDone(generation, results, QString());
                        }
                    });
        }
    });
}

MusicDownloader::MusicDownloader(QObject* pParent)
        : QObject(pParent),
          m_pFile(nullptr) {
    m_network.setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
    m_network.setTransferTimeout(kDownloadTimeoutMs);
}

// static
QString MusicDownloader::fileNameFor(const MusicSearchResult& result) {
    QString name = result.title;
    if (!result.artist.isEmpty()) {
        name = result.artist + QStringLiteral(" - ") + result.title;
    }
    return sanitizeFileName(name) + QChar('.') +
            (result.fileExtension.isEmpty() ? QStringLiteral("mp3") : result.fileExtension);
}

bool MusicDownloader::enqueue(
        const MusicSearchResult& result, const QString& targetDirectory, int tag) {
    if (m_pCurrentReply && m_currentJob.result.downloadUrl == result.downloadUrl) {
        return false;
    }
    for (const Job& job : std::as_const(m_queue)) {
        if (job.result.downloadUrl == result.downloadUrl) {
            return false;
        }
    }
    m_queue.append(Job{result, targetDirectory, tag});
    if (!m_pCurrentReply) {
        startNext();
    }
    return true;
}

void MusicDownloader::cancelAll() {
    m_queue.clear();
    if (m_pCurrentReply) {
        m_pCurrentReply->abort();
    }
}

void MusicDownloader::startNext() {
    if (m_queue.isEmpty()) {
        return;
    }
    m_currentJob = m_queue.takeFirst();
    const Job job = m_currentJob;

    QDir directory(job.targetDirectory);
    if (!directory.mkpath(QStringLiteral("."))) {
        emit failed(job.tag, tr("Cannot create the folder %1").arg(job.targetDirectory));
        startNext();
        return;
    }
    const QString filePath = directory.filePath(fileNameFor(job.result));
    if (QFileInfo::exists(filePath)) {
        // Already downloaded before
        emit finished(job.tag, filePath, job.result);
        startNext();
        return;
    }
    // QSaveFile only replaces the target when the download is complete
    m_pFile = new QSaveFile(filePath, this);
    if (!m_pFile->open(QIODevice::WriteOnly)) {
        emit failed(job.tag, tr("Cannot write %1").arg(filePath));
        delete m_pFile;
        m_pFile = nullptr;
        startNext();
        return;
    }
    QNetworkRequest request(job.result.downloadUrl);
    request.setRawHeader("User-Agent", kUserAgent);
    m_pCurrentReply = m_network.get(request);
    QNetworkReply* pReply = m_pCurrentReply;
    connect(pReply, &QNetworkReply::readyRead, this, [this, pReply]() {
        if (m_pFile) {
            m_pFile->write(pReply->readAll());
        }
    });
    connect(pReply,
            &QNetworkReply::downloadProgress,
            this,
            [this, tag = job.tag](qint64 received, qint64 total) {
                emit progress(tag, received, total);
            });
    connect(pReply, &QNetworkReply::finished, this, [this, pReply, job, filePath]() {
        pReply->deleteLater();
        m_pCurrentReply.clear();
        QSaveFile* pFile = m_pFile;
        m_pFile = nullptr;
        if (pReply->error() == QNetworkReply::NoError && pFile) {
            pFile->write(pReply->readAll());
            if (pFile->commit()) {
                emit finished(job.tag, filePath, job.result);
            } else {
                emit failed(job.tag, tr("Cannot save %1").arg(filePath));
            }
        } else {
            if (pFile) {
                pFile->cancelWriting();
            }
            emit failed(job.tag,
                    tr("Download of \"%1\" failed: %2")
                            .arg(job.result.title, pReply->errorString()));
        }
        delete pFile;
        startNext();
    });
}

} // namespace aidj
} // namespace mixxx

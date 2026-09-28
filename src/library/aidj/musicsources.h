#pragma once

#include <QJsonObject>
#include <QList>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>

class QSaveFile;

namespace mixxx {
namespace aidj {

/// What a Creative Commons (or public domain) license allows a DJ
struct LicenseInfo {
    QString name;       // e.g. "CC BY-SA 4.0"
    bool known = false; // false if the license could not be recognized
    bool paidGigsOk = false;
    bool publishMixesOk = false;
    QString summary; // one line for the DJ
};

/// Recognizes a license from its URL (creativecommons.org/...) or name
LicenseInfo classifyLicense(const QString& licenseUrlOrName);

enum class MusicSource {
    InternetArchive,
    CcMixter,
    /// Searches all sources at once
    All,
};

QString musicSourceName(MusicSource source);

/// One downloadable track
struct MusicSearchResult {
    MusicSource source = MusicSource::InternetArchive;
    QString title;
    QString artist;
    QString album;
    double durationSeconds = 0.0;
    double bpm = 0.0;
    QString format; // e.g. "FLAC", "MP3"
    qint64 sizeBytes = 0;
    QUrl downloadUrl;
    QUrl pageUrl; // for attribution
    QString licenseUrl;
    LicenseInfo license;
    QString fileExtension; // without dot, e.g. "flac"
};

/// Parsers for the API responses (public for tests)
QList<MusicSearchResult> parseCcMixterResults(const QByteArray& json);
QStringList parseInternetArchiveSearch(const QByteArray& json);
QList<MusicSearchResult> parseInternetArchiveItem(
        const QByteArray& json, bool preferLossless);
/// Parses "123.4" or "mm:ss" or "h:mm:ss"
double parseDurationSeconds(const QString& text);

/// Searches legal, free music sources: the netlabels collection of the
/// Internet Archive and ccMixter. Both allow downloading the files.
class MusicSearch : public QObject {
    Q_OBJECT
  public:
    explicit MusicSearch(QObject* pParent = nullptr);

    /// paidGigsOnly: only ask for licenses that allow commercial use (where
    /// the source can filter, the others are filtered by the caller)
    void search(const QString& query,
            MusicSource source,
            bool preferLossless,
            bool paidGigsOnly = false);

    /// Search query of the Internet Archive (public for tests)
    static QString internetArchiveQuery(const QString& text, bool paidGigsOnly);
    void cancel();

  signals:
    void resultsReady(const QList<MusicSearchResult>& results);
    void failed(const QString& message);

  private:
    void searchCcMixter(const QString& query, int limit);
    void searchInternetArchive(const QString& query, bool preferLossless, bool paidGigsOnly);
    /// A source finished (error is empty on success)
    void sourceDone(int generation,
            const QList<MusicSearchResult>& results,
            const QString& error);
    QNetworkReply* get(const QUrl& url);

    QNetworkAccessManager m_network;
    // Incremented on every search, late replies of old searches are ignored
    int m_generation;
    // Results of all sources of the running search
    QList<MusicSearchResult> m_pendingResults;
    QStringList m_errors;
    int m_pendingSources;
    // Internet Archive items whose file lists are still loading
    QList<MusicSearchResult> m_archiveResults;
    int m_pendingItems;
};

/// Downloads tracks one after another into a folder, in the background.
class MusicDownloader : public QObject {
    Q_OBJECT
  public:
    explicit MusicDownloader(QObject* pParent = nullptr);

    /// Queues a download, returns false if it is already queued
    bool enqueue(const MusicSearchResult& result, const QString& targetDirectory, int tag);
    int pendingCount() const {
        return static_cast<int>(m_queue.size()) + (m_pCurrentReply ? 1 : 0);
    }
    void cancelAll();

    /// File name for a result, e.g. "Artist - Title.flac"
    static QString fileNameFor(const MusicSearchResult& result);

  signals:
    void progress(int tag, qint64 received, qint64 total);
    void finished(int tag, const QString& filePath, const MusicSearchResult& result);
    void failed(int tag, const QString& message);

  private:
    struct Job {
        MusicSearchResult result;
        QString targetDirectory;
        int tag;
    };
    void startNext();

    QNetworkAccessManager m_network;
    QList<Job> m_queue;
    Job m_currentJob;
    QPointer<QNetworkReply> m_pCurrentReply;
    QSaveFile* m_pFile;
};

} // namespace aidj
} // namespace mixxx

Q_DECLARE_METATYPE(mixxx::aidj::MusicSearchResult)

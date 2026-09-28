#include "library/aidj/aidjlibrary.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include "library/dao/trackschema.h"
#include "track/keyutils.h"
#include "util/db/dbconnection.h"
#include "util/logger.h"
#include "util/math.h"

namespace mixxx {
namespace aidj {

namespace {

const Logger kLogger("AiDjLibrary");

// Reference loudness of Mixxx' ReplayGain 2.0 analysis
constexpr double kReplayGainReferenceLufs = -18.0;

} // namespace

const QStringList& moodVocabulary() {
    static const QStringList kMoods = {
            QStringLiteral("happy"),
            QStringLiteral("romantic"),
            QStringLiteral("energetic"),
            QStringLiteral("party"),
            QStringLiteral("chill"),
            QStringLiteral("dark"),
            QStringLiteral("sad"),
            QStringLiteral("uplifting"),
            QStringLiteral("spiritual"),
            QStringLiteral("elegant"),
            QStringLiteral("aggressive"),
            QStringLiteral("nostalgic"),
    };
    return kMoods;
}

AiDjLibrary::AiDjLibrary(QSqlDatabase database)
        : m_database(std::move(database)) {
}

// static
std::optional<double> AiDjLibrary::loudnessFromReplayGainRatio(double ratio) {
    if (ratio <= 0.0) {
        return std::nullopt;
    }
    return kReplayGainReferenceLufs - ratio2db(ratio);
}

QList<TrackCandidate> AiDjLibrary::loadCandidates() const {
    QList<TrackCandidate> candidates;
    QSqlQuery query(m_database);
    query.setForwardOnly(true);
    const QString library = QStringLiteral(LIBRARY_TABLE ".");
    const QString locations = QStringLiteral(TRACKLOCATIONS_TABLE ".");
    // The order must match the query.value() indices below
    const QStringList columns = {
            library + LIBRARYTABLE_ID,
            library + LIBRARYTABLE_ARTIST,
            library + LIBRARYTABLE_TITLE,
            library + LIBRARYTABLE_GENRE,
            library + LIBRARYTABLE_BPM,
            library + LIBRARYTABLE_KEY_ID,
            library + LIBRARYTABLE_DURATION,
            library + LIBRARYTABLE_RATING,
            library + LIBRARYTABLE_TIMESPLAYED,
            library + LIBRARYTABLE_LAST_PLAYED_AT,
            library + LIBRARYTABLE_REPLAYGAIN,
            locations + TRACKLOCATIONSTABLE_LOCATION,
            library + LIBRARYTABLE_PLAYED,
    };
    const QString sql = QStringLiteral(
            "SELECT %1 FROM " LIBRARY_TABLE " INNER JOIN " TRACKLOCATIONS_TABLE
            " ON %2 = %3 WHERE %4 = 0 AND %5 = 0")
                                .arg(columns.join(QStringLiteral(", ")),
                                        library + LIBRARYTABLE_LOCATION,
                                        locations + TRACKLOCATIONSTABLE_ID,
                                        library + LIBRARYTABLE_MIXXXDELETED,
                                        locations + TRACKLOCATIONSTABLE_FSDELETED);
    if (!query.exec(sql)) {
        kLogger.warning() << "Failed to read the library:" << query.lastError();
        return candidates;
    }
    while (query.next()) {
        TrackCandidate track;
        track.id = TrackId(query.value(0));
        track.artist = query.value(1).toString();
        track.title = query.value(2).toString();
        track.genre = query.value(3).toString();
        track.bpm = query.value(4).toDouble();
        track.key = KeyUtils::keyFromNumericValue(query.value(5).toInt());
        track.durationSeconds = query.value(6).toDouble();
        track.rating = query.value(7).toInt();
        track.timesPlayed = query.value(8).toInt();
        const QVariant lastPlayed = query.value(9);
        if (!lastPlayed.isNull()) {
            track.lastPlayedAt = lastPlayed.toDateTime();
        }
        track.loudnessLufs = loudnessFromReplayGainRatio(query.value(10).toDouble());
        track.location = query.value(11).toString();
        // The "played" flag is reset when Mixxx starts
        track.playedThisSession = query.value(12).toBool();
        candidates.append(std::move(track));
    }
    return candidates;
}

MoodStore::MoodStore(const QString& filePath)
        : m_filePath(filePath) {
}

// static
QString MoodStore::defaultFilePath(const UserSettingsPointer& pConfig) {
    return QDir(pConfig->getSettingsPath()).filePath(QStringLiteral("aidj_moods.json"));
}

QStringList MoodStore::moods(TrackId id) const {
    return m_moods.value(id.toString());
}

bool MoodStore::hasMoods(TrackId id) const {
    return m_moods.contains(id.toString());
}

void MoodStore::setMoods(TrackId id, const QStringList& moods) {
    if (!id.isValid()) {
        return;
    }
    m_moods.insert(id.toString(), moods);
}

void MoodStore::apply(QList<TrackCandidate>* pCandidates) const {
    for (TrackCandidate& track : *pCandidates) {
        track.moods = moods(track.id);
    }
}

bool MoodStore::load() {
    QFile file(m_filePath);
    if (!file.exists()) {
        return true;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        kLogger.warning() << "Failed to open" << m_filePath;
        return false;
    }
    const QJsonObject object = QJsonDocument::fromJson(file.readAll()).object();
    m_moods.clear();
    for (auto it = object.begin(); it != object.end(); ++it) {
        QStringList moods;
        const QJsonArray array = it.value().toArray();
        for (const QJsonValue& value : array) {
            moods << value.toString();
        }
        m_moods.insert(it.key(), moods);
    }
    return true;
}

bool MoodStore::save() const {
    QJsonObject object;
    for (auto it = m_moods.begin(); it != m_moods.end(); ++it) {
        object.insert(it.key(), QJsonArray::fromStringList(it.value()));
    }
    QSaveFile file(m_filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        kLogger.warning() << "Failed to write" << m_filePath;
        return false;
    }
    file.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
    return file.commit();
}

} // namespace aidj
} // namespace mixxx

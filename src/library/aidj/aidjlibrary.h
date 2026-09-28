#pragma once

#include <QHash>
#include <QList>
#include <QSqlDatabase>
#include <QString>
#include <QStringList>

#include "library/aidj/djrecommender.h"
#include "preferences/usersettings.h"
#include "track/trackid.h"

namespace mixxx {
namespace aidj {

/// Reads the tracks of the Mixxx library for the recommender. Must be used on
/// the thread of the database connection (the main thread).
class AiDjLibrary {
  public:
    explicit AiDjLibrary(QSqlDatabase database);

    /// All tracks that are not deleted and whose file exists
    QList<TrackCandidate> loadCandidates() const;

    /// Converts the stored ReplayGain ratio to integrated loudness (LUFS).
    /// Mixxx normalizes to -18 LUFS, a ratio of 0 means "not analyzed".
    static std::optional<double> loudnessFromReplayGainRatio(double ratio);

  private:
    QSqlDatabase m_database;
};

/// Mood tags per track, e.g. set by the AI mood tagging. They are kept in a
/// JSON file in the settings directory so the library schema is unchanged.
class MoodStore {
  public:
    explicit MoodStore(const QString& filePath);

    static QString defaultFilePath(const UserSettingsPointer& pConfig);

    QStringList moods(TrackId id) const;
    void setMoods(TrackId id, const QStringList& moods);
    bool hasMoods(TrackId id) const;
    int size() const {
        return static_cast<int>(m_moods.size());
    }

    /// Adds the stored moods to the candidates
    void apply(QList<TrackCandidate>* pCandidates) const;

    bool load();
    bool save() const;

  private:
    QString m_filePath;
    QHash<QString, QStringList> m_moods;
};

/// The moods the AI may choose from
const QStringList& moodVocabulary();

} // namespace aidj
} // namespace mixxx

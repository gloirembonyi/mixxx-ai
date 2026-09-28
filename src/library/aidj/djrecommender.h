#pragma once

#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>
#include <optional>

#include "proto/keys.pb.h"
#include "track/trackid.h"

namespace mixxx {
namespace aidj {

/// Kind of event the DJ is playing. It sets the energy curve of the set and
/// which genres fit.
enum class EventType : int {
    Club = 0,
    WeddingCeremony,
    CocktailHour,
    Dinner,
    WeddingReception,
    Birthday,
    Corporate,
    ChurchService,
    Memorial,
    Lounge,
    WarmUp,
    PeakTime,
    Count,
};

/// Energy ranges from 0 (calm) to 1 (peak)
struct EventProfile {
    EventType type;
    QString name;
    /// Target energy at the start, middle and end of the set
    double startEnergy;
    double middleEnergy;
    double endEnergy;
    /// Preferred BPM range, 0 = any
    double minBpm;
    double maxBpm;
    /// Genre words that fit (lower case, matched as substrings)
    QStringList preferredGenres;
    /// Genre words that do not fit
    QStringList avoidedGenres;
    /// Text for the AI prompts
    QString description;
};

/// Everything the recommender knows about a track of the library
struct TrackCandidate {
    TrackId id;
    QString artist;
    QString title;
    QString genre;
    QString location;
    double bpm = 0.0;
    track::io::key::ChromaticKey key = track::io::key::INVALID;
    double durationSeconds = 0.0;
    int rating = 0; // 0..5
    int timesPlayed = 0;
    QDateTime lastPlayedAt;
    /// Integrated loudness in LUFS, if known from the ReplayGain analysis
    std::optional<double> loudnessLufs;
    /// Mood tags, e.g. from the AI mood tagging ("romantic", "happy", ...)
    QStringList moods;
    /// Played in the current session (history) or loaded in a deck
    bool playedThisSession = false;
};

/// Score of a candidate with the reasons, all parts range from 0 to 1
struct Recommendation {
    TrackCandidate track;
    double score = 0.0;
    double keyScore = 0.0;
    double bpmScore = 0.0;
    double energyScore = 0.0;
    double genreScore = 0.0;
    double moodScore = 0.0;
    double estimatedEnergy = 0.0;
    /// Human readable reasons, e.g. "Key 8A -> 9A (harmonic)"
    QStringList reasons;
};

/// What the next track should fit
struct MixContext {
    EventType event = EventType::Club;
    /// Track that is playing now, if any
    std::optional<TrackCandidate> current;
    /// 0 = start of the set, 1 = end of the set
    double setProgress = 0.0;
    /// Overrides the energy of the event curve if set
    std::optional<double> targetEnergy;
    /// Wanted moods, e.g. {"romantic"}
    QStringList moods;
    /// Allowed BPM deviation for a smooth tempo change
    double maxBpmDeviation = 0.08;
    /// Artists played recently (lower case), repeating them is penalized
    QStringList recentArtists;
    /// Skip tracks whose key clashes with the playing track
    bool harmonicOnly = false;
};

const QList<EventProfile>& eventProfiles();
const EventProfile& eventProfile(EventType type);

/// Estimates the energy (0..1) from BPM, loudness and genre. This works
/// offline for every analyzed track.
double estimateEnergy(const TrackCandidate& track);

/// Target energy of the event curve at a point of the set
double targetEnergy(const EventProfile& profile, double setProgress);

/// Key compatibility for harmonic mixing, 1 = same or compatible key
double keyCompatibility(track::io::key::ChromaticKey from, track::io::key::ChromaticKey to);

/// Tempo compatibility, allows half and double tempo, 1 = same tempo
double bpmCompatibility(double fromBpm, double toBpm, double maxDeviation);

/// Scores one candidate
Recommendation scoreCandidate(const TrackCandidate& candidate, const MixContext& context);

/// Scores and sorts all candidates, best first. Tracks played in this
/// session and the current track are skipped.
QList<Recommendation> recommend(const QList<TrackCandidate>& candidates,
        const MixContext& context,
        int maxResults);

/// Builds a playlist by chaining the best next track, starting from the
/// context's current track. The energy follows the event curve over the
/// given duration. With maxTracks >= 0 the set ends after that many tracks
/// and the energy curve is spread over them.
QList<Recommendation> buildSet(const QList<TrackCandidate>& candidates,
        MixContext context,
        double durationMinutes,
        int maxTracks = -1);

/// Lower case artist name for comparisons, empty if unknown
QString normalizedArtist(const QString& artist);

/// Offline advice how to mix from one track into the next, based on key,
/// tempo and energy (no AI service needed)
QString transitionTip(const TrackCandidate& from, const TrackCandidate& to);

/// Camelot/Lancelot notation of a key, e.g. "8A", or "?" if unknown
QString keyName(track::io::key::ChromaticKey key);

} // namespace aidj
} // namespace mixxx

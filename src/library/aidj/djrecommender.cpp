#include "library/aidj/djrecommender.h"

#include <QCoreApplication>
#include <QHash>
#include <QSet>
#include <algorithm>
#include <cmath>

#include "track/keyutils.h"

using mixxx::track::io::key::ChromaticKey;

namespace mixxx {
namespace aidj {

namespace {

// Weights of the score parts, they add up to 1
constexpr double kKeyWeight = 0.25;
constexpr double kBpmWeight = 0.25;
constexpr double kEnergyWeight = 0.25;
constexpr double kGenreWeight = 0.10;
constexpr double kMoodWeight = 0.05;
constexpr double kRatingWeight = 0.05;
constexpr double kFreshnessWeight = 0.05;

// Unknown values are neither good nor bad
constexpr double kUnknownScore = 0.5;

// Score factors for artist repeats
constexpr double kSameArtistFactor = 0.6;
constexpr double kRecentArtistFactor = 0.85;
// Artists of the last tracks of a built set that count as recent
constexpr int kRecentArtistCount = 3;

// Loudness range of typical music masters (LUFS)
constexpr double kQuietLufs = -20.0;
constexpr double kLoudLufs = -6.0;
// Tempo range from calm to peak energy
constexpr double kCalmBpm = 70.0;
constexpr double kPeakBpm = 140.0;

const QStringList kHighEnergyGenres = {
        QStringLiteral("techno"),
        QStringLiteral("house"),
        QStringLiteral("edm"),
        QStringLiteral("electro"),
        QStringLiteral("dance"),
        QStringLiteral("trance"),
        QStringLiteral("drum"),
        QStringLiteral("dubstep"),
        QStringLiteral("hardstyle"),
        QStringLiteral("trap"),
        QStringLiteral("hip hop"),
        QStringLiteral("hip-hop"),
        QStringLiteral("rap"),
        QStringLiteral("reggaeton"),
        QStringLiteral("dancehall"),
        QStringLiteral("afrobeat"),
        QStringLiteral("amapiano"),
        QStringLiteral("gqom"),
        QStringLiteral("rock"),
        QStringLiteral("metal"),
        QStringLiteral("punk"),
        QStringLiteral("pop"),
        QStringLiteral("disco"),
        QStringLiteral("funk"),
        QStringLiteral("salsa"),
        QStringLiteral("soca"),
};

const QStringList kLowEnergyGenres = {
        QStringLiteral("ambient"),
        QStringLiteral("classical"),
        QStringLiteral("piano"),
        QStringLiteral("acoustic"),
        QStringLiteral("jazz"),
        QStringLiteral("lounge"),
        QStringLiteral("chill"),
        QStringLiteral("ballad"),
        QStringLiteral("gospel"),
        QStringLiteral("worship"),
        QStringLiteral("soul"),
        QStringLiteral("folk"),
        QStringLiteral("instrumental"),
        QStringLiteral("orchestral"),
        QStringLiteral("soundtrack"),
        QStringLiteral("r&b"),
        QStringLiteral("rnb"),
        QStringLiteral("bossa"),
        QStringLiteral("downtempo"),
};

bool genreMatchesAny(const QString& genre, const QStringList& words) {
    const QString lowerGenre = genre.toLower();
    for (const QString& word : words) {
        if (lowerGenre.contains(word)) {
            return true;
        }
    }
    return false;
}

double clamp01(double value) {
    return std::clamp(value, 0.0, 1.0);
}

/// Average energy of the mood tags (see moodVocabulary()), if any is known
std::optional<double> energyOfMoods(const QStringList& moods) {
    static const QHash<QString, double> kMoodEnergy = {
            {QStringLiteral("aggressive"), 0.9},
            {QStringLiteral("energetic"), 0.85},
            {QStringLiteral("party"), 0.85},
            {QStringLiteral("uplifting"), 0.7},
            {QStringLiteral("happy"), 0.65},
            {QStringLiteral("dark"), 0.55},
            {QStringLiteral("nostalgic"), 0.45},
            {QStringLiteral("elegant"), 0.35},
            {QStringLiteral("chill"), 0.3},
            {QStringLiteral("romantic"), 0.3},
            {QStringLiteral("spiritual"), 0.3},
            {QStringLiteral("sad"), 0.25},
    };
    double sum = 0.0;
    int count = 0;
    for (const QString& mood : moods) {
        const auto it = kMoodEnergy.constFind(mood.toLower());
        if (it != kMoodEnergy.constEnd()) {
            sum += it.value();
            ++count;
        }
    }
    if (count == 0) {
        return std::nullopt;
    }
    return sum / count;
}

QList<EventProfile> createEventProfiles() {
    const QStringList dance = {QStringLiteral("house"),
            QStringLiteral("dance"),
            QStringLiteral("pop"),
            QStringLiteral("afro"),
            QStringLiteral("amapiano"),
            QStringLiteral("hip hop"),
            QStringLiteral("r&b"),
            QStringLiteral("disco"),
            QStringLiteral("funk"),
            QStringLiteral("reggaeton"),
            QStringLiteral("dancehall")};
    const QStringList calm = {QStringLiteral("acoustic"),
            QStringLiteral("classical"),
            QStringLiteral("piano"),
            QStringLiteral("instrumental"),
            QStringLiteral("jazz"),
            QStringLiteral("soul"),
            QStringLiteral("ballad"),
            QStringLiteral("lounge")};
    const QStringList hard = {QStringLiteral("metal"),
            QStringLiteral("hardstyle"),
            QStringLiteral("dubstep"),
            QStringLiteral("punk"),
            QStringLiteral("drill")};
    const QStringList sacred = {QStringLiteral("gospel"),
            QStringLiteral("worship"),
            QStringLiteral("christian"),
            QStringLiteral("choir"),
            QStringLiteral("hymn"),
            QStringLiteral("classical")};

    auto tr = [](const char* text) {
        return QCoreApplication::translate("AiDj", text);
    };

    return {
            {EventType::Club,
                    tr("Club night"),
                    0.55,
                    0.85,
                    0.75,
                    115,
                    135,
                    {QStringLiteral("house"),
                            QStringLiteral("techno"),
                            QStringLiteral("dance"),
                            QStringLiteral("afro"),
                            QStringLiteral("amapiano"),
                            QStringLiteral("edm")},
                    {QStringLiteral("classical"), QStringLiteral("ballad")},
                    QStringLiteral("a club night that builds to a peak and "
                                   "keeps the dance floor full")},
            {EventType::WeddingCeremony,
                    tr("Wedding ceremony"),
                    0.25,
                    0.30,
                    0.45,
                    0,
                    110,
                    calm + QStringList{QStringLiteral("love"), QStringLiteral("romantic")},
                    hard + QStringList{QStringLiteral("techno"), QStringLiteral("trap")},
                    QStringLiteral("a wedding ceremony: elegant, romantic, calm "
                                   "music for the entrance, the vows and the "
                                   "exit of the couple")},
            {EventType::CocktailHour,
                    tr("Cocktail hour"),
                    0.35,
                    0.45,
                    0.50,
                    90,
                    122,
                    {QStringLiteral("jazz"),
                            QStringLiteral("soul"),
                            QStringLiteral("lounge"),
                            QStringLiteral("deep house"),
                            QStringLiteral("r&b"),
                            QStringLiteral("bossa"),
                            QStringLiteral("acoustic")},
                    hard,
                    QStringLiteral("a cocktail hour: relaxed, classy "
                                   "background music that allows "
                                   "conversation")},
            {EventType::Dinner,
                    tr("Dinner"),
                    0.30,
                    0.35,
                    0.40,
                    0,
                    115,
                    calm,
                    hard + QStringList{QStringLiteral("techno")},
                    QStringLiteral("dinner: soft, pleasant music at a low "
                                   "energy that does not disturb "
                                   "conversation")},
            {EventType::WeddingReception,
                    tr("Wedding reception party"),
                    0.55,
                    0.85,
                    0.90,
                    95,
                    130,
                    dance,
                    hard,
                    QStringLiteral("a wedding reception party for all ages: "
                                   "well known feel-good songs that get "
                                   "everybody dancing")},
            {EventType::Birthday,
                    tr("Birthday party"),
                    0.55,
                    0.80,
                    0.85,
                    95,
                    130,
                    dance,
                    {},
                    QStringLiteral("a birthday party: happy, fun, well known "
                                   "dance music")},
            {EventType::Corporate,
                    tr("Corporate event"),
                    0.35,
                    0.55,
                    0.65,
                    90,
                    125,
                    dance + calm,
                    hard + QStringList{QStringLiteral("explicit")},
                    QStringLiteral("a corporate event: professional, positive, "
                                   "clean music without explicit lyrics")},
            {EventType::ChurchService,
                    tr("Church service"),
                    0.30,
                    0.45,
                    0.40,
                    0,
                    120,
                    sacred,
                    hard + QStringList{QStringLiteral("techno"), QStringLiteral("trap")},
                    QStringLiteral("a church service: worship and gospel music "
                                   "with reverence")},
            {EventType::Memorial,
                    tr("Memorial / funeral"),
                    0.15,
                    0.20,
                    0.20,
                    0,
                    100,
                    calm + sacred,
                    dance + hard,
                    QStringLiteral("a memorial service: respectful, calm, "
                                   "comforting music")},
            {EventType::Lounge,
                    tr("Lounge / bar"),
                    0.40,
                    0.50,
                    0.55,
                    90,
                    124,
                    {QStringLiteral("deep house"),
                            QStringLiteral("lounge"),
                            QStringLiteral("soul"),
                            QStringLiteral("jazz"),
                            QStringLiteral("chill"),
                            QStringLiteral("nu disco")},
                    hard,
                    QStringLiteral("a lounge or bar: stylish, groovy music at "
                                   "a medium energy")},
            {EventType::WarmUp,
                    tr("Warm-up set"),
                    0.35,
                    0.50,
                    0.60,
                    100,
                    124,
                    {QStringLiteral("deep house"),
                            QStringLiteral("house"),
                            QStringLiteral("disco"),
                            QStringLiteral("afro")},
                    hard,
                    QStringLiteral("a warm-up set that slowly builds up the "
                                   "energy without stealing the headliner's "
                                   "peak")},
            {EventType::PeakTime,
                    tr("Peak time"),
                    0.80,
                    0.95,
                    0.90,
                    122,
                    140,
                    {QStringLiteral("techno"),
                            QStringLiteral("house"),
                            QStringLiteral("edm"),
                            QStringLiteral("trance"),
                            QStringLiteral("drum")},
                    calm,
                    QStringLiteral("peak time: the highest energy of the night "
                                   "with big, driving tracks")},
    };
}

double freshnessScore(const TrackCandidate& track) {
    if (!track.lastPlayedAt.isValid()) {
        return 1.0;
    }
    // Tracks played in the last days are less fresh for the audience
    const qint64 days = track.lastPlayedAt.daysTo(QDateTime::currentDateTime());
    return clamp01(days / 14.0);
}

double genreScore(const TrackCandidate& candidate,
        const EventProfile& profile,
        const std::optional<TrackCandidate>& current) {
    if (candidate.genre.isEmpty()) {
        return kUnknownScore;
    }
    double score = kUnknownScore;
    if (genreMatchesAny(candidate.genre, profile.preferredGenres)) {
        score = 0.8;
    }
    if (genreMatchesAny(candidate.genre, profile.avoidedGenres)) {
        score = 0.0;
    }
    if (current && !current->genre.isEmpty() &&
            candidate.genre.compare(current->genre, Qt::CaseInsensitive) == 0) {
        // Keeping the genre gives a coherent flow
        score = std::min(1.0, score + 0.3);
    }
    return score;
}

double moodScore(const TrackCandidate& candidate, const QStringList& wantedMoods) {
    if (wantedMoods.isEmpty()) {
        return kUnknownScore;
    }
    if (candidate.moods.isEmpty()) {
        return kUnknownScore;
    }
    for (const QString& mood : wantedMoods) {
        if (candidate.moods.contains(mood, Qt::CaseInsensitive)) {
            return 1.0;
        }
    }
    return 0.1;
}

} // namespace

const QList<EventProfile>& eventProfiles() {
    static const QList<EventProfile> kProfiles = createEventProfiles();
    return kProfiles;
}

const EventProfile& eventProfile(EventType type) {
    const auto& profiles = eventProfiles();
    for (const auto& profile : profiles) {
        if (profile.type == type) {
            return profile;
        }
    }
    return profiles.first();
}

QString keyName(ChromaticKey key) {
    if (key == track::io::key::INVALID) {
        return QStringLiteral("?");
    }
    return KeyUtils::keyToString(key, KeyUtils::KeyNotation::Lancelot);
}

double estimateEnergy(const TrackCandidate& track) {
    double weightSum = 0.0;
    double energy = 0.0;
    if (track.bpm > 0.0) {
        double bpm = track.bpm;
        // Half time feel of very fast tracks, e.g. drum & bass at 174
        if (bpm > 150.0) {
            bpm = std::max(kPeakBpm, bpm * 0.8);
        }
        energy += 0.5 * clamp01((bpm - kCalmBpm) / (kPeakBpm - kCalmBpm));
        weightSum += 0.5;
    }
    if (track.loudnessLufs) {
        energy += 0.3 * clamp01((*track.loudnessLufs - kQuietLufs) / (kLoudLufs - kQuietLufs));
        weightSum += 0.3;
    }
    if (!track.genre.isEmpty()) {
        double genreEnergy = 0.5;
        if (genreMatchesAny(track.genre, kHighEnergyGenres)) {
            genreEnergy = 0.8;
        } else if (genreMatchesAny(track.genre, kLowEnergyGenres)) {
            genreEnergy = 0.2;
        }
        energy += 0.2 * genreEnergy;
        weightSum += 0.2;
    }
    if (const std::optional<double> moodEnergy = energyOfMoods(track.moods)) {
        // Moods matter most for tracks that are not analyzed yet (no BPM)
        const double weight = track.bpm > 0.0 ? 0.2 : 0.5;
        energy += weight * *moodEnergy;
        weightSum += weight;
    }
    if (weightSum == 0.0) {
        return kUnknownScore;
    }
    return clamp01(energy / weightSum);
}

double targetEnergy(const EventProfile& profile, double setProgress) {
    const double progress = clamp01(setProgress);
    // Piecewise linear curve through start, middle (at 60 %) and end
    constexpr double kMiddle = 0.6;
    if (progress <= kMiddle) {
        return profile.startEnergy +
                (profile.middleEnergy - profile.startEnergy) * progress / kMiddle;
    }
    return profile.middleEnergy +
            (profile.endEnergy - profile.middleEnergy) * (progress - kMiddle) / (1.0 - kMiddle);
}

double keyCompatibility(ChromaticKey from, ChromaticKey to) {
    if (from == track::io::key::INVALID || to == track::io::key::INVALID) {
        return kUnknownScore;
    }
    if (from == to) {
        return 1.0;
    }
    if (KeyUtils::getCompatibleKeys(from).contains(to)) {
        return 0.9;
    }
    // Not compatible: how many semitones of key shift would fix it
    const int steps = std::abs(KeyUtils::shortestStepsToCompatibleKey(to, from));
    return steps <= 1 ? 0.3 : 0.1;
}

namespace {
// Scores below this are key clashes
constexpr double kHarmonicThreshold = 0.5;
} // namespace

double bpmCompatibility(double fromBpm, double toBpm, double maxDeviation) {
    if (fromBpm <= 0.0 || toBpm <= 0.0 || maxDeviation <= 0.0) {
        return kUnknownScore;
    }
    const double deviations[] = {
            std::abs(toBpm - fromBpm) / fromBpm,
            std::abs(toBpm * 2.0 - fromBpm) / fromBpm,
            std::abs(toBpm / 2.0 - fromBpm) / fromBpm,
    };
    const double deviation = *std::min_element(std::begin(deviations), std::end(deviations));
    return clamp01(1.0 - deviation / maxDeviation);
}

Recommendation scoreCandidate(const TrackCandidate& candidate, const MixContext& context) {
    const EventProfile& profile = eventProfile(context.event);
    Recommendation result;
    result.track = candidate;
    result.estimatedEnergy = estimateEnergy(candidate);

    const double wantedEnergy = context.targetEnergy.value_or(
            targetEnergy(profile, context.setProgress));
    result.energyScore = clamp01(1.0 - std::abs(result.estimatedEnergy - wantedEnergy) * 2.0);

    if (context.current) {
        result.keyScore = keyCompatibility(context.current->key, candidate.key);
        result.bpmScore = bpmCompatibility(
                context.current->bpm, candidate.bpm, context.maxBpmDeviation);
    } else {
        result.keyScore = kUnknownScore;
        // Without a playing track the BPM should fit the event
        if (candidate.bpm > 0.0 && profile.maxBpm > 0.0) {
            const bool inRange = candidate.bpm >= profile.minBpm && candidate.bpm <= profile.maxBpm;
            result.bpmScore = inRange ? 1.0 : 0.2;
        } else {
            result.bpmScore = kUnknownScore;
        }
    }
    result.genreScore = genreScore(candidate, profile, context.current);
    result.moodScore = moodScore(candidate, context.moods);
    const double ratingScore = candidate.rating > 0 ? candidate.rating / 5.0 : kUnknownScore;

    result.score = kKeyWeight * result.keyScore +
            kBpmWeight * result.bpmScore +
            kEnergyWeight * result.energyScore +
            kGenreWeight * result.genreScore +
            kMoodWeight * result.moodScore +
            kRatingWeight * ratingScore +
            kFreshnessWeight * freshnessScore(candidate);

    // Variety: the same artist twice in a row sounds like a playlist mistake
    const QString artist = normalizedArtist(candidate.artist);
    bool sameArtist = false;
    bool recentArtist = false;
    if (!artist.isEmpty()) {
        sameArtist = context.current && normalizedArtist(context.current->artist) == artist;
        recentArtist = !sameArtist && context.recentArtists.contains(artist);
    }
    if (sameArtist) {
        result.score *= kSameArtistFactor;
    } else if (recentArtist) {
        result.score *= kRecentArtistFactor;
    }

    // Reasons shown to the DJ
    if (context.current) {
        if (result.keyScore >= 0.9) {
            result.reasons << QCoreApplication::translate("AiDj", "Key %1 → %2 (harmonic)")
                                      .arg(keyName(context.current->key),
                                              keyName(candidate.key));
        } else if (result.keyScore < kHarmonicThreshold) {
            const int steps = KeyUtils::shortestStepsToCompatibleKey(
                    candidate.key, context.current->key);
            result.reasons << QCoreApplication::translate("AiDj",
                    "Key clash %1 → %2 (fix: key shift %3)")
                                      .arg(keyName(context.current->key),
                                              keyName(candidate.key),
                                              (steps > 0 ? QStringLiteral("+") : QString()) +
                                                      QString::number(steps));
        }
        if (candidate.bpm > 0.0 && context.current->bpm > 0.0) {
            const double change = (candidate.bpm - context.current->bpm) /
                    context.current->bpm * 100.0;
            result.reasons << QCoreApplication::translate("AiDj", "BPM %1 → %2 (%3%)")
                                      .arg(context.current->bpm, 0, 'f', 1)
                                      .arg(candidate.bpm, 0, 'f', 1)
                                      .arg(change >= 0 ? QStringLiteral("+") +
                                                              QString::number(change, 'f', 1)
                                                       : QString::number(change, 'f', 1));
        }
    }
    if (result.energyScore >= 0.8) {
        result.reasons << QCoreApplication::translate("AiDj", "Energy fits the moment");
    } else if (result.estimatedEnergy > wantedEnergy) {
        result.reasons << QCoreApplication::translate("AiDj", "Raises the energy");
    } else {
        result.reasons << QCoreApplication::translate("AiDj", "Lowers the energy");
    }
    if (result.genreScore == 0.0) {
        result.reasons << QCoreApplication::translate("AiDj", "Genre does not fit the event");
    }
    if (result.moodScore == 1.0) {
        result.reasons << QCoreApplication::translate("AiDj", "Matches the mood");
    }
    if (sameArtist) {
        result.reasons << QCoreApplication::translate("AiDj", "Same artist as now");
    } else if (recentArtist) {
        result.reasons << QCoreApplication::translate("AiDj", "Artist played recently");
    }
    return result;
}

QString normalizedArtist(const QString& artist) {
    return artist.trimmed().toLower();
}

QList<Recommendation> recommend(const QList<TrackCandidate>& candidates,
        const MixContext& context,
        int maxResults) {
    QList<Recommendation> results;
    results.reserve(candidates.size());
    for (const TrackCandidate& candidate : candidates) {
        if (candidate.playedThisSession) {
            continue;
        }
        if (context.current && candidate.id.isValid() && candidate.id == context.current->id) {
            continue;
        }
        Recommendation recommendation = scoreCandidate(candidate, context);
        if (context.harmonicOnly && context.current &&
                context.current->key != track::io::key::INVALID &&
                recommendation.keyScore < kHarmonicThreshold) {
            continue;
        }
        results.append(std::move(recommendation));
    }
    std::stable_sort(results.begin(),
            results.end(),
            [](const Recommendation& a, const Recommendation& b) {
                return a.score > b.score;
            });
    if (maxResults >= 0 && results.size() > maxResults) {
        results.erase(results.begin() + maxResults, results.end());
    }
    return results;
}

QString transitionTip(const TrackCandidate& from, const TrackCandidate& to) {
    auto tr = [](const char* text) {
        return QCoreApplication::translate("AiDj", text);
    };
    QStringList tips;
    const double keyScore = keyCompatibility(from.key, to.key);
    const double bpmScore = bpmCompatibility(from.bpm, to.bpm, 0.08);
    const double energyChange = estimateEnergy(to) - estimateEnergy(from);
    const bool closeTempo = bpmScore >= 0.5;

    if (from.bpm > 0.0 && to.bpm > 0.0 && !closeTempo) {
        tips << tr(
                "The tempos are far apart: loop the last bar of the outgoing "
                "track, use Echo to fade it out, then drop the new track on "
                "the downbeat.");
    } else if (keyScore >= 0.9) {
        tips << tr(
                "The keys are compatible: do a long blend over 16 to 32 "
                "bars and swap the bass EQ on the phrase change (watch the "
                "beat counter).");
    } else if (keyScore < kHarmonicThreshold && to.key != track::io::key::INVALID) {
        const int steps = KeyUtils::shortestStepsToCompatibleKey(to.key, from.key);
        tips << tr(
                "The keys clash: shift the key of the new track by %1 "
                "semitone(s) (or press \"sync key\"), or keep the overlap "
                "short and cut the mids and bass of the outgoing track.")
                        .arg((steps > 0 ? QStringLiteral("+") : QString()) +
                                QString::number(steps));
    } else {
        tips << tr(
                "Blend with sync on and swap the bass EQ on the phrase "
                "change.");
    }
    if (energyChange > 0.15) {
        tips << tr(
                "The energy goes up: build tension with a high-pass filter "
                "sweep on the outgoing track and release it on the drop.");
    } else if (energyChange < -0.15) {
        tips << tr(
                "The energy goes down: let the outgoing track fade with a "
                "Reverb tail, so the drop in energy feels intentional.");
    }
    return tips.join(QChar(' '));
}

QList<Recommendation> buildSet(const QList<TrackCandidate>& candidates,
        MixContext context,
        double durationMinutes,
        int maxTracks) {
    QList<Recommendation> set;
    if (durationMinutes <= 0.0 || maxTracks == 0) {
        return set;
    }
    QList<TrackCandidate> remaining = candidates;
    const double totalSeconds = durationMinutes * 60.0;
    double elapsedSeconds = 0.0;
    // Assume DJs mix out before the end of a track
    constexpr double kPlayedFraction = 0.8;
    constexpr double kDefaultTrackSeconds = 210.0;

    while (!remaining.isEmpty()) {
        if (maxTracks > 0) {
            if (set.size() >= maxTracks) {
                break;
            }
            // The energy curve is spread over the wanted number of tracks
            context.setProgress = maxTracks > 1
                    ? static_cast<double>(set.size()) / (maxTracks - 1)
                    : 0.0;
        } else {
            if (elapsedSeconds >= totalSeconds) {
                break;
            }
            context.setProgress = elapsedSeconds / totalSeconds;
        }
        const QList<Recommendation> next = recommend(remaining, context, 1);
        if (next.isEmpty()) {
            break;
        }
        const Recommendation& best = next.first();
        set.append(best);
        const TrackId chosenId = best.track.id;
        remaining.erase(std::remove_if(remaining.begin(),
                                remaining.end(),
                                [&](const TrackCandidate& track) {
                                    return track.id == chosenId;
                                }),
                remaining.end());
        const double length = best.track.durationSeconds > 0.0
                ? best.track.durationSeconds
                : kDefaultTrackSeconds;
        elapsedSeconds += length * kPlayedFraction;
        if (context.current) {
            const QString artist = normalizedArtist(context.current->artist);
            if (!artist.isEmpty()) {
                context.recentArtists.prepend(artist);
                while (context.recentArtists.size() > kRecentArtistCount) {
                    context.recentArtists.removeLast();
                }
            }
        }
        context.current = best.track;
    }
    return set;
}

} // namespace aidj
} // namespace mixxx

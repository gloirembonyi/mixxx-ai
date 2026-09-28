// Tests for the AI DJ assistant and the free music finder (offline parts)

#include <gtest/gtest.h>

#include <QJsonArray>
#include <QJsonDocument>

#include "library/aidj/aidjlibrary.h"
#include "library/aidj/aiservice.h"
#include "library/aidj/djrecommender.h"
#include "library/aidj/musicsources.h"
#include "track/keyutils.h"

using namespace mixxx::aidj;
namespace key = mixxx::track::io::key;

namespace {

TrackCandidate makeTrack(int id,
        double bpm,
        key::ChromaticKey trackKey,
        const QString& genre = QString(),
        double lufs = -9.0) {
    TrackCandidate track;
    track.id = TrackId(QVariant(id));
    track.artist = QStringLiteral("Artist %1").arg(id);
    track.title = QStringLiteral("Title %1").arg(id);
    track.genre = genre;
    track.bpm = bpm;
    track.key = trackKey;
    track.durationSeconds = 240;
    track.loudnessLufs = lufs;
    return track;
}

} // namespace

TEST(AiDjRecommenderTest, KeyCompatibility) {
    // 8A (A minor) -> 8A, 9A (E minor), 7A (D minor), 8B (C major) are harmonic
    EXPECT_DOUBLE_EQ(1.0, keyCompatibility(key::A_MINOR, key::A_MINOR));
    EXPECT_DOUBLE_EQ(0.9, keyCompatibility(key::A_MINOR, key::E_MINOR));
    EXPECT_DOUBLE_EQ(0.9, keyCompatibility(key::A_MINOR, key::D_MINOR));
    EXPECT_DOUBLE_EQ(0.9, keyCompatibility(key::A_MINOR, key::C_MAJOR));
    // A minor -> E flat minor is a tritone away: a clash that one semitone of
    // key shift fixes (E flat minor -> D minor or E minor)
    EXPECT_DOUBLE_EQ(0.3, keyCompatibility(key::A_MINOR, key::E_FLAT_MINOR));
    EXPECT_EQ(1,
            std::abs(KeyUtils::shortestStepsToCompatibleKey(
                    key::E_FLAT_MINOR, key::A_MINOR)));
    // Unknown keys are neutral
    EXPECT_DOUBLE_EQ(0.5, keyCompatibility(key::INVALID, key::A_MINOR));
}

TEST(AiDjRecommenderTest, BpmCompatibilityAllowsHalfAndDoubleTempo) {
    EXPECT_DOUBLE_EQ(1.0, bpmCompatibility(124, 124, 0.08));
    EXPECT_NEAR(0.0, bpmCompatibility(100, 140, 0.08), 1e-9);
    // 70 BPM is half of 140: the beats line up
    EXPECT_DOUBLE_EQ(1.0, bpmCompatibility(140, 70, 0.08));
    EXPECT_DOUBLE_EQ(1.0, bpmCompatibility(87, 174, 0.08));
    // 4% off is half of the allowed 8%
    EXPECT_NEAR(0.5, bpmCompatibility(125, 130, 0.08), 1e-9);
}

TEST(AiDjRecommenderTest, EnergyEstimate) {
    const TrackCandidate calm = makeTrack(1, 72, key::C_MAJOR, QStringLiteral("Classical"), -20);
    const TrackCandidate peak = makeTrack(2, 138, key::A_MINOR, QStringLiteral("Techno"), -6);
    EXPECT_LT(estimateEnergy(calm), 0.2);
    EXPECT_GT(estimateEnergy(peak), 0.85);
    // Without any information the energy is neutral
    EXPECT_DOUBLE_EQ(0.5, estimateEnergy(TrackCandidate()));
}

TEST(AiDjRecommenderTest, TargetEnergyFollowsTheEventCurve) {
    const EventProfile& reception = eventProfile(EventType::WeddingReception);
    EXPECT_DOUBLE_EQ(reception.startEnergy, targetEnergy(reception, 0.0));
    EXPECT_DOUBLE_EQ(reception.middleEnergy, targetEnergy(reception, 0.6));
    EXPECT_DOUBLE_EQ(reception.endEnergy, targetEnergy(reception, 1.0));
    EXPECT_GT(targetEnergy(reception, 0.3), reception.startEnergy);
}

TEST(AiDjRecommenderTest, EveryEventHasAProfile) {
    for (int i = 0; i < static_cast<int>(EventType::Count); ++i) {
        const auto type = static_cast<EventType>(i);
        EXPECT_EQ(type, eventProfile(type).type);
        EXPECT_FALSE(eventProfile(type).name.isEmpty());
        EXPECT_FALSE(eventProfile(type).description.isEmpty());
    }
}

TEST(AiDjRecommenderTest, RecommendsTheHarmonicTrackWithCloseTempo) {
    MixContext context;
    context.event = EventType::Club;
    context.current = makeTrack(1, 124, key::A_MINOR, QStringLiteral("House"));
    context.targetEnergy = estimateEnergy(*context.current);

    const QList<TrackCandidate> candidates = {
            makeTrack(2, 140, key::E_FLAT_MINOR, QStringLiteral("House")), // clash, far
            makeTrack(3, 125, key::E_MINOR, QStringLiteral("House")),      // perfect
            makeTrack(4, 124, key::F_SHARP_MAJOR, QStringLiteral("House")),
            *context.current, // the playing track itself
    };
    const QList<Recommendation> results = recommend(candidates, context, 10);
    ASSERT_EQ(3, results.size()); // the current track is skipped
    EXPECT_EQ(TrackId(QVariant(3)), results.first().track.id);
    EXPECT_EQ(TrackId(QVariant(2)), results.last().track.id);
    EXPECT_FALSE(results.first().reasons.isEmpty());
}

TEST(AiDjRecommenderTest, SkipsTracksPlayedInThisSession) {
    MixContext context;
    TrackCandidate played = makeTrack(1, 124, key::A_MINOR);
    played.playedThisSession = true;
    const QList<Recommendation> results =
            recommend({played, makeTrack(2, 124, key::A_MINOR)}, context, 10);
    ASSERT_EQ(1, results.size());
    EXPECT_EQ(TrackId(QVariant(2)), results.first().track.id);
}

TEST(AiDjRecommenderTest, CalmEventsAvoidPartyGenres) {
    MixContext context;
    context.event = EventType::WeddingCeremony;
    const QList<Recommendation> results = recommend(
            {makeTrack(1, 128, key::A_MINOR, QStringLiteral("Techno"), -7),
                    makeTrack(2, 76, key::C_MAJOR, QStringLiteral("Piano"), -18)},
            context,
            10);
    ASSERT_EQ(2, results.size());
    EXPECT_EQ(TrackId(QVariant(2)), results.first().track.id);
}

TEST(AiDjRecommenderTest, MoodsInfluenceTheOrder) {
    MixContext context;
    context.moods << QStringLiteral("romantic");
    TrackCandidate romantic = makeTrack(1, 100, key::C_MAJOR);
    romantic.moods << QStringLiteral("romantic");
    TrackCandidate dark = makeTrack(2, 100, key::C_MAJOR);
    dark.moods << QStringLiteral("dark");
    const QList<Recommendation> results = recommend({dark, romantic}, context, 10);
    EXPECT_EQ(TrackId(QVariant(1)), results.first().track.id);
}

TEST(AiDjRecommenderTest, BuildSetChainsTracksWithoutRepeats) {
    QList<TrackCandidate> library;
    for (int i = 1; i <= 30; ++i) {
        library << makeTrack(i, 118 + i % 10, key::A_MINOR, QStringLiteral("House"));
    }
    MixContext context;
    // 10 tracks of 4 minutes, 80 % played each: about 13 tracks for 40 min
    const QList<Recommendation> set = buildSet(library, context, 40);
    EXPECT_GE(set.size(), 10);
    QSet<QString> ids;
    for (const auto& item : set) {
        ids.insert(item.track.id.toString());
    }
    EXPECT_EQ(set.size(), ids.size());
}

TEST(AiDjRecommenderTest, TransitionTipExplainsClashesAndTempo) {
    const QString clash = transitionTip(makeTrack(1, 124, key::A_MINOR),
            makeTrack(2, 124, key::E_FLAT_MINOR));
    EXPECT_TRUE(clash.contains(QStringLiteral("clash")));
    EXPECT_TRUE(clash.contains(QStringLiteral("semitone")));
    const QString tempo = transitionTip(makeTrack(1, 124, key::A_MINOR),
            makeTrack(2, 100, key::A_MINOR));
    EXPECT_TRUE(tempo.contains(QStringLiteral("Echo")));
}

TEST(AiDjLibraryTest, LoudnessFromReplayGain) {
    EXPECT_FALSE(AiDjLibrary::loudnessFromReplayGainRatio(0.0).has_value());
    // A ratio of 1 means the track already has the reference loudness
    EXPECT_NEAR(-18.0, *AiDjLibrary::loudnessFromReplayGainRatio(1.0), 1e-9);
    // Loud tracks are turned down (ratio 0.5 = -6 dB): about -12 LUFS
    EXPECT_NEAR(-12.0, *AiDjLibrary::loudnessFromReplayGainRatio(0.5), 0.05);
}

TEST(MusicFinderTest, ClassifiesLicenses) {
    const LicenseInfo by = classifyLicense(
            QStringLiteral("http://creativecommons.org/licenses/by/4.0/"));
    EXPECT_TRUE(by.known);
    EXPECT_TRUE(by.paidGigsOk);
    EXPECT_TRUE(by.publishMixesOk);
    EXPECT_EQ(QStringLiteral("CC BY 4.0"), by.name);

    const LicenseInfo nc = classifyLicense(
            QStringLiteral("https://creativecommons.org/licenses/by-nc-sa/3.0/"));
    EXPECT_FALSE(nc.paidGigsOk);
    EXPECT_FALSE(nc.publishMixesOk);

    const LicenseInfo nd = classifyLicense(
            QStringLiteral("http://creativecommons.org/licenses/by-nd/3.0/"));
    EXPECT_TRUE(nd.paidGigsOk);
    EXPECT_FALSE(nd.publishMixesOk);

    const LicenseInfo cc0 = classifyLicense(
            QStringLiteral("http://creativecommons.org/publicdomain/zero/1.0/"));
    EXPECT_TRUE(cc0.paidGigsOk);
    EXPECT_EQ(QStringLiteral("CC0"), cc0.name);

    // ccMixter license names
    EXPECT_FALSE(classifyLicense(QStringLiteral("Attribution Noncommercial")).paidGigsOk);
    EXPECT_TRUE(classifyLicense(QStringLiteral("Attribution")).paidGigsOk);

    const LicenseInfo unknown = classifyLicense(QString());
    EXPECT_FALSE(unknown.known);
    EXPECT_FALSE(unknown.paidGigsOk);
}

TEST(MusicFinderTest, ParsesDurations) {
    EXPECT_DOUBLE_EQ(245.5, parseDurationSeconds(QStringLiteral("245.5")));
    EXPECT_DOUBLE_EQ(239.0, parseDurationSeconds(QStringLiteral("3:59")));
    EXPECT_DOUBLE_EQ(3723.0, parseDurationSeconds(QStringLiteral("1:02:03")));
    EXPECT_DOUBLE_EQ(0.0, parseDurationSeconds(QString()));
}

TEST(MusicFinderTest, ParsesInternetArchiveItemAndPrefersLossless) {
    const QByteArray json = R"({
        "metadata": {"identifier": "netlabel-123", "creator": "Some Artist",
                     "title": "Some EP",
                     "licenseurl": "http://creativecommons.org/licenses/by-sa/3.0/"},
        "files": [
            {"name": "01 Intro.flac", "format": "Flac", "size": "30000000",
             "length": "4:10", "title": "Intro", "source": "original"},
            {"name": "01 Intro.mp3", "format": "VBR MP3", "size": "5000000",
             "length": "250.1", "source": "derivative"},
            {"name": "02 Groove.mp3", "format": "VBR MP3", "size": "6000000",
             "length": "300", "creator": "Guest", "source": "original"},
            {"name": "cover.jpg", "format": "JPEG"},
            {"name": "all.zip", "format": "ZIP"}
        ]})";
    const QList<MusicSearchResult> lossless = parseInternetArchiveItem(json, true);
    ASSERT_EQ(2, lossless.size());
    EXPECT_EQ(QStringLiteral("Intro"), lossless[0].title);
    EXPECT_EQ(QStringLiteral("FLAC"), lossless[0].format);
    EXPECT_EQ(QStringLiteral("flac"), lossless[0].fileExtension);
    EXPECT_EQ(QStringLiteral("Some Artist"), lossless[0].artist);
    EXPECT_DOUBLE_EQ(250.0, lossless[0].durationSeconds);
    EXPECT_TRUE(lossless[0].license.paidGigsOk);
    EXPECT_EQ(QStringLiteral("https"), lossless[0].downloadUrl.scheme());
    // Spaces must be encoded in the request
    EXPECT_TRUE(lossless[0].downloadUrl.toEncoded().endsWith("/netlabel-123/01%20Intro.flac"));
    EXPECT_EQ(QStringLiteral("Guest"), lossless[1].artist);

    const QList<MusicSearchResult> compact = parseInternetArchiveItem(json, false);
    ASSERT_EQ(2, compact.size());
    EXPECT_EQ(QStringLiteral("MP3"), compact[0].format);
}

TEST(MusicFinderTest, ParsesInternetArchiveSearch) {
    const QByteArray json = R"({"response": {"numFound": 2, "docs": [
        {"identifier": "a"}, {"identifier": "b"}]}})";
    EXPECT_EQ(QStringList({QStringLiteral("a"), QStringLiteral("b")}),
            parseInternetArchiveSearch(json));
    EXPECT_TRUE(parseInternetArchiveSearch("not json").isEmpty());
}

TEST(MusicFinderTest, ParsesCcMixterResults) {
    const QByteArray json = R"([{
        "upload_name": "Night Drive", "user_name": "dj1", "user_real_name": "DJ One",
        "file_page_url": "http://ccmixter.org/files/dj1/123",
        "license_url": "http://creativecommons.org/licenses/by/3.0/",
        "upload_extra": {"bpm": "122"},
        "files": [
            {"download_url": "http://ccmixter.org/content/dj1/night.zip",
             "file_format_info": {"mime_type": "application/zip"}},
            {"download_url": "http://ccmixter.org/content/dj1/night.mp3",
             "file_rawsize": 7654321,
             "file_format_info": {"mime_type": "audio/mpeg", "ps": "3:59"}}
        ]}])";
    const QList<MusicSearchResult> results = parseCcMixterResults(json);
    ASSERT_EQ(1, results.size());
    const MusicSearchResult& result = results.first();
    EXPECT_EQ(QStringLiteral("Night Drive"), result.title);
    EXPECT_EQ(QStringLiteral("DJ One"), result.artist);
    EXPECT_DOUBLE_EQ(122.0, result.bpm);
    EXPECT_DOUBLE_EQ(239.0, result.durationSeconds);
    EXPECT_EQ(7654321, result.sizeBytes);
    EXPECT_EQ(QStringLiteral("mp3"), result.fileExtension);
    EXPECT_EQ(QStringLiteral("https"), result.downloadUrl.scheme());
    EXPECT_TRUE(result.license.paidGigsOk);
}

TEST(MusicFinderTest, FileNamesAreSafe) {
    MusicSearchResult result;
    result.artist = QStringLiteral("AC/DC");
    result.title = QStringLiteral("What? Why: \"Now\" ...");
    result.fileExtension = QStringLiteral("flac");
    const QString name = MusicDownloader::fileNameFor(result);
    EXPECT_FALSE(name.contains(QChar('/')));
    EXPECT_FALSE(name.contains(QChar('?')));
    EXPECT_FALSE(name.contains(QChar(':')));
    EXPECT_FALSE(name.contains(QChar('"')));
    EXPECT_TRUE(name.endsWith(QStringLiteral(".flac")));
}

TEST(AiServiceTest, BuildsJevChoiceRequest) {
    MixContext context;
    context.event = EventType::WeddingReception;
    context.current = makeTrack(1, 120, key::A_MINOR, QStringLiteral("Pop"));
    context.moods << QStringLiteral("happy");
    Recommendation a;
    a.track = makeTrack(7, 121, key::E_MINOR, QStringLiteral("Pop"));
    Recommendation b;
    b.track = makeTrack(9, 118, key::C_MAJOR, QStringLiteral("Disco"));

    const QJsonObject request = AiService::buildJevChoiceRequest(context, {a, b});
    EXPECT_EQ(QStringLiteral("jev-latest"), request.value(QStringLiteral("model")).toString());
    const QJsonObject question = request.value(QStringLiteral("questions"))
                                         .toObject()
                                         .value(QStringLiteral("next_track"))
                                         .toObject();
    EXPECT_EQ(QStringLiteral("choice"), question.value(QStringLiteral("type")).toString());
    const QJsonObject criteria = question.value(QStringLiteral("criteria")).toObject();
    EXPECT_EQ(2, criteria.size());
    EXPECT_TRUE(criteria.contains(QStringLiteral("t7")));
    EXPECT_TRUE(criteria.value(QStringLiteral("t9")).toString().contains(QStringLiteral("Disco")));
    const QJsonObject state = request.value(QStringLiteral("state")).toObject();
    EXPECT_TRUE(state.value(QStringLiteral("now_playing")).toString().contains(QStringLiteral("Title 1")));
    EXPECT_EQ(QStringLiteral("happy"), state.value(QStringLiteral("wanted_moods")).toString());
}

TEST(AiServiceTest, ParsesJevChoice) {
    const QJsonObject response = QJsonDocument::fromJson(R"({
        "model": "jev-1.13.0",
        "answers": {"next_track": {"type": "choice", "choice": "t42",
                    "confidence": 0.83, "probabilities": {"t42": 0.83}}}})")
                                         .object();
    double confidence = 0.0;
    EXPECT_EQ(TrackId(QVariant(42)), AiService::parseJevChoice(response, &confidence));
    EXPECT_DOUBLE_EQ(0.83, confidence);
    EXPECT_FALSE(AiService::parseJevChoice(QJsonObject(), nullptr).isValid());
}

TEST(AiServiceTest, ExtractsGeminiText) {
    const QJsonObject response = QJsonDocument::fromJson(R"({
        "candidates": [{"content": {"role": "model", "parts": [
            {"text": "{\"trackIds\": [1, "}, {"text": "2]}"}]}}]})")
                                         .object();
    EXPECT_EQ(QStringLiteral("{\"trackIds\": [1, 2]}"), AiService::geminiResponseText(response));
    EXPECT_TRUE(AiService::geminiResponseText(QJsonObject()).isEmpty());
}

TEST(AiDjRecommenderTest, BuildSetHonorsTheTrackCount) {
    QList<TrackCandidate> library;
    for (int i = 1; i <= 40; ++i) {
        library << makeTrack(i, 118 + i % 10, key::A_MINOR, QStringLiteral("House"));
    }
    MixContext context;
    // The duration would allow many more tracks, the count wins
    EXPECT_EQ(7, buildSet(library, context, 600, 7).size());
    EXPECT_EQ(40, buildSet(library, context, 6000, 100).size());
    EXPECT_TRUE(buildSet(library, context, 60, 0).isEmpty());
}

TEST(AiDjRecommenderTest, AvoidsTheSameArtistBackToBack) {
    MixContext context;
    context.current = makeTrack(1, 124, key::A_MINOR, QStringLiteral("House"));
    TrackCandidate sameArtist = makeTrack(2, 124, key::A_MINOR, QStringLiteral("House"));
    sameArtist.artist = context.current->artist.toUpper(); // case does not matter
    const TrackCandidate other = makeTrack(3, 124, key::A_MINOR, QStringLiteral("House"));
    const QList<Recommendation> results = recommend({sameArtist, other}, context, 10);
    ASSERT_EQ(2, results.size());
    EXPECT_EQ(TrackId(QVariant(3)), results.first().track.id);
    EXPECT_TRUE(results.last().reasons.join(QChar(' ')).contains(QStringLiteral("Same artist")));

    // Recently played artists are penalized less, but still
    MixContext recent;
    recent.recentArtists << normalizedArtist(other.artist);
    const QList<Recommendation> recentResults =
            recommend({other, makeTrack(4, 124, key::A_MINOR, QStringLiteral("House"))},
                    recent,
                    10);
    EXPECT_EQ(TrackId(QVariant(4)), recentResults.first().track.id);
}

TEST(AiDjRecommenderTest, HarmonicOnlySkipsKeyClashes) {
    MixContext context;
    context.current = makeTrack(1, 124, key::A_MINOR);
    context.harmonicOnly = true;
    const QList<Recommendation> results = recommend(
            {makeTrack(2, 124, key::E_FLAT_MINOR), // clash
                    makeTrack(3, 124, key::E_MINOR),
                    makeTrack(4, 124, key::INVALID)}, // unknown keys stay
            context,
            10);
    ASSERT_EQ(2, results.size());
    for (const Recommendation& result : results) {
        EXPECT_NE(TrackId(QVariant(2)), result.track.id);
    }
}

TEST(AiServiceTest, AdjustsTheConfigForGemini3) {
    const QJsonObject config{{QStringLiteral("temperature"), 0.2},
            {QStringLiteral("maxOutputTokens"), 1024}};
    const QJsonObject gemini3 = AiService::adjustGeminiConfig(config, QStringLiteral("gemini-3.5-flash-lite"));
    EXPECT_FALSE(gemini3.contains(QStringLiteral("temperature")));
    EXPECT_EQ(QStringLiteral("low"),
            gemini3.value(QStringLiteral("thinkingConfig"))
                    .toObject()
                    .value(QStringLiteral("thinkingLevel"))
                    .toString());
    EXPECT_EQ(1024, gemini3.value(QStringLiteral("maxOutputTokens")).toInt());
    // Older models keep the config as it is
    EXPECT_EQ(config, AiService::adjustGeminiConfig(config, QStringLiteral("gemini-2.5-flash")));
}

TEST(AiServiceTest, ParsesGeminiJsonAlsoInCodeBlocks) {
    auto response = [](const QString& text) {
        return QJsonObject{{QStringLiteral("candidates"),
                QJsonArray{QJsonObject{{QStringLiteral("content"),
                        QJsonObject{{QStringLiteral("parts"),
                                QJsonArray{QJsonObject{{QStringLiteral("text"), text}}}}}}}}}};
    };
    EXPECT_EQ(5,
            AiService::parseGeminiJson(response(QStringLiteral("{\"trackId\": 5}")))
                    .value(QStringLiteral("trackId"))
                    .toInt());
    EXPECT_EQ(6,
            AiService::parseGeminiJson(response(QStringLiteral("```json\n{\"trackId\": 6}\n```")))
                    .value(QStringLiteral("trackId"))
                    .toInt());
    EXPECT_TRUE(AiService::parseGeminiJson(response(QStringLiteral("no json"))).isEmpty());
}

TEST(AiServiceTest, ExplainsEmptyGeminiAnswers) {
    const QJsonObject blocked = QJsonDocument::fromJson(
            R"({"promptFeedback": {"blockReason": "SAFETY"}})")
                                        .object();
    EXPECT_TRUE(AiService::geminiEmptyReason(blocked).contains(QStringLiteral("SAFETY")));
    const QJsonObject cutOff = QJsonDocument::fromJson(
            R"({"candidates": [{"finishReason": "MAX_TOKENS"}]})")
                                       .object();
    EXPECT_TRUE(AiService::geminiEmptyReason(cutOff).contains(QStringLiteral("cut off")));
    EXPECT_FALSE(AiService::geminiEmptyReason(QJsonObject()).isEmpty());
}

TEST(MusicFinderTest, InternetArchiveQueryFiltersLicensesForPaidGigs) {
    const QString all = MusicSearch::internetArchiveQuery(QStringLiteral("deep \"house\""), false);
    EXPECT_TRUE(all.contains(QStringLiteral("(deep house)")));
    EXPECT_FALSE(all.contains(QStringLiteral("licenseurl")));
    const QString paid = MusicSearch::internetArchiveQuery(QStringLiteral("techno"), true);
    EXPECT_TRUE(paid.contains(QStringLiteral("NOT licenseurl:*nc*")));
}

TEST(AiDjRecommenderTest, MoodsGiveTheEnergyOfTracksWithoutBpm) {
    TrackCandidate party;
    party.moods << QStringLiteral("party") << QStringLiteral("energetic");
    TrackCandidate calm;
    calm.moods << QStringLiteral("romantic") << QStringLiteral("chill");
    EXPECT_GT(estimateEnergy(party), 0.8);
    EXPECT_LT(estimateEnergy(calm), 0.35);
}

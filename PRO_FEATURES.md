# Pro features added to this Mixxx build

This checkout is Mixxx 2.5 (branch `pro-features`) with extra performance
features that pro DJ software like Serato, Rekordbox and Traktor offer.

## Build and run (Windows)

```bat
build_mixxx.bat          :: configure (first time) and build
build_mixxx.bat test     :: build and run the unit tests
build_mixxx.bat run      :: build and start Mixxx
```

Requirements: Visual Studio 2022 Build Tools with the C++ workload. The
prebuilt dependencies are downloaded to `buildenv\` on the first build.
The program is built to `build\portable\mixxx.exe`.

## Features

### Beat, bar and phrase counter
Each deck shows 4 beat LEDs (the orange one is the downbeat), the current bar
of the 8-bar phrase, and a countdown of beats until the next 32-beat phrase.
The countdown turns orange during the last bar, the moment to start a
transition. The count starts at the first beat of the beatgrid: if it is not
on the downbeat, shift the beatgrid.

Controls (per deck, read-only): `beat_number`, `beat_in_bar`, `bar_number`,
`bar_in_phrase`, `phrase_number`, `beats_until_phrase`, `phrase_last_bar`.

### Phase meter
A thin meter above each overview waveform shows how far the deck's beats are
ahead (right) or behind (left) the sync leader, or the other playing deck.
It turns green when the beats line up: beatmatching by eye without sync.

### Pitch Play (like Serato)
Plays the last used hotcue (or the main cue) transposed in semitones, one pad
per semitone. Map it to your controller's performance pads.

- `pitchplay_1_activate` ... `pitchplay_8_activate`: play the cue with the
  pad's pitch offset
- `pitchplay_range`: 0 = low (-7..0), 1 = mid (-3..+4, default), 2 = up (0..+7)
- `pitchplay_reset`: back to the pitch before Pitch Play was used
- `pitchplay_N_enabled`, `pitchplay_offset`: for pad LEDs and displays

### Smooth tempo reset
The RESET button below SYNC glides the tempo back to the track's original BPM
over 4 seconds, without an audible jump. Moving the tempo slider cancels it.
Controls: `rate_reset_slow`, `rate_reset_slow_active`.

### Main output limiter
A zero-latency peak limiter at -0.3 dBFS protects the speakers, recordings
and live streams against digital clipping. It is on by default and only
acts when the signal would clip. The LIMIT button in the top toolbar turns it
on or off and gets a red frame while the limiter reduces the volume: lower the
main gain if it lights up constantly.
Controls: `[Master],limiter_enabled`, `[Master],limiter_reduction_db`.

### Restore decks after a crash or restart
Mixxx remembers the track of each deck and loads it again on the next start,
so a crash in the middle of a set doesn't leave you with empty decks. The
tracks are loaded, not played. Turn it off in Preferences > Decks
("Restore deck tracks of the last session on startup").

### AI DJ assistant (library sidebar: "AI DJ")
Suggests the next track while you play, and builds sets and playlists for
the event. It works **offline, instantly and without any account**; Gemini
and Jev are optional extras.

- **Event**: club night, wedding ceremony, cocktail hour, dinner, wedding
  reception, birthday, corporate, church service, memorial, lounge, warm-up,
  peak time. Each event has its own energy curve over the set, BPM range and
  fitting genres. Mixxx cannot hear the room, so you tell it what kind of
  event it is. It knows the time and how far into the set you are
  (set length + "Start new set").
- **Suggestions** follow the playing deck and are ranked by harmonic key
  (Camelot), tempo (also half/double time), energy (from BPM, loudness and
  genre, and the mood tags for tracks without BPM), genre continuity, mood,
  your rating, freshness and artist variety (the same artist twice in a row
  is ranked down). Tracks played in this session or loaded in a deck are
  skipped. If many tracks have no BPM or key yet, the page says so: analyze
  them for much better suggestions. Each suggestion shows **why** it fits, e.g.
  "Key 8A → 9A (harmonic) · BPM 124 → 126 (+1.6%) · Energy fits the moment",
  or how to fix a key clash ("key shift +1").
- The **Now playing** card shows BPM, key (in its Camelot color) and energy
  of the playing track, the **energy curve** of the event with where the set
  is now, and the set clock.
- **Harmonic only** hides tracks whose key clashes with the playing track.
- **Load to free deck**, **Add to Auto DJ** (double-click loads).
- **Keep Auto DJ filled** (AI Auto DJ): keeps at least 3 tracks in the Auto
  DJ queue, each one chosen to flow from the last queued track along the
  event's energy curve. Turn on Auto DJ and Mixxx mixes a whole set by
  itself.
- **Transition tip**: concrete mixing advice (EQ swap on the phrase, filter
  sweep, echo out, key shift) for the selected next track. Offline; Gemini
  gives a more detailed tip if configured.
- **Build offline**: chains the best next tracks into a playlist of exactly
  the chosen number of tracks that follows the event's energy curve.
- **Build with Gemini**: describe the set in your own words ("romantic
  entrance for the bride, then uplifting songs for the exit") and Gemini
  picks and orders tracks from your library. Only tracks from your library
  are accepted.
- **Tag moods with Gemini**: adds moods (happy, romantic, energetic, chill,
  spiritual, ...) to your tracks, used by the mood filter. Stored in
  `aidj_moods.json` in the Mixxx settings folder.
- **Ask AI to pick**: Jev (TypeSafe AI) chooses the best next track out of
  the top 20 suggestions in a fraction of a second. Without a Jev key Gemini
  picks and explains why.

**AI settings** (button on the AI DJ page):
- Gemini: free API key from https://aistudio.google.com/apikey. The default
  model `gemini-3.5-flash-lite` has the largest free daily quota (a few
  hundred requests per day). In the EU, UK and Switzerland Google only allows
  paid keys for apps. The model list offers the current Flash and
  Flash-Lite models; **Test connection** checks the key and model before
  saving. For Gemini 3 models Mixxx keeps the default temperature and asks
  for a low thinking level, as Google recommends, so answers stay fast and
  are not cut off.
- Jev: key from https://console.typesafe.ai (only input tokens are billed,
  about $0.04 per million; a choice costs a tiny fraction of a cent). New
  sign-ups were paused in September 2026 and there is no free tier.
- Keys are stored in the Windows Credential Manager, not in files.
- Nothing is sent before you tick "Allow sending ... to these services".
  Only artist, title, genre, BPM, key and moods are sent, never audio or file
  names. On free tiers the providers may use the requests to improve their
  services.

### Free Music Finder (library sidebar: "Free Music Finder")
Search free, legal music by independent artists and download it straight
into the library, while the decks keep playing:

- Sources: the Internet Archive netlabels collection (thousands of releases,
  often lossless FLAC) and ccMixter (with BPM), or **All sources** at once.
  No account is needed.
- Downloads run in the background, one after another, with progress. The
  tracks go to `Music\Mixxx Downloads` (changeable) and are added to the
  library automatically.
- **Download & preview** (double-click) plays the track in the headphones
  (preview deck), **Download & load to free deck**, **Download & add to Auto
  DJ**.
- Every track shows its **license**. "Only licenses allowed at paid gigs" is
  on by default: NC (non-commercial) tracks are not allowed at paid gigs, ND
  (no derivatives) tracks should not be in published mixes. Most netlabel
  releases are NC, so with this option the Internet Archive is asked only for
  releases that allow commercial use. Tracks allowed at paid gigs are listed
  first. The license and source are saved in the track's comment.
- **Copy credits** copies "title by artist – license – source" of the
  selected tracks, e.g. for the description of a published mix.
- The status column shows the download progress as a bar.
- Commercial hits are not available for free download. Buy them (Beatport,
  Bandcamp, ...) and add the folder to the library. Jamendo is not included
  because its API terms forbid offline downloads. Ripping YouTube or
  streaming services is illegal and not supported.

## Where the code is

| Feature | Code |
|---|---|
| Beat counter | `src/engine/controls/clockcontrol.cpp` |
| Pitch Play | `src/engine/controls/pitchplaycontrol.cpp` |
| Smooth tempo reset | `src/engine/controls/ratecontrol.cpp` |
| Limiter | `src/engine/enginelimiter.cpp`, `src/engine/enginemixer.cpp` |
| Deck restore | `src/mixer/playermanager.cpp`, `src/coreservices.cpp` |
| Phase meter | `src/widget/wphasemeter.cpp` |
| Skin (LateNight) | `res/skins/LateNight/decks/`, `toolbar.xml`, `style.qss` |
| AI DJ recommender (offline) | `src/library/aidj/djrecommender.cpp` |
| Gemini / Jev clients, key storage | `src/library/aidj/aiservice.cpp` |
| AI DJ page | `src/library/aidj/dlgaidj.cpp` |
| Look of the AI DJ pages, energy curve, meters | `src/library/aidj/aidjwidgets.cpp` |
| Music Finder sources and downloads | `src/library/aidj/musicsources.cpp`, `dlgmusicfinder.cpp` |
| Tests | `src/test/profeatures_test.cpp`, `src/test/aidj_test.cpp` |

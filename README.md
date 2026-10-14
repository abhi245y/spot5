<p align="center">
  <img src="docs/icon.png" width="128" alt="Spot5 icon">
</p>

<h1 align="center">Spot5</h1>

<p align="center">
  A native, standalone Spotify app for jailbroken PlayStation 5 consoles.<br>
  Browse and play your music on the TV with the controller — no PSN, and no phone needed after the first login.
</p>

<p align="center">
  <img src="https://img.shields.io/badge/platform-PS5%20(homebrew)-1ed760" alt="Platform: PS5 homebrew">
  <img src="https://img.shields.io/badge/output-native%204K-1ed760" alt="Native 4K">
  <img src="https://img.shields.io/badge/license-GPL--3.0--or--later-blue" alt="License: GPL-3.0-or-later">
</p>

<p align="center">
  <img src="docs/screenshots/home.jpg" alt="Spot5 Home screen" width="900">
</p>

---

## Features

**Your music, on the TV**
- **Home** — your personal shelves, mixes and recently played, with full-bleed artist artwork and morning/evening shelves in your time zone.
- **Your Library** — playlists, albums and artists (Liked Songs pinned first), filterable and sortable.
- **Playlist and album pages** — the full track list; play from any song, or shuffle.
- **Artist pages** — wide header art, popular tracks, biography, albums and singles.
- **Now Playing** — large artwork, a live visualiser, seeking with the D-pad and triggers, and the queue beside it.

**Built for the couch**
- A standalone app driven entirely by the controller, like any other app on the console.
- Native 4K rendering (or 1440p / 1080p), designed for a controller from the ground up.
- A mini player that follows you between screens and docks under the cover on playlist pages.
- Streams at 96, 160 or 320 kbps; covers and Home are cached, so the app opens instantly.
- Hold the touchpad for 3 seconds on any screen to quit; a reduced-motion mode calms every animation.
- Registers as a media app on the console's home screen.

**Sign in once, then forget the phone**
- The first time, log in by choosing **PS5** in the Spotify app on your phone. Spot5 keeps the login and signs in by itself from then on.

**Spotify Connect as a bonus**
- Spot5 also appears as a speaker in Spotify on your phone, tablet or computer, so you can start or control music from there when you like.

## Screenshots

| | |
|:---:|:---:|
| <img src="docs/screenshots/launch.jpg" alt="Launch screen"> | <img src="docs/screenshots/pairing.jpg" alt="Pairing with a phone"> |
| Launch | First-time pairing |
| <img src="docs/screenshots/playlist.jpg" alt="Playlist page"> | <img src="docs/screenshots/now-playing.jpg" alt="Now Playing"> |
| Playlist page | Now Playing with the queue |
| <img src="docs/screenshots/library.jpg" alt="Your Library"> | <img src="docs/screenshots/artist.jpg" alt="Artist page"> |
| Your Library | Artist page |
| <img src="docs/screenshots/settings.jpg" alt="Settings"> | <img src="docs/screenshots/home.jpg" alt="Home"> |
| Settings | Home |

## Requirements

- A jailbroken PS5 that can run homebrew apps (tested on firmware 13.00), with:
  - a homebrew launcher that registers apps from `/data/homebrew` (for example etaHEN or ShadowMountPlus);
  - an ELF loader listening on port **9021** — Spot5 uses it to start its small pairing helper the first time you pair;
  - FTP access to the console, to copy the app across.
- A **Spotify Premium** account.
- A phone (or computer) with the Spotify app, on the **same network** as the PS5.

## Installation

1. **Get the app folder.** Download `PPSA99061.zip` from the [Releases](../../releases) page and unzip it, or [build it yourself](#building-from-source).
2. **Copy it to the console.** Upload the `PPSA99061` folder to `/data/homebrew/PPSA99061` over FTP.
   From a source checkout this is one command: `make deploy PS5_HOST=<console IP>`.
3. **Register it.** Use your homebrew launcher to register the folder. A **Spot5** tile appears on the home screen.
4. **Launch Spot5** and wait for *Pair with your phone*.
5. **Pair.** On your phone, open Spotify, tap the devices icon and choose **PS5**. Spot5 signs in and opens Home.

That's it — from now on Spot5 signs in by itself. To sign out, open **Settings › Account › Forget this login**.

> **Note:** deleting the Spot5 tile from the home screen also deletes its folder on the console, including the saved login and settings. You'll need to pair again after reinstalling.

### Optional configuration

Spot5 needs no configuration file. To override the Spotify client ID it uses, place a `config.json` next to the app on the console:

```json
{ "client_id": "<your client id>" }
```

## Using Spot5

| Where | Button | Action |
|---|---|---|
| Anywhere | Touchpad (tap) | Open / close Settings |
| Anywhere | Touchpad (hold 3 s) | Quit Spot5 |
| Home, Library | L1 / R1 | Switch between Home and Your Library |
| Home, Library | R3 | Refresh |
| Home, Library, pages | △ | Open Now Playing |
| Your Library | L2 / R2, □ | Filter, sort |
| Playlist page | ✕ / L1, R1 | Play from the selected song / scroll a page |
| Now Playing | ✕ | Play / pause |
| Now Playing | ← / →, L2 / R2 | Seek 5 s, 15 s |
| Now Playing | □ | Show / hide the queue |
| Settings | L2 / R2 | Previous / next category |
| Everywhere | ◯ | Back |

### Settings

- **Playback** — audio quality, volume, and whether your phone's volume buttons control the console.
- **Device** — the name shown in Spotify's device list, and the time zone for Home (automatic from the console, or chosen from a list).
- **Account** — who is signed in, and *Forget this login*.
- **Display** — resolution (1080p, 1440p, 4K) and reduce motion.
- **About** — version details, *Clear cache* and *Reset all settings*.

## Known limitations

- Music stops when you start a game: the console suspends media apps while a game runs.
- Spotify DJ can't be played from the console yet.
- Song and artist names with accented letters may show `?` in place of those letters.

## Roadmap

Spot5 grows in small releases, each tested on a console before it ships. The order puts groundwork first (speed and the shared song menu many later features build on), then the biggest everyday gaps, then the showpieces.

### v1.0.0 — first release ✅
A standalone app in native 4K: Home, Your Library, playlist, album and artist pages, Now Playing with queue and scrubber, settings and hold-to-quit; one-time login from your phone, and Spotify Connect.

### v1.1 — Polish and speed
- [ ] Button hints drawn in Spot5's own style on every screen
- [ ] Faster artwork: smarter cover fetching, caching and decoding
- [ ] Accented and non-Latin letters in titles and names

### v1.2 — The song menu
- [ ] An options menu for any song — from playlists, albums, artist pages, the queue and Now Playing
- [ ] Go to the song's album or artist
- [ ] Add to Liked Songs, or to one of your playlists
- [ ] Start a song radio
- [ ] Song details (album, release date, credits), with links between song, album and artist pages

### v1.3 — Search
- [ ] Search songs, artists, albums and playlists with the on-screen keyboard
- [ ] Recent searches and quick results while you type

### v1.4 — Profiles and accounts
- [ ] Your profile picture across the app
- [ ] More than one Spotify account on one console, switchable from Settings

### v1.5 — Lyrics
- [ ] Time-synced lyrics on Now Playing

### v1.6 — Canvas
- [ ] The short looping videos some songs show, behind Now Playing

## Releases and branches

- `main` always holds the latest tested code; every release is a tag on it (`v1.0.0`, `v1.1.0`, ...) with a [GitHub Release](../../releases) carrying the ready-to-install `PPSA99061.zip`.
- Each roadmap item is built on its own `feature/<name>` branch and merged into `main` through a pull request once it works on a console.
- Version numbers follow [Semantic Versioning](https://semver.org/): new features raise the minor number, fixes the patch number. The console shows the same version in Settings › About.

## Building from source

Spot5 builds on Linux (or WSL) and fetches its PS5 toolchain itself.

**Prerequisites:** Clang (18 or newer; set `PS5_CLANG` to choose one), `llvm-ar`, `ninja`, `make`, `python3`, `curl`, `wget`, `unzip`, `tar` and `sha256sum`.

```sh
git clone <this repository> spot5
cd spot5
make                                   # SDKs, dependencies, app and pairing helper
make deploy PS5_HOST=192.168.1.2       # copy to /data/homebrew/PPSA99061 over FTP
```

The first `make` downloads the pinned [PS5 payload SDK](https://github.com/ps5-payload-dev/sdk) and the ps5-opengl SDK into `.deps/`, then builds mbedTLS, libwebp and the vendored cspot/bell libraries. The result is `dist/PPSA99061/` and `dist/PPSA99061.zip`.

**Developing without a console:** `tools/preview.sh` renders every screen on the PC (Mesa, no GPU needed) to `build/preview/`. Host tests for the Home and Library parsers and for the pairing helper are described at the top of the files in `tests/`.

### Repository layout

| Path | What's there |
|---|---|
| `src/app/` | App start-up, the controller thread, configuration and settings |
| `src/service/` | The Spotify side: session, Home, Library, pages, covers and pairing |
| `src/screens/` | Every screen: launch, Home, Library, playlist, artist, Now Playing, Settings |
| `src/ui/`, `src/gfx/`, `src/audio/` | The UI kit, renderer and audio output |
| `helper/` | The pairing helper, started through the ELF loader during first-time pairing |
| `third_party/cspot/` | The cspot and bell sources Spot5 is built on (see `VERSION`) |
| `assets/`, `sce_sys/` | Fonts, logo, certificates, icon and the app's `param.json` |
| `tools/` | Build, deploy, preview and icon scripts |

## Credits

Spot5 stands on the work of many open-source projects. Thank you to their authors and contributors:

- [**cspot**](https://github.com/feelfreelinux/cspot) by feelfreelinux and contributors, and its maintained fork by [philippe44](https://github.com/philippe44/cspot) — the Spotify Connect engine (GPL-3.0).
- [**bell**](https://github.com/philippe44/bell) — cspot's networking, TLS and HTTP layer, with
  [nanopb](https://github.com/nanopb/nanopb) (zlib),
  [nlohmann/json](https://github.com/nlohmann/json) (MIT),
  [CivetWeb](https://github.com/civetweb/civetweb) (MIT),
  [Tremor](https://xiph.org/vorbis/) by Xiph.org (BSD),
  [picohttpparser](https://github.com/h2o/picohttpparser) (MIT),
  [tiny-AES-c](https://github.com/kokke/tiny-AES-c) (public domain) and
  [tinysvcmdns](https://github.com/geekman/tinysvcmdns) by Darell Tan (BSD), used by the pairing helper.
- [**librespot**](https://github.com/librespot-org/librespot) — the public client ID Spot5 uses by default.
- [**ps5-homebrew-ui**](https://github.com/blackbearreloaded/ps5-homebrew-ui) and the
  [**PS5 Native App Boilerplate**](https://github.com/blackbearreloaded/ps5-native-app-boilerplate) by BlackBearReloaded — the UI kit, renderer and native app foundation (GPL-3.0).
- [**ps5-opengl**](https://github.com/blackbearreloaded/ps5-opengl) with [Mesa](https://mesa3d.org/) — OpenGL 4.6 on the PS5.
- [**PS5 payload SDK**](https://github.com/ps5-payload-dev/sdk) and [**elfldr**](https://github.com/ps5-payload-dev/elfldr) by John Törnblom and contributors.
- [**SharpProspero**](https://github.com/SvenGDK/SharpProspero) by SvenGDK — the basis of the ELF converter in `tooling/native/`.
- [**Mbed TLS**](https://github.com/Mbed-TLS/mbedtls) (Apache-2.0) and [**libwebp**](https://chromium.googlesource.com/webm/libwebp) (BSD).
- [**stb**](https://github.com/nothings/stb) by Sean Barrett (public domain / MIT).
- Typefaces [Inter](https://github.com/rsms/inter), [Montserrat](https://github.com/JulietaUla/Montserrat) and [DejaVu](https://dejavu-fonts.github.io/) (SIL OFL / Bitstream Vera).
- Inigo Quilez's [2D distance functions](https://iquilezles.org/articles/distfunctions2d/) (MIT), used by the shape shader.

Full licence details are in [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).

## License

Spot5 is free software, released under the [GNU General Public License v3.0 or later](LICENSE).

Spot5 is an independent project for personal use. It is not affiliated with, endorsed by or sponsored by Spotify AB or Sony Interactive Entertainment. Spotify is a trademark of Spotify AB; PlayStation and PS5 are trademarks of Sony Interactive Entertainment Inc.

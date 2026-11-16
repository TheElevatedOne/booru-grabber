# Changelog

## 1.0.0

First release of Booru Grabber, a C++17 / GTK 3 rewrite of [Grabber](https://github.com/Bionus/imgbrd-grabber)
7.14.0 without Qt.

### Added

- GTK 3 interface with Grabber's layout: search tabs with a tags / favorites / view later / wiki sidebar,
  Favorites, Downloads, Monitors and Log tabs, image viewer, site and settings dialogs. Colours follow the
  GTK theme, corners are square and the window uses the compositor's decorations.
- Core library `libbooru-grabber.so` (GLib, libsoup 3, quickjs-ng, lexbor, libxml2, SQLite) shared by the
  GUI and the CLI.
- `booru-grabber-cli` with the options of `Grabber-cli` (`-t -s -p -m -i -l -f`, `--return-count`,
  `--return-tags`, `--return-pure-tags`, `--return-images`, `--download`, `--json`, `--load-details`,
  `--get-details`, `--load-tag-database`), plus `--list-sites`, `--profile`, `--portable` and
  `--import-grabber`.
- Three install modes from one build: an FHS install (`/usr/bin`, `/usr/lib`, `/usr/share/booru-grabber`,
  `/etc/booru-grabber`), a standalone directory (`packaging/make-portable.sh`, profile kept next to the
  program) and an AppImage (`packaging/build-appimage.sh`).
- Settings in `$XDG_CONFIG_HOME/booru-grabber`, data in `$XDG_DATA_HOME/booru-grabber`, cache in
  `$XDG_CACHE_HOME/booru-grabber`, logs in `$XDG_STATE_HOME/booru-grabber`, system defaults in
  `/etc/booru-grabber/settings.ini` and `$XDG_CONFIG_DIRS`.
- `--import-grabber DIR` and *File → Import Grabber profile…* copy settings, favorites, lists, the MD5
  database, monitors, per-site settings and tag databases from a Grabber profile. Nothing is imported
  automatically and the old profile is never modified.
- Video playback through GStreamer (no GTK video sink plugin needed), optional tray icon through
  libayatana-appindicator, desktop notifications for monitors.
- `pixiv://` links are handled by the desktop file, so Pixiv's PKCE login works on Linux (it was Windows only).
- File type detection for WebP, AVIF, HEIC, JPEG XL and ZIP when fixing wrong extensions.
- Sources are compiled with TypeScript 5.9 or 6 (ES2020 target) and checked by the C++ test suite against
  their sample pages.

### Changed

- Downloads without a configured folder go to `$XDG_PICTURES_DIR/Booru Grabber` instead of the working directory.
- Files being viewed are stored in the cache directory instead of `$XDG_RUNTIME_DIR` (which is in RAM).
- The download list (`restore.igl`), favorites, monitors and history keep Grabber's JSON formats; the
  settings keep its INI format.
- A single HTTP session with a persistent cookie jar and a 64 MB HTTP cache replaces the per-site Qt
  network managers; per-site concurrency and throttling are kept.

### Fixed

- HTML pages with non-ASCII text were truncated before parsing (UTF-16 lengths passed for UTF-8 data), and
  every parsed page, CSS selector and text extraction leaked memory.
- Comma-separated CSS selectors (`.cn, .cs`) returned the same node several times.
- Pages, image details and images that got HTTP 429/503/509 were retried forever; they now retry up to 3–5 times.
- Downloads hung when a site had no details API (loading the details never finished).
- Downloading a search looped forever on sites that ignore the page parameter; result packs now stop when a
  page only returns images already seen.
- Logging in again or reloading a page connected its handlers once more each time, running them several times.
- The concurrency counter of the network queue could exceed the limit or stall the queue.
- Monitors saved their downloads in the working directory (the path was read from `save/path` instead of `Save/path`).
- URL login fields (API keys, passwords) and OAuth2 refresh requests were not URL-encoded.
- OAuth2: the refresh token's validity was checked against the access token's expiration, token expirations
  inside JWTs were never read (base64 instead of base64url), and tokens were written to the log.
- `md5:` request headers hashed the prefix along with the value.
- Tag database lookups built SQL from tag names, failing on quotes.
- Advanced post-filters and blacklists using JavaScript crashed; `%token%:value` filters wrote to a stray
  default settings file.
- The thumbnail colour of images without a parent page (restored downloads) crashed the program.
- `%num:noext%` picked a wrong number when existing files had another extension, and kept tokens lost the
  separators between their options.
- Dates with half-hour offsets (`+0530`) were shifted by 5.3 hours.
- File sizes written as `KiB`/`MiB` were not understood by filters.
- Failed copies and cross-filesystem moves were reported as saved.
- Tag commands applied SQL escaping to shell commands and none to SQL queries.
- The main window could not be narrower than about 1580 px, and the sidebar could collapse to an invisible 1 px
  strip that was saved and restored on every start.
- "Keep N, then add text" always kept one tag: the settings had no field for its own N.
- Images with medias that only give a width (booru.io) got no file URL, and result packs treated every image
  without an ID or MD5 as the same image.
- Sites were contacted with empty login forms when no account was configured.
- Loading the next page of a search on sites that page by cursor (e-hentai) failed with "You need valid previous
  page information": the page URL was computed before the previous page was known.
- rule34.paheal.net returned no images after a change of its thumbnail markup, and returned MD5s with a leading `/`.
- zerochan.net answered every request with its browser check (a project User-Agent is now sent).
- safebooru.org, yande.re, konachan.com, rule34.paheal.net, rule34.us, kemono.cr, pawoo.net, Newgrounds and the
  Danbooru mirrors were contacted over HTTP first.

### Removed

- Windows, macOS and Android builds, the QML interface, the crash reporter, translations and Qt themes.
- Usage statistics (Google Analytics), the program updater and source registries (sources ship with the program).
- MySQL/PostgreSQL commands (SQLite databases remain), OAuth 1 (no source used it), Windows file properties,
  and the "shut down / open CD tray after downloads" options.
- The Gfycat, Hentai Cosplays, Sankaku, Slushe, Tumblr and Twitter sources, which were already disabled upstream.
- The behoimi.org and tentaclerape.net presets: both sites are gone.

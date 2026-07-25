# text2sprech — local TTS reader with live highlighting

Implements the v1 scope from the proposal: open text/HTML or paste from
clipboard, sentence-by-sentence Piper TTS with pipelined lookahead, live
sentence highlighting, play/pause/seek/prev/next, voice + speed selection.
See `PROTOCOL.md` for the wire protocol between the Qt app and the Piper
server.

## Status / what's actually been tested

This was built in a sandbox with **no Qt6, no CMake, and no network
access**, so here's the honest breakdown:

| Component | Status |
|---|---|
| `piper_server/` (Python) | **Tested end-to-end** — server + protocol framing + a real socket client, see below. Uses a `SilentTestBackend` (silent WAVs) because no Piper install/models are available in this sandbox; falls back automatically to a real Piper CLI or Python backend if either is present on your machine. |
| `sentence_splitter.py` | **Tested** against abbreviations/decimals/initials, newline-forced breaks, blank-line paragraph breaks, max-length subdivision (both a whitespace-breakable long run and a hard, unbreakable one), and dropping punctuation/symbol-only junk segments. |
| `synth.py`'s `PiperCliBackend` fallback behavior | **Tested** against a fake `piper` binary reproducing the real crash: confirmed unspeakable text is pre-filtered without invoking the CLI at all, confirmed the CLI-crash-despite-passing-the-filter case degrades to silence with a logged warning rather than raising, and confirmed a genuinely different failure (missing voice model) still raises normally. |
| C++ (`src/`, `tests/`) | **Written but not compiled** — no Qt6/CMake toolchain available here. `SentenceSplitter.cpp` mirrors the tested Python logic; `tests/test_sentence_splitter.cpp` exists so you can verify it the moment you build. `PiperServerManager` (spawns/stops the server, unique per-instance socket) and the CLI options are new and also unverified by a compiler yet. Everything else (PiperClient, PlaybackController, DocumentView, MainWindow) is unverified by a compiler — expect the normal first-build friction (a missed include, a signal/slot signature mismatch) rather than fundamental design bugs, since the interfaces were kept deliberately small and single-purpose. |
| `resources/` (icon, `.qrc`, `.desktop`) | **Not verified visually** — no display/desktop environment in this sandbox to confirm the icon actually renders in a taskbar or app menu, or that the `.desktop` file's `Exec`/`Icon` fields resolve correctly once installed. The `.qrc` path (`:/icons/text2sprech.png`) and file structure follow standard Qt/freedesktop.org conventions, but worth a visual check after your first `cmake --install` or `nix build`. |
| `flake.nix` | **Not built** — no network here to fetch nixpkgs. There's one known real gap: the Piper Python package isn't reliably in nixpkgs, so `piper_server`'s empty `pythonEnv` relies on the CLI-binary fallback in `synth.py` unless you vendor a real package (flake.nix has a TODO with the two concrete options). |

## Running it

```bash
# Build (once you have Qt6 + CMake):
cmake -B build -G Ninja
cmake --build build

# Just run it -- piper-server is started and stopped automatically:
./build/text2sprech
```

The app now manages the Piper server's lifecycle itself (`PiperServerManager`,
in `src/speech/`): on startup it generates a unique socket path (PID + UUID,
under the OS temp dir) and spawns `piper-server` pointed at it, so multiple
instances of the app can run at once without colliding. On clean shutdown it
sends SIGTERM to the child (which `server.py`'s own signal handler turns into
a graceful stop), escalating to SIGKILL if it doesn't exit within ~2s.

It locates the server to spawn in this order: `$TEXT2SPRECH_PIPER_SERVER_BIN` if
set, then a `piper-server` executable on `PATH` (what the flake's
`piper-server` package + `qtWrapperArgs` provide once installed), then
falling back to `python3 piper_server/server.py` using the source-tree path
baked in at CMake configure time (dev-build convenience only).

### Command-line options

```
./build/text2sprech [options] [file]

--voice <name>       Select this voice on startup (must match one the server
                     reports; falls back to the server default with a stderr
                     warning if not found).
--speed <factor>     Initial playback speed, 0.5-3.0 (default 1.0).
--autoplay           Start playback automatically as soon as a document is
                     loaded (via Open…, Paste, or the [file] argument).
--piper-socket <path> Connect to an already-running piper-server at this
                     socket instead of spawning one. The app will not stop
                     a server given this way -- you own its lifecycle.
--paste-clipboard    Load the clipboard's contents on startup (HTML if
                     available, plain text otherwise). Takes priority over
                     [file] if both are given.
--paste-selection    Load the primary selection (X11/Wayland-with-support
                     only) on startup. Takes priority over --paste-clipboard
                     and [file] if more than one is given.
[file]               Optional text/HTML file to open on startup. Deferred
                     until the piper-server connection is actually up, so
                     --autoplay doesn't race the async socket connect.
```

Example: `./build/text2sprech --voice en_US-lessac-medium --speed 1.25 --autoplay book.txt`

Example attaching to your own server: start `python3 piper_server/server.py
--socket /tmp/my-piper.sock` yourself, then `./build/text2sprech --piper-socket
/tmp/my-piper.sock`.

The toolbar has separate "Paste Clipboard" and "Paste Selection" buttons
(the latter disabled with a tooltip on platforms without primary-selection
support, i.e. Windows/macOS).

### Config file

Defaults for voice, speed, and text zoom level can be set at (Linux)
`~/.config/text2sprech/text2sprech.ini` (a plain text file, forced to that format on
every platform rather than the Windows registry or macOS plist QSettings
would otherwise use) so you don't have to pass the same CLI flags every
time. CLI flags always override the config file when given.

```ini
[general]
voice=en_US-lessac-medium
speed=1.25
textZoomSteps=2
```

`textZoomSteps` is the net number of `DocumentView::increaseTextSize()`
steps to apply on startup (negative to start smaller); there's no CLI flag
for it, config-file only for now. Invalid values (out of range, wrong type)
are ignored with a warning printed to stderr, falling back to the default.

Or via the flake, once you can reach the network to resolve nixpkgs:

```bash
nix develop
nix build .#text2sprech
nix run .#piper-server &
nix run .#text2sprech
```

## Project layout

```
text2sprech/
├── flake.nix
├── CMakeLists.txt
├── PROTOCOL.md              # wire protocol between text2sprech and piper-server
├── resources/
│   ├── text2sprech.png       # app icon (512x512), embedded via qrc + installed to hicolor theme
│   ├── text2sprech.qrc       # Qt resource file (AUTORCC) embedding the icon into the binary
│   └── text2sprech.desktop   # Linux desktop entry (app menu, file-manager "Open With")
├── piper_server/
│   ├── server.py            # asyncio unix-socket server
│   ├── protocol.py           # framing (tested)
│   ├── synth.py              # backend abstraction: python-api / cli / silent-test
│   └── sentence_splitter.py  # reference splitter (tested)
├── src/
│   ├── main.cpp
│   ├── ui/
│   │   ├── MainWindow.{h,cpp}
│   │   └── DocumentView.{h,cpp}
│   ├── document/
│   │   ├── Sentence.h
│   │   └── SentenceSplitter.{h,cpp}   # C++ port of the tested Python logic
│   └── speech/
│       ├── PiperClient.{h,cpp}        # protocol client (QLocalSocket)
│       ├── PiperServerManager.{h,cpp} # spawns/stops the piper-server child process
│       └── PlaybackController.{h,cpp} # sentence queue, lookahead, QMediaPlayer
└── tests/
    └── test_sentence_splitter.cpp
```

## Design notes / deviations from the original proposal

- **Protocol is JSON + 4-byte length prefix, not line-based text.** The
  proposal's `SPEAK 123 <text>` / `AUDIO 123 <data>` sketch breaks on text
  containing newlines and can't safely carry binary audio. See
  `PROTOCOL.md` for the full schema and rationale.
- **Speed is applied at synthesis time (Piper's `length_scale`), not by
  speeding up playback.** Naive playback-rate changes on WAV audio distort
  pitch; baking speed into synthesis avoids that at the cost of a fresh
  request when speed changes mid-sentence.
- **Sentence splitting is a heuristic**, not a full tokenizer. It handles
  common abbreviations, decimal numbers, and single-letter initials, but
  unusual abbreviations or deeply nested quotes can still split wrong.
  Newlines are always a hard boundary (independent of punctuation), and any
  segment still longer than `max_len` (280 chars by default) with no natural
  break gets further subdivided at whitespace -- see the docstring in
  `piper_server/sentence_splitter.py` for the tradeoffs this introduces
  (hard-wrapped text files get chopped per line).
- **Every sentence gets a background tint** in `DocumentView`, alternating
  between two yellow-family shades (pale butter yellow / warmer gold,
  tuned separately for light and dark themes). An earlier version of this
  only tinted every other sentence and left the rest at the plain
  background, which showed up as plain white in a light theme -- fixed so
  every sentence is now colored.
- **Config file** at (Linux) `~/.config/text2sprech/text2sprech.ini` lets you set
  defaults for voice, speed, and text zoom level without passing CLI flags
  every time. CLI flags always override it when given -- see the
  `[general]` section format below.
- **Clicking a sentence highlights it immediately**, independent of
  playback state -- previously the highlight only appeared once audio
  actually started, so clicking while paused looked like it did nothing.
- **Text size** is adjustable via toolbar buttons or Ctrl+=/Ctrl+-/Ctrl+0,
  using `QTextEdit::zoomIn`/`zoomOut` (which scales all font sizes
  proportionally, so HTML headings stay bigger than body text). The
  cumulative zoom level is reapplied automatically whenever a new document
  is loaded, since zoom only affects text already in the document at the
  time it's called.
- **Switching voices used to "stick" with the old voice for a sentence or
  two.** The bug: the voice combo was wired directly to `PiperClient`,
  which only updates the server's voice for *future* `speak` requests --
  sentences already prefetched (lookahead) or mid-synthesis under the old
  voice kept arriving and playing regardless. Fixed by routing voice
  changes through `PlaybackController::setVoice()`, which cancels
  in-flight/queued synthesis and discards cached-but-unplayed audio before
  switching, so only the sentence already actively playing (audio already
  flowing to the device) finishes in the old voice.
- **HTML rendering relies on Qt's built-in subset** (`QTextDocument::setHtml`),
  which covers headings/paragraphs/emphasis/line breaks as scoped, but is
  not a full browser engine.
- **Active-sentence highlight is now a fixed amber background with
  near-black text**, not the default `palette().highlight()`/
  `highlightedText()` (blue/white), which was hard to read -- especially
  layered over the yellow tint stripes underneath.
- **Auto-scroll centers the active sentence** in the viewport
  (`DocumentView::centerCursor()`, computed from `cursorRect()` and the
  vertical scrollbar) rather than just nudging it into view at an edge,
  which is all the default `ensureCursorVisible()` does.
- **Status bar now has two permanent regions**: the left/normal area shows
  the current activity ("Starting piper-server…", "Preparing audio…",
  "Playing", "Paused", "Stopped", etc. -- see `MainWindow::setActivity()`),
  temporarily covered by transient notifications (errors, "Loaded N
  sentences") the same way `QStatusBar::showMessage()` always covers normal
  widgets. The right/permanent area (`MainWindow::refreshInfoLabel()`)
  shows a rough total length ("~1,234 words, ~8 min", scaled by the current
  speed) and current position ("word 456 (37%)"), based on a simple
  whitespace word count and a fixed 150 words/minute baseline -- both
  explicitly approximate, not meant to be precise.
- **Punctuation/symbol-only "sentences" (e.g. leftover `---`, `***`, stray
  HTML-derived junk) are dropped before ever being sent to Piper.** Piper's
  own text normalization reduces text with no letters or digits at all to
  zero phonemes, then crashes trying to write a WAV header for zero frames
  (a confusing `wave.Error: # channels not specified`, not a clean failure).
  Both `sentence_splitter.py` and `SentenceSplitter.cpp` filter these out at
  the source (see `_is_speakable`/`hasSpeakableContent`), and `synth.py`'s
  `PiperCliBackend` also checks before invoking Piper and degrades to a
  brief silent WAV (with a logged warning, not a client-visible error) if
  Piper still produces empty/malformed output despite that -- e.g. text
  that looks speakable but trips up Piper's phonemizer in some other way.
  A genuinely different failure (bad model path, Piper crashing for an
  unrelated reason) still raises normally.
- **Renamed from "reader" to "text2sprech"** (binary, window title,
  project/organization name used for the config file and `QSettings` path,
  Nix package/app names, socket-name prefix). Also added a proper Linux
  desktop entry (`resources/text2sprech.desktop`) and icon: the icon is
  both embedded into the binary via Qt resources (`resources/text2sprech.qrc`,
  works even when run directly without a desktop launcher) and installed
  to the `hicolor` icon theme at `share/icons/hicolor/512x512/apps/` (works
  for icon-theme lookup from the `.desktop` file, app menus, etc.).

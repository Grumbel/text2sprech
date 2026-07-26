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
| C++ (`src/`, `tests/`) | **Written but not compiled** — no Qt6/CMake toolchain available here. `SentenceSplitter.cpp` mirrors the tested Python logic; `tests/test_sentence_splitter.cpp` exists so you can verify it the moment you build. `PiperServerManager` (spawns/stops the server, unique per-instance socket), `PreferencesDialog`, and the CLI options are new and also unverified by a compiler yet. Everything else (PiperClient, PlaybackController, DocumentView, MainWindow) is unverified by a compiler — expect the normal first-build friction (a missed include, a signal/slot signature mismatch) rather than fundamental design bugs, since the interfaces were kept deliberately small and single-purpose. |
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
support, i.e. Windows/macOS). The document view also supports **Ctrl+V**
(pastes clipboard) and **middle-click** (pastes the primary selection, the
X11/Wayland convention) directly -- both replace the whole document, the
same as the toolbar buttons, since this is a single-document read-only
view rather than a text box you type into.

### Config file

Defaults for voice, speed, and font (family + point size) live at (Linux)
`~/.config/text2sprech/text2sprech.ini` (a plain text file, forced to that format on
every platform rather than the Windows registry or macOS plist QSettings
would otherwise use). **The Preferences dialog (toolbar button) is the
only thing that writes to this file.** It shows and edits those saved
startup defaults only -- not the live toolbar values -- and saving does
not change the current session. CLI flags (`--voice`, `--speed`) and the
regular toolbar controls (voice combo, speed spinbox, A-/A+) are
session-only by design: they override the config file's defaults for the
current run but never modify it. CLI flags still win over the config file
for the current session when given.

```ini
[startup]
voice=en_US-lessac-medium
speed=1.25
fontFamily=Noto Sans
fontPointSize=13
```

`fontFamily` empty or absent leaves the widget's platform-default family
alone; `fontPointSize` <= 0 or absent leaves its default size alone.
Invalid `fontPointSize` values (outside 6-72, wrong type) are ignored with
a warning printed to stderr, falling back to the default.

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
│   │   ├── DocumentView.{h,cpp}
│   │   └── PreferencesDialog.{h,cpp}
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
- **Sentence text stays plain** in `DocumentView` -- no static background
  tint on every segment. Only the currently playing / click-selected
  sentence gets the amber highlight; hovering the mouse over any other
  sentence applies a lighter yellow hover highlight so segment boundaries
  are still discoverable without painting the whole document.
- **Config file** at (Linux) `~/.config/text2sprech/text2sprech.ini` lets you set
  defaults for voice, speed, and text zoom level without passing CLI flags
  every time. CLI flags always override it when given -- see the
  `[general]` section format below.
- **Clicking a sentence highlights it immediately**, independent of
  playback state -- previously the highlight only appeared once audio
  actually started, so clicking while paused looked like it did nothing.
- **Text size** started as `QTextEdit::zoomIn`/`zoomOut`-based relative
  zoom, then got replaced with a real font family + point size
  (`DocumentView::setBaseFont()`, applied via `QTextDocument::
  setDefaultFont()`) -- see the Preferences dialog note below for why.
  Toolbar A-/A+ (and Ctrl++/Ctrl+-) now do quick ±1pt adjustments on the
  actual point size rather than an abstract zoom step; there's no
  "reset" shortcut anymore since there's no longer a single well-defined
  "zero" to reset to; open Preferences to set an exact size instead.
  `setDefaultFont()` persists across `loadPlainText()`/`loadHtml()` calls
  (they reuse the same `QTextDocument`, never replace it), so unlike the
  old zoom approach this doesn't need to be reapplied after every load.
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
- **Active-sentence highlight is a fixed amber background with near-black
  text**, not the default `palette().highlight()`/`highlightedText()`
  (blue/white), which was hard to read. The first attempt at this fix
  still showed blue/white despite setting those colors on the
  `ExtraSelection` -- the actual bug was that `highlightRange()` also
  called `setTextCursor()` with that same range as a real selection
  (anchor != position), and Qt paints the widget's *native* selection
  (always palette Highlight/HighlightedText) on top of `ExtraSelection`s
  regardless of their configured colors. Fixed by never giving the widget
  a real selection at all -- `cursorRect()` works on any cursor, not just
  the widget's actual one, so centering doesn't need one either.
- **Auto-scroll centers the active sentence** in the viewport
  (`DocumentView::centerCursor()`, computed from `cursorRect()` and the
  vertical scrollbar) rather than just nudging it into view at an edge,
  which is all the default `ensureCursorVisible()` does.
- **Cursor-key sentence navigation** (Left/Right) is handled directly in
  `DocumentView::keyPressEvent()`, not as app-level `QAction` shortcuts.
  Plain arrow keys are consumed by whichever widget has focus before
  reaching the shortcut system, so a `QAction` shortcut for `Key_Left` on
  `MainWindow` silently loses to `QTextEdit`'s own character-cursor
  movement whenever the document has focus -- which is most of the time.
  Repurposing the keys entirely in `DocumentView` (this is a read-only
  reader, not an editor, so fine-grained character-cursor movement isn't
  needed) and having it emit `previousSentenceRequested()`/
  `nextSentenceRequested()` instead reaches `MainWindow`'s new shared
  `jumpToSentence()` reliably. That same helper backs click-to-jump too,
  so both paths highlight immediately, update the position label, and seek
  playback the same way.
- **Toolbar cleanup**: Previous/Next sentence buttons and the "Reset size"
  button were dropped since the same functionality is already reachable
  elsewhere (clicking any sentence or the arrow keys; an exact size is one
  Preferences dialog away). Added a "Voice:" label since the combo box
  didn't otherwise say what it was for. ("Paste Clipboard"/"Paste
  Selection" were briefly combined into one grouped button and then
  reverted back to two separate ones on request.)
- **Preferences dialog** (`src/ui/PreferencesDialog`, opened via the
  toolbar's "Preferences…" button) is the only place in the app that writes
  to the config file. It is seeded from the *saved* startup defaults (not
  the live toolbar/session values), and Save writes those defaults back
  without applying them to the current run -- toolbar and CLI remain the
  only way to change the live session. Three follow-up fixes after the
  first pass:
  - **Font/size instead of zoom.** The dialog originally exposed the same
    abstract "zoom step" integer `DocumentView` tracked internally, which
    was both a confusing unit to show a user (a bare `0` doesn't read as
    "normal size" without added context) and the wrong level of control --
    asked for directly, this became a real `QFontComboBox` (family, with
    each entry previewed in its own typeface) plus a point-size `QSpinBox`,
    combined into one "Font:" row. `DocumentView::setBaseFont()` applies
    this via `QTextDocument::setDefaultFont()` rather than `zoomIn`/
    `zoomOut` -- a real document property that persists across
    `loadPlainText()`/`loadHtml()` calls on its own, unlike the old
    zoom-step approach which needed explicit reapplication after every
    load. The config file's `textZoomSteps` key became `fontFamily` +
    `fontPointSize`.
  - **Unreadable text** (white on light grey) turned out to be a known Qt
    gotcha: the muted note label under the fields used `setStyleSheet()`,
    and applying a stylesheet to even one widget in a dialog can push Qt
    onto its CSS-based style engine for that (and sometimes sibling)
    widgets instead of the native platform style -- which is what produced
    the mismatched, barely-readable color combination. Replaced with plain
    `QFont`-based styling (smaller point size, italic), which doesn't touch
    palette resolution at all.
  - **Cramped fields**, still an issue after a first attempt at
    `setMinimumWidth()` alone: widgets now also get an explicit
    `QSizePolicy::Expanding` horizontally, and the font family + size share
    one row via a small `QWidget` wrapper (a `QHBoxLayout` added to
    `QFormLayout` directly as a field, rather than as a raw layout, doesn't
    reliably pick up the form's field-growth policy in the way a plain
    widget does) -- that combination is what actually made the growth
    policy take effect this time.
- **Middle-click and Ctrl+V paste** directly into the document view
  (`DocumentView::mousePressEvent`/`keyPressEvent`, emitting a
  `pasteRequested(QClipboard::Mode)` signal `MainWindow` forwards to the
  same `pasteFromSource()` the toolbar buttons use). Middle-click pastes
  the primary selection (X11/Wayland convention) regardless of exactly
  where in the view the click lands, since -- unlike a normal editor --
  there's no cursor position to insert at; it replaces the whole document,
  same as the toolbar buttons. Ctrl+V is intercepted before
  `QTextEdit::keyPressEvent()` since the widget is read-only and would
  otherwise silently do nothing.
- **HTML cleanup is a known TODO, deliberately deferred.** Right now
  `DocumentView::loadHtml()` hands pasted/opened HTML straight to Qt's
  `QTextDocument::setHtml()`, which will happily apply inline font-size,
  font-family, and other styling that fights with the user's own font
  choice and generally makes no sense read aloud by a TTS engine. The plan
  (not yet implemented, see the `TODO(HTML cleanup, deferred)` comment in
  `DocumentView::loadHtml()`) is to sanitize down to a genuinely minimal
  structural subset before parsing -- keeping things like headings, since
  they're useful signal for future features (a chime on chapter change,
  for instance), while dropping presentational styling entirely.
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

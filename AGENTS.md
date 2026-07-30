# AGENTS.md — guidance for coding agents

This file orients automated agents and human developers working on **text2sprech**. Prefer it over guessing from partial file views.

- **Users:** [`README.md`](README.md) (features, install, usage, CLI, config).
- **Wire protocol:** [`PROTOCOL.md`](PROTOCOL.md) (authoritative).
- **This file:** layout, build, architecture rules, versioning, agent workflow.

## TODO.md and commits

- Track unfinished product work in **`TODO.md`** (checkbox list). When you complete an item, mark it `[x]` in the same change set.
- Prefer implementing or updating TODO items before inventing unrelated scope.
- **Every larger change** (feature, protocol tweak, multi-file refactor, or a batch of TODO items) should end with a **detailed git commit message** in the agent’s final answer: subject line + body explaining *why*, what files/areas moved, and any follow-ups or intentional non-goals. Agents should not rely on a one-line “fix bug” summary for multi-file work.

## Version number handling

* Version number is stored in a file **`VERSION`** in the top-level directory and it is the **only source of truth**. Do not duplicate the version string in other places; generate it dynamically from that file.
* Inside git, the version number always contains a **`-dev` suffix** (e.g. `0.2.0-dev`).
* In **`CMakeLists.txt`**, the `VERSION` file is read and made available as `PROJECT_VERSION_FULL` (overrideable with `-DPROJECT_VERSION_FULL=...` for packaging).
* CMake passes `PROJECT_VERSION_FULL` into the sources as **`TEXT2SPRECH_VERSION`**.
* **`flake.nix`** reads `VERSION` and uses it as the package version, appending the short git hash when available, e.g. `0.2.0-dev+gd910b1c` or `0.2.0-dev+gfb40b2c-dirty`.
* The version is shown via the **`--version`** CLI flag (`QApplication::applicationVersion`) and in the **About** dialog.
* When a release is prepared, remove the `-dev` suffix, commit `VERSION`, and tag the tree: e.g. `0.2.0-dev` becomes `0.2.0` and the tag is **`v0.2.0`** (always `v` + the contents of `VERSION`).

## What this project is

Local **text-to-speech reader**: open or paste text/HTML, split into sentences, synthesize with **Piper**, play with **live sentence highlighting**, seek/prev/next, voice and speed controls.

Two processes:

1. **Qt6 GUI** (`text2sprech`) — document UI, playback queue, optional spawn of the server.
2. **Python piper-server** (`piper_server/`) — keeps models loaded, speaks over a Unix domain socket (JSON frames).

Nothing is sent to the cloud; synthesis stays on the machine.

## Layout

```
text2sprech/
├── VERSION                 # Sole version source of truth (e.g. 0.1.0-dev)
├── CMakeLists.txt          # Qt6 app + optional Qt Test target
├── flake.nix / flake.lock  # Nix packages, dev shell, optional bundled voice
├── PROTOCOL.md             # Client ↔ server message schema (source of truth)
├── README.md               # User-facing docs (features, install, usage)
├── TODO.md                 # Open product checklist (mark done when shipped)
├── REUSE.toml              # SPDX annotations for non-source assets
├── piper_server/           # Python server (tested without Qt)
│   ├── server.py           # asyncio Unix socket server
│   ├── protocol.py         # 4-byte length + UTF-8 JSON framing
│   ├── synth.py            # piper-python | piper-cli | silent-test backends
│   └── sentence_splitter.py  # reference splitter (keep in sync with C++)
├── src/
│   ├── main.cpp            # CLI + QSettings bootstrap
│   ├── ui/                 # MainWindow, DocumentView, PreferencesDialog
│   ├── document/           # Sentence, SentenceSplitter, HtmlSanitizer
│   └── speech/             # PiperClient, PiperServerManager, PlaybackController
├── resources/              # icons, .desktop, AppStream metainfo, .qrc
│   ├── text2sprech.desktop / text2sprech-clipboard.desktop
│   ├── text2sprech.metainfo.xml
│   ├── text2sprech.png / icons/text2sprech-*.png
│   └── text2sprech.qrc
└── tests/                  # test_sentence_splitter (Qt Test)
```

Include paths are rooted at `src/` (`#include "ui/MainWindow.h"`, etc.).

## Build and run

**CMake (needs Qt6: Core, Widgets, Multimedia, Network):**

```bash
cmake -B build -G Ninja
cmake --build build
./build/text2sprech
```

`cmake --install` places the binary, both `.desktop` files, AppStream metainfo, hicolor icons, and `share/text2sprech/piper_server/*.py`.

Server lifecycle: by default the app spawns a unique per-instance socket via `PiperServerManager`. Lookup order for the server:

1. `$TEXT2SPRECH_PIPER_SERVER_BIN`
2. `piper-server` on `PATH`
3. `python3` + `../share/text2sprech/piper_server/server.py` relative to the binary (installed layout)
4. `python3` + `PIPER_SERVER_SCRIPT_PATH` (compile-time source-tree path, dev only)

Attach to an external server: `--piper-socket /path/to.sock` (app must **not** stop that process).

**Nix:** `nix develop`, `nix build .#text2sprech-full` (default app with bundled en_US-lessac-medium). Piper Python package is not reliably in nixpkgs; real audio often relies on the **piper CLI** fallback in `synth.py`.

**Tests:**

- Python splitter / protocol / CLI crash degradation: exercise under `piper_server/` (no Qt required).
- C++ splitter: build with Qt Test present, then `ctest` / `./build/test_sentence_splitter`.
- Full GUI and C++ compile may be unavailable in minimal sandboxes; do not assume a green compile proves design correctness if the toolchain was never run.

## Architecture rules (do not casually break)

### Protocol

- Framing: `[4-byte big-endian uint32 length][UTF-8 JSON]` — **not** line-delimited text. See `PROTOCOL.md`.
- Client pipelines `speak` requests with monotonic `id`; responses match by `id`.
- Audio is full **WAV** per sentence, base64 inside JSON (v1 simplicity).
- `ready` carries `audio` / `warning`; GUI must treat `audio: false` as unusable for listening, not “play silent WAVs as success.”
- Changing message types or fields requires **both** `piper_server/` and `src/speech/PiperClient.*`.

### Playback and voice

- Speed is Piper **`length_scale` at synthesis time**, not `QMediaPlayer` rate (avoids pitch distortion). Mid-sentence speed change implies a new synth request for unplayed work.
- Voice changes go through **`PlaybackController::setVoice()`**, which cancels in-flight/queued synthesis and drops cached unplayed audio. Wiring the toolbar combo only to `PiperClient` reintroduces “stuck on old voice for a sentence or two.”
- `PiperServerManager` owns spawn/stop only when the app started the server; `--piper-socket` means external lifecycle.

### Document / UI

- Sentence offsets are on **`document()->toPlainText()`** after load (including HTML path). Highlighting and click-to-jump depend on this.
- **C++ `SentenceSplitter` must stay behaviorally aligned** with `piper_server/sentence_splitter.py` (abbreviations, decimals, initials, newline boundaries, `max_len` subdivision, drop non-alnum-only junk). Prefer updating the Python reference and tests first, then port. Splitter is heuristic, not a full NLP tokenizer; newlines are always hard boundaries.
- Drop punctuation/symbol-only segments before Piper (`hasSpeakableContent` / `_is_speakable`); Piper can crash on zero-phoneme WAV headers. `PiperCliBackend` also degrades known empty-WAV crashes to short silence + log warning.
- HTML goes through **`HtmlSanitizer::toSemanticHtml()`** before `DocumentView::loadHtml()` so author fonts/colors do not fight Preferences. Qt’s HTML subset is not a full browser.
- Highlights use **`QTextEdit::ExtraSelection` only**. Do not call `setTextCursor()` with a real selection for the active sentence — Qt paints native palette Highlight on top and breaks custom colors. Centering uses `cursorRect()` on a local cursor (`centerCursor`); auto-scroll is optional in the UI.
- Arrow Left/Right for sentence nav are handled in **`DocumentView::keyPressEvent`**, not MainWindow `QAction` shortcuts (focus steals plain arrows). Shared path: `MainWindow::jumpToSentence()` (also used by Find and click-to-jump).
- **Space** play/pause is handled in a MainWindow `eventFilter` (not a fragile `QAction` shortcut) so toolbar focus does not swallow it; Find’s line edit still gets literal spaces.
- Document font is a real **family + point size** (`DocumentView::setBaseFont` → `QTextDocument::setDefaultFont`), not relative zoom steps.
- **Only `PreferencesDialog` writes** the startup config file (`~/.config/text2sprech/text2sprech.ini` via `QSettings` IniFormat). Toolbar and CLI are session overrides. Recent files and per-path positions use other groups (`recent`, `positions`).

### Synthesis backends (`synth.py`)

Preference order: `PiperPythonBackend` → `PiperCliBackend` → `SilentTestBackend`.

- Keep silent-test so protocol and GUI can be developed without models.
- CLI backend: pre-filter unspeakable text; on known empty-WAV / `channels not specified` crashes, degrade to short silence + log warning; other failures still raise.

## Coding conventions

- **License:** GPL-3.0-or-later. New source files need SPDX headers:
  ```
  // SPDX-FileCopyrightText: 2026 Ingo Ruhnke <grumbel@gmail.com>
  // SPDX-License-Identifier: GPL-3.0-or-later
  ```
  (Python: `# SPDX-...`). Non-code paths: update `REUSE.toml` if needed.
- **C++:** C++20, Qt6, MOC/RCC via CMake. Prefer small, single-purpose classes matching existing `ui/` / `document/` / `speech/` split.
- **Python:** 3.x, asyncio server; keep framing in `protocol.py` and backends in `synth.py`.
- Match existing style: descriptive comments where Qt gotchas or protocol rationale matter; avoid drive-by refactors unrelated to the task.
- Do not expand scope into cloud TTS, full browser HTML, or a second binary protocol channel unless explicitly requested (PROTOCOL.md notes base64 overhead is acceptable for sentence-length audio until proven otherwise).

## Config and CLI (quick reference)

| Mechanism | Role |
|-----------|------|
| `[startup]` in ini | Defaults: voice, speed, fontFamily, fontPointSize, optional color* |
| `[voices] extraDirs` | Extra model directories (restart to apply) |
| `[recent]`, `[positions]` | Recent paths; base64url path keys → sentence index |
| CLI `--voice`, `--speed`, `--autoplay`, `--piper-socket`, `--paste-*`, `--config`, `--font-family`, `--font-size`, `--margin`, `--line-spacing`, `--color-*`, `[file]`/`-` | Session overrides; load deferred until server `ready`. `--config` seeds startup from an alternate `.ini` and Preferences Save writes there. |

Voice models: `~/.local/share/text2sprech/voices/` (`Name.onnx` + `Name.onnx.json`), also `~/.local/share/piper/voices/`, env `TEXT2SPRECH_PIPER_MODELS`, Preferences extra dirs.

## Safe change checklist

1. Protocol or framing change → update `PROTOCOL.md`, Python server, C++ `PiperClient`, and any manual client tests.
2. Splitter heuristic change → Python + C++ + `tests/test_sentence_splitter.cpp` (+ Python ad-hoc checks if used).
3. Playback/voice/speed → preserve cancel-on-voice-change and synthesis-time speed.
4. Highlighting/scroll → no real `QTextCursor` selection for the active range; keep ExtraSelection + `centerCursor`.
5. Preferences vs session → do not write startup ini from toolbar/CLI paths.
6. Server lifecycle → never `stop()` a server the user passed via `--piper-socket`.
7. New Qt sources → list them in `CMakeLists.txt` (and Test target if needed).
8. User-visible behavior → consider a short README note only if the public CLI/config/protocol surface changes.

## Out of scope unless asked

- Vendoring a full Piper Python package into nixpkgs (see flake TODO).
- Binary audio channel alongside JSON.
- Turning the document view into a full editor or multi-document IDE.
- Proprietary or network-dependent TTS backends.

# AGENTS.md — guidance for coding agents

This file orients automated agents working on **text2sprech**. Prefer it over guessing from partial file views. User-facing detail and design history live in `README.md`; the wire format is authoritative in `PROTOCOL.md`.

## What this project is

Local **text-to-speech reader**: open or paste text/HTML, split into sentences, synthesize with **Piper**, play with **live sentence highlighting**, seek/prev/next, voice and speed controls.

Two processes:

1. **Qt6 GUI** (`text2sprech`) — document UI, playback queue, optional spawn of the server.
2. **Python piper-server** (`piper_server/`) — keeps models loaded, speaks over a Unix domain socket (JSON frames).

Nothing is sent to the cloud; synthesis stays on the machine.

## Layout

```
text2sprech/
├── CMakeLists.txt          # Qt6 app + optional Qt Test target
├── flake.nix / flake.lock  # Nix packages, dev shell, optional bundled voice
├── PROTOCOL.md             # Client ↔ server message schema (source of truth)
├── README.md               # Features, CLI, config, design notes
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
├── resources/              # .qrc icon embed, .desktop, PNG
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

Server lifecycle: by default the app spawns a unique per-instance socket via `PiperServerManager`. Lookup order for the server binary: `$TEXT2SPRECH_PIPER_SERVER_BIN` → `piper-server` on `PATH` → `python3` + `PIPER_SERVER_SCRIPT_PATH` (compile-time path to `piper_server/server.py`).

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
- **C++ `SentenceSplitter` must stay behaviorally aligned** with `piper_server/sentence_splitter.py` (abbreviations, decimals, initials, newline boundaries, `max_len` subdivision, drop non-alnum-only junk). Prefer updating the Python reference and tests first, then port.
- Drop punctuation/symbol-only segments before Piper (`hasSpeakableContent` / `_is_speakable`); Piper can crash on zero-phoneme WAV headers.
- HTML goes through **`HtmlSanitizer::toSemanticHtml()`** before `DocumentView::loadHtml()` so author fonts/colors do not fight Preferences.
- Highlights use **`QTextEdit::ExtraSelection` only**. Do not call `setTextCursor()` with a real selection for the active sentence — Qt paints native palette Highlight on top and breaks custom colors. Centering uses `cursorRect()` on a local cursor.
- Arrow Left/Right for sentence nav are handled in **`DocumentView::keyPressEvent`**, not MainWindow `QAction` shortcuts (focus steals plain arrows).
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
| CLI `--voice`, `--speed`, `--autoplay`, `--piper-socket`, `--paste-*`, `[file]`/`-` | Session overrides; load deferred until server `ready` |

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

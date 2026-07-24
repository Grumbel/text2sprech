# reader — local TTS reader with live highlighting

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
| `sentence_splitter.py` | **Tested** against abbreviations/decimals/initials edge cases. |
| C++ (`src/`, `tests/`) | **Written but not compiled** — no Qt6/CMake toolchain available here. `SentenceSplitter.cpp` mirrors the tested Python logic; `tests/test_sentence_splitter.cpp` exists so you can verify it the moment you build. Everything else (PiperClient, PlaybackController, DocumentView, MainWindow) is unverified by a compiler — expect the normal first-build friction (a missed include, a signal/slot signature mismatch) rather than fundamental design bugs, since the interfaces were kept deliberately small and single-purpose. |
| `flake.nix` | **Not built** — no network here to fetch nixpkgs. There's one known real gap: the Piper Python package isn't reliably in nixpkgs, so `piper_server`'s empty `pythonEnv` relies on the CLI-binary fallback in `synth.py` unless you vendor a real package (flake.nix has a TODO with the two concrete options). |

## Running it

```bash
# Terminal 1: start the Piper server
python3 piper_server/server.py --verbose
# (falls back to silent test audio if Piper isn't installed; install the
#  `piper` CLI or a piper Python package and point --model-dir at your
#  .onnx voices to get real speech)

# Terminal 2: build and run the Qt app (once you have Qt6 + CMake)
cmake -B build -G Ninja
cmake --build build
./build/reader
```

Or via the flake, once you can reach the network to resolve nixpkgs:

```bash
nix develop
nix build .#reader
nix run .#piper-server &
nix run .#reader
```

## Project layout

```
reader/
├── flake.nix
├── CMakeLists.txt
├── PROTOCOL.md              # wire protocol between reader and piper-server
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
- **HTML rendering relies on Qt's built-in subset** (`QTextDocument::setHtml`),
  which covers headings/paragraphs/emphasis/line breaks as scoped, but is
  not a full browser engine.

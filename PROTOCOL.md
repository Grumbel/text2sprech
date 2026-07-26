# text2sprech <-> Piper Server Protocol

Transport: Unix domain socket. The app generates a unique per-instance path
(e.g. `/tmp/text2sprech-piper-<pid>-<uuid>.sock`, see `PiperServerManager`)
so multiple instances can run at once; `--piper-socket` lets you point at
an already-running server instead. TCP fallback on `127.0.0.1:<port>` is
fine too (same framing) if you want to run the server in a container/VM
separate from the GUI.

## Framing

Every message (both directions) is:

```
[4 bytes: big-endian uint32 length N] [N bytes: UTF-8 JSON]
```

No newline-delimited text. This avoids ambiguity with text containing
newlines and lets us send binary audio safely (base64-encoded inside JSON
for v1 simplicity; see "Future work" below).

## Client -> Server messages

```jsonc
{"type": "speak", "id": 123, "text": "The spacecraft entered orbit around Mars."}
{"type": "stop", "id": 123}          // cancel a specific in-flight request
{"type": "stop_all"}                  // flush the whole queue
{"type": "set_voice", "voice": "en_US-lessac-medium"}
{"type": "set_speed", "speed": 1.25}  // 0.5 - 3.0
{"type": "list_voices"}
{"type": "shutdown"}
```

## Server -> Client messages

```jsonc
{
  "type": "ready",
  "voices": ["en_US-lessac-medium", ...],
  "voice": "en_US-lessac-medium",
  "backend": "piper-cli",          // piper-python | piper-cli | silent-test
  "audio": true,                   // false when silent-test or no models found
  "model_dir": "/path/to/voices",  // may be ""
  "warning": ""                    // non-empty human message when audio is unusable
}
{"type": "audio", "id": 123, "format": "wav", "sample_rate": 22050, "data": "<base64 wav bytes>"}
{"type": "error", "id": 123, "message": "..."}
{"type": "voices", "voices": [...]}
{"type": "ack", "for": "set_voice", "voice": "en_US-lessac-medium"}
```

Clients should treat `audio: false` (or a non-empty `warning`) as “not ready
for listening” and surface that in the UI rather than playing silent WAVs.

## Notes / rationale

- `id` is chosen by the client (monotonically increasing) so requests can be
  pipelined: the client can send sentences 2, 3, 4 while sentence 1 is still
  synthesizing, and match responses back by id regardless of arrival order.
- Full WAV per sentence (not raw PCM) so the client doesn't need to track
  sample format state across messages; Qt Multimedia can decode WAV directly.
- Base64-in-JSON is wasteful (~33% overhead) but simple and fine for
  sentence-length audio (typically a few hundred KB at most). If profiling
  later shows this matters, switch to a second raw-binary channel keyed by
  id, but don't build that complexity until it's proven necessary.

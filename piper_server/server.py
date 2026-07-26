"""Persistent Piper TTS server.

Listens on a Unix domain socket, keeps voice models loaded, and answers
`speak` requests asynchronously so multiple sentences can be in flight
at once (the client pipelines requests; responses are matched by id).
"""
from __future__ import annotations

import argparse
import asyncio
import base64
import logging
import os
import signal
from pathlib import Path

from protocol import FrameError, read_message, write_message
from synth import SynthBackend, default_user_voices_dir, resolve_model_dirs, select_backend

log = logging.getLogger("piper_server")


class Session:
    def __init__(self, backend: SynthBackend, default_voice: str):
        self.backend = backend
        self.voice = default_voice
        self.speed = 1.0
        self._cancelled: set[int] = set()

    def cancel(self, request_id: int | None) -> None:
        if request_id is None:
            return
        self._cancelled.add(request_id)

    def is_cancelled(self, request_id: int) -> bool:
        return request_id in self._cancelled


async def handle_speak(
    session: Session, writer: asyncio.StreamWriter, msg: dict
) -> None:
    request_id = msg["id"]
    text = msg["text"]
    if not session.backend.real_audio:
        write_message(
            writer,
            {
                "type": "error",
                "id": request_id,
                "message": (
                    "No Piper TTS install available (silent-test backend). "
                    "Install piper and voice models, then restart text2sprech."
                ),
            },
        )
        return
    if session.voice not in session.backend.list_voices():
        write_message(
            writer,
            {
                "type": "error",
                "id": request_id,
                "message": (
                    f"Voice model not found: {session.voice!r}. "
                    "Choose another voice or install the matching .onnx files."
                ),
            },
        )
        return
    try:
        result = await session.backend.synthesize(text, session.voice, session.speed)
    except Exception as e:  # noqa: BLE001 - report all synth failures to the client
        log.exception("synthesis failed for id=%s", request_id)
        write_message(writer, {"type": "error", "id": request_id, "message": str(e)})
        return

    if session.is_cancelled(request_id):
        return

    write_message(
        writer,
        {
            "type": "audio",
            "id": request_id,
            "format": "wav",
            "sample_rate": result.sample_rate,
            "data": base64.b64encode(result.wav_bytes).decode("ascii"),
        },
    )


def _ready_payload(backend: SynthBackend, default_voice: str) -> dict:
    """Build the initial `ready` message, including whether real TTS audio
    is available so the GUI can warn instead of looking broken."""
    voices = backend.list_voices()
    model_dir = str(backend.model_dir) if backend.model_dir is not None else ""
    drop_dir = str(default_user_voices_dir())
    warning = ""
    # Silent test backend: protocol works, but nothing is audible.
    if not backend.real_audio:
        warning = (
            "No Piper TTS install found — audio is silent. "
            "Install the piper CLI or Python package, then place matching "
            f".onnx + .onnx.json voice files in {drop_dir}."
        )
    elif not voices:
        warning = (
            "Piper is installed but no voice models were found. "
            f"Drop .onnx + .onnx.json files into {drop_dir} "
            "(or another directory listed under Preferences → Extra voice directories)."
        )
    # Prefer a voice that actually exists; fall back to the configured default.
    voice = default_voice if default_voice in voices else (voices[0] if voices else default_voice)
    return {
        "type": "ready",
        "voices": voices,
        "voice": voice,
        "backend": backend.name,
        "audio": bool(backend.real_audio and voices),
        "model_dir": model_dir,
        "warning": warning,
    }


async def handle_connection(
    reader: asyncio.StreamReader, writer: asyncio.StreamWriter, backend: SynthBackend, default_voice: str
) -> None:
    peer = writer.get_extra_info("peername") or "unix-client"
    log.info("client connected: %s", peer)
    ready = _ready_payload(backend, default_voice)
    session = Session(backend, ready["voice"])
    in_flight: set[asyncio.Task] = set()

    write_message(writer, ready)

    try:
        while True:
            try:
                msg = await read_message(reader)
            except FrameError as e:
                log.warning("framing error from %s: %s", peer, e)
                break
            if msg is None:
                break

            mtype = msg.get("type")
            if mtype == "speak":
                task = asyncio.create_task(handle_speak(session, writer, msg))
                in_flight.add(task)
                task.add_done_callback(in_flight.discard)
            elif mtype == "stop":
                session.cancel(msg.get("id"))
            elif mtype == "stop_all":
                for task in in_flight:
                    task.cancel()
            elif mtype == "set_voice":
                session.voice = msg["voice"]
                write_message(writer, {"type": "ack", "for": "set_voice", "voice": session.voice})
            elif mtype == "set_speed":
                session.speed = float(msg["speed"])
                write_message(writer, {"type": "ack", "for": "set_speed", "speed": session.speed})
            elif mtype == "list_voices":
                write_message(writer, {"type": "voices", "voices": backend.list_voices()})
            elif mtype == "shutdown":
                log.info("shutdown requested by %s", peer)
                break
            else:
                write_message(writer, {"type": "error", "id": msg.get("id"), "message": f"unknown message type: {mtype}"})
    finally:
        for task in in_flight:
            task.cancel()
        writer.close()
        log.info("client disconnected: %s", peer)


async def run_server(socket_path: str, model_dirs: list[Path], default_voice: str) -> None:
    backend = select_backend(model_dirs)
    log.info("using synthesis backend: %s", backend.name)
    log.info("voice model directories: %s", os.pathsep.join(str(d) for d in model_dirs))
    if backend.name == "silent-test":
        log.warning(
            "No Piper install found -- running with the SilentTestBackend. "
            "Install piper (Python package or CLI binary) and place .onnx "
            "voice models in %s to get real audio.",
            default_user_voices_dir(),
        )

    if os.path.exists(socket_path):
        os.remove(socket_path)

    server = await asyncio.start_unix_server(
        lambda r, w: handle_connection(r, w, backend, default_voice), path=socket_path
    )
    log.info("listening on %s", socket_path)

    loop = asyncio.get_running_loop()
    stop = loop.create_future()
    for sig in (signal.SIGINT, signal.SIGTERM):
        loop.add_signal_handler(sig, stop.set_result, None)

    await stop
    # Close the listening socket and do not wait long for client handlers —
    # the GUI aborts its QLocalSocket on quit, but bound this so a stuck
    # connection cannot delay process exit (and the parent QProcess wait).
    server.close()
    try:
        await asyncio.wait_for(server.wait_closed(), timeout=0.3)
    except asyncio.TimeoutError:
        log.debug("server.wait_closed timed out; exiting anyway")
    log.info("server stopped")


def main() -> None:
    parser = argparse.ArgumentParser(description="Persistent Piper TTS server")
    parser.add_argument(
        "--socket",
        default=os.environ.get("TEXT2SPRECH_PIPER_SOCKET", "/tmp/text2sprech-piper.sock"),
    )
    parser.add_argument(
        "--model-dir",
        action="append",
        default=None,
        help=(
            "Directory containing Piper .onnx voice models (repeatable). "
            "Also reads TEXT2SPRECH_PIPER_MODELS (path-separator list). "
            f"Default: {default_user_voices_dir()} and ~/.local/share/piper/voices"
        ),
    )
    parser.add_argument("--default-voice", default="en_US-lessac-medium")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )
    explicit = [Path(p) for p in args.model_dir] if args.model_dir else None
    model_dirs = resolve_model_dirs(explicit)
    asyncio.run(run_server(args.socket, model_dirs, args.default_voice))


if __name__ == "__main__":
    main()

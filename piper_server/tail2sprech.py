#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Ingo Ruhnke <grumbel@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later

"""Speak text as it arrives, like `tail -f` for your ears.

    tail2sprech /var/log/app.log
    make 2>&1 | tail2sprech

A standalone client of piper-server rather than a GUI mode: the GUI assumes
a finished document (sentence offsets into a fixed QTextDocument, resume
positions, seek bar, export), while this only ever moves forward through a
queue. Following a file is delegated to `tail -F`, which already handles
rotation and truncation; this program only sees a stream of bytes.
"""
from __future__ import annotations

import argparse
import asyncio
import base64
import codecs
import os
import re
import shlex
import shutil
import signal
import sys
import tempfile
import uuid
from collections import deque
from pathlib import Path

from protocol import FrameError, read_message, write_message
from sentence_splitter import DEFAULT_MAX_LEN, split_sentences

READ_SIZE = 64 * 1024
SERVER_START_TIMEOUT_S = 30.0

# Tried in order; each is called with the WAV path appended.
PLAYERS = (
    ("pw-play",),
    ("paplay",),
    ("aplay", "-q"),
    ("ffplay", "-nodisp", "-autoexit", "-loglevel", "error"),
)

# Colors and cursor movement from build tools and loggers, which would
# otherwise be read out ("thirty-two m").
_ANSI_ESCAPE = re.compile(r"\x1b\[[0-9;?]*[ -/]*[@-~]")


class Tail2SprechError(Exception):
    pass


class SynthesisError(Exception):
    pass


def warn(message: str) -> None:
    print(f"tail2sprech: {message}", file=sys.stderr, flush=True)


class SentenceStream:
    """Incremental front end to split_sentences().

    split_sentences() wants a finished text, so the newest sentence is held
    back until something proves it ended: a later sentence or a newline.
    Boundaries already found are stable under appending, since the splitter
    only looks one character past the whitespace that follows a sentence.
    """

    def __init__(self, max_len: int = DEFAULT_MAX_LEN):
        self._max_len = max_len
        self._buffer = ""

    @property
    def has_partial(self) -> bool:
        return bool(self._buffer.strip())

    def feed(self, text: str) -> list[str]:
        """Add text; returns the sentences that are now known to be complete."""
        # Stripping the whole buffer, not just `text`, catches an escape
        # sequence that was split across two reads.
        self._buffer = _ANSI_ESCAPE.sub("", self._buffer + text)
        sentences = split_sentences(self._buffer, self._max_len)
        complete_upto = self._buffer.rfind("\n") + 1

        if sentences and sentences[-1].end > complete_upto:
            complete_upto = sentences[-1].start
            sentences.pop()
        self._buffer = self._buffer[complete_upto:]
        return [s.text for s in sentences]

    def flush(self) -> list[str]:
        """Give up waiting and return whatever is held back as a sentence."""
        sentences = split_sentences(self._buffer, self._max_len)
        self._buffer = ""
        return [s.text for s in sentences]


class PiperConnection:
    """One client session with piper-server; see PROTOCOL.md."""

    def __init__(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
        self._reader = reader
        self._writer = writer
        self._next_id = 1
        self._requests: dict[int, asyncio.Future[bytes]] = {}

    async def _read(self) -> dict:
        try:
            msg = await read_message(self._reader)
        except (FrameError, ConnectionError) as e:
            raise Tail2SprechError(f"piper-server connection failed: {e}") from e
        if msg is None:
            raise Tail2SprechError("piper-server closed the connection")
        return msg

    async def read_ready(self) -> dict:
        while True:
            msg = await self._read()
            if msg.get("type") == "ready":
                return msg

    def set_voice(self, voice: str) -> None:
        write_message(self._writer, {"type": "set_voice", "voice": voice})

    def set_speed(self, speed: float) -> None:
        write_message(self._writer, {"type": "set_speed", "speed": speed})

    def speak(self, text: str) -> asyncio.Future[bytes]:
        request_id = self._next_id
        self._next_id += 1
        future = asyncio.get_running_loop().create_future()
        self._requests[request_id] = future
        write_message(self._writer, {"type": "speak", "id": request_id, "text": text})
        return future

    async def dispatch_responses(self) -> None:
        """Runs for the life of the connection; raises when it is lost."""
        while True:
            msg = await self._read()
            mtype = msg.get("type")
            if mtype == "audio":
                future = self._requests.pop(msg["id"], None)
                if future is not None:
                    future.set_result(base64.b64decode(msg["data"]))
            elif mtype == "error":
                future = self._requests.pop(msg.get("id"), None)
                if future is not None:
                    future.set_exception(SynthesisError(msg["message"]))
                else:
                    warn(msg["message"])

    def close(self) -> None:
        self._writer.close()


class Speaker:
    """Plays sentences in arrival order, keeping `lookahead` of them
    synthesized or in flight so sentences follow each other without a gap."""

    def __init__(
        self,
        connection: PiperConnection,
        player: list[str],
        lookahead: int,
        max_backlog: int,
        quiet: bool,
    ):
        self._connection = connection
        self._player = player
        self._lookahead = lookahead
        self._max_backlog = max_backlog
        self._quiet = quiet
        self._queued: deque[str] = deque()
        self._requested: deque[tuple[str, asyncio.Future[bytes]]] = deque()
        self._wake = asyncio.Event()
        self._input_finished = False

    def add(self, sentences: list[str]) -> None:
        self._queued.extend(sentences)
        if self._max_backlog > 0 and len(self._queued) > self._max_backlog:
            dropped = len(self._queued) - self._max_backlog
            for _ in range(dropped):
                self._queued.popleft()
            warn(f"falling behind, skipped {dropped} sentence(s)")
        self._request_more()
        self._wake.set()

    def finish(self) -> None:
        self._input_finished = True
        self._wake.set()

    def _request_more(self) -> None:
        while self._queued and len(self._requested) < self._lookahead:
            text = self._queued.popleft()
            self._requested.append((text, self._connection.speak(text)))

    async def run(self) -> None:
        """Returns once the input has finished and everything was played."""
        while True:
            if not self._requested:
                if self._input_finished:
                    return
                self._wake.clear()
                await self._wake.wait()
                continue

            text, audio = self._requested[0]
            try:
                wav = await audio
            except SynthesisError as e:
                warn(f"synthesis failed for {text!r}: {e}")
                self._requested.popleft()
                self._request_more()
                continue
            self._requested.popleft()
            self._request_more()

            if not self._quiet:
                print(text, flush=True)
            await self._play(wav)

    async def _play(self, wav: bytes) -> None:
        with tempfile.NamedTemporaryFile(prefix="tail2sprech-", suffix=".wav") as wav_file:
            wav_file.write(wav)
            wav_file.flush()
            proc = await asyncio.create_subprocess_exec(
                *self._player, wav_file.name,
                stdin=asyncio.subprocess.DEVNULL,
                stdout=asyncio.subprocess.DEVNULL,
            )
            try:
                returncode = await proc.wait()
            finally:
                if proc.returncode is None:
                    proc.terminate()
                    await proc.wait()
        if returncode != 0:
            warn(f"{self._player[0]} exited with status {returncode}")


async def pump_input(
    stdout: asyncio.StreamReader, speaker: Speaker, flush_after: float
) -> None:
    decoder = codecs.getincrementaldecoder("utf-8")(errors="replace")
    stream = SentenceStream()
    while True:
        # A writer that has gone quiet mid-line (a prompt, a progress
        # message without newline) would otherwise never be spoken.
        timeout = flush_after if stream.has_partial and flush_after > 0 else None
        try:
            data = await asyncio.wait_for(stdout.read(READ_SIZE), timeout)
        except asyncio.TimeoutError:
            speaker.add(stream.flush())
            continue
        if not data:
            break
        speaker.add(stream.feed(decoder.decode(data)))
    speaker.add(stream.feed(decoder.decode(b"", final=True)) + stream.flush())
    speaker.finish()


def find_server_command() -> list[str]:
    """Same lookup order as PiperServerManager::findServerCommand()."""
    env_bin = os.environ.get("TEXT2SPRECH_PIPER_SERVER_BIN")
    if env_bin and os.path.exists(env_bin):
        return [env_bin]
    on_path = shutil.which("piper-server")
    if on_path:
        return [on_path]
    return [sys.executable, str(Path(__file__).resolve().parent / "server.py")]


def find_player(override: str | None) -> list[str]:
    if override:
        command = shlex.split(override)
        if not command or shutil.which(command[0]) is None:
            raise Tail2SprechError(f"player not found: {override}")
        return command
    for command in PLAYERS:
        if shutil.which(command[0]) is not None:
            return list(command)
    names = ", ".join(command[0] for command in PLAYERS)
    raise Tail2SprechError(f"no audio player found (tried {names}); use --player")


def source_command(file: str, lines: str) -> list[str]:
    if file == "-":
        return ["cat"]  # gives stdin, pipe or not, the same async interface
    if not os.path.exists(file):
        raise Tail2SprechError(f"cannot open {file!r}: no such file")
    return ["tail", "-n", lines, "-F", "--", file]


async def start_server(socket_path: str, verbose: bool) -> asyncio.subprocess.Process:
    command = find_server_command()
    try:
        return await asyncio.create_subprocess_exec(
            *command, "--socket", socket_path,
            stdin=asyncio.subprocess.DEVNULL,
            stdout=None if verbose else asyncio.subprocess.DEVNULL,
            stderr=None if verbose else asyncio.subprocess.DEVNULL,
        )
    except OSError as e:
        raise Tail2SprechError(f"cannot start {command[0]}: {e}") from e


async def stop_server(server: asyncio.subprocess.Process, socket_path: str) -> None:
    if server.returncode is None:
        server.terminate()
        try:
            await asyncio.wait_for(server.wait(), timeout=1.0)
        except asyncio.TimeoutError:
            server.kill()
            await server.wait()
    Path(socket_path).unlink(missing_ok=True)


async def connect(
    socket_path: str, server: asyncio.subprocess.Process | None
) -> PiperConnection:
    loop = asyncio.get_running_loop()
    deadline = loop.time() + SERVER_START_TIMEOUT_S
    while True:
        try:
            reader, writer = await asyncio.open_unix_connection(socket_path)
            return PiperConnection(reader, writer)
        except (FileNotFoundError, ConnectionRefusedError) as e:
            if server is None:
                raise Tail2SprechError(f"cannot connect to {socket_path}: {e}") from e
            if server.returncode is not None:
                raise Tail2SprechError(
                    f"piper-server exited with status {server.returncode} "
                    "(run with --verbose to see why)"
                ) from e
            if loop.time() > deadline:
                raise Tail2SprechError("timed out waiting for piper-server") from e
            await asyncio.sleep(0.05)


def apply_settings(connection: PiperConnection, ready: dict, args: argparse.Namespace) -> None:
    # Same rule as the GUI: never play silent WAVs as if that were success.
    if not ready.get("audio") or ready.get("warning"):
        raise Tail2SprechError(ready.get("warning") or "piper-server has no usable audio backend")
    if args.voice:
        if args.voice not in ready["voices"]:
            raise Tail2SprechError(
                f"unknown voice {args.voice!r}; available: {', '.join(ready['voices'])}"
            )
        connection.set_voice(args.voice)
    if args.speed is not None:
        connection.set_speed(args.speed)


async def speak_stream(connection: PiperConnection, player: list[str], args: argparse.Namespace) -> None:
    source = await asyncio.create_subprocess_exec(
        *source_command(args.file, args.lines), stdout=asyncio.subprocess.PIPE
    )
    speaker = Speaker(connection, player, args.lookahead, args.max_backlog, args.quiet)
    pumping = asyncio.create_task(pump_input(source.stdout, speaker, args.flush_after))
    dispatching = asyncio.create_task(connection.dispatch_responses())
    speaking = asyncio.create_task(speaker.run())
    tasks = (pumping, dispatching, speaking)
    try:
        # The dispatcher only ever ends by raising, so a lost connection
        # surfaces here instead of leaving `speaking` waiting forever.
        await asyncio.wait({speaking, dispatching}, return_when=asyncio.FIRST_COMPLETED)
        if dispatching.done():
            dispatching.result()
        speaking.result()
        if await source.wait() != 0:
            raise Tail2SprechError(f"{args.file}: input failed with status {source.returncode}")
    finally:
        for task in tasks:
            task.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)
        if source.returncode is None:
            source.terminate()
            await source.wait()


async def run(args: argparse.Namespace) -> int:
    asyncio.get_running_loop().add_signal_handler(signal.SIGTERM, asyncio.current_task().cancel)

    player = None if args.list_voices else find_player(args.player)
    if not args.list_voices:
        source_command(args.file, args.lines)  # fail before starting a server

    server = None
    socket_path = args.piper_socket
    if socket_path is None:
        socket_path = os.path.join(
            tempfile.gettempdir(), f"text2sprech-piper-{os.getpid()}-{uuid.uuid4().hex}.sock"
        )
        server = await start_server(socket_path, args.verbose)
    connection = None
    try:
        connection = await connect(socket_path, server)
        ready = await connection.read_ready()
        if args.list_voices:
            for voice in ready["voices"]:
                print(voice)
            return 0
        apply_settings(connection, ready, args)
        await speak_stream(connection, player, args)
        return 0
    finally:
        if connection is not None:
            connection.close()
        # Only a server we started: --piper-socket is someone else's process.
        if server is not None:
            await stop_server(server, socket_path)


def positive_float(text: str) -> float:
    value = float(text)
    if value <= 0:
        raise argparse.ArgumentTypeError("must be greater than 0")
    return value


def positive_int(text: str) -> int:
    value = int(text)
    if value <= 0:
        raise argparse.ArgumentTypeError("must be greater than 0")
    return value


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        prog="tail2sprech",
        description="Speak text as it is appended to a file or pipe, using piper-server.",
    )
    parser.add_argument(
        "file", nargs="?", default="-",
        help="file to follow, surviving rotation like tail -F; "
             "'-' or nothing reads stdin until it ends",
    )
    parser.add_argument(
        "-n", "--lines", default="0", metavar="N",
        help="also speak the last N existing lines, or from line N with +N, "
             "as in tail(1); ignored for stdin (default: 0, only new text)",
    )
    parser.add_argument("--voice", help="voice name (see --list-voices)")
    parser.add_argument("--speed", type=positive_float, help="speech speed factor, 1.0 is normal")
    parser.add_argument("--list-voices", action="store_true", help="list voices and exit")
    parser.add_argument(
        "--piper-socket", metavar="PATH",
        help="use a running piper-server instead of starting one",
    )
    parser.add_argument(
        "--player", metavar="COMMAND",
        help="command that plays the WAV file given as its last argument "
             "(default: first found of pw-play, paplay, aplay, ffplay)",
    )
    parser.add_argument(
        "--flush-after", type=float, default=1.0, metavar="SECONDS",
        help="speak an unfinished line after this long without new input; "
             "0 waits for the end of the line (default: %(default)s)",
    )
    parser.add_argument(
        "--max-backlog", type=int, default=0, metavar="N",
        help="when more than N sentences wait to be spoken, skip the oldest "
             "to catch up (default: 0, never skip)",
    )
    parser.add_argument(
        "--lookahead", type=positive_int, default=2, metavar="N",
        help="sentences to synthesize ahead of playback (default: %(default)s)",
    )
    parser.add_argument("-q", "--quiet", action="store_true", help="do not print spoken sentences")
    parser.add_argument("-v", "--verbose", action="store_true", help="show piper-server log output")
    return parser.parse_args(argv)


def main() -> int:
    args = parse_args()
    try:
        return asyncio.run(run(args))
    except Tail2SprechError as e:
        warn(str(e))
        return 1
    except (KeyboardInterrupt, asyncio.CancelledError):
        return 130


if __name__ == "__main__":
    sys.exit(main())

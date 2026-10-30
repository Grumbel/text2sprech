# SPDX-FileCopyrightText: 2026 Ingo Ruhnke <grumbel@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later

"""tail2sprech tests; no Qt, Piper or audio device needed.

The end-to-end cases run the real program against a fake piper-server that
returns each sentence's text as its "audio", and a fake player that appends
that "audio" to a log, so the log shows what was spoken and in which order.

Run with: python3 -m unittest discover -s tests -p 'test_*.py'
"""
import os
import signal
import stat
import subprocess
import sys
import tempfile
import textwrap
import time
import unittest
from pathlib import Path

SERVER_DIR = Path(__file__).resolve().parent.parent / "piper_server"
sys.path.insert(0, str(SERVER_DIR))

from tail2sprech import SentenceStream  # noqa: E402

TAIL2SPRECH = SERVER_DIR / "tail2sprech.py"

FAKE_SERVER = textwrap.dedent(
    """\
    #!{python}
    import asyncio, base64, signal, sys
    sys.path.insert(0, {server_dir!r})
    from protocol import read_message, write_message

    async def handle(reader, writer):
        write_message(writer, {{"type": "ready", "voices": ["fake"], "voice": "fake",
                                "backend": "fake", "audio": True, "warning": ""}})
        while (msg := await read_message(reader)) is not None:
            if msg["type"] == "speak":
                if msg["text"] == "Fail.":
                    write_message(writer, {{"type": "error", "id": msg["id"], "message": "boom"}})
                    continue
                data = base64.b64encode(msg["text"].encode()).decode()
                write_message(writer, {{"type": "audio", "id": msg["id"], "format": "wav",
                                        "sample_rate": 22050, "data": data}})
        writer.close()

    async def main():
        await asyncio.start_unix_server(handle, path=sys.argv[sys.argv.index("--socket") + 1])
        stop = asyncio.get_running_loop().create_future()
        asyncio.get_running_loop().add_signal_handler(signal.SIGTERM, stop.set_result, None)
        await stop

    asyncio.run(main())
    """
)

FAKE_PLAYER = textwrap.dedent(
    """\
    #!/bin/sh
    cat "$1" >> "$SPOKEN_LOG"
    echo >> "$SPOKEN_LOG"
    """
)


class SentenceStreamTest(unittest.TestCase):
    def test_holds_newest_sentence_until_proven_complete(self):
        stream = SentenceStream()
        self.assertEqual(stream.feed("Hello there. Sec"), ["Hello there."])
        self.assertEqual(stream.feed("ond one"), [])
        self.assertEqual(stream.feed(" ends.\n"), ["Second one ends."])
        self.assertFalse(stream.has_partial)

    def test_chunking_does_not_change_the_result(self):
        text = "Dr. Smith paid 3.50 dollars. J. R. R. Tolkien wrote. Done\nNext line\n"
        whole = SentenceStream().feed(text)
        stream = SentenceStream()
        by_char = [s for ch in text for s in stream.feed(ch)]
        self.assertEqual(by_char, whole)
        self.assertEqual(whole, ["Dr. Smith paid 3.50 dollars.", "J. R. R. Tolkien wrote.",
                                 "Done", "Next line"])

    def test_newline_completes_a_line_without_punctuation(self):
        self.assertEqual(SentenceStream().feed("build finished\n"), ["build finished"])

    def test_sentence_before_trailing_junk_is_not_held_back(self):
        self.assertEqual(SentenceStream().feed("Hello.\n---"), ["Hello."])

    def test_unspeakable_lines_do_not_accumulate(self):
        stream = SentenceStream()
        self.assertEqual(stream.feed("-----\n"), [])
        self.assertFalse(stream.has_partial)

    def test_flush_returns_partial_text(self):
        stream = SentenceStream()
        self.assertEqual(stream.feed("no newline yet"), [])
        self.assertEqual(stream.flush(), ["no newline yet"])
        self.assertEqual(stream.flush(), [])

    def test_ansi_escapes_are_removed_even_across_reads(self):
        stream = SentenceStream()
        self.assertEqual(stream.feed("\x1b[32mgreen\x1b["), [])
        self.assertEqual(stream.feed("0m text\n"), ["green text"])

    def test_long_unpunctuated_run_is_emitted_in_pieces(self):
        stream = SentenceStream(max_len=20)
        pieces = stream.feed("word " * 20)
        self.assertTrue(pieces)
        self.assertTrue(all(len(piece) <= 20 for piece in pieces))


class EndToEndTest(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.dir = Path(tmp.name)
        self.log = self.dir / "spoken.log"
        self.log.touch()
        self.server = self._script(
            "fake-server", FAKE_SERVER.format(python=sys.executable, server_dir=str(SERVER_DIR))
        )
        self.player = self._script("fake-player", FAKE_PLAYER)
        self.env = {
            **os.environ,
            "SPOKEN_LOG": str(self.log),
            "TEXT2SPRECH_PIPER_SERVER_BIN": str(self.server),
        }

    def _script(self, name, content):
        path = self.dir / name
        path.write_text(content)
        path.chmod(path.stat().st_mode | stat.S_IXUSR)
        return path

    def _command(self, *args):
        return [sys.executable, str(TAIL2SPRECH), "--player", str(self.player), *args]

    def _spoken(self):
        return self.log.read_text().splitlines()

    def _wait_for_spoken(self, expected):
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            if self._spoken() == expected:
                return
            time.sleep(0.02)
        self.assertEqual(self._spoken(), expected)

    def _run(self, text, *args):
        return subprocess.run(
            self._command(*args), input=text, env=self.env, text=True,
            capture_output=True, timeout=20,
        )

    def test_stdin_is_spoken_in_order_and_ends_at_eof(self):
        lines = [f"Line {i}" for i in range(12)]
        result = self._run("\n".join(lines) + "\nNo newline", "--lookahead", "3")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self._spoken(), lines + ["No newline"])
        self.assertEqual(result.stdout.splitlines(), lines + ["No newline"])

    def test_empty_stdin_is_not_an_error(self):
        for _ in range(5):
            result = self._run("")
            self.assertEqual((result.returncode, result.stderr), (0, ""))

    def test_invalid_lines_argument_is_an_error(self):
        path = self.dir / "app.log"
        path.touch()
        result = self._run("", str(path), "-n", "bogus")
        self.assertEqual(result.returncode, 1)
        self.assertIn("input failed", result.stderr)

    def test_quiet_prints_nothing(self):
        result = self._run("Hello\n", "--quiet")
        self.assertEqual(result.stdout, "")
        self.assertEqual(self._spoken(), ["Hello"])

    def test_synthesis_error_skips_only_that_sentence(self):
        result = self._run("One\nFail.\nTwo\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self._spoken(), ["One", "Two"])
        self.assertIn("boom", result.stderr)

    def test_max_backlog_skips_oldest_sentences(self):
        result = self._run("".join(f"Line {i}\n" for i in range(10)),
                           "--max-backlog", "2", "--lookahead", "1")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self._spoken(), ["Line 8", "Line 9"])
        self.assertIn("skipped 8", result.stderr)

    def test_list_voices(self):
        result = self._run("", "--list-voices")
        self.assertEqual(result.stdout, "fake\n")

    def test_unknown_voice_is_an_error(self):
        result = self._run("Hello\n", "--voice", "nope")
        self.assertEqual(result.returncode, 1)
        self.assertIn("unknown voice", result.stderr)
        self.assertEqual(self._spoken(), [])

    def test_missing_file_is_an_error(self):
        result = self._run("", str(self.dir / "nope"))
        self.assertEqual(result.returncode, 1)
        self.assertIn("no such file", result.stderr)

    def test_attached_server_is_left_running(self):
        socket_path = self.dir / "external.sock"
        server = subprocess.Popen([str(self.server), "--socket", str(socket_path)])
        self.addCleanup(server.wait)
        self.addCleanup(server.terminate)
        while not socket_path.exists():
            time.sleep(0.01)
        result = self._run("Hello\n", "--piper-socket", str(socket_path))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self._spoken(), ["Hello"])
        self.assertIsNone(server.poll())

    def _follow(self, *args):
        proc = subprocess.Popen(
            self._command(*args), env=self.env,
            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True,
        )
        self.addCleanup(proc.stderr.close)
        self.addCleanup(proc.wait)
        self.addCleanup(proc.kill)
        return proc

    def _server_pids(self):
        result = subprocess.run(["pgrep", "-f", str(self.server)], capture_output=True, text=True)
        return result.stdout.split()

    def test_follow_file_rotation_partial_line_and_clean_exit(self):
        path = self.dir / "app.log"
        path.write_text("Old one\nOld two\n")
        proc = self._follow(str(path), "-n", "1", "--flush-after", "0.1")
        self._wait_for_spoken(["Old two"])

        with path.open("a") as f:
            f.write("Appended\n")
        self._wait_for_spoken(["Old two", "Appended"])

        with path.open("a") as f:
            f.write("no newline")
        self._wait_for_spoken(["Old two", "Appended", "no newline"])

        path.rename(self.dir / "app.log.1")
        path.write_text("After rotation\n")
        self._wait_for_spoken(["Old two", "Appended", "no newline", "After rotation"])

        self.assertTrue(self._server_pids())
        proc.send_signal(signal.SIGTERM)
        proc.wait(timeout=5)
        self.assertEqual(self._server_pids(), [])
        self.assertNotIn("Traceback", proc.stderr.read())


if __name__ == "__main__":
    unittest.main()

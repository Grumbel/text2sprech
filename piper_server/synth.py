"""Synthesis backends.

There isn't one single stable 'Piper Python API' across all installs -- the
PyPI packaging around Piper has shifted over time, and plenty of setups only
have the `piper` CLI binary available. So we define a small interface and
pick whichever backend actually works at startup, rather than hard-coding
one import path that might not exist on the user's machine.
"""
from __future__ import annotations

import abc
import asyncio
import io
import logging
import os
import shutil
import subprocess
import tempfile
import wave
from dataclasses import dataclass
from pathlib import Path

log = logging.getLogger("piper_server.synth")


@dataclass
class SynthResult:
    wav_bytes: bytes
    sample_rate: int


def _is_speakable(text: str) -> bool:
    """Whether there's anything for Piper to actually say.

    Text with no letters or digits at all -- stray punctuation or symbols
    left over from imperfect HTML-to-text extraction ("---", "***", "»") --
    gets reduced by Piper's own text normalization to zero phonemes. Piper
    then crashes writing a WAV header for zero frames (a confusing
    wave.Error) rather than failing cleanly, so callers should check this
    *before* invoking Piper rather than parsing that crash after the fact.
    """
    return any(ch.isalnum() for ch in text)


def _silent_wav(sample_rate: int = 22050, duration_s: float = 0.05) -> bytes:
    """A brief silent WAV, used in place of a synthesis result for text with
    nothing speakable in it, or as a last-resort fallback if Piper still
    produces no usable audio despite the _is_speakable() check (e.g. text
    that *looks* speakable but Piper's phonemizer still can't handle)."""
    n_frames = int(sample_rate * duration_s)
    buf = io.BytesIO()
    with wave.open(buf, "wb") as wav_file:
        wav_file.setnchannels(1)
        wav_file.setsampwidth(2)
        wav_file.setframerate(sample_rate)
        wav_file.writeframes(b"\x00\x00" * n_frames)
    return buf.getvalue()


def default_user_voices_dir() -> Path:
    """Primary drop-in directory for user-installed Piper voices."""
    xdg = os.environ.get("XDG_DATA_HOME")
    base = Path(xdg) if xdg else Path.home() / ".local" / "share"
    return base / "text2sprech" / "voices"


def resolve_model_dirs(explicit: list[Path] | None = None) -> list[Path]:
    """Ordered list of directories to scan for *.onnx voice models.

    Priority:
      1. explicit paths (CLI --model-dir, repeated)
      2. TEXT2SPRECH_PIPER_MODELS (os.pathsep-separated)
      3. defaults: ~/.local/share/text2sprech/voices, ~/.local/share/piper/voices
    """
    dirs: list[Path] = []
    seen: set[str] = set()

    def add(path: Path) -> None:
        key = str(path.expanduser())
        if key in seen:
            return
        seen.add(key)
        dirs.append(Path(key))

    if explicit:
        for p in explicit:
            add(p)
    env = os.environ.get("TEXT2SPRECH_PIPER_MODELS", "").strip()
    if env:
        for part in env.split(os.pathsep):
            part = part.strip()
            if part:
                add(Path(part))
    if not dirs:
        add(default_user_voices_dir())
        xdg = os.environ.get("XDG_DATA_HOME")
        base = Path(xdg) if xdg else Path.home() / ".local" / "share"
        add(base / "piper" / "voices")
    return dirs


def list_onnx_voices(model_dirs: list[Path]) -> list[str]:
    """Voice names (file stems) found under any model dir; first dir wins ties."""
    found: dict[str, Path] = {}
    for d in model_dirs:
        if not d.is_dir():
            continue
        for p in sorted(d.glob("*.onnx")):
            found.setdefault(p.stem, p)
    return sorted(found.keys())


def find_voice_model(model_dirs: list[Path], voice: str) -> Path:
    for d in model_dirs:
        candidate = d / f"{voice}.onnx"
        if candidate.is_file():
            return candidate
    searched = ", ".join(str(d) for d in model_dirs) or "(none)"
    raise FileNotFoundError(f"voice model not found: {voice}.onnx (searched: {searched})")


class SynthBackend(abc.ABC):
    name: str = "abstract"
    # False for SilentTestBackend — GUI uses this to warn that nothing will
    # be audible even though the protocol still works end-to-end.
    real_audio: bool = True
    # Human-readable summary of search paths (joined); used in ready/warning.
    model_dir: str | None = None

    @abc.abstractmethod
    async def synthesize(self, text: str, voice: str, speed: float) -> SynthResult:
        ...

    @abc.abstractmethod
    def list_voices(self) -> list[str]:
        ...


class PiperPythonBackend(SynthBackend):
    """Uses the piper Python package directly, if importable.

    Kept deliberately defensive: different piper-* PyPI packages expose
    slightly different class names/signatures. We try the most common one
    (`piper.voice.PiperVoice`) and fail fast to CLI fallback otherwise.
    """

    name = "piper-python"
    real_audio = True

    def __init__(self, model_dirs: list[Path]):
        from piper.voice import PiperVoice  # type: ignore  # noqa: F401

        self._PiperVoice = PiperVoice
        self._model_dirs = model_dirs
        self.model_dir = os.pathsep.join(str(d) for d in model_dirs)
        self._voices: dict[str, "PiperVoice"] = {}

    def _load(self, voice: str):
        if voice not in self._voices:
            model_path = find_voice_model(self._model_dirs, voice)
            self._voices[voice] = self._PiperVoice.load(str(model_path))
        return self._voices[voice]

    async def synthesize(self, text: str, voice: str, speed: float) -> SynthResult:
        loop = asyncio.get_running_loop()
        return await loop.run_in_executor(None, self._synthesize_sync, text, voice, speed)

    def _synthesize_sync(self, text: str, voice: str, speed: float) -> SynthResult:
        model = self._load(voice)
        if not _is_speakable(text):
            return SynthResult(wav_bytes=_silent_wav(), sample_rate=model.config.sample_rate)
        buf = io.BytesIO()
        with wave.open(buf, "wb") as wav_file:
            # length_scale is Piper's inverse-speed knob: >1 slower, <1 faster
            model.synthesize(text, wav_file, length_scale=1.0 / max(speed, 0.01))
        return SynthResult(wav_bytes=buf.getvalue(), sample_rate=model.config.sample_rate)

    def list_voices(self) -> list[str]:
        return list_onnx_voices(self._model_dirs)


class PiperCliBackend(SynthBackend):
    """Shells out to the `piper` binary. More portable, slightly higher latency."""

    name = "piper-cli"
    real_audio = True

    def __init__(self, model_dirs: list[Path], binary: str = "piper"):
        if shutil.which(binary) is None:
            raise FileNotFoundError(f"'{binary}' not found on PATH")
        self._binary = binary
        self._model_dirs = model_dirs
        self.model_dir = os.pathsep.join(str(d) for d in model_dirs)

    async def synthesize(self, text: str, voice: str, speed: float) -> SynthResult:
        model_path = find_voice_model(self._model_dirs, voice)
        if not _is_speakable(text):
            return SynthResult(wav_bytes=_silent_wav(), sample_rate=22050)
        with tempfile.NamedTemporaryFile(suffix=".wav") as tmp:
            proc = await asyncio.create_subprocess_exec(
                self._binary,
                "--model", str(model_path),
                "--output_file", tmp.name,
                "--length_scale", str(1.0 / max(speed, 0.01)),
                stdin=asyncio.subprocess.PIPE,
                stdout=asyncio.subprocess.DEVNULL,
                stderr=asyncio.subprocess.PIPE,
            )
            _, stderr = await proc.communicate(text.encode("utf-8"))
            stderr_text = stderr.decode(errors="replace")
            if proc.returncode != 0:
                # This specific crash (Piper's own text normalization
                # reducing input to zero phonemes, then failing to write a
                # WAV header for zero frames) is almost always a text-content
                # issue, not a real configuration/install problem -- the
                # _is_speakable() check above should catch most cases, but
                # text that *looks* speakable can still hit this if Piper's
                # phonemizer can't handle it. Degrade to silence rather than
                # surfacing a scary traceback to the user for what amounts
                # to "nothing came out for one sentence".
                if "channels not specified" in stderr_text or "wave.Error" in stderr_text:
                    log.warning(
                        "piper produced no audio for %r; substituting silence (%s)",
                        text[:80], stderr_text.strip().splitlines()[-1] if stderr_text.strip() else "no stderr",
                    )
                    return SynthResult(wav_bytes=_silent_wav(), sample_rate=22050)
                raise RuntimeError(f"piper CLI failed: {stderr_text}")
            wav_bytes = Path(tmp.name).read_bytes()
        if not wav_bytes:
            log.warning("piper produced an empty WAV for %r; substituting silence", text[:80])
            return SynthResult(wav_bytes=_silent_wav(), sample_rate=22050)
        with wave.open(io.BytesIO(wav_bytes)) as wf:
            sample_rate = wf.getframerate()
        return SynthResult(wav_bytes=wav_bytes, sample_rate=sample_rate)

    def list_voices(self) -> list[str]:
        return list_onnx_voices(self._model_dirs)


class SilentTestBackend(SynthBackend):
    """Generates short silent WAVs. No Piper install required.

    Exists so the server, protocol, and client plumbing can be exercised
    end-to-end (including in this sandbox, which has no Piper install and
    no network to fetch models) without pretending real audio came out.
    The GUI must treat real_audio=False as "not usable for listening".
    """

    name = "silent-test"
    real_audio = False

    def __init__(self, model_dirs: list[Path] | None = None):
        self._model_dirs = model_dirs or []
        self.model_dir = os.pathsep.join(str(d) for d in self._model_dirs) if self._model_dirs else ""
        self._voices = ["test-voice-a", "test-voice-b"]

    async def synthesize(self, text: str, voice: str, speed: float) -> SynthResult:
        sample_rate = 22050
        # ~60ms per character as a stand-in duration, just so timing looks plausible
        duration_s = max(0.3, len(text) * 0.06 / max(speed, 0.01))
        n_frames = int(sample_rate * duration_s)
        buf = io.BytesIO()
        with wave.open(buf, "wb") as wav_file:
            wav_file.setnchannels(1)
            wav_file.setsampwidth(2)
            wav_file.setframerate(sample_rate)
            wav_file.writeframes(b"\x00\x00" * n_frames)
        return SynthResult(wav_bytes=buf.getvalue(), sample_rate=sample_rate)

    def list_voices(self) -> list[str]:
        return list(self._voices)


def select_backend(model_dirs: list[Path]) -> SynthBackend:
    """Pick the best available backend, in order of preference."""
    try:
        return PiperPythonBackend(model_dirs)
    except ImportError:
        pass
    try:
        return PiperCliBackend(model_dirs)
    except FileNotFoundError:
        pass
    return SilentTestBackend(model_dirs)

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
import shutil
import subprocess
import tempfile
import wave
from dataclasses import dataclass
from pathlib import Path


@dataclass
class SynthResult:
    wav_bytes: bytes
    sample_rate: int


class SynthBackend(abc.ABC):
    name: str = "abstract"

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

    def __init__(self, model_dir: Path):
        from piper.voice import PiperVoice  # type: ignore  # noqa: F401

        self._PiperVoice = PiperVoice
        self._model_dir = model_dir
        self._voices: dict[str, "PiperVoice"] = {}

    def _load(self, voice: str):
        if voice not in self._voices:
            model_path = self._model_dir / f"{voice}.onnx"
            if not model_path.exists():
                raise FileNotFoundError(f"voice model not found: {model_path}")
            self._voices[voice] = self._PiperVoice.load(str(model_path))
        return self._voices[voice]

    async def synthesize(self, text: str, voice: str, speed: float) -> SynthResult:
        loop = asyncio.get_running_loop()
        return await loop.run_in_executor(None, self._synthesize_sync, text, voice, speed)

    def _synthesize_sync(self, text: str, voice: str, speed: float) -> SynthResult:
        model = self._load(voice)
        buf = io.BytesIO()
        with wave.open(buf, "wb") as wav_file:
            # length_scale is Piper's inverse-speed knob: >1 slower, <1 faster
            model.synthesize(text, wav_file, length_scale=1.0 / max(speed, 0.01))
        return SynthResult(wav_bytes=buf.getvalue(), sample_rate=model.config.sample_rate)

    def list_voices(self) -> list[str]:
        if not self._model_dir.exists():
            return []
        return sorted(p.stem for p in self._model_dir.glob("*.onnx"))


class PiperCliBackend(SynthBackend):
    """Shells out to the `piper` binary. More portable, slightly higher latency."""

    name = "piper-cli"

    def __init__(self, model_dir: Path, binary: str = "piper"):
        if shutil.which(binary) is None:
            raise FileNotFoundError(f"'{binary}' not found on PATH")
        self._binary = binary
        self._model_dir = model_dir

    async def synthesize(self, text: str, voice: str, speed: float) -> SynthResult:
        model_path = self._model_dir / f"{voice}.onnx"
        if not model_path.exists():
            raise FileNotFoundError(f"voice model not found: {model_path}")
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
            if proc.returncode != 0:
                raise RuntimeError(f"piper CLI failed: {stderr.decode(errors='replace')}")
            wav_bytes = Path(tmp.name).read_bytes()
        with wave.open(io.BytesIO(wav_bytes)) as wf:
            sample_rate = wf.getframerate()
        return SynthResult(wav_bytes=wav_bytes, sample_rate=sample_rate)

    def list_voices(self) -> list[str]:
        if not self._model_dir.exists():
            return []
        return sorted(p.stem for p in self._model_dir.glob("*.onnx"))


class SilentTestBackend(SynthBackend):
    """Generates short silent WAVs. No Piper install required.

    Exists so the server, protocol, and client plumbing can be exercised
    end-to-end (including in this sandbox, which has no Piper install and
    no network to fetch models) without pretending real audio came out.
    """

    name = "silent-test"

    def __init__(self):
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


def select_backend(model_dir: Path) -> SynthBackend:
    """Pick the best available backend, in order of preference."""
    try:
        return PiperPythonBackend(model_dir)
    except ImportError:
        pass
    try:
        return PiperCliBackend(model_dir)
    except FileNotFoundError:
        pass
    return SilentTestBackend()

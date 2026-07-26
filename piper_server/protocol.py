# SPDX-FileCopyrightText: 2026 Ingo Ruhnke <grumbel@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later

"""Length-prefixed JSON framing over asyncio streams.

Wire format: [4-byte big-endian uint32 length][UTF-8 JSON payload]
See PROTOCOL.md at the project root for the message schema.
"""
from __future__ import annotations

import asyncio
import json
import struct
from typing import Any

HEADER = struct.Struct(">I")
MAX_FRAME_BYTES = 64 * 1024 * 1024  # 64MB sanity cap; refuses to allocate more


class FrameError(Exception):
    pass


async def read_message(reader: asyncio.StreamReader) -> dict[str, Any] | None:
    """Read one framed JSON message. Returns None on clean EOF."""
    header = await _read_exact(reader, HEADER.size)
    if header is None:
        return None
    (length,) = HEADER.unpack(header)
    if length > MAX_FRAME_BYTES:
        raise FrameError(f"frame length {length} exceeds max {MAX_FRAME_BYTES}")
    body = await _read_exact(reader, length)
    if body is None:
        raise FrameError("connection closed mid-frame")
    try:
        return json.loads(body.decode("utf-8"))
    except (json.JSONDecodeError, UnicodeDecodeError) as e:
        raise FrameError(f"invalid JSON frame: {e}") from e


async def _read_exact(reader: asyncio.StreamReader, n: int) -> bytes | None:
    if n == 0:
        return b""
    try:
        buf = await reader.readexactly(n)
    except asyncio.IncompleteReadError as e:
        if len(e.partial) == 0:
            return None  # clean EOF at a frame boundary
        raise FrameError("connection closed mid-frame") from e
    return buf


def write_message(writer: asyncio.StreamWriter, message: dict[str, Any]) -> None:
    body = json.dumps(message).encode("utf-8")
    writer.write(HEADER.pack(len(body)))
    writer.write(body)

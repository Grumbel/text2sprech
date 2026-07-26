# SPDX-FileCopyrightText: 2026 Ingo Ruhnke <grumbel@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later

r"""Heuristic sentence splitter.

Naive splitting on '. ' breaks on abbreviations ("Dr. Smith"), decimals
("3.5 km/s"), initials ("J. R. R. Tolkien"), and ellipses. This is a
pragmatic heuristic, not a full NLP sentence tokenizer -- it will still get
edge cases wrong (nested quotes, unusual abbreviations), but it's a large
improvement over splitting on bare periods and is cheap enough to run
inline in the UI thread for a document load.

Two additional passes on top of punctuation-based splitting:

1. Newlines are always a hard boundary, independent of punctuation. Plain
   `\s+` in the punctuation regex swallows newlines silently, so text with
   no terminal punctuation (a list, a poem, dialogue without periods) used
   to come out as one giant segment. Known tradeoff: a text file that's
   hard-wrapped at some column width (rather than using real paragraph
   breaks) will now be chopped into a segment per line -- there's no way to
   distinguish "line break because that's a new list item" from "line break
   because the file wraps at 80 columns" from the text alone.
2. Any resulting segment still longer than `max_len` (default 280 chars) --
   i.e. a very long run with no punctuation or newlines at all -- gets
   further subdivided at the nearest whitespace, so a segment can't grow
   unboundedly long just because the source text has no natural breaks.
"""
from __future__ import annotations

import re
from dataclasses import dataclass

_ABBREVIATIONS = {
    "mr", "mrs", "ms", "dr", "prof", "sr", "jr", "st", "vs", "etc",
    "e.g", "i.e", "fig", "vol", "no", "approx", "gen", "col", "lt",
    "capt", "cmdr", "gov", "rep", "sen", "rev", "inc", "ltd", "co",
}

# Sentence-ending punctuation, optionally followed by closing quotes/brackets,
# followed by whitespace and a capital letter or opening quote (next sentence).
_BOUNDARY = re.compile(
    r"""
    (?P<end>[.!?]+)          # one or more terminal punctuation marks
    (?P<close>["'”’)\]]*)    # optional closing quotes/brackets
    (?P<space>\s+)
    (?=[A-Z"'“‘(\[]|$)       # lookahead: capital letter, opening quote, or end
    """,
    re.VERBOSE,
)

_DECIMAL = re.compile(r"\d\.\d")
_NEWLINE_RUN = re.compile(r"\n+")

DEFAULT_MAX_LEN = 280


@dataclass
class Sentence:
    text: str
    start: int  # offset into the original string
    end: int    # exclusive


def split_sentences(text: str, max_len: int = DEFAULT_MAX_LEN) -> list[Sentence]:
    if not text:
        return []

    boundaries: set[int] = set()

    for m in _BOUNDARY.finditer(text):
        preceding_word = _last_word(text, m.start("end"))
        bare_word = preceding_word.rstrip(".")
        if bare_word.lower() in _ABBREVIATIONS:
            continue
        if len(bare_word) == 1 and bare_word.isupper():
            continue  # single-letter initial, e.g. "J. R. R. Tolkien"
        if _DECIMAL.match(text, m.start("end") - 1):
            continue
        boundaries.add(m.end())

    for m in _NEWLINE_RUN.finditer(text):
        boundaries.add(m.end())

    sorted_boundaries = sorted(b for b in boundaries if 0 < b < len(text))

    # Raw (untrimmed) ranges covering the whole text contiguously.
    raw_ranges: list[tuple[int, int]] = []
    start = 0
    for end in sorted_boundaries:
        raw_ranges.append((start, end))
        start = end
    raw_ranges.append((start, len(text)))

    sentences: list[Sentence] = []
    for raw_start, raw_end in raw_ranges:
        for sub_start, sub_end in _split_long_range(text, raw_start, raw_end, max_len):
            chunk = text[sub_start:sub_end]
            stripped = chunk.strip()
            if not stripped:
                continue
            if not any(ch.isalnum() for ch in stripped):
                # No letters or digits at all -- e.g. stray punctuation or
                # a leftover HTML entity/symbol from imperfect HTML-to-text
                # extraction ("---", "»", "&nbsp;" if it wasn't decoded).
                # Piper's own text normalization reduces these to zero
                # phonemes and then crashes trying to write a WAV header
                # for zero frames, so skip sending them at all rather than
                # relying on the server to degrade gracefully.
                continue
            offset = sub_start + chunk.index(stripped[0])
            sentences.append(Sentence(text=stripped, start=offset, end=offset + len(stripped)))

    return sentences


def _split_long_range(text: str, start: int, end: int, max_len: int) -> list[tuple[int, int]]:
    """Subdivide [start, end) so no piece exceeds max_len, breaking at
    whitespace when one is available; hard-cuts only as a last resort."""
    if max_len <= 0 or end - start <= max_len:
        return [(start, end)]

    pieces: list[tuple[int, int]] = []
    pos = start
    while end - pos > max_len:
        search_limit = pos + max_len
        # Look for the last whitespace at or before the limit, but after pos.
        break_at = -1
        for i in range(search_limit, pos, -1):
            if text[i - 1].isspace():
                break_at = i - 1
                break
        if break_at <= pos:
            break_at = search_limit  # no whitespace to break on; hard cut
        pieces.append((pos, break_at))
        pos = break_at
    pieces.append((pos, end))
    return pieces


def _last_word(text: str, pos: int) -> str:
    """The word immediately before position `pos` (the period itself)."""
    j = pos
    i = j
    while i > 0 and (text[i - 1].isalnum() or text[i - 1] == "."):
        i -= 1
    return text[i:j]

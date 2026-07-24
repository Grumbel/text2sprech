"""Heuristic sentence splitter.

Naive splitting on '. ' breaks on abbreviations ("Dr. Smith"), decimals
("3.5 km/s"), initials ("J. R. R. Tolkien"), and ellipses. This is a
pragmatic heuristic, not a full NLP sentence tokenizer -- it will still get
edge cases wrong (nested quotes, unusual abbreviations), but it's a large
improvement over splitting on bare periods and is cheap enough to run
inline in the UI thread for a document load.
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


@dataclass
class Sentence:
    text: str
    start: int  # offset into the original string
    end: int    # exclusive


def split_sentences(text: str) -> list[Sentence]:
    if not text:
        return []

    boundaries: list[int] = []  # end offsets (exclusive, after trailing space)
    for m in _BOUNDARY.finditer(text):
        end_pos = m.end()
        preceding_word = _last_word(text, m.start("end"))
        bare_word = preceding_word.rstrip(".")
        if bare_word.lower() in _ABBREVIATIONS:
            continue
        if len(bare_word) == 1 and bare_word.isupper():
            continue  # single-letter initial, e.g. "J. R. R. Tolkien"
        if _DECIMAL.match(text, m.start("end") - 1):
            continue
        boundaries.append(end_pos)

    sentences: list[Sentence] = []
    start = 0
    for end in boundaries:
        chunk = text[start:end]
        stripped = chunk.strip()
        if stripped:
            offset = start + chunk.index(stripped[0])
            sentences.append(Sentence(text=stripped, start=offset, end=offset + len(stripped)))
        start = end
    tail = text[start:].strip()
    if tail:
        offset = start + text[start:].index(tail[0])
        sentences.append(Sentence(text=tail, start=offset, end=offset + len(tail)))
    return sentences


def _last_word(text: str, pos: int) -> str:
    """The word immediately before position `pos` (the period itself)."""
    j = pos
    i = j
    while i > 0 and (text[i - 1].isalnum() or text[i - 1] == "."):
        i -= 1
    return text[i:j]

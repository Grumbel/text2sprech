#pragma once

#include <QString>
#include <QVector>
#include "Sentence.h"

// Heuristic sentence splitter -- mirrors piper_server/sentence_splitter.py.
// Keep the two in sync if you tune the abbreviation list or boundary rules;
// see that file's docstring for why this is a heuristic and not a full
// tokenizer (abbreviations, decimals, initials are handled; unusual
// abbreviations and nested quotes are known gaps).
class SentenceSplitter {
public:
    // ids are assigned sequentially starting at startId, in document order.
    static QVector<Sentence> split(const QString &text, int startId = 0);

private:
    static bool endsWithAbbreviation(const QString &text, int periodPos);
    static bool isDecimalPoint(const QString &text, int periodPos);
    static QString lastWord(const QString &text, int pos);
};

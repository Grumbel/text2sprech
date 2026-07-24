#include "SentenceSplitter.h"

#include <QRegularExpression>
#include <QSet>

namespace {

const QSet<QString> &abbreviations()
{
    static const QSet<QString> kAbbrevs = {
        "mr", "mrs", "ms", "dr", "prof", "sr", "jr", "st", "vs", "etc",
        "e.g", "i.e", "fig", "vol", "no", "approx", "gen", "col", "lt",
        "capt", "cmdr", "gov", "rep", "sen", "rev", "inc", "ltd", "co",
    };
    return kAbbrevs;
}

// Matches: terminal punctuation, optional closing quote/bracket, whitespace,
// followed by (lookahead) a capital letter / opening quote / end of string.
const QRegularExpression &boundaryRegex()
{
    static const QRegularExpression kRe(
        QStringLiteral("[.!?]+[\"'\\x{201d}\\x{2019})\\]]*\\s+(?=[A-Z\"'\\x{201c}\\x{2018}(\\[]|$)"));
    return kRe;
}

} // namespace

QString SentenceSplitter::lastWord(const QString &text, int pos)
{
    int i = pos;
    while (i > 0 && (text[i - 1].isLetterOrNumber() || text[i - 1] == QLatin1Char('.'))) {
        --i;
    }
    return text.mid(i, pos - i);
}

bool SentenceSplitter::endsWithAbbreviation(const QString &text, int periodPos)
{
    QString word = lastWord(text, periodPos);
    QString bare = word;
    while (bare.endsWith(QLatin1Char('.'))) {
        bare.chop(1);
    }
    if (bare.isEmpty()) {
        return false;
    }
    if (abbreviations().contains(bare.toLower())) {
        return true;
    }
    if (bare.size() == 1 && bare[0].isUpper()) {
        return true; // single-letter initial, e.g. "J. R. R. Tolkien"
    }
    return false;
}

bool SentenceSplitter::isDecimalPoint(const QString &text, int periodPos)
{
    // periodPos points at the first char of the punctuation run; check
    // for digit-dot-digit straddling it.
    if (periodPos <= 0 || periodPos + 1 >= text.size()) {
        return false;
    }
    return text[periodPos - 1].isDigit() && text[periodPos] == QLatin1Char('.')
        && text[periodPos + 1].isDigit();
}

QVector<Sentence> SentenceSplitter::split(const QString &text, int startId)
{
    QVector<Sentence> result;
    if (text.isEmpty()) {
        return result;
    }

    QVector<int> boundaryEnds; // exclusive end offsets, after trailing whitespace
    auto it = boundaryRegex().globalMatch(text);
    while (it.hasNext()) {
        auto m = it.next();
        int punctStart = m.capturedStart(0);
        if (endsWithAbbreviation(text, punctStart) || isDecimalPoint(text, punctStart)) {
            continue;
        }
        boundaryEnds.append(m.capturedEnd(0));
    }

    int start = 0;
    int nextId = startId;
    auto emitChunk = [&](int chunkStart, int chunkEnd) {
        QString chunk = text.mid(chunkStart, chunkEnd - chunkStart);
        QString trimmed = chunk.trimmed();
        if (trimmed.isEmpty()) {
            return;
        }
        int leadingWs = chunk.indexOf(trimmed.left(1));
        int offset = chunkStart + (leadingWs < 0 ? 0 : leadingWs);
        result.append(Sentence{nextId++, offset, offset + static_cast<int>(trimmed.size()), trimmed});
    };

    for (int end : boundaryEnds) {
        emitChunk(start, end);
        start = end;
    }
    emitChunk(start, text.size());

    return result;
}

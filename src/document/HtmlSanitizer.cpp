// SPDX-FileCopyrightText: 2026 Ingo Ruhnke <grumbel@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "HtmlSanitizer.h"

#include <QFont>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextDocument>
#include <QTextFragment>
#include <QTextList>
#include <QTextListFormat>

namespace HtmlSanitizer {
namespace {

QString escapeText(const QString &text)
{
    QString out;
    out.reserve(text.size());
    for (QChar c : text) {
        switch (c.unicode()) {
        case '&':
            out += QLatin1String("&amp;");
            break;
        case '<':
            out += QLatin1String("&lt;");
            break;
        case '>':
            out += QLatin1String("&gt;");
            break;
        case '"':
            out += QLatin1String("&quot;");
            break;
        default:
            out += c;
            break;
        }
    }
    return out;
}

QString escapeAttr(const QString &text)
{
    return escapeText(text);
}

// Map a single text fragment to semantic inline markup.
QString formatFragment(const QTextFragment &frag)
{
    QString text = frag.text();
    if (text.isEmpty()) {
        return {};
    }

    // Object replacement characters stand in for images / widgets — drop.
    text.remove(QChar::ObjectReplacementCharacter);
    if (text.isEmpty()) {
        return {};
    }

    // Preserve hard line breaks encoded as Unicode line/paragraph separators
    // as <br>; QTextDocument uses U+2028 / U+2029 and '\n' inside blocks.
    text.replace(QChar::LineSeparator, QLatin1Char('\n'));
    text.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));

    const QTextCharFormat fmt = frag.charFormat();
    // Split on newlines so we can emit <br> between runs without wrapping
    // the whole fragment in emphasis tags incorrectly across breaks.
    const QStringList parts = text.split(QLatin1Char('\n'));
    QString combined;
    for (int i = 0; i < parts.size(); ++i) {
        if (i > 0) {
            combined += QLatin1String("<br/>");
        }
        QString piece = escapeText(parts[i]);
        if (piece.isEmpty()) {
            continue;
        }

        // QFont::Bold is 75; some paths report CSS-style 700.
        const bool bold = fmt.fontWeight() >= QFont::Bold || fmt.fontWeight() >= 700;
        const bool italic = fmt.fontItalic();
        const bool monospace =
            fmt.fontFixedPitch() || fmt.fontStyleHint() == QFont::Monospace;

        if (monospace) {
            piece = QLatin1String("<code>") + piece + QLatin1String("</code>");
        }
        if (bold) {
            piece = QLatin1String("<strong>") + piece + QLatin1String("</strong>");
        }
        if (italic) {
            piece = QLatin1String("<em>") + piece + QLatin1String("</em>");
        }
        if (fmt.isAnchor() && !fmt.anchorHref().isEmpty()) {
            piece = QLatin1String("<a href=\"") + escapeAttr(fmt.anchorHref()) + QLatin1String("\">")
                + piece + QLatin1String("</a>");
        }
        combined += piece;
    }
    return combined;
}

QString formatBlockInner(const QTextBlock &block)
{
    QString inner;
    for (auto it = block.begin(); !(it.atEnd()); ++it) {
        const QTextFragment frag = it.fragment();
        if (!frag.isValid()) {
            continue;
        }
        inner += formatFragment(frag);
    }
    return inner;
}

QString listTagFor(const QTextList *list)
{
    if (!list) {
        return QStringLiteral("ul");
    }
    switch (list->format().style()) {
    case QTextListFormat::ListDecimal:
    case QTextListFormat::ListLowerAlpha:
    case QTextListFormat::ListUpperAlpha:
    case QTextListFormat::ListLowerRoman:
    case QTextListFormat::ListUpperRoman:
        return QStringLiteral("ol");
    default:
        return QStringLiteral("ul");
    }
}

} // namespace

QString toSemanticHtml(const QString &html)
{
    if (html.trimmed().isEmpty()) {
        return {};
    }

    QTextDocument doc;
    doc.setHtml(html);

    QString out;
    QTextList *openList = nullptr;
    QString openListTag;

    auto closeList = [&] {
        if (openList) {
            out += QLatin1String("</") + openListTag + QLatin1String(">\n");
            openList = nullptr;
            openListTag.clear();
        }
    };

    for (QTextBlock block = doc.begin(); block.isValid(); block = block.next()) {
        QTextList *list = block.textList();
        const QString inner = formatBlockInner(block);
        // Skip empty non-list blocks (common trailing noise from rich paste).
        if (!list && inner.trimmed().isEmpty()) {
            // Still close a list if we left one.
            if (openList && (!list || list != openList)) {
                // empty paragraph between list items handled below
            }
            if (!list) {
                if (openList) {
                    closeList();
                }
                continue;
            }
        }

        if (openList && list != openList) {
            closeList();
        }
        if (list && list != openList) {
            openList = list;
            openListTag = listTagFor(list);
            out += QLatin1Char('<') + openListTag + QLatin1String(">\n");
        }

        if (list) {
            out += QLatin1String("<li>") + inner + QLatin1String("</li>\n");
            continue;
        }

        const int heading = block.blockFormat().headingLevel();
        if (heading >= 1 && heading <= 6) {
            out += QLatin1String("<h") + QString::number(heading) + QLatin1Char('>') + inner
                + QLatin1String("</h") + QString::number(heading) + QLatin1String(">\n");
        } else {
            out += QLatin1String("<p>") + inner + QLatin1String("</p>\n");
        }
    }
    closeList();
    return out;
}

} // namespace HtmlSanitizer

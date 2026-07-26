#pragma once

#include <QString>

// Reduce arbitrary HTML to a small semantic subset suitable for a TTS
// reader (think "reader mode"): structure and emphasis only, no visual
// styling, no images, no scripts.
//
// Qt's QTextDocument already parses a limited HTML subset; we re-emit from
// the resulting document model so author-supplied style attributes, font
// tags, colours, sizes, and embedded objects never reach DocumentView.
//
// Allowed output tags:
//   h1–h6, p, br, ul, ol, li, strong, em, code, a (href only)
namespace HtmlSanitizer {

QString toSemanticHtml(const QString &html);

} // namespace HtmlSanitizer

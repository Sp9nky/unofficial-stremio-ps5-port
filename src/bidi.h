// Right-to-left text (Arabic, Hebrew, ...) for the UI's text renderer, which
// lays glyphs out left to right and doesn't join Arabic letters itself: each
// line is reordered into display order and Arabic letters are replaced by
// their joined forms (Presentation Forms), with FriBidi. Without it, Arabic
// subtitles were missing glyphs (white boxes, issue #1) and, with a font,
// would have shown separate letters in reverse.
//
// This runs once, in the font (ui/dynamic_font.cpp), when a text is measured or drawn: text given to the
// fonts stays in logical order (subtitles included), or it would be reordered twice and come out backwards.
#pragma once

#include <string>

// True if the UTF-8 text has right-to-left letters.
bool has_rtl(const std::string& utf8);

// One line of plain UTF-8 text in display order, Arabic letters joined.
// Text without right-to-left letters comes back unchanged.
std::string bidi_visual(const std::string& utf8);

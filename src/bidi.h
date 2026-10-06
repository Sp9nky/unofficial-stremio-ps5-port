// Right-to-left text (Arabic, Hebrew, ...) for the UI's text renderer, which
// lays glyphs out left to right and doesn't join Arabic letters itself: each
// line is reordered into display order and Arabic letters are replaced by
// their joined forms (Presentation Forms), with FriBidi. Without it, Arabic
// subtitles were missing glyphs (white boxes, issue #1) and, with a font,
// would have shown separate letters in reverse.
#pragma once

#include <string>

// True if the UTF-8 text has right-to-left letters.
bool has_rtl(const std::string& utf8);

// One line of plain UTF-8 text in display order, Arabic letters joined.
// Text without right-to-left letters comes back unchanged.
std::string bidi_visual(const std::string& utf8);

// The same for subtitle RML (escaped text, <em>/<strong>, <br/> between
// lines): each line with right-to-left letters is reordered; its inline
// tags are dropped, keeping italics/bold for the whole line.
std::string bidi_visual_rml(const std::string& rml);

#pragma once
//
// Emoji rendering, as an LVGL fallback font.
//
// A message that arrives from the phone app carries real UTF-8 emoji. Montserrat
// has no glyph for U+1F44D, so LVGL drew its placeholder box and a thumbs-up
// looked like a fault in the firmware.
//
// The obvious fix -- ship an emoji font -- is the wrong one here. Colour emoji
// fonts are megabytes, LVGL's font engine is alpha-only so the colour would be
// discarded anyway, and subsetting a monochrome face means a new binary asset,
// a new generator, and a new thing to keep in step with the picker.
//
// Meshtastic already solved it. graphics/emotes.cpp pairs 146 UTF-8 code points
// with 16x16 monochrome bitmaps, compiled into the firmware, maintained upstream
// and shared with the phone app's own tapback set. This file wraps that table in
// an lv_font_t.
//
// It is installed as the FALLBACK of the normal text fonts, not as a font
// anything selects. lv_font_get_glyph_dsc() walks the fallback chain whenever
// the primary font reports a placeholder, so emoji render in message bubbles,
// contact names, and the composer without a single call site knowing they exist.
//
// See also: patches/upstream/0004-emotes-require-screen, which is what makes the
// table available in a build with the stock UI compiled out.

#include <stdint.h>

struct _lv_font_t;
typedef struct _lv_font_t lv_font_t;

namespace pgros {
namespace emoji {

// The fallback font. Never null; if the upstream table is empty the font simply
// reports no glyphs and the placeholder box comes back.
//
// Glyphs are 16x16 with a 17px advance, sized against Montserrat 14 (line height
// 16, base line 3) so an emoji fills the text line exactly rather than
// overhanging it.
const lv_font_t *font();

// --- picker data ----------------------------------------------------------
//
// The same table, addressed by index, so the picker grid and the renderer can
// never disagree about what this device can draw.

// Number of distinct emoji offered by the picker. Upstream's table contains
// aliases -- several code points mapping to one bitmap, and a few code points
// carrying a variation selector -- which are collapsed here.
uint16_t count();

// UTF-8 text for entry i, NUL terminated, suitable for appending to a draft.
// Returns "" when i is out of range.
const char *text(uint16_t i);

} // namespace emoji
} // namespace pgros

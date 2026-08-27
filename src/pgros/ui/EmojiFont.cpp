#ifdef PGROS

#include "ui/EmojiFont.h"

#include "configuration.h"
#include "graphics/emotes.h"

#include <lvgl.h>
#include <string.h>

namespace pgros
{
namespace emoji
{

namespace
{

// Glyph box. Upstream's bitmaps are all 16x16; anything else in the table is
// skipped rather than drawn wrong.
constexpr int kGlyphW = 16;
constexpr int kGlyphH = 16;

// Metrics matched to Montserrat 14 (line_height 16, base_line 3).
//
// LVGL places a glyph at y1 = line_top + (line_height - base_line) - box_h -
// ofs_y. With ofs_y = -3 that is line_top exactly, so a 16px emoji fills the
// 16px line instead of hanging below it into the next one.
constexpr int kOfsY = -3;
constexpr int kAdvance = kGlyphW + 1;

struct Entry {
    uint32_t cp;      // first code point of the label
    uint16_t emote;   // index into graphics::emotes[]
};

// Upstream's table is the upper bound; the deduped list is always smaller.
constexpr uint16_t kMaxEntries = 192;

Entry sEntries[kMaxEntries];
uint16_t sCount = 0;
bool sReady = false;

// --- UTF-8 ----------------------------------------------------------------

// Decodes one code point. Returns 0 on a malformed or truncated sequence, which
// is treated as "no emoji here" rather than as an error worth reporting: the
// table is a compile-time constant and cannot be malformed in practice.
uint32_t firstCodepoint(const char *s)
{
    if (!s || !s[0])
        return 0;

    const uint8_t *p = (const uint8_t *)s;
    if (p[0] < 0x80)
        return p[0];

    if ((p[0] & 0xE0) == 0xC0)
        return (p[1] & 0xC0) != 0x80 ? 0 : (uint32_t)((p[0] & 0x1F) << 6 | (p[1] & 0x3F));

    if ((p[0] & 0xF0) == 0xE0) {
        if ((p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80)
            return 0;
        return (uint32_t)((p[0] & 0x0F) << 12 | (p[1] & 0x3F) << 6 | (p[2] & 0x3F));
    }

    if ((p[0] & 0xF8) == 0xF0) {
        if ((p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80 || (p[3] & 0xC0) != 0x80)
            return 0;
        return (uint32_t)((p[0] & 0x07) << 18 | (p[1] & 0x3F) << 12 | (p[2] & 0x3F) << 6 | (p[3] & 0x3F));
    }

    return 0;
}

// Code points that modify the glyph before them rather than drawing anything of
// their own. They are extremely common inside emoji sequences -- upstream's own
// labels carry U+FE0F -- and without this every "✔️" would render as a check
// mark followed by a placeholder box.
bool isZeroWidth(uint32_t cp)
{
    return cp == 0xFE0F ||                    // variation selector-16, "render as emoji"
           cp == 0xFE0E ||                    // variation selector-15, "render as text"
           cp == 0x200D ||                    // zero-width joiner
           (cp >= 0x1F3FB && cp <= 0x1F3FF);  // skin tone modifiers
}

// --- table ----------------------------------------------------------------

void build()
{
    if (sReady)
        return;
    sReady = true;

    for (int i = 0; i < graphics::numEmotes && sCount < kMaxEntries; ++i) {
        const graphics::Emote &e = graphics::emotes[i];
        if (!e.bitmap || e.width != kGlyphW || e.height != kGlyphH)
            continue;

        const uint32_t cp = firstCodepoint(e.label);
        if (!cp || isZeroWidth(cp))
            continue;

        // Collapse aliases. Upstream lists some emoji twice -- bare and with a
        // variation selector -- and they are one glyph and one picker entry.
        bool seen = false;
        for (uint16_t j = 0; j < sCount; ++j) {
            if (sEntries[j].cp == cp) {
                seen = true;
                break;
            }
        }
        if (seen)
            continue;

        sEntries[sCount].cp = cp;
        sEntries[sCount].emote = (uint16_t)i;
        sCount++;
    }

    LOG_INFO("PgrOS: emoji font ready, %u glyphs from %d upstream emotes", (unsigned)sCount, graphics::numEmotes);
}

// Linear, because this only ever runs for a code point the primary font could
// not supply -- emoji and genuinely missing characters. Ordinary text never
// reaches the fallback chain at all, so a sorted index would buy nothing.
int find(uint32_t cp)
{
    for (uint16_t i = 0; i < sCount; ++i)
        if (sEntries[i].cp == cp)
            return (int)i;
    return -1;
}

// --- lv_font_t callbacks ---------------------------------------------------

bool getGlyphDsc(const lv_font_t *, lv_font_glyph_dsc_t *dsc, uint32_t letter, uint32_t)
{
    build();

    if (isZeroWidth(letter)) {
        // Claim it and draw nothing. Reporting "not found" would send LVGL on to
        // the placeholder box, which is exactly the artefact this avoids.
        dsc->adv_w = 0;
        dsc->box_w = 0;
        dsc->box_h = 0;
        dsc->ofs_x = 0;
        dsc->ofs_y = 0;
        dsc->format = LV_FONT_GLYPH_FORMAT_NONE;
        dsc->is_placeholder = 0;
        dsc->gid.index = 0;
        return true;
    }

    const int idx = find(letter);
    if (idx < 0)
        return false;

    dsc->adv_w = kAdvance;
    dsc->box_w = kGlyphW;
    dsc->box_h = kGlyphH;
    dsc->ofs_x = 0;
    dsc->ofs_y = kOfsY;
    dsc->format = LV_FONT_GLYPH_FORMAT_A8;
    dsc->is_placeholder = 0;
    dsc->gid.index = (uint32_t)idx;
    return true;
}

// Expands the 1bpp source into the A8 buffer LVGL hands us.
//
// The bitmaps are XBM: two bytes per row, least significant bit leftmost. LVGL
// wants one alpha byte per pixel, so the conversion is a plain per-pixel
// expansion to 0x00 or 0xFF. The draw layer then blends that as a mask in the
// current text colour, which is why emoji inherit the colour of the text around
// them instead of needing their own palette.
const void *getGlyphBitmap(lv_font_glyph_dsc_t *dsc, lv_draw_buf_t *draw_buf)
{
    if (!draw_buf || !draw_buf->data)
        return NULL;
    if (dsc->gid.index >= sCount)
        return NULL;

    const graphics::Emote &e = graphics::emotes[sEntries[dsc->gid.index].emote];
    const uint8_t *src = (const uint8_t *)e.bitmap;
    const uint32_t stride = draw_buf->header.stride;
    uint8_t *dst = (uint8_t *)draw_buf->data;

    for (int y = 0; y < kGlyphH; ++y) {
        uint8_t *row = dst + (uint32_t)y * stride;
        const uint8_t *srow = src + y * 2;
        for (int x = 0; x < kGlyphW; ++x)
            row[x] = (srow[x >> 3] >> (x & 7)) & 1 ? 0xFF : 0x00;
    }

    return draw_buf;
}

lv_font_t sFont;
bool sFontInit = false;

} // namespace

const lv_font_t *font()
{
    if (!sFontInit) {
        sFontInit = true;
        build();

        memset(&sFont, 0, sizeof(sFont));
        sFont.get_glyph_dsc = getGlyphDsc;
        sFont.get_glyph_bitmap = getGlyphBitmap;
        sFont.line_height = kGlyphH;
        sFont.base_line = 0;
        sFont.subpx = LV_FONT_SUBPX_NONE;
        sFont.kerning = LV_FONT_KERNING_NONE;

        // 0: the bitmap is generated into LVGL's buffer on demand rather than
        // living in flash in a drawable layout. Claiming otherwise makes the SW
        // renderer take a fast path that reads our source XBM as if it were A8.
        sFont.static_bitmap = 0;
    }
    return &sFont;
}

uint16_t count()
{
    build();
    return sCount;
}

const char *text(uint16_t i)
{
    build();
    if (i >= sCount)
        return "";
    return graphics::emotes[sEntries[i].emote].label;
}

} // namespace emoji
} // namespace pgros

#endif // PGROS

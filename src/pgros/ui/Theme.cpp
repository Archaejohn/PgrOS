#ifdef PGROS
//
// PgrOS visual language, implemented.
//
// ---------------------------------------------------------------------------
// Palette notes
// ---------------------------------------------------------------------------
//
// Panel: ST7796, RGB565, bright, small, viewed outdoors and in the dark.
//
// * No pure #000000 background. This panel bands visibly in the bottom two or
//   three steps of the value ramp, and RGB565 has only 32 blue levels to begin
//   with; a near-black with a slight blue cast (#0E1117) keeps gradients and
//   antialiased glyph edges from posterising into blocks.
// * No pure #FFFFFF text. At this pixel density full white on near-black
//   blooms and leaves a purple fringe on the subpixel edges. #E8EDF2 measures
//   as "white" to the eye and is markedly calmer to read a message thread in.
// * Every step in the surface ramp (bg -> surface -> surfaceAlt) is a real,
//   visible step at 16-bit depth. Two greys three units apart are the same
//   colour once quantised, which is how dark UIs end up looking flat.
//
// ---------------------------------------------------------------------------
// The accent: amber #FFB020
// ---------------------------------------------------------------------------
//
// Deliberate, and picked against two constraints rather than on taste:
//
//   1. It must not collide with status semantics. The UI carries a
//      traffic-light vocabulary it cannot give up -- green delivered, red
//      failed, and a middle state for queued/weak signal. An accent drawn from
//      that same family makes a focus ring read as a warning. So green, red
//      and true orange are all out as brand colours.
//   2. Blue and cyan are already spoken for by the radios: the Bluetooth glyph
//      is blue everywhere on earth, and a cyan accent next to a cyan BT icon
//      is ambiguous at 14px. Cyan is also the default of every ESP32 LVGL demo,
//      which is precisely the look this is trying not to have.
//
// Amber is what is left, and it happens to be right: it is the colour of
// instrument panels and of the pagers this device descends from, it has the
// highest luminance of any saturated hue so it survives sunlight, and it sits
// far from the green/red axis where colour-blind users lose discrimination.
// The one hazard -- amber accent versus amber "warn" -- is handled by pushing
// warn to a distinctly redder orange (#FF7A29); the two never appear adjacent.
//
// Outbound message bubbles use a slightly deepened amber (#E09A12) rather than
// the accent itself, so that a focus ring drawn on a bubble stays visible
// against it.

#include "Theme.h"
#include "configuration.h"
#include <lvgl.h>

namespace pgros {

Theme theme;

// ---------------------------------------------------------------------------
// Shared styles.
//
// These are file-scope rather than members so that Theme.h stays free of an
// <lvgl.h> include (it is pulled in by nearly everything). They are shared
// objects: every widget that used lv_obj_add_style() holds a pointer, so
// rebuilding them on a theme switch updates the whole tree at once, which is
// the entire reason setDark() can work without rebuilding widgets.
// ---------------------------------------------------------------------------
namespace {

lv_style_t sScreen;
lv_style_t sCard;
lv_style_t sRow;
lv_style_t sRowSel; // applied for LV_STATE_FOCUSED|CHECKED on top of sRow
lv_style_t sBubbleIn;
lv_style_t sBubbleOut;
lv_style_t sBtn;
lv_style_t sBtnPrimary;
lv_style_t sInput;
lv_style_t sLabelDim;

bool sStylesInited = false;

inline lv_color_t c(Color v)
{
    return lv_color_hex(v);
}

// First call initialises; later calls reset so the same lv_style_t objects can
// be refilled with a new palette without invalidating anyone's pointer.
void prepare(lv_style_t *s)
{
    if (sStylesInited)
        lv_style_reset(s);
    else
        lv_style_init(s);
}

} // namespace

// ---------------------------------------------------------------------------
// Fonts
//
// Three sizes is all a 222px-tall panel can justify: 12 for status bar and
// timestamps, 14 for everything that is read, 20 for the clock and headings.
//
// The sizes are guarded because src/pgros/ui/lv_conf.h is owned elsewhere; if a
// size is not compiled in we fall back rather than failing the build, and the
// UI degrades to one font instead of not existing.
// ---------------------------------------------------------------------------

const lv_font_t *Theme::fontSmall() const
{
#if LV_FONT_MONTSERRAT_12
    return &lv_font_montserrat_12;
#else
    return LV_FONT_DEFAULT;
#endif
}

const lv_font_t *Theme::fontBody() const
{
#if LV_FONT_MONTSERRAT_14
    return &lv_font_montserrat_14;
#else
    return LV_FONT_DEFAULT;
#endif
}

const lv_font_t *Theme::fontLarge() const
{
#if LV_FONT_MONTSERRAT_20
    return &lv_font_montserrat_20;
#else
    return LV_FONT_DEFAULT;
#endif
}

// ---------------------------------------------------------------------------
// Palette
// ---------------------------------------------------------------------------

void Theme::buildPalette()
{
    if (mDark) {
        mPalette.bg = 0x0E1117;         // near-black, blue cast, no banding
        mPalette.surface = 0x161B22;    // status bar, cards, list rows
        mPalette.surfaceAlt = 0x222B36; // selected row -- a real step, not +3
        mPalette.border = 0x2E3947;     // hairlines; visible but not a line of dots
        mPalette.text = 0xE8EDF2;       // soft white
        mPalette.textDim = 0x93A1B0;    // ~4.6:1 on bg; still comfortable
        mPalette.textFaint = 0x5E6C7A;  // placeholders only, never body text
        mPalette.accent = 0xFFB020;
        mPalette.accentText = 0x12151A;
        mPalette.bubbleOut = 0xE09A12;
        mPalette.bubbleIn = 0x1F2731;
        mPalette.ok = 0x35D07F;
        mPalette.warn = 0xFF7A29; // pushed red-ward to stay distinct from accent
        mPalette.error = 0xFF4D4F;
    } else {
        // Light is the daylight fallback, not the design target. Amber has to
        // darken considerably to hold contrast on white, so the accent here is
        // a bronze rather than the brand amber.
        mPalette.bg = 0xEFF2F6;
        mPalette.surface = 0xFFFFFF;
        mPalette.surfaceAlt = 0xE2E8F0;
        mPalette.border = 0xCBD4DF;
        mPalette.text = 0x161B22;
        mPalette.textDim = 0x55616E;
        mPalette.textFaint = 0x8B98A5;
        mPalette.accent = 0xB56A00;
        mPalette.accentText = 0xFFFFFF;
        mPalette.bubbleOut = 0xFFC24D;
        mPalette.bubbleIn = 0xFFFFFF;
        mPalette.ok = 0x0F7A46;
        mPalette.warn = 0xB2500E;
        mPalette.error = 0xC0272D;
    }
}

// ---------------------------------------------------------------------------
// Styles
// ---------------------------------------------------------------------------

void Theme::begin()
{
    buildPalette();

    const Palette &p = mPalette;

    // Screen / plain container: flat, no scrollbar chrome, no padding. Apps
    // position their own content; a container that quietly adds 8px of padding
    // is how a 222px screen loses a row.
    prepare(&sScreen);
    lv_style_set_bg_color(&sScreen, c(p.bg));
    lv_style_set_bg_opa(&sScreen, LV_OPA_COVER);
    lv_style_set_border_width(&sScreen, 0);
    lv_style_set_radius(&sScreen, 0);
    lv_style_set_pad_all(&sScreen, 0);
    lv_style_set_pad_row(&sScreen, 0);
    lv_style_set_pad_column(&sScreen, 0);
    lv_style_set_text_color(&sScreen, c(p.text));
    lv_style_set_text_font(&sScreen, fontBody());

    prepare(&sCard);
    lv_style_set_bg_color(&sCard, c(p.surface));
    lv_style_set_bg_opa(&sCard, LV_OPA_COVER);
    lv_style_set_border_color(&sCard, c(p.border));
    lv_style_set_border_width(&sCard, 1);
    lv_style_set_radius(&sCard, metrics::radiusM);
    lv_style_set_pad_all(&sCard, metrics::padM);
    lv_style_set_text_color(&sCard, c(p.text));
    lv_style_set_text_font(&sCard, fontBody());

    // List row: square-ish, edge to edge, separated by a bottom hairline rather
    // than by gaps. Gaps between rows would cost 4-5px each and we only have
    // five rows to spend.
    prepare(&sRow);
    lv_style_set_bg_color(&sRow, c(p.bg));
    lv_style_set_bg_opa(&sRow, LV_OPA_COVER);
    lv_style_set_border_color(&sRow, c(p.border));
    lv_style_set_border_width(&sRow, 1);
    lv_style_set_border_side(&sRow, LV_BORDER_SIDE_BOTTOM);
    lv_style_set_radius(&sRow, 0);
    lv_style_set_pad_hor(&sRow, metrics::padM);
    lv_style_set_pad_ver(&sRow, metrics::padS);
    lv_style_set_pad_column(&sRow, metrics::padM);
    lv_style_set_text_color(&sRow, c(p.text));
    lv_style_set_text_font(&sRow, fontBody());

    // Selection has to be unmistakable without touch: there is no hover, no
    // press animation and no cursor. A filled surface plus a 2px accent bar on
    // the leading edge reads instantly even in peripheral vision.
    prepare(&sRowSel);
    lv_style_set_bg_color(&sRowSel, c(p.surfaceAlt));
    lv_style_set_bg_opa(&sRowSel, LV_OPA_COVER);
    lv_style_set_border_color(&sRowSel, c(p.accent));
    lv_style_set_border_width(&sRowSel, 2);
    lv_style_set_border_side(&sRowSel, LV_BORDER_SIDE_LEFT);

    prepare(&sBubbleIn);
    lv_style_set_bg_color(&sBubbleIn, c(p.bubbleIn));
    lv_style_set_bg_opa(&sBubbleIn, LV_OPA_COVER);
    lv_style_set_text_color(&sBubbleIn, c(p.text));
    lv_style_set_text_font(&sBubbleIn, fontBody());
    lv_style_set_radius(&sBubbleIn, metrics::radiusBubble);
    lv_style_set_border_width(&sBubbleIn, 0);
    lv_style_set_pad_hor(&sBubbleIn, metrics::padM);
    lv_style_set_pad_ver(&sBubbleIn, metrics::padS + 1);
    lv_style_set_max_width(&sBubbleIn, LV_PCT(metrics::bubbleMaxPct));

    prepare(&sBubbleOut);
    lv_style_set_bg_color(&sBubbleOut, c(p.bubbleOut));
    lv_style_set_bg_opa(&sBubbleOut, LV_OPA_COVER);
    lv_style_set_text_color(&sBubbleOut, c(p.accentText)); // dark on amber
    lv_style_set_text_font(&sBubbleOut, fontBody());
    lv_style_set_radius(&sBubbleOut, metrics::radiusBubble);
    lv_style_set_border_width(&sBubbleOut, 0);
    lv_style_set_pad_hor(&sBubbleOut, metrics::padM);
    lv_style_set_pad_ver(&sBubbleOut, metrics::padS + 1);
    lv_style_set_max_width(&sBubbleOut, LV_PCT(metrics::bubbleMaxPct));

    prepare(&sBtn);
    lv_style_set_bg_color(&sBtn, c(p.surfaceAlt));
    lv_style_set_bg_opa(&sBtn, LV_OPA_COVER);
    lv_style_set_text_color(&sBtn, c(p.text));
    lv_style_set_text_font(&sBtn, fontBody());
    lv_style_set_border_color(&sBtn, c(p.border));
    lv_style_set_border_width(&sBtn, 1);
    lv_style_set_radius(&sBtn, metrics::radiusS);
    lv_style_set_pad_hor(&sBtn, metrics::padL);
    lv_style_set_pad_ver(&sBtn, metrics::padS);

    prepare(&sBtnPrimary);
    lv_style_set_bg_color(&sBtnPrimary, c(p.accent));
    lv_style_set_bg_opa(&sBtnPrimary, LV_OPA_COVER);
    lv_style_set_text_color(&sBtnPrimary, c(p.accentText));
    lv_style_set_text_font(&sBtnPrimary, fontBody());
    lv_style_set_border_width(&sBtnPrimary, 0);
    lv_style_set_radius(&sBtnPrimary, metrics::radiusS);
    lv_style_set_pad_hor(&sBtnPrimary, metrics::padL);
    lv_style_set_pad_ver(&sBtnPrimary, metrics::padS);

    prepare(&sInput);
    lv_style_set_bg_color(&sInput, c(p.surface));
    lv_style_set_bg_opa(&sInput, LV_OPA_COVER);
    lv_style_set_text_color(&sInput, c(p.text));
    lv_style_set_text_font(&sInput, fontBody());
    lv_style_set_border_color(&sInput, c(p.border));
    lv_style_set_border_width(&sInput, 1);
    lv_style_set_radius(&sInput, metrics::radiusS);
    lv_style_set_pad_hor(&sInput, metrics::padM);
    lv_style_set_pad_ver(&sInput, metrics::padS);

    prepare(&sLabelDim);
    lv_style_set_text_color(&sLabelDim, c(p.textDim));
    lv_style_set_text_font(&sLabelDim, fontSmall());

    sStylesInited = true;

    // Recolour LVGL's own default theme so any widget we do not style by hand
    // (scrollbars, the spinner arc, keyboard) still lands in the palette.
#if LV_USE_THEME_DEFAULT
    lv_display_t *disp = lv_display_get_default();
    if (disp) {
        lv_theme_t *t = lv_theme_default_init(disp, c(mPalette.accent), c(mPalette.textDim), mDark, fontBody());
        lv_display_set_theme(disp, t);
    }
#endif

    // Existing widgets hold pointers to the styles we just refilled; this is
    // what makes the refill visible.
    lv_obj_report_style_change(nullptr);

    LOG_INFO("PgrOS theme: %s, accent #%06X", mDark ? "dark" : "light", (unsigned)mPalette.accent);
}

void Theme::setDark(bool dark)
{
    if (dark == mDark && sStylesInited)
        return;
    mDark = dark;
    begin(); // refills the same style objects; no widget is rebuilt
    lv_obj_t *scr = lv_screen_active();
    if (scr)
        lv_obj_invalidate(scr);
}

// ---------------------------------------------------------------------------
// Style application helpers
// ---------------------------------------------------------------------------

void Theme::styleScreen(lv_obj_t *obj)
{
    if (!obj)
        return;
    lv_obj_add_style(obj, &sScreen, LV_PART_MAIN);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
}

void Theme::styleCard(lv_obj_t *obj)
{
    if (!obj)
        return;
    lv_obj_add_style(obj, &sCard, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
}

void Theme::styleListRow(lv_obj_t *obj)
{
    if (!obj)
        return;
    lv_obj_add_style(obj, &sRow, LV_PART_MAIN);
    // Both states, because navigation is by encoder (focus) in some screens and
    // by an explicit selection index (checked) in others.
    lv_obj_add_style(obj, &sRowSel, LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_add_style(obj, &sRowSel, LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_set_height(obj, metrics::listRowH);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
}

void Theme::styleBubble(lv_obj_t *obj, bool outbound)
{
    if (!obj)
        return;
    lv_obj_add_style(obj, outbound ? &sBubbleOut : &sBubbleIn, LV_PART_MAIN);
    lv_obj_set_width(obj, LV_SIZE_CONTENT);
    lv_obj_set_height(obj, LV_SIZE_CONTENT);
}

void Theme::styleButton(lv_obj_t *obj, bool primary)
{
    if (!obj)
        return;
    lv_obj_add_style(obj, primary ? &sBtnPrimary : &sBtn, LV_PART_MAIN);
    // Focus ring, since there is no pointer to hover with.
    lv_obj_set_style_outline_color(obj, c(mPalette.accent), LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(obj, 2, LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_outline_pad(obj, 1, LV_PART_MAIN | LV_STATE_FOCUSED);
}

void Theme::styleTextInput(lv_obj_t *obj)
{
    if (!obj)
        return;
    lv_obj_add_style(obj, &sInput, LV_PART_MAIN);
    lv_obj_set_style_text_color(obj, c(mPalette.textFaint), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_border_color(obj, c(mPalette.accent), LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(obj, c(mPalette.accent), LV_PART_CURSOR);
}

void Theme::styleLabelDim(lv_obj_t *obj)
{
    if (!obj)
        return;
    lv_obj_add_style(obj, &sLabelDim, LV_PART_MAIN);
}

// ---------------------------------------------------------------------------
// Semantic colour mappings. Kept here so no app hard-codes a delivery colour.
// ---------------------------------------------------------------------------

Color Theme::statusColor(uint8_t msgStatus) const
{
    // Mirrors store::MsgStatus. Taken as a raw uint8_t because Theme.h must not
    // depend on the store layer.
    switch (msgStatus) {
    case 1: // Composing -- not handed to the mesh yet
        return mPalette.textFaint;
    case 2: // Queued -- accepted by the router, waiting for airtime
        return mPalette.textDim;
    case 3: // Sent -- on the air, no ack yet
        return mPalette.textDim;
    case 4: // Delivered -- acknowledged
        return mPalette.ok;
    case 5: // Failed
        return mPalette.error;
    case 7: // Read -- the recipient's device confirmed it was opened
        return mPalette.accent;
    case 6: // Received
        return mPalette.textDim;
    default:
        return mPalette.textFaint;
    }
}

Color Theme::signalColor(uint8_t bars) const
{
    // Four bars, three bands. Two bars is genuinely usable on LoRa, so it is
    // not painted as a warning; one bar is.
    if (bars == 0)
        return mPalette.error;
    if (bars == 1)
        return mPalette.warn;
    if (bars == 2)
        return mPalette.text;
    return mPalette.ok;
}

} // namespace pgros

#endif // PGROS

// launcher_theme.h — shared design tokens for the launcher.
//
// One source of truth for color / spacing / radius / type, expressed in LOGICAL
// units. Each backend multiplies the pixel-affecting values by the platform
// display_scale so the look is identical at 100%, 125%, 150%, 175% and across
// backends. Keeping tokens here (not baked into a backend) is what lets a
// future toolkit swap reuse the same visual language.

#ifndef LAUNCHER_NG_THEME_H
#define LAUNCHER_NG_THEME_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { float r, g, b, a; } LngColor;

typedef struct {
    LngColor background;      // ink — the CRT ground
    LngColor background2;     // slightly lifted ground for the vignette center
    LngColor panel;
    LngColor panel_hovered;
    LngColor control;         // button/input fill — MUST contrast with panel
    LngColor control_hovered;
    LngColor border;          // panel + control outline
    LngColor accent;          // the one bold place: brand + primary CTA
    LngColor accent_dim;      // gradient partner / pressed
    LngColor accent_text;
    LngColor text;
    LngColor text_muted;
    LngColor good;        // verified / connected (phosphor mint)
    LngColor warn;        // unverified / caution (amber)
    LngColor focus_ring;  // gamepad/keyboard focus outline (cyan, sparingly)
    LngColor scanline;    // CRT scanline overlay (very low alpha)

    // logical dimensions (unscaled)
    float spacing_xs, spacing_sm, spacing_md, spacing_lg;
    float radius_sm, radius_lg;
    float row_height;
    float font_body, font_title, font_small;
    float focus_ring_width;
    int   scanlines;      // 1 = draw the CRT scanline overlay, 0 = flat (e.g. PSX)
} LauncherTheme;

static inline LngColor lng_rgba(float r, float g, float b, float a) {
    LngColor c; c.r = r; c.g = g; c.b = b; c.a = a; return c;
}

// "CRT Console Boot Screen" theme. A cinematic dark retro-console look: a
// violet-biased near-black ground (chosen, not a default grey), ONE bold neon
// accent (electric violet) reserved for brand + primary action, phosphor-mint
// and amber for state only, cyan for focus. Boldness spent in one place; the
// rest kept quiet.
static inline LauncherTheme launcher_theme_default(void) {
    LauncherTheme t;
    t.background      = lng_rgba(0.039f, 0.051f, 0.086f, 1.0f); // #0A0D16 ink
    t.background2     = lng_rgba(0.071f, 0.090f, 0.145f, 1.0f); // #121725 vignette center
    t.panel           = lng_rgba(0.078f, 0.102f, 0.157f, 1.0f); // #141A28 card
    t.panel_hovered   = lng_rgba(0.125f, 0.165f, 0.243f, 1.0f); // #202A3E
    t.control         = lng_rgba(0.106f, 0.137f, 0.208f, 1.0f); // #1B2335 button
    t.control_hovered = lng_rgba(0.145f, 0.188f, 0.278f, 1.0f); // #253047
    t.border          = lng_rgba(0.169f, 0.208f, 0.314f, 1.0f); // #2B3550 hairline
    t.accent          = lng_rgba(0.604f, 0.361f, 1.000f, 1.0f); // #9A5CFF electric violet
    t.accent_dim      = lng_rgba(0.431f, 0.247f, 0.812f, 1.0f); // #6E3FCF gradient/pressed
    t.accent_text     = lng_rgba(1.0f, 1.0f, 1.0f, 1.0f);
    t.text            = lng_rgba(0.925f, 0.933f, 0.965f, 1.0f); // #ECEEF6
    t.text_muted      = lng_rgba(0.529f, 0.565f, 0.659f, 1.0f); // #8790A8
    t.good            = lng_rgba(0.275f, 0.890f, 0.608f, 1.0f); // #46E39B phosphor mint
    t.warn            = lng_rgba(0.961f, 0.698f, 0.235f, 1.0f); // #F5B23C amber
    t.focus_ring      = lng_rgba(0.220f, 0.882f, 0.902f, 1.0f); // #38E1E6 cyan
    t.scanline        = lng_rgba(0.0f, 0.0f, 0.0f, 0.18f);      // CRT scanline

    t.spacing_xs = 4.0f;  t.spacing_sm = 8.0f;
    t.spacing_md = 16.0f; t.spacing_lg = 24.0f;
    t.radius_sm  = 6.0f;  t.radius_lg  = 14.0f;
    t.row_height = 44.0f;                      // large rows: Steam Deck friendly
    t.font_body  = 18.0f; t.font_title = 34.0f; t.font_small = 13.0f;
    t.focus_ring_width = 2.5f;
    t.scanlines  = 1;                          // CRT look: scanlines ON
    return t;
}

// "PlayStation" theme. A cooler, flatter take for PSX titles: deep blue-black
// ground, ONE clean PlayStation-blue accent for brand + primary action, and NO
// CRT scanlines (the disc era, not the cartridge/CRT-arcade era). Same layout
// and design language as the default; only the palette + scanline flag differ.
static inline LauncherTheme launcher_theme_psx(void) {
    LauncherTheme t = launcher_theme_default();   // inherit spacing/type/dims
    t.background      = lng_rgba(0.039f, 0.047f, 0.078f, 1.0f); // #0A0C14 blue-black
    t.background2     = lng_rgba(0.063f, 0.078f, 0.122f, 1.0f); // #10141F lifted center
    t.panel           = lng_rgba(0.071f, 0.094f, 0.149f, 1.0f); // #121826 card
    t.panel_hovered   = lng_rgba(0.110f, 0.153f, 0.251f, 1.0f); // #1C2740
    t.control         = lng_rgba(0.086f, 0.114f, 0.180f, 1.0f); // #161D2E button
    t.control_hovered = lng_rgba(0.129f, 0.176f, 0.271f, 1.0f); // #212D45
    t.border          = lng_rgba(0.157f, 0.196f, 0.282f, 1.0f); // #283248 hairline
    t.accent          = lng_rgba(0.180f, 0.490f, 1.000f, 1.0f); // #2E7DFF PlayStation blue
    t.accent_dim      = lng_rgba(0.102f, 0.353f, 0.839f, 1.0f); // #1A5AD6 pressed/gradient
    t.accent_text     = lng_rgba(1.0f, 1.0f, 1.0f, 1.0f);
    t.text            = lng_rgba(0.910f, 0.925f, 0.961f, 1.0f); // #E8ECF5
    t.text_muted      = lng_rgba(0.494f, 0.541f, 0.639f, 1.0f); // #7E8AA3
    /* good/warn keep their semantic colors; focus stays cyan (reads clearly on blue). */
    t.scanlines       = 0;                       // flat, no CRT scanlines
    return t;
}

// "Game Boy Advance" theme. The handheld LCD era: a deep indigo ground (the
// AGB-001 shell), ONE saturated indigo-violet accent for brand + primary
// action, and NO CRT scanlines (a backlit LCD never had them). Same layout and
// design language as the default; only the palette + scanline flag differ.
static inline LauncherTheme launcher_theme_gba(void) {
    LauncherTheme t = launcher_theme_default();   // inherit spacing/type/dims
    t.background      = lng_rgba(0.047f, 0.043f, 0.090f, 1.0f); // #0C0B17 indigo-black
    t.background2     = lng_rgba(0.082f, 0.075f, 0.145f, 1.0f); // #151325 lifted center
    t.panel           = lng_rgba(0.094f, 0.086f, 0.165f, 1.0f); // #18162A card
    t.panel_hovered   = lng_rgba(0.145f, 0.133f, 0.247f, 1.0f); // #25223F
    t.control         = lng_rgba(0.118f, 0.110f, 0.200f, 1.0f); // #1E1C33 button
    t.control_hovered = lng_rgba(0.161f, 0.149f, 0.271f, 1.0f); // #292645
    t.border          = lng_rgba(0.204f, 0.188f, 0.325f, 1.0f); // #343053 hairline
    t.accent          = lng_rgba(0.463f, 0.427f, 0.945f, 1.0f); // #766DF1 GBA indigo
    t.accent_dim      = lng_rgba(0.325f, 0.290f, 0.741f, 1.0f); // #534ABD pressed/gradient
    t.accent_text     = lng_rgba(1.0f, 1.0f, 1.0f, 1.0f);
    t.text            = lng_rgba(0.922f, 0.918f, 0.957f, 1.0f); // #EBEAF4
    t.text_muted      = lng_rgba(0.525f, 0.510f, 0.647f, 1.0f); // #8682A5
    /* good/warn keep their semantic colors; focus stays cyan (reads on indigo). */
    t.scanlines       = 0;                       // LCD, not CRT: flat
    return t;
}

// "Nintendo Entertainment System" theme. The cartridge/CRT era: a neutral
// graphite ground (the front-loader's grey, not a color-biased ink) with ONE
// bold Nintendo red accent for brand + primary action — the color of the
// console's stripe and the controller's A/B buttons. CRT scanlines ON (the
// living-room-TV era). Same layout and design language as the default; only the
// palette differs (neutral greys instead of the default's violet-blue bias).
static inline LauncherTheme launcher_theme_nes(void) {
    LauncherTheme t = launcher_theme_default();   // inherit spacing/type/dims
    t.background      = lng_rgba(0.051f, 0.055f, 0.063f, 1.0f); // #0D0E10 graphite ink
    t.background2     = lng_rgba(0.086f, 0.094f, 0.106f, 1.0f); // #16181B lifted center
    t.panel           = lng_rgba(0.098f, 0.106f, 0.118f, 1.0f); // #191B1E card
    t.panel_hovered   = lng_rgba(0.149f, 0.161f, 0.180f, 1.0f); // #26292E
    t.control         = lng_rgba(0.125f, 0.137f, 0.153f, 1.0f); // #202327 button
    t.control_hovered = lng_rgba(0.173f, 0.188f, 0.212f, 1.0f); // #2C3036
    t.border          = lng_rgba(0.200f, 0.216f, 0.239f, 1.0f); // #33373D hairline
    t.accent          = lng_rgba(0.898f, 0.196f, 0.153f, 1.0f); // #E53227 NES red
    t.accent_dim      = lng_rgba(0.659f, 0.118f, 0.090f, 1.0f); // #A81E17 pressed/gradient
    t.accent_text     = lng_rgba(1.0f, 1.0f, 1.0f, 1.0f);
    t.text            = lng_rgba(0.925f, 0.925f, 0.933f, 1.0f); // #ECECEE
    t.text_muted      = lng_rgba(0.541f, 0.557f, 0.588f, 1.0f); // #8A8E96
    /* good/warn keep their semantic colors; focus stays cyan (reads on graphite). */
    t.scanlines       = 1;                        // cartridge/CRT era: scanlines ON
    return t;
}

// "Sega Genesis" theme. The 16-bit cartridge/CRT-arcade era: a cool blue-black
// ground and ONE bold Sega azure accent for brand + primary action, distinctly
// more cyan than the PSX royal blue so the two never read alike. CRT scanlines
// stay ON (this is the tube/cartridge era, same rationale as the default).
static inline LauncherTheme launcher_theme_genesis(void) {
    LauncherTheme t = launcher_theme_default();   // inherit spacing/type/dims
    t.background      = lng_rgba(0.035f, 0.047f, 0.075f, 1.0f); // #090C13 blue-black ink
    t.background2     = lng_rgba(0.059f, 0.082f, 0.129f, 1.0f); // #0F1521 lifted center
    t.panel           = lng_rgba(0.067f, 0.094f, 0.149f, 1.0f); // #111826 card
    t.panel_hovered   = lng_rgba(0.106f, 0.153f, 0.235f, 1.0f); // #1B273C
    t.control         = lng_rgba(0.082f, 0.114f, 0.176f, 1.0f); // #151D2D button
    t.control_hovered = lng_rgba(0.122f, 0.173f, 0.263f, 1.0f); // #1F2C43
    t.border          = lng_rgba(0.149f, 0.204f, 0.298f, 1.0f); // #26344C hairline
    t.accent          = lng_rgba(0.090f, 0.635f, 0.902f, 1.0f); // #17A2E6 Sega azure
    t.accent_dim      = lng_rgba(0.055f, 0.451f, 0.702f, 1.0f); // #0E73B3 pressed/gradient
    t.accent_text     = lng_rgba(1.0f, 1.0f, 1.0f, 1.0f);
    t.text            = lng_rgba(0.914f, 0.929f, 0.961f, 1.0f); // #E9EDF5
    t.text_muted      = lng_rgba(0.498f, 0.545f, 0.643f, 1.0f); // #7F8BA4
    /* good/warn keep their semantic colors; focus stays cyan (reads on blue). */
    t.scanlines       = 1;                       // CRT/cartridge era: scanlines ON
    return t;
}

// Pick a built-in theme by name ("psx" -> PlayStation, "gba" -> Game Boy
// Advance, "nes" -> Nintendo Entertainment System, "genesis" -> Sega Genesis;
// anything else -> default CRT).
static inline LauncherTheme launcher_theme_by_name(const char* name) {
    if (name && (name[0] == 'p' || name[0] == 'P') &&
        (name[1] == 's' || name[1] == 'S'))
        return launcher_theme_psx();
    if (name && (name[0] == 'g' || name[0] == 'G') &&
        (name[1] == 'b' || name[1] == 'B') &&
        (name[2] == 'a' || name[2] == 'A'))
        return launcher_theme_gba();
    if (name && (name[0] == 'n' || name[0] == 'N') &&
        (name[1] == 'e' || name[1] == 'E') &&
        (name[2] == 's' || name[2] == 'S'))
        return launcher_theme_nes();
    if (name && (name[0] == 'g' || name[0] == 'G') &&
        (name[1] == 'e' || name[1] == 'E'))
        return launcher_theme_genesis();
    return launcher_theme_default();
}

#ifdef __cplusplus
}
#endif

#endif // LAUNCHER_NG_THEME_H

// launcher_imgui.cpp — Dear ImGui (MIT) backend for the next-gen launcher.
//
// Draws the shared LauncherModel with Dear ImGui + SDL3 + OpenGL3, at parity
// with the shipping RmlUi MMX launcher (box art, controller art, and all
// panels). Icons are drawn as vector primitives rather than font glyphs so they
// stay crisp at any DPI and don't depend on the text font's glyph coverage.
// Demonstrates the two hard requirements:
//   (1) DPI: fonts re-rasterize at (logical size * display_scale); style
//       re-scales on display-scale change -> crisp at 125/150/175% + monitors.
//   (2) Live resize: immediate mode redraws every frame; a logical-width
//       breakpoint switches the dashboard between two columns and one column.

#include "launcher_backend.h"
#include "launcher_gl.h"
#include "launcher_input.h"
#include "launcher_files.h"
#include "launcher_debug.h"
#include "launcher_binds.h"
#include "launcher_panels.h"
#include "launcher_system.h"
#include "consoles/n64/n64_binds.h"   // RUI_N64_FIELD_* for the pad-capture path

#include "launcher_sdlcompat.h"   // pulls the right SDL header + event shim

#include "imgui.h"
#if defined(LNG_SDL3)
  #include "imgui_impl_sdl3.h"
  #define LNG_ImplSDL_InitForOpenGL  ImGui_ImplSDL3_InitForOpenGL
  #define LNG_ImplSDL_NewFrame       ImGui_ImplSDL3_NewFrame
  #define LNG_ImplSDL_ProcessEvent   ImGui_ImplSDL3_ProcessEvent
  #define LNG_ImplSDL_Shutdown       ImGui_ImplSDL3_Shutdown
#else
  #include "imgui_impl_sdl2.h"
  #define LNG_ImplSDL_InitForOpenGL  ImGui_ImplSDL2_InitForOpenGL
  #define LNG_ImplSDL_NewFrame       ImGui_ImplSDL2_NewFrame
  #define LNG_ImplSDL_ProcessEvent   ImGui_ImplSDL2_ProcessEvent
  #define LNG_ImplSDL_Shutdown       ImGui_ImplSDL2_Shutdown
#endif
#include "imgui_impl_opengl3.h"

#include <cstring>
#include <string>

extern "C" const char* launcher_backend_name(void) { return "Dear ImGui"; }

namespace {

// ImGui coordinates are already DPI-independent: the SDL2 platform reports the
// window in points and the GL backend applies DisplayFramebufferScale when it
// submits vertices to the (Retina/HiDPI) drawable. Scaling widget geometry here
// too would DOUBLE every size on HiDPI (and make labels collide with their
// controls), so keep all layout tokens in logical units. Fonts are logical-
// sized as well; the renderer scales their atlas with the framebuffer.
// (Ported from launcher_ng's "Fix launcher DPI layout and text alignment".)
float  px(float logical) { return logical; }
ImVec4 col(const LngColor& c) { return ImVec4(c.r, c.g, c.b, c.a); }
const LauncherTheme* g_th = nullptr;

LauncherTexture g_boxart, g_pad, g_pad_analog, g_pad_digital, g_brand, g_memcard;
// Optional platform wordmark (SystemProfile.wordmark_image) — rendered in the
// header instead of the platform text when the asset is present. Absent => text.
LauncherTexture g_wordmark;
// N64 Transfer Pak cartridge art, indexed by host cart_kind: [0] empty/unknown
// (gray GB shell), [1] red, [2] blue, [3] yellow, [4] green. Loaded only for a
// tpak game; real GB cart PNGs from the RmlUi launchers (assets/consoles/n64).
LauncherTexture g_cart[5];
// Disc-verdict icons (verify.mode==1 systems, e.g. PSX) — keyed by
// VerifyResult.verdict (0 none,1 ok,2 warn,3 bad); see draw_verdict_block().
LauncherTexture g_verdict_ok, g_verdict_warn, g_verdict_bad, g_verdict_none;
ImTextureID tid(const LauncherTexture& t) { return (ImTextureID)(intptr_t)t.id; }

LauncherPad g_pads[LNG_MAX_PADS];   // live gamepad list (repolled every frame)
int         g_pad_count = 0;

char        g_pick_buf[512] = {};    // ROM picker result

// Context flag the dashboard composer sets just before invoking the "game"
// panel's registered draw() — the LauncherPanelDrawFn signature (Model*,
// const Theme*) has no room for the layout-context fill_h flag that
// draw_game_panel needs (fill the column height in the wide 2-column
// dashboard vs hug its content in the narrow stacked layout). Same pattern as
// the other per-frame context globals below (g_th, g_pads).
bool g_game_fill_h = false;

// ---- panel registry lookup helper ------------------------------------------
// Resolve `id` against a SystemProfile's NULL-terminated composition array:
// the panel must both be LISTED (this system composes it at all) and
// AVAILABLE (this game instance offers it) to be drawn. Returns nullptr
// otherwise — the caller simply skips that slot.
const LauncherPanel* find_composed(const char* const* ids, const char* id, LauncherModel* m) {
    if (!ids || !id) return nullptr;
    for (int i = 0; ids[i]; ++i) {
        if (strcmp(ids[i], id) != 0) continue;
        const LauncherPanel* p = launcher_panel_find(id);
        return (p && launcher_panel_available(p, m)) ? p : nullptr;
    }
    return nullptr;
}

// ---- DPI: rebuild fonts + re-derive style from an unscaled baseline ----------
void apply_scale(const LauncherTheme& th, float scale, const char* font_path,
                 const char* jp_font_path) {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    ImFontConfig cfg; cfg.OversampleH = 2; cfg.OversampleV = 2;
    (void)scale;   // DPI is handled by the framebuffer scale, not by re-scaling layout/fonts
    const float body = th.font_body;
    // Cover Basic Latin + Latin-1 AND General Punctuation so em/en dashes and
    // curly quotes used in the game notes render as glyphs, not "?" tofu.
    static const ImWchar kRanges[] = {
        0x0020, 0x00FF,   // Basic Latin + Latin-1 Supplement
        0x2010, 0x2027,   // dashes, curly quotes, ellipsis (General Punctuation)
        0,
    };
    bool loaded = false;
    if (font_path && font_path[0])
        loaded = io.Fonts->AddFontFromFileTTF(font_path, body, &cfg, kRanges) != nullptr;
    if (!loaded) { cfg.SizePixels = body; io.Fonts->AddFontDefault(&cfg); }
    // Merge a Japanese subset atlas over the Latin base when the game ships one
    // (PMS-J's kana cart names / trainer strings). MergeMode folds the JP glyphs
    // into the same font so mixed Latin+kana strings render in one pass; absent
    // file => Latin-only, unchanged for every other console.
    if (jp_font_path && jp_font_path[0]) {
        if (FILE* jf = fopen(jp_font_path, "rb")) {
            fclose(jf);
            ImFontConfig jcfg; jcfg.OversampleH = 2; jcfg.OversampleV = 2;
            jcfg.MergeMode = true;
            io.Fonts->AddFontFromFileTTF(jp_font_path, body, &jcfg,
                                         io.Fonts->GetGlyphRangesJapanese());
        }
    }
    io.Fonts->Build();
    ImGui_ImplOpenGL3_DestroyFontsTexture();
    ImGui_ImplOpenGL3_CreateFontsTexture();

    ImGuiStyle style; ImGui::StyleColorsDark(&style);
    style.WindowRounding = th.radius_lg; style.ChildRounding = th.radius_lg;
    style.FrameRounding  = th.radius_sm; style.GrabRounding  = th.radius_sm;
    style.WindowPadding  = ImVec2(th.spacing_lg, th.spacing_lg);
    style.FramePadding   = ImVec2(th.spacing_md, th.spacing_sm);
    style.ItemSpacing    = ImVec2(th.spacing_md, th.spacing_sm);
    style.ChildBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;   // controls get a visible outline
    style.Colors[ImGuiCol_WindowBg]        = col(th.background);
    style.Colors[ImGuiCol_ChildBg]         = col(th.panel);
    style.Colors[ImGuiCol_PopupBg]         = col(th.panel);
    style.Colors[ImGuiCol_Border]          = col(th.border);
    style.Colors[ImGuiCol_FrameBg]         = col(th.control);
    style.Colors[ImGuiCol_FrameBgHovered]  = col(th.control_hovered);
    style.Colors[ImGuiCol_FrameBgActive]   = col(th.control_hovered);
    style.Colors[ImGuiCol_Button]          = col(th.control);
    style.Colors[ImGuiCol_ButtonHovered]   = col(th.control_hovered);
    style.Colors[ImGuiCol_ButtonActive]    = col(th.accent);
    style.Colors[ImGuiCol_Header]          = col(th.control_hovered);
    style.Colors[ImGuiCol_HeaderHovered]   = col(th.control_hovered);
    style.Colors[ImGuiCol_HeaderActive]    = col(th.accent);
    style.Colors[ImGuiCol_CheckMark]       = col(th.accent);
    style.Colors[ImGuiCol_Text]            = col(th.text);
    style.Colors[ImGuiCol_TextDisabled]    = col(th.text_muted);
    style.Colors[ImGuiCol_Separator]       = col(th.border);
    style.Colors[ImGuiCol_ScrollbarBg]     = col(th.panel);
    style.Colors[ImGuiCol_ScrollbarGrab]   = col(th.border);
    style.Colors[ImGuiCol_ScrollbarGrabHovered] = col(th.control_hovered);
    // Gamepad/keyboard focus ring: bright cyan so a Deck user always sees where
    // they are. (NavHighlight is the pre-1.91.4 alias of NavCursor.)
    style.Colors[ImGuiCol_NavCursor]       = col(th.focus_ring);
    ImGui::GetStyle() = style;
}

// ---- CRT / neon atmosphere (drawn with ImDrawList) ---------------------------
ImU32 imcol(const LngColor& c, float a = 1.0f) {
    return ImGui::GetColorU32(ImVec4(c.r, c.g, c.b, c.a * a));
}

// Vertical center-bright gradient (CRT ground) + faint scanlines. Drawn on the
// background/foreground draw lists so it sits behind/over the whole UI.
void draw_crt_background(ImVec2 origin, ImVec2 size) {
    const LauncherTheme& th = *g_th;
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    ImU32 ink = imcol(th.background), lift = imcol(th.background2);
    float midY = origin.y + size.y * 0.42f;
    // top: ink -> lift, bottom: lift -> ink  (soft horizontal glow band)
    bg->AddRectFilledMultiColor(origin, ImVec2(origin.x + size.x, midY),
                                ink, ink, lift, lift);
    bg->AddRectFilledMultiColor(ImVec2(origin.x, midY), ImVec2(origin.x + size.x, origin.y + size.y),
                                lift, lift, ink, ink);
    // a soft violet bloom behind the header (arcade marquee glow)
    bg->AddRectFilledMultiColor(origin, ImVec2(origin.x + size.x, origin.y + px(90)),
                                imcol(th.accent, 0.10f), imcol(th.accent, 0.10f),
                                imcol(th.accent, 0.0f),  imcol(th.accent, 0.0f));
    // scanlines over everything, very subtle — only for CRT-style themes (the PSX
    // theme sets scanlines = 0 for a flat, disc-era look).
    if (th.scanlines) {
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        float step = px(3.0f); if (step < 2.0f) step = 2.0f;
        ImU32 sl = imcol(th.scanline);
        for (float y = origin.y; y < origin.y + size.y; y += step)
            fg->AddLine(ImVec2(origin.x, y), ImVec2(origin.x + size.x, y), sl, 1.0f);
    }
}

// Neon glow: concentric rounded rects fading outward behind [min,max].
void glow_rect(ImDrawList* dl, ImVec2 mn, ImVec2 mx, float rounding,
               const LngColor& c, float intensity, int layers = 5) {
    for (int i = layers; i >= 1; --i) {
        float grow = px(2.0f) * i;
        float a = intensity * (0.10f) * (float)(layers - i + 1) / layers;
        dl->AddRectFilled(ImVec2(mn.x - grow, mn.y - grow),
                          ImVec2(mx.x + grow, mx.y + grow),
                          imcol(c, a), rounding + grow);
    }
}

// Filled rounded rect with a vertical gradient (top -> bottom).
void grad_rect(ImDrawList* dl, ImVec2 mn, ImVec2 mx, float rounding,
               const LngColor& top, const LngColor& bot) {
    dl->AddRectFilled(mn, mx, imcol(bot), rounding);   // base (rounded)
    // overlay a gradient clipped to the rounded rect via a slightly-inset fill
    dl->PushClipRect(mn, mx, true);
    dl->AddRectFilledMultiColor(mn, mx, imcol(top), imcol(top), imcol(bot), imcol(bot));
    dl->PopClipRect();
}

// ---- primitive icons (crisp at any DPI, no font dependency) -------------------
void draw_check(const LngColor& c) {   // green check, advances cursor like text
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float s = ImGui::GetTextLineHeight(), y = p.y + s * 0.5f;
    ImU32 u = ImGui::GetColorU32(col(c));
    dl->AddLine(ImVec2(p.x + s*0.15f, y), ImVec2(p.x + s*0.40f, y + s*0.28f), u, px(2.0f));
    dl->AddLine(ImVec2(p.x + s*0.40f, y + s*0.28f), ImVec2(p.x + s*0.85f, y - s*0.28f), u, px(2.0f));
    ImGui::Dummy(ImVec2(s, s)); ImGui::SameLine(0, px(6));
}
void draw_dot(bool on, const LngColor& good, const LngColor& off) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float s = ImGui::GetTextLineHeight(), r = px(5.0f);
    ImVec2 c(p.x + r, p.y + s * 0.5f);
    if (on) dl->AddCircleFilled(c, r, ImGui::GetColorU32(col(good)));
    else    dl->AddCircle(c, r, ImGui::GetColorU32(col(off)), 0, px(1.5f));
    ImGui::Dummy(ImVec2(r * 2, s)); ImGui::SameLine(0, px(8));
}
// The primary neon CTA (PLAY): glow + violet gradient + play triangle. Fully
// custom-drawn over an InvisibleButton so it looks nothing like a stock button.
bool neon_cta(const char* id, const char* label, ImVec2 size) {
    const LauncherTheme& th = *g_th;
    ImVec2 p = ImGui::GetCursorScreenPos();
    // EnableNav is REQUIRED: ImGui::InvisibleButton() adds ImGuiItemFlags_NoNav by
    // default, which silently excludes the CTA from gamepad/keyboard nav — that was
    // why PLAY could never be focused at runtime (only via boot SetItemDefaultFocus)
    // while normal widgets (Skip, Settings) always could.
    bool clk = ImGui::InvisibleButton(id, size, ImGuiButtonFlags_EnableNav);
    bool hov = ImGui::IsItemHovered();
    bool act = ImGui::IsItemActive();
    bool foc = ImGui::IsItemFocused();   // gamepad/keyboard nav focus
    ImVec2 mn = p, mx = ImVec2(p.x + size.x, p.y + size.y);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float r = px(th.radius_sm);

    glow_rect(dl, mn, mx, r, th.accent, hov ? 1.6f : 1.0f, 6);
    LngColor top = hov ? th.accent : th.accent;
    LngColor bot = act ? th.accent_dim : th.accent_dim;
    grad_rect(dl, mn, mx, r, top, bot);
    dl->AddRect(mn, mx, imcol(th.accent, hov ? 0.9f : 0.5f), r, 0, px(1.0f));  // crisp edge
    // InvisibleButton draws no nav highlight itself — paint the cyan focus ring
    // when nav-focused so the CTA reads as selectable via controller/keyboard.
    if (foc) {
        ImVec2 om = ImVec2(mn.x - px(2), mn.y - px(2)), ox = ImVec2(mx.x + px(2), mx.y + px(2));
        dl->AddRect(om, ox, imcol(th.focus_ring), r + px(2), 0, px(th.focus_ring_width));
    }

    // centered "▶ label"
    float th_h = ImGui::GetTextLineHeight();
    float tw = ImGui::CalcTextSize(label).x;
    float tri = px(11.0f), gap = px(10.0f);
    float total = tri + gap + tw;
    float cx = p.x + (size.x - total) * 0.5f, cy = p.y + size.y * 0.5f;
    ImU32 fg = imcol(th.accent_text);
    dl->AddTriangleFilled(ImVec2(cx, cy - tri*0.55f), ImVec2(cx, cy + tri*0.55f),
                          ImVec2(cx + tri, cy), fg);
    dl->AddText(ImVec2(cx + tri + gap, cy - th_h*0.5f), fg, label);
    return clk;
}

// Uppercase section eyebrow with letter-spacing + a short accent tick, e.g.
//   ▎ CONTROLLERS   — encodes "this is a section header", arcade panel style.
void eyebrow_tracked(const char* s) {
    const LauncherTheme& th = *g_th;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = ImGui::GetTextLineHeight();
    // accent tick
    dl->AddRectFilled(ImVec2(p.x, p.y + h*0.12f), ImVec2(p.x + px(3.0f), p.y + h*0.9f),
                      imcol(th.accent), px(1.5f));
    // letter-spaced text
    float x = p.x + px(10.0f);
    ImU32 c = imcol(th.accent);
    char buf[2] = {0,0};
    for (const char* q = s; *q; ++q) {
        buf[0] = *q;
        dl->AddText(ImVec2(x, p.y), c, buf);
        x += ImGui::CalcTextSize(buf).x + px(2.2f);
    }
    ImGui::Dummy(ImVec2(x - p.x, h));
    ImGui::Spacing();
}

// Draw a texture fit inside a logical box, preserving aspect.
void image_fit(const LauncherTexture& t, float box_w, float box_h) {
    if (!t.id || t.w <= 0 || t.h <= 0) { ImGui::Dummy(ImVec2(px(box_w), px(box_h))); return; }
    float bw = px(box_w), bh = px(box_h);
    float s = (bw / t.w < bh / t.h) ? bw / (float)t.w : bh / (float)t.h;
    ImGui::Image(tid(t), ImVec2(t.w * s, t.h * s));
}

// Like image_fit, but horizontally centers the FITTED image within avail_w.
// image_fit alone centers on the box width, so a near-square art (the N64 pad)
// fit into a landscape box draws narrow and sits left-of-center — this offsets
// by the real fitted width instead.
void image_fit_centered(const LauncherTexture& t, float box_w, float box_h, float avail_w) {
    float fitted_w = px(box_w);
    if (t.id && t.w > 0 && t.h > 0) {
        float bw = px(box_w), bh = px(box_h);
        float s = (bw / t.w < bh / t.h) ? bw / (float)t.w : bh / (float)t.h;
        fitted_w = t.w * s;
    }
    float off = (avail_w - fitted_w) * 0.5f;
    if (off > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + off);
    image_fit(t, box_w, box_h);
}

void eyebrow(const char* s) { eyebrow_tracked(s); }
// A card: filled + bordered. Hugs its content by default; `fill_h` stretches it
// to the remaining height (used by the dashboard columns so the layout doesn't
// leave a big empty gap under short cards).
bool begin_panel(const char* id, float logical_w = 0.0f, bool fill_h = false,
                 bool no_scroll = false) {
    ImGuiChildFlags flags = ImGuiChildFlags_Borders;
    if (!fill_h) flags |= ImGuiChildFlags_AutoResizeY;
    // A fill-height card (e.g. GAME) must SCROLL when the window is too short —
    // otherwise its folded-in content (SAVES) clips out of reach. Only the
    // fixed-size settings cards, which are sized to fit, suppress the scrollbar
    // (no_scroll) to avoid a stray bar. Content-hugging cards (AutoResizeY) never
    // overflow themselves, so scrollable-by-default is a no-op for them.
    ImGuiWindowFlags wflags = no_scroll ? (ImGuiWindowFlags_NoScrollbar |
                                           ImGuiWindowFlags_NoScrollWithMouse)
                                        : 0;
    return ImGui::BeginChild(id, ImVec2(px(logical_w), 0.0f), flags, wflags);
}
void end_panel() { ImGui::EndChild(); }

// A layout container: no fill, no border. Without this a nested child inherits
// ChildBg and paints a large panel-coloured rectangle behind the real cards,
// which reads as "dead space".
bool begin_container(const char* id, ImVec2 size, ImGuiChildFlags flags = ImGuiChildFlags_None,
                     ImGuiWindowFlags wflags = 0) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    return ImGui::BeginChild(id, size, flags, wflags);
}
void end_container() { ImGui::EndChild(); ImGui::PopStyleColor(); }

void state_mark(bool ok, const LauncherTheme& th);   // fwd
void draw_save_row(LauncherModel* m, const LauncherTheme& th);   // fwd (Save module row-drawer)

// One metadata row inside a 3-column table: label | value | optional check.
// `show_mark` puts a mint check / amber cross in its own column instead of a
// text badge, so it can never crowd the panel edge.
void kv_row(const char* k, const char* v, const LauncherTheme& th,
            bool show_mark, bool ok) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::PushStyleColor(ImGuiCol_Text, col(th.text_muted));
    ImGui::TextUnformatted(k);
    ImGui::PopStyleColor();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(v);
    ImGui::TableNextColumn();
    if (show_mark) state_mark(ok, th);
}

// Key/value row, drawn full width: muted label column, value, and an optional
// right-aligned badge. No wrapping — the row owns the whole panel width, so
// long values (CRC/SHA) have room instead of being clipped or char-wrapped.
void kv(const char* k, const char* v, const LauncherTheme& th,
        const char* badge = nullptr, bool good = true) {
    const float x0 = ImGui::GetCursorPosX();
    ImGui::PushStyleColor(ImGuiCol_Text, col(th.text_muted));
    ImGui::TextUnformatted(k); ImGui::PopStyleColor();
    ImGui::SameLine(x0 + px(84.0f));
    ImGui::TextUnformatted(v);
    if (badge) {
        char b[24]; snprintf(b, sizeof(b), "[%s]", badge);
        const float bw = ImGui::CalcTextSize(b).x;
        ImGui::SameLine(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - bw);
        ImGui::PushStyleColor(ImGuiCol_Text, col(good ? th.good : th.warn));
        ImGui::TextUnformatted(b); ImGui::PopStyleColor();
    }
}
void stepper(const char* id, int value, const char* suffix, int* out_delta) {
    ImGui::PushID(id);
    const float bh = px(30), fw = px(58);
    if (ImGui::Button("-", ImVec2(px(32), bh))) *out_delta = -5;
    ImGui::SameLine(0, px(6));
    // value centered in a fixed-width field so "+" never shifts with the digits
    char buf[32]; snprintf(buf, sizeof(buf), "%d%s", value, suffix);
    float cx = ImGui::GetCursorPosX();
    ImVec2 ts = ImGui::CalcTextSize(buf);
    ImGui::SetCursorPosX(cx + (fw - ts.x) * 0.5f);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(buf);
    ImGui::SameLine(0, 0);
    ImGui::SetCursorPosX(cx + fw + px(6));
    if (ImGui::Button("+", ImVec2(px(32), bh))) *out_delta = +5;
    ImGui::PopID();
}

// "Label ......... [control]" row: label baseline-aligned to the control.
// col_w > 0 reserves a FIXED label column so the control starts at the same x
// on every row — the caller passes the widest label's width (+gap) to line all
// the controls up into a clean grid. col_w == 0 keeps the legacy flow layout
// (control hugs the label with a fixed gap).
void row_label(const char* text, const LauncherTheme& th, float col_w = 0.0f) {
    float x0 = ImGui::GetCursorPosX();
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(col(th.text_muted), "%s", text);
    if (col_w > 0.0f) {
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::SetCursorPosX(x0 + col_w);          // fixed label column → controls align
    } else {
        ImGui::SameLine(0.0f, px(th.spacing_md));  // flow from label width (no fixed-x overlap)
    }
}

// ---- views -----------------------------------------------------------------
// Box art, centered, framed. No neon glow — the art is photographic content and
// a violet halo around it reads as a bug, not a design. Glow is reserved for
// the PLAY CTA, where it means "this is the action".
void hero_boxart_centered(const LauncherTexture& t, float box_h, float avail_w) {
    const LauncherTheme& th = *g_th;
    float bh = px(box_h);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (t.id && t.w > 0 && t.h > 0) {
        float s = bh / (float)t.h;
        float iw = t.w * s, ih = bh;
        if (iw > avail_w) { s = avail_w / (float)t.w; iw = avail_w; ih = t.h * s; }
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail_w - iw) * 0.5f);  // center
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImVec2 mn = p, mx = ImVec2(p.x + iw, p.y + ih);
        dl->AddImageRounded(tid(t), mn, mx, ImVec2(0,0), ImVec2(1,1),
                            imcol(lng_rgba(1,1,1,1)), px(4.0f));
        dl->AddRect(mn, mx, imcol(th.border), px(4.0f), 0, px(1.0f));
        ImGui::Dummy(ImVec2(iw, ih));
    } else {
        // No box art was supplied for this game — draw a tasteful SNES-cartridge
        // placeholder so the GAME card never shows dead space. Game-agnostic: any
        // title that declares no boxart.tga gets this instead of an empty slot.
        float iw = bh * 0.72f;               // match a box-art portrait aspect
        if (iw > avail_w) iw = avail_w;
        float ih = bh;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail_w - iw) * 0.5f);  // center
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImVec2 mn = p, mx = ImVec2(p.x + iw, p.y + ih);
        dl->AddRectFilled(mn, mx, imcol(th.panel_hovered), px(6.0f));
        dl->AddRect(mn, mx, imcol(th.border), px(6.0f), 0, px(1.0f));

        // cartridge body, centered in the slot
        float cw = iw * 0.52f, ch = cw * 1.04f;
        float cx = (mn.x + mx.x) * 0.5f, cy = (mn.y + mx.y) * 0.5f;
        ImVec2 bmn = ImVec2(cx - cw * 0.5f, cy - ch * 0.5f);
        ImVec2 bmx = ImVec2(cx + cw * 0.5f, cy + ch * 0.5f);
        dl->AddRectFilled(bmn, bmx, imcol(th.accent_dim), cw * 0.10f);
        // top ridges
        for (int i = 0; i < 3; i++) {
            float rx = bmn.x + cw * (0.20f + i * 0.24f);
            dl->AddRectFilled(ImVec2(rx, bmn.y - ch * 0.05f),
                              ImVec2(rx + cw * 0.12f, bmn.y + ch * 0.10f),
                              imcol(th.accent), cw * 0.03f);
        }
        // recessed label window
        dl->AddRectFilled(ImVec2(bmn.x + cw * 0.16f, bmn.y + ch * 0.30f),
                          ImVec2(bmx.x - cw * 0.16f, bmx.y - ch * 0.16f),
                          imcol(th.panel), cw * 0.04f);
        ImGui::Dummy(ImVec2(iw, ih));
    }
}

// A verified/failed state marker: mint check or amber cross. Replaces the
// [MATCH] badge that crowded the panel edge.
void state_mark(bool ok, const LauncherTheme& th) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float s = ImGui::GetTextLineHeight();
    ImU32 c = imcol(ok ? th.good : th.warn);
    float y = p.y + s * 0.5f;
    if (ok) {
        dl->AddLine(ImVec2(p.x + s*0.16f, y), ImVec2(p.x + s*0.40f, y + s*0.26f), c, px(2.0f));
        dl->AddLine(ImVec2(p.x + s*0.40f, y + s*0.26f), ImVec2(p.x + s*0.84f, y - s*0.26f), c, px(2.0f));
    } else {
        dl->AddLine(ImVec2(p.x + s*0.22f, y - s*0.24f), ImVec2(p.x + s*0.78f, y + s*0.24f), c, px(2.0f));
        dl->AddLine(ImVec2(p.x + s*0.78f, y - s*0.24f), ImVec2(p.x + s*0.22f, y + s*0.24f), c, px(2.0f));
    }
    ImGui::Dummy(ImVec2(s, s));
}

// Pick the verdict icon for VerifyResult.verdict (0 none,1 ok,2 warn,3 bad).
const LauncherTexture& verdict_texture(int verdict) {
    switch (verdict) {
        case 1:  return g_verdict_ok;
        case 2:  return g_verdict_warn;
        case 3:  return g_verdict_bad;
        default: return g_verdict_none;
    }
}

// Disc-verdict block (verify.mode==1 systems, e.g. PSX): a verdict icon +
// headline, followed by a Serial/Region/ISO-header checklist. Replaces the
// CRC/SHA "verified" line that mode==0 (cart/ROM-hash) systems draw instead
// (see draw_game_panel) — same slot in the card, different module. Reads
// m->verify, populated by launcher_model_set_rom()/run_verify() in
// launcher_model.c (real probe when the SystemProfile has one, a synthesized
// placeholder verdict otherwise).
void draw_verdict_block(const LauncherModel* m, const LauncherTheme& th, float availw) {
    const VerifyResult& v = m->verify;
    const char* headline =
        v.verdict == 1 ? "Disc verified" :
        v.verdict == 2 ? "Disc verified (warnings)" :
        v.verdict == 3 ? "Disc verification failed" :
                          "Disc not recognized";
    // th has no dedicated "bad"/error slot (only good/warn) — reuse warn for
    // the warn AND none cases (both are cautionary, matching the ROM-hash
    // line's existing amber-for-"not recognized" convention) and fall back to
    // a plain red only for the explicit "bad" verdict.
    LngColor headline_color = (v.verdict == 1) ? th.good
                              : (v.verdict == 3) ? lng_rgba(0.945f, 0.322f, 0.322f, 1.0f)
                              : th.warn;

    const LauncherTexture& icon = verdict_texture(v.verdict);
    float ih = ImGui::GetTextLineHeight() * 1.35f;
    float iw = (icon.id && icon.h > 0) ? ih * ((float)icon.w / (float)icon.h) : ih;
    float w = iw + px(6) + ImGui::CalcTextSize(headline).x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (availw - w) * 0.5f);
    if (icon.id) {
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddImage(tid(icon), p, ImVec2(p.x + iw, p.y + ih));
        ImGui::Dummy(ImVec2(iw, ih));
    } else {
        state_mark(v.verdict == 1, th);   // icon failed to load: vector fallback
    }
    ImGui::SameLine(0, px(6));
    ImGui::TextColored(col(headline_color), "%s", headline);
    ImGui::Dummy(ImVec2(0, px(8)));

    // Checklist: Serial / Region / ISO header, each with its own pass/fail
    // mark, derived straight from the minimal VerifyResult fields.
    if (ImGui::BeginTable("verdict_checklist", 3, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthFixed, px(76));
        ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("m", ImGuiTableColumnFlags_WidthFixed, px(28));
        kv_row("Serial",     v.serial[0] ? v.serial : "\xE2\x80\x94", th, true, v.serial[0] != '\0');
        kv_row("Region",     v.region[0] ? v.region : "\xE2\x80\x94", th, true, v.region[0] != '\0');
        kv_row("ISO header", v.iso_ok ? "OK" : "Mismatch",             th, true, v.iso_ok);
        ImGui::EndTable();
    }
}

void draw_game_panel(LauncherModel* m, const LauncherTheme& th, bool fill_h = false) {
    if (!begin_panel("game", 0, fill_h)) { end_panel(); return; }
    // No "GAME" eyebrow: the box art itself tells the user this is the game.
    const float availw = ImGui::GetContentRegionAvail().x;

    // Verify module: verify.mode==1 systems (PSX) render a disc-verdict block
    // (icon + Serial/Region/ISO checklist) here instead of the CRC/SHA line;
    // mode==0 systems (SNES/cart) keep the CRC/SHA line exactly as before.
    const bool disc_verdict = m->profile && m->profile->verify.mode == 1;

    // Box art on top (centered), everything else BELOW it. Height is derived
    // from the space actually left after the metadata + button, so the art is
    // as large as it can be WITHOUT pushing the last row out of the card.
    {
        // Reserve space for everything under the art: verified line + 2 meta rows
        // + Change ROM, plus the SAVES block when this game has battery SRAM.
        float reserve = px(198.0f);
        if (disc_verdict) reserve += px(96.0f);           // taller: icon+headline + 3-row checklist
        if (m->saves_supported) reserve += px(96.0f);    // compact SAVES row below Change ROM
        if (m->msu1_patch_available) reserve += px(198.0f);  // MSU-1 patch-available sub-block
                                                              // (title + up-to-3-line wrapped note + 2 stacked buttons)
        float art_h = ImGui::GetContentRegionAvail().y - reserve;
        if (art_h > px(368.0f)) art_h = px(368.0f);   // allow a larger hero box art (~15% bigger than before)
        if (art_h < px(248.0f)) art_h = px(248.0f);   // keep it big enough to balance the side column
        hero_boxart_centered(g_boxart, art_h, availw);
    }
    ImGui::Dummy(ImVec2(0, px(10)));

    // Region + verification state, centered under the art.
    const char* noun = (m->rom_noun && m->rom_noun[0]) ? m->rom_noun : "ROM";
    if (disc_verdict) {
        draw_verdict_block(m, th, availw);
    } else {
        const bool verified = launcher_model_rom_verified(m);
        char line[64];
        if (!m->rom_present)   snprintf(line, sizeof(line), "No %s loaded", noun);
        else if (verified)     snprintf(line, sizeof(line), "%s verified", noun);
        else                   snprintf(line, sizeof(line), "%s not recognized", noun);
        float w = ImGui::GetTextLineHeight() + px(6) + ImGui::CalcTextSize(line).x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (availw - w) * 0.5f);
        state_mark(verified, th);
        ImGui::SameLine(0, px(6));
        ImGui::TextColored(verified ? col(th.good) : col(th.warn), "%s", line);
    }
    ImGui::Dummy(ImVec2(0, px(10)));

    // Metadata a PLAYER cares about — just Region + File. The "is my ROM good?"
    // question is answered by the ROM-verified line above; raw size and CRC/SHA
    // digests are developer noise, so they're not shown.
    if (ImGui::BeginTable("meta", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthFixed, px(76));
        ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthStretch);
        // The disc-verdict block already reports Region in its checklist, so
        // don't repeat it here (it would show a redundant/blank second row).
        if (!disc_verdict)
            kv_row("Region", m->region[0] ? m->region : "", th, false, false);
        kv_row("File",   m->rom_file, th, false, false);
        ImGui::EndTable();
    }
    ImGui::Dummy(ImVec2(0, px(12)));
    char change_label[32];
    snprintf(change_label, sizeof(change_label), "Change %s", noun);
    if (ImGui::Button(change_label, ImVec2(availw, px(34)))) {
        // Native file dialog filter comes from the active console's
        // SystemProfile.rom_filter — never a hardcoded SNES set. Falls back
        // to launcher_pick_rom's built-in filter only if a profile omits it.
        const SystemProfile* prof = (const SystemProfile*)m->profile;
        char title[48];
        snprintf(title, sizeof(title), "Select %s", noun);
        bool picked;
        if (prof && prof->rom_filter.patterns && prof->rom_filter.pattern_count > 0)
            picked = launcher_pick_file(title, prof->rom_filter.patterns,
                                        prof->rom_filter.pattern_count,
                                        prof->rom_filter.desc,
                                        g_pick_buf, sizeof(g_pick_buf));
        else
            picked = launcher_pick_rom(g_pick_buf, sizeof(g_pick_buf));
        if (picked) launcher_model_set_rom(m, g_pick_buf);
    }

    // MSU-1 patch-available sub-block: this game ships an IPS patch that
    // converts the verified vanilla ROM into its MSU-1 streamed-audio variant.
    // Ported from the RmlUi launcher's dashboard "MSU-1 patch available" card
    // (snesrecomp/runner/src/launcher/launcher_gui.cpp: msu1_patch_available +
    // do_patch()/patch_rom/skip_patch). "warn" amber styling — this is a
    // choice the player should notice, not a routine control.
    if (m->msu1_patch_available) {
        ImGui::Dummy(ImVec2(0, px(10)));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, col(th.panel_hovered));
        ImGui::PushStyleColor(ImGuiCol_Border, col(th.warn));
        if (ImGui::BeginChild("msu1_patch_block", ImVec2(availw, 0),
                              ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
            ImGui::TextColored(col(th.warn), "MSU-1 patch available");
            const float inner_w = ImGui::GetContentRegionAvail().x;
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + inner_w);
            ImGui::TextColored(col(th.text_muted), "%s",
                (m->msu1_note && m->msu1_note[0])
                    ? m->msu1_note
                    : "An MSU-1 patch exists for this game. Patch a copy beside "
                      "your ROM (the original is never modified)?");
            ImGui::PopTextWrapPos();
            ImGui::Dummy(ImVec2(0, px(8)));
            // Stacked full-width buttons, not side-by-side: "Skip (Play Unpatched)"
            // is long enough that splitting the row in half clips its label at
            // common card widths (verified via the LNG_DEMO_MSU harness).
            if (ImGui::Button("Patch ROM", ImVec2(inner_w, px(32))))
                launcher_model_apply_msu1_patch(m);
            ImGui::Dummy(ImVec2(0, px(th.spacing_xs)));
            if (ImGui::Button("Skip (Play Unpatched)", ImVec2(inner_w, px(32))))
                launcher_model_skip_msu1_patch(m);
        }
        ImGui::EndChild();
        ImGui::PopStyleColor(2);
    }

    // SAVES lives in the GAME card as a compact row (no separate card / eyebrow).
    // Present only for games with battery SRAM — data-driven, never by name.
    // Content lives in draw_save_row() (the Save module's shared row-drawer,
    // also used standalone by panel_save's own card — see below); folding it
    // in here, uncarded, is what preserves today's exact GAME-card layout.
    if (m->saves_supported) {
        ImGui::Dummy(ImVec2(0, px(8)));
        ImGui::PushStyleColor(ImGuiCol_Separator, col(th.border));
        ImGui::Separator();
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, px(6)));
        draw_save_row(m, th);
    }
    end_panel();
}

// Save module (docs/ARCHITECTURE.md): one reusable picker row — label + path +
// Import/Clear. SAVE_SRAM and (until the block-grid UI lands) SAVE_MEMCARD
// both render this same compact row: kind-switched data, one widget.
void draw_save_row(LauncherModel* m, const LauncherTheme& th) {
    const char* sp = m->sram_path ? m->sram_path : "";
    const char* base = sp;
    for (const char* q = sp; *q; ++q) if (*q == '/' || *q == '\\') base = q + 1;

    // Reflect the ACTUAL save file state, not just the configured path. Showing
    // the filename unconditionally made a present and an absent save look
    // identical — so Clear (which correctly no-ops when there's nothing to
    // delete) read as "broken". Stat the file each frame: show its size when it
    // exists, "no save yet" when it doesn't, and disable Clear when empty so the
    // button's effect is always visible.
    long sz = -1;
    if (sp[0]) { FILE* f = fopen(sp, "rb"); if (f) { fseek(f, 0, SEEK_END); sz = ftell(f); fclose(f); } }
    const bool has_save = sz >= 0;

    ImGui::PushStyleColor(ImGuiCol_Text, col(th.text_muted));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Save");
    ImGui::PopStyleColor();
    ImGui::SameLine(px(76));
    ImGui::AlignTextToFramePadding();
    if (has_save) {
        char lbl[160];
        if (sz >= 1024) snprintf(lbl, sizeof lbl, "%s  (%.0f KB)", base, sz / 1024.0);
        else            snprintf(lbl, sizeof lbl, "%s  (%ld B)", base, sz);
        ImGui::TextUnformatted(lbl);
    } else {
        ImGui::TextColored(col(th.text_muted), "no save yet");
    }
    const float bw = px(84);
    ImGui::SameLine(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - bw*2 - px(th.spacing_sm));
    static const char* kSramPatterns[] = { "*.srm", "*.sav" };
    if (ImGui::Button("Import", ImVec2(bw, px(30)))) {
        char buf[512];
        if (launcher_pick_file("Import SRAM save", kSramPatterns, 2,
                               "SNES save (.srm .sav)", buf, sizeof(buf)))
            launcher_model_import_sram(m, buf);   // backs up existing to .bak, then copies in
    }
    ImGui::SameLine(0, px(th.spacing_sm));
    ImGui::BeginDisabled(!has_save);              // nothing to clear when no save exists
    if (ImGui::Button("Clear", ImVec2(bw, px(30))))
        launcher_model_clear_sram(m);             // backs up to .bak, then deletes
    ImGui::EndDisabled();
}

// ---- panel adapters: LauncherPanelDrawFn = void(LauncherModel*, const LauncherTheme*) ----
void panel_game_draw(LauncherModel* m, const LauncherTheme* th) {
    draw_game_panel(m, *th, g_game_fill_h);
}

// ---- Save module, SAVE_MEMCARD half (PSX) -----------------------------------
// SAVE_SRAM keeps the compact row above, folded into the GAME card (SNES,
// unchanged). SAVE_MEMCARD (PSX) is a standalone WIDE dashboard panel (see
// kPanelsDashboardPsx in launcher_system.h): one sub-section per card slot —
// icon + path picker (Browse/New) + a real 15-block usage grid, matching PS1
// memory-card conventions (each card holds 15 save blocks).

// One compact memory-card slot: icon + name, inline block count, and Browse/
// New actions. `probe` (SystemProfile.save.probe) is the host hook that
// would refresh m->memcard_blocks_used[slot] from the real card image; it is
// NULL in every profile today (unimplemented proto hook), so this falls back to
// a representative placeholder count rather than an all-empty slot.
// Draws the CONTENT of one small memory-card card (the caller supplies the card
// chrome + width). Vertical stack so nothing crowds at a controller-narrow
// width: icon + label, block count, then a Browse/New button pair.
void draw_memcard_slot(LauncherModel* m, const LauncherTheme& th, int slot) {
    const SystemProfile* prof = (const SystemProfile*)m->profile;
    ImGui::PushID(slot);

    const bool enabled = m->s.memcard_enabled[slot] != 0;

    image_fit(g_memcard, 20, 20);
    ImGui::SameLine(0, px(8));
    ImGui::AlignTextToFramePadding();
    const char* mp = m->s.memcard_path[slot];
    const char* base = mp;
    for (const char* q = mp; *q; ++q) if (*q == '/' || *q == '\\') base = q + 1;
    char label[40];
    if (base[0]) snprintf(label, sizeof(label), "%s", base);
    else         snprintf(label, sizeof(label), "Memory Card %d", slot + 1);
    ImGui::PushStyleColor(ImGuiCol_Text, col(enabled ? th.accent : th.text_muted));
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();

    // Per-slot Enable/Disable switch, right-aligned on the header row —
    // mirrors the RmlUi launcher's `.switch`/`toggle_mc1`/`toggle_mc2` (a
    // disabled slot is an empty SIO port once a host wires that up). The
    // header stays full-strength; only the body below dims when disabled.
    {
        const char* tglabel = "Enabled";
        const float tgw = ImGui::GetFrameHeight() + px(6.0f) + ImGui::CalcTextSize(tglabel).x;
        ImGui::SameLine(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - tgw);
        bool enabled_box = enabled;
        if (ImGui::Checkbox(tglabel, &enabled_box))
            launcher_model_toggle_memcard(m, slot);
    }

    // Dim the rest of the slot body when disabled (mirrors the RmlUi
    // launcher's `.card-body.disabled { opacity: 0.4; }` — visual only, the
    // Browse/New controls stay clickable so the slot can be re-configured
    // while off).
    const float body_alpha = enabled ? 1.0f : 0.4f;
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * body_alpha);

    // Block usage source, most-authoritative first: a host memcard_inspect
    // callback (REAL card contents) → a card we just formatted blank (0) → a
    // SystemProfile SaveProbeFn → a representative placeholder pattern.
    uint16_t used;
    if (m->memcard_inspected[slot])
        used = m->memcard_blocks_used[slot];
    else if (m->memcard_freshly_formatted[slot])
        used = 0;
    else if (prof && prof->save.probe && prof->save.probe(m, slot))
        used = m->memcard_blocks_used[slot];
    else
        used = (uint16_t)(slot == 0 ? 0x0025u : 0x0009u);
    int used_count = 0;
    for (int i = 0; i < 15; ++i) if (used & (1u << i)) ++used_count;
    // Block count sits right-aligned on its OWN row (the header row's right
    // side is now the Enabled switch); the 15-block grid is its own
    // full-width row below, with slightly larger cells.
    char cap[20]; snprintf(cap, sizeof(cap), "%d / 15 blocks", used_count);
    const float capw = ImGui::CalcTextSize(cap).x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - capw);
    ImGui::PushStyleColor(ImGuiCol_Text, col(th.text_muted));
    ImGui::TextUnformatted(cap);
    ImGui::PopStyleColor();

    ImGui::Dummy(ImVec2(0, px(6)));
    {
        const int   kB   = 15;
        const float bgap = px(4.0f);
        const float availw = ImGui::GetContentRegionAvail().x;
        float cell = (availw - bgap * (kB - 1)) / (float)kB;
        if (cell > px(18.0f)) cell = px(18.0f);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        for (int i = 0; i < kB; ++i) {
            const bool onb = (used & (1u << i)) != 0;
            const ImVec2 mn(p0.x + i * (cell + bgap), p0.y);
            const ImVec2 mx(mn.x + cell, mn.y + cell);
            dl->AddRectFilled(mn, mx, imcol(onb ? th.accent : th.control), px(3.0f));
            dl->AddRect(mn, mx, imcol(th.border), px(3.0f), 0, px(1.0f));
        }
        ImGui::Dummy(ImVec2(cell * kB + bgap * (kB - 1), cell));
    }

    ImGui::Dummy(ImVec2(0, px(10)));
    static const char* kCardPatterns[] = { "*.mcd", "*.mcr", "*.mc" };
    const float cw = ImGui::GetContentRegionAvail().x;
    const float bw = (cw - px(th.spacing_sm)) * 0.5f;
    if (ImGui::Button("Browse", ImVec2(bw, px(32)))) {
        char buf[512];
        if (launcher_pick_file("Select memory card image", kCardPatterns, 3,
                               "PS1 memory card (.mcd .mcr .mc)", buf, sizeof(buf)))
            launcher_model_set_memcard_path(m, slot, buf);
    }
    ImGui::SameLine(0, px(th.spacing_sm));
    if (ImGui::Button("New", ImVec2(bw, px(32)))) {
        char buf[512];
        // "New" picks a DESTINATION (the file need not exist yet — a save
        // dialog, not the open dialog Browse uses), then writes a real,
        // freshly formatted blank 128KB card there and adopts it.
        if (launcher_pick_save_file("Create new memory card", kCardPatterns, 3,
                                    "PS1 memory card (.mcd)", buf, sizeof(buf)))
            launcher_model_new_memcard(m, slot, buf);
    }

    ImGui::PopStyleVar();  // body_alpha
    ImGui::PopID();
}

// Available whenever this system's SaveSpec says there's something to show:
// SAVE_MEMCARD (PSX) always offers the panel — the memory-card slots exist
// independent of whether the GAME itself also has legacy SRAM — while
// SAVE_SRAM keeps the original per-GAME gate (sram_path != NULL) and
// SAVE_NONE stays hidden.
int avail_save(const LauncherModel* m) {
    const SystemProfile* prof = (const SystemProfile*)m->profile;
    const SaveKind kind = prof ? prof->save.kind : SAVE_NONE;
    if (kind == SAVE_MEMCARD) return 1;
    if (kind == SAVE_SRAM)    return m->saves_supported;
    return 0;
}

void panel_save_draw(LauncherModel* m, const LauncherTheme* th) {
    const SystemProfile* prof = (const SystemProfile*)m->profile;
    const SaveKind kind = prof ? prof->save.kind : SAVE_NONE;
    if (kind == SAVE_MEMCARD) {
        // No outer "MEMORY CARDS" card/eyebrow: the small per-slot cards ARE the
        // UI (each self-labels with a memcard icon + "Memory Card N"). Laid side
        // by side at ~a controller-card width, the whole row is short enough to
        // sit under the controller card without scrolling.
        const int slots = (prof->save.slots > 0 && prof->save.slots <= 2) ? prof->save.slots : 2;
        const float gap = px(th->spacing_sm);
        const float avail = ImGui::GetContentRegionAvail().x;
        const float cw = (avail - gap * (slots - 1)) / (float)slots;
        for (int slot = 0; slot < slots; ++slot) {
            if (slot) ImGui::SameLine(0, gap);
            char cid[16]; snprintf(cid, sizeof(cid), "mcc%d", slot);
            char pid[16]; snprintf(pid, sizeof(pid), "mcp%d", slot);
            begin_container(cid, ImVec2(cw, 0), ImGuiChildFlags_AutoResizeY);
                if (begin_panel(pid, cw, false))
                    draw_memcard_slot(m, *th, slot);
                end_panel();
            end_container();
        }
        return;
    }
    if (!begin_panel("save", 0)) { end_panel(); return; }
    eyebrow("SAVES");
    draw_save_row(m, *th);
    end_panel();
}

// ---- N64 Transfer Pak: one card per controller port -----------------------
// Composes only for games whose GameInfo passes tpak_slots > 0 (the Stadium
// titles) — the availability gate below keeps it off every other console/game.
static const char* elide_left(const char* s, float max_w, char* out, size_t cap);  // fwd (defined below)

int avail_tpak(const LauncherModel* m) { return m->tpak_slots > 0; }

// Real GB-cartridge art for a Transfer Pak slot, picked by the host-reported
// cart kind (1 red / 2 blue / 3 yellow / 4 green); the gray empty shell for an
// empty slot or a recognized-but-uncolored cart. Falls back to a blank box if
// the art didn't load. `box` is the logical fit size.
void draw_tpak_cart(int cart_kind, bool present, float box) {
    const int idx = (present && cart_kind >= 1 && cart_kind <= 4) ? cart_kind : 0;
    image_fit(g_cart[idx], box, box);   // image_fit Dummies when the texture is absent
}

static const char* rui_basename(const char* path) {
    const char* base = path;
    for (const char* q = path; *q; ++q)
        if (*q == '/' || *q == '\\') base = q + 1;
    return base;
}

// Transfer Pak config modal state. A tile click stages a request (open_req);
// the modal itself is drawn once per frame at root scope (draw_tpak_modal) so
// OpenPopup and BeginPopupModal share the same ID stack from wherever a tile
// was clicked (dashboard row OR the Controller page).
static int g_tpak_open_req  = -1;
static int g_tpak_modal_slot = -1;

// One compact port tile: the cartridge (gray shell when empty, colored R/B/Y
// once a cart is set), the cart name, and a Configure button — clicking either
// the cart or Configure opens the config modal. This is ALL that shows inline
// now; picking cart + save happens in the modal, so the dashboard row stays a
// short strip of carts instead of tall cards that push the layout into a
// scroll.
void draw_tpak_tile(LauncherModel* m, const LauncherTheme& th, int slot) {
    char eb[24]; snprintf(eb, sizeof(eb), "TRANSFER PAK %d", slot + 1);
    eyebrow(eb);

    const bool has_cart  = m->s.tpak_rom_path[slot][0] != '\0';
    const bool inspected = m->tpak_inspected[slot];
    const RecompLauncherCTpak* info = &m->tpak_info[slot];
    const float inner = ImGui::GetContentRegionAvail().x;

    // Centered cartridge art, itself a click target that opens the modal.
    ImGui::PushID(slot);
    const float art = px(66);
    ImVec2 art_cursor = ImGui::GetCursorScreenPos();
    image_fit_centered(g_cart[(has_cart && inspected && info->cart_kind >= 1 &&
                               info->cart_kind <= 4) ? info->cart_kind : 0], 60, 66, inner);
    // invisible hit-box over the art
    ImGui::SetCursorScreenPos(art_cursor);
    if (ImGui::InvisibleButton("cart_hit", ImVec2(inner, art))) g_tpak_open_req = slot;

    // name line (centered), muted "Empty" when nothing inserted
    const char* label = !has_cart ? "Empty"
                        : (inspected && info->cart_label[0])
                            ? info->cart_label : rui_basename(m->s.tpak_rom_path[slot]);
    float lw = ImGui::CalcTextSize(label).x;
    if (lw < inner) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (inner - lw) * 0.5f);
    ImGui::TextColored(has_cart ? col(th.text) : col(th.text_muted), "%s", label);
    ImGui::Dummy(ImVec2(0, px(4)));

    if (ImGui::Button(has_cart ? "Configure" : "Insert...",
                      ImVec2(ImGui::GetContentRegionAvail().x, px(28))))
        g_tpak_open_req = slot;
    ImGui::PopID();
}

// The per-port config surface, drawn as a modal. Everything cartridge-related
// lives here: a large live cart preview, the cartridge/trainer facts, and the
// Change / Remove / save-file actions. Reached from any tile (dashboard or
// Controller page). Draw ONCE per frame at root scope.
void draw_tpak_modal(LauncherModel* m, const LauncherTheme& th) {
    if (g_tpak_open_req >= 0) {
        g_tpak_modal_slot = g_tpak_open_req;
        g_tpak_open_req = -1;
        ImGui::OpenPopup("Transfer Pak");
    }
    ImGui::SetNextWindowSize(ImVec2(px(440), 0), ImGuiCond_Appearing);
    // No OS-style title bar (it renders in ImGui's un-themed blue) — the card's
    // own "TRANSFER PAK · PORT n" eyebrow is the heading.
    if (!ImGui::BeginPopupModal("Transfer Pak", nullptr,
                                ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                ImGuiWindowFlags_NoTitleBar))
        return;
    const int slot = g_tpak_modal_slot;
    if (slot < 0 || slot >= m->tpak_slots) { ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }

    const bool has_cart  = m->s.tpak_rom_path[slot][0] != '\0';
    const bool inspected = m->tpak_inspected[slot];
    const RecompLauncherCTpak* info = &m->tpak_info[slot];

    ImGui::PushStyleColor(ImGuiCol_Text, col(th.accent));
    ImGui::Text("TRANSFER PAK  \xC2\xB7  PORT %d", slot + 1);
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, px(6)));

    // Large live cart preview, centered — turns from the gray shell into the
    // colored cart the moment a recognized ROM is picked.
    const float avail = ImGui::GetContentRegionAvail().x;
    image_fit_centered(g_cart[(has_cart && inspected && info->cart_kind >= 1 &&
                               info->cart_kind <= 4) ? info->cart_kind : 0], 132, 138, avail);
    ImGui::Dummy(ImVec2(0, px(6)));

    // name + trainer, centered
    auto centered = [&](ImU32 c, const char* s) {
        float w = ImGui::CalcTextSize(s).x;
        if (w < avail) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - w) * 0.5f);
        ImGui::PushStyleColor(ImGuiCol_Text, c); ImGui::TextUnformatted(s); ImGui::PopStyleColor();
    };
    if (!has_cart) {
        centered(ImGui::GetColorU32(col(th.text_muted)), "No cartridge inserted");
    } else {
        const char* label = (inspected && info->cart_label[0])
                              ? info->cart_label : rui_basename(m->s.tpak_rom_path[slot]);
        centered(ImGui::GetColorU32(col(th.text)), label);
        char line[80];
        if (inspected && info->trainer_name[0]) {
            if (info->trainer_id[0])
                snprintf(line, sizeof(line), "Trainer %s  \xC2\xB7  ID %s",
                         info->trainer_name, info->trainer_id);
            else snprintf(line, sizeof(line), "Trainer %s", info->trainer_name);
        } else snprintf(line, sizeof(line), "No save data");
        centered(ImGui::GetColorU32(col(th.text_muted)), line);
    }

    ImGui::Dummy(ImVec2(0, px(10)));
    // Change / Remove cartridge
    const float full = ImGui::GetContentRegionAvail().x;
    if (ImGui::Button(has_cart ? "Change cartridge..." : "Insert cartridge...",
                      ImVec2(full, px(32)))) {
        static const char* kGbPatterns[] = { "*.gb", "*.gbc" };
        char buf[512];
        if (launcher_pick_file("Select Game Boy cartridge", kGbPatterns, 2,
                               "Game Boy cartridge (.gb .gbc)", buf, sizeof(buf)))
            launcher_model_set_tpak_rom(m, slot, buf);
    }
    if (has_cart) {
        if (ImGui::Button("Remove cartridge", ImVec2(full, px(28))))
            launcher_model_clear_tpak(m, slot);

        // Battery-save row: label + value + Browse / Reset.
        ImGui::Dummy(ImVec2(0, px(8)));
        ImGui::TextColored(col(th.text_muted), "Battery save");
        const char* sv = m->s.tpak_save_path[slot][0]
                           ? rui_basename(m->s.tpak_save_path[slot])
                           : "Default (runtime chooses)";
        char elided[128];
        elide_left(sv, ImGui::GetContentRegionAvail().x, elided, sizeof(elided));
        ImGui::TextUnformatted(elided);
        const float bw = (ImGui::GetContentRegionAvail().x - px(th.spacing_sm)) * 0.5f;
        if (ImGui::Button("Browse save...", ImVec2(bw, px(26)))) {
            static const char* kSavPatterns[] = { "*.sav", "*.srm" };
            char buf[512];
            if (launcher_pick_file("Select battery save", kSavPatterns, 2,
                                   "Battery save (.sav)", buf, sizeof(buf)))
                launcher_model_set_tpak_save(m, slot, buf);
        }
        ImGui::SameLine(0, px(th.spacing_sm));
        if (ImGui::Button("Use default", ImVec2(bw, px(26))))
            launcher_model_set_tpak_save(m, slot, "");
    }

    ImGui::Dummy(ImVec2(0, px(10)));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, px(4)));
    if (ImGui::Button("Done", ImVec2(ImGui::GetContentRegionAvail().x, px(30))))
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void panel_tpak_draw(LauncherModel* m, const LauncherTheme* th) {
    // Compact per-port tiles in one full-width row (click a cart to configure).
    const int slots = m->tpak_slots;
    const float gap = px(th->spacing_sm);
    const float avail = ImGui::GetContentRegionAvail().x;
    const float cw = (avail - gap * (slots - 1)) / (float)slots;
    static const char* kCid[RECOMP_LAUNCHER_MAX_TPAKS] = { "tpc0", "tpc1", "tpc2", "tpc3" };
    static const char* kPid[RECOMP_LAUNCHER_MAX_TPAKS] = { "tpp0", "tpp1", "tpp2", "tpp3" };
    for (int slot = 0; slot < slots; ++slot) {
        if (slot) ImGui::SameLine(0, gap);
        begin_container(kCid[slot], ImVec2(cw, 0), ImGuiChildFlags_AutoResizeY);
            if (begin_panel(kPid[slot], cw, false))
                draw_tpak_tile(m, *th, slot);
            end_panel();
        end_container();
    }
}

// PSX-style 3-way pad-mode selector: Hybrid / Analog / D-Pad segmented row.
// Caller only draws this when pad_mode_supported && pad_mode_selectable (a
// locked mode draws nothing — there's nothing to pick). The Hybrid segment is
// itself hidden when !allow_hybrid, matching the real RmlUi PSX launcher.
void pad_mode_selector(LauncherModel* m, const LauncherTheme& th, int p, float w) {
    struct Seg { int mode; const char* label; };
    Seg segs[3];
    int n = 0;
    if (m->allow_hybrid) segs[n++] = { 0, "Hybrid" };
    segs[n++] = { 1, "Analog" };
    segs[n++] = { 2, "D-Pad" };

    const float gap = px(4.0f);
    const float seg_w = (w - gap * (n - 1)) / n;
    for (int i = 0; i < n; ++i) {
        if (i) ImGui::SameLine(0, gap);
        bool sel = m->s.pad_mode[p] == segs[i].mode;
        ImGui::PushID(i);
        ImGui::PushStyleColor(ImGuiCol_Button, sel ? col(th.accent) : col(th.control));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, sel ? col(th.accent) : col(th.control_hovered));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, col(th.accent));
        ImGui::PushStyleColor(ImGuiCol_Text, sel ? col(th.accent_text) : col(th.text));
        if (ImGui::Button(segs[i].label, ImVec2(seg_w, px(28))))
            launcher_model_set_pad_mode(m, p, segs[i].mode);
        ImGui::PopStyleColor(4);
        ImGui::PopID();
    }
}

// Each player is its OWN self-contained card ("PLAYER 1" as its eyebrow), not a
// floating column inside one big CONTROLLERS box. A 1-player game shows a
// single card (no wasted width); a 2-player game shows two identical cards side
// by side. Same module, composed per the game's declared player count.
void draw_player_panel(LauncherModel* m, const LauncherTheme& th, int p, float w) {
    char id[24];  snprintf(id, sizeof(id), "player%d", p);
    char eb[16];  snprintf(eb, sizeof(eb), "PLAYER %d", p + 1);

    if (!begin_panel(id, w, false)) { end_panel(); return; }
    ImGui::PushID(p);
    eyebrow(eb);

    const float inner = ImGui::GetContentRegionAvail().x;
    const float cw    = inner;   // controls span the card => flush by construction

    // pad art centered in the card: PSX-style games swap analog/digital art
    // with the mode; consoles without pad modes (SNES) always show the
    // generic pad.tga.
    {
        const bool digital = m->pad_mode_supported && m->s.pad_mode[p] == 2;
        const LauncherTexture& art = m->pad_mode_supported
            ? (digital ? g_pad_digital : g_pad_analog) : g_pad;
        // Center on the FITTED width so a near-square pad (N64) sits centered,
        // not left-shifted by the landscape box's spare width.
        image_fit_centered(art, 120, 78, inner);
    }
    ImGui::Dummy(ImVec2(0, px(6)));

    // Pad-mode selector: only when the game supports pad modes AND the mode
    // is user-selectable (not locked to a single mode).
    if (m->pad_mode_supported && m->pad_mode_selectable) {
        pad_mode_selector(m, th, p, cw);
        ImGui::Dummy(ImVec2(0, px(6)));
    }

    ImGui::SetNextItemWidth(cw);
    if (ImGui::BeginCombo("##src", launcher_model_player_src_label(m, p))) {
        if (ImGui::Selectable("None", m->s.player_src[p] == 0))
            launcher_model_set_source(m, p, 0, 0, nullptr);
        if (ImGui::Selectable("Keyboard", m->s.player_src[p] == 1))
            launcher_model_set_source(m, p, 1, 0, nullptr);
        for (int i = 0; i < g_pad_count; ++i) {
            bool sel = m->s.player_src[p] == 2 && m->player_pad_id[p] == g_pads[i].id;
            if (ImGui::Selectable(g_pads[i].name, sel))
                launcher_model_set_source(m, p, 2, g_pads[i].id, g_pads[i].name);
        }
        if (g_pad_count == 0) {
            ImGui::BeginDisabled();
            ImGui::Selectable("(no gamepad connected)");
            ImGui::EndDisabled();
        }
        ImGui::EndCombo();
    }
    ImGui::Dummy(ImVec2(0, px(4)));
    // Configure + connection status share ONE half/half row (Configure left,
    // status right) so the card stays short — that keeps the memory cards below
    // it from being pushed off the bottom. (Analog-stick deadzone lives on the
    // Configure page's per-player Deadzone stepper, not on this card.)
    {
        const float gap  = px(th.spacing_sm);
        const float half = (cw - gap) * 0.5f;
        const float btnh = px(32);
        if (ImGui::Button("Configure", ImVec2(half, btnh))) launcher_model_open_config(m, p);
        ImGui::SameLine(0, gap);
        const bool on = m->s.player_src[p] != 0;
        const char* st = on ? "connected" : "not assigned";
        const float sw = px(10) + px(8) + ImGui::CalcTextSize(st).x;
        // center the dot+label within the right half, vertically on the button
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (half - sw) * 0.5f);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (btnh - ImGui::GetTextLineHeight()) * 0.5f);
        draw_dot(on, th.good, th.text_muted);
        ImGui::TextColored(on ? col(th.good) : col(th.text_muted), "%s", st);
    }
    ImGui::PopID();
    end_panel();
}

// Lays out the player cards: one card for a 1-player game, two side-by-side
// for a 2-player game, a 2x2 grid for a 4-port console (N64). Driven by the
// model, never hardcoded.
void draw_controllers_row(LauncherModel* m, const LauncherTheme& th) {
    if (m->lock_device) return;   // fixed pad: hide the player controller cards entirely
    int n = m->player_count;
    if (n < 1) n = 1;
    if (n > LNG_MAX_PLAYERS) n = LNG_MAX_PLAYERS;
    const float gap = px(th.spacing_md);
    const float availw = ImGui::GetContentRegionAvail().x;
    // A 2P game splits the row; a 1P game gets ONE card of the same size rather
    // than a full-width card with a lone pad floating in it. 3-4 players wrap
    // into rows of two same-size cards.
    float cardw = (availw - gap) * 0.5f;
    if (n == 1 && cardw < px(300.0f)) cardw = availw;   // narrow window: fill
    static const char* kCardIds[LNG_MAX_PLAYERS] = { "pc0", "pc1", "pc2", "pc3" };
    for (int p = 0; p < n; ++p) {
        if (p & 1) ImGui::SameLine(0, gap);
        else if (p) ImGui::Dummy(ImVec2(0, gap));   // new row of cards
        begin_container(kCardIds[p], ImVec2(cardw, 0), ImGuiChildFlags_AutoResizeY);
        draw_player_panel(m, th, p, cardw);
        end_container();
    }
}

void panel_controller_draw(LauncherModel* m, const LauncherTheme* th) {
    draw_controllers_row(m, *th);
}

// The dashboard COMPOSES whichever panels this game's SystemProfile lists in
// panels_dashboard — it does not hardcode a fixed set. GAME is always
// present; the side column stacks CONTROLLERS plus any optional modules
// (SAVES only when the game has SRAM, folded into the GAME card — see
// draw_save_row). A different system's profile simply contributes a
// different panel list.
void draw_dashboard(LauncherModel* m, const LauncherTheme& th, int logical_w) {
    const SystemProfile* prof = (const SystemProfile*)m->profile;
    const LauncherPanel* game_p = find_composed(prof->panels_dashboard, "game", m);
    const LauncherPanel* ctrl_p = find_composed(prof->panels_dashboard, "controller", m);
    const LauncherPanel* save_p = find_composed(prof->panels_dashboard, "save", m);
    const LauncherPanel* tpak_p = find_composed(prof->panels_dashboard, "tpak", m);

    if (logical_w >= 820) {
        const float gap = px(th.spacing_md);
        // When a WIDE panel (save/tpak) follows this row, the columns must hug
        // their own content (AutoResizeY) instead of stretching to fill the
        // whole scrollable "body" — otherwise there's never any room left
        // below them and the WIDE panel silently draws off the bottom edge.
        // SNES's composition never lists "save" here (save_p == nullptr), so
        // it keeps the original fill-to-height columns byte-identical.
        const bool hug = (save_p != nullptr) || (tpak_p != nullptr);
        const ImGuiChildFlags col_flags = hug ? ImGuiChildFlags_AutoResizeY : ImGuiChildFlags_None;
        // Art-led left column sized to the box art; side column takes the rest.
        if (game_p) {
            g_game_fill_h = !hug;
            begin_container("dash_l", ImVec2(px(400), 0), col_flags);
            game_p->draw(m, &th);
            end_container();
        }

        if (game_p && ctrl_p) ImGui::SameLine(0, gap);
        if (ctrl_p) {
            begin_container("dash_r", ImVec2(0, 0), col_flags);
                // The side column stacks the controller card(s) and, directly
                // beneath, the memory-card card at the SAME width as a controller
                // card — so the user sees memcards without scrolling below the
                // GAME card (they sit beside it, not under the whole row).
                ctrl_p->draw(m, &th);
                if (save_p) {
                    // The memory-card panel spans the side column and lays its
                    // small per-slot cards side by side (each ~a controller-card
                    // width), so the row is short and sits under the controller.
                    ImGui::Dummy(ImVec2(0, gap));
                    save_p->draw(m, &th);
                }
            end_container();
        }
        // Transfer Pak: a genuinely FULL-WIDTH row under both columns — four
        // per-port cards need the whole window, not the side column.
        if (tpak_p) {
            ImGui::Dummy(ImVec2(0, gap));
            tpak_p->draw(m, &th);
        }
    } else {
        if (game_p) { g_game_fill_h = false; game_p->draw(m, &th); }
        if (game_p && ctrl_p) ImGui::Spacing();
        if (ctrl_p) ctrl_p->draw(m, &th);
        // Narrow single-column layout: memcards stack under the controller.
        if (save_p) { ImGui::Spacing(); save_p->draw(m, &th); }
        if (tpak_p) { ImGui::Spacing(); tpak_p->draw(m, &th); }
    }
}

// Trim a path from the LEFT to fit max_w, prefixing "…" so the meaningful tail
// stays visible (e.g. "…\build\bin-x64-Release\msu"). Same idea as the old hash
// ellipsis.
static const char* elide_left(const char* s, float max_w, char* out, size_t cap) {
    if (ImGui::CalcTextSize(s).x <= max_w) { snprintf(out, cap, "%s", s); return out; }
    size_t n = strlen(s);
    for (size_t start = 1; start < n; ++start) {
        char tmp[320];
        snprintf(tmp, sizeof(tmp), "\xE2\x80\xA6%s", s + start);   // "…" + tail
        if (ImGui::CalcTextSize(tmp).x <= max_w) { snprintf(out, cap, "%s", tmp); return out; }
    }
    snprintf(out, cap, "\xE2\x80\xA6");
    return out;
}

// True when this game exposes ANY of the deeper PSX-style DISPLAY controls.
// SNES (and any console leaving every has_* flag 0) takes the legacy-only
// branch below and gets EXACTLY today's 3-row DISPLAY card, unchanged.
bool any_deep_display(const LauncherModel* m) {
    return m->has_window_size || m->has_renderer || m->has_supersampling ||
           m->has_antialiasing || m->has_texture_filter || m->has_screen_kind ||
           m->has_frame_interp || m->has_skip_fmv || m->has_turbo_loads ||
           m->has_fullscreen_toggle;
}

// Inline amber "EXPERIMENTAL" tag, drawn on the same row right after a control
// whose feature is not yet production-ready. Currently every SNES 16:9
// widescreen path is experimental (per-game rendering still maturing), so the
// widescreen checkbox always carries this marker. Uses the same th.warn amber
// as the other cautionary labels (e.g. the MSU-1 notice).
static void experimental_tag(const LauncherTheme& th) {
    ImGui::SameLine(0, px(8));
    ImGui::TextColored(col(th.warn), "EXPERIMENTAL");
}

void draw_display_controls(LauncherModel* m, const LauncherTheme& th) {
    eyebrow("DISPLAY");

    if (!any_deep_display(m)) {
        // ---- legacy minimal surface (SNES etc.) — aligned label grid ----------
        float cw = ImGui::CalcTextSize("Linear filtering").x;   // widest legacy label
        { float t = ImGui::CalcTextSize("Widescreen 16:9").x; if (t > cw) cw = t; }
        cw += px(18.0f);
        row_label("Window scale", th, cw);
        if (ImGui::Button(launcher_model_scale_label(m), ImVec2(px(120), px(30))))
            launcher_model_cycle_scale(m);
        row_label("Linear filtering", th, cw);
        bool filter = m->s.linear_filter != 0;
        if (ImGui::Checkbox("##filter", &filter)) launcher_model_toggle_filter(m);
        if (m->aspect_mask || m->num_aspect_labels > 0) {   // PSX-style cycle, or a game-supplied vocabulary
            row_label("Aspect ratio", th, cw);
            if (ImGui::Button(launcher_model_aspect_label(m), ImVec2(px(180), px(30))))
                launcher_model_cycle_aspect(m);
            if (m->aspect_experimental) experimental_tag(th);
        } else if (m->widescreen_supported) {   // legacy module: only for games that support it
            row_label("Widescreen 16:9", th, cw);
            bool ws = m->s.widescreen != 0;
            if (ImGui::Checkbox("##ws", &ws)) launcher_model_toggle_widescreen(m);
            experimental_tag(th);
        }
        return;
    }

    // ---- deeper PSX-style surface, capability-gated per control -----------
    // Order matches the real RmlUi PSX launcher: Window size, Renderer,
    // Supersampling, Aspect ratio, Texture filtering, Antialiasing, Screen
    // model, Frame interpolation (+Presentation target), Skip FMVs, Turbo
    // loads, Fullscreen.
    if (m->has_window_size) {
        row_label("Window size", th);
        if (ImGui::Button(launcher_model_window_size_label(m), ImVec2(px(150), px(30))))
            launcher_model_cycle_window_size(m);
    } else {
        row_label("Window scale", th);
        if (ImGui::Button(launcher_model_scale_label(m), ImVec2(px(120), px(30))))
            launcher_model_cycle_scale(m);
    }

    if (m->has_renderer) {
        row_label("Renderer", th);
        if (ImGui::Button(launcher_model_renderer_label(m), ImVec2(px(120), px(30))))
            launcher_model_toggle_renderer(m);
    }

    if (m->has_supersampling) {
        row_label("Supersampling", th);
        if (ImGui::Button(launcher_model_supersampling_label(m), ImVec2(px(90), px(30))))
            launcher_model_cycle_supersampling(m);
    }

    if (m->aspect_mask || m->num_aspect_labels > 0) {
        row_label("Aspect ratio", th);
        if (ImGui::Button(launcher_model_aspect_label(m), ImVec2(px(180), px(30))))
            launcher_model_cycle_aspect(m);
        if (m->aspect_experimental) experimental_tag(th);
    } else if (m->widescreen_supported) {
        row_label("Widescreen 16:9", th);
        bool ws = m->s.widescreen != 0;
        if (ImGui::Checkbox("##ws", &ws)) launcher_model_toggle_widescreen(m);
        experimental_tag(th);
    }

    if (m->has_texture_filter) {
        row_label("Texture filtering", th);
        if (ImGui::Button(launcher_model_texture_filter_label(m), ImVec2(px(120), px(30))))
            launcher_model_toggle_texture_filter(m);
    } else {
        row_label("Linear filtering", th);
        bool filter = m->s.linear_filter != 0;
        if (ImGui::Checkbox("##filter", &filter)) launcher_model_toggle_filter(m);
    }

    if (m->has_antialiasing) {
        row_label("Antialiasing", th);
        if (ImGui::Button(launcher_model_aa_label(m), ImVec2(px(90), px(30))))
            launcher_model_cycle_aa(m);
    }

    if (m->has_screen_kind) {
        row_label("Screen model", th);
        if (ImGui::Button(launcher_model_screen_kind_label(m), ImVec2(px(130), px(30))))
            launcher_model_cycle_screen_kind(m);
    }

    // Frame interpolation is only meaningful under OpenGL (Software has no
    // interpolation pass); Presentation target only matters once frame
    // interpolation is actually on.
    if (m->has_frame_interp && m->s.renderer) {
        row_label("Frame interpolation", th);
        bool fi = m->s.frame_interp != 0;
        if (ImGui::Checkbox("##fi", &fi)) launcher_model_toggle_frame_interp(m);
        if (m->s.frame_interp) {
            row_label("Presentation target", th);
            if (ImGui::Button(launcher_model_interp_fps_label(m), ImVec2(px(150), px(30))))
                launcher_model_cycle_interp_fps(m);
        }
    }

    if (m->has_skip_fmv) {
        row_label("Skip FMVs", th);
        bool sk = m->s.auto_skip_fmv != 0;
        if (ImGui::Checkbox("##skipfmv", &sk)) launcher_model_toggle_skip_fmv(m);
    }

    if (m->has_turbo_loads) {
        row_label("Turbo loads", th);
        bool tl = m->s.turbo_loads != 0;
        if (ImGui::Checkbox("##turbo", &tl)) launcher_model_toggle_turbo_loads(m);
    }

    if (m->has_fullscreen_toggle) {
        row_label("Fullscreen on launch", th);
        bool fs = m->s.fullscreen != 0;
        if (ImGui::Checkbox("##fson", &fs)) launcher_model_toggle_fullscreen(m);
    }
}

// Video/Display module (docs/ARCHITECTURE.md): base window-scale/fullscreen
// row set, specialized per system — SNES adds linear-filter + widescreen,
// PSX adds the full deep surface (window size/renderer/supersampling/aspect/
// texture-filter/AA/screen-model/frame-interp/skip-fmv/turbo/fullscreen).
// draw_display_controls() gates each row on the model's has_* caps (sourced
// from the ABI, unchanged); this adapter supplies the card chrome, choosing
// AutoResizeY (deep surface, more rows than the fixed band fits) vs a fixed
// row_h band with no_scroll (legacy minimal surface) — exactly the sizing
// draw_settings used to pick inline, now co-located with its own content.
void panel_video_draw(LauncherModel* m, const LauncherTheme* th) {
    if (any_deep_display(m)) {
        if (begin_panel("disp", 0, false)) draw_display_controls(m, *th);
        end_panel();
    } else {
        if (begin_panel("disp", 0, true, /*no_scroll*/true)) draw_display_controls(m, *th);
        end_panel();
    }
}

void draw_audio_controls(LauncherModel* m, const LauncherTheme& th) {
    eyebrow("AUDIO");
    // Fixed label column so the sample-rate button, the volume stepper's "-",
    // and the SPU toggle all share the same left edge (aligned grid).
    float cw = ImGui::CalcTextSize("Sample rate").x;
    if (m->has_spu_hq) { float t = ImGui::CalcTextSize("High-quality SPU").x; if (t > cw) cw = t; }
    cw += px(18.0f);
    row_label("Sample rate", th, cw);
    if (ImGui::Button(launcher_model_freq_label(m), ImVec2(px(120), px(30))))
        launcher_model_cycle_freq(m);
    row_label("Volume", th, cw);
    int dv = 0; stepper("vol", m->s.volume, "%", &dv);
    if (dv) launcher_model_volume_delta(m, dv);

    // Output-device pick (host-enumerated names; N64/RT64 hosts) — "(system
    // default)" first, committing "" so an unplugged device degrades sanely.
    if (m->num_audio_devices > 0 && m->audio_device_labels) {
        row_label("Output device", th, cw);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        if (ImGui::BeginCombo("##audiodev", launcher_model_audio_device_label(m))) {
            if (ImGui::Selectable("(system default)", m->s.audio_device[0] == '\0'))
                launcher_model_set_audio_device(m, NULL);
            for (int i = 0; i < m->num_audio_devices; ++i) {
                const char* name = m->audio_device_labels[i];
                if (!name || !name[0]) continue;
                bool sel = strcmp(m->s.audio_device, name) == 0;
                if (ImGui::Selectable(name, sel))
                    launcher_model_set_audio_device(m, name);
            }
            ImGui::EndCombo();
        }
    }

    if (m->has_spu_hq) {
        row_label("High-quality SPU", th, cw);
        bool hq = m->s.spu_hq != 0;
        if (ImGui::Checkbox("##spuhq", &hq)) launcher_model_toggle_spu_hq(m);
    }

    // NOTE: analog deadzone is NOT here — it belongs to the input device, so it
    // lives on each controller card (draw_player_panel), gated on
    // has_deadzone_pct. Kept out of Audio deliberately.

    // MSU-1: no header/subsection — just one line under the rows above:
    //   [x] Enable MSU-1 music (?)   …folder tail     [Browse]
    if (m->msu1_supported) {
        bool on = m->s.msu1_enabled != 0;
        if (ImGui::Checkbox("Enable MSU-1 music", &on))
            launcher_model_toggle_msu1(m);
        if (m->msu1_note && m->msu1_note[0]) {
            ImGui::SameLine(0, px(6));
            ImGui::TextColored(col(th.accent), "(?)");
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(px(360));
                ImGui::TextUnformatted(m->msu1_note);
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }
        }
        const float bw = px(78);
        ImGui::SameLine(0, px(14));
        float avail = ImGui::GetContentRegionAvail().x - bw - px(th.spacing_sm);
        if (avail < px(50)) avail = px(50);
        const char* dir = m->s.msu1_dir[0] ? m->s.msu1_dir : "(not set)";
        char elided[192]; elide_left(dir, avail, elided, sizeof(elided));
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(col(th.text_muted), "%s", elided);
        ImGui::SameLine(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - bw);
        if (ImGui::Button("Browse", ImVec2(bw, px(30)))) {  // px(30) matches the other settings buttons + the row's frame height (px(28) sat the label high)
            char buf[512];
            if (launcher_pick_folder("Select MSU-1 music folder", buf, sizeof(buf)))
                launcher_model_set_msu1_dir(m, buf);
        }
    }

    // Localization: only games that declare a language list get this
    // mini-section (mirrors the real PSX launcher's Language cycle).
    if (m->num_languages > 0) {
        ImGui::Dummy(ImVec2(0, px(6)));
        eyebrow("LOCALIZATION");
        row_label("Language", th);
        if (ImGui::Button(launcher_model_language_label(m), ImVec2(px(140), px(30))))
            launcher_model_cycle_language(m);
    }
}

// Audio module adapter — same AutoResizeY-vs-fixed-row_h sizing pattern as
// panel_video_draw, decided from the same "deep" predicate draw_settings used
// to compute inline.
void panel_audio_draw(LauncherModel* m, const LauncherTheme* th) {
    const bool deep_audio = m->has_spu_hq || m->num_languages > 0 || m->num_audio_devices > 0;   /* deadzone moved to controller card */
    if (deep_audio) {
        if (begin_panel("audio", 0, false)) draw_audio_controls(m, *th);
        end_panel();
    } else {
        if (begin_panel("audio", 0, true, /*no_scroll*/true)) draw_audio_controls(m, *th);
        end_panel();
    }
}

// SYSTEM module: BIOS path picker — a half-width card stacked under AUDIO in
// the right column (see draw_settings), composed only for systems whose
// profile lists "system" (PSX, GBA) AND only shown for a game instance that
// needs one (has_bios) — composition + availability, both layers, matching
// the architecture.
int avail_system(const LauncherModel* m) { return m->has_bios; }
void draw_system_controls(LauncherModel* m, const LauncherTheme& th) {
    eyebrow("SYSTEM");
    row_label("BIOS", th);
    const float bw = px(78);
    float avail = ImGui::GetContentRegionAvail().x - bw - px(th.spacing_sm);
    if (avail < px(50)) avail = px(50);
    const char* bp = m->s.bios_path[0] ? m->s.bios_path : "(default)";
    char elided[192]; elide_left(bp, avail, elided, sizeof(elided));
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(col(th.text), "%s", elided);
    ImGui::SameLine(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - bw);
    if (ImGui::Button("Browse", ImVec2(bw, px(28)))) {
        char buf[512];
        static const char* kBiosPatterns[] = { "*.bin", "*.rom" };
        if (launcher_pick_file("Select BIOS file", kBiosPatterns, 2,
                               "BIOS image (.bin .rom)", buf, sizeof(buf)))
            launcher_model_set_bios_path(m, buf);
    }
}
void panel_system_draw(LauncherModel* m, const LauncherTheme* th) {
    if (begin_panel("system", 0)) draw_system_controls(m, *th);
    end_panel();
}

// Hotkeys module: the universal emulator-hotkeys catalog, opt-in per system
// via SystemProfile.hotkeys_mask. SNES opts into LNG_HOTKEYS_ALL — the full
// catalog, grid byte-identical to the original hardcoded panel. PSX opts into
// a narrower everyday-transport subset (launcher_system.h), so its grid packs
// only the set bits — no holes, columns re-wrap around the smaller count.
void draw_hotkeys_controls(LauncherModel* m, const LauncherTheme& th) {
    eyebrow("HOTKEYS");
    const SystemProfile* prof = (const SystemProfile*)m->profile;
    const uint32_t mask = prof ? prof->hotkeys_mask : LNG_HOTKEYS_ALL;
    // Same responsive grid treatment as the bindings list.
    const float cell_w = px(280.0f);
    int cols = (int)(ImGui::GetContentRegionAvail().x / cell_w);
    cols = cols < 1 ? 1 : (cols > 3 ? 3 : cols);
    // Uniform label column: measure the widest bound hotkey name up front so
    // every bind button in the grid starts at the same x within its cell —
    // aligned left edges (and, since the buttons are fixed-width, aligned right
    // edges too), instead of hugging each variable-width label.
    float label_w = 0.0f;
    for (int h = 0; h < LNG_HK_COUNT; ++h) {
        if (!(mask & (1u << h))) continue;
        float w = ImGui::CalcTextSize(launcher_hotkey_name((LngHotkey)h)).x;
        if (w > label_w) label_w = w;
    }
    label_w += px(16.0f);   // gap between label and its bind button
    if (ImGui::BeginTable("hk", cols)) {
        for (int h = 0; h < LNG_HK_COUNT; ++h) {
            if (!(mask & (1u << h))) continue;
            ImGui::TableNextColumn();
            ImGui::PushID(h);
            const char* hkname = launcher_hotkey_name((LngHotkey)h);
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(col(th.text_muted), "%s", hkname);
            // Pad with RELATIVE spacing (not absolute x) so the bind button
            // starts at a uniform offset within every table cell — absolute
            // SameLine() fights ImGui's per-cell cursor tracking and spills
            // buttons into the wrong column.
            ImGui::SameLine(0.0f, label_w - ImGui::CalcTextSize(hkname).x);
            const bool cap = m->hk_capturing && m->capture_hk == (LngHotkey)h;
            const char* lbl = cap ? "[ press... ]"
                            : m->hotkeys[h][0] ? m->hotkeys[h] : "(unbound)";
            if (cap) ImGui::PushStyleColor(ImGuiCol_Button, col(th.accent));
            if (ImGui::Button(lbl, ImVec2(px(130), 0)))
                launcher_model_begin_hk_capture(m, (LngHotkey)h);
            if (cap) ImGui::PopStyleColor();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}
void panel_hotkeys_draw(LauncherModel* m, const LauncherTheme* th) {
    if (begin_panel("hotkeys", 0)) draw_hotkeys_controls(m, *th);
    end_panel();
}

// The settings VIEW composes whichever panels this game's SystemProfile
// lists in panels_settings, in order: DISPLAY (MAIN) + AUDIO (SIDE) share the
// top band; SYSTEM (SIDE, PSX/GBA only — its BIOS "Browse" row) then stacks
// directly beneath AUDIO as a second half-width card in the SAME right
// column, instead of spanning full width under both columns; any WIDE panels
// (HOTKEYS) still stack full-width below that — exactly today's fixed layout
// otherwise, now driven by the composition array + the registry's
// available() gate instead of hardcoded calls.
void draw_settings(LauncherModel* m, const LauncherTheme& th) {
    // Row 1: DISPLAY | AUDIO share the top band. For the legacy minimal
    // surface (no deep caps set — e.g. SNES) both cards are pinned to the
    // SAME fixed height, exactly as before, so that screenshot is unchanged.
    // A PSX-style game with the deeper capability set has far more rows than
    // that fixed height fits — rather than clip (or reintroduce a stray
    // scrollbar via no_scroll on an overflowing fixed-height card), those
    // cards switch to AutoResizeY so they simply grow to fit their content.
    // (Same "deep" predicates panel_video_draw/panel_audio_draw use for their
    // OWN inner card — computed twice, independently, so outer/inner sizing
    // never has to be threaded through the generic draw(model,theme) signature.)
    const SystemProfile* prof = (const SystemProfile*)m->profile;
    const float gap  = px(th.spacing_md);
    const float half = (ImGui::GetContentRegionAvail().x - gap) * 0.5f;
    const float row_h = px(198.0f);   // legacy fixed band height

    const bool deep_display = any_deep_display(m);
    const bool deep_audio   = m->has_spu_hq || m->num_languages > 0 || m->num_audio_devices > 0;   /* deadzone moved to controller card */

    const LauncherPanel* video_p   = find_composed(prof->panels_settings, "video", m);
    const LauncherPanel* audio_p   = find_composed(prof->panels_settings, "audio", m);
    const LauncherPanel* system_p  = find_composed(prof->panels_settings, "system", m);
    const LauncherPanel* hotkeys_p = find_composed(prof->panels_settings, "hotkeys", m);

    // Left content edge in SCREEN space — where DISPLAY starts and where any
    // full-width content (HOTKEYS) below both columns must resume.
    const float content_left_x = ImGui::GetCursorScreenPos().x;

    float left_bottom = 0.0f;   // DISPLAY's bottom edge (screen space)
    if (video_p) {
        if (deep_display) begin_container("set_l", ImVec2(half, 0), ImGuiChildFlags_AutoResizeY);
        else               begin_container("set_l", ImVec2(half, row_h));
        video_p->draw(m, &th);
        end_container();
        left_bottom = ImGui::GetItemRectMax().y;
    }
    if (video_p && audio_p) ImGui::SameLine(0, gap);
    // Capture the right column's SCREEN x BEFORE opening AUDIO's child, so a
    // SYSTEM card composed alongside it (SIDE slot — see kPanelRegistry) can
    // be reopened at the same x once AUDIO's child ends (a finished child,
    // like any item, returns the cursor to the LEFT edge of the row on the
    // next line, not to its own column).
    const float right_x = ImGui::GetCursorScreenPos().x;
    float audio_bottom = 0.0f;   // AUDIO's bottom edge (screen space)
    if (audio_p) {
        if (deep_audio) begin_container("set_r", ImVec2(half, 0), ImGuiChildFlags_AutoResizeY);
        else             begin_container("set_r", ImVec2(half, row_h));
        audio_p->draw(m, &th);
        end_container();
        audio_bottom = ImGui::GetItemRectMax().y;
    }

    // SYSTEM (PSX/GBA's BIOS row) is a SIDE-slot card: stack it as a second
    // half-width card directly under AUDIO, in the same right column, rather
    // than spanning the full width below both columns. Falls back to full
    // width only if some profile composes "system" without "audio".
    if (system_p) {
        if (audio_p) {
            // Place SYSTEM's top edge one standard card-gap below AUDIO's
            // bottom edge specifically (not "wherever the taller of the two
            // columns ended up", which is what plain SameLine/next-line flow
            // would give — DISPLAY's deep surface is usually taller than
            // AUDIO, and that gap would show as dead space above SYSTEM).
            ImGui::SetCursorScreenPos(ImVec2(right_x, audio_bottom + gap));
            begin_container("set_r2", ImVec2(half, 0), ImGuiChildFlags_AutoResizeY);
            system_p->draw(m, &th);
            end_container();
            const float system_bottom = ImGui::GetItemRectMax().y;
            // The manual SetCursorScreenPos above pulled SYSTEM out of the
            // normal same-line row flow, so ImGui's auto-advanced cursor now
            // only accounts for SYSTEM's own bottom, not DISPLAY's (which can
            // still be the taller column). Explicitly resume the layout below
            // whichever column is taller so HOTKEYS never overlaps DISPLAY.
            const float below_y = (left_bottom > system_bottom) ? left_bottom : system_bottom;
            ImGui::SetCursorScreenPos(ImVec2(content_left_x, below_y + gap));
        } else {
            system_p->draw(m, &th);
        }
    }
    if (hotkeys_p) hotkeys_p->draw(m, &th);
}

// CONTROLLER-view rebind page: input source + deadzone, and the keyboard
// bindings grid — reached from the dashboard CONTROLLER panel's Configure
// button. The bindings grid walks the ACTIVE SystemProfile's
// ControllerSpec.buttons[]/button_count (launcher_system.h) so each system
// renders its own real vocabulary (SNES: A/B/X/Y/L/R/...; PSX: Triangle/
// Circle/Cross/Square/L1/L2/R1/R2/L3/R3/...) instead of a hardcoded SNES set.
void draw_controller_config_view(LauncherModel* m, const LauncherTheme& th) {
    const int p = m->cfg_player;
    if (begin_panel("cfg_src", 0)) {
        ImGui::PushStyleColor(ImGuiCol_Text, col(th.accent));
        ImGui::Text("CONTROLLER - PLAYER %d", p + 1); ImGui::PopStyleColor(); ImGui::Spacing();
        row_label("Input source", th);
        ImGui::SetNextItemWidth(px(200));
        if (ImGui::BeginCombo("##csrc", launcher_model_player_src_label(m, p))) {
            if (ImGui::Selectable("None", m->s.player_src[p] == 0))
                launcher_model_set_source(m, p, 0, 0, nullptr);
            if (ImGui::Selectable("Keyboard", m->s.player_src[p] == 1))
                launcher_model_set_source(m, p, 1, 0, nullptr);
            for (int i = 0; i < g_pad_count; ++i) {
                bool sel = m->s.player_src[p] == 2 && m->player_pad_id[p] == g_pads[i].id;
                if (ImGui::Selectable(g_pads[i].name, sel))
                    launcher_model_set_source(m, p, 2, g_pads[i].id, g_pads[i].name);
            }
            if (g_pad_count == 0) {
                ImGui::BeginDisabled();
                ImGui::Selectable("(no gamepad connected)");
                ImGui::EndDisabled();
            }
            ImGui::EndCombo();
        }
        row_label("Deadzone", th);
        int dz = 0; stepper("dz", m->s.deadzone[p], "%", &dz);
        if (dz) launcher_model_deadzone_delta(m, p, dz);
    } end_panel();

    // Transfer Pak for THIS controller port (N64 tpak games), so it's reachable
    // from the Controller page without scrolling the dashboard. Same compact
    // tile + config modal; the port is the player being configured.
    if (m->tpak_slots > p) {
        if (begin_panel("cfg_tpak", 0)) {
            draw_tpak_tile(m, th, p);
        } end_panel();
    }

    // Some systems ship no rebindable input layer (N64 Snap / PMS-J read no
    // input.cfg): GameInfo.hide_rebind drops the bindings card entirely and the
    // Controller view is source+deadzone only.
    if (m->hide_rebind) return;

    if (begin_panel("cfg_binds", 0)) {
        // Responsive grid: fit as many label+chip columns as the width allows
        // (1..4) instead of one tall column with dead space to the right.
        const SystemProfile* prof = (const SystemProfile*)m->profile;
        const ControllerSpec& spec = prof->controller;
        // Alternate binds per input (N64's input.cfg keeps two; SNES/PSX/GBA
        // keep one). 0 in the spec reads as 1 (older positional initializers).
        const int bpi = spec.binds_per_input < 1 ? 1 : spec.binds_per_input;

        // Stores that follow the input SOURCE (N64: one shared table per device
        // TYPE) must re-read display strings on entry so switching
        // Keyboard<->pad shows the table actually in effect. Single-bind stores
        // are per-player and unaffected by the source, so skip the refresh to
        // keep their behaviour byte-identical.
        if (bpi >= 2) launcher_binds_refresh(m);

        // When the player's source is a gamepad the N64 store captures pad
        // fields, not keys — reflect that in the card title and the capture
        // placeholder.
        const bool pad_cap = launcher_binds_wants_pad_capture(m, p + 1) != 0;
        ImGui::PushStyleColor(ImGuiCol_Text, col(th.accent));
        if (pad_cap) ImGui::TextUnformatted("CONTROLLER BINDINGS");
        else         ImGui::Text("KEYBOARD BINDINGS - PLAYER %d", p + 1);
        ImGui::PopStyleColor(); ImGui::Spacing();

        // Label column width is sized to the WIDEST label this system's spec
        // actually uses (e.g. PSX's "Triangle") instead of a constant tuned
        // for SNES's shorter names ("Select") — otherwise longer per-system
        // vocab overlaps the bind-chip button next to it.
        float label_col_w = px(70.0f);
        for (int b = 0; b < spec.button_count; ++b) {
            float w = ImGui::CalcTextSize(spec.buttons[b].label).x + px(20.0f);
            if (w > label_col_w) label_col_w = w;
        }
        // Narrower chips when two sit side by side; the single-bind width and
        // resulting cell_w stay exactly as before (bpi==1 -> label+170).
        const float chip_w = bpi >= 2 ? px(120.0f) : px(160.0f);
        const float cell_w = label_col_w + bpi * (chip_w + px(6.0f)) + px(4.0f);
        int cols = (int)(ImGui::GetContentRegionAvail().x / cell_w);
        if (cols < 1) cols = 1;
        if (cols > 4) cols = 4;
        // Fixed-width columns, explicitly sized to cell_w: the default
        // stretch policy divides available width evenly across `cols`
        // regardless of our computed cell_w, which reintroduces the very
        // overlap/clip this sizing pass exists to avoid.
        if (ImGui::BeginTable("binds", cols, ImGuiTableFlags_SizingFixedFit)) {
            for (int c = 0; c < cols; ++c)
                ImGui::TableSetupColumn(nullptr, ImGuiTableColumnFlags_WidthFixed, cell_w);
            for (int b = 0; b < spec.button_count; ++b) {
                ImGui::TableNextColumn();
                ImGui::PushID(b);
                ImGui::AlignTextToFramePadding();
                ImGui::TextColored(col(th.text_muted), "%s", spec.buttons[b].label);
                ImGui::SameLine(label_col_w);
                for (int slot = 0; slot < bpi; ++slot) {
                    if (slot) ImGui::SameLine();
                    ImGui::PushID(slot);
                    const bool cap = m->capturing && m->capture_btn == b
                                                  && m->capture_slot == slot;
                    const char* txt = cap
                        ? (pad_cap ? "[ press a key / pad... ]" : "[ press a key... ]")
                        : (slot == 0 ? m->binds[p][b] : m->binds_alt[p][b]);
                    if (cap) ImGui::PushStyleColor(ImGuiCol_Button, col(th.accent));
                    if (ImGui::Button(txt, ImVec2(chip_w, 0)))
                        launcher_model_begin_capture_slot(m, b, slot);
                    if (cap) ImGui::PopStyleColor();
                    ImGui::PopID();
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::Spacing();
        if (ImGui::Button("Reset to Defaults")) launcher_binds_reset_player(m, m->cfg_player + 1);
        if (m->capturing) ImGui::TextColored(col(th.warn), "Listening... (Esc cancels)");
    } end_panel();
}

void panel_controller_config_draw(LauncherModel* m, const LauncherTheme* th) {
    draw_controller_config_view(m, *th);
}

// The CONTROLLER view composes whichever panel(s) this game's SystemProfile
// lists in panels_controller — today always the single "controller_config"
// page (source+deadzone card, then the bindings grid card), matching the
// architecture's "Binds ... page reached from the Controller panel's
// Configure" note.
void draw_controller(LauncherModel* m, const LauncherTheme& th) {
    const SystemProfile* prof = (const SystemProfile*)m->profile;
    const LauncherPanel* p = find_composed(prof->panels_controller, "controller_config", m);
    if (p) p->draw(m, &th);
}

// ---- panel registry: id -> {view, slot, available, draw} --------------------
// The single implementation table for every panel this backend draws. A
// SystemProfile's panels_dashboard/panels_settings/panels_controller arrays
// (launcher_system.h) list which of these ids compose into each view, in
// slot order; draw_dashboard/draw_settings/draw_controller above look each id
// up here via find_composed() and draw whatever's both listed and available().
const LauncherPanel kPanelRegistry[] = {
    { "game",              LNG_VIEW_DASHBOARD,  LNG_SLOT_MAIN, nullptr,      panel_game_draw },
    { "controller",        LNG_VIEW_DASHBOARD,  LNG_SLOT_SIDE, nullptr,      panel_controller_draw },
    { "save",              LNG_VIEW_DASHBOARD,  LNG_SLOT_WIDE, avail_save,   panel_save_draw },
    { "tpak",              LNG_VIEW_DASHBOARD,  LNG_SLOT_WIDE, avail_tpak,   panel_tpak_draw },
    { "video",             LNG_VIEW_SETTINGS,   LNG_SLOT_MAIN, nullptr,      panel_video_draw },
    { "audio",             LNG_VIEW_SETTINGS,   LNG_SLOT_SIDE, nullptr,      panel_audio_draw },
    { "system",            LNG_VIEW_SETTINGS,   LNG_SLOT_SIDE, avail_system, panel_system_draw },
    { "hotkeys",           LNG_VIEW_SETTINGS,   LNG_SLOT_WIDE, nullptr,      panel_hotkeys_draw },
    { "controller_config", LNG_VIEW_CONTROLLER, LNG_SLOT_WIDE, nullptr,      panel_controller_config_draw },
    { nullptr,              LNG_VIEW_DASHBOARD,  0,             nullptr,      nullptr },   // sentinel
};

// Footer: a fixed-height band with the neon divider pinned to its TOP and the
// CTA vertically centred inside it. Laid out from an explicit origin (not the
// running cursor) so it is pixel-identical on every view and the CTA's glow
// always has clearance below the divider — Settings has less body content, and
// a cursor-relative footer let the glow ride up into the rule.
void draw_footer(LauncherModel* m, const LauncherTheme& th, float footer_h) {
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float  fullw  = ImGui::GetContentRegionAvail().x;
    const float  play_w = px(210), play_h = px(46);

    // divider at the very top of the band
    ImGui::GetWindowDrawList()->AddRectFilledMultiColor(
        origin, ImVec2(origin.x + fullw, origin.y + px(1.5f)),
        imcol(th.border, 0.2f), imcol(th.accent, 0.7f),
        imcol(th.accent, 0.7f), imcol(th.border, 0.2f));

    // CTA centred in the remaining band height (glow clears the rule on both sides)
    const float band_y = origin.y + px(1.5f);
    const float band_h = footer_h - px(1.5f);
    const float cta_y  = band_y + (band_h - play_h) * 0.5f;

    const ImVec2 win = ImGui::GetWindowPos();
    if (m->view == LNG_VIEW_DASHBOARD) {
        bool skip = m->s.skip_launcher != 0;
        ImGui::SetCursorScreenPos(ImVec2(origin.x, cta_y + (play_h - ImGui::GetFrameHeight()) * 0.5f));
        if (ImGui::Checkbox("Skip launcher on boot", &skip))
            launcher_model_request_skip_toggle(m);
    }
    // Back/Cancel (Circle/O on a DualSense, B on Xbox, Backspace on keyboard) AND
    // Start re-home the focus ring to PLAY. Directional nav through the card child
    // windows is easy to wander out of with no way back; these are the forcing
    // functions that snap the highlight back to the primary action. Start
    // deliberately only HIGHLIGHTS PLAY (does not launch), so it can't fire the
    // game by accident — the launch is the activate button (A/Cross) on the
    // focused PLAY, or a mouse click. SetKeyboardFocusHere() targets the NEXT
    // submitted item — PLAY's button.
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) ||
        ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false) ||
        ImGui::IsKeyPressed(ImGuiKey_Backspace, false))
        ImGui::SetKeyboardFocusHere();
    ImGui::SetCursorScreenPos(ImVec2(origin.x + fullw - play_w, cta_y));
    if (neon_cta("##play", "PLAY", ImVec2(play_w, play_h)))
        m->action = LNG_ACTION_LAUNCH;
    ImGui::SetItemDefaultFocus();   // gamepad/keyboard start on the primary action
    (void)win;
}

void draw_skip_modal(LauncherModel* m) {
    if (m->skip_modal_open) ImGui::OpenPopup("Skip the launcher on boot?");
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Skip the launcher on boot?", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("The launcher will no longer appear - the game boots straight in. "
                           "Run with \"--launcher\" or set \"SkipLauncher = 0\" in config.ini "
                           "to bring it back.");
        ImGui::Spacing();
        if (ImGui::Button("Cancel", ImVec2(px(120), 0))) {
            launcher_model_skip_cancel(m); ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Skip on Boot", ImVec2(px(140), 0))) {
            launcher_model_skip_confirm(m); ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void draw_ui(LauncherModel* m, const LauncherTheme& th, int logical_w, int logical_h) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    // CRT ground + scanlines behind everything.
    draw_crt_background(vp->Pos, vp->Size);

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));   // let CRT show
    ImGui::Begin("##launcher", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleColor();

    // ---- Marquee header: brand · GAME TITLE · subtitle .......... [nav] ----
    ImVec2 hp = ImGui::GetCursorScreenPos();
    // Vertically center the brand mark against the two-line title block (drop it
    // down ~9px so it doesn't sit high against the first line).
    float hdr_top = ImGui::GetCursorPosY();
    ImGui::SetCursorPosY(hdr_top + px(9));
    image_fit(g_brand, 44, 33); ImGui::SameLine(0, px(12));
    ImGui::SetCursorPosY(hdr_top);
    ImGui::BeginGroup();
        ImGui::SetWindowFontScale(1.55f);
        ImGui::TextUnformatted(m->game_name);
        ImGui::SetWindowFontScale(1.0f);
        // Platform lockup: the wordmark image when a host supplied one
        // (SystemProfile.wordmark_image), else the plain platform text.
        if (g_wordmark.id && g_wordmark.w > 0) {
            ImGui::Dummy(ImVec2(0, px(2)));
            image_fit(g_wordmark, 240, 20);
        } else if (m->platform && m->platform[0]) {
            ImGui::PushStyleColor(ImGuiCol_Text, col(th.text_muted));
            ImGui::TextUnformatted(m->platform);
            ImGui::PopStyleColor();
        }
    ImGui::EndGroup();
    {   // right-aligned nav button
        const char* label = (m->view == LNG_VIEW_DASHBOARD) ? "Settings" : "< Back";
        const float w = px(110.0f);
        ImGui::SameLine(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - w);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + px(6.0f));
        if (ImGui::Button(label, ImVec2(w, px(34)))) {
            launcher_model_set_view(m, m->view == LNG_VIEW_DASHBOARD
                                        ? LNG_VIEW_SETTINGS : LNG_VIEW_DASHBOARD);
        }
    }
    // marquee underline: neon gradient rule under the header
    ImGui::Dummy(ImVec2(0, px(8.0f)));
    {
        ImVec2 u = ImGui::GetCursorScreenPos();
        float fw = ImGui::GetContentRegionAvail().x;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilledMultiColor(u, ImVec2(u.x + fw, u.y + px(2.0f)),
            imcol(th.accent, 0.9f), imcol(th.accent, 0.15f),
            imcol(th.accent, 0.15f), imcol(th.accent, 0.9f));
        glow_rect(dl, u, ImVec2(u.x + fw*0.5f, u.y + px(2.0f)), 0, th.accent, 0.5f, 3);
    }
    ImGui::Dummy(ImVec2(0, px(12.0f)));
    (void)hp;

    // Body: fixed-height child that scrolls when content overflows, so nothing
    // is ever clipped out of reach. The footer below stays fixed (in the fold).
    const float footer_h = px(92.0f);   // divider + clearance + CTA + its glow
    float body_h = ImGui::GetContentRegionAvail().y - footer_h;
    if (body_h < px(80.0f)) body_h = px(80.0f);
    begin_container("body", ImVec2(0, body_h));
    switch (m->view) {
        case LNG_VIEW_DASHBOARD:  draw_dashboard(m, th, logical_w); break;
        case LNG_VIEW_SETTINGS:   draw_settings(m, th);             break;
        case LNG_VIEW_CONTROLLER: draw_controller(m, th);           break;
    }
    end_container();

    draw_footer(m, th, footer_h);
    draw_skip_modal(m);
    // Transfer Pak config modal (N64): opened by any tile, dashboard or
    // Controller page. Drawn at root so it isn't clipped by a card child.
    if (m->tpak_slots > 0) draw_tpak_modal(m, th);
    ImGui::End();
    (void)logical_h;
}

bool is_modifier_scancode(SDL_Scancode sc) {
    return sc == SDL_SCANCODE_LCTRL || sc == SDL_SCANCODE_RCTRL ||
           sc == SDL_SCANCODE_LALT  || sc == SDL_SCANCODE_RALT  ||
           sc == SDL_SCANCODE_LSHIFT|| sc == SDL_SCANCODE_RSHIFT ||
           sc == SDL_SCANCODE_LGUI  || sc == SDL_SCANCODE_RGUI;
}

// Keyboard capture for the rebind editors. Player buttons persist a SCANCODE to
// keybinds.ini; system hotkeys persist a KEYCODE+mods to config.ini [KeyMap].
#if !defined(LNG_SDL3)
// SDL2 only: is this raw joystick button/axis already part of the pad's
// SDL_GameController mapping? Raw capture is reserved for inputs the mapping
// can't express (PSR issue #15: 8BitDo 64 C-buttons) — prefer the clean gamepad
// event otherwise. Ported from PSR input_bindings.cpp raw_input_is_mapped().
static bool raw_input_is_mapped(SDL_JoystickID which, bool is_axis, int raw_index) {
    SDL_GameController* gc = SDL_GameControllerFromInstanceID(which);
    if (!gc) return false;
    auto hit = [&](SDL_GameControllerButtonBind b) {
        if (!is_axis && b.bindType == SDL_CONTROLLER_BINDTYPE_BUTTON) return b.value.button == raw_index;
        if ( is_axis && b.bindType == SDL_CONTROLLER_BINDTYPE_AXIS)   return b.value.axis   == raw_index;
        return false;
    };
    for (int i = 0; i < SDL_CONTROLLER_BUTTON_MAX; ++i)
        if (hit(SDL_GameControllerGetBindForButton(gc, (SDL_GameControllerButton)i))) return true;
    for (int i = 0; i < SDL_CONTROLLER_AXIS_MAX; ++i)
        if (hit(SDL_GameControllerGetBindForAxis(gc, (SDL_GameControllerAxis)i))) return true;
    return false;
}
#endif

bool try_capture(LauncherModel* m, const SDL_Event& ev) {
    if (!m->capturing && !m->hk_capturing) return false;

    // ESC cancels any capture — keyboard, pad, or hotkey.
    if (ev.type == SDL_EVENT_KEY_DOWN && LNG_EVKEY(ev) == SDLK_ESCAPE) {
        launcher_model_cancel_capture(m);
        launcher_model_cancel_hk_capture(m);
        return true;
    }

    // N64 pad capture: when the player being configured has a gamepad source,
    // the input.cfg store captures pad fields, not keys. Listen for pad
    // buttons / decisive axis throws / (SDL2) raw joystick fields; swallow the
    // keyboard entirely so a stray key can't land in a controller bind.
    if (m->capturing && launcher_binds_wants_pad_capture(m, m->cfg_player + 1)) {
        const int pl = m->cfg_player + 1, b = m->capture_btn, slot = m->capture_slot;
        constexpr int kScanThreshold = 20000;   // decisive throw; ignores resting drift
        if (ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
            launcher_binds_set_field(m, pl, b, slot, RUI_N64_FIELD_PAD_BUTTON, (int)LNG_EVGBTN(ev));
            launcher_model_cancel_capture(m);
        } else if (ev.type == SDL_EVENT_GAMEPAD_AXIS_MOTION) {
            const int v = (int)LNG_EVGAXISVAL(ev);
            if (v > kScanThreshold) {
                launcher_binds_set_field(m, pl, b, slot, RUI_N64_FIELD_PAD_AXIS_P, (int)LNG_EVGAXIS(ev));
                launcher_model_cancel_capture(m);
            } else if (v < -kScanThreshold) {
                launcher_binds_set_field(m, pl, b, slot, RUI_N64_FIELD_PAD_AXIS_N, (int)LNG_EVGAXIS(ev));
                launcher_model_cancel_capture(m);
            }
        }
#if !defined(LNG_SDL3)
        else if (ev.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN) {
            if (!raw_input_is_mapped(LNG_EVJBTNWHICH(ev), false, (int)LNG_EVJBTN(ev))) {
                launcher_binds_set_field(m, pl, b, slot, RUI_N64_FIELD_JOY_BUTTON, (int)LNG_EVJBTN(ev));
                launcher_model_cancel_capture(m);
            }
        } else if (ev.type == SDL_EVENT_JOYSTICK_AXIS_MOTION) {
            const int v = (int)LNG_EVJAXISVAL(ev);
            if ((v > kScanThreshold || v < -kScanThreshold) &&
                !raw_input_is_mapped(LNG_EVJAXISWHICH(ev), true, (int)LNG_EVJAXIS(ev))) {
                launcher_binds_set_field(m, pl, b, slot,
                    v > 0 ? RUI_N64_FIELD_JOY_AXIS_P : RUI_N64_FIELD_JOY_AXIS_N, (int)LNG_EVJAXIS(ev));
                launcher_model_cancel_capture(m);
            }
        }
#endif
        return true;   // swallow all other input (keyboard included) while pad-capturing
    }

    if (ev.type != SDL_EVENT_KEY_DOWN) return true;   // swallow non-key input while capturing
    if (m->capturing) {
        // N64's input.cfg keeps two alternate binds per input, so a keyboard
        // capture there must honour capture_slot via the slot-aware field API.
        // Single-bind stores (SNES/PSX/GBA) use the legacy scancode setter
        // (capture_slot is always 0 for them).
        const SystemProfile* prof = (const SystemProfile*)m->profile;
        if (prof && prof->controller.binds_per_input >= 2)
            launcher_binds_set_field(m, m->cfg_player + 1, m->capture_btn, m->capture_slot,
                                     RUI_N64_FIELD_KEY, (int)LNG_EVSCAN(ev));
        else
            launcher_binds_set_button(m, m->cfg_player + 1, m->capture_btn, (int)LNG_EVSCAN(ev));
        launcher_model_cancel_capture(m);
        return true;
    }
    // hotkey capture: wait past a bare modifier press for the real key
    if (is_modifier_scancode((SDL_Scancode)LNG_EVSCAN(ev))) return true;
    launcher_binds_set_hotkey(m, m->capture_hk, (int)LNG_EVKEY(ev), (int)LNG_EVMOD(ev));
    launcher_model_cancel_hk_capture(m);
    return true;
}

std::string asset(const char* rel) {
    const char* base = SDL_GetBasePath();
    return std::string(base ? base : "") + rel;
}

} // namespace

// ---- launcher_panels.h contract implementation -------------------------------
// The panel registry is defined above (kPanelRegistry, anonymous namespace) —
// only this backend's draw functions can populate it (they call ImGui::*), so
// this is the sole implementation, matching today's single-backend reality.
extern "C" const LauncherPanel* launcher_panels_all(void) {
    return kPanelRegistry;   // {id=NULL}-sentinel-terminated
}

extern "C" const LauncherPanel* launcher_panel_find(const char* id) {
    if (!id) return nullptr;
    for (int i = 0; kPanelRegistry[i].id; ++i)
        if (strcmp(kPanelRegistry[i].id, id) == 0) return &kPanelRegistry[i];
    return nullptr;
}

extern "C" bool launcher_panel_available(const LauncherPanel* p, const LauncherModel* m) {
    if (!p) return false;
    return p->available ? (p->available(m) != 0) : true;
}

extern "C" LngAction launcher_backend_run(LauncherPlatform* p,
                                          LauncherModel* m,
                                          const LauncherTheme* th) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.IniFilename = nullptr;
    // Test hook: force the focus ring always-on so scripted screenshots can
    // verify nav rendering without a physical pad. Off => normal auto behaviour
    // (ring appears on pad/keyboard, hides on mouse).
    if (const char* nv = SDL_getenv("LNG_NAV_ALWAYS"); nv && nv[0] == '1')
        io.ConfigNavCursorVisibleAlways = true;

    g_th = th;
    LNG_ImplSDL_InitForOpenGL(p->window, p->gl);
    ImGui_ImplOpenGL3_Init("#version 330");

    // Box art: per-game path from the ABI when given (multi-variant repos
    // stage one file per variant in a shared build dir), else the default.
    g_boxart = launcher_texture_load(
        asset(m->boxart_path && m->boxart_path[0] ? m->boxart_path
                                                  : "assets/img/boxart.tga").c_str());
    // Controller art comes from the active SystemProfile's ControllerSpec —
    // never hardcoded console filenames in this common backend. Conventions:
    // a 32-bit TGA carries real alpha -> the plain alpha-respecting loader;
    // a 24-bit TGA has a flat backdrop baked in -> keyed out (top-left pixel)
    // so the art sits transparently on the panel. `image_analog`/
    // `image_digital` (the optional PSX-style mode-swap pair) are 32-bit.
    {
        const SystemProfile* prof = (const SystemProfile*)m->profile;
        const char* pad_img = (prof && prof->controller.image)
                                ? prof->controller.image : "pad.tga";
        std::string pad_path = asset((std::string("assets/img/") + pad_img).c_str());
        // TGA header byte 16 = bits per pixel: pick the loader by depth.
        int bpp = 0;
        if (FILE* f = fopen(pad_path.c_str(), "rb")) {
            unsigned char hdr[18];
            if (fread(hdr, 1, sizeof(hdr), f) == sizeof(hdr)) bpp = hdr[16];
            fclose(f);
        }
        g_pad = (bpp == 32)
            ? launcher_texture_load(pad_path.c_str())
            : launcher_texture_load_colorkey(pad_path.c_str(), 24);
        if (prof && prof->controller.image_analog)
            g_pad_analog = launcher_texture_load(
                asset((std::string("assets/img/") + prof->controller.image_analog).c_str()).c_str());
        if (prof && prof->controller.image_digital)
            g_pad_digital = launcher_texture_load(
                asset((std::string("assets/img/") + prof->controller.image_digital).c_str()).c_str());
    }
    // Header brand mark: the active SystemProfile's own (N64 -> the four-color
    // logo), falling back to the shared recomp-ui mark for consoles that don't
    // set one. Keeps the top-left mark matched to the system on screen.
    {
        const SystemProfile* bprof = (const SystemProfile*)m->profile;
        const char* brand_file = (bprof && bprof->brand_image)
                                   ? bprof->brand_image : "brand_mark.tga";
        g_brand = launcher_texture_load(
            asset((std::string("assets/img/") + brand_file).c_str()).c_str());
        // Optional platform wordmark: loads only if the profile names one AND
        // the file is present (recomp-ui ships none — a console wordmark may be
        // a third-party trademark). Missing file => header falls back to text.
        if (bprof && bprof->wordmark_image && bprof->wordmark_image[0])
            g_wordmark = launcher_texture_load(
                asset((std::string("assets/img/") + bprof->wordmark_image).c_str()).c_str());
    }
    // Transfer Pak cartridge art (only a tpak game needs it): real GB cart PNGs
    // keyed by cart_kind, empty shell at index 0 (see g_cart / draw_tpak_cart).
    if (m->tpak_slots > 0) {
        static const char* const kCartFiles[5] = {
            "cart_empty.tga", "cart_red.tga", "cart_blue.tga",
            "cart_yellow.tga", "cart_green.tga",
        };
        for (int i = 0; i < 5; ++i)
            g_cart[i] = launcher_texture_load(
                asset((std::string("assets/img/") + kCartFiles[i]).c_str()).c_str());
    }
    g_verdict_ok    = launcher_texture_load(asset("assets/img/verdict_ok.tga").c_str());
    g_verdict_warn  = launcher_texture_load(asset("assets/img/verdict_warn.tga").c_str());
    g_verdict_bad   = launcher_texture_load(asset("assets/img/verdict_bad.tga").c_str());
    g_verdict_none  = launcher_texture_load(asset("assets/img/verdict_none.tga").c_str());
    // memcard.tga is already 32-bit with real alpha (no colorkey backdrop),
    // same as pad_analog.tga/pad_digital.tga above.
    g_memcard = launcher_texture_load(asset("assets/img/memcard.tga").c_str());

    std::string font_path = asset("assets/fonts/LatoLatin-Regular.ttf");
    // Optional Japanese subset, merged over the Latin base when present (PMS-J).
    // Games that don't ship it stay Latin-only (fopen in apply_scale fails
    // silently), so this path is inert for every other console.
    std::string jp_font_path = asset("assets/fonts/NotoSansJP-Subset.ttf");
    float applied_scale = 0.0f;
    launcher_debug_init();

    long smoke_frames = 0, frame = 0;
    if (const char* sf = SDL_getenv("LNG_SMOKE_FRAMES")) smoke_frames = SDL_atoi(sf);

    // Test hook: LNG_FORCE_SCALE simulates a HiDPI display (see the platform
    // layer, which enlarges the window and reports a logical/pixel split). When
    // active, feed that split to ImGui so it renders at pixel density over a
    // logical-sized layout — validating the DPI-independent layout on any OS.
    // Unset => stock ImGui behavior (the SDL/GL backend's own framebuffer scale).
    const char* force_scale_env = SDL_getenv("LNG_FORCE_SCALE");
    const bool force_dpi = force_scale_env && force_scale_env[0] && SDL_atof(force_scale_env) > 1.0;

    while (m->action == LNG_ACTION_NONE && !p->should_quit) {
        if (smoke_frames > 0 && ++frame > smoke_frames) { m->action = LNG_ACTION_QUIT; break; }

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) p->should_quit = true;
            if (ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) p->should_quit = true;
            if (try_capture(m, ev)) continue;
            LNG_ImplSDL_ProcessEvent(&ev);
        }

        launcher_platform_refresh_metrics(p);
        if (applied_scale != p->display_scale) {
            apply_scale(*th, p->display_scale, font_path.c_str(), jp_font_path.c_str());
            applied_scale = p->display_scale;
        }

        // Re-poll connected gamepads every frame so hot-plugged pads (e.g. a
        // DualSense powered on after launch) appear without a relaunch.
        g_pad_count = launcher_input_poll(g_pads, LNG_MAX_PADS);

        ImGui_ImplOpenGL3_NewFrame();
        LNG_ImplSDL_NewFrame();
        if (force_dpi) {   // Windows has no native point/pixel split — inject it
            ImGuiIO& io = ImGui::GetIO();
            io.DisplaySize = ImVec2((float)p->logical_w, (float)p->logical_h);
            io.DisplayFramebufferScale = ImVec2(p->display_scale, p->display_scale);
        }
        ImGui::NewFrame();
        draw_ui(m, *th, p->logical_w, p->logical_h);
        ImGui::Render();

        glViewport(0, 0, p->pixel_w, p->pixel_h);
        const LngColor bg = th->background;
        glClearColor(bg.r, bg.g, bg.b, bg.a);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        launcher_debug_step(p, m);   // script/screenshot: after draw, before swap
        launcher_platform_present(p);
    }

    launcher_texture_free(&g_boxart);
    launcher_texture_free(&g_pad);
    launcher_texture_free(&g_pad_analog);
    launcher_texture_free(&g_pad_digital);
    launcher_texture_free(&g_brand);
    launcher_texture_free(&g_verdict_ok);
    launcher_texture_free(&g_verdict_warn);
    launcher_texture_free(&g_verdict_bad);
    launcher_texture_free(&g_verdict_none);
    launcher_texture_free(&g_memcard);
    launcher_texture_free(&g_wordmark);
    for (int i = 0; i < 5; ++i) launcher_texture_free(&g_cart[i]);
    ImGui_ImplOpenGL3_Shutdown();
    LNG_ImplSDL_Shutdown();
    ImGui::DestroyContext();

    if (p->should_quit && m->action == LNG_ACTION_NONE) m->action = LNG_ACTION_QUIT;
    return m->action;
}

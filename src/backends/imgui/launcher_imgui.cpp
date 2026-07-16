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

float  g_scale = 1.0f;
float  px(float logical) { return logical * g_scale; }
ImVec4 col(const LngColor& c) { return ImVec4(c.r, c.g, c.b, c.a); }
const LauncherTheme* g_th = nullptr;

LauncherTexture g_boxart, g_pad, g_pad_analog, g_pad_digital, g_brand;
ImTextureID tid(const LauncherTexture& t) { return (ImTextureID)(intptr_t)t.id; }

LauncherPad g_pads[LNG_MAX_PADS];   // live gamepad list (repolled every frame)
int         g_pad_count = 0;

char        g_pick_buf[512] = {};    // ROM picker result

// Context flag the dashboard composer sets just before invoking the "game"
// panel's registered draw() — the LauncherPanelDrawFn signature (Model*,
// const Theme*) has no room for the layout-context fill_h flag that
// draw_game_panel needs (fill the column height in the wide 2-column
// dashboard vs hug its content in the narrow stacked layout). Same pattern as
// the other per-frame context globals below (g_scale, g_th, g_pads).
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
void apply_scale(const LauncherTheme& th, float scale, const char* font_path) {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    ImFontConfig cfg; cfg.OversampleH = 2; cfg.OversampleV = 2;
    const float body = th.font_body * scale;
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
    style.ScaleAllSizes(scale);
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
void row_label(const char* text, const LauncherTheme& th) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(col(th.text_muted), "%s", text);
    ImGui::SameLine(px(170.0f));
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

void draw_game_panel(LauncherModel* m, const LauncherTheme& th, bool fill_h = false) {
    if (!begin_panel("game", 0, fill_h)) { end_panel(); return; }
    eyebrow("GAME");
    const float availw = ImGui::GetContentRegionAvail().x;

    // Box art on top (centered), everything else BELOW it. Height is derived
    // from the space actually left after the metadata + button, so the art is
    // as large as it can be WITHOUT pushing the last row out of the card.
    {
        // Reserve space for everything under the art: verified line + 2 meta rows
        // + Change ROM, plus the SAVES block when this game has battery SRAM.
        float reserve = px(198.0f);
        if (m->saves_supported) reserve += px(96.0f);    // compact SAVES row below Change ROM
        float art_h = ImGui::GetContentRegionAvail().y - reserve;
        if (art_h > px(280.0f)) art_h = px(280.0f);
        if (art_h < px(132.0f)) art_h = px(132.0f);
        hero_boxart_centered(g_boxart, art_h / g_scale, availw);
    }
    ImGui::Dummy(ImVec2(0, px(10)));

    // Region + verification state, centered under the art.
    const bool verified = launcher_model_rom_verified(m);
    const char* noun = (m->rom_noun && m->rom_noun[0]) ? m->rom_noun : "ROM";
    {
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
        kv_row("Region", m->region[0] ? m->region : "", th, false, false);
        kv_row("File",   m->rom_file, th, false, false);
        ImGui::EndTable();
    }
    ImGui::Dummy(ImVec2(0, px(12)));
    char change_label[32];
    snprintf(change_label, sizeof(change_label), "Change %s", noun);
    if (ImGui::Button(change_label, ImVec2(availw, px(34))))
        if (launcher_pick_rom(g_pick_buf, sizeof(g_pick_buf)))
            launcher_model_set_rom(m, g_pick_buf);

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
    ImGui::PushStyleColor(ImGuiCol_Text, col(th.text_muted));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Save");
    ImGui::PopStyleColor();
    ImGui::SameLine(px(76));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(base[0] ? base : "(none yet)");
    const float bw = px(84);
    ImGui::SameLine(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - bw*2 - px(th.spacing_sm));
    ImGui::Button("Import", ImVec2(bw, px(30)));
    ImGui::SameLine(0, px(th.spacing_sm));
    ImGui::Button("Clear", ImVec2(bw, px(30)));
}

// ---- panel adapters: LauncherPanelDrawFn = void(LauncherModel*, const LauncherTheme*) ----
void panel_game_draw(LauncherModel* m, const LauncherTheme* th) {
    draw_game_panel(m, *th, g_game_fill_h);
}

// Standalone SAVE card (own eyebrow) — not composed by any SystemProfile yet
// (SAVES still folds into GAME per draw_game_panel, matching today's pixel
// layout exactly), registered so the module is a complete, addressable unit
// per the architecture and ready for a future standalone composition.
int avail_save(const LauncherModel* m) { return m->saves_supported; }
void panel_save_draw(LauncherModel* m, const LauncherTheme* th) {
    if (begin_panel("save", 0)) { eyebrow("SAVES"); draw_save_row(m, *th); }
    end_panel();
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

    if (!begin_panel(id, w / g_scale, false)) { end_panel(); return; }
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
        const float aw = px(120);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (inner - aw) * 0.5f);
        image_fit(art, 120, 70);
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
    if (ImGui::Button("Configure", ImVec2(cw, px(32)))) launcher_model_open_config(m, p);
    ImGui::Dummy(ImVec2(0, px(6)));
    // status line, centered
    {
        const bool on = m->s.player_src[p] != 0;
        const char* st = on ? "connected" : "not assigned";
        float sw = px(10) + px(8) + ImGui::CalcTextSize(st).x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (inner - sw) * 0.5f);
        draw_dot(on, th.good, th.text_muted);
        ImGui::TextColored(on ? col(th.good) : col(th.text_muted), "%s", st);
    }
    ImGui::PopID();
    end_panel();
}

// Lays out the player cards: one card for a 1-player game, two side-by-side
// for a 2-player game. Driven by the model, never hardcoded.
void draw_controllers_row(LauncherModel* m, const LauncherTheme& th) {
    if (m->lock_device) return;   // fixed pad: hide the player controller cards entirely
    const int   n   = (m->player_count >= 2) ? 2 : 1;
    const float gap = px(th.spacing_md);
    const float availw = ImGui::GetContentRegionAvail().x;
    // A 2P game splits the row; a 1P game gets ONE card of the same size rather
    // than a full-width card with a lone pad floating in it.
    float cardw = (availw - gap) * 0.5f;
    if (n == 1 && cardw < px(300.0f)) cardw = availw;   // narrow window: fill
    for (int p = 0; p < n; ++p) {
        if (p) ImGui::SameLine(0, gap);
        begin_container(p ? "pc1" : "pc0", ImVec2(cardw, 0), ImGuiChildFlags_AutoResizeY);
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

    if (logical_w >= 820) {
        const float gap = px(th.spacing_md);
        // Art-led left column sized to the box art; side column takes the rest.
        if (game_p) {
            g_game_fill_h = true;
            begin_container("dash_l", ImVec2(px(400), 0));
            game_p->draw(m, &th);
            end_container();
        }

        if (game_p && ctrl_p) ImGui::SameLine(0, gap);
        if (ctrl_p) {
            begin_container("dash_r", ImVec2(0, 0), ImGuiChildFlags_None);
                // One self-contained card per player. SAVES now lives in the GAME
                // card and MSU-1 in Settings > Audio, so the side column is just
                // the controller card(s).
                ctrl_p->draw(m, &th);
            end_container();
        }
    } else {
        if (game_p) { g_game_fill_h = false; game_p->draw(m, &th); }
        if (game_p && ctrl_p) ImGui::Spacing();
        if (ctrl_p) ctrl_p->draw(m, &th);
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

void draw_display_controls(LauncherModel* m, const LauncherTheme& th) {
    eyebrow("DISPLAY");

    if (!any_deep_display(m)) {
        // ---- legacy minimal surface (SNES etc.) — byte-identical to before ----
        row_label("Window scale", th);
        if (ImGui::Button(launcher_model_scale_label(m), ImVec2(px(120), px(30))))
            launcher_model_cycle_scale(m);
        row_label("Linear filtering", th);
        bool filter = m->s.linear_filter != 0;
        if (ImGui::Checkbox("##filter", &filter)) launcher_model_toggle_filter(m);
        if (m->aspect_mask) {   // PSX-style: 4:3/16:9/21:9 cycle (supersedes the legacy checkbox)
            row_label("Aspect ratio", th);
            if (ImGui::Button(launcher_model_aspect_label(m), ImVec2(px(180), px(30))))
                launcher_model_cycle_aspect(m);
        } else if (m->widescreen_supported) {   // legacy module: only for games that support it
            row_label("Widescreen 16:9", th);
            bool ws = m->s.widescreen != 0;
            if (ImGui::Checkbox("##ws", &ws)) launcher_model_toggle_widescreen(m);
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

    if (m->aspect_mask) {
        row_label("Aspect ratio", th);
        if (ImGui::Button(launcher_model_aspect_label(m), ImVec2(px(180), px(30))))
            launcher_model_cycle_aspect(m);
    } else if (m->widescreen_supported) {
        row_label("Widescreen 16:9", th);
        bool ws = m->s.widescreen != 0;
        if (ImGui::Checkbox("##ws", &ws)) launcher_model_toggle_widescreen(m);
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
        bool aa = m->s.antialiasing != 0;
        if (ImGui::Checkbox("##aa", &aa)) launcher_model_toggle_aa(m);
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
    row_label("Sample rate", th);
    if (ImGui::Button(launcher_model_freq_label(m), ImVec2(px(120), px(30))))
        launcher_model_cycle_freq(m);
    row_label("Volume", th);
    int dv = 0; stepper("vol", m->s.volume, "%", &dv);
    if (dv) launcher_model_volume_delta(m, dv);

    if (m->has_spu_hq) {
        row_label("SPU high-quality", th);
        bool hq = m->s.spu_hq != 0;
        if (ImGui::Checkbox("##spuhq", &hq)) launcher_model_toggle_spu_hq(m);
    }

    if (m->has_deadzone_pct) {
        row_label("Analog deadzone", th);
        if (ImGui::Button(launcher_model_deadzone_pct_label(m), ImVec2(px(90), px(30))))
            launcher_model_cycle_deadzone_pct(m);
    }

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
        if (ImGui::Button("Browse", ImVec2(bw, px(28)))) {
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
    const bool deep_audio = m->has_spu_hq || m->has_deadzone_pct || m->num_languages > 0;
    if (deep_audio) {
        if (begin_panel("audio", 0, false)) draw_audio_controls(m, *th);
        end_panel();
    } else {
        if (begin_panel("audio", 0, true, /*no_scroll*/true)) draw_audio_controls(m, *th);
        end_panel();
    }
}

// SYSTEM module: BIOS path picker — a full-width row of its own, composed
// only for systems whose profile lists "system" (PSX) AND only shown for a
// game instance that needs one (has_bios) — composition + availability, both
// layers, matching the architecture.
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
// via SystemProfile.hotkeys_mask (both snes and psx opt into every catalog
// entry today — LNG_HOTKEYS_ALL — so the grid is unchanged; a future system
// with a narrower mask would simply see fewer rows).
void draw_hotkeys_controls(LauncherModel* m, const LauncherTheme& th) {
    eyebrow("HOTKEYS");
    const SystemProfile* prof = (const SystemProfile*)m->profile;
    const uint32_t mask = prof ? prof->hotkeys_mask : LNG_HOTKEYS_ALL;
    // Same responsive grid treatment as the bindings list.
    const float cell_w = px(280.0f);
    int cols = (int)(ImGui::GetContentRegionAvail().x / cell_w);
    cols = cols < 1 ? 1 : (cols > 3 ? 3 : cols);
    if (ImGui::BeginTable("hk", cols)) {
        for (int h = 0; h < LNG_HK_COUNT; ++h) {
            if (!(mask & (1u << h))) continue;
            ImGui::TableNextColumn();
            ImGui::PushID(h);
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(col(th.text_muted), "%-13s", launcher_hotkey_name((LngHotkey)h));
            ImGui::SameLine(px(130));
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
// top band, then any WIDE panels (SYSTEM, HOTKEYS) stack full-width below —
// exactly today's fixed layout, now driven by the composition array + the
// registry's available() gate instead of hardcoded calls.
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
    const bool deep_audio   = m->has_spu_hq || m->has_deadzone_pct || m->num_languages > 0;

    const LauncherPanel* video_p   = find_composed(prof->panels_settings, "video", m);
    const LauncherPanel* audio_p   = find_composed(prof->panels_settings, "audio", m);
    const LauncherPanel* system_p  = find_composed(prof->panels_settings, "system", m);
    const LauncherPanel* hotkeys_p = find_composed(prof->panels_settings, "hotkeys", m);

    if (video_p) {
        if (deep_display) begin_container("set_l", ImVec2(half, 0), ImGuiChildFlags_AutoResizeY);
        else               begin_container("set_l", ImVec2(half, row_h));
        video_p->draw(m, &th);
        end_container();
    }
    if (video_p && audio_p) ImGui::SameLine(0, gap);
    if (audio_p) {
        if (deep_audio) begin_container("set_r", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY);
        else             begin_container("set_r", ImVec2(0, row_h));
        audio_p->draw(m, &th);
        end_container();
    }

    if (system_p)  system_p->draw(m, &th);
    if (hotkeys_p) hotkeys_p->draw(m, &th);
}

// CONTROLLER-view rebind page: input source + deadzone, and the keyboard
// bindings grid — reached from the dashboard CONTROLLER panel's Configure
// button. Per-system base button set today is the same LNG_BTN_* catalog for
// every system (ControllerSpec.buttons in launcher_system.h documents this
// as data; launcher_button_name() stays the single rendered source of truth).
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

    if (begin_panel("cfg_binds", 0)) {
        ImGui::PushStyleColor(ImGuiCol_Text, col(th.accent));
        ImGui::Text("KEYBOARD BINDINGS - PLAYER %d", p + 1); ImGui::PopStyleColor(); ImGui::Spacing();

        // Responsive grid: fit as many label+chip columns as the width allows
        // (1..4) instead of one tall column with dead space to the right.
        const float cell_w = px(270.0f);
        int cols = (int)(ImGui::GetContentRegionAvail().x / cell_w);
        if (cols < 1) cols = 1;
        if (cols > 4) cols = 4;
        if (ImGui::BeginTable("binds", cols)) {
            for (int b = 0; b < LNG_BTN_COUNT; ++b) {
                ImGui::TableNextColumn();
                ImGui::PushID(b);
                ImGui::AlignTextToFramePadding();
                ImGui::TextColored(col(th.text_muted), "%-6s", launcher_button_name((LngButton)b));
                ImGui::SameLine(px(70));
                const bool cap = m->capturing && m->capture_btn == (LngButton)b;
                if (cap) ImGui::PushStyleColor(ImGuiCol_Button, col(th.accent));
                if (ImGui::Button(cap ? "[ press a key... ]" : m->binds[p][b], ImVec2(px(160), 0)))
                    launcher_model_begin_capture(m, (LngButton)b);
                if (cap) ImGui::PopStyleColor();
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
    { "video",             LNG_VIEW_SETTINGS,   LNG_SLOT_MAIN, nullptr,      panel_video_draw },
    { "audio",             LNG_VIEW_SETTINGS,   LNG_SLOT_SIDE, nullptr,      panel_audio_draw },
    { "system",            LNG_VIEW_SETTINGS,   LNG_SLOT_WIDE, avail_system, panel_system_draw },
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
        if (m->platform && m->platform[0]) {
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
bool try_capture(LauncherModel* m, const SDL_Event& ev) {
    if (!m->capturing && !m->hk_capturing) return false;
    if (ev.type != SDL_EVENT_KEY_DOWN) return true;   // swallow input while capturing
    if (LNG_EVKEY(ev) == SDLK_ESCAPE) {
        launcher_model_cancel_capture(m);
        launcher_model_cancel_hk_capture(m);
        return true;
    }
    if (m->capturing) {
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

    g_boxart = launcher_texture_load(asset("assets/img/boxart.tga").c_str());
    // pad.tga is 24-bit (no alpha) with a flat backdrop baked in -> key it
    // out so the pad art sits transparently on the panel.
    g_pad    = launcher_texture_load_colorkey(asset("assets/img/pad.tga").c_str(), 24);
    // pad_analog.tga / pad_digital.tga are already 32-bit with real alpha (no
    // colorkey backdrop) -> the plain alpha-respecting loader, not colorkey.
    g_pad_analog  = launcher_texture_load(asset("assets/img/pad_analog.tga").c_str());
    g_pad_digital = launcher_texture_load(asset("assets/img/pad_digital.tga").c_str());
    g_brand  = launcher_texture_load(asset("assets/img/brand_mark.tga").c_str());

    std::string font_path = asset("assets/fonts/LatoLatin-Regular.ttf");
    float applied_scale = 0.0f;
    launcher_debug_init();

    long smoke_frames = 0, frame = 0;
    if (const char* sf = SDL_getenv("LNG_SMOKE_FRAMES")) smoke_frames = SDL_atoi(sf);

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
        g_scale = p->display_scale;
        if (applied_scale != p->display_scale) {
            apply_scale(*th, p->display_scale, font_path.c_str());
            applied_scale = p->display_scale;
        }

        // Re-poll connected gamepads every frame so hot-plugged pads (e.g. a
        // DualSense powered on after launch) appear without a relaunch.
        g_pad_count = launcher_input_poll(g_pads, LNG_MAX_PADS);

        ImGui_ImplOpenGL3_NewFrame();
        LNG_ImplSDL_NewFrame();
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
    ImGui_ImplOpenGL3_Shutdown();
    LNG_ImplSDL_Shutdown();
    ImGui::DestroyContext();

    if (p->should_quit && m->action == LNG_ACTION_NONE) m->action = LNG_ACTION_QUIT;
    return m->action;
}

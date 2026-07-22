// Modern in-game presentation for the renderer-independent runtime UI model.
// The host owns the ImGui context/frame/submission. This file only draws.

#include "recomp_runtime_ui_internal.h"
#include "launcher_theme.h"

#include "imgui.h"

#include <cstdio>

namespace {

ImVec4 col(const LngColor &c, float alpha = 1.0f) {
    return ImVec4(c.r, c.g, c.b, c.a * alpha);
}

ImU32 u32(const LngColor &c, float alpha = 1.0f) {
    return ImGui::ColorConvertFloat4ToU32(col(c, alpha));
}

void value_text(RecompRuntimeUi *ui, const RecompRuntimeUiItem *item,
                char *out, size_t out_size) {
    if (item->type == RECOMP_RUNTIME_UI_ACTION) {
        std::snprintf(out, out_size, "OPEN");
        return;
    }
    int value = 0;
    if (!recomp_runtime_ui_current_value(ui, item, &value)) {
        std::snprintf(out, out_size, "Unavailable");
    } else if (item->type == RECOMP_RUNTIME_UI_BOOL) {
        std::snprintf(out, out_size, "%s", value ? "On" : "Off");
    } else if (item->type == RECOMP_RUNTIME_UI_CHOICE && item->choices &&
               value >= item->minimum &&
               static_cast<size_t>(value - item->minimum) < item->choice_count) {
        std::snprintf(out, out_size, "%s", item->choices[value - item->minimum]);
    } else {
        std::snprintf(out, out_size, "%d", value);
    }
}

void draw_sections(RecompRuntimeUi *ui, const LauncherTheme &theme,
                   bool enter_on_click) {
    for (size_t index = 0; index < ui->section_count; ++index) {
        ImGui::PushID(static_cast<int>(index));
        const bool selected = index == ui->section_index;
        if (ImGui::Selectable(ui->sections[index], selected, 0,
                              ImVec2(0.0f, theme.row_height))) {
            ui->section_index = index;
            if (enter_on_click) recomp_runtime_ui_enter_section(ui, index);
        }
        if (ImGui::IsItemHovered()) ui->section_index = index;
        ImGui::PopID();
    }
}

void draw_items(RecompRuntimeUi *ui, const LauncherTheme &theme) {
    const size_t count = recomp_runtime_ui_section_item_count(
        ui, ui->section_index);
    for (size_t index = 0; index < count; ++index) {
        const RecompRuntimeUiItem *item = recomp_runtime_ui_section_item(
            ui, ui->section_index, index);
        if (!item) continue;

        const bool enabled = recomp_runtime_ui_item_enabled(ui, item) != 0;
        const bool selected = ui->in_section && index == ui->row_index;
        char value[128];
        value_text(ui, item, value, sizeof(value));

        ImGui::PushID(static_cast<int>(index));
        if (!enabled) ImGui::BeginDisabled();
        const ImVec2 start = ImGui::GetCursorScreenPos();
        const float row_h = item->description && item->description[0]
                                ? theme.row_height + theme.spacing_sm
                                : theme.row_height;
        if (ImGui::Selectable("##setting", selected, 0, ImVec2(0.0f, row_h))) {
            ui->row_index = index;
            ui->in_section = 1;
            recomp_runtime_ui_adjust_current(ui, 1, 1, 0);
        }
        if (ImGui::IsItemHovered()) {
            ui->row_index = index;
            ui->in_section = 1;
        }

        ImDrawList *draw = ImGui::GetWindowDrawList();
        const ImVec2 end = ImGui::GetItemRectMax();
        const ImU32 label_color = u32(enabled ? theme.text : theme.text_muted,
                                      enabled ? 1.0f : 0.65f);
        draw->AddText(ImVec2(start.x + theme.spacing_md,
                            start.y + theme.spacing_sm),
                      label_color, item->label ? item->label : "");
        const ImVec2 value_size = ImGui::CalcTextSize(value);
        draw->AddText(ImVec2(end.x - theme.spacing_md - value_size.x,
                            start.y + theme.spacing_sm),
                      u32(selected && enabled ? theme.accent2 : theme.text_muted),
                      value);
        if (item->description && item->description[0]) {
            draw->AddText(ImVec2(start.x + theme.spacing_md,
                                start.y + theme.spacing_sm +
                                    ImGui::GetTextLineHeight() + 2.0f),
                          u32(theme.text_muted), item->description);
        }
        if (!enabled) ImGui::EndDisabled();
        ImGui::PopID();
    }
}

void push_runtime_style(const LauncherTheme &theme) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, theme.radius_lg);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, theme.radius_sm);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, theme.radius_sm);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2(theme.spacing_lg, theme.spacing_lg));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                        ImVec2(theme.spacing_sm, theme.spacing_sm));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, col(theme.background, 0.98f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col(theme.panel, 0.96f));
    ImGui::PushStyleColor(ImGuiCol_Border, col(theme.border));
    ImGui::PushStyleColor(ImGuiCol_Text, col(theme.text));
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, col(theme.text_muted));
    ImGui::PushStyleColor(ImGuiCol_Header, col(theme.control_hovered));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, col(theme.panel_hovered));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, col(theme.accent_dim));
    ImGui::PushStyleColor(ImGuiCol_Button, col(theme.control));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, col(theme.control_hovered));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, col(theme.accent_dim));
}

void pop_runtime_style() {
    ImGui::PopStyleColor(11);
    ImGui::PopStyleVar(5);
}

} // namespace

extern "C" void recomp_runtime_ui_render_imgui(RecompRuntimeUi *ui) {
    if (!ui || !ui->open || ImGui::GetCurrentContext() == nullptr) return;

    const LauncherTheme theme = launcher_theme_by_name(ui->config.theme);
    ImGuiIO &io = ImGui::GetIO();
    const ImVec2 display = io.DisplaySize;
    if (display.x <= 0.0f || display.y <= 0.0f) return;

    ImGui::GetBackgroundDrawList()->AddRectFilled(
        ImVec2(0.0f, 0.0f), display, IM_COL32(0, 0, 0, 150));

    const float margin = 24.0f;
    const float width = display.x - margin * 2.0f < 780.0f
                            ? display.x - margin * 2.0f
                            : 780.0f;
    const float height = display.y - margin * 2.0f < 680.0f
                             ? display.y - margin * 2.0f
                             : 680.0f;
    const bool wide = width >= 640.0f;
    ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_Always);

    push_runtime_style(theme);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
                                   ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("##recomp-runtime-ui", nullptr, flags)) {
        ImGui::PushStyleColor(ImGuiCol_Text, col(theme.accent2));
        ImGui::TextUnformatted(ui->config.title ? ui->config.title : "Settings");
        ImGui::PopStyleColor();
        if (ui->config.subtitle && ui->config.subtitle[0]) {
            ImGui::SameLine();
            const float subtitle_w = ImGui::CalcTextSize(ui->config.subtitle).x;
            ImGui::SetCursorPosX(ImGui::GetWindowWidth() -
                                 ImGui::GetStyle().WindowPadding.x - subtitle_w);
            ImGui::TextDisabled("%s", ui->config.subtitle);
        }
        ImGui::Separator();

        const float footer_h = ImGui::GetTextLineHeightWithSpacing() +
                               theme.spacing_lg + 2.0f;
        const float content_h = ImGui::GetContentRegionAvail().y - footer_h;
        if (wide) {
            const float sidebar_w = 190.0f;
            ImGui::BeginChild("##sections", ImVec2(sidebar_w, content_h), true);
            draw_sections(ui, theme, false);
            ImGui::EndChild();
            ImGui::SameLine();
            ImGui::BeginChild("##items", ImVec2(0.0f, content_h), true);
            ImGui::PushStyleColor(ImGuiCol_Text, col(theme.accent2));
            ImGui::TextUnformatted(ui->sections[ui->section_index]);
            ImGui::PopStyleColor();
            ImGui::Separator();
            draw_items(ui, theme);
            ImGui::EndChild();
        } else {
            ImGui::BeginChild("##content", ImVec2(0.0f, content_h), true);
            if (ui->in_section) {
                if (ImGui::SmallButton("< Back"))
                    recomp_runtime_ui_leave_section(ui);
                ImGui::SameLine();
                ImGui::TextUnformatted(ui->sections[ui->section_index]);
                ImGui::Separator();
                draw_items(ui, theme);
            } else {
                draw_sections(ui, theme, true);
            }
            ImGui::EndChild();
        }

        ImGui::Separator();
        const char *accept = ui->config.accept_label
                                 ? ui->config.accept_label : "Select";
        const char *back = ui->config.back_label
                               ? ui->config.back_label : "Back";
        ImGui::TextDisabled("%s  Select    %s  Back    D-pad / Arrows  Navigate",
                            accept, back);
        if (ui->status_frames) {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, col(theme.accent2));
            ImGui::TextUnformatted(ui->status);
            ImGui::PopStyleColor();
            --ui->status_frames;
        }
        ImGui::SameLine();
        const char *close_label = "Resume";
        const float close_w = ImGui::CalcTextSize(close_label).x +
                              theme.spacing_lg * 2.0f;
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() -
                             ImGui::GetStyle().WindowPadding.x - close_w);
        if (ImGui::Button(close_label, ImVec2(close_w, 0.0f)))
            recomp_runtime_ui_close(ui);
    }
    ImGui::End();
    pop_runtime_style();
}

#include "recomp_runtime_ui.h"
#include "imgui.h"

#include <cassert>
#include <cstring>

namespace {

int get_value(void *context, const RecompRuntimeUiItem *, int *out) {
    *out = *static_cast<int *>(context);
    return 1;
}

int set_value(void *context, const RecompRuntimeUiItem *, int value) {
    *static_cast<int *>(context) = value;
    return 1;
}

} // namespace

int main() {
    static const char *const modes[] = { "Standard (4:3)", "16:9", "Adaptive" };
    static const RecompRuntimeUiItem items[] = {
        { "view", "Display", "View mode", "Choose the visible game area.",
          RECOMP_RUNTIME_UI_CHOICE, 0, 2, 1, modes, 3, nullptr },
    };
    int value = 0;
    RecompRuntimeUiConfig config{};
    config.title = "Runtime UI";
    config.subtitle = "NINTENDO 64";
    config.items = items;
    config.item_count = 1;
    config.theme = "n64";
    config.callbacks.context = &value;
    config.callbacks.get_value = get_value;
    config.callbacks.set_value = set_value;

    RecompRuntimeUi *ui = recomp_runtime_ui_create(&config);
    assert(ui != nullptr);
    recomp_runtime_ui_open(ui);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1280.0f, 720.0f);
    io.DeltaTime = 1.0f / 60.0f;
    unsigned char *font_pixels = nullptr;
    int font_w = 0, font_h = 0;
    io.Fonts->GetTexDataAsRGBA32(&font_pixels, &font_w, &font_h);

    ImGui::NewFrame();
    recomp_runtime_ui_render_imgui(ui);
    ImGui::Render();
    assert(ImGui::GetDrawData()->CmdListsCount > 0);

    ImGui::DestroyContext();
    recomp_runtime_ui_destroy(ui);
    return 0;
}

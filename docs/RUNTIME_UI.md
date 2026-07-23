# Runtime UI architecture

The in-game menu is cross-platform at the model and design-system layers. It
does not assume a console, emulator core, game configuration structure, window
library, or GPU API.

## Ownership

`recomp-ui` owns:

- item/section descriptors and callbacks;
- navigation, selection, enabled state, status, and persistence requests;
- the shared `LauncherTheme` design tokens;
- a modern active-context ImGui presentation;
- a compact ARGB8888 framebuffer fallback.

The game host owns:

- translating SDL/native events into semantic menu inputs;
- applying settings to its live renderer, audio, and emulated machine;
- config-file format and persistence;
- pause/resume policy and input suppression;
- beginning/submitting the renderer frame.

That boundary is necessary: recomp-ui cannot safely pause an N64 recomp, reset
a GBA core, or recreate a Genesis renderer without the host's lifecycle rules.

## Presentation choices

| Host shape | Presentation | Notes |
|---|---|---|
| Existing runtime ImGui (for example RT64) | `recomp_runtime_ui_render_imgui` | Preferred; host owns context and draw submission. |
| GPU host that can add an ImGui frame | `recomp_runtime_ui_render_imgui` | Preferred; use the existing recomp-ui ImGui dependency. |
| Writable low-resolution CPU framebuffer | `recomp_runtime_ui_render_argb8888` | Compatibility fallback; compact by design. |
| Different UI toolkit | Implement a presentation over the shared model | Do not duplicate settings behavior or console policy. |

The ImGui presentation is responsive: wide windows show persistent section
navigation beside the selected page; narrow windows use a drill-in hierarchy.
Both use the same palette, spacing, radius, and semantic colors as the launcher.
An N64 host therefore gets the clean graphite/red/blue N64 theme with no CRT
scanlines; SNES uses its console theme; handheld themes remain flat rather than
pretending to be a CRT.

## What “every recomp-ui game” requires

Consuming recomp-ui is the anchor, but updating the git submodule alone cannot
inject an in-game frame into an arbitrary host loop. Universal rollout needs
one small adapter for each host/runtime family, not one implementation per
game:

1. create the shared runtime model with that game's supported settings;
2. route menu input before emulated input;
3. call the appropriate presentation in the host's render frame;
4. pause or continue simulation according to that host's policy;
5. apply and persist values through callbacks.

Games built from the same host template should share that adapter. Per-game
code should be limited to capabilities, labels/choices, and live-setting
callbacks. Console detection must never select behavior in the runtime core;
the system id selects design tokens only.

## Portability rules

- Do not add console-specific keys or settings to `recomp_runtime_ui.c`.
- Do not make the ARGB8888 fallback the required render path.
- Do not create or destroy a host's ImGui context in
  `recomp_runtime_ui_render_imgui`.
- Use the same choice arrays for pre-boot and runtime settings when both expose
  a value.
- The shared `save` callback follows successful value changes. Actions own any
  persistence they require and do not implicitly rewrite the settings file.
- Keep input semantic (`ACCEPT`, `BACK`, directions); hosts choose physical
  buttons and supply their prompt labels.
- Prefer UTF-8-capable modern presentation; the compact fallback is ASCII-only.
- A renderer/backend switch that cannot be applied live should remain in the
  pre-boot launcher until the host provides a safe restart workflow.

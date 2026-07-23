# Host netplay integration notes

Status: **active** · 2026-07-23

recomp-ui owns presentation and the universal UDP port policy. The **game**
owns lobby transport, soft-return, and rematch reboot. This page lists
contracts that bite every snesrecomp (and sibling) title wiring MotK-style
netplay through `RecompLauncherCNetplayCallbacks`.

For the engine-side checklist see snesrecomp
[`docs/RECOMP_NET.md`](https://github.com/mstan/snesrecomp/blob/main/docs/RECOMP_NET.md)
→ "Soft-return rematch checklist".

---

## `join()` guest bind

```c
int (*join)(void* ctx, const char* lobby_id, const char* password,
            char* guest_bind /* in/out, capacity >= 64 */);
```

Before every `join()` call, the ImGui backend fills `guest_bind` via
`launcher_udp_prepare_guest_bind()` (prefer UDP **7778**, then +1..+31 →
`0.0.0.0:<port>`). Hosts must advertise that bind on the lobby join.

| Path | Host behavior |
|------|----------------|
| Online / lobby-server join | Pass `guest_bind` through to the lobby client |
| LAN file-registry (`lan:…`) | May ignore `guest_bind` |
| Engine fallback | snesrecomp `snes_lobby_join` still rewrites NULL/empty/`host:0` |

Never rewrite a prepared bind to `:0` — the server would publish `peer_ip:0`
and LAN session start rejects it.

Host **create** uses the same helper family (`launcher_udp_port.*`): LAN keeps
the exact UI port; online prefers **7777**..+31.

---

## Soft-return launcher and `SDL_Quit`

`launcher_platform_close()` (SDL2 and SDL3) ends with **`SDL_Quit()`** so the
game gets a clean slate (GL attributes reset, no leftover launcher window).

That is intentional. Side effect: **all** SDL subsystems — including
**audio** — are torn down when the waiting room closes for a rematch.

### Symptom

After Escape / peer leave → lobby → **Play**:

```text
audio outputs: -1 device(s)
Audio subsystem is not initialized
Failed to open audio device
```

(or a silent hang if the host treats audio open as fatal).

### Host fix

On the rematch `session_reboot` path (after `recomp_launcher_run_window`
returns with a new `netplay_launch`), re-init before creating the game window
or opening audio:

```c
if (!SDL_WasInit(SDL_INIT_VIDEO) || !SDL_WasInit(SDL_INIT_AUDIO) ||
    !SDL_WasInit(SDL_INIT_GAMECONTROLLER)) {
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0)
    return 1;
}
```

First boot usually already called `SDL_Init` once; the `WasInit` guard avoids
stacking unnecessary init refcounts.

Do **not** remove `SDL_Quit()` from the platform close without a coordinated
host change — games rely on a clean shutdown between launcher and gameplay.

---

## Resume room fields

When soft-returning after a match, set on `RecompLauncherCGameInfo`:

- `resume_netplay_room = 1`
- `resume_netplay_endpoint` — optional LAN endpoint string for the header

Clear launch-pending / re-arm ready in the game’s lobby callbacks before
showing the waiting room again (`snes_lobby_clear_launch_pending`,
`snes_lobby_set_ready`, etc.).

---

## Peer disconnect UX

recomp-ui does not own the in-game disconnect dialog. Hosts should treat
mid-match peer loss like a local quit: soft-return to the lobby **without** a
blocking `SDL_ShowSimpleMessageBox`. Reserve modals for connect-timeout /
firewall guidance before the session starts.

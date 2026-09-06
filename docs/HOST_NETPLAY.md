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

## The lobby room is a view, not a popup

`LNG_VIEW_LOBBY` is the full-screen room. The launcher enters it whenever the
backend's `in_lobby()` reports the local player seated and leaves it when that
stops being true — there is no open/close call for hosts to make, and a
soft-return that lands on Netplay flips into the room on the next frame by the
same rule. The page is two columns (seat tables left; room address, match
settings inline, mod plan right) and its actions — Leave Lobby, Mods, PLAY —
live in the fixed footer. Every profile that opens a lobby shares it.

A host therefore only needs `in_lobby()` to be truthful: a backend that keeps
reporting seated after a kick or a dropped socket keeps the player on a room
page that will never start.

The right column is the lobby chat. The room address, match settings and mod
plan moved into the **Settings** popup (footer button; guests see it as
**Room Info**, read-only), and the header's top-right corner shows the mod
plan summary ("Mods: vanilla match", "Mods: Widescreen (16:9) +1 more") in
place of the old LOBBY label when the build has a mod provider.

### Chat callbacks

Three optional, append-only members: `chat_send(text)`, `chat_count()`,
`chat_get(index, RecompLauncherCNetplayChatMessage*)`. The UI never appends
its own line: a send returns 0 when accepted and the line appears through
`chat_get` once the room has it (online: the server's `chat` echo, which is
the room's order; PSX LAN: the host's `MOTK5 CHAT` relay). Keep a ring of the
last few dozen lines, oldest first, cleared on create / join / leave, and make
`seq` monotonic across rooms — the panel scrolls to a line only when `seq`
changes, so reusing 1 for the first line of a new room would leave it unscrolled.
A backend with no way to deliver a line (SNES LAN rooms today) returns nonzero
from `chat_send`; the UI reports "Chat is not available in this room."

For layout work without a server, the prototype launcher takes
`LNG_DEMO_LOBBY=host|guest` (a fake three-player room) and screenshots through
`LNG_SCRIPT`, e.g. `LNG_VARIANT=psx LNG_DEMO_LOBBY=host LNG_SCRIPT="wait:40;shot:/tmp/lobby.png;quit"`.

## Resume room fields

When soft-returning after a match, set on `RecompLauncherCGameInfo`:

- `resume_netplay_room = 1`
- `resume_netplay_endpoint` — optional LAN endpoint string for the header

Clear launch-pending / re-arm ready in the game’s lobby callbacks before
showing the waiting room again (`snes_lobby_clear_launch_pending`,
`snes_lobby_set_ready`, etc.).

---

## Bring-your-own memory card (PSX)

Three optional, append-only members at the end of
`RecompLauncherCNetplayCallbacks`, plus three `RecompLauncherCNetplayMember`
fields and `RecompLauncherCNetplayLaunch.guest_memcard`:

| Callback | Who | Meaning |
|----------|-----|---------|
| `memcard_offer_set(has_card, share)` | every peer | Publish this peer's offer. The launcher computes `has_card` (slot 1 enabled and, if a file is picked, it inspected valid) and calls this every frame the room is open with `share = -1` (keep); the seat-row glyph calls it with `share = 0/1`. Backends re-advertise only on change. |
| `guest_memcard_get()` | every peer | Host allow flag as this peer sees it (default 1). |
| `guest_memcard_set(allow)` | host | Flip the allow flag; refused (<0) elsewhere. |

The seat-row glyph is drawn on **seat 1** (P2) — by seat, not by who hosts:
seat 0 is always the sim authority whose cards are the match cards. It is lit
when `member[1].memcard_offer_valid && memcard_has_card && memcard_share &&
guest_memcard_get()`. P2's click toggles its own `share`; the host's click
toggles `allow`. A member with `memcard_offer_valid == 0` is an older build
and the tooltip says so.

`launch.guest_memcard` must come from the **host-settled** value delivered
with the launch (online: `match_caps.guest_memcard_active`; LAN: the trailing
`MOTK1 START` line), never from the local seat table — otherwise a toggle that
races the start splits the room into peers that wait for a card and peers
that never send one.

## Peer disconnect UX

recomp-ui does not own the in-game disconnect dialog. Hosts should treat
mid-match peer loss like a local quit: soft-return to the lobby **without** a
blocking `SDL_ShowSimpleMessageBox`. Reserve modals for connect-timeout /
firewall guidance before the session starts.

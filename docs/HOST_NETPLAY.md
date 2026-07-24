# Host netplay integration notes

Status: **active** · 2026-07-23

recomp-ui owns presentation and the universal UDP port policy. snesrecomp
owns lobby transport helpers, soft-return, and rematch reboot primitives.
Game trees stay **thin** — wire callbacks and register per-title hooks.

For the engine-side checklist see snesrecomp
[`docs/RECOMP_NET.md`](https://github.com/mstan/snesrecomp/blob/main/docs/RECOMP_NET.md)
→ "Soft-return rematch checklist".

---

## Where to put fixes

**Prefer snesrecomp or recomp-ui** for anything that optimizes networking or
the launcher ↔ netplay interaction. Do **not** land shared behavior as
one-off patches in each game’s `main.c` when it can live in:

| Layer | Owns |
|-------|------|
| **recomp-ui** (this repo) | Waiting-room UI, create/join UDP port prep (`guest_bind`), resume-room flags, presentation UX |
| **snesrecomp** | `snes_host_lobby_*`, `snes_host_barrier_admit` + `snes_netplay_connect_wait_*` (session-scoped connect clock), rematch/`session_reset` helpers |
| **Game repo** | Thin: lobby identity/caps, pad + SDL poll + connect-timeout **modal** hooks, `RtlRunFrame`, `RtlGameInfo.session_reset` — no private admit/timer loops |

Per-title quirks that still belong in the engine (shared runner path) should
use snesrecomp’s existing extension points (`RtlGameInfo` hooks, title /
match_caps gates) — not a private fork of the helpers inside one game tree.
See snesrecomp `docs/RECOMP_NET.md` → "Layering policy".

---

## `join()` guest bind

```c
int (*join)(void* ctx, const char* lobby_id, const char* password,
            char* guest_bind /* in/out, capacity >= 64 */);
```

Before every `join()` call, the ImGui backend fills `guest_bind` via
`launcher_udp_prepare_guest_bind()` (prefer UDP **7778**, then +1..+31 on a
**concrete local IPv4** when available, else `0.0.0.0:<port>`). Online host
create uses `launcher_udp_prepare_host_bind()` the same way (prefer **7777**).
Advertising a real LAN IP stops MotK `rewrite_endpoint` from substituting a
wrong WebSocket TCP peer (e.g. router `.1`). Hosts must pass `guest_bind`
through on lobby join.

| Path | Host behavior |
|------|----------------|
| Online / lobby-server join | Pass `guest_bind` through to the lobby client |
| LAN file-registry (`lan:…`, same machine) | May ignore `guest_bind` |
| LAN Direct IP (typed `lan:ip:port`) | snesrecomp UDP seat-claim (`rnet_lan_direct_*`); pass prepared `guest_bind` when possible |
| Engine fallback | snesrecomp `snes_lobby_join` still rewrites NULL/empty/`host:0` |

Join Direct return codes (UI): `0` ok, `-2` password, `-3` no UDP response /
unreachable, other = full / started / identity mismatch.

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

### Host fix (snesrecomp helper)

On the rematch `session_reboot` path:

```c
if (snes_host_ensure_sdl() != 0)
  return 1;
snes_host_session_reset(); /* RtlGameInfo.session_reset — title sticky clears */
```

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

recomp-ui does not own the in-game disconnect dialog. Hosts should call
`snes_netplay_soft_exit_to_lobby(...)` (or the same path as local quit):
soft-return to the lobby **without** a blocking `SDL_ShowSimpleMessageBox`.
Reserve modals for connect-timeout / firewall guidance before the session
starts.

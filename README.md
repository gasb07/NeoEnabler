# NeoEnabler

<p align="center"><img src="assets/icon0.png" width="128" alt="NeoEnabler"></p>

Play PS4 games on your PS5 in **PS4 Pro mode** at higher resolutions, up to native 4K.

The PS5 already runs PS4 games that have PS4 Pro support in Pro mode. Games without it run as on a regular PS4.
NeoEnabler makes those games start as if they were on a PS4 Pro (games with Pro support are left as they are). This
alone does not make any game work: every game tested so far broke in Pro mode without its patch (black or
corrupted image, or the game closing). The patches in `xml/` fix each game for Pro mode and raise its resolution,
so NeoEnabler is only useful for the games listed below.

You pick the patches in the NeoEnabler panel. A game starts in Pro mode, and gets the 8 GiB memory budget, only
when one of its enabled patches needs it; every other game starts as usual.

## What you need

- A jailbroken PS5 on firmware 4.00 to 13.60 (tested on 13.20) with an ELF loader, such as etaHEN or elfldr.
- [ps-patch-system](https://github.com/illusionyy/ps-patch-system), loaded **before** NeoEnabler.

> [!IMPORTANT]
> **Load [ps-patch-system](https://github.com/illusionyy/ps-patch-system) first, then NeoEnabler, after every
> console boot.** NeoEnabler hooks into ps-patch-system and installs nothing without it: the panel then shows
> "ps-patch-system not loaded" and no game starts in PS4 Pro mode. If you loaded NeoEnabler first, load
> ps-patch-system, quit NeoEnabler in the panel and send `neo-manager.elf` again.

## Install

1. Download `neo-manager.elf` from the [latest release](https://github.com/gasb07/NeoEnabler/releases/latest).
2. After every console boot, load [ps-patch-system](https://github.com/illusionyy/ps-patch-system) **first**,
   then send `neo-manager.elf` with your payload sender (port 9021).
3. A notification on the PS5 confirms that NeoEnabler is enabled. It downloads the game patches from this
   repository and adds them to the ps-patch-system files (see [Game patches](#game-patches)).
4. Open the panel and tick the patches you want for each game.

The **8 GiB PS4 Pro memory budget** is used only by patches that need it (marked "8 GiB" in the panel). It is
supported **only** when both conditions are met:

- **Firmware 13.20** (tested) or **13.60** (offsets found, **needs testing**).
- **Hypervisor exploit enabled** (Byepervisor or another method that allows reading and writing the kernel code
  page). NeoEnabler checks that access without relying on exploit-specific markers and does not unlock the
  hypervisor itself.

When either condition is missing, patches that need 8 GiB are greyed out and cannot be enabled. Everything else
works without 8 GiB.

**If you are on firmware 13.20 or 13.60, please test the 8 GiB budget and report your results.**

NeoEnabler must keep running for the automatic Pro mode and 8 GiB: send it again after every reboot.

## Control panel

Open `http://<PS5 IP>:8765` in a browser on your network, or the **NeoEnabler** shortcut in the PS5 Media tab.

![Games page](docs/panel-games.jpg)

- **Games**: every installed PS4 game that has a NeoEnabler patch for its installed version, with its icon, title
  ID, version and all of its patches. Tick a patch to enable it; the change goes straight to the ps-patch-system
  settings, there is nothing to save. Only one NeoEnabler resolution entry per game can be on: ticking one turns
  the others off. In a collection, each game in it can have one resolution on. The collection starts in its
  first game, so the other games' resolutions need one of the first game: the panel ticks it for you. Patches
  that need 8 GiB are marked and greyed out when 8 GiB is not available.
  Use the search bar to find a game by name or title ID.
- **PS4 Pro mode and 8 GiB are automatic.** When a PS4 game starts, NeoEnabler checks its enabled patches: the
  game starts in PS4 Pro mode only when one of them needs it, and 8 GiB is switched on before the game starts
  only when one of them needs it. 8 GiB returns to the original 5.25 GiB budget after the game closes.
- **Status**: PS4 Pro mode of the running game, 8 GiB state and the running game.
- **Game patches**: Update Patches downloads the patches from this repository (also done at startup).
- **Logs**: Show Logs, Download Logs, Save Logs and Save Diagnostic. They give one text with a report of the
  console, NeoEnabler's own log (`/data/neo-enabler/neo.log`) and the kernel log kept in
  `/data/neo-enabler/kernel.log`.
- **Advanced** (collapsed): Pro mode for every PS4 game, or 8 GiB kept on, regardless of the patches.

Quitting the manager stops the automatic Pro mode and 8 GiB until it runs again. Rebooting removes the hook.
A failed kernel-write recovery of the memory budget requires a console reboot before launching games.

Anyone on your local network can open the panel.

### GPU clock tab (0.3.0)

![GPU clock tab](docs/panel-gpu.jpg)

Open **GPU clock** in the panel, select a target from **1800 to 2200 MHz** in 50 MHz steps, and use
**Apply boost**. The current clock reported by the SMU updates every two seconds. The target and enable flag
are saved with **Save Settings** and requested again when a PS4 game starts. This works independently of NEO and
8 GiB, including PS4 base mode.

GPU boost is **off by default**, with 2200 MHz selected. It supports **firmware 13.20 only** and uses the stock
`/dev/mp1` driver, without patching the kernel or needing a hypervisor exploit. The sustained frequency may be
lower than the target. A short gameplay test at 2200 MHz read around 2155-2200 MHz. FPS gains and long-term
stability have not been established.

**Disable boost** and **Quit** restore the clock the game had before boost. When the game closes, the system
manages the clocks again, and the next PS4 launch starts from a new baseline. If the clock is reset to 1800 MHz
while the game runs, the manager restores the target up to three times. If a request or restoration fails, the
panel shows the error and stops retrying: use Disable to retry the restoration before enabling again.

Boost does not change CPU clocks, thermal or power limits, memory bandwidth or ROPs. On other firmwares, or when
the MP1 driver does not answer, the GPU clock tab is not shown.

## Game patches

Every game needs its patch from `xml/` to work in PS4 Pro mode. NeoEnabler installs them for you:

1. At startup (and with **Update Patches** in the panel), NeoEnabler downloads `xml/` from this repository and adds
   its entries to every ps-patch-system file of the same game in `/data/user/patches/xml_orbis/` (all regions,
   matched by the game title). Entries already there are updated; other patches in those files are kept. An entry
   with the same patch lines under another name is renamed to the repository name, and stays enabled if it was. A
   game without a file gets a new `<TITLE ID>.xml`. Changed files are backed up first to
   `/data/neo-enabler/xml-backup/<date-time>/` (the last 5 backups are kept).
2. In the panel's Games page, tick **one** resolution entry for the game, for example "Native 4K (PS4 Pro mode)".
3. For 4K entries, set the PS5 video output to 2160p.

Every NeoEnabler entry carries `NeoRequires="pro"` or `NeoRequires="pro,8gib"`, which tells the manager what the
game needs at launch. ps-patch-system ignores this attribute.

| Game (version) | Resolutions | Notes |
| --- | --- | --- |
| Need for Speed Rivals (01.04, 01.03) | 1440p, 4K | If your existing patch file for the game has "Resolution Patch", keep it off. |
| Resident Evil 6 (01.01) | 1440p, 4K | Small Capcom logo in 4K. |
| Bloodborne (01.09) | 1440p, 1620p, native 4K | Native 4K needs the 8 GiB memory budget. **Tested on firmware 13.20 only.** |
| Just Cause 3 (01.05) | 1440p, 4K | Also enable "Offline mode", or the game keeps trying to log in. |
| Battlefield 4 (01.24) | 1440p, 1620p, 1800p, 4K | 1800p holds 60 fps. 4K runs around 49 fps. |
| Battlefield Hardline (01.09) | 1440p, 1620p, 1800p, 4K | |
| Killzone Shadow Fall (01.81) | 1440p, 1620p, 1800p, 4K | Some surfaces are slightly darker from 1440p to 1800p. 4K is clean but below 60 fps. |
| The Order: 1886 (01.02) | 4K, 4K up to 60 fps | Small sparkles may appear on edges while moving. Native 4K 60fps uses triple buffering: no tearing, slight stutter. |
| Dark Souls III: The Fire Fades Edition (01.03) | 1440p, 1620p, 1800p | The game already runs in PS4 Pro mode. 4K does not fit in its memory. |
| Batman: Arkham Knight (01.15) | 1620p, 1800p, native 4K | |
| Sleeping Dogs: Definitive Edition (01.01) | 1440p, 1620p, 1800p, native 4K, each also up to 60 fps | The 60 fps entries use triple buffering: no tearing, slight stutter. 1800p up to 60 fps runs at 53-57 fps, native 4K up to 60 fps at 41-44 fps. |
| Driveclub (01.28) | 1440p, 4K (1620p render), 4K (1800p render), native 4K | 4K needs the 8 GiB memory budget. **Tested on firmware 13.20 only.** 4K with 1800p render runs close to 60 fps; native 4K is below 60 fps in races. 4K with 1620p or 1800p render may show blocky artifacts in the clouds and on other surfaces. Native 4K: snow can show inside the car. |
| Uncharted: The Nathan Drake Collection (01.02) | 1440p, 1620p, 1800p, native 4K for each of the three games | The collection starts in Uncharted 1, which sets PS4 Pro mode for Uncharted 2 and 3: an Uncharted 2 or 3 entry needs an Uncharted 1 entry on (the panel enables it). Native 4K fits the stock memory with a smaller texture pool. |

Patch IDs, how the hook works and how to build from source: [docs/TECHNICAL.md](docs/TECHNICAL.md).

Test results by game and firmware: [docs/COMPATIBILITY.md](docs/COMPATIBILITY.md). Report yours with the
[Test result form](https://github.com/gasb07/NeoEnabler/issues/new?template=test-result.yml).

## Helping extend 8 GiB to other firmwares

The 8 GiB budget is tested on firmware **13.20** only. Firmware **13.60** has verified offsets but needs a
hardware test. To port it to other firmwares we need a diagnostic capture. **Do not post kernel dumps**; see
[diagnostics/README.md](diagnostics/README.md).
Use **Save Diagnostic** in the web panel (or `GET /diagnostic`) and see
[diagnostics/README.md](diagnostics/README.md) for what to submit. **If you are on 13.60, please test and
share your results.**

## Credits

### Code and tools

| Project | Author | Used for |
| --- | --- | --- |
| [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk) | [John Törnblom](https://github.com/john-tornblom) | Toolchain (`prospero-clang`, `ld.lld`), runtime with `kernel_proc_copyin/out` and `kernel_dynlib_*`, `prospero-deploy` |
| [elfldr](https://github.com/ps5-payload-dev/elfldr) | [ps5-payload-dev](https://github.com/ps5-payload-dev) ([John Törnblom](https://github.com/john-tornblom)) | ELF loader on port 9021 |
| [ps-patch-system](https://github.com/illusionyy/ps-patch-system) | [illusion](https://github.com/illusionyy) | Patch server, XML format, the `getAppInfo` hook the shim chains onto, call site patterns and `ATTRIBUTE` offsets. `src/dynlib.h` and `src/proc.h` copied from `shared/` |
| [ps5-app-dumper](https://github.com/EchoStretch/ps5-app-dumper) | [EchoStretch](https://github.com/EchoStretch) | Reference for the `libSceAppInstUtil` load order and the Media tab shortcut (`deeplinkUri`) |
| [etaHEN](https://github.com/etaHEN/etaHEN) | [LightningMods](https://github.com/LightningMods) | Jailbreak, ELF loader, FTP and klog used during development and testing |
| [Pillow](https://github.com/python-pillow/Pillow) | [python-pillow](https://github.com/python-pillow) | Icon generation (`tools/make_icon.py`) |
| [ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus) | [drakmor](https://github.com/drakmor) | Visual style of the games page (colors and layout of its web library) |

### Game patches

| Patch | Author | Source |
| --- | --- | --- |
| Bloodborne: Resolution Patch WxH, base of the NEO 1440p/1620p entries | [Kyo](https://github.com/Kyoskii) | [shadps4-emu/ps4_cheats](https://github.com/shadps4-emu/ps4_cheats/blob/main/PATCHES/Bloodborne.xml) |
| The Order: 1886: 60 FPS Unlock, used by the Native 4K 60fps entry | [illusion](https://github.com/illusionyy) | PS-Game-Patch, `patches/xml/TheOrder1886-Orbis.xml` |

Entries with author "Local experiment" (Pro mode, resolutions, JC3 offline mode) and the NEO fixes inside the
Bloodborne resolution entries were made in this project.

## License

GPL-3.0 (`LICENSE`). The payload includes the ps5-payload-sdk runtime (GPL-3.0-or-later), so
the binary must be distributed under GPL-3.0.

Third-party files keep their own licenses:

- `src/dynlib.h`, `src/proc.h`: Apache-2.0 (ps-patch-system), compatible with GPL-3.0.
- The Bloodborne resolution code from shadps4-emu/ps4_cheats: the repository declares no license. It is included
  with attribution to its author.

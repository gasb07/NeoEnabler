# NEO Enabler: technical notes

Details for developers. The user guide is in the [README](../README.md).

## How it works

ps-patch-system copies its code to the start of the `SceShellCore` `eh_frame`, makes the module RWX and redirects
the `getAppInfo` call to its own hook. NEO Enabler chains onto that hook:

1. The manager finds the `getAppInfo` call with the same per-firmware patterns as ps-patch-system
   (`patchGetAppInfoSfo`). The call must point inside `eh_frame` (the ps-patch-system hook), otherwise the panel shows
   "ps-patch-system not loaded".
2. The shim (`src/shim.S`, position independent) is copied to the last 512 bytes of `eh_frame` and the call is
   redirected to it. The shim calls the ps-patch-system hook first, then, for PS4 title IDs whose `ATTRIBUTE` is
   exactly 2, sets bit `0x00800000` (`kPS4Pro`) and logs `[NEO <title>] old>new` to klog.
3. Per-firmware values are filled into the shim at install time: the previous call target, `printf`, the
   `ATTRIBUTE` offset in the app info (`0x26c` up to 10.60, `0x1f4` for 11.00 to 13.00, `0x214` from 13.20, as in
   ps-patch-system `get_ps5_attr`) and whether the app info is the third or fourth argument (fourth from 9.00).
4. Every write happens with `SceShellCore` stopped (SIGSTOP) and is read back. Disabling points the call back to
   the ps-patch-system hook.
5. Disabling also removes the hook of NEO Enabler v4 when it is still in `SceShellCore` (fw 13.20). v4 overwrote
   124 bytes of ps-patch-system code (`eh_frame + 0x5cf`) and redirected the call at `eh_frame + 0x6bc` to them, so
   games kept launching in Pro mode while the panel showed "disabled". The original bytes are restored only when
   the exact v4 bytes are found (`src/legacy_v4.h`).
6. On first run it installs the `NEOE00001` ("NEO Enabler") shortcut in the Media tab, which opens the panel.

The shim overwrites unwind data at the end of `eh_frame`, as ps-patch-system does at its start. Rebooting the
console restores it.

Pro mode alone does not make any game work: every game tested broke without its fixes in `xml/` (display buffer
too small, corrupted layout, GPU hangs).

## Build

Dependencies: [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk) (default `/opt/ps5-payload-sdk`, or set
`PS5_PAYLOAD_SDK`), `python3`, `xxd`.

```sh
./build.sh                  # builds build/neo-manager.elf
python3 tools/make_icon.py  # optional: regenerates assets/icon0.png (needs Pillow and the DejaVu Sans Bold font)
```

| File | Contents |
| --- | --- |
| `src/manager.c` | Payload: web panel, notification, Media tab shortcut |
| `src/shellcore_hook.h` | Call site scan, cave, install and removal of the hook |
| `src/legacy_v4.h` | Detection and removal of the NEO Enabler v4 hook |
| `src/shim.S`, `src/shim.ld` | Hook code, linked at 0 and copied to the cave |
| `src/dynlib.h`, `src/proc.h` | Headers from ps-patch-system (`shared/`) |
| `tools/gen_shim.py` | Generates `build/shim_data.h` (shim bytes and data field offsets) |
| `tools/make_icon.py` | Draws `assets/icon0.png` |
| `xml/` | Game patches |

## Releases

GitHub Actions (`.github/workflows/build.yml`) builds every push and pull request with the PS5 Payload SDK
release set in `SDK_VERSION`, and keeps `neo-manager.elf` as a build artifact.

To publish a release:

1. Set the new version in `src/manager.c` (`#define VERSION "NEO Enabler manager X.Y.Z"`).
2. Rename the `## Unreleased` section of `CHANGELOG.md` to `## X.Y.Z` (plain description of the changes for users)
   and commit both.
3. Tag the commit and push the tag:
   ```sh
   git tag vX.Y.Z
   git push origin vX.Y.Z
   ```

The workflow checks that the tag matches `VERSION` and that `CHANGELOG.md` has a section for it, builds the payload
and creates the GitHub release with that section as the description and `neo-manager.elf` and its SHA-256
checksum attached.

## Web panel

| Route | Effect |
| --- | --- |
| `GET /` | Independent NEO and 8 GiB states/controls, open PS4 games and reported DMEM |
| `POST /enable`, `POST /disable` | Installs or removes the hook (disable also removes the v4 hook) and notifies on the PS5 |
| `POST /memory/enable`, `POST /memory/disable` | Selects 8 GiB or restores the original budget, independently of the hook; defers actual writes while a game is open. Refused on firmwares without a budget layout (only 13.20 and 13.60 have one) |
| `POST /quit` | Stops the panel and keeps the hook state |
| `GET /status` | Plain text: version, 8 GiB state/automatic policy/result, hook state, firmware and open games with reported DMEM |
| `GET /diagnostic` | JSONL metadata capture (same format as `diagnostics/neo_diagnostic.c`): firmware, SDK, running games, driver presence, DMEM/FMEM queries. No patches applied, no kernel TEXT reads |

- Starting the manager enables the hook. The panel shows "enabled" while the v4 hook is present.
- Changes apply to games launched afterwards. Rebooting the console removes the hook and the panel.
- The panel port is bound before changing either setting. Sending another manager while that port is occupied
  reports the conflict and exits without changing the hook or budget.
- Open games: `eboot.bin` processes that load a PS4 graphics driver, named from the `TITLE` in
  `/system_data/priv/appmeta/<title ID>/param.sfo`. `libSceGnmDriverForNeoMode` means Pro mode and `libSceGnmDriver`
  means base mode. Some games load `libSceGnmDriverCompat1` instead (The Order: 1886 on 13.20, while Battlefield 4
  loads the Pro driver on the same firmware), with the same name in both modes, and then the mode comes from the
  `kPS4Pro` bit of the launch attribute (offset 0x28 of `sceKernelGetAppInfo`, checked on 13.20).
- The panel has no authentication: any device on the local network can use it.
- Log: every action and its result go to the kernel log with the prefix `[NeoEnabler]`, followed by a `state:` line
  (NEO hook, v4 hook, actual and requested 8 GiB state). Show Logs, Download Logs and Save Logs give a report, NeoEnabler's own log (`/data/neo-enabler/neo.log`) and the kernel log it keeps (`kernel.log`, polled four times a second); Save Logs writes it to `/data/neo-logs.txt`.

## 8 GiB memory budget (0.2.0)

`src/neo_budget.h` supports only the captured firmware13.20 budget constructor. Other supported NEO firmwares
keep normal launch-hook functionality and report the memory adjustment unavailable. The actual firmware is
read from `machdep.upd_version`, not the process SDK version.

Detection is capability-based and does not read a Byepervisor cookie or try to execute a hypervisor exploit.
Before reading kernel instructions, it reads ordinary DATA globals for the direct-map indices and kernel pmap,
then walks the target page's guest page tables. All levels must be present, writable and free of XOTEXT. A
missing/unreadable mapping blocks the code read. Page-table discovery uses the13.20 layout documented in
the local Byepervisor-Reborn sources; the traversal does not change any PTE or disable protections.

Guest permission bits alone **do not attest that nested-page protections or the entire hypervisor are disabled**.
The subsequent bounded instruction reads and verified patch write establish the actual capability required here.
Methods must expose the relevant kernel page for read/write through the SDK primitives; merely having arbitrary
kernel DATA read/write is insufficient. Read/write failure leaves the feature unavailable or failed, rather than
claiming that an exploit succeeded. The new detection path still requires live testing on a locked console and
with other unlock methods; only local simulated protection/error cases have been tested for0.2.0.

The patch replaces12bytes at TEXT+0x9f9076 with `movabs r15,0x200000000; jmp short TEXT+0x9f90b8`.
The existing relay jumps to the original continuation at+0x9f92b6. Guards verify the baseline at+0x9f9044,
relay, continuation and exact original/known5.5GiB/8GiB selection. Application and restoration require no
`eboot.bin` processes and empty application memory areas1..3. These checks are conservative snapshots, not a
lock against an external launch racing the operation; close games and avoid launching during a change.
Writes are read back; failures attempt rollback, and an unverified rollback blocks all further memory actions
for that manager instance and instructs reboot. The manager never patches an unknown sequence or a live budget.
Restoration selects the captured original5.25GiB NEO configuration.

`src/memory_control.h` defaults to automatic8GiB at manager startup. It rechecks prerequisites roughly every
two seconds while the panel runs, in the same HTTP thread: the SDK's shared kernel pipe state must not be used
concurrently by a new worker thread. No game is paused or remotely called. A requested change waits for games
to close. Manual disable changes the target to the original budget and keeps it there; it does not touch the
launch hook. Manual enable resumes automatic8GiB. NEO Enable/Disable do not touch the memory target or patch.
These preferences last for the manager session; a new manager starts with the startup defaults. Quit stops
polling without restoring either patch. Reboot removes both patches.

The web status distinguishes `memory_8gib` (observed constructor selection) from `memory_auto` (requested
policy). Running games show `kern.app.dmem_size_pid`;8192MiB is their reported launch budget, not memory use
or an allocation/access test. The pre-release8GiB experiment was measured on Driveclub13.20; the user also
reported successful gameplay with a patch that uses memory. This is distinct from a completed instrumented GPU
probe: the separate native-allocation probes were refused by mmap in the game, including CPU-only mapping.

Run host tests with `cc -O2 -Wall -Wextra -Werror tests/budget_test.c -o /tmp/neo-budget-test`, then execute
`/tmp/neo-budget-test`. They use simulated kernel memory, never a console. Tests cover page access before code
reads,1GiB/2MiB/4KiB mappings, unknown layouts, games/areas, readback errors, rollback, automatic/deferred/manual
policy and permanent blocking after failed recovery. CI runs them before building. They do not validate a live
hypervisor, the new detector on hardware, allocation capacity or GPU behavior.

## Game patches

The files in `xml/` use the ps-patch-system format. The patch system looks for `<TITLE ID>.xml`, so rename the
file when copying it to `/data/user/patches/xml_orbis/`, for example `Bloodborne_CUSA00900.xml` becomes
`CUSA00900.xml`. Enable the entries in the Patch Manager or in `!_settings.xml`. Hashes are computed by the patch system from the file
name, entry name, title, ELF and app version, so renaming an entry changes its hash.

| Game | File | Enable | Result |
| --- | --- | --- | --- |
| Need for Speed Rivals 01.04 | `NeedForSpeedRivals_CUSA00168.xml` | one resolution: `7f4ee42f` 4K or `fc7fe6b6` 1440p | native 4K or 1440p |
| Resident Evil 6 01.01 | `ResidentEvil6_CUSA03856.xml` | one resolution: `86499dd1` 1440p or `05789f48` 4K | 1440p or native 4K (small Capcom logo in 4K) |
| Bloodborne 01.09 | `Bloodborne_CUSA00900.xml` | one resolution: `e7fcd40e` native 4K, `f0e950ea` 1620p or `0e99b4ca` 1440p | up to 1620p, or native 4K with the 8 GiB budget (private heap +512 MiB, privateB +1 GiB) |
| Just Cause 3 01.05 | `JustCause3_CUSA01493.xml` | one resolution: `1575f9b2` 1440p or `9644fb2b` 4K, + `fd9434a8` Offline mode | 1440p or native 4K |
| Battlefield 4 01.24 | `Battlefield4_CUSA00049.xml` | one resolution: `f9ec0351` 1440p, `079ce771` 1620p, `3ad0f441` 1800p or `7add01c8` 4K | 1440p, 1620p, 1800p (stable 60 fps) or native 4K (~49 fps) |
| Battlefield Hardline 01.09 | `BattlefieldHardline_CUSA00625.xml` | one resolution: `e7de2901` 1440p, `19aecd21` 1620p, `24e2de11` 1800p or `64ef2b98` Native 4K | 1440p, 1620p, 1800p or native 4K |
| Killzone Shadow Fall 01.81 | `KillzoneShadowFall_CUSA00190.xml` | one resolution: `8c299cea` 1440p, `725978ca` 1620p, `4f156bfa` 1800p or `0f189e73` Native 4K | 1440p, 1620p, 1800p (slight darkening on some surfaces) or native 4K (clean, below 60 fps) |
| The Order: 1886 01.02 | `TheOrder1886_CUSA00785.xml` | one resolution: `6378fc73` Native 4K or `d753e8d4` Native 4K 60fps | native 4K (small one-frame edge sparkles may remain while moving). Native 4K 60fps adds triple buffering: third display buffer in the free GpuStack tail of the Native 4K entry, flip index remapped per frame, the GPU waits for a free buffer only at the display pass; vsync on, 30 to 60 fps |
| Driveclub 01.28 | `Driveclub_CUSA00093.xml` | one resolution: `8cdf7724` 1440p, `d4ad7fde` 4K with 1620p render, `4903b2ea` 4K with 1800p render or `65ba17e0` native 4K | native 1440p, 4K output with 1620p or 1800p render, or native 4K (4K entries need the 8 GiB budget: +1 GiB render memory; with the stock budget the game crashes at launch). 1440p and 4K entries use an adaptive flip: vsync when the frame is on time, immediate flip when it is late (the game's own late-frame flip mode hangs the GPU in PS4 Pro mode) |
| Sleeping Dogs: Definitive Edition 01.01 | `SleepingDogsDefinitiveEdition_CUSA01111.xml` | one resolution: `3e86be10` 1440p, `b5ab6c71` 1440p 60fps, `c0f65a30` 1620p, `9d79ba91` 1620p 60fps, `fdba4900` 1800p, `9cca9281` 1800p 60fps, `bdb7bc89` Native 4K or `099ca82e` Native 4K 60fps | 1440p to native 4K. The 60fps entries add triple buffering (a third swapchain target, back buffer index cycling 0-2, vsync on) and illusion's 60 FPS Unlock; video heap +512 MiB. Every entry also fixes a game race that crashed the load at random (a thread object on the stack, moved to static memory) |
| Uncharted: The Nathan Drake Collection 01.02 | `UnchartedTheNathanDrakeCollection_CUSA02320.xml` | one resolution per game: Uncharted 1 (`eboot.bin`) `d0451c04` 1440p, `5c867f88` 1620p, `c03b7ff4` 1800p or `587bcd5d` Native 4K; Uncharted 2 (`big2-ps4_Shipping.elf`) `c44dac36`, `25f016fa`, `2fad03e6` or `e493ae0f`; Uncharted 3 (`big3-ps4_Shipping.elf`) `0f645e9f`, `a18bfaf7`, `0c95d93f` or `efebe61e` | 1440p, 1620p, 1800p or native 4K. The collection menu runs in Uncharted 1, and Uncharted 2 and 3 inherit its PS4 Pro mode, so their entries need an Uncharted 1 entry on. The game asks for 472 MiB of extra direct memory at most (more fails); 1800p and 4K take the unused part of the Render Buffer pool, and 4K also 200 MiB (Uncharted 3: 300 MiB) of the texture pool, for the render target heap (about 1370 MiB used at 4K) |

- Each resolution entry already includes the Pro mode fixes. Enable only one per game (in a collection, one per
  game executable, its AppElf).
- Rivals: keep the community "Resolution Patch" disabled if it is in the patch file.
- JC3: the Offline mode entry is required. Without it the game loops on the online login.
- The XMLs only carry entries made in this project. The Bloodborne entries build on Kyo's resolution patch.

# NEO Enabler: technical notes

Details for developers. The user guide is in the [README](../README.md).

## How it works

ps-patch-system copies its code to the start of the `SceShellCore` `eh_frame`, makes the module RWX and redirects
the `getAppInfo` call to its own hook. NEO Enabler chains onto that hook:

1. The manager finds the `getAppInfo` call with the same per-firmware patterns as ps-patch-system
   (`patchGetAppInfoSfo`). The call must point inside `eh_frame` (the ps-patch-system hook), otherwise the panel shows
   "ps-patch-system not loaded".
2. The shim (`src/shim.S`, position independent) is copied to the last 512 bytes of `eh_frame` and the call is
   redirected to it. The shim calls the ps-patch-system hook first. For PS4 title IDs it then posts the title ID and
   the app info address to the manager and waits up to 5 s for the answer: Pro mode or not, and the `ATTRIBUTE`
   offset in the app info. When the answer is Pro mode and `ATTRIBUTE` is exactly 2, it sets bit `0x00800000`
   (`kPS4Pro`) and logs `[NEO <title>] old>new` to klog.
3. Per-firmware values are filled into the shim at install time: the previous call target, `printf`, the
   `ATTRIBUTE` offset in the app info (`0x26c` up to 10.60, `0x1f4` for 11.00 to 13.00, `0x214` from 13.20, as in
   ps-patch-system `get_ps5_attr`) and whether the app info is the third or fourth argument (fourth from 9.00).
   The table offset was measured on 4.50, 8.00, 11.00 and 13.20 only. At each launch the manager reads the app info
   and looks for the `ATTRIBUTE` and `ATTRIBUTE2` values of the game's `param.sfo` at consecutive offsets
   (`src/attr_detect.h`). The table offset wins when it matches, then the offsets of the other firmwares, then a
   single match. Without a choice, or without an answer, the shim uses the table offset. The log line
   `launch: <title>: ATTRIBUTE ...` shows the matches and the offset used, followed by the app info bytes from
   `0x1c0` to `0x2a0` when the offset differs from the table.
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

## No-HV 8 GiB budget (0.3.5)

The manager selects the firmware-specific DATA backend for 13.20 and 13.60. It reads the actual firmware from
`machdep.upd_version`, validates the ShellCore BeginAppMount2 function, exact call, import stub, GOT target and
libkernel wrapper, and installs the retained shim in a fresh padding reservation. Userland pages must already
be RWX. Installation and detach confirm ShellCore's stopped state using the bounded KERN_PROC_PROC enumeration;
partial call writes are rolled back before resumption. Retained code is never reclaimed or reused.

After a successful target mount the POST/CLAIM/HELD exchange establishes the pre-constructor barrier. The DATA
transaction rechecks container identity and eligibility before changing extension byte+3 from zero to `0xc0`.
This is a 3 GiB increment for the reference stock configuration, not an 8 GiB increment. The global stays owned
for the game's lifetime, with the gate retained against another mount. The constructor must report
`field38=0x24c000000`; the running game separately reports its budget via `kern.app.dmem_size_pid`.

After confirmed teardown, no game process and no active shim callback, rollback restores the original global
before clearing dirty and releasing the gate. A deadline or changed identity does not authorize an uncertain
rollback: the controller retains ownership, blocks further mutation and reports that a reboot is required.
Quiescent teardown can still recover the global. Quit refuses while the transaction is owned or recovery is blocked.
All SDK reads/writes and mailbox polling run in the HTTP loop, without a concurrent kernel-pipe worker.

The older TEXT implementation and its tests remain as legacy code for the non-DATA selection. It is not
probed as a fallback after a supported DATA hook fails. A reboot is required before switching from an old
TEXT memory payload to this implementation, since DATA identity cannot attest to untouched protected code.

See [8GB-NO-HV-1360.md](8GB-NO-HV-1360.md) for offsets, input identities and limits and
[VALIDATION-0.3.5-experimental.md](VALIDATION-0.3.5-experimental.md) for the console regression.

## Game patches

The files in `xml/` use the ps-patch-system format. The patch system looks for `<TITLE ID>.xml`, so rename the
file when copying it to `/data/user/patches/xml_orbis/`, for example `Bloodborne_CUSA00900.xml` becomes
`CUSA00900.xml`. Enable the entries in the Patch Manager or in `!_settings.xml`. Hashes are computed by the patch system from the file
name, entry name, title, ELF and app version, so renaming an entry changes its hash.

The patch system only patches the executable a game starts with. An entry whose `AppElf` is a module the game loads
itself (a name ending in `.prx` or `.sprx`, for example `PreyDll.prx`) is applied by the manager: when the game
process appears it reads the enabled module entries, waits up to 120 s for each module to load, writes the lines at
the module base plus `Address` (an offset in the module) and reads them back. Supported line types: bytes, byte,
bytes16, bytes32, bytes64, float32, float64 and utf8. The result is logged as `modules:` lines.

| Game | File | Enable | Result |
| --- | --- | --- | --- |
| Need for Speed Rivals 01.04 | `NeedForSpeedRivals_CUSA00168.xml` | one resolution: `7f4ee42f` 4K or `fc7fe6b6` 1440p | native 4K or 1440p |
| Resident Evil 6 01.01 | `ResidentEvil6_CUSA03856.xml` | one resolution: `86499dd1` 1440p or `05789f48` 4K | 1440p or native 4K (small Capcom logo in 4K) |
| Bloodborne 01.09 | `Bloodborne_CUSA00900.xml` | one resolution: `e7fcd40e` native 4K, `32931f2f` 1800p, `f0e950ea` 1620p or `0e99b4ca` 1440p | up to 1620p, or 1800p and native 4K with 8GB Mode (1800p: private heap +1.5 GiB, privateB +1 GiB; native 4K: private heap +512 MiB, privateB +1 GiB) |
| Just Cause 3 01.05 | `JustCause3_CUSA01493.xml` | one resolution: `1575f9b2` 1440p or `9644fb2b` 4K, + `fd9434a8` Offline mode | 1440p or native 4K |
| Battlefield 4 01.24 | `Battlefield4_CUSA00049.xml` | one resolution: `f9ec0351` 1440p, `079ce771` 1620p, `3ad0f441` 1800p or `7add01c8` 4K | 1440p, 1620p, 1800p (stable 60 fps) or native 4K (~49 fps) |
| Battlefield Hardline 01.09 | `BattlefieldHardline_CUSA00625.xml` | one resolution: `e7de2901` 1440p, `19aecd21` 1620p, `24e2de11` 1800p or `64ef2b98` Native 4K | 1440p, 1620p, 1800p or native 4K |
| Killzone Shadow Fall 01.81 | `KillzoneShadowFall_CUSA00190.xml` | one resolution: `8c299cea` 1440p, `725978ca` 1620p, `4f156bfa` 1800p or `0f189e73` Native 4K | 1440p, 1620p, 1800p (slight darkening on some surfaces) or native 4K (clean, below 60 fps) |
| The Order: 1886 01.02 | `TheOrder1886_CUSA00785.xml` | one resolution: `6378fc73` Native 4K or `d753e8d4` Native 4K 60fps | native 4K (small one-frame edge sparkles may remain while moving). Native 4K 60fps adds triple buffering: third display buffer in the free GpuStack tail of the Native 4K entry, flip index remapped per frame, the GPU waits for a free buffer only at the display pass; vsync on, 30 to 60 fps |
| Dark Souls III: The Fire Fades Edition 01.03 | `DarkSoulsIII_CUSA07439.xml` | one resolution: `30827e19` 1440p, `cef29a39` 1620p, `f3be8909` 1800p or `d9e71edd` Native 4K with 8GB Mode | native PS4 Pro game: 1440p to 1800p, or native 4K with 8GB Mode (graphics private heap +2.5 GiB) |
| Driveclub 01.28 | `Driveclub_CUSA00093.xml` | one resolution: `8cdf7724` 1440p, `d4ad7fde` 4K with 1620p render, `4903b2ea` 4K with 1800p render or `65ba17e0` native 4K | native 1440p, 4K output with 1620p or 1800p render, or native 4K (4K entries need the 8 GiB budget: +1 GiB render memory; with the stock budget the game crashes at launch). 1440p and 4K entries use an adaptive flip: vsync when the frame is on time, immediate flip when it is late (the game's own late-frame flip mode hangs the GPU in PS4 Pro mode) |
| Sleeping Dogs: Definitive Edition 01.01 | `SleepingDogsDefinitiveEdition_CUSA01111.xml` | one resolution: `3e86be10` 1440p, `b5ab6c71` 1440p 60fps, `c0f65a30` 1620p, `9d79ba91` 1620p 60fps, `fdba4900` 1800p, `9cca9281` 1800p 60fps, `bdb7bc89` Native 4K or `099ca82e` Native 4K 60fps | 1440p to native 4K. The 60fps entries add triple buffering (a third swapchain target, back buffer index cycling 0-2, vsync on) and illusion's 60 FPS Unlock; video heap +512 MiB. Every entry also fixes a game race that crashed the load at random (a thread object on the stack, moved to static memory) |
| Uncharted: The Nathan Drake Collection 01.02 | `UnchartedTheNathanDrakeCollection_CUSA02320.xml` | one resolution per game: Uncharted 1 (`eboot.bin`) `d0451c04` 1440p, `5c867f88` 1620p, `c03b7ff4` 1800p `587bcd5d` Native 4K or `67b4a754` Native 4K with 8GB Mode; Uncharted 2 (`big2-ps4_Shipping.elf`) `c44dac36`, `25f016fa`, `2fad03e6` or `e493ae0f` or `78b0f5c6` (Native 4K with 8GB Mode); Uncharted 3 (`big3-ps4_Shipping.elf`) `0f645e9f`, `a18bfaf7`, `0c95d93f` or `efebe61e` or `450336ab` (Native 4K with 8GB Mode) | 1440p, 1620p, 1800p or native 4K. The collection menu runs in Uncharted 1, and Uncharted 2 and 3 inherit its PS4 Pro mode, so their entries need an Uncharted 1 entry on. The game asks for 472 MiB of extra direct memory at most (more fails); 1800p and 4K take the unused part of the Render Buffer pool, and 4K also 200 MiB (Uncharted 3: 300 MiB) of the texture pool, for the render target heap (about 1370 MiB used at 4K). The Native 4K entries with 8GB Mode keep the full texture pool and grow the render target heap (Uncharted 1 and 2: +256 MiB, Uncharted 3: +128 MiB, held below the 4 GiB Garlic window) |
| The Evil Within 01.06 | `TheEvilWithin_CUSA00203.xml` | one resolution: `c55a4799` 1440p, `0666b089` 1800p or `2c3f275d` Native 4K with 8GB Mode | 1440p, 1800p, or native 4K with 8GB Mode. Display buffers in ALT layout with their texture descriptors also marked ALT (the loading screen reads the display), fast clear and HTILE off, Pro-mode hang fixes (texture/buffer memory type bits, occlusion predication), scene targets and fixed 1920x1080/1920x768 sizes scaled. Render targets live in the Garlic video memory pool (2390 MiB): +512 MiB at 1440p, +768 MiB with the CPU map heap 256 MiB smaller at 1800p (the stock budget allows about +576 MiB), +1536 MiB at 4K (8GB Mode) |
| The Evil Within 2 01.05 | `TheEvilWithin2_CUSA06166.xml` | one resolution: `90e93e3c` 1620p, `ada52d0c` 1800p or `87fcbad8` Native 4K with 8GB Mode | native PS4 Pro game: the Pro-mode render size index (0x112d97e, table of sizes: 21 = 2880x1620, 20 = 3200x1800, 16 = 3840x2160) replaces 2240x1260. Render targets live in the shared video memory pool (2560 MiB + 512 in Pro mode): +256 MiB at 1620p and 1800p (the stock budget leaves about 300 MiB), +1000 MiB at 4K (8GB Mode, 4072 MiB) |
| Dragon's Dogma: Dark Arisen 01.01 | `DragonsDogmaDarkArisen_CUSA07340.xml` | `a5e8351f` Native 4K | native PS4 Pro game: its Pro-mode display and render size (0x148fb37, 2560x1440) becomes 3840x2160; the game's fixed memory already fits 4K |
| Mad Max 01.06 | `MadMax_CUSA00054.xml` | one resolution: `74bf2089` 1440p, `b783d799` 1800p or `f78e2210` Native 4K | display buffers in ALT layout (CB_COLOR_INFO bit 31 and slice) with the ALT size, flipMode 1, fast clear off, HTILE off in Gnm and in the game's own depth surface setup, which sets it again; renderer and display default sizes (1920x1080) replaced |
| inFAMOUS Second Son 01.07 | `inFAMOUSSecondSon_CUSA00004.xml` | `9007b85c` 4K checkerboard | native PS4 Pro game: its Pro render size 3200x1800 (checkerboard buffer 1600x1800) becomes 3840x2160 (1920x2160), at startup and in the mode setter that runs when the settings load |
| NieR:Automata 01.06 | `NieRAutomata_CUSA04480.xml` | one resolution: `883a66da` 1440p, `764a82fa` 1620p or `4b0691ca` 1800p | native PS4 Pro game: its Pro render and display size (1920x1080) becomes the target size; the render-target heap (GRAPHIC VRAM) and its parent pool grow by 336 MiB, all the direct memory the game leaves free; a frame setup thread waits for the middleware object the slower boot creates later. Native 4K display hangs the GPU in gameplay (cause not found) |
| Watch Dogs 01.05 | `WatchDogs_CUSA00021.xml` | one resolution: `7370d094` 1440p, `8d0034b4` 1620p, `b04c2784` 1800p or `f041d20d` Native 4K, plus `007c637a` 60 FPS (alone or with any of them) or `ea7939f7` 60 FPS (triple buffering, needs one of them) | display buffers in ALT layout set by a command-buffer walker on both submit calls, flipMode 1, fast clear off, HTILE off; HTILE-only depth clears redirected to a real D32 clear, MSAA depth included (all samples); legacy texture memory-type bits cleared; renderer GPU pool +384 MiB (+512 MiB at 4K) and the fixed 210 MiB render-target heap grown to 448/544/600/768 MiB, which otherwise overlaps live targets above 1080p; 60 FPS sets the video flip rate to 60 Hz (0x1e3669a); 60 FPS (triple buffering) also adds a third display buffer (display object 0x98 -> 0xe0 bytes, index 0-2, RegisterBuffers with three), whose code is in each resolution entry |
| Dishonored Definitive Edition 01.03 | `Dishonored_CUSA02218.xml` | one resolution: `66ebdd3c` 1440p, `a5d72a2c` 1800p or `e5dadfa5` Native 4K, plus `ffe2345f` 60 FPS (triple buffering, alone or with any of them) | Unreal Engine 3: GScreenWidth/Height (static data) becomes the target size and feeds the engine resolution, viewport and display; display buffers in ALT layout with sizefix while they are created, flipMode 1, fast clear off, HTILE off in the Gnm depth constructor and in the engine surface setup; 1800p and Native 4K grow the video memory pool by 384 MiB; triple buffering sets 3 display buffers in the device constructor (every user of the count is generic) and flip rate 0 |
| Dishonored 2 01.07 | `Dishonored2_CUSA03642.xml` | one resolution: `5b668a51` 1620p, `662a9961` 1800p or `26276ce8` Native 4K, plus `d61add9f` 60 FPS and `f03d75db` Skip intro videos (alone or with any of them) | native PS4 Pro game: the Pro output size (2560x1440 on a 4K TV) becomes the target size; Native 4K pins the dynamic resolution floor (rs_minimumResolutionScale) at 1.0; the TXAA current-frame weight goes from 0.1 to the engine cap 0.2; 60 FPS sets flip rate 0 (the game already has 3 display buffers); the intro videos are not queued |
| Prey 01.12 | `Prey_CUSA04482.xml` | one resolution: `34074c89` 1620p, `094b5fb9` 1800p or `4946aa30` Native 4K, plus `b97b1b47` 60 FPS and `9f5cb303` Skip intro videos (alone or with any of them) | native PS4 Pro game, all entries patch `PreyDll.prx` (applied by the manager, 0.5.0+): Pro output and render size set in the size function, overload scene manager minimum scale 1.0, WriteCombined heap extra x4 (x8 for Native 4K), r_Width/r_Height registered at the target size and protected from neo.cfg (flag 0x40000 plus a Set(int) guard), so in-world touch screens map the cursor correctly; 60 FPS sets flip rate 0; the logo and legal videos are not queued |
| Spyro Reignited Trilogy 01.03 (APP_VER 01.00, VERSION 01.03) | `SpyroReignitedTrilogy_CUSA12125.xml` | one resolution: `e4ba9c70` 1620p, `d9f68f40` 1800p or `99fb7ac9` Native 4K | native PS4 Pro game (Unreal Engine 4): its `PS4_Neo` device profile sets `r.ScreenPercentage=133.33` on a 1920x1080 output (2560x1440 render). The 3840x2160 display size, which the engine only uses with `r.PS4Allow4kOutput` on, is enabled in both copies of the display size getter (0x122734a, 0x1228a2e). The screen percentage fraction getter (0x2b5dc66) returns a fixed 0.75, 0.8333333 or 1.0 of that output |

- Each resolution entry already includes the Pro mode fixes. Enable only one per game (in a collection, one per
  game executable, its AppElf).
- Rivals: keep the community "Resolution Patch" disabled if it is in the patch file.
- JC3: the Offline mode entry is required. Without it the game loops on the online login.
- The XMLs only carry entries made in this project. The Bloodborne entries build on Kyo's resolution patch.

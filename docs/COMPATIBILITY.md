# Compatibility

Results of NeoEnabler patches on real consoles. Report yours with the
[Test result form](https://github.com/gasb07/NeoEnabler/issues/new?template=test-result.yml).

Status meanings:

- **Validated**: tested by the maintainer.
- **Reported**: at least one community report, not yet confirmed.
- **Confirmed**: reported by two or more independent users.
- **Untested**: no result yet.

## Firmware 13.20

All entries below are validated by the maintainer on firmware 13.20. Notes and frame rates are in
[GAMES.md](GAMES.md).

| Game (version) | Resolution entries | Status |
| --- | --- | --- |
| Battlefield 4 (01.24) | 1440p, 1620p, 1800p, 4K | Validated |
| Battlefield Hardline (01.09) | 1440p, 1620p, 1800p, 4K | Validated |
| Bloodborne (01.09) | 1440p, 1620p, 1800p (8GB Mode), Native 4K (8GB Mode) | Validated |
| Dishonored Definitive Edition (01.03) | 1440p, 1800p, Native 4K, 60 FPS (triple buffering) | Validated |
| Dishonored 2 (01.07) | 1620p, 1800p, Native 4K, 60 FPS, Skip intro videos | Validated |
| Driveclub (01.28) | 1440p, 4K (1620p render), 4K (1800p render), Native 4K (8 GiB) | Validated |
| Just Cause 3 (01.05) | 1440p, 4K | Validated |
| Killzone Shadow Fall (01.81) | 1440p, 1620p, 1800p, 4K | Validated |
| Need for Speed Rivals (01.04, 01.03) | 1440p, 4K | Validated |
| Prey (01.12) | 1620p, 1800p, Native 4K, 60 FPS, Skip intro videos | 1800p, Native 4K, 60 FPS and Skip intro videos validated; 1620p not tested yet |
| Resident Evil 6 (01.01) | 1440p, 4K | Validated |
| The Order: 1886 (01.02) | 4K, 4K up to 60 fps | Validated |
| Dark Souls III: The Fire Fades Edition (01.03) | 1440p, 1620p, 1800p, Native 4K (8GB Mode) | Validated |
| Batman: Arkham Knight (01.15) | 1620p, 1800p, Native 4K | Native 4K validated; 1620p and 1800p not tested yet |
| Sleeping Dogs: Definitive Edition (01.01) | 1440p, 1620p, 1800p, Native 4K, each also up to 60 fps | Native 4K and the 60 fps entries validated; 1440p, 1620p and 1800p not tested yet |
| Uncharted: The Nathan Drake Collection (01.02) | Uncharted 1, 2 and 3: 1440p, 1620p, 1800p, Native 4K, Native 4K (8GB Mode) | Uncharted 1 and 3 validated; Uncharted 2 runs in Native 4K, its 1440p, 1620p and 1800p not tested yet |
| The Evil Within (01.06) | 1440p, 1800p, Native 4K (8GB Mode) | Native 4K validated; 1440p and 1800p reach gameplay |
| The Evil Within 2 (01.05) | 1620p, 1800p, Native 4K (8GB Mode) | Native 4K and 1800p reach gameplay; 1620p not tested yet |
| Dragon's Dogma: Dark Arisen (01.01) | Native 4K | Validated |
| Mad Max (01.06) | 1440p, 1800p, Native 4K | Native 4K validated; 1440p and 1800p reach gameplay |
| inFAMOUS Second Son (01.07) | 4K checkerboard | Validated |
| NieR:Automata (01.06) | 1440p, 1620p, 1800p | Validated |
| Watch Dogs (01.05) | 1440p, 1620p, 1800p, Native 4K, 60 FPS, 60 FPS (triple buffering) | Validated |
| Spyro Reignited Trilogy (01.03) | 1620p, 1800p, Native 4K | 1800p and Native 4K validated; 1620p not tested yet |

## GPU boost

GPU boost is validated by the maintainer on 13.20 and confirmed working on 13.60
through a community result relayed by the maintainer on 2026-10-20. The 13.60
report did not specify target or measured clocks. Other firmwares remain untested.

## Other firmwares

Pro mode is supported from 4.00 to 13.60, and the no-HV 8 GiB budget has profiles for 13.20 and 13.60. Community reports are listed below; one successful report does not validate every game or launch.

| Firmware | Pro mode patches | 8 GiB entries (Bloodborne Native 4K, Driveclub Native 4K) |
| --- | --- | --- |
| 4.00 - 13.00 | Untested | Not supported |
| 13.60 | Reported: Bloodborne Native 4K | Reported: Bloodborne reached gameplay; panel showed 8 GiB enabled. Other games untested |

## Community reports

| Firmware | Game | Entry | Result | Reports |
| --- | --- | --- | --- | --- |
| 13.60 | Bloodborne (version not supplied) | Native 4K, 8 GiB | Reported: gameplay reached, 8 GiB enabled in panel | 1 |

Reported through the maintainer on 2026-10-20. Raw status/logs, a full backing test and restoration after closing
were not supplied for this community test. The earlier idle "not supported" label did not prevent the successful launch.

| Game and entry | Firmware | Result | Reports | Status |
| --- | --- | --- | --- | --- |

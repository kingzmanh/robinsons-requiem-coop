# Robinson's Requiem – Co-op Plan

Date: 2026-11-07. Everything for the mod lives in this folder.

## 1. What the game is (findings)

| Fact | Evidence |
|---|---|
| GOG ships the original 1994 **DOS** game run by DOSBox 0.74-2. `START.EXE` is a 32-bit DOS program (FlashTek X-32 extender, Zortech C + hand-written asm). | `original/`, MZ + X-32 strings |
| The exe is only an **interpreter** for Silmarils' **ALIS** script VM. All gameplay (hero, body/medical system, inventory, monsters, maps, menus) is in the 124 `*.IO` script files. | strings `programme main attendu`, `MAIN.IO`; 124 scripts unpacked |
| `S.TST` is a **debug build of the same interpreter** with CodeView symbols: 4,092 names (3,134 code labels). | `tools/cvdump.py`, `analysis/symbols.txt` |
| The open-source **maestun/alis** (MIT, C + SDL2, active Oct 2026) reimplements ALIS and lists Robinson's Requiem PC as *Playable*, including the DOS 3D landscape renderer (`rrq3d_dos.c`). **509** of its function names match the original interpreter's names one to one. | `reference/alis`, name comparison |
| Engine version for this data: ALIS v3.1 (`version 31`), PC, little-endian. | `rrq_unpack` |
| Random numbers: one 16-bit seed, `seed = seed*0x7ab7 - 0x77f` (`ornd`/`calcrnd`, retail `0x40b358`). No hardware timer in it. | REA on `START.EXE`, `opernames.c:813` |
| Other outside inputs to the VM: mouse/keys/joystick opcodes, `cgettime` (wall clock), `alis.timeclock` (frame clock), `sys_random()` when a script seeds with 0. All live in our engine code, so we control them. | `opcodes.c`, `alis.c` |
| Entities are actors: each running script has its own variable block ("vram"); they talk by message queues (`csend`). Shared state lives in the **main script's variables** (`omain*` / `smain*` opcodes). | `opernames.c`, `opcodes.c` |

### Where the hero's state lives (the key co-op fact)
The game ships its own developer debug screen (`BARATIN*.IO`). It labels **71 hero variables**: `tools/debugvars.py` → `analysis/hero_vars_debugscreen.tsv`.

- **Body and condition = global main variables** (words around `0x192c–0x1a94`): blood `SANG 0x1932`, water `EAU 0x1936`, bag `SAC 0x1938`, energy `ENER 0x1942`, fatigue `FAT 0x193e`, mental state `PSY 0x1944`, fever `FIEVRE 0x192c`, body temperature `TEMPEX 0x1946`, clothes `VET 0x194c`, malaria `PALU 0x1952`, sleep `SOMMEIL 0x19dc`, drunk `IVRE 0x19e0`, …
- **Illness and wound tables** = main int tables `0x0ec0[]` (venom, sunstroke, stomach, flu, …) and `0x0e92[]` (wounds).
- **Equipment/inventory flags** = main bytes `0x39b4–0x3be2` (lamp, gourd, bag, raft, gas, atomic, light, …). The full inventory list is not mapped yet (Phase 1).
- **Position and view angle** = variables of the **hero entity** (`CX/CY/CZ` at +0/+8/+0x10, `ACX/ACY/ACZ` at +0x18/+0x20/+0x28). Scripts reach it through an entity-id slot.

So the game has **one global "player record"** plus one hero entity. Co-op means turning that into one record and one hero entity **per player**.

## 2. Approach

**Base: our own fork of maestun/alis** (native C, SDL2, MIT). No DOSBox and no patched DOS exe. The original `START.EXE`/`S.TST` + Ghidra/REA stay as the reference for checking behaviour. The game data stays the user's own GOG files.

Co-op is added **inside the interpreter**, not by rewriting the scripts:

1. **Player contexts.** Split main variables into *world* (shared: quests, map, time, weather, monsters) and *player* (body, illness, wounds, inventory, equipment, camera/UI state). Each player gets their own copy of the player ranges and their own hero entity.
2. **Context swap scheduler.** Each VM tick runs world entities once. Then for each player, it swaps that player's record into main memory and runs that player's hero, UI and view scripts. A swap is a memcpy of a few KB, so it's cheap.
3. **Monsters / NPCs.** They read "the hero" through the main hero-entity slot. Before a monster runs, we swap in the context of the player it's dealing with (nearest player, or its current target). Dialogue and trading happen in the context of the player who started them.
4. **Seeing each other.** P2 shows up in P1's 3D world as an entity using a human sprite set from the game data, and the other way round.
5. **Two screens.** Each player has their own screen buffer (3D view, inventory, body/medical screens, map). Both are drawn every tick, so the simulation stays identical. A machine *shows* only its local player's screen.
6. **Networking: deterministic lockstep.** Both PCs run the same simulation from the same save + seed and exchange only per-tick input (mouse/keys per player). The engine's clock, RNG seed and wall-clock time come from the session, not the local PC. A per-tick checksum of VM memory detects desyncs, and a resync sends a full VM state (the engine already has `alis_save_state`). This is the same model as Doom-era netplay. It needs no state streaming, and it suits a deterministic interpreter.
7. **Bonus from the same design:** a local split-screen mode (both screens side by side, two mice or mouse + gamepad). It's also the easiest way to test.

### P2 rules (defaults, adjustable)
- P2 has **their own character record**: body, health, illnesses, wounds, needs, inventory and equipment, all starting fresh like a new game.
- Shared: the world, story flags, map knowledge, time of day, weather, monsters, items left on the ground.
- Items can be given between players (drop/pick up at first, a give action later).
- Save = one co-op save file: world + both player records + both hero entities. Single-player saves load as P1, and P2 starts fresh.
- If one player dies, they become a spectator until the other uses a save or reaches a revive point. The exact rule is decided in Phase 4.

## 3. Phases

**Phase 0 – Analysis (done today)** – see section 5.

**Phase 1 – Reference and variable map**
- Build our fork: fix the missing `alisdis` target (done locally), confirm the original game runs start to finish in the engine, and fix any engine bugs against DOSBox + the original exe.
- Add a static **ALIS disassembler** using the engine's own opcode tables, and disassemble all 124 scripts.
- Add **main-variable write tracing**: which script/entity writes each main variable. Play test scenes (eat, drink, get hurt, pick up and drop, sleep, change area, talk) and classify every main variable as WORLD or PLAYER → `analysis/player_vars.tsv`.
- Find the hero entity script, the inventory data structures (`OBJET.IO`, `BODY.IO`, `HEROS.IO`, `ROBINSON.IO`) and the UI screens.

**Phase 2 – Two player records, one machine**
- Player-context data structure, swap scheduler, second hero entity.
- Separate input routing (P1/P2 devices).
- Local split-screen debug view.

**Phase 3 – Interaction**
- P2 visible as an entity in the 3D world, and monster targeting per player.
- Dialogues/trades bound to the player who started them, item hand-off, co-op save/load.

**Phase 4 – Online co-op**
- Lockstep netcode (UDP; host/join by IP first, LAN discovery second). Virtual clock/RNG/time, input delay, checksums, resync with a full state.
- Join flow: the host picks a save, the client gets the full state, then play starts.
- Death and respawn rules.

**Phase 5 – Release**
- One prebuilt installer (`Robinsons-Requiem-Coop-Setup.exe`) that points at the user's GOG folder, with icon, fullscreen by default (Alt+Enter toggles) and no game files inside.
- GitHub repo (private first) + a Release with the installer.

**Later (separate task, after co-op works) – modern display**
- Higher internal resolution for the 3D landscape, a widescreen FOV, and a native GPU renderer. Each is an **option that can be turned on or off**, like the Ghoulies port.

## 4. Lessons applied from earlier ports (Aliens, KOTOR, Arx)
- Analyse first (all functions and scripts are mapped) before changing code. Do the user's steps in their order, and nothing extra.
- Native code, no emulation layers. DOSBox is only a reference.
- One simple installer, prebuilt, with icon and fullscreen, and it goes in the GitHub Release. Game data is never in the repo.
- Test runs stay hidden and muted, with no popups or window flashing on the user's screen.
- Use the Edit tool for multi-line code changes. Bash heredocs mangle `\n`.

## 5. What's in this folder

| Path | What |
|---|---|
| `original/` | Copies of `START.EXE`, `S.TST`, `START.STP`, DOSBox configs |
| `game/` | Working copy of the game data (scripts + DAT), the engine's run folder |
| `reference/alis/` | maestun/alis clone (local patch: optional `alisdis` in CMakeLists) |
| `build/` | Engine debug build (`alis-debug/alis.exe`), `rrq_unpack` |
| `tools/cvdump.py` | CodeView NB05 parser (fixes Zortech's 16-bit size wrap) |
| `tools/extract32.py` | Extracts the flat 32-bit code+data from both exes, resolves symbol addresses |
| `tools/transfer_names.py` | Carries debug names over to the retail exe (2,759 / 3,134) |
| `tools/wrap_pe.py` | PE wrapper with named exports, so REA (Windows) can analyse the exes |
| `tools/ghidra/RRQSetup.java`, `RRQExport.java` | Ghidra headless layout + decompile export |
| `tools/rrq_unpack/` | Unpacks all `.IO` scripts using the engine's unpacker |
| `tools/debugvars.py` | Extracts the hero variable map from the built-in debug screens |
| `analysis/ghidra_out/` | Decompiled C, functions, calls, strings for both builds (2,688 / 2,482 functions) |
| `analysis/ghidra_project/` | Ghidra project (open via `C:\Users\Msi\Tools\rrq_coop`, which has no apostrophe) |
| `analysis/ghidra_in/*_pe.exe` | REA-ready PE wrappers (VA = 0x401000 + flat offset) |
| `analysis/graph_interp`, `graph_alis`, `graph_merged.json` | graphify graphs: original interpreter (2,813 nodes), ALIS source, merged (5,257 nodes) |
| `analysis/scripts_unpacked/` | All 124 scripts unpacked + `index.tsv` |
| `analysis/hero_vars_debugscreen.tsv` | 71 hero state variables → addresses |

Gotchas: Ghidra project paths and REA junctions — Ghidra rejects the apostrophe in the folder name, so use the junction `C:\Users\Msi\Tools\rrq_coop`. REA rejects junctions, so use the real `E:\…` path. REA on Windows is read-only, so names reach it through the PE export table. Build with MSYS2 mingw64 (`C:\msys64\mingw64\bin` first on PATH) and the junction path.

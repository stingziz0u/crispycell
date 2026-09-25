# CrispyCell

A native homebrew port of [Crispy Doom](https://github.com/fabiangreffrath/crispy-doom)
for the PS3 — Doom, Heretic, Hexen and Strife.

## Lineage

Crispy Doom is a limit-removing enhanced source port of Doom, Heretic, Hexen
and Strife, built on Chocolate Doom's faithful reimplementation of the
original 1997 source release. This project adds a full PS3 platform layer on
top of it: RSX video, libaudio sound and music, DualShock 3 input, and a WAD
launcher built for a console.

```
Doom (id Software, 1997) -> Chocolate Doom -> Crispy Doom -> CrispyCell (this project)
```

## Features

- **Four games:** Doom, Heretic, Hexen and Strife, each as its own package.
- **Video:** 640x400 internal render, GPU-scaled to the display by the RSX
  with triple buffering. Correct aspect ratio with pillarboxing, plus
  optional 16:10, 16:9 and 21:9 widescreen that widens the field of view
  rather than stretching the picture.
- **TV Screen Fit:** a setting from 70 % to 100 % that shrinks the picture
  for TVs that crop the edges (overscan), or fills a monitor edge to edge.
- **Audio:** native 48000 Hz through libaudio. OPL3 music emulation and a
  from-scratch SFX mixer, sharing a single audio port. Strife's spoken
  dialogue plays in full.
- **Input:** DualShock 3, both sticks, every action reachable from the
  pad and rebindable in the configuration files. Adjustable
  turn/move/look sensitivity from the in-game menu.
- **Automap zoom on the pad** (D-pad up/down while the map is open), kept
  from level to level.
- **WAD launcher:** scans for what you installed and lists each playable
  thing as its own entry — base games and add-ons alike — pairing every
  add-on with the base game it needs. Skipped entirely when there is only
  one thing to play. The WADs listed under Installation are the ones I
  tested personally; anything else the engine supports should work too.
- **Mods:** multi-file mods and total conversions (several WADs plus
  DeHackEd patches, like Aliens TC) go in a folder each under `mods/` and
  get their own tab in the launcher and their own savegames.
- **Strife without a keyboard:** character and savegame names are typed on
  the PS3's own on-screen keyboard; dialogue is answered with the pad.
- **Gamma correction from the menu**, since F11 is not reachable without a
  keyboard.
- Clean exit to the XMB from the in-game Quit option or the PS button.

## Requirements

- A PS3 capable of running homebrew (HEN or CFW).
- Your own legally-owned game data. No commercial WAD is included in this
  repository or in any release build. The Doom package does ship the
  freely redistributable `DOOM1.WAD` shareware.
- To build from source: the ps3dev PSL1GHT toolchain (`ppu-gcc`, PSL1GHT
  SDK, `make_self_npdrm`, `pkg.py`). No SDL2 is needed — this port replaces
  it entirely.

## Installation

Four separate packages, one per engine, each with its own APPID so they
coexist on the XMB and keep their own savegames and configuration.

| Package | APPID | Installs to |
|---|---|---|
| `CrispyDoom-1.1.pkg` | `CRISPDOOM` | `/dev_hdd0/game/CRISPDOOM/USRDIR/` |
| `CrispyHeretic-1.1.pkg` | `CRISPHERE` | `/dev_hdd0/game/CRISPHERE/USRDIR/` |
| `CrispyHexen-1.1.pkg` | `CRISPHEX1` | `/dev_hdd0/game/CRISPHEX1/USRDIR/` |
| `CrispyStrife-1.1.pkg` | `CRISPSTRF` | `/dev_hdd0/game/CRISPSTRF/USRDIR/` |

Install from the XMB, then FTP your WADs into that folder. The launcher
picks them up on the next boot — no configuration file to edit.

Filenames are matched case-insensitively, but the PS3 filesystem itself is
case-sensitive — if a WAD is not being found, that is the first thing to
check.

### Doom

Drop any of these into `CRISPDOOM/USRDIR/`:

```
DOOM.WAD          The Ultimate DOOM
DOOM2.WAD         DOOM II: Hell on Earth
TNT.WAD           Final Doom: TNT Evilution
PLUTONIA.WAD      Final Doom: Plutonia
DOOM1.WAD         DOOM shareware (already in the package)
FREEDOOM1.WAD     Freedoom: Phase 1
FREEDOOM2.WAD     Freedoom: Phase 2
FREEDM.WAD        FreeDM
HACX.WAD          HacX
```

Add-ons go in the same folder and appear as their own entries, each loaded
on top of the base game it needs:

```
SIGIL.WAD         SIGIL           (needs DOOM.WAD)
SIGIL2.WAD        SIGIL II        (needs DOOM.WAD)
NERVE.WAD         No Rest for the Living   (needs DOOM2.WAD)
MASTERLEVELS.WAD  Master Levels            (needs DOOM2.WAD)
CHEX.WAD          Chex Quest
CHEX2.WAD         Chex Quest 2    (needs CHEX.WAD)
```

`SIGIL_SHREDS.WAD` and `SIGIL_II_MP3.WAD` are not listed separately — they
are soundtrack replacements and load automatically alongside their episode
when present.

Some releases ship `NERVE.WAD` and `MASTERLEVELS.WAD` marked as IWADs
rather than PWADs (the BFG and Unity re-releases do). Those show up as
standalone games instead of add-ons. Both work.

### Heretic

Into `CRISPHERE/USRDIR/`:

```
HERETIC.WAD       Heretic: Shadow of the Serpent Riders
HERETIC1.WAD      Heretic shareware
```

### Hexen

Into `CRISPHEX1/USRDIR/`:

```
HEXEN.WAD         Hexen: Beyond Heretic
HEXDD.WAD         Deathkings of the Dark Citadel   (needs HEXEN.WAD)
```

### Strife

Into `CRISPSTRF/USRDIR/`:

```
STRIFE1.WAD       Strife: Quest for the Sigil
VOICES.WAD        spoken dialogue (optional)
```

`VOICES.WAD` is not a game of its own and does not appear in the launcher: 
the engine loads it by itself when it sits next to `STRIFE1.WAD`.
Without it the game plays fine, with subtitles only.

### Mods

Anything more than a single WAD goes in its own folder under `mods/`, one
folder per mod:

```
CRISPDOOM/USRDIR/mods/ALIENS_TC/    <- every .wad and .deh/.bex of the mod
CRISPDOOM/USRDIR/mods/MYMOD/
```

When `mods/` has at least one mod, the launcher shows two tabs, **GAMES**
and **MODS**; switch with L1/R1 (or D-pad left/right). Each folder is one
entry, named after the folder (underscores read as spaces), with the base
game it will run on shown to the right.

- **What loads:** every `.wad` in the folder, in alphabetical order, then
  every DeHackEd patch: `.deh` or `.bex` in Doom and Strife, `.hhe` or
  `.deh` in Heretic. Hexen has no DeHackEd support, so patches are ignored
  there. `DEHACKED` lumps inside the WADs load by themselves, as usual in
  Crispy.
- **Base game:** picked from the maps inside, like add-ons (`ExMy` needs
  an episodic IWAD such as `DOOM.WAD`, `MAPxx` a Doom II one). A mod with
  no maps at all (graphics or sound replacements) still gets listed, on
  the package's default base game (Doom II in the Doom package, if
  installed). An IWAD inside the folder makes the mod a standalone game
  that runs on it.
- **Savegames:** each mod saves to `USRDIR/savegames/mod-<folder>/`, so
  its saves never mix with the base game's.

When the defaults are not right, an optional `mod.txt` in the folder
overrides them:

```
name = Aliens TC     # label in the launcher
iwad = DOOM.WAD      # base game (must be installed)
file = ALIENS.WAD    # load order: with any file line, only the
file = ALIENS.DEH    #   files listed are loaded, in this order
```

If a mod ships both a `.deh` and a `.bex` of the same patch, keep only one
in the folder (or list one in `mod.txt`): otherwise both are applied, the
second on top of the first.

If a mod does not show up, `USRDIR/crispy_log.txt` says why (missing base
game, no WADs, a `file` from `mod.txt` that is not in the folder).

## Controls

### Doom

| Action | Button |
|---|---|
| Move | Left stick |
| Turn / look | Right stick |
| Fire | R2 |
| Run | L2 |
| Use | Cross |
| Jump | Square |
| Strafe left / right | L1 / R1 |
| Next / previous weapon | D-pad up / down |
| Quicksave / quickload | Triangle / Circle |
| Automap | Select |
| Automap zoom in / out | D-pad up / down (map open) |
| Menu | Start |

Jumping only does something with "Allow Jumping" enabled in Crispness.

### Heretic and Hexen

| Action | Button |
|---|---|
| Move | Left stick |
| Turn / look | Right stick |
| Fire | R2 |
| Run | L2 |
| Use | Cross |
| Use inventory item | Square |
| Jump (Hexen) | Circle |
| Inventory left / right | L1 / R1 |
| Fly down / up | L3 / R3 |
| Center flight | Triangle |
| Next / previous weapon | D-pad up / down |
| Automap | Select |
| Automap zoom in / out | D-pad up / down (map open) |
| Quicksave / quickload | unbound (see Rebinding) |
| Menu | Start |

Heretic has no jump; Circle does nothing there.

### Strife

| Action | Button |
|---|---|
| Move | Left stick |
| Turn / look | Right stick |
| Fire | R2 |
| Run | L2 |
| Use / talk | Cross |
| Use inventory item | Square |
| Jump | Circle |
| Inventory left / right | L1 / R1 |
| Objectives (hold) | Triangle |
| Use a medkit | D-pad left |
| Keys (hold; press again for the next page) | D-pad right |
| Next / previous weapon | D-pad up / down |
| Automap | Select |
| Automap zoom in / out | D-pad up / down (map open) |
| Quicksave / quickload, drop item | unbound (see Rebinding) |
| Menu | Start |

**Conversations** are a menu: D-pad to pick an answer, Cross to say it. To
leave, pick the goodbye answer or press Start. The button that picked an
answer is ignored until it is released, so it cannot talk to the same
character again by accident.

**Naming your character:** Strife asks for a name when a new game starts,
and for every save. The PS3's own on-screen keyboard opens for it — type
the name and confirm, or cancel to go back to the slot list. Names are
stored in upper case; characters the game font cannot draw (accents, for
example) are dropped, and an empty name becomes `STRIFEGUY <slot>`. If the
keyboard cannot open at all, the slot is pre-filled with that name and
Cross accepts it; the reason is in `crispy_log.txt`.

### In menus

| Action | Button |
|---|---|
| Navigate | D-pad or left stick |
| Confirm | Cross |
| Back | Circle |
| Open / close | Start |
| Savegame page (Doom, Heretic, Hexen) | D-pad left / right |

## Rebinding

There is no in-game rebinding screen. The buttons are ordinary Crispy
settings, so they can be edited in the configuration files under
`USRDIR/`. Each game has two files, and the settings are split between
them. The files are created the first time the game is quit, so start and
quit once before editing.

**Main file:** `default.cfg` (Doom), `heretic.cfg`, `hexen.cfg`,
`strife.cfg`

| Key | Default | Button |
|---|---|---|
| `joyb_fire` | 7 | R2 |
| `joyb_use` | 0 | Cross |
| `joyb_speed` | 6 | L2 |
| `joyb_strafe` | -1 | disabled |

**Crispy file:** `crispy-doom.cfg`, `crispy-heretic.cfg`, `crispy-hexen.cfg`,
`crispy-strife.cfg`

| Key | Doom | Heretic / Hexen | Strife |
|---|---|---|---|
| `joyb_jump` | 1 (Square) | 2 (Circle) | 2 (Circle) |
| `joyb_strafeleft` | 4 (L1) | -1 | -1 |
| `joyb_straferight` | 5 (R1) | -1 | -1 |
| `joyb_nextweapon` | 12 (D-pad up) | 12 | 12 |
| `joyb_prevweapon` | 13 (D-pad down) | 13 | 13 |
| `joyb_menu_activate` | 11 (Start) | 11 | 11 |
| `joyb_toggle_automap` | 10 (Select) | 10 | 10 |
| `joyb_map_zoomin` | 12 (D-pad up) | 12 | 12 |
| `joyb_map_zoomout` | 13 (D-pad down) | 13 | 13 |
| `joyb_quicksave` | 3 (Triangle) | -1 | -1 |
| `joyb_quickload` | 2 (Circle) | -1 | -1 |
| `joyb_useartifact` | — | 1 (Square) | 1 (Square) |
| `joyb_invleft` | — | 4 (L1) | 4 (L1) |
| `joyb_invright` | — | 5 (R1) | 5 (R1) |
| `joyb_flyup` | — | 9 (R3) | — |
| `joyb_flydown` | — | 8 (L3) | — |
| `joyb_flycenter` | — | 3 (Triangle) | — |
| `joyb_mission` | — | — | 3 (Triangle) |
| `joyb_usehealth` | — | — | 14 (D-pad left) |
| `joyb_invkey` | — | — | 15 (D-pad right) |
| `joyb_invdrop` | — | — | -1 |

The automap zoom buttons only zoom while the map is open, and only there do
they stop switching weapons. Bind them to something else and the D-pad
switches weapons on the map too.

Button indices:

```
0  Cross       6  L2         12  D-pad up
1  Square      7  R2         13  D-pad down
2  Circle      8  L3         14  D-pad left
3  Triangle    9  R3         15  D-pad right
4  L1         10  Select
5  R1         11  Start
```

`-1` disables an action. Valid values are 0 to 19.

Sensitivity lives in the Crispy file too and is adjustable from Options ->
Joystick Sensitivity: `joystick_turn_sensitivity`,
`joystick_move_sensitivity`, `joystick_look_sensitivity` (10 is 1.0x) and
`joystick_look_invert`.

Two things are not rebindable: menu confirm and back (Cross and Circle)
and the stick axes.

## TV Screen Fit

- Doom and Strife: Options -> Crispness -> page 1 (Rendering).
- Heretic and Hexen: Options -> More... -> TV Screen Fit.

It sets how much of the screen the picture may use, from 70 % to 100 % in
steps of 2; 90 % is the default and what 1.0 used. Lower it if the TV cuts
off the status bar or the edges of the menu; on a monitor, 100 % uses the
whole height. The change shows immediately. It is saved as
`ps3_screen_fit` in the game's Crispy file, and the launcher reads it from
there too.

This is not the classic Screen Size option, which trades the status bar
and view size inside the game's own picture.

## Known limitations

- **Savegames are shared between games that run on the same IWAD.** Chex
  Quest and SIGIL both run as `gamemission == doom`, so their saves land in
  the same folder as The Ultimate DOOM's and appear in each other's load
  menus. This is upstream behaviour — Crispy picks the savegame folder from
  the game mission rather than the WAD — and it is more visible here
  because the launcher makes switching easy. Mods in `mods/` are not
  affected: each has its own save folder.
- **No multiplayer.** The networking layer is a stub.
- **No mouse or keyboard**, even if one is plugged in.
- **Errors are logged, not shown.** A fatal error writes to
  `USRDIR/crispy_log.txt` and exits to the XMB with no message on screen.
  That log is the first place to look when something does not work. It
  is started fresh on every boot; the previous session is kept as
  `crispy_log.old.txt`, so a crash can still be read after relaunching.
- **Add-ons that change only textures or sounds are not listed** when
  dropped loose in `USRDIR/`: the launcher places an add-on by the `ExMy`
  or `MAPxx` maps inside it. Put them in a folder under `mods/` instead,
  with a `mod.txt` naming the base game if needed.

## Differences from upstream Crispy Doom

Beyond the platform layer, a few deliberate behaviour changes:

- **Automatic loading of SIGIL, SIGIL II, NERVE and MASTERLEVELS is
  disabled.** Upstream looks for them next to the IWAD and loads them
  silently. On a console where every WAD shares one folder, that means
  picking "The Ultimate DOOM" would quietly drag SIGIL in with it, title
  graphic and extra episodes and all. The launcher decides instead.
- **Enable VSync and Smooth Pixel Scaling are removed from the Crispness
  menu.** Both are SDL renderer concepts with no equivalent here: the RSX
  scans out in sync already and the blit is a fixed GPU scale.
- **Mouse Sensitivity is replaced by Joystick Sensitivity**, and the look
  axis no longer saturates — upstream clamps after scaling, which made the
  vertical slider do nothing past its midpoint.
- **Menu buttons are edge-triggered.** Upstream throttles repeats to about
  seven a second, which on a pad means holding a button walks through
  several menus before you let go.
- **Strife's keyboard-only actions are on the pad.** Objectives, keys,
  medkit and item drop are keys upstream; here they are buttons, sent as
  those keys going down and up, so the hold-to-show pop-ups behave as they
  do on a keyboard.

## Building from source

The `ps3/` directory holds everything specific to this port's build:

- **`crispy-ps3-toolchain.cmake`** — the CMake toolchain file. Points CMake
  at the PPU cross-compiler and sets the link libraries. Edit `PS3DEV` at
  the top to match your toolchain location.
- **`appid.mk`** — the single source of truth for which engine is being
  built and under what APPID. Read both by CMake (via `file(STRINGS)`) and
  by the packaging Makefile, so the paths compiled into the binary can
  never disagree with the package.
- **`build.sh`** — builds and packages one engine end to end:
  `./ps3/build.sh doom`, `heretic`, `hexen` or `strife`. Rewrites
  `appid.mk`, reconfigures, clears the object tree when the engine changes,
  builds, and packages `Crispy<Game>-1.1.pkg` in the project root.
- **`icons/`** — the `ICON0.PNG` for each engine's XMB entry (320x176).

`Makefile.crispypkg` in the project root packages an already-built `.elf`
into an installable `.pkg`, using `ppu_rules`' automatic targets. It does
not compile the engine.

`pkgfiles-<game>/USRDIR/` is what ships inside each package. Doom's holds
the shareware WAD; Heretic's, Hexen's and Strife's are empty, since their
IWADs are commercial.

```bash
mkdir build-ps3 && cd build-ps3
cmake .. -DCMAKE_TOOLCHAIN_FILE=../ps3/crispy-ps3-toolchain.cmake -DPS3_BUILD=ON
cd ..
./ps3/build.sh doom
```

Everything PS3-specific in the engine sits behind `PS3_BUILD`, and the
per-engine differences behind `PS3_GAME_DOOM`, `PS3_GAME_HERETIC`,
`PS3_GAME_HEXEN` and `PS3_GAME_STRIFE`. The new platform files are
`src/i_ps3video.c`, `src/i_ps3sound.c`, `src/i_ps3joystick.c`,
`src/i_ps3launcher.c`, `src/i_ps3stubs.c`, `src/i_ps3input.c` (which also
holds the on-screen keyboard), `src/net_ps3.c` and `opl/opl_ps3.c`.

Development diagnostics (audio block counters, launcher decisions, every
voice started) are compiled out of release builds; define
`PS3_VERBOSE_LOG` to get them back in `crispy_log.txt`.

## Credits

- [Crispy Doom](https://github.com/fabiangreffrath/crispy-doom) by Fabian
  Greffrath and contributors — the engine this project is built on.
- [Chocolate Doom](https://github.com/chocolate-doom/chocolate-doom) by
  Simon Howard and contributors, which Crispy Doom is based on, including
  the Strife reconstruction.
- id Software, Raven Software and Rogue Entertainment, for the original
  games and source releases.
- [PSL1GHT](https://github.com/ps3dev/PSL1GHT) and the ps3dev toolchain.
- [Apollo Save Tool](https://github.com/bucanero/apollo-ps3) by bucanero,
  whose use of the system on-screen keyboard was the reference for
  Strife's name entry.

## License

GPLv2, inherited from Crispy Doom. See [COPYING.md](COPYING.md).

No commercial game data is included in this repository. You need your own
legally-owned copy of the games.

## AI disclosure

This project's code was written collaboratively with Claude (Anthropic),
working through this port with me in real time over many sessions. Every
bit of testing and debugging was done by me on real hardware.

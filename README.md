# CrispyCell

A native homebrew port of [Crispy Doom](https://github.com/fabiangreffrath/crispy-doom)
for the PS3 — Doom, Heretic and Hexen.

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

- **Video:** 640x400 internal render, GPU-scaled to the display by the RSX
  with triple buffering. Correct aspect ratio with pillarboxing, plus
  optional 16:10, 16:9 and 21:9 widescreen that widens the field of view
  rather than stretching the picture.
- **Audio:** native 48000 Hz through libaudio. OPL3 music emulation and a
  from-scratch SFX mixer, sharing a single audio port.
- **Input:** DualShock 3, both sticks, every button mapped. Adjustable
  turn/move/look sensitivity from the in-game menu.
- **WAD launcher:** scans for what you installed and lists each playable
  thing as its own entry — base games and add-ons alike — pairing every
  add-on with the base game it needs. Skipped entirely when there is only
  one thing to play. The WADs listed under Installation are the ones I
  tested personally; anything else the engine supports should work too,
  as long as it carries `ExMy` or `MAPxx` maps (see Known limitations).
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

Three separate packages, one per engine, each with its own APPID so they
coexist on the XMB and keep their own savegames and configuration.

| Package | APPID | Installs to |
|---|---|---|
| `CrispyDoom-1.0.pkg` | `CRISPDOOM` | `/dev_hdd0/game/CRISPDOOM/USRDIR/` |
| `CrispyHeretic-1.0.pkg` | `CRISPHERE` | `/dev_hdd0/game/CRISPHERE/USRDIR/` |
| `CrispyHexen-1.0.pkg` | `CRISPHEX1` | `/dev_hdd0/game/CRISPHEX1/USRDIR/` |

Install from the XMB, then FTP your WADs into that folder. The launcher
picks them up on the next boot — no configuration file to edit.

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

```
HERETIC.WAD       Heretic: Shadow of the Serpent Riders
HERETIC1.WAD      Heretic shareware
```

### Hexen

```
HEXEN.WAD         Hexen: Beyond Heretic
HEXDD.WAD         Deathkings of the Dark Citadel   (needs HEXEN.WAD)
```

Filenames are matched case-insensitively, but the PS3 filesystem itself is
case-sensitive — if a WAD is not being found, that is the first thing to
check.

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
| Menu | Start |

### Heretic and Hexen

| Action | Button |
|---|---|
| Move | Left stick |
| Turn / look | Right stick |
| Fire | R2 |
| Run | L2 |
| Use | Cross |
| Use inventory item | Square |
| Jump | Circle |
| Inventory left / right | L1 / R1 |
| Fly down / up | L3 / R3 |
| Center flight | Triangle |
| Next / previous weapon | D-pad up / down |
| Automap | Select |
| Menu | Start |

### In menus

| Action | Button |
|---|---|
| Navigate | D-pad or left stick |
| Confirm | Cross |
| Back | Circle |
| Open / close | Start |
| Savegame page | D-pad left / right |

Jumping only does something with "Allow Jumping" enabled in Crispness.

## Rebinding

There is no in-game rebinding screen. The buttons are ordinary Crispy
settings, so they can be edited in the configuration files under
`USRDIR/`, split across two files depending on the setting:

**`default.cfg`**

| Key | Default | Button |
|---|---|---|
| `joyb_fire` | 7 | R2 |
| `joyb_use` | 0 | Cross |
| `joyb_speed` | 6 | L2 |
| `joyb_strafe` | -1 | disabled |

**`crispy-doom.cfg`** (or `crispy-heretic.cfg`, `crispy-hexen.cfg`)

| Key | Doom | Heretic / Hexen |
|---|---|---|
| `joyb_jump` | 1 (Square) | 2 (Circle) |
| `joyb_strafeleft` | 4 (L1) | -1 |
| `joyb_straferight` | 5 (R1) | -1 |
| `joyb_nextweapon` | 12 (D-pad up) | 12 |
| `joyb_prevweapon` | 13 (D-pad down) | 13 |
| `joyb_menu_activate` | 11 (Start) | 11 |
| `joyb_toggle_automap` | 10 (Select) | 10 |
| `joyb_useartifact` | — | 1 (Square) |
| `joyb_invleft` | — | 4 (L1) |
| `joyb_invright` | — | 5 (R1) |
| `joyb_flyup` | — | 9 (R3) |
| `joyb_flydown` | — | 8 (L3) |
| `joyb_flycenter` | — | 3 (Triangle) |

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

Sensitivity lives in the same file and is adjustable from Options ->
Joystick Sensitivity: `joystick_turn_sensitivity`,
`joystick_move_sensitivity`, `joystick_look_sensitivity` (10 is 1.0x) and
`joystick_look_invert`.

Three things are not rebindable, because Doom has no configuration variable
for them: quicksave and quickload (Triangle and Circle), menu confirm and
back (Cross and Circle), and the stick axes.

## Known limitations

- **Savegames are shared between games that run on the same IWAD.** Chex
  Quest and SIGIL both run as `gamemission == doom`, so their saves land in
  the same folder as The Ultimate DOOM's and appear in each other's load
  menus. This is upstream behaviour — Crispy picks the savegame folder from
  the game mission rather than the WAD — and it is more visible here
  because the launcher makes switching easy.
- **No Strife.** The engine compiles and boots, but Strife asks you to type
  a character name before the first game and its NPC dialogue is answered
  with number keys. Without a keyboard there is no way past that.
- **No multiplayer.** The networking layer is a stub.
- **No mouse or keyboard**, even if one is plugged in.
- **Errors are logged, not shown.** A fatal error writes to
  `USRDIR/crispy_log.txt` and exits to the XMB with no message on screen.
  That log is the first place to look when something does not work.
- **Add-ons that change only textures or sounds are not listed.** The
  launcher decides which base game an add-on needs by looking for `ExMy` or
  `MAPxx` maps inside it; a WAD with neither cannot be placed.

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
  `./ps3/build.sh doom`, `heretic` or `hexen`. Rewrites `appid.mk`,
  reconfigures, clears the object tree when the engine changes, builds, and
  packages.
- **`icons/`** — the `ICON0.PNG` for each engine's XMB entry (320x176).

`Makefile.crispypkg` in the project root packages an already-built `.elf`
into an installable `.pkg`, using `ppu_rules`' automatic targets. It does
not compile the engine.

`pkgfiles-<game>/USRDIR/` is what ships inside each package. Doom's holds
the shareware WAD; Heretic's and Hexen's are empty, since their IWADs are
commercial.

```bash
mkdir build-ps3 && cd build-ps3
cmake .. -DCMAKE_TOOLCHAIN_FILE=../ps3/crispy-ps3-toolchain.cmake -DPS3_BUILD=ON
cd ..
./ps3/build.sh doom
```

Everything PS3-specific in the engine sits behind `PS3_BUILD`, and the
per-engine differences behind `PS3_GAME_DOOM`, `PS3_GAME_HERETIC` and
`PS3_GAME_HEXEN`. The new platform files are `src/i_ps3video.c`,
`src/i_ps3sound.c`, `src/i_ps3joystick.c`, `src/i_ps3launcher.c`,
`src/i_ps3stubs.c`, `src/i_ps3input.c`, `src/net_ps3.c` and
`opl/opl_ps3.c`.

## Credits

- [Crispy Doom](https://github.com/fabiangreffrath/crispy-doom) by Fabian
  Greffrath and contributors — the engine this project is built on.
- [Chocolate Doom](https://github.com/chocolate-doom/chocolate-doom) by
  Simon Howard, which Crispy Doom is based on.
- id Software and Raven Software, for the original games and source
  releases.

## License

GPLv2, inherited from Crispy Doom. See [COPYING.md](COPYING.md).

No commercial game data is included in this repository. You need your own
legally-owned copy of the games.

## AI disclosure

This project's code was written collaboratively with Claude (Anthropic),
working through this port with me in real time over many sessions. Every
bit of testing and debugging was done by me on real hardware.

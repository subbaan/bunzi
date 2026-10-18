# Bunzi development notes

How the game works inside, the developer options and the source layout. For
playing, see the [README](../README.md).

## How it draws

The game draws into a 480×270 software framebuffer, upscales it with
nearest-neighbour, and hands each frame to kitty through the
[graphics protocol](https://sw.kovidgoyal.net/kitty/graphics-protocol/) via
POSIX shared memory. Input uses kitty's keyboard protocol for real key
press/release events, plus SDL3 for game controllers.

## Debug keys

M cycles pixel transport (shm/direct/directz), G toggles who upscales (us or
kitty), F cycles target fps, Tab (gamepad Start) shows the debug overlay, and
1–9 jump straight to a level. Runs that used the debug
teleport (`BUNZI_DUMP_TX`, dump only) are marked practice and not recorded.

## Options

`--probe` reports terminal and controller capabilities, `--bench SECS`
autoplays and prints frame statistics, `--dump FILE` renders one frame to a
PPM, `--level N` skips the title and starts at level N, `--fps`, `--mode`,
`--scale`, `--assets DIR`. `BUNZI_DUMP_TITLE=SECS` with `--dump`
renders the title screen, and `BUNZI_DUMP_CONTROLS="ROW [SECS [MODE]]"`
the controls screen over it (row -1 the keyboard / gamepad tabs, 0–5 an
action, 6 the buttons; MODE `listen`, `pad` or `pad-listen`). See `--help`.

`--show-fps` shows the frame rate kitty is actually displaying in the
bottom-left corner (green at the target rate, amber below it).

## The opening

The opening (on by default when the title screen opens; `--no-intro` skips
it, `--intro` plays it even with `--level`) dissolves the terminal's own text
into the title screen at start (about 7 s: the terminal holds with the cursor glowing, then
a slow wave of characters lifts off as glowing letters and turns to sparks;
the logo drops in once the world is showing) and reassembles it on quit
(about 3 s). Nothing is screenshotted: the game draws a
transparent full-window image over kitty's normal screen, so the real text
shows through until each character's cell is covered and a pixel copy of the
character flies off. Character positions and colours come from kitty remote
control (`kitten @ get-text/get-colors` on `$KITTY_LISTEN_ON`, matched to
`$KITTY_WINDOW_ID`), are held in memory only, and the shell's screen is never
written to. Without remote control it falls back to a plain sweep. Any key
skips. `BUNZI_FX_DUMP=DIR` (developer opt-in) saves a few overlay frames.

## Scores

Scores are kept in `~/.local/share/bunzi/scores.txt` (or `$XDG_DATA_HOME/bunzi/`),
written through a temporary file and a rename. Each table belongs to one
level layout: editing a level starts it a fresh table, and the old one stays
in the file. The enemy time bonuses are per kind, in `KILL_BONUS` in
`src/game.c`.

## Layout

- `src/` – engine: `term.c` (tty, kitty keyboard), `kgfx.c` (kitty graphics
  output), `pad.c` (SDL3 gamepad), `assets.c` (PNG, atlas, sprite drawing),
  `level.c` (text levels), `game.c` (gameplay + rendering), `results.c` (results
  screen and Top 10), `title.c` (title screen), `controls.c` (key and button bindings,
  `~/.config/bunzi/controls.txt`, and the controls screen), `main.c` (loop).
- `assets/<level>/` – one folder per level with its `levelN.txt` and `gen/`:
  `sprites.png` + `sprites.txt` (the sprite atlas: name, x, y, w, h, anchor)
  and the parallax backgrounds (`bg_*.png`).
  1 `moonlit_ruins` (its atlas also holds the shared sprites: bunny, coins,
  effects), 2 `sunset_lake`, 3 `tropical_island`, 4 `crystal_caverns`.
- `assets/exit/`, `assets/effects/` – the burrow door, and the water squirt,
  splash and spray.
- `assets/title/` – the title screen's layers, logo and magic sparkles.
- `assets/controls/` – the controls screen's frame (SpriteCook, drawn at 2x).
- `assets/sounds/` – every effect in a few variants (`jump_a/_b/_c.wav` …), a
  night ambience loop and three music loops: `music_chip.wav` (in game),
  `music_title.wav` (the title screen) and `music_calm.wav` (a spare).
  `sounds.txt` maps game events to files and volumes; edit it to pick variants.
- `packaging/` – the AppImage build script, desktop entry and icon.
- `docs/` – these notes, the terminal-transition plan and the README's
  screenshots, rendered by the game's own `--dump`.

The art and sounds are ready-made in `assets/`; the game reads each level's
`gen/` folder, its level file and `assets/sounds/`.

Levels are plain text, one character per 16px tile; each file's legend is at
its top (`=` platform, `_` rope bridge, `~` water, `e` mushroom, `n` snail, `c` crab,
`j` jellyfish, `y` bird, `a` bat, `s` slime, `z` cave worm, `q` spiky fish …). `./bunzi --check` reports hanging decorations with
nothing to hang from.

## Packaging

`make appimage` builds an AppImage locally (`packaging/appimage.sh`), and the
`AppImage` workflow in `.github/workflows/` builds the release one on
Ubuntu 22.04 so it runs on older systems too.

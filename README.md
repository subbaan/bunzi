# Bunzi

A full-colour pixel-art platformer that runs **inside the kitty terminal**,
starring a white bunny in a red scarf.

![The title screen](docs/screenshots/title.png)

The game draws into a 480×270 software framebuffer, upscales it with
nearest-neighbour, and hands each frame to kitty through the
[graphics protocol](https://sw.kovidgoyal.net/kitty/graphics-protocol/) via
POSIX shared memory. Input uses kitty's keyboard protocol for real key
press/release events, plus SDL3 for game controllers.

Levels: 1 Moonlit Ruins (night), 2 Sunset Lake (dusk), 3 Tropical Island
(day), 4 Crystal Caverns (underground). The game opens on the title screen
(start, level choice, quit) with its own SID-style tune; Esc in a level returns to it.

Version: 0.11.4

## Screenshots

| | |
|---|---|
| ![Moonlit Ruins: the burrow door](docs/screenshots/moonlit_ruins.png) | ![Sunset Lake](docs/screenshots/sunset_lake.png) |
| ![Tropical Island: a jellyfish squirts](docs/screenshots/tropical_island.png) | ![Crystal Caverns: a spiky fish leaps](docs/screenshots/crystal_caverns.png) |
| ![A perfect clear on the results screen](docs/screenshots/results.png) | |

## The goal

Reach the burrow door at the end of each level, ideally with **every coin**,
as fast as you can.

- The clock starts with your first move and shows hundredths.
- Each enemy you stomp takes **0.5 s** off the clock (a green `-0.5S` pops up and
  the clock flashes). Whether a far-off enemy is worth the detour is up to you.
  The amounts are per kind, in `KILL_BONUS` in `src/game.c`.
- Taking damage costs nothing but the knockback; losing all three hearts
  restarts the level and the clock.
- The results screen shows the time, the bonus and the final time, then offers
  **Retry**, **Next level** and **Title**.
- Only a **perfect clear** (all coins) can enter the level's Top 10. Enter three
  initials (type them, or up/down; Enter when done). The last ones you used come
  pre-filled. Runs that used the debug teleport are marked practice and not
  recorded.
- Scores are kept in `~/.local/share/bunzi/scores.txt` (or `$XDG_DATA_HOME/bunzi/`),
  written through a temporary file and a rename. Each table belongs to one
  level layout: editing a level starts it a fresh table, and the old one stays
  in the file.

Water enemies:
- **Spiky fish** (level 4) trail you under the surface, leap on a beat (bubbles
  give it away) and snap up at you as you jump over their pool. Each needs a
  moment between leaps, so cross just after one.
- **Jellyfish** (level 3) squirt water at you, aiming where you're heading.

## Build & run

```sh
make            # builds build/bunzi and symlinks ./bunzi
./bunzi
```

Needs: a C compiler, SDL3, libpng, zlib. Run it in kitty (Ghostty should also
work). The art and sounds are ready-made in `assets/`; the game reads each
level's `gen/` folder, its level file and `assets/sounds/`.

## Controls

| Action | Keyboard | Gamepad |
|---|---|---|
| Move | arrows / A D | d-pad / left stick |
| Jump (hold = higher) | Z / space / up / W | A |
| Run | X / shift | X / RB / RT |
| Drop through platform | down + jump | down + A |
| Restart level | R | Back |
| Results: choose / select | arrows, WASD / Z, space, enter | d-pad / A |
| Mute sound | N | |
| Jump to level | 1–9 | |
| Debug overlay | Tab | Start |
| Back to the title | Esc / Q | Back + Start |
| Title: choose / select | arrows, WASD / Z, space, enter | d-pad / A, start |
| Title: quit | Esc / Q | B or back (to QUIT) |

Debug keys: M cycles pixel transport (shm/direct/directz), S toggles who
upscales (us or kitty), F cycles target fps.

## Options

`--probe` reports terminal and controller capabilities, `--bench SECS`
autoplays and prints frame statistics, `--dump FILE` renders one frame to a
PPM, `--level N` skips the title and starts at level N, `--fps`, `--mode`,
`--scale`, `--assets DIR`. `BUNZI_DUMP_TITLE=SECS` with `--dump`
renders the title screen. See `--help`.

`--show-fps` shows the frame rate kitty is actually displaying in the
bottom-left corner (green at the target rate, amber below it).

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

## Layout

- `src/` – engine: `term.c` (tty, kitty keyboard), `kgfx.c` (kitty graphics
  output), `pad.c` (SDL3 gamepad), `assets.c` (PNG, atlas, sprite drawing),
  `level.c` (text levels), `game.c` (gameplay + rendering), `results.c` (results
  screen and Top 10), `title.c` (title screen), `main.c` (loop).
- `assets/<level>/` – one folder per level with its `levelN.txt` and `gen/`:
  `sprites.png` + `sprites.txt` (the sprite atlas: name, x, y, w, h, anchor)
  and the parallax backgrounds (`bg_*.png`).
  1 `moonlit_ruins` (its atlas also holds the shared sprites: bunny, coins,
  effects), 2 `sunset_lake`, 3 `tropical_island`, 4 `crystal_caverns`.
- `assets/exit/`, `assets/effects/` – the burrow door, and the water squirt,
  splash and spray.
- `assets/title/` – the title screen's layers, logo and magic sparkles.
- `assets/sounds/` – every effect in a few variants (`jump_a/_b/_c.wav` …), a
  night ambience loop and three music loops: `music_chip.wav` (in game),
  `music_title.wav` (the title screen) and `music_calm.wav` (a spare).
  `sounds.txt` maps game events to files and volumes; edit it to pick variants.
- `docs/` – design notes (the terminal-transition plan) and the README's
  screenshots, rendered by the game's own `--dump`.

Levels are plain text, one character per 16px tile; each file's legend is at
its top (`=` platform, `_` rope bridge, `~` water, `e` mushroom, `n` snail, `c` crab,
`j` jellyfish, `y` bird, `a` bat, `s` slime, `z` cave worm, `q` spiky fish …). `./bunzi --check` reports hanging decorations with
nothing to hang from.

## Art and sound

The level, title and creature art was painted with ChatGPT image generation
and converted to hard-edged pixel art; the exit door and water effects were
made with SpriteCook. All sound effects and music are synthesized in code.

## Licence

MIT, see `LICENSE`.

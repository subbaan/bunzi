# Title Screen and Terminal Transformation Plan

**Version:** 1.0  
**Status:** Design / implementation proposal  
**Scope:** Animated launch, title screen, animated quit, art asset pipeline

## 1. Vision

The game is launched from a real terminal. For a brief moment the player sees that familiar terminal, then its characters begin to detach, become glowing particles, and reveal the painted world of the platformer. The title, menu, and gentle parallax animation settle into place.

When the player chooses **Quit**, the sequence reverses: the illustrated scene collapses into a constellation of characters and settles into a recognizable image of the original terminal. The game window closes to expose the *actual*, still-running terminal.

**Visual tone:** magical and slightly playful, in the same soft illustrated style as Moonlit Ruins and level two. Avoid aggressive hacker/glitch aesthetics, loud distortion, and lengthy boot logs.

**Design principle:** Terminal transition, title scene, and menu are separate subsystems. The title screen must also work when terminal capture is unavailable.

## 2. Agent preflight: inspect actual project before implementation

Before editing code:

1. Inspect current repository branch and latest changes, game framework/rendering backend, application entry point, window manager integration, startup/shutdown flow, input handling, resolution/scaling, resource/asset loading, and existing audio/animation helpers.
2. Identify whether the executable runs in a terminal, through a wrapper script, from a desktop launcher, or in a standalone window. Determine exactly when the game window is first created and first presented.
3. Identify existing level art/parallax conventions and shared theme/layout components. Reuse them rather than building a parallel renderer.
4. Confirm Linux/X11 and Wayland behaviour, fullscreen behaviour, and whether current startup has a splash screen or main menu.
5. Report any engine limitations and the proposed file/module layout before major changes. Never replace the existing startup path without a working fallback.
6. Increment the game's existing version using its established versioning convention for each code edit. Scripts should include an appropriate shebang.

The suggested API names below are conceptual, not a mandate to add classes or a specific engine.

## 3. Experience storyboard and timings

**Total intro target: about 2.5 to 3.5 seconds. Configurable, skip-able.**

| Phase | Target duration | Visuals and behaviour |
|---|---:|---|
| A. Capture / hold | 0.2-0.4s | Display the captured terminal image in a game-controlled surface, aligned where practical to the terminal window. A subtle cursor blink or glow signals that something is happening. |
| B. First spark | 0.3-0.5s | One character or cursor fragment detaches, glows, floats upward, and transforms into a firefly. |
| C. Dissolve | 0.8-1.2s | Selected glyph cells progressively break into small fragments. Faint light reveals the illustrated scenery underneath. Dissolve travels in a pleasing direction, not simply a uniform fade. |
| D. World reveal | 0.6-0.9s | Midground and foreground scenery resolves; particles become fireflies, petals, and atmospheric motes. Subtle parallax starts. |
| E. Logo and menu | 0.4-0.7s | Title logo appears, followed by Play / Options / Quit. Input becomes active. |

These phases can overlap; duration is an art-directable parameter rather than a series of forced pauses. The first visible game frame must be ready immediately, and no screenshot operation should freeze the UI for seconds.

**Outro target: about 1 to 2 seconds.** Hide/deactivate menu -> scenery and logo dissolve -> glyphs reform in their saved terminal positions -> full captured terminal image -> close the window to reveal actual terminal. Prefer easing and a shortened, recognizable inverse rather than literally running all intro frames backward.

### What the player should see

At the end of the intro, show a normal attractive title screen, *not* a terminal UI. A very small blinking underscore or status-text flourish is optional, but it should never compete with the logo.

## 4. Architecture

Proposed logical responsibilities:

- **TerminalCaptureProvider:** Capture an image of the launching terminal when authorized/supported. Return image, pixel dimensions, capture bounds if known, optional text-cell metadata, and capture status. No capture failure should stop launch.
- **TransitionController:** State machine controlling timings, skip, cancellation, scene composition, and all transition lifecycle states.
- **GlyphParticleSystem:** Break a screenshot or glyph grid into bounded procedural pieces and render them in a GPU-friendly manner; support emission, dispersal, glow, and return-to-origin. Handle ordinary terminal text and Unicode gracefully.
- **TitleScene:** Art-directed parallax background, foreground decorations, ambience, logo, optional character silhouette or pose.
- **TitleMenu:** Play / Options / Quit, keyboard and controller navigation, focus/selection, hover, click, accessibility and transition locking.
- **GameLifecycle:** Own transitions between launch, title, gameplay, title again, and exit. Ensure a single quit path and clean final teardown.

Potential state flow:

`BOOT -> CAPTURE -> INTRO -> TITLE -> STARTING_LEVEL -> GAMEPLAY -> TITLE`  
`TITLE -> OUTRO -> EXIT`

Also handle direct exits from pause/gameplay. Decide whether exiting directly from gameplay should return to the title first, or use a reduced transition that goes from game imagery to terminal. Keep this explicit and consistent.

At every stage provide a fast path for capture failure, skipped animations, unsupported render targets, and shutdown errors.

## 5. Terminal capture: feasibility and sensible fallback

**First target: Kitty on Linux.** Official kitty remote control includes a `screenshot` operation and a separate `get-text` operation. The screenshot is the more reliable source for exact colours, fonts, spacing and text appearance. `get-text` can optionally inform glyph segmentation but does not reproduce every on-screen visual detail.

Official documentation:
- https://sw.kovidgoyal.net/kitty/remote-control/
- https://sw.kovidgoyal.net/kitty/rc_protocol/

**Important:** Kitty remote control is subject to configuration and permissions; it is not safe to assume arbitrary access to a terminal window. Determine whether `KITTY_WINDOW_ID` or other reliable session information can safely target *the launching window*, not another kitty tab/window. Never take an unrelated window screenshot.

### Capture order

1. If the launch environment identifies the original terminal and an appropriate capture mechanism is available, attempt capture **before creating/showing any window that might cover it**.
2. Set a short timeout and validate the returned image, dimensions, and identity of the window captured.
3. Hold captured image in memory for use in both directions. Do not require writing a persistent screenshot to disk; if a temporary file is unavoidable, secure it and remove it promptly.
4. The game's renderer shows the captured image as its initial state. If terminal and game surfaces have different dimensions/aspect ratios, choose fit/crop/pad deliberately. Do not claim pixel-perfect continuity when window alignment is impossible.
5. On quitting, the transition reconstructs the *saved image*. Actual command output, shell prompt, and terminal cursor may have changed slightly while the game runs; closing the game reveals the real terminal. A subtle final crossfade or rapid window close can conceal a one-frame mismatch.

### Fallback modes

- **Full capture:** Real terminal screenshot and optional glyph coordinates. Preferred.
- **Visual capture only:** Real screenshot; procedural image-block fragmentation when per-character information is unavailable.
- **Textual approximation:** When text can be acquired safely but screenshots cannot, render a terminal-like screen matching available theme metadata. Clearly treat as approximation internally.
- **Generic intro:** Use an authored dark terminal-inspired graphic and the same dissolve effect when launched through a desktop file, capture is blocked, or no terminal exists.
- **Effects off:** Immediately show title screen.

Support X11 and Wayland without assuming `xwd`, shelling out to general screen capture indiscriminately, or silently requesting broad screen privileges. Kitty-provided capture is worth trying first; other desktop capture APIs need platform-specific capability checks and possibly user interaction.

Never request elevated permissions, modify kitty config, enable remote control globally, or capture sensitive content without an explicit user-approved design. A terminal screenshot can contain tokens, paths, passwords or personal data. Keep captures local to memory, never upload, log, or persist image/text. Offer a **terminal capture enabled** preference, and a privacy-first fallback. If adding screenshot storage for debugging, make it developer-only and explicitly opt-in.

## 6. Glyph dissolve implementation

This should be **procedural**, not a huge sprite sheet of every printable character.

- Start with a captured screenshot texture spanning the initial viewport.
- For an accurate text-disintegration effect, identify character cell locations using terminal metrics/metadata when available. Avoid brittle OCR. Another workable method is tiling the screenshot into small cell-sized rectangles; it looks convincing even if the cells are not exact glyph boundaries.
- Each emitted piece records `origin_position`, sampled texture/colour, `start_time`, `duration`, `velocity`, `rotation`, `opacity`, and `target` for optional reassembly.
- Dissolve only a controlled subset of cells/fragments while blending the remainder of the screenshot away with a soft spatial mask. This keeps GPU/draw-call and memory costs bounded.
- Use seeded randomization for repeatable and reversible choreography; ideally record particle origins rather than attempting to mathematically invert a particle simulation.
- Particles travel on gently curved trajectories with varied velocities. Small percentage become golden/cool-green fireflies that continue into the title ambience.
- Apply restrained glow at the moment of detachment; avoid flicker-heavy full-screen glitch effects.
- During the outro, particles converge toward saved origin positions and the screenshot texture fades in underneath. A visual composition can accomplish the illusion without reconstituting every glyph.
- Scale particle counts by resolution/performance tier. Put hard caps on active particles and transient GPU allocations.
- Keep transition dimensions and coordinates stable on window resize; either reproject from normalized coordinates or complete/skip the animation safely.

Potential optional polish: Let the terminal cursor's underscore be the first firefly, but avoid assuming its position can always be obtained. An authored first spark at the cursor-like position is acceptable as a fallback.

## 7. Title page art direction

**Working title is TBD. Do not permanently bake the game name into the background.**

Design at the project's reference aspect ratio (likely 16:9; verify) with safe areas for other aspect ratios and window sizes.

Proposed composition:

- **Far distance:** Distant mountains, setting sun/moonlit gradient, and a small castle silhouette recalling level two.
- **Middle distance:** Lake, pine treeline and hints of the two level palettes.
- **Playable-world echo:** Ancient ruined stones from Moonlit Ruins, a short fence/bridge suggestion, rocks and wildflowers around the base of the composition. The title screen should feel like the same world, not like a collage of the two game levels.
- **Foreground framing:** Dark shrub and grass silhouettes toward the corners. Do not obstruct text/menu or dominate the centre.
- **Character (optional):** Small character standing on a ledge looking toward the castle; reuse existing player art only if it works at menu scale.
- **Logo:** Large, readable, separate transparent image. Place in the upper third/middle as composition allows.
- **Menu:** Play, Options, Quit on a quiet, legible part of the scene. Use soft focused states and explicit controller/keyboard focus.

The scene should look finished even with zero animation. Avoid unnecessary bloom and moving backgrounds that compete with readable menus.

### Art asset checklist

| Asset | Delivery suggestion | Required? | Notes |
|---|---|---|---|
| Title background: sky, distant mountains, castle | Wide transparent/opaque layer as appropriate | Yes | Existing level-two art may be adapted; keep sun/moon in distant layer. |
| Mid-distance trees and lake | Wide separate parallax layer, alpha if appropriate | Yes | Full opacity for the painted scene details; alpha around silhouette. Reuse/extend existing pieces if resolution permits. |
| Foreground ruins, rocks, bridge/fence accents | Transparent grouped layers | Yes | Reuse level tiles intelligently; may need custom composition pieces. |
| Foreground bush/grass silhouettes | Several transparent variants | Yes | Reuse previously generated level-two foreground bushes if suited. |
| Game logo | High-resolution transparent PNG or SVG if engine supports | Yes, once name decided | Separate from artwork for scaling/animation. |
| Menu highlight/selection ornament | Small transparent nine-slice or vector | Optional | Can also render procedurally; do not hard-code text into images. |
| Firefly halo / luminous particle | 1-3 tiny soft sprites with alpha | Recommended | Subtle warm gold and cool teal/green variations. |
| First spark / glyph-to-firefly effect | Small 6-12-frame sprite strip OR shader/procedural animation | Optional | Prefer procedural where possible. |
| Glimmer/star twinkles | Tiny sprite atlas | Optional | Sparse, low motion. |
| Dust motes / petals | 2-4 very small transparent sprites | Optional | Light atmospheric motion. |
| Terminal capture texture | Runtime generated | Yes for full mode | Never ship a screenshot of a user's terminal. |
| Glyph fragment atlas | Runtime generated from captured image | Yes for full mode | No pre-rendered alphabet sprite sheet needed. |
| Generic fallback terminal image | Authored, nonpersonal, dark terminal mockup | Yes for fallback mode | No secrets, paths or identifying usernames. |
| Vignette/light sweep | Shader or procedural mask | Recommended | No large bitmap required. |

**Art delivery specification:** Each layer should use a consistent reference canvas and origin, with transparent pixels where intended, generous horizontal overscan for parallax, no flattened menu text, and no unintended backdrop baked into transparent foreground pieces. Preserve working/source assets alongside exported runtime textures. Asset sizes, texture formats and atlas packing should follow the engine's established constraints. All animation components should be usable independently.

### Art production order

1. Approve one **fully revealed static title-screen mockup** first, with enough quiet space for logo/menu.
2. Separate it into actual depth layers or rebuild those layers from originals. Do not merely cut flat artwork into fake layers with missing scenery.
3. Design a separate transparent logo and selection ornament.
4. Generate a tiny particle/FX sheet **only if** procedural rendering cannot cover those visuals.
5. Test legibility at real game resolution and smaller window sizes.

## 8. Menu interaction and audio

- Input: keyboard, mouse, and existing controller mappings. Respect established navigation conventions.
- During intro, pressing confirm, escape, or other supported skip input should finish the transition and reveal a fully functional menu, without accidentally pressing Play in the same event.
- During outro, handle repeated Quit, Escape, or close-window events idempotently. Allow immediate shutdown on an explicit second skip/quit event.
- Provide `Skip intro`, `Animation intensity`, `Terminal capture`, and optionally `Reduced motion` settings if consistent with existing options architecture.
- Keep sound subtle: one bright spark/chime, quiet airy reveal, soft menu select, restrained retreat sound. Use available audio infrastructure; do not make audio assets a blocker.
- Title scene ambient animation: very slow parallax (perhaps a few pixels), sparse fireflies, minor vegetation movement, occasional water glimmer. Respect reduced motion.
- Menu text must maintain contrast against both light sunset and dark moonlit regions. Use shadows/backplates as needed.
- On returning from gameplay to the title, **do not capture the terminal again** and normally do not replay the terminal introduction.

## 9. Window geometry and OS integration

This is the key illusion risk, so prototype it first.

- Capture the target kitty window while visible, before the game obscures it.
- Verify which pixels are in the captured content (terminal pane versus OS decorations, margins, tabs). Define how each is presented in the game scene.
- Prefer a transitional borderless window, sized/placed over the captured terminal if engine/WM allows, then smoothly animate to normal game window or fullscreen. Do not assume absolute positioning works consistently on Wayland.
- If geometry/focus constraints make alignment unreliable, avoid pretending exact overlap: start with the terminal screenshot centered in the game viewport, then let it expand or dissolve into the title world.
- Manage scaling across HiDPI, multiple monitors, non-16:9 geometry, compositor, terminal tabs/splits, and focus-follows-mouse.
- Ensure a cancelled/fullscreen-resized transition doesn't leave an invisible overlay or focus-trapping window.
- Closing the game should expose the existing terminal. It should not force a clear, reset the scrollback, append ANSI escape sequences, or try to reconstruct shell state.

## 10. Implementation milestones

### Phase 1: engineering spike

Build a tiny prototype that captures the *correct launching kitty window*, displays it as an image in the engine, and closes cleanly back to the terminal. Test X11 first if that's the development environment; assess Wayland separately. Keep a generic fallback path. **Gate:** reliable first frame, identity and geometry, no terminal corruption, no hangs.

### Phase 2: transition effect

Implement spatial dissolve, textured cell fragments and first spark, then reconstruct in outro. Start with simple coloured quads and a screenshot. Add glow only after the basic visual illusion is convincing. **Gate:** bounded allocations, smooth FPS, skip/cancel reliable.

### Phase 3: standalone title/menu

Integrate approved title art layers and build menu navigation/return-from-game. Placeholder art acceptable until final artwork is approved. **Gate:** every menu path and resolution works without terminal capture.

### Phase 4: art and audio polish

Add real layers, logo, atmosphere, particle sprites if necessary, subtle SFX, and settings. Tune pacing at 60 FPS and on lower powered machines. **Gate:** intro/outro feel like one sequence, aesthetics match levels, scene still reads when motion is off.

### Phase 5: regression and packaging

Verify execution from kitty, other terminals, desktop launcher, SSH session (if applicable), X11, Wayland (where supported), windowed/fullscreen, multiple monitors, terminal capture disabled, and absent capabilities. Document optional Kitty enhancements versus baseline dependencies.

## 11. Acceptance criteria

- [ ] Terminal-aware intro uses a real capture when supported and authorized.
- [ ] Launch never requires changing terminal settings or installing screenshot services.
- [ ] Capture targets the launching terminal, not a random active application.
- [ ] On unsupported systems the game still opens straight into a polished title screen / generic intro.
- [ ] Terminal-derived content is never persistently logged or uploaded.
- [ ] First frame, transition and title screen avoid black-frame flashes and unresponsive controls.
- [ ] Intro is optional/skip-able, and skip consumes the initiating input.
- [ ] Title art layers are independent and parallax is restrained.
- [ ] Menu works with mouse, keyboard and configured controller.
- [ ] Quit transition ends by cleanly closing the game and revealing the existing terminal.
- [ ] Repeated quit events, close-window, SIGTERM, load errors and other exceptional shutdowns do not leave a blocking overlay or damaged shell.
- [ ] Gameplay -> title -> gameplay does not replay the terminal sequence unexpectedly.
- [ ] All graphics are correctly positioned under resize, HiDPI, aspect-ratio variation, and fullscreen switches.
- [ ] Reduced motion and no-terminal-capture settings work.
- [ ] Existing game tests and packaging/build commands pass.

## 12. Scope guardrails / risks

1. **Do not delay playable title-screen development waiting for a universal screenshot facility.** The title must function standalone.
2. **Do not attempt to manipulate the original terminal's shell history or output.** The game closes to reveal it naturally.
3. **Do not store sensitive screenshots beyond the process lifetime.** Design a privacy-safe off switch.
4. **Do not make thousands of independent scene objects for individual characters.** Batch particles and cap counts.
5. **Do not build a generalized terminal emulator.** Use an image texture and cell animation. Accurate ANSI recreation is unnecessary.
6. **Do not promise exact continuity across different monitors/Wayland compositors.** Use deliberate fallback choreography.
7. **Do not bake the logo or menu into the background illustration.** Keep later localization, layout and animation possible.

## 13. Questions for agent to resolve from repository

- Which renderer/window library owns the screen and supports a texture-backed transition surface?
- Can a first transitional window inherit/overlap terminal bounds, and would that disrupt fullscreen?
- Is kitty launched with an existing remote-control interface, and can we securely capture the launching window without configuration changes?
- What are the game's internal dimensions and current background parallax asset dimensions?
- Does Quit currently originate in main menu, pause menu, OS close event, or elsewhere?
- Which existing asset path, shader convention, and versioning rules should be used?

**Suggested immediate next task:** Build and evaluate the Kitty capture/window-overlap engineering spike, *then* finalize a static title screen mockup. These can be pursued independently, with the approved art feeding Phase 3.

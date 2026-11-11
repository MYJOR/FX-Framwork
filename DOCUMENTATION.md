# FX Framework

Two real-time scenes written in C11 on a shared, dependency-light base: a Win32 window, an OpenGL 3.3 core context, and a small shader loader with hot reload.

| Executable | What it is |
| --- | --- |
| `desert` | Third-person dune field: wind-blown sand, footprints and slide grooves, an animated character, coins, laser beams. |
| `office` | First-person interior: baked area lighting, a scene editor, a torch, a horror look, and a hand-held motion tracker. |

Both are locked to 30 fps in play. The office editor runs at 60.

## Building

Requirements: Windows, CMake 3.16 or newer, Ninja, and a MinGW-w64 GCC. Everything links statically against `opengl32`, `gdi32` and `user32` (`desert` also needs `psapi`).

`third_party/CUI` must be present. If it is empty, clone <https://github.com/yappologistic/CUI> into it; the configure step stops with a message otherwise.

```powershell
cmake -S . -B build_local -G Ninja
ninja -C build_local office
ninja -C build_local desert
```

Run the executables from the build folder. They look for `shaders/` and `office assets/` in the working directory, its parent and its grandparent.

## Repository layout

| Path | Contents |
| --- | --- |
| `src/` | All C sources (see the module tables below) |
| `shaders/` | GLSL. `common.glsl` is prepended to every desert shader, `interior_common.glsl` to every office shader |
| `assets/` | Character and animation GLBs (Universal Animation Library 1 and 2) for the desert |
| `office assets/` | Room textures, the smudge decal, the chair and table FBX models |
| `animation repo/` | Source files of the animation libraries |
| `third_party/` | `CUI` (immediate-mode UI) and `cgltf` (glTF loader) |
| `office_scene.txt` | The office layout as last saved from the editor (created on first save) |

## Shared layer

| File | Role |
| --- | --- |
| `platform.h`, `platform_win32.c` | Window, GL context, keyboard and raw mouse input, mouse capture, vsync, frame pacing, a hidden-window mode for automated runs |
| `gl.h`, `gl.c` | The GL functions beyond 1.1, declared in one X-macro list and loaded at start-up |
| `shader.h`, `shader.c` | Builds a program from the prelude plus a vertex and a fragment file. Reloads when a file changes and keeps the old program if the new one fails to compile |
| `mathx.h` | `V3`, column-major `M4`, perspective and view matrices |
| `png.c`, `inflate.c` | PNG reading and writing, no external library |
| `gamepad.h`, `gamepad.c` | Xbox controller through XInput, loaded at run time |

## Desert

### Controls

| Input | Action |
| --- | --- |
| W A S D / left stick | Move |
| Right mouse + mouse / right stick | Orbit the chase camera |
| Shift / RT / left stick click | Sprint |
| Ctrl / LT | Walk |
| Space / A | Jump |
| X | Cast the laser beam |
| F / Y | Toggle the free camera (Space and Ctrl then move it up and down) |
| Q, E / D-pad down, up | Lower and raise the sun |
| F1 | Diagnostics overlay: off, HUD, full panel |
| F2 | Pause the profiler capture |
| F3 | Wireframe |
| F4 | Cycle the sand debug view |
| F5 | Vsync |
| Esc | Quit |

### Modules

| File | Role |
| --- | --- |
| `main.c` | Scene set-up, chase and free cameras, the frame, command-line test options |
| `terrain.c` | Procedural dune field: asymmetric dunes with a gentle windward side and a steep slip face. Height and normal queries, plus a cached grid for bulk lookups |
| `wind.h` | The gust model. The same formula lives in `shaders/common.glsl`, so CPU particles and shader effects agree. Change both together |
| `particles.c` | 24,000 wind-blown sand grains and a 6,000-entry pool of grains and dust kicked up by the character |
| `player.c` | Character controller on the terrain: gaits, lean, jumping, sliding on steep slopes |
| `character.c` | Skinned glTF character. Locomotion blends idle, walk, jog and sprint by ground speed, with slide and jump clips retargeted from the animation libraries |
| `trail.c` | Footprints and slide grooves in a wrapping 2048 x 2048 texture centred on the player |
| `coins.c` | Collectable coins scattered over the dunes |
| `beam.c` | Additive laser beams and their flare |
| `diag.c`, `debug_ui.c` | CPU zones, GPU timer queries, frame history and counters, shown in a CUI overlay |

## Office

A small office, a hallway along its +X side and a larger room beyond, joined by arched doorways. The room shell is fixed. Everything in it (chair, desk, light panels, vents) is an object that can be added, moved, hidden and saved.

### Controls

| Input | Action |
| --- | --- |
| W A S D / left stick | Move |
| Mouse / right stick | Look |
| Shift / RT / left stick click | Run |
| Ctrl / B (toggle) | Crouch |
| Right mouse / LT | Zoom. With the tracker up: look past it at the room |
| Space / LB (hold) | Raise the motion tracker |
| F / Y | Torch |
| L / X | Room lights |
| H / D-pad up | Horror look |
| - and = / D-pad left, right | Exposure |
| Y / Start | Scene editor on and off |
| F1 (Shift+F1 back) | Debug view |
| F2, F3, F4 | Ambient obscurance, reflections, depth of field |
| F5 | Vsync |
| Esc | Quit. In the editor: deselect, then back to play |

In the editor, Q, G and R pick the select, move and rotate tools, and holding the right mouse button looks around.

### Modules

| File | Role |
| --- | --- |
| `office_main.c` | The player (movement, collision, head bob, tilt, idle sway), input, zoom and focus pull, torch, horror look, the frame |
| `office_scene.c` | Builds the rooms and the object prototypes, places objects, derives lights, reflection proxies and colliders, saves and loads the layout |
| `office_editor.c` | The CUI editor: menus, toolbar, outliner, details panel, move and rotate gizmos, undo and redo |
| `interior.c` | The interior renderer |
| `tracker.c` | The motion tracker |
| `fbx.c` | Minimal binary FBX reader for static meshes |

### Interior renderer

| Stage | How |
| --- | --- |
| Direct light | Rectangular area lights with an analytic form factor and contact-hardening soft shadows |
| Indirect light | A grid of L1 spherical-harmonic irradiance probes, baked on the GPU with several bounces |
| Specular | One box-projected reflection probe per room, plus box proxies that stand in for furniture |
| Contact shadow | Screen-space ambient obscurance, blurred |
| Torch | A spot light with its own shadow map and beam pattern, one bounce, and a visible beam in the air |
| Decals | Stains multiplied over the lit surface, with optional normal maps |
| Image | HDR, MSAA resolved after tonemapping, eye adaptation, bloom, depth of field, barrel distortion, film grain, colour fringing, lens dirt |

Shadows and probes are baked once at load (`interior_bake`) and updated incrementally while the scene is edited (`interior_bake_step`). Everything a lighting artist would tune is in `IntLook` (`interior.h`).

A frame is: torch shadow map, depth and normal prepass, ambient obscurance, forward pass, decals, held item, light meter, glow, resolve and grade, depth of field, lens.

### Motion tracker

A hand-held scanner modelled on the one in Alien: Isolation. It sends out a ping, and anything moving within 12 m shows as a blip that lights when the ping passes over it and fades until the next one. Ahead is up, the player is the mark near the bottom of the screen, and things that hold still do not show. The ping speeds up as the nearest contact gets closer.

- **How it is held.** Low and slightly right of centre, leant back and turned in, with the grip running off the bottom of the frame. It rises on a spring, trails the view when the camera turns and swings with each step. The pose constants are at the top of `tracker.c`.
- **How it is drawn.** The interior renderer calls `IntRenderer.held_draw` after the room, into the same HDR target, so the tracker gets the same exposure, bloom, grade and lens. It is lit where it really is, at arm's length, but drawn pulled towards the eye so it never pokes through a wall. It has its own fixed field of view.
- **Depth of field.** The tracker writes alpha 0, which is how the resolve pass tells it from the room. While it is up, `IntLook.dof_hand` moves the focus onto it and the room behind goes soft. The zoom button pulls the focus back to the room and the tracker blurs instead. The blur amount is stored signed (in front of or behind the focus), so the soft room never smears over the sharp tracker.
- **Screen.** Drawn procedurally in `shaders/tracker.frag`: range rings, bearing lines, the ping, blips, a distance readout and a range bar, under glass that reflects the room. It is emissive, blooms, and spills onto its own bezel. Most of the eye adaptation is taken back out so it does not blow out in the dark.
- **Contacts.** The game passes world positions and a motion amount through `TrackerInput`. Nothing in the office moves yet, so `office_main.c` supplies one invisible contact that walks a loop through the hallway and the big room. Set `TRACKER_DEMO_SIGNAL` to 0 to remove it.

Not done yet: a hand model, sound, the torch lighting the tracker or being shadowed by it, and the screen lighting the room.

## Command-line options

Both executables take options for automated runs and screenshots.

Common to both:

| Option | Effect |
| --- | --- |
| `--shot <file.png>` | Render, save a screenshot, exit |
| `--pos <x> <z>` | Start position |
| `--yaw <deg>`, `--pitch <deg>` | Start orientation |
| `--view <n>` | Debug view |

Office only:

| Option | Effect |
| --- | --- |
| `--headless` | Hidden window, frames go to an offscreen target |
| `--size <w> <h>` | Window size |
| `--frames <n>` | Frames to render before `--shot` |
| `--default-scene` | Ignore `office_scene.txt` |
| `--edit`, `--select <i>`, `--place <i> <x> <z> <deg>`, `--cmd <name>` | Start in the editor and drive it |
| `--drag`, `--axis-drag`, `--ring-drag`, `--wheel` | Scripted mouse input for the editor |
| `--flash`, `--lights-off`, `--horror`, `--zoom` | Torch on, room lights off, horror look, zoom held |
| `--tracker`, `--tracker-focus` | Tracker held up; and looking past it |
| `--noao`, `--nodof`, `--nolens`, `--noadapt`, `--nobloom`, `--noair` | Switch one effect off |
| `--fov`, `--exposure`, `--msaa`, `--eye`, `--roll`, `--sway` | Set one look or camera value |
| `--bench` | Uncapped, prints the frame time, exits |
| `--pace-test` | Prints how evenly paced frames were |

The desert has its own set in `main.c`, including `--sun`, `--time`, `--walk`, `--run`, `--stroll`, `--orbit`, `--free`, `--ui`, `--tab`, `--wire` and `--pause`.

Example, a tracker screenshot in the dark hallway:

```powershell
cd build_local
.\office.exe --headless --default-scene --tracker --horror --lights-off --pos 4.2 2.5 --shot tracker.png --frames 60
```

## History

| Date | Commit |
| --- | --- |
| 2026-11-09 | First commit: the desert |
| 2026-11-09 | Trails and footsteps on the sand |
| 2026-11-09 | Character |
| 2026-11-09 | Laser beams |
| 2026-11-10 | Office: custom edit mode |
| 2026-11-10 | Zoom, camera tilt, a fuller edit mode |
| 2026-11-10 | Torch |
| 2026-11-10 | Horror profile |
| 2026-11-10 | Hallway |
| 2026-11-10 | Decals (early) |
| 2026-11-10 | Motion tracker ("added scanner") |

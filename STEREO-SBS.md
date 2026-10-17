# Side-by-side 3D (experimental)

Full side-by-side stereoscopic output, aimed at XR glasses such as the VITURE Pro
in 3D mode (3840x1080, one 1920x1080 image per eye).

## How it works

Diablo is drawn as 2D sprites, so there is no 3D scene to render twice. Instead:

1. While the dungeon is drawn, the tile and sprite blitters also write a depth
   value for every opaque pixel (`stereo::MarkDepth`, hooked into
   `dun_render.cpp` and `clx_render.cpp`).
2. Depth comes from each element's screen-space "foot" line, where it meets the
   ground. Pixels above the foot get a bit nearer for every row they rise (a
   vertical surface leans toward a camera looking down at ~30 degrees); pixels
   below it are floor.
   - Floor, items and corpses lie flat.
   - Monsters, players and objects stand on a single row.
   - Walls and roofs stand on the back edges of a tile diamond (a "^"), so a wall
     running across several tiles gets one continuous ground line. For tiles
     inside a building, that's the front tile of the building in the same screen
     columns, so the whole face of a building shares one ground line.
3. Wall and roof depth is then smoothed across neighbouring wall/roof pixels
   (about one tile's reach), which blends the patches large roofs are made of.
4. Each eye is made by shifting world pixels left or right in proportion to
   depth, on a grid four times finer than a pixel, then blending back down to
   full colour. Whole-pixel shifts would make the sloping floor jump visibly
   every few rows. Holes that open up are filled from the farther neighbour.
5. Anything drawn after the world is found by diffing against a snapshot of the world.
   Item name labels and floating damage numbers register the screen area they drew
   and the ground row of what they belong to, and are shifted to that depth. The cursor
   is shown at the depth of whatever is under its tip (from the copy of the pixels behind
   it the game already keeps), or flat over the HUD. Everything else (HUD, panels,
   inventory) stays at screen depth in both eyes; a second snapshot taken right after
   the labels keeps panels drawn over them flat.

Menus, loading screens and videos are shown flat, duplicated to both eyes.

Code: `Source/engine/render/stereo.{hpp,cpp}` plus small hooks in `scrollrt.cpp`
(depth capture and foot lines), `dx.cpp` (SBS present), `display.cpp`
(double-width window), `controls/input.h` (mouse mapping) and `hwcursor.hpp`.

## Running

Turn on **Settings → Graphics → Side-by-Side 3D** from the main menu. It's saved in
`diablo.ini` and switches immediately. It can't be changed during a game; quit to the
main menu first.

The same page has **3D Depth** (tenths of a percent of the image width; 20 = 2%),
**3D Screen Plane** (percent down the view) and **3D Swap Eyes**. The in-game hotkeys
below change these settings and save them straight away.

Alternatively, `devilutionx.exe --sbs` forces it on for one run without changing the setting.
The tuning flags below also apply to that run only, until a hotkey changes the same value.

| Flag | Meaning |
|---|---|
| `--sbs` | Enable full side-by-side output |
| `--sbs-strength <n>` | Depth in tenths of a percent of eye width (default 20 = 2%) |
| `--sbs-convergence <n>` | Screen-plane height as percent of the view (default 80) |
| `--sbs-swap` | Swap eyes |
| `--sbs-display <n>` | Display to open on. By default the game picks the first display at least 3x wider than tall (e.g. 3840x1080), whichever screen it was launched from |

In game, hold Ctrl+Alt and press:

| Key | Action |
|---|---|
| Up / Down | More / less depth |
| Right / Left | Move the screen plane down / up the view |
| Home | Swap eyes |

Requirements and settings:
- **Upscale must be on** (the default on desktop). Without it the game falls back to flat output.
- The hardware cursor is turned off automatically in stereo mode. The game's own cursor is drawn in both eyes.
- Use **Fullscreen** with **Fit to Screen**. The window is twice the per-eye width,
  and fit-to-screen picks the per-eye aspect from half the display width.
- Pick the per-eye game resolution in Settings, then Graphics. 960x540 or 1280x720 give a
  view close to the original game's field of view. Both are upscaled to 1920x1080 per eye.

### VITURE Pro

1. Switch the glasses to 3D (SBS) mode with their 2D/3D toggle (see the VITURE manual).
   Windows should then report a 3840x1080 display.
2. Turn on Side-by-Side 3D in the Graphics settings (or launch with `--sbs`) with fullscreen enabled.
   In side-by-side mode, fullscreen is a borderless window one pixel taller than the display. A window
   that exactly covers it gets direct scanout / G-Sync, which upset stereo fusion on the glasses.
3. If depth looks inverted (near things look far away), press Ctrl+Alt+Home or use `--sbs-swap`.
4. If your eyes strain, lower the depth with Ctrl+Alt+Down. Background disparity above
   about 2% of the image width can exceed eye separation on a large virtual screen.

## Running on another PC

The build is self-contained apart from the game data. Put these in a folder of their own:

- `devilutionx.exe` from the build folder
- the build's `assets` folder
- `DIABDAT.MPQ` (your own copy of the game), plus Hellfire's `.mpq` files if you use it

Don't copy the exe into an existing DevilutionX install. The game prefers files inside `.mpq`
archives over the `assets` folder, so an older release's `devilutionx.mpq` would be used instead
of the matching assets, and the game crashes once the title screen loads.

The exe needs the Microsoft Visual C++ 2015-2022 runtime (x64), which most Windows PCs already
have. If it reports a missing `VCRUNTIME140.dll` or `MSVCP140.dll`, install the
"Visual C++ Redistributable (x64)" from Microsoft.

Settings and saves live in `%APPDATA%\diasurgical\devilution`, shared with any normal
DevilutionX install on that PC. To keep them separate:

```
devilutionx.exe --config-dir .\config --save-dir .\saves
```

## Multiplayer

Build with networking on (don't pass `-DNONET=ON`) to get the Client-Server (TCP) and ZeroTier
options. Joining a game requires the exact same DevilutionX version, and this fork is 1.6.0, so
everyone in the game needs this build (or another 1.6.0 build); release 1.5.x players can't join.
Side-by-side 3D is purely local, so other players don't need to use it.

## Known limitations

- The monster health bar (a fixed bar at the top), the automap and other HUD stay at screen depth.
- Labels, damage numbers and the cursor are shifted by whole pixels (they're flat at one depth, so no ripples).
- The floor still has very faint quarter-pixel depth steps, seen as gentle "hills".
- Translucent (blended) pixels take the depth of whatever is behind them.
- SDL3 and SDL1 builds ignore `--sbs` (output stays flat). Only the default SDL2 build is wired up.
  On Windows the game always uses the Direct3D 11 renderer, because Direct3D 9 loses its device when
  the glasses switch between 2D and 3D modes.
- Cost at 1280x720 per eye: about 4 ms per frame for the eye images plus about 1.3 ms for wall/roof
  smoothing, both spread across CPU cores.

DevilutionX with side-by-side 3D (SBS fork)
============================================

An unofficial build of DevilutionX with full side-by-side stereoscopic 3D for XR glasses
in a 3840x1080 SBS mode (e.g. VITURE Pro). Source: https://github.com/mtuxworth/DevilutionX-SBS

Setup
-----
1. Unzip into a folder of its own. Don't copy it over an existing DevilutionX install: an older
   devilutionx.mpq there takes priority over this build's assets folder and the game crashes.
2. Copy DIABDAT.MPQ from your Diablo CD or GOG install into the same folder.
   For Hellfire, also copy hellfire.mpq and the hf*.mpq files.
3. Run devilutionx.exe.

3D
--
Switch your glasses to 3D (SBS) mode, then turn on Settings > Graphics > Side-by-Side 3D.
The game opens on the side-by-side display automatically. 3D Depth, 3D Screen Plane and
3D Swap Eyes are on the same page. In game, hold Ctrl+Alt and press:
  Up / Down      more / less depth
  Left / Right   move the screen plane
  Home           swap eyes (if near things look far away)

Multiplayer
-----------
Client-Server (TCP) and ZeroTier work, but everyone in the game needs this build (version 1.6.0).
Release DevilutionX 1.5.x players can't join.

Settings and saves are stored in %APPDATA%\diasurgical\devilution and shared with any other
DevilutionX install. To keep them separate, create "config" and "saves" folders next to the exe
(the game won't create them itself), then run:
  devilutionx.exe --config-dir .\config --save-dir .\saves

Full details: STEREO-SBS.md. Licence: LICENSE.md (DevilutionX's Sustainable Use License).
Diablo and its game data are owned by Blizzard; this package doesn't include them.

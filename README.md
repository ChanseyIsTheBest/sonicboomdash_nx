# Sonic Dash 2: Sonic Boom — Nintendo Switch port (Unity 6 / IL2CPP wrapper)
 
This is a native wrapper / loader that runs the original ARM64 Android build of Sonic Dash 2: Sonic Boom v3.24.0 on Switch homebrew. It contains no game code and no game assets — it loads the game's own libraries and recreates, natively, the Android layer Unity expects: the activity and its window, EGL, audio, input, storage, and the JNI surface the game and SEGA's SDKs call out to. Ads, purchases and sign-in report unavailable, so the game runs its own offline path.
 
## Install & run
 
You need files from your own copy of Sonic Dash 2 v3.24.0. It ships as an App Bundle, so that is two APKs.
 
```
sdmc:/switch/sonicboomdash
├── sonicboomdash.nro
├── libmain.so              <- from config.arm64_v8a.apk: lib/arm64-v8a/
├── libunity.so                (only these three — not the split's ad,
├── libil2cpp.so                analytics or crash-reporting libraries)
├── assets/                 <- the base APK's whole assets/ folder, as it is
├── config.txt              (written on first launch)
└── save_edit.txt           (written on first launch)
```
 
Any folder name works. The first boot packs `assets/` into a single file — a few minutes, with about 250 MB free — and every later boot opens the pack.
 
Launch over a game — hold **R** while starting any installed title — not from the album: Unity needs more memory than the album applet gets.
 
## Controls
 
Sonic Dash 2 is played by touch, so the controller drives an on-screen cursor.
 
| Input | Action |
|---|---|
| Touchscreen | Tap and swipe (handheld) |
| **+** | Toggle the on-screen cursor |
| **–** | Toggle gyro pointing (tilt/turn the controller to aim) |
| Left stick | Move the cursor; steers during an Enerbeam |
| **L** / **R** | Recenter the cursor (helps gyro aiming) |
| **A** / **ZR** / **ZL** | Tap / confirm (ZL and ZR let you play one-handed); hold while moving the cursor to swipe |
| D-pad up / down | Adjust sensitivity of whatever is driving the cursor |
| Tilt | Steers during an Enerbeam |
 
The cursor is on by default docked and off in handheld; **+** overrides either way. A USB mouse works in both modes: click to tap, scroll to change sensitivity, and gyro turns itself off while one is connected. Sensitivities are saved in `pointer.cfg`; a `cursor.png` of up to 64×64 next to the `.nro` replaces the arrow.
 
## Settings
 
`config.txt` is written next to the `.nro` on first launch, documented inline:
 
```
resolution = 720        # picture height, 720 .. 1080 — the same docked and handheld
rotation = 0            # 0 landscape, 1 = 90 clockwise, 2 = 90 counter-clockwise, 3 upright
language = auto         # auto, or en fr de it pt ru es
tilt = both             # Enerbeam steering: both, gyro, stick or off
tilt_invert = 0         # 1 if leaning right swings you left
tilt_sensitivity = 100  # percent, 25 .. 400
```

## Save editing
 
`save_edit.txt` is written next to the `.nro` on first launch, listing every option with every line commented out: rings, red star rings, level or XP, characters and their upgrade levels, presents and sprites. Uncomment a line and it is applied at each launch; `present` and `sprite.add` lines are used once, then commented out for you. The save is re-encrypted with the game's own key, the untouched original is kept once as `save.original`, and what each launch did goes to `save_edit_result.txt`.
 
## Building
 
Requires devkitPro with the switch-dev group plus these portlibs:
 
```
dkp-pacman -S switch-dev
dkp-pacman -S switch-sdl2 switch-mesa switch-libdrm_nouveau switch-zlib \
              switch-libpng
 
export DEVKITPRO=/opt/devkitpro
make                        # -> sonicboomdash.nro
```
 
## Credits
 
The loader and shim infrastructure — so_util, libc_shim, jni_fake and the diagnostics — derives from the open-source Switch/Vita .so-loader lineage: Andy Nguyen, fgsfds and Rinnegatamante, building on TheOfficialFloW's loader tradition. It reaches this project via the Zookeeper DX, CloverPit, Fruit Ninja, Colour Sheep and Clay Jam ports, and the Sonic Dash port this one is forked from; the cursor, rotation and config.txt come from the Bloons Pop port, the asset pack from Fruit Ninja Classic, Enerbeam tilt from Sonic Jump, the save editor's design from Sonic Dash and Bloons Adventure Time TD, the window-geometry rule and Java callbacks from Data Defense, and the Unity 6 clock findings from CloverPit and Bounce Masters. All MIT-licensed. Thanks to everyone in that lineage for making this approach possible.

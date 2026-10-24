# Bully: Anniversary Edition for dArkOSen (RK3326)

A port of the Android release of **Bully: Anniversary Edition** to RK3326 handhelds running dArkOSen (R36S and similar).

This project does not reimplement the game. It loads the original Android native game library, patches it at runtime, and runs it on Linux with SDL2, OpenGL ES 3 (Mali-G31) and OpenAL. It is based on bully-NX, the Nintendo Switch port.

## Important

You need your own **officially obtained** copy of the game:

- `Bully: Anniversary Edition`, Android version **v1.4.311**
- with the **arm64-v8a** libraries

No game files are included in this repository or its releases. Do not distribute extracted APK contents.

## Installation

1. Copy the release files into your ports folder (`/roms/ports/` or `/roms2/ports/`):
   ```txt
   ports/
     Bully - Anniversary Edition.sh
     bully/
       bully
   ```
2. Put your APK in `ports/bully/`. Any file name ending in `.apk` works.
3. Start **Bully - Anniversary Edition** from the Ports menu.

On the first start the port finds the APK and offers to install the game:

- **A** installs, **B** exits.
- It checks that there's enough free space before it starts.
- It extracts the game library, `libc++_shared.so`, the `assets/` folder and the banner image into `ports/bully/`.
- The APK is kept. Delete it yourself afterwards if you want the space back.
- If the install is interrupted, it starts over on the next launch.

After that, every start goes straight to the game.

## Controls

- Standard gamepad layout, using the handheld's PortMaster controller mapping.
- **Select + Start** exits the game at any time.
- Face-button swaps and crouch toggle are available in `config.txt`.

## Settings: `ports/bully/config.txt`

The file is created on the first start and rewritten from its template on every start. Your values are kept, every key is explained in the file, and out-of-range values are clamped.

| Key | Values | Default | What it does |
|---|---|---|---|
| `render_scale` | 0.50–1.00 | 0.75 | Fraction of the screen resolution the game renders at, scaled up in one filtered pass. Lower is faster, higher is sharper. |
| `clarity` | 0 Low, 1 Medium, 2 High | 0 | The game's own quality preset: internal resolution level and post-processing. |
| `shadows` | 0 Off – 3 High | 0 | Shadow quality. Expensive on this GPU. |
| `trilinear_filter` | 0 / 1 | 0 | Smoother transitions between texture detail levels. |
| `disable_mipmaps` | 0 / 1 | 0 | Experimental. Skips the smaller texture copies. |
| `language` | 0 English, 1 Spanish, 2 French, 3 German, 4 Italian, 5 Japanese, 6 Russian | 0 | Game text language. |
| `crouch_toggle` | 0 hold / 1 toggle | 0 | How R3 crouch works. |
| `swap_ab_buttons` | 0 / 1 | 0 | Swaps the bottom and right face buttons. |
| `swap_xy_buttons` | 0 / 1 | 0 | Swaps the left and top face buttons. |
| `debug_log` | 0 / 1 | 0 | Detailed tracing in `bully.log`. Only for bug hunting: it slows the game and the log grows fast. |

The game's own menu settings for resolution, effects and shadows are replaced by the `config.txt` values on the next start.

## What the launcher changes while the game runs

Everything below is put back when the game exits, including after a crash:

- CPU governor set to `performance`.
- GPU pinned at 520 MHz. 600 MHz hangs the GPU under load on these boards.
- zram swap resized to 768 MB and `vm.swappiness` set to 100, so idle memory moves to compressed swap and the game gets more RAM.
  - Resizing takes a few seconds before the game appears.
  - If the size is already 768 MB, nothing is resized.

Override any of these from the environment: `BULLY_GPU_MAX_HZ`, `BULLY_ZRAM_SIZE` (bytes), `BULLY_SWAPPINESS`.

## Memory

These handhelds have 1 GB of RAM shared with the GPU, so the port limits the game's memory use:

- The game's texture budget is capped at 256 MB.
- Unused textures are released on area and scene changes, and every 30 seconds.
- Freed memory is handed back to the system every 10 seconds.

If the game is killed (the log ends with `Bully was killed (137)`), it ran out of memory:

- Lower `clarity` or `shadows`.
- Reboot before playing. Memory left fragmented by earlier sessions makes this more likely.

## Saves

Saves and the game's own settings are kept in `ports/bully/savegames/`: `BullyFile<N>`, `FileInfo<N>`, `settings.ini` and `storage.ini`. Saves left in `ports/bully/` by older builds are moved there on the next start.

## Logs

`ports/bully/bully.log` is rewritten on every start. It contains the device and clock settings, display setup, memory events and the exit status. Include it with any bug report.

## Status

Tested on dArkOSen on an R36S (640x480) and an RG351MP-class device (rotated 1280x720 panel). Other RK3326 devices are expected to work.

Movies are not played. The game skips them.

## Building

GitHub Actions builds the port on an arm64 runner in a `debian:trixie` container (`.github/workflows/build.yml`). The artifact `bully-aarch64` contains `bully/bully` and the launcher.

To build on an arm64 Debian Trixie machine:

```bash
sudo apt install build-essential libsdl2-dev libopenal-dev libegl-dev libgles-dev libminizip-dev zlib1g-dev
make
```

## Credits

- `fgsfds` and `Andy Nguyen` for the original wrapper/loader base used by these ports
- `givethesourceplox` for the Bully AArch64 Switch port (bully-NX) this port is based on
- `djparentx` for the dArkOSen port
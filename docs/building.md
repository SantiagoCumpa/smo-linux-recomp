# Building

Everything the build makes from the game is derived from **your own dump** on **your own PC**, into `build/` (ignored
by git). One script does all of it, in steps that resume where they stopped:

```sh
.venv/bin/python scripts/setup.py --dump ./dump
```

No console keys and no firmware are needed: the dump's files are already decrypted by your console, and the build only
converts and reads them.

**From NSP files** (the base game and the 1.4.1 update, with your console's `prod.keys`), one command does everything,
decrypting included, and makes a game folder to play from:

```sh
./smo-recomp --in_dir=<folder with the NSP files> --keys=<prod.keys> --out_dir=<game folder> [--language=es]
```

Decrypting uses `tools/bin/hactool`, a static build of hactool the repository carries
([THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md); `tools/build_hactool.py` rebuilds it, patched with
`tools/hactool/bktr-new-layout.patch` for the 1.4.1 update's RomFS patch). The NSP files may
be two (base and update) or one that carries both; personalized tickets are refused (dump with a common ticket). The
decrypted dump goes to `build/install/dump` (`tools/extract/nsp.py`), then `setup.py` and `tools/make_game_folder.py` run
on it. `./smo-recomp --help` lists the steps; each resumes.

## What you need

- **Linux x86-64** (developed on Arch Linux, Wayland with niri) and a GPU with Vulkan 1.3 (developed on NVIDIA). The
  renderer uses `VK_EXT_graphics_pipeline_library`, `VK_EXT_extended_dynamic_state3`,
  `VK_EXT_vertex_input_dynamic_state` and `VK_EXT_depth_clip_control`. On a hybrid laptop the game is started on the
  discrete GPU (see Running).
- The system packages (Arch names):
  ```sh
  sudo pacman -S clang lld llvm pkgconf python sdl3 vulkan-headers vulkan-icd-loader shaderc ffmpeg freetype2 \
                 fontconfig openssl
  ```
  plus your GPU's Vulkan driver (`nvidia-utils`, `vulkan-radeon` or `vulkan-intel`). Optional: `zenity` or `kdialog`
  (the game's text entry), `xdg-utils` (Help in the web browser), `noto-fonts` and `noto-fonts-cjk` (fonts, below).
  - **clang/lld** compile and link everything (`CLANG` or `LLVM_ROOT` point at another clang). The lifted code is
    built with `-ffp-contract=off`, so a recent clang does not fuse multiply-adds the console's CPU does not.
  - **SDL3**: the window (native Wayland, X11 too), keyboard, game controllers and the sound output.
  - **shaderc** turns the translated GLSL into SPIR-V; it is linked into the program (`runtime/gfx/vk_shaderc_shim.c`).
  - **FFmpeg** (libavformat, libavcodec, libswscale, libswresample) plays the game's H.264 movies.
  - **FreeType** and **fontconfig**: the outlines of the console's shared fonts, and system fonts when you have none.
- **Python 3** in a virtual environment in the repository:
  ```sh
  python -m venv .venv
  .venv/bin/python -m pip install lz4 capstone unicorn numpy Pillow
  ```

`scripts/setup.py` checks for each of these first and names the `pacman` package of what is missing.

the game from your own console with a homebrew dumping tool that writes decrypted files, such as nxdumptool. The
build needs the game as the 1.4.1 update leaves it, in one folder laid out like this:

```
<your dump folder>/
  exefs\          the program's ExeFS with the 1.4.1 update: main, main.npdm, sdk, rtld, subsdk0
  romfs\          the game's RomFS with the 1.4.1 update applied
  control.nacp    the update's control data
  manual\         optional: the Help pages (the RomFS of the game's HTML document content: html-document/...)
  fonts\          optional: the console's shared fonts, as the console stores them (nintendo_udsg-r_std_003.bfttf
                  and the others) or as .ttf files of the same names
```

- **One version:** nothing of the base game (1.0.0) is read. The 1.4.1 executable keeps almost no symbols; the few
  game functions the runtime needs are found in it by what they are and do (`tools/lifter/gen_tables.py`). A dump
  folder laid out for older builds may still hold the base game's ExeFS as `exefs-1.0.0/`: it is ignored.
- **Checks:** the import reads the program id from `main.npdm`, the NintendoSDK version `main` was built with (7.3.2)
  and the display version in `control.nacp` (1.4.1), and looks for the RomFS's own folders. A missing or wrong piece is
  named.
- **Read in place:** `build/romfs` and `build/manual` become symbolic links to your dump's folders (the RomFS is
  about 5.7 GB), so keep the dump where it is. Only the executable modules are converted, into `build/dump`.
- **Upper and lower case:** the game names its files as the console's file system (which ignores case) allows; when a
  name is not found as written, the runtime looks for it ignoring case and remembers the answer.
- **An encrypted dump** (`.nsp` / `.xci`, e.g. the base game and the update as two NSP files) has to be decrypted with
  your own console's keys first, the update's RomFS applied over the base game's: for instance with hactoolnet,
  `hactoolnet -k prod.keys -t nca --basenca <base program NCA> <update program NCA> --exefsdir exefs --romfsdir romfs`, and
  the update's control NCA for `control.nacp`. The build itself never reads keys.
- **Fonts:** the game draws with the console's shared fonts in one place met so far, the map screen's legend and its
  travel-guide pages. Without `fonts\`, free fonts stand in: put Noto Sans (and Noto Sans SC, TC and KR for Chinese
  and Korean) as `.ttf` or `.otf` in `deps/fonts`; without those, the system's fonts for each script through
  fontconfig (on Arch, `noto-fonts` and `noto-fonts-cjk`). The log says which file each font type got.

## The steps

```
deps       check the Python packages, clang, lld, pkg-config and the libraries (pkg-config)
import     your dump -> build/dump (modules as ELF, versions); RomFS and Help read in place   tools/extract/extract.py
functions  every function of each module, with exact boundaries                             tools/extract/functions.py
lift       AArch64 -> C for every function -> build/lift/main                               tools/lifter/recomp.py
compile    the lifted C -> objects (clang -O2, incremental)                                 tools/lifter/build_main.py
tables     generated headers and tables                                       tools/lifter/gen_tables.py, gen_sdktest.py
sdk        the system-library functions that are lifted too                                  tools/lifter/sdklift.py
link       game library + host runtime -> build/bin/libsmo_game.so, smo_native             tools/lifter/build_exe.py
check      differential test against Unicorn, link check (on demand)        tools/lifter/difftest.py, linkcheck.py
```

A step whose output exists is skipped, so an interrupted setup resumes where it stopped. `compile`, `tables` and `link`
always run and only rebuild what changed. `--status` shows what is done, `--only <step>` runs one step, `--redo <step>`
repeats one, `--from <step>` repeats it and everything after it, `--check` adds the check step (minutes). After a change
to `tools/lifter/sdklift_rules.txt` run `--redo sdk`; after a change to the runtime, `--only link` is enough.

## Running

```sh
.venv/bin/python scripts/run.py play --exe smo_native --seconds 0
```

- `smo_native` (and `smo_user`) open a visible window and play sound; `--seconds 0` means no time limit. The program
  is `build/bin/smo_native` with the game in `build/bin/libsmo_game.so` beside it.
- **GPU:** `run.py` starts the game with NVIDIA's PRIME render offload variables (`__NV_PRIME_RENDER_OFFLOAD=1`,
  `__VK_LAYER_NV_optimus=NVIDIA_only`), so a hybrid laptop renders on its discrete GPU; the renderer also prefers a
  discrete GPU among the Vulkan devices. `SMO_PRIME=0` leaves the variables out; `SMO_VK_DEVICE=<index>` (or `gpu = <index or part of the
  name>` in `graphics.ini`) picks a device. The log names the device it chose (`nvn_vk: device ...`).
- **Caches stay in the repository:** the translated shaders in `build/cache/vk`, and the driver's own caches in
  `build/cache` (`run.py` sets `XDG_CACHE_HOME`, `__GL_SHADER_DISK_CACHE_PATH` and `MESA_SHADER_CACHE_DIR`).
  Temporary files go to `build/tmp`.
- Without `--exe` (the name is then `smo_test`), or with any other name, the run is a test: `scripts/run.py` copies
  `smo_native` to that name, keeps the window hidden (`SMO_SHOW=1` shows it) and silent, and stops the game after
  `--seconds` (default 30). Everything the game prints goes to `build/run/<run name>.log`, and a summary (boot stages
  reached, unimplemented system calls, frames presented, a crash with its host frames named) is printed when it ends.
- A test copy first waits while `SMO_MAX_GAMES` test games started this way run on the PC (default: one per 12 GB of
  memory; `0` = no limit). A place is a lock on `build/run/place_<k>.lock`, freed by the kernel when its launcher ends.
  A game in a stage takes 4-7 GB; more games than fit make the PC swap, which slows every one of them and can make
  graphics memory allocations fail.
- `--userdata <folder>` gives the run its own save folder: a new, empty folder starts a new game, a copy of an earlier
  run's folder continues from there. Without it, saves go to `build/userdata/save`.
- `--seq <file>` plays an input sequence (see [`scripted-runs.md`](scripted-runs.md)); `--env NAME=VALUE` sets one of
  the runtime's switches (see [`debugging.md`](debugging.md)).
- Graphics and controller settings live in `build/config/graphics.ini` and `build/config/controllers.ini` (written
  with documented defaults on the first run; `controllers.ini` is read again when you edit it while the game runs).
- The window: by default a borderless window of the size in `graphics.ini` (on Wayland the compositor places it; a
  tiling compositor such as niri tiles it). F11, or a double-click on it, switches between that and fullscreen;
  `window = borderless_fullscreen` starts that way, and `windowed` (with a title bar) and `fullscreen` are there too.
  The game keeps running while its window does not have the focus or is minimised (pads and keyboard count only while
  it has the focus); `unfocused = pause` in `graphics.ini` stops it then instead, as the console stops a game behind
  its HOME menu.
- Help (the pause menu's Action Guide) opens in your desktop's web browser (`xdg-open`). Unlike the console, the game
  keeps running while it is open, and the guide's Choose Mode item does not lead back into the game.
- Text entry (a name the game asks for) opens a `zenity` or `kdialog` dialog when one is installed; without them the
  game's suggested text is taken.
- amiibo: press F1, or both stick buttons of a pad, while you play. The game waits and a menu like its pause menu lists
  the amiibo it knows; up / down (L / R: a page) to choose, A to scan, B (or F1 again) to close. When the game is
  waiting for an amiibo, → is held for you until it has read it; otherwise hold → yourself (the game's amiibo mode)
  and it reads the one you chose. The key and the buttons are set in `controllers.ini`, section `[hotkeys]`.
- The Capture button: F12, or a pad's share button, saves the frame as a BMP in the save folder's `album`.
- Controllers go through SDL3: on Linux a pad needs read access to its `/dev/input/event*` device (the usual
  `uaccess` rule of systemd-logind gives it to the logged-in user).
- Translated shaders are cached in `build/cache/vk`. The first run compiles them as the game meets them and is slower;
  later runs reuse them.

The scripted Cap Kingdom run checks the game from a new save up to Mario's first capture:

```sh
.venv/bin/python tests/run_cap_kingdom.py <new run name>
```

It takes about 5 minutes with a warm shader cache and ends by itself once the capture's message is closed (give a
first run a larger `--seconds`, e.g. 1500); it exits with code 0 when every checkpoint was reached and every health
counter is zero.

## Troubleshooting

- **A tool is missing:** the `deps` step names it with its `pacman` package; another clang can be named with `CLANG`
  or `LLVM_ROOT`.
- **The import stops:** it names the piece it could not find or that is not what it should be (another game, another
  version, an encrypted file). Check the folder layout above.
- **The first minutes are slow, or loading screens pause:** shaders are being translated and compiled; the cache in
  `build/cache/vk` makes the next run faster.
- **"NOT IMPLEMENTED in the Vulkan back end, skipped" in the log:** an NVN feature the renderer does not have yet; the
  game keeps running, but that part of the picture is not the game's. Please report it with the log.
- **A crash:** the log has a `HOST EXCEPTION` block (from the signal handler: SIGSEGV, SIGBUS, SIGILL, SIGFPE,
  SIGABRT) with the guest function, the registers and the host call stack (named from the linker map); `SMO_CRASH_DUMP` and `SMO_WATCH` give more (see [`debugging.md`](debugging.md)).
- **No new frame for 5 seconds:** a watchdog writes every thread's state to the log and to `<exe name>_freeze.txt` (threads read from `/proc/self/task`).
- **The game runs on the integrated GPU:** check the log's `nvn_vk: device` line; set `SMO_VK_DEVICE` or `gpu =` in `graphics.ini`, and make
  sure `nvidia-utils` (or your discrete GPU's Vulkan driver) is installed.

# Building and packaging the rom128 build (Windows, 64-bit)

This folder holds the two scripts used to make the Windows release of the `rom128` branch of
mGBA celio edition (mGBA celio edition 2.0.0 + 128MiB ROM, 16MiB flash save, online link, overclock, ...).

- `build_win.sh` : cmake + ninja with MSYS2 mingw64, then gathers `mgba-celio.exe`, DLLs and Qt plugins into a `dist` folder.
- `package.py`   : makes `mGBA-celio-2.0.0-rom128-<N>-win64.zip` from `dist` and checks its contents.

Every change of this branch is already a commit: **no patch files have to be applied** (unlike older `rom64`-era builds).
Build from a clean checkout of the branch.

## 1. What is different from upstream

See the top of [README.md](../../README.md). In short: 128MiB ROM (4 banks of 32MiB, window at `0x0A000000`),
flash save up to 16MiB, 16MiB extra RAM at `0x01000000`, CPU overclock, built-in online link, save backups, two-level log,
256-color sprite / map-material rendering, and a `portable.ini` so that settings stay next to the exe.

## 2. Requirements

- Windows 10/11, 64-bit.
- [MSYS2](https://www.msys2.org/), using the **MINGW64** environment. Any folder works (`C:\msys64`, `D:\msys64`, ...).
- A source path made of **ASCII characters only** (Qt's `moc` fails in paths that contain Japanese characters).
- About 3GB of free disk space for the build.

MSYS2 packages (mingw64). Versions of the reference build (2026-11):

| package | version |
|---|---|
| mingw-w64-x86_64-gcc | 15.2.0 |
| mingw-w64-x86_64-cmake | 4.2.3 |
| mingw-w64-x86_64-ninja | 1.13.2 |
| mingw-w64-x86_64-pkgconf | 2.5.1 |
| mingw-w64-x86_64-qt5-base | 5.15.18 |
| mingw-w64-x86_64-qt5-multimedia | 5.15.18 |
| mingw-w64-x86_64-qt5-tools | 5.15.18 |
| mingw-w64-x86_64-SDL2 | 2.32.10 |
| mingw-w64-x86_64-lua | 5.4.8 |
| mingw-w64-x86_64-libzip | 1.11.4 |
| mingw-w64-x86_64-libepoxy | 1.5.10 |
| mingw-w64-x86_64-json-c | 0.18 |
| mingw-w64-x86_64-brotli | 1.2.1 (loaded by libfreetype; older 1.2.0 has a known security bug) |
| mingw-w64-x86_64-ffmpeg | 8.0.1 (only for `FFMPEG=ON`, video recording) |

Install (MSYS2 MINGW64 shell):

```sh
pacman -S --needed mingw-w64-x86_64-{gcc,cmake,ninja,pkgconf,qt5-base,qt5-multimedia,qt5-tools,SDL2,lua,libzip,libepoxy,json-c,brotli}
pacman -S --needed mingw-w64-x86_64-ffmpeg          # only if you want video recording
```

If `pacman` fails with an SSL error, download the URLs printed by `pacman -Sp <packages>` with `curl.exe` and install with `pacman -U <file>`.

## 3. Build

In Git Bash (or any bash), from the repository root:

```sh
MSYS2_ROOT=/c/msys64 FFMPEG=ON CELIO_VERSION=rom128-4 DIST=/c/mgba-rom128/dist \
  bash tools/rom128/build_win.sh
```

| variable | meaning | default |
|---|---|---|
| `MSYS2_ROOT` | MSYS2 folder as seen from Git Bash (`/d/msys64`, ...) | `/c/msys64` |
| `DIST` | output folder | `<repo>/dist` |
| `BUILD_DIR` | cmake build folder inside the repository | `build` |
| `FFMPEG` | `ON` = video recording (webm/mp4/GIF). The bundled FFmpeg is GPL, see section 5 | `OFF` |
| `CELIO_VERSION` | version string in the window title (`rom128-N`). **Rewrites `version.cmake` in the working tree.** | unchanged |
| `JOBS` | parallel jobs | `8` |
| `LTO` | `ON` = link time optimization (breaks the Lua bindings; do not use for normal builds) | `OFF` |

Result: `<DIST>/mgba-celio.exe`, the DLLs, `platforms/ imageformats/ audio/ styles/ mediaservice/` and an empty `portable.ini`.
An existing `config.ini` / `qt.ini` / `logs` in `DIST` are kept as they are.
The exe reports the git commit of celio edition 2.0.0 (`MGBA_GIT_COMMIT`) so that Celio-mGBA-Link keeps working.

Manual steps without the script (inside the MSYS2 MINGW64 shell):

```sh
export MGBA_GIT_COMMIT=3da13060a586f2da8eb4ecbb167f642e2e4889c2
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_QT=ON -DBUILD_SDL=ON \
  -DUSE_FFMPEG=OFF -DUSE_DISCORD_RPC=OFF -DUSE_LUA=ON -DBUILD_SHARED=OFF -DBUILD_STATIC=ON -DBUILD_LTO=OFF
cmake --build build -j 8
```

## 4. Make the release zip

```sh
python tools/rom128/package.py --release 4 --dist /c/mgba-rom128/dist --out /c/mgba-rom128/out
```

(`MSYS2_ROOT_WIN=C:/msys64` tells the script where MSYS2 is as a Windows path; it uses `pacman -Qo` to record the package of each DLL.)

Output: `mGBA-celio-2.0.0-rom128-4-win64.zip` and `.sha256`. Inside, the top folder `mGBA-celio-rom128-4/` holds:

- `mgba-celio.exe`, the DLLs, Qt plugins, empty `portable.ini` (no `config.ini` / `qt.ini` / logs / backups / scripts are included)
- `readme.txt` (Japanese usage notes; the line `リリース: rom128-N` must match the zip name), `LICENSE`, `hgx4-abi.md`, `obj256-abi.md`
- `source/rom128-branch.diff` (full diff against celio edition 2.0.0), `source/SOURCE.txt` (commit, build settings, upstream links),
  `source/BUNDLED_DLLS.txt` (MSYS2 package, version and SHA-256 of every DLL)

`SOURCE.txt` records the HEAD commit of the checkout by default (`--commit` overrides). Make the zip from a clean, pushed commit.
The script then checks the zip (required files, no settings, readme release line, GPL notice when FFmpeg is included).
To also check that a name does not appear anywhere in the zip, pass `--forbid-word WORD` (repeatable) or set
`ROM128_FORBIDDEN_WORDS=word1,word2`. `--verify-zip <zip>` only checks an existing zip; `--brotli-pkg <file>` compares the Brotli DLLs
with an MSYS2 package file.

The release number `N` is written in three places that must agree: the `readme.txt` line `リリース: rom128-N`,
`--release N`, and `CELIO_VERSION=rom128-N` at build time. Tag the commit `rom128-N`.

## 5. Licenses and source availability

- mGBA and the changes in this branch: MPL-2.0 (`LICENSE`). Source: this repository
  (https://github.com/pm-start/mGBA_celio_edition, branch `rom128`); upstream https://github.com/Exormeter/mGBA_celio_edition and https://github.com/mgba-emu/mgba.
- With `FFMPEG=ON` the zip contains FFmpeg (`--enable-gpl --enable-version3`), x264, x265, Xvid and vid.stab built by MSYS2.
  These are GPL (https://www.gnu.org/licenses/gpl-3.0.html, https://ffmpeg.org/legal.html) and are shipped unmodified.
  Their sources: https://repo.msys2.org/mingw/sources/ and https://github.com/msys2/MINGW-packages.
  `readme.txt` and `source/BUNDLED_DLLS.txt` in the zip state this; keep both when you distribute.
- Qt 5 (LGPL v3), SDL2 (zlib), Lua (MIT) and the other MSYS2 DLLs keep their own licenses.

## 6. Unit tests

Two small C tests link against `libmgba.a` of a finished build. The gcc command lines are in each `README.txt`.

- `tools/rom128/test_rom128/` : 128MiB bank window (default bank, switching 1..3, 8/16/32-bit reads, wrong key sequences,
  save state round trip, reset, 64MiB / 32MiB unchanged; 50 checks).
- `tools/rom128/test_save16/` : 16MiB flash save (6MiB read/write, no growth on read, banks 64..255, RTC move,
  opening 128KiB / 1MiB / 16MiB saves, save state; 94 checks).

Both return exit code 0 when everything passes.

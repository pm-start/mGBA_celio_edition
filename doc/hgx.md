# HGX2 dedicated map renderer

This branch starts from `origin/rom64` at `b66331411` and adds an explicitly
opted-in background extension for PokemonStart's HG-style maps. Normal ROMs
retain the existing rendering and I/O behavior. HGX ROMs use the synchronous
software renderer, including the matching Qt CPU framebuffer path.

The GBA header reserves bytes B5..B7 for `HGX` and B8..BB for the little-endian
descriptor pointer register address `0x040003E0`. The ROM owns a heap-allocated
descriptor: magic `HGX2`, BG enable mask, four screen-map pointers, generation,
and a global BGR555 fade color/coefficient word. Each 32x32 screen map contains
1024 twelve-byte tile/palette/resource entries. A zero or invalid entry falls
back to ordinary VRAM rendering.

Entries can reference raw 4bpp tiles and independent 16-color palettes in ROM
or RAM. Compressed tiles retain their original ROM addresses and tile indices;
the emulator lazily decodes bounded BIOS LZ77 resources into a host cache.
Restoring a state rebuilds this cache without persistent host resource IDs.
Flip, transparency, windows, OBJ composition, alpha, brightness and palette
fade remain supported. Expected native screen words detect subsequent native
door updates and restore ordinary rendering for changed entries.

Read-only tooling mirrors `0x040003E8` and `0x040003EA` expose the stored BG1
horizontal/vertical offsets. The actual hardware scroll registers are
write-only and must not be used as camera readback values.

The matching game code currently keeps its current-map tiles on native
rendering and uses HGX resources for the neighboring map preview. This retains
current-map animation and game-specific palette effects. Native GBA hardware
does not implement this extension.

Build with CMake and `-DBUILD_STATIC=ON -DBUILD_HGX_TEST=ON`, then run
`mgba-hgx-test`. The synthetic test covers raw and compressed resources,
unsafe reference fallback, native updates, state restoration, opt-in isolation,
tooling mirrors and full-frame compositor equivalence. The current suite
passes 39 checks. Actual-game cold and room-state startup were also checked
with independent PID/port ownership validation.

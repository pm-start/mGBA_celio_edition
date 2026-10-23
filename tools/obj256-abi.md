# Independent 256-color OBJ materials

This opt-in HGX4 extension gives each registered OAM object its own 8bpp frame
and 256-entry palette. It does not expand or reinterpret native OBJ VRAM.
Unregistered objects, old 32-byte HGX4 controls, HGX2/3, and untagged ROMs retain
the original renderer. Tagged ROMs already force synchronous software rendering.

All fields use little-endian GBA addresses and integers. The existing HGX4
control becomes 40 bytes, retaining its original fields. `descriptorBytes`
at +4 is 40; `capabilityFlags` at +20 has bit zero enabled; +32 is objScenePtr;
+36 is objSceneBytes=32. The field BG renderer accepts both 32 and 40 bytes.

OBJ scene (32 bytes):

| Offset | Field |
| --- | --- |
| 0 | serial |
| 4 | entryCount, 0..128 |
| 8 | entriesPtr |
| 12 | entryBytes=16 |
| 16 | global DNS coefficient, clamped to 0..16 |
| 20 | DNS BGR555 target |
| 24 | global fade BGR555 target |
| 28 | fade coefficient, clamped to 0..16 |

Entry (16 bytes), indexed by actual final hardware OAM slot:

| Offset | Field |
| --- | --- |
| 0 | pixelsROMPtr |
| 4 | paletteROMPtr |
| 8 | expected attrA, u16 |
| 10 | expected attrB, u16 |
| 12 | expected attrC, u16 |
| 14 | flags, u16; bit 0 enables, bits 1..3 select source row stride |

Pixels are uncompressed 8bpp in native one-dimensional 8x8 tile order, with
dimensions from the expected OAM shape/size. The byte count is width*height,
at most 4096. Palette data is 256 consecutive little-endian BGR555 colors.
Stride code zero uses the OAM width in tiles; codes 1..7 use 2..8 source tiles
per tile row. Bits 4..15 are reserved and must be zero. The source stride must
be at least the child width in tiles. A child object's pointer may start at a
tile within a larger parent frame; the host checks the complete strided ROM
span and copies only the child's tile columns into its packed snapshot.
For example, a 16x16 child within a 32x32 parent uses stride code 3 (flags 7).
Both resources must reside completely in ROM. Index zero is transparent;
indices 1..255 are opaque, including entries whose color is black.

The game builds the indexed entry table after OAM sorting and publishes its
inactive scene pointer last. At scanline zero the host verifies the entire
A/B/C signature against the same native OAM slot, copies the frame and palette,
applies DNS to nonblack colors, then applies fade with exact five-bit rounding.
Subsequent accesses verify the signature again. Missing capability, invalid
scene/entry/resource, unknown flags, unsupported shape, or changed signature
falls back to native rendering without retaining an earlier sidecar material.

Position, clipping, flip, affine matrix/double size, mosaic, priority,
semi-transparency, object windows, brightness and highlighting remain in the
existing software OBJ compositor. Registered entries select its 8bpp loops
without requiring the native OAM color-depth bit, and use their own 1D source
stride independently of native VRAM mapping. Native OAM/palette memory is not
modified. Native global OBJ palette values cannot collide with private palettes.

Special game palette effects beyond the global DNS/fade must unregister the
affected sprite until explicitly represented. Host material caches are not
serialized; reset, ROM switch and state restore discard them, and the next
frame reconstructs them from serialized pointers and signatures.

`mgba-hgx-test` uses original synthetic images to verify high palette indices,
independent actors, transparency, flips, affine/double-size transforms,
multi-tile addressing, windows, brightness, blending flags, malformed and stale
bindings, field BG coexistence, native opt-out and save/load reconstruction.

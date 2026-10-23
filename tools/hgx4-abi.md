# HGX4 field renderer

HGX4 is an independent software BG path. HGX2/3 remain supported unchanged.
The ROM header reserves B5..B7 for `HGX`, and B8 contains either the control
address or the private pointer register address `0x040003E0`. Tagged ROMs use
the synchronous software renderer regardless of the user's GPU/thread setting.

All fields below are little-endian u32 unless stated otherwise. Pointers are
GBA addresses, never host cache IDs. Canonical material resources and animation
frame sources reside in immutable ROM. Native BG VRAM/tilemaps are not inputs.

## Published structures

Control (32 bytes): magic `0x34584748`, controlBytes=32, sceneBytes=64,
cellBytes=16, scenePtr, capabilities, animationLookupPtr, animationLookupCount.
The optional [OBJ256 extension](obj256-abi.md) expands the control to 40 bytes;
the field renderer accepts both control sizes.

Scene (64 bytes): serial, enabledBgMask, cellsPtr, columns=16, rows=12,
signed originScreenX, signed originScreenY, animationTick, dnsCoefficient,
dnsColour555, dnsPaletteMask, overlaysPtr, overlayCount, cellGeneration,
fadeColour555, fadeCoefficient. The scene origin is the screen position of
the first cell's top-left pixel. A cell is 16 by 16 pixels.

Cell (16 bytes): ownerLayoutPtr, metatileId, paddingLayoutPtr,
paddingRefsPtrWithNorthBit. Padding refs point to eight u16 tile references;
bit zero of that pointer marks north-overhang ordering.

Overlay (36 bytes): signed column, signed row, ownerLayoutPtr, layerAndFlags,
eight u16 literal tile references, sourceTilesPtr. Layer low two bits select
the native metatile plane arrangement. Bit 8 makes the lower half intrinsically
transparent; bit 9 makes the upper half intrinsically transparent. An explicit
nonzero sourceTilesPtr supplies ROM tiles 1016..1023 at consecutive 32-byte
offsets. This represents native door frames without reading their VRAM DMA
destination. Source zero uses ordinary material resolution.

The game prepares cells and scene in inactive buffers, then publishes the
aligned scene pointer last. It reuses overlapping cells on walking and resolves
only entering strips or dirty cells. Warps may build a new complete scene.
The host snapshots the entire scene, cell grid and overlays at scanline zero,
resolves immutable commands, and uses those commands until the next frame.
Midframe publications neither tear the material snapshot nor hold a framebuffer.

## Materials

Layout +16/+20 point to primary/secondary tilesets. Tileset fields are:
compression byte +0; tilesPtr +4; palettesPtr +8; metatileRefsPtr +12;
animationCallbackKey +16; metatileAttributesPtr +20.

Metatile IDs and tile references split at 640. Secondary tile indices subtract
640. Palette banks split at 7, but both palette arrays use global bank indices
0..15. Each metatile has eight u16 tile references; attributes are u32 and layer
type is `(attributes >> 29) & 3`. Each reference uses the native ten-bit tile
index, H/V flip bits 10/11, and palette-bank bits 12..15.

Layer 0 uses BG2 lower/BG1 upper, layer 1 BG3 lower/BG2 upper, and layer 2 BG3
lower/BG1 upper. Other planes are transparent independently of atlas tile zero
or tile twenty. Padding follows the game's north/south source ordering, using
explicit tile alpha. Native OBJ, windows, priorities and blending still use
the existing compositor. DNS applies to nonblack colors in selected banks,
followed by fade; both use exact five-bit interpolation.

## Animation

Lookup (12 bytes): callbackKey, role (0 primary, 1 secondary), schedulePtr.

Schedule (20 bytes): period, tileStripCount, tileStripsPtr,
paletteStripCount, paletteStripsPtr.

Strip (16 bytes): localStartTile or globalFirstColor, tileCount or colorCount,
eventCount, eventsPtr. Event (8 bytes): tickWithinPeriod, sourceROMPtr.
Tile event sources contain 32 bytes per tile; palette sources contain two bytes
per color. Secondary tile strips use local tile indices. Palette indices are
global 0..255, matching the palette arrays.

For `animationTick % period`, the latest cyclic event wins. Before the first
event, use the previous cycle's final event. Cold/unvisited materials therefore
never expose placeholder animation pixels. The same material and serialized
game clock produce the same pixels when current, foreign, or revisited. Host
wall time and previous visits do not participate. All caches rebuild after
reset or state restore.

`mgba-hgx-test` exercises the actual software compositor with synthetic ROM
resources; it does not require a commercial ROM or modify a save file.

## Optional 256-color map chips, version one

A tileset opts in by setting its native padding bytes +2 to `0xA8` and +3 to
`1`. All other native fields retain their offsets. A marked descriptor has an
additional +24 legacyPaletteROMPtr, making it 28 bytes. Unmarked tilesets retain
the original 24-byte layout and four-bit behavior.

The marked tileset's +4 source contains uncompressed 64-byte 8bpp tiles; its
+8 palette contains exactly 256 BGR555 colors, 512 bytes. Both sources reside in
ROM. Compression byte +0 must be zero in this version. Tile numbers still split
at 640, secondary tile numbers subtract 640, and H/V flip bits retain their
native meanings. The reference palette nibble is ignored: each tile uses the
palette of its own primary/secondary tile role. DNS eligibility uses role bank
zero for primary, seven for secondary. Alpha depends on the pixel index: zero
is transparent; every other index is opaque, including RGB black.

Native four-bit tiles, marked eight-bit tiles and private OBJ256 palettes may
coexist in the same frame. Shared palette snapshots avoid replicating 256
colors in every tile command. Legacy four-bit tile/palette animation recipes
are not applied to marked materials; version one displays their static base
frame. Eight-bit animation/compressed sources need a future explicit recipe.

The optional legacy palette pointer retains the original complete 256-entry
native banked palette. A native four-bit tile whose palette owner is marked
uses this legacy array. Explicit 32-byte door frames also remain four-bit and
select their original bank from this array. An unavailable legacy door palette,
unsupported marked codec/version, or invalid overlay resource rejects the
entire overlay before replacing any base-cell command. No native VRAM fallback
or reinterpretation of 32-byte frame recipes as 64-byte tiles is introduced.

## Optional scaled 256-color map chips, version one

Tag byte +2 may instead be `0xA9`, with version byte +3 equal to `1`. This is a
40-byte descriptor. Fields +0 through +24 have the same meaning as the A8
descriptor, including `legacyPaletteROMPtr` at +24. The added fields are:

* +28 `tileSide`: one of 8, 16, 32, 64, or 128 pixels
* +32 `tileCount`: number of square tiles in the source
* +36 `resourceBytes`: exactly `tileSide * tileSide * tileCount`

The +4 source is raw, row-major 8bpp indexed data with `tileSide * tileSide`
bytes per tile. Tile indices and primary/secondary normalization are unchanged,
so one native 8 by 8 tile reference still selects one square source image.
The renderer samples that image into the native 8 by 8 footprint using the
centre of each equal source region. Thus tile sides 8, 16, 32, 64, and 128
correspond to authored 16, 32, 64, 128, and 256 pixel metatiles while the
logical cell grid, camera range, and 240 by 160 output remain unchanged.

With compression byte +0 equal to zero, version one accepts the raw 8bpp source
described above. Zero tile count, unsupported sides, out-of-range tile indices,
inconsistent byte counts, inaccessible ROM ranges, and unknown A9 versions are
rejected. Visibility
checks inspect all `tileSide * tileSide` indices. Palette, index-zero alpha,
flip, DNS/fade, padding, overlays, doors, animation isolation, and native
compositor behavior otherwise match A8. This ABI only accepts and samples
higher-resolution ROM resources; it does not generate those resources or force
a 4K host output mode.

Compression byte +0 equal to `2` makes +4 point to an HGZ1 container. Its
20-byte little-endian header is `magic=0x315A4748`, `codec=1`, `rawBytes`,
`packedBytes`, and `crc32`, followed immediately by one zlib-wrapped deflate
payload. `rawBytes` must exactly equal descriptor `resourceBytes`; the latter
continues to describe the unpacked tile atlas. The palette, metatile references,
attributes, and legacy palette remain uncompressed and keep their existing
pointers.

The decoder limits a single unpacked HGZ1 resource to 16 MiB and the live HGX4
resource cache to 1024 entries and 128 MiB. It requires an exact zlib stream end,
complete input consumption, exact output size, and matching CRC32. Truncated,
mutated, trailing, oversized, or otherwise inconsistent containers are rejected.
Stale resources may be evicted only at a frame boundary; resources referenced by
the current immutable commands retain stable pointers. The cache therefore does
not fill permanently merely by visiting additional maps.

The same HGZ1 container is also accepted when compression byte +0 is `2` on an
A8 descriptor or an unmarked legacy 24-byte tileset. For A8, `rawBytes` must be
a nonzero multiple of 64 and each tile reference selects one 64-byte indexed
tile. For legacy tilesets it must be a nonzero multiple of 32 and each reference
selects one native 32-byte four-bit tile. In both cases a reference outside the
unpacked atlas is rejected. A8 palettes and legacy banked palettes stay raw.
Legacy animation lookup still runs first, so a matching 32-byte animation event
overrides the compressed base tile exactly as it overrides an uncompressed or
GBA-LZ base atlas. Compression values zero and one retain their previous raw and
GBA-LZ meanings; A8 retains zero as raw and adds only value two.

## A9 day/night palettes, version two

A9 version byte +3 equal to `2` extends the version-one descriptor from 40 to
48 bytes. Every version-one field and raw/HGZ1 tile rule stays at the same
offset. The added fields are +40 `nightPaletteROMPtr` and +44 `paletteCount`.
The count is 1 through 256 and includes transparent index zero. The day palette
at +8 and night palette at +40 each contain exactly `paletteCount` little-endian
BGR555 entries. Tile indices are unchanged and are never quantized; if any
index in a resolved square tile is at least `paletteCount`, that material is
rejected and remains transparent.

The control's sceneBytes field may be 64 for the original scene or 68 for the
extended scene. The latter appends +64 `nightMix`, from 0 through 16. A 64-byte
scene behaves as `nightMix=0`. A9 version two mixes each five-bit BGR channel as
`(day * (16-nightMix) + night * nightMix) >> 4`, making zero exact day and 16
exact night. Palette index zero remains transparent regardless of its stored
color. Version two palettes bypass the older DNS palette tint to avoid applying
the environment twice; the independent scene fade still applies afterward.

The night pointer, day pointer, count, scene mix, HGZ1 size, and every tile
index are validated before rendering. A9 version one, A8, legacy materials,
64-byte scenes, doors, animations, and their existing DNS behavior are
unchanged. The producer may publish day or night directly with mix 0 or 16;
the ABI does not require or generate additional interpolation tables.

## Tagged material metadata pointers

Tileset pointer fields `+8` (palette), `+12` (metatile references), and `+20`
(metatile attributes), plus marked A8/A9 `+24` (legacy palette) and A9v2 `+40`
(night palette), reserve pointer bit zero. An aligned pointer with bit zero clear
is read directly. Bit zero set means that `pointer & ~3` is the base of an HGZ1
container containing the complete uncompressed blob.

Consumers pass the base pointer and a separate byte offset. Palette bank
selection therefore reads `(base, bank * 32, 32)`, and metatile selection reads
`(base, id * 16, 16)` or `(base, id * 4, 4)`. The decoder validates HGZ1 size,
codec, exact compressed length, CRC32, and `offset + length` before returning a
slice. A tagged pointer whose aligned base is not HGZ1 is invalid. Raw pointers
must remain four-byte aligned.

Logical map, logical attribute, border, tile-atlas, and animation-source
pointers remain raw and untagged. This keeps native logic buffers and tile
descriptors byte-addressable while allowing only dedicated material metadata
to use HGZ1.

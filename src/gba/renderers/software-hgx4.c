/* Copyright (c) 2026 mGBA contributors
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. https://mozilla.org/MPL/2.0/ */
#include "gba/renderers/software-private.h"
#include <mgba/internal/gba/gba.h>

/* HGX4 is independent of HGX2/3. The game publishes ownership cells, camera,
 * clock and environment atomically. At scanline zero these become immutable
 * material commands. No field BG pixel reads native VRAM or its tilemap, and
 * scene changes never hold or restore a framebuffer. Native OBJ/UI compositor
 * semantics remain shared with mGBA. All caches are rebuildable after states. */
#define H4_COLUMNS 16
#define H4_ROWS 12
#define H4_CELLS (H4_COLUMNS * H4_ROWS)
#define H4_OVERLAYS 64
#define H4_RESOURCES 32
#define H4_PALETTES 4096

struct H4Tile {
	const uint8_t* pixels;
	const uint16_t* palette;
	uint16_t ref;
	uint8_t depth, paletteBank;
};
struct H4Palette {
	uint32_t source, callback;
	unsigned role, firstColor, count;
	uint16_t colors[256];
};
struct H4Resource { uint32_t source; uint8_t* data; size_t size; };
struct H4MaterialCache { uint32_t layout; uint16_t ref; struct H4Tile tile; };
struct GBAVideoHGX4 {
	bool valid;
	uint32_t scene[16];
	uint32_t cells[H4_CELLS][4];
	uint8_t overlays[H4_OVERLAYS][36];
	uint32_t animationTable, animationCount;
	struct H4Tile tiles[3][H4_CELLS][4];
	struct H4Resource resources[H4_RESOURCES];
	unsigned resourceCount;
	struct H4MaterialCache material[4096];
	struct H4Palette palettes[H4_PALETTES];
	unsigned paletteCount;
};

static const uint8_t* _h4Read(struct GBA* gba, uint32_t address, size_t size) {
	uint32_t offset;
	if (address >= 0x02000000 && address < 0x02040000) {
		offset = address - 0x02000000;
		if (size <= 0x40000 - offset) { return (const uint8_t*) gba->memory.wram + offset; }
	} else if (address >= 0x03000000 && address < 0x03008000) {
		offset = address - 0x03000000;
		if (size <= 0x8000 - offset) { return (const uint8_t*) gba->memory.iwram + offset; }
	} else if (address >= 0x08000000 && address < 0x0C000000) {
		offset = address - 0x08000000;
		if (offset <= gba->memory.romSize && size <= gba->memory.romSize - offset) {
			return (const uint8_t*) gba->memory.rom + offset;
		}
	}
	return NULL;
}
static uint32_t _h4Word(const uint8_t* bytes, unsigned offset) {
	uint32_t value; LOAD_32(value, offset, bytes); return value;
}
static uint16_t _h4Half(const uint8_t* bytes, unsigned offset) {
	uint16_t value; LOAD_16(value, offset, bytes); return value;
}

void GBAVideoSoftwareRendererResetHGX4(struct GBAVideoSoftwareRenderer* renderer) {
	if (!renderer->hgx4) { return; }
	for (unsigned n = 0; n < renderer->hgx4->resourceCount; ++n) { free(renderer->hgx4->resources[n].data); }
	free(renderer->hgx4);
	renderer->hgx4 = NULL;
}

static const uint8_t* _h4Lz(struct GBAVideoSoftwareRenderer* renderer, uint32_t source, unsigned tile) {
	struct GBAVideoHGX4* state = renderer->hgx4;
	for (unsigned n = 0; n < state->resourceCount; ++n) {
		struct H4Resource* resource = &state->resources[n];
		if (resource->source == source) {
			return resource->size >= (tile + 1U) * 32 ? resource->data + tile * 32 : NULL;
		}
	}
	/* ROM resources persist across scenes; never evict a pointer used by this
	 * frame. 32 atlases cover current + four distinct neighboring map pairs. */
	if (state->resourceCount == H4_RESOURCES) { return NULL; }
	const uint8_t* header = _h4Read(renderer->d.p, source, 4);
	if (!header) { return NULL; }
	uint32_t value = _h4Word(header, 0);
	size_t size = value >> 8;
	if ((value & 255) != 0x10 || !size || size > 0x10000) { return NULL; }
	uint8_t* output = malloc(size);
	if (!output) { return NULL; }
	unsigned cursor = 4;
	size_t pos = 0;
	while (pos < size) {
		const uint8_t* input = _h4Read(renderer->d.p, source + cursor++, 1);
		if (!input) { goto invalid; }
		unsigned flags = *input;
		for (unsigned bit = 0; bit < 8 && pos < size; ++bit) {
			if (flags & (0x80 >> bit)) {
				input = _h4Read(renderer->d.p, source + cursor, 2); cursor += 2;
				if (!input) { goto invalid; }
				unsigned count = (input[0] >> 4) + 3;
				unsigned distance = ((input[0] & 15) << 8 | input[1]) + 1;
				if (distance > pos) { goto invalid; }
				while (count-- && pos < size) { output[pos] = output[pos - distance]; ++pos; }
			} else {
				input = _h4Read(renderer->d.p, source + cursor++, 1);
				if (!input) { goto invalid; }
				output[pos++] = *input;
			}
		}
	}
	state->resources[state->resourceCount++] = (struct H4Resource) { source, output, size };
	return size >= (tile + 1U) * 32 ? output + tile * 32 : NULL;
invalid:
	free(output); return NULL;
}

static const uint8_t* _h4Animated(struct GBAVideoSoftwareRenderer* renderer,
                                const uint8_t* tileset, unsigned role, unsigned tile, bool palette) {
	struct GBAVideoHGX4* state = renderer->hgx4;
	uint32_t callback = _h4Word(tileset, 16);
	const uint8_t* table = _h4Read(renderer->d.p, state->animationTable, state->animationCount * 12);
	if (!callback || !table) { return NULL; }
	const uint8_t* result = NULL;
	for (unsigned n = 0; n < state->animationCount; ++n) {
		const uint8_t* entry = table + n * 12;
		if (_h4Word(entry, 0) != callback || _h4Word(entry, 4) != role) { continue; }
		const uint8_t* schedule = _h4Read(renderer->d.p, _h4Word(entry, 8), 20);
		if (!schedule) { break; }
		unsigned period = _h4Word(schedule, 0), count = _h4Word(schedule, palette ? 12 : 4);
		if (!period || count > 1024) { break; }
		unsigned phase = state->scene[7] % period;
		const uint8_t* strips = _h4Read(renderer->d.p, _h4Word(schedule, palette ? 16 : 8), count * 16);
		if (!strips) { break; }
		for (unsigned s = 0; s < count; ++s) {
			const uint8_t* strip = strips + s * 16;
			unsigned first = _h4Word(strip, 0), length = _h4Word(strip, 4), events = _h4Word(strip, 8);
			if (tile < first || tile - first >= length || !events || events > 65536) { continue; }
			const uint8_t* frames = _h4Read(renderer->d.p, _h4Word(strip, 12), events * 8);
			if (!frames) { continue; }
			/* Previous cyclic event also initializes a cold atlas before its first
			 * phase. Thus tick zero never exposes original placeholder artwork. */
			unsigned bestAge = UINT32_MAX;
			uint32_t source = 0;
			for (unsigned e = 0; e < events; ++e) {
				unsigned tick = _h4Word(frames, e * 8);
				if (tick >= period) { continue; }
				unsigned age = phase >= tick ? phase - tick : period - (tick - phase);
				if (age < bestAge) { bestAge = age; source = _h4Word(frames, e * 8 + 4); }
			}
			if (source) { unsigned unit = palette ? 2 : 32; result = _h4Read(renderer->d.p, source + (tile - first) * unit, unit); }
		}
		break;
	}
	return result;
}

static bool _h4Is8(const uint8_t* set) { return set[2] == 0xA8 && set[3] == 1; }

/* Palette snapshots are shared by material commands instead of embedding
 * 256 colors in every tile and every material-cache entry. */
static const uint16_t* _h4Palette(struct GBAVideoSoftwareRenderer* renderer, uint32_t source,
                                const uint8_t* set, unsigned role, unsigned firstColor, unsigned count,
                                bool animate) {
	struct GBAVideoHGX4* state = renderer->hgx4;
	uint32_t callback = animate ? _h4Word(set, 16) : 0;
	for (unsigned n = 0; n < state->paletteCount; ++n) {
		const struct H4Palette* cached = &state->palettes[n];
		if (cached->source == source && cached->callback == callback && cached->role == role
		    && cached->firstColor == firstColor && cached->count == count) { return cached->colors; }
	}
	const uint8_t* raw = _h4Read(renderer->d.p, source, count * 2);
	if (!raw || state->paletteCount == H4_PALETTES) { return NULL; }
	struct H4Palette* cached = &state->palettes[state->paletteCount++];
	cached->source = source; cached->callback = callback; cached->role = role;
	cached->firstColor = firstColor; cached->count = count;
	for (unsigned color = 0; color < count; ++color) {
		const uint8_t* animated = animate ? _h4Animated(renderer, set, role, firstColor + color, true) : NULL;
		cached->colors[color] = animated ? _h4Half(animated, 0) : _h4Half(raw, color * 2);
	}
	return cached->colors;
}

static const uint16_t* _h4LegacyPalette(struct GBAVideoSoftwareRenderer* renderer,
                                      const uint8_t* layout, uint16_t ref) {
	unsigned bank = ref >> 12, role = bank >= 7;
	uint32_t address = _h4Word(layout, 16 + role * 4);
	const uint8_t* set = _h4Read(renderer->d.p, address, 24);
	if (!set) { return NULL; }
	if (set[2] == 0xA8 && !_h4Is8(set)) { return NULL; }
	uint32_t source = _h4Word(set, 8);
	if (_h4Is8(set)) {
		set = _h4Read(renderer->d.p, address, 28);
		if (!set) { return NULL; }
		source = _h4Word(set, 24);
		/* A marked set retains the original complete native palette specifically
		 * for native4 materials and explicit 32-byte door frame sources. */
		if (source < 0x08000000 || source >= 0x0C000000 || !_h4Read(renderer->d.p, source, 512)) { return NULL; }
	}
	return _h4Palette(renderer, source + bank * 32, set, role, bank * 16, 16, !_h4Is8(set));
}

static struct H4Tile _h4MaterialUncached(struct GBAVideoSoftwareRenderer* renderer, uint32_t layoutAddress, uint16_t ref) {
	struct H4Tile result = { .ref = ref, .depth = 4, .paletteBank = ref >> 12 };
	const uint8_t* layout = _h4Read(renderer->d.p, layoutAddress, 24);
	if (!layout) { return result; }
	unsigned index = ref & 1023, role = index >= 640;
	const uint8_t* set = _h4Read(renderer->d.p, _h4Word(layout, 16 + role * 4), 24);
	if (!set) { return result; }
	index -= role * 640;
	uint32_t source = _h4Word(set, 4);
	if (_h4Is8(set)) {
		/* Version one is raw8 only. Legacy 32-byte animation recipes cannot be
		 * reinterpreted as 64-byte tiles; marked materials use their base frame. */
		if (set[0] || source < 0x08000000 || source >= 0x0C000000) { return result; }
		result.depth = 8; result.paletteBank = role ? 7 : 0;
		result.pixels = _h4Read(renderer->d.p, source + index * 64, 64);
		uint32_t palette = _h4Word(set, 8);
		if (palette >= 0x08000000 && palette < 0x0C000000) { result.palette = _h4Palette(renderer, palette, set, role, 0, 256, false); }
	} else {
		if (set[2] == 0xA8) { return result; } /* Unknown marked version. */
		result.pixels = _h4Animated(renderer, set, role, index, false);
		if (!result.pixels) { result.pixels = set[0] ? _h4Lz(renderer, source, index) : _h4Read(renderer->d.p, source + index * 32, 32); }
		result.palette = _h4LegacyPalette(renderer, layout, ref);
	}
	return result;
}

static struct H4Tile _h4DoorTile(struct GBAVideoSoftwareRenderer* renderer, uint32_t layoutAddress,
                               uint16_t ref, uint32_t source) {
	struct H4Tile result = { .ref = ref, .depth = 4, .paletteBank = ref >> 12 };
	const uint8_t* layout = _h4Read(renderer->d.p, layoutAddress, 24);
	if (!layout || source < 0x08000000 || source >= 0x0C000000) { return result; }
	result.pixels = _h4Read(renderer->d.p, source + ((ref & 1023) - 1016) * 32, 32);
	result.palette = _h4LegacyPalette(renderer, layout, ref);
	return result;
}

static struct H4Tile _h4Material(struct GBAVideoSoftwareRenderer* renderer, uint32_t layout, uint16_t ref) {
	if (!layout) { return (struct H4Tile) { 0 }; }
	unsigned key = ((layout >> 2) * 31U + ref * 2654435761U) & 4095;
	for (unsigned probe = 0; probe < 4096; ++probe) {
		struct H4MaterialCache* cached = &renderer->hgx4->material[(key + probe) & 4095];
		if (!cached->layout) {
			cached->layout = layout; cached->ref = ref;
			cached->tile = _h4MaterialUncached(renderer, layout, ref);
			return cached->tile;
		}
		if (cached->layout == layout && cached->ref == ref) { return cached->tile; }
	}
	return _h4MaterialUncached(renderer, layout, ref);
}

static bool _h4Visible(const struct H4Tile* tile) {
	if (!tile->pixels) { return false; }
	for (unsigned n = 0; n < (tile->depth == 8 ? 64U : 32U); ++n) { if (tile->pixels[n]) { return true; } }
	return false;
}

static bool _h4Cell(struct GBAVideoSoftwareRenderer* renderer, unsigned cell,
                    uint32_t layout, const uint8_t* refs, unsigned layer, uint32_t sourceTiles, bool overlay) {
	struct GBAVideoHGX4* state = renderer->hgx4;
	struct H4Tile layers[3][4] = { 0 };
	for (unsigned q = 0; q < 4; ++q) {
		struct H4Tile entries[2] = { 0 };
		for (unsigned half = 0; half < 2; ++half) {
			if (layer & (0x100U << half)) { continue; }
			uint16_t ref = _h4Half(refs, (q + half * 4) * 2);
			bool door = sourceTiles && (ref & 1023) >= 1016;
			entries[half] = door ? _h4DoorTile(renderer, layout, ref, sourceTiles) : _h4Material(renderer, layout, ref);
			/* Invalid or unavailable legacy door palettes reject the whole overlay
			 * before any base-cell command is replaced. */
			if ((door || overlay) && (!entries[half].pixels || !entries[half].palette)) { return false; }
		}
		if ((layer & 3) == 0) { layers[1][q] = entries[0]; layers[0][q] = entries[1]; }
		else if ((layer & 3) == 1) { layers[2][q] = entries[0]; layers[1][q] = entries[1]; }
		else if ((layer & 3) == 2) { layers[2][q] = entries[0]; layers[0][q] = entries[1]; }
	}
	for (unsigned bg = 0; bg < 3; ++bg) { memcpy(state->tiles[bg][cell], layers[bg], sizeof(layers[bg])); }
	return true;
}

static void _h4Padding(struct GBAVideoSoftwareRenderer* renderer, unsigned cell, const uint8_t* baseRefs) {
	struct GBAVideoHGX4* state = renderer->hgx4;
	uint32_t* data = state->cells[cell];
	const uint8_t* refs = _h4Read(renderer->d.p, data[3] & ~1U, 16);
	if (!refs || !data[2]) { return; }
	bool north = data[3] & 1;
	for (unsigned q = 0; q < 4; ++q) {
		struct H4Tile candidates[4] = {
			_h4Material(renderer, data[0], _h4Half(baseRefs, q * 2)),
			_h4Material(renderer, data[0], _h4Half(baseRefs, (q + 4) * 2)),
			_h4Material(renderer, data[2], _h4Half(refs, q * 2)),
			_h4Material(renderer, data[2], _h4Half(refs, (q + 4) * 2))
		};
		if (!_h4Visible(&candidates[2]) && !_h4Visible(&candidates[3])) { continue; }
		unsigned order[4] = { 0, north ? 1 : 2, north ? 2 : 3, north ? 3 : 1 };
		unsigned visible[4], count = 0;
		for (unsigned n = 0; n < 4; ++n) { if (_h4Visible(&candidates[order[n]])) { visible[count++] = order[n]; } }
		if (!count || count > 3) { continue; }
		for (unsigned bg = 0; bg < 3; ++bg) { memset(&state->tiles[bg][cell][q], 0, sizeof(struct H4Tile)); }
		if (count == 3) { for (unsigned n = 0; n < 3; ++n) { state->tiles[2 - n][cell][q] = candidates[visible[n]]; } }
		else { if (count == 2) { state->tiles[2][cell][q] = candidates[visible[0]]; } state->tiles[0][cell][q] = candidates[visible[count - 1]]; }
	}
}

void GBAVideoSoftwareRendererLatchHGX4(struct GBAVideoSoftwareRenderer* renderer) {
	if (renderer->hgx4) { renderer->hgx4->valid = false; }
	struct GBA* gba = renderer->d.p;
	if (!gba || !gba->memory.rom || gba->memory.romSize < 0xBC) { return; }
	const uint8_t* rom = (const uint8_t*) gba->memory.rom;
	if (memcmp(rom + 0xB5, "HGX", 3)) { return; }
	uint32_t address = _h4Word(rom, 0xB8);
	if (address == 0x040003E0) { address = gba->memory.io[0x1F0] | ((uint32_t) gba->memory.io[0x1F1] << 16); }
	const uint8_t* control = _h4Read(gba, address, 32);
	if (!control || _h4Word(control, 0) != 0x34584748 || (_h4Word(control, 4) != 32 && _h4Word(control, 4) != 40)
	    || _h4Word(control, 8) != 64 || _h4Word(control, 12) != 16) { return; }
	const uint8_t* scene = _h4Read(gba, _h4Word(control, 16), 64);
	if (!scene || _h4Word(scene, 12) != H4_COLUMNS || _h4Word(scene, 16) != H4_ROWS) { return; }
	const uint8_t* cells = _h4Read(gba, _h4Word(scene, 8), H4_CELLS * 16);
	unsigned overlays = _h4Word(scene, 48);
	const uint8_t* overlayData = overlays ? _h4Read(gba, _h4Word(scene, 44), overlays <= H4_OVERLAYS ? overlays * 36 : 0) : NULL;
	if (!cells || overlays > H4_OVERLAYS || (overlays && !overlayData)) { return; }
	if (!renderer->hgx4) { renderer->hgx4 = calloc(1, sizeof(*renderer->hgx4)); }
	struct GBAVideoHGX4* state = renderer->hgx4;
	if (!state) { return; }
	for (unsigned n = 0; n < 16; ++n) { state->scene[n] = _h4Word(scene, n * 4); }
	for (unsigned c = 0; c < H4_CELLS; ++c) { for (unsigned n = 0; n < 4; ++n) { state->cells[c][n] = _h4Word(cells, c * 16 + n * 4); } }
	if (overlays) { memcpy(state->overlays, overlayData, overlays * 36); }
	state->animationTable = _h4Word(control, 24);
	state->animationCount = _h4Word(control, 28);
	if (state->animationCount > 1024) { return; }
	/* Release caches only at a scene boundary, before any commands retain
	 * pointers. No lifetime/visit-count limit on world traversal. */
	if (state->resourceCount > H4_RESOURCES - 10) {
		for (unsigned n = 0; n < state->resourceCount; ++n) { free(state->resources[n].data); }
		state->resourceCount = 0;
	}
	memset(state->tiles, 0, sizeof(state->tiles));
	memset(state->material, 0, sizeof(state->material));
	state->paletteCount = 0;
	for (unsigned c = 0; c < H4_CELLS; ++c) {
		const uint8_t* layout = _h4Read(gba, state->cells[c][0], 24);
		if (!layout) { continue; }
		unsigned id = state->cells[c][1] & 1023, role = id >= 640;
		id -= role * 640;
		const uint8_t* set = _h4Read(gba, _h4Word(layout, 16 + role * 4), 24);
		if (!set) { continue; }
		const uint8_t* refs = _h4Read(gba, _h4Word(set, 12) + id * 16, 16);
		const uint8_t* attrs = _h4Read(gba, _h4Word(set, 20) + id * 4, 4);
		if (!refs || !attrs) { continue; }
		_h4Cell(renderer, c, state->cells[c][0], refs, (_h4Word(attrs, 0) >> 29) & 3, 0, false);
		_h4Padding(renderer, c, refs);
	}
	for (unsigned n = 0; n < overlays; ++n) {
		const uint8_t* overlay = state->overlays[n];
		unsigned col = _h4Word(overlay, 0), row = _h4Word(overlay, 4);
		if (col >= H4_COLUMNS || row >= H4_ROWS) { continue; }
		unsigned c = row * H4_COLUMNS + col;
		_h4Cell(renderer, c, _h4Word(overlay, 8), overlay + 16, _h4Word(overlay, 12), _h4Word(overlay, 32), true);
	}
	state->valid = true;
}

static uint16_t _h4Tint(uint16_t raw, unsigned target, unsigned coefficient) {
	if (coefficient > 16) { coefficient = 16; }
	unsigned result = 0;
	for (unsigned shift = 0; shift < 15; shift += 5) {
		result |= ((((raw >> shift) & 31) * (16 - coefficient) + ((target >> shift) & 31) * coefficient) >> 4) << shift;
	}
	return result;
}

bool GBAVideoSoftwareRendererDrawHGX4(struct GBAVideoSoftwareRenderer* renderer,
                                    struct GBAVideoSoftwareBackground* background, int y) {
	struct GBAVideoHGX4* state = renderer->hgx4;
	if (!state || !state->valid || background->index < 1 || background->index > 3
	    || !(state->scene[1] & (1U << background->index))) { return false; }
	unsigned mosaicH = 1;
	if (background->mosaic) { unsigned mosaicV = GBAMosaicControlGetBgV(renderer->mosaic) + 1; y -= y % mosaicV; mosaicH = GBAMosaicControlGetBgH(renderer->mosaic) + 1; }
	int sy = y - (int32_t) state->scene[6];
	if (sy < 0 || sy >= H4_ROWS * 16) { return true; }
	bool objwin = GBARegisterDISPCNTIsObjwinEnable(renderer->dispcnt);
	for (int x = renderer->start; x < renderer->end; ++x) {
		uint32_t current = renderer->row[x];
		if (!IS_WRITABLE(current)) { continue; }
		if (objwin && !background->objwinForceEnable && (!(current & FLAG_OBJWIN)) != background->objwinOnly) { continue; }
		int sx = x - x % mosaicH - (int32_t) state->scene[5];
		if (sx < 0 || sx >= H4_COLUMNS * 16) { continue; }
		unsigned cell = (sy >> 4) * H4_COLUMNS + (sx >> 4), quad = ((sy >> 3) & 1) * 2 + ((sx >> 3) & 1);
		const struct H4Tile* tile = &state->tiles[background->index - 1][cell][quad];
		if (!tile->pixels || !tile->palette) { continue; }
		unsigned tx = sx & 7, ty = sy & 7;
		if (tile->ref & 0x400) { tx ^= 7; } if (tile->ref & 0x800) { ty ^= 7; }
		unsigned pixel = tile->depth == 8 ? tile->pixels[ty * 8 + tx] : (tile->pixels[ty * 4 + tx / 2] >> ((tx & 1) * 4)) & 15;
		if (!pixel) { continue; }
		uint16_t raw = tile->palette[pixel];
		if ((raw & 0x7FFF) && (state->scene[10] & (1U << tile->paletteBank))) { raw = _h4Tint(raw, state->scene[9], state->scene[8]); }
		raw = _h4Tint(raw, state->scene[14], state->scene[15]);
		unsigned color = mColorFrom555(raw);
		bool variant = renderer->blendEffect == BLEND_BRIGHTEN || renderer->blendEffect == BLEND_DARKEN;
		if (objwin && (current & FLAG_OBJWIN)) { variant = variant && background->target1 && GBAWindowControlIsBlendEnable(renderer->objwin.packed); }
		else { variant = variant && background->variant; }
		if ((current & (FLAG_IS_BACKGROUND | FLAG_REBLEND)) == FLAG_REBLEND && !(objwin && (current & FLAG_OBJWIN))) { variant = false; }
		if (variant) { color = renderer->blendEffect == BLEND_BRIGHTEN ? _brighten(color, renderer->bldy) : _darken(color, renderer->bldy); }
		if (renderer->d.highlightAmount && background->highlight) { unsigned amount = renderer->d.highlightAmount >> 4; color = mColorMix5Bit(16 - amount, color, amount, renderer->d.highlightColor); }
		uint32_t flags = objwin && (current & FLAG_OBJWIN) ? background->objwinFlags : background->flags;
		if (objwin) {
			if (flags & FLAG_TARGET_2) { _compositeBlendObjwin(renderer, &renderer->row[x], color | flags, current); }
			else { _compositeNoBlendObjwin(renderer, &renderer->row[x], color | flags, current); }
		} else if (flags & FLAG_TARGET_2) { _compositeBlendNoObjwin(renderer, &renderer->row[x], color | flags, current); }
		else { _compositeNoBlendNoObjwin(renderer, &renderer->row[x], color | flags, current); }
	}
	return true;
}

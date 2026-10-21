/* Copyright (c) 2013-2015 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "gba/renderers/software-private.h"

#include <mgba/internal/gba/gba.h>

#define BACKGROUND_TEXT_SELECT_CHARACTER \
	xBase = localX & 0xF8; \
	if (background->size & 1) { \
		xBase += (localX & 0x100) << 5; \
	} \
	screenBase = yBase + (xBase >> 3); \
	LOAD_16(mapData, screenBase << 1, vram); \

#define DRAW_BACKGROUND_MODE_0_TILE_SUFFIX_16(BLEND, OBJWIN) \
	paletteData = GBA_TEXT_MAP_PALETTE(mapData) << 4; \
	charBase = (background->charBase + (GBA_TEXT_MAP_TILE(mapData) << 5)) + (localY << 2); \
	LOAD_32(tileData, charBase, vram); \
	if (!GBA_TEXT_MAP_HFLIP(mapData)) { \
		tileData >>= 4 * mod8; \
		for (; outX < end; ++outX, ++pixel) { \
			BACKGROUND_DRAW_PIXEL_16(BLEND, OBJWIN, 0); \
		} \
	} else { \
		for (outX = end - 1; outX >= renderer->start; --outX) { \
			uint32_t* pixel = &renderer->row[outX]; \
			BACKGROUND_DRAW_PIXEL_16(BLEND, OBJWIN, 0); \
		} \
	}

#define DRAW_BACKGROUND_MODE_0_TILE_PREFIX_16(BLEND, OBJWIN) \
	charBase = (background->charBase + (GBA_TEXT_MAP_TILE(mapData) << 5)) + (localY << 2); \
	LOAD_32(tileData, charBase, vram); \
	paletteData = GBA_TEXT_MAP_PALETTE(mapData) << 4; \
	pixel = &renderer->row[outX]; \
	if (!GBA_TEXT_MAP_HFLIP(mapData)) { \
		if (outX < renderer->start) { \
			tileData >>= 4 * (renderer->start - outX); \
			outX = renderer->start; \
			pixel = &renderer->row[outX]; \
		} \
		for (; outX < renderer->end; ++outX, ++pixel) { \
			BACKGROUND_DRAW_PIXEL_16(BLEND, OBJWIN, 0); \
		} \
	} else { \
		tileData >>= 4 * (0x8 - mod8); \
		int end = renderer->end - 8; \
		if (end < -1) { \
			end = -1; \
		} \
		outX = renderer->end - 1; \
		pixel = &renderer->row[outX]; \
		for (; outX > end; --outX, --pixel) { \
			BACKGROUND_DRAW_PIXEL_16(BLEND, OBJWIN, 0); \
		} \
		/* Needed for consistency checks */ \
		if (VIDEO_CHECKS) { \
			outX = renderer->end; \
			pixel = &renderer->row[outX]; \
		} \
	}

#define DRAW_BACKGROUND_MODE_0_MOSAIC_16(BLEND, OBJWIN) \
	x = inX & 7; \
	if (mosaicWait) { \
		int baseX = x - (mosaicH - mosaicWait); \
		if (baseX < 0) { \
			int disturbX = (16 + baseX) >> 3; \
			inX -= disturbX << 3; \
			localX = tileX * 8 + inX; \
			BACKGROUND_TEXT_SELECT_CHARACTER; \
			localY = inY & 0x7; \
			if (GBA_TEXT_MAP_VFLIP(mapData)) { \
				localY = 7 - localY; \
			} \
			baseX -= disturbX << 3; \
			inX += disturbX << 3; \
		} else { \
			localX = tileX * 8 + inX; \
			BACKGROUND_TEXT_SELECT_CHARACTER; \
			localY = inY & 0x7; \
			if (GBA_TEXT_MAP_VFLIP(mapData)) { \
				localY = 7 - localY; \
			} \
		} \
		charBase = (background->charBase + (GBA_TEXT_MAP_TILE(mapData) << 5)) + (localY << 2); \
		if (UNLIKELY(charBase >= 0x10000)) { \
			carryData = 0; \
		} else { \
			paletteData = GBA_TEXT_MAP_PALETTE(mapData) << 4; \
			LOAD_32(tileData, charBase, vram); \
			if (!GBA_TEXT_MAP_HFLIP(mapData)) { \
				tileData >>= 4 * baseX; \
			} else { \
				tileData >>= 4 * (7 - baseX); \
			} \
			tileData &= 0xF; \
			tileData |= tileData << 4; \
			tileData |= tileData << 8; \
			tileData |= tileData << 16; \
			carryData = tileData; \
		} \
	} \
	localX = tileX * 8 + inX; \
	for (; length; ++tileX) { \
		mapData = background->mapCache[(localX >> 3) & 0x3F]; \
		localX += 8; \
		localY = inY & 0x7; \
		if (GBA_TEXT_MAP_VFLIP(mapData)) { \
			localY = 7 - localY; \
		} \
		charBase = (background->charBase + (GBA_TEXT_MAP_TILE(mapData) << 5)) + (localY << 2); \
		tileData = carryData; \
		for (; x < 8 && length; ++x, --length) { \
			if (!mosaicWait) { \
				if (UNLIKELY(charBase >= 0x10000)) { \
					carryData = 0; \
				} else { \
					paletteData = GBA_TEXT_MAP_PALETTE(mapData) << 4; \
					LOAD_32(tileData, charBase, vram); \
					if (!GBA_TEXT_MAP_HFLIP(mapData)) { \
						tileData >>= x * 4; \
					} else { \
						tileData >>= (7 - x) * 4; \
					} \
					tileData &= 0xF; \
					tileData |= tileData << 4; \
					tileData |= tileData << 8; \
					tileData |= tileData << 16; \
					carryData = tileData; \
				} \
				mosaicWait = mosaicH; \
			} \
			--mosaicWait; \
			BACKGROUND_DRAW_PIXEL_16(BLEND, OBJWIN, 0); \
			++pixel; \
		} \
		x = 0; \
	}

#define DRAW_BACKGROUND_MODE_0_TILES_16(BLEND, OBJWIN) \
	for (; tileX < tileEnd; ++tileX) { \
		mapData = background->mapCache[(localX >> 3) & 0x3F]; \
		localX += 8; \
		localY = inY & 0x7; \
		if (GBA_TEXT_MAP_VFLIP(mapData)) { \
			localY = 7 - localY; \
		} \
		paletteData = GBA_TEXT_MAP_PALETTE(mapData) << 4; \
		charBase = (background->charBase + (GBA_TEXT_MAP_TILE(mapData) << 5)) + (localY << 2); \
		if (UNLIKELY(charBase >= 0x10000)) { \
			pixel += 8; \
			continue; \
		} \
		if (!GBA_TEXT_MAP_HFLIP(mapData)) { \
			LOAD_32(tileData, charBase, vram); \
		} else { \
			LOAD_32BE(tileData, charBase, vram); \
			tileData = ((tileData & 0xF0F0F0F0) >> 4) | ((tileData & 0x0F0F0F0F) << 4); \
		} \
		if (tileData) { \
			BACKGROUND_DRAW_PIXEL_16(BLEND, OBJWIN, 0); \
			BACKGROUND_DRAW_PIXEL_16(BLEND, OBJWIN, 1); \
			BACKGROUND_DRAW_PIXEL_16(BLEND, OBJWIN, 2); \
			BACKGROUND_DRAW_PIXEL_16(BLEND, OBJWIN, 3); \
			BACKGROUND_DRAW_PIXEL_16(BLEND, OBJWIN, 4); \
			BACKGROUND_DRAW_PIXEL_16(BLEND, OBJWIN, 5); \
			BACKGROUND_DRAW_PIXEL_16(BLEND, OBJWIN, 6); \
			BACKGROUND_DRAW_PIXEL_16(BLEND, OBJWIN, 7); \
		} \
		pixel += 8; \
	}

#define DRAW_BACKGROUND_MODE_0_TILE_SUFFIX_256(BLEND, OBJWIN) \
	charBase = (background->charBase + (GBA_TEXT_MAP_TILE(mapData) << 6)) + (localY << 3); \
	int end2 = end - 4; \
	if (!GBA_TEXT_MAP_HFLIP(mapData)) { \
		int shift = inX & 0x3; \
		if (LIKELY(charBase < 0x10000)) { \
			if (end2 > outX) { \
				LOAD_32(tileData, charBase, vram); \
				tileData >>= 8 * shift; \
				shift = 0; \
				for (; outX < end2; ++outX, ++pixel) { \
					BACKGROUND_DRAW_PIXEL_256(BLEND, OBJWIN, 0); \
				} \
			} \
		} \
		\
		if (LIKELY(charBase < 0x10000)) { \
			LOAD_32(tileData, charBase + 4, vram); \
			tileData >>= 8 * shift; \
			for (; outX < end; ++outX, ++pixel) { \
				BACKGROUND_DRAW_PIXEL_256(BLEND, OBJWIN, 0); \
			} \
		} \
	} else { \
		int start = outX; \
		outX = end - 1; \
		pixel = &renderer->row[outX]; \
		if (LIKELY(charBase < 0x10000)) { \
			if (end2 > start) { \
				LOAD_32(tileData, charBase, vram); \
				for (; outX >= end2; --outX, --pixel) { \
					BACKGROUND_DRAW_PIXEL_256(BLEND, OBJWIN, 0); \
				} \
				charBase += 4; \
			} \
		} \
		\
		if (LIKELY(charBase < 0x10000)) { \
			LOAD_32(tileData, charBase, vram); \
			for (; outX >= renderer->start; --outX, --pixel) { \
				BACKGROUND_DRAW_PIXEL_256(BLEND, OBJWIN, 0); \
			} \
		} \
		outX = end; \
		pixel = &renderer->row[outX]; \
	}

#define DRAW_BACKGROUND_MODE_0_TILE_PREFIX_256(BLEND, OBJWIN) \
	charBase = (background->charBase + (GBA_TEXT_MAP_TILE(mapData) << 6)) + (localY << 3); \
	if (UNLIKELY(charBase >= 0x10000)) { \
		return; \
	} \
	int end = mod8 - 4; \
	pixel = &renderer->row[outX]; \
	if (!GBA_TEXT_MAP_HFLIP(mapData)) { \
		if (end > 0) { \
			LOAD_32(tileData, charBase, vram); \
			for (; outX < renderer->end - end; ++outX, ++pixel) { \
				BACKGROUND_DRAW_PIXEL_256(BLEND, OBJWIN, 0); \
			} \
			charBase += 4; \
		} \
		\
		LOAD_32(tileData, charBase, vram); \
		for (; outX < renderer->end; ++outX, ++pixel) { \
			BACKGROUND_DRAW_PIXEL_256(BLEND, OBJWIN, 0); \
		} \
	} else { \
		int shift = (8 - mod8) & 0x3; \
		int start = outX; \
		outX = renderer->end - 1; \
		pixel = &renderer->row[outX]; \
		if (end > 0) { \
			LOAD_32(tileData, charBase, vram); \
			tileData >>= 8 * shift; \
			for (; outX >= start + 4; --outX, --pixel) { \
				BACKGROUND_DRAW_PIXEL_256(BLEND, OBJWIN, 0); \
			} \
			shift = 0; \
		} \
		\
		LOAD_32(tileData, charBase + 4, vram); \
		tileData >>= 8 * shift; \
		for (; outX >= start; --outX, --pixel) { \
			BACKGROUND_DRAW_PIXEL_256(BLEND, OBJWIN, 0); \
		} \
		/* Needed for consistency checks */ \
		if (VIDEO_CHECKS) { \
			outX = renderer->end; \
			pixel = &renderer->row[outX]; \
		} \
	}

#define DRAW_BACKGROUND_MODE_0_TILES_256(BLEND, OBJWIN) \
	for (; tileX < tileEnd; ++tileX) { \
		mapData = background->mapCache[(localX >> 3) & 0x3F]; \
		localX += 8; \
		localY = inY & 0x7; \
		if (GBA_TEXT_MAP_VFLIP(mapData)) { \
			localY = 7 - localY; \
		} \
		charBase = (background->charBase + (GBA_TEXT_MAP_TILE(mapData) << 6)) + (localY << 3); \
		if (UNLIKELY(charBase >= 0x10000)) { \
			pixel += 8; \
			continue; \
		} \
		if (!GBA_TEXT_MAP_HFLIP(mapData)) { \
			LOAD_32(tileData, charBase, vram); \
			if (tileData) { \
					BACKGROUND_DRAW_PIXEL_256(BLEND, OBJWIN, 0); \
					BACKGROUND_DRAW_PIXEL_256(BLEND, OBJWIN, 1); \
					BACKGROUND_DRAW_PIXEL_256(BLEND, OBJWIN, 2); \
					BACKGROUND_DRAW_PIXEL_256(BLEND, OBJWIN, 3); \
			} \
			pixel += 4; \
			LOAD_32(tileData, charBase + 4, vram); \
			if (tileData) { \
					BACKGROUND_DRAW_PIXEL_256(BLEND, OBJWIN, 0); \
					BACKGROUND_DRAW_PIXEL_256(BLEND, OBJWIN, 1); \
					BACKGROUND_DRAW_PIXEL_256(BLEND, OBJWIN, 2); \
					BACKGROUND_DRAW_PIXEL_256(BLEND, OBJWIN, 3); \
			} \
			pixel += 4; \
		} else { \
			LOAD_32(tileData, charBase + 4, vram); \
			if (tileData) { \
				BACKGROUND_DRAW_PIXEL_256(BLEND, OBJWIN, 3); \
				BACKGROUND_DRAW_PIXEL_256(BLEND, OBJWIN, 2); \
				BACKGROUND_DRAW_PIXEL_256(BLEND, OBJWIN, 1); \
				BACKGROUND_DRAW_PIXEL_256(BLEND, OBJWIN, 0); \
			} \
			pixel += 4; \
			LOAD_32(tileData, charBase, vram); \
			if (tileData) { \
				BACKGROUND_DRAW_PIXEL_256(BLEND, OBJWIN, 3); \
				BACKGROUND_DRAW_PIXEL_256(BLEND, OBJWIN, 2); \
				BACKGROUND_DRAW_PIXEL_256(BLEND, OBJWIN, 1); \
				BACKGROUND_DRAW_PIXEL_256(BLEND, OBJWIN, 0); \
			} \
			pixel += 4; \
		} \
	}

#define DRAW_BACKGROUND_MODE_0_MOSAIC_256(BLEND, OBJWIN) \
	x = inX & 7; \
	if (mosaicWait) { \
		int baseX = x - (mosaicH - mosaicWait); \
		if (baseX < 0) { \
			int disturbX = (16 + baseX) >> 3; \
			inX -= disturbX << 3; \
			localX = tileX * 8 + inX; \
			BACKGROUND_TEXT_SELECT_CHARACTER; \
			localY = inY & 0x7; \
			if (GBA_TEXT_MAP_VFLIP(mapData)) { \
				localY = 7 - localY; \
			} \
			baseX -= disturbX << 3; \
			inX += disturbX << 3; \
		} else { \
			localX = tileX * 8 + inX; \
			BACKGROUND_TEXT_SELECT_CHARACTER; \
			localY = inY & 0x7; \
			if (GBA_TEXT_MAP_VFLIP(mapData)) { \
				localY = 7 - localY; \
			} \
		} \
		charBase = (background->charBase + (GBA_TEXT_MAP_TILE(mapData) << 6)) + (localY << 3); \
		if (UNLIKELY(charBase >= 0x10000)) { \
			carryData = 0; \
		} else { \
			LOAD_32(tileData, charBase, vram); \
			if (!GBA_TEXT_MAP_HFLIP(mapData)) { \
				if (x >= 4) { \
					LOAD_32(tileData, charBase + 4, vram); \
					tileData >>= (x - 4) * 8; \
				} else { \
					LOAD_32(tileData, charBase, vram); \
					tileData >>= x * 8; \
				} \
			} else { \
				if (x >= 4) { \
					LOAD_32(tileData, charBase, vram); \
					tileData >>= (7 - x) * 8; \
				} else { \
					LOAD_32(tileData, charBase + 4, vram); \
					tileData >>= (3 - x) * 8; \
				} \
			} \
			tileData &= 0xFF; \
			carryData = tileData; \
		} \
	} \
	localX = tileX * 8 + inX; \
	for (; length; ++tileX) { \
		mapData = background->mapCache[(localX >> 3) & 0x3F]; \
		localX += 8; \
		localY = inY & 0x7; \
		if (GBA_TEXT_MAP_VFLIP(mapData)) { \
			localY = 7 - localY; \
		} \
		charBase = (background->charBase + (GBA_TEXT_MAP_TILE(mapData) << 6)) + (localY << 3); \
		tileData = carryData; \
		for (; x < 8 && length; ++x, --length) { \
			if (!mosaicWait) { \
				if (UNLIKELY(charBase >= 0x10000)) { \
					carryData = 0; \
				} else { \
					if (!GBA_TEXT_MAP_HFLIP(mapData)) { \
						if (x >= 4) { \
							LOAD_32(tileData, charBase + 4, vram); \
							tileData >>= (x - 4) * 8; \
						} else { \
							LOAD_32(tileData, charBase, vram); \
							tileData >>= x * 8; \
						} \
					} else { \
						if (x >= 4) { \
							LOAD_32(tileData, charBase, vram); \
							tileData >>= (7 - x) * 8; \
						} else { \
							LOAD_32(tileData, charBase + 4, vram); \
							tileData >>= (3 - x) * 8; \
						} \
					} \
					tileData &= 0xFF; \
					carryData = tileData; \
				} \
				mosaicWait = mosaicH; \
			} \
			tileData |= tileData << 8; \
			--mosaicWait; \
			BACKGROUND_DRAW_PIXEL_256(BLEND, OBJWIN, 0); \
			++pixel; \
		} \
		x = 0; \
	}

#define DRAW_BACKGROUND_MODE_0(BPP, BLEND, OBJWIN) \
	uint32_t* pixel = &renderer->row[outX]; \
	if (background->mosaic && GBAMosaicControlGetBgH(renderer->mosaic)) { \
		int mosaicH = GBAMosaicControlGetBgH(renderer->mosaic) + 1; \
		int x; \
		int mosaicWait = (mosaicH - outX + GBA_VIDEO_HORIZONTAL_PIXELS * mosaicH) % mosaicH; \
		int carryData = 0; \
		paletteData = 0; /* Quiets compiler warning */ \
		DRAW_BACKGROUND_MODE_0_MOSAIC_ ## BPP (BLEND, OBJWIN) \
		return; \
	} \
	\
	if (inX & 0x7) { \
		localX = tileX * 8 + inX; \
		BACKGROUND_TEXT_SELECT_CHARACTER; \
		localY = inY & 0x7; \
		if (GBA_TEXT_MAP_VFLIP(mapData)) { \
			localY = 7 - localY; \
		} \
		int mod8 = inX & 0x7; \
		int end = outX + 0x8 - mod8; \
		if (end > renderer->end) { \
			end = renderer->end; \
		} \
		if (UNLIKELY(end == outX)) { \
			return; \
		} \
		if (UNLIKELY(end < outX)) { \
			mLOG(GBA_VIDEO, FATAL, "Out of bounds background draw!"); \
			return; \
		} \
		DRAW_BACKGROUND_MODE_0_TILE_SUFFIX_ ## BPP (BLEND, OBJWIN) \
		outX = end; \
		if (tileX < tileEnd) { \
			++tileX; \
		} else if (VIDEO_CHECKS && UNLIKELY(tileX > tileEnd)) { \
			mLOG(GBA_VIDEO, FATAL, "Invariant doesn't hold in background draw! tileX (%u) > tileEnd (%u)", tileX, tileEnd); \
			return; \
		} \
		length -= end - renderer->start; \
	} \
	/*! TODO: Make sure these lines can be removed */ \
	/*!*/ pixel = &renderer->row[outX]; \
	outX += (tileEnd - tileX) * 8; \
	/*!*/ if (VIDEO_CHECKS &&  UNLIKELY(outX > GBA_VIDEO_HORIZONTAL_PIXELS)) { \
	/*!*/	mLOG(GBA_VIDEO, FATAL, "Out of bounds background draw would occur!"); \
	/*!*/	return; \
	/*!*/ } \
	localX = (tileX * 8 + inX) & 0x1FF; \
	DRAW_BACKGROUND_MODE_0_TILES_ ## BPP (BLEND, OBJWIN) \
	if (length & 0x7) { \
		localX = tileX * 8 + inX; \
		BACKGROUND_TEXT_SELECT_CHARACTER; \
		localY = inY & 0x7; \
		if (GBA_TEXT_MAP_VFLIP(mapData)) { \
			localY = 7 - localY; \
		} \
		int mod8 = length & 0x7; \
		if (VIDEO_CHECKS && UNLIKELY(outX + mod8 != renderer->end)) { \
			mLOG(GBA_VIDEO, FATAL, "Invariant doesn't hold in background draw!"); \
			return; \
		} \
		DRAW_BACKGROUND_MODE_0_TILE_PREFIX_ ## BPP (BLEND, OBJWIN) \
	} \
	if (VIDEO_CHECKS && UNLIKELY(&renderer->row[outX] != pixel)) { \
		mLOG(GBA_VIDEO, FATAL, "Background draw ended in the wrong place! Diff: %" PRIXPTR, &renderer->row[outX] - pixel); \
	} \
	if (VIDEO_CHECKS && UNLIKELY(outX > GBA_VIDEO_HORIZONTAL_PIXELS)) { \
		mLOG(GBA_VIDEO, FATAL, "Out of bounds background draw occurred!"); \
		return; \
	}

/* HGX2/HGX3 is explicitly enabled by the ROM header (reserved bytes B5..BB).
 * Header B8 may name the HGX pointer register at 040003E0.
 * Descriptor: magic, BG mask, four screen-map pointers, generation, fade word.
 * HGX3 extends the descriptor to 48 bytes: current binding pointer at +32,
 * descriptor size (48) at +36, entry stride (12) at +40. Binding is an atomic
 * pointer to {primaryTiles, secondaryTiles, primaryPalettes, secondaryPalettes}.
 * Matching source resources are late-bound independently to live VRAM/palette
 * RAM; foreign source resources remain ROM-resident across atlas switches.
 * Optional +44 points at {DNS coefficient, color555, paletteMask, bindingPtr}.
 * DNS and its binding are latched together. Only foreign nonblack palettes are
 * tinted, before the ordinary fade; live native palette colors are final.
 * Fade word: BGR555 color in bits 0..14, coefficient 0..16 in bits 16..20.
 * Maps contain 1024 {tileAddress | hFlip | vFlip<<1, paletteAddress, resourceFlags} entries.
 * resourceFlags: bit31 = BIOS LZ77 source, low10 = decoded tile index;
 * bit30 enables expected native screen word comparison (bits10..25).
 * Resources may be in EWRAM, IWRAM or the 64 MiB ROM aperture. No bus reads,
 * callbacks or guest writes occur while rendering. Invalid resources fall back
 * to the native screen entry. Color zero retains normal GBA transparency. */
static const uint8_t* _hgxResource(struct GBA* gba, uint32_t address, size_t size) {
	uint32_t offset;
	if (address >= 0x02000000 && address < 0x02040000) {
		offset = address - 0x02000000;
		if (size <= 0x40000 - offset) {
			return (const uint8_t*) gba->memory.wram + offset;
		}
	} else if (address >= 0x03000000 && address < 0x03008000) {
		offset = address - 0x03000000;
		if (size <= 0x8000 - offset) {
			return (const uint8_t*) gba->memory.iwram + offset;
		}
	} else if (address >= 0x08000000 && address < 0x0C000000) {
		offset = address - 0x08000000;
		if (offset <= gba->memory.romSize && size <= gba->memory.romSize - offset) {
			return (const uint8_t*) gba->memory.rom + offset;
		}
	}
	return NULL;
}

/* BIOS-compatible LZ77 (type 0x10), decoded lazily and bounded to 64 KiB.
 * Entry metadata retains the original ROM pointer, so loading a state in a
 * fresh emulator rebuilds exactly the same resource without host cache IDs. */
static const uint8_t* _hgxLZTile(struct GBAVideoSoftwareRenderer* renderer, uint32_t source, unsigned tileIndex) {
	if (source < 0x08000000 || source >= 0x0C000000) { return NULL; }
	unsigned slot;
	for (slot = 0; slot < 64; ++slot) {
		if (renderer->hgxSources[slot] == source) { break; }
	}
	if (slot == 64) {
		slot = renderer->hgxNext++ & 63;
		free(renderer->hgxDecoded[slot]);
		renderer->hgxDecoded[slot] = NULL;
		renderer->hgxSizes[slot] = 0;
		renderer->hgxSources[slot] = source;
		const uint8_t* header = _hgxResource(renderer->d.p, source, 4);
		uint32_t value = 0;
		if (header) { LOAD_32(value, 0, header); }
		size_t size = value >> 8;
		if ((value & 255) != 0x10 || !size || size > 0x10000) { return NULL; }
		uint8_t* data = malloc(size);
		if (!data) { return NULL; }
		unsigned cursor = 4;
		size_t out = 0;
		bool valid = true;
		while (out < size && valid) {
			const uint8_t* input = _hgxResource(renderer->d.p, source + cursor++, 1);
			if (!input) { valid = false; break; }
			unsigned flags = *input;
			for (unsigned bit = 0; bit < 8 && out < size; ++bit) {
				if (flags & (0x80 >> bit)) {
					input = _hgxResource(renderer->d.p, source + cursor, 2);
					cursor += 2;
					if (!input) { valid = false; break; }
					unsigned count = (input[0] >> 4) + 3;
					unsigned distance = ((input[0] & 15) << 8 | input[1]) + 1;
					if (distance > out) { valid = false; break; }
					while (count-- && out < size) {
						data[out] = data[out - distance];
						++out;
					}
				} else {
					input = _hgxResource(renderer->d.p, source + cursor++, 1);
					if (!input) { valid = false; break; }
					data[out++] = *input;
				}
			}
		}
		if (!valid) { free(data); return NULL; }
		renderer->hgxDecoded[slot] = data;
		renderer->hgxSizes[slot] = size;
	}
	size_t offset = tileIndex * 32;
	if (offset > renderer->hgxSizes[slot] || renderer->hgxSizes[slot] - offset < 32) { return NULL; }
	return renderer->hgxDecoded[slot] + offset;
}

/* Optional HGX3 transaction capability. Legacy generation counters must never
 * be interpreted as epochs: only environment.paletteMask bit31 opts in.
 * Guest writes cannot interleave a software scanline on the synchronous core. */
bool GBAVideoSoftwareRendererHGXEpoch(struct GBAVideoSoftwareRenderer* renderer, uint32_t* control, uint32_t* epoch) {
	struct GBA* gba = renderer->d.p;
	if (!gba || !gba->memory.rom || gba->memory.romSize < 0xBC) { return false; }
	const uint8_t* rom = (const uint8_t*) gba->memory.rom;
	if (rom[0xB5] != 'H' || rom[0xB6] != 'G' || rom[0xB7] != 'X') { return false; }
	uint32_t address;
	LOAD_32(address, 0xB8, rom);
	if (address == 0x040003E0) { address = gba->memory.io[0x1F0] | ((uint32_t) gba->memory.io[0x1F1] << 16); }
	if ((address & 3) || address < 0x02000000 || address > 0x0203FFD0) { return false; }
	const uint8_t* descriptor = _hgxResource(gba, address, 48);
	uint32_t magic, size, stride, environmentAddress, mask;
	LOAD_32(magic, 0, descriptor);
	LOAD_32(size, 36, descriptor);
	LOAD_32(stride, 40, descriptor);
	LOAD_32(environmentAddress, 44, descriptor);
	const uint8_t* environment = _hgxResource(gba, environmentAddress, 16);
	if (magic != 0x33584748 || size != 48 || stride != 12 || !environment || (environmentAddress & 3)) { return false; }
	LOAD_32(mask, 8, environment);
	if (!(mask & 0x80000000)) { return false; }
	*control = address;
	LOAD_32(*epoch, 24, descriptor);
	return true;
}

/* Latch at scanline zero, after the native VBlank uploads. Reading an atomic
 * binding pointer is not sufficient: mainloop writes can also stage individual
 * ring entries before the native BG copy. Keep both stable for the frame.
 * This host snapshot is rebuildable and is invalidated by renderer reset/state
 * restore. A first draw at a nonzero line lazily captures one coherent frame. */
void GBAVideoSoftwareRendererLatchHGXFrame(struct GBAVideoSoftwareRenderer* renderer) {
	renderer->hgxFrameStarted = true;
	renderer->hgxFrameValid = false;
	renderer->hgxFrameHasBinding = false;
	memset(renderer->hgxFrameDns, 0, sizeof(renderer->hgxFrameDns));
	memset(renderer->hgxFrameScreenValid, 0, sizeof(renderer->hgxFrameScreenValid));
	struct GBA* gba = renderer->d.p;
	if (!gba || !gba->memory.rom || gba->memory.romSize < 0xBC) { return; }
	const uint8_t* rom = (const uint8_t*) gba->memory.rom;
	if (rom[0xB5] != 'H' || rom[0xB6] != 'G' || rom[0xB7] != 'X') { return; }
	uint32_t address;
	LOAD_32(address, 0xB8, rom);
	if (address == 0x040003E0) {
		address = gba->memory.io[0x1F0] | ((uint32_t) gba->memory.io[0x1F1] << 16);
	}
	if ((address & 3) || address < 0x02000000 || address > 0x0203FFD0) { return; }
	const uint8_t* descriptor = _hgxResource(gba, address, 48);
	uint32_t magic, mask, descriptorSize, entrySize, bindingAddress;
	LOAD_32(magic, 0, descriptor);
	LOAD_32(mask, 4, descriptor);
	LOAD_32(descriptorSize, 36, descriptor);
	LOAD_32(entrySize, 40, descriptor);
	if (magic == 0x33584748) {
		for (unsigned reg = 0x3EC; reg <= 0x3FE; reg += 2) { gba->memory.io[reg >> 1] = 0; }
	}
	if (magic != 0x33584748 || !mask || descriptorSize != 48 || entrySize != 12) { return; }
	renderer->hgxFrameDescriptor = address;
	LOAD_32(bindingAddress, 32, descriptor);
	uint32_t environmentAddress;
	LOAD_32(environmentAddress, 44, descriptor);
	const uint8_t* environment = _hgxResource(gba, environmentAddress, 16);
	if (environment && !(environmentAddress & 3)) {
		uint32_t coefficient;
		LOAD_32(coefficient, 0, environment);
		if (coefficient <= 16) {
			for (unsigned n = 0; n < 3; ++n) { LOAD_32(renderer->hgxFrameDns[n], n * 4, environment); }
			LOAD_32(bindingAddress, 12, environment);
		}
	}
	gba->memory.io[0x3EC >> 1] = renderer->hgxFrameDns[0];
	gba->memory.io[0x3EE >> 1] = renderer->hgxFrameDns[1];
	gba->memory.io[0x3F0 >> 1] = renderer->hgxFrameDns[2];
	const uint8_t* binding = _hgxResource(gba, bindingAddress, 16);
	if (binding && !(bindingAddress & 3)) {
		for (unsigned n = 0; n < 4; ++n) { LOAD_32(renderer->hgxFrameBinding[n], n * 4, binding); }
		renderer->hgxFrameHasBinding = true;
	}
	for (unsigned n = 0; n < 4; ++n) {
		LOAD_32(address, 8 + n * 4, descriptor);
		renderer->hgxFrameScreen[n] = address;
		const uint8_t* screen = _hgxResource(gba, address, 12288);
		if (screen && !(address & 3) && (mask & (1U << n))) {
			memcpy(renderer->hgxFrameEntries[n], screen, 12288);
			renderer->hgxFrameScreenValid[n] = true;
		}
	}
	renderer->hgxFrameValid = true;
	unsigned flags = 1 | (renderer->hgxFrameHasBinding ? 2 : 0);
	for (unsigned n = 0; n < 4; ++n) { if (renderer->hgxFrameScreenValid[n]) { flags |= 16U << n; } }
	gba->memory.io[0x3F2 >> 1] = flags;
}

static bool _drawBackgroundHGX(struct GBAVideoSoftwareRenderer* renderer,
                              struct GBAVideoSoftwareBackground* background, int y) {
	struct GBA* gba = renderer->d.p;
	if (!gba || !gba->memory.rom || gba->memory.romSize < 0xBC || background->size || background->multipalette) {
		return false;
	}
	const uint8_t* rom = (const uint8_t*) gba->memory.rom;
	if (rom[0xB5] != 'H' || rom[0xB6] != 'G' || rom[0xB7] != 'X') {
		return false;
	}
	uint32_t address;
	LOAD_32(address, 0xB8, rom);
	if (address == 0x040003E0) {
		address = gba->memory.io[0x1F0] | ((uint32_t) gba->memory.io[0x1F1] << 16);
	}
	if ((address & 3) || address < 0x02000000 || address > 0x0203FFE0) {
		return false;
	}
	uint32_t descriptorAddress = address;
	const uint8_t* descriptor = _hgxResource(gba, address, 32);
	uint32_t magic, mask;
	LOAD_32(magic, 0, descriptor);
	LOAD_32(mask, 4, descriptor);
	if ((magic != 0x32584748 && magic != 0x33584748) || !(mask & (1U << background->index))) {
		if (magic == 0x33584748 && !mask) { renderer->hgxFrameValid = false; gba->memory.io[0x3F2 >> 1] = 0; }
		return false;
	}
	uint32_t bindingBases[4] = { 0 };
	bool hasBinding = false;
	if (magic == 0x33584748) {
		if (!renderer->hgxFrameStarted) { GBAVideoSoftwareRendererLatchHGXFrame(renderer); }
		if (!renderer->hgxFrameValid || renderer->hgxFrameDescriptor != descriptorAddress) {
			renderer->hgxFrameValid = false;
			gba->memory.io[0x3F2 >> 1] &= ~1U;
			return false;
		}
		hasBinding = renderer->hgxFrameHasBinding;
		memcpy(bindingBases, renderer->hgxFrameBinding, sizeof(bindingBases));
	}
	LOAD_32(address, 8 + 4 * background->index, descriptor);
	const uint8_t* entries = _hgxResource(gba, address, 12288);
	if (!entries || (address & 3)) {
		return false;
	}
	if (magic == 0x33584748) {
		if (!renderer->hgxFrameScreenValid[background->index] || renderer->hgxFrameScreen[background->index] != address) {
			renderer->hgxFrameValid = false;
			gba->memory.io[0x3F2 >> 1] &= ~1U;
			return false;
		}
		entries = renderer->hgxFrameEntries[background->index];
	}
	int mosaicH = 1;
	if (background->mosaic) {
		int mosaicV = GBAMosaicControlGetBgV(renderer->mosaic) + 1;
		y -= y % mosaicV;
		mosaicH = GBAMosaicControlGetBgH(renderer->mosaic) + 1;
	}
	uint32_t fade;
	LOAD_32(fade, 28, descriptor);
	unsigned fadeY = (fade >> 16) & 31;
	if (fadeY > 16) { fadeY = 16; }
	unsigned inY = (y + background->y - background->offsetY) & 255;
	bool objwin = GBARegisterDISPCNTIsObjwinEnable(renderer->dispcnt);
	for (int x = renderer->start; x < renderer->end; ++x) {
		uint32_t current = renderer->row[x];
		if (!IS_WRITABLE(current)) {
			continue;
		}
		if (objwin && !background->objwinForceEnable && (!(current & FLAG_OBJWIN)) != background->objwinOnly) {
			continue;
		}
		unsigned inX = (x - x % mosaicH + background->x - background->offsetX) & 255;
		unsigned index = (inY >> 3) * 32 + (inX >> 3);
		uint32_t tileAddress, paletteAddress, resourceFlags;
		LOAD_32(tileAddress, index * 12, entries);
		LOAD_32(paletteAddress, index * 12 + 4, entries);
		LOAD_32(resourceFlags, index * 12 + 8, entries);
		const uint8_t* tile = NULL;
		uint16_t nativeEntry;
		LOAD_16(nativeEntry, background->screenBase + index * 2, renderer->d.vram);
		bool nativeMatches = !(resourceFlags & 0x40000000) || nativeEntry == ((resourceFlags >> 10) & 65535);
		bool nativeTile = false;
		bool nativePaletteResource = false;
		if (hasBinding && nativeMatches && tileAddress && (resourceFlags & 0x40000000)) {
			unsigned sourceIndex = nativeEntry & 1023;
			unsigned tileRole = sourceIndex < 640 ? 0 : 1;
			unsigned localIndex = sourceIndex - (tileRole ? 640 : 0);
			uint32_t tileBase = tileAddress & ~3U;
			if (!(resourceFlags & 0x80000000)) {
				if (tileBase >= localIndex * 32) { tileBase -= localIndex * 32; }
				else { tileBase = 0; }
			}
			nativeTile = tileBase && tileBase == bindingBases[tileRole]
			    && (!(resourceFlags & 0x80000000) || (resourceFlags & 1023) == localIndex);
			unsigned bank = nativeEntry >> 12;
			uint32_t paletteBase = paletteAddress >= bank * 32 ? paletteAddress - bank * 32 : 0;
			nativePaletteResource = paletteBase && paletteBase == bindingBases[bank < 7 ? 2 : 3];
		}
		if (tileAddress && nativeMatches) {
			if (nativeTile) {
				unsigned offset = background->charBase + (nativeEntry & 1023) * 32;
				if (offset <= 0x10000 - 32) { tile = (const uint8_t*) renderer->d.vram + offset; }
			} else {
				tile = resourceFlags & 0x80000000
				    ? _hgxLZTile(renderer, tileAddress & ~3U, resourceFlags & 1023)
				    : _hgxResource(gba, tileAddress & ~3U, 32);
			}
		}
		const uint8_t* palette = _hgxResource(gba, paletteAddress, 32);
		unsigned tx = inX & 7;
		unsigned ty = inY & 7;
		unsigned color;
		bool useVariant = renderer->blendEffect == BLEND_BRIGHTEN || renderer->blendEffect == BLEND_DARKEN;
		if (objwin && (current & FLAG_OBJWIN)) {
			useVariant = useVariant && background->target1 && GBAWindowControlIsBlendEnable(renderer->objwin.packed);
		} else {
			useVariant = useVariant && background->variant;
		}
		if ((current & (FLAG_IS_BACKGROUND | FLAG_REBLEND)) == FLAG_REBLEND && !(objwin && (current & FLAG_OBJWIN))) {
			useVariant = false;
		}
		if (tile && (nativePaletteResource || palette) && !(paletteAddress & 1)) {
			if (tileAddress & 1) { tx ^= 7; }
			if (tileAddress & 2) { ty ^= 7; }
			unsigned pixel = (tile[ty * 4 + (tx >> 1)] >> ((tx & 1) * 4)) & 15;
			if (!pixel) { continue; }
			if (nativePaletteResource) {
				if (gba->memory.io[0x3F6 >> 1] != 0xFFFF) { ++gba->memory.io[0x3F6 >> 1]; }
				unsigned paletteIndex = (nativeEntry >> 12) * 16 + pixel;
				mColor* livePalette = useVariant ? renderer->variantPalette : renderer->normalPalette;
				if (renderer->d.highlightAmount && background->highlight) {
					livePalette = useVariant ? renderer->highlightVariantPalette : renderer->highlightPalette;
				}
				color = livePalette[paletteIndex];
			} else {
			uint16_t raw;
			LOAD_16(raw, pixel * 2, palette);
			if (magic == 0x33584748 && gba->memory.io[0x3F8 >> 1] != 0xFFFF) {
				if (!gba->memory.io[0x3F8 >> 1]) { gba->memory.io[0x3F2 >> 1] |= ((nativeEntry >> 12) + 1) << 8; }
				++gba->memory.io[0x3F8 >> 1];
			}
			/* The game DNS leaves opaque black colors unchanged. Current
			 * palettes already contain DNS/fade and never enter this path. */
			unsigned dnsY = magic == 0x33584748 ? renderer->hgxFrameDns[0] : 0;
			if (dnsY && (raw & 0x7FFF) && (renderer->hgxFrameDns[2] & (1U << (nativeEntry >> 12)))) {
				bool sample = gba->memory.io[0x3F4 >> 1] == 0;
				if (sample) { gba->memory.io[0x3FC >> 1] = raw; }
				if (gba->memory.io[0x3F4 >> 1] != 0xFFFF) { ++gba->memory.io[0x3F4 >> 1]; }
				unsigned tinted = 0;
				for (unsigned shift = 0; shift < 15; shift += 5) {
					unsigned a = (raw >> shift) & 31;
					unsigned b = (renderer->hgxFrameDns[1] >> shift) & 31;
					tinted |= ((a * (16 - dnsY) + b * dnsY) >> 4) << shift;
				}
				raw = tinted;
				if (sample) { gba->memory.io[0x3FE >> 1] = raw; }
			}
			if (fadeY) {
				unsigned faded = 0;
				for (unsigned shift = 0; shift < 15; shift += 5) {
					unsigned a = (raw >> shift) & 31;
					unsigned b = (fade >> shift) & 31;
					faded |= ((a * (16 - fadeY) + b * fadeY) >> 4) << shift;
				}
				raw = faded;
			}
			color = mColorFrom555(raw);
			if (useVariant) {
				color = renderer->blendEffect == BLEND_BRIGHTEN ? _brighten(color, renderer->bldy) : _darken(color, renderer->bldy);
			}
			if (renderer->d.highlightAmount && background->highlight) {
				unsigned amount = renderer->d.highlightAmount >> 4;
				color = mColorMix5Bit(16 - amount, color, amount, renderer->d.highlightColor);
			}
			}
		} else {
			uint16_t native;
			LOAD_16(native, background->screenBase + index * 2, renderer->d.vram);
			if (native & 0x400) { tx ^= 7; }
			if (native & 0x800) { ty ^= 7; }
			unsigned offset = background->charBase + (native & 1023) * 32 + ty * 4 + (tx >> 1);
			if (offset >= 0x10000) { continue; }
			unsigned pixel = (((const uint8_t*) renderer->d.vram)[offset] >> ((tx & 1) * 4)) & 15;
			if (!pixel) { continue; }
			if (magic == 0x33584748 && gba->memory.io[0x3FA >> 1] != 0xFFFF) { ++gba->memory.io[0x3FA >> 1]; }
			unsigned paletteIndex = (native >> 12) * 16 + pixel;
			mColor* nativePalette = useVariant ? renderer->variantPalette : renderer->normalPalette;
			if (renderer->d.highlightAmount && background->highlight) {
				nativePalette = useVariant ? renderer->highlightVariantPalette : renderer->highlightPalette;
			}
			color = nativePalette[paletteIndex];
		}
		uint32_t flags = objwin && (current & FLAG_OBJWIN) ? background->objwinFlags : background->flags;
		if (objwin) {
			if (flags & FLAG_TARGET_2) {
				_compositeBlendObjwin(renderer, &renderer->row[x], color | flags, current);
			} else {
				_compositeNoBlendObjwin(renderer, &renderer->row[x], color | flags, current);
			}
		} else if (flags & FLAG_TARGET_2) {
			_compositeBlendNoObjwin(renderer, &renderer->row[x], color | flags, current);
		} else {
			_compositeNoBlendNoObjwin(renderer, &renderer->row[x], color | flags, current);
		}
	}
	return true;
}

void GBAVideoSoftwareRendererDrawBackgroundMode0(struct GBAVideoSoftwareRenderer* renderer, struct GBAVideoSoftwareBackground* background, int y) {
	if (GBAVideoSoftwareRendererDrawHGX4(renderer, background, y)) {
		return;
	}
	if (_drawBackgroundHGX(renderer, background, y)) {
		return;
	}
	int inX = (renderer->start + background->x - background->offsetX) & 0x1FF;
	int length = renderer->end - renderer->start;
	if (background->mosaic) {
		int mosaicV = GBAMosaicControlGetBgV(renderer->mosaic) + 1;
		y -= y % mosaicV;
	}
	int inY = y + background->y - background->offsetY;
	uint16_t mapData;

	unsigned yBase = inY & 0xF8;
	if (background->size == 2) {
		yBase += inY & 0x100;
	} else if (background->size == 3) {
		yBase += (inY & 0x100) << 1;
	}
	yBase = (background->screenBase >> 1) + (yBase << 2);

	int localX;
	int localY;

	unsigned xBase;

	uint32_t flags = background->flags;
	uint32_t objwinFlags = background->objwinFlags;
	bool variant = background->variant;

	uint32_t screenBase;
	uint32_t charBase;
	mColor* palette = renderer->normalPalette;
	if (renderer->d.highlightAmount && background->highlight) {
		palette = renderer->highlightPalette;
	}
	if (variant) {
		palette = renderer->variantPalette;
		if (renderer->d.highlightAmount && background->highlight) {
			palette = renderer->highlightVariantPalette;
		}
	}
	PREPARE_OBJWIN;

	int outX = renderer->start;

	uint32_t tileData;
	uint32_t current;
	int pixelData;
	int paletteData;
	int tileX;
	int tileEnd = ((length + inX) >> 3) - (inX >> 3);
	uint16_t* vram = renderer->d.vram;

	if (background->yCache != inY >> 3) {
		localX = 0;
		for (tileX = 0; tileX < 64; ++tileX, localX += 8) {
			BACKGROUND_TEXT_SELECT_CHARACTER;
			background->mapCache[tileX] = mapData;
		}
		background->yCache = inY >> 3;
	}

	tileX = 0;
	if (!objwinSlowPath) {
		if (!(flags & FLAG_TARGET_2)) {
			if (!background->multipalette) {
				DRAW_BACKGROUND_MODE_0(16, NoBlend, NO_OBJWIN);
			} else {
				DRAW_BACKGROUND_MODE_0(256, NoBlend, NO_OBJWIN);
			}
		} else {
			if (!background->multipalette) {
				DRAW_BACKGROUND_MODE_0(16, Blend, NO_OBJWIN);
			} else {
				DRAW_BACKGROUND_MODE_0(256, Blend, NO_OBJWIN);
			}
		}
	} else {
		if (!(flags & FLAG_TARGET_2)) {
			if (!background->multipalette) {
				DRAW_BACKGROUND_MODE_0(16, NoBlend, OBJWIN);
			} else {
				DRAW_BACKGROUND_MODE_0(256, NoBlend, OBJWIN);
			}
		} else {
			if (!background->multipalette) {
				DRAW_BACKGROUND_MODE_0(16, Blend, OBJWIN);
			} else {
				DRAW_BACKGROUND_MODE_0(256, Blend, OBJWIN);
			}
		}
	}
}

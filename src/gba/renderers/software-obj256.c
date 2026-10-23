/* Copyright (c) 2026 mGBA contributors
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. https://mozilla.org/MPL/2.0/ */
#include "gba/renderers/software-private.h"
#include <mgba/internal/gba/gba.h>

/* Opt-in per-OAM 8bpp materials. OAM remains authoritative for all geometry
 * and compositing. Signature checks prevent a recycled/sorted OAM slot from
 * borrowing another actor's material. The complete material is latched at
 * line zero; no mutable game memory or global OBJ palette is read afterwards. */
struct OBJ256Material {
	bool enabled;
	uint16_t a, b, c;
	uint8_t pixels[64 * 64];
	mColor palette[256];
};
struct GBAVideoOBJ256 {
	bool valid;
	struct OBJ256Material materials[128];
};

static const uint8_t* _obj256Read(struct GBA* gba, uint32_t address, size_t size, bool romOnly) {
	uint32_t offset;
	if (address >= 0x08000000 && address < 0x0C000000) {
		offset = address - 0x08000000;
		if (offset <= gba->memory.romSize && size <= gba->memory.romSize - offset) { return (const uint8_t*) gba->memory.rom + offset; }
	} else if (!romOnly && address >= 0x02000000 && address < 0x02040000) {
		offset = address - 0x02000000;
		if (size <= 0x40000 - offset) { return (const uint8_t*) gba->memory.wram + offset; }
	} else if (!romOnly && address >= 0x03000000 && address < 0x03008000) {
		offset = address - 0x03000000;
		if (size <= 0x8000 - offset) { return (const uint8_t*) gba->memory.iwram + offset; }
	}
	return NULL;
}
static uint32_t _obj256Word(const uint8_t* data, unsigned offset) { uint32_t value; LOAD_32(value, offset, data); return value; }
static uint16_t _obj256Half(const uint8_t* data, unsigned offset) { uint16_t value; LOAD_16(value, offset, data); return value; }
static uint16_t _obj256Tint(uint16_t raw, unsigned target, unsigned coefficient) {
	if (coefficient > 16) { coefficient = 16; }
	unsigned result = 0;
	for (unsigned shift = 0; shift < 15; shift += 5) {
		result |= ((((raw >> shift) & 31) * (16 - coefficient) + ((target >> shift) & 31) * coefficient) >> 4) << shift;
	}
	return result;
}

void GBAVideoSoftwareRendererResetOBJ256(struct GBAVideoSoftwareRenderer* renderer) {
	free(renderer->obj256);
	renderer->obj256 = NULL;
}

void GBAVideoSoftwareRendererLatchOBJ256(struct GBAVideoSoftwareRenderer* renderer) {
	if (renderer->obj256) { renderer->obj256->valid = false; }
	struct GBA* gba = renderer->d.p;
	if (!gba || !gba->memory.rom || gba->memory.romSize < 0xBC) { return; }
	const uint8_t* rom = (const uint8_t*) gba->memory.rom;
	if (memcmp(rom + 0xB5, "HGX", 3)) { return; }
	uint32_t address = _obj256Word(rom, 0xB8);
	if (address == 0x040003E0) { address = gba->memory.io[0x1F0] | ((uint32_t) gba->memory.io[0x1F1] << 16); }
	const uint8_t* control = _obj256Read(gba, address, 40, false);
	if (!control || _obj256Word(control, 0) != 0x34584748 || _obj256Word(control, 4) != 40
	    || !(_obj256Word(control, 20) & 1) || _obj256Word(control, 36) != 32) { return; }
	const uint8_t* scene = _obj256Read(gba, _obj256Word(control, 32), 32, false);
	if (!scene || _obj256Word(scene, 12) != 16) { return; }
	unsigned count = _obj256Word(scene, 4);
	if (count > 128) { return; }
	const uint8_t* entries = count ? _obj256Read(gba, _obj256Word(scene, 8), count * 16, false) : NULL;
	if (count && !entries) { return; }
	if (!renderer->obj256) { renderer->obj256 = calloc(1, sizeof(*renderer->obj256)); }
	struct GBAVideoOBJ256* state = renderer->obj256;
	if (!state) { return; }
	for (unsigned n = 0; n < 128; ++n) { state->materials[n].enabled = false; }
	for (unsigned n = 0; n < count; ++n) {
		const uint8_t* entry = entries + n * 16;
		unsigned flags = _obj256Half(entry, 14);
		if (!(flags & 1) || (flags & ~15U)) { continue; }
		uint16_t a = _obj256Half(entry, 8), b = _obj256Half(entry, 10), c = _obj256Half(entry, 12);
		unsigned shape = GBAObjAttributesAGetShape(a), size = GBAObjAttributesBGetSize(b);
		if (shape >= 3) { continue; }
		unsigned width = GBAVideoObjSizes[shape * 4 + size][0], height = GBAVideoObjSizes[shape * 4 + size][1];
		if (!width || !height || width > 64 || height > 64) { continue; }
		unsigned strideCode = (flags >> 1) & 7;
		unsigned sourceRowTiles = strideCode ? strideCode + 1 : width / 8;
		if (sourceRowTiles < width / 8) { continue; }
		unsigned sourceBytes = ((height / 8 - 1) * sourceRowTiles + width / 8) * 64;
		/* Both signatures and native OAM are sampled at the same frame edge. */
		const struct GBAObj* oam = &renderer->d.oam->obj[n];
		if (oam->a != a || oam->b != b || oam->c != c) { continue; }
		const uint8_t* pixels = _obj256Read(gba, _obj256Word(entry, 0), sourceBytes, true);
		const uint8_t* palette = _obj256Read(gba, _obj256Word(entry, 4), 512, true);
		if (!pixels || !palette) { continue; }
		struct OBJ256Material* material = &state->materials[n];
		material->a = a; material->b = b; material->c = c;
		for (unsigned tileRow = 0; tileRow < height / 8; ++tileRow) {
			memcpy(material->pixels + tileRow * width * 8, pixels + tileRow * sourceRowTiles * 64, width * 8);
		}
		for (unsigned color = 0; color < 256; ++color) {
			uint16_t raw = _obj256Half(palette, color * 2);
			if (raw & 0x7FFF) { raw = _obj256Tint(raw, _obj256Word(scene, 20), _obj256Word(scene, 16)); }
			raw = _obj256Tint(raw, _obj256Word(scene, 24), _obj256Word(scene, 28));
			material->palette[color] = mColorFrom555(raw);
		}
		material->enabled = true;
	}
	state->valid = true;
}

bool GBAVideoSoftwareRendererGetOBJ256(struct GBAVideoSoftwareRenderer* renderer,
                                     const struct GBAObj* sprite, unsigned index,
                                     const uint8_t** pixels, const mColor** palette) {
	if (!renderer->obj256 || !renderer->obj256->valid || index >= 128) { return false; }
	const struct OBJ256Material* material = &renderer->obj256->materials[index];
	if (!material->enabled || sprite->a != material->a || sprite->b != material->b || sprite->c != material->c) { return false; }
	*pixels = material->pixels; *palette = material->palette;
	return true;
}

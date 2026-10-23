/* Synthetic HGX2 regression. No commercial ROM or save data is used.
 * Build with -DBUILD_HGX_TEST=ON -DBUILD_STATIC=ON and run mgba-hgx-test.
 * Exercises the real software BG compositor and core save/load entry points. */
#include <mgba/core/core.h>
#include <mgba/gba/core.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/io.h>
#include <mgba/internal/gba/renderers/video-software.h>
#include <mgba/internal/arm/macros.h>
#include <mgba-util/vfs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef USE_ZLIB
#include <zlib.h>
#endif

void GBAVideoSoftwareRendererLatchHGXFrame(struct GBAVideoSoftwareRenderer*);
void GBAVideoSoftwareRendererLatchHGX4(struct GBAVideoSoftwareRenderer*);
void GBAVideoSoftwareRendererLatchOBJ256(struct GBAVideoSoftwareRenderer*);
int GBAVideoSoftwareRendererPreprocessSprite(struct GBAVideoSoftwareRenderer*, struct GBAObj*, int, int);
void GBAVideoSoftwareRendererPostprocessSprite(struct GBAVideoSoftwareRenderer*, int);
void GBAVideoSoftwareRendererDrawBackgroundMode0(struct GBAVideoSoftwareRenderer*, struct GBAVideoSoftwareBackground*, int);

static unsigned checks;
static void require(bool pass, const char* label) {
    ++checks;
    if (!pass) { fprintf(stderr, "FAIL: %s\n", label); exit(1); }
    printf("PASS: %s\n", label);
}
static void entry(uint8_t* ram, unsigned index, uint32_t tile, uint32_t palette, uint32_t flags) {
    STORE_32(tile, 0x1000 + index * 12, ram);
    STORE_32(palette, 0x1004 + index * 12, ram);
    STORE_32(flags, 0x1008 + index * 12, ram);
}
static void drawSameFrame(struct GBAVideoSoftwareRenderer* renderer, int y) {
    for (unsigned x = 0; x < 240; ++x) { renderer->row[x] = FLAG_UNWRITTEN; }
    renderer->start = 0; renderer->end = 32;
    GBAVideoSoftwareRendererDrawBackgroundMode0(renderer, &renderer->bg[0], y);
}
static void draw(struct GBAVideoSoftwareRenderer* renderer, int y) {
    GBAVideoSoftwareRendererLatchHGXFrame(renderer);
    drawSameFrame(renderer, y);
}
static bool isColor(struct GBAVideoSoftwareRenderer* renderer, unsigned x, unsigned bgr) {
    return (renderer->row[x] & 0xFFFFFF) == (mColorFrom555(bgr) & 0xFFFFFF);
}

static void h4Draw(struct GBAVideoSoftwareRenderer* renderer, unsigned bg, int y, bool latch) {
	if (latch) { GBAVideoSoftwareRendererLatchHGX4(renderer); }
	for (unsigned x = 0; x < 240; ++x) { renderer->row[x] = FLAG_UNWRITTEN; }
	renderer->start = 0; renderer->end = 240;
	GBAVideoSoftwareRendererDrawBackgroundMode0(renderer, &renderer->bg[bg], y);
}

static void h4Tests(struct GBA* gba, struct GBAVideoSoftwareRenderer* renderer) {
	uint8_t* rom = (uint8_t*) gba->memory.rom;
	uint8_t* ram = (uint8_t*) gba->memory.wram;
	uint8_t* control = ram + 0x10000;
	uint8_t* scene = ram + 0x10040;
	uint8_t* cells = ram + 0x11000;
	memset(control, 0, 128); memset(cells, 0, 3072);
	STORE_32(0x34584748, 0, control); STORE_32(32, 4, control);
	STORE_32(64, 8, control); STORE_32(16, 12, control);
	STORE_32(0x02010040, 16, control);
	STORE_32(14, 4, scene); STORE_32(0x02011000, 8, scene);
	STORE_32(16, 12, scene); STORE_32(12, 16, scene);
	const unsigned colors[5] = { 0x001F, 0x03E0, 0x7C00, 0x03FF, 0x7C1F };
	for (unsigned i = 0; i < 5; ++i) {
		unsigned layout = 0x3000 + i * 32, set = 0x3100 + i * 32;
		memset(rom + layout, 0, 24); memset(rom + set, 0, 24);
		STORE_32(0x08000000 + set, layout + 16, rom); STORE_32(0x08000000 + set, layout + 20, rom);
		STORE_32(0x08003200 + i * 64, set + 4, rom); STORE_32(0x08003400 + i * 32, set + 8, rom);
		STORE_32(0x08003500 + i * 16, set + 12, rom); STORE_32(0x08003580 + i * 4, set + 20, rom);
		memset(rom + 0x3200 + i * 64, 0x11, 64);
		memset(rom + 0x3400 + i * 32, 0, 32); STORE_16(colors[i], 0x3402 + i * 32, rom);
		STORE_16(0x7C00, 0x3404 + i * 32, rom);
		for (unsigned q = 0; q < 8; ++q) { STORE_16(0, 0x3500 + i * 16 + q * 2, rom); }
		STORE_32(0, 0x3580 + i * 4, rom);
		STORE_32(0x08000000 + layout, i * 16, cells);
	}
	GBAIOWrite32(gba, 0x3E0, 0x02010000);
	for (unsigned bg = 1; bg < 4; ++bg) {
		memset(&renderer->bg[bg], 0, sizeof(renderer->bg[bg])); renderer->bg[bg].index = bg;
		renderer->bg[bg].flags = FLAG_IS_BACKGROUND; renderer->bg[bg].objwinFlags = FLAG_IS_BACKGROUND;
	}
	renderer->dispcnt = 0; renderer->blendEffect = BLEND_NONE; renderer->bldy = 0;
	memset(gba->video.vram, 0xFF, 0x18000);
	h4Draw(renderer, 2, 0, true);
	for (unsigned i = 0; i < 5; ++i) { require(isColor(renderer, i * 16, colors[i]), "HGX4 current plus four resident maps retain distinct material identity"); }
	h4Draw(renderer, 3, 0, false);
	require(renderer->row[0] == FLAG_UNWRITTEN, "HGX4 inactive native plane is intrinsically transparent");
	STORE_32(0x08003040, 0, cells); /* owner changes after frame publication */
	h4Draw(renderer, 2, 0, false);
	require(isColor(renderer, 0, colors[0]), "HGX4 midframe cell changes do not affect immutable material snapshot");
	h4Draw(renderer, 2, 0, true);
	require(isColor(renderer, 0, colors[2]), "HGX4 next frame adopts changed owner without framebuffer hold");
	STORE_32(0x08003000, 0, cells);
	h4Draw(renderer, 2, 0, true);
	require(isColor(renderer, 0, colors[0]), "HGX4 A to B to C to A restores original material without visit cache");
	/* Native tile indices/VRAM offsets and camera scroll are irrelevant. */
	renderer->bg[2].x = 143; renderer->bg[2].y = 231; renderer->bg[2].screenBase = 0xF000;
	h4Draw(renderer, 2, 0, true);
	require(isColor(renderer, 0, colors[0]), "HGX4 material ignores native VRAM tilemap and ring coordinates");
	STORE_32((uint32_t) -8, 20, scene);
	h4Draw(renderer, 2, 0, true);
	require(isColor(renderer, 8, colors[1]), "HGX4 authoritative camera origin advances smooth subcell scroll");
	STORE_32(0, 20, scene);
	/* One deterministic cyclic strip, including cold clock before first event. */
	STORE_32(0x08000BAD, 0x3110, rom);
	STORE_32(0x08000BAD, 0x3600, rom); STORE_32(0, 0x3604, rom); STORE_32(0x08003620, 0x3608, rom);
	STORE_32(16, 0x3620, rom); STORE_32(1, 0x3624, rom); STORE_32(0x08003640, 0x3628, rom);
	STORE_32(0, 0x362C, rom); STORE_32(0, 0x3630, rom);
	STORE_32(0, 0x3640, rom); STORE_32(1, 0x3644, rom); STORE_32(2, 0x3648, rom); STORE_32(0x08003660, 0x364C, rom);
	STORE_32(1, 0x3660, rom); STORE_32(0x08003700, 0x3664, rom);
	STORE_32(9, 0x3668, rom); STORE_32(0x08003720, 0x366C, rom);
	memset(rom + 0x3700, 0x11, 32); memset(rom + 0x3720, 0x22, 32);
	STORE_32(0x08003600, 24, control); STORE_32(1, 28, control);
	h4Draw(renderer, 2, 0, true);
	require(isColor(renderer, 0, 0x7C00), "HGX4 cold unvisited animated material clock zero uses previous cyclic frame");
	STORE_32(1, 28, scene); h4Draw(renderer, 2, 0, true);
	require(isColor(renderer, 0, 0x001F), "HGX4 native animation starts exact deterministic phase boundary");
	STORE_32(9, 28, scene); h4Draw(renderer, 2, 0, true);
	require(isColor(renderer, 0, 0x7C00), "HGX4 native animation advances without live native callback or VRAM");
	STORE_32(0x08003000, 16, cells); /* duplicate foreign owner */
	h4Draw(renderer, 2, 0, true);
	require(isColor(renderer, 0, 0x7C00) && isColor(renderer, 16, 0x7C00), "HGX4 same world material and clock yield identical current and foreign pixels");
	STORE_32(8, 32, scene); STORE_32(0x03E0, 36, scene); STORE_32(1, 40, scene);
	STORE_32(8, 60, scene); STORE_32(0, 56, scene);
	h4Draw(renderer, 2, 0, true);
	require(isColor(renderer, 0, (7 << 10) | (7 << 5)), "HGX4 DNS then fade use exact shared five-bit interpolation");
	STORE_32(0, 32, scene); STORE_32(0, 60, scene);
	/* Direct native door writes are represented by explicit literal references. */
	uint8_t* overlay = ram + 0x12000; memset(overlay, 0, 36);
	STORE_32(0x08003080, 8, overlay); STORE_32(2, 12, overlay);
	STORE_32(0x02012000, 44, scene); STORE_32(1, 48, scene);
	h4Draw(renderer, 1, 0, true);
	require(isColor(renderer, 0, colors[4]), "HGX4 explicit door overlay resolves literal materials without native fallback");
	h4Draw(renderer, 2, 0, false);
	require(renderer->row[0] == FLAG_UNWRITTEN, "HGX4 door layer change clears previously occupied plane");
	STORE_32(0x200, 12, overlay); STORE_32(0x08003720, 32, overlay);
	for (unsigned q = 0; q < 4; ++q) { STORE_16(1016, 16 + q * 2, overlay); }
	h4Draw(renderer, 2, 0, true);
	require(isColor(renderer, 0, 0x7C00), "HGX4 native door tile slots resolve explicit ROM frame source without VRAM");
	h4Draw(renderer, 1, 0, false);
	require(renderer->row[0] == FLAG_UNWRITTEN, "HGX4 native door upper plane stays transparent even when HG tile zero is opaque");
	/* Palette-only schedules share the same serialized world clock. */
	STORE_32(0, 48, scene); STORE_32(0, 0x3624, rom);
	STORE_32(1, 0x362C, rom); STORE_32(0x08003680, 0x3630, rom);
	STORE_32(0, 0x3680, rom); STORE_32(16, 0x3684, rom);
	STORE_32(2, 0x3688, rom); STORE_32(0x080036A0, 0x368C, rom);
	STORE_32(0, 0x36A0, rom); STORE_32(0x08003740, 0x36A4, rom);
	STORE_32(8, 0x36A8, rom); STORE_32(0x08003760, 0x36AC, rom);
	memset(rom + 0x3740, 0, 64); STORE_16(0x03E0, 0x3742, rom); STORE_16(0x7FFF, 0x3762, rom);
	STORE_32(0, 28, scene); h4Draw(renderer, 2, 0, true);
	require(isColor(renderer, 0, 0x03E0) && isColor(renderer, 16, 0x03E0), "HGX4 current and foreign palette-only animation share clock zero");
	STORE_32(8, 28, scene); h4Draw(renderer, 2, 0, true);
	require(isColor(renderer, 0, 0x7FFF), "HGX4 palette-only schedule advances without native palette RAM");
	STORE_32(0, 48, scene); STORE_32(0, 28, control);
	STORE_32(0, 0x3110, rom);
	/* Native compressed atlases use the same material route as raw HG data. */
	STORE_32(0x00002010, 0x3800, rom);
	for (unsigned group = 0; group < 4; ++group) {
		rom[0x3804 + group * 9] = 0;
		memset(rom + 0x3805 + group * 9, 0x11, 8);
	}
	rom[0x3140] = 1; STORE_32(0x08003800, 0x3144, rom);
	h4Draw(renderer, 2, 0, true);
	require(isColor(renderer, 32, colors[2]), "HGX4 compressed native atlas and raw HG atlas use one explicit resolver");
	STORE_32(0x08003FF0, 0x3144, rom);
	h4Draw(renderer, 2, 0, true);
	require(renderer->row[32] == FLAG_UNWRITTEN, "HGX4 invalid material remains transparent without native BG fallback");
	/* Global palette-bank addressing and secondary local tile normalization. */
	STORE_32(0x08003120, 0x3014, rom); STORE_16(640, 0x3500, rom);
	h4Draw(renderer, 2, 0, true);
	require(isColor(renderer, 0, colors[0]), "HGX4 secondary tile zero uses local atlas index and primary global palette bank");
	STORE_16(0, 0x3500, rom);
	/* State restore reconstructs cache from serialized scene clock. */
	STORE_32(0, 4, scene); h4Draw(renderer, 2, 0, true);
	require(renderer->hgx4 != NULL, "HGX4 host scene cache remains rebuildable outside serialized game state");
	STORE_32(0, 0, control); /* opt out before original regressions resume */
	GBAVideoSoftwareRendererLatchHGX4(renderer);
}
static void obj256Setup(struct GBAVideoSoftwareRenderer* renderer) {
	renderer->start = 0; renderer->end = 64; renderer->dispcnt = 0x40;
	renderer->objOffsetX = 0; renderer->objOffsetY = 0;
	renderer->target1Obj = false; renderer->target2Bd = false; renderer->forceTarget1 = false;
	renderer->blendEffect = BLEND_NONE; renderer->bldy = 0;
	renderer->currentWindow.packed = 0x3F; renderer->currentWindow.priority = 0;
	renderer->objwin.packed = 0x3F; renderer->objwin.priority = 0;
	for (unsigned n = 0; n < 4; ++n) { renderer->bg[n].target2 = false; }
	renderer->normalPalette[0x101] = mColorFrom555(0x03FF);
}
static void obj256Entry(uint8_t* ram, unsigned index, const struct GBAObj* sprite, uint32_t palette) {
	uint8_t* entry = ram + 0x20200 + index * 16;
	STORE_32(0x08004000, 0, entry); STORE_32(palette, 4, entry);
	STORE_16(sprite->a, 8, entry); STORE_16(sprite->b, 10, entry); STORE_16(sprite->c, 12, entry);
	STORE_16(1, 14, entry);
}
static void obj256Draw(struct GBAVideoSoftwareRenderer* renderer, unsigned index, int y, bool latch) {
	if (latch) { GBAVideoSoftwareRendererLatchOBJ256(renderer); }
	for (unsigned x = 0; x < 240; ++x) { renderer->row[x] = FLAG_UNWRITTEN; renderer->spriteLayer[x] = FLAG_UNWRITTEN; }
	GBAVideoSoftwareRendererPreprocessSprite(renderer, &renderer->d.oam->obj[index], index, y);
}
static bool obj256Color(struct GBAVideoSoftwareRenderer* renderer, unsigned x, unsigned color) {
	return renderer->spriteLayer[x] != FLAG_UNWRITTEN && (renderer->spriteLayer[x] & 0xFFFFFF) == (mColorFrom555(color) & 0xFFFFFF);
}
static void obj256Tests(struct mCore* core) {
	struct GBA* gba = core->board;
	uint8_t* fixture = calloc(1, 0x10000);
	require(fixture != NULL, "allocate original synthetic OBJ256 fixture");
	memcpy(fixture, gba->memory.rom, 0x4000);
	memset(fixture + 0x4000, 1, 64);
	fixture[0x4000] = 0; fixture[0x4001] = 15; fixture[0x4002] = 16; fixture[0x4003] = 255;
	fixture[0x4038] = 255; fixture[0x4039] = 16;
	STORE_16(0x03FF, 0x4102, fixture); STORE_16(0x001F, 0x411E, fixture);
	STORE_16(0x03E0, 0x4120, fixture); STORE_16(0x7C00, 0x42FE, fixture);
	STORE_16(0x7FFF, 0x4302, fixture); STORE_16(0x7FE0, 0x431E, fixture);
	STORE_16(0x7C1F, 0x4320, fixture); STORE_16(0x03FF, 0x44FE, fixture);
	memset(fixture + 0x4500, 15, 64); memset(fixture + 0x4540, 16, 64);
	memset(fixture + 0x4580, 255, 64); memset(fixture + 0x45C0, 1, 64);
	memset(fixture + 0x4800, 1, 1024);
	memset(fixture + 0x4800 + 5 * 64, 16, 64); memset(fixture + 0x4800 + 6 * 64, 255, 64);
	memset(fixture + 0x4800 + 9 * 64, 15, 64);
	core->unloadROM(core);
	require(core->loadROM(core, VFileMemChunk(fixture, 0x10000)), "load original synthetic OBJ256 ROM");
	free(fixture); core->reset(core);
	struct GBAVideoSoftwareRenderer* renderer = (struct GBAVideoSoftwareRenderer*) gba->video.renderer;
	uint8_t* ram = (uint8_t*) gba->memory.wram;
	uint8_t* control = ram + 0x20000; uint8_t* scene = ram + 0x20100;
	memset(control, 0, 0x1000);
	STORE_32(0x34584748, 0, control); STORE_32(40, 4, control); STORE_32(64, 8, control);
	STORE_32(16, 12, control); STORE_32(1, 20, control);
	STORE_32(0x02020100, 32, control); STORE_32(32, 36, control);
	STORE_32(2, 4, scene); STORE_32(0x02020200, 8, scene); STORE_32(16, 12, scene);
	GBAIOWrite32(gba, 0x3E0, 0x02020000);
	memset(gba->video.oam.raw, 0, 0x400); memset((uint8_t*) gba->video.vram + 0x10000, 0x11, 0x8000);
	obj256Setup(renderer);
	struct GBAObj* sprite = &renderer->d.oam->obj[0];
	obj256Entry(ram, 0, sprite, 0x08004100);
	obj256Entry(ram, 1, &renderer->d.oam->obj[1], 0x08004300);
	/* Extended controls must drive field BG and independent OBJ together. */
	uint8_t* field = ram + 0x21000; memset(field, 0, 64); memset(ram + 0x22000, 0, 3072);
	STORE_32(0x02021000, 16, control); STORE_32(4, 4, field);
	STORE_32(0x02022000, 8, field); STORE_32(16, 12, field); STORE_32(12, 16, field);
	STORE_32(0x08003000, 0x22000, ram);
	renderer->bg[2].flags = FLAG_IS_BACKGROUND;
	h4Draw(renderer, 2, 0, true);
	require(isColor(renderer, 0, 0x001F), "OBJ256 forty-byte control retains HGX4 field BG material rendering");
	obj256Draw(renderer, 0, 0, true);
	require(renderer->spriteLayer[0] == FLAG_UNWRITTEN, "OBJ256 index zero remains transparent");
	require(obj256Color(renderer, 1, 0x001F), "OBJ256 palette index fifteen retains full color");
	require(obj256Color(renderer, 2, 0x03E0), "OBJ256 palette index sixteen is not truncated to four bits");
	require(obj256Color(renderer, 3, 0x7C00), "OBJ256 palette index 255 is rendered from private palette");
	obj256Draw(renderer, 1, 0, false);
	require(obj256Color(renderer, 1, 0x7FE0) && obj256Color(renderer, 2, 0x7C1F), "OBJ256 each simultaneous OAM actor has an independent 256-color palette");
	renderer->d.oam->obj[2].a = 0x2000; renderer->normalPalette[0x111] = mColorFrom555(0x7FFF);
	obj256Draw(renderer, 2, 0, false);
	require(obj256Color(renderer, 0, 0x7FFF), "OBJ256 unregistered native eight-bit sprite keeps shared native palette");
	sprite->b = 0x1000; obj256Entry(ram, 0, sprite, 0x08004100); obj256Draw(renderer, 0, 0, true);
	require(obj256Color(renderer, 4, 0x7C00) && obj256Color(renderer, 6, 0x001F) && renderer->spriteLayer[7] == FLAG_UNWRITTEN,
	        "OBJ256 horizontal flip preserves high indices and transparency");
	sprite->b = 0x2000; obj256Entry(ram, 0, sprite, 0x08004100); obj256Draw(renderer, 0, 0, true);
	require(obj256Color(renderer, 0, 0x7C00) && obj256Color(renderer, 1, 0x03E0), "OBJ256 vertical flip samples final source row");
	sprite->a = 0x100; sprite->b = 0;
	renderer->d.oam->mat[0].a = 256; renderer->d.oam->mat[0].b = 0;
	renderer->d.oam->mat[0].c = 0; renderer->d.oam->mat[0].d = 256;
	obj256Entry(ram, 0, sprite, 0x08004100); obj256Draw(renderer, 0, 0, true);
	require(obj256Color(renderer, 1, 0x001F) && obj256Color(renderer, 3, 0x7C00), "OBJ256 native affine identity preserves private 8bpp material");
	sprite->a = 0x300; obj256Entry(ram, 0, sprite, 0x08004100); obj256Draw(renderer, 0, 4, true);
	require(obj256Color(renderer, 5, 0x001F) && obj256Color(renderer, 7, 0x7C00), "OBJ256 affine double-size uses native transformed geometry");
	sprite->a = 0; sprite->b = 0x4000; obj256Entry(ram, 0, sprite, 0x08004100);
	STORE_32(0x08004500, 0x20200, ram); renderer->dispcnt = 0; obj256Draw(renderer, 0, 0, true);
	require(obj256Color(renderer, 0, 0x001F) && obj256Color(renderer, 8, 0x03E0), "OBJ256 tile-ordered 16 by 16 frame ignores native two-dimensional mapping");
	obj256Draw(renderer, 0, 8, false);
	require(obj256Color(renderer, 0, 0x7C00) && obj256Color(renderer, 8, 0x03FF), "OBJ256 multi-tile source uses correct bottom tile-row stride");
	/* Subsprites crop a parent frame by explicit source tile-row stride. */
	STORE_32(0x08004940, 0x20200, ram); STORE_16(7, 0x2020E, ram);
	obj256Draw(renderer, 0, 0, true);
	require(obj256Color(renderer, 0, 0x03E0) && obj256Color(renderer, 8, 0x7C00), "OBJ256 sixteen-pixel child crops correct top row from 32-pixel parent");
	obj256Draw(renderer, 0, 8, false);
	require(obj256Color(renderer, 0, 0x001F) && obj256Color(renderer, 8, 0x03FF), "OBJ256 child crop skips parent tile-row gap without sampling neighboring tiles");
	renderer->d.oam->obj[1].b = 0x4010;
	obj256Entry(ram, 1, &renderer->d.oam->obj[1], 0x08004300);
	STORE_32(0x08004940, 0x20210, ram); STORE_16(7, 0x2021E, ram);
	obj256Draw(renderer, 1, 0, true);
	require(obj256Color(renderer, 16, 0x7C1F) && obj256Color(renderer, 24, 0x03FF), "OBJ256 cropped children retain independent parent palettes");
	STORE_32(0x0800FF00, 0x20200, ram); obj256Draw(renderer, 0, 0, true);
	require(obj256Color(renderer, 0, 0x03FF), "OBJ256 strided source validates full span rather than packed child byte count");
	sprite->b = 0x8000; obj256Entry(ram, 0, sprite, 0x08004100); STORE_16(3, 0x2020E, ram);
	obj256Draw(renderer, 0, 0, true);
	require(obj256Color(renderer, 0, 0x03FF), "OBJ256 source stride smaller than child width safely falls back");
	sprite->b = 0; obj256Entry(ram, 0, sprite, 0x08004100); obj256Draw(renderer, 0, 0, true);
	STORE_32(0x08004300, 0x20204, ram); obj256Draw(renderer, 0, 0, false);
	require(obj256Color(renderer, 1, 0x001F), "OBJ256 palette changes remain invisible until next frame snapshot");
	obj256Draw(renderer, 0, 0, true);
	require(obj256Color(renderer, 1, 0x7FE0), "OBJ256 next frame switches palette atomically");
	obj256Entry(ram, 0, sprite, 0x08004100); obj256Draw(renderer, 0, 0, true);
	sprite->b = 1; obj256Draw(renderer, 0, 0, false);
	require(obj256Color(renderer, 1, 0x03FF), "OBJ256 reused OAM position signature falls back to native 16-color material");
	sprite->b = 0; sprite->c = 1; obj256Draw(renderer, 0, 0, false);
	require(obj256Color(renderer, 0, 0x03FF), "OBJ256 changed native tile binding rejects stale private material");
	sprite->c = 0; sprite->a = 0x1000; obj256Draw(renderer, 0, 0, false);
	require(obj256Color(renderer, 0, 0x03FF), "OBJ256 changed native attribute A rejects stale private material");
	sprite->a = 0;
	sprite->b = 0; obj256Entry(ram, 0, sprite, 0x08004100); STORE_16(0x11, 0x2020E, ram);
	obj256Draw(renderer, 0, 0, true);
	require(obj256Color(renderer, 0, 0x03FF), "OBJ256 unknown entry flags retain native OBJ behavior");
	obj256Entry(ram, 0, sprite, 0x08004100); STORE_32(0x0800FFF0, 0x20200, ram);
	obj256Draw(renderer, 0, 0, true);
	require(obj256Color(renderer, 0, 0x03FF), "OBJ256 out-of-range source safely falls back to native OBJ");
	obj256Entry(ram, 0, sprite, 0x08004100);
	STORE_32(8, 16, scene); STORE_32(0x03E0, 20, scene); STORE_32(8, 28, scene);
	obj256Draw(renderer, 0, 0, true);
	require(obj256Color(renderer, 3, (7 << 10) | (7 << 5)), "OBJ256 raw private palette receives exact global DNS then fade");
	STORE_32(0, 16, scene); STORE_32(0, 28, scene);
	renderer->target1Obj = true; renderer->blendEffect = BLEND_BRIGHTEN; renderer->bldy = 16;
	obj256Draw(renderer, 0, 0, true);
	require(obj256Color(renderer, 3, 0x7FFF), "OBJ256 native brightness compositor applies to private palette");
	obj256Setup(renderer);
	sprite->a = 0x400; obj256Entry(ram, 0, sprite, 0x08004100);
	renderer->bg[0].target2 = true; renderer->bg[0].enabled = 4;
	obj256Draw(renderer, 0, 0, true);
	require((renderer->spriteLayer[3] & (FLAG_REBLEND | FLAG_TARGET_1)) == (FLAG_REBLEND | FLAG_TARGET_1),
	        "OBJ256 native semi-transparent mode retains alpha compositor flags");
	obj256Setup(renderer);
	sprite->a = 0x800; obj256Entry(ram, 0, sprite, 0x08004100); renderer->dispcnt |= 0x8000;
	obj256Draw(renderer, 0, 0, true);
	require(!(renderer->row[0] & FLAG_OBJWIN) && (renderer->row[3] & FLAG_OBJWIN), "OBJ256 native object-window mode uses eight-bit source alpha");
	obj256Setup(renderer); sprite->a = 0; obj256Entry(ram, 0, sprite, 0x08004100);
	STORE_32(0, 20, control); obj256Draw(renderer, 0, 0, true);
	require(obj256Color(renderer, 0, 0x03FF), "OBJ256 absent capability leaves original 16-color renderer unchanged");
	STORE_32(1, 20, control);
	uint8_t* loadedRom = (uint8_t*) gba->memory.rom; loadedRom[0xB5] = 0;
	obj256Draw(renderer, 0, 0, true);
	require(obj256Color(renderer, 0, 0x03FF), "OBJ256 non-HGX ROM never activates sidecar materials");
	loadedRom[0xB5] = 'H';
	void* state = malloc(core->stateSize(core));
	require(state && core->saveState(core, state), "save original OBJ256 scene state");
	STORE_32(0, 20, control); obj256Draw(renderer, 0, 0, true);
	require(core->loadState(core, state), "restore original OBJ256 scene state");
	obj256Setup(renderer); obj256Draw(renderer, 0, 0, true);
	require(obj256Color(renderer, 1, 0x001F) && obj256Color(renderer, 3, 0x7C00), "OBJ256 save reload reconstructs private materials from serialized scene");
	free(state);
}

static void bg256Tests(struct mCore* core) {
	struct GBA* gba = core->board;
	struct GBAVideoSoftwareRenderer* renderer = (struct GBAVideoSoftwareRenderer*) gba->video.renderer;
	uint8_t* rom = (uint8_t*) gba->memory.rom; uint8_t* ram = (uint8_t*) gba->memory.wram;
	uint8_t* control = ram + 0x28000; uint8_t* scene = ram + 0x28100; uint8_t* cells = ram + 0x28200;
	memset(control, 0, 40); memset(scene, 0, 64); memset(cells, 0, 3072);
	STORE_32(0x34584748, 0, control); STORE_32(40, 4, control); STORE_32(64, 8, control);
	STORE_32(16, 12, control); STORE_32(0x02028100, 16, control); STORE_32(1, 20, control);
	STORE_32(0x02020100, 32, control); STORE_32(32, 36, control);
	STORE_32(14, 4, scene); STORE_32(0x02028200, 8, scene); STORE_32(16, 12, scene); STORE_32(12, 16, scene);
	for (unsigned n = 0; n < 4; ++n) {
		unsigned address = 0x5000 + n * 32; memset(rom + address, 0, 24);
		STORE_32(0x08005100, address + 16, rom); STORE_32(0x08005120, address + 20, rom);
	}
	STORE_32(0x08005140, 0x5054, rom); /* layout C: native4 secondary with marked8 primary palette owner */
	STORE_32(0x08005160, 0x5070, rom); /* layout D: rejected compressed8 overlay */
	for (unsigned role = 0; role < 2; ++role) {
		unsigned set = 0x5100 + role * 32; memset(rom + set, 0, 28);
		rom[set + 2] = 0xA8; rom[set + 3] = 1;
		STORE_32(0x08006000 + role * 512, set + 4, rom);
		STORE_32(0x08007000 + role * 512, set + 8, rom);
		STORE_32(0x08007800 + role * 64, set + 12, rom);
		STORE_32(0x08007880 + role * 4, set + 20, rom);
		STORE_32(0x08007400 + role * 512, set + 24, rom);
		memset(rom + 0x6000 + role * 512, 1, 64);
		uint8_t* pixels = rom + 0x6000 + role * 512;
		pixels[0] = 0; pixels[1] = 15; pixels[2] = 16; pixels[3] = 255; pixels[4] = 18;
		pixels[56] = 16; pixels[57] = 255;
		memset(pixels + 64, 17, 64); memset(pixels + 128, 0, 64);
		memset(rom + 0x7000 + role * 512, 0, 512); memset(rom + 0x7400 + role * 512, 0, 512);
		STORE_16(role ? 0x7FE0 : 0x001F, 0x701E + role * 512, rom);
		STORE_16(role ? 0x7C1F : 0x03E0, 0x7020 + role * 512, rom);
		STORE_16(role ? 0x03FF : 0x7C00, 0x71FE + role * 512, rom);
		STORE_16(0x7FFF, 0x7002 + role * 512, rom); STORE_16(0x03FF, 0x7022 + role * 512, rom);
		for (unsigned q = 0; q < 8; ++q) {
			unsigned index = q < 4 ? (q == 1 ? 1 : 0) : 2;
			unsigned flip = q == 2 ? 0x400 : q == 3 ? 0x800 : 0;
			STORE_16((role ? 640 : 0) + index + flip + 0xF000, 0x7800 + role * 64 + q * 2, rom);
		}
		STORE_32(0, 0x7880 + role * 4, rom);
	}
	memset(rom + 0x5140, 0, 28); STORE_32(0x08006400, 0x5144, rom);
	STORE_32(0x08007900, 0x514C, rom); STORE_32(0x08007920, 0x5154, rom);
	memset(rom + 0x6400, 0x11, 32); STORE_32(0, 0x7920, rom);
	for (unsigned q = 0; q < 8; ++q) { STORE_16(0x3280, 0x7900 + q * 2, rom); }
	STORE_16(0x03FF, 0x7462, rom); STORE_16(0x7C1F, 0x7464, rom); /* legacy bank3 colors1/2 */
	memcpy(rom + 0x5160, rom + 0x5100, 28); rom[0x5160] = 1;
	for (unsigned n = 0; n < 4; ++n) { STORE_32(n == 2 ? 0x08005040 : 0x08005000, n * 16, cells); }
	STORE_32(640, 20, cells); STORE_32(640, 36, cells);
	GBAIOWrite32(gba, 0x3E0, 0x02028000); obj256Setup(renderer);
	for (unsigned bg = 1; bg < 4; ++bg) { renderer->bg[bg].flags = FLAG_IS_BACKGROUND; }
	h4Draw(renderer, 2, 0, true);
	require(renderer->row[0] == FLAG_UNWRITTEN, "BG256 index zero remains transparent");
	require(isColor(renderer, 1, 0x001F) && isColor(renderer, 2, 0x03E0) && isColor(renderer, 3, 0x7C00), "BG256 map chips render palette indices 15, 16 and 255 without truncation");
	require(renderer->row[4] != FLAG_UNWRITTEN && isColor(renderer, 4, 0), "BG256 nonzero palette index remains opaque even when its RGB color is black");
	require(isColor(renderer, 8, 0x03FF), "BG256 tile indices advance by 64 bytes");
	require(isColor(renderer, 17, 0x7FE0) && isColor(renderer, 18, 0x7C1F) && isColor(renderer, 19, 0x03FF), "BG256 secondary role uses its own 256 palette and local tile index");
	require(isColor(renderer, 32, 0x03FF), "BG256 mixed native4 source uses retained legacy palette of marked8 palette owner");
	require(isColor(renderer, 49, 0x001F) && isColor(renderer, 51, 0x7C00), "BG256 identical current and foreign materials share one resolver");
	h4Draw(renderer, 2, 8, false);
	require(isColor(renderer, 4, 0x7C00) && isColor(renderer, 6, 0x001F) && renderer->row[7] == FLAG_UNWRITTEN, "BG256 horizontal flip preserves eight-bit alpha and colors");
	require(isColor(renderer, 8, 0x03E0) && isColor(renderer, 9, 0x7C00), "BG256 vertical flip uses eight-byte source rows");
	h4Draw(renderer, 3, 0, false); require(renderer->row[1] == FLAG_UNWRITTEN, "BG256 unused plane is transparent despite opaque authored tile zero");
	STORE_32(8, 32, scene); STORE_32(0x03E0, 36, scene); STORE_32(1, 40, scene);
	h4Draw(renderer, 2, 0, true);
	require(isColor(renderer, 3, (15 << 10) | (15 << 5)) && isColor(renderer, 19, 0x03FF), "BG256 DNS mask uses role banks zero and seven while reference palette nibble is ignored");
	STORE_32(8, 60, scene); h4Draw(renderer, 2, 0, true);
	require(isColor(renderer, 3, (7 << 10) | (7 << 5)), "BG256 shared palettes receive native five-bit DNS then fade");
	STORE_32(0, 32, scene); STORE_32(0, 60, scene);
	STORE_32(0x08000BAD, 0x5110, rom); STORE_32(0x08007A00, 24, control); STORE_32(1, 28, control);
	STORE_32(0x08000BAD, 0x7A00, rom); STORE_32(0, 0x7A04, rom); STORE_32(0x08007A20, 0x7A08, rom);
	STORE_32(16, 0x7A20, rom); STORE_32(1, 0x7A24, rom); STORE_32(0x08007A40, 0x7A28, rom);
	STORE_32(0, 0x7A2C, rom); STORE_32(0, 0x7A30, rom);
	STORE_32(0, 0x7A40, rom); STORE_32(1, 0x7A44, rom); STORE_32(1, 0x7A48, rom); STORE_32(0x08007A60, 0x7A4C, rom);
	STORE_32(0, 0x7A60, rom); STORE_32(0x08007A80, 0x7A64, rom); memset(rom + 0x7A80, 0x22, 32);
	h4Draw(renderer, 2, 0, true);
	require(isColor(renderer, 1, 0x001F) && isColor(renderer, 3, 0x7C00), "BG256 legacy 32-byte animation recipe never overwrites marked raw8 base frame");
	uint8_t* overlay = ram + 0x29000; memset(overlay, 0, 36);
	STORE_32(0x08005000, 8, overlay); STORE_32(0x200, 12, overlay); STORE_32(0x08007B00, 32, overlay);
	for (unsigned q = 0; q < 4; ++q) { STORE_16(0x33F8, 16 + q * 2, overlay); }
	memset(rom + 0x7B00, 0x22, 32); STORE_32(0x02029000, 44, scene); STORE_32(1, 48, scene);
	h4Draw(renderer, 2, 0, true);
	require(isColor(renderer, 0, 0x7C1F) && isColor(renderer, 7, 0x7C1F), "BG256 native door keeps explicit four-bit frame and original banked legacy palette");
	STORE_32(0, 0x5118, rom); h4Draw(renderer, 2, 0, true);
	require(renderer->row[0] == FLAG_UNWRITTEN && isColor(renderer, 1, 0x001F), "BG256 missing legacy door palette rejects overlay and preserves authored base cell");
	STORE_32(0x08007400, 0x5118, rom);
	STORE_32(0x08005060, 8, overlay); STORE_32(0, 32, overlay);
	for (unsigned q = 0; q < 4; ++q) { STORE_16(0, 16 + q * 2, overlay); }
	h4Draw(renderer, 2, 0, true);
	require(isColor(renderer, 1, 0x001F), "BG256 unsupported compressed8 overlay preserves valid base cell");
	rom[0x5160] = 0; rom[0x5163] = 2; h4Draw(renderer, 2, 0, true);
	require(isColor(renderer, 1, 0x001F), "BG256 unknown marked version rejects overlay without decoding as native4");
	STORE_32(0, 48, scene); STORE_32(0, 28, control); STORE_32(0, 0x5110, rom);
	/* Compose a high-index private OBJ on top of a high-index field tile. */
	h4Draw(renderer, 2, 0, true); GBAVideoSoftwareRendererLatchOBJ256(renderer);
	for (unsigned x = 0; x < 240; ++x) { renderer->spriteLayer[x] = FLAG_UNWRITTEN; }
	GBAVideoSoftwareRendererPreprocessSprite(renderer, &renderer->d.oam->obj[1], 1, 0);
	GBAVideoSoftwareRendererPostprocessSprite(renderer, 0);
	require(isColor(renderer, 17, 0x7C1F), "BG256 and independent OBJ256 compose together with native object priority");
	void* state = malloc(core->stateSize(core)); require(state && core->saveState(core, state), "save simultaneous BG256 and OBJ256 scene");
	STORE_32(0, 4, scene); h4Draw(renderer, 2, 0, true);
	require(core->loadState(core, state), "restore simultaneous BG256 and OBJ256 scene");
	obj256Setup(renderer); renderer->bg[2].flags = FLAG_IS_BACKGROUND;
	h4Draw(renderer, 2, 0, true); require(isColor(renderer, 3, 0x7C00) && isColor(renderer, 19, 0x03FF), "BG256 reload reconstructs role palettes and material caches");
	free(state);
}

#ifdef USE_ZLIB
static unsigned hgzFixture(uint8_t* destination, const uint8_t* raw, unsigned rawBytes) {
	uLongf packedBytes = compressBound(rawBytes);
	int status = compress2(destination + 20, &packedBytes, raw, rawBytes, Z_BEST_COMPRESSION);
	require(status == Z_OK && packedBytes <= UINT32_MAX, "create synthetic HGZ1 zlib payload");
	STORE_32(0x315A4748, 0, destination); STORE_32(1, 4, destination);
	STORE_32(rawBytes, 8, destination); STORE_32((uint32_t) packedBytes, 12, destination);
	STORE_32(crc32(0, raw, rawBytes), 16, destination);
	return (unsigned) packedBytes + 20;
}
#endif

static void bgScaled8Tests(struct mCore* core) {
	struct GBA* gba = core->board;
	struct GBAVideoSoftwareRenderer* renderer = (struct GBAVideoSoftwareRenderer*) gba->video.renderer;
	uint8_t* rom = (uint8_t*) gba->memory.rom;
	uint8_t* ram = (uint8_t*) gba->memory.wram;
	uint8_t* control = ram + 0x28000;
	uint8_t* scene = ram + 0x28100;
	uint8_t* cells = ram + 0x28200;
	uint8_t* set = rom + 0x7C20;
	memset(control, 0, 40); memset(scene, 0, 64); memset(cells, 0, 3072);
	STORE_32(0x34584748, 0, control); STORE_32(40, 4, control); STORE_32(64, 8, control);
	STORE_32(16, 12, control); STORE_32(0x02028100, 16, control); STORE_32(1, 20, control);
	STORE_32(14, 4, scene); STORE_32(0x02028200, 8, scene); STORE_32(16, 12, scene); STORE_32(12, 16, scene);
	memset(rom + 0x7C00, 0, 0x400);
	STORE_32(0x08007C20, 0x7C10, rom); STORE_32(0x08007C20, 0x7C14, rom);
	set[2] = 0xA9; set[3] = 1;
	STORE_32(0x08008000, 4, set); STORE_32(0x08007D00, 8, set);
	STORE_32(0x08007C50, 12, set); STORE_32(0x08007C60, 20, set); STORE_32(0x08007D00, 24, set);
	STORE_32(1, 32, set); STORE_32(0, 0x7C60, rom);
	for (unsigned q = 0; q < 8; ++q) { STORE_16(0, 0x7C50 + q * 2, rom); }
	STORE_16(0x001F, 0x7D1E, rom); STORE_16(0x03E0, 0x7D20, rom);
	STORE_32(0x08007C00, 0, cells);
	GBAIOWrite32(gba, 0x3E0, 0x02028000);
	for (unsigned bg = 1; bg < 4; ++bg) { renderer->bg[bg].flags = FLAG_IS_BACKGROUND; }
	const unsigned sides[] = { 8, 16, 32, 64, 128 };
	for (unsigned n = 0; n < sizeof(sides) / sizeof(sides[0]); ++n) {
		unsigned side = sides[n], bytes = side * side;
		STORE_32(side, 28, set); STORE_32(bytes, 36, set);
		for (unsigned y = 0; y < side; ++y) {
			memset(rom + 0x8000 + y * side, 15, side / 2);
			memset(rom + 0x8000 + y * side + side / 2, 16, side / 2);
		}
		h4Draw(renderer, 2, 0, true);
		require(isColor(renderer, 0, 0x001F) && isColor(renderer, 7, 0x03E0),
		        "HGX4 A9 scaled raw8 tile uses centre sampling without changing native footprint");
		STORE_16(0x400, 0x7C50, rom); h4Draw(renderer, 2, 0, true);
		require(isColor(renderer, 0, 0x03E0) && isColor(renderer, 7, 0x001F),
		        "HGX4 A9 horizontal flip is applied in logical native coordinates");
		STORE_16(0, 0x7C50, rom);
	}
	STORE_32(12, 28, set); h4Draw(renderer, 2, 0, true);
	require(renderer->row[0] == FLAG_UNWRITTEN, "HGX4 A9 rejects unsupported tile side");
	STORE_32(16, 28, set); STORE_32(255, 36, set); h4Draw(renderer, 2, 0, true);
	require(renderer->row[0] == FLAG_UNWRITTEN, "HGX4 A9 rejects inconsistent resource byte count");
	STORE_32(256, 36, set); STORE_16(1, 0x7C50, rom); h4Draw(renderer, 2, 0, true);
	require(renderer->row[0] == FLAG_UNWRITTEN, "HGX4 A9 rejects tile indices outside tileCount");
	STORE_16(0, 0x7C50, rom); set[3] = 2; h4Draw(renderer, 2, 0, true);
	require(renderer->row[0] == FLAG_UNWRITTEN, "HGX4 A9 rejects unknown descriptor versions");
	set[3] = 1; set[0] = 1; h4Draw(renderer, 2, 0, true);
	require(renderer->row[0] == FLAG_UNWRITTEN, "HGX4 A9 version one rejects unsupported compression values");
#ifdef USE_ZLIB
	uint8_t raw[256];
	for (unsigned y = 0; y < 16; ++y) {
		memset(raw + y * 16, 15, 8); memset(raw + y * 16 + 8, 16, 8);
	}
	unsigned containerBytes = hgzFixture(rom + 0xC100, raw, sizeof(raw));
	set[0] = 2; set[3] = 1; STORE_32(16, 28, set); STORE_32(1, 32, set); STORE_32(256, 36, set);
	STORE_32(0x0800C100, 4, set); h4Draw(renderer, 2, 0, true);
	require(isColor(renderer, 0, 0x001F) && isColor(renderer, 7, 0x03E0),
	        "HGX4 A9 HGZ1 output is pixel-equivalent to raw row-major input");
	memcpy(rom + 0xC200, rom + 0xC100, containerBytes); STORE_32(0, 0xC200, rom);
	STORE_32(0x0800C200, 4, set); h4Draw(renderer, 2, 0, true);
	require(renderer->row[0] == FLAG_UNWRITTEN, "HGX4 A9 HGZ1 rejects bad magic");
	memcpy(rom + 0xC300, rom + 0xC100, containerBytes); STORE_32(2, 0xC304, rom);
	STORE_32(0x0800C300, 4, set); h4Draw(renderer, 2, 0, true);
	require(renderer->row[0] == FLAG_UNWRITTEN, "HGX4 A9 HGZ1 rejects unknown codec");
	memcpy(rom + 0xC400, rom + 0xC100, containerBytes); STORE_32(255, 0xC408, rom);
	STORE_32(0x0800C400, 4, set); h4Draw(renderer, 2, 0, true);
	require(renderer->row[0] == FLAG_UNWRITTEN, "HGX4 A9 HGZ1 rejects raw size mismatch");
	uint32_t packedBytes, checksum; LOAD_32(packedBytes, 0xC10C, rom); LOAD_32(checksum, 0xC110, rom);
	memcpy(rom + 0xC500, rom + 0xC100, containerBytes); STORE_32(packedBytes - 1, 0xC50C, rom);
	STORE_32(0x0800C500, 4, set); h4Draw(renderer, 2, 0, true);
	require(renderer->row[0] == FLAG_UNWRITTEN, "HGX4 A9 HGZ1 rejects truncated zlib payload");
	memcpy(rom + 0xC600, rom + 0xC100, containerBytes); rom[0xC600 + containerBytes] = 0;
	STORE_32(packedBytes + 1, 0xC60C, rom);
	STORE_32(0x0800C600, 4, set); h4Draw(renderer, 2, 0, true);
	require(renderer->row[0] == FLAG_UNWRITTEN, "HGX4 A9 HGZ1 rejects trailing payload bytes");
	memcpy(rom + 0xC700, rom + 0xC100, containerBytes); STORE_32(checksum ^ 1, 0xC710, rom);
	STORE_32(0x0800C700, 4, set); h4Draw(renderer, 2, 0, true);
	require(renderer->row[0] == FLAG_UNWRITTEN, "HGX4 A9 HGZ1 rejects CRC32 mismatch");
	memcpy(rom + 0xC800, rom + 0xC100, containerBytes); rom[0xC814] ^= 0x80;
	STORE_32(0x0800C800, 4, set); h4Draw(renderer, 2, 0, true);
	require(renderer->row[0] == FLAG_UNWRITTEN, "HGX4 A9 HGZ1 rejects mutated deflate data");
	/* A9 v2 adds a bounded night palette and a 68-byte scene night mix while
	 * preserving v1 tile layout and native screen geometry. */
	set[0] = 0; set[2] = 0xA9; set[3] = 2; STORE_32(0x0800CB00, 4, set);
	STORE_32(16, 28, set); STORE_32(1, 32, set); STORE_32(256, 36, set);
	STORE_32(0x08007F00, 40, set); STORE_32(17, 44, set); STORE_16(0, 0x7C50, rom);
	for (unsigned y = 0; y < 16; ++y) {
		memset(rom + 0xCB00 + y * 16, 15, 8); memset(rom + 0xCB00 + y * 16 + 8, 16, 8);
	}
	memset(rom + 0x7F00, 0, 512); STORE_16(0x7C00, 0x7F1E, rom); STORE_16(0x7C1F, 0x7F20, rom);
	STORE_32(68, 8, control); STORE_32(0, 64, scene); h4Draw(renderer, 2, 0, true);
	require(isColor(renderer, 0, 0x001F) && isColor(renderer, 7, 0x03E0), "HGX4 A9 v2 nightMix zero is exact day palette");
	STORE_32(16, 64, scene); h4Draw(renderer, 2, 0, true);
	require(isColor(renderer, 0, 0x7C00) && isColor(renderer, 7, 0x7C1F), "HGX4 A9 v2 nightMix sixteen is exact night palette");
	hgzFixture(rom + 0xCC00, raw, sizeof(raw)); set[0] = 2; STORE_32(0x0800CC00, 4, set);
	h4Draw(renderer, 2, 0, true);
	require(isColor(renderer, 0, 0x7C00) && isColor(renderer, 7, 0x7C1F), "HGX4 A9 v2 HGZ1 preserves night palette indices");
	/* Metadata uses a tagged container base plus a separately validated slice.
	 * Cell id one exercises nonzero metatile and attribute offsets. */
	uint8_t metadataRefs[32] = { 0 }, metadataAttrs[8] = { 0 };
	memcpy(metadataRefs + 16, rom + 0x7C50, 16);
	memcpy(metadataAttrs + 4, rom + 0x7C60, 4);
	hgzFixture(rom + 0xF000, rom + 0x7D00, 512);
	hgzFixture(rom + 0xF100, rom + 0x7F00, 512);
	hgzFixture(rom + 0xF200, metadataRefs, sizeof(metadataRefs));
	hgzFixture(rom + 0xF280, metadataAttrs, sizeof(metadataAttrs));
	STORE_32(0x0800F001, 8, set); STORE_32(0x0800F201, 12, set);
	STORE_32(0x0800F281, 20, set); STORE_32(0x0800F101, 40, set);
	STORE_32(1, 4, cells); STORE_32(16, 64, scene); h4Draw(renderer, 2, 0, true);
	require(isColor(renderer, 0, 0x7C00) && isColor(renderer, 7, 0x7C1F),
	        "HGX4 tagged HGZ1 material palette, night palette, metatile and attrs match raw metadata");
	STORE_32(0x0800F003, 8, set); h4Draw(renderer, 2, 0, true);
	require(renderer->row[0] == FLAG_UNWRITTEN, "HGX4 metadata tag rejects reserved pointer bit one");
	STORE_32(0x08007D01, 8, set); h4Draw(renderer, 2, 0, true);
	require(renderer->row[0] == FLAG_UNWRITTEN, "HGX4 tagged raw material pointer is rejected instead of masking the tag");
	uint8_t shortRefs[16] = { 0 }; hgzFixture(rom + 0xF300, shortRefs, sizeof(shortRefs));
	STORE_32(0x0800F001, 8, set); STORE_32(0x0800F301, 12, set); h4Draw(renderer, 2, 0, true);
	require(renderer->row[0] == FLAG_UNWRITTEN, "HGX4 tagged metatile rejects offset plus length beyond decoded bytes");
	uint8_t shortPalette[32] = { 0 }; hgzFixture(rom + 0xF380, shortPalette, sizeof(shortPalette));
	STORE_32(0x0800F201, 12, set); STORE_32(0x0800F381, 40, set); h4Draw(renderer, 2, 0, true);
	require(renderer->row[0] == FLAG_UNWRITTEN, "HGX4 tagged night palette validates color count against decoded bytes");
	STORE_32(0, 4, cells); STORE_32(0x08007D00, 8, set); STORE_32(0x08007C50, 12, set);
	STORE_32(0x08007C60, 20, set); STORE_32(0x08007F00, 40, set);
	set[0] = 0; STORE_32(0x0800CB00, 4, set);
	STORE_32(8, 64, scene); h4Draw(renderer, 2, 0, true);
	require(isColor(renderer, 0, (15 << 10) | 15), "HGX4 A9 v2 interpolates day and night in five-bit channels");
	STORE_32(16, 32, scene); STORE_32(0x03E0, 36, scene); STORE_32(1, 40, scene);
	STORE_32(0, 64, scene); h4Draw(renderer, 2, 0, true);
	require(isColor(renderer, 0, 0x001F), "HGX4 A9 v2 day-night palette bypasses legacy DNS tint");
	STORE_32(0, 32, scene); STORE_32(0, 40, scene);
	STORE_32(64, 8, control); STORE_32(16, 64, scene); h4Draw(renderer, 2, 0, true);
	require(isColor(renderer, 0, 0x001F), "HGX4 64-byte scene defaults nightMix to day for A9 v2");
	STORE_32(68, 8, control); STORE_32(17, 64, scene); h4Draw(renderer, 2, 0, true);
	require(renderer->row[0] == FLAG_UNWRITTEN, "HGX4 rejects 68-byte scene nightMix above sixteen");
	STORE_32(0, 64, scene); STORE_32(0, 44, set); h4Draw(renderer, 2, 0, true);
	require(renderer->row[0] == FLAG_UNWRITTEN, "HGX4 A9 v2 rejects zero palette count");
	STORE_32(257, 44, set); h4Draw(renderer, 2, 0, true);
	require(renderer->row[0] == FLAG_UNWRITTEN, "HGX4 A9 v2 rejects palette count above 256");
	STORE_32(17, 44, set); STORE_32(0x0800FFF0, 40, set); h4Draw(renderer, 2, 0, true);
	require(renderer->row[0] == FLAG_UNWRITTEN, "HGX4 A9 v2 rejects inaccessible night palette");
	STORE_32(0x08007F00, 40, set); STORE_32(0x0800FFF0, 8, set); h4Draw(renderer, 2, 0, true);
	require(renderer->row[0] == FLAG_UNWRITTEN, "HGX4 A9 v2 rejects inaccessible day palette");
	STORE_32(0x08007D00, 8, set); STORE_32(16, 44, set); h4Draw(renderer, 2, 0, true);
	require(renderer->row[0] == FLAG_UNWRITTEN, "HGX4 A9 v2 rejects tile indices outside palette count");
	STORE_32(17, 44, set); for (unsigned y = 0; y < 16; ++y) { memset(rom + 0xCB00 + y * 16, 0, 8); }
	h4Draw(renderer, 2, 0, true);
	require(renderer->row[0] == FLAG_UNWRITTEN && isColor(renderer, 7, 0x03E0), "HGX4 A9 v2 keeps palette index zero transparent");
	STORE_32(64, 8, control); STORE_32(0, 64, scene);
	memset(raw, 15, 64); raw[0] = 0; raw[1] = 16;
	hgzFixture(rom + 0xC900, raw, 64);
	set[0] = 2; set[2] = 0xA8; set[3] = 1; STORE_32(0x0800C900, 4, set);
	STORE_16(0, 0x7C50, rom); h4Draw(renderer, 2, 0, true);
	require(renderer->row[0] == FLAG_UNWRITTEN && isColor(renderer, 1, 0x03E0),
	        "HGX4 A8 HGZ1 matches raw 64-byte indexed tile semantics");
	uint8_t raw4[32]; memset(raw4, 0x11, sizeof(raw4));
	hgzFixture(rom + 0xCA00, raw4, sizeof(raw4));
	set[0] = 2; set[2] = 0; set[3] = 0; STORE_32(0x0800CA00, 4, set);
	STORE_32(0x08007D00, 8, set); STORE_16(0x001F, 0x7D02, rom);
	STORE_16(0, 0x7C50, rom); h4Draw(renderer, 2, 0, true);
	require(isColor(renderer, 0, 0x001F), "HGX4 legacy four-bit HGZ1 matches raw atlas pixels and palette banks");
	uint8_t legacyPalette[512]; memcpy(legacyPalette, rom + 0x7D00, sizeof(legacyPalette));
	STORE_16(0x7C1F, 34, legacyPalette); hgzFixture(rom + 0xF400, legacyPalette, sizeof(legacyPalette));
	STORE_32(0x0800F401, 8, set); STORE_16(0x1000, 0x7C50, rom); h4Draw(renderer, 2, 0, true);
	require(isColor(renderer, 0, 0x7C1F), "HGX4 tagged HGZ1 legacy palette preserves bank offset semantics");
	uint8_t oneBank[32] = { 0 }; hgzFixture(rom + 0xF500, oneBank, sizeof(oneBank));
	STORE_32(0x0800F501, 8, set); h4Draw(renderer, 2, 0, true);
	require(renderer->row[0] == FLAG_UNWRITTEN, "HGX4 tagged legacy palette rejects a bank beyond decoded bytes");
	STORE_32(0x08007D00, 8, set); STORE_16(0, 0x7C50, rom);
	STORE_16(1, 0x7C50, rom); h4Draw(renderer, 2, 0, true);
	require(renderer->row[0] == FLAG_UNWRITTEN, "HGX4 legacy HGZ1 rejects tile references beyond unpacked atlas");
	STORE_16(0, 0x7C50, rom);
	/* More than the former 32-entry cache limit must coexist in one immutable
	 * scene without turning later cells transparent. */
	memset(cells, 0, 3072); memset(raw, 15, 64);
	for (unsigned n = 0; n < 40; ++n) {
		unsigned layoutAddress = 0xD000 + n * 24;
		unsigned setAddress = 0xD400 + n * 40;
		unsigned refsAddress = 0xDB00 + n * 16;
		unsigned attrsAddress = 0xDE00 + n * 4;
		unsigned containerAddress = 0xE000 + n * 64;
		memset(rom + layoutAddress, 0, 24); memset(rom + setAddress, 0, 40);
		STORE_32(0x08000000 + setAddress, layoutAddress + 16, rom);
		STORE_32(0x08000000 + setAddress, layoutAddress + 20, rom);
		rom[setAddress] = 2; rom[setAddress + 2] = 0xA9; rom[setAddress + 3] = 1;
		STORE_32(0x08000000 + containerAddress, setAddress + 4, rom);
		STORE_32(0x08007D00, setAddress + 8, rom); STORE_32(0x08000000 + refsAddress, setAddress + 12, rom);
		STORE_32(0x08000000 + attrsAddress, setAddress + 20, rom); STORE_32(0x08007D00, setAddress + 24, rom);
		STORE_32(8, setAddress + 28, rom); STORE_32(1, setAddress + 32, rom); STORE_32(64, setAddress + 36, rom);
		memset(rom + refsAddress, 0, 16); STORE_32(0, attrsAddress, rom);
		hgzFixture(rom + containerAddress, raw, 64);
		STORE_32(0x08000000 + layoutAddress, n * 16, cells);
	}
	h4Draw(renderer, 2, 0, true);
	require(isColor(renderer, 0, 0x001F) && isColor(renderer, 14 * 16, 0x001F),
	        "HGX4 A9 retains more than 32 compressed resources in one frame");
#endif
	STORE_32(0, 0, control); GBAVideoSoftwareRendererLatchHGX4(renderer);
}

static void paletteCapacityTest(struct mCore* core) {
	struct GBA* gba = core->board;
	struct GBAVideoSoftwareRenderer* renderer = (struct GBAVideoSoftwareRenderer*) gba->video.renderer;
	uint8_t* rom = (uint8_t*) gba->memory.rom; uint8_t* ram = (uint8_t*) gba->memory.wram;
	uint8_t* control = ram + 0x30000; uint8_t* scene = ram + 0x30040; uint8_t* cells = ram + 0x30100;
	memset(control, 0, 32); memset(scene, 0, 64); memset(cells, 0, 3072);
	STORE_32(0x34584748, 0, control); STORE_32(32, 4, control); STORE_32(64, 8, control); STORE_32(16, 12, control);
	STORE_32(0x02030040, 16, control); STORE_32(14, 4, scene); STORE_32(0x02030100, 8, scene);
	STORE_32(16, 12, scene); STORE_32(12, 16, scene);
	/* 192 owner layouts, each with different primary/secondary palettes.
	 * Their complete native palette arrays may overlap; each used global bank
	 * is a distinct, valid 32-byte ROM slice. All 384 commands are resident. */
	for (unsigned cell = 0; cell < 192; ++cell) {
		unsigned layout = 0xB200 + cell * 24; memset(rom + layout, 0, 24);
		for (unsigned role = 0; role < 2; ++role) {
			unsigned set = 0xC400 + (cell * 2 + role) * 24;
			unsigned palette = 0x8000 + (cell * 2 + role) * 32;
			memset(rom + set, 0, 24); memset(rom + palette, 0, 32);
			STORE_32(0x08000000 + set, layout + 16 + role * 4, rom);
			STORE_32(0x0800E820, set + 4, rom);
			STORE_32(0x08000000 + palette - role * 7 * 32, set + 8, rom);
			STORE_32(0x0800E800, set + 12, rom); STORE_32(0x0800E810, set + 20, rom);
			STORE_16((role ? 0x4000 : 0) + cell + 1, palette + 2, rom);
		}
		STORE_32(0x08000000 + layout, cell * 16, cells);
	}
	for (unsigned q = 0; q < 4; ++q) { STORE_16(0, 0xE800 + q * 2, rom); STORE_16(0x7280, 0xE808 + q * 2, rom); }
	STORE_32(0, 0xE810, rom); memset(rom + 0xE820, 0x11, 32);
	GBAIOWrite32(gba, 0x3E0, 0x02030000); obj256Setup(renderer);
	renderer->bg[1].flags = FLAG_IS_BACKGROUND; renderer->bg[2].flags = FLAG_IS_BACKGROUND;
	bool seen[192][2] = { { false } }; bool correct = true;
	/* Four camera positions expose the whole retained 16x12 cell scene through
	 * the native 240x160 viewport, including both late-allocated palette roles. */
	for (unsigned view = 0; view < 4; ++view) {
		unsigned firstColumn = view & 1, firstRow = (view >> 1) * 2;
		STORE_32((uint32_t) -(int32_t) (firstColumn * 16), 20, scene);
		STORE_32((uint32_t) -(int32_t) (firstRow * 16), 24, scene);
		GBAVideoSoftwareRendererLatchHGX4(renderer);
		for (unsigned role = 0; role < 2; ++role) {
			for (unsigned row = firstRow; row < firstRow + 10; ++row) {
				h4Draw(renderer, role ? 1 : 2, (row - firstRow) * 16, false);
				for (unsigned col = firstColumn; col < firstColumn + 15; ++col) {
					unsigned cell = row * 16 + col;
					correct &= isColor(renderer, (col - firstColumn) * 16, (role ? 0x4000 : 0) + cell + 1);
					seen[cell][role] = true;
				}
			}
		}
	}
	for (unsigned cell = 0; cell < 192; ++cell) { correct &= seen[cell][0] && seen[cell][1]; }
	require(correct, "HGX4 one retained scene preserves all 384 distinct primary and secondary palettes across camera views");
}

int main(void) {
    uint8_t* rom = calloc(1, 0x4000);
    require(rom != NULL, "allocate artificial ROM");
    STORE_32(0xEAFFFFFE, 0, rom); /* ARM infinite loop */
    memcpy(rom + 0xA0, "HGX SYNTH   ", 12);
    rom[0xB2] = 0x96;
    memcpy(rom + 0xB5, "HGX", 3);
    STORE_32(0x040003E0, 0xB8, rom);
    memset(rom + 0x2000, 0x11, 32);
    rom[0x2000] = 0x21; rom[0x2001] = 0x10; rom[0x2003] = 0x32; rom[0x201C] = 0x43;
    STORE_16(0x001F, 0x2102, rom); /* palette1 red */
    STORE_16(0x7C00, 0x2104, rom); /* palette2 blue */
    STORE_16(0x03FF, 0x2106, rom); /* palette3 yellow */
    STORE_16(0x7FFF, 0x2108, rom); /* palette4 white */
    STORE_32(0x00002010, 0x2200, rom); /* 32-byte LZ with literal groups */
    for (unsigned g = 0; g < 4; ++g) {
        rom[0x2204 + g * 9] = 0;
        memcpy(rom + 0x2205 + g * 9, rom + 0x2000 + g * 8, 8);
    }
    STORE_32(0x00002010, 0x2300, rom); /* invalid early back-reference */
    rom[0x2304] = 0x80; rom[0x2305] = 0xF0;
    STORE_32(0x00002010, 0x2400, rom);
    rom[0x2404] = 0x60; rom[0x2405] = 0x11; /* literal then overlapping copies */
    rom[0x2406] = 0xF0; rom[0x2407] = 0;
    rom[0x2408] = 0xA0; rom[0x2409] = 0;
    memset(rom + 0x2500, 0x22, 32); /* atlas B: same local index, different tile */
    memset(rom + 0x2800, 0x44, 32); /* secondary atlas C */
    STORE_16(0x03FF, 0x2602, rom);
    STORE_16(0x7FFF, 0x2604, rom);
    STORE_16(0x7FFF, 0x2784, rom);
    STORE_16(0x7FFF, 0x27A4, rom);
    STORE_16(0x7C00, 0x2908, rom);
    STORE_16(0x7FE0, 0x2B0A, rom);
    STORE_16(0x7C1F, 0x2D0C, rom);
    for (unsigned resource = 0; resource < 2; ++resource) {
        unsigned start = 0x2E00 + resource * 0x100;
        STORE_32(0x00002010, start, rom);
        for (unsigned group = 0; group < 4; ++group) {
            rom[start + 4 + group * 9] = 0;
            memset(rom + start + 5 + group * 9, resource ? 0x66 : 0x55, 8);
        }
    }
    rom[0xBD] = (uint8_t) -0x19;
    for (unsigned i = 0xA0; i <= 0xBC; ++i) { rom[0xBD] -= rom[i]; }
    struct mCore* core = GBACoreCreate();
    require(core && core->init(core), "create real GBA core");
    mCoreInitConfig(core, NULL);
    mCoreConfigSetIntValue(&core->config, "threadedVideo", 1);
    mColor* output = calloc(240 * 160, sizeof(*output));
    require(output != NULL, "allocate video output");
    core->setVideoBuffer(core, output, 240);
    require(core->loadROM(core, VFileMemChunk(rom, 0x4000)), "load artificial ROM");
    free(rom);
    core->reset(core);
    struct GBA* gba = core->board;
    require(!core->supportsFeature(core, mCORE_FEATURE_OPENGL), "HGX advertises software-only rendering to GUI frontends");
    struct GBAVideoSoftwareRenderer* renderer = (struct GBAVideoSoftwareRenderer*) gba->video.renderer;
    require(renderer->d.p == gba && renderer->d.rendererId(&renderer->d) == 0x6E727773, "HGX forces direct software renderer despite threadedVideo=1");
    uint8_t* ram = (uint8_t*) gba->memory.wram;
    STORE_32(0x32584748, 0, ram);
    STORE_32(1, 4, ram);
    STORE_32(0x02001000, 8, ram);
    GBAIOWrite32(gba, 0x3E0, 0x02000000);
    require(GBAIORead(gba, 0x3E0) == 0 && GBAIORead(gba, 0x3E2) == 0x0200, "MMIO 32-bit write / 16-bit reads");
    GBAIOWrite(gba, GBA_REG_BG1HOFS, 0x0123);
    GBAIOWrite(gba, GBA_REG_BG1VOFS, 0x0145);
    require(GBAIORead(gba, 0x3E8) == 0x0123, "HGX horizontal viewport mirror reads BG1HOFS");
    require(GBAIORead(gba, 0x3EA) == 0x0145, "HGX vertical viewport mirror reads BG1VOFS");
    struct GBAVideoSoftwareBackground* bg = &renderer->bg[0];
    bg->size = 0; bg->multipalette = 0; bg->screenBase = 0x800;
    bg->charBase = 0; bg->x = 0; bg->y = 0;
    bg->offsetX = 0; bg->offsetY = 0; bg->mosaic = 0;
    bg->flags = FLAG_IS_BACKGROUND; bg->objwinFlags = FLAG_IS_BACKGROUND;
    bg->variant = false; bg->yCache = -1;
    memset(gba->video.vram, 0, 0x18000);
    memset(gba->video.vram, 0x11, 32);
    renderer->normalPalette[1] = mColorFrom555(0x03E0); /* native green */
    entry(ram, 0, 0x08002000, 0x08002100, 0);
    entry(ram, 1, 0x08002001, 0x08002100, 0);
    entry(ram, 2, 0x08002002, 0x08002100, 0);
    draw(renderer, 0);
    require(isColor(renderer, 0, 0x001F) && isColor(renderer, 1, 0x7C00), "arbitrary raw ROM tile and palette");
    require(renderer->row[2] == FLAG_UNWRITTEN, "HGX pixel zero preserves transparency");
    require(isColor(renderer, 8, 0x03FF), "horizontal flip");
    require(isColor(renderer, 16, 0x03FF) && isColor(renderer, 17, 0x7FFF), "vertical flip");
    require(isColor(renderer, 24, 0x03E0), "tile zero falls back to native VRAM");
    memset(ram + 0x600, 0x22, 32);
    entry(ram, 0, 0x02000600, 0x08002100, 0);
    draw(renderer, 0);
    require(isColor(renderer, 0, 0x7C00), "dynamic EWRAM tile resource");
    STORE_16(0x7C00, 0x502, ram);
    entry(ram, 0, 0x08002000, 0x02000500, 0);
    draw(renderer, 0);
    require(isColor(renderer, 0, 0x7C00), "dynamic EWRAM palette resource");
    entry(ram, 0, 0x0BFFFFFC, 0x08002100, 0);
    draw(renderer, 0);
    require(isColor(renderer, 0, 0x03E0), "out-of-range tile safely falls back");
    entry(ram, 0, 0x08002000, 0x0203FFF0, 0);
    draw(renderer, 0);
    require(isColor(renderer, 0, 0x03E0), "palette crossing RAM limit safely falls back");
    entry(ram, 0, 0x08002200, 0x08002100, 0x80000000);
    draw(renderer, 0);
    require(isColor(renderer, 0, 0x001F) && isColor(renderer, 1, 0x7C00), "LZ native compressed tileset resource");
    entry(ram, 0, 0x08002400, 0x08002100, 0x80000000);
    draw(renderer, 0);
    require(isColor(renderer, 0, 0x001F) && isColor(renderer, 7, 0x001F), "LZ overlapping back-reference expansion");
    entry(ram, 0, 0x08002300, 0x08002100, 0x80000000);
    draw(renderer, 0);
    require(isColor(renderer, 0, 0x03E0), "malformed LZ back-reference safely falls back");
    entry(ram, 0, 0x08002200, 0x08002100, 0x80000001);
    draw(renderer, 0);
    require(isColor(renderer, 0, 0x03E0), "compressed tile index past decoded size safely falls back");
    entry(ram, 0, 0x08002000, 0x08002100, 0x40000400);
    draw(renderer, 0);
    require(isColor(renderer, 0, 0x03E0), "native screen edit invalidates stale metadata");
    entry(ram, 0, 0x08002000, 0x08002100, 0x40000000);
    draw(renderer, 0);
    require(isColor(renderer, 0, 0x001F), "matching native screen reactivates HGX resource");
    entry(ram, 0, 0x08002000, 0x08002100, 0);
    STORE_32(0x00100000, 28, ram);
    draw(renderer, 0);
    require(isColor(renderer, 0, 0), "descriptor fade coefficient 16 produces black");
    STORE_32(0x00080000, 28, ram);
    draw(renderer, 0);
    require(isColor(renderer, 0, 0x000F), "game palette fade uses exact 5-bit quantization");
    STORE_32(0, 28, ram);
    renderer->blendEffect = BLEND_BRIGHTEN; renderer->bldy = 16; bg->variant = true;
    draw(renderer, 0);
    require(isColor(renderer, 0, 0x7FFF), "hardware brightness composition");
    renderer->blendEffect = BLEND_ALPHA; bg->variant = false;
    renderer->blda = 8; renderer->bldb = 8; bg->flags |= FLAG_TARGET_2;
    renderer->start = 0; renderer->end = 1;
    renderer->row[0] = FLAG_TARGET_1 | mColorFrom555(0x7C00);
    GBAVideoSoftwareRendererDrawBackgroundMode0(renderer, bg, 0);
    #ifdef COLOR_16_BIT
    require(isColor(renderer, 0, 0x3C0F), "alpha blends existing blue target with HGX red");
#else
    require((renderer->row[0] & 0xFFFFFF) == 0x7F007F, "alpha blends existing blue target with HGX red");
#endif
    bg->flags = FLAG_IS_BACKGROUND;
    renderer->blendEffect = BLEND_NONE;
    /* HGX3 keeps every source entry intact and changes only an atomic atlas
     * binding. Current resources late-bind independently to native RAM. */
    STORE_32(0x33584748, 0, ram);
    STORE_32(48, 36, ram); STORE_32(12, 40, ram);
    STORE_32(0x02000080, 32, ram);
    STORE_32(0x08002000, 0x80, ram); STORE_32(0x08002800, 0x84, ram);
    STORE_32(0x08002100, 0x88, ram); STORE_32(0x08002600, 0x8C, ram);
    STORE_32(0x08002500, 0x90, ram); STORE_32(0x08002800, 0x94, ram);
    STORE_32(0x08002600, 0x98, ram); STORE_32(0x08002600, 0x9C, ram);
    entry(ram, 0, 0x08002000, 0x08002100, 0x40000000);
    entry(ram, 1, 0x08002500, 0x08002600, 0x40000000);
    uint8_t sourceSnapshot[24]; memcpy(sourceSnapshot, ram + 0x1000, 24);
    memset(gba->video.vram, 0x33, 32);
    renderer->normalPalette[3] = mColorFrom555(0x03E0);
    draw(renderer, 0);
    require(isColor(renderer, 0, 0x03E0), "HGX3 current atlas A uses live VRAM and DNS palette");
    require(isColor(renderer, 8, 0x7FFF), "HGX3 same tile index in foreign atlas B remains ROM-resident");
    STORE_32(0x02000090, 32, ram);
    memset(gba->video.vram, 0x44, 32);
    renderer->normalPalette[4] = mColorFrom555(0x7C00);
    draw(renderer, 0);
    require(isColor(renderer, 0, 0x001F) && isColor(renderer, 8, 0x7C00),
            "HGX3 A to B switches ownership without rewriting either entry");
    STORE_32(0x02000080, 32, ram);
    memset(gba->video.vram, 0x33, 32);
    draw(renderer, 0);
    require(isColor(renderer, 0, 0x03E0) && isColor(renderer, 8, 0x7FFF)
            && memcmp(sourceSnapshot, ram + 0x1000, 24) == 0,
            "HGX3 A to B to A keeps resident sources and restores native ownership");
    memset(gba->video.vram, 0x44, 32);
    renderer->normalPalette[4] = mColorFrom555(0x7FFF);
    draw(renderer, 0);
    require(isColor(renderer, 0, 0x7FFF), "HGX3 current tile animation follows VRAM writes");
    renderer->normalPalette[4] = mColorFrom555(0x7C1F);
    draw(renderer, 0);
    require(isColor(renderer, 0, 0x7C1F), "HGX3 current DNS and weather follow palette updates");
    memset(gba->video.vram, 0x33, 32);
    STORE_32(0x08002600, 0x88, ram); /* tile A still current, palette A foreign */
    renderer->normalPalette[2] = mColorFrom555(0x7C1F);
    draw(renderer, 0);
    require(isColor(renderer, 0, 0x03FF), "HGX3 current tile and foreign palette bind independently");
    require(isColor(renderer, 8, 0x7C1F), "HGX3 foreign tile and current palette bind independently");
    STORE_32(0x08002100, 0x88, ram);
    entry(ram, 0, 0x08002200, 0x08002100, 0xC0000000);
    STORE_32(0x08002200, 0x80, ram);
    draw(renderer, 0);
    require(isColor(renderer, 0, 0x03E0), "HGX3 compressed current atlas late-binds to VRAM");
    STORE_32(0x08002000, 0x80, ram);
    STORE_32(0x08002600, 0x88, ram);
    draw(renderer, 0);
    require(isColor(renderer, 0, 0x001F), "HGX3 compressed foreign atlas decodes resident ROM source");
    STORE_32(0x08002100, 0x88, ram);
    entry(ram, 0, 0x08002800, 0x08002100, 0x40000000 | (640U << 10));
    STORE_16(640, 0x800, gba->video.vram);
    memset((uint8_t*) gba->video.vram + 640 * 32, 0x11, 32);
    renderer->normalPalette[1] = mColorFrom555(0x03E0);
    draw(renderer, 0);
    require(isColor(renderer, 0, 0x03E0), "HGX3 secondary tile role uses its native 640-tile offset");
    STORE_16(0x7000, 0x800, gba->video.vram);
    entry(ram, 0, 0x08002000, 0x080026E0, 0x40000000 | (0x7000U << 10));
    renderer->normalPalette[7 * 16 + 3] = mColorFrom555(0x7FE0);
    draw(renderer, 0);
    require(isColor(renderer, 0, 0x7FE0), "HGX3 primary tile and secondary palette roles bind independently");
    STORE_16(0x0400, 0x800, gba->video.vram);
    entry(ram, 0, 0x08002001, 0x08002100, 0x40000000 | (0x0400U << 10));
    ((uint8_t*) gba->video.vram)[3] = 0x43;
    renderer->normalPalette[4] = mColorFrom555(0x7C00);
    draw(renderer, 0);
    require(isColor(renderer, 0, 0x7C00), "HGX3 live native tile retains horizontal flip");
    STORE_16(1, 0x800, gba->video.vram);
    memset((uint8_t*) gba->video.vram + 32, 0x22, 32);
    renderer->normalPalette[2] = mColorFrom555(0x7C00);
    draw(renderer, 0);
    require(isColor(renderer, 0, 0x7C00), "HGX3 door screen edits bypass stale source metadata");
    STORE_16(0, 0x800, gba->video.vram);
    memset(gba->video.vram, 0x33, 32);
    entry(ram, 0, 0x08002000, 0x08002100, 0x40000000);
    STORE_32(0x00080000, 28, ram);
    draw(renderer, 0);
    require(isColor(renderer, 0, 0x03E0) && isColor(renderer, 8, 0x3DEF),
            "HGX3 resident palette fades while current native palette avoids double fade");
    STORE_32(0, 28, ram);
    /* Game-verified night DNS: coefficient 3, BGR555 color 19464, banks 0..12.
     * The foreign white result is 5-bit RGB (26,25,28), then ordinary fade. */
    STORE_32(0x020000A0, 44, ram);
    STORE_32(3, 0xA0, ram); STORE_32(19464, 0xA4, ram);
    STORE_32(0x1FFF, 0xA8, ram); STORE_32(0x02000080, 0xAC, ram);
    STORE_32(0x02000090, 32, ram); /* env binding must override this future binding */
    draw(renderer, 0);
    require(isColor(renderer, 0, 0x03E0) && isColor(renderer, 8, 0x733A),
            "HGX3 foreign DNS matches game 5-bit night tint without retinting current palette");
    STORE_32(0x02000090, 0xAC, ram);
    draw(renderer, 0);
    require(isColor(renderer, 0, 0x0C1A) && isColor(renderer, 8, 0x03E0),
            "HGX3 DNS uses exact positive and negative 5-bit channel interpolation");
    STORE_32(0x02000080, 0xAC, ram);
    STORE_32(0x00080000, 28, ram);
    draw(renderer, 0);
    require(isColor(renderer, 0, 0x03E0) && isColor(renderer, 8, 0x398D),
            "HGX3 foreign palette applies DNS before fade without double fading current palette");
    STORE_32(0, 28, ram);
    STORE_32(0, 0xA0, ram); STORE_32(0x02000090, 0xAC, ram);
    drawSameFrame(renderer, 1);
    require(isColor(renderer, 0, 0x03E0) && isColor(renderer, 8, 0x733A),
            "HGX3 DNS values and binding remain the same epoch throughout a frame");
    draw(renderer, 0);
    require(isColor(renderer, 0, 0x001F) && isColor(renderer, 8, 0x03E0),
            "HGX3 next frame publishes DNS environment and matching binding together");
    STORE_32(0x02000080, 32, ram);
    STORE_32(3, 0xA0, ram); STORE_32(0x02000080, 0xAC, ram);
    STORE_32(0x08002900, 0x8C, ram); /* secondary palette B remains foreign */
    STORE_16(0xC000, 0x802, gba->video.vram);
    entry(ram, 1, 0x08002500, 0x08002780, 0x40000000 | (0xC000U << 10));
    draw(renderer, 0);
    require(isColor(renderer, 8, 0x733A), "HGX3 DNS actual palette mask includes bank 12");
    STORE_16(0xD000, 0x802, gba->video.vram);
    entry(ram, 1, 0x08002500, 0x080027A0, 0x40000000 | (0xD000U << 10));
    draw(renderer, 0);
    require(isColor(renderer, 8, 0x7FFF), "HGX3 DNS palette mask excludes bank 13");
    STORE_16(0, 0x802, gba->video.vram);
    entry(ram, 1, 0x08002500, 0x02000500, 0x40000000);
    STORE_16(0, 0x504, ram);
    draw(renderer, 0);
    require(isColor(renderer, 8, 0) && renderer->row[8] != FLAG_UNWRITTEN,
            "HGX3 DNS preserves opaque black exactly like the game");
    entry(ram, 1, 0x08002500, 0x08002600, 0x40000000);
    STORE_32(0x08002600, 0x8C, ram);
    STORE_32(0, 0xA8, ram);
    draw(renderer, 0);
    require(isColor(renderer, 8, 0x7FFF), "HGX3 DNS zero palette mask leaves foreign colors unchanged");
    STORE_32(0x1FFF, 0xA8, ram); STORE_32(16, 0xA0, ram);
    draw(renderer, 0);
    require(isColor(renderer, 8, 0x4C08), "HGX3 DNS coefficient 16 reaches the exact tint color");
    STORE_32(17, 0xA0, ram); STORE_32(0x02000090, 0xAC, ram);
    draw(renderer, 0);
    require(isColor(renderer, 0, 0x03E0) && isColor(renderer, 8, 0x7FFF),
            "HGX3 invalid DNS coefficient safely retains legacy binding and original colors");
    STORE_32(0x0203FFF8, 44, ram);
    draw(renderer, 0);
    require(isColor(renderer, 8, 0x7FFF), "HGX3 DNS pointer crossing RAM boundary safely disables tint");
    STORE_32(0, 44, ram);
    draw(renderer, 0);
    require(isColor(renderer, 0, 0x03E0) && isColor(renderer, 8, 0x7FFF),
            "HGX3 absent DNS environment preserves legacy binding behavior");
    STORE_32(0x0203FFF8, 32, ram);
    draw(renderer, 0);
    require(isColor(renderer, 0, 0x001F), "HGX3 invalid atomic binding safely uses resident source");
    STORE_32(0x02000080, 32, ram);
    STORE_32(16, 40, ram);
    bg->yCache = -1;
    draw(renderer, 0);
    require(isColor(renderer, 0, 0x03E0), "HGX3 unsupported entry stride safely uses native rendering");
    STORE_32(12, 40, ram);
    STORE_32(0x02000080, 32, ram);
    entry(ram, 1, 0x08002500, 0x08002600, 0x40000000);
    renderer->hgxFrameStarted = false; /* equivalent to reset/state restore */
    drawSameFrame(renderer, 5);
    require(isColor(renderer, 0, 0x03E0) && isColor(renderer, 8, 0x7FFF),
            "HGX3 first nonzero scanline lazily captures a coherent frame");
    STORE_32(0x02000090, 32, ram);
    entry(ram, 1, 0x08002000, 0x08002100, 0x40000000);
    drawSameFrame(renderer, 6);
    require(isColor(renderer, 0, 0x03E0) && isColor(renderer, 8, 0x7FFF),
            "HGX3 midframe binding and source entry writes remain staged");
    draw(renderer, 0);
    require(isColor(renderer, 0, 0x001F) && isColor(renderer, 8, 0x001F),
            "HGX3 next frame commits binding and source entries together");
    memcpy(ram + 0x200, ram, 48);
    GBAIOWrite32(gba, 0x3E0, 0x02000200);
    bg->yCache = -1;
    drawSameFrame(renderer, 1);
    require(isColor(renderer, 0, 0x03E0), "HGX3 descriptor pointer replacement invalidates old frame snapshot");
    draw(renderer, 0);
    require(isColor(renderer, 0, 0x001F), "HGX3 replacement descriptor activates on the next frame");
    GBAIOWrite32(gba, 0x3E0, 0x02000000);
    draw(renderer, 0);
    STORE_32(0, 4, ram);
    drawSameFrame(renderer, 1);
    STORE_32(1, 4, ram);
    drawSameFrame(renderer, 2);
    require(isColor(renderer, 0, 0x03E0), "HGX3 disable invalidates snapshot until next frame even after re-enable");
    draw(renderer, 0);
    require(isColor(renderer, 0, 0x001F), "HGX3 enabled descriptor resumes at the next frame boundary");
    STORE_32(0x02000080, 32, ram);
    GBAIOWrite(gba, GBA_REG_BG0CNT, 0x0100);
    gba->video.palette[3] = 0x03E0;
    renderer->d.writePalette(&renderer->d, 6, 0x03E0);
    draw(renderer, 0);
    void* atlasState = malloc(core->stateSize(core));
    require(atlasState && core->saveState(core, atlasState), "save HGX3 resident source state");
    STORE_32(0x02000090, 32, ram);
    draw(renderer, 0);
    require(core->loadState(core, atlasState) && !renderer->hgxFrameStarted,
            "HGX3 state restore discards host frame snapshot");
    bg->flags = FLAG_IS_BACKGROUND; bg->yCache = -1;
    drawSameFrame(renderer, 3);
    require(isColor(renderer, 0, 0x03E0), "HGX3 state restore rebuilds binding and source snapshot at nonzero line");
    free(atlasState);
    /* Current A plus four independently resident neighbors. Two neighbors
     * use compressed resources with the same local tile index. */
    entry(ram, 0, 0x08002000, 0x08002100, 0x40000000);
    entry(ram, 1, 0x08002500, 0x08002600, 0x40000000);
    entry(ram, 2, 0x08002800, 0x08002900, 0x40000000);
    entry(ram, 3, 0x08002E00, 0x08002B00, 0xC0000000);
    entry(ram, 4, 0x08002F00, 0x08002D00, 0xC0000000);
    uint8_t fiveSources[60]; memcpy(fiveSources, ram + 0x1000, sizeof(fiveSources));
    memset((uint8_t*) gba->video.vram + 0x800, 0, 10);
    memset(gba->video.vram, 0x22, 32);
    renderer->normalPalette[2] = mColorFrom555(0x4210);
    STORE_32(0x08002800, 0xC0, ram); STORE_32(0, 0xC4, ram);
    STORE_32(0x08002900, 0xC8, ram); STORE_32(0, 0xCC, ram);
    STORE_32(0x02000080, 32, ram);
    draw(renderer, 0);
    renderer->start = 32; renderer->end = 40;
    GBAVideoSoftwareRendererDrawBackgroundMode0(renderer, bg, 0);
    require(isColor(renderer, 0, 0x4210) && isColor(renderer, 8, 0x7FFF) && isColor(renderer, 16, 0x7C00)
            && isColor(renderer, 24, 0x7FE0) && isColor(renderer, 32, 0x7C1F),
            "HGX3 current A simultaneously renders four distinct raw and compressed neighbor atlases");
    STORE_32(0x02000090, 32, ram);
    draw(renderer, 0);
    renderer->start = 32; renderer->end = 40;
    GBAVideoSoftwareRendererDrawBackgroundMode0(renderer, bg, 0);
    require(isColor(renderer, 0, 0x001F) && isColor(renderer, 8, 0x4210) && isColor(renderer, 16, 0x7C00)
            && isColor(renderer, 24, 0x7FE0) && isColor(renderer, 32, 0x7C1F),
            "HGX3 current B changes ownership without discarding any of four other sources");
    STORE_32(0x020000C0, 32, ram);
    draw(renderer, 0);
    renderer->start = 32; renderer->end = 40;
    GBAVideoSoftwareRendererDrawBackgroundMode0(renderer, bg, 0);
    require(isColor(renderer, 0, 0x001F) && isColor(renderer, 8, 0x7FFF) && isColor(renderer, 16, 0x4210)
            && isColor(renderer, 24, 0x7FE0) && isColor(renderer, 32, 0x7C1F),
            "HGX3 A to B to C retains independent same-index neighbor resources");
    STORE_32(0x02000080, 32, ram);
    draw(renderer, 0);
    renderer->start = 32; renderer->end = 40;
    GBAVideoSoftwareRendererDrawBackgroundMode0(renderer, bg, 0);
    require(isColor(renderer, 0, 0x4210) && isColor(renderer, 8, 0x7FFF) && isColor(renderer, 16, 0x7C00)
            && isColor(renderer, 24, 0x7FE0) && isColor(renderer, 32, 0x7C1F)
            && memcmp(fiveSources, ram + 0x1000, sizeof(fiveSources)) == 0,
            "HGX3 returning C to A restores ownership with all five source entries unchanged");
    STORE_32(0x32584748, 0, ram); /* HGX2 compatibility fixture continues below. */
    memset(gba->video.vram, 0x11, 32);
    STORE_16(0, 0x800, gba->video.vram);
    renderer->normalPalette[1] = mColorFrom555(0x03E0);
    STORE_32(0, 4, ram);
    bg->yCache = -1;
    draw(renderer, 0);
    require(isColor(renderer, 0, 0x03E0), "disabled descriptor uses original native renderer");
    STORE_32(1, 4, ram);
    entry(ram, 0, 0x08002400, 0x08002100, 0x80000000);
    GBAIOWrite(gba, GBA_REG_BG0CNT, 0x0100);
    void* state = malloc(core->stateSize(core));
    require(state && core->saveState(core, state), "save synthetic state");
    core->reset(core); /* discard host decoded cache, as in a fresh launch */
    require(core->loadState(core, state), "load synthetic state");
    require(GBAIORead(gba, 0x3E2) == 0x0200, "MMIO descriptor pointer survives save/load");
    bg->flags = FLAG_IS_BACKGROUND; bg->yCache = -1;
    draw(renderer, 0);
    require(isColor(renderer, 0, 0x001F), "state reload rebuilds compressed resources without host IDs");
    free(state);
    /* Compare complete rendering, including windows, OBJ and blend dispatch,
     * with fallback metadata enabled versus the original BG implementation. */
    memset(ram + 0x1000, 0, 12288);
    gba->video.palette[1] = 0x03E0;
    renderer->d.writePalette(&renderer->d, 2, 0x03E0);
    for (unsigned n = 0; n < 4; ++n) {
        STORE_32(0x02001000, 8 + n * 4, ram);
        GBAIOWrite(gba, GBA_REG_BG0CNT + n * 2, 0x0100 | n);
        renderer->bg[n].enabled = 4;
    }
    GBAIOWrite(gba, GBA_REG_DISPCNT, 0x7F60);
    GBAIOWrite(gba, GBA_REG_WININ, 0x3F3F);
    GBAIOWrite(gba, GBA_REG_WINOUT, 0x3F3F);
    GBAIOWrite(gba, GBA_REG_BLDCNT, 0x1E40);
    GBAIOWrite(gba, GBA_REG_BLDALPHA, 0x0010);
    for (unsigned n = 0; n < 4; ++n) { renderer->bg[n].enabled = 4; }
    mColor* reference = malloc(240 * 160 * sizeof(*reference));
    require(reference != NULL, "allocate full-pipeline native reference");
    STORE_32(0, 4, ram);
    for (unsigned y = 0; y < 160; ++y) { renderer->d.drawScanline(&renderer->d, y); }
    memcpy(reference, output, 240 * 160 * sizeof(*reference));
    require(reference[100 * 240 + 100] == mColorFrom555(0x03E0), "native full-pipeline fixture draws visible green scene");
    STORE_32(15, 4, ram);
    for (unsigned y = 0; y < 160; ++y) { renderer->d.drawScanline(&renderer->d, y); }
    require(memcmp(reference, output, 240 * 160 * sizeof(*reference)) == 0,
            "full pipeline zero-metadata fallback equals native with windows OBJ and alpha");
    STORE_32(0x33584748, 0, ram);
    STORE_32(48, 36, ram); STORE_32(12, 40, ram);
    STORE_32(0x02000080, 32, ram);
    STORE_32(0x08002000, 0x80, ram); STORE_32(0x08002800, 0x84, ram);
    STORE_32(0x08002100, 0x88, ram); STORE_32(0x08002600, 0x8C, ram);
    for (unsigned n = 0; n < 1024; ++n) { entry(ram, n, 0x08002000, 0x08002100, 0x40000000); }
    for (unsigned y = 0; y < 160; ++y) { renderer->d.drawScanline(&renderer->d, y); }
    require(memcmp(reference, output, 240 * 160 * sizeof(*reference)) == 0,
            "HGX3 all-current resident metadata exactly equals native full pipeline");
    /* A native, explicitly tinted palette is the independent reference for
     * foreign source-white palettes rendered through the full scanline path. */
    memset(gba->video.vram, 0x22, 32);
    gba->video.palette[2] = 0x773A;
    renderer->d.writePalette(&renderer->d, 4, 0x773A);
    STORE_32(0, 4, ram);
    for (unsigned y = 0; y < 160; ++y) { renderer->d.drawScanline(&renderer->d, y); }
    memcpy(reference, output, 240 * 160 * sizeof(*reference));
    require((reference[100 * 240 + 100] & 0xFFFFFF) == mColorFrom555(0x773A),
            "native full-pipeline DNS reference has the exact game night color");
    STORE_32(15, 4, ram);
    STORE_32(0x020000A0, 44, ram);
    STORE_32(3, 0xA0, ram); STORE_32(21509, 0xA4, ram);
    STORE_32(8191, 0xA8, ram); STORE_32(0x02000080, 0xAC, ram);
    for (unsigned n = 0; n < 1024; ++n) { entry(ram, n, 0x08002500, 0x08002600, 0x40000000); }
    for (unsigned y = 0; y < 160; ++y) { renderer->d.drawScanline(&renderer->d, y); }
    require(memcmp(reference, output, 240 * 160 * sizeof(*reference)) == 0,
            "HGX3 snapshotted foreign DNS exactly equals native DNS across the full rendered frame");
    require(GBAIORead(gba, 0x3EC) == 3 && GBAIORead(gba, 0x3EE) == 21509 && GBAIORead(gba, 0x3F0) == 8191,
            "HGX3 readonly diagnostics expose the renderer latched DNS environment");
    require((GBAIORead(gba, 0x3F2) & 0x1FFF) == 0x01F3 && GBAIORead(gba, 0x3F4) != 0
            && GBAIORead(gba, 0x3F6) == 0 && GBAIORead(gba, 0x3F8) != 0,
            "HGX3 diagnostics count foreign DNS branch and report its first bank");
    require(GBAIORead(gba, 0x3FC) == 0x7FFF && GBAIORead(gba, 0x3FE) == 0x773A,
            "HGX3 diagnostics expose the first DNS input and output palette colors");
    GBAIOWrite(gba, 0x3EC, 99);
    require(GBAIORead(gba, 0x3EC) == 3, "HGX3 diagnostic registers reject guest writes");
    /* New sources opt into coherent transactions with environment mask bit31.
     * Odd epochs may span many CPU frames; only complete images are public. */
    STORE_32(0x80001FFF, 0xA8, ram); STORE_32(0, 24, ram);
    for (unsigned y = 0; y < 160; ++y) { renderer->d.drawScanline(&renderer->d, y); }
    renderer->d.finishFrame(&renderer->d);
    memcpy(reference, output, 240 * 160 * sizeof(*reference));
    require(renderer->hgxPreviousValid && memcmp(reference, renderer->hgxPreviousFrame, 240 * 160 * sizeof(*reference)) == 0,
            "coherent HGX3 retains only a complete published frame");
    for (unsigned y = 0; y < 80; ++y) { renderer->d.drawScanline(&renderer->d, y); }
    STORE_32(1, 24, ram);
    memset(gba->video.vram, 0x11, 32);
    gba->video.palette[1] = 0x7C00;
    renderer->d.writePalette(&renderer->d, 2, 0x7C00);
    for (unsigned n = 0; n < 1024; ++n) { entry(ram, n, 0x08002000, 0x08002100, 0x40000000); }
    renderer->d.drawScanline(&renderer->d, 80);
    require(renderer->hgxHoldFrame && memcmp(reference, output, 240 * 160 * sizeof(*reference)) == 0,
            "midframe odd epoch retracts every partial row to the previous complete image");
    STORE_32(2, 24, ram);
    for (unsigned y = 81; y < 160; ++y) { renderer->d.drawScanline(&renderer->d, y); }
    renderer->d.finishFrame(&renderer->d);
    require(memcmp(reference, output, 240 * 160 * sizeof(*reference)) == 0
            && memcmp(reference, renderer->hgxPreviousFrame, 240 * 160 * sizeof(*reference)) == 0,
            "midframe commit cannot publish or replace the held complete image");
    for (unsigned y = 0; y < 160; ++y) { renderer->d.drawScanline(&renderer->d, y); }
    renderer->d.finishFrame(&renderer->d);
    require(!renderer->hgxHoldFrame && (output[100 * 240 + 100] & 0xFFFFFF) == mColorFrom555(0x7C00),
            "even transaction resumes the new atlas only at the next frame boundary");
    memcpy(reference, output, 240 * 160 * sizeof(*reference));
    for (unsigned y = 0; y < 80; ++y) { renderer->d.drawScanline(&renderer->d, y); }
    STORE_32(4, 24, ram); /* Entire transaction happened between two scanlines. */
    gba->video.palette[1] = 0x03E0;
    renderer->d.writePalette(&renderer->d, 2, 0x03E0);
    for (unsigned y = 80; y < 160; ++y) { renderer->d.drawScanline(&renderer->d, y); }
    renderer->d.finishFrame(&renderer->d);
    require(memcmp(reference, output, 240 * 160 * sizeof(*reference)) == 0,
            "even epoch changes between scanlines also retract a mixed frame");
    for (unsigned y = 0; y < 160; ++y) { renderer->d.drawScanline(&renderer->d, y); }
    renderer->d.finishFrame(&renderer->d);
    memcpy(reference, output, 240 * 160 * sizeof(*reference));
    STORE_32(5, 24, ram);
    for (unsigned frame = 0; frame < 3; ++frame) {
        for (unsigned y = 0; y < 160; ++y) { renderer->d.drawScanline(&renderer->d, y); }
        renderer->d.finishFrame(&renderer->d);
    }
    require(memcmp(reference, output, 240 * 160 * sizeof(*reference)) == 0,
            "transactions spanning multiple frames never expose staging resources");
    renderer->hgxPreviousValid = false;
    renderer->normalPalette[0] = mColorFrom555(0x4210);
    for (unsigned n = 0; n < 240 * 160; ++n) { output[n] = 0x55AA; }
    renderer->d.drawScanline(&renderer->d, 0);
    bool uniform = true;
    for (unsigned n = 0; n < 240 * 160; ++n) { uniform &= output[n] == mColorFrom555(0x4210); }
    require(uniform, "first transaction without a completed image uses a uniform safe backdrop");
    mColor* padded = malloc(256 * 160 * sizeof(*padded));
    require(padded != NULL, "allocate padded-stride hold regression");
    for (unsigned n = 0; n < 256 * 160; ++n) { padded[n] = 0x55AA; }
    core->setVideoBuffer(core, padded, 256);
    core->putPixels(core, reference, 240); /* A complete restored PNG may seed the hold. */
    renderer->d.drawScanline(&renderer->d, 0);
    const void* presented = NULL;
    size_t presentedStride = 0;
    core->getPixels(core, &presented, &presentedStride);
    bool strideSafe = presented == padded && presentedStride == 256;
    for (unsigned y = 0; y < 160; ++y) {
        strideSafe &= memcmp(padded + y * 256, reference + y * 240, 240 * sizeof(mColor)) == 0;
        for (unsigned x = 240; x < 256; ++x) { strideSafe &= padded[y * 256 + x] == 0x55AA; }
    }
    require(strideSafe, "held getPixels and restored screenshots honor output stride and padding");
    core->setVideoBuffer(core, output, 240);
    core->putPixels(core, reference, 240);
    free(padded);
    void* coherentState = malloc(core->stateSize(core));
    require(coherentState && core->saveState(core, coherentState), "save in-progress coherent transaction state");
    require(core->loadState(core, coherentState) && !renderer->hgxPreviousValid && !renderer->hgxHoldFrame,
            "state restore invalidates previous host transaction ownership");
    core->putPixels(core, reference, 240);
    renderer->d.drawScanline(&renderer->d, 88);
    require(renderer->hgxPreviousValid && memcmp(reference, output, 240 * 160 * sizeof(*reference)) == 0,
            "restored screenshot safely holds an odd epoch on a first nonzero scanline");
    free(coherentState);
    STORE_32(8191, 0xA8, ram); /* Old HGX3 sources keep their generation counter semantics. */
    STORE_32(9, 24, ram);
    renderer->d.writePalette(&renderer->d, 2, 0x03E0);
    for (unsigned n = 0; n < 4; ++n) { renderer->bg[n].enabled = 4; }
    for (unsigned y = 0; y < 160; ++y) { renderer->d.drawScanline(&renderer->d, y); }
    require(!renderer->hgxHoldFrame && (output[100 * 240 + 100] & 0xFFFFFF) == mColorFrom555(0x03E0),
            "legacy HGX3 without capability never treats odd generations as transactions");
    STORE_32(0x32584748, 0, ram); STORE_32(0x80001FFF, 0xA8, ram);
    for (unsigned y = 0; y < 160; ++y) { renderer->d.drawScanline(&renderer->d, y); }
    require(!renderer->hgxHoldFrame && (output[100 * 240 + 100] & 0xFFFFFF) == mColorFrom555(0x001F),
            "HGX2 rendering remains unchanged even with odd generations and unrelated high bits");
	free(reference);
	h4Tests(gba, renderer);
	obj256Tests(core);
	bg256Tests(core);
	bgScaled8Tests(core);
	paletteCapacityTest(core);
    uint8_t* ordinary = malloc(0x4000);
    require(ordinary != NULL, "allocate ordinary-ROM opt-out regression");
    memcpy(ordinary, gba->memory.rom, 0x4000);
    ordinary[0xB5] = 0; /* Same artificial ROM, without HGX opt-in signature. */
    core->unloadROM(core);
    mCoreConfigSetIntValue(&core->config, "threadedVideo", 0);
    require(core->loadROM(core, VFileMemChunk(ordinary, 0x4000)), "load ordinary artificial ROM");
    free(ordinary);
    core->reset(core);
    GBAIOWrite(gba, GBA_REG_BG1HOFS, 0x0123);
    GBAIOWrite(gba, GBA_REG_BG1VOFS, 0x0145);
    uint16_t openBus = (uint16_t) GBALoadBad(gba->cpu);
    require(GBAIORead(gba, 0x3E8) == openBus && GBAIORead(gba, 0x3EA) == openBus,
            "ordinary ROM retains open-bus reads at HGX viewport mirror addresses");
    require(GBAIORead(gba, 0x3EC) == openBus && GBAIORead(gba, 0x3FE) == openBus,
            "ordinary ROM retains open-bus reads at HGX3 diagnostics addresses");
    mCoreConfigDeinit(&core->config);
    core->deinit(core);
    free(output);
    printf("HGX synthetic regression: %u checks passed\n", checks);
    return 0;
}

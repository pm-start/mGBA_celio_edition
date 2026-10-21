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

void GBAVideoSoftwareRendererLatchHGXFrame(struct GBAVideoSoftwareRenderer*);
void GBAVideoSoftwareRendererLatchHGX4(struct GBAVideoSoftwareRenderer*);
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

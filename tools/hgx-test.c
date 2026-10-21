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
static void draw(struct GBAVideoSoftwareRenderer* renderer, int y) {
    for (unsigned x = 0; x < 240; ++x) { renderer->row[x] = FLAG_UNWRITTEN; }
    renderer->start = 0; renderer->end = 32;
    GBAVideoSoftwareRendererDrawBackgroundMode0(renderer, &renderer->bg[0], y);
}
static bool isColor(struct GBAVideoSoftwareRenderer* renderer, unsigned x, unsigned bgr) {
    return (renderer->row[x] & 0xFFFFFF) == (mColorFrom555(bgr) & 0xFFFFFF);
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
    free(reference);
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
    mCoreConfigDeinit(&core->config);
    core->deinit(core);
    free(output);
    printf("HGX synthetic regression: %u checks passed\n", checks);
    return 0;
}

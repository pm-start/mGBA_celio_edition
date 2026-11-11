/* Unit test for the 128MiB ROM bank window (feat-rom128, C0_DESIGN.md).
 * Build (MSYS2 mingw64, after building mGBA into <build>):
 *   gcc -O1 -I include -I <build>/include -DM_CORE_GBA src/gba/test/rom128.c <build>/libmgba.a -lz -lm -lzip -lshlwapi -lole32 -lshell32 -luuid -lws2_32 -lpng -o rom128.exe
 * Usage: rom128.exe <work dir>   (exit code 0 = all passed)
 */
#include <mgba/core/core.h>
#include <mgba/gba/core.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/memory.h>
#include <mgba/internal/arm/arm.h>
#include <mgba/internal/arm/isa-inlines.h>
#include <mgba-util/vfs.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MB (1024u * 1024u)
static int failures = 0, checks = 0;
#define CHECK(cond, ...) do { ++checks; if (!(cond)) { ++failures; printf("NG  line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static struct mCore* core;
static int cyc;
#define CPU ((struct ARMCore*) core->cpu)
static void w16(uint32_t a, uint16_t v) { CPU->memory.store16(CPU, a, v, &cyc); }
static uint32_t r32(uint32_t a) { return CPU->memory.load32(CPU, a, &cyc); }
static void sw(uint16_t n) { w16(GBA_ROM_BANK_REG, 0xB128); w16(GBA_ROM_BANK_REG, 0x5A0E); w16(GBA_ROM_BANK_REG, n); w16(GBA_ROM_BANK_REG, (uint16_t) ~n); }
/* bank b: words: +0 = marker, +0x1000, last word */
static uint32_t M(int b) { return 0xB0000000u | b; }
static uint32_t W1(int b) { return 0xC0000000u | b; }
static uint32_t W2(int b) { return 0xD0000000u | b; }

static void makeRom(const char* path, unsigned banks, int armCode) {
	FILE* f = fopen(path, "wb");
	uint8_t* buf = calloc(1, 32 * MB);
	unsigned b;
	for (b = 0; b < banks; ++b) {
		memset(buf, 0xFF, 32 * MB);
		uint32_t m = M(b), w = W1(b), x = W2(b);
		memcpy(buf, &m, 4); memcpy(buf + 0x1000, &w, 4); memcpy(buf + 32 * MB - 4, &x, 4);
		if (armCode) {
			uint32_t code[4];
			if (b == 0) {
				/* mov r1,#0x0A000000 ; add r1,r1,#8 ; bx r1 (arm->arm, bit0 clear) */
				code[0] = 0xE3A0140A; code[1] = 0xE2811008; code[2] = 0xE12FFF11;
				memcpy(buf + 0x2000, code, 12);
			} else if (b == 1 || b == 2) {
				/* mov r0,#0x11 (bank 2: 0x22) ; b . */
				code[0] = b == 1 ? 0xE3A00011 : 0xE3A00022; code[1] = 0xEAFFFFFE;
				memcpy(buf + 8, code, 8);
			}
		}
		fwrite(buf, 1, 32 * MB, f);
	}
	free(buf);
	fclose(f);
}

static bool load(const char* path) {
	if (core) { core->deinit(core); core = NULL; }
	core = GBACoreCreate();
	if (!core) return false;
	core->init(core);
	mCoreInitConfig(core, NULL);
	{ struct VFile* vf = VFileOpen(path, O_RDONLY | O_BINARY); if (!vf || !core->loadROM(core, vf)) { return false; } }
	core->reset(core);
	return true;
}

int main(int argc, char** argv) {
	char p[700];
	if (argc < 2) return 2;
	snprintf(p, sizeof(p), "%s/r128.gba", argv[1]);
	makeRom(p, 4, 0);
	CHECK(load(p), "load 128MiB");
	CHECK(r32(0x08000000) == M(0), "bank0 at 0x08");
	CHECK(r32(0x08001000) == W1(0), "bank0 +0x1000");
	CHECK(r32(0x0A000000) == M(1), "default window = bank1");
	CHECK(r32(0x0BFFFFFC) == W2(1), "bank1 last word");
	CHECK(r32(0x0C000000) == M(0), "0x0C mirrors bank0");
	CHECK(CPU->memory.load8(CPU, 0x0A000000, &cyc) == (M(1) & 0xFF), "8-bit read");
	CHECK(CPU->memory.load16(CPU, 0x0A001000, &cyc) == (W1(1) & 0xFFFF), "16-bit read");
	sw(2);
	CHECK(r32(0x0A000000) == M(2), "window 2");
	CHECK(r32(0x0A001000) == W1(2), "window 2 +0x1000");
	CHECK(r32(0x0BFFFFFC) == W2(2), "window 2 last");
	CHECK(CPU->memory.load8(CPU, 0x0A000000, &cyc) == (M(2) & 0xFF), "8-bit read in window 2");
	CHECK(CPU->memory.load16(CPU, 0x0A001000, &cyc) == (W1(2) & 0xFFFF), "16-bit read in window 2");
	CHECK(r32(0x08000000) == M(0), "bank0 unaffected");
	CHECK(r32(0x0C000000) == M(0), "mirror unaffected");
	sw(3);
	CHECK(r32(0x0A000000) == M(3) && r32(0x0BFFFFFC) == W2(3), "window 3");
	sw(1);
	CHECK(r32(0x0A000000) == M(1), "back to 1");
	/* wrong sequences */
	w16(GBA_ROM_BANK_REG, 0xB128); w16(GBA_ROM_BANK_REG, 0x5A0E); w16(GBA_ROM_BANK_REG, 2); w16(GBA_ROM_BANK_REG, 2);
	CHECK(r32(0x0A000000) == M(1), "bad complement refused");
	w16(GBA_ROM_BANK_REG, 0xB128); w16(GBA_ROM_BANK_REG, 0x1234); w16(GBA_ROM_BANK_REG, 2); w16(GBA_ROM_BANK_REG, (uint16_t) ~2);
	CHECK(r32(0x0A000000) == M(1), "bad key2 refused");
	sw(4);
	CHECK(r32(0x0A000000) == M(1), "bank 4 refused");
	sw(0);
	CHECK(r32(0x0A000000) == M(1), "bank 0 refused");
	w16(GBA_ROM_BANK_REG, 2); w16(GBA_ROM_BANK_REG, (uint16_t) ~2);
	CHECK(r32(0x0A000000) == M(1), "no key = nothing");
	w16(GBA_ROM_BANK_REG, 0xB128); sw(2);
	CHECK(r32(0x0A000000) == M(2), "restart on key1");
	sw(1);
	{
		uint32_t addrs[] = { 0x0B7FFF02, 0x0B7FFEFE, 0x0A7FFF00, 0x097FFF00, 0x09FFFFFE, 0x0BFFFFFE, 0x0D7FFF00 };
		unsigned i;
		for (i = 0; i < sizeof(addrs) / sizeof(*addrs); ++i) {
			uint32_t a = addrs[i];
			w16(a, 0xB128); w16(a, 0x5A0E); w16(a, 2); w16(a, (uint16_t) ~2);
			CHECK(r32(0x0A000000) == M(1), "other address %08X refused", a);
		}
		CPU->memory.store8(CPU, GBA_ROM_BANK_REG, 0x28, &cyc); CPU->memory.store32(CPU, GBA_ROM_BANK_REG, 0xB128, &cyc);
		CHECK(r32(0x0A000000) == M(1), "8/32-bit ignored");
	}
	CHECK(r32(GBA_ROM_BANK_REG & ~3u) == 0xFFFFFFFFu, "data at register address reads normally");
	{
		size_t sz = core->stateSize(core);
		void* st = malloc(sz);
		sw(3);
		CHECK(core->saveState(core, st), "save state");
		sw(1);
		CHECK(r32(0x0A000000) == M(1), "switched to 1");
		CHECK(core->loadState(core, st), "load state");
		CHECK(r32(0x0A000000) == M(3), "window restored from state (3)");
		sw(1);
		CHECK(core->saveState(core, st), "save state 1");
		sw(2);
		CHECK(core->loadState(core, st), "load state");
		CHECK(r32(0x0A000000) == M(1), "window restored from state (1)");
		free(st);
	}
	sw(2);
	core->reset(core);
	CHECK(r32(0x0A000000) == M(1), "reset -> window 1");
	w16(GBA_ROM_BANK_REG, 0xB128); w16(GBA_ROM_BANK_REG, 0x5A0E); core->reset(core);
	w16(GBA_ROM_BANK_REG, 2); w16(GBA_ROM_BANK_REG, (uint16_t) ~2);
	CHECK(r32(0x0A000000) == M(1), "key progress cleared by reset");
	/* instruction fetch from the window */
	core->deinit(core); core = NULL;
	makeRom(p, 4, 1);
	CHECK(load(p), "load code rom");
	{
		struct ARMCore* cpu = core->cpu;
		int i;
		cpu->gprs[ARM_PC] = 0x08002000;
		cpu->cpsr.t = 0;
		ARMWritePC(cpu);
		for (i = 0; i < 40; ++i) core->step(core);
		CHECK(cpu->gprs[0] == 0x11, "execute in window 1: r0=%X (expect 0x11)", cpu->gprs[0]);
		sw(2);
		cpu->gprs[0] = 0;
		cpu->gprs[ARM_PC] = 0x08002000;
		ARMWritePC(cpu);
		for (i = 0; i < 40; ++i) core->step(core);
		CHECK(cpu->gprs[0] == 0x22, "execute in window 2: r0=%X (expect 0x22)", cpu->gprs[0]);
	}
	core->deinit(core); core = NULL;
	makeRom(p, 2, 0);
	CHECK(load(p), "load 64MiB");
	CHECK(r32(0x0A000000) == M(1) && r32(0x08000000) == M(0), "64MiB layout unchanged");
	sw(2);
	CHECK(r32(0x0A000000) == M(1), "64MiB: bank 2 ignored");
	core->deinit(core); core = NULL;
	makeRom(p, 1, 0);
	CHECK(load(p), "load 32MiB");
	CHECK(r32(0x08000000) == M(0), "32MiB layout unchanged");
	sw(1);
	CHECK(r32(0x08000000) == M(0), "32MiB: register ignored");
	core->deinit(core);
	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}

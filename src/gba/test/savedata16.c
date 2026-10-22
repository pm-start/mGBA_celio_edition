/* Unit test for the 16MB flash extension of GBASavedata (feat-save16).
 * Build (MSYS2 mingw64), see save16-test.txt in the project tools:
 *   gcc -O1 -I include -I <build>/include -DM_CORE_GBA src/gba/test/savedata16.c <build>/libmgba.a -lz -lm -o savedata16.exe
 * Usage: savedata16.exe <work dir>   (exit code 0 = all passed)
 */
#include <mgba/core/timing.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/savedata.h>
#include <mgba/internal/gba/serialize.h>
#include <mgba-util/vfs.h>
#include <mgba/core/interface.h>
#include <mgba/internal/gba/savedata.h>
#include <fcntl.h>
#include <mgba/internal/defines.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MB (1024u * 1024u)
static int failures = 0;
static int checks = 0;
#define CHECK(cond, ...) do { ++checks; if (!(cond)) { ++failures; printf("NG  line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static char dir[512];
static char path[600];

static struct GBA* gba;
static int32_t nextEvent, cycles;

static const char* P(const char* name) {
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	return path;
}

static size_t fileSize(const char* name) {
	FILE* f = fopen(P(name), "rb");
	if (!f) {
		return (size_t) -1;
	}
	fseek(f, 0, SEEK_END);
	size_t n = ftell(f);
	fclose(f);
	return n;
}

static uint8_t* readAll(const char* name, size_t* n) {
	FILE* f = fopen(P(name), "rb");
	if (!f) {
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	*n = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t* b = malloc(*n ? *n : 1);
	if (fread(b, 1, *n, f) != *n) {
		free(b);
		f = NULL;
	}
	fclose(f);
	return b;
}

// bank number b is stored in byte 0 of every 4KB sector as (b ^ sector)
static uint8_t patternAt(size_t off) {
	return (uint8_t) ((off >> 16) * 7 + ((off >> 12) & 0xF) + 1) & 0xFE;
}

static void makeSave(const char* name, size_t dataSize, int footer) {
	FILE* f = fopen(P(name), "wb");
	uint8_t* b = malloc(dataSize + 16);
	for (size_t i = 0; i < dataSize; ++i) {
		b[i] = patternAt(i);
	}
	size_t n = dataSize;
	if (footer) {
		for (int i = 0; i < 16; ++i) {
			b[n + i] = 0xA0 + i;
		}
		n += 16;
	}
	fwrite(b, 1, n, f);
	fclose(f);
	free(b);
}

static void freshGBA(void) {
	if (gba) {
		free(gba);
	}
	gba = calloc(1, sizeof(*gba));
	mTimingInit(&gba->timing, &nextEvent, &cycles);
	nextEvent = 0;
	cycles = 0;
}

static struct GBASavedata* openSave(const char* name, struct VFile** vfOut) {
	freshGBA();
	struct VFile* vf = VFileOpen(P(name), O_RDWR | O_CREAT);
	struct GBASavedata* sd = &gba->memory.savedata;
	GBASavedataInit(sd, vf);
	sd->p = gba;
	GBASavedataForceType(sd, GBA_SAVEDATA_FLASH1M);
	*vfOut = vf;
	return sd;
}

static void closeSave(struct GBASavedata* sd, struct VFile* vf) {
	GBASavedataDeinit(sd);
	vf->close(vf);
}

static void cmd(struct GBASavedata* sd, uint8_t c) {
	GBASavedataWriteFlash(sd, 0x5555, 0xAA);
	GBASavedataWriteFlash(sd, 0x2AAA, 0x55);
	GBASavedataWriteFlash(sd, 0x5555, c);
}

static void switchBank(struct GBASavedata* sd, int bank) {
	cmd(sd, 0xB0);
	GBASavedataWriteFlash(sd, 0, bank);
}

static void program(struct GBASavedata* sd, uint16_t addr, uint8_t v) {
	cmd(sd, 0xA0);
	GBASavedataWriteFlash(sd, addr, v);
	// let the chip settle (otherwise reads in the same sector return the data# polling bit)
	mTimingDeschedule(&gba->timing, &sd->dust);
}

static void flush(struct GBASavedata* sd) {
	GBASavedataClean(sd, 0);
	sd->dirty = mSAVEDATA_DIRT_NEW;
	sd->dirtAge = 0;
	GBASavedataClean(sd, 100000);
}

static void setRtc(void) {
	gba->memory.hw.devices = HW_RTC;
	uint8_t t[7] = {0x26, 0x10, 0x22, 0x04, 0x12, 0x34, 0x56};
	memcpy(gba->memory.hw.rtc.time, t, 7);
	gba->memory.hw.rtc.control = 0x40;
	gba->memory.hw.rtc.lastLatch = 1234567890ULL;
}

static int allFF(const uint8_t* b, size_t n) {
	for (size_t i = 0; i < n; ++i) {
		if (b[i] != 0xFF) {
			return 0;
		}
	}
	return 1;
}

static int patternOk(const uint8_t* b, size_t from, size_t to) {
	for (size_t i = from; i < to; ++i) {
		if (b[i] != patternAt(i)) {
			return 0;
		}
	}
	return 1;
}

static void test_6mb_kept(void) {
	printf("-- 6MB save (no footer): open, read, write inside 6MB, must stay 6MB\n");
	makeSave("t1.sav", 6 * MB, 0);
	struct VFile* vf;
	struct GBASavedata* sd = openSave("t1.sav", &vf);
	CHECK(sd->flashExt && sd->flashExtSize == 6 * MB, "flashExtSize=%zu", sd->flashExtSize);
	CHECK(GBASavedataSize(sd) == 6 * MB, "size");
	CHECK(fileSize("t1.sav") == 6 * MB, "file size after open = %zu", fileSize("t1.sav"));
	switchBank(sd, 95);
	CHECK(GBASavedataReadFlash(sd, 0x1234) == patternAt((95u << 16) + 0x1234), "bank95 read");
	program(sd, 0x1234, 0x00);
	switchBank(sd, 2);
	CHECK(GBASavedataReadFlash(sd, 0x10) == patternAt((2u << 16) + 0x10), "bank2 read");
	// bank 96..255 read-only visits must not grow the file
	for (int b = 96; b < 256; b += 17) {
		switchBank(sd, b);
		CHECK(GBASavedataReadFlash(sd, 0x0) == 0xFF && GBASavedataReadFlash(sd, 0xFFFF) == 0xFF, "bank %d reads erased", b);
	}
	switchBank(sd, 255);
	CHECK(GBASavedataReadFlash(sd, 0x8000) == 0xFF, "bank255 erased");
	CHECK(sd->flashExtSize == 6 * MB && sd->pendingBank == 255, "still 6MB, pending=%d", sd->pendingBank);
	switchBank(sd, 1);
	CHECK(sd->pendingBank == -1, "pending cleared");
	flush(sd);
	closeSave(sd, vf);
	CHECK(fileSize("t1.sav") == 6 * MB, "file still 6MB after read-only visits: %zu", fileSize("t1.sav"));
	size_t n;
	uint8_t* b = readAll("t1.sav", &n);
	CHECK(b[(95u << 16) + 0x1234] == 0x00, "programmed byte kept");
	b[(95u << 16) + 0x1234] = patternAt((95u << 16) + 0x1234);
	CHECK(patternOk(b, 0, n), "rest of 6MB unchanged");
	free(b);
}

static void test_6mb_footer_grow(void) {
	printf("-- 6MB+16 save: RTC kept at 6MB until a write beyond bank 95; then 16MB+16, footer moved\n");
	makeSave("t2.sav", 6 * MB, 1);
	struct VFile* vf;
	struct GBASavedata* sd = openSave("t2.sav", &vf);
	setRtc();
	CHECK(sd->flashExtSize == 6 * MB, "6MB tier");
	CHECK(fileSize("t2.sav") == 6 * MB + 16, "size after open %zu", fileSize("t2.sav"));
	GBASavedataRTCWrite(sd);
	CHECK(fileSize("t2.sav") == 6 * MB + 16, "size after RTC write %zu", fileSize("t2.sav"));
	switchBank(sd, 130);
	CHECK(GBASavedataReadFlash(sd, 5) == 0xFF, "bank130 read erased");
	GBASavedataRTCWrite(sd);
	CHECK(sd->pendingBank == 130 && sd->currentBank != NULL && GBASavedataReadFlash(sd, 5) == 0xFF, "RTC write keeps the virtual bank");
	CHECK(fileSize("t2.sav") == 6 * MB + 16, "size still 6MB+16 %zu", fileSize("t2.sav"));
	program(sd, 0x4321, 0x5A);
	CHECK(sd->flashExtSize == 16 * MB && sd->pendingBank == -1, "grown to 16MB");
	CHECK(GBASavedataReadFlash(sd, 0x4321) == 0x5A, "read back bank130");
	CHECK(GBASavedataReadFlash(sd, 0x4322) == 0xFF, "neighbour erased");
	switchBank(sd, 94);
	CHECK(GBASavedataReadFlash(sd, 0x100) == patternAt((94u << 16) + 0x100), "old data still there");
	setRtc();
	flush(sd);
	closeSave(sd, vf);
	CHECK(fileSize("t2.sav") == 16 * MB + 16, "file 16MB+16: %zu", fileSize("t2.sav"));
	size_t n;
	uint8_t* b = readAll("t2.sav", &n);
	CHECK(patternOk(b, 0, 6 * MB), "first 6MB intact");
	CHECK(allFF(b + 6 * MB, 16), "old footer area erased");
	CHECK(allFF(b + 6 * MB + 16, (130u << 16) - 6 * MB - 16), "between erased");
	CHECK(b[(130u << 16) + 0x4321] == 0x5A, "written byte");
	// footer: time bytes 26 10 22 ...
	CHECK(b[16 * MB] == 0x26 && b[16 * MB + 2] == 0x22, "RTC footer at 16MB: %02x %02x", b[16 * MB], b[16 * MB + 2]);
	free(b);
	// reopen: stays 16MB, RTC readable
	sd = openSave("t2.sav", &vf);
	CHECK(sd->flashExtSize == 16 * MB && GBASavedataSize(sd) == 16 * MB, "reopen as 16MB");
	gba->memory.hw.devices = HW_RTC;
	GBASavedataRTCRead(sd);
	CHECK(gba->memory.hw.rtc.time[2] == 0x22 && gba->memory.hw.rtc.time[6] == 0x56, "RTC read back %02x", gba->memory.hw.rtc.time[2]);
	switchBank(sd, 130);
	CHECK(GBASavedataReadFlash(sd, 0x4321) == 0x5A, "bank130 persisted");
	closeSave(sd, vf);
}

static void test_fresh_16mb(void) {
	printf("-- new 128KB save: banks 64..255 write/read, RTC, file sizes\n");
	remove(P("t3.sav"));
	struct VFile* vf;
	struct GBASavedata* sd = openSave("t3.sav", &vf);
	setRtc();
	CHECK(!sd->flashExt && GBASavedataSize(sd) == 128 * 1024, "starts at 128KB");
	switchBank(sd, 2);
	CHECK(GBASavedataReadFlash(sd, 0) == 0xFF && sd->flashExtSize == 0, "bank2 read does not grow");
	program(sd, 0, 0x11);
	CHECK(sd->flashExtSize == 6 * MB, "bank2 write grows to 6MB, got %zu", sd->flashExtSize);
	static const int banks[] = {64, 95, 96, 127, 128, 175, 200, 254, 255};
	for (unsigned i = 0; i < sizeof(banks) / sizeof(banks[0]); ++i) {
		switchBank(sd, banks[i]);
		program(sd, 0x100 + i, (uint8_t) (0x40 + i));
		program(sd, 0xFFFF, (uint8_t) (0x80 + i));
	}
	CHECK(sd->flashExtSize == 16 * MB, "16MB after bank >= 96 write");
	for (unsigned i = 0; i < sizeof(banks) / sizeof(banks[0]); ++i) {
		switchBank(sd, banks[i]);
		CHECK(GBASavedataReadFlash(sd, 0x100 + i) == (uint8_t) (0x40 + i), "bank %d byte", banks[i]);
		CHECK(GBASavedataReadFlash(sd, 0xFFFF) == (uint8_t) (0x80 + i), "bank %d last", banks[i]);
	}
	switchBank(sd, 0);
	program(sd, 0x20, 0x77);
	GBASavedataRTCWrite(sd);
	CHECK(GBASavedataReadFlash(sd, 0x20) == 0x77, "bank0 still current after RTC write");
	flush(sd);
	closeSave(sd, vf);
	CHECK(fileSize("t3.sav") == 16 * MB + 16, "t3 size %zu", fileSize("t3.sav"));
	sd = openSave("t3.sav", &vf);
	setRtc();
	CHECK(sd->flashExtSize == 16 * MB, "reopen 16MB");
	for (unsigned i = 0; i < sizeof(banks) / sizeof(banks[0]); ++i) {
		switchBank(sd, banks[i]);
		CHECK(GBASavedataReadFlash(sd, 0x100 + i) == (uint8_t) (0x40 + i), "after reopen bank %d", banks[i]);
	}
	switchBank(sd, 0);
	CHECK(GBASavedataReadFlash(sd, 0x20) == 0x77, "bank0 persisted");
	closeSave(sd, vf);
}

static void test_16mb_open(void) {
	printf("-- 16MB file with and without footer\n");
	makeSave("t4.sav", 16 * MB, 1);
	struct VFile* vf;
	struct GBASavedata* sd = openSave("t4.sav", &vf);
	CHECK(sd->flashExtSize == 16 * MB && fileSize("t4.sav") == 16 * MB + 16, "16MB+16 unchanged");
	switchBank(sd, 255);
	CHECK(GBASavedataReadFlash(sd, 7) == patternAt((255u << 16) + 7), "bank255 pattern");
	program(sd, 7, 0x00);
	switchBank(sd, 64);
	CHECK(GBASavedataReadFlash(sd, 0x3000) == patternAt((64u << 16) + 0x3000), "bank64");
	setRtc();
	GBASavedataRTCWrite(sd);
	flush(sd);
	closeSave(sd, vf);
	size_t n;
	uint8_t* b = readAll("t4.sav", &n);
	CHECK(n == 16 * MB + 16 && b[(255u << 16) + 7] == 0 && b[16 * MB] == 0x26, "after write n=%zu", n);
	free(b);
	makeSave("t5.sav", 16 * MB, 0);
	sd = openSave("t5.sav", &vf);
	CHECK(sd->flashExtSize == 16 * MB, "16MB no footer");
	setRtc();
	GBASavedataRTCWrite(sd);
	closeSave(sd, vf);
	CHECK(fileSize("t5.sav") == 16 * MB + 16, "footer appended: %zu", fileSize("t5.sav"));
}

static void test_small_saves(void) {
	printf("-- 128KB and 1MiB saves, GBASavedataLoad of a 128KB(+16) save does not grow\n");
	makeSave("t6.sav", 128 * 1024, 1);
	struct VFile* vf;
	struct GBASavedata* sd = openSave("t6.sav", &vf);
	CHECK(!sd->flashExt && fileSize("t6.sav") == 128 * 1024 + 16, "128KB+16 stays");
	closeSave(sd, vf);
	// Load path
	freshGBA();
	remove(P("t7.sav"));
	vf = VFileOpen(P("t7.sav"), O_RDWR | O_CREAT);
	sd = &gba->memory.savedata;
	GBASavedataInit(sd, vf);
	sd->p = gba;
	GBASavedataForceType(sd, GBA_SAVEDATA_FLASH1M);
	struct VFile* in = VFileOpen(P("t6.sav"), O_RDONLY);
	CHECK(GBASavedataLoad(sd, in), "load ok");
	CHECK(!sd->flashExt && GBASavedataSize(sd) == 128 * 1024, "Load of 128KB+16 does not grow");
	in->close(in);
	closeSave(sd, vf);
	makeSave("t8.sav", MB, 0);
	sd = openSave("t8.sav", &vf);
	CHECK(sd->flashExtSize == 6 * MB, "1MiB -> 6MB");
	closeSave(sd, vf);
	CHECK(fileSize("t8.sav") == 6 * MB, "1MiB file grew to 6MB only: %zu", fileSize("t8.sav"));
	size_t n;
	uint8_t* b = readAll("t8.sav", &n);
	CHECK(patternOk(b, 0, MB) && allFF(b + MB, 5 * MB), "1MiB data kept, rest erased");
	free(b);
	// Load a 6MB save into a fresh file: stays 6MB; load 16MB: 16MB
	freshGBA();
	remove(P("t9.sav"));
	vf = VFileOpen(P("t9.sav"), O_RDWR | O_CREAT);
	sd = &gba->memory.savedata;
	GBASavedataInit(sd, vf);
	sd->p = gba;
	GBASavedataForceType(sd, GBA_SAVEDATA_FLASH1M);
	in = VFileOpen(P("t1.sav"), O_RDONLY);
	GBASavedataLoad(sd, in);
	in->close(in);
	CHECK(sd->flashExtSize == 6 * MB, "Load 6MB -> 6MB");
	in = VFileOpen(P("t5.sav"), O_RDONLY);
	GBASavedataLoad(sd, in);
	in->close(in);
	CHECK(sd->flashExtSize == 16 * MB, "Load 16MB -> 16MB");
	closeSave(sd, vf);
}

static void test_state(void) {
	printf("-- savestate round trip (bank in reserved u16)\n");
	makeSave("ts.sav", 6 * MB, 1);
	struct VFile* vf;
	struct GBASavedata* sd = openSave("ts.sav", &vf);
	struct GBASerializedState* st = calloc(1, sizeof(*st));
	switchBank(sd, 200);
	GBASavedataSerialize(sd, st);
	uint16_t r;
	memcpy(&r, &st->savedata.reserved, 2);
	CHECK(r == 200, "reserved=%u", r);
	closeSave(sd, vf);
	sd = openSave("ts.sav", &vf);
	GBASavedataDeserialize(sd, st);
	CHECK(sd->pendingBank == 200 && sd->flashExtSize == 6 * MB, "restored pending=%d size not grown", sd->pendingBank);
	CHECK(fileSize("ts.sav") == 6 * MB + 16, "state load does not grow the file");
	// state made by an older build: reserved=0, flash bank in flags
	memset(st, 0, sizeof(*st));
	st->savedata.type = GBA_SAVEDATA_FLASH1M;
	st->savedata.flags = GBASerializedSavedataFlagsTestFillFlashBank(0, true);
	GBASavedataDeserialize(sd, st);
	CHECK(sd->pendingBank == -1 && sd->currentBank == &sd->data[0x10000], "old state: bank 1");
	r = 70;
	memcpy(&st->savedata.reserved, &r, 2);
	GBASavedataDeserialize(sd, st);
	CHECK(sd->currentBank == &sd->data[70u << 16], "old state: bank 70 inside 6MB");
	free(st);
	closeSave(sd, vf);
}

int main(int argc, char** argv) {
	if (argc < 2) {
		printf("usage: %s <work dir>\n", argv[0]);
		return 2;
	}
	snprintf(dir, sizeof(dir), "%s", argv[1]);
	test_6mb_kept();
	test_6mb_footer_grow();
	test_fresh_16mb();
	test_16mb_open();
	test_small_saves();
	test_state();
	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}

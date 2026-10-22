/* Copyright (c) 2013-2015 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/gba/savedata.h>

#include <mgba/internal/arm/macros.h>
#include <mgba/internal/defines.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/serialize.h>

#include <mgba-util/memory.h>
#include <mgba-util/vfs.h>

#ifdef PSP2
#include <psp2/rtc.h>
#endif

#include <errno.h>
#include <fcntl.h>

// Some testing was done here...
// Erase cycles can vary greatly.
// Some games may vary anywhere between about 2000 cycles to up to 30000 cycles. (Observed on a Macronix (09C2) chip).
// Other games vary from very little, with a fairly solid 20500 cycle count. (Observed on a SST (D4BF) chip).
// An average estimation is as follows.
// Short flash waits so the in-game save takes a few frames instead of ~1.3 s.
// Games poll the chip until it is ready, so a shorter wait is safe; real hardware is unchanged.
#define FLASH_ERASE_CYCLES 2000
#define FLASH_PROGRAM_CYCLES 20
// This needs real testing, and is only an estimation currently
#define EEPROM_SETTLE_CYCLES 115000

mLOG_DEFINE_CATEGORY(GBA_SAVE, "GBA Savedata", "gba.savedata");

static void _flashSwitchBank(struct GBASavedata* savedata, int bank);
static bool _flashExtend(struct GBASavedata* savedata, size_t target);
static size_t _flashExtDataEnd(off_t end);
static size_t _flashCapacity(const struct GBASavedata* savedata);
static size_t _flashTierForSize(off_t end);
static void _flashMaterialize(struct GBASavedata* savedata);

// Reads of a bank beyond the data held see erased flash from here; the data grows on the first write
static uint8_t _erasedBank[0x10000];
static void _flashErase(struct GBASavedata* savedata);
static void _flashEraseSector(struct GBASavedata* savedata, uint16_t sectorStart);

static void _ashesToAshes(struct mTiming* timing, void* user, uint32_t cyclesLate) {
	UNUSED(timing);
	UNUSED(user);
	UNUSED(cyclesLate);
	// Funk to funky
}

void GBASavedataInit(struct GBASavedata* savedata, struct VFile* vf) {
	savedata->type = GBA_SAVEDATA_AUTODETECT;
	savedata->data = NULL;
	savedata->command = EEPROM_COMMAND_NULL;
	savedata->flashState = FLASH_STATE_RAW;
	savedata->vf = vf;
	if (savedata->realVf && savedata->realVf != vf) {
		savedata->realVf->close(savedata->realVf);
	}
	savedata->realVf = vf;
	savedata->mapMode = MAP_WRITE;
	savedata->maskWriteback = false;
	savedata->dirty = 0;
	savedata->dirtAge = 0;
	savedata->flashExt = false;
	savedata->flashExtSize = 0;
	savedata->pendingBank = -1;
	savedata->dust.name = "GBA Savedata Settling";
	savedata->dust.priority = 0x70;
	savedata->dust.context = savedata;
	savedata->dust.callback = _ashesToAshes;
}

void GBASavedataReset(struct GBASavedata* savedata) {
	savedata->command = EEPROM_COMMAND_NULL;
	savedata->flashState = FLASH_STATE_RAW;
}

void GBASavedataDeinit(struct GBASavedata* savedata) {
	if (savedata->vf) {
		size_t size = GBASavedataSize(savedata);
		if (savedata->data) {
			savedata->vf->unmap(savedata->vf, savedata->data, size);
		}
		savedata->vf = NULL;
	} else {
		switch (savedata->type) {
		case GBA_SAVEDATA_SRAM:
			mappedMemoryFree(savedata->data, GBA_SIZE_SRAM);
			break;
		case GBA_SAVEDATA_SRAM512:
			mappedMemoryFree(savedata->data, GBA_SIZE_SRAM512);
			break;
		case GBA_SAVEDATA_FLASH512:
			mappedMemoryFree(savedata->data, GBA_SIZE_FLASH512);
			break;
		case GBA_SAVEDATA_FLASH1M:
			mappedMemoryFree(savedata->data, GBA_SIZE_FLASH_EXT);
			break;
		case GBA_SAVEDATA_EEPROM:
			mappedMemoryFree(savedata->data, GBA_SIZE_EEPROM);
			break;
		case GBA_SAVEDATA_EEPROM512:
			mappedMemoryFree(savedata->data, GBA_SIZE_EEPROM512);
			break;
		case GBA_SAVEDATA_FORCE_NONE:
		case GBA_SAVEDATA_AUTODETECT:
			break;
		}
	}
	savedata->data = 0;
	savedata->type = GBA_SAVEDATA_AUTODETECT;
	savedata->flashExt = false;
	savedata->flashExtSize = 0;
	savedata->pendingBank = -1;
}

void GBASavedataMask(struct GBASavedata* savedata, struct VFile* vf, bool writeback) {
	enum GBASavedataType type = savedata->type;
	struct VFile* oldVf = savedata->vf;
	GBASavedataDeinit(savedata);
	if (oldVf && oldVf != savedata->realVf) {
		oldVf->close(oldVf);
	}
	savedata->vf = vf;
	savedata->mapMode = MAP_READ;
	savedata->maskWriteback = writeback;
	GBASavedataForceType(savedata, type);
}

void GBASavedataUnmask(struct GBASavedata* savedata) {
	if (!savedata->realVf || savedata->vf == savedata->realVf) {
		return;
	}
	enum GBASavedataType type = savedata->type;
	struct VFile* vf = savedata->vf;
	GBASavedataDeinit(savedata);
	savedata->vf = savedata->realVf;
	savedata->mapMode = MAP_WRITE;
	GBASavedataForceType(savedata, type);
	if (savedata->maskWriteback) {
		GBASavedataLoad(savedata, vf);
		savedata->maskWriteback = false;
	}
	vf->close(vf);
}

bool GBASavedataClone(struct GBASavedata* savedata, struct VFile* out) {
	if (savedata->data) {
		switch (savedata->type) {
		case GBA_SAVEDATA_SRAM:
			return out->write(out, savedata->data, GBA_SIZE_SRAM) == GBA_SIZE_SRAM;
		case GBA_SAVEDATA_SRAM512:
			return out->write(out, savedata->data, GBA_SIZE_SRAM512) == GBA_SIZE_SRAM512;
		case GBA_SAVEDATA_FLASH512:
			return out->write(out, savedata->data, GBA_SIZE_FLASH512) == GBA_SIZE_FLASH512;
		case GBA_SAVEDATA_FLASH1M:
			return out->write(out, savedata->data, GBASavedataSize(savedata)) == (ssize_t) GBASavedataSize(savedata);
		case GBA_SAVEDATA_EEPROM:
			return out->write(out, savedata->data, GBA_SIZE_EEPROM) == GBA_SIZE_EEPROM;
		case GBA_SAVEDATA_EEPROM512:
			return out->write(out, savedata->data, GBA_SIZE_EEPROM512) == GBA_SIZE_EEPROM512;
		case GBA_SAVEDATA_AUTODETECT:
		case GBA_SAVEDATA_FORCE_NONE:
			return true;
		}
	} else if (savedata->vf) {
		off_t read = 0;
		uint8_t buffer[2048];
		savedata->vf->seek(savedata->vf, 0, SEEK_SET);
		do {
			read = savedata->vf->read(savedata->vf, buffer, sizeof(buffer));
			out->write(out, buffer, read);
		} while (read == sizeof(buffer));
		return read >= 0;
	}
	return true;
}

size_t GBASavedataSize(const struct GBASavedata* savedata) {
	switch (savedata->type) {
	case GBA_SAVEDATA_SRAM:
		return GBA_SIZE_SRAM;
	case GBA_SAVEDATA_SRAM512:
		return GBA_SIZE_SRAM512;
	case GBA_SAVEDATA_FLASH512:
		return GBA_SIZE_FLASH512;
	case GBA_SAVEDATA_FLASH1M:
		return _flashCapacity(savedata);
	case GBA_SAVEDATA_EEPROM:
		return GBA_SIZE_EEPROM;
	case GBA_SAVEDATA_EEPROM512:
		return GBA_SIZE_EEPROM512;
	case GBA_SAVEDATA_FORCE_NONE:
		return 0;
	case GBA_SAVEDATA_AUTODETECT:
	default:
		if (savedata->vf) {
			return savedata->vf->size(savedata->vf);
		}
		return 0;
	}
}

bool GBASavedataLoad(struct GBASavedata* savedata, struct VFile* in) {
	if (savedata->data) {
		if (!in || savedata->type == GBA_SAVEDATA_FORCE_NONE) {
			return false;
		}
		if (savedata->type == GBA_SAVEDATA_FLASH1M) {
			// Grow only as far as the save being loaded needs (a 128KiB save, with or without the RTC footer, stays as it is)
			size_t tier = _flashTierForSize(in->size(in));
			if (tier > (savedata->flashExt ? savedata->flashExtSize : 0)) {
				_flashExtend(savedata, tier);
			}
		}
		ssize_t size = GBASavedataSize(savedata);
		in->seek(in, 0, SEEK_SET);
		if (savedata->flashExt) {
			// An older or smaller save fills the start; the rest stays erased
			ssize_t dataEnd = _flashExtDataEnd(in->size(in));
			if (dataEnd > size) {
				dataEnd = size;
			}
			if (dataEnd < size) {
				memset(&savedata->data[dataEnd], 0xFF, size - dataEnd);
			}
			return in->read(in, savedata->data, dataEnd) == dataEnd;
		}
		return in->read(in, savedata->data, size) == size;
	} else if (savedata->vf) {
		off_t read = 0;
		uint8_t buffer[2048];
		savedata->vf->seek(savedata->vf, 0, SEEK_SET);
		if (in) {
			in->seek(in, 0, SEEK_SET);
			do {
				read = in->read(in, buffer, sizeof(buffer));
				read = savedata->vf->write(savedata->vf, buffer, read);
			} while (read == sizeof(buffer));
		}
		memset(buffer, 0xFF, sizeof(buffer));
		ssize_t fsize = savedata->vf->size(savedata->vf);
		ssize_t pos = savedata->vf->seek(savedata->vf, 0, SEEK_CUR);
		while (fsize - pos >= (ssize_t) sizeof(buffer)) {
			savedata->vf->write(savedata->vf, buffer, sizeof(buffer));
			pos = savedata->vf->seek(savedata->vf, 0, SEEK_CUR);
		}
		if (fsize - pos > 0) {
			savedata->vf->write(savedata->vf, buffer, fsize - pos);
		}
		return read >= 0;
	}
	return true;
}

void GBASavedataForceType(struct GBASavedata* savedata, enum GBASavedataType type) {
	if (savedata->type == type) {
		return;
	}
	if (savedata->type != GBA_SAVEDATA_AUTODETECT) {
		struct VFile* vf = savedata->vf;
		int mapMode = savedata->mapMode;
		bool maskWriteback = savedata->maskWriteback;
		GBASavedataDeinit(savedata);
		GBASavedataInit(savedata, vf);
		savedata->mapMode = mapMode;
		savedata->maskWriteback = maskWriteback;
	}
	switch (type) {
	case GBA_SAVEDATA_FLASH512:
	case GBA_SAVEDATA_FLASH1M:
		savedata->type = type;
		GBASavedataInitFlash(savedata);
		return;
	case GBA_SAVEDATA_EEPROM:
	case GBA_SAVEDATA_EEPROM512:
		savedata->type = type;
		GBASavedataInitEEPROM(savedata);
		return;
	case GBA_SAVEDATA_SRAM:
		GBASavedataInitSRAM(savedata);
		return;
	case GBA_SAVEDATA_SRAM512:
		GBASavedataInitSRAM512(savedata);
		return;
	case GBA_SAVEDATA_FORCE_NONE:
		savedata->type = GBA_SAVEDATA_FORCE_NONE;
		break;
	case GBA_SAVEDATA_AUTODETECT:
		break;
	}
	mCALLBACKS_INVOKE(savedata->p, memoryBlocksChanged);
}

void GBASavedataInitFlash(struct GBASavedata* savedata) {
	if (savedata->type == GBA_SAVEDATA_AUTODETECT) {
		savedata->type = GBA_SAVEDATA_FLASH512;
	}
	if (savedata->type != GBA_SAVEDATA_FLASH512 && savedata->type != GBA_SAVEDATA_FLASH1M) {
		mLOG(GBA_SAVE, WARN, "Can't re-initialize savedata");
		return;
	}
	int32_t flashSize = GBA_SIZE_FLASH512;
	if (savedata->type == GBA_SAVEDATA_FLASH1M) {
		flashSize = GBA_SIZE_FLASH1M;
	}
	off_t end;
	savedata->flashExt = false;
	savedata->flashExtSize = 0;
	savedata->pendingBank = -1;
	if (!savedata->vf) {
		end = 0;
		savedata->data = anonymousMemoryMap(GBA_SIZE_FLASH_EXT);
		memset(&savedata->data[GBA_SIZE_FLASH1M], 0xFF, GBA_SIZE_FLASH_EXT - GBA_SIZE_FLASH1M);
	} else {
		end = savedata->vf->size(savedata->vf);
		if (savedata->type == GBA_SAVEDATA_FLASH1M && end >= GBA_SIZE_FLASH_EXT_6M) {
			// A save that already holds the extension: 6MB stays 6MB, 16MB is 16MB
			savedata->flashExt = true;
			savedata->flashExtSize = end >= GBA_SIZE_FLASH_EXT ? GBA_SIZE_FLASH_EXT : GBA_SIZE_FLASH_EXT_6M;
			flashSize = savedata->flashExtSize;
		}
		if (end < flashSize) {
			savedata->vf->truncate(savedata->vf, flashSize);
		}
		savedata->data = savedata->vf->map(savedata->vf, flashSize, savedata->mapMode);
		if (savedata->flashExt && end < flashSize) {
			memset(&savedata->data[end], 0xFF, flashSize - end);
		}
	}

	savedata->currentBank = savedata->data;
	if (end < GBA_SIZE_FLASH512) {
		memset(&savedata->data[end], 0xFF, flashSize - end);
	}
	if (savedata->type == GBA_SAVEDATA_FLASH1M && !savedata->flashExt && end >= GBA_SIZE_FLASH_EXT_1M) {
		// A save of the earlier 1MiB extension
		_flashExtend(savedata, GBA_SIZE_FLASH_EXT_6M);
	}

	mCALLBACKS_INVOKE(savedata->p, memoryBlocksChanged);
}

void GBASavedataInitEEPROM(struct GBASavedata* savedata) {
	if (savedata->type == GBA_SAVEDATA_AUTODETECT) {
		savedata->type = GBA_SAVEDATA_EEPROM512;
	} else if (savedata->type != GBA_SAVEDATA_EEPROM512 && savedata->type != GBA_SAVEDATA_EEPROM) {
		mLOG(GBA_SAVE, WARN, "Can't re-initialize savedata");
		return;
	}
	int32_t eepromSize = GBA_SIZE_EEPROM512;
	if (savedata->type == GBA_SAVEDATA_EEPROM) {
		eepromSize = GBA_SIZE_EEPROM;
	}
	off_t end;
	if (!savedata->vf) {
		end = 0;
		savedata->data = anonymousMemoryMap(GBA_SIZE_EEPROM);
	} else {
		end = savedata->vf->size(savedata->vf);
		if (end < eepromSize) {
			savedata->vf->truncate(savedata->vf, eepromSize);
		}
		savedata->data = savedata->vf->map(savedata->vf, eepromSize, savedata->mapMode);
	}
	if (end < GBA_SIZE_EEPROM512) {
		memset(&savedata->data[end], 0xFF, GBA_SIZE_EEPROM512 - end);
	}
	mCALLBACKS_INVOKE(savedata->p, memoryBlocksChanged);
}

void GBASavedataInitSRAM(struct GBASavedata* savedata) {
	if (savedata->type == GBA_SAVEDATA_AUTODETECT) {
		savedata->type = GBA_SAVEDATA_SRAM;
	} else {
		mLOG(GBA_SAVE, WARN, "Can't re-initialize savedata");
		return;
	}
	off_t end;
	if (!savedata->vf) {
		end = 0;
		savedata->data = anonymousMemoryMap(GBA_SIZE_SRAM);
	} else {
		end = savedata->vf->size(savedata->vf);
		if (end < GBA_SIZE_SRAM) {
			savedata->vf->truncate(savedata->vf, GBA_SIZE_SRAM);
		}
		savedata->data = savedata->vf->map(savedata->vf, GBA_SIZE_SRAM, savedata->mapMode);
	}

	if (end < GBA_SIZE_SRAM) {
		memset(&savedata->data[end], 0xFF, GBA_SIZE_SRAM - end);
	}
	mCALLBACKS_INVOKE(savedata->p, memoryBlocksChanged);
}

void GBASavedataInitSRAM512(struct GBASavedata* savedata) {
	if (savedata->type == GBA_SAVEDATA_AUTODETECT) {
		savedata->type = GBA_SAVEDATA_SRAM512;
	} else {
		mLOG(GBA_SAVE, WARN, "Can't re-initialize savedata");
		return;
	}
	off_t end;
	if (!savedata->vf) {
		end = 0;
		savedata->data = anonymousMemoryMap(GBA_SIZE_SRAM512);
	} else {
		end = savedata->vf->size(savedata->vf);
		if (end < GBA_SIZE_SRAM512) {
			savedata->vf->truncate(savedata->vf, GBA_SIZE_SRAM512);
		}
		savedata->data = savedata->vf->map(savedata->vf, GBA_SIZE_SRAM512, savedata->mapMode);
	}

	if (end < GBA_SIZE_SRAM512) {
		memset(&savedata->data[end], 0xFF, GBA_SIZE_SRAM512 - end);
	}
	mCALLBACKS_INVOKE(savedata->p, memoryBlocksChanged);
}

uint8_t GBASavedataReadFlash(struct GBASavedata* savedata, uint16_t address) {
	if (savedata->command == FLASH_COMMAND_ID) {
		if (savedata->type == GBA_SAVEDATA_FLASH512) {
			if (address < 2) {
				return FLASH_PANASONIC_MN63F805MNP >> (address * 8);
			}
		} else if (savedata->type == GBA_SAVEDATA_FLASH1M) {
			if (address < 2) {
				return FLASH_SANYO_LE26FV10N1TS >> (address * 8);
			}
		}
	}
	if (mTimingIsScheduled(&savedata->p->timing, &savedata->dust) && (address >> 12) == savedata->settling) {
		// This should read Q7 XOR data bit 7 (data# polling), Q6 flipping
		// every read (toggle bit), and /Q5 (error bit cleared), but implementing
		// just data# polling is sufficient for games to figure it out
		return (savedata->currentBank[address] ^ 0x80) & 0x80;
	}
	return savedata->currentBank[address];
}

void GBASavedataWriteFlash(struct GBASavedata* savedata, uint16_t address, uint8_t value) {
	switch (savedata->flashState) {
	case FLASH_STATE_RAW:
		switch (savedata->command) {
		case FLASH_COMMAND_PROGRAM:
			_flashMaterialize(savedata);
			if (savedata->pendingBank >= 0) {
				// The data could not grow: the write is lost
				savedata->command = FLASH_COMMAND_NONE;
				break;
			}
			savedata->dirty |= mSAVEDATA_DIRT_NEW;
			savedata->currentBank[address] = value;
			savedata->command = FLASH_COMMAND_NONE;
			mTimingDeschedule(&savedata->p->timing, &savedata->dust);
			mTimingSchedule(&savedata->p->timing, &savedata->dust, FLASH_PROGRAM_CYCLES);
			break;
		case FLASH_COMMAND_SWITCH_BANK:
			if (address == 0) { // banks 2 and up: flash extension (the bank number is one byte: up to 256 banks)
				_flashSwitchBank(savedata, value);
			} else {
				mLOG(GBA_SAVE, GAME_ERROR, "Bad flash bank switch");
				savedata->command = FLASH_COMMAND_NONE;
			}
			savedata->command = FLASH_COMMAND_NONE;
			break;
		default:
			if (address == FLASH_BASE_HI && value == FLASH_COMMAND_START) {
				savedata->flashState = FLASH_STATE_START;
			} else {
				mLOG(GBA_SAVE, GAME_ERROR, "Bad flash write: %#04x = %#02x", address, value);
			}
			break;
		}
		break;
	case FLASH_STATE_START:
		if (address == FLASH_BASE_LO && value == FLASH_COMMAND_CONTINUE) {
			savedata->flashState = FLASH_STATE_CONTINUE;
		} else {
			mLOG(GBA_SAVE, GAME_ERROR, "Bad flash write: %#04x = %#02x", address, value);
			savedata->flashState = FLASH_STATE_RAW;
		}
		break;
	case FLASH_STATE_CONTINUE:
		savedata->flashState = FLASH_STATE_RAW;
		if (address == FLASH_BASE_HI) {
			switch (savedata->command) {
			case FLASH_COMMAND_NONE:
				switch (value) {
				case FLASH_COMMAND_ERASE:
				case FLASH_COMMAND_ID:
				case FLASH_COMMAND_PROGRAM:
				case FLASH_COMMAND_SWITCH_BANK:
					savedata->command = value;
					break;
				default:
					mLOG(GBA_SAVE, GAME_ERROR, "Unsupported flash operation: %#02x", value);
					break;
				}
				break;
			case FLASH_COMMAND_ERASE:
				switch (value) {
				case FLASH_COMMAND_ERASE_CHIP:
					_flashErase(savedata);
					break;
				default:
					mLOG(GBA_SAVE, GAME_ERROR, "Unsupported flash erase operation: %#02x", value);
					break;
				}
				savedata->command = FLASH_COMMAND_NONE;
				break;
			case FLASH_COMMAND_ID:
				if (value == FLASH_COMMAND_TERMINATE) {
					savedata->command = FLASH_COMMAND_NONE;
				}
				break;
			default:
				mLOG(GBA_SAVE, ERROR, "Flash entered bad state: %#02x", savedata->command);
				savedata->command = FLASH_COMMAND_NONE;
				break;
			}
		} else if (savedata->command == FLASH_COMMAND_ERASE) {
			if (value == FLASH_COMMAND_ERASE_SECTOR) {
				_flashEraseSector(savedata, address);
				savedata->command = FLASH_COMMAND_NONE;
			} else {
				mLOG(GBA_SAVE, GAME_ERROR, "Unsupported flash erase operation: %#02x", value);
			}
		}
		break;
	}
}

static void _ensureEeprom(struct GBASavedata* savedata, uint32_t size) {
	if (size < GBA_SIZE_EEPROM512) {
		return;
	}
	if (savedata->type == GBA_SAVEDATA_EEPROM) {
		return;
	}
	savedata->type = GBA_SAVEDATA_EEPROM;
	if (!savedata->vf) {
		return;
	}
	savedata->vf->unmap(savedata->vf, savedata->data, GBA_SIZE_EEPROM512);
	if (savedata->vf->size(savedata->vf) < GBA_SIZE_EEPROM) {
		savedata->vf->truncate(savedata->vf, GBA_SIZE_EEPROM);
		savedata->data = savedata->vf->map(savedata->vf, GBA_SIZE_EEPROM, savedata->mapMode);
		memset(&savedata->data[GBA_SIZE_EEPROM512], 0xFF, GBA_SIZE_EEPROM - GBA_SIZE_EEPROM512);
	} else {
		savedata->data = savedata->vf->map(savedata->vf, GBA_SIZE_EEPROM, savedata->mapMode);
	}
	mCALLBACKS_INVOKE(savedata->p, memoryBlocksChanged);
}

void GBASavedataWriteEEPROM(struct GBASavedata* savedata, uint16_t value, uint32_t writeSize) {
	switch (savedata->command) {
	// Read header
	case EEPROM_COMMAND_NULL:
	default:
		savedata->command = value & 0x1;
		break;
	case EEPROM_COMMAND_PENDING:
		savedata->command <<= 1;
		savedata->command |= value & 0x1;
		if (savedata->command == EEPROM_COMMAND_WRITE) {
			savedata->writeAddress = 0;
		} else {
			savedata->readAddress = 0;
		}
		break;
	// Do commands
	case EEPROM_COMMAND_WRITE:
		// Write
		if (writeSize > 65) {
			savedata->writeAddress <<= 1;
			savedata->writeAddress |= (value & 0x1) << 6;
		} else if (writeSize == 1) {
			savedata->command = EEPROM_COMMAND_NULL;
		} else if ((savedata->writeAddress >> 3) < GBA_SIZE_EEPROM) {
			_ensureEeprom(savedata, savedata->writeAddress >> 3);
			uint8_t current = savedata->data[savedata->writeAddress >> 3];
			current &= ~(1 << (0x7 - (savedata->writeAddress & 0x7)));
			current |= (value & 0x1) << (0x7 - (savedata->writeAddress & 0x7));
			savedata->dirty |= mSAVEDATA_DIRT_NEW;
			savedata->data[savedata->writeAddress >> 3] = current;
			mTimingDeschedule(&savedata->p->timing, &savedata->dust);
			mTimingSchedule(&savedata->p->timing, &savedata->dust, EEPROM_SETTLE_CYCLES);
			++savedata->writeAddress;
		} else {
			mLOG(GBA_SAVE, GAME_ERROR, "Writing beyond end of EEPROM: %08X", (savedata->writeAddress >> 3));
		}
		break;
	case EEPROM_COMMAND_READ_PENDING:
		// Read
		if (writeSize > 1) {
			savedata->readAddress <<= 1;
			if (value & 0x1) {
				savedata->readAddress |= 0x40;
			}
		} else {
			savedata->readBitsRemaining = 68;
			savedata->command = EEPROM_COMMAND_READ;
		}
		break;
	}
}

uint16_t GBASavedataReadEEPROM(struct GBASavedata* savedata) {
	if (savedata->command != EEPROM_COMMAND_READ) {
		if (!mTimingIsScheduled(&savedata->p->timing, &savedata->dust)) {
			return 1;
		} else {
			return 0;
		}
	}
	--savedata->readBitsRemaining;
	if (savedata->readBitsRemaining < 64) {
		int step = 63 - savedata->readBitsRemaining;
		uint32_t address = (savedata->readAddress + step) >> 3;
		_ensureEeprom(savedata, address);
		if (address >= GBA_SIZE_EEPROM) {
			mLOG(GBA_SAVE, GAME_ERROR, "Reading beyond end of EEPROM: %08X", address);
			return 0xFF;
		}
		uint8_t data = savedata->data[address] >> (0x7 - (step & 0x7));
		if (!savedata->readBitsRemaining) {
			savedata->command = EEPROM_COMMAND_NULL;
		}
		return data & 0x1;
	}
	return 0;
}

void GBASavedataClean(struct GBASavedata* savedata, uint32_t frameCount) {
	if (!savedata->vf) {
		return;
	}
	if (mSavedataClean(&savedata->dirty, &savedata->dirtAge, frameCount)) {
		if (savedata->maskWriteback) {
			GBASavedataUnmask(savedata);
		}
		if (savedata->mapMode & MAP_WRITE) {
			size_t size = GBASavedataSize(savedata);
			if (savedata->data && savedata->vf->sync(savedata->vf, savedata->data, size)) {
				GBASavedataRTCWrite(savedata);
				mLOG(GBA_SAVE, INFO, "Savedata synced");
			} else {
				mLOG(GBA_SAVE, INFO, "Savedata failed to sync!");
			}
		}
	}
}

void GBASavedataRTCWrite(struct GBASavedata* savedata) {
	if (!(savedata->p->memory.hw.devices & HW_RTC) || !savedata->vf || savedata->mapMode == MAP_READ) {
		return;
	}

	struct GBASavedataRTCBuffer buffer;

	memcpy(&buffer.time, savedata->p->memory.hw.rtc.time, 7);
	buffer.control = savedata->p->memory.hw.rtc.control;
	STORE_64LE(savedata->p->memory.hw.rtc.lastLatch, 0, &buffer.lastLatch);

	size_t size = GBASavedataSize(savedata);
	savedata->vf->seek(savedata->vf, size & ~0xFF, SEEK_SET);

	size_t bankOffset = 0;
	bool virtualBank = savedata->pendingBank >= 0;
	if ((savedata->vf->size(savedata->vf) & 0xFF) != sizeof(buffer)) {
		// Writing past the end of the file can invalidate the file mapping
		if (savedata->type == GBA_SAVEDATA_FLASH1M && savedata->currentBank && !virtualBank) {
			bankOffset = savedata->currentBank - savedata->data;
		}
		savedata->vf->unmap(savedata->vf, savedata->data, size);
		savedata->data = NULL;
	}
	savedata->vf->write(savedata->vf, &buffer, sizeof(buffer));
	if (!savedata->data) {
		savedata->data = savedata->vf->map(savedata->vf, size, MAP_WRITE);
		if (savedata->type == GBA_SAVEDATA_FLASH1M) {
			if (!virtualBank) {
				savedata->currentBank = &savedata->data[bankOffset];
			}
		} else if (savedata->type == GBA_SAVEDATA_FLASH512) {
			savedata->currentBank = savedata->data;
		}
	}
}

static uint8_t _unBCD(uint8_t byte) {
	return (byte >> 4) * 10 + (byte & 0xF);
}

void GBASavedataRTCRead(struct GBASavedata* savedata) {
	if (!savedata->vf) {
		return;
	}

	struct GBASavedataRTCBuffer buffer;

	size_t size = GBASavedataSize(savedata) & ~0xFF;
	savedata->vf->seek(savedata->vf, size, SEEK_SET);
	size = savedata->vf->read(savedata->vf, &buffer, sizeof(buffer));
	if (size < sizeof(buffer)) {
		return;
	}

	memcpy(savedata->p->memory.hw.rtc.time, &buffer.time, 7);

	// Older FlashGBX sets this to 0x01 instead of the control flag.
	// Since that bit is invalid on hardware, we can check for != 0x01
	// to see if it's a valid value instead of just a filler value.
	if (buffer.control != 1) {
		savedata->p->memory.hw.rtc.control = buffer.control;
	}
	LOAD_64LE(savedata->p->memory.hw.rtc.lastLatch, 0, &buffer.lastLatch);

	time_t rtcTime;

#ifndef PSP2
	struct tm date;
	date.tm_year = _unBCD(savedata->p->memory.hw.rtc.time[0]) + 100;
	date.tm_mon = _unBCD(savedata->p->memory.hw.rtc.time[1]) - 1;
	date.tm_mday = _unBCD(savedata->p->memory.hw.rtc.time[2]);
	date.tm_hour = _unBCD(savedata->p->memory.hw.rtc.time[4]);
	date.tm_min = _unBCD(savedata->p->memory.hw.rtc.time[5]);
	date.tm_sec = _unBCD(savedata->p->memory.hw.rtc.time[6]);
	date.tm_isdst = -1;
	rtcTime = mktime(&date);
#else
	struct SceDateTime date;
	date.year = _unBCD(savedata->p->memory.hw.rtc.time[0]) + 2000;
	date.month = _unBCD(savedata->p->memory.hw.rtc.time[1]);
	date.day = _unBCD(savedata->p->memory.hw.rtc.time[2]);
	date.hour = _unBCD(savedata->p->memory.hw.rtc.time[4]);
	date.minute = _unBCD(savedata->p->memory.hw.rtc.time[5]);
	date.second = _unBCD(savedata->p->memory.hw.rtc.time[6]);
	date.microsecond = 0;

	struct SceRtcTick tick;
	int res;
	res = sceRtcConvertDateTimeToTick(&date, &tick);
	if (res < 0) {
		mLOG(GBA_SAVE, ERROR, "sceRtcConvertDateTimeToTick %lx", res);
	}
	res = sceRtcConvertLocalTimeToUtc(&tick, &tick);
	if (res < 0) {
		mLOG(GBA_SAVE, ERROR, "sceRtcConvertUtcToLocalTime %lx", res);
	}
	res = sceRtcConvertTickToDateTime(&tick, &date);
	if (res < 0) {
		mLOG(GBA_SAVE, ERROR, "sceRtcConvertTickToDateTime %lx", res);
	}
	res = sceRtcConvertDateTimeToTime_t(&date, &rtcTime);
	if (res < 0) {
		mLOG(GBA_SAVE, ERROR, "sceRtcConvertDateTimeToTime_t %lx", res);
	}
#endif

	savedata->p->memory.hw.rtc.offset = savedata->p->memory.hw.rtc.lastLatch - rtcTime;

	mLOG(GBA_SAVE, DEBUG, "Savegame time offset set to %li", savedata->p->memory.hw.rtc.offset);
}

void GBASavedataSerialize(const struct GBASavedata* savedata, struct GBASerializedState* state) {
	state->savedata.type = savedata->type;
	state->savedata.command = savedata->command;
	GBASerializedSavedataFlags flags = 0;
	flags = GBASerializedSavedataFlagsSetFlashState(flags, savedata->flashState);
	flags = GBASerializedSavedataFlagsTestFillFlashBank(flags, savedata->currentBank == &savedata->data[0x10000]);

	if (mTimingIsScheduled(&savedata->p->timing, &savedata->dust)) {
		STORE_32(savedata->dust.when - mTimingCurrentTime(&savedata->p->timing), 0, &state->savedata.settlingDust);
		flags = GBASerializedSavedataFlagsFillDustSettling(flags);
	}

	state->savedata.flags = flags;
	state->savedata.readBitsRemaining = savedata->readBitsRemaining;
	STORE_32(savedata->readAddress, 0, &state->savedata.readAddress);
	STORE_32(savedata->writeAddress, 0, &state->savedata.writeAddress);
	STORE_16(savedata->settling, 0, &state->savedata.settlingSector);
	uint16_t bank = 0;
	if (savedata->pendingBank >= 0) {
		bank = savedata->pendingBank;
	} else if (savedata->data && savedata->currentBank) {
		bank = (savedata->currentBank - savedata->data) >> 16;
	}
	STORE_16(bank >= 2 ? bank : 0, 0, &state->savedata.reserved);
}

void GBASavedataDeserialize(struct GBASavedata* savedata, const struct GBASerializedState* state) {
	if (savedata->type != state->savedata.type) {
		mLOG(GBA_SAVE, DEBUG, "Switching save types");
		GBASavedataForceType(savedata, state->savedata.type);
	}
	savedata->command = state->savedata.command;
	GBASerializedSavedataFlags flags = state->savedata.flags;
	savedata->flashState = GBASerializedSavedataFlagsGetFlashState(flags);
	savedata->readBitsRemaining = state->savedata.readBitsRemaining;
	LOAD_32(savedata->readAddress, 0, &state->savedata.readAddress);
	LOAD_32(savedata->writeAddress, 0, &state->savedata.writeAddress);
	LOAD_16(savedata->settling, 0, &state->savedata.settlingSector);

	if (savedata->type == GBA_SAVEDATA_FLASH1M) {
		uint16_t bank;
		LOAD_16(bank, 0, &state->savedata.reserved);
		_flashSwitchBank(savedata, bank >= 2 && bank < GBA_FLASH_EXT_BANKS ? bank : GBASerializedSavedataFlagsGetFlashBank(flags));
	}

	if (GBASerializedSavedataFlagsIsDustSettling(flags)) {
		uint32_t when;
		LOAD_32(when, 0, &state->savedata.settlingDust);
		mTimingSchedule(&savedata->p->timing, &savedata->dust, when);
	}
}

static void _flashSwitchBank(struct GBASavedata* savedata, int bank) {
	mLOG(GBA_SAVE, DEBUG, "Performing flash bank switch to bank %i", bank);
	if (bank > 0 && savedata->type == GBA_SAVEDATA_FLASH512) {
		mLOG(GBA_SAVE, INFO, "Updating flash chip from 512kb to 1Mb");
		savedata->type = GBA_SAVEDATA_FLASH1M;
		if (savedata->vf) {
			savedata->vf->unmap(savedata->vf, savedata->data, GBA_SIZE_FLASH512);
			if (savedata->vf->size(savedata->vf) < GBA_SIZE_FLASH1M) {
				savedata->vf->truncate(savedata->vf, GBA_SIZE_FLASH1M);
				savedata->data = savedata->vf->map(savedata->vf, GBA_SIZE_FLASH1M, MAP_WRITE);
				memset(&savedata->data[GBA_SIZE_FLASH512], 0xFF, GBA_SIZE_FLASH512);
			} else {
				savedata->data = savedata->vf->map(savedata->vf, GBA_SIZE_FLASH1M, MAP_WRITE);
			}
		}
		mCALLBACKS_INVOKE(savedata->p, memoryBlocksChanged);
	}
	if (bank >= GBA_FLASH_EXT_BANKS) {
		bank &= 1;
	}
	if (bank >= 2 && savedata->type == GBA_SAVEDATA_FLASH1M && ((size_t) bank << 16) >= _flashCapacity(savedata)) {
		// Beyond the data held: reads see erased flash, and the save grows on the first write
		memset(_erasedBank, 0xFF, sizeof(_erasedBank));
		savedata->pendingBank = bank;
		savedata->currentBank = _erasedBank;
		return;
	}
	if (bank >= 2 && savedata->type != GBA_SAVEDATA_FLASH1M) {
		bank &= 1;
	}
	savedata->pendingBank = -1;
	savedata->currentBank = &savedata->data[(size_t) bank << 16];
}

// Size of the flash data held now
static size_t _flashCapacity(const struct GBASavedata* savedata) {
	return savedata->flashExt ? savedata->flashExtSize : GBA_SIZE_FLASH1M;
}

// Extension size a save file of the given size holds (0: none; a 1MiB file is brought up to 6MB)
static size_t _flashTierForSize(off_t end) {
	if (end >= GBA_SIZE_FLASH_EXT) {
		return GBA_SIZE_FLASH_EXT;
	}
	if (end >= GBA_SIZE_FLASH_EXT_1M) {
		return GBA_SIZE_FLASH_EXT_6M;
	}
	return 0;
}

// End of the flash data held in a save file of the given size (the RTC footer may follow it)
static size_t _flashExtDataEnd(off_t end) {
	if (end >= GBA_SIZE_FLASH_EXT) {
		return GBA_SIZE_FLASH_EXT;
	}
	if (end >= GBA_SIZE_FLASH_EXT_6M) {
		return GBA_SIZE_FLASH_EXT_6M;
	}
	if (end >= GBA_SIZE_FLASH_EXT_1M) {
		return GBA_SIZE_FLASH_EXT_1M;
	}
	return end >= GBA_SIZE_FLASH1M ? GBA_SIZE_FLASH1M : (size_t) end;
}

// The game writes to a bank beyond the data held: grow the save to hold it
static void _flashMaterialize(struct GBASavedata* savedata) {
	if (savedata->pendingBank < 0) {
		return;
	}
	_flashExtend(savedata, savedata->pendingBank < GBA_FLASH_EXT_6M_BANKS ? GBA_SIZE_FLASH_EXT_6M : GBA_SIZE_FLASH_EXT);
}

// Flash extension: grow the flash to the target size (6MB = 96 banks, or 16MB = 256 banks of 64KB).
// Real carts only have banks 0 and 1, so a game only uses banks 2 and up on this build of mGBA.
// The existing data is never rewritten: the new part is appended as erased flash first and the RTC footer
// is moved last, so a stop in the middle still leaves the old save readable.
static bool _flashExtend(struct GBASavedata* savedata, size_t target) {
	if (savedata->type != GBA_SAVEDATA_FLASH1M) {
		return false;
	}
	size_t current = savedata->flashExt ? savedata->flashExtSize : 0;
	if (target <= current) {
		return true;
	}
	mLOG(GBA_SAVE, INFO, "Extending flash to %uMB", (unsigned) (target >> 20));
	size_t oldSize = _flashCapacity(savedata);
	size_t bankOffset = (savedata->pendingBank < 0 && savedata->currentBank) ? savedata->currentBank - savedata->data : 0;
	if (savedata->vf) {
		off_t end = savedata->vf->size(savedata->vf);
		size_t dataEnd = _flashExtDataEnd(end);
		uint8_t footer[sizeof(struct GBASavedataRTCBuffer)];
		uint8_t erased[4096];
		memset(erased, 0xFF, sizeof(erased));
		bool hasFooter = false;
		bool ok = true;
		savedata->vf->unmap(savedata->vf, savedata->data, oldSize);
		if (end < (off_t) target) {
			if ((size_t) end == dataEnd + sizeof(footer)) {
				// The RTC footer sits right after the flash data
				savedata->vf->seek(savedata->vf, dataEnd, SEEK_SET);
				hasFooter = savedata->vf->read(savedata->vf, footer, sizeof(footer)) == sizeof(footer);
			}
			// 1. Append erased flash up to the new size (the old footer stays where it is)
			savedata->vf->seek(savedata->vf, end, SEEK_SET);
			for (off_t pos = end; ok && pos < (off_t) target;) {
				size_t chunk = target - pos < sizeof(erased) ? target - pos : sizeof(erased);
				ok = savedata->vf->write(savedata->vf, erased, chunk) == (ssize_t) chunk;
				pos += chunk;
			}
			// 2. The footer goes after the new end
			if (ok && hasFooter) {
				ok = savedata->vf->write(savedata->vf, footer, sizeof(footer)) == (ssize_t) sizeof(footer);
			}
			// 3. The old footer position becomes erased flash
			if (ok && (size_t) end > dataEnd) {
				savedata->vf->seek(savedata->vf, dataEnd, SEEK_SET);
				ok = savedata->vf->write(savedata->vf, erased, end - dataEnd) == end - (off_t) dataEnd;
			}
		}
		if (ok) {
			savedata->data = savedata->vf->map(savedata->vf, target, savedata->mapMode);
			ok = savedata->data != NULL;
		}
		if (!ok) {
			mLOG(GBA_SAVE, WARN, "Could not extend flash");
			savedata->data = savedata->vf->map(savedata->vf, oldSize, savedata->mapMode);
			if (savedata->pendingBank < 0 && savedata->data) {
				savedata->currentBank = &savedata->data[bankOffset];
			}
			return false;
		}
	}
	savedata->flashExt = true;
	savedata->flashExtSize = target;
	if (savedata->pendingBank >= 0 && ((size_t) savedata->pendingBank << 16) < target) {
		bankOffset = (size_t) savedata->pendingBank << 16;
		savedata->pendingBank = -1;
	}
	if (savedata->pendingBank < 0) {
		savedata->currentBank = &savedata->data[bankOffset];
	}
	savedata->dirty |= mSAVEDATA_DIRT_NEW;
	mCALLBACKS_INVOKE(savedata->p, memoryBlocksChanged);
	return true;
}

void _flashErase(struct GBASavedata* savedata) {
	mLOG(GBA_SAVE, DEBUG, "Performing flash chip erase");
	savedata->dirty |= mSAVEDATA_DIRT_NEW;
	size_t size = GBA_SIZE_FLASH512;
	if (savedata->type == GBA_SAVEDATA_FLASH1M) {
		size = _flashCapacity(savedata);
	}
	memset(savedata->data, 0xFF, size);
}

void _flashEraseSector(struct GBASavedata* savedata, uint16_t sectorStart) {
	mLOG(GBA_SAVE, DEBUG, "Performing flash sector erase at 0x%04x", sectorStart);
	savedata->dirty |= mSAVEDATA_DIRT_NEW;
	size_t size = 0x1000;
	if (savedata->type == GBA_SAVEDATA_FLASH1M) {
		mLOG(GBA_SAVE, DEBUG, "Performing unknown sector-size erase at 0x%04x", sectorStart);
	}
	savedata->settling = sectorStart >> 12;
	mTimingDeschedule(&savedata->p->timing, &savedata->dust);
	mTimingSchedule(&savedata->p->timing, &savedata->dust, FLASH_ERASE_CYCLES);
	if (savedata->pendingBank < 0) {
		// A bank beyond the data held is already erased
		memset(&savedata->currentBank[sectorStart & ~(size - 1)], 0xFF, size);
	}
}

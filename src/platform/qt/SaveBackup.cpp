/* Automatic save game backups (see SaveBackup.h).
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "SaveBackup.h"

#include "CoreController.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>

#include <cstdlib>

#include <mgba/core/config.h>
#include <mgba/core/core.h>
#include <mgba/core/interface.h>
#include <mgba/core/thread.h>

using namespace QGBA;

// CoreController::path() can be just the file name; baseDirectory() is the folder it was opened from
QString SaveBackup::backupDir(const CoreController* controller) {
	QFileInfo rom(QDir(controller->baseDirectory()).filePath(controller->path()));
	return rom.absolutePath() + "/backups/" + rom.completeBaseName();
}

void SaveBackup::hookController(std::shared_ptr<CoreController> controller) {
	mCore* core = controller->thread()->core;
	if (!core || controller->path().isEmpty()) {
		return;
	}
	int enabled = 1;
	mCoreConfigGetIntValue(&core->config, "saveBackup", &enabled);
	if (!enabled) {
		return;
	}
	// Owned by the controller: deleted after the core is torn down
	SaveBackup* backup = new SaveBackup(controller.get(), core);
	mCoreCallbacks callbacks{};
	callbacks.context = backup;
	callbacks.savedataUpdated = &SaveBackup::savedataUpdated;
	core->addCoreCallbacks(core, &callbacks);
}

SaveBackup::SaveBackup(CoreController* controller, mCore* core)
	: QObject(controller)
	, m_core(core)
	, m_dir(backupDir(controller))
{
}

// Emulation thread: the save data has just been written out
void SaveBackup::savedataUpdated(void* context) {
	SaveBackup* self = static_cast<SaveBackup*>(context);
	void* sram = nullptr;
	size_t size = self->m_core->savedataClone(self->m_core, &sram);
	if (!size || !sram) {
		return;
	}
	QByteArray data(static_cast<const char*>(sram), static_cast<int>(size));
	free(sram);
	QMetaObject::invokeMethod(self, [self, data]() {
		self->store(data);
	}, Qt::QueuedConnection);
}

// UI thread
void SaveBackup::store(const QByteArray& data) {
	QDir dir(m_dir);
	if (!dir.mkpath(".")) {
		return;
	}
	QStringList files = dir.entryList({"*.sav"}, QDir::Files, QDir::Name);
	if (!files.isEmpty()) {
		QFile newest(dir.filePath(files.last()));
		if (newest.size() == data.size() && newest.open(QIODevice::ReadOnly)) {
			bool same = newest.readAll() == data;
			newest.close();
			if (same) {
				return;
			}
		}
		// The name holds the time the file was started: writes within MERGE_SECONDS of it replace its contents,
		// so at most one backup is kept per MERGE_SECONDS
		QDateTime started = QDateTime::fromString(files.last().left(15), "yyyyMMdd-HHmmss");
		if (started.isValid() && started.secsTo(QDateTime::currentDateTime()) < MERGE_SECONDS) {
			QFile again(dir.filePath(files.last()));
			if (again.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
				again.write(data);
			}
			return;
		}
	}
	QString base = QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss");
	QString name = base + ".sav";
	for (int i = 1; dir.exists(name); ++i) {
		name = QString("%1-%2.sav").arg(base).arg(i);
	}
	QFile out(dir.filePath(name));
	if (!out.open(QIODevice::WriteOnly) || out.write(data) != data.size()) {
		out.remove();
		return;
	}
	out.close();
	files.append(name);
	// a big save (8MB or more) keeps fewer generations so the backup folder stays small
	int limit = data.size() >= LARGE_SAVE_SIZE ? MAX_BACKUPS_LARGE : MAX_BACKUPS;
	while (files.size() > limit) {
		dir.remove(files.takeFirst());
	}
}

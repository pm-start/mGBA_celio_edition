/* Automatic save game backups.
 *
 * Every time the game finishes writing its save data, a copy is kept in
 * "backups/<ROM name>/<date-time>.sav" next to the ROM: at most one file per 5 minutes
 * (later writes in that window replace it) and the newest 30 files (10 files for saves of 8MB or more). Config key "saveBackup" (default 1, 0 = off).
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

#include <memory>

struct mCore;

namespace QGBA {

class CoreController;

class SaveBackup : public QObject {
Q_OBJECT

public:
	static constexpr int MAX_BACKUPS = 30;
	static constexpr int MAX_BACKUPS_LARGE = 10;
	static constexpr int LARGE_SAVE_SIZE = 0x800000;
	static constexpr int MERGE_SECONDS = 5 * 60;

	static void hookController(std::shared_ptr<CoreController> controller);
	static QString backupDir(const CoreController* controller);

private:
	SaveBackup(CoreController* controller, mCore* core);

	static void savedataUpdated(void* context);
	void store(const QByteArray& data);

	mCore* m_core;
	QString m_dir;
};

}

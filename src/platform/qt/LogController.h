/* Copyright (c) 2013-2015 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#pragma once

#include "GBAApp.h"
#include "Log.h"

#include <mgba/core/log.h>

#include <QFile>
#include <QLoggingCategory>
#include <QObject>
#include <QStringList>
#include <QTextStream>
#include <memory>

namespace QGBA {

class ConfigController;

class LogController : public QObject, public Log {
Q_OBJECT

public:
	LogController(int levels, QObject* parent = nullptr);
	~LogController();

	int levels() const { return m_filter.defaultLevels; }
	int levels(int category) const;
	mLogFilter* filter() { return &m_filter; }

	static LogController* global();
	static QtMessageHandler installMessageHandler();
	static QString toString(int level);
	static int categoryId(const char*);

	void load(const ConfigController*);
	void save(ConfigController*) const;

	// Log files: normal = FATAL+ERROR+WARN only, debug = every level.
	// Debug is on with --log-debug, the environment variable MGBA_DEBUG_LOG (not empty, not 0) or the config key debugLog=1.
	bool debugMode() const { return m_debug; }
	static void requestDebug();
	static bool debugRequested(const ConfigController* = nullptr);
	static QString defaultLogFile();

signals:
	void logPosted(int level, int category, const QString& log);
	void levelsSet(int levels);
	void levelsEnabled(int levels);
	void levelsDisabled(int levels);
	void levelsSet(int levels, int category);
	void levelsEnabled(int levels, int category);
	void levelsDisabled(int levels, int category);

public slots:
	void postLog(int level, int category, const QString& string) override;
	void setLevels(int levels);
	void enableLevels(int levels);
	void disableLevels(int levels);
	void setLevels(int levels, int category);
	void enableLevels(int levels, int category);
	void disableLevels(int levels, int category);
	void clearLevels(int category);

	void logToFile(bool);
	void logToStdout(bool);
	void setLogFile(const QString&);

private:
	mLogFilter m_filter;
	bool m_logToFile = false;
	bool m_logToStdout = false;
	bool m_debug = false;
	int m_fileLevels = mLOG_WARN | mLOG_ERROR | mLOG_FATAL;
	QString m_logPath;
	std::unique_ptr<QFile> m_logFile;
	std::unique_ptr<QTextStream> m_logStream;

	static LogController s_global;
	static bool s_debugArg;
	static int s_qtCat;
};

}

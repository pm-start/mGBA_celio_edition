/* Copyright (c) 2013-2015 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "LogController.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QMessageBox>
#include <QMutexLocker>
#include <QTextStream>
#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
#include <QStringConverter>
#endif

#include "ConfigController.h"

using namespace QGBA;

#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
using Qt::endl;
#endif

LogController LogController::s_global(mLOG_ALL);
int LogController::s_qtCat{-1};
bool LogController::s_debugArg = false;

static const qint64 LOG_LIMIT_NORMAL = 1 * 1024 * 1024;
static const qint64 LOG_LIMIT_DEBUG = 16 * 1024 * 1024;

static void logMessageHandler(QtMsgType msgType, const QMessageLogContext&, const QString& msg) {
	switch (msgType) {
	case QtDebugMsg:
		LOG(QT, DEBUG) << msg;
		break;
	case QtInfoMsg:
		LOG(QT, INFO) << msg;
		break;
	case QtCriticalMsg:
		LOG(QT, ERROR) << msg;
		break;
	case QtFatalMsg:
		LOG(QT, FATAL) << msg;
		break;
	case QtWarningMsg:
	default:
		LOG(QT, WARN) << msg;
		break;
	}
}

QtMessageHandler LogController::installMessageHandler() {
	return qInstallMessageHandler(logMessageHandler);
}

LogController::LogController(int levels, QObject* parent)
	: QObject(parent)
{
	mLogFilterInit(&m_filter);
	mLogFilterSet(&m_filter, "gba.bios", mLOG_STUB | mLOG_FATAL);
	mLogFilterSet(&m_filter, "core.status", mLOG_ALL & ~mLOG_DEBUG);
	m_filter.defaultLevels = levels;
	s_qtCat = mLogCategoryById("platform.qt");

	if (this == &s_global) {
		setDefaultTarget(this);
	} else {
		connect(this, static_cast<void (LogController::*)(int)>(&LogController::levelsSet), &s_global, static_cast<void (LogController::*)(int)>(&LogController::setLevels));
		connect(this, static_cast<void (LogController::*)(int)>(&LogController::levelsEnabled), &s_global, static_cast<void (LogController::*)(int)>(&LogController::enableLevels));
		connect(this, static_cast<void (LogController::*)(int)>(&LogController::levelsDisabled), &s_global, static_cast<void (LogController::*)(int)>(&LogController::disableLevels));
	}
}

LogController::~LogController() {
	mLogFilterDeinit(&m_filter);
}

int LogController::levels(int category) const {
	return mLogFilterLevels(&m_filter, category);
}

void LogController::requestDebug() {
	s_debugArg = true;
}

bool LogController::debugRequested(const ConfigController* config) {
	if (s_debugArg) {
		return true;
	}
	QString env = qEnvironmentVariable("MGBA_DEBUG_LOG");
	if (!env.isEmpty() && env != QLatin1String("0")) {
		return true;
	}
	if (config) {
		QVariant value = config->getOption("debugLog");
		if (!value.isNull() && value.toString() != QLatin1String("0") && !value.toString().isEmpty()) {
			return true;
		}
	}
	return false;
}

QString LogController::defaultLogFile() {
	QString dir = qEnvironmentVariable("MGBA_LOG_DIR");
	if (dir.isEmpty()) {
		dir = QCoreApplication::applicationDirPath() + QLatin1String("/logs");
	}
	return QDir(dir).absoluteFilePath(QLatin1String("mGBA.log"));
}

void LogController::load(const ConfigController* config) {
	mLogFilterLoad(&m_filter, config->config());
	if (!levels(mLogCategoryById("gba.bios"))) {
		mLogFilterSet(&m_filter, "gba.bios", mLOG_STUB | mLOG_FATAL);
	}
	if (!levels(mLogCategoryById("core.status"))) {
		mLogFilterSet(&m_filter, "core.status", mLOG_ALL & ~mLOG_DEBUG);
	}
	m_debug = debugRequested(config);
	m_fileLevels = m_debug ? mLOG_ALL : (mLOG_WARN | mLOG_ERROR | mLOG_FATAL);
	if (m_debug) {
		// every level, including the game's debug register / AGBPrint (INFO, DEBUG), STUB and GAME_ERROR
		mLogFilterSet(&m_filter, "gba.bios", mLOG_ALL);
		mLogFilterSet(&m_filter, "core.status", mLOG_ALL);
		setLevels(mLOG_ALL);
	}
	// MGBA_LOG_DIR beats the config; with no setting the log goes to <exe folder>/logs/mGBA.log
	QString logFile = config->getOption("logFile");
	if (!qEnvironmentVariable("MGBA_LOG_DIR").isEmpty()) {
		logFile.clear();
	}
	setLogFile(logFile);
	logToStdout(config->getOption("logToStdout").toInt());
	QVariant toFile = config->getOption("logToFile");
	logToFile(toFile.isNull() || toFile.toString().isEmpty() ? true : toFile.toInt());
	if (this == &s_global && m_logToFile && m_logStream) {
		*m_logStream << QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"))
		             << QStringLiteral(" [SESSION] mGBA started, log mode: ")
		             << (m_debug ? QStringLiteral("debug (all levels)") : QStringLiteral("normal (FATAL+ERROR+WARN)")) << endl;
	}
}

void LogController::save(ConfigController* config) const {
	mLogFilterSave(&m_filter, config->config());
}

void LogController::postLog(int level, int category, const QString& string) {
	if (!mLogFilterTest(&m_filter, category, static_cast<mLogLevel>(level))) {
		return;
	}
	if (m_logToStdout || m_logToFile) {
		QString line = tr("[%1] %2: %3").arg(LogController::toString(level)).arg(mLogCategoryName(category)).arg(string);

		if (m_logToStdout) {
			QTextStream out(stdout);
			out << line << endl;
		}
		// Log::Stream posts to the global logger from whichever thread logged (audio, network), so serialize file writes
		QMutexLocker locker(&m_fileLock);
		if (m_logToFile && m_logStream && (level & m_fileLevels)) {
			*m_logStream << QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")) << QLatin1Char(' ') << line << endl;
		}
	}
	if (category == s_qtCat && level == mLOG_ERROR && this == &s_global) {
		QMessageBox* dialog = new QMessageBox(QMessageBox::Critical, tr("An error occurred"), string, QMessageBox::Ok);
		dialog->setAttribute(Qt::WA_DeleteOnClose);
		dialog->show();
	}
	emit logPosted(level, category, string);
}

void LogController::setLevels(int levels) {
	m_filter.defaultLevels = levels;
	emit levelsSet(levels);
}

void LogController::enableLevels(int levels) {
	m_filter.defaultLevels |= levels;
	emit levelsEnabled(levels);
}

void LogController::disableLevels(int levels) {
	m_filter.defaultLevels &= ~levels;
	emit levelsDisabled(levels);
}

void LogController::setLevels(int levels, int category) {
	auto id = mLogCategoryId(category);
	mLogFilterSet(&m_filter, id, levels);
	emit levelsSet(levels, category);
}

void LogController::enableLevels(int levels, int category) {
	auto id = mLogCategoryId(category);
	int newLevels = mLogFilterLevels(&m_filter, category) | levels;
	mLogFilterSet(&m_filter, id, newLevels);
	emit levelsEnabled(levels, category);
}

void LogController::disableLevels(int levels, int category) {
	auto id = mLogCategoryId(category);
	int newLevels = mLogFilterLevels(&m_filter, category) & ~levels;
	mLogFilterSet(&m_filter, id, newLevels);
	emit levelsDisabled(levels, category);
}

void LogController::clearLevels(int category) {
	auto id = mLogCategoryId(category);
	mLogFilterReset(&m_filter, id);
}

void LogController::logToFile(bool log) {
	m_logToFile = log;
}

void LogController::logToStdout(bool log) {
	m_logToStdout = log;
}

void LogController::setLogFile(const QString& file) {
	QMutexLocker locker(&m_fileLock);
	m_logStream.reset();
	m_logFile.reset();
	QString path = file.isEmpty() ? defaultLogFile() : file;
	m_logPath = path;
	QDir().mkpath(QFileInfo(path).absolutePath());
	// one generation of rotation at open time: mGBA.log -> mGBA.log.1 when over the limit (1MB, 16MB in debug mode)
	QFileInfo info(path);
	if (info.exists() && info.size() > (m_debug ? LOG_LIMIT_DEBUG : LOG_LIMIT_NORMAL)) {
		QFile::remove(path + QStringLiteral(".1"));
		QFile::rename(path, path + QStringLiteral(".1"));
	}
	m_logFile = std::make_unique<QFile>(path);
	if (!m_logFile->open(QIODevice::Append | QIODevice::Text)) {
		m_logFile.reset();
		return;
	}
	m_logStream = std::make_unique<QTextStream>(m_logFile.get());
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
	m_logStream->setCodec("UTF-8");
#else
	m_logStream->setEncoding(QStringConverter::Utf8);
#endif
}

LogController* LogController::global() {
	return &s_global;
}

QString LogController::toString(int level) {
	switch (level) {
	case mLOG_DEBUG:
		return tr("DEBUG");
	case mLOG_STUB:
		return tr("STUB");
	case mLOG_INFO:
		return tr("INFO");
	case mLOG_WARN:
		return tr("WARN");
	case mLOG_ERROR:
		return tr("ERROR");
	case mLOG_FATAL:
		return tr("FATAL");
	case mLOG_GAME_ERROR:
		return tr("GAME ERROR");
	}
	return QString();
}

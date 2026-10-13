/* Dialog for the built-in Celio net link (see CelioNet.h).
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#pragma once

#include <QDialog>
#include <QTimer>

#include <memory>
#include <string>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;

namespace QGBA {

class ConfigController;
class CoreController;
class Window;

class CelioNetView : public QDialog {
Q_OBJECT

public:
	CelioNetView(Window* window, QWidget* parent = nullptr);

	// MGBA_CELIO_NET=create | join:<room> starts the link once the game runs (for scripts and tests)
	static void hookController(std::shared_ptr<CoreController> controller, ConfigController* config);

private slots:
	void create();
	void join();
	void disconnectLink();
	void copyRoom();
	void refresh();
	void refreshPorts();
	void direct();
	void resetServer();

private:
	static std::string usbPortFromEnv();
	static bool parseServer(const QString& text, bool tls, std::string& host, int& port, bool& outTls);
	static bool savedTls(ConfigController* config);
	static bool applySavedServer(ConfigController* config);
	std::string selectedPort() const;
	bool applyServer();

	Window* m_window;
	QLabel* m_status;
	QLabel* m_room;
	QLineEdit* m_roomInput;
	QPushButton* m_create;
	QPushButton* m_join;
	QPushButton* m_disconnect;
	QPushButton* m_copy;
	QComboBox* m_ports;
	QPushButton* m_refreshPorts;
	QPushButton* m_direct;
	QLineEdit* m_server;
	QCheckBox* m_wss;
	QPushButton* m_serverReset;
	QString m_serverError;
	QCheckBox* m_useUsb;
	QTimer m_timer;
};

}

/* Net link over the public Celio relay, built into the Qt frontend (Windows only).
 *
 * Replaces the two pieces that used to be needed next to mGBA celio edition:
 *  - the Celio-mGBA-Link Lua script (emulated Celio device on the SIO port)
 *  - the Celio web page (Socket.IO client that talks to the relay server)
 * The device logic is a straight port of celio_device.lua (Celio-mGBA-Link 0.2) so that
 * both sides behave the same as the script + page combination.
 *
 * A real GBA can take part through a GBLink / Celio USB adapter (RP2040, CDC-ACM serial,
 * "GB" framed packets as in Celio-Client's LinkDeviceService):
 *  - Direct: this emulator and the real GBA, no server. The relay's session logic
 *    (Celio-Server session.ts) runs here between the two devices.
 *  - NetUsb: the adapter takes the place of the emulated device in a relay room
 *    (what the Celio web page's online link does).
 *  - FakeAdapter (tests): the emulated device answers on a TCP port the way the adapter
 *    firmware does, so Direct / NetUsb can be tried without the hardware.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

struct GBA;
struct mCore;
struct mSioMask;

namespace QGBA {

class CoreController;
class CelioWebSocket;
class CelioPort;

class CelioNet {
public:
	enum class Kind {
		Net,
		NetUsb,
		Direct,
		FakeAdapter,
	};

	struct SerialPortInfo {
		std::string name; // COM5
		std::string description;
		bool adapter; // USB VID 0x2FE3 (GBLink / Celio)
	};
	static std::vector<SerialPortInfo> listSerialPorts();

	enum class State {
		Idle,
		Connecting,
		WaitingPartner,
		Linking,
		Connected,
		Finished,
		Error,
	};

	struct Snapshot {
		State state;
		std::string room;
		std::string message;
		bool active;
		Kind kind;
	};

	static CelioNet* instance();

	// GUI thread. room is empty to create a new room.
	bool start(std::shared_ptr<CoreController> controller, const std::string& room);
	// port: "COM5", or "tcp:host:port" (a FakeAdapter, for tests)
	bool startDirect(std::shared_ptr<CoreController> controller, const std::string& port);
	bool startNetUsb(const std::string& port, const std::string& room);
	bool startFakeAdapter(std::shared_ptr<CoreController> controller, int listenPort);
	// touchCore = false when the core is going away (CoreController::stopping)
	void stop(bool touchCore = true);
	Snapshot snapshot();

	// Core thread entry points (registered on the core)
	static void sioReadHook(void* context);
	static void vblankHook(void* context);
	static void timer3Hook(void* context);

	// Server to use for the next start (empty host = built-in default). Copied at start.
	void setServer(const std::string& host, int port, bool secure, const char* source = "dialog");
	bool serverConfigured();
	void failStart(const std::string& message, const std::string& logLine);

private:
	CelioNet() = default;

	struct Device;
	struct Outgoing {
		bool isStatus;
		uint16_t status;
		std::vector<uint16_t> data;
	};
	struct Incoming {
		bool isCommand;
		uint16_t command;
		std::vector<uint16_t> data;
	};

	bool startCommon(Kind kind, std::shared_ptr<CoreController> controller, const std::string& room, const std::string& port, int listenPort);

	// core side
	void attachCore();
	void detachCore();
	void drainIncoming();
	void emitStatus(uint16_t status);
	void emitData(const std::vector<uint16_t>& data);
	void setSioMask(uint16_t mask);

	// network side
	void netThread();
	void workThread();
	bool connectOnce(bool first);
	void handleMessage(const std::string& msg);
	void handleEvent(const std::string& json, long ackId);
	void handleAck(long id, const std::string& json);
	bool sendText(const std::string& text);
	void startLinkMode();
	void pushIncoming(Incoming in);
	void deliverData(long sequence, std::vector<uint16_t> data);
	void queueOutgoing(Outgoing item);
	void noteStatus(uint16_t status);

	// USB adapter side
	void portThread();
	void handleFrame(uint8_t channel, const std::vector<uint8_t>& payload);
	bool portSend(uint8_t channel, const uint8_t* data, size_t size);
	bool portCommand(uint16_t command);
	bool portData(const std::vector<uint16_t>& data);
	void portStatus(uint16_t status);
	// Direct: Celio-Server session.ts between the emulated device (0) and the adapter (1)
	void sessionStatus(int who, uint16_t status);
	void sessionCommand(int who, uint16_t command);

	void setState(State state, const std::string& message);
	void setMessage(const std::string& message);
	void log(const std::string& line);

	std::mutex m_stateLock;
	State m_state = State::Idle;
	std::string m_room;
	std::string m_message;

	std::shared_ptr<CoreController> m_controller;
	GBA* m_gba = nullptr;
	mSioMask* m_oldMask = nullptr;
	std::unique_ptr<mSioMask> m_mask;
	std::atomic<bool> m_attached{false};
	std::atomic<bool> m_deviceOn{false};
	std::unique_ptr<Device> m_device;
	bool m_callbacksAdded = false;
	mCore* m_callbackCore = nullptr;

	std::mutex m_inLock;
	std::deque<Incoming> m_incoming;
	std::atomic<bool> m_hasIncoming{false};
	std::atomic<bool> m_resetRequested{false};
	std::atomic<bool> m_linkActive{false};
	std::atomic<bool> m_roleSet{false};

	std::mutex m_outLock;
	std::condition_variable m_outCond;
	std::deque<Outgoing> m_outgoing;
	long m_outSequence = 0;
	long long m_linkStartAt = 0;

	long m_expectedSequence = 0;
	std::map<long, std::vector<uint16_t>> m_buffered;
	std::set<std::string> m_seenCommands;

	std::unique_ptr<CelioWebSocket> m_ws;
	std::mutex m_wsLock;
	std::thread m_net;
	std::thread m_work;
	std::atomic<bool> m_stopping{false};
	std::atomic<bool> m_connected{false};
	std::atomic<long long> m_lastReceive{0};
	std::string m_clientId;
	std::string m_joinRoom;
	bool m_inSession = false;
	long m_nextAck = 0;
	long m_sessionAck = -1;
	std::string m_logPath;

	Kind m_kind = Kind::Net;
	std::string m_portName;
	std::string m_cfgHost; // set by setServer
	int m_cfgPort = 0;
	bool m_cfgSecure = true;
	std::string m_cfgSource;
	bool m_cfgSet = false;
	std::mutex m_cfgLock;
	std::string m_useSource;
	std::string m_useHost; // copy taken at startCommon, read by netThread
	int m_usePort = 0;
	bool m_useSecure = true;
	int m_listenPort = 0;
	std::unique_ptr<CelioPort> m_port;
	std::thread m_portThread;
	std::mutex m_sessionLock;
	uint16_t m_sessionStatus[2] = {};
	bool m_masterSelected = false;
	long long m_closeAt = 0;
	std::atomic<bool> m_firmwareSeen{false};
};

}

/* Net link over the public Celio relay, built into the Qt frontend (Windows only).
 * See CelioNet.h.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "CelioNet.h"

#include "CoreController.h"

#include <mgba/core/core.h>
#include <mgba/core/interface.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/io.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <cctype>
#include <random>

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#include <setupapi.h>

using namespace QGBA;

namespace {

// celio_device.lua
enum LinkStatus : uint16_t {
	AwaitModeEmulator = 0xFF01,
	AwaitMode = 0xFF02,
	HandshakeReceived = 0xFF03,
	HandshakeFinished = 0xFF04,
	LinkConnected = 0xFF05,
	LinkReconnecting = 0xFF06,
	LinkClosed = 0xFF07,
	DeviceReady = 0xFF08,
	EmuTradeSessionFinished = 0xFF09,
	EmuSessionStarted = 0xFF0A,
	StatusDebug = 0xFFFF,
};

enum CommandType : uint16_t {
	CmdSetMode = 0x00,
	CmdCancel = 0x01,
	CmdGetFirmwareInfo = 0x0F,
	CmdSetModeMaster = 0x10,
	CmdSetModeSlave = 0x11,
	CmdStartHandshake = 0x12,
	CmdConnectLink = 0x13,
	CmdEmuSessionStart = 0xFF0A,
};

enum class Transive { Handshake, Crc, Command };
enum class Handshake { Waiting, Listening, WaitingToRespond, Responding };
enum class Mode { Master, Slave };

// Celio-Firmware SerialLayer: | 'G' 'B' | channel | len:2 LE | payload |
const uint8_t SYNC_0 = 0x47;
const uint8_t SYNC_1 = 0x42;
const uint8_t CH_CMD = 0x00;
const uint8_t CH_DATA = 0x01;
const uint8_t CH_STATUS = 0x02;
const size_t MAX_PAYLOAD = 64;
const uint8_t LINK_MODE_ONLINE = 0x01; // LinkMode.onlineLink

const uint32_t IE_REG = 0x200;
const uint32_t IF_REG = 0x202;
const uint16_t SIO_IRQ_MASK = 0x80;

long long nowMs() {
	using namespace std::chrono;
	return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::string makeUuid() {
	std::random_device rd;
	std::mt19937_64 gen(((uint64_t) rd() << 32) ^ rd() ^ (uint64_t) std::time(nullptr));
	uint64_t a = gen();
	uint64_t b = gen();
	a = (a & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
	b = (b & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;
	char buf[40];
	snprintf(buf, sizeof(buf), "%08x-%04x-%04x-%04x-%012llx",
	         (unsigned) (a >> 32), (unsigned) ((a >> 16) & 0xFFFF), (unsigned) (a & 0xFFFF),
	         (unsigned) (b >> 48), (unsigned long long) (b & 0xFFFFFFFFFFFFULL));
	return buf;
}

// Tiny lookups for the few fixed JSON shapes the relay sends
size_t findKey(const std::string& json, const char* key) {
	std::string pat = std::string("\"") + key + "\"";
	size_t p = json.find(pat);
	if (p == std::string::npos) {
		return p;
	}
	p = json.find(':', p + pat.size());
	if (p == std::string::npos) {
		return p;
	}
	++p;
	while (p < json.size() && (json[p] == ' ' || json[p] == '\t')) {
		++p;
	}
	return p;
}

bool jsonNumber(const std::string& json, const char* key, long& out) {
	size_t p = findKey(json, key);
	if (p == std::string::npos) {
		return false;
	}
	char* end = nullptr;
	out = strtol(json.c_str() + p, &end, 10);
	return end != json.c_str() + p;
}

bool jsonString(const std::string& json, const char* key, std::string& out) {
	size_t p = findKey(json, key);
	if (p == std::string::npos || p >= json.size() || json[p] != '"') {
		return false;
	}
	size_t e = json.find('"', p + 1);
	if (e == std::string::npos) {
		return false;
	}
	out = json.substr(p + 1, e - p - 1);
	return true;
}

bool jsonNumberArray(const std::string& json, const char* key, std::vector<uint16_t>& out) {
	size_t p = findKey(json, key);
	if (p == std::string::npos || p >= json.size() || json[p] != '[') {
		return false;
	}
	++p;
	out.clear();
	while (p < json.size() && json[p] != ']') {
		if (json[p] == ',' || json[p] == ' ') {
			++p;
			continue;
		}
		char* end = nullptr;
		long v = strtol(json.c_str() + p, &end, 10);
		if (end == json.c_str() + p) {
			return false;
		}
		out.push_back((uint16_t) v);
		p = end - json.c_str();
	}
	return true;
}

std::wstring widen(const std::string& s) {
	return std::wstring(s.begin(), s.end());
}

}

namespace QGBA {

// WinHTTP websocket client. One receiver thread, sends are serialised by the caller.
class CelioWebSocket {
public:
	~CelioWebSocket() { close(); }

	bool open(const std::string& host, int port, bool secure, const std::string& path, std::string& error) {
		m_session = WinHttpOpen(L"mGBA-celio-net/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
		if (!m_session) {
			error = "WinHttpOpen " + std::to_string(GetLastError());
			return false;
		}
		WinHttpSetTimeouts(m_session, 5000, 5000, 5000, 0);
		m_connect = WinHttpConnect(m_session, widen(host).c_str(), (INTERNET_PORT) port, 0);
		if (!m_connect) {
			error = "WinHttpConnect " + std::to_string(GetLastError());
			return false;
		}
		HINTERNET request = WinHttpOpenRequest(m_connect, L"GET", widen(path).c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
		if (!request) {
			error = "WinHttpOpenRequest " + std::to_string(GetLastError());
			return false;
		}
		if (!WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) ||
		    !WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) ||
		    !WinHttpReceiveResponse(request, nullptr)) {
			error = "WinHttp request " + std::to_string(GetLastError());
			WinHttpCloseHandle(request);
			return false;
		}
		DWORD status = 0;
		DWORD size = sizeof(status);
		WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
		if (status != 101) {
			error = "HTTP " + std::to_string(status);
			WinHttpCloseHandle(request);
			return false;
		}
		HINTERNET ws = WinHttpWebSocketCompleteUpgrade(request, 0);
		WinHttpCloseHandle(request);
		if (!ws) {
			error = "WinHttpWebSocketCompleteUpgrade " + std::to_string(GetLastError());
			return false;
		}
		m_ws.store(ws);
		return true;
	}

	bool send(const std::string& text) {
		HINTERNET ws = m_ws.load();
		if (!ws) {
			return false;
		}
		DWORD err = WinHttpWebSocketSend(ws, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, (PVOID) text.data(), (DWORD) text.size());
		return err == NO_ERROR;
	}

	bool receive(std::string& out) {
		out.clear();
		char buffer[4096];
		while (true) {
			HINTERNET ws = m_ws.load();
			if (!ws) {
				return false;
			}
			DWORD read = 0;
			WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
			DWORD err = WinHttpWebSocketReceive(ws, buffer, sizeof(buffer), &read, &type);
			if (err != NO_ERROR || type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
				return false;
			}
			out.append(buffer, read);
			if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) {
				return true;
			}
		}
	}

	// Safe from any thread; makes a blocked receive return
	void close() {
		HINTERNET ws = m_ws.exchange(nullptr);
		if (ws) {
			WinHttpWebSocketClose(ws, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
			WinHttpCloseHandle(ws);
		}
		if (m_connect) {
			WinHttpCloseHandle(m_connect);
			m_connect = nullptr;
		}
		if (m_session) {
			WinHttpCloseHandle(m_session);
			m_session = nullptr;
		}
	}

	void abort() {
		HINTERNET ws = m_ws.exchange(nullptr);
		if (ws) {
			WinHttpCloseHandle(ws);
		}
	}

private:
	HINTERNET m_session = nullptr;
	HINTERNET m_connect = nullptr;
	std::atomic<HINTERNET> m_ws{nullptr};
};

// Byte pipe to the USB adapter: a COM port (CDC-ACM), or TCP for tests.
// One reader thread; writes are serialised here. close() makes a blocked read return.
class CelioPort {
public:
	// The reader thread must be joined first
	~CelioPort() {
		close();
		if (m_sock != INVALID_SOCKET) {
			closesocket(m_sock);
		}
		if (m_com != INVALID_HANDLE_VALUE) {
			CloseHandle(m_com);
		}
		if (m_readEvent) {
			CloseHandle(m_readEvent);
		}
		if (m_writeEvent) {
			CloseHandle(m_writeEvent);
		}
	}

	// "COM5" | "tcp:host:port" | "listen:port" (accepts one connection on 127.0.0.1)
	bool open(const std::string& spec, std::string& error) {
		if (spec.compare(0, 4, "tcp:") == 0) {
			std::string rest = spec.substr(4);
			size_t colon = rest.rfind(':');
			if (colon == std::string::npos) {
				error = "tcp:host:port";
				return false;
			}
			return openTcp(rest.substr(0, colon), rest.substr(colon + 1), error);
		}
		if (spec.compare(0, 7, "listen:") == 0) {
			return openListen(atoi(spec.c_str() + 7), error);
		}
		return openCom(spec, error);
	}

	// Blocks until data arrives. Returns false when the port is gone or closed.
	bool read(std::vector<uint8_t>& out) {
		out.clear();
		uint8_t buffer[256];
		while (!m_closed) {
			if (m_listen != INVALID_SOCKET && m_sock == INVALID_SOCKET) {
				SOCKET s = accept(m_listen, nullptr, nullptr);
				if (s == INVALID_SOCKET) {
					return false;
				}
				int one = 1;
				setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*) &one, sizeof(one));
				m_sock = s;
				continue;
			}
			if (m_sock != INVALID_SOCKET) {
				int n = recv(m_sock, (char*) buffer, sizeof(buffer), 0);
				if (n <= 0) {
					return false;
				}
				out.assign(buffer, buffer + n);
				return true;
			}
			OVERLAPPED ov{};
			ov.hEvent = m_readEvent;
			DWORD got = 0;
			if (!ReadFile(m_com, buffer, sizeof(buffer), &got, &ov)) {
				if (GetLastError() != ERROR_IO_PENDING || !GetOverlappedResult(m_com, &ov, &got, TRUE)) {
					return false;
				}
			}
			if (got) {
				out.assign(buffer, buffer + got);
				return true;
			}
			// read timeout with nothing received: go round again
		}
		return false;
	}

	bool write(const uint8_t* data, size_t size) {
		std::lock_guard<std::mutex> lock(m_writeLock);
		if (m_closed) {
			return false;
		}
		if (m_sock != INVALID_SOCKET) {
			size_t sent = 0;
			while (sent < size) {
				int n = send(m_sock, (const char*) data + sent, (int) (size - sent), 0);
				if (n <= 0) {
					return false;
				}
				sent += n;
			}
			return true;
		}
		if (m_com == INVALID_HANDLE_VALUE) {
			return false;
		}
		OVERLAPPED ov{};
		ov.hEvent = m_writeEvent;
		DWORD done = 0;
		if (!WriteFile(m_com, data, (DWORD) size, &done, &ov)) {
			if (GetLastError() != ERROR_IO_PENDING || !GetOverlappedResult(m_com, &ov, &done, TRUE)) {
				return false;
			}
		}
		return done == size;
	}

	// Any thread: makes blocked reads and writes return. Handles are freed in the destructor.
	void close() {
		m_closed = true;
		if (m_com != INVALID_HANDLE_VALUE) {
			CancelIoEx(m_com, nullptr);
		}
		SOCKET s = m_sock;
		if (s != INVALID_SOCKET) {
			shutdown(s, SD_BOTH);
		}
		SOCKET l = m_listen.exchange(INVALID_SOCKET);
		if (l != INVALID_SOCKET) {
			closesocket(l);
		}
	}

private:
	bool openCom(const std::string& name, std::string& error) {
		std::string path = "\\\\.\\" + name;
		m_com = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
		if (m_com == INVALID_HANDLE_VALUE) {
			error = name + " を ひらけません（" + std::to_string(GetLastError()) + "）";
			return false;
		}
		SetupComm(m_com, 4096, 4096);
		DCB dcb{};
		dcb.DCBlength = sizeof(dcb);
		if (GetCommState(m_com, &dcb)) {
			// CDC-ACM ignores the line settings; DTR/RTS on as Web Serial does
			dcb.BaudRate = 115200;
			dcb.ByteSize = 8;
			dcb.Parity = NOPARITY;
			dcb.StopBits = ONESTOPBIT;
			dcb.fBinary = TRUE;
			dcb.fDtrControl = DTR_CONTROL_ENABLE;
			dcb.fRtsControl = RTS_CONTROL_ENABLE;
			dcb.fOutxCtsFlow = FALSE;
			dcb.fOutxDsrFlow = FALSE;
			dcb.fOutX = FALSE;
			dcb.fInX = FALSE;
			SetCommState(m_com, &dcb);
		}
		COMMTIMEOUTS timeouts{};
		// Return as soon as anything arrives, or after 200 ms with nothing
		timeouts.ReadIntervalTimeout = MAXDWORD;
		timeouts.ReadTotalTimeoutMultiplier = MAXDWORD;
		timeouts.ReadTotalTimeoutConstant = 200;
		timeouts.WriteTotalTimeoutConstant = 2000;
		SetCommTimeouts(m_com, &timeouts);
		EscapeCommFunction(m_com, SETDTR);
		EscapeCommFunction(m_com, SETRTS);
		PurgeComm(m_com, PURGE_RXCLEAR | PURGE_TXCLEAR);
		m_readEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
		m_writeEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
		return true;
	}

	bool startWinsock(std::string& error) {
		WSADATA wsa;
		if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
			error = "WSAStartup";
			return false;
		}
		return true;
	}

	bool openTcp(const std::string& host, const std::string& port, std::string& error) {
		if (!startWinsock(error)) {
			return false;
		}
		addrinfo hints{};
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_STREAM;
		addrinfo* res = nullptr;
		if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || !res) {
			error = "getaddrinfo " + host;
			return false;
		}
		SOCKET s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
		if (s == INVALID_SOCKET || connect(s, res->ai_addr, (int) res->ai_addrlen) != 0) {
			error = "tcp " + host + ":" + port + " に つながりません";
			if (s != INVALID_SOCKET) {
				closesocket(s);
			}
			freeaddrinfo(res);
			return false;
		}
		freeaddrinfo(res);
		int one = 1;
		setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*) &one, sizeof(one));
		m_sock = s;
		return true;
	}

	bool openListen(int port, std::string& error) {
		if (!startWinsock(error)) {
			return false;
		}
		SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_port = htons((u_short) port);
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		if (s == INVALID_SOCKET || bind(s, (sockaddr*) &addr, sizeof(addr)) != 0 || listen(s, 1) != 0) {
			error = "listen " + std::to_string(port);
			if (s != INVALID_SOCKET) {
				closesocket(s);
			}
			return false;
		}
		m_listen = s;
		return true;
	}

	HANDLE m_com = INVALID_HANDLE_VALUE;
	HANDLE m_readEvent = nullptr;
	HANDLE m_writeEvent = nullptr;
	std::atomic<SOCKET> m_sock{INVALID_SOCKET};
	std::atomic<SOCKET> m_listen{INVALID_SOCKET};
	std::atomic<bool> m_closed{false};
	std::mutex m_writeLock;
};

// Port of celio_device.lua (state lives on the core thread only)
struct CelioNet::Device {
	Handshake handshake = Handshake::Waiting;
	Transive transive = Transive::Handshake;
	bool emuReconnect = false;
	bool gbaReconnect = false;
	bool keepAlive = true;
	bool startResponse = false;
	bool startConnect = false;
	std::deque<uint16_t> receivedQueue;
	std::vector<uint16_t> transmitQueue;
	std::deque<uint16_t> currentTx;
	std::vector<uint16_t> currentRx;
	int emptyDirectionStreak = 0;
	uint16_t checksum = 0;
	bool timerEnabled = false;
	int timerCount = 0;
	Mode mode = Mode::Slave;

	void resetState(bool init) {
		Mode keep = mode;
		*this = Device();
		mode = keep;
		if (!init) {
			handshake = Handshake::Listening;
		}
	}
};

CelioNet* CelioNet::instance() {
	static CelioNet net;
	return &net;
}

void CelioNet::log(const std::string& line) {
	if (m_logPath.empty()) {
		return;
	}
	FILE* f = fopen(m_logPath.c_str(), "a");
	if (!f) {
		return;
	}
	fprintf(f, "%lld %s\n", nowMs(), line.c_str());
	fclose(f);
}

void CelioNet::setState(State state, const std::string& message) {
	{
		std::lock_guard<std::mutex> lock(m_stateLock);
		m_state = state;
		m_message = message;
	}
	log("state " + std::to_string((int) state) + " " + message);
}

void CelioNet::setMessage(const std::string& message) {
	std::lock_guard<std::mutex> lock(m_stateLock);
	m_message = message;
}

CelioNet::Snapshot CelioNet::snapshot() {
	std::lock_guard<std::mutex> lock(m_stateLock);
	return Snapshot{m_state, m_room, m_message, m_net.joinable() || m_work.joinable(), m_kind};
}

bool CelioNet::start(std::shared_ptr<CoreController> controller, const std::string& room) {
	return startCommon(Kind::Net, controller, room, std::string(), 0);
}

bool CelioNet::startDirect(std::shared_ptr<CoreController> controller, const std::string& port) {
	return startCommon(Kind::Direct, controller, std::string(), port, 0);
}

bool CelioNet::startNetUsb(const std::string& port, const std::string& room) {
	return startCommon(Kind::NetUsb, nullptr, room, port, 0);
}

bool CelioNet::startFakeAdapter(std::shared_ptr<CoreController> controller, int listenPort) {
	return startCommon(Kind::FakeAdapter, controller, std::string(), std::string(), listenPort);
}

bool CelioNet::serverConfigured() {
	std::lock_guard<std::mutex> lock(m_cfgLock);
	return m_cfgSet;
}

void CelioNet::failStart(const std::string& message, const std::string& logLine) {
	const char* logPath = getenv("MGBA_CELIO_NET_LOG");
	m_logPath = logPath ? logPath : "";
	log(logLine);
	setState(State::Error, message);
}

void CelioNet::setServer(const std::string& host, int port, bool secure, const char* source) {
	std::lock_guard<std::mutex> lock(m_cfgLock);
	m_cfgSet = true;
	m_cfgSource = source;
	m_cfgHost = host;
	m_cfgPort = port;
	m_cfgSecure = secure;
}

bool CelioNet::startCommon(Kind kind, std::shared_ptr<CoreController> controller, const std::string& room, const std::string& port, int listenPort) {
	stop();
	bool needsCore = kind != Kind::NetUsb;
	if (needsCore && (!controller || controller->platform() != mPLATFORM_GBA)) {
		setState(State::Error, "ゲームを起動してから使ってください");
		return false;
	}
	if ((kind == Kind::NetUsb || kind == Kind::Direct) && port.empty()) {
		setState(State::Error, "USB の変換器が みつかりません");
		return false;
	}
	const char* logPath = getenv("MGBA_CELIO_NET_LOG");
	m_logPath = logPath ? logPath : "";
	{
		std::lock_guard<std::mutex> lock(m_cfgLock);
		m_useHost = m_cfgHost;
		m_usePort = m_cfgPort;
		m_useSecure = m_cfgSecure;
		m_useSource = m_cfgSource;
	}
	m_kind = kind;
	m_portName = port;
	m_listenPort = listenPort;
	m_controller = needsCore ? controller : nullptr;
	m_joinRoom = room;
	{
		std::lock_guard<std::mutex> lock(m_stateLock);
		m_room = room;
	}
	m_stopping = false;
	m_connected = false;
	m_inSession = false;
	m_linkActive = false;
	m_roleSet = false;
	m_sessionAck = -1;
	m_nextAck = 0;
	m_outSequence = 0;
	m_expectedSequence = 0;
	m_buffered.clear();
	m_seenCommands.clear();
	m_outgoing.clear();
	m_incoming.clear();
	m_hasIncoming = false;
	m_linkStartAt = 0;
	m_closeAt = 0;
	m_masterSelected = false;
	m_sessionStatus[0] = m_sessionStatus[1] = 0;
	m_firmwareSeen = false;
	m_clientId = makeUuid();

	if (kind != Kind::Net) {
		std::string spec = kind == Kind::FakeAdapter ? "listen:" + std::to_string(listenPort) : port;
		std::unique_ptr<CelioPort> p(new CelioPort);
		std::string error;
		if (!p->open(spec, error)) {
			log("port open failed: " + error);
			m_controller.reset();
			setState(State::Error, error);
			return false;
		}
		m_port = std::move(p);
		log("port " + spec);
	}
	if (m_controller) {
		CoreController::Interrupter interrupter(m_controller);
		attachCore();
	}
	if (m_port) {
		m_portThread = std::thread(&CelioNet::portThread, this);
	}
	switch (kind) {
	case Kind::Net:
	case Kind::NetUsb:
		if (m_port) {
			portCommand(CmdGetFirmwareInfo);
		}
		{
			std::string eff = "celio-server.up.railway.app:443 (wss)";
			if (!m_useHost.empty()) {
				eff = m_useHost + ":" + std::to_string(m_usePort) + (m_useSecure ? " (wss)" : " (ws)");
			}
			const char* envServer = getenv("MGBA_CELIO_NET_SERVER");
			bool envOn = envServer && *envServer;
			if (envOn) {
				eff = std::string(envServer) + " (ws)";
			}
			log("server " + eff + " [" + (envOn ? std::string("env") : m_useHost.empty() ? std::string("default") : m_useSource) + "]");
			setState(State::Connecting, "サーバー " + eff + " に つないでいます…" + (envOn ? "（環境変数を つかっています）" : ""));
		}
		m_net = std::thread(&CelioNet::netThread, this);
		break;
	case Kind::Direct:
		portCommand(CmdGetFirmwareInfo);
		m_deviceOn = true;
		setState(State::Linking, "実機と つないでいます（" + port + "）…\n実機と このゲームの 両方で ポケモンセンター 2 階の受付へ");
		{
			// LinkDeviceUtils.tryEnableLinkMode on both devices: Cancel, wait 500 ms, SetMode
			pushIncoming(Incoming{true, CmdCancel, {}});
			portCommand(CmdCancel);
			std::lock_guard<std::mutex> lock(m_outLock);
			m_linkStartAt = nowMs() + 500;
		}
		break;
	case Kind::FakeAdapter:
		m_deviceOn = true;
		setState(State::Linking, "変換器の かわりとして まっています（tcp " + std::to_string(listenPort) + "）");
		break;
	}
	m_work = std::thread(&CelioNet::workThread, this);
	return true;
}

void CelioNet::stop(bool touchCore) {
	m_stopping = true;
	m_linkActive = false;
	m_roleSet = false;
	if (m_connected && m_inSession) {
		sendText("42[\"sessionLeft\"]");
	}
	{
		std::lock_guard<std::mutex> lock(m_wsLock);
		if (m_ws) {
			m_ws->abort();
		}
	}
	m_outCond.notify_all();
	if (m_net.joinable()) {
		m_net.join();
	}
	if (m_work.joinable()) {
		m_work.join();
	}
	{
		std::lock_guard<std::mutex> lock(m_wsLock);
		m_ws.reset();
	}
	if (m_port) {
		if (m_kind == Kind::Direct || m_kind == Kind::NetUsb) {
			// LinkExchangeSession.destroy
			portCommand(CmdCancel);
		}
		m_port->close();
		if (m_portThread.joinable()) {
			m_portThread.join();
		}
		m_port.reset();
	}
	m_deviceOn = false;
	if (m_controller) {
		if (m_attached && touchCore) {
			CoreController::Interrupter interrupter(m_controller);
			detachCore();
		} else if (m_attached.exchange(false)) {
			m_gba = nullptr;
			m_device.reset();
		}
		m_controller.reset();
	}
	std::lock_guard<std::mutex> lock(m_stateLock);
	if (m_state != State::Error && m_state != State::Finished) {
		m_state = State::Idle;
		m_message.clear();
	}
}

// ---- core side ---------------------------------------------------------------

void CelioNet::attachCore() {
	mCore* core = m_controller->thread()->core;
	m_gba = static_cast<GBA*>(core->board);
	if (!m_callbacksAdded || m_callbackCore != core) {
		mCoreCallbacks callbacks{};
		callbacks.context = this;
		callbacks.vblankIRQ = &CelioNet::vblankHook;
		callbacks.timer3IRQ = &CelioNet::timer3Hook;
		core->addCoreCallbacks(core, &callbacks);
		m_callbacksAdded = true;
		m_callbackCore = core;
	}
	if (!m_mask) {
		m_mask.reset(new mSioMask);
	}
	m_mask->mask = 0xFFFF;
	m_oldMask = m_gba->sioMask;
	m_gba->sioMask = m_mask.get();
	m_gba->sioReadHookContext = this;
	m_gba->sioReadHook = &CelioNet::sioReadHook;
	m_device.reset(new Device);
	m_resetRequested = false;
	m_attached = true;
}

void CelioNet::detachCore() {
	if (!m_attached.exchange(false)) {
		return;
	}
	if (m_gba) {
		if (m_gba->sioMask == m_mask.get()) {
			m_gba->sioMask = m_oldMask;
		}
		m_gba->sioReadHook = nullptr;
		m_gba->sioReadHookContext = nullptr;
		if (m_device && m_device->timerEnabled) {
			GBAIOWrite(m_gba, GBA_REG_TM3CNT_HI, 0);
			GBAIOWrite(m_gba, GBA_REG_TM3CNT_LO, 0);
		}
	}
	m_device.reset();
}

void CelioNet::setSioMask(uint16_t mask) {
	m_mask->mask = mask;
}

void CelioNet::pushIncoming(Incoming in) {
	if (m_kind == Kind::NetUsb) {
		// The adapter is the device
		if (in.isCommand) {
			portCommand(in.command);
		} else {
			portData(in.data);
		}
		return;
	}
	std::lock_guard<std::mutex> lock(m_inLock);
	m_incoming.push_back(std::move(in));
	m_hasIncoming = true;
}

void CelioNet::noteStatus(uint16_t status) {
	switch (status) {
	case LinkConnected:
		setState(State::Connected, "つながりました（通信中）");
		break;
	case LinkClosed:
		setMessage("通信を おえています…");
		break;
	default:
		break;
	}
}

void CelioNet::queueOutgoing(Outgoing item) {
	// LinkExchangeSession.handleDeviceStatusToSocket
	if (item.isStatus && (item.status == DeviceReady || item.status == EmuTradeSessionFinished || item.status == StatusDebug)) {
		return;
	}
	std::lock_guard<std::mutex> lock(m_outLock);
	m_outgoing.push_back(std::move(item));
	m_outCond.notify_all();
}

// From the emulated device
void CelioNet::emitStatus(uint16_t status) {
	log("device status " + std::to_string(status));
	noteStatus(status);
	switch (m_kind) {
	case Kind::Net:
		queueOutgoing(Outgoing{true, status, {}});
		break;
	case Kind::Direct:
		sessionStatus(0, status);
		break;
	case Kind::FakeAdapter:
		portStatus(status);
		break;
	case Kind::NetUsb:
		break;
	}
}

void CelioNet::emitData(const std::vector<uint16_t>& data) {
	switch (m_kind) {
	case Kind::Net:
		queueOutgoing(Outgoing{false, 0, data});
		break;
	case Kind::Direct:
	case Kind::FakeAdapter:
		portData(data);
		break;
	case Kind::NetUsb:
		break;
	}
}

void CelioNet::drainIncoming() {
	if (m_resetRequested.exchange(false) && m_device && !m_roleSet) {
		m_device->resetState(true);
		m_device->mode = Mode::Slave;
		m_mask->mask = 0xFFFF;
	}
	if (!m_hasIncoming) {
		return;
	}
	std::deque<Incoming> items;
	{
		std::lock_guard<std::mutex> lock(m_inLock);
		items.swap(m_incoming);
		m_hasIncoming = false;
	}
	Device* d = m_device.get();
	if (!d) {
		return;
	}
	for (Incoming& in : items) {
		if (!in.isCommand) {
			// celio_device:receive_data
			for (uint16_t v : in.data) {
				d->receivedQueue.push_back(v);
			}
			continue;
		}
		// celio_device:receive_command
		switch (in.command) {
		case CmdSetMode:
			log("device command SetMode");
			emitStatus(DeviceReady);
			emitStatus(AwaitMode);
			break;
		case CmdEmuSessionStart:
			emitStatus(AwaitMode);
			break;
		case CmdCancel:
			log("device command Cancel");
			break;
		case CmdSetModeSlave:
			log("device command SetModeSlave");
			m_roleSet = true;
			d->handshake = Handshake::Listening;
			d->mode = Mode::Slave;
			setSioMask(0x600B);
			break;
		case CmdSetModeMaster:
			log("device command SetModeMaster");
			m_roleSet = true;
			d->handshake = Handshake::Listening;
			d->mode = Mode::Master;
			setSioMask(0x601F);
			break;
		case CmdStartHandshake:
			log("device command StartHandshake");
			d->startResponse = true;
			break;
		case CmdConnectLink:
			log("device command ConnectLink");
			d->startConnect = true;
			break;
		default:
			log("device unknown command " + std::to_string(in.command));
			break;
		}
	}
}

static void writeIo(GBA* gba, uint32_t reg, uint16_t value) {
	gba->memory.io[reg >> 1] = value;
}

static uint16_t readIo(GBA* gba, uint32_t reg) {
	return gba->memory.io[reg >> 1];
}

void CelioNet::sioReadHook(void* context) {
	CelioNet* self = static_cast<CelioNet*>(context);
	if (!self->m_attached) {
		return;
	}
	self->drainIncoming();
	Device* d = self->m_device.get();
	if (!d || !self->m_deviceOn) {
		return;
	}
	GBA* gba = self->m_gba;
	uint16_t rx = readIo(gba, 0x12A);
	uint16_t tx = 0;

	switch (d->transive) {
	case Transive::Handshake: {
		// transive_handshake
		bool done = false;
		if (rx == 0xB9A0 && d->handshake == Handshake::Listening) {
			self->emitStatus(HandshakeReceived);
			d->handshake = Handshake::WaitingToRespond;
		}
		if (d->startConnect) {
			self->emitStatus(LinkConnected);
			d->transive = Transive::Crc;
			tx = 0x8FFF;
			done = true;
		}
		if (!done) {
			if (rx == 0x8FFF) {
				self->emitStatus(LinkConnected);
				d->transive = Transive::Crc;
			}
			if (d->handshake == Handshake::Responding) {
				tx = 0xB9A0;
			} else {
				if (d->startResponse) {
					d->handshake = Handshake::Responding;
				}
				tx = 0xD15E;
			}
		}
		break;
	}
	case Transive::Crc: {
		// transive_crc
		auto flush = [self, d]() {
			std::vector<uint16_t> data(d->transmitQueue.begin(), d->transmitQueue.end());
			data.resize(32, 0);
			d->transmitQueue.clear();
			self->emitData(data);
		};
		if (d->emuReconnect && d->gbaReconnect) {
			flush();
			if (d->keepAlive) {
				self->log("device reconnecting");
				self->emitStatus(LinkReconnecting);
				d->resetState(false);
			} else {
				self->log("device link closed");
				self->emitStatus(LinkClosed);
			}
			if (d->timerEnabled) {
				d->timerEnabled = false;
			}
			d->timerCount = 0;
			GBAIOWrite(gba, GBA_REG_TM3CNT_HI, 0);
			GBAIOWrite(gba, GBA_REG_TM3CNT_LO, 0);
		} else {
			d->transive = Transive::Command;
		}
		tx = d->checksum;
		d->checksum = 0;
		break;
	}
	case Transive::Command: {
		// transive_command
		d->currentRx.push_back(rx);
		if (d->currentTx.empty()) {
			// load_tx_command
			if (d->receivedQueue.empty()) {
				d->currentTx.assign(8, 0);
			} else {
				for (int i = 0; i < 8 && !d->receivedQueue.empty(); ++i) {
					d->currentTx.push_back(d->receivedQueue.front());
					d->receivedQueue.pop_front();
				}
			}
			if (d->currentTx.size() >= 2 && d->currentTx[0] == 0xCAFE && d->currentTx[1] == 0x0017) {
				d->keepAlive = false;
			}
			if (d->currentTx[0] == 0x5FFF) {
				self->log("partner ready for reconnect");
				d->emuReconnect = true;
			}
		}
		if (d->currentRx.size() == 8) {
			if (d->currentRx[0] == 0x5FFF) {
				self->log("ready for reconnect");
				d->gbaReconnect = true;
			}
			// save_rx_command
			if (d->currentRx[0] == 0xCAFE && d->currentRx[1] == 0x0011) {
				++d->emptyDirectionStreak;
				if (d->emptyDirectionStreak > 1) {
					d->currentRx[0] = 0;
					d->currentRx[1] = 0;
				}
			} else {
				d->emptyDirectionStreak = 0;
			}
			bool queue = !d->transmitQueue.empty();
			for (uint16_t v : d->currentRx) {
				if (v) {
					queue = true;
					break;
				}
			}
			if (queue) {
				d->transmitQueue.insert(d->transmitQueue.end(), d->currentRx.begin(), d->currentRx.end());
			}
			d->currentRx.clear();
			d->transive = Transive::Crc;
		}
		if (d->transmitQueue.size() >= 32) {
			std::vector<uint16_t> data(d->transmitQueue.begin(), d->transmitQueue.end());
			data.resize(32, 0);
			d->transmitQueue.clear();
			self->emitData(data);
		}
		tx = d->currentTx.front();
		d->currentTx.pop_front();
		d->checksum = (uint16_t) (d->checksum + tx);
		d->checksum = (uint16_t) (d->checksum + rx);
		break;
	}
	}

	if (d->mode == Mode::Master) {
		writeIo(gba, 0x120, tx);
		writeIo(gba, 0x122, rx);
	} else {
		writeIo(gba, 0x120, rx);
		writeIo(gba, 0x122, tx);
	}
	writeIo(gba, 0x124, 0xFFFF);
	writeIo(gba, 0x126, 0xFFFF);
}

void CelioNet::vblankHook(void* context) {
	CelioNet* self = static_cast<CelioNet*>(context);
	if (!self->m_attached) {
		return;
	}
	self->drainIncoming();
	Device* d = self->m_device.get();
	if (!d || !self->m_deviceOn) {
		return;
	}
	GBA* gba = self->m_gba;
	// sync_timer
	uint16_t ie = readIo(gba, IE_REG);
	if (d->mode == Mode::Slave || !(ie & SIO_IRQ_MASK)) {
		if (d->timerEnabled) {
			d->timerEnabled = false;
			d->timerCount = 0;
			GBAIOWrite(gba, GBA_REG_TM3CNT_HI, 0);
			GBAIOWrite(gba, GBA_REG_TM3CNT_LO, 0);
		}
		return;
	}
	writeIo(gba, IF_REG, readIo(gba, IF_REG) | SIO_IRQ_MASK);
	if (d->transive == Transive::Crc && !d->timerEnabled) {
		d->timerEnabled = true;
		GBAIOWrite(gba, GBA_REG_TM3CNT_LO, 0xFED0);
		GBAIOWrite(gba, GBA_REG_TM3CNT_HI, 0x00C1);
	}
}

void CelioNet::timer3Hook(void* context) {
	CelioNet* self = static_cast<CelioNet*>(context);
	if (!self->m_attached) {
		return;
	}
	Device* d = self->m_device.get();
	if (!d || !self->m_deviceOn) {
		return;
	}
	GBA* gba = self->m_gba;
	// transmission_timer
	uint16_t ie = readIo(gba, IE_REG);
	if (d->mode == Mode::Slave || !(ie & SIO_IRQ_MASK)) {
		return;
	}
	writeIo(gba, IF_REG, (readIo(gba, IF_REG) & 0xFFBF) | SIO_IRQ_MASK);
	if (d->timerCount == 7) {
		d->timerEnabled = false;
		d->timerCount = 0;
		GBAIOWrite(gba, GBA_REG_TM3CNT_HI, 0);
		GBAIOWrite(gba, GBA_REG_TM3CNT_LO, 0);
	} else {
		++d->timerCount;
	}
}

// ---- network side ------------------------------------------------------------

bool CelioNet::sendText(const std::string& text) {
	std::lock_guard<std::mutex> lock(m_wsLock);
	if (!m_ws) {
		return false;
	}
	return m_ws->send(text);
}

bool CelioNet::connectOnce(bool first) {
	std::string host = "celio-server.up.railway.app";
	int port = 443;
	bool secure = true;
	if (!m_useHost.empty()) {
		host = m_useHost;
		port = m_usePort;
		secure = m_useSecure;
	}
	// MGBA_CELIO_NET_SERVER=host:port (plain ws, for a self-hosted Celio-Server); wins over the dialog
	const char* server = getenv("MGBA_CELIO_NET_SERVER");
	if (server && *server) {
		std::string s = server;
		size_t colon = s.rfind(':');
		host = s.substr(0, colon);
		port = colon == std::string::npos ? 80 : atoi(s.c_str() + colon + 1);
		secure = false;
	}
	std::unique_ptr<CelioWebSocket> ws(new CelioWebSocket);
	std::string error;
	if (!ws->open(host, port, secure, "/socket.io/?EIO=4&transport=websocket", error)) {
		log("connect failed: " + error);
		return false;
	}
	{
		std::lock_guard<std::mutex> lock(m_wsLock);
		m_ws = std::move(ws);
	}
	log(first ? "connected" : "reconnected");
	m_lastReceive = nowMs();
	return true;
}

void CelioNet::netThread() {
	int failures = 0;
	bool first = true;
	while (!m_stopping) {
		if (!connectOnce(first)) {
			if (++failures > 5) {
				setState(State::Error, "サーバーに つながりません");
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(500));
			continue;
		}
		first = false;
		failures = 0;
		std::string msg;
		while (!m_stopping) {
			CelioWebSocket* ws;
			{
				std::lock_guard<std::mutex> lock(m_wsLock);
				ws = m_ws.get();
			}
			if (!ws || !ws->receive(msg)) {
				break;
			}
			m_lastReceive = nowMs();
			handleMessage(msg);
		}
		m_connected = false;
		{
			std::lock_guard<std::mutex> lock(m_wsLock);
			if (m_ws) {
				m_ws->abort();
			}
		}
		State state = snapshot().state;
		if (m_stopping || state == State::Finished || state == State::Error) {
			break;
		}
		log("connection lost, retrying");
		m_linkActive = false;
		m_roleSet = false;
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
	}
	m_connected = false;
	m_outCond.notify_all();
}

void CelioNet::handleMessage(const std::string& msg) {
	if (msg.empty()) {
		return;
	}
	switch (msg[0]) {
	case '0': // engine.io open
		sendText("40{\"clientId\":\"" + m_clientId + "\"}");
		return;
	case '2': // ping
		sendText("3");
		return;
	case '1':
		{
			std::lock_guard<std::mutex> lock(m_wsLock);
			if (m_ws) {
				m_ws->abort();
			}
		}
		return;
	case '4':
		break;
	default:
		return;
	}
	if (msg.size() < 2) {
		return;
	}
	char type = msg[1];
	size_t i = 2;
	long id = -1;
	if (i < msg.size() && msg[i] >= '0' && msg[i] <= '9') {
		id = 0;
		while (i < msg.size() && msg[i] >= '0' && msg[i] <= '9') {
			id = id * 10 + (msg[i] - '0');
			++i;
		}
	}
	std::string rest = msg.substr(i);
	switch (type) {
	case '0': // socket.io connect
		m_connected = true;
		if (!m_inSession && m_sessionAck == -1) {
			m_sessionAck = m_nextAck++;
			if (m_joinRoom.empty()) {
				setMessage("へやを つくっています…");
				sendText("42" + std::to_string(m_sessionAck) + "[\"sessionCreate\",null]");
			} else {
				setMessage("へや " + m_joinRoom + " に はいっています…");
				sendText("42" + std::to_string(m_sessionAck) + "[\"sessionJoin\",\"" + m_joinRoom + "\"]");
			}
		}
		m_outCond.notify_all();
		break;
	case '4': // connect error
		log("connect error " + rest);
		setState(State::Error, "サーバーに はいれませんでした");
		m_stopping = true;
		break;
	case '1':
		{
			std::lock_guard<std::mutex> lock(m_wsLock);
			if (m_ws) {
				m_ws->abort();
			}
		}
		break;
	case '2':
		handleEvent(rest, id);
		break;
	case '3':
		handleAck(id, rest);
		break;
	default:
		break;
	}
}

void CelioNet::handleAck(long id, const std::string& json) {
	if (id != m_sessionAck) {
		return;
	}
	m_sessionAck = -2;
	log("session reply " + json);
	std::string variant;
	jsonString(json, "variant", variant);
	if (variant != "Ok") {
		std::string error;
		jsonString(json, "error", error);
		std::string message = "へやに はいれませんでした";
		if (error == "Session not found") {
			message = "その番号の へやは ありません";
		} else if (error == "Session is full") {
			message = "その へやは もう 2 人 はいっています";
		}
		setState(State::Error, message);
		m_stopping = true;
		std::lock_guard<std::mutex> lock(m_wsLock);
		if (m_ws) {
			m_ws->abort();
		}
		return;
	}
	std::string room;
	jsonString(json, "id", room);
	{
		std::lock_guard<std::mutex> lock(m_stateLock);
		m_room = room;
	}
	m_inSession = true;
	m_deviceOn = true;
	m_roleSet = false;
	m_linkActive = !m_joinRoom.empty();
	if (m_joinRoom.empty()) {
		setState(State::WaitingPartner, "へやの 番号を 相手に つたえて、まってください");
	} else {
		setState(State::Linking, "相手と つながりました。通信の じゅんびを しています…");
		startLinkMode();
	}
}

void CelioNet::handleEvent(const std::string& json, long ackId) {
	std::string name;
	if (json.size() < 3 || json[0] != '[' || json[1] != '"') {
		return;
	}
	size_t e = json.find('"', 2);
	if (e == std::string::npos) {
		return;
	}
	name = json.substr(2, e - 2);

	// Answer every event the server sends with an ackId (deviceData is answered below).
	// Without a reply the public Celio server stops after its retry limit and closes the room.
	if (ackId >= 0 && name != "deviceData") {
		sendText("43" + std::to_string(ackId) + "[true]");
	}

	if (name == "deviceData") {
		if (ackId >= 0) {
			sendText("43" + std::to_string(ackId) + "[true]");
		}
		// Celio-Server since 2026-10-03 sends an array of packets ([{sequence,data},...]);
		// older servers send one object. Both are read here: one {...} per packet (no nesting inside).
		size_t objStart = json.find('{');
		while (objStart != std::string::npos) {
			size_t objEnd = json.find('}', objStart);
			if (objEnd == std::string::npos) {
				break;
			}
			std::string obj = json.substr(objStart, objEnd - objStart + 1);
			long sequence = 0;
			std::vector<uint16_t> data;
			if (jsonNumber(obj, "sequence", sequence) && jsonNumberArray(obj, "data", data)) {
				deliverData(sequence, std::move(data));
			}
			objStart = json.find('{', objEnd);
		}
	} else if (name == "deviceCommand") {
		std::string uuid;
		long command = 0;
		jsonString(json, "uuid", uuid);
		if (!jsonNumber(json, "command", command)) {
			return;
		}
		if (!uuid.empty()) {
			if (m_seenCommands.count(uuid)) {
				return;
			}
			m_seenCommands.insert(uuid);
		}
		log("server command " + std::to_string(command));
		pushIncoming(Incoming{true, (uint16_t) command, {}});
	} else if (name == "partnerJoined") {
		if (m_linkActive.exchange(true)) {
			log("partnerJoined ignored (already linking)");
			return;
		}
		m_roleSet = false;
		setState(State::Linking, "相手が はいりました。通信の じゅんびを しています…");
		m_resetRequested = true;
		m_expectedSequence = 0;
		m_buffered.clear();
		{
			std::lock_guard<std::mutex> lock(m_outLock);
			m_outgoing.clear();
			m_outSequence = 0;
		}
		startLinkMode();
	} else if (name == "partnerLeft") {
		m_linkActive = false;
		m_roleSet = false;
		setState(State::WaitingPartner, "相手が ぬけました。へやの 番号を つたえて、まってください");
		std::lock_guard<std::mutex> lock(m_outLock);
		m_linkStartAt = 0;
	} else if (name == "sessionClose") {
		m_inSession = false;
		m_linkActive = false;
		m_roleSet = false;
		m_deviceOn = false;
		setState(State::Finished, "通信が おわりました");
		m_stopping = true;
		std::lock_guard<std::mutex> lock(m_wsLock);
		if (m_ws) {
			m_ws->abort();
		}
	}
}

void CelioNet::deliverData(long sequence, std::vector<uint16_t> data) {
	// LinkExchangeSession.handleSocketDataToDevice
	if (sequence < m_expectedSequence) {
		return;
	}
	if (sequence > m_expectedSequence) {
		m_buffered[sequence] = std::move(data);
		return;
	}
	pushIncoming(Incoming{false, 0, std::move(data)});
	++m_expectedSequence;
	auto it = m_buffered.find(m_expectedSequence);
	while (it != m_buffered.end()) {
		pushIncoming(Incoming{false, 0, std::move(it->second)});
		m_buffered.erase(it);
		++m_expectedSequence;
		it = m_buffered.find(m_expectedSequence);
	}
}

void CelioNet::startLinkMode() {
	// LinkDeviceUtils.tryEnableLinkMode: Cancel, wait 500 ms, SetMode
	pushIncoming(Incoming{true, CmdCancel, {}});
	std::lock_guard<std::mutex> lock(m_outLock);
	m_linkStartAt = nowMs() + 500;
	m_outCond.notify_all();
}

void CelioNet::workThread() {
	std::unique_lock<std::mutex> lock(m_outLock);
	while (!m_stopping) {
		m_outCond.wait_for(lock, std::chrono::milliseconds(50));
		if (m_stopping) {
			break;
		}
		long long now = nowMs();
		if (m_linkStartAt && now >= m_linkStartAt) {
			m_linkStartAt = 0;
			lock.unlock();
			pushIncoming(Incoming{true, CmdSetMode, {}});
			if (m_kind == Kind::Direct) {
				portCommand(CmdSetMode);
			}
			lock.lock();
		}
		if (m_closeAt && now >= m_closeAt) {
			// session.ts evict(): both links closed
			m_closeAt = 0;
			lock.unlock();
			m_deviceOn = false;
			portCommand(CmdCancel);
			setState(State::Finished, "通信が おわりました");
			lock.lock();
			continue;
		}
		if (m_connected && now - m_lastReceive > 6000) {
			// The relay pings every 500 ms; silence means the socket is dead
			lock.unlock();
			log("receive timeout");
			{
				std::lock_guard<std::mutex> wsLock(m_wsLock);
				if (m_ws) {
					m_ws->abort();
				}
			}
			lock.lock();
			continue;
		}
		while (m_connected && m_inSession && !m_outgoing.empty()) {
			Outgoing item = m_outgoing.front();
			std::string text;
			if (item.isStatus) {
				text = "42[\"deviceStatus\",{\"uuid\":\"" + makeUuid() + "\",\"linkStatus\":" + std::to_string(item.status) + "}]";
			} else {
				// Celio-Server since 2026-10-03 only accepts an array of packets (isDataPacketBatch) and
				// answers a single object with ack=false and drops it. MGBA_CELIO_NET_LEGACY=1 sends the old single object.
				static const bool legacy = []() { const char* e = getenv("MGBA_CELIO_NET_LEGACY"); return e && *e && *e != '0'; }();
				text = std::string("42[\"deviceData\",") + (legacy ? "" : "[") + "{\"sequence\":" + std::to_string(m_outSequence) + ",\"data\":[";
				for (size_t i = 0; i < item.data.size(); ++i) {
					if (i) {
						text += ",";
					}
					text += std::to_string(item.data[i]);
				}
				text += legacy ? "]}]" : "]}]]";
			}
			lock.unlock();
			bool ok = sendText(text);
			lock.lock();
			if (!ok) {
				break;
			}
			if (!item.isStatus) {
				++m_outSequence;
			}
			if (!m_outgoing.empty()) {
				m_outgoing.pop_front();
			}
		}
	}
}


// ---- USB adapter -------------------------------------------------------------

bool CelioNet::portSend(uint8_t channel, const uint8_t* data, size_t size) {
	if (!m_port || size > MAX_PAYLOAD) {
		return false;
	}
	std::vector<uint8_t> frame;
	frame.reserve(5 + size);
	frame.push_back(SYNC_0);
	frame.push_back(SYNC_1);
	frame.push_back(channel);
	frame.push_back((uint8_t) (size & 0xFF));
	frame.push_back((uint8_t) (size >> 8));
	frame.insert(frame.end(), data, data + size);
	return m_port->write(frame.data(), frame.size());
}

bool CelioNet::portCommand(uint16_t command) {
	if (command > 0xFF) {
		return false;
	}
	// SetMode carries the mode (LinkDeviceUtils.enableLinkMode); the rest are one byte
	uint8_t payload[2] = {(uint8_t) command, LINK_MODE_ONLINE};
	size_t size = command == CmdSetMode ? 2 : 1;
	log("adapter command " + std::to_string(command));
	return portSend(CH_CMD, payload, size);
}

bool CelioNet::portData(const std::vector<uint16_t>& data) {
	uint8_t payload[64] = {};
	for (size_t i = 0; i < 32 && i < data.size(); ++i) {
		payload[i * 2] = (uint8_t) data[i];
		payload[i * 2 + 1] = (uint8_t) (data[i] >> 8);
	}
	return portSend(CH_DATA, payload, sizeof(payload));
}

void CelioNet::portStatus(uint16_t status) {
	uint8_t payload[2] = {(uint8_t) status, (uint8_t) (status >> 8)};
	portSend(CH_STATUS, payload, sizeof(payload));
}

static std::vector<uint16_t> unpackData(const std::vector<uint8_t>& payload) {
	std::vector<uint16_t> data(32);
	for (size_t i = 0; i < 32; ++i) {
		data[i] = (uint16_t) (payload[i * 2] | (payload[i * 2 + 1] << 8));
	}
	return data;
}

void CelioNet::portThread() {
	enum { Sync1, Sync2, Channel, LenLo, LenHi, Payload } state = Sync1;
	uint8_t channel = 0;
	size_t length = 0;
	std::vector<uint8_t> payload;
	std::vector<uint8_t> chunk;
	while (!m_stopping && m_port->read(chunk)) {
		for (uint8_t b : chunk) {
			// LinkDeviceService.feedByte
			switch (state) {
			case Sync1:
				if (b == SYNC_0) {
					state = Sync2;
				}
				break;
			case Sync2:
				state = b == SYNC_1 ? Channel : b == SYNC_0 ? Sync2 : Sync1;
				break;
			case Channel:
				channel = b;
				state = LenLo;
				break;
			case LenLo:
				length = b;
				state = LenHi;
				break;
			case LenHi:
				length |= (size_t) b << 8;
				payload.clear();
				if (length > MAX_PAYLOAD) {
					state = Sync1;
				} else if (length == 0) {
					handleFrame(channel, payload);
					state = Sync1;
				} else {
					state = Payload;
				}
				break;
			case Payload:
				payload.push_back(b);
				if (payload.size() >= length) {
					handleFrame(channel, payload);
					state = Sync1;
				}
				break;
			}
		}
	}
	if (m_stopping) {
		return;
	}
	State s = snapshot().state;
	if (s == State::Finished || s == State::Error) {
		return;
	}
	log("port closed");
	m_deviceOn = false;
	setState(State::Error, m_kind == Kind::FakeAdapter ? "相手が はずれました" : "USB の変換器が はずれました");
	if (m_kind == Kind::NetUsb) {
		m_stopping = true;
		std::lock_guard<std::mutex> lock(m_wsLock);
		if (m_ws) {
			m_ws->abort();
		}
	}
}

void CelioNet::handleFrame(uint8_t channel, const std::vector<uint8_t>& payload) {
	if (m_kind == Kind::FakeAdapter) {
		// Answer as the adapter firmware does (control.hpp, module/link.cpp) with the emulated device behind it
		if (channel == CH_CMD && !payload.empty()) {
			uint8_t command = payload[0];
			log("fake adapter command " + std::to_string(command));
			if (command == CmdGetFirmwareInfo) {
				uint8_t reply[5] = {CmdGetFirmwareInfo, 0, 0, 0, 0};
				portSend(CH_DATA, reply, sizeof(reply));
			} else {
				pushIncoming(Incoming{true, command, {}});
			}
		} else if (channel == CH_DATA && payload.size() == 64) {
			pushIncoming(Incoming{false, 0, unpackData(payload)});
		}
		return;
	}
	if (channel == CH_STATUS && payload.size() == 2) {
		uint16_t status = (uint16_t) (payload[0] | (payload[1] << 8));
		log("adapter status " + std::to_string(status));
		if (m_kind == Kind::NetUsb) {
			noteStatus(status);
			queueOutgoing(Outgoing{true, status, {}});
		} else if (m_kind == Kind::Direct) {
			sessionStatus(1, status);
		}
	} else if (channel == CH_DATA && payload.size() == 64) {
		if (m_kind == Kind::NetUsb) {
			queueOutgoing(Outgoing{false, 0, unpackData(payload)});
		} else if (m_kind == Kind::Direct) {
			// The serial line keeps the order, so no sequence numbers here
			pushIncoming(Incoming{false, 0, unpackData(payload)});
		}
	} else if (channel == CH_DATA && payload.size() >= 4 && payload[0] == CmdGetFirmwareInfo) {
		m_firmwareSeen = true;
		log("adapter firmware " + std::to_string(payload[1]) + "." + std::to_string(payload[2]) + "." + std::to_string(payload[3]));
	}
}

void CelioNet::sessionCommand(int who, uint16_t command) {
	if (who == 0) {
		pushIncoming(Incoming{true, command, {}});
	} else {
		portCommand(command);
	}
}

// Celio-Server session.ts handleStatusMessage between the two local devices
void CelioNet::sessionStatus(int who, uint16_t status) {
	// LinkExchangeSession.handleDeviceStatusToSocket drops these before the server
	if (status == DeviceReady || status == EmuTradeSessionFinished || status == StatusDebug) {
		return;
	}
	std::lock_guard<std::mutex> lock(m_sessionLock);
	int other = 1 - who;
	switch (status) {
	case AwaitMode:
		sessionCommand(who, m_masterSelected ? CmdSetModeSlave : CmdSetModeMaster);
		m_masterSelected = true;
		break;
	case AwaitModeEmulator:
		sessionCommand(who, CmdSetModeSlave);
		break;
	case HandshakeReceived:
		m_sessionStatus[who] = HandshakeReceived;
		if (m_sessionStatus[other] == HandshakeReceived) {
			sessionCommand(who, CmdStartHandshake);
			sessionCommand(other, CmdStartHandshake);
		}
		break;
	case HandshakeFinished:
	case LinkReconnecting:
		m_sessionStatus[who] = status;
		break;
	case LinkConnected:
		m_sessionStatus[who] = LinkConnected;
		sessionCommand(other, CmdConnectLink);
		break;
	case LinkClosed:
		m_sessionStatus[who] = LinkClosed;
		if (m_sessionStatus[other] == LinkClosed) {
			std::lock_guard<std::mutex> outLock(m_outLock);
			m_closeAt = nowMs() + 2000;
		}
		break;
	default:
		break;
	}
}

std::vector<CelioNet::SerialPortInfo> CelioNet::listSerialPorts() {
	std::vector<SerialPortInfo> ports;
	// GUID_DEVCLASS_PORTS
	static const GUID portsClass = {0x4d36e978, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};
	HDEVINFO set = SetupDiGetClassDevsW(&portsClass, nullptr, nullptr, DIGCF_PRESENT);
	if (set == INVALID_HANDLE_VALUE) {
		return ports;
	}
	SP_DEVINFO_DATA info{};
	info.cbSize = sizeof(info);
	for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &info); ++i) {
		HKEY key = SetupDiOpenDevRegKey(set, &info, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
		if (key == INVALID_HANDLE_VALUE) {
			continue;
		}
		char name[64] = {};
		DWORD size = sizeof(name) - 1;
		LONG ok = RegQueryValueExA(key, "PortName", nullptr, nullptr, (LPBYTE) name, &size);
		RegCloseKey(key);
		if (ok != ERROR_SUCCESS || strncmp(name, "COM", 3) != 0) {
			continue;
		}
		char hwid[512] = {};
		SetupDiGetDeviceRegistryPropertyA(set, &info, SPDRP_HARDWAREID, nullptr, (PBYTE) hwid, sizeof(hwid) - 2, nullptr);
		wchar_t wdesc[256] = {};
		SetupDiGetDeviceRegistryPropertyW(set, &info, SPDRP_FRIENDLYNAME, nullptr, (PBYTE) wdesc, sizeof(wdesc) - sizeof(wchar_t), nullptr);
		char desc[768] = {};
		WideCharToMultiByte(CP_UTF8, 0, wdesc, -1, desc, sizeof(desc) - 1, nullptr, nullptr);
		std::string id = hwid;
		for (char& c : id) {
			c = (char) toupper((unsigned char) c);
		}
		ports.push_back(SerialPortInfo{name, desc, id.find("VID_2FE3") != std::string::npos});
	}
	SetupDiDestroyDeviceInfoList(set);
	std::stable_sort(ports.begin(), ports.end(), [](const SerialPortInfo& a, const SerialPortInfo& b) {
		return a.adapter && !b.adapter;
	});
	return ports;
}

}

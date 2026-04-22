#include "a2s_server.h"
#include "config.h"
#include "windrose_engine.h"

#include <random>
#include <chrono>
#include <cstring>
#include <cctype>

extern QueryConfig g_Config;
extern void LogMessage(const std::string& message);

namespace {
    // Small helpers to build little-endian A2S packets.
    class Writer {
    public:
        Writer() { buf.reserve(1400); }

        void WriteU8(uint8_t v) { buf.push_back(v); }

        void WriteI32(int32_t v) {
            uint8_t b[4];
            std::memcpy(b, &v, 4);
            buf.insert(buf.end(), b, b + 4);
        }

        void WriteU16(uint16_t v) {
            uint8_t b[2];
            std::memcpy(b, &v, 2);
            buf.insert(buf.end(), b, b + 2);
        }

        void WriteF32(float v) {
            uint8_t b[4];
            std::memcpy(b, &v, 4);
            buf.insert(buf.end(), b, b + 4);
        }

        void WriteCString(const std::string& s) {
            buf.insert(buf.end(), s.begin(), s.end());
            buf.push_back(0);
        }

        void WriteSimpleHeader() {
            // -1 as int32 (0xFFFFFFFF) = single packet response marker
            WriteI32(-1);
        }

        const std::vector<uint8_t>& data() const { return buf; }
        size_t size() const { return buf.size(); }

    private:
        std::vector<uint8_t> buf;
    };

    class Reader {
    public:
        Reader(const uint8_t* d, int sz) : data(d), size(sz), pos(0) {}

        bool Good(int need) const { return pos + need <= size; }

        int32_t ReadI32() {
            int32_t v = 0;
            if (Good(4)) { std::memcpy(&v, data + pos, 4); pos += 4; }
            return v;
        }

        uint8_t ReadU8() {
            return Good(1) ? data[pos++] : 0;
        }

        std::string ReadCString() {
            std::string out;
            while (Good(1)) {
                uint8_t c = data[pos++];
                if (c == 0) break;
                out.push_back((char)c);
            }
            return out;
        }

        int Remaining() const { return size - pos; }
        const uint8_t* CurrentPtr() const { return data + pos; }

    private:
        const uint8_t* data;
        int size;
        int pos;
    };

    std::string WStringToUtf8(const std::wstring& ws) {
        if (ws.empty()) return {};
        int needed = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(),
                                         nullptr, 0, nullptr, nullptr);
        if (needed <= 0) return {};
        std::string out(needed, '\0');
        WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(),
                            out.data(), needed, nullptr, nullptr);
        return out;
    }

    uint64_t SockaddrKey(const sockaddr_in& from) {
        // IP (32) + port (16) packed
        return ((uint64_t)from.sin_addr.s_addr << 16) | (uint64_t)from.sin_port;
    }

    bool IsRunningUnderWine() {
        HKEY key = nullptr;
        LONG regResult = RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Wine", 0,
                                       KEY_QUERY_VALUE, &key);
        if (regResult == ERROR_SUCCESS) {
            RegCloseKey(key);
            return true;
        }

        key = nullptr;
        regResult = RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\Wine", 0,
                                  KEY_QUERY_VALUE, &key);
        if (regResult == ERROR_SUCCESS) {
            RegCloseKey(key);
            return true;
        }

        HMODULE ntdll = GetModuleHandleA("ntdll.dll");
        return ntdll && GetProcAddress(ntdll, "wine_get_version") != nullptr;
    }

    void WriteU16BE(std::vector<uint8_t>& buf, size_t offset, uint16_t value) {
        buf[offset] = (uint8_t)((value >> 8) & 0xFF);
        buf[offset + 1] = (uint8_t)(value & 0xFF);
    }

    uint16_t ReadU16BE(const uint8_t* data) {
        return (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
    }

    uint16_t InternetChecksum(const uint8_t* data, size_t size) {
        uint32_t sum = 0;
        size_t pos = 0;

        while (pos + 1 < size) {
            sum += ((uint32_t)data[pos] << 8) | data[pos + 1];
            pos += 2;
        }

        if (pos < size) {
            sum += (uint32_t)data[pos] << 8;
        }

        while (sum >> 16) {
            sum = (sum & 0xFFFF) + (sum >> 16);
        }

        return (uint16_t)(~sum & 0xFFFF);
    }

    std::string TrimAscii(const std::string& value) {
        size_t begin = 0;
        size_t end = value.size();

        while (begin < end && std::isspace((unsigned char)value[begin])) begin++;
        while (end > begin && std::isspace((unsigned char)value[end - 1])) end--;

        return value.substr(begin, end - begin);
    }

    bool ParseIPv4Address(const std::string& input, in_addr& out, std::string& normalized) {
        std::string host = TrimAscii(input);
        if (host.size() >= 2 &&
            ((host.front() == '"' && host.back() == '"') ||
             (host.front() == '\'' && host.back() == '\''))) {
            host = TrimAscii(host.substr(1, host.size() - 2));
        }

        if (host.empty()) return false;

        uint32_t octets[4] = {};
        size_t pos = 0;

        for (int i = 0; i < 4; ++i) {
            if (pos >= host.size() || !std::isdigit((unsigned char)host[pos])) {
                return false;
            }

            uint32_t value = 0;
            while (pos < host.size() && std::isdigit((unsigned char)host[pos])) {
                value = (value * 10) + (uint32_t)(host[pos] - '0');
                if (value > 255) return false;
                pos++;
            }

            octets[i] = value;

            if (i < 3) {
                if (pos >= host.size() || host[pos] != '.') return false;
                pos++;
            } else if (pos != host.size()) {
                return false;
            }
        }

        uint32_t hostOrder =
            (octets[0] << 24) |
            (octets[1] << 16) |
            (octets[2] << 8) |
            octets[3];

        out.s_addr = htonl(hostOrder);
        normalized = std::to_string(octets[0]) + "." +
                     std::to_string(octets[1]) + "." +
                     std::to_string(octets[2]) + "." +
                     std::to_string(octets[3]);
        return true;
    }
}

A2SServer::A2SServer()
    : udpSocket(INVALID_SOCKET),
      rawSocket(INVALID_SOCKET),
      running(false),
      m_Port(27015),
      m_BindAddr{},
      m_HasBindAddr(false),
      m_IsWine(IsRunningUnderWine()),
      m_UseRawListener(false),
      m_RawSocketAttempted(false),
      m_RawSendActiveLogged(false),
      m_RawSendErrorLogged(false),
      listenerThread(nullptr) {
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
}

A2SServer::~A2SServer() {
    Stop();
    WSACleanup();
}

bool A2SServer::Start(int port, const std::string& bindHost) {
    if (running) return false;

    m_Port = port;
    m_BindHost.clear();
    m_BindAddr.s_addr = INADDR_ANY;
    m_HasBindAddr = false;
    m_UseRawListener = false;
    m_RawSocketAttempted = false;
    m_RawSendActiveLogged = false;
    m_RawSendErrorLogged = false;

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons((u_short)m_Port);

    std::string trimmedBindHost = TrimAscii(bindHost);
    if (trimmedBindHost.empty()) {
        serverAddr.sin_addr.s_addr = INADDR_ANY;
    } else {
        in_addr parsedAddr{};
        std::string normalizedAddr;

        // Wine can report valid dotted IPv4 input as rejected through InetPtonA,
        // so parse the simple -MultiHome IPv4 form locally.
        if (!ParseIPv4Address(trimmedBindHost, parsedAddr, normalizedAddr)) {
            LogMessage("A2S: Invalid -MultiHome address '" + bindHost +
                       "', refusing to fall back to 0.0.0.0");
            closesocket(udpSocket);
            udpSocket = INVALID_SOCKET;
            return false;
        }

        serverAddr.sin_addr = parsedAddr;
        m_BindHost = normalizedAddr;
        m_BindAddr = parsedAddr;
        m_HasBindAddr = true;
    }

    if (m_IsWine && m_HasBindAddr) {
        if (!OpenRawSocket()) {
            LogMessage("A2S: Failed to start raw IPv4 listener for Wine -MultiHome " +
                       m_BindHost + ":" + std::to_string(m_Port));
            return false;
        }

        m_UseRawListener = true;
        running = true;
        listenerThread = new std::thread(&A2SServer::ListenerLoop, this);

        LogMessage("A2S: Wine raw IPv4 query listener active on " +
                   m_BindHost + ":" + std::to_string(m_Port) +
                   " (no Winsock UDP bind)");
        return true;
    }

    udpSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udpSocket == INVALID_SOCKET) {
        LogMessage("A2S: Failed to create UDP socket");
        return false;
    }

    if (bind(udpSocket, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        int err = WSAGetLastError();
        LogMessage("A2S: Failed to bind UDP socket on " +
                   (m_BindHost.empty() ? std::string("0.0.0.0") : m_BindHost) +
                   ":" + std::to_string(m_Port) +
                   " (WSA error " + std::to_string(err) + ")");
        closesocket(udpSocket);
        udpSocket = INVALID_SOCKET;
        return false;
    }

    running = true;
    listenerThread = new std::thread(&A2SServer::ListenerLoop, this);

    LogMessage("A2S: Query server listening on " +
               (m_BindHost.empty() ? std::string("0.0.0.0") : m_BindHost) +
               ":" + std::to_string(m_Port));
    return true;
}

bool A2SServer::OpenRawSocket() {
    if (rawSocket != INVALID_SOCKET) return true;
    if (m_RawSocketAttempted) return false;

    m_RawSocketAttempted = true;
    rawSocket = socket(AF_INET, SOCK_RAW, IPPROTO_UDP);
    if (rawSocket == INVALID_SOCKET) {
        LogMessage("A2S: Raw IPv4 socket unavailable; Wine -MultiHome cannot avoid UDP wildcard bind " +
                   std::string("(WSA error ") + std::to_string(WSAGetLastError()) + ")");
        return false;
    }

    int hdrIncl = 1;
    if (setsockopt(rawSocket, IPPROTO_IP, IP_HDRINCL,
                   (const char*)&hdrIncl, sizeof(hdrIncl)) == SOCKET_ERROR) {
        LogMessage("A2S: IP_HDRINCL failed on raw IPv4 socket " +
                   std::string("(WSA error ") + std::to_string(WSAGetLastError()) + ")");
        closesocket(rawSocket);
        rawSocket = INVALID_SOCKET;
        return false;
    }

    return true;
}

bool A2SServer::SendRawPacket(const sockaddr_in& to, const std::vector<uint8_t>& payload) {
    if (!m_UseRawListener || !m_HasBindAddr) return false;
    if (payload.size() + 28 > 0xFFFF) return false;
    if (!OpenRawSocket()) return false;

    static uint16_t packetId = 1;

    const size_t ipHeaderSize = 20;
    const size_t udpHeaderSize = 8;
    const size_t packetSize = ipHeaderSize + udpHeaderSize + payload.size();
    std::vector<uint8_t> packet(packetSize, 0);

    packet[0] = 0x45; // IPv4, 20-byte header
    packet[1] = 0;
    WriteU16BE(packet, 2, (uint16_t)packetSize);
    WriteU16BE(packet, 4, packetId++);
    WriteU16BE(packet, 6, 0);
    packet[8] = 64;
    packet[9] = IPPROTO_UDP;
    std::memcpy(packet.data() + 12, &m_BindAddr.s_addr, 4);
    std::memcpy(packet.data() + 16, &to.sin_addr.s_addr, 4);
    WriteU16BE(packet, 10, InternetChecksum(packet.data(), ipHeaderSize));

    size_t udpOffset = ipHeaderSize;
    WriteU16BE(packet, udpOffset, (uint16_t)m_Port);
    std::memcpy(packet.data() + udpOffset + 2, &to.sin_port, 2);
    WriteU16BE(packet, udpOffset + 4, (uint16_t)(udpHeaderSize + payload.size()));
    WriteU16BE(packet, udpOffset + 6, 0); // UDP checksum is optional for IPv4.
    std::memcpy(packet.data() + udpOffset + udpHeaderSize, payload.data(), payload.size());

    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_addr = to.sin_addr;
    dst.sin_port = 0;

    int sent = sendto(rawSocket, (const char*)packet.data(), (int)packet.size(), 0,
                      (const sockaddr*)&dst, sizeof(dst));
    if (sent == (int)packet.size()) {
        if (!m_RawSendActiveLogged) {
            m_RawSendActiveLogged = true;
            LogMessage("A2S: Raw IPv4 replies active from " +
                       m_BindHost + ":" + std::to_string(m_Port));
        }
        return true;
    }

    if (!m_RawSendErrorLogged) {
        m_RawSendErrorLogged = true;
        LogMessage("A2S: Raw IPv4 reply failed (WSA error " +
                   std::to_string(WSAGetLastError()) + ")");
    }

    return false;
}

void A2SServer::SendPacket(const sockaddr_in& to, const std::vector<uint8_t>& payload) {
    if (m_UseRawListener) {
        SendRawPacket(to, payload);
        return;
    }

    if (udpSocket != INVALID_SOCKET) {
        sendto(udpSocket, (const char*)payload.data(), (int)payload.size(), 0,
               (const sockaddr*)&to, sizeof(to));
    }
}

void A2SServer::Stop() {
    bool wasRunning = running;
    running = false;

    if (udpSocket != INVALID_SOCKET) {
        closesocket(udpSocket);
        udpSocket = INVALID_SOCKET;
    }

    if (rawSocket != INVALID_SOCKET) {
        closesocket(rawSocket);
        rawSocket = INVALID_SOCKET;
    }

    if (listenerThread && listenerThread->joinable()) {
        listenerThread->join();
        delete listenerThread;
        listenerThread = nullptr;
    }

    if (wasRunning) {
        LogMessage("A2S: Query server stopped");
    }
}

void A2SServer::ListenerLoop() {
    uint8_t buffer[2048];
    while (running) {
        SOCKET listenSocket = m_UseRawListener ? rawSocket : udpSocket;
        if (listenSocket == INVALID_SOCKET) break;

        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(listenSocket, &readSet);

        timeval timeout{1, 0};
        int result = select(0, &readSet, nullptr, nullptr, &timeout);
        if (result <= 0) continue;
        if (!FD_ISSET(listenSocket, &readSet)) continue;

        sockaddr_in from{};
        int fromLen = sizeof(from);
        int n = recvfrom(listenSocket, (char*)buffer, sizeof(buffer), 0,
                         (sockaddr*)&from, &fromLen);
        if (n <= 0) continue;

        if (m_UseRawListener) {
            HandleRawDatagram(buffer, n);
        } else {
            HandleDatagram(buffer, n, from);
        }
    }
}

void A2SServer::HandleRawDatagram(const uint8_t* data, int size) {
    if (!m_HasBindAddr || size < 28) return;

    uint8_t version = data[0] >> 4;
    uint8_t headerLen = (uint8_t)((data[0] & 0x0F) * 4);
    if (version != 4 || headerLen < 20) return;
    if (size < headerLen + 8) return;
    if (data[9] != IPPROTO_UDP) return;

    uint16_t totalLen = ReadU16BE(data + 2);
    if (totalLen < headerLen + 8) return;
    if (totalLen > (uint16_t)size) totalLen = (uint16_t)size;

    uint16_t fragment = ReadU16BE(data + 6);
    if ((fragment & 0x3FFF) != 0) return;

    uint32_t dstAddr = 0;
    std::memcpy(&dstAddr, data + 16, sizeof(dstAddr));
    if (dstAddr != m_BindAddr.s_addr) return;

    const uint8_t* udp = data + headerLen;
    uint16_t srcPort = ReadU16BE(udp);
    uint16_t dstPort = ReadU16BE(udp + 2);
    uint16_t udpLen = ReadU16BE(udp + 4);
    if (dstPort != (uint16_t)m_Port) return;
    if (udpLen < 8 || headerLen + udpLen > totalLen) return;

    sockaddr_in from{};
    from.sin_family = AF_INET;
    std::memcpy(&from.sin_addr.s_addr, data + 12, sizeof(from.sin_addr.s_addr));
    from.sin_port = htons(srcPort);

    HandleDatagram(udp + 8, (int)udpLen - 8, from);
}

int32_t A2SServer::IssueChallenge(const sockaddr_in& from) {
    static thread_local std::mt19937 rng(
        (unsigned)std::chrono::steady_clock::now().time_since_epoch().count());
    std::uniform_int_distribution<int32_t> dist(1, 0x7FFFFFFE);

    int32_t token = dist(rng);
    std::lock_guard<std::mutex> lock(challengeMutex);
    challenges[SockaddrKey(from)] = token;
    return token;
}

bool A2SServer::VerifyChallenge(const sockaddr_in& from, int32_t token) {
    std::lock_guard<std::mutex> lock(challengeMutex);
    auto it = challenges.find(SockaddrKey(from));
    if (it == challenges.end()) return false;
    return it->second == token;
}

void A2SServer::HandleDatagram(const uint8_t* data, int size, const sockaddr_in& from) {
    Reader r(data, size);
    if (!r.Good(5)) return;

    int32_t header = r.ReadI32();
    if (header != -1) return; // Not a simple/single-packet request
    uint8_t kind = r.ReadU8();

    switch (kind) {
        case A2S::REQ_INFO: {
            // Payload: "Source Engine Query\0" [optional challenge int32]
            std::string payload = r.ReadCString();
            if (payload != A2S::INFO_PAYLOAD) return;

            // Match proven query behavior: any valid A2S_INFO
            // prefix gets the info response directly. Some admin tools never
            // retry INFO after an S2C_CHALLENGE.
            SendInfo(from);
            break;
        }
        case A2S::REQ_PLAYER: {
            if (r.Remaining() < 4) {
                SendChallenge(from, A2S::REQ_PLAYER);
                break;
            }
            int32_t token = r.ReadI32();
            if (token == -1 || !VerifyChallenge(from, token)) {
                SendChallenge(from, A2S::REQ_PLAYER);
                break;
            }
            SendPlayer(from);
            break;
        }
        case A2S::REQ_RULES: {
            if (r.Remaining() < 4) {
                SendChallenge(from, A2S::REQ_RULES);
                break;
            }
            int32_t token = r.ReadI32();
            if (token == -1 || !VerifyChallenge(from, token)) {
                SendChallenge(from, A2S::REQ_RULES);
                break;
            }
            SendRules(from);
            break;
        }
        case A2S::REQ_CHALLENGE: {
            SendChallenge(from, A2S::REQ_CHALLENGE);
            break;
        }
        case A2S::REQ_PING: {
            SendPong(from);
            break;
        }
        default:
            break;
    }
}

void A2SServer::SendChallenge(const sockaddr_in& to, uint8_t /*forHeader*/) {
    int32_t token = IssueChallenge(to);

    Writer w;
    w.WriteSimpleHeader();
    w.WriteU8(A2S::RESP_CHALLENGE);
    w.WriteI32(token);

    SendPacket(to, w.data());
}

void A2SServer::SendPong(const sockaddr_in& to) {
    Writer w;
    w.WriteSimpleHeader();
    w.WriteU8(A2S::RESP_PONG);
    w.WriteCString("00000000000000");
    SendPacket(to, w.data());
}

void A2SServer::SendInfo(const sockaddr_in& to) {
    // Always respond from the cached snapshot - never block the query thread
    // on a live GObjects scan. The snapshot refresher keeps this ~1-2s fresh.
    UnrealEngine::PlayerSnapshot snap;
    if (UnrealEngine::g_Snapshot) {
        snap = UnrealEngine::g_Snapshot->Get();
    }

    size_t playerCount = snap.players.size();
    const ServerMetadata& meta = snap.meta;

    std::string name = !meta.serverName.empty() ? meta.serverName : g_Config.serverName;
    std::string map = g_Config.map;
    std::string folder = g_Config.gameFolder;
    std::string game = g_Config.gameDescription;
    std::string version = !meta.deploymentId.empty() ? meta.deploymentId : g_Config.version;

    uint8_t curPlayers = (uint8_t)(playerCount > 255 ? 255 : playerCount);
    uint8_t maxPlayers = g_Config.maxPlayers;
    if (!meta.maxPlayers.empty()) {
        try {
            int v = std::stoi(meta.maxPlayers);
            if (v > 0 && v <= 255) maxPlayers = (uint8_t)v;
        } catch (...) {}
    }

    Writer w;
    w.WriteSimpleHeader();
    w.WriteU8(A2S::RESP_INFO);
    w.WriteU8(17);                     // Protocol version
    w.WriteCString(name);              // Server name
    w.WriteCString(map);               // Map
    w.WriteCString(folder);            // Game folder
    w.WriteCString(game);              // Game description
    w.WriteU16(g_Config.appId);        // Steam AppID
    w.WriteU8(curPlayers);             // Players
    w.WriteU8(maxPlayers);             // Max players
    w.WriteU8(g_Config.botCount);      // Bots
    w.WriteU8(A2S::SERVER_TYPE_DEDICATED);
    w.WriteU8(A2S::ENV_WINDOWS);
    // Prefer IsPasswordProtected from ServerDescription.json when available,
    // fall back to the compile-time default otherwise.
    bool isPrivate = meta.passwordKnown ? meta.passwordProtected : g_Config.privateServer;
    w.WriteU8(isPrivate ? 1 : 0);
    w.WriteU8(g_Config.vacSecured ? 1 : 0);
    w.WriteCString(version);           // Version string
    w.WriteU8(0);                      // EDF: no optional fields

    SendPacket(to, w.data());
}

void A2SServer::SendPlayer(const sockaddr_in& to) {
    UnrealEngine::PlayerSnapshot snap;
    if (UnrealEngine::g_Snapshot) {
        snap = UnrealEngine::g_Snapshot->Get();
    }
    std::vector<PlayerInfo>& players = snap.players;

    if (players.size() > 255) players.resize(255);

    Writer w;
    w.WriteSimpleHeader();
    w.WriteU8(A2S::RESP_PLAYER);
    w.WriteU8((uint8_t)players.size());

    uint8_t index = 0;
    for (const auto& p : players) {
        std::string name = WStringToUtf8(p.playerName);
        if (name.empty()) {
            // Mirrors WindrosePlus' fallback: we can't access PlayerId without
            // a verified offset, so the stable list index is used instead.
            name = "Player " + std::to_string((int)index + 1);
        }
        w.WriteU8(index++);
        w.WriteCString(name);
        w.WriteI32(p.score);
        w.WriteF32(p.connectedSeconds);
    }

    SendPacket(to, w.data());
}

void A2SServer::SendRules(const sockaddr_in& to) {
    UnrealEngine::PlayerSnapshot snap;
    if (UnrealEngine::g_Snapshot) {
        snap = UnrealEngine::g_Snapshot->Get();
    }
    const ServerMetadata& meta = snap.meta;

    std::vector<std::pair<std::string, std::string>> rules;
    rules.emplace_back("game", g_Config.gameDescription);
    rules.emplace_back("map", g_Config.map);
    if (!meta.inviteCode.empty())    rules.emplace_back("invite_code", meta.inviteCode);
    if (!meta.deploymentId.empty())  rules.emplace_back("deployment_id", meta.deploymentId);
    if (!meta.serverAddress.empty()) rules.emplace_back("server_address", meta.serverAddress);
    if (!meta.serverPort.empty())    rules.emplace_back("server_port", meta.serverPort);
    rules.emplace_back("max_players", std::to_string((int)g_Config.maxPlayers));

    if (rules.size() > 0xFFFF) rules.resize(0xFFFF);

    Writer w;
    w.WriteSimpleHeader();
    w.WriteU8(A2S::RESP_RULES);
    w.WriteU16((uint16_t)rules.size());
    for (const auto& [k, v] : rules) {
        w.WriteCString(k);
        w.WriteCString(v);
    }

    SendPacket(to, w.data());
}

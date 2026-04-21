#include "a2s_server.h"
#include "config.h"
#include "windrose_engine.h"

#include <random>
#include <chrono>
#include <cstring>

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
}

A2SServer::A2SServer() : udpSocket(INVALID_SOCKET), running(false), m_Port(27015), listenerThread(nullptr) {
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
}

A2SServer::~A2SServer() {
    Stop();
    WSACleanup();
}

bool A2SServer::Start(int port) {
    m_Port = port;
    if (running) return false;

    udpSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udpSocket == INVALID_SOCKET) {
        LogMessage("A2S: Failed to create UDP socket");
        return false;
    }

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons((u_short)m_Port);

    if (bind(udpSocket, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        LogMessage("A2S: Failed to bind UDP socket on port " + std::to_string(m_Port));
        closesocket(udpSocket);
        udpSocket = INVALID_SOCKET;
        return false;
    }

    running = true;
    listenerThread = new std::thread(&A2SServer::ListenerLoop, this);

    LogMessage("A2S: Query server started on UDP port " + std::to_string(m_Port));
    return true;
}

void A2SServer::Stop() {
    if (!running) return;
    running = false;

    if (udpSocket != INVALID_SOCKET) {
        closesocket(udpSocket);
        udpSocket = INVALID_SOCKET;
    }

    if (listenerThread && listenerThread->joinable()) {
        listenerThread->join();
        delete listenerThread;
        listenerThread = nullptr;
    }

    LogMessage("A2S: Query server stopped");
}

void A2SServer::ListenerLoop() {
    uint8_t buffer[1400];
    while (running) {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(udpSocket, &readSet);

        timeval timeout{1, 0};
        int result = select(0, &readSet, nullptr, nullptr, &timeout);
        if (result <= 0) continue;
        if (!FD_ISSET(udpSocket, &readSet)) continue;

        sockaddr_in from{};
        int fromLen = sizeof(from);
        int n = recvfrom(udpSocket, (char*)buffer, sizeof(buffer), 0,
                         (sockaddr*)&from, &fromLen);
        if (n <= 0) continue;

        HandleDatagram(buffer, n, from);
    }
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

            // If a challenge is provided, verify it. Otherwise issue one.
            if (r.Remaining() >= 4) {
                int32_t token = r.ReadI32();
                if (VerifyChallenge(from, token)) {
                    SendInfo(from);
                } else {
                    SendChallenge(from, A2S::REQ_INFO);
                }
            } else {
                // Spec allows responding directly to A2S_INFO without challenge
                // but modern clients retry with challenge. We send the challenge
                // to match Valve's current behavior.
                SendChallenge(from, A2S::REQ_INFO);
            }
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

    sendto(udpSocket, (const char*)w.data().data(), (int)w.size(), 0,
           (const sockaddr*)&to, sizeof(to));
}

void A2SServer::SendPong(const sockaddr_in& to) {
    Writer w;
    w.WriteSimpleHeader();
    w.WriteU8(A2S::RESP_PONG);
    w.WriteCString("00000000000000");
    sendto(udpSocket, (const char*)w.data().data(), (int)w.size(), 0,
           (const sockaddr*)&to, sizeof(to));
}

void A2SServer::SendInfo(const sockaddr_in& to) {
    // Pull live data from engine + ServerDescription.json
    size_t playerCount = 0;
    ServerMetadata meta;

    if (UnrealEngine::g_Engine && UnrealEngine::g_Engine->IsInitialized()) {
        playerCount = UnrealEngine::g_Engine->GetAllPlayers().size();
        meta = UnrealEngine::g_Engine->GetServerMetadata();
    }

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
    w.WriteU8(g_Config.privateServer ? 1 : 0);
    w.WriteU8(g_Config.vacSecured ? 1 : 0);
    w.WriteCString(version);           // Version string
    // EDF (Extra Data Flag): omit to keep packet minimal.

    sendto(udpSocket, (const char*)w.data().data(), (int)w.size(), 0,
           (const sockaddr*)&to, sizeof(to));
}

void A2SServer::SendPlayer(const sockaddr_in& to) {
    std::vector<PlayerInfo> players;
    if (UnrealEngine::g_Engine && UnrealEngine::g_Engine->IsInitialized()) {
        players = UnrealEngine::g_Engine->GetAllPlayers();
    }

    if (players.size() > 255) players.resize(255);

    Writer w;
    w.WriteSimpleHeader();
    w.WriteU8(A2S::RESP_PLAYER);
    w.WriteU8((uint8_t)players.size());

    uint8_t index = 0;
    for (const auto& p : players) {
        w.WriteU8(index++);
        w.WriteCString(WStringToUtf8(p.playerName));
        w.WriteI32(p.score);
        w.WriteF32(p.connectedSeconds);
    }

    sendto(udpSocket, (const char*)w.data().data(), (int)w.size(), 0,
           (const sockaddr*)&to, sizeof(to));
}

void A2SServer::SendRules(const sockaddr_in& to) {
    ServerMetadata meta;
    if (UnrealEngine::g_Engine && UnrealEngine::g_Engine->IsInitialized()) {
        meta = UnrealEngine::g_Engine->GetServerMetadata();
    }

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

    sendto(udpSocket, (const char*)w.data().data(), (int)w.size(), 0,
           (const sockaddr*)&to, sizeof(to));
}

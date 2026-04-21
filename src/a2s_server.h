#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <unordered_map>
#include <cstdint>

#pragma comment(lib, "ws2_32.lib")

// Source A2S query headers
// Reference: https://developer.valvesoftware.com/wiki/Server_queries
namespace A2S {
    // Request headers (single-packet form: 0xFFFFFFFF prefix + byte)
    constexpr uint8_t REQ_INFO    = 0x54; // A2S_INFO
    constexpr uint8_t REQ_PLAYER  = 0x55; // A2S_PLAYER
    constexpr uint8_t REQ_RULES   = 0x56; // A2S_RULES
    constexpr uint8_t REQ_PING    = 0x69; // A2S_PING (legacy)

    // Response headers
    constexpr uint8_t RESP_INFO      = 0x49; // 'I'
    constexpr uint8_t RESP_PLAYER    = 0x44; // 'D'
    constexpr uint8_t RESP_RULES     = 0x45; // 'E'
    constexpr uint8_t RESP_CHALLENGE = 0x41; // 'A'
    constexpr uint8_t RESP_PONG      = 0x6A; // 'j'

    constexpr const char* INFO_PAYLOAD = "Source Engine Query";

    // Server types
    constexpr uint8_t SERVER_TYPE_DEDICATED = 'd';
    constexpr uint8_t SERVER_TYPE_LISTEN    = 'l';
    constexpr uint8_t ENV_WINDOWS           = 'w';
}

class A2SServer {
private:
    SOCKET udpSocket;
    bool running;
    int m_Port;
    std::thread* listenerThread;

    std::mutex challengeMutex;
    // Map client IP -> last issued challenge token
    std::unordered_map<uint64_t, int32_t> challenges;

    void ListenerLoop();
    void HandleDatagram(const uint8_t* data, int size, const sockaddr_in& from);

    void SendInfo(const sockaddr_in& to);
    void SendPlayer(const sockaddr_in& to);
    void SendRules(const sockaddr_in& to);
    void SendChallenge(const sockaddr_in& to, uint8_t forHeader);
    void SendPong(const sockaddr_in& to);

    int32_t IssueChallenge(const sockaddr_in& from);
    bool VerifyChallenge(const sockaddr_in& from, int32_t token);

public:
    A2SServer();
    ~A2SServer();
    bool Start(int port = 27015);
    void Stop();
    bool IsRunning() const { return running; }
};

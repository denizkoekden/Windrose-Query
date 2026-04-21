#pragma once
#include <string>
#include <cstdint>

// Runtime configuration.
//
// Values come from the Unreal command line (-QueryPort, -MultiHome) with
// compile-time fallbacks below. Server metadata (name, max players, version)
// is read from ServerDescription.json at query time and falls back to the
// defaults defined here when the file is missing.
struct QueryConfig {
    // From -QueryPort on the CLI; defaults to 27015 when the switch is absent.
    int port = 27015;

    // From -MultiHome on the CLI; empty string means bind on all interfaces
    // (INADDR_ANY). Unreal servers that pass -MultiHome bind their game port
    // to a specific IP, so we match that for the query socket as well.
    std::string multiHome;

    // Fallback values used only when ServerDescription.json is unavailable.
    std::string serverName = "Windrose Server";
    std::string map = "Windrose";
    std::string gameFolder = "windrose";
    std::string gameDescription = "Windrose";
    uint16_t appId = 0;
    uint8_t maxPlayers = 64;
    uint8_t botCount = 0;
    bool vacSecured = false;
    bool privateServer = false;
    std::string version = "1.0.0";

    // Log path is always <exe_dir>\windrosequery\query.log
    std::string logFile = "windrosequery\\query.log";
};

extern QueryConfig g_Config;

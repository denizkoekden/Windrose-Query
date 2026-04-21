#pragma once
#include <string>
#include <fstream>
#include <sstream>
#include <windows.h>

struct QueryConfig {
    int port = 27015;
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
    bool enableLogging = true;
    std::string logFile = "windrosequery\\query.log";

    void Load(const std::string& configPath) {
        std::ifstream file(configPath);
        if (!file.is_open()) {
            return;
        }

        std::string line;
        while (std::getline(file, line)) {
            if (line.empty() || line[0] == '#' || line[0] == ';' || line[0] == '[') continue;

            size_t pos = line.find('=');
            if (pos == std::string::npos) continue;

            std::string key = line.substr(0, pos);
            std::string value = line.substr(pos + 1);

            key.erase(0, key.find_first_not_of(" \t"));
            key.erase(key.find_last_not_of(" \t") + 1);
            value.erase(0, value.find_first_not_of(" \t"));
            value.erase(value.find_last_not_of(" \t") + 1);

            if (key == "Port") {
                port = std::stoi(value);
            } else if (key == "ServerName") {
                serverName = value;
            } else if (key == "Map") {
                map = value;
            } else if (key == "GameFolder") {
                gameFolder = value;
            } else if (key == "GameDescription") {
                gameDescription = value;
            } else if (key == "AppId") {
                appId = (uint16_t)std::stoi(value);
            } else if (key == "MaxPlayers") {
                maxPlayers = (uint8_t)std::stoi(value);
            } else if (key == "BotCount") {
                botCount = (uint8_t)std::stoi(value);
            } else if (key == "VACSecured") {
                vacSecured = (value == "true" || value == "1");
            } else if (key == "Private") {
                privateServer = (value == "true" || value == "1");
            } else if (key == "Version") {
                version = value;
            } else if (key == "EnableLogging") {
                enableLogging = (value == "true" || value == "1");
            } else if (key == "LogFile") {
                logFile = value;
            }
        }
        file.close();
    }

    void Save(const std::string& configPath) {
        std::ofstream file(configPath);
        if (!file.is_open()) return;

        file << "# Windrose Query Configuration\n";
        file << "# Generated automatically - edit as needed\n\n";
        file << "[Query]\n";
        file << "Port=" << port << "\n";
        file << "ServerName=" << serverName << "\n";
        file << "Map=" << map << "\n";
        file << "GameFolder=" << gameFolder << "\n";
        file << "GameDescription=" << gameDescription << "\n";
        file << "AppId=" << appId << "\n";
        file << "MaxPlayers=" << (int)maxPlayers << "\n";
        file << "BotCount=" << (int)botCount << "\n";
        file << "VACSecured=" << (vacSecured ? "true" : "false") << "\n";
        file << "Private=" << (privateServer ? "true" : "false") << "\n";
        file << "Version=" << version << "\n";
        file << "EnableLogging=" << (enableLogging ? "true" : "false") << "\n";
        file << "LogFile=" << logFile << "\n";

        file.close();
    }

    static std::string GetConfigPath() {
        char exePath[MAX_PATH];
        GetModuleFileNameA(NULL, exePath, MAX_PATH);
        std::string path(exePath);
        size_t pos = path.find_last_of("\\/");
        if (pos != std::string::npos) {
            path = path.substr(0, pos);
        }
        return path + "\\windrosequery\\settings.ini";
    }

    static void EnsureConfigDirectory() {
        char exePath[MAX_PATH];
        GetModuleFileNameA(NULL, exePath, MAX_PATH);
        std::string path(exePath);
        size_t pos = path.find_last_of("\\/");
        if (pos != std::string::npos) {
            path = path.substr(0, pos);
        }
        std::string configDir = path + "\\windrosequery";
        CreateDirectoryA(configDir.c_str(), NULL);
    }
};

extern QueryConfig g_Config;

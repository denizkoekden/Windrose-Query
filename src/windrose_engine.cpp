#include "windrose_engine.h"
#include "pattern_finder.h"
#include <Psapi.h>
#include <sstream>
#include <algorithm>
#include <fstream>
#include <Windows.h>

extern void LogMessage(const std::string& message);

#pragma comment(lib, "Psapi.lib")

namespace UnrealEngine {
    StandaloneIntegration* g_Engine = nullptr;

    StandaloneIntegration::StandaloneIntegration()
        : moduleBase(0), moduleSize(0), GObjectsPtr(0), GNamesPtr(0) {
    }

    StandaloneIntegration::~StandaloneIntegration() {
    }

    std::vector<uint8_t> StandaloneIntegration::FindPattern(const char* pattern, const char* mask) {
        std::vector<uint8_t> result;
        size_t patternLength = strlen(mask);

        for (size_t i = 0; i < moduleSize - patternLength; i++) {
            bool found = true;
            for (size_t j = 0; j < patternLength; j++) {
                if (mask[j] != '?' && pattern[j] != *reinterpret_cast<char*>(moduleBase + i + j)) {
                    found = false;
                    break;
                }
            }
            if (found) {
                for (size_t j = 0; j < 8; j++) {
                    result.push_back(*reinterpret_cast<uint8_t*>(moduleBase + i + j));
                }
                return result;
            }
        }
        return result;
    }

    uintptr_t StandaloneIntegration::FindGObjects() {
        return PatternScanner::ScanForGObjects(moduleBase, moduleSize);
    }

    uintptr_t StandaloneIntegration::FindGNames() {
        return 0;
    }

    bool StandaloneIntegration::Initialize() {
        HMODULE hModule = GetModuleHandleA(nullptr);
        if (!hModule) {
            LogMessage("Standalone: Failed to get module handle");
            return false;
        }

        MODULEINFO modInfo;
        if (!GetModuleInformation(GetCurrentProcess(), hModule, &modInfo, sizeof(MODULEINFO))) {
            LogMessage("Standalone: Failed to get module information");
            return false;
        }

        moduleBase = reinterpret_cast<uintptr_t>(modInfo.lpBaseOfDll);
        moduleSize = modInfo.SizeOfImage;

        char hexBuffer[32];
        sprintf_s(hexBuffer, "0x%llX", moduleBase);
        LogMessage(std::string("Standalone: Module base: ") + hexBuffer);
        LogMessage("Standalone: Module size: " + std::to_string(moduleSize));

        GObjectsPtr = FindGObjects();

        if (GObjectsPtr) {
            sprintf_s(hexBuffer, "0x%llX", GObjectsPtr);
            LogMessage(std::string("GObjects found at: ") + hexBuffer);
        } else {
            LogMessage("WARNING: GObjects not found - player queries will return empty");
        }

        LogMessage("Standalone: Integration initialized");
        return true;
    }

    std::vector<PlayerInfo> StandaloneIntegration::GetAllPlayers() {
        std::vector<PlayerInfo> players;

        if (!GObjectsPtr) {
            return players;
        }

        auto playerStates = PatternScanner::FindAllPlayerStates(GObjectsPtr);

        for (uintptr_t obj : playerStates) {
            PlayerInfo info;
            info.playerStatePtr = obj;

            // PlayerNamePrivate offset in APlayerState
            // Reference: SDK/Engine_classes.hpp - APlayerState::PlayerNamePrivate at 0x0340
            FString* namePtr = (FString*)(obj + 0x0340);
            if (namePtr && namePtr->Length > 0) {
                info.playerName = namePtr->ToString();
            }

            // AccountId offset in AR5DataKeeper_PlayerState
            // Reference: SDK/R5DataKeepers_classes.hpp - AR5DataKeeper_PlayerState::AccountData at 0x0378
            // Reference: SDK/R5DataKeepers_structs.hpp - FR5DataKeeper_AccountData::AccountId at +0x0010
            // Total offset: 0x0378 + 0x0010 = 0x0388
            FString* idPtr = (FString*)(obj + 0x0388);
            if (idPtr && idPtr->Length > 0) {
                std::wstring idW = idPtr->ToString();
                info.accountId = std::string(idW.begin(), idW.end());
            }

            // Score offset in APlayerState
            // Reference: SDK/Engine_classes.hpp - APlayerState::Score at 0x02B8 (float)
            float* scorePtr = (float*)(obj + 0x02B8);
            if (!IsBadReadPtr(scorePtr, sizeof(float))) {
                info.score = (int32_t)*scorePtr;
            }

            // StartTime offset in APlayerState (seconds since server start)
            // Reference: SDK/Engine_classes.hpp - APlayerState::StartTime at 0x02C8 (int32)
            int32_t* startTimePtr = (int32_t*)(obj + 0x02C8);
            if (!IsBadReadPtr(startTimePtr, sizeof(int32_t))) {
                info.connectedSeconds = (float)*startTimePtr;
            }

            players.push_back(info);
        }

        return players;
    }

    static std::string ReadServerInfoField(const std::string& key) {
        char exePath[MAX_PATH];
        GetModuleFileNameA(NULL, exePath, MAX_PATH);
        std::string exeDir(exePath);
        size_t lastSlash = exeDir.find_last_of("\\/");
        exeDir = exeDir.substr(0, lastSlash);

        std::string jsonPath = exeDir + "\\..\\..\\ServerDescription.json";

        std::ifstream file(jsonPath);
        if (!file.is_open()) {
            return "";
        }

        std::string line;
        std::string searchKey = "\"" + key + "\":";
        while (std::getline(file, line)) {
            size_t pos = line.find(searchKey);
            if (pos != std::string::npos) {
                size_t valueStart = pos + searchKey.length();
                while (valueStart < line.length() && (line[valueStart] == ' ' || line[valueStart] == '\t')) {
                    valueStart++;
                }

                if (valueStart >= line.length()) break;

                if (line[valueStart] == '"') {
                    size_t start = valueStart;
                    size_t end = line.find("\"", start + 1);
                    if (end != std::string::npos) {
                        return line.substr(start + 1, end - start - 1);
                    }
                } else {
                    size_t end = line.find_first_of(",\n\r", valueStart);
                    if (end != std::string::npos) {
                        std::string value = line.substr(valueStart, end - valueStart);
                        while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
                            value.pop_back();
                        }
                        return value;
                    }
                }
            }
        }
        return "";
    }

    ServerMetadata StandaloneIntegration::GetServerMetadata() {
        ServerMetadata meta;
        meta.serverName = ReadServerInfoField("ServerName");
        meta.inviteCode = ReadServerInfoField("InviteCode");
        meta.deploymentId = ReadServerInfoField("DeploymentId");
        meta.maxPlayers = ReadServerInfoField("MaxPlayerCount");
        meta.serverAddress = ReadServerInfoField("DirectConnectionServerAddress");
        meta.serverPort = ReadServerInfoField("DirectConnectionServerPort");
        return meta;
    }
}

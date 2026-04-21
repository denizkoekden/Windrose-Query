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
    EngineSnapshot* g_Snapshot = nullptr;

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

    // Reads the raw contents of <server>\R5\ServerDescription.json.
    // Returns empty string if the file is missing.
    static std::string ReadServerDescriptionRaw() {
        char exePath[MAX_PATH];
        GetModuleFileNameA(NULL, exePath, MAX_PATH);
        std::string exeDir(exePath);
        size_t lastSlash = exeDir.find_last_of("\\/");
        if (lastSlash != std::string::npos) exeDir = exeDir.substr(0, lastSlash);

        // Binaries\Win64 -> ..\..\ = R5\
        std::string jsonPath = exeDir + "\\..\\..\\ServerDescription.json";

        std::ifstream file(jsonPath);
        if (!file.is_open()) return "";

        std::stringstream ss;
        ss << file.rdbuf();
        return ss.str();
    }

    // Finds "key":<value> anywhere in the (single) JSON text. This deliberately
    // ignores nesting - Windrose's server description mirrors the relevant
    // fields either at top level (DeploymentId) or under
    // ServerDescription_Persistent (ServerName, InviteCode, MaxPlayerCount,
    // IsPasswordProtected), but no field name occurs in both scopes with
    // different meanings, so a first-hit search is safe and avoids a full JSON
    // parser dependency.
    //
    // valueStart returns the offset just after "key":; the caller interprets
    // the value as string / number / bool.
    static bool FindKeyValueStart(const std::string& json, const std::string& key, size_t& valueStart) {
        std::string needle = "\"" + key + "\"";
        size_t pos = 0;
        while ((pos = json.find(needle, pos)) != std::string::npos) {
            size_t after = pos + needle.size();
            // Allow whitespace between the key and the colon.
            while (after < json.size() && (json[after] == ' ' || json[after] == '\t' ||
                                           json[after] == '\r' || json[after] == '\n')) {
                after++;
            }
            if (after < json.size() && json[after] == ':') {
                after++;
                while (after < json.size() && (json[after] == ' ' || json[after] == '\t' ||
                                               json[after] == '\r' || json[after] == '\n')) {
                    after++;
                }
                valueStart = after;
                return true;
            }
            pos = after;
        }
        return false;
    }

    static std::string ReadJsonString(const std::string& json, const std::string& key) {
        size_t pos;
        if (!FindKeyValueStart(json, key, pos)) return "";
        if (pos >= json.size() || json[pos] != '"') return "";
        size_t start = pos + 1;
        size_t end = json.find('"', start);
        if (end == std::string::npos) return "";
        return json.substr(start, end - start);
    }

    // Returns tri-state: 1 = true, 0 = false, -1 = key not found / not a bool.
    static int ReadJsonBool(const std::string& json, const std::string& key) {
        size_t pos;
        if (!FindKeyValueStart(json, key, pos)) return -1;
        if (json.compare(pos, 4, "true") == 0)  return 1;
        if (json.compare(pos, 5, "false") == 0) return 0;
        return -1;
    }

    // Keeps only the leading [0-9.] characters so that DeploymentId like
    // "1.2.3-456-win64" becomes "1.2.3".
    static std::string TrimToVersion(const std::string& s) {
        std::string out;
        for (char c : s) {
            if ((c >= '0' && c <= '9') || c == '.') out.push_back(c);
            else break;
        }
        return out;
    }

    ServerMetadata StandaloneIntegration::GetServerMetadata() {
        ServerMetadata meta;

        std::string raw = ReadServerDescriptionRaw();
        if (raw.empty()) return meta;

        // Under ServerDescription_Persistent
        meta.serverName    = ReadJsonString(raw, "ServerName");
        meta.inviteCode    = ReadJsonString(raw, "InviteCode");
        meta.maxPlayers    = ReadJsonString(raw, "MaxPlayerCount");
        if (meta.maxPlayers.empty()) {
            // MaxPlayerCount is a number in JSON, not a string. Try number form.
            size_t pos;
            if (FindKeyValueStart(raw, "MaxPlayerCount", pos)) {
                size_t end = raw.find_first_of(",}\n\r", pos);
                if (end != std::string::npos) {
                    std::string v = raw.substr(pos, end - pos);
                    while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
                    meta.maxPlayers = v;
                }
            }
        }

        // Top-level
        meta.deploymentId  = TrimToVersion(ReadJsonString(raw, "DeploymentId"));

        // Optional (present on some builds)
        meta.serverAddress = ReadJsonString(raw, "DirectConnectionServerAddress");
        meta.serverPort    = ReadJsonString(raw, "DirectConnectionServerPort");

        int pw = ReadJsonBool(raw, "IsPasswordProtected");
        if (pw >= 0) {
            meta.passwordProtected = (pw == 1);
            meta.passwordKnown = true;
        }

        return meta;
    }

    // --- EngineSnapshot -----------------------------------------------------

    EngineSnapshot::EngineSnapshot(StandaloneIntegration* engine,
                                   int activeIntervalMs,
                                   int idleIntervalMs)
        : m_engine(engine),
          m_activeIntervalMs(activeIntervalMs),
          m_idleIntervalMs(idleIntervalMs) {}

    EngineSnapshot::~EngineSnapshot() {
        Stop();
    }

    void EngineSnapshot::RefreshOnce() {
        if (!m_engine || !m_engine->IsInitialized()) return;

        PlayerSnapshot fresh;
        fresh.players = m_engine->GetAllPlayers();
        fresh.meta = m_engine->GetServerMetadata();
        fresh.refreshedAt = std::chrono::steady_clock::now();
        fresh.populated = true;

        std::lock_guard<std::mutex> lock(m_mutex);
        m_snapshot = std::move(fresh);
    }

    void EngineSnapshot::RefreshLoop() {
        while (m_running.load(std::memory_order_acquire)) {
            // Pick the next interval based on the *current* snapshot: empty
            // servers refresh much more rarely, matching WindrosePlus' idle
            // affinity behavior.
            int interval;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                interval = m_snapshot.players.empty() ? m_idleIntervalMs
                                                      : m_activeIntervalMs;
            }

            auto nextTick = std::chrono::steady_clock::now() +
                            std::chrono::milliseconds(interval);
            RefreshOnce();

            // Sleep in short slices so Stop() can break out quickly.
            while (m_running.load(std::memory_order_acquire) &&
                   std::chrono::steady_clock::now() < nextTick) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    }

    void EngineSnapshot::Start() {
        if (m_running.exchange(true)) return;
        // Prime the snapshot synchronously so the first query has real data.
        RefreshOnce();
        m_thread = std::thread(&EngineSnapshot::RefreshLoop, this);
    }

    void EngineSnapshot::Stop() {
        if (!m_running.exchange(false)) return;
        if (m_thread.joinable()) m_thread.join();
    }

    PlayerSnapshot EngineSnapshot::Get() {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_snapshot;
    }
}

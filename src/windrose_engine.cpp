#include "windrose_engine.h"
#include "pattern_finder.h"
#include <Psapi.h>
#include <sstream>
#include <algorithm>
#include <fstream>
#include <Windows.h>

extern void LogMessage(const std::string& message);

#pragma comment(lib, "Psapi.lib")

// Layout offsets verified in Ghidra against a live Windrose server build.
//
// AGameStateBase
namespace {
    constexpr size_t GS_AuthorityGameMode = 0x02B0;  // AGameModeBase*
    constexpr size_t GS_PlayerArray       = 0x02C0;  // TArray<APlayerState*>

    // AGameModeBase
    constexpr size_t GM_GameSession       = 0x0300;  // AGameSession*

    // AGameSession
    constexpr size_t GSession_MaxPlayers  = 0x02AC;  // int32

    // APlayerState
    constexpr size_t PS_Score             = 0x02A8;  // float
    constexpr size_t PS_PlayerId          = 0x02AC;  // int32
    constexpr size_t PS_CompressedPing    = 0x02B0;  // uint8 (ping in 4ms units)
    constexpr size_t PS_PlayerFlags       = 0x02B2;  // uint8 bitfield
    constexpr size_t PS_StartTime         = 0x02B4;  // float (seconds since server start)
    constexpr size_t PS_PawnPrivate       = 0x0320;  // APawn*
    constexpr size_t PS_PlayerNamePrivate = 0x0340;  // FString

    // AR5DataKeeper_PlayerState::AccountData at 0x0378, AccountId at +0x0010.
    constexpr size_t PS_AccountId         = 0x0388;  // FString

    // APlayerState flags bitfield (at PS_PlayerFlags)
    constexpr uint8_t PSF_IsInactive      = 1u << 5;
    constexpr uint8_t PSF_OnlySpectator   = 1u << 2;
}

namespace UnrealEngine {
    StandaloneIntegration* g_Engine = nullptr;
    EngineSnapshot* g_Snapshot = nullptr;

    StandaloneIntegration::StandaloneIntegration()
        : moduleBase(0), moduleSize(0), GObjectsPtr(0), GameStatePtr(0) {
    }

    StandaloneIntegration::~StandaloneIntegration() {
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

        GObjectsPtr = PatternScanner::ScanForGObjects(moduleBase, moduleSize);
        if (GObjectsPtr) {
            sprintf_s(hexBuffer, "0x%llX", GObjectsPtr);
            LogMessage(std::string("GObjects found at: ") + hexBuffer);
        } else {
            LogMessage("WARNING: GObjects not found - queries will return empty player lists");
        }

        // GameState is resolved lazily on the first refresh; at DLL load time
        // the world has not been constructed yet.
        LogMessage("Standalone: Integration initialized");
        return true;
    }

    bool StandaloneIntegration::IsGameStateValid(uintptr_t gameState) const {
        if (!gameState) return false;
        if (IsBadReadPtr((void*)gameState, GS_PlayerArray + sizeof(TArrayLayout))) return false;

        uintptr_t gameMode = *(uintptr_t*)(gameState + GS_AuthorityGameMode);
        if (!gameMode || IsBadReadPtr((void*)gameMode, 8)) return false;

        TArrayLayout* arr = (TArrayLayout*)(gameState + GS_PlayerArray);
        if (arr->Num < 0 || arr->Num > 256)         return false;
        if (arr->Max < arr->Num || arr->Max > 1024) return false;
        if (arr->Num > 0 && (!arr->Data || IsBadReadPtr(arr->Data, arr->Num * sizeof(void*)))) return false;

        return true;
    }

    uintptr_t StandaloneIntegration::ResolveGameState() {
        if (GameStatePtr && IsGameStateValid(GameStatePtr)) return GameStatePtr;

        GameStatePtr = PatternScanner::ScanForGameState(GObjectsPtr);
        return GameStatePtr;
    }

    std::vector<PlayerInfo> StandaloneIntegration::GetAllPlayers() {
        std::vector<PlayerInfo> players;

        uintptr_t gameState = ResolveGameState();
        if (!gameState) return players;

        TArrayLayout* arr = (TArrayLayout*)(gameState + GS_PlayerArray);
        int32_t num = arr->Num;
        if (num <= 0 || !arr->Data) {
            m_firstSeen.clear();
            return players;
        }

        uintptr_t* entries = (uintptr_t*)arr->Data;
        auto now = std::chrono::steady_clock::now();

        std::map<uintptr_t, std::chrono::steady_clock::time_point> seenThisPass;

        for (int i = 0; i < num && i < 256; i++) {
            uintptr_t ps = entries[i];
            if (!ps) continue;
            if (IsBadReadPtr((void*)ps, PS_AccountId + sizeof(FString))) continue;

            // Drop inactive / spectator-only entries.
            uint8_t flags = *(uint8_t*)(ps + PS_PlayerFlags);
            if (flags & (PSF_IsInactive | PSF_OnlySpectator)) continue;

            // Zombie filter: a stale PlayerState lingers after a client drop
            // until GC runs. Require either a valid pawn or a live ping, both
            // of which are cleared on disconnect before the PS is removed.
            uintptr_t pawn = *(uintptr_t*)(ps + PS_PawnPrivate);
            uint8_t compressedPing = *(uint8_t*)(ps + PS_CompressedPing);
            if (!pawn && compressedPing == 0) continue;

            PlayerInfo info;
            info.playerStatePtr = ps;

            FString* namePtr = (FString*)(ps + PS_PlayerNamePrivate);
            if (namePtr->Length > 0 && namePtr->Length <= 64) {
                const wchar_t* data = namePtr->GetData();
                if (namePtr->IsInline() ||
                    !IsBadReadPtr((void*)data, namePtr->Length * sizeof(wchar_t))) {
                    info.playerName = std::wstring(data, namePtr->Length);
                }
            }

            FString* idPtr = (FString*)(ps + PS_AccountId);
            if (idPtr->Length > 0 && idPtr->Length <= 64) {
                const wchar_t* data = idPtr->GetData();
                if (idPtr->IsInline() ||
                    !IsBadReadPtr((void*)data, idPtr->Length * sizeof(wchar_t))) {
                    std::wstring w(data, idPtr->Length);
                    info.accountId = std::string(w.begin(), w.end());
                }
            }

            float score = *(float*)(ps + PS_Score);
            info.score = (int32_t)score;

            // A2S duration = seconds since we first saw this PlayerState. Real
            // "seconds since connect" would require AGameState::
            // ReplicatedWorldTimeSeconds which we don't have a verified offset
            // for; first-seen is accurate to within the refresh interval and
            // survives across multiple queries as long as the PS stays in
            // PlayerArray.
            auto it = m_firstSeen.find(ps);
            std::chrono::steady_clock::time_point firstSeen;
            if (it != m_firstSeen.end()) {
                firstSeen = it->second;
            } else {
                firstSeen = now;
            }
            seenThisPass[ps] = firstSeen;

            auto secs = std::chrono::duration<float>(now - firstSeen).count();
            info.connectedSeconds = secs;

            players.push_back(info);
        }

        // Prune PlayerStates that no longer appear in PlayerArray.
        m_firstSeen = std::move(seenThisPass);

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
        if (!raw.empty()) {
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
        }

        // If the live GameMode->GameSession chain is reachable, prefer the
        // authoritative in-memory MaxPlayers. This stays correct across runtime
        // changes that the JSON dump hasn't caught up to yet.
        uintptr_t gameState = ResolveGameState();
        if (gameState) {
            uintptr_t gameMode = *(uintptr_t*)(gameState + GS_AuthorityGameMode);
            if (gameMode && !IsBadReadPtr((void*)gameMode, GM_GameSession + sizeof(void*))) {
                uintptr_t session = *(uintptr_t*)(gameMode + GM_GameSession);
                if (session && !IsBadReadPtr((void*)session, GSession_MaxPlayers + sizeof(int32_t))) {
                    int32_t mp = *(int32_t*)(session + GSession_MaxPlayers);
                    if (mp > 0 && mp <= 1024) {
                        meta.maxPlayers = std::to_string(mp);
                    }
                }
            }
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

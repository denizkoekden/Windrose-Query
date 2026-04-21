#include "pattern_finder.h"
#include <string>

extern void LogMessage(const std::string& message);

struct FUObjectItem {
    void*   Object;
    int32_t Flags;
    int32_t ClusterRootIndex;
    int32_t SerialNumber;
};

struct FUObjectArray {
    FUObjectItem** Objects;
    uint8_t Pad_8[0x8];
    int32_t MaxElements;
    int32_t NumElements;
    int32_t MaxChunks;
    int32_t NumChunks;

    static constexpr int32_t ElementsPerChunk = 0x10000;

    void* GetByIndex(int32_t Index) const {
        int32_t ChunkIndex = Index / ElementsPerChunk;
        int32_t InChunkIdx = Index % ElementsPerChunk;

        if (Index < 0 || ChunkIndex >= NumChunks || Index >= NumElements)
            return nullptr;

        FUObjectItem* ChunkPtr = Objects[ChunkIndex];
        if (!ChunkPtr) return nullptr;

        return ChunkPtr[InChunkIdx].Object;
    }

    FUObjectItem* GetItemByIndex(int32_t Index) const {
        int32_t ChunkIndex = Index / ElementsPerChunk;
        int32_t InChunkIdx = Index % ElementsPerChunk;

        if (Index < 0 || ChunkIndex >= NumChunks || Index >= NumElements)
            return nullptr;

        FUObjectItem* ChunkPtr = Objects[ChunkIndex];
        if (!ChunkPtr) return nullptr;

        return &ChunkPtr[InChunkIdx];
    }
};

// UObject::ObjectFlags bitmask. Only the one we need here.
// Reference: SDK/Basic.hpp - EObjectFlags::RF_ClassDefaultObject
constexpr int32_t RF_ClassDefaultObject = 0x00000010;

uintptr_t PatternScanner::FindPattern(uintptr_t start, size_t size, const char* pattern, const char* mask) {
    size_t patternLen = strlen(mask);

    for (size_t i = 0; i < size - patternLen; i++) {
        bool found = true;
        for (size_t j = 0; j < patternLen; j++) {
            if (mask[j] != '?' && pattern[j] != *(char*)(start + i + j)) {
                found = false;
                break;
            }
        }
        if (found) {
            return start + i;
        }
    }
    return 0;
}

uintptr_t PatternScanner::ScanForGObjects(uintptr_t moduleBase, size_t moduleSize) {
    LogMessage("Using Dumper7 GObjects offset...");

    constexpr int32_t GObjectsOffset = 0x0FA1D650;
    uintptr_t gobjectsAddr = moduleBase + GObjectsOffset;

    char msg[128];
    sprintf_s(msg, "GObjects address: 0x%llX", gobjectsAddr);
    LogMessage(msg);

    return gobjectsAddr;
}

// Layout constants used only by the heuristic. Offsets verified in Ghidra on
// a live Windrose build.
namespace {
    // AGameStateBase
    constexpr size_t GS_AuthorityGameMode = 0x02B0;
    constexpr size_t GS_PlayerArray       = 0x02C0;

    // AGameModeBase
    constexpr size_t GM_GameSession       = 0x0300;

    // AGameSession
    constexpr size_t GSession_MaxPlayers  = 0x02AC;

    // UObject header
    constexpr size_t UObject_ObjectFlags  = 0x0008;
}

uintptr_t PatternScanner::ScanForGameState(uintptr_t gobjectsAddr) {
    if (!gobjectsAddr) return 0;

    FUObjectArray* GObjects = (FUObjectArray*)gobjectsAddr;
    if (IsBadReadPtr(GObjects, sizeof(FUObjectArray))) {
        LogMessage("ScanForGameState: GObjects pointer not readable");
        return 0;
    }

    if (GObjects->NumElements <= 0 || GObjects->NumElements > 2000000) {
        return 0;
    }

    int scanned = 0;
    int candidates = 0;

    for (int i = 0; i < GObjects->NumElements; i++) {
        void* objPtr = GObjects->GetByIndex(i);
        if (!objPtr) continue;

        uintptr_t obj = (uintptr_t)objPtr;
        // Reading up to the PlayerArray tail (0x02C0 + 16).
        if (IsBadReadPtr((void*)obj, GS_PlayerArray + sizeof(TArrayLayout))) continue;

        scanned++;

        // Skip CDOs - those have a valid layout but no live data.
        int32_t flags = *(int32_t*)(obj + UObject_ObjectFlags);
        if (flags & RF_ClassDefaultObject) continue;

        // AuthorityGameMode is only set on the server copy of GameState.
        uintptr_t gameMode = *(uintptr_t*)(obj + GS_AuthorityGameMode);
        if (!gameMode) continue;
        if (IsBadReadPtr((void*)gameMode, GM_GameSession + sizeof(void*))) continue;

        // PlayerArray sanity check.
        TArrayLayout* arr = (TArrayLayout*)(obj + GS_PlayerArray);
        if (arr->Num < 0 || arr->Num > 256)           continue;
        if (arr->Max < arr->Num || arr->Max > 1024)   continue;
        if (arr->Num > 0 && (!arr->Data || IsBadReadPtr(arr->Data, arr->Num * sizeof(void*)))) continue;

        // Follow GameMode->GameSession->MaxPlayers as a final sanity gate. A
        // random unrelated UObject is extremely unlikely to chain cleanly
        // through three pointers landing on a plausible MaxPlayers value.
        uintptr_t gameSession = *(uintptr_t*)(gameMode + GM_GameSession);
        if (!gameSession) continue;
        if (IsBadReadPtr((void*)gameSession, GSession_MaxPlayers + sizeof(int32_t))) continue;
        int32_t maxPlayers = *(int32_t*)(gameSession + GSession_MaxPlayers);
        if (maxPlayers < 0 || maxPlayers > 1024) continue;

        candidates++;

        char msg[256];
        sprintf_s(msg,
            "GameState located at 0x%llX (idx %d, players=%d/%d, gameMode=0x%llX, session=0x%llX)",
            obj, i, arr->Num, maxPlayers, gameMode, gameSession);
        LogMessage(msg);
        return obj;
    }

    char msg[160];
    sprintf_s(msg, "ScanForGameState: no match (scanned %d of %d objects, %d candidates)",
              scanned, GObjects->NumElements, candidates);
    LogMessage(msg);
    return 0;
}

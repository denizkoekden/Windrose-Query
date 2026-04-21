#include "pattern_finder.h"
#include <string>

extern void LogMessage(const std::string& message);

struct FString {
    union {
        wchar_t* Data;
        wchar_t InlineData[12];
    };
    int32_t Length;
    int32_t Max;

    bool IsInline() const { return Max == 0; }

    const wchar_t* GetData() const {
        return IsInline() ? InlineData : Data;
    }
};

struct FUObjectItem {
    void* Object;
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
};

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

std::vector<uintptr_t> PatternScanner::FindAllPlayerStates(uintptr_t gobjectsAddr) {
    std::vector<uintptr_t> playerStates;

    if (!gobjectsAddr) return playerStates;

    FUObjectArray* GObjects = (FUObjectArray*)gobjectsAddr;

    if (!GObjects || IsBadReadPtr(GObjects, 32)) {
        LogMessage("Invalid GObjects pointer");
        return playerStates;
    }

    char debugMsg[256];
    sprintf_s(debugMsg, "GObjects: NumElements=%d, MaxElements=%d, NumChunks=%d",
        GObjects->NumElements, GObjects->MaxElements, GObjects->NumChunks);
    LogMessage(debugMsg);

    if (GObjects->NumElements <= 0 || GObjects->NumElements > 2000000) {
        LogMessage("Invalid NumElements");
        return playerStates;
    }

    int scannedCount = 0;
    int validCount = 0;

    for (int i = 0; i < GObjects->NumElements; i++) {
        scannedCount++;

        void* objPtr = GObjects->GetByIndex(i);
        if (!objPtr) continue;

        uintptr_t obj = (uintptr_t)objPtr;
        if (IsBadReadPtr((void*)obj, 0x400)) continue;

        validCount++;

        // Player validation offsets from SDK
        // Reference: SDK/Engine_classes.hpp - APlayerState::PlayerNamePrivate at 0x0340
        FString* playerName = (FString*)(obj + 0x0340);

        // Reference: SDK/R5DataKeepers_classes.hpp + R5DataKeepers_structs.hpp
        // AccountData at 0x0378, AccountId at +0x0010 = 0x0388
        FString* accountId = (FString*)(obj + 0x0388);

        if (IsBadReadPtr(playerName, sizeof(FString))) continue;
        if (playerName->Length < 2 || playerName->Length > 50) continue;

        if (IsBadReadPtr(accountId, sizeof(FString))) continue;
        if (accountId->Length < 32 || accountId->Length > 33) continue;

        const wchar_t* nameData = playerName->GetData();
        const wchar_t* idData = accountId->GetData();

        bool nameValid = playerName->IsInline() || !IsBadReadPtr(nameData, playerName->Length * sizeof(wchar_t));
        bool idValid = accountId->IsInline() || !IsBadReadPtr(idData, accountId->Length * sizeof(wchar_t));

        if (!nameValid || !idValid) continue;

        wchar_t nameBuffer[51] = {0};
        wcsncpy_s(nameBuffer, 51, nameData, min(playerName->Length, 50));

        wchar_t idBuffer[101] = {0};
        wcsncpy_s(idBuffer, 101, idData, min(accountId->Length, 100));

        if (accountId->Length == 32 || accountId->Length == 33) {
            bool isHex = true;
            for (int j = 0; j < 32; j++) {
                wchar_t c = idBuffer[j];
                if (!((c >= L'0' && c <= L'9') || (c >= L'A' && c <= L'F') || (c >= L'a' && c <= L'f'))) {
                    isHex = false;
                    break;
                }
            }
            if (!isHex) continue;
        } else {
            continue;
        }

        bool hasValidChars = false;
        for (int j = 0; j < playerName->Length && j < 50; j++) {
            if (nameBuffer[j] >= 32 && nameBuffer[j] <= 126) {
                hasValidChars = true;
                break;
            }
        }

        if (!hasValidChars) continue;

        // Reference: SDK/Engine_classes.hpp - APlayerState flags at 0x02B2
        // Used to filter inactive/spectator players
        uint8_t* playerFlagsPtr = (uint8_t*)(obj + 0x02B2);
        if (IsBadReadPtr(playerFlagsPtr, 1)) continue;

        uint8_t playerFlags = *playerFlagsPtr;
        bool bIsInactive = (playerFlags & (1 << 5)) != 0;
        bool bOnlySpectator = (playerFlags & (1 << 2)) != 0;

        if (bIsInactive || bOnlySpectator) continue;

        playerStates.push_back(obj);
    }

    char summary[512];
    sprintf_s(summary, "Scanned %d/%d objects, %d valid, found %zu player states",
        scannedCount, GObjects->NumElements, validCount, playerStates.size());
    LogMessage(summary);

    return playerStates;
}

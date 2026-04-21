#pragma once
#include <windows.h>
#include <vector>
#include <cstdint>

// TArray<T> layout used by PlayerArray on AGameStateBase.
// Reference: SDK/UnrealContainers.hpp
struct TArrayLayout {
    void*   Data;   // 0x00
    int32_t Num;    // 0x08
    int32_t Max;    // 0x0C
};

class PatternScanner {
public:
    static uintptr_t FindPattern(uintptr_t start, size_t size, const char* pattern, const char* mask);

    // Returns the absolute address of the FUObjectArray singleton inside the
    // host module. Currently uses a hardcoded Dumper7 offset.
    static uintptr_t ScanForGObjects(uintptr_t moduleBase, size_t moduleSize);

    // Walks GObjects and returns the live AGameStateBase* for the current
    // world. Uses a heuristic: non-CDO + valid AuthorityGameMode pointer at
    // 0x02B0 + a plausible TArray layout at PlayerArray (0x02C0).
    // Returns 0 if no match is found (e.g. before the world has spun up).
    static uintptr_t ScanForGameState(uintptr_t gobjectsAddr);
};

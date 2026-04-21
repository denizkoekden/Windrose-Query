#pragma once
#include <windows.h>
#include <vector>
#include <cstdint>

class PatternScanner {
public:
    static uintptr_t FindPattern(uintptr_t start, size_t size, const char* pattern, const char* mask);
    static uintptr_t ScanForGObjects(uintptr_t moduleBase, size_t moduleSize);
    static std::vector<uintptr_t> FindAllPlayerStates(uintptr_t gobjectsAddr);
};

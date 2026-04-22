#pragma once
#include <windows.h>
#include <cstdint>

class PatternScanner {
public:
    static uintptr_t FindPattern(uintptr_t start, size_t size, const char* pattern, const char* mask);
};

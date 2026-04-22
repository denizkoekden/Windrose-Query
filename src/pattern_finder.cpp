#include "pattern_finder.h"

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
        if (found) return start + i;
    }
    return 0;
}

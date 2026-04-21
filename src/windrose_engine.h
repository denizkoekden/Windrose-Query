#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <map>

// FString with Small String Optimization (SSO)
// Reference: SDK/UnrealContainers.hpp
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

    std::wstring ToString() const {
        if (Length <= 0) return L"";
        return std::wstring(GetData(), Length);
    }

    FString(const wchar_t* str) {
        Length = (int32_t)wcslen(str);
        if (Length < 12) {
            Max = 0;
            wcscpy_s(InlineData, 12, str);
        } else {
            Max = Length + 1;
            Data = new wchar_t[Max];
            wcscpy_s(Data, Max, str);
        }
    }

    FString() : Data(nullptr), Length(0), Max(0) {}
};

// UObject structure
// Reference: SDK/CoreUObject_classes.hpp - Class CoreUObject.Object (0x0028 size)
struct UObject {
    void** VTable;                  // 0x0000
    int32_t ObjectFlags;            // 0x0008 (EObjectFlags)
    int32_t InternalIndex;          // 0x000C (Index)
    void* ClassPrivate;             // 0x0010 (UClass*)
    void* NamePrivate;              // 0x0018 (FName)
    void* OuterPrivate;             // 0x0020 (UObject*)
};

struct PlayerInfo {
    std::string accountId;
    std::wstring playerName;
    uintptr_t playerStatePtr;
    int32_t score = 0;
    float connectedSeconds = 0.0f;
};

struct ServerMetadata {
    std::string serverName;
    std::string inviteCode;
    std::string deploymentId;
    std::string maxPlayers;
    std::string serverAddress;
    std::string serverPort;
};

namespace UnrealEngine {
    class StandaloneIntegration {
    private:
        uintptr_t moduleBase;
        size_t moduleSize;

        uintptr_t GObjectsPtr;
        uintptr_t GNamesPtr;

        std::vector<uint8_t> FindPattern(const char* pattern, const char* mask);
        uintptr_t FindGObjects();
        uintptr_t FindGNames();

    public:
        StandaloneIntegration();
        ~StandaloneIntegration();

        bool Initialize();
        std::vector<PlayerInfo> GetAllPlayers();
        ServerMetadata GetServerMetadata();
        bool IsInitialized() const { return moduleBase != 0; }
    };

    extern StandaloneIntegration* g_Engine;
}

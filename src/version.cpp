#include <winsock2.h>
#include <windows.h>
#include <shellapi.h>
#include <fstream>
#include <string>
#include <ctime>
#include <chrono>
#include <thread>
#include "a2s_server.h"
#include "windrose_engine.h"
#include "config.h"

#pragma comment(lib, "Shell32.lib")

HMODULE hOriginalDll = nullptr;
A2SServer* g_QueryServer = nullptr;
QueryConfig g_Config;
std::thread* g_InitThread = nullptr;

static std::string GetLogPath() {
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    std::string dir(exePath);
    size_t slash = dir.find_last_of("\\/");
    if (slash != std::string::npos) dir = dir.substr(0, slash);
    return dir + "\\" + g_Config.logFile;
}

static void EnsureLogDirectory() {
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    std::string dir(exePath);
    size_t slash = dir.find_last_of("\\/");
    if (slash != std::string::npos) dir = dir.substr(0, slash);
    CreateDirectoryA((dir + "\\windrosequery").c_str(), NULL);
}

void LogMessage(const std::string& message) {
    std::ofstream logFile(GetLogPath(), std::ios::app);
    if (logFile.is_open()) {
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        char timestamp[100];
        std::strftime(timestamp, sizeof(timestamp), "%a %b %d %H:%M:%S %Y", std::localtime(&time));
        logFile << "[" << timestamp << "] " << message << std::endl;
        logFile.flush();
        logFile.close();
    }
}

// Extracts the value for a -Key=Value style Unreal switch from the process
// command line. Case-insensitive match on the key. Returns empty string if
// the switch is not present.
//
// Also recognizes the ?Key=Value URL-form that Unreal accepts on map URLs,
// e.g. MapName?listen?QueryPort=27016.
static std::string GetCmdLineSwitch(const std::wstring& key) {
    LPWSTR cmdLine = GetCommandLineW();
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(cmdLine, &argc);
    if (!argv) return "";

    std::wstring dashPrefix = L"-" + key + L"=";
    std::wstring qPrefix    = L"?" + key + L"=";

    std::wstring valueW;

    auto tryMatch = [&](const std::wstring& token, const std::wstring& prefix) -> bool {
        if (token.size() <= prefix.size()) return false;
        if (_wcsnicmp(token.c_str(), prefix.c_str(), prefix.size()) != 0) return false;
        valueW = token.substr(prefix.size());
        return true;
    };

    for (int i = 0; i < argc && valueW.empty(); ++i) {
        std::wstring token = argv[i];

        if (tryMatch(token, dashPrefix)) break;

        // URL-form embedded in an argument like "MapName?listen?QueryPort=27016"
        size_t pos = 0;
        while ((pos = token.find(L'?', pos)) != std::wstring::npos) {
            std::wstring sub = token.substr(pos);
            // sub starts with '?'; compare against "?Key="
            if (sub.size() > qPrefix.size() &&
                _wcsnicmp(sub.c_str(), qPrefix.c_str(), qPrefix.size()) == 0) {
                std::wstring rest = sub.substr(qPrefix.size());
                size_t end = rest.find(L'?');
                valueW = (end == std::wstring::npos) ? rest : rest.substr(0, end);
                break;
            }
            pos++;
        }
    }

    LocalFree(argv);

    if (valueW.empty()) return "";

    int needed = WideCharToMultiByte(CP_UTF8, 0, valueW.c_str(), (int)valueW.size(),
                                     nullptr, 0, nullptr, nullptr);
    std::string out(needed, '\0');
    WideCharToMultiByte(CP_UTF8, 0, valueW.c_str(), (int)valueW.size(),
                        out.data(), needed, nullptr, nullptr);
    return out;
}

static void ApplyCmdLineOverrides() {
    std::string queryPort = GetCmdLineSwitch(L"QueryPort");
    if (!queryPort.empty()) {
        try {
            int p = std::stoi(queryPort);
            if (p > 0 && p <= 65535) {
                g_Config.port = p;
                LogMessage("CLI: -QueryPort=" + queryPort);
            } else {
                LogMessage("CLI: -QueryPort out of range, ignoring: " + queryPort);
            }
        } catch (...) {
            LogMessage("CLI: -QueryPort not a number, ignoring: " + queryPort);
        }
    }

    std::string multiHome = GetCmdLineSwitch(L"MultiHome");
    if (!multiHome.empty()) {
        g_Config.multiHome = multiHome;
        LogMessage("CLI: -MultiHome=" + multiHome);
    }
}

void AsyncInitialize() {
    // Wait for the host to finish early startup before we attach.
    Sleep(2000);

    EnsureLogDirectory();
    LogMessage("Initializing A2S Query...");

    ApplyCmdLineOverrides();

    UnrealEngine::g_Engine = new UnrealEngine::StandaloneIntegration();
    if (UnrealEngine::g_Engine->Initialize()) {
        LogMessage("Standalone Engine integration active");
    } else {
        LogMessage("Failed to initialize engine integration");
    }

    g_QueryServer = new A2SServer();
    if (g_QueryServer->Start(g_Config.port, g_Config.multiHome)) {
        LogMessage("A2S query server online");
    } else {
        LogMessage("ERROR: Failed to start A2S query server on port " +
                   std::to_string(g_Config.port));
    }
}

void InitializeInjection() {
    g_InitThread = new std::thread(AsyncInitialize);
    g_InitThread->detach();
}

void CleanupInjection() {
    if (g_QueryServer) {
        g_QueryServer->Stop();
        delete g_QueryServer;
        g_QueryServer = nullptr;
    }

    if (UnrealEngine::g_Engine) {
        delete UnrealEngine::g_Engine;
        UnrealEngine::g_Engine = nullptr;
    }

    if (g_InitThread) {
        delete g_InitThread;
        g_InitThread = nullptr;
    }

    LogMessage("DLL Injection detached");
}

// Forward all exports to original version.dll
#pragma comment(linker, "/export:GetFileVersionInfoA=C:\\Windows\\System32\\version.GetFileVersionInfoA")
#pragma comment(linker, "/export:GetFileVersionInfoByHandle=C:\\Windows\\System32\\version.GetFileVersionInfoByHandle")
#pragma comment(linker, "/export:GetFileVersionInfoExA=C:\\Windows\\System32\\version.GetFileVersionInfoExA")
#pragma comment(linker, "/export:GetFileVersionInfoExW=C:\\Windows\\System32\\version.GetFileVersionInfoExW")
#pragma comment(linker, "/export:GetFileVersionInfoSizeA=C:\\Windows\\System32\\version.GetFileVersionInfoSizeA")
#pragma comment(linker, "/export:GetFileVersionInfoSizeExA=C:\\Windows\\System32\\version.GetFileVersionInfoSizeExA")
#pragma comment(linker, "/export:GetFileVersionInfoSizeExW=C:\\Windows\\System32\\version.GetFileVersionInfoSizeExW")
#pragma comment(linker, "/export:GetFileVersionInfoSizeW=C:\\Windows\\System32\\version.GetFileVersionInfoSizeW")
#pragma comment(linker, "/export:GetFileVersionInfoW=C:\\Windows\\System32\\version.GetFileVersionInfoW")
#pragma comment(linker, "/export:VerFindFileA=C:\\Windows\\System32\\version.VerFindFileA")
#pragma comment(linker, "/export:VerFindFileW=C:\\Windows\\System32\\version.VerFindFileW")
#pragma comment(linker, "/export:VerInstallFileA=C:\\Windows\\System32\\version.VerInstallFileA")
#pragma comment(linker, "/export:VerInstallFileW=C:\\Windows\\System32\\version.VerInstallFileW")
#pragma comment(linker, "/export:VerLanguageNameA=C:\\Windows\\System32\\version.VerLanguageNameA")
#pragma comment(linker, "/export:VerLanguageNameW=C:\\Windows\\System32\\version.VerLanguageNameW")
#pragma comment(linker, "/export:VerQueryValueA=C:\\Windows\\System32\\version.VerQueryValueA")
#pragma comment(linker, "/export:VerQueryValueW=C:\\Windows\\System32\\version.VerQueryValueW")

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID /*lpReserved*/) {
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        InitializeInjection();
        break;

    case DLL_PROCESS_DETACH:
        CleanupInjection();
        break;
    }
    return TRUE;
}

#include <winsock2.h>
#include <windows.h>
#include <fstream>
#include <string>
#include <ctime>
#include <chrono>
#include <thread>
#include "a2s_server.h"
#include "windrose_engine.h"
#include "config.h"

HMODULE hOriginalDll = nullptr;
A2SServer* g_QueryServer = nullptr;
QueryConfig g_Config;
std::thread* g_InitThread = nullptr;

void LogMessage(const std::string& message) {
    if (!g_Config.enableLogging) return;
    std::ofstream logFile(g_Config.logFile, std::ios::app);
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

void AsyncInitialize() {
    // Wait for game to finish loading, then initialize the query server.
    // Matches the delay pattern used for UE4SS-hosted servers.
    Sleep(2000);

    LogMessage("Initializing A2S Query...");

    QueryConfig::EnsureConfigDirectory();
    std::string configPath = QueryConfig::GetConfigPath();

    std::ifstream testFile(configPath);
    bool configExists = testFile.good();
    testFile.close();

    if (!configExists) {
        g_Config.Save(configPath);
        LogMessage("Created default config: " + configPath);
    } else {
        g_Config.Load(configPath);
        LogMessage("Loaded config from: " + configPath);
    }

    UnrealEngine::g_Engine = new UnrealEngine::StandaloneIntegration();
    if (UnrealEngine::g_Engine->Initialize()) {
        LogMessage("Standalone Engine integration active");
    } else {
        LogMessage("Failed to initialize engine integration");
    }

    g_QueryServer = new A2SServer();
    if (g_QueryServer->Start(g_Config.port)) {
        char msg[256];
        sprintf_s(msg, "A2S query server started on UDP port %d", g_Config.port);
        LogMessage(msg);
    } else {
        char msg[256];
        sprintf_s(msg, "ERROR: Failed to start A2S query server - check if UDP port %d is already in use", g_Config.port);
        LogMessage(msg);
    }
}

void InitializeInjection() {
    LogMessage("DLL Injection initialized - Starting background thread");
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
        LogMessage("DLL_PROCESS_ATTACH called");
        DisableThreadLibraryCalls(hModule);
        InitializeInjection();
        break;

    case DLL_PROCESS_DETACH:
        LogMessage("DLL_PROCESS_DETACH called");
        CleanupInjection();
        break;
    }
    return TRUE;
}

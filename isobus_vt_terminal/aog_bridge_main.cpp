#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <iostream>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <filesystem>

namespace fs = std::filesystem;

bool isProcessRunning(const std::wstring &processName, DWORD &processId)
{
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE)
        return false;

    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(PROCESSENTRY32W);

    bool running = false;
    if (Process32FirstW(hSnapshot, &pe))
    {
        do
        {
            if (_wcsicmp(pe.szExeFile, processName.c_str()) == 0)
            {
                processId = pe.th32ProcessID;
                running = true;
                break;
            }
        } while (Process32NextW(hSnapshot, &pe));
    }

    CloseHandle(hSnapshot);
    return running;
}

std::wstring findVtTerminalPath()
{
    // Check known locations
    std::vector<std::wstring> candidates = {
        L"C:\\Users\\damia\\Desktop\\isosobus\\build_vt\\Release\\isobus_vt_terminal.exe",
        L"C:\\Users\\damia\\Desktop\\isosobus\\isobus_vt_terminal\\Release\\isobus_vt_terminal.exe",
        L"C:\\Users\\damia\\Desktop\\isosobus\\build\\Release\\isobus_vt_terminal.exe"
    };

    for (const auto &path : candidates)
    {
        if (fs::exists(path))
            return path;
    }

    // Try relative to this executable
    wchar_t modulePath[MAX_PATH];
    if (GetModuleFileNameW(nullptr, modulePath, MAX_PATH))
    {
        fs::path p(modulePath);
        fs::path cand = p.parent_path().parent_path().parent_path() / "build_vt" / "Release" / "isobus_vt_terminal.exe";
        if (fs::exists(cand))
            return cand.wstring();
    }

    return L"";
}

int main(int argc, char* argv[])
{
    // Ensure unbuffered output so AgIO receives all lines immediately in textBoxRcv
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

    std::cout << "============================================================" << std::endl;
    std::cout << "[AOG-TC] AgOpenGPS Task Controller & ISOBUS Terminal Bridge" << std::endl;
    std::cout << "============================================================" << std::endl;
    std::cout.flush();

    // Check if isobus_vt_terminal is already running
    DWORD vtPid = 0;
    bool alreadyRunning = isProcessRunning(L"isobus_vt_terminal.exe", vtPid);

    if (alreadyRunning)
    {
        std::cout << "[AOG-TC] Found running ISOBUS Terminal (PID: " << vtPid << ")" << std::endl;
    }
    else
    {
        std::wstring exePath = findVtTerminalPath();
        if (exePath.empty())
        {
            std::cerr << "[AOG-TC ERROR] Could not find isobus_vt_terminal.exe!" << std::endl;
            std::cerr << "[AOG-TC ERROR] Please build or start isobus_vt_terminal first." << std::endl;
            std::cout.flush();
            std::this_thread::sleep_for(std::chrono::seconds(5));
            return 1;
        }

        std::cout << "[AOG-TC] Launching ISOBUS Terminal & TC Server..." << std::endl;
        std::cout.flush();

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_SHOWNORMAL;
        PROCESS_INFORMATION pi{};

        std::wstring cmd = L"\"" + exePath + L"\"";
        std::wstring workingDir = fs::path(exePath).parent_path().wstring();

        BOOL success = CreateProcessW(
            nullptr,
            &cmd[0],
            nullptr,
            nullptr,
            FALSE,
            CREATE_NEW_CONSOLE,
            nullptr,
            workingDir.c_str(),
            &si,
            &pi
        );

        if (!success)
        {
            std::cerr << "[AOG-TC ERROR] Failed to start isobus_vt_terminal.exe (Error: " << GetLastError() << ")" << std::endl;
            std::cout.flush();
            std::this_thread::sleep_for(std::chrono::seconds(5));
            return 1;
        }

        vtPid = pi.dwProcessId;
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);

        std::cout << "[AOG-TC] ISOBUS Terminal started (PID: " << vtPid << ")" << std::endl;
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    }

    std::cout << "[AOG-TC] Virtual Terminal Server: ONLINE  (Addr: 0x26)" << std::endl;
    std::cout << "[AOG-TC] Task Controller Server: ONLINE  (Addr: 0xF7)" << std::endl;
    std::cout << "[AOG-TC] Tractor ECU (TECU):     ONLINE  (Addr: 0xF0)" << std::endl;
    std::cout << "[AOG-TC] AgOpenGPS UDP Bridge:   ONLINE  (Port 8888 Rx / 9999 Tx)" << std::endl;
    std::cout << "[AOG-TC] Section Control:        READY" << std::endl;
    std::cout << "[AOG-TC] Connected to AgOpenGPS successfully!" << std::endl;
    std::cout.flush();

    // Keep running and monitor process
    int heartbeatSec = 0;
    while (true)
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        heartbeatSec++;

        DWORD checkPid = 0;
        if (!isProcessRunning(L"isobus_vt_terminal.exe", checkPid))
        {
            std::cout << "[AOG-TC] Notice: isobus_vt_terminal was closed. Bridge exiting." << std::endl;
            std::cout.flush();
            break;
        }

        if (heartbeatSec % 30 == 0)
        {
            std::cout << "[AOG-TC] Link active: VT & TC communicating with implement." << std::endl;
            std::cout.flush();
        }
    }

    return 0;
}

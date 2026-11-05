#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <d3d11.h>
#include <tchar.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"

#include "CustomVTServer.hpp"
#include "AuxManager.hpp"
#include "CanManager.hpp"
#include "TaskControllerManager.hpp"
#include "VTRenderer.hpp"
#include "TerminalSettings.hpp"
#include "SplashScreen.hpp"
#include "AudioBeeper.hpp"
#include "TractorEcuManager.hpp"
#include "TcBasReportManager.hpp"
#include "IsobusFileServer.hpp"
#include "DdiExplorerManager.hpp"
#include "ObjectPoolInspector.hpp"
#include "AefPoolLinter.hpp" 

#include "isobus/isobus/can_network_manager.hpp"
#include "isobus/isobus/can_stack_logger.hpp"
#include "isobus/isobus/isobus_virtual_terminal_objects.hpp"
#include "isobus/utility/system_timing.hpp"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

// Executable directory helper
static std::wstring GetExecutableDir()
{
    wchar_t path[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p(path);
    auto pos = p.find_last_of(L"\\/");
    return (pos != std::wstring::npos) ? p.substr(0, pos) : L".";
}

// Unhandled exception crash filter
static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep)
{
    std::wstring dir = GetExecutableDir();
    std::wstring dmpPath = dir + L"\\vt_crash.dmp";
    std::wstring logPath = dir + L"\\vt_crash.log";

    std::wofstream log(logPath, std::ios::out | std::ios::trunc);
    if (log.is_open())
    {
        log << L"ISOBUS Universal Terminal Crash Report\n";
        log << L"Exception Code: 0x" << std::hex << ep->ExceptionRecord->ExceptionCode << L"\n";
        log << L"Exception Address: 0x" << std::hex << (uintptr_t)ep->ExceptionRecord->ExceptionAddress << L"\n";
        log.flush();
    }

    HANDLE hFile = CreateFileW(dmpPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile != INVALID_HANDLE_VALUE)
    {
        MINIDUMP_EXCEPTION_INFORMATION mei;
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = ep;
        mei.ClientPointers = FALSE;

        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile, MiniDumpNormal, &mei, nullptr, nullptr);
        CloseHandle(hFile);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// Data for DirectX 11
static ID3D11Device*            g_pd3dDevice = nullptr;
static ID3D11DeviceContext*     g_pd3dDeviceContext = nullptr;
static IDXGISwapChain*          g_pSwapChain = nullptr;
static UINT                     g_ResizeWidth = 0, g_ResizeHeight = 0;
static ID3D11RenderTargetView*  g_mainRenderTargetView = nullptr;

// Forward declarations
bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Forward declare ImGui Win32 handler
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Fullscreen Kiosk Mode Management
static bool g_isFullscreen = false;
static RECT g_prevWindowRect = {};
static DWORD g_prevWindowStyle = 0;

static void ToggleFullscreen(HWND hwnd)
{
    g_isFullscreen = !g_isFullscreen;
    if (g_isFullscreen)
    {
        g_prevWindowStyle = GetWindowLong(hwnd, GWL_STYLE);
        GetWindowRect(hwnd, &g_prevWindowRect);
        HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfo(hMon, &mi);

        SetWindowLong(hwnd, GWL_STYLE, g_prevWindowStyle & ~WS_OVERLAPPEDWINDOW);
        SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    else
    {
        SetWindowLong(hwnd, GWL_STYLE, g_prevWindowStyle);
        SetWindowPos(hwnd, nullptr, g_prevWindowRect.left, g_prevWindowRect.top,
                     g_prevWindowRect.right - g_prevWindowRect.left,
                     g_prevWindowRect.bottom - g_prevWindowRect.top,
                     SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
}

// HUD Overlay Mode Management (Dockable over AgOpenGPS)
static bool g_isHudMode = false;
static RECT g_prevHudRect = {};
static DWORD g_prevHudStyle = 0;
static DWORD g_prevHudExStyle = 0;

static void SetHudOverlayMode(HWND hwnd, bool enable, int dockSide = 0, float opacity = 0.88f, bool alwaysOnTop = true)
{
    g_isHudMode = enable;
    if (g_isHudMode)
    {
        g_prevHudStyle = GetWindowLong(hwnd, GWL_STYLE);
        g_prevHudExStyle = GetWindowLong(hwnd, GWL_EXSTYLE);
        GetWindowRect(hwnd, &g_prevHudRect);

        HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfo(hMon, &mi);

        int screenW = mi.rcWork.right - mi.rcWork.left;
        int screenH = mi.rcWork.bottom - mi.rcWork.top;
        int hudW = 440; // Ideal width for docked strip
        int hudH = screenH;
        int hudX = (dockSide == 0) ? (mi.rcWork.right - hudW) : mi.rcWork.left;
        int hudY = mi.rcWork.top;

        if (dockSide == 2) // Floating compact
        {
            hudW = 460;
            hudH = std::min(700, screenH - 60);
            hudX = mi.rcWork.right - hudW - 30;
            hudY = mi.rcWork.top + 30;
        }

        DWORD newStyle = (g_prevHudStyle & ~WS_MAXIMIZEBOX) | WS_POPUP | WS_VISIBLE | WS_BORDER;
        DWORD newExStyle = g_prevHudExStyle | WS_EX_LAYERED;
        if (alwaysOnTop)
        {
            newExStyle |= WS_EX_TOPMOST;
        }

        SetWindowLong(hwnd, GWL_STYLE, newStyle);
        SetWindowLong(hwnd, GWL_EXSTYLE, newExStyle);

        BYTE alpha = static_cast<BYTE>(std::max(0.40f, std::min(1.0f, opacity)) * 255.0f);
        SetLayeredWindowAttributes(hwnd, 0, alpha, LWA_ALPHA);

        SetWindowPos(hwnd, alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST,
                     hudX, hudY, hudW, hudH,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }
    else
    {
        SetWindowLong(hwnd, GWL_STYLE, g_prevHudStyle);
        SetWindowLong(hwnd, GWL_EXSTYLE, g_prevHudExStyle & ~(WS_EX_TOPMOST | WS_EX_LAYERED));
        SetWindowPos(hwnd, HWND_NOTOPMOST,
                     g_prevHudRect.left, g_prevHudRect.top,
                     g_prevHudRect.right - g_prevHudRect.left,
                     g_prevHudRect.bottom - g_prevHudRect.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }
}

static void ToggleHudMode(HWND hwnd)
{
    auto &cfg = isobus_vt::TerminalSettings::get_instance().get_config();
    cfg.hudModeEnabled = !cfg.hudModeEnabled;
    SetHudOverlayMode(hwnd, cfg.hudModeEnabled, cfg.hudDockSide, cfg.hudOpacity, cfg.hudAlwaysOnTop);
    isobus_vt::AudioBeeper::get_instance().play_touch_click();
}

// Modern Precision Ag Cockpit Theme (John Deere CommandCenter Gen5 / Trimble TMX Style)
static void SetupModernCommercialTheme(bool isDayMode)
{
    ImGuiStyle& style = ImGui::GetStyle();

    style.WindowPadding     = ImVec2(10.0f, 10.0f);
    style.FramePadding      = ImVec2(8.0f, 5.0f);
    style.CellPadding       = ImVec2(8.0f, 5.0f);
    style.ItemSpacing       = ImVec2(8.0f, 7.0f);
    style.ItemInnerSpacing  = ImVec2(6.0f, 6.0f);
    style.IndentSpacing     = 18.0f;
    style.ScrollbarSize     = 14.0f;
    style.GrabMinSize       = 12.0f;

    style.WindowRounding    = 8.0f;
    style.ChildRounding     = 6.0f;
    style.FrameRounding     = 5.0f;
    style.PopupRounding     = 6.0f;
    style.ScrollbarRounding = 6.0f;
    style.GrabRounding      = 4.0f;
    style.TabRounding       = 6.0f;

    style.WindowBorderSize  = 1.0f;
    style.ChildBorderSize   = 1.0f;
    style.PopupBorderSize   = 1.0f;
    style.FrameBorderSize   = 1.0f;
    style.TabBorderSize     = 0.0f;

    ImVec4* colors = style.Colors;
    if (isDayMode)
    {
        ImGui::StyleColorsLight();
        colors[ImGuiCol_WindowBg]              = ImVec4(0.93f, 0.95f, 0.97f, 0.98f);
        colors[ImGuiCol_MenuBarBg]             = ImVec4(0.88f, 0.91f, 0.95f, 1.00f);
        colors[ImGuiCol_TitleBg]               = ImVec4(0.84f, 0.88f, 0.93f, 1.00f);
        colors[ImGuiCol_TitleBgActive]         = ImVec4(0.78f, 0.84f, 0.92f, 1.00f);
        colors[ImGuiCol_Button]                = ImVec4(0.83f, 0.88f, 0.94f, 1.00f);
        colors[ImGuiCol_ButtonHovered]         = ImVec4(0.74f, 0.82f, 0.92f, 1.00f);
        colors[ImGuiCol_ButtonActive]          = ImVec4(0.68f, 0.77f, 0.90f, 1.00f);
        colors[ImGuiCol_CheckMark]             = ImVec4(0.12f, 0.68f, 0.35f, 1.00f);
        colors[ImGuiCol_Border]                = ImVec4(0.72f, 0.78f, 0.86f, 0.80f);
    }
    else
    {
        // Deep obsidian glass with precision emerald highlights
        colors[ImGuiCol_Text]                  = ImVec4(0.93f, 0.95f, 0.98f, 1.00f);
        colors[ImGuiCol_TextDisabled]          = ImVec4(0.48f, 0.54f, 0.64f, 1.00f);
        colors[ImGuiCol_WindowBg]              = ImVec4(0.08f, 0.10f, 0.15f, 0.98f);
        colors[ImGuiCol_ChildBg]               = ImVec4(0.10f, 0.13f, 0.19f, 0.85f);
        colors[ImGuiCol_PopupBg]               = ImVec4(0.09f, 0.11f, 0.16f, 0.98f);
        colors[ImGuiCol_Border]                = ImVec4(0.20f, 0.27f, 0.38f, 0.85f);
        colors[ImGuiCol_BorderShadow]          = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
        colors[ImGuiCol_FrameBg]               = ImVec4(0.12f, 0.16f, 0.23f, 1.00f);
        colors[ImGuiCol_FrameBgHovered]        = ImVec4(0.17f, 0.23f, 0.33f, 1.00f);
        colors[ImGuiCol_FrameBgActive]         = ImVec4(0.22f, 0.30f, 0.44f, 1.00f);
        colors[ImGuiCol_TitleBg]               = ImVec4(0.10f, 0.13f, 0.19f, 1.00f);
        colors[ImGuiCol_TitleBgActive]         = ImVec4(0.14f, 0.19f, 0.28f, 1.00f);
        colors[ImGuiCol_TitleBgCollapsed]      = ImVec4(0.08f, 0.10f, 0.14f, 0.90f);
        colors[ImGuiCol_MenuBarBg]             = ImVec4(0.07f, 0.09f, 0.13f, 1.00f);
        colors[ImGuiCol_ScrollbarBg]           = ImVec4(0.07f, 0.09f, 0.13f, 0.60f);
        colors[ImGuiCol_ScrollbarGrab]         = ImVec4(0.22f, 0.29f, 0.40f, 1.00f);
        colors[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.28f, 0.37f, 0.50f, 1.00f);
        colors[ImGuiCol_ScrollbarGrabActive]   = ImVec4(0.35f, 0.46f, 0.62f, 1.00f);
        colors[ImGuiCol_CheckMark]             = ImVec4(0.22f, 0.85f, 0.48f, 1.00f);
        colors[ImGuiCol_SliderGrab]            = ImVec4(0.18f, 0.72f, 0.42f, 1.00f);
        colors[ImGuiCol_SliderGrabActive]      = ImVec4(0.24f, 0.88f, 0.52f, 1.00f);
        colors[ImGuiCol_Button]                = ImVec4(0.14f, 0.19f, 0.28f, 1.00f);
        colors[ImGuiCol_ButtonHovered]         = ImVec4(0.20f, 0.27f, 0.40f, 1.00f);
        colors[ImGuiCol_ButtonActive]          = ImVec4(0.26f, 0.36f, 0.52f, 1.00f);
        colors[ImGuiCol_Header]                = ImVec4(0.14f, 0.20f, 0.30f, 0.80f);
        colors[ImGuiCol_HeaderHovered]         = ImVec4(0.20f, 0.28f, 0.42f, 0.85f);
        colors[ImGuiCol_HeaderActive]          = ImVec4(0.24f, 0.34f, 0.50f, 1.00f);
        colors[ImGuiCol_Separator]             = ImVec4(0.18f, 0.24f, 0.34f, 0.75f);
        colors[ImGuiCol_SeparatorHovered]      = ImVec4(0.24f, 0.34f, 0.48f, 1.00f);
        colors[ImGuiCol_SeparatorActive]       = ImVec4(0.22f, 0.85f, 0.48f, 1.00f);
        colors[ImGuiCol_ResizeGrip]            = ImVec4(0.20f, 0.27f, 0.38f, 0.30f);
        colors[ImGuiCol_ResizeGripHovered]     = ImVec4(0.28f, 0.38f, 0.54f, 0.60f);
        colors[ImGuiCol_ResizeGripActive]      = ImVec4(0.22f, 0.85f, 0.48f, 0.85f);
        colors[ImGuiCol_Tab]                   = ImVec4(0.10f, 0.14f, 0.20f, 1.00f);
        colors[ImGuiCol_TabHovered]            = ImVec4(0.18f, 0.25f, 0.38f, 1.00f);
        colors[ImGuiCol_TabActive]             = ImVec4(0.15f, 0.22f, 0.33f, 1.00f);
        colors[ImGuiCol_TabUnfocused]          = ImVec4(0.08f, 0.11f, 0.16f, 1.00f);
        colors[ImGuiCol_TabUnfocusedActive]    = ImVec4(0.12f, 0.17f, 0.26f, 1.00f);
        colors[ImGuiCol_TableHeaderBg]         = ImVec4(0.11f, 0.15f, 0.22f, 1.00f);
        colors[ImGuiCol_TableBorderStrong]     = ImVec4(0.20f, 0.27f, 0.38f, 1.00f);
        colors[ImGuiCol_TableBorderLight]      = ImVec4(0.14f, 0.19f, 0.28f, 1.00f);
        colors[ImGuiCol_TextSelectedBg]        = ImVec4(0.18f, 0.48f, 0.90f, 0.45f);
        colors[ImGuiCol_NavHighlight]          = ImVec4(0.22f, 0.85f, 0.48f, 1.00f);
    }
}

// Custom logger to file for VT diagnostics
class TerminalFileLogger : public isobus::CANStackLogger
{
public:
    TerminalFileLogger(const std::wstring &path) : file(path.c_str(), std::ios::out | std::ios::trunc) {}
    ~TerminalFileLogger() { if (file.is_open()) file.close(); }
    void sink_CAN_stack_log(CANStackLogger::LoggingLevel level, const std::string &text) override
    {
        if (file.is_open())
        {
            file << "[" << isobus::SystemTiming::get_timestamp_ms() << "] " << text << std::endl;
            file.flush();
        }
    }
private:
    std::ofstream file;
};

static std::unique_ptr<TerminalFileLogger> g_TerminalLogger;

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow)
{
    SetUnhandledExceptionFilter(CrashFilter);

    std::wstring exeDir = GetExecutableDir();
    g_TerminalLogger = std::make_unique<TerminalFileLogger>(exeDir + L"\\vt_terminal.log");
    // Create Application Window
    WNDCLASSEXW wc = {
        sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, hInstance,
        nullptr, nullptr, nullptr, nullptr, L"IsobusVTWindowClass", nullptr
    };
    ::RegisterClassExW(&wc);

    HWND hwnd = ::CreateWindowW(
        wc.lpszClassName,
        L"ISOBUS Universal Terminal (VT Server) with AUX-N [PEAK PCAN]",
        WS_OVERLAPPEDWINDOW,
        100, 100, 1480, 960,
        nullptr, nullptr, wc.hInstance, nullptr
    );

    // Initialize Direct3D
    if (!CreateDeviceD3D(hwnd))
    {
        CleanupDeviceD3D();
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    ::ShowWindow(hwnd, (nCmdShow == SW_HIDE || nCmdShow == 0) ? SW_SHOW : nCmdShow);
    ::UpdateWindow(hwnd);

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // Load TrueType fonts for crisp, non-blurry ISOBUS rendering with full Polish diacritics
    static const ImWchar glyphRangesPolish[] =
    {
        0x0020, 0x00FF, // Basic Latin + Latin Supplement (ó, Ó, etc.)
        0x0100, 0x017F, // Latin Extended-A (ą, ć, ę, ł, ń, ś, ź, ż, Ą, Ć, Ę, Ł, Ń, Ś, Ź, Ż)
        0x0180, 0x024F, // Latin Extended-B
        0x2010, 0x205E, // General Punctuation
        0x2190, 0x21FF, // Arrows
        0x25A0, 0x25FF, // Geometric Shapes (▶, ⏸, ⏹, etc.)
        0
    };

    ImFontConfig fontConfig;
    fontConfig.OversampleH = 3;
    fontConfig.OversampleV = 3;
    fontConfig.PixelSnapH = true;
    fontConfig.GlyphRanges = glyphRangesPolish;

    ImFont* fontRegular = nullptr;
    ImFont* fontMedium  = nullptr;
    ImFont* fontLarge   = nullptr;

    if (std::filesystem::exists("C:\\Windows\\Fonts\\segoeui.ttf"))
    {
        fontRegular = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 16.0f, &fontConfig, glyphRangesPolish);
        fontMedium  = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeuib.ttf", 22.0f, &fontConfig, glyphRangesPolish);
        fontLarge   = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeuib.ttf", 34.0f, &fontConfig, glyphRangesPolish);
    }
    else if (std::filesystem::exists("C:\\Windows\\Fonts\\arial.ttf"))
    {
        fontRegular = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\arial.ttf", 16.0f, &fontConfig, glyphRangesPolish);
        fontMedium  = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\arialbd.ttf", 22.0f, &fontConfig, glyphRangesPolish);
        fontLarge   = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\arialbd.ttf", 34.0f, &fontConfig, glyphRangesPolish);
    }
    else if (std::filesystem::exists("C:\\Windows\\Fonts\\consola.ttf"))
    {
        fontRegular = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\consola.ttf", 16.0f, &fontConfig, glyphRangesPolish);
        fontMedium  = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\consolab.ttf", 22.0f, &fontConfig, glyphRangesPolish);
        fontLarge   = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\consolab.ttf", 34.0f, &fontConfig, glyphRangesPolish);
    }
    else
    {
        fontRegular = io.Fonts->AddFontDefault(&fontConfig);
        fontMedium  = fontRegular;
        fontLarge   = fontRegular;
    }

    auto splashScreen = std::make_shared<isobus_vt::SplashScreen>();

    // Persistent Terminal & System Settings
    auto &ts = isobus_vt::TerminalSettings::get_instance();
    const auto &cfg = ts.get_config();

    // Apply Modern Commercial Precision Ag Cockpit Theme
    bool isDayMode = cfg.isDayMode;
    SetupModernCommercialTheme(isDayMode);

    // Setup Platform/Renderer backends
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);
    isobus_vt::TractorEcuManager::get_instance().set_d3d_device(g_pd3dDevice);

    // --- Initialize ISOBUS Stack ---
    isobus::CANStackLogger::set_can_stack_logger_sink(g_TerminalLogger.get());
    isobus::CANStackLogger::set_log_level(isobus::CANStackLogger::LoggingLevel::Debug);

    isobus::NAME vtName(0);
    vtName.set_arbitrary_address_capable(true);
    vtName.set_industry_group(2); // Agriculture
    vtName.set_device_class(0);
    vtName.set_function_code(static_cast<std::uint8_t>(isobus::NAME::Function::VirtualTerminal)); // 29
    vtName.set_identity_number(0x0001);
    vtName.set_ecu_instance(0);
    vtName.set_function_instance(0);

    // Create Virtual Terminal InternalControlFunction with preferred address 0x26 (38)
    auto vtServerCF = isobus::CANNetworkManager::CANNetwork.create_internal_control_function(vtName, 0, 0x26);

    auto vtServer = std::make_shared<isobus_vt::CustomVTServer>(vtServerCF);
    vtServer->initialize();
    isobus_vt::ObjectPoolInspector::get_instance().set_vt_server(vtServer);
    isobus_vt::AefPoolLinter::get_instance().set_vt_server(vtServer);

    auto auxManager = std::make_shared<isobus_vt::AuxManager>(vtServerCF);
    auxManager->set_enabled(cfg.auxEnabled);
    if (cfg.auxEnabled)
    {
        auxManager->initialize();
    }

    // Initialize CAN Driver from saved configuration (PEAK PCAN or Virtual)
    int selectedDriverIndex = cfg.canDriver;
    int selectedPcanChannel = cfg.pcanChannel;
    auto canManager = std::make_shared<isobus_vt::CanManager>();
    auto driverType = (selectedDriverIndex == 0) ? isobus_vt::CanDriverType::PeakPcan : isobus_vt::CanDriverType::VirtualLoopback;
    canManager->initialize(driverType, selectedPcanChannel);

    auto tcManager = std::make_shared<isobus_vt::TaskControllerManager>();
    if (cfg.tcEnabled)
    {
        tcManager->initialize(0);
    }

    auto vtRenderer = std::make_shared<isobus_vt::VTRenderer>(vtServer);
    vtRenderer->set_managers(canManager, tcManager);

    // Subsystems enable states
    isobus_vt::IsobusFileServer::get_instance().set_enabled(cfg.fileServerEnabled);
    isobus_vt::DdiExplorerManager::get_instance().set_enabled(cfg.ddiExplorerEnabled);
    isobus_vt::TractorEcuManager::get_instance().set_enabled(cfg.tecuEnabled);
    if (cfg.tecuEnabled)
    {
        isobus_vt::TractorEcuManager::get_instance().start();
    }

    // UI state variables initialized from persistent configuration
    bool showAuxWindow = cfg.showAuxWindow && cfg.auxEnabled;
    bool showCanLogWindow = cfg.showCanLogWindow;
    bool showWsInspector = cfg.showWsInspector;
    bool showAogTcWindow = cfg.showAogTcWindow;
    bool showDiagnosticsWindow = cfg.showDiagnosticsWindow;
    bool showMiniViewsWindow = cfg.showMiniViewsWindow && cfg.miniViewsEnabled;
    bool showTractorWindow = cfg.tecuEnabled;
    bool showTractorPgnConfig = false;
    bool showFileServerWindow = false;
    bool showDdiExplorerWindow = false;
    bool showPoolInspectorWindow = false;
    bool showAefLinterWindow = false;
    static bool g_TriggerResetLayout = false;
    bool alarmSoundMuted = cfg.alarmSoundMuted;
    float uiTouchScale = cfg.uiTouchScale;

    if (cfg.hudModeEnabled)
    {
        SetHudOverlayMode(hwnd, true, cfg.hudDockSide, cfg.hudOpacity, cfg.hudAlwaysOnTop);
    }

    // Virtual Joystick test sliders / buttons state
    bool virtButtonState[8] = {false};
    int virtSlider1 = 0;
    int virtSlider2 = 50;

    // Main loop
    bool done = false;
    try {
    while (!done)
    {
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE))
        {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
                done = true;
        }
        if (done)
            break;

        // Resize swap chain if needed
        if (g_ResizeWidth != 0 && g_ResizeHeight != 0)
        {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight, DXGI_FORMAT_UNKNOWN, 0);
            g_ResizeWidth = g_ResizeHeight = 0;
            CreateRenderTarget();
        }

        // Periodic updates for ISOBUS stack, AUX and Task Controller
        vtServer->run_update();
        if (cfg.auxEnabled && auxManager)
            auxManager->update();
        if (cfg.tcEnabled && tcManager && tcManager->is_initialized())
            tcManager->update();
        if (cfg.fileServerEnabled)
            isobus_vt::IsobusFileServer::get_instance().update();
        if (cfg.tecuEnabled)
            isobus_vt::TractorEcuManager::get_instance().update();

        // Auto-select active working set if none is active and an object pool is ready
        if (!vtServer->get_active_ws())
        {
            auto allWs = vtServer->get_all_ws();
            for (auto &ws : allWs)
            {
                if (ws &&
                    ws->get_working_set_object() != nullptr &&
                    ws->get_object_pool_processing_state() != isobus::VirtualTerminalServerManagedWorkingSet::ObjectPoolProcessingThreadState::Running)
                {
                    vtServer->select_working_set(ws);
                    break;
                }
            }
        }

        // Request address claim ONCE upon initial CAN connection (never poll periodically to avoid pruning active CFs)
        static bool hasRequestedInitialAddressClaim = false;
        if (canManager->is_connected())
        {
            if (!hasRequestedInitialAddressClaim)
            {
                hasRequestedInitialAddressClaim = true;
                isobus::CANNetworkManager::CANNetwork.send_request_for_address_claim(0);
            }
        }
        else
        {
            hasRequestedInitialAddressClaim = false;
        }

        // Start Dear ImGui frame
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // Handle F10 Fullscreen hotkey
        if (ImGui::IsKeyPressed(ImGuiKey_F10))
        {
            ToggleFullscreen(hwnd);
        }

        // Handle F11 HUD Overlay Mode hotkey
        if (ImGui::IsKeyPressed(ImGuiKey_F11))
        {
            ToggleHudMode(hwnd);
        }

        // Handle F1 Splash Screen boot animation
        if (ImGui::IsKeyPressed(ImGuiKey_F1))
        {
            if (splashScreen) splashScreen->start();
        }

        // Handle F2 Settings dialog hotkey
        if (ImGui::IsKeyPressed(ImGuiKey_F2))
        {
            if (vtRenderer) vtRenderer->open_settings_dialog();
        }

        // Handle Audible Alarm Buzzer
        if (vtServer->is_alarm_active())
        {
            isobus_vt::AudioBeeper::get_instance().update_alarm_loop(true);
        }

        // ── BOOT SEQUENCE: When Splash Screen is active, ONLY render the splash screen! ──
        // Application windows, tiles and menu bar appear cleanly after loading finishes.
        if (splashScreen && splashScreen->is_active())
        {
            splashScreen->render(fontLarge, fontMedium, fontRegular);
        }
        else
        {
            // First frame after splash screen: trigger clean responsive layout & startup window selection
            static bool initialLayoutApplied = false;
            static int tcActiveTabTarget = cfg.tcStartupTab;
            if (!initialLayoutApplied)
            {
                initialLayoutApplied = true;
                g_TriggerResetLayout = true;
                if (vtRenderer) vtRenderer->reset_layout();

                // Apply user-selected startup window
                switch (cfg.startupWindow)
                {
                    case 1: // Task Controller & ŚOR
                        showAogTcWindow = true;
                        tcActiveTabTarget = cfg.tcStartupTab;
                        break;
                    case 2: // Tractor Telemetry TECU
                        showTractorWindow = true;
                        break;
                    case 3: // CAN Bus Monitor
                        showCanLogWindow = true;
                        break;
                    case 4: // AUX-N Control Center
                        showAuxWindow = true;
                        break;
                    case 5: // Multi-Working Set Mini-Views
                        showMiniViewsWindow = true;
                        break;
                    case 6: // DDI Process Data Explorer
                        showDdiExplorerWindow = true;
                        break;
                    case 7: // Live Object Pool Inspector
                        showPoolInspectorWindow = true;
                        break;
                    case 8: // AEF Conformance Linter
                        showAefLinterWindow = true;
                        break;
                    case 9: // ISOBUS File Server
                        showFileServerWindow = true;
                        break;
                    case 0: // Virtual Terminal Server (Default)
                    default:
                        break;
                }
            }

            // Bring selected startup window into foreground focus on first frame
            static bool startupFocusApplied = false;
            if (!startupFocusApplied)
            {
                startupFocusApplied = true;
                switch (cfg.startupWindow)
                {
                    case 1: ImGui::SetWindowFocus("Task Controller (TC-BAS & SC) | Ewidencja ŚOR 2027###AogTcWindow"); break;
                    case 2: ImGui::SetWindowFocus("Telemetria Ciągnika (TECU / J1939)###TractorDashboard"); break;
                    case 3: ImGui::SetWindowFocus("CAN Bus Monitor & Diagnostics"); break;
                    case 4: ImGui::SetWindowFocus("AUX-N Control Center"); break;
                    case 5: ImGui::SetWindowFocus("Podglad Maszyn w Tle (Mini-Views)###MiniViews"); break;
                    case 6: ImGui::SetWindowFocus("DDI Process Data Explorer (ISO 11783-11)###DdiExplorer"); break;
                    case 7: ImGui::SetWindowFocus("Live Object Pool Tree Browser & Inspector###PoolInspector"); break;
                    case 8: ImGui::SetWindowFocus("AEF Object Pool Linter & Conformance Checker###AefLinter"); break;
                    case 9: ImGui::SetWindowFocus("ISOBUS File Server (ISO 11783-13)###FileServer"); break;
                    default: break;
                }
            }

            // Draw precision ag cockpit canvas background
            ImDrawList* bgDl = ImGui::GetBackgroundDrawList();
            ImGuiViewport* vp = ImGui::GetMainViewport();
            bgDl->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y), IM_COL32(11, 14, 20, 255));
            for (float gx = vp->Pos.x + 24.0f; gx < vp->Pos.x + vp->Size.x; gx += 44.0f)
            {
                for (float gy = vp->Pos.y + 40.0f; gy < vp->Pos.y + vp->Size.y; gy += 44.0f)
                {
                    bgDl->AddCircleFilled(ImVec2(gx, gy), 1.0f, IM_COL32(28, 38, 54, 75));
                }
            }

            // 1. Top Menu / Hardware Control Bar (Modern Console Header)
            ImGui::BeginMainMenuBar();
            {
                // Commercial Console Brand Pill
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.20f, 0.92f, 0.50f, 1.0f));
                ImGui::Text(" \xE2\x98\x85 AG-ISOBUS PRO ");
                ImGui::PopStyleColor();
                ImGui::Separator();

            // Menu: Hardware
            if (ImGui::BeginMenu("Hardware"))
            {
                ImGui::TextDisabled("CAN Bus Driver:");
                bool isPcan = (selectedDriverIndex == 0);
                bool isLoopback = (selectedDriverIndex == 1);
                if (ImGui::MenuItem("PEAK PCAN-USB", nullptr, isPcan))
                {
                    selectedDriverIndex = 0;
                    canManager->initialize(isobus_vt::CanDriverType::PeakPcan, selectedPcanChannel);
                }
                if (ImGui::MenuItem("Virtual Loopback CAN", nullptr, isLoopback))
                {
                    selectedDriverIndex = 1;
                    canManager->initialize(isobus_vt::CanDriverType::VirtualLoopback, selectedPcanChannel);
                }

                if (selectedDriverIndex == 0)
                {
                    ImGui::Separator();
                    ImGui::TextDisabled("PCAN Channel:");
                    for (int ch = 0; ch < 8; ++ch)
                    {
                        std::string chLabel = "PCAN USB " + std::to_string(ch + 1);
                        bool isSel = (selectedPcanChannel == PCAN_USBBUS1 + ch);
                        if (ImGui::MenuItem(chLabel.c_str(), nullptr, isSel))
                        {
                            selectedPcanChannel = PCAN_USBBUS1 + ch;
                            canManager->initialize(isobus_vt::CanDriverType::PeakPcan, selectedPcanChannel);
                        }
                    }
                }

                ImGui::Separator();
                if (ImGui::MenuItem("Reconnect CAN Bus"))
                {
                    canManager->reconnect();
                    isobus::CANNetworkManager::CANNetwork.send_request_for_address_claim(0);
                }
                ImGui::EndMenu();
            }

            // Menu: Working Set (Multi-ECU switcher)
            auto allWs = vtServer->get_all_ws();
            auto activeWs = vtServer->get_active_ws();
            std::string wsTitle = "Working Set";
            if (activeWs && activeWs->get_control_function())
            {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "WS: ECU 0x%02X", activeWs->get_control_function()->get_address());
                wsTitle = buf;
            }
            if (ImGui::BeginMenu(wsTitle.c_str()))
            {
                if (allWs.empty())
                {
                    ImGui::TextDisabled("No Working Sets detected on CAN bus.");
                }
                else
                {
                    for (std::size_t i = 0; i < allWs.size(); ++i)
                    {
                        if (!allWs[i] || !allWs[i]->get_control_function()) continue;
                        char label[64];
                        std::uint8_t addr = allWs[i]->get_control_function()->get_address();
                        std::snprintf(label, sizeof(label), "ECU 0x%02X (%u)", addr, addr);
                        bool isCur = (allWs[i] == activeWs);
                        if (ImGui::MenuItem(label, nullptr, isCur))
                        {
                            vtServer->switch_working_set(i);
                        }
                    }
                }
                ImGui::EndMenu();
            }

            // Menu: View
            if (ImGui::BeginMenu("View"))
            {
                if (ImGui::MenuItem("Tryb Nakladki HUD na AgOpenGPS (F11)", nullptr, g_isHudMode))
                {
                    ToggleHudMode(hwnd);
                }
                if (ImGui::MenuItem("Fullscreen (F10)", nullptr, g_isFullscreen))
                {
                    ToggleFullscreen(hwnd);
                }
                if (ImGui::MenuItem(isDayMode ? "Night Mode" : "Day Mode"))
                {
                    isDayMode = !isDayMode;
                    SetupModernCommercialTheme(isDayMode);
                }
                if (ImGui::MenuItem("Mute Alarm Buzzer", nullptr, alarmSoundMuted))
                {
                    alarmSoundMuted = !alarmSoundMuted;
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Ekran powitalny (Splash Screen)...", "F1"))
                {
                    if (splashScreen) splashScreen->start();
                }
                if (ImGui::MenuItem("Reset Window Layout"))
                {
                    g_TriggerResetLayout = true;
                    if (vtRenderer) vtRenderer->reset_layout();
                }
                ImGui::EndMenu();
            }

            // Menu: Panels
            if (ImGui::BeginMenu("Panels"))
            {
                ImGui::MenuItem("Task Controller & Ewidencja ŚOR (TC-BAS / SC)", nullptr, &showAogTcWindow);
                ImGui::MenuItem("AUX-N Control Center", nullptr, &showAuxWindow);
                ImGui::MenuItem("CAN Bus Monitor", nullptr, &showCanLogWindow);
                ImGui::MenuItem("Working Set Inspector", nullptr, &showWsInspector);
                ImGui::MenuItem("Machine Diagnostics (DM1 / DM2)", nullptr, &showDiagnosticsWindow);
                ImGui::MenuItem("Podglad Maszyn w Tle (Mini-Views)", nullptr, &showMiniViewsWindow);
                ImGui::Separator();
                ImGui::MenuItem("Telemetria Ciagnika (TUZ / WOM / Silnik)", nullptr, &showTractorWindow);
                ImGui::MenuItem("Konfiguracja PGN Ciagnika (2. PCAN)", nullptr, &showTractorPgnConfig);
                ImGui::EndMenu();
            }

            // Menu: ISOBUS Tools
            if (ImGui::BeginMenu("ISOBUS Tools"))
            {
                ImGui::MenuItem("ISOBUS File Server (ISO 11783-13)", nullptr, &showFileServerWindow);
                ImGui::MenuItem("DDI Process Data Explorer (ISO 11783-11)", nullptr, &showDdiExplorerWindow);
                ImGui::MenuItem("Live Object Pool Tree Browser & Inspector", nullptr, &showPoolInspectorWindow);
                ImGui::MenuItem("AEF Object Pool Linter & Conformance Checker", nullptr, &showAefLinterWindow);
                ImGui::EndMenu();
            }

            // Menu: Settings / Konfiguracja
            if (ImGui::BeginMenu("Settings"))
            {
                if (ImGui::MenuItem("Konfiguracja Terminala (Setup)...", "F2"))
                {
                    if (vtRenderer) vtRenderer->open_settings_dialog();
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Zapisz biezacy stan jako domyslny"))
                {
                    auto &c = isobus_vt::TerminalSettings::get_instance().get_config();
                    c.showAuxWindow = showAuxWindow;
                    c.showCanLogWindow = showCanLogWindow;
                    c.showWsInspector = showWsInspector;
                    c.showAogTcWindow = showAogTcWindow;
                    c.showDiagnosticsWindow = showDiagnosticsWindow;
                    c.showMiniViewsWindow = showMiniViewsWindow;
                    c.hudModeEnabled = g_isHudMode;
                    c.isDayMode = isDayMode;
                    c.alarmSoundMuted = alarmSoundMuted;
                    c.canDriver = selectedDriverIndex;
                    c.pcanChannel = selectedPcanChannel;
                    isobus_vt::TerminalSettings::get_instance().save();
                    isobus_vt::TerminalSettings::get_instance().play_touch_beep();
                }
                ImGui::EndMenu();
            }

            ImGui::Separator();

            // Compact CAN Status badge
            float busLoad = canManager->get_bus_load();
            if (canManager->is_connected())
            {
                ImVec4 loadCol = (busLoad < 50.0f) ? ImVec4(0.20f, 0.90f, 0.45f, 1.0f) :
                                 (busLoad < 80.0f) ? ImVec4(1.0f, 0.8f, 0.2f, 1.0f) :
                                                     ImVec4(1.0f, 0.3f, 0.2f, 1.0f);
                ImGui::TextColored(loadCol, "[ CAN: OK | %4.1f%% ]", busLoad);
            }
            else
            {
                ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.35f, 1.0f), "[ CAN: Offline ]");
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted(canManager->get_status_string().c_str());
                ImGui::Separator();
                ImGui::Text("RX Frames: %llu", canManager->get_rx_count());
                ImGui::Text("TX Frames: %llu", canManager->get_tx_count());
                ImGui::Text("CAN FPS:   %.1f", canManager->get_fps());
                ImGui::Text("Bus Load:  %.1f%% (250 kbps)", busLoad);
                ImGui::EndTooltip();
            }

            // Compact AOG TC Status badge
            if (!cfg.tcEnabled)
            {
                ImGui::TextColored(ImVec4(0.50f, 0.55f, 0.60f, 1.0f), "[ TC: Wylaczony ]");
                if (ImGui::IsItemHovered())
                {
                    ImGui::BeginTooltip();
                    ImGui::TextUnformatted("Task Controller jest wylaczony w konfiguracji.");
                    ImGui::TextDisabled("Przejdz do: Konfiguracja (F2) -> Moduly & Autostart aby wlaczyc.");
                    ImGui::EndTooltip();
                }
            }
            else
            {
            bool aogActive = tcManager->is_aog_connected();
            if (aogActive)
            {
                ImGui::TextColored(ImVec4(0.20f, 0.90f, 0.45f, 1.0f), "[ AOG TC: OK ]");
            }
            else
            {
                ImGui::TextColored(ImVec4(0.60f, 0.65f, 0.75f, 1.0f), "[ AOG TC: Waiting ]");
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted("AgOpenGPS Section Control Bridge");
                ImGui::Text("UDP Rx: 8888 | Tx: 9999");
                ImGui::Text("Sections: %u | Mode: %s",
                            tcManager->get_num_sections(),
                            tcManager->is_auto_mode() ? "AUTO" : "MANUAL");
                ImGui::Text("Speed: %.1f km/h", tcManager->get_speed_kph());
                ImGui::EndTooltip();
            }
            }

            // Tractor TECU Status badge
            if (!cfg.tecuEnabled)
            {
                ImGui::TextColored(ImVec4(0.50f, 0.55f, 0.60f, 1.0f), "[ TECU: Wylaczony ]");
                if (ImGui::IsItemHovered())
                {
                    ImGui::BeginTooltip();
                    ImGui::TextUnformatted("Telemetria ciagnika (TECU) jest wylaczona w konfiguracji.");
                    ImGui::TextDisabled("Przejdz do: Konfiguracja (F2) -> Moduly & Autostart aby wlaczyc.");
                    ImGui::EndTooltip();
                }
            }
            else
            {
            bool tecuConnected = isobus_vt::TractorEcuManager::get_instance().is_connected();
            auto tecuTel = isobus_vt::TractorEcuManager::get_instance().get_telemetry();
            bool tecuLive = tecuConnected && (tecuTel.totalFramesReceived > 0);
            if (tecuLive)
            {
                ImVec4 tecuCol = tecuTel.isWorkConditionMet ? ImVec4(0.20f, 0.90f, 0.45f, 1.0f) : ImVec4(0.40f, 0.80f, 1.0f, 1.0f);
                ImGui::TextColored(tecuCol, "[ TECU: %s ]", tecuTel.isWorkConditionMet ? "PRACA" : "OK");
            }
            else
            {
                ImGui::TextColored(ImVec4(0.55f, 0.60f, 0.70f, 1.0f), "[ TECU: Czuwanie ]");
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted("Tractor ECU (J1939 / ISO 11783-7)");
                ImGui::Separator();
                ImGui::Text("TUZ: %.1f%% (%s)", tecuTel.rearHitchPositionPct, tecuTel.isRearHitchLowered ? "Opuszczony" : "Podniesiony");
                ImGui::Text("WOM: %.0f RPM (%s)", tecuTel.rearPtoRpm, tecuTel.isRearPtoActive ? "Zalaczony" : "Wylaczony");
                ImGui::Text("Silnik: %.0f RPM | Spalanie: %.1f l/h", tecuTel.engineRpm, tecuTel.fuelRateLitersPerHour);
                ImGui::Text("Szyna: %s", isobus_vt::TractorEcuManager::get_instance().get_status_string().c_str());
                ImGui::EndTooltip();
            }
            }

            // VT Version badge
            {
                const auto &cfg = isobus_vt::TerminalSettings::get_instance().get_config();
                ImGui::TextColored(ImVec4(0.40f, 0.85f, 1.0f, 1.0f), "[ VT: v%d ]", cfg.vtVersion);
                if (ImGui::IsItemHovered())
                {
                    ImGui::BeginTooltip();
                    ImGui::Text("ISO 11783-6 Virtual Terminal Version %d", cfg.vtVersion);
                    ImGui::Separator();
                    ImGui::Text("Data Mask: %d x %d px", cfg.dataMaskWidth, cfg.dataMaskHeight);
                    ImGui::Text("Soft Keys: %d kol. x %d wierszy (%d x %d px)",
                                cfg.skColumns, cfg.skRows, cfg.skDescWidth, cfg.skDescHeight);
                    ImGui::Text("Liczba klawiszy: %d (F1..F%d)", cfg.skColumns * cfg.skRows, cfg.skColumns * cfg.skRows);
                    ImGui::EndTooltip();
                }
            }

            // Real-time Digital System Clock
            SYSTEMTIME st;
            GetLocalTime(&st);
            char clockBuf[16];
            std::snprintf(clockBuf, sizeof(clockBuf), "%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);
            ImGui::Separator();
            ImGui::TextColored(ImVec4(0.55f, 0.75f, 0.95f, 1.0f), "[ %s ]", clockBuf);

            // Modern Styled Actions
            ImGui::Separator();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.35f, 0.22f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.18f, 0.48f, 0.30f, 1.0f));
            if (ImGui::SmallButton("Konfiguracja (F2)"))
            {
                if (vtRenderer) vtRenderer->open_settings_dialog();
            }
            ImGui::PopStyleColor(2);

            ImGui::SameLine();
            if (ImGui::SmallButton("Reset Układu"))
            {
                g_TriggerResetLayout = true;
                if (vtRenderer) vtRenderer->reset_layout();
            }

            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, g_isHudMode ? ImVec4(0.85f, 0.40f, 0.15f, 1.0f) : ImVec4(0.18f, 0.30f, 0.48f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, g_isHudMode ? ImVec4(0.95f, 0.50f, 0.20f, 1.0f) : ImVec4(0.24f, 0.40f, 0.60f, 1.0f));
            if (ImGui::SmallButton(g_isHudMode ? " 🖥️ Tryb Studio (F11) " : " 🪟 Tryb HUD (F11) "))
            {
                ToggleHudMode(hwnd);
            }
            ImGui::PopStyleColor(2);

            if (g_isHudMode)
            {
                ImGui::SameLine();
                const char* dockSideLabel = (cfg.hudDockSide == 0) ? "[ Dok: Prawa ]" :
                                            (cfg.hudDockSide == 1) ? "[ Dok: Lewa ]" : "[ Pływający ]";
                if (ImGui::SmallButton(dockSideLabel))
                {
                    auto &mutCfg = isobus_vt::TerminalSettings::get_instance().get_config();
                    mutCfg.hudDockSide = (mutCfg.hudDockSide + 1) % 3;
                    SetHudOverlayMode(hwnd, true, mutCfg.hudDockSide, mutCfg.hudOpacity, mutCfg.hudAlwaysOnTop);
                    isobus_vt::AudioBeeper::get_instance().play_touch_click();
                }
            }
        }
        ImGui::EndMainMenuBar();

        // Viewport layout calculation
        float vpW = vp->Size.x;
        float vpH = vp->Size.y;
        float leftColW = (vpW > 1320.0f) ? 730.0f : (vpW * 0.52f);
        float rightColX = leftColW + 16.0f;
        float rightColW = std::max(400.0f, vpW - rightColX - 10.0f);

        // 2. Main Terminal Viewport Window
        vtRenderer->render_terminal_window();

        // 2b. Multi-Working Set Mini-Views Window (Podgląd Maszyn w Tle)
        if (showMiniViewsWindow && !g_isHudMode)
        {
            vtRenderer->render_mini_views_window(&showMiniViewsWindow);
        }

        // 3. AUX-N Control Center Window
        if (showAuxWindow && !g_isHudMode)
        {
            if (g_TriggerResetLayout)
            {
                ImGui::SetNextWindowPos(ImVec2(8.0f, 626.0f), ImGuiCond_Always);
                ImGui::SetNextWindowSize(ImVec2(leftColW, std::max(280.0f, vpH - 636.0f)), ImGuiCond_Always);
                ImGui::SetNextWindowCollapsed(false, ImGuiCond_Always);
            }
            else
            {
                ImGui::SetNextWindowPos(ImVec2(8.0f, 626.0f), ImGuiCond_FirstUseEver);
                ImGui::SetNextWindowSize(ImVec2(leftColW, 320.0f), ImGuiCond_FirstUseEver);
                ImGui::SetNextWindowCollapsed(false, ImGuiCond_Appearing);
            }
            ImGui::Begin("AUX-N Control Center", &showAuxWindow, ImGuiWindowFlags_NoCollapse);
            if (ImGui::BeginTabBar("AuxTabs"))
            {
                // Tab 1: Assignments Management
                if (ImGui::BeginTabItem("Assignments (Mapowanie)"))
                {
                    auto activeWs = vtServer->get_active_ws();
                    auto auxFns = auxManager->get_available_functions(activeWs);
                    auto inputDevs = auxManager->get_input_devices();
                    auto currentAssignments = auxManager->get_assignments();

                    ImGui::Text("Map physical or virtual joystick inputs to implement AUX-N functions:");
                    ImGui::Separator();

                    static int selectedFnIdx = 0;
                    static int selectedDevIdx = 0;
                    static int selectedInputObjId = 1;
                    static const char* inputTypes[] = { "Button 1", "Button 2", "Button 3", "Button 4", "Button 5", "Button 6", "Button 7", "Button 8", "Axis X (Proportional)", "Axis Y (Proportional)" };
                    static int selectedInputTypeIdx = 0;

                    if (!auxFns.empty() && !inputDevs.empty())
                    {
                        ImGui::Columns(2, "AuxCols", true);

                        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "1. Select Implement Function:");
                        std::vector<std::string> fnLabels;
                        for (const auto &fn : auxFns)
                            fnLabels.push_back(fn.description);

                        if (selectedFnIdx >= static_cast<int>(fnLabels.size())) selectedFnIdx = 0;
                        if (ImGui::BeginListBox("##FnList", ImVec2(-1, 140)))
                        {
                            for (int i = 0; i < static_cast<int>(fnLabels.size()); ++i)
                            {
                                bool isSel = (selectedFnIdx == i);
                                if (ImGui::Selectable(fnLabels[i].c_str(), isSel))
                                    selectedFnIdx = i;
                            }
                            ImGui::EndListBox();
                        }

                        ImGui::NextColumn();

                        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "2. Select Input Device & Control:");
                        std::vector<std::string> devLabels;
                        for (const auto &dev : inputDevs)
                            devLabels.push_back(dev.nameString);

                        if (selectedDevIdx >= static_cast<int>(devLabels.size())) selectedDevIdx = 0;
                        if (ImGui::BeginCombo("Device", devLabels[selectedDevIdx].c_str()))
                        {
                            for (int i = 0; i < static_cast<int>(devLabels.size()); ++i)
                            {
                                if (ImGui::Selectable(devLabels[i].c_str(), selectedDevIdx == i))
                                    selectedDevIdx = i;
                            }
                            ImGui::EndCombo();
                        }

                        ImGui::Combo("Control", &selectedInputTypeIdx, inputTypes, 10);
                        selectedInputObjId = selectedInputTypeIdx + 1;

                        ImGui::Spacing();
                        if (ImGui::Button("Assign Control to Function", ImVec2(220, 32)))
                        {
                            if (activeWs && activeWs->get_control_function())
                            {
                                const auto &fn = auxFns[selectedFnIdx];
                                const auto &dev = inputDevs[selectedDevIdx];
                                auxManager->assign_function(
                                    activeWs->get_control_function(),
                                    activeWs->get_control_function()->get_NAME().get_full_name(),
                                    fn.functionObjectId,
                                    dev.rawName,
                                    static_cast<std::uint16_t>(selectedInputObjId),
                                    static_cast<std::uint8_t>(fn.functionType),
                                    fn.description,
                                    inputTypes[selectedInputTypeIdx]
                                );
                            }
                        }

                        ImGui::Columns(1);
                    }
                    else
                    {
                        if (auxFns.empty())
                            ImGui::TextDisabled("No Auxiliary Functions found in active Implement Object Pool.");
                        if (inputDevs.empty())
                            ImGui::TextDisabled("No Auxiliary Input Devices (joysticks) discovered on CAN.");
                    }

                    ImGui::Separator();
                    ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.3f, 1.0f), "Current Active Assignments (%zu):", currentAssignments.size());

                    if (ImGui::BeginTable("AssignTable", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, 160)))
                    {
                        ImGui::TableSetupColumn("Function Object");
                        ImGui::TableSetupColumn("Function Type");
                        ImGui::TableSetupColumn("Assigned Input Device");
                        ImGui::TableSetupColumn("Input Control");
                        ImGui::TableSetupColumn("Action");
                        ImGui::TableHeadersRow();

                        for (const auto &asgn : currentAssignments)
                        {
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn();
                            ImGui::Text("%s (#%u)", asgn.functionDescription.c_str(), asgn.functionObjectId);
                            ImGui::TableNextColumn();
                            ImGui::Text("%s", isobus_vt::AuxManager::get_function_type_name(static_cast<isobus::AuxiliaryFunctionType2::FunctionType>(asgn.functionType)));
                            ImGui::TableNextColumn();
                            ImGui::Text("0x%llX", asgn.inputDeviceName);
                            ImGui::TableNextColumn();
                            ImGui::Text("%s (#%u)", asgn.inputDescription.c_str(), asgn.inputObjectId);
                            ImGui::TableNextColumn();
                            std::string unbtn = "Remove##" + std::to_string(asgn.functionObjectId);
                            if (ImGui::Button(unbtn.c_str()))
                            {
                                if (activeWs && activeWs->get_control_function())
                                    auxManager->unassign_function(activeWs->get_control_function(), asgn.implementName, asgn.functionObjectId);
                            }
                        }
                        ImGui::EndTable();
                    }

                    ImGui::EndTabItem();
                }

                // Tab 2: Virtual Joystick Simulator
                if (ImGui::BeginTabItem("Virtual Joystick Simulator"))
                {
                    bool vJoyEnabled = auxManager->get_virtual_joystick_enabled();
                    if (ImGui::Checkbox("Enable Virtual ISOBUS Joystick on CAN", &vJoyEnabled))
                    {
                        auxManager->set_virtual_joystick_enabled(vJoyEnabled);
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("(Broadcasts Maintenance 0x23 every 1s, lets you test without physical joystick)");

                    ImGui::Separator();
                    ImGui::Text("Buttons (Push / Momentary):");

                    // 8 buttons in 2 rows
                    for (int b = 0; b < 8; ++b)
                    {
                        std::string bLabel = "Button " + std::to_string(b + 1);
                        if (b % 4 != 0) ImGui::SameLine();

                        if (ImGui::Button(bLabel.c_str(), ImVec2(90, 40)))
                        {
                            // Trigger momentary toggle
                            virtButtonState[b] = !virtButtonState[b];
                            auxManager->simulate_virtual_input(static_cast<std::uint16_t>(b + 1), virtButtonState[b] ? 1 : 0);
                        }
                        if (ImGui::IsItemActive())
                        {
                            // Held down
                            auxManager->simulate_virtual_input(static_cast<std::uint16_t>(b + 1), 1);
                        }
                        else if (ImGui::IsItemDeactivated())
                        {
                            // Released
                            auxManager->simulate_virtual_input(static_cast<std::uint16_t>(b + 1), 0);
                        }
                    }

                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Text("Proportional Analog Controls:");

                    if (ImGui::SliderInt("Axis X (Input #9)", &virtSlider1, -100, 100))
                    {
                        std::uint16_t rawVal = static_cast<std::uint16_t>((virtSlider1 + 100) * 327); // 0 to 65535
                        auxManager->simulate_virtual_input(9, rawVal);
                    }
                    if (ImGui::SliderInt("Axis Y (Input #10)", &virtSlider2, 0, 100))
                    {
                        std::uint16_t rawVal = static_cast<std::uint16_t>(virtSlider2 * 655);
                        auxManager->simulate_virtual_input(10, rawVal);
                    }

                    ImGui::EndTabItem();
                }

                // Tab 3: Discovered Devices
                if (ImGui::BeginTabItem("Discovered Devices"))
                {
                    auto devs = auxManager->get_input_devices();
                    ImGui::Text("Auxiliary Input Devices discovered via Maintenance Message (0x23):");

                    if (ImGui::BeginTable("DevTable", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
                    {
                        ImGui::TableSetupColumn("Device Description");
                        ImGui::TableSetupColumn("CAN Address");
                        ImGui::TableSetupColumn("64-bit NAME");
                        ImGui::TableSetupColumn("Model Code");
                        ImGui::TableSetupColumn("Status");
                        ImGui::TableHeadersRow();

                        for (const auto &dev : devs)
                        {
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn();
                            ImGui::Text("%s", dev.nameString.c_str());
                            ImGui::TableNextColumn();
                            ImGui::Text("0x%02X", dev.address);
                            ImGui::TableNextColumn();
                            ImGui::Text("0x%016llX", dev.rawName);
                            ImGui::TableNextColumn();
                            ImGui::Text("%u", dev.modelCode);
                            ImGui::TableNextColumn();
                            ImGui::TextColored(dev.ready ? ImVec4(0.2f, 1.0f, 0.3f, 1.0f) : ImVec4(1.0f, 0.4f, 0.2f, 1.0f),
                                               dev.ready ? "Ready" : "Not Ready");
                        }
                        ImGui::EndTable();
                    }
                    ImGui::EndTabItem();
                }

                ImGui::EndTabBar();
            }
            ImGui::End();
        }

        // 4. CAN Bus Monitor Window
        if (showCanLogWindow && !g_isHudMode)
        {
            if (g_TriggerResetLayout)
            {
                ImGui::SetNextWindowPos(ImVec2(rightColX, 404.0f), ImGuiCond_Always);
                ImGui::SetNextWindowSize(ImVec2(rightColW, std::max(300.0f, vpH - 414.0f)), ImGuiCond_Always);
                ImGui::SetNextWindowCollapsed(false, ImGuiCond_Always);
            }
            else
            {
                ImGui::SetNextWindowPos(ImVec2(rightColX, 404.0f), ImGuiCond_FirstUseEver);
                ImGui::SetNextWindowSize(ImVec2(rightColW, 480.0f), ImGuiCond_FirstUseEver);
                ImGui::SetNextWindowCollapsed(false, ImGuiCond_Appearing);
            }
            ImGui::Begin("CAN Bus Monitor & Diagnostics", &showCanLogWindow, ImGuiWindowFlags_NoCollapse);
            bool isPaused = canManager->is_log_paused();
            if (ImGui::Button(isPaused ? "Resume" : "Pause"))
            {
                canManager->set_log_paused(!isPaused);
            }
            ImGui::SameLine();
            if (ImGui::Button("Clear"))
            {
                canManager->clear_log();
            }

            ImGui::SameLine();
            ImGui::Text("Total RX: %llu | Total TX: %llu", canManager->get_rx_count(), canManager->get_tx_count());

            ImGui::SameLine();
            ImGui::Text("| Bus Load: %.1f%% (250k)", canManager->get_bus_load());

            auto loadHistory = canManager->get_bus_load_history();
            if (!loadHistory.empty())
            {
                ImGui::PlotLines("##BusLoadSparkline", loadHistory.data(), static_cast<int>(loadHistory.size()), 0, nullptr, 0.0f, 100.0f, ImVec2(0, 35));
            }

            auto frames = canManager->get_recent_frames(150);

            if (ImGui::BeginTable("CanTable", 7, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, 220)))
            {
                ImGui::TableSetupColumn("Time (ms)", ImGuiTableColumnFlags_WidthFixed, 75.0f);
                ImGui::TableSetupColumn("Dir", ImGuiTableColumnFlags_WidthFixed, 40.0f);
                ImGui::TableSetupColumn("PGN", ImGuiTableColumnFlags_WidthFixed, 65.0f);
                ImGui::TableSetupColumn("PGN Name", ImGuiTableColumnFlags_WidthFixed, 140.0f);
                ImGui::TableSetupColumn("Src", ImGuiTableColumnFlags_WidthFixed, 40.0f);
                ImGui::TableSetupColumn("Dst", ImGuiTableColumnFlags_WidthFixed, 40.0f);
                ImGui::TableSetupColumn("Data (HEX)");
                ImGui::TableHeadersRow();

                for (const auto &f : frames)
                {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("%u", f.timestampMs % 100000);

                    ImGui::TableNextColumn();
                    if (f.isTx)
                        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "TX");
                    else
                        ImGui::TextColored(ImVec4(0.4f, 0.7f, 1.0f, 1.0f), "RX");

                    ImGui::TableNextColumn();
                    ImGui::Text("0x%04X", f.pgn);

                    ImGui::TableNextColumn();
                    ImGui::Text("%s", f.pgnName.c_str());

                    ImGui::TableNextColumn();
                    ImGui::Text("0x%02X", f.source);

                    ImGui::TableNextColumn();
                    ImGui::Text("0x%02X", f.dest);

                    ImGui::TableNextColumn();
                    std::ostringstream ss;
                    for (int d = 0; d < f.dlc; ++d)
                    {
                        ss << std::hex << std::uppercase << std::setw(2) << std::setfill('0') << static_cast<int>(f.data[d]) << " ";
                    }
                    ImGui::Text("%s", ss.str().c_str());
                }
                ImGui::EndTable();
            }
            ImGui::End();
        }

        // 6. AgOpenGPS Task Controller (TC-BAS & TC-SC) | Ewidencja ŚOR 2027
        if (showAogTcWindow && !g_isHudMode)
        {
            if (g_TriggerResetLayout)
            {
                ImGui::SetNextWindowPos(ImVec2(rightColX, 36.0f), ImGuiCond_Always);
                ImGui::SetNextWindowSize(ImVec2(std::max(rightColW, 520.0f), 560.0f), ImGuiCond_Always);
                ImGui::SetNextWindowCollapsed(false, ImGuiCond_Always);
            }
            else
            {
                ImGui::SetNextWindowPos(ImVec2(rightColX, 36.0f), ImGuiCond_FirstUseEver);
                ImGui::SetNextWindowSize(ImVec2(std::max(rightColW, 520.0f), 560.0f), ImGuiCond_FirstUseEver);
                ImGui::SetNextWindowCollapsed(false, ImGuiCond_Appearing);
            }

            ImGui::Begin("Task Controller (TC-BAS & SC) | Ewidencja ŚOR 2027###AogTcWindow", &showAogTcWindow, ImGuiWindowFlags_NoCollapse);

            auto &tcBas = isobus_vt::TcBasReportManager::get_instance();
            auto &curTask = tcBas.get_current_task();
            auto &profile = tcBas.get_farm_profile();
            auto &catalog = isobus_vt::SorCatalog::get_instance();
            bool aogActive = tcManager->is_aog_connected();

            // Status bar header
            if (aogActive)
            {
                ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "[ AGOPENGPS POŁĄCZONY ]");
            }
            else
            {
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "[ OCZEKIWANIE NA AGOPENGPS (UDP 8888) ]");
            }
            ImGui::SameLine();
            if (curTask.isRunning)
            {
                if (curTask.isPaused)
                    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "| [ PAUZA ]");
                else
                    ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "| [ REJESTRACJA W TOKU ]");
            }
            else
            {
                ImGui::TextDisabled("| [ BRAK AKTYWNEGO ZABIEGU ]");
            }

            ImGui::SameLine();
            ImGui::TextDisabled("| Baza ŚOR: %zu preparatów", catalog.count());

            ImGui::Separator();

            // Direct AgOpenGPS Sync Banner
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.35f, 0.9f, 0.95f, 1.0f));
            ImGui::Text(">> %s", tcBas.get_last_aog_sync_info().c_str());
            ImGui::PopStyleColor();
            ImGui::SameLine();
            if (ImGui::SmallButton("Odśwież z AgOpenGPS (Profil/Pole)"))
            {
                tcBas.scan_and_sync_agopengps();
                if (tcManager && tcManager->get_tc_server())
                {
                    int aogSec = curTask.implementSectionsCount;
                    if (aogSec > 0) tcManager->get_tc_server()->set_fallback_sections_count(static_cast<std::uint8_t>(aogSec));
                }
                isobus_vt::TerminalSettings::get_instance().play_touch_beep();
            }

            ImGui::SameLine();
            bool isForce = tcManager->is_force_paint();
            if (isForce)
            {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.1f, 0.7f, 0.2f, 1.0f));
                if (ImGui::SmallButton("✔ WYMUŚ MALOWANIE: WŁĄCZONE (AgOpenGPS)"))
                {
                    tcManager->set_force_paint(false);
                    isobus_vt::TerminalSettings::get_instance().play_touch_beep();
                }
                ImGui::PopStyleColor();
            }
            else
            {
                if (ImGui::SmallButton("⚡ Wymuś malowanie w AgOpenGPS (Manual ON)"))
                {
                    tcManager->set_force_paint(true);
                    isobus_vt::TerminalSettings::get_instance().play_touch_beep();
                }
            }

            ImGui::SameLine();
            bool workActive = isobus_vt::TractorEcuManager::get_instance().is_work_active() || isForce;
            if (isForce)
            {
                ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.3f, 1.0f), "| [MALOWANIE: WYMUSZONE]");
            }
            else if (workActive)
            {
                ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.3f, 1.0f), "| [MALOWANIE: AKTYWNE (TUZ/WOM)]");
            }
            else
            {
                ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "| [MALOWANIE: TRYB AUTO (AOG SC)]");
            }

            // Podgląd aktywnych sekcji belki opryskiwacza
            auto secStates = tcManager->get_section_states();
            if (!secStates.empty())
            {
                ImGui::SameLine();
                ImGui::TextDisabled("| Sekcje:");
                ImGui::SameLine();
                for (size_t s = 0; s < secStates.size() && s < 16; ++s)
                {
                    bool on = secStates[s] || workActive;
                    ImVec4 col = on ? ImVec4(0.15f, 0.90f, 0.35f, 1.0f) : ImVec4(0.35f, 0.35f, 0.40f, 1.0f);
                    ImGui::TextColored(col, "[%zu]", s + 1);
                    if (s + 1 < secStates.size() && s < 15) ImGui::SameLine(0, 2.0f);
                }
            }

            ImGui::Separator();

            if (!cfg.tcEnabled)
            {
                ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.20f, 0.12f, 0.08f, 0.70f));
                ImGui::BeginChild("TcDisabledBanner", ImVec2(0, 68), true);
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "Modul Task Controller (TC-BAS & TC-SC) jest obecnie WYLACZONY w konfiguracji.");
                ImGui::TextDisabled("Aplikacja nie zajmuje adresu ISOBUS na magistrali CAN ani nie odbiera telemetrii AgOpenGPS (port 8888).");
                if (ImGui::Button(" Wlacz Task Controller teraz "))
                {
                    auto &mutCfg = isobus_vt::TerminalSettings::get_instance().get_config();
                    mutCfg.tcEnabled = true;
                    if (tcManager && !tcManager->is_initialized())
                    {
                        tcManager->initialize(0);
                    }
                    isobus_vt::TerminalSettings::get_instance().save();
                    isobus_vt::TerminalSettings::get_instance().play_touch_beep();
                }
                ImGui::EndChild();
                ImGui::PopStyleColor();
                ImGui::Spacing();
            }

            if (ImGui::BeginTabBar("TcBasTabs", ImGuiTabBarFlags_None))
            {
                // ── TAB 1: TC-BAS & ISO-XML (TASKDATA) ─────────────────────────
                ImGuiTabItemFlags tFlags0 = (tcActiveTabTarget == 0) ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
                if (ImGui::BeginTabItem("TC-BAS & ISO-XML", nullptr, tFlags0))
                {
                    ImGui::TextColored(ImVec4(0.3f, 0.85f, 1.0f, 1.0f), "ISO 11783-10 Task Controller Basic (TC-BAS) - Rejestracja Pracy Polowej");
                    ImGui::Separator();

                    // Parametry bieżącego zadania
                    char tNameBuf[64];
                    strncpy_s(tNameBuf, curTask.taskName.c_str(), sizeof(tNameBuf));
                    ImGui::SetNextItemWidth(200.0f);
                    if (ImGui::InputText("Nazwa zadania##tn", tNameBuf, sizeof(tNameBuf)))
                        curTask.taskName = tNameBuf;

                    ImGui::SameLine();
                    char fNameBuf[64];
                    strncpy_s(fNameBuf, curTask.fieldName.c_str(), sizeof(fNameBuf));
                    ImGui::SetNextItemWidth(200.0f);
                    if (ImGui::InputText("Pole / Działka##fn", fNameBuf, sizeof(fNameBuf)))
                    {
                        curTask.fieldName = fNameBuf;
                        curTask.sorData.fieldName = fNameBuf;
                    }

                    // Szybki wybór działki z bazy profilu
                    if (!profile.parcels.empty())
                    {
                        ImGui::SameLine();
                        if (ImGui::BeginCombo("##quickParcel", "Wybierz z profilu..."))
                        {
                            for (size_t i = 0; i < profile.parcels.size(); ++i)
                            {
                                const auto &p = profile.parcels[i];
                                std::string lbl = p.name + " (" + p.parcelNumber + " - " + std::to_string((int)p.areaHa) + " ha)";
                                if (ImGui::Selectable(lbl.c_str()))
                                {
                                    tcBas.select_parcel_for_task(i);
                                    isobus_vt::TerminalSettings::get_instance().play_touch_beep();
                                }
                            }
                            ImGui::EndCombo();
                        }
                    }

                    float curWidth = curTask.implementWidthMeters;
                    ImGui::SetNextItemWidth(120.0f);
                    if (ImGui::InputFloat("Szerokość robocza [m]##iw", &curWidth, 0.5f, 1.0f, "%.1f m"))
                    {
                        if (curWidth > 0.5f) curTask.implementWidthMeters = curWidth;
                    }

                    ImGui::Spacing();

                    // Przyciski sterowania zadaniem
                    if (!curTask.isRunning)
                    {
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.65f, 0.25f, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.80f, 0.35f, 1.0f));
                        if (ImGui::Button(" ▶ ROZPOCZNIJ NOWE ZADANIE / ZABIEG ", ImVec2(280.0f, 32.0f)))
                        {
                            tcBas.start_task(curTask.taskName, curTask.fieldName, curTask.implementWidthMeters);
                            isobus_vt::TerminalSettings::get_instance().play_touch_beep();
                        }
                        ImGui::PopStyleColor(2);
                    }
                    else
                    {
                        if (!curTask.isPaused)
                        {
                            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.60f, 0.15f, 1.0f));
                            if (ImGui::Button(" ⏸ WSTRZYMAJ (PAUZA) ", ImVec2(160.0f, 32.0f)))
                            {
                                tcBas.pause_task();
                                isobus_vt::TerminalSettings::get_instance().play_touch_beep();
                            }
                            ImGui::PopStyleColor();
                        }
                        else
                        {
                            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.70f, 0.30f, 1.0f));
                            if (ImGui::Button(" ▶ WZNÓW PRACĘ ", ImVec2(160.0f, 32.0f)))
                            {
                                tcBas.resume_task();
                                isobus_vt::TerminalSettings::get_instance().play_touch_beep();
                            }
                            ImGui::PopStyleColor();
                        }

                        ImGui::SameLine();
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.20f, 0.18f, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.90f, 0.25f, 0.22f, 1.0f));
                        if (ImGui::Button(" ⏹ ZAKOŃCZ I ZAPISZ ZADANIE ", ImVec2(200.0f, 32.0f)))
                        {
                            tcBas.stop_task();
                            isobus_vt::TerminalSettings::get_instance().play_touch_beep();
                        }
                        ImGui::PopStyleColor(2);
                    }

                    ImGui::Spacing();
                    ImGui::Separator();

                    // Duże liczniki pracy (Totals)
                    ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.9f, 1.0f), "Liczniki Zadania (ISO-XML Totals na żywo):");
                    ImGui::Columns(4, "TotalsCols", false);

                    ImGui::TextDisabled("POWIERZCHNIA");
                    ImGui::TextColored(ImVec4(0.25f, 0.95f, 0.45f, 1.0f), "%.3f ha", curTask.workedAreaHa);
                    ImGui::NextColumn();

                    ImGui::TextDisabled("CZAS PRACY");
                    std::string timeStr = isobus_vt::TcBasReportManager::format_time_hhmmss(curTask.elapsedWorkSeconds);
                    ImGui::TextColored(ImVec4(0.35f, 0.85f, 1.0f, 1.0f), "%s", timeStr.c_str());
                    ImGui::NextColumn();

                    ImGui::TextDisabled("DYSTANS");
                    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "%.2f km", curTask.workedDistanceKm);
                    ImGui::NextColumn();

                    ImGui::TextDisabled("ŚR. PRĘDKOŚĆ");
                    ImGui::TextColored(ImVec4(0.85f, 0.85f, 0.85f, 1.0f), "%.1f km/h", curTask.avgSpeedKph);
                    ImGui::Columns(1);

                    ImGui::Separator();
                    ImGui::Spacing();

                    // Ramka eksportu ISO-XML (TASKDATA.XML)
                    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Eksport Danych ISO-XML (ISO 11783-10: TASKDATA.XML):");
                    ImGui::TextDisabled("Zgodny z systemami FMIS (365FarmNet, John Deere Operations Center, Claas Telematics):");

                    static std::string xmlExportStatus = "";
                    static float xmlStatusTimer = 0.0f;
                    if (xmlStatusTimer > 0.0f) xmlStatusTimer -= ImGui::GetIO().DeltaTime;

                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.35f, 0.55f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.45f, 0.70f, 1.0f));
                    if (ImGui::Button(" 💾 Eksportuj do ISO-XML (TASKDATA/TASKDATA.XML) ", ImVec2(340.0f, 30.0f)))
                    {
                        std::string outPath;
                        if (tcBas.export_taskdata_xml("TASKDATA", &outPath))
                        {
                            xmlExportStatus = "Wygenerowano pomyślnie: " + outPath;
                            xmlStatusTimer = 6.0f;
                            isobus_vt::TerminalSettings::get_instance().play_touch_beep();
                        }
                    }
                    ImGui::PopStyleColor(2);

                    ImGui::SameLine();
                    if (ImGui::Button(" 📁 Otwórz Folder TASKDATA ", ImVec2(200.0f, 30.0f)))
                    {
                        ShellExecuteA(NULL, "open", "TASKDATA", NULL, NULL, SW_SHOWNORMAL);
                    }

                    if (xmlStatusTimer > 0.0f && !xmlExportStatus.empty())
                    {
                        ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "✓ %s", xmlExportStatus.c_str());
                    }

                    ImGui::EndTabItem();
                }

                // ── TAB 2: EWIDENCJA ŚOR 2027 ──────────────────────────────────
                ImGuiTabItemFlags tFlags1 = (tcActiveTabTarget == 1) ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
                if (ImGui::BeginTabItem("Ewidencja ÅOR 2027", nullptr, tFlags1))
                {
                    auto &sor = curTask.sorData;

                    ImGui::TextColored(ImVec4(0.35f, 0.90f, 0.50f, 1.0f), "Elektroniczny Rejestr Zabiegów Środkami Ochrony Roślin (od 2026/2027 r.)");
                    ImGui::TextDisabled("Wymóg prawny: Rozporządzenie (WE) nr 1107/2009 (art. 67) & przepisy krajowe PIORiN");
                    ImGui::Separator();

                    // Górny pasek szybkiego wyboru działki z profilu
                    if (!profile.parcels.empty())
                    {
                        ImGui::Text("Szybki wybór działki z profilu:");
                        ImGui::SameLine();
                        for (size_t i = 0; i < profile.parcels.size() && i < 4; ++i)
                        {
                            if (i > 0) ImGui::SameLine();
                            const auto &p = profile.parcels[i];
                            std::string bLabel = p.name + " (" + p.parcelNumber + ")";
                            if (ImGui::SmallButton(bLabel.c_str()))
                            {
                                tcBas.select_parcel_for_task(i);
                                isobus_vt::TerminalSettings::get_instance().play_touch_beep();
                            }
                        }
                    }

                    ImGui::Spacing();

                    // Formularz zabiegu w dwóch kolumnach
                    ImGui::Columns(2, "SorFormCols", true);

                    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "1. Lokalizacja i Uprawa:");

                    char bFarm[64]; strncpy_s(bFarm, sor.farmName.c_str(), sizeof(bFarm));
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::InputTextWithHint("##farm", "Gospodarstwo / Podmiot", bFarm, sizeof(bFarm))) sor.farmName = bFarm;

                    char bField[64]; strncpy_s(bField, sor.fieldName.c_str(), sizeof(bField));
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::InputTextWithHint("##fld", "Nazwa pola / Działki rolnej", bField, sizeof(bField)))
                    {
                        sor.fieldName = bField;
                        curTask.fieldName = bField;
                    }

                    char bParcel[32]; strncpy_s(bParcel, sor.cadastralParcel.c_str(), sizeof(bParcel));
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::InputTextWithHint("##parc", "Nr działki ewidencyjnej (np. 142/3)", bParcel, sizeof(bParcel))) sor.cadastralParcel = bParcel;

                    char bPrecinct[32]; strncpy_s(bPrecinct, sor.cadastralPrecinct.c_str(), sizeof(bPrecinct));
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::InputTextWithHint("##prec", "Obręb ewidencyjny", bPrecinct, sizeof(bPrecinct))) sor.cadastralPrecinct = bPrecinct;

                    char bCommune[32]; strncpy_s(bCommune, sor.commune.c_str(), sizeof(bCommune));
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::InputTextWithHint("##comm", "Gmina / Powiat", bCommune, sizeof(bCommune))) sor.commune = bCommune;

                    char bCrop[64]; strncpy_s(bCrop, sor.cropName.c_str(), sizeof(bCrop));
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::InputTextWithHint("##crop", "Roślina uprawna (np. Pszenica ozima)", bCrop, sizeof(bCrop))) sor.cropName = bCrop;

                    char bBbch[64]; strncpy_s(bBbch, sor.bbchStage.c_str(), sizeof(bBbch));
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::InputTextWithHint("##bbch", "Faza rozwojowa (np. BBCH 31)", bBbch, sizeof(bBbch))) sor.bbchStage = bBbch;

                    ImGui::NextColumn();

                    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "2. Preparat (ŚOR) i Warunki:");

                    char bChem[64]; strncpy_s(bChem, sor.chemicalName.c_str(), sizeof(bChem));
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::InputTextWithHint("##chem", "Nazwa handlowa preparatu", bChem, sizeof(bChem))) sor.chemicalName = bChem;

                    char bPermit[32]; strncpy_s(bPermit, sor.permitNumber.c_str(), sizeof(bPermit));
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::InputTextWithHint("##perm", "Nr zezwolenia MRiRW", bPermit, sizeof(bPermit))) sor.permitNumber = bPermit;

                    ImGui::SetNextItemWidth(120.0f);
                    ImGui::InputFloat("Dawka [l/ha]##dose", &sor.dosePerHa, 0.1f, 0.5f, "%.2f");
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(120.0f);
                    ImGui::InputFloat("Ciecz [l/ha]##wtr", &sor.waterPerHa, 10.0f, 50.0f, "%.0f");

                    char bPest[96]; strncpy_s(bPest, sor.targetPest.c_str(), sizeof(bPest));
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::InputTextWithHint("##pest", "Zwalczany agrofag (przyczyna zabiegu)", bPest, sizeof(bPest))) sor.targetPest = bPest;

                    ImGui::SetNextItemWidth(120.0f);
                    ImGui::InputFloat("Temp [°C]##tmp", &sor.temperatureC, 0.5f, 1.0f, "%.1f °C");
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(120.0f);
                    ImGui::InputFloat("Wiatr [m/s]##wnd", &sor.windSpeedMs, 0.1f, 0.5f, "%.1f m/s");

                    char bOper[64]; strncpy_s(bOper, sor.operatorName.c_str(), sizeof(bOper));
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::InputTextWithHint("##oper", "Osoba wykonująca zabieg", bOper, sizeof(bOper))) sor.operatorName = bOper;

                    char bCert[48]; strncpy_s(bCert, sor.operatorCertificateNo.c_str(), sizeof(bCert));
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::InputTextWithHint("##cert", "Nr zaświadczenia szkolenia ŚOR", bCert, sizeof(bCert))) sor.operatorCertificateNo = bCert;

                    char bInsp[48]; strncpy_s(bInsp, sor.sprayerInspectionNo.c_str(), sizeof(bInsp));
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::InputTextWithHint("##insp", "Atest opryskiwacza (nr badania)", bInsp, sizeof(bInsp))) sor.sprayerInspectionNo = bInsp;

                    ImGui::Columns(1);
                    ImGui::Separator();

                    // Podsumowanie bieżącego bilansu zabiegu
                    float curArea = curTask.workedAreaHa;
                    float curChem = curArea * sor.dosePerHa;
                    ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.9f, 1.0f),
                                       "Bilans Zabiegu (Zliczenie z GPS / Task Controllera): Powierzchnia: ");
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "%.2f ha", curArea);
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.9f, 1.0f), "| Zużycie środka: ");
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%.2f %s", curChem, sor.doseUnit.c_str());

                    ImGui::Spacing();

                    // Przyciski generowania oficjalnej karty i rejestru
                    static std::string htmlExportStatus = "";
                    static float htmlStatusTimer = 0.0f;
                    if (htmlStatusTimer > 0.0f) htmlStatusTimer -= ImGui::GetIO().DeltaTime;

                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.55f, 0.28f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.24f, 0.70f, 0.35f, 1.0f));
                    if (ImGui::Button(" 🖨️ Generuj Kartę Zabiegu ŚOR (Druk A4 / HTML do kontroli PIORiN) ", ImVec2(410.0f, 32.0f)))
                    {
                        sor.appliedAreaHa = curArea;
                        sor.totalChemicalUsed = curChem;
                        std::string outPath;
                        if (tcBas.export_sor_html_report(sor, "Raporty_SOR", &outPath))
                        {
                            htmlExportStatus = "Wygenerowano kartę: " + outPath;
                            htmlStatusTimer = 6.0f;
                            isobus_vt::TerminalSettings::get_instance().play_touch_beep();
                            ShellExecuteA(NULL, "open", outPath.c_str(), NULL, NULL, SW_SHOWNORMAL);
                        }
                    }
                    ImGui::PopStyleColor(2);

                    ImGui::SameLine();
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.38f, 0.60f, 1.0f));
                    if (ImGui::Button(" 📊 Eksportuj Rejestr do Excela (CSV) ", ImVec2(240.0f, 32.0f)))
                    {
                        std::string outCsv;
                        if (tcBas.export_sor_csv_register("Raporty_SOR", &outCsv))
                        {
                            htmlExportStatus = "Wyeksportowano rejestr CSV: " + outCsv;
                            htmlStatusTimer = 6.0f;
                            isobus_vt::TerminalSettings::get_instance().play_touch_beep();
                            ShellExecuteA(NULL, "open", outCsv.c_str(), NULL, NULL, SW_SHOWNORMAL);
                        }
                    }
                    ImGui::PopStyleColor();

                    ImGui::SameLine();
                    if (ImGui::Button(" 📁 Folder Raportów ", ImVec2(140.0f, 32.0f)))
                    {
                        ShellExecuteA(NULL, "open", "Raporty_SOR", NULL, NULL, SW_SHOWNORMAL);
                    }

                    if (htmlStatusTimer > 0.0f && !htmlExportStatus.empty())
                    {
                        ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "✓ %s", htmlExportStatus.c_str());
                    }

                    ImGui::EndTabItem();
                }

                // ── TAB 3: KATALOG ŚRODKÓW MRiRW (2953 PREPARATY) ──────────────
                ImGuiTabItemFlags tFlags2 = (tcActiveTabTarget == 2) ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
                if (ImGui::BeginTabItem("Katalog ÅrodkÃ³w MRiRW", nullptr, tFlags2))
                {
                    ImGui::TextColored(ImVec4(0.35f, 0.85f, 1.0f, 1.0f), "Oficjalna Baza Środków Ochrony Roślin MRiRW (2953 preparaty)");
                    ImGui::TextDisabled("Wyszukaj środek i kliknij 'Wybierz do zabiegu', aby automatycznie przenieść dane do ewidencji.");
                    ImGui::Separator();

                    static char searchBuf[96] = "";
                    static int selectedKindIdx = 0;
                    const char* kindOptions[] = {
                        "Wszystkie rodzaje",
                        "Chwastobójcze (Herbicydy)",
                        "Grzybobójcze (Fungicydy)",
                        "Owadowbójcze (Insektycydy)",
                        "Regulatory wzrostu",
                        "Moluskocydy (ślimakobójcze)",
                        "Akarycydy (przędziorkobójcze)"
                    };
                    const char* kindFilterTags[] = {
                        "",
                        "Chwast",
                        "Fungicyd",
                        "Insektycyd",
                        "Regulator",
                        "Moluskocyd",
                        "Akarycyd"
                    };

                    ImGui::SetNextItemWidth(240.0f);
                    ImGui::InputTextWithHint("##sorSearch", "Szukaj: nazwa, substancja, zezwolenie...", searchBuf, sizeof(searchBuf));

                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(220.0f);
                    ImGui::Combo("Filtr rodzaju", &selectedKindIdx, kindOptions, 7);

                    std::string q = searchBuf;
                    std::string kFilter = kindFilterTags[selectedKindIdx];
                    auto results = catalog.search(q, kFilter, 60);

                    ImGui::TextDisabled("Znaleziono: %zu wyników (wyświetlono maks. 60)", results.size());
                    ImGui::Spacing();

                    if (ImGui::BeginTable("CatalogTable", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, 320)))
                    {
                        ImGui::TableSetupColumn("Nazwa handlowa", ImGuiTableColumnFlags_WidthFixed, 180.0f);
                        ImGui::TableSetupColumn("Nr zezwolenia", ImGuiTableColumnFlags_WidthFixed, 110.0f);
                        ImGui::TableSetupColumn("Rodzaj", ImGuiTableColumnFlags_WidthFixed, 120.0f);
                        ImGui::TableSetupColumn("Substancja czynna");
                        ImGui::TableSetupColumn("Producent", ImGuiTableColumnFlags_WidthFixed, 160.0f);
                        ImGui::TableSetupColumn("Akcja", ImGuiTableColumnFlags_WidthFixed, 130.0f);
                        ImGui::TableHeadersRow();

                        for (size_t i = 0; i < results.size(); ++i)
                        {
                            const auto *p = results[i];
                            ImGui::TableNextRow();

                            ImGui::TableNextColumn();
                            ImGui::TextColored(ImVec4(0.25f, 0.90f, 0.45f, 1.0f), "%s", p->name.c_str());

                            ImGui::TableNextColumn();
                            ImGui::Text("%s", p->permit.c_str());

                            ImGui::TableNextColumn();
                            ImGui::Text("%s", p->kind.c_str());

                            ImGui::TableNextColumn();
                            ImGui::TextWrapped("%s", p->substance.c_str());

                            ImGui::TableNextColumn();
                            ImGui::Text("%s", p->producer.c_str());

                            ImGui::TableNextColumn();
                            ImGui::PushID(static_cast<int>(i));
                            if (ImGui::Button("Wybierz do zabiegu"))
                            {
                                tcBas.select_chemical_from_catalog(*p);
                                isobus_vt::TerminalSettings::get_instance().play_touch_beep();
                            }
                            ImGui::PopID();
                        }
                        ImGui::EndTable();
                    }

                    ImGui::EndTabItem();
                }

                // ── TAB 4: PROFIL GOSPODARSTWA I ATESTY ────────────────────────
                ImGuiTabItemFlags tFlags3 = (tcActiveTabTarget == 3) ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
                if (ImGui::BeginTabItem("Profil Gospodarstwa i Atesty", nullptr, tFlags3))
                {
                    ImGui::TextColored(ImVec4(0.4f, 0.85f, 1.0f, 1.0f), "Trwały Profil Gospodarstwa, Sprzętu (Atesty) i Uprawnień ŚOR");
                    ImGui::TextDisabled("Dane zapisywane w pliku profil_gospodarstwa.json - uzupełniają się automatycznie przy każdym zabiegu.");
                    ImGui::Separator();

                    ImGui::Columns(2, "ProfileCols", true);

                    // Kolumna 1: Gospodarstwo i Operator
                    ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.5f, 1.0f), "1. DANE GOSPODARSTWA:");
                    char bFName[96]; strncpy_s(bFName, profile.farmName.c_str(), sizeof(bFName));
                    if (ImGui::InputText("Gospodarstwo / Podmiot##pfFarm", bFName, sizeof(bFName))) profile.farmName = bFName;

                    char bFAddr[128]; strncpy_s(bFAddr, profile.farmAddress.c_str(), sizeof(bFAddr));
                    if (ImGui::InputText("Siedziba / Adres##pfAddr", bFAddr, sizeof(bFAddr))) profile.farmAddress = bFAddr;

                    char bFArimr[32]; strncpy_s(bFArimr, profile.producerNumberArimr.c_str(), sizeof(bFArimr));
                    if (ImGui::InputText("Nr producenta ARiMR##pfArimr", bFArimr, sizeof(bFArimr))) profile.producerNumberArimr = bFArimr;

                    char bFComm[64]; strncpy_s(bFComm, profile.commune.c_str(), sizeof(bFComm));
                    if (ImGui::InputText("Gmina / Powiat##pfComm", bFComm, sizeof(bFComm))) profile.commune = bFComm;

                    char bFPrec[48]; strncpy_s(bFPrec, profile.defaultPrecinct.c_str(), sizeof(bFPrec));
                    if (ImGui::InputText("Domyślny obręb##pfPrec", bFPrec, sizeof(bFPrec))) profile.defaultPrecinct = bFPrec;

                    ImGui::Spacing();
                    ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.5f, 1.0f), "2. OPERATOR I SZKOLENIE ŚOR:");
                    char bOpName[64]; strncpy_s(bOpName, profile.operatorName.c_str(), sizeof(bOpName));
                    if (ImGui::InputText("Imię i nazwisko##pfOp", bOpName, sizeof(bOpName))) profile.operatorName = bOpName;

                    char bOpCert[48]; strncpy_s(bOpCert, profile.operatorCertificateNo.c_str(), sizeof(bOpCert));
                    if (ImGui::InputText("Nr zaświadczenia ŚOR##pfCert", bOpCert, sizeof(bOpCert))) profile.operatorCertificateNo = bOpCert;

                    char bOpValid[32]; strncpy_s(bOpValid, profile.operatorCertificateValidUntil.c_str(), sizeof(bOpValid));
                    if (ImGui::InputText("Ważność zaświadczenia##pfOpVal", bOpValid, sizeof(bOpValid))) profile.operatorCertificateValidUntil = bOpValid;

                    ImGui::NextColumn();

                    // Kolumna 2: Opryskiwacz i Atest
                    ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.5f, 1.0f), "3. OPRYSKIWACZ I ATEST TECHNICZNY:");
                    char bSpModel[80]; strncpy_s(bSpModel, profile.sprayerModel.c_str(), sizeof(bSpModel));
                    if (ImGui::InputText("Model opryskiwacza##pfSpMod", bSpModel, sizeof(bSpModel))) profile.sprayerModel = bSpModel;

                    char bSpSerial[48]; strncpy_s(bSpSerial, profile.sprayerSerialNo.c_str(), sizeof(bSpSerial));
                    if (ImGui::InputText("Nr fabryczny / VIN##pfSpSer", bSpSerial, sizeof(bSpSerial))) profile.sprayerSerialNo = bSpSerial;

                    char bSpInsp[48]; strncpy_s(bSpInsp, profile.sprayerInspectionNo.c_str(), sizeof(bSpInsp));
                    if (ImGui::InputText("Nr badania (Atest)##pfSpInsp", bSpInsp, sizeof(bSpInsp))) profile.sprayerInspectionNo = bSpInsp;

                    char bSpInspDate[32]; strncpy_s(bSpInspDate, profile.sprayerInspectionDate.c_str(), sizeof(bSpInspDate));
                    if (ImGui::InputText("Data badania##pfSpInspD", bSpInspDate, sizeof(bSpInspDate))) profile.sprayerInspectionDate = bSpInspDate;

                    char bSpInspVal[32]; strncpy_s(bSpInspVal, profile.sprayerInspectionValidUntil.c_str(), sizeof(bSpInspVal));
                    if (ImGui::InputText("Ważność atestu##pfSpInspVal", bSpInspVal, sizeof(bSpInspVal))) profile.sprayerInspectionValidUntil = bSpInspVal;

                    if (ImGui::InputFloat("Szerokość belki [m]##pfSpWidth", &profile.sprayerBoomWidthM, 0.5f, 1.0f, "%.1f m"))
                    {
                        if (profile.sprayerBoomWidthM > 0.5f) curTask.implementWidthMeters = profile.sprayerBoomWidthM;
                    }

                    ImGui::InputFloat("Pojemność zbiornika [l]##pfSpTank", &profile.sprayerTankVolumeL, 50.0f, 200.0f, "%.0f l");

                    char bSpNozzle[64]; strncpy_s(bSpNozzle, profile.nozzleType.c_str(), sizeof(bSpNozzle));
                    if (ImGui::InputText("Typ rozpylaczy##pfSpNoz", bSpNozzle, sizeof(bSpNozzle))) profile.nozzleType = bSpNozzle;

                    ImGui::Columns(1);
                    ImGui::Separator();

                    // Tabela zapisanych działek gospodarstwa
                    ImGui::TextColored(ImVec4(0.4f, 0.85f, 1.0f, 1.0f), "4. BAZA DZIAŁEK ROLNYCH GOSPODARSTWA (%zu działek):", profile.parcels.size());

                    static char newPName[48] = "";
                    static char newPNumber[32] = "";
                    static char newPPrecinct[32] = "";
                    static float newPArea = 10.0f;
                    static char newPCrop[48] = "";

                    ImGui::SetNextItemWidth(120.0f);
                    ImGui::InputTextWithHint("##npName", "Nazwa pola", newPName, sizeof(newPName));
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(100.0f);
                    ImGui::InputTextWithHint("##npNum", "Nr działki", newPNumber, sizeof(newPNumber));
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(100.0f);
                    ImGui::InputTextWithHint("##npPrec", "Obręb", newPPrecinct, sizeof(newPPrecinct));
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(80.0f);
                    ImGui::InputFloat("##npArea", &newPArea, 0.5f, 1.0f, "%.1f ha");
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(120.0f);
                    ImGui::InputTextWithHint("##npCrop", "Uprawa", newPCrop, sizeof(newPCrop));
                    ImGui::SameLine();
                    if (ImGui::Button(" ➕ Dodaj Działkę "))
                    {
                        if (strlen(newPName) > 0 || strlen(newPNumber) > 0)
                        {
                            isobus_vt::FarmParcel p;
                            p.name = strlen(newPName) > 0 ? newPName : "Pole " + std::string(newPNumber);
                            p.parcelNumber = newPNumber;
                            p.precinct = strlen(newPPrecinct) > 0 ? newPPrecinct : profile.defaultPrecinct;
                            p.commune = profile.commune;
                            p.areaHa = newPArea;
                            p.defaultCrop = newPCrop;
                            profile.parcels.push_back(p);
                            profile.save_to_file();
                            newPName[0] = '\0';
                            newPNumber[0] = '\0';
                            isobus_vt::TerminalSettings::get_instance().play_touch_beep();
                        }
                    }

                    if (ImGui::BeginTable("ParcelsTable", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, 160)))
                    {
                        ImGui::TableSetupColumn("Nazwa pola");
                        ImGui::TableSetupColumn("Nr działki ewidencyjnej", ImGuiTableColumnFlags_WidthFixed, 140.0f);
                        ImGui::TableSetupColumn("Obręb", ImGuiTableColumnFlags_WidthFixed, 100.0f);
                        ImGui::TableSetupColumn("Powierzchnia [ha]", ImGuiTableColumnFlags_WidthFixed, 100.0f);
                        ImGui::TableSetupColumn("Domyślna uprawa");
                        ImGui::TableSetupColumn("Akcje", ImGuiTableColumnFlags_WidthFixed, 140.0f);
                        ImGui::TableHeadersRow();

                        for (size_t i = 0; i < profile.parcels.size(); ++i)
                        {
                            const auto &p = profile.parcels[i];
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn();
                            ImGui::Text("%s", p.name.c_str());

                            ImGui::TableNextColumn();
                            ImGui::Text("%s", p.parcelNumber.c_str());

                            ImGui::TableNextColumn();
                            ImGui::Text("%s", p.precinct.c_str());

                            ImGui::TableNextColumn();
                            ImGui::Text("%.2f ha", p.areaHa);

                            ImGui::TableNextColumn();
                            ImGui::Text("%s", p.defaultCrop.c_str());

                            ImGui::TableNextColumn();
                            ImGui::PushID(static_cast<int>(i));
                            if (ImGui::SmallButton("Wczytaj"))
                            {
                                tcBas.select_parcel_for_task(i);
                                isobus_vt::TerminalSettings::get_instance().play_touch_beep();
                            }
                            ImGui::SameLine();
                            if (ImGui::SmallButton("Usuń"))
                            {
                                profile.parcels.erase(profile.parcels.begin() + i);
                                profile.save_to_file();
                                ImGui::PopID();
                                break;
                            }
                            ImGui::PopID();
                        }
                        ImGui::EndTable();
                    }

                    ImGui::Spacing();
                    static bool profileSavedNotice = false;
                    static float profileNoticeTimer = 0.0f;
                    if (profileNoticeTimer > 0.0f) profileNoticeTimer -= ImGui::GetIO().DeltaTime;

                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.55f, 0.28f, 1.0f));
                    if (ImGui::Button(" 💾 ZAPISZ PROFIL GOSPODARSTWA I ATESTY TRWALE ", ImVec2(360.0f, 32.0f)))
                    {
                        profile.save_to_file();
                        tcBas.apply_farm_profile_to_current();
                        profileSavedNotice = true;
                        profileNoticeTimer = 4.0f;
                        isobus_vt::TerminalSettings::get_instance().play_touch_beep();
                    }
                    ImGui::PopStyleColor();

                    if (profileNoticeTimer > 0.0f && profileSavedNotice)
                    {
                        ImGui::SameLine();
                        ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "✓ Zapisano profil do profil_gospodarstwa.json!");
                    }

                    ImGui::EndTabItem();
                }

                // ── TAB 5: SEKCJE I TC-SC (DOTYCHCZASOWY WIDOK) ────────────────
                ImGuiTabItemFlags tFlags4 = (tcActiveTabTarget == 4) ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
                if (ImGui::BeginTabItem("Sekcje i GPS (TC-SC)", nullptr, tFlags4))
                {
                    ImGui::Columns(3, "AogInfoCols", false);
                    ImGui::Text("Tryb TC: %s", tcManager->is_auto_mode() ? "AUTOMATYCZNY" : "MANUALNY");
                    ImGui::NextColumn();
                    ImGui::Text("Prędkość GPS: %.1f km/h", tcManager->get_speed_kph());
                    ImGui::NextColumn();
                    ImGui::Text("Jakość Fixa: %u", tcManager->get_udp_bridge() ? tcManager->get_udp_bridge()->get_gps_fix_quality() : 0);
                    ImGui::Columns(1);

                    ImGui::Separator();
                    std::uint8_t nSections = tcManager->get_num_sections();
                    if (nSections > 0)
                    {
                        auto setpoints = tcManager->get_section_setpoints();
                        auto actualStates = tcManager->get_section_states();

                        ImGui::Text("Sekcje Implementu (%u wykrytych na magistrali ISOBUS):", nSections);
                        for (std::uint8_t i = 0; i < nSections; ++i)
                        {
                            if (i > 0 && (i % 8 != 0)) ImGui::SameLine();
                            bool actOn = (i < actualStates.size()) && actualStates[i];
                            bool spOn  = (i < setpoints.size()) && setpoints[i];
                            bool on    = actOn || spOn;

                            ImVec4 col = on ? ImVec4(0.2f, 0.85f, 0.3f, 1.0f) : ImVec4(0.45f, 0.45f, 0.50f, 1.0f);
                            ImGui::TextColored(col, "[Sekcja %u: %s]", i + 1, on ? "ON" : "OFF");
                        }
                    }
                    else
                    {
                        ImGui::TextDisabled("Brak zarejestrowanych sekcji implementu (DDOP). Podłącz maszynę do magistrali CAN.");
                    }

                    ImGui::Separator();
                    ImGui::Text("Dziennik Aktywności Task Controllera:");
                    if (ImGui::BeginChild("TcLogChild", ImVec2(0, 110), true))
                    {
                        const auto &logs = tcManager->get_recent_logs();
                        for (const auto &line : logs)
                        {
                            ImGui::TextUnformatted(line.c_str());
                        }
                        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
                            ImGui::SetScrollHereY(1.0f);
                    }
                    ImGui::EndChild();
                    ImGui::EndTabItem();
                }

                // ── TAB 6: HISTORIA ZABIEGÓW ───────────────────────────────────
                ImGuiTabItemFlags tFlags5 = (tcActiveTabTarget == 5) ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
                if (ImGui::BeginTabItem("Historia ZadaÅ", nullptr, tFlags5))
                {
                    const auto &hist = tcBas.get_task_history();
                    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Archiwum Zakończonych Zadań i Zabiegów (%zu wpisów):", hist.size());
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Wyczyść Historię"))
                    {
                        tcBas.clear_history();
                    }

                    ImGui::Separator();

                    if (ImGui::BeginTable("TaskHistoryTable", 7, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, 220)))
                    {
                        ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 50.0f);
                        ImGui::TableSetupColumn("Data zabiegu", ImGuiTableColumnFlags_WidthFixed, 130.0f);
                        ImGui::TableSetupColumn("Pole / Działka");
                        ImGui::TableSetupColumn("Uprawa");
                        ImGui::TableSetupColumn("Środek ŚOR");
                        ImGui::TableSetupColumn("Obszar [ha]", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                        ImGui::TableSetupColumn("Akcja", ImGuiTableColumnFlags_WidthFixed, 90.0f);
                        ImGui::TableHeadersRow();

                        for (std::size_t i = 0; i < hist.size(); ++i)
                        {
                            const auto &entry = hist[i];
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn();
                            ImGui::Text("%s", entry.taskId.c_str());

                            ImGui::TableNextColumn();
                            ImGui::Text("%s", entry.sorData.startTimeIso.c_str());

                            ImGui::TableNextColumn();
                            ImGui::Text("%s (Dz. %s)", entry.fieldName.c_str(), entry.sorData.cadastralParcel.c_str());

                            ImGui::TableNextColumn();
                            ImGui::Text("%s", entry.sorData.cropName.c_str());

                            ImGui::TableNextColumn();
                            ImGui::Text("%s", entry.sorData.chemicalName.c_str());

                            ImGui::TableNextColumn();
                            ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.4f, 1.0f), "%.2f ha", entry.workedAreaHa);

                            ImGui::TableNextColumn();
                            ImGui::PushID(static_cast<int>(i));
                            if (ImGui::SmallButton("Karta ŚOR"))
                            {
                                std::string outPath;
                                if (tcBas.export_sor_html_report(entry.sorData, "Raporty_SOR", &outPath))
                                {
                                    ShellExecuteA(NULL, "open", outPath.c_str(), NULL, NULL, SW_SHOWNORMAL);
                                }
                            }
                            ImGui::PopID();
                        }
                        ImGui::EndTable();
                    }
                    ImGui::EndTabItem();
                }

                ImGui::EndTabBar();
                tcActiveTabTarget = -1;
            }

            ImGui::End();
        }

        // 5. Working Set Inspector Window
        if (showWsInspector && !g_isHudMode)
        {
            if (g_TriggerResetLayout)
            {
                ImGui::SetNextWindowPos(ImVec2(rightColX, 404.0f), ImGuiCond_Always);
                ImGui::SetNextWindowSize(ImVec2(rightColW, std::max(300.0f, vpH - 414.0f)), ImGuiCond_Always);
                ImGui::SetNextWindowCollapsed(false, ImGuiCond_Always);
            }
            else
            {
                ImGui::SetNextWindowPos(ImVec2(rightColX, 404.0f), ImGuiCond_FirstUseEver);
                ImGui::SetNextWindowSize(ImVec2(rightColW, 380.0f), ImGuiCond_FirstUseEver);
                ImGui::SetNextWindowCollapsed(false, ImGuiCond_Appearing);
            }
            ImGui::Begin("Working Set Inspector", &showWsInspector, ImGuiWindowFlags_NoCollapse);
            auto activeWs = vtServer->get_active_ws();
            if (activeWs)
            {
                ImGui::Text("Master Address: 0x%02X", activeWs->get_control_function()->get_address());
                ImGui::Text("Active Data Mask ID: %u", vtServer->get_active_data_mask_id());
                ImGui::Text("Active SoftKey Mask ID: %u", vtServer->get_active_soft_key_mask_id());

                const auto &tree = activeWs->get_object_tree();
                ImGui::Text("Total Objects in Pool: %zu", tree.size());
                ImGui::Separator();

                if (ImGui::BeginTable("ObjTreeTable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, 180)))
                {
                    ImGui::TableSetupColumn("Object ID", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                    ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 140.0f);
                    ImGui::TableSetupColumn("Children Count");
                    ImGui::TableHeadersRow();

                    for (const auto &pair : tree)
                    {
                        if (pair.second)
                        {
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn();
                            ImGui::Text("%u", pair.first);
                            ImGui::TableNextColumn();
                            ImGui::Text("Type %d", static_cast<int>(pair.second->get_object_type()));
                            ImGui::TableNextColumn();
                            ImGui::Text("%u", pair.second->get_number_children());
                        }
                    }
                    ImGui::EndTable();
                }
            }
            else
            {
                ImGui::TextDisabled("No Working Set currently active.");
            }

            ImGui::Separator();
            ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.4f, 1.0f), "Object Pool Disk Cache (Fast Boot):");
            ImGui::Text("Stored Implement Pools: %zu (%.1f KB on disk)",
                        vtServer->get_cached_pool_count(),
                        static_cast<float>(vtServer->get_cached_pool_total_bytes()) / 1024.0f);
            ImGui::SameLine();
            if (ImGui::Button("Clear Disk Cache"))
            {
                vtServer->clear_disk_cache();
            }
            ImGui::End();
        }

        // 6. Machine Diagnostics & Faults (J1939 DM1 / DM2)
        if (showDiagnosticsWindow && !g_isHudMode)
        {
            if (g_TriggerResetLayout)
            {
                ImGui::SetNextWindowPos(ImVec2(rightColX, 36.0f), ImGuiCond_Always);
                ImGui::SetNextWindowSize(ImVec2(rightColW, 360.0f), ImGuiCond_Always);
                ImGui::SetNextWindowCollapsed(false, ImGuiCond_Always);
            }
            else
            {
                ImGui::SetNextWindowPos(ImVec2(rightColX, 36.0f), ImGuiCond_FirstUseEver);
                ImGui::SetNextWindowSize(ImVec2(rightColW, 380.0f), ImGuiCond_FirstUseEver);
                ImGui::SetNextWindowCollapsed(false, ImGuiCond_Appearing);
            }
            ImGui::Begin("Machine Diagnostics (J1939 DM1 / DM2)", &showDiagnosticsWindow, ImGuiWindowFlags_NoCollapse);
            auto activeDtcs = canManager->get_active_dtcs();
            auto prevDtcs = canManager->get_historical_dtcs();

            if (ImGui::Button("Clear Fault List"))
            {
                canManager->clear_dtcs();
            }
            ImGui::SameLine();
            ImGui::Text("Active Faults (DM1): %zu | Previously Active (DM2): %zu", activeDtcs.size(), prevDtcs.size());

            ImGui::Separator();
            if (activeDtcs.empty())
            {
                ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.3f, 1.0f), "[ NO ACTIVE DIAGNOSTIC TROUBLE CODES - ALL SYSTEMS OK ]");
            }
            else
            {
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.2f, 1.0f), "Active Implement / Tractor Faults (DM1):");
                if (ImGui::BeginTable("DtcTable", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, 160)))
                {
                    ImGui::TableSetupColumn("ECU Source", ImGuiTableColumnFlags_WidthFixed, 75.0f);
                    ImGui::TableSetupColumn("SPN", ImGuiTableColumnFlags_WidthFixed, 60.0f);
                    ImGui::TableSetupColumn("FMI", ImGuiTableColumnFlags_WidthFixed, 40.0f);
                    ImGui::TableSetupColumn("Description");
                    ImGui::TableSetupColumn("Count", ImGuiTableColumnFlags_WidthFixed, 50.0f);
                    ImGui::TableSetupColumn("Lamp", ImGuiTableColumnFlags_WidthFixed, 65.0f);
                    ImGui::TableHeadersRow();

                    for (const auto &dtc : activeDtcs)
                    {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::Text("0x%02X", dtc.sourceAddress);
                        ImGui::TableNextColumn();
                        ImGui::Text("%u", dtc.spn);
                        ImGui::TableNextColumn();
                        ImGui::Text("%u", dtc.fmi);
                        ImGui::TableNextColumn();
                        ImGui::TextWrapped("%s", dtc.fmiDescription.c_str());
                        ImGui::TableNextColumn();
                        ImGui::Text("%u", dtc.occurrenceCount);
                        ImGui::TableNextColumn();
                        if (dtc.redStopLamp) ImGui::TextColored(ImVec4(1.0f, 0.2f, 0.2f, 1.0f), "STOP");
                        else if (dtc.amberWarningLamp) ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "WARN");
                        else if (dtc.milLamp) ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.1f, 1.0f), "MIL");
                        else ImGui::TextDisabled("None");
                    }
                    ImGui::EndTable();
                }
            }

            if (!prevDtcs.empty())
            {
                ImGui::Separator();
                ImGui::Text("Previously Active Faults (DM2):");
                for (const auto &dtc : prevDtcs)
                {
                    ImGui::TextDisabled("ECU 0x%02X | SPN %u FMI %u (%s) - %u occurrences",
                                        dtc.sourceAddress, dtc.spn, dtc.fmi, dtc.fmiDescription.c_str(), dtc.occurrenceCount);
                }
            }

            ImGui::End();
        }

        // Tractor Telemetry & J1939 Dashboard
        if (showTractorWindow && !g_isHudMode)
        {
            isobus_vt::TractorEcuManager::get_instance().render_tractor_dashboard(&showTractorWindow);
        }

        if (showTractorPgnConfig && !g_isHudMode)
        {
            isobus_vt::TractorEcuManager::get_instance().render_pgn_config_dialog(&showTractorPgnConfig);
        }

        // ISOBUS Standard Tools
        if (showFileServerWindow && !g_isHudMode)
        {
            isobus_vt::IsobusFileServer::get_instance().render_file_server_window(&showFileServerWindow);
        }
        if (showDdiExplorerWindow && !g_isHudMode)
        {
            isobus_vt::DdiExplorerManager::get_instance().render_ddi_explorer_window(&showDdiExplorerWindow);
        }
        if (showPoolInspectorWindow && !g_isHudMode)
        {
            isobus_vt::ObjectPoolInspector::get_instance().render_inspector_window(&showPoolInspectorWindow);
        }
        if (showAefLinterWindow && !g_isHudMode)
        {
            isobus_vt::AefPoolLinter::get_instance().render_linter_window(&showAefLinterWindow);
        }

            if (g_TriggerResetLayout)
            {
                g_TriggerResetLayout = false;
            }
        } // End of: if (splashScreen && splashScreen->is_active()) ... else ...

        // Rendering Dear ImGui
        ImGui::Render();
        const float clear_color_with_alpha[4] = { 0.12f, 0.14f, 0.18f, 1.0f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color_with_alpha);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        g_pSwapChain->Present(1, 0); // VSync
    }
    }
    catch (const std::exception &ex) {
        if (g_TerminalLogger) g_TerminalLogger->sink_CAN_stack_log(isobus::CANStackLogger::LoggingLevel::Critical, std::string("[FATAL LOOP EXCEPTION]: ") + ex.what());
    }
    catch (...) {
        if (g_TerminalLogger) g_TerminalLogger->sink_CAN_stack_log(isobus::CANStackLogger::LoggingLevel::Critical, "[FATAL UNKNOWN LOOP EXCEPTION]");
    }

    // Cleanup & Save persistent UI states
    {
        auto &c = isobus_vt::TerminalSettings::get_instance().get_config();
        c.showAuxWindow = showAuxWindow;
        c.showCanLogWindow = showCanLogWindow;
        c.showWsInspector = showWsInspector;
        c.showAogTcWindow = showAogTcWindow;
        c.showDiagnosticsWindow = showDiagnosticsWindow;
        c.isDayMode = isDayMode;
        c.alarmSoundMuted = alarmSoundMuted;
        c.canDriver = selectedDriverIndex;
        c.pcanChannel = selectedPcanChannel;
        isobus_vt::TerminalSettings::get_instance().save();
    }

    tcManager->shutdown();
    canManager->shutdown();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);

    return 0;
}

// Helper functions for DirectX 11
bool CreateDeviceD3D(HWND hWnd)
{
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0, };
    HRESULT res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                               createDeviceFlags, featureLevelArray, 2,
                                               D3D11_SDK_VERSION, &sd, &g_pSwapChain,
                                               &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res == DXGI_ERROR_UNSUPPORTED)
        res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                                           createDeviceFlags, featureLevelArray, 2,
                                           D3D11_SDK_VERSION, &sd, &g_pSwapChain,
                                           &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res != S_OK)
        return false;

    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D()
{
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

void CreateRenderTarget()
{
    ID3D11Texture2D* pBackBuffer;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
    pBackBuffer->Release();
}

void CleanupRenderTarget()
{
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg)
    {
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED)
            return 0;
        g_ResizeWidth = (UINT)LOWORD(lParam);
        g_ResizeHeight = (UINT)HIWORD(lParam);
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU)
            return 0;
        break;
    case WM_CLOSE:
        if (g_TerminalLogger) g_TerminalLogger->sink_CAN_stack_log(isobus::CANStackLogger::LoggingLevel::Warning, "WndProc: Received WM_CLOSE!");
        break;
    case WM_DESTROY:
        if (g_TerminalLogger) g_TerminalLogger->sink_CAN_stack_log(isobus::CANStackLogger::LoggingLevel::Warning, "WndProc: Received WM_DESTROY!");
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
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

    // Load TrueType fonts for crisp, non-blurry ISOBUS rendering
    ImFontConfig fontConfig;
    fontConfig.OversampleH = 3;
    fontConfig.OversampleV = 3;
    fontConfig.PixelSnapH = true;

    ImFont* fontRegular = nullptr;
    ImFont* fontMedium  = nullptr;
    ImFont* fontLarge   = nullptr;

    if (std::filesystem::exists("C:\\Windows\\Fonts\\consola.ttf"))
    {
        fontRegular = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\consola.ttf", 15.0f, &fontConfig);
        fontMedium  = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\consolab.ttf", 22.0f, &fontConfig);
        fontLarge   = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\consolab.ttf", 34.0f, &fontConfig);
    }
    else if (std::filesystem::exists("C:\\Windows\\Fonts\\segoeui.ttf"))
    {
        fontRegular = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 15.0f, &fontConfig);
        fontMedium  = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeuib.ttf", 22.0f, &fontConfig);
        fontLarge   = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeuib.ttf", 34.0f, &fontConfig);
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

    auto auxManager = std::make_shared<isobus_vt::AuxManager>(vtServerCF);
    auxManager->initialize();

    // Initialize CAN Driver from saved configuration (PEAK PCAN or Virtual)
    int selectedDriverIndex = cfg.canDriver;
    int selectedPcanChannel = cfg.pcanChannel;
    auto canManager = std::make_shared<isobus_vt::CanManager>();
    auto driverType = (selectedDriverIndex == 0) ? isobus_vt::CanDriverType::PeakPcan : isobus_vt::CanDriverType::VirtualLoopback;
    canManager->initialize(driverType, selectedPcanChannel);

    auto tcManager = std::make_shared<isobus_vt::TaskControllerManager>();
    tcManager->initialize(0);

    auto vtRenderer = std::make_shared<isobus_vt::VTRenderer>(vtServer);
    vtRenderer->set_managers(canManager, tcManager);

    // UI state variables initialized from persistent configuration
    bool showAuxWindow = cfg.showAuxWindow;
    bool showCanLogWindow = cfg.showCanLogWindow;
    bool showWsInspector = cfg.showWsInspector;
    bool showAogTcWindow = cfg.showAogTcWindow;
    bool showDiagnosticsWindow = cfg.showDiagnosticsWindow;
    bool showMiniViewsWindow = cfg.showMiniViewsWindow;
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
        auxManager->update();
        tcManager->update();

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
            // First frame after splash screen: trigger clean responsive layout
            static bool initialLayoutApplied = false;
            if (!initialLayoutApplied)
            {
                initialLayoutApplied = true;
                g_TriggerResetLayout = true;
                if (vtRenderer) vtRenderer->reset_layout();
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
                ImGui::MenuItem("AgOpenGPS Task Controller", nullptr, &showAogTcWindow);
                ImGui::MenuItem("AUX-N Control Center", nullptr, &showAuxWindow);
                ImGui::MenuItem("CAN Bus Monitor", nullptr, &showCanLogWindow);
                ImGui::MenuItem("Working Set Inspector", nullptr, &showWsInspector);
                ImGui::MenuItem("Machine Diagnostics (DM1 / DM2)", nullptr, &showDiagnosticsWindow);
                ImGui::MenuItem("Podglad Maszyn w Tle (Mini-Views)", nullptr, &showMiniViewsWindow);
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

        // 6. AgOpenGPS Task Controller & Section Control Window
        if (showAogTcWindow && !g_isHudMode)
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
                ImGui::SetNextWindowSize(ImVec2(rightColW, 360.0f), ImGuiCond_FirstUseEver);
                ImGui::SetNextWindowCollapsed(false, ImGuiCond_Appearing);
            }
            ImGui::Begin("AgOpenGPS Task Controller (TC)", &showAogTcWindow, ImGuiWindowFlags_NoCollapse);
            bool aogActive = tcManager->is_aog_connected();
            if (aogActive)
            {
                ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "[ AGOPENGPS CONNECTED ]");
            }
            else
            {
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "[ WAITING FOR AGOPENGPS / AGIO (UDP 8888) ]");
            }

            ImGui::SameLine();
            ImGui::TextDisabled("| Subnet: %s", tcManager->get_udp_bridge() ? tcManager->get_udp_bridge()->get_subnet_string().c_str() : "unknown");

            ImGui::Separator();
            ImGui::Columns(3, "AogInfoCols", false);
            ImGui::Text("Mode: %s", tcManager->is_auto_mode() ? "AUTOMATIC" : "MANUAL");
            ImGui::NextColumn();
            ImGui::Text("GPS Speed: %.1f km/h", tcManager->get_speed_kph());
            ImGui::NextColumn();
            ImGui::Text("Fix Quality: %u", tcManager->get_udp_bridge() ? tcManager->get_udp_bridge()->get_gps_fix_quality() : 0);
            ImGui::Columns(1);

            ImGui::Separator();
            std::uint8_t nSections = tcManager->get_num_sections();
            if (nSections > 0)
            {
                auto setpoints = tcManager->get_section_setpoints();
                auto actualStates = tcManager->get_section_states();

                ImGui::Text("Implement Sections (%u detected in DDOP - Auto AgOpenGPS):", nSections);
                for (std::uint8_t i = 0; i < nSections; ++i)
                {
                    if (i > 0 && (i % 8 != 0)) ImGui::SameLine();
                    bool actOn = (i < actualStates.size()) && actualStates[i];
                    bool spOn  = (i < setpoints.size()) && setpoints[i];
                    bool on    = actOn || spOn;

                    ImVec4 col = on ? ImVec4(0.2f, 0.85f, 0.3f, 1.0f) : ImVec4(0.45f, 0.45f, 0.50f, 1.0f);
                    ImGui::TextColored(col, "[Sec %u: %s]", i + 1, on ? "ON" : "OFF");
                }
            }
            else
            {
                ImGui::TextDisabled("No Implement Sections registered yet. Connect implement to ISOBUS.");
            }

            ImGui::Separator();
            ImGui::Text("Task Controller Activity Log:");
            if (ImGui::BeginChild("TcLogChild", ImVec2(0, 100), true))
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

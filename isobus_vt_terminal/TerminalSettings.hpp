#pragma once

#include <string>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstdint>
#include <vector>

namespace isobus_vt
{
    struct TerminalConfig
    {
        // ── Hardware & CAN Bus ──
        int canDriver = 0;                  // 0 = PEAK PCAN, 1 = Virtual Loopback
        int pcanChannel = 81;               // PCAN_USBBUS1 (0x51 = 81)
        int canBaudRate = 250000;           // 250 kbps (standard ISOBUS)
        bool autoReconnect = true;          // Auto-reconnect CAN on bus error/disconnection

        // ── VT Universal Terminal Display & Engine ──
        int vtVersion = 6;                  // ISO 11783-6 VT Version (2, 3, 4, 5, 6)
        int dataMaskSize = 480;             // Base square dimension
        int dataMaskWidth = 480;            // Width (px)
        int dataMaskHeight = 480;           // Height (px)
        int skDescWidth = 60;               // Soft key designator width
        int skDescHeight = 60;              // Soft key designator height
        int skColumns = 1;                  // Soft key columns (1 or 2)
        int skRows = 6;                     // Soft key rows (F1..F6 or F1..F12)
        int vtNumber = 1;                   // VT instance (1 = primary)
        float zoom = 1.0f;                  // Visual zoom factor (0.5x to 3.0x)
        bool softKeyOnRight = true;         // True: right side, False: left side
        int bezelStyle = 0;                 // 0: Rugged Tractor, 1: Modern Glass, 2: Borderless
        bool autoScaleSoftKeys = true;      // Auto-fit oversized soft key graphics
        bool autoScaleDataMask = true;      // Auto-scale data mask if content exceeds canvas

        // ── Audio & Tactile Feedback ──
        bool alarmSoundMuted = false;       // Alarm buzzer sound
        bool touchBeepEnabled = true;       // Click sound on touch / button press
        int buzzerVolume = 100;             // Volume percentage

        // ── Theme & Appearance ──
        bool isDayMode = false;             // False: Night Mode (Dark), True: Day Mode (Light)
        float uiTouchScale = 1.0f;          // Scale of UI elements for touchscreens

        // ── AgOpenGPS Task Controller (TC) ──
        bool tcEnabled = true;              // Task Controller enabled
        int aogRxPort = 8888;               // UDP Port listening from AgOpenGPS/AGIO
        int aogTxPort = 9999;               // UDP Port broadcasting to AgOpenGPS
        std::string aogSubnet = "192.168.5";// Subnet for UDP broadcast
        bool autoBoomSections = true;       // Automatically map DDOP boom sections

        // ── Panel Visibility States ──
        bool showAogTcWindow = true;
        bool showAuxWindow = true;
        bool showCanLogWindow = true;
        bool showWsInspector = true;
        bool showDiagnosticsWindow = false;
        bool showMiniViewsWindow = true;    // Podgląd maszyn w tle (Multi-Working Set Mini-Views)

        // ── Dockable HUD Overlay na AgOpenGPS ──
        bool hudModeEnabled = false;        // Tryb kompaktowej nakładki HUD
        int hudDockSide = 0;                // 0 = Dokuj do Prawej, 1 = Dokuj do Lewej, 2 = Pływające
        float hudOpacity = 0.88f;           // Przezroczystość nakładki HUD (0.60 do 1.00)
        bool hudAlwaysOnTop = true;         // Zawsze na wierzchu (nad AgOpenGPS)

        // ── Layout & View Splitter ──
        float splitRatio = 0.50f;           // Proporcja szerokości AgOpen (0.20f do 0.80f)
        int activeRightTab = 0;             // 0 = Terminal VT, 1 = AUX-N, 2 = Sekcje TC, 3 = CAN Monitor, 4 = Diagnostyka, 5 = Mini-Views

        // ── Regional Localization & Units (ISO 11783-7) ──
        std::string langCode = "en";
        std::string countryCode = "US";
        int unitArea = 0;                   // 0: Metric (ha), 1: Imperial (acre)
        int unitDate = 0;                   // 0: DD/MM/YYYY, 1: MM/DD/YYYY, 2: YYYY-MM-DD
        int unitDecimal = 0;                // 0: Dot (.), 1: Comma (,)
        int unitDistance = 0;               // 0: Metric (km/m), 1: Imperial (mi/ft)
        int unitForce = 0;
        int unitGeneric = 0;
        int unitMass = 0;                   // 0: Metric (kg), 1: Imperial (lb)
        int unitPressure = 0;               // 0: Metric (bar/kPa), 1: Imperial (psi)
        int unitTemperature = 0;            // 0: Celsius, 1: Fahrenheit
        int unitTime = 0;
        int unitVolume = 0;                 // 0: Metric (L), 1: Imperial (gal)
    };

    class TerminalSettings
    {
    public:
        static TerminalSettings& get_instance();

        TerminalConfig& get_config() { return config; }
        const TerminalConfig& get_config() const { return config; }

        void load();
        void save();
        void reset_to_defaults();

        std::string get_settings_file_path() const;

        // Object pool cache helpers
        std::size_t get_cached_pools_count() const;
        std::uint64_t get_cached_pools_total_bytes() const;
        std::vector<std::string> get_cached_pool_names() const;
        void clear_object_pool_cache();

        // Audio helper
        void play_touch_beep();
        void play_alarm_beep();

    private:
        TerminalSettings();
        TerminalConfig config;
    };
}

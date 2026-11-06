#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "TerminalSettings.hpp"
#include "AudioBeeper.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;

namespace isobus_vt
{
    static std::string get_exe_dir_path()
    {
        char buf[MAX_PATH] = {0};
        DWORD len = GetModuleFileNameA(nullptr, buf, MAX_PATH);
        if (len > 0)
        {
            std::string s(buf, len);
            auto p = s.find_last_of("\\/");
            if (p != std::string::npos)
            {
                return s.substr(0, p + 1);
            }
        }
        return "";
    }

    TerminalSettings& TerminalSettings::get_instance()
    {
        static TerminalSettings instance;
        return instance;
    }

    TerminalSettings::TerminalSettings()
    {
        reset_to_defaults();
        load();
    }

    void TerminalSettings::reset_to_defaults()
    {
        config = TerminalConfig{};
    }

    std::string TerminalSettings::get_settings_file_path() const
    {
        std::string exeDir = get_exe_dir_path();
        if (!exeDir.empty())
        {
            return exeDir + "vt_settings.ini";
        }
        return "vt_settings.ini";
    }

    void TerminalSettings::load()
    {
        std::string paths[] = { get_settings_file_path(), "vt_settings.ini" };
        std::ifstream in;
        for (const auto &p : paths)
        {
            in.open(p);
            if (in.is_open()) break;
        }

        if (!in.is_open())
        {
            // First time boot - save defaults
            save();
            return;
        }

        std::string line;
        while (std::getline(in, line))
        {
            auto pos = line.find('=');
            if (pos == std::string::npos) continue;
            std::string key = line.substr(0, pos);
            std::string val = line.substr(pos + 1);

            key.erase(std::remove_if(key.begin(), key.end(), ::isspace), key.end());
            val.erase(std::remove_if(val.begin(), val.end(), [](unsigned char c) { return std::isspace(c); }), val.end());

            try
            {
                // [Hardware]
                if (key == "can_driver") config.canDriver = std::stoi(val);
                else if (key == "pcan_channel") config.pcanChannel = std::stoi(val);
                else if (key == "can_baud") config.canBaudRate = std::stoi(val);
                else if (key == "auto_reconnect") config.autoReconnect = (std::stoi(val) != 0);

                // [VT]
                else if (key == "version")
                {
                    int v = std::stoi(val);
                    if (v >= 0 && v <= 1)
                        config.vtVersion = (v == 0) ? 2 : 3; // map old enum 0/1 to version 2/3
                    else if (v >= 2 && v <= 6)
                        config.vtVersion = v;
                    else
                        config.vtVersion = 4;
                }
                else if (key == "data_mask_size")
                {
                    config.dataMaskSize = std::max(60, std::min(1920, std::stoi(val)));
                    config.dataMaskWidth = config.dataMaskSize;
                    config.dataMaskHeight = config.dataMaskSize;
                }
                else if (key == "data_mask_width") config.dataMaskWidth = std::max(60, std::min(1920, std::stoi(val)));
                else if (key == "data_mask_height") config.dataMaskHeight = std::max(60, std::min(1920, std::stoi(val)));
                else if (key == "sk_desc_width") config.skDescWidth = std::max(40, std::min(255, std::stoi(val)));
                else if (key == "sk_desc_height") config.skDescHeight = std::max(40, std::min(255, std::stoi(val)));
                else if (key == "sk_columns") config.skColumns = std::max(1, std::min(4, std::stoi(val)));
                else if (key == "sk_rows") config.skRows = std::max(1, std::min(16, std::stoi(val)));
                else if (key == "vt_number") config.vtNumber = std::max(1, std::min(32, std::stoi(val)));
                else if (key == "zoom") config.zoom = std::max(0.5f, std::min(3.0f, std::stof(val)));
                else if (key == "softkey_right") config.softKeyOnRight = (std::stoi(val) != 0);
                else if (key == "bezel_style") config.bezelStyle = std::max(0, std::min(2, std::stoi(val)));
                else if (key == "autoscale_softkeys") config.autoScaleSoftKeys = (std::stoi(val) != 0);
                else if (key == "autoscale_datamask") config.autoScaleDataMask = (std::stoi(val) != 0);

                // [Audio & Theme]
                else if (key == "alarm_muted") config.alarmSoundMuted = (std::stoi(val) != 0);
                else if (key == "touch_beep") config.touchBeepEnabled = (std::stoi(val) != 0);
                else if (key == "day_mode") config.isDayMode = (std::stoi(val) != 0);
                else if (key == "touch_scale") config.uiTouchScale = std::max(0.7f, std::min(2.0f, std::stof(val)));

                // [TaskController]
                // [TaskController & Modules]
                else if (key == "tc_enabled") config.tcEnabled = (std::stoi(val) != 0);
                else if (key == "tecu_enabled") config.tecuEnabled = (std::stoi(val) != 0);
                else if (key == "file_server_enabled") config.fileServerEnabled = (std::stoi(val) != 0);
                else if (key == "aux_enabled") config.auxEnabled = (std::stoi(val) != 0);
                else if (key == "ddi_explorer_enabled") config.ddiExplorerEnabled = (std::stoi(val) != 0);
                else if (key == "mini_views_enabled") config.miniViewsEnabled = (std::stoi(val) != 0);
                else if (key == "amazone_bridge_enabled") config.amazoneBridgeEnabled = (std::stoi(val) != 0);
                else if (key == "startup_window") config.startupWindow = std::max(0, std::min(9, std::stoi(val)));
                else if (key == "tc_startup_tab") config.tcStartupTab = std::max(0, std::min(5, std::stoi(val)));
                else if (key == "aog_rx_port") config.aogRxPort = std::stoi(val);
                else if (key == "aog_tx_port") config.aogTxPort = std::stoi(val);
                else if (key == "aog_subnet") config.aogSubnet = val;
                else if (key == "auto_sections") config.autoBoomSections = (std::stoi(val) != 0);

                // [Layout]
                else if (key == "split_ratio") config.splitRatio = std::max(0.15f, std::min(0.85f, std::stof(val)));
                else if (key == "active_tab") config.activeRightTab = std::max(0, std::min(5, std::stoi(val)));

                // [Panels]
                else if (key == "show_aog_tc") config.showAogTcWindow = (std::stoi(val) != 0);
                else if (key == "show_aux") config.showAuxWindow = (std::stoi(val) != 0);
                else if (key == "show_can_log") config.showCanLogWindow = (std::stoi(val) != 0);
                else if (key == "show_ws_inspector") config.showWsInspector = (std::stoi(val) != 0);
                else if (key == "show_diagnostics") config.showDiagnosticsWindow = (std::stoi(val) != 0);
                else if (key == "show_mini_views") config.showMiniViewsWindow = (std::stoi(val) != 0);
                else if (key == "show_amazone") config.showAmazoneWindow = (std::stoi(val) != 0);

                // [HUD Overlay]
                else if (key == "cockpit_mode") config.cockpitMode = (std::stoi(val) != 0);
                else if (key == "hud_mode") config.hudModeEnabled = (std::stoi(val) != 0);
                else if (key == "hud_dock_side") config.hudDockSide = std::max(0, std::min(2, std::stoi(val)));
                else if (key == "hud_opacity") config.hudOpacity = std::max(0.40f, std::min(1.0f, std::stof(val)));
                else if (key == "hud_always_on_top") config.hudAlwaysOnTop = (std::stoi(val) != 0);

                // [Language]
                else if (key == "lang" && val.length() >= 2) config.langCode = val.substr(0, 2);
                else if (key == "country" && val.length() >= 2) config.countryCode = val.substr(0, 2);
                else if (key == "unit_area") config.unitArea = std::stoi(val);
                else if (key == "unit_date") config.unitDate = std::stoi(val);
                else if (key == "unit_decimal") config.unitDecimal = std::stoi(val);
                else if (key == "unit_distance") config.unitDistance = std::stoi(val);
                else if (key == "unit_force") config.unitForce = std::stoi(val);
                else if (key == "unit_generic") config.unitGeneric = std::stoi(val);
                else if (key == "unit_mass") config.unitMass = std::stoi(val);
                else if (key == "unit_pressure") config.unitPressure = std::stoi(val);
                else if (key == "unit_temperature") config.unitTemperature = std::stoi(val);
                else if (key == "unit_time") config.unitTime = std::stoi(val);
                else if (key == "unit_volume") config.unitVolume = std::stoi(val);
            }
            catch (...) {}
        }
    }

    void TerminalSettings::save()
    {
        auto writeTo = [this](std::ostream &out) {
            out << "# ===================================================================\n";
            out << "# ISOBUS Universal Terminal & Task Controller Configuration File\n";
            out << "# Autogenerated & Persistent - Compatible with ISO 11783 Standards\n";
            out << "# ===================================================================\n\n";

            out << "[Hardware]\n";
            out << "can_driver=" << config.canDriver << "\n";
            out << "pcan_channel=" << config.pcanChannel << "\n";
            out << "can_baud=" << config.canBaudRate << "\n";
            out << "auto_reconnect=" << (config.autoReconnect ? 1 : 0) << "\n\n";

            out << "[VT]\n";
            out << "version=" << config.vtVersion << "\n";
            out << "data_mask_size=" << config.dataMaskSize << "\n";
            out << "data_mask_width=" << config.dataMaskWidth << "\n";
            out << "data_mask_height=" << config.dataMaskHeight << "\n";
            out << "sk_desc_width=" << config.skDescWidth << "\n";
            out << "sk_desc_height=" << config.skDescHeight << "\n";
            out << "sk_columns=" << config.skColumns << "\n";
            out << "sk_rows=" << config.skRows << "\n";
            out << "vt_number=" << config.vtNumber << "\n";
            out << "zoom=" << config.zoom << "\n";
            out << "softkey_right=" << (config.softKeyOnRight ? 1 : 0) << "\n";
            out << "bezel_style=" << config.bezelStyle << "\n";
            out << "autoscale_softkeys=" << (config.autoScaleSoftKeys ? 1 : 0) << "\n";
            out << "autoscale_datamask=" << (config.autoScaleDataMask ? 1 : 0) << "\n\n";

            out << "[Audio]\n";
            out << "alarm_muted=" << (config.alarmSoundMuted ? 1 : 0) << "\n";
            out << "touch_beep=" << (config.touchBeepEnabled ? 1 : 0) << "\n";
            out << "day_mode=" << (config.isDayMode ? 1 : 0) << "\n";
            out << "touch_scale=" << config.uiTouchScale << "\n\n";

            out << "[Startup]\n";
            out << "startup_window=" << config.startupWindow << "\n";
            out << "tc_startup_tab=" << config.tcStartupTab << "\n\n";

            out << "[Modules]\n";
            out << "tc_enabled=" << (config.tcEnabled ? 1 : 0) << "\n";
            out << "tecu_enabled=" << (config.tecuEnabled ? 1 : 0) << "\n";
            out << "file_server_enabled=" << (config.fileServerEnabled ? 1 : 0) << "\n";
            out << "aux_enabled=" << (config.auxEnabled ? 1 : 0) << "\n";
            out << "ddi_explorer_enabled=" << (config.ddiExplorerEnabled ? 1 : 0) << "\n";
            out << "mini_views_enabled=" << (config.miniViewsEnabled ? 1 : 0) << "\n";
            out << "amazone_bridge_enabled=" << (config.amazoneBridgeEnabled ? 1 : 0) << "\n\n";

            out << "[TaskController]\n";
            out << "tc_enabled=" << (config.tcEnabled ? 1 : 0) << "\n";
            out << "aog_rx_port=" << config.aogRxPort << "\n";
            out << "aog_tx_port=" << config.aogTxPort << "\n";
            out << "aog_subnet=" << config.aogSubnet << "\n";
            out << "auto_sections=" << (config.autoBoomSections ? 1 : 0) << "\n\n";

            out << "[Layout]\n";
            out << "split_ratio=" << config.splitRatio << "\n";
            out << "active_tab=" << config.activeRightTab << "\n\n";

            out << "[Panels]\n";
            out << "show_aog_tc=" << (config.showAogTcWindow ? 1 : 0) << "\n";
            out << "show_aux=" << (config.showAuxWindow ? 1 : 0) << "\n";
            out << "show_can_log=" << (config.showCanLogWindow ? 1 : 0) << "\n";
            out << "show_ws_inspector=" << (config.showWsInspector ? 1 : 0) << "\n";
            out << "show_diagnostics=" << (config.showDiagnosticsWindow ? 1 : 0) << "\n";
            out << "show_mini_views=" << (config.showMiniViewsWindow ? 1 : 0) << "\n";
            out << "show_amazone=" << (config.showAmazoneWindow ? 1 : 0) << "\n\n";

            out << "[HUD]\n";
            out << "cockpit_mode=" << (config.cockpitMode ? 1 : 0) << "\n";
            out << "hud_mode=" << (config.hudModeEnabled ? 1 : 0) << "\n";
            out << "hud_dock_side=" << config.hudDockSide << "\n";
            out << "hud_opacity=" << config.hudOpacity << "\n";
            out << "hud_always_on_top=" << (config.hudAlwaysOnTop ? 1 : 0) << "\n\n";

            out << "[Language]\n";
            out << "lang=" << config.langCode << "\n";
            out << "country=" << config.countryCode << "\n";
            out << "unit_area=" << config.unitArea << "\n";
            out << "unit_date=" << config.unitDate << "\n";
            out << "unit_decimal=" << config.unitDecimal << "\n";
            out << "unit_distance=" << config.unitDistance << "\n";
            out << "unit_force=" << config.unitForce << "\n";
            out << "unit_generic=" << config.unitGeneric << "\n";
            out << "unit_mass=" << config.unitMass << "\n";
            out << "unit_pressure=" << config.unitPressure << "\n";
            out << "unit_temperature=" << config.unitTemperature << "\n";
            out << "unit_time=" << config.unitTime << "\n";
            out << "unit_volume=" << config.unitVolume << "\n";
        };

        {
            std::ofstream out("vt_settings.ini");
            if (out.is_open()) writeTo(out);
        }
        std::string fullPath = get_settings_file_path();
        if (fullPath != "vt_settings.ini")
        {
            std::ofstream out2(fullPath);
            if (out2.is_open()) writeTo(out2);
        }
    }

    std::size_t TerminalSettings::get_cached_pools_count() const
    {
        std::string dir = "object_pool_cache";
        if (!fs::exists(dir))
        {
            std::string exeDir = get_exe_dir_path();
            dir = exeDir + "object_pool_cache";
        }
        if (!fs::exists(dir)) return 0;

        std::size_t count = 0;
        try {
            for (const auto &entry : fs::directory_iterator(dir))
            {
                if (entry.is_regular_file() && entry.path().extension() == ".iop")
                    count++;
            }
        } catch (...) {}
        return count;
    }

    std::uint64_t TerminalSettings::get_cached_pools_total_bytes() const
    {
        std::string dir = "object_pool_cache";
        if (!fs::exists(dir))
        {
            std::string exeDir = get_exe_dir_path();
            dir = exeDir + "object_pool_cache";
        }
        if (!fs::exists(dir)) return 0;

        std::uint64_t total = 0;
        try {
            for (const auto &entry : fs::directory_iterator(dir))
            {
                if (entry.is_regular_file() && entry.path().extension() == ".iop")
                    total += entry.file_size();
            }
        } catch (...) {}
        return total;
    }

    std::vector<std::string> TerminalSettings::get_cached_pool_names() const
    {
        std::vector<std::string> names;
        std::string dir = "object_pool_cache";
        if (!fs::exists(dir))
        {
            std::string exeDir = get_exe_dir_path();
            dir = exeDir + "object_pool_cache";
        }
        if (!fs::exists(dir)) return names;

        try {
            for (const auto &entry : fs::directory_iterator(dir))
            {
                if (entry.is_regular_file() && entry.path().extension() == ".iop")
                    names.push_back(entry.path().filename().string());
            }
        } catch (...) {}
        return names;
    }

    void TerminalSettings::clear_object_pool_cache()
    {
        std::string dirs[] = { "object_pool_cache", get_exe_dir_path() + "object_pool_cache" };
        for (const auto &d : dirs)
        {
            if (fs::exists(d))
            {
                try {
                    for (const auto &entry : fs::directory_iterator(d))
                    {
                        if (entry.is_regular_file() && entry.path().extension() == ".iop")
                            fs::remove(entry.path());
                    }
                } catch (...) {}
            }
        }
    }

    void TerminalSettings::play_touch_beep()
    {
        AudioBeeper::get_instance().play_touch_click();
    }

    void TerminalSettings::play_alarm_beep()
    {
        AudioBeeper::get_instance().play_alarm_tone();
    }
}

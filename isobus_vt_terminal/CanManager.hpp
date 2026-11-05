#pragma once

#include "isobus/hardware_integration/can_hardware_interface.hpp"
#include "isobus/hardware_integration/pcan_basic_windows_plugin.hpp"
#include "isobus/hardware_integration/virtual_can_plugin.hpp"
#include "isobus/isobus/can_message_frame.hpp"

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace isobus_vt
{
    struct CanLogEntry
    {
        std::uint32_t timestampMs = 0;
        std::uint32_t id = 0;
        std::uint32_t pgn = 0;
        std::uint8_t source = 0;
        std::uint8_t dest = 0;
        std::uint8_t dlc = 0;
        std::uint8_t data[8] = {0};
        bool isTx = false;
        std::string pgnName;
    };

    struct DiagnosticTroubleCode
    {
        std::uint8_t sourceAddress = 0;
        std::uint32_t spn = 0;
        std::uint8_t fmi = 0;
        std::uint8_t occurrenceCount = 0;
        bool redStopLamp = false;
        bool amberWarningLamp = false;
        bool milLamp = false;
        bool protectLamp = false;
        bool isPreviouslyActive = false; // DM2
        std::uint32_t timestampMs = 0;
        std::string fmiDescription;
    };

    enum class CanDriverType
    {
        PeakPcan,
        VirtualLoopback
    };

    class CanManager
    {
    public:
        static CanManager* get_active_instance();
    public:
        CanManager();
        ~CanManager();

        bool initialize(CanDriverType driverType = CanDriverType::PeakPcan, int pcanChannel = PCAN_USBBUS1);
        void shutdown();

        bool reconnect();
        void set_driver_type(CanDriverType type);
        CanDriverType get_driver_type() const;

        void set_pcan_channel(int channel);
        int get_pcan_channel() const;

        bool is_connected() const;
        std::string get_status_string() const;
        std::string get_last_error() const;

        std::uint64_t get_rx_count() const;
        std::uint64_t get_tx_count() const;
        float get_fps() const;

        // Bus Load (250 kbps nominal ISOBUS baud)
        float get_bus_load() const;
        std::vector<float> get_bus_load_history() const;

        // Diagnostics DM1 / DM2
        std::vector<DiagnosticTroubleCode> get_active_dtcs() const;
        std::vector<DiagnosticTroubleCode> get_historical_dtcs() const;
        void clear_dtcs();
        static std::string get_fmi_description(std::uint8_t fmi);

        std::vector<CanLogEntry> get_recent_frames(std::size_t maxCount = 200);
        void clear_log();
        void set_log_paused(bool paused);
        bool is_log_paused() const;

    private:
        CanDriverType currentDriverType = CanDriverType::PeakPcan;
        int currentPcanChannel = PCAN_USBBUS1;

        std::shared_ptr<isobus::CANHardwarePlugin> activeDriver = nullptr;
        std::atomic<bool> connected{false};
        std::string lastError;

        std::atomic<std::uint64_t> totalRxFrames{0};
        std::atomic<std::uint64_t> totalTxFrames{0};
        std::atomic<std::uint32_t> framesLastSecond{0};
        std::uint32_t lastFpsCalcTimestamp = 0;
        float currentFps = 0.0f;

        mutable std::mutex logMutex;
        std::deque<CanLogEntry> logEntries;
        std::atomic<bool> logPaused{false};

        // Bus Load Tracking
        std::atomic<float> currentBusLoadPercent{0.0f};
        std::atomic<std::uint32_t> bitsLastSecond{0};
        std::deque<float> busLoadHistory;

        // J1939 Diagnostics Tracking
        mutable std::mutex dtcMutex;
        std::vector<DiagnosticTroubleCode> activeDtcs;
        std::vector<DiagnosticTroubleCode> historicalDtcs;
        void process_j1939_diagnostics(const isobus::CANMessageFrame &frame);

        void on_frame_received(const isobus::CANMessageFrame &frame);
        void on_frame_transmitted(const isobus::CANMessageFrame &frame);
        static std::string format_pgn_name(std::uint32_t pgn);
    };
}

#pragma once

#include "isobus/isobus/can_internal_control_function.hpp"
#include "isobus/isobus/isobus_data_dictionary.hpp"
#include "isobus/isobus/isobus_device_descriptor_object_pool.hpp"
#include "isobus/isobus/isobus_device_descriptor_object_pool_helpers.hpp"
#include "isobus/isobus/isobus_functionalities.hpp"
#include "isobus/isobus/isobus_speed_distance_messages.hpp"
#include "isobus/isobus/isobus_standard_data_description_indices.hpp"
#include "isobus/isobus/isobus_task_controller_server.hpp"
#include "isobus/isobus/isobus_time_date_interface.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

namespace isobus_vt
{
    // Section working state
    enum class SectionState : std::uint8_t
    {
        Off = 0,
        On = 1,
        Error = 2,
        NotInstalled = 3
    };

    // Client state tracking connected implement sections & geometry
    class TcClientState
    {
    public:
        void set_number_of_sections(std::uint8_t number);
        std::uint8_t get_number_of_sections() const;

        void set_section_setpoint(std::uint8_t section, std::uint8_t state);
        std::uint8_t get_section_setpoint(std::uint8_t section) const;

        void set_section_actual(std::uint8_t section, std::uint8_t state);
        std::uint8_t get_section_actual(std::uint8_t section) const;

        void set_element_for_section(std::uint8_t section, std::uint16_t elementNumber);
        std::uint16_t get_element_for_section(std::uint8_t section) const;

        bool is_section_control_enabled() const { return sectionControlEnabled; }
        void set_section_control_enabled(bool enabled) { sectionControlEnabled = enabled; }

        isobus::DeviceDescriptorObjectPool& get_pool() { return ddop; }

        bool are_measurements_sent() const { return measurementsSent; }
        void mark_measurements_sent() { measurementsSent = true; }

        void set_element_for_ddi(isobus::DataDescriptionIndex ddi, std::uint16_t elem) { ddiToElem[ddi] = elem; }
        bool has_element_for_ddi(isobus::DataDescriptionIndex ddi) const { return ddiToElem.find(ddi) != ddiToElem.end(); }
        std::uint16_t get_element_for_ddi(isobus::DataDescriptionIndex ddi) const
        {
            auto it = ddiToElem.find(ddi);
            return (it != ddiToElem.end()) ? it->second : 0;
        }

    private:
        isobus::DeviceDescriptorObjectPool ddop;
        std::uint8_t numberOfSections = 0;
        std::vector<std::uint8_t> setpoints;
        std::vector<std::uint8_t> actualStates;
        std::vector<std::uint16_t> sectionToElement;
        std::map<isobus::DataDescriptionIndex, std::uint16_t> ddiToElem;
        bool sectionControlEnabled = true;
        bool measurementsSent = false;
    };

    // Derived TaskControllerServer implementation
    class TerminalTCServer : public isobus::TaskControllerServer
    {
    public:
        TerminalTCServer(std::shared_ptr<isobus::InternalControlFunction> tcCF,
                         isobus::TaskControllerServer::TaskControllerVersion version =
                             isobus::TaskControllerServer::TaskControllerVersion::SecondEditionDraft);

        bool activate_object_pool(std::shared_ptr<isobus::ControlFunction> partnerCF,
                                  ObjectPoolActivationError &activationError,
                                  ObjectPoolErrorCodes &objectPoolError,
                                  std::uint16_t &parentObjectIDOfFaultyObject,
                                  std::uint16_t &faultyObjectID) override;

        bool deactivate_object_pool(std::shared_ptr<isobus::ControlFunction> partnerCF) override;
        bool delete_device_descriptor_object_pool(std::shared_ptr<isobus::ControlFunction> partnerCF,
                                                  ObjectPoolDeletionErrors &error) override;
        bool change_designator(std::shared_ptr<isobus::ControlFunction>, std::uint16_t, const std::vector<std::uint8_t> &) override { return true; }
        bool store_device_descriptor_object_pool(std::shared_ptr<isobus::ControlFunction> partnerCF,
                                                 const std::vector<std::uint8_t> &binaryPool,
                                                 bool appendToPool) override;

        bool get_is_stored_device_descriptor_object_pool_by_structure_label(
            std::shared_ptr<isobus::ControlFunction>,
            const std::vector<std::uint8_t> &,
            const std::vector<std::uint8_t> &) override { return false; }

        bool get_is_stored_device_descriptor_object_pool_by_localization_label(
            std::shared_ptr<isobus::ControlFunction>,
            const std::array<std::uint8_t, 7> &) override { return false; }

        bool get_is_enough_memory_available(std::uint32_t) override { return true; }
        void identify_task_controller(std::uint8_t) override {}

        void on_client_version_received(std::shared_ptr<isobus::ControlFunction> client, std::uint8_t version) override;
        void on_client_timeout(std::shared_ptr<isobus::ControlFunction> partner) override;

        void on_process_data_acknowledge(std::shared_ptr<isobus::ControlFunction> partner,
                                         std::uint16_t dataDescriptionIndex,
                                         std::uint16_t elementNumber,
                                         std::uint8_t errorCodesFromClient,
                                         ProcessDataCommands processDataCommand) override;

        bool on_value_command(std::shared_ptr<isobus::ControlFunction> partner,
                              std::uint16_t dataDescriptionIndex,
                              std::uint16_t elementNumber,
                              std::int32_t processDataValue,
                              std::uint8_t &errorCodes) override;

        void update_section_setpoints(const std::vector<bool> &states);
        void update_section_control_enabled(bool enabled);

        // Fallback / VT implement section control
        std::function<void(std::uint8_t sec, bool state)> onFallbackSectionChanged;
        void set_fallback_sections_count(std::uint8_t count);
        void set_fallback_section_state(std::uint8_t sec, bool state);

        // State access for UI & UDP bridge
        std::uint8_t get_total_sections() const;
        std::vector<bool> get_actual_section_states() const;
        std::vector<bool> get_setpoint_section_states() const;
        bool is_auto_mode() const { return autoMode; }

        ~TerminalTCServer() override;
        void install_safe_callbacks();
        void remove_safe_callbacks();
        static void safe_store_rx_message(const isobus::CANMessage &message, void *parentPointer);

    private:
        mutable std::recursive_mutex clientMutex;
        std::map<std::shared_ptr<isobus::ControlFunction>, TcClientState> clients;
        std::map<std::shared_ptr<isobus::ControlFunction>, std::queue<std::vector<std::uint8_t>>> uploadedPools;
        bool autoMode = true;

        std::uint8_t fallbackSections = 6;
        std::vector<bool> fallbackActualStates = { true, true, false, false, true, true };
        std::vector<bool> fallbackSetpoints = { true, true, false, false, true, true };
    };

    // AgOpenGPS UDP Bridge (WinSock2 implementation for Ports 8888 / 9999)
    class AogUdpBridge
    {
    public:
        AogUdpBridge();
        ~AogUdpBridge();

        bool start(std::uint16_t rxPort = 8888, std::uint16_t txPort = 9999);
        void stop();
        void poll();

        // Send section heartbeat packet (PGN 0xF0) to AgOpenGPS
        void send_heartbeat(bool autoMode, std::uint8_t numSections, const std::vector<bool> &actualStates);

        // Send remote switch / work switch command (PGN 0xEA) to AgOpenGPS
        void send_work_switch_command(bool workActive);

        // Send AutoSteer guidance command (PGN 0xFE) to AgIO / Steer Controller
        void send_autosteer_command(bool engaged, float steerAngleDeg, float speedKph);

        // Send hardware text banner (PGN 0xDD) to AgOpenGPS map
        void send_hardware_message(const std::string &text, std::uint8_t durationSec = 5, std::uint8_t colour = 1);

        // Status queries
        bool is_aog_active() const;
        float get_gps_speed_kph() const { return gpsSpeedKph.load(); }
        std::uint8_t get_gps_fix_quality() const { return gpsFixQuality.load(); }
        std::string get_subnet_string() const;
        void set_subnet(std::uint8_t o1, std::uint8_t o2, std::uint8_t o3);
        void set_subnet_from_string(const std::string &subnetStr);
        std::array<std::uint8_t, 3> get_subnet() const { return subnet; }
        std::uint32_t get_rx_packets() const { return rxPackets.load(); }
        std::uint32_t get_tx_packets() const { return txPackets.load(); }

        // Callback hooks
        std::function<void(const std::vector<bool>&)> onSectionStatesReceived;
        std::function<void(bool)> onSectionControlModeReceived;
        std::function<void(float speedKph, std::int32_t rawMmPerSec)> onSpeedReceived;
        std::function<void(const std::string&)> onFieldNameReceived;
        std::function<void(float totalWidthM, std::uint8_t numSections, const std::vector<float>& widths)> onToolGeometryReceived;

    private:
        static constexpr std::uint16_t PACKET_START = 0x8081;
        std::uint8_t calculate_crc(const std::uint8_t *data, std::size_t len);

        std::uintptr_t rxSocket = ~0ULL; // SOCKET descriptor for Port 8888
        std::uintptr_t rxSocketLoopback = ~0ULL; // SOCKET descriptor for Port 17777 (AgOpenGPS epAgIO direct loopback)
        std::array<std::uint8_t, 3> subnet = { 192, 168, 5 };
        std::uint16_t txPort = 9999;

        std::atomic<bool> running{ false };
        std::atomic<std::uint32_t> lastRxTimestamp{ 0 };
        std::atomic<float> gpsSpeedKph{ 0.0f };
        std::atomic<std::uint8_t> gpsFixQuality{ 0 };
        std::atomic<std::uint32_t> rxPackets{ 0 };
        std::atomic<std::uint32_t> txPackets{ 0 };
    };

    // Central Task Controller Manager combining TC Server, Tractor ECU & AgOpenGPS UDP
    class TaskControllerManager
    {
    public:
        TaskControllerManager();
        ~TaskControllerManager();

        bool initialize(std::uint8_t canChannelIndex = 0);
        void update();
        void shutdown();
        bool is_initialized() const { return tcServer != nullptr; }

        // Component references
        std::shared_ptr<TerminalTCServer> get_tc_server() const { return tcServer; }
        std::shared_ptr<AogUdpBridge> get_udp_bridge() const { return udpBridge; }

        // Live status for ImGui UI
        bool is_aog_connected() const;
        std::uint8_t get_num_sections() const;
        std::vector<bool> get_section_states() const;
        std::vector<bool> get_section_setpoints() const;
        float get_speed_kph() const;
        bool is_auto_mode() const;
        void set_aog_subnet(const std::string &subnetStr);
        void set_aog_subnet(std::uint8_t o1, std::uint8_t o2, std::uint8_t o3);
        std::array<std::uint8_t, 3> get_aog_subnet() const;
        std::string get_aog_subnet_string() const;

        // Force painting toggle (Manual Override: all sections ON & work switch active)
        void set_force_paint(bool force) { forcePaint = force; }
        bool is_force_paint() const { return forcePaint; }

        void set_section_actual(std::uint8_t sec, bool state);
        std::function<void(std::uint8_t sec, bool state)> onSectionStateChanged;
        std::function<void(float speedKph)> onSpeedChanged;

        const std::deque<std::string>& get_recent_logs() const;

    private:
        void log_message(const std::string &msg);

        std::shared_ptr<isobus::InternalControlFunction> tcCF;
        std::shared_ptr<isobus::InternalControlFunction> tecuCF;
        std::shared_ptr<TerminalTCServer> tcServer;
        std::shared_ptr<AogUdpBridge> udpBridge;

        std::unique_ptr<isobus::ControlFunctionFunctionalities> tcFunctionalities;
        std::unique_ptr<isobus::ControlFunctionFunctionalities> tecuFunctionalities;
        std::unique_ptr<isobus::SpeedMessagesInterface> speedMessagesInterface;
        std::unique_ptr<isobus::TimeDateInterface> timeDateInterface;

        std::uint32_t lastHeartbeatMs = 0;
        bool forcePaint = false;
        mutable std::mutex logMutex;
        std::deque<std::string> recentLogs;
    };
}

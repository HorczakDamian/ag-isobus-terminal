#pragma once

#include "isobus/isobus/can_control_function.hpp"
#include "isobus/isobus/can_internal_control_function.hpp"
#include "isobus/isobus/can_message.hpp"
#include "isobus/isobus/can_network_manager.hpp"
#include "isobus/isobus/isobus_virtual_terminal_base.hpp"
#include "isobus/isobus/isobus_virtual_terminal_objects.hpp"
#include "isobus/isobus/isobus_virtual_terminal_server_managed_working_set.hpp"

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace isobus_vt
{
    struct DiscoveredAuxInputDevice
    {
        std::uint64_t rawName = 0;
        std::uint8_t address = 0xFF;
        std::uint16_t modelCode = 0;
        bool ready = false;
        std::uint32_t lastSeenTimestampMs = 0;
        std::string nameString;
    };

    struct DiscoveredAuxFunction
    {
        std::uint16_t functionObjectId = 0;
        isobus::AuxiliaryFunctionType2::FunctionType functionType = isobus::AuxiliaryFunctionType2::FunctionType::BooleanNonLatchingIncreaseValue;
        std::uint16_t designatorObjectId = 0;
        std::string description;
        bool singleAssignment = false;
        bool criticalControl = false;
    };

    struct AuxAssignment
    {
        std::uint64_t implementName = 0;
        std::uint16_t functionObjectId = 0;
        std::uint64_t inputDeviceName = 0;
        std::uint16_t inputObjectId = 0;
        std::uint8_t functionType = 0;
        std::string functionDescription;
        std::string inputDescription;
    };

    class AuxManager
    {
    public:
        AuxManager(std::shared_ptr<isobus::InternalControlFunction> vtServerCF);
        ~AuxManager() = default;

        void initialize();
        void update();

        // CAN frame / message processing
        void process_can_message(const isobus::CANMessage &message);

        // Discovered lists
        std::vector<DiscoveredAuxInputDevice> get_input_devices();
        std::vector<DiscoveredAuxFunction> get_available_functions(std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> activeWs);
        std::vector<AuxAssignment> get_assignments();

        // Assignment actions
        bool assign_function(std::shared_ptr<isobus::ControlFunction> implementCF,
                             std::uint64_t implementName,
                             std::uint16_t functionObjectId,
                             std::uint64_t inputDeviceName,
                             std::uint16_t inputObjectId,
                             std::uint8_t functionType,
                             const std::string &fnDesc,
                             const std::string &inDesc);

        bool unassign_function(std::shared_ptr<isobus::ControlFunction> implementCF,
                               std::uint64_t implementName,
                               std::uint16_t functionObjectId);

        // Virtual Joystick Simulator (for immediate testing without physical AUX joystick)
        void set_virtual_joystick_enabled(bool enabled);
        bool get_virtual_joystick_enabled() const;
        std::uint64_t get_virtual_joystick_name() const;

        void simulate_virtual_input(std::uint16_t inputObjectId, std::uint16_t value1, std::uint16_t value2 = 0);

        static const char* get_function_type_name(isobus::AuxiliaryFunctionType2::FunctionType type);

    private:
        std::shared_ptr<isobus::InternalControlFunction> vtServerCF;
        std::shared_ptr<isobus::InternalControlFunction> virtualJoystickCF;

        mutable std::mutex auxMutex;
        std::map<std::uint64_t, DiscoveredAuxInputDevice> inputDevices;
        std::vector<AuxAssignment> assignments;

        bool virtualJoystickEnabled = true;
        std::uint32_t lastVirtualMaintTimestamp = 0;

        void send_virtual_joystick_maintenance();
        void handle_preferred_assignment(const isobus::CANMessage &message);
    };
}

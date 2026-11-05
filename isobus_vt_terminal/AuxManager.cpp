#include "AuxManager.hpp"
#include "isobus/isobus/can_general_parameter_group_numbers.hpp"
#include "isobus/isobus/can_stack_logger.hpp"
#include "isobus/utility/system_timing.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace isobus_vt
{
    // ISO 11783-6 Virtual Terminal Function Codes for AUX
    constexpr std::uint8_t VT_FUNC_PREFERRED_ASSIGNMENT = 0x22;
    constexpr std::uint8_t VT_FUNC_AUX_INPUT_MAINTENANCE = 0x23;
    constexpr std::uint8_t VT_FUNC_AUX_ASSIGNMENT_TYPE_TWO = 0x24;
    constexpr std::uint8_t VT_FUNC_AUX_INPUT_STATUS_TYPE_TWO = 0x26;

    AuxManager::AuxManager(std::shared_ptr<isobus::InternalControlFunction> vtServerCF) :
        vtServerCF(vtServerCF)
    {
    }

    static void can_message_callback(const isobus::CANMessage &message, void *parent)
    {
        if (parent)
        {
            static_cast<AuxManager*>(parent)->process_can_message(message);
        }
    }

    void AuxManager::initialize()
    {
        // Setup a Virtual Joystick InternalControlFunction so operator can test AUX immediately
        isobus::NAME joystickName(0);
        joystickName.set_arbitrary_address_capable(true);
        joystickName.set_industry_group(2); // Agriculture
        joystickName.set_device_class(0);
        joystickName.set_function_code(static_cast<std::uint8_t>(isobus::NAME::Function::OperatorControlsMachineSpecific));
        joystickName.set_identity_number(0x7777);
        joystickName.set_manufacturer_code(1407); // AEF Manufacturer Code
        joystickName.set_ecu_instance(0);
        joystickName.set_function_instance(0);

        virtualJoystickCF = isobus::CANNetworkManager::CANNetwork.create_internal_control_function(joystickName, 0, 0x82);

        // Listen for ECU to VT messages (PGN 0xE700) to detect AUX input devices and preferred assignments
        isobus::CANNetworkManager::CANNetwork.add_any_control_function_parameter_group_number_callback(
            static_cast<std::uint32_t>(isobus::CANLibParameterGroupNumber::ECUtoVirtualTerminal),
            can_message_callback,
            this
        );
    }

    void AuxManager::update()
    {
        if (!enabled) return;
        if (virtualJoystickEnabled && virtualJoystickCF && virtualJoystickCF->get_address_valid())
        {
            if (isobus::SystemTiming::time_expired_ms(lastVirtualMaintTimestamp, 1000))
            {
                lastVirtualMaintTimestamp = isobus::SystemTiming::get_timestamp_ms();
                send_virtual_joystick_maintenance();
            }
        }
    }

    void AuxManager::send_virtual_joystick_maintenance()
    {
        if (!virtualJoystickCF || !virtualJoystickCF->get_address_valid())
            return;

        std::array<std::uint8_t, 8> data = {
            VT_FUNC_AUX_INPUT_MAINTENANCE, // 0x23
            0x01, 0x00, // model code = 1
            0x01,       // ready = 1
            0xFF, 0xFF, 0xFF, 0xFF
        };

        isobus::CANNetworkManager::CANNetwork.send_can_message(
            static_cast<std::uint32_t>(isobus::CANLibParameterGroupNumber::ECUtoVirtualTerminal),
            data.data(),
            static_cast<std::uint32_t>(data.size()),
            virtualJoystickCF
        );

        // Self-register in inputDevices
        std::lock_guard<std::mutex> lock(auxMutex);
        auto rawName = virtualJoystickCF->get_NAME().get_full_name();
        auto &dev = inputDevices[rawName];
        dev.rawName = rawName;
        dev.address = virtualJoystickCF->get_address();
        dev.modelCode = 1;
        dev.ready = true;
        dev.lastSeenTimestampMs = isobus::SystemTiming::get_timestamp_ms();
        dev.nameString = "Virtual VT Joystick (Built-in)";
    }

    void AuxManager::process_can_message(const isobus::CANMessage &message)
    {
        if (message.get_data_length() < 1)
            return;

        std::uint8_t funcCode = message.get_uint8_at(0);

        if (funcCode == VT_FUNC_AUX_INPUT_MAINTENANCE)
        {
            if (message.get_data_length() >= 4 && message.get_source_control_function())
            {
                std::uint16_t modelCode = message.get_uint16_at(1);
                bool ready = (1 == message.get_uint8_at(3));
                auto rawName = message.get_source_control_function()->get_NAME().get_full_name();

                std::lock_guard<std::mutex> lock(auxMutex);
                auto &dev = inputDevices[rawName];
                dev.rawName = rawName;
                dev.address = message.get_source_control_function()->get_address();
                dev.modelCode = modelCode;
                dev.ready = ready;
                dev.lastSeenTimestampMs = isobus::SystemTiming::get_timestamp_ms();

                if (dev.nameString.empty())
                {
                    std::ostringstream ss;
                    ss << "Input Device (0x" << std::hex << std::uppercase << rawName << ")";
                    dev.nameString = ss.str();
                }
            }
        }
        else if (funcCode == VT_FUNC_PREFERRED_ASSIGNMENT)
        {
            handle_preferred_assignment(message);
        }
    }

    void AuxManager::handle_preferred_assignment(const isobus::CANMessage &message)
    {
        if (!vtServerCF || !vtServerCF->get_address_valid())
            return;

        LOG_INFO("[AUX-N]: Received Preferred Assignment Command from ECU 0x%02X, answering ACK OK",
                 message.get_source_control_function() ? message.get_source_control_function()->get_address() : 0);

        // ISO 11783-6: VT answers with PreferredAssignmentCommand (0x22), Error byte = 0x00 (Accepted)
        std::array<std::uint8_t, 8> response = {
            VT_FUNC_PREFERRED_ASSIGNMENT, // 0x22
            0x00, // Error: 0x00 = No error (Accepted)
            0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
        };

        isobus::CANNetworkManager::CANNetwork.send_can_message(
            static_cast<std::uint32_t>(isobus::CANLibParameterGroupNumber::VirtualTerminalToECU),
            response.data(),
            static_cast<std::uint32_t>(response.size()),
            vtServerCF,
            message.get_source_control_function()
        );
    }

    std::vector<DiscoveredAuxInputDevice> AuxManager::get_input_devices()
    {
        std::lock_guard<std::mutex> lock(auxMutex);
        std::vector<DiscoveredAuxInputDevice> result;
        for (const auto &pair : inputDevices)
        {
            result.push_back(pair.second);
        }
        return result;
    }

    std::vector<DiscoveredAuxFunction> AuxManager::get_available_functions(std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> activeWs)
    {
        std::vector<DiscoveredAuxFunction> functions;
        if (!activeWs)
            return functions;

        const auto &tree = activeWs->get_object_tree();
        for (const auto &objPair : tree)
        {
            if (objPair.second && objPair.second->get_object_type() == isobus::VirtualTerminalObjectType::AuxiliaryFunctionType2)
            {
                auto auxFn = std::dynamic_pointer_cast<isobus::AuxiliaryFunctionType2>(objPair.second);
                if (auxFn)
                {
                    DiscoveredAuxFunction fn;
                    fn.functionObjectId = auxFn->get_id();
                    fn.functionType = auxFn->get_function_type();
                    fn.singleAssignment = auxFn->get_function_attribute(isobus::AuxiliaryFunctionType2::FunctionAttribute::SingleAssignment);
                    fn.criticalControl = auxFn->get_function_attribute(isobus::AuxiliaryFunctionType2::FunctionAttribute::CriticalControl);

                    // Check if child has a designator or string
                    if (auxFn->get_number_children() > 0)
                    {
                        fn.designatorObjectId = auxFn->get_child_id(0);
                        auto childObj = activeWs->get_object_by_id(fn.designatorObjectId);
                        if (childObj && childObj->get_object_type() == isobus::VirtualTerminalObjectType::OutputString)
                        {
                            auto outStr = std::dynamic_pointer_cast<isobus::OutputString>(childObj);
                            if (outStr)
                            {
                                fn.description = outStr->displayed_value(tree);
                            }
                        }
                    }

                    if (fn.description.empty())
                    {
                        std::ostringstream ss;
                        ss << "Aux Fn #" << fn.functionObjectId << " (" << get_function_type_name(fn.functionType) << ")";
                        fn.description = ss.str();
                    }

                    functions.push_back(fn);
                }
            }
        }
        return functions;
    }

    std::vector<AuxAssignment> AuxManager::get_assignments()
    {
        std::lock_guard<std::mutex> lock(auxMutex);
        return assignments;
    }

    bool AuxManager::assign_function(std::shared_ptr<isobus::ControlFunction> implementCF,
                                    std::uint64_t implementName,
                                    std::uint16_t functionObjectId,
                                    std::uint64_t inputDeviceName,
                                    std::uint16_t inputObjectId,
                                    std::uint8_t functionType,
                                    const std::string &fnDesc,
                                    const std::string &inDesc)
    {
        if (!vtServerCF || !implementCF)
            return false;

        // ISO 11783-6: Auxiliary Assignment Type 2 Command (0x24) - 16 bytes
        std::vector<std::uint8_t> data(16, 0xFF);
        data[0] = VT_FUNC_AUX_ASSIGNMENT_TYPE_TWO; // 0x24
        data[1] = static_cast<std::uint8_t>(functionObjectId & 0xFF);
        data[2] = static_cast<std::uint8_t>((functionObjectId >> 8) & 0xFF);

        // 64-bit NAME of input unit
        for (int i = 0; i < 8; ++i)
        {
            data[3 + i] = static_cast<std::uint8_t>((inputDeviceName >> (8 * i)) & 0xFF);
        }

        data[11] = static_cast<std::uint8_t>(inputObjectId & 0xFF);
        data[12] = static_cast<std::uint8_t>((inputObjectId >> 8) & 0xFF);
        data[13] = functionType;
        data[14] = 0xFF;
        data[15] = 0xFF;

        bool sent = isobus::CANNetworkManager::CANNetwork.send_can_message(
            static_cast<std::uint32_t>(isobus::CANLibParameterGroupNumber::VirtualTerminalToECU),
            data.data(),
            static_cast<std::uint32_t>(data.size()),
            vtServerCF,
            implementCF
        );

        if (sent)
        {
            std::lock_guard<std::mutex> lock(auxMutex);
            // Remove existing assignment for same function if present
            assignments.erase(std::remove_if(assignments.begin(), assignments.end(),
                [implementName, functionObjectId](const AuxAssignment &a) {
                    return a.implementName == implementName && a.functionObjectId == functionObjectId;
                }), assignments.end());

            AuxAssignment newAssign;
            newAssign.implementName = implementName;
            newAssign.functionObjectId = functionObjectId;
            newAssign.inputDeviceName = inputDeviceName;
            newAssign.inputObjectId = inputObjectId;
            newAssign.functionType = functionType;
            newAssign.functionDescription = fnDesc;
            newAssign.inputDescription = inDesc;
            assignments.push_back(newAssign);

            LOG_INFO("[AUX-N]: Assigned function %u to device 0x%llX input %u",
                     functionObjectId, inputDeviceName, inputObjectId);
        }

        return sent;
    }

    bool AuxManager::unassign_function(std::shared_ptr<isobus::ControlFunction> implementCF,
                                      std::uint64_t implementName,
                                      std::uint16_t functionObjectId)
    {
        if (!vtServerCF || !implementCF)
            return false;

        // Removing assignment: functionType = 31 (ReservedRangeEnd)
        std::vector<std::uint8_t> data(16, 0xFF);
        data[0] = VT_FUNC_AUX_ASSIGNMENT_TYPE_TWO; // 0x24
        data[1] = static_cast<std::uint8_t>(functionObjectId & 0xFF);
        data[2] = static_cast<std::uint8_t>((functionObjectId >> 8) & 0xFF);
        // Device name, input object ID all 0xFF
        data[13] = 31; // Remove assignment code

        bool sent = isobus::CANNetworkManager::CANNetwork.send_can_message(
            static_cast<std::uint32_t>(isobus::CANLibParameterGroupNumber::VirtualTerminalToECU),
            data.data(),
            static_cast<std::uint32_t>(data.size()),
            vtServerCF,
            implementCF
        );

        if (sent)
        {
            std::lock_guard<std::mutex> lock(auxMutex);
            assignments.erase(std::remove_if(assignments.begin(), assignments.end(),
                [implementName, functionObjectId](const AuxAssignment &a) {
                    return a.implementName == implementName && a.functionObjectId == functionObjectId;
                }), assignments.end());

            LOG_INFO("[AUX-N]: Unassigned function %u", functionObjectId);
        }

        return sent;
    }

    void AuxManager::set_virtual_joystick_enabled(bool enabled)
    {
        virtualJoystickEnabled = enabled;
    }

    bool AuxManager::get_virtual_joystick_enabled() const
    {
        return virtualJoystickEnabled;
    }

    std::uint64_t AuxManager::get_virtual_joystick_name() const
    {
        if (virtualJoystickCF)
            return virtualJoystickCF->get_NAME().get_full_name();
        return 0;
    }

    void AuxManager::simulate_virtual_input(std::uint16_t inputObjectId, std::uint16_t value1, std::uint16_t value2)
    {
        if (!virtualJoystickCF || !virtualJoystickCF->get_address_valid())
            return;

        std::array<std::uint8_t, 8> data = {
            VT_FUNC_AUX_INPUT_STATUS_TYPE_TWO, // 0x26
            static_cast<std::uint8_t>(inputObjectId & 0xFF),
            static_cast<std::uint8_t>((inputObjectId >> 8) & 0xFF),
            static_cast<std::uint8_t>(value1 & 0xFF),
            static_cast<std::uint8_t>((value1 >> 8) & 0xFF),
            static_cast<std::uint8_t>(value2 & 0xFF),
            static_cast<std::uint8_t>((value2 >> 8) & 0xFF),
            0x00 // normal operating state
        };

        isobus::CANNetworkManager::CANNetwork.send_can_message(
            static_cast<std::uint32_t>(isobus::CANLibParameterGroupNumber::ECUtoVirtualTerminal),
            data.data(),
            static_cast<std::uint32_t>(data.size()),
            virtualJoystickCF
        );
    }

    const char* AuxManager::get_function_type_name(isobus::AuxiliaryFunctionType2::FunctionType type)
    {
        using FT = isobus::AuxiliaryFunctionType2::FunctionType;
        switch (type)
        {
            case FT::BooleanLatchingOnOff: return "Latching Switch (On/Off)";
            case FT::Analouge: return "Analog (Maintains Position)";
            case FT::BooleanNonLatchingIncreaseValue: return "Momentary Button (Push)";
            case FT::AnalougeReturnTo50Percent: return "Analog 2-Way (Center 50%)";
            case FT::AnalougeReturnTo0PercentIncreaseValue: return "Analog 1-Way (Return 0%)";
            case FT::DualBooleanBothLatching: return "3-Pos Switch (Latching)";
            case FT::DualBooleanBothNonLatching: return "3-Pos Switch (Return Center)";
            case FT::DualBooleanLatchingUp: return "3-Pos Switch (Latch Up, Mom Down)";
            case FT::DualBooleanLatchingDown: return "3-Pos Switch (Latch Down, Mom Up)";
            case FT::QuadratureBooleanNonLatching: return "4-Way Hat Switch";
            case FT::QuadratureAnalougeReturnTo50Percent: return "2-Axis Joystick (Center 50%)";
            default: return "Custom Function Type";
        }
    }
}

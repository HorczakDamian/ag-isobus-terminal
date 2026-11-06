#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include "TaskControllerManager.hpp"
#include "TerminalSettings.hpp"
#include "AmazoneBridge.hpp"
#include "DdiExplorerManager.hpp"
#include "FieldNavigation.hpp"
#include "TcBasReportManager.hpp"
#include "TractorEcuManager.hpp"
#include "isobus/isobus/can_network_manager.hpp"
#include "isobus/isobus/can_stack_logger.hpp"
#include "isobus/isobus/isobus_preferred_addresses.hpp"
#include "isobus/utility/system_timing.hpp"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <sstream>

#pragma comment(lib, "ws2_32.lib")

namespace isobus_vt
{
static void safe_update_tc(isobus::TaskControllerServer* tc)
{
    if (!tc) return;
    __try
    {
        tc->update();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

    // =========================================================================
    // TcClientState Implementation
    // =========================================================================
    void TcClientState::set_number_of_sections(std::uint8_t number)
    {
        numberOfSections = number;
        setpoints.assign(number, static_cast<std::uint8_t>(SectionState::Off));
        actualStates.assign(number, static_cast<std::uint8_t>(SectionState::Off));
        sectionToElement.assign(number, 0);
    }

    std::uint8_t TcClientState::get_number_of_sections() const
    {
        return numberOfSections;
    }

    void TcClientState::set_section_setpoint(std::uint8_t section, std::uint8_t state)
    {
        if (section < setpoints.size())
            setpoints[section] = state;
    }

    std::uint8_t TcClientState::get_section_setpoint(std::uint8_t section) const
    {
        return (section < setpoints.size()) ? setpoints[section] : static_cast<std::uint8_t>(SectionState::Off);
    }

    void TcClientState::set_section_actual(std::uint8_t section, std::uint8_t state)
    {
        if (section < actualStates.size())
            actualStates[section] = state;
    }

    std::uint8_t TcClientState::get_section_actual(std::uint8_t section) const
    {
        return (section < actualStates.size()) ? actualStates[section] : static_cast<std::uint8_t>(SectionState::Off);
    }

    void TcClientState::set_element_for_section(std::uint8_t section, std::uint16_t elementNumber)
    {
        if (section < sectionToElement.size())
            sectionToElement[section] = elementNumber;
    }

    std::uint16_t TcClientState::get_element_for_section(std::uint8_t section) const
    {
        return (section < sectionToElement.size()) ? sectionToElement[section] : 0;
    }

    // =========================================================================
    // TerminalTCServer Implementation
    // =========================================================================
    TerminalTCServer::TerminalTCServer(std::shared_ptr<isobus::InternalControlFunction> tcCF,
                                       isobus::TaskControllerServer::TaskControllerVersion version)
        : TaskControllerServer(tcCF,
                               1,  // 1 Boom (AgOpenGPS default)
                               64, // 64 Sections
                               64, // 64 Channels
                               isobus::TaskControllerOptions().with_implement_section_control(),
                               version)
    {
    }

    TerminalTCServer::~TerminalTCServer()
    {
        remove_safe_callbacks();
    }

    void TerminalTCServer::install_safe_callbacks()
    {
        isobus::CANNetworkManager::CANNetwork.remove_global_parameter_group_number_callback(0x00CB00, store_rx_message, this);
        isobus::CANNetworkManager::CANNetwork.remove_global_parameter_group_number_callback(0x00FE0D, store_rx_message, this);

        isobus::CANNetworkManager::CANNetwork.add_global_parameter_group_number_callback(0x00CB00, safe_store_rx_message, this);
        isobus::CANNetworkManager::CANNetwork.add_global_parameter_group_number_callback(0x00FE0D, safe_store_rx_message, this);
    }

    void TerminalTCServer::remove_safe_callbacks()
    {
        isobus::CANNetworkManager::CANNetwork.remove_global_parameter_group_number_callback(0x00CB00, safe_store_rx_message, this);
        isobus::CANNetworkManager::CANNetwork.remove_global_parameter_group_number_callback(0x00FE0D, safe_store_rx_message, this);
    }

    void TerminalTCServer::safe_store_rx_message(const isobus::CANMessage &message, void *parentPointer)
    {
        if (!message.get_source_control_function())
        {
            return;
        }
        store_rx_message(message, parentPointer);
    }

    bool TerminalTCServer::store_device_descriptor_object_pool(std::shared_ptr<isobus::ControlFunction> partnerCF,
                                                               const std::vector<std::uint8_t> &binaryPool,
                                                               bool /*appendToPool*/)
    {
        std::lock_guard<std::recursive_mutex> lock(clientMutex);
        if (uploadedPools.find(partnerCF) == uploadedPools.end())
        {
            uploadedPools[partnerCF] = std::queue<std::vector<std::uint8_t>>();
        }
        uploadedPools[partnerCF].push(binaryPool);
        return true;
    }

    bool TerminalTCServer::activate_object_pool(std::shared_ptr<isobus::ControlFunction> partnerCF,
                                                ObjectPoolActivationError &activationError,
                                                ObjectPoolErrorCodes &objectPoolError,
                                                std::uint16_t &parentObjectIDOfFaultyObject,
                                                std::uint16_t &faultyObjectID)
    {
        std::lock_guard<std::recursive_mutex> lock(clientMutex);
        activationError = ObjectPoolActivationError::NoErrors;
        objectPoolError = ObjectPoolErrorCodes::NoErrors;
        parentObjectIDOfFaultyObject = isobus::NULL_OBJECT_ID;
        faultyObjectID = isobus::NULL_OBJECT_ID;

        if (uploadedPools.find(partnerCF) == uploadedPools.end())
        {
            activationError = ObjectPoolActivationError::AnyOtherError;
            return false;
        }

        auto state = TcClientState();
        state.get_pool().set_task_controller_compatibility_level(
            static_cast<std::uint8_t>(TaskControllerVersion::SecondEditionDraft));

        bool deserialized = false;
        while (!uploadedPools[partnerCF].empty())
        {
            auto binaryPool = uploadedPools[partnerCF].front();
            uploadedPools[partnerCF].pop();
            deserialized = state.get_pool().deserialize_binary_object_pool(
                binaryPool.data(), static_cast<std::uint32_t>(binaryPool.size()), partnerCF->get_NAME());
        }

        if (deserialized)
        {
            auto implement = isobus::DeviceDescriptorObjectPoolHelper::get_implement_geometry(state.get_pool());
            std::uint8_t numSections = 0;
            std::vector<std::uint16_t> sectionElements;

            for (const auto &boom : implement.booms)
            {
                for (const auto &subBoom : boom.subBooms)
                {
                    for (const auto &sec : subBoom.sections)
                    {
                        numSections++;
                        sectionElements.push_back(sec.elementNumber);
                    }
                }
                for (const auto &sec : boom.sections)
                {
                    numSections++;
                    sectionElements.push_back(sec.elementNumber);
                }
            }

            // Fallback: If implement geometry didn't list sections, search DeviceElement objects
            if (numSections == 0)
            {
                for (std::uint32_t i = 0; i < state.get_pool().size(); ++i)
                {
                    auto obj = state.get_pool().get_object_by_index(i);
                    if (obj && obj->get_object_type() == isobus::task_controller_object::ObjectTypes::DeviceElement)
                    {
                        auto elem = std::dynamic_pointer_cast<isobus::task_controller_object::DeviceElementObject>(obj);
                        if (elem && elem->get_type() == isobus::task_controller_object::DeviceElementObject::Type::Section)
                        {
                            numSections++;
                            sectionElements.push_back(elem->get_element_number());
                        }
                    }
                }
            }

            state.set_number_of_sections(numSections);
            for (std::uint8_t i = 0; i < numSections && i < sectionElements.size(); ++i)
            {
                state.set_element_for_section(i, sectionElements[i]);
            }

            // Map standard DDIs
            for (std::uint32_t i = 0; i < state.get_pool().size(); ++i)
            {
                auto obj = state.get_pool().get_object_by_index(i);
                if (obj && obj->get_object_type() == isobus::task_controller_object::ObjectTypes::DeviceProcessData)
                {
                    auto dpd = std::dynamic_pointer_cast<isobus::task_controller_object::DeviceProcessDataObject>(obj);
                    if (dpd)
                    {
                        auto ddi = static_cast<isobus::DataDescriptionIndex>(dpd->get_ddi());
                        state.set_element_for_ddi(ddi, dpd->get_object_id());
                    }
                }
            }

            clients[partnerCF] = std::move(state);
            return true;
        }

        activationError = ObjectPoolActivationError::ThereAreErrorsInTheDDOP;
        return false;
    }

    bool TerminalTCServer::deactivate_object_pool(std::shared_ptr<isobus::ControlFunction> partnerCF)
    {
        std::lock_guard<std::recursive_mutex> lock(clientMutex);
        clients.erase(partnerCF);
        uploadedPools.erase(partnerCF);
        return true;
    }

    bool TerminalTCServer::delete_device_descriptor_object_pool(std::shared_ptr<isobus::ControlFunction> partnerCF,
                                                               ObjectPoolDeletionErrors &)
    {
        std::lock_guard<std::recursive_mutex> lock(clientMutex);
        clients.erase(partnerCF);
        uploadedPools.erase(partnerCF);
        return true;
    }

    void TerminalTCServer::on_client_version_received(std::shared_ptr<isobus::ControlFunction>, std::uint8_t)
    {
    }

    void TerminalTCServer::on_client_timeout(std::shared_ptr<isobus::ControlFunction> partner)
    {
        std::lock_guard<std::recursive_mutex> lock(clientMutex);
        clients.erase(partner);
        uploadedPools.erase(partner);
    }

    void TerminalTCServer::on_process_data_acknowledge(std::shared_ptr<isobus::ControlFunction>,
                                                      std::uint16_t,
                                                      std::uint16_t,
                                                      std::uint8_t,
                                                      ProcessDataCommands)
    {
    }

    bool TerminalTCServer::on_value_command(std::shared_ptr<isobus::ControlFunction> partner,
                                           std::uint16_t dataDescriptionIndex,
                                           std::uint16_t elementNumber,
                                           std::int32_t processDataValue,
                                           std::uint8_t &/*errorCodes*/)
    {
        std::lock_guard<std::recursive_mutex> lock(clientMutex);
        DdiExplorerManager::get_instance().update_ddi_value(dataDescriptionIndex, processDataValue, partner ? partner->get_address() : 0, elementNumber);
        auto it = clients.find(partner);
        if (it == clients.end()) return true;

        // DDI 161: ActualCondensedWorkState1_16
        auto ddi = static_cast<isobus::DataDescriptionIndex>(dataDescriptionIndex);
        if (ddi >= isobus::DataDescriptionIndex::ActualCondensedWorkState1_16 &&
            ddi <= isobus::DataDescriptionIndex::ActualCondensedWorkState49_64)
        {
            std::uint8_t offset = static_cast<std::uint8_t>(dataDescriptionIndex -
                static_cast<std::uint16_t>(isobus::DataDescriptionIndex::ActualCondensedWorkState1_16)) * 16;
            for (std::uint8_t i = 0; i < 16; ++i)
            {
                std::uint8_t state = (processDataValue >> (2 * i)) & 0x03;
                it->second.set_section_actual(offset + i, state);
            }
        }
        else if (ddi == isobus::DataDescriptionIndex::ActualWorkState)
        {
            // Per-element actual work state (DDI 141)
            for (std::uint8_t s = 0; s < it->second.get_number_of_sections(); ++s)
            {
                if (it->second.get_element_for_section(s) == elementNumber)
                {
                    it->second.set_section_actual(s, (processDataValue == 1) ? 
                        static_cast<std::uint8_t>(SectionState::On) : static_cast<std::uint8_t>(SectionState::Off));
                    break;
                }
            }
        }
        return true;
    }

    void TerminalTCServer::set_fallback_sections_count(std::uint8_t count)
    {
        std::lock_guard<std::recursive_mutex> lock(clientMutex);
        fallbackSections = count;
        fallbackActualStates.resize(count, true);
        fallbackSetpoints.resize(count, true);
    }

    void TerminalTCServer::set_fallback_section_state(std::uint8_t sec, bool state)
    {
        std::lock_guard<std::recursive_mutex> lock(clientMutex);
        if (sec < fallbackActualStates.size())
            fallbackActualStates[sec] = state;
        if (sec < fallbackSetpoints.size())
            fallbackSetpoints[sec] = state;

        for (auto &pair : clients)
        {
            if (sec < pair.second.get_number_of_sections())
            {
                pair.second.set_section_actual(sec, state ? static_cast<std::uint8_t>(SectionState::On) : static_cast<std::uint8_t>(SectionState::Off));
                pair.second.set_section_setpoint(sec, state ? static_cast<std::uint8_t>(SectionState::On) : static_cast<std::uint8_t>(SectionState::Off));
            }
        }
    }

    void TerminalTCServer::update_section_setpoints(const std::vector<bool> &states)
    {
        std::lock_guard<std::recursive_mutex> lock(clientMutex);
        for (std::size_t i = 0; i < fallbackSections && i < states.size(); ++i)
        {
            if (fallbackSetpoints[i] != states[i])
            {
                fallbackSetpoints[i] = states[i];
                fallbackActualStates[i] = states[i];
                if (onFallbackSectionChanged)
                    onFallbackSectionChanged(static_cast<std::uint8_t>(i), states[i]);
            }
        }

        for (auto &pair : clients)
        {
            auto &clientState = pair.second;
            std::uint8_t n = clientState.get_number_of_sections();
            if (n == 0) continue;

            bool changed = false;
            for (std::uint8_t i = 0; i < n && i < states.size(); ++i)
            {
                std::uint8_t cur = clientState.get_section_setpoint(i);
                std::uint8_t next = states[i] ? static_cast<std::uint8_t>(SectionState::On)
                                             : static_cast<std::uint8_t>(SectionState::Off);
                if (cur != next)
                {
                    clientState.set_section_setpoint(i, next);
                    // CRITICAL: Mirror setpoint directly to actual work state so AgOpenGPS receives confirmation
                    // in Heartbeat (PGN 0xF0) and turns sections GREEN to paint!
                    clientState.set_section_actual(i, next);
                    changed = true;
                }
            }

            if (changed && autoMode)
            {
                // Send condensed setpoint (DDI 290)
                std::uint32_t val1_16 = 0;
                for (std::uint8_t i = 0; i < std::min<std::uint8_t>(16, n); ++i)
                {
                    val1_16 |= (clientState.get_section_setpoint(i) << (2 * i));
                }

                std::uint16_t ddi290 = static_cast<std::uint16_t>(isobus::DataDescriptionIndex::SetpointCondensedWorkState1_16);
                std::uint16_t ddi161 = static_cast<std::uint16_t>(isobus::DataDescriptionIndex::ActualCondensedWorkState1_16);

                if (clientState.has_element_for_ddi(static_cast<isobus::DataDescriptionIndex>(ddi290)))
                {
                    send_set_value(pair.first, ddi290, clientState.get_element_for_ddi(static_cast<isobus::DataDescriptionIndex>(ddi290)), val1_16);
                }
                else if (clientState.has_element_for_ddi(static_cast<isobus::DataDescriptionIndex>(ddi161)))
                {
                    send_set_value(pair.first, ddi161, clientState.get_element_for_ddi(static_cast<isobus::DataDescriptionIndex>(ddi161)), val1_16);
                }
            }
        }
    }

    void TerminalTCServer::update_section_control_enabled(bool enabled)
    {
        std::lock_guard<std::recursive_mutex> lock(clientMutex);
        autoMode = enabled;
        for (auto &pair : clients)
        {
            pair.second.set_section_control_enabled(enabled);
        }
    }

    std::uint8_t TerminalTCServer::get_total_sections() const
    {
        std::lock_guard<std::recursive_mutex> lock(clientMutex);
        std::uint8_t total = 0;
        for (const auto &pair : clients)
            total = std::max(total, pair.second.get_number_of_sections());
        if (total == 0)
            total = fallbackSections;
        return total;
    }

    std::vector<bool> TerminalTCServer::get_actual_section_states() const
    {
        std::lock_guard<std::recursive_mutex> lock(clientMutex);
        for (const auto &pair : clients)
        {
            std::uint8_t n = pair.second.get_number_of_sections();
            if (n > 0)
            {
                std::vector<bool> res(n, false);
                for (std::uint8_t i = 0; i < n; ++i)
                    res[i] = (pair.second.get_section_actual(i) == static_cast<std::uint8_t>(SectionState::On));
                return res;
            }
        }
        return fallbackActualStates;
    }

    std::vector<bool> TerminalTCServer::get_setpoint_section_states() const
    {
        std::lock_guard<std::recursive_mutex> lock(clientMutex);
        for (const auto &pair : clients)
        {
            std::uint8_t n = pair.second.get_number_of_sections();
            if (n > 0)
            {
                std::vector<bool> res(n, false);
                for (std::uint8_t i = 0; i < n; ++i)
                    res[i] = (pair.second.get_section_setpoint(i) == static_cast<std::uint8_t>(SectionState::On));
                return res;
            }
        }
        return fallbackSetpoints;
    }

    // =========================================================================
    // AogUdpBridge Implementation
    // =========================================================================
    AogUdpBridge::AogUdpBridge()
    {
    }

    AogUdpBridge::~AogUdpBridge()
    {
        stop();
    }

    std::uint8_t AogUdpBridge::calculate_crc(const std::uint8_t *data, std::size_t len)
    {
        std::uint32_t sum = 0;
        for (std::size_t i = 0; i < len; ++i)
            sum += data[i];
        return static_cast<std::uint8_t>(sum & 0xFF);
    }

    bool AogUdpBridge::start(std::uint16_t rxPort, std::uint16_t txPortNum)
    {
        stop();
        txPort = txPortNum;

        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
            return false;

        // 1. Główne gniazdo UDP Port 8888 (Odbiór od AgIO / sieci lokalnej)
        SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s != INVALID_SOCKET)
        {
            BOOL opt = TRUE;
            setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));
            setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&opt), sizeof(opt));

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
            BOOL bNewBehavior = FALSE;
            DWORD dwBytesReturned = 0;
            WSAIoctl(s, SIO_UDP_CONNRESET, &bNewBehavior, sizeof(bNewBehavior), NULL, 0, &dwBytesReturned, NULL, NULL);

            u_long mode = 1;
            ioctlsocket(s, FIONBIO, &mode);

            sockaddr_in bindAddr{};
            bindAddr.sin_family = AF_INET;
            bindAddr.sin_port = htons(rxPort);
            bindAddr.sin_addr.s_addr = htonl(INADDR_ANY);

            if (bind(s, reinterpret_cast<sockaddr*>(&bindAddr), sizeof(bindAddr)) != SOCKET_ERROR)
            {
                rxSocket = static_cast<std::uintptr_t>(s);
            }
            else
            {
                closesocket(s);
            }
        }

        // 2. Bezpośrednie gniazdo UDP Port 17777 (Przechwytywanie komunikatów wysyłanych przez AgOpenGPS epAgIO na żywo bez AgIO!)
        SOCKET sLoop = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sLoop != INVALID_SOCKET)
        {
            BOOL opt = TRUE;
            setsockopt(sLoop, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));

            BOOL bNewBehavior = FALSE;
            DWORD dwBytesReturned = 0;
            WSAIoctl(sLoop, SIO_UDP_CONNRESET, &bNewBehavior, sizeof(bNewBehavior), NULL, 0, &dwBytesReturned, NULL, NULL);

            u_long mode = 1;
            ioctlsocket(sLoop, FIONBIO, &mode);

            sockaddr_in loopBind{};
            loopBind.sin_family = AF_INET;
            loopBind.sin_port = htons(17777);
            loopBind.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

            if (bind(sLoop, reinterpret_cast<sockaddr*>(&loopBind), sizeof(loopBind)) != SOCKET_ERROR)
            {
                rxSocketLoopback = static_cast<std::uintptr_t>(sLoop);
            }
            else
            {
                closesocket(sLoop);
            }
        }

        // 1. Initialize from configured aogSubnet in TerminalSettings
        const auto &cfg = TerminalSettings::get_instance().get_config();
        if (!cfg.aogSubnet.empty())
        {
            set_subnet_from_string(cfg.aogSubnet);
        }
        else
        {
            // Fallback auto-detect local subnet if not configured
            char host[256];
            if (gethostname(host, sizeof(host)) == 0)
            {
                addrinfo hints{}, *res = nullptr;
                hints.ai_family = AF_INET;
                if (getaddrinfo(host, nullptr, &hints, &res) == 0 && res != nullptr)
                {
                    for (addrinfo *p = res; p != nullptr; p = p->ai_next)
                    {
                        auto sin = reinterpret_cast<sockaddr_in*>(p->ai_addr);
                        std::uint32_t ip = ntohl(sin->sin_addr.s_addr);
                        std::uint8_t o1 = static_cast<std::uint8_t>((ip >> 24) & 0xFF);
                        std::uint8_t o2 = static_cast<std::uint8_t>((ip >> 16) & 0xFF);
                        std::uint8_t o3 = static_cast<std::uint8_t>((ip >> 8) & 0xFF);
                        if (o1 == 192 && o2 == 168)
                        {
                            subnet[0] = o1;
                            subnet[1] = o2;
                            subnet[2] = o3;
                            break;
                        }
                    }
                    freeaddrinfo(res);
                }
            }
        }

        running = (rxSocket != ~0ULL || rxSocketLoopback != ~0ULL);
        return running;
    }

    void AogUdpBridge::stop()
    {
        running = false;
        if (rxSocket != ~0ULL)
        {
            closesocket(static_cast<SOCKET>(rxSocket));
            rxSocket = ~0ULL;
        }
        if (rxSocketLoopback != ~0ULL)
        {
            closesocket(static_cast<SOCKET>(rxSocketLoopback));
            rxSocketLoopback = ~0ULL;
        }
        WSACleanup();
    }

    static void ParseIncomingNmea(const char *buf, int len)
    {
        if (len < 10 || buf[0] != '$') return;
        std::string line(buf, len);
        auto nl = line.find_first_of("\r\n");
        if (nl != std::string::npos) line = line.substr(0, nl);

        std::vector<std::string> tokens;
        std::stringstream ss(line);
        std::string token;
        while (std::getline(ss, token, ','))
        {
            tokens.push_back(token);
        }
        if (tokens.size() < 7) return;

        std::string type = tokens[0];
        if (type.size() >= 5) type = type.substr(type.size() - 3);

        if (type == "GGA" && tokens.size() >= 10)
        {
            if (!tokens[2].empty() && !tokens[4].empty())
            {
                try
                {
                    double rawLat = std::stod(tokens[2]);
                    double latDeg = std::floor(rawLat / 100.0);
                    double latMin = rawLat - (latDeg * 100.0);
                    double lat = latDeg + (latMin / 60.0);
                    if (tokens[3] == "S") lat = -lat;

                    double rawLon = std::stod(tokens[4]);
                    double lonDeg = std::floor(rawLon / 100.0);
                    double lonMin = rawLon - (lonDeg * 100.0);
                    double lon = lonDeg + (lonMin / 60.0);
                    if (tokens[5] == "W") lon = -lon;

                    int fix = std::stoi(tokens[6]);
                    int sats = tokens[7].empty() ? 0 : std::stoi(tokens[7]);
                    float hdop = tokens[8].empty() ? 99.0f : std::stof(tokens[8]);
                    double alt = tokens[9].empty() ? 0.0 : std::stod(tokens[9]);

                    auto fixEnum = static_cast<GpsFixQuality>(fix);
                    auto curTel = FieldNavigation::get_instance().get_telemetry();
                    FieldNavigation::get_instance().update_gps_nmea(
                        lat, lon, alt, curTel.speedKph, curTel.headingDeg, fixEnum, static_cast<uint8_t>(sats), hdop);
                }
                catch (...) {}
            }
        }
        else if (type == "VTG" && tokens.size() >= 8)
        {
            try
            {
                float heading = tokens[1].empty() ? 0.0f : std::stof(tokens[1]);
                float speedKph = tokens[7].empty() ? 0.0f : std::stof(tokens[7]);
                auto curTel = FieldNavigation::get_instance().get_telemetry();
                FieldNavigation::get_instance().update_gps_nmea(
                    curTel.latitude, curTel.longitude, curTel.altitude, speedKph, heading, curTel.fixQuality, curTel.satellites, curTel.hdop);
            }
            catch (...) {}
        }
    }

    void AogUdpBridge::poll()
    {
        if (!running) return;

        SOCKET socketsToPoll[2] = {
            (rxSocket != ~0ULL) ? static_cast<SOCKET>(rxSocket) : INVALID_SOCKET,
            (rxSocketLoopback != ~0ULL) ? static_cast<SOCKET>(rxSocketLoopback) : INVALID_SOCKET
        };

        std::uint8_t buf[1024];
        sockaddr_in srcAddr{};
        int srcLen = sizeof(srcAddr);

        for (int sIdx = 0; sIdx < 2; ++sIdx)
        {
            SOCKET s = socketsToPoll[sIdx];
            if (s == INVALID_SOCKET) continue;

            while (true)
            {
                srcLen = sizeof(srcAddr);
                int received = recvfrom(s, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                                        reinterpret_cast<sockaddr*>(&srcAddr), &srcLen);
                if (received <= 0) break;

                rxPackets++;
                lastRxTimestamp = isobus::SystemTiming::get_timestamp_ms();

                // 1. Raw ASCII NMEA stream from AgIO ($GPGGA, $GNGGA, $GNVTG)
                if (buf[0] == '$')
                {
                    ParseIncomingNmea(reinterpret_cast<const char*>(buf), received);
                    continue;
                }

                // 2. Framing: 0x80 0x81 [src] [pgn] [len] [payload...] [crc]
                if (received >= 6)
                {
                    std::uint16_t header = (static_cast<std::uint16_t>(buf[0]) << 8) | buf[1];
                    if (header == PACKET_START)
                    {
                        std::uint8_t src = buf[2];
                        std::uint8_t pgn = buf[3];
                        std::uint8_t len = buf[4];

                        if (received >= len + 6)
                        {
                            const std::uint8_t *payload = buf + 5;

                            // 0xC9: Subnet Detection [0xC9, 0xC9, IP0, IP1, IP2]
                            if (pgn == 0xC9 && len >= 5 && payload[0] == 0xC9 && payload[1] == 0xC9)
                            {
                                subnet[0] = payload[2];
                                subnet[1] = payload[3];
                                subnet[2] = payload[4];
                            }
                            // 0xD6: GPS / Fix Quality
                            else if (pgn == 0xD6 && len >= 39)
                            {
                                gpsFixQuality = payload[38];
                            }
                            // 0xFD: AutoSteer Board Feedback [Actual Angle, Roll, Switches]
                            else if (pgn == 0xFD && len >= 6)
                            {
                                int16_t steerRaw = static_cast<int16_t>((payload[0] << 8) | payload[1]);
                                float actualSteer = steerRaw * 0.01f;
                                int16_t rollRaw = static_cast<int16_t>((payload[2] << 8) | payload[3]);
                                float rollDeg = rollRaw * 0.1f;
                                uint8_t sw = payload[4];
                                bool steerSw = (sw & 1) != 0;
                                bool workSw = (sw & 2) != 0;

                                FieldNavigation::get_instance().update_steer_feedback(actualSteer, steerSw, workSw);
                                FieldNavigation::get_instance().update_imu(rollDeg, 0.0f);
                            }
                            // 0xD3: IMU Sentence [Roll, Pitch]
                            else if (pgn == 0xD3 && len >= 8)
                            {
                                int16_t rollRaw = static_cast<int16_t>((payload[0] << 8) | payload[1]);
                                int16_t pitchRaw = static_cast<int16_t>((payload[2] << 8) | payload[3]);
                                FieldNavigation::get_instance().update_imu(rollRaw * 0.1f, pitchRaw * 0.1f);
                            }
                            // 0xEB (235): Tool & Sections Geometry from AgOpenGPS (CPGN_EB)
                            else if (pgn == 0xEB && len >= 33)
                            {
                                std::uint8_t nSec = payload[32];
                                if (nSec > 0 && nSec <= 64)
                                {
                                    std::vector<float> widths;
                                    widths.reserve(nSec);
                                    float totalW = 0.0f;
                                    for (std::uint8_t i = 0; i < nSec && i < 16; ++i)
                                    {
                                        std::uint16_t wCm = static_cast<std::uint16_t>(payload[i]) | (static_cast<std::uint16_t>(payload[16 + i]) << 8);
                                        float wM = wCm * 0.01f;
                                        widths.push_back(wM);
                                        totalW += wM;
                                    }
                                    if (totalW > 0.5f && onToolGeometryReceived)
                                    {
                                        onToolGeometryReceived(totalW, nSec, widths);
                                    }
                                }
                            }
                            // 0xEF (239): Machine Data from AgOpenGPS (CPGN_EF - Speed, Hydraulic Lift, Sections 1..16)
                            else if (pgn == 0xEF && len >= 7)
                            {
                                float spd = payload[1] * 0.1f;
                                if (spd > 0.0f) gpsSpeedKph = spd;
                                std::vector<bool> secStates;
                                secStates.reserve(16);
                                for (int i = 0; i < 8; ++i) secStates.push_back((payload[5] & (1 << i)) != 0);
                                for (int i = 0; i < 8; ++i) secStates.push_back((payload[6] & (1 << i)) != 0);
                                if (onSectionStatesReceived) onSectionStatesReceived(secStates);
                            }
                            // 0xE5 (229): 64 Sections Setpoints Bitfield + Tool Left/Right Speed
                            else if (pgn == 0xE5 && len >= 8)
                            {
                                std::vector<bool> secStates;
                                secStates.reserve(64);
                                for (std::uint8_t j = 0; j < 8; ++j)
                                {
                                    for (std::uint8_t i = 0; i < 8; ++i)
                                        secStates.push_back((payload[j] & (1 << i)) != 0);
                                }
                                if (onSectionStatesReceived)
                                    onSectionStatesReceived(secStates);

                                if (len >= 12)
                                {
                                    int16_t lSpd = static_cast<int16_t>(payload[8] | (payload[9] << 8));
                                    int16_t rSpd = static_cast<int16_t>(payload[10] | (payload[11] << 8));
                                    float spd = (std::abs(lSpd) + std::abs(rSpd)) * 0.05f;
                                    if (spd > 0.0f) gpsSpeedKph = spd;
                                }
                            }
                            // 0xF1: Section Control Mode
                            else if (pgn == 0xF1 && len >= 1)
                            {
                                bool autoMode = (payload[0] == 1);
                                if (onSectionControlModeReceived)
                                    onSectionControlModeReceived(autoMode);
                            }
                            // 0xF2: Process Data (Speed, Distance, XTE)
                            else if (pgn == 0xF2 && len >= 6)
                            {
                                std::uint16_t ddi = static_cast<std::uint16_t>(payload[0]) | (static_cast<std::uint16_t>(payload[1]) << 8);
                                std::int32_t val = static_cast<std::int32_t>(
                                    static_cast<std::uint32_t>(payload[2]) |
                                    (static_cast<std::uint32_t>(payload[3]) << 8) |
                                    (static_cast<std::uint32_t>(payload[4]) << 16) |
                                    (static_cast<std::uint32_t>(payload[5]) << 24));

                                if (ddi == static_cast<std::uint16_t>(isobus::DataDescriptionIndex::ActualSpeed))
                                {
                                    float speedKph = static_cast<float>(std::abs(val)) * 0.0036f; // mm/s to km/h
                                    gpsSpeedKph = speedKph;
                                    if (onSpeedReceived)
                                        onSpeedReceived(speedKph, val);
                                }
                            }
                            // 0xF3: Field Name from AgOpenGPS (UTF-8 string of active field directory / name)
                            else if (pgn == 0xF3 && len > 0)
                            {
                                std::string aogField(reinterpret_cast<const char*>(payload), len);
                                while (!aogField.empty() && (aogField.back() == '\0' || aogField.back() == '\r' || aogField.back() == '\n'))
                                    aogField.pop_back();

                                if (!aogField.empty() && onFieldNameReceived)
                                {
                                    onFieldNameReceived(aogField);
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    void AogUdpBridge::send_heartbeat(bool autoMode, std::uint8_t numSections, const std::vector<bool> &actualStates)
    {
        if (!running || rxSocket == ~0ULL) return;

        SOCKET s = static_cast<SOCKET>(rxSocket);
        std::uint8_t txBuf[256];
        std::size_t idx = 0;

        txBuf[idx++] = 0x80;
        txBuf[idx++] = 0x81;
        txBuf[idx++] = 0x80; // Source: Task Controller
        txBuf[idx++] = 0xF0; // PGN 0xF0 (240) - Section heartbeat / state

        std::uint8_t bitfieldBytes = (numSections + 7) / 8;
        txBuf[idx++] = 2 + bitfieldBytes; // Length

        // Critical AgOpenGPS CISOBUS.DeserializeHeartbeat compatibility:
        // bit 0: sectionControlEnabled
        // bits 1..7: numClients (MUST BE > 0, otherwise AgOpenGPS resets sections to 0 and disables painting!)
        std::uint8_t clientsCount = 1;
        txBuf[idx++] = static_cast<std::uint8_t>((clientsCount << 1) | (autoMode ? 1 : 0));
        txBuf[idx++] = numSections;

        for (std::uint8_t b = 0; b < bitfieldBytes; ++b)
        {
            std::uint8_t byteVal = 0;
            for (std::uint8_t bit = 0; bit < 8; ++bit)
            {
                std::uint8_t sIdx = b * 8 + bit;
                if (sIdx < actualStates.size() && actualStates[sIdx])
                    byteVal |= (1 << bit);
            }
            txBuf[idx++] = byteVal;
        }

        txBuf[idx] = calculate_crc(txBuf + 2, idx - 2);
        idx++;

        // 1. Send to loopback 127.0.0.1:9999 (AgIO)
        sockaddr_in loopAddr{};
        loopAddr.sin_family = AF_INET;
        loopAddr.sin_port = htons(txPort);
        loopAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        sendto(s, reinterpret_cast<const char*>(txBuf), static_cast<int>(idx), 0,
               reinterpret_cast<sockaddr*>(&loopAddr), sizeof(loopAddr));

        // 2. Send directly to AgOpenGPS Loopback Server (127.0.0.1:15555) for instantaneous field painting
        sockaddr_in aogLoopAddr{};
        aogLoopAddr.sin_family = AF_INET;
        aogLoopAddr.sin_port = htons(15555);
        aogLoopAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        sendto(s, reinterpret_cast<const char*>(txBuf), static_cast<int>(idx), 0,
               reinterpret_cast<sockaddr*>(&aogLoopAddr), sizeof(aogLoopAddr));

        // 3. Send to general broadcast 255.255.255.255:9999
        sockaddr_in bcastAny{};
        bcastAny.sin_family = AF_INET;
        bcastAny.sin_port = htons(txPort);
        bcastAny.sin_addr.s_addr = htonl(INADDR_BROADCAST);
        sendto(s, reinterpret_cast<const char*>(txBuf), static_cast<int>(idx), 0,
               reinterpret_cast<sockaddr*>(&bcastAny), sizeof(bcastAny));

        // 4. Send to LAN broadcast <subnet>.255:9999 (configured in settings)
        sockaddr_in bcastAddr{};
        bcastAddr.sin_family = AF_INET;
        bcastAddr.sin_port = htons(txPort);
        std::string bcastIp = std::to_string(subnet[0]) + "." + std::to_string(subnet[1]) + "." + std::to_string(subnet[2]) + ".255";
        inet_pton(AF_INET, bcastIp.c_str(), &bcastAddr.sin_addr);
        sendto(s, reinterpret_cast<const char*>(txBuf), static_cast<int>(idx), 0,
               reinterpret_cast<sockaddr*>(&bcastAddr), sizeof(bcastAddr));

        // 5. Also send to other active network adapter subnets (e.g. 192.168.68.255)
        char host[256];
        if (gethostname(host, sizeof(host)) == 0)
        {
            addrinfo hints{}, *res = nullptr;
            hints.ai_family = AF_INET;
            if (getaddrinfo(host, nullptr, &hints, &res) == 0 && res != nullptr)
            {
                for (addrinfo *p = res; p != nullptr; p = p->ai_next)
                {
                    auto sin = reinterpret_cast<sockaddr_in*>(p->ai_addr);
                    std::uint32_t ip = ntohl(sin->sin_addr.s_addr);
                    std::uint8_t o1 = static_cast<std::uint8_t>((ip >> 24) & 0xFF);
                    std::uint8_t o2 = static_cast<std::uint8_t>((ip >> 16) & 0xFF);
                    std::uint8_t o3 = static_cast<std::uint8_t>((ip >> 8) & 0xFF);
                    if (o1 != 127 && (o1 != subnet[0] || o2 != subnet[1] || o3 != subnet[2]))
                    {
                        sockaddr_in extraBcast{};
                        extraBcast.sin_family = AF_INET;
                        extraBcast.sin_port = htons(txPort);
                        std::string extraIp = std::to_string(o1) + "." + std::to_string(o2) + "." + std::to_string(o3) + ".255";
                        inet_pton(AF_INET, extraIp.c_str(), &extraBcast.sin_addr);
                        sendto(s, reinterpret_cast<const char*>(txBuf), static_cast<int>(idx), 0,
                               reinterpret_cast<sockaddr*>(&extraBcast), sizeof(extraBcast));
                    }
                }
                freeaddrinfo(res);
            }
        }

        txPackets++;
    }

    void AogUdpBridge::send_work_switch_command(bool workActive)
    {
        if (!running || rxSocket == ~0ULL) return;

        SOCKET s = static_cast<SOCKET>(rxSocket);

        // 1. PGN 0xEA (234) - Switch Sentence do AgOpenGPS
        // swMain:
        // bit 1 (wartość 2) = Master Switch AUTO ON!
        // bit 0 (wartość 1) = Master Switch OFF!
        std::uint8_t txBuf[32];
        std::size_t idx = 0;
        txBuf[idx++] = 0x80;
        txBuf[idx++] = 0x81;
        txBuf[idx++] = 0x7F; // Machine / Switch Module
        txBuf[idx++] = 0xEA; // PGN 0xEA (234)
        txBuf[idx++] = 9;    // Długość 9
        txBuf[idx++] = 0;    // swHeader
        txBuf[idx++] = workActive ? 2 : 1; // swMain: 2 = Master Auto ON, 1 = Master OFF
        txBuf[idx++] = 0;    // swAutoGr0
        txBuf[idx++] = 0;    // swAutoGr1
        txBuf[idx++] = 0;    // swNumSections
        txBuf[idx++] = 0;    // swOnGr0
        txBuf[idx++] = 0;    // swOffGr0
        txBuf[idx++] = 0;    // swOnGr1
        txBuf[idx++] = 0;    // swOffGr1
        txBuf[idx] = calculate_crc(txBuf + 2, idx - 2);
        idx++;

        sockaddr_in loopAddr{};
        loopAddr.sin_family = AF_INET;
        loopAddr.sin_port = htons(txPort);
        loopAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        sendto(s, reinterpret_cast<const char*>(txBuf), static_cast<int>(idx), 0,
               reinterpret_cast<sockaddr*>(&loopAddr), sizeof(loopAddr));

        sockaddr_in aogLoop{};
        aogLoop.sin_family = AF_INET;
        aogLoop.sin_port = htons(15555);
        aogLoop.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        sendto(s, reinterpret_cast<const char*>(txBuf), static_cast<int>(idx), 0,
               reinterpret_cast<sockaddr*>(&aogLoop), sizeof(aogLoop));

        // 2. PGN 0xFD (253) - Sprzętowy stan przełącznika roboczego (AutoSteer / WorkSwitch High)
        std::uint8_t fdBuf[32];
        std::size_t fIdx = 0;
        fdBuf[fIdx++] = 0x80;
        fdBuf[fIdx++] = 0x81;
        fdBuf[fIdx++] = 0x7E; // Steer Module
        fdBuf[fIdx++] = 0xFD; // PGN 0xFD (253)
        fdBuf[fIdx++] = 10;   // Długość 10
        fdBuf[fIdx++] = 0;    // steerAngleHi
        fdBuf[fIdx++] = 0;    // steerAngleLo
        fdBuf[fIdx++] = 0;    // rollHi
        fdBuf[fIdx++] = 0;    // rollLo
        fdBuf[fIdx++] = workActive ? 0x02 : 0x00; // byte 4: bit 1 = work switch active
        fdBuf[fIdx++] = 0;    // pwm
        fdBuf[fIdx++] = workActive ? 0x01 : 0x00; // byte 11 w pakiecie (offset 5+6): workSwitchHigh bit 0 = 1!
        fdBuf[fIdx++] = 0;
        fdBuf[fIdx++] = 0;
        fdBuf[fIdx++] = 0;
        fdBuf[fIdx] = calculate_crc(fdBuf + 2, fIdx - 2);
        fIdx++;

        sendto(s, reinterpret_cast<const char*>(fdBuf), static_cast<int>(fIdx), 0,
               reinterpret_cast<sockaddr*>(&aogLoop), sizeof(aogLoop));
        sendto(s, reinterpret_cast<const char*>(fdBuf), static_cast<int>(fIdx), 0,
               reinterpret_cast<sockaddr*>(&loopAddr), sizeof(loopAddr));
    }

    void AogUdpBridge::send_autosteer_command(bool engaged, float steerAngleDeg, float speedKph)
    {
        if (!running || rxSocket == ~0ULL) return;

        SOCKET s = static_cast<SOCKET>(rxSocket);
        std::uint8_t txBuf[32];
        std::size_t idx = 0;

        txBuf[idx++] = 0x80;
        txBuf[idx++] = 0x81;
        txBuf[idx++] = 0x7F; // Source: AgOpenGPS
        txBuf[idx++] = 0xFE; // PGN 0xFE (254) - AutoSteer data command
        txBuf[idx++] = 8;    // Length

        std::uint16_t spd = static_cast<std::uint16_t>(std::clamp(speedKph * 10.0f, 0.0f, 65535.0f));
        txBuf[idx++] = static_cast<std::uint8_t>((spd >> 8) & 0xFF);
        txBuf[idx++] = static_cast<std::uint8_t>(spd & 0xFF);

        txBuf[idx++] = engaged ? 1 : 0; // Bit 0: steer enable

        std::int16_t angleRaw = static_cast<std::int16_t>(std::clamp(steerAngleDeg * 100.0f, -32768.0f, 32767.0f));
        txBuf[idx++] = static_cast<std::uint8_t>((angleRaw >> 8) & 0xFF);
        txBuf[idx++] = static_cast<std::uint8_t>(angleRaw & 0xFF);

        txBuf[idx++] = 0; // tramline
        txBuf[idx++] = 0; // spare
        txBuf[idx++] = 0; // spare

        txBuf[idx] = calculate_crc(txBuf + 2, idx - 2);
        idx++;

        sockaddr_in loopAddr{};
        loopAddr.sin_family = AF_INET;
        loopAddr.sin_port = htons(txPort);
        loopAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        sendto(s, reinterpret_cast<const char*>(txBuf), static_cast<int>(idx), 0,
               reinterpret_cast<sockaddr*>(&loopAddr), sizeof(loopAddr));
    }

    void AogUdpBridge::send_hardware_message(const std::string &text, std::uint8_t durationSec, std::uint8_t colour)
    {
        if (!running || rxSocket == ~0ULL) return;

        SOCKET s = static_cast<SOCKET>(rxSocket);
        std::uint8_t txBuf[128];
        std::size_t idx = 0;

        txBuf[idx++] = 0x80;
        txBuf[idx++] = 0x81;
        txBuf[idx++] = 0x80;
        txBuf[idx++] = 0xDD; // PGN 0xDD (221) - Hardware message

        std::size_t txtLen = std::min<std::size_t>(text.size(), 60);
        txBuf[idx++] = static_cast<std::uint8_t>(txtLen + 2);
        txBuf[idx++] = durationSec;
        txBuf[idx++] = colour;

        std::memcpy(txBuf + idx, text.data(), txtLen);
        idx += txtLen;

        txBuf[idx] = calculate_crc(txBuf + 2, idx - 2);
        idx++;

        sockaddr_in loopAddr{};
        loopAddr.sin_family = AF_INET;
        loopAddr.sin_port = htons(txPort);
        loopAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        sendto(s, reinterpret_cast<const char*>(txBuf), static_cast<int>(idx), 0,
               reinterpret_cast<sockaddr*>(&loopAddr), sizeof(loopAddr));
    }

    bool AogUdpBridge::is_aog_active() const
    {
        if (lastRxTimestamp == 0) return false;
        return isobus::SystemTiming::get_time_elapsed_ms(lastRxTimestamp) < 2500;
    }

    std::string AogUdpBridge::get_subnet_string() const
    {
        return std::to_string(subnet[0]) + "." + std::to_string(subnet[1]) + "." + std::to_string(subnet[2]) + ".x";
    }

    void AogUdpBridge::set_subnet(std::uint8_t o1, std::uint8_t o2, std::uint8_t o3)
    {
        subnet[0] = o1;
        subnet[1] = o2;
        subnet[2] = o3;
    }

    void AogUdpBridge::set_subnet_from_string(const std::string &subnetStr)
    {
        std::stringstream ss(subnetStr);
        std::string token;
        std::vector<std::uint8_t> parts;
        while (std::getline(ss, token, '.'))
        {
            if (!token.empty())
            {
                try {
                    int val = std::stoi(token);
                    if (val >= 0 && val <= 255)
                        parts.push_back(static_cast<std::uint8_t>(val));
                } catch (...) {}
            }
        }
        if (parts.size() >= 3)
        {
            subnet[0] = parts[0];
            subnet[1] = parts[1];
            subnet[2] = parts[2];
        }
    }

    // =========================================================================
    // TaskControllerManager Implementation
    // =========================================================================
    TaskControllerManager::TaskControllerManager()
    {
    }

    TaskControllerManager::~TaskControllerManager()
    {
        shutdown();
    }

    void TaskControllerManager::log_message(const std::string &msg)
    {
        std::lock_guard<std::mutex> lock(logMutex);
        if (recentLogs.size() > 50)
            recentLogs.pop_front();
        recentLogs.push_back(msg);
        std::cout << "[AOG-TC] " << msg << std::endl;
    }

    bool TaskControllerManager::initialize(std::uint8_t canChannelIndex)
    {
        log_message("Initializing AgOpenGPS Task Controller Integration...");

        // 1. Create Task Controller InternalControlFunction (Address 247 / 0xF7)
        isobus::NAME tcName(0);
        tcName.set_arbitrary_address_capable(true);
        tcName.set_industry_group(2); // Agriculture
        tcName.set_device_class(0);
        tcName.set_function_code(static_cast<std::uint8_t>(isobus::NAME::Function::TaskController)); // 130
        tcName.set_identity_number(0x0014);
        tcName.set_manufacturer_code(1407);
        tcName.set_ecu_instance(0);
        tcName.set_function_instance(0);

        tcCF = isobus::CANNetworkManager::CANNetwork.create_internal_control_function(
            tcName, canChannelIndex, isobus::preferred_addresses::IndustryGroup2::TaskController_MappingComputer);

        // 2. Create Tractor ECU InternalControlFunction (Address 240 / 0xF0)
        isobus::NAME tecuName(0);
        tecuName.set_arbitrary_address_capable(false);
        tecuName.set_industry_group(2);
        tecuName.set_device_class(0);
        tecuName.set_function_code(static_cast<std::uint8_t>(isobus::NAME::Function::TractorECU)); // 134
        tecuName.set_identity_number(0x0014);
        tecuName.set_manufacturer_code(1407);
        tecuName.set_ecu_instance(0);
        tecuName.set_function_instance(0);

        tecuCF = isobus::CANNetworkManager::CANNetwork.create_internal_control_function(
            tecuName, canChannelIndex, isobus::preferred_addresses::IndustryGroup2::TractorECU);

        // 3. Create and initialize Terminal TC Server
        tcServer = std::make_shared<TerminalTCServer>(tcCF);
        auto &tcLang = tcServer->get_language_command_interface();
        tcLang.set_language_code("pl");
        tcLang.set_country_code("PL");
        tcLang.set_commanded_decimal_symbol(isobus::LanguageCommandInterface::DecimalSymbols::Comma);
        tcLang.set_commanded_time_format(isobus::LanguageCommandInterface::TimeFormats::TwentyFourHour);
        tcLang.set_commanded_date_format(isobus::LanguageCommandInterface::DateFormats::ddmmyyyy);
        tcLang.set_commanded_distance_units(isobus::LanguageCommandInterface::DistanceUnits::Metric);
        tcLang.set_commanded_area_units(isobus::LanguageCommandInterface::AreaUnits::Metric);
        tcLang.set_commanded_volume_units(isobus::LanguageCommandInterface::VolumeUnits::Metric);
        tcLang.set_commanded_mass_units(isobus::LanguageCommandInterface::MassUnits::Metric);
        tcLang.set_commanded_generic_units(isobus::LanguageCommandInterface::UnitSystem::Metric);

        tcServer->initialize();
        tcServer->install_safe_callbacks();
        tcServer->set_task_totals_active(false);
        tcServer->onFallbackSectionChanged = [this](std::uint8_t sec, bool state) {
            log_message("AgOpenGPS commanded Section " + std::to_string(sec + 1) + " -> " + (state ? "ON" : "OFF"));
            if (onSectionStateChanged)
                onSectionStateChanged(sec, state);
        };

        // 4. Announce TC Functionalities via PGN 64654 (AEF TC-BAS, TC-GEO, TC-SC)
        tcFunctionalities = std::make_unique<isobus::ControlFunctionFunctionalities>(tcCF);
        tcFunctionalities->set_functionality_is_supported(
            isobus::ControlFunctionFunctionalities::Functionalities::TaskControllerBasicServer, 1, true);
        tcFunctionalities->set_functionality_is_supported(
            isobus::ControlFunctionFunctionalities::Functionalities::TaskControllerGeoServer, 1, true);
        tcFunctionalities->set_task_controller_geo_server_option_state(
            isobus::ControlFunctionFunctionalities::TaskControllerGeoServerOptions::PolygonBasedPrescriptionMapsAreSupported, true);
        tcFunctionalities->set_functionality_is_supported(
            isobus::ControlFunctionFunctionalities::Functionalities::TaskControllerSectionControlServer, 1, true);
        tcFunctionalities->set_task_controller_section_control_server_option_state(6, 128); // 6 booms / 128 sections for wide implements
        log_message("TC announced TC-BAS, TC-GEO (Prescriptions) and TC-SC (6 booms / 128 sections) on CAN");

        // 5. Announce TECU Functionalities (Class 2 + Navigation + Guidance) and initialize Speed Messages Interface
        tecuFunctionalities = std::make_unique<isobus::ControlFunctionFunctionalities>(tecuCF);
        tecuFunctionalities->set_functionality_is_supported(
            isobus::ControlFunctionFunctionalities::Functionalities::BasicTractorECUServer, 2, true);
        tecuFunctionalities->set_basic_tractor_ECU_server_option_state(
            isobus::ControlFunctionFunctionalities::BasicTractorECUOptions::Class2NoOptions, true);
        tecuFunctionalities->set_basic_tractor_ECU_server_option_state(
            isobus::ControlFunctionFunctionalities::BasicTractorECUOptions::NavigationOption, true);
        tecuFunctionalities->set_basic_tractor_ECU_server_option_state(
            isobus::ControlFunctionFunctionalities::BasicTractorECUOptions::GuidanceOption, true);

        speedMessagesInterface = std::make_unique<isobus::SpeedMessagesInterface>(tecuCF, true, true, true, false);
        speedMessagesInterface->initialize();
        speedMessagesInterface->wheelBasedSpeedTransmitData.set_implement_start_stop_operations_state(
            isobus::WheelBasedMachineSpeedData::ImplementStartStopOperations::NotAvailable);
        speedMessagesInterface->wheelBasedSpeedTransmitData.set_key_switch_state(
            isobus::WheelBasedMachineSpeedData::KeySwitchState::NotAvailable);
        speedMessagesInterface->wheelBasedSpeedTransmitData.set_operator_direction_reversed_state(
            isobus::WheelBasedMachineSpeedData::OperatorDirectionReversed::NotAvailable);
        speedMessagesInterface->machineSelectedSpeedTransmitData.set_speed_source(
            isobus::MachineSelectedSpeedData::SpeedSource::NavigationBasedSpeed);

        // Initial default speed 0 mm/s forward
        speedMessagesInterface->groundBasedSpeedTransmitData.set_machine_speed(0);
        speedMessagesInterface->wheelBasedSpeedTransmitData.set_machine_speed(0);
        speedMessagesInterface->machineSelectedSpeedTransmitData.set_machine_speed(0);
        speedMessagesInterface->groundBasedSpeedTransmitData.set_machine_direction_of_travel(isobus::MachineDirection::Forward);
        speedMessagesInterface->wheelBasedSpeedTransmitData.set_machine_direction_of_travel(isobus::MachineDirection::Forward);
        speedMessagesInterface->machineSelectedSpeedTransmitData.set_machine_direction_of_travel(isobus::MachineDirection::Forward);
        log_message("TECU Speed Interface initialized");

        // 5b. ISO 11783-7 Time & Date Interface (PGN 65254 / 0xFEE6)
        timeDateInterface = std::make_unique<isobus::TimeDateInterface>(tecuCF, [](isobus::TimeDateInterface::TimeAndDate &td) {
            SYSTEMTIME st;
            GetSystemTime(&st);
            td.year = st.wYear;
            td.month = static_cast<std::uint8_t>(st.wMonth);
            td.day = static_cast<std::uint8_t>(st.wDay);
            td.hours = static_cast<std::uint8_t>(st.wHour);
            td.minutes = static_cast<std::uint8_t>(st.wMinute);
            td.seconds = static_cast<std::uint8_t>(st.wSecond);
            td.milliseconds = static_cast<std::uint16_t>((st.wMilliseconds / 250) * 250);

            TIME_ZONE_INFORMATION tzi;
            GetTimeZoneInformation(&tzi);
            td.localHourOffset = static_cast<std::int8_t>((-tzi.Bias) / 60);
            td.localMinuteOffset = static_cast<std::int8_t>((-tzi.Bias) % 60);
            return true;
        });
        timeDateInterface->initialize();
        log_message("TECU Time/Date Interface (PGN 65254) initialized");

        // 6. Create UDP Bridge for AgOpenGPS / AgIO
        udpBridge = std::make_shared<AogUdpBridge>();
        if (!udpBridge->start(8888, 9999))
        {
            log_message("Warning: Failed to bind UDP 8888 for AgOpenGPS");
        }
        else
        {
            log_message("UDP Bridge active: Listening on 8888, Broadcast on 9999");
        }

        // Connect UDP hooks to TC Server & Speed Interface
        udpBridge->onSectionStatesReceived = [this](const std::vector<bool> &states) {
            if (tcServer)
                tcServer->update_section_setpoints(states);
            AmazoneBridge::get_instance().on_aog_section_states(states);
        };

        udpBridge->onSectionControlModeReceived = [this](bool autoMode) {
            if (tcServer)
                tcServer->update_section_control_enabled(autoMode);
            AmazoneBridge::get_instance().on_aog_mode_received(autoMode);
            log_message(std::string("Section Control Mode: ") + (autoMode ? "AUTO" : "MANUAL"));
        };

        udpBridge->onSpeedReceived = [this](float speedKph, std::int32_t valMmPerSec) {
            if (speedMessagesInterface)
            {
                std::uint16_t speed = static_cast<std::uint16_t>(std::abs(valMmPerSec));
                auto direction = valMmPerSec < 0 ? isobus::MachineDirection::Reverse : isobus::MachineDirection::Forward;
                speedMessagesInterface->groundBasedSpeedTransmitData.set_machine_direction_of_travel(direction);
                speedMessagesInterface->wheelBasedSpeedTransmitData.set_machine_direction_of_travel(direction);
                speedMessagesInterface->machineSelectedSpeedTransmitData.set_machine_direction_of_travel(direction);

                speedMessagesInterface->groundBasedSpeedTransmitData.set_machine_speed(speed);
                speedMessagesInterface->wheelBasedSpeedTransmitData.set_machine_speed(speed);
                speedMessagesInterface->machineSelectedSpeedTransmitData.set_machine_speed(speed);
            }
            AmazoneBridge::get_instance().on_aog_speed_received(speedKph);
            if (onSpeedChanged)
            {
                onSpeedChanged(speedKph);
            }
        };

        // Direct AgOpenGPS hooks for field name, tool geometry and auto-configuration
        udpBridge->onFieldNameReceived = [this](const std::string &fn) {
            TcBasReportManager::get_instance().set_field_name_from_aog(fn);
            log_message("AgOpenGPS aktywne pole: '" + fn + "'");
        };

        udpBridge->onToolGeometryReceived = [this](float widthM, std::uint8_t nSec, const std::vector<float>& widths) {
            if (widthM > 0.5f)
            {
                auto &tcBas = TcBasReportManager::get_instance();
                tcBas.get_current_task().implementWidthMeters = widthM;
                tcBas.get_current_task().implementSectionsCount = nSec;
                tcBas.get_current_task().implementName = "AgOpenGPS (" + std::to_string(static_cast<int>(widthM + 0.5f)) + "m, " + std::to_string(nSec) + " sekcji)";
                tcBas.set_last_aog_sync_info("AgOpenGPS Live UDP: " + std::to_string(static_cast<int>(widthM + 0.5f)) + "m robocze, " + std::to_string(nSec) + " sekcji");
                if (tcServer)
                {
                    tcServer->set_fallback_sections_count(nSec);
                }
                log_message("AgOpenGPS Belka PGN 0xEB: " + std::to_string(widthM) + " m, " + std::to_string(nSec) + " sekcji");
            }
        };

        // Wykonaj bezpośrednie wstępne skanowanie profili AgOpenGPS
        TcBasReportManager::get_instance().scan_and_sync_agopengps();
        if (tcServer)
        {
            int initialSecs = TcBasReportManager::get_instance().get_current_task().implementSectionsCount;
            if (initialSecs > 0)
            {
                tcServer->set_fallback_sections_count(static_cast<std::uint8_t>(initialSecs));
            }
        }

        log_message("TC Server & TECU online on ISOBUS channel " + std::to_string(canChannelIndex));
        return true;
    }

    void TaskControllerManager::update()
    {
        std::uint32_t now = isobus::SystemTiming::get_timestamp_ms();

        // 1. Cykliczny auto-skan plików AgOpenGPS (Profil maszyny / pole / sekcje) co 1.5 sekundy
        static std::uint32_t lastAogScanTick = 0;
        if (now - lastAogScanTick >= 1500)
        {
            lastAogScanTick = now;
            TcBasReportManager::get_instance().scan_and_sync_agopengps();
            if (tcServer)
            {
                int aogSecs = TcBasReportManager::get_instance().get_current_task().implementSectionsCount;
                if (aogSecs > 0 && tcServer->get_total_sections() < aogSecs)
                {
                    tcServer->set_fallback_sections_count(static_cast<std::uint8_t>(aogSecs));
                }
            }
        }

        if (udpBridge)
        {
            udpBridge->poll();

            // Send 50 ms heartbeat PGN 0xF0 to AgOpenGPS / AgIO (20 Hz dla stabilnego malowania i braku timeoutu 1.0s)
            if (now - lastHeartbeatMs >= 50)
            {
                lastHeartbeatMs = now;
                bool autoMode = tcServer ? tcServer->is_auto_mode() : true;
                std::uint8_t nSec = tcServer ? tcServer->get_total_sections() : 0;
                int aogSec = TcBasReportManager::get_instance().get_current_task().implementSectionsCount;
                if (aogSec > 0 && (nSec == 0 || nSec < aogSec))
                {
                    nSec = static_cast<std::uint8_t>(aogSec);
                    if (tcServer) tcServer->set_fallback_sections_count(nSec);
                }
                if (nSec == 0) nSec = 8; // Domyślnie 8 sekcji dla belki 28m

                std::vector<bool> actual = tcServer ? tcServer->get_actual_section_states() : std::vector<bool>();
                if (actual.size() < nSec) actual.resize(nSec, false);
                
                // Sprawdź warunek roboczy traktora (TUZ opuszczony / WOM aktywny / wymuszenie w UI)
                bool workActive = TractorEcuManager::get_instance().is_work_active() || forcePaint;
                if (workActive)
                {
                    // Malowanie wymuszone / stan roboczy: wszystkie sekcje aktywne
                    std::fill(actual.begin(), actual.end(), true);
                    if (tcServer)
                    {
                        for (std::uint8_t s = 0; s < actual.size(); ++s)
                            tcServer->set_fallback_section_state(s, true);
                    }
                }
                else
                {
                    // Tryb Auto: pobierz setpointy wysłane przez AgOpenGPS (PGN 0xE5) i potwierdź je w stanie actual,
                    // dzięki czemu AgOpenGPS widzi, że sekcja faktycznie pryska i maluje mapę na zielono!
                    auto setpoints = tcServer ? tcServer->get_setpoint_section_states() : std::vector<bool>();
                    for (std::size_t i = 0; i < actual.size() && i < setpoints.size(); ++i)
                    {
                        if (setpoints[i])
                        {
                            actual[i] = true;
                        }
                    }
                }

                udpBridge->send_heartbeat(autoMode, static_cast<std::uint8_t>(actual.size()), actual);
            }

            // Send 100 ms Work Switch Command (PGN 0xEA + 0xFD) to AgOpenGPS to synchronize Master work switch
            static std::uint32_t lastWorkSendMs = 0;
            if (now - lastWorkSendMs >= 100)
            {
                lastWorkSendMs = now;
                bool workActive = TractorEcuManager::get_instance().is_work_active() || forcePaint;
                if (tcServer)
                {
                    auto actual = tcServer->get_actual_section_states();
                    for (bool b : actual) if (b) { workActive = true; break; }
                }
                udpBridge->send_work_switch_command(workActive);
            }

            // Send 50 ms AutoSteer guidance command (PGN 0xFE) to AgIO
            static std::uint32_t lastSteerSendMs = 0;
            if (now - lastSteerSendMs >= 50)
            {
                lastSteerSendMs = now;
                auto &nav = FieldNavigation::get_instance();
                auto &steer = nav.get_autosteer();
                auto tel = nav.get_telemetry();
                udpBridge->send_autosteer_command(steer.is_engaged(), steer.get_target_steer_angle(), tel.speedKph);
            }

            if (tcServer)
            {
                std::uint8_t totalSec = tcServer->get_total_sections();
                std::vector<bool> actual = tcServer->get_actual_section_states();
                FieldNavigation::get_instance().get_coverage().update_sections_from_tc(totalSec, actual);
            }
        }

        bool aogConnected = is_aog_connected();

        if (tcServer)
        {
            if (tcServer->get_task_totals_active() != aogConnected)
            {
                tcServer->set_task_totals_active(aogConnected);
                log_message(std::string("Task Controller Task State: ") +
                            (aogConnected ? "ACTIVE (AOG Field Running)" : "INACTIVE (Waiting for AOG)"));
            }
            safe_update_tc(tcServer.get());
        }

        // Live accumulation for TC-BAS (ISO 11783-10) and Ewidencja ŚOR (2027)
        static std::uint32_t lastTcBasUpdateMs = 0;
        std::uint32_t currentTickMs = isobus::SystemTiming::get_timestamp_ms();
        if (lastTcBasUpdateMs == 0) lastTcBasUpdateMs = currentTickMs;
        float dtSec = (currentTickMs - lastTcBasUpdateMs) / 1000.0f;
        lastTcBasUpdateMs = currentTickMs;

        if (dtSec > 0.0f && dtSec < 1.0f)
        {
            float speedKph = get_speed_kph();
            if (speedKph <= 0.05f)
            {
                auto tel = TractorEcuManager::get_instance().get_telemetry();
                if (tel.groundSpeedKph > 0.05f) speedKph = tel.groundSpeedKph;
                else if (tel.wheelSpeedKph > 0.05f) speedKph = tel.wheelSpeedKph;
            }

            float sectionRatio = 0.0f;
            bool workActive = TractorEcuManager::get_instance().is_work_active() || forcePaint;

            if (tcServer)
            {
                auto actual = tcServer->get_actual_section_states();
                if (!actual.empty())
                {
                    int onCount = 0;
                    for (bool b : actual) if (b) onCount++;
                    sectionRatio = static_cast<float>(onCount) / static_cast<float>(actual.size());
                }
                else if (tcServer->is_auto_mode() && aogConnected)
                {
                    sectionRatio = 1.0f;
                }
            }
            else if (aogConnected)
            {
                sectionRatio = 1.0f;
            }

            if (sectionRatio <= 0.0f && workActive)
            {
                sectionRatio = 1.0f;
            }

            TcBasReportManager::get_instance().update_work_data(speedKph, sectionRatio, dtSec);
        }

        // Broadcast TECU Speed (0xFE48 / 0xFE49) periodically (10 Hz)
        if (speedMessagesInterface)
        {
            speedMessagesInterface->update();
        }

        // Periodic ISO 11783-7 Time & Date broadcast (1 Hz)
        static std::uint32_t lastTimeDateTx = 0;
        if (isobus::SystemTiming::time_expired_ms(lastTimeDateTx, 1000) && timeDateInterface)
        {
            lastTimeDateTx = isobus::SystemTiming::get_timestamp_ms();
            isobus::TimeDateInterface::TimeAndDate td;
            SYSTEMTIME st;
            GetSystemTime(&st);
            td.year = st.wYear;
            td.month = static_cast<std::uint8_t>(st.wMonth);
            td.day = static_cast<std::uint8_t>(st.wDay);
            td.hours = static_cast<std::uint8_t>(st.wHour);
            td.minutes = static_cast<std::uint8_t>(st.wMinute);
            td.seconds = static_cast<std::uint8_t>(st.wSecond);
            td.milliseconds = static_cast<std::uint16_t>((st.wMilliseconds / 250) * 250);

            TIME_ZONE_INFORMATION tzi;
            GetTimeZoneInformation(&tzi);
            td.localHourOffset = static_cast<std::int8_t>((-tzi.Bias) / 60);
            td.localMinuteOffset = static_cast<std::int8_t>((-tzi.Bias) % 60);
            timeDateInterface->send_time_and_date(td);
        }
    }

    void TaskControllerManager::shutdown()
    {
        if (udpBridge)
        {
            udpBridge->stop();
            udpBridge.reset();
        }
        timeDateInterface.reset();
        speedMessagesInterface.reset();
        tcFunctionalities.reset();
        tecuFunctionalities.reset();
        if (tcServer)
        {
            tcServer->terminate();
            tcServer.reset();
        }
        tcCF.reset();
        tecuCF.reset();
    }

    bool TaskControllerManager::is_aog_connected() const
    {
        return udpBridge && udpBridge->is_aog_active();
    }

    std::uint8_t TaskControllerManager::get_num_sections() const
    {
        return tcServer ? tcServer->get_total_sections() : 0;
    }

    std::vector<bool> TaskControllerManager::get_section_states() const
    {
        return tcServer ? tcServer->get_actual_section_states() : std::vector<bool>();
    }

    std::vector<bool> TaskControllerManager::get_section_setpoints() const
    {
        return tcServer ? tcServer->get_setpoint_section_states() : std::vector<bool>();
    }

    float TaskControllerManager::get_speed_kph() const
    {
        return udpBridge ? udpBridge->get_gps_speed_kph() : 0.0f;
    }

    void TaskControllerManager::set_aog_subnet(const std::string &subnetStr)
    {
        if (udpBridge) udpBridge->set_subnet_from_string(subnetStr);
    }

    void TaskControllerManager::set_aog_subnet(std::uint8_t o1, std::uint8_t o2, std::uint8_t o3)
    {
        if (udpBridge) udpBridge->set_subnet(o1, o2, o3);
    }

    std::array<std::uint8_t, 3> TaskControllerManager::get_aog_subnet() const
    {
        if (udpBridge) return udpBridge->get_subnet();
        return { 192, 168, 5 };
    }

    std::string TaskControllerManager::get_aog_subnet_string() const
    {
        if (udpBridge) return udpBridge->get_subnet_string();
        return "192.168.5.x";
    }

    bool TaskControllerManager::is_auto_mode() const
    {
        return tcServer ? tcServer->is_auto_mode() : true;
    }

    void TaskControllerManager::set_section_actual(std::uint8_t sec, bool state)
    {
        if (tcServer)
            tcServer->set_fallback_section_state(sec, state);
    }

    const std::deque<std::string>& TaskControllerManager::get_recent_logs() const
    {
        return recentLogs;
    }
}

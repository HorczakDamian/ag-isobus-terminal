#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include "TaskControllerManager.hpp"
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
    }

    void TerminalTCServer::update_section_setpoints(const std::vector<bool> &states)
    {
        std::lock_guard<std::recursive_mutex> lock(clientMutex);
        if (clients.empty())
        {
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
            return;
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

        SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s == INVALID_SOCKET)
            return false;

        BOOL opt = TRUE;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));
        setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&opt), sizeof(opt));

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
        BOOL bNewBehavior = FALSE;
        DWORD dwBytesReturned = 0;
        WSAIoctl(s, SIO_UDP_CONNRESET, &bNewBehavior, sizeof(bNewBehavior), NULL, 0, &dwBytesReturned, NULL, NULL);

        // Detect local subnet (e.g. 192.168.68.x)
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

        // Non-blocking mode
        u_long mode = 1;
        ioctlsocket(s, FIONBIO, &mode);

        sockaddr_in bindAddr{};
        bindAddr.sin_family = AF_INET;
        bindAddr.sin_port = htons(rxPort);
        bindAddr.sin_addr.s_addr = htonl(INADDR_ANY);

        if (bind(s, reinterpret_cast<sockaddr*>(&bindAddr), sizeof(bindAddr)) == SOCKET_ERROR)
        {
            closesocket(s);
            return false;
        }

        rxSocket = static_cast<std::uintptr_t>(s);
        running = true;
        return true;
    }

    void AogUdpBridge::stop()
    {
        running = false;
        if (rxSocket != ~0ULL)
        {
            closesocket(static_cast<SOCKET>(rxSocket));
            rxSocket = ~0ULL;
            WSACleanup();
        }
    }

    void AogUdpBridge::poll()
    {
        if (!running || rxSocket == ~0ULL) return;

        SOCKET s = static_cast<SOCKET>(rxSocket);
        std::uint8_t buf[1024];
        sockaddr_in srcAddr{};
        int srcLen = sizeof(srcAddr);

        while (true)
        {
            srcLen = sizeof(srcAddr);
            int received = recvfrom(s, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                                    reinterpret_cast<sockaddr*>(&srcAddr), &srcLen);
            if (received <= 0) break;

            rxPackets++;
            lastRxTimestamp = isobus::SystemTiming::get_timestamp_ms();

            // Framing: 0x80 0x81 [src] [pgn] [len] [payload...] [crc]
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
                        // 0xE5: 64 Sections Setpoints Bitfield
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
        txBuf[idx++] = autoMode ? 1 : 0;
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

        // Send to loopback 127.0.0.1:9999
        sockaddr_in loopAddr{};
        loopAddr.sin_family = AF_INET;
        loopAddr.sin_port = htons(txPort);
        loopAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        sendto(s, reinterpret_cast<const char*>(txBuf), static_cast<int>(idx), 0,
               reinterpret_cast<sockaddr*>(&loopAddr), sizeof(loopAddr));

        // Send to general broadcast 255.255.255.255:9999
        sockaddr_in bcastAny{};
        bcastAny.sin_family = AF_INET;
        bcastAny.sin_port = htons(txPort);
        bcastAny.sin_addr.s_addr = htonl(INADDR_BROADCAST);
        sendto(s, reinterpret_cast<const char*>(txBuf), static_cast<int>(idx), 0,
               reinterpret_cast<sockaddr*>(&bcastAny), sizeof(bcastAny));

        // Send to LAN broadcast <subnet>.255:9999
        sockaddr_in bcastAddr{};
        bcastAddr.sin_family = AF_INET;
        bcastAddr.sin_port = htons(txPort);
        std::string bcastIp = std::to_string(subnet[0]) + "." + std::to_string(subnet[1]) + "." + std::to_string(subnet[2]) + ".255";
        inet_pton(AF_INET, bcastIp.c_str(), &bcastAddr.sin_addr);
        sendto(s, reinterpret_cast<const char*>(txBuf), static_cast<int>(idx), 0,
               reinterpret_cast<sockaddr*>(&bcastAddr), sizeof(bcastAddr));

        txPackets++;
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
        tcServer->initialize();
        tcServer->set_task_totals_active(false);
        tcServer->onFallbackSectionChanged = [this](std::uint8_t sec, bool state) {
            log_message("AgOpenGPS commanded Section " + std::to_string(sec + 1) + " -> " + (state ? "ON" : "OFF"));
            if (onSectionStateChanged)
                onSectionStateChanged(sec, state);
        };

        // 4. Announce TC Functionalities via PGN 64654
        tcFunctionalities = std::make_unique<isobus::ControlFunctionFunctionalities>(tcCF);
        tcFunctionalities->set_functionality_is_supported(
            isobus::ControlFunctionFunctionalities::Functionalities::TaskControllerBasicServer, 1, true);
        tcFunctionalities->set_functionality_is_supported(
            isobus::ControlFunctionFunctionalities::Functionalities::TaskControllerGeoServer, 1, false);
        tcFunctionalities->set_functionality_is_supported(
            isobus::ControlFunctionFunctionalities::Functionalities::TaskControllerSectionControlServer, 1, true);
        tcFunctionalities->set_task_controller_section_control_server_option_state(1, 64);
        log_message("TC announced TC-BAS and TC-SC (1 boom / 64 sections) on CAN");

        // 5. Announce TECU Functionalities and initialize Speed Messages Interface
        tecuFunctionalities = std::make_unique<isobus::ControlFunctionFunctionalities>(tecuCF);
        tecuFunctionalities->set_functionality_is_supported(
            isobus::ControlFunctionFunctionalities::Functionalities::BasicTractorECUServer, 2, true);
        tecuFunctionalities->set_basic_tractor_ECU_server_option_state(
            isobus::ControlFunctionFunctionalities::BasicTractorECUOptions::Class1NoOptions, true);

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
        };

        udpBridge->onSectionControlModeReceived = [this](bool autoMode) {
            if (tcServer)
                tcServer->update_section_control_enabled(autoMode);
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
            if (onSpeedChanged)
            {
                onSpeedChanged(speedKph);
            }
        };

        log_message("TC Server & TECU online on ISOBUS channel " + std::to_string(canChannelIndex));
        return true;
    }

    void TaskControllerManager::update()
    {
        if (udpBridge)
        {
            udpBridge->poll();

            // Send 100 ms heartbeat PGN 0xF0 to AgOpenGPS / AgIO
            std::uint32_t now = isobus::SystemTiming::get_timestamp_ms();
            if (now - lastHeartbeatMs >= 100)
            {
                lastHeartbeatMs = now;
                bool autoMode = tcServer ? tcServer->is_auto_mode() : true;
                std::uint8_t nSec = tcServer ? tcServer->get_total_sections() : 0;
                std::vector<bool> actual = tcServer ? tcServer->get_actual_section_states() : std::vector<bool>();
                udpBridge->send_heartbeat(autoMode, nSec, actual);
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
            tcServer->update();
        }

        // Only broadcast TECU Speed (0xFE48 / 0xFE49) when AgOpenGPS is actively connected and providing live speed
        if (speedMessagesInterface && aogConnected)
        {
            speedMessagesInterface->update();
        }
    }

    void TaskControllerManager::shutdown()
    {
        if (udpBridge)
        {
            udpBridge->stop();
            udpBridge.reset();
        }
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

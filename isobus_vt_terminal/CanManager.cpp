#include "CanManager.hpp"
#include "IsobusFileServer.hpp"
#include "DdiExplorerManager.hpp"
#include "TractorEcuManager.hpp"
#include "isobus/hardware_integration/PCANBasic.h"
#include "isobus/isobus/can_identifier.hpp"
#include "isobus/isobus/can_stack_logger.hpp"
#include "isobus/utility/system_timing.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <chrono>

namespace isobus_vt
{
    static CanManager *g_ActiveCanManager = nullptr;
    CanManager* CanManager::get_active_instance() { return g_ActiveCanManager; }
    CanManager::CanManager()
    {
        g_ActiveCanManager = this;
        isobus::CANHardwareInterface::get_can_frame_received_event_dispatcher().add_listener([this](const isobus::CANMessageFrame &frame) {
            on_frame_received(frame);
        });

        isobus::CANHardwareInterface::get_can_frame_transmitted_event_dispatcher().add_listener([this](const isobus::CANMessageFrame &frame) {
            on_frame_transmitted(frame);
        });
    }

    CanManager::~CanManager()
    {
        if (g_ActiveCanManager == this) g_ActiveCanManager = nullptr;
        shutdown();
    }

    bool CanManager::initialize(CanDriverType driverType, int pcanChannel)
    {
        currentDriverType = driverType;
        currentPcanChannel = pcanChannel;

        if (isobus::CANHardwareInterface::is_running())
        {
            isobus::CANHardwareInterface::stop();
        }

        isobus::CANHardwareInterface::set_number_of_can_channels(1, 256);

        if (driverType == CanDriverType::PeakPcan)
        {
            CAN_Uninitialize(static_cast<TPCANHandle>(currentPcanChannel));
            CAN_Uninitialize(PCAN_NONEBUS);
            activeDriver = std::make_shared<isobus::PCANBasicWindowsPlugin>(static_cast<WORD>(currentPcanChannel));
        }
        else
        {
            activeDriver = std::make_shared<isobus::VirtualCANPlugin>();
        }

        isobus::CANHardwareInterface::assign_can_channel_frame_handler(0, activeDriver);
        bool started = isobus::CANHardwareInterface::start();

        if (started && activeDriver && activeDriver->get_is_valid())
        {
            connected = true;
            lastError.clear();
            LOG_INFO("[CanManager]: CAN driver %s successfully opened on channel %d",
                     activeDriver->get_name().c_str(), currentPcanChannel);
            return true;
        }
        else
        {
            connected = false;
            if (activeDriver)
                lastError = activeDriver->get_last_error();
            if (lastError.empty())
                lastError = "Failed to initialize CAN driver hardware";
            LOG_ERROR("[CanManager]: %s", lastError.c_str());
            return false;
        }
    }

    void CanManager::shutdown()
    {
        if (isobus::CANHardwareInterface::is_running())
        {
            isobus::CANHardwareInterface::stop();
        }
        if (currentDriverType == CanDriverType::PeakPcan)
        {
            CAN_Uninitialize(static_cast<TPCANHandle>(currentPcanChannel));
        }
        connected = false;
        activeDriver = nullptr;
    }

    bool CanManager::reconnect()
    {
        shutdown();
        return initialize(currentDriverType, currentPcanChannel);
    }

    void CanManager::set_driver_type(CanDriverType type)
    {
        currentDriverType = type;
    }

    CanDriverType CanManager::get_driver_type() const
    {
        return currentDriverType;
    }

    void CanManager::set_pcan_channel(int channel)
    {
        currentPcanChannel = channel;
    }

    int CanManager::get_pcan_channel() const
    {
        return currentPcanChannel;
    }

    bool CanManager::is_connected() const
    {
        return connected && activeDriver && activeDriver->get_is_valid();
    }

    std::string CanManager::get_status_string() const
    {
        if (is_connected())
        {
            if (currentDriverType == CanDriverType::PeakPcan)
            {
                std::ostringstream ss;
                ss << "PEAK PCAN (USB " << (currentPcanChannel - PCAN_USBBUS1 + 1) << ") - 250k";
                return ss.str();
            }
            return "Virtual Loopback CAN - Active";
        }
        return "Disconnected (" + (lastError.empty() ? "Hardware unavailable" : lastError) + ")";
    }

    std::string CanManager::get_last_error() const
    {
        return lastError;
    }

    std::uint64_t CanManager::get_rx_count() const
    {
        return totalRxFrames;
    }

    std::uint64_t CanManager::get_tx_count() const
    {
        return totalTxFrames;
    }

    float CanManager::get_fps() const
    {
        return currentFps;
    }

    float CanManager::get_bus_load() const
    {
        return currentBusLoadPercent.load();
    }

    std::vector<float> CanManager::get_bus_load_history() const
    {
        std::lock_guard<std::mutex> lock(logMutex);
        return std::vector<float>(busLoadHistory.begin(), busLoadHistory.end());
    }

    std::vector<DiagnosticTroubleCode> CanManager::get_active_dtcs() const
    {
        std::lock_guard<std::mutex> lock(dtcMutex);
        return activeDtcs;
    }

    std::vector<DiagnosticTroubleCode> CanManager::get_historical_dtcs() const
    {
        std::lock_guard<std::mutex> lock(dtcMutex);
        return historicalDtcs;
    }

    void CanManager::clear_dtcs()
    {
        std::lock_guard<std::mutex> lock(dtcMutex);
        activeDtcs.clear();
        historicalDtcs.clear();
    }

    std::string CanManager::get_fmi_description(std::uint8_t fmi)
    {
        switch (fmi)
        {
        case 0: return "Data Valid But Above Normal Range - Most Severe";
        case 1: return "Data Valid But Below Normal Range - Most Severe";
        case 2: return "Data Erratic, Intermittent Or Incorrect";
        case 3: return "Voltage Above Normal, Or Shorted High";
        case 4: return "Voltage Below Normal, Or Shorted Low";
        case 5: return "Current Below Normal Or Open Circuit";
        case 6: return "Current Above Normal Or Grounded Circuit";
        case 7: return "Mechanical System Not Responding Properly";
        case 8: return "Abnormal Frequency, Pulse Width Or Period";
        case 9: return "Abnormal Update Rate";
        case 10: return "Abnormal Rate Of Change";
        case 11: return "Root Cause Not Known";
        case 12: return "Bad Intelligent Device Or Component";
        case 13: return "Out Of Calibration";
        case 14: return "Special Instructions";
        case 15: return "Data Above Normal - Least Severe";
        case 16: return "Data Above Normal - Moderately Severe";
        case 17: return "Data Below Normal - Least Severe";
        case 18: return "Data Below Normal - Moderately Severe";
        case 19: return "Received Network Data In Error";
        case 31: return "Condition Exists";
        default: return "Reserved / Manufacturer Specific";
        }
    }

    std::vector<CanLogEntry> CanManager::get_recent_frames(std::size_t maxCount)
    {
        std::lock_guard<std::mutex> lock(logMutex);
        std::vector<CanLogEntry> list;
        std::size_t start = (logEntries.size() > maxCount) ? (logEntries.size() - maxCount) : 0;
        for (std::size_t i = start; i < logEntries.size(); ++i)
        {
            list.push_back(logEntries[i]);
        }
        return list;
    }

    void CanManager::clear_log()
    {
        std::lock_guard<std::mutex> lock(logMutex);
        logEntries.clear();
    }

    void CanManager::set_log_paused(bool paused)
    {
        logPaused = paused;
    }

    bool CanManager::is_log_paused() const
    {
        return logPaused;
    }

    void CanManager::process_j1939_diagnostics(const isobus::CANMessageFrame &frame)
    {
        isobus::CANIdentifier id(frame.identifier);
        std::uint32_t pgn = id.get_parameter_group_number();

        if (pgn != 0xFECA && pgn != 0xFECB)
            return; // Only DM1 (Active) and DM2 (Historical)

        if (frame.dataLength < 6)
            return;

        bool isDM2 = (pgn == 0xFECB);
        std::uint8_t src = id.get_source_address();

        bool mil = ((frame.data[0] >> 6) & 0x03) == 0x01;
        bool redStop = ((frame.data[0] >> 4) & 0x03) == 0x01;
        bool amberWarn = ((frame.data[0] >> 2) & 0x03) == 0x01;
        bool protect = (frame.data[0] & 0x03) == 0x01;

        std::lock_guard<std::mutex> lock(dtcMutex);
        auto &list = isDM2 ? historicalDtcs : activeDtcs;

        // Clear previous reports from this ECU
        for (auto it = list.begin(); it != list.end(); )
        {
            if (it->sourceAddress == src)
                it = list.erase(it);
            else
                ++it;
        }

        // Process each 4-byte DTC block
        for (std::uint8_t offset = 2; offset + 4 <= frame.dataLength; offset += 4)
        {
            std::uint32_t spn = static_cast<std::uint32_t>(frame.data[offset]) |
                                (static_cast<std::uint32_t>(frame.data[offset + 1]) << 8) |
                                ((static_cast<std::uint32_t>(frame.data[offset + 2]) & 0xE0) << 11);
            std::uint8_t fmi = frame.data[offset + 2] & 0x1F;
            std::uint8_t count = frame.data[offset + 3] & 0x7F;

            // SPN 0 / FMI 0 means no active DTCs
            if (spn == 0 && fmi == 0)
                continue;

            DiagnosticTroubleCode dtc;
            dtc.sourceAddress = src;
            dtc.spn = spn;
            dtc.fmi = fmi;
            dtc.occurrenceCount = count;
            dtc.milLamp = mil;
            dtc.redStopLamp = redStop;
            dtc.amberWarningLamp = amberWarn;
            dtc.protectLamp = protect;
            dtc.isPreviouslyActive = isDM2;
            dtc.timestampMs = isobus::SystemTiming::get_timestamp_ms();
            dtc.fmiDescription = get_fmi_description(fmi);

            list.push_back(dtc);
        }
    }

    void CanManager::on_frame_received(const isobus::CANMessageFrame &frame)
    {
        totalRxFrames++;
        framesLastSecond++;

        // Calculate approximate wire bits including bit stuffing (~150 bits for 8-byte frame at 250k)
        std::uint32_t frameBits = static_cast<std::uint32_t>((67 + 8 * frame.dataLength) * 1.15f);
        bitsLastSecond += frameBits;

        auto now = isobus::SystemTiming::get_timestamp_ms();
        if (isobus::SystemTiming::time_expired_ms(lastFpsCalcTimestamp, 1000))
        {
            currentFps = static_cast<float>(framesLastSecond.exchange(0));
            std::uint32_t bits = bitsLastSecond.exchange(0);
            float load = (static_cast<float>(bits) / 250000.0f) * 100.0f;
            if (load > 100.0f) load = 100.0f;
            currentBusLoadPercent = load;

            std::lock_guard<std::mutex> lock(logMutex);
            if (busLoadHistory.size() >= 60)
                busLoadHistory.pop_front();
            busLoadHistory.push_back(load);

            lastFpsCalcTimestamp = now;
        }

        isobus::CANIdentifier id(frame.identifier);

        // Process J1939 DM1/DM2 diagnostics
        process_j1939_diagnostics(frame);

        // Forward to TractorEcuManager if configured as SharedWithCan1
        if (TractorEcuManager::get_instance().get_source() == TractorCanSource::SharedWithCan1)
        {
            TractorEcuManager::get_instance().process_can_frame(frame.identifier, frame.data, frame.dataLength);
        }

        // Forward to IsobusFileServer (ISO 11783-13) ONLY for File Server request (0xB100)
        std::uint32_t pgnVal = id.get_parameter_group_number();
        if ((pgnVal & 0xFF00) == 0xB100 || pgnVal == 45312)
        {
            IsobusFileServer::get_instance().process_can_frame(
                pgnVal,
                id.get_source_address(),
                id.get_destination_address(),
                frame.data,
                frame.dataLength);
        }

        // Forward to DdiExplorerManager (ISO 11783-11) ONLY for Process Data (0xCB00 or 0xFE4A)
        if (pgnVal == 0xCB00 || pgnVal == 0xFE4A)
        {
            DdiExplorerManager::get_instance().record_process_data_from_can(
                pgnVal,
                id.get_source_address(),
                frame.data,
                frame.dataLength);
        }

        if (id.get_source_address() == 132)
        {
            char buf[128];
            snprintf(buf, sizeof(buf), "[RAW CAN RX 132] ID: 0x%08X (PGN: 0x%05X, DA: 0x%02X) [DLC %u]: %02X %02X %02X %02X %02X %02X %02X %02X",
                     frame.identifier, id.get_parameter_group_number(), id.get_destination_address(), frame.dataLength,
                     frame.data[0], frame.data[1], frame.data[2], frame.data[3], frame.data[4], frame.data[5], frame.data[6], frame.data[7]);
            isobus::CANStackLogger::info(buf);
        }

        // Continuous logging to can_traffic.log
        {
            char hexData[32] = {0};
            for (std::uint8_t i = 0; i < frame.dataLength && i < 8; ++i)
            {
                snprintf(hexData + (i * 3), 4, "%02X ", frame.data[i]);
            }
            std::string annotation;
            if (frame.identifier == 0x18E6FFCE)
            {
                bool master = (frame.data[3] & 0x80) != 0;
                annotation = " | [AMAZONE AMACLICK RX] Master:" + std::string(master ? "ON" : "OFF") + " Sekcje:";
                for (int s = 0; s < 8; ++s) if (frame.data[2] & (1 << s)) annotation += " S" + std::to_string(s + 1);
                if (frame.data[3] & 0x01) annotation += " S9";
            }
            else if (frame.identifier == 0x18EEFFCE)
            {
                annotation = " | [AMAZONE CLAIM RX] Adres 0xCE";
            }

            char logLine[256];
            snprintf(logLine, sizeof(logLine), "[RX] ID: 0x%08X [DLC %u] DATA: %-24s PGN: 0x%05X SA: 0x%02X DA: 0x%02X%s",
                     frame.identifier, frame.dataLength, hexData,
                     id.get_parameter_group_number(), id.get_source_address(), id.get_destination_address(),
                     annotation.c_str());
            write_file_log(logLine);
        }

        if (logPaused)
            return;

        CanLogEntry entry;
        entry.timestampMs = now;
        entry.id = frame.identifier;
        entry.pgn = id.get_parameter_group_number();
        entry.source = id.get_source_address();
        entry.dest = id.get_destination_address();
        entry.dlc = frame.dataLength;
        std::memcpy(entry.data, frame.data, std::min<std::uint8_t>(frame.dataLength, 8));
        entry.isTx = false;
        entry.pgnName = format_pgn_name(entry.pgn);

        std::lock_guard<std::mutex> lock(logMutex);
        if (logEntries.size() > 1000)
            logEntries.pop_front();
        logEntries.push_back(entry);
    }

    void CanManager::on_frame_transmitted(const isobus::CANMessageFrame &frame)
    {
        totalTxFrames++;
        framesLastSecond++;

        std::uint32_t frameBits = static_cast<std::uint32_t>((67 + 8 * frame.dataLength) * 1.15f);
        bitsLastSecond += frameBits;

        auto now = isobus::SystemTiming::get_timestamp_ms();
        isobus::CANIdentifier id(frame.identifier);

        if (id.get_destination_address() == 132 || id.get_source_address() == 132)
        {
            char buf[128];
            snprintf(buf, sizeof(buf), "[RAW CAN TX to 132] ID: 0x%08X (PGN: 0x%05X, DA: 0x%02X) [DLC %u]: %02X %02X %02X %02X %02X %02X %02X %02X",
                     frame.identifier, id.get_parameter_group_number(), id.get_destination_address(), frame.dataLength,
                     frame.data[0], frame.data[1], frame.data[2], frame.data[3], frame.data[4], frame.data[5], frame.data[6], frame.data[7]);
            isobus::CANStackLogger::info(buf);
        }

        // Continuous logging to can_traffic.log
        {
            char hexData[32] = {0};
            for (std::uint8_t i = 0; i < frame.dataLength && i < 8; ++i)
            {
                snprintf(hexData + (i * 3), 4, "%02X ", frame.data[i]);
            }
            std::string annotation;
            if (frame.identifier == 0x18E6FFCE)
            {
                bool master = (frame.data[3] & 0x80) != 0;
                annotation = " | [AMAZONE AMACLICK TX] Master:" + std::string(master ? "ON" : "OFF") + " Sekcje:";
                for (int s = 0; s < 8; ++s) if (frame.data[2] & (1 << s)) annotation += " S" + std::to_string(s + 1);
                if (frame.data[3] & 0x01) annotation += " S9";
            }
            else if (frame.identifier == 0x18EEFFCE)
            {
                annotation = " | [AMAZONE CLAIM TX] Adres 0xCE zgloszony do magistrali";
            }

            char logLine[256];
            snprintf(logLine, sizeof(logLine), "[TX] ID: 0x%08X [DLC %u] DATA: %-24s PGN: 0x%05X SA: 0x%02X DA: 0x%02X%s",
                     frame.identifier, frame.dataLength, hexData,
                     id.get_parameter_group_number(), id.get_source_address(), id.get_destination_address(),
                     annotation.c_str());
            write_file_log(logLine);
        }

        if (logPaused)
            return;

        CanLogEntry entry;
        entry.timestampMs = now;
        entry.id = frame.identifier;
        entry.pgn = id.get_parameter_group_number();
        entry.source = id.get_source_address();
        entry.dest = id.get_destination_address();
        entry.dlc = frame.dataLength;
        std::memcpy(entry.data, frame.data, std::min<std::uint8_t>(frame.dataLength, 8));
        entry.isTx = true;
        entry.pgnName = format_pgn_name(entry.pgn);

        std::lock_guard<std::mutex> lock(logMutex);
        if (logEntries.size() > 1000)
            logEntries.pop_front();
        logEntries.push_back(entry);
    }

    std::string CanManager::format_pgn_name(std::uint32_t pgn)
    {
        switch (pgn)
        {
            case 0xE600: return "VT to ECU";
            case 0xE700: return "ECU to VT";
            case 0xEE00: return "Address Claim";
            case 0xEA00: return "PGN Request";
            case 0xEC00: return "TP.CM (Conn Mgmt)";
            case 0xEB00: return "TP.DT (Data Transfer)";
            case 0xC800: return "ETP.CM (Ext Conn)";
            case 0xC700: return "ETP.DT (Ext Data)";
            case 0xFE0D: return "Working Set Master";
            case 0xFE0F: return "Language Command";
            case 0xFE48: return "Wheel Speed / AUX";
            case 0xFE49: return "Ground Speed";
            case 0xFECA: return "Active DTCs (DM1)";
            case 0xFECB: return "Previous DTCs (DM2)";
            case 0xCB00: return "Process Data (TC)";
            case 0xB100: return "File Server Req (ISO 11783-13)";
            case 0xB000: return "File Server Resp (ISO 11783-13)";
            default:
            {
                std::ostringstream ss;
                ss << "PGN 0x" << std::hex << std::uppercase << pgn;
                return ss.str();
            }
        }
    }

    std::string CanManager::get_file_log_path()
    {
        return "can_traffic.log";
    }

    void CanManager::init_file_log()
    {
        if (!fileLogStream.is_open())
        {
            fileLogStream.open(get_file_log_path(), std::ios::out | std::ios::app);
            if (fileLogStream.is_open())
            {
                fileLogStream << "\n======================================================================\n";
                fileLogStream << "=== CAN TRAFFIC LOG SESSION INITIALIZED (PEAK PCAN 250 kbps) ===\n";
                fileLogStream << "======================================================================\n";
                fileLogStream.flush();
            }
        }
    }

    void CanManager::clear_file_log()
    {
        std::lock_guard<std::mutex> lock(fileLogMutex);
        if (fileLogStream.is_open()) fileLogStream.close();
        fileLogStream.open(get_file_log_path(), std::ios::out | std::ios::trunc);
        if (fileLogStream.is_open())
        {
            fileLogStream << "=== CAN TRAFFIC LOG CLEARED ===\n";
            fileLogStream.flush();
        }
        fileLogLinesWritten = 0;
    }

    void CanManager::write_file_log(const std::string &line)
    {
        std::lock_guard<std::mutex> lock(fileLogMutex);
        if (!fileLogStream.is_open())
        {
            init_file_log();
        }
        if (fileLogStream.is_open())
        {
            auto now = std::chrono::system_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
            auto timer = std::chrono::system_clock::to_time_t(now);
            std::tm bt;
#ifdef _WIN32
            localtime_s(&bt, &timer);
#else
            localtime_r(&timer, &bt);
#endif
            char timeBuf[32];
            snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d:%02d.%03d",
                     bt.tm_hour, bt.tm_min, bt.tm_sec, static_cast<int>(ms.count()));

            fileLogStream << "[" << timeBuf << "] " << line << "\n";
            fileLogStream.flush();
            fileLogLinesWritten++;
        }
    }
}

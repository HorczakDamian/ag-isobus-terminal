#include "IsobusFileServer.hpp"
#include "CanManager.hpp"
#include "isobus/isobus/can_hardware_abstraction.hpp"
#include "imgui.h"

#include <chrono>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <cstring>
#include <windows.h>
#include <shellapi.h>

namespace isobus_vt
{
    static std::uint32_t get_now_ms()
    {
        return static_cast<std::uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    IsobusFileServer& IsobusFileServer::get_instance()
    {
        static IsobusFileServer instance;
        return instance;
    }

    IsobusFileServer::IsobusFileServer()
    {
        initialize("isobus_file_server");
    }

    IsobusFileServer::~IsobusFileServer()
    {
        shutdown();
    }

    bool IsobusFileServer::initialize(const std::string &storageDirectory)
    {
        std::lock_guard<std::mutex> lock(serverMutex);
        rootStoragePath = storageDirectory;

        try
        {
            if (!std::filesystem::exists(rootStoragePath))
            {
                std::filesystem::create_directories(rootStoragePath);
            }

            // Create a default demo implement calibration file if empty
            std::string sampleFile = rootStoragePath + "/IMPLEMENT_CALIB.BIN";
            if (!std::filesystem::exists(sampleFile))
            {
                std::ofstream f(sampleFile, std::ios::binary);
                if (f.is_open())
                {
                    const char sampleData[] = "ISO11783-13_FILE_SERVER_PROFILE_DATA_v1.0";
                    f.write(sampleData, sizeof(sampleData) - 1);
                }
            }
        }
        catch (...)
        {
            return false;
        }

        return true;
    }

    void IsobusFileServer::shutdown()
    {
        std::lock_guard<std::mutex> lock(serverMutex);
        activeHandles.clear();
    }

    void IsobusFileServer::update()
    {
        // Periodic cleanup of stale handles (> 60 seconds of inactivity)
        std::lock_guard<std::mutex> lock(serverMutex);
        std::uint32_t now = get_now_ms();
        for (auto it = activeHandles.begin(); it != activeHandles.end(); )
        {
            if (now - it->second.openTimestampMs > 60000)
            {
                it = activeHandles.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    std::uint32_t IsobusFileServer::get_active_handles_count() const
    {
        std::lock_guard<std::mutex> lock(serverMutex);
        return static_cast<std::uint32_t>(activeHandles.size());
    }

    void IsobusFileServer::set_storage_path(const std::string &path)
    {
        std::lock_guard<std::mutex> lock(serverMutex);
        rootStoragePath = path;
        if (!std::filesystem::exists(rootStoragePath))
        {
            std::filesystem::create_directories(rootStoragePath);
        }
    }

    std::string IsobusFileServer::sanitize_path(const std::string &requestedPath)
    {
        std::string clean = requestedPath;
        std::replace(clean.begin(), clean.end(), '\\', '/');
        while (clean.find("../") != std::string::npos)
        {
            clean.erase(clean.find("../"), 3);
        }
        if (!clean.empty() && clean[0] == '/')
        {
            clean.erase(0, 1);
        }
        return rootStoragePath + "/" + clean;
    }

    void IsobusFileServer::process_can_frame(std::uint32_t pgn, std::uint8_t sa, std::uint8_t da, const std::uint8_t *data, std::uint32_t len)
    {
        if (!serverEnabled || !data || len < 1) return;

        // PGN 45312 (0xB100) is the ISO 11783-13 Client to File Server request
        if (pgn != 45312 && (pgn & 0xFF00) != 0xB100) return;

        std::uint8_t cmdByte = data[0];
        auto cmd = static_cast<FileServerCommand>(cmdByte);
        totalRequests++;

        switch (cmd)
        {
            case FileServerCommand::OpenFile:
                handle_open_file(sa, data, len);
                break;
            case FileServerCommand::ReadFile:
                handle_read_file(sa, data, len);
                break;
            case FileServerCommand::WriteFile:
                handle_write_file(sa, data, len);
                break;
            case FileServerCommand::CloseFile:
                handle_close_file(sa, data, len);
                break;
            case FileServerCommand::GetVolumeInfo:
                handle_get_volume_info(sa);
                break;
            case FileServerCommand::ServerStatus:
                handle_status_request(sa);
                break;
            default:
                break;
        }
    }

    void IsobusFileServer::handle_open_file(std::uint8_t sa, const std::uint8_t *data, std::uint32_t len)
    {
        std::lock_guard<std::mutex> lock(serverMutex);
        if (len < 3) return;

        std::uint8_t mode = data[1];
        std::string fileName;
        for (std::uint32_t i = 2; i < len; ++i)
        {
            if (data[i] == 0) break;
            fileName += static_cast<char>(data[i]);
        }

        std::string fullPath = sanitize_path(fileName);
        FileServerError err = FileServerError::Ok;
        std::uint8_t assignedHandle = 0;

        if (mode == 1 || mode == 3) // Read or ReadWrite
        {
            if (!std::filesystem::exists(fullPath))
            {
                err = FileServerError::FileNotFound;
            }
        }
        else if (mode == 2 || mode == 4) // Write or Create
        {
            std::ofstream out(fullPath, std::ios::binary | ((mode == 4) ? std::ios::trunc : std::ios::app));
            if (!out.is_open())
            {
                err = FileServerError::AccessDenied;
            }
        }

        if (err == FileServerError::Ok)
        {
            assignedHandle = nextHandleId++;
            if (nextHandleId == 0) nextHandleId = 1;

            FileServerHandle h;
            h.handleId = assignedHandle;
            h.relativePath = fileName;
            h.fullPath = fullPath;
            h.accessMode = mode;
            h.filePointer = 0;
            h.fileSize = std::filesystem::exists(fullPath) ? static_cast<std::uint32_t>(std::filesystem::file_size(fullPath)) : 0;
            h.clientSourceAddress = sa;
            h.openTimestampMs = get_now_ms();

            activeHandles[assignedHandle] = h;
        }

        // Response payload: [Command, ErrorCode, HandleId, FileSize(4B)]
        std::vector<std::uint8_t> resp(7, 0);
        resp[0] = static_cast<std::uint8_t>(FileServerCommand::OpenFile);
        resp[1] = static_cast<std::uint8_t>(err);
        resp[2] = assignedHandle;
        std::uint32_t sz = (err == FileServerError::Ok) ? activeHandles[assignedHandle].fileSize : 0;
        resp[3] = static_cast<std::uint8_t>(sz & 0xFF);
        resp[4] = static_cast<std::uint8_t>((sz >> 8) & 0xFF);
        resp[5] = static_cast<std::uint8_t>((sz >> 16) & 0xFF);
        resp[6] = static_cast<std::uint8_t>((sz >> 24) & 0xFF);

        send_can_response(sa, resp);

        FileServerLogEntry log;
        log.timestampMs = get_now_ms();
        log.clientSA = sa;
        log.commandName = "OPEN";
        log.details = fileName + " (Mode=" + std::to_string(mode) + ", Handle=" + std::to_string(assignedHandle) + ")";
        log.result = err;
        activityLog.push_back(log);
    }

    void IsobusFileServer::handle_read_file(std::uint8_t sa, const std::uint8_t *data, std::uint32_t len)
    {
        std::lock_guard<std::mutex> lock(serverMutex);
        if (len < 4) return;

        std::uint8_t handle = data[1];
        std::uint16_t bytesRequested = static_cast<std::uint16_t>(data[2]) | (static_cast<std::uint16_t>(data[3]) << 8);

        auto it = activeHandles.find(handle);
        if (it == activeHandles.end())
        {
            std::vector<std::uint8_t> resp = { static_cast<std::uint8_t>(FileServerCommand::ReadFile), static_cast<std::uint8_t>(FileServerError::InvalidHandle), handle, 0, 0 };
            send_can_response(sa, resp);
            return;
        }

        it->second.openTimestampMs = get_now_ms();
        std::ifstream in(it->second.fullPath, std::ios::binary);
        if (!in.is_open())
        {
            std::vector<std::uint8_t> resp = { static_cast<std::uint8_t>(FileServerCommand::ReadFile), static_cast<std::uint8_t>(FileServerError::AccessDenied), handle, 0, 0 };
            send_can_response(sa, resp);
            return;
        }

        in.seekg(it->second.filePointer, std::ios::beg);
        std::vector<char> buf(bytesRequested, 0);
        in.read(buf.data(), bytesRequested);
        std::streamsize bytesRead = in.gcount();
        it->second.filePointer += static_cast<std::uint32_t>(bytesRead);
        totalBytesRead += bytesRead;

        std::vector<std::uint8_t> resp;
        resp.push_back(static_cast<std::uint8_t>(FileServerCommand::ReadFile));
        resp.push_back(static_cast<std::uint8_t>(FileServerError::Ok));
        resp.push_back(handle);
        resp.push_back(static_cast<std::uint8_t>(bytesRead & 0xFF));
        resp.push_back(static_cast<std::uint8_t>((bytesRead >> 8) & 0xFF));
        for (std::streamsize i = 0; i < bytesRead && i < 32; ++i)
        {
            resp.push_back(static_cast<std::uint8_t>(buf[i]));
        }

        send_can_response(sa, resp);

        FileServerLogEntry log;
        log.timestampMs = get_now_ms();
        log.clientSA = sa;
        log.commandName = "READ";
        log.details = "H=" + std::to_string(handle) + ", Read=" + std::to_string(bytesRead) + "B";
        log.result = FileServerError::Ok;
        activityLog.push_back(log);
    }

    void IsobusFileServer::handle_write_file(std::uint8_t sa, const std::uint8_t *data, std::uint32_t len)
    {
        std::lock_guard<std::mutex> lock(serverMutex);
        if (len < 4) return;

        std::uint8_t handle = data[1];
        std::uint16_t bytesCount = static_cast<std::uint16_t>(data[2]) | (static_cast<std::uint16_t>(data[3]) << 8);

        auto it = activeHandles.find(handle);
        if (it == activeHandles.end())
        {
            std::vector<std::uint8_t> resp = { static_cast<std::uint8_t>(FileServerCommand::WriteFile), static_cast<std::uint8_t>(FileServerError::InvalidHandle), handle, 0, 0 };
            send_can_response(sa, resp);
            return;
        }

        it->second.openTimestampMs = get_now_ms();
        std::ofstream out(it->second.fullPath, std::ios::binary | std::ios::in | std::ios::out);
        if (!out.is_open())
        {
            out.open(it->second.fullPath, std::ios::binary | std::ios::out);
        }

        if (out.is_open() && len >= 4 + bytesCount)
        {
            out.seekp(it->second.filePointer, std::ios::beg);
            out.write(reinterpret_cast<const char*>(data + 4), bytesCount);
            it->second.filePointer += bytesCount;
            if (it->second.filePointer > it->second.fileSize)
            {
                it->second.fileSize = it->second.filePointer;
            }
            totalBytesWritten += bytesCount;
        }

        std::vector<std::uint8_t> resp = {
            static_cast<std::uint8_t>(FileServerCommand::WriteFile),
            static_cast<std::uint8_t>(FileServerError::Ok),
            handle,
            static_cast<std::uint8_t>(bytesCount & 0xFF),
            static_cast<std::uint8_t>((bytesCount >> 8) & 0xFF)
        };
        send_can_response(sa, resp);

        FileServerLogEntry log;
        log.timestampMs = get_now_ms();
        log.clientSA = sa;
        log.commandName = "WRITE";
        log.details = "H=" + std::to_string(handle) + ", Written=" + std::to_string(bytesCount) + "B";
        log.result = FileServerError::Ok;
        activityLog.push_back(log);
    }

    void IsobusFileServer::handle_close_file(std::uint8_t sa, const std::uint8_t *data, std::uint32_t len)
    {
        std::lock_guard<std::mutex> lock(serverMutex);
        if (len < 2) return;

        std::uint8_t handle = data[1];
        FileServerError err = FileServerError::Ok;

        auto it = activeHandles.find(handle);
        if (it != activeHandles.end())
        {
            activeHandles.erase(it);
        }
        else
        {
            err = FileServerError::InvalidHandle;
        }

        std::vector<std::uint8_t> resp = {
            static_cast<std::uint8_t>(FileServerCommand::CloseFile),
            static_cast<std::uint8_t>(err),
            handle
        };
        send_can_response(sa, resp);

        FileServerLogEntry log;
        log.timestampMs = get_now_ms();
        log.clientSA = sa;
        log.commandName = "CLOSE";
        log.details = "H=" + std::to_string(handle);
        log.result = err;
        activityLog.push_back(log);
    }

    void IsobusFileServer::handle_get_volume_info(std::uint8_t sa)
    {
        // ISO 11783-13 Get Volume Info response: Total size (4B), Free space (4B) in MB
        std::uint32_t totalMb = 32768; // 32 GB Virtual Card
        std::uint32_t freeMb = 24500;  // 24 GB Free

        try
        {
            auto spaceInfo = std::filesystem::space(rootStoragePath);
            totalMb = static_cast<std::uint32_t>(spaceInfo.capacity / (1024 * 1024));
            freeMb = static_cast<std::uint32_t>(spaceInfo.available / (1024 * 1024));
        }
        catch (...) {}

        std::vector<std::uint8_t> resp(10, 0);
        resp[0] = static_cast<std::uint8_t>(FileServerCommand::GetVolumeInfo);
        resp[1] = static_cast<std::uint8_t>(FileServerError::Ok);
        resp[2] = static_cast<std::uint8_t>(totalMb & 0xFF);
        resp[3] = static_cast<std::uint8_t>((totalMb >> 8) & 0xFF);
        resp[4] = static_cast<std::uint8_t>((totalMb >> 16) & 0xFF);
        resp[5] = static_cast<std::uint8_t>((totalMb >> 24) & 0xFF);
        resp[6] = static_cast<std::uint8_t>(freeMb & 0xFF);
        resp[7] = static_cast<std::uint8_t>((freeMb >> 8) & 0xFF);
        resp[8] = static_cast<std::uint8_t>((freeMb >> 16) & 0xFF);
        resp[9] = static_cast<std::uint8_t>((freeMb >> 24) & 0xFF);

        send_can_response(sa, resp);
    }

    void IsobusFileServer::handle_status_request(std::uint8_t sa)
    {
        std::vector<std::uint8_t> resp = {
            static_cast<std::uint8_t>(FileServerCommand::ServerStatus),
            static_cast<std::uint8_t>(FileServerError::Ok),
            0x02, // ISO 11783-13 Version 2
            static_cast<std::uint8_t>(activeHandles.size())
        };
        send_can_response(sa, resp);
    }

    void IsobusFileServer::send_can_response(std::uint8_t da, const std::vector<std::uint8_t> &payload)
    {
        // PGN 45056 (0xB000): File Server Response (Server -> Client)
        isobus::CANMessageFrame txFrame;
        txFrame.identifier = 0x18B00000 | (static_cast<std::uint32_t>(da) << 8) | fileServerAddress;
        txFrame.isExtendedFrame = true;
        txFrame.dataLength = static_cast<std::uint8_t>(std::min<size_t>(payload.size(), 8));
        std::memset(txFrame.data, 0xFF, 8);
        std::memcpy(txFrame.data, payload.data(), txFrame.dataLength);
        txFrame.channel = 0;
        isobus::send_can_message_frame_to_hardware(txFrame);
    }

    void IsobusFileServer::simulate_client_file_transfer(const std::string &fileName, bool isWrite)
    {
        std::uint8_t clientSA = 0x81; // Implement ECU
        std::vector<std::uint8_t> openData;
        openData.push_back(static_cast<std::uint8_t>(FileServerCommand::OpenFile));
        openData.push_back(isWrite ? 4 : 1); // 4=Create, 1=Read
        for (char ch : fileName) openData.push_back(static_cast<std::uint8_t>(ch));
        openData.push_back(0);

        handle_open_file(clientSA, openData.data(), static_cast<std::uint32_t>(openData.size()));

        if (isWrite)
        {
            // Simulate write of 64 bytes calibration data
            std::vector<std::uint8_t> writeData;
            writeData.push_back(static_cast<std::uint8_t>(FileServerCommand::WriteFile));
            writeData.push_back(nextHandleId > 1 ? (nextHandleId - 1) : 1);
            writeData.push_back(16); // 16 bytes
            writeData.push_back(0);
            for (int i = 0; i < 16; ++i) writeData.push_back(static_cast<std::uint8_t>(0xA0 + i));
            handle_write_file(clientSA, writeData.data(), static_cast<std::uint32_t>(writeData.size()));
        }
        else
        {
            // Simulate read of 16 bytes
            std::vector<std::uint8_t> readData = {
                static_cast<std::uint8_t>(FileServerCommand::ReadFile),
                static_cast<std::uint8_t>(nextHandleId > 1 ? (nextHandleId - 1) : 1),
                16,
                0
            };
            handle_read_file(clientSA, readData.data(), static_cast<std::uint32_t>(readData.size()));
        }

        // Close
        std::vector<std::uint8_t> closeData = {
            static_cast<std::uint8_t>(FileServerCommand::CloseFile),
            static_cast<std::uint8_t>(nextHandleId > 1 ? (nextHandleId - 1) : 1)
        };
        handle_close_file(clientSA, closeData.data(), static_cast<std::uint32_t>(closeData.size()));
    }

    void IsobusFileServer::render_file_server_window(bool *p_open)
    {
        if (!p_open || !*p_open) return;

        ImGui::SetNextWindowSizeConstraints(ImVec2(650, 480), ImVec2(1920, 1080));
        ImGui::SetNextWindowSize(ImVec2(750, 560), ImGuiCond_FirstUseEver);

        if (!ImGui::Begin("ISOBUS File Server (ISO 11783-13)", p_open))
        {
            ImGui::End();
            return;
        }

        // Top Status Bar
        bool running = serverEnabled && (CanManager::get_active_instance() && CanManager::get_active_instance()->is_connected());
        ImGui::PushStyleColor(ImGuiCol_Text, running ? ImVec4(0.2f, 1.0f, 0.3f, 1.0f) : ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
        ImGui::Text(running ? "[ SERWER PLIKOW ISO 11783-13: AKTYWNY ]" : "[ SERWER PLIKOW: CZUWANIE / ROZLACZONY ]");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::Text("| SA: 0x%02X (%u)", fileServerAddress, fileServerAddress);
        ImGui::SameLine();
        ImGui::Text("| Zadania: %u", totalRequests);
        ImGui::SameLine();
        ImGui::Text("| Odczyt: %llu B | Zapis: %llu B", totalBytesRead, totalBytesWritten);

        ImGui::Separator();

        // Server control bar
        ImGui::Checkbox("Wlacz Serwer Plikow na magistrali CAN", &serverEnabled);
        ImGui::SameLine(320);
        if (ImGui::Button("📂 Otworz folder serwera w Eksploratorze"))
        {
            std::string absPath = std::filesystem::absolute(rootStoragePath).string();
            ShellExecuteA(NULL, "open", absPath.c_str(), NULL, NULL, SW_SHOWNORMAL);
        }
        ImGui::SameLine();
        if (ImGui::Button("🧪 Symuluj zapytanie od maszyny"))
        {
            simulate_client_file_transfer("HOR_SEEDER_CALIB.DAT", true);
        }

        ImGui::Text("Katalog pamieci masowej: %s", rootStoragePath.c_str());

        ImGui::Separator();

        // Tabs: [Pliki na serwerze], [Aktywne uchwyty], [Dziennik zapytan CAN]
        if (ImGui::BeginTabBar("FileServerTabs"))
        {
            if (ImGui::BeginTabItem("Przegladarka Plikow Maszyn"))
            {
                if (ImGui::BeginTable("FilesTable", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, 240)))
                {
                    ImGui::TableSetupColumn("Nazwa pliku", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("Rozmiar (Bajty)", ImGuiTableColumnFlags_WidthFixed, 110.0f);
                    ImGui::TableSetupColumn("Typ", ImGuiTableColumnFlags_WidthFixed, 90.0f);
                    ImGui::TableSetupColumn("Akcja", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                    ImGui::TableHeadersRow();

                    try
                    {
                        for (const auto &entry : std::filesystem::directory_iterator(rootStoragePath))
                        {
                            ImGui::TableNextRow();
                            std::string fname = entry.path().filename().string();
                            bool isDir = entry.is_directory();
                            uintmax_t fsize = isDir ? 0 : entry.file_size();

                            ImGui::TableNextColumn();
                            ImGui::Text("%s %s", isDir ? "📁" : "📄", fname.c_str());

                            ImGui::TableNextColumn();
                            if (isDir) ImGui::TextDisabled("<DIR>");
                            else ImGui::Text("%llu B", fsize);

                            ImGui::TableNextColumn();
                            ImGui::TextUnformatted(isDir ? "Folder" : entry.path().extension().string().c_str());

                            ImGui::TableNextColumn();
                            if (!isDir)
                            {
                                ImGui::PushID(fname.c_str());
                                if (ImGui::SmallButton("Usun"))
                                {
                                    std::filesystem::remove(entry.path());
                                }
                                ImGui::PopID();
                            }
                        }
                    }
                    catch (...)
                    {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextDisabled("Blad odczytu katalogu.");
                    }

                    ImGui::EndTable();
                }

                ImGui::Spacing();
                ImGui::TextDisabled("Standard ISO 11783-13 pozwala maszynom (opryskiwacze, siewniki, kombajny) zapisywac profile i odczytywac firmware.");
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Otwarte Pliki (Uchwyty)"))
            {
                std::lock_guard<std::mutex> lock(serverMutex);
                if (activeHandles.empty())
                {
                    ImGui::TextDisabled("Brak aktywnych sesji plikowych. Maszyny otwieraja pliki tylko na czas transferu.");
                }
                else
                {
                    if (ImGui::BeginTable("HandlesTable", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
                    {
                        ImGui::TableSetupColumn("ID Uchwytu", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                        ImGui::TableSetupColumn("Plik", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn("Klient (SA)", ImGuiTableColumnFlags_WidthFixed, 90.0f);
                        ImGui::TableSetupColumn("Tryb", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                        ImGui::TableSetupColumn("Wskaznik", ImGuiTableColumnFlags_WidthFixed, 90.0f);
                        ImGui::TableSetupColumn("Rozmiar", ImGuiTableColumnFlags_WidthFixed, 90.0f);
                        ImGui::TableHeadersRow();

                        for (const auto &pair : activeHandles)
                        {
                            const auto &h = pair.second;
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn(); ImGui::Text("#%u", h.handleId);
                            ImGui::TableNextColumn(); ImGui::TextUnformatted(h.relativePath.c_str());
                            ImGui::TableNextColumn(); ImGui::Text("0x%02X (%u)", h.clientSourceAddress, h.clientSourceAddress);
                            ImGui::TableNextColumn(); ImGui::Text("%s", (h.accessMode == 1) ? "READ" : ((h.accessMode == 4) ? "CREATE" : "WRITE"));
                            ImGui::TableNextColumn(); ImGui::Text("%u B", h.filePointer);
                            ImGui::TableNextColumn(); ImGui::Text("%u B", h.fileSize);
                        }
                        ImGui::EndTable();
                    }
                }
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Dziennik Zdarzen Serwera"))
            {
                if (ImGui::Button("Wyczysc historie"))
                {
                    std::lock_guard<std::mutex> lock(serverMutex);
                    activityLog.clear();
                }

                if (ImGui::BeginTable("LogTable", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, 240)))
                {
                    ImGui::TableSetupColumn("Czas", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                    ImGui::TableSetupColumn("Klient SA", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                    ImGui::TableSetupColumn("Komenda", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                    ImGui::TableSetupColumn("Szczegoly", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("Wynik", ImGuiTableColumnFlags_WidthFixed, 90.0f);
                    ImGui::TableHeadersRow();

                    std::lock_guard<std::mutex> lock(serverMutex);
                    for (auto it = activityLog.rbegin(); it != activityLog.rend(); ++it)
                    {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn(); ImGui::Text("%.1f s", it->timestampMs / 1000.0f);
                        ImGui::TableNextColumn(); ImGui::Text("0x%02X", it->clientSA);
                        ImGui::TableNextColumn(); ImGui::TextUnformatted(it->commandName.c_str());
                        ImGui::TableNextColumn(); ImGui::TextUnformatted(it->details.c_str());
                        ImGui::TableNextColumn();
                        if (it->result == FileServerError::Ok)
                        {
                            ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "OK (0x00)");
                        }
                        else
                        {
                            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "ERR 0x%02X", static_cast<uint8_t>(it->result));
                        }
                    }
                    ImGui::EndTable();
                }
                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }

        ImGui::End();
    }
}

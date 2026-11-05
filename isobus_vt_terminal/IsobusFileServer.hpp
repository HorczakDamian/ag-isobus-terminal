#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <fstream>
#include <memory>
#include <filesystem>

namespace isobus_vt
{
    // ISO 11783-13 Function codes (Commands)
    enum class FileServerCommand : std::uint8_t
    {
        OpenFile = 0x00,
        SeekFile = 0x01,
        ReadFile = 0x02,
        WriteFile = 0x03,
        CloseFile = 0x04,
        ChangeDirectory = 0x05,
        GetFileInfoOrList = 0x06,
        DeleteFile = 0x07,
        RenameMoveFile = 0x08,
        FormatVolume = 0x09,
        ServerStatus = 0x0A,
        GetVolumeInfo = 0x0B
    };

    // ISO 11783-13 Error codes
    enum class FileServerError : std::uint8_t
    {
        Ok = 0x00,
        FileNotFound = 0x01,
        PathNotFound = 0x02,
        TooManyOpenFiles = 0x03,
        AccessDenied = 0x04,
        InvalidHandle = 0x05,
        DiskFull = 0x06,
        EndOfFile = 0x07,
        SeekError = 0x08,
        GeneralError = 0x09,
        NotSupported = 0x0A
    };

    struct FileServerHandle
    {
        std::uint8_t handleId = 0;
        std::string relativePath;
        std::string fullPath;
        std::uint8_t accessMode = 1; // 1=Read, 2=Write, 3=ReadWrite, 4=Create/Truncate
        std::uint32_t filePointer = 0;
        std::uint32_t fileSize = 0;
        std::uint8_t clientSourceAddress = 0;
        std::uint32_t openTimestampMs = 0;
    };

    struct FileServerLogEntry
    {
        std::uint32_t timestampMs = 0;
        std::uint8_t clientSA = 0;
        std::string commandName;
        std::string details;
        FileServerError result = FileServerError::Ok;
    };

    class IsobusFileServer
    {
    public:
        static IsobusFileServer& get_instance();

        bool initialize(const std::string &storageDirectory = "isobus_file_server");
        void shutdown();
        void update();

        // CAN frame processing (PGN 45312 / 0xB100 Client to File Server)
        void process_can_frame(std::uint32_t pgn, std::uint8_t sa, std::uint8_t da, const std::uint8_t *data, std::uint32_t len);

        // GUI Rendering
        void render_file_server_window(bool *p_open);

        // Server control
        bool is_enabled() const { return serverEnabled; }
        void set_enabled(bool enabled) { serverEnabled = enabled; }
        std::string get_storage_path() const { return rootStoragePath; }
        void set_storage_path(const std::string &path);

        std::uint64_t get_total_bytes_read() const { return totalBytesRead; }
        std::uint64_t get_total_bytes_written() const { return totalBytesWritten; }
        std::uint32_t get_active_handles_count() const;

        // Test simulation
        void simulate_client_file_transfer(const std::string &fileName, bool isWrite);

    private:
        IsobusFileServer();
        ~IsobusFileServer();

        void handle_open_file(std::uint8_t sa, const std::uint8_t *data, std::uint32_t len);
        void handle_read_file(std::uint8_t sa, const std::uint8_t *data, std::uint32_t len);
        void handle_write_file(std::uint8_t sa, const std::uint8_t *data, std::uint32_t len);
        void handle_close_file(std::uint8_t sa, const std::uint8_t *data, std::uint32_t len);
        void handle_get_volume_info(std::uint8_t sa);
        void handle_status_request(std::uint8_t sa);

        void send_can_response(std::uint8_t da, const std::vector<std::uint8_t> &payload);
        std::string sanitize_path(const std::string &requestedPath);

        mutable std::mutex serverMutex;
        bool serverEnabled = true;
        std::string rootStoragePath;
        std::uint8_t fileServerAddress = 0xEA; // Preferred SA for File Server in ISO 11783

        std::map<std::uint8_t, FileServerHandle> activeHandles;
        std::uint8_t nextHandleId = 1;

        std::vector<FileServerLogEntry> activityLog;
        std::uint64_t totalBytesRead = 0;
        std::uint64_t totalBytesWritten = 0;
        std::uint32_t totalRequests = 0;

        // UI Helpers
        char newFolderNameBuf[64] = { 0 };
    };
}

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <functional>
#include <memory>

namespace isobus_vt
{
    class WebVtServer
    {
    public:
        static WebVtServer& get_instance();

        bool is_enabled() const { return enabled.load(); }
        void set_enabled(bool en);

        int get_port() const { return port; }
        void set_port(int p);

        bool is_running() const { return isRunning.load(); }
        std::string get_local_ip() const;
        std::string get_url() const;
        int get_client_count() const { return activeClients.load(); }

        bool start();
        void stop();

        // Feed newest rendered JPEG frame to connected clients
        void update_frame(const std::uint8_t* jpegData, std::size_t jpegSize);

        // Callbacks from Web UI
        using TouchCallback = std::function<void(float relX, float relY, bool down)>;
        using SoftKeyCallback = std::function<void(int keyIndex, bool pressed)>;
        using AckCallback = std::function<void()>;

        void set_touch_callback(TouchCallback cb) { touchCallback = cb; }
        void set_softkey_callback(SoftKeyCallback cb) { softkeyCallback = cb; }
        void set_ack_callback(AckCallback cb) { ackCallback = cb; }

    private:
        WebVtServer();
        ~WebVtServer();

        void server_worker();
        void handle_client(SOCKET clientSocket);
        std::string build_index_html();

        std::atomic<bool> enabled{ false };
        std::atomic<bool> isRunning{ false };
        int port = 8080;

        std::thread workerThread;
        SOCKET listenSocket = INVALID_SOCKET;

        std::mutex frameMutex;
        std::vector<std::uint8_t> latestJpegFrame;
        std::uint64_t frameCounter = 0;
        std::atomic<int> activeClients{ 0 };

        TouchCallback touchCallback;
        SoftKeyCallback softkeyCallback;
        AckCallback ackCallback;
    };
}

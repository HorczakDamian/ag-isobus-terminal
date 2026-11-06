#include "WebVtServer.hpp"
#include <iostream>
#include <sstream>
#include <algorithm>
#include <chrono>

#pragma comment(lib, "ws2_32.lib")

namespace isobus_vt
{
    static bool send_all(SOCKET s, const char* data, int len)
    {
        int total = 0;
        while (total < len)
        {
            int n = send(s, data + total, len - total, 0);
            if (n <= 0) return false;
            total += n;
        }
        return true;
    }

    static void close_client_cleanly(SOCKET s)
    {
        shutdown(s, SD_SEND);
        char dummy[256];
        recv(s, dummy, sizeof(dummy), 0);
        closesocket(s);
    }

    WebVtServer& WebVtServer::get_instance()
    {
        static WebVtServer instance;
        return instance;
    }

    WebVtServer::WebVtServer()
    {
        WSADATA wsaData;
        WSAStartup(MAKEWORD(2, 2), &wsaData);
    }

    WebVtServer::~WebVtServer()
    {
        stop();
        WSACleanup();
    }

    void WebVtServer::set_enabled(bool en)
    {
        if (enabled.load() == en) return;
        enabled.store(en);
        if (en)
        {
            start();
        }
        else
        {
            stop();
        }
    }

    void WebVtServer::set_port(int p)
    {
        if (port != p)
        {
            port = p;
            if (isRunning.load())
            {
                stop();
                start();
            }
        }
    }

    std::string WebVtServer::get_local_ip() const
    {
        char hostname[256] = { 0 };
        if (gethostname(hostname, sizeof(hostname)) != 0)
        {
            return "127.0.0.1";
        }

        struct addrinfo hints = {}, *info = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;

        std::string bestIp = "127.0.0.1";
        int bestScore = 0; // 0=fallback, 1=other non-loopback, 2=private LAN (192.168.*, 10.*)

        if (getaddrinfo(hostname, nullptr, &hints, &info) == 0)
        {
            for (struct addrinfo *p = info; p != nullptr; p = p->ai_next)
            {
                struct sockaddr_in *ipv4 = reinterpret_cast<struct sockaddr_in*>(p->ai_addr);
                char ipStr[INET_ADDRSTRLEN] = { 0 };
                inet_ntop(AF_INET, &(ipv4->sin_addr), ipStr, INET_ADDRSTRLEN);
                std::string ipCandidate = ipStr;

                if (ipCandidate.rfind("127.", 0) == 0) continue;
                // Exclude Radmin VPN (26.*) and APIPA (169.254.*)
                if (ipCandidate.rfind("26.", 0) == 0 || ipCandidate.rfind("169.254.", 0) == 0) continue;

                if (ipCandidate.rfind("192.168.", 0) == 0 || ipCandidate.rfind("10.", 0) == 0)
                {
                    bestIp = ipCandidate;
                    bestScore = 2;
                    break;
                }
                else if (bestScore < 1)
                {
                    bestIp = ipCandidate;
                    bestScore = 1;
                }
            }
            freeaddrinfo(info);
        }
        return bestIp;
    }

    std::string WebVtServer::get_url() const
    {
        return "http://" + get_local_ip() + ":" + std::to_string(port);
    }

    bool WebVtServer::start()
    {
        if (isRunning.load()) return true;

        WSADATA wsaData;
        WSAStartup(MAKEWORD(2, 2), &wsaData);

        listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listenSocket == INVALID_SOCKET)
        {
            return false;
        }

        int opt = 1;
        setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));

        sockaddr_in serverAddr = {};
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_addr.s_addr = INADDR_ANY;
        serverAddr.sin_port = htons(static_cast<u_short>(port));

        if (bind(listenSocket, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)) == SOCKET_ERROR)
        {
            closesocket(listenSocket);
            listenSocket = INVALID_SOCKET;
            return false;
        }

        if (listen(listenSocket, 10) == SOCKET_ERROR)
        {
            closesocket(listenSocket);
            listenSocket = INVALID_SOCKET;
            return false;
        }

        isRunning.store(true);
        workerThread = std::thread(&WebVtServer::server_worker, this);
        return true;
    }

    void WebVtServer::stop()
    {
        if (!isRunning.load()) return;
        isRunning.store(false);

        if (listenSocket != INVALID_SOCKET)
        {
            closesocket(listenSocket);
            listenSocket = INVALID_SOCKET;
        }

        if (workerThread.joinable())
        {
            workerThread.join();
        }
    }

    void WebVtServer::update_frame(const std::uint8_t* jpegData, std::size_t jpegSize)
    {
        if (!jpegData || jpegSize == 0) return;
        std::lock_guard<std::mutex> lock(frameMutex);
        latestJpegFrame.assign(jpegData, jpegData + jpegSize);
        frameCounter++;
    }

    void WebVtServer::server_worker()
    {
        while (isRunning.load())
        {
            fd_set readSet;
            FD_ZERO(&readSet);
            FD_SET(listenSocket, &readSet);

            timeval tv = { 0, 200000 }; // 200 ms timeout
            int selectRes = select(0, &readSet, nullptr, nullptr, &tv);
            if (!isRunning.load()) break;

            if (selectRes > 0 && FD_ISSET(listenSocket, &readSet))
            {
                sockaddr_in clientAddr;
                int clientLen = sizeof(clientAddr);
                SOCKET clientSocket = accept(listenSocket, reinterpret_cast<sockaddr*>(&clientAddr), &clientLen);
                if (clientSocket != INVALID_SOCKET)
                {
                    std::thread clientThread(&WebVtServer::handle_client, this, clientSocket);
                    clientThread.detach();
                }
            }
            else if (selectRes < 0)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    }

    static const unsigned char g_fallbackJpeg[] = {
        0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 0x4A, 0x46, 0x49, 0x46, 0x00, 0x01, 0x01, 0x01, 0x00, 0x48,
        0x00, 0x48, 0x00, 0x00, 0xFF, 0xDB, 0x00, 0x43, 0x00, 0x08, 0x06, 0x06, 0x07, 0x06, 0x05, 0x08,
        0x07, 0x07, 0x07, 0x09, 0x09, 0x08, 0x0A, 0x0C, 0x14, 0x0D, 0x0C, 0x0B, 0x0B, 0x0C, 0x19, 0x12,
        0x13, 0x0F, 0x14, 0x1D, 0x1A, 0x1F, 0x1E, 0x1D, 0x1A, 0x1C, 0x1C, 0x20, 0x24, 0x2E, 0x27, 0x20,
        0x22, 0x2C, 0x23, 0x1C, 0x1C, 0x28, 0x37, 0x29, 0x2C, 0x30, 0x31, 0x34, 0x34, 0x34, 0x1F, 0x27,
        0x39, 0x3D, 0x38, 0x32, 0x3C, 0x2E, 0x33, 0x34, 0x32, 0xFF, 0xC0, 0x00, 0x0B, 0x08, 0x00, 0x01,
        0x00, 0x01, 0x01, 0x01, 0x11, 0x00, 0xFF, 0xC4, 0x00, 0x1F, 0x00, 0x00, 0x01, 0x05, 0x01, 0x01,
        0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04,
        0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0xFF, 0xDA, 0x00, 0x08, 0x01, 0x01, 0x00, 0x00, 0x3F,
        0x00, 0xBF, 0x80, 0xFF, 0xD9
    };

    void WebVtServer::handle_client(SOCKET clientSocket)
    {
        activeClients++;

        char buffer[2048] = { 0 };
        int bytesRead = recv(clientSocket, buffer, sizeof(buffer) - 1, 0);
        if (bytesRead <= 0)
        {
            closesocket(clientSocket);
            activeClients--;
            return;
        }

        std::string request(buffer, bytesRead);
        std::istringstream reqStream(request);
        std::string method, rawPath, protocol;
        reqStream >> method >> rawPath >> protocol;

        if (rawPath.empty()) rawPath = "/";

        // Preflight CORS
        if (method == "OPTIONS")
        {
            std::string resp = "HTTP/1.1 204 No Content\r\n"
                               "Access-Control-Allow-Origin: *\r\n"
                               "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                               "Access-Control-Allow-Headers: *\r\n"
                               "Connection: close\r\n\r\n";
            send_all(clientSocket, resp.c_str(), static_cast<int>(resp.size()));
            close_client_cleanly(clientSocket);
            activeClients--;
            return;
        }

        // Strip query string for route matching
        size_t qPos = rawPath.find('?');
        std::string cleanPath = (qPos != std::string::npos) ? rawPath.substr(0, qPos) : rawPath;
        std::string queryStr = (qPos != std::string::npos) ? rawPath.substr(qPos + 1) : "";

        auto parseQueryParam = [&](const std::string& key) -> std::string {
            size_t p = queryStr.find(key + "=");
            if (p == std::string::npos) return "";
            p += key.size() + 1;
            size_t endP = queryStr.find('&', p);
            return queryStr.substr(p, (endP == std::string::npos) ? std::string::npos : (endP - p));
        };

        // Route: HTML Main Page
        if (cleanPath == "/" || cleanPath == "/index.html")
        {
            std::string body = build_index_html();
            std::ostringstream response;
            response << "HTTP/1.1 200 OK\r\n"
                     << "Content-Type: text/html; charset=utf-8\r\n"
                     << "Content-Length: " << body.size() << "\r\n"
                     << "Access-Control-Allow-Origin: *\r\n"
                     << "Connection: close\r\n\r\n"
                     << body;
            std::string respStr = response.str();
            send_all(clientSocket, respStr.c_str(), static_cast<int>(respStr.size()));
            close_client_cleanly(clientSocket);
        }
        // Route: Single Frame Snapshot (Auto-refreshed by JS)
        else if (cleanPath == "/vt_screen.jpg")
        {
            std::vector<std::uint8_t> frameCopy;
            {
                std::lock_guard<std::mutex> lock(frameMutex);
                frameCopy = latestJpegFrame;
            }

            const unsigned char* pData = frameCopy.empty() ? g_fallbackJpeg : frameCopy.data();
            size_t dataLen = frameCopy.empty() ? sizeof(g_fallbackJpeg) : frameCopy.size();

            std::ostringstream response;
            response << "HTTP/1.1 200 OK\r\n"
                     << "Content-Type: image/jpeg\r\n"
                     << "Content-Length: " << dataLen << "\r\n"
                     << "Access-Control-Allow-Origin: *\r\n"
                     << "Cache-Control: no-cache, no-store, must-revalidate\r\n"
                     << "Connection: close\r\n\r\n";
            std::string header = response.str();
            send_all(clientSocket, header.c_str(), static_cast<int>(header.size()));
            send_all(clientSocket, reinterpret_cast<const char*>(pData), static_cast<int>(dataLen));
            close_client_cleanly(clientSocket);
        }
        // Route: Live MJPEG Stream (for browsers that support it)
        else if (cleanPath == "/vt_stream")
        {
            std::ostringstream header;
            header << "HTTP/1.1 200 OK\r\n"
                   << "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
                   << "Access-Control-Allow-Origin: *\r\n"
                   << "Cache-Control: no-cache, no-store, must-revalidate\r\n"
                   << "Connection: keep-alive\r\n\r\n";
            std::string headerStr = header.str();
            send_all(clientSocket, headerStr.c_str(), static_cast<int>(headerStr.size()));

            std::uint64_t lastSentFrame = 0;
            while (isRunning.load())
            {
                std::vector<std::uint8_t> frameCopy;
                std::uint64_t currentFrameIdx = 0;
                {
                    std::lock_guard<std::mutex> lock(frameMutex);
                    currentFrameIdx = frameCounter;
                    if (currentFrameIdx != lastSentFrame && !latestJpegFrame.empty())
                    {
                        frameCopy = latestJpegFrame;
                        lastSentFrame = currentFrameIdx;
                    }
                }

                if (!frameCopy.empty())
                {
                    std::ostringstream part;
                    part << "--frame\r\n"
                         << "Content-Type: image/jpeg\r\n"
                         << "Content-Length: " << frameCopy.size() << "\r\n\r\n";
                    std::string partHeader = part.str();

                    if (!send_all(clientSocket, partHeader.c_str(), static_cast<int>(partHeader.size()))) break;
                    if (!send_all(clientSocket, reinterpret_cast<const char*>(frameCopy.data()), static_cast<int>(frameCopy.size()))) break;
                    if (!send_all(clientSocket, "\r\n", 2)) break;
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(80)); // ~12 FPS
            }
            close_client_cleanly(clientSocket);
        }
        // Route: API Touch Coordinate
        else if (cleanPath == "/api/touch")
        {
            float relX = 0.5f, relY = 0.5f;
            bool down = true;

            std::string sX = parseQueryParam("x");
            std::string sY = parseQueryParam("y");
            std::string sDown = parseQueryParam("down");

            if (!sX.empty()) relX = std::stof(sX);
            if (!sY.empty()) relY = std::stof(sY);
            if (!sDown.empty()) down = (sDown == "1" || sDown == "true");

            if (touchCallback)
            {
                touchCallback(relX, relY, down);
            }

            const char* okResp = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n{\"status\":\"ok\"}";
            send_all(clientSocket, okResp, static_cast<int>(strlen(okResp)));
            close_client_cleanly(clientSocket);
        }
        // Route: API SoftKey
        else if (cleanPath == "/api/softkey")
        {
            int keyIdx = 0;
            bool pressed = true;

            std::string sIdx = parseQueryParam("idx");
            std::string sPres = parseQueryParam("pressed");
            if (!sIdx.empty()) keyIdx = std::stoi(sIdx);
            if (!sPres.empty()) pressed = (sPres == "1" || sPres == "true");

            if (softkeyCallback)
            {
                softkeyCallback(keyIdx, pressed);
            }

            const char* okResp = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n{\"status\":\"ok\"}";
            send_all(clientSocket, okResp, static_cast<int>(strlen(okResp)));
            close_client_cleanly(clientSocket);
        }
        // Route: API ACK
        else if (cleanPath == "/api/ack")
        {
            if (ackCallback)
            {
                ackCallback();
            }
            const char* okResp = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n{\"status\":\"ok\"}";
            send_all(clientSocket, okResp, static_cast<int>(strlen(okResp)));
            close_client_cleanly(clientSocket);
        }
        // Route: API Status JSON
        else if (cleanPath == "/api/status")
        {
            std::ostringstream json;
            json << "{\"running\":true,\"active_clients\":" << activeClients.load() << ",\"ip\":\"" << get_local_ip() << "\",\"port\":" << port << "}";
            std::string body = json.str();

            std::ostringstream response;
            response << "HTTP/1.1 200 OK\r\n"
                     << "Content-Type: application/json\r\n"
                     << "Content-Length: " << body.size() << "\r\n"
                     << "Access-Control-Allow-Origin: *\r\n"
                     << "Connection: close\r\n\r\n"
                     << body;
            std::string respStr = response.str();
            send_all(clientSocket, respStr.c_str(), static_cast<int>(respStr.size()));
            close_client_cleanly(clientSocket);
        }
        else
        {
            const char* notFound = "HTTP/1.1 404 Not Found\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 0\r\n\r\n";
            send_all(clientSocket, notFound, static_cast<int>(strlen(notFound)));
            close_client_cleanly(clientSocket);
        }

        activeClients--;
    }

    std::string WebVtServer::build_index_html()
    {
        return R"rawhtml(<!DOCTYPE html>
<html lang="pl">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
<title>ISOBUS Virtual Terminal | In-Cab Tablet</title>
<style>
  * { box-sizing: border-box; margin: 0; padding: 0; user-select: none; -webkit-user-select: none; }
  body {
    background: #0b0f15;
    color: #e2e8f0;
    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
    height: 100vh;
    display: flex;
    flex-direction: column;
    overflow: hidden;
  }
  header {
    background: #141b26;
    padding: 6px 14px;
    display: flex;
    justify-content: space-between;
    align-items: center;
    border-bottom: 2px solid #222f3e;
    height: 44px;
  }
  .title { font-weight: 700; font-size: 1.05rem; color: #38bdf8; display: flex; align-items: center; gap: 8px; }
  .badge { background: #15803d; color: #dcfce7; padding: 2px 7px; border-radius: 4px; font-size: 0.75rem; font-weight: bold; }
  .fps-badge { font-size: 0.75rem; color: #94a3b8; }
  .main-area {
    flex: 1;
    display: flex;
    align-items: center;
    justify-content: center;
    padding: 8px;
    gap: 12px;
    position: relative;
    overflow: hidden;
  }
  .vt-screen-box {
    position: relative;
    border-radius: 12px;
    overflow: hidden;
    box-shadow: 0 10px 30px rgba(0, 0, 0, 0.85);
    border: 3px solid #2c394b;
    background: #0d121a;
    height: calc(100vh - 104px);
    flex: 1;
    max-width: 100%;
    display: flex;
    justify-content: center;
    align-items: center;
  }
  #vt-img {
    display: block;
    max-width: 100%;
    max-height: 100%;
    width: auto;
    height: auto;
    object-fit: contain;
    cursor: crosshair;
  }
  .softkeys-panel {
    display: flex;
    flex-direction: column;
    gap: 8px;
    height: calc(100vh - 100px);
    width: 72px;
    justify-content: space-between;
  }
  .softkey-btn {
    background: linear-gradient(180deg, #2b3648 0%, #1a2230 100%);
    border: 2px solid #41516b;
    color: #f8fafc;
    border-radius: 8px;
    width: 100%;
    flex: 1;
    display: flex;
    flex-direction: column;
    justify-content: center;
    align-items: center;
    font-weight: 800;
    font-size: 1.15rem;
    box-shadow: 0 4px 6px rgba(0,0,0,0.5);
    cursor: pointer;
    touch-action: manipulation;
    transition: all 0.05s ease;
  }
  .softkey-btn:active {
    background: #38bdf8;
    color: #000;
    transform: scale(0.96);
    border-color: #7dd3fc;
  }
  footer {
    background: #141b26;
    border-top: 2px solid #222f3e;
    padding: 6px 14px;
    display: flex;
    justify-content: space-between;
    align-items: center;
    height: 48px;
  }
  .bottom-btn {
    background: #273447;
    color: #f1f5f9;
    border: 1px solid #3d4f68;
    padding: 7px 16px;
    border-radius: 6px;
    font-size: 0.9rem;
    font-weight: 700;
    cursor: pointer;
  }
  .bottom-btn:active { background: #3d4f68; }
  .btn-ack { background: #b45309; color: #fff; border-color: #d97706; }
  .btn-ack:active { background: #f59e0b; color: #000; }
</style>
</head>
<body>
  <header>
    <div class="title">
      <span>🚜 AG-ISOBUS PRO 1200</span>
      <span class="badge">WI-FI AKTYWNE</span>
    </div>
    <div class="fps-badge" id="status-info">Łączenie...</div>
  </header>

  <div class="main-area">
    <div class="vt-screen-box">
      <img id="vt-img" src="/vt_screen.jpg" alt="ISOBUS Terminal Screen" draggable="false">
    </div>

    <div class="softkeys-panel" id="softkeys-panel">
      <button class="softkey-btn" onmousedown="pressKey(0)" onmouseup="releaseKey(0)" ontouchstart="pressKey(0)" ontouchend="releaseKey(0)">F1</button>
      <button class="softkey-btn" onmousedown="pressKey(1)" onmouseup="releaseKey(1)" ontouchstart="pressKey(1)" ontouchend="releaseKey(1)">F2</button>
      <button class="softkey-btn" onmousedown="pressKey(2)" onmouseup="releaseKey(2)" ontouchstart="pressKey(2)" ontouchend="releaseKey(2)">F3</button>
      <button class="softkey-btn" onmousedown="pressKey(3)" onmouseup="releaseKey(3)" ontouchstart="pressKey(3)" ontouchend="releaseKey(3)">F4</button>
      <button class="softkey-btn" onmousedown="pressKey(4)" onmouseup="releaseKey(4)" ontouchstart="pressKey(4)" ontouchend="releaseKey(4)">F5</button>
      <button class="softkey-btn" onmousedown="pressKey(5)" onmouseup="releaseKey(5)" ontouchstart="pressKey(5)" ontouchend="releaseKey(5)">F6</button>
    </div>
  </div>

  <footer>
    <button class="bottom-btn btn-ack" onclick="sendAck()">⚠️ ACK</button>
        <button class="bottom-btn" id="btn-toggle-sk" onclick="toggleSoftkeys()">⌨️ Klawisze F1-F6</button>
  <div style="font-size: 0.8rem; color: #64748b;">Dotknij maski lub klawiszy F1-F6</div>
    <button class="bottom-btn" onclick="toggleFullScreen()">⛶ Pełny Ekran</button>
  </footer>

  <script>
    const img = document.getElementById('vt-img');
    const statusInfo = document.getElementById('status-info');
    let framesLoaded = 0;
    let lastFpsTime = Date.now();

    function fetchNextFrame() {
      const nextImg = new Image();
      let timer = setTimeout(() => {
        nextImg.src = '';
        setTimeout(fetchNextFrame, 300);
      }, 2500);

      nextImg.onload = () => {
        clearTimeout(timer);
        img.src = nextImg.src;
        framesLoaded++;
        const now = Date.now();
        if (now - lastFpsTime >= 1000) {
          statusInfo.innerText = framesLoaded + ' FPS | Połączono';
          framesLoaded = 0;
          lastFpsTime = now;
        }
        setTimeout(fetchNextFrame, 75); // ~13 FPS
      };
      nextImg.onerror = () => {
        clearTimeout(timer);
        statusInfo.innerText = 'Czuwanie...';
        setTimeout(fetchNextFrame, 350);
      };
      nextImg.src = '/vt_screen.jpg?t=' + Date.now();
    }
    fetchNextFrame();

    function sendTouch(evt, down) {
      evt.preventDefault();
      const rect = img.getBoundingClientRect();
      const clientX = (evt.touches && evt.touches.length > 0) ? evt.touches[0].clientX : evt.clientX;
      const clientY = (evt.touches && evt.touches.length > 0) ? evt.touches[0].clientY : evt.clientY;

      const relX = (clientX - rect.left) / rect.width;
      const relY = (clientY - rect.top) / rect.height;

      if (relX >= 0 && relX <= 1 && relY >= 0 && relY <= 1) {
        fetch('/api/touch?x=' + relX.toFixed(4) + '&y=' + relY.toFixed(4) + '&down=' + (down ? 1 : 0), { method: 'POST', mode: 'no-cors' }).catch(() => {});
      }
    }

    img.addEventListener('mousedown', (e) => sendTouch(e, true));
    img.addEventListener('mouseup', (e) => sendTouch(e, false));
    img.addEventListener('touchstart', (e) => sendTouch(e, true), { passive: false });
    img.addEventListener('touchend', (e) => sendTouch(e, false), { passive: false });

    function pressKey(idx) {
      fetch('/api/softkey?idx=' + idx + '&pressed=1', { method: 'POST', mode: 'no-cors' }).catch(() => {});
    }
    function releaseKey(idx) {
      fetch('/api/softkey?idx=' + idx + '&pressed=0', { method: 'POST', mode: 'no-cors' }).catch(() => {});
    }
    function sendAck() {
      fetch('/api/ack', { method: 'POST', mode: 'no-cors' }).catch(() => {});
    }
    function toggleSoftkeys() {
      const panel = document.getElementById('softkeys-panel');
      const btn = document.getElementById('btn-toggle-sk');
      if (panel.style.display === 'none') {
        panel.style.display = 'flex';
        if (btn) btn.style.background = '#273447';
      } else {
        panel.style.display = 'none';
        if (btn) btn.style.background = '#155e75';
      }
    }
    function toggleFullScreen() {
      if (!document.fullscreenElement) {
        document.documentElement.requestFullscreen().catch(() => {});
      } else {
        document.exitFullscreen().catch(() => {});
      }
    }
  </script>
</body>
</html>
)rawhtml";
    }
}

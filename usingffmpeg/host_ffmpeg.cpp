// DEFINED FIRST: Force Windows 7+ API visibility
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601 
#endif

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "user32.lib")

#define INPUT_PORT 50005

// Configuration
std::string deviceKey = "TEST_KEY_123";
int g_screenW = 1920;
int g_screenH = 1080;
// We assume FFmpeg streams at 1280x720, but input mapping needs to know.
int g_streamW = 1280;
int g_streamH = 720;

struct InputPacket
{
    int type;
    int x;
    int y;
    int key;
};

// --- INPUT LISTENER (Same as before, keeps mouse working) ---
void InputListener(SOCKET sock)
{
    sockaddr_in senderAddr;
    int senderSize = sizeof(senderAddr);
    char buffer[1024];

    std::cout << "[INPUT] Listening for mouse/keyboard on port " << INPUT_PORT << "...\n";

    while (true)
    {
        int recvLen = recvfrom(sock, buffer, sizeof(buffer), 0, (sockaddr *)&senderAddr, &senderSize);
        if (recvLen == sizeof(InputPacket))
        {
            InputPacket *pkt = (InputPacket *)buffer;
            
            // Map input back to real screen coordinates
            int realX = (pkt->x * g_screenW) / g_streamW;
            int realY = (pkt->y * g_screenH) / g_streamH;

            switch (pkt->type)
            {
            case 1: SetCursorPos(realX, realY); break;
            case 2: SetCursorPos(realX, realY); mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0); break;
            case 3: mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0); break;
            case 4: SetCursorPos(realX, realY); mouse_event(MOUSEEVENTF_RIGHTDOWN, 0, 0, 0, 0); break;
            case 5: mouse_event(MOUSEEVENTF_RIGHTUP, 0, 0, 0, 0); break;
            case 6: keybd_event((BYTE)pkt->key, 0, 0, 0); break;
            case 7: keybd_event((BYTE)pkt->key, 0, KEYEVENTF_KEYUP, 0); break;
            }
        }
    }
}

// --- FFMPEG LAUNCHER ---
void StartFFmpeg(std::string clientIP)
{
    // COMMAND EXPLANATION:
    // -f gdigrab: Capture Windows screen
    // -framerate 30: Smooth 30fps
    // -video_size 1280x720: Downscale for speed/bandwidth
    // -c:v libx264: H.264 Encoder (High efficiency)
    // -preset ultrafast: Lowest CPU usage, lowest latency
    // -tune zerolatency: CRITICAL for real-time streaming
    // -b:v 2000k: Limit to 2Mbps (Fixes Tailscale lag)
    // -f mpegts: Standard streaming format for UDP
    
    std::string cmd = "ffmpeg.exe -f gdigrab -framerate 30 -video_size 1280x720 -i desktop "
                      "-c:v libx264 -preset ultrafast -tune zerolatency "
                      "-b:v 2000k -f mpegts udp://" + clientIP + ":50006?pkt_size=1316";

    std::cout << "[FFMPEG] Starting Stream...\n";
    std::cout << "[CMD] " << cmd << "\n";

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    // Create the FFmpeg process
    // We use a mutable char buffer because CreateProcessA requires it
    std::vector<char> cmdBuffer(cmd.begin(), cmd.end());
    cmdBuffer.push_back(0);

    if (!CreateProcessA(NULL, cmdBuffer.data(), NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
    {
        std::cout << "[ERROR] Could not launch ffmpeg.exe! Is it in this folder?\n";
        return;
    }

    // Wait until the user closes THIS console, then kill FFmpeg
    std::cout << "------------------------------------------------\n";
    std::cout << "  STREAM IS LIVE. CLOSE THIS WINDOW TO STOP.    \n";
    std::cout << "------------------------------------------------\n";
    
    // Simple way to keep main thread alive while FFmpeg runs
    WaitForSingleObject(pi.hProcess, INFINITE);

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
}

int main()
{
    // 1. Setup Screen Metrics
    SetProcessDPIAware();
    g_screenW = GetSystemMetrics(SM_CXSCREEN);
    g_screenH = GetSystemMetrics(SM_CYSCREEN);

    // 2. Network Setup for Input
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(INPUT_PORT);
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    bind(sock, (sockaddr *)&serverAddr, sizeof(serverAddr));

    // 3. Get Client IP
    std::string clientIP;
    std::cout << "Enter Client IP (Tailscale IP): ";
    std::cin >> clientIP;

    // 4. Start Input Listener in background
    std::thread inputThread(InputListener, sock);
    inputThread.detach();

    // 5. Start FFmpeg (Main Thread)
    StartFFmpeg(clientIP);

    return 0;
}
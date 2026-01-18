// DEFINED FIRST: Force Windows 7+ API visibility
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601 
#endif

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iostream>
#include <string>
#include <vector>
#include <fstream> // For file checking

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

#define HOST_INPUT_PORT 50005
#define CLIENT_STREAM_PORT 50006

// Configuration
// Host resolution (Must match what you set in host_ffmpeg.cpp)
int HOST_WIDTH = 1280;
int HOST_HEIGHT = 720;
const char* VIDEO_WINDOW_TITLE = "Remote Video Stream";

struct InputPacket
{
    int type;
    int x;
    int y;
    int key;
};

SOCKET sock;
sockaddr_in hostAddr;
int g_windowW = 0;
int g_windowH = 0;
HWND g_overlayHwnd = NULL;

// --- UTILS ---
bool FileExists(const std::string& name) {
    std::ifstream f(name.c_str());
    return f.good();
}

// --- NETWORK SENDER ---
void SendInputPacket(int type, int x, int y, int key)
{
    if (g_windowW == 0 || g_windowH == 0) return;

    InputPacket pkt;
    pkt.type = type;
    pkt.x = x;
    pkt.y = y;
    pkt.key = key;

    // Scale coordinates from our window size to Host resolution
    pkt.x = (x * HOST_WIDTH) / g_windowW;
    pkt.y = (y * HOST_HEIGHT) / g_windowH;

    sendto(sock, (char *)&pkt, sizeof(pkt), 0, (sockaddr *)&hostAddr, sizeof(hostAddr));
}

// --- WINDOW SNAPPER ---
// This makes the overlay follow the ffplay window automatically
void SnapOverlayToVideo()
{
    HWND hVideo = FindWindowA("SDL_app", VIDEO_WINDOW_TITLE); // SDL_app is the class for ffplay/SDL windows
    
    if (hVideo && IsWindowVisible(hVideo))
    {
        // --- Z-ORDER FIX ---
        // Make the Overlay "Owned" by the Video Window.
        // This ensures the Overlay moves in the Z-order STACK with the video.
        // If Video goes behind VSCode, Overlay goes behind too.
        static HWND hCurrentOwner = NULL;
        if (hCurrentOwner != hVideo)
        {
            SetWindowLongPtr(g_overlayHwnd, GWLP_HWNDPARENT, (LONG_PTR)hVideo);
            hCurrentOwner = hVideo;
        }

        RECT rcClient;
        GetClientRect(hVideo, &rcClient);
        
        // Convert client area (the video part) to screen coordinates
        POINT pt = { rcClient.left, rcClient.top };
        ClientToScreen(hVideo, &pt);
        
        // Move our overlay to match the video area exactly
        int width = rcClient.right - rcClient.left;
        int height = rcClient.bottom - rcClient.top;

        // Check if move is needed to prevent jitter
        RECT rcOverlay;
        GetWindowRect(g_overlayHwnd, &rcOverlay);
        
        if (rcOverlay.left != pt.x || rcOverlay.top != pt.y || 
            (rcOverlay.right - rcOverlay.left) != width || 
            (rcOverlay.bottom - rcOverlay.top) != height)
        {
            // Use SWP_NOZORDER so we don't force it to top/bottom manually.
            // The "Owner" relationship handles the Z-order for us.
            SetWindowPos(g_overlayHwnd, NULL, pt.x, pt.y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
}

// --- WINDOW PROCEDURE ---
LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    case WM_SIZE:
        g_windowW = LOWORD(lp);
        g_windowH = HIWORD(lp);
        return 0;

    case WM_TIMER:
        if (wp == 1) SnapOverlayToVideo();
        return 0;

    // Mouse Inputs
    case WM_MOUSEMOVE:     SendInputPacket(1, LOWORD(lp), HIWORD(lp), 0); break;
    case WM_LBUTTONDOWN:   SendInputPacket(2, LOWORD(lp), HIWORD(lp), 0); break;
    case WM_LBUTTONUP:     SendInputPacket(3, LOWORD(lp), HIWORD(lp), 0); break;
    case WM_RBUTTONDOWN:   SendInputPacket(4, LOWORD(lp), HIWORD(lp), 0); break;
    case WM_RBUTTONUP:     SendInputPacket(5, LOWORD(lp), HIWORD(lp), 0); break;
    case WM_KEYDOWN:       SendInputPacket(6, 0, 0, (int)wp); break;
    case WM_KEYUP:         SendInputPacket(7, 0, 0, (int)wp); break;
        
    case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            FillRect(hdc, &ps.rcPaint, (HBRUSH) (COLOR_WINDOW+1));
            EndPaint(hwnd, &ps);
        }
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// --- FFPLAY LAUNCHER ---
void StartFFplay()
{
    if (!FileExists("ffplay.exe"))
    {
        MessageBoxA(NULL, "ffplay.exe not found! Place it in this folder.", "Error", MB_ICONERROR);
        exit(1);
    }

    // -noborder: removes title bar (optional, often better for overlay)
    // udp://0.0.0.0 is safer than @ on Windows
    std::string cmd = "ffplay.exe -fflags nobuffer -flags low_delay -framedrop -window_title \"" + 
                      std::string(VIDEO_WINDOW_TITLE) + "\" udp://0.0.0.0:50006?overrun_nonfatal=1";

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    std::vector<char> cmdBuffer(cmd.begin(), cmd.end());
    cmdBuffer.push_back(0);

    if (!CreateProcessA(NULL, cmdBuffer.data(), NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
    {
        MessageBoxA(NULL, "Failed to launch ffplay.", "Error", MB_ICONERROR);
    }
}

int main()
{
    // REVERTED: Removed SetProcessDPIAware() based on request.
    
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    sock = socket(AF_INET, SOCK_DGRAM, 0);
    
    std::string hostIP;
    std::cout << "Enter Host IP: ";
    std::cin >> hostIP;

    hostAddr.sin_family = AF_INET;
    hostAddr.sin_port = htons(HOST_INPUT_PORT);
    hostAddr.sin_addr.s_addr = inet_addr(hostIP.c_str());

    StartFFplay();

    WNDCLASSW wc{};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"InputClient";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1); 
    
    RegisterClassW(&wc);

    // Create a POPUP window (no border) so it looks like part of the video
    // REMOVED WS_EX_TOPMOST to allow it to go behind other windows
    g_overlayHwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW, // Toolwindow hides it from alt-tab (optional)
        L"InputClient", 
        L"Overlay", 
        WS_POPUP | WS_VISIBLE, 
        0, 0, 100, 100, // Size doesn't matter, auto-snaps
        NULL, NULL, wc.hInstance, NULL
    );

    // Set transparency: Alpha=1 (almost invisible) but still captures clicks
    SetLayeredWindowAttributes(g_overlayHwnd, 0, 1, LWA_ALPHA);

    // Start timer to follow the video window
    SetTimer(g_overlayHwnd, 1, 10, NULL); 

    std::cout << "[INFO] Client Running. The overlay will attach to the video window automatically.\n";

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return 0;
}
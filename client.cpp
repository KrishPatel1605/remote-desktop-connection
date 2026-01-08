#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iostream>
#include <string>
#include <vector>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "gdi32.lib")

#define HOST_PORT 50005
#define CLIENT_PORT 50006
#define MAX_PACKET_SIZE 65535 

std::string deviceKey = "TEST_KEY_123";

struct PacketHeader {
    int offset;
    int dataLen;
    int totalSize;
    int width;
    int height;
};

// Input Packet Structure (Must match Host)
struct InputPacket {
    int type; // 1=Move, 2=LDown, 3=LUp, 4=RDown, 5=RUp, 6=KeyDown, 7=KeyUp
    int x;
    int y;
    int key;
};

// Globals
HWND hwnd;
HBITMAP hBitmap = NULL;
int currentW = 0, currentH = 0;
SOCKET sock; // Global socket for sending input
sockaddr_in hostAddrGlobal; // Host address for sending input
std::vector<char> frameBuffer; 
std::vector<char> displayBuffer; 

// Input sending helper
void SendInputPacket(int type, int x, int y, int key) {
    if (currentW == 0 || currentH == 0) return;

    // Scale coordinates to the host's expected resolution (2340x1080)
    // We assume the host is sending images at 2340x1080 (as per host code)
    // The 'x' and 'y' here are relative to the client window client area
    // We need to map [0, currentW] -> [0, 2340]
    
    int hostW = 2340;
    int hostH = 1080;

    int scaledX = (x * hostW) / currentW;
    int scaledY = (y * hostH) / currentH;

    InputPacket pkt;
    pkt.type = type;
    pkt.x = scaledX;
    pkt.y = scaledY;
    pkt.key = key;

    sendto(sock, (char*)&pkt, sizeof(pkt), 0, (sockaddr*)&hostAddrGlobal, sizeof(hostAddrGlobal));
}

LRESULT CALLBACK WindowProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch(msg) {
        case WM_DESTROY: 
            PostQuitMessage(0); 
            return 0;
        
        // --- Input Handling ---
        case WM_MOUSEMOVE:
            SendInputPacket(1, LOWORD(lp), HIWORD(lp), 0);
            break;
        case WM_LBUTTONDOWN:
            SendInputPacket(2, LOWORD(lp), HIWORD(lp), 0);
            break;
        case WM_LBUTTONUP:
            SendInputPacket(3, LOWORD(lp), HIWORD(lp), 0);
            break;
        case WM_RBUTTONDOWN:
            SendInputPacket(4, LOWORD(lp), HIWORD(lp), 0);
            break;
        case WM_RBUTTONUP:
            SendInputPacket(5, LOWORD(lp), HIWORD(lp), 0);
            break;
        case WM_KEYDOWN:
            SendInputPacket(6, 0, 0, (int)wp); // wp contains VK code
            break;
        case WM_KEYUP:
            SendInputPacket(7, 0, 0, (int)wp);
            break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void initWindow() {
    WNDCLASSW wc{};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"RemoteDisplay";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW); // Show cursor
    RegisterClassW(&wc);

    hwnd = CreateWindowW(L"RemoteDisplay", L"Waiting for Stream...", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        100, 100, 800, 600, NULL, NULL, wc.hInstance, NULL);
}

void updateWindowSize(int w, int h) {
    if (w != currentW || h != currentH) {
        currentW = w;
        currentH = h;
        frameBuffer.resize(w * h * 4);
        displayBuffer.resize(w * h * 4);
        
        SetWindowPos(hwnd, NULL, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER);
        SetWindowTextW(hwnd, L"Remote Stream (Connected + Input Enabled)");
        
        HDC hdc = GetDC(hwnd);
        if (hBitmap) DeleteObject(hBitmap);
        hBitmap = CreateCompatibleBitmap(hdc, w, h);
        ReleaseDC(hwnd, hdc);
    }
}

void drawFrame() {
    if (!hBitmap || currentW == 0) return;

    HDC hdc = GetDC(hwnd);
    HDC memDC = CreateCompatibleDC(hdc);
    SelectObject(memDC, hBitmap);

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = currentW;
    bmi.bmiHeader.biHeight = -currentH;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    SetDIBits(memDC, hBitmap, 0, currentH, displayBuffer.data(), &bmi, DIB_RGB_COLORS);
    BitBlt(hdc, 0, 0, currentW, currentH, memDC, 0, 0, SRCCOPY);

    DeleteDC(memDC);
    ReleaseDC(hwnd, hdc);
}

int main() {
    std::string targetIP;
    std::cout << "Enter Host IP (e.g., 127.0.0.1 or 192.168.1.5): ";
    std::cin >> targetIP;

    initWindow();

    WSADATA wsa;
    WSAStartup(MAKEWORD(2,2), &wsa);

    sock = socket(AF_INET, SOCK_DGRAM, 0);
    int buffSize = 1024 * 1024 * 10; 
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (char*)&buffSize, sizeof(buffSize));

    DWORD timeout = 2000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));

    sockaddr_in clientAddr{};
    clientAddr.sin_family = AF_INET;
    clientAddr.sin_port = htons(CLIENT_PORT);
    clientAddr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock, (sockaddr*)&clientAddr, sizeof(clientAddr)) == SOCKET_ERROR) {
        std::cout << "[ERROR] Bind failed.\n";
        return 1;
    }

    hostAddrGlobal.sin_family = AF_INET;
    hostAddrGlobal.sin_port = htons(HOST_PORT);
    hostAddrGlobal.sin_addr.s_addr = inet_addr(targetIP.c_str());

    // Send Auth
    sendto(sock, deviceKey.c_str(), deviceKey.size(), 0, (sockaddr*)&hostAddrGlobal, sizeof(hostAddrGlobal));
    std::cout << "[INFO] Sent auth key. Input is enabled.\n";

    std::vector<char> recvBuffer(MAX_PACKET_SIZE);
    sockaddr_in senderAddr;
    int senderSize = sizeof(senderAddr);

    MSG msg;
    while (true) {
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) return 0;
        }

        int len = recvfrom(sock, recvBuffer.data(), MAX_PACKET_SIZE, 0, (sockaddr*)&senderAddr, &senderSize);

        if (len > sizeof(PacketHeader)) {
            PacketHeader* header = (PacketHeader*)recvBuffer.data();
            updateWindowSize(header->width, header->height);

            if (header->offset + header->dataLen <= frameBuffer.size()) {
                memcpy(frameBuffer.data() + header->offset, 
                       recvBuffer.data() + sizeof(PacketHeader), 
                       header->dataLen);
            }

            if (header->offset + header->dataLen >= header->totalSize) {
                displayBuffer = frameBuffer; 
                drawFrame();
            }
        }
    }
}
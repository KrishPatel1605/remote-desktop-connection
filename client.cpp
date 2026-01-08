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
#define MAX_PACKET_SIZE 65535 // Max buffer for recv

std::string deviceKey = "TEST_KEY_123";

// Must match Host struct exactly
struct PacketHeader {
    int offset;
    int dataLen;
    int totalSize;
    int width;
    int height;
};

// Globals for Window and Graphics
HWND hwnd;
HBITMAP hBitmap = NULL;
int currentW = 0, currentH = 0;
std::vector<char> frameBuffer; // Reassembly buffer
std::vector<char> displayBuffer; // Buffer to draw from (double buffering)

LRESULT CALLBACK WindowProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_DESTROY) PostQuitMessage(0);
    return DefWindowProcW(h, msg, wp, lp);
}

void initWindow() {
    WNDCLASSW wc{};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"RemoteDisplay";
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
        
        // Resize Window
        SetWindowPos(hwnd, NULL, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER);
        SetWindowTextW(hwnd, L"Remote Stream (Connected)");
        
        // Reset Bitmap
        HDC hdc = GetDC(hwnd);
        if (hBitmap) DeleteObject(hBitmap);
        hBitmap = CreateCompatibleBitmap(hdc, w, h);
        ReleaseDC(hwnd, hdc);
        
        std::cout << "[INFO] Resized buffer to: " << w << "x" << h << "\n";
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

    // Use SetDIBits to put pixel data into the HBITMAP
    SetDIBits(memDC, hBitmap, 0, currentH, displayBuffer.data(), &bmi, DIB_RGB_COLORS);
    
    // Blit to screen
    BitBlt(hdc, 0, 0, currentW, currentH, memDC, 0, 0, SRCCOPY);

    DeleteDC(memDC);
    ReleaseDC(hwnd, hdc);
}

int main() {
    // Console setup for IP input
    std::string targetIP;
    std::cout << "Enter Host IP (e.g., 127.0.0.1 or 192.168.1.5): ";
    std::cin >> targetIP;

    initWindow();

    WSADATA wsa;
    WSAStartup(MAKEWORD(2,2), &wsa);

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, 0);
    
    // Increase receive buffer to handle incoming burst
    int buffSize = 1024 * 1024 * 10; 
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (char*)&buffSize, sizeof(buffSize));

    DWORD timeout = 2000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));

    sockaddr_in clientAddr{};
    clientAddr.sin_family = AF_INET;
    clientAddr.sin_port = htons(CLIENT_PORT);
    clientAddr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock, (sockaddr*)&clientAddr, sizeof(clientAddr)) == SOCKET_ERROR) {
        std::cout << "[ERROR] Bind failed. Port " << CLIENT_PORT << " might be in use.\n";
        return 1;
    }

    // --- Send Auth Key ---
    sockaddr_in hostAddr{};
    hostAddr.sin_family = AF_INET;
    hostAddr.sin_port = htons(HOST_PORT);
    hostAddr.sin_addr.s_addr = inet_addr(targetIP.c_str());

    sendto(sock, deviceKey.c_str(), deviceKey.size(), 0, (sockaddr*)&hostAddr, sizeof(hostAddr));
    std::cout << "[INFO] Sent auth key to " << targetIP << ". Waiting for stream...\n";

    // --- Receive Loop ---
    std::vector<char> recvBuffer(MAX_PACKET_SIZE);
    sockaddr_in senderAddr;
    int senderSize = sizeof(senderAddr);
    int receivedBytesThisFrame = 0;

    MSG msg;
    while (true) {
        // Handle Windows Messages (keep window responsive)
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) return 0;
        }

        int len = recvfrom(sock, recvBuffer.data(), MAX_PACKET_SIZE, 0, (sockaddr*)&senderAddr, &senderSize);

        if (len > sizeof(PacketHeader)) {
            // 1. Extract Header
            PacketHeader* header = (PacketHeader*)recvBuffer.data();
            
            // 2. Initialize/Resize if resolution changed
            updateWindowSize(header->width, header->height);

            // 3. Copy Data to correct offset
            if (header->offset + header->dataLen <= frameBuffer.size()) {
                memcpy(frameBuffer.data() + header->offset, 
                       recvBuffer.data() + sizeof(PacketHeader), 
                       header->dataLen);
                
                receivedBytesThisFrame += header->dataLen;
            }

            // 4. Check if we reached the end of the frame (simple logic)
            // Note: UDP is unreliable, packets might be lost. 
            // We draw whenever we get the last chunk or enough data.
            if (header->offset + header->dataLen >= header->totalSize) {
                // Swap buffers to avoid flickering
                displayBuffer = frameBuffer; 
                drawFrame();
                receivedBytesThisFrame = 0;
            }
        }
    }
}
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <gdiplus.h> // Added GDI+ for JPEG decoding
#include <iostream>
#include <string>
#include <vector>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "gdiplus.lib") // Link GDI+

using namespace Gdiplus;

#define HOST_PORT 50005
#define CLIENT_PORT 50006
#define MAX_PACKET_SIZE 65535 

std::string deviceKey = "TEST_KEY_123";

// FIXED: Force packing to match Host and Python
#pragma pack(push, 1)
struct PacketHeader {
    int offset;
    int dataLen;
    int totalSize;
    int width;
    int height;
};
#pragma pack(pop)

struct InputPacket {
    int type; 
    int x;
    int y;
    int key;
};

// Globals
HWND hwnd;
int currentW = 0, currentH = 0;
SOCKET sock; 
sockaddr_in hostAddrGlobal; 
std::vector<char> frameBuffer; 
std::vector<char> displayBuffer; 

void SendInputPacket(int type, int x, int y, int key) {
    if (currentW == 0 || currentH == 0) return;
    
    // Simple scaling assuming window rect matches stream rect
    InputPacket pkt;
    pkt.type = type;
    pkt.x = x; 
    pkt.y = y;
    pkt.key = key;

    RECT rect;
    if(GetClientRect(hwnd, &rect)) {
        int winW = rect.right - rect.left;
        int winH = rect.bottom - rect.top;
        if (winW > 0 && winH > 0) {
            // Scale mouse from Window Size to Stream Resolution
            pkt.x = (x * currentW) / winW;
            pkt.y = (y * currentH) / winH;
        }
    }
    sendto(sock, (char*)&pkt, sizeof(pkt), 0, (sockaddr*)&hostAddrGlobal, sizeof(hostAddrGlobal));
}

LRESULT CALLBACK WindowProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch(msg) {
        case WM_DESTROY: PostQuitMessage(0); return 0;
        case WM_MOUSEMOVE: SendInputPacket(1, LOWORD(lp), HIWORD(lp), 0); break;
        case WM_LBUTTONDOWN: SendInputPacket(2, LOWORD(lp), HIWORD(lp), 0); break;
        case WM_LBUTTONUP: SendInputPacket(3, LOWORD(lp), HIWORD(lp), 0); break;
        case WM_RBUTTONDOWN: SendInputPacket(4, LOWORD(lp), HIWORD(lp), 0); break;
        case WM_RBUTTONUP: SendInputPacket(5, LOWORD(lp), HIWORD(lp), 0); break;
        case WM_KEYDOWN: SendInputPacket(6, 0, 0, (int)wp); break;
        case WM_KEYUP: SendInputPacket(7, 0, 0, (int)wp); break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void initWindow() {
    WNDCLASSW wc{};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"RemoteDisplay";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);
    hwnd = CreateWindowW(L"RemoteDisplay", L"Waiting for Stream...", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 800, 600, NULL, NULL, wc.hInstance, NULL);
}

void updateWindowSize(int w, int h) {
    if (w <= 0 || h <= 0 || w > 10000 || h > 10000) return;
    if (w != currentW || h != currentH) {
        currentW = w;
        currentH = h;
        try {
            frameBuffer.resize(w * h * 4); // Roughly enough for raw, definitely enough for JPEG
            displayBuffer.resize(w * h * 4);
        } catch (...) { return; }
        
        SetWindowPos(hwnd, NULL, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER);
        SetWindowTextW(hwnd, L"Remote Stream (Connected)");
    }
}

// FIXED: Draw function now Decodes JPEG instead of dumping raw bits
void drawFrame(int dataSize) {
    if (currentW == 0 || dataSize <= 0) return;

    // 1. Create IStream from the Memory Buffer containing the JPEG
    IStream* pStream = NULL;
    // We must allocate global memory for CreateStreamOnHGlobal to work reliably with GDI+
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, dataSize);
    if (!hMem) return;
    
    void* pData = GlobalLock(hMem);
    memcpy(pData, displayBuffer.data(), dataSize);
    GlobalUnlock(hMem);

    if (CreateStreamOnHGlobal(hMem, TRUE, &pStream) == S_OK) {
        // 2. Load Image from Stream (Decodes JPEG)
        Bitmap* bmp = Bitmap::FromStream(pStream);
        if (bmp && bmp->GetLastStatus() == Ok) {
            
            // 3. Draw to Screen
            HDC hdc = GetDC(hwnd);
            Graphics graphics(hdc);
            
            RECT rect;
            GetClientRect(hwnd, &rect);
            graphics.DrawImage(bmp, 0, 0, rect.right, rect.bottom); // Draw scaled to window
            
            ReleaseDC(hwnd, hdc);
            delete bmp;
        }
        pStream->Release();
    }
    // hMem is freed by pStream->Release() because fDeleteOnRelease=TRUE
}

int main() {
    std::string targetIP;
    std::cout << "Enter Host IP: ";
    std::cin >> targetIP;

    // Initialize GDI+ for Client too!
    GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken;
    GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);

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

    sendto(sock, deviceKey.c_str(), deviceKey.size(), 0, (sockaddr*)&hostAddrGlobal, sizeof(hostAddrGlobal));
    std::cout << "[INFO] Connected. Window should appear shortly.\n";

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
                // Pass total size to draw function so it knows how much of buffer is real JPEG
                drawFrame(header->totalSize); 
            }
        }
    }
}
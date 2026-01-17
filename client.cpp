#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <gdiplus.h>
#include <iostream>
#include <string>
#include <vector>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "gdiplus.lib")

using namespace Gdiplus;

#define HOST_PORT 50005
#define CLIENT_PORT 50006
#define MAX_PACKET_SIZE 65535

std::string deviceKey = "TEST_KEY_123";

#pragma pack(push, 1)
struct PacketHeader
{
    int offset;
    int dataLen;
    int totalSize;
    int width;
    int height;
};
#pragma pack(pop)

struct InputPacket
{
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

// Optimization: Reuse Graphics object logic where possible or keep it simple
// GDI+ Graphics creation is relatively cheap compared to network, but we'll optimize drawing.

void SendInputPacket(int type, int x, int y, int key)
{
    if (currentW == 0 || currentH == 0) return;

    InputPacket pkt;
    pkt.type = type;
    pkt.x = x;
    pkt.y = y;
    pkt.key = key;

    RECT rect;
    if (GetClientRect(hwnd, &rect))
    {
        int winW = rect.right - rect.left;
        int winH = rect.bottom - rect.top;
        if (winW > 0 && winH > 0)
        {
            pkt.x = (x * currentW) / winW;
            pkt.y = (y * currentH) / winH;
        }
    }
    sendto(sock, (char *)&pkt, sizeof(pkt), 0, (sockaddr *)&hostAddrGlobal, sizeof(hostAddrGlobal));
}

LRESULT CALLBACK WindowProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_DESTROY: PostQuitMessage(0); return 0;
    // Mouse input
    case WM_MOUSEMOVE: SendInputPacket(1, LOWORD(lp), HIWORD(lp), 0); break;
    case WM_LBUTTONDOWN: SendInputPacket(2, LOWORD(lp), HIWORD(lp), 0); break;
    case WM_LBUTTONUP: SendInputPacket(3, LOWORD(lp), HIWORD(lp), 0); break;
    case WM_RBUTTONDOWN: SendInputPacket(4, LOWORD(lp), HIWORD(lp), 0); break;
    case WM_RBUTTONUP: SendInputPacket(5, LOWORD(lp), HIWORD(lp), 0); break;
    // Keyboard input
    case WM_KEYDOWN: SendInputPacket(6, 0, 0, (int)wp); break;
    case WM_KEYUP: SendInputPacket(7, 0, 0, (int)wp); break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void initWindow()
{
    WNDCLASSW wc{};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"RemoteDisplay";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);
    hwnd = CreateWindowW(L"RemoteDisplay", L"Waiting for Stream...", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 1280, 720, NULL, NULL, wc.hInstance, NULL);
}

void updateWindowSize(int w, int h)
{
    if (w <= 0 || h <= 0) return;
    if (w != currentW || h != currentH)
    {
        currentW = w;
        currentH = h;
        try {
            frameBuffer.resize(w * h * 4);
            displayBuffer.resize(w * h * 4);
        } catch (...) { return; }
        
        SetWindowTextW(hwnd, L"Remote Stream (High Performance)");
    }
}

void drawFrame(int dataSize)
{
    if (currentW == 0 || dataSize <= 0) return;

    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, dataSize);
    if (!hMem) return;

    void *pData = GlobalLock(hMem);
    memcpy(pData, displayBuffer.data(), dataSize);
    GlobalUnlock(hMem);

    IStream *pStream = NULL;
    if (CreateStreamOnHGlobal(hMem, TRUE, &pStream) == S_OK)
    {
        // Decode
        Bitmap *bmp = Bitmap::FromStream(pStream);
        if (bmp && bmp->GetLastStatus() == Ok)
        {
            HDC hdc = GetDC(hwnd);
            Graphics graphics(hdc);
            
            // Optimization: Set interpolation to NearestNeighbor for speed if you want retro look, 
            // but Default is usually fine. LowQuality = Faster.
            graphics.SetCompositingMode(CompositingModeSourceCopy);
            graphics.SetCompositingQuality(CompositingQualityHighSpeed);
            graphics.SetInterpolationMode(InterpolationModeLowQuality); 
            graphics.SetSmoothingMode(SmoothingModeHighSpeed);

            RECT rect;
            GetClientRect(hwnd, &rect);
            graphics.DrawImage(bmp, 0, 0, rect.right, rect.bottom);

            ReleaseDC(hwnd, hdc);
            delete bmp;
        }
        pStream->Release();
    }
}

int main()
{
    std::string targetIP;
    std::cout << "Enter Host IP: ";
    std::cin >> targetIP;

    GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken;
    GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);

    initWindow();

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    sock = socket(AF_INET, SOCK_DGRAM, 0);
    // Massive buffer for 60FPS
    int buffSize = 1024 * 1024 * 32; 
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (char *)&buffSize, sizeof(buffSize));
    
    // Set a very short timeout so we can process window messages frequently
    DWORD timeout = 5; 
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout));

    sockaddr_in clientAddr{};
    clientAddr.sin_family = AF_INET;
    clientAddr.sin_port = htons(CLIENT_PORT);
    clientAddr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock, (sockaddr *)&clientAddr, sizeof(clientAddr)) == SOCKET_ERROR)
    {
        std::cout << "[ERROR] Bind failed.\n";
        return 1;
    }

    hostAddrGlobal.sin_family = AF_INET;
    hostAddrGlobal.sin_port = htons(HOST_PORT);
    hostAddrGlobal.sin_addr.s_addr = inet_addr(targetIP.c_str());

    sendto(sock, deviceKey.c_str(), deviceKey.size(), 0, (sockaddr *)&hostAddrGlobal, sizeof(hostAddrGlobal));
    std::cout << "[INFO] Connected.\n";

    std::vector<char> recvBuffer(MAX_PACKET_SIZE);
    sockaddr_in senderAddr;
    int senderSize = sizeof(senderAddr);

    MSG msg;
    while (true)
    {
        // Process Windows events
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) return 0;
        }

        // Receive Data
        int len = recvfrom(sock, recvBuffer.data(), MAX_PACKET_SIZE, 0, (sockaddr *)&senderAddr, &senderSize);

        if (len > sizeof(PacketHeader))
        {
            PacketHeader *header = (PacketHeader *)recvBuffer.data();
            updateWindowSize(header->width, header->height);

            if (header->offset + header->dataLen <= frameBuffer.size())
            {
                memcpy(frameBuffer.data() + header->offset,
                       recvBuffer.data() + sizeof(PacketHeader),
                       header->dataLen);
            }

            if (header->offset + header->dataLen >= header->totalSize)
            {
                displayBuffer = frameBuffer;
                drawFrame(header->totalSize);
            }
        }
    }
}
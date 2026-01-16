// DEFINED FIRST: Force Windows 7+ API visibility to fix "SetProcessDPIAware undefined"
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601 
#endif

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
#pragma comment(lib, "user32.lib")

using namespace Gdiplus;

#define LISTEN_PORT 50005
#define STREAM_PORT 50006
#define MAX_PACKET_SIZE 60000
#define JPEG_QUALITY 40

std::string deviceKey = "TEST_KEY_123";

int g_screenW = 0;
int g_screenH = 0;

// CHANGED: Resolution set to 1920x1080
int g_sendW = 1920;
int g_sendH = 1080;

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

// Check if the process has Administrator privileges
bool IsElevated()
{
    bool fRet = false;
    HANDLE hToken = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken))
    {
        TOKEN_ELEVATION Elevation;
        DWORD cbSize = sizeof(TOKEN_ELEVATION);
        if (GetTokenInformation(hToken, TokenElevation, &Elevation, sizeof(Elevation), &cbSize))
        {
            fRet = Elevation.TokenIsElevated;
        }
    }
    if (hToken)
    {
        CloseHandle(hToken);
    }
    return fRet;
}

// Enhanced reporter to find the correct local IP for your router table
void PrintLocalIPInfo()
{
    char szHostName[255];
    if (gethostname(szHostName, 255) == 0)
    {
        struct hostent *host_entry = gethostbyname(szHostName);
        if (host_entry)
        {
            std::cout << "--- NETWORK CONFIGURATION CHECK ---\n";
            for (int i = 0; host_entry->h_addr_list[i] != NULL; ++i)
            {
                char *szLocalIP = inet_ntoa(*(struct in_addr *)host_entry->h_addr_list[i]);
                std::cout << "[CHECK] Found Local IP: " << szLocalIP << "\n";
            }
            std::cout << "[ACTION] Ensure the IP in your Router Table matches one of these.\n";
            std::cout << "-----------------------------------\n";
        }
    }
}

int GetEncoderClsid(const WCHAR *format, CLSID *pClsid)
{
    UINT num = 0, size = 0;
    GetImageEncodersSize(&num, &size);
    if (size == 0)
        return -1;
    ImageCodecInfo *pImageCodecInfo = (ImageCodecInfo *)(malloc(size));
    if (pImageCodecInfo == NULL)
        return -1;
    GetImageEncoders(num, size, pImageCodecInfo);
    for (UINT j = 0; j < num; ++j)
    {
        if (wcscmp(pImageCodecInfo[j].MimeType, format) == 0)
        {
            *pClsid = pImageCodecInfo[j].Clsid;
            free(pImageCodecInfo);
            return j;
        }
    }
    free(pImageCodecInfo);
    return -1;
}

bool validateIncomingKey(const std::string &incoming)
{
    return incoming == deviceKey;
}

DWORD WINAPI InputListener(LPVOID lpParam)
{
    SOCKET sock = (SOCKET)lpParam;
    sockaddr_in senderAddr;
    int senderSize = sizeof(senderAddr);
    char buffer[1024];

    while (true)
    {
        int recvLen = recvfrom(sock, buffer, sizeof(buffer), 0, (sockaddr *)&senderAddr, &senderSize);
        if (recvLen == sizeof(InputPacket))
        {
            InputPacket *pkt = (InputPacket *)buffer;
            
            // Map input back to real screen coordinates
            int realX = (pkt->x * g_screenW) / g_sendW;
            int realY = (pkt->y * g_screenH) / g_sendH;

            switch (pkt->type)
            {
            case 1:
                SetCursorPos(realX, realY);
                break;
            case 2:
                SetCursorPos(realX, realY);
                mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
                break;
            case 3:
                mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
                break;
            case 4:
                SetCursorPos(realX, realY);
                mouse_event(MOUSEEVENTF_RIGHTDOWN, 0, 0, 0, 0);
                break;
            case 5:
                mouse_event(MOUSEEVENTF_RIGHTUP, 0, 0, 0, 0);
                break;
            case 6:
                keybd_event((BYTE)pkt->key, 0, 0, 0);
                break;
            case 7:
                keybd_event((BYTE)pkt->key, 0, KEYEVENTF_KEYUP, 0);
                break;
            }
        }
    }
    return 0;
}

int main()
{
    // CHANGED: Fix for "cut off" screen and incorrect mouse mapping on high DPI displays
    SetProcessDPIAware();

    // WARNING CHECK: Inform user if not running as Admin
    if (!IsElevated())
    {
        std::cout << "\n===================================================\n";
        std::cout << "[WARNING] NOT RUNNING AS ADMINISTRATOR\n";
        std::cout << "You will NOT be able to control Task Manager or \n";
        std::cout << "other system windows unless you run this as Admin.\n";
        std::cout << "Please restart and 'Run as Administrator'.\n";
        std::cout << "===================================================\n\n";
    }

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    PrintLocalIPInfo();

    GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken;
    GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);

    CLSID jpgClsid;
    GetEncoderClsid(L"image/jpeg", &jpgClsid);

    EncoderParameters encoderParameters;
    encoderParameters.Count = 1;
    encoderParameters.Parameter[0].Guid = EncoderQuality;
    encoderParameters.Parameter[0].Type = EncoderParameterValueTypeLong;
    encoderParameters.Parameter[0].NumberOfValues = 1;
    ULONG quality = JPEG_QUALITY;
    encoderParameters.Parameter[0].Value = &quality;

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, 0);
    int buffSize = 1024 * 1024 * 10;
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, (char *)&buffSize, sizeof(buffSize));
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (char *)&buffSize, sizeof(buffSize));

    sockaddr_in serverAddr{}, clientAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(LISTEN_PORT);
    serverAddr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock, (sockaddr *)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR)
    {
        std::cout << "[ERROR] Host bind failed.\n";
        return 1;
    }

    std::cout << "[INFO] Host is running (1920x1080). Waiting for connection on port " << LISTEN_PORT << "...\n";

    int clientSize = sizeof(clientAddr);
    char authBuffer[1024];
    bool authenticated = false;

    while (!authenticated)
    {
        int recvLen = recvfrom(sock, authBuffer, sizeof(authBuffer) - 1, 0, (sockaddr *)&clientAddr, &clientSize);
        if (recvLen > 0)
        {
            authBuffer[recvLen] = '\0';
            if (validateIncomingKey(authBuffer))
            {
                authenticated = true;
                clientAddr.sin_port = htons(STREAM_PORT);
                std::cout << "[SUCCESS] External Client authenticated from: " << inet_ntoa(clientAddr.sin_addr) << "\n";
            }
        }
    }

    g_screenW = GetSystemMetrics(SM_CXSCREEN);
    g_screenH = GetSystemMetrics(SM_CYSCREEN);
    std::cout << "[INFO] Capturing Real Screen Size: " << g_screenW << "x" << g_screenH << "\n";

    CreateThread(NULL, 0, InputListener, (LPVOID)sock, 0, NULL);

    HDC screenDC = GetDC(NULL);
    HDC memDC = CreateCompatibleDC(screenDC);
    HBITMAP hBitmap = CreateCompatibleBitmap(screenDC, g_sendW, g_sendH);
    SelectObject(memDC, hBitmap);
    SetStretchBltMode(memDC, COLORONCOLOR);

    std::vector<char> sendBuffer(MAX_PACKET_SIZE + sizeof(PacketHeader));
    IStream *pStream = NULL;

    while (true)
    {
        // StretchBlt captures the screen (screenDC) and resizes it to our send resolution (memDC)
        StretchBlt(memDC, 0, 0, g_sendW, g_sendH, screenDC, 0, 0, g_screenW, g_screenH, SRCCOPY);
        
        CreateStreamOnHGlobal(NULL, TRUE, &pStream);
        Bitmap *bmp = Bitmap::FromHBITMAP(hBitmap, NULL);
        bmp->Save(pStream, &jpgClsid, &encoderParameters);
        delete bmp;

        HGLOBAL hMem = NULL;
        GetHGlobalFromStream(pStream, &hMem);
        void *pData = GlobalLock(hMem);
        int streamSize = GlobalSize(hMem);
        char *pBytes = (char *)pData;

        int currentOffset = 0;
        while (currentOffset < streamSize)
        {
            int remaining = streamSize - currentOffset;
            int chunkLen = (remaining > MAX_PACKET_SIZE) ? MAX_PACKET_SIZE : remaining;

            PacketHeader header;
            header.offset = currentOffset;
            header.dataLen = chunkLen;
            header.totalSize = streamSize;
            header.width = g_sendW;
            header.height = g_sendH;

            memcpy(sendBuffer.data(), &header, sizeof(PacketHeader));
            memcpy(sendBuffer.data() + sizeof(PacketHeader), pBytes + currentOffset, chunkLen);

            sendto(sock, sendBuffer.data(), sizeof(PacketHeader) + chunkLen, 0, (sockaddr *)&clientAddr, sizeof(clientAddr));
            currentOffset += chunkLen;
            if (chunkLen == MAX_PACKET_SIZE)
                Sleep(2);
        }

        GlobalUnlock(hMem);
        pStream->Release();
        Sleep(33);
    }
}
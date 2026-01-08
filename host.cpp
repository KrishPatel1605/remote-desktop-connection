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

#define LISTEN_PORT 50005
#define STREAM_PORT 50006
#define MAX_PACKET_SIZE 60000 

// JPEG Compression Quality (0-100)
// 50 is a good balance for speed/size
#define JPEG_QUALITY 50 

std::string deviceKey = "TEST_KEY_123";

struct PacketHeader {
    int offset;
    int dataLen;
    int totalSize;
    int type; // 0 = Raw, 1 = JPEG
};

// Helper to get encoder CLSID for JPEG
int GetEncoderClsid(const WCHAR* format, CLSID* pClsid) {
    UINT  num = 0;          // number of image encoders
    UINT  size = 0;         // size of the image encoder array in bytes
    GetImageEncodersSize(&num, &size);
    if (size == 0) return -1;
    ImageCodecInfo* pImageCodecInfo = (ImageCodecInfo*)(malloc(size));
    if (pImageCodecInfo == NULL) return -1;
    GetImageEncoders(num, size, pImageCodecInfo);
    for (UINT j = 0; j < num; ++j) {
        if (wcscmp(pImageCodecInfo[j].MimeType, format) == 0) {
            *pClsid = pImageCodecInfo[j].Clsid;
            free(pImageCodecInfo);
            return j;
        }
    }
    free(pImageCodecInfo);
    return -1;
}

bool validateIncomingKey(const std::string& incoming) {
    return incoming == deviceKey;
}

int main() {
    // 1. Initialize Winsock
    WSADATA wsa;
    WSAStartup(MAKEWORD(2,2), &wsa);

    // 2. Initialize GDI+
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
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, (char*)&buffSize, sizeof(buffSize));

    sockaddr_in serverAddr{}, clientAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(LISTEN_PORT);
    serverAddr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        std::cout << "[ERROR] Host bind failed.\n";
        return 1;
    }

    std::cout << "[INFO] Waiting for client...\n";

    int clientSize = sizeof(clientAddr);
    char authBuffer[1024];
    bool authenticated = false;

    while (!authenticated) {
        int recvLen = recvfrom(sock, authBuffer, sizeof(authBuffer)-1, 0, (sockaddr*)&clientAddr, &clientSize);
        if (recvLen > 0) {
            authBuffer[recvLen] = '\0';
            if (validateIncomingKey(authBuffer)) {
                authenticated = true;
                clientAddr.sin_port = htons(STREAM_PORT);
                std::cout << "[SUCCESS] Client authenticated!\n";
            }
        }
    }

    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    
    // --- USER REQUESTED RESOLUTION ---
    // Forcing exact resolution: 2340x1080
    int sendW = 2340;
    int sendH = 1080;
    
    HDC screenDC = GetDC(NULL);
    HDC memDC = CreateCompatibleDC(screenDC);
    HBITMAP hBitmap = CreateCompatibleBitmap(screenDC, sendW, sendH);
    SelectObject(memDC, hBitmap);
    SetStretchBltMode(memDC, COLORONCOLOR);

    std::vector<char> sendBuffer(MAX_PACKET_SIZE + sizeof(PacketHeader));
    IStream* pStream = NULL;

    std::cout << "[INFO] Streaming MJPEG at " << sendW << "x" << sendH << " (Forced Res)\n";

    while (true) {
        // Capture
        StretchBlt(memDC, 0, 0, sendW, sendH, screenDC, 0, 0, screenW, screenH, SRCCOPY);

        // Compress to JPEG in Memory
        CreateStreamOnHGlobal(NULL, TRUE, &pStream);
        Bitmap* bmp = Bitmap::FromHBITMAP(hBitmap, NULL);
        bmp->Save(pStream, &jpgClsid, &encoderParameters);
        delete bmp; // Cleanup GDI+ wrapper

        // Get Pointer to Data
        HGLOBAL hMem = NULL;
        GetHGlobalFromStream(pStream, &hMem);
        void* pData = GlobalLock(hMem);
        int streamSize = GlobalSize(hMem);

        // Fragment and Send
        int currentOffset = 0;
        char* pBytes = (char*)pData;

        while (currentOffset < streamSize) {
            int remaining = streamSize - currentOffset;
            int chunkLen = (remaining > MAX_PACKET_SIZE) ? MAX_PACKET_SIZE : remaining;

            PacketHeader header;
            header.offset = currentOffset;
            header.dataLen = chunkLen;
            header.totalSize = streamSize;
            header.type = 1; // 1 = JPEG

            memcpy(sendBuffer.data(), &header, sizeof(PacketHeader));
            memcpy(sendBuffer.data() + sizeof(PacketHeader), pBytes + currentOffset, chunkLen);

            sendto(sock, sendBuffer.data(), sizeof(PacketHeader) + chunkLen, 0, (sockaddr*)&clientAddr, sizeof(clientAddr));
            currentOffset += chunkLen;
            
            // Tiny sleep to prevent UDP packet loss on router
            if (chunkLen == MAX_PACKET_SIZE) Sleep(1);
        }

        GlobalUnlock(hMem);
        pStream->Release();

        // 30 FPS target
        Sleep(33);
    }
}
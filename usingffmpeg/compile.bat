g++ host_ffmpeg.cpp -o host.exe -lws2_32 -luser32
g++ client_ffmpeg.cpp -o client.exe -lws2_32 -luser32 -lgdi32
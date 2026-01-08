import socket
import struct
import sys
import threading
import tkinter as tk
import io
from tkinter import simpledialog, messagebox
from PIL import Image, ImageTk

# Configuration
HOST_PORT = 50005
CLIENT_PORT = 50006
DEVICE_KEY = "TEST_KEY_123"
MAX_PACKET_SIZE = 65535

# Global variables
current_frame = None
is_running = True
frame_lock = threading.Lock()

def udp_listener(host_ip):
    global current_frame, is_running
    
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 2 * 1024 * 1024) # 2MB Buffer is enough for JPEG
    except: pass

    try:
        sock.bind(("0.0.0.0", CLIENT_PORT))
    except OSError:
        messagebox.showerror("Error", f"Port {CLIENT_PORT} is busy!")
        return

    sock.settimeout(1.0) 

    # Auth
    try:
        sock.sendto(DEVICE_KEY.encode(), (host_ip, HOST_PORT))
    except: pass

    frame_buffer = bytearray()
    
    while is_running:
        try:
            data, addr = sock.recvfrom(MAX_PACKET_SIZE)
            
            if len(data) < 16: continue # Header is smaller now

            # Header: offset, dataLen, totalSize, type
            offset, data_len, total_size, img_type = struct.unpack('iiii', data[:16])
            
            # Reset buffer for new frame (offset 0)
            if offset == 0:
                frame_buffer = bytearray(total_size)
            
            # Copy data
            img_data = data[16:]
            if offset + len(img_data) <= total_size:
                frame_buffer[offset : offset + len(img_data)] = img_data

            # Render if complete
            if offset + data_len >= total_size:
                try:
                    # Load JPEG directly from memory
                    stream = io.BytesIO(frame_buffer)
                    img = Image.open(stream)
                    
                    with frame_lock:
                        current_frame = img
                except Exception as e:
                    pass
                    
        except socket.timeout:
            sock.sendto(DEVICE_KEY.encode(), (host_ip, HOST_PORT))
        except OSError:
            break

    sock.close()

class RemoteScreenApp:
    def __init__(self, root, host_ip):
        self.root = root
        self.root.title("Remote Stream (MJPEG)")
        self.root.geometry("800x600")
        
        self.label = tk.Label(root, text="Connecting...", bg="black", fg="white")
        self.label.pack(expand=True, fill=tk.BOTH)
        
        self.thread = threading.Thread(target=udp_listener, args=(host_ip,))
        self.thread.daemon = True
        self.thread.start()
        
        self.update_ui()

    def update_ui(self):
        global current_frame
        with frame_lock:
            img = current_frame
            current_frame = None 

        if img:
            # Resize to fit window (optional)
            win_w = self.root.winfo_width()
            win_h = self.root.winfo_height()
            if win_w > 1 and win_h > 1:
                img = img.resize((win_w, win_h), Image.NEAREST)

            photo = ImageTk.PhotoImage(img)
            self.label.config(image=photo, text="")
            self.label.image = photo
        
        self.root.after(30, self.update_ui)

    def on_close(self):
        global is_running
        is_running = False
        self.root.destroy()
        sys.exit(0)

if __name__ == "__main__":
    root = tk.Tk()
    root.withdraw()
    host_ip = simpledialog.askstring("Connect", "Enter Host IP:", initialvalue="192.168.1.")
    if host_ip:
        root.deiconify()
        app = RemoteScreenApp(root, host_ip)
        root.protocol("WM_DELETE_WINDOW", app.on_close)
        root.mainloop()
    else:
        sys.exit()
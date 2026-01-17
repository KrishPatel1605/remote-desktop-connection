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

# Default placeholders
HOST_WIDTH = 1280
HOST_HEIGHT = 720

# Global variables
current_frame = None
is_running = True
frame_lock = threading.Lock()

# Socket for sending input (shared)
client_sock = None
host_address = None

def udp_listener(host_ip):
    global current_frame, is_running, client_sock, host_address, HOST_WIDTH, HOST_HEIGHT
    
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        # 4MB Buffer for smooth HD
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024) 
    except: pass

    try:
        sock.bind(("0.0.0.0", CLIENT_PORT))
    except OSError:
        messagebox.showerror("Error", f"Port {CLIENT_PORT} is busy!")
        return

    sock.settimeout(1.0) 

    client_sock = sock
    host_address = (host_ip, HOST_PORT)

    try:
        sock.sendto(DEVICE_KEY.encode(), host_address)
    except: pass

    frame_buffer = None
    current_frame_size = 0
    bytes_received = 0
    
    while is_running:
        try:
            data, addr = sock.recvfrom(MAX_PACKET_SIZE)
            if len(data) < 20: continue 

            offset, data_len, total_size, width, height = struct.unpack('iiiii', data[:20])
            HOST_WIDTH, HOST_HEIGHT = width, height
            
            # --- TEARING FIX ---
            if offset == 0:
                frame_buffer = bytearray(total_size)
                current_frame_size = total_size
                bytes_received = 0
            
            if frame_buffer is None or total_size != current_frame_size:
                continue

            img_data = data[20:] 
            data_len_actual = len(img_data)
            
            if offset + data_len_actual <= current_frame_size:
                frame_buffer[offset : offset + data_len_actual] = img_data
                bytes_received += data_len_actual

            if bytes_received >= current_frame_size:
                try:
                    stream = io.BytesIO(frame_buffer)
                    img = Image.open(stream)
                    img.load() 
                    
                    with frame_lock:
                        current_frame = img
                    
                    frame_buffer = None
                    bytes_received = 0
                except: pass
                    
        except socket.timeout:
            if client_sock and host_address:
                sock.sendto(DEVICE_KEY.encode(), host_address)
        except OSError:
            break
    sock.close()

class RemoteScreenApp:
    def __init__(self, root, host_ip):
        self.root = root
        self.root.title("Remote (Fast)")
        self.root.geometry("800x600")
        
        self.label = tk.Label(root, text="Connecting...", bg="black", fg="white")
        self.label.pack(expand=True, fill=tk.BOTH)

        self.touch_start_x = 0
        self.touch_start_y = 0
        
        self.label.bind('<Motion>', self.on_mouse_move)
        self.label.bind('<Button-1>', self.on_touch_start)
        self.label.bind('<B1-Motion>', self.on_touch_move) 
        self.label.bind('<ButtonRelease-1>', self.on_touch_end)
        self.label.bind('<Button-3>', lambda e: self.send_input(4, e.x, e.y, 0))
        self.label.bind('<ButtonRelease-3>', lambda e: self.send_input(5, e.x, e.y, 0))
        self.root.bind('<KeyPress>', self.on_key_down)
        self.root.bind('<KeyRelease>', self.on_key_up)

        self.thread = threading.Thread(target=udp_listener, args=(host_ip,))
        self.thread.daemon = True
        self.thread.start()
        
        # INCREASED REFRESH RATE: 10ms instead of 30ms for ~100FPS potential
        self.update_ui_loop()

    def send_input(self, type_id, x, y, key):
        if not client_sock or not host_address: return
        win_w = self.label.winfo_width()
        win_h = self.label.winfo_height()
        if win_w == 0 or win_h == 0: return

        scaled_x = int((x / win_w) * HOST_WIDTH)
        scaled_y = int((y / win_h) * HOST_HEIGHT)
        packet = struct.pack('iiii', type_id, scaled_x, scaled_y, key)
        try: client_sock.sendto(packet, host_address)
        except: pass

    def on_mouse_move(self, event): self.send_input(1, event.x, event.y, 0)
    def on_touch_start(self, event):
        self.touch_start_x = event.x
        self.touch_start_y = event.y
        self.send_input(1, event.x, event.y, 0)
    def on_touch_move(self, event): self.send_input(1, event.x, event.y, 0)
    def on_touch_end(self, event):
        dx = abs(event.x - self.touch_start_x)
        dy = abs(event.y - self.touch_start_y)
        if dx < 5 and dy < 5:
            self.send_input(2, event.x, event.y, 0)
            self.send_input(3, event.x, event.y, 0)
    def on_key_down(self, event): self.send_input(6, 0, 0, event.keycode)
    def on_key_up(self, event): self.send_input(7, 0, 0, event.keycode)

    def update_ui_loop(self):
        global current_frame
        img = None
        
        with frame_lock:
            if current_frame:
                img = current_frame
                current_frame = None 

        if img:
            win_w = self.root.winfo_width()
            win_h = self.root.winfo_height()
            if win_w > 1 and win_h > 1:
                # NEAREST is faster than ANTIALIAS
                img = img.resize((win_w, win_h), Image.NEAREST)

            photo = ImageTk.PhotoImage(img)
            self.label.config(image=photo, text="")
            self.label.image = photo
        
        # Check for new frames aggressively (every 5ms)
        self.root.after(5, self.update_ui_loop)

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
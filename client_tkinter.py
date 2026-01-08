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

# Expected Host Resolution (Fallback if needed)
HOST_WIDTH = 2340
HOST_HEIGHT = 1080

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
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 2 * 1024 * 1024) 
    except: pass

    try:
        sock.bind(("0.0.0.0", CLIENT_PORT))
    except OSError:
        messagebox.showerror("Error", f"Port {CLIENT_PORT} is busy!")
        return

    sock.settimeout(1.0) 

    # Shared socket reference for input sending
    client_sock = sock
    host_address = (host_ip, HOST_PORT)

    # Auth
    try:
        sock.sendto(DEVICE_KEY.encode(), host_address)
    except: pass

    frame_buffer = bytearray()
    
    while is_running:
        try:
            data, addr = sock.recvfrom(MAX_PACKET_SIZE)
            
            # FIXED: With #pragma pack(1) in C++, header is exactly 20 bytes.
            if len(data) < 20: continue 

            offset, data_len, total_size, width, height = struct.unpack('iiiii', data[:20])
            
            # Update globals for input scaling
            HOST_WIDTH = width
            HOST_HEIGHT = height
            
            if offset == 0:
                frame_buffer = bytearray(total_size)
            
            img_data = data[20:] 
            if offset + len(img_data) <= total_size:
                frame_buffer[offset : offset + len(img_data)] = img_data

            if offset + data_len >= total_size:
                try:
                    stream = io.BytesIO(frame_buffer)
                    img = Image.open(stream)
                    
                    with frame_lock:
                        current_frame = img
                except Exception as e:
                    pass
                    
        except socket.timeout:
            # Keep alive / re-auth
            if client_sock and host_address:
                sock.sendto(DEVICE_KEY.encode(), host_address)
        except OSError:
            break

    sock.close()

class RemoteScreenApp:
    def __init__(self, root, host_ip):
        self.root = root
        self.root.title("Remote Stream (MJPEG + Input)")
        self.root.geometry("800x600")
        
        self.label = tk.Label(root, text="Connecting...", bg="black", fg="white")
        self.label.pack(expand=True, fill=tk.BOTH)

        # Touch state for "Tap to Click" vs "Drag to Hover"
        self.touch_start_x = 0
        self.touch_start_y = 0
        
        # --- Input Bindings ---
        # Standard Mouse Move (Desktop)
        self.label.bind('<Motion>', self.on_mouse_move)
        
        # Touch / Left Click Handling
        # Replaced direct LDown/LUp with logic to separate Drag (Hover) from Tap (Click)
        self.label.bind('<Button-1>', self.on_touch_start)
        self.label.bind('<B1-Motion>', self.on_touch_move) 
        self.label.bind('<ButtonRelease-1>', self.on_touch_end)
        
        # Right Click (Keep standard)
        self.label.bind('<Button-3>', lambda e: self.send_input(4, e.x, e.y, 0)) # RDown
        self.label.bind('<ButtonRelease-3>', lambda e: self.send_input(5, e.x, e.y, 0)) # RUp
        
        # Keyboard
        self.root.bind('<KeyPress>', self.on_key_down)
        self.root.bind('<KeyRelease>', self.on_key_up)

        self.thread = threading.Thread(target=udp_listener, args=(host_ip,))
        self.thread.daemon = True
        self.thread.start()
        
        self.update_ui()

    def send_input(self, type_id, x, y, key):
        if not client_sock or not host_address: return

        # Get current display size of the label/image
        win_w = self.label.winfo_width()
        win_h = self.label.winfo_height()

        if win_w == 0 or win_h == 0: return

        # Scale coordinates to Host Resolution
        scaled_x = int((x / win_w) * HOST_WIDTH)
        scaled_y = int((y / win_h) * HOST_HEIGHT)

        # Struct format: 4 ints (type, x, y, key)
        packet = struct.pack('iiii', type_id, scaled_x, scaled_y, key)
        
        try:
            client_sock.sendto(packet, host_address)
        except: pass

    def on_mouse_move(self, event):
        # Throttle moves if necessary, but UDP is fast
        self.send_input(1, event.x, event.y, 0)

    # --- Touch Handling Logic ---
    def on_touch_start(self, event):
        # Record start position
        self.touch_start_x = event.x
        self.touch_start_y = event.y
        # Move cursor immediately to finger position (Hover)
        self.send_input(1, event.x, event.y, 0)

    def on_touch_move(self, event):
        # When dragging finger, treated as Moving Mouse (Hover), NOT dragging selection
        self.send_input(1, event.x, event.y, 0)

    def on_touch_end(self, event):
        # Check if we moved significantly
        dx = abs(event.x - self.touch_start_x)
        dy = abs(event.y - self.touch_start_y)
        
        # If movement is small (< 5 pixels), treat it as a TAP (Click)
        if dx < 5 and dy < 5:
            self.send_input(2, event.x, event.y, 0) # LDown
            self.send_input(3, event.x, event.y, 0) # LUp
        
        # If moved > 5 pixels, it was just a hover move, do nothing on release.

    def on_key_down(self, event):
        # event.keycode on Windows usually matches VK codes
        self.send_input(6, 0, 0, event.keycode)

    def on_key_up(self, event):
        self.send_input(7, 0, 0, event.keycode)

    def update_ui(self):
        global current_frame
        with frame_lock:
            img = current_frame
            current_frame = None 

        if img:
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
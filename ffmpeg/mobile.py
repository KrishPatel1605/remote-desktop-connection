import socket
import struct
import sys
import threading
import tkinter as tk
import cv2  # Requires: pip install opencv-python
from PIL import Image, ImageTk
from tkinter import simpledialog

# Configuration
INPUT_PORT = 50005
STREAM_PORT = 50006
HOST_WIDTH = 1280
HOST_HEIGHT = 720

class RemoteScreenApp:
    def __init__(self, root, host_ip):
        self.root = root
        self.root.title("Remote (FFmpeg + OpenCV)")
        self.root.geometry("1000x600") # Start a bit larger

        self.host_ip = host_ip
        self.host_address = (host_ip, INPUT_PORT)
        
        # Setup UDP Input Socket
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

        # UI Label for Video
        self.label = tk.Label(root, text="Waiting for stream...", bg="black", fg="white")
        self.label.pack(expand=True, fill=tk.BOTH)

        # --- INPUT BINDINGS (Touchpad) ---
        self.touch_start_x = 0
        self.touch_start_y = 0
        
        self.label.bind('<Button-1>', self.on_touch_start)
        self.label.bind('<B1-Motion>', self.on_touch_move) 
        self.label.bind('<ButtonRelease-1>', self.on_touch_end)
        
        # Right Click (2 Finger equivalent)
        self.label.bind('<Button-3>', self.on_right_click_down)
        self.label.bind('<ButtonRelease-3>', self.on_right_click_up)
        # Mouse Move
        self.label.bind('<Motion>', self.on_mouse_move)

        # Start Video Thread
        self.is_running = True
        self.thread = threading.Thread(target=self.video_loop)
        self.thread.daemon = True
        self.thread.start()

    def video_loop(self):
        # OpenCV Video Capture from UDP
        # Using udp://@:port listens on all interfaces
        video_url = f"udp://@:{STREAM_PORT}?overrun_nonfatal=1&fifo_size=500000"
        
        print(f"Connecting to video stream: {video_url}")
        cap = cv2.VideoCapture(video_url)

        # Optimize buffer for low latency
        cap.set(cv2.CAP_PROP_BUFFERSIZE, 0) 

        while self.is_running:
            ret, frame = cap.read()
            if ret:
                # Convert BGR (OpenCV) to RGB (PIL/Tkinter)
                cv2image = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
                img = Image.fromarray(cv2image)
                
                # Resize to fit window (optional, can be slow on weak phones)
                # For best performance, try to match window size to stream size
                win_w = self.root.winfo_width()
                win_h = self.root.winfo_height()
                if win_w > 10 and win_h > 10:
                     img = img.resize((win_w, win_h), Image.NEAREST)

                imgtk = ImageTk.PhotoImage(image=img)
                
                # Update UI thread-safely
                self.label.after(0, self.update_image, imgtk)
            else:
                # If no frame, wait a tiny bit to not burn CPU
                cv2.waitKey(10)

        cap.release()

    def update_image(self, imgtk):
        self.label.configure(image=imgtk)
        self.label.image = imgtk # Keep reference

    # --- INPUT SENDING ---
    def send_input(self, type_id, x, y, key):
        win_w = self.label.winfo_width()
        win_h = self.label.winfo_height()
        if win_w == 0 or win_h == 0: return

        # Scale to match the Host's expected resolution (set in host C++ as g_streamW)
        scaled_x = int((x / win_w) * HOST_WIDTH)
        scaled_y = int((y / win_h) * HOST_HEIGHT)

        packet = struct.pack('iiii', type_id, scaled_x, scaled_y, key)
        try: self.sock.sendto(packet, self.host_address)
        except: pass

    # --- EVENTS ---
    def on_touch_start(self, e):
        self.touch_start_x = e.x; self.touch_start_y = e.y
        self.send_input(1, e.x, e.y, 0) # Hover

    def on_touch_move(self, e):
        self.send_input(1, e.x, e.y, 0) # Hover

    def on_touch_end(self, e):
        dx = abs(e.x - self.touch_start_x)
        dy = abs(e.y - self.touch_start_y)
        if dx < 10 and dy < 10: # Tap
            self.send_input(2, e.x, e.y, 0)
            self.send_input(3, e.x, e.y, 0)

    def on_mouse_move(self, e): self.send_input(1, e.x, e.y, 0)
    def on_right_click_down(self, e): self.send_input(4, e.x, e.y, 0)
    def on_right_click_up(self, e): self.send_input(5, e.x, e.y, 0)

    def on_close(self):
        self.is_running = False
        self.root.destroy()
        sys.exit(0)

if __name__ == "__main__":
    root = tk.Tk()
    root.withdraw()
    ip = simpledialog.askstring("Connect", "Enter Host Tailscale IP:")
    if ip:
        root.deiconify()
        app = RemoteScreenApp(root, ip)
        root.protocol("WM_DELETE_WINDOW", app.on_close)
        root.mainloop()
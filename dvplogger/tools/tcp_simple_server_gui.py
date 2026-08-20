#!/usr/bin/env python3
import argparse
import queue
import re
import socket
import threading
from datetime import datetime
import tkinter as tk
from tkinter import ttk

ANTENNA_NAMES = {
    0: "None",
    1: "Triband dipole",
    2: "Tribander",
    3: "50MHz 2el",
    4: "144/430/1200 GP",
    5: "2.4GHz antenna",
    6: "5.6GHz antenna",
    7: "Second triband DP",
    8: "Antenna 8",
    9: "Antenna 9",
}

def timestamp():
    return datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]

class AntennaServer:
    def __init__(self, host, port, event_queue):
        self.host = host
        self.port = port
        self.q = event_queue
        self.stop_event = threading.Event()
        self.server_socket = None

    def emit(self, kind, **kwargs):
        self.q.put((kind, kwargs))

    def handle_client(self, conn, address):
        self.emit("connected", address=address)
        buf = bytearray()
        try:
            while not self.stop_event.is_set():
                data = conn.recv(1024)
                if not data:
                    break
                self.emit("raw", data=data)
                buf.extend(data)
                while b"\r" in buf:
                    line, _, rest = buf.partition(b"\r")
                    buf = bytearray(rest)
                    command = line.decode("ascii", errors="replace").strip()
                    if not command:
                        continue
                    self.emit("command", command=command)

                    m = re.fullmatch(r"AUX([12])\s+(\d+)", command, re.I)
                    if m:
                        aux = int(m.group(1))
                        ant = int(m.group(2))
                        self.emit("antenna", radio=aux - 1, antenna=ant)

                    response = f"OK {command}\r"
                    conn.sendall(response.encode("ascii"))
                    self.emit("tx", response=response)
        except (ConnectionResetError, OSError) as exc:
            if not self.stop_event.is_set():
                self.emit("log", text=f"[{timestamp()}] Socket: {exc}")
        finally:
            try:
                conn.close()
            except OSError:
                pass
            self.emit("disconnected", address=address)

    def run(self):
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
                self.server_socket = s
                s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                s.bind((self.host, self.port))
                s.listen(5)
                s.settimeout(0.5)
                self.emit("listening")
                while not self.stop_event.is_set():
                    try:
                        conn, addr = s.accept()
                    except socket.timeout:
                        continue
                    except OSError:
                        break
                    threading.Thread(
                        target=self.handle_client,
                        args=(conn, addr),
                        daemon=True
                    ).start()
        except OSError as exc:
            self.emit("fatal", text=str(exc))

    def stop(self):
        self.stop_event.set()
        if self.server_socket:
            try:
                self.server_socket.close()
            except OSError:
                pass

class App:
    def __init__(self, root, host, port):
        self.root = root
        self.root.title("DVPlogger Antenna / OTRSP TCP Demo")
        self.root.geometry("760x500")

        self.q = queue.Queue()
        self.server = AntennaServer(host, port, self.q)

        self.connection = tk.StringVar(value="Disconnected")
        self.radio = [
            tk.StringVar(value="0  —  None"),
            tk.StringVar(value="0  —  None"),
        ]

        outer = ttk.Frame(root, padding=12)
        outer.pack(fill="both", expand=True)

        status = ttk.LabelFrame(outer, text="Server status", padding=10)
        status.pack(fill="x")
        ttk.Label(status, text=f"Listening: {host}:{port}").pack(side="left")
        ttk.Label(status, textvariable=self.connection).pack(side="right")

        ants = ttk.LabelFrame(outer, text="Current antenna selection", padding=12)
        ants.pack(fill="x", pady=(12, 0))
        for i in range(2):
            row = ttk.Frame(ants)
            row.pack(fill="x", pady=8)
            ttk.Label(row, text=f"Radio {i}", width=12,
                      font=("TkDefaultFont", 12, "bold")).pack(side="left")
            ttk.Label(row, textvariable=self.radio[i],
                      font=("TkDefaultFont", 16)).pack(side="left")

        legend = ttk.LabelFrame(outer, text="Antenna IDs", padding=10)
        legend.pack(fill="x", pady=(12, 0))
        text = "   ".join(f"{k}: {v}" for k, v in ANTENNA_NAMES.items() if k)
        ttk.Label(legend, text=text, wraplength=710, justify="left").pack(anchor="w")

        lf = ttk.LabelFrame(outer, text="TCP / OTRSP log", padding=8)
        lf.pack(fill="both", expand=True, pady=(12, 0))
        self.log = tk.Text(lf, wrap="none")
        self.log.pack(side="left", fill="both", expand=True)
        sb = ttk.Scrollbar(lf, orient="vertical", command=self.log.yview)
        sb.pack(side="right", fill="y")
        self.log.configure(yscrollcommand=sb.set)

        threading.Thread(target=self.server.run, daemon=True).start()
        self.root.after(50, self.poll)
        self.root.protocol("WM_DELETE_WINDOW", self.close)

    def append(self, text):
        self.log.insert("end", text.rstrip() + "\n")
        self.log.see("end")

    def poll(self):
        try:
            while True:
                kind, d = self.q.get_nowait()
                if kind == "listening":
                    self.append(f"[{timestamp()}] Listening")
                elif kind == "connected":
                    a = d["address"]
                    self.connection.set(f"Connected: {a[0]}:{a[1]}")
                    self.append(f"[{timestamp()}] Connected: {a[0]}:{a[1]}")
                elif kind == "disconnected":
                    self.connection.set("Disconnected")
                    a = d["address"]
                    self.append(f"[{timestamp()}] Disconnected: {a[0]}:{a[1]}")
                elif kind == "raw":
                    self.append(f"[{timestamp()}] RX raw: {d['data']!r}")
                elif kind == "command":
                    self.append(f"[{timestamp()}] COMMAND: {d['command']}")
                elif kind == "tx":
                    self.append(f"[{timestamp()}] TX: {d['response']!r}")
                elif kind == "antenna":
                    r, a = d["radio"], d["antenna"]
                    name = ANTENNA_NAMES.get(a, f"Antenna {a}")
                    if 0 <= r < 2:
                        self.radio[r].set(f"{a}  —  {name}")
                elif kind == "log":
                    self.append(d["text"])
                elif kind == "fatal":
                    self.connection.set("Server error")
                    self.append(f"[{timestamp()}] FATAL: {d['text']}")
        except queue.Empty:
            pass
        self.root.after(50, self.poll)

    def close(self):
        self.server.stop()
        self.root.destroy()

def main():
    ap = argparse.ArgumentParser(description="GUI DVPlogger antenna/OTRSP TCP demo server")
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=12001)
    args = ap.parse_args()
    root = tk.Tk()
    App(root, args.host, args.port)
    root.mainloop()

if __name__ == "__main__":
    main()

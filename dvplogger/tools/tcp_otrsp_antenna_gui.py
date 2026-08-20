#!/usr/bin/env python3
import argparse
import json
import queue
import re
import socket
import threading
from datetime import datetime
from pathlib import Path
import tkinter as tk
from tkinter import ttk, messagebox


DEFAULT_ANTENNA_NAMES = {
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


def format_freq(freq_hz):
    if freq_hz <= 0:
        return "-"
    if freq_hz >= 1_000_000:
        return f"{freq_hz / 1_000_000:.6f} MHz"
    if freq_hz >= 1_000:
        return f"{freq_hz / 1_000:.3f} kHz"
    return f"{freq_hz} Hz"


class AntennaModel:
    def __init__(self, config_path):
        self.lock = threading.Lock()
        self.config_path = Path(config_path)
        self.antenna_names = dict(DEFAULT_ANTENNA_NAMES)
        self.radio = [
            {"freq": 0, "mode": "-", "antenna": 0},
            {"freq": 0, "mode": "-", "antenna": 0},
        ]
        self.load_names()

    def load_names(self):
        if not self.config_path.exists():
            return
        try:
            data = json.loads(self.config_path.read_text())
            names = data.get("antenna_names", {})
            with self.lock:
                for key, value in names.items():
                    ant = int(key)
                    if 1 <= ant <= 9 and isinstance(value, str):
                        self.antenna_names[ant] = value
        except Exception:
            pass

    def save_names(self, names):
        with self.lock:
            self.antenna_names = dict(names)
            data = {
                "antenna_names": {
                    str(k): v for k, v in sorted(self.antenna_names.items())
                }
            }
        self.config_path.write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n")

    def snapshot_names(self):
        with self.lock:
            return dict(self.antenna_names)

    def set_radio_freq(self, radio, freq):
        with self.lock:
            self.radio[radio]["freq"] = freq

    def set_radio_mode(self, radio, mode):
        with self.lock:
            self.radio[radio]["mode"] = mode

    def set_radio_antenna(self, radio, antenna):
        with self.lock:
            self.radio[radio]["antenna"] = antenna

    def snapshot_radio(self):
        with self.lock:
            return [dict(x) for x in self.radio]


class OTRSPServer:
    def __init__(self, host, port, model, event_queue):
        self.host = host
        self.port = port
        self.model = model
        self.q = event_queue
        self.stop_event = threading.Event()
        self.server_socket = None

    def emit(self, kind, **kwargs):
        self.q.put((kind, kwargs))

    @staticmethod
    def send_line(conn, line):
        conn.sendall((line + "\r").encode("utf-8"))

    def send_antlist(self, conn):
        names = self.model.snapshot_names()
        for ant in sorted(names):
            self.send_line(conn, f"ANTNAME {ant} {names[ant]}")
        self.send_line(conn, "ANTLIST END")
        self.emit("log", text=f"[{timestamp()}] TX: ANTLIST ({len(names)} names)")

    def process_command(self, conn, command):
        self.emit("command", command=command)

        if command.upper() == "?ANTLIST":
            self.send_antlist(conn)
            return

        m = re.fullmatch(r"AUX([12])\s+(\d+)", command, re.I)
        if m:
            radio = int(m.group(1)) - 1
            antenna = int(m.group(2))
            self.model.set_radio_antenna(radio, antenna)
            self.emit("state_changed")
            self.send_line(conn, f"OK {command}")
            return

        m = re.fullmatch(r"RADIO([01])\s+FREQ\s+(\d+)", command, re.I)
        if m:
            radio = int(m.group(1))
            freq = int(m.group(2))
            self.model.set_radio_freq(radio, freq)
            self.emit("state_changed")
            self.send_line(conn, f"OK {command}")
            return

        m = re.fullmatch(r"RADIO([01])\s+MODE\s+(\S+)", command, re.I)
        if m:
            radio = int(m.group(1))
            mode = m.group(2)
            self.model.set_radio_mode(radio, mode)
            self.emit("state_changed")
            self.send_line(conn, f"OK {command}")
            return

        # Keep demo compatibility with arbitrary OTRSP/test commands.
        self.send_line(conn, f"OK {command}")

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
                    command = line.decode("utf-8", errors="replace").strip()
                    if command:
                        self.process_command(conn, command)

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
                self.emit("listening", host=self.host, port=self.port)

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
                        daemon=True,
                    ).start()

        except OSError as exc:
            self.emit("fatal", text=str(exc))

    def stop(self):
        self.stop_event.set()
        if self.server_socket is not None:
            try:
                self.server_socket.close()
            except OSError:
                pass


class App:
    def __init__(self, root, host, port, config_path):
        self.root = root
        self.root.title("DVPlogger OTRSP Antenna Server")
        self.root.geometry("900x720")

        self.q = queue.Queue()
        self.model = AntennaModel(config_path)
        self.server = OTRSPServer(host, port, self.model, self.q)

        self.connection_var = tk.StringVar(value="Disconnected")
        self.radio_title = [tk.StringVar(), tk.StringVar()]
        self.radio_detail = [tk.StringVar(), tk.StringVar()]
        self.name_vars = {
            ant: tk.StringVar(value=name)
            for ant, name in self.model.snapshot_names().items()
        }

        self.build_ui(host, port)
        self.refresh_radio_display()

        threading.Thread(target=self.server.run, daemon=True).start()
        self.root.after(50, self.poll)
        self.root.protocol("WM_DELETE_WINDOW", self.close)

    def build_ui(self, host, port):
        outer = ttk.Frame(self.root, padding=12)
        outer.pack(fill="both", expand=True)

        status = ttk.LabelFrame(outer, text="Server status", padding=10)
        status.pack(fill="x")
        ttk.Label(status, text=f"Listening: {host}:{port}").pack(side="left")
        ttk.Label(status, textvariable=self.connection_var).pack(side="right")

        rf = ttk.LabelFrame(outer, text="Radio / antenna assignment", padding=12)
        rf.pack(fill="x", pady=(12, 0))

        for i in range(2):
            frame = ttk.Frame(rf, padding=8)
            frame.pack(fill="x", pady=4)
            ttk.Label(
                frame,
                textvariable=self.radio_title[i],
                width=28,
                font=("TkDefaultFont", 15, "bold"),
            ).pack(side="left")
            ttk.Label(
                frame,
                textvariable=self.radio_detail[i],
                font=("TkDefaultFont", 13),
            ).pack(side="left", padx=(10, 0))

        names = ttk.LabelFrame(
            outer,
            text="Antenna definitions (server is master)",
            padding=10,
        )
        names.pack(fill="x", pady=(12, 0))

        for ant in range(1, 10):
            row = (ant - 1) // 3
            col = ((ant - 1) % 3) * 2
            ttk.Label(names, text=f"{ant}:").grid(
                row=row, column=col, sticky="e", padx=(4, 2), pady=3
            )
            ttk.Entry(
                names,
                textvariable=self.name_vars[ant],
                width=24,
            ).grid(row=row, column=col + 1, sticky="ew", padx=(0, 10), pady=3)

        for col in (1, 3, 5):
            names.columnconfigure(col, weight=1)

        ttk.Button(
            names,
            text="Save antenna names",
            command=self.save_names,
        ).grid(row=3, column=0, columnspan=6, pady=(8, 0))

        lf = ttk.LabelFrame(outer, text="TCP / OTRSP log", padding=8)
        lf.pack(fill="both", expand=True, pady=(12, 0))

        self.log = tk.Text(lf, wrap="none", height=18)
        self.log.pack(side="left", fill="both", expand=True)
        sb = ttk.Scrollbar(lf, orient="vertical", command=self.log.yview)
        sb.pack(side="right", fill="y")
        self.log.configure(yscrollcommand=sb.set)

    def append(self, text):
        self.log.insert("end", text.rstrip() + "\n")
        self.log.see("end")

    def save_names(self):
        names = {
            ant: self.name_vars[ant].get().strip() or f"Antenna {ant}"
            for ant in range(1, 10)
        }
        try:
            self.model.save_names(names)
            self.append(f"[{timestamp()}] Saved antenna names")
            self.refresh_radio_display()
        except OSError as exc:
            messagebox.showerror("Save failed", str(exc))

    def refresh_radio_display(self):
        radios = self.model.snapshot_radio()
        names = self.model.snapshot_names()

        for i, r in enumerate(radios):
            ant = r["antenna"]
            ant_name = "None" if ant == 0 else names.get(ant, f"Antenna {ant}")
            self.radio_title[i].set(
                f"Radio {i}: {format_freq(r['freq'])}  {r['mode']}"
            )
            self.radio_detail[i].set(
                f"→ ANT {ant}: {ant_name}"
            )

    def poll(self):
        try:
            while True:
                kind, d = self.q.get_nowait()

                if kind == "listening":
                    self.append(
                        f"[{timestamp()}] Listening on {d['host']}:{d['port']}"
                    )
                elif kind == "connected":
                    a = d["address"]
                    self.connection_var.set(f"Connected: {a[0]}:{a[1]}")
                    self.append(f"[{timestamp()}] Connected: {a[0]}:{a[1]}")
                elif kind == "disconnected":
                    a = d["address"]
                    self.connection_var.set("Disconnected")
                    self.append(f"[{timestamp()}] Disconnected: {a[0]}:{a[1]}")
                elif kind == "raw":
                    self.append(f"[{timestamp()}] RX raw: {d['data']!r}")
                elif kind == "command":
                    self.append(f"[{timestamp()}] COMMAND: {d['command']}")
                elif kind == "state_changed":
                    self.refresh_radio_display()
                elif kind == "log":
                    self.append(d["text"])
                elif kind == "fatal":
                    self.connection_var.set("Server error")
                    self.append(f"[{timestamp()}] FATAL: {d['text']}")

        except queue.Empty:
            pass

        self.root.after(50, self.poll)

    def close(self):
        self.server.stop()
        self.root.destroy()


def main():
    ap = argparse.ArgumentParser(
        description="GUI OTRSP antenna server with DVPlogger extensions"
    )
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=12001)
    ap.add_argument(
        "--config",
        default="antenna_names.json",
        help="antenna-name JSON file (default: antenna_names.json)",
    )
    args = ap.parse_args()

    root = tk.Tk()
    App(root, args.host, args.port, args.config)
    root.mainloop()


if __name__ == "__main__":
    main()

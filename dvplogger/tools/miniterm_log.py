#!/usr/bin/env python3
import argparse, sys, serial
from serial.tools.miniterm import Miniterm

class LoggingMiniterm(Miniterm):
    def __init__(self, serial_instance, logfile, **kwargs):
        super().__init__(serial_instance, **kwargs)
        self._log = open(logfile, "ab", buffering=0)

    def reader(self):
        try:
            while self.alive and self._reader_alive:
                data = self.serial.read(self.serial.in_waiting or 1)
                if data:
                    self._log.write(data)
                    if self.raw:
                        self.console.write_bytes(data)
                    else:
                        text = self.rx_decoder.decode(data)
                        for transformation in self.rx_transformations:
                            text = transformation.rx(text)
                        self.console.write(text)
        except serial.SerialException:
            self.alive = False
            self.console.cancel()
            raise

    def close_log(self):
        if self._log:
            self._log.close()
            self._log = None

def main():
    ap = argparse.ArgumentParser(description="pySerial miniterm with RX logging")
    ap.add_argument("port")
    ap.add_argument("baudrate", nargs="?", type=int, default=115200)
    ap.add_argument("--logfile", required=True)
    ap.add_argument("--encoding", default="UTF-8")
    ap.add_argument("--eol", choices=("CR","LF","CRLF"), default="CRLF")
    ap.add_argument("--rtscts", action="store_true")
    ap.add_argument("--xonxoff", action="store_true")
    ap.add_argument("--raw", action="store_true")
    a=ap.parse_args()

    try:
        ser=serial.serial_for_url(a.port, baudrate=a.baudrate,
            parity=serial.PARITY_NONE, rtscts=a.rtscts,
            xonxoff=a.xonxoff, timeout=1)
    except serial.SerialException as e:
        print(f"Could not open port {a.port}: {e}", file=sys.stderr)
        return 1

    term=LoggingMiniterm(ser, a.logfile, echo=False,
                         eol=a.eol.lower(), filters=("default",))
    term.raw=a.raw
    term.set_rx_encoding(a.encoding)
    term.set_tx_encoding(a.encoding)
    print(f"--- Miniterm on {a.port} {a.baudrate},8,N,1 ---")
    print(f"--- Logging RX to: {a.logfile} ---")
    print("--- Quit: Ctrl+] | Menu: Ctrl+T ---")
    try:
        term.start()
        term.join(True)
    except KeyboardInterrupt:
        pass
    finally:
        term.stop()
        term.close_log()
        ser.close()
    return 0

if __name__ == "__main__":
    raise SystemExit(main())

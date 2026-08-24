#!/usr/bin/env python3
"""Small Z-Server-compatible development server for DVPlogger.

This is intentionally a development emulator, not a complete replacement for
all zLog/Z-Server functions.  It implements the pieces useful for DVPlogger
bring-up and protocol debugging:

* TCP server with configurable listen address/port
* CRLF-delimited Z-Server command framing
* CP932 (Shift-JIS/Windows-31J) decoding/encoding
* Hex dump option for exact byte-level inspection
* PUTMESSAGE relay to other connected clients
* Interactive terminal commands to inject PUTMESSAGE/raw ZLOG lines
* Optional relay of every received line to other clients

Examples:
  python3 zserver_test.py --port 2323
  python3 zserver_test.py --host 0.0.0.0 --port 2323 --hex

Then set DVPlogger zserver_name to, for example:
  192.168.1.100:2323
"""

from __future__ import annotations

import argparse
import asyncio
import itertools
import signal
from dataclasses import dataclass
from datetime import datetime
from typing import Dict, Optional

CRLF = b"\r\n"
PUTMESSAGE_PREFIX = "#ZLOG# PUTMESSAGE "


def ts() -> str:
    return datetime.now().strftime("%H:%M:%S.%f")[:-3]


def hex_bytes(data: bytes) -> str:
    return " ".join(f"{b:02X}" for b in data)


def encode_cp932(line: str) -> bytes:
    # Replace characters outside CP932 rather than aborting the test server.
    return line.encode("cp932", errors="replace") + CRLF


def decode_cp932(data: bytes) -> str:
    return data.decode("cp932", errors="replace")


@dataclass
class Client:
    cid: int
    reader: asyncio.StreamReader
    writer: asyncio.StreamWriter
    peer: str


class ZServerEmulator:
    def __init__(self, show_hex: bool = False, relay_all: bool = False,
                 echo_sender: bool = False) -> None:
        self.show_hex = show_hex
        self.relay_all = relay_all
        self.echo_sender = echo_sender
        self.clients: Dict[int, Client] = {}
        self._ids = itertools.count(1)
        self.server: Optional[asyncio.AbstractServer] = None
        self.stop_event = asyncio.Event()

    def log(self, text: str) -> None:
        print(f"[{ts()}] {text}", flush=True)

    async def start(self, host: str, port: int) -> None:
        self.server = await asyncio.start_server(self.handle_client, host, port)
        addresses = ", ".join(str(sock.getsockname()) for sock in self.server.sockets or [])
        self.log(f"Z-Server emulator listening on {addresses}")
        self.log("Wire encoding: CP932, line ending: CRLF")
        self.log("Commands: /msg TEXT, /send LINE, /clients, /help, /quit")

    async def close(self) -> None:
        if self.server is not None:
            self.server.close()
            await self.server.wait_closed()
        for client in list(self.clients.values()):
            try:
                client.writer.close()
                await client.writer.wait_closed()
            except Exception:
                pass
        self.clients.clear()

    async def handle_client(self, reader: asyncio.StreamReader,
                            writer: asyncio.StreamWriter) -> None:
        cid = next(self._ids)
        peername = writer.get_extra_info("peername")
        peer = str(peername)
        client = Client(cid, reader, writer, peer)
        self.clients[cid] = client
        self.log(f"client {cid} connected from {peer}")

        buf = bytearray()
        try:
            while True:
                chunk = await reader.read(4096)
                if not chunk:
                    break
                buf.extend(chunk)
                while True:
                    pos = buf.find(CRLF)
                    if pos < 0:
                        break
                    raw = bytes(buf[:pos])
                    del buf[:pos + 2]
                    await self.process_line(client, raw)
        except (ConnectionError, asyncio.CancelledError):
            pass
        finally:
            self.clients.pop(cid, None)
            try:
                writer.close()
                await writer.wait_closed()
            except Exception:
                pass
            self.log(f"client {cid} disconnected")

    async def process_line(self, client: Client, raw: bytes) -> None:
        text = decode_cp932(raw)
        self.log(f"RX C{client.cid}: {text}")
        if self.show_hex:
            self.log(f"RX C{client.cid} HEX: {hex_bytes(raw + CRLF)}")

        # The documented chat command is bidirectional.  Z-Server distributes
        # a client's PUTMESSAGE to the other zLog clients.
        if text.startswith(PUTMESSAGE_PREFIX):
            await self.broadcast_line(text, exclude=None if self.echo_sender else client.cid)
        elif self.relay_all and text.startswith("#ZLOG# "):
            await self.broadcast_line(text, exclude=None if self.echo_sender else client.cid)

    async def send_line(self, client: Client, line: str) -> bool:
        if client.writer.is_closing():
            return False
        payload = encode_cp932(line)
        try:
            client.writer.write(payload)
            await client.writer.drain()
            self.log(f"TX C{client.cid}: {line}")
            if self.show_hex:
                self.log(f"TX C{client.cid} HEX: {hex_bytes(payload)}")
            return True
        except ConnectionError:
            return False

    async def broadcast_line(self, line: str, exclude: Optional[int] = None) -> None:
        targets = [c for cid, c in self.clients.items() if cid != exclude]
        if not targets:
            self.log("TX: no target clients")
            return
        for client in targets:
            await self.send_line(client, line)

    async def console(self) -> None:
        while not self.stop_event.is_set():
            try:
                line = await asyncio.to_thread(input, "zserver> ")
            except (EOFError, KeyboardInterrupt):
                self.stop_event.set()
                return

            line = line.strip()
            if not line:
                continue
            if line in ("/quit", "/exit"):
                self.stop_event.set()
                return
            if line == "/clients":
                if not self.clients:
                    self.log("no clients")
                else:
                    for c in self.clients.values():
                        self.log(f"client {c.cid}: {c.peer}")
                continue
            if line == "/help":
                print(
                    "/msg TEXT     send '#ZLOG# PUTMESSAGE TEXT' to all clients\n"
                    "/send LINE    send LINE verbatim (CP932 + CRLF) to all clients\n"
                    "/clients      list connected clients\n"
                    "/quit         stop server",
                    flush=True,
                )
                continue
            if line.startswith("/msg "):
                await self.broadcast_line(PUTMESSAGE_PREFIX + line[5:])
                continue
            if line.startswith("/send "):
                await self.broadcast_line(line[6:])
                continue

            # Bare terminal text is convenient shorthand for chat.
            await self.broadcast_line(PUTMESSAGE_PREFIX + line)


async def amain() -> int:
    parser = argparse.ArgumentParser(description="DVPlogger Z-Server development emulator")
    parser.add_argument("--host", default="0.0.0.0", help="listen address (default: 0.0.0.0)")
    parser.add_argument("--port", type=int, default=2323,
                        help="listen TCP port (default: 2323; use 23 only with privileges)")
    parser.add_argument("--hex", action="store_true", help="show raw RX/TX bytes in hex")
    parser.add_argument("--relay-all", action="store_true",
                        help="relay all '#ZLOG# ...' lines to other clients, not only PUTMESSAGE")
    parser.add_argument("--echo-sender", action="store_true",
                        help="include sender when relaying client messages")
    args = parser.parse_args()

    if not (1 <= args.port <= 65535):
        parser.error("--port must be 1..65535")

    z = ZServerEmulator(show_hex=args.hex,
                        relay_all=args.relay_all,
                        echo_sender=args.echo_sender)
    loop = asyncio.get_running_loop()
    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(sig, z.stop_event.set)
        except NotImplementedError:
            pass

    try:
        await z.start(args.host, args.port)
    except PermissionError:
        print(
            f"Permission denied opening TCP/{args.port}. "
            "On Linux use an unprivileged port such as 2323, or grant privileges.",
            flush=True,
        )
        return 1

    console_task = asyncio.create_task(z.console())
    try:
        await z.stop_event.wait()
    finally:
        console_task.cancel()
        await z.close()
    return 0


def main() -> None:
    raise SystemExit(asyncio.run(amain()))


if __name__ == "__main__":
    main()

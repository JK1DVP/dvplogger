#!/usr/bin/env python3
import argparse
import socket
import threading
from datetime import datetime


def timestamp() -> str:
    return datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]


def handle_client(conn: socket.socket, address: tuple[str, int]) -> None:
    print(f"[{timestamp()}] Connected: {address[0]}:{address[1]}")

    buffer = bytearray()

    try:
        while True:
            data = conn.recv(1024)
            if not data:
                break

            print(
                f"[{timestamp()}] RX raw: "
                f"{data!r}"
            )

            buffer.extend(data)

            # DVPlogger側が \r 終端で送る想定
            while b"\r" in buffer:
                line, _, remainder = buffer.partition(b"\r")
                buffer = bytearray(remainder)

                command = line.decode("ascii", errors="replace").strip()
                if not command:
                    continue

                print(f"[{timestamp()}] COMMAND: {command}")

                # 状態応答を試す場合の例。
                # 現在のDVPloggerが応答を期待していなければ、
                # この部分はコメントアウトして構いません。
                response = f"OK {command}\r"
                conn.sendall(response.encode("ascii"))
                print(f"[{timestamp()}] TX: {response!r}")

    except ConnectionResetError:
        print(f"[{timestamp()}] Connection reset")
    except OSError as exc:
        print(f"[{timestamp()}] Socket error: {exc}")
    finally:
        conn.close()
        print(f"[{timestamp()}] Disconnected: {address[0]}:{address[1]}")


def main() -> None:
    parser = argparse.ArgumentParser(
        description="DVPlogger antenna/OTRSP TCP test server"
    )
    parser.add_argument(
        "--host",
        default="0.0.0.0",
        help="listen address (default: 0.0.0.0)",
    )
    parser.add_argument(
        "--port",
        type=int,
        default=12001,
        help="listen TCP port (default: 12001)",
    )
    args = parser.parse_args()

    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server:
        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server.bind((args.host, args.port))
        server.listen(5)

        print(
            f"[{timestamp()}] Listening on "
            f"{args.host}:{args.port}"
        )

        while True:
            conn, address = server.accept()
            thread = threading.Thread(
                target=handle_client,
                args=(conn, address),
                daemon=True,
            )
            thread.start()


if __name__ == "__main__":
    main()

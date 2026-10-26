#!/usr/bin/env python3
"""Small UDP echo peer for pingudp's 16-byte UPNG probes."""

import argparse
import socket


def main() -> None:
    parser = argparse.ArgumentParser(description="UDP echo peer for pingudp")
    parser.add_argument("--host", default="0.0.0.0", help="IPv4 bind address")
    parser.add_argument("--port", type=int, default=9000, help="UDP port (default: 9000)")
    args = parser.parse_args()
    if not 1 <= args.port <= 65535:
        parser.error("port must be between 1 and 65535")

    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind((args.host, args.port))
        print(f"UDP echo listening on {args.host}:{args.port}", flush=True)
        while True:
            data, address = sock.recvfrom(64)
            if len(data) == 16 and data.startswith(b"UPNG"):
                sock.sendto(data, address)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""
Simple WebSocket echo server for RPS proxy testing.
Usage: python3 echo_ws.py [port]
"""
import sys
import struct
import hashlib
import base64
import socket
import select
import time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 9091
WS_MAGIC = b'258EAFA5-E914-47DA-95CA-C5AB0DC85B11'
IDLE_TIMEOUT = 30

# ── WebSocket frame helpers ──


def ws_accept_key(client_key: str) -> str:
    sha1 = hashlib.sha1(client_key.encode() + WS_MAGIC).digest()
    return base64.b64encode(sha1).decode()


def ws_send(sock: socket.socket, opcode: int, payload: bytes) -> None:
    """Send a WebSocket frame (unmasked, server→client)."""
    header = bytes([0x80 | opcode])  # FIN=1

    length = len(payload)
    if length < 126:
        header += bytes([length])
    elif length < 65536:
        header += bytes([126]) + struct.pack('>H', length)
    else:
        header += bytes([127]) + struct.pack('>Q', length)

    sock.sendall(header + payload)


def ws_recv(sock: socket.socket) -> tuple:
    """Receive one WebSocket frame. Returns (opcode, payload) or (None, None) on error."""
    try:
        b1 = sock.recv(1)
        if not b1:
            return None, None
        b1 = b1[0]
    except Exception:
        return None, None

    opcode = b1 & 0x0F

    try:
        b2 = sock.recv(1)[0]
    except Exception:
        return None, None

    masked = (b2 & 0x80) != 0
    length = b2 & 0x7F

    if length == 126:
        length = struct.unpack('>H', sock.recv(2))[0]
    elif length == 127:
        length = struct.unpack('>Q', sock.recv(8))[0]

    mask_key = sock.recv(4) if masked else b''

    payload = b''
    while len(payload) < length:
        chunk = sock.recv(length - len(payload))
        if not chunk:
            return None, None
        payload += chunk

    if masked:
        payload = bytes(b ^ mask_key[i % 4] for i, b in enumerate(payload))

    return opcode, payload


# ── Server ──


def main():
    listen_fd = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listen_fd.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listen_fd.bind(('127.0.0.1', PORT))
    listen_fd.listen(16)
    listen_fd.setblocking(False)

    print(f'WebSocket echo server on :{PORT}')

    clients = {}  # fd -> (socket, last_active, state, buf)

    while True:
        rlist = [listen_fd] + list(clients.keys())
        r, _, _ = select.select(rlist, [], [], 1.0)

        now = time.time()

        for fd in r:
            if fd == listen_fd:
                conn, addr = listen_fd.accept()
                conn.setblocking(False)
                clients[conn] = (conn, now, 'handshake', b'')
                print(f'new WS connection fd={conn.fileno()} from {addr} '
                      f'({len(clients)} total)')
                continue

            sock, last_active, state, buf = clients.get(fd, (None, 0, '', b''))
            if sock is None:
                continue

            if state == 'handshake':
                try:
                    data = sock.recv(65536)
                except Exception:
                    data = b''
                if not data:
                    print(f'[close] fd={fd} reason=client_close (handshake)')
                    sock.close()
                    del clients[fd]
                    continue

                buf += data
                if b'\r\n\r\n' not in buf:
                    clients[fd] = (sock, now, state, buf)
                    continue

                # Parse handshake
                hdr_text = buf.split(b'\r\n\r\n')[0].decode(errors='replace')
                ws_key = ''
                for line in hdr_text.split('\r\n'):
                    if line.lower().startswith('sec-websocket-key:'):
                        ws_key = line.split(':', 1)[1].strip()
                        break

                if not ws_key:
                    sock.sendall(b'HTTP/1.1 400 Bad Request\r\n\r\n')
                    sock.close()
                    del clients[fd]
                    continue

                accept = ws_accept_key(ws_key)
                resp = (
                    'HTTP/1.1 101 Switching Protocols\r\n'
                    'Upgrade: websocket\r\n'
                    'Connection: Upgrade\r\n'
                    f'Sec-WebSocket-Accept: {accept}\r\n'
                    '\r\n'
                )
                sock.sendall(resp.encode())
                print(f'WS handshake OK fd={fd}')

                clients[fd] = (sock, now, 'active', b'')
                continue

            # Active: receive WebSocket frames
            opcode, payload = ws_recv(sock)
            if opcode is None:
                print(f'[close] fd={fd} reason=client_close alive={now - last_active:.0f}s')
                sock.close()
                del clients[fd]
                continue

            clients[fd] = (sock, now, 'active', b'')

            if opcode == 0x8:  # Close
                print(f'[close] fd={fd} reason=ws_close')
                ws_send(sock, 0x8, payload[:2])  # Echo close frame
                sock.close()
                del clients[fd]
            elif opcode == 0x9:  # Ping
                ws_send(sock, 0xA, payload)  # Pong
                print(f'[ping/pong] fd={fd}')
            elif opcode in (0x1, 0x2):  # Text / Binary
                ws_send(sock, opcode, payload)  # Echo
                print(f'[echo] fd={fd} → {len(payload)} bytes '
                      f'({"text" if opcode == 1 else "bin"})')

        # Idle timeout
        for fd in list(clients.keys()):
            sock, last_active, _, _ = clients[fd]
            if now - last_active > IDLE_TIMEOUT:
                print(f'[close] fd={fd} reason=idle_timeout alive={now - last_active:.0f}s')
                sock.close()
                del clients[fd]


if __name__ == '__main__':
    main()

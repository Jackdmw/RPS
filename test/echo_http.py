#!/usr/bin/env python3
"""
Simple HTTP/1.1 keep-alive echo server for RPS proxy testing.
Usage: python3 echo_http.py [port]
"""
import sys
import socket
import select
import time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 9090
IDLE_TIMEOUT = 30


def handle_request(data: bytes) -> bytes:
    """Build a response with keep-alive support."""
    lines = data.decode(errors='replace').split('\r\n')
    request_line = lines[0] if lines else ''
    parts = request_line.split(' ')

    method = parts[0] if len(parts) > 0 else 'GET'
    path = parts[1] if len(parts) > 1 else '/'

    # Parse headers
    headers = {}
    for line in lines[1:]:
        if ':' in line:
            k, v = line.split(':', 1)
            headers[k.strip().lower()] = v.strip()

    connection_hdr = headers.get('connection', '')
    http_version = '1.1'
    want_keepalive = 'close' not in connection_hdr.lower()

    # Route
    if path == '/':
        body = 'Hello from backend'
    elif path == '/echo':
        body = data.decode(errors='replace')
    elif path == '/headers':
        body = '\r\n'.join(f'{k}: {v}' for k, v in headers.items())
    elif path == '/status/500':
        status = '500 Internal Server Error'
        body = 'Backend error'
        resp = (
            f'HTTP/1.1 {status}\r\n'
            f'Content-Type: text/plain\r\n'
            f'Content-Length: {len(body)}\r\n'
            f'Connection: {"keep-alive" if want_keepalive else "close"}\r\n'
            f'\r\n{body}'
        )
        return resp.encode()
    elif path == '/large':
        body = 'X' * 8191 + '\n'
    else:
        status = '404 Not Found'
        body = 'Not found'
        resp = (
            f'HTTP/1.1 {status}\r\n'
            f'Content-Type: text/plain\r\n'
            f'Content-Length: {len(body)}\r\n'
            f'Connection: {"keep-alive" if want_keepalive else "close"}\r\n'
            f'\r\n{body}'
        )
        return resp.encode()

    status = '200 OK'
    resp = (
        f'HTTP/1.1 {status}\r\n'
        f'Content-Type: text/plain\r\n'
        f'Content-Length: {len(body)}\r\n'
        f'Connection: {"keep-alive" if want_keepalive else "close"}\r\n'
        f'\r\n{body}'
    )
    return resp.encode()


def main():
    listen_fd = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listen_fd.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listen_fd.bind(('127.0.0.1', PORT))
    listen_fd.listen(16)
    listen_fd.setblocking(False)

    print(f'HTTP echo server (keep-alive) on :{PORT}')

    clients = {}  # fd -> (socket, last_active, buf)

    while True:
        rlist = [listen_fd] + list(clients.keys())
        r, _, _ = select.select(rlist, [], [], 1.0)

        now = time.time()

        for fd in r:
            if fd == listen_fd:
                conn, addr = listen_fd.accept()
                conn.setblocking(False)
                clients[conn] = (conn, now, b'')
                print(f'new connection fd={conn.fileno()} from {addr} ({len(clients)} total)')
                continue

            sock, last_active, buf = clients.get(fd, (None, 0, b''))
            if sock is None:
                continue

            try:
                data = sock.recv(65536)
            except Exception:
                data = b''

            if data:
                buf += data
                clients[fd] = (sock, now, buf)

                # Check for complete request
                if b'\r\n\r\n' in buf:
                    # Find end of headers
                    hdr_end = buf.find(b'\r\n\r\n')
                    # Check Content-Length
                    hdr_text = buf[:hdr_end].decode(errors='replace')
                    cl = 0
                    for line in hdr_text.split('\r\n'):
                        if line.lower().startswith('content-length:'):
                            cl = int(line.split(':', 1)[1].strip())

                    total_needed = hdr_end + 4 + cl
                    if len(buf) >= total_needed:
                        resp = handle_request(buf)
                        try:
                            sock.sendall(resp)
                        except Exception:
                            pass
                        print(f'[{hdr_text.split()[0][:8]}] {hdr_text.split()[1] if len(hdr_text.split())>1 else "/"} '
                              f'→ {len(resp)} bytes')

                        # Check if keep-alive
                        conn_hdr = ''
                        for line in hdr_text.split('\r\n'):
                            if line.lower().startswith('connection:'):
                                conn_hdr = line.split(':', 1)[1].strip().lower()
                        if 'close' in conn_hdr:
                            print(f'[close] fd={fd} reason=connection_close')
                            sock.close()
                            del clients[fd]
                        else:
                            clients[fd] = (sock, now, buf[total_needed:])
            else:
                # Client closed
                print(f'[close] fd={fd} reason=client_close alive={now - last_active:.0f}s')
                sock.close()
                del clients[fd]

        # Idle timeout
        for fd in list(clients.keys()):
            sock, last_active, _ = clients[fd]
            if now - last_active > IDLE_TIMEOUT:
                print(f'[close] fd={fd} reason=idle_timeout alive={now - last_active:.0f}s')
                sock.close()
                del clients[fd]


if __name__ == '__main__':
    main()

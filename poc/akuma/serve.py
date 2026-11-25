#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 unkernel contributors
"""Serves the site directory and bridges WebSockets to TCP, so a guest on kwemu in a browser can reach the network.

    poc/akuma/serve.py [SITE_DIR] [--port 8765] [--bind 127.0.0.1]

POST /dns relays one DNS query (wire format) to the machine's resolver; GET /tcp?host=H&port=N upgrades to a WebSocket and relays binary frames to and from a TCP connection to H:N. The page
opens one per guest connection (host.mjs `wsNet`); `?net=ws` on the page picks this proxy. It is an open TCP relay:
bind it to localhost (the default) unless you mean to share it.
"""
import argparse, asyncio, base64, hashlib, os, socket, struct, urllib.parse
from http import HTTPStatus

def resolver():
    """The machine's first nameserver, else 1.1.1.1."""
    try:
        for ln in open("/etc/resolv.conf"):
            f = ln.split()
            if len(f) == 2 and f[0] == "nameserver" and ":" not in f[1] and not f[1].startswith("127."):
                return f[1]
    except OSError:
        pass
    return "1.1.1.1"


def dns_query(wire):
    """One DNS query over UDP to the resolver; the raw answer (blocking: run it in an executor)."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.settimeout(3)
        s.sendto(wire, (resolver(), 53))
        return s.recv(4096)


GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
TYPES = {".wasm": "application/wasm", ".mjs": "text/javascript", ".js": "text/javascript", ".html": "text/html", ".tar": "application/x-tar"}


async def read_frame(r):
    """One WebSocket frame -> (opcode, payload); fragments are joined by the caller."""
    b = await r.readexactly(2)
    op, n = b[0] & 0x0F, b[1] & 0x7F
    if n == 126:
        n = struct.unpack(">H", await r.readexactly(2))[0]
    elif n == 127:
        n = struct.unpack(">Q", await r.readexactly(8))[0]
    mask = await r.readexactly(4) if b[1] & 0x80 else None
    data = await r.readexactly(n)
    if mask:
        data = bytes(c ^ mask[i & 3] for i, c in enumerate(data))
    return op, data


def frame(op, data):
    n = len(data)
    head = bytes([0x80 | op]) + (bytes([n]) if n < 126 else b"\x7e" + struct.pack(">H", n) if n < 65536 else b"\x7f" + struct.pack(">Q", n))
    return head + data


async def handle(r, w, root):
    try:
        line = await r.readline()
        method, target, _ = line.decode().split(" ", 2)
        headers = {}
        while (h := await r.readline()) not in (b"\r\n", b""):
            k, _, v = h.decode().partition(":")
            headers[k.strip().lower()] = v.strip()
        url = urllib.parse.urlsplit(target)
        if url.path == "/tcp" and headers.get("upgrade", "").lower() == "websocket":
            q = urllib.parse.parse_qs(url.query)
            try:
                tr, tw = await asyncio.open_connection(q["host"][0], int(q["port"][0]))
            except (OSError, KeyError) as e:
                w.write(b"HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\n\r\n")
                await w.drain()
                print(f"tcp {q.get('host')}:{q.get('port')}: {e}")
                return
            accept = base64.b64encode(hashlib.sha1(headers["sec-websocket-key"].encode() + GUID).digest()).decode()
            w.write(f"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: {accept}\r\n\r\n".encode())
            await w.drain()

            async def up():  # browser -> tcp
                while True:
                    op, data = await read_frame(r)
                    if op == 8:
                        break
                    if op in (1, 2):
                        tw.write(data)
                        await tw.drain()
                tw.close()

            async def down():  # tcp -> browser
                while data := await tr.read(65536):
                    w.write(frame(2, data))
                    await w.drain()
                w.write(frame(8, b""))
                await w.drain()

            done, pending = await asyncio.wait([asyncio.ensure_future(up()), asyncio.ensure_future(down())], return_when=asyncio.FIRST_COMPLETED)
            for p in pending:
                p.cancel()
            for d in done:
                if d.exception():
                    print(f"relay ended: {d.exception()!r}")
            tw.close()
            return
        if url.path == "/dns" and method == "POST":
            body = await r.readexactly(int(headers.get("content-length", "0")))
            try:
                ans = await asyncio.get_running_loop().run_in_executor(None, dns_query, body)
                w.write(f"HTTP/1.1 200 OK\r\nContent-Type: application/dns-message\r\nContent-Length: {len(ans)}\r\n\r\n".encode() + ans)
            except OSError as e:
                print(f"dns: {e}")
                w.write(b"HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\n\r\n")
            await w.drain()
            return
        path = os.path.normpath(os.path.join(root, urllib.parse.unquote(url.path).lstrip("/") or "index.html"))
        if not path.startswith(root) or not os.path.isfile(path):
            w.write(b"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n")
        else:
            body = open(path, "rb").read()
            ctype = TYPES.get(os.path.splitext(path)[1], "application/octet-stream")
            w.write(f"HTTP/1.1 200 OK\r\nContent-Type: {ctype}\r\nContent-Length: {len(body)}\r\nCache-Control: no-cache\r\n\r\n".encode() + body)
        await w.drain()
    except (asyncio.IncompleteReadError, ConnectionError, ValueError) as e:
        print(f"connection ended: {e!r}")
    finally:
        w.close()


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("site", nargs="?", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../vendor/build/akuma-poc/site"))
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--bind", default="127.0.0.1")
    o = ap.parse_args()
    root = os.path.abspath(o.site)
    srv = await asyncio.start_server(lambda r, w: handle(r, w, root), o.bind, o.port)
    print(f"serving {root} on http://{o.bind}:{o.port}/ (WebSocket TCP relay at /tcp)")
    async with srv:
        await srv.serve_forever()


asyncio.run(main())

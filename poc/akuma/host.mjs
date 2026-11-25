// SPDX-License-Identifier: MIT
// Copyright (C) 2026 unkernel contributors
//
// Host for a non-Linux kernel on kwemu: instantiates a wasm64 kernel module that links libkwemu.a, hands it a
// program, and drives it. The kernel owns its memory (memory64, exported), so the host provides the console, two
// clocks and, optionally, a network. Works in a browser (Chrome >= 133, memory64) and in Node >= 24 (the same file).
//
// Contract with the module (crates/akuma-kernel-wasm in the akuma repo):
//   imports  env.host_write(fd, ptr, len)  env.host_now_ns() -> f64  env.host_wall_ns() -> f64
//            env.host_net_connect(id, ip, port)  env.host_net_send(id, ptr, len)  env.host_net_close(id)
//            env.host_net_udp(id, ip, port, ptr, len)
//   exports  memory, akuma_trace(flags), akuma_rootfs_buf(len), akuma_image_buf(len), akuma_args_buf(len),
//            akuma_start(image_len, args_len, argc) -> i32, akuma_step() -> 0 done | 1 busy | 2 idle,
//            akuma_status(), akuma_wake_at() -> ns, akuma_net_event(id, kind, arg),
//            akuma_net_rx_buf(id) / _rx_space(id) / _rx_commit(id, n) / _rx_peer(id, ip, port)
// Pointers and sizes are i64 (wasm64), so they cross the JS boundary as BigInt; ids, ips and ports are numbers.
//
// A network backend is { attach(host), tcpConnect(id, ip, port), tcpSend(id, bytes), tcpClose(id),
// udpSend(id, ip, port, bytes) }; `host` (given to attach) has event(id, kind, arg) with kind 1 connected, 2 failed
// (errno), 3 closed by the peer, data(id, bytes), and datagram(id, ip, port, bytes). ip is a big-endian u32.

const enc = new TextEncoder();
const tick = () => new Promise((r) => setTimeout(r, 0));

export const EV_CONNECTED = 1, EV_ERROR = 2, EV_EOF = 3;
export const ECONNREFUSED = 111, ENETUNREACH = 101, ETIMEDOUT = 110;

/**
 * @param {{ wasm: BufferSource, elf: Uint8Array, argv: string[], envp?: string[],
 *           write: (fd: number, text: string) => void, trace?: boolean, rootfs?: Uint8Array | null,
 *           net?: object | null }} o
 * @returns {Promise<number>} the program's exit status (128 + signal on a fatal trap, negative on a boot failure)
 */
export async function run({ wasm, elf, argv, envp = ["HOME=/", "PATH=/bin:/sbin:/usr/bin", "TERM=dumb"], write, trace = false, rootfs = null, net = null }) {
  const dec = [null, new TextDecoder("utf-8"), new TextDecoder("utf-8")];
  let mem, x;
  let kick = () => {};
  const view = (ptr, len) => new Uint8Array(mem.buffer, Number(ptr), Number(len));

  // Bytes the backend has produced but the kernel's receive buffer cannot take yet, per socket slot.
  const queues = new Map(); // id -> { chunks: Uint8Array[], eof: boolean }
  const pending = (id) => {
    let q = queues.get(id);
    if (!q) queues.set(id, (q = { chunks: [], eof: false }));
    return q;
  };
  const hostApi = {
    event(id, kind, arg = 0) {
      if (kind === EV_EOF) pending(id).eof = true; // delivered after the data already queued
      else x.akuma_net_event(id, kind, arg);
      kick();
    },
    data(id, bytes) {
      pending(id).chunks.push(bytes);
      kick();
    },
    datagram(id, ip, port, bytes) {
      if (!x.akuma_net_rx_space(id)) return; // a full queue drops, as UDP does
      x.akuma_net_rx_peer(id, ip >>> 0, port);
      view(x.akuma_net_rx_buf(id), bytes.length).set(bytes);
      x.akuma_net_rx_commit(id, BigInt(bytes.length));
      kick();
    },
  };
  net?.attach?.(hostApi);

  const pump = () => {
    for (const [id, q] of queues) {
      while (q.chunks.length) {
        const space = Number(x.akuma_net_rx_space(id));
        if (!space) break;
        const chunk = q.chunks[0];
        const n = Math.min(space, chunk.length);
        view(x.akuma_net_rx_buf(id), n).set(chunk.subarray(0, n));
        x.akuma_net_rx_commit(id, BigInt(n));
        if (n < chunk.length) q.chunks[0] = chunk.subarray(n);
        else q.chunks.shift();
      }
      if (q.eof && !q.chunks.length) {
        q.eof = false;
        x.akuma_net_event(id, EV_EOF, 0);
      }
    }
  };
  const busy = () => [...queues.values()].some((q) => q.chunks.length);

  const noNet = () => { throw new Error("the guest used the network but the page has none"); };
  const { instance } = await WebAssembly.instantiate(wasm, {
    env: {
      host_write(fd, ptr, len) {
        write(fd, (dec[fd] ?? dec[1]).decode(view(ptr, len), { stream: true }));
      },
      host_now_ns: () => performance.now() * 1e6,
      host_wall_ns: () => Date.now() * 1e6,
      host_net_connect(id, ip, port) {
        if (!net) return queueMicrotask(() => hostApi.event(id, EV_ERROR, ENETUNREACH));
        net.tcpConnect(id, ip >>> 0, port);
      },
      host_net_send(id, ptr, len) {
        net?.tcpSend(id, view(ptr, len).slice());
      },
      host_net_close(id) {
        queues.delete(id);
        net?.tcpClose(id);
      },
      host_net_udp(id, ip, port, ptr, len) {
        if (!net) return;
        net.udpSend(id, ip >>> 0, port, view(ptr, len).slice());
      },
    },
  });
  x = instance.exports;
  mem = x.memory;

  const strings = [...argv, ...envp].map((s) => enc.encode(s + "\0"));
  const argsLen = strings.reduce((n, s) => n + s.length, 0);
  const imgPtr = x.akuma_image_buf(BigInt(elf.length));
  const argPtr = x.akuma_args_buf(BigInt(argsLen));
  if (!imgPtr || !argPtr) throw new Error("image or arguments do not fit");
  view(imgPtr, elf.length).set(elf);
  let at = Number(argPtr);
  for (const s of strings) {
    new Uint8Array(mem.buffer, at, s.length).set(s);
    at += s.length;
  }
  if (rootfs) {
    // A ustar archive: the guest's file system, imported into its tmpfs at boot.
    const p = x.akuma_rootfs_buf(BigInt(rootfs.length));
    if (!p) throw new Error("root file system does not fit");
    view(p, rootfs.length).set(rootfs);
  }
  x.akuma_trace(trace ? 1 : 0);
  const boot = x.akuma_start(BigInt(elf.length), BigInt(argsLen), BigInt(argv.length));
  if (boot !== 0) return boot;

  // The loop: run in bursts of ~10 ms so the page stays responsive; when the kernel is idle, wait for a backend
  // event or the kernel's next timer.
  for (;;) {
    const t0 = performance.now();
    let r;
    do {
      pump();
      r = x.akuma_step();
    } while (r === 1 && performance.now() - t0 < 10);
    if (r === 0) return x.akuma_status();
    if (r === 1 || busy()) {
      await tick();
      continue;
    }
    const wakeAt = x.akuma_wake_at();
    const ms = wakeAt < 0 ? Infinity : Math.max(0, (wakeAt - performance.now() * 1e6) / 1e6);
    await new Promise((resolve) => {
      let timer;
      kick = () => {
        clearTimeout(timer);
        kick = () => {};
        resolve();
      };
      if (ms !== Infinity) timer = setTimeout(kick, ms);
    });
  }
}

// ---- backends ----

/** TCP through a WebSocket-to-TCP proxy (poc/akuma/serve.py): one WebSocket per connection, `?host=IP&port=N`.
 *  UDP (the guest's DNS lookups) goes to `doh`: serve.py's /dns, which forwards the wire-format query to the machine's resolver
 *  (a DNS-over-HTTPS URL such as https://dns.google/dns-query also works if the server allows CORS). */
export function wsNet({ proxy, doh = proxy.replace(/^ws/, "http").replace(/\/tcp$/, "/dns") }) {
  let host;
  const socks = new Map();
  const dotted = (ip) => `${ip >>> 24}.${(ip >>> 16) & 255}.${(ip >>> 8) & 255}.${ip & 255}`;
  return {
    attach: (h) => (host = h),
    tcpConnect(id, ip, port) {
      // The QEMU-style gateway address stands for the machine serving the page.
      const target = ip === 0x0a000202 ? "127.0.0.1" : dotted(ip);
      const ws = new WebSocket(`${proxy}?host=${target}&port=${port}`);
      ws.binaryType = "arraybuffer";
      socks.set(id, ws);
      ws.onopen = () => host.event(id, EV_CONNECTED);
      ws.onmessage = (e) => host.data(id, new Uint8Array(e.data));
      ws.onerror = () => host.event(id, EV_ERROR, ECONNREFUSED);
      ws.onclose = () => host.event(id, EV_EOF);
    },
    tcpSend: (id, bytes) => socks.get(id)?.send(bytes),
    tcpClose(id) {
      socks.get(id)?.close();
      socks.delete(id);
    },
    async udpSend(id, ip, port, bytes) {
      if (port !== 53) return;
      try {
        const r = await fetch(doh, { method: "POST", headers: { "content-type": "application/dns-message", accept: "application/dns-message" }, body: bytes });
        if (!r.ok) throw new Error(`${doh}: HTTP ${r.status}`);
        host.datagram(id, ip, port, new Uint8Array(await r.arrayBuffer()));
      } catch (e) {
        console.warn("[akuma] DNS lookup failed:", e);
      }
    },
  };
}

/** A fake world for tests: `servers` maps "a.b.c.d:port" to `(conn) => (bytes) => void` (the returned function gets
 *  what the guest sends; `conn.write(bytes|string)` and `conn.close()` answer), `names` maps a host name to its
 *  address. DNS queries to any resolver are answered from `names`. */
export function loopbackNet({ servers, names }) {
  let host;
  const conns = new Map();
  const ipNum = (s) => s.split(".").reduce((a, b) => ((a << 8) | +b) >>> 0, 0);
  const dotted = (ip) => `${ip >>> 24}.${(ip >>> 16) & 255}.${(ip >>> 8) & 255}.${ip & 255}`;
  return {
    attach: (h) => (host = h),
    tcpConnect(id, ip, port) {
      const make = servers[`${dotted(ip)}:${port}`];
      setTimeout(() => {
        if (!make) return host.event(id, EV_ERROR, ECONNREFUSED);
        const conn = {
          write: (b) => host.data(id, typeof b === "string" ? enc.encode(b) : b),
          close: () => host.event(id, EV_EOF),
        };
        conns.set(id, make(conn));
        host.event(id, EV_CONNECTED);
      }, 2);
    },
    tcpSend: (id, bytes) => conns.get(id)?.(bytes),
    tcpClose: (id) => conns.delete(id),
    udpSend(id, ip, port, q) {
      if (port !== 53 || q.length < 12) return;
      // Question: labels from byte 12.
      let i = 12, name = [];
      while (q[i]) {
        name.push(new TextDecoder().decode(q.subarray(i + 1, i + 1 + q[i])));
        i += q[i] + 1;
      }
      const qend = i + 5; // NUL, QTYPE, QCLASS
      const qtype = (q[i + 1] << 8) | q[i + 2];
      const addr = names[name.join(".")];
      const head = [q[0], q[1], 0x81, addr ? 0x80 : 0x83, 0, 1, 0, addr && qtype === 1 ? 1 : 0, 0, 0, 0, 0];
      const out = [...head, ...q.subarray(12, qend)];
      if (addr && qtype === 1) out.push(0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, ...addr.split(".").map(Number));
      setTimeout(() => host.datagram(id, ip, port, Uint8Array.from(out)), 1);
      void ipNum;
    },
  };
}

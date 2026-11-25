#!/usr/bin/env -S /usr/local/bin/node
// SPDX-License-Identifier: MIT
// Copyright (C) 2026 unkernel contributors
//
// Runs a guest on the Akuma kernel under Node (>= 24, memory64) with the same host.mjs the browser uses and a real
// network: guest TCP becomes a TCP connection from this process, guest DNS (UDP port 53) goes to this machine's
// resolver. Exits with the guest's exit status.
//
//   run-node.mjs [--site DIR] [--rootfs rootfs.tar] [--net real|test|none] [--trace] ELF [ARG...]
//   run-node.mjs --sh 'apk --version; ls /bin'          (runs busybox sh -c)
import { readFileSync } from "node:fs";
import net from "node:net";
import dgram from "node:dgram";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { run, EV_CONNECTED, EV_ERROR, EV_EOF, ECONNREFUSED, ECONNRESET, ETIMEDOUT } from "./host.mjs";

const here = path.dirname(fileURLToPath(import.meta.url));
const opt = { site: path.join(here, "../../vendor/build/akuma-poc/site"), rootfs: "rootfs.tar", net: "real", trace: false, sh: null };
const pos = [];
for (let i = 2; i < process.argv.length; i++) {
  const a = process.argv[i];
  if (a === "--site") opt.site = process.argv[++i];
  else if (a === "--rootfs") opt.rootfs = process.argv[++i];
  else if (a === "--net") opt.net = process.argv[++i];
  else if (a === "--trace") opt.trace = true;
  else if (a === "--sh") opt.sh = process.argv[++i];
  else pos.push(a);
}

const dotted = (ip) => `${ip >>> 24}.${(ip >>> 16) & 255}.${(ip >>> 8) & 255}.${ip & 255}`;
const resolver = () => {
  try {
    for (const l of readFileSync("/etc/resolv.conf", "utf8").split("\n")) {
      const f = l.trim().split(/\s+/);
      if (f[0] === "nameserver" && /^\d+\.\d+\.\d+\.\d+$/.test(f[1]) && !f[1].startsWith("127.")) return f[1];
    }
  } catch {}
  return "1.1.1.1";
};

function nodeNet() {
  let host;
  const socks = new Map();
  return {
    attach: (h) => (host = h),
    tcpConnect(id, ip, port) {
      const target = ip === 0x0a000202 ? "127.0.0.1" : dotted(ip); // 10.0.2.2: the machine running this
      const s = net.connect({ host: target, port });
      socks.set(id, s);
      s.on("connect", () => host.event(id, EV_CONNECTED));
      s.on("data", (b) => host.data(id, new Uint8Array(b)));
      s.on("error", (e) => host.event(id, EV_ERROR, e.code === "ECONNRESET" ? ECONNRESET : e.code === "ETIMEDOUT" ? ETIMEDOUT : ECONNREFUSED));
      s.on("end", () => host.event(id, EV_EOF));
    },
    tcpSend: (id, bytes) => socks.get(id)?.write(bytes),
    tcpClose(id) {
      socks.get(id)?.destroy();
      socks.delete(id);
    },
    udpSend(id, ip, port, bytes) {
      if (port !== 53) return;
      const u = dgram.createSocket("udp4");
      u.on("message", (m) => {
        host.datagram(id, ip, port, new Uint8Array(m));
        u.close();
      });
      u.on("error", () => u.close());
      u.send(bytes, 53, resolver());
      setTimeout(() => { try { u.close(); } catch {} }, 5000).unref();
    },
  };
}

const wasm = readFileSync(path.join(opt.site, "akuma.wasm"));
const name = opt.sh ? "busybox.elf" : pos.shift() ?? "nettest.elf";
const argv = opt.sh ? ["/bin/busybox", "sh", "-c", opt.sh] : [name, ...pos];
let netBackend = null;
if (opt.net === "real") netBackend = nodeNet();
else if (opt.net === "test") netBackend = (await import("./testnet.mjs")).testNet();
const status = await run({
  wasm,
  elf: new Uint8Array(readFileSync(path.join(opt.site, name))),
  rootfs: opt.rootfs === "none" ? null : new Uint8Array(readFileSync(path.join(opt.site, opt.rootfs))),
  argv,
  trace: opt.trace,
  net: netBackend,
  write: (fd, text) => (fd === 2 ? process.stderr : process.stdout).write(text),
});
process.exit(typeof status === "number" ? status & 255 : 1);

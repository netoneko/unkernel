// SPDX-License-Identifier: MIT
// Copyright (C) 2026 unkernel contributors
//
// The fake world the in-page network test runs against: a resolver that knows example.test, and an HTTP server on
// 10.0.2.2:80 that answers GET /hello with a body and GET /big with 100000 bytes.
import { loopbackNet } from "./host.mjs";

const http = (conn) => {
  let buf = "";
  return (bytes) => {
    buf += new TextDecoder().decode(bytes);
    if (!buf.includes("\r\n\r\n")) return;
    const path = buf.split(" ")[1];
    const body = path === "/big" ? "0123456789abcdef".repeat(6250) : `hello from the fake server, path=${path}\n`;
    conn.write(`HTTP/1.1 200 OK\r\nContent-Length: ${body.length}\r\nConnection: close\r\n\r\n`);
    conn.write(body);
    setTimeout(() => conn.close(), 5);
  };
};

export const testNet = () => loopbackNet({ servers: { "10.0.2.2:80": http }, names: { "example.test": "10.0.2.2" } });

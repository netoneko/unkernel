#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 unkernel contributors
"""Run a staged site's index.html in headless Chrome and print what the guest wrote.

  run-chrome.py SITE_DIR [--elf NAME] [--arg X ...] [--trace] [--timeout S]

SITE_DIR holds index.html, host.mjs, akuma.wasm and the guest ELF(s) (build.sh stages it). The page runs the
program to its end and leaves the console text in <pre id=out|err> and the status in <body data-exit=N>;
the script dumps the DOM, prints stdout/stderr, and exits with the guest's status (125: host error,
124: timeout).

The page is opened as a file:// URL (with --allow-file-access-from-files), not served over HTTP: a
sandboxed Chrome can fail to reach even 127.0.0.1 (no request ever arrives), and the PoC needs no server.
"""
import argparse, html, os, re, shutil, signal, subprocess, sys, tempfile, urllib.parse

ap = argparse.ArgumentParser()
ap.add_argument("site")
ap.add_argument("--elf", default="hello.elf")
ap.add_argument("--arg", action="append", default=[])
ap.add_argument("--rootfs", help="ustar archive in SITE_DIR for the guest's file system")
ap.add_argument("--trace", action="store_true")
ap.add_argument("--timeout", type=int, default=120)
ap.add_argument("--chrome", default=shutil.which("google-chrome") or shutil.which("chromium") or "google-chrome")
o = ap.parse_args()

q = urllib.parse.urlencode([("elf", o.elf)] + [("arg", a) for a in o.arg] + ([("rootfs", o.rootfs)] if o.rootfs else []) + ([("trace", "1")] if o.trace else []))
url = f"file://{os.path.abspath(o.site)}/index.html?{q}"
prof = tempfile.mkdtemp(prefix="akuma-chrome-")
p = subprocess.Popen([o.chrome, "--headless=new", "--no-sandbox", "--disable-gpu", "--allow-file-access-from-files",
                      f"--user-data-dir={prof}", "--virtual-time-budget=600000", "--dump-dom", url],
                     stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, start_new_session=True)
try:
    dom, _ = p.communicate(timeout=o.timeout)
except subprocess.TimeoutExpired:
    os.killpg(p.pid, signal.SIGKILL)
    p.wait()
    print("timeout", file=sys.stderr)
    sys.exit(124)
finally:
    shutil.rmtree(prof, ignore_errors=True)

def pre(i):
    m = re.search(rf'<pre id="{i}">(.*?)</pre>', dom, re.S)
    return html.unescape(m.group(1)) if m else ""

sys.stdout.write(pre("out"))
sys.stderr.write(pre("err"))
m = re.search(r'data-exit="([^"]*)"', dom)
try:
    sys.exit(int(m.group(1)) & 0xFF)
except (AttributeError, ValueError):
    print("page did not finish", file=sys.stderr)
    sys.exit(125)

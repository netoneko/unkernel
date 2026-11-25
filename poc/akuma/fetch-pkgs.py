#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 unkernel contributors
"""fetch-pkgs.py DEST NAME...: download the named Alpine (aarch64, main) packages and unpack them into DEST.

Used to pre-seed a root file system archive (rootfs-curl.tar) with what `apk add curl` would install, so a test does
not spend minutes installing it on the emulated CPU every run. Downloads are cached next to DEST (DEST.cache)."""
import io, os, subprocess, sys, tarfile, urllib.request

BASE = "https://dl-cdn.alpinelinux.org/alpine/latest-stable/main/aarch64"
dest, names = sys.argv[1], sys.argv[2:]
cache = dest + ".cache"
os.makedirs(cache, exist_ok=True)
os.makedirs(dest, exist_ok=True)

index_path = os.path.join(cache, "APKINDEX.tar.gz")
if not os.path.exists(index_path):
    urllib.request.urlretrieve(f"{BASE}/APKINDEX.tar.gz", index_path)
text = tarfile.open(index_path).extractfile("APKINDEX").read().decode()
versions = {}
for block in text.split("\n\n"):
    f = dict(l.split(":", 1) for l in block.split("\n") if ":" in l)
    if "P" in f:
        versions[f["P"]] = f["V"]
for n in names:
    apk = os.path.join(cache, f"{n}-{versions[n]}.apk")
    if not os.path.exists(apk):
        urllib.request.urlretrieve(f"{BASE}/{n}-{versions[n]}.apk", apk)
    # An apk is three gzip streams (signature, control, data); -i reads across them.
    subprocess.run(["tar", "-xzi", "-f", apk, "-C", dest, "--exclude=.PKG*", "--exclude=.SIGN*", "--exclude=.post-*", "--exclude=.pre-*", "--exclude=.trigger"], check=True, stderr=subprocess.DEVNULL)
    print(f"{n}-{versions[n]}")

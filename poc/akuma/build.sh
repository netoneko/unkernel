#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 unkernel contributors
#
# Build and run the Akuma-on-kwemu proof of concept: a different kernel (Akuma, Rust, wasm64) in place of Linux,
# driving the same kwemu CPU through include/kw_emu.h. See README.md.
#
#   poc/akuma/build.sh [--akuma DIR] build      kwemu + guests (zig), the Akuma wasm module (cargo), the site
#   poc/akuma/build.sh [--akuma DIR] test       build, then run the guests in headless Chrome and check them
#   poc/akuma/build.sh [--akuma DIR] run ELF [ARG...]   build, then run one guest (a name in the site dir)
#   poc/akuma/build.sh [--akuma DIR] apk-demo   Alpine's static apk installs a local package (fetches two files once)
#
# Environment: ZIG (Zig 0.16; else vendor/toolchain/zig-dl/*/zig, else PATH), OUT (default vendor/build/akuma-poc),
# CHROME. AKUMA is the Akuma checkout (default ../akuma), on a branch with crates/akuma-kernel-wasm.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
akuma="${AKUMA:-$root/../akuma}"
if [ "${1:-}" = --akuma ]; then akuma="$(cd "$2" && pwd)"; shift 2; fi
out="${OUT:-$root/vendor/build/akuma-poc}"
zig="${ZIG:-$(ls "$root"/vendor/toolchain/zig-dl/*/zig 2>/dev/null | head -1 || true)}"
[ -n "$zig" ] || zig="$(command -v zig || true)"
[ -x "$zig" ] || { echo "zig 0.16 not found (set ZIG=)" >&2; exit 1; }
[ -d "$akuma/crates/akuma-kernel-wasm" ] || { echo "$akuma has no crates/akuma-kernel-wasm (check out the unkernel-poc branch, or --akuma DIR)" >&2; exit 1; }

build() {
	mkdir -p "$out"
	# kwemu as a wasm64 static archive, and the A64 test guests. (`install` builds everything in build.zig;
	# the second run is a cache hit.)
	(cd "$root" && "$zig" build -Doptimize=ReleaseFast --prefix "$out/zig")
	# The Akuma kernel for wasm64: nightly + build-std, pinned by the akuma checkout's rust-toolchain.toml.
	(cd "$akuma" && KWEMU_LIB_DIR="$out/zig/lib" CARGO_TARGET_DIR="$out/cargo" \
		cargo rustc --release -Zbuild-std=core --target wasm64-unknown-unknown -p akuma-kernel-wasm --crate-type cdylib)
	local site="$out/site"
	rm -rf "$site"
	mkdir -p "$site"
	cp "$here/index.html" "$here/host.mjs" "$here/testnet.mjs" "$site/"
	cp "$out/cargo/wasm64-unknown-unknown/release/akuma_kernel_wasm.wasm" "$site/akuma.wasm"
	cp "$out"/zig/guest/a64/*.elf "$site/"
	# A guest of our own for the file system, built with zig's musl (static aarch64), and the archive it reads.
	"$zig" cc -target aarch64-linux-musl -O2 -static -o "$site/fstest.elf" "$here/guest/fstest.c"
	"$zig" cc -target aarch64-linux-musl -O2 -static -o "$site/pipetest.elf" "$here/guest/pipetest.c"
	"$zig" cc -target aarch64-linux-musl -O2 -static -o "$site/nettest.elf" "$here/guest/nettest.c"
	"$zig" cc -target aarch64-linux-musl -O2 -static -o "$site/httpget.elf" "$here/guest/httpget.c"
	# The root file system: poc/akuma/rootfs plus the programs built here (/bin/echoargs).
	rm -rf "$out/rootfs-stage" && cp -r "$here/rootfs" "$out/rootfs-stage"
	"$zig" cc -target aarch64-linux-musl -O2 -static -o "$out/rootfs-stage/bin/echoargs" "$here/guest/echoargs.c"
	tar --format=ustar --owner=0 --group=0 --numeric-owner -cf "$site/rootfs.tar" -C "$out/rootfs-stage" .
	# Akuma's own userspace (static aarch64-unknown-none ELFs on libakuma), when the checkout has built them.
	local u="$akuma/userspace/target/aarch64-unknown-none/release"
	for p in hello echo2; do [ -f "$u/$p" ] && cp "$u/$p" "$site/akuma-$p"; done
	true
}

# name  expected-exit  expected-output-substring  [args...]
cases=(
	"hello.elf 42 tls=42 one two"
	"akuma-echo2 0 Goodbye"
	"akuma-hello 0 hello:\\ done 2 10"
	"inttest.elf 0 inttest\\ done"
	"cryptotest.elf 0 0\\ failure"
	"atomictest.elf 0 all\\ ok"
	"lsetest.elf 0 0\\ failure"
	"fptest.elf 0 0\\ known-answer\\ failure"
	"simdtest.elf 0 words"
	"simdfptest.elf 0 0\\ known-answer\\ failure"
	"simdmovetest.elf 0 0\\ failure"
	"fstest.elf 7 fstest\\ ok"
	"pipetest.elf 7 pipetest\\ ok"
	"nettest.elf 7 nettest\\ ok"
)

test_all() {
	local fail=0 name want sub log
	for c in "${cases[@]}"; do
		eval "set -- $c"
		name=$1 want=$2 sub=$3
		shift 3
		[ -f "$out/site/$name" ] || { echo "skip $name (not built)"; continue; }
		log="$(mktemp)"
		args=()
		for a in "$@"; do args+=("--arg=$a"); done
		[ "$name" = nettest.elf ] && args+=(--net test)
		set +e
		"$here/run-chrome.py" "$out/site" --rootfs rootfs.tar --elf "$name" "${args[@]}" >"$log" 2>"$log.err"
		got=$?
		set -e
		if [ "$got" = "$want" ] && grep -aqF -- "$sub" "$log"; then
			echo "ok   $name (exit $got)"
		else
			echo "FAIL $name: exit $got (want $want), looking for '$sub'"
			head -c 600 "$log" "$log.err" | sed 's/^/     /'
			fail=1
		fi
		rm -f "$log" "$log.err"
	done
	return $fail
}

# Alpine's own static apk, installing a package from a local file into a scratch root on the kernel (needs network
# once, to fetch apk-tools-static and one small package from dl-cdn.alpinelinux.org; cached under $out/dl).
apk_demo() {
	build
	local dl="$out/dl" base=https://dl-cdn.alpinelinux.org/alpine/latest-stable/main/aarch64
	mkdir -p "$dl"
	if [ ! -f "$dl/apk.static" ] || [ ! -f "$dl/ca.apk" ]; then
		local index apkpkg capkg
		index=$(curl -sSfL "$base/")
		apkpkg=$(grep -o 'apk-tools-static-[^"]*\.apk' <<<"$index" | head -1)
		capkg=$(grep -o 'ca-certificates-bundle-[0-9][^"]*\.apk' <<<"$index" | head -1)
		curl -sSfL -o "$dl/apk-tools-static.apk" "$base/$apkpkg"
		curl -sSfL -o "$dl/ca.apk" "$base/$capkg"
		tar xzf "$dl/apk-tools-static.apk" -C "$dl" sbin/apk.static 2>/dev/null
		mv "$dl/sbin/apk.static" "$dl/apk.static"
	fi
	rm -rf "$out/apk-stage"
	cp -r "$out/rootfs-stage" "$out/apk-stage"
	mkdir -p "$out/apk-stage/sbin" "$out/apk-stage/pkg" "$out/apk-stage/tmp"
	cp "$dl/apk.static" "$out/apk-stage/sbin/apk"
	cp "$dl/ca.apk" "$out/apk-stage/pkg/ca.apk"
	tar --format=ustar --owner=0 --group=0 --numeric-owner -cf "$out/site/apk.tar" -C "$out/apk-stage" .
	cp "$dl/apk.static" "$out/site/apk.static"
	"$here/run-chrome.py" "$out/site" --rootfs apk.tar --elf apk.static \
		--arg=--root --arg=/tmp/r --arg=--initdb --arg=--allow-untrusted --arg=--no-network --arg=--no-scripts \
		--arg=--force-non-repository --arg=add --arg=/pkg/ca.apk
}

case "${1:-build}" in
build) build ;;
test) build && test_all ;;
apk-demo) apk_demo ;;
run)
	shift
	build
	elf=$1
	shift
	args=()
	for a in "$@"; do args+=("--arg=$a"); done
	"$here/run-chrome.py" "$out/site" --rootfs rootfs.tar --elf "$elf" "${args[@]}"
	;;
*) sed -n 4,12p "$0" >&2; exit 2 ;;
esac

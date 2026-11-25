// SPDX-License-Identifier: MIT
// Copyright (C) 2026 unkernel contributors
//
// Minimal host for a non-Linux kernel on kwemu: instantiates a wasm64 kernel module that links libkwemu.a and
// hands it a program to run. The kernel owns its memory (memory64, exported), so the host only provides the
// console and a clock. Works in a browser (Chrome >= 133, memory64) and in Node >= 24 (the same file).
//
// Contract with the module (crates/akuma-kernel-wasm in the akuma repo):
//   imports  env.host_write(fd: i32, ptr: i64, len: i64)   env.host_now_ns() -> f64
//   exports  memory, akuma_trace(flags), akuma_rootfs_buf(len) -> ptr (optional ustar archive), akuma_image_buf(len) -> ptr, akuma_args_buf(len) -> ptr, akuma_run(image_len, args_len, argc) -> i32
// Pointers and sizes are i64 (wasm64), so they cross the JS boundary as BigInt.

const enc = new TextEncoder();

/**
 * @param {{ wasm: BufferSource, elf: Uint8Array, argv: string[], envp?: string[],
 *           write: (fd: number, text: string) => void, trace?: boolean, rootfs?: Uint8Array | null }} o
 * @returns {number} the program's exit status (128 + signal on a fatal trap, negative on a boot failure)
 */
export async function run({ wasm, elf, argv, envp = ["HOME=/", "PATH=/bin", "TERM=dumb"], write, trace = false, rootfs = null }) {
  const dec = [null, new TextDecoder("utf-8"), new TextDecoder("utf-8")];
  let mem;
  const { instance } = await WebAssembly.instantiate(wasm, {
    env: {
      host_write(fd, ptr, len) {
        const bytes = new Uint8Array(mem.buffer, Number(ptr), Number(len));
        write(fd, (dec[fd] ?? dec[1]).decode(bytes, { stream: true }));
      },
      host_now_ns: () => performance.now() * 1e6,
    },
  });
  const x = instance.exports;
  mem = x.memory;

  const strings = [...argv, ...envp].map((s) => enc.encode(s + "\0"));
  const argsLen = strings.reduce((n, s) => n + s.length, 0);
  const imgPtr = x.akuma_image_buf(BigInt(elf.length));
  const argPtr = x.akuma_args_buf(BigInt(argsLen));
  if (!imgPtr || !argPtr) throw new Error("image or arguments do not fit");
  new Uint8Array(mem.buffer, Number(imgPtr), elf.length).set(elf);
  let at = Number(argPtr);
  for (const s of strings) {
    new Uint8Array(mem.buffer, at, s.length).set(s);
    at += s.length;
  }
  if (rootfs) {
    // A read-only ustar archive: the guest's file system.
    const p = x.akuma_rootfs_buf(BigInt(rootfs.length));
    if (!p) throw new Error("root file system does not fit");
    new Uint8Array(mem.buffer, Number(p), rootfs.length).set(rootfs);
  }
  x.akuma_trace(trace ? 1 : 0);
  return x.akuma_run(BigInt(elf.length), BigInt(argsLen), BigInt(argv.length));
}

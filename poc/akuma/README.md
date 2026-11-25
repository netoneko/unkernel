# Akuma on kwemu (proof of concept)

A different kernel in place of Linux: [Akuma](https://github.com/netoneko/akuma) (Rust, `no_std`) compiled to a
wasm64 module that links **kwemu** (this repository's `src/`, contract `include/kw_emu.h`, ABI 4) and runs
unmodified aarch64 programs. It is the "kernel driving the C ABI only" idea of `src/emu/harness.zig`, with the
kernel written in Rust from Akuma's crates instead of Zig. Nothing in `kernel/` or `js/` changed: this directory
is the whole unkernel side.

```sh
poc/akuma/build.sh test                       # kwemu + guests (zig), the Akuma module (cargo), 11 guests in headless Chrome
poc/akuma/build.sh run akuma-echo2            # one guest
poc/akuma/run-chrome.py vendor/build/akuma-poc/site --elf hello.elf --arg one --trace
```

Needs Zig 0.16 (`ZIG=` or `vendor/toolchain/zig-dl/*/zig`), the Akuma checkout on branch `unkernel-poc` at
`../akuma` (`AKUMA=` or `--akuma DIR`; its `rust-toolchain.toml` pins the nightly, `rust-src` is enough for
`-Zbuild-std`), and Chrome 133 or later (memory64). No Node and no `vendor/` kernel build are needed.

## What is here

| File | What |
| --- | --- |
| `host.mjs` | The whole host: instantiates the module, gives it a console and a clock, copies the ELF and arguments in, calls `akuma_run`. Browser and Node. |
| `index.html` | A page that runs `?elf=NAME&arg=X...` and leaves the output in `<pre id=out|err>` and the status in `<body data-exit>`. |
| `run-chrome.py` | Runs the page in headless Chrome as a `file://` URL (see below) and turns the DOM into stdout, stderr and an exit status. |
| `build.sh` | `zig build` (libkwemu.a, the A64 guests), `cargo rustc` (the Akuma module), stage `vendor/build/akuma-poc/site`, the test list. |

## The contract a non-Linux kernel meets

What a kernel module must provide for kwemu, found by reading `include/kw_emu.h`, `src/emu/harness.zig` and
`.agents/kernel.md`:

- **Link**: kwemu is a **wasm64** static archive (`zig build` -> `libkwemu.a`, no allocator, no globals, JIT off by
  default). Link it into a wasm64 module (here Rust's `rust-lld`, `-l kwemu`); it imports nothing without the JIT.
- **Memory**: the kernel owns `struct kw_cpu` (`KW_CPU_SIZE` = 9 MiB, 8-aligned) and a `struct kw_regs`
  (800 bytes, 16-aligned); "physical" = linear address minus `cpu->phys_base`. Guest RAM is any region of the
  module's memory; the PoC grows its own (`memory.grow`) and uses `phys_base = 0`.
- **Page tables**: RISC-V Sv48 PTEs (`V R W X U G A D`, PPN at bit 10), 4 KiB leaves at level 0 only, user VAs
  below 2^48. kwemu never writes a PTE: `A=0`, or `D=0` on a store, is a `KW_EV_FAULT` and the kernel sets the bits and
  calls `kw_flush_tlb_range`. Switch spaces with `kw_set_pgd`.
- **The loop**: `cpu->budget = N; ev = kw_run(cpu, regs)`; on `KW_EV_SYSCALL` the number is `x8`, arguments
  `x0..x5`, `pc` already past the `SVC`; on `KW_EV_FAULT` `pc` is at the faulting instruction; call
  `kw_clear_reservation` on every trap return. `kw_abi_version()` must equal 4 (the module checks it at boot).
- **Host imports**: none from kwemu itself. Linux's `unkernel` and WASI imports (`js/core/kernel.ts`) are *Linux's*;
  a different kernel brings its own (here `env.host_write`, `env.host_now_ns`). The Linux-specific suspending imports
  (`wasm_switch_to`, `poll_oneoff` ...) are only needed by a kernel that switches stacks with JSPI.

## Decisions and findings

- **wasm64, directly in Chrome.** Linux's build is wasm64 then lowered to memory32 by Binaryen for every host; Chrome
  runs memory64 natively (the page just passes BigInt pointers), so the PoC skips the lowering. For other browsers run
  `wasm-opt --memory64-lowering --table64-lowering` as `kernel/build.sh lower` does; pointers stay i64.
- **No HTTP.** In this environment headless Chrome never connected to `127.0.0.1` (the server saw no request), so
  `run-chrome.py` loads the page from `file://` with `--allow-file-access-from-files` and reads the DOM
  (`--dump-dom --virtual-time-budget`). The clock is therefore virtual and `host_now_ns` reads 0 in those runs.
- **JIT off.** `-Dkwemu-jit=true` makes kwemu import `unkernel.kw_host_jit_*` and own 1024 slots of the module's function
  table; `js/core/jit.ts` is the host half and is not wired here. The interpreter tier runs all eleven guests.
- **kwemu test guests as a CPU conformance suite** for any kernel built on it: the eight `test/guest/a64/*.elf` programs
  exercise integer, crypto, LSE, atomics, FP and AdvSIMD through the kernel's own page tables (every guest page goes
  through the demand-fault and A/D path), and pass under Akuma.
- **A static non-PIE aarch64 ELF is the whole requirement** of the programs. Akuma's own userspace (`aarch64-unknown-none`,
  libakuma) additionally needs a read-only `ProcessInfo` page at VA `0x1000` and two private syscalls (`UPTIME` 319,
  `SET_TPIDR_EL0` 320), which the Akuma module provides.

## Next steps on this side

1. Share `js/core/jit.ts` with `host.mjs` so the module can be built with the JIT (needs the exported function table).
2. A `disk` import (a `kwblk`-style block device from `js/core/block.ts`) so Akuma's ext2 can serve `openat`.
3. Decide, with the Akuma side, whether a blocking syscall is a JSPI suspension (`WebAssembly.Suspending` imports, as
   `wasm_switch_to`) or an explicit continuation; the former reuses `js/core/kernel.ts`'s machinery.
4. If this becomes more than a PoC: have the module export the ABI number it was linked against so `host.mjs` can check it, and a CI job (zig + cargo + Chrome) for `build.sh test`.

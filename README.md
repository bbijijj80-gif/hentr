# hentrOS

A tiny hobby toy operating system with a Windows-like graphical desktop,
booted through a custom UEFI bootloader. Written from scratch in freestanding
C — no borrowed OS code, no external EFI SDK.

This is an educational hobby-OS demo, not a production or general-purpose
operating system. It has no filesystem driver, no process model, no
networking, and no hardware driver stack beyond a framebuffer, PS/2
mouse/keyboard polling, and the CMOS real-time clock.

## What's here

- `boot/` — a minimal UEFI application (`BOOTX64.EFI`). It talks to the
  firmware directly using a small hand-written subset of the UEFI API
  (`boot/efi.h`, no gnu-efi/edk2 dependency): it picks a Graphics Output
  Protocol video mode, loads `KERNEL.BIN` from the EFI System Partition,
  calls `ExitBootServices`, and jumps into the kernel.
- `kernel/` — a freestanding, flat-binary kernel with no libc. It draws a
  desktop: a gradient wallpaper, a taskbar with a Start button and a live
  clock (read from the CMOS RTC), a draggable window with a close button,
  and a Start menu — all rendered by hand into the linear framebuffer with
  a small custom 5x7 bitmap font. Mouse and keyboard input come from
  polling the PS/2 controller directly (no interrupts).

## Building

Requires `clang`+`lld` (bootloader, built for the `x86_64-unknown-windows`
/ PE-COFF target so it emits an EFI application) and GNU `binutils`
(`ld`/`objcopy`, kernel, built as a normal ELF then flattened to a raw
binary). On Debian/Ubuntu:

```sh
apt-get install clang lld binutils qemu-system-x86 ovmf mtools dosfstools
make
```

This produces `iso/EFI/BOOT/BOOTX64.EFI` and `iso/KERNEL.BIN` — the layout
of a FAT boot volume ready to hand to a UEFI firmware.

## Running it

```sh
make run
```

boots the `iso/` directory directly as a FAT volume in QEMU with OVMF
firmware. Move the mouse to interact with the desktop: click **Start** to
open the menu, drag the window's title bar to move it, click the red
**X** to close it.

## How the handoff works

The bootloader and kernel don't share a runtime or calling convention —
the bootloader is compiled as Windows/PE code (required for UEFI), while
the kernel is a plain System V ELF. The jump between them is done through
a function pointer explicitly marked `sysv_abi` on the caller side
(`boot/boot.c`), and the kernel clears its own `.bss` on entry, since
`AllocatePages` does not promise zeroed memory (`kernel/kernel.c`,
`kernel/kernel.ld`).

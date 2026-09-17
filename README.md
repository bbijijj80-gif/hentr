# hentrOS

A tiny hobby toy operating system with a Windows-like graphical desktop,
booted through a custom UEFI bootloader. Written from scratch in freestanding
C — no borrowed OS code, no external EFI SDK.

This is an educational hobby-OS demo, not a production or general-purpose
operating system. It has no filesystem driver, no process model, and no
networking.

## What's here

hentrOS is a single UEFI application (`BOOTX64.EFI`) — it never calls
`ExitBootServices`, so there's no separate kernel stage. It talks to the
firmware directly using a small hand-written subset of the UEFI API
(`boot/efi.h`, no gnu-efi/edk2 dependency):

- `boot/boot.c` — picks a Graphics Output Protocol video mode, draws the
  boot menu, then runs the whole desktop: a gradient wallpaper, a taskbar
  with a Start button and a live clock (`EFI_RUNTIME_SERVICES.GetTime`),
  a draggable window with a close button, and a Start menu.
- `kernel/gfx.c`, `kernel/font.c` — a small software rasterizer (rects,
  lines, gradients, a custom 5x7 bitmap font) drawing into an off-screen
  buffer, plus a `gfx_present()` that blits it to the real framebuffer in
  one shot each frame. Drawing off-screen and presenting atomically is
  what keeps the screen flicker/tear-free — painting shapes directly onto
  a framebuffer the display is simultaneously scanning out causes visible
  tearing, which is worse the slower the machine draws.
- `kernel/logo.c` — a stylized, procedurally-drawn recreation of the
  project's hand-drawn yellow-marker logo sketch (a hatched center with
  eight swirling petals), built from integer-only spiral math (no libm).

**Input** goes through UEFI's own protocols instead of raw hardware
ports: `EFI_SIMPLE_POINTER_PROTOCOL` (falling back to
`EFI_ABSOLUTE_POINTER_PROTOCOL`) for the mouse, and
`EFI_SIMPLE_TEXT_INPUT_PROTOCOL` for the keyboard. This matters because
UEFI's own driver stack understands USB HID devices — almost universal on
real hardware today — where hand-rolling a PS/2 port driver would not see
a USB mouse or keyboard at all once boot firmware handed off. Arrow
keys + Enter/Space always work as a keyboard-only fallback for the whole
desktop, in case no pointer device is found.

## Boot menu: Live vs. Install

The bootloader always shows a menu with two choices before it does
anything to a disk:

- **[1] Live mode** — runs hentrOS straight from RAM, using only the
  media it was booted from. Nothing on any disk is written.
- **[2] Install** — uses the UEFI Simple File System protocol to find
  another disk volume and copies `BOOTX64.EFI` onto it under a new
  `\EFI\HENTROS\` directory. It never touches `\EFI\BOOT\` or any other
  existing file, so an existing Windows/Linux install on that disk is
  left completely intact — hentrOS just becomes an additional entry you
  can pick from your firmware's one-time boot menu (e.g. F12/Esc at
  power-on). After copying, it boots straight into hentrOS itself so you
  can try it immediately.

## Ready-made ISO

[`hentros.iso`](hentros.iso) in the repo root is a prebuilt, bootable disc
image — no toolchain needed to try it. It's a standard UEFI-only El
Torito ISO (a small FAT12 EFI System Partition embedded in an ISO 9660
disc), built with:

```sh
make iso
```

**Don't just copy the `.iso` file onto a USB drive** (drag-and-drop /
Ctrl+C-Ctrl+V) — that only puts a copy of the file on the stick, it does
not make the stick bootable. Write it with a raw-image tool instead:

- **Windows:** [Rufus](https://rufus.ie) — select the drive, pick
  `hentros.iso`, partition scheme **GPT**, target system **UEFI
  (non-CSM)**, and if it asks, write mode **DD Image** (not ISO mode).
  Or [balenaEtcher](https://etcher.balena.io), which does this
  automatically.
- **Linux/macOS:** `dd if=hentros.iso of=/dev/sdX bs=4M status=progress`
  (**check the device name carefully** — this overwrites the whole
  drive).

It also works directly as a virtual CD/DVD in a UEFI-enabled VM
(VirtualBox, VMware, QEMU: `qemu-system-x86_64 -bios OVMF.fd -cdrom
hentros.iso`). Either way you land on the same boot menu, and **Secure
Boot must be off** in your firmware settings first — this loader isn't
signed.

## Building

Requires `clang`+`lld` (built for the `x86_64-unknown-windows` / PE-COFF
target so it emits an EFI application). On Debian/Ubuntu:

```sh
apt-get install clang lld qemu-system-x86 ovmf mtools dosfstools xorriso
make        # produces iso/EFI/BOOT/BOOTX64.EFI
make iso    # also produces hentros.iso
```

## Running it

```sh
make run
```

boots the `iso/` directory directly as a FAT volume in QEMU with OVMF
firmware, plus emulated USB mouse/keyboard so the real input path gets
exercised rather than QEMU's default PS/2 devices. Move the mouse to
interact with the desktop: click **Start** to open the menu, drag the
window's title bar to move it, click the red **X** to close it — or use
the arrow keys and Enter if no pointer device is available.

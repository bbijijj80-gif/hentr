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

Different boards expose pointer devices very differently — some only
finish USB enumeration a couple of seconds after power-on, some publish
more than one pointer instance (a touchpad and a USB mouse both), a few
only ever expose the absolute variant. `desktop_loop()` in `boot/boot.c`
handles this by: forcing the full driver tree to connect
(`connect_all_controllers()`, the same thing UEFI Shell's `connect -r`
does, since several boards only bind USB HID drivers on demand),
enumerating *every* handle for both pointer protocols instead of taking
whichever one `LocateProtocol` happens to hand back first, retrying that
whole scan for a few seconds at startup, and re-scanning periodically
while running in case a device is hot-plugged or finishes enumerating
late.

If your mouse still doesn't move after that, it's very likely a BIOS/UEFI
setting rather than something the OS can work around: check for and
**disable "Fast Boot"**, and make sure **"Legacy USB Support"** /
**"USB Configuration"** is set to full/enabled rather than "boot only" or
disabled, and that **"XHCI Hand-off"** is enabled. Several boards skip
full USB initialization before the OS loads specifically to shave boot
time, which keeps *any* OS (not just this one) from seeing the mouse
until much later in a normal boot sequence. The on-screen keyboard
fallback (arrow keys + Enter) works regardless of any of this.

### In progress: a from-scratch xHCI driver

UEFI's own pointer protocols have proven unreliable on some real boards
(a `EFI_ABSOLUTE_POINTER_PROTOCOL` instance that reports as found but
never once returns real movement). To eventually bypass UEFI's USB stack
entirely, `boot/boot.c` is growing an original xHCI (USB 3 host
controller) driver, written from scratch rather than borrowed from any
other project:

1. **Done:** find the xHCI controller via `EFI_PCI_IO_PROTOCOL` (PCI
   class 0x0C/0x03, prog-if 0x30) and read its 64-bit MMIO BAR. Visible
   in the on-screen diagnostics as `XHCI: FOUND`.
2. **Done:** reset and initialize the controller: stop it, issue a host
   controller reset, program the Device Context Base Address Array,
   set up a Command Ring and an Event Ring (polled, no interrupts
   wired up), and start it running again. Visible as `XHCIINIT: OK`
   with the controller's slot/port counts and `USBSTS` before/after.
   Taking over the controller this way resets whatever firmware's own
   USB stack had going, so `EFI_ABSOLUTE_POINTER_PROTOCOL` support
   disappears at this point - expected, since our own driver is meant
   to replace it once later stages can actually read a port.
3. Scan ports for connected devices.
4. Enable a device slot, address it, fetch USB descriptors.
5. Configure an interrupt endpoint and read HID reports from the
   mouse/keyboard directly off the hardware.

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

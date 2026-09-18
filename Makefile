BUILD := build
ISO := iso

CC := clang
OBJCOPY := objcopy

# hentrOS is a single UEFI application: it never calls ExitBootServices,
# so there is no separate kernel stage or ABI bridge to worry about -
# everything is one PE/COFF binary built for the EFI target.
BOOT_CFLAGS := -target x86_64-unknown-windows -ffreestanding -fshort-wchar \
               -mno-red-zone -fno-stack-protector -Wall -Wno-unused-parameter -c

SRCS := boot/boot.c kernel/gfx.c kernel/font.c kernel/logo.c
OBJS := $(patsubst boot/%.c,$(BUILD)/%.o,$(filter boot/%.c,$(SRCS))) \
        $(patsubst kernel/%.c,$(BUILD)/%.o,$(filter kernel/%.c,$(SRCS)))

ISO_IMAGE := hentros.iso

.PHONY: all clean run iso

all: $(ISO)/EFI/BOOT/BOOTX64.EFI

$(BUILD)/%.o: boot/%.c
	@mkdir -p $(BUILD)
	$(CC) $(BOOT_CFLAGS) -o $@ $<

$(BUILD)/%.o: kernel/%.c
	@mkdir -p $(BUILD)
	$(CC) $(BOOT_CFLAGS) -o $@ $<

$(ISO)/EFI/BOOT/BOOTX64.EFI: $(OBJS)
	mkdir -p $(ISO)/EFI/BOOT
	$(CC) -target x86_64-unknown-windows -nostdlib -Wl,-entry:efi_main \
	      -Wl,-subsystem:efi_application -fuse-ld=lld -o $@ $(OBJS)

iso: $(ISO_IMAGE)

# Wraps the EFI System Partition (BOOTX64.EFI) into a FAT image, then
# that image into a plain ISO 9660 disc with an El Torito "no emulation"
# EFI boot entry pointing at it. This is the standard recipe for a
# single .iso that boots on real UEFI firmware as well as in QEMU+OVMF,
# and it can also just be `dd`'d onto a USB stick.
$(ISO_IMAGE): $(ISO)/EFI/BOOT/BOOTX64.EFI
	rm -f $(BUILD)/esp.img
	mkdir -p $(BUILD)/isoroot
	dd if=/dev/zero of=$(BUILD)/esp.img bs=1M count=4 status=none
	mkfs.vfat -F 12 -n HENTROS $(BUILD)/esp.img >/dev/null
	mmd -i $(BUILD)/esp.img ::/EFI ::/EFI/BOOT
	mcopy -i $(BUILD)/esp.img $(ISO)/EFI/BOOT/BOOTX64.EFI ::/EFI/BOOT/BOOTX64.EFI
	cp $(BUILD)/esp.img $(BUILD)/isoroot/efi.img
	xorriso -as mkisofs -V HENTROS -o $(ISO_IMAGE) \
	  -eltorito-platform efi -eltorito-alt-boot -e efi.img -no-emul-boot \
	  -isohybrid-gpt-basdat \
	  $(BUILD)/isoroot

clean:
	rm -rf $(BUILD) $(ISO) $(ISO_IMAGE)

run: all
	mkdir -p $(BUILD)
	cp -n /usr/share/OVMF/OVMF_VARS_4M.fd $(BUILD)/OVMF_VARS.fd 2>/dev/null || true
	qemu-system-x86_64 \
	  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
	  -drive if=pflash,format=raw,file=$(BUILD)/OVMF_VARS.fd \
	  -drive format=raw,file=fat:rw:$(ISO) \
	  -usb -device usb-mouse -device usb-kbd \
	  -m 256M -serial stdio

BUILD := build
ISO := iso

CC := clang
LD := ld
OBJCOPY := objcopy

BOOT_CFLAGS := -target x86_64-unknown-windows -ffreestanding -fshort-wchar \
               -mno-red-zone -fno-stack-protector -Wall -Wno-unused-parameter -c

KERNEL_CFLAGS := -ffreestanding -fno-stack-protector -mno-red-zone \
                  -mno-sse -mno-sse2 -mno-mmx -fno-pic -fno-pie -Wall -Wno-unused-parameter -c

KERNEL_SRCS := kernel/kernel.c kernel/gfx.c kernel/font.c kernel/mouse.c kernel/rtc.c
KERNEL_OBJS := $(patsubst kernel/%.c,$(BUILD)/%.o,$(KERNEL_SRCS))

.PHONY: all clean run

all: $(ISO)/EFI/BOOT/BOOTX64.EFI $(ISO)/KERNEL.BIN

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/boot.o: boot/boot.c boot/efi.h boot/bootinfo.h | $(BUILD)
	$(CC) $(BOOT_CFLAGS) -o $@ $<

$(ISO)/EFI/BOOT/BOOTX64.EFI: $(BUILD)/boot.o
	mkdir -p $(ISO)/EFI/BOOT
	$(CC) -target x86_64-unknown-windows -nostdlib -Wl,-entry:efi_main \
	      -Wl,-subsystem:efi_application -fuse-ld=lld -o $@ $<

$(BUILD)/%.o: kernel/%.c | $(BUILD)
	$(CC) $(KERNEL_CFLAGS) -o $@ $<

$(BUILD)/kernel.elf: $(KERNEL_OBJS) kernel/kernel.ld
	$(LD) -T kernel/kernel.ld -nostdlib -static -o $@ $(KERNEL_OBJS)

$(ISO)/KERNEL.BIN: $(BUILD)/kernel.elf
	mkdir -p $(ISO)
	$(OBJCOPY) -O binary $< $@

clean:
	rm -rf $(BUILD) $(ISO)

run: all
	qemu-system-x86_64 \
	  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
	  -drive if=pflash,format=raw,file=$(BUILD)/OVMF_VARS.fd \
	  -drive format=raw,file=fat:rw:$(ISO) \
	  -m 256M -serial stdio

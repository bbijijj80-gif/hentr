BUILD := build
ISO := iso

CC := clang
LD := ld
OBJCOPY := objcopy

BOOT_CFLAGS := -target x86_64-unknown-windows -ffreestanding -fshort-wchar \
               -mno-red-zone -fno-stack-protector -Wall -Wno-unused-parameter -c

KERNEL_CFLAGS := -ffreestanding -fno-stack-protector -mno-red-zone \
                  -mno-sse -mno-sse2 -mno-mmx -fno-pic -fno-pie -Wall -Wno-unused-parameter -c

# gfx.c/font.c/logo.c are shared: the bootloader uses them to draw its boot
# menu and logo directly on the GOP framebuffer, the kernel uses them for
# the desktop. Each side compiles its own copies with its own flags/target.
SHARED_SRCS := kernel/gfx.c kernel/font.c kernel/logo.c

BOOT_SRCS := boot/boot.c $(SHARED_SRCS)
BOOT_OBJS := $(patsubst boot/%.c,$(BUILD)/boot/%.o,$(filter boot/%.c,$(BOOT_SRCS))) \
             $(patsubst kernel/%.c,$(BUILD)/boot/%.o,$(filter kernel/%.c,$(BOOT_SRCS)))

KERNEL_SRCS := kernel/kernel.c kernel/mouse.c kernel/rtc.c $(SHARED_SRCS)
KERNEL_OBJS := $(patsubst kernel/%.c,$(BUILD)/kernel/%.o,$(KERNEL_SRCS))

.PHONY: all clean run

all: $(ISO)/EFI/BOOT/BOOTX64.EFI $(ISO)/KERNEL.BIN

$(BUILD)/boot/%.o: boot/%.c
	@mkdir -p $(BUILD)/boot
	$(CC) $(BOOT_CFLAGS) -o $@ $<

$(BUILD)/boot/%.o: kernel/%.c
	@mkdir -p $(BUILD)/boot
	$(CC) $(BOOT_CFLAGS) -o $@ $<

$(ISO)/EFI/BOOT/BOOTX64.EFI: $(BOOT_OBJS)
	mkdir -p $(ISO)/EFI/BOOT
	$(CC) -target x86_64-unknown-windows -nostdlib -Wl,-entry:efi_main \
	      -Wl,-subsystem:efi_application -fuse-ld=lld -o $@ $(BOOT_OBJS)

$(BUILD)/kernel/%.o: kernel/%.c
	@mkdir -p $(BUILD)/kernel
	$(CC) $(KERNEL_CFLAGS) -o $@ $<

$(BUILD)/kernel.elf: $(KERNEL_OBJS) kernel/kernel.ld
	$(LD) -T kernel/kernel.ld -nostdlib -static -o $@ $(KERNEL_OBJS)

$(ISO)/KERNEL.BIN: $(BUILD)/kernel.elf
	mkdir -p $(ISO)
	$(OBJCOPY) -O binary $< $@

clean:
	rm -rf $(BUILD) $(ISO)

run: all
	mkdir -p $(BUILD)
	cp -n /usr/share/OVMF/OVMF_VARS_4M.fd $(BUILD)/OVMF_VARS.fd 2>/dev/null || true
	qemu-system-x86_64 \
	  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
	  -drive if=pflash,format=raw,file=$(BUILD)/OVMF_VARS.fd \
	  -drive format=raw,file=fat:rw:$(ISO) \
	  -m 256M -serial stdio

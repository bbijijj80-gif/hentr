/* hentrOS bootloader: finds the GOP framebuffer, shows a boot menu (Live
 * from RAM, or Install a copy to a disk), loads KERNEL.BIN from the ESP,
 * exits boot services, and jumps into the kernel. */
#include "efi.h"
#include "bootinfo.h"
#include "../kernel/gfx.h"
#include "../kernel/logo.h"

static EFI_SYSTEM_TABLE *ST;
static EFI_BOOT_SERVICES *BS;

static void puts_(CHAR16 *s) {
    ST->ConOut->OutputString(ST->ConOut, s);
}

typedef void (__attribute__((sysv_abi)) *KernelEntry)(BootInfo *info);

#define COL_BG      0x101010
#define COL_YELLOW  0xF6D51A
#define COL_TEXT    0xE8E8E8
#define COL_HINT    0x8A8A8A
#define COL_OK      0x40C060
#define COL_ERR     0xE05050

static uint32_t screenW, screenH;

static void center_string(int y, const char *s, uint32_t color, int scale) {
    int w = text_width(s, scale);
    draw_string(((int)screenW - w) / 2, y, s, color, scale);
}

/* Blocks until '1' or '2' is pressed; any other key is ignored. */
static char wait_for_choice(void) {
    for (;;) {
        EFI_INPUT_KEY key;
        EFI_STATUS st = ST->ConIn->ReadKeyStroke(ST->ConIn, &key);
        if (st == EFI_SUCCESS) {
            if (key.UnicodeChar == '1' || key.UnicodeChar == '2') return (char)key.UnicodeChar;
        }
        BS->Stall(20000);
    }
}

static void wait_for_any_key(void) {
    for (;;) {
        EFI_INPUT_KEY key;
        if (ST->ConIn->ReadKeyStroke(ST->ConIn, &key) == EFI_SUCCESS) return;
        BS->Stall(20000);
    }
}

static void draw_menu(void) {
    fill_rect(0, 0, screenW, screenH, COL_BG);
    draw_hentros_logo((int)screenW / 2, (int)screenH / 3, 6, COL_YELLOW);

    int y = (int)screenH * 2 / 3;
    center_string(y, "HENTROS", COL_TEXT, 4);
    y += 50;
    center_string(y, "[1] LIVE MODE - RUN FROM RAM, YOUR DISK IS NOT TOUCHED", COL_TEXT, 1);
    y += 20;
    center_string(y, "[2] INSTALL - COPY HENTROS TO A DISK, KEEPS YOUR OTHER OS", COL_TEXT, 1);
    y += 30;
    center_string(y, "PRESS 1 OR 2", COL_HINT, 1);
}

static void status_line(int row, const char *s, uint32_t color) {
    int y = (int)screenH * 2 / 3 + 90 + row * 18;
    fill_rect(0, y, screenW, 18, COL_BG);
    center_string(y, s, color, 1);
}

/* Loads \KERNEL.BIN from the given filesystem root into freshly
 * allocated pages at a fixed load address. Returns the loaded address
 * and size via out-params, or a non-success status on failure. */
static EFI_STATUS load_kernel(EFI_FILE_PROTOCOL *root, EFI_PHYSICAL_ADDRESS *outAddr, UINTN *outSize) {
    EFI_FILE_PROTOCOL *kernelFile = NULL;
    EFI_STATUS status = root->Open(root, &kernelFile, L"\\KERNEL.BIN", EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status)) return status;

    EFI_GUID fiGuid = EFI_FILE_INFO_GUID;
    UINTN infoSize = sizeof(EFI_FILE_INFO) + 16;
    EFI_FILE_INFO *fileInfo = NULL;
    BS->AllocatePool(EfiLoaderData, infoSize, (VOID **)&fileInfo);
    kernelFile->GetInfo(kernelFile, &fiGuid, &infoSize, fileInfo);
    UINTN kernelSize = (UINTN)fileInfo->FileSize;
    BS->FreePool(fileInfo);

    UINTN pages = (kernelSize + 4095) / 4096;
    EFI_PHYSICAL_ADDRESS kernelAddr = 0x200000; /* load kernel at 2 MiB */
    status = BS->AllocatePages(AllocateAddress, EfiLoaderData, pages, &kernelAddr);
    if (EFI_ERROR(status)) { kernelFile->Close(kernelFile); return status; }

    status = kernelFile->Read(kernelFile, &kernelSize, (VOID *)kernelAddr);
    kernelFile->Close(kernelFile);
    if (EFI_ERROR(status)) return status;

    *outAddr = kernelAddr;
    *outSize = kernelSize;
    return EFI_SUCCESS;
}

/* Creates (or opens, if it already exists) a subdirectory without
 * touching anything else already on the volume. */
static EFI_STATUS open_or_create_dir(EFI_FILE_PROTOCOL *parent, CHAR16 *name, EFI_FILE_PROTOCOL **out) {
    return parent->Open(parent, out, name,
                         EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
                         EFI_FILE_DIRECTORY);
}

static EFI_STATUS write_whole_file(EFI_FILE_PROTOCOL *dir, CHAR16 *name, VOID *data, UINTN size) {
    EFI_FILE_PROTOCOL *f = NULL;
    EFI_STATUS status = dir->Open(dir, &f, name,
                                   EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
    if (EFI_ERROR(status)) return status;
    UINTN written = size;
    status = f->Write(f, &written, data);
    f->Close(f);
    return status;
}

/* Copies this running bootloader image plus the already-loaded kernel
 * onto the first other disk volume it can find, under \EFI\HENTROS\ -
 * never touching \EFI\BOOT\ or any file that belongs to whatever OS is
 * already installed there. */
static void do_install(EFI_HANDLE ImageHandle, EFI_LOADED_IMAGE_PROTOCOL *loadedImage,
                        VOID *kernelData, UINTN kernelSize) {
    EFI_GUID fsGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    UINTN count = 0;
    EFI_HANDLE *handles = NULL;
    EFI_STATUS status = BS->LocateHandleBuffer(ByProtocol, &fsGuid, NULL, &count, &handles);
    if (EFI_ERROR(status) || count == 0) {
        status_line(0, "NO DISK VOLUMES FOUND.", COL_ERR);
        status_line(1, "PRESS ANY KEY TO CONTINUE IN LIVE MODE.", COL_HINT);
        wait_for_any_key();
        return;
    }

    EFI_HANDLE target = NULL;
    for (UINTN i = 0; i < count; i++) {
        if (handles[i] != loadedImage->DeviceHandle) { target = handles[i]; break; }
    }
    if (!target && count > 0) target = handles[0]; /* fall back to the boot volume itself */
    BS->FreePool(handles);

    if (!target) {
        status_line(0, "NO SUITABLE DISK VOLUME FOUND.", COL_ERR);
        status_line(1, "PRESS ANY KEY TO CONTINUE IN LIVE MODE.", COL_HINT);
        wait_for_any_key();
        return;
    }

    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
    status = BS->HandleProtocol(target, &fsGuid, (VOID **)&fs);
    EFI_FILE_PROTOCOL *root = NULL;
    if (!EFI_ERROR(status)) status = fs->OpenVolume(fs, &root);
    if (EFI_ERROR(status)) {
        status_line(0, "COULD NOT OPEN THE TARGET DISK VOLUME.", COL_ERR);
        status_line(1, "PRESS ANY KEY TO CONTINUE IN LIVE MODE.", COL_HINT);
        wait_for_any_key();
        return;
    }

    EFI_FILE_PROTOCOL *efiDir = NULL, *hentrosDir = NULL;
    status = open_or_create_dir(root, L"EFI", &efiDir);
    if (!EFI_ERROR(status)) status = open_or_create_dir(efiDir, L"HENTROS", &hentrosDir);
    if (EFI_ERROR(status)) {
        status_line(0, "COULD NOT CREATE \\EFI\\HENTROS ON THAT DISK.", COL_ERR);
        status_line(1, "PRESS ANY KEY TO CONTINUE IN LIVE MODE.", COL_HINT);
        wait_for_any_key();
        return;
    }

    status = write_whole_file(hentrosDir, L"BOOTX64.EFI", loadedImage->ImageBase, loadedImage->ImageSize);
    if (!EFI_ERROR(status))
        status = write_whole_file(hentrosDir, L"KERNEL.BIN", kernelData, kernelSize);

    if (hentrosDir) hentrosDir->Close(hentrosDir);
    if (efiDir) efiDir->Close(efiDir);
    if (root) root->Close(root);

    if (EFI_ERROR(status)) {
        status_line(0, "INSTALL FAILED WHILE WRITING FILES.", COL_ERR);
        status_line(1, "YOUR EXISTING DISK CONTENTS WERE NOT MODIFIED.", COL_HINT);
        status_line(2, "PRESS ANY KEY TO CONTINUE IN LIVE MODE.", COL_HINT);
        wait_for_any_key();
        return;
    }

    status_line(0, "INSTALLED TO \\EFI\\HENTROS\\ ON THE TARGET DISK.", COL_OK);
    status_line(1, "YOUR OTHER OS FILES WERE NOT TOUCHED OR REMOVED.", COL_TEXT);
    status_line(2, "USE YOUR FIRMWARE'S BOOT MENU (E.G. F12 / ESC) TO", COL_HINT);
    status_line(3, "PICK IT LATER. PRESS ANY KEY TO TRY IT NOW.", COL_HINT);
    wait_for_any_key();
    (void)ImageHandle;
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    ST = SystemTable;
    BS = ST->BootServices;

    puts_(L"hentrOS bootloader starting...\r\n");

    /* --- Graphics Output Protocol --- */
    EFI_GUID gopGuid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
    EFI_STATUS status = BS->LocateProtocol(&gopGuid, NULL, (VOID **)&gop);
    if (EFI_ERROR(status) || gop == NULL) {
        puts_(L"GOP not found!\r\n");
        return status;
    }

    /* Pick the highest-resolution 32bpp mode that is still comfortable to
     * push from a CPU-driven, unaccelerated software renderer. */
    #define MAX_PIXELS (1920u * 1200u)
    uint32_t bestMode = gop->Mode->Mode;
    uint32_t bestPixels = gop->Mode->Info->HorizontalResolution * gop->Mode->Info->VerticalResolution;
    if (bestPixels > MAX_PIXELS) bestPixels = 0;
    for (uint32_t i = 0; i < gop->Mode->MaxMode; i++) {
        UINTN sz;
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info;
        if (!EFI_ERROR(gop->QueryMode(gop, i, &sz, &info))) {
            if (info->PixelFormat == PixelRedGreenBlueReserved8BitPerColor ||
                info->PixelFormat == PixelBlueGreenRedReserved8BitPerColor) {
                uint32_t px = info->HorizontalResolution * info->VerticalResolution;
                if (px > bestPixels && px <= MAX_PIXELS) {
                    bestPixels = px;
                    bestMode = i;
                }
            }
        }
    }
    gop->SetMode(gop, bestMode);

    BootInfo binfo;
    binfo.framebuffer = (uint32_t *)gop->Mode->FrameBufferBase;
    binfo.width = gop->Mode->Info->HorizontalResolution;
    binfo.height = gop->Mode->Info->VerticalResolution;
    binfo.pixels_per_scanline = gop->Mode->Info->PixelsPerScanLine;
    screenW = binfo.width;
    screenH = binfo.height;
    gfx_init(&binfo);

    /* --- Locate this image and the volume it booted from --- */
    EFI_GUID liGuid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_LOADED_IMAGE_PROTOCOL *loadedImage = NULL;
    status = BS->HandleProtocol(ImageHandle, &liGuid, (VOID **)&loadedImage);
    if (EFI_ERROR(status)) { puts_(L"LoadedImage failed\r\n"); return status; }

    EFI_GUID fsGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
    status = BS->HandleProtocol(loadedImage->DeviceHandle, &fsGuid, (VOID **)&fs);
    if (EFI_ERROR(status)) { puts_(L"FS protocol failed\r\n"); return status; }

    EFI_FILE_PROTOCOL *root = NULL;
    status = fs->OpenVolume(fs, &root);
    if (EFI_ERROR(status)) { puts_(L"OpenVolume failed\r\n"); return status; }

    /* --- Load the kernel into RAM up front; both modes need it --- */
    EFI_PHYSICAL_ADDRESS kernelAddr;
    UINTN kernelSize;
    status = load_kernel(root, &kernelAddr, &kernelSize);
    if (EFI_ERROR(status)) { puts_(L"KERNEL.BIN load failed\r\n"); return status; }

    /* --- Boot menu: Live (RAM only) or Install (copy to a disk) --- */
    draw_menu();
    char choice = wait_for_choice();
    if (choice == '2') {
        do_install(ImageHandle, loadedImage, (VOID *)kernelAddr, kernelSize);
    }

    draw_menu();
    center_string((int)screenH - 40, "STARTING HENTROS...", COL_HINT, 1);

    /* --- Exit boot services and jump to the kernel --- */
    UINTN mapSize = 0, mapKey, descSize;
    uint32_t descVer;
    EFI_MEMORY_DESCRIPTOR *map = NULL;
    BS->GetMemoryMap(&mapSize, map, &mapKey, &descSize, &descVer);
    mapSize += descSize * 8;
    BS->AllocatePool(EfiLoaderData, mapSize, (VOID **)&map);
    BS->GetMemoryMap(&mapSize, map, &mapKey, &descSize, &descVer);

    status = BS->ExitBootServices(ImageHandle, mapKey);
    if (EFI_ERROR(status)) {
        /* Memory map changed between calls; retry once. */
        mapSize = 0;
        BS->GetMemoryMap(&mapSize, map, &mapKey, &descSize, &descVer);
        mapSize += descSize * 8;
        BS->AllocatePool(EfiLoaderData, mapSize, (VOID **)&map);
        BS->GetMemoryMap(&mapSize, map, &mapKey, &descSize, &descVer);
        BS->ExitBootServices(ImageHandle, mapKey);
    }

    KernelEntry entry = (KernelEntry)kernelAddr;
    entry(&binfo);

    for (;;) { __asm__ volatile("hlt"); }
    return EFI_SUCCESS;
}

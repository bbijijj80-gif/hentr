/* Minimal UEFI bootloader: finds the GOP framebuffer, loads KERNEL.BIN
 * from the ESP, exits boot services, and jumps into the kernel. */
#include "efi.h"
#include "bootinfo.h"

static EFI_SYSTEM_TABLE *ST;

static void puts(CHAR16 *s) {
    ST->ConOut->OutputString(ST->ConOut, s);
}

/* The kernel is a plain ELF->flat binary built with the default System V
 * x86_64 ABI, while this bootloader is compiled for the Windows/PE target
 * (required for EFI), so force sysv_abi on the call itself. */
typedef void (__attribute__((sysv_abi)) *KernelEntry)(BootInfo *info);

EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    ST = SystemTable;
    EFI_BOOT_SERVICES *BS = ST->BootServices;

    puts(L"hentrOS bootloader starting...\r\n");

    /* --- Graphics Output Protocol --- */
    EFI_GUID gopGuid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
    EFI_STATUS status = BS->LocateProtocol(&gopGuid, NULL, (VOID **)&gop);
    if (EFI_ERROR(status) || gop == NULL) {
        puts(L"GOP not found!\r\n");
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

    /* --- Load KERNEL.BIN from the same volume as this loader --- */
    EFI_GUID liGuid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_LOADED_IMAGE_PROTOCOL *loadedImage = NULL;
    status = BS->HandleProtocol(ImageHandle, &liGuid, (VOID **)&loadedImage);
    if (EFI_ERROR(status)) { puts(L"LoadedImage failed\r\n"); return status; }

    EFI_GUID fsGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
    status = BS->HandleProtocol(loadedImage->DeviceHandle, &fsGuid, (VOID **)&fs);
    if (EFI_ERROR(status)) { puts(L"FS protocol failed\r\n"); return status; }

    EFI_FILE_PROTOCOL *root = NULL;
    status = fs->OpenVolume(fs, &root);
    if (EFI_ERROR(status)) { puts(L"OpenVolume failed\r\n"); return status; }

    EFI_FILE_PROTOCOL *kernelFile = NULL;
    status = root->Open(root, &kernelFile, L"\\KERNEL.BIN", EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status)) { puts(L"KERNEL.BIN not found\r\n"); return status; }

    /* Query file size via EFI_FILE_INFO. */
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
    if (EFI_ERROR(status)) { puts(L"AllocatePages failed\r\n"); return status; }

    status = kernelFile->Read(kernelFile, &kernelSize, (VOID *)kernelAddr);
    if (EFI_ERROR(status)) { puts(L"Kernel read failed\r\n"); return status; }
    kernelFile->Close(kernelFile);

    puts(L"Kernel loaded, exiting boot services...\r\n");

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

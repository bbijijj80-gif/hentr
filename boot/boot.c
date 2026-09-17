/* hentrOS: a single UEFI application. It shows a boot menu (Live from
 * RAM, or Install a copy to a disk), then either way runs the whole
 * desktop itself without ever calling ExitBootServices.
 *
 * Staying inside Boot/Runtime Services the whole time is deliberate:
 * it means mouse, keyboard and the clock all go through UEFI's own
 * protocols (EFI_SIMPLE_POINTER_PROTOCOL, EFI_SIMPLE_TEXT_INPUT_PROTOCOL,
 * EFI_RUNTIME_SERVICES.GetTime) instead of us hand-rolling a PS/2 or
 * CMOS driver. UEFI's own driver stack already understands USB HID, so
 * this is what makes the mouse and keyboard work on real, USB-only
 * hardware - polling raw PS/2 ports directly does not. */
#include "efi.h"
#include "bootinfo.h"
#include "../kernel/gfx.h"
#include "../kernel/font.h"
#include "../kernel/logo.h"

static EFI_SYSTEM_TABLE *ST;
static EFI_BOOT_SERVICES *BS;

static void puts_(CHAR16 *s) {
    ST->ConOut->OutputString(ST->ConOut, s);
}

#define COL_BG      0x101010
#define COL_YELLOW  0xF6D51A
#define COL_TEXT    0xE8E8E8
#define COL_HINT    0x8A8A8A
#define COL_OK      0x40C060
#define COL_ERR     0xE05050

#define COL_DESKTOP_TOP    0x2F6FB0
#define COL_DESKTOP_BOTTOM 0x0B3A66
#define COL_TASKBAR        0x1B2733
#define COL_TASKBAR_EDGE   0x3A4A5C
#define COL_START_BTN      0x2E8B57
#define COL_START_BTN_HI   0x3DAF71
#define COL_WINDOW_BODY    0xECECEC
#define COL_TITLEBAR_TOP   0x3E7FCB
#define COL_TITLEBAR_BOT   0x1F4E96
#define COL_CLOSE_BTN      0xE03030
#define COL_TEXT_LIGHT     0xFFFFFF
#define COL_TEXT_DARK      0x222222
#define COL_BORDER         0x0A1A2E
#define COL_MENU_BG        0x263447
#define COL_MENU_HI        0x35516E

static uint32_t screenW, screenH;
static uint32_t *realFb;
static uint32_t realStride;

static void present(void) { gfx_present(realFb, realStride); }

static void center_string(int y, const char *s, uint32_t color, int scale) {
    int w = text_width(s, scale);
    draw_string(((int)screenW - w) / 2, y, s, color, scale);
}

/* ============================== Boot menu ============================== */

static char wait_for_choice(void) {
    for (;;) {
        EFI_INPUT_KEY key;
        if (ST->ConIn->ReadKeyStroke(ST->ConIn, &key) == EFI_SUCCESS) {
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
    present();
}

static void status_line(int row, const char *s, uint32_t color) {
    int y = (int)screenH * 2 / 3 + 90 + row * 18;
    fill_rect(0, y, screenW, 18, COL_BG);
    center_string(y, s, color, 1);
    present();
}

/* ============================== Installer =============================== */

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

/* Copies this running image onto the first other disk volume it can
 * find, under \EFI\HENTROS\ - never touching \EFI\BOOT\ or any file
 * that belongs to whatever OS is already installed there. */
static void do_install(EFI_LOADED_IMAGE_PROTOCOL *loadedImage) {
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
    if (!target && count > 0) target = handles[0];
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
}

/* ============================== Desktop =============================== */

typedef struct { int x, y, w, h, visible; } Window;

static Window win = { .x = 220, .y = 120, .w = 420, .h = 260, .visible = 1 };
static int start_menu_open = 0;
static int dragging = 0;
static int drag_off_x, drag_off_y;
static int mouse_x, mouse_y;
static int taskbarH = 40;

static int rect_hit(int px, int py, int x, int y, int w, int h) {
    return px >= x && px < x + w && py >= y && py < y + h;
}

static void draw_taskbar(void) {
    int ty = (int)screenH - taskbarH;
    fill_rect(0, ty, screenW, taskbarH, COL_TASKBAR);
    draw_hline(0, ty, screenW, COL_TASKBAR_EDGE);

    uint32_t sbtn = start_menu_open ? COL_START_BTN_HI : COL_START_BTN;
    fill_rect(8, ty + 6, 90, taskbarH - 12, sbtn);
    draw_string(8 + 14, ty + 6 + (taskbarH - 12 - FONT_H) / 2, "START", COL_TEXT_LIGHT, 1);

    if (win.visible) {
        fill_rect(110, ty + 6, 150, taskbarH - 12, COL_MENU_HI);
        draw_string(110 + 8, ty + 6 + (taskbarH - 12 - FONT_H) / 2, "HENTROS", COL_TEXT_LIGHT, 1);
    }

    EFI_TIME t;
    char buf[9] = "--:--:--";
    if (ST->RuntimeServices && ST->RuntimeServices->GetTime &&
        ST->RuntimeServices->GetTime(&t, NULL) == EFI_SUCCESS) {
        buf[0] = '0' + t.Hour / 10;   buf[1] = '0' + t.Hour % 10;
        buf[3] = '0' + t.Minute / 10; buf[4] = '0' + t.Minute % 10;
        buf[6] = '0' + t.Second / 10; buf[7] = '0' + t.Second % 10;
    }
    int tw = text_width(buf, 1);
    draw_string((int)screenW - tw - 14, ty + (taskbarH - FONT_H) / 2, buf, COL_TEXT_LIGHT, 1);
}

static void draw_start_menu(void) {
    int mw = 220, mh = 190;
    int mx = 8, my = (int)screenH - taskbarH - mh;
    fill_rect(mx, my, mw, mh, COL_MENU_BG);
    draw_rect(mx, my, mw, mh, COL_TASKBAR_EDGE);
    draw_string(mx + 12, my + 12, "HENTROS MENU", COL_TEXT_LIGHT, 1);
    draw_hline(mx + 8, my + 28, mw - 16, COL_TASKBAR_EDGE);

    const char *items[] = { "SHOW WINDOW", "ABOUT", "REBOOT" };
    for (int i = 0; i < 3; i++) {
        int iy = my + 40 + i * 30;
        if (rect_hit(mouse_x, mouse_y, mx + 6, iy, mw - 12, 26))
            fill_rect(mx + 6, iy, mw - 12, 26, COL_MENU_HI);
        draw_string(mx + 16, iy + 9, items[i], COL_TEXT_LIGHT, 1);
    }
}

static void draw_window(void) {
    if (!win.visible) return;
    int titleH = 28;
    fill_gradient_v(win.x, win.y, win.w, titleH, COL_TITLEBAR_TOP, COL_TITLEBAR_BOT);
    draw_string(win.x + 10, win.y + (titleH - FONT_H) / 2, "WELCOME TO HENTROS", COL_TEXT_LIGHT, 1);

    int cbx = win.x + win.w - 24, cby = win.y + 5, cbs = 18;
    fill_rect(cbx, cby, cbs, cbs, COL_CLOSE_BTN);
    draw_string(cbx + 5, cby + 5, "X", COL_TEXT_LIGHT, 1);

    fill_rect(win.x, win.y + titleH, win.w, win.h - titleH, COL_WINDOW_BODY);
    draw_rect(win.x, win.y, win.w, win.h, COL_BORDER);
    draw_hline(win.x, win.y + titleH, win.w, COL_BORDER);

    draw_string(win.x + 16, win.y + titleH + 20, "A TOY HOBBY OPERATING SYSTEM DEMO.", COL_TEXT_DARK, 1);
    draw_string(win.x + 16, win.y + titleH + 40, "BUILT WITH A UEFI BOOTLOADER AND A", COL_TEXT_DARK, 1);
    draw_string(win.x + 16, win.y + titleH + 60, "FRAMEBUFFER BASED GRAPHICAL SHELL.", COL_TEXT_DARK, 1);
    draw_string(win.x + 16, win.y + titleH + 90, "DRAG THIS TITLE BAR TO MOVE ME.", COL_TEXT_DARK, 1);
}

static void draw_desktop_icons(void) {
    fill_rect(30, 30, 48, 40, 0xD8E4F0);
    draw_rect(30, 30, 48, 40, COL_BORDER);
    fill_rect(38, 38, 32, 20, 0x1B2733);
    draw_string(18, 76, "MY COMPUTER", COL_TEXT_LIGHT, 1);

    draw_hentros_logo((int)screenW - 90, 90, 2, 0xF6D51A);
    draw_string((int)screenW - 130, 140, "HENTROS", COL_TEXT_LIGHT, 1);
}

#define CUR_SZ 16
static void draw_cursor(int x, int y) {
    static const char *shape[CUR_SZ] = {
        "#...............", "##..............", "#.#.............", "#..#............",
        "#...#...........", "#....#..........", "#.....#.........", "#......#........",
        "#.......#.......", "#....#####......", "#..##...........", "#.#.............",
        "##..............", "#...............", "................", "................",
    };
    for (int j = 0; j < CUR_SZ; j++)
        for (int i = 0; i < CUR_SZ; i++)
            if (shape[j][i] == '#') {
                put_pixel(x + i, y + j, 0x000000);
                put_pixel(x + i + 1, y + j, 0xFFFFFF);
            }
}

/* Everything is redrawn into the off-screen buffer every frame, then
 * blitted to the real framebuffer in one shot by present() - simpler
 * and tear-free compared to patching only the changed regions of a
 * live-scanned-out framebuffer. */
static void render_frame(void) {
    fill_gradient_v(0, 0, screenW, screenH - taskbarH, COL_DESKTOP_TOP, COL_DESKTOP_BOTTOM);
    draw_desktop_icons();
    draw_window();
    draw_taskbar();
    if (start_menu_open) draw_start_menu();
    draw_cursor(mouse_x, mouse_y);
    present();
}

/* Some firmware only binds USB HID drivers (mouse/keyboard) lazily, on
 * demand, rather than eagerly at boot. Force the whole driver tree to
 * connect - the same thing the UEFI Shell's "connect -r" does - so a
 * USB mouse's pointer protocol actually shows up before we go looking
 * for it. */
static void connect_all_controllers(void) {
    UINTN count = 0;
    EFI_HANDLE *handles = NULL;
    if (EFI_ERROR(BS->LocateHandleBuffer(AllHandles, NULL, NULL, &count, &handles))) return;
    for (UINTN i = 0; i < count; i++) {
        BS->ConnectController(handles[i], NULL, NULL, TRUE);
    }
    BS->FreePool(handles);
}

#define MAX_POINTERS 8

typedef struct {
    EFI_SIMPLE_POINTER_PROTOCOL *sp[MAX_POINTERS];
    int spDivisor[MAX_POINTERS];
    int spCount;
    EFI_ABSOLUTE_POINTER_PROTOCOL *ap[MAX_POINTERS];
    int apCount;
} PointerSet;

/* Different motherboards/firmware expose pointer devices very
 * differently: some publish EFI_SIMPLE_POINTER_PROTOCOL for a PS/2
 * mouse instantly, some only after USB has enumerated (which can take
 * a couple of seconds on real hardware with hubs/dongles), some only
 * ever expose EFI_ABSOLUTE_POINTER_PROTOCOL, and a few publish more
 * than one instance (e.g. a touchpad AND a USB mouse). Rather than
 * grabbing whatever LocateProtocol hands back first, enumerate every
 * handle for both protocols and poll all of them. */
static void find_pointers(PointerSet *out) {
    out->spCount = 0;
    out->apCount = 0;

    EFI_GUID spGuid = EFI_SIMPLE_POINTER_PROTOCOL_GUID;
    UINTN n = 0; EFI_HANDLE *h = NULL;
    if (!EFI_ERROR(BS->LocateHandleBuffer(ByProtocol, &spGuid, NULL, &n, &h))) {
        for (UINTN i = 0; i < n && out->spCount < MAX_POINTERS; i++) {
            EFI_SIMPLE_POINTER_PROTOCOL *p = NULL;
            if (!EFI_ERROR(BS->HandleProtocol(h[i], &spGuid, (VOID **)&p)) && p) {
                out->sp[out->spCount] = p;
                int div = 1;
                if (p->Mode && p->Mode->ResolutionX > 1000) div = (int)(p->Mode->ResolutionX / 1000);
                out->spDivisor[out->spCount] = div;
                out->spCount++;
            }
        }
        if (h) BS->FreePool(h);
    }

    EFI_GUID apGuid = EFI_ABSOLUTE_POINTER_PROTOCOL_GUID;
    n = 0; h = NULL;
    if (!EFI_ERROR(BS->LocateHandleBuffer(ByProtocol, &apGuid, NULL, &n, &h))) {
        for (UINTN i = 0; i < n && out->apCount < MAX_POINTERS; i++) {
            EFI_ABSOLUTE_POINTER_PROTOCOL *p = NULL;
            if (!EFI_ERROR(BS->HandleProtocol(h[i], &apGuid, (VOID **)&p)) && p) {
                out->ap[out->apCount++] = p;
            }
        }
        if (h) BS->FreePool(h);
    }
}

static void desktop_loop(void) {
    PointerSet ptrs;
    ptrs.spCount = 0;
    ptrs.apCount = 0;

    /* USB enumeration can take a couple of seconds on real hardware
     * (hubs, wireless dongles, slow devices), so keep retrying instead
     * of giving up after a single check. */
    for (int attempt = 0; attempt < 8; attempt++) {
        connect_all_controllers();
        find_pointers(&ptrs);
        if (ptrs.spCount > 0 || ptrs.apCount > 0) break;
        BS->Stall(250000);
    }

    mouse_x = (int)screenW / 2;
    mouse_y = (int)screenH / 2;
    int left_prev = 0;
    uint32_t rescanCounter = 0;

    render_frame();

    for (;;) {
        int left_now = left_prev;

        /* If no pointer device was found yet, keep periodically
         * re-checking - it may appear late (slow enumeration) or get
         * hot-plugged while the desktop is already running. */
        if (ptrs.spCount == 0 && ptrs.apCount == 0) {
            rescanCounter++;
            if (rescanCounter >= 100) { /* roughly once a second at the 10ms frame stall below */
                rescanCounter = 0;
                connect_all_controllers();
                find_pointers(&ptrs);
            }
        }

        for (int i = 0; i < ptrs.spCount; i++) {
            EFI_SIMPLE_POINTER_STATE st;
            if (ptrs.sp[i]->GetState(ptrs.sp[i], &st) == EFI_SUCCESS) {
                int dx = st.RelativeMovementX / ptrs.spDivisor[i];
                int dy = st.RelativeMovementY / ptrs.spDivisor[i];
                if (dx > 60) dx = 60; if (dx < -60) dx = -60;
                if (dy > 60) dy = 60; if (dy < -60) dy = -60;
                mouse_x += dx;
                mouse_y += dy;
                if (st.LeftButton) left_now = 1;
            }
        }
        for (int i = 0; i < ptrs.apCount; i++) {
            EFI_ABSOLUTE_POINTER_STATE st;
            if (ptrs.ap[i]->GetState(ptrs.ap[i], &st) == EFI_SUCCESS) {
                EFI_ABSOLUTE_POINTER_MODE *m = ptrs.ap[i]->Mode;
                uint64_t rangeX = m->AbsoluteMaxX - m->AbsoluteMinX;
                uint64_t rangeY = m->AbsoluteMaxY - m->AbsoluteMinY;
                if (rangeX > 0) mouse_x = (int)(((st.CurrentX - m->AbsoluteMinX) * screenW) / rangeX);
                if (rangeY > 0) mouse_y = (int)(((st.CurrentY - m->AbsoluteMinY) * screenH) / rangeY);
                if (st.ActiveButtons & EFI_ABSOLUTE_POINTER_TOUCH_ACTIVE) left_now = 1;
            }
        }

        /* Keyboard is always available (arrow keys move the cursor,
         * Enter clicks) so the desktop stays usable even without a
         * working pointer device. */
        EFI_INPUT_KEY key;
        while (ST->ConIn->ReadKeyStroke(ST->ConIn, &key) == EFI_SUCCESS) {
            switch (key.ScanCode) {
                case 1: mouse_y -= 10; break; /* up */
                case 2: mouse_y += 10; break; /* down */
                case 3: mouse_x += 10; break; /* right */
                case 4: mouse_x -= 10; break; /* left */
            }
            if (key.UnicodeChar == 13 || key.UnicodeChar == ' ') left_now = 1;
        }

        if (mouse_x < 0) mouse_x = 0;
        if (mouse_y < 0) mouse_y = 0;
        if ((uint32_t)mouse_x > screenW - 2) mouse_x = screenW - 2;
        if ((uint32_t)mouse_y > screenH - 2) mouse_y = screenH - 2;

        int left_click = left_now && !left_prev;
        int left_release = !left_now && left_prev;

        if (left_click) {
            int ty = (int)screenH - taskbarH;
            if (rect_hit(mouse_x, mouse_y, 8, ty + 6, 90, taskbarH - 12)) {
                start_menu_open = !start_menu_open;
            } else if (start_menu_open) {
                int mw = 220, mh = 190, mx = 8, my = (int)screenH - taskbarH - mh;
                if (rect_hit(mouse_x, mouse_y, mx, my, mw, mh)) {
                    for (int i = 0; i < 3; i++) {
                        int iy = my + 40 + i * 30;
                        if (rect_hit(mouse_x, mouse_y, mx + 6, iy, mw - 12, 26)) {
                            if (i == 0) win.visible = 1;
                            start_menu_open = 0;
                        }
                    }
                } else {
                    start_menu_open = 0;
                }
            } else if (win.visible) {
                int cbx = win.x + win.w - 24, cby = win.y + 5, cbs = 18;
                if (rect_hit(mouse_x, mouse_y, cbx, cby, cbs, cbs)) {
                    win.visible = 0;
                } else if (rect_hit(mouse_x, mouse_y, win.x, win.y, win.w, 28)) {
                    dragging = 1;
                    drag_off_x = mouse_x - win.x;
                    drag_off_y = mouse_y - win.y;
                }
            }
        }
        if (left_release) dragging = 0;

        if (dragging && left_now) {
            win.x = mouse_x - drag_off_x;
            win.y = mouse_y - drag_off_y;
            if (win.x < 0) win.x = 0;
            if (win.y < 0) win.y = 0;
            if (win.x + win.w > (int)screenW) win.x = (int)screenW - win.w;
            if (win.y + win.h > (int)screenH - taskbarH) win.y = (int)screenH - taskbarH - win.h;
        }

        left_prev = left_now;

        render_frame();
        BS->Stall(10000); /* ~100 fps cap; keeps this from pegging a CPU core */
    }
}

/* ============================== Entry point ============================= */

EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    ST = SystemTable;
    BS = ST->BootServices;

    puts_(L"hentrOS starting...\r\n");

    EFI_GUID gopGuid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
    EFI_STATUS status = BS->LocateProtocol(&gopGuid, NULL, (VOID **)&gop);
    if (EFI_ERROR(status) || gop == NULL) {
        puts_(L"GOP not found!\r\n");
        return status;
    }

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

    screenW = gop->Mode->Info->HorizontalResolution;
    screenH = gop->Mode->Info->VerticalResolution;
    realFb = (uint32_t *)gop->Mode->FrameBufferBase;
    realStride = gop->Mode->Info->PixelsPerScanLine;

    /* Draw into an off-screen buffer, tightly packed (stride == width),
     * and only ever touch the real, live-scanned-out framebuffer with a
     * single fast blit in present(). This is what stops the tearing /
     * flicker you get from painting shapes directly onto a framebuffer
     * the display is simultaneously reading from. */
    VOID *backbuf = NULL;
    UINTN backbufSize = (UINTN)screenW * screenH * 4;
    status = BS->AllocatePool(EfiLoaderData, backbufSize, &backbuf);
    if (EFI_ERROR(status)) { puts_(L"Backbuffer allocation failed\r\n"); return status; }

    BootInfo binfo;
    binfo.framebuffer = (uint32_t *)backbuf;
    binfo.width = screenW;
    binfo.height = screenH;
    binfo.pixels_per_scanline = screenW;
    gfx_init(&binfo);

    EFI_GUID liGuid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_LOADED_IMAGE_PROTOCOL *loadedImage = NULL;
    BS->HandleProtocol(ImageHandle, &liGuid, (VOID **)&loadedImage);

    draw_menu();
    char choice = wait_for_choice();
    if (choice == '2' && loadedImage) {
        do_install(loadedImage);
    }

    desktop_loop(); /* never returns */
    return EFI_SUCCESS;
}

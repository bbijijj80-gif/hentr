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

/* ---- On-screen diagnostics ----
 * Different boards behave differently enough (keyboard layout quirks,
 * USB enumeration timing, firmware that never exposes a pointer
 * protocol at all) that guessing blind from reports like "it doesn't
 * work" stops being productive. This puts the actual live state on
 * screen - no serial cable or debug build needed - so it's visible in
 * a photo of the screen: how many pointer devices were found, and the
 * raw scan code / character of the last key that was actually
 * received, which tells us immediately whether input is reaching the
 * code at all and, if so, what a given key actually reports as. */
static int g_pointerCount = -1; /* -1 = not scanned yet */
static int g_lastScan = -1, g_lastUnicode = -1;
static const char *g_pointerKind = "NONE";
static int g_pollTotal = 0, g_pollSuccess = 0;
static int g_lastRawDx = 0, g_lastRawDy = 0, g_lastDivisor = 1, g_lastBtn = 0;
static int g_xhciFound = 0;
static uint64_t g_xhciMmioBase = 0;
static int g_pciHandleCount = -1;
static uint32_t g_pciLastClassReg = 0;

static void itoa10(int v, char *out) {
    char tmp[12]; int i = 0; int neg = v < 0; if (neg) v = -v;
    if (v == 0) tmp[i++] = '0';
    while (v > 0) { tmp[i++] = (char)('0' + (v % 10)); v /= 10; }
    int j = 0;
    if (neg) out[j++] = '-';
    while (i > 0) out[j++] = tmp[--i];
    out[j] = 0;
}

static void strcat_local(char *dst, const char *src) {
    while (*dst) dst++;
    while (*src) *dst++ = *src++;
    *dst = 0;
}

static void draw_diagnostics(void) {
    char line[80]; char num[12];

    line[0] = 0;
    strcat_local(line, "MOUSE: ");
    if (g_pointerCount < 0) {
        strcat_local(line, "SEARCHING...");
    } else {
        itoa10(g_pointerCount, num);
        strcat_local(line, num);
        strcat_local(line, g_pointerCount == 0 ? " FOUND (USE ARROWS+ENTER)" : " FOUND");
    }
    draw_string(6, 4, line, 0xFFFF40, 1);

    line[0] = 0;
    strcat_local(line, "LAST KEY: SCAN=");
    itoa10(g_lastScan, num); strcat_local(line, num);
    strcat_local(line, " CHAR=");
    itoa10(g_lastUnicode, num); strcat_local(line, num);
    draw_string(6, 4 + FONT_H + 3, line, 0xFFFF40, 1);

    line[0] = 0;
    strcat_local(line, "TYPE=");
    strcat_local(line, g_pointerKind);
    strcat_local(line, " POLLS=");
    itoa10(g_pollSuccess, num); strcat_local(line, num);
    strcat_local(line, "/");
    itoa10(g_pollTotal, num); strcat_local(line, num);
    draw_string(6, 4 + 2 * (FONT_H + 3), line, 0xFFFF40, 1);

    line[0] = 0;
    strcat_local(line, "RAWDX=");
    itoa10(g_lastRawDx, num); strcat_local(line, num);
    strcat_local(line, " RAWDY=");
    itoa10(g_lastRawDy, num); strcat_local(line, num);
    strcat_local(line, " DIV=");
    itoa10(g_lastDivisor, num); strcat_local(line, num);
    strcat_local(line, " BTN=");
    itoa10(g_lastBtn, num); strcat_local(line, num);
    draw_string(6, 4 + 3 * (FONT_H + 3), line, 0xFFFF40, 1);

    line[0] = 0;
    strcat_local(line, "XHCI: ");
    strcat_local(line, g_xhciFound ? "FOUND" : "NOT FOUND");
    strcat_local(line, " PCIHANDLES=");
    itoa10(g_pciHandleCount, num); strcat_local(line, num);
    strcat_local(line, " LASTCLASS=");
    itoa10((int)g_pciLastClassReg, num); strcat_local(line, num);
    draw_string(6, 4 + 4 * (FONT_H + 3), line, 0xFFFF40, 1);
}

/* ============================== Boot menu ============================== */

static void wait_for_any_key(void) {
    for (;;) {
        EFI_INPUT_KEY key;
        if (ST->ConIn->ReadKeyStroke(ST->ConIn, &key) == EFI_SUCCESS) return;
        BS->Stall(20000);
    }
}

/* selected: 0 for option 1, 1 for option 2. phase1000 drives the logo's
 * dissolve/reassemble animation: 0 = scattered into a cloud of dots,
 * 1000 = fully assembled. */
static void draw_menu(int selected, const LogoPoint *logoPts, int logoPtCount, int phase1000) {
    fill_rect(0, 0, screenW, screenH, COL_BG);
    draw_logo_particles(logoPts, logoPtCount, (int)screenW / 2, (int)screenH / 3, phase1000, COL_YELLOW);

    int y = (int)screenH * 2 / 3;
    center_string(y, "HENTROS", COL_TEXT, 4);
    y += 50;

    const char *opt1 = "[1] LIVE MODE - RUN FROM RAM, YOUR DISK IS NOT TOUCHED";
    const char *opt2 = "[2] INSTALL - COPY HENTROS TO A DISK, KEEPS YOUR OTHER OS";
    int w1 = text_width(opt1, 1), w2 = text_width(opt2, 1);
    if (selected == 0) fill_rect(((int)screenW - w1) / 2 - 6, y - 2, w1 + 12, FONT_H + 4, COL_MENU_HI);
    center_string(y, opt1, COL_TEXT, 1);
    y += 20;
    if (selected == 1) fill_rect(((int)screenW - w2) / 2 - 6, y - 2, w2 + 12, FONT_H + 4, COL_MENU_HI);
    center_string(y, opt2, COL_TEXT, 1);
    y += 30;
    center_string(y, "PRESS 1 OR 2, OR USE ARROW KEYS + ENTER", COL_HINT, 1);
    draw_diagnostics();
    present();
}

/* Trapezoid wave: ramps 0 -> 1000 over `rampFrames`, holds at 1000 for
 * `holdFrames` (so the assembled logo stays put long enough to actually
 * look at), then ramps back down to 0 over `rampFrames` and repeats -
 * the same "the mark is made of many small points, coming together"
 * motion as Claude's own loading animation, just paused at the top. */
static int trapezoid_wave1000(uint32_t frame, uint32_t rampFrames, uint32_t holdFrames) {
    uint32_t period = 2 * rampFrames + holdFrames;
    uint32_t t = frame % period;
    if (t < rampFrames) return (int)(t * 1000 / rampFrames);
    if (t < rampFrames + holdFrames) return 1000;
    uint32_t td = t - rampFrames - holdFrames;
    return (int)(1000 - td * 1000 / rampFrames);
}

/* Accepts either a direct digit press or arrow-key navigation confirmed
 * with Enter/Space, since keyboards and firmware vary enough that one
 * single input method isn't reliably enough on every board. */
static char wait_for_choice(void) {
    int selected = 0;
    static LogoPoint logoPts[1300];
    int logoPtCount = build_hentros_logo_points((int)screenW / 2, (int)screenH / 3, 6, logoPts, 1300);

    uint32_t frame = 0;
    for (;;) {
        EFI_INPUT_KEY key;
        if (ST->ConIn->ReadKeyStroke(ST->ConIn, &key) == EFI_SUCCESS) {
            g_lastScan = key.ScanCode;
            g_lastUnicode = (int)key.UnicodeChar;
            if (key.UnicodeChar == '1' || key.UnicodeChar == '2') return (char)key.UnicodeChar;
            if (key.ScanCode == 1 || key.ScanCode == 4) selected = 0; /* up/left */
            if (key.ScanCode == 2 || key.ScanCode == 3) selected = 1; /* down/right */
            if (key.UnicodeChar == 13 || key.UnicodeChar == ' ') return selected == 0 ? '1' : '2';
        }
        int phase = trapezoid_wave1000(frame, 70, 180); /* ~1.2s to assemble, ~3s held, ~1.2s to scatter, at 60fps */
        draw_menu(selected, logoPts, logoPtCount, phase);
        frame++;
        BS->Stall(16000); /* ~60 fps-ish frame pacing */
    }
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
static int about_open = 0;
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

static void draw_about(void) {
    int w = 300, h = 120;
    int x = ((int)screenW - w) / 2, y = ((int)screenH - h) / 2;
    fill_rect(x, y, w, h, COL_MENU_BG);
    draw_rect(x, y, w, h, COL_TEXT_LIGHT);
    draw_string(x + 14, y + 14, "HENTROS - TOY HOBBY OS", COL_TEXT_LIGHT, 1);
    draw_string(x + 14, y + 40, "A UEFI APPLICATION, NOT A REAL OS.", COL_HINT, 1);
    draw_string(x + 14, y + 58, "WRITTEN FROM SCRATCH IN C.", COL_HINT, 1);
    draw_string(x + 14, y + 90, "CLICK ANYWHERE TO CLOSE.", COL_TEXT_LIGHT, 1);
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
    if (about_open) draw_about();
    draw_diagnostics();
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

/* ---- xHCI discovery (stage 1 of a from-scratch USB 3 host controller
 * driver) ----
 * UEFI's own pointer protocols have proven unreliable on some real
 * boards (a "found" EFI_ABSOLUTE_POINTER_PROTOCOL instance that never
 * once returns real movement). The plan is to eventually bypass UEFI's
 * USB stack entirely and talk to the xHCI controller's registers
 * directly. Step one is just finding it: walk PCI config space via
 * EFI_PCI_IO_PROTOCOL looking for the standard xHCI class code
 * (base class 0x0C serial bus, sub class 0x03 USB, prog-if 0x30 XHCI),
 * then read its 64-bit MMIO BAR (BAR0/BAR1) so later stages have an
 * address to map registers at. UEFI identity-maps physical memory, so
 * the BAR's physical address can be used directly as a pointer once
 * boot services are still active. */
static int find_xhci_controller(uint64_t *mmioBaseOut) {
    EFI_GUID pciIoGuid = EFI_PCI_IO_PROTOCOL_GUID;
    UINTN n = 0; EFI_HANDLE *h = NULL;
    if (EFI_ERROR(BS->LocateHandleBuffer(ByProtocol, &pciIoGuid, NULL, &n, &h))) { g_pciHandleCount = -2; return 0; }
    g_pciHandleCount = (int)n;

    int found = 0;
    for (UINTN i = 0; i < n; i++) {
        EFI_PCI_IO_PROTOCOL *pci = NULL;
        if (EFI_ERROR(BS->HandleProtocol(h[i], &pciIoGuid, (VOID **)&pci)) || !pci) continue;

        /* Read the class-code/revision dword at offset 0x08: byte 0 is
         * RevisionID, byte 1 ProgIF, byte 2 SubClass, byte 3 BaseClass. */
        uint32_t classReg = 0;
        if (EFI_ERROR(pci->Pci.Read(pci, EfiPciIoWidthUint32, 0x08, 1, &classReg))) continue;
        g_pciLastClassReg = classReg;
        uint8_t progIf = (uint8_t)(classReg >> 8);
        uint8_t subClass = (uint8_t)(classReg >> 16);
        uint8_t baseClass = (uint8_t)(classReg >> 24);
        if (baseClass != 0x0C || subClass != 0x03 || progIf != 0x30) continue;

        uint32_t bar0 = 0, bar1 = 0;
        if (EFI_ERROR(pci->Pci.Read(pci, EfiPciIoWidthUint32, PCI_CONFIG_OFFSET_BAR0, 1, &bar0))) continue;
        /* Bit 2 of a memory BAR's low dword set means it's 64-bit and
         * BAR1 holds the upper 32 bits; xHCI BARs are always memory
         * BARs, but check anyway rather than assume. */
        uint64_t base = bar0 & ~0xFULL;
        if ((bar0 & 0x6) == 0x4) {
            if (!EFI_ERROR(pci->Pci.Read(pci, EfiPciIoWidthUint32, PCI_CONFIG_OFFSET_BAR1, 1, &bar1))) {
                base |= ((uint64_t)bar1) << 32;
            }
        }
        if (base == 0) continue;

        *mmioBaseOut = base;
        found = 1;
        break;
    }

    if (h) BS->FreePool(h);
    return found;
}

#define MAX_POINTERS 8

typedef struct {
    EFI_USB_IO_PROTOCOL *io;
    uint8_t ep;
} RawHidMouse;

typedef struct {
    EFI_SIMPLE_POINTER_PROTOCOL *sp[MAX_POINTERS];
    int spDivisor[MAX_POINTERS];
    int spCount;
    EFI_ABSOLUTE_POINTER_PROTOCOL *ap[MAX_POINTERS];
    int apCount;
    RawHidMouse raw[MAX_POINTERS];
    int rawCount;
} PointerSet;

/* Last-resort fallback: some firmware's USB stack enumerates a mouse
 * (EFI_USB_IO_PROTOCOL binds to it) but never loads a HID class driver
 * on top, so neither pointer protocol above ever appears for it - even
 * though the OS's own drivers (Windows, Linux) see the same mouse just
 * fine once they take over. When that happens, talk to the mouse's USB
 * HID Boot Protocol interface directly: find its interrupt-IN endpoint
 * and read raw 3-byte boot mouse reports (buttons, dx, dy) ourselves. */
static void find_raw_hid_mice(PointerSet *out) {
    out->rawCount = 0;
    EFI_GUID usbIoGuid = EFI_USB_IO_PROTOCOL_GUID;
    UINTN n = 0; EFI_HANDLE *h = NULL;
    if (EFI_ERROR(BS->LocateHandleBuffer(ByProtocol, &usbIoGuid, NULL, &n, &h))) return;
    for (UINTN i = 0; i < n && out->rawCount < MAX_POINTERS; i++) {
        EFI_USB_IO_PROTOCOL *io = NULL;
        if (EFI_ERROR(BS->HandleProtocol(h[i], &usbIoGuid, (VOID **)&io)) || !io) continue;
        EFI_USB_INTERFACE_DESCRIPTOR iface;
        if (!io->UsbGetInterfaceDescriptor || EFI_ERROR(io->UsbGetInterfaceDescriptor(io, &iface))) continue;
        if (iface.InterfaceClass != USB_HID_CLASS || iface.InterfaceProtocol != USB_HID_PROTOCOL_MOUSE) continue;
        for (uint8_t e = 0; e < iface.NumEndpoints; e++) {
            EFI_USB_ENDPOINT_DESCRIPTOR ep;
            if (!io->UsbGetEndpointDescriptor || EFI_ERROR(io->UsbGetEndpointDescriptor(io, e, &ep))) continue;
            if ((ep.Attributes & USB_ENDPOINT_TYPE_MASK) == USB_ENDPOINT_TYPE_INTERRUPT &&
                (ep.EndpointAddress & USB_ENDPOINT_DIR_IN)) {
                out->raw[out->rawCount].io = io;
                out->raw[out->rawCount].ep = ep.EndpointAddress;
                out->rawCount++;
                break;
            }
        }
    }
    if (h) BS->FreePool(h);
}

/* Different motherboards/firmware expose pointer devices very
 * differently: some publish EFI_SIMPLE_POINTER_PROTOCOL for a PS/2
 * mouse instantly, some only after USB has enumerated (which can take
 * a couple of seconds on real hardware with hubs/dongles), some only
 * ever expose EFI_ABSOLUTE_POINTER_PROTOCOL, and a few publish more
 * than one instance (e.g. a touchpad AND a USB mouse). Rather than
 * grabbing whatever LocateProtocol hands back first, enumerate every
 * handle for both protocols and poll all of them. If neither is found
 * at all, fall back to driving the raw USB HID interface ourselves. */
static void find_pointers(PointerSet *out) {
    out->spCount = 0;
    out->apCount = 0;
    out->rawCount = 0;

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

    if (out->spCount == 0 && out->apCount == 0) {
        find_raw_hid_mice(out);
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
        g_pointerCount = ptrs.spCount + ptrs.apCount + ptrs.rawCount;
        if (ptrs.spCount > 0 || ptrs.apCount > 0) break;
        BS->Stall(250000);
    }

    g_xhciFound = find_xhci_controller(&g_xhciMmioBase);

    mouse_x = (int)screenW / 2;
    mouse_y = (int)screenH / 2;
    int left_prev = 0;
    uint32_t rescanCounter = 0;
    int stuckFallbackTried = 0;

    render_frame();

    for (;;) {
        /* left_now tracks the pointer device's real, level-based button
         * state (down for as long as the device reports it down) and is
         * what persists into left_prev below. kbClick is a one-shot
         * pulse: UEFI's keyboard protocol only ever delivers a press
         * event, never a matching release, so if Enter/Space were
         * merged into the same persisted state the very first press
         * would latch it "held" forever and no click - keyboard or
         * mouse - would ever register again afterwards. */
        int left_now = left_prev;
        int kbClick = 0;

        /* If no pointer device was found yet, keep periodically
         * re-checking - it may appear late (slow enumeration) or get
         * hot-plugged while the desktop is already running. */
        if (ptrs.spCount == 0 && ptrs.apCount == 0) {
            rescanCounter++;
            if (rescanCounter >= 100) { /* roughly once a second at the 10ms frame stall below */
                rescanCounter = 0;
                connect_all_controllers();
                find_pointers(&ptrs);
                g_pointerCount = ptrs.spCount + ptrs.apCount + ptrs.rawCount;
            }
        }

        /* A pointer protocol can be "found" but dead - registered by
         * firmware without ever being wired to real hardware, so
         * GetState never once returns success no matter how much the
         * mouse actually moves. If that's what happened, stop trusting
         * it and go straight for the raw USB HID fallback instead. */
        if (!stuckFallbackTried && ptrs.rawCount == 0 && ptrs.apCount > 0 &&
            g_pollTotal >= 150 && g_pollSuccess == 0) {
            stuckFallbackTried = 1;
            find_raw_hid_mice(&ptrs);
            g_pollTotal = 0;
            g_pollSuccess = 0;
            g_pointerCount = ptrs.spCount + ptrs.apCount + ptrs.rawCount;
        }

        for (int i = 0; i < ptrs.spCount; i++) {
            g_pointerKind = "SP"; g_pollTotal++;
            EFI_SIMPLE_POINTER_STATE st;
            if (ptrs.sp[i]->GetState(ptrs.sp[i], &st) == EFI_SUCCESS) {
                g_pollSuccess++;
                g_lastRawDx = (int)st.RelativeMovementX;
                g_lastRawDy = (int)st.RelativeMovementY;
                g_lastDivisor = ptrs.spDivisor[i];
                g_lastBtn = st.LeftButton;
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
            g_pointerKind = "AP"; g_pollTotal++;
            EFI_ABSOLUTE_POINTER_STATE st;
            if (ptrs.ap[i]->GetState(ptrs.ap[i], &st) == EFI_SUCCESS) {
                g_pollSuccess++;
                EFI_ABSOLUTE_POINTER_MODE *m = ptrs.ap[i]->Mode;
                uint64_t rangeX = m->AbsoluteMaxX - m->AbsoluteMinX;
                uint64_t rangeY = m->AbsoluteMaxY - m->AbsoluteMinY;
                g_lastRawDx = (int)st.CurrentX;
                g_lastRawDy = (int)st.CurrentY;
                g_lastDivisor = (int)rangeX;
                g_lastBtn = (st.ActiveButtons & EFI_ABSOLUTE_POINTER_TOUCH_ACTIVE) != 0;
                if (rangeX > 0) mouse_x = (int)(((st.CurrentX - m->AbsoluteMinX) * screenW) / rangeX);
                if (rangeY > 0) mouse_y = (int)(((st.CurrentY - m->AbsoluteMinY) * screenH) / rangeY);
                if (st.ActiveButtons & EFI_ABSOLUTE_POINTER_TOUCH_ACTIVE) left_now = 1;
            }
        }
        for (int i = 0; i < ptrs.rawCount; i++) {
            g_pointerKind = "RAW"; g_pollTotal++;
            uint8_t buf[8];
            UINTN len = 4;
            uint32_t xferStatus = 0;
            EFI_USB_IO_PROTOCOL *io = ptrs.raw[i].io;
            if (io->UsbSyncInterruptTransfer(io, ptrs.raw[i].ep, buf, &len, 1, &xferStatus) == EFI_SUCCESS && len >= 3) {
                g_pollSuccess++;
                int dx = (int8_t)buf[1];
                int dy = (int8_t)buf[2];
                g_lastRawDx = dx; g_lastRawDy = dy; g_lastDivisor = 1; g_lastBtn = buf[0] & 0x01;
                if (dx > 60) dx = 60; if (dx < -60) dx = -60;
                if (dy > 60) dy = 60; if (dy < -60) dy = -60;
                mouse_x += dx;
                mouse_y += dy;
                if (buf[0] & 0x01) left_now = 1;
            }
        }

        /* Keyboard is always available (arrow keys move the cursor,
         * Enter/Space clicks) so the desktop stays usable even without
         * a working pointer device. */
        EFI_INPUT_KEY key;
        while (ST->ConIn->ReadKeyStroke(ST->ConIn, &key) == EFI_SUCCESS) {
            g_lastScan = key.ScanCode;
            g_lastUnicode = (int)key.UnicodeChar;
            switch (key.ScanCode) {
                case 1: mouse_y -= 10; break; /* up */
                case 2: mouse_y += 10; break; /* down */
                case 3: mouse_x += 10; break; /* right */
                case 4: mouse_x -= 10; break; /* left */
            }
            if (key.UnicodeChar == 13 || key.UnicodeChar == ' ') kbClick = 1;
        }

        if (mouse_x < 0) mouse_x = 0;
        if (mouse_y < 0) mouse_y = 0;
        if ((uint32_t)mouse_x > screenW - 2) mouse_x = screenW - 2;
        if ((uint32_t)mouse_y > screenH - 2) mouse_y = screenH - 2;

        int left_click = (left_now && !left_prev) || kbClick;
        int left_release = !left_now && left_prev;

        if (left_click) {
            int ty = (int)screenH - taskbarH;
            if (about_open) {
                about_open = 0; /* any click anywhere closes it */
            } else if (rect_hit(mouse_x, mouse_y, 8, ty + 6, 90, taskbarH - 12)) {
                start_menu_open = !start_menu_open;
            } else if (start_menu_open) {
                int mw = 220, mh = 190, mx = 8, my = (int)screenH - taskbarH - mh;
                if (rect_hit(mouse_x, mouse_y, mx, my, mw, mh)) {
                    for (int i = 0; i < 3; i++) {
                        int iy = my + 40 + i * 30;
                        if (rect_hit(mouse_x, mouse_y, mx + 6, iy, mw - 12, 26)) {
                            if (i == 0) { win.visible = 1; win.x = 220; win.y = 120; } /* SHOW WINDOW: snap it back so clicking is visible even if it was already open */
                            if (i == 1) about_open = 1; /* ABOUT */
                            if (i == 2 && ST->RuntimeServices && ST->RuntimeServices->ResetSystem) {
                                ST->RuntimeServices->ResetSystem(EfiResetCold, EFI_SUCCESS, 0, NULL); /* REBOOT */
                            }
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

    char choice = wait_for_choice();
    if (choice == '2' && loadedImage) {
        do_install(loadedImage);
    }

    desktop_loop(); /* never returns */
    return EFI_SUCCESS;
}

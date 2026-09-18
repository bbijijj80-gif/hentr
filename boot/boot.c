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

/* -ffreestanding means no libc, but clang still emits calls to memcpy
 * for things like struct assignment - provide the one symbol it needs. */
void *memcpy(void *dst, const void *src, UINTN n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (UINTN i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

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
static int g_xhciInitOk = 0;
static uint32_t g_xhciMaxSlots = 0, g_xhciMaxPorts = 0;
static uint32_t g_xhciUsbStsBefore = 0xFFFFFFFF, g_xhciUsbStsAfter = 0xFFFFFFFF;
static int g_xhciResetTimedOut = 0, g_xhciStartTimedOut = 0;
static int g_xhciMouseActive = 0;
static uint32_t g_xhciMousePort = 0, g_xhciMouseSpeed = 0, g_xhciMouseSlot = 0;
static uint32_t g_xhciAnyEventCount = 0;
static uint32_t g_xhciMouseEpAddr = 0, g_xhciMouseInterval = 0, g_xhciMouseMaxPacket = 0, g_xhciMouseIsHid = 0;
static uint32_t g_xhciMouseArmCount = 0, g_xhciMouseEvtCount = 0;
static uint32_t g_xhciLastEvtType = 0, g_xhciLastEvtCC = 0;
static uint32_t g_xhciCfgEpAttempted = 0, g_xhciCfgEpGotEvt = 0, g_xhciCfgEpCC = 0xFF;
static int g_xhciCfgEpAllocIntrFailed = 0, g_xhciCfgEpAllocBufFailed = 0, g_xhciCfgEpEntered = 0;
static uint32_t g_xhciOutSlotState = 0xFF, g_xhciOutEpState = 0xFF, g_xhciPortscSnapshot = 0;

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

    line[0] = 0;
    strcat_local(line, "XHCIINIT: ");
    strcat_local(line, g_xhciInitOk ? "OK" : (g_xhciResetTimedOut ? "RESET TIMEOUT" : (g_xhciStartTimedOut ? "START TIMEOUT" : "NOT RUN")));
    strcat_local(line, " SLOTS=");
    itoa10((int)g_xhciMaxSlots, num); strcat_local(line, num);
    strcat_local(line, " PORTS=");
    itoa10((int)g_xhciMaxPorts, num); strcat_local(line, num);
    strcat_local(line, " STS ");
    itoa10((int)g_xhciUsbStsBefore, num); strcat_local(line, num);
    strcat_local(line, " TO ");
    itoa10((int)g_xhciUsbStsAfter, num); strcat_local(line, num);
    draw_string(6, 4 + 5 * (FONT_H + 3), line, 0xFFFF40, 1);

    line[0] = 0;
    strcat_local(line, "XHCIMOUSE: ");
    strcat_local(line, g_xhciMouseActive ? "ACTIVE" : "NONE");
    strcat_local(line, " PORT=");
    itoa10((int)g_xhciMousePort, num); strcat_local(line, num);
    strcat_local(line, " SPEED=");
    itoa10((int)g_xhciMouseSpeed, num); strcat_local(line, num);
    strcat_local(line, " SLOT=");
    itoa10((int)g_xhciMouseSlot, num); strcat_local(line, num);
    strcat_local(line, " EP=");
    itoa10((int)g_xhciMouseEpAddr, num); strcat_local(line, num);
    strcat_local(line, " IVL=");
    itoa10((int)g_xhciMouseInterval, num); strcat_local(line, num);
    strcat_local(line, " MP=");
    itoa10((int)g_xhciMouseMaxPacket, num); strcat_local(line, num);
    strcat_local(line, " HID=");
    itoa10((int)g_xhciMouseIsHid, num); strcat_local(line, num);
    draw_string(6, 4 + 6 * (FONT_H + 3), line, 0xFFFF40, 1);

    line[0] = 0;
    strcat_local(line, "XHCIEVT: ANY=");
    itoa10((int)g_xhciAnyEventCount, num); strcat_local(line, num);
    strcat_local(line, " ARMS=");
    itoa10((int)g_xhciMouseArmCount, num); strcat_local(line, num);
    strcat_local(line, " GOT=");
    itoa10((int)g_xhciMouseEvtCount, num); strcat_local(line, num);
    strcat_local(line, " LASTTYPE=");
    itoa10((int)g_xhciLastEvtType, num); strcat_local(line, num);
    strcat_local(line, " LASTCC=");
    itoa10((int)g_xhciLastEvtCC, num); strcat_local(line, num);
    draw_string(6, 4 + 7 * (FONT_H + 3), line, 0xFFFF40, 1);

    line[0] = 0;
    strcat_local(line, "XHCICFGEP: ATTEMPTED=");
    itoa10((int)g_xhciCfgEpAttempted, num); strcat_local(line, num);
    strcat_local(line, " GOTEVT=");
    itoa10((int)g_xhciCfgEpGotEvt, num); strcat_local(line, num);
    strcat_local(line, " CC=");
    itoa10((int)g_xhciCfgEpCC, num); strcat_local(line, num);
    strcat_local(line, " ENTERED=");
    itoa10((int)g_xhciCfgEpEntered, num); strcat_local(line, num);
    strcat_local(line, " ALLOCINTR=");
    itoa10((int)g_xhciCfgEpAllocIntrFailed, num); strcat_local(line, num);
    strcat_local(line, " ALLOCBUF=");
    itoa10((int)g_xhciCfgEpAllocBufFailed, num); strcat_local(line, num);
    draw_string(6, 4 + 8 * (FONT_H + 3), line, 0xFFFF40, 1);

    line[0] = 0;
    strcat_local(line, "XHCISTATE: SLOTSTATE=");
    itoa10((int)g_xhciOutSlotState, num); strcat_local(line, num);
    strcat_local(line, " EPSTATE=");
    itoa10((int)g_xhciOutEpState, num); strcat_local(line, num);
    strcat_local(line, " PORTSC=");
    itoa10((int)g_xhciPortscSnapshot, num); strcat_local(line, num);
    draw_string(6, 4 + 9 * (FONT_H + 3), line, 0xFFFF40, 1);
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

        /* Some firmware only enables memory-space decode and bus
         * mastering (DMA) on a PCI function once its own driver binds
         * to it. Since we're about to talk to the controller ourselves,
         * make sure both are on regardless of what firmware already
         * did - without bus mastering the controller can't write
         * command completions or events back into our rings at all. */
        if (pci->Attributes) {
            pci->Attributes(pci, EfiPciIoAttributeOperationEnable,
                             EFI_PCI_IO_ATTRIBUTE_MEMORY | EFI_PCI_IO_ATTRIBUTE_BUS_MASTER, NULL);
        }

        *mmioBaseOut = base;
        found = 1;
        break;
    }

    if (h) BS->FreePool(h);
    return found;
}

/* ---- xHCI stage 2: controller reset and initialization ----
 * Register layouts below are transcribed directly from the xHCI 1.2
 * specification's register field tables (not copied from any existing
 * driver's source). All registers are little-endian, which matches
 * x86_64 natively so no byte-swapping is needed. */

typedef struct {
    volatile uint8_t  CapLength;
    uint8_t  Rsvd;
    volatile uint16_t HciVersion;
    volatile uint32_t HcsParams1;
    volatile uint32_t HcsParams2;
    volatile uint32_t HcsParams3;
    volatile uint32_t HccParams1;
    volatile uint32_t DbOff;
    volatile uint32_t RtsOff;
    volatile uint32_t HccParams2;
} XhciCapRegs;

typedef struct {
    volatile uint32_t UsbCmd;
    volatile uint32_t UsbSts;
    volatile uint32_t PageSize;
    uint32_t Rsvd1[2];
    volatile uint32_t DnCtrl;
    volatile uint32_t CrcrLo;
    volatile uint32_t CrcrHi;
    uint32_t Rsvd2[4];
    volatile uint32_t DcbaapLo;
    volatile uint32_t DcbaapHi;
    volatile uint32_t Config;
} XhciOpRegs;

typedef struct {
    volatile uint32_t Portsc;
    volatile uint32_t Portpmsc;
    volatile uint32_t Portli;
    volatile uint32_t Porthlpmc;
} XhciPortRegs;

typedef struct {
    volatile uint32_t Iman;
    volatile uint32_t Imod;
    volatile uint32_t Erstsz;
    uint32_t Rsvd;
    volatile uint32_t ErstbaLo;
    volatile uint32_t ErstbaHi;
    volatile uint32_t ErdpLo;
    volatile uint32_t ErdpHi;
} XhciIntrRegs;

typedef struct { uint32_t P0, P1, P2, P3; } XhciTrb; /* every TRB is 16 bytes */

typedef struct {
    uint64_t RingSegmentBase;
    uint32_t RingSegmentSize;
    uint32_t Rsvd;
} XhciErstEntry;

#define XHCI_USBCMD_RUN     0x1u
#define XHCI_USBCMD_HCRST   0x2u
#define XHCI_USBSTS_HCH     0x1u
#define XHCI_USBSTS_CNR     (1u << 11)

#define XHCI_TRB_C_BIT      0x1u
#define XHCI_TRB_TC_BIT     (1u << 1) /* Toggle Cycle, link TRBs only */
#define XHCI_TRB_TYPE_SHIFT 10
#define XHCI_TRB_TYPE_LINK      6u
#define XHCI_TRB_TYPE_SETUP     2u
#define XHCI_TRB_TYPE_DATA      3u
#define XHCI_TRB_TYPE_STATUS    4u
#define XHCI_TRB_TYPE_NORMAL    1u
#define XHCI_TRB_TYPE_ENABLE_SLOT     9u
#define XHCI_TRB_TYPE_ADDRESS_DEVICE  11u
#define XHCI_TRB_TYPE_CONFIG_ENDPOINT 12u
#define XHCI_TRB_TYPE_TRANSFER_EVENT  32u
#define XHCI_TRB_TYPE_CMD_COMPLETION  33u
#define XHCI_TRB_TYPE_PORT_STATUS_CHANGE 34u

#define XHCI_CMD_RING_TRBS 16
#define XHCI_EVT_RING_TRBS 16

/* A ring buffer of TRBs with the last slot reserved for a Link TRB back
 * to the start (the standard xHCI technique for turning a fixed-size
 * buffer into a logical ring), plus the software-side bookkeeping
 * (index, producer/consumer cycle state) needed to enqueue into it.
 * Used for the Command Ring and for every endpoint's Transfer Ring. */
typedef struct {
    XhciTrb *trbs;
    uint64_t phys;
    uint32_t size; /* TRB slots, including the trailing link TRB */
    uint32_t index;
    int cycle;
} XhciRing;

static void xhci_ring_init(XhciRing *r, XhciTrb *trbs, uint64_t phys, uint32_t size) {
    r->trbs = trbs; r->phys = phys; r->size = size; r->index = 0; r->cycle = 1;
    XhciTrb *link = &trbs[size - 1];
    link->P0 = (uint32_t)(phys & 0xFFFFFFFFu);
    link->P1 = (uint32_t)(phys >> 32);
    link->P3 = (XHCI_TRB_TYPE_LINK << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_TC_BIT | XHCI_TRB_C_BIT;
}

/* Returns the next TRB slot to fill, transparently handling wrap: when
 * the ring's last slot is reached, (re)writes the Link TRB with the
 * *current* producer cycle bit before wrapping - required every lap,
 * since the link TRB is written by the producer just like any other
 * TRB and must match the producer's cycle state at that moment. */
static XhciTrb *xhci_ring_reserve(XhciRing *r) {
    if (r->index == r->size - 1) {
        XhciTrb *link = &r->trbs[r->size - 1];
        link->P3 = (link->P3 & ~XHCI_TRB_C_BIT) | (uint32_t)r->cycle;
        r->index = 0;
        r->cycle ^= 1;
    }
    XhciTrb *slot = &r->trbs[r->index];
    r->index++;
    return slot;
}

/* Cycle bit is written last (after the rest of the TRB's fields) so the
 * controller never observes a partially-written TRB as valid. */
static void xhci_ring_submit(XhciRing *r, XhciTrb *trb, uint32_t p0, uint32_t p1, uint32_t p2, uint32_t p3NoCycle) {
    trb->P0 = p0;
    trb->P1 = p1;
    trb->P2 = p2;
    trb->P3 = (p3NoCycle & ~XHCI_TRB_C_BIT) | (uint32_t)r->cycle;
}

/* Everything an already-initialized controller needs to be driven
 * further (stage 3+): register blocks, the rings, and where the
 * software cycle-bit/dequeue bookkeeping currently stands. */
typedef struct {
    XhciCapRegs *cap;
    XhciOpRegs *op;
    XhciPortRegs *ports; /* ports[0] is port #1 */
    uint32_t *doorbells;
    XhciIntrRegs *intr0;
    uint64_t *dcbaa;
    XhciRing cmd;
    XhciTrb *evtRing;
    uint64_t evtRingPhys;
    int evtCycle; /* current event-ring consumer cycle state */
    uint32_t evtIndex;
    uint32_t maxSlots;
    uint32_t maxPorts;
    uint32_t ctxSize; /* 32 or 64 bytes per device/endpoint context */
} XhciController;

static XhciController g_xhci;

static void xhci_doorbell(XhciController *x, uint32_t slotId, uint32_t target) {
    x->doorbells[slotId] = target;
}

/* Allocates whole pages (always physically contiguous and page-aligned,
 * comfortably meeting every alignment requirement the xHCI spec asks
 * for on these structures) and zeroes them, since AllocatePages makes
 * no promise about initial content. */
static void *xhci_alloc_pages(UINTN pages, uint64_t *physOut) {
    EFI_PHYSICAL_ADDRESS addr = 0;
    if (EFI_ERROR(BS->AllocatePages(AllocateAnyPages, EfiBootServicesData, pages, &addr))) return NULL;
    uint8_t *p = (uint8_t *)addr;
    for (UINTN i = 0; i < pages * 4096; i++) p[i] = 0;
    if (physOut) *physOut = addr;
    return (void *)addr;
}

/* Spins until (*(reg) & mask) == want, or gives up after ~timeoutUs
 * microseconds. Used for the handful of places the xHCI spec requires
 * polling a status bit (HCH after stop, CNR after reset, ...). */
static int xhci_wait_bits(volatile uint32_t *reg, uint32_t mask, uint32_t want, uint32_t timeoutUs) {
    uint32_t waited = 0;
    while ((*reg & mask) != want) {
        if (waited >= timeoutUs) return 0;
        BS->Stall(1000);
        waited += 1000;
    }
    return 1;
}

/* Resets the controller and stands up the three structures every xHCI
 * controller needs before it can run at all: the Device Context Base
 * Address Array (one slot per addressable device, plus slot 0 for
 * scratchpad), a Command Ring (software -> controller requests), and
 * an Event Ring (controller -> software completions/notifications).
 * No interrupts are wired up - later stages just poll the event ring
 * directly, which the spec fully allows. */
static int xhci_init(uint64_t mmioBase) {
    XhciCapRegs *cap = (XhciCapRegs *)mmioBase;
    uint8_t capLen = cap->CapLength;
    XhciOpRegs *op = (XhciOpRegs *)(mmioBase + capLen);
    uint32_t hcsParams1 = cap->HcsParams1;
    uint32_t maxSlots = hcsParams1 & 0xFF;
    uint32_t maxPorts = (hcsParams1 >> 24) & 0xFF;
    uint32_t dbOff = cap->DbOff & ~0x3u;
    uint32_t rtsOff = cap->RtsOff & ~0x1Fu;
    uint32_t *doorbells = (uint32_t *)(mmioBase + dbOff);
    XhciIntrRegs *intr0 = (XhciIntrRegs *)(mmioBase + rtsOff + 0x20);
    XhciPortRegs *ports = (XhciPortRegs *)((uint8_t *)op + 0x400);

    g_xhciUsbStsBefore = op->UsbSts;

    /* Stop the controller (it may already be running, e.g. if
     * firmware's own driver had it up), then wait for it to actually
     * halt before resetting - resetting a still-running controller is
     * not well-defined behavior. */
    op->UsbCmd &= ~XHCI_USBCMD_RUN;
    xhci_wait_bits(&op->UsbSts, XHCI_USBSTS_HCH, XHCI_USBSTS_HCH, 1000000);

    op->UsbCmd |= XHCI_USBCMD_HCRST;
    if (!xhci_wait_bits(&op->UsbCmd, XHCI_USBCMD_HCRST, 0, 1000000)) { g_xhciResetTimedOut = 1; return 0; }
    if (!xhci_wait_bits(&op->UsbSts, XHCI_USBSTS_CNR, 0, 1000000)) { g_xhciResetTimedOut = 1; return 0; }

    op->Config = maxSlots;

    UINTN dcbaaPages = ((maxSlots + 1) * 8 + 4095) / 4096;
    if (dcbaaPages == 0) dcbaaPages = 1;
    uint64_t dcbaaPhys = 0;
    uint64_t *dcbaa = (uint64_t *)xhci_alloc_pages(dcbaaPages, &dcbaaPhys);
    if (!dcbaa) return 0;
    op->DcbaapLo = (uint32_t)dcbaaPhys;
    op->DcbaapHi = (uint32_t)(dcbaaPhys >> 32);

    uint64_t cmdRingPhys = 0;
    XhciTrb *cmdRingTrbs = (XhciTrb *)xhci_alloc_pages(1, &cmdRingPhys);
    if (!cmdRingTrbs) return 0;
    XhciRing cmdRing;
    xhci_ring_init(&cmdRing, cmdRingTrbs, cmdRingPhys, XHCI_CMD_RING_TRBS);

    op->CrcrLo = (uint32_t)(cmdRingPhys & 0xFFFFFFFFu) | 0x1u; /* RCS = 1 */
    op->CrcrHi = (uint32_t)(cmdRingPhys >> 32);

    uint64_t evtRingPhys = 0;
    XhciTrb *evtRing = (XhciTrb *)xhci_alloc_pages(1, &evtRingPhys);
    if (!evtRing) return 0;

    uint64_t erstPhys = 0;
    XhciErstEntry *erst = (XhciErstEntry *)xhci_alloc_pages(1, &erstPhys);
    if (!erst) return 0;
    erst[0].RingSegmentBase = evtRingPhys;
    erst[0].RingSegmentSize = XHCI_EVT_RING_TRBS;

    /* Order matters here per spec: segment table size, then the
     * dequeue pointer, then the segment table base address. */
    intr0->Erstsz = 1;
    intr0->ErdpLo = (uint32_t)(evtRingPhys & 0xFFFFFFFFu);
    intr0->ErdpHi = (uint32_t)(evtRingPhys >> 32);
    intr0->ErstbaLo = (uint32_t)(erstPhys & 0xFFFFFFFFu);
    intr0->ErstbaHi = (uint32_t)(erstPhys >> 32);

    op->UsbCmd |= XHCI_USBCMD_RUN;
    if (!xhci_wait_bits(&op->UsbSts, XHCI_USBSTS_HCH, 0, 1000000)) { g_xhciStartTimedOut = 1; return 0; }

    g_xhciUsbStsAfter = op->UsbSts;

    g_xhci.cap = cap;
    g_xhci.op = op;
    g_xhci.ports = ports;
    g_xhci.doorbells = doorbells;
    g_xhci.intr0 = intr0;
    g_xhci.dcbaa = dcbaa;
    g_xhci.cmd = cmdRing;
    g_xhci.evtRing = evtRing;
    g_xhci.evtRingPhys = evtRingPhys;
    g_xhci.evtCycle = 1;
    g_xhci.evtIndex = 0;
    g_xhci.maxSlots = maxSlots;
    g_xhci.maxPorts = maxPorts;
    g_xhci.ctxSize = (cap->HccParams1 & 0x4) ? 64 : 32; /* CSZ bit */
    g_xhciMaxSlots = maxSlots;
    g_xhciMaxPorts = maxPorts;

    return 1;
}

/* ---- xHCI stages 3-5: port scan, device bring-up, interrupt polling ----
 * From here on this is an original (if minimal) USB stack: enumerate
 * ports, address a device, walk its configuration descriptor for an
 * interrupt IN endpoint, switch it into HID Boot Protocol, and keep a
 * single Normal TRB in flight on that endpoint to receive reports.
 * Only mice are driven this way; keyboard input keeps using UEFI's own
 * EFI_SIMPLE_TEXT_INPUT_PROTOCOL; a from-scratch driver only needs to
 * replace the one thing that was actually unreliable. */

#define XHCI_SPEED_FULL  1u
#define XHCI_SPEED_LOW   2u
#define XHCI_SPEED_HIGH  3u
#define XHCI_SPEED_SUPER 4u

static uint32_t xhci_ep0_max_packet(uint32_t speed) {
    if (speed == XHCI_SPEED_HIGH) return 64;
    if (speed == XHCI_SPEED_SUPER) return 512;
    return 8; /* low and full speed both start at 8 until told otherwise */
}

/* The Endpoint Context's Interval field is 2^Interval * 125us, not a
 * raw millisecond count. High/Super speed descriptors already give
 * bInterval as an exponent (1-16), needing only a -1 shift; Low/Full
 * speed descriptors give bInterval in whole 1ms frames (8 * 125us),
 * which has to be converted to the nearest power-of-two exponent. */
static uint32_t xhci_encode_interval(uint32_t speed, uint8_t descInterval) {
    if (descInterval == 0) descInterval = 1;
    if (speed == XHCI_SPEED_HIGH || speed == XHCI_SPEED_SUPER) {
        uint32_t v = descInterval;
        if (v > 16) v = 16;
        return v - 1;
    }
    uint32_t target = (uint32_t)descInterval * 8;
    uint32_t n = 0, p = 1;
    while (p < target && n < 15) { p <<= 1; n++; }
    return n;
}

/* Drains the event ring looking for a TRB of wantType (and, for
 * completion-style events, a matching slot ID). Every event seen along
 * the way - including ones that don't match, like an unrelated Port
 * Status Change - is consumed and the hardware dequeue pointer (ERDP)
 * advanced, so nothing the caller doesn't care about can ever wedge
 * the ring. */
static int xhci_event_poll(XhciController *x, uint32_t wantType, uint32_t wantSlot, XhciTrb *out, uint32_t timeoutUs) {
    uint32_t waited = 0;
    for (;;) {
        int any = 0;
        for (;;) {
            XhciTrb *trb = &x->evtRing[x->evtIndex];
            if ((trb->P3 & XHCI_TRB_C_BIT) != (uint32_t)x->evtCycle) break;
            any = 1;
            XhciTrb got = *trb;
            x->evtIndex++;
            if (x->evtIndex == XHCI_EVT_RING_TRBS) { x->evtIndex = 0; x->evtCycle ^= 1; }
            uint64_t erdp = x->evtRingPhys + (uint64_t)x->evtIndex * 16;
            x->intr0->ErdpLo = (uint32_t)(erdp & 0xFFFFFFF0u) | 0x8u; /* clear EHB */
            x->intr0->ErdpHi = (uint32_t)(erdp >> 32);
            g_xhciAnyEventCount++;

            uint32_t type = (got.P3 >> XHCI_TRB_TYPE_SHIFT) & 0x3F;
            g_xhciLastEvtType = type;
            g_xhciLastEvtCC = (got.P2 >> 24) & 0xFF;
            if (type == wantType) {
                uint32_t slotId = (got.P3 >> 24) & 0xFF;
                if (wantSlot == 0xFFFFFFFFu || slotId == wantSlot) {
                    if (out) *out = got;
                    return 1;
                }
            }
        }
        if (!any) {
            if (waited >= timeoutUs) return 0;
            BS->Stall(1000);
            waited += 1000;
        }
    }
}

static int xhci_cmd_wait(XhciController *x, XhciTrb *trbOut, uint32_t wantSlot, uint32_t timeoutUs) {
    xhci_doorbell(x, 0, 0);
    if (!xhci_event_poll(x, XHCI_TRB_TYPE_CMD_COMPLETION, wantSlot, trbOut, timeoutUs)) return 0;
    return ((trbOut->P2 >> 24) & 0xFF) == 1; /* Completion Code == Success */
}

static int xhci_enable_slot(XhciController *x, uint32_t *slotIdOut) {
    XhciTrb *t = xhci_ring_reserve(&x->cmd);
    xhci_ring_submit(&x->cmd, t, 0, 0, 0, XHCI_TRB_TYPE_ENABLE_SLOT << XHCI_TRB_TYPE_SHIFT);
    XhciTrb evt;
    if (!xhci_cmd_wait(x, &evt, 0xFFFFFFFFu, 1000000)) return 0;
    *slotIdOut = (evt.P3 >> 24) & 0xFF;
    return 1;
}

/* A device's usable state after Address Device: the output context the
 * controller writes into, its own control (EP0) transfer ring, and
 * (once configured) its interrupt IN endpoint's transfer ring. */
typedef struct {
    int active;
    uint32_t slotId;
    uint32_t portNum1;   /* 1-based root hub port number */
    uint32_t speed;
    uint32_t maxPacket0;
    void *inputCtx; uint64_t inputCtxPhys;
    void *outputCtx; uint64_t outputCtxPhys;
    XhciRing ep0;
    XhciRing intr;
    uint32_t intrDci;
    uint32_t intrMaxPacket;
    uint8_t *reportBuf; uint64_t reportBufPhys;
} XhciMouse;

static XhciMouse g_xhciMouse;

static int xhci_address_device(XhciController *x, XhciMouse *m) {
    uint32_t ctxSize = x->ctxSize;
    uint64_t inputPhys = 0;
    uint8_t *input = (uint8_t *)xhci_alloc_pages(1, &inputPhys);
    if (!input) return 0;
    uint64_t outputPhys = 0;
    uint8_t *output = (uint8_t *)xhci_alloc_pages(1, &outputPhys);
    if (!output) return 0;

    uint64_t ep0RingPhys = 0;
    XhciTrb *ep0RingTrbs = (XhciTrb *)xhci_alloc_pages(1, &ep0RingPhys);
    if (!ep0RingTrbs) return 0;
    xhci_ring_init(&m->ep0, ep0RingTrbs, ep0RingPhys, XHCI_CMD_RING_TRBS);

    uint32_t *icc = (uint32_t *)(input + 0);
    icc[1] = 0x3; /* A0 (slot context) | A1 (EP0 context) */

    uint32_t *slotCtx = (uint32_t *)(input + ctxSize);
    slotCtx[0] = (1u << 27) | (m->speed << 20); /* Context Entries=1, Speed */
    slotCtx[1] = ((m->portNum1 & 0xFF) << 16);  /* Root Hub Port Number */

    uint32_t *ep0Ctx = (uint32_t *)(input + 2 * ctxSize);
    ep0Ctx[1] = (4u << 3) | (3u << 1) | (m->maxPacket0 << 16); /* EP Type=Control, CErr=3 */
    uint64_t ep0Deq = ep0RingPhys | 1u; /* DCS = 1 */
    ep0Ctx[2] = (uint32_t)(ep0Deq & 0xFFFFFFFFu);
    ep0Ctx[3] = (uint32_t)(ep0Deq >> 32);
    ep0Ctx[4] = 8u; /* Average TRB Length */

    x->dcbaa[m->slotId] = outputPhys;

    XhciTrb *t = xhci_ring_reserve(&x->cmd);
    uint32_t p3 = (XHCI_TRB_TYPE_ADDRESS_DEVICE << XHCI_TRB_TYPE_SHIFT) | ((m->slotId & 0xFF) << 24);
    xhci_ring_submit(&x->cmd, t, (uint32_t)(inputPhys & 0xFFFFFFFFu), (uint32_t)(inputPhys >> 32), 0, p3);
    XhciTrb evt;
    if (!xhci_cmd_wait(x, &evt, m->slotId, 1000000)) return 0;

    m->inputCtx = input;
    m->inputCtxPhys = inputPhys;
    m->outputCtx = output;
    m->outputCtxPhys = outputPhys;
    return 1;
}

/* Runs one control transfer on a device's EP0 ring: Setup stage, an
 * optional Data stage, and a Status stage (direction always opposite
 * the Data stage, or IN for a no-data request) - the standard 3-part
 * shape every USB control transfer takes. */
static int xhci_control_transfer(XhciController *x, XhciMouse *m, uint8_t bmRequestType, uint8_t bRequest,
                                  uint16_t wValue, uint16_t wIndex, uint16_t wLength, void *buf, uint64_t bufPhys) {
    int isIn = (bmRequestType & 0x80) != 0;
    int hasData = wLength != 0;

    XhciTrb *setup = xhci_ring_reserve(&m->ep0);
    uint32_t w0 = bmRequestType | ((uint32_t)bRequest << 8) | ((uint32_t)wValue << 16);
    uint32_t w1 = wIndex | ((uint32_t)wLength << 16);
    uint32_t trt = hasData ? (isIn ? 3u : 2u) : 0u;
    uint32_t setupP3 = (XHCI_TRB_TYPE_SETUP << XHCI_TRB_TYPE_SHIFT) | (trt << 16) | (1u << 6) /* IDT */ | (hasData ? (1u << 4) : 0);
    xhci_ring_submit(&m->ep0, setup, w0, w1, 8u, setupP3);

    if (hasData) {
        XhciTrb *data = xhci_ring_reserve(&m->ep0);
        uint32_t dataP3 = (XHCI_TRB_TYPE_DATA << XHCI_TRB_TYPE_SHIFT) | ((isIn ? 1u : 0u) << 16) | (1u << 4) /* CH */;
        xhci_ring_submit(&m->ep0, data, (uint32_t)(bufPhys & 0xFFFFFFFFu), (uint32_t)(bufPhys >> 32), wLength, dataP3);
    }

    XhciTrb *status = xhci_ring_reserve(&m->ep0);
    uint32_t statusDir = hasData ? (isIn ? 0u : 1u) : 1u;
    uint32_t statusP3 = (XHCI_TRB_TYPE_STATUS << XHCI_TRB_TYPE_SHIFT) | (statusDir << 16) | (1u << 5) /* IOC */;
    xhci_ring_submit(&m->ep0, status, 0, 0, 0, statusP3);

    xhci_doorbell(x, m->slotId, 1); /* DCI 1 = EP0 */

    XhciTrb evt;
    if (!xhci_event_poll(x, XHCI_TRB_TYPE_TRANSFER_EVENT, m->slotId, &evt, 1000000)) return 0;
    uint32_t cc = (evt.P2 >> 24) & 0xFF;
    return cc == 1 /* Success */ || cc == 13 /* Short Packet */;
}

static int xhci_get_descriptor(XhciController *x, XhciMouse *m, uint8_t type, uint8_t index,
                                void *buf, uint64_t bufPhys, uint16_t len) {
    return xhci_control_transfer(x, m, 0x80, 6 /* GET_DESCRIPTOR */, ((uint16_t)type << 8) | index, 0, len, buf, bufPhys);
}

static int xhci_set_configuration(XhciController *x, XhciMouse *m, uint8_t value) {
    return xhci_control_transfer(x, m, 0x00, 9 /* SET_CONFIGURATION */, value, 0, 0, NULL, 0);
}

static int xhci_set_boot_protocol(XhciController *x, XhciMouse *m, uint8_t ifaceNum) {
    /* HID class request, interface recipient: SET_PROTOCOL(Boot=0) */
    xhci_control_transfer(x, m, 0x21, 0x0B, 0, ifaceNum, 0, NULL, 0);
    /* SET_IDLE(0): report only on change, avoids flooding the ring */
    return xhci_control_transfer(x, m, 0x21, 0x0A, 0, ifaceNum, 0, NULL, 0);
}

/* Issues Configure Endpoint for the device's interrupt IN endpoint and
 * stands up its transfer ring. epAddr is the raw bEndpointAddress from
 * the endpoint descriptor (high bit = direction). */
static int xhci_configure_intr_endpoint(XhciController *x, XhciMouse *m, uint8_t epAddr, uint16_t maxPacket, uint8_t interval) {
    g_xhciCfgEpEntered = 1;
    uint32_t dci = ((uint32_t)(epAddr & 0x0F) * 2) + ((epAddr & 0x80) ? 1u : 0u);
    m->intrDci = dci;
    m->intrMaxPacket = maxPacket;

    uint64_t intrRingPhys = 0;
    XhciTrb *intrRingTrbs = (XhciTrb *)xhci_alloc_pages(1, &intrRingPhys);
    if (!intrRingTrbs) { g_xhciCfgEpAllocIntrFailed = 1; return 0; }
    xhci_ring_init(&m->intr, intrRingTrbs, intrRingPhys, XHCI_CMD_RING_TRBS);

    uint64_t reportBufPhys = 0;
    uint8_t *reportBuf = (uint8_t *)xhci_alloc_pages(1, &reportBufPhys);
    if (!reportBuf) { g_xhciCfgEpAllocBufFailed = 1; return 0; }
    m->reportBuf = reportBuf;
    m->reportBufPhys = reportBufPhys;

    uint32_t ctxSize = x->ctxSize;
    uint8_t *input = (uint8_t *)m->inputCtx;
    for (UINTN i = 0; i < 4096; i++) input[i] = 0;

    uint32_t *icc = (uint32_t *)(input + 0);
    icc[1] = 0x1u | (1u << dci); /* A0 (slot context, entry count changed) | this endpoint */

    uint32_t *slotCtx = (uint32_t *)(input + ctxSize);
    slotCtx[0] = (dci << 27) | (m->speed << 20);
    slotCtx[1] = ((m->portNum1 & 0xFF) << 16);

    uint32_t *epCtx = (uint32_t *)(input + (1 + dci) * ctxSize);
    epCtx[0] = (xhci_encode_interval(m->speed, interval) << 16);
    epCtx[1] = (7u << 3) | (3u << 1) | ((uint32_t)maxPacket << 16); /* EP Type=Interrupt IN, CErr=3 */
    uint64_t deq = m->intr.phys | 1u; /* DCS = 1 */
    epCtx[2] = (uint32_t)(deq & 0xFFFFFFFFu);
    epCtx[3] = (uint32_t)(deq >> 32);
    epCtx[4] = (uint32_t)maxPacket;

    XhciTrb *t = xhci_ring_reserve(&x->cmd);
    uint32_t p3 = (XHCI_TRB_TYPE_CONFIG_ENDPOINT << XHCI_TRB_TYPE_SHIFT) | ((m->slotId & 0xFF) << 24);
    xhci_ring_submit(&x->cmd, t, (uint32_t)(m->inputCtxPhys & 0xFFFFFFFFu), (uint32_t)(m->inputCtxPhys >> 32), 0, p3);
    xhci_doorbell(x, 0, 0);
    XhciTrb evt;
    g_xhciCfgEpAttempted = 1;
    g_xhciCfgEpGotEvt = xhci_event_poll(x, XHCI_TRB_TYPE_CMD_COMPLETION, m->slotId, &evt, 1000000);
    g_xhciCfgEpCC = g_xhciCfgEpGotEvt ? ((evt.P2 >> 24) & 0xFF) : 0xFF;
    return g_xhciCfgEpGotEvt && g_xhciCfgEpCC == 1;
}

/* Posts one Normal TRB on the interrupt endpoint's ring pointing at the
 * shared report buffer, and rings its doorbell. A fresh TRB is posted
 * every time the previous one completes (see xhci_mouse_poll), so
 * there is always exactly one read in flight - simpler than pipelining
 * several, and plenty fast for a mouse. */
static void xhci_mouse_arm(XhciController *x, XhciMouse *m) {
    XhciTrb *t = xhci_ring_reserve(&m->intr);
    uint32_t p3 = (XHCI_TRB_TYPE_NORMAL << XHCI_TRB_TYPE_SHIFT) | (1u << 5) /* IOC */;
    xhci_ring_submit(&m->intr, t, (uint32_t)(m->reportBufPhys & 0xFFFFFFFFu), (uint32_t)(m->reportBufPhys >> 32),
                      m->intrMaxPacket, p3);
    xhci_doorbell(x, m->slotId, m->intrDci);
    g_xhciMouseArmCount++;
}

/* Walks a just-fetched configuration descriptor for the first interrupt
 * IN endpoint, preferring one inside a HID boot-protocol mouse
 * interface (class 3, subclass 1, protocol 2) but accepting any
 * interrupt IN endpoint as a fallback, since not every mouse bothers
 * declaring boot protocol support. Returns 1 and fills the out
 * parameters if one was found. */
static int xhci_find_interrupt_in_endpoint(uint8_t *cfg, uint16_t total, uint8_t *epAddrOut, uint16_t *maxPacketOut,
                                            uint8_t *intervalOut, uint8_t *ifaceNumOut, int *isHidMouseOut) {
    int found = 0, foundHid = 0;
    uint8_t curIfaceClass = 0, curIfaceSub = 0, curIfaceProto = 0, curIfaceNum = 0;
    for (uint16_t off = 0; off + 2 <= total; ) {
        uint8_t len = cfg[off], type = cfg[off + 1];
        if (len < 2 || off + len > total) break;
        if (type == 4 && len >= 9) { /* INTERFACE */
            curIfaceNum = cfg[off + 2];
            curIfaceClass = cfg[off + 5];
            curIfaceSub = cfg[off + 6];
            curIfaceProto = cfg[off + 7];
        } else if (type == 5 && len >= 7) { /* ENDPOINT */
            uint8_t addr = cfg[off + 2];
            uint8_t attr = cfg[off + 3];
            if ((addr & 0x80) && (attr & 0x3) == 3) { /* interrupt IN */
                int isHid = (curIfaceClass == 3 && curIfaceSub == 1 && curIfaceProto == 2);
                if (!found || (isHid && !foundHid)) {
                    *epAddrOut = addr;
                    *maxPacketOut = (uint16_t)(cfg[off + 4] | (cfg[off + 5] << 8));
                    *intervalOut = cfg[off + 6];
                    *ifaceNumOut = curIfaceNum;
                    *isHidMouseOut = isHid;
                    found = 1;
                    if (isHid) foundHid = 1;
                }
            }
        }
        off += len;
    }
    return found;
}

/* Full bring-up for one device on one port: address it, read its
 * configuration descriptor, set the configuration, switch HID
 * interfaces into Boot Protocol, configure the interrupt IN endpoint
 * and start it receiving reports. Returns 1 on success. */
static int xhci_bring_up_mouse(XhciController *x, XhciMouse *m, uint32_t portNum1, uint32_t speed) {
    m->slotId = 0;
    m->portNum1 = portNum1;
    m->speed = speed;
    m->maxPacket0 = xhci_ep0_max_packet(speed);

    uint32_t slotId;
    if (!xhci_enable_slot(x, &slotId)) return 0;
    m->slotId = slotId;
    /* Record progress even on eventual failure, so diagnostics show how
     * far bring-up got rather than just "NONE". */
    g_xhciMousePort = portNum1;
    g_xhciMouseSpeed = speed;
    g_xhciMouseSlot = slotId;

    if (!xhci_address_device(x, m)) return 0;

    uint64_t descPhys = 0;
    uint8_t *desc = (uint8_t *)xhci_alloc_pages(1, &descPhys);
    if (!desc) return 0;

    if (!xhci_get_descriptor(x, m, 2 /* CONFIGURATION */, 0, desc, descPhys, 9)) return 0;
    uint16_t totalLen = (uint16_t)(desc[2] | (desc[3] << 8));
    if (totalLen < 9) totalLen = 9;
    if (totalLen > 4096) totalLen = 4096;
    if (!xhci_get_descriptor(x, m, 2 /* CONFIGURATION */, 0, desc, descPhys, totalLen)) return 0;

    uint8_t configValue = desc[5];
    uint8_t epAddr = 0, iface = 0, interval = 1;
    uint16_t maxPacket = 8;
    int isHid = 0;
    if (!xhci_find_interrupt_in_endpoint(desc, totalLen, &epAddr, &maxPacket, &interval, &iface, &isHid)) return 0;
    if (maxPacket == 0 || maxPacket > 64) maxPacket = 8;
    if (interval == 0) interval = 1;
    g_xhciMouseEpAddr = epAddr;
    g_xhciMouseInterval = interval;
    g_xhciMouseMaxPacket = maxPacket;
    g_xhciMouseIsHid = (uint32_t)isHid;

    if (!xhci_set_configuration(x, m, configValue)) return 0;
    if (isHid) xhci_set_boot_protocol(x, m, iface); /* best-effort, don't fail bring-up over it */

    if (!xhci_configure_intr_endpoint(x, m, epAddr, maxPacket, interval)) return 0;

    xhci_mouse_arm(x, m);
    m->active = 1;
    return 1;
}

/* PORTSC mixes RW status-change bits (CSC/PEC/WRC/OCC/PRC/PLC/CEC,
 * bits 17-23) that clear on a write of 1 with ordinary RW/RO fields.
 * Writing back a value read straight from the register would silently
 * clear whichever change bits happened to be set at read time; this
 * helper always clears that whole range to 0 first (no-op for W1C
 * bits) and then ORs in only the specific bit(s) the caller actually
 * wants to change. */
#define XHCI_PORTSC_RW1C_MASK 0x00FE0000u
static void xhci_portsc_write(volatile uint32_t *portsc, uint32_t setBits) {
    uint32_t v = *portsc;
    v &= ~XHCI_PORTSC_RW1C_MASK;
    v |= setBits;
    *portsc = v;
}

/* Scans every root hub port for a connected device and attempts to
 * bring up the first one as a mouse. Ports that are connected but
 * whose link hasn't trained yet get a Port Reset (works uniformly for
 * USB2 and USB3 ports on xHCI - the controller runs the appropriate
 * signaling for whichever the port actually is). */
static int xhci_scan_and_bring_up(XhciController *x) {
    for (uint32_t p = 0; p < x->maxPorts; p++) {
        volatile uint32_t *portsc = &x->ports[p].Portsc;
        uint32_t sc = *portsc;
        if (!(sc & 0x1)) continue; /* CCS: nothing connected */

        if (!(sc & 0x2)) { /* not yet Port Enabled - reset it */
            xhci_portsc_write(portsc, (1u << 4)); /* PR */
            xhci_wait_bits(portsc, (1u << 21), (1u << 21), 500000); /* wait for PRC */
            xhci_portsc_write(portsc, (1u << 21)); /* clear PRC */
            sc = *portsc;
            if (!(sc & 0x2)) continue; /* still not enabled - skip this port */
        }

        uint32_t speed = (sc >> 10) & 0xF;
        if (speed == 0) continue;

        XhciMouse m;
        m.active = 0;
        if (xhci_bring_up_mouse(x, &m, p + 1, speed)) {
            g_xhciMouse = m;
            g_xhciMouseActive = 1;
            g_xhciMousePort = p + 1;
            g_xhciMouseSpeed = speed;
            g_xhciMouseSlot = m.slotId;
            return 1;
        }
    }
    return 0;
}

/* Refreshes diagnostic snapshots of the controller-owned Output Device
 * Context (Slot State, EP State) and the port's live PORTSC - lets us
 * tell "configured but the xHC never actually started the endpoint"
 * apart from "started fine, device just isn't sending reports". */
static void xhci_mouse_refresh_state(XhciController *x, XhciMouse *m) {
    if (!m->outputCtx) return;
    uint32_t ctxSize = x->ctxSize;
    uint32_t *slotCtx = (uint32_t *)((uint8_t *)m->outputCtx + 0);
    g_xhciOutSlotState = (slotCtx[3] >> 27) & 0x1F;
    uint32_t *epCtx = (uint32_t *)((uint8_t *)m->outputCtx + (uint64_t)m->intrDci * ctxSize);
    g_xhciOutEpState = epCtx[0] & 0x7;
    if (m->portNum1 >= 1 && m->portNum1 <= x->maxPorts) {
        g_xhciPortscSnapshot = x->ports[m->portNum1 - 1].Portsc;
    }
}

/* Called once per frame. If a completed report is waiting, decodes the
 * standard HID boot mouse layout (buttons, signed dx, signed dy,
 * optional wheel) and re-arms the endpoint for the next one. */
static int xhci_mouse_poll(int *dxOut, int *dyOut, int *btnOut) {
    if (!g_xhciMouse.active) return 0;
    xhci_mouse_refresh_state(&g_xhci, &g_xhciMouse);
    XhciTrb evt;
    if (!xhci_event_poll(&g_xhci, XHCI_TRB_TYPE_TRANSFER_EVENT, g_xhciMouse.slotId, &evt, 0)) return 0;
    g_xhciMouseEvtCount++;
    uint32_t cc = (evt.P2 >> 24) & 0xFF;
    if (cc == 1 || cc == 13) {
        uint8_t *b = g_xhciMouse.reportBuf;
        *btnOut = b[0] & 0x01;
        *dxOut = (int8_t)b[1];
        *dyOut = (int8_t)b[2];
    } else {
        *dxOut = 0; *dyOut = 0; *btnOut = 0;
    }
    xhci_mouse_arm(&g_xhci, &g_xhciMouse);
    return 1;
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
    if (g_xhciFound) g_xhciInitOk = xhci_init(g_xhciMmioBase);
    if (g_xhciInitOk) {
        /* Ports can take a moment to report CCS after the reset above
         * (same real-hardware enumeration delay as everywhere else in
         * this file), so retry for a good while before giving up. */
        for (int attempt = 0; attempt < 30; attempt++) {
            if (xhci_scan_and_bring_up(&g_xhci)) break;
            BS->Stall(300000);
        }
    }

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

        /* Our own xHCI driver, once it has a mouse configured, is
         * authoritative - skip UEFI's own pointer protocols entirely
         * rather than mixing two sources of truth for the same mouse. */
        if (g_xhciMouse.active) {
            int dx = 0, dy = 0, btn = 0;
            if (xhci_mouse_poll(&dx, &dy, &btn)) {
                g_pointerKind = "XHCI"; g_pollTotal++; g_pollSuccess++;
                g_lastRawDx = dx; g_lastRawDy = dy; g_lastDivisor = 1; g_lastBtn = btn;
                if (dx > 60) dx = 60; if (dx < -60) dx = -60;
                if (dy > 60) dy = 60; if (dy < -60) dy = -60;
                mouse_x += dx;
                mouse_y += dy;
                if (btn) left_now = 1;
            }
        }

        /* If no pointer device was found yet, keep periodically
         * re-checking - it may appear late (slow enumeration) or get
         * hot-plugged while the desktop is already running. Our own
         * xHCI driver gets first refusal at each rescan, since it's
         * meant to replace the UEFI-protocol fallback whenever it can
         * actually get a device configured. */
        if (!g_xhciMouse.active && ptrs.spCount == 0 && ptrs.apCount == 0) {
            rescanCounter++;
            if (rescanCounter >= 100) { /* roughly once a second at the 10ms frame stall below */
                rescanCounter = 0;
                if (g_xhciInitOk) xhci_scan_and_bring_up(&g_xhci);
                if (!g_xhciMouse.active) {
                    connect_all_controllers();
                    find_pointers(&ptrs);
                    g_pointerCount = ptrs.spCount + ptrs.apCount + ptrs.rawCount;
                }
            }
        }

        /* A pointer protocol can be "found" but dead - registered by
         * firmware without ever being wired to real hardware, so
         * GetState never once returns success no matter how much the
         * mouse actually moves. If that's what happened, stop trusting
         * it and go straight for the raw USB HID fallback instead. Only
         * relevant when our own xHCI driver didn't already take over -
         * once it resets the controller, UEFI's own pointer protocol
         * handles for the same device are stale anyway. */
        if (!g_xhciMouse.active && !stuckFallbackTried && ptrs.rawCount == 0 && ptrs.apCount > 0 &&
            g_pollTotal >= 150 && g_pollSuccess == 0) {
            stuckFallbackTried = 1;
            find_raw_hid_mice(&ptrs);
            g_pollTotal = 0;
            g_pollSuccess = 0;
            g_pointerCount = ptrs.spCount + ptrs.apCount + ptrs.rawCount;
        }

        for (int i = 0; i < (g_xhciMouse.active ? 0 : ptrs.spCount); i++) {
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
        for (int i = 0; i < (g_xhciMouse.active ? 0 : ptrs.apCount); i++) {
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
        for (int i = 0; i < (g_xhciMouse.active ? 0 : ptrs.rawCount); i++) {
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

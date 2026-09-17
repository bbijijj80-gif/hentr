#include <stdint.h>
#include "../boot/bootinfo.h"
#include "gfx.h"
#include "mouse.h"
#include "rtc.h"
#include "io.h"
#include "font.h"
#include "logo.h"

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
static int taskbarH = 40;

typedef struct {
    int x, y, w, h;
    int visible;
} Window;

static Window win = { .x = 220, .y = 120, .w = 420, .h = 260, .visible = 1 };
static int start_menu_open = 0;
static int dragging = 0;
static int drag_off_x, drag_off_y;

static int mouse_x, mouse_y;
static uint8_t prev_buttons = 0;

#define CUR_SZ 16
static uint32_t cursor_backing[CUR_SZ][CUR_SZ];
static int cursor_backed = 0;
static int last_cur_x = -1, last_cur_y = -1;

static int rect_hit(int px, int py, int x, int y, int w, int h) {
    return px >= x && px < x + w && py >= y && py < y + h;
}

static void draw_taskbar(void) {
    int ty = screenH - taskbarH;
    fill_rect(0, ty, screenW, taskbarH, COL_TASKBAR);
    draw_hline(0, ty, screenW, COL_TASKBAR_EDGE);

    uint32_t sbtn = start_menu_open ? COL_START_BTN_HI : COL_START_BTN;
    fill_rect(8, ty + 6, 90, taskbarH - 12, sbtn);
    draw_string(8 + 14, ty + 6 + (taskbarH - 12 - FONT_H) / 2, "START", COL_TEXT_LIGHT, 1);

    if (win.visible) {
        fill_rect(110, ty + 6, 150, taskbarH - 12, COL_MENU_HI);
        draw_string(110 + 8, ty + 6 + (taskbarH - 12 - FONT_H) / 2, "HENTROS", COL_TEXT_LIGHT, 1);
    }

    RtcTime t = rtc_read();
    char buf[9];
    buf[0] = '0' + t.hour / 10; buf[1] = '0' + t.hour % 10; buf[2] = ':';
    buf[3] = '0' + t.min / 10;  buf[4] = '0' + t.min % 10;  buf[5] = ':';
    buf[6] = '0' + t.sec / 10;  buf[7] = '0' + t.sec % 10;  buf[8] = 0;
    int tw = text_width(buf, 1);
    draw_string(screenW - tw - 14, ty + (taskbarH - FONT_H) / 2, buf, COL_TEXT_LIGHT, 1);
}

static void draw_start_menu(void) {
    int mw = 220, mh = 190;
    int mx = 8, my = screenH - taskbarH - mh;
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

static void render_scene(void) {
    fill_gradient_v(0, 0, screenW, screenH - taskbarH, COL_DESKTOP_TOP, COL_DESKTOP_BOTTOM);
    draw_desktop_icons();
    draw_window();
    draw_taskbar();
    if (start_menu_open) draw_start_menu();
    cursor_backed = 0; /* framebuffer changed under the cursor; force a fresh save next draw */
}

static void restore_cursor(void) {
    if (!cursor_backed || last_cur_x < 0) return;
    for (int j = 0; j < CUR_SZ; j++)
        for (int i = 0; i < CUR_SZ; i++)
            put_pixel(last_cur_x + i, last_cur_y + j, cursor_backing[j][i]);
}

static void save_cursor(int x, int y) {
    for (int j = 0; j < CUR_SZ; j++)
        for (int i = 0; i < CUR_SZ; i++)
            cursor_backing[j][i] = get_pixel(x + i, y + j);
    cursor_backed = 1;
    last_cur_x = x; last_cur_y = y;
}

static void draw_cursor(int x, int y) {
    static const char *shape[CUR_SZ] = {
        "#...............",
        "##..............",
        "#.#.............",
        "#..#............",
        "#...#...........",
        "#....#..........",
        "#.....#.........",
        "#......#........",
        "#.......#.......",
        "#....#####......",
        "#..##...........",
        "#.#.............",
        "##..............",
        "#...............",
        "................",
        "................",
    };
    for (int j = 0; j < CUR_SZ; j++) {
        for (int i = 0; i < CUR_SZ; i++) {
            if (shape[j][i] == '#') {
                put_pixel(x + i, y + j, 0x000000);
                put_pixel(x + i + 1, y + j, 0xFFFFFF);
            }
        }
    }
}

static void delay(uint32_t n) {
    for (uint32_t i = 0; i < n; i++) io_wait();
}

extern char __bss_start[], __bss_end[];

__attribute__((section(".text.start")))
void kernel_entry(BootInfo *info) {
    /* AllocatePages does not zero memory, so uninitialized statics start
     * out full of firmware garbage unless we clear .bss ourselves. */
    for (char *p = __bss_start; p < __bss_end; p++) *p = 0;

    gfx_init(info);
    screenW = gfx_width();
    screenH = gfx_height();
    mouse_x = (int)screenW / 2;
    mouse_y = (int)screenH / 2;

    mouse_init();
    keyboard_init();

    render_scene();

    uint32_t tick = 0;
    while (1) {
        int dx = 0, dy = 0;
        uint8_t buttons = prev_buttons;
        int moved = 0;
        int mdx, mdy; uint8_t mbtn;
        while (mouse_poll(&mdx, &mdy, &mbtn)) {
            dx += mdx; dy += mdy; buttons = mbtn; moved = 1;
        }

        int scene_changed = 0;

        if (moved) {
            mouse_x += dx; mouse_y += dy; /* mouse.c already flips dy into screen-down-positive convention */
            if (mouse_x < 0) mouse_x = 0;
            if (mouse_y < 0) mouse_y = 0;
            if ((uint32_t)mouse_x > screenW - 2) mouse_x = screenW - 2;
            if ((uint32_t)mouse_y > screenH - 2) mouse_y = screenH - 2;
        }

        int left_now = buttons & 0x01;
        int left_prev = prev_buttons & 0x01;
        int left_click = left_now && !left_prev;
        int left_release = !left_now && left_prev;

        if (left_click) {
            int ty = screenH - taskbarH;
            if (rect_hit(mouse_x, mouse_y, 8, ty + 6, 90, taskbarH - 12)) {
                start_menu_open = !start_menu_open;
                scene_changed = 1;
            } else if (start_menu_open) {
                int mw = 220, mh = 190, mx = 8, my = screenH - taskbarH - mh;
                if (rect_hit(mouse_x, mouse_y, mx, my, mw, mh)) {
                    for (int i = 0; i < 3; i++) {
                        int iy = my + 40 + i * 30;
                        if (rect_hit(mouse_x, mouse_y, mx + 6, iy, mw - 12, 26)) {
                            if (i == 0) win.visible = 1;
                            start_menu_open = 0;
                            scene_changed = 1;
                        }
                    }
                } else {
                    start_menu_open = 0;
                    scene_changed = 1;
                }
            } else if (win.visible) {
                int cbx = win.x + win.w - 24, cby = win.y + 5, cbs = 18;
                if (rect_hit(mouse_x, mouse_y, cbx, cby, cbs, cbs)) {
                    win.visible = 0;
                    scene_changed = 1;
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
            if (win.x + win.w > (int)screenW) win.x = screenW - win.w;
            if (win.y + win.h > (int)(screenH - taskbarH)) win.y = screenH - taskbarH - win.h;
            scene_changed = 1;
        }

        if (scene_changed) {
            render_scene();
        } else {
            restore_cursor();
        }

        save_cursor(mouse_x, mouse_y);
        draw_cursor(mouse_x, mouse_y);

        prev_buttons = buttons;
        tick++;
        if ((tick & 0xFFF) == 0) render_scene(); /* periodic refresh keeps the clock ticking */
        delay(300);
    }
}

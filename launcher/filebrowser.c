/*
 * PocketGo S30 launcher: a file browser that starts apps (RetroArch, Ocarina
 * of Time, anything with a launch script). Started by init (/usr/libexec/s30/frontend).
 *
 * Based on the stock-firmware filebrowser by Bkacjios, ported to the
 * mainline system: SDL2 on KMS (the screen is drawn landscape and rotated
 * onto the portrait panel), the gamepad read straight from evdev, Ozone fonts
 * from retroarch-assets (ui.c).
 *
 * Opens /mnt/sd/apps (or argv[1]). D-pad/stick: move, A: open/run, B: parent
 * folder. Folders open; *.sh files run with /bin/sh; ELF binaries run
 * directly (on exFAT every file looks executable, so the file type decides).
 * While an app runs the launcher releases the display and waits for it.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <libgen.h>
#include <limits.h>
#include <linux/input.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "ui.h"

#define SCRW UI_W
#define SCRH UI_H
#define ROW_H 22
#define LIST_Y 36
#define VISIBLE_ROWS ((SCRH - LIST_Y - 8) / ROW_H)

#define DEFAULT_DIR "/mnt/sd/apps"

/* ---------------------------------------------------------------------------
 * Directory listing
 * ------------------------------------------------------------------------- */

typedef struct {
    char* name;
    bool isDir;
} Entry;

static struct {
    char path[PATH_MAX];
    Entry* entries;
    int count;
    int selected;
    int scroll;
} sDir;

static int entry_cmp(const void* a, const void* b) {
    const Entry* ea = a;
    const Entry* eb = b;

    if (ea->isDir != eb->isDir) {
        return eb->isDir - ea->isDir; // folders first
    }
    return strcasecmp(ea->name, eb->name);
}

static void dir_free(void) {
    int i;

    for (i = 0; i < sDir.count; i++) {
        free(sDir.entries[i].name);
    }
    free(sDir.entries);
    sDir.entries = NULL;
    sDir.count = 0;
}

/* Opens a folder; selects `select` if given (e.g. the folder we came from) */
static void dir_open(const char* path, const char* select) {
    char resolved[PATH_MAX];
    DIR* d;
    struct dirent* de;
    int cap = 0, i;

    if (realpath(path, resolved) == NULL || (d = opendir(resolved)) == NULL) {
        return;
    }
    dir_free();
    snprintf(sDir.path, sizeof(sDir.path), "%s", resolved);
    while ((de = readdir(d)) != NULL) {
        char full[PATH_MAX];
        struct stat st;

        if (de->d_name[0] == '.') {
            continue; // hidden, ".", ".."
        }
        snprintf(full, sizeof(full), "%s/%s", resolved, de->d_name);
        if (stat(full, &st) != 0) {
            continue;
        }
        if (sDir.count == cap) {
            cap = cap ? cap * 2 : 32;
            sDir.entries = realloc(sDir.entries, cap * sizeof(Entry));
        }
        sDir.entries[sDir.count].name = strdup(de->d_name);
        sDir.entries[sDir.count].isDir = S_ISDIR(st.st_mode);
        sDir.count++;
    }
    closedir(d);
    qsort(sDir.entries, sDir.count, sizeof(Entry), entry_cmp);
    sDir.selected = 0;
    sDir.scroll = 0;
    for (i = 0; select != NULL && i < sDir.count; i++) {
        if (strcmp(sDir.entries[i].name, select) == 0) {
            sDir.selected = i;
        }
    }
}

static void dir_parent(void) {
    char cur[PATH_MAX], name[PATH_MAX], parent[PATH_MAX];

    if (strcmp(sDir.path, "/") == 0) {
        return;
    }
    snprintf(cur, sizeof(cur), "%s", sDir.path);
    snprintf(name, sizeof(name), "%s", basename(cur));
    snprintf(cur, sizeof(cur), "%s", sDir.path);
    snprintf(parent, sizeof(parent), "%s", dirname(cur));
    dir_open(parent, name);
}

/* ---------------------------------------------------------------------------
 * Gamepad and display (ui.c)
 * ------------------------------------------------------------------------- */

enum { IN_UP, IN_DOWN, IN_A, IN_B, IN_SELECT, IN_COUNT };
static const int sPadCodes[IN_COUNT] = { BTN_DPAD_UP, BTN_DPAD_DOWN, BTN_EAST, BTN_SOUTH, BTN_SELECT };
static int sPadFd = -1;
static bool sHeld[IN_COUNT];
static int sAxisY, sAxisYCentre;

/* Drops input that built up while an app ran */
static void pad_flush(void) {
    int x = 0;

    ui_pad_poll(sPadFd, sHeld, sPadCodes, IN_COUNT);
    memset(sHeld, 0, sizeof(sHeld));
    ui_pad_axes(sPadFd, &x, &sAxisY);
}

/* Battery percentage from the AXP fuel gauge, -1 if none; *charging set
   (charger connected and still charging; "Full" counts as not charging) */
static int battery(bool* charging) {
    glob_t g;
    int pct = -1;

    *charging = false;
    if (glob("/sys/class/power_supply/*/capacity", 0, NULL, &g) == 0) {
        char status[32] = "", path[PATH_MAX];
        FILE* f = fopen(g.gl_pathv[0], "r");
        if (f != NULL) {
            if (fscanf(f, "%d", &pct) != 1) {
                pct = -1;
            }
            fclose(f);
        }
        snprintf(path, sizeof(path), "%s", g.gl_pathv[0]);
        strcpy(strrchr(path, '/'), "/status");
        f = fopen(path, "r");
        if (f != NULL) {
            if (fscanf(f, "%31s", status) == 1) {
                *charging = strcmp(status, "Charging") == 0;
            }
            fclose(f);
        }
        globfree(&g);
    }
    return pct;
}

/* Battery icon at (x, y), 45x20: filled to the level in green, yellow or
   red, the percentage inside; a lightning bolt to its left while charging */
static void draw_battery(int x, int y, int pct, bool charging) {
    const int w = 42, h = 20;
    SDL_Rect body = { x, y, w, h }, nub = { x + w, y + 6, 3, h - 12 };
    SDL_Rect fill = { x + 2, y + 2, (w - 4) * (pct > 100 ? 100 : pct) / 100, h - 4 };
    SDL_Color c = pct > 50 ? (SDL_Color){ 70, 200, 90, 255 }
                  : pct > 20 ? (SDL_Color){ 235, 190, 40, 255 }
                             : (SDL_Color){ 225, 60, 50, 255 };
    const SDL_Color shadow = { 0, 0, 0, 255 };
    char text[8];
    int tw = 0, th = 0;

    SDL_SetRenderDrawColor(gUiRenderer, 30, 30, 30, 255);
    SDL_RenderFillRect(gUiRenderer, &body);
    SDL_SetRenderDrawColor(gUiRenderer, c.r, c.g, c.b, 255);
    SDL_RenderFillRect(gUiRenderer, &fill);
    SDL_SetRenderDrawColor(gUiRenderer, 200, 200, 200, 255);
    SDL_RenderDrawRect(gUiRenderer, &body);
    SDL_RenderFillRect(gUiRenderer, &nub);

    snprintf(text, sizeof(text), "%d%%", pct);
    TTF_SizeUTF8(gUiFontSmall, text, &tw, &th);
    ui_text(gUiFontSmall, text, shadow, x + (w - tw) / 2 + 1, y + (h - th) / 2 + 1, w);
    ui_text(gUiFontSmall, text, UI_TEXT, x + (w - tw) / 2, y + (h - th) / 2, w);

    if (charging) {
        // Bolt: two triangles, 10x18
        const SDL_Color bolt = { 255, 255, 255, 255 };
        const float bx = x - 14, by = y + 1;
        SDL_Vertex v[6] = {
            { { bx + 7, by + 0 }, bolt, { 0, 0 } },  { { bx + 0, by + 10 }, bolt, { 0, 0 } },
            { { bx + 6, by + 10 }, bolt, { 0, 0 } }, { { bx + 4, by + 8 }, bolt, { 0, 0 } },
            { { bx + 10, by + 8 }, bolt, { 0, 0 } }, { { bx + 3, by + 18 }, bolt, { 0, 0 } },
        };
        SDL_RenderGeometry(gUiRenderer, NULL, v, 6, NULL, 0);
    }
}

/* ---------------------------------------------------------------------------
 * System menu (Select): reboot / shut down
 * ------------------------------------------------------------------------- */

static const char* const sMenuItems[] = { "Reboot", "Shut down", "Cancel" };
#define MENU_COUNT (int)(sizeof(sMenuItems) / sizeof(sMenuItems[0]))
static bool sMenuOpen;
static int sMenuSel;

static void render_menu(void) {
    const int w = 200, h = 24 + MENU_COUNT * ROW_H, x = (SCRW - w) / 2, y = (UI_H - h) / 2;
    SDL_Rect box = { x, y, w, h };
    int i;

    SDL_SetRenderDrawColor(gUiRenderer, 20, 20, 20, 255);
    SDL_RenderFillRect(gUiRenderer, &box);
    SDL_SetRenderDrawColor(gUiRenderer, 60, 60, 60, 255);
    SDL_RenderDrawRect(gUiRenderer, &box);
    for (i = 0; i < MENU_COUNT; i++) {
        int iy = y + 12 + i * ROW_H;

        if (i == sMenuSel) {
            SDL_Rect bar = { x + 6, iy - 1, w - 12, ROW_H };
            SDL_SetRenderDrawColor(gUiRenderer, 30, 60, 55, 255);
            SDL_RenderFillRect(gUiRenderer, &bar);
        }
        ui_text(gUiFontRegular, sMenuItems[i], i == sMenuSel ? UI_ACCENT : UI_TEXT, x + 16, iy, w - 32);
    }
}

/* Reboot / power off through init, which stops everything and syncs */
static void menu_select(void) {
    const char* cmd = sMenuSel == 0 ? "reboot" : sMenuSel == 1 ? "poweroff" : NULL;

    sMenuOpen = false;
    if (cmd != NULL) {
        printf("launcher: %s\n", cmd);
        ui_video_close();
        execlp(cmd, cmd, (char*)NULL);
        fprintf(stderr, "launcher: %s: %s\n", cmd, strerror(errno));
        if (!ui_video_open()) {
            exit(1);
        }
    }
}

static void render(void) {
    bool charging;
    int pct = battery(&charging);
    int i;

    ui_begin();

    // Header: current folder, battery
    ui_text(gUiFontBold, sDir.path, UI_ACCENT, 10, 6, SCRW - 90);
    if (pct >= 0) {
        draw_battery(SCRW - 53, 6, pct, charging);
    }
    SDL_SetRenderDrawColor(gUiRenderer, 60, 60, 60, 255);
    SDL_RenderDrawLine(gUiRenderer, 8, LIST_Y - 6, SCRW - 8, LIST_Y - 6);

    // List, scrolled to keep the selection visible
    if (sDir.selected < sDir.scroll) {
        sDir.scroll = sDir.selected;
    } else if (sDir.selected >= sDir.scroll + VISIBLE_ROWS) {
        sDir.scroll = sDir.selected - VISIBLE_ROWS + 1;
    }
    if (sDir.count == 0) {
        ui_text(gUiFontRegular, "(empty)", UI_DIM, 18, LIST_Y, SCRW - 36);
    }
    for (i = sDir.scroll; i < sDir.count && i < sDir.scroll + VISIBLE_ROWS; i++) {
        int y = LIST_Y + (i - sDir.scroll) * ROW_H;
        char label[PATH_MAX + 2];

        if (i == sDir.selected) {
            SDL_Rect bar = { 8, y - 1, SCRW - 16, ROW_H };
            SDL_SetRenderDrawColor(gUiRenderer, 30, 60, 55, 255);
            SDL_RenderFillRect(gUiRenderer, &bar);
        }
        snprintf(label, sizeof(label), "%s%s", sDir.entries[i].name, sDir.entries[i].isDir ? "/" : "");
        ui_text(gUiFontRegular, label, i == sDir.selected ? UI_ACCENT : sDir.entries[i].isDir ? UI_DIM : UI_TEXT,
                18, y, SCRW - 36);
    }
    if (sMenuOpen) {
        render_menu();
    }

    ui_present();
}

/* ---------------------------------------------------------------------------
 * Running apps
 * ------------------------------------------------------------------------- */

static bool is_elf(const char* path) {
    unsigned char m[4];
    FILE* f = fopen(path, "rb");
    bool elf = false;

    if (f != NULL) {
        elf = fread(m, 1, 4, f) == 4 && m[0] == 0x7F && m[1] == 'E' && m[2] == 'L' && m[3] == 'F';
        fclose(f);
    }
    return elf;
}

static void run(const char* path) {
    const char* dot = strrchr(path, '.');
    bool script = dot != NULL && strcmp(dot, ".sh") == 0;
    char dir[PATH_MAX];
    pid_t pid;

    if (!script && !is_elf(path)) {
        return; // not something we can start
    }
    printf("launcher: running %s\n", path);
    ui_video_close();
    snprintf(dir, sizeof(dir), "%s", path);
    pid = fork();
    if (pid == 0) {
        if (chdir(dirname(dir)) != 0) {
            _exit(127);
        }
        if (script) {
            execl("/bin/sh", "sh", path, (char*)NULL);
        } else {
            execl(path, path, (char*)NULL);
        }
        fprintf(stderr, "launcher: exec %s: %s\n", path, strerror(errno));
        _exit(127);
    } else if (pid > 0) {
        int status;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        printf("launcher: %s exited (status 0x%x)\n", path, status);
    }
    if (!ui_video_open()) {
        exit(1); // init respawns the frontend
    }
    pad_flush();
}

/* ---------------------------------------------------------------------------
 * Main loop
 * ------------------------------------------------------------------------- */

#define REPEAT_DELAY_MS 350
#define REPEAT_RATE_MS 70

int main(int argc, char** argv) {
    Uint32 nextRepeat = 0, lastRender = 0;
    int moveDir = 0;
    bool dirty = true, prevA = false, prevB = false, prevSelect = false;

    setvbuf(stdout, NULL, _IOLBF, 0);
    if (!ui_init()) {
        return 1;
    }
    sPadFd = ui_pad_open();
    pad_flush();
    sAxisYCentre = sAxisY;
    dir_open(argc > 1 ? argv[1] : DEFAULT_DIR, NULL);
    if (sDir.path[0] == 0) {
        dir_open("/", NULL);
    }

    for (;;) {
        Uint32 now = SDL_GetTicks();
        int dir;
        bool a, b, select;

        {
            int x = 0, y = sAxisY;
            if (ui_pad_poll(sPadFd, sHeld, sPadCodes, IN_COUNT)) {
                dirty = true;
            }
            ui_pad_axes(sPadFd, &x, &sAxisY);
            if (y != sAxisY) {
                dirty = true;
            }
        }
        // Up/down from the D-pad or the stick (raw 0-255, up = low)
        dir = sHeld[IN_UP] || sAxisY < sAxisYCentre - 50 ? -1 : sHeld[IN_DOWN] || sAxisY > sAxisYCentre + 50 ? 1 : 0;
        a = sHeld[IN_A];
        b = sHeld[IN_B];
        select = sHeld[IN_SELECT];
        if (sMenuOpen) {
            if (dir != moveDir && dir != 0) {
                sMenuSel = (sMenuSel + dir + MENU_COUNT) % MENU_COUNT;
                dirty = true;
            }
            moveDir = dir;
            if (a && !prevA) {
                menu_select();
                dirty = true;
            } else if ((b && !prevB) || (select && !prevSelect)) {
                sMenuOpen = false;
                dirty = true;
            }
            prevA = a;
            prevB = b;
            prevSelect = select;
            if (dirty) {
                render();
                lastRender = now;
                dirty = false;
            }
            SDL_Delay(16);
            continue;
        }
        if (select && !prevSelect) {
            sMenuOpen = true;
            sMenuSel = MENU_COUNT - 1; // Cancel
            prevSelect = select;
            prevA = a;
            prevB = b;
            render();
            lastRender = now;
            continue;
        }
        prevSelect = select;
        if (dir != moveDir) {
            moveDir = dir;
            if (dir != 0 && sDir.count > 0) {
                sDir.selected = (sDir.selected + dir + sDir.count) % sDir.count;
                nextRepeat = now + REPEAT_DELAY_MS;
                dirty = true;
            }
        } else if (dir != 0 && sDir.count > 0 && now >= nextRepeat) {
            int s = sDir.selected + dir;
            if (s >= 0 && s < sDir.count) { // repeat stops at the ends
                sDir.selected = s;
                dirty = true;
            }
            nextRepeat = now + REPEAT_RATE_MS;
        }

        if (a && !prevA && sDir.count > 0) {
            Entry* e = &sDir.entries[sDir.selected];
            char path[PATH_MAX * 2];
            snprintf(path, sizeof(path), "%s/%s", strcmp(sDir.path, "/") == 0 ? "" : sDir.path, e->name);
            if (e->isDir) {
                dir_open(path, NULL);
            } else {
                run(path);
                a = b = false;
            }
            dirty = true;
        } else if (b && !prevB) {
            dir_parent();
            dirty = true;
        }
        prevA = a;
        prevB = b;

        if (dirty || now - lastRender > 2000) { // battery refresh
            render();
            lastRender = now;
            dirty = false;
        }
        SDL_Delay(16);
    }
}

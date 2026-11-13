/*
 * PocketGo S30 launcher: starts apps (RetroArch, Ocarina of Time, anything
 * with a launch script) and shows screenshots, with a file browser behind its
 * Files tile. Started by init (/usr/libexec/s30/frontend).
 *
 * Based on the stock-firmware filebrowser by Bkacjios, ported to the
 * mainline system: SDL2 on KMS (the screen is drawn landscape and rotated
 * onto the portrait panel), the gamepad read straight from evdev, text in
 * the system menu's font (ui.c).
 *
 * Home: Screenshots (/mnt/sd/screenshots, where the system menu's
 * Screenshot saves) and Files, then the apps in /mnt/sd/apps (or argv[1]).
 * Files opens a browser at the card (/mnt/sd): everything listed, B up to
 * /, X back home. D-pad/stick: move, A: open/run, B: parent folder, Y: tiles
 * or a list (saved in ~/.config/s30/launcher_view; the list pages with
 * left/right). Folders open; *.sh files run with /bin/sh;
 * ELF binaries run directly (on exFAT every file looks executable, so the
 * file type decides). While an app runs the launcher releases the display
 * and waits for it. The app's output (stdout, stderr) goes to the launcher's
 * own (the frontend's log) and to the text console (tty1, cleared first),
 * which s30-display shows while no app holds the display: an app that only
 * prints is seen on screen. When it exits, the launcher comes straight back.
 *
 * Icons: an image next to an entry with the same name (Ocarina of Time.png
 * for Ocarina of Time.sh, Folder.png for Folder/) is its tile and is not
 * listed itself. Other images (png, jpg, bmp) are listed as thumbnails and
 * open in the viewer: left/right step through the folder's images, B goes
 * back. In Screenshots (and its viewer) X deletes the picture, after a
 * confirmation (A delete, B cancel).
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <libgen.h>
#include <limits.h>
#include <linux/input.h>
#include <poll.h>
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

#define DEFAULT_DIR "/mnt/sd/apps"
#define SHOTS_DIR "/mnt/sd/screenshots"
#define FILES_DIR "/mnt/sd" // where the Files tile's browser starts

/* Grid: 4 x 2 tiles under the header, the footer below */
#define HEADER_H 34
#define FOOTER_Y (SCRH - 22)
#define COLS 4
#define ROWS 2
#define TILE_W 104
#define TILE_H 116
#define PITCH_X 112
#define PITCH_Y 124
#define GRID_X ((SCRW - (COLS * PITCH_X - (PITCH_X - TILE_W))) / 2)
#define GRID_Y 40
#define ICON_BOX 72  // the built-in icons' square inside a tile (pictures: the tile's width)
#define LABEL_COLS ((TILE_W - 8) / UI_CHAR_W) // label lines of 12 characters
#define THUMB_DIM 160 // thumbnails are decoded to at most this (2x the box)
#define LOADS_PER_FRAME 2
/* List: a row per entry, a small icon and the name */
#define LIST_ROW_H 24
#define LIST_ROWS ((FOOTER_Y - 2 - GRID_Y) / LIST_ROW_H)
#define LIST_ICON 20
#define VIEW_FILE "/.config/s30/launcher_view" // under $HOME: "list" or "grid"

static const SDL_Color kBgTop = { 34, 30, 64, 255 };
static const SDL_Color kBgBottom = { 4, 4, 10, 255 };
static const SDL_Color kTile = { 32, 30, 52, 255 };
static const SDL_Color kTileEdge = { 60, 56, 90, 255 };
static const SDL_Color kSelected = { 0x80, 0x60, 0xe0, 110 }; // as the overlay menus
static const SDL_Color kSelectedEdge = { 0x80, 0x60, 0xe0, 255 };
static const SDL_Color kLine = { 70, 64, 110, 255 };

/* ---------------------------------------------------------------------------
 * Directory listing
 * ------------------------------------------------------------------------- */

typedef enum { K_DIR, K_APP, K_IMAGE, K_FILE } Kind;

typedef struct {
    char* name;     // as listed
    char* path;     // full path
    char* iconPath; // image shown on the tile (images: the file itself), or NULL
    Kind kind;
    SDL_Texture* icon;
    int iconW, iconH;
    bool iconTried;
} Entry;

static struct {
    char path[PATH_MAX];
    Entry* entries;
    int count, cap;
    int selected;
    int scroll; // first visible row
} sDir;

static char sStartDir[PATH_MAX];
static bool sBrowsing; // in Files: every entry listed; else home or Screenshots
static bool sList;     // the list view, else tiles (Y)

static void view_mode_path(char* path, size_t size) {
    const char* home = getenv("HOME");
    snprintf(path, size, "%s%s", home != NULL ? home : "", VIEW_FILE);
}

static void view_mode_load(void) {
    char path[PATH_MAX], mode[8] = "";
    FILE* f;

    view_mode_path(path, sizeof(path));
    f = fopen(path, "r");
    if (f != NULL) {
        sList = fscanf(f, "%7s", mode) == 1 && strcmp(mode, "list") == 0;
        fclose(f);
    }
}

static void view_mode_save(void) {
    char path[PATH_MAX];
    FILE* f;

    view_mode_path(path, sizeof(path));
    f = fopen(path, "w");
    if (f != NULL) {
        fputs(sList ? "list\n" : "grid\n", f);
        fclose(f);
    }
}

static const char* extension(const char* name) {
    const char* dot = strrchr(name, '.');
    return dot != NULL && dot != name ? dot : "";
}

static bool is_image_name(const char* name) {
    const char* ext = extension(name);
    return strcasecmp(ext, ".png") == 0 || strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0 ||
           strcasecmp(ext, ".bmp") == 0;
}

/* Name without the extension, for matching icons to entries and for labels */
static void stem(const Entry* e, char* out, size_t size) {
    size_t n = strlen(e->name);

    if (e->kind != K_DIR) {
        n -= strlen(extension(e->name));
    }
    snprintf(out, size, "%.*s", (int)(n < size ? n : size - 1), e->name);
}

static int entry_cmp(const void* a, const void* b) {
    const Entry* ea = a;
    const Entry* eb = b;

    if ((ea->kind == K_DIR) != (eb->kind == K_DIR)) {
        return (eb->kind == K_DIR) - (ea->kind == K_DIR); // folders first
    }
    return strcasecmp(ea->name, eb->name);
}

static void entry_free(Entry* e) {
    SDL_DestroyTexture(e->icon);
    free(e->name);
    free(e->path);
    free(e->iconPath);
    memset(e, 0, sizeof(*e));
}

static void dir_free(void) {
    int i;

    for (i = 0; i < sDir.count; i++) {
        entry_free(&sDir.entries[i]);
    }
    free(sDir.entries);
    sDir.entries = NULL;
    sDir.count = 0;
    sDir.cap = 0;
}

static Entry* entry_add(const char* name, const char* path, Kind kind) {
    Entry* e;

    if (sDir.count == sDir.cap) {
        sDir.cap = sDir.cap ? sDir.cap * 2 : 32;
        sDir.entries = realloc(sDir.entries, sDir.cap * sizeof(Entry));
    }
    e = &sDir.entries[sDir.count++];
    memset(e, 0, sizeof(*e));
    e->name = strdup(name);
    e->path = strdup(path);
    e->kind = kind;
    return e;
}

/* Images named like another entry become its icon and leave the list */
static void attach_icons(void) {
    int i, j, n = 0;

    for (i = 0; i < sDir.count; i++) {
        Entry* img = &sDir.entries[i];
        char s[NAME_MAX + 1];
        bool used = false;

        if (img->kind != K_IMAGE) {
            continue;
        }
        stem(img, s, sizeof(s));
        for (j = 0; j < sDir.count && !used; j++) {
            Entry* e = &sDir.entries[j];
            char t[NAME_MAX + 1];

            if (e->name == NULL || e->kind == K_IMAGE || e->iconPath != NULL) {
                continue; // dropped, an image, or has its icon
            }
            stem(e, t, sizeof(t));
            if (strcasecmp(s, t) == 0) {
                e->iconPath = img->path;
                img->path = NULL;
                used = true;
            }
        }
        if (used) {
            entry_free(img); // name NULL: dropped below
        }
    }
    for (i = 0; i < sDir.count; i++) {
        if (sDir.entries[i].name != NULL) {
            sDir.entries[n++] = sDir.entries[i];
        }
    }
    sDir.count = n;
    for (i = 0; i < sDir.count; i++) {
        Entry* e = &sDir.entries[i];
        if (e->kind == K_IMAGE) {
            e->iconPath = strdup(e->path);
        }
    }
}

/* Opens a folder; selects `select` if given (e.g. the folder we came from) */
static void dir_open(const char* path, const char* select) {
    char resolved[PATH_MAX];
    DIR* d;
    struct dirent* de;
    struct stat st;
    int i;

    if (realpath(path, resolved) == NULL || (d = opendir(resolved)) == NULL) {
        return;
    }
    dir_free();
    snprintf(sDir.path, sizeof(sDir.path), "%s", resolved);
    while ((de = readdir(d)) != NULL) {
        char full[PATH_MAX];
        Kind kind;

        if (de->d_name[0] == '.') {
            continue; // hidden, ".", ".."
        }
        snprintf(full, sizeof(full), "%s/%s", resolved, de->d_name);
        if (stat(full, &st) != 0) {
            continue;
        }
        kind = S_ISDIR(st.st_mode)                            ? K_DIR
               : strcasecmp(extension(de->d_name), ".sh") == 0 ? K_APP
               : is_image_name(de->d_name)                     ? K_IMAGE
                                                               : K_FILE;
        entry_add(de->d_name, full, kind);
    }
    closedir(d);
    attach_icons();
    if (!sBrowsing && strcmp(resolved, sStartDir) == 0) { // home: the apps only
        int n = 0;

        for (i = 0; i < sDir.count; i++) {
            if (sDir.entries[i].kind == K_APP) {
                sDir.entries[n++] = sDir.entries[i];
            } else {
                entry_free(&sDir.entries[i]);
            }
        }
        sDir.count = n;
        qsort(sDir.entries, sDir.count, sizeof(Entry), entry_cmp);
        // Screenshots and Files first
        n = 0;
        if (stat(SHOTS_DIR, &st) == 0 && S_ISDIR(st.st_mode)) {
            entry_add("Screenshots", SHOTS_DIR, K_DIR);
            n++;
        }
        entry_add("Files", FILES_DIR, K_DIR);
        n++;
        {
            Entry front[2];

            memcpy(front, &sDir.entries[sDir.count - n], n * sizeof(Entry));
            memmove(&sDir.entries[n], &sDir.entries[0], (sDir.count - n) * sizeof(Entry));
            memcpy(sDir.entries, front, n * sizeof(Entry));
        }
    } else {
        qsort(sDir.entries, sDir.count, sizeof(Entry), entry_cmp);
    }
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

    if (!sBrowsing && strcmp(sDir.path, sStartDir) == 0) {
        return; // home
    }
    if (!sBrowsing) { // Screenshots: back home, to its tile
        dir_open(sStartDir, "Screenshots");
        return;
    }
    if (strcmp(sDir.path, "/") == 0) {
        return; // the top (X leaves Files)
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

enum { IN_UP, IN_DOWN, IN_LEFT, IN_RIGHT, IN_A, IN_B, IN_X, IN_Y, IN_COUNT };
static const int sPadCodes[IN_COUNT] = { BTN_DPAD_UP, BTN_DPAD_DOWN, BTN_DPAD_LEFT, BTN_DPAD_RIGHT,
                                         BTN_EAST,    BTN_SOUTH,     BTN_NORTH,     BTN_WEST };
static int sPadFd = -1;
static bool sHeld[IN_COUNT], sPressed[IN_COUNT]; // pressed: since the last poll
static int sAxisX, sAxisY, sAxisXCentre, sAxisYCentre;

/* Drops input that built up while an app ran */
static void pad_flush(void) {
    ui_pad_poll(sPadFd, sHeld, NULL, sPadCodes, IN_COUNT);
    memset(sHeld, 0, sizeof(sHeld));
    ui_pad_axes(sPadFd, &sAxisX, &sAxisY);
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

/* Battery, right-aligned to `right` and centred on `cy`, as in the system
   menu (s30-display): filled to the level in green, yellow or red with the
   percentage inside, a pixel bolt to its left while charging (the battery
   itself never moves) */
static void draw_battery(int right, int cy, int pct, bool charging) {
    static const char* const bolt[7] = { "...##", "..##.", ".##..", "#####", "..##.", ".##..", "##..." };
    const int bw = 42, bh = 18, y = cy - bh / 2, x = right - 3 - bw;
    SDL_Color c = pct > 50 ? (SDL_Color){ 70, 200, 90, 255 }
                  : pct > 20 ? (SDL_Color){ 235, 190, 40, 255 }
                             : (SDL_Color){ 225, 60, 50, 255 };
    char text[8];
    int len, i, j;

    pct = pct > 100 ? 100 : pct;
    ui_fill((SDL_Rect){ x, y, bw, bh }, (SDL_Color){ 200, 200, 200, 255 });
    ui_fill((SDL_Rect){ x + bw, y + 5, 3, bh - 10 }, (SDL_Color){ 200, 200, 200, 255 });
    ui_fill((SDL_Rect){ x + 1, y + 1, bw - 2, bh - 2 }, (SDL_Color){ 30, 30, 30, 255 });
    ui_fill((SDL_Rect){ x + 2, y + 2, (bw - 4) * pct / 100, bh - 4 }, c);

    // Percentage inside, shadowed (the digits are font rows 2-11: centred)
    len = snprintf(text, sizeof(text), "%d%%", pct);
    ui_text(text, (SDL_Color){ 0, 0, 0, 255 }, x + (bw - len * 8) / 2 + 1, y + bh / 2 - 7 + 1, bw);
    ui_text(text, UI_TEXT, x + (bw - len * 8) / 2, y + bh / 2 - 7, bw);

    if (charging) {
        for (j = 0; j < 7; j++) {
            for (i = 0; i < 5; i++) {
                if (bolt[j][i] == '#') {
                    ui_fill((SDL_Rect){ x - 14 + i * 2, y + 2 + j * 2, 2, 2 }, UI_TEXT);
                }
            }
        }
    }
}

/* A triangle, anti-aliased: each pixel blended by its coverage (4x4
   samples against the three edges) */
static void fill_triangle(float ax, float ay, float bx, float by, float cx, float cy, SDL_Color c) {
    const float area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    const int x0 = (int)SDL_floorf(SDL_min(ax, SDL_min(bx, cx))), x1 = (int)SDL_ceilf(SDL_max(ax, SDL_max(bx, cx)));
    const int y0 = (int)SDL_floorf(SDL_min(ay, SDL_min(by, cy))), y1 = (int)SDL_ceilf(SDL_max(ay, SDL_max(by, cy)));
    int x, y, i, j;

    if (area == 0.0f) {
        return;
    }
    for (y = y0; y < y1; y++) {
        for (x = x0; x < x1; x++) {
            int hits = 0;

            for (j = 0; j < 4; j++) {
                for (i = 0; i < 4; i++) {
                    float px = x + (i + 0.5f) / 4, py = y + (j + 0.5f) / 4;
                    // Same side of all three edges as the triangle's winding
                    float e0 = ((bx - ax) * (py - ay) - (by - ay) * (px - ax)) / area;
                    float e1 = ((cx - bx) * (py - by) - (cy - by) * (px - bx)) / area;
                    float e2 = ((ax - cx) * (py - cy) - (ay - cy) * (px - cx)) / area;

                    hits += e0 >= 0 && e1 >= 0 && e2 >= 0;
                }
            }
            if (hits != 0) {
                ui_fill((SDL_Rect){ x, y, 1, 1 }, (SDL_Color){ c.r, c.g, c.b, (Uint8)(c.a * hits / 16) });
            }
        }
    }
}

/* Built-in icons, drawn into `box` */
static void draw_default_icon(const Entry* e, SDL_Rect box, bool selected) {
    const SDL_Color body = selected ? (SDL_Color){ 230, 225, 255, 255 } : (SDL_Color){ 170, 160, 220, 255 };
    const SDL_Color shade = selected ? (SDL_Color){ 180, 170, 240, 255 } : (SDL_Color){ 120, 110, 170, 255 };
    const int x = box.x, y = box.y, w = box.w, h = box.h;
    int i;

    switch (e->kind) {
    case K_DIR: // folder: tab, back, front
        ui_fill((SDL_Rect){ x + w / 8, y + h / 5, w / 3, h / 8 }, shade);
        ui_fill((SDL_Rect){ x + w / 8, y + h / 5 + h / 8, w * 3 / 4, h / 2 }, shade);
        ui_fill((SDL_Rect){ x + w / 8 + 2, y + h / 5 + h / 8 + h / 10, w * 3 / 4, h / 2 - h / 10 }, body);
        if (strcmp(e->path, SHOTS_DIR) == 0) { // a small picture on the front: mountains and a sun
            const SDL_Color frame = { 60, 50, 90, 255 }, sky = { 95, 85, 145, 255 };
            const int pw = w * 3 / 8, ph = h / 4, px = x + w / 8 + 2 + (w * 3 / 4 - pw) / 2,
                      py = y + h / 5 + h / 8 + h / 10 + (h / 2 - h / 10 - ph) / 2;
            const float bot = (float)(py + ph - 2);

            ui_fill((SDL_Rect){ px, py, pw, ph }, frame);
            ui_fill((SDL_Rect){ px + 2, py + 2, pw - 4, ph - 4 }, sky);
            fill_triangle(px + 2, bot, px + pw * 0.42f, py + ph * 0.35f, px + pw * 0.75f, bot, body);
            fill_triangle(px + pw * 0.45f, bot, px + pw * 0.7f, py + ph * 0.55f, px + pw - 2, bot, body);
            ui_fill((SDL_Rect){ px + pw - 2 - pw / 5, py + 2 + ph / 8, pw / 8, pw / 8 }, body);
        }
        break;
    case K_APP: { // play triangle in a tile, centred on its centroid
        const float th = h / 3.0f, tw = th * 0.87f, left = x + w / 2.0f - tw / 3;

        ui_fill((SDL_Rect){ x + w / 6, y + h / 6, w * 2 / 3, h * 2 / 3 }, shade);
        fill_triangle(left, y + h / 3.0f, left + tw, y + h / 2.0f, left, y + h * 2 / 3.0f, body);
        break;
    }
    default: // a sheet with a folded corner
        ui_fill((SDL_Rect){ x + w / 4, y + h / 6, w / 2, h * 2 / 3 }, body);
        ui_fill((SDL_Rect){ x + w * 3 / 4 - w / 6, y + h / 6, w / 6, h / 6 }, shade);
        for (i = 0; i < 3; i++) {
            ui_fill((SDL_Rect){ x + w / 4 + 6, y + h / 2 - 4 + i * 8, w / 2 - 12, 2 }, shade);
        }
        break;
    }
}

/* Decodes up to LOADS_PER_FRAME missing icons among the visible tiles; true
   while more are left (render again) */
static bool load_icons(int first, int last) {
    int i, loads = 0;

    for (i = first; i <= last && i < sDir.count; i++) {
        Entry* e = &sDir.entries[i];

        if (e->iconTried || e->iconPath == NULL) {
            continue;
        }
        if (loads == LOADS_PER_FRAME) {
            return true;
        }
        e->icon = ui_image_load(e->iconPath, THUMB_DIM, &e->iconW, &e->iconH);
        e->iconTried = true;
        loads++;
    }
    return false;
}

/* Label on up to two lines, broken at a space where possible */
static void draw_label(const char* text, SDL_Color color, int x, int y) {
    char line[LABEL_COLS + 1];
    int len = (int)strlen(text), n = len, i;

    if (len > LABEL_COLS) {
        n = 0;
        for (i = 1; i <= LABEL_COLS && text[i] != '\0'; i++) {
            if (text[i] == ' ') {
                n = i;
            }
        }
        if (n == 0) {
            n = LABEL_COLS;
        }
    }
    snprintf(line, sizeof(line), "%.*s", n, text);
    ui_text(line, color, x + (TILE_W - n * UI_CHAR_W) / 2, y, TILE_W);
    if (n < len) {
        const char* rest = text + n + (text[n] == ' ');
        int m = (int)strlen(rest) < LABEL_COLS ? (int)strlen(rest) : LABEL_COLS;
        snprintf(line, sizeof(line), "%.*s", m, rest);
        ui_text(line, color, x + (TILE_W - m * UI_CHAR_W) / 2, y + UI_CHAR_H, TILE_W);
    }
}

static void draw_tile(const Entry* e, int x, int y, bool selected) {
    const SDL_Rect tile = { x, y, TILE_W, TILE_H };
    // Pictures (thumbnails and icon images) get the tile's width; the
    // built-in icons a square
    const SDL_Rect iconBox = e->icon != NULL ? (SDL_Rect){ x + 4, y + 6, TILE_W - 8, ICON_BOX }
                                             : (SDL_Rect){ x + (TILE_W - ICON_BOX) / 2, y + 6, ICON_BOX, ICON_BOX };
    char label[NAME_MAX + 1];

    ui_fill(tile, kTile);
    if (selected) {
        ui_fill(tile, kSelected);
        ui_box(tile, kSelectedEdge, 2);
    } else {
        ui_box(tile, kTileEdge, 1);
    }
    if (e->icon != NULL) {
        ui_image_fit(e->icon, e->iconW, e->iconH, iconBox);
    } else {
        draw_default_icon(e, iconBox, selected);
    }
    stem(e, label, sizeof(label));
    draw_label(label, selected ? UI_TEXT : UI_DIM, x, y + ICON_BOX + 12);
}

static void draw_header(const char* title) {
    bool charging;
    int pct = battery(&charging);

    ui_text(title, UI_ACCENT, 12, (HEADER_H - UI_CHAR_H) / 2, SCRW - 90);
    if (pct >= 0) {
        draw_battery(SCRW - 10, HEADER_H / 2, pct, charging);
    }
    ui_fill((SDL_Rect){ 8, HEADER_H - 1, SCRW - 16, 1 }, kLine);
}

/* Screenshots (not Files) with a picture selected: X deletes it */
static bool can_delete(void) {
    return !sBrowsing && strcmp(sDir.path, SHOTS_DIR) == 0 && sDir.count > 0 &&
           sDir.entries[sDir.selected].kind == K_IMAGE;
}

static bool sConfirm; // "Delete ...?" is up (A deletes, B cancels)

/* The confirmation, a box over whatever is drawn */
static void draw_confirm(void) {
    const Entry* e = &sDir.entries[sDir.selected];
    const SDL_Rect box = { SCRW / 2 - 150, SCRH / 2 - 44, 300, 88 };
    char text[NAME_MAX + 16];
    // "(A) Delete   (B) Cancel", centred under the question
    const int hintsW = 2 * (BUTTON_SIZE + 5) + ui_text_width("Delete") + 20 + ui_text_width("Cancel");
    int x = box.x + (box.w - hintsW) / 2, w;

    ui_fill((SDL_Rect){ 0, 0, SCRW, SCRH }, (SDL_Color){ 0, 0, 0, 120 });
    ui_fill(box, kTile);
    ui_box(box, kSelectedEdge, 2);
    snprintf(text, sizeof(text), "Delete %s?", e->name);
    w = ui_text_width(text);
    w = w > box.w - 24 ? box.w - 24 : w;
    ui_text(text, UI_TEXT, box.x + (box.w - w) / 2, box.y + 18, box.w - 24);
    ui_button(kLetterA, x, box.y + 54);
    x += BUTTON_SIZE + 5;
    ui_text("Delete", UI_TEXT, x, box.y + 53, SCRW);
    x += ui_text_width("Delete") + 20;
    ui_button(kLetterB, x, box.y + 54);
    x += BUTTON_SIZE + 5;
    ui_text("Cancel", UI_DIM, x, box.y + 53, SCRW);
}

/* "(A) action   (B) Back" (where B goes somewhere: not home, not / in
   Files), in Files "(X) Exit" (in Screenshots "(X) Delete"), then "(Y)
   List" or "(Y) Tiles" at the left; `name` right-aligned */
static void draw_footer(const char* action, const char* name) {
    const int ty = FOOTER_Y + 3, by = FOOTER_Y + 4;
    int x = 12;

    ui_fill((SDL_Rect){ 8, FOOTER_Y - 2, SCRW - 16, 1 }, kLine);
    if (action != NULL) {
        ui_button(kLetterA, x, by);
        x += BUTTON_SIZE + 5;
        ui_text(action, UI_DIM, x, ty, SCRW);
        x += ui_text_width(action) + 14;
    }
    if (sBrowsing ? strcmp(sDir.path, "/") != 0 : strcmp(sDir.path, sStartDir) != 0) {
        ui_button(kLetterB, x, by);
        x += BUTTON_SIZE + 5;
        ui_text("Back", UI_DIM, x, ty, SCRW);
        x += ui_text_width("Back") + 14;
    }
    if (sBrowsing) {
        ui_button(kLetterX, x, by);
        x += BUTTON_SIZE + 5;
        ui_text("Exit", UI_DIM, x, ty, SCRW);
        x += ui_text_width("Exit") + 14;
    } else if (can_delete()) {
        ui_button(kLetterX, x, by);
        x += BUTTON_SIZE + 5;
        ui_text("Delete", UI_DIM, x, ty, SCRW);
        x += ui_text_width("Delete") + 14;
    }
    ui_button(kLetterY, x, by);
    x += BUTTON_SIZE + 5;
    ui_text(sList ? "Tiles" : "List", UI_DIM, x, ty, SCRW);
    x += ui_text_width(sList ? "Tiles" : "List") + 16;
    if (name != NULL) {
        int w = ui_text_width(name), max = SCRW - 12 - x;
        if (w > max) {
            w = max;
        }
        ui_text(name, UI_TEXT, SCRW - 12 - w, ty, w);
    }
}

/* A list row: the icon (or the built-in one) small, then the name */
static void draw_row(const Entry* e, int y, bool selected) {
    const SDL_Rect row = { 8, y, SCRW - 22, LIST_ROW_H - 2 };
    const SDL_Rect iconBox = { row.x + 4, y + (LIST_ROW_H - 2 - LIST_ICON) / 2, LIST_ICON, LIST_ICON };
    char label[NAME_MAX + 1];

    if (selected) {
        ui_fill(row, kSelected);
        ui_box(row, kSelectedEdge, 1);
    }
    if (e->icon != NULL) {
        ui_image_fit(e->icon, e->iconW, e->iconH, iconBox);
    } else {
        draw_default_icon(e, iconBox, selected);
    }
    stem(e, label, sizeof(label));
    ui_text(label, selected ? UI_TEXT : UI_DIM, iconBox.x + LIST_ICON + 10, y + (LIST_ROW_H - 2 - UI_CHAR_H) / 2,
            row.w - LIST_ICON - 22);
}

/* The folder as tiles or as a list, the header and the footer */
static void render_dir(void) {
    const int per = sList ? 1 : COLS, shown = sList ? LIST_ROWS : ROWS; // entries per row, rows shown
    const int rows = (sDir.count + per - 1) / per;
    char title[PATH_MAX];
    const char* name = NULL;
    int row = sDir.selected / per, first, last, i;
    bool more;

    // Keep the selected row visible
    if (row < sDir.scroll) {
        sDir.scroll = row;
    } else if (row >= sDir.scroll + shown) {
        sDir.scroll = row - shown + 1;
    }
    first = sDir.scroll * per;
    last = first + per * shown - 1;
    more = load_icons(first, last);

    ui_begin();
    ui_gradient((SDL_Rect){ 0, 0, SCRW, SCRH }, kBgTop, kBgBottom);
    snprintf(title, sizeof(title), "%s", sDir.path);
    draw_header(!sBrowsing && strcmp(sDir.path, sStartDir) == 0 ? "PocketGo S30"
                : sBrowsing && strcmp(sDir.path, FILES_DIR) == 0 ? "Files"
                                                                  : basename(title));

    if (sDir.count == 0) {
        ui_text("(empty)", UI_DIM, GRID_X, GRID_Y + 4, SCRW);
    }
    for (i = first; i <= last && i < sDir.count; i++) {
        if (sList) {
            draw_row(&sDir.entries[i], GRID_Y + (i - first) * LIST_ROW_H, i == sDir.selected);
        } else {
            int c = (i - first) % COLS, r = (i - first) / COLS;
            draw_tile(&sDir.entries[i], GRID_X + c * PITCH_X, GRID_Y + r * PITCH_Y, i == sDir.selected);
        }
    }
    if (rows > shown) { // scroll bar at the right edge
        int track = sList ? LIST_ROWS * LIST_ROW_H - 2 : ROWS * PITCH_Y - (PITCH_Y - TILE_H);
        int th = track * shown / rows, ty = track * sDir.scroll / rows;
        ui_fill((SDL_Rect){ SCRW - 8, GRID_Y, 3, track }, kTile);
        ui_fill((SDL_Rect){ SCRW - 8, GRID_Y + ty, 3, th }, kSelectedEdge);
    }
    if (sDir.count > 0 && !sList) {
        name = sDir.entries[sDir.selected].name; // the list shows it already
    }
    draw_footer(sDir.count == 0                               ? NULL
                : sDir.entries[sDir.selected].kind == K_DIR   ? "Open"
                : sDir.entries[sDir.selected].kind == K_IMAGE ? "View"
                                                              : "Run",
                name);
    if (sConfirm) {
        draw_confirm();
    }
    ui_present();
    if (more) {
        render_dir();
    }
}

/* ---------------------------------------------------------------------------
 * Image viewer
 * ------------------------------------------------------------------------- */

#define VIEW_DIM 960 // pictures larger than this are downsampled

static struct {
    bool open;
    SDL_Texture* tex;
    int w, h;
} sView;

static void view_close(void) {
    SDL_DestroyTexture(sView.tex);
    sView.tex = NULL;
    sView.open = false;
}

static void view_open(void) {
    const Entry* e = &sDir.entries[sDir.selected];

    view_close();
    sView.tex = ui_image_load(e->path, VIEW_DIM, &sView.w, &sView.h);
    sView.open = true;
}

/* The next image entry in `dir` (+1 / -1), or -1 */
static int view_step(int dir) {
    int i;

    for (i = sDir.selected + dir; i >= 0 && i < sDir.count; i += dir) {
        if (sDir.entries[i].kind == K_IMAGE) {
            return i;
        }
    }
    return -1;
}

static void render_view(void) {
    const Entry* e = &sDir.entries[sDir.selected];
    char pos[32];
    int i, n = 0, at = 0;

    for (i = 0; i < sDir.count; i++) {
        if (sDir.entries[i].kind == K_IMAGE) {
            n++;
            if (i == sDir.selected) {
                at = n;
            }
        }
    }
    ui_begin();
    if (sView.tex != NULL) {
        ui_image_fit(sView.tex, sView.w, sView.h, (SDL_Rect){ 0, 0, SCRW, SCRH });
    } else {
        ui_text("Can't open this picture", UI_DIM, 12, SCRH / 2 - 8, SCRW);
    }
    // Name and position in a translucent strip
    ui_fill((SDL_Rect){ 0, SCRH - 22, SCRW, 22 }, (SDL_Color){ 0, 0, 0, 150 });
    ui_text(e->name, UI_TEXT, 10, SCRH - 19, SCRW - 90);
    snprintf(pos, sizeof(pos), "%d/%d", at, n);
    ui_text(pos, UI_DIM, SCRW - 10 - ui_text_width(pos), SCRH - 19, 80);
    if (can_delete()) {
        int x = SCRW - 10 - ui_text_width(pos) - 16 - ui_text_width("Delete");
        ui_text("Delete", UI_DIM, x, SCRH - 19, SCRW);
        ui_button(kLetterX, x - 5 - BUTTON_SIZE, SCRH - 18);
    }
    if (sConfirm) {
        draw_confirm();
    }
    ui_present();
}

/* Deletes the selected screenshot; the one after it (or before, at the end)
   is selected, and shown if the viewer is open (closed when none is left) */
static void delete_selected(void) {
    const Entry* e = &sDir.entries[sDir.selected];
    char next[NAME_MAX + 1] = "";

    if (sDir.selected + 1 < sDir.count) {
        snprintf(next, sizeof(next), "%s", sDir.entries[sDir.selected + 1].name);
    } else if (sDir.selected > 0) {
        snprintf(next, sizeof(next), "%s", sDir.entries[sDir.selected - 1].name);
    }
    if (unlink(e->path) != 0) {
        fprintf(stderr, "launcher: delete %s: %s\n", e->path, strerror(errno));
        return;
    }
    sync();
    printf("launcher: deleted %s\n", e->path);
    dir_open(SHOTS_DIR, next[0] ? next : NULL);
    if (sView.open) {
        if (sDir.count > 0 && sDir.entries[sDir.selected].kind == K_IMAGE) {
            view_open();
        } else {
            view_close();
        }
    }
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

/* Copy the app's output from `out` to stdout (the log) and `tty` until it
   exits. Polled, not read to EOF: something the app left running in the
   background may keep the pipe open. */
static void relay_output(const char* path, pid_t pid, int out, int tty) {
    struct pollfd pfd = { .fd = out, .events = POLLIN };
    char buf[4096];
    bool exited = false;
    ssize_t n;

    while (!exited) {
        int status;

        exited = waitpid(pid, &status, WNOHANG) == pid;
        if (exited) {
            printf("launcher: %s exited (status 0x%x)\n", path, status);
        } else if (poll(&pfd, 1, 200) <= 0) {
            continue;
        }
        // Drain what is there (all of it once the app has gone)
        while ((n = read(out, buf, sizeof(buf))) > 0) {
            fflush(stdout);
            if (write(STDOUT_FILENO, buf, n) < 0) {
                // the log is gone: the screen still gets it
            }
            if (tty >= 0 && write(tty, buf, n) < 0) {
                // nothing to do
            }
        }
        if (n == 0) {
            pfd.fd = -1; // EOF: just wait for the exit
        }
    }
}

static void run(const char* path) {
    const char* dot = strrchr(path, '.');
    bool script = dot != NULL && strcmp(dot, ".sh") == 0;
    char dir[PATH_MAX];
    int pipefd[2], tty;
    pid_t pid;

    if (!script && !is_elf(path)) {
        return; // not something we can start
    }
    printf("launcher: running %s\n", path);
    ui_video_close();
    snprintf(dir, sizeof(dir), "%s", path);
    // The console starts empty: no earlier app's text when it shows
    tty = open("/dev/tty1", O_WRONLY | O_NOCTTY | O_CLOEXEC);
    if (tty >= 0 && write(tty, "\033[H\033[2J", 7) < 0) {
        // not fatal
    }
    if (pipe2(pipefd, O_CLOEXEC) != 0) {
        pipefd[0] = pipefd[1] = -1;
    }
    pid = fork();
    if (pid == 0) {
        if (pipefd[1] >= 0) {
            dup2(pipefd[1], STDOUT_FILENO); // dup2 clears close-on-exec
            dup2(pipefd[1], STDERR_FILENO);
        }
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
        if (pipefd[1] >= 0) {
            close(pipefd[1]); // the app's copy is the only writer: EOF at its exit
            pipefd[1] = -1;
            fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
            relay_output(path, pid, pipefd[0], tty);
        } else {
            int status;
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
            }
            printf("launcher: %s exited (status 0x%x)\n", path, status);
        }
    }
    if (pipefd[0] >= 0) {
        close(pipefd[0]);
    }
    if (pipefd[1] >= 0) {
        close(pipefd[1]); // fork failed
    }
    if (tty >= 0) {
        close(tty);
    }
    if (!ui_video_open()) {
        exit(1); // init respawns the frontend
    }
    // Textures died with the renderer: decode again on the next frame
    {
        int i;
        for (i = 0; i < sDir.count; i++) {
            sDir.entries[i].icon = NULL;
            sDir.entries[i].iconTried = false;
        }
    }
    pad_flush();
}

/* ---------------------------------------------------------------------------
 * Main loop
 * ------------------------------------------------------------------------- */

#define REPEAT_DELAY_MS 350
#define REPEAT_RATE_MS 70
#define STICK_DEAD 50

/* One step in the grid: left/right by one, up/down by a row (clamped; the
   last row reachable from above even when short). In the list: up/down by
   one, left/right by a page (clamped to the ends). */
static void grid_move(int dx, int dy) {
    int s = sDir.selected + dx + dy * COLS;

    if (sDir.count == 0) {
        return;
    }
    if (sList) {
        s = sDir.selected + dy + dx * LIST_ROWS;
        sDir.selected = s < 0 ? 0 : s >= sDir.count ? sDir.count - 1 : s;
        return;
    }
    if (dy > 0 && s >= sDir.count && sDir.selected / COLS < (sDir.count - 1) / COLS) {
        s = sDir.count - 1;
    }
    if (s >= 0 && s < sDir.count) {
        sDir.selected = s;
    }
}

int main(int argc, char** argv) {
    Uint32 nextRepeat = 0, lastRender = 0;
    int moveX = 0, moveY = 0;
    bool dirty = true, prevA = false, prevB = false, prevX = false, prevY = false;

    setvbuf(stdout, NULL, _IOLBF, 0);
    if (!ui_init()) {
        return 1;
    }
    view_mode_load();
    sPadFd = ui_pad_open();
    pad_flush();
    sAxisXCentre = sAxisX;
    sAxisYCentre = sAxisY;
    if (realpath(argc > 1 ? argv[1] : DEFAULT_DIR, sStartDir) == NULL) {
        snprintf(sStartDir, sizeof(sStartDir), "/");
    }
    dir_open(sStartDir, NULL);
    if (sDir.path[0] == 0) {
        dir_open("/", NULL);
    }

    for (;;) {
        Uint32 now = SDL_GetTicks();
        int dx, dy;
        bool a, b, xb, yb, repeat = false;

        {
            int x = sAxisX, y = sAxisY;
            if (ui_pad_poll(sPadFd, sHeld, sPressed, sPadCodes, IN_COUNT)) {
                dirty = true;
            }
            ui_pad_axes(sPadFd, &sAxisX, &sAxisY);
            if (x != sAxisX || y != sAxisY) {
                dirty = true;
            }
        }
        // Direction from the D-pad (a tap counts even if it was released
        // within the poll interval) or the stick (raw 0-255, up/left = low)
        dy = sHeld[IN_UP] || sPressed[IN_UP] || sAxisY < sAxisYCentre - STICK_DEAD           ? -1
             : sHeld[IN_DOWN] || sPressed[IN_DOWN] || sAxisY > sAxisYCentre + STICK_DEAD     ? 1
                                                                                             : 0;
        dx = sHeld[IN_LEFT] || sPressed[IN_LEFT] || sAxisX < sAxisXCentre - STICK_DEAD       ? -1
             : sHeld[IN_RIGHT] || sPressed[IN_RIGHT] || sAxisX > sAxisXCentre + STICK_DEAD   ? 1
                                                                                             : 0;
        if (dy != 0) {
            dx = 0; // one axis at a time
        }
        a = sHeld[IN_A] || sPressed[IN_A];
        b = sHeld[IN_B] || sPressed[IN_B];
        xb = sHeld[IN_X] || sPressed[IN_X];
        yb = sHeld[IN_Y] || sPressed[IN_Y];
        if (dx != moveX || dy != moveY) {
            moveX = dx;
            moveY = dy;
            nextRepeat = now + REPEAT_DELAY_MS;
            repeat = dx != 0 || dy != 0;
        } else if ((dx != 0 || dy != 0) && now >= nextRepeat) {
            nextRepeat = now + REPEAT_RATE_MS;
            repeat = true;
        }

        if (sConfirm) { // only A or B
            if (a && !prevA) {
                sConfirm = false;
                delete_selected();
                dirty = true;
            } else if (b && !prevB) {
                sConfirm = false;
                dirty = true;
            }
        } else if (xb && !prevX && can_delete()) {
            sConfirm = true;
            dirty = true;
        } else if (sView.open) {
            if (repeat && dx != 0) {
                int i = view_step(dx);
                if (i >= 0) {
                    sDir.selected = i;
                    view_open();
                    dirty = true;
                }
            }
            if (b && !prevB) {
                view_close();
                dirty = true;
            }
        } else {
            if (repeat) {
                grid_move(dx, dy);
                dirty = true;
            }
            if (a && !prevA && sDir.count > 0) {
                Entry* e = &sDir.entries[sDir.selected];
                if (e->kind == K_DIR) {
                    if (!sBrowsing && strcmp(sDir.path, sStartDir) == 0 && strcmp(e->name, "Files") == 0 &&
                        strcmp(e->path, FILES_DIR) == 0) {
                        sBrowsing = true; // the Files tile
                    }
                    dir_open(e->path, NULL);
                } else if (e->kind == K_IMAGE) {
                    view_open();
                } else {
                    run(e->path);
                    a = b = false;
                }
                dirty = true;
            } else if (b && !prevB) {
                dir_parent();
                dirty = true;
            } else if (yb && !prevY) { // tiles or list
                sList = !sList;
                sDir.scroll = 0; // render_dir brings the selection into view
                view_mode_save();
                dirty = true;
            } else if (xb && !prevX && sBrowsing) { // out of Files, from anywhere
                sBrowsing = false;
                dir_open(sStartDir, "Files");
                dirty = true;
            }
        }
        prevA = a;
        prevB = b;
        prevX = xb;
        prevY = yb;

        if (dirty || now - lastRender > 2000) { // battery refresh
            if (sView.open) {
                render_view();
            } else {
                render_dir();
            }
            lastRender = now;
            dirty = false;
        }
        SDL_Delay(16);
    }
}

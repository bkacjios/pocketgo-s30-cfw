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
 * Home: Games (/mnt/sd/games: the ports' launch scripts), ROMs
 * (/mnt/sd/roms, any folders), Apps (/mnt/sd/apps: RetroArch, tools),
 * Screenshots (/mnt/sd/screenshots, where the system menu's Screenshot
 * saves) and Files. A section lists its folders, launch scripts, ROMs and
 * pictures (other files are hidden); B goes up to the section's tile.
 * Files opens a browser at the card (/mnt/sd): everything listed, B up to
 * /, X back home. D-pad/stick: move, A: open/run, B: parent folder, Y: tiles
 * or a list (each folder remembers its own, in ~/.config/s30/launcher_view;
 * the list pages with left/right). Folders open; *.sh files run with /bin/sh;
 * ELF binaries run directly (on exFAT every file looks executable, so the
 * file type decides).
 *
 * ROMs: a file whose extension one of the installed RetroArch cores (the
 * _libretro.so files in retroarch.cfg's libretro_directory and in
 * ~/.config/retroarch/cores, described by an .info file in its
 * libretro_info_path or next to the core) supports runs in RetroArch with
 * that core.
 * Which core: the ROM's own choice, else its folder's (or a parent folder's),
 * else the first core that takes the extension (mGBA before gpSP for GBA,
 * Gambatte for GB/GBC). X on a ROM, or on a folder in ROMs, picks the core
 * from the ones that fit (a ROM can also go back to its folder's choice);
 * saved in ~/.config/s30/cores as "path=core" lines.
 *
 * While an app runs the launcher releases the display
 * and waits for it. The app's output (stdout, stderr) goes to the launcher's
 * own (the frontend's log). A launch script with a "# s30-console" line in
 * its first lines also gets it on the text console (tty1), which
 * s30-display shows while no app holds the display: an app that only prints
 * is seen on screen. The console is cleared before every app, so no earlier
 * text shows. When the app exits, the launcher comes straight back.
 *
 * Icons: an image next to an entry with the same name (Ocarina of Time.png
 * for Ocarina of Time.sh, Folder.png for Folder/) is its tile and is not
 * listed itself. Other images (png, jpg, bmp) are listed as thumbnails and
 * open in the viewer: left/right step through the folder's images, B goes
 * back. In Screenshots (and its viewer) X deletes the picture, after a
 * confirmation (A delete, B cancel).
 */
#include <ctype.h>
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

#define GAMES_DIR "/mnt/sd/games"
#define ROMS_DIR "/mnt/sd/roms"
#define APPS_DIR "/mnt/sd/apps"
#define SHOTS_DIR "/mnt/sd/screenshots"
#define FILES_DIR "/mnt/sd" // where the Files tile's browser starts
#define CORES_DIR "/usr/lib/libretro" // unless retroarch.cfg says libretro_directory
#define USER_CORES_DIR "/.config/retroarch/cores" // under $HOME: scanned as well (wins by name)
#define CORES_INFO_DIR "/usr/share/libretro/info" // unless it says libretro_info_path
#define CORES_FILE "/.config/s30/cores" // under $HOME: "path=core" lines
#define RETROARCH_CFG "/.config/retroarch/retroarch.cfg" // under $HOME

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
#define VIEW_FILE "/.config/s30/launcher_view" // under $HOME: "path=list|grid" lines ("home" for home)

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

typedef enum { K_DIR, K_APP, K_ROM, K_IMAGE, K_FILE } Kind;

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

static bool sBrowsing; // in Files: every entry listed; else home or a section
static bool sList;     // the list view, else tiles (Y)

/* Home is no folder: sDir.path is empty there */
static bool is_home(void) {
    return !sBrowsing && sDir.path[0] == 0;
}

/* The section (home tile) a path is the root of, or NULL */
static const char* section_name(const char* path) {
    return strcmp(path, GAMES_DIR) == 0   ? "Games"
           : strcmp(path, ROMS_DIR) == 0  ? "ROMs"
           : strcmp(path, APPS_DIR) == 0  ? "Apps"
           : strcmp(path, SHOTS_DIR) == 0 ? "Screenshots"
                                          : NULL;
}


/* ---------------------------------------------------------------------------
 * Settings files: "key=value" lines under $HOME (the view per folder, the
 * core per ROM or folder)
 * ------------------------------------------------------------------------- */

static void home_path(char* out, size_t size, const char* suffix) {
    const char* home = getenv("HOME");
    snprintf(out, size, "%s%s", home != NULL ? home : "/root", suffix);
}

typedef struct {
    char file[PATH_MAX];
    char** keys;
    char** values;
    int count;
} KvFile;

static void kv_load(KvFile* kv, const char* suffix) {
    char line[PATH_MAX + 64];
    FILE* f;

    home_path(kv->file, sizeof(kv->file), suffix);
    f = fopen(kv->file, "r");
    if (f == NULL) {
        return;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        char* eq = strrchr(line, '=');
        char* nl = strchr(line, '\n');
        if (nl != NULL) {
            *nl = 0;
        }
        if (eq == NULL || eq == line) {
            continue;
        }
        *eq = 0;
        kv->keys = realloc(kv->keys, (kv->count + 1) * sizeof(char*));
        kv->values = realloc(kv->values, (kv->count + 1) * sizeof(char*));
        kv->keys[kv->count] = strdup(line);
        kv->values[kv->count] = strdup(eq + 1);
        kv->count++;
    }
    fclose(f);
}

static void kv_save(const KvFile* kv) {
    char dir[PATH_MAX];
    FILE* f;
    int i;

    snprintf(dir, sizeof(dir), "%s", kv->file);
    mkdir(dirname(dir), 0755);
    f = fopen(kv->file, "w");
    if (f == NULL) {
        return;
    }
    for (i = 0; i < kv->count; i++) {
        fprintf(f, "%s=%s\n", kv->keys[i], kv->values[i]);
    }
    fclose(f);
}

static const char* kv_get(const KvFile* kv, const char* key) {
    int i;
    for (i = 0; i < kv->count; i++) {
        if (strcmp(kv->keys[i], key) == 0) {
            return kv->values[i];
        }
    }
    return NULL;
}

/* Sets (value != NULL) or removes a key, and saves */
static void kv_set(KvFile* kv, const char* key, const char* value) {
    int i;

    for (i = 0; i < kv->count; i++) {
        if (strcmp(kv->keys[i], key) == 0) {
            free(kv->keys[i]);
            free(kv->values[i]);
            kv->count--;
            memmove(&kv->keys[i], &kv->keys[i + 1], (kv->count - i) * sizeof(char*));
            memmove(&kv->values[i], &kv->values[i + 1], (kv->count - i) * sizeof(char*));
            break;
        }
    }
    if (value != NULL) {
        kv->keys = realloc(kv->keys, (kv->count + 1) * sizeof(char*));
        kv->values = realloc(kv->values, (kv->count + 1) * sizeof(char*));
        kv->keys[kv->count] = strdup(key);
        kv->values[kv->count] = strdup(value);
        kv->count++;
    }
    kv_save(kv);
}

/* ---------------------------------------------------------------------------
 * RetroArch cores: the installed ones and which runs which ROM
 * ------------------------------------------------------------------------- */

static const char* extension(const char* name);


#define MAX_CORES 32

typedef struct {
    char name[48];       // "mgba" (mgba_libretro.so)
    char path[PATH_MAX]; // the .so
    char display[96];    // from the info file, else the name
    char exts[128];      // "|gb|gbc|gba|"
} Core;

static Core sCores[MAX_CORES];
static int sNumCores;

static KvFile sCoreMap; // CORES_FILE: a ROM or folder path = core name

/* An info file's quoted value for `key`, or "" */
static void info_value(const char* path, const char* key, char* out, size_t size) {
    FILE* f = fopen(path, "r");
    char line[512];
    size_t klen = strlen(key);

    out[0] = 0;
    if (f == NULL) {
        return;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        char* q;
        if (strncmp(line, key, klen) != 0 || line[klen] != ' ') {
            continue;
        }
        q = strchr(line, '"');
        if (q != NULL) {
            char* end = strchr(q + 1, '"');
            if (end != NULL) {
                *end = 0;
            }
            snprintf(out, size, "%s", q + 1);
        }
        break;
    }
    fclose(f);
}

static char sCoresDir[PATH_MAX], sCoresInfoDir[PATH_MAX];

/* A "key = "value"" setting of retroarch.cfg, ~ expanded, else `fallback` */
static void cfg_dir(const char* key, const char* fallback, char* out, size_t size) {
    char cfg[PATH_MAX], value[PATH_MAX];

    home_path(cfg, sizeof(cfg), RETROARCH_CFG);
    info_value(cfg, key, value, sizeof(value));
    if (value[0] == '~') {
        home_path(out, size, value + 1);
    } else {
        snprintf(out, size, "%s", value[0] ? value : fallback);
    }
}

static const Core* core_by_name(const char* name);

/* The cores in one folder; a name already listed (from the user's folder,
   scanned first) is kept */
static void cores_scan(const char* folder) {
    char pattern[PATH_MAX];
    glob_t g;
    size_t i;

    snprintf(pattern, sizeof(pattern), "%s/*_libretro.so", folder);
    if (glob(pattern, 0, NULL, &g) != 0) {
        return;
    }
    for (i = 0; i < g.gl_pathc && sNumCores < MAX_CORES; i++) {
        Core* c = &sCores[sNumCores];
        char base[NAME_MAX + 1], info[PATH_MAX], exts[128];
        char* suffix;

        snprintf(base, sizeof(base), "%s", basename(g.gl_pathv[i]));
        suffix = strstr(base, "_libretro.so");
        if (suffix == NULL) {
            continue;
        }
        *suffix = 0;
        if (core_by_name(base) != NULL) {
            continue;
        }
        snprintf(c->name, sizeof(c->name), "%s", base);
        snprintf(c->path, sizeof(c->path), "%s", g.gl_pathv[i]);
        snprintf(info, sizeof(info), "%s/%s_libretro.info", sCoresInfoDir, base);
        if (access(info, R_OK) != 0) { // else next to the core
            snprintf(info, sizeof(info), "%s/%s_libretro.info", folder, base);
        }
        info_value(info, "display_name", c->display, sizeof(c->display));
        if (c->display[0] == 0) {
            snprintf(c->display, sizeof(c->display), "%s", base);
        }
        info_value(info, "supported_extensions", exts, sizeof(exts));
        snprintf(c->exts, sizeof(c->exts), "|%s|", exts);
        sNumCores++;
    }
    globfree(&g);
}

static void cores_load(void) {
    char user[PATH_MAX];

    sNumCores = 0;
    cfg_dir("libretro_directory", CORES_DIR, sCoresDir, sizeof(sCoresDir));
    cfg_dir("libretro_info_path", CORES_INFO_DIR, sCoresInfoDir, sizeof(sCoresInfoDir));
    home_path(user, sizeof(user), USER_CORES_DIR);
    cores_scan(user);
    cores_scan(sCoresDir);
}

static const Core* core_by_name(const char* name) {
    int i;
    for (i = 0; i < sNumCores; i++) {
        if (strcmp(sCores[i].name, name) == 0) {
            return &sCores[i];
        }
    }
    return NULL;
}

/* The extension (no dot, lower case) of a file name */
static void rom_ext(const char* name, char* out, size_t size) {
    const char* e = extension(name);
    size_t i;

    snprintf(out, size, "%s", e[0] ? e + 1 : "");
    for (i = 0; out[i]; i++) {
        out[i] = tolower((unsigned char)out[i]);
    }
}

static bool core_takes(const Core* c, const char* ext) {
    char key[64];
    snprintf(key, sizeof(key), "|%s|", ext);
    return ext[0] != 0 && strstr(c->exts, key) != NULL;
}

static bool is_rom_name(const char* name) {
    char ext[32];
    int i;

    rom_ext(name, ext, sizeof(ext));
    for (i = 0; i < sNumCores; i++) {
        if (core_takes(&sCores[i], ext)) {
            return true;
        }
    }
    return false;
}

/* The default core for an extension: the preferred one when installed,
   else the first that takes it */
static const Core* core_default(const char* ext) {
    static const char* const kPreferred[][2] = { { "gba", "mgba" }, { "gb", "gambatte" }, { "gbc", "gambatte" } };
    size_t i;
    int j;

    for (i = 0; i < sizeof(kPreferred) / sizeof(kPreferred[0]); i++) {
        if (strcmp(ext, kPreferred[i][0]) == 0) {
            const Core* c = core_by_name(kPreferred[i][1]);
            if (c != NULL && core_takes(c, ext)) {
                return c;
            }
        }
    }
    for (j = 0; j < sNumCores; j++) {
        if (core_takes(&sCores[j], ext)) {
            return &sCores[j];
        }
    }
    return NULL;
}

static const char* core_map_get(const char* path) {
    return kv_get(&sCoreMap, path);
}

static void core_map_set(const char* path, const char* core) {
    kv_set(&sCoreMap, path, core);
}

/* The core set for a folder or one of its parents (the nearest), or NULL */
static const char* core_for_folder(const char* dir) {
    char cur[PATH_MAX];

    snprintf(cur, sizeof(cur), "%s", dir);
    while (strcmp(cur, "/") != 0) {
        const char* c = core_map_get(cur);
        char tmp[PATH_MAX];

        if (c != NULL) {
            return c;
        }
        snprintf(tmp, sizeof(tmp), "%s", cur);
        snprintf(cur, sizeof(cur), "%s", dirname(tmp));
    }
    return NULL;
}

/* The core a ROM runs on (installed), or NULL */
static const Core* core_for_rom(const char* path) {
    char dir[PATH_MAX], ext[32];
    const char* name = core_map_get(path);
    const Core* c;

    if (name != NULL && (c = core_by_name(name)) != NULL) {
        return c;
    }
    snprintf(dir, sizeof(dir), "%s", path);
    name = core_for_folder(dirname(dir));
    if (name != NULL && (c = core_by_name(name)) != NULL) {
        return c;
    }
    rom_ext(basename((char*)path), ext, sizeof(ext));
    return core_default(ext);
}

static KvFile sViews; // VIEW_FILE

/* The key a folder's view is saved under */
static const char* view_key(void) {
    return is_home() ? "home" : sDir.path;
}

/* Applies the folder's saved view (tiles unless it chose the list) */
static void view_apply(void) {
    const char* v = kv_get(&sViews, view_key());
    sList = v != NULL && strcmp(v, "list") == 0;
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
               : is_rom_name(de->d_name)                       ? K_ROM
                                                               : K_FILE;
        entry_add(de->d_name, full, kind);
    }
    closedir(d);
    attach_icons();
    if (!sBrowsing) { // a section: folders, launch scripts, ROMs and pictures
        int n = 0;

        for (i = 0; i < sDir.count; i++) {
            if (sDir.entries[i].kind != K_FILE) {
                sDir.entries[n++] = sDir.entries[i];
            } else {
                entry_free(&sDir.entries[i]);
            }
        }
        sDir.count = n;
    }
    qsort(sDir.entries, sDir.count, sizeof(Entry), entry_cmp);
    view_apply();
    sDir.selected = 0;
    sDir.scroll = 0;
    for (i = 0; select != NULL && i < sDir.count; i++) {
        if (strcmp(sDir.entries[i].name, select) == 0) {
            sDir.selected = i;
        }
    }
}

/* Home: the sections (those that exist) and Files */
static void home_open(const char* select) {
    static const char* const kSections[][2] = {
        { "Games", GAMES_DIR }, { "ROMs", ROMS_DIR }, { "Apps", APPS_DIR }, { "Screenshots", SHOTS_DIR }
    };
    struct stat st;
    size_t i;

    dir_free();
    sDir.path[0] = 0;
    for (i = 0; i < sizeof(kSections) / sizeof(kSections[0]); i++) {
        if (stat(kSections[i][1], &st) == 0 && S_ISDIR(st.st_mode)) {
            entry_add(kSections[i][0], kSections[i][1], K_DIR);
        }
    }
    entry_add("Files", FILES_DIR, K_DIR);
    view_apply();
    sDir.selected = 0;
    sDir.scroll = 0;
    for (i = 0; select != NULL && (int)i < sDir.count; i++) {
        if (strcmp(sDir.entries[i].name, select) == 0) {
            sDir.selected = i;
        }
    }
}

static void dir_parent(void) {
    char cur[PATH_MAX], name[PATH_MAX], parent[PATH_MAX];

    if (is_home()) {
        return;
    }
    if (!sBrowsing && section_name(sDir.path) != NULL) { // a section's root: home, to its tile
        home_open(section_name(sDir.path));
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

/* Internet through the USB host: the default route that usb-network's DHCP
   lease brings (a host that shares its connection) */
static bool online(void) {
    FILE* f = fopen("/proc/net/route", "r");
    char line[256];
    bool found = false;

    while (f != NULL && !found && fgets(line, sizeof(line), f) != NULL) {
        char iface[16];
        unsigned dest;
        found = sscanf(line, "%15s %x", iface, &dest) == 2 && dest == 0;
    }
    if (f != NULL) {
        fclose(f);
    }
    return found;
}

/* A 2x pixel glyph: rows of '#' and '.' */
static void draw_glyph(const char* const* rows, int n, int x, int y, SDL_Color c) {
    int i, j;
    for (j = 0; j < n; j++) {
        for (i = 0; rows[j][i]; i++) {
            if (rows[j][i] == '#') {
                ui_fill((SDL_Rect){ x + i * 2, y + j * 2, 2, 2 }, c);
            }
        }
    }
}

/* Battery, right-aligned to `right` and centred on `cy`, as in the system
   menu (s30-display): filled to the level in green, yellow or red with the
   percentage inside, a pixel bolt to its left while charging, a chain link
   left of that while online (the battery and the link never move) */
static void draw_battery(int right, int cy, int pct, bool charging, bool net) {
    static const char* const bolt[7] = { "...##", "..##.", ".##..", "#####", "..##.", ".##..", "##..." };
    static const char* const link[5] = { ".###...###.", "#...#.#...#", "#..#####..#", "#...#.#...#", ".###...###." };
    const int bw = 42, bh = 18, y = cy - bh / 2, x = right - 3 - bw;
    SDL_Color c = pct > 50 ? (SDL_Color){ 70, 200, 90, 255 }
                  : pct > 20 ? (SDL_Color){ 235, 190, 40, 255 }
                             : (SDL_Color){ 225, 60, 50, 255 };
    char text[8];
    int len;

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
        draw_glyph(bolt, 7, x - 14, y + 2, UI_TEXT);
    }
    if (net) {
        draw_glyph(link, 5, x - 14 - 6 - 22, y + 4, UI_TEXT);
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
    case K_ROM: // a cartridge: body, a label, the edge connector
        ui_fill((SDL_Rect){ x + w / 5, y + h / 6, w * 3 / 5, h * 2 / 3 }, shade);
        ui_fill((SDL_Rect){ x + w / 5 + 4, y + h / 6 + 4, w * 3 / 5 - 8, h / 3 }, body);
        ui_fill((SDL_Rect){ x + w / 5 + w / 10, y + h / 6 + h * 2 / 3 - h / 8, w * 3 / 5 - w / 5, h / 8 }, body);
        break;
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

    ui_text(title, UI_ACCENT, 12, (HEADER_H - UI_CHAR_H) / 2, SCRW - 120);
    if (pct >= 0) {
        draw_battery(SCRW - 10, HEADER_H / 2, pct, charging, online());
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

/* ---------------------------------------------------------------------------
 * Core picker (X): the cores for the selected ROM, or every core for a
 * folder inside ROMs
 * ------------------------------------------------------------------------- */

static struct {
    bool open;
    char path[PATH_MAX]; // the ROM or folder being set
    bool isDir;
    int items[MAX_CORES + 1]; // core indexes; -1: the folder's choice (ROM) or none (folder)
    int count, selected;
} sPick;

static bool can_pick_core(void) {
    const Entry* e;

    if (sDir.count == 0 || sNumCores == 0 || is_home()) {
        return false;
    }
    e = &sDir.entries[sDir.selected];
    return e->kind == K_ROM || (e->kind == K_DIR && !sBrowsing && strncmp(sDir.path, ROMS_DIR, strlen(ROMS_DIR)) == 0);
}

static void pick_open(void) {
    const Entry* e = &sDir.entries[sDir.selected];
    const char* current = core_map_get(e->path);
    char ext[32];
    int i;

    snprintf(sPick.path, sizeof(sPick.path), "%s", e->path);
    sPick.isDir = e->kind == K_DIR;
    sPick.count = 0;
    sPick.items[sPick.count++] = -1;
    rom_ext(e->name, ext, sizeof(ext));
    for (i = 0; i < sNumCores; i++) {
        if (sPick.isDir || core_takes(&sCores[i], ext)) {
            sPick.items[sPick.count++] = i;
        }
    }
    sPick.selected = 0;
    for (i = 1; i < sPick.count; i++) {
        if (current != NULL && strcmp(sCores[sPick.items[i]].name, current) == 0) {
            sPick.selected = i;
        }
    }
    sPick.open = true;
}

static void pick_apply(void) {
    int item = sPick.items[sPick.selected];
    core_map_set(sPick.path, item < 0 ? NULL : sCores[item].name);
    sPick.open = false;
}

/* The picker, a box over whatever is drawn: a row per choice, the current
   one marked */
static void draw_pick(void) {
    const int rowH = 20, pad = 10;
    const int boxH = pad + UI_CHAR_H + 8 + sPick.count * rowH + pad + 20;
    const SDL_Rect box = { SCRW / 2 - 190, (SCRH - boxH) / 2, 380, boxH };
    const char* current = core_map_get(sPick.path);
    char title[NAME_MAX + 32], name[NAME_MAX + 1];
    int i, y = box.y + pad + UI_CHAR_H + 8;
    const Entry* e = &sDir.entries[sDir.selected];

    ui_fill((SDL_Rect){ 0, 0, SCRW, SCRH }, (SDL_Color){ 0, 0, 0, 120 });
    ui_fill(box, kTile);
    ui_box(box, kSelectedEdge, 2);
    stem(e, name, sizeof(name));
    snprintf(title, sizeof(title), "Core for %s", name);
    ui_text(title, UI_ACCENT, box.x + pad, box.y + pad, box.w - 2 * pad);
    for (i = 0; i < sPick.count; i++) {
        const SDL_Rect row = { box.x + 6, y + i * rowH, box.w - 12, rowH - 2 };
        char label[160];
        bool isCurrent;

        if (sPick.items[i] < 0) {
            if (sPick.isDir) {
                snprintf(label, sizeof(label), "No choice (each ROM's own, or the default)");
            } else {
                char dir[PATH_MAX];
                const char* folder;
                const Core* d;

                snprintf(dir, sizeof(dir), "%s", sPick.path);
                folder = core_for_folder(dirname(dir));
                d = folder != NULL ? core_by_name(folder) : NULL;
                if (d == NULL) {
                    char ext[32];
                    rom_ext(e->name, ext, sizeof(ext));
                    d = core_default(ext);
                }
                snprintf(label, sizeof(label), "Folder's choice (%s)", d != NULL ? d->display : "none");
            }
            isCurrent = current == NULL;
        } else {
            snprintf(label, sizeof(label), "%s", sCores[sPick.items[i]].display);
            isCurrent = current != NULL && strcmp(current, sCores[sPick.items[i]].name) == 0;
        }
        if (i == sPick.selected) {
            ui_fill(row, kSelected);
            ui_box(row, kSelectedEdge, 1);
        }
        ui_text(isCurrent ? "*" : " ", UI_ACCENT, row.x + 6, row.y + 1, 12);
        ui_text(label, i == sPick.selected ? UI_TEXT : UI_DIM, row.x + 20, row.y + 1, row.w - 26);
    }
    {
        int x = box.x + pad;
        const int by = box.y + box.h - 22;
        ui_button(kLetterA, x, by + 1);
        x += BUTTON_SIZE + 5;
        ui_text("Choose", UI_TEXT, x, by, SCRW);
        x += ui_text_width("Choose") + 20;
        ui_button(kLetterB, x, by + 1);
        x += BUTTON_SIZE + 5;
        ui_text("Cancel", UI_DIM, x, by, SCRW);
    }
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
    if (sBrowsing ? strcmp(sDir.path, "/") != 0 : !is_home()) {
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
    if (can_pick_core()) {
        ui_button(kLetterX, x, by);
        x += BUTTON_SIZE + 5;
        ui_text("Core", UI_DIM, x, ty, SCRW);
        x += ui_text_width("Core") + 14;
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
    draw_header(is_home()                                        ? "PocketGo S30"
                : sBrowsing && strcmp(sDir.path, FILES_DIR) == 0 ? "Files"
                : !sBrowsing && section_name(sDir.path) != NULL  ? section_name(sDir.path)
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
                : sDir.entries[sDir.selected].kind == K_ROM   ? "Play"
                                                              : "Run",
                name);
    if (sConfirm) {
        draw_confirm();
    }
    if (sPick.open) {
        draw_pick();
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

#define CONSOLE_MARK "# s30-console"
#define CONSOLE_MARK_LINES 10

/* Whether the launch script asks for its output on the console: a line
   CONSOLE_MARK among its first CONSOLE_MARK_LINES */
static bool wants_console(const char* path) {
    FILE* f = fopen(path, "r");
    char line[256];
    bool found = false;
    int i;

    for (i = 0; f != NULL && !found && i < CONSOLE_MARK_LINES && fgets(line, sizeof(line), f) != NULL; i++) {
        line[strcspn(line, "\r\n")] = '\0';
        found = strcmp(line, CONSOLE_MARK) == 0;
    }
    if (f != NULL) {
        fclose(f);
    }
    return found;
}

/* Runs a command (argv[0] by PATH) in the folder of `path`, which is what
   the console relay and the log name it; `console` copies its output to
   tty1 */
static void run_command(const char* path, char* const argv[], bool console) {
    char dir[PATH_MAX];
    int pipefd[2] = { -1, -1 }, tty;
    pid_t pid;

    printf("launcher: running %s\n", path);
    ui_video_close();
    snprintf(dir, sizeof(dir), "%s", path);
    // The console starts empty: no earlier app's text when it shows
    tty = open("/dev/tty1", O_WRONLY | O_NOCTTY | O_CLOEXEC);
    if (tty >= 0 && write(tty, "\033[H\033[2J", 7) < 0) {
        // not fatal
    }
    // Output to the console too only when the script asks for it
    if (console && tty >= 0 && pipe2(pipefd, O_CLOEXEC) != 0) {
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
        execvp(argv[0], argv);
        fprintf(stderr, "launcher: exec %s: %s\n", argv[0], strerror(errno));
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

/* A launch script (with /bin/sh) or an ELF binary */
static void run(const char* path) {
    const char* dot = strrchr(path, '.');
    bool script = dot != NULL && strcmp(dot, ".sh") == 0;

    if (script) {
        char* const argv[] = { "sh", (char*)path, NULL };
        run_command(path, argv, wants_console(path));
    } else if (is_elf(path)) {
        char* const argv[] = { (char*)path, NULL };
        run_command(path, argv, false);
    }
}

/* A ROM in RetroArch with its core */
static void run_rom(const char* path) {
    const Core* c = core_for_rom(path);
    char cfg[PATH_MAX];

    if (c == NULL) {
        return;
    }
    home_path(cfg, sizeof(cfg), RETROARCH_CFG);
    {
        char* const argv[] = { "retroarch", "-c", cfg, "-L", (char*)c->path, (char*)path, NULL };
        run_command(path, argv, false);
    }
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
    kv_load(&sViews, VIEW_FILE);
    sPadFd = ui_pad_open();
    pad_flush();
    sAxisXCentre = sAxisX;
    sAxisYCentre = sAxisY;
    (void)argc;
    (void)argv;
    mkdir(GAMES_DIR, 0755);
    mkdir(ROMS_DIR, 0755);
    mkdir(APPS_DIR, 0755);
    cores_load();
    kv_load(&sCoreMap, CORES_FILE);
    home_open(NULL);

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
        } else if (sPick.open) { // up/down, A or B
            if (repeat && dy != 0) {
                int n = sPick.selected + dy;
                sPick.selected = n < 0 ? 0 : n >= sPick.count ? sPick.count - 1 : n;
                dirty = true;
            }
            if (a && !prevA) {
                pick_apply();
                dirty = true;
            } else if (b && !prevB) {
                sPick.open = false;
                dirty = true;
            }
        } else if (xb && !prevX && can_delete()) {
            sConfirm = true;
            dirty = true;
        } else if (xb && !prevX && can_pick_core()) {
            pick_open();
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
                    if (is_home() && strcmp(e->name, "Files") == 0 && strcmp(e->path, FILES_DIR) == 0) {
                        sBrowsing = true; // the Files tile
                    }
                    dir_open(e->path, NULL);
                } else if (e->kind == K_IMAGE) {
                    view_open();
                } else if (e->kind == K_ROM) {
                    run_rom(e->path);
                    a = b = false;
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
                kv_set(&sViews, view_key(), sList ? "list" : "grid");
                dirty = true;
            } else if (xb && !prevX && sBrowsing) { // out of Files, from anywhere
                sBrowsing = false;
                home_open("Files");
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

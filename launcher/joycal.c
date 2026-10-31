/*
 * PocketGo S30 joystick calibration.
 *
 * 1. Centre: the stick rests for a second; its average is the zero point.
 * 2. Rotate: the user turns the stick around its edge (3 turns); the furthest
 *    point in each of 64 directions is kept. Done after 3 turns once 3/4 of
 *    the directions have been reached (the fit uses those).
 * 3. Fit: least-squares ellipse a*x^2 + b*xy + c*y^2 = 1 through those edge
 *    points (relative to the centre). With Q = [[a, b/2], [b/2, c]], the
 *    matrix square root S of Q maps the ellipse onto the unit circle, which
 *    corrects per-axis range, skew between the axes and diagonal reach
 *    together. M = 127 * S puts the edge at full deflection.
 * 4. Test: live corrected stick; A saves, B redoes, Start quits unchanged.
 *
 * The gamepad driver applies "cx cy m00 m01 m10 m11" (M in Q12) from sysfs to
 * every sample; the result is also saved to CAL_FILE, which /etc/s30/rc loads at
 * boot. While calibrating, the driver is set to identity (raw values).
 */
#include <glob.h>
#include <linux/input.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ui.h"

#define CAL_FILE "/mnt/sd/home/.config/joystick.cal"
#define CAL_SYSFS_GLOB "/sys/bus/i2c/drivers/pocketgo-s30-gamepad/*/calibration"
#define IDENTITY_RAW "128 128 4096 0 0 4096 0" // no deadzone: raw values
#define DEADZONE 8

#define BINS 64
#define TURNS 3.0

enum { IN_A, IN_B, IN_START, IN_COUNT };
static const int sCodes[IN_COUNT] = { BTN_EAST, BTN_SOUTH, BTN_START };

enum { PH_CENTER, PH_ROTATE, PH_TEST };

static char sSysfs[256];
static int sFilled; // direction bins reached so far

static bool sysfs_find(void) {
    glob_t g;

    if (glob(CAL_SYSFS_GLOB, 0, NULL, &g) != 0) {
        return false;
    }
    snprintf(sSysfs, sizeof(sSysfs), "%s", g.gl_pathv[0]);
    globfree(&g);
    return true;
}

static bool write_text(const char* path, const char* text) {
    FILE* f = fopen(path, "w");
    bool ok;

    if (f == NULL) {
        return false;
    }
    ok = fputs(text, f) >= 0;
    return fclose(f) == 0 && ok;
}

static void read_text(const char* path, char* buf, size_t n) {
    FILE* f = fopen(path, "r");

    buf[0] = 0;
    if (f != NULL) {
        if (fgets(buf, (int)n, f) == NULL) {
            buf[0] = 0;
        }
        fclose(f);
    }
}

/* Least squares a x^2 + b xy + c y^2 = 1; returns false if not an ellipse */
static bool fit_ellipse(const double* px, const double* py, int n, double q[3]) {
    double m[3][4] = { { 0 } };
    int i, j, k;

    for (i = 0; i < n; i++) {
        double r[3] = { px[i] * px[i], px[i] * py[i], py[i] * py[i] };
        for (j = 0; j < 3; j++) {
            for (k = 0; k < 3; k++) {
                m[j][k] += r[j] * r[k];
            }
            m[j][3] += r[j];
        }
    }
    // Gaussian elimination with partial pivoting
    for (i = 0; i < 3; i++) {
        int p = i;
        for (j = i + 1; j < 3; j++) {
            if (fabs(m[j][i]) > fabs(m[p][i])) {
                p = j;
            }
        }
        for (k = 0; k < 4; k++) {
            double t = m[i][k];
            m[i][k] = m[p][k];
            m[p][k] = t;
        }
        if (fabs(m[i][i]) < 1e-12) {
            return false;
        }
        for (j = 0; j < 3; j++) {
            if (j != i) {
                double f = m[j][i] / m[i][i];
                for (k = i; k < 4; k++) {
                    m[j][k] -= f * m[i][k];
                }
            }
        }
    }
    q[0] = m[0][3] / m[0][0];
    q[1] = m[1][3] / m[1][1];
    q[2] = m[2][3] / m[2][2];
    // Positive definite: a > 0 and 4ac - b^2 > 0
    return q[0] > 0 && 4 * q[0] * q[2] - q[1] * q[1] > 0;
}

/* Matrix square root of the symmetric positive definite [[a, h], [h, c]] */
static void sqrtm2(double a, double h, double c, double s[4]) {
    double d = sqrt(a * c - h * h);
    double t = sqrt(a + c + 2 * d);

    s[0] = (a + d) / t;
    s[1] = h / t;
    s[2] = h / t;
    s[3] = (c + d) / t;
}

static void draw_circle(int cx, int cy, int r, SDL_Color c) {
    int i;

    SDL_SetRenderDrawColor(gUiRenderer, c.r, c.g, c.b, 255);
    for (i = 0; i < 128; i++) {
        double a0 = i * 2 * M_PI / 128, a1 = (i + 1) * 2 * M_PI / 128;
        SDL_RenderDrawLine(gUiRenderer, cx + (int)(r * cos(a0)), cy + (int)(r * sin(a0)), cx + (int)(r * cos(a1)),
                           cy + (int)(r * sin(a1)));
    }
}

static void draw_dot(int x, int y, int r, SDL_Color c) {
    SDL_Rect d = { x - r, y - r, 2 * r + 1, 2 * r + 1 };

    SDL_SetRenderDrawColor(gUiRenderer, c.r, c.g, c.b, 255);
    SDL_RenderFillRect(gUiRenderer, &d);
}

int main(void) {
    const SDL_Color red = { 255, 80, 80, 255 };
    char previous[128], msg[128];
    bool held[IN_COUNT] = { 0 }, prev[IN_COUNT] = { 0 };
    int fd, phase = PH_CENTER, rx = 128, ry = 128, i;
    double cx = 128, cy = 128, sumX = 0, sumY = 0, turns = 0, lastAng = 0;
    int samples = 0, minX = 255, maxX = 0, minY = 255, maxY = 0;
    double binR[BINS], binX[BINS], binY[BINS];
    int cal[6] = { 128, 128, 4096, 0, 0, 4096 };
    bool haveAng = false, saved = false;

    setvbuf(stdout, NULL, _IOLBF, 0);
    if (!ui_init()) {
        return 1;
    }
    fd = ui_pad_open();
    if (fd < 0 || !sysfs_find()) {
        fprintf(stderr, "joycal: gamepad or calibration attribute not found\n");
        return 1;
    }
    read_text(sSysfs, previous, sizeof(previous));
    write_text(sSysfs, IDENTITY_RAW); // raw values while calibrating
    memset(binR, 0, sizeof(binR));

    for (;;) {
        double dx, dy, r, ang;
        bool pressA, pressB, pressStart;

        ui_pad_poll(fd, held, sCodes, IN_COUNT);
        pressA = held[IN_A] && !prev[IN_A];
        pressB = held[IN_B] && !prev[IN_B];
        pressStart = held[IN_START] && !prev[IN_START];
        memcpy(prev, held, sizeof(prev));
        ui_pad_axes(fd, &rx, &ry);
        if (pressStart) {
            break;
        }

        dx = rx - cx;
        dy = ry - cy;
        r = sqrt(dx * dx + dy * dy);
        ang = atan2(dy, dx);

        if (phase == PH_CENTER) {
            // One second of samples within a few counts of each other
            sumX += rx;
            sumY += ry;
            samples++;
            minX = rx < minX ? rx : minX;
            maxX = rx > maxX ? rx : maxX;
            minY = ry < minY ? ry : minY;
            maxY = ry > maxY ? ry : maxY;
            if (maxX - minX > 4 || maxY - minY > 4) {
                sumX = sumY = 0;
                samples = 0;
                minX = minY = 255;
                maxX = maxY = 0;
            } else if (samples >= 60) {
                cx = sumX / samples;
                cy = sumY / samples;
                phase = PH_ROTATE;
            }
        } else if (phase == PH_ROTATE) {
            int b = (int)((ang + M_PI) / (2 * M_PI) * BINS) % BINS;
            int filled = 0;
            double maxR = 0;

            for (i = 0; i < BINS; i++) {
                maxR = binR[i] > maxR ? binR[i] : maxR;
            }
            if (r > binR[b]) {
                binR[b] = r;
                binX[b] = dx;
                binY[b] = dy;
            }
            // Count turns made near the edge
            if (r > 0.6 * maxR && r > 20) {
                if (haveAng) {
                    double d = ang - lastAng;
                    d = d > M_PI ? d - 2 * M_PI : d < -M_PI ? d + 2 * M_PI : d;
                    turns += d / (2 * M_PI);
                }
                lastAng = ang;
                haveAng = true;
            } else {
                haveAng = false;
            }
            for (i = 0; i < BINS; i++) {
                filled += binR[i] > 20;
            }
            sFilled = filled;
            if (filled >= BINS * 3 / 4 && fabs(turns) >= TURNS) {
                double px[BINS], py[BINS], q[3], s[4];
                int n = 0;
                for (i = 0; i < BINS; i++) {
                    if (binR[i] > 20) {
                        px[n] = binX[i];
                        py[n] = binY[i];
                        n++;
                    }
                }
                if (fit_ellipse(px, py, n, q)) {
                    sqrtm2(q[0], q[1] / 2, q[2], s);
                    cal[0] = (int)lround(cx);
                    cal[1] = (int)lround(cy);
                    for (i = 0; i < 4; i++) {
                        cal[2 + i] = (int)lround(127.0 * s[i] * 4096);
                    }
                    phase = PH_TEST;
                } else {
                    memset(binR, 0, sizeof(binR)); // bad data: rotate again
                    turns = 0;
                }
            }
        } else if (pressA) {
            snprintf(msg, sizeof(msg), "%d %d %d %d %d %d %d\n", cal[0], cal[1], cal[2], cal[3], cal[4], cal[5],
                     DEADZONE);
            saved = write_text(sSysfs, msg) && write_text(CAL_FILE, msg);
            printf("joycal: %s %s", saved ? "saved" : "save FAILED", msg);
            break;
        } else if (pressB) {
            phase = PH_CENTER;
            samples = 0;
            sumX = sumY = 0;
            minX = minY = 255;
            maxX = maxY = 0;
            turns = 0;
            haveAng = false;
            memset(binR, 0, sizeof(binR));
            cx = cy = 128;
        }

        // --- Draw: raw stick on the left, corrected on the right ---
        ui_begin();
        if (phase == PH_CENTER) {
            ui_text("Let go of the stick...", UI_ACCENT, 12, 8, UI_W - 24);
        } else if (phase == PH_ROTATE) {
            snprintf(msg, sizeof(msg), "Rotate around the edge: %.1f / %.0f turns, %d%% of directions",
                     fabs(turns) < TURNS ? fabs(turns) : TURNS, TURNS, sFilled * 100 / BINS);
            ui_text(msg, UI_ACCENT, 12, 8, UI_W - 24);
        } else {
            ui_text("Check the stick. A: save   B: redo   Start: quit", UI_ACCENT, 12, 8, UI_W - 24);
        }
        ui_text("raw", UI_DIM, 110, 290, 60);
        ui_text("calibrated", UI_DIM, 330, 290, 100);

        // Raw: 0-255 mapped to a 220 px box centred at (120, 160)
        draw_circle(120, 160, 110, UI_DIM);
        for (i = 0; i < BINS; i++) {
            if (binR[i] > 0) {
                draw_dot(120 + (int)((cx + binX[i] - 128) * 110 / 128), 160 + (int)((cy + binY[i] - 128) * 110 / 128),
                         1, UI_ACCENT);
            }
        }
        draw_dot(120 + (rx - 128) * 110 / 128, 160 + (ry - 128) * 110 / 128, 3, UI_TEXT);
        if (phase != PH_CENTER) {
            draw_dot(120 + (int)((cx - 128) * 110 / 128), 160 + (int)((cy - 128) * 110 / 128), 2, red);
        }

        // Corrected (preview of the driver's output)
        draw_circle(360, 160, 110, UI_DIM);
        if (phase == PH_TEST) {
            double ox = (cal[2] * dx + cal[3] * dy) / 4096.0, oy = (cal[4] * dx + cal[5] * dy) / 4096.0;
            ox = ox < -128 ? -128 : ox > 127 ? 127 : ox;
            oy = oy < -128 ? -128 : oy > 127 ? 127 : oy;
            draw_circle(360, 160, 110 * 127 / 128, UI_ACCENT);
            draw_dot(360 + (int)(ox * 110 / 128), 160 + (int)(oy * 110 / 128), 3, UI_TEXT);
        }
        ui_present();
        SDL_Delay(16);
    }

    if (!saved) {
        write_text(sSysfs, previous[0] ? previous : "128 128 4096 0 0 4096"); // unchanged
    }
    return 0;
}

// Crab puck - a physical status indicator for Claude Code.
// Target: LilyGO TTGO T-Display (ESP32-D0WDQ6, ST7789 135x240).
// Protocol: newline-terminated "state|detail|model|used|max" on USB serial.

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <Preferences.h>

static TFT_eSPI tft;
static TFT_eSprite fb(&tft);

static const int SCR_W = 240;
static const int SCR_H = 135;

static const int PIN_BL = 4;
static const int PIN_BTN_TOP = 35;
static const int PIN_BTN_BOT = 0;

#define C_BG     0x0841
#define C_SHELL  0xEAA7
#define C_LIGHT  0xF44D
#define C_DARK   0x78E2
#define C_MOUTH  0x58A2
#define C_WHITE  0xFFFF
#define C_PUPIL  0x0000
#define C_TEXT   0xBDF7
#define C_DIMTXT 0x52AA
#define C_WATER  0x1968
#define C_SAND   0x4A69
#define C_GOLD   0xFEA0
#define C_STEEL  0xA534
#define C_HEART  0xF9C3
#define C_GAME   0x07FF

enum State { ST_IDLE, ST_WORKING, ST_APPROVAL, ST_DONE, ST_ERROR, ST_ASLEEP, ST_COUNT };

static const char *STATE_NAME[ST_COUNT] = {
    "idle", "working", "approval", "done", "error", "asleep"};
static const char *STATE_LABEL[ST_COUNT] = {
    "IDLE", "WORKING", "APPROVE?", "DONE", "ERROR", "ASLEEP"};
static const uint16_t STATE_COLOR[ST_COUNT] = {
    0x3C1E, 0xF4E1, 0xEA28, 0x262B, 0xD924, 0x63B1};

// 24x14 shell. K outline, L highlight, R shell, M mouth, W eye white.
static const char *SHELL[] = {
    "        KKKKKKKK        ",
    "     KKKLLLLLLLLKKK     ",
    "   KKLLLLLLLLLLLLLLKK   ",
    "  KLLLLLLLLLLLLLLLLLLK  ",
    " KRRRRRRRRRRRRRRRRRRRRK ",
    " KRRRRRRRRRRRRRRRRRRRRK ",
    "KRRRWWRRRRRRRRRRRRWWRRRK",
    "KRRRWWRRRRRRRRRRRRWWRRRK",
    "KRRRWWRRRRRRRRRRRRWWRRRK",
    " KRRRRRRRRRRRRRRRRRRRRK ",
    " KRRRRRRMMMMMMMMRRRRRRK ",
    "  KRRRRRRRRRRRRRRRRRRK  ",
    "   KKRRRRRRRRRRRRRRKK   ",
    "     KKKKKKKKKKKKKK     "};
static const int SHELL_W = 24;
static const int SHELL_H = 14;

// eye modes
enum { EYE_OPEN, EYE_SHUT, EYE_WIDE, EYE_X, EYE_HAPPY, EYE_UP };

// idle vignettes - the crab amuses itself between jobs
enum { V_MOOCH, V_BUBBLES, V_DANCE, V_DIG, V_WAVE, V_STRETCH, V_SIDESTEP,
       V_SCRATCH, V_PONDER, V_SNOOZE, V_DOUBLETAKE, V_CASUAL, V_APPLAUD,
       V_STARTLE, V_HANDSTAND, V_BOX, V_NOD, V_PEEK, V_ITCH, V_SNEEZE,
       V_MOONWALK, V_FLEX, V_HICCUP, V_STARE, V_COUNT };
// one entry per vignette, in enum order. Left unsized on purpose: sized
// [V_COUNT] a missing duration silently zero-fills and that vignette expires
// on the frame it starts, so it never plays. Unsized, the assert catches it.
static const uint16_t V_FRAMES[] = {
    200, 150, 160, 170, 120, 130, 320, 140, 170, 110,
    150, 190, 130, 140, 150, 160, 220, 180, 200, 150,
    170, 190, 160, 170};
static_assert(sizeof(V_FRAMES) / sizeof(V_FRAMES[0]) == V_COUNT,
              "V_FRAMES needs exactly one duration per vignette");

// work motifs - what "busy" looks like this minute
enum { W_SCUTTLE, W_TYPE, W_JUGGLE, W_HAMMER, W_RUMMAGE, W_CARRY, W_SPIN,
       W_POINT, W_PANIC, W_STIR, W_COUNT };

static State state = ST_IDLE;
static char detail[64] = "waiting for claude";
static char modelName[20] = "";
static uint32_t ctxUsed = 0;
static uint32_t ctxMax = 0;
static uint32_t stateSince = 0;
static uint32_t lastMsg = 0;
static uint8_t brightness = 3;
static const uint8_t BRIGHT[] = {12, 40, 100, 200, 255};
static uint32_t frame = 0;

static uint8_t vignette = V_MOOCH;
static uint32_t vigUntil = 0;
static uint8_t motif = W_SCUTTLE;
static uint32_t motifUntil = 0;

static char rx[112];
static uint8_t rxLen = 0;

// ---------------------------------------------------------------- particles

struct Particle {
    float x, y, vx, vy;
    int16_t life;
    uint16_t col;
    uint8_t r;
    uint8_t kind;  // 0 dot, 1 bubble ring, 2 confetti (falls), 3 sand fleck
};
static const int MAXP = 30;
static Particle P[MAXP];

static const uint16_t CONFETTI[] = {0xF4E1, 0x262B, 0x3C1E, 0xEA28, 0xFFFF};

static void spawnP(float x, float y, float vx, float vy, int life, uint16_t col,
                   uint8_t r, uint8_t kind) {
    for (int i = 0; i < MAXP; i++) {
        if (P[i].life > 0) continue;
        P[i] = {x, y, vx, vy, (int16_t)life, col, r, kind};
        return;
    }
}

static void updateParticles() {
    for (int i = 0; i < MAXP; i++) {
        if (P[i].life <= 0) continue;
        P[i].x += P[i].vx;
        P[i].y += P[i].vy;
        if (P[i].kind == 2) P[i].vy += 0.07f;
        if (P[i].kind == 3) P[i].vy += 0.12f;
        if (P[i].kind == 1) P[i].x += 0.25f * sinf((frame + i * 9) * 0.12f);
        P[i].life--;
    }
}

static void drawParticles(bool front) {
    for (int i = 0; i < MAXP; i++) {
        if (P[i].life <= 0) continue;
        bool isFront = (P[i].kind == 2 || P[i].kind == 3);
        if (isFront != front) continue;
        int x = (int)P[i].x, y = (int)P[i].y;
        switch (P[i].kind) {
            case 1: fb.drawCircle(x, y, P[i].r, P[i].col); break;
            case 2: fb.fillRect(x, y, P[i].r + 1, P[i].r + 1, P[i].col); break;
            default: fb.fillCircle(x, y, P[i].r, P[i].col); break;
        }
    }
}

// a stray fish wanders past now and then
static float fishX = -999, fishY = 0, fishV = 0;

static void maybeFish() {
    if (fishX > -900) return;
    if (random(1400)) return;
    bool ltr = random(2);
    fishX = ltr ? -20 : SCR_W + 20;
    fishV = ltr ? 0.9f : -0.9f;
    fishY = random(28, 80);
}

static void drawFish() {
    if (fishX < -900) return;
    int x = (int)fishX, y = (int)(fishY + 2 * sinf(frame * 0.12f));
    int d = fishV > 0 ? 1 : -1;
    fb.fillEllipse(x, y, 6, 4, C_WATER + 0x2104);
    fb.fillTriangle(x - d * 6, y, x - d * 11, y - 4, x - d * 11, y + 4, C_WATER + 0x2104);
    fb.drawPixel(x + d * 3, y - 1, C_WHITE);
    fishX += fishV;
    if (fishX < -30 || fishX > SCR_W + 30) fishX = -999;
}

// ------------------------------------------------------------------ drawing

static void setBacklight(uint8_t duty) { ledcWrite(0, duty); }

static uint16_t shade(char c) {
    switch (c) {
        case 'K': return C_DARK;
        case 'L': return C_LIGHT;
        case 'R': return C_SHELL;
        case 'M': return C_MOUTH;
        case 'W': return C_WHITE;
    }
    return 0;
}

static void drawClaw(int x, int y, int dir, int s, int gap) {
    int jaw = 3 * s;
    fb.fillRect(x - s, y - s, 2 * s, 2 * s, C_SHELL);
    fb.drawRect(x - s, y - s, 2 * s, 2 * s, C_DARK);
    fb.drawWideLine(x, y, x + dir * jaw, y - gap - s, s, C_SHELL, C_SHELL);
    fb.drawWideLine(x, y, x + dir * jaw, y + gap + s, s, C_SHELL, C_SHELL);
}

static void drawArm(int sx, int sy, int dir, int s, int lift, int gap) {
    int ex = sx + dir * 5 * s;
    int ey = sy - s - lift / 2;
    int wx = ex + dir * 4 * s;
    int wy = ey - 2 * s - lift;
    fb.drawWideLine(sx, sy, ex, ey, s, C_SHELL, C_SHELL);
    fb.drawWideLine(ex, ey, wx, wy, s, C_SHELL, C_SHELL);
    drawClaw(wx, wy, dir, s, gap);
}

static void drawLeg(int hx, int hy, int dir, int s, int swing) {
    int kx = hx + dir * 3 * s;
    int ky = hy + 2 * s;
    int fx = kx + dir * 2 * s + swing;
    int fy = ky + 3 * s;
    fb.drawWideLine(hx, hy, kx, ky, s, C_DARK, C_DARK);
    fb.drawWideLine(kx, ky, fx, fy, s, C_DARK, C_DARK);
}

struct Pose {
    int dx, dy, liftL, liftR, gapL, gapR, eyes, pdx, pdy, legPhase;
};

static void drawEye(int ex, int ey, int s, int mode, int pdx, int pdy) {
    switch (mode) {
        case EYE_SHUT:
            fb.fillRect(ex, ey, 2 * s, 3 * s, C_SHELL);
            fb.fillRect(ex, ey + s, 2 * s, s, C_DARK);
            break;
        case EYE_X:
            fb.fillRect(ex, ey, 2 * s, 3 * s, C_SHELL);
            fb.drawWideLine(ex, ey + s / 2, ex + 2 * s, ey + 3 * s - s / 2, 2, C_DARK, C_DARK);
            fb.drawWideLine(ex + 2 * s, ey + s / 2, ex, ey + 3 * s - s / 2, 2, C_DARK, C_DARK);
            break;
        case EYE_HAPPY:
            fb.fillRect(ex, ey, 2 * s, 3 * s, C_SHELL);
            fb.drawWideLine(ex, ey + 2 * s, ex + s, ey + s, 2, C_DARK, C_DARK);
            fb.drawWideLine(ex + s, ey + s, ex + 2 * s, ey + 2 * s, 2, C_DARK, C_DARK);
            break;
        case EYE_WIDE:
            fb.fillRect(ex - s / 2, ey - s / 2, 3 * s, 4 * s, C_WHITE);
            fb.fillRect(ex + pdx * s, ey + (1 + pdy) * s, s, s, C_PUPIL);
            break;
        case EYE_UP:
            fb.fillRect(ex + pdx * s, ey, s, s, C_PUPIL);
            break;
        default:
            fb.fillRect(ex + pdx * s, ey + (1 + pdy) * s, s, s, C_PUPIL);
            break;
    }
}

static void drawCrab(int cx, int cy, int s, const Pose &p) {
    int ox = cx - (SHELL_W * s) / 2;
    int oy = cy - (SHELL_H * s) / 2;

    for (int side = 0; side < 2; side++) {
        int dir = side ? 1 : -1;
        int hx = cx + dir * (SHELL_W / 2 - 2) * s;
        for (int i = 0; i < 3; i++) {
            int swing = (int)(s * 1.2f * sinf((p.legPhase + i * 40) * 0.0174533f));
            drawLeg(hx, oy + (7 + i * 2) * s, dir, s, swing);
        }
        drawArm(cx + dir * (SHELL_W / 2 - 1) * s, oy + 5 * s, dir, s,
                side ? p.liftR : p.liftL, side ? p.gapR : p.gapL);
    }

    for (int r = 0; r < SHELL_H; r++) {
        for (int c = 0; c < SHELL_W; c++) {
            char ch = SHELL[r][c];
            if (ch == ' ') continue;
            fb.fillRect(ox + c * s, oy + r * s, s, s, shade(ch));
        }
    }

    for (int side = 0; side < 2; side++) {
        drawEye(ox + (side ? 18 : 4) * s, oy + 6 * s, s, p.eyes, p.pdx, p.pdy);
    }
}

static void drawSpeech(int x, int y, const char *txt, uint16_t col) {
    int w = 9 * (int)strlen(txt) + 12;
    fb.fillRoundRect(x - w / 2, y - 11, w, 22, 6, col);
    fb.fillTriangle(x - 4, y + 10, x + 4, y + 10, x, y + 17, col);
    fb.setTextFont(2);
    fb.setTextDatum(MC_DATUM);
    fb.setTextColor(C_BG, col);
    fb.drawString(txt, x, y);
}

// ------------------------------------------------------------------ buttons

struct Btn {
    uint8_t pin;
    bool down, rose, fell, longFired;
    uint32_t changedAt, downAt;
};

static Btn bTop = {PIN_BTN_TOP, false, false, false, false, 0, 0};
static Btn bBot = {PIN_BTN_BOT, false, false, false, false, 0, 0};

static void btnUpdate(Btn &b) {
    b.rose = b.fell = false;
    bool raw = digitalRead(b.pin) == LOW;
    uint32_t now = millis();
    if (raw == b.down || now - b.changedAt < 25) return;
    b.down = raw;
    b.changedAt = now;
    if (raw) {
        b.downAt = now;
        b.rose = true;
        b.longFired = false;
    } else {
        b.fell = true;
    }
}

// fires once, while the button is still held
static bool btnLong(Btn &b, uint32_t ms) {
    if (!b.down || b.longFired || millis() - b.downAt < ms) return false;
    b.longFired = true;
    return true;
}

// a press that came back up without turning into a long press
static bool btnTap(Btn &b) { return b.fell && !b.longFired; }

// swallow whatever is currently held, so leaving the game does not also
// cycle the brightness or the state on the way out
static void consumeBtns() { bTop.longFired = bBot.longFired = true; }

// --------------------------------------------------------------- crab catch
//
// Two buttons, one crab: catch the pearls, dodge the anchors. A real message
// from Claude always wins - setState drops you back to status, mid-game or not.

enum GamePhase { G_OFF, G_READY, G_PLAY, G_OVER };

static const int G_FLOOR = 122;  // sand line
static const int G_CY = 104;     // crab centre while playing
static const int G_S = 2;        // crab scale - smaller than status, room to dodge
static const int G_HALF = 24;    // half the crab width at that scale

static uint8_t gamePhase = G_OFF;
static uint32_t gameTimer = 0;  // frames left in READY
static uint32_t lastInput = 0;  // wall clock, for the bail-out when you wander off
static uint32_t spawnAt = 0;
static uint32_t bothSince = 0;  // both buttons held - the quit gesture
static int score = 0, best = 0, lives = 3, level = 1;
static int shake = 0, invuln = 0, flash = 0;
static bool swapCtl = false, newBest = false;
static float crabX = SCR_W / 2, crabVX = 0;

struct Faller {
    float x, y, vy;
    uint8_t kind;  // 0 pearl, 1 anchor, 2 gold pearl
    bool live;
};
static const int MAXF = 12;
static Faller FL[MAXF];

static Preferences prefs;

static void setState(State s, const char *text);

static void gameClear() {
    for (int i = 0; i < MAXF; i++) FL[i].live = false;
    for (int i = 0; i < MAXP; i++) P[i].life = 0;
}

static void gameStart() {
    gameClear();
    score = 0;
    lives = 3;
    level = 1;
    crabX = SCR_W / 2;
    crabVX = 0;
    shake = invuln = flash = 0;
    newBest = false;
    bothSince = 0;
    spawnAt = frame + 25;
    gameTimer = 75;  // three seconds of countdown at 25 fps
    gamePhase = G_READY;
    lastInput = millis();
}

static void gameEnter() {
    best = prefs.getInt("best", 0);
    swapCtl = prefs.getBool("swap", false);
    setBacklight(BRIGHT[brightness]);
    gameStart();
}

static void gameExit() {
    gamePhase = G_OFF;
    bothSince = 0;
    consumeBtns();
    gameClear();
    stateSince = millis();
    setBacklight(state == ST_ASLEEP ? 10 : BRIGHT[brightness]);
}

static void spawnFaller() {
    for (int i = 0; i < MAXF; i++) {
        if (FL[i].live) continue;
        int anchorPct = 34 + level * 3;
        if (anchorPct > 62) anchorPct = 62;
        uint8_t kind = 0;
        if ((int)random(100) < anchorPct)
            kind = 1;
        else if (random(100) < 8)
            kind = 2;
        FL[i] = {(float)random(14, SCR_W - 14), -8.0f,
                 1.0f + level * 0.16f + random(0, 7) / 10.0f, kind, true};
        return;
    }
}

#ifdef CRAB_CAPTURE
// Capture build only: plays the game so it can be recorded without hands on
// the buttons. Mode 1 chases the lowest pearl and sidesteps anchors about to
// land; mode 2 walks into anchors, for a quick game-over card; 0 sits still.
static uint8_t autopilot = 1;

static void autoSteer(bool &left, bool &right) {
    left = right = false;
    lastInput = millis();  // never trip the walked-off timeout while recording
    if (!autopilot) return;
    float target = crabX;
    if (autopilot == 2) {
        float lowest = -99;
        for (int i = 0; i < MAXF; i++)
            if (FL[i].live && FL[i].kind == 1 && FL[i].y < G_CY && FL[i].y > lowest) {
                lowest = FL[i].y;
                target = FL[i].x;
            }
        if (target < crabX - 3) left = true;
        else if (target > crabX + 3) right = true;
        return;
    }
    float bestY = -99;
    for (int i = 0; i < MAXF; i++) {
        if (!FL[i].live || FL[i].kind == 1 || FL[i].y > G_CY) continue;
        if (FL[i].y > bestY) { bestY = FL[i].y; target = FL[i].x; }
    }
    for (int i = 0; i < MAXF; i++) {
        if (!FL[i].live || FL[i].kind != 1) continue;
        if (FL[i].y < G_CY - 55 || FL[i].y > G_CY + 4) continue;
        float d = crabX - FL[i].x;
        if (fabsf(d) < G_HALF + 10) target = crabX + (d >= 0 ? 40 : -40);
        if (target < G_HALF + 2) target = FL[i].x + 50;
        if (target > SCR_W - G_HALF - 2) target = FL[i].x - 50;
    }
    if (target < crabX - 5) left = true;
    else if (target > crabX + 5) right = true;
}
#endif

static void gameUpdate() {
    bool left = swapCtl ? bBot.down : bTop.down;
    bool right = swapCtl ? bTop.down : bBot.down;
    if (bTop.down || bBot.down) lastInput = millis();
#ifdef CRAB_CAPTURE
    autoSteer(left, right);
#endif

    if (gamePhase == G_READY && gameTimer) {
        gameTimer--;
        if (!gameTimer) gamePhase = G_PLAY;
    }

    // steering stays live during the countdown - no standing start
    if (left) crabVX -= 0.55f;
    if (right) crabVX += 0.55f;
    if (!left && !right) crabVX *= 0.80f;
    if (crabVX > 4.2f) crabVX = 4.2f;
    if (crabVX < -4.2f) crabVX = -4.2f;
    crabX += crabVX;
    if (crabX < G_HALF) {
        crabX = G_HALF;
        crabVX = 0;
    }
    if (crabX > SCR_W - G_HALF) {
        crabX = SCR_W - G_HALF;
        crabVX = 0;
    }

    if (invuln) invuln--;
    if (shake) shake--;
    if (flash) flash--;

    if (gamePhase != G_PLAY) return;

    level = score / 10 + 1;
    uint32_t gap = level < 9 ? 26 - level * 2 : 9;
    if (frame >= spawnAt) {
        spawnFaller();
        spawnAt = frame + gap;
    }

    for (int i = 0; i < MAXF; i++) {
        if (!FL[i].live) continue;
        FL[i].y += FL[i].vy;
        if (FL[i].y > SCR_H + 10) {
            FL[i].live = false;
            continue;
        }
        if (fabsf(FL[i].x - crabX) > G_HALF + 3) continue;
        if (FL[i].y < G_CY - 16 || FL[i].y > G_CY + 16) continue;

        if (FL[i].kind == 1) {
            if (invuln) continue;
            lives--;
            invuln = 45;
            shake = 12;
            flash = 6;
            for (int k = 0; k < 10; k++)
                spawnP(FL[i].x, FL[i].y, random(-20, 20) / 10.0f,
                       random(-16, 2) / 10.0f, 26, C_STEEL, 1, 3);
            FL[i].live = false;
            if (lives <= 0) {
                gamePhase = G_OVER;
                if (score > best) {
                    best = score;
                    newBest = true;
                    prefs.putInt("best", best);
                }
            }
        } else {
            score += FL[i].kind == 2 ? 5 : 1;
            for (int k = 0; k < (FL[i].kind == 2 ? 10 : 5); k++)
                spawnP(FL[i].x, FL[i].y, random(-12, 12) / 10.0f,
                       random(-18, -4) / 10.0f, 30,
                       FL[i].kind == 2 ? C_GOLD : C_WHITE, 1, 2);
            FL[i].live = false;
        }
    }
}

static void drawAnchor(int x, int y) {
    fb.drawCircle(x, y - 7, 2, C_STEEL);
    fb.drawFastVLine(x, y - 5, 12, C_STEEL);
    fb.drawFastHLine(x - 5, y - 2, 11, C_STEEL);
    fb.drawLine(x, y + 7, x - 6, y + 1, C_STEEL);
    fb.drawLine(x, y + 7, x + 6, y + 1, C_STEEL);
}

static void drawPearl(int x, int y, bool gold) {
    fb.fillCircle(x, y, 4, gold ? C_GOLD : C_WHITE);
    fb.drawCircle(x, y, 4, gold ? 0xC480 : C_DARK);
    fb.drawPixel(x - 1, y - 2, C_WHITE);
    if (gold && (frame / 3) % 2 == 0) {
        fb.drawFastHLine(x - 8, y, 3, C_GOLD);
        fb.drawFastHLine(x + 6, y, 3, C_GOLD);
    }
}

static void gameRender() {
    int sx = shake ? (int)random(-2, 3) : 0;
    fb.fillSprite(flash ? 0x5000 : C_BG);

    fb.fillRect(0, G_FLOOR, SCR_W, SCR_H - G_FLOOR, C_SAND);
    for (int i = 0; i < 26; i++)
        fb.drawPixel((i * 37 + 11) % SCR_W, G_FLOOR + 2 + (i * 13) % 9, 0x630C);

    drawParticles(false);

    Pose p = {0, 0, 0, 0, 0, 0, EYE_OPEN, 0, 0, (int)(frame * 6)};
    p.gapL = p.gapR = (int)(2 + 2 * sinf(frame * 0.4f));
    p.pdx = crabVX > 0.4f ? 1 : 0;

    // eyes go wide when an anchor is lined up overhead
    for (int i = 0; i < MAXF; i++)
        if (FL[i].live && FL[i].kind == 1 && FL[i].y > 30 && FL[i].y < G_CY - 14 &&
            fabsf(FL[i].x - crabX) < 34)
            p.eyes = EYE_WIDE;

    if (!invuln || (frame / 3) % 2 == 0) drawCrab((int)crabX + sx, G_CY, G_S, p);

    for (int i = 0; i < MAXF; i++) {
        if (!FL[i].live) continue;
        if (FL[i].kind == 1)
            drawAnchor((int)FL[i].x + sx, (int)FL[i].y);
        else
            drawPearl((int)FL[i].x + sx, (int)FL[i].y, FL[i].kind == 2);
    }

    drawParticles(true);

    fb.fillRect(0, 0, SCR_W, 3, C_GAME);

    char buf[28];
    fb.setTextFont(2);
    fb.setTextDatum(TL_DATUM);
    fb.setTextColor(C_TEXT, C_BG);
    snprintf(buf, sizeof(buf), "%d", score);
    fb.drawString(buf, 6, 5);

    fb.setTextDatum(TR_DATUM);
    fb.setTextColor(C_DIMTXT, C_BG);
    snprintf(buf, sizeof(buf), "best %d", best);
    fb.drawString(buf, SCR_W - 46, 5);

    for (int i = 0; i < 3; i++)
        fb.fillCircle(SCR_W - 10 - i * 11, 12, 4, i < lives ? C_HEART : 0x2965);

    if (gamePhase == G_READY) {
        snprintf(buf, sizeof(buf), "%lu", (unsigned long)((gameTimer + 24) / 25));
        fb.setTextDatum(MC_DATUM);
        fb.setTextFont(4);
        fb.setTextColor(C_GAME, C_BG);
        fb.drawString(buf, SCR_W / 2, 46);
        fb.setTextFont(2);
        fb.setTextColor(C_DIMTXT, C_BG);
        fb.drawString("catch pearls, dodge anchors", SCR_W / 2, 72);
        fb.drawString("hold both to quit", SCR_W / 2, 90);
    }

    if (gamePhase == G_OVER) {
        fb.fillRoundRect(24, 26, SCR_W - 48, 80, 6, 0x18E3);
        fb.drawRoundRect(24, 26, SCR_W - 48, 80, 6, newBest ? C_GOLD : C_HEART);
        fb.setTextDatum(MC_DATUM);
        fb.setTextFont(4);
        fb.setTextColor(newBest ? C_GOLD : C_HEART, 0x18E3);
        fb.drawString(newBest ? "NEW BEST" : "GAME OVER", SCR_W / 2, 46);
        fb.setTextFont(2);
        fb.setTextColor(C_TEXT, 0x18E3);
        snprintf(buf, sizeof(buf), "%d pearls   best %d", score, best);
        fb.drawString(buf, SCR_W / 2, 72);
        fb.setTextColor(C_DIMTXT, 0x18E3);
        fb.drawString("tap again   hold to quit", SCR_W / 2, 92);
    }

    fb.pushSprite(0, 0);
}

// Quitting cannot be a long press on one button: while playing, both buttons
// ARE the steering, and holding left to cross the screen is the whole game.
// So mid-game it takes both at once. On the game-over card, where nothing is
// moving, a single hold is fine.
static void gameButtons() {
    if (gamePhase == G_OVER) {
        if (bTop.down && bBot.down) {  // controls were the wrong way round
            swapCtl = !swapCtl;
            prefs.putBool("swap", swapCtl);
            consumeBtns();
            gameStart();
            return;
        }
        if (btnLong(bTop, 600) || btnLong(bBot, 600)) {
            gameExit();
            return;
        }
        if (btnTap(bTop) || btnTap(bBot)) gameStart();
        return;
    }

    if (bTop.down && bBot.down) {
        if (!bothSince)
            bothSince = millis();
        else if (millis() - bothSince > 700)
            gameExit();
    } else {
        bothSince = 0;
    }
}

// ------------------------------------------------------------------ protocol

static void fmtTokens(uint32_t v, char *out, size_t n) {
    if (v >= 1000000UL)
        snprintf(out, n, "%.1fM", v / 1000000.0f);
    else if (v >= 1000UL)
        snprintf(out, n, "%luk", (unsigned long)(v / 1000UL));
    else
        snprintf(out, n, "%lu", (unsigned long)v);
}

static void pickVignette();
static void pickMotif();

static void setState(State s, const char *text) {
    if (gamePhase != G_OFF) gameExit();  // real work beats playtime
    if (s != state) {
        state = s;
        stateSince = millis();
        if (s == ST_IDLE) pickVignette();
        if (s == ST_WORKING) pickMotif();
        if (s == ST_DONE) {
            for (int i = 0; i < 18; i++)
                spawnP(random(60, 180), random(10, 40), random(-14, 14) / 10.0f,
                       random(-18, -4) / 10.0f, 70, CONFETTI[random(5)], 2, 2);
        }
    }
    if (text && *text) {
        strncpy(detail, text, sizeof(detail) - 1);
        detail[sizeof(detail) - 1] = 0;
    }
    lastMsg = millis();
    setBacklight(state == ST_ASLEEP ? 10 : BRIGHT[brightness]);
}

#ifdef CRAB_CAPTURE
// ------------------------------------------------------------------ capture
// Capture build only (pio run -e capture). Streams finished frames over
// serial so tools/capture.py can turn them into GIFs. Animation is driven by
// the frame counter, so a slow link just slows the loop down - every frame
// still arrives, and the GIF plays back at the real 25 fps.
//
//   !stream N    send the next N frames
//   !vig K       start idle vignette K        !motif K   start work motif K
//   !game        start crab catch             !ai 0|1|2  autopilot off/play/lose
//
// Frame: "FRM", u32 payload length, then runs of (u8 count, u16 pixel as
// stored in the sprite, i.e. byte-swapped RGB565).
static uint32_t capFrames = 0;

static void capSend() {
    if (!capFrames) return;
    capFrames--;
    const uint16_t *px = (const uint16_t *)fb.getPointer();
    const int n = SCR_W * SCR_H;
    uint32_t len = 0;
    for (int i = 0; i < n;) {
        int r = 1;
        while (i + r < n && r < 255 && px[i + r] == px[i]) r++;
        len += 3;
        i += r;
    }
    uint8_t hdr[7] = {'F', 'R', 'M', (uint8_t)len, (uint8_t)(len >> 8),
                      (uint8_t)(len >> 16), (uint8_t)(len >> 24)};
    Serial.write(hdr, sizeof(hdr));
    static uint8_t buf[1536];
    int b = 0;
    for (int i = 0; i < n;) {
        int r = 1;
        while (i + r < n && r < 255 && px[i + r] == px[i]) r++;
        buf[b++] = r;
        buf[b++] = px[i] & 0xFF;
        buf[b++] = px[i] >> 8;
        if (b >= (int)sizeof(buf) - 3) {
            Serial.write(buf, b);
            b = 0;
        }
        i += r;
    }
    if (b) Serial.write(buf, b);
    Serial.flush();
}

static void gameEnter();

static void capCommand(char *cmd) {
    char *arg = strchr(cmd, ' ');
    int v = arg ? atoi(arg + 1) : 0;
    if (!strncmp(cmd, "stream", 6)) {
        capFrames = v;
    } else if (!strncmp(cmd, "vig", 3)) {
        vignette = v % V_COUNT;
        vigUntil = frame + V_FRAMES[vignette];
    } else if (!strncmp(cmd, "motif", 5)) {
        motif = v % W_COUNT;
        motifUntil = frame + 100000;
    } else if (!strncmp(cmd, "game", 4)) {
        gameEnter();
    } else if (!strncmp(cmd, "ai", 2)) {
        autopilot = v;
    }
    Serial.print("ok !");
    Serial.println(cmd);
}
#endif

// state|detail|model|used|max - every field after the state is optional
static void handleLine(char *line) {
#ifdef CRAB_CAPTURE
    if (line[0] == '!') {
        capCommand(line + 1);
        return;
    }
#endif
    char *f[5] = {line, NULL, NULL, NULL, NULL};
    int n = 1;
    for (char *p = line; *p && n < 5; p++) {
        if (*p == '|') {
            *p = 0;
            f[n++] = p + 1;
        }
    }

    for (int i = 0; i < ST_COUNT; i++) {
        if (strcasecmp(f[0], STATE_NAME[i]) != 0) continue;

        if (f[2] && *f[2]) {
            strncpy(modelName, f[2], sizeof(modelName) - 1);
            modelName[sizeof(modelName) - 1] = 0;
        }
        if (f[3]) ctxUsed = strtoul(f[3], NULL, 10);
        if (f[4]) ctxMax = strtoul(f[4], NULL, 10);

        setState((State)i, f[1] ? f[1] : "");
        Serial.print("ok ");
        Serial.println(STATE_NAME[i]);
        return;
    }
    Serial.print("err unknown state ");
    Serial.println(f[0]);
}

static void pollSerial() {
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            if (rxLen) {
                rx[rxLen] = 0;
                handleLine(rx);
                rxLen = 0;
            }
        } else if (rxLen < sizeof(rx) - 1) {
            rx[rxLen++] = c;
        }
    }
}

static void pollButtons() {
    btnUpdate(bTop);
    btnUpdate(bBot);

    if (gamePhase != G_OFF) {
        gameButtons();
        return;
    }

    // hold the bottom button to go and play
    if (btnLong(bBot, 600)) {
        gameEnter();
        return;
    }

    if (btnTap(bTop)) {
        brightness = (brightness + 1) % (sizeof(BRIGHT) / sizeof(BRIGHT[0]));
        setBacklight(state == ST_ASLEEP ? 10 : BRIGHT[brightness]);
    }
    if (btnTap(bBot)) {
        if (state == ST_IDLE) {
            pickVignette();  // idle: skip to the next bit of business
        } else {
            setState((State)((state + 1) % ST_COUNT), "button preview");
        }
    }
}

// ----------------------------------------------------------------- vignettes

static void pickVignette() {
    uint8_t next = random(V_COUNT);
    if (next == vignette) next = (next + 1) % V_COUNT;
    vignette = next;
    vigUntil = frame + V_FRAMES[vignette];
}

static void pickMotif() {
    uint8_t next = random(W_COUNT);
    if (next == motif) next = (next + 1) % W_COUNT;
    motif = next;
    motifUntil = frame + 160;
}

static void poseIdle(Pose &p, int &cx, int cy, int s, uint16_t accent) {
    uint32_t e = vigUntil - frame;  // frames left
    float t = frame * 0.1f;
    p.legPhase = frame;

    switch (vignette) {
        case V_BUBBLES:
            p.dy = (int)(2 * sinf(t * 0.6f));
            if (frame % 14 == 0)
                spawnP(cx + random(-6, 6), cy + 22, random(-4, 4) / 10.0f, -0.8f,
                       90, C_WATER + 0x3186, random(1, 3), 1);
            break;

        case V_DANCE:
            p.dx = (int)(7 * sinf(t * 2.2f));
            p.dy = -abs((int)(4 * sinf(t * 4.4f)));
            p.liftL = sinf(t * 4.4f) > 0 ? 12 : 0;
            p.liftR = sinf(t * 4.4f) > 0 ? 0 : 12;
            p.gapL = p.gapR = 4;
            p.eyes = EYE_HAPPY;
            p.legPhase = frame * 7;
            break;

        case V_DIG:
            p.dy = 5;
            p.liftL = (int)(-4 + 4 * sinf(t * 5.0f));
            p.liftR = (int)(-4 + 4 * sinf(t * 5.0f + 3.14f));
            p.eyes = EYE_OPEN;
            p.pdy = 1;
            if (frame % 9 == 0)
                spawnP(cx + random(-30, 30), cy + 26, random(-12, 12) / 10.0f,
                       -1.6f, 26, C_SAND, 1, 3);
            break;

        case V_WAVE:
            p.liftR = (int)(14 + 4 * sinf(t * 6.0f));
            p.gapR = (int)(4 + 3 * sinf(t * 6.0f));
            p.eyes = EYE_HAPPY;
            p.dy = (int)(1.5f * sinf(t * 1.2f));
            break;

        case V_STRETCH: {
            float k = sinf((1.0f - e / (float)V_FRAMES[V_STRETCH]) * 3.14159f);
            p.liftL = p.liftR = (int)(16 * k);
            p.gapL = p.gapR = (int)(7 * k);
            p.eyes = k > 0.6f ? EYE_SHUT : EYE_OPEN;
            p.dy = (int)(-3 * k);
            break;
        }

        case V_SIDESTEP: {
            float u = 1.0f - e / (float)V_FRAMES[V_SIDESTEP];
            cx = 46 + (int)(148 * (0.5f - 0.5f * cosf(u * 6.28318f)));
            p.pdx = sinf(u * 6.28318f) > 0 ? 1 : 0;
            p.legPhase = frame * 11;
            p.gapL = p.gapR = (int)(2 + 2 * sinf(t * 4.0f));
            break;
        }

        case V_SCRATCH:
            p.liftR = 17;
            p.gapR = (frame / 4) % 2 ? 5 : 1;
            p.eyes = EYE_SHUT;
            p.dy = (frame / 4) % 2 ? 1 : 0;
            break;

        case V_PONDER:
            p.liftL = 13;
            p.gapL = 2;
            p.eyes = EYE_UP;
            p.pdx = ((frame / 30) % 2) ? 1 : 0;
            drawSpeech(cx + 52, cy - 34, "?", accent);
            break;

        case V_SNOOZE:
            p.dy = 4 + (int)(2 * sinf(t * 0.5f));
            p.eyes = EYE_SHUT;
            p.legPhase = 0;
            fb.setTextFont(2);
            fb.setTextDatum(TL_DATUM);
            fb.setTextColor(C_DIMTXT, C_BG);
            fb.drawString("z", cx + 40, cy - 26 - (int)((frame / 4) % 12));
            break;

        case V_DOUBLETAKE: {
            uint32_t age = V_FRAMES[V_DOUBLETAKE] - e;
            if (age < 40) {  // minding its own business
                p.pdx = -1;
                p.dy = (int)(2 * sinf(t * 0.6f));
            } else if (age < 50) {  // wait. what.
                p.eyes = EYE_WIDE;
                p.pdx = 1;
                p.dx = -4;
                p.liftL = p.liftR = 6;
            } else {  // leans in, staring
                p.eyes = EYE_WIDE;
                p.pdx = 1;
                p.dx = (int)(3 * sinf(t * 1.4f));
            }
            break;
        }

        case V_CASUAL: {
            uint32_t age = V_FRAMES[V_CASUAL] - e;
            if (age < 45) {  // caught mid-scratch
                p.liftR = 17;
                p.gapR = (frame / 4) % 2 ? 5 : 1;
                p.eyes = EYE_SHUT;
            } else if (age < 55) {  // freezes
                p.liftR = 17;
                p.gapR = 5;
                p.eyes = EYE_WIDE;
                p.pdx = 1;
            } else {  // nothing to see here
                p.eyes = EYE_UP;
                p.pdx = ((frame / 25) % 2) ? 1 : -1;
                p.dy = (int)(1.5f * sinf(t * 0.8f));
                if ((age / 20) % 2 == 0) drawSpeech(cx + 52, cy - 34, "~", accent);
            }
            break;
        }

        case V_APPLAUD:  // delighted with its own joke
            p.liftL = p.liftR = 11;
            p.gapL = p.gapR = (frame / 3) % 2 ? 6 : 0;
            p.eyes = EYE_HAPPY;
            p.dy = (frame / 3) % 2 ? -1 : 1;
            p.legPhase = frame * 4;
            break;

        case V_STARTLE: {
            uint32_t age = V_FRAMES[V_STARTLE] - e;
            if (age == 20)  // blows one bubble
                spawnP(cx + 10, cy + 20, 0.1f, -0.7f, 120, C_WATER + 0x3186, 3, 1);
            if (age < 52) {
                p.dy = (int)(2 * sinf(t * 0.6f));
                p.pdy = -1;
            } else if (age < 70) {  // and is appalled by it
                p.eyes = EYE_WIDE;
                p.pdy = -1;
                p.liftL = p.liftR = 15;
                p.gapL = p.gapR = 6;
                p.dy = -3;
            } else {  // composes itself
                p.dy = (int)(2 * sinf(t * 1.2f));
            }
            break;
        }

        case V_HANDSTAND: {
            float u = 1.0f - e / (float)V_FRAMES[V_HANDSTAND];
            if (u < 0.65f) {  // up on the claws, wobbling
                float k = sinf(u / 0.65f * 3.14159f);
                p.liftL = p.liftR = (int)(-10 * k);
                p.dy = (int)(-7 * k) + (int)(2 * sinf(t * 7.0f) * k);
                p.eyes = EYE_WIDE;
                p.pdy = -1;
                p.legPhase = frame * 6;
            } else {  // flop
                p.dy = 6;
                p.eyes = EYE_X;
                p.liftL = p.liftR = -3;
                p.legPhase = 0;
            }
            break;
        }

        case V_BOX:  // shadow-boxing nobody
            p.liftL = ((frame / 4) % 2) ? 14 : 8;
            p.liftR = ((frame / 4) % 2) ? 8 : 14;
            p.gapL = p.gapR = 0;
            p.eyes = EYE_WIDE;
            p.pdx = ((frame / 4) % 2) ? 1 : -1;
            p.dx = ((frame / 4) % 2) ? 2 : -2;
            p.legPhase = frame * 8;
            break;

        case V_NOD: {  // nodding off standing up
            uint32_t cyc = (V_FRAMES[V_NOD] - e) % 55;
            if (cyc < 42) {  // sinking
                p.dy = (int)(cyc / 6);
                p.eyes = cyc > 14 ? EYE_SHUT : EYE_OPEN;
                p.legPhase = 0;
            } else {  // WHAT no I am awake
                p.dy = -2;
                p.eyes = EYE_WIDE;
                p.pdy = -1;
                p.liftL = p.liftR = 8;
            }
            break;
        }

        case V_PEEK: {  // certain it is being watched
            uint32_t cyc = (V_FRAMES[V_PEEK] - e) % 60;
            p.eyes = EYE_WIDE;
            p.pdx = cyc < 25 ? -1 : (cyc < 50 ? 1 : 0);
            p.dx = cyc < 25 ? -3 : (cyc < 50 ? 3 : 0);
            p.liftL = p.liftR = 3;
            p.legPhase = frame * 2;
            break;
        }

        case V_ITCH: {
            float u = 1.0f - e / (float)V_FRAMES[V_ITCH];
            if (u < 0.75f) {  // an itch on the shell, just out of reach
                p.liftL = (int)(16 + 4 * sinf(t * 6.0f));
                p.liftR = (int)(16 + 4 * sinf(t * 6.0f + 3.14f));
                p.gapL = p.gapR = 6;
                p.eyes = EYE_SHUT;
                p.dx = (int)(3 * sinf(t * 3.0f));
                p.dy = (int)(2 * sinf(t * 6.0f));
                p.legPhase = frame * 7;
            } else {  // gives up, sulks
                p.eyes = EYE_UP;
                p.pdx = -1;
                p.dy = 3;
                p.legPhase = 0;
            }
            break;
        }

        case V_SNEEZE: {
            uint32_t age = V_FRAMES[V_SNEEZE] - e;
            if (age < 46) {  // aaah... aaaah...
                p.eyes = EYE_SHUT;
                p.dy = -(int)(age / 12);
                p.liftL = p.liftR = (int)(age / 8);
                p.pdy = -1;
            } else if (age < 54) {  // CHOO
                p.dy = 5;
                p.eyes = EYE_X;
                p.liftL = p.liftR = -4;
                if (age == 46)
                    for (int i = 0; i < 6; i++)
                        spawnP(cx, cy + 6, random(-20, 20) / 10.0f,
                               random(-16, -4) / 10.0f, 30, C_SAND, 1, 3);
            } else {  // sniff
                p.dy = (int)(2 * sinf(t * 0.9f));
                p.liftL = 9;
                p.gapL = 2;
            }
            break;
        }

        case V_MOONWALK: {  // gliding right to left, legs going the other way
            float u = 1.0f - e / (float)V_FRAMES[V_MOONWALK];
            cx = 170 - (int)(100 * u);
            p.legPhase = frame * 13;
            p.liftL = 10;
            p.liftR = 4;
            p.gapL = p.gapR = 2;
            p.eyes = EYE_HAPPY;
            p.dy = (int)(1.5f * sinf(t * 3.0f));
            break;
        }

        case V_FLEX: {  // bodybuilder routine, audience of zero
            uint32_t cyc = (V_FRAMES[V_FLEX] - e) % 64;
            if (cyc < 32) {  // double biceps
                p.liftL = p.liftR = 15;
                p.gapL = p.gapR = 1;
                p.dy = -2;
            } else {  // side chest
                p.liftL = 17;
                p.liftR = 2;
                p.gapL = 0;
                p.gapR = 5;
                p.dx = 3;
            }
            p.eyes = EYE_HAPPY;
            break;
        }

        case V_HICCUP: {
            uint32_t cyc = (V_FRAMES[V_HICCUP] - e) % 38;
            if (cyc < 5) {  // *hic*
                p.dy = -6;
                p.eyes = EYE_WIDE;
                p.liftL = p.liftR = 7;
                if (cyc == 0)
                    spawnP(cx + 12, cy - 10, 0.2f, -0.9f, 60, C_WATER + 0x3186, 2, 1);
            } else {
                p.dy = (int)(2 * sinf(t * 0.7f));
                p.pdy = 1;
            }
            break;
        }

        case V_STARE: {  // deadpan eye contact, holds it far too long
            uint32_t age = V_FRAMES[V_STARE] - e;
            p.eyes = (age % 90 < 6) ? EYE_SHUT : EYE_WIDE;
            p.dy = (int)(1 * sinf(t * 0.4f));
            p.legPhase = 0;
            break;
        }

        default:  // V_MOOCH - just being a crab
            p.dy = (int)(2 * sinf(t * 0.6f));
            p.gapL = p.gapR = (frame % 120 < 8) ? 3 : 0;
            p.eyes = (frame % 170 < 5) ? EYE_SHUT : EYE_OPEN;
            p.pdx = ((frame / 70) % 3 == 0) ? 1 : 0;
            break;
    }
}

static void poseWorking(Pose &p, int &cx, int cy, int s) {
    float t = frame * 0.1f;

    switch (motif) {
        case W_TYPE:
            p.liftL = (frame / 3) % 2 ? -2 : 1;
            p.liftR = (frame / 3) % 2 ? 1 : -2;
            p.gapL = p.gapR = 2;
            p.eyes = EYE_OPEN;
            p.pdy = 1;
            p.legPhase = frame * 2;
            p.dy = (frame / 3) % 2 ? 1 : 0;
            break;

        case W_JUGGLE:
            p.liftL = p.liftR = 13;
            p.gapL = p.gapR = (int)(3 + 3 * sinf(t * 6.0f));
            p.eyes = EYE_WIDE;
            p.pdy = 0;
            p.legPhase = frame * 3;
            for (int i = 0; i < 3; i++) {
                float a = t * 2.2f + i * 2.094f;
                fb.fillCircle(cx + (int)(26 * cosf(a)), cy - 34 + (int)(11 * sinf(a)),
                              3, CONFETTI[i]);
            }
            break;

        case W_HAMMER:
            p.liftR = ((frame / 5) % 3 == 0) ? -5 : 15;
            p.gapR = 1;
            p.liftL = 0;
            p.eyes = EYE_OPEN;
            p.pdy = 1;
            p.legPhase = frame * 2;
            if ((frame % 15) == 0)
                for (int i = 0; i < 4; i++)
                    spawnP(cx + 44, cy + 4, random(2, 14) / 10.0f,
                           random(-14, -2) / 10.0f, 18, 0xF4E1, 1, 3);
            break;

        case W_RUMMAGE:  // tearing through the seabed looking for it
            p.dy = 5;
            p.liftL = (int)(-5 + 6 * sinf(t * 7.0f));
            p.liftR = (int)(-5 + 6 * sinf(t * 7.0f + 3.14f));
            p.gapL = p.gapR = 4;
            p.eyes = EYE_WIDE;
            p.pdy = 1;
            p.legPhase = frame * 3;
            if (frame % 7 == 0)
                spawnP(cx + random(-34, 34), cy + 24, random(-16, 16) / 10.0f,
                       -1.9f, 24, C_SAND, 1, 3);
            break;

        case W_CARRY:  // hauling something heavy across and back
            cx += (int)(30 * sinf(t * 0.5f));
            p.liftL = p.liftR = 12;
            p.gapL = p.gapR = 5;
            p.eyes = EYE_SHUT;
            p.dy = (frame / 4) % 2 ? 1 : 0;
            p.legPhase = frame * 5;
            fb.fillRoundRect(cx - 13, cy - 44, 26, 15, 3, C_STEEL);
            break;

        case W_SPIN:  // pacing a circle, thinking
            cx += (int)(16 * cosf(t * 1.1f));
            p.dy = (int)(4 * sinf(t * 1.1f));
            p.pdx = cosf(t * 1.1f) > 0 ? 1 : -1;
            p.eyes = EYE_UP;
            p.liftL = 11;
            p.gapL = 2;
            p.legPhase = frame * 10;
            break;

        case W_POINT:  // that one. that one there.
            p.liftR = 16;
            p.gapR = 0;
            p.eyes = EYE_OPEN;
            p.pdx = 1;
            p.dy = ((frame / 8) % 2) ? 2 : 0;
            break;

        case W_PANIC:  // full sprint, both ends of the screen
            cx += (int)(34 * sinf(t * 3.4f));
            p.liftL = p.liftR = 15;
            p.gapL = p.gapR = 6;
            p.eyes = EYE_WIDE;
            p.pdx = sinf(t * 3.4f) > 0 ? 1 : -1;
            p.dy = -abs((int)(2 * sinf(t * 6.8f)));
            p.legPhase = frame * 18;
            break;

        case W_STIR:  // stirring something just off-screen
            p.liftR = (int)(11 + 4 * sinf(t * 4.0f));
            p.gapR = 1;
            p.liftL = 3;
            p.eyes = EYE_OPEN;
            p.pdy = 1;
            p.dy = (int)(1 * sinf(t * 4.0f));
            if (frame % 20 == 0)
                spawnP(cx + 40, cy - 6, random(-3, 3) / 10.0f, -0.6f, 50,
                       C_WATER + 0x3186, random(1, 3), 1);
            break;

        default:  // W_SCUTTLE
            cx += (int)(9 * sinf(t * 1.6f));
            p.gapL = p.gapR = (int)(3 + 3 * sinf(t * 5.0f));
            p.pdx = sinf(t * 1.6f) > 0 ? 1 : 0;
            p.legPhase = frame * 9;
            break;
    }
}

// -------------------------------------------------------------------- render

static void render() {
    uint16_t accent = STATE_COLOR[state];
    bool haveMeta = modelName[0] || ctxMax;

    fb.fillSprite(C_BG);

    int cx = SCR_W / 2;
    int cy = haveMeta ? 58 : 68;
    int s = 3;
    float t = frame * 0.1f;

    Pose p = {0, 0, 0, 0, 0, 0, EYE_OPEN, 0, 0, (int)frame};

    // ambient: slow bubbles drifting up behind everything
    if (state != ST_ASLEEP && frame % 40 == 0)
        spawnP(random(12, SCR_W - 12), SCR_H - 26, 0, -0.45f, 200, C_WATER, random(1, 3), 1);

    drawParticles(false);
    if (state != ST_ASLEEP) {
        maybeFish();
        drawFish();
    }

    switch (state) {
        case ST_IDLE:
            if (frame >= vigUntil) pickVignette();
            poseIdle(p, cx, cy, s, accent);
            break;

        case ST_WORKING:
            if (frame >= motifUntil) pickMotif();
            poseWorking(p, cx, cy, s);
            break;

        case ST_APPROVAL:
            p.liftL = p.liftR = (int)(9 + 5 * sinf(t * 4.0f));
            p.gapL = p.gapR = (int)(4 + 4 * sinf(t * 4.0f));
            p.eyes = EYE_WIDE;
            p.legPhase = frame * 2;
            p.dy = 4 - abs((int)(3 * sinf(t * 2.0f)));
            break;

        case ST_DONE:
            p.liftL = p.liftR = 10 + (int)(2 * sinf(t * 2.0f));
            p.gapL = p.gapR = 5;
            p.eyes = EYE_HAPPY;
            p.legPhase = frame * 2;
            p.dy = -abs((int)(3 * sinf(t * 1.6f)));
            break;

        case ST_ERROR:
            p.dy = 6;
            p.eyes = EYE_X;
            p.legPhase = frame / 2;
            if (frame % 70 == 0)
                spawnP(cx + 30, cy - 18, 0.1f, 0.9f, 40, 0x6E5F, 2, 0);
            break;

        case ST_ASLEEP:
            p.dy = 8 + (int)(2 * sinf(t * 0.35f));
            p.eyes = EYE_SHUT;
            p.legPhase = 0;
            break;

        default:
            break;
    }

    if (state == ST_APPROVAL && (frame / 6) % 2 == 0)
        fb.drawRoundRect(1, 1, SCR_W - 2, SCR_H - 2, 6, accent);

    drawCrab(cx + p.dx, cy + p.dy, s, p);
    drawParticles(true);

    if (state == ST_APPROVAL) drawSpeech(cx, 20, "!", accent);
    if (state == ST_ASLEEP) {
        fb.setTextFont(2);
        fb.setTextDatum(TL_DATUM);
        fb.setTextColor(C_DIMTXT, C_BG);
        for (int i = 0; i < 3; i++) {
            int zy = 44 - i * 12 - (int)((frame / 4 + i * 7) % 14);
            fb.drawString("z", cx + 44 + i * 9, zy);
        }
    }

    fb.fillRect(0, 0, SCR_W, 3, accent);

    fb.setTextFont(2);
    fb.setTextDatum(TL_DATUM);
    fb.setTextColor(accent, C_BG);
    fb.drawString(STATE_LABEL[state], 6, 6);

    uint32_t secs = (millis() - stateSince) / 1000;
    char clock[12];
    snprintf(clock, sizeof(clock), "%lu:%02lu", (unsigned long)(secs / 60),
             (unsigned long)(secs % 60));
    fb.setTextDatum(TR_DATUM);
    fb.setTextColor(C_DIMTXT, C_BG);
    fb.drawString(clock, SCR_W - 6, 6);

    fb.setTextDatum(BC_DATUM);
    fb.setTextColor(C_TEXT, C_BG);
    char shown[34];
    strncpy(shown, detail, sizeof(shown) - 1);
    shown[sizeof(shown) - 1] = 0;
    fb.drawString(shown, SCR_W / 2, haveMeta ? SCR_H - 24 : SCR_H - 2);

    if (haveMeta) {
        fb.setTextDatum(BL_DATUM);
        fb.setTextColor(C_DIMTXT, C_BG);
        fb.drawString(modelName, 6, SCR_H - 5);

        if (ctxMax) {
            char used[10], cap[10], tokens[24];
            fmtTokens(ctxUsed, used, sizeof(used));
            fmtTokens(ctxMax, cap, sizeof(cap));
            snprintf(tokens, sizeof(tokens), "%s / %s", used, cap);

            uint32_t pct = (uint32_t)((uint64_t)ctxUsed * 100 / ctxMax);
            if (pct > 100) pct = 100;
            uint16_t barCol = pct < 60 ? 0x262B : (pct < 85 ? 0xF4E1 : 0xD924);

            fb.setTextDatum(BR_DATUM);
            fb.setTextColor(barCol, C_BG);
            fb.drawString(tokens, SCR_W - 6, SCR_H - 5);

            int bw = SCR_W - 12;
            fb.fillRect(6, SCR_H - 3, bw, 2, 0x2124);
            fb.fillRect(6, SCR_H - 3, (int)(bw * pct / 100), 2, barCol);
        }
    }

    fb.pushSprite(0, 0);
}

void setup() {
#ifdef CRAB_CAPTURE
    Serial.setTxBufferSize(8192);
    Serial.begin(2000000);
#else
    Serial.begin(115200);
#endif
    pinMode(PIN_BTN_TOP, INPUT_PULLUP);
    pinMode(PIN_BTN_BOT, INPUT_PULLUP);

    randomSeed(esp_random());

    prefs.begin("crab", false);
    best = prefs.getInt("best", 0);
    swapCtl = prefs.getBool("swap", false);

    ledcSetup(0, 5000, 8);
    ledcAttachPin(PIN_BL, 0);
    setBacklight(BRIGHT[brightness]);

    tft.init();
    tft.setRotation(1);
    tft.fillScreen(C_BG);
    fb.setColorDepth(16);
    fb.createSprite(SCR_W, SCR_H);

    stateSince = millis();
    lastMsg = millis();
    lastInput = millis();
    pickVignette();
    Serial.println("crab-puck ready: send state|detail|model|used|max lines");
}

void loop() {
    pollSerial();
    pollButtons();

    if (gamePhase == G_OFF) {
        // no word from claude for five minutes, take a nap
        if (state != ST_ASLEEP && millis() - lastMsg > 300000UL) {
            state = ST_ASLEEP;
            stateSince = millis();
            strncpy(detail, "nothing for a while", sizeof(detail) - 1);
            setBacklight(10);
        }
    } else if (millis() - lastInput > 60000UL) {
        gameExit();  // you wandered off mid-game
    }

    updateParticles();
    if (gamePhase == G_OFF) {
        render();
    } else {
        gameUpdate();
        gameRender();
    }
#ifdef CRAB_CAPTURE
    capSend();
#endif
    frame++;
    delay(40);
}

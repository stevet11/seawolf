// Seawolf — a periscope torpedo game. C++17 + raylib.
//
// Ships cross the sea at several distances. Swing the periscope, lead your
// target and fire before the clock runs out. Mines block torpedoes, and
// destroyers roll depth charges toward you that knock the periscope out.
//
// Controls (gamepad): left stick / d-pad aim, A / RB / RT fire,
//                     START pause/start, BACK quit (from title / game over).
// Controls (keyboard): arrows / A-D aim, SPACE fire, P / ENTER pause/start,
//                      ESC quit, F11 borderless fullscreen.

#include "raylib.h"
#include "rlgl.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <random>
#include <string>
#include <vector>

// ------------------------------------------------------------------ constants

constexpr int VW = 1280, VH = 720;     // virtual resolution
constexpr float HORIZON = 250.0f;
constexpr float NEAR_Y = 560.0f;
constexpr float LAUNCH_Y = 662.0f;

constexpr float ROUND_TIME = 90.0f;
constexpr int BONUS_SCORE = 3000;
constexpr float BONUS_TIME = 30.0f;
constexpr int MAX_TUBES = 4;
constexpr float RELOAD_TIME = 1.1f;
constexpr float FIRE_COOLDOWN = 0.22f;
constexpr float STUN_TIME = 2.0f;
constexpr float STUN_PENALTY = 5.0f;
constexpr float SINK_TIME = 3.5f;
constexpr float AIM_SPEED = 780.0f;

// Perspective scale for a given screen y on the water.
static float persp(float y) { return 0.22f + 0.78f * (y - HORIZON) / (NEAR_Y - HORIZON); }

static std::mt19937 rng{std::random_device{}()};
static float frand(float a, float b) { return std::uniform_real_distribution<float>(a, b)(rng); }
static int irand(int a, int b) { return std::uniform_int_distribution<int>(a, b)(rng); }

static Color lerpc(Color a, Color b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return Color{(unsigned char)(a.r + (b.r - a.r) * t), (unsigned char)(a.g + (b.g - a.g) * t),
                 (unsigned char)(a.b + (b.b - a.b) * t), (unsigned char)(a.a + (b.a - a.a) * t)};
}
static Color rgb(int r, int g, int b, int a = 255) {
    return Color{(unsigned char)r, (unsigned char)g, (unsigned char)b, (unsigned char)a};
}

const Color HAZE = rgb(150, 130, 140);

// ----------------------------------------------------------------- data types

struct Lane {
    float y, p;
};

struct ShipKind {
    const char* name;
    float len, deckH, speed;
    int points;
    bool drops, smoke;
};

const ShipKind KINDS[] = {
    {"TANKER", 320, 22, 70, 100, false, true},
    {"FREIGHTER", 250, 24, 95, 150, false, true},
    {"DESTROYER", 230, 18, 140, 250, true, true},
    {"PATROL BOAT", 110, 14, 250, 500, false, false},
};

struct Ship {
    int kind, lane;
    float x, dir;
    bool sinking = false;
    float sinkT = 0, tiltSign = 1;
    float smokeT = 0, wakeT = 0, dropT = 2;
    float bobPhase = 0;
    Vector2 funnel{0, 0}; // screen position of funnel top, updated when drawn
};

struct Torpedo {
    float x, y;
    bool dead = false;
};

struct Mine {
    float x, y, vx, phase;
    bool dead = false;
};

struct Charge {
    float x, y;
    float bubbleT = 0;
    bool dead = false;
};

struct Particle {
    Vector2 pos, vel;
    float life, maxLife, size, grow, grav, drag;
    Color col;
};

struct Popup {
    Vector2 pos;
    std::string text;
    float life;
    Color col;
};

struct Cloud {
    float x, y, w, speed;
};

struct Streak {
    float u, y, phase;
};

// ---------------------------------------------------------------- ship painter

// Draws ship parts in local coordinates (bow toward +x, up toward -y, units
// are pixels at scale 1) transformed to screen space.
struct Painter {
    Vector2 o;
    float s, dir, cs, sn, haze;

    Painter(Vector2 origin, float scale, float direction, float angle, float hazeAmt)
        : o(origin), s(scale), dir(direction), cs(std::cos(angle)), sn(std::sin(angle)), haze(hazeAmt) {}

    Vector2 P(float lx, float ly) const {
        float rx = lx * cs - ly * sn;
        float ry = lx * sn + ly * cs;
        return {o.x + rx * dir * s, o.y + ry * s};
    }
    Color c(Color col) const { return lerpc(col, HAZE, haze); }
    void tri(Vector2 a, Vector2 b, Vector2 d, Color col) const { DrawTriangle(a, b, d, c(col)); }
    void quad(Vector2 a, Vector2 b, Vector2 d, Vector2 e, Color col) const {
        DrawTriangle(a, b, d, c(col));
        DrawTriangle(a, d, e, c(col));
    }
    void rect(float x0, float y0, float x1, float y1, Color col) const {
        quad(P(x0, y0), P(x1, y0), P(x1, y1), P(x0, y1), col);
    }
    void line(float x0, float y0, float x1, float y1, float w, Color col) const {
        float dx = x1 - x0, dy = y1 - y0, l = std::sqrt(dx * dx + dy * dy);
        if (l <= 0) return;
        float nx = -dy / l * w * 0.5f, ny = dx / l * w * 0.5f;
        quad(P(x0 + nx, y0 + ny), P(x1 + nx, y1 + ny), P(x1 - nx, y1 - ny), P(x0 - nx, y0 - ny), col);
    }
    void circle(float lx, float ly, float r, Color col) const { DrawCircleV(P(lx, ly), r * s, c(col)); }
};

// Hull with a raked bow; returns nothing, draws hull + boot stripe + rail.
static void drawHull(const Painter& pt, float L, float H, float bowRise, Color hull, Color boot) {
    float sternX = -L / 2, bowTopX = L / 2 + 12, bowBotX = L / 2 - 14;
    float top = -H, bowTop = -H - bowRise;
    pt.quad(pt.P(sternX, top), pt.P(bowTopX, bowTop), pt.P(bowBotX, 16), pt.P(sternX + 6, 16), hull);
    // Boot stripe just above the waterline.
    auto bowXAt = [&](float y) { return bowTopX + (bowBotX - bowTopX) * (y - bowTop) / (16 - bowTop); };
    pt.quad(pt.P(sternX + 2, -6), pt.P(bowXAt(-6), -6), pt.P(bowXAt(16), 16), pt.P(sternX + 6, 16), boot);
    pt.line(sternX, top, bowTopX, bowTop, 2.5f, lerpc(hull, WHITE, 0.35f));
}

// Draws a ship and returns the funnel top (for smoke) in screen coordinates.
static Vector2 drawShipBody(const Ship& sh, const Painter& pt) {
    const ShipKind& k = KINDS[sh.kind];
    float L = k.len, H = k.deckH;
    const Color window = rgb(35, 50, 70);
    Vector2 smoke{0, 0};

    switch (sh.kind) {
    case 0: { // tanker
        drawHull(pt, L, H, 4, rgb(55, 60, 74), rgb(140, 40, 35));
        pt.rect(-L / 2 + 70, -H - 6, L / 2, -H - 3, rgb(165, 150, 95));     // pipeline
        pt.rect(-12, -H - 14, 12, -H, rgb(150, 140, 90));                    // manifold
        pt.line(L / 2 - 30, -H, L / 2 - 30, -H - 42, 2.5f, rgb(90, 90, 90)); // fore mast
        pt.rect(-L / 2 + 8, -H - 44, -L / 2 + 70, -H, rgb(228, 226, 214));  // accommodation
        pt.rect(-L / 2 + 4, -H - 46, -L / 2 + 76, -H - 40, rgb(240, 240, 232));
        for (int row = 0; row < 3; row++)
            for (int i = 0; i < 6; i++)
                pt.rect(-L / 2 + 14 + i * 9.5f, -H - 36 + row * 11, -L / 2 + 20 + i * 9.5f, -H - 31 + row * 11, window);
        pt.rect(-L / 2 + 18, -H - 68, -L / 2 + 40, -H - 46, rgb(200, 60, 40)); // funnel
        pt.rect(-L / 2 + 18, -H - 72, -L / 2 + 40, -H - 66, rgb(25, 25, 25));
        smoke = pt.P(-L / 2 + 29, -H - 72);
        break;
    }
    case 1: { // freighter
        drawHull(pt, L, H, 6, rgb(130, 42, 36), rgb(30, 30, 34));
        const Color boxes[] = {rgb(200, 70, 50), rgb(50, 110, 170), rgb(230, 170, 40), rgb(60, 140, 80), rgb(180, 180, 175)};
        int idx = 0;
        for (float x = -L / 2 + 82; x < L / 2 - 40; x += 26, idx++) {
            int stack = 1 + (idx * 7 + 3) % 3;
            for (int s = 0; s < stack; s++) {
                Color col = boxes[(idx * 3 + s * 2) % 5];
                pt.rect(x, -H - 13 * (s + 1), x + 24, -H - 13 * s - 1, col);
                pt.rect(x, -H - 13 * (s + 1), x + 24, -H - 13 * (s + 1) + 2, lerpc(col, WHITE, 0.3f));
            }
        }
        for (float cx : {-L / 2 + 78.0f, 30.0f}) { // cranes
            pt.line(cx, -H, cx, -H - 62, 3, rgb(220, 180, 40));
            pt.line(cx, -H - 58, cx + 40, -H - 30, 2, rgb(220, 180, 40));
        }
        pt.rect(L / 2 - 30, -H - 8, L / 2 + 8, -H, rgb(130, 42, 36)); // forecastle
        pt.rect(-L / 2 + 18, -H - 42, -L / 2 + 70, -H, rgb(236, 226, 200));
        for (int row = 0; row < 3; row++)
            for (int i = 0; i < 5; i++)
                pt.rect(-L / 2 + 24 + i * 9.5f, -H - 36 + row * 11, -L / 2 + 30 + i * 9.5f, -H - 31 + row * 11, window);
        pt.rect(-L / 2 + 30, -H - 62, -L / 2 + 50, -H - 42, rgb(30, 30, 30));
        pt.rect(-L / 2 + 30, -H - 56, -L / 2 + 50, -H - 50, rgb(230, 185, 40));
        smoke = pt.P(-L / 2 + 40, -H - 62);
        break;
    }
    case 2: { // destroyer
        const Color grey = rgb(112, 122, 134), light = rgb(146, 156, 168), dark = rgb(70, 78, 90);
        drawHull(pt, L, H, 10, grey, dark);
        pt.rect(-L / 2 + 4, -H - 6, -L / 2 + 18, -H, dark); // depth charge rack
        pt.rect(-90, -H - 12, -66, -H, light);              // aft turret
        pt.line(-72, -H - 8, -106, -H - 10, 3, dark);
        pt.rect(62, -H - 12, 86, -H, light); // fore turret
        pt.line(80, -H - 8, 114, -H - 11, 3, dark);
        pt.rect(-42, -H - 22, 50, -H, light); // superstructure
        pt.rect(8, -H - 40, 46, -H - 22, light);
        pt.rect(14, -H - 36, 44, -H - 31, window);
        pt.line(26, -H - 40, 22, -H - 88, 3, dark); // mast
        pt.line(12, -H - 74, 34, -H - 74, 2, dark);
        pt.rect(18, -H - 92, 28, -H - 86, dark); // radar
        pt.rect(-30, -H - 46, -10, -H - 22, grey); // funnel
        pt.rect(-30, -H - 50, -10, -H - 44, rgb(30, 30, 30));
        smoke = pt.P(-20, -H - 50);
        break;
    }
    default: { // patrol boat
        drawHull(pt, L, H, 6, rgb(215, 215, 205), rgb(40, 80, 140));
        pt.rect(-30, -H - 18, 14, -H, rgb(232, 232, 226));
        pt.tri(pt.P(14, -H - 18), pt.P(30, -H), pt.P(14, -H), rgb(232, 232, 226));
        pt.rect(-24, -H - 14, 10, -H - 9, window);
        pt.line(-10, -H - 18, -13, -H - 40, 2, rgb(80, 80, 80));
        pt.rect(34, -H - 7, 42, -H, rgb(90, 95, 100));
        pt.line(40, -H - 5, 56, -H - 7, 2, rgb(60, 60, 60));
        smoke = pt.P(-L / 2, -H);
        break;
    }
    }
    return smoke;
}

// ---------------------------------------------------------------------- sound

static Sound synth(float dur, const std::function<float(float)>& f) {
    const int rate = 44100;
    int n = (int)(dur * rate);
    auto* data = (short*)MemAlloc(n * sizeof(short));
    for (int i = 0; i < n; i++) {
        float v = std::clamp(f((float)i / rate), -1.0f, 1.0f);
        data[i] = (short)(v * 32000);
    }
    Wave w{(unsigned)n, (unsigned)rate, 16, 1, data};
    Sound s = LoadSoundFromWave(w);
    UnloadWave(w);
    return s;
}

static float noise() { return frand(-1, 1); }

struct Sounds {
    Sound launch{}, explode{}, boom{}, splash{}, ping{}, click{}, tick{}, sea{};
    bool ok = false;

    void load() {
        ok = IsAudioDeviceReady();
        if (!ok) return;
        constexpr float TAU = 6.2831853f;
        {
            float lp = 0;
            launch = synth(0.8f, [=](float t) mutable {
                lp += 0.08f * (noise() - lp);
                float whoosh = lp * 3.0f * std::sin(std::min(t / 0.8f, 1.0f) * 3.14159f);
                float thunk = std::sin(TAU * 85 * t) * std::exp(-t * 18);
                return 0.7f * thunk + 0.5f * whoosh;
            });
        }
        {
            float lp = 0;
            explode = synth(1.6f, [=](float t) mutable {
                float a = 0.25f * std::exp(-t * 2.0f) + 0.02f;
                lp += a * (noise() - lp);
                return (lp * 2.2f + std::sin(TAU * 48 * t) * std::exp(-t * 6)) * std::exp(-t * 2.2f);
            });
        }
        {
            float lp = 0;
            boom = synth(1.8f, [=](float t) mutable {
                lp += 0.04f * (noise() - lp);
                return (lp * 4.0f + 0.9f * std::sin(TAU * (40 - 10 * t) * t)) * std::exp(-t * 2.0f);
            });
        }
        {
            float prev = 0;
            splash = synth(0.7f, [=](float t) mutable {
                float n = noise(), hp = n - prev;
                prev = n;
                return 0.5f * hp * std::exp(-t * 6);
            });
        }
        ping = synth(2.0f, [=](float t) {
            auto p = [&](float tt) { return tt < 0 ? 0.0f : std::sin(TAU * 1320 * tt) * std::exp(-tt * 4.5f); };
            return 0.45f * p(t) + 0.18f * p(t - 0.55f) + 0.07f * p(t - 1.1f);
        });
        click = synth(0.06f, [=](float t) { return 0.3f * (std::fmod(t * 520, 1.0f) < 0.5f ? 1.0f : -1.0f) * std::exp(-t * 60); });
        tick = synth(0.08f, [=](float t) { return 0.35f * std::sin(TAU * 980 * t) * std::exp(-t * 40); });

        // Seamless looping sea ambience (crossfaded brown noise).
        {
            const int rate = 44100, n = rate * 6, fade = rate;
            std::vector<float> raw(n + fade);
            float lp = 0, lp2 = 0;
            for (int i = 0; i < n + fade; i++) {
                lp += 0.02f * (noise() - lp);
                lp2 += 0.3f * (lp - lp2);
                float swell = 0.6f + 0.4f * std::sin(6.2831853f * i / (float)n * 2);
                raw[i] = lp2 * 6.0f * swell;
            }
            auto* data = (short*)MemAlloc(n * sizeof(short));
            for (int i = 0; i < n; i++) {
                float v = raw[i];
                if (i < fade) v = raw[i] * (i / (float)fade) + raw[n + i] * (1 - i / (float)fade);
                data[i] = (short)(std::clamp(v, -1.0f, 1.0f) * 32000);
            }
            Wave w{(unsigned)n, (unsigned)rate, 16, 1, data};
            sea = LoadSoundFromWave(w);
            UnloadWave(w);
            SetSoundVolume(sea, 0.35f);
        }
        SetSoundVolume(boom, 1.0f);
    }

    void play(Sound s, float vol = 1.0f, float pitch = 1.0f) {
        if (!ok) return;
        SetSoundVolume(s, vol);
        SetSoundPitch(s, pitch);
        PlaySound(s);
    }
    void unload() {
        if (!ok) return;
        for (Sound s : {launch, explode, boom, splash, ping, click, tick, sea}) UnloadSound(s);
    }
};

// ---------------------------------------------------------------------- input

// GLFW is linked into raylib; we reach in to fix up pads it has no mapping for.
extern "C" {
int glfwJoystickPresent(int jid);
int glfwJoystickIsGamepad(int jid);
const char* glfwGetJoystickGUID(int jid);
const char* glfwGetJoystickName(int jid);
}

// Many Xbox 360-compatible pads (clones, odd firmware versions) report a GUID
// that is missing from GLFW's built-in database, so they show up with a name
// but no buttons or axes. Give them the standard Linux xpad layout.
static void ensurePadMappings() {
    for (int jid = 0; jid < 4; jid++) {
        if (!glfwJoystickPresent(jid) || glfwJoystickIsGamepad(jid)) continue;
        const char* guid = glfwGetJoystickGUID(jid);
        const char* name = glfwGetJoystickName(jid);
        if (!guid || !name) continue;
        std::string n = name;
        std::transform(n.begin(), n.end(), n.begin(), ::tolower);
        if (n.find("x-box") == std::string::npos && n.find("xbox") == std::string::npos &&
            n.find("xinput") == std::string::npos)
            continue;
        std::string map = std::string(guid) +
                          ",Xbox 360 Controller,a:b0,b:b1,x:b2,y:b3,leftshoulder:b4,rightshoulder:b5,"
                          "back:b6,start:b7,guide:b8,leftstick:b9,rightstick:b10,"
                          "dpup:h0.1,dpright:h0.2,dpdown:h0.4,dpleft:h0.8,"
                          "leftx:a0,lefty:a1,lefttrigger:a2,rightx:a3,righty:a4,righttrigger:a5,platform:Linux,";
        SetGamepadMappings(map.c_str());
        TraceLog(LOG_INFO, "Added gamepad mapping for %s (%s)", name, guid);
    }
}

struct Input {
    float move = 0;
    bool fireHeld = false, fire = false, start = false, back = false, quit = false;
    bool pad = false;
};

static Input readInput(bool prevFireHeld) {
    Input in;
    const int gp = 0;
    in.pad = IsGamepadAvailable(gp);
    if (in.pad) {
        float ax = GetGamepadAxisMovement(gp, GAMEPAD_AXIS_LEFT_X);
        if (std::fabs(ax) < 0.15f) ax = 0;
        else ax = (ax > 0 ? 1 : -1) * std::pow((std::fabs(ax) - 0.15f) / 0.85f, 1.6f);
        in.move += ax;
        if (IsGamepadButtonDown(gp, GAMEPAD_BUTTON_LEFT_FACE_LEFT)) in.move -= 1;
        if (IsGamepadButtonDown(gp, GAMEPAD_BUTTON_LEFT_FACE_RIGHT)) in.move += 1;
        float rt = GetGamepadAxisMovement(gp, GAMEPAD_AXIS_RIGHT_TRIGGER); // -1 rest .. 1 pressed
        in.fireHeld |= IsGamepadButtonDown(gp, GAMEPAD_BUTTON_RIGHT_FACE_DOWN) ||
                       IsGamepadButtonDown(gp, GAMEPAD_BUTTON_RIGHT_TRIGGER_1) || rt > 0.3f;
        in.start |= IsGamepadButtonPressed(gp, GAMEPAD_BUTTON_MIDDLE_RIGHT);
        in.back |= IsGamepadButtonPressed(gp, GAMEPAD_BUTTON_MIDDLE_LEFT);
    }
    if (IsKeyDown(KEY_LEFT) || IsKeyDown(KEY_A)) in.move -= 1;
    if (IsKeyDown(KEY_RIGHT) || IsKeyDown(KEY_D)) in.move += 1;
    in.fireHeld |= IsKeyDown(KEY_SPACE) || IsKeyDown(KEY_UP) || IsKeyDown(KEY_W);
    in.start |= IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_P);
    in.quit = IsKeyPressed(KEY_ESCAPE);
    in.move = std::clamp(in.move, -1.0f, 1.0f);
    in.fire = in.fireHeld && !prevFireHeld;
    return in;
}

// ----------------------------------------------------------------------- game

enum class State { Title, Playing, Paused, Over };

static std::string hiPath() {
    const char* home = std::getenv("HOME");
    return std::string(home ? home : ".") + "/.seawolf_hi";
}
static int loadHi() {
    int v = 0;
    if (FILE* f = std::fopen(hiPath().c_str(), "r")) {
        if (std::fscanf(f, "%d", &v) != 1) v = 0;
        std::fclose(f);
    }
    return v;
}
static void saveHi(int v) {
    if (FILE* f = std::fopen(hiPath().c_str(), "w")) {
        std::fprintf(f, "%d\n", v);
        std::fclose(f);
    }
}

struct Game {
    State state = State::Title;
    Sounds snd;

    std::vector<Lane> lanes;
    std::vector<Ship> ships;
    std::vector<Torpedo> torps;
    std::vector<Mine> mines;
    std::vector<Charge> charges;
    std::vector<Particle> waterFx, smokeFx, fireFx;
    std::vector<Popup> popups;
    std::vector<Cloud> clouds;
    std::vector<Streak> streaks;

    float clock = 0;
    int score = 0, hi = 0, shots = 0, hits = 0, combo = 0;
    bool newHi = false, bonusGiven = false;
    float timeLeft = ROUND_TIME;
    float aimX = VW / 2.0f;
    int tubes = MAX_TUBES;
    float reload = 0, cooldown = 0, stun = 0, shake = 0, flash = 0;
    float spawnT = 0.5f, mineT = 3.0f, overT = 0;
    int lastTick = -1;
    bool prevFire = false;
    bool padSeen = false;

    void init() {
        for (float y : {292.0f, 345.0f, 425.0f, 540.0f}) lanes.push_back({y, persp(y)});
        for (int i = 0; i < 7; i++)
            clouds.push_back({frand(0, VW), frand(40, 180), frand(120, 320), frand(4, 12)});
        for (int i = 0; i < 260; i++) {
            float t = frand(0, 1);
            streaks.push_back({frand(0, 1), HORIZON + 4 + t * t * (VH - HORIZON - 60), frand(0, 6.28f)});
        }
        hi = loadHi();
        snd.load();
        if (snd.ok) PlaySound(snd.sea);
    }

    void reset() {
        score = shots = hits = combo = 0;
        newHi = bonusGiven = false;
        timeLeft = ROUND_TIME;
        tubes = MAX_TUBES;
        reload = cooldown = stun = shake = flash = 0;
        spawnT = 0.3f;
        mineT = 3.0f;
        lastTick = -1;
        torps.clear();
        mines.clear();
        charges.clear();
        popups.clear();
        aimX = VW / 2.0f;
        snd.play(snd.ping, 0.6f);
    }

    // ---------------------------------------------------------------- effects

    void addParticle(std::vector<Particle>& v, Vector2 p, Vector2 vel, float life, float size, float grow, float grav,
                     float drag, Color c) {
        v.push_back({p, vel, life, life, size, grow, grav, drag, c});
    }

    void explosion(float x, float y, float p, bool big) {
        int nFire = big ? 40 : 14;
        addParticle(fireFx, {x, y - 10 * p}, {0, 0}, 0.18f, (big ? 90 : 40) * p, 0, 0, 0, rgb(255, 230, 160));
        for (int i = 0; i < nFire; i++) {
            float a = frand(3.3f, 6.1f), sp = frand(60, 280) * p;
            addParticle(fireFx, {x + frand(-10, 10) * p, y - 8 * p}, {std::cos(a) * sp * 1.3f, std::sin(a) * sp},
                        frand(0.35f, 0.9f), frand(6, 15) * p, -8 * p, 220 * p, 1.5f,
                        irand(0, 1) ? rgb(255, 150, 40) : rgb(255, 210, 80));
        }
        for (int i = 0; i < (big ? 22 : 8); i++)
            addParticle(smokeFx, {x + frand(-30, 30) * p, y - frand(10, 40) * p}, {frand(-25, 25) * p, frand(-70, -20) * p},
                        frand(1.2f, 2.6f), frand(7, 14) * p, 14 * p, -5 * p, 0.6f, rgb(70, 68, 72, 130));
        splash(x, y, p, big ? 45 : 18);
        for (int i = 0; i < (big ? 12 : 0); i++)
            addParticle(smokeFx, {x, y - 10 * p}, {frand(-220, 220) * p, frand(-380, -120) * p}, frand(0.8f, 1.4f),
                        frand(2, 4) * p, 0, 600 * p, 0.2f, rgb(30, 25, 20));
        shake = std::max(shake, big ? 0.35f : 0.15f);
    }

    void splash(float x, float y, float p, int n) {
        for (int i = 0; i < n; i++)
            addParticle(smokeFx, {x + frand(-14, 14) * p, y}, {frand(-50, 50) * p, frand(-460, -120) * p},
                        frand(0.8f, 1.5f), frand(3, 7) * p, 2 * p, 620 * p, 0.3f, rgb(225, 238, 245, 230));
        for (int i = 0; i < n / 2; i++)
            addParticle(waterFx, {x + frand(-30, 30) * p, y + frand(-2, 4) * p}, {frand(-30, 30) * p, 0}, frand(1.5f, 2.5f),
                        frand(4, 8) * p, 10 * p, 0, 1.0f, rgb(220, 235, 240, 160));
    }

    void stepParticles(std::vector<Particle>& v, float dt) {
        for (auto& q : v) {
            q.vel.y += q.grav * dt;
            float d = std::max(0.0f, 1 - q.drag * dt);
            q.vel.x *= d;
            q.vel.y *= d;
            q.pos.x += q.vel.x * dt;
            q.pos.y += q.vel.y * dt;
            q.size = std::max(0.0f, q.size + q.grow * dt);
            q.life -= dt;
        }
        v.erase(std::remove_if(v.begin(), v.end(), [](const Particle& q) { return q.life <= 0 || q.size <= 0; }), v.end());
    }

    void popup(Vector2 pos, const std::string& t, Color c, float life = 1.3f) { popups.push_back({pos, t, life, c}); }

    // ---------------------------------------------------------------- spawning

    bool laneFree(int lane, float dir, float halfLen) {
        for (auto& s : ships) {
            if (s.lane != lane) continue;
            if (s.dir != dir) return false; // one-way lanes so hulls never overlap
            float sh = KINDS[s.kind].len * 0.5f * lanes[lane].p;
            if (dir > 0 && s.x - sh < halfLen * 2 + 40) return false;
            if (dir < 0 && s.x + sh > VW - halfLen * 2 - 40) return false;
        }
        return true;
    }

    void spawnShip() {
        int lane = irand(0, (int)lanes.size() - 1);
        float dir = irand(0, 1) ? 1.0f : -1.0f;
        int w[4] = {3, 4, 3, 1};
        if (state == State::Playing && timeLeft < 45) {
            int late[4] = {2, 3, 4, 2};
            std::copy(late, late + 4, w);
        }
        int r = irand(0, w[0] + w[1] + w[2] + w[3] - 1), k = 0;
        while (r >= w[k]) r -= w[k++];
        float half = KINDS[k].len * 0.5f * lanes[lane].p + 20;
        if (!laneFree(lane, dir, half)) return;
        Ship s;
        s.kind = k;
        s.lane = lane;
        s.dir = dir;
        s.x = dir > 0 ? -half : VW + half;
        s.bobPhase = frand(0, 6.28f);
        s.dropT = frand(1.0f, 2.5f);
        ships.push_back(s);
    }

    // ------------------------------------------------------------------ update

    void fire() {
        if (stun > 0 || cooldown > 0) return;
        if (tubes == 0) {
            snd.play(snd.click, 0.8f);
            return;
        }
        tubes--;
        shots++;
        cooldown = FIRE_COOLDOWN;
        torps.push_back({aimX, LAUNCH_Y});
        snd.play(snd.launch, 0.8f, frand(0.9f, 1.1f));
    }

    void addScore(int pts) {
        int before = score;
        score += pts;
        if (!bonusGiven && before < BONUS_SCORE && score >= BONUS_SCORE) {
            bonusGiven = true;
            timeLeft += BONUS_TIME;
            popup({VW / 2.0f, VH / 2.0f - 40}, "EXTENDED TIME +30", rgb(120, 255, 160), 2.5f);
            snd.play(snd.ping, 0.8f);
        }
        if (score > hi) {
            hi = score;
            newHi = true;
        }
    }

    void endRound() {
        state = State::Over;
        overT = 0;
        if (newHi) saveHi(hi);
        snd.play(snd.ping, 0.7f, 0.7f);
    }

    void update(float dt, const Input& in) {
        clock += dt;
        if (in.pad) padSeen = true;

        switch (state) {
        case State::Title:
            if (in.start || in.fire) {
                reset();
                state = State::Playing;
            }
            break;
        case State::Paused:
            if (in.start) state = State::Playing;
            return; // freeze everything
        case State::Over:
            overT += dt;
            if (overT > 1.2f && (in.start || in.fire)) {
                reset();
                state = State::Playing;
            }
            break;
        case State::Playing:
            if (in.start) {
                state = State::Paused;
                return;
            }
            updatePlayer(dt, in);
            break;
        }

        updateWorld(dt);
    }

    void updatePlayer(float dt, const Input& in) {
        timeLeft -= dt;
        int secs = (int)std::ceil(timeLeft);
        if (timeLeft < 10.5f && secs != lastTick && secs > 0) {
            lastTick = secs;
            snd.play(snd.tick, 0.6f);
        }
        if (timeLeft <= 0) {
            timeLeft = 0;
            endRound();
            return;
        }

        float speed = stun > 0 ? AIM_SPEED * 0.3f : AIM_SPEED;
        aimX = std::clamp(aimX + in.move * speed * dt, 60.0f, VW - 60.0f);
        if (in.fire) fire();

        cooldown = std::max(0.0f, cooldown - dt);
        stun = std::max(0.0f, stun - dt);
        if (tubes < MAX_TUBES) {
            reload += dt;
            if (reload >= RELOAD_TIME) {
                reload = 0;
                tubes++;
            }
        } else {
            reload = 0;
        }

        // Mines.
        mineT -= dt;
        if (mineT <= 0 && mines.size() < 3) {
            int i = irand(0, (int)lanes.size() - 2);
            float y = (lanes[i].y + lanes[i + 1].y) * 0.5f;
            float vx = frand(15, 40) * persp(y);
            float x = -20;
            if (irand(0, 1)) vx = -vx, x = VW + 20;
            mines.push_back({x, y, vx, frand(0, 6.28f)});
            mineT = frand(4, 9);
        }
        for (auto& m : mines) {
            m.x += m.vx * dt;
            if (m.x < -40 || m.x > VW + 40) m.dead = true;
        }

        // Torpedoes.
        for (auto& t : torps) {
            float prev = t.y;
            t.y -= (400 * persp(t.y) + 40) * dt;
            float p = persp(t.y);
            if (irand(0, 1)) addParticle(waterFx, {t.x + frand(-2, 2) * p, t.y + 4 * p}, {0, 0}, 1.4f, 3 * p, 5 * p, 0, 0,
                                         rgb(230, 245, 255, 190));
            // Mines.
            for (auto& m : mines) {
                float mp = persp(m.y);
                if (!m.dead && prev >= m.y && t.y < m.y && std::fabs(t.x - m.x) < 16 * mp + 4) {
                    m.dead = t.dead = true;
                    combo = 0;
                    explosion(m.x, m.y, mp, false);
                    snd.play(snd.explode, 0.6f, 1.3f);
                    popup({m.x, m.y - 40 * mp}, "MINE", rgb(255, 120, 100));
                    break;
                }
            }
            if (t.dead) continue;
            // Ships (waterline crossing).
            for (auto& s : ships) {
                const Lane& ln = lanes[s.lane];
                if (s.sinking || !(prev >= ln.y && t.y < ln.y)) continue;
                const ShipKind& k = KINDS[s.kind];
                float half = k.len * 0.5f * ln.p;
                if (t.x < s.x - half || t.x > s.x + half + 10 * ln.p) continue;
                t.dead = true;
                s.sinking = true;
                s.tiltSign = (t.x - s.x) * s.dir > 0 ? -1.0f : 1.0f; // hit near bow: bow sinks first
                hits++;
                combo++;
                float laneMult = 1.0f + 0.5f * (lanes.size() - 1 - s.lane);
                int m = std::min(combo, 5);
                int pts = (int)(k.points * laneMult) * m;
                addScore(pts);
                std::string label = std::to_string(pts) + (m > 1 ? "  x" + std::to_string(m) : "");
                popup({t.x, ln.y - 90 * ln.p}, label, rgb(255, 230, 120));
                explosion(t.x, ln.y, ln.p, true);
                snd.play(snd.explode, 1.0f, frand(0.85f, 1.05f));
                break;
            }
            if (!t.dead && t.y <= HORIZON + 4) {
                t.dead = true;
                combo = 0;
                splash(t.x, HORIZON + 4, persp(HORIZON + 4), 6);
            }
        }

        // Depth charges roll toward the viewer.
        for (auto& c : charges) {
            float p = persp(c.y);
            c.y += (150 * p + 30) * dt;
            c.bubbleT -= dt;
            if (c.bubbleT <= 0) {
                c.bubbleT = 0.05f;
                addParticle(waterFx, {c.x + frand(-6, 6) * p, c.y - 6 * p}, {0, 0}, 1.0f, 2.5f * p, 4 * p, 0, 0,
                            rgb(200, 225, 240, 150));
            }
            if (c.y >= LAUNCH_Y - 12) {
                c.dead = true;
                float pp = persp(c.y);
                splash(c.x, c.y, pp, 40);
                snd.play(snd.boom, 0.9f);
                shake = std::max(shake, 0.25f);
                if (std::fabs(c.x - aimX) < 85 && stun <= 0) {
                    stun = STUN_TIME;
                    shake = 0.8f;
                    flash = 1.0f;
                    combo = 0;
                    timeLeft = std::max(0.1f, timeLeft - STUN_PENALTY);
                    popup({aimX, LAUNCH_Y - 120}, "PERISCOPE HIT  -5 SEC", rgb(255, 90, 80), 1.8f);
                }
            }
        }

        auto gone = [](const auto& o) { return o.dead; };
        torps.erase(std::remove_if(torps.begin(), torps.end(), gone), torps.end());
        mines.erase(std::remove_if(mines.begin(), mines.end(), gone), mines.end());
        charges.erase(std::remove_if(charges.begin(), charges.end(), gone), charges.end());
    }

    void updateWorld(float dt) {
        spawnT -= dt;
        if (spawnT <= 0) {
            spawnShip();
            spawnT = frand(0.8f, 2.0f);
        }

        for (auto& s : ships) {
            const ShipKind& k = KINDS[s.kind];
            const Lane& ln = lanes[s.lane];
            float half = k.len * 0.5f * ln.p;
            if (s.sinking) {
                s.sinkT += dt;
                if (s.sinkT < 2.6f && irand(0, 2) == 0) {
                    float fx = s.x + frand(-half, half) * 0.8f;
                    addParticle(fireFx, {fx, ln.y - frand(5, 25) * ln.p}, {frand(-10, 10), frand(-60, -20) * ln.p},
                                frand(0.3f, 0.7f), frand(5, 10) * ln.p, -6 * ln.p, -20, 0.5f,
                                irand(0, 1) ? rgb(255, 140, 40) : rgb(255, 200, 70));
                    addParticle(smokeFx, {fx, ln.y - 25 * ln.p}, {frand(-10, 10) * ln.p, frand(-50, -25) * ln.p},
                                frand(2, 3.5f), 8 * ln.p, 16 * ln.p, 0, 0.3f, rgb(40, 38, 40, 170));
                }
                continue;
            }
            s.x += s.dir * k.speed * ln.p * dt;

            s.smokeT -= dt;
            if (k.smoke && s.smokeT <= 0 && s.funnel.x != 0) {
                s.smokeT = 0.1f;
                addParticle(smokeFx, s.funnel, {-s.dir * 10 * ln.p + frand(-4, 4), -22 * ln.p}, 3.0f, 4 * ln.p, 9 * ln.p,
                            -3, 0.2f, rgb(70, 66, 70, 120));
            }

            s.wakeT -= dt;
            if (s.wakeT <= 0) {
                s.wakeT = 0.05f;
                float sternX = s.x - s.dir * half;
                addParticle(waterFx, {sternX + frand(-4, 4) * ln.p, ln.y + frand(-1, 3) * ln.p},
                            {-s.dir * 8 * ln.p, 0}, 2.2f, 4 * ln.p, 7 * ln.p, 0, 0.3f, rgb(235, 245, 250, 150));
                float bowX = s.x + s.dir * (half + 4 * ln.p);
                float spray = k.speed > 200 ? 2.0f : 1.0f;
                addParticle(waterFx, {bowX, ln.y + 1}, {s.dir * frand(5, 25) * ln.p, frand(-15, 0) * ln.p * spray},
                            0.8f, 3 * ln.p * spray, 5 * ln.p, 30 * ln.p, 0.5f, rgb(245, 250, 255, 190));
            }

            if (k.drops && state == State::Playing && s.x > 80 && s.x < VW - 80) {
                s.dropT -= dt;
                if (s.dropT <= 0) {
                    s.dropT = frand(1.8f, 3.8f);
                    charges.push_back({s.x - s.dir * half * 0.8f, ln.y + 6});
                    splash(s.x - s.dir * half * 0.8f, ln.y, ln.p, 8);
                    snd.play(snd.splash, 0.5f * ln.p + 0.2f);
                }
            }
        }
        ships.erase(std::remove_if(ships.begin(), ships.end(),
                                   [&](const Ship& s) {
                                       if (s.sinking) return s.sinkT > SINK_TIME;
                                       float half = KINDS[s.kind].len * 0.5f * lanes[s.lane].p + 30;
                                       return (s.dir > 0 && s.x - half > VW) || (s.dir < 0 && s.x + half < 0);
                                   }),
                    ships.end());

        for (auto& c : clouds) {
            c.x += c.speed * dt;
            if (c.x - c.w > VW) c.x = -c.w, c.y = frand(40, 180);
        }

        stepParticles(waterFx, dt);
        stepParticles(smokeFx, dt);
        stepParticles(fireFx, dt);
        for (auto& p : popups) {
            p.life -= dt;
            p.pos.y -= 30 * dt;
        }
        popups.erase(std::remove_if(popups.begin(), popups.end(), [](const Popup& p) { return p.life <= 0; }), popups.end());
        shake = std::max(0.0f, shake - dt);
        flash = std::max(0.0f, flash - dt * 0.8f);
    }

    // -------------------------------------------------------------------- draw

    void drawSky() {
        DrawRectangleGradientV(0, 0, VW, (int)HORIZON, rgb(18, 26, 64), rgb(232, 146, 104));
        DrawCircleGradient(930, (int)HORIZON - 30, 120, rgb(255, 200, 140, 120), rgb(255, 160, 120, 0));
        DrawCircle(930, (int)HORIZON - 30, 34, rgb(255, 226, 170));
        for (auto& c : clouds) {
            Color col = rgb(250, 190, 170, 110);
            DrawEllipse((int)c.x, (int)c.y, c.w * 0.5f, c.w * 0.08f, col);
            DrawEllipse((int)(c.x + c.w * 0.15f), (int)(c.y - c.w * 0.04f), c.w * 0.3f, c.w * 0.07f, col);
        }
        // Distant coastline.
        for (int x = 0; x < VW; x += 4) {
            float h = 10 + 8 * std::sin(x * 0.012f) + 5 * std::sin(x * 0.037f + 1.3f);
            if (x > 380 && x < 760) h *= 0.2f;
            DrawRectangle(x, (int)(HORIZON - h), 4, (int)h + 1, rgb(80, 70, 100));
        }
    }

    void drawSea() {
        DrawRectangleGradientV(0, (int)HORIZON, VW, VH - (int)HORIZON, rgb(60, 92, 128), rgb(6, 26, 52));
        // Sun glitter.
        for (int i = 0; i < 40; i++) {
            float y = HORIZON + 3 + i * i * 0.12f;
            float w = (8 + i * 1.2f) * (0.6f + 0.4f * std::sin(clock * 3 + i * 1.7f));
            DrawRectangle((int)(930 - w / 2 + std::sin(clock * 2 + i) * 6), (int)y, (int)w, 2, rgb(255, 210, 160, 120 - i * 2));
        }
        for (auto& s : streaks) {
            float p = persp(s.y);
            float x = std::fmod(s.u * VW + clock * 12 * p + VW * 10, (float)VW + 80) - 40;
            float a = 0.5f + 0.5f * std::sin(clock * 1.3f + s.phase);
            DrawRectangle((int)x, (int)s.y, (int)(36 * p), std::max(1, (int)(2 * p)), rgb(170, 200, 225, (int)(70 * a)));
        }
        DrawLine(0, (int)HORIZON, VW, (int)HORIZON, rgb(255, 210, 170, 150));
    }

    void drawParticles(const std::vector<Particle>& v) {
        for (auto& q : v) {
            float a = q.life / q.maxLife;
            Color c = q.col;
            c.a = (unsigned char)(c.a * std::min(1.0f, a * 1.5f));
            DrawCircleV(q.pos, q.size, c);
        }
    }

    void drawShip(Ship& s) {
        const Lane& ln = lanes[s.lane];
        float bob = std::sin(clock * 1.6f + s.bobPhase) * 1.5f * ln.p;
        float ang = std::sin(clock * 1.1f + s.bobPhase) * 0.012f;
        float sink = 0;
        if (s.sinking) {
            float t = s.sinkT;
            ang += s.tiltSign * std::min(t * 0.09f, 0.32f);
            sink = t * t * 7.0f * ln.p;
        }
        Painter pt({s.x, ln.y + bob + sink}, ln.p, s.dir, ang, (1 - ln.p) * 0.45f);
        BeginScissorMode(0, 0, VW, (int)(ln.y + 2));
        s.funnel = drawShipBody(s, pt);
        EndScissorMode();
        if (s.sinking) return;
        // Waterline foam.
        float half = KINDS[s.kind].len * 0.5f * ln.p;
        DrawEllipse((int)(s.x + s.dir * half), (int)(ln.y + 2), 16 * ln.p, 3 * ln.p, rgb(240, 248, 255, 170));
        DrawEllipse((int)s.x, (int)(ln.y + 2), half, 2.5f * ln.p, rgb(220, 235, 245, 70));
    }

    void drawMine(const Mine& m) {
        float p = persp(m.y);
        float y = m.y + std::sin(clock * 2 + m.phase) * 2 * p;
        float r = 12 * p;
        for (int i = 0; i < 6; i++) {
            float a = i * 3.14159f / 5 + 3.14159f;
            DrawLineEx({m.x, y}, {m.x + std::cos(a) * r * 1.5f, y + std::sin(a) * r * 1.5f}, 2 * p, rgb(30, 30, 30));
        }
        DrawCircleV({m.x, y}, r, rgb(28, 30, 34));
        DrawCircleV({m.x - r * 0.35f, y - r * 0.4f}, r * 0.3f, rgb(90, 95, 100));
        if (std::fmod(clock + m.phase, 1.0f) < 0.4f) DrawCircleV({m.x, y - r * 1.5f}, 3 * p, rgb(255, 60, 40));
        DrawEllipse((int)m.x, (int)(y + r * 0.6f), r * 1.4f, r * 0.25f, rgb(220, 235, 245, 90));
    }

    void drawCharge(const Charge& c) {
        float p = persp(c.y);
        Rectangle r{c.x - 9 * p, c.y - 11 * p, 18 * p, 22 * p};
        DrawRectangleRounded(r, 0.4f, 6, rgb(20, 30, 40, 200));
        DrawRectangleRounded({r.x + 2 * p, r.y + 4 * p, r.width - 4 * p, 3 * p}, 0.5f, 4, rgb(150, 40, 30, 220));
    }

    void drawTorpedo(const Torpedo& t) {
        float p = persp(t.y);
        DrawEllipse((int)t.x, (int)t.y, 3 * p + 1, 9 * p + 2, rgb(20, 30, 40, 220));
        DrawCircleV({t.x, t.y - 8 * p}, 2 * p + 1, rgb(255, 255, 255, 220));
    }

    void drawReticle() {
        Color c = stun > 0 ? rgb(140, 140, 140, 120) : rgb(140, 255, 160, 150);
        float x = aimX;
        // Faint aim line from the launch point to the horizon.
        for (float y = HORIZON - 70; y < LAUNCH_Y; y += 14) DrawLineEx({x, y}, {x, y + 6}, 1.5f, Fade(c, 0.5f));
        DrawLineEx({x - 120, HORIZON}, {x + 120, HORIZON}, 1.5f, c);
        for (int i = -6; i <= 6; i++) {
            float h = i % 3 == 0 ? 12 : 6;
            DrawLineEx({x + i * 20.0f, HORIZON - h}, {x + i * 20.0f, HORIZON + h}, 1.5f, c);
        }
        DrawRing({x, HORIZON}, 16, 18, 0, 360, 32, c);
        DrawTriangle({x, LAUNCH_Y - 6}, {x - 12, LAUNCH_Y + 14}, {x + 12, LAUNCH_Y + 14}, c);
    }

    void drawFrame() {
        // Periscope vignette.
        for (int i = 0; i < 6; i++) {
            int a = 230 - i * 38;
            int w = 22 + i * 18;
            DrawRectangleGradientH(0, 0, w, VH, rgb(0, 0, 0, a), rgb(0, 0, 0, 0));
            DrawRectangleGradientH(VW - w, 0, w, VH, rgb(0, 0, 0, 0), rgb(0, 0, 0, a));
        }
        DrawRectangleGradientV(0, 0, VW, 60, rgb(0, 0, 0, 200), rgb(0, 0, 0, 0));
        // Bottom console.
        DrawRectangleGradientV(0, VH - 46, VW, 46, rgb(30, 34, 40), rgb(12, 14, 18));
        DrawLine(0, VH - 46, VW, VH - 46, rgb(90, 100, 110));
    }

    void text(const char* t, int x, int y, int size, Color c) {
        DrawText(t, x + 2, y + 2, size, rgb(0, 0, 0, 160));
        DrawText(t, x, y, size, c);
    }
    void ctext(const char* t, int y, int size, Color c) { text(t, (VW - MeasureText(t, size)) / 2, y, size, c); }

    void drawHUD() {
        char buf[64];
        std::snprintf(buf, sizeof buf, "SCORE %06d", score);
        text(buf, 30, 14, 28, rgb(255, 230, 140));
        std::snprintf(buf, sizeof buf, "HI %06d", hi);
        text(buf, 280, 18, 22, rgb(200, 200, 210));

        int secs = (int)std::ceil(timeLeft);
        std::snprintf(buf, sizeof buf, "TIME %02d", secs);
        Color tc = (timeLeft < 10 && std::fmod(clock, 0.5f) < 0.25f) ? rgb(255, 80, 70) : rgb(255, 230, 140);
        ctext(buf, 12, 32, tc);

        if (combo > 1) {
            std::snprintf(buf, sizeof buf, "COMBO x%d", std::min(combo, 5));
            text(buf, VW - 230, 16, 26, rgb(255, 160, 70));
        }

        // Tubes on the console.
        text("TUBES", 30, VH - 34, 22, rgb(170, 180, 190));
        for (int i = 0; i < MAX_TUBES; i++) {
            float x = 120 + i * 70.0f, y = VH - 30;
            Rectangle r{x, y, 56, 16};
            DrawRectangleRounded(r, 0.8f, 8, rgb(40, 44, 50));
            if (i < tubes) {
                DrawRectangleRounded(r, 0.8f, 8, rgb(110, 230, 130));
            } else if (i == tubes) {
                DrawRectangleRounded({x, y, 56 * (reload / RELOAD_TIME), 16}, 0.8f, 8, rgb(200, 170, 60));
            }
        }
        if (stun > 0 && std::fmod(clock, 0.4f) < 0.25f) ctext("PERISCOPE FLOODED", VH - 36, 24, rgb(255, 100, 90));

        const char* ctl = padSeen ? "STICK aim   A/RT fire   START pause" : "ARROWS aim   SPACE fire   P pause";
        text(ctl, VW - MeasureText(ctl, 18) - 30, VH - 32, 18, rgb(130, 140, 150));
    }

    void drawPanel(int y, int h) {
        DrawRectangleRounded({VW / 2.0f - 380, (float)y, 760, (float)h}, 0.08f, 8, rgb(5, 10, 20, 215));
        DrawRectangleRoundedLinesEx({VW / 2.0f - 380, (float)y, 760, (float)h}, 0.08f, 8, 2, rgb(110, 200, 220, 160));
    }

    void drawTitle() {
        drawPanel(110, 470);
        ctext("SEAWOLF", 130, 84, rgb(140, 230, 255));
        ctext("Sink as many ships as you can before time runs out.", 236, 22, RAYWHITE);
        ctext("Torpedoes take time to run - lead your targets!", 264, 22, RAYWHITE);
        int y = 312;
        for (int i = 0; i < 4; i++) {
            char buf[64];
            std::snprintf(buf, sizeof buf, "%-12s %4d", KINDS[i].name, KINDS[i].points);
            int x = VW / 2 - 330 + (i % 2) * 340;
            text(buf, x, y + (i / 2) * 30, 22, rgb(255, 230, 140));
        }
        ctext("Distant ships score up to 2.5x.  Consecutive hits multiply up to x5.", 386, 20, rgb(200, 210, 220));
        ctext("Mines block torpedoes. Destroyers roll depth charges at you - dodge them!", 412, 20, rgb(200, 210, 220));
        char buf[96];
        std::snprintf(buf, sizeof buf, "Reach %d points for +%d seconds.", BONUS_SCORE, (int)BONUS_TIME);
        ctext(buf, 438, 20, rgb(200, 210, 220));

        std::string pad = IsGamepadAvailable(0) ? std::string("Controller: ") + GetGamepadName(0) : "No controller detected - keyboard ready";
        ctext(pad.c_str(), 482, 18, IsGamepadAvailable(0) ? rgb(120, 255, 160) : rgb(255, 180, 120));
        if (std::fmod(clock, 1.0f) < 0.65f) ctext("PRESS START / A TO DIVE", 524, 30, rgb(140, 230, 255));
    }

    void drawOver() {
        drawPanel(170, 360);
        ctext("TIME'S UP", 196, 64, rgb(140, 230, 255));
        char buf[96];
        std::snprintf(buf, sizeof buf, "SCORE  %d", score);
        ctext(buf, 290, 36, rgb(255, 230, 140));
        int acc = shots ? hits * 100 / shots : 0;
        std::snprintf(buf, sizeof buf, "Ships sunk %d     Accuracy %d%%", hits, acc);
        ctext(buf, 340, 24, RAYWHITE);
        if (newHi && std::fmod(clock, 0.6f) < 0.4f) ctext("NEW HIGH SCORE!", 386, 30, rgb(255, 160, 70));
        if (overT > 1.2f) ctext("START / A to play again     BACK / ESC to quit", 460, 22, rgb(170, 200, 220));
    }

    void draw() {
        Camera2D cam{};
        cam.zoom = 1;
        if (shake > 0) cam.offset = {frand(-1, 1) * shake * 18, frand(-1, 1) * shake * 12};
        BeginMode2D(cam);
        drawSky();
        drawSea();
        drawParticles(waterFx);
        for (auto& m : mines) drawMine(m);
        for (int l = 0; l < (int)lanes.size(); l++)
            for (auto& s : ships)
                if (s.lane == l) drawShip(s);
        for (auto& c : charges) drawCharge(c);
        for (auto& t : torps) drawTorpedo(t);
        drawParticles(smokeFx);
        BeginBlendMode(BLEND_ADDITIVE);
        drawParticles(fireFx);
        EndBlendMode();
        if (state == State::Playing || state == State::Paused) drawReticle();
        EndMode2D();

        for (auto& p : popups) {
            int size = 30;
            Color c = p.col;
            c.a = (unsigned char)(255 * std::min(1.0f, p.life * 2));
            int w = MeasureText(p.text.c_str(), size);
            DrawText(p.text.c_str(), (int)p.pos.x - w / 2 + 2, (int)p.pos.y + 2, size, rgb(0, 0, 0, c.a / 2));
            DrawText(p.text.c_str(), (int)p.pos.x - w / 2, (int)p.pos.y, size, c);
        }

        if (flash > 0) {
            DrawRectangle(0, 0, VW, VH, rgb(150, 200, 230, (int)(flash * 160)));
            for (int i = 0; i < 30; i++) {
                float x = std::fmod(i * 197.0f, (float)VW), y = std::fmod(i * 131.0f + (1 - flash) * 200, (float)VH);
                DrawCircleV({x, y}, 6 + (i % 4) * 4, rgb(220, 240, 255, (int)(flash * 120)));
            }
        }

        drawFrame();
        if (state != State::Title) drawHUD();
        if (state == State::Title) drawTitle();
        if (state == State::Over) drawOver();
        if (state == State::Paused) {
            DrawRectangle(0, 0, VW, VH, rgb(0, 0, 0, 120));
            ctext("PAUSED", VH / 2 - 40, 60, RAYWHITE);
            ctext("START / P to resume", VH / 2 + 30, 24, rgb(170, 200, 220));
        }
    }
};

// ----------------------------------------------------------------------- main

int main() {
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT | FLAG_MSAA_4X_HINT);
    InitWindow(VW, VH, "Seawolf");
    SetWindowMinSize(640, 360);
    SetExitKey(KEY_NULL);
    SetTargetFPS(60);
    InitAudioDevice();
    rlDisableBackfaceCulling(); // mirrored ships flip triangle winding

    RenderTexture2D target = LoadRenderTexture(VW, VH);
    SetTextureFilter(target.texture, TEXTURE_FILTER_BILINEAR);

    ensurePadMappings();
    Game g;
    g.init();

    bool quit = false;
    const bool autoplay = std::getenv("SEAWOLF_AUTOPLAY") != nullptr;
    while (!quit && !WindowShouldClose()) {
        if (IsKeyPressed(KEY_F11)) ToggleBorderlessWindowed();
        static double lastPadCheck = 0;
        if (GetTime() - lastPadCheck > 1.0) { // also catches pads plugged in later
            lastPadCheck = GetTime();
            ensurePadMappings();
        }
        if (g.snd.ok && !IsSoundPlaying(g.snd.sea)) PlaySound(g.snd.sea);

        Input in = readInput(g.prevFire);
        if (autoplay) { // simple bot: lead the first healthy ship and fire (for testing / demo)
            in.start = g.state == State::Title;
            for (auto& s : g.ships) {
                if (s.sinking) continue;
                const Lane& ln = g.lanes[s.lane];
                float tt = (LAUNCH_Y - ln.y) / (400 * ln.p * 0.8f + 40);
                float lead = s.x + s.dir * KINDS[s.kind].speed * ln.p * tt;
                if (lead < 80 || lead > VW - 80) continue;
                in.move = std::clamp((lead - g.aimX) / 60.0f, -1.0f, 1.0f);
                in.fireHeld = std::fabs(lead - g.aimX) < 12 && std::fmod(g.clock, 0.5f) < 0.1f;
                break;
            }
            in.fire = in.fireHeld && !g.prevFire;
        }
        g.prevFire = in.fireHeld;
        if (in.quit || (in.back && (g.state == State::Title || g.state == State::Over))) {
            if (g.state == State::Playing || g.state == State::Paused) {
                if (g.newHi) saveHi(g.hi);
            }
            quit = true;
        }
        if (in.back && g.state == State::Playing) g.state = State::Paused;

        float dt = std::min(GetFrameTime(), 0.05f);
        g.update(dt, in);

        BeginTextureMode(target);
        ClearBackground(BLACK);
        g.draw();
        EndTextureMode();

        BeginDrawing();
        ClearBackground(BLACK);
        float sw = (float)GetScreenWidth(), sh = (float)GetScreenHeight();
        float scale = std::min(sw / VW, sh / VH);
        DrawTexturePro(target.texture, {0, 0, (float)VW, -(float)VH},
                       {(sw - VW * scale) / 2, (sh - VH * scale) / 2, VW * scale, VH * scale}, {0, 0}, 0, WHITE);
        EndDrawing();
    }

    UnloadRenderTexture(target);
    g.snd.unload();
    CloseAudioDevice();
    CloseWindow();
    return 0;
}

// WiiU Aim v1.3: Wii U style pointer aim for Plutonium T6 multiplayer.
//
// The game's view angles (cl->viewangles) stay the AIM direction: that is what goes to the
// server, so bullets go where the crosshair is. The camera is drawn at aim minus an offset.
//
// Hip fire (default, "PC look"): the aim moves 1:1 with the input and the camera trails it by
// hip_follow_time, so the gun swings out into turns and settles back.
//
// ADS (and hip fire with hip_wiiu=1): the Wii U pointer model. The behaviour and default numbers
// follow the Wii U build's wiiu_aim_core.cpp (WiiU_AdjustAxes, WiiU_CalculateTurnRates,
// WiiU_Aim_Core, see NOTES.md):
//   - the pointer is a screen position in [-1, 1]; mouse/stick input moves it and it stops at
//     the screen edge, smoothed like the Wii U's IR filter. The aim goes through that screen
//     point, so the crosshair sits on it.
//   - yaw: past the horizontal deadzone (0.40) the camera turns at turn_yaw * t^0.5, where t is
//     how far into the outer zone the pointer is; the turn rate changes at most turn_accel/s.
//   - pitch (lookspring, hip fire): past the vertical deadzone (0.38) the pointer sets a target
//     pitch (t^0.75 * pitch_max) the camera eases to; back inside, the camera levels out again.
//   - ADS: same pointer model with the ADS deadzone, turn speeds and curve (t^1).
// Outside normal control (dead, killcam, spectating, turrets, vehicles, remote killstreaks,
// locked cameras) the game's camera is left alone.
// Turn speeds default to 3x the Wii U's, which was tuned for pointing at a TV.
// The game places the gun and the crosshair from the aim angles, so once the camera is turned
// back, the gun points at the crosshair and (with cg_crosshairDynamic forced on) the game draws
// its own crosshair at the aim point.
//
// Hooks (t6mp, Plutonium build, see NOTES.md):
//   0x51DEA3  call 0x562860 in CG_DrawActiveFrame -> frame_hook: runs after the gun is placed,
//             before the camera is copied to the renderer (0x51DEAF).
//   0x452DE5  jnz -> jmp in CG_DrawCrosshair: crosshair follows the gun angles, as if
//             cg_crosshairDynamic were 1.
//   0x79C680  call CG_DrawCrosshair -> crosshair_hook: draws the Wii U style reticles (red
//             dot, rocket launcher ring, edge X) while free aim is active.
//   0x7E15D3  call 0x7E1450 (gun rotation) -> wiiuaim_gun_hook: the gun follows the crosshair
//             partly and rolls with it. Sniper scopes already float: the game centers the scope
//             overlay on the projected gun direction (0x79E140).
// Settings: <Plutonium>\wiiuaim\wiiuaim.ini. Log: <Plutonium>\wiiuaim\wiiuaim.log.
//
// Plutonium SDK v1 plugin object, laid out by hand as in BO2-Ring (MSVC 32-bit layout).
#include <windows.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define THISCALL __attribute__((thiscall))

// Assembly stub for the gun hook (bottom of the file).
extern "C" void wiiuaim_gun_hook();

namespace {

// ---------------------------------------------------------------------------------------
// Game addresses

const uintptr_t kClientActive = 0x11C7D74;  // clientActive_t*
const uintptr_t kClientCount = 0x11C7D6C;
const uintptr_t kCg = 0x113F18C;  // cg_t*
const uintptr_t kClViewAngles = 0x2C8C;  // cl->viewangles[0..1]: pitch, yaw
const uintptr_t kCgTanHalfFov = 0x4D8B0;  // cg.refdef.tanHalfFovX, tanHalfFovY
const uintptr_t kCgViewAxis = 0x4D8D4;  // cg.refdef.viewaxis[3][3]: forward, left, up
const uintptr_t kCgAdsFrac = 0x48288;  // predicted ps weapon position fraction, 1 = fully ADS
const uintptr_t kCgClientNum = 0x0;  // cg.clientNum: our own client
const uintptr_t kCgPs = 0x480A8;  // cg.predictedPlayerState
// playerState_s fields (same layout in the PC and Wii U builds; offsets from the netfield table)
const uintptr_t kPsPmType = 0x04;
const uintptr_t kPsPmFlags = 0x0C;
const uintptr_t kPsOtherFlags = 0x18;
const uintptr_t kPsEFlags = 0xFC;
const uintptr_t kPsClientNum = 0x1A0;
const uintptr_t kPsViewLocked = 0x470;
const uintptr_t kPsLinkFlags = 0x484;

const uintptr_t kPsWeapon = 0x1B8;

const uintptr_t kFrameCall = 0x51DEA3;  // E8 -> 0x562860
const uintptr_t kFrameTarget = 0x562860;
const uintptr_t kCrosshairJnz = 0x452DE5;  // 75 15
const uintptr_t kCrosshairCall = 0x79C680;  // E8 -> CG_DrawCrosshair
const uintptr_t kCrosshairTarget = 0x452A60;  // CG_DrawCrosshair(localClientNum) -> bool
// CG_CalculateWeaponMovement_ClientSpecific calls this with cg in ESI and (gunAngles, placement)
// on the stack; it builds the gun rotation from AnglesToAxis(gunAngles) x AnglesToAxis(view).
const uintptr_t kGunAnglesCall = 0x7E15D3;
const uintptr_t kGunAnglesTarget = 0x7E1450;

// 2D drawing (cdecl). Coordinates with align 2/2 are in 640x480 units from the screen center.
typedef void*(__cdecl* ScrPlaceGetView_t)(int localClientNum);
typedef void(__cdecl* DrawStretchPic_t)(void* scrPlace, float x, float y, float depth, float w, float h, int horzAlign,
                                        int vertAlign, float s0, float t0, float s1, float t1, const float* color,
                                        void* material);
typedef void(__cdecl* DrawRotatedPic_t)(void* scrPlace, float x, float y, float depth, float w, float h, int horzAlign,
                                        int vertAlign, float angle, const float* color, void* material);
typedef void*(__cdecl* MaterialRegisterHandle_t)(const char* name, int imageTrack, int errorIfMissing, int waitTime);
typedef void*(__cdecl* GetWeaponDef_t)(uint32_t weapon);
typedef bool(__cdecl* DvarGetBool_t)(void* dvar);
const ScrPlaceGetView_t ScrPlace_GetView = (ScrPlaceGetView_t)0x6F6AF0;
const DrawStretchPic_t CL_DrawStretchPic = (DrawStretchPic_t)0x4D5880;
const DrawRotatedPic_t CG_DrawRotatedPic = (DrawRotatedPic_t)0x440DE0;
const MaterialRegisterHandle_t Material_RegisterHandle = (MaterialRegisterHandle_t)0x734CC0;
const GetWeaponDef_t BG_GetWeaponDef = (GetWeaponDef_t)0x5225F0;
const DvarGetBool_t Dvar_GetBool = (DvarGetBool_t)0x6226F0;
const uintptr_t kDvarDrawCrosshair = 0x10AA244;  // dvar_t** cg_drawCrosshair
const uintptr_t kDrawSortKey = 0x341D404;  // the game adds 0x400 to [ptr+0x18] around HUD draws
const uintptr_t kWeapDefClass = 0x20;  // weapClass_t; 6 = rocket launcher (same in the Wii U build)
const uintptr_t kVerifyPush = 0x6D36C8;  // push "m_yaw": identifies this t6mp build

const float kRadToDeg = 57.2957795f;

// ---------------------------------------------------------------------------------------
// Settings and log

// Wii U names in brackets. Deadzones, curves and lookspring are the Wii U defaults; turn speeds
// keep the Wii U ratios (yaw 120 : pitch 29.5 : ADS 63.25 / 29.15) but are 3x faster, closer to
// a PC/controller turn at high sensitivity.
struct Settings {
    bool enabled = true;
    // Hip fire: false = PC look (aim follows input 1:1, the camera trails it so the gun swings
    // into turns), true = the Wii U pointer model below, as in ADS.
    bool hip_wiiu = false;
    float hip_follow_time = 0.4f;  // seconds for the camera to catch up with the aim (PC look)
    float hip_max_yaw = 25.0f;  // furthest the aim can get ahead of the camera, degrees
    float hip_max_pitch = 15.0f;
    float pointer_speed = 0.6f;  // pointer speed relative to the game's sensitivity
    float pointer_smoothing = 0.06f;  // seconds; like the Wii U IR filter (dpd_*), 0 = off
    float pointer_limit = 0.95f;  // how close to the screen edge the pointer can go (1 = edge)
    float deadzone_x = 0.40f;  // [wiiu_aim_deadzone_width]
    float deadzone_y = 0.38f;  // [wiiu_aim_deadzone_height]
    float deadzone_x_ads = 0.40f;  // [wiiu_aim_deadzone_width_ads]
    float deadzone_y_ads = 0.40f;  // [wiiu_aim_deadzone_height_ads]
    float turn_yaw = 360.0f;  // deg/s at the edge [wiiu_aim_turnrate_yaw 120]
    float turn_pitch = 90.0f;  // deg/s [wiiu_aim_turnrate_pitch 29.5]
    float turn_yaw_ads = 190.0f;  // [wiiu_aim_turnrate_yaw_ads 63.25]
    float turn_pitch_ads = 90.0f;  // [wiiu_aim_turnrate_pitch_ads 29.15]
    float turn_curve = 0.5f;  // exponent on the outer-zone fraction (2.5 / aim_turnrate_ramp_factor 5)
    float turn_curve_ads = 1.0f;  // (5.0 / aim_turnrate_ramp_factor 5)
    float turn_curve_y = 1.5f;  // vertical exponent multiplier
    float turn_accel = 1800.0f;  // max change of turn rate, deg/s^2 [wiiu_aim_accel_turnrate_lerp 600]
    bool lookspring = true;  // hip fire only [wiiu_aim_lookspring_enabled, _ads = 0]
    float lookspring_speed = 0.5f;  // leveling speed relative to turn_pitch [wiiu_aim_lookspring_speed]
    float pitch_max = 70.0f;  // pitch at the top/bottom edge [player_view_pitch_up/down]
    float ads_recenter_speed = 0.0f;  // >0: ADS slides the pointer back to the center (1/s)
    // Reticles, drawn like the Wii U's: red dot, ring for rocket launchers, X at the screen edge.
    bool reticle = true;  // false = the game's normal crosshair
    bool reticle_in_ads = false;  // also draw the dot/ring while aiming down sights
    bool edge_x = true;  // X while the pointer is pushed against the screen edge
    float dot_size = 4.0f;  // 640x480 units
    float ring_radius = 20.0f;
    float x_size = 30.0f;
    // Gun: how much it turns toward the crosshair (1 = points straight at it) and how far it
    // rolls with the crosshair to the side. [wiiu_viewmodelTrackFactorX/Y, ik_wiiu_roll_limit]
    float gun_follow_x = 1.0f;  // the Wii U used 0.5
    float gun_follow_y = 1.0f;  // the Wii U used 0.1
    float gun_follow_ads = 1.0f;  // the Wii U used 0.75; below 1 the sniper scope drifts off the aim
    float gun_roll = 15.0f;  // degrees at the screen edge (Wii U limit 30); negative flips it
    float reset_jump = 45.0f;  // a view jump bigger than this (respawn, teleport) recenters
};

Settings g_cfg;
char g_home[MAX_PATH];
FILE* g_log = nullptr;

void plog(const char* fmt, ...) {
    if (!g_log) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "%02d:%02d:%02d ", t.wHour, t.wMinute, t.wSecond);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

void open_home() {
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&open_home, &self);
    char path[MAX_PATH] = {0};
    GetModuleFileNameA(self, path, MAX_PATH);
    for (int i = 0; i < 2; i++) {  // ...\Plutonium\plugins\wiiuaim.dll -> ...\Plutonium
        char* slash = strrchr(path, '\\');
        if (slash) *slash = 0;
    }
    snprintf(g_home, sizeof(g_home), "%s\\wiiuaim\\", path);
    CreateDirectoryA(g_home, nullptr);
    char logPath[MAX_PATH];
    snprintf(logPath, sizeof(logPath), "%swiiuaim.log", g_home);
    g_log = fopen(logPath, "w");
}

float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

float ini_float(const char* ini, const char* key, float def) {
    char buf[64], defs[64];
    snprintf(defs, sizeof(defs), "%g", def);
    GetPrivateProfileStringA("aim", key, defs, buf, sizeof(buf), ini);
    return (float)atof(buf);
}

// Bumped when the meaning of a setting or its default changes; an older ini is rewritten.
const int kIniVersion = 9;

// Reads wiiuaim.ini, writing one with the defaults if there is none or it is older.
void load_settings() {
    char ini[MAX_PATH];
    snprintf(ini, sizeof(ini), "%swiiuaim.ini", g_home);
    if ((int)ini_float(ini, "version", 0) < kIniVersion) {
        FILE* f = fopen(ini, "w");
        if (f) {
            Settings d;
            fprintf(f,
                    "; WiiU Aim settings. Restart the game after editing.\n"
                    "; Deadzones, curves and lookspring are the Wii U version's own values.\n"
                    "[aim]\n"
                    "version=%d\n"
                    "; 1 = on, 0 = plugin does nothing\n"
                    "enabled=1\n"
                    "\n"
                    "; --- Hip fire ---\n"
                    "; 0 = PC look: aim moves normally, the camera trails it so the gun swings into turns\n"
                    "; 1 = Wii U pointer, like ADS below\n"
                    "hip_wiiu=%d\n"
                    "; seconds for the camera to catch up with the aim (PC look); higher = more gun swing\n"
                    "hip_follow_time=%g\n"
                    "; furthest the aim can get ahead of the camera, degrees (PC look)\n"
                    "hip_max_yaw=%g\nhip_max_pitch=%g\n"
                    "\n"
                    "; --- Wii U pointer (ADS, and hip fire with hip_wiiu=1) ---\n"
                    "; pointer speed: 1 = same as your in-game sensitivity, 0.5 = half\n"
                    "pointer_speed=%g\n"
                    "; pointer smoothing in seconds, steadies the aim like the Wii Remote filter (0 = off)\n"
                    "pointer_smoothing=%g\n"
                    "; how close to the screen edge the pointer can go (1 = the edge)\n"
                    "pointer_limit=%g\n"
                    "; no-turn zone around the center, as a fraction of the half screen (hip / ADS)\n"
                    "deadzone_x=%g\ndeadzone_y=%g\n"
                    "deadzone_x_ads=%g\ndeadzone_y_ads=%g\n"
                    "; turn speed with the pointer at the edge, degrees/second (hip / ADS)\n"
                    "; the Wii U used 120 / 29.5 and 63.25 / 29.15\n"
                    "turn_yaw=%g\nturn_pitch=%g\n"
                    "turn_yaw_ads=%g\nturn_pitch_ads=%g\n"
                    "; turn curve past the deadzone: 0.5 = ramps up fast, 1 = linear, 2 = slow start\n"
                    "turn_curve=%g\nturn_curve_ads=%g\n"
                    "; vertical curve = turn curve times this\n"
                    "turn_curve_y=%g\n"
                    "; how fast the turn speed can change, degrees/second^2 (0 = instant)\n"
                    "turn_accel=%g\n"
                    "; 1 = in hip fire, pointing up/down tilts the camera and it levels out in the middle (Wii U)\n"
                    "; 0 = up/down turns like left/right\n"
                    "lookspring=%d\n"
                    "lookspring_speed=%g\n"
                    "; camera pitch with the pointer at the top/bottom edge (lookspring)\n"
                    "pitch_max=%g\n"
                    "; >0: aiming down sights slides the pointer back to the center at this speed\n"
                    "ads_recenter_speed=%g\n"
                    "\n"
                    "; --- Reticle ---\n"
                    "; 1 = Wii U style: red dot, orange ring for rocket launchers; 0 = the game's normal crosshair\n"
                    "reticle=%d\n"
                    "; 1 = also show the dot/ring while aiming down sights\n"
                    "reticle_in_ads=%d\n"
                    "; 1 = show an X while the pointer is pushed against the screen edge\n"
                    "edge_x=%d\n"
                    "; sizes in 640x480 screen units\n"
                    "dot_size=%g\nring_radius=%g\nx_size=%g\n"
                    "\n"
                    "; --- Gun ---\n"
                    "; how much the gun turns toward the crosshair: 1 = points straight at it (Wii U: 0.5 / 0.1)\n"
                    "gun_follow_x=%g\ngun_follow_y=%g\n"
                    "; while aiming down sights; keep 1 so sniper scopes stay on the aim (Wii U: 0.75)\n"
                    "gun_follow_ads=%g\n"
                    "; how far the gun rolls with the crosshair at the screen edge, degrees (negative = other way)\n"
                    "gun_roll=%g\n",
                    kIniVersion, d.hip_wiiu ? 1 : 0, d.hip_follow_time, d.hip_max_yaw, d.hip_max_pitch,
                    d.pointer_speed, d.pointer_smoothing, d.pointer_limit, d.deadzone_x, d.deadzone_y,
                    d.deadzone_x_ads, d.deadzone_y_ads, d.turn_yaw, d.turn_pitch, d.turn_yaw_ads, d.turn_pitch_ads,
                    d.turn_curve, d.turn_curve_ads, d.turn_curve_y, d.turn_accel, d.lookspring ? 1 : 0,
                    d.lookspring_speed, d.pitch_max, d.ads_recenter_speed, d.reticle ? 1 : 0,
                    d.reticle_in_ads ? 1 : 0, d.edge_x ? 1 : 0, d.dot_size, d.ring_radius, d.x_size,
                    d.gun_follow_x, d.gun_follow_y, d.gun_follow_ads, d.gun_roll);
            fclose(f);
            plog("wrote default settings (v%d)", kIniVersion);
        }
    }
    g_cfg.enabled = ini_float(ini, "enabled", 1) != 0;
    g_cfg.hip_wiiu = ini_float(ini, "hip_wiiu", 0) != 0;
    g_cfg.hip_follow_time = clampf(ini_float(ini, "hip_follow_time", g_cfg.hip_follow_time), 0.0f, 2.0f);
    g_cfg.hip_max_yaw = clampf(ini_float(ini, "hip_max_yaw", g_cfg.hip_max_yaw), 0.0f, 40.0f);
    g_cfg.hip_max_pitch = clampf(ini_float(ini, "hip_max_pitch", g_cfg.hip_max_pitch), 0.0f, 30.0f);
    g_cfg.pointer_speed = ini_float(ini, "pointer_speed", g_cfg.pointer_speed);
    g_cfg.pointer_smoothing = clampf(ini_float(ini, "pointer_smoothing", g_cfg.pointer_smoothing), 0.0f, 1.0f);
    g_cfg.pointer_limit = clampf(ini_float(ini, "pointer_limit", g_cfg.pointer_limit), 0.1f, 1.0f);
    g_cfg.deadzone_x = clampf(ini_float(ini, "deadzone_x", g_cfg.deadzone_x), 0.0f, 0.95f);
    g_cfg.deadzone_y = clampf(ini_float(ini, "deadzone_y", g_cfg.deadzone_y), 0.0f, 0.95f);
    g_cfg.deadzone_x_ads = clampf(ini_float(ini, "deadzone_x_ads", g_cfg.deadzone_x_ads), 0.0f, 0.95f);
    g_cfg.deadzone_y_ads = clampf(ini_float(ini, "deadzone_y_ads", g_cfg.deadzone_y_ads), 0.0f, 0.95f);
    g_cfg.turn_yaw = ini_float(ini, "turn_yaw", g_cfg.turn_yaw);
    g_cfg.turn_pitch = ini_float(ini, "turn_pitch", g_cfg.turn_pitch);
    g_cfg.turn_yaw_ads = ini_float(ini, "turn_yaw_ads", g_cfg.turn_yaw_ads);
    g_cfg.turn_pitch_ads = ini_float(ini, "turn_pitch_ads", g_cfg.turn_pitch_ads);
    g_cfg.turn_curve = clampf(ini_float(ini, "turn_curve", g_cfg.turn_curve), 0.05f, 10.0f);
    g_cfg.turn_curve_ads = clampf(ini_float(ini, "turn_curve_ads", g_cfg.turn_curve_ads), 0.05f, 10.0f);
    g_cfg.turn_curve_y = clampf(ini_float(ini, "turn_curve_y", g_cfg.turn_curve_y), 0.05f, 10.0f);
    g_cfg.turn_accel = ini_float(ini, "turn_accel", g_cfg.turn_accel);
    g_cfg.lookspring = ini_float(ini, "lookspring", 1) != 0;
    g_cfg.lookspring_speed = ini_float(ini, "lookspring_speed", g_cfg.lookspring_speed);
    g_cfg.pitch_max = clampf(ini_float(ini, "pitch_max", g_cfg.pitch_max), 0.0f, 89.0f);
    g_cfg.ads_recenter_speed = ini_float(ini, "ads_recenter_speed", g_cfg.ads_recenter_speed);
    g_cfg.reticle = ini_float(ini, "reticle", 1) != 0;
    g_cfg.reticle_in_ads = ini_float(ini, "reticle_in_ads", 0) != 0;
    g_cfg.edge_x = ini_float(ini, "edge_x", 1) != 0;
    g_cfg.dot_size = clampf(ini_float(ini, "dot_size", g_cfg.dot_size), 1.0f, 40.0f);
    g_cfg.ring_radius = clampf(ini_float(ini, "ring_radius", g_cfg.ring_radius), 4.0f, 120.0f);
    g_cfg.x_size = clampf(ini_float(ini, "x_size", g_cfg.x_size), 4.0f, 200.0f);
    g_cfg.gun_follow_x = clampf(ini_float(ini, "gun_follow_x", g_cfg.gun_follow_x), 0.0f, 1.0f);
    g_cfg.gun_follow_y = clampf(ini_float(ini, "gun_follow_y", g_cfg.gun_follow_y), 0.0f, 1.0f);
    g_cfg.gun_follow_ads = clampf(ini_float(ini, "gun_follow_ads", g_cfg.gun_follow_ads), 0.0f, 1.0f);
    g_cfg.gun_roll = clampf(ini_float(ini, "gun_roll", g_cfg.gun_roll), -45.0f, 45.0f);
    plog("gun: follow %.2f/%.2f (ads %.2f) roll %.1f", g_cfg.gun_follow_x, g_cfg.gun_follow_y, g_cfg.gun_follow_ads,
         g_cfg.gun_roll);
    plog("reticle: %s in_ads=%d edge_x=%d dot=%.1f ring=%.1f x=%.1f", g_cfg.reticle ? "wiiu" : "game",
         g_cfg.reticle_in_ads, g_cfg.edge_x, g_cfg.dot_size, g_cfg.ring_radius, g_cfg.x_size);
    plog("hip: %s follow=%.2fs max=%.1f/%.1f", g_cfg.hip_wiiu ? "wiiu" : "pc", g_cfg.hip_follow_time,
         g_cfg.hip_max_yaw, g_cfg.hip_max_pitch);
    plog("settings: enabled=%d pointer=%.2f smooth=%.3f limit=%.2f deadzone=%.2fx%.2f (ads %.2fx%.2f) "
         "turn=%.1f/%.1f (ads %.1f/%.1f) curve=%.2f (ads %.2f, y x%.2f) accel=%.0f lookspring=%d (%.2f, max %.0f) "
         "ads_recenter=%.1f",
         g_cfg.enabled, g_cfg.pointer_speed, g_cfg.pointer_smoothing, g_cfg.pointer_limit, g_cfg.deadzone_x,
         g_cfg.deadzone_y, g_cfg.deadzone_x_ads, g_cfg.deadzone_y_ads, g_cfg.turn_yaw, g_cfg.turn_pitch,
         g_cfg.turn_yaw_ads, g_cfg.turn_pitch_ads, g_cfg.turn_curve, g_cfg.turn_curve_ads, g_cfg.turn_curve_y,
         g_cfg.turn_accel, g_cfg.lookspring, g_cfg.lookspring_speed, g_cfg.pitch_max, g_cfg.ads_recenter_speed);
}

// ---------------------------------------------------------------------------------------
// Aim state

struct AimState {
    bool valid = false;  // prev angles are from the last frame of the same client
    uintptr_t cl = 0;
    float prev_pitch = 0, prev_yaw = 0;  // cl->viewangles as we left them last frame
    float rx = 0, ry = 0;  // pointer as moved by input, [-1, 1]; +x right, +y up
    float px = 0, py = 0;  // smoothed pointer: where the aim and crosshair are
    float off_pitch = 0, off_yaw = 0;  // aim minus camera, degrees (+pitch = down, +yaw = left)
    float yaw_rate = 0, pitch_rate = 0;  // current camera turn rates, deg/s (game sign convention)
    float edge_push = 0;  // seconds left to show the edge X; set while input pushes past the edge
    bool ads = false;
    LARGE_INTEGER last_time = {};
};

bool g_aim_active = false;  // free aim controlled this frame's camera; the reticle hook draws only then

AimState g_aim;
LARGE_INTEGER g_freq;

float angle_delta(float a, float b) {
    float d = fmodf(a - b, 360.0f);
    if (d > 180.0f) d -= 360.0f;
    if (d < -180.0f) d += 360.0f;
    return d;
}

// Past the deadzone: how far into the outer zone the pointer is, shaped by the turn curve,
// signed like the pointer. 0 inside the deadzone, +-1 at the screen edge.
float outer_zone(float p, float deadzone, float curve) {
    float a = fabsf(p);
    if (a <= deadzone) return 0.0f;
    float t = clampf((a - deadzone) / (1.0f - deadzone), 0.0f, 1.0f);
    t = powf(t, curve);
    return p > 0 ? t : -t;
}

// PC-look lead of the aim over the camera: the input adds to it, then it decays toward 0.
// It can grow only up to max, but a bigger lead (e.g. leaving ADS) just decays.
float trail(float lead, float input, float decay, float max) {
    float next = (lead + input) * decay;
    float cap = fmaxf(max, fabsf(lead) * decay);
    return clampf(next, -cap, cap);
}

// Moves rate toward target by at most accel*dt (accel 0 = jump straight there).
float approach(float rate, float target, float accel, float dt) {
    if (accel <= 0.0f) return target;
    float step = accel * dt;
    return rate + clampf(target - rate, -step, step);
}

// Turns cg.refdef.viewaxis from the aim direction to the camera direction: the camera is the
// aim rotated back by the crosshair offset. With M = AnglesToAxis(offset) (the aim direction
// seen from the camera), aim_k = sum_i M[k][i] cam_i, so cam_i = sum_k M[k][i] aim_k.
void rotate_axis(float* axis, float pitch, float yaw) {
    const float kDegToRad = 3.14159265f / 180.0f;
    float sp = sinf(pitch * kDegToRad), cp = cosf(pitch * kDegToRad);
    float sy = sinf(yaw * kDegToRad), cy = cosf(yaw * kDegToRad);
    // Quake AnglesToAxis with roll 0: forward, left, up.
    const float m[3][3] = {{cp * cy, cp * sy, -sp}, {-sy, cy, 0.0f}, {sp * cy, sp * sy, cp}};
    float aim[9];
    memcpy(aim, axis, sizeof(aim));
    for (int i = 0; i < 3; i++)
        for (int c = 0; c < 3; c++)
            axis[i * 3 + c] = m[0][i] * aim[c] + m[1][i] * aim[3 + c] + m[2][i] * aim[6 + c];
}

// Why the free aim is off this frame, or nullptr when the player is in normal control of their
// own soldier. Follows the Wii U's own checks (WiiU_ShouldForceLookspringOff,
// WiiU_IsCameraLocked, WiiU_UsingRemoteControlledProjectile) plus killcam/spectating.
const char* inactive_reason(uintptr_t cg) {
    uintptr_t ps = cg + kCgPs;
    int pm_type = *(int*)(ps + kPsPmType);
    if (pm_type != 0 && pm_type != 7) return "not in normal control (dead, spectator, intermission)";  // 7 = last stand
    if (*(int*)(ps + kPsClientNum) != *(int*)(cg + kCgClientNum)) return "watching another player (killcam, spectating)";
    if (*(uint32_t*)(ps + kPsPmFlags) & 0x100800) return "movement locked";
    if (*(uint32_t*)(ps + kPsEFlags) & 0x4300) return "turret or vehicle";
    if (*(uint32_t*)(ps + kPsLinkFlags) & 4) return "remote-controlled killstreak";
    if (*(uint32_t*)(ps + kPsOtherFlags) & 2) return "camera locked";
    if (*(int*)(ps + kPsViewLocked) != 0) return "view locked";
    return nullptr;
}

void aim_frame() {
    g_aim_active = false;
    if (!g_cfg.enabled) return;
    int count = *(int*)kClientCount;
    uintptr_t cl = *(uintptr_t*)kClientActive;
    uintptr_t cg = *(uintptr_t*)kCg;
    if (count < 1 || !cl || !cg) {
        g_aim.valid = false;
        return;
    }
    // Outside normal control the game's camera is left alone; free aim restarts centered.
    static const char* last_reason = "";
    const char* reason = inactive_reason(cg);
    if (reason != last_reason) {
        uintptr_t ps = cg + kCgPs;
        plog("free aim %s%s (pm_type %d, client %d/%d, pm_flags %x, eFlags %x, linkFlags %x)", reason ? "off: " : "on",
             reason ? reason : "", *(int*)(ps + kPsPmType), *(int*)(ps + kPsClientNum), *(int*)(cg + kCgClientNum),
             *(uint32_t*)(ps + kPsPmFlags), *(uint32_t*)(ps + kPsEFlags), *(uint32_t*)(ps + kPsLinkFlags));
        last_reason = reason;
    }
    if (reason) {
        g_aim.valid = false;
        return;
    }
    float* va = (float*)(cl + kClViewAngles);
    float* axis = (float*)(cg + kCgViewAxis);
    const float* tan_half = (const float*)(cg + kCgTanHalfFov);
    if (tan_half[0] <= 0.0f || tan_half[1] <= 0.0f) return;

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    float dt = g_aim.valid ? (float)(now.QuadPart - g_aim.last_time.QuadPart) / (float)g_freq.QuadPart : 0.0f;
    dt = clampf(dt, 0.0f, 0.1f);
    g_aim.last_time = now;

    float dp = angle_delta(va[0], g_aim.prev_pitch), dy = angle_delta(va[1], g_aim.prev_yaw);
    if (!g_aim.valid || cl != g_aim.cl || fabsf(dp) > g_cfg.reset_jump || fabsf(dy) > g_cfg.reset_jump) {
        // New client, map, respawn or teleport: pointer back to the center.
        if (g_aim.valid) plog("recenter (cl %08x, jump %.1f/%.1f)", (unsigned)cl, dp, dy);
        g_aim = AimState{};
        g_aim.valid = true;
        g_aim.cl = cl;
        g_aim.last_time = now;
        dp = dy = 0;
    }

    float half_fov_x = atanf(tan_half[0]) * kRadToDeg;
    float half_fov_y = atanf(tan_half[1]) * kRadToDeg;
    // The aim (pre-rotation refdef forward) and the camera's absolute pitch, for lookspring.
    float aim_pitch = -asinf(clampf(axis[2], -1.0f, 1.0f)) * kRadToDeg;
    float cam_pitch = aim_pitch - g_aim.off_pitch;

    float turn_p = 0, turn_y = 0;
    bool ads = clampf(*(float*)(cg + kCgAdsFrac), 0.0f, 1.0f) > 0.5f;
    float old_p = g_aim.off_pitch, old_y = g_aim.off_yaw;
    g_aim.ads = ads;
    g_aim.edge_push = fmaxf(0.0f, g_aim.edge_push - dt);
    g_aim_active = true;

    if (!ads && !g_cfg.hip_wiiu) {
        // PC look: the aim (crosshair, gun, bullets) moves exactly with the input; the camera
        // trails it and catches up, so the gun swings out into turns and settles back.
        float decay = g_cfg.hip_follow_time > 0.0f ? expf(-dt / g_cfg.hip_follow_time) : 0.0f;
        g_aim.off_yaw = trail(old_y, dy, decay, g_cfg.hip_max_yaw);
        g_aim.off_pitch = trail(old_p, dp, decay, g_cfg.hip_max_pitch);
        // The camera turns by whatever the aim moved minus the change in lead.
        turn_y = dy - (g_aim.off_yaw - old_y);
        turn_p = dp - (g_aim.off_pitch - old_p);
        // Keep the pointer where the aim is on screen, so ADS starts from there.
        g_aim.rx = g_aim.px = -tanf(g_aim.off_yaw / kRadToDeg) / tan_half[0];
        g_aim.ry = g_aim.py = -tanf(g_aim.off_pitch / kRadToDeg) / tan_half[1];
        g_aim.yaw_rate = g_aim.pitch_rate = 0;
        va[0] = g_aim.prev_pitch + turn_p + (g_aim.off_pitch - old_p);
        va[1] = g_aim.prev_yaw + turn_y + (g_aim.off_yaw - old_y);
        g_aim.prev_pitch = va[0];
        g_aim.prev_yaw = va[1];
        rotate_axis(axis, g_aim.off_pitch, g_aim.off_yaw);
        return;
    }

    // Wii U pointer. Input only moves the pointer; it stops at the screen edge (the rest is dropped).
    // +yaw is left and +pitch is down, the pointer's +x is right and +y is up.
    float lim = g_cfg.pointer_limit;
    float want_x = g_aim.rx - dy / half_fov_x * g_cfg.pointer_speed;
    float want_y = g_aim.ry - dp / half_fov_y * g_cfg.pointer_speed;
    // Input pushing the pointer past the edge: on Wii U that's the remote pointing off the
    // screen, shown with an X.
    if (fabsf(want_x) > lim + 1e-3f || fabsf(want_y) > lim + 1e-3f) g_aim.edge_push = 0.25f;
    g_aim.rx = clampf(want_x, -lim, lim);
    g_aim.ry = clampf(want_y, -lim, lim);
    if (ads && g_cfg.ads_recenter_speed > 0.0f) {
        float k = clampf(g_cfg.ads_recenter_speed * dt, 0.0f, 1.0f);
        g_aim.rx -= g_aim.rx * k;
        g_aim.ry -= g_aim.ry * k;
    }
    // Smoothing, like the Wii U's filter on the IR pointer: the aim follows the input pointer
    // with a short lag, which takes out jitter.
    float k = g_cfg.pointer_smoothing > 0.0f ? 1.0f - expf(-dt / g_cfg.pointer_smoothing) : 1.0f;
    g_aim.px += (g_aim.rx - g_aim.px) * k;
    g_aim.py += (g_aim.ry - g_aim.py) * k;

    // Yaw: past the deadzone the camera turns, faster further out.
    float curve = ads ? g_cfg.turn_curve_ads : g_cfg.turn_curve;
    float zx = outer_zone(g_aim.px, ads ? g_cfg.deadzone_x_ads : g_cfg.deadzone_x, curve);
    float zy = outer_zone(g_aim.py, ads ? g_cfg.deadzone_y_ads : g_cfg.deadzone_y, curve * g_cfg.turn_curve_y);
    g_aim.yaw_rate = approach(g_aim.yaw_rate, -zx * (ads ? g_cfg.turn_yaw_ads : g_cfg.turn_yaw), g_cfg.turn_accel, dt);
    turn_y = g_aim.yaw_rate * dt;

    if (g_cfg.lookspring && !ads) {
        // Lookspring (hip fire only, as on Wii U): the pointer's height sets where the camera
        // pitch settles; inside the deadzone that is level, reached at the lookspring speed.
        float target = -zy * g_cfg.pitch_max;
        float speed = g_cfg.turn_pitch * (zy != 0.0f ? 1.0f : g_cfg.lookspring_speed);
        turn_p = clampf(target - cam_pitch, -speed * dt, speed * dt);
        g_aim.pitch_rate = 0;
    } else {
        float rate = ads ? g_cfg.turn_pitch_ads : g_cfg.turn_pitch;
        g_aim.pitch_rate = approach(g_aim.pitch_rate, -zy * rate, g_cfg.turn_accel, dt);
        turn_p = g_aim.pitch_rate * dt;
    }

    // The aim goes through the pointer's screen position.
    g_aim.off_yaw = -atanf(g_aim.px * tan_half[0]) * kRadToDeg;
    g_aim.off_pitch = -atanf(g_aim.py * tan_half[1]) * kRadToDeg;

    // aim = camera + offset: rewrite the view angles from the camera's turn and the new offset,
    // replacing what the raw input did.
    va[0] = g_aim.prev_pitch + turn_p + (g_aim.off_pitch - old_p);
    va[1] = g_aim.prev_yaw + turn_y + (g_aim.off_yaw - old_y);
    g_aim.prev_pitch = va[0];
    g_aim.prev_yaw = va[1];

    rotate_axis(axis, g_aim.off_pitch, g_aim.off_yaw);
}

// ---------------------------------------------------------------------------------------
// Hooks

// ---------------------------------------------------------------------------------------
// Reticles (recreated from simple shapes; no Wii U assets)

void* g_white = nullptr;

void rect(void* place, float cx, float cy, float w, float h, const float* color) {
    CL_DrawStretchPic(place, cx - w * 0.5f, cy - h * 0.5f, 1.0f, w, h, 2, 2, 0.0f, 0.0f, 1.0f, 1.0f, color, g_white);
}

// A bar of length len centered on (cx, cy), rotated by angle degrees.
void bar(void* place, float cx, float cy, float len, float thick, float angle, const float* color) {
    CG_DrawRotatedPic(place, cx - len * 0.5f, cy - thick * 0.5f, 1.0f, len, thick, 2, 2, angle, color, g_white);
}

void draw_dot(void* place, float x, float y) {
    static const float kBorder[4] = {0.05f, 0.0f, 0.0f, 0.85f};
    static const float kRed[4] = {1.0f, 0.12f, 0.08f, 1.0f};
    float s = g_cfg.dot_size;
    rect(place, x, y, s + 2.0f, s + 2.0f, kBorder);
    rect(place, x, y, s, s, kRed);
}

// Rocket launchers: a ring with four ticks and a center dot.
void draw_ring(void* place, float x, float y) {
    static const float kOrange[4] = {1.0f, 0.42f, 0.12f, 0.95f};
    const int kSegments = 36;
    float r = g_cfg.ring_radius, thick = fmaxf(1.0f, r * 0.08f);
    float seg = 2.0f * 3.14159265f * r / kSegments * 1.2f;
    for (int i = 0; i < kSegments; i++) {
        float a = 360.0f * i / kSegments;
        float rad = a / kRadToDeg;
        bar(place, x + r * cosf(rad), y + r * sinf(rad), seg, thick, a + 90.0f, kOrange);
    }
    for (int i = 0; i < 4; i++) {
        float a = 90.0f * i, rad = a / kRadToDeg;
        bar(place, x + (r + 3.0f) * cosf(rad), y + (r + 3.0f) * sinf(rad), 6.0f, thick, a, kOrange);
    }
    rect(place, x, y, thick * 1.6f, thick * 1.6f, kOrange);
}

// Pointer pushed off the screen: a pulsing X, like the Wii U's invalid-pointer reticle (500 ms).
void draw_x(void* place, float x, float y) {
    float pulse = 0.5f + 0.5f * sinf((float)(GetTickCount() % 500) / 500.0f * 2.0f * 3.14159265f);
    const float color[4] = {1.0f, 0.45f, 0.35f, 0.55f + 0.35f * pulse};
    float len = g_cfg.x_size, thick = fmaxf(1.5f, len * 0.05f);
    bar(place, x, y, len, thick, 45.0f, color);
    bar(place, x, y, len, thick, -45.0f, color);
}

bool is_rocket_launcher(uintptr_t cg) {
    uint32_t weapon = *(uint32_t*)(cg + kCgPs + kPsWeapon);
    if (!(weapon & 0xFF)) return false;
    uintptr_t def = (uintptr_t)BG_GetWeaponDef(weapon);
    return def && *(int*)(def + kWeapDefClass) == 6;
}

typedef bool(__cdecl* DrawCrosshairFn)(int localClientNum);

// Replaces CG_DrawCrosshair while free aim is active. The game's own version still runs in
// ADS (scope overlays) and whenever free aim is off (killcams, killstreaks, dead).
bool __cdecl crosshair_hook(int localClientNum) {
    uintptr_t cg = *(uintptr_t*)kCg;
    if (!g_cfg.reticle || localClientNum != 0 || !g_aim_active || !cg)
        return ((DrawCrosshairFn)kCrosshairTarget)(localClientNum);
    if (!g_white) g_white = Material_RegisterHandle("white", 7, 1, -1);

    bool drew = false;
    if (g_aim.ads) drew = ((DrawCrosshairFn)kCrosshairTarget)(localClientNum);
    if (!g_white) return drew;
    void* dvar = *(void**)kDvarDrawCrosshair;
    if (dvar && !Dvar_GetBool(dvar)) return drew;

    // Same spot the aim goes through: the pointer, in 640x480 units from the screen center.
    float x = g_aim.px * 320.0f, y = -g_aim.py * 240.0f;
    void* place = ScrPlace_GetView(localClientNum);
    *(int*)(*(uintptr_t*)kDrawSortKey + 0x18) += 0x400;
    if (g_cfg.edge_x && g_aim.edge_push > 0.0f)
        draw_x(place, x, y);
    else if (!g_aim.ads || g_cfg.reticle_in_ads)
        is_rocket_launcher(cg) ? draw_ring(place, x, y) : draw_dot(place, x, y);
    *(int*)(*(uintptr_t*)kDrawSortKey + 0x18) -= 0x400;
    return true;
}

// Gun angles relative to the view, before the game turns them into the gun's rotation (the
// spot where the Wii U applies WiiU_CalculateViewmodelPosition). The view here is the aim; the
// camera is drawn at aim - offset afterwards. Pulling the gun back by (1 - follow) * offset
// makes it point that fraction of the way from the camera to the crosshair; the roll follows
// the crosshair sideways, like the Wii U's hand IK. Uses the offset from the last aim frame.
void gun_adjust(float* angles, uintptr_t cg) {
    if (!g_aim_active || !cg) return;
    float ads = clampf(*(float*)(cg + kCgAdsFrac), 0.0f, 1.0f);
    float follow_x = g_cfg.gun_follow_x + (g_cfg.gun_follow_ads - g_cfg.gun_follow_x) * ads;
    float follow_y = g_cfg.gun_follow_y + (g_cfg.gun_follow_ads - g_cfg.gun_follow_y) * ads;
    angles[0] -= (1.0f - follow_y) * g_aim.off_pitch;
    angles[1] -= (1.0f - follow_x) * g_aim.off_yaw;
    angles[2] += clampf(g_aim.px, -1.0f, 1.0f) * g_cfg.gun_roll * (1.0f - ads);
}

typedef int(__cdecl* FrameFn)(int localClientNum);

int __cdecl frame_hook(int localClientNum) {
    int r = ((FrameFn)kFrameTarget)(localClientNum);
    if (localClientNum == 0) aim_frame();
    return r;
}

bool patch(uintptr_t addr, const void* bytes, size_t n) {
    DWORD old;
    if (!VirtualProtect((void*)addr, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy((void*)addr, bytes, n);
    VirtualProtect((void*)addr, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)addr, n);
    return true;
}

template <typename T>
bool peek(uintptr_t addr, T* out) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), (LPCVOID)addr, out, sizeof(T), &got) && got == sizeof(T);
}

bool is_t6mp() {
    uint8_t code[5];
    static const uint8_t kPushMYaw[5] = {0x68, 0xD0, 0xFA, 0xBE, 0x00};
    return peek(kVerifyPush, &code) && !memcmp(code, kPushMYaw, 5);
}

// True if the 5 bytes at site are `call target`.
bool is_call_to(uintptr_t site, uintptr_t target) {
    uint8_t call[5];
    if (!peek(site, &call) || call[0] != 0xE8) return false;
    int32_t rel = 0;
    memcpy(&rel, call + 1, 4);
    return site + 5 + rel == target;
}

bool redirect_call(uintptr_t site, const void* hook) {
    uint8_t call[5] = {0xE8};
    int32_t rel = (int32_t)((uintptr_t)hook - (site + 5));
    memcpy(call + 1, &rel, 4);
    return patch(site, call, 5);
}

// Patches only if every byte we touch is what this build is known to have.
void install() {
    uint8_t jnz[2];
    if (!is_call_to(kFrameCall, kFrameTarget) || !is_call_to(kCrosshairCall, kCrosshairTarget) ||
        !is_call_to(kGunAnglesCall, kGunAnglesTarget) || !peek(kCrosshairJnz, &jnz) || jnz[0] != 0x75 ||
        jnz[1] != 0x15) {
        plog("unexpected bytes at patch sites (different build?), nothing installed");
        return;
    }
    static const uint8_t jmp = 0xEB;
    if (redirect_call(kFrameCall, (const void*)&frame_hook) && patch(kCrosshairJnz, &jmp, 1) &&
        redirect_call(kCrosshairCall, (const void*)&crosshair_hook) &&
        redirect_call(kGunAnglesCall, (const void*)&wiiuaim_gun_hook))
        plog("hooks installed");
    else
        plog("VirtualProtect failed, hooks may be partly installed");
}

DWORD WINAPI start_thread(LPVOID) {
    // Plutonium maps the game into its bootstrapper; wait until the t6mp code is there.
    for (int i = 0; !is_t6mp(); i++) {
        if (i == 120) {
            plog("t6mp code not found after 60s (zombies, or a different build), idle");
            return 0;
        }
        Sleep(500);
    }
    load_settings();
    if (g_cfg.enabled) install();
    return 0;
}

// ---------------------------------------------------------------------------------------
// Plutonium plugin entry

struct Plugin {
    void* const* vtable;
};
void* THISCALL p_destroy(Plugin* self, unsigned) { return self; }
uint32_t THISCALL p_version(Plugin*) { return 1; }
const char* THISCALL p_name(Plugin*) { return "wiiuaim"; }
bool THISCALL p_supported(Plugin*, int game) { return game == 3; }  // t6
void THISCALL p_startup(Plugin*, void*, int game) {
    QueryPerformanceFrequency(&g_freq);
    open_home();
    plog("wiiuaim v1.3, game %d", game);
    CreateThread(nullptr, 0, start_thread, nullptr, 0, nullptr);
}
void THISCALL p_shutdown(Plugin*) {}
void* const g_vtable[] = {(void*)p_destroy, (void*)p_version, (void*)p_name,
                          (void*)p_supported, (void*)p_startup, (void*)p_shutdown};
Plugin g_plugin = {g_vtable};

}  // namespace

extern "C" void __cdecl wiiuaim_gun_adjust(float* angles, uintptr_t cg) { gun_adjust(angles, cg); }

// Replaces `call 0x7E1450` at 0x7E15D3. On entry: [esp] = return address, [esp+4] = gun angles,
// [esp+8] = placement, esi = cg. Calls wiiuaim_gun_adjust(angles, cg), then continues into the
// original with the stack and esi as they were.
asm(".text\n"
    ".globl _wiiuaim_gun_hook\n"
    "_wiiuaim_gun_hook:\n"
    "    pushl %esi\n"
    "    pushl %esi\n"
    "    pushl 12(%esp)\n"
    "    call _wiiuaim_gun_adjust\n"
    "    addl $8, %esp\n"
    "    popl %esi\n"
    "    pushl $0x7E1450\n"
    "    ret\n");

extern "C" __declspec(dllexport) void* __cdecl on_initialize() { return &g_plugin; }

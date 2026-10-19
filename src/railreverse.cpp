// railreverse - trains reverse (locomotive pushes) instead of flipping ends.
//
// The game reverses a train by moving its head to the old tail
// (FUN_1406bc9a0, "the flip") and drawing the locomotive there. Game logic
// is fine with that and is left alone. What this plugin changes is only how
// the train is drawn: after a flip the train is marked "pushing", and every
// car of a pushing train is put on the mirror image of its slot on the same
// stretch of track (metres behind the head d -> L - d). The locomotive stays
// where it was and the cars run ahead of it. The next flip clears the mark.
//
// Addresses, formulas and the reasoning: docs/findings-1a.md.

#include "../include/tesmio_plugin.h"
#include <math.h>

#define RR_VERSION "0.1.0"
#define RR_INI     "plugins\\railreverse.ini"

// ---------------------------------------------------------------- layout (1.1.1.9)
#define V_PARENT       0x398      // wagon: its lead; 0 on the lead itself
#define V_WAGONS_BEGIN 0x3A0      // lead: std::vector<vehicle*> of wagons, front to back
#define V_WAGONS_END   0x3A8
#define V_REVERSED     0x4C4      // u8: car coupled back to front
#define V_SPEED        0xD24      // float, km/h (the lead's is the train's)
#define V_TYPE         0x1708     // vehicle type
#define T_CATEGORY     0x294      // int: 3 wagon, 4 locomotive, 5 rail service
#define T_LENGTH       0x7A78     // float, car length (m)
#define T_FRONT_EXT    0x7A7C     // float, front extent of the model (m)
#define T_PIVOT_FRONT  0x7AF8     // float, front pivot inset from the front end (m)
#define T_PIVOT_BACK   0x7AFC     // float, back pivot inset from the back end (m)
#define CAT_LOCOMOTIVE 4

// ---------------------------------------------------------------- signatures
// The flip: mov r11,rsp; push rbp; push rbx; lea rbp,[r11-0x458]; sub rsp,0x548 (19 B stolen) ...
static const short SIG_FLIP[] = {
    0x4c,0x8b,0xdc, 0x55, 0x53, 0x49,0x8d,0xab,0xa8,0xfb,0xff,0xff, 0x48,0x81,0xec,0x48,0x05,0x00,0x00,
    0x48,0x8b,0x05,-1,-1,-1,-1, 0x48,0x33,0xc4, 0x48,0x89,0x85,0x10,0x04,0x00,0x00,
    0x49,0x89,0x73,0x10, 0x41,0x0f,0xb6,0xd8,
};
#define FLIP_STOLEN 19

// setTransform(vehicle, float3* pos, float yaw, float pitch): mov [rsp+8],rbx; push rdi;
// sub rsp,0x80; cmp byte [rcx+0x4c4],0 (20 B stolen) ...
static const short SIG_XFORM[] = {
    0x48,0x89,0x5c,0x24,0x08, 0x57, 0x48,0x81,0xec,0x80,0x00,0x00,0x00, 0x80,0xb9,0xc4,0x04,0x00,0x00,0x00,
    0x48,0x8b,0xda, 0x0f,0x29,0x74,0x24,0x70, 0x48,0x8b,0xf9, 0x0f,0x29,0x7c,0x24,0x60, 0x0f,0x28,0xf3,
};
#define XFORM_STOLEN 20

// sample(lead, float3* out, float metresBehindHead, ...): the point on the train's trail.
// Called, not hooked.
static const short SIG_SAMPLE[] = {
    0x48,0x89,0x5c,0x24,0x08, 0x48,0x89,0x74,0x24,0x10, 0x48,0x89,0x7c,0x24,0x18, 0x41,0x56,
    0x48,0x83,0xec,0x50, 0x4c,0x8b,0x81,0x98,0x07,0x00,0x00, 0x4c,0x8d,0x91,0xe0,0x07,0x00,0x00,
    0x0f,0x57,0xc0, 0x33,0xff, 0x48,0x8b,0xda, 0x4d,0x8b,0xf1, 0x0f,0xb6,0x91,0xa0,0x07,0x00,0x00,
};

typedef void   (*FnFlip)(BYTE* lead, char routeMode, char extend);
typedef void   (*FnXform)(BYTE* vehicle, float* pos, float yaw, float pitch);
typedef float* (*FnSample)(BYTE* lead, float* out, float metres, void* outSeg, void* outDir, void* outRest);
typedef void   (*FnRotXY)(float* outRot, const float* dir);   // C3DVec3GetRotationXY

static FnFlip   g_origFlip;
static FnXform  g_origXform;
static FnSample g_sample;
static FnRotXY  g_rotXY;

// ---------------------------------------------------------------- settings
static struct {
    float pushKmh;          // speed cap while pushing, 0 = none
    int   skipBothEnds;     // a locomotive at the far end too: leave the train alone
    int   logFlips;
} cfg;

// ---------------------------------------------------------------- pushing trains
// Keyed by the lead's address. The type and the wagon buffer are kept as a
// fingerprint: after a save is loaded, or wagons are added or removed, the
// address may be reused by a different train, and a mismatch then reads as
// "not pushing" rather than drawing someone else's train mirrored.
struct Train { BYTE* lead; BYTE* type; BYTE* wagons; int pushing; };
#define TRAIN_SLOTS 4096
static Train    g_trains[TRAIN_SLOTS];
static int      g_trainCount;
static volatile LONG g_anyPushing;        // fast path: zero means no lookups at all
static unsigned g_flips, g_fallbacks;

static unsigned SlotOf(BYTE* p) { return (unsigned)(((ULONG_PTR)p >> 4) * 2654435761u) & (TRAIN_SLOTS - 1); }

template <class T> static bool Rd(const BYTE* base, size_t off, T* out)
{
    if (!base || !ReadablePtr(base + off, sizeof(T))) return false;
    *out = *(const T*)(base + off);
    return true;
}

static Train* Find(BYTE* lead)            // caller holds g_lock
{
    for (unsigned i = SlotOf(lead), n = 0; n < TRAIN_SLOTS; i = (i + 1) & (TRAIN_SLOTS - 1), ++n) {
        if (g_trains[i].lead == lead) return &g_trains[i];
        if (!g_trains[i].lead) return 0;
    }
    return 0;
}

static void Compact(void)                 // caller holds g_lock; keep only pushing trains
{
    static Train keep[TRAIN_SLOTS];
    int n = 0;
    for (int i = 0; i < TRAIN_SLOTS; ++i) if (g_trains[i].lead && g_trains[i].pushing) keep[n++] = g_trains[i];
    memset(g_trains, 0, sizeof(g_trains));
    g_trainCount = 0;
    for (int k = 0; k < n; ++k) {
        unsigned i = SlotOf(keep[k].lead);
        while (g_trains[i].lead) i = (i + 1) & (TRAIN_SLOTS - 1);
        g_trains[i] = keep[k];
        ++g_trainCount;
    }
}

static Train* Insert(BYTE* lead)          // caller holds g_lock
{
    if (Train* t = Find(lead)) return t;
    if (g_trainCount > TRAIN_SLOTS * 3 / 4) Compact();
    if (g_trainCount > TRAIN_SLOTS * 3 / 4) return 0;
    unsigned i = SlotOf(lead);
    while (g_trains[i].lead) i = (i + 1) & (TRAIN_SLOTS - 1);
    g_trains[i].lead = lead;
    ++g_trainCount;
    return &g_trains[i];
}

static bool Fingerprint(BYTE* lead, BYTE** type, BYTE** wagons)
{
    return Rd(lead, V_TYPE, type) && Rd(lead, V_WAGONS_BEGIN, wagons);
}

// Hot path: runs for every rail car every frame while any train is pushing.
// `lead` comes from the game's own vehicle, so it is read directly rather
// than through readablePtr (a VirtualQuery per read).
static bool IsPushing(BYTE* lead)
{
    if (!g_anyPushing) return false;
    BYTE* type   = *(BYTE**)(lead + V_TYPE);
    BYTE* wagons = *(BYTE**)(lead + V_WAGONS_BEGIN);
    bool r = false;
    EnterCriticalSection(&g_lock);
    Train* t = Find(lead);
    if (t && t->pushing) {
        if (t->type == type && t->wagons == wagons) r = true;
        else { t->pushing = 0; InterlockedDecrement(&g_anyPushing); }   // a different train now
    }
    LeaveCriticalSection(&g_lock);
    return r;
}

// ---------------------------------------------------------------- consist
static int Wagons(BYTE* lead, BYTE*** begin)
{
    BYTE *b = 0, *e = 0;
    if (!Rd(lead, V_WAGONS_BEGIN, &b) || !Rd(lead, V_WAGONS_END, &e) || e < b) return -1;
    int n = (int)((e - b) / 8);
    if (n > 1024 || (n && !ReadablePtr(b, (size_t)n * 8))) return -1;
    *begin = (BYTE**)b;
    return n;
}

static float CarLength(BYTE* v)
{
    BYTE* t = 0; float len = 0;
    return Rd(v, V_TYPE, &t) && Rd(t, T_LENGTH, &len) ? len : 0.0f;
}

static int Category(BYTE* v)
{
    BYTE* t = 0; int c = -1;
    return Rd(v, V_TYPE, &t) && Rd(t, T_CATEGORY, &c) ? c : -1;
}

// ---------------------------------------------------------------- the flip
static void HookFlip(BYTE* lead, char routeMode, char extend)
{
    g_origFlip(lead, routeMode, extend);

    BYTE** w = 0;
    int n = Wagons(lead, &w);
    ++g_flips;
    if (n <= 0 || Category(lead) != CAT_LOCOMOTIVE) return;              // nothing to push
    bool bothEnds = Category(w[n - 1]) == CAT_LOCOMOTIVE;

    BYTE *type, *wagons;
    if (!Fingerprint(lead, &type, &wagons)) return;
    int now = -1;
    EnterCriticalSection(&g_lock);
    Train* t = Insert(lead);
    if (t) {
        bool same = t->type == type && t->wagons == wagons;
        int was = same ? t->pushing : 0;
        if (t->pushing) InterlockedDecrement(&g_anyPushing);
        t->type = type; t->wagons = wagons;
        t->pushing = (cfg.skipBothEnds && bothEnds) ? 0 : !was;
        if (t->pushing) InterlockedIncrement(&g_anyPushing);
        now = t->pushing;
    }
    LeaveCriticalSection(&g_lock);

    if (cfg.logFlips) {
        float L = CarLength(lead);
        for (int i = 0; i < n; ++i) L += CarLength(w[i]);
        Logf("railreverse  flip #%u train %p (%d wagons, %.1f m)%s -> %s", g_flips, lead, n, L,
             bothEnds ? " loco at both ends" : "", now < 0 ? "untracked" : now ? "PUSHING" : "pulling");
    }
}

// ---------------------------------------------------------------- placement
// The wagon placement in FUN_1406a5410, with d -> L - d.
static bool MirrorPlace(BYTE* v, BYTE* lead, float* pos, float* yaw, float* pitch)
{
    BYTE** w = 0;
    int n = Wagons(lead, &w);
    if (n <= 0) return false;

    float L = CarLength(lead), a = -1.0f;
    if (v == lead) a = 0.0f;
    for (int i = 0; i < n; ++i) {
        if (w[i] == v) a = L;
        L += CarLength(w[i]);
    }
    if (a < 0.0f) return false;                                           // not in this consist

    BYTE* type = 0;
    float len, ext, pf, pb;
    unsigned char rev = 0;
    if (!Rd(v, V_TYPE, &type) || !Rd(type, T_LENGTH, &len) || !Rd(type, T_FRONT_EXT, &ext)) return false;
    Rd(v, V_REVERSED, &rev);
    if (!Rd(type, rev ? T_PIVOT_BACK : T_PIVOT_FRONT, &pf) || !Rd(type, rev ? T_PIVOT_FRONT : T_PIVOT_BACK, &pb)) return false;

    float front[3] = {0}, back[3] = {0};
    g_sample(lead, front, L - (a + pf), 0, 0, 0);
    g_sample(lead, back,  L - (a + len - pb), 0, 0, 0);
    if ((!front[0] && !front[1] && !front[2]) || (!back[0] && !back[1] && !back[2])) return false;  // past the trail

    float d[3] = { front[0] - back[0], front[1] - back[1], front[2] - back[2] };
    float m = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (m < 1e-4f) return false;
    d[0] /= m; d[1] /= m; d[2] /= m;

    float rot[3] = {0};
    g_rotXY(rot, d);
    float k = ext - pf;
    pos[0] = front[0] - d[0] * k;
    pos[1] = front[1] - d[1] * k;
    pos[2] = front[2] - d[2] * k;
    *yaw = rot[1];
    *pitch = rot[0];
    return true;
}

static void HookXform(BYTE* v, float* pos, float yaw, float pitch)
{
    if (g_anyPushing && v) {
        BYTE* lead = *(BYTE**)(v + V_PARENT);
        if (!lead) lead = v;
        {
            if (IsPushing(lead)) {
                if (v == lead && cfg.pushKmh > 0.0f) {
                    float* speed = (float*)(lead + V_SPEED);
                    if (*speed > cfg.pushKmh) *speed = cfg.pushKmh;
                }
                float p[3], y, pt;
                if (MirrorPlace(v, lead, p, &y, &pt)) {
                    g_origXform(v, p, y, pt);
                    return;
                }
                if (++g_fallbacks <= 20)
                    Logf("railreverse  car %p of pushing train %p: trail not available, drawn as vanilla this frame", v, lead);
            }
        }
    }
    g_origXform(v, pos, yaw, pitch);
}

// ---------------------------------------------------------------- plugin
static BYTE* FindUnique(const short* sig, size_t n)
{
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(g_exeBase + ((IMAGE_DOS_HEADER*)g_exeBase)->e_lfanew);
    IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
    BYTE* found = 0;
    int hits = 0;
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++s) {
        if (memcmp(s->Name, ".text", 5) != 0) continue;
        BYTE* p   = g_exeBase + s->VirtualAddress;
        BYTE* end = p + s->Misc.VirtualSize - n;
        for (; p <= end; ++p) {
            if (p[0] != (BYTE)sig[0]) continue;
            size_t k = 1;
            while (k < n && (sig[k] < 0 || p[k] == (BYTE)sig[k])) ++k;
            if (k == n) { found = p; ++hits; }
        }
    }
    return hits == 1 ? found : 0;
}

#define SIG(x) x, sizeof(x) / sizeof(x[0])

static bool Hook(const short* sig, size_t n, size_t stolen, void* detour, void** orig, const char* label)
{
    BYTE* site = FindUnique(sig, n);
    if (!site) { Logf("railreverse  %s: signature not found or not unique", label); return false; }
    BYTE expect[32];
    for (size_t i = 0; i < stolen; ++i) expect[i] = (BYTE)sig[i];
    if (!InstallInlineHook(site, detour, orig, expect, stolen, label)) { Logf("railreverse  %s: hook refused", label); return false; }
    Logf("railreverse  %s hooked at +%06llX", label, (unsigned long long)(site - g_exeBase));
    return true;
}

extern "C" __declspec(dllexport) unsigned TsmPluginApiVersion(void)
{
    return TSM_API_VERSION;
}

extern "C" __declspec(dllexport) int TsmPluginInit(const TsmHost* host, TsmPluginInfo* info)
{
    TsmBind(host);
    info->name    = "railreverse";
    info->version = RR_VERSION;

    if (!H->configInt(RR_INI, "railreverse", "enabled", 1)) {
        Logf("railreverse  disabled in ini");
        return 1;                       // nothing hooked, safe to unload
    }
    cfg.pushKmh      = (float)H->configInt(RR_INI, "railreverse", "push_speed_kmh", 30);
    cfg.skipBothEnds = H->configInt(RR_INI, "railreverse", "skip_loco_both_ends", 1);
    cfg.logFlips     = H->configInt(RR_INI, "railreverse", "log_flips", 1);
    Logf("railreverse  v%s init: push speed %.0f km/h, skip loco at both ends %d",
         RR_VERSION, cfg.pushKmh, cfg.skipBothEnds);
    return 0;
}

extern "C" __declspec(dllexport) int TsmPluginStart(void)
{
    // Everything is resolved before anything is hooked: a missing piece
    // leaves the game untouched instead of half-patched.
    g_sample = (FnSample)FindUnique(SIG(SIG_SAMPLE));
    g_rotXY  = (FnRotXY)GetProcAddress(g_engine, "?C3DVec3GetRotationXY@@YAXPEAVC3DVECTOR3@@PEBV1@@Z");
    if (!g_sample || !g_rotXY) {
        Logf("railreverse  %s not found - nothing hooked", !g_sample ? "trail sampler" : "C3DVec3GetRotationXY");
        return 1;
    }
    if (!FindUnique(SIG(SIG_FLIP)) || !FindUnique(SIG(SIG_XFORM))) {
        Logf("railreverse  flip or placement signature not found - nothing hooked");
        return 1;
    }
    if (!Hook(SIG(SIG_XFORM), XFORM_STOLEN, (void*)HookXform, (void**)&g_origXform, "placement")) return 1;
    if (!Hook(SIG(SIG_FLIP), FLIP_STOLEN, (void*)HookFlip, (void**)&g_origFlip, "flip")) {
        // placement is hooked but inert: with no flip hook nothing is ever marked pushing
        Logf("railreverse  placement hook stays inert");
        return 0;
    }
    Logf("railreverse  ready");
    return 0;
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }

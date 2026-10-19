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
// Which trains push is chosen in game, in the train window: one hotkey turns
// reversing on or off for that train, two more switch it on for every
// freight / every passenger train. All of it is remembered in the save folder.
//
// Addresses, formulas and the reasoning: docs/findings-1a.md, docs/findings-0.3.md.

#include "../include/tesmio_plugin.h"
#include <math.h>
#include <wchar.h>

#define RR_VERSION "0.3.1"
#define RR_INI     "plugins\\railreverse.ini"

// ---------------------------------------------------------------- layout (1.1.1.9)
#define V_PARENT       0x398      // wagon: its lead; 0 on the lead itself
#define V_WAGONS_BEGIN 0x3A0      // lead: std::vector<vehicle*> of wagons, front to back
#define V_WAGONS_END   0x3A8
#define V_REVERSED     0x4C4      // u8: car coupled back to front
#define V_BUILDING     0x4F0      // building the train is at / entering
#define V_SPEED        0xD24      // float, km/h (the lead's is the train's)
#define V_TYPE         0x1708     // vehicle type
#define B_HAS_TRACKS   0x5D0      // building: nonzero for stations, zero for depots that swallow trains
#define T_NAME         0x000      // char[512], the type's folder
#define T_CATEGORY     0x294      // int: 3 wagon, 4 locomotive, 5 rail service
#define T_CLASS        0x8600     // int: transport class, 7 = passengers (RailPhysics findings 06)
#define CLASS_PASSENGERS 7
#define T_LENGTH       0x7A78     // float, car length (m)
#define T_FRONT_EXT    0x7A7C     // float, front extent of the model (m)
#define T_PIVOT_FRONT  0x7AF8     // float, front pivot inset from the front end (m)
#define T_PIVOT_BACK   0x7AFC     // float, back pivot inset from the back end (m)
#define CAT_LOCOMOTIVE 4

// Info window (the same object for every kind of window, see FUN_1406e9b80)
#define W_POS_X        0x004
#define W_POS_Y        0x008
#define W_OFF_X        0x028
#define W_OFF_Y        0x02C
#define W_OBJECT       0x240      // what the window shows
#define W_KIND         0x248      // int: 0 building, 1 vehicle, ...
#define WINDOW_VEHICLE 1
#define RVA_UI_SCALE   0x992088   // float, 1.1.1.9 (DockDistributionOffice, ForceNodes)

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

// The vehicle info window. FUN_1406e9b80 draws a vehicle window (kind 1) with
// FUN_1407d5460(ui, window), or with FUN_14080cc60(ui, window) when
// window+0x23C is 9 or 10. Both start: push rbp/rsi/rdi/r12-r15;
// lea rbp,[rsp-N] (20 B stolen); mov eax,size; call __chkstk; sub rsp,rax ...
static const short SIG_VWINDOW[] = {          // FUN_1407d5460, the usual one
    0x40,0x55, 0x56, 0x57, 0x41,0x54, 0x41,0x55, 0x41,0x56, 0x41,0x57, 0x48,0x8d,0xac,0x24,0x50,0xd1,0xff,0xff,
    0xb8,0xc0,0x2f,0x00,0x00, 0xe8,-1,-1,-1,-1, 0x48,0x2b,0xe0,
    0x48,0xc7,0x85,0xf0,0x01,0x00,0x00,0xfe,0xff,0xff,0xff,
};
static const short SIG_VPANEL[] = {           // FUN_14080cc60, tabs 9-10
    0x40,0x55, 0x56, 0x57, 0x41,0x54, 0x41,0x55, 0x41,0x56, 0x41,0x57, 0x48,0x8d,0xac,0x24,0x90,0xe5,0xff,0xff,
    0xb8,0x70,0x1b,0x00,0x00, 0xe8,-1,-1,-1,-1, 0x48,0x2b,0xe0,
    0x48,0xc7,0x85,0x78,0x03,0x00,0x00,0xfe,0xff,0xff,0xff,
};
#define VPANEL_STOLEN 20

// The info-window dispatcher FUN_1406e9b80(ui): push rbp/rbx/rsi/rdi/r14;
// lea rbp,[rsp-0x1400] (15 B stolen); mov eax,0x1500; call __chkstk; sub rsp,rax;
// mov rax,[rip+cookie]; xor rax,rsp; mov [rbp+0x1350],rax
static const short SIG_DISPATCH[] = {
    0x40,0x55, 0x53, 0x56, 0x57, 0x41,0x56, 0x48,0x8d,0xac,0x24,0x00,0xec,0xff,0xff,
    0xb8,0x00,0x15,0x00,0x00, 0xe8,-1,-1,-1,-1, 0x48,0x2b,0xe0,
    0x48,0x8b,0x05,-1,-1,-1,-1, 0x48,0x33,0xc4, 0x48,0x89,0x85,0x50,0x13,0x00,0x00,
};
#define DISPATCH_STOLEN 15

// The game's list of every vehicle, in save order: vehicles.bin is written by
// walking it and read back into it in the same order.
// mov rax,[rip+end]; mov rcx,[rip+begin]; ... call <read vehicle>; mov [rbx+4],edi
static const short SIG_VEHVEC[] = {
    0x48,0x8b,0x05,-1,-1,-1,-1, 0x48,0x8b,0x0d,-1,-1,-1,-1, 0x48,0x2b,0xc1, 0x48,0xc1,0xf8,0x03,
    0x48,0x85,0xc0, 0x0f,0x84,-1,-1,-1,-1, 0x49,0x8b,0xd7, 0x66,0x90, 0x48,0x8b,0x1c,0xd1,
    0x48,0x8b,0xd6, 0x48,0x8b,0xcb, 0xe8,-1,-1,-1,-1, 0x89,0x7b,0x04,
};
#define VEHVEC_DISP_AT 10         // disp32 of the `mov rcx,[rip+begin]`
#define VEHVEC_NEXT_AT 14         // the instruction after it

#define EXP_ROTXY "?C3DVec3GetRotationXY@@YAXPEAVC3DVECTOR3@@PEBV1@@Z"
#define EXP_PRINT "?PrintLeftUnicodeNoArg@C3D_FONTMANAGER@@QEAAXPEAVC3D_FONT@@MMKPEB_W@Z"

typedef void   (*FnFlip)(BYTE* lead, char routeMode, char extend);
typedef void   (*FnXform)(BYTE* vehicle, float* pos, float yaw, float pitch);
typedef float* (*FnSample)(BYTE* lead, float* out, float metres, void* outSeg, void* outDir, void* outRest);
typedef void   (*FnRotXY)(float* outRot, const float* dir);
typedef void   (*FnPanel)(BYTE* ui, BYTE* window);
typedef void   (*FnPrint)(void* manager, void* font, float x, float y, unsigned long color, const wchar_t* text);
typedef FILE*  (*FnFopen)(const char* path, const char* mode);

static FnFlip   g_origFlip;
static FnXform  g_origXform;
static FnPanel  g_origWindow, g_origPanel;
typedef void   (*FnDispatch)(BYTE* ui);
static FnDispatch g_origDispatch;
static FnFopen  g_origFopen;
static FnSample g_sample;
static FnRotXY  g_rotXY;
static FnPrint  g_print;
static BYTE*    g_fontManager;
static void**   g_fontSlot;
static BYTE***  g_vehicles;       // &vector.begin; end is the next qword

// ---------------------------------------------------------------- settings
struct Key { int vk; int ctrl, shift, alt; char label[32]; };

static struct {
    int   allFreightDefault;    // the switches a save without our file starts with
    int   allPassengerDefault;
    float pushKmh;          // speed cap while pushing, 0 = none
    float lightKmh;         // speed cap for a locomotive running backwards on its own, 0 = none
    int   skipBothEnds;     // a locomotive at the far end too: leave the train alone
    int   logFlips;
    int   indicator;        // draw the state above the train window
    int   anchor;           // screen corner: ANCHOR_*
    float indicatorX, indicatorY;   // offset from that corner, % of the screen width / height
    int   fontScale;        // indicator font, % of the default UI font
    int   outline;          // outline width, px
    Key   keyTrain, keyFreight, keyPassenger;
} cfg;

// A train pushes after a reversal if it was chosen in its window, or if every
// train of its class (freight / passenger) is switched on.
static volatile LONG g_allFreight, g_allPassenger;

static bool ParseKey(const char* s, Key* k)
{
    memset(k, 0, sizeof(*k));
    strncpy(k->label, s, sizeof(k->label) - 1);
    char buf[64];
    strncpy(buf, s, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    for (char* tok = strtok(buf, "+ "); tok; tok = strtok(0, "+ ")) {
        if (!_stricmp(tok, "CTRL")) k->ctrl = 1;
        else if (!_stricmp(tok, "SHIFT")) k->shift = 1;
        else if (!_stricmp(tok, "ALT")) k->alt = 1;
        else if ((tok[0] == 'F' || tok[0] == 'f') && atoi(tok + 1) >= 1 && atoi(tok + 1) <= 12) k->vk = VK_F1 + atoi(tok + 1) - 1;
        else if (!tok[1] && ((tok[0] >= 'A' && tok[0] <= 'Z') || (tok[0] >= '0' && tok[0] <= '9'))) k->vk = tok[0];
        else if (!tok[1] && tok[0] >= 'a' && tok[0] <= 'z') k->vk = tok[0] - 32;
        else return false;
    }
    return k->vk != 0;
}

static bool Down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

// One press = one action: true on the frame the combination becomes held.
static bool Pressed(const Key* k, int* latch)
{
    bool held = k->vk && Down(k->vk) && Down(VK_CONTROL) == !!k->ctrl && Down(VK_SHIFT) == !!k->shift &&
                Down(VK_MENU) == !!k->alt;
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    if (pid != GetCurrentProcessId()) held = false;   // not while the game is in the background
    bool edge = held && !*latch;
    *latch = held;
    return edge;
}

// ---------------------------------------------------------------- trains
// Keyed by the locomotive's address, with its type as the identity check: a
// different type at the same address is another vehicle (memory reused after
// a sale) and starts fresh.
//   chosen   - reversing turned on for this locomotive. Belongs to the
//              locomotive, so it survives swapping wagons at a distribution
//              office.
//   pushing  - drawn mirrored right now.
//   pose     - where the locomotive was last drawn (x, y, z, yaw). When the
//              wagon count changes (wagons picked up or left at a distribution
//              office) the game rebuilds the consist behind its logical head;
//              `pushing` is then set to whichever drawing keeps the
//              locomotive where and how it stood, so coupling never makes it
//              jump or turn round. See Resolve.
//   turned   - the locomotive is drawn turned round (as if coupled back to
//              front, +0x4C4), cab towards its wagons. The game always draws a
//              locomotive with its back to its wagons; a distribution office
//              that couples wagons at the cab end of a locomotive that came in
//              backwards needs this. Physical, so a reversal keeps it; a new
//              consist or a depot settles it again.
struct Train { BYTE* lead; BYTE* type; INT_PTR wagons; int pushing; int chosen; int turned;
               int hasPose; float pose[4];          // model position + heading, for Resolve
               float last[5]; int lastTurn; };     // the last setTransform arguments
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

static void Compact(void)                 // caller holds g_lock; keep only trains with state
{
    static Train keep[TRAIN_SLOTS];
    int n = 0;
    for (int i = 0; i < TRAIN_SLOTS; ++i)
        if (g_trains[i].lead && (g_trains[i].pushing || g_trains[i].chosen)) keep[n++] = g_trains[i];
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

static bool Fingerprint(BYTE* lead, BYTE** type, INT_PTR* wagons)
{
    BYTE *b, *e;
    if (!Rd(lead, V_TYPE, type) || !Rd(lead, V_WAGONS_BEGIN, &b) || !Rd(lead, V_WAGONS_END, &e)) return false;
    *wagons = (e - b) / 8;
    return true;
}

// The record for this lead, reset if it belonged to another train. Caller holds g_lock.
static Train* Current(BYTE* lead, bool create)
{
    BYTE* type; INT_PTR wagons;
    if (!Fingerprint(lead, &type, &wagons)) return 0;
    Train* t = create ? Insert(lead) : Find(lead);
    if (!t) return 0;
    if (t->type != type) {                                                 // another vehicle
        if (t->pushing) InterlockedDecrement(&g_anyPushing);
        t->pushing = 0; t->chosen = 0; t->turned = 0; t->hasPose = 0;
        t->type = type; t->wagons = wagons;
    }
    // A new wagon count is left for Resolve (placement or flip), which knows
    // where the locomotive stood.
    return t;
}

static void SetPushing(Train* t, int on)  // caller holds g_lock
{
    if (t->pushing == on) return;
    t->pushing = on;
    if (on) InterlockedIncrement(&g_anyPushing); else InterlockedDecrement(&g_anyPushing);
}

// Hot path: runs for every rail car every frame while any train is pushing.
// `lead` comes from the game's own vehicle, so it is read directly rather
// than through readablePtr (a VirtualQuery per read).
static bool IsPushing(BYTE* lead)
{
    if (!g_anyPushing) return false;
    BYTE* type     = *(BYTE**)(lead + V_TYPE);
    INT_PTR wagons = (*(BYTE**)(lead + V_WAGONS_END) - *(BYTE**)(lead + V_WAGONS_BEGIN)) / 8;
    bool r = false;
    EnterCriticalSection(&g_lock);
    Train* t = Find(lead);
    if (t && t->pushing) {
        // A wagon count not settled yet (Resolve) is drawn as the game draws
        // it: mirroring wagons around a consist that is still being built put
        // them on top of the locomotive.
        if (t->type == type) r = t->wagons == wagons;
        else { SetPushing(t, 0); t->chosen = 0; t->turned = 0; t->hasPose = 0; }   // another vehicle
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

// A locomotive heading its train (with or without wagons right now):
// what a choice belongs to.
static bool IsLoco(BYTE* v)
{
    BYTE* parent = (BYTE*)1;
    return v && Rd(v, V_PARENT, &parent) && !parent && Category(v) == CAT_LOCOMOTIVE;
}

// A locomotive with wagons: the only trains that can push.
static bool IsTrainLead(BYTE* v)
{
    BYTE** w = 0;
    return IsLoco(v) && Wagons(v, &w) > 0;
}

// A train with any passenger car is a passenger train (as RailPhysics plans it).
static bool IsPassengerTrain(BYTE* lead)
{
    BYTE** w = 0;
    int n = Wagons(lead, &w);
    for (int i = -1; i < n; ++i) {
        BYTE* v = i < 0 ? lead : w[i];
        BYTE* t = 0; int c = -1;
        if (Rd(v, V_TYPE, &t) && Rd(t, T_CLASS, &c) && c == CLASS_PASSENGERS) return true;
    }
    return false;
}

static bool ClassOn(BYTE* lead) { return IsPassengerTrain(lead) ? g_allPassenger != 0 : g_allFreight != 0; }

static bool LocoAtBothEnds(BYTE* lead)
{
    BYTE** w = 0;
    int n = Wagons(lead, &w);
    return n > 0 && Category(w[n - 1]) == CAT_LOCOMOTIVE;
}

// ---------------------------------------------------------------- the flip
static void Resolve(Train* t, BYTE* lead, INT_PTR wagons, const float* vanPos, float vanYaw, const char* when);

static void HookFlip(BYTE* lead, char routeMode, char extend)
{
    // Wagons coupled or left and not settled yet (a distribution office adds
    // them while the train stands inside, then reverses it to send it off).
    // That reversal belongs to the consist change: it does not toggle; the
    // new consist is drawn whichever way keeps the locomotive where it stood.
    bool changed = false;
    BYTE* type = 0; INT_PTR wagons = 0;
    if (g_trainCount && lead) {
        EnterCriticalSection(&g_lock);
        Train* t = Find(lead);
        changed = t && Fingerprint(lead, &type, &wagons) && t->type == type && t->wagons != wagons;
        LeaveCriticalSection(&g_lock);
    }

    g_origFlip(lead, routeMode, extend);

    ++g_flips;
    if (changed) {
        EnterCriticalSection(&g_lock);
        if (Train* t = Find(lead)) Resolve(t, lead, wagons, 0, 0.0f, "at its reversal");
        LeaveCriticalSection(&g_lock);
        return;
    }
    if (!IsLoco(lead)) return;                       // a locomotive, with wagons or running light
    bool bothEnds = LocoAtBothEnds(lead);
    bool classOn  = ClassOn(lead);

    int now = -1, chosen = 0;
    EnterCriticalSection(&g_lock);
    Train* t = Current(lead, true);
    if (t) {
        // Reversing that is switched off takes effect here, at a reversal,
        // so a train never jumps: it finishes the trip it is pushing.
        bool allowed = (classOn || t->chosen) && !(cfg.skipBothEnds && bothEnds);
        SetPushing(t, allowed ? !t->pushing : 0);
        now = t->pushing;
        chosen = t->chosen;
    }
    LeaveCriticalSection(&g_lock);

    if (cfg.logFlips) {
        BYTE** w = 0;
        int n = Wagons(lead, &w);
        float L = CarLength(lead);
        for (int i = 0; i < n; ++i) L += CarLength(w[i]);
        Logf("railreverse  flip #%u train %p (%d wagons, %.1f m)%s%s -> %s", g_flips, lead, n, L,
             bothEnds ? " loco at both ends" : "", chosen ? " chosen" : "",
             now < 0 ? "untracked" : now ? "PUSHING" : "pulling");
    }
}

// ---------------------------------------------------------------- saves
// railreverse.txt beside vehicles.bin:
//   freight 0|1
//   passenger 0|1
//   <index in the vehicle list> <flags: 1 pushing, 2 chosen, 4 locomotive turned round> <type folder>
// The index is stable because the game writes and reads the list in the same
// order; the type folder guards against a stale file. The save itself is
// never touched.
#define SIDECAR       "railreverse.txt"
#define TYPE_NAME_MAX 512
struct Pending { int index; int flags; char type[TYPE_NAME_MAX]; };
static Pending        g_pending[512];
static int            g_pendingCount;
static volatile LONG  g_pendingLive;      // nonzero while restoring after a load
static unsigned       g_pendingTries;

static bool EndsWithI(const char* s, const char* tail)
{
    size_t n = strlen(s), m = strlen(tail);
    return n >= m && _stricmp(s + n - m, tail) == 0;
}

static bool SidecarPath(const char* path, char* out, size_t cap)
{
    const char* slash = 0;
    for (const char* p = path; *p; ++p) if (*p == '\\' || *p == '/') slash = p;
    if (!slash || (size_t)(slash - path) + sizeof(SIDECAR) + 2 > cap) return false;
    memcpy(out, path, (size_t)(slash - path) + 1);
    strcpy(out + (slash - path) + 1, SIDECAR);
    return true;
}

static int VehicleCount(BYTE*** begin)
{
    if (!g_vehicles) return 0;
    BYTE** b = g_vehicles[0];
    BYTE** e = g_vehicles[1];
    if (!b || e < b || !ReadablePtr(b, (size_t)(e - b) * 8)) return 0;
    *begin = b;
    return (int)(e - b);
}

static void TypeName(BYTE* v, char* out)
{
    BYTE* t = 0;
    out[0] = 0;
    if (!Rd(v, V_TYPE, &t) || !t || !ReadablePtr(t + T_NAME, TYPE_NAME_MAX)) return;
    int i = 0;
    for (; i < TYPE_NAME_MAX - 1 && t[T_NAME + i]; ++i) {
        char c = (char)t[T_NAME + i];
        out[i] = (c == '\r' || c == '\n') ? ' ' : c;
    }
    out[i] = 0;
}

static void SaveSidecar(const char* vehiclesBin)
{
    char path[MAX_PATH * 2];
    if (!SidecarPath(vehiclesBin, path, sizeof(path))) return;
    BYTE** veh = 0;
    int n = VehicleCount(&veh);
    static char buf[512 * (TYPE_NAME_MAX + 24) + 64];
    int len = _snprintf_s(buf, sizeof(buf), _TRUNCATE, "freight %d\npassenger %d\n",
                          g_allFreight ? 1 : 0, g_allPassenger ? 1 : 0);
    int trains = 0;
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < n && trains < 512; ++i) {
        BYTE* v = veh[i];
        BYTE* parent = 0;
        if (!v || !Rd(v, V_PARENT, &parent) || parent) continue;
        Train* t = Find(v);
        if (!t || (!t->pushing && !t->chosen)) continue;
        BYTE* type; INT_PTR wagons;
        if (!Fingerprint(v, &type, &wagons) || type != t->type) continue;
        int pushing = t->pushing && wagons == t->wagons;                  // consist changed: saved as pulling
        char name[TYPE_NAME_MAX];
        TypeName(v, name);
        len += _snprintf_s(buf + len, sizeof(buf) - len, _TRUNCATE, "%d %d %s\n", i,
                           (pushing ? 1 : 0) | (t->chosen ? 2 : 0) | (t->turned && pushing == t->pushing ? 4 : 0), name);
        ++trains;
    }
    LeaveCriticalSection(&g_lock);
    if (!trains && (int)g_allFreight == cfg.allFreightDefault && (int)g_allPassenger == cfg.allPassengerDefault) {
        DeleteFileA(path);                // nothing to remember: no stale file for the next load
        Logf("railreverse  save: nothing to remember");
        return;
    }
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE) { Logf("railreverse  save: cannot write %s", path); return; }
    DWORD wrote = 0;
    WriteFile(h, buf, (DWORD)len, &wrote, 0);
    CloseHandle(h);
    Logf("railreverse  save: all freight %d, all passenger %d, %d train(s) -> %s",
         g_allFreight ? 1 : 0, g_allPassenger ? 1 : 0, trains, path);
}

static void LoadSidecar(const char* vehiclesBin)
{
    // A load means a new world: forget every train of the old one.
    EnterCriticalSection(&g_lock);
    memset(g_trains, 0, sizeof(g_trains));
    g_trainCount = 0;
    g_anyPushing = 0;
    g_pendingCount = 0;
    g_pendingLive = 0;
    g_allFreight = cfg.allFreightDefault;
    g_allPassenger = cfg.allPassengerDefault;
    LeaveCriticalSection(&g_lock);

    char path[MAX_PATH * 2];
    if (!SidecarPath(vehiclesBin, path, sizeof(path))) return;
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE) return;          // nothing remembered for this save
    static char buf[512 * (TYPE_NAME_MAX + 24) + 64 + 1];
    DWORD got = 0;
    ReadFile(h, buf, sizeof(buf) - 1, &got, 0);
    CloseHandle(h);
    buf[got] = 0;

    EnterCriticalSection(&g_lock);
    for (char* line = buf; *line; ) {
        char* end = strchr(line, '\n');
        if (end) *end = 0;
        size_t k = strlen(line);
        if (k && line[k - 1] == '\r') line[k - 1] = 0;
        if (!strncmp(line, "freight ", 8)) {
            g_allFreight = atoi(line + 8) != 0;
        } else if (!strncmp(line, "passenger ", 10)) {
            g_allPassenger = atoi(line + 10) != 0;
        } else if (g_pendingCount < 512) {
            char* a = strchr(line, ' ');
            char* b = a ? strchr(a + 1, ' ') : 0;
            if (a && b) {
                Pending* p = &g_pending[g_pendingCount++];
                p->index = atoi(line);
                p->flags = atoi(a + 1);
                strncpy(p->type, b + 1, TYPE_NAME_MAX - 1);
                p->type[TYPE_NAME_MAX - 1] = 0;
            }
        }
        if (!end) break;
        line = end + 1;
    }
    g_pendingTries = 0;
    g_pendingLive = g_pendingCount;
    LeaveCriticalSection(&g_lock);
    Logf("railreverse  load: all freight %d, all passenger %d, %d train(s) to restore from %s",
         g_allFreight ? 1 : 0, g_allPassenger ? 1 : 0, g_pendingCount, path);
}

// Runs from the placement hook, i.e. once the world is drawn, until every
// remembered train has been found (or ~20000 frames have passed).
static void RestorePending(void)
{
    BYTE** veh = 0;
    int n = VehicleCount(&veh);
    int restored = 0;
    EnterCriticalSection(&g_lock);
    for (int k = 0; k < g_pendingCount; ++k) {
        Pending* p = &g_pending[k];
        if (p->index < 0 || p->index >= n) continue;
        BYTE* v = veh[p->index];
        if (!IsLoco(v)) continue;
        char name[TYPE_NAME_MAX];
        TypeName(v, name);
        if (strcmp(name, p->type) != 0) continue;
        Train* t = Current(v, true);
        if (!t) continue;
        t->chosen = (p->flags & 2) ? 1 : 0;
        t->turned = (p->flags & 4) ? 1 : 0;
        SetPushing(t, (p->flags & 1) ? 1 : 0);
        p->index = -1;                    // done
        ++restored;
    }
    int left = 0;
    for (int k = 0; k < g_pendingCount; ++k) if (g_pending[k].index >= 0) ++left;
    bool giveUp = ++g_pendingTries > 20000;
    if (!left || giveUp) g_pendingLive = 0;
    LeaveCriticalSection(&g_lock);
    if (restored) Logf("railreverse  load: %d train(s) restored", restored);
    if (giveUp && left) Logf("railreverse  load: %d remembered train(s) not found, dropped", left);
}

static FILE* HookFopen(const char* path, const char* mode)
{
    if (path && mode && EndsWithI(path, "vehicles.bin")) {
        if (mode[0] == 'w') SaveSidecar(path);
        else if (mode[0] == 'r') LoadSidecar(path);
    }
    return g_origFopen(path, mode);
}

// ---------------------------------------------------------------- placement
// The wagon placement in FUN_1406a5410: as the game does it (mirror false),
// or on the mirror image of the car's slot, metres behind the head d -> L - d.
// turn: place the car as if its "coupled back to front" flag were flipped
// (the caller then draws it with the flag flipped too).
static bool Place(BYTE* v, BYTE* lead, bool mirror, bool turn, float* pos, float* yaw, float* pitch)
{
    BYTE** w = 0;
    int n = Wagons(lead, &w);
    if (n < 0) return false;                         // 0: a light engine, mirrored onto itself

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
    if (turn) rev = !rev;
    if (!Rd(type, rev ? T_PIVOT_BACK : T_PIVOT_FRONT, &pf) || !Rd(type, rev ? T_PIVOT_FRONT : T_PIVOT_BACK, &pb)) return false;

    float front[3] = {0}, back[3] = {0};
    float df = a + pf, db = a + len - pb;
    if (mirror) { df = L - df; db = L - db; }
    g_sample(lead, front, df, 0, 0, 0);
    g_sample(lead, back,  db, 0, 0, 0);
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

// A pushing train going into a depot: the building swallows the train car by
// car from its logical front, which is the locomotive drawn at the back. Stop
// mirroring at the door instead; it leaves the depot pulling.
static bool EnteringDepot(BYTE* lead)
{
    BYTE* b = *(BYTE**)(lead + V_BUILDING);
    BYTE* tracks = 0;
    return b && Rd(b, B_HAS_TRACKS, &tracks) && !tracks;
}

#define PI 3.1415927f

// The heading the model really gets: setTransform adds pi for a car with its
// "coupled back to front" flag set.
static float ModelYaw(BYTE* v, float yaw, bool turn)
{
    unsigned char rev = 0;
    Rd(v, V_REVERSED, &rev);
    return (rev != 0) != turn ? yaw + PI : yaw;
}

static float PoseError(const float* a, const float* p, float yaw)
{
    float dx = a[0] - p[0], dy = a[1] - p[1], dz = a[2] - p[2];
    float dyaw = fmodf(fabsf(a[3] - yaw), 2.0f * PI);
    if (dyaw > PI) dyaw = 2.0f * PI - dyaw;
    return sqrtf(dx * dx + dy * dy + dz * dz) + 8.0f * dyaw;              // 1 rad weighs like 8 m
}

// The wagon count changed: of the four ways to draw the new consist - as the
// game does, mirrored, and either with the locomotive turned round - take the
// one that keeps the locomotive where and how it stood before the change.
// vanPos/vanYaw: the game's own drawing of the locomotive this frame, or null
// to compute it. Caller holds g_lock.
static void Resolve(Train* t, BYTE* lead, INT_PTR wagons, const float* vanPos, float vanYaw, const char* when)
{
    float err[4] = { -1, -1, -1, -1 };     // index = mirror | turn << 1
    int best = 0;
    if (t->hasPose) {
        float bestErr = 1e30f;
        for (int k = 0; k < 4; ++k) {
            bool mirror = k & 1, turn = (k >> 1) & 1;
            float p[3], y, pt;
            bool ok;
            if (k == 0 && vanPos) { p[0] = vanPos[0]; p[1] = vanPos[1]; p[2] = vanPos[2]; y = vanYaw; ok = true; }
            else ok = Place(lead, lead, mirror, turn, p, &y, &pt);
            if (!ok) continue;
            err[k] = PoseError(t->pose, p, ModelYaw(lead, y, turn));
            if (err[k] < bestErr) { bestErr = err[k]; best = k; }
        }
    }
    SetPushing(t, best & 1);
    t->turned = (best >> 1) & 1;
    t->wagons = wagons;
    Logf("railreverse  train %p now has %d wagons (%s): %s%s  [error: pulling %.1f, pushing %.1f, "
         "turned+pulling %.1f, turned+pushing %.1f]", lead, (int)wagons, when,
         t->pushing ? "PUSHING" : "pulling", t->turned ? ", locomotive turned round" : "",
         err[0], err[1], err[2], err[3]);
}

// Draws the locomotive and remembers where its model went. turn: drawn with
// its "coupled back to front" flag flipped for the call only.
static void DrawLead(Train* t, BYTE* v, const float* p, float y, float pt, bool turn)
{
    t->pose[0] = p[0]; t->pose[1] = p[1]; t->pose[2] = p[2];
    t->pose[3] = ModelYaw(v, y, turn);
    t->hasPose = 1;
    t->last[0] = p[0]; t->last[1] = p[1]; t->last[2] = p[2]; t->last[3] = y; t->last[4] = pt;
    t->lastTurn = turn;
    if (turn) v[V_REVERSED] = !v[V_REVERSED];
    g_origXform(v, (float*)p, y, pt);
    if (turn) v[V_REVERSED] = !v[V_REVERSED];
}

// A locomotive heading a train this plugin keeps a record of.
static bool LeadXform(BYTE* v, float* pos, float yaw, float pitch)
{
    INT_PTR wagons = (*(BYTE**)(v + V_WAGONS_END) - *(BYTE**)(v + V_WAGONS_BEGIN)) / 8;
    BYTE* type = *(BYTE**)(v + V_TYPE);
    EnterCriticalSection(&g_lock);
    Train* t = Find(v);
    if (!t || t->type != type) { LeaveCriticalSection(&g_lock); return false; }
    if (t->wagons != wagons) {
        // Inside a building (a distribution office coupling or leaving wagons,
        // a station) the change is settled at the reversal that follows; until
        // then the train is drawn as the game draws it and the pose from
        // before the change is kept. Out on the line it is settled now.
        if (*(BYTE**)(v + V_BUILDING)) {
            // Standing there while wagons are coupled or left, the locomotive
            // stays exactly as it was last drawn. Moving (a train passing
            // through a building that takes or gives cars), it is drawn as
            // before the change and its pose follows.
            if (*(float*)(v + V_SPEED) > 1.0f) {
                float p[3], y, pt;
                bool turn = t->turned != 0;
                if ((t->pushing || turn) && Place(v, v, t->pushing != 0, turn, p, &y, &pt)) DrawLead(t, v, p, y, pt, turn);
                else DrawLead(t, v, pos, yaw, pitch, false);
            } else if (t->hasPose) {
                float p[3] = { t->last[0], t->last[1], t->last[2] };
                float y = t->last[3], pt = t->last[4];
                bool turn = t->lastTurn != 0;
                float keep[4] = { t->pose[0], t->pose[1], t->pose[2], t->pose[3] };
                DrawLead(t, v, p, y, pt, turn);
                memcpy(t->pose, keep, sizeof(keep));
            } else {
                g_origXform(v, pos, yaw, pitch);
            }
            LeaveCriticalSection(&g_lock);
            return true;
        }
        Resolve(t, v, wagons, pos, yaw, "on the line");
    }
    int pushing = t->pushing;
    bool turn = t->turned != 0;
    if ((pushing || turn) && EnteringDepot(v)) {
        SetPushing(t, 0);
        t->turned = 0;
        pushing = 0; turn = false;
        Logf("railreverse  train %p enters a depot: drawn as the game draws it again", v);
    }

    if (pushing) {
        // Wagons ahead: the pushing limit. Running light: its own limit.
        float cap = wagons ? cfg.pushKmh : cfg.lightKmh;
        float* speed = (float*)(v + V_SPEED);
        if (cap > 0.0f && *speed > cap) *speed = cap;
    }
    if (pushing || turn) {
        float p[3], y, pt;
        if (Place(v, v, pushing != 0, turn, p, &y, &pt)) {
            DrawLead(t, v, p, y, pt, turn);
            LeaveCriticalSection(&g_lock);
            return true;
        }
        if (++g_fallbacks <= 20)
            Logf("railreverse  train %p: trail not available, drawn as vanilla this frame", v);
    }
    DrawLead(t, v, pos, yaw, pitch, false);
    LeaveCriticalSection(&g_lock);
    return true;
}

static void HookXform(BYTE* v, float* pos, float yaw, float pitch)
{
    if (g_pendingLive) RestorePending();
    if (v && g_trainCount) {
        BYTE* lead = *(BYTE**)(v + V_PARENT);
        if (!lead) {
            if (LeadXform(v, pos, yaw, pitch)) return;
        } else if (g_anyPushing && IsPushing(lead)) {
            float p[3], y, pt;
            if (Place(v, lead, true, false, p, &y, &pt)) {
                g_origXform(v, p, y, pt);
                return;
            }
            if (++g_fallbacks <= 20)
                Logf("railreverse  car %p of pushing train %p: trail not available, drawn as vanilla this frame", v, lead);
        }
    }
    g_origXform(v, pos, yaw, pitch);
}

// ---------------------------------------------------------------- train window
static int g_latchTrain, g_latchFreight, g_latchPassenger;

// The game's fonts: every global the game passes as the font to a
// C3D_FONTMANAGER::Print* import (found at start). The indicator uses the one
// closest to indicator_font_scale x the default UI font, measured in game
// with C3D_FONT::GetSize, since the fonts exist only once the UI is built.
typedef float (*FnFontSize)(void* font);
static FnFontSize g_fontSize;
static void**     g_fonts[16];
static int        g_fontCount;
static void*      g_font;                 // chosen
static float      g_fontPx;               // its size

static float FontSize(void* f)
{
    if (!f || !g_fontSize || !ReadablePtr(f, 64)) return 0.0f;
    float s = g_fontSize(f);
    return s > 3.0f && s < 200.0f ? s : 0.0f;
}

static void PickFont(void)
{
    void* base = g_fontSlot ? *g_fontSlot : 0;
    float want = FontSize(base) * (float)cfg.fontScale / 100.0f;
    void* best = base;
    float bestPx = FontSize(base), bestErr = 1e9f;
    for (int i = 0; i < g_fontCount && want > 0.0f; ++i) {
        void* f = *g_fonts[i];
        float px = FontSize(f);
        if (!px) continue;
        float err = fabsf(px - want);
        if (err < bestErr) { bestErr = err; best = f; bestPx = px; }
    }
    if (best != g_font) {
        g_font = best;
        g_fontPx = bestPx;
        char list[256] = "";
        for (int i = 0, n = 0; i < g_fontCount; ++i)
            n += _snprintf_s(list + n, sizeof(list) - n, _TRUNCATE, " %.0f", FontSize(*g_fonts[i]));
        Logf("railreverse  indicator font %.0f px (default %.0f, scale %d%%; fonts:%s)",
             bestPx, FontSize(base), cfg.fontScale, list);
    }
}

// Text with a dark outline all round, readable on any background.
static void Text(float x, float y, unsigned long color, const wchar_t* s)
{
    if (!g_print || !g_font) return;
    static const float o[8][2] = { {-1,-1},{0,-1},{1,-1},{-1,0},{1,0},{-1,1},{0,1},{1,1} };
    for (int i = 0; i < 8; ++i)
        g_print(g_fontManager, g_font, x + o[i][0] * cfg.outline, y + o[i][1] * cfg.outline, 0xF0000000u, s);
    g_print(g_fontManager, g_font, x, y, color, s);
}

#define GREEN 0xFF9BE59Bu
#define GREY  0xFFD8D8D8u

#define ANCHOR_TOP_LEFT     0
#define ANCHOR_BOTTOM_LEFT  1
#define ANCHOR_TOP_RIGHT    2
#define ANCHOR_BOTTOM_RIGHT 3
static const char* kAnchors[] = { "top_left", "bottom_left", "top_right", "bottom_right" };

static float ConfigFloat(const char* key, float fallback)
{
    char s[32];
    if (!H->configString(RR_INI, "railreverse", key, s, sizeof(s), "") || !s[0]) return fallback;
    for (char* p = s; *p; ++p) if (*p == ',') *p = '.';                  // "0,6" works too
    return (float)atof(s);
}

static int ConfigAnchor(int fallback)
{
    char s[32];
    H->configString(RR_INI, "railreverse", "indicator_anchor", s, sizeof(s), kAnchors[fallback]);
    for (int i = 0; i < 4; ++i) if (!_stricmp(s, kAnchors[i])) return i;
    return fallback;
}

// The indicator settings are re-read every ~2 s, so its place and size can be
// tuned in railreverse.ini while the game runs.
static void ReloadIndicatorSettings(void)
{
    static DWORD last;
    DWORD now = GetTickCount();
    if (last && now - last < 2000) return;
    last = now;
    int   a = ConfigAnchor(cfg.anchor);
    float x = ConfigFloat("indicator_x", cfg.indicatorX);
    float y = ConfigFloat("indicator_y", cfg.indicatorY);
    int   s = H->configInt(RR_INI, "railreverse", "indicator_font_scale", cfg.fontScale);
    int   o = H->configInt(RR_INI, "railreverse", "indicator_outline", cfg.outline);
    if (a != cfg.anchor || x != cfg.indicatorX || y != cfg.indicatorY || s != cfg.fontScale || o != cfg.outline)
        Logf("railreverse  indicator: %s, x %.2f%%, y %.2f%%, font scale %d%%, outline %d", kAnchors[a], x, y, s, o);
    cfg.anchor = a; cfg.indicatorX = x; cfg.indicatorY = y; cfg.fontScale = s; cfg.outline = o;
    PickFont();
}

// The screen the UI is drawn on, in pixels: the game's own width/height
// (1.1.1.9 globals, also used by ForceNodes), else the game window's client area.
#define RVA_SCREEN_W 0x99528C
#define RVA_SCREEN_H 0x995274
static bool ScreenSize(float* w, float* h)
{
    int sw = 0, sh = 0;
    if (Rd(g_exeBase, RVA_SCREEN_W, &sw) && Rd(g_exeBase, RVA_SCREEN_H, &sh) &&
        sw >= 320 && sh >= 200 && sw <= 20000 && sh <= 20000) {
        *w = (float)sw; *h = (float)sh;
        return true;
    }
    RECT r;
    HWND wnd = GetForegroundWindow();
    DWORD pid = 0;
    GetWindowThreadProcessId(wnd, &pid);
    if (pid == GetCurrentProcessId() && GetClientRect(wnd, &r) && r.right > 0 && r.bottom > 0) {
        *w = (float)r.right; *h = (float)r.bottom;
        return true;
    }
    return false;
}

// The indicator is drawn in a corner of the screen after the game has drawn
// all its windows, not inside the train window: a window clips and scrolls
// what is drawn while it draws, and the text was cut off or scrolled away.
// The train window only records what to show; the window dispatcher's hook
// draws it if a train window was drawn in the same pass.
static struct { BYTE* lead; int chosen, pushing, turned; unsigned pass; } g_shown;
static unsigned g_pass;                   // windows drawn so far (dispatcher calls)

// Three lines:
//   Реверс: ВКЛ (вручную) · толкает   [CTRL+F9]
//   Все грузовые: вкл   [CTRL+SHIFT+F9]
//   Все пассажирские: выкл   [CTRL+ALT+F9]
static void Indicator(BYTE* lead, int chosen, int pushing, int turned)
{
    if (!cfg.indicator || !g_print) return;
    ReloadIndicatorSettings();
    if (!g_font) return;
    float scale = 1.0f;
    Rd(g_exeBase, RVA_UI_SCALE, &scale);
    if (!(scale > 0.25f && scale < 8.0f)) scale = 1.0f;
    float lineH = (g_fontPx > 0.0f ? g_fontPx * 1.25f : 18.0f * scale);
    float sw, sh;
    if (!ScreenSize(&sw, &sh)) return;

    BYTE** w = 0;
    bool light = Wagons(lead, &w) == 0;
    bool passenger = IsPassengerTrain(lead);
    bool classOn = passenger ? g_allPassenger != 0 : g_allFreight != 0;
    const wchar_t* why = chosen && classOn ? L" (вручную, и включены все)"
                       : chosen            ? L" (вручную)"
                       : classOn           ? (passenger ? L" (все пассажирские)" : L" (все грузовые)")
                       : L"";
    wchar_t lines[3][200], kT[40], kF[40], kP[40];
    swprintf(kT, 40, L"%hs", cfg.keyTrain.label);
    swprintf(kF, 40, L"%hs", cfg.keyFreight.label);
    swprintf(kP, 40, L"%hs", cfg.keyPassenger.label);
    // What is on the screen, from how the train is drawn: mirrored puts the
    // locomotive at the back, turned round faces its cab the other way, so
    // the cab faces the way of travel when both or neither apply.
    bool cabForward = (pushing != 0) == (turned != 0);
    const wchar_t* motion =
        light ? (cabForward ? L"" : L" · задним ходом")
              : (!pushing && !turned) ? L""
              : pushing ? (cabForward ? L" · толкает, кабиной вперёд" : L" · толкает, кабиной назад")
              : L" · тянет, кабиной назад";
    swprintf(lines[0], 200, L"Реверс: %ls%ls%ls   [%ls]", chosen || classOn ? L"ВКЛ" : L"выкл", why, motion, kT);
    swprintf(lines[1], 200, L"Все грузовые: %ls   [%ls]", g_allFreight ? L"вкл" : L"выкл", kF);
    swprintf(lines[2], 200, L"Все пассажирские: %ls   [%ls]", g_allPassenger ? L"вкл" : L"выкл", kP);
    unsigned long colors[3] = { chosen || classOn ? GREEN : GREY, g_allFreight ? GREEN : GREY,
                                g_allPassenger ? GREEN : GREY };

    // Place the block by its corner. The font has no width query here, so a
    // right-hand anchor uses an estimate of the longest line (~0.55 em a glyph).
    float blockH = 3.0f * lineH, blockW = 0.0f;
    for (int i = 0; i < 3; ++i) {
        float wl = (float)wcslen(lines[i]) * (g_fontPx > 0.0f ? g_fontPx : 17.0f) * 0.55f;
        if (wl > blockW) blockW = wl;
    }
    bool right  = cfg.anchor == ANCHOR_TOP_RIGHT || cfg.anchor == ANCHOR_BOTTOM_RIGHT;
    bool bottom = cfg.anchor == ANCHOR_BOTTOM_LEFT || cfg.anchor == ANCHOR_BOTTOM_RIGHT;
    float dx = sw * cfg.indicatorX / 100.0f, dy = sh * cfg.indicatorY / 100.0f;
    float x = right  ? sw - dx - blockW : dx;
    float y = bottom ? sh - dy - blockH : dy;
    if (x < 0.0f) x = 0.0f;
    if (y < 0.0f) y = 0.0f;

    for (int i = 0; i < 3; ++i) Text(x, y + i * lineH, colors[i], lines[i]);
}

static void TrainWindow(BYTE* ui, BYTE* window, FnPanel orig)
{
    BYTE* lead = 0;
    int kind = -1;
    if (window && Rd(window, W_KIND, &kind) && kind == WINDOW_VEHICLE && Rd(window, W_OBJECT, &lead) && lead) {
        BYTE* parent = 0;
        if (Rd(lead, V_PARENT, &parent) && parent) lead = parent;          // a wagon's window: its train
        if (!IsLoco(lead)) lead = 0;                                      // with or without wagons
    } else {
        lead = 0;
    }

    static BYTE* lastSeen;
    if (lead && lead != lastSeen) {
        lastSeen = lead;
        Logf("railreverse  train window: train %p (%s)", lead, IsPassengerTrain(lead) ? "passenger" : "freight");
    }

    int chosen = 0, pushing = 0, turned = 0;
    if (lead) {
        // The combinations share F9, so each is matched exactly (modifiers
        // included): CTRL+SHIFT+F9 never also counts as CTRL+F9.
        bool toggleTrain     = Pressed(&cfg.keyTrain, &g_latchTrain);
        bool toggleFreight   = Pressed(&cfg.keyFreight, &g_latchFreight);
        bool togglePassenger = Pressed(&cfg.keyPassenger, &g_latchPassenger);
        EnterCriticalSection(&g_lock);
        if (toggleFreight)   g_allFreight = !g_allFreight;
        if (togglePassenger) g_allPassenger = !g_allPassenger;
        Train* t = Current(lead, toggleTrain);
        if (t && toggleTrain) t->chosen = !t->chosen;
        if (t) { chosen = t->chosen; pushing = t->pushing; turned = t->turned; }
        LeaveCriticalSection(&g_lock);
        if (toggleTrain)     Logf("railreverse  train %p: reversing %s", lead, chosen ? "ON" : "off");
        if (toggleFreight)   Logf("railreverse  all freight trains: %s", g_allFreight ? "ON" : "off");
        if (togglePassenger) Logf("railreverse  all passenger trains: %s", g_allPassenger ? "ON" : "off");
    }

    orig(ui, window);
    if (lead) {
        g_shown.lead = lead; g_shown.chosen = chosen; g_shown.pushing = pushing; g_shown.turned = turned;
        g_shown.pass = g_pass;
    }
}

static void HookWindow(BYTE* ui, BYTE* window) { TrainWindow(ui, window, g_origWindow); }
static void HookPanel(BYTE* ui, BYTE* window)  { TrainWindow(ui, window, g_origPanel); }

// FUN_1406e9b80(ui): draws every open info window.
static void HookDispatch(BYTE* ui)
{
    unsigned pass = ++g_pass;
    g_origDispatch(ui);
    if (g_shown.lead && g_shown.pass == pass) Indicator(g_shown.lead, g_shown.chosen, g_shown.pushing, g_shown.turned);
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

// The font manager and the UI font, read off the game's own calls to
// C3D_FONTMANAGER::PrintLeftUnicodeNoArg: `lea rcx,[rip+manager]` and
// `mov rdx,[rip+font]` shortly before `call [rip+import]`.
static bool FindFont(void)
{
    void** slot = FindIatSlot(g_exe, DLL_ENGINE, EXP_PRINT);
    if (!slot) return false;
    g_print = (FnPrint)*slot;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(g_exeBase + ((IMAGE_DOS_HEADER*)g_exeBase)->e_lfanew);
    IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++s) {
        if (memcmp(s->Name, ".text", 5) != 0) continue;
        BYTE* p   = g_exeBase + s->VirtualAddress;
        BYTE* end = p + s->Misc.VirtualSize - 6;
        for (; p < end; ++p) {
            if (p[0] != 0xFF || p[1] != 0x15 || p + 6 + *(INT32*)(p + 2) != (BYTE*)slot) continue;
            BYTE *mgr = 0, *font = 0;
            for (BYTE* q = p - 1; q > p - 80; --q) {
                if (!mgr && q[0] == 0x48 && q[1] == 0x8d && q[2] == 0x0d) mgr = q + 7 + *(INT32*)(q + 3);
                if (!font && q[0] == 0x48 && q[1] == 0x8b && q[2] == 0x15) font = q + 7 + *(INT32*)(q + 3);
            }
            if (mgr && font) {
                g_fontManager = mgr;
                g_fontSlot = (void**)font;
                goto others;
            }
        }
    }
    return false;

others:
    // Every font global handed to any Print* import: the sizes to choose from.
    g_fontSize = (FnFontSize)GetProcAddress(g_engine, "?GetSize@C3D_FONT@@QEAAMXZ");
    static const char* prints[] = {
        EXP_PRINT,
        "?PrintLeftUnicode@C3D_FONTMANAGER@@QEAAXPEAVC3D_FONT@@MMKPEB_WZZ",
        "?PrintCenterUnicode@C3D_FONTMANAGER@@QEAAXPEAVC3D_FONT@@MMKPEB_WZZ",
        "?PrintRightUnicode@C3D_FONTMANAGER@@QEAAXPEAVC3D_FONT@@MMKPEB_WZZ",
    };
    void** islots[4];
    for (int k = 0; k < 4; ++k) islots[k] = FindIatSlot(g_exe, DLL_ENGINE, prints[k]);
    g_fonts[g_fontCount++] = g_fontSlot;
    s = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++s) {
        if (memcmp(s->Name, ".text", 5) != 0) continue;
        BYTE* p   = g_exeBase + s->VirtualAddress;
        BYTE* end = p + s->Misc.VirtualSize - 6;
        for (; p < end && g_fontCount < 16; ++p) {
            if (p[0] != 0xFF || p[1] != 0x15) continue;
            BYTE* tgt = p + 6 + *(INT32*)(p + 2);
            bool isPrint = false;
            for (int k = 0; k < 4; ++k) if (islots[k] && tgt == (BYTE*)islots[k]) isPrint = true;
            if (!isPrint) continue;
            for (BYTE* q = p - 1; q > p - 120; --q) {
                if (q[0] == 0x48 && q[1] == 0x8b && q[2] == 0x15) {
                    void** f = (void**)(q + 7 + *(INT32*)(q + 3));
                    bool known = false;
                    for (int j = 0; j < g_fontCount; ++j) if (g_fonts[j] == f) known = true;
                    if (!known) g_fonts[g_fontCount++] = f;
                    break;
                }
            }
        }
    }
    Logf("railreverse  %d font(s) found%s", g_fontCount, g_fontSize ? "" : ", C3D_FONT::GetSize missing: default font only");
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
    char s[64];
    cfg.allFreightDefault   = H->configInt(RR_INI, "railreverse", "all_freight", 0) != 0;
    cfg.allPassengerDefault = H->configInt(RR_INI, "railreverse", "all_passenger", 0) != 0;
    g_allFreight   = cfg.allFreightDefault;
    g_allPassenger = cfg.allPassengerDefault;
    cfg.pushKmh      = (float)H->configInt(RR_INI, "railreverse", "push_speed_kmh", 25);
    cfg.lightKmh     = (float)H->configInt(RR_INI, "railreverse", "light_engine_speed_kmh", 0);
    cfg.skipBothEnds = H->configInt(RR_INI, "railreverse", "skip_loco_both_ends", 1);
    cfg.logFlips     = H->configInt(RR_INI, "railreverse", "log_flips", 1);
    cfg.indicator    = H->configInt(RR_INI, "railreverse", "indicator", 1);
    cfg.anchor       = ConfigAnchor(ANCHOR_BOTTOM_LEFT);
    cfg.indicatorX   = ConfigFloat("indicator_x", 0.6f);
    cfg.indicatorY   = ConfigFloat("indicator_y", 0.0f);
    cfg.fontScale    = H->configInt(RR_INI, "railreverse", "indicator_font_scale", 140);
    cfg.outline      = H->configInt(RR_INI, "railreverse", "indicator_outline", 1);
    H->configString(RR_INI, "railreverse", "key_train", s, sizeof(s), "CTRL+F9");
    if (!ParseKey(s, &cfg.keyTrain)) { Logf("railreverse  key_train \"%s\" not understood, using CTRL+F9", s); ParseKey("CTRL+F9", &cfg.keyTrain); }
    H->configString(RR_INI, "railreverse", "key_all_freight", s, sizeof(s), "CTRL+SHIFT+F9");
    if (!ParseKey(s, &cfg.keyFreight)) { Logf("railreverse  key_all_freight \"%s\" not understood, using CTRL+SHIFT+F9", s); ParseKey("CTRL+SHIFT+F9", &cfg.keyFreight); }
    H->configString(RR_INI, "railreverse", "key_all_passenger", s, sizeof(s), "CTRL+ALT+F9");
    if (!ParseKey(s, &cfg.keyPassenger)) { Logf("railreverse  key_all_passenger \"%s\" not understood, using CTRL+ALT+F9", s); ParseKey("CTRL+ALT+F9", &cfg.keyPassenger); }
    Logf("railreverse  v%s init: all freight %d, all passenger %d, push speed %.0f km/h, keys %s / %s / %s",
         RR_VERSION, cfg.allFreightDefault, cfg.allPassengerDefault, cfg.pushKmh,
         cfg.keyTrain.label, cfg.keyFreight.label, cfg.keyPassenger.label);
    return 0;
}

extern "C" __declspec(dllexport) int TsmPluginStart(void)
{
    // The core (flip + placement) is resolved before anything is hooked: a
    // missing piece leaves the game untouched instead of half-patched.
    g_sample = (FnSample)FindUnique(SIG(SIG_SAMPLE));
    g_rotXY  = (FnRotXY)GetProcAddress(g_engine, EXP_ROTXY);
    if (!g_sample || !g_rotXY || !FindUnique(SIG(SIG_FLIP)) || !FindUnique(SIG(SIG_XFORM))) {
        Logf("railreverse  %s not found - nothing hooked",
             !g_sample ? "trail sampler" : !g_rotXY ? "C3DVec3GetRotationXY" : "flip or placement signature");
        return 1;
    }
    if (!Hook(SIG(SIG_XFORM), XFORM_STOLEN, (void*)HookXform, (void**)&g_origXform, "placement")) return 1;
    if (!Hook(SIG(SIG_FLIP), FLIP_STOLEN, (void*)HookFlip, (void**)&g_origFlip, "flip")) {
        Logf("railreverse  placement hook stays inert");    // nothing is ever marked pushing
        return 0;
    }

    // Everything below is optional and degrades on its own.
    bool window = Hook(SIG(SIG_VWINDOW), VPANEL_STOLEN, (void*)HookWindow, (void**)&g_origWindow, "train window");
    bool tabs   = Hook(SIG(SIG_VPANEL), VPANEL_STOLEN, (void*)HookPanel, (void**)&g_origPanel, "train window tabs 9-10");
    if (!window && !tabs) {
        Logf("railreverse  no train window: hotkeys unavailable, every train reverses");
        g_allFreight = g_allPassenger = cfg.allFreightDefault = cfg.allPassengerDefault = 1;
    }
    if (!FindFont()) {
        g_print = 0;
        Logf("railreverse  font not found - no indicator");
    } else if ((window || tabs) &&
               !Hook(SIG(SIG_DISPATCH), DISPATCH_STOLEN, (void*)HookDispatch, (void**)&g_origDispatch, "window dispatcher")) {
        g_print = 0;
        Logf("railreverse  window dispatcher not found - no indicator");
    }

    if (BYTE* at = FindUnique(SIG(SIG_VEHVEC)))
        g_vehicles = (BYTE***)(at + VEHVEC_NEXT_AT + *(INT32*)(at + VEHVEC_DISP_AT));
    if (!g_vehicles)
        Logf("railreverse  vehicle list not found - nothing remembered in saves");
    else if (!PatchIat(g_exe, DLL_STDIO, "fopen", (void*)HookFopen, (void**)&g_origFopen, "railreverse fopen"))
        Logf("railreverse  fopen import not patched - nothing remembered in saves");
    else
        Logf("railreverse  saves: vehicle list at +%06llX, state kept in " SIDECAR,
             (unsigned long long)((BYTE*)g_vehicles - g_exeBase));
    Logf("railreverse  ready");
    return 0;
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }

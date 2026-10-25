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
#include <shlwapi.h>

#define RR_VERSION "0.5.0"
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

// A distribution office (building type 0x34) hands a train wagons in groups in
// FUN_14068b330: train length (lead + wagons) against the length of its track,
// then `call FUN_1403e5a00(group wagon type)` for the room a group needs:
//   mov rcx,[r13|r15+0x1708]; call <room>; comiss xmm1,xmm0; ja <couple>; mov [rbp+0x368],sil|dil (full)
// The lead is at [rbp+0x340] there (the function's first argument, spilled).
// Both calls are redirected: a locomotive with its hub limit reached gets "no
// room", so the office treats the train as full.
static const short SIG_ROOM_A[] = {
    0x49,0x8b,0x8d,0x08,0x17,0x00,0x00, 0xe8,-1,-1,-1,-1, 0x0f,0x2f,0xc8, 0x77,0x13, 0x40,0x88,0xb5,0x68,0x03,0x00,0x00,
};
static const short SIG_ROOM_B[] = {
    0x49,0x8b,0x8f,0x08,0x17,0x00,0x00, 0xe8,-1,-1,-1,-1, 0x0f,0x2f,0xc8, 0x77,0x22, 0x40,0x88,0xbd,0x68,0x03,0x00,0x00,
};
#define ROOM_CALL_AT 7            // the e8 in both

// The highest hub limit the keys reach: ini hub_limit_max, 1 ... 255 (it is
// kept in 8 bits of a train's flags in railreverse.txt).
#define HUB_LIMIT_DEFAULT_MAX 20
#define HUB_LIMIT_CEILING     255

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
    float pushKmh;          // speed cap while pushing, 0 = none
    float lightKmh;         // speed cap for a locomotive running backwards on its own, 0 = none
    int   skipBothEnds;     // a locomotive at the far end too: leave the train alone
    int   hubLimitMax;      // the highest hub limit
    int   logFlips;
    int   indicator;        // draw the state above the train window
    int   anchor;           // screen corner: ANCHOR_*
    float indicatorX, indicatorY;   // offset from that corner, % of the screen width / height
    int   fontScale;        // indicator font, % of the default UI font
    int   outline;          // outline width, px
    int   windowButtons;    // the two buttons in the train window
    float buttonX, buttonY; // their place: centre of the upper one, game units (x 1 at 100 % UI scale) from the window's corner
    Key   keyTrain, keyLimitUp, keyLimitDown, keyLimitReset;
} cfg;

// A train pushes after a reversal if it was chosen in its window, or if every
// train of its class (freight / passenger) is switched on.
static volatile LONG g_allFreight, g_allPassenger;   // from the Scripts menu flags (ScriptFlags)
static int g_allForced;                 // no train window to switch anything in: every train reverses
static volatile LONG g_iconsStale;      // a save was loaded: the game let go of the textures it had
static volatile LONG g_flagsStale = 1;  // a save was loaded: the Scripts menu flags are read afresh

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
        else if (!_stricmp(tok, "MINUS")) k->vk = VK_OEM_MINUS;              // the -_ key
        else if (!_stricmp(tok, "PLUS") || !_stricmp(tok, "EQUALS")) k->vk = VK_OEM_PLUS;   // the =+ key
        else if (!_stricmp(tok, "NUMPADMINUS")) k->vk = VK_SUBTRACT;
        else if (!_stricmp(tok, "NUMPADPLUS")) k->vk = VK_ADD;
        else return false;
    }
    return k->vk != 0;
}

// A key's own name as the indicator shows it ("F9", "-", "+"; the game font
// has no U+2212, so the plain hyphen).
static void KeyName(const Key* k, wchar_t* out, size_t cap)
{
    if (k->vk >= VK_F1 && k->vk <= VK_F12) swprintf(out, cap, L"F%d", k->vk - VK_F1 + 1);
    else if (k->vk == VK_OEM_MINUS) swprintf(out, cap, L"-");
    else if (k->vk == VK_OEM_PLUS)  swprintf(out, cap, L"+");
    else if (k->vk == VK_SUBTRACT)  swprintf(out, cap, L"Num -");
    else if (k->vk == VK_ADD)       swprintf(out, cap, L"Num +");
    else swprintf(out, cap, L"%c", (wchar_t)k->vk);
}

// Its modifiers, joined by sep: "Ctrl+Shift+" or "Ctrl Shift ".
static void KeyMods(const Key* k, const wchar_t* sep, wchar_t* out, size_t cap)
{
    swprintf(out, cap, L"%ls%ls%ls%ls%ls%ls", k->ctrl ? L"Ctrl" : L"", k->ctrl ? sep : L"",
             k->shift ? L"Shift" : L"", k->shift ? sep : L"", k->alt ? L"Alt" : L"", k->alt ? sep : L"");
}

// "Ctrl+F9" for the F-keys and the like; "Ctrl Shift -" where a "+" in the
// name would read as a separator.
static void KeyText(const Key* k, wchar_t* out, size_t cap)
{
    wchar_t mods[32], name[16];
    bool sym = k->vk == VK_OEM_MINUS || k->vk == VK_OEM_PLUS || k->vk == VK_SUBTRACT || k->vk == VK_ADD;
    KeyMods(k, sym ? L" " : L"+", mods, 32);
    KeyName(k, name, 16);
    swprintf(out, cap, L"%ls%ls", mods, name);
}

static bool SameMods(const Key* a, const Key* b)
{
    return a->ctrl == b->ctrl && a->shift == b->shift && a->alt == b->alt;
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
               float last[5]; int lastTurn;        // the last setTransform arguments
               int hubLimit; };                     // most wagons a distribution office hands it, 0 = no limit
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
        if (g_trains[i].lead && (g_trains[i].pushing || g_trains[i].chosen || g_trains[i].hubLimit)) keep[n++] = g_trains[i];
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

static bool ClassOn(BYTE* lead) { return g_allForced || (IsPassengerTrain(lead) ? g_allPassenger != 0 : g_allFreight != 0); }

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

// ---------------------------------------------------------------- distribution offices
// Hub limit. The office's "room for one more group" (FUN_1403e5a00), asked
// for this lead; "no room" once it already has its limit.
typedef float (*FnRoom)(BYTE* type);
static FnRoom g_room;

// group: the cut of wagons the office is about to couple - a stored vehicle
// heading its own wagons (r13 at the first call site, r15 at the second; both
// are what the office passes to FUN_14067e780 just before). It goes on whole,
// so the limit counts it in full; a train still without wagons always gets
// its first group, or it would leave empty.
extern "C" float RR_Room(BYTE* type, BYTE* lead, BYTE* group)
{
    float room = g_room(type);
    if (!lead || !g_trainCount) return room;
    int limit = 0;
    EnterCriticalSection(&g_lock);
    Train* t = Find(lead);
    if (t && t->type == *(BYTE**)(lead + V_TYPE)) limit = t->hubLimit;
    LeaveCriticalSection(&g_lock);
    if (limit <= 0) return room;
    INT_PTR wagons = (*(BYTE**)(lead + V_WAGONS_END) - *(BYTE**)(lead + V_WAGONS_BEGIN)) / 8;
    BYTE** gw = 0;
    int n = group ? Wagons(group, &gw) : -1;
    int groupSize = n >= 0 ? n + 1 : 1;
    if (wagons == 0 || wagons + groupSize <= limit) return room;
    static BYTE* lastFull;
    if (lastFull != lead) {
        lastFull = lead;
        Logf("railreverse  train %p: %d wagons + a group of %d is over its hub limit (%d): the office hands it no more",
             lead, (int)wagons, groupSize, limit);
    }
    return 1e30f;
}

// Between the office's call and RR_Room. The office keeps values in xmm1 and
// other volatile registers across this call (the compiler knew the callee),
// so all of them are saved; the lead is read off the office's frame
// ([rbp+0x340], FUN_14068b330's first argument), the group from GROUPREG.
#define ROOM_THUNK(NAME, GROUPREG)                         \
extern "C" __attribute__((naked)) void NAME(void)          \
{                                                          \
    __asm__ volatile(                                      \
        "sub $0xa8, %rsp\n\t"                              \
        "movdqu %xmm1, 0x20(%rsp)\n\t"                     \
        "movdqu %xmm2, 0x30(%rsp)\n\t"                     \
        "movdqu %xmm3, 0x40(%rsp)\n\t"                     \
        "movdqu %xmm4, 0x50(%rsp)\n\t"                     \
        "movdqu %xmm5, 0x60(%rsp)\n\t"                     \
        "mov %rcx, 0x70(%rsp)\n\t"                         \
        "mov %rdx, 0x78(%rsp)\n\t"                         \
        "mov %r8, 0x80(%rsp)\n\t"                          \
        "mov %r9, 0x88(%rsp)\n\t"                          \
        "mov %r10, 0x90(%rsp)\n\t"                         \
        "mov %r11, 0x98(%rsp)\n\t"                         \
        "mov 0x340(%rbp), %rdx\n\t"                        \
        "mov %" GROUPREG ", %r8\n\t"                       \
        "call RR_Room\n\t"                                 \
        "movdqu 0x20(%rsp), %xmm1\n\t"                     \
        "movdqu 0x30(%rsp), %xmm2\n\t"                     \
        "movdqu 0x40(%rsp), %xmm3\n\t"                     \
        "movdqu 0x50(%rsp), %xmm4\n\t"                     \
        "movdqu 0x60(%rsp), %xmm5\n\t"                     \
        "mov 0x70(%rsp), %rcx\n\t"                         \
        "mov 0x78(%rsp), %rdx\n\t"                         \
        "mov 0x80(%rsp), %r8\n\t"                          \
        "mov 0x88(%rsp), %r9\n\t"                          \
        "mov 0x90(%rsp), %r10\n\t"                         \
        "mov 0x98(%rsp), %r11\n\t"                         \
        "add $0xa8, %rsp\n\t"                              \
        "ret\n\t");                                        \
}
ROOM_THUNK(RR_RoomThunkA, "r13")
ROOM_THUNK(RR_RoomThunkB, "r15")

// ---------------------------------------------------------------- saves
// railreverse.txt beside vehicles.bin:
//   freight 0|1
//   passenger 0|1
//   <index in the vehicle list> <flags: 1 pushing, 2 chosen, 4 locomotive turned round, hub limit << 8> <type folder>
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
    int len = 0;
    int trains = 0;
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < n && trains < 512; ++i) {
        BYTE* v = veh[i];
        BYTE* parent = 0;
        if (!v || !Rd(v, V_PARENT, &parent) || parent) continue;
        Train* t = Find(v);
        if (!t || (!t->pushing && !t->chosen && !t->hubLimit)) continue;
        BYTE* type; INT_PTR wagons;
        if (!Fingerprint(v, &type, &wagons) || type != t->type) continue;
        int pushing = t->pushing && wagons == t->wagons;                  // consist changed: saved as pulling
        char name[TYPE_NAME_MAX];
        TypeName(v, name);
        len += _snprintf_s(buf + len, sizeof(buf) - len, _TRUNCATE, "%d %d %s\n", i,
                           (pushing ? 1 : 0) | (t->chosen ? 2 : 0) | (t->turned && pushing == t->pushing ? 4 : 0) | (t->hubLimit << 8), name);
        ++trains;
    }
    LeaveCriticalSection(&g_lock);
    if (!trains) {
        DeleteFileA(path);                // nothing to remember: no stale file for the next load
        Logf("railreverse  save: nothing to remember");
        return;
    }
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE) { Logf("railreverse  save: cannot write %s", path); return; }
    DWORD wrote = 0;
    WriteFile(h, buf, (DWORD)len, &wrote, 0);
    CloseHandle(h);
    Logf("railreverse  save: %d train(s) -> %s", trains, path);
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
    g_allFreight = g_allPassenger = 0;               // until the Scripts menu flags are read
    g_iconsStale = 1;
    g_flagsStale = 1;
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
        if (!strncmp(line, "freight ", 8) || !strncmp(line, "passenger ", 10)) {
            // 0.4: the class switches were kept here; now the Scripts menu has them
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
    Logf("railreverse  load: %d train(s) to restore from %s", g_pendingCount, path);
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
        t->hubLimit = (p->flags >> 8) & 0xFF;
        if (t->hubLimit > cfg.hubLimitMax) t->hubLimit = cfg.hubLimitMax;
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
static int g_latchTrain, g_latchUp, g_latchDown, g_latchReset;

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

// ---------------------------------------------------------------- language
// The indicator speaks Russian when the game does, English otherwise.
struct Ui {
    const wchar_t *whyBoth, *whyManual, *whyPassenger, *whyFreight;
    const wchar_t *backward, *pushCabFwd, *pushCabBack, *pullCabBack;
    const wchar_t *reverse, *ON, *OFF, *allFreight, *allPassenger, *on, *off;
    const wchar_t *hubLimit, *hubNoLimit;
};
static const Ui kRussian = {
    L" (вручную, и включены все)", L" (вручную)", L" (все пассажирские)", L" (все грузовые)",
    L" · задним ходом", L" · толкает, кабиной вперёд", L" · толкает, кабиной назад", L" · тянет, кабиной назад",
    L"Реверс: %ls%ls%ls   [%ls]", L"ВКЛ", L"выкл",
    L"Все грузовые: %ls   [%ls]", L"Все пассажирские: %ls   [%ls]", L"вкл", L"выкл",
    L"Вагонов из хаба: не больше %d   (%ls, сброс: %ls)", L"Вагонов из хаба: без ограничения   (%ls, сброс: %ls)",
};
static const Ui kEnglish = {
    L" (this train, and all of its class)", L" (this train)", L" (all passenger trains)", L" (all freight trains)",
    L" · running backwards", L" · pushing, cab first", L" · pushing, cab last", L" · pulling, cab last",
    L"Reverse: %ls%ls%ls   [%ls]", L"ON", L"off",
    L"All freight trains: %ls   [%ls]", L"All passenger trains: %ls   [%ls]", L"on", L"off",
    L"Wagons from a hub: at most %d   (%ls, reset: %ls)", L"Wagons from a hub: no limit   (%ls, reset: %ls)",
};
static const Ui* T = &kRussian;

// railreverse.ini `language`: ru, en, or auto - the game's own choice, read
// from media_soviet/config.ini ($TEXT LANGUAGE2 sovietRussian.btf, or "auto",
// which the game resolves to the Windows language).
static void PickLanguage(void)
{
    char want[16];
    H->configString(RR_INI, "railreverse", "language", want, sizeof(want), "auto");
    bool ru;
    const char* why;
    if (!_stricmp(want, "ru"))      { ru = true;  why = "ini"; }
    else if (!_stricmp(want, "en")) { ru = false; why = "ini"; }
    else {
        char path[MAX_PATH], line[256], value[128] = "";
        DWORD n = GetModuleFileNameA(g_exe, path, MAX_PATH);
        while (n && path[n - 1] != '\\' && path[n - 1] != '/') --n;
        path[n] = 0;
        strncat(path, "media_soviet\\config.ini", MAX_PATH - n - 1);
        if (FILE* f = fopen(path, "r")) {
            while (fgets(line, sizeof(line), f))
                if (sscanf(line, " $TEXT LANGUAGE2 %127s", value) == 1) break;
            fclose(f);
        }
        if (value[0] && _stricmp(value, "auto") != 0) {
            ru = StrStrIA(value, "Russian") != 0;
            why = value;
        } else {
            ru = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_RUSSIAN;
            why = "Windows";
        }
    }
    T = ru ? &kRussian : &kEnglish;
    Logf("railreverse  indicator language: %s (%s)", ru ? "Russian" : "English", why);
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
    float bx = ConfigFloat("window_button_x", cfg.buttonX), by = ConfigFloat("window_button_y", cfg.buttonY);
    if (bx != cfg.buttonX || by != cfg.buttonY) Logf("railreverse  train window buttons at %.0f, %.0f", bx, by);
    cfg.buttonX = bx; cfg.buttonY = by;
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
static struct { BYTE* lead; int chosen, pushing, turned, limit; unsigned pass; } g_shown;
static unsigned g_pass;                   // windows drawn so far (dispatcher calls)

// Three lines:
//   Реверс: ВКЛ (вручную) · толкает   [CTRL+F9]
//   Все грузовые: вкл   [CTRL+SHIFT+F9]
//   Все пассажирские: выкл   [CTRL+ALT+F9]
static void Indicator(BYTE* lead, int chosen, int pushing, int turned, int limit)
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
    const wchar_t* why = chosen && classOn ? T->whyBoth
                       : chosen            ? T->whyManual
                       : classOn           ? (passenger ? T->whyPassenger : T->whyFreight)
                       : L"";
    wchar_t lines[4][200], kT[40], kF[40], kP[40], kU[40], kD[40], kR[40];
    KeyText(&cfg.keyLimitUp, kU, 40);
    KeyText(&cfg.keyLimitDown, kD, 40);
    KeyText(&cfg.keyLimitReset, kR, 40);
    KeyText(&cfg.keyTrain, kT, 40);
    wcscpy(kF, T == &kRussian ? L"меню Скрипты" : L"Scripts menu");
    wcscpy(kP, kF);
    // What is on the screen, from how the train is drawn: mirrored puts the
    // locomotive at the back, turned round faces its cab the other way, so
    // the cab faces the way of travel when both or neither apply.
    bool cabForward = (pushing != 0) == (turned != 0);
    const wchar_t* motion =
        light ? (cabForward ? L"" : T->backward)
              : (!pushing && !turned) ? L""
              : pushing ? (cabForward ? T->pushCabFwd : T->pushCabBack)
              : T->pullCabBack;
    swprintf(lines[0], 200, T->reverse, chosen || classOn ? T->ON : T->OFF, why, motion, kT);
    swprintf(lines[1], 200, T->allFreight, g_allFreight ? T->on : T->off, kF);
    swprintf(lines[2], 200, T->allPassenger, g_allPassenger ? T->on : T->off, kP);
    // "(Ctrl -/+, сброс: Ctrl Shift -)"; the two step keys share one prefix
    // when their modifiers are the same.
    wchar_t steps[64];
    if (SameMods(&cfg.keyLimitDown, &cfg.keyLimitUp)) {
        wchar_t mods[32], dn[16], up[16];
        bool sym = cfg.keyLimitDown.vk == VK_OEM_MINUS || cfg.keyLimitDown.vk == VK_SUBTRACT;
        KeyMods(&cfg.keyLimitDown, sym ? L" " : L"+", mods, 32);
        KeyName(&cfg.keyLimitDown, dn, 16);
        KeyName(&cfg.keyLimitUp, up, 16);
        swprintf(steps, 64, L"%ls%ls/%ls", mods, dn, up);
    } else {
        swprintf(steps, 64, L"%ls / %ls", kD, kU);
    }
    if (limit) swprintf(lines[3], 200, T->hubLimit, limit, steps, kR);
    else       swprintf(lines[3], 200, T->hubNoLimit, steps, kR);
    unsigned long colors[4] = { chosen || classOn ? GREEN : GREY, g_allFreight ? GREEN : GREY,
                                g_allPassenger ? GREEN : GREY, limit ? GREEN : GREY };

    // Place the block by its corner. The font has no width query here, so a
    // right-hand anchor uses an estimate of the longest line (~0.55 em a glyph).
    float blockH = 4.0f * lineH, blockW = 0.0f;
    for (int i = 0; i < 4; ++i) {
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

    for (int i = 0; i < 4; ++i) Text(x, y + i * lineH, colors[i], lines[i]);
}

// ---------------------------------------------------------------- train window buttons
// Two buttons below the game's signal button, drawn and clicked the way the
// game does its own (same panel, so they are clipped and scrolled with the
// window): reversing for this train (as key_train) and the most wagons a
// supply base hands it (left click +1, right click -1, round in a ring like
// the keys). The icons sit next to the dll.
#define RVA_INPUT      0xA54B90   // C3D_INPUT
#define RVA_CLICK      0xA54E91   // u8: a left click this frame, cleared by whoever takes it
#define RVA_RCLICK     0xA54E93   // u8: a right click (0xA54E90/91 left down/click, 92/93 right)
#define RVA_PANEL      0x9BE060   // the C3D_PANEL2D the windows draw their buttons with
#define RVA_PANEL_SIZE 0x9BE2E8   // float w, h
#define RVA_PANEL_POS  0x9BE2F0   // float x, y; then a dword 0
#define RVA_PANEL_RGBA 0x9BE30C   // float r, g, b, a
#define RVA_SAMPLER    0x9EAD08   // what a texture is bound with
#define RVA_MIDDLE     0x9EACD0   // C3D_MIDDLEPOINT: makes textures
#define UI_TOOLTIP     0xD5A0     // ui: wchar_t tooltip of what is under the mouse
#define W_ACTIVE       0x20       // u8: the window takes the mouse
#define EXP_MOUSE      "?GetMouseSolid@C3D_INPUT@@QEAA?AVC3DVECTOR3@@XZ"
#define EXP_TEXTURE    "?CreateManagedTexture@C3D_MIDDLEPOINT@@QEAAPEAVC3DAPI_TEXTURE@@PEBD@Z"
#define EXP_DRAW       "?Draw@C3D_PANEL2D@@QEAAXMMMMM_N@Z"
typedef float* (*FnMouse)(void* input, float* out);
typedef void* (*FnTexture)(void* middle, const char* name);
typedef int (*FnLoad)(void* tex, const char* path, int w, int h, unsigned mips, int format);
typedef void (*FnBind)(void* tex, int slot, void* sampler);
typedef void (*FnDraw)(void* panel, float u0, float v0, float u1, float v1, float rot, bool blend);
static FnMouse   g_mouse;
static FnTexture g_texture;
static FnDraw    g_panelDraw;
#define ICONS 4
static void*     g_icons[ICONS];   // push off, on; limit off, on
#define ICON_PUSH  0
#define ICON_LIMIT 2

// Loaded the way the game loads its buttons, by a path relative to media_soviet.
static bool LoadIcons(void)
{
    static int tried;
    if (InterlockedExchange(&g_iconsStale, 0)) {
        tried = 0;
        for (int i = 0; i < ICONS; ++i) g_icons[i] = 0;
    }
    if (tried++) {
        for (int i = 0; i < ICONS; ++i) if (!g_icons[i]) return false;
        return true;
    }
    char dir[MAX_PATH] = "";
    HMODULE self = 0;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&LoadIcons, &self);
    DWORD n = GetModuleFileNameA(self, dir, MAX_PATH);
    while (n && dir[n - 1] != '\\' && dir[n - 1] != '/') --n;
    dir[n] = 0;
    const char* names[ICONS] = { "button_push_off.png", "button_push_on.png", "button_limit_off.png", "button_limit_on.png" };
    for (int i = 0; i < ICONS; ++i) {
        char path[MAX_PATH], full[MAX_PATH], media[MAX_PATH];
        _snprintf_s(full, sizeof(full), _TRUNCATE, "%s%s", dir, names[i]);
        DWORD m = GetModuleFileNameA(0, media, MAX_PATH);
        while (m && media[m - 1] != '\\') --m;
        _snprintf_s(media + m, sizeof(media) - m, _TRUNCATE, "media_soviet");
        if (!PathRelativePathToA(path, media, FILE_ATTRIBUTE_DIRECTORY, full, 0)) strcpy(path, full);
        for (char* c = path; *c; ++c) if (*c == '\\') *c = '/';
        if (path[0] == '.' && path[1] == '/') memmove(path, path + 2, strlen(path + 2) + 1);
        void* tex = g_texture(g_exeBase + RVA_MIDDLE, path);
        int err = tex ? ((FnLoad)(*(void***)tex)[2])(tex, path, 0, 0, 1, 0) : -1;   // 0: loaded (the game checks != 0)
        if (err) Logf("railreverse  icon %s not loaded (%d)", path, err);
        g_icons[i] = err ? 0 : tex;
    }
    for (int i = 0; i < ICONS; ++i) if (!g_icons[i]) return false;
    return true;
}

static void DrawIcon(void* tex, float x, float y, float size, float alpha)
{
    ((FnBind)(*(void***)tex)[0x70 / 8])(tex, 0, *(void**)(g_exeBase + RVA_SAMPLER));
    float* sz = (float*)(g_exeBase + RVA_PANEL_SIZE);
    float* at = (float*)(g_exeBase + RVA_PANEL_POS);
    float* rgba = (float*)(g_exeBase + RVA_PANEL_RGBA);
    sz[0] = sz[1] = size;
    at[0] = x; at[1] = y; *(int*)(at + 2) = 0;
    rgba[0] = rgba[1] = rgba[2] = 1.0f;
    rgba[3] = alpha;
    g_panelDraw(g_exeBase + RVA_PANEL, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, true);
}

static bool Hover(BYTE* window, float x, float y, float size)
{
    float m[3] = { 0, 0, 0 };
    g_mouse(g_exeBase + RVA_INPUT, m);
    return *(window + W_ACTIVE) && fabsf(m[0] - x) <= size / 2 && fabsf(m[1] - y) <= size / 2;
}

static void Tooltip(BYTE* ui, const wchar_t* tip)
{
    wcsncpy((wchar_t*)(ui + UI_TOOLTIP), tip, 160);
    ((wchar_t*)(ui + UI_TOOLTIP))[159] = 0;
}

// The wagon limit one step up or down, round in a ring: no limit -> 1 -> ...
// -> hub_limit_max -> no limit (keys and the button alike).
static int StepLimit(int limit, int step)
{
    if (step > 0) return limit >= cfg.hubLimitMax ? 0 : limit + 1;
    return limit <= 0 ? cfg.hubLimitMax : limit - 1;
}

static void WindowButtons(BYTE* ui, BYTE* window, BYTE* lead)
{
    if (!LoadIcons()) return;
    ReloadIndicatorSettings();
    float sc = 1.0f, wx = 0, wy = 0, ox = 0, oy = 0;
    Rd(g_exeBase, RVA_UI_SCALE, &sc);
    if (!(sc > 0.25f && sc < 8.0f)) sc = 1.0f;
    if (!Rd(window, W_POS_X, &wx) || !Rd(window, W_POS_Y, &wy) || !Rd(window, W_OFF_X, &ox) || !Rd(window, W_OFF_Y, &oy)) return;
    float size = sc * 50.0f, x = wx + ox + sc * cfg.buttonX, y1 = wy + oy + sc * cfg.buttonY, y2 = y1 + sc * 52.0f;
    bool ru = T == &kRussian;

    EnterCriticalSection(&g_lock);
    Train* t = Current(lead, false);
    int chosen = t ? t->chosen : 0, limit = t ? t->hubLimit : 0;
    LeaveCriticalSection(&g_lock);

    // green while this train reverses: its own switch, or all trains of its class (Scripts menu)
    bool passenger = IsPassengerTrain(lead);
    bool classOn = passenger ? g_allPassenger != 0 : g_allFreight != 0;
    bool h1 = Hover(window, x, y1, size), h2 = Hover(window, x, y2, size);
    DrawIcon(g_icons[ICON_PUSH + (chosen || classOn ? 1 : 0)], x, y1, size, h1 ? 1.0f : 0.8f);
    DrawIcon(g_icons[ICON_LIMIT + (limit ? 1 : 0)], x, y2, size, h2 ? 1.0f : 0.8f);
    if (limit && g_font) {
        wchar_t n[8];
        _snwprintf_s(n, 8, _TRUNCATE, L"%d", limit);
        float tw = (float)wcslen(n) * g_fontPx * 0.55f;
        Text(x - tw / 2, y2 - size * 0.42f, 0xFF289628u, n);
    }
    if (!h1 && !h2) return;

    BYTE* click = g_exeBase + RVA_CLICK;
    BYTE* rclick = g_exeBase + RVA_RCLICK;
    wchar_t tip[160];
    if (h1) {
        wchar_t kTrain[40];
        KeyText(&cfg.keyTrain, kTrain, 40);
        _snwprintf_s(tip, 160, _TRUNCATE,
                     ru ? L"Реверс выбранного поезда: %ls\r\nЛокомотив толкает вагоны задним ходом.\r\nНажмите, чтобы %ls (%ls)\r\nВсе %ls: %ls (меню Скрипты)"
                        : L"Reversing for the selected train: %ls\r\nThe locomotive pushes its wagons backwards.\r\nClick to switch %ls (%ls)\r\nAll %ls trains: %ls (Scripts menu)",
                     chosen ? (ru ? L"ВКЛ." : L"ON") : (ru ? L"выкл." : L"off"),
                     chosen ? (ru ? L"выключить" : L"off") : (ru ? L"включить" : L"on"), kTrain,
                     passenger ? (ru ? L"пассажирские" : L"passenger") : (ru ? L"грузовые" : L"freight"),
                     classOn ? (ru ? L"вкл." : L"on") : (ru ? L"выкл." : L"off"));
        Tooltip(ui, tip);
        if (*click) {
            *click = 0;                                    // taken: nothing under the window gets it
            EnterCriticalSection(&g_lock);
            if (Train* c = Current(lead, true)) chosen = c->chosen = !c->chosen;
            LeaveCriticalSection(&g_lock);
            Logf("railreverse  train %p: reversing %s (button)", lead, chosen ? "ON" : "off");
        }
        return;
    }
    wchar_t now[40], kReset[40];
    KeyText(&cfg.keyLimitReset, kReset, 40);
    if (limit) _snwprintf_s(now, 40, _TRUNCATE, ru ? L"не больше %d" : L"at most %d", limit);
    else wcscpy(now, ru ? L"без ограничения" : L"no limit");
    _snwprintf_s(tip, 160, _TRUNCATE,
                 ru ? L"Вагоны с базы снабжения: %ls\r\nЛевый клик +1, правый -1\r\nСброс на «без ограничения»: %ls"
                    : L"Wagons from a supply base: %ls\r\nLeft click +1, right click -1\r\nBack to no limit: %ls",
                 now, kReset);
    Tooltip(ui, tip);
    int step = *click ? 1 : *rclick ? -1 : 0;
    if (!step) return;
    *click = 0;
    *rclick = 0;
    EnterCriticalSection(&g_lock);
    if (Train* c = Current(lead, true)) {
        c->hubLimit = StepLimit(c->hubLimit, step);
        limit = c->hubLimit;
    }
    LeaveCriticalSection(&g_lock);
    Logf("railreverse  train %p: hub limit %d%s (button)", lead, limit, limit ? " wagons" : " (none)");
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

    int chosen = 0, pushing = 0, turned = 0, limit = 0;
    if (lead) {
        // The combinations share keys, so each is matched exactly (modifiers
        // included): CTRL+SHIFT+F9 never also counts as CTRL+F9.
        bool toggleTrain     = Pressed(&cfg.keyTrain, &g_latchTrain);
        bool limitUp         = Pressed(&cfg.keyLimitUp, &g_latchUp);
        bool limitDown       = Pressed(&cfg.keyLimitDown, &g_latchDown);
        bool limitReset      = Pressed(&cfg.keyLimitReset, &g_latchReset);
        EnterCriticalSection(&g_lock);
        Train* t = Current(lead, toggleTrain || limitUp || limitDown || limitReset);
        if (t && toggleTrain) t->chosen = !t->chosen;
        if (t && limitUp)   t->hubLimit = StepLimit(t->hubLimit, 1);
        if (t && limitDown) t->hubLimit = StepLimit(t->hubLimit, -1);
        if (t && limitReset) t->hubLimit = 0;                              // "no limit"
        if (t) { chosen = t->chosen; pushing = t->pushing; turned = t->turned; limit = t->hubLimit; }
        LeaveCriticalSection(&g_lock);
        if (toggleTrain)     Logf("railreverse  train %p: reversing %s", lead, chosen ? "ON" : "off");
        if (limitUp || limitDown || limitReset) Logf("railreverse  train %p: hub limit %d%s", lead, limit, limit ? " wagons" : " (none)");
    }

    orig(ui, window);
    if (lead && cfg.windowButtons) WindowButtons(ui, window, lead);
    if (lead) {
        g_shown.lead = lead; g_shown.chosen = chosen; g_shown.pushing = pushing; g_shown.turned = turned;
        g_shown.limit = limit;
        g_shown.pass = g_pass;
    }
}

static void HookWindow(BYTE* ui, BYTE* window) { TrainWindow(ui, window, g_origWindow); }
static void HookPanel(BYTE* ui, BYTE* window)  { TrainWindow(ui, window, g_origPanel); }

// ---------------------------------------------------------------- Scripts menu
// "All freight" / "all passenger" are switched only in the Scripts menu
// (Options -> Scripts): its two buttons of this mod, RR_AllFreight /
// RR_AllPassenger, start or stop a script that only sleeps,
// RR_AllFreightOn.txt / RR_AllPassengerOn.txt; a class reverses while its
// flag runs. Running scripts are kept in the save (runningScripts.bin,
// FUN_14059a4a0): world +0x13BB8, a vector of script*, each with two wide
// strings at *(+0xB8) and *(+0xC0) (its file and name, FUN_140598890).
#define RVA_WORLD        0x9D4F10
#define WORLD_SCRIPTS    0x13BB8
#define FLAG_FREIGHT     L"RR_AllFreightOn"
#define FLAG_PASSENGER   L"RR_AllPassengerOn"

static bool ScriptNamed(BYTE* script, const wchar_t* name)
{
    static const size_t at[2] = { 0xB8, 0xC0 };
    for (int i = 0; i < 2; ++i) {
        wchar_t* p = 0;
        wchar_t buf[200];
        size_t n = 0;
        if (!Rd(script, at[i], &p) || !p) continue;
        while (n + 1 < 200 && ReadablePtr(p + n, 2) && p[n]) buf[n] = p[n], ++n;
        buf[n] = 0;
        if (wcsstr(buf, name)) return true;
    }
    return false;
}

static void ScriptFlags(void)
{
    static DWORD last;
    static int freightWas = -1, passengerWas = -1;      // -1: not logged since the load
    DWORD now = GetTickCount();
    if (now - last < 500) return;
    last = now;
    if (InterlockedExchange(&g_flagsStale, 0)) freightWas = passengerWas = -1;
    BYTE *b = 0, *e = 0;
    BYTE* world = g_exeBase + RVA_WORLD;
    if (!Rd(world, WORLD_SCRIPTS, &b) || !Rd(world, WORLD_SCRIPTS + 8, &e) || (b && e < b) || e - b > 8 * 4096) return;
    int n = b ? (int)((e - b) / 8) : 0;
    if (n && !ReadablePtr(b, (size_t)n * 8)) return;
    int freight = 0, passenger = 0;
    for (int i = 0; i < n; ++i) {
        BYTE* sc = ((BYTE**)b)[i];
        if (!sc) continue;
        if (ScriptNamed(sc, FLAG_FREIGHT)) freight = 1;
        else if (ScriptNamed(sc, FLAG_PASSENGER)) passenger = 1;
    }
    g_allFreight = freight;
    g_allPassenger = passenger;
    if (freight != freightWas) Logf("railreverse  all freight trains: %s (Scripts menu)", freight ? "ON" : "off");
    if (passenger != passengerWas) Logf("railreverse  all passenger trains: %s (Scripts menu)", passenger ? "ON" : "off");
    freightWas = freight;
    passengerWas = passenger;
}

// FUN_1406e9b80(ui): draws every open info window.
static void HookDispatch(BYTE* ui)
{
    ScriptFlags();
    unsigned pass = ++g_pass;
    g_origDispatch(ui);
    if (g_shown.lead && g_shown.pass == pass) Indicator(g_shown.lead, g_shown.chosen, g_shown.pushing, g_shown.turned, g_shown.limit);
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
    cfg.pushKmh      = (float)H->configInt(RR_INI, "railreverse", "push_speed_kmh", 25);
    cfg.lightKmh     = (float)H->configInt(RR_INI, "railreverse", "light_engine_speed_kmh", 0);
    cfg.skipBothEnds = H->configInt(RR_INI, "railreverse", "skip_loco_both_ends", 1);
    cfg.hubLimitMax  = H->configInt(RR_INI, "railreverse", "hub_limit_max", HUB_LIMIT_DEFAULT_MAX);
    if (cfg.hubLimitMax < 1) cfg.hubLimitMax = 1;
    if (cfg.hubLimitMax > HUB_LIMIT_CEILING) cfg.hubLimitMax = HUB_LIMIT_CEILING;
    cfg.logFlips     = H->configInt(RR_INI, "railreverse", "log_flips", 1);
    cfg.indicator    = H->configInt(RR_INI, "railreverse", "indicator", 0);
    cfg.windowButtons = H->configInt(RR_INI, "railreverse", "window_buttons", 1);
    cfg.buttonX      = ConfigFloat("window_button_x", 570.0f);
    cfg.buttonY      = ConfigFloat("window_button_y", 24.0f);
    cfg.anchor       = ConfigAnchor(ANCHOR_BOTTOM_LEFT);
    cfg.indicatorX   = ConfigFloat("indicator_x", 0.6f);
    cfg.indicatorY   = ConfigFloat("indicator_y", 0.0f);
    cfg.fontScale    = H->configInt(RR_INI, "railreverse", "indicator_font_scale", 140);
    PickLanguage();
    cfg.outline      = H->configInt(RR_INI, "railreverse", "indicator_outline", 1);
    H->configString(RR_INI, "railreverse", "key_train", s, sizeof(s), "CTRL+F9");
    if (!ParseKey(s, &cfg.keyTrain)) { Logf("railreverse  key_train \"%s\" not understood, using CTRL+F9", s); ParseKey("CTRL+F9", &cfg.keyTrain); }
    H->configString(RR_INI, "railreverse", "key_hub_limit_up", s, sizeof(s), "CTRL+PLUS");
    if (!ParseKey(s, &cfg.keyLimitUp)) { Logf("railreverse  key_hub_limit_up \"%s\" not understood, using CTRL+PLUS", s); ParseKey("CTRL+PLUS", &cfg.keyLimitUp); }
    H->configString(RR_INI, "railreverse", "key_hub_limit_down", s, sizeof(s), "CTRL+MINUS");
    if (!ParseKey(s, &cfg.keyLimitDown)) { Logf("railreverse  key_hub_limit_down \"%s\" not understood, using CTRL+MINUS", s); ParseKey("CTRL+MINUS", &cfg.keyLimitDown); }
    H->configString(RR_INI, "railreverse", "key_hub_limit_reset", s, sizeof(s), "CTRL+SHIFT+MINUS");
    if (!ParseKey(s, &cfg.keyLimitReset)) { Logf("railreverse  key_hub_limit_reset \"%s\" not understood, using CTRL+SHIFT+MINUS", s); ParseKey("CTRL+SHIFT+MINUS", &cfg.keyLimitReset); }
    Logf("railreverse  v%s init: push speed %.0f km/h, key %s", RR_VERSION, cfg.pushKmh, cfg.keyTrain.label);
    return 0;
}

// ---------------------------------------------------------------- crash log
// A diagnostic only: on an access violation anywhere in the game, log which
// module and offset it happened at, and the module addresses found on the
// stack (return addresses: who called whom). Nothing is handled or changed;
// the exception goes on exactly as without it. This is first-chance, so an
// exception the game catches itself is logged too - the line says so.
static volatile LONG g_crashLines;

static bool ModuleOf(void* a, char* out, size_t cap, ULONG_PTR* off)
{
    HMODULE m = 0;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)a, &m) || !m) return false;
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(m, path, sizeof(path));
    const char* base = path;
    for (DWORD i = 0; i < n; ++i) if (path[i] == '\\' || path[i] == '/') base = path + i + 1;
    _snprintf_s(out, cap, _TRUNCATE, "%s", n ? base : "?");
    *off = (ULONG_PTR)a - (ULONG_PTR)m;
    return true;
}

static LONG CALLBACK CrashLog(PEXCEPTION_POINTERS ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_PRIV_INSTRUCTION) return EXCEPTION_CONTINUE_SEARCH;
    // Each faulting address is logged once, up to 64 of them: an exception the
    // game handles and repeats does not use up the lines a real crash needs.
    static void* seen[64];
    void* at = ep->ExceptionRecord->ExceptionAddress;
    for (int i = 0; i < 64; ++i) if (seen[i] == at) return EXCEPTION_CONTINUE_SEARCH;
    LONG slot = InterlockedIncrement(&g_crashLines) - 1;
    if (slot >= 64) return EXCEPTION_CONTINUE_SEARCH;
    seen[slot] = at;

    char mod[64]; ULONG_PTR off = 0;
    if (!ModuleOf(at, mod, sizeof(mod), &off)) { strcpy(mod, "?"); off = (ULONG_PTR)at; }
    const char* how = "?";
    ULONG_PTR target = 0;
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
        ULONG_PTR kind = ep->ExceptionRecord->ExceptionInformation[0];
        how = kind == 0 ? "read" : kind == 1 ? "write" : kind == 8 ? "execute" : "?";
        target = ep->ExceptionRecord->ExceptionInformation[1];
    }
    char stack[512] = "";
    int len = 0, found = 0;
    ULONG_PTR* sp = (ULONG_PTR*)ep->ContextRecord->Rsp;
    for (int i = 0; i < 256 && found < 8; ++i) {
        ULONG_PTR q = sp[i];
        if (q < 0x10000) continue;
        char m2[64]; ULONG_PTR o2;
        if (ModuleOf((void*)q, m2, sizeof(m2), &o2) && o2 > 0x1000) {
            len += _snprintf_s(stack + len, sizeof(stack) - len, _TRUNCATE, " %s+%llX", m2, (unsigned long long)o2);
            ++found;
        }
    }
    Logf("railreverse  CRASH LOG (first chance, may still be handled by the game): 0x%08lX at %s+%llX, %s of %p; "
         "stack:%s", code, mod, (unsigned long long)off, how, (void*)target, stack);
    return EXCEPTION_CONTINUE_SEARCH;
}

extern "C" __declspec(dllexport) int TsmPluginStart(void)
{
    if (H->configInt(RR_INI, "railreverse", "crash_log", 1) && AddVectoredExceptionHandler(1, CrashLog))
        Logf("railreverse  crash log armed");

    // The core (flip + placement) is resolved before anything is hooked: a
    // missing piece leaves the game untouched instead of half-patched.
    g_sample = (FnSample)FindUnique(SIG(SIG_SAMPLE));
    g_rotXY  = (FnRotXY)GetProcAddress(g_engine, EXP_ROTXY);
    // drawing and clicking buttons the way the game's windows do
    g_mouse = (FnMouse)GetProcAddress(g_engine, EXP_MOUSE);
    g_texture = (FnTexture)GetProcAddress(g_engine, EXP_TEXTURE);
    g_panelDraw = (FnDraw)GetProcAddress(g_engine, EXP_DRAW);
    if (!g_mouse || !g_texture || !g_panelDraw) {
        cfg.windowButtons = 0;
        Logf("railreverse  train window buttons unavailable (engine exports missing)");
    }
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
        g_allForced = 1;
    }
    if (!FindFont()) {
        g_print = 0;
        Logf("railreverse  font not found - no indicator");
    } else if ((window || tabs) &&
               !Hook(SIG(SIG_DISPATCH), DISPATCH_STOLEN, (void*)HookDispatch, (void**)&g_origDispatch, "window dispatcher")) {
        g_print = 0;
        Logf("railreverse  window dispatcher not found - no indicator");
    }

    // Distribution offices: the hub limit (two call sites).
    {
        BYTE* a = FindUnique(SIG(SIG_ROOM_A));
        BYTE* b = FindUnique(SIG(SIG_ROOM_B));
        BYTE* ta = a ? a + ROOM_CALL_AT + 5 + *(INT32*)(a + ROOM_CALL_AT + 1) : 0;
        BYTE* tb = b ? b + ROOM_CALL_AT + 5 + *(INT32*)(b + ROOM_CALL_AT + 1) : 0;
        BYTE* stub = (a && b && ta == tb) ? AllocNear(a, 32) : 0;
        if (stub) {
            // two of: jmp [rip+0]; dq <thunk> - within reach of the call sites
            void* thunks[2] = { (void*)RR_RoomThunkA, (void*)RR_RoomThunkB };
            for (int i = 0; i < 2; ++i) {
                BYTE* s = stub + 16 * i;
                s[0] = 0xFF; s[1] = 0x25; *(INT32*)(s + 2) = 0;
                *(void**)(s + 6) = thunks[i];
            }
            g_room = (FnRoom)ta;
            BYTE* sites[2] = { a + ROOM_CALL_AT, b + ROOM_CALL_AT };
            for (int i = 0; i < 2; ++i) {
                DWORD old;
                VirtualProtect(sites[i], 5, PAGE_EXECUTE_READWRITE, &old);
                *(INT32*)(sites[i] + 1) = (INT32)(stub + 16 * i - (sites[i] + 5));
                VirtualProtect(sites[i], 5, old, &old);
                FlushInstructionCache(GetCurrentProcess(), sites[i], 5);
            }
            Logf("railreverse  hub limit: office room checks at +%06llX / +%06llX redirected",
                 (unsigned long long)(sites[0] - g_exeBase), (unsigned long long)(sites[1] - g_exeBase));
        } else {
            Logf("railreverse  hub limit unavailable: office room check not found");
        }
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

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

#define RR_VERSION "0.6.1"
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
#define RVA_WORLD      0x9D4F10   // DAT_1409d4f10, what the game passes as the world

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
    int   shunting;         // run round: uncouple / stop-and-turn marks in the route window (ini shunting)
    int   research;         // research lines in the log, every second per train (ini research_log)
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
#define BUILD_1119 0x6A3EB6ADu          // SOVIET64.exe 1.1.1.9: its PE time stamp
static bool g_build119;                 // the fixed addresses below are this build's
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

// Trains that have left their wagons at a marked waypoint, one record each,
// until the wagons are back. The locomotive couples again only once it has
// run LEFT_AWAY_M (it is turned round right there quite often), the same
// train does not stop at that waypoint again before it has run AGAIN_M, and
// on its way round every waypoint counts only once it is there
// (RR_PassWaypoint). The way run is its speed over the time it moves (it is
// not drawn, and not counted, while it stands).
#define LEFT_AWAY_M 40.0f
#define AGAIN_M     100.0f
#define LEFT_MAX    64
struct Left { BYTE* loco; BYTE* wp; float run; DWORD tick; DWORD stood; };
static Left g_left[LEFT_MAX];
static int  g_leftCount;

static Left* LeftOf(BYTE* loco)
{
    for (int i = 0; i < g_leftCount; ++i) if (g_left[i].loco == loco) return &g_left[i];
    return 0;
}

static void LeftAdd(BYTE* loco, BYTE* wp)
{
    Left* l = LeftOf(loco);
    if (!l) {
        if (g_leftCount == LEFT_MAX) memmove(g_left, g_left + 1, sizeof(Left) * --g_leftCount);   // the oldest goes
        l = &g_left[g_leftCount++];
    }
    l->loco = loco; l->wp = wp; l->run = 0.0f; l->tick = 0; l->stood = 0;
}

static void LeftForget(BYTE* loco)
{
    if (Left* l = LeftOf(loco)) *l = g_left[--g_leftCount];
}

// Research: a locomotive running round, once a second while it moves.
static int  g_planFails, g_signalWaits;   // since the last line
static float ToNode(BYTE* v);
static bool SignalAt(BYTE* node);
static void RoundLog(BYTE* v)
{
    static DWORD last;
    if (!cfg.research || GetTickCount() - last < 1000) return;
    last = GetTickCount();
    BYTE *sb = 0, *se = 0, *recs = 0, *nb = 0, *ne = 0, *gb = 0, *ge = 0, *seg = 0, *node = 0;
    int at = -1, idx = -1;
    float kmh = 0.0f;
    Rd(v, 0x680, &sb); Rd(v, 0x688, &se); Rd(v, 0x638, &recs); Rd(v, 0x698, &at); Rd(v, V_SPEED, &kmh);
    Rd(v, 0x6A0, &nb); Rd(v, 0x6A8, &ne); Rd(v, 0x6B8, &gb); Rd(v, 0x6C0, &ge); Rd(v, 0x700, &idx); Rd(v, 0x798, &seg);
    int n = sb && se > sb ? (int)((se - sb) / 8) : 0, nn = nb && ne > nb ? (int)((ne - nb) / 8) : 0;
    int ng = gb && ge > gb ? (int)((ge - gb) / 8) : 0;
    BYTE* stop = at >= 0 && at < n ? ((BYTE**)sb)[at] : 0;
    if (!stop && at >= 0 && at < n && recs) Rd(recs + (size_t)at * 0x198, 0x190, &node);
    const char* kind = stop ? "building" : !node ? "?" : SignalAt(node) ? "signal" : "waypoint";
    BYTE* head = idx >= 0 && idx < nn ? ((BYTE**)nb)[idx] : 0;
    Logf("railreverse  [round] %p: %.1f km/h, stop %d of %d (%s %p), way %d nodes / %d segments, heading for node %d "
         "%p%s (%.1f m), %s; planner said no %d times, signal said wait %d times", v, kmh, at + 1, n, kind,
         stop ? stop : node, nn, ng, idx, head, head && SignalAt(head) ? " (a signal)" : "", ToNode(v),
         nn && ((BYTE**)nb)[nn - 1] == node ? "its stop ends the way" : "its stop is not the way's end", g_planFails, g_signalWaits);
    // where the way ends, and the signal there with its "block free" flags (+0x1A8 / +0x1A9, 1 = free)
    BYTE* endNode = nn ? ((BYTE**)nb)[nn - 1] : 0;
    BYTE* sig = 0;
    if (endNode) Rd(endNode, 0xB8, &sig);
    float ex = 0, ez = 0;
    if (endNode) { Rd(endNode, 4, &ex); Rd(endNode, 12, &ez); }
    char flags[64];
    _snprintf_s(flags, sizeof(flags), _TRUNCATE, "D39 %d D3A %d D3B %d D3C %d", *(v + 0xD39), *(v + 0xD3A),
                *(v + 0xD3B), *(v + 0xD3C));
    if (sig) Logf("railreverse  [round]   the way ends at %.0f,%.0f by a signal %p, flags %d %d; %s", ex, ez, sig,
                  *(sig + 0x1A8), *(sig + 0x1A9), flags);
    else Logf("railreverse  [round]   the way ends at %.0f,%.0f, no signal there; %s", ex, ez, flags);
    g_planFails = g_signalWaits = 0;
}

// Every drawn frame of a lead with a record: add the way it ran since the last one.
static void LeftRun(BYTE* v)
{
    Left* l = LeftOf(v);
    if (!l) return;
    DWORD now = GetTickCount();
    float dt = l->tick ? (now - l->tick) / 1000.0f : 0.0f;
    if (dt > 0.1f) dt = 0.1f;                              // back from standing: not the whole time it stood
    l->tick = now;
    float kmh = 0.0f;
    Rd(v, V_SPEED, &kmh);
    float before = l->run;
    l->run += fabsf(kmh) / 3.6f * dt;
    if (before < LEFT_AWAY_M && l->run >= LEFT_AWAY_M)
        Logf("railreverse  [couple] %p: ran %.0f m from its wagons, may couple again", v, LEFT_AWAY_M);
    if (l->run > 1e6f) l->run = 1e6f;
    BYTE** lw;
    if (Wagons(v, &lw) == 0) RoundLog(v);
}

// Research: any train the game turns round is followed for 30 s (RoundLog).
static BYTE* g_turned;
static DWORD g_turnedUntil;
static BYTE* AheadOf(BYTE* lead, float* at);
static bool  Couple(BYTE* loco, BYTE* hit, BYTE* hitSeg, float hitAt);
static void* TrainStop(BYTE* lead, BYTE** wp, int* ord, int* at);
static int   MarkIndex(BYTE* route, void* key, BYTE* wp, int ord, int kind);
static BYTE* RouteOf(BYTE* lead);
static volatile LONG g_marksPending;   // loaded marks wait for their routes (ResolveMarks)
static void  ResolveMarks(void);
static int   g_standCount;           // standing consists watched, see StandCheck
static volatile LONG g_standScan;    // a save was loaded: look for wagons standing on their own


static void HookFlip(BYTE* lead, char routeMode, char extend)
{
    // Shunting: uncoupled wagons have no route, and the game's reversal
    // reads one (it reverses them when something stands against them).
    if (cfg.shunting && lead && *(BYTE**)(lead + 0x6A0) == *(BYTE**)(lead + 0x6A8)) {
        *(lead + 0xD41) = 0;
        static BYTE* told;
        if (told != lead) told = lead, Logf("railreverse  [tail] %p: reversal without a route skipped", lead);
        return;
    }
    // A coupled-back car keeps the flag it got as a lead; it has no segment to turn on.
    if (cfg.shunting && lead && (*(BYTE**)(lead + V_PARENT) || !*(BYTE**)(lead + 0x798))) {
        *(lead + 0xD41) = 0;
        Logf("railreverse  [couple] %p: reversal of a car or of a train off the track skipped", lead);
        return;
    }
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

    // Shunting: a locomotive without wagons that the game turns round
    // against standing wagons (a consist without a locomotive) couples them.
    BYTE *hit = 0, *hitSeg = 0;
    float hitAt = 0.0f;
    BYTE** lw;
    if (cfg.shunting && lead && Wagons(lead, &lw) == 0 && IsLoco(lead)) {
        Rd(lead, 0x798, &hitSeg);
        hit = AheadOf(lead, &hitAt);
        BYTE* consist = 0;
        if (hit && Rd(hit, V_PARENT, &consist) && !consist) consist = hit;
        if (!consist || consist == lead || IsLoco(consist)) hit = 0;
        Left* left = hit ? LeftOf(lead) : 0;
        if (left && left->run < LEFT_AWAY_M) {               // turned round right where it left them
            Logf("railreverse  [couple] %p: against the wagons it has just left - not coupled", lead);
            hit = 0;
        }
    }

    // A train standing at a waypoint marked "stop and turn round" is turned
    // round by the game there (its way ends there): that stop is done, the
    // way after the turn is for the next one.
    bool turnedAtStop = false;
    if (cfg.shunting && lead && !*(BYTE**)(lead + V_PARENT)) {
        BYTE* swp = 0;
        int sord = 0, sat = -1;
        void* skey = TrainStop(lead, &swp, &sord, &sat);
        float d = skey && swp ? ToNode(lead) : -1.0f;
        if (skey && swp && d >= 0.0f && d < 5.0f && MarkIndex(RouteOf(lead), swp, swp, sord, 1) >= 0) {
            BYTE *sb = 0, *se = 0;
            Rd(lead, 0x680, &sb); Rd(lead, 0x688, &se);
            int n = sb && se > sb ? (int)((se - sb) / 8) : 0;
            if (n > 0) {
                turnedAtStop = true;
                *(int*)(lead + 0x698) = (sat + 1) % n;
                Logf("railreverse  [stop] train %p turned round at its stop-and-turn stop %d: on to stop %d", lead, sat + 1,
                     (sat + 1) % n + 1);
            }
        }
    }

    g_origFlip(lead, routeMode, extend);

    // After a reversal the game gives a short way to the next node with +0xD3A
    // (carry the way on at that node) and +0xD3B; with both, the last 10 m of
    // the way are crept at about 1 km/h (FUN_1406a7520). Without +0xD3B the
    // train runs on to the node at speed and the way is carried on there all
    // the same: for a locomotive running round and a stop-and-turn train.
    BYTE** tw;
    if (cfg.shunting && lead && !*(BYTE**)(lead + V_PARENT) && !hit &&
        (turnedAtStop || (LeftOf(lead) && Wagons(lead, &tw) == 0)))
        *(lead + 0xD3B) = 0;
    if (turnedAtStop) *(float*)(lead + V_SPEED) = 0.0f;   // it sets off from standing
    if (cfg.research && lead && !*(BYTE**)(lead + V_PARENT)) g_turned = lead, g_turnedUntil = GetTickCount() + 30000;
    if (cfg.research && lead && !*(BYTE**)(lead + V_PARENT))                   // the way it is given
        Logf("railreverse  [round] %p turned round (%d, %d): way %d nodes, D39 %d D3A %d D3B %d D3C %d", lead, routeMode,
             extend, (int)((*(BYTE**)(lead + 0x6A8) - *(BYTE**)(lead + 0x6A0)) / 8), *(lead + 0xD39), *(lead + 0xD3A),
             *(lead + 0xD3B), *(lead + 0xD3C));

    if (hit) Couple(lead, hit, hitSeg, hitAt);
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

// Locomotives running round their wagons (g_left), one line each:
//   left <index in the vehicle list> <x> <y> <z> <metres run> <type folder>
// x y z: the waypoint where it left them, found again in its schedule.
struct LeftPending { int index; float at[3]; float run; char type[TYPE_NAME_MAX]; };
static LeftPending    g_leftPending[LEFT_MAX];
static int            g_leftPendingCount;
static volatile LONG  g_leftPendingLive;
static unsigned       g_leftPendingTries;

static int  MarksText(char* out, int cap, int* count);   // the route window's uncouple marks, see below
static void LoadMark(const char* text, int kind);
static void ClearMarks(void);
static void ClearWatch(void);

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
    int lefts = 0;
    for (int k = 0; k < g_leftCount; ++k) {
        const Left& l = g_left[k];
        int i = 0;
        while (i < n && veh[i] != l.loco) ++i;
        if (i >= n) continue;                                // gone (sold)
        float at[3] = { 0, 0, 0 };
        if (l.wp) { Rd(l.wp, 4, &at[0]); Rd(l.wp, 8, &at[1]); Rd(l.wp, 12, &at[2]); }
        char name[TYPE_NAME_MAX];
        TypeName(l.loco, name);
        len += _snprintf_s(buf + len, sizeof(buf) - len, _TRUNCATE, "left %d %.3f %.3f %.3f %.1f %s\n", i, at[0], at[1], at[2],
                           l.run, name);
        ++lefts;
    }
    LeaveCriticalSection(&g_lock);
    int marks = 0;
    len += MarksText(buf + len, (int)sizeof(buf) - len, &marks);
    if (!trains && !marks && !lefts) {
        DeleteFileA(path);                // nothing to remember: no stale file for the next load
        Logf("railreverse  save: nothing to remember");
        return;
    }
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE) { Logf("railreverse  save: cannot write %s", path); return; }
    DWORD wrote = 0;
    WriteFile(h, buf, (DWORD)len, &wrote, 0);
    CloseHandle(h);
    Logf("railreverse  save: %d train(s), %d stop mark(s), %d locomotive(s) running round -> %s", trains, marks, lefts, path);
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
    g_leftCount = 0;
    g_leftPendingCount = 0;
    g_leftPendingLive = 0;
    ClearMarks();
    ClearWatch();
    g_standCount = 0;
    g_standScan = 1;
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
        } else if (!strncmp(line, "uncouple ", 9)) {
            LoadMark(line + 9, 0);
        } else if (!strncmp(line, "reverse ", 8)) {
            LoadMark(line + 8, 1);
        } else if (!strncmp(line, "left ", 5)) {
            LeftPending l = {};
            int used = 0;
            if (g_leftPendingCount < LEFT_MAX &&
                sscanf(line + 5, "%d %f %f %f %f %n", &l.index, &l.at[0], &l.at[1], &l.at[2], &l.run, &used) == 5 && used > 0) {
                strncpy(l.type, line + 5 + used, TYPE_NAME_MAX - 1);
                g_leftPending[g_leftPendingCount++] = l;
            }
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
    g_leftPendingTries = 0;
    g_leftPendingLive = g_leftPendingCount;
    LeaveCriticalSection(&g_lock);
    Logf("railreverse  load: %d train(s), %d locomotive(s) running round to restore from %s", g_pendingCount,
         g_leftPendingCount, path);
}

// The waypoint of a train's schedule at x y z (0: none there now).
static BYTE* ScheduleWaypointAt(BYTE* lead, const float* at)
{
    BYTE *sb = 0, *se = 0, *recs = 0;
    Rd(lead, 0x680, &sb); Rd(lead, 0x688, &se); Rd(lead, 0x638, &recs);
    int n = sb && se > sb ? (int)((se - sb) / 8) : 0;
    if (!recs || !ReadablePtr(sb, (size_t)n * 8)) return 0;
    for (int i = 0; i < n; ++i) {
        BYTE* wp = 0;
        float p[3];
        if (((BYTE**)sb)[i] || !Rd(recs + (size_t)i * 0x198, 0x190, &wp) || !wp) continue;   // REC_SIZE, REC_WAYPOINT
        if (!Rd(wp, 4, &p[0]) || !Rd(wp, 8, &p[1]) || !Rd(wp, 12, &p[2])) continue;
        float dx = p[0] - at[0], dy = p[1] - at[1], dz = p[2] - at[2];
        if (dx * dx + dy * dy + dz * dz < 1.0f) return wp;
    }
    return 0;
}

// Like RestorePending: the locomotives running round go back on the list.
static void RestoreLeft(void)
{
    BYTE** veh = 0;
    int n = VehicleCount(&veh), restored = 0, left = 0;
    EnterCriticalSection(&g_lock);
    for (int k = 0; k < g_leftPendingCount; ++k) {
        LeftPending* p = &g_leftPending[k];
        if (p->index < 0 || p->index >= n) continue;
        BYTE* v = veh[p->index];
        if (!IsLoco(v)) continue;
        char name[TYPE_NAME_MAX];
        TypeName(v, name);
        if (strcmp(name, p->type) != 0) continue;
        LeftAdd(v, ScheduleWaypointAt(v, p->at));
        LeftOf(v)->run = p->run;
        p->index = -1;
        ++restored;
    }
    for (int k = 0; k < g_leftPendingCount; ++k) if (g_leftPending[k].index >= 0) ++left;
    bool giveUp = ++g_leftPendingTries > 20000;
    if (!left || giveUp) g_leftPendingLive = 0;
    LeaveCriticalSection(&g_lock);
    if (restored) Logf("railreverse  load: %d locomotive(s) running round restored", restored);
    if (giveUp && left) Logf("railreverse  load: %d locomotive(s) running round not found, dropped", left);
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

// ---------------------------------------------------------------- route research (ini research_log)
// Research for shunting: how a train stops behind another one. For a chosen
// train (Ctrl+F9), twice a second: speed, its route (+0x6A0 track segments,
// +0x700 the current one), where it is on its segment (+0x798 segment,
// +0x7A4 position, segment length at +0x11C), how far the route goes, and the
// first segment ahead on the route that another train stands on (its
// occupancy ring, +0x7D0: 75 x {segment*, u8 direction}).
#define V_ROUTE_BEGIN  0x6A0
#define V_ROUTE_END    0x6A8
#define V_ROUTE_INDEX  0x700
#define V_SEGMENT      0x798
#define V_SEGMENT_POS  0x7A4
#define V_RING         0x7D0
#define RING_ENTRIES   75
#define SEG_LENGTH     0x11C

static float SegLength(BYTE* seg)
{
    float l = 0.0f;
    return Rd(seg, SEG_LENGTH, &l) ? l : 0.0f;
}

static BYTE* RingOwner(BYTE* seg, BYTE* self)
{
    BYTE** b;
    int n = VehicleCount(&b);
    for (int i = 0; i < n; ++i) {
        BYTE* v = b[i];
        BYTE* parent = 0;
        if (!v || v == self || !Rd(v, V_PARENT, &parent) || parent) continue;
        if (!ReadablePtr(v + V_RING, RING_ENTRIES * 16)) continue;
        for (int k = 0; k < RING_ENTRIES; ++k)
            if (*(BYTE**)(v + V_RING + k * 16) == seg) return v;
    }
    return 0;
}

static void RouteDebug(BYTE* lead)
{
    static DWORD last;
    EnterCriticalSection(&g_lock);
    Train* t = Find(lead);
    bool chosen = t && t->chosen;
    LeaveCriticalSection(&g_lock);
    if (!chosen) return;
    DWORD now = GetTickCount();
    if (now - last < 500) return;
    last = now;

    float speed = 0.0f, pos = 0.0f;
    BYTE *rb = 0, *re = 0, *seg = 0;
    int idx = -1;
    Rd(lead, V_SPEED, &speed);
    Rd(lead, V_ROUTE_BEGIN, &rb);
    Rd(lead, V_ROUTE_END, &re);
    Rd(lead, V_ROUTE_INDEX, &idx);
    Rd(lead, V_SEGMENT, &seg);
    Rd(lead, V_SEGMENT_POS, &pos);
    int n = rb && re > rb && ReadablePtr(rb, (size_t)(re - rb)) ? (int)((re - rb) / 8) : 0;
    BYTE** route = (BYTE**)rb;

    // what is left of the route, and the first segment ahead someone stands on
    float toEnd = seg ? SegLength(seg) - pos : 0.0f, ahead = toEnd;
    BYTE* blocker = 0;
    int blockAt = -1;
    if (seg) blocker = RingOwner(seg, lead), blockAt = blocker ? idx : -1;
    for (int i = idx + 1; i >= 1 && i < n && !blocker; ++i) {
        blocker = RingOwner(route[i], lead);
        if (blocker) blockAt = i;
        else ahead += SegLength(route[i]);
    }
    for (int i = idx + 1; i >= 1 && i < n; ++i) toEnd += SegLength(route[i]);
    BYTE* lastSeg = n ? route[n - 1] : 0;

    // the other train: where its own head is, and how far round its ring our segment is
    BYTE* oseg = 0;
    float opos = -1.0f;
    int oring = -1;
    if (blocker) {
        Rd(blocker, V_SEGMENT, &oseg);
        Rd(blocker, V_SEGMENT_POS, &opos);
        for (int k = 0; k < RING_ENTRIES; ++k)
            if (*(BYTE**)(blocker + V_RING + k * 16) == seg) { oring = k; break; }
    }

    Logf("railreverse  [route] train %p: %.1f km/h | segment %p (route[%d] %p) at %.1f of %.1f m | route %d/%d, "
         "%.1f m to its end, last %p | other train %p on route[%d], %.1f m ahead; its head on %p at %.1f, "
         "our segment at its ring[%d]",
         lead, speed, seg, idx, idx >= 0 && idx < n ? route[idx] : 0, pos, SegLength(seg), idx, n, toEnd, lastSeg,
         blocker, blockAt, blocker ? ahead : -1.0f, oseg, opos, oring);
}

// ---------------------------------------------------------------- uncoupling (ini shunting)
// Research for shunting: CTRL+F11 in a train window uncouples the wagons with
// the game's own split, FUN_1406109d0(_, lead): the locomotives at the front
// stay, the first other car becomes the lead of a new consist with the rest.
// Wagons have no track position of their own (FUN_1406a5410 places them back
// from the lead along its trail, the ring), so the new lead gets one here: the
// point its front stands at, and the rest of the old lead's trail behind it.
static const short SIG_SPLIT[] = {
    0x48,0x89,0x5c,0x24,0x20, 0x48,0x89,0x4c,0x24,0x08, 0x55,0x56,0x57,0x41,0x56,0x41,0x57, 0x48,0x83,0xec,0x20,
    0x48,0x8b,0xba,0x98,0x03,0x00,0x00, 0x48,0x8b,0xda, 0x48,0x85,0xff, 0x0f,0x84,-1,-1,-1,-1,
    0x48,0x8b,0x87,0xa8,0x03,0x00,0x00, 0x33,0xed, 0x48,0x2b,0x87,0xa0,0x03,0x00,0x00, 0x48,0xc1,0xf8,0x03,
};
#define V_SEGMENT_DIR  0x7A0      // u8
#define V_SEGS_BEGIN   0x6B8      // route segments vector
#define V_SEGS_END     0x6C0
typedef void (*FnSplit)(void*, BYTE*);
static FnSplit g_split;

// A signal's "block taken" flags (+0x1A8 / +0x1A9) are set and cleared as
// trains pass it, each time worked out by FUN_14041ec00(world, signal, ...)
// from who is written in the block's segments. Wagons left standing are
// written in their segments; sold there, they are struck out of them, but
// the flags stay "taken" until a train passes. So every standing consist is
// watched; once it is gone (sold, not coupled: a coupled one is a car now),
// the signals around the segments it stood on are worked out again, found
// afresh from the track at that moment (a signal kept for later could be
// pulled up meanwhile).
static const short SIG_BLOCKSTATE[] = {
    0x48,0x89,0x5c,0x24,0x08, 0x48,0x89,0x6c,0x24,0x10, 0x48,0x89,0x74,0x24,0x18, 0x48,0x89,0x7c,0x24,0x20,
    0x41,0x56, 0x48,0x83,0xec,0x30, 0x4c,0x8b,0x72,0x08, 0x33,0xf6,
};
typedef void (*FnBlockState)(void* world, BYTE* signal, char a, char b);
static FnBlockState g_blockState;
struct Stand { BYTE* v; BYTE* segs[16]; int n; };
static Stand g_stand[16];

static void StandAdd(BYTE* v)
{
    if (g_standCount >= 16) return;
    Stand& st = g_stand[g_standCount++];
    st.v = v; st.n = 0;
    BYTE* seg = *(BYTE**)(v + V_SEGMENT);
    if (seg) st.segs[st.n++] = seg;
    for (int k = 0; k < RING_ENTRIES && st.n < 16; ++k) {
        BYTE* r = *(BYTE**)(v + V_RING + k * 16);
        if (!r) break;
        bool seen = false;
        for (int j = 0; j < st.n; ++j) seen |= st.segs[j] == r;
        if (!seen) st.segs[st.n++] = r;
    }
}

// The signals bounding the block of these segments: walked over the nodes
// (+0x20 / +0x28 a segment's ends, +0x28 a node's segments) up to signals.
static int BlockSignals(BYTE* const* start, int nstart, BYTE** sig, int cap)
{
    BYTE* block[64];
    int nblock = 0, nsig = 0;
    for (int i = 0; i < nstart && nblock < 64; ++i) block[nblock++] = start[i];
    for (int q = 0; q < nblock; ++q) {
        for (int end = 0; end < 2; ++end) {
            BYTE* node = 0;
            if (!ReadablePtr(block[q], 0x48) || !Rd(block[q], end ? 0x28 : 0x20, &node) || !node) continue;
            if (SignalAt(node)) {
                BYTE* sg = *(BYTE**)(node + 0xB8);
                bool seen = false;
                for (int k = 0; k < nsig; ++k) seen |= sig[k] == sg;
                if (!seen && nsig < cap) sig[nsig++] = sg;
                continue;
            }
            BYTE *sb = 0, *se = 0;
            if (!Rd(node, 0x28, &sb) || !Rd(node, 0x30, &se) || !sb || se <= sb || !ReadablePtr(sb, (size_t)(se - sb))) continue;
            for (BYTE** sp = (BYTE**)sb; sp < (BYTE**)se && nblock < 64; ++sp) {
                bool seen = false;
                for (int k = 0; k < nblock && !seen; ++k) seen = block[k] == *sp;
                if (*sp && !seen) block[nblock++] = *sp;
            }
        }
    }
    return nsig;
}

static bool StandingWagons(BYTE* v);

// After a load: the wagons left standing in the save.
static void StandScan(void)
{
    BYTE** veh;
    int nv = VehicleCount(&veh), found = 0;
    if (nv <= 0) return;
    g_standScan = 0;
    for (int k = 0; k < nv; ++k)
        if (veh[k] && *(BYTE**)(veh[k] + V_SEGMENT) && StandingWagons(veh[k])) { StandAdd(veh[k]); ++found; }
    if (found) Logf("railreverse  load: %d consist(s) of wagons standing on their own", found);
}

static void StandCheck(void)
{
    static DWORD last;
    if (!g_standCount || GetTickCount() - last < 500) return;
    last = GetTickCount();
    BYTE** veh;
    int nv = VehicleCount(&veh);
    for (int i = 0; i < g_standCount; ++i) {
        Stand& st = g_stand[i];
        bool alive = false;
        for (int k = 0; k < nv && !alive; ++k) alive = veh[k] == st.v;
        BYTE* parent = 0;
        if (alive && Rd(st.v, V_PARENT, &parent) && !parent) continue;   // still standing on its own
        if (!alive && g_blockState) {                                      // sold: the signals around it afresh
            BYTE* sig[16];
            int ns = BlockSignals(st.segs, st.n, sig, 16);
            for (int k = 0; k < ns; ++k) g_blockState(g_exeBase + RVA_WORLD, sig[k], 1, 1);
            Logf("railreverse  [stop] wagons %p gone (sold): %d signal(s) around them worked out again", st.v, ns);
        }
        g_stand[i] = g_stand[--g_standCount];                             // gone, or coupled: no more watching
        --i;
    }
}

static void Uncouple(BYTE* lead)
{
    // Pushing, the locomotive is drawn at the back but is the game's front:
    // reverse first (same picture, now pulling), so it leaves from where it is seen.
    if (IsPushing(lead)) {
        HookFlip(lead, 1, 0);
        Logf("railreverse  [uncouple] train %p was pushing: reversed first, %s now", lead,
             IsPushing(lead) ? "still pushing" : "pulling");
    }
    BYTE** w;
    int n = Wagons(lead, &w);
    BYTE* seg = 0;
    float pos = 0.0f;
    if (!g_split || n <= 0 || !Rd(lead, V_SEGMENT, &seg) || !seg || !Rd(lead, V_SEGMENT_POS, &pos) ||
        !ReadablePtr(lead + V_RING, RING_ENTRIES * 16)) {
        Logf("railreverse  [uncouple] train %p: nothing to do (%d wagons, segment %p)", lead, n, seg);
        return;
    }
    BYTE* before[1024];
    memcpy(before, w, (size_t)n * 8);
    BYTE dir = *(lead + V_SEGMENT_DIR);
    BYTE ring[RING_ENTRIES * 16];
    memcpy(ring, lead + V_RING, sizeof(ring));

    g_split(0, lead);
    int kept = Wagons(lead, &w);
    if (kept < 0 || kept >= n) {
        Logf("railreverse  [uncouple] train %p: the game kept all %d wagons", lead, n);
        return;
    }
    BYTE* tail = before[kept];

    // Where the tail's front stands: that far back from the lead's front.
    float back = CarLength(lead);
    for (int i = 0; i < kept; ++i) back += CarLength(before[i]);
    float at = pos - back;
    int k = 0;
    while (at < 0.0f && k < RING_ENTRIES && *(BYTE**)(ring + k * 16)) {
        seg = *(BYTE**)(ring + k * 16);
        dir = ring[k * 16 + 8];
        at += SegLength(seg);
        ++k;
    }
    if (at < 0.0f) Logf("railreverse  [uncouple] trail too short, %.1f m missing", -at);

    *(BYTE**)(tail + V_SEGMENT) = seg;
    *(tail + V_SEGMENT_DIR) = dir;
    *(float*)(tail + V_SEGMENT_POS) = at < 0.0f ? 0.0f : at;
    memset(tail + V_RING, 0, sizeof(ring));
    memcpy(tail + V_RING, ring + k * 16, (size_t)(RING_ENTRIES - k) * 16);
    *(float*)(tail + V_SPEED) = 0.0f;
    *(BYTE**)(tail + V_ROUTE_END) = *(BYTE**)(tail + V_ROUTE_BEGIN);
    *(BYTE**)(tail + V_SEGS_END) = *(BYTE**)(tail + V_SEGS_BEGIN);
    *(int*)(tail + V_ROUTE_INDEX) = 0;      // a wagon keeps -1 there, the rail update reads route[index] (FUN_1406a7520)
    *(tail + 0xD41) = 0;                    // the reverse button: a wagon may carry it, see HookFlip

    BYTE** tb;
    Logf("railreverse  [uncouple] train %p: kept %d of %d wagons; new consist %p (%d wagons) at %.1f m of segment %p "
         "dir %d, %.1f m back, trail from ring[%d]",
         lead, kept, n, tail, Wagons(tail, &tb), at, seg, dir, back, k);
    StandAdd(tail);
}


// The game's obstacle lookup: the car at a point of a track segment
// (FUN_1405219e0, its map at DAT_1409e4818). 1.1.1.9 addresses (off on another build).
#define RVA_VEHICLE_AT 0x5219E0
#define RVA_TRACK_MAP  0x9E4818
typedef BYTE* (*FnVehicleAt)(void* map, BYTE* seg, char dir, float at, void* node, int index);
static const BYTE VEHICLE_AT_HEAD[] = { 0x48,0x89,0x5c,0x24,0x20, 0x48,0x89,0x4c,0x24,0x08, 0x55,0x56,0x57, 0x48,0x83,0xec,0x60 };

static const short SIG_COUPLE[] = {     // FUN_1403fe3c0(_, a, b): b and its wagons go behind a's
    0x48,0x85,0xd2, 0x0f,0x84,-1,-1,-1,-1, 0x4c,0x89,0x44,0x24,0x18, 0x48,0x89,0x4c,0x24,0x08, 0x41,0x56,
    0x48,0x83,0xec,0x30, 0x4c,0x8b,0xf2, 0x4d,0x85,0xc0, 0x0f,0x84,-1,-1,-1,-1, 0x48,0x89,0x5c,0x24,0x48,
    0x48,0x8d,0x9a,0xa0,0x03,0x00,
};
typedef void (*FnCouple)(void*, BYTE*, BYTE*);
static FnCouple g_couple3;

static const short SIG_UNRESERVE[] = {  // FUN_140681970(vehicle): out of every segment it holds (+0x7B8),
                                        // then written into its current one (+0x798) again, if any
    0x48,0x89,0x5c,0x24,0x10, 0x48,0x89,0x6c,0x24,0x18, 0x48,0x89,0x74,0x24,0x20, 0x57, 0x41,0x54, 0x41,0x56,
    0x48,0x83,0xec,0x20, 0x48,0x8d,0xb1,0xb8,0x07,0x00,0x00, 0x4c,0x89,0x7c,0x24,0x40, 0x48,0x8b,0x46,0x08,
};
typedef void (*FnUnreserve)(BYTE*);
static FnUnreserve g_unreserve;

// Signals. A lead writes itself into the segments it stands on (segment
// +0x38 vector, its own +0x7B8; FUN_1406a6f50), and the way planner
// (FUN_1406b8eb0) plans no way through a segment someone else is written in:
// the train waits at the signal. Standing wagons are such a someone. For a
// locomotive without wagons they are struck out while it plans, and written
// back right after: it may run up to them and couple; any other train waits.
static const short SIG_OCCUPY[] = {     // FUN_1406a6f50(lead): into its segment and its trail's
    0x40,0x56, 0x57, 0x41,0x54, 0x41,0x56, 0x41,0x57, 0x48,0x83,0xec,0x20, 0x49,0x83,0xcf,0xff,
    0x48,0x89,0x5c,0x24,0x50, 0x4c,0x8b,0xf1, 0x48,0x89,0x6c,0x24,0x60, 0x48,0x8d,
};
static const short SIG_PLAN[] = {       // FUN_1406b8eb0(lead, ...): plan its way
    0x48,0x8b,0xc4, 0x88,0x50,0x10, 0x55, 0x56, 0x57, 0x41,0x54, 0x41,0x55, 0x41,0x56, 0x41,0x57,
    0x48,0x8d,0x68,0xa1, 0x48,0x81,0xec,0xe0,0x00,0x00,0x00, 0x48,0xc7,0x45,0x9f,0xfe,0xff,0xff,0xff,
};
#define PLAN_STOLEN 17
typedef void (*FnOccupy)(BYTE*);
typedef unsigned long long (*FnPlan)(BYTE* lead, char a, char b, void* c);
static FnOccupy g_occupy;
static FnPlan   g_origPlan;

// Wagons standing on their own: a lead with no locomotive in the consist, not in a building.
static bool StandingWagons(BYTE* v)
{
    BYTE *parent = (BYTE*)1, *building = (BYTE*)1, **w;
    if (!Rd(v, V_PARENT, &parent) || parent || !Rd(v, V_BUILDING, &building) || building) return false;
    if (Category(v) == CAT_LOCOMOTIVE) return false;   // the game may plan it a way: it cannot go anyway
    int n = Wagons(v, &w);
    for (int i = 0; i < n; ++i) if (Category(w[i]) == CAT_LOCOMOTIVE) return false;
    return n >= 0;
}

// The signal itself: FUN_1406ac6e0(lead) answers whether the train may go
// past the signal at the node it heads for (+0x6A0[+0x700], the node's +0xB8
// is the signal, its +0x14 the kind, 10 = none). A locomotive without wagons
// that is told to wait may go anyway when all that stands in the block behind
// the signal (its way up to the next signal) is wagons without a locomotive.
static const short SIG_SIGNAL[] = {
    0x40,0x53, 0x48,0x83,0xec,0x70, 0x80,0xb9,0xe0,0x05,0x00,0x00,0x00, 0x48,0x8b,0xd9, 0x74,0x08, 0xb0,0x01,
    0x48,0x83,0xc4,0x70, 0x5b, 0xc3, 0x48,0x8b,0x81,0xa8,0x06,0x00,
};
#define SIGNAL_STOLEN 16
typedef char (*FnSignal)(BYTE* lead);
static FnSignal g_origSignal;

static bool SignalAt(BYTE* node)
{
    BYTE* sig = 0;
    int kind = 10;
    return node && Rd(node, 0xB8, &sig) && sig && Rd(sig, 0x14, &kind) && kind != 10;
}

// A chain signal (+0x1AA 1 ... 3): the game's check looks past it along the
// way to the next block (FUN_1406ac6e0), so a block it bounds goes on there.
static bool ChainAt(BYTE* node)
{
    BYTE* sig = 0;
    BYTE type = 0;
    return SignalAt(node) && Rd(node, 0xB8, &sig) && Rd(sig, 0x1AA, &type) && (BYTE)(type - 1) < 3;
}

static bool StandingWagons(BYTE* v);

// The same node, or one at the same place (x, y, z at +4, +8, +12): a
// building's track has nodes of its own where it meets the network.
static bool NodesMeet(BYTE* a, BYTE* b)
{
    if (a == b) return true;
    float p[3], q[3];
    for (int i = 0; i < 3; ++i) if (!Rd(a, 4 + 4 * i, &p[i]) || !Rd(b, 4 + 4 * i, &q[i])) return false;
    float dx = p[0] - q[0], dy = p[1] - q[1], dz = p[2] - q[2];
    return dx * dx + dy * dy + dz * dz < 1.0f;
}

static BYTE* g_watch;
static void ClearWatch(void) { g_watch = 0; }

// After a coupling the cars must not be written in any segment; one was found
// written again a moment after (not known yet by whom). For 10 s every frame
// the new train's segments are checked and such a car struck out.
static DWORD g_watchUntil;

static void WatchCars(void)
{
    if (!g_watch || !g_unreserve) return;
    if ((int)(GetTickCount() - g_watchUntil) > 0) { g_watch = 0; return; }
    BYTE* v = g_watch;
    BYTE** veh;                                            // still there (track pulled up, train sold)
    int n = VehicleCount(&veh), i = 0;
    while (i < n && veh[i] != v) ++i;
    if (i >= n) { g_watch = 0; return; }
    for (int k = -1; k < RING_ENTRIES; ++k) {
        BYTE* seg = k < 0 ? *(BYTE**)(v + V_SEGMENT) : *(BYTE**)(v + V_RING + k * 16);
        if (!seg) break;
        BYTE *ob = 0, *oe = 0;
        Rd(seg, 0x38, &ob); Rd(seg, 0x40, &oe);
        for (BYTE** o = (BYTE**)ob; ob && o < (BYTE**)oe; ++o) {
            BYTE* parent = 0;
            if (*o && Rd(*o, V_PARENT, &parent) && parent) {
                Logf("railreverse  [couple] car %p (of %p) written in segment %p again, %u ms after coupling: struck out",
                     *o, parent, seg, 10000 - (g_watchUntil - GetTickCount()));
                g_unreserve(*o);
                return;                                    // the vector changed under us: next frame for the rest
            }
        }
    }
}

// Research: why a locomotive running round was not let past, once a second.
// Research: the signal nodes the last block walk stopped at, and the entry.
static BYTE* g_walkStops[8];
static int   g_walkStopCount;
static BYTE* g_walkEntry;

static void SignalDesc(BYTE* node, char* out, size_t cap)
{
    BYTE* sig = 0;
    int kind = -1;
    BYTE t8 = 0, t9 = 0, ta = 0, tb = 0, tc = 0;
    if (node) Rd(node, 0xB8, &sig);
    if (sig) { Rd(sig, 0x14, &kind); Rd(sig, 0x1A8, &t8); Rd(sig, 0x1A9, &t9); Rd(sig, 0x1AA, &ta); Rd(sig, 0x1AB, &tb); Rd(sig, 0x1AC, &tc); }
    _snprintf_s(out, cap, _TRUNCATE, "node %p signal %p kind %d, 1A8..1AC %d %d %d %d %d", node, sig, kind, t8, t9, ta, tb, tc);
}

static void SignalWhy(BYTE* lead, const char* why, BYTE* v, BYTE** block, int nblock)
{
    static DWORD last;
    if (!cfg.research || !LeftOf(lead) || GetTickCount() - last < 1000) return;
    last = GetTickCount();
    char line[1024];
    int len = _snprintf_s(line, sizeof(line), _TRUNCATE, "railreverse  [signal] locomotive %p waits: %s", lead, why);
    if (v) {
        BYTE *seg = 0, *bld = 0, **w;
        Rd(v, V_SEGMENT, &seg); Rd(v, V_BUILDING, &bld);
        len += _snprintf_s(line + len, sizeof(line) - len, _TRUNCATE, " %p (%d wagons, category %d, %s, building %p, on %p)",
                           v, Wagons(v, &w), Category(v), IsLoco(v) ? "locomotive" : "car", bld, seg);
    }
    len += _snprintf_s(line + len, sizeof(line) - len, _TRUNCATE, "; block:");
    for (int b = 0; b < nblock && len > 0 && len < 900; ++b)
        len += _snprintf_s(line + len, sizeof(line) - len, _TRUNCATE, " %p", block[b]);
    // and where the wagons left by this locomotive are
    BYTE** veh;
    int n = VehicleCount(&veh);
    for (int i = 0; i < n && len > 0 && len < 900; ++i) {
        BYTE* t = veh[i];
        if (!t || !StandingWagons(t)) continue;
        BYTE* seg = 0;
        float pos = 0.0f;
        Rd(t, V_SEGMENT, &seg); Rd(t, V_SEGMENT_POS, &pos);
        len += _snprintf_s(line + len, sizeof(line) - len, _TRUNCATE, "; wagons %p on %p at %.1f, trail %p", t, seg, pos,
                           *(BYTE**)(t + V_RING));
    }
    Logf("%s", line);
    char d[200];
    SignalDesc(g_walkEntry, d, sizeof(d));
    Logf("railreverse  [signal]   entry: %s", d);
    for (int k = 0; k < g_walkStopCount; ++k) {
        SignalDesc(g_walkStops[k], d, sizeof(d));
        Logf("railreverse  [signal]   stopped at: %s", d);
    }
}

static char HookSignal(BYTE* lead)
{
    char go = g_origSignal(lead);
    if (!go && lead && LeftOf(lead)) ++g_signalWaits;
    BYTE** w;
    if (go || !lead || !cfg.shunting || *(BYTE**)(lead + V_PARENT) || Wagons(lead, &w) != 0 || !IsLoco(lead)) return go;

    // the block behind the signal: from the segment past it, every segment
    // reached without going by another signal, points included (the way the
    // train has planned may end before the block does). A segment's ends are
    // nodes (+0x20, +0x28), a node's segments the vector at +0x28.
    BYTE *nb = 0, *ne = 0, *gb = 0, *ge = 0;
    int idx = -1;
    Rd(lead, V_ROUTE_BEGIN, &nb); Rd(lead, V_ROUTE_END, &ne); Rd(lead, V_SEGS_BEGIN, &gb); Rd(lead, V_SEGS_END, &ge);
    Rd(lead, V_ROUTE_INDEX, &idx);
    int nn = nb && ne > nb ? (int)((ne - nb) / 8) : 0, ng = gb && ge > gb ? (int)((ge - gb) / 8) : 0;
    if (idx < 0 || idx >= ng || idx >= nn || !ReadablePtr(nb, (size_t)nn * 8) || !ReadablePtr(gb, (size_t)ng * 8)) return go;
    BYTE* entry = ((BYTE**)nb)[idx];
    g_walkEntry = entry;
    g_walkStopCount = 0;
    // Waiting at a chain signal: the game looks on to the next block (FUN_1406ac6e0),
    // so the walk goes on past the next signal on the locomotive's way too.
    // only the first signal past it: one block more, not the whole way
    BYTE* past = 0;
    for (int j = idx + 1; ChainAt(entry) && j < nn && !past; ++j)
        if (SignalAt(((BYTE**)nb)[j]) && !ChainAt(((BYTE**)nb)[j])) past = ((BYTE**)nb)[j];
    BYTE* block[64];
    int nblock = 0;
    // the nodes the walk went through: a building's own track (a shunting yard
    // asset) ends at such a node but is not in its list of segments
    BYTE* reached[64];
    int nreached = 0;
    block[nblock++] = ((BYTE**)gb)[idx];
    for (int q = 0; q < nblock; ++q) {
        for (int end = 0; end < 2; ++end) {
            BYTE* node = 0;
            // a chain signal does not end the block: the wagons may stand beyond it
            if (!Rd(block[q], end ? 0x28 : 0x20, &node) || !node || node == entry) continue;
            if (SignalAt(node) && !ChainAt(node) && node != past) {
                bool seen = false;
                for (int k = 0; k < g_walkStopCount; ++k) seen |= g_walkStops[k] == node;
                if (!seen && g_walkStopCount < 8) g_walkStops[g_walkStopCount++] = node;
                continue;
            }
            bool known = false;
            for (int k = 0; k < nreached && !known; ++k) known = reached[k] == node;
            if (!known && nreached < 64) reached[nreached++] = node;
            BYTE *sb = 0, *se = 0;
            if (!Rd(node, 0x28, &sb) || !Rd(node, 0x30, &se) || !sb || se <= sb || !ReadablePtr(sb, (size_t)(se - sb))) continue;
            for (BYTE** sp = (BYTE**)sb; sp < (BYTE**)se && nblock < 64; ++sp) {
                bool seen = false;
                for (int b = 0; b < nblock && !seen; ++b) seen = block[b] == *sp;
                if (*sp && !seen) block[nblock++] = *sp;
            }
        }
    }

    // who stands there: a train's segment and as much of its trail as it is
    // long (the trail goes on far behind it)
    BYTE** veh;
    int n = VehicleCount(&veh), wagons = 0;
    for (int i = 0; i < n; ++i) {
        BYTE* v = veh[i];
        if (!v || v == lead || *(BYTE**)(v + V_PARENT) || *(BYTE**)(v + V_BUILDING)) continue;
        BYTE** vw;
        float rest = 0.0f, len = CarLength(v);
        int nw = Wagons(v, &vw);
        for (int k = 0; k < nw; ++k) len += CarLength(vw[k]);
        Rd(v, V_SEGMENT_POS, &rest);
        rest -= len;
        bool there = false;
        for (int k = -1; k < RING_ENTRIES && !there; ++k) {
            BYTE* seg = k < 0 ? *(BYTE**)(v + V_SEGMENT) : *(BYTE**)(v + V_RING + k * 16);
            if (!seg || (k >= 0 && rest >= 0.0f)) break;
            if (k >= 0) rest += SegLength(seg);
            for (int b = 0; b < nblock && !there; ++b) there = seg == block[b];
            // a building's track: one of its ends is where the walk went through
            for (int e = 0; e < 2 && !there; ++e) {
                BYTE* node = 0;
                if (!Rd(seg, e ? 0x28 : 0x20, &node) || !node) continue;
                for (int r = 0; r < nreached && !there; ++r) there = NodesMeet(reached[r], node);
            }
        }
        if (!there && cfg.research && StandingWagons(v)) {
            static DWORD last;
            if (GetTickCount() - last > 1000) {
                last = GetTickCount();
                BYTE *seg = *(BYTE**)(v + V_SEGMENT), *n[2] = {};
                float p[2][3] = {};
                if (seg) { Rd(seg, 0x20, &n[0]); Rd(seg, 0x28, &n[1]); }
                for (int e = 0; e < 2; ++e) if (n[e]) { Rd(n[e], 4, &p[e][0]); Rd(n[e], 8, &p[e][1]); Rd(n[e], 12, &p[e][2]); }
                char line[800];
                int len = _snprintf_s(line, sizeof(line), _TRUNCATE, "railreverse  [signal]   wagons %p on %p (ends %p at %.1f %.1f %.1f, "
                                      "%p at %.1f %.1f %.1f) not in the block; walk reached:", v, seg, n[0], p[0][0], p[0][1], p[0][2],
                                      n[1], p[1][0], p[1][1], p[1][2]);
                for (int r = 0; r < nreached && len > 0 && len < 700; ++r) {
                    float q[3] = {};
                    Rd(reached[r], 4, &q[0]); Rd(reached[r], 8, &q[1]); Rd(reached[r], 12, &q[2]);
                    len += _snprintf_s(line + len, sizeof(line) - len, _TRUNCATE, " %p at %.1f %.1f %.1f", reached[r], q[0], q[1], q[2]);
                }
                Logf("%s", line);
            }
        }
        if (!there) continue;
        if (!StandingWagons(v)) {                          // a train: wait, as the game says
            SignalWhy(lead, "a train in the block", v, block, nblock);
            return go;
        }
        ++wagons;
    }
    if (!wagons) { SignalWhy(lead, "nothing found in the block", 0, block, nblock); return go; }
    static BYTE* told;
    if (told != lead) {
        told = lead;
        Logf("railreverse  [signal] locomotive %p goes past the signal: only %d consist(s) of wagons in the block "
             "(%d segments)", lead, wagons, nblock);
    }
    return 1;
}

// The rail update (FUN_1406a7520) warns "trains cannot go on" (FUN_1404cc320,
// kind 1) when a slow train has another ahead whose blocks hold its segment.
// A locomotive running round and the wagons it left share the long track of a
// building (a shunting yard asset) right after the uncoupling: not a jam. The
// one call site is redirected; the 5th argument is the train (+0x390).
static const short SIG_JAM_CALL[] = {     // mov rax,[rsi+390h]; mov [rsp+20h],rax; mov r9d,1; mov r8d,r9d; lea rcx,world; call
    0x48,0x8b,0x86,0x90,0x03,0x00,0x00, 0x48,0x89,0x44,0x24,0x20, 0x41,0xb9,0x01,0x00,0x00,0x00, 0x45,0x8b,0xc1,
    0x48,0x8d,0x0d,-1,-1,-1,-1, 0xe8,-1,-1,-1,-1, 0x44,0x88,0xae,0x98,0x17,0x00,0x00,
};
#define JAM_CALL_AT  28
typedef void (*FnWarn)(BYTE* world, void* pos, int kind, int mode, BYTE* train, float radius, void* a7, void* a8);
static FnWarn g_origWarn;

static void HookJamWarning(BYTE* world, void* pos, int kind, int mode, BYTE* train, float radius, void* a7, void* a8)
{
    if (train && cfg.shunting && (LeftOf(train) || StandingWagons(train))) return;
    g_origWarn(world, pos, kind, mode, train, radius, a7, a8);
}

static unsigned long long HookPlan(BYTE* lead, char a, char b, void* c)
{
    BYTE* struck[64];
    int ns = 0;
    BYTE** w;
    if (lead && cfg.shunting && !*(BYTE**)(lead + V_PARENT) && Wagons(lead, &w) == 0 && IsLoco(lead)) {
        BYTE** veh;
        int n = VehicleCount(&veh);
        for (int i = 0; i < n && ns < 64; ++i) {
            BYTE* v = veh[i];
            if (!v || v == lead || *(BYTE**)(v + 0x7C0) == *(BYTE**)(v + 0x7B8) || !StandingWagons(v)) continue;
            BYTE* seg = *(BYTE**)(v + V_SEGMENT);           // out of its own segment too
            *(BYTE**)(v + V_SEGMENT) = 0;
            g_unreserve(v);
            *(BYTE**)(v + V_SEGMENT) = seg;
            struck[ns++] = v;
        }
    }
    unsigned long long r = g_origPlan(lead, a, b, c);
    if ((r & 0xFF) == 0 && lead && LeftOf(lead)) ++g_planFails;
    // written back only while still standing on their own: the planner may
    // turn the locomotive round, and turned round against them it couples them
    for (int i = 0; i < ns; ++i) if (StandingWagons(struck[i])) g_occupy(struck[i]);

    // research: a light locomotive that got no way - who is written in the segments around it
    if (lead && cfg.research && !*(BYTE**)(lead + V_PARENT) && Wagons(lead, &w) == 0 && IsLoco(lead) &&
        ((r & 0xFF) == 0 || *(BYTE**)(lead + 0x6A0) == *(BYTE**)(lead + 0x6A8))) {
        static DWORD last;
        if (GetTickCount() - last > 3000) {
            last = GetTickCount();
            char line[1024];
            int len = _snprintf_s(line, sizeof(line), _TRUNCATE, "railreverse  [plan] locomotive %p got no way (%llu), "
                                  "struck %d; written in its segment and trail:", lead, r, ns);
            for (int k = -1; k < RING_ENTRIES && len > 0 && len < 900; ++k) {
                BYTE* seg = k < 0 ? *(BYTE**)(lead + V_SEGMENT) : *(BYTE**)(lead + V_RING + k * 16);
                if (!seg) break;
                BYTE *ob = 0, *oe = 0;
                Rd(seg, 0x38, &ob); Rd(seg, 0x40, &oe);
                int no = ob && oe > ob ? (int)((oe - ob) / 8) : 0;
                for (int i = 0; i < no && i < 4; ++i) {
                    BYTE* o = ((BYTE**)ob)[i];
                    BYTE** ow;
                    len += _snprintf_s(line + len, sizeof(line) - len, _TRUNCATE, " seg%d:%p(%s%s,%d wagons)", k, o,
                                       o == lead ? "self" : IsLoco(o) ? "loco" : "other",
                                       o && StandingWagons(o) ? " standing" : "", o ? Wagons(o, &ow) : -1);
                }
            }
            Logf("%s", line);
            // and who is written where the standing wagons are
            BYTE** veh;
            int nv = VehicleCount(&veh);
            for (int j = 0; j < nv; ++j) {
                BYTE* t = veh[j];
                if (!t || !StandingWagons(t)) continue;
                len = _snprintf_s(line, sizeof(line), _TRUNCATE, "railreverse  [plan]   wagons %p (holds %d segments):", t,
                                  (int)((*(BYTE**)(t + 0x7C0) - *(BYTE**)(t + 0x7B8)) / 8));
                for (int k = -1; k < RING_ENTRIES && len > 0 && len < 900; ++k) {
                    BYTE* seg = k < 0 ? *(BYTE**)(t + V_SEGMENT) : *(BYTE**)(t + V_RING + k * 16);
                    if (!seg) break;
                    BYTE *ob = 0, *oe = 0;
                    Rd(seg, 0x38, &ob); Rd(seg, 0x40, &oe);
                    int no = ob && oe > ob ? (int)((oe - ob) / 8) : 0;
                    len += _snprintf_s(line + len, sizeof(line) - len, _TRUNCATE, " seg%d %p:", k, seg);
                    for (int i = 0; i < no && i < 4; ++i) {
                        BYTE* o = ((BYTE**)ob)[i];
                        len += _snprintf_s(line + len, sizeof(line) - len, _TRUNCATE, " %p%s", o,
                                           o == t ? "(itself)" : o == lead ? "(this locomotive)" : IsLoco(o) ? "(a locomotive)" : "");
                    }
                }
                Logf("%s", line);
            }
        }
    }
    static BYTE* told;
    if (cfg.research && ns && told != lead) {
        told = lead;
        Logf("railreverse  [plan] locomotive %p plans past %d standing consist(s) of wagons", lead, ns);
    }
    return r;
}

// The car this far in front of a lead (the game looks 1.5 m, 6.5 m, ... 31.5 m ahead).
static BYTE* AheadAt(BYTE* lead, float ahead, float* at)
{
    if (memcmp(g_exeBase + RVA_VEHICLE_AT, VEHICLE_AT_HEAD, sizeof(VEHICLE_AT_HEAD)) != 0) return 0;
    BYTE* seg = 0;
    float pos = 0.0f;
    if (!Rd(lead, V_SEGMENT, &seg) || !seg || !Rd(lead, V_SEGMENT_POS, &pos)) return 0;
    char dir = (char)*(lead + V_SEGMENT_DIR);
    *at = dir ? SegLength(seg) - pos - ahead : pos + ahead;
    return ((FnVehicleAt)(g_exeBase + RVA_VEHICLE_AT))(*(void**)(g_exeBase + RVA_TRACK_MAP), seg, dir, *at, 0, 0);
}

static BYTE* AheadOf(BYTE* lead, float* at) { return AheadAt(lead, 1.5f, at); }

// A locomotive without wagons running up to standing wagons: the game slows
// any train down to 50, 40, ... 10, 3 km/h when something stands 31.5, 26.5,
// ... 6.5 m ahead (FUN_1406a7520); for coupling it runs up faster, and
// walks only the last 3 m (the speeds were tried in game).
static void Approach(BYTE* lead)
{
    static const float cap[7] = { 0.0f, 3.0f, 10.0f, 20.0f, 30.0f, 40.0f, 50.0f };   // the game's, 1.5 + 5k m ahead
    BYTE** w;
    if (Wagons(lead, &w) != 0 || !IsLoco(lead)) return;
    float v = 0.0f, at;
    Rd(lead, V_SPEED, &v);
    if (v < 0.5f) return;
    // where the wagons begin: the game's 5 m steps, then half metres inside the step
    float d = -1.0f;
    int k = 0;
    for (; k <= 6 && d < 0.0f; ++k) {
        BYTE* hit = AheadAt(lead, 1.5f + 5.0f * k, &at);
        if (!hit) continue;
        BYTE* consist = 0;
        if (Rd(hit, V_PARENT, &consist) && !consist) consist = hit;
        if (!consist || consist == lead || IsLoco(consist)) return;          // a train: as the game does
        if (k == 0) return;                                                  // at them: the game stops it
        d = 1.5f + 5.0f * k;
        for (float x = d - 4.5f; x < d; x += 0.5f)
            if (AheadAt(lead, x, &at)) { d = x; break; }
    }
    if (d < 0.0f) return;
    --k;                                                   // the game's step: its cap
    // tried in game: fast up to 3 m, then a walk
    float ours = d <= 3.0f ? 3.5f : d <= 11.5f ? 15.0f : d <= 16.5f ? 35.0f : d <= 21.5f ? 40.0f : d <= 26.5f ? 45.0f : 55.0f;
    if (d <= 3.0f) { if (v > ours) *(float*)(lead + V_SPEED) = ours; }   // down to a walk at once
    else if (v >= cap[k] * 0.9f && v < ours) *(float*)(lead + V_SPEED) = ours;
}

// Right after the game turned the light locomotive away from the wagons it
// stood against: couple them behind it and give it a trail over them, the
// wagons are placed back from the lead along that trail. Touching the first
// car, the order stays; touching the last one (it ran round), the order and
// every car's direction turn over. The lookup names the consist, not the car
// (always its lead), so the end is told by where the touch is: hitAt on
// hitSeg, a point along the segment from its start.
static bool Couple(BYTE* loco, BYTE* hit, BYTE* hitSeg, float hitAt)
{
    BYTE* hitParent = 0;
    Rd(hit, V_PARENT, &hitParent);
    BYTE* tail = hitParent ? hitParent : hit;
    BYTE** w;
    int n = Wagons(tail, &w);
    if (n < 0 || !g_couple3) {
        Logf("railreverse  [couple] %p: touches %p (its train %p), not the wagons %p - not coupled",
             loco, hit, hitParent, tail);
        return false;
    }

    // The wagons' segments, front to back: the tail's own and its trail over its length.
    BYTE* segs[RING_ENTRIES + 1];
    BYTE dirs[RING_ENTRIES + 1];
    int m = 0;
    float len = CarLength(tail), pos = 0.0f;
    for (int i = 0; i < n; ++i) len += CarLength(w[i]);
    Rd(tail, V_SEGMENT, &segs[0]);
    Rd(tail, V_SEGMENT_POS, &pos);
    dirs[0] = *(tail + V_SEGMENT_DIR);
    for (float rest = pos - len; rest < 0.0f && m < RING_ENTRIES && *(BYTE**)(tail + V_RING + m * 16); ++m) {
        segs[m + 1] = *(BYTE**)(tail + V_RING + m * 16);
        dirs[m + 1] = *(tail + V_RING + m * 16 + 8);
        rest += SegLength(segs[m + 1]);
    }
    int count = m + 1;

    // Which end it touches: the nearer one, a point on a segment from its start.
    float rest = pos - len;
    for (int i = 1; i < count; ++i) rest += SegLength(segs[i]);
    float frontAt = dirs[0] ? SegLength(segs[0]) - pos : pos;
    float rearAt  = dirs[count - 1] ? SegLength(segs[count - 1]) - rest : rest;
    float toFront = hitSeg == segs[0] ? fabsf(hitAt - frontAt) : 1e9f;
    float toRear  = hitSeg == segs[count - 1] ? fabsf(hitAt - rearAt) : 1e9f;
    bool front = toFront <= toRear;
    if ((front ? toFront : toRear) > 10.0f) {
        Logf("railreverse  [couple] %p: touches the wagons at %.1f of %p, but their ends are at %.1f of %p and "
             "%.1f of %p - not coupled", loco, hitAt, hitSeg, frontAt, segs[0], rearAt, segs[count - 1]);
        return false;
    }
    bool rear = !front;

    // The locomotive's trail: those segments as it runs away from them.
    BYTE* lseg = 0;
    float lpos = 0.0f;
    Rd(loco, V_SEGMENT, &lseg);
    Rd(loco, V_SEGMENT_POS, &lpos);
    BYTE ring[RING_ENTRIES * 16];
    memset(ring, 0, sizeof(ring));
    int r = 0;
    for (int j = 0; j < count && r < RING_ENTRIES; ++j) {
        int i = front ? j : count - 1 - j;
        if (r == 0 && segs[i] == lseg) continue;
        *(BYTE**)(ring + r * 16) = segs[i];
        ring[r * 16 + 8] = front ? dirs[i] : (BYTE)!dirs[i];
        ++r;
    }

    // The wagons stop being a train: what they held is let go, as the game
    // does when a lead becomes a car (else the next train waits there); with
    // no current segment, so they are not written into that one again. The
    // locomotive gets a new trail: out of the segments of its old one first,
    // into those of the new one after.
    *(BYTE**)(tail + V_SEGMENT) = 0;
    if (g_unreserve) g_unreserve(tail);
    if (g_unreserve) g_unreserve(loco);
    g_couple3(0, loco, tail);
    BYTE** lw;
    int ln = Wagons(loco, &lw);
    if (rear && ln > 0) {
        for (int i = 0, j = ln - 1; i < j; ++i, --j) { BYTE* x = lw[i]; lw[i] = lw[j]; lw[j] = x; }
        for (int i = 0; i < ln; ++i) *(lw[i] + V_REVERSED) = !*(lw[i] + V_REVERSED);
    }
    memcpy(loco + V_RING, ring, sizeof(ring));
    if (g_occupy) g_occupy(loco);
    *(short*)(loco + 0xD3A) = 1;                           // a new train: its way planned again at once
    g_watch = loco;
    g_watchUntil = GetTickCount() + 10000;
    *(BYTE**)(tail + V_SEGMENT) = 0;
    *(int*)(tail + V_ROUTE_INDEX) = -1;
    memset(tail + V_RING, 0, sizeof(ring));
    *(tail + 0xD41) = 0;                    // blocked by the locomotive it had asked to reverse
    *(float*)(tail + V_SPEED) = 0.0f;
    LeftForget(loco);                                       // its wagons are back
    // It was heading for a waypoint beyond the wagons; turned round with them
    // it goes back the way it came: that waypoint is done, on to the next stop.
    BYTE *swp = 0;
    int sord = 0, sat = -1;
    void* skey = TrainStop(loco, &swp, &sord, &sat);
    BYTE *sb = 0, *se = 0;
    Rd(loco, 0x680, &sb); Rd(loco, 0x688, &se);
    int nstops = sb && se > sb ? (int)((se - sb) / 8) : 0;
    if (skey && swp && nstops > 0) {
        *(int*)(loco + 0x698) = (sat + 1) % nstops;
        *(short*)(loco + 0xD3A) = 1;                        // plan the way again, as the game does on passing a waypoint
        Logf("railreverse  [couple] %p: stop %d (the waypoint beyond the wagons) done, on to stop %d", loco, sat + 1,
             (sat + 1) % nstops + 1);
    }
    Logf("railreverse  [couple] %p: coupled %d cars at the %s end (touch at %.1f, front end %.1f m off, back end "
         "%.1f m off); locomotive on %p at %.1f, trail of %d over %d segments of the wagons (%.1f m)",
         loco, ln, front ? "front" : "back (ran round)", hitAt, toFront, toRear, lseg, lpos, r, count, len);
    return true;
}

static void MarkedStop(BYTE* lead, const float* at3);

static void HookXform(BYTE* v, float* pos, float yaw, float pitch)
{
    if (g_pendingLive) RestorePending();
    if (g_leftPendingLive) RestoreLeft();
    if (g_marksPending) ResolveMarks();
    if (cfg.shunting && g_standScan) StandScan();
    if (g_standCount) StandCheck();
    if (v && cfg.shunting && !*(BYTE**)(v + V_PARENT)) MarkedStop(v, pos);
    if (v && g_leftCount && !*(BYTE**)(v + V_PARENT)) LeftRun(v);
    if (v && v == g_turned && cfg.research) {
        if ((int)(GetTickCount() - g_turnedUntil) > 0) g_turned = 0;
        else RoundLog(v);
    }
    if (v && cfg.shunting && !*(BYTE**)(v + V_PARENT)) Approach(v);
    if (cfg.shunting) WatchCars();
    if (v && g_trainCount) {
        BYTE* lead = *(BYTE**)(v + V_PARENT);
        if (!lead && cfg.research) RouteDebug(v);
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

static void WindowButtons(BYTE* ui, BYTE* window, BYTE* lead);

// The wagon limit one step up or down, round in a ring: no limit -> 1 -> ...
// -> hub_limit_max -> no limit (keys and the train window button alike).
static int StepLimit(int limit, int step)
{
    if (step > 0) return limit >= cfg.hubLimitMax ? 0 : limit + 1;
    return limit <= 0 ? cfg.hubLimitMax : limit - 1;
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
    if (!g_build119) return;
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

// ---------------------------------------------------------------- route window buttons (ini shunting)
// Research for shunting: a "uncouple here" switch for a stop of a route. The
// route window (kind 8) is drawn by FUN_1407cff50, called from the window
// dispatcher at one site; that call is redirected here. Window fields (seen in
// FUN_1407cff50): +0x264 the selected stop, +0x270 the route being shown;
// route +0x210 stops (vector of building*, 0 for a waypoint), +0x228 a record
// per stop (0x198 bytes, +0x190 the waypoint for a stop without a building).
// 1.1.1.9 addresses (off on another build).
static const short SIG_ROUTE_CALL[] = {   // cmp ecx,8; jne; mov rdx,rsi; mov rcx,rdi; call FUN_1407cff50; jmp
    0x83,0xf9,0x08, 0x75,0x10, 0x48,0x8b,0xd6, 0x48,0x8b,0xcf, 0xe8,-1,-1,-1,-1, 0xe9,-1,-1,-1,-1, 0x83,0xf9,0x0a, 0x75,
};
#define ROUTE_CALL_AT  11
#define RW_SELECTED    0x264
#define RW_ROUTE       0x270
#define R_STOPS        0x210
#define R_STOPS_END    0x218
#define R_RECORDS      0x228
#define REC_SIZE       0x198
#define REC_WAYPOINT   0x190
#define RVA_INPUT      0xA54B90   // C3D_INPUT, as FUN_1407cff50 uses it
#define RVA_CLICK      0xA54E91   // u8: a left click this frame, cleared by whoever takes it
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
typedef void (*FnRouteWindow)(BYTE* ui, BYTE* window);
typedef float* (*FnMouse)(void* input, float* out);
typedef void* (*FnTexture)(void* middle, const char* name);
typedef int (*FnLoad)(void* tex, const char* path, int w, int h, unsigned mips, int format);
typedef void (*FnBind)(void* tex, int slot, void* sampler);
typedef void (*FnDraw)(void* panel, float u0, float v0, float u1, float v1, float rot, bool blend);
static FnRouteWindow g_origRoute;
static FnMouse   g_mouse;
static FnTexture g_texture;
static FnDraw    g_panelDraw;
#define ICONS 8
static void*     g_icons[ICONS];   // uncouple off, on; reverse off, on; push off, on; limit off, on
#define ICON_PUSH  4
#define ICON_LIMIT 6
// Stops marked "uncouple" or "stop and turn round" (kind). A waypoint by where it is: the route window shows
// a copy of the route, its waypoints are not the trains' ones. A route may go
// by the same waypoint (or building) more than once: the mark is for one
// visit, ord, counted from 0 in the order of the stops (-1: any, a mark from
// before visits were told apart). A mark is for one route: the route window
// shows it at +0x240, a train's lead has its route at +0x650; a route has its
// name at +0xC and its vehicles at +0x240 (FUN_1404945a0). In the sidecar the
// route is one of its vehicles' index and its name; a mark without a route
// (from before routes were told apart) is for every route.
#define ANY_VISIT -2
#define MARK_UNCOUPLE 0
#define MARK_REVERSE  1
#define W_ROUTE       0x240       // route window: the route itself (+0x270 is the copy being edited)
#define V_ROUTE       0x650       // lead: its route
#define ROUTE_NAME    0x0C        // route: wchar_t name
#define ROUTE_VEHICLES 0x240      // route: vector of vehicle*
#define ROUTE_NAME_MAX 64
struct Mark {
    void* building; float at[3]; int ord; int kind;
    BYTE* route;                  // 0: not known yet (see veh, name) or any route
    int veh;                      // from the sidecar: index of a vehicle on the route, -1 none
    wchar_t name[ROUTE_NAME_MAX]; // the route's name, "" for a mark of any route
};
static Mark g_marks[64];
static int  g_markCount;

static BYTE* RouteOf(BYTE* lead)
{
    BYTE* r = 0;
    return lead && Rd(lead, V_ROUTE, &r) ? r : 0;
}

static void RouteName(BYTE* route, wchar_t* out)
{
    out[0] = 0;
    if (!route) return;
    int n = 0;
    const wchar_t* p = (const wchar_t*)(route + ROUTE_NAME);
    while (n + 1 < ROUTE_NAME_MAX && ReadablePtr(p + n, 2) && p[n]) out[n] = p[n], ++n;
    out[n] = 0;
}

// Its route for a mark from the sidecar: by the vehicle, else by the name.
static void ResolveMarks(void)
{
    if (!g_marksPending) return;
    BYTE** veh = 0;
    int n = VehicleCount(&veh), open = 0;
    if (n <= 0) return;
    for (int i = 0; i < g_markCount; ++i) {
        Mark& m = g_marks[i];
        if (m.route || !m.name[0]) continue;
        if (m.veh >= 0 && m.veh < n && veh[m.veh]) m.route = RouteOf(veh[m.veh]);
        for (int k = 0; k < n && !m.route; ++k) {
            BYTE* r = veh[k] ? RouteOf(veh[k]) : 0;
            wchar_t name[ROUTE_NAME_MAX];
            RouteName(r, name);
            if (r && !wcscmp(name, m.name)) m.route = r;
        }
        if (!m.route) ++open;
    }
    g_marksPending = 0;
    if (open) Logf("railreverse  %d stop mark(s) whose route has no train now: matched by name later", open);
}

static void MarkOf(void* key, BYTE* wp, int ord, Mark* m)
{
    m->route = 0;
    m->veh = -1;
    m->name[0] = 0;
    m->building = wp ? 0 : key;
    m->at[0] = m->at[1] = m->at[2] = 0.0f;
    if (wp) { Rd(wp, 4, &m->at[0]); Rd(wp, 8, &m->at[1]); Rd(wp, 12, &m->at[2]); }
    m->ord = ord;
    m->kind = MARK_UNCOUPLE;
}

static bool SamePlace(const Mark& a, const Mark& b)
{
    return a.building ? a.building == b.building
                      : !b.building && fabsf(a.at[0] - b.at[0]) < 0.5f && fabsf(a.at[1] - b.at[1]) < 0.5f &&
                        fabsf(a.at[2] - b.at[2]) < 0.5f;
}

// Stop idx of a list of stops (building* each, 0 for a waypoint) with their
// records: its key, its waypoint, and which visit of that place it is.
static void* StopKey(BYTE** stops, BYTE* recs, int n, int idx, BYTE** wp, int* ord)
{
    *wp = 0; *ord = 0;
    if (!stops || idx < 0 || idx >= n || !ReadablePtr(stops, (size_t)n * 8)) return 0;
    BYTE* key = stops[idx];
    if (!key && recs) Rd(recs + (size_t)idx * REC_SIZE, REC_WAYPOINT, wp);
    if (!key) key = *wp;
    if (!key) return 0;
    Mark self, other;
    MarkOf(key, *wp, 0, &self);
    for (int j = 0; j < idx; ++j) {
        BYTE *ow = 0, *ok = stops[j];
        if (!ok && recs) Rd(recs + (size_t)j * REC_SIZE, REC_WAYPOINT, &ow);
        if (!ok) ok = ow;
        if (!ok) continue;
        MarkOf(ok, ow, 0, &other);
        if (SamePlace(self, other)) ++*ord;
    }
    return key;
}

// The current stop of a train: its key, waypoint and visit.
static void* TrainStop(BYTE* lead, BYTE** wp, int* ord, int* at)
{
    BYTE *sb = 0, *se = 0, *recs = 0;
    *at = -1;
    Rd(lead, 0x680, &sb); Rd(lead, 0x688, &se); Rd(lead, 0x638, &recs); Rd(lead, 0x698, at);
    int n = sb && se > sb ? (int)((se - sb) / 8) : 0;
    return StopKey((BYTE**)sb, recs, n, *at, wp, ord);
}

// In the save's sidecar, one line per waypoint:
// "uncouple|reverse <x> <y> <z> <visit> <vehicle> <route name>" (vehicle: the
// index of one on the route, -1 none; the name in UTF-8). A building is known
// by its address only, which a load changes: not kept.
static int MarksText(char* out, int cap, int* count)
{
    int len = 0;
    *count = 0;
    BYTE** veh = 0;
    int nv = VehicleCount(&veh);
    for (int i = 0; i < g_markCount && len >= 0 && len < cap; ++i) {
        const Mark& m = g_marks[i];
        if (m.building) continue;
        int vi = -1;
        for (int k = 0; k < nv && m.route && vi < 0; ++k)
            if (veh[k] && !*(BYTE**)(veh[k] + V_PARENT) && RouteOf(veh[k]) == m.route) vi = k;   // a lead
        if (vi < 0) vi = m.veh;                            // not resolved since the load: as it was
        char name[ROUTE_NAME_MAX * 3] = "";
        WideCharToMultiByte(CP_UTF8, 0, m.name, -1, name, sizeof(name), 0, 0);
        len += _snprintf_s(out + len, cap - len, _TRUNCATE, "%s %.3f %.3f %.3f %d %d %s\n",
                           m.kind == MARK_REVERSE ? "reverse" : "uncouple", m.at[0], m.at[1], m.at[2], m.ord, vi, name);
        ++*count;
    }
    return len > 0 ? len : 0;
}

static void LoadMark(const char* text, int kind)
{
    Mark m = {};
    m.kind = kind;
    m.ord = -1;                                            // a line from before visits were told apart: any
    m.veh = -1;
    int used = 0;
    int got = sscanf(text, "%f %f %f %d %d %n", &m.at[0], &m.at[1], &m.at[2], &m.ord, &m.veh, &used);
    if (g_markCount >= 64 || got < 3) return;
    if (got >= 5 && used > 0 && text[used])                // a route: from 0.6.0 on
        MultiByteToWideChar(CP_UTF8, 0, text + used, -1, m.name, ROUTE_NAME_MAX - 1);
    m.name[ROUTE_NAME_MAX - 1] = 0;
    if (m.name[0]) g_marksPending = 1;
    g_marks[g_markCount++] = m;
}

static void ClearMarks(void) { g_markCount = 0; g_marksPending = 0; }

// A mark of this route; ord ANY_VISIT: on any visit of that place.
static bool SameRoute(Mark& g, BYTE* route)
{
    if (g.route) return g.route == route;
    if (!g.name[0]) return true;                           // from before routes were told apart: any
    wchar_t name[ROUTE_NAME_MAX];                          // its route has had no train since the load
    RouteName(route, name);
    if (!route || wcscmp(name, g.name)) return false;
    g.route = route;
    return true;
}

static int MarkIndex(BYTE* route, void* key, BYTE* wp, int ord, int kind = MARK_UNCOUPLE)
{
    Mark m;
    MarkOf(key, wp, ord, &m);
    for (int i = 0; i < g_markCount; ++i) {
        Mark& g = g_marks[i];
        if (g.kind == kind && SamePlace(m, g) && (ord == ANY_VISIT || g.ord < 0 || g.ord == ord) && SameRoute(g, route))
            return i;
    }
    return -1;
}

// Stop idx of the shown route: its key (building or waypoint), or 0.
static void* StopAt(BYTE* window, int idx, int* count, BYTE** route, BYTE** wp, int* ord)
{
    *count = 0; *route = 0; *wp = 0; *ord = 0;
    BYTE *b = 0, *e = 0, *recs = 0;
    if (!Rd(window, RW_ROUTE, route) || !*route) return 0;
    if (!Rd(*route, R_STOPS, &b) || !Rd(*route, R_STOPS_END, &e) || e < b) return 0;
    *count = (int)((e - b) / 8);
    Rd(*route, R_RECORDS, &recs);
    return StopKey((BYTE**)b, recs, *count, idx, wp, ord);
}

// The selected stop of the shown route.
static void* SelectedStop(BYTE* window, int* idx, int* count, BYTE** route, BYTE** wp, int* ord)
{
    *idx = -1;
    Rd(window, RW_SELECTED, idx);
    return StopAt(window, *idx, count, route, wp, ord);
}

// The stop list's rows: FUN_1407cff50 draws each with
// C3D_PANEL2D::DrawFrom9Patch, one call per stop in order (the call at
// +7D2616). That import is hooked: right after the row of a marked stop, a
// small icon at its right end, drawn by the same panel (scrolled and clipped
// with the row).
#define EXP_9PATCH   "?DrawFrom9Patch@C3D_PANEL2D@@QEAAXUC3DRECT@@MMMMMM@Z"
#define RVA_ROW_DRAW 0x7D261C         // the return address of the stop rows' call
typedef void (*Fn9Patch)(void* panel, int* rect, float a, float b, float c, float d, float e, float f);
static Fn9Patch g_orig9Patch;
static BYTE* g_rowWindow;             // the route window being drawn, its next row
static int   g_rowIdx;
static void  RowIcon(BYTE* window, int idx, const int* rect);

static void Hook9Patch(void* panel, int* rect, float a, float b, float c, float d, float e, float f)
{
    g_orig9Patch(panel, rect, a, b, c, d, e, f);
    if (g_rowWindow && rect && (BYTE*)__builtin_return_address(0) == g_exeBase + RVA_ROW_DRAW)
        RowIcon(g_rowWindow, g_rowIdx++, rect);
}

// The icons sit next to this plugin's dll; loaded the way the game loads its buttons.
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
    const char* names[ICONS] = { "button_uncouple_off.png", "button_uncouple_on.png", "button_reverse_off.png",
                                 "button_reverse_on.png", "button_push_off.png", "button_push_on.png",
                                 "button_limit_off.png", "button_limit_on.png" };
    for (int i = 0; i < ICONS; ++i) {
        char path[MAX_PATH];
        // the game opens textures relative to its media folder ("editor/button_cancel.png")
        char full[MAX_PATH], media[MAX_PATH];
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

static void RowIcon(BYTE* window, int idx, const int* rect)
{
    int count;
    BYTE *route, *wp;
    int ord;
    void* key = StopAt(window, idx, &count, &route, &wp, &ord);
    if (!key || !LoadIcons()) return;
    float sc = 1.0f;
    Rd(g_exeBase, RVA_UI_SCALE, &sc);
    float size = sc * 30.0f, x = (float)rect[2] - sc * 60.0f;
    for (int kind = MARK_UNCOUPLE; kind <= MARK_REVERSE; ++kind) {
        if (MarkIndex(*(BYTE**)(window + W_ROUTE), key, wp, ord, kind) < 0) continue;
        DrawIcon(g_icons[kind * 2 + 1], x, (rect[1] + rect[3]) * 0.5f, size, 1.0f);
        x -= sc * 36.0f;
    }
}

// After the game drew the route window: two more buttons at the end of its
// row, drawn and clicked the way the game does its own (same panel, so they
// are clipped and scrolled with them). They act on the selected stop, a
// waypoint or a signal only (a station stops the train anyway).
static void RouteButton(BYTE* ui, BYTE* window, int kind)
{
    if (!LoadIcons()) return;
    float sc = 1.0f, wx = 0, wy = 0, ox = 0, oy = 0;
    Rd(g_exeBase, RVA_UI_SCALE, &sc);
    if (!Rd(window, W_POS_X, &wx) || !Rd(window, W_POS_Y, &wy) || !Rd(window, W_OFF_X, &ox) || !Rd(window, W_OFF_Y, &oy)) return;
    // the game's buttons: x0 + 20 * scale, 52 * scale apart, 50 * scale square, at y0 + 25 * scale.
    // "Delete all stops" comes third when the route (+1) and its copy have
    // stops (FUN_1407cff50); the buttons after it move one place on.
    BYTE *real = 0, *copy = 0, *cb = 0, *ce = 0;
    Rd(window, W_ROUTE, &real); Rd(window, RW_ROUTE, &copy);
    bool extra = real && real[1] && copy && Rd(copy, R_STOPS, &cb) && Rd(copy, R_STOPS_END, &ce) && ce > cb;
    float size = sc * 50.0f, x = wx + ox + sc * (20.0f + (6 + kind + (extra ? 1 : 0)) * 52.0f), y = wy + oy + sc * 25.0f;

    int idx, count;
    BYTE *route, *wp;
    int ord;
    void* key = SelectedStop(window, &idx, &count, &route, &wp, &ord);
    if (!wp) key = 0;                                      // a station: nothing to do here
    bool marked = key && MarkIndex(real, key, wp, ord, kind) >= 0;

    float m[3] = { 0, 0, 0 };
    g_mouse(g_exeBase + RVA_INPUT, m);
    // the panel's position is the button's centre
    bool hover = *(window + W_ACTIVE) && fabsf(m[0] - x) <= size / 2 && fabsf(m[1] - y) <= size / 2;

    DrawIcon(g_icons[kind * 2 + marked], x, y, size, !key ? 0.25f : hover ? 1.0f : 0.8f);   // the game's: dim when it can't act, bright under the mouse

    static BYTE* lastWindow;
    if (cfg.research && kind == MARK_UNCOUPLE && window != lastWindow) {
        lastWindow = window;
        Logf("railreverse  [route] window %p: route %p, %d stops, selected %d; at %.0f,%.0f + %.0f,%.0f, scale %.2f, "
             "button at %.0f,%.0f", window, route, count, idx, wx, wy, ox, oy, sc, x, y);
    }
    if (!hover) return;
    static const wchar_t* tips[2][2][3] = {
        { { L"Расцепка: выберите пункт маршрута или светофор",
            L"Расцепка в этом пункте: выкл.\r\nЛокомотив оставит здесь вагоны, объедет их\r\nпо следующим пунктам и прицепится с другой стороны.\r\nНажмите, чтобы включить",
            L"Расцепка в этом пункте: ВКЛ.\r\nНажмите, чтобы выключить" },
          { L"Стоп и разворот: выберите пункт маршрута или светофор",
            L"Стоп и разворот в этом пункте: выкл.\r\nПоезд встанет здесь и поедет дальше,\r\nразвернувшись, если нужно. Нажмите, чтобы включить",
            L"Стоп и разворот в этом пункте: ВКЛ.\r\nНажмите, чтобы выключить" } },
        { { L"Uncouple: select a waypoint or a signal",
            L"Uncouple here: off\r\nThe locomotive leaves its wagons here, runs round\r\nthem by the next stops and couples at the other end.\r\nClick to switch on",
            L"Uncouple here: ON\r\nClick to switch off" },
          { L"Stop and turn round: select a waypoint or a signal",
            L"Stop and turn round here: off\r\nThe train stops here and goes on,\r\nturned round if need be. Click to switch on",
            L"Stop and turn round here: ON\r\nClick to switch off" } },
    };
    wcsncpy((wchar_t*)(ui + UI_TOOLTIP), tips[T == &kRussian ? 0 : 1][kind][!key ? 0 : marked ? 2 : 1], 160);
    ((wchar_t*)(ui + UI_TOOLTIP))[159] = 0;

    BYTE* click = g_exeBase + RVA_CLICK;
    if (!*click || !key) return;
    *click = 0;                                            // taken: nothing under the window gets it
    int i = MarkIndex(real, key, wp, ord, kind);
    if (i >= 0) g_marks[i] = g_marks[--g_markCount];      // an old mark on every visit goes as a whole
    else if (g_markCount < 64) {
        MarkOf(key, wp, ord, &g_marks[g_markCount]);
        g_marks[g_markCount].route = real;
        RouteName(real, g_marks[g_markCount].name);
        g_marks[g_markCount++].kind = kind;
    }
    float pos[3] = { 0, 0, 0 };
    if (wp) { Rd(wp, 4, &pos[0]); Rd(wp, 8, &pos[1]); Rd(wp, 12, &pos[2]); }
    Logf("railreverse  [route] route %p stop %d of %d (%s %p, at %.1f %.1f %.1f, visit %d): %s %s", route, idx + 1,
         count, wp ? "waypoint" : "building", key, pos[0], pos[1], pos[2], ord + 1,
         kind == MARK_REVERSE ? "stop and turn round" : "uncouple", MarkIndex(real, key, wp, ord, kind) < 0 ? "OFF" : "ON");
}

// The train window: two buttons below the game's signal button. The upper one
// switches reversing for this train (as key_train), the lower one sets the
// most wagons a supply base hands it (left click +1, right click -1; past
// hub_limit_max and below 1: no limit). The keys keep working.
#define RVA_RCLICK 0xA54E93           // u8: a right click this frame (0xA54E90/91 left down/click, 92/93 right)

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

    bool h1 = Hover(window, x, y1, size), h2 = Hover(window, x, y2, size);
    // green while this train reverses: its own switch, or all trains of its class (Scripts menu)
    bool passenger = IsPassengerTrain(lead);
    bool classOn = passenger ? g_allPassenger != 0 : g_allFreight != 0;
    DrawIcon(g_icons[ICON_PUSH + (chosen || classOn ? 1 : 0)], x, y1, size, h1 ? 1.0f : 0.8f);
    DrawIcon(g_icons[ICON_LIMIT + (limit ? 1 : 0)], x, y2, size, h2 ? 1.0f : 0.8f);
    if (limit && g_font) {
        wchar_t n[8];
        _snwprintf_s(n, 8, _TRUNCATE, L"%d", limit);
        float tw = (float)wcslen(n) * g_fontPx * 0.55f;
        Text(x - tw / 2, y2 - size * 0.42f, limit ? 0xFF289628u : 0xFF3C3930u, n);
    }

    static BYTE* lastWindow;
    if (cfg.research && window != lastWindow) {
        lastWindow = window;
        Logf("railreverse  train window %p: at %.0f,%.0f + %.0f,%.0f, scale %.2f, buttons at %.0f,%.0f and %.0f,%.0f",
             window, wx, wy, ox, oy, sc, x, y1, x, y2);
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
            *click = 0;
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

// A train heading for a waypoint marked "uncouple": it brakes to stand just
// short of it, then leaves its wagons there. The waypoint is a node of its
// planned way (+0x6A0); the game counts it passed when the train heads for it
// (end of FUN_1406a7520), see MarkedStop.
#define V_STOPS_B      0x680      // vector of building*, 0 for a waypoint
#define V_STOPS_E      0x688
#define V_RECORDS_B    0x638      // a record per stop, REC_SIZE each
#define V_STOP_AT      0x698      // int: the stop it is heading for
#define STOP_SHORT_M   1.5f       // stands this far before the waypoint
#define BRAKE_MS2      1.2f       // braking towards it, m/s^2: from 40 km/h it starts ~50 m out

// Research: why a train with wagons is not taken, every 2 s.
static void Why(BYTE* lead, const char* why, int cur, int idx, int ng)
{
    static DWORD last;
    if (!cfg.research || GetTickCount() - last < 2000) return;
    BYTE** w;
    if (Wagons(lead, &w) <= 0) return;
    last = GetTickCount();
    Logf("railreverse  [stop] train %p not taken: %s (on segment %d of %d, route index %d; marks: %d, first at %.1f %.1f %.1f)",
         lead, why, cur, ng, idx, g_markCount, g_marks[0].at[0], g_marks[0].at[1], g_marks[0].at[2]);
}

// Is this waypoint one of the train's stops?
static bool InSchedule(BYTE* lead, BYTE* wp)
{
    BYTE *sb = 0, *se = 0, *recs = 0;
    if (!Rd(lead, V_STOPS_B, &sb) || !Rd(lead, V_STOPS_E, &se) || !Rd(lead, V_RECORDS_B, &recs) || !sb || !recs) return false;
    int n = se > sb ? (int)((se - sb) / 8) : 0;
    if (!ReadablePtr(sb, (size_t)n * 8)) return false;
    for (int i = 0; i < n; ++i) {
        BYTE* p = 0;
        if (!((BYTE**)sb)[i] && Rd(recs + (size_t)i * REC_SIZE, REC_WAYPOINT, &p) && p == wp) return true;
    }
    return false;
}

// Stand still and leave the wagons at this waypoint; the locomotive goes on
// to its next stop. That step is taken here: a pushing train is turned round
// first, and then the game no longer sees it at the waypoint. Returns whether
// the stop moved on.
static bool LeaveWagons(BYTE* lead, BYTE* wp)
{
    *(float*)(lead + V_SPEED) = 0.0f;
    LeftAdd(lead, wp);
    Logf("railreverse  [stop] train %p leaves its wagons at the marked waypoint %p", lead, wp);
    Uncouple(lead);
    BYTE *sb = 0, *se = 0, *recs = 0, *cur = 0;
    int at = -1;
    Rd(lead, V_STOPS_B, &sb); Rd(lead, V_STOPS_E, &se); Rd(lead, V_STOP_AT, &at); Rd(lead, V_RECORDS_B, &recs);
    int n = sb && se > sb ? (int)((se - sb) / 8) : 0;
    if (at < 0 || at >= n || !recs || ((BYTE**)sb)[at] || !Rd(recs + (size_t)at * REC_SIZE, REC_WAYPOINT, &cur) || cur != wp)
        return false;                                      // the game has already moved it on
    *(int*)(lead + V_STOP_AT) = (at + 1) % n;
    *(short*)(lead + 0xD3A) = 1;                           // plan the way again, as the game does on passing a waypoint
    Logf("railreverse  [stop] train %p: stop %d done, on to stop %d", lead, at + 1, (at + 1) % n + 1);
    return true;
}

// The game counts a waypoint passed as soon as the train enters the segment
// that ends at it (its stop index moves on long before it gets there), so the
// stop index is no guide: the marked waypoint is looked for among the nodes
// of the way ahead. Segment j of +0x6B8 runs from node j to node j + 1 of
// +0x6A0; the train is on the one that is +0x798 (+0x700 is the node it
// heads for).

static void TurnStop(BYTE* lead);

static void MarkedStop(BYTE* lead, const float* at3)
{
    if (!g_markCount) return;
    TurnStop(lead);
    BYTE** w;
    if (Wagons(lead, &w) <= 0) return;
    BYTE *nb = 0, *ne = 0, *gb = 0, *ge = 0, *seg = 0;
    int idx = -1;
    float pos = 0.0f;
    Rd(lead, V_ROUTE_BEGIN, &nb); Rd(lead, V_ROUTE_END, &ne); Rd(lead, V_SEGS_BEGIN, &gb); Rd(lead, V_SEGS_END, &ge);
    Rd(lead, V_ROUTE_INDEX, &idx); Rd(lead, V_SEGMENT, &seg); Rd(lead, V_SEGMENT_POS, &pos);
    int nn = nb && ne > nb ? (int)((ne - nb) / 8) : 0, ng = gb && ge > gb ? (int)((ge - gb) / 8) : 0;
    if (!seg || !ReadablePtr(nb, (size_t)nn * 8) || !ReadablePtr(gb, (size_t)ng * 8)) return;
    int cur = 0;
    while (cur < ng && ((BYTE**)gb)[cur] != seg) ++cur;
    if (cur >= ng) { Why(lead, "its segment is not on its planned way", -1, idx, ng); return; }

    // its current stop, the visit marked (the stop is held there, RR_PassWaypoint), on its way ahead
    BYTE *wp = 0, *swp = 0;
    int sord = 0, sat = -1;
    void* skey = TrainStop(lead, &swp, &sord, &sat);
    bool marked = skey && swp && MarkIndex(RouteOf(lead), swp, swp, sord) >= 0;
    float d = SegLength(seg) - pos;
    int k = cur + 1;
    for (; marked && k < nn && d < 400.0f; ++k) {
        if (((BYTE**)nb)[k] == swp) { wp = swp; break; }
        if (k < ng) d += SegLength(((BYTE**)gb)[k]);
    }
    if (!wp) {
        Why(lead, "no marked waypoint of its schedule ahead", cur, idx, ng);
        // research: what it heads for, and the nodes it has planned, every 2 s
        static DWORD last;
        BYTE *sb = 0, *se = 0, *recs = 0, *swp = 0;
        int at = -1;
        Rd(lead, V_STOPS_B, &sb); Rd(lead, V_STOPS_E, &se); Rd(lead, V_STOP_AT, &at); Rd(lead, V_RECORDS_B, &recs);
        int n = sb && se > sb && ReadablePtr(sb, (size_t)(se - sb)) ? (int)((se - sb) / 8) : 0;
        bool ours = false;                                 // only a train with the marked waypoint in its schedule
        for (int i = 0; i < n && recs && !ours; ++i) {
            BYTE* p = 0;
            if (!((BYTE**)sb)[i] && Rd(recs + (size_t)i * REC_SIZE, REC_WAYPOINT, &p) && p) ours = MarkIndex(RouteOf(lead), p, p, ANY_VISIT) >= 0;
        }
        if (cfg.research && ours && GetTickCount() - last >= 2000) {
            last = GetTickCount();
            BYTE* sbld = at >= 0 && at < n ? ((BYTE**)sb)[at] : 0;
            if (at >= 0 && at < n && recs) Rd(recs + (size_t)at * REC_SIZE, REC_WAYPOINT, &swp);
            float q[3] = { 0, 0, 0 };
            if (swp) { Rd(swp, 4, &q[0]); Rd(swp, 12, &q[2]); }
            char line[1024];
            int len = _snprintf_s(line, sizeof(line), _TRUNCATE,
                                  "railreverse  [stop]   heading for stop %d of %d (building %p, waypoint %p at %.1f %.1f); "
                                  "%d nodes, %d segments, on %d:", at + 1, n, sbld, swp, q[0], q[2], nn, ng, cur);
            for (int i = 0; i < nn && i < 12 && len > 0; ++i) {
                BYTE* node = ((BYTE**)nb)[i];
                float r[3] = { 0, 0, 0 };
                if (node) { Rd(node, 4, &r[0]); Rd(node, 12, &r[2]); }
                float mx = r[0] - g_marks[0].at[0], mz = r[2] - g_marks[0].at[2];
                len += _snprintf_s(line + len, sizeof(line) - len, _TRUNCATE, " [%d] %p %.0f,%.0f%s%s", i, node, r[0], r[2],
                                   node && node == swp ? " =stop" : "", mx * mx + mz * mz < 9.0f ? " =MARK" : "");
            }
            Logf("%s", line);
        }
        return;
    }
    Left* left = LeftOf(lead);
    if (left && left->wp == wp && left->run < AGAIN_M) return;   // it has just left its wagons here
    d -= STOP_SHORT_M;

    float v = 0.0f;
    Rd(lead, V_SPEED, &v);
    float vmax = d > 0.0f ? sqrtf(2.0f * BRAKE_MS2 * d) * 3.6f : 0.0f;
    if (v > vmax) *(float*)(lead + V_SPEED) = vmax;
    static DWORD last;
    if (cfg.research && d < 80.0f && GetTickCount() - last > 1000) {
        last = GetTickCount();
        Logf("railreverse  [stop] train %p: %.1f m to the marked waypoint %p (node %d), %.1f km/h, cap %.1f", lead, d, wp,
             k, v, vmax);
    }
    if (d > 0.5f) return;                                  // a standing train is not redrawn: act while it still rolls

    float w3[3] = { 0, 0, 0 };
    Rd(wp, 4, &w3[0]); Rd(wp, 8, &w3[1]); Rd(wp, 12, &w3[2]);
    float dx = at3 ? at3[0] - w3[0] : 0, dz = at3 ? at3[2] - w3[2] : 0;
    Logf("railreverse  [stop] train %p at the marked waypoint %p: locomotive at %.1f %.1f, waypoint at %.1f %.1f, %.1f m "
         "apart (way: %.1f m left, node %d of %d, on segment %d of %d, route index %d)",
         lead, wp, at3 ? at3[0] : 0, at3 ? at3[2] : 0, w3[0], w3[2], sqrtf(dx * dx + dz * dz), d, k, nn, cur, ng, idx);
    LeaveWagons(lead, wp);
}

// A train heading for its stop marked "stop and turn round" brakes to stand
// at it, as for a marked uncouple; the game then turns it round there and it
// sets off again from standing (the speed is zeroed in HookFlip).
#define TURN_CREEP_KMH 1.5f           // the last metre: it has to get there, not stop just short

static void TurnStop(BYTE* lead)
{
    BYTE* swp = 0;
    int sord = 0, sat = -1;
    void* skey = TrainStop(lead, &swp, &sord, &sat);
    if (!skey || !swp || MarkIndex(RouteOf(lead), swp, swp, sord, MARK_REVERSE) < 0) return;
    BYTE *nb = 0, *ne = 0, *gb = 0, *ge = 0, *seg = 0;
    float pos = 0.0f;
    Rd(lead, V_ROUTE_BEGIN, &nb); Rd(lead, V_ROUTE_END, &ne); Rd(lead, V_SEGS_BEGIN, &gb); Rd(lead, V_SEGS_END, &ge);
    Rd(lead, V_SEGMENT, &seg); Rd(lead, V_SEGMENT_POS, &pos);
    int nn = nb && ne > nb ? (int)((ne - nb) / 8) : 0, ng = gb && ge > gb ? (int)((ge - gb) / 8) : 0;
    if (!seg || !ReadablePtr(nb, (size_t)nn * 8) || !ReadablePtr(gb, (size_t)ng * 8)) return;
    int cur = 0;
    while (cur < ng && ((BYTE**)gb)[cur] != seg) ++cur;
    if (cur >= ng) return;
    float d = SegLength(seg) - pos;
    int k = cur + 1;
    for (; k < nn && d < 400.0f; ++k) {
        if (((BYTE**)nb)[k] == swp) break;
        if (k < ng) d += SegLength(((BYTE**)gb)[k]);
    }
    if (k >= nn || d >= 400.0f) return;
    float v = 0.0f;
    Rd(lead, V_SPEED, &v);
    float vmax = d > 0.0f ? sqrtf(2.0f * BRAKE_MS2 * d) * 3.6f : 0.0f;
    if (vmax < TURN_CREEP_KMH) vmax = TURN_CREEP_KMH;
    if (v > vmax) *(float*)(lead + V_SPEED) = vmax;
}

// Holding a marked waypoint. At the end of FUN_1406a7520 the game counts a
// waypoint stop passed once the node the train heads for (+0x6A0[+0x700]) is
// that waypoint: its stop moves on and its way is planned to the next stop,
// long before it gets there. For a marked one, with wagons on, that check is
// answered "not yet": the train keeps heading for the waypoint, the game
// brakes it to stand there as at the end of any way, and once it stands it
// leaves its wagons and the check goes through. 1.1.1.9 (off on another build).
static const short SIG_PASS[] = {     // imul r8,rdx,198h; ...; cmp [r8+rdx+190h],rcx; jne; lea ecx,[r9+1]; mov [rsi+698h],ecx
    0x4c,0x69,0xc2,0x98,0x01,0x00,0x00, 0x48,0x8b,0x96,0x38,0x06,0x00,0x00, 0x48,0x8b,0x07, 0x48,0x8b,0x0c,0xc8,
    0x49,0x39,0x8c,0x10,0x90,0x01,0x00,0x00, 0x75,0x31, 0x41,0x8d,0x49,0x01, 0x89,0x8e,0x98,0x06,0x00,0x00,
};
#define PASS_CMP_AT 21                // the cmp + jne, 10 bytes: call stub; test al,al; je +32h; nop

// How far the train still is from the node it heads for (+0x6A0[+0x700]):
// segment j of +0x6B8 runs from node j to node j + 1. 0 once it is past it,
// -1 when its segment is not on its way.
static float ToNode(BYTE* v)
{
    BYTE *gb = 0, *ge = 0, *seg = 0;
    int idx = -1;
    float pos = 0.0f;
    Rd(v, V_SEGS_BEGIN, &gb); Rd(v, V_SEGS_END, &ge); Rd(v, V_ROUTE_INDEX, &idx); Rd(v, V_SEGMENT, &seg);
    Rd(v, V_SEGMENT_POS, &pos);
    int ng = gb && ge > gb ? (int)((ge - gb) / 8) : 0;
    if (!seg || !ReadablePtr(gb, (size_t)ng * 8)) return -1.0f;
    int cur = 0;
    while (cur < ng && ((BYTE**)gb)[cur] != seg) ++cur;
    if (cur >= ng) return -1.0f;
    if (cur >= idx) return 0.0f;
    float d = SegLength(seg) - pos;
    for (int j = cur + 1; j < idx && j < ng; ++j) d += SegLength(((BYTE**)gb)[j]);
    return d;
}

#define AT_WAYPOINT_M 5.0f            // a locomotive running round has got to a waypoint this close
#define STOOD_MS      10000           // or after standing this long short of it (a signal, a train)

extern "C" int RR_PassWaypoint(BYTE* v, BYTE* recWp, BYTE* node)
{
    if (recWp != node) return 0;                           // the game's own test
    Left* left = LeftOf(v);
    if (left && left->wp == node && left->run < AGAIN_M) return 1;   // left its wagons here: on it goes
    BYTE** lw;
    if (left && Wagons(v, &lw) == 0) {                     // signals taken as stops too
        // Running round its wagons: the waypoints are where it turns, it
        // has to get there (the game would count it passed as soon as it
        // enters the segment and plan on from the middle of it). Its way
        // ends at the waypoint, so the game stops it there.
        float d = ToNode(v), speed = 0.0f;
        Rd(v, V_SPEED, &speed);
        DWORD now = GetTickCount();
        if (d >= 0.0f && d < AT_WAYPOINT_M) { left->stood = 0; return 1; }
        if (speed > 0.5f) { left->stood = 0; return 0; }
        if (!left->stood) left->stood = now;
        if (now - left->stood < STOOD_MS) return 0;
        left->stood = 0;
        Logf("railreverse  [stop] locomotive %p stood %.1f m short of waypoint %p: counted passed", v, d, node);
        return 1;
    }
    BYTE* swp = 0;
    int sord = 0, sat = -1;
    TrainStop(v, &swp, &sord, &sat);
    if (g_markCount && swp == node && MarkIndex(RouteOf(v), node, node, sord, MARK_REVERSE) >= 0) {
        // Stop and turn round: the stop counts once the train is there (its way
        // ends there, the game stops it), the way on is planned from there.
        float d = ToNode(v), speed = 0.0f;
        Rd(v, V_SPEED, &speed);
        if (speed > 0.5f) return 0;                        // it is to stop there, not roll by
        if (d >= 0.0f && d < AT_WAYPOINT_M) return 1;
        static BYTE* stoodLead;
        static DWORD stoodSince;
        if (stoodLead != v) stoodLead = v, stoodSince = GetTickCount();
        if (GetTickCount() - stoodSince < STOOD_MS) return 0;
        stoodLead = 0;
        Logf("railreverse  [stop] train %p stood %.1f m short of its stop-and-turn waypoint %p: counted passed", v, d, node);
        return 1;
    }
    if (!g_markCount || swp != node || MarkIndex(RouteOf(v), node, node, sord) < 0) return 1;
    BYTE** w;
    if (Wagons(v, &w) <= 0) return 1;
    float speed = 0.0f;
    Rd(v, V_SPEED, &speed);
    if (speed > 0.5f) return 0;                            // still on its way to it
    Logf("railreverse  [stop] train %p stands at the marked waypoint %p", v, node);
    return LeaveWagons(v, node) ? 0 : 1;                   // moved on already: the game must not do it again
}

// rsi = the vehicle, rcx = the node it heads for, [r8+rdx+190h] = its stop's
// waypoint. r9 (the stop index) and r13 are used after the call.
extern "C" __attribute__((naked)) void RR_PassThunk(void)
{
    __asm__ volatile(
        "push %r9\n\t"
        "push %r8\n\t"
        "push %rdx\n\t"
        "push %rcx\n\t"
        "push %r10\n\t"
        "push %r11\n\t"
        "sub $0x28, %rsp\n\t"
        "mov 0x190(%r8,%rdx), %rdx\n\t"
        "mov %rcx, %r8\n\t"
        "mov %rsi, %rcx\n\t"
        "call RR_PassWaypoint\n\t"
        "add $0x28, %rsp\n\t"
        "pop %r11\n\t"
        "pop %r10\n\t"
        "pop %rcx\n\t"
        "pop %rdx\n\t"
        "pop %r8\n\t"
        "pop %r9\n\t"
        "ret\n\t");
}

extern "C" void RR_RouteWindow(BYTE* ui, BYTE* window)
{
    g_rowWindow = window;
    g_rowIdx = 0;
    g_origRoute(ui, window);
    g_rowWindow = 0;
    if (ui && window) {
        RouteButton(ui, window, MARK_UNCOUPLE);
        RouteButton(ui, window, MARK_REVERSE);
    }
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
    cfg.shunting     = H->configInt(RR_INI, "railreverse", "shunting", 1);
    cfg.research     = H->configInt(RR_INI, "railreverse", "research_log", 0);
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
        cfg.windowButtons = cfg.shunting = 0;
        Logf("railreverse  train window buttons and shunting unavailable (engine exports missing)");
    }
    // The buttons, the Scripts menu switches and shunting use fixed 1.1.1.9
    // addresses besides their signatures (windows, input, the running
    // scripts, the obstacle map): on another build of the game they stay off.
    {
        IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(g_exeBase + ((IMAGE_DOS_HEADER*)g_exeBase)->e_lfanew);
        g_build119 = nt->FileHeader.TimeDateStamp == BUILD_1119;
        if (!g_build119) {
            cfg.windowButtons = cfg.shunting = 0;
            Logf("railreverse  game build %08lX is not 1.1.1.9 (%08X): train window buttons, Scripts menu switches and "
                 "shunting off, reversing works", (unsigned long)nt->FileHeader.TimeDateStamp, BUILD_1119);
        }
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
    if (cfg.shunting) {
        g_split = (FnSplit)FindUnique(SIG(SIG_SPLIT));
        g_blockState = (FnBlockState)FindUnique(SIG(SIG_BLOCKSTATE));
        if (!g_blockState) Logf("railreverse  block state not found: signals may stay red where wagons were sold");
        g_couple3 = (FnCouple)FindUnique(SIG(SIG_COUPLE));
        g_unreserve = (FnUnreserve)FindUnique(SIG(SIG_UNRESERVE));
        if (!g_unreserve) Logf("railreverse  block release not found: coupled wagons may keep their blocks");
        g_occupy = (FnOccupy)FindUnique(SIG(SIG_OCCUPY));
        if (g_unreserve && g_occupy)
            Hook(SIG(SIG_PLAN), PLAN_STOLEN, (void*)HookPlan, (void**)&g_origPlan, "way planner");
        else
            Logf("railreverse  way planner not hooked: light locomotives wait at signals before wagons");
        Hook(SIG(SIG_SIGNAL), SIGNAL_STOLEN, (void*)HookSignal, (void**)&g_origSignal, "signal check");
        // no "trains cannot go on" warning for a locomotive running round and its wagons
        if (BYTE* jam = FindUnique(SIG(SIG_JAM_CALL))) {
            BYTE* at = jam + JAM_CALL_AT;
            BYTE* hop = AllocNear(at, 16);
            if (hop) {
                g_origWarn = (FnWarn)(at + 5 + *(INT32*)(at + 1));
                hop[0] = 0xFF; hop[1] = 0x25; *(INT32*)(hop + 2) = 0;
                *(void**)(hop + 6) = (void*)HookJamWarning;
                DWORD old;
                VirtualProtect(at, 5, PAGE_EXECUTE_READWRITE, &old);
                *(INT32*)(at + 1) = (INT32)(hop - (at + 5));
                VirtualProtect(at, 5, old, &old);
                FlushInstructionCache(GetCurrentProcess(), at, 5);
                Logf("railreverse  jam warning: call at +%06llX redirected", (unsigned long long)(at - g_exeBase));
            }
        } else {
            Logf("railreverse  jam warning: call site not found");
        }
        Logf("railreverse  uncouple test (CTRL+F11): %s, couple %s, obstacle lookup %s", g_split ? "on" : "split not found",
             g_couple3 ? "on" : "not found",
             memcmp(g_exeBase + RVA_VEHICLE_AT, VEHICLE_AT_HEAD, sizeof(VEHICLE_AT_HEAD)) ? "not found" : "on");

        // holding marked waypoints: the game's "waypoint passed" test calls RR_PassThunk
        if (BYTE* pass = FindUnique(SIG(SIG_PASS))) {
            BYTE* at = pass + PASS_CMP_AT;
            BYTE* hop = AllocNear(at, 16);
            if (hop) {
                hop[0] = 0xFF; hop[1] = 0x25; *(INT32*)(hop + 2) = 0;
                *(void**)(hop + 6) = (void*)RR_PassThunk;
                BYTE code[10] = { 0xE8, 0, 0, 0, 0, 0x84, 0xC0, 0x74, 0x32, 0x90 };
                *(INT32*)(code + 1) = (INT32)(hop - (at + 5));
                DWORD old;
                VirtualProtect(at, 10, PAGE_EXECUTE_READWRITE, &old);
                memcpy(at, code, 10);
                VirtualProtect(at, 10, old, &old);
                FlushInstructionCache(GetCurrentProcess(), at, 10);
                Logf("railreverse  marked waypoints: pass test at +%06llX redirected", (unsigned long long)(at - g_exeBase));
            }
        } else {
            Logf("railreverse  marked waypoints: pass test not found");
        }

        // the route window button: its one call site goes through a stub to RR_RouteWindow
        BYTE* site = FindUnique(SIG(SIG_ROUTE_CALL));
        BYTE* stub = site && g_mouse && g_texture && g_panelDraw ? AllocNear(site, 16) : 0;
        if (stub) {
            site += ROUTE_CALL_AT;
            g_origRoute = (FnRouteWindow)(site + 5 + *(INT32*)(site + 1));
            stub[0] = 0xFF; stub[1] = 0x25; *(INT32*)(stub + 2) = 0;
            *(void**)(stub + 6) = (void*)RR_RouteWindow;
            DWORD old;
            VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old);
            *(INT32*)(site + 1) = (INT32)(stub - (site + 5));
            VirtualProtect(site, 5, old, &old);
            FlushInstructionCache(GetCurrentProcess(), site, 5);
            Logf("railreverse  route window button: call at +%06llX redirected", (unsigned long long)(site - g_exeBase));
            if (!PatchIat(g_exe, DLL_ENGINE, EXP_9PATCH, (void*)Hook9Patch, (void**)&g_orig9Patch, "railreverse 9-patch"))
                Logf("railreverse  route window rows: no marks on the rows (9-patch import not patched)");
        } else {
            Logf("railreverse  route window button unavailable (%s)", !site ? "call site not found" : "engine exports missing");
        }
    }
    Logf("railreverse  ready");
    return 0;
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }

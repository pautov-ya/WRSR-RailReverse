# Findings for 0.2–0.3 — saves, the train window, text — SOVIET64.exe 1.1.1.9

RVAs, image base `0x140000000`.

## Saves

| RVA | What |
|---|---|
| `0x42CC70` | save the world (`param_1` = game object `0x1409D4F10`). Writes every `%s/*.bin` with `fopen(…,"wb")` |
| `0x42F3E0` | load, pass 1: `operator new(0x1ad8)` + `FUN_140663720` per vehicle into the vehicle list |
| `0x430FC0` | load, pass 2: `FUN_1406c81d0(vehicle, FILE*)` per vehicle, then `vehicle+4 = index` |
| `0x6C6770` | write one vehicle (`FUN_1406c6770(vehicle, FILE*)`) |
| `0x9E7720` | the vehicle list `{begin, end}` (= game object `+0x12810`), walked in the same order by save and load |

So a vehicle's index in that list at save time is its index after the load. `+0x4C4` (car reversed)
is part of the saved vehicle; the 1a note calling its writers "GUI code" was wrong — they are these
save/load functions.

The plugin keeps its own state in `railreverse.txt` next to `vehicles.bin`, written and read when the
game `fopen`s `vehicles.bin` (`"wb"` save / `"rb"` load). `fopen` is taken through the import table
(`api-ms-win-crt-stdio-l1-1-0.dll`). The vehicle list is found by the load loop:
`mov rax,[rip+end]; mov rcx,[rip+begin]; … call <read>; mov [rbx+4],edi` (`0x43356D`).

## The train window

`FUN_1406e9b80` walks the open info windows and dispatches on `window+0x248` (kind):
0 building, 1 **vehicle** → `FUN_14080cc60(ui, window)`, 2, 3 (district: `+0x1A0` buildings), 5, 7, 8,
10, 11, 12–14. `window+0x240` is the object shown. Window position: `+0x04/+0x08`, plus `+0x28/+0x2C`
(from DockDistributionOffice). UI scale: float at `0x992088`.

For kind 1 the dispatcher (`0x6EA93B`) calls `FUN_14080cc60` only when `window+0x23C` is 9 or 10;
every other vehicle window is drawn by **`FUN_1407d5460(ui, window)`** (`call` at `0x6EA958`,
`rcx` = ui, `rdx` = window). 0.3.0 first hooked only `0x80CC60` and never saw a train window.
Both are hooked now.

Prologues (20 B stolen, no RIP-relative), then `b8 <size> e8 <__chkstk> 48 2b e0`:
- `0x7D5460`: `40 55 56 57 41 54 41 55 41 56 41 57 48 8d ac 24 50 d1 ff ff`, size `0x2FC0`
- `0x80CC60`: `40 55 56 57 41 54 41 55 41 56 41 57 48 8d ac 24 90 e5 ff ff`, size `0x1B70`

## Text

`C3D_FONTMANAGER::PrintLeftUnicodeNoArg(manager, font, x, y, ARGB, const wchar_t*)` is a C3DDLL64
import. The game calls it 3 times; two sites agree on `lea rcx,[0x996FB0]` (manager) and
`mov rdx,[0x994208]` (font). The plugin reads both off those call sites at start.

## Depots

`lead+0x4F0` is the building the train is at. When that building's `+0x5D0` is zero the wagon
placement (`FUN_1406a5410`) swallows the cars into it — a depot / distribution office, not a
station. A pushing train is switched back to pulling there.

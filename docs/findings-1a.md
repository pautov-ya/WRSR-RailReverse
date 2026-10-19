# Stage 1a findings — SOVIET64.exe 1.1.1.9

SHA-256 `296644a9f207d609031fc2ae73fed2dcb34619a1d55a35d1c7b51965ce6841b8`, image base `0x140000000`.
RVAs below; Ghidra project `C:\WRSR-Dev\re\ghidra-proj`, decompiler output in `C:\WRSR-Dev\re\out`.

## Key functions

| RVA | What | Notes |
|---|---|---|
| `0x6A7520` | per-frame rail update of a train lead (21 KB) | RailPhysics sites: curve `0x6A8436`, brake `0x6A8553` |
| `0x6BC9A0` | **the flip** (`FUN_1406bc9a0(lead, char routeMode, char extend)`) | see below |
| `0x6A5410` | per-frame placement of one wagon (`param_1` = wagon) | returns early if `wagon+0x4F0 != 0` (inside a building) |
| `0x6A52A0` | `sample(lead, float3* out, float metresBehindHead, ...)` — point on the train's trail | walks the occupancy ring; out = 0,0,0 past its end |
| `0x54BEE0` | point at a distance on one track segment | used by wagon and lead placement |
| `0x6AFDE0` | `setTransform(vehicle, float3* pos, float yaw, float pitch)` — every car, lead and wagon | if `+0x4C4`: yaw += pi, pos shifted, pitch negated. 8 callers |

## The flip (`0x6BC9A0`)

1. Train length `L = type(lead)+0x7A78 + sum type(wagon)+0x7A78` over `lead+0x3A0`.
2. Walks back `L` metres from the head along the occupancy ring (`lead+0x7D0`, 75 x {segment*, u8 dir}).
3. Puts the lead there: `+0x798` = tail segment, `+0x7A4` = position, `+0x7A0` = inverted tail direction.
4. Clears and rebuilds the ring and the route (`+0x6A0`, `+0x6B8`, `+0x700`) for the new direction.

It does **not** touch the wagon order (`+0x3A0`) or any `+0x4C4`. The "teleport" is only the head
moving to the old tail: the lead is drawn there and the wagons trail behind it.
Nothing persistent marks a flipped train, so a "propelling" state must be kept by the plugin.

Callers: rail update `0x6A8A27`, `0x6A9475`, `0x6A9655` (end of route), plus `0x2593D0`, `0x3FBBA0` (jmp),
`0x67FA20`, `0x680A20` (not yet identified — UI / manual reverse / re-pathing).

`+0x4C4` is written only at `0x663B1E` and `0x6C858A`, both reached from GUI code (`0x42xxxx–0x43xxxx`):
the player turning a car in the consist editor.

## Wagon placement (`0x6A5410`, main branch)

```
a  = len(lead) + sum len(wagons before this one)          // metres behind the head
pf = reversed ? type+0x7AFC : type+0x7AF8                 // front pivot inset
pb = reversed ? type+0x7AF8 : type+0x7AFC                 // back pivot inset
front = trail(a + pf);  back = trail(a + len - pb)
dir   = normalize(front - back);  (pitch, yaw) = C3DVec3GetRotationXY(dir)
pos   = front - dir * (type+0x7A7C - pf)
setTransform(wagon, pos, yaw, pitch)
```

## Plan consequence: propelling = mirrored placement

For a train flagged "propelling", place every car (lead included) with `d -> L - d`:
`front = trail(L - (a + pf))`, `back = trail(L - (a + len - pb))`, same formulas otherwise.
Each car lands on the mirror image of its slot on the same stretch of track, and its heading
reverses by itself. Game logic (head, ring, signals, collisions) is untouched.

## Hook sites (entry hooks, no RIP-relative bytes in the stolen window)

| Site | Stolen | Bytes |
|---|---|---|
| `0x6AFDE0` setTransform | 20 B (4 insns) | `48 89 5c 24 08 57 48 81 ec 80 00 00 00 80 b9 c4 04 00 00 00` |
| `0x6BC9A0` flip | 19 B (5 insns) | `4c 8b dc 55 53 49 8d ab a8 fb ff ff 48 81 ec 48 05 00 00` |

RailPhysics patches the `call [rip+disp32]` *inside* `0x6AFDE0` (at `0x6AFED6`), not its entry — compatible.

C3DDLL64 exports used by name: `?C3DVec3GetRotationXY@@YAXPEAVC3DVECTOR3@@PEBV1@@Z`,
`?C3DVec3Normalize@@YAPEAVC3DVECTOR3@@PEAV1@PEBV1@@Z`.

## Open for stage 1b (in game)

- Confirm the flip hook fires once per terminus reversal and what `routeMode` / `extend` are.
- Confirm `trail(L - d)` stays valid for the whole train right after a flip (ring rebuilt).
- Identify the four non-rail callers of the flip.
- Lead placement in `0x6A7520` (calls at `0x6ABD9A`, `0x6AC5DE`) — check it matches the wagon formula.

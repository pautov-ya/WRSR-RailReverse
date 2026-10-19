# railreverse

A plugin for **Workers & Resources: Soviet Republic** 1.1.1.9. When a train reverses, the locomotive
stays where it is and pushes its wagons back, instead of jumping to the other end of the train.

Game logic is untouched: after the game's own reversal the train is marked as pushing and every car
is drawn on the mirror image of its place on the same stretch of track. While pushing, the train is
limited to `push_speed_kmh`.

Loaded through [Republic Mod Loader](https://steamcommunity.com/sharedfiles/filedetails/?id=3787969749)
(Tesmio API 4).

## Build

```sh
./build.sh            # build with llvm-mingw and deploy to media_soviet/workshop_wip/9784150
./build.sh nodeploy   # build only
```

Paths to the compiler and the game are at the top of `build.sh`.

## Settings

`src/railreverse.ini`, deployed beside the DLL:

| Key | Default | Meaning |
|---|---|---|
| `enabled` | 1 | 0 = load and do nothing |
| `push_speed_kmh` | 30 | speed limit while pushing, 0 = none |
| `skip_loco_both_ends` | 1 | leave trains with a locomotive at both ends as in the vanilla game |
| `log_flips` | 1 | one line per reversal in the RML log |

## How it works

See [docs/findings-1a.md](docs/findings-1a.md): the reversal function, the wagon placement formula
and the two hook sites.

## Licence

GPL-3.0, like TesmioLoader, whose plugin headers are in `include/`.

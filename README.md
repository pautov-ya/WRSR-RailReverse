# railreverse

A plugin for **Workers & Resources: Soviet Republic** 1.1.1.9. When a train reverses, the locomotive
stays where it is and pushes its wagons back, instead of jumping to the other end of the train.

Game logic is untouched: after the game's own reversal the train is marked as pushing and every car
is drawn on the mirror image of its place on the same stretch of track. Mass, power and grades work
exactly as when the train pulls. While pushing, the train is limited to `push_speed_kmh`
(25 km/h, the Soviet/Russian shunting limit for moving wagons first).

Loaded through [Republic Mod Loader](https://steamcommunity.com/sharedfiles/filedetails/?id=3787969749)
(Tesmio API 4).

## In game

With a train window open (any car of the train):

| Key | |
|---|---|
| `Ctrl+F9` | reversing on/off for this train |
| `Ctrl+Shift+F9` | reversing for every freight train on/off |
| `Ctrl+Alt+F9` | reversing for every passenger train on/off |

A train pushes if it was turned on itself or its class is. Turned off while pushing, it finishes the
trip and pulls from the next reversal on. The state is drawn above the train window and kept in
`railreverse.txt` beside the save. A pushing train entering a depot pulls again.

## Build

```sh
./build.sh            # build with llvm-mingw and deploy to media_soviet/workshop_wip/9784150
./build.sh nodeploy   # build only
```

Paths to the compiler and the game are at the top of `build.sh`.

## Settings

`src/railreverse.ini`, deployed beside the DLL: the keys, the starting `all_freight` /
`all_passenger` switches, `push_speed_kmh`, `skip_loco_both_ends`, the indicator position and
`log_flips`. Each key is documented there.

## How it works

- [docs/findings-1a.md](docs/findings-1a.md): the reversal function, the wagon placement formula and
  the two core hook sites.
- [docs/findings-0.3.md](docs/findings-0.3.md): saves, the train window, text drawing, depots.

## Known limitations

- For the game a pushing train still runs locomotive first: its logical head is where the first
  wagon is drawn. When a distribution office takes **all** the wagons of a pushing train, the
  locomotive appears where that first wagon stood and leaves from there. A standing train is not
  redrawn, so the plugin only learns of it at the office's reversal of the empty locomotive. A fix
  needs the office's wagon-removal code; an attempt is on branch `experiment/normalize-at-stops`.
- A train with a locomotive at both ends never pushes (`skip_loco_both_ends`), although its window
  may show reversing as on.
- The game always draws a locomotive with its back to its wagons. Where wagons are coupled at the
  cab end, the plugin draws the locomotive turned round (its "coupled back to front" flag, for the
  drawing only).

## Licence

GPL-3.0, like TesmioLoader, whose plugin headers are in `include/`.

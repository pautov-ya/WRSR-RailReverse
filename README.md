[English](README.md) | [Русский](README_RU.md)

# Rail Operations

**[Steam Workshop](https://steamcommunity.com/sharedfiles/filedetails/?id=3823129138)** · [Changes](CHANGELOG.md)

A plugin for **Workers & Resources: Soviet Republic** that makes trains work more like a real railway.

### Reversing

When a train reverses, its locomotive stays put and **pushes the wagons back**, as in real shunting:

- pushing speed is capped at 25 km/h (the Soviet shunting limit);
- reversing is switched on per train, for all freight trains, or for all passenger trains;
- trains with a locomotive at each end reverse as in the base game.

### Supply base wagon limit

Each locomotive can be given how many wagons it takes from a rail supply base: 1 to 20 (the maximum is `hub_limit_max` in `railreverse.ini`).

All settings are kept in the save and in a file next to it; the save itself is not changed.

## Requirements

- Workers & Resources: Soviet Republic **1.1.1.9**, 64-bit.
- [Republic Mod Loader](https://steamcommunity.com/sharedfiles/filedetails/?id=3787969749) (RML).
- No DLC. Compatible with [RailPhysics](https://steamcommunity.com/sharedfiles/filedetails/?id=3776784867).

## Installation with RML

1. Subscribe to [Republic Mod Loader](https://steamcommunity.com/sharedfiles/filedetails/?id=3787969749) and [Rail Operations](https://steamcommunity.com/sharedfiles/filedetails/?id=3823129138) on the Steam Workshop.
2. Start RML and enable **railreverse** under **Plugins**.
3. Start the game from RML.

To uninstall, unsubscribe from the mod.

## Installation with TesmioLoader

1. Subscribe to [TesmioLoader](https://steamcommunity.com/sharedfiles/filedetails/?id=3773169177) and [Rail Operations](https://steamcommunity.com/sharedfiles/filedetails/?id=3823129138).
2. Copy the `tesmioloader` folder from `<SteamLibrary>\steamapps\workshop\content\784150\3773169177\` into the game folder, next to `media_soviet`.
3. Copy all files from `<SteamLibrary>\steamapps\workshop\content\784150\3823129138\plugins\` into `tesmioloader\build\plugins\`.
4. Start Steam, then `tesmioloader\build\tesmiolauncher.exe`.

The mod has not been fully tested with TesmioLoader.

After a Steam update of the mod, copy the files from `plugins` again. To uninstall, delete them from `tesmioloader\build\plugins\`.

## Controls

The mod adds buttons to the locomotive window and to the route window.

### Locomotive window

Two buttons to the right of the signal button:

- **Reversing** switches reversing on and off for the selected train. It is green while the train reverses: switched on for the train or for all trains of its kind.
- **Wagons from a supply base** sets how many wagons the locomotive takes from a supply base: left click +1, right click −1. The number over the wagon shows the limit.

### Route window

Two buttons in the top row, for the waypoint or signal stop selected in the list:

- **Uncouple**: the train leaves its wagons at this stop.
- **Stop and turn round**: the train stops at this stop and turns round. With reversing on, the locomotive pushes the wagons back; with reversing off, the train turns round as in the base game.

With Uncouple you can build a route on which the locomotive runs round its train, as at a real station. After the uncouple stop, add route stops that lead the locomotive by a neighbouring track to the other end of the wagons: there it couples.

Marked stops are shown with icons in the route's list. A mark is for this route only; when the route passes a stop several times, it is for one visit.

### Options → Scripts

**Rail Operations: reverse all freight on/off** and **Rail Operations: reverse all passenger on/off** switch reversing on and off for all trains of that kind. Their state is kept in each save.

### Hotkeys

With a train window open:

| Keys | Action |
|---|---|
| `Ctrl+F9` | reverse for the selected train |
| `Ctrl -` / `Ctrl +` | supply base wagon limit −1 / +1 |
| `Ctrl Shift -` | no supply base wagon limit |

## Settings

Keys, speeds and the place of the train window buttons are set in `railreverse.ini`; each option is described there. `shunting = 0` turns running round and the route window buttons off. The file is in `<SteamLibrary>\steamapps\workshop\content\784150\3823129138\plugins\`. A Steam update of the mod overwrites it, so redo your changes after an update.

## How it works

For reversing, the game still turns the train round as usual and the mod changes only the drawing: each car of a pushing train is drawn at the mirror position on the same stretch of track, so the locomotive stays where it was. Running round and stop-and-turn work with the train's route, signals and coupling. Game code is found by byte signatures; if an update breaks one, that feature turns off and the RML log says so. Details are in [docs/](docs/).

## Known limitations

- When a supply base takes **all** the wagons of a pushing train, the locomotive jumps to where the first wagon was.
- Wagons left at an uncouple stop stand as a train without a locomotive. Saved like that and loaded with a version of the mod before 0.6.0 or with `shunting = 0`, they can slow the game down badly until the game removes them. Let the locomotives couple back before going back to an older version.
- Running round, the train window buttons and the Scripts menu switches work with game version 1.1.1.9 only. On another version they turn off; reversing keeps working.

## Building

```sh
./build.sh            # build and deploy to the game
./build.sh nodeploy   # build only
```

Requires [llvm-mingw](https://github.com/mstorsjo/llvm-mingw); paths are set in `build.sh`. The icons are drawn by `art/make_icons.py`.

## Credits

- [TesmioLoader](https://github.com/MaxLegend/TesmioLoader) by MaxLegend: plugin API headers.
- [Republic Mod Loader](https://github.com/Ultimate-Universe/WRSR-RepublicModLoader) by Ultimate-Universe.
- [RailPhysics](https://github.com/TheRealMeowMeow00/WRSR_RailPhysics): research on the game's rail code.
- `scripts/SOVIETInstructions.txt`: the game's basic script instructions, © 2017 Michal Kuchárik (Tau).

## License

[GPL-3.0](LICENSE).

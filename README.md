[English](README.md) | [Русский](README_RU.md)

# Rail Reverse

**[Steam Workshop](https://steamcommunity.com/sharedfiles/filedetails/?id=3823129138)**

A plugin for **Workers & Resources: Soviet Republic** that makes trains work more like a real railway.

### Reversing

In the base game, a reversing train's locomotive instantly jumps to the other end. With Rail Reverse it stays put and **pushes the wagons back**, as in real shunting:

- pushing speed is capped at 25 km/h (the Soviet shunting limit);
- a locomotive without wagons runs backwards instead of turning round;
- reversing is switched on per train, for all freight trains, or for all passenger trains;
- trains with a locomotive at each end work as in the base game.

### Supply base wagon limit

In the base game, a rail supply base fills a train with wagons up to the length of its track. Rail Reverse lets you give each locomotive its own limit: 1 to 20 wagons (the maximum is `hub_limit_max` in `railreverse.ini`).

All settings are kept in the save and in a file next to it; the save itself is not changed.

## Requirements

- Workers & Resources: Soviet Republic **1.1.1.9**, 64-bit.
- [Republic Mod Loader](https://steamcommunity.com/sharedfiles/filedetails/?id=3787969749) (RML).
- No DLC. Compatible with [RailPhysics](https://steamcommunity.com/sharedfiles/filedetails/?id=3776784867).

## Installation

1. Subscribe to [Republic Mod Loader](https://steamcommunity.com/sharedfiles/filedetails/?id=3787969749) and [Rail Reverse](https://steamcommunity.com/sharedfiles/filedetails/?id=3823129138) on the Steam Workshop.
2. Start RML and enable **railreverse** under **Plugins**.
3. Start the game from RML.

To uninstall, unsubscribe from the mod.

## Controls

The train window has two new buttons next to its signal button:

- **reversing for the selected train**: green while the train pushes its wagons (its own switch, or all trains of its kind switched on);
- **wagons from a supply base**: left click +1, right click −1. The limit is shown above the wagon. Past 20 and below 1 it is "no limit" again.

Reversing for all freight or all passenger trains is switched only in **Options → Scripts**: **Rail Reverse: all freight on/off** and **Rail Reverse: all passenger on/off**. Their state is kept in each save.

The hotkeys work with a train window open:

| Keys | Action |
|---|---|
| `Ctrl+F9` | reverse for the selected train |
| `Ctrl -` / `Ctrl +` | supply base wagon limit −1 / +1 |
| `Ctrl Shift -` | no supply base wagon limit |

## Settings

Keys, speeds and the place of the train window buttons are set in `railreverse.ini`; each option is described there. The file is in `<SteamLibrary>\steamapps\workshop\content\784150\3823129138\plugins\`. A Steam update of the mod overwrites it, so redo your changes after an update.

## How it works

The game still reverses the train as usual; only the drawing changes. Each car of a pushing train is drawn at the mirror position on the same stretch of track, so the locomotive stays where it was. Physics, signals and routes are untouched. Game code is found by byte signatures; if an update breaks one, that feature turns off and the RML log says so. Details are in [docs/](docs/).

## Known limitations

- When a supply base takes **all** the wagons of a pushing train, the locomotive jumps to where the first wagon was.

## Changes

- **0.5.0.** Reversing and wagon limit buttons in the train window, "all freight" and "all passenger" buttons in the Scripts menu. The Ctrl+Shift+F9 and Ctrl+Alt+F9 keys and the `all_freight`, `all_passenger` options are gone: these switches are in the Scripts menu only. The state lines in a corner of the screen are off (`indicator = 1` in the ini brings them back). Wagon limit 1 to 20; keys and clicks go round.
- **0.4.3.** `hub_limit_max` in `railreverse.ini`.

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

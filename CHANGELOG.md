[English](CHANGELOG.md) | [Русский](CHANGELOG_RU.md)

# Changes

## 0.6.0

- The mod is renamed Rail Operations (it was Rail Reverse). The file names (`railreverse.dll`, `railreverse.ini`) stay.
- Running round: an "Uncouple" button in the route window. The train leaves its wagons at the marked stop, the locomotive runs round them by a neighbouring track and couples at the other end.
- Stop and turn round: a button in the route window. The train brakes at the marked stop, turns round and sets off from standing.
- Marks are for their own route only and shown with icons in the route's list.
- Signals let a locomotive through to its wagons, chain signals too. Signals around wagons sold where they stood are worked out again at once.
- The `shunting` option in the ini. On a game version other than 1.1.1.9, running round, the buttons and the Scripts menu switches turn off.

## 0.5.0

- Reversing and wagon limit buttons in the train window.
- "All freight" and "all passenger" buttons in the Scripts menu. The Ctrl+Shift+F9 and Ctrl+Alt+F9 keys and the `all_freight`, `all_passenger` options are gone.
- The state lines in a corner of the screen are off; `indicator = 1` in the ini brings them back.
- Wagon limit 1 to 20; keys and clicks go round.

## 0.4.3

- `hub_limit_max` in `railreverse.ini`.

## 0.4.1

- State lines in English, README in English and Russian, published on the Steam Workshop.

## 0.4.0

- Supply base wagon limit.

## 0.3.0–0.3.2

- Trains chosen in game, state kept in saves, coupling without jumps, a crash log for diagnostics.

## 0.1.0

- The locomotive pushes the train back after a reversal.

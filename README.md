# fpcounter

A rough Geode debug prototype for GDR2 macro frame-window checking in Geometry Dash.

This intentionally skips polished UI, stats, and settings. It loads `acu.gdr2` from Geometry Dash's `replays` folder, replays macro inputs through hidden gameplay branches, tests nearby timing offsets, and prints/stores the raw detected window for each relevant input.

## Build

Make sure `GEODE_SDK` points to your Geode SDK, then run:

```powershell
geode build
```

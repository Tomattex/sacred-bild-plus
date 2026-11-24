# Changelog

## Unreleased

### SacredBild

- The menus use the same frame limit as the game (`[Display] FpsLimit`). `FpsLimitMenu` is removed; an old ini's
  value is ignored.
- The menus, the full-screen windows in game (options, save/load, map) and the loading screen are stretched to the
  full resolution instead of a centered 4:3 area with black bars. `[UI] ScaleMode=Full` with a fixed `Scale` keeps
  the in-game UI size for them.

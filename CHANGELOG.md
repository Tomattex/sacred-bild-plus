# Changelog

## Unreleased

### SacredBild

- The menus use the same frame limit as the game (`[Display] FpsLimit`). `FpsLimitMenu` is removed; an old ini's
  value is ignored.
- The menus, the full-screen windows in game (options, save/load, map) and the loading screen are stretched to the
  full resolution instead of a centered 4:3 area with black bars. `[UI] ScaleMode=Full` with a fixed `Scale` keeps
  the in-game UI size for them.
- `[Display] Mode` (settings window: Display mode) replaces `Borderless`: `Borderless` (fullscreen windowed, the
  default) covers the whole monitor and stretches a lower resolution to it, `Fullscreen` switches the display to the
  chosen resolution (exclusive), `Windowed` is a window with a title bar. The old `Borderless` key is ignored.
- The water in the main menu and the character selection moves at the same speed at any frame rate (it sped up
  with the frame limit).
- A screenshot (Print Screen, Ctrl+B) no longer makes the animations and effects jump: the game switched its clocks
  to its video capture mode for the captured frame.
- `[Render] SMAA` no longer washes out the whole picture: after its first frame in game everything, the menus
  included, was drawn too bright (an sRGB state it set stayed on).
- Text fields (savegame and character names, chat, network screens) edit like a normal text box: a movable caret
  (arrows, Home / End, click, Ctrl for words), selection (Shift, Ctrl+A, mouse drag), Delete, and Ctrl+C / X / V
  with the Windows clipboard.
- The menus show "SacredBild+" in gold under the game's version in the bottom right corner.
- The character name field has a palette on right click: colours and some of the game's sprites go into the name as
  the font's codes (`\c` colour, `\g` sprite). Text fields edit these codes as one character.

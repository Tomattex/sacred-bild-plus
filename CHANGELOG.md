# Changelog

## Unreleased

### SacredBild

- With `[Render] GpuSkinning` the sheen of buffs and spells on characters (stone skin, light shield and the other
  overlays) covers the whole model again, as in the original game, instead of showing in patches.

## 0.3.0

### SacredBild

- The multiplayer menu shows the lobby server for Closed and Open Internet and switches between servers (arrows, or a
  list to pick, add `host[:port]` and remove); the list is kept in `SacredBild-lobbies.txt`, first filled from
  `Settings.cfg` and a `servers.json` in the game folder, and the game saves the choice in `Settings.cfg`.
- In game, a row of icons left of the hero's portrait shows the effects on the hero: combat arts and spells (also a
  party member's or a monster's) and poison, each with the game's icon and the time it has left; the cursor over an
  icon shows its name. It moves aside when a window opens there; `[UI] EffectsBar=0` turns it off.
- The settings window before the game starts looks like the game's menus: dark stone, the page names in a column in
  the game's uncial (the shown one over the portal's red glow), the page in a steel frame, buttons as plaques with
  glowing orbs, under the Sacred Gold logo. Logo and typefaces come from the game's own files (`readme\Gold.jpg`,
  `font\CAROLING.TTF`, `font\ANTQS__.TTF`). Its settings are regrouped on five pages: Display, Graphics, Interface,
  Network, Advanced; the HUD layout is on the Interface page.
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
- The menus show "SacredBild+" under the game's version in the bottom right corner, in a gold gradient that runs
  across it.
- The character name field has a palette on right click: colours and some of the game's sprites go into the name as
  the font's codes (`\c` colour, `\g` sprite). A colour replaces the name's colour (or the selection's) instead of
  adding codes; the palette shows the length used and dims what no longer fits. Text fields edit these codes as one
  character.
- `[Display] SkipIntro` (on by default; settings window: Display): the Ascaron and publisher logo movies at the
  start are not played. The story movies still are.
- The game window no longer stays "not responding" for good when it starts behind another window (without the logo
  movies, whose skipping click brought it to the front, it mostly did): the game waited for the foreground without
  end and answered nothing meanwhile. It now tries for half a second and goes on; a click brings it to the front.

#pragma once
#include <windows.h>

struct IDirect3DDevice7;

// Editing in the game's text fields (cUI_EditControl: savegame and character names, the chat line, the network
// screens). The game only appends and takes the last character; this gives them a caret that moves (arrows,
// Home / End, Ctrl for words, a click), a selection (Shift, Ctrl+A, dragging the mouse), Delete, and Ctrl+C / X / V
// with the Windows clipboard. Password fields edit alike but don't copy. Read-only fields stay as they are. The font's
// codes (a colour, a sprite) are edited as one character each; a right click into the character name field opens a
// palette that puts colours and some of the UI's sprites into the name.
namespace TextEdit
{
    // Hooks the text field's event handling and drawing; call inside a Patch transaction.
    void install();
    // From the main window's procedure, before the game's: the keys the game never hands a text field (arrows,
    // Home, End, Delete) become characters for it while one has the focus, and the character name field's palette
    // takes its clicks and Esc. True: swallow the message.
    bool message(UINT& message, WPARAM& wParam);
    // After the UI manager drew, in its UI scope: the name field's palette, over the rest of the UI.
    void drawOverlay(IDirect3DDevice7* device);
}

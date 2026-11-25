#pragma once
#include <windows.h>

// Editing in the game's text fields (cUI_EditControl: savegame and character names, the chat line, the network
// screens). The game only appends and takes the last character; this gives them a caret that moves (arrows,
// Home / End, Ctrl for words, a click), a selection (Shift, Ctrl+A, dragging the mouse), Delete, and Ctrl+C / X / V
// with the Windows clipboard. Password fields edit alike but don't copy. Read-only fields stay as they are.
namespace TextEdit
{
    // Hooks the text field's event handling and drawing; call inside a Patch transaction.
    void install();
    // From the main window's procedure, before the game's: the keys the game never hands a text field (arrows,
    // Home, End, Delete) become characters for it while one has the focus. True: swallow the message.
    bool translateKey(UINT& message, WPARAM& wParam);
}

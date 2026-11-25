#pragma once

// The game plays its sound effects on 22 voices (sacred/sound.h) and drops a new effect when all of them are busy:
// with many monsters around, up to half of the hits, cries and spells of a second were never heard. When no voice is
// free, the one whose sound ends soonest takes the new effect instead; looping sounds keep theirs.
namespace SoundVoices
{
    // Queues the hook in the caller's Patch transaction.
    void install();
}

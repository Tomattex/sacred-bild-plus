#pragma once
#include "game/sacred_addr.h"

#include <windows.h>

#include <cstddef>
#include <cstdint>

// Sound: the sound system's lock and its voices.
namespace Sacred
{
    // A kernel mutex in a small wrapper: constructor ENG 0066F9C0 (CreateMutexA(nullptr, FALSE, nullptr)), close
    // 0066F9E0, lock 0066FA00 (WaitForSingleObject, INFINITE), unlock 0066FA10 (ReleaseMutex). The sound system
    // (one instance, made by ENG 006770E0) holds the only one at +0x60 and takes it in every call into its state,
    // among them the sound commands (ENG 0067A7A0) the object passes issue for each object, every frame.
    struct cMutex
    {
        HANDLE handle;
    };
    static_assert(sizeof(cMutex) == 4);

    // A voice of the sound system: a Miles sample handle for 2D sound and one for 3D (the sound mode uses one of
    // them), then what the game keeps about the sound on it.
    struct cMSSVoice
    {
        void* sample;       // HSAMPLE
        void* sample3D;     // H3DSAMPLE
        uint8_t rest[0x28];
    };
    static_assert(sizeof(cMSSVoice) == 0x30);

    // The sound system (Miles Sound System 6, one instance, made by ENG 006770E0). Sound effects play on voices
    // 0..voiceCount-1 (22, allocated anew for each sound mode by ENG 006928B0); 22..27 are a 2D pool for other sounds.
    struct cMSS
    {
        uint8_t pad0[0x60];
        cMutex lock;
        uint8_t pad64[0x1C];
        bool use3D;         // the voices' 3D handles are the ones in use
        uint8_t pad81[0x0F];
        cMSSVoice voices[28];
        uint32_t voiceCount;
    };
    static_assert(offsetof(cMSS, lock) == 0x60 && offsetof(cMSS, use3D) == 0x80 && offsetof(cMSS, voices) == 0x90 &&
                  offsetof(cMSS, voiceCount) == 0x5D0);
}

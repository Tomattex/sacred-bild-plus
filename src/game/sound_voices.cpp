#include "game/sound_voices.h"
#include "log.h"
#include "patch.h"
#include "sacred/sound.h"

#include <windows.h>
#include <cstdint>

namespace
{
    using namespace Sacred;

    // Miles queries the game doesn't import (mss32.dll 6.x, stdcall).
    constexpr int32_t kPlaying = 4;     // SMP_PLAYING
    using StatusFn = int32_t(__stdcall*)(void* sample);
    using MsPositionFn = void(__stdcall*)(void* sample, int32_t* totalMs, int32_t* currentMs);
    StatusFn g_status = nullptr;
    StatusFn g_loopCount = nullptr;     // loops left; 0 = endless
    MsPositionFn g_msPosition = nullptr;
    StatusFn g_status3D = nullptr;
    StatusFn g_loopCount3D = nullptr;
    StatusFn g_length3D = nullptr;      // bytes
    StatusFn g_offset3D = nullptr;      // bytes played
    StatusFn g_rate3D = nullptr;        // samples per second

    decltype(Addr::cMSS_findFreeVoice)::Ptr g_origFind = nullptr;

    // How long the voice's sound has left (2D: milliseconds; 3D: bytes over the sample rate, which ranks like time
    // for samples of one format); -1 if it can't give up its voice: not playing, or looping.
    int64_t remaining(const cMSS* mss, const cMSSVoice& voice)
    {
        if (mss->use3D)
        {
            void* s = voice.sample3D;
            if (!s || g_status3D(s) != kPlaying || g_loopCount3D(s) == 0)
            {
                return -1;
            }
            const int64_t left = static_cast<int64_t>(g_length3D(s)) - g_offset3D(s);
            const int32_t rate = g_rate3D(s);
            return left * 1000 / (rate > 0 ? rate : 22050);
        }
        void* s = voice.sample;
        if (!s || g_status(s) != kPlaying || g_loopCount(s) == 0)
        {
            return -1;
        }
        int32_t total = 0, current = 0;
        g_msPosition(s, &total, &current);
        return total - current;
    }

    // In playSFX, under the sound system's lock.
    uint32_t __fastcall hookFindFreeVoice(cMSS* self, void* edx, uint32_t extraPool)
    {
        const uint32_t found = g_origFind(self, edx, extraPool);
        const uint32_t count = self->voiceCount;
        if ((extraPool & 0xFF) || found < count || count > 22)
        {
            return found;
        }
        uint32_t taken = count;
        int64_t soonest = INT64_MAX;
        for (uint32_t i = 0; i < count; ++i)
        {
            const int64_t left = remaining(self, self->voices[i]);
            if (left >= 0 && left < soonest)
            {
                soonest = left;
                taken = i;
            }
        }
        if (taken == count)
        {
            return found;
        }
        Addr::cMSS_stopVoice(self, taken);
        return taken;
    }

    template <class Fn>
    bool bind(HMODULE mss, const char* name, Fn& out)
    {
        out = reinterpret_cast<Fn>(GetProcAddress(mss, name));
        return out != nullptr;
    }
}

void SoundVoices::install()
{
    if (!Addr::cMSS_findFreeVoice || !Addr::cMSS_stopVoice)
    {
        return;
    }
    HMODULE mss = GetModuleHandleW(L"mss32.dll");
    const bool bound = mss && bind(mss, "_AIL_sample_status@4", g_status) &&
        bind(mss, "_AIL_sample_loop_count@4", g_loopCount) && bind(mss, "_AIL_sample_ms_position@12", g_msPosition) &&
        bind(mss, "_AIL_3D_sample_status@4", g_status3D) && bind(mss, "_AIL_3D_sample_loop_count@4", g_loopCount3D) &&
        bind(mss, "_AIL_3D_sample_length@4", g_length3D) && bind(mss, "_AIL_3D_sample_offset@4", g_offset3D) &&
        bind(mss, "_AIL_3D_sample_playback_rate@4", g_rate3D);
    if (!bound)
    {
        LOG("Sound voices: Miles' sample queries were not found: a busy voice set still drops new sound effects");
        return;
    }
    if (Patch::hook(g_origFind, Addr::cMSS_findFreeVoice, &hookFindFreeVoice, "cMSS::findFreeVoice"))
    {
        LOG("Sound voices: a sound effect with all voices busy takes the one that ends soonest");
    }
}

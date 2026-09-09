#include "daisy_pod.h"
#include <cmath>

using namespace daisy;

DaisyPod pod;

static volatile float block_peak = 0.0f;

// ------------------------------------------------------------
// Audio Callback
// ------------------------------------------------------------

void AudioCallback(AudioHandle::InputBuffer in,
                   AudioHandle::OutputBuffer out,
                   size_t size)
{
    float peak = 0.0f;

    for(size_t i = 0; i < size; ++i)
    {
        // Linker Eingang
        const float x = in[0][i];

        const float abs_x = fabsf(x);

        if(abs_x > peak)
            peak = abs_x;

        // Audio einfach durchschleifen
        out[0][i] = x;
        out[1][i] = x;
    }

    if(peak > block_peak)
        block_peak = peak;
}

// ------------------------------------------------------------
// Main
// ------------------------------------------------------------

int main(void)
{
    pod.Init();

    pod.SetAudioBlockSize(48);

    pod.StartAudio(AudioCallback);

    uint32_t last_blink_time = 0;
    bool led1_state = false;

    while(1)
    {
        const uint32_t now = System::GetNow();

        // ----------------------------------------------------
        // LED 1: grünes Blinken
        // ----------------------------------------------------

        if(now - last_blink_time >= 500)
        {
            last_blink_time = now;
            led1_state = !led1_state;
        }

        if(led1_state)
            pod.led1.Set(0.0f, 1.0f, 0.0f);
        else
            pod.led1.Set(0.0f, 0.0f, 0.0f);

        // ----------------------------------------------------
        // LED 2: Level Meter
        // ----------------------------------------------------

        const float peak = block_peak;
        block_peak = 0.0f;

        if(peak >= 1.0f)
        {
            // >= 0 dBFS = Clipping
            pod.led2.Set(1.0f, 0.0f, 0.0f);
        }
        else if(peak > 0.001f)
        {
            // Signal vorhanden, aber < 0 dBFS
            pod.led2.Set(0.0f, 1.0f, 0.0f);
        }
        else
        {
            // praktisch kein Signal
            pod.led2.Set(0.0f, 0.0f, 0.0f);
        }

        pod.UpdateLeds();

        System::Delay(1);
    }
}
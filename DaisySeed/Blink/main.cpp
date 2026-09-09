#include "daisy_pod.h"

using namespace daisy;

DaisyPod pod;

int main(void)
{
    pod.Init();

    while(1)
    {
        pod.led1.Set(0.0f, 1.0f, 0.0f);
        pod.UpdateLeds();

        System::Delay(500);

        pod.led1.Set(0.0f, 0.0f, 0.0f);
        pod.UpdateLeds();

        System::Delay(500);
    }
}
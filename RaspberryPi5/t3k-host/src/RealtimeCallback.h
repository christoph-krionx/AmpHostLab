// RealtimeCallback: puts the audio device's thread on realtime scheduling.
// JUCE's ALSA thread starts with normal (SCHED_OTHER) scheduling, so any
// busy thread, the display included, can delay it. The first callback on a
// (re)started device raises its own thread to SCHED_FIFO, which the systemd
// unit's LimitRTPRIO (or /etc/security/limits.d for a login shell) allows.

#pragma once

#include <juce_audio_devices/juce_audio_devices.h>

#include <atomic>
#include <cerrno>
#include <pthread.h>
#include <sched.h>

namespace t3k {

class RealtimeCallback : public juce::AudioIODeviceCallback {
public:
  RealtimeCallback(juce::AudioIODeviceCallback& wrapped, int fifoPriority) : inner(wrapped), priority(fifoPriority) {}

  /** 0 = not tried yet, > 0 = running at this SCHED_FIFO priority,
      < 0 = refused (minus the error code). */
  int result() const { return outcome.load(); }

  void audioDeviceIOCallbackWithContext(const float* const* inputs, int numInputs, float* const* outputs,
                                        int numOutputs, int numSamples,
                                        const juce::AudioIODeviceCallbackContext& context) override {
    if (!tried) {
      tried = true;
      sched_param param{};
      param.sched_priority = priority;
      const int error = pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);
      outcome = error == 0 ? priority : -error;
    }
    inner.audioDeviceIOCallbackWithContext(inputs, numInputs, outputs, numOutputs, numSamples, context);
  }

  void audioDeviceAboutToStart(juce::AudioIODevice* device) override {
    tried = false;  // a restarted device runs on a new thread
    inner.audioDeviceAboutToStart(device);
  }
  void audioDeviceStopped() override { inner.audioDeviceStopped(); }
  void audioDeviceError(const juce::String& message) override { inner.audioDeviceError(message); }

private:
  juce::AudioIODeviceCallback& inner;
  const int priority;
  bool tried{false};  // audio thread only
  std::atomic<int> outcome{0};
};

}  // namespace t3k

// RealtimeCallback: puts the audio device's thread on realtime scheduling.
// JUCE's ALSA thread starts with normal (SCHED_OTHER) scheduling, so any
// busy thread, the display included, can delay it. The first callback on a
// (re)started device raises its own thread to SCHED_FIFO, which the systemd
// unit's LimitRTPRIO (or /etc/security/limits.d for a login shell) allows.
//
// It also silences the output while `muted` says so (the tuner), with a
// short fade either way so the switch does not click.

#pragma once

#include <juce_audio_devices/juce_audio_devices.h>

#include <atomic>
#include <cerrno>
#include <functional>
#include <pthread.h>
#include <sched.h>

namespace t3k {

class RealtimeCallback : public juce::AudioIODeviceCallback {
public:
  RealtimeCallback(juce::AudioIODeviceCallback& wrapped, int fifoPriority) : inner(wrapped), priority(fifoPriority) {}

  /** 0 = not tried yet, > 0 = running at this SCHED_FIFO priority,
      < 0 = refused (minus the error code). */
  int result() const { return outcome.load(); }

  /** Asked on the audio thread once per block: true = silence the output.
      Set before the device starts; must not block or allocate. */
  std::function<bool()> muted;

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

    const float target = muted && muted() ? 0.0f : 1.0f;
    if (gain == target && target == 1.0f)
      return;
    for (int i = 0; i < numSamples; ++i) {
      gain = target > gain ? juce::jmin(target, gain + kFadeStep) : juce::jmax(target, gain - kFadeStep);
      for (int c = 0; c < numOutputs; ++c)
        if (outputs[c] != nullptr)
          outputs[c][i] *= gain;
    }
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
  // About 5 ms from full to silent at 48 kHz.
  static constexpr float kFadeStep = 1.0f / 240.0f;
  bool tried{false};  // audio thread only
  float gain{1.0f};   // audio thread only
  std::atomic<int> outcome{0};
};

}  // namespace t3k

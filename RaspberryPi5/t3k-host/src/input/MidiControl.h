// MidiControl: MIDI from the audio interface's MIDI input (a foot
// controller, for example). Everything reaches the processor, whose
// MidiMapper loads presets on program changes (n = the (n+1)th preset in
// list order) and drives its CC mappings: parameters, block powers, preset
// steps. The host answers one CC itself: the tuner, which is no parameter.

#pragma once

#include <juce_audio_devices/juce_audio_devices.h>

#include <atomic>
#include <functional>

namespace t3k {

class MidiControl : public juce::MidiInputCallback {
public:
  /** `processorInput` gets every message (the audio processor player);
      `onTuner` runs on the message thread. */
  MidiControl(juce::AudioDeviceManager& deviceManager, juce::MidiInputCallback& processorInput,
              std::function<void()> onTuner)
      : dm(deviceManager), processor(processorInput), tuner(std::move(onTuner)) {}

  ~MidiControl() override {
    if (deviceId.isNotEmpty()) {
      dm.removeMidiInputDeviceCallback(deviceId, &processor);
      dm.removeMidiInputDeviceCallback(deviceId, this);
      dm.setMidiInputDeviceEnabled(deviceId, false);
    }
  }

  /** Opens the first MIDI input whose name contains `match`. `channel`:
      0 = all, 1-16 = only that one; `tunerCc` < 0 = no tuner control.
      Returns the device name, empty when there is none. */
  juce::String start(const juce::String& match, int channel, int tunerCc) {
    channelFilter = channel;
    tunerController = tunerCc;
    for (const auto& device : juce::MidiInput::getAvailableDevices()) {
      if (!device.name.containsIgnoreCase(match))
        continue;
      deviceId = device.identifier;
      dm.setMidiInputDeviceEnabled(deviceId, true);
      dm.addMidiInputDeviceCallback(deviceId, &processor);
      dm.addMidiInputDeviceCallback(deviceId, this);
      return device.name;
    }
    return {};
  }

  // MIDI thread.
  void handleIncomingMidiMessage(juce::MidiInput*, const juce::MidiMessage& message) override {
    if (tunerController < 0 || !message.isController() || message.getControllerNumber() != tunerController)
      return;
    if (channelFilter > 0 && message.getChannel() != channelFilter)
      return;
    // One toggle per press, as the plugin's own mappings count them: a
    // value of 64 or more fires, a lower one too unless it is the release
    // of a momentary switch that just sent a high value.
    const int value = message.getControllerValue();
    const int previous = lastValue.exchange(value);
    if (value < 64 && previous >= 64)
      return;
    juce::MessageManager::callAsync(tuner);
  }

private:
  juce::AudioDeviceManager& dm;
  juce::MidiInputCallback& processor;
  std::function<void()> tuner;
  juce::String deviceId;
  int channelFilter{0};
  int tunerController{-1};
  std::atomic<int> lastValue{-1};
};

}  // namespace t3k

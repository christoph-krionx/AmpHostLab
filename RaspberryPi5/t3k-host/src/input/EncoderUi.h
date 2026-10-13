// EncoderUi: one-encoder operation of the device.
// Top level: turning moves the cursor over the items, a press enters the
// selected item. Inside an item: turning changes its value, a press leaves.
// On a block of the chain a press switches that block on or off.
//
// The encoder is read on its own thread; everything that touches the
// processor runs on the message thread.

#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include <vector>

#include "EncoderReader.h"
#include "UiState.h"

class TONE3000Processor;

namespace t3k {

class DisplayUi;

class EncoderUi : private juce::Thread, private juce::Timer {
public:
  EncoderUi(TONE3000Processor& processor, UiState& state, DisplayUi* display);
  ~EncoderUi() override;

  /** The preset that is loaded right now, so stepping starts from it. */
  void setCurrentPreset(const juce::String& name);

  /** Returns an error message, empty on success. */
  juce::String start(const EncoderPins& pins);

private:
  struct Preset {
    juce::String id;
    juce::String name;
  };

  void run() override;
  void timerCallback() override;
  void handle(EncoderEvent event, int step);
  void adjust(UiItem item, int delta, int step);
  void toggleBlock(int toneIndex);
  void stepPreset(int delta);
  void loadPendingPreset();
  float get(const char* paramId) const;
  void set(const char* paramId, float value);

  TONE3000Processor& processor;
  UiState& state;
  DisplayUi* display;
  EncoderReader reader;

  std::vector<Preset> presets;
  int presetIndex{-1};

  JUCE_DECLARE_WEAK_REFERENCEABLE(EncoderUi)
  // Made on the message thread; the encoder thread only copies it.
  juce::WeakReference<EncoderUi> weakSelf;
};

}  // namespace t3k

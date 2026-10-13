// EncoderUi: one-encoder operation of the device.
// Top level: turning moves the cursor over the items, a press enters the
// selected item. Inside an item: turning changes its value, a press leaves.
//
// The encoder is read on its own thread; everything that touches the
// processor runs on the message thread.

#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include <string>

#include "EncoderReader.h"
#include "UiState.h"

class TONE3000Processor;

namespace t3k {

class DisplayUi;

class EncoderUi : private juce::Thread, private juce::Timer {
public:
  EncoderUi(TONE3000Processor& processor, UiState& state, DisplayUi* display);
  ~EncoderUi() override;

  /** Folder whose .nam files the model item steps through. */
  void setTonesDir(const juce::File& dir);
  /** The tone that is loaded right now and the chain block it sits in. */
  void setCurrentTone(const juce::File& file, const std::string& blockId);

  /** Returns an error message, empty on success. */
  juce::String start(const EncoderPins& pins);

private:
  void run() override;
  void timerCallback() override;
  void handle(EncoderEvent event, int step);
  void adjust(UiItem item, int delta, int step);
  void stepModel(int delta);
  void loadPendingModel();
  float get(const char* paramId) const;
  void set(const char* paramId, float value);

  TONE3000Processor& processor;
  UiState& state;
  DisplayUi* display;
  EncoderReader reader;

  juce::Array<juce::File> tones;
  int toneIndex{-1};
  std::string toneBlockId;

  JUCE_DECLARE_WEAK_REFERENCEABLE(EncoderUi)
  // Made on the message thread; the encoder thread only copies it.
  juce::WeakReference<EncoderUi> weakSelf;
};

}  // namespace t3k

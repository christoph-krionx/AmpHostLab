// EncoderInput: reads the front-panel encoders on their own thread and
// hands the panel input events on the message thread. Sorts presses into
// short and long (a long press fires while the button is still held) and
// speeds up turns that come in quick succession.

#pragma once

#include <juce_events/juce_events.h>

#include <functional>
#include <vector>

#include "EncoderReader.h"
#include "InputEvent.h"

namespace t3k {

class EncoderInput : private juce::Thread {
public:
  /** `sink` runs on the message thread. */
  explicit EncoderInput(std::function<void(const InputEvent&)> sink);
  ~EncoderInput() override;

  /** The navigation encoder on these pins. Returns an error message, empty
      on success. */
  juce::String start(const EncoderPins& nav);

private:
  void run() override;
  void post(const InputEvent& event);

  std::function<void(const InputEvent&)> sink;
  EncoderReader reader;
  std::vector<InputEvent::Source> sources;  // per reader encoder
  std::vector<int> indices;

  JUCE_DECLARE_WEAK_REFERENCEABLE(EncoderInput)
  // Made on the message thread; the reader thread only copies it.
  juce::WeakReference<EncoderInput> weakSelf;
};

}  // namespace t3k

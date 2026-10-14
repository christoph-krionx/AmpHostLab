// DisplayUi: draws the device screen on the SPI display from its own
// low-priority thread. Reads the processor through its parameters and meter
// poll only; never touches the audio thread.

#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include <atomic>
#include <map>
#include <vector>

#include "SpiLcd.h"
#include "UiState.h"

class TONE3000Processor;

namespace t3k {

class DisplayUi : private juce::Thread {
public:
  DisplayUi(TONE3000Processor& processor, juce::AudioDeviceManager& deviceManager, const UiState& state);
  ~DisplayUi() override;

  /** Opens the display and shows the splash screen. Returns an error
      message, empty on success. */
  juce::String start(const LcdConfig& config);

  /** Text for the top line of the main screen (tone or preset name). */
  void setTitle(const juce::String& text);

  /** Line under the name on the splash screen, e.g. what the host waits for. */
  void setStatus(const juce::String& text);

  /** Leave the splash screen (it stays up for a minimum time regardless). */
  void showMain() { mainRequested = true; }

  /** Draws the main screen into an image instead of onto the panel, for
      previews. Does not need start(). */
  juce::Image renderPreview(int width, int height);

private:
  void run() override;
  void drawSplash(juce::Graphics& g) const;
  void drawMain(juce::Graphics& g);
  void present(const juce::Image& frame);
  const juce::Image& glyphFor(const juce::String& gear) const;

  TONE3000Processor& processor;
  juce::AudioDeviceManager& deviceManager;
  const UiState& state;
  SpiLcd lcd;
  std::vector<uint16_t> shown;    // what the panel currently displays
  std::vector<uint16_t> pending;  // the frame being sent
  bool firstFrame{true};
  int frameW{0};  // size of the picture being drawn
  int frameH{0};
  // Gear pictograms as alpha masks, rendered once on the message thread.
  std::map<juce::String, juce::Image> glyphs;
  juce::uint32 clipUntilMs[2]{0, 0};  // input, output: clip light stays on until then
  std::atomic<bool> mainRequested{false};
  juce::CriticalSection titleLock;  // guards title and status
  juce::String title;
  juce::String status{"starting"};
};

}  // namespace t3k

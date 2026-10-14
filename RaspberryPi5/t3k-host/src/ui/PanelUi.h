// PanelUi: what the controls do. Takes input events on the message thread
// and turns them into cursor moves, page changes and processor calls.
//
// Navigation encoder: turning moves the cursor over the page's targets, or
// changes the value being edited. A short press switches (a block, an
// effect, a switch entry) or starts and ends editing; a long press opens
// the page of an effect switch or a block, goes back from a page, or resets
// the knob being edited to its default.
// Value encoders: each works the knob slot above it; a short press toggles
// fine steps, a long press resets the knob.
//
// The block page can also move its block: back on the grid the block then
// follows the turns through its row until a press puts it down.

#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include <functional>
#include <vector>

#include "UiState.h"
#include "input/InputEvent.h"

class TONE3000Processor;

namespace t3k {

class DisplayUi;

class PanelUi : private juce::Timer {
public:
  PanelUi(TONE3000Processor& processor, UiState& state, DisplayUi* display);
  ~PanelUi() override;

  /** The preset that is loaded right now, so stepping starts from it. */
  void setCurrentPreset(const juce::String& name);

  /** Message thread only. */
  void handle(const InputEvent& event);

  /** Called on the message thread with the name of each preset loaded. */
  std::function<void(const juce::String&)> onPresetLoaded;

private:
  struct Preset {
    juce::String id;
    juce::String name;
  };

  std::vector<Target> targets() const;
  void handleNav(const InputEvent& event);
  void press(const Target& target);
  void pressBlockEntry(EntryKind kind);
  void longPress(const Target& target);
  void turnEditing(const Target& target, int delta, int step);
  void endEditing(const Target& target);
  void openPage(Page page);
  void openBlockPage(int toneIndex);
  void closePage();

  juce::RangedAudioParameter* param(const char* id) const;
  void toggle(const char* paramId);
  void stepChoice(const char* paramId, int delta);
  void adjustKnob(int slot, int delta, int step, bool fine);
  void resetKnob(int slot);
  void setBlockKnob(const KnobDef& knob, float value);
  bool isChoice(const char* paramId) const;

  void toggleBlock(int toneIndex);
  void setBlockSwitch(const char* name, bool on);
  void stepModel(int delta);
  void applyModel(bool finished);
  void startMove();
  void moveBlock(int delta);
  void endMove();
  void removeBlock();

  void stepPreset(int delta);
  void loadPendingPreset();
  void savePreset();
  void timerCallback() override;

  TONE3000Processor& processor;
  UiState& state;
  DisplayUi* display;

  std::vector<Preset> presets;
  int presetIndex{-1};
  int returnSelected{0};  // overview cursor to come back to from a page
  int modelRequest{0};    // counts model picks; only the latest one loads
  int appliedModel{-1};   // model id last sent while picking

  JUCE_DECLARE_WEAK_REFERENCEABLE(PanelUi)
};

}  // namespace t3k

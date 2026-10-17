// PanelUi: what the controls do. Takes input events on the message thread
// and turns them into cursor moves, page changes and processor calls.
//
// Navigation encoder: turning moves the cursor over the page's targets, or
// changes the value being edited. A short press switches (a block, an
// effect, a switch entry) or starts and ends editing; a long press opens
// the page of an effect switch or a block, goes back from a page, or resets
// the knob being edited to its default.
// Value encoders: each works the knob slot above it; a short press toggles
// fine steps. A long press puts what the cursor is on onto the encoder for
// the zen mode (the idle view): there each encoder works its own control,
// the same for every preset, and the screen shows it alone while it turns
// (see ZenControls). In the zen mode a press switches a switch (a knob
// ignores it); the preset steps through the presets, with the idle view
// showing them; the tuner shows full screen.
//
// The block page can also move its block: back on the grid the block then
// follows the turns through its row until a press puts it down.
//
// Second encoder: a value encoder (role "knob1".."knob5"); the preset's
// volume, like an amp's master knob (it acts at once, even out of the idle
// view); or, as role "models", steps through the models of the block under
// the cursor (or of the block page's block).
// A long press on the preset name discards unsaved changes; the settings
// page can power the device off.
//
// Presets dropped into the preset folder (or reordered there) show up in
// the list without a restart; a replaced file of the preset that is playing
// is loaded again, unless the sound has unsaved changes on the device.

#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include <functional>
#include <vector>

#include "UiState.h"
#include "input/InputEvent.h"
#include "ui/PresetFolderWatcher.h"
#include "ui/ZenControls.h"

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

  /** Switches the tuner on or off from outside the panel (a MIDI control). */
  void toggleTuner();

  /** What the second encoder does: the volume (default) or the models. */
  void setSecondEncoderStepsModels(bool models) { secondEncoderModels = models; }

  /** The zen-mode control of a value encoder, as the settings file keeps
      it (see ZenControls); unknown text keeps the default. */
  void setZenControl(int slot, const juce::String& spec);

  /** Another preset became active without the panel (MIDI): follow it. */
  void presetChangedElsewhere(const juce::String& presetId);

  /** Called on the message thread with the name of each preset loaded. */
  std::function<void(const juce::String&)> onPresetLoaded;

  /** Called on the message thread when a machine setting changed on the
      settings page, with its settings-file key and value. */
  std::function<void(const juce::String& key, const juce::String& value)> onSettingChanged;

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
  void adjustParamKnob(const KnobDef& knob, float move);
  void resetKnob(int slot);
  void setBlockKnob(const std::string& blockId, const KnobDef& knob, float value);
  void turnZen(int slot, int delta, int step);
  void pressZen(int slot);
  void setZenTuner(bool on);
  void assignZen(int slot);
  bool isChoice(const char* paramId) const;

  void toggleBlock(int toneIndex);
  void setBlockSwitch(const char* name, bool on);
  void stepInputMode(int delta);
  void turnModelEncoder(int delta);
  void turnVolume(int delta, int step);
  void discardChanges();
  void powerOff();
  void stepModel(int delta);
  void applyModel(bool finished);
  void startMove();
  void moveBlock(int delta);
  void endMove();
  void removeBlock();

  void readPresets();
  void presetFolderChanged(const juce::StringArray& changedIds);
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
  bool secondEncoderModels{false};
  ZenControl zen[kKnobSlots]{defaultZenControl(0), defaultZenControl(1), defaultZenControl(2), defaultZenControl(3),
                             defaultZenControl(4)};
  PresetFolderWatcher presetFolder{[this](const juce::StringArray& ids) { presetFolderChanged(ids); }};

  JUCE_DECLARE_WEAK_REFERENCEABLE(PanelUi)
};

}  // namespace t3k

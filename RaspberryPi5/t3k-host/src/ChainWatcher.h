// ChainWatcher: keeps UiState in step with the processor. It mirrors the
// chain, runs the tuner while the panel has it switched on, and notices
// when the sound no longer matches the preset it was loaded from.
// Polls on the message thread; the processor answers with a cheap
// "unchanged" while the chain did not change.

#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "Processor.h"
#include "UiState.h"

namespace t3k {

class ChainWatcher : private juce::Timer {
public:
  ChainWatcher(TONE3000Processor& p, UiState& s) : processor(p), state(s) {
    timerCallback();
    startTimer(40);
  }

private:
  // The plugin's gear id; without one, the file format decides.
  static juce::String gearOf(const juce::var& tone) {
    const auto gear = tone["gear"].toString().toLowerCase();
    if (gear.isNotEmpty())
      return gear;
    return tone["format"].toString().equalsIgnoreCase("ir") ? "ir" : "file";
  }

  static std::vector<ChainCell> readRow(const juce::var& row) {
    std::vector<ChainCell> cells;
    if (const auto* items = row.getArray()) {
      for (const auto& item : *items) {
        ChainCell cell;
        cell.id = item["blockId"].toString().toStdString();
        cell.insert = item["kind"].toString() == "insert";
        if (!cell.insert) {
          cell.name = item["tone"]["title"].toString();
          cell.gear = gearOf(item["tone"]);
          const auto params = item["params"];
          cell.enabled = static_cast<bool>(params["enabled"]);
          cell.normalize = static_cast<bool>(params["normalize"]);
          cell.inputGain = static_cast<float>(static_cast<double>(params["inputGain"]));
          cell.outputGain = static_cast<float>(static_cast<double>(params["outputGain"]));
          cell.mix = static_cast<float>(static_cast<double>(params["mix"]));
          cell.loading = static_cast<bool>(item["modelLoading"]) || !static_cast<bool>(item["loaded"]);
          cell.activeModel = static_cast<int>(item["activeModelId"]);
          if (const auto* models = item["tone"]["models"].getArray())
            for (const auto& m : *models)
              cell.models.push_back({static_cast<int>(m["id"]), m["name"].toString(), m["model_url"].toString()});
        }
        cells.push_back(std::move(cell));
      }
    }
    return cells;
  }

  void timerCallback() override {
    const bool tunerOn = state.tunerOn.load();
    processor.setTunerEnabled(tunerOn);
    state.tunerHz = tunerOn ? static_cast<float>(static_cast<double>(processor.getTunerReading()["frequency"])) : 0.0f;

    readChain();

    // Unsaved changes: compare what makes up the sound with how it was
    // when the preset was loaded or last saved.
    const auto now = soundPrint();
    if (state.presetSaved.exchange(false) || presetId != baselinePresetId) {
      baseline = now;
      baselinePresetId = presetId;
    }
    state.presetDirty = now != baseline;
  }

  void readChain() {
    const auto chain = processor.getChainState(revision);
    if (static_cast<bool>(chain["unchanged"]))
      return;
    revision = static_cast<int>(chain["revision"]);
    presetId = chain["preset"]["id"].toString();

    ChainView view;
    view.stereo = static_cast<bool>(chain["stereoEnabled"]);
    view.rows[0] = readRow(chain["chain"]);
    if (view.stereo)
      view.rows[1] = readRow(chain["chainRight"]);

    // Branched: the branch row is fed from the trunk row after one of its
    // blocks, so it starts in the column behind that block.
    const auto branch = chain["branch"];
    if (view.stereo && branch.isObject()) {
      const int trunk = branch["side"].toString().equalsIgnoreCase("right") ? 1 : 0;
      const auto tapId = branch["afterBlockId"].toString().toStdString();
      const auto& trunkRow = view.rows[trunk];
      for (size_t i = 0; i < trunkRow.size(); ++i)
        if (trunkRow[i].id == tapId)
          view.offset[1 - trunk] = static_cast<int>(i) + 1;
    }

    chainPrint.clear();
    for (const auto& row : view.rows)
      for (const auto& cell : row)
        if (!cell.insert)
          chainPrint << cell.name << (cell.enabled ? "+" : "-") << (cell.normalize ? "n" : "")
                     << juce::String(cell.inputGain, 4) << "/" << juce::String(cell.outputGain, 4) << "/"
                     << juce::String(cell.mix, 4) << "#" << cell.activeModel;

    // A block page (or a move) whose block is gone ends on the overview.
    // There the blocks sit in the middle of the cursor order: when their
    // number changes, keep the cursor on the knob it was on, and move it
    // off a block that is gone.
    if ((state.currentPage() == Page::Block || state.moving.load()) && view.find(state.getPageBlock()) == nullptr) {
      state.page = static_cast<int>(Page::Overview);
      state.moving = false;
      state.editing = false;
      state.selected = 0;
    } else if (state.currentPage() == Page::Overview) {
      const int firstBlock = kHeadItems + kSwitches;
      const int oldBlocks = static_cast<int>(state.getChain().tones().size());
      const int newBlocks = static_cast<int>(view.tones().size());
      const int position = state.selected.load();
      if (position >= firstBlock + oldBlocks) {
        state.selected = position + newBlocks - oldBlocks;
      } else if (position >= firstBlock + newBlocks) {
        state.selected = 0;
        state.editing = false;
      }
    }
    state.setChain(std::move(view));
  }

  // Everything a preset stores that the panel can change: the blocks'
  // on/off state and the parameters, minus those that describe the
  // machine rather than the sound.
  juce::String soundPrint() const {
    juce::String print = chainPrint;
    for (auto* p : static_cast<juce::AudioProcessor&>(processor).getParameters())
      if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(p))
        if (!TONE3000Processor::isMachineDefaultParameter(ranged->getParameterID()))
          print << "|" << juce::String(ranged->getValue(), 4);
    return print;
  }

  TONE3000Processor& processor;
  UiState& state;
  int revision{-1};
  juce::String presetId;
  juce::String chainPrint;
  juce::String baseline;
  juce::String baselinePresetId;
};

}  // namespace t3k

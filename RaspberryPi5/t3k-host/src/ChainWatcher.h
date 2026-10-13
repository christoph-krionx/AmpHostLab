// ChainWatcher: keeps UiState's picture of the chain in step with the
// processor. Polls on the message thread; the processor answers with a
// cheap "unchanged" while nothing happened.

#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "Processor.h"
#include "UiState.h"

namespace t3k {

class ChainWatcher : private juce::Timer {
public:
  ChainWatcher(TONE3000Processor& p, UiState& s) : processor(p), state(s) {
    timerCallback();
    startTimer(150);
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
          cell.enabled = static_cast<bool>(item["params"]["enabled"]);
          cell.loading = static_cast<bool>(item["modelLoading"]) || !static_cast<bool>(item["loaded"]);
        }
        cells.push_back(std::move(cell));
      }
    }
    return cells;
  }

  void timerCallback() override {
    const auto chain = processor.getChainState(revision);
    if (static_cast<bool>(chain["unchanged"]))
      return;
    revision = static_cast<int>(chain["revision"]);

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

    // The blocks sit in the middle of the cursor order: when their number
    // changes, keep the cursor on the knob or switch it was on, and move
    // it off a block that is gone.
    const int oldBlocks = static_cast<int>(state.getChain().tones().size());
    const int newBlocks = static_cast<int>(view.tones().size());
    const int position = state.selected.load();
    if (position > oldBlocks) {
      state.selected = position + newBlocks - oldBlocks;
    } else if (position > newBlocks) {
      state.selected = 0;
      state.editing = false;
    }
    state.setChain(std::move(view));
  }

  TONE3000Processor& processor;
  UiState& state;
  int revision{-1};
};

}  // namespace t3k

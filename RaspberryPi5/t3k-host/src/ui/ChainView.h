// ChainView: the chain as the panel sees it, read from the processor's
// chain state by the ChainWatcher.

#pragma once

#include <juce_core/juce_core.h>

#include <string>
#include <vector>

namespace t3k {

// One of a tone's models (a folder of captures loads as one tone with
// several models).
struct ToneModel {
  int id{0};
  juce::String name;
  juce::String url;  // local tones only; a catalog tone stores just its active model
};

// One slot of the chain grid.
struct ChainCell {
  std::string id;
  juce::String name;
  juce::String gear;    // the plugin's gear id (amp, cab, pedal, ...), picks the pictogram
  bool insert{false};   // empty slot
  bool enabled{true};
  bool loading{false};
  bool normalize{true};
  // The block's own knobs, normalized 0..1 like the plugin stores them.
  float inputGain{0.5f};
  float outputGain{0.5f};
  float mix{1.0f};
  int activeModel{0};
  std::vector<ToneModel> models;
};

// The chain as the plugin's grid shows it: one row in mono, two in stereo
// (left and right chain), each at least five slots wide.
struct ChainView {
  std::vector<ChainCell> rows[2];
  // Column each row starts in. A branched chain's branch row starts one
  // column after the trunk block it taps, like in the plugin.
  int offset[2]{0, 0};
  bool stereo{false};

  /** Tone blocks in cursor order: first row left to right, then the second. */
  std::vector<const ChainCell*> tones() const {
    std::vector<const ChainCell*> out;
    for (const auto& row : rows)
      for (const auto& cell : row)
        if (!cell.insert)
          out.push_back(&cell);
    return out;
  }

  const ChainCell* find(const std::string& blockId) const {
    for (const auto& row : rows)
      for (const auto& cell : row)
        if (cell.id == blockId)
          return &cell;
    return nullptr;
  }

  ChainCell* find(const std::string& blockId) {
    return const_cast<ChainCell*>(static_cast<const ChainView&>(*this).find(blockId));
  }

  /** Position of a block among the tones, -1 if it is not there. */
  int toneIndex(const std::string& blockId) const {
    const auto list = tones();
    for (size_t i = 0; i < list.size(); ++i)
      if (list[i]->id == blockId)
        return static_cast<int>(i);
    return -1;
  }
};

}  // namespace t3k

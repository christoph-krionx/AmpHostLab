// What the front panel is pointing at and what the chain looks like;
// written on the message thread, read by the display thread.

#pragma once

#include <juce_core/juce_core.h>

#include <atomic>
#include <string>
#include <vector>

namespace t3k {

// The fixed items of the panel.
enum class UiItem { Preset, Save, Tuner, Settings, Gain, Bass, Mid, Treble, Volume, Gate, Pitch, Spread };
constexpr int kUiItemCount = 12;
// The items of the top line; the chain's blocks come after them in the
// cursor order, the rest after the blocks.
constexpr int kUiHeadItems = 4;

// What a cursor position points at. The cursor runs in screen order: the
// top line (preset, save, tuner, settings), the chain's blocks, then the knobs
// and the effect switches.
struct CursorTarget {
  bool isBlock;
  int block;    // index among the chain's tone blocks, when isBlock
  UiItem item;  // otherwise
};

inline CursorTarget cursorTarget(int position, int blockCount) {
  if (position < kUiHeadItems)
    return {false, 0, static_cast<UiItem>(juce::jmax(0, position))};
  if (position < kUiHeadItems + blockCount)
    return {true, position - kUiHeadItems, UiItem::Preset};
  return {false, 0, static_cast<UiItem>(position - blockCount)};
}

// The gate's threshold range on the panel; one step below the minimum is "off".
constexpr float kGateMinDb = -90.0f;

// One slot of the chain grid.
struct ChainCell {
  std::string id;
  juce::String name;
  juce::String gear;    // the plugin's gear id (amp, cab, pedal, ...), picks the pictogram
  bool insert{false};   // empty slot
  bool enabled{true};
  bool loading{false};
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
};

struct UiState {
  std::atomic<bool> active{false};  // false = no encoder, draw no cursor
  std::atomic<int> selected{0};
  std::atomic<bool> editing{false};
  // Position in the preset list, for the display's context line.
  std::atomic<int> presetIndex{-1};
  std::atomic<int> presetCount{0};
  // True while the sound differs from the preset as loaded or last saved.
  std::atomic<bool> presetDirty{false};
  // Set by the panel after saving; the watcher takes it as the new reference.
  std::atomic<bool> presetSaved{false};
  // Tuner: switched by the panel, measured on the message thread.
  std::atomic<bool> tunerOn{false};
  std::atomic<float> tunerHz{0.0f};  // 0 = no reliable pitch

  ChainView getChain() const {
    const juce::ScopedLock lock(chainLock);
    return chain;
  }
  void setChain(ChainView view) {
    const juce::ScopedLock lock(chainLock);
    chain = std::move(view);
  }

private:
  juce::CriticalSection chainLock;
  ChainView chain;
};

}  // namespace t3k

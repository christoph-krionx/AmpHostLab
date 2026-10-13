// What the front panel is pointing at; written by the encoder controller,
// read by the display thread.

#pragma once

#include <atomic>

namespace t3k {

// Order = the order the encoder steps through on the top level.
enum class UiItem { Model, Gain, Bass, Mid, Treble, Volume, Gate, Pitch, Spread };
constexpr int kUiItemCount = 9;

// The gate's threshold range on the panel; one step below the minimum is "off".
constexpr float kGateMinDb = -90.0f;

struct UiState {
  std::atomic<bool> active{false};  // false = no encoder, draw no cursor
  std::atomic<int> selected{0};
  std::atomic<bool> editing{false};
};

}  // namespace t3k

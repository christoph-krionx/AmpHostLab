// InputEvent: what the panel logic gets from the controls, whatever reads
// them (header GPIOs today, I2C expanders later). Presses arrive already
// sorted into short and long.

#pragma once

namespace t3k {

struct InputEvent {
  enum class Source { Nav, Value, Footswitch };
  enum class Kind { Turn, Press, LongPress };

  Source source;
  int index;   // value encoder 0..4 or footswitch 0..2; 0 for the nav encoder
  Kind kind;
  int delta;   // Turn: detents, + clockwise
  int step;    // Turn: acceleration factor for continuous values (1 = slow)
};

}  // namespace t3k

// EncoderReader: rotary encoders with push buttons on header GPIOs.
// Uses the kernel's GPIO edge events with internal pull-ups and debounce,
// so the encoder only needs wires to the pins and to GND.
//
// Not realtime: call wait() from a normal-priority thread.

#pragma once

#include <string>
#include <vector>

namespace t3k {

struct EncoderPins {
  // BCM GPIO numbers; sw = -1 for an encoder without button.
  int a;
  int b;
  int sw;
};

struct EncoderEvent {
  enum class Kind { Turn, Press, Release };
  int encoder;  // index into the pin list
  Kind kind;
  int delta;    // Turn: +1 per detent clockwise, -1 counter-clockwise
};

class EncoderReader {
public:
  EncoderReader() = default;
  ~EncoderReader();
  EncoderReader(const EncoderReader&) = delete;
  EncoderReader& operator=(const EncoderReader&) = delete;

  bool open(const std::vector<EncoderPins>& encoders);
  void close();
  const std::string& error() const { return lastError; }

  /** Blocks up to timeoutMs and appends what happened to `events`.
      Returns false on a read error. */
  bool wait(int timeoutMs, std::vector<EncoderEvent>& events);

private:
  struct State {
    EncoderPins pins;
    int level{3};       // (a << 1) | b, both high at rest
    int quarters{0};    // quadrature transitions since the last detent
  };

  int lineFd{-1};
  std::vector<State> states;
  std::string lastError;
};

}  // namespace t3k

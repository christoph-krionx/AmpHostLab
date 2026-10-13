// enc-test: wiring check for one rotary encoder with push button.
// Prints every detent and button event until Ctrl+C.

#include <cstdlib>
#include <iostream>
#include <string>

#include "input/EncoderReader.h"

int main(int argc, char** argv) {
  t3k::EncoderPins pins{5, 6, 26};
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string a = argv[i];
    const int v = std::atoi(argv[i + 1]);
    if (a == "--a") pins.a = v;
    else if (a == "--b") pins.b = v;
    else if (a == "--sw") pins.sw = v;
    else {
      std::cout << "Usage: enc-test [--a <gpio>] [--b <gpio>] [--sw <gpio>]   (default 5, 6, 26)\n";
      return 2;
    }
  }

  t3k::EncoderReader reader;
  if (!reader.open({pins})) {
    std::cerr << "Encoder error: " << reader.error() << std::endl;
    return 1;
  }
  std::cout << "Encoder on A=GPIO" << pins.a << " B=GPIO" << pins.b << " SW=GPIO" << pins.sw
            << ". Turn and press (Ctrl+C to quit)." << std::endl;

  int position = 0;
  std::vector<t3k::EncoderEvent> events;
  while (reader.wait(1000, events)) {
    for (const auto& e : events) {
      if (e.kind == t3k::EncoderEvent::Kind::Turn) {
        position += e.delta;
        std::cout << (e.delta > 0 ? "right  " : "left   ") << position << std::endl;
      } else {
        std::cout << (e.kind == t3k::EncoderEvent::Kind::Press ? "button pressed" : "button released") << std::endl;
      }
    }
    events.clear();
  }
  std::cerr << "Encoder error: read failed" << std::endl;
  return 1;
}

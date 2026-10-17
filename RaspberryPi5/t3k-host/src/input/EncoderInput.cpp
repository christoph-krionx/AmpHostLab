#include "EncoderInput.h"

namespace t3k {

namespace {

// Held this long, a press counts as long.
constexpr juce::uint32 kLongPressMs = 600;

// Faster turning takes bigger steps on the continuous values.
int stepFor(juce::uint32 msSinceLastDetent) {
  return msSinceLastDetent < 25 ? 4 : msSinceLastDetent < 60 ? 2 : 1;
}

}  // namespace

EncoderInput::EncoderInput(std::function<void(const InputEvent&)> s) : juce::Thread("encoder"), sink(std::move(s)) {}

EncoderInput::~EncoderInput() {
  stopThread(1000);
}

juce::String EncoderInput::start(const EncoderPins& nav, const EncoderPins* model) {
  std::vector<EncoderPins> pins{nav};
  sources = {InputEvent::Source::Nav};
  indices = {0};
  if (model != nullptr) {
    pins.push_back(*model);
    sources.push_back(InputEvent::Source::Model);
    indices.push_back(0);
  }
  if (!reader.open(pins))
    return reader.error();
  weakSelf = this;
  startThread(juce::Thread::Priority::normal);
  return {};
}

void EncoderInput::post(const InputEvent& event) {
  juce::MessageManager::callAsync([weak = weakSelf, event] {
    if (auto* self = weak.get())
      self->sink(event);
  });
}

void EncoderInput::run() {
  const size_t count = sources.size();
  std::vector<juce::uint32> lastTurnMs(count, juce::Time::getMillisecondCounter());
  // Per encoder: when its button went down, 0 while up or already handled.
  std::vector<juce::uint32> pressedAtMs(count, 0);
  std::vector<EncoderEvent> events;

  while (!threadShouldExit()) {
    events.clear();
    if (!reader.wait(50, events))
      break;
    const auto now = juce::Time::getMillisecondCounter();
    for (const auto& event : events) {
      const auto e = static_cast<size_t>(event.encoder);
      if (e >= count)
        continue;
      const auto source = sources[e];
      const int index = indices[e];
      switch (event.kind) {
        case EncoderEvent::Kind::Turn: {
          const int step = stepFor(now - lastTurnMs[e]);
          lastTurnMs[e] = now;
          // Turning while the button is down (a foot slipping onto the
          // knob) is no press at all.
          pressedAtMs[e] = 0;
          post({source, index, InputEvent::Kind::Turn, event.delta, step});
          break;
        }
        case EncoderEvent::Kind::Press: pressedAtMs[e] = juce::jmax<juce::uint32>(now, 1); break;
        case EncoderEvent::Kind::Release:
          if (pressedAtMs[e] != 0)
            post({source, index, InputEvent::Kind::Press, 0, 1});
          pressedAtMs[e] = 0;
          break;
      }
    }
    // A long press fires once the button has been held long enough, so the
    // page opens under the finger instead of on release.
    for (size_t e = 0; e < count; ++e) {
      if (pressedAtMs[e] != 0 && now - pressedAtMs[e] >= kLongPressMs) {
        post({sources[e], indices[e], InputEvent::Kind::LongPress, 0, 1});
        pressedAtMs[e] = 0;
      }
    }
  }
}

}  // namespace t3k

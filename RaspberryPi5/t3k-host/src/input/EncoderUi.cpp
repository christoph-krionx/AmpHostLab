#include "EncoderUi.h"

#include <iostream>

#include "Processor.h"
#include "display/DisplayUi.h"

namespace t3k {

namespace {

// Stepping through models loads only once the encoder has come to rest.
constexpr int kModelLoadDelayMs = 350;

// Faster turning takes bigger steps on the continuous values.
int stepFor(juce::uint32 msSinceLastDetent) {
  return msSinceLastDetent < 25 ? 4 : msSinceLastDetent < 60 ? 2 : 1;
}

}  // namespace

EncoderUi::EncoderUi(TONE3000Processor& p, UiState& s, DisplayUi* d)
    : juce::Thread("encoder"), processor(p), state(s), display(d) {}

EncoderUi::~EncoderUi() {
  stopTimer();
  stopThread(1000);
  state.active = false;
}

void EncoderUi::setTonesDir(const juce::File& dir) {
  tones.clear();
  for (const auto& file : dir.findChildFiles(juce::File::findFiles, false))
    if (file.getFileExtension().equalsIgnoreCase(".nam"))
      tones.add(file);
  std::sort(tones.begin(), tones.end(), [](const juce::File& a, const juce::File& b) {
    return a.getFileName().compareNatural(b.getFileName()) < 0;
  });
}

void EncoderUi::setCurrentTone(const juce::File& file, const std::string& blockId) {
  toneIndex = tones.indexOf(file);
  toneBlockId = blockId;
}

juce::String EncoderUi::start(const EncoderPins& pins) {
  if (!reader.open({pins}))
    return reader.error();
  state.selected = 0;
  state.editing = false;
  state.active = true;
  weakSelf = this;
  startThread(juce::Thread::Priority::normal);
  return {};
}

void EncoderUi::run() {
  std::vector<EncoderEvent> events;
  auto lastTurnMs = juce::Time::getMillisecondCounter();
  while (!threadShouldExit()) {
    events.clear();
    if (!reader.wait(100, events))
      break;
    for (const auto& event : events) {
      int step = 1;
      if (event.kind == EncoderEvent::Kind::Turn) {
        const auto now = juce::Time::getMillisecondCounter();
        step = stepFor(now - lastTurnMs);
        lastTurnMs = now;
      }
      juce::MessageManager::callAsync([weak = weakSelf, event, step] {
        if (auto* self = weak.get())
          self->handle(event, step);
      });
    }
  }
}

void EncoderUi::handle(EncoderEvent event, int step) {
  if (event.kind == EncoderEvent::Kind::Press) {
    const bool leaving = state.editing.load();
    if (leaving && isTimerRunning())  // a model is still waiting to load
      loadPendingModel();
    state.editing = !leaving;
    return;
  }
  if (event.kind != EncoderEvent::Kind::Turn)
    return;

  if (!state.editing.load()) {
    // The cursor wraps around at both ends.
    state.selected = (state.selected.load() + event.delta + kUiItemCount) % kUiItemCount;
    return;
  }
  adjust(static_cast<UiItem>(state.selected.load()), event.delta, step);
}

void EncoderUi::adjust(UiItem item, int delta, int step) {
  const float move = static_cast<float>(delta * step);
  switch (item) {
    case UiItem::Model: stepModel(delta); break;
    case UiItem::Gain: set("inputLevel", get("inputLevel") + move * 0.01f); break;
    case UiItem::Bass: set("toneBass", get("toneBass") + move * 0.1f); break;
    case UiItem::Mid: set("toneMid", get("toneMid") + move * 0.1f); break;
    case UiItem::Treble: set("toneTreble", get("toneTreble") + move * 0.1f); break;
    case UiItem::Volume: set("outputLevel", get("outputLevel") + move * 0.01f); break;
    case UiItem::Gate: {
      // One step below the lowest threshold switches the gate off.
      const float off = kGateMinDb - 1.0f;
      const float current = get("gateEnabled") > 0.5f ? juce::jmax(kGateMinDb, get("gateThreshold")) : off;
      const float wanted = juce::jlimit(off, 0.0f, std::round(current + move));
      set("gateEnabled", wanted > off ? 1.0f : 0.0f);
      if (wanted > off)
        set("gateThreshold", wanted);
      break;
    }
    case UiItem::Pitch: {
      // Whole semitones; 0 switches the shifter (and its latency) off.
      const float current = get("pitchEnabled") > 0.5f ? std::round(get("pitchSemitones")) : 0.0f;
      const float wanted = juce::jlimit(-24.0f, 24.0f, current + static_cast<float>(delta));
      set("pitchSemitones", wanted);
      set("pitchEnabled", wanted != 0.0f ? 1.0f : 0.0f);
      break;
    }
    case UiItem::Spread: set("spreadEnabled", delta > 0 ? 1.0f : 0.0f); break;
  }
}

void EncoderUi::stepModel(int delta) {
  if (tones.isEmpty())
    return;
  toneIndex = toneIndex < 0 ? 0 : (toneIndex + delta + tones.size()) % tones.size();
  if (display != nullptr)
    display->setTitle(tones[toneIndex].getFileNameWithoutExtension());
  startTimer(kModelLoadDelayMs);
}

void EncoderUi::timerCallback() {
  loadPendingModel();
}

void EncoderUi::loadPendingModel() {
  stopTimer();
  if (!juce::isPositiveAndBelow(toneIndex, tones.size()))
    return;
  const auto file = tones[toneIndex];
  const auto result = processor.loadLocalTonePath(file, toneBlockId);
  if (result.hasProperty("error")) {
    std::cerr << "Tone not loaded: " << result["error"].toString() << std::endl;
    return;
  }
  toneBlockId = result["blockId"].toString().toStdString();
  std::cout << "Tone loaded: " << file.getFileName() << std::endl;
}

float EncoderUi::get(const char* paramId) const {
  const auto* raw = processor.getParameters().getRawParameterValue(paramId);
  return raw != nullptr ? raw->load() : 0.0f;
}

void EncoderUi::set(const char* paramId, float value) {
  if (auto* param = processor.getParameters().getParameter(paramId))
    param->setValueNotifyingHost(param->convertTo0to1(param->getNormalisableRange().snapToLegalValue(value)));
}

}  // namespace t3k

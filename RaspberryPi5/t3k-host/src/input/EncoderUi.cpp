#include "EncoderUi.h"

#include <iostream>

#include "Processor.h"
#include "display/DisplayUi.h"

namespace t3k {

namespace {

// Stepping through presets loads only once the encoder has come to rest.
constexpr int kPresetLoadDelayMs = 350;

// Faster turning takes bigger steps on the continuous values.
int stepFor(juce::uint32 msSinceLastDetent) {
  return msSinceLastDetent < 25 ? 4 : msSinceLastDetent < 60 ? 2 : 1;
}

}  // namespace

EncoderUi::EncoderUi(TONE3000Processor& p, UiState& s, DisplayUi* d)
    : juce::Thread("encoder"), processor(p), state(s), display(d) {
  const auto list = processor.getPresetList();
  if (const auto* items = list["presets"].getArray())
    for (const auto& item : *items)
      presets.push_back({item["id"].toString(), item["name"].toString()});
  state.presetCount = static_cast<int>(presets.size());
}

EncoderUi::~EncoderUi() {
  stopTimer();
  stopThread(1000);
  state.active = false;
  state.tunerOn = false;
}

void EncoderUi::setCurrentPreset(const juce::String& name) {
  for (size_t i = 0; i < presets.size(); ++i)
    if (presets[i].name.equalsIgnoreCase(name))
      presetIndex = static_cast<int>(i);
  state.presetIndex = presetIndex;
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
  const int selected = state.selected.load();
  const int blockCount = static_cast<int>(state.getChain().tones().size());
  const auto target = cursorTarget(selected, blockCount);

  if (event.kind == EncoderEvent::Kind::Press) {
    if (target.isBlock) {  // on/off right away, no edit level
      toggleBlock(target.block);
      return;
    }
    const bool leaving = state.editing.load();
    if (leaving && isTimerRunning())  // a preset is still waiting to load
      loadPendingPreset();
    if (target.item == UiItem::Settings)  // no function yet
      return;
    if (target.item == UiItem::Save) {
      savePreset();
      return;
    }
    state.editing = !leaving;
    if (target.item == UiItem::Tuner)  // the tuner runs while its item is entered
      state.tunerOn = !leaving;
    return;
  }
  if (event.kind != EncoderEvent::Kind::Turn)
    return;

  if (!state.editing.load()) {
    // The cursor wraps around at both ends.
    const int itemCount = kUiItemCount + blockCount;
    state.selected = (selected + event.delta + itemCount) % itemCount;
    return;
  }
  adjust(target.item, event.delta, step);
}

void EncoderUi::toggleBlock(int toneIndex) {
  const auto chain = state.getChain();
  const auto tones = chain.tones();
  if (!juce::isPositiveAndBelow(toneIndex, static_cast<int>(tones.size())))
    return;
  const auto& cell = *tones[static_cast<size_t>(toneIndex)];
  processor.setBlockParam(cell.id, "enabled", cell.enabled ? 0.0 : 1.0);
}

void EncoderUi::adjust(UiItem item, int delta, int step) {
  const float move = static_cast<float>(delta * step);
  switch (item) {
    case UiItem::Preset: stepPreset(delta); break;
    case UiItem::Save:
    case UiItem::Tuner:
    case UiItem::Settings: break;
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

void EncoderUi::stepPreset(int delta) {
  if (presets.empty())
    return;
  const int count = static_cast<int>(presets.size());
  presetIndex = presetIndex < 0 ? 0 : (presetIndex + delta + count) % count;
  state.presetIndex = presetIndex;
  if (display != nullptr)
    display->setTitle(presets[static_cast<size_t>(presetIndex)].name);
  startTimer(kPresetLoadDelayMs);
}

// Writes the current sound back to the preset it came from.
void EncoderUi::savePreset() {
  if (isTimerRunning())  // a preset is still waiting to load
    loadPendingPreset();
  if (!juce::isPositiveAndBelow(presetIndex, static_cast<int>(presets.size())))
    return;
  const auto name = presets[static_cast<size_t>(presetIndex)].name;
  const auto saved = processor.savePreset(name);
  if (!saved.hasProperty("id")) {
    std::cerr << "Preset not saved: " << name << std::endl;
    return;
  }
  presets[static_cast<size_t>(presetIndex)].id = saved["id"].toString();
  state.presetSaved = true;
  std::cout << "Preset saved: " << name << std::endl;
}

void EncoderUi::timerCallback() {
  loadPendingPreset();
}

void EncoderUi::loadPendingPreset() {
  stopTimer();
  if (!juce::isPositiveAndBelow(presetIndex, static_cast<int>(presets.size())))
    return;
  const auto& preset = presets[static_cast<size_t>(presetIndex)];
  if (processor.loadPreset(preset.id)) {
    std::cout << "Preset loaded: " << preset.name << std::endl;
    if (onPresetLoaded)
      onPresetLoaded(preset.name);
  } else
    std::cerr << "Preset not loaded: " << preset.name << std::endl;
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

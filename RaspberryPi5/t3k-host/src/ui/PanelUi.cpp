#include "PanelUi.h"

#include <iostream>

#include "Processor.h"
#include "display/DisplayUi.h"

namespace t3k {

namespace {

// Stepping through presets loads only once the encoder has come to rest.
constexpr int kPresetLoadDelayMs = 350;
// Knob steps without a unit step of their own: 1 % of the range, 0.1 % fine.
constexpr float kKnobStep = 0.01f;
constexpr float kFineDivider = 10.0f;

}  // namespace

PanelUi::PanelUi(TONE3000Processor& p, UiState& s, DisplayUi* d) : processor(p), state(s), display(d) {
  const auto list = processor.getPresetList();
  if (const auto* items = list["presets"].getArray())
    for (const auto& item : *items)
      presets.push_back({item["id"].toString(), item["name"].toString()});
  state.presetCount = static_cast<int>(presets.size());
  state.page = static_cast<int>(Page::Overview);
  state.selected = 0;
  state.editing = false;
  state.active = true;
}

PanelUi::~PanelUi() {
  stopTimer();
  state.active = false;
  state.tunerOn = false;
}

void PanelUi::setCurrentPreset(const juce::String& name) {
  for (size_t i = 0; i < presets.size(); ++i)
    if (presets[i].name.equalsIgnoreCase(name))
      presetIndex = static_cast<int>(i);
  state.presetIndex = presetIndex;
}

std::vector<Target> PanelUi::targets() const {
  return cursorTargets(state.currentPage(), static_cast<int>(state.getChain().tones().size()),
                       state.knobsInPath.load());
}

void PanelUi::handle(const InputEvent& event) {
  switch (event.source) {
    case InputEvent::Source::Nav: handleNav(event); break;
    case InputEvent::Source::Value: {
      const int slot = event.index;
      const int bit = 1 << slot;
      if (event.kind == InputEvent::Kind::Turn)
        adjustKnob(slot, event.delta, event.step, (state.fineSlots.load() & bit) != 0);
      else if (event.kind == InputEvent::Kind::Press)
        state.fineSlots = state.fineSlots.load() ^ bit;
      else
        resetKnob(slot);
      break;
    }
    case InputEvent::Source::Footswitch: break;  // not assigned yet
  }
}

void PanelUi::handleNav(const InputEvent& event) {
  const auto list = targets();
  if (list.empty())
    return;
  const int selected = juce::jlimit(0, static_cast<int>(list.size()) - 1, state.selected.load());
  const auto target = list[static_cast<size_t>(selected)];
  const bool editing = state.editing.load();

  switch (event.kind) {
    case InputEvent::Kind::Turn:
      if (editing) {
        turnEditing(target, event.delta, event.step);
      } else {
        // The cursor wraps around at both ends.
        const int count = static_cast<int>(list.size());
        state.selected = ((selected + event.delta) % count + count) % count;
      }
      break;
    case InputEvent::Kind::Press:
      if (editing) {
        if (isTimerRunning())  // a preset is still waiting to load
          loadPendingPreset();
        if (target.kind == TargetKind::Head && target.index == static_cast<int>(HeadItem::Tuner))
          state.tunerOn = false;
        state.editing = false;
      } else {
        press(target);
      }
      break;
    case InputEvent::Kind::LongPress:
      if (editing) {
        if (target.kind == TargetKind::Knob)
          resetKnob(target.index);
      } else {
        longPress(target);
      }
      break;
  }
}

void PanelUi::press(const Target& target) {
  const auto& def = pageDef(state.currentPage());
  switch (target.kind) {
    case TargetKind::Head:
      switch (static_cast<HeadItem>(target.index)) {
        case HeadItem::Save: savePreset(); break;
        case HeadItem::Tuner:  // the tuner runs while its item is entered
          state.tunerOn = true;
          state.editing = true;
          break;
        case HeadItem::Preset: state.editing = true; break;
        case HeadItem::Settings: break;  // no function yet
      }
      break;
    case TargetKind::Switch: toggle(pageDef(kSwitchPages[target.index]).enableParam); break;
    case TargetKind::Block: toggleBlock(target.index); break;
    case TargetKind::Entry: {
      const char* id = def.entries[static_cast<size_t>(target.index)].paramId;
      if (isChoice(id))
        state.editing = true;
      else
        toggle(id);
      break;
    }
    case TargetKind::Knob: state.editing = true; break;
  }
}

void PanelUi::longPress(const Target& target) {
  if (state.currentPage() != Page::Overview) {
    closePage();
    return;
  }
  if (target.kind == TargetKind::Switch)
    openPage(kSwitchPages[target.index]);
}

void PanelUi::turnEditing(const Target& target, int delta, int step) {
  switch (target.kind) {
    case TargetKind::Head:
      if (target.index == static_cast<int>(HeadItem::Preset))
        stepPreset(delta);
      break;
    case TargetKind::Entry: {
      const char* id = pageDef(state.currentPage()).entries[static_cast<size_t>(target.index)].paramId;
      stepChoice(id, delta);
      break;
    }
    case TargetKind::Knob: adjustKnob(target.index, delta, step, false); break;
    case TargetKind::Switch:
    case TargetKind::Block: break;
  }
}

void PanelUi::openPage(Page page) {
  returnSelected = state.selected.load();
  state.editing = false;
  state.tunerOn = false;
  state.page = static_cast<int>(page);
  state.selected = 1;  // the page's own on/off, right after save
}

void PanelUi::closePage() {
  state.editing = false;
  state.page = static_cast<int>(Page::Overview);
  state.selected = returnSelected;
}

juce::RangedAudioParameter* PanelUi::param(const char* id) const {
  return id != nullptr ? processor.getParameters().getParameter(id) : nullptr;
}

bool PanelUi::isChoice(const char* paramId) const {
  return dynamic_cast<juce::AudioParameterChoice*>(param(paramId)) != nullptr;
}

void PanelUi::toggle(const char* paramId) {
  if (auto* p = param(paramId))
    p->setValueNotifyingHost(p->getValue() > 0.5f ? 0.0f : 1.0f);
}

void PanelUi::stepChoice(const char* paramId, int delta) {
  auto* choice = dynamic_cast<juce::AudioParameterChoice*>(param(paramId));
  if (choice == nullptr)
    return;
  const int count = choice->choices.size();
  const int index = juce::jlimit(0, count - 1, choice->getIndex() + delta);
  choice->setValueNotifyingHost(choice->convertTo0to1(static_cast<float>(index)));
}

void PanelUi::adjustKnob(int slot, int delta, int step, bool fine) {
  const auto& knobs = pageDef(state.currentPage()).knobs;
  if (!juce::isPositiveAndBelow(slot, static_cast<int>(knobs.size())))
    return;
  const auto& knob = knobs[static_cast<size_t>(slot)];
  auto* p = param(knob.paramId);
  if (p == nullptr)
    return;
  const float move = static_cast<float>(delta * step) / (fine ? kFineDivider : 1.0f);
  if (knob.unitStep > 0.0f) {
    const auto& range = p->getNormalisableRange();
    const float wanted = range.snapToLegalValue(realValue(*p) + move * knob.unitStep);
    p->setValueNotifyingHost(p->convertTo0to1(wanted));
  } else {
    p->setValueNotifyingHost(juce::jlimit(0.0f, 1.0f, p->getValue() + move * kKnobStep));
  }
}

void PanelUi::resetKnob(int slot) {
  const auto& knobs = pageDef(state.currentPage()).knobs;
  if (!juce::isPositiveAndBelow(slot, static_cast<int>(knobs.size())))
    return;
  if (auto* p = param(knobs[static_cast<size_t>(slot)].paramId))
    p->setValueNotifyingHost(p->getDefaultValue());
}

void PanelUi::toggleBlock(int toneIndex) {
  const auto chain = state.getChain();
  const auto tones = chain.tones();
  if (!juce::isPositiveAndBelow(toneIndex, static_cast<int>(tones.size())))
    return;
  const auto& cell = *tones[static_cast<size_t>(toneIndex)];
  processor.setBlockParam(cell.id, "enabled", cell.enabled ? 0.0 : 1.0);
}

void PanelUi::stepPreset(int delta) {
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
void PanelUi::savePreset() {
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

void PanelUi::timerCallback() {
  loadPendingPreset();
}

void PanelUi::loadPendingPreset() {
  stopTimer();
  if (!juce::isPositiveAndBelow(presetIndex, static_cast<int>(presets.size())))
    return;
  const auto& preset = presets[static_cast<size_t>(presetIndex)];
  if (processor.loadPreset(preset.id)) {
    std::cout << "Preset loaded: " << preset.name << std::endl;
    if (onPresetLoaded)
      onPresetLoaded(preset.name);
  } else {
    std::cerr << "Preset not loaded: " << preset.name << std::endl;
  }
}

}  // namespace t3k

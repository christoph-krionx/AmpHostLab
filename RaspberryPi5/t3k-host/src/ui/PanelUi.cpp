#include "PanelUi.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <thread>

#include "Processor.h"
#include "display/DisplayUi.h"

namespace t3k {

namespace {

// Stepping through presets or models loads only once the encoder has come
// to rest: each load is a whole model in the background.
constexpr int kPresetLoadDelayMs = 350;
// Knob steps without a unit step of their own: 1 % of the range, 0.1 % fine.
constexpr float kKnobStep = 0.01f;
constexpr float kFineDivider = 10.0f;
// Where the blocks start in the overview's cursor order.
constexpr int kFirstBlock = kHeadItems + kSwitches;

// A lane of the processor's chain state as block ids, inserts included.
std::vector<std::string> laneOrder(const juce::var& lane) {
  std::vector<std::string> order;
  if (const auto* items = lane.getArray())
    for (const auto& item : *items)
      order.push_back(item["blockId"].toString().toStdString());
  return order;
}

// The block's position among the tone blocks as the processor has them
// right now (the panel's copy of the chain can be a poll behind).
int freshToneIndex(TONE3000Processor& processor, const std::string& blockId) {
  const auto chain = processor.getChainState(-1);
  int index = 0;
  for (const char* key : {"chain", "chainRight"})
    if (const auto* items = chain[key].getArray())
      for (const auto& item : *items) {
        if (item["kind"].toString() == "insert")
          continue;
        if (item["blockId"].toString().toStdString() == blockId)
          return index;
        ++index;
      }
  return -1;
}

}  // namespace

PanelUi::PanelUi(TONE3000Processor& p, UiState& s, DisplayUi* d) : processor(p), state(s), display(d) {
  readPresets();
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

void PanelUi::readPresets() {
  presets.clear();
  const auto list = processor.getPresetList();
  if (const auto* items = list["presets"].getArray())
    for (const auto& item : *items)
      presets.push_back({item["id"].toString(), item["name"].toString()});
  state.presetCount = static_cast<int>(presets.size());
}

void PanelUi::presetFolderChanged(const juce::StringArray& changedIds) {
  // Keep pointing at the same preset, wherever the new order puts it.
  const juce::String activeId =
      juce::isPositiveAndBelow(presetIndex, static_cast<int>(presets.size())) ? presets[static_cast<size_t>(presetIndex)].id
                                                                               : juce::String();
  readPresets();
  presetIndex = -1;
  for (size_t i = 0; i < presets.size(); ++i)
    if (activeId.isNotEmpty() && presets[i].id == activeId)
      presetIndex = static_cast<int>(i);
  state.presetIndex = presetIndex;
  std::cout << "Presets changed: " << presets.size() << " in the list" << std::endl;
  state.notify("Presets updated");
  if (presetIndex < 0)
    return;

  const auto& active = presets[static_cast<size_t>(presetIndex)];
  if (display != nullptr)
    display->setTitle(active.name);  // it may have been renamed
  // The playing preset's file was replaced: play the new one, unless the
  // device has changes of its own that would be lost, or a preset is about
  // to load anyway.
  if (!changedIds.contains(active.id) || state.presetDirty.load() || isTimerRunning())
    return;
  if (processor.loadPreset(active.id)) {
    state.presetSaved = true;  // the reloaded sound is the new reference
    std::cout << "Preset reloaded: " << active.name << std::endl;
    state.notify("Preset reloaded: " + active.name);
  }
}

void PanelUi::toggleTuner() {
  const bool on = !state.tunerOn.load();
  state.removeArmed = false;
  state.powerArmed = false;
  if (on) {
    // The tuner shows on the overview, with the cursor on its button as if
    // it had been pressed there.
    state.moving = false;
    state.page = static_cast<int>(Page::Overview);
    state.selected = static_cast<int>(HeadItem::Tuner);
  }
  state.editing = on;
  state.tunerOn = on;
}

void PanelUi::presetChangedElsewhere(const juce::String& presetId) {
  if (juce::isPositiveAndBelow(presetIndex, static_cast<int>(presets.size())) &&
      presets[static_cast<size_t>(presetIndex)].id == presetId)
    return;  // the panel loaded it itself
  for (size_t i = 0; i < presets.size(); ++i) {
    if (presets[i].id != presetId)
      continue;
    stopTimer();  // a preset the encoder was about to load is overtaken
    presetIndex = static_cast<int>(i);
    state.presetIndex = presetIndex;
    if (display != nullptr)
      display->setTitle(presets[i].name);
    std::cout << "Preset loaded (MIDI): " << presets[i].name << std::endl;
    if (onPresetLoaded)
      onPresetLoaded(presets[i].name);
    return;
  }
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
    case InputEvent::Source::Model:
      if (event.kind == InputEvent::Kind::Turn)
        turnModelEncoder(event.delta);
      break;
    case InputEvent::Source::Footswitch: break;  // not assigned yet
  }
}

void PanelUi::handleNav(const InputEvent& event) {
  // A block on the move follows the turns until a press puts it down.
  if (state.moving.load()) {
    if (event.kind == InputEvent::Kind::Turn)
      moveBlock(event.delta);
    else
      endMove();
    return;
  }

  const auto list = targets();
  if (list.empty())
    return;
  const int selected = juce::jlimit(0, static_cast<int>(list.size()) - 1, state.selected.load());
  const auto target = list[static_cast<size_t>(selected)];
  const bool editing = state.editing.load();

  // An armed remove only survives the press that confirms it.
  const auto entryKind = target.kind == TargetKind::Entry
                             ? pageDef(state.currentPage()).entries[static_cast<size_t>(target.index)].kind
                             : EntryKind::Param;
  const bool confirming = event.kind == InputEvent::Kind::Press;
  if (!(confirming && entryKind == EntryKind::BlockRemove))
    state.removeArmed = false;
  if (!(confirming && entryKind == EntryKind::PowerOff))
    state.powerArmed = false;

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
      if (editing)
        endEditing(target);
      else
        press(target);
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
        case HeadItem::Settings: openPage(Page::Settings); break;
      }
      break;
    case TargetKind::Switch: toggle(pageDef(kSwitchPages[target.index]).enableParam); break;
    case TargetKind::Block: toggleBlock(target.index); break;
    case TargetKind::Entry: {
      const auto& entry = def.entries[static_cast<size_t>(target.index)];
      if (entry.kind == EntryKind::InputMode) {
        state.editing = true;
      } else if (entry.kind == EntryKind::PowerOff) {
        if (state.powerArmed.load())
          powerOff();
        else
          state.powerArmed = true;
      } else if (entry.kind != EntryKind::Param)
        pressBlockEntry(entry.kind);
      else if (isChoice(entry.paramId))
        state.editing = true;
      else
        toggle(entry.paramId);
      break;
    }
    case TargetKind::Knob: state.editing = true; break;
  }
}

void PanelUi::pressBlockEntry(EntryKind kind) {
  const auto chain = state.getChain();
  const auto* cell = chain.find(state.getPageBlock());
  if (cell == nullptr)
    return;
  switch (kind) {
    case EntryKind::BlockOn: setBlockSwitch("enabled", !cell->enabled); break;
    case EntryKind::BlockNormalize: setBlockSwitch("normalize", !cell->normalize); break;
    case EntryKind::BlockModel: {
      // Only a tone with several models of its own has something to pick.
      if (cell->models.size() < 2)
        break;
      int index = 0;
      for (size_t i = 0; i < cell->models.size(); ++i)
        if (cell->models[i].id == cell->activeModel)
          index = static_cast<int>(i);
      state.pendingModel = index;
      appliedModel = cell->activeModel;
      state.editing = true;
      break;
    }
    case EntryKind::BlockMove: startMove(); break;
    case EntryKind::BlockRemove:
      if (state.removeArmed.load())
        removeBlock();
      else
        state.removeArmed = true;
      break;
    case EntryKind::Param:
    case EntryKind::InputMode:
    case EntryKind::PowerOff: break;
  }
}

void PanelUi::longPress(const Target& target) {
  if (state.currentPage() != Page::Overview) {
    closePage();
    return;
  }
  if (target.kind == TargetKind::Switch)
    openPage(kSwitchPages[target.index]);
  else if (target.kind == TargetKind::Block)
    openBlockPage(target.index);
  else if (target.kind == TargetKind::Head && target.index == static_cast<int>(HeadItem::Preset))
    discardChanges();
}

// Back to the preset as it is saved.
void PanelUi::discardChanges() {
  if (!state.presetDirty.load()) {
    state.notify("No changes to discard");
    return;
  }
  if (!juce::isPositiveAndBelow(presetIndex, static_cast<int>(presets.size())))
    return;
  const auto& preset = presets[static_cast<size_t>(presetIndex)];
  if (processor.loadPreset(preset.id)) {
    state.presetSaved = true;
    state.notify("Changes discarded");
    std::cout << "Changes discarded: " << preset.name << std::endl;
  }
}

// The device shuts down; the screen keeps telling when it is safe to
// unplug, since the panel holds its last picture while it has power.
// Needs a sudoers rule for the user (see README).
void PanelUi::powerOff() {
  state.powerArmed = false;
  state.shuttingDown = true;
  std::cout << "Power off" << std::endl;
  std::thread([weak = juce::WeakReference<PanelUi>(this)] {
    if (std::system("sudo -n /usr/bin/systemctl poweroff") == 0)
      return;
    juce::MessageManager::callAsync([weak] {
      if (auto* self = weak.get()) {
        self->state.shuttingDown = false;
        self->state.notify("Power off not allowed (see README)", 4000);
      }
    });
  }).detach();
}

// The block under the cursor, or the block page's block, steps through
// its tone's models; each pick loads once the encoder rests.
void PanelUi::turnModelEncoder(int delta) {
  std::string blockId;
  const auto chain = state.getChain();
  if (state.currentPage() == Page::Block) {
    blockId = state.getPageBlock();
  } else if (state.currentPage() == Page::Overview && !state.moving.load()) {
    const auto list = targets();
    const int selected = state.selected.load();
    const auto tones = chain.tones();
    if (juce::isPositiveAndBelow(selected, static_cast<int>(list.size()))) {
      const auto target = list[static_cast<size_t>(selected)];
      if (target.kind == TargetKind::Block && juce::isPositiveAndBelow(target.index, static_cast<int>(tones.size())))
        blockId = tones[static_cast<size_t>(target.index)]->id;
    }
  }
  const auto* cell = chain.find(blockId);
  if (cell == nullptr) {
    state.notify("Select a block to change its model");
    return;
  }
  if (cell->models.size() < 2) {
    state.notify("This tone has one model");
    return;
  }
  // A new block, or nothing picked yet: start from the playing model.
  if (state.getPageBlock() != blockId || state.pendingModel.load() < 0) {
    state.setPageBlock(blockId);
    int index = 0;
    for (size_t i = 0; i < cell->models.size(); ++i)
      if (cell->models[i].id == cell->activeModel)
        index = static_cast<int>(i);
    state.pendingModel = index;
    appliedModel = cell->activeModel;
  }
  state.pendingModel = juce::jlimit(0, static_cast<int>(cell->models.size()) - 1, state.pendingModel.load() + delta);
  const int request = ++modelRequest;
  juce::Timer::callAfterDelay(kPresetLoadDelayMs, [weak = juce::WeakReference<PanelUi>(this), request] {
    if (auto* self = weak.get(); self != nullptr && request == self->modelRequest && !self->state.editing.load())
      self->applyModel(true);
  });
}

void PanelUi::turnEditing(const Target& target, int delta, int step) {
  switch (target.kind) {
    case TargetKind::Head:
      if (target.index == static_cast<int>(HeadItem::Preset))
        stepPreset(delta);
      break;
    case TargetKind::Entry: {
      const auto& entry = pageDef(state.currentPage()).entries[static_cast<size_t>(target.index)];
      if (entry.kind == EntryKind::BlockModel)
        stepModel(delta);
      else if (entry.kind == EntryKind::InputMode)
        stepInputMode(delta);
      else
        stepChoice(entry.paramId, delta);
      break;
    }
    case TargetKind::Knob: adjustKnob(target.index, delta, step, false); break;
    case TargetKind::Switch:
    case TargetKind::Block: break;
  }
}

void PanelUi::endEditing(const Target& target) {
  if (isTimerRunning())  // a preset is still waiting to load
    loadPendingPreset();
  if (target.kind == TargetKind::Head && target.index == static_cast<int>(HeadItem::Tuner))
    state.tunerOn = false;
  if (state.pendingModel.load() >= 0) {
    ++modelRequest;  // nothing scheduled loads after this
    applyModel(true);
  }
  // The input mode is kept for the next start.
  if (target.kind == TargetKind::Entry && state.currentPage() == Page::Settings && onSettingChanged)
    onSettingChanged("input-mode", TONE3000Processor::inputModeToString(processor.getInputMode()));
  state.editing = false;
}

void PanelUi::openPage(Page page) {
  returnSelected = state.selected.load();
  state.editing = false;
  state.tunerOn = false;
  state.page = static_cast<int>(page);
  // The page's own on/off, right after save; the settings page has no save.
  state.selected = page == Page::Settings ? 0 : 1;
}

void PanelUi::openBlockPage(int toneIndex) {
  const auto chain = state.getChain();
  const auto tones = chain.tones();
  if (!juce::isPositiveAndBelow(toneIndex, static_cast<int>(tones.size())))
    return;
  state.setPageBlock(tones[static_cast<size_t>(toneIndex)]->id);
  state.removeArmed = false;
  openPage(Page::Block);
}

void PanelUi::closePage() {
  state.editing = false;
  state.removeArmed = false;
  int back = returnSelected;
  // Back onto the block itself, wherever it stands now.
  if (state.currentPage() == Page::Block) {
    const int index = freshToneIndex(processor, state.getPageBlock());
    if (index >= 0)
      back = kFirstBlock + index;
  }
  state.page = static_cast<int>(Page::Overview);
  state.selected = back;
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
  const float move = static_cast<float>(delta * step) / (fine ? kFineDivider : 1.0f);
  if (knob.blockParam) {
    const auto chain = state.getChain();
    if (const auto* cell = chain.find(state.getPageBlock()))
      setBlockKnob(knob, blockValue(knob, *cell) + move * kKnobStep);
    return;
  }
  auto* p = param(knob.paramId);
  if (p == nullptr)
    return;
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
  const auto& knob = knobs[static_cast<size_t>(slot)];
  if (knob.blockParam)
    setBlockKnob(knob, knob.blockDefault);
  else if (auto* p = param(knob.paramId))
    p->setValueNotifyingHost(p->getDefaultValue());
}

void PanelUi::setBlockKnob(const KnobDef& knob, float value) {
  const auto id = state.getPageBlock();
  const float n = juce::jlimit(0.0f, 1.0f, value);
  if (!processor.setBlockParam(id, knob.paramId, n))
    return;
  // The processor reports a turned knob only once the turning has settled,
  // so the screen takes the value straight away.
  const juce::String name(knob.paramId);
  state.updateCell(id, [&name, n](ChainCell& cell) {
    (name == "inputGain" ? cell.inputGain : name == "outputGain" ? cell.outputGain : cell.mix) = n;
  });
}

void PanelUi::toggleBlock(int toneIndex) {
  const auto chain = state.getChain();
  const auto tones = chain.tones();
  if (!juce::isPositiveAndBelow(toneIndex, static_cast<int>(tones.size())))
    return;
  const auto& cell = *tones[static_cast<size_t>(toneIndex)];
  processor.setBlockParam(cell.id, "enabled", cell.enabled ? 0.0 : 1.0);
}

void PanelUi::setBlockSwitch(const char* name, bool on) {
  const auto id = state.getPageBlock();
  if (!processor.setBlockParam(id, name, on ? 1.0 : 0.0))
    return;
  const juce::String which(name);
  state.updateCell(id, [&which, on](ChainCell& cell) { (which == "enabled" ? cell.enabled : cell.normalize) = on; });
}

// Switches right away, so the difference can be heard while turning.
void PanelUi::stepInputMode(int delta) {
  const int current = static_cast<int>(processor.getInputMode());
  int index = 0;
  for (int i = 0; i < static_cast<int>(std::size(kInputModes)); ++i)
    if (kInputModes[i] == current)
      index = i;
  index = juce::jlimit(0, static_cast<int>(std::size(kInputModes)) - 1, index + delta);
  processor.setInputMode(static_cast<TONE3000Processor::InputMode>(kInputModes[index]));
}

void PanelUi::stepModel(int delta) {
  const auto chain = state.getChain();
  const auto* cell = chain.find(state.getPageBlock());
  if (cell == nullptr || cell->models.empty())
    return;
  state.pendingModel = juce::jlimit(0, static_cast<int>(cell->models.size()) - 1, state.pendingModel.load() + delta);
  // The model follows the turning, once the encoder rests.
  const int request = ++modelRequest;
  juce::Timer::callAfterDelay(kPresetLoadDelayMs, [weak = juce::WeakReference<PanelUi>(this), request] {
    if (auto* self = weak.get(); self != nullptr && request == self->modelRequest)
      self->applyModel(false);
  });
}

// Sends the picked model to the block, unless it is already there.
// `finished`: editing ends, the pick is done.
void PanelUi::applyModel(bool finished) {
  const int index = finished ? state.pendingModel.exchange(-1) : state.pendingModel.load();
  const auto id = state.getPageBlock();
  const auto chain = state.getChain();
  const auto* cell = chain.find(id);
  if (cell == nullptr || !juce::isPositiveAndBelow(index, static_cast<int>(cell->models.size())))
    return;
  const auto& model = cell->models[static_cast<size_t>(index)];
  if (model.id == appliedModel || model.url.isEmpty())
    return;
  appliedModel = model.id;
  juce::DynamicObject::Ptr data = new juce::DynamicObject();
  data->setProperty("id", model.id);
  data->setProperty("name", model.name);
  data->setProperty("model_url", model.url);
  if (!processor.switchModel(id, model.id, juce::var(data.get())))
    std::cerr << "Model not switched: " << model.name << std::endl;
}

void PanelUi::startMove() {
  const int index = freshToneIndex(processor, state.getPageBlock());
  if (index < 0)
    return;
  state.editing = false;
  state.page = static_cast<int>(Page::Overview);
  state.selected = kFirstBlock + index;
  state.moving = true;
}

// One place per detent along the block's own row; empty slots count as
// places, so a block can be put down with a gap before it.
void PanelUi::moveBlock(int delta) {
  const auto id = state.getPageBlock();
  const auto chain = processor.getChainState(-1);
  for (const char* key : {"chain", "chainRight"}) {
    auto order = laneOrder(chain[key]);
    const auto found = std::find(order.begin(), order.end(), id);
    if (found == order.end())
      continue;
    int index = static_cast<int>(found - order.begin());
    const int wanted = juce::jlimit(0, static_cast<int>(order.size()) - 1, index + delta);
    if (wanted == index)
      return;
    while (index != wanted) {
      const int next = index + (wanted > index ? 1 : -1);
      std::swap(order[static_cast<size_t>(index)], order[static_cast<size_t>(next)]);
      index = next;
    }
    processor.reorderChainBlocks(order);
    return;
  }
}

void PanelUi::endMove() {
  state.moving = false;
  const int index = freshToneIndex(processor, state.getPageBlock());
  if (index >= 0)
    state.selected = kFirstBlock + index;
}

void PanelUi::removeBlock() {
  const auto id = state.getPageBlock();
  state.removeArmed = false;
  state.editing = false;
  if (!processor.removeChainBlock(id))
    std::cerr << "Block not removed" << std::endl;
  // Onto the block that took its place, or the one before when it was last.
  int tones = 0;
  const auto chain = processor.getChainState(-1);
  for (const char* key : {"chain", "chainRight"})
    if (const auto* items = chain[key].getArray())
      for (const auto& item : *items)
        tones += item["kind"].toString() == "insert" ? 0 : 1;
  state.page = static_cast<int>(Page::Overview);
  state.selected = tones == 0 ? 0 : kFirstBlock + juce::jmin(returnSelected - kFirstBlock, tones - 1);
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
  presetFolder.resync();  // our own save is no change from outside
  state.notify("Saved");
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

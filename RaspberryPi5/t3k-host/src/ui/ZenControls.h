// ZenControls: what the value encoders do in the zen mode, the idle view
// that shows only the preset. Each encoder has one control there, the same
// for every preset: a processor parameter (a knob, a switch, a choice), a
// block's knob or a block's on/off. Blocks are named by their place in the
// chain (the second block), since every preset has blocks of its own.
//
// A long press on a value encoder puts the control under the cursor on it.
// The settings file keeps the assignment as "zen-knob-N = <control>":
//   param:<id>         a processor parameter
//   block<N>:on        the Nth block's on/off
//   block<N>:<knob>    the Nth block's inputGain | outputGain | mix

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "ui/ChainView.h"
#include "ui/PanelModel.h"

namespace t3k {

struct ZenControl {
  enum class Kind { None, Param, BlockOn, BlockKnob };
  Kind kind{Kind::None};
  juce::String id;  // the parameter, or the block knob
  int block{0};     // the block's place among the tone blocks, from 0

  juce::String toString() const {
    switch (kind) {
      case Kind::Param: return "param:" + id;
      case Kind::BlockOn: return "block" + juce::String(block + 1) + ":on";
      case Kind::BlockKnob: return "block" + juce::String(block + 1) + ":" + id;
      case Kind::None: break;
    }
    return {};
  }

  static ZenControl parse(const juce::String& text) {
    const auto head = text.upToFirstOccurrenceOf(":", false, false).trim();
    const auto tail = text.fromFirstOccurrenceOf(":", false, false).trim();
    if (head == "param" && tail.isNotEmpty())
      return {Kind::Param, tail, 0};
    if (head.startsWith("block") && tail.isNotEmpty()) {
      const int block = head.substring(5).getIntValue() - 1;
      if (block < 0)
        return {};
      if (tail == "on")
        return {Kind::BlockOn, {}, block};
      return {Kind::BlockKnob, tail, block};
    }
    return {};
  }
};

/** Noise gate, then the preset's bass, mid and treble, then its volume. */
inline ZenControl defaultZenControl(int slot) {
  static const char* const ids[kKnobSlots] = {"gateThreshold", "toneBass", "toneMid", "toneTreble", "outputLevel"};
  return {ZenControl::Kind::Param, ids[juce::jlimit(0, kKnobSlots - 1, slot)], 0};
}

/** Where the panel shows a processor parameter: as a knob or as an entry. */
struct ParamPlace {
  Page page{Page::Overview};
  const KnobDef* knob{nullptr};
  const EntryDef* entry{nullptr};
};

inline bool findParamPlace(const juce::String& id, ParamPlace& out) {
  for (int p = 0; p < kPageCount; ++p)
    for (const auto& knob : pageDef(static_cast<Page>(p)).knobs)
      if (!knob.blockParam && id == knob.paramId) {
        out = {static_cast<Page>(p), &knob, nullptr};
        return true;
      }
  for (int p = 0; p < kPageCount; ++p)
    for (const auto& entry : pageDef(static_cast<Page>(p)).entries)
      if (entry.kind == EntryKind::Param && entry.paramId != nullptr && id == entry.paramId) {
        out = {static_cast<Page>(p), nullptr, &entry};
        return true;
      }
  return false;
}

inline const KnobDef* blockKnobDef(const juce::String& id) {
  for (const auto& knob : pageDef(Page::Block).knobs)
    if (id == knob.paramId)
      return &knob;
  return nullptr;
}

/** A control as the zen mode shows it while it turns. */
struct ZenReading {
  juce::String name;
  juce::String value;
  float position{-1.0f};  // 0..1 for a knob, -1 for a switch or a choice
  bool on{true};          // a switch: on
};

inline ZenReading readZenControl(const ZenControl& control, juce::AudioProcessorValueTreeState& params,
                                 const ChainView& chain) {
  const auto dot = juce::String::fromUTF8(" \xc2\xb7 ");
  ZenReading r;
  const auto onOff = [&r](bool on) {
    r.on = on;
    r.value = on ? "On" : "Off";
  };
  switch (control.kind) {
    case ZenControl::Kind::None: r.name = "Not assigned"; break;
    case ZenControl::Kind::Param: {
      ParamPlace place;
      auto* p = params.getParameter(control.id);
      if (p == nullptr || !findParamPlace(control.id, place)) {
        r.name = control.id;
        r.value = "-";
        break;
      }
      const juce::String title(pageDef(place.page).title);
      if (place.knob != nullptr) {
        r.name = place.page == Page::Overview ? juce::String(place.knob->name) : title + dot + place.knob->name;
        r.value = formatKnob(*place.knob, params, nullptr, true);
        r.position = knobPosition(*place.knob, params, nullptr);
      } else {
        // The page's own on/off reads as the page; other entries carry it.
        const auto* enable = pageDef(place.page).enableParam;
        const bool pageSwitch = enable != nullptr && control.id == enable;
        r.name = pageSwitch ? title : title + dot + place.entry->label;
        if (auto* choice = dynamic_cast<juce::AudioParameterChoice*>(p))
          r.value = choice->getCurrentChoiceName();
        else
          onOff(p->getValue() > 0.5f);
      }
      break;
    }
    case ZenControl::Kind::BlockOn:
    case ZenControl::Kind::BlockKnob: {
      const auto tones = chain.tones();
      const auto* knob = control.kind == ZenControl::Kind::BlockKnob ? blockKnobDef(control.id) : nullptr;
      const juce::String what = knob != nullptr ? dot + knob->name : juce::String();
      if (!juce::isPositiveAndBelow(control.block, static_cast<int>(tones.size()))) {
        r.name = "Block " + juce::String(control.block + 1) + what;
        r.value = "No block";
        break;
      }
      const auto* cell = tones[static_cast<size_t>(control.block)];
      r.name = cell->name + what;
      if (control.kind == ZenControl::Kind::BlockOn) {
        onOff(cell->enabled);
      } else if (knob != nullptr) {
        r.value = formatKnob(*knob, params, cell, true);
        r.position = knobPosition(*knob, params, cell);
      }
      break;
    }
  }
  return r;
}

}  // namespace t3k

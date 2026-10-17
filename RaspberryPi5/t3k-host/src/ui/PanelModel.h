// PanelModel: the pages of the front panel as data, shared by the display
// (what to draw) and the panel logic (what a turn or press does).
//
// Every page has the same frame: the top line, the switch row, the stage
// (the chain grid on the overview, a page's entries otherwise) and the knob
// row of five slots along the bottom edge, one slot above each value
// encoder. The navigation encoder's cursor walks the targets in screen
// order; while there are no value encoders it walks through the knob slots
// as well and edits them itself.

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <vector>

#include "ImageDeck.h"
#include "ChainView.h"

namespace t3k {

enum class Page { Overview, Gate, Pitch, Spread, Block, Settings };
constexpr int kPageCount = 6;
constexpr int kKnobSlots = 5;

// The top line's items, left to right.
enum class HeadItem { Preset, Save, Tuner, Settings };
constexpr int kHeadItems = 4;

// How a knob shows its value.
enum class Format {
  Tenths,      // the parameter times `scale`, one decimal (overview knobs, 0..10)
  Db,          // whole dB
  Ms,          // whole ms
  Hz,          // whole Hz; the top of the range reads "Off" (pitch tonality)
  Semitones,   // signed; whole while pitch STEP is on
  Percent,     // normalized 0..1 as 0..100 %
  SpreadOffset,  // bipolar 0..1 -> L/R lag in ms
  CrossoverHz,   // normalized on the image deck's log Hz map
  GainDb,        // a block's normalized gain, 0.5 = 0 dB, +-24 dB
};

struct KnobDef {
  const char* label;  // under the knob
  const char* name;   // in the context line
  const char* paramId;  // a processor parameter, or the block's own (blockParam)
  Format format;
  float scale{1.0f};  // Tenths only
  float unitStep{0.0f};  // one detent in the parameter's own units; 0 = 1 % of the range
  bool blockParam{false};  // paramId names a block parameter (setBlockParam)
  float blockDefault{0.5f};
};

// A line on a page's stage. A parameter entry is a switch (bool, a press
// flips it) or a choice (a press edits it, turning picks); the block page
// has entries of its own.
enum class EntryKind { Param, BlockOn, BlockNormalize, BlockModel, BlockMove, BlockRemove, InputMode, PowerOff };
struct EntryDef {
  const char* label;
  const char* paramId;
  EntryKind kind{EntryKind::Param};
};

struct PageDef {
  const char* title;
  const char* enableParam;  // the page's own on/off, also its switch-row badge
  std::vector<EntryDef> entries;
  std::vector<KnobDef> knobs;  // at most kKnobSlots
};

inline const PageDef& pageDef(Page page) {
  static const PageDef pages[kPageCount] = {
      {"", nullptr, {},
       {{"GAIN", "Gain", "inputLevel", Format::Tenths, 10.0f},
        {"BASS", "Bass", "toneBass", Format::Tenths, 1.0f},
        {"MID", "Mid", "toneMid", Format::Tenths, 1.0f},
        {"TREBLE", "Treble", "toneTreble", Format::Tenths, 1.0f},
        {"VOLUME", "Volume", "outputLevel", Format::Tenths, 10.0f}}},
      {"Noise gate", "gateEnabled", {{"Gate", "gateEnabled"}},
       {{"THRESH", "Threshold", "gateThreshold", Format::Db, 1.0f, 1.0f},
        {"RELEASE", "Release", "gateRelease", Format::Ms},
        {"HOLD", "Hold", "gateHold", Format::Ms, 1.0f, 2.0f},
        {"RANGE", "Range", "gateRange", Format::Db, 1.0f, 1.0f}}},
      {"Pitch", "pitchEnabled", {{"Pitch", "pitchEnabled"}, {"Step", "pitchStep"}, {"Window", "pitchWindow"}},
       {{"SEMI", "Semitones", "pitchSemitones", Format::Semitones, 1.0f, 1.0f},
        {"TONALITY", "Tonality", "pitchTonality", Format::Hz}}},
      {"Spread", "spreadEnabled",
       {{"Spread", "spreadEnabled"},
        {"Wobble", "spreadWobbleEnabled"},
        {"Crossover", "spreadCrossoverEnabled"},
        {"Diffuse", "spreadDiffuseEnabled"}},
       {{"OFFSET", "Offset", "spreadOffset", Format::SpreadOffset},
        {"WOBBLE", "Wobble", "spreadWobble", Format::Percent},
        {"X-OVER", "Crossover", "spreadCrossover", Format::CrossoverHz}}},
      // Block: the title is the block's name. Move and remove share a line.
      {"", nullptr,
       {{"Block", nullptr, EntryKind::BlockOn},
        {"Normalize", nullptr, EntryKind::BlockNormalize},
        {"Model", nullptr, EntryKind::BlockModel},
        {"Move", nullptr, EntryKind::BlockMove},
        {"Remove", nullptr, EntryKind::BlockRemove}},
       {{"INPUT", "Input", "inputGain", Format::GainDb, 1.0f, 0.0f, true, 0.5f},
        {"OUTPUT", "Output", "outputGain", Format::GainDb, 1.0f, 0.0f, true, 0.5f},
        {"MIX", "Mix", "mix", Format::Percent, 1.0f, 0.0f, true, 1.0f}}},
      // Settings: the machine, not the sound; below the entries the stage
      // shows network, load and version.
      {"Settings", nullptr, {{"Input", nullptr, EntryKind::InputMode}, {"Power off", nullptr, EntryKind::PowerOff}}, {}},
  };
  return pages[static_cast<int>(page)];
}

// The input modes in the order the settings page steps through them,
// with their names there. The plugin's own order differs.
constexpr int kInputModes[] = {1, 2, 0, 3};  // left, right, stereo, dual mono
inline const char* inputModeName(int mode) {
  switch (mode) {
    case 1: return "Left (input 1)";
    case 2: return "Right (input 2)";
    case 3: return "Dual mono";
    default: return "Stereo";
  }
}

// The switch row, left to right; each opens its page on a long press.
constexpr Page kSwitchPages[] = {Page::Gate, Page::Pitch, Page::Spread};
constexpr int kSwitches = 3;

// One stop of the navigation cursor.
enum class TargetKind { Head, Switch, Block, Entry, Knob };
struct Target {
  TargetKind kind;
  int index;  // HeadItem, switch, tone block, entry or knob slot
};

/** The cursor stops of a page in screen order. On the overview: the top
    line, the switch row, the chain's tone blocks, the knobs. On a page:
    save (not on the settings page, which changes no sound), the stage
    entries, the knobs. Knobs only while they are edited with the
    navigation encoder (no value encoders). */
inline std::vector<Target> cursorTargets(Page page, int blockCount, bool knobsInPath) {
  std::vector<Target> out;
  const auto& def = pageDef(page);
  if (page == Page::Overview) {
    for (int i = 0; i < kHeadItems; ++i)
      out.push_back({TargetKind::Head, i});
    for (int i = 0; i < kSwitches; ++i)
      out.push_back({TargetKind::Switch, i});
    for (int i = 0; i < blockCount; ++i)
      out.push_back({TargetKind::Block, i});
  } else {
    if (page != Page::Settings)
      out.push_back({TargetKind::Head, static_cast<int>(HeadItem::Save)});
    for (int i = 0; i < static_cast<int>(def.entries.size()); ++i)
      out.push_back({TargetKind::Entry, i});
  }
  if (knobsInPath)
    for (int i = 0; i < static_cast<int>(def.knobs.size()); ++i)
      out.push_back({TargetKind::Knob, i});
  return out;
}

inline juce::RangedAudioParameter* findParam(juce::AudioProcessorValueTreeState& params, const char* id) {
  return params.getParameter(id);
}

inline float realValue(const juce::RangedAudioParameter& p) {
  return p.convertFrom0to1(p.getValue());
}

/** A block knob's normalized value. */
inline float blockValue(const KnobDef& knob, const ChainCell& cell) {
  const juce::String id(knob.paramId);
  return id == "inputGain" ? cell.inputGain : id == "outputGain" ? cell.outputGain : cell.mix;
}

/** The knob's value as text: the number alone for inside the knob, or with
    its unit for the context line. `cell` is the block page's block. */
inline juce::String formatKnob(const KnobDef& knob, juce::AudioProcessorValueTreeState& params, const ChainCell* cell,
                               bool withUnit) {
  const auto unit = [withUnit](const char* u) { return withUnit ? juce::String(" ") + u : juce::String(); };
  if (knob.blockParam) {
    if (cell == nullptr)
      return "-";
    const float n = blockValue(knob, *cell);
    if (knob.format == Format::GainDb) {
      const float db = (n - 0.5f) * 48.0f;
      return (db > 0.04f ? "+" : "") + juce::String(db, 1) + unit("dB");
    }
    return juce::String(juce::roundToInt(n * 100.0f)) + unit("%");
  }
  auto* p = findParam(params, knob.paramId);
  if (p == nullptr)
    return "-";
  const float v = realValue(*p);
  switch (knob.format) {
    case Format::Tenths: return juce::String(v * knob.scale, 1);
    case Format::Db: return juce::String(juce::roundToInt(v)) + unit("dB");
    case Format::Ms: return juce::String(juce::roundToInt(v)) + unit("ms");
    case Format::Hz:
      if (p->getValue() >= 0.999f)
        return "Off";
      return juce::String(juce::roundToInt(v)) + unit("Hz");
    case Format::Semitones: {
      auto* step = findParam(params, "pitchStep");
      const bool whole = step == nullptr || step->getValue() > 0.5f;
      const auto text = whole ? juce::String(juce::roundToInt(v)) : juce::String(v, 1);
      return (v > 0.0f && text != "0" ? "+" : "") + text + unit("st");
    }
    case Format::Percent: return juce::String(juce::roundToInt(v * 100.0f)) + unit("%");
    case Format::SpreadOffset: {
      // Same decode as SpreadParams: > 0 lags the right channel.
      const float ms = (juce::jlimit(0.0f, 1.0f, v) * 2.0f - 1.0f) * 24.0f;
      if (std::abs(ms) < 0.12f)
        return "0" + unit("ms");
      return juce::String(ms > 0.0f ? "R " : "L ") + juce::String(std::abs(ms), 1) + unit("ms");
    }
    case Format::CrossoverHz: return juce::String(juce::roundToInt(deckCrossoverHz(v))) + unit("Hz");
    case Format::GainDb: break;
  }
  return {};
}

/** Where the knob's arc stands, 0..1. */
inline float knobPosition(const KnobDef& knob, juce::AudioProcessorValueTreeState& params, const ChainCell* cell) {
  if (knob.blockParam)
    return cell != nullptr ? juce::jlimit(0.0f, 1.0f, blockValue(knob, *cell)) : 0.0f;
  auto* p = findParam(params, knob.paramId);
  return p != nullptr ? juce::jlimit(0.0f, 1.0f, p->getValue()) : 0.0f;
}

}  // namespace t3k

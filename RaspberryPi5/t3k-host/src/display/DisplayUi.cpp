#include "DisplayUi.h"

#include "Processor.h"
#include "core/GearGlyphs.h"
#include "core/LucideIcons.h"

namespace t3k {

namespace {

constexpr int kFrameMs = 50;
constexpr int kMinSplashMs = 3000;
// How often the panel's settings and the whole picture are sent again, so
// a transfer glitch never stays on screen for long.
constexpr int kRefreshMs = 2000;
// The picture is compared and sent in tiles of this size.
constexpr int kTile = 16;
// Size the gear pictograms are rendered at; drawn smaller.
constexpr int kGlyphSize = 48;
// Level that counts as clipping, and how long the clip light stays on.
constexpr float kClipDb = -0.5f;
// Above this level the meter bar turns orange.
constexpr float kHotDb = -3.0f;
constexpr juce::uint32 kClipHoldMs = 5000;

const juce::Colour kBackground{0xff000000};
const juce::Colour kAccent{0xffffa000};
const juce::Colour kText{0xffffffff};
const juce::Colour kDim{0xff808080};
const juce::Colour kTrack{0xff303030};
const juce::Colour kMeter{0xff30c060};
const juce::Colour kWarn{0xffff4040};
const juce::Colour kEdit{0xff40c8ff};
const juce::Colour kEditFill{0xff0a2a3a};

juce::Font font(float height, bool bold = false) {
  return juce::Font(juce::FontOptions(height, bold ? juce::Font::bold : juce::Font::plain));
}

// The plugin's pictogram for a gear id, drawn white on transparent.
juce::Image renderGlyph(const char* svg, int size) {
  juce::Image image(juce::Image::ARGB, size, size, true, juce::SoftwareImageType());
  if (const auto drawable = juce::Drawable::createFromSVGString(svg)) {
    juce::Graphics g(image);
    drawable->drawWithin(g, image.getBounds().toFloat(), juce::RectanglePlacement::centred, 1.0f);
  }
  return image;
}

float meterDb(const juce::var& levels, const char* key) {
  const auto pair = levels[key];
  return juce::jmax(static_cast<float>(pair[0]), static_cast<float>(pair[1]));
}

}  // namespace

DisplayUi::DisplayUi(TONE3000Processor& p, juce::AudioDeviceManager& dm, const UiState& s)
    : juce::Thread("display"), processor(p), deviceManager(dm), state(s) {
  for (const char* gear : {"amp", "amp-cab", "pedal", "outboard", "cab", "space", "experimental", "ir"})
    glyphs[gear] = renderGlyph(ui::gear::svgFor(gear), kGlyphSize);
  glyphs["file"] = renderGlyph(ui::gear::kFileIcon, kGlyphSize);
  glyphs["settings"] = renderGlyph(ui::lucideSvg(ui::Icon::Settings), kGlyphSize);
  glyphs["save"] = renderGlyph(ui::lucideSvg(ui::Icon::Save), kGlyphSize);
}

const juce::Image& DisplayUi::glyphFor(const juce::String& gear) const {
  const auto found = glyphs.find(gear == "full-rig" ? juce::String("amp-cab") : gear);
  return found != glyphs.end() ? found->second : glyphs.at("amp");
}

DisplayUi::~DisplayUi() {
  stopThread(1000);
}

juce::String DisplayUi::start(const LcdConfig& config) {
  if (!lcd.open(config))
    return lcd.error();
  frameW = lcd.width();
  frameH = lcd.height();
  shown.assign(static_cast<size_t>(lcd.width()) * static_cast<size_t>(lcd.height()), 0);
  pending = shown;
  startThread(juce::Thread::Priority::low);
  return {};
}

juce::Image DisplayUi::renderPreview(int width, int height) {
  frameW = width;
  frameH = height;
  juce::Image frame(juce::Image::RGB, width, height, true, juce::SoftwareImageType());
  juce::Graphics g(frame);
  g.fillAll(kBackground);
  drawMain(g);
  return frame;
}

void DisplayUi::setTitle(const juce::String& text) {
  const juce::ScopedLock lock(titleLock);
  title = text;
}

void DisplayUi::setStatus(const juce::String& text) {
  const juce::ScopedLock lock(titleLock);
  status = text;
}

void DisplayUi::run() {
  juce::Image frame(juce::Image::RGB, lcd.width(), lcd.height(), true, juce::SoftwareImageType());
  const auto startMs = juce::Time::getMillisecondCounter();
  bool backlightOn = false;
  auto lastRefreshMs = startMs;

  while (!threadShouldExit()) {
    const bool splash = !mainRequested.load() || juce::Time::getMillisecondCounter() - startMs < kMinSplashMs;
    {
      juce::Graphics g(frame);
      g.fillAll(kBackground);
      if (splash)
        drawSplash(g);
      else
        drawMain(g);
    }
    if (juce::Time::getMillisecondCounter() - lastRefreshMs >= kRefreshMs) {
      lastRefreshMs = juce::Time::getMillisecondCounter();
      lcd.reconfigure();
      firstFrame = true;  // forces a full redraw
    }
    present(frame);
    if (!backlightOn) {  // only once there is a picture to show
      lcd.setBacklight(true);
      backlightOn = true;
    }
    wait(kFrameMs);
  }
}

void DisplayUi::drawSplash(juce::Graphics& g) const {
  const auto area = juce::Rectangle<int>(frameW, frameH);
  g.setColour(kAccent);
  g.setFont(font(46.0f, true));
  g.drawText("AMPHOST", area.withTrimmedBottom(40), juce::Justification::centred);
  juce::String line;
  {
    const juce::ScopedLock lock(titleLock);
    line = status;
  }
  g.setColour(kDim);
  g.setFont(font(16.0f));
  g.drawText(line, area.withTrimmedTop(area.getHeight() / 2 + 20), juce::Justification::centredTop);
  g.setFont(font(12.0f));
  g.drawText(juce::String::fromUTF8("v" T3K_HOST_VERSION "  \xc2\xb7  plugin " T3K_PLUGIN_REF), area.withTrimmedBottom(6),
             juce::Justification::centredBottom);
}

void DisplayUi::drawMain(juce::Graphics& g) {
  const int w = frameW;
  const auto levels = processor.getMeterLevels();
  auto& params = processor.getParameters();
  const auto param = [&params](const char* id) {
    const auto* raw = params.getRawParameterValue(id);
    return raw != nullptr ? raw->load() : 0.0f;
  };

  const auto page = state.currentPage();
  const auto& def = pageDef(page);
  const bool onPage = page != Page::Overview;
  const auto chain = state.getChain();
  const auto tones = chain.tones();
  const bool cursorOn = state.active.load();
  const bool editing = state.editing.load();
  const bool knobsInPath = state.knobsInPath.load();
  const int fineSlots = state.fineSlots.load();
  const auto targets = cursorTargets(page, static_cast<int>(tones.size()), knobsInPath);
  const int selected = state.selected.load();
  const bool hasTarget = cursorOn && juce::isPositiveAndBelow(selected, static_cast<int>(targets.size()));
  const Target target = hasTarget ? targets[static_cast<size_t>(selected)] : Target{TargetKind::Head, -1};
  const auto isSelected = [&](TargetKind kind, int index) {
    return hasTarget && target.kind == kind && target.index == index;
  };

  // Cursor: white frame on the selected item, blue once it is being edited.
  const auto cursor = [&](TargetKind kind, int index, juce::Rectangle<float> box) {
    if (!isSelected(kind, index))
      return;
    if (editing) {
      g.setColour(kEditFill);
      g.fillRoundedRectangle(box, 6.0f);
    }
    g.setColour(editing ? kEdit : kText);
    g.drawRoundedRectangle(box.reduced(1.0f), 6.0f, 2.0f);
  };

  // Layout, top to bottom: preset name with the buttons, the effect
  // switches, the stage (the chain grid, or a page's entries) between the
  // level meters, the context line, and the knob row along the bottom edge
  // where the value encoders sit, five equal slots.
  const int gap = 4;
  const int titleTop = 2;
  const int titleH = 26;
  const int switchTop = titleTop + titleH + gap;
  const int switchH = 20;
  const int stageTop = switchTop + switchH + gap;
  const int stageH = 88;
  const int contextTop = stageTop + stageH + 2;
  const int contextH = 14;
  const int knobTop = contextTop + contextH + 3;
  const int knobH = frameH - knobTop;

  // Preset name left, with a star while it has unsaved changes; save,
  // tuner and settings buttons right.
  juce::String name;
  {
    const juce::ScopedLock lock(titleLock);
    name = title;
  }
  int xruns = 0;
  if (auto* dev = deviceManager.getCurrentAudioDevice())
    xruns = dev->getXRunCount();
  const bool tunerOn = state.tunerOn.load();
  const juce::Rectangle<int> settingsBox(w - 2 - titleH, titleTop, titleH, titleH);
  const juce::Rectangle<int> tunerBox(settingsBox.getX() - gap - titleH, titleTop, titleH, titleH);
  const juce::Rectangle<int> saveBox(tunerBox.getX() - gap - titleH, titleTop, titleH, titleH);
  const bool dirty = state.presetDirty.load();
  const int titleW = saveBox.getX() - gap - 2;
  const int starW = 16;
  cursor(TargetKind::Head, static_cast<int>(HeadItem::Preset), juce::Rectangle<int>(2, titleTop, titleW, titleH).toFloat());
  const auto titleFont = font(18.0f, true);
  g.setColour(kText);
  g.setFont(titleFont);
  g.drawText(name, 8, titleTop, titleW - 10 - starW, titleH, juce::Justification::centredLeft, true);
  if (dirty) {
    juce::GlyphArrangement measure;
    measure.addLineOfText(titleFont, name, 0.0f, 0.0f);
    const int nameW = juce::roundToInt(measure.getBoundingBox(0, -1, true).getWidth());
    g.setColour(kAccent);
    g.drawText("*", 8 + juce::jmin(nameW + 3, titleW - 10 - starW), titleTop, starW, titleH, juce::Justification::centredLeft);
  }

  // Save button: lit while there is something to save.
  cursor(TargetKind::Head, static_cast<int>(HeadItem::Save), saveBox.toFloat());
  g.setColour(dirty ? kAccent : kDim);
  g.drawImage(glyphs.at("save"), saveBox.toFloat().reduced(5.0f), juce::RectanglePlacement::centred, true);

  // Tuner button: a tuning fork. Tuner and settings rest while a page is open.
  const float headAlpha = onPage ? 0.35f : 1.0f;
  cursor(TargetKind::Head, static_cast<int>(HeadItem::Tuner), tunerBox.toFloat());
  {
    const auto c = tunerBox.toFloat().getCentre();
    juce::Path fork;
    fork.startNewSubPath(c.x - 4.0f, c.y - 8.0f);
    fork.lineTo(c.x - 4.0f, c.y - 2.0f);
    fork.quadraticTo(c.x - 4.0f, c.y + 2.0f, c.x, c.y + 2.0f);
    fork.quadraticTo(c.x + 4.0f, c.y + 2.0f, c.x + 4.0f, c.y - 2.0f);
    fork.lineTo(c.x + 4.0f, c.y - 8.0f);
    fork.startNewSubPath(c.x, c.y + 2.0f);
    fork.lineTo(c.x, c.y + 8.0f);
    g.setColour((tunerOn ? kAccent : kDim).withMultipliedAlpha(headAlpha));
    g.strokePath(fork, juce::PathStrokeType(2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
  }

  // Settings button.
  cursor(TargetKind::Head, static_cast<int>(HeadItem::Settings), settingsBox.toFloat());
  g.setColour(kDim.withMultipliedAlpha(headAlpha));
  g.drawImage(glyphs.at("settings"), settingsBox.toFloat().reduced(5.0f), juce::RectanglePlacement::centred, true);

  // Effect switches under the top line: lit when on. On a page, its own
  // switch is framed: that is where the page belongs.
  const float semitones = param("pitchSemitones");
  const bool stepped = param("pitchStep") > 0.5f;
  const juce::String shift = stepped ? juce::String(juce::roundToInt(semitones)) : juce::String(semitones, 1);
  const juce::String pitchText = semitones > 0.0f && shift != "0" ? "+" + shift : shift;
  const bool switchOn[kSwitches] = {param("gateEnabled") > 0.5f, param("pitchEnabled") > 0.5f,
                                    param("spreadEnabled") > 0.5f};
  const juce::String switchText[kSwitches] = {
      switchOn[0] ? "GATE " + juce::String(juce::roundToInt(param("gateThreshold"))) : juce::String("GATE OFF"),
      switchOn[1] ? "PITCH " + pitchText : juce::String("PITCH OFF"),
      "SPREAD",
  };
  {
    const int badgeGap = 2 * gap;
    const int badgeW = (w - 8 - 2 * badgeGap) / kSwitches;
    g.setFont(font(12.0f, true));
    for (int i = 0; i < kSwitches; ++i) {
      const auto box = juce::Rectangle<int>(4 + i * (badgeW + badgeGap), switchTop, badgeW, switchH).toFloat();
      cursor(TargetKind::Switch, i, box.expanded(2.0f));
      if (switchOn[i]) {
        g.setColour(kAccent);
        g.fillRoundedRectangle(box, 5.0f);
        g.setColour(kBackground);
      } else {
        g.setColour(kTrack);
        g.drawRoundedRectangle(box.reduced(0.5f), 5.0f, 1.0f);
        g.setColour(kDim);
      }
      g.drawText(switchText[i], box, juce::Justification::centred);
      if (onPage && kSwitchPages[i] == page) {
        g.setColour(kEdit);
        g.drawRoundedRectangle(box.expanded(2.0f).reduced(1.0f), 6.0f, 2.0f);
      }
    }
  }

  // Input level far left, output level far right, -60..0 dB, filling
  // upwards, each with a clip light on top that stays on for a moment.
  // They run the height of the stage and the context line together.
  const int meterW = 10;
  const int meterMargin = 5;
  const int clipSize = 8;
  const int labelH = 13;
  const int meterBottom = contextTop + contextH;
  const char* const meterKeys[] = {"input", "output"};
  const char* const meterNames[] = {"I", "O"};
  const auto nowMs = juce::Time::getMillisecondCounter();
  for (int i = 0; i < 2; ++i) {
    const int x = i == 0 ? meterMargin : w - meterMargin - meterW;
    const int barTop = stageTop + clipSize + 6;
    const juce::Rectangle<int> bar(x, barTop, meterW, meterBottom - labelH - barTop);
    const float db = meterDb(levels, meterKeys[i]);
    const float pos = juce::jlimit(0.0f, 1.0f, (db + 60.0f) / 60.0f);
    g.setColour(kTrack);
    g.fillRect(bar);
    g.setColour(db > kHotDb ? kAccent : kMeter);
    g.fillRect(bar.withTop(bar.getBottom() - juce::roundToInt(static_cast<float>(bar.getHeight()) * pos)));

    // On the gate's page the input meter shows where the threshold sits.
    if (i == 0 && page == Page::Gate) {
      const float threshold = juce::jlimit(0.0f, 1.0f, (param("gateThreshold") + 60.0f) / 60.0f);
      const int y = bar.getBottom() - juce::roundToInt(static_cast<float>(bar.getHeight()) * threshold);
      g.setColour(kEdit);
      g.fillRect(x - 3, y - 1, meterW + 6, 3);
    }

    if (db >= kClipDb)
      clipUntilMs[i] = nowMs + kClipHoldMs;
    if (static_cast<juce::int32>(clipUntilMs[i] - nowMs) > 0) {
      g.setColour(kWarn);
      g.fillEllipse(static_cast<float>(x + (meterW - clipSize) / 2), static_cast<float>(stageTop),
                    static_cast<float>(clipSize), static_cast<float>(clipSize));
    }

    g.setColour(kDim);
    g.setFont(font(12.0f, true));
    g.drawText(meterNames[i], x - 4, meterBottom - labelH, meterW + 8, labelH, juce::Justification::centred);
  }

  const int stageLeft = meterMargin + meterW + 8;
  const juce::Rectangle<int> stage(stageLeft, stageTop, w - 2 * stageLeft, stageH);
  juce::String tunerText;

  if (onPage) {
    // A page: its title, then one line per entry with the value right.
    g.setColour(kText);
    g.setFont(font(15.0f, true));
    g.drawText(def.title, stage.withHeight(18).withTrimmedLeft(6), juce::Justification::centredLeft, true);
    const int rowH = 16;
    g.setFont(font(13.0f));
    for (int i = 0; i < static_cast<int>(def.entries.size()); ++i) {
      const auto& entry = def.entries[static_cast<size_t>(i)];
      const auto row = juce::Rectangle<int>(stage.getX(), stage.getY() + 20 + i * (rowH + 1), stage.getWidth(), rowH);
      cursor(TargetKind::Entry, i, row.toFloat());
      auto* p = params.getParameter(entry.paramId);
      juce::String value;
      bool lit = false;
      if (auto* choice = dynamic_cast<juce::AudioParameterChoice*>(p)) {
        value = juce::String::fromUTF8("\xe2\x80\xb9 ") + choice->getCurrentChoiceName() + juce::String::fromUTF8(" \xe2\x80\xba");
      } else if (p != nullptr) {
        lit = p->getValue() > 0.5f;
        value = lit ? "On" : "Off";
      }
      g.setColour(kText);
      g.drawText(entry.label, row.withTrimmedLeft(8), juce::Justification::centredLeft, true);
      g.setColour(lit ? kAccent : kDim);
      g.drawText(value, row.withTrimmedRight(8), juce::Justification::centredRight, true);
    }
  } else if (tunerOn) {
    // While the tuner is on it takes the grid's place.
    const auto zone = stage;
    const float hz = state.tunerHz.load();
    const int barY = zone.getBottom() - 12;
    const int half = zone.getWidth() / 2 - 8;
    g.setColour(kTrack);
    g.fillRect(zone.getCentreX() - half, barY, 2 * half, 4);
    g.setColour(kDim);
    g.fillRect(zone.getCentreX() - 1, barY - 6, 2, 16);
    if (hz > 0.0f) {
      static const char* const kNotes[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
      const float midi = 69.0f + 12.0f * std::log2(hz / 440.0f);
      const int nearest = juce::roundToInt(midi);
      const float cents = (midi - static_cast<float>(nearest)) * 100.0f;
      const bool inTune = std::abs(cents) <= 5.0f;
      const juce::String note = juce::String(kNotes[((nearest % 12) + 12) % 12]) + juce::String(nearest / 12 - 1);
      g.setColour(inTune ? kMeter : kText);
      g.setFont(font(40.0f, true));
      g.drawText(note, zone.withTrimmedBottom(20), juce::Justification::centred);
      // The marker's distance from the centre is the deviation, 50 cents at either end.
      const int markerX = zone.getCentreX() + juce::roundToInt(juce::jlimit(-1.0f, 1.0f, cents / 50.0f) * static_cast<float>(half));
      g.setColour(inTune ? kMeter : kAccent);
      g.fillRect(markerX - 3, barY - 6, 6, 16);
      tunerText = juce::String(hz, 1) + " Hz   " + (cents >= 0.0f ? "+" : "") + juce::String(juce::roundToInt(cents)) + " cent";
    } else {
      g.setColour(kDim);
      g.setFont(font(40.0f, true));
      g.drawText("--", zone.withTrimmedBottom(20), juce::Justification::centred);
      tunerText = "Play a single string";
    }
  } else {
    // The chain as a grid between the meters: one row in mono, two in
    // stereo, at least five slots wide, each block shown by its gear
    // pictogram. Blocks that are on are filled, blocks that are off only
    // outlined, empty slots faint.
    const int gridRows = chain.stereo ? 2 : 1;
    const int gridH = juce::jmin(stageH, chain.stereo ? stageH : 64);
    const int rowH = (gridH - (gridRows - 1) * gap) / gridRows;
    const int rowsTop = stageTop + (stageH - gridRows * rowH - (gridRows - 1) * gap) / 2;
    int slots = 5;
    for (int row = 0; row < gridRows; ++row)
      slots = juce::jmax(slots, chain.offset[row] + static_cast<int>(chain.rows[row].size()));
    const int slotW = (stage.getWidth() - (slots - 1) * gap) / slots;
    int toneIndex = 0;
    for (int row = 0; row < gridRows; ++row) {
      const auto& cells = chain.rows[row];
      for (int i = 0; i < static_cast<int>(cells.size()); ++i) {
        const auto& cell = cells[static_cast<size_t>(i)];
        const auto box =
            juce::Rectangle<int>(stageLeft + (chain.offset[row] + i) * (slotW + gap), rowsTop + row * (rowH + gap), slotW, rowH)
                .toFloat();
        if (cell.insert) {
          g.setColour(kTrack.withAlpha(0.6f));
          g.drawRoundedRectangle(box.reduced(0.5f), 4.0f, 1.0f);
          continue;
        }
        const bool blockSelected = isSelected(TargetKind::Block, toneIndex);
        ++toneIndex;
        g.setColour(kTrack);
        if (cell.enabled)
          g.fillRoundedRectangle(box, 4.0f);
        else
          g.drawRoundedRectangle(box.reduced(0.5f), 4.0f, 1.0f);
        if (blockSelected) {
          g.setColour(kText);
          g.drawRoundedRectangle(box.reduced(1.0f), 4.0f, 2.0f);
        }
        g.setColour(!cell.enabled || cell.loading ? kDim : kAccent);
        g.drawImage(glyphFor(cell.gear), box.reduced(4.0f), juce::RectanglePlacement::centred, true);
      }
    }
  }

  // The knob row: five equal slots along the bottom edge, one above each
  // value encoder. Empty slots stay empty; their encoder does nothing here.
  const float slotW = static_cast<float>(w) / static_cast<float>(kKnobSlots);
  const float radius = 20.0f;
  const float cy = static_cast<float>(knobTop) + 4.0f + radius;
  const float sweep = juce::MathConstants<float>::pi * 0.75f;
  g.setColour(kTrack);
  g.fillRect(0, knobTop - 2, w, 1);
  for (int i = 1; i < kKnobSlots; ++i)
    g.fillRect(juce::roundToInt(slotW * static_cast<float>(i)), knobTop + 4, 1, knobH - 10);
  for (int i = 0; i < static_cast<int>(def.knobs.size()) && i < kKnobSlots; ++i) {
    const auto& k = def.knobs[static_cast<size_t>(i)];
    const float cx = slotW * (static_cast<float>(i) + 0.5f);
    const float pos = knobPosition(k, params);
    cursor(TargetKind::Knob, i,
           juce::Rectangle<float>(cx - slotW * 0.5f + 1.0f, static_cast<float>(knobTop), slotW - 2.0f,
                                  static_cast<float>(knobH - 1)));

    juce::Path track;
    track.addCentredArc(cx, cy, radius, radius, 0.0f, -sweep, sweep, true);
    g.setColour(kTrack);
    g.strokePath(track, juce::PathStrokeType(4.0f));
    juce::Path arc;
    arc.addCentredArc(cx, cy, radius, radius, 0.0f, -sweep, -sweep + 2.0f * sweep * pos, true);
    g.setColour(kAccent);
    g.strokePath(arc, juce::PathStrokeType(4.0f));

    const auto cell = juce::Rectangle<float>(cx - slotW * 0.5f, cy - radius, slotW, radius * 2.0f);
    const auto text = formatKnob(k, params, false);
    g.setColour(kText);
    g.setFont(font(text.length() > 4 ? 12.0f : 14.0f, true));
    g.drawText(text, cell.reduced(radius * 0.25f, 0.0f), juce::Justification::centred, true);
    g.setColour(kDim);
    g.setFont(font(11.0f));
    g.drawText(k.label, cell.translated(0.0f, radius * 2.0f + 1.0f).withHeight(14.0f), juce::Justification::centred);
    if ((fineSlots & (1 << i)) != 0) {
      g.setColour(kAccent);
      g.setFont(font(11.0f, true));
      g.drawText("FINE", juce::Rectangle<float>(cx - slotW * 0.5f, static_cast<float>(knobTop), slotW - 4.0f, 12.0f),
                 juce::Justification::centredRight);
    }
  }

  // Context line: what the cursor is on, in plain words.
  juce::String context;
  if (hasTarget) {
    switch (target.kind) {
      case TargetKind::Head:
        switch (static_cast<HeadItem>(target.index)) {
          case HeadItem::Save:
            context = dirty ? "Save changes to " + name : juce::String("No changes to save");
            break;
          case HeadItem::Tuner: context = tunerOn ? tunerText : juce::String("Tuner"); break;
          case HeadItem::Settings:
            context = "Settings   CPU " + juce::String(juce::roundToInt(static_cast<double>(levels["cpu"]) * 100.0)) +
                      " %   " + juce::String(xruns) + " dropouts";
            break;
          case HeadItem::Preset: {
            const int index = state.presetIndex.load();
            const int count = state.presetCount.load();
            context = index >= 0 ? "Preset " + juce::String(index + 1) + " of " + juce::String(count)
                                 : juce::String(count) + " presets";
            break;
          }
        }
        break;
      case TargetKind::Switch: {
        const auto& switchPage = pageDef(kSwitchPages[target.index]);
        context = juce::String(switchPage.title) + (switchOn[target.index] ? " on" : " off") + "  -  hold for settings";
        break;
      }
      case TargetKind::Block:
        if (juce::isPositiveAndBelow(target.index, static_cast<int>(tones.size())))
          context = tones[static_cast<size_t>(target.index)]->name;
        break;
      case TargetKind::Entry: {
        const auto& entry = def.entries[static_cast<size_t>(target.index)];
        const bool choice = dynamic_cast<juce::AudioParameterChoice*>(params.getParameter(entry.paramId)) != nullptr;
        context = editing ? juce::String("Turn to choose, press when done")
                          : juce::String(entry.label) + (choice ? "  -  press to change" : "  -  press to switch") +
                                ", hold to go back";
        break;
      }
      case TargetKind::Knob: {
        const auto& k = def.knobs[static_cast<size_t>(target.index)];
        context = juce::String(k.name) + " " + formatKnob(k, params, true) + (editing ? "  -  hold for default" : "");
        break;
      }
    }
  }
  g.setColour(kText);
  g.setFont(font(12.0f));
  g.drawText(context, stageLeft, contextTop, w - 2 * stageLeft, contextH, juce::Justification::centred, true);
}

// Converts the frame to RGB565 and sends only the parts that changed: the
// picture is compared in tiles, and neighbouring changed tiles are sent
// together as one rectangle.
void DisplayUi::present(const juce::Image& frame) {
  const int w = lcd.width();
  const int h = lcd.height();
  const int cols = (w + kTile - 1) / kTile;
  const int rows = (h + kTile - 1) / kTile;
  std::vector<uint8_t> dirty(static_cast<size_t>(cols) * static_cast<size_t>(rows), firstFrame ? 1 : 0);

  const juce::Image::BitmapData bitmap(frame, juce::Image::BitmapData::readOnly);
  for (int y = 0; y < h; ++y) {
    const auto* src = bitmap.getLinePointer(y);
    const size_t line = static_cast<size_t>(y) * static_cast<size_t>(w);
    auto* tileRow = dirty.data() + static_cast<size_t>(y / kTile) * static_cast<size_t>(cols);
    for (int x = 0; x < w; ++x) {
      const auto* px = reinterpret_cast<const juce::PixelRGB*>(src + x * bitmap.pixelStride);
      const uint16_t c = SpiLcd::rgb(px->getRed(), px->getGreen(), px->getBlue());
      if (c != shown[line + static_cast<size_t>(x)])
        tileRow[x / kTile] = 1;
      pending[line + static_cast<size_t>(x)] = c;
    }
  }

  // A run of changed tiles in one row, grown downwards while the rows below
  // have the same run.
  bool ok = true;
  for (int row = 0; row < rows && ok; ++row) {
    for (int col = 0; col < cols && ok; ++col) {
      if (!dirty[static_cast<size_t>(row * cols + col)])
        continue;
      int endCol = col;
      while (endCol < cols && dirty[static_cast<size_t>(row * cols + endCol)])
        ++endCol;
      int endRow = row + 1;
      const auto sameRun = [&](int r) {
        for (int c = col; c < endCol; ++c)
          if (!dirty[static_cast<size_t>(r * cols + c)])
            return false;
        return true;
      };
      while (endRow < rows && sameRun(endRow))
        ++endRow;
      for (int r = row; r < endRow; ++r)
        for (int c = col; c < endCol; ++c)
          dirty[static_cast<size_t>(r * cols + c)] = 0;

      const int x = col * kTile;
      const int y = row * kTile;
      const int blockW = juce::jmin(w, endCol * kTile) - x;
      const int blockH = juce::jmin(h, endRow * kTile) - y;
      ok = lcd.blit(x, y, blockW, blockH, pending.data() + static_cast<size_t>(y) * static_cast<size_t>(w) + x, w);
      col = endCol - 1;
    }
  }

  if (ok)
    shown = pending;
  firstFrame = !ok;  // after a failed transfer, send everything again
}

}  // namespace t3k

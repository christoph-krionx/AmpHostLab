#include "DisplayUi.h"

#include "Processor.h"
#include "core/GearGlyphs.h"

namespace t3k {

namespace {

constexpr int kFrameMs = 50;
constexpr int kMinSplashMs = 1500;
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

struct Knob {
  UiItem item;
  const char* label;
  const char* paramId;
  float scale;  // parameter value -> 0..10 readout
};
constexpr Knob kKnobs[] = {
    {UiItem::Gain, "GAIN", "inputLevel", 10.0f},     {UiItem::Bass, "BASS", "toneBass", 1.0f},
    {UiItem::Mid, "MID", "toneMid", 1.0f},           {UiItem::Treble, "TREBLE", "toneTreble", 1.0f},
    {UiItem::Volume, "VOLUME", "outputLevel", 10.0f},
};

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
  shown.assign(static_cast<size_t>(lcd.width()) * static_cast<size_t>(lcd.height()), 0);
  pending = shown;
  startThread(juce::Thread::Priority::low);
  return {};
}

void DisplayUi::setTitle(const juce::String& text) {
  const juce::ScopedLock lock(titleLock);
  title = text;
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
  const auto area = juce::Rectangle<int>(lcd.width(), lcd.height());
  g.setColour(kAccent);
  g.setFont(font(46.0f, true));
  g.drawText("AMPHOST", area.withTrimmedBottom(40), juce::Justification::centred);
  g.setColour(kDim);
  g.setFont(font(16.0f));
  g.drawText("starting", area.withTrimmedTop(area.getHeight() / 2 + 20), juce::Justification::centredTop);
}

void DisplayUi::drawMain(juce::Graphics& g) {
  const int w = lcd.width();
  const int h = lcd.height();
  const auto levels = processor.getMeterLevels();

  // Cursor: white frame on the selected item, blue once it is being edited.
  const bool cursorOn = state.active.load();
  const auto chain = state.getChain();
  const auto target = cursorTarget(state.selected.load(), static_cast<int>(chain.tones().size()));
  const bool editing = state.editing.load();
  const auto cursor = [&](UiItem item, juce::Rectangle<float> box) {
    if (!cursorOn || target.isBlock || target.item != item)
      return;
    if (editing) {
      g.setColour(kEditFill);
      g.fillRoundedRectangle(box, 6.0f);
    }
    g.setColour(editing ? kEdit : kText);
    g.drawRoundedRectangle(box.reduced(1.0f), 6.0f, 2.0f);
  };

  // Layout, top to bottom, like the plugin: preset name, the chain grid with
  // a level meter on each side, the selected block's name, the knobs, the
  // effect switches. The same gap separates every band.
  const int gap = 5;
  const int titleTop = 2;
  const int titleH = 30;
  const int gridTop = titleTop + titleH + gap;
  const int gridH = 72;
  const int nameTop = gridTop + gridH + gap;
  const int nameH = 13;
  const int knobTop = nameTop + nameH + gap;
  const int knobH = 76;
  const int switchTop = knobTop + knobH + gap;
  const int switchH = 24;

  // Preset name left, audio load right (red once the device dropped out).
  juce::String name;
  {
    const juce::ScopedLock lock(titleLock);
    name = title;
  }
  int xruns = 0;
  if (auto* dev = deviceManager.getCurrentAudioDevice())
    xruns = dev->getXRunCount();
  cursor(UiItem::Preset, juce::Rectangle<int>(2, titleTop, w - 76, titleH).toFloat());
  g.setColour(kText);
  g.setFont(font(20.0f, true));
  g.drawText(name, 8, titleTop, w - 86, titleH, juce::Justification::centredLeft, true);
  g.setColour(xruns > 0 ? kWarn : kDim);
  g.setFont(font(14.0f));
  g.drawText(juce::String(juce::roundToInt(static_cast<double>(levels["cpu"]) * 100.0)) + " %",
             w - 70, titleTop, 62, titleH, juce::Justification::centredRight);

  // Input level far left, output level far right, -60..0 dB, filling
  // upwards, each with a clip light on top that stays on for a moment.
  // They run the height of the grid and the name line together.
  const int meterW = 10;
  const int meterMargin = 6;
  const int clipSize = 8;
  const int labelH = 13;
  const int meterBottom = nameTop + nameH;
  const char* const meterKeys[] = {"input", "output"};
  const char* const meterNames[] = {"I", "O"};
  const auto nowMs = juce::Time::getMillisecondCounter();
  for (int i = 0; i < 2; ++i) {
    const int x = i == 0 ? meterMargin : w - meterMargin - meterW;
    const int barTop = gridTop + clipSize + 8;
    const juce::Rectangle<int> bar(x, barTop, meterW, meterBottom - labelH - barTop);
    const float db = meterDb(levels, meterKeys[i]);
    const float pos = juce::jlimit(0.0f, 1.0f, (db + 60.0f) / 60.0f);
    g.setColour(kTrack);
    g.fillRect(bar);
    g.setColour(db > kHotDb ? kAccent : kMeter);
    g.fillRect(bar.withTop(bar.getBottom() - juce::roundToInt(static_cast<float>(bar.getHeight()) * pos)));

    if (db >= kClipDb)
      clipUntilMs[i] = nowMs + kClipHoldMs;
    if (static_cast<juce::int32>(clipUntilMs[i] - nowMs) > 0) {
      g.setColour(kWarn);
      g.fillEllipse(static_cast<float>(x + (meterW - clipSize) / 2), static_cast<float>(gridTop),
                    static_cast<float>(clipSize), static_cast<float>(clipSize));
    }

    g.setColour(kDim);
    g.setFont(font(12.0f, true));
    g.drawText(meterNames[i], x - 4, meterBottom - labelH, meterW + 8, labelH, juce::Justification::centred);
  }

  // The chain as a grid between the meters: one row in mono, two in stereo,
  // at least five slots wide, each block shown by its gear pictogram.
  // Blocks that are on are filled, blocks that are off only outlined,
  // empty slots faint.
  const int gridLeft = meterMargin + meterW + 12;
  const int gridRows = chain.stereo ? 2 : 1;
  const int rowH = juce::jmin(44, (gridH - (gridRows - 1) * gap) / gridRows);
  const int rowsTop = gridTop + (gridH - gridRows * rowH - (gridRows - 1) * gap) / 2;
  int slots = 5;
  for (int row = 0; row < gridRows; ++row)
    slots = juce::jmax(slots, chain.offset[row] + static_cast<int>(chain.rows[row].size()));
  const int slotW = (w - 2 * gridLeft - (slots - 1) * gap) / slots;
  int toneIndex = 0;
  juce::String selectedBlockName;
  for (int row = 0; row < gridRows; ++row) {
    const auto& cells = chain.rows[row];
    for (int i = 0; i < static_cast<int>(cells.size()); ++i) {
      const auto& cell = cells[static_cast<size_t>(i)];
      const auto box =
          juce::Rectangle<int>(gridLeft + (chain.offset[row] + i) * (slotW + gap), rowsTop + row * (rowH + gap), slotW, rowH)
              .toFloat();
      if (cell.insert) {
        g.setColour(kTrack.withAlpha(0.6f));
        g.drawRoundedRectangle(box.reduced(0.5f), 4.0f, 1.0f);
        continue;
      }
      const bool isSelected = cursorOn && target.isBlock && target.block == toneIndex;
      ++toneIndex;
      g.setColour(kTrack);
      if (cell.enabled)
        g.fillRoundedRectangle(box, 4.0f);
      else
        g.drawRoundedRectangle(box.reduced(0.5f), 4.0f, 1.0f);
      if (isSelected) {
        selectedBlockName = cell.name;
        g.setColour(kText);
        g.drawRoundedRectangle(box.reduced(1.0f), 4.0f, 2.0f);
      }
      g.setColour(!cell.enabled || cell.loading ? kDim : kAccent);
      g.drawImage(glyphFor(cell.gear), box.reduced(4.0f), juce::RectanglePlacement::centred, true);
    }
  }

  // Five knobs.
  const float cellW = static_cast<float>(w) / static_cast<float>(std::size(kKnobs));
  const float radius = juce::jmin(24.0f, cellW * 0.38f);
  const float cy = static_cast<float>(knobTop) + 7.0f + radius;
  const float sweep = juce::MathConstants<float>::pi * 0.75f;
  for (size_t i = 0; i < std::size(kKnobs); ++i) {
    const auto& k = kKnobs[i];
    const float cx = cellW * (static_cast<float>(i) + 0.5f);
    const auto* raw = processor.getParameters().getRawParameterValue(k.paramId);
    const float value = raw != nullptr ? raw->load() * k.scale : 0.0f;
    const float pos = juce::jlimit(0.0f, 1.0f, value / 10.0f);
    cursor(k.item, juce::Rectangle<float>(cx - cellW * 0.5f + 1.0f, static_cast<float>(knobTop), cellW - 2.0f,
                                          static_cast<float>(knobH)));

    juce::Path track;
    track.addCentredArc(cx, cy, radius, radius, 0.0f, -sweep, sweep, true);
    g.setColour(kTrack);
    g.strokePath(track, juce::PathStrokeType(5.0f));
    juce::Path arc;
    arc.addCentredArc(cx, cy, radius, radius, 0.0f, -sweep, -sweep + 2.0f * sweep * pos, true);
    g.setColour(kAccent);
    g.strokePath(arc, juce::PathStrokeType(5.0f));

    const auto cell = juce::Rectangle<float>(cx - cellW * 0.5f, cy - radius, cellW, radius * 2.0f);
    g.setColour(kText);
    g.setFont(font(16.0f, true));
    g.drawText(juce::String(value, 1), cell, juce::Justification::centred);
    g.setColour(kDim);
    g.setFont(font(12.0f));
    g.drawText(k.label, cell.translated(0.0f, radius * 2.0f + 2.0f).withHeight(16.0f), juce::Justification::centred);
  }

  // Effect switches along the bottom: lit when on.
  const auto param = [this](const char* id) {
    const auto* raw = processor.getParameters().getRawParameterValue(id);
    return raw != nullptr ? raw->load() : 0.0f;
  };
  const float semitones = param("pitchSemitones");
  const bool stepped = param("pitchStep") > 0.5f;
  const juce::String shift = stepped ? juce::String(juce::roundToInt(semitones)) : juce::String(semitones, 1);
  const bool gateOn = param("gateEnabled") > 0.5f;
  const bool pitchOn = param("pitchEnabled") > 0.5f;
  const juce::String pitchText = semitones > 0.0f && shift != "0" ? "+" + shift : shift;
  const struct {
    UiItem item;
    juce::String text;
    bool on;
  } badges[] = {
      {UiItem::Gate, gateOn ? "GATE " + juce::String(juce::roundToInt(param("gateThreshold"))) : juce::String("GATE OFF"), gateOn},
      {UiItem::Pitch, pitchOn ? "PITCH " + pitchText : juce::String("PITCH OFF"), pitchOn},
      {UiItem::Spread, "SPREAD", param("spreadEnabled") > 0.5f},
  };

  // Context line under the grid: what the cursor is on, in plain words.
  juce::String context = selectedBlockName;
  if (cursorOn && !target.isBlock) {
    const auto item = target.item;
    if (item == UiItem::Preset) {
      const int index = state.presetIndex.load();
      const int count = state.presetCount.load();
      context = index >= 0 ? "Preset " + juce::String(index + 1) + " of " + juce::String(count)
                           : juce::String(count) + " presets";
    } else if (item == UiItem::Gate) {
      context = gateOn ? "Gate threshold " + juce::String(juce::roundToInt(param("gateThreshold"))) + " dB"
                       : juce::String("Gate off");
    } else if (item == UiItem::Pitch) {
      context = pitchOn ? "Pitch " + pitchText + " semitones" : juce::String("Pitch off");
    } else if (item == UiItem::Spread) {
      context = badges[2].on ? "Spread on" : "Spread off";
    } else {
      for (const auto& k : kKnobs)
        if (k.item == item) {
          const juce::String label(k.label);
          context = label.substring(0, 1) + label.substring(1).toLowerCase() + " " + juce::String(param(k.paramId) * k.scale, 1);
        }
    }
  }
  g.setColour(kText);
  g.setFont(font(12.0f));
  g.drawText(context, gridLeft, nameTop, w - 2 * gridLeft, nameH, juce::Justification::centred, true);

  const int badgeGap = 2 * gap;
  const int badgeW = (w - 16 - 2 * badgeGap) / 3;
  g.setFont(font(13.0f, true));
  for (int i = 0; i < 3; ++i) {
    const auto box = juce::Rectangle<int>(8 + i * (badgeW + badgeGap), switchTop, badgeW, switchH).toFloat();
    cursor(badges[i].item, box.expanded(2.0f));
    if (badges[i].on) {
      g.setColour(kAccent);
      g.fillRoundedRectangle(box, 5.0f);
      g.setColour(kBackground);
    } else {
      g.setColour(kTrack);
      g.drawRoundedRectangle(box.reduced(0.5f), 5.0f, 1.0f);
      g.setColour(kDim);
    }
    g.drawText(badges[i].text, box, juce::Justification::centred);
  }
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

#include "DisplayUi.h"

#include "Processor.h"
#include "core/GearGlyphs.h"
#include "core/LucideIcons.h"
#include "display/Logo.h"

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
// Level that counts as clipping: the number under the meter is inverted.
constexpr float kClipDb = -0.5f;
// Above this level the meter bar turns white.
constexpr float kHotDb = -3.0f;
// How long the level in numbers under a meter holds its peak.
constexpr juce::uint32 kPeakHoldMs = 3000;

// Greyscale for every state; one accent colour for navigation only: the
// cursor, and what changes while turning. Warnings are inverted.
const juce::Colour kBackground{0xff000000};
const juce::Colour kText{0xffffffff};
const juce::Colour kOn{0xffe6e6e6};      // switched on, values
const juce::Colour kDim{0xff8a8a8a};     // off, labels, the number in the top line
const juce::Colour kNumber{0xff6e6e6e};  // the idle view's preset number
const juce::Colour kMeterFill{0xff9a9a9a};
const juce::Colour kTrack{0xff303030};
juce::Colour gAccent{0xffc8501e};
juce::Colour gAccentFill{0xff2a1006};
juce::String gTypeface;
// A condensed typeface is set a little larger: its narrow letters leave
// the room, and small labels stay readable.
float gSizeScale{1.0f};

juce::Font font(float height, bool bold = false) {
  const int style = bold ? juce::Font::bold : juce::Font::plain;
  const float size = height * gSizeScale;
  return juce::Font(gTypeface.isEmpty() ? juce::FontOptions(size, style) : juce::FontOptions(gTypeface, size, style));
}

// White text on a white-filled box turned black: how warnings stand out
// without a second colour.
void drawInverted(juce::Graphics& g, const juce::String& text, juce::Rectangle<float> box) {
  g.setColour(kText);
  g.fillRoundedRectangle(box, 3.0f);
  g.setColour(kBackground);
  g.drawText(text, box, juce::Justification::centred, true);
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

void setDisplayLook(const juce::String& typeface, juce::Colour accent) {
  gTypeface = typeface;
  gSizeScale = typeface.containsIgnoreCase("condensed") ? 1.15f : 1.0f;
  gAccent = accent;
  gAccentFill = accent.interpolatedWith(kBackground, 0.8f);
}

DisplayUi::DisplayUi(TONE3000Processor& p, juce::AudioDeviceManager& dm, const UiState& s)
    : juce::Thread("display"), processor(p), deviceManager(dm), state(s) {
  for (const char* gear : {"amp", "amp-cab", "pedal", "outboard", "cab", "space", "experimental", "ir"})
    glyphs[gear] = renderGlyph(ui::gear::svgFor(gear), kGlyphSize);
  glyphs["file"] = renderGlyph(ui::gear::kFileIcon, kGlyphSize);
  glyphs["settings"] = renderGlyph(ui::lucideSvg(ui::Icon::Settings), kGlyphSize);
  glyphs["save"] = renderGlyph(ui::lucideSvg(ui::Icon::Save), kGlyphSize);
  logo = juce::Drawable::createFromSVGString(kLogoSvg);
}

const juce::Image& DisplayUi::glyphFor(const juce::String& gear) const {
  const auto found = glyphs.find(gear == "full-rig" ? juce::String("amp-cab") : gear);
  return found != glyphs.end() ? found->second : glyphs.at("amp");
}

DisplayUi::~DisplayUi() {
  stopThread(1000);
  // Dark while the host is not running; when the system shuts down, the
  // message about unplugging stays lit instead.
  if (!state.shuttingDown.load())
    lcd.setBacklight(false);
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

juce::Image DisplayUi::renderPreview(int width, int height, bool splash) {
  frameW = width;
  frameH = height;
  juce::Image frame(juce::Image::RGB, width, height, true, juce::SoftwareImageType());
  juce::Graphics g(frame);
  g.fillAll(kBackground);
  if (splash)
    drawSplash(g);
  else
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
  // The logo with the maker's name beside it, set like a nameplate:
  // GREYSCALE large, AUDIO small and widely spaced underneath. The group
  // is centred as a whole.
  const auto nameFont = font(40.0f, true);
  const auto subFont = font(14.0f, true).withExtraKerningFactor(0.7f);
  juce::GlyphArrangement nameWidth;
  nameWidth.addLineOfText(nameFont, "GREYSCALE", 0.0f, 0.0f);
  const float textW = nameWidth.getBoundingBox(0, -1, true).getWidth();
  const float logoH = 72.0f;
  const float logoW = logoH * 100.0f / 120.0f;
  const float gap = 10.0f;
  const float left = (static_cast<float>(frameW) - (logoW + gap + textW)) / 2.0f;
  const float centreY = 96.0f;
  if (logo != nullptr)
    logo->drawWithin(g, juce::Rectangle<float>(left, centreY - logoH / 2.0f, logoW, logoH),
                     juce::RectanglePlacement::centred, 1.0f);
  const float textX = left + logoW + gap;
  g.setColour(kText);
  g.setFont(nameFont);
  g.drawText("GREYSCALE", juce::Rectangle<float>(textX, centreY - 34.0f, textW + 4.0f, 46.0f),
             juce::Justification::bottomLeft);
  g.setColour(kDim);
  g.setFont(subFont);
  g.drawText("AUDIO", juce::Rectangle<float>(textX + 2.0f, centreY + 14.0f, textW, 20.0f), juce::Justification::topLeft);
  juce::String line;
  {
    const juce::ScopedLock lock(titleLock);
    line = status;
  }
  g.setColour(kDim);
  g.setFont(font(16.0f));
  g.drawText(line, area.withTrimmedTop(156), juce::Justification::centredTop);
  g.setFont(font(12.0f));
  g.drawText(juce::String::fromUTF8("v" T3K_HOST_VERSION "  \xc2\xb7  plugin " T3K_PLUGIN_REF), area.withTrimmedBottom(6),
             juce::Justification::centredBottom);
}

// The idle view: the preset and nothing else. Its number large in the top
// right corner, its name at the bottom left, both on the same margin from
// the edges and in one typeface. A long name shrinks, then takes a second
// line upwards; the number gives way above it.
void DisplayUi::drawIdle(juce::Graphics& g) {
  juce::String name;
  {
    const juce::ScopedLock lock(titleLock);
    name = title;
  }
  const int index = state.presetIndex.load();
  const float margin = 18.0f;
  const auto area = juce::Rectangle<float>(static_cast<float>(frameW), static_cast<float>(frameH)).reduced(margin);

  // The name: the largest size that fits one line, down to a floor; below
  // that two lines at the floor size.
  const float floorSize = 24.0f;
  float size = 36.0f;
  const auto widthAt = [&name](float s) {
    juce::GlyphArrangement measure;
    measure.addLineOfText(font(s, true), name, 0.0f, 0.0f);
    return measure.getBoundingBox(0, -1, true).getWidth();
  };
  while (size > floorSize && widthAt(size) > area.getWidth())
    size -= 1.0f;
  const bool twoLines = widthAt(size) > area.getWidth();
  const float nameH = size * (twoLines ? 2.2f : 1.15f);
  const auto nameBox = area.withTop(area.getBottom() - nameH);
  g.setColour(kText);
  g.setFont(font(size, true));
  g.drawFittedText(name, nameBox.toNearestInt(), juce::Justification::bottomLeft, twoLines ? 2 : 1, 1.0f);

  if (index < 0)
    return;
  // The number: its digits measured and scaled to fill the room above the
  // name, right-aligned to the margin, never wider than two thirds.
  const auto numberBox = area.withBottom(nameBox.getY() - 10.0f).withTrimmedLeft(area.getWidth() / 3.0f);
  juce::GlyphArrangement digits;
  digits.addLineOfText(font(100.0f, true), juce::String(index + 1).paddedLeft('0', 2), 0.0f, 0.0f);
  // The ink of the digits, not their line box.
  juce::Path outline;
  digits.createPath(outline);
  const auto bounds = outline.getBounds();
  const float scale =
      juce::jmin(numberBox.getHeight() / bounds.getHeight(), numberBox.getWidth() / bounds.getWidth());
  g.setColour(kNumber);
  digits.draw(g, juce::AffineTransform::translation(-bounds.getRight(), -bounds.getY())
                     .scaled(scale)
                     .translated(numberBox.getRight(), numberBox.getY()));
}

void DisplayUi::drawMain(juce::Graphics& g) {
  const int w = frameW;
  if (!state.shuttingDown.load() && state.idle()) {
    drawIdle(g);
    return;
  }
  if (state.shuttingDown.load()) {
    const auto area = juce::Rectangle<int>(frameW, frameH);
    g.setColour(kText);
    g.setFont(font(24.0f, true));
    g.drawText("Shutting down", area.withTrimmedBottom(40), juce::Justification::centred);
    g.setColour(kDim);
    g.setFont(font(14.0f));
    g.drawText("Unplug when the LED stops blinking", area.withTrimmedTop(area.getHeight() / 2 + 10),
               juce::Justification::centredTop);
    return;
  }
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
  // The block page's block, also the block on the move.
  const auto pageBlockId = state.getPageBlock();
  const ChainCell* pageCell = chain.find(pageBlockId);
  const bool moving = state.moving.load();
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

  // Cursor: a frame in the accent colour, filled with its dark tint once
  // the item is being edited.
  const auto cursor = [&](TargetKind kind, int index, juce::Rectangle<float> box) {
    if (!isSelected(kind, index))
      return;
    if (editing) {
      g.setColour(gAccentFill);
      g.fillRoundedRectangle(box, 6.0f);
    }
    g.setColour(gAccent);
    g.drawRoundedRectangle(box.reduced(1.0f), 6.0f, 2.0f);
  };

  // Layout, top to bottom: preset name with the buttons, the effect
  // switches, the stage (the chain grid, or a page's entries) between the
  // level meters, the context line, and the knob row along the bottom edge
  // where the value encoders sit, five equal slots.
  const int gap = 4;
  const int titleTop = 2;
  // The preset name is big enough to read from a distance; the buttons
  // beside it keep their size.
  const int titleH = 34;
  const int buttonSize = 26;
  const int switchTop = titleTop + titleH + gap;
  const int switchH = 20;
  const int stageTop = switchTop + switchH + gap;
  const int stageH = 86;
  const int contextTop = stageTop + stageH + 2;
  const int contextH = 17;
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
  // CPU temperature for the load readouts, read every couple of seconds.
  if (const auto frameMs = juce::Time::getMillisecondCounter();
      temperatureCheckedMs == 0 || frameMs - temperatureCheckedMs > 2000) {
    temperatureCheckedMs = frameMs;
    const auto milli = juce::File("/sys/class/thermal/thermal_zone0/temp").loadFileAsString().trim();
    cpuTemperature = milli.isEmpty() ? juce::String()
                                     : juce::String(juce::roundToInt(milli.getIntValue() / 1000.0)) +
                                           juce::String::fromUTF8(" \xc2\xb0" "C");
  }
  const juce::String cpuLoad = "CPU " + juce::String(juce::roundToInt(static_cast<double>(levels["cpu"]) * 100.0)) +
                               " %" + (cpuTemperature.isEmpty() ? juce::String() : "  " + cpuTemperature);
  const bool tunerOn = state.tunerOn.load();
  const int buttonTop = titleTop + (titleH - buttonSize) / 2;
  const juce::Rectangle<int> settingsBox(w - 2 - buttonSize, buttonTop, buttonSize, buttonSize);
  const juce::Rectangle<int> tunerBox(settingsBox.getX() - gap - buttonSize, buttonTop, buttonSize, buttonSize);
  const juce::Rectangle<int> saveBox(tunerBox.getX() - gap - buttonSize, buttonTop, buttonSize, buttonSize);
  const bool dirty = state.presetDirty.load();
  const int titleW = saveBox.getX() - gap - 2;
  const int starW = 16;
  cursor(TargetKind::Head, static_cast<int>(HeadItem::Preset), juce::Rectangle<int>(2, titleTop, titleW, titleH).toFloat());
  const auto textWidth = [](const juce::Font& f, const juce::String& text) {
    juce::GlyphArrangement measure;
    measure.addLineOfText(f, text, 0.0f, 0.0f);
    return juce::roundToInt(measure.getBoundingBox(0, -1, true).getWidth());
  };
  // The preset's place in the list (the order presets are stepped in) in
  // front of its name; a tone loaded without a preset has none.
  const int presetNumber = state.presetIndex.load() + 1;
  const auto number = presetNumber > 0 ? juce::String(presetNumber).paddedLeft('0', 2) : juce::String();
  // A long name shrinks a little before it gets cut.
  auto titleFont = font(26.0f, true);
  for (float size = 26.0f; size >= 20.0f; size -= 2.0f) {
    titleFont = font(size, true);
    const int numberW = number.isEmpty() ? 0 : textWidth(titleFont, number) + 8;
    if (8 + numberW + textWidth(titleFont, name) + 2 + starW <= titleW)
      break;
  }
  g.setFont(titleFont);
  int textX = 8;
  if (number.isNotEmpty()) {
    g.setColour(kDim);
    g.drawText(number, textX, titleTop, titleW - 10, titleH, juce::Justification::centredLeft, false);
    textX += textWidth(titleFont, number) + 8;
  }
  const int nameRoom = titleW - textX - 2 - starW;
  g.setColour(kText);
  g.drawText(name, textX, titleTop, nameRoom, titleH, juce::Justification::centredLeft, true);
  if (dirty) {
    g.setColour(kText);
    g.drawText("*", textX + juce::jmin(textWidth(titleFont, name) + 3, nameRoom), titleTop, starW, titleH,
               juce::Justification::centredLeft);
  }

  // Save button: lit while there is something to save.
  cursor(TargetKind::Head, static_cast<int>(HeadItem::Save), saveBox.toFloat());
  g.setColour(dirty ? kText : kDim);
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
    g.setColour((tunerOn ? kText : kDim).withMultipliedAlpha(headAlpha));
    g.strokePath(fork, juce::PathStrokeType(2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
  }

  // Settings button, framed while its page is open.
  cursor(TargetKind::Head, static_cast<int>(HeadItem::Settings), settingsBox.toFloat());
  if (page == Page::Settings) {
    g.setColour(kDim);
    g.drawRoundedRectangle(settingsBox.toFloat().reduced(1.0f), 6.0f, 1.0f);
  }
  g.setColour(kDim.withMultipliedAlpha(page == Page::Settings ? 1.0f : headAlpha));
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
        g.setColour(kOn);
        g.fillRoundedRectangle(box, 5.0f);
        g.setColour(kBackground);
      } else {
        g.setColour(kTrack);
        g.drawRoundedRectangle(box.reduced(0.5f), 5.0f, 1.0f);
        g.setColour(kDim);
      }
      g.drawText(switchText[i], box, juce::Justification::centred);
      if (onPage && kSwitchPages[i] == page) {
        g.setColour(kDim);
        g.drawRoundedRectangle(box.expanded(3.0f).reduced(0.5f), 7.0f, 1.0f);
      }
    }
  }

  // Input level far left, output level far right, -60..0 dB, filling
  // upwards, with the level in numbers underneath. They run the height of
  // the stage and the context line together.
  const int meterW = 10;
  const int meterMargin = 5;
  const int labelH = 13;
  const int meterBottom = contextTop + contextH;
  const char* const meterKeys[] = {"input", "output"};
  const auto nowMs = juce::Time::getMillisecondCounter();
  for (int i = 0; i < 2; ++i) {
    const int x = i == 0 ? meterMargin : w - meterMargin - meterW;
    const int barTop = stageTop;
    const juce::Rectangle<int> bar(x, barTop, meterW, meterBottom - labelH - barTop);
    const float db = meterDb(levels, meterKeys[i]);
    const float pos = juce::jlimit(0.0f, 1.0f, (db + 60.0f) / 60.0f);
    g.setColour(kTrack);
    g.fillRect(bar);
    g.setColour(db > kHotDb ? kText : kMeterFill);
    g.fillRect(bar.withTop(bar.getBottom() - juce::roundToInt(static_cast<float>(bar.getHeight()) * pos)));

    // On the gate's page the input meter shows where the threshold sits.
    if (i == 0 && page == Page::Gate) {
      const float threshold = juce::jlimit(0.0f, 1.0f, (param("gateThreshold") + 60.0f) / 60.0f);
      const int y = bar.getBottom() - juce::roundToInt(static_cast<float>(bar.getHeight()) * threshold);
      g.setColour(kText);
      g.fillRect(x - 3, y - 1, meterW + 6, 3);
    }

    // The level in numbers under the meter, its peak held for a moment;
    // red after clipping.
    if (db >= peakDb[i] || static_cast<juce::int32>(peakUntilMs[i] - nowMs) <= 0) {
      peakDb[i] = db;
      peakUntilMs[i] = nowMs + kPeakHoldMs;
    }
    const juce::String peakText = peakDb[i] <= -59.5f ? juce::String("--") : juce::String(juce::roundToInt(peakDb[i]));
    g.setFont(font(11.0f, true));
    const juce::Rectangle<int> peakBox(x - 5, meterBottom - labelH, meterW + 10, labelH);
    if (peakDb[i] >= kClipDb) {
      drawInverted(g, peakText, peakBox.toFloat());
    } else {
      g.setColour(peakDb[i] > kHotDb ? kText : kDim);
      g.drawText(peakText, peakBox, juce::Justification::centred);
    }
  }

  const int stageLeft = meterMargin + meterW + 8;
  const juce::Rectangle<int> stage(stageLeft, stageTop, w - 2 * stageLeft, stageH);
  juce::String tunerText;

  if (onPage) {
    // A page: its title, then one line per entry with the value right.
    g.setColour(kText);
    g.setFont(font(15.0f, true));
    const juce::String pageTitle = page == Page::Block ? (pageCell != nullptr ? pageCell->name : juce::String()) : def.title;
    // The settings page goes without its title (the framed button says
    // where you are), so its information fits below the entries.
    const int entriesTop = page == Page::Settings ? 2 : 19;
    if (page != Page::Settings)
      g.drawText(pageTitle, stage.withHeight(17).withTrimmedLeft(6), juce::Justification::centredLeft, true);
    const int rowH = 15;
    const auto choiceText = [](const juce::String& text) {
      return juce::String::fromUTF8("\xe2\x80\xb9 ") + text + juce::String::fromUTF8(" \xe2\x80\xba");
    };
    g.setFont(font(13.0f));
    int line = 0;
    for (int i = 0; i < static_cast<int>(def.entries.size()); ++i) {
      const auto& entry = def.entries[static_cast<size_t>(i)];
      auto row = juce::Rectangle<int>(stage.getX(), stage.getY() + entriesTop + line * (rowH + 1), stage.getWidth(), rowH);

      // Move and remove: two buttons on one line.
      if (entry.kind == EntryKind::BlockMove || entry.kind == EntryKind::BlockRemove) {
        const bool left = entry.kind == EntryKind::BlockMove;
        const auto half = left ? row.withWidth(row.getWidth() / 2 - 2) : row.withTrimmedLeft(row.getWidth() / 2 + 2);
        cursor(TargetKind::Entry, i, half.toFloat());
        const bool armed = !left && state.removeArmed.load();
        if (armed) {
          drawInverted(g, "Remove?", half.toFloat().reduced(2.0f, 1.0f));
        } else {
          g.setColour(kTrack);
          g.drawRoundedRectangle(half.toFloat().reduced(2.0f, 1.0f), 4.0f, 1.0f);
          g.setColour(kText);
          g.drawText(entry.label, half, juce::Justification::centred, true);
        }
        if (!left)
          ++line;
        continue;
      }
      ++line;
      cursor(TargetKind::Entry, i, row.toFloat());
      juce::String value;
      bool lit = false;
      switch (entry.kind) {
        case EntryKind::Param:
          if (auto* choice = dynamic_cast<juce::AudioParameterChoice*>(params.getParameter(entry.paramId))) {
            value = choiceText(choice->getCurrentChoiceName());
          } else if (auto* p = params.getParameter(entry.paramId)) {
            lit = p->getValue() > 0.5f;
            value = lit ? "On" : "Off";
          }
          break;
        case EntryKind::BlockOn:
        case EntryKind::BlockNormalize:
          if (pageCell != nullptr) {
            lit = entry.kind == EntryKind::BlockOn ? pageCell->enabled : pageCell->normalize;
            value = lit ? "On" : "Off";
          }
          break;
        case EntryKind::BlockModel:
          if (pageCell != nullptr) {
            const int pending = state.pendingModel.load();
            juce::String modelName;
            for (size_t m = 0; m < pageCell->models.size(); ++m)
              if (editing && pending >= 0 ? static_cast<int>(m) == pending : pageCell->models[m].id == pageCell->activeModel)
                modelName = pageCell->models[m].name;
            // Shortened so the arrows still show.
            if (modelName.length() > 20)
              modelName = modelName.substring(0, 18) + "..";
            value = pageCell->models.size() > 1 ? choiceText(modelName) : modelName;
          }
          break;
        case EntryKind::InputMode:
          value = choiceText(inputModeName(static_cast<int>(processor.getInputMode())));
          break;
        case EntryKind::PowerOff:
          if (state.powerArmed.load()) {
            value = "Press again";
            lit = true;
          }
          break;
        case EntryKind::BlockMove:
        case EntryKind::BlockRemove: break;
      }
      const bool armedOff = entry.kind == EntryKind::PowerOff && state.powerArmed.load();
      if (armedOff) {
        g.setColour(kText);
        g.fillRoundedRectangle(row.toFloat().reduced(1.0f), 3.0f);
      }
      g.setColour(armedOff ? kBackground : kText);
      g.drawText(entry.label, row.withTrimmedLeft(8), juce::Justification::centredLeft, true);
      g.setColour(armedOff ? kBackground : lit ? kOn : kDim);
      g.drawText(value, row.withTrimmedRight(8).withTrimmedLeft(90), juce::Justification::centredRight, true);
    }

    // The settings page tells about the machine below its entries.
    if (page == Page::Settings) {
      if (address.isEmpty() || nowMs - addressCheckedMs > 5000) {
        addressCheckedMs = nowMs;
        address = "no network";
        for (const auto& ip : juce::IPAddress::getAllAddresses())
          if (!ip.isIPv6 && ip != juce::IPAddress::local()) {
            address = ip.toString();
            break;
          }
      }
      juce::String audio;
      if (auto* dev = deviceManager.getCurrentAudioDevice())
        audio = juce::String(juce::roundToInt(dev->getCurrentSampleRate() / 1000.0)) + " kHz / " +
                juce::String(dev->getCurrentBufferSizeSamples());
      const juce::String info[] = {
          "Network  " + address,
          cpuLoad + "   " +
              juce::String(xruns) + " dropouts   " + audio,
          juce::String::fromUTF8("v" T3K_HOST_VERSION "  \xc2\xb7  plugin " T3K_PLUGIN_REF),
      };
      g.setFont(font(12.0f));
      g.setColour(kDim);
      const int infoTop = stage.getY() + entriesTop + static_cast<int>(def.entries.size()) * (rowH + 1) + 4;
      for (int i = 0; i < 3; ++i)
        g.drawText(info[i], stage.getX() + 8, infoTop + i * 15, stage.getWidth() - 16, 15,
                   juce::Justification::centredLeft, true);
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
      g.setColour(inTune ? kText : kDim);
      g.setFont(font(40.0f, true));
      g.drawText(note, zone.withTrimmedBottom(20), juce::Justification::centred);
      // The marker's distance from the centre is the deviation, 50 cents at either end.
      const int markerX = zone.getCentreX() + juce::roundToInt(juce::jlimit(-1.0f, 1.0f, cents / 50.0f) * static_cast<float>(half));
      g.setColour(inTune ? kText : kOn);
      g.fillRect(markerX - 3, barY - 6, inTune ? 6 : 4, 16);
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
        if (moving && cell.id == pageBlockId) {
          g.setColour(gAccent);
          g.drawRoundedRectangle(box.reduced(1.0f), 4.0f, 3.0f);
        } else if (blockSelected && !moving) {
          g.setColour(gAccent);
          g.drawRoundedRectangle(box.reduced(1.0f), 4.0f, 2.0f);
        }
        g.setColour(!cell.enabled || cell.loading ? kDim : kOn);
        g.drawImage(glyphFor(cell.gear), box.reduced(4.0f), juce::RectanglePlacement::centred, true);
      }
    }
  }

  // The knob row: five equal slots along the bottom edge, one above each
  // value encoder. Empty slots stay empty; their encoder does nothing here.
  const float slotW = static_cast<float>(w) / static_cast<float>(kKnobSlots);
  // The knobs sit on the bottom edge: label in the last line, knob above it.
  const float knobLabelH = 17.0f;
  const float radius = 23.0f;
  const float cy = static_cast<float>(frameH) - 1.0f - knobLabelH - 2.0f - radius;
  const float sweep = juce::MathConstants<float>::pi * 0.75f;
  g.setColour(kTrack);
  g.fillRect(0, knobTop - 2, w, 1);
  for (int i = 1; i < kKnobSlots; ++i)
    g.fillRect(juce::roundToInt(slotW * static_cast<float>(i)), knobTop + 4, 1, knobH - 10);
  for (int i = 0; i < static_cast<int>(def.knobs.size()) && i < kKnobSlots; ++i) {
    const auto& k = def.knobs[static_cast<size_t>(i)];
    const float cx = slotW * (static_cast<float>(i) + 0.5f);
    const float pos = knobPosition(k, params, pageCell);
    cursor(TargetKind::Knob, i,
           juce::Rectangle<float>(cx - slotW * 0.5f + 1.0f, static_cast<float>(knobTop), slotW - 2.0f,
                                  static_cast<float>(knobH - 1)));

    juce::Path track;
    track.addCentredArc(cx, cy, radius, radius, 0.0f, -sweep, sweep, true);
    g.setColour(kTrack);
    g.strokePath(track, juce::PathStrokeType(4.0f));
    juce::Path arc;
    arc.addCentredArc(cx, cy, radius, radius, 0.0f, -sweep, -sweep + 2.0f * sweep * pos, true);
    g.setColour(editing && isSelected(TargetKind::Knob, i) ? gAccent : kOn);
    g.strokePath(arc, juce::PathStrokeType(4.0f));

    const auto cell = juce::Rectangle<float>(cx - slotW * 0.5f, cy - radius, slotW, radius * 2.0f);
    const auto text = formatKnob(k, params, pageCell, false);
    g.setColour(kText);
    g.setFont(font(text.length() > 4 ? 12.0f : 14.0f, true));
    g.drawText(text, cell.reduced(radius * 0.25f, 0.0f), juce::Justification::centred, true);
    g.setColour(kDim);
    g.setFont(font(15.0f));
    g.drawText(k.label, cell.withY(static_cast<float>(frameH) - 1.0f - knobLabelH).withHeight(knobLabelH),
               juce::Justification::centred);
    if ((fineSlots & (1 << i)) != 0) {
      g.setColour(kText);
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
            context = "Settings   " + cpuLoad + "   " + juce::String(xruns) + " dropouts";
            break;
          case HeadItem::Preset: {
            const int index = state.presetIndex.load();
            const int count = state.presetCount.load();
            context = index >= 0 ? "Preset " + juce::String(index + 1) + " of " + juce::String(count)
                                 : juce::String(count) + " presets";
            if (dirty)
              context << "  -  hold to discard changes";
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
        if (juce::isPositiveAndBelow(target.index, static_cast<int>(tones.size()))) {
          // With several models, which one plays (or is being picked).
          const auto& cell = *tones[static_cast<size_t>(target.index)];
          context = cell.name;
          if (cell.models.size() > 1) {
            const int pending = state.pendingModel.load();
            int shown = 0;
            for (size_t m = 0; m < cell.models.size(); ++m)
              if (pending >= 0 && pageBlockId == cell.id ? static_cast<int>(m) == pending
                                                         : cell.models[m].id == cell.activeModel)
                shown = static_cast<int>(m);
            // The position first: it stays readable when a long name is cut.
            context = juce::String(shown + 1) + "/" + juce::String(static_cast<int>(cell.models.size())) + "  " +
                      cell.name + ": " + cell.models[static_cast<size_t>(shown)].name;
          } else {
            context << "  -  hold for details";
          }
        }
        break;
      case TargetKind::Entry: {
        const auto& entry = def.entries[static_cast<size_t>(target.index)];
        const bool choice = entry.kind == EntryKind::Param
                                ? dynamic_cast<juce::AudioParameterChoice*>(params.getParameter(entry.paramId)) != nullptr
                                : entry.kind == EntryKind::BlockModel || entry.kind == EntryKind::InputMode;
        if (editing)
          context = "Turn to choose, press when done";
        else if (entry.kind == EntryKind::BlockMove)
          context = "Move the block along its row";
        else if (entry.kind == EntryKind::BlockRemove)
          context = state.removeArmed.load() ? juce::String("Press again to remove, turn to keep")
                                             : juce::String("Remove the block from the chain");
        else if (entry.kind == EntryKind::BlockModel && (pageCell == nullptr || pageCell->models.size() < 2))
          context = "This tone has one model";
        else if (entry.kind == EntryKind::PowerOff)
          context = state.powerArmed.load() ? juce::String("Press again to power off, turn to keep")
                                            : juce::String("Shut down before unplugging");
        else
          context = juce::String(entry.label) + (choice ? "  -  press to change" : "  -  press to switch") +
                    ", hold to go back";
        break;
      }
      case TargetKind::Knob: {
        const auto& k = def.knobs[static_cast<size_t>(target.index)];
        context = juce::String(k.name) + " " + formatKnob(k, params, pageCell, true) + (editing ? "  -  hold for default" : "");
        break;
      }
    }
  }
  if (moving && pageCell != nullptr)
    context = "Moving " + pageCell->name + "  -  press to put down";
  // A short message wins over all of it for a moment.
  if (const auto notice = state.getNotice(); notice.isNotEmpty())
    context = notice;
  g.setColour(kText);
  g.setFont(font(16.0f));
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

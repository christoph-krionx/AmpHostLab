#include "DisplayUi.h"

#include "Processor.h"

namespace t3k {

namespace {

constexpr int kFrameMs = 50;
constexpr int kMinSplashMs = 1500;

const juce::Colour kBackground{0xff000000};
const juce::Colour kAccent{0xffffa000};
const juce::Colour kText{0xffffffff};
const juce::Colour kDim{0xff808080};
const juce::Colour kTrack{0xff303030};
const juce::Colour kMeter{0xff30c060};
const juce::Colour kWarn{0xffff4040};

struct Knob {
  const char* label;
  const char* paramId;
  float scale;  // parameter value -> 0..10 readout
};
constexpr Knob kKnobs[] = {
    {"GAIN", "inputLevel", 10.0f}, {"BASS", "toneBass", 1.0f},     {"MID", "toneMid", 1.0f},
    {"TREBLE", "toneTreble", 1.0f}, {"VOLUME", "outputLevel", 10.0f},
};

juce::Font font(float height, bool bold = false) {
  return juce::Font(juce::FontOptions(height, bold ? juce::Font::bold : juce::Font::plain));
}

float meterDb(const juce::var& levels, const char* key) {
  const auto pair = levels[key];
  return juce::jmax(static_cast<float>(pair[0]), static_cast<float>(pair[1]));
}

}  // namespace

DisplayUi::DisplayUi(TONE3000Processor& p, juce::AudioDeviceManager& dm)
    : juce::Thread("display"), processor(p), deviceManager(dm) {}

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

  // Top line: title left, audio load right (red once the device dropped out).
  juce::String name;
  {
    const juce::ScopedLock lock(titleLock);
    name = title;
  }
  int xruns = 0;
  if (auto* dev = deviceManager.getCurrentAudioDevice())
    xruns = dev->getXRunCount();
  g.setColour(kText);
  g.setFont(font(20.0f, true));
  g.drawText(name, 8, 6, w - 80, 26, juce::Justification::centredLeft, true);
  g.setColour(xruns > 0 ? kWarn : kDim);
  g.setFont(font(14.0f));
  g.drawText(juce::String(juce::roundToInt(static_cast<double>(levels["cpu"]) * 100.0)) + " %",
             w - 70, 6, 62, 26, juce::Justification::centredRight);
  g.setColour(kTrack);
  g.fillRect(0, 38, w, 1);

  // Five knobs, one per encoder.
  const float cellW = static_cast<float>(w) / static_cast<float>(std::size(kKnobs));
  const float radius = juce::jmin(24.0f, cellW * 0.38f);
  const float cy = 46.0f + radius + 10.0f;
  const float sweep = juce::MathConstants<float>::pi * 0.75f;
  for (size_t i = 0; i < std::size(kKnobs); ++i) {
    const auto& k = kKnobs[i];
    const float cx = cellW * (static_cast<float>(i) + 0.5f);
    const auto* raw = processor.getParameters().getRawParameterValue(k.paramId);
    const float value = raw != nullptr ? raw->load() * k.scale : 0.0f;
    const float pos = juce::jlimit(0.0f, 1.0f, value / 10.0f);

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

  // Effect switches: lit when on.
  const auto param = [this](const char* id) {
    const auto* raw = processor.getParameters().getRawParameterValue(id);
    return raw != nullptr ? raw->load() : 0.0f;
  };
  const float semitones = param("pitchSemitones");
  const bool stepped = param("pitchStep") > 0.5f;
  const juce::String shift = stepped ? juce::String(juce::roundToInt(semitones)) : juce::String(semitones, 1);
  const struct {
    juce::String text;
    bool on;
  } badges[] = {
      {"GATE", param("gateEnabled") > 0.5f},
      {"PITCH " + (semitones > 0.0f && shift != "0" ? "+" + shift : shift), param("pitchEnabled") > 0.5f},
      {"SPREAD", param("spreadEnabled") > 0.5f},
  };
  const int badgeGap = 8;
  const int badgeW = (w - 16 - 2 * badgeGap) / 3;
  g.setFont(font(13.0f, true));
  for (int i = 0; i < 3; ++i) {
    const auto box = juce::Rectangle<int>(8 + i * (badgeW + badgeGap), h - 96, badgeW, 26).toFloat();
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

  // Input and output level, -60..0 dB.
  const char* const meterKeys[] = {"input", "output"};
  const char* const meterNames[] = {"IN", "OUT"};
  for (int i = 0; i < 2; ++i) {
    const int y = h - 52 + i * 24;
    const float db = meterDb(levels, meterKeys[i]);
    const float pos = juce::jlimit(0.0f, 1.0f, (db + 60.0f) / 60.0f);
    g.setColour(kDim);
    g.setFont(font(12.0f));
    g.drawText(meterNames[i], 8, y, 30, 14, juce::Justification::centredLeft);
    const juce::Rectangle<int> bar(42, y, w - 50, 14);
    g.setColour(kTrack);
    g.fillRect(bar);
    g.setColour(db > -3.0f ? kWarn : kMeter);
    g.fillRect(bar.withWidth(juce::roundToInt(static_cast<float>(bar.getWidth()) * pos)));
  }
}

// Converts the frame to RGB565 and sends only the band of rows that changed.
void DisplayUi::present(const juce::Image& frame) {
  const int w = lcd.width();
  const int h = lcd.height();
  const juce::Image::BitmapData bitmap(frame, juce::Image::BitmapData::readOnly);
  int firstRow = h;
  int lastRow = -1;
  for (int y = 0; y < h; ++y) {
    const auto* src = bitmap.getLinePointer(y);
    auto* dst = pending.data() + static_cast<size_t>(y) * static_cast<size_t>(w);
    bool changed = firstFrame;
    for (int x = 0; x < w; ++x) {
      const auto* px = reinterpret_cast<const juce::PixelRGB*>(src + x * bitmap.pixelStride);
      const uint16_t c = SpiLcd::rgb(px->getRed(), px->getGreen(), px->getBlue());
      changed = changed || c != shown[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)];
      dst[x] = c;
    }
    if (changed) {
      firstRow = juce::jmin(firstRow, y);
      lastRow = y;
    }
  }
  if (lastRow < firstRow)
    return;
  if (lcd.blit(0, firstRow, w, lastRow - firstRow + 1, pending.data() + static_cast<size_t>(firstRow) * static_cast<size_t>(w))) {
    shown = pending;
    firstFrame = false;
  }
}

}  // namespace t3k

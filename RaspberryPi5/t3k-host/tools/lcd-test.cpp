// lcd-test: wiring and panel check for the SPI display.
// Draws a test picture that shows orientation, colour order and inversion:
//   - white 1 px frame around the whole screen
//   - white square in the top-left corner
//   - "AMPHOST" in the middle
//   - bars along the bottom, left to right: red, green, blue, white
// Keeps running until Ctrl+C: the GPIO lines (reset, backlight) are only
// held while the process owns them.

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

#include "display/SpiLcd.h"

namespace {

using t3k::SpiLcd;

struct Canvas {
  int w, h;
  std::vector<uint16_t> px;

  Canvas(int width, int height) : w(width), h(height), px(static_cast<size_t>(width) * height, 0) {}

  void rect(int x, int y, int rw, int rh, uint16_t c) {
    for (int yy = y; yy < y + rh; ++yy)
      for (int xx = x; xx < x + rw; ++xx)
        if (xx >= 0 && yy >= 0 && xx < w && yy < h)
          px[static_cast<size_t>(yy) * w + xx] = c;
  }
};

// 5x7 glyphs, one byte per row, bit 4 = leftmost pixel.
struct Glyph {
  char ch;
  uint8_t rows[7];
};
constexpr Glyph kGlyphs[] = {
    {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}},
    {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
    {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}},
    {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
};

void drawText(Canvas& c, const char* text, int x, int y, int scale, uint16_t color) {
  for (; *text; ++text, x += 6 * scale)
    for (const auto& g : kGlyphs)
      if (g.ch == *text)
        for (int row = 0; row < 7; ++row)
          for (int col = 0; col < 5; ++col)
            if (g.rows[row] & (0x10 >> col))
              c.rect(x + col * scale, y + row * scale, scale, scale, color);
}

void usage() {
  std::cout << "Usage: lcd-test [options]\n"
               "  --panel <ili9341|st7789>   controller (default st7789)\n"
               "  --rotation <0..3>          quarter turns (default 1, landscape)\n"
               "  --spidev <path>            default /dev/spidev0.0\n"
               "  --speed <hz>               SPI clock (default 16000000)\n"
               "  --dc <gpio> --rst <gpio>   default 25 and 27\n"
               "  --bl <gpio>                backlight pin, default none\n";
}

}  // namespace

int main(int argc, char** argv) {
  t3k::LcdConfig cfg;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    const char* v = i + 1 < argc ? argv[i + 1] : nullptr;
    if (v == nullptr) {
      usage();
      return 2;
    }
    ++i;
    if (a == "--panel") {
      if (std::strcmp(v, "ili9341") == 0 || std::strcmp(v, "ili9340") == 0) cfg.panel = t3k::LcdPanel::Ili9341;
      else if (std::strcmp(v, "st7789") == 0) cfg.panel = t3k::LcdPanel::St7789;
      else { usage(); return 2; }
    }
    else if (a == "--rotation") cfg.rotation = std::atoi(v);
    else if (a == "--spidev") cfg.spiDevice = v;
    else if (a == "--speed") cfg.spiHz = static_cast<uint32_t>(std::atol(v));
    else if (a == "--dc") cfg.dcPin = std::atoi(v);
    else if (a == "--rst") cfg.resetPin = std::atoi(v);
    else if (a == "--bl") cfg.backlightPin = std::atoi(v);
    else { usage(); return 2; }
  }

  SpiLcd lcd;
  if (!lcd.open(cfg)) {
    std::cerr << "Display error: " << lcd.error() << std::endl;
    return 1;
  }

  const uint16_t white = SpiLcd::rgb(255, 255, 255);
  Canvas c(lcd.width(), lcd.height());
  c.rect(0, 0, c.w, c.h, white);
  c.rect(1, 1, c.w - 2, c.h - 2, 0);
  c.rect(1, 1, 16, 16, white);

  const int scale = c.w >= 320 ? 6 : 4;
  drawText(c, "AMPHOST", (c.w - (7 * 6 - 1) * scale) / 2, c.h / 2 - 7 * scale, scale, SpiLcd::rgb(255, 160, 0));

  const int barW = (c.w - 2) / 4;
  const int barH = c.h / 5;
  const uint16_t bars[4] = {SpiLcd::rgb(255, 0, 0), SpiLcd::rgb(0, 255, 0), SpiLcd::rgb(0, 0, 255), white};
  for (int i = 0; i < 4; ++i)
    c.rect(1 + i * barW, c.h - 1 - barH, barW, barH, bars[i]);

  if (!lcd.blit(0, 0, c.w, c.h, c.px.data())) {
    std::cerr << "Display error: transfer failed" << std::endl;
    return 1;
  }
  lcd.setBacklight(true);
  std::cout << "Test picture sent: " << c.w << "x" << c.h << " (Ctrl+C to quit)" << std::endl;
  pause();
  return 0;
}

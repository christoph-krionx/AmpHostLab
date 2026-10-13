// SpiLcd: 240x320 SPI TFT driver (ILI9341/ILI9340 and ST7789) on spidev
// plus three GPIO lines. Userspace only, no kernel display driver.
//
// Not realtime: call it from a normal-priority thread, never from audio.

#pragma once

#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

namespace t3k {

enum class LcdPanel { Ili9341, St7789 };

struct LcdConfig {
  LcdPanel panel{LcdPanel::St7789};
  std::string spiDevice{"/dev/spidev0.0"};
  uint32_t spiHz{16000000};
  // BCM GPIO numbers on the 40-pin header; -1 = not connected.
  int dcPin{25};
  int resetPin{27};
  int backlightPin{-1};
  // 0..3, quarter turns. 1 and 3 are landscape (320x240).
  int rotation{1};
};

class SpiLcd {
public:
  SpiLcd() = default;
  ~SpiLcd();
  SpiLcd(const SpiLcd&) = delete;
  SpiLcd& operator=(const SpiLcd&) = delete;

  /** Opens SPI and GPIO, resets and initialises the panel. On failure
      returns false and `error()` says why. The backlight stays off. */
  bool open(const LcdConfig& config);
  void close();
  const std::string& error() const { return lastError; }

  int width() const { return w; }
  int height() const { return h; }

  void setBacklight(bool on);

  /** Sends a w*h block of RGB565 pixels (host byte order) to x,y. */
  bool blit(int x, int y, int blockW, int blockH, const uint16_t* pixels);
  bool fill(uint16_t color);

  static constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
  }

private:
  bool command(uint8_t cmd, std::initializer_list<uint8_t> data = {});
  bool writeBytes(const uint8_t* data, size_t size);
  bool setWindow(int x, int y, int blockW, int blockH);
  bool initPanel(const LcdConfig& config);
  bool fail(const std::string& what);

  int spiFd{-1};
  int dcFd{-1};
  int resetFd{-1};
  int backlightFd{-1};
  int w{0};
  int h{0};
  std::vector<uint8_t> txBuffer;
  std::string lastError;
};

}  // namespace t3k

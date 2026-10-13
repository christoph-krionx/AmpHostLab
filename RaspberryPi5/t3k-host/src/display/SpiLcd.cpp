#include "SpiLcd.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <linux/gpio.h>
#include <linux/spi/spidev.h>
#include <sys/ioctl.h>
#include <thread>
#include <unistd.h>

namespace t3k {

namespace {

// spidev's default per-transfer limit (bufsiz).
constexpr size_t kSpiChunk = 4096;

void sleepMs(int ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// The 40-pin header GPIOs sit on the RP1; its chip number differs between
// kernels, so find it by label.
int openHeaderGpioChip() {
  for (int i = 0; i < 16; ++i) {
    const auto path = "/dev/gpiochip" + std::to_string(i);
    const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0)
      continue;
    gpiochip_info info{};
    if (ioctl(fd, GPIO_GET_CHIPINFO_IOCTL, &info) == 0 && std::strcmp(info.label, "pinctrl-rp1") == 0)
      return fd;
    ::close(fd);
  }
  return -1;
}

int requestOutput(int chipFd, int pin, bool initial) {
  gpio_v2_line_request req{};
  req.offsets[0] = static_cast<uint32_t>(pin);
  req.num_lines = 1;
  std::strncpy(req.consumer, "t3k-lcd", sizeof(req.consumer) - 1);
  req.config.flags = GPIO_V2_LINE_FLAG_OUTPUT;
  req.config.num_attrs = 1;
  req.config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
  req.config.attrs[0].attr.values = initial ? 1 : 0;
  req.config.attrs[0].mask = 1;
  if (ioctl(chipFd, GPIO_V2_GET_LINE_IOCTL, &req) != 0)
    return -1;
  return req.fd;
}

void setLine(int fd, bool value) {
  if (fd < 0)
    return;
  gpio_v2_line_values v{};
  v.bits = value ? 1 : 0;
  v.mask = 1;
  ioctl(fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &v);
}

// MADCTL per quarter turn. Both controllers share the bit layout; the
// panels differ in scan direction and colour order (ILI9341 modules are BGR).
constexpr uint8_t kMadctlIli9341[4] = {0x48, 0x28, 0x88, 0xE8};
constexpr uint8_t kMadctlSt7789[4] = {0x00, 0x60, 0xC0, 0xA0};

}  // namespace

SpiLcd::~SpiLcd() {
  close();
}

bool SpiLcd::fail(const std::string& what) {
  lastError = what + ": " + std::strerror(errno);
  close();
  return false;
}

bool SpiLcd::open(const LcdConfig& config) {
  close();

  spiFd = ::open(config.spiDevice.c_str(), O_RDWR | O_CLOEXEC);
  if (spiFd < 0)
    return fail("open " + config.spiDevice);
  uint8_t mode = SPI_MODE_0;
  uint8_t bits = 8;
  uint32_t hz = config.spiHz;
  if (ioctl(spiFd, SPI_IOC_WR_MODE, &mode) != 0 || ioctl(spiFd, SPI_IOC_WR_BITS_PER_WORD, &bits) != 0
      || ioctl(spiFd, SPI_IOC_WR_MAX_SPEED_HZ, &hz) != 0)
    return fail("configure SPI");

  const int chipFd = openHeaderGpioChip();
  if (chipFd < 0)
    return fail("find GPIO chip pinctrl-rp1");
  dcFd = requestOutput(chipFd, config.dcPin, false);
  if (config.resetPin >= 0)
    resetFd = requestOutput(chipFd, config.resetPin, true);
  if (config.backlightPin >= 0)
    backlightFd = requestOutput(chipFd, config.backlightPin, false);
  const int requestErrno = errno;
  ::close(chipFd);
  if (dcFd < 0 || (config.resetPin >= 0 && resetFd < 0) || (config.backlightPin >= 0 && backlightFd < 0)) {
    errno = requestErrno;
    return fail("request GPIO lines");
  }

  const bool landscape = (config.rotation & 1) != 0;
  w = landscape ? 320 : 240;
  h = landscape ? 240 : 320;

  if (!initPanel(config))
    return fail("initialise panel");
  return true;
}

void SpiLcd::close() {
  for (int* fd : {&backlightFd, &resetFd, &dcFd, &spiFd}) {
    if (*fd >= 0)
      ::close(*fd);
    *fd = -1;
  }
}

void SpiLcd::setBacklight(bool on) {
  setLine(backlightFd, on);
}

bool SpiLcd::writeBytes(const uint8_t* data, size_t size) {
  while (size > 0) {
    const size_t n = size < kSpiChunk ? size : kSpiChunk;
    if (::write(spiFd, data, n) != static_cast<ssize_t>(n))
      return false;
    data += n;
    size -= n;
  }
  return true;
}

bool SpiLcd::command(uint8_t cmd, std::initializer_list<uint8_t> data) {
  setLine(dcFd, false);
  if (!writeBytes(&cmd, 1))
    return false;
  setLine(dcFd, true);
  return data.size() == 0 || writeBytes(data.begin(), data.size());
}

bool SpiLcd::initPanel(const LcdConfig& config) {
  if (resetFd >= 0) {
    setLine(resetFd, false);
    sleepMs(20);
    setLine(resetFd, true);
    sleepMs(150);
  }
  bool ok = command(0x01);  // software reset
  sleepMs(150);
  ok = ok && command(0x11);  // sleep out
  sleepMs(120);

  const int rot = config.rotation & 3;
  if (config.panel == LcdPanel::Ili9341) {
    ok = ok && command(0xC0, {0x23})                    // power control 1
         && command(0xC1, {0x10})                       // power control 2
         && command(0xC5, {0x3E, 0x28})                 // VCOM 1
         && command(0xC7, {0x86})                       // VCOM 2
         && command(0xB1, {0x00, 0x18})                 // frame rate
         && command(0xB6, {0x08, 0x82, 0x27})           // display function
         && command(0x26, {0x01})                       // gamma curve
         && command(0xE0, {0x0F, 0x31, 0x2B, 0x0C, 0x0E, 0x08, 0x4E, 0xF1, 0x37, 0x07, 0x10, 0x03, 0x0E, 0x09, 0x00})
         && command(0xE1, {0x00, 0x0E, 0x14, 0x03, 0x11, 0x07, 0x31, 0xC1, 0x48, 0x08, 0x0F, 0x0C, 0x31, 0x36, 0x0F})
         && command(0x36, {kMadctlIli9341[rot]})
         && command(0x20);                              // inversion off
  } else {
    ok = ok && command(0x36, {kMadctlSt7789[rot]})
         && command(0x21);                              // inversion on (IPS)
  }
  ok = ok && command(0x3A, {0x55})                      // 16 bit per pixel
       && command(0x13)                                 // normal display mode
       && command(0x29);                                // display on
  sleepMs(20);
  return ok;
}

bool SpiLcd::setWindow(int x, int y, int blockW, int blockH) {
  const int x1 = x + blockW - 1;
  const int y1 = y + blockH - 1;
  return command(0x2A, {uint8_t(x >> 8), uint8_t(x), uint8_t(x1 >> 8), uint8_t(x1)})
         && command(0x2B, {uint8_t(y >> 8), uint8_t(y), uint8_t(y1 >> 8), uint8_t(y1)})
         && command(0x2C);
}

bool SpiLcd::blit(int x, int y, int blockW, int blockH, const uint16_t* pixels) {
  if (spiFd < 0 || blockW <= 0 || blockH <= 0 || x < 0 || y < 0 || x + blockW > w || y + blockH > h)
    return false;
  const size_t count = static_cast<size_t>(blockW) * static_cast<size_t>(blockH);
  txBuffer.resize(count * 2);
  for (size_t i = 0; i < count; ++i) {  // the panel wants big-endian RGB565
    txBuffer[2 * i] = static_cast<uint8_t>(pixels[i] >> 8);
    txBuffer[2 * i + 1] = static_cast<uint8_t>(pixels[i]);
  }
  return setWindow(x, y, blockW, blockH) && writeBytes(txBuffer.data(), txBuffer.size());
}

bool SpiLcd::fill(uint16_t color) {
  const std::vector<uint16_t> frame(static_cast<size_t>(w) * static_cast<size_t>(h), color);
  return blit(0, 0, w, h, frame.data());
}

}  // namespace t3k

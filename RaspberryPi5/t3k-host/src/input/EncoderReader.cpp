#include "EncoderReader.h"

#include "HeaderGpio.h"

#include <cerrno>
#include <cstdint>
#include <initializer_list>
#include <poll.h>

namespace t3k {

namespace {

constexpr uint32_t kTurnDebounceUs = 500;
constexpr uint32_t kButtonDebounceUs = 5000;
// Mechanical encoders (EC11 style) pass through four quadrature states per detent.
constexpr int kQuartersPerDetent = 4;

// Quadrature step for (previous << 2) | current; 0 for no move or an
// invalid jump (a missed edge).
constexpr int kQuadrature[16] = {0, 1, -1, 0, -1, 0, 0, 1, 1, 0, 0, -1, 0, -1, 1, 0};

}  // namespace

EncoderReader::~EncoderReader() {
  close();
}

void EncoderReader::close() {
  if (lineFd >= 0)
    ::close(lineFd);
  lineFd = -1;
  states.clear();
}

bool EncoderReader::open(const std::vector<EncoderPins>& encoders) {
  close();

  gpio_v2_line_request req{};
  std::strncpy(req.consumer, "t3k-encoder", sizeof(req.consumer) - 1);
  req.config.flags = GPIO_V2_LINE_FLAG_INPUT | GPIO_V2_LINE_FLAG_BIAS_PULL_UP | GPIO_V2_LINE_FLAG_EDGE_RISING
                     | GPIO_V2_LINE_FLAG_EDGE_FALLING;
  uint64_t turnMask = 0;
  uint64_t buttonMask = 0;
  for (const auto& e : encoders) {
    for (const int pin : {e.a, e.b, e.sw}) {
      if (pin < 0)
        continue;
      if (req.num_lines >= GPIO_V2_LINES_MAX) {
        lastError = "too many encoder lines";
        return false;
      }
      (pin == e.sw ? buttonMask : turnMask) |= 1ull << req.num_lines;
      req.offsets[req.num_lines++] = static_cast<uint32_t>(pin);
    }
  }
  req.config.num_attrs = 2;
  req.config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_DEBOUNCE;
  req.config.attrs[0].attr.debounce_period_us = kTurnDebounceUs;
  req.config.attrs[0].mask = turnMask;
  req.config.attrs[1].attr.id = GPIO_V2_LINE_ATTR_ID_DEBOUNCE;
  req.config.attrs[1].attr.debounce_period_us = kButtonDebounceUs;
  req.config.attrs[1].mask = buttonMask;
  req.event_buffer_size = 256;

  const int chipFd = openHeaderGpioChip();
  if (chipFd < 0) {
    lastError = "GPIO chip pinctrl-rp1 not found";
    return false;
  }
  const int rc = ioctl(chipFd, GPIO_V2_GET_LINE_IOCTL, &req);
  const int requestErrno = errno;
  ::close(chipFd);
  if (rc != 0) {
    lastError = std::string("request GPIO lines: ") + std::strerror(requestErrno);
    return false;
  }
  lineFd = req.fd;

  for (const auto& e : encoders)
    states.push_back({e});
  return true;
}

bool EncoderReader::wait(int timeoutMs, std::vector<EncoderEvent>& events) {
  pollfd pfd{lineFd, POLLIN, 0};
  const int ready = ::poll(&pfd, 1, timeoutMs);
  if (ready == 0 || (ready < 0 && errno == EINTR))
    return true;
  if (ready < 0)
    return false;

  gpio_v2_line_event buffer[32];
  const ssize_t bytes = ::read(lineFd, buffer, sizeof(buffer));
  if (bytes < 0)
    return errno == EINTR || errno == EAGAIN;

  const size_t count = static_cast<size_t>(bytes) / sizeof(buffer[0]);
  for (size_t i = 0; i < count; ++i) {
    const int pin = static_cast<int>(buffer[i].offset);
    const bool high = buffer[i].id == GPIO_V2_LINE_EVENT_RISING_EDGE;
    for (size_t n = 0; n < states.size(); ++n) {
      auto& s = states[n];
      const int index = static_cast<int>(n);
      if (pin == s.pins.sw) {  // the button pulls the line to GND
        events.push_back({index, high ? EncoderEvent::Kind::Release : EncoderEvent::Kind::Press, 0});
      } else if (pin == s.pins.a || pin == s.pins.b) {
        const int bit = pin == s.pins.a ? 2 : 1;
        const int level = high ? (s.level | bit) : (s.level & ~bit);
        s.quarters += kQuadrature[(s.level << 2) | level];
        s.level = level;
        if (s.level == 3) {  // back at rest: a full detent, or bounce that cancels out
          if (s.quarters >= kQuartersPerDetent / 2)
            events.push_back({index, EncoderEvent::Kind::Turn, 1});
          else if (s.quarters <= -kQuartersPerDetent / 2)
            events.push_back({index, EncoderEvent::Kind::Turn, -1});
          s.quarters = 0;
        }
      }
    }
  }
  return true;
}

}  // namespace t3k

// Access to the GPIO chip behind the 40-pin header.

#pragma once

#include <cstring>
#include <fcntl.h>
#include <linux/gpio.h>
#include <string>
#include <sys/ioctl.h>
#include <unistd.h>

namespace t3k {

// The 40-pin header GPIOs sit on the RP1; its chip number differs between
// kernels, so find it by label. Returns an open fd or -1.
inline int openHeaderGpioChip() {
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

}  // namespace t3k

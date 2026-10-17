// TubeGlow: the logo glows like a warm tube. A steady low glow while nothing
// is played; while a signal passes the gate it brightens and breathes,
// slowly and a little unevenly, and fades back afterwards. It glows up
// briefly when another preset comes in. The brightness drives the logo on
// the screen (through UiState).
//
// "Played" means the input is above the gate's threshold (-55 dB with the
// gate off) and at least 12 dB above the input's own noise floor, which is
// learned on the way: it follows the level down at once and creeps up
// slowly. The noise of a hot input stage alone therefore never counts.
//
// When configured, the logo light follows too: a GPIO that is on while the
// guitar is played (held a moment across short gaps between notes). The
// light's own RC filter makes the fade; the heartbeat keeps its own GPIO on
// the same LED.
// Runs on the message thread, 20 times a second.

#pragma once

#include <juce_events/juce_events.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>

#include "HeaderGpio.h"
#include "Processor.h"
#include "UiState.h"

namespace t3k {

class TubeGlow : private juce::Timer {
public:
  /** `lightPin`: BCM GPIO of the logo light, -1 = none. */
  TubeGlow(TONE3000Processor& p, UiState& s, int lightPin) : processor(p), state(s) {
    if (lightPin >= 0) {
      lightFd = requestOutput(lightPin);
      if (lightFd < 0)
        std::cerr << "Logo light not available on GPIO " << lightPin << std::endl;
    }
    startTimer(50);
  }

  ~TubeGlow() override {
    stopTimer();
    if (lightFd >= 0) {
      setLight(false);
      ::close(lightFd);
    }
  }

private:
  // How long the light stays on after the signal drops below the threshold.
  static constexpr juce::uint32 kLightHoldMs = 400;
  // How far above the noise floor playing starts, and how fast the learned
  // floor creeps up per tick (0.2 dB a second).
  static constexpr float kAboveFloorDb = 12.0f;
  static constexpr float kFloorRiseDb = 0.01f;

  static int requestOutput(int pin) {
    const int chip = openHeaderGpioChip();
    if (chip < 0)
      return -1;
    gpio_v2_line_request req{};
    req.offsets[0] = static_cast<uint32_t>(pin);
    req.num_lines = 1;
    std::strncpy(req.consumer, "t3k-logo", sizeof(req.consumer) - 1);
    req.config.flags = GPIO_V2_LINE_FLAG_OUTPUT;
    req.config.num_attrs = 1;
    req.config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
    req.config.attrs[0].attr.values = 0;
    req.config.attrs[0].mask = 1;
    const int fd = ioctl(chip, GPIO_V2_GET_LINE_IOCTL, &req) == 0 ? req.fd : -1;
    ::close(chip);
    return fd;
  }

  void setLight(bool on) {
    gpio_v2_line_values v{};
    v.bits = on ? 1 : 0;
    v.mask = 1;
    ioctl(lightFd, GPIO_V2_LINE_SET_VALUES_IOCTL, &v);
  }

  void timerCallback() override {
    const auto nowMs = juce::Time::getMillisecondCounter();

    // Another preset: a short glow-up.
    const int preset = state.presetIndex.load();
    if (preset != lastPreset) {
      glowSinceMs = lastPreset == -2 ? 0 : nowMs;
      lastPreset = preset;
    }

    // Playing: above the gate's threshold and clear of the noise floor.
    // Quick to light up, slow to fade.
    auto& params = processor.getParameters();
    const auto* gateOn = params.getRawParameterValue("gateEnabled");
    const auto* gateDb = params.getRawParameterValue("gateThreshold");
    const float gate = gateOn != nullptr && gateOn->load() > 0.5f && gateDb != nullptr ? gateDb->load() : -55.0f;
    const auto input = processor.getMeterLevels()["input"];
    const float db = juce::jmax(static_cast<float>(input[0]), static_cast<float>(input[1]));
    noiseFloor = noiseFloor < -199.0f || db < noiseFloor ? db : noiseFloor + kFloorRiseDb;
    const float threshold = juce::jmax(gate, noiseFloor + kAboveFloorDb);
    const bool signal = db > threshold;
    if (debug && nowMs - lastDebugMs > 10000) {
      lastDebugMs = nowMs;
      std::cout << "glow: input " << juce::roundToInt(db) << " dB, floor " << juce::roundToInt(noiseFloor)
                << " dB, threshold " << juce::roundToInt(threshold) << " dB" << std::endl;
    }
    playing += (signal ? 1.0f - playing : -playing) * (signal ? 0.35f : 0.04f);

    const double t = nowMs / 1000.0;
    const double breath = 0.08 * std::sin(t * juce::MathConstants<double>::twoPi / 2.6) +
                          0.04 * std::sin(t * juce::MathConstants<double>::twoPi / 0.9 + 1.3);
    const double still = 0.55;
    const double lit = 0.88 + breath;
    const double since = (nowMs - glowSinceMs) / 1000.0;
    const double glow = glowSinceMs != 0 && since < 1.2 ? 0.4 * (1.0 - since / 1.2) : 0.0;
    state.logoGlow = static_cast<float>(juce::jlimit(0.0, 1.0, still + (lit - still) * playing + glow));

    // The light: on while played, held across the gaps between notes.
    if (lightFd >= 0) {
      if (signal)
        lastSignalMs = nowMs;
      const bool on = nowMs - lastSignalMs < kLightHoldMs;
      if (on != lightOn) {
        setLight(on);
        lightOn = on;
      }
    }
  }

  TONE3000Processor& processor;
  UiState& state;
  int lightFd{-1};
  bool lightOn{false};
  juce::uint32 lastSignalMs{0};
  float playing{0.0f};
  float noiseFloor{-200.0f};  // -200 = not measured yet
  const bool debug{std::getenv("T3K_DEBUG_GLOW") != nullptr};
  juce::uint32 lastDebugMs{0};
  int lastPreset{-2};
  juce::uint32 glowSinceMs{0};
};

}  // namespace t3k

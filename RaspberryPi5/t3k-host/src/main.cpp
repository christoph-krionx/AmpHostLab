// t3k-host: runs the TONE3000 processor headless on an ALSA device.
//
// The main thread is the JUCE message thread (the processor needs a running
// dispatch loop for its async work); audio runs on JUCE's ALSA thread.
// Anything added later (encoders, display, web control) belongs on its own
// non-realtime thread and talks to the processor through its parameters and
// public methods, never from inside the audio callback.

#include <juce_audio_utils/juce_audio_utils.h>

#include <csignal>
#include <iostream>
#include <sys/mman.h>

#include "Processor.h"
#include "display/DisplayUi.h"

namespace {

volatile std::sig_atomic_t quitRequested = 0;

void onSignal(int) {
  quitRequested = 1;
}

struct Options {
  juce::String device;  // substring of the ALSA device name
  double sampleRate{48000.0};  // the chain is anchored at 48 kHz
  int blockSize{64};
  juce::String tone;    // .nam / .wav file or a folder of them
  juce::String preset;  // preset name
  juce::String inputMode{"left"};
  int statusSeconds{5};
  juce::String lcd{"none"};  // none | ili9341 | st7789
  int lcdRotation{1};
  int lcdSpeed{16000000};
  juce::StringArray sets;  // "paramId=value", in the parameter's own units
  bool listDevices{false};
  bool listParams{false};
};

void printUsage() {
  std::cout << "Usage: t3k-host [options]\n"
               "  --list-devices        list ALSA devices and exit\n"
               "  --device <text>       ALSA device whose name contains <text>\n"
               "  --rate <hz>           sample rate (default 48000)\n"
               "  --block <frames>      buffer size (default 64)\n"
               "  --tone <path>         load a local A2 .nam / IR .wav file or folder\n"
               "  --preset <name>       load a preset by name\n"
               "  --input-mode <mode>   stereo | left | right | dual (default left)\n"
               "  --status <seconds>    CPU/xrun report interval, 0 = off (default 5)\n"
               "  --set <id>=<value>    set a parameter, repeatable (e.g. pitchSemitones=-2)\n"
               "  --list-params         list parameter ids with range and value, then exit\n"
               "  --lcd <panel>        SPI display: none | ili9341 | st7789 (default none)\n"
               "  --lcd-rotation <0..3> quarter turns (default 1, landscape)\n"
               "  --lcd-speed <hz>      SPI clock (default 16000000)\n";
}

bool parseArgs(const juce::StringArray& args, Options& o) {
  for (int i = 0; i < args.size(); ++i) {
    const auto& a = args[i];
    const auto value = [&]() -> juce::String { return i + 1 < args.size() ? args[++i] : juce::String(); };
    if (a == "--list-devices") o.listDevices = true;
    else if (a == "--device") o.device = value();
    else if (a == "--rate") o.sampleRate = value().getDoubleValue();
    else if (a == "--block") o.blockSize = value().getIntValue();
    else if (a == "--tone") o.tone = value();
    else if (a == "--preset") o.preset = value();
    else if (a == "--input-mode") o.inputMode = value();
    else if (a == "--status") o.statusSeconds = value().getIntValue();
    else if (a == "--set") o.sets.add(value());
    else if (a == "--list-params") o.listParams = true;
    else if (a == "--lcd") o.lcd = value();
    else if (a == "--lcd-rotation") o.lcdRotation = value().getIntValue();
    else if (a == "--lcd-speed") o.lcdSpeed = value().getIntValue();
    else return false;
  }
  return true;
}

juce::AudioIODeviceType* findAlsa(juce::AudioDeviceManager& dm) {
  for (auto* type : dm.getAvailableDeviceTypes())
    if (type->getTypeName() == "ALSA")
      return type;
  return nullptr;
}

juce::String findDevice(const juce::StringArray& names, const juce::String& wanted) {
  for (const auto& n : names)
    if (n.containsIgnoreCase(wanted))
      return n;
  return {};
}

// Polls the quit flag and prints load figures from the message thread.
class Monitor : private juce::Timer {
public:
  Monitor(juce::AudioDeviceManager& d, int statusSeconds) : dm(d), statusTicks(statusSeconds * 4) {
    startTimer(250);
  }

private:
  void timerCallback() override {
    if (quitRequested) {
      stopTimer();
      juce::MessageManager::getInstance()->stopDispatchLoop();
      return;
    }
    if (statusTicks > 0 && ++ticks % statusTicks == 0)
      if (auto* dev = dm.getCurrentAudioDevice())
        std::cout << "cpu " << juce::String(dm.getCpuUsage() * 100.0, 1) << " %  xruns "
                  << dev->getXRunCount() << std::endl;
  }

  juce::AudioDeviceManager& dm;
  int statusTicks;
  int ticks{0};
};

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  if (!parseArgs(juce::StringArray(argv + 1, argc - 1), opt)) {
    printUsage();
    return 2;
  }

  juce::ScopedJuceInitialiser_GUI juceInit;

  juce::AudioDeviceManager deviceManager;
  auto* alsa = findAlsa(deviceManager);
  if (alsa == nullptr) {
    std::cerr << "ALSA device type not available" << std::endl;
    return 1;
  }
  alsa->scanForDevices();

  if (opt.listDevices) {
    std::cout << "Inputs:\n";
    for (const auto& n : alsa->getDeviceNames(true)) std::cout << "  " << n << "\n";
    std::cout << "Outputs:\n";
    for (const auto& n : alsa->getDeviceNames(false)) std::cout << "  " << n << "\n";
    return 0;
  }

  TONE3000Processor processor;
  processor.setInputMode(TONE3000Processor::inputModeFromString(opt.inputMode));

  if (opt.listParams) {
    for (auto* p : static_cast<juce::AudioProcessor&>(processor).getParameters())
      if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(p)) {
        const auto& range = ranged->getNormalisableRange();
        std::cout << ranged->getParameterID() << "  " << range.start << " .. " << range.end << "  = "
                  << ranged->convertFrom0to1(ranged->getValue()) << "\n";
      }
    return 0;
  }
  for (const auto& entry : opt.sets) {
    const auto id = entry.upToFirstOccurrenceOf("=", false, false).trim();
    auto* param = processor.getParameters().getParameter(id);
    if (param == nullptr || !entry.contains("=")) {
      std::cerr << "Unknown parameter in --set " << entry << " (see --list-params)" << std::endl;
      return 2;
    }
    const auto wanted = entry.fromFirstOccurrenceOf("=", false, false).getFloatValue();
    param->setValueNotifyingHost(param->convertTo0to1(wanted));
    std::cout << "Set " << id << " = " << param->convertFrom0to1(param->getValue()) << std::endl;
  }

  // The display comes up first so the splash covers device and tone setup.
  std::unique_ptr<t3k::DisplayUi> display;
  if (opt.lcd != "none") {
    t3k::LcdConfig lcd;
    lcd.panel = opt.lcd == "st7789" ? t3k::LcdPanel::St7789 : t3k::LcdPanel::Ili9341;
    lcd.rotation = opt.lcdRotation;
    lcd.spiHz = static_cast<uint32_t>(opt.lcdSpeed);
    display = std::make_unique<t3k::DisplayUi>(processor, deviceManager);
    const auto lcdError = display->start(lcd);
    if (lcdError.isNotEmpty()) {
      std::cerr << "Display not available: " << lcdError << std::endl;
      display.reset();
    }
  }

  juce::AudioDeviceManager::AudioDeviceSetup setup;
  setup.inputDeviceName = findDevice(alsa->getDeviceNames(true), opt.device);
  setup.outputDeviceName = findDevice(alsa->getDeviceNames(false), opt.device);
  if (setup.inputDeviceName.isEmpty() || setup.outputDeviceName.isEmpty()) {
    std::cerr << "No ALSA device matching \"" << opt.device << "\" (try --list-devices)" << std::endl;
    return 1;
  }
  setup.sampleRate = opt.sampleRate;
  setup.bufferSize = opt.blockSize;
  setup.useDefaultInputChannels = false;
  setup.useDefaultOutputChannels = false;
  setup.inputChannels.setRange(0, 2, true);
  setup.outputChannels.setRange(0, 2, true);

  // Keep the process's pages in RAM so the audio thread never page-faults.
  if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
    std::cerr << "warning: mlockall failed (check memlock limit)" << std::endl;

  juce::AudioProcessorPlayer player;
  player.setProcessor(&processor);

  deviceManager.setCurrentAudioDeviceType("ALSA", true);
  const auto error = deviceManager.initialise(2, 2, nullptr, false, {}, &setup);
  if (error.isNotEmpty()) {
    std::cerr << "Audio device error: " << error << std::endl;
    player.setProcessor(nullptr);
    return 1;
  }
  deviceManager.addAudioCallback(&player);

  if (auto* dev = deviceManager.getCurrentAudioDevice()) {
    const auto rate = dev->getCurrentSampleRate();
    const auto block = dev->getCurrentBufferSizeSamples();
    const auto latency = dev->getInputLatencyInSamples() + dev->getOutputLatencyInSamples();
    std::cout << "Device: " << dev->getName() << "\n"
              << "Rate:   " << rate << " Hz, block " << block << " frames\n"
              << "Reported round trip: " << juce::String(1000.0 * latency / rate, 2) << " ms" << std::endl;
  }

  if (opt.tone.isNotEmpty()) {
    const auto file = juce::File::getCurrentWorkingDirectory().getChildFile(opt.tone);
    const auto result = processor.loadLocalTonePath(file);
    if (result.hasProperty("error"))
      std::cerr << "Tone not loaded: " << result["error"].toString() << std::endl;
    else
      std::cout << "Tone loaded: " << file.getFileName() << std::endl;
    if (display != nullptr)
      display->setTitle(file.getFileNameWithoutExtension());
  }

  if (opt.preset.isNotEmpty()) {
    const auto list = processor.getPresetList();
    juce::String id;
    if (const auto* presets = list["presets"].getArray())
      for (const auto& p : *presets)
        if (p["name"].toString().equalsIgnoreCase(opt.preset))
          id = p["id"].toString();
    if (id.isEmpty() || !processor.loadPreset(id))
      std::cerr << "Preset not loaded: " << opt.preset << std::endl;
    else
      std::cout << "Preset loaded: " << opt.preset << std::endl;
    if (display != nullptr)
      display->setTitle(opt.preset);
  }

  if (display != nullptr)
    display->showMain();

  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);

  {
    Monitor monitor(deviceManager, opt.statusSeconds);
    juce::MessageManager::getInstance()->runDispatchLoop();
  }

  display.reset();
  deviceManager.removeAudioCallback(&player);
  deviceManager.closeAudioDevice();
  player.setProcessor(nullptr);
  return 0;
}

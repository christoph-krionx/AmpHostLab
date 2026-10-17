// t3k-host: runs the TONE3000 processor headless on an ALSA device.
//
// The main thread is the JUCE message thread (the processor needs a running
// dispatch loop for its async work); audio runs on JUCE's ALSA thread.
// Anything added later (encoders, display, web control) belongs on its own
// non-realtime thread and talks to the processor through its parameters and
// public methods, never from inside the audio callback.

#include <juce_audio_utils/juce_audio_utils.h>

#include <csignal>
#include <cstring>
#include <iostream>
#include <alsa/asoundlib.h>
#include <sys/mman.h>

#include "ChainWatcher.h"
#include "HostConfig.h"
#include "RealtimeCallback.h"
#include "Processor.h"
#include "display/DisplayUi.h"
#include "input/EncoderInput.h"
#include "input/MidiControl.h"
#include "ui/PanelUi.h"
#include "ui/TubeGlow.h"

namespace {

volatile std::sig_atomic_t quitRequested = 0;

void onSignal(int) {
  quitRequested = 1;
}

struct Options {
  juce::String device;  // substring of the ALSA device name
  int deviceWait{30};   // seconds to wait for the device to appear
  double sampleRate{48000.0};  // the chain is anchored at 48 kHz
  int blockSize{64};
  juce::String tone;    // .nam / .wav file or a folder of them
  juce::String preset;  // preset name
  juce::String inputMode{"left"};
  int statusSeconds{5};
  juce::String lcd{"none"};  // none | ili9341 | st7789
  int lcdRotation{1};
  int lcdSpeed{16000000};
  int lcdBacklight{-1};  // backlight GPIO, -1 = wired to 3.3 V
  int idleView{10};      // seconds without input until the idle view, 0 = never
  juce::String uiFont{"Barlow Condensed"};  // typeface of the screen, empty = system default
  juce::String uiAccent{"amber"};  // amber | blue | #rrggbb
  int logoLight{-1};  // GPIO of the logo light, on while the guitar is played; -1 = none
  juce::String encoder;   // "a,b,sw" BCM GPIO numbers, empty = no encoder
  juce::String modelEncoder;  // the same for the second encoder
  juce::String modelEncoderRole{"knob1"};  // knob1..knob5 | volume | models
  juce::String zenKnobs[t3k::kKnobSlots];  // zen-mode control per value encoder, empty = default
  juce::String midiInput;     // MIDI input whose name contains this, empty = none
  int midiChannel{0};         // 0 = all channels
  int midiTunerCc{-1};        // CC that switches the tuner, -1 = none
  bool tunerMute{true};       // silent output while the tuner runs
  juce::StringArray midiCcs;  // "cc:target" mappings for the processor
  juce::StringArray sets;  // "paramId=value", in the parameter's own units
  juce::String config;  // start settings file; empty = the default one
  bool noConfig{false};
  bool version{false};
  bool listDevices{false};
  bool listParams{false};
  juce::String preview;  // write the screen to this PNG instead of running
  int previewCursor{-1};
  juce::String previewPage;  // overview | gate | pitch | spread
  bool previewEdit{false};
};

void printUsage() {
  std::cout << "Usage: t3k-host [options]\n"
               "  --config <file>       start settings (default ~/.config/t3k-host/t3k-host.conf)\n"
               "  --no-config           ignore the settings file\n"
               "  --version             print the version and exit\n"
               "  --list-devices        list ALSA devices and exit\n"
               "  --device <text>       ALSA device whose name contains <text>\n"
               "  --device-wait <s>     wait this long for the device to appear (default 30)\n"
               "  --rate <hz>           sample rate (default 48000)\n"
               "  --block <frames>      buffer size (default 64)\n"
               "  --tone <path>         load a local A2 .nam / IR .wav file or folder\n"
               "  --preset <name>       load a preset by name\n"
               "  --input-mode <mode>   stereo | left | right | dual (default left)\n"
               "  --status <seconds>    CPU/xrun report interval, 0 = off (default 5)\n"
               "  --set <id>=<value>    set a parameter, repeatable (e.g. pitchSemitones=-2)\n"
               "  --list-params         list parameter ids with range and value, then exit\n"
               "  --preview <png>       draw the screen into a PNG (320x240) and exit, no audio\n"
               "  --preview-cursor <n>  cursor position in the preview (default: no cursor)\n"
               "  --preview-edit        show the cursor in edit mode\n"
               "  --preview-page <name> page in the preview: overview | gate | pitch | spread | block | settings | idle | zen | zen-tuner | splash\n"
               "  --encoder <a,b,sw>    front-panel encoder on these GPIOs (e.g. 5,6,26)\n"
               "  --model-encoder <a,b,sw> second encoder (e.g. 18,19,20)\n"
               "  --model-encoder-role <r> what it does: knob1 (default) .. knob5 | volume | models\n"
               "  --zen-knob-<n> <control> value encoder n's control in the zen mode: param:<id> | block<N>:on |\n"
               "                        block<N>:inputGain|outputGain|mix (set by a long press on the panel)\n"
               "  --midi-input <text>   MIDI input whose name contains <text> (program changes, CCs)\n"
               "  --midi-channel <n>    MIDI channel 1-16, 0 = all (default)\n"
               "  --midi-tuner-cc <n>   CC number that switches the tuner on and off\n"
               "  --tuner-mute <on|off> silence the output while the tuner runs (default on)\n"
               "  --midi-cc <cc>:<target> map a CC to a parameter id, blockNPower, presetNext, presetPrevious; repeatable\n"
               "  --lcd <panel>        SPI display: none | ili9341 | st7789 (default none)\n"
               "  --lcd-rotation <0..3> quarter turns (default 1, landscape)\n"
               "  --lcd-speed <hz>      SPI clock (default 16000000)\n"
               "  --lcd-backlight <gpio> backlight switched by this GPIO (dark while the host is not running)\n"
               "  --idle-view <s>       show only the preset after this many seconds without input, 0 = never (default 10)\n"
               "  --ui-font <name>      typeface of the screen, e.g. \"Space Grotesk\" (default: Barlow Condensed)\n"
               "  --ui-accent <colour>  the screen's navigation colour: amber | blue | #rrggbb (default amber)\n"
               "  --logo-light <gpio>   GPIO switched on while the guitar is played (logo light, e.g. 12)\n";
}

bool parseArgs(const juce::StringArray& args, Options& o) {
  for (int i = 0; i < args.size(); ++i) {
    const auto& a = args[i];
    const auto value = [&]() -> juce::String { return i + 1 < args.size() ? args[++i] : juce::String(); };
    if (a == "--list-devices") o.listDevices = true;
    else if (a == "--config") o.config = value();
    else if (a == "--no-config") o.noConfig = true;
    else if (a == "--version") o.version = true;
    else if (a == "--device") o.device = value();
    else if (a == "--device-wait") o.deviceWait = value().getIntValue();
    else if (a == "--rate") o.sampleRate = value().getDoubleValue();
    else if (a == "--block") o.blockSize = value().getIntValue();
    else if (a == "--tone") o.tone = value();
    else if (a == "--preset") o.preset = value();
    else if (a == "--input-mode") o.inputMode = value();
    else if (a == "--status") o.statusSeconds = value().getIntValue();
    else if (a == "--set") o.sets.add(value());
    else if (a == "--list-params") o.listParams = true;
    else if (a == "--preview") o.preview = value();
    else if (a == "--preview-cursor") o.previewCursor = value().getIntValue();
    else if (a == "--preview-edit") o.previewEdit = true;
    else if (a == "--preview-page") o.previewPage = value();
    else if (a == "--encoder") o.encoder = value();
    else if (a == "--model-encoder") o.modelEncoder = value();
    else if (a == "--model-encoder-role") o.modelEncoderRole = value();
    else if (a.startsWith("--zen-knob-") && juce::isPositiveAndBelow(a.substring(11).getIntValue() - 1, t3k::kKnobSlots))
      o.zenKnobs[a.substring(11).getIntValue() - 1] = value();
    else if (a == "--midi-input") o.midiInput = value();
    else if (a == "--midi-channel") o.midiChannel = value().getIntValue();
    else if (a == "--midi-tuner-cc") o.midiTunerCc = value().getIntValue();
    else if (a == "--tuner-mute") o.tunerMute = !value().equalsIgnoreCase("off");
    else if (a == "--midi-cc") o.midiCcs.add(value());
    else if (a == "--lcd") o.lcd = value();
    else if (a == "--lcd-rotation") o.lcdRotation = value().getIntValue();
    else if (a == "--lcd-speed") o.lcdSpeed = value().getIntValue();
    else if (a == "--lcd-backlight") o.lcdBacklight = value().getIntValue();
    else if (a == "--idle-view") o.idleView = value().getIntValue();
    else if (a == "--ui-font") o.uiFont = value();
    else if (a == "--ui-accent") o.uiAccent = value();
    else if (a == "--logo-light") o.logoLight = value().getIntValue();
    else return false;
  }
  return true;
}

// The screen's navigation colour by name or as #rrggbb.
juce::Colour accentColour(const juce::String& text) {
  if (text.equalsIgnoreCase("blue"))
    return juce::Colour(0xff6fa3d6);
  if (text.startsWith("#") && text.length() == 7)
    return juce::Colour::fromString("ff" + text.substring(1));
  return juce::Colour(0xffc8501e);  // amber: a tube's glow
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

// Polls the quit flag and prints load figures from the message thread,
// and says once whether the audio thread got realtime scheduling.
class Monitor : private juce::Timer {
public:
  Monitor(juce::AudioDeviceManager& d, const t3k::RealtimeCallback& r, int statusSeconds)
      : dm(d), realtime(r), statusTicks(statusSeconds * 4) {
    startTimer(250);
  }

private:
  void timerCallback() override {
    if (quitRequested) {
      stopTimer();
      juce::MessageManager::getInstance()->stopDispatchLoop();
      return;
    }
    if (const int result = realtime.result(); result != reportedRealtime) {
      reportedRealtime = result;
      if (result > 0)
        std::cout << "Audio thread: realtime (SCHED_FIFO " << result << ")" << std::endl;
      else if (result < 0)
        std::cerr << "warning: audio thread stays at normal priority: " << std::strerror(-result)
                  << " (check the rtprio limit)" << std::endl;
    }
    if (statusTicks > 0 && ++ticks % statusTicks == 0)
      if (auto* dev = dm.getCurrentAudioDevice())
        std::cout << "cpu " << juce::String(dm.getCpuUsage() * 100.0, 1) << " %  xruns "
                  << dev->getXRunCount() << std::endl;
  }

  juce::AudioDeviceManager& dm;
  const t3k::RealtimeCallback& realtime;
  int reportedRealtime{0};
  int statusTicks;
  int ticks{0};
};

}  // namespace

int main(int argc, char** argv) {
  // The settings file first, then the command line on top of it.
  const juce::StringArray cliArgs(argv + 1, argc - 1);
  Options cli;
  if (!parseArgs(cliArgs, cli)) {
    printUsage();
    return 2;
  }
  if (cli.version) {
    std::cout << "t3k-host " << T3K_HOST_VERSION << " (plugin " << T3K_PLUGIN_REF << ")" << std::endl;
    return 0;
  }
  const juce::File configFile =
      cli.noConfig ? juce::File()
                   : cli.config.isNotEmpty() ? juce::File::getCurrentWorkingDirectory().getChildFile(cli.config)
                                             : t3k::HostConfig::defaultFile();
  Options opt;
  if (configFile.existsAsFile()) {
    juce::StringArray configArgs;
    juce::String configError;
    if (!t3k::HostConfig::read(configFile, configArgs, configError) || !parseArgs(configArgs, opt)) {
      std::cerr << "Bad settings file " << configFile.getFullPathName() << ": "
                << (configError.isNotEmpty() ? configError : juce::String("unknown key")) << std::endl;
      return 2;
    }
    std::cout << "Settings: " << configFile.getFullPathName() << std::endl;
  } else if (cli.config.isNotEmpty()) {
    std::cerr << "Settings file not found: " << configFile.getFullPathName() << std::endl;
    return 2;
  }
  // A tone or preset on the command line replaces the file's sound.
  if (cliArgs.contains("--tone") || cliArgs.contains("--preset")) {
    opt.tone.clear();
    opt.preset.clear();
  }
  parseArgs(cliArgs, opt);

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

  // Preview: draw the screen into a PNG, without audio device or panel.
  if (opt.preview.isNotEmpty()) {
    processor.setPlayConfigDetails(2, 2, opt.sampleRate, opt.blockSize);
    processor.prepareToPlay(opt.sampleRate, opt.blockSize);
    t3k::UiState previewState;
    t3k::setDisplayLook(opt.uiFont, accentColour(opt.uiAccent));
    t3k::DisplayUi previewUi(processor, deviceManager, previewState);
    if (opt.tone.isNotEmpty()) {
      const auto file = juce::File::getCurrentWorkingDirectory().getChildFile(opt.tone);
      processor.loadLocalTonePath(file);
      previewUi.setTitle(file.getFileNameWithoutExtension());
    }
    if (opt.preset.isNotEmpty()) {
      const auto list = processor.getPresetList();
      if (const auto* presets = list["presets"].getArray())
        for (int i = 0; i < presets->size(); ++i)
          if ((*presets)[i]["name"].toString().equalsIgnoreCase(opt.preset)) {
            processor.loadPreset((*presets)[i]["id"].toString());
            previewState.presetIndex = i;
          }
      previewUi.setTitle(opt.preset);
    }
    t3k::ChainWatcher previewWatcher(processor, previewState);
    // Give the models a moment to load before drawing; the page and cursor
    // are set once the chain is there (the block page shows the first block).
    juce::Timer::callAfterDelay(3000, [&] {
      const auto previewChain = previewState.getChain();
      if (const auto tones = previewChain.tones(); !tones.empty())
        previewState.setPageBlock(tones.front()->id);
      const juce::StringArray pageNames{"overview", "gate", "pitch", "spread", "block", "settings"};
      previewState.page = juce::jmax(0, pageNames.indexOf(opt.previewPage, true));
      previewState.active = opt.previewCursor >= 0;
      previewState.selected = juce::jmax(0, opt.previewCursor);
      previewState.editing = opt.previewEdit;
      // "idle": as if nobody had touched the controls for a minute; "zen":
      // the idle view while the first value encoder turns.
      const bool zen = opt.previewPage.equalsIgnoreCase("zen");
      previewState.lastInputMs = juce::Time::getMillisecondCounter() -
                                 (zen || opt.previewPage.equalsIgnoreCase("idle") ? 60000u : 0u);
      // "zen-tuner": the zen mode's tuner, a little sharp of A.
      if (opt.previewPage.equalsIgnoreCase("zen-tuner")) {
        previewState.lastInputMs = juce::Time::getMillisecondCounter() - 60000u;
        previewState.zenTuner = true;
        previewState.tunerOn = true;
        previewState.tunerHz = 110.6f;
      }
      if (zen) {
        const auto control = opt.zenKnobs[0].isNotEmpty() ? t3k::ZenControl::parse(opt.zenKnobs[0])
                                                           : t3k::defaultZenControl(0);
        previewState.showZen(t3k::readZenControl(control, processor.getParameters(), previewChain), 60000);
      }
      previewState.logoGlow = 0.95f;  // the logo as while playing
      const auto frame = previewUi.renderPreview(320, 240, opt.previewPage.equalsIgnoreCase("splash"));
      const juce::File out = juce::File::getCurrentWorkingDirectory().getChildFile(opt.preview);
      out.deleteFile();
      juce::FileOutputStream stream(out);
      juce::PNGImageFormat png;
      if (stream.openedOk() && png.writeImageToStream(frame, stream))
        std::cout << "Preview written: " << out.getFullPathName() << std::endl;
      else
        std::cerr << "Preview not written: " << out.getFullPathName() << std::endl;
      juce::MessageManager::getInstance()->stopDispatchLoop();
    });
    juce::MessageManager::getInstance()->runDispatchLoop();
    processor.releaseResources();
    return 0;
  }

  // Early, so a stop while waiting for the device still ends cleanly.
  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);

  // The display comes up first so the splash covers device and tone setup.
  t3k::UiState uiState;
  std::unique_ptr<t3k::DisplayUi> display;
  if (opt.lcd != "none") {
    t3k::LcdConfig lcd;
    lcd.panel = opt.lcd == "st7789" ? t3k::LcdPanel::St7789 : t3k::LcdPanel::Ili9341;
    lcd.rotation = opt.lcdRotation;
    lcd.spiHz = static_cast<uint32_t>(opt.lcdSpeed);
    lcd.backlightPin = opt.lcdBacklight;
    t3k::setDisplayLook(opt.uiFont, accentColour(opt.uiAccent));
    display = std::make_unique<t3k::DisplayUi>(processor, deviceManager, uiState);
    const auto lcdError = display->start(lcd);
    if (lcdError.isNotEmpty()) {
      std::cerr << "Display not available: " << lcdError << std::endl;
      display.reset();
    }
  }

  // On boot a USB interface can show up after the host has started.
  juce::AudioDeviceManager::AudioDeviceSetup setup;
  const auto waitUntilMs =
      juce::Time::getMillisecondCounter() + static_cast<juce::uint32>(juce::jmax(0, opt.deviceWait)) * 1000;
  for (bool announced = false;;) {
    setup.inputDeviceName = findDevice(alsa->getDeviceNames(true), opt.device);
    setup.outputDeviceName = findDevice(alsa->getDeviceNames(false), opt.device);
    if (setup.inputDeviceName.isNotEmpty() && setup.outputDeviceName.isNotEmpty())
      break;
    if (quitRequested)
      return 0;
    if (juce::Time::getMillisecondCounter() >= waitUntilMs) {
      std::cerr << "No ALSA device matching \"" << opt.device << "\" (try --list-devices)" << std::endl;
      if (display != nullptr) {
        display->setStatus("No audio interface \"" + opt.device + "\"");
        juce::Thread::sleep(3000);  // long enough to read before the restart
      }
      return 1;
    }
    if (!announced) {
      std::cout << "Waiting for an ALSA device matching \"" << opt.device << "\"" << std::endl;
      if (display != nullptr)
        display->setStatus("Waiting for " + opt.device);
      announced = true;
    }
    juce::Thread::sleep(500);
    // JUCE's ALSA type lists the devices only on its first scan, so a card
    // that appears later needs a fresh one (on a fresh alsa-lib
    // configuration), which then replaces the device manager's.
    snd_config_update_free_global();
    std::unique_ptr<juce::AudioIODeviceType> fresh(juce::AudioIODeviceType::createAudioIODeviceType_ALSA());
    fresh->scanForDevices();
    if (findDevice(fresh->getDeviceNames(true), opt.device).isNotEmpty() &&
        findDevice(fresh->getDeviceNames(false), opt.device).isNotEmpty()) {
      deviceManager.removeAudioDeviceType(alsa);
      alsa = fresh.get();
      deviceManager.addAudioDeviceType(std::move(fresh));
    }
  }
  if (display != nullptr)
    display->setStatus("starting");
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
  // Below the limit of 95 the service allows, above JUCE's own threads.
  t3k::RealtimeCallback realtime(player, 80);
  // Silent while the tuner runs, if so set.
  uiState.tunerMute = opt.tunerMute;
  realtime.muted = [&uiState] { return uiState.tunerOn.load() && uiState.tunerMute.load(); };

  deviceManager.setCurrentAudioDeviceType("ALSA", true);
  const auto error = deviceManager.initialise(2, 2, nullptr, false, {}, &setup);
  if (error.isNotEmpty()) {
    std::cerr << "Audio device error: " << error << std::endl;
    player.setProcessor(nullptr);
    return 1;
  }
  deviceManager.addAudioCallback(&realtime);

  if (auto* dev = deviceManager.getCurrentAudioDevice()) {
    const auto rate = dev->getCurrentSampleRate();
    const auto block = dev->getCurrentBufferSizeSamples();
    const auto latency = dev->getInputLatencyInSamples() + dev->getOutputLatencyInSamples();
    std::cout << "Device: " << dev->getName() << "\n"
              << "Rate:   " << rate << " Hz, block " << block << " frames\n"
              << "Reported round trip: " << juce::String(1000.0 * latency / rate, 2) << " ms" << std::endl;
  }

  // A preset wins; the tone is the fallback when there is none (or it is gone).
  bool presetLoaded = false;
  if (opt.preset.isNotEmpty()) {
    const auto list = processor.getPresetList();
    juce::String id;
    if (const auto* presets = list["presets"].getArray())
      for (const auto& p : *presets)
        if (p["name"].toString().equalsIgnoreCase(opt.preset))
          id = p["id"].toString();
    presetLoaded = id.isNotEmpty() && processor.loadPreset(id);
    if (presetLoaded) {
      std::cout << "Preset loaded: " << opt.preset << std::endl;
      if (display != nullptr)
        display->setTitle(opt.preset);
    } else {
      std::cerr << "Preset not loaded: " << opt.preset << std::endl;
      opt.preset.clear();
    }
  }

  if (!presetLoaded && opt.tone.isNotEmpty()) {
    const auto file = juce::File::getCurrentWorkingDirectory().getChildFile(opt.tone);
    const auto result = processor.loadLocalTonePath(file);
    if (result.hasProperty("error"))
      std::cerr << "Tone not loaded: " << result["error"].toString() << std::endl;
    else
      std::cout << "Tone loaded: " << file.getFileName() << std::endl;
    if (display != nullptr)
      display->setTitle(file.getFileNameWithoutExtension());
  }

  t3k::ChainWatcher chainWatcher(processor, uiState);
  // The logo's tube glow, on the screen and on the logo light.
  t3k::TubeGlow tubeGlow(processor, uiState, opt.logoLight);

  std::unique_ptr<t3k::PanelUi> panel;
  std::unique_ptr<t3k::EncoderInput> encoder;
  if (opt.encoder.isNotEmpty()) {
    uiState.idleViewMs = static_cast<juce::uint32>(juce::jmax(0, opt.idleView)) * 1000;
    // Like an amp: after the splash the screen shows the preset right away;
    // the first touch of a control brings the full screen.
    uiState.lastInputMs = juce::Time::getMillisecondCounter() - uiState.idleViewMs.load() - 1000;
    panel = std::make_unique<t3k::PanelUi>(processor, uiState, display.get());
    panel->setCurrentPreset(opt.preset);
    panel->setSecondEncoderStepsModels(opt.modelEncoderRole.equalsIgnoreCase("models"));
    for (int i = 0; i < t3k::kKnobSlots; ++i)
      if (opt.zenKnobs[i].isNotEmpty())
        panel->setZenControl(i, opt.zenKnobs[i]);
    // The preset turned to last and the settings changed on the panel come
    // back on the next start.
    if (configFile.existsAsFile()) {
      const auto remember = [configFile](const juce::String& key, const juce::String& value) {
        if (!t3k::HostConfig::rememberSetting(configFile, key, value))
          std::cerr << "Could not write " << configFile.getFullPathName() << std::endl;
      };
      panel->onPresetLoaded = [remember](const juce::String& name) { remember("preset", name); };
      panel->onSettingChanged = remember;
    }
    encoder = std::make_unique<t3k::EncoderInput>([&panel](const t3k::InputEvent& event) {
      if (panel != nullptr)
        panel->handle(event);
    });
    const auto pinsOf = [](const juce::String& text, t3k::EncoderPins& out) {
      const auto pins = juce::StringArray::fromTokens(text, ",", "");
      out = {pins[0].getIntValue(), pins[1].getIntValue(), pins[2].getIntValue()};
      return pins.size() == 3;
    };
    t3k::EncoderPins nav{}, model{};
    const bool haveModel = opt.modelEncoder.isNotEmpty();
    juce::String encoderError;
    if (!pinsOf(opt.encoder, nav))
      encoderError = "expected --encoder a,b,sw";
    else if (haveModel && !pinsOf(opt.modelEncoder, model))
      encoderError = "expected --model-encoder a,b,sw";
    else {
      // The second encoder as a value encoder ("knob2" = the second slot),
      // or the volume or model encoder.
      const auto role = opt.modelEncoderRole.toLowerCase();
      const bool knob = role.startsWith("knob");
      const int slot = juce::jlimit(0, t3k::kKnobSlots - 1, role.substring(4).getIntValue() - 1);
      encoderError = encoder->start(nav, haveModel ? &model : nullptr,
                                    knob ? t3k::InputEvent::Source::Value : t3k::InputEvent::Source::Model, slot);
    }
    if (encoderError.isNotEmpty()) {
      std::cerr << "Encoder not available: " << encoderError << std::endl;
      encoder.reset();
      panel.reset();
    }
  }

  // The display follows presets that MIDI loads.
  chainWatcher.onPresetChanged = [&panel](const juce::String& presetId) {
    if (panel != nullptr)
      panel->presetChangedElsewhere(presetId);
  };

  // MIDI: program changes and the CC mappings go to the processor, the
  // tuner CC to the panel.
  std::unique_ptr<t3k::MidiControl> midi;
  if (opt.midiInput.isNotEmpty()) {
    processor.midiMapper.setChannelFilter(juce::jlimit(0, 16, opt.midiChannel));
    for (const auto& entry : opt.midiCcs) {
      const int cc = entry.upToFirstOccurrenceOf(":", false, false).trim().getIntValue();
      const auto target = entry.fromFirstOccurrenceOf(":", false, false).trim();
      if (!entry.contains(":") || !processor.midiMapper.setCcMapping(target, cc))
        std::cerr << "MIDI mapping not set: " << entry << std::endl;
    }
    midi = std::make_unique<t3k::MidiControl>(deviceManager, player, [&panel, &uiState] {
      if (panel != nullptr)
        panel->toggleTuner();
      else
        uiState.tunerOn = !uiState.tunerOn.load();
    });
    const auto name = midi->start(opt.midiInput, opt.midiChannel, opt.midiTunerCc);
    if (name.isEmpty())
      std::cerr << "No MIDI input matching \"" << opt.midiInput << "\"" << std::endl;
    else
      std::cout << "MIDI input: " << name << std::endl;
  }

  if (display != nullptr)
    display->showMain();

  {
    Monitor monitor(deviceManager, realtime, opt.statusSeconds);
    juce::MessageManager::getInstance()->runDispatchLoop();
  }

  midi.reset();
  encoder.reset();
  panel.reset();
  display.reset();
  deviceManager.removeAudioCallback(&realtime);
  deviceManager.closeAudioDevice();
  player.setProcessor(nullptr);
  return 0;
}

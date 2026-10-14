// HostConfig: the start settings in a plain text file, so the host can come
// up on boot without a command line. One "key = value" per line, "#" starts
// a comment, keys are the command-line option names without "--" ("set" may
// repeat). A leading "~/" in a value stands for the home directory.
//
// The host writes back only the "preset" line (the preset loaded last) and
// leaves every other line, comments included, as it is.

#pragma once

#include <juce_core/juce_core.h>

namespace t3k::HostConfig {

inline juce::File defaultFile() {
  return juce::File::getSpecialLocation(juce::File::userHomeDirectory).getChildFile(".config/t3k-host/t3k-host.conf");
}

inline juce::String keyOf(const juce::String& line) {
  return line.upToFirstOccurrenceOf("#", false, false).upToFirstOccurrenceOf("=", false, false).trim();
}

/** The file as command-line arguments ("--key", "value", ...). Lines
    without a value are skipped. Returns false with `error` set when a line
    has no "=". */
inline bool read(const juce::File& file, juce::StringArray& args, juce::String& error) {
  juce::StringArray lines;
  file.readLines(lines);
  const auto home = juce::File::getSpecialLocation(juce::File::userHomeDirectory).getFullPathName();
  for (int i = 0; i < lines.size(); ++i) {
    const auto line = lines[i].upToFirstOccurrenceOf("#", false, false).trim();
    if (line.isEmpty())
      continue;
    if (!line.contains("=")) {
      error = file.getFileName() + " line " + juce::String(i + 1) + ": expected key = value";
      return false;
    }
    auto value = line.fromFirstOccurrenceOf("=", false, false).trim();
    if (value.isEmpty())
      continue;
    if (value.startsWith("~/"))
      value = home + value.substring(1);
    args.add("--" + keyOf(line));
    args.add(value);
  }
  return true;
}

/** Sets the "preset" line to `name`, appending one if there is none. The
    file is replaced in one step, so a power cut leaves the old or the new
    file, never half of one. */
inline bool rememberPreset(const juce::File& file, const juce::String& name) {
  juce::StringArray lines;
  file.readLines(lines);
  // readLines leaves an empty last entry for the final newline.
  if (!lines.isEmpty() && lines[lines.size() - 1].isEmpty())
    lines.remove(lines.size() - 1);
  bool found = false;
  for (auto& line : lines) {
    if (keyOf(line) == "preset") {
      line = "preset = " + name;
      found = true;
      break;
    }
  }
  if (!found)
    lines.add("preset = " + name);

  juce::TemporaryFile temp(file);
  if (!temp.getFile().replaceWithText(lines.joinIntoString("\n") + "\n"))
    return false;
  return temp.overwriteTargetFileWithTemporary();
}

}  // namespace t3k::HostConfig

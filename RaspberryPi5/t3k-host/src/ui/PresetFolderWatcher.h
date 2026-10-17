// PresetFolderWatcher: notices when presets are dropped into, replaced in or
// removed from the user preset folder (a network share, for example), or
// when their order changes. Checks once a second on the message thread and
// reports a change only once the folder has stopped changing, so a file
// still being copied is never read half-written.

#pragma once

#include <juce_events/juce_events.h>

#include <functional>
#include <map>

#include "PresetFile.h"

namespace t3k {

class PresetFolderWatcher : private juce::Timer {
public:
  /** `changed` gets the ids of the presets whose files are new or were
      rewritten (removed ones simply disappear from the list). */
  explicit PresetFolderWatcher(std::function<void(const juce::StringArray& changed)> onChange)
      : callback(std::move(onChange)),
        folder(juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                   .getChildFile("TONE3000")
                   .getChildFile("Presets")) {
    baseline = scan();
    startTimer(1000);
  }

  /** Takes the folder as it is now as the reference, e.g. after the device
      saved a preset itself. */
  void resync() {
    baseline = scan();
    pending.clear();
  }

private:
  struct Stamp {
    juce::int64 modifiedMs;
    juce::int64 size;
    bool operator==(const Stamp& other) const { return modifiedMs == other.modifiedMs && size == other.size; }
  };
  using Snapshot = std::map<juce::String, Stamp>;  // full path -> stamp

  Snapshot scan() const {
    Snapshot out;
    for (const auto& file : folder.findChildFiles(juce::File::findFiles, false, "*" + juce::String(presetfile::kExtension)))
      out[file.getFullPathName()] = {file.getLastModificationTime().toMilliseconds(), file.getSize()};
    // The order the presets are listed (and numbered) in.
    const auto order = folder.getChildFile("order.json");
    if (order.existsAsFile())
      out[order.getFullPathName()] = {order.getLastModificationTime().toMilliseconds(), order.getSize()};
    return out;
  }

  void timerCallback() override {
    auto now = scan();
    if (now == baseline) {
      pending.clear();
      return;
    }
    // Changed since the last look: wait for one more look without change.
    if (now != pending) {
      pending = std::move(now);
      return;
    }
    juce::StringArray changedIds;
    for (const auto& [path, stamp] : now) {
      const auto old = baseline.find(path);
      if (old != baseline.end() && old->second == stamp)
        continue;
      const juce::File file(path);
      if (!file.hasFileExtension(presetfile::kExtension))
        continue;
      // The same id the plugin lists the preset under.
      const auto header = presetfile::readHeader(file);
      if (header.valid)
        changedIds.add("user:" + (header.id.isEmpty() ? file.getFileNameWithoutExtension() : header.id));
    }
    baseline = std::move(now);
    pending.clear();
    callback(changedIds);
  }

  std::function<void(const juce::StringArray&)> callback;
  juce::File folder;
  Snapshot baseline;
  Snapshot pending;
};

}  // namespace t3k

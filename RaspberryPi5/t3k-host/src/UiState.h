// What the front panel is pointing at and what the chain looks like;
// written on the message thread, read by the display thread.

#pragma once

#include <juce_core/juce_core.h>

#include <atomic>
#include <functional>
#include <string>

#include "ui/ChainView.h"
#include "ui/PanelModel.h"

namespace t3k {

struct UiState {
  std::atomic<bool> active{false};  // false = no encoder, draw no cursor
  std::atomic<int> page{static_cast<int>(Page::Overview)};
  std::atomic<int> selected{0};     // index into cursorTargets() of the page
  std::atomic<bool> editing{false};
  // True while the knob slots are worked by the navigation encoder (no
  // value encoders fitted): the cursor then walks through them too.
  std::atomic<bool> knobsInPath{true};
  // Value encoders in fine mode, one bit per knob slot.
  std::atomic<int> fineSlots{0};
  // The block page's block is being moved through its row on the grid.
  std::atomic<bool> moving{false};
  // The block page's remove asked for confirmation; the next press removes.
  std::atomic<bool> removeArmed{false};
  // The settings page's power off asked for confirmation.
  std::atomic<bool> powerArmed{false};
  // The system is shutting down: the screen says when to unplug.
  std::atomic<bool> shuttingDown{false};
  // Model picked while editing the block page's model entry, applied when
  // editing ends; -1 = none.
  std::atomic<int> pendingModel{-1};
  // Position in the preset list, for the display's context line.
  std::atomic<int> presetIndex{-1};
  std::atomic<int> presetCount{0};
  // True while the sound differs from the preset as loaded or last saved.
  std::atomic<bool> presetDirty{false};
  // Set by the panel after saving; the watcher takes it as the new reference.
  std::atomic<bool> presetSaved{false};
  // Tuner: switched by the panel, measured on the message thread.
  std::atomic<bool> tunerOn{false};
  std::atomic<float> tunerHz{0.0f};  // 0 = no reliable pitch

  Page currentPage() const { return static_cast<Page>(page.load()); }

  ChainView getChain() const {
    const juce::ScopedLock lock(chainLock);
    return chain;
  }
  void setChain(ChainView view) {
    const juce::ScopedLock lock(chainLock);
    chain = std::move(view);
  }
  /** Changes one block in place, so the screen shows a new value before
      the processor's next chain state confirms it. */
  void updateCell(const std::string& blockId, const std::function<void(ChainCell&)>& change) {
    const juce::ScopedLock lock(chainLock);
    if (auto* cell = chain.find(blockId))
      change(*cell);
  }

  /** A short message for the context line, shown for a moment instead of
      what the cursor is on. */
  void notify(const juce::String& text, juce::uint32 forMs = 2000) {
    const juce::ScopedLock lock(chainLock);
    notice = text;
    noticeUntilMs = juce::Time::getMillisecondCounter() + forMs;
  }
  /** The current message, empty when there is none. */
  juce::String getNotice() const {
    const juce::ScopedLock lock(chainLock);
    return static_cast<juce::int32>(noticeUntilMs - juce::Time::getMillisecondCounter()) > 0 ? notice : juce::String();
  }

  /** The block the block page (or a move) is about. */
  std::string getPageBlock() const {
    const juce::ScopedLock lock(chainLock);
    return pageBlock;
  }
  void setPageBlock(const std::string& blockId) {
    const juce::ScopedLock lock(chainLock);
    pageBlock = blockId;
  }

private:
  juce::CriticalSection chainLock;  // guards chain, pageBlock and the notice
  ChainView chain;
  std::string pageBlock;
  juce::String notice;
  juce::uint32 noticeUntilMs{0};
};

}  // namespace t3k

#pragma once

// The Borges screens. Everything that needs the radio runs in the minimal network boot
// and leaves through a reboot or deep sleep. docs/borges-sync.md sections 1, 2 and 5.

#include <I18n.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "BorgesGlue.h"
#include "BorgesStack.h"
#include "ReaderTracker.h"
#include "SessionWorker.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

namespace borges {
namespace glue {

// Leaves the current activity through its onExit() (a reader finalizes there), then
// reboots into the network boot.
class BorgesHandoffActivity final : public Activity {
 public:
  BorgesHandoffActivity(GfxRenderer& renderer, MappedInputManager& input, NetPayload payload, std::string bookPath = {})
      : Activity("BorgesHandoff", renderer, input), payload_(payload), bookPath_(std::move(bookPath)) {}
  void onEnter() override;

 private:
  NetPayload payload_;
  std::string bookPath_;
};

// Before a book opens. "Any button opens now" is literal: the reboot does not wait for
// the session, and everything the session writes is crash-safe.
class BorgesOpenActivity final : public Activity {
 public:
  BorgesOpenActivity(GfxRenderer& renderer, MappedInputManager& input, std::string path);
  ~BorgesOpenActivity() override;
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }

 private:
  struct Session;
  static void sessionMain(void* self);
  void openBook();

  std::string path_;
  std::string title_;
  std::unique_ptr<BorgesStack> stack_;
  std::unique_ptr<Session> session_;
  SessionWorker worker_;
  std::atomic<bool> awaitingChoice_{false};
};

// ¿Dónde querés seguir leyendo? Owns the live stack: the engine keeps the pending choice.
class BorgesResumeActivity final : public Activity {
 public:
  enum class Return : uint8_t { Reader, Status };
  BorgesResumeActivity(GfxRenderer& renderer, MappedInputManager& input, std::unique_ptr<BorgesStack> stack,
                       std::unique_ptr<ResumeDecision> decision, std::string path, std::string title, Return back);
  ~BorgesResumeActivity() override;
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return back_ == Return::Reader; }

 private:
  enum class State : uint8_t { Choosing, Working, CouldNotLocate };
  static void stayMain(void* self);
  static void drainMain(void* self);
  void goThere();
  void stayHere();
  bool applyJump();
  // What the saved page maps to, for a book the ledger cannot answer for. Radio already off.
  void mapSavedPage(Position& out);
  void leave(bool success);

  std::unique_ptr<BorgesStack> stack_;
  std::unique_ptr<ResumeDecision> decision_;
  std::unique_ptr<Position> before_;
  std::unique_ptr<Position> stayAt_;
  std::string path_;
  std::string title_;
  Return back_;
  State state_ = State::Choosing;
  int focus_ = 0;
  bool firstPaint_ = true;
  bool choiceStored_ = false;
  unsigned long noticeUntil_ = 0;
  ButtonNavigator navigator_;
  SessionWorker worker_;
};

// No screen: the sleep image stays on the glass. Sleep happens whatever goes wrong.
class BorgesSleepFlushActivity final : public Activity {
 public:
  BorgesSleepFlushActivity(GfxRenderer& renderer, MappedInputManager& input)
      : Activity("BorgesSleepFlush", renderer, input) {}
  void onEnter() override;
  void loop() override;
  // Owns the main loop: no auto-sleep or global shortcut can repaint the sleep image.
  bool requiresExclusiveStorageLoop() const override { return true; }
  bool preventAutoSleep() override { return true; }

 private:
  static void sessionMain(void* self);
  void sleepNow();

  std::unique_ptr<BorgesStack> stack_;
  SessionWorker worker_;
  unsigned long deadlineAt_ = 0;
  bool sleeping_ = false;
};

// Home -> Borges and Settings -> Borges. The same class in both boots: read-only status
// and preferences need no radio, and anything that does reboots into the network boot.
class BorgesActivity final : public Activity {
 public:
  BorgesActivity(GfxRenderer& renderer, MappedInputManager& input, bool networkBoot, NetPayload start);
  ~BorgesActivity() override;
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class View : uint8_t { Status, Syncing, SyncResult, PairConnecting, PairCode, PairResult };
  enum class Row : uint8_t { Connect, SyncNow, Library, OnOpen, OnSleep, Disconnect };

  static void syncMain(void* self);
  static void pairMain(void* self);
  void buildRows();
  void activate(Row row);
  void startSync();
  void startPair();
  void beginPairSession();
  void leave();
  void setView(View view);
  void renderStatus();
  void renderPairCode();
  void renderCentered(const char* primary, const char* secondary);
  void savePrefs();

  bool networkBoot_;
  NetPayload start_;
  std::unique_ptr<BorgesStack> stack_;
  std::unique_ptr<PairingFlow> pairing_;
  std::unique_ptr<ResumeDecision> decision_;
  std::unique_ptr<BookRef> bookRef_;
  std::unique_ptr<Position> local_;
  std::string bookTitle_;
  std::string bookPath_;
  View view_ = View::Status;
  std::vector<Row> rows_;
  int focus_ = 0;
  bool stackOk_ = false;
  bool fullPaint_ = true;
  SessionWorker worker_;
  ButtonNavigator navigator_;

  // Written by the session task, read by the main task.
  std::atomic<View> requestedView_{View::Status};
  std::atomic<bool> viewChanged_{false};
  std::atomic<StrId> resultText_{StrId::STR_NONE_OPT};
  std::atomic<bool> resultOk_{false};
  std::atomic<bool> needsChoice_{false};
  unsigned long autoLeaveAt_ = 0;
  char requestId_[40] = {0};
};

class BorgesLibraryActivity final : public Activity {
 public:
  BorgesLibraryActivity(GfxRenderer& renderer, MappedInputManager& input);
  ~BorgesLibraryActivity() override;
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class View : uint8_t { Loading, List, Downloading, Downloaded, DownloadFailed };
  struct Visitor;

  static void loadMain(void* self);
  static void downloadMain(void* self);
  void startLoad(uint16_t page);
  void startDownload();
  void openDownloaded();
  void leave();
  void setView(View view);
  void renderList();
  void renderDownload();
  int pageSize() const;

  std::unique_ptr<BorgesStack> stack_;
  std::unique_ptr<Catalog> catalog_;
  std::vector<CatalogEntry> entries_;
  std::vector<uint8_t> onSd_;
  uint16_t page_ = 1;
  uint16_t requestedPage_ = 1;
  bool hasNext_ = false;
  bool hasPrevious_ = false;
  int focus_ = 0;
  int downloadFocus_ = 0;
  bool wantLastRow_ = false;
  View view_ = View::Loading;
  bool fullPaint_ = true;
  CatalogEntry downloading_;
  SessionWorker worker_;
  ButtonNavigator navigator_;

  std::atomic<View> requestedView_{View::Loading};
  std::atomic<bool> viewChanged_{false};
  std::atomic<StrId> note_{StrId::STR_NONE_OPT};
  std::atomic<bool> hasNote_{false};
  std::atomic<uint32_t> downloadPercent_{0};
  std::atomic<uint32_t> shownPercent_{0};
  unsigned long lastProgressPaint_ = 0;
  class Progress;
  std::unique_ptr<Progress> progress_;
};

}  // namespace glue
}  // namespace borges

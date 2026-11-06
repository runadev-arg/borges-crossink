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
  static void sessionMain(void* session);
  void openBook();

  std::string path_;
  std::string title_;
  std::unique_ptr<BorgesStack> stack_;
  std::unique_ptr<Session> session_;  // All the session task touches. See SessionWorker.
  SessionWorker worker_;
  bool painted_ = false;
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
  bool preventAutoSleep() override { return state_ == State::Working; }

 private:
  enum class State : uint8_t { Choosing, Working, CouldNotLocate };
  static void stayMain(void* work);
  static void drainMain(void* work);
  void goThere();
  void stayHere();
  bool applyJump();
  // What the saved page maps to, for a book the ledger cannot answer for. Radio already off.
  void mapSavedPage(Position& out);
  void leave(bool success);
  void leaveWorkBehind();

  // All the session task touches. See SessionWorker.
  struct Work {
    BorgesStack* stack = nullptr;
    ResumeDecision* decision = nullptr;
    Position stayAt;
  };

  std::unique_ptr<BorgesStack> stack_;
  std::unique_ptr<ResumeDecision> decision_;
  std::unique_ptr<Position> before_;
  std::unique_ptr<Work> work_;
  std::string path_;
  std::string title_;
  Return back_;
  State state_ = State::Choosing;
  int focus_ = 0;
  bool choiceStored_ = false;
  bool jumped_ = false;  // The place was moved: the way back is worth saying.
  unsigned long linkDownAt_ = 0;
  unsigned long noticeUntil_ = 0;
  unsigned long workDeadlineAt_ = 0;
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
  static void sessionMain(void* stack);
  void sleepNow();
  void wakeNow();

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
  // Sleeping out of a manual sync started in a book wakes back into that book.
  bool isReaderActivity() const override { return readerSession(); }
  bool preventAutoSleep() override {
    return view_ == View::Connecting || view_ == View::Syncing || view_ == View::Unlinking || view_ == View::PairCode;
  }

 private:
  // Connecting is the radio coming up, Syncing is the conversation with the server.
  enum class View : uint8_t { Status, Connecting, Syncing, Unlinking, SyncResult, PairCode, PairResult };
  enum class Row : uint8_t { Connect, SyncNow, Library, OnOpen, OnSleep, Disconnect };

  struct Shared;
  bool readerSession() const { return start_ == NetPayload::ManualFromReader || start_ == NetPayload::OtherDevice; }
  static void syncMain(void* shared);
  static void pairMain(void* shared);
  static void disconnectMain(void* shared);
  void buildRows();
  void activate(Row row);
  void startSync();
  void startPair();
  void startDisconnect();
  void beginPairSession();
  void showStatus();
  void cancelWork();
  void leave();
  void setView(View view);
  void renderStatus();
  void renderPairCode();
  void renderCentered(const char* primary, const char* secondary);
  void renderResult();
  void savePrefs();

  bool networkBoot_;
  NetPayload start_;
  std::unique_ptr<Shared> s_;  // All the session task touches. See SessionWorker.
  View view_ = View::Status;
  std::vector<Row> rows_;
  int focus_ = 0;
  bool stackOk_ = false;
  bool fullPaint_ = true;
  bool cancelling_ = false;  // Painted at once: the wait for a session to stop is not a frozen screen.
  bool pairedNow_ = false;   // The next Status screen puts the focus on the library.
  SessionWorker worker_;
  ButtonNavigator navigator_;
  unsigned long autoLeaveAt_ = 0;  // 0: stays until a button is pressed.
  unsigned long hardDeadlineAt_ = 0;
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
  bool preventAutoSleep() override { return view_ == View::Loading || view_ == View::Downloading; }

 private:
  enum class View : uint8_t { Loading, List, Downloading, Downloaded, DownloadFailed };
  struct Visitor;
  struct Shared;

  static void loadMain(void* shared);
  static void downloadMain(void* shared);
  void startLoad(uint16_t page);
  void startDownload();
  void cancelAndLeave();
  void noteLink(bool up);
  void openDownloaded();
  void leave();
  void setView(View view);
  void renderList();
  void renderDownload();
  int pageSize() const;

  std::unique_ptr<Shared> s_;  // All the session task touches. See SessionWorker.
  int focus_ = 0;
  int downloadFocus_ = 0;
  bool wantLastRow_ = false;
  View view_ = View::Loading;
  bool fullPaint_ = true;
  bool listPainted_ = false;  // A list was already on the glass: the next one is a page change.
  bool cancelling_ = false;
  bool linkShown_ = false;  // What the screen says about the radio: coming up, or up.
  SessionWorker worker_;
  ButtonNavigator navigator_;
  uint32_t shownPercent_ = 0;
  unsigned long lastProgressPaint_ = 0;
  unsigned long linkDownAt_ = 0;
  unsigned long hardDeadlineAt_ = 0;  // Loading, or a cancelled download.
  unsigned long lastProgressAt_ = 0;
  uint32_t lastProgressPercent_ = 0;
  class Progress;
};

}  // namespace glue
}  // namespace borges

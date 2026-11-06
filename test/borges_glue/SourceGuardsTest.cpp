// Some rules live where only hardware could run them: a restart path, a crash report, an OTA. What
// can be held here is that the line is there, so that a rebase cannot quietly drop it.

#include <gtest/gtest.h>

#include <fstream>
#include <regex>
#include <sstream>
#include <string>

#include "PrivatePaths.h"

using borges::glue::isBorgesPrivatePath;

namespace {

std::string source(const std::string& relative) {
  std::ifstream f(std::string(BORGES_REPO_ROOT) + "/" + relative, std::ios::binary);
  EXPECT_TRUE(f.good()) << relative;
  std::stringstream all;
  all << f.rdbuf();
  return all.str();
}

}  // namespace

TEST(PrivatePaths, NeverServedWhateverTheCaseOrTheSpelling) {
  for (const char* path : {"/.crosspoint/borges", "/.crosspoint/borges/cred.a", "/.CrossPoint/BORGES/state.b",
                           "/.borges", "/.borges/borges.json", "//.crosspoint//borges/x", "/./.crosspoint/./borges/x",
                           ".crosspoint/borges/x", "/CROSSP~1/borges/cred.a", "/CROSSP~1/BORGES~1/cred.a",
                           "/BORGES~1/toto.json", "\\.crosspoint\\borges\\cred.a"}) {
    EXPECT_TRUE(isBorgesPrivatePath(path)) << path;
  }
  EXPECT_TRUE(isBorgesPrivatePath(nullptr));
}

TEST(PrivatePaths, TheBooksAndEverythingElseStayTheirs) {
  for (const char* path :
       {"/", "", "/Borges", "/Borges/ficciones.epub", "/.crosspoint", "/.crosspoint/settings.json",
        "/.crosspoint/epub_12/progress.bin", "/books/.borges", "/books/borges/x.epub", "/.crosspointer/borges"}) {
    EXPECT_FALSE(isBorgesPrivatePath(path)) << path;
  }
}

TEST(SourceGuards, EveryTransferServerAsksAboutPrivatePaths) {
  EXPECT_NE(source("src/network/CrossPointWebServer.cpp").find("isBorgesPrivatePath(path.c_str())"), std::string::npos);
  EXPECT_NE(source("src/network/WebDAVHandler.cpp").find("isBorgesPrivatePath(path.c_str())"), std::string::npos);
  EXPECT_NE(source("src/network/UsbSerialFileTransfer.cpp").find("isBorgesPrivatePath(path)"), std::string::npos);
}

TEST(SourceGuards, EveryWayBackIntoABookOnARestartHoldsIt) {
  // A book entered by any door other than a session: its progress waits for a check.
  for (const char* file :
       {"src/activities/network/CalibreConnectActivity.cpp", "src/activities/network/NearbyBookTransferActivity.cpp",
        "src/activities/network/CrossPointWebServerActivity.cpp", "src/activities/reader/KOReaderSyncActivity.cpp",
        "src/activities/reader/NearbyBookPositionSyncActivity.cpp"}) {
    const std::string text = source(file);
    const std::regex restart(R"(silentRestartToReader\()");
    for (auto it = std::sregex_iterator(text.begin(), text.end(), restart); it != std::sregex_iterator(); ++it) {
      const size_t at = static_cast<size_t>(it->position());
      const size_t from = at > 300 ? at - 300 : 0;
      EXPECT_NE(text.substr(from, at - from).find("noteOpenedWithoutSession"), std::string::npos)
          << file << " at byte " << at;
    }
  }
}

TEST(SourceGuards, TheReadingIsKeptOnlyForTheRestartThatSkipsTheExit) {
  const std::string reader = source("src/activities/reader/EpubReaderActivity.cpp");
  const size_t first = reader.find("keepReadingAcrossRestart");
  ASSERT_NE(first, std::string::npos);
  EXPECT_EQ(reader.find("keepReadingAcrossRestart", first + 1), std::string::npos)
      << "saveProgressBeforeRestart leads to onExit, which finalizes: a copy left behind invents a session";
  EXPECT_LT(first, reader.find("silentRestartToReader();", first));
}

TEST(SourceGuards, ACrashReportOnTheCardHoldsNoStackMemory) {
  const std::string hal = source("lib/hal/HalSystem.cpp");
  const size_t dump = hal.find("Stack memory:");
  ASSERT_NE(dump, std::string::npos);
  const size_t guard = hal.rfind("#if defined(CROSSINK_CRASH_REPORT_STACK)", dump);
  ASSERT_NE(guard, std::string::npos);
  EXPECT_EQ(hal.substr(guard, dump - guard).find("#endif"), std::string::npos);
  EXPECT_EQ(source("platformio.ini").find("CROSSINK_CRASH_REPORT_STACK"), std::string::npos);
}

TEST(SourceGuards, AnOtaDownloadIsCappedAndCanBeCancelled) {
  EXPECT_NE(source("src/network/OtaUpdater.cpp").find("downloadOptions.maxBytes"), std::string::npos);
  EXPECT_NE(source("src/activities/settings/OtaUpdateActivity.cpp").find("&cancelRequested"), std::string::npos);
}

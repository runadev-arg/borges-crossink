#include "TestSupport.h"

using namespace borges;
using namespace borges_test;

namespace {

std::string md5Of(const std::string& content) {
  MemFs fs;
  fs.put("/f", content);
  BookId id;
  EXPECT_TRUE(partialMd5(fs, "/f", id));
  return id.hex;
}

std::string book(const std::string& id, const std::string& md5, const std::string& title, const std::string& filename = "libro.epub",
                 long size = 1000) {
  return "{\"id\":\"" + id + "\",\"document_id\":\"d\",\"sha256\":\"" + std::string(64, 'a') + "\",\"koreader_partial_md5\":\"" + md5 +
         "\",\"original_filename\":\"" + filename + "\",\"format\":\"epub\",\"title\":\"" + title +
         "\",\"author\":\"Borges\",\"status\":\"ready\",\"file_size\":" + std::to_string(size) + ",\"download_url\":\"/x\"}";
}

std::string page(const std::string& books, int number = 1, bool hasNext = false, bool hasPrevious = false) {
  return "{\"books\":[" + books + "],\"page\":" + std::to_string(number) + ",\"page_size\":20,\"has_next\":" + (hasNext ? "true" : "false") +
         ",\"has_previous\":" + (hasPrevious ? "true" : "false") + ",\"hidden_missing_files\":0,\"reconciliation\":{\"strategy\":\"additive\"}}";
}

class Collect : public ICatalogVisitor {
 public:
  std::vector<CatalogEntry> entries;
  void onBook(uint16_t, const CatalogEntry& e) override { entries.push_back(e); }
};

class CatalogTest : public testing::Test {
 protected:
  void SetUp() override {
    d.boot();
    d.http.handler = nullptr;
    catalog = std::make_unique<Catalog>(*d.store, d.fs, d.http, d.clock, d.cancel);
    d.fs.mkdir("/Books");
  }
  std::vector<CatalogEntry> cached(uint16_t number = 1) {
    Collect c;
    CatalogPage info;
    EXPECT_TRUE(catalog->loadPage(number, info, c));
    return c.entries;
  }
  Device d;
  std::unique_ptr<Catalog> catalog;
};

}  // namespace

TEST_F(CatalogTest, FetchesParsesAndCaches) {
  d.http.script.push_back(jsonReply(200, page(book("b1", kMd5A, "Ficciones") + "," + book("b2", kMd5B, "El Aleph", "aleph.epub", 2000), 1, true)));
  CatalogPage info;
  ASSERT_EQ(catalog->fetchPage(1, 20, info), CatalogResult::Ok);
  EXPECT_EQ(info.count, 2);
  EXPECT_TRUE(info.hasNext);
  EXPECT_FALSE(info.hasPrevious);
  EXPECT_EQ(d.http.log[0].path, "/api/library/plugin/books?page=1&limit=20");
  EXPECT_EQ(d.http.log[0].headers.at("Authorization"), "Bearer toto_SECRET_TOKEN_VALUE");

  const auto entries = cached();
  ASSERT_EQ(entries.size(), 2u);
  EXPECT_STREQ(entries[0].id, "b1");
  EXPECT_STREQ(entries[0].title, "Ficciones");
  EXPECT_STREQ(entries[0].author, "Borges");
  EXPECT_STREQ(entries[0].book.hex, kMd5A);
  EXPECT_STREQ(entries[1].fileName, "aleph.epub");
  EXPECT_EQ(entries[1].fileSize, 2000u);

  // Offline, the cache still opens, with its paging flags.
  CatalogPage again;
  Collect c;
  ASSERT_TRUE(catalog->loadPage(1, again, c));
  EXPECT_TRUE(again.hasNext);
  EXPECT_EQ(again.count, 2);
}

TEST_F(CatalogTest, PagesAreCachedSeparately) {
  d.http.script.push_back(jsonReply(200, page(book("b1", kMd5A, "Uno"), 1, true)));
  d.http.script.push_back(jsonReply(200, page(book("b2", kMd5B, "Dos"), 2, false, true)));
  CatalogPage info;
  ASSERT_EQ(catalog->fetchPage(1, 20, info), CatalogResult::Ok);
  ASSERT_EQ(catalog->fetchPage(2, 20, info), CatalogResult::Ok);
  EXPECT_TRUE(info.hasPrevious);
  EXPECT_STREQ(cached(1)[0].title, "Uno");
  EXPECT_STREQ(cached(2)[0].title, "Dos");
  Collect c;
  EXPECT_FALSE(catalog->loadPage(3, info, c));
}

TEST_F(CatalogTest, LimitIsBounded) {
  d.http.script.push_back(jsonReply(200, page("")));
  CatalogPage info;
  catalog->fetchPage(1, 60000, info);
  EXPECT_EQ(d.http.log[0].path, "/api/library/plugin/books?page=1&limit=50");
}

TEST_F(CatalogTest, HostileEntriesAreDroppedAndTheRestSurvive) {
  std::string books = book("good", kMd5A, "Buena");
  const std::vector<std::string> hostile = {
      book("../../etc/passwd", kMd5A, "traversal in id"),
      book("id with space", kMd5A, "space in id"),
      book("x?y=1", kMd5A, "query in id"),
      book(std::string(500, 'z'), kMd5A, "long id"),
      book("b3", "nothex", "bad md5"),
      book("b4", std::string(31, 'a'), "short md5"),
      book("b5", kMd5A, "no size", "a.epub", 0),
      book("b6", kMd5A, "negative size", "a.epub", -5),
      "5",
      "\"text\"",
      "null",
      "[]",
      "{}",
      "{\"id\":7,\"koreader_partial_md5\":8,\"title\":9}",
      "{\"id\":\"b7\",\"koreader_partial_md5\":\"" + std::string(kMd5A) + "\",\"file_size\":\"big\"}",
      "{\"id\":\"b8\",\"koreader_partial_md5\":\"" + std::string(kMd5A) + "\",\"file_size\":100,\"status\":\"processing\"}",
      "{\"id\":\"b9\",\"koreader_partial_md5\":\"" + std::string(kMd5A) + "\",\"file_size\":100,\"sha256\":\"short\"}",
  };
  for (const std::string& h : hostile) books += "," + h;
  books += "," + book("good2", kMd5B, "Otra");
  d.http.script.push_back(jsonReply(200, page(books)));
  CatalogPage info;
  ASSERT_EQ(catalog->fetchPage(1, 50, info), CatalogResult::Ok);
  EXPECT_EQ(info.count, 2);
  const auto entries = cached();
  ASSERT_EQ(entries.size(), 2u);
  EXPECT_STREQ(entries[0].id, "good");
  EXPECT_STREQ(entries[1].id, "good2");
}

TEST_F(CatalogTest, LongFieldsAreTruncatedWithoutSplittingACharacter) {
  std::string title;
  for (int i = 0; i < 200; ++i) title += "\xC3\xA9";
  d.http.script.push_back(jsonReply(200, page(book("b1", kMd5A, title))));
  CatalogPage info;
  ASSERT_EQ(catalog->fetchPage(1, 20, info), CatalogResult::Ok);
  const auto entries = cached();
  const size_t n = strlen(entries[0].title);
  EXPECT_LT(n, sizeof(entries[0].title));
  EXPECT_EQ(n % 2, 0u);
  EXPECT_EQ(static_cast<uint8_t>(entries[0].title[n - 2]), 0xC3);
}

TEST_F(CatalogTest, ServerSendingMoreThanAskedIsCutAtTheLimit) {
  std::string books;
  for (int i = 0; i < 500; ++i) {
    if (i) books += ",";
    books += book("b" + std::to_string(i), kMd5A, "T");
  }
  d.http.script.push_back(jsonReply(200, page(books)));
  CatalogPage info;
  ASSERT_EQ(catalog->fetchPage(1, 10, info), CatalogResult::Ok);
  EXPECT_EQ(info.count, 10);
  EXPECT_EQ(cached().size(), 10u);
}

TEST_F(CatalogTest, BadDocumentsKeepTheOldCache) {
  d.http.script.push_back(jsonReply(200, page(book("b1", kMd5A, "Buena"))));
  CatalogPage info;
  ASSERT_EQ(catalog->fetchPage(1, 20, info), CatalogResult::Ok);

  const std::vector<std::string> bad = {
      "",
      "not json",
      "{\"books\":[",
      "{\"books\":\"none\",\"page\":1}",
      "{\"page\":1}",
      page(book("b2", kMd5A, "Otra pagina"), 7),  // Not the page we asked for.
      "{\"books\":[],\"page\":\"1\"}",
      page("") + " trailing",
  };
  for (const std::string& body : bad) {
    d.http.script.push_back(jsonReply(200, body));
    EXPECT_EQ(catalog->fetchPage(1, 20, info), CatalogResult::Malformed) << body;
    ASSERT_EQ(cached().size(), 1u) << body;
    EXPECT_STREQ(cached()[0].id, "b1");
  }
}

TEST_F(CatalogTest, CorruptCacheIsAMissNotGarbage) {
  d.http.script.push_back(jsonReply(200, page(book("b1", kMd5A, "Buena"))));
  CatalogPage info;
  ASSERT_EQ(catalog->fetchPage(1, 20, info), CatalogResult::Ok);
  const std::string path = "/.crosspoint/borges/catalog/p1";
  ASSERT_TRUE(d.fs.has(path));
  d.fs.corrupt(path, 30);
  Collect c;
  EXPECT_FALSE(catalog->loadPage(1, info, c));
  EXPECT_TRUE(c.entries.empty());
  d.fs.put(path, "short");
  EXPECT_FALSE(catalog->loadPage(1, info, c));
}

TEST_F(CatalogTest, ErrorsAreClassified) {
  CatalogPage info;
  d.http.script.push_back(apiError(401, "invalid_credentials"));
  EXPECT_EQ(catalog->fetchPage(1, 20, info), CatalogResult::NeedsRelink);
  d.http.script.push_back(apiError(500, "boom"));
  EXPECT_EQ(catalog->fetchPage(1, 20, info), CatalogResult::ServerError);
  EXPECT_STREQ(catalog->lastError().code, "boom");
  d.http.script.push_back(dropped());
  d.http.script.push_back(dropped());
  EXPECT_EQ(catalog->fetchPage(1, 20, info), CatalogResult::Offline);
  d.cancel.flag = true;
  EXPECT_EQ(catalog->fetchPage(1, 20, info), CatalogResult::Cancelled);
}

TEST_F(CatalogTest, RouteErrorsInTheLibraryShapeAreDecoded) {
  d.http.script.push_back(jsonReply(404, "{\"error\":\"Not found\",\"code\":\"book_not_found\"}"));
  CatalogPage info;
  EXPECT_EQ(catalog->fetchPage(1, 20, info), CatalogResult::ServerError);
  EXPECT_STREQ(catalog->lastError().code, "book_not_found");
  EXPECT_STREQ(catalog->lastError().detail, "Not found");
}

TEST(SafeFileName, MakesAnythingSafeForFat) {
  const struct {
    const char* input;
    const char* expected;
  } cases[] = {
      {"libro.epub", "libro.epub"},
      {"LIBRO.EPUB", "LIBRO.epub"},
      {"libro", "libro.epub"},
      {"../../etc/passwd", "passwd.epub"},
      {"..\\..\\windows\\system32", "system32.epub"},
      {"a<b>c:d\"e|f?g*h", "a_b_c_d_e_f_g_h.epub"},
      {"...hidden", "hidden.epub"},
      {"   spaced", "spaced.epub"},
      {"trailing. .", "trailing.epub"},
      {"line\nbreak\ttab", "linebreaktab.epub"},
      {"", "fallback-id.epub"},
      {"/", "fallback-id.epub"},
      {"...", "fallback-id.epub"},
      {"Tl\xC3\xB6n.epub", "Tl\xC3\xB6n.epub"},
  };
  for (const auto& c : cases) {
    char out[64];
    safeFileName(c.input, "fallback-id", out, sizeof(out));
    EXPECT_STREQ(out, c.expected) << c.input;
  }
}

TEST(SafeFileName, LongNamesAreBoundedAndKeepTheExtension) {
  std::string name;
  for (int i = 0; i < 100; ++i) name += "\xC3\xA9";
  char out[64];
  safeFileName(name.c_str(), "x", out, sizeof(out));
  EXPECT_LT(strlen(out), sizeof(out));
  EXPECT_EQ(std::string(out).substr(strlen(out) - 5), ".epub");
  EXPECT_EQ((strlen(out) - 5) % 2, 0u) << "no split character";
}

// --- Partial MD5 ------------------------------------------------------------------

TEST(PartialMd5, MatchesKoreadersAlgorithm) {
  // Reference values computed independently with Python's hashlib.
  std::string small(5000, '\0');
  for (size_t i = 0; i < small.size(); ++i) small[i] = static_cast<char>(i % 251);
  std::string large(300000, '\0');
  for (size_t i = 0; i < large.size(); ++i) large[i] = static_cast<char>((i * 7) % 256);
  MemFs fs;
  fs.put("/small", small);
  fs.put("/large", large);
  BookId id;
  ASSERT_TRUE(partialMd5(fs, "/small", id));
  EXPECT_STREQ(id.hex, "e77dcca7f22a949ae8492c260ca19f32");
  ASSERT_TRUE(partialMd5(fs, "/large", id));
  EXPECT_STREQ(id.hex, "6d4913ec2305d61266af0a7788e8fe58");
}

// --- Download ---------------------------------------------------------------------

namespace {

class DownloadTest : public testing::Test {
 protected:
  void SetUp() override {
    d.boot();
    d.http.handler = nullptr;
    catalog = std::make_unique<Catalog>(*d.store, d.fs, d.http, d.clock, d.cancel);
    content.resize(5000);
    for (size_t i = 0; i < content.size(); ++i) content[i] = static_cast<char>(i % 251);
    entry = CatalogEntry{};
    copyText(entry.id, sizeof(entry.id), "b1");
    ASSERT_TRUE(entry.book.setHex(md5Of(content).c_str()));
    entry.fileSize = static_cast<uint32_t>(content.size());
    copyText(entry.fileName, sizeof(entry.fileName), "libro.epub");
  }
  Reply full() {
    Reply r = jsonReply(200, content);
    r.etag = "\"etag-1\"";
    r.koreaderMd5 = entry.book.hex;
    return r;
  }
  Device d;
  std::unique_ptr<Catalog> catalog;
  std::string content;
  CatalogEntry entry;
};

}  // namespace

TEST_F(DownloadTest, DownloadsVerifiesAndPublishes) {
  d.http.script.push_back(full());
  EXPECT_FALSE(catalog->isOnSd(entry, "/Books"));
  EXPECT_EQ(catalog->download(entry, "/Books", nullptr, 60000), DownloadResult::Done);
  EXPECT_EQ(d.fs.content("/Books/libro.epub"), content);
  EXPECT_FALSE(d.fs.has("/Books/libro.epub.part"));
  EXPECT_FALSE(d.fs.has("/Books/libro.epub.etag"));
  EXPECT_TRUE(catalog->isOnSd(entry, "/Books"));
  EXPECT_EQ(d.http.log[0].path, "/api/library/plugin/books/b1/download");
  EXPECT_EQ(d.http.log[0].headers.count("Range"), 0u);
}

TEST_F(DownloadTest, AlreadyThereDoesNoNetwork) {
  d.fs.put("/Books/libro.epub", content);
  EXPECT_EQ(catalog->download(entry, "/Books", nullptr, 60000), DownloadResult::AlreadyPresent);
  EXPECT_TRUE(d.http.log.empty());
}

TEST_F(DownloadTest, ResumesWithRangeAfterADrop) {
  Reply cut = full();
  cut.dropAfterBytes = 2000;
  d.http.script.push_back(cut);
  EXPECT_EQ(catalog->download(entry, "/Books", nullptr, 60000), DownloadResult::Interrupted);
  ASSERT_TRUE(d.fs.has("/Books/libro.epub.part"));
  EXPECT_EQ(d.fs.content("/Books/libro.epub.part").size(), 2000u);
  EXPECT_FALSE(d.fs.has("/Books/libro.epub"));

  Reply rest = jsonReply(206, content.substr(2000));
  rest.koreaderMd5 = entry.book.hex;
  d.http.script.push_back(rest);
  EXPECT_EQ(catalog->download(entry, "/Books", nullptr, 60000), DownloadResult::Done);
  EXPECT_EQ(d.http.log[1].headers.at("Range"), "bytes=2000-");
  EXPECT_EQ(d.http.log[1].headers.at("If-Range"), "\"etag-1\"");
  EXPECT_EQ(d.fs.content("/Books/libro.epub"), content);
}

TEST_F(DownloadTest, ServerIgnoringRangeStartsOverCleanly) {
  Reply cut = full();
  cut.dropAfterBytes = 2000;
  d.http.script.push_back(cut);
  catalog->download(entry, "/Books", nullptr, 60000);

  // The file changed: If-Range failed and the whole thing comes with 200.
  d.http.script.push_back(full());
  EXPECT_EQ(catalog->download(entry, "/Books", nullptr, 60000), DownloadResult::Interrupted);
  EXPECT_FALSE(d.fs.has("/Books/libro.epub.part")) << "old and new bytes must never mix";

  d.http.script.push_back(full());
  EXPECT_EQ(catalog->download(entry, "/Books", nullptr, 60000), DownloadResult::Done);
  EXPECT_EQ(d.fs.content("/Books/libro.epub"), content);
}

TEST_F(DownloadTest, AnErrorPageIsNeverAppendedToTheFile) {
  Reply cut = full();
  cut.dropAfterBytes = 2000;
  d.http.script.push_back(cut);
  catalog->download(entry, "/Books", nullptr, 60000);
  d.http.script.push_back(apiError(500, "boom"));
  EXPECT_EQ(catalog->download(entry, "/Books", nullptr, 60000), DownloadResult::ServerError);
  EXPECT_EQ(d.fs.content("/Books/libro.epub.part"), content.substr(0, 2000));
}

TEST_F(DownloadTest, WrongContentIsRejected) {
  std::string wrong = content;
  wrong[10] = 'X';
  Reply r = jsonReply(200, wrong);
  r.etag = "\"e\"";
  r.koreaderMd5 = entry.book.hex;
  d.http.script.push_back(r);
  EXPECT_EQ(catalog->download(entry, "/Books", nullptr, 60000), DownloadResult::IntegrityFailed);
  EXPECT_FALSE(d.fs.has("/Books/libro.epub"));
  EXPECT_FALSE(d.fs.has("/Books/libro.epub.part"));
}

TEST_F(DownloadTest, HeaderDisagreeingWithTheManifestIsRejected) {
  Reply r = full();
  r.koreaderMd5 = kMd5A;
  d.http.script.push_back(r);
  EXPECT_EQ(catalog->download(entry, "/Books", nullptr, 60000), DownloadResult::IntegrityFailed);
  EXPECT_FALSE(d.fs.has("/Books/libro.epub"));
}

TEST_F(DownloadTest, RevokedDeviceAndCancelAreReported) {
  d.http.script.push_back(apiError(401, "invalid_credentials"));
  EXPECT_EQ(catalog->download(entry, "/Books", nullptr, 60000), DownloadResult::NeedsRelink);
  d.cancel.flag = true;
  EXPECT_EQ(catalog->download(entry, "/Books", nullptr, 60000), DownloadResult::Cancelled);
}

TEST_F(DownloadTest, ProgressIsReported) {
  class Meter : public IDownloadProgress {
   public:
    uint64_t last = 0;
    uint64_t total = 0;
    void onProgress(uint64_t received, uint64_t t) override {
      EXPECT_GE(received, last);
      last = received;
      total = t;
    }
  } meter;
  d.http.script.push_back(full());
  EXPECT_EQ(catalog->download(entry, "/Books", &meter, 60000), DownloadResult::Done);
  EXPECT_EQ(meter.last, content.size());
  EXPECT_EQ(meter.total, content.size());
}

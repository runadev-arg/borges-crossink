#include "Catalog.h"

#include <cstring>

#include "JsonStream.h"

namespace borges {

namespace {

constexpr uint8_t kCacheMagic[4] = {'B', 'C', 'P', '2'};
constexpr size_t kCacheTailBytes = 11;  // count, flags, page, account epoch, crc
constexpr const char* kCacheDir = "/.crosspoint/borges/catalog";
constexpr const char* kBooksPath = "/api/library/plugin/books";

// Record: the fields of CatalogEntry that are not derived.
constexpr size_t kRecordBytes = 40 + 33 + 65 + 4 + 96 + 64 + 64;

bool isSafeIdChar(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '_';
}

void encodeEntry(const CatalogEntry& e, uint8_t* out) {
  memset(out, 0, kRecordBytes);
  size_t o = 0;
  memcpy(out + o, e.id, sizeof(e.id));
  o += sizeof(e.id);
  memcpy(out + o, e.book.hex, sizeof(e.book.hex));
  o += sizeof(e.book.hex);
  memcpy(out + o, e.sha256, sizeof(e.sha256));
  o += sizeof(e.sha256);
  ByteWriter w(out + o, 4);
  w.u32(e.fileSize);
  o += 4;
  memcpy(out + o, e.title, sizeof(e.title));
  o += sizeof(e.title);
  memcpy(out + o, e.author, sizeof(e.author));
  o += sizeof(e.author);
  memcpy(out + o, e.fileName, sizeof(e.fileName));
}

void decodeEntry(const uint8_t* in, CatalogEntry& e) {
  size_t o = 0;
  memcpy(e.id, in + o, sizeof(e.id));
  e.id[sizeof(e.id) - 1] = '\0';
  o += sizeof(e.id);
  char hex[33];
  memcpy(hex, in + o, sizeof(hex));
  hex[32] = '\0';
  e.book = BookId{};
  e.book.setHex(hex);
  o += sizeof(hex);
  memcpy(e.sha256, in + o, sizeof(e.sha256));
  e.sha256[sizeof(e.sha256) - 1] = '\0';
  o += sizeof(e.sha256);
  ByteReader r(in + o, 4);
  e.fileSize = r.u32();
  o += 4;
  memcpy(e.title, in + o, sizeof(e.title));
  e.title[sizeof(e.title) - 1] = '\0';
  o += sizeof(e.title);
  memcpy(e.author, in + o, sizeof(e.author));
  e.author[sizeof(e.author) - 1] = '\0';
  o += sizeof(e.author);
  memcpy(e.fileName, in + o, sizeof(e.fileName));
  e.fileName[sizeof(e.fileName) - 1] = '\0';
}

// Only the body the request actually asked for reaches the file. An error page, or a
// whole file answering a Range request, must not be appended to the bytes already here.
class FileSink : public IHttpSink {
 public:
  FileSink(IFs& fs, IFs::Handle handle, IDownloadProgress* progress, uint64_t base, uint64_t total)
      : fs_(fs), handle_(handle), progress_(progress), received_(base), total_(total) {}
  void onStatus(int status) override { accept_ = base() == 0 ? (status == 200) : (status == 206); }
  bool onBody(const uint8_t* data, size_t len) override {
    if (!accept_) return true;
    if (fs_.write(handle_, data, len) != static_cast<int32_t>(len)) {
      failed_ = true;
      return false;
    }
    received_ += len;
    if (progress_) progress_->onProgress(received_, total_);
    return true;
  }
  bool failed() const { return failed_; }

 private:
  uint64_t base() const { return received_; }
  IFs& fs_;
  IFs::Handle handle_;
  IDownloadProgress* progress_;
  uint64_t received_;
  uint64_t total_;
  bool accept_ = false;
  bool failed_ = false;
};

}  // namespace

void safeFileName(const char* suggested, const char* fallback, char* out, size_t cap) {
  static constexpr char kSuffix[] = ".epub";
  static constexpr size_t kSuffixLen = sizeof(kSuffix) - 1;
  if (cap <= kSuffixLen + 1) {
    if (cap) out[0] = '\0';
    return;
  }
  const size_t room = cap - kSuffixLen - 1;
  size_t n = 0;
  // Only the last path component is ever a name.
  const char* start = suggested ? suggested : "";
  for (const char* p = start; *p; ++p) {
    if (*p == '/' || *p == '\\') start = p + 1;
  }
  for (const char* p = start; *p && n < room; ++p) {
    const unsigned char c = static_cast<unsigned char>(*p);
    if (c < 0x20 || c == 0x7F) continue;
    if (strchr("<>:\"|?*", c)) {
      out[n++] = '_';
      continue;
    }
    if (n == 0 && (c == '.' || c == ' ')) continue;
    out[n++] = static_cast<char>(c);
  }
  // Do not cut a UTF-8 sequence.
  if (n == room) {
    while (n > 0 && (static_cast<uint8_t>(out[n - 1]) & 0xC0) == 0x80) --n;
    if (n > 0 && static_cast<uint8_t>(out[n - 1]) >= 0xC0) --n;
  }
  // The name may already end in the extension, in any case.
  if (n >= kSuffixLen) {
    bool has = true;
    for (size_t i = 0; i < kSuffixLen && has; ++i) {
      char c = out[n - kSuffixLen + i];
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
      has = c == kSuffix[i];
    }
    if (has) n -= kSuffixLen;
  }
  while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '.')) --n;
  if (n == 0 && fallback) {
    for (const char* p = fallback; *p && n < room; ++p) {
      out[n++] = isSafeIdChar(*p) ? *p : '_';
    }
  }
  if (n == 0) out[n++] = '_';
  memcpy(out + n, kSuffix, kSuffixLen + 1);
}

bool partialMd5(IFs& fs, const char* path, BookId& out) {
  out = BookId{};
  if (!fs.exists(path)) return false;
  File f(fs, path, IFs::Mode::Read);
  if (!f) return false;
  const int64_t rawSize = f.size();
  if (rawSize < 0) return false;
  const uint64_t size = static_cast<uint64_t>(rawSize);
  Md5 md5;
  uint8_t chunk[256];
  for (int i = -1; i <= 10; ++i) {
    const uint64_t offset = i < 0 ? 0 : (1024ULL << (2 * i));
    if (offset >= size) continue;
    if (!f.seek(static_cast<uint32_t>(offset))) return false;
    uint64_t left = size - offset < 1024 ? size - offset : 1024;
    while (left) {
      const size_t want = left < sizeof(chunk) ? static_cast<size_t>(left) : sizeof(chunk);
      if (!f.readExact(chunk, want)) return false;
      md5.update(chunk, want);
      left -= want;
    }
  }
  uint8_t digest[16];
  md5.finish(digest);
  out.setRaw(digest);
  return true;
}

Catalog::Catalog(Store& store, IFs& fs, IHttp& http, IClock& clock, ICancel& cancel)
    : store_(store), fs_(fs), clock_(clock), api_(fs, http, clock, cancel) {}

void Catalog::note(const ApiResult& r, const char* code) {
  error_.stage = SyncStage::Catalog;
  error_.httpStatus = static_cast<int16_t>(r.status);
  error_.transportStage = r.stage;
  error_.transportCode = r.transportCode;
  error_.atEpoch = clock_.wallNow().epochSec;
  copyText(error_.code, sizeof(error_.code), r.code[0] ? r.code : code);
  copyText(error_.detail, sizeof(error_.detail), r.message);
  copyText(error_.requestId, sizeof(error_.requestId), r.requestId);
}

void Catalog::cachePath(uint16_t page, char* out) const {
  const size_t n = strlen(kCacheDir);
  memcpy(out, kCacheDir, n);
  memcpy(out + n, "/p", 2);
  formatU64(page, out + n + 2);
}

CatalogResult Catalog::fetchPage(uint16_t page, uint16_t limit, CatalogPage& out, uint32_t budgetMs) {
  out = CatalogPage{};
  error_.clear();
  if (page == 0) page = 1;
  if (limit == 0) limit = 1;
  if (limit > kMaxPageSize) limit = kMaxPageSize;

  char path[80];
  JsonWriter p(path, sizeof(path));
  p.raw(kBooksPath);
  p.raw("?page=");
  char num[21];
  formatU64(page, num);
  p.raw(num);
  p.raw("&limit=");
  formatU64(limit, num);
  p.raw(num);
  path[p.size()] = '\0';

  api_.configure(store_.creds().baseUrl, store_.creds().token);
  api_.beginSession(budgetMs);
  const ApiResult r = api_.call("GET", path, nullptr, 0);
  if (r.kind != ApiKind::Ok) {
    note(r, r.kind == ApiKind::Transport ? "transport" : "catalog_failed");
    if (r.kind == ApiKind::Cancelled || r.kind == ApiKind::Budget) return CatalogResult::Cancelled;
    if (r.kind == ApiKind::Transport) return CatalogResult::Offline;
    if (r.kind == ApiKind::LocalIo) return CatalogResult::LocalError;
    if (r.status == 401 || r.status == 403) return CatalogResult::NeedsRelink;
    return CatalogResult::ServerError;
  }

  // Validate the document before touching the cache: a bad page must not replace a
  // good one.
  File src(fs_, kSpoolPath, IFs::Mode::Read);
  if (!src) return CatalogResult::LocalError;
  FileByteSource source(src);
  JsonScanner scan(source);
  uint32_t booksOffset = 0;
  bool hasBooks = false;
  int64_t servedPage = -1;
  char key[24];
  if (!scan.enterObject()) {
    note(r, "malformed_response");
    return CatalogResult::Malformed;
  }
  while (scan.nextMember(key, sizeof(key))) {
    if (strcmp(key, "books") == 0) {
      if (scan.peek() == JsonType::Array) {
        hasBooks = true;
        booksOffset = scan.offset();
      }
      scan.skipValue();
    } else if (strcmp(key, "page") == 0) {
      scan.readInt(servedPage);
    } else if (strcmp(key, "has_next") == 0) {
      scan.readBool(out.hasNext);
    } else if (strcmp(key, "has_previous") == 0) {
      scan.readBool(out.hasPrevious);
    } else {
      scan.skipValue();
    }
  }
  if (scan.failed() || !scan.atEnd() || !hasBooks || servedPage != page) {
    note(r, "malformed_response");
    return CatalogResult::Malformed;
  }

  char finalPath[AtomicFile::kMaxPath];
  cachePath(page, finalPath);
  char tempPath[AtomicFile::kMaxPath + 8];
  const size_t n = strlen(finalPath);
  memcpy(tempPath, finalPath, n);
  memcpy(tempPath + n, ".new", 5);

  File dst(fs_, tempPath, IFs::Mode::WriteTrunc);
  if (!dst) return CatalogResult::LocalError;
  // Header rewritten by content, not by position: count goes in a trailing record so
  // the cache can be written front to back with no seek in write mode.
  dst.writeAll(kCacheMagic, 4);
  uint32_t crc = crc32Begin();
  uint16_t count = 0;
  bool ioOk = true;
  CatalogEntry entry;
  uint8_t record[kRecordBytes];

  scan.seekTo(booksOffset);
  if (!scan.enterArray()) return CatalogResult::Malformed;
  while (scan.nextElement()) {
    entry = CatalogEntry{};
    char md5[40] = {0};
    char filename[160] = {0};
    int64_t fileSize = 0;
    bool idCut = false;
    bool shaCut = false;
    char status[12] = {0};
    bool ready = true;
    if (scan.enterObject()) {
      while (scan.nextMember(key, sizeof(key))) {
        if (strcmp(key, "id") == 0) {
          scan.readString(entry.id, sizeof(entry.id), &idCut);
        } else if (strcmp(key, "koreader_partial_md5") == 0) {
          scan.readString(md5, sizeof(md5));
        } else if (strcmp(key, "sha256") == 0) {
          scan.readString(entry.sha256, sizeof(entry.sha256), &shaCut);
        } else if (strcmp(key, "title") == 0) {
          scan.readString(entry.title, sizeof(entry.title));
        } else if (strcmp(key, "author") == 0) {
          scan.readString(entry.author, sizeof(entry.author));
        } else if (strcmp(key, "original_filename") == 0) {
          scan.readString(filename, sizeof(filename));
        } else if (strcmp(key, "file_size") == 0) {
          scan.readInt(fileSize);
        } else if (strcmp(key, "status") == 0) {
          scan.readString(status, sizeof(status));
          ready = status[0] == '\0' || strcmp(status, "ready") == 0;
        } else {
          scan.skipValue();
        }
      }
    } else {
      scan.skipValue();
      continue;
    }
    if (scan.failed()) break;

    // Past the limit we asked for, the server is not honest. Stop keeping, keep reading.
    if (count >= limit) continue;

    bool valid = entry.id[0] && !idCut && !shaCut && ready && entry.book.setHex(md5);
    for (const char* c = entry.id; valid && *c; ++c) valid = isSafeIdChar(*c);
    if (valid && entry.sha256[0]) valid = strlen(entry.sha256) == 64 && isHex(entry.sha256, 64);
    if (valid) valid = fileSize > 0 && fileSize < 0x7FFFFFFFLL;
    if (!valid) continue;
    entry.fileSize = static_cast<uint32_t>(fileSize);
    if (!entry.title[0]) copyText(entry.title, sizeof(entry.title), filename);
    safeFileName(filename[0] ? filename : entry.title, entry.id, entry.fileName, sizeof(entry.fileName));

    encodeEntry(entry, record);
    crc = crc32Update(crc, record, kRecordBytes);
    ioOk = ioOk && dst.writeAll(record, kRecordBytes);
    ++count;
  }
  if (scan.failed()) {
    dst.close();
    fs_.remove(tempPath);
    note(r, "malformed_response");
    return CatalogResult::Malformed;
  }

  uint8_t tail[kCacheTailBytes];
  ByteWriter w(tail, sizeof(tail));
  w.u16(count);
  w.u8((out.hasNext ? 1 : 0) | (out.hasPrevious ? 2 : 0));
  w.u16(page);
  w.u16(store_.state().accountEpoch);
  w.u32(crc32End(crc));
  ioOk = ioOk && dst.writeAll(tail, sizeof(tail)) && dst.flush();
  dst.close();
  if (!ioOk) {
    fs_.remove(tempPath);
    return CatalogResult::LocalError;
  }
  // A cache: losing it between these two lines only costs a refetch.
  fs_.remove(finalPath);
  fs_.rename(tempPath, finalPath);

  out.page = page;
  out.count = count;
  return CatalogResult::Ok;
}

bool Catalog::loadPage(uint16_t page, CatalogPage& out, ICatalogVisitor& visitor) {
  out = CatalogPage{};
  char path[AtomicFile::kMaxPath];
  cachePath(page, path);
  if (!fs_.exists(path)) return false;
  File f(fs_, path, IFs::Mode::Read);
  if (!f) return false;
  const int64_t size = f.size();
  if (size < static_cast<int64_t>(4 + kCacheTailBytes)) return false;
  uint8_t magic[4];
  if (!f.readExact(magic, 4) || memcmp(magic, kCacheMagic, 4) != 0) return false;
  const uint32_t bodyBytes = static_cast<uint32_t>(size) - 4 - kCacheTailBytes;
  if (bodyBytes % kRecordBytes != 0) return false;
  uint8_t tail[kCacheTailBytes];
  if (!f.seek(4 + bodyBytes) || !f.readExact(tail, kCacheTailBytes)) return false;
  ByteReader r(tail, kCacheTailBytes);
  const uint16_t count = r.u16();
  const uint8_t flags = r.u8();
  const uint16_t storedPage = r.u16();
  const uint16_t epoch = r.u16();
  const uint32_t expect = r.u32();
  // Another account's list is not this one's, even offline.
  if (count != bodyBytes / kRecordBytes || storedPage != page || epoch != store_.state().accountEpoch) return false;

  uint8_t record[kRecordBytes];
  // Verify everything first, so a visitor never sees half of a corrupt page.
  uint32_t crc = crc32Begin();
  if (!f.seek(4)) return false;
  for (uint16_t i = 0; i < count; ++i) {
    if (!f.readExact(record, kRecordBytes)) return false;
    crc = crc32Update(crc, record, kRecordBytes);
  }
  if (crc32End(crc) != expect) return false;

  if (!f.seek(4)) return false;
  CatalogEntry entry;
  for (uint16_t i = 0; i < count; ++i) {
    if (!f.readExact(record, kRecordBytes)) return false;
    decodeEntry(record, entry);
    visitor.onBook(i, entry);
  }
  out.page = page;
  out.count = count;
  out.hasNext = (flags & 1) != 0;
  out.hasPrevious = (flags & 2) != 0;
  return true;
}

namespace {

bool joinPath(const char* dir, const char* name, char* out, size_t cap) {
  JsonWriter w(out, cap - 1);
  w.raw(dir);
  w.raw("/");
  w.raw(name);
  if (!w.ok()) return false;
  out[w.size()] = '\0';
  return true;
}

// <stem>-<8 hex of the partial MD5>.epub, for when the plain name is another book's.
// Fits CatalogEntry::fileName and stays stable, so a resume finds its own .part.
void hashedFileName(const CatalogEntry& entry, char* out, size_t cap) {
  static constexpr size_t kTail = 1 + 8 + 5;  // -xxxxxxxx.epub
  const size_t nameLen = strlen(entry.fileName);
  const size_t stemLen = nameLen > 5 ? nameLen - 5 : 0;
  const size_t n = utf8Copy(out, cap - kTail, entry.fileName, stemLen);
  out[n] = '-';
  memcpy(out + n + 1, entry.book.hex, 8);
  memcpy(out + n + 9, ".epub", 6);
}

}  // namespace

// A name on the SD card says nothing about what is inside: two books in the account can
// share a file name, and the person can have put anything there. Only the identity counts.
Catalog::Local Catalog::locate(const CatalogEntry& entry, const char* dir, char* name, size_t nameCap) {
  char path[192];
  if (!joinPath(dir, entry.fileName, path, sizeof(path))) return Local::Unreadable;
  copyText(name, nameCap, entry.fileName);
  if (!fs_.exists(path)) return Local::Absent;
  BookId found;
  if (!partialMd5(fs_, path, found)) return Local::Unreadable;
  if (memcmp(found.md5, entry.book.md5, 16) == 0) return Local::Here;

  hashedFileName(entry, name, nameCap);
  if (!joinPath(dir, name, path, sizeof(path))) return Local::Unreadable;
  if (!fs_.exists(path)) return Local::Absent;
  if (!partialMd5(fs_, path, found)) return Local::Unreadable;
  return memcmp(found.md5, entry.book.md5, 16) == 0 ? Local::Here : Local::Unreadable;
}

bool Catalog::isOnSd(const CatalogEntry& entry, const char* dir) {
  char name[sizeof(entry.fileName)];
  return locate(entry, dir, name, sizeof(name)) == Local::Here;
}

DownloadResult Catalog::download(const CatalogEntry& entry, const char* dir, IDownloadProgress* progress, uint32_t budgetMs) {
  CatalogEntry copy = entry;
  return download(copy, dir, progress, budgetMs);
}

DownloadResult Catalog::download(CatalogEntry& entry, const char* dir, IDownloadProgress* progress, uint32_t budgetMs) {
  error_.clear();
  char finalName[sizeof(entry.fileName)];
  const Local local = locate(entry, dir, finalName, sizeof(finalName));
  if (local == Local::Unreadable) return DownloadResult::LocalError;
  copyText(entry.fileName, sizeof(entry.fileName), finalName);
  if (local == Local::Here) return DownloadResult::AlreadyPresent;
  char finalPath[192];
  if (!joinPath(dir, entry.fileName, finalPath, sizeof(finalPath))) return DownloadResult::LocalError;
  char partPath[200];
  char metaPath[200];
  const size_t n = strlen(finalPath);
  memcpy(partPath, finalPath, n);
  memcpy(partPath + n, ".part", 6);
  memcpy(metaPath, finalPath, n);
  memcpy(metaPath + n, ".etag", 6);

  uint64_t have = 0;
  char storedEtag[72] = {0};
  if (fs_.exists(partPath) && fs_.exists(metaPath)) {
    File part(fs_, partPath, IFs::Mode::Read);
    File meta(fs_, metaPath, IFs::Mode::Read);
    if (part && meta) {
      const int64_t size = part.size();
      const int32_t got = meta.read(storedEtag, sizeof(storedEtag) - 1);
      if (size > 0 && got > 0 && static_cast<uint64_t>(size) <= entry.fileSize) {
        have = static_cast<uint64_t>(size);
        storedEtag[got] = '\0';
      } else {
        storedEtag[0] = '\0';
      }
    }
  }

  char path[120];
  {
    JsonWriter w(path, sizeof(path) - 1);
    w.raw(kBooksPath);
    w.raw("/");
    w.raw(entry.id);
    w.raw("/download");
    if (!w.ok()) return DownloadResult::LocalError;
    path[w.size()] = '\0';
  }

  api_.configure(store_.creds().baseUrl, store_.creds().token);
  api_.beginSession(budgetMs);

  ApiResult r;
  bool appended = false;
  if (have == entry.fileSize && have > 0) {
    // Every byte is here; only the verification below is missing. A Range past the
    // end would only get a 416.
    r.kind = ApiKind::Ok;
    r.status = 206;
    copyText(r.etag, sizeof(r.etag), storedEtag);
    appended = true;
  } else {
    char range[40];
    HttpHeader headers[2];
    uint8_t headerCount = 0;
    if (have > 0) {
      JsonWriter w(range, sizeof(range) - 1);
      w.raw("bytes=");
      char num[21];
      formatU64(have, num);
      w.raw(num);
      w.raw("-");
      range[w.size()] = '\0';
      headers[headerCount++] = HttpHeader{"Range", range};
      headers[headerCount++] = HttpHeader{"If-Range", storedEtag};
    }
    // A 200 to a Range request means the file changed. The sink drops that body and
    // the code below starts over.
    File out(fs_, partPath, have > 0 ? IFs::Mode::Append : IFs::Mode::WriteTrunc);
    if (!out) return DownloadResult::LocalError;
    FileSink sink(fs_, out.handle(), progress, have, entry.fileSize);
    r = api_.callTo("GET", path, sink, headers, headerCount);
    out.flush();
    out.close();
    if (sink.failed()) return DownloadResult::LocalError;
    if (r.status == 200 && have > 0) {
      // The server ignored the Range (the file changed, or If-Range failed).
      fs_.remove(partPath);
      fs_.remove(metaPath);
      return DownloadResult::Interrupted;
    }
    appended = have > 0 ? r.status == 206 : r.status == 200;
    if (r.etag[0] && !have) {
      File meta(fs_, metaPath, IFs::Mode::WriteTrunc);
      if (meta) {
        meta.writeAll(r.etag, strlen(r.etag));
        meta.flush();
      }
    }
  }

  if (r.kind == ApiKind::Cancelled) return DownloadResult::Cancelled;
  if (r.kind == ApiKind::Budget || r.kind == ApiKind::Transport) {
    note(r, "transport");
    return DownloadResult::Interrupted;
  }
  if (r.kind == ApiKind::HttpError) {
    note(r, "download_failed");
    if (r.status == 401 || r.status == 403) return DownloadResult::NeedsRelink;
    if (r.status == 416) {
      fs_.remove(partPath);
      fs_.remove(metaPath);
    }
    return DownloadResult::ServerError;
  }
  if (!appended) return DownloadResult::ServerError;

  // The header, when present, must agree with the manifest; the file must agree with both.
  if (r.koreaderMd5[0]) {
    BookId header;
    if (!header.setHex(r.koreaderMd5) || memcmp(header.md5, entry.book.md5, 16) != 0) {
      fs_.remove(partPath);
      fs_.remove(metaPath);
      note(r, "integrity_header_mismatch");
      return DownloadResult::IntegrityFailed;
    }
  }
  BookId actual;
  if (!partialMd5(fs_, partPath, actual)) return DownloadResult::LocalError;
  if (memcmp(actual.md5, entry.book.md5, 16) != 0) {
    fs_.remove(partPath);
    fs_.remove(metaPath);
    note(r, "integrity_failed");
    return DownloadResult::IntegrityFailed;
  }
  if (!fs_.rename(partPath, finalPath)) return DownloadResult::LocalError;
  fs_.remove(metaPath);
  return DownloadResult::Done;
}

}  // namespace borges

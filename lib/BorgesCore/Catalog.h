#pragma once

// Home -> Borges -> Biblioteca: the account's books, a page at a time, cached on SD so
// the list opens without a radio, and one-press download with resume.
//
// Everything in a catalog response is treated as hostile: an id ends up in a URL, a
// file name ends up as a path, and a page can claim to hold anything. Entries that fail
// validation are dropped; the page around them is still good.

#include <cstddef>
#include <cstdint>

#include "Api.h"
#include "Ports.h"
#include "Store.h"
#include "Types.h"

namespace borges {

struct CatalogEntry {
  char id[40] = {0};
  BookId book;  // koreader_partial_md5. Required: without it a book cannot sync.
  char sha256[65] = {0};
  uint32_t fileSize = 0;
  char title[96] = {0};
  char author[64] = {0};
  char fileName[64] = {0};  // Safe to use as a file name. Always ends in .epub.
};

class ICatalogVisitor {
 public:
  virtual ~ICatalogVisitor() = default;
  virtual void onBook(uint16_t index, const CatalogEntry& entry) = 0;
};

struct CatalogPage {
  uint16_t page = 0;
  uint16_t count = 0;
  bool hasNext = false;
  bool hasPrevious = false;
};

enum class CatalogResult : uint8_t { Ok, Offline, Cancelled, NeedsRelink, ServerError, Malformed, LocalError };

enum class DownloadResult : uint8_t {
  Done,
  AlreadyPresent,
  Interrupted,   // .part kept: the next call resumes it.
  Cancelled,
  NeedsRelink,
  ServerError,
  IntegrityFailed,  // Partial MD5 does not match. Nothing is accepted.
  LocalError,
};

class IDownloadProgress {
 public:
  virtual ~IDownloadProgress() = default;
  virtual void onProgress(uint64_t received, uint64_t total) = 0;
};

class Catalog {
 public:
  static constexpr uint16_t kMaxPageSize = 50;

  Catalog(Store& store, IFs& fs, IHttp& http, IClock& clock, ICancel& cancel);

  // Downloads a page, validates it and replaces the cached copy. page is 1-based.
  CatalogResult fetchPage(uint16_t page, uint16_t limit, CatalogPage& out, uint32_t budgetMs = 30000);
  // From the SD cache. False when there is no intact copy of that page.
  bool loadPage(uint16_t page, CatalogPage& out, ICatalogVisitor& visitor);

  // dir must exist. Resumes from <dir>/<fileName>.part with Range + If-Range, and only
  // renames onto the final name after the partial MD5 of the whole file matches both
  // the header and the manifest. If another book already has that file name, this one
  // lands as <stem>-<hash>.epub, and the overload taking a mutable entry updates fileName
  // to where the book really is, whatever the result.
  DownloadResult download(CatalogEntry& entry, const char* dir, IDownloadProgress* progress, uint32_t budgetMs);
  DownloadResult download(const CatalogEntry& entry, const char* dir, IDownloadProgress* progress, uint32_t budgetMs);
  // True only if the file there really is this edition. Reads its partial MD5: at most 12 KiB.
  bool isOnSd(const CatalogEntry& entry, const char* dir);

  const Diagnostics& lastError() const { return error_; }

 private:
  enum class Local : uint8_t { Absent, Here, Unreadable };
  // name gets where this book is, or where it should go when Absent.
  Local locate(const CatalogEntry& entry, const char* dir, char* name, size_t nameCap);
  void cachePath(uint16_t page, char* out) const;
  void note(const ApiResult& result, const char* code);

  Store& store_;
  IFs& fs_;
  IClock& clock_;
  Api api_;
  Diagnostics error_;
};

// Reduces a server-supplied name to one safe on FAT: no separators, no control bytes,
// no reserved characters, no leading dots, bounded, ending in .epub. Falls back to the
// book's id if nothing usable remains.
void safeFileName(const char* suggested, const char* fallback, char* out, size_t cap);

// KOReader's partial MD5 of a file: 1 KiB at 0 and at 1024 << 2i for i in 0..10. A read
// error means no identity at all, never a wrong hash (design rule 16).
bool partialMd5(IFs& fs, const char* path, BookId& out);

}  // namespace borges

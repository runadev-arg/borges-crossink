#include "Underlines.h"

#include <Utf8.h>

#include <cstring>

#include "Codec.h"

namespace borges {

namespace {

constexpr uint8_t kPlanMagic[4] = {'B', 'U', 'P', '1'};
constexpr uint8_t kSpinesMagic[4] = {'B', 'U', 'S', '1'};
constexpr size_t kPlanHeaderBytes = 4 + 4 + 4 + 4 + 2 + 2;
constexpr size_t kSegmentBytes = 2 + 2 + 2 + 4;
constexpr size_t kSpinesBytes = 4 + 4 + kUnderlineSpineBytes + 4;

void writeSegment(const UnderlineSegment& s, uint8_t* out) {
  ByteWriter w(out, kSegmentBytes);
  w.u16(s.page);
  w.u16(s.delta);
  w.u16(s.length);
  w.u32(s.hash);
}

UnderlineSegment readSegment(const uint8_t* in) {
  ByteReader r(in, kSegmentBytes);
  UnderlineSegment s;
  s.page = r.u16();
  s.delta = r.u16();
  s.length = r.u16();
  s.hash = r.u32();
  return s;
}

}  // namespace

bool underlineCounts(const uint32_t cp) {
  if (cp == ' ' || cp == '\n' || cp == '\t' || cp == '\r' || cp == '\f' || cp == '\v') return false;
  if (cp < 0x80) return cp != 0;
  if (cp <= 0xA0 || cp == 0xAD || cp == 0xFEFF) return false;
  if (cp >= 0x0300 && cp <= 0x036F) return false;
  if ((cp >= 0x2000 && cp <= 0x200B) || cp == 0x202F || cp == 0x205F || cp == 0x3000) return false;
  return true;
}

uint32_t underlineFold(const uint32_t cp) {
  uint32_t base = cp;
  if (cp >= 0xC0) {
    const uint32_t decomposed = utf8DecomposedBase(cp);
    if (decomposed) base = decomposed;
  }
  if (base >= 'A' && base <= 'Z') base += 'a' - 'A';
  return base;
}

bool underlineTextSignature(const char* text, uint32_t& hash, uint16_t& chars) {
  hash = kUnderlineHashBasis;
  chars = 0;
  if (!text) return false;
  const auto* p = reinterpret_cast<const unsigned char*>(text);
  while (*p) {
    const uint32_t cp = utf8NextCodepoint(&p);
    if (!underlineCounts(cp)) continue;
    hash = underlineHashStep(hash, underlineFold(cp));
    ++chars;
  }
  return chars > 0;
}

// ---------------------------------------------------------------------------------
// What the log says about a chapter
// ---------------------------------------------------------------------------------

bool collectUnderlineTargets(IFs& fs, const BookId& book, const uint16_t epoch, const int32_t spine, IOwnPassages* own,
                             const UnderlinePointerKey keyOf, UnderlineTarget* targets, const uint16_t cap,
                             uint16_t& count) {
  class Collect final : public InboundVisitor {
   public:
    Collect(const int32_t spine, IOwnPassages* own, const UnderlinePointerKey keyOf, UnderlineTarget* targets,
            const uint16_t cap)
        : spine_(spine), own_(own), keyOf_(keyOf), targets_(targets), cap_(cap) {}
    bool onLive(uint32_t, const InboundHighlight& h) override {
      if (!h.xpointer[0] || spineOfXpointer(h.xpointer) != spine_) return true;
      if (own_ && own_->isOwn(h.passage, spine_)) return true;
      UnderlineTarget target;
      if (!keyOf_(h.xpointer, target.key, target.offset)) return true;
      if (!underlineTextSignature(h.text, target.hash, target.chars)) return true;
      target.passage = static_cast<uint32_t>(h.passage);
      // Unlike a list, a page can show the same words twice in their two places: only the same place
      // from two devices is one line.
      for (uint16_t i = 0; i < count; ++i) {
        if (targets_[i].passage == target.passage && targets_[i].key == target.key &&
            targets_[i].offset == target.offset) {
          targets_[i] = target;
          return true;
        }
      }
      if (count < cap_) targets_[count++] = target;
      return true;
    }
    uint16_t count = 0;

   private:
    int32_t spine_;
    IOwnPassages* own_;
    UnderlinePointerKey keyOf_;
    UnderlineTarget* targets_;
    uint16_t cap_;
  };

  Collect collect(spine, own, keyOf, targets, cap);
  if (!InboundList::visit(fs, book, epoch, collect)) return false;
  count = collect.count;
  for (uint16_t i = 1; i < count; ++i) {
    const UnderlineTarget held = targets[i];
    uint16_t j = i;
    while (j > 0 &&
           (targets[j - 1].key > held.key || (targets[j - 1].key == held.key && targets[j - 1].offset > held.offset))) {
      targets[j] = targets[j - 1];
      --j;
    }
    targets[j] = held;
  }
  return true;
}

bool collectUnderlineSpines(IFs& fs, const BookId& book, const uint16_t epoch, uint8_t bits[kUnderlineSpineBytes]) {
  class Collect final : public InboundVisitor {
   public:
    explicit Collect(uint8_t* bits) : bits_(bits) {}
    bool onLive(uint32_t, const InboundHighlight& h) override {
      const int32_t spine = spineOfXpointer(h.xpointer);
      if (spine < 0) return true;
      const auto at = static_cast<uint32_t>(spine) % (kUnderlineSpineBytes * 8);
      bits_[at / 8] = static_cast<uint8_t>(bits_[at / 8] | (1u << (at % 8)));
      return true;
    }

   private:
    uint8_t* bits_;
  };
  memset(bits, 0, kUnderlineSpineBytes);
  Collect collect(bits);
  return InboundList::visit(fs, book, epoch, collect);
}

// ---------------------------------------------------------------------------------
// The walk
// ---------------------------------------------------------------------------------

UnderlinePlanner::UnderlinePlanner(const UnderlineTarget* targets, const uint16_t targetCount,
                                   const uint32_t* pageStarts, const uint16_t pageCount, UnderlineSegment* out,
                                   const uint16_t outCap)
    : targets_(targets),
      targetCount_(targetCount),
      pageStarts_(pageStarts),
      pageCount_(pageCount),
      out_(out),
      outCap_(outCap) {}

void UnderlinePlanner::advancePage(const uint32_t at) {
  while (page_ + 1u < pageCount_ && at >= pageStarts_[page_ + 1u]) {
    for (uint8_t i = 0; i < activeCount_; ++i) closeSegment(active_[i]);
    ++page_;
    counted_ = 0;
  }
}

void UnderlinePlanner::closeSegment(Active& active) {
  if (active.segmentLength == 0) return;
  if (outCount_ < outCap_) {
    UnderlineSegment& segment = out_[outCount_++];
    segment.page = page_;
    segment.delta = active.segmentStart;
    segment.length = active.segmentLength;
    segment.hash = active.segmentHash;
    segment.owner = active.target;
  } else {
    // No room for this part, and half an underline is not what was asked for. The target is refused
    // when it ends: its seen count stops mattering.
    active.seen = 0xFFFF;
  }
  active.segmentLength = 0;
}

void UnderlinePlanner::forget(const Active& active) {
  // Other highlights may have written in between: each part says whose it is.
  for (uint16_t s = active.firstOut; s < outCount_; ++s) {
    if (out_[s].owner == active.target) out_[s].length = 0;
  }
}

void UnderlinePlanner::drop(const uint8_t index) {
  active_[index] = active_[activeCount_ - 1];
  --activeCount_;
}

void UnderlinePlanner::onHit(const uint16_t target, const uint32_t at) {
  advancePage(at);
  ++hits_;
  if (target >= targetCount_ || activeCount_ >= kUnderlineActiveMax) {
    ++refused_;
    return;
  }
  Active& active = active_[activeCount_++];
  active.target = target;
  active.hash = kUnderlineHashBasis;
  active.seen = 0;
  active.segmentHash = kUnderlineHashBasis;
  active.segmentStart = 0;
  active.segmentLength = 0;
  active.firstOut = outCount_;
}

bool UnderlinePlanner::onChar(const uint32_t cp, const uint32_t at) {
  if (pageCount_ == 0 || at < pageStarts_[0]) return hits_ >= targetCount_ && activeCount_ == 0;
  advancePage(at);
  if (!underlineCounts(cp)) return false;
  if (activeCount_ > 0) {
    const uint32_t folded = underlineFold(cp);
    for (uint8_t i = 0; i < activeCount_;) {
      Active& active = active_[i];
      if (active.segmentLength == 0) {
        active.segmentStart = counted_ > 0xFFFF ? 0xFFFF : static_cast<uint16_t>(counted_);
        active.segmentHash = kUnderlineHashBasis;
      }
      active.segmentHash = underlineHashStep(active.segmentHash, folded);
      ++active.segmentLength;
      active.hash = underlineHashStep(active.hash, folded);
      if (active.seen != 0xFFFF) ++active.seen;
      const UnderlineTarget& target = targets_[active.target];
      const bool overflowed = active.seen == 0xFFFF;
      if (overflowed || active.seen == target.chars) {
        closeSegment(active);
        if (!overflowed && active.seen == target.chars && active.hash == target.hash) {
          ++verified_;
        } else {
          // The text there is not the highlight's. None of what was noted for it stays.
          ++refused_;
          forget(active);
        }
        drop(i);
        continue;
      }
      ++i;
    }
  }
  ++counted_;
  return hits_ >= targetCount_ && activeCount_ == 0;
}

uint16_t UnderlinePlanner::finish() {
  // What was still open is a highlight whose text ran past the end of the chapter: not what it says.
  for (uint8_t i = 0; i < activeCount_; ++i) {
    active_[i].segmentLength = 0;
    forget(active_[i]);
    ++refused_;
  }
  activeCount_ = 0;
  uint16_t kept = 0;
  for (uint16_t i = 0; i < outCount_; ++i) {
    if (out_[i].length != 0) out_[kept++] = out_[i];
  }
  outCount_ = kept;
  for (uint16_t i = 1; i < outCount_; ++i) {
    const UnderlineSegment held = out_[i];
    uint16_t j = i;
    while (j > 0 &&
           (out_[j - 1].page > held.page || (out_[j - 1].page == held.page && out_[j - 1].delta > held.delta))) {
      out_[j] = out_[j - 1];
      --j;
    }
    out_[j] = held;
  }
  return outCount_;
}

// ---------------------------------------------------------------------------------
// On the card
// ---------------------------------------------------------------------------------

bool saveUnderlineSpines(IFs& fs, const char* path, const uint32_t logIdentity,
                         const uint8_t bits[kUnderlineSpineBytes]) {
  uint8_t raw[kSpinesBytes];
  memcpy(raw, kSpinesMagic, 4);
  ByteWriter w(raw + 4, sizeof(raw) - 4);
  w.u32(logIdentity);
  w.bytes(bits, kUnderlineSpineBytes);
  w.u32(crc32(raw, sizeof(raw) - 4));
  File f(fs, path, IFs::Mode::WriteTrunc);
  return f && f.writeAll(raw, sizeof(raw)) && f.flush();
}

bool loadUnderlineSpines(IFs& fs, const char* path, const uint32_t logIdentity, uint8_t bits[kUnderlineSpineBytes]) {
  uint8_t raw[kSpinesBytes];
  File f(fs, path, IFs::Mode::Read);
  if (!f || f.size() != static_cast<int64_t>(sizeof(raw)) || !f.readExact(raw, sizeof(raw))) return false;
  if (memcmp(raw, kSpinesMagic, 4) != 0) return false;
  ByteReader r(raw + 4, sizeof(raw) - 4);
  if (r.u32() != logIdentity) return false;
  r.bytes(bits, kUnderlineSpineBytes);
  return r.u32() == crc32(raw, sizeof(raw) - 4);
}

bool saveUnderlinePlan(IFs& fs, const char* path, const UnderlinePlanKey& key, const UnderlineSegment* segments,
                       const uint16_t count) {
  uint8_t header[kPlanHeaderBytes];
  memcpy(header, kPlanMagic, 4);
  ByteWriter w(header + 4, sizeof(header) - 4);
  w.u32(key.log);
  w.u32(key.own);
  w.u32(key.layout);
  w.u16(key.pages);
  w.u16(count);
  File f(fs, path, IFs::Mode::WriteTrunc);
  if (!f || !f.writeAll(header, sizeof(header))) return false;
  uint32_t crc = crc32Update(crc32Begin(), header, sizeof(header));
  for (uint16_t i = 0; i < count; ++i) {
    uint8_t raw[kSegmentBytes];
    writeSegment(segments[i], raw);
    crc = crc32Update(crc, raw, sizeof(raw));
    if (!f.writeAll(raw, sizeof(raw))) return false;
  }
  uint8_t tail[4];
  ByteWriter t(tail, sizeof(tail));
  t.u32(crc32End(crc));
  return f.writeAll(tail, sizeof(tail)) && f.flush();
}

bool loadUnderlinePlan(IFs& fs, const char* path, const UnderlinePlanKey& key, uint16_t& count,
                       uint8_t pageBits[kUnderlinePageBytes]) {
  count = 0;
  File f(fs, path, IFs::Mode::Read);
  if (!f) return false;
  uint8_t header[kPlanHeaderBytes];
  const int64_t size = f.size();
  if (size < static_cast<int64_t>(sizeof(header) + 4) || !f.readExact(header, sizeof(header))) return false;
  if (memcmp(header, kPlanMagic, 4) != 0) return false;
  ByteReader r(header + 4, sizeof(header) - 4);
  const uint32_t log = r.u32();
  const uint32_t own = r.u32();
  const uint32_t layout = r.u32();
  const uint16_t pages = r.u16();
  const uint16_t stored = r.u16();
  if (log != key.log || own != key.own || layout != key.layout || pages != key.pages) return false;
  if (stored > kUnderlineMaxSegments || size != static_cast<int64_t>(sizeof(header) + stored * kSegmentBytes + 4)) {
    return false;
  }
  memset(pageBits, 0, kUnderlinePageBytes);
  uint32_t crc = crc32Update(crc32Begin(), header, sizeof(header));
  for (uint16_t i = 0; i < stored; ++i) {
    uint8_t raw[kSegmentBytes];
    if (!f.readExact(raw, sizeof(raw))) return false;
    crc = crc32Update(crc, raw, sizeof(raw));
    const UnderlineSegment segment = readSegment(raw);
    const uint32_t bit = segment.page % (kUnderlinePageBytes * 8);
    pageBits[bit / 8] = static_cast<uint8_t>(pageBits[bit / 8] | (1u << (bit % 8)));
  }
  uint8_t tail[4];
  if (!f.readExact(tail, sizeof(tail))) return false;
  ByteReader t(tail, sizeof(tail));
  if (t.u32() != crc32End(crc)) return false;
  count = stored;
  return true;
}

uint8_t readUnderlineWindow(IFs& fs, const char* path, const uint16_t total, const uint16_t page, UnderlineSegment* out,
                            const uint8_t cap, uint32_t& coversBefore) {
  coversBefore = static_cast<uint32_t>(page) + 1;
  if (total == 0 || cap == 0) return 0;
  File f(fs, path, IFs::Mode::Read);
  if (!f) return 0;
  uint32_t low = 0;
  uint32_t high = total;
  while (low < high) {
    const uint32_t middle = (low + high) / 2;
    uint8_t raw[kSegmentBytes];
    if (!f.seek(static_cast<uint32_t>(kPlanHeaderBytes + middle * kSegmentBytes)) || !f.readExact(raw, sizeof(raw))) {
      return 0;
    }
    if (readSegment(raw).page < page) {
      low = middle + 1;
    } else {
      high = middle;
    }
  }
  const uint32_t left = total - low;
  const uint8_t wanted = left < cap ? static_cast<uint8_t>(left) : cap;
  if (wanted == 0) {
    coversBefore = UINT32_MAX;  // Nothing from here to the end of the chapter.
    return 0;
  }
  // A page's whole set in one read: on the stack, at most cap records.
  uint8_t raw[kUnderlinePageMax * kSegmentBytes];
  const size_t bytes = wanted * kSegmentBytes;
  if (wanted > kUnderlinePageMax || !f.seek(static_cast<uint32_t>(kPlanHeaderBytes + low * kSegmentBytes)) ||
      !f.readExact(raw, bytes)) {
    return 0;
  }
  for (uint8_t i = 0; i < wanted; ++i) out[i] = readSegment(raw + i * kSegmentBytes);
  if (left <= cap) {
    coversBefore = UINT32_MAX;
  } else if (out[wanted - 1].page > page) {
    // The last page read may have more than fit: it is not vouched for.
    coversBefore = out[wanted - 1].page;
  }
  return wanted;
}

// ---------------------------------------------------------------------------------
// The page
// ---------------------------------------------------------------------------------

UnderlinePageMatch::UnderlinePageMatch(const UnderlineSegment* segments, const uint8_t count)
    : segments_(segments), count_(count > kUnderlinePageMax ? kUnderlinePageMax : count) {}

bool UnderlinePageMatch::looksLikeListMarker(const char* text, const size_t length) {
  if (length == 3 && memcmp(text, "\xE2\x80\xA2", 3) == 0) return true;
  if (length < 2 || text[length - 1] != '.') return false;
  for (size_t i = 0; i + 1 < length; ++i) {
    if (text[i] < '0' || text[i] > '9') {
      if (!(i == 0 && text[i] == '-')) return false;
    }
  }
  return true;
}

namespace {

// The end of a word without its inserted hyphen, which is the reader's and not the book's.
size_t countedBytes(const char* text, const size_t length, const bool insertedHyphen) {
  if (!insertedHyphen || length == 0) return length;
  if (text[length - 1] == '-') return length - 1;
  if (length >= 3 && memcmp(text + length - 3, "\xE2\x80\x90", 3) == 0) return length - 3;
  return length;
}

}  // namespace

uint32_t UnderlinePageMatch::wordChars(const char* text, const size_t length, const bool insertedHyphen) {
  const auto* p = reinterpret_cast<const unsigned char*>(text);
  const auto* const end = p + countedBytes(text, length, insertedHyphen);
  uint32_t chars = 0;
  while (p < end) {
    if (underlineCounts(utf8NextCodepoint(&p))) ++chars;
  }
  return chars;
}

size_t UnderlinePageMatch::byteOfChar(const char* text, const size_t length, const uint32_t charIndex) {
  const auto* const start = reinterpret_cast<const unsigned char*>(text);
  const auto* p = start;
  const auto* const end = p + length;
  uint32_t chars = 0;
  while (p < end) {
    const auto* before = p;
    if (underlineCounts(utf8NextCodepoint(&p))) {
      if (chars == charIndex) return static_cast<size_t>(before - start);
      ++chars;
    }
  }
  return length;
}

void UnderlinePageMatch::feedWord(const char* text, const size_t length, const bool insertedHyphen,
                                  const bool pageOnly) {
  const auto* p = reinterpret_cast<const unsigned char*>(text);
  const auto* const end = p + countedBytes(text, length, insertedHyphen);
  while (p < end) {
    const uint32_t cp = utf8NextCodepoint(&p);
    if (!underlineCounts(cp)) continue;
    uint32_t folded = 0;
    bool haveFolded = false;
    for (uint8_t i = 0; i < count_; ++i) {
      const UnderlineSegment& segment = segments_[i];
      State& state = states_[i];
      const auto step = [&](Run& run, const uint32_t position) {
        if (position < segment.delta || position >= static_cast<uint32_t>(segment.delta) + segment.length) return;
        if (!haveFolded) {
          folded = underlineFold(cp);
          haveFolded = true;
        }
        if (run.seen == 0) run.rawStart = all_;
        run.hash = underlineHashStep(run.hash, folded);
        ++run.seen;
        run.rawEnd = all_ + 1;
      };
      step(state.all, all_);
      if (!pageOnly) step(state.strict, strict_);
    }
    ++all_;
    if (!pageOnly) ++strict_;
  }
}

void UnderlinePageMatch::finish() {
  accepted_ = 0;
  for (uint8_t i = 0; i < count_; ++i) {
    const UnderlineSegment& segment = segments_[i];
    State& state = states_[i];
    const Run* run = nullptr;
    if (state.all.seen == segment.length && state.all.hash == segment.hash) {
      run = &state.all;
    } else if (state.strict.seen == segment.length && state.strict.hash == segment.hash) {
      run = &state.strict;
    }
    if (!run) continue;
    state.ok = true;
    state.from = run->rawStart;
    state.to = run->rawEnd;
    state.leftOutTheReaders = run == &state.strict;
    ++accepted_;
  }
}

bool UnderlinePageMatch::spanOf(const uint32_t wordFirstChar, const uint32_t wordChars, const bool pageOnly,
                                uint32_t& charFrom, uint32_t& charTo) const {
  if (wordChars == 0) return false;
  bool any = false;
  for (uint8_t i = 0; i < count_; ++i) {
    const State& state = states_[i];
    if (!state.ok || (pageOnly && state.leftOutTheReaders)) continue;
    const uint32_t to = state.to;
    if (state.from >= wordFirstChar + wordChars || to <= wordFirstChar) continue;
    const uint32_t from = state.from > wordFirstChar ? state.from - wordFirstChar : 0;
    const uint32_t until = to < wordFirstChar + wordChars ? to - wordFirstChar : wordChars;
    if (!any || from < charFrom) charFrom = from;
    if (!any || until > charTo) charTo = until;
    any = true;
  }
  return any;
}

}  // namespace borges

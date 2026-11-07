// The underlines' two hot paths on real RISC-V code: walking a chapter's characters and matching a page.
// Instruction counts from QEMU (8 ns each), so an order of magnitude and not a measured time: no SD
// card, no flash cache misses, no contention. Heap numbers are the device allocator's.
//
//   pio run -c test/borges_qemu/platformio-qemu.ini -e underline-bench
//   python -I test/borges_qemu/run.py --env underline-bench --name underlines --fresh --no-nic --watch "@@ UNDERLINES DONE"

#include <Arduino.h>
#include <esp_heap_caps.h>

#include <new>

#include "Underlines.h"

using namespace borges;

// What the firmware's link flags wrap, with nothing of the firmware here to wrap it.
extern "C" {
void __real_panic_print_backtrace(const void* frame, int core);
void __real_panic_abort(const char* details);
void __wrap_panic_print_backtrace(const void* frame, int core) { __real_panic_print_backtrace(frame, core); }
void __wrap_panic_abort(const char* details) { __real_panic_abort(details); }
esp_err_t __wrap_bootloader_common_check_efuse_blk_validity(uint32_t, uint32_t) { return ESP_OK; }
}

namespace {

// As in the e2e book, a line of a page with words of Spanish length.
constexpr const char* kWords[] = {"Las",  "palabras", "siguen",      "por",      "el",     "corredor",
                                  "de",   "la",       "biblioteca,", "hexagono", "tras",   "hexagono,",
                                  "para", "que",      "cada",        "pagina",   "tenga",  "texto",
                                  "de",   "verdad",   "y",           "el",       "lector", "tenga"};
constexpr size_t kWordCount = sizeof(kWords) / sizeof(kWords[0]);

uint32_t instructionsNow() { return micros(); }

void walkBench(const uint32_t chapterChars, const uint16_t targetCount, const uint16_t pageCount) {
  const uint32_t freeBefore = esp_get_free_heap_size();
  auto* targets = new (std::nothrow) UnderlineTarget[targetCount];
  auto* starts = new (std::nothrow) uint32_t[pageCount];
  auto* segments = new (std::nothrow) UnderlineSegment[kUnderlineMaxSegments];
  if (!targets || !starts || !segments) {
    printf("@@ underline walk: no memory\n");
    return;
  }
  const uint32_t pageChars = chapterChars / pageCount;
  for (uint16_t p = 0; p < pageCount; ++p) starts[p] = p * pageChars;
  for (uint16_t t = 0; t < targetCount; ++t) {
    targets[t].chars = 80;
    targets[t].hash = 1;  // Refused at its end: the walk does the same work up to there.
  }
  const uint32_t heldBytes = freeBefore - esp_get_free_heap_size();
  UnderlinePlanner planner(targets, targetCount, starts, pageCount, segments, kUnderlineMaxSegments);
  const uint32_t every = chapterChars / targetCount;
  const uint32_t began = instructionsNow();
  uint16_t next = 0;
  for (uint32_t at = 0; at < chapterChars; ++at) {
    if (next < targetCount && at == next * every) planner.onHit(next++, at);
    planner.onChar((at % 6) == 5 ? ' ' : 'a' + (at % 26), at);
  }
  planner.finish();
  const uint32_t took = instructionsNow() - began;
  printf("@@ underline walk chars=%u targets=%u pages=%u took=%u us (%u ns/char) held=%u B freeNow=%u\n",
         static_cast<unsigned>(chapterChars), targetCount, pageCount, static_cast<unsigned>(took),
         static_cast<unsigned>(took * 1000ULL / chapterChars), static_cast<unsigned>(heldBytes),
         static_cast<unsigned>(esp_get_free_heap_size()));
  delete[] segments;
  delete[] starts;
  delete[] targets;
}

void matchBench(const uint16_t words, const uint8_t segmentCount) {
  UnderlineSegment segments[kUnderlinePageMax];
  uint32_t position = 0;
  const uint32_t step = words * 5 / (segmentCount + 1);
  for (uint8_t s = 0; s < segmentCount; ++s) {
    segments[s].page = 0;
    segments[s].delta = static_cast<uint16_t>(position += step);
    segments[s].length = 60;
    segments[s].hash = 0;
  }
  const uint32_t began = instructionsNow();
  UnderlinePageMatch match(segments, segmentCount);
  for (uint16_t w = 0; w < words; ++w) {
    const char* word = kWords[w % kWordCount];
    match.feedWord(word, strlen(word), false, false);
  }
  match.finish();
  uint32_t first = 0;
  for (uint16_t w = 0; w < words; ++w) {
    const char* word = kWords[w % kWordCount];
    const uint32_t chars = UnderlinePageMatch::wordChars(word, strlen(word), false);
    uint32_t from = 0;
    uint32_t to = 0;
    match.spanOf(first, chars, false, from, to);
    first += chars;
  }
  const uint32_t took = instructionsNow() - began;
  printf("@@ underline page words=%u segments=%u took=%u us stackUse=%u B\n", words, segmentCount,
         static_cast<unsigned>(took), static_cast<unsigned>(sizeof(UnderlinePageMatch)));
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(100);
  printf("@@ sizeof UnderlineBook=%u UnderlinePlanner=%u UnderlinePageMatch=%u free=%u largest=%u\n",
         static_cast<unsigned>(sizeof(UnderlineBook)), static_cast<unsigned>(sizeof(UnderlinePlanner)),
         static_cast<unsigned>(sizeof(UnderlinePageMatch)), static_cast<unsigned>(esp_get_free_heap_size()),
         static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
  walkBench(100000, 1, 40);
  walkBench(100000, 256, 40);
  walkBench(500000, 256, 200);
  matchBench(400, 1);
  matchBench(400, 8);
  matchBench(400, 16);
  printf("@@ UNDERLINES DONE\n");
}

void loop() { delay(1000); }

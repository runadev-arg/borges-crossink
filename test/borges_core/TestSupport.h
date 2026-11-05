#pragma once

// In-memory file system with crash injection, a scripted HTTP fake, and a small model
// of the Borges server. Host-only: uses the standard library and ArduinoJson freely,
// which also means every request body the core writes is parsed by an independent JSON
// implementation.

#include <ArduinoJson.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "Catalog.h"
#include "Outbox.h"
#include "PairingFlow.h"
#include "Ports.h"
#include "Store.h"
#include "SyncEngine.h"

namespace borges_test {

using borges::HttpHeader;
using borges::HttpRequest;
using borges::HttpResponse;
using borges::ICancel;
using borges::IClock;
using borges::IFs;
using borges::IHttp;
using borges::IHttpSink;
using borges::IRandom;
using borges::TimePrecision;
using borges::WallTime;

// ---------------------------------------------------------------------------------
// MemFs
// ---------------------------------------------------------------------------------

enum class TornMode {
  Drop,   // Unflushed bytes are gone.
  Half,   // The first half of the unflushed bytes reached the disk.
  Keep,   // All of them reached the disk even though flush never returned.
};

class MemFs : public IFs {
 public:
  struct Data {
    std::string visible;
    std::string durable;
  };
  struct Open {
    std::string path;
    Mode mode = Mode::Read;
    size_t pos = 0;
    bool used = false;
  };

  // Every mutating operation counts. The Nth one fails, and so does everything after it
  // until reboot(). 0 disables.
  void killAtOp(uint64_t n) { killAt_ = n; }
  uint64_t ops() const { return ops_; }
  bool dead() const { return dead_; }
  void setTornMode(TornMode mode) { torn_ = mode; }

  void reboot() {
    dead_ = false;
    killAt_ = 0;
    for (auto& kv : files_) {
      Data& d = kv.second;
      const size_t unflushed = d.visible.size() - d.durable.size();
      if (torn_ == TornMode::Half) d.durable = d.visible.substr(0, d.durable.size() + unflushed / 2);
      if (torn_ == TornMode::Keep) d.durable = d.visible;
      d.visible = d.durable;
    }
    handles_.clear();
    openCount_ = 0;
  }

  // Test hooks.
  bool has(const std::string& path) const { return files_.count(path) != 0; }
  std::string content(const std::string& path) const {
    auto it = files_.find(path);
    return it == files_.end() ? std::string() : it->second.visible;
  }
  void put(const std::string& path, const std::string& bytes) {
    files_[path].visible = bytes;
    files_[path].durable = bytes;
  }
  void corrupt(const std::string& path, size_t offset) {
    Data& d = files_.at(path);
    d.visible[offset] = static_cast<char>(d.visible[offset] ^ 0x5A);
    if (offset < d.durable.size()) d.durable[offset] = d.visible[offset];
  }
  void failReadsAfter(const std::string& path, size_t bytes) {
    failReadPath_ = path;
    failReadAfter_ = bytes;
    readSoFar_ = 0;
  }
  void failSeeks(const std::string& path) { failSeekPath_ = path; }
  void failOpen(const std::string& path) { failOpenPath_ = path; }
  void failWritesTo(const std::string& path) { failWritePath_ = path; }
  std::vector<std::string> paths() const {
    std::vector<std::string> out;
    for (const auto& kv : files_) out.push_back(kv.first);
    return out;
  }
  int peakOpen() const { return peakOpen_; }
  uint64_t writeOpsTo(const std::string& fragment) const {
    uint64_t n = 0;
    for (const auto& kv : writesByPath_) {
      if (kv.first.find(fragment) != std::string::npos) n += kv.second;
    }
    return n;
  }
  void resetWriteCounts() { writesByPath_.clear(); }

  // IFs
  Handle open(const char* path, Mode mode) override {
    if (dead_ || failOpenPath_ == path) return kInvalid;
    if (mode == Mode::Read && files_.count(path) == 0) return kInvalid;
    if (mode != Mode::Read && !tick()) return kInvalid;
    Data& d = files_[path];
    if (mode == Mode::WriteTrunc) {
      d.visible.clear();
      d.durable.clear();
    }
    size_t slot = 0;
    while (slot < handles_.size() && handles_[slot].used) ++slot;
    if (slot == handles_.size()) handles_.push_back(Open{});
    handles_[slot] = Open{path, mode, 0, true};
    ++openCount_;
    if (openCount_ > peakOpen_) peakOpen_ = openCount_;
    return static_cast<Handle>(slot);
  }
  int32_t read(Handle h, void* dst, size_t len) override {
    if (dead_ || !valid(h)) return -1;
    Open& o = handles_[static_cast<size_t>(h)];
    if (!failReadPath_.empty() && o.path == failReadPath_) {
      if (readSoFar_ >= failReadAfter_) return -1;
    }
    const std::string& v = files_[o.path].visible;
    if (o.pos >= v.size()) return 0;
    const size_t n = std::min(len, v.size() - o.pos);
    memcpy(dst, v.data() + o.pos, n);
    o.pos += n;
    if (!failReadPath_.empty() && o.path == failReadPath_) readSoFar_ += n;
    return static_cast<int32_t>(n);
  }
  int32_t write(Handle h, const void* src, size_t len) override {
    if (dead_ || !valid(h)) return -1;
    Open& o = handles_[static_cast<size_t>(h)];
    if (o.mode == Mode::Read || failWritePath_ == o.path) return -1;
    Data& d = files_[o.path];
    if (!tick()) {
      // Killed in the middle of this write: part of it may have gone out.
      if (torn_ != TornMode::Drop) d.visible.append(static_cast<const char*>(src), len / 2);
      return -1;
    }
    writesByPath_[o.path] += 1;
    if (o.mode == Mode::Append) o.pos = d.visible.size();
    if (o.pos > d.visible.size()) d.visible.resize(o.pos);
    d.visible.replace(o.pos, std::min(len, d.visible.size() - o.pos), static_cast<const char*>(src), len);
    o.pos += len;
    return static_cast<int32_t>(len);
  }
  bool seek(Handle h, uint32_t pos) override {
    if (dead_ || !valid(h)) return false;
    Open& o = handles_[static_cast<size_t>(h)];
    if (!failSeekPath_.empty() && o.path == failSeekPath_) return false;
    o.pos = pos;
    return true;
  }
  int64_t size(Handle h) override {
    if (dead_ || !valid(h)) return -1;
    return static_cast<int64_t>(files_[handles_[static_cast<size_t>(h)].path].visible.size());
  }
  bool flush(Handle h) override {
    if (dead_ || !valid(h)) return false;
    Open& o = handles_[static_cast<size_t>(h)];
    if (o.mode == Mode::Read) return true;
    if (!tick()) return false;
    Data& d = files_[o.path];
    d.durable = d.visible;
    return true;
  }
  void close(Handle h) override {
    if (!valid(h)) return;
    handles_[static_cast<size_t>(h)].used = false;
    --openCount_;
  }
  bool exists(const char* path) override { return !dead_ && (files_.count(path) != 0 || dirs_.count(path) != 0); }
  bool remove(const char* path) override {
    if (dead_) return false;
    if (files_.count(path) == 0) return true;
    if (!tick()) return false;
    files_.erase(path);
    return true;
  }
  bool rename(const char* from, const char* to) override {
    if (dead_ || files_.count(from) == 0 || files_.count(to) != 0) return false;
    if (!tick()) return false;
    Data d = files_[from];
    files_.erase(from);
    files_[to] = d;
    return true;
  }
  bool mkdir(const char* path) override {
    if (dead_) return false;
    if (dirs_.count(path)) return true;
    if (!tick()) return false;
    dirs_.insert(path);
    return true;
  }

 private:
  bool valid(Handle h) const {
    return h >= 0 && static_cast<size_t>(h) < handles_.size() && handles_[static_cast<size_t>(h)].used;
  }
  bool tick() {
    ++ops_;
    if (killAt_ != 0 && ops_ >= killAt_) {
      dead_ = true;
      return false;
    }
    return true;
  }

  std::map<std::string, Data> files_;
  std::set<std::string> dirs_;
  std::vector<Open> handles_;
  std::map<std::string, uint64_t> writesByPath_;
  uint64_t ops_ = 0;
  uint64_t killAt_ = 0;
  bool dead_ = false;
  TornMode torn_ = TornMode::Drop;
  std::string failReadPath_;
  size_t failReadAfter_ = 0;
  size_t readSoFar_ = 0;
  std::string failSeekPath_;
  std::string failOpenPath_;
  std::string failWritePath_;
  int openCount_ = 0;
  int peakOpen_ = 0;
};

// ---------------------------------------------------------------------------------
// Clock, random, cancel
// ---------------------------------------------------------------------------------

constexpr int64_t kNow = 1793800000;  // 2026-11-04-ish. Only ever compared with itself.

class FakeClock : public IClock {
 public:
  uint64_t mono = 1000;
  WallTime wall{kNow, TimePrecision::Exact};
  int setCalls = 0;
  uint64_t monotonicMs() override { return mono; }
  WallTime wallNow() override { return wall; }
  void setWall(int64_t epochSec, TimePrecision precision) override {
    wall = WallTime{epochSec, precision};
    ++setCalls;
  }
};

class FakeRandom : public IRandom {
 public:
  uint64_t state = 0x1234567;
  int fixedByte = -1;
  void fill(uint8_t* dst, size_t len) override {
    for (size_t i = 0; i < len; ++i) {
      state = state * 6364136223846793005ULL + 1442695040888963407ULL;
      dst[i] = fixedByte >= 0 ? static_cast<uint8_t>(fixedByte) : static_cast<uint8_t>(state >> 56);
    }
  }
};

class FakeCancel : public ICancel {
 public:
  bool flag = false;
  bool cancelled() override { return flag; }
};

// ---------------------------------------------------------------------------------
// HttpFake
// ---------------------------------------------------------------------------------

struct Reply {
  int status = 200;
  std::string body;
  bool hasRetryAfter = false;
  uint32_t retryAfter = 0;
  std::string requestId = "req-1";
  std::string etag;
  std::string koreaderMd5;
  std::string sha256;
  // Transport failures.
  bool dropBeforeResponse = false;
  long dropAfterBytes = -1;  // Body cut after this many bytes.
};

inline Reply jsonReply(int status, const std::string& body) {
  Reply r;
  r.status = status;
  r.body = body;
  return r;
}

inline Reply apiError(int status, const std::string& code, const std::string& details = "") {
  std::string body = "{\"error\":{\"code\":\"" + code + "\",\"message\":\"m\",\"retryable\":false";
  if (!details.empty()) body += ",\"details\":" + details;
  body += "},\"request_id\":\"srv-req\"}";
  return jsonReply(status, body);
}

inline Reply dropped() {
  Reply r;
  r.dropBeforeResponse = true;
  return r;
}

struct RecordedRequest {
  std::string method;
  std::string url;
  std::string path;
  std::string body;
  std::map<std::string, std::string> headers;
};

class HttpFake : public IHttp {
 public:
  std::vector<RecordedRequest> log;
  std::deque<Reply> script;
  std::function<Reply(const RecordedRequest&)> handler;
  std::function<void(size_t)> beforeRequest;  // Index of the request about to run.
  int disconnects = 0;
  int connections = 0;
  bool connected = false;
  size_t chunk = 512;
  int cancelChecks = 0;
  uint64_t* monoToAdvance = nullptr;
  uint64_t advancePerRequestMs = 0;

  void perform(const HttpRequest& req, IHttpSink& sink, ICancel& abort, HttpResponse& out) override {
    RecordedRequest rec;
    rec.method = req.method;
    rec.url = req.url;
    const size_t schemeEnd = rec.url.find("://");
    const size_t pathStart = schemeEnd == std::string::npos ? 0 : rec.url.find('/', schemeEnd + 3);
    rec.path = pathStart == std::string::npos ? "/" : rec.url.substr(pathStart);
    if (req.body) rec.body.assign(reinterpret_cast<const char*>(req.body), req.bodyLen);
    for (uint8_t i = 0; i < req.headerCount; ++i) rec.headers[req.headers[i].name] = req.headers[i].value;
    if (beforeRequest) beforeRequest(log.size());
    log.push_back(rec);
    if (monoToAdvance) *monoToAdvance += advancePerRequestMs;
    if (!connected) {
      ++connections;
      connected = true;
    }

    out = HttpResponse{};
    if (abort.cancelled()) {
      ++cancelChecks;
      out.stage = borges::TransportStage::Aborted;
      return;
    }
    Reply reply;
    if (!script.empty()) {
      reply = script.front();
      script.pop_front();
    } else if (handler) {
      reply = handler(rec);
    } else {
      reply = jsonReply(500, "{}");
    }
    if (reply.dropBeforeResponse) {
      connected = false;
      out.stage = borges::TransportStage::ReadHeaders;
      out.transportCode = -7;
      return;
    }
    out.status = reply.status;
    out.hasRetryAfter = reply.hasRetryAfter;
    out.retryAfterSec = reply.retryAfter;
    borges::copyText(out.requestId, sizeof(out.requestId), reply.requestId.c_str());
    borges::copyText(out.etag, sizeof(out.etag), reply.etag.c_str());
    borges::copyText(out.koreaderMd5, sizeof(out.koreaderMd5), reply.koreaderMd5.c_str());
    borges::copyText(out.contentSha256, sizeof(out.contentSha256), reply.sha256.c_str());
    sink.onStatus(reply.status);
    size_t limit = reply.body.size();
    if (reply.dropAfterBytes >= 0 && static_cast<size_t>(reply.dropAfterBytes) < limit) {
      limit = static_cast<size_t>(reply.dropAfterBytes);
    }
    size_t sent = 0;
    while (sent < limit) {
      if (abort.cancelled()) {
        ++cancelChecks;
        out.stage = borges::TransportStage::Aborted;
        connected = false;
        return;
      }
      const size_t n = std::min(chunk, limit - sent);
      if (!sink.onBody(reinterpret_cast<const uint8_t*>(reply.body.data()) + sent, n)) {
        out.stage = borges::TransportStage::ReadBody;
        connected = false;
        return;
      }
      sent += n;
    }
    if (limit < reply.body.size()) {
      out.stage = borges::TransportStage::ReadBody;
      connected = false;
      return;
    }
    out.complete = true;
  }
  void disconnect() override {
    ++disconnects;
    connected = false;
  }

  size_t countPath(const std::string& path) const {
    size_t n = 0;
    for (const auto& r : log) {
      if (r.path == path) ++n;
    }
    return n;
  }
};

// ---------------------------------------------------------------------------------
// A model of the server. Implements what the core relies on, from syncV2Service.js and
// deviceEventRepository.js: idempotent ids, unique sequences, cursor checkpoints.
// ---------------------------------------------------------------------------------

constexpr const char* kMd5A = "abababababababababababababababab";
constexpr const char* kMd5B = "cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd";
constexpr const char* kSelfId = "11111111-1111-4111-8111-111111111111";
constexpr const char* kOtherId = "22222222-2222-4222-8222-222222222222";

struct RecordedEvent {
  std::string id;
  std::string seq;
  std::string type;
  std::string body;  // Canonical JSON of the event minus client_sequence.
  uint64_t serverSeq = 0;
  bool rejected = false;
};

class FakeServer {
 public:
  std::vector<RecordedEvent> recorded;
  std::map<std::string, size_t> byId;
  std::set<std::string> usedSeq;
  std::vector<std::string> feed;           // Raw JSON of events for this device, in server order.
  std::vector<uint64_t> feedSeq;
  uint64_t nextServerSeq = 100;
  uint64_t delivered = 0;
  uint64_t committed = 0;
  std::string focusEvent = "null";
  std::set<std::string> rejectText;        // Events whose payload.text is this get rejected.
  int duplicates = 0;
  std::vector<std::string> violations;
  std::function<void(const std::string& id)> onEventSeen;
  bool ignoreFocusOnly = false;
  size_t pageLimit = 20;

  void addFeedEvent(uint64_t seq, const std::string& json) {
    feed.push_back(json);
    feedSeq.push_back(seq);
    if (seq >= nextServerSeq) nextServerSeq = seq + 1;
  }

  uint64_t highWatermark() const {
    uint64_t hw = feedSeq.empty() ? 0 : feedSeq.back();
    for (const auto& e : recorded) {
      if (e.serverSeq > hw) hw = e.serverSeq;
    }
    return hw;
  }

  Reply handle(const RecordedRequest& req) {
    if (req.path == "/api/sync/v2/capabilities") return capabilities();
    if (req.path == "/api/sync/v2/pull") return pull(req);
    if (req.path == "/api/sync/v2/exchange") return exchange(req);
    if (req.path == "/api/sync/v2/cursor/commit") return commit(req);
    if (req.path == "/api/devices/self/health") return jsonReply(200, "{\"device\":{}}");
    if (req.path.rfind("/api/sync/v2/suggestions/", 0) == 0) return jsonReply(200, "{\"id\":\"x\"}");
    return apiError(404, "not_found");
  }

  static Reply capabilities() {
    return jsonReply(200,
                     "{\"protocol_version\":2,\"supported_protocol_versions\":[2],\"event_versions\":{"
                     "\"progress.changed\":[1],\"session.started\":[1],\"session.ended\":[1],\"page_stat.recorded\":[1],"
                     "\"annotation.created\":[1],\"annotation.updated\":[1],\"annotation.deleted\":[1],"
                     "\"bookmark.upserted\":[1],\"bookmark.deleted\":[1],\"device.library_reported\":[1]},"
                     "\"limits\":{\"push_events\":100},\"cursor\":{\"encoding\":\"decimal_string\"}}");
  }

  std::string pageJson(uint64_t cursor, size_t limit) {
    std::string events = "[";
    size_t count = 0;
    uint64_t last = cursor;
    bool more = false;
    for (size_t i = 0; i < feed.size(); ++i) {
      if (feedSeq[i] <= cursor) continue;
      if (count == limit) {
        more = true;
        break;
      }
      if (count) events += ",";
      events += feed[i];
      last = feedSeq[i];
      ++count;
    }
    events += "]";
    const uint64_t next = more ? last : std::max(cursor, highWatermark());
    if (next > delivered) delivered = next;
    return "\"events\":" + events + ",\"cursor\":\"" + std::to_string(next) + "\",\"high_watermark\":\"" +
           std::to_string(highWatermark()) + "\",\"has_more\":" + (more ? "true" : "false");
  }

  Reply pull(const RecordedRequest& req) {
    JsonDocument doc;
    if (deserializeJson(doc, req.body)) {
      violations.push_back("pull body is not JSON");
      return apiError(422, "validation_failed");
    }
    const uint64_t cursor = strtoull(doc["cursor"] | "0", nullptr, 10);
    if (doc["focus_only"] == true && !ignoreFocusOnly) {
      return jsonReply(200, std::string("{\"protocol_version\":2,\"focus_only\":true,\"reading_head_checked\":true,"
                                        "\"focus_event\":") +
                                focusEvent + ",\"events\":[],\"cursor\":\"" + std::to_string(cursor) +
                                "\",\"has_more\":false,\"server_time\":\"2026-11-05T12:00:00.000Z\"}");
    }
    if (cursor > delivered && cursor > 0) {
      return apiError(409, "cursor_not_delivered",
                      "{\"cursor\":\"" + std::to_string(cursor) + "\",\"deliveredCursor\":\"" + std::to_string(delivered) + "\"}");
    }
    if (cursor > committed) committed = cursor;
    return jsonReply(200, "{\"protocol_version\":2," + pageJson(cursor, doc["limit"] | pageLimit) +
                              ",\"server_time\":\"2026-11-05T12:00:00.000Z\"}");
  }

  Reply commit(const RecordedRequest& req) {
    JsonDocument doc;
    if (deserializeJson(doc, req.body)) return apiError(422, "validation_failed");
    const uint64_t cursor = strtoull(doc["cursor"] | "0", nullptr, 10);
    if (cursor < committed) {
      return apiError(409, "cursor_regression",
                      "{\"cursor\":\"" + std::to_string(cursor) + "\",\"committedCursor\":\"" + std::to_string(committed) + "\"}");
    }
    if (cursor > delivered) {
      return apiError(409, "cursor_not_delivered",
                      "{\"cursor\":\"" + std::to_string(cursor) + "\",\"deliveredCursor\":\"" + std::to_string(delivered) + "\"}");
    }
    committed = cursor;
    return jsonReply(200, "{\"protocol_version\":2,\"cursor\":\"" + std::to_string(cursor) + "\"}");
  }

  Reply exchange(const RecordedRequest& req) {
    JsonDocument doc;
    if (deserializeJson(doc, req.body)) {
      violations.push_back("exchange body is not JSON: " + req.body.substr(0, 200));
      return apiError(422, "validation_failed");
    }
    if ((doc["protocol_version"] | 0) != 2) violations.push_back("protocol_version");
    JsonArray events = doc["events"];
    if (events.size() < 1 || events.size() > 100) violations.push_back("event count");

    // Prove the sequences first, as checkClientSequences does.
    std::string unrecorded;
    bool conflict = false;
    uint64_t maxSeq = 0;
    for (const std::string& s : usedSeq) maxSeq = std::max<uint64_t>(maxSeq, strtoull(s.c_str(), nullptr, 10));
    for (JsonObject e : events) {
      const std::string id = e["client_event_id"] | "";
      const std::string seq = e["client_sequence"] | "";
      if (byId.count(id)) continue;
      if (!unrecorded.empty()) unrecorded += ",";
      unrecorded += "{\"client_event_id\":\"" + id + "\",\"client_sequence\":\"" + seq + "\"}";
      if (usedSeq.count(seq)) conflict = true;
    }
    if (conflict) {
      return apiError(409, "client_sequence_conflict",
                      "{\"next_client_sequence\":\"" + std::to_string(maxSeq + 1) + "\",\"unrecorded_events\":[" + unrecorded + "]}");
    }

    std::string acks = "[";
    bool firstAck = true;
    for (JsonObject e : events) {
      const std::string id = e["client_event_id"] | "";
      const std::string seq = e["client_sequence"] | "";
      if (onEventSeen) onEventSeen(id);
      std::string canonical;
      JsonDocument copy;
      copy.set(e);
      copy.remove("client_sequence");
      serializeJson(copy, canonical);

      bool duplicate = false;
      bool rejected = false;
      uint64_t serverSeq = 0;
      auto it = byId.find(id);
      if (it != byId.end()) {
        RecordedEvent& old = recorded[it->second];
        if (old.body != canonical) violations.push_back("event body changed for id " + id);
        if (old.seq != seq) violations.push_back("event sequence changed for recorded id " + id);
        duplicate = true;
        rejected = old.rejected;
        serverSeq = old.serverSeq;
        ++duplicates;
      } else {
        RecordedEvent rec;
        rec.id = id;
        rec.seq = seq;
        rec.type = e["event_type"] | "";
        rec.body = canonical;
        rec.serverSeq = nextServerSeq++;
        rec.rejected = rejectText.count(e["payload"]["text"] | "") != 0;
        serverSeq = rec.serverSeq;
        rejected = rec.rejected;
        byId[id] = recorded.size();
        recorded.push_back(rec);
        usedSeq.insert(seq);
      }
      if (!firstAck) acks += ",";
      firstAck = false;
      acks += "{\"client_event_id\":\"" + id + "\",\"client_sequence\":\"" + seq + "\",\"server_sequence\":\"" +
              std::to_string(serverSeq) + "\",\"status\":\"" + (rejected ? "rejected" : "accepted") +
              "\",\"rejection_code\":" + (rejected ? "\"nope\"" : "null") +
              ",\"rejection_detail\":null,\"metadata\":{},\"duplicate\":" + (duplicate ? "true" : "false") + "}";
    }
    acks += "]";
    const uint64_t cursor = strtoull(doc["cursor"] | "0", nullptr, 10);
    if (cursor > committed) committed = cursor;
    return jsonReply(200, "{\"protocol_version\":2,\"acknowledgements\":" + acks + ",\"pull\":{" +
                              pageJson(cursor, doc["pull_limit"] | pageLimit) +
                              "},\"server_time\":\"2026-11-05T12:00:00.000Z\"}");
  }

  bool hasText(const std::string& text) const {
    for (const auto& e : recorded) {
      if (e.body.find("\"text\":\"" + text + "\"") != std::string::npos) return true;
    }
    return false;
  }
};

// JSON of a pulled progress event, in the server's shape.
inline std::string remoteProgress(uint64_t seq, const std::string& origin, const std::string& md5, double pct,
                                  const std::string& xpointer = "", const std::string& extraMeta = "",
                                  const std::string& directive = "suggest_resume") {
  std::string xp = xpointer.empty() ? "null" : "\"" + xpointer + "\"";
  std::string meta = extraMeta.empty() ? "{}" : extraMeta;
  return "{\"event_id\":\"evt-" + std::to_string(seq) + "\",\"server_sequence\":\"" + std::to_string(seq) +
         "\",\"origin_device\":{\"id\":\"" + origin + "\",\"name\":\"Kobo\",\"platform\":\"kobo\"},"
         "\"client_event_id\":\"30000000-0000-4000-8000-000000000001\",\"client_sequence\":\"1\",\"event_version\":1,"
         "\"event_type\":\"progress.changed\",\"aggregate_type\":\"reading_progress\",\"aggregate_id\":\"x\","
         "\"document_id\":null,\"book_identifier\":{\"kind\":\"koreader_partial_md5\",\"value\":\"" + md5 + "\"},"
         "\"occurred_at\":\"2026-11-05T10:00:00.000Z\",\"time_precision\":\"exact\",\"monotonic_ms\":null,"
         "\"payload\":{\"percentage\":" + std::to_string(pct) + ",\"current_page\":10,\"total_pages\":100,\"xpointer\":" + xp +
         ",\"explicit_jump\":false},\"received_at\":\"2026-11-05T10:00:01.000Z\",\"directive\":\"" + directive +
         "\",\"directive_metadata\":" + meta + "}";
}

// JSON of a focus_event, as getFocusedProgressEvent builds it.
inline std::string focusEventJson(const std::string& origin, const std::string& md5, double pct,
                                  const std::string& eventId = "evt-500", const std::string& xpointer = "/body/p[9].0",
                                  const std::string& precision = "exact") {
  std::string xp = xpointer.empty() ? "null" : "\"" + xpointer + "\"";
  return "{\"event_id\":\"" + eventId + "\",\"server_sequence\":\"500\",\"event_type\":\"progress.changed\",\"event_version\":1,"
         "\"document_id\":null,\"book_identifier\":{\"kind\":\"koreader_partial_md5\",\"value\":\"" + md5 + "\"},"
         "\"origin_device\":{\"id\":\"" + origin + "\",\"name\":\"Kobo Clara\",\"platform\":\"kobo\"},"
         "\"occurred_at\":\"2026-11-05T10:00:00.000Z\",\"time_precision\":\"" + precision + "\","
         "\"payload\":{\"percentage\":" + std::to_string(pct) + ",\"current_page\":10,\"total_pages\":100,\"xpointer\":" + xp +
         ",\"explicit_jump\":false,\"event_id\":\"" + eventId + "\",\"device_id\":\"" + origin + "\"},"
         "\"directive\":\"suggest_resume\",\"directive_metadata\":{\"reading_head\":true,\"source_is_stale\":false,"
         "\"source\":{\"percentage\":" + std::to_string(pct) + "}}}";
}

// ---------------------------------------------------------------------------------
// A device, with everything wired up.
// ---------------------------------------------------------------------------------

inline const uint8_t* testKey() {
  static const uint8_t key[6] = {0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6};
  return key;
}

struct Device {
  MemFs fs;
  FakeClock clock;
  FakeRandom random;
  FakeCancel cancel;
  HttpFake http;
  FakeServer server;

  std::unique_ptr<borges::Store> store;
  std::unique_ptr<borges::Outbox> outbox;
  std::unique_ptr<borges::SyncEngine> engine;
  borges::EngineConfig config;

  Device() {
    config.firmwareVersion = "1.6.1";
    config.clientVersion = "crossink-borges/1.6.1";
    http.handler = [this](const RecordedRequest& r) { return server.handle(r); };
    http.monoToAdvance = &clock.mono;
    http.advancePerRequestMs = 50;
  }

  void boot(bool paired = true) {
    engine.reset();
    outbox.reset();
    store = std::make_unique<borges::Store>(fs, testKey(), 6);
    ASSERT_TRUE(store->begin());
    outbox = std::make_unique<borges::Outbox>(fs, random, borges::kOutboxDir);
    engine = std::make_unique<borges::SyncEngine>(*store, *outbox, fs, http, clock, random, cancel, config);
    if (paired && !store->creds().paired()) {
      borges::copyText(store->creds().deviceId, sizeof(store->creds().deviceId), kSelfId);
      borges::copyText(store->creds().token, sizeof(store->creds().token), "toto_SECRET_TOKEN_VALUE");
      ASSERT_TRUE(store->saveCredentials());
    }
    ASSERT_TRUE(engine->begin());
  }

  // After a crash: everything in RAM is gone, and the disk may hold torn bytes.
  void crashAndBoot() {
    engine.reset();
    outbox.reset();
    store.reset();
    fs.reboot();
    boot();
  }

  static borges::BookId book(const char* md5 = kMd5A) {
    borges::BookId id;
    id.setHex(md5);
    return id;
  }

  static borges::BookRef bookRef(const char* md5 = kMd5A) {
    borges::BookRef ref;
    ref.id = book(md5);
    ref.title = "Ficciones";
    ref.author = "Borges";
    ref.file = "ficciones.epub";
    ref.totalPages = 200;
    return ref;
  }

  static borges::Position position(double pct, const char* xp = "/body/p[1].0", int page = 10) {
    borges::Position p;
    p.setPercent(pct);
    p.page = page;
    p.totalPages = 200;
    borges::copyText(p.xpointer, sizeof(p.xpointer), xp);
    return p;
  }

  // Appends a small valid annotation-like body directly to the outbox.
  borges::Outbox::AppendResult appendRaw(const std::string& text, std::string* idOut = nullptr, const char* md5 = kMd5A,
                                         size_t padding = 0, borges::EventKind kind = borges::EventKind::Annotation,
                                         bool replace = false) {
    const borges::BookId id = book(md5);
    std::string paddedText = text;
    std::string filler(padding, 'x');
    std::string typeName = kind == borges::EventKind::Progress ? "progress.changed" : "annotation.created";
    std::string body = "\"event_version\":1,\"event_type\":\"" + typeName +
                       "\",\"aggregate_type\":\"annotation\",\"aggregate_id\":\"a\",\"document_id\":null,"
                       "\"book_identifier\":{\"kind\":\"koreader_partial_md5\",\"value\":\"" +
                       std::string(md5) + "\"},\"occurred_at\":null,\"time_precision\":\"unknown\",\"payload\":{\"text\":\"" +
                       paddedText + "\",\"filler\":\"" + filler + "\"}";
    uint8_t raw[16];
    const auto result = outbox->append(kind, id.md5, body.data(), body.size(), replace, raw, nullptr);
    if (result == borges::Outbox::AppendResult::Ok && idOut) {
      char text36[37];
      borges::uuidFormat(raw, text36);
      *idOut = text36;
    }
    return result;
  }

  borges::FinalizeInput finalizeInput(double pct, const char* xp = "/body/p[1].0", int64_t turnEpoch = kNow) {
    borges::FinalizeInput input;
    input.book = bookRef();
    input.position = position(pct, xp);
    input.paged = true;
    input.lastTurn = WallTime{turnEpoch, TimePrecision::Exact};
    return input;
  }
};

// Everything the outbox would send, without changing its state beyond ATTEMPT records.
inline std::vector<std::pair<std::string, uint64_t>> liveEvents(borges::Outbox& outbox) {
  std::vector<std::pair<std::string, uint64_t>> out;
  for (size_t i = 0; i < outbox.depth(); ++i) {
    const borges::OutboxEntry* e = outbox.entryAt(i);
    borges::BatchItem item{};
    item.offset = e->offset;
    item.seq = e->seq;
    std::string buffer(e->payloadLen + 256, '\0');
    borges::JsonWriter w(buffer.data(), buffer.size());
    w.arrayBegin();
    if (outbox.writeEnvelope(item, w) != borges::Outbox::EnvelopeResult::Ok) continue;
    char id[37];
    borges::uuidFormat(item.id, id);
    out.emplace_back(id, item.seq);
  }
  return out;
}

}  // namespace borges_test

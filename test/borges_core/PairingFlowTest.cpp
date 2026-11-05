#include "TestSupport.h"

using namespace borges;
using namespace borges_test;

namespace {

constexpr const char* kRequestId = "60000000-0000-4000-8000-000000000001";

const char* kRequestReply =
    "{\"request_id\":\"60000000-0000-4000-8000-000000000001\",\"user_code\":\"ABCD-EFGH\","
    "\"verification_url\":\"/devices/pair\",\"expires_at\":\"2026-11-05T12:05:00.000Z\",\"expires_in\":300,\"interval\":5}";

std::string statusReply(const std::string& status) {
  return "{\"request_id\":\"" + std::string(kRequestId) + "\",\"status\":\"" + status +
         "\",\"expires_at\":\"2026-11-05T12:05:00.000Z\",\"approved_at\":null,\"device_id\":null,\"retryable\":true}";
}

std::string claimReply(const std::string& account = "account-1") {
  return "{\"request_id\":\"" + std::string(kRequestId) +
         "\",\"status\":\"claimed\",\"account\":{\"id\":\"" + account + "\",\"username\":\"toto\"},"
         "\"device\":{\"id\":\"" + kSelfId + "\",\"name\":\"X4\",\"platform\":\"crossink-x4\",\"external_id\":\"x4-1\",\"status\":\"active\",\"capabilities\":{}},"
         "\"credential\":{\"token\":\"toto_PAIRED_TOKEN\",\"id\":\"c\",\"scopes\":[\"sync:v2\"],\"username\":\"" + kSelfId + "\",\"token_prefix\":\"toto_\",\"created_at\":\"x\"},"
         "\"claimed_at\":\"x\"}";
}

class PairingFlowTest : public testing::Test {
 protected:
  void SetUp() override {
    d.boot(false);
    config.externalId = "x4-0011223344556677";
    config.firmwareVersion = "1.6.1";
    config.clientVersion = "crossink-borges/1.6.1";
    d.http.handler = nullptr;
    d.http.monoToAdvance = nullptr;
    flow = std::make_unique<PairingFlow>(*d.store, *d.outbox, d.fs, d.http, d.clock, d.random, d.cancel, config);
  }
  void advance(uint64_t seconds) { d.clock.mono += seconds * 1000; }

  bool startFlow() {
    d.http.script.push_back(jsonReply(201, kRequestReply));
    return flow->start();
  }

  Device d;
  PairingConfig config;
  std::unique_ptr<PairingFlow> flow;
};

}  // namespace

TEST_F(PairingFlowTest, RequestMatchesTheServersValidation) {
  ASSERT_TRUE(startFlow());
  EXPECT_EQ(flow->state(), PairingState::WaitingForApproval);
  ASSERT_EQ(d.http.log.size(), 1u);
  EXPECT_EQ(d.http.log[0].path, "/api/devices/pairing/request");
  EXPECT_EQ(d.http.log[0].headers.count("Authorization"), 0u);
  JsonDocument doc;
  ASSERT_FALSE(deserializeJson(doc, d.http.log[0].body));
  EXPECT_EQ(doc["platform"], "crossink-x4");
  EXPECT_EQ(doc["external_id"], "x4-0011223344556677");
  EXPECT_EQ(doc["device_name"], "Xteink X4");
  EXPECT_EQ(doc["client_version"], "crossink-borges/1.6.1");
  EXPECT_EQ(doc["protocol_version"], 2);
  EXPECT_TRUE(doc["user_id"].isUnbound());
  const std::string nonce = doc["device_nonce"].as<std::string>();
  EXPECT_GE(nonce.size(), 22u);
  EXPECT_LE(nonce.size(), 128u);
  for (char c : nonce) EXPECT_TRUE(isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') << c;
}

TEST_F(PairingFlowTest, ShowsTheCodeAndTheQrUrl) {
  ASSERT_TRUE(startFlow());
  EXPECT_STREQ(flow->userCode(), "ABCD-EFGH");
  EXPECT_STREQ(flow->qrUrl(), "https://borges.runadev.com/devices/pair?code=ABCD-EFGH");
  EXPECT_EQ(flow->intervalSec(), 5u);
}

TEST_F(PairingFlowTest, AVerificationUrlFromTheServerCannotRedirectTheQr) {
  d.http.script.push_back(jsonReply(201, "{\"request_id\":\"" + std::string(kRequestId) +
                                             "\",\"user_code\":\"ABCD-EFGH\",\"verification_url\":\"https://evil.example/pair\",\"expires_in\":300,\"interval\":5}"));
  ASSERT_TRUE(flow->start());
  EXPECT_STREQ(flow->qrUrl(), "https://borges.runadev.com/devices/pair?code=ABCD-EFGH");
}

TEST_F(PairingFlowTest, PollsAtTheServersIntervalAndNoFaster) {
  ASSERT_TRUE(startFlow());
  d.http.handler = [](const RecordedRequest&) { return jsonReply(200, statusReply("pending")); };
  const size_t afterStart = d.http.log.size();
  flow->step();
  advance(4);
  flow->step();
  EXPECT_EQ(d.http.log.size(), afterStart) << "not due yet";
  advance(1);
  flow->step();
  EXPECT_EQ(d.http.log.size(), afterStart + 1);
  flow->step();
  EXPECT_EQ(d.http.log.size(), afterStart + 1);
  EXPECT_EQ(d.http.log.back().path, "/api/devices/pairing/status");
  EXPECT_EQ(flow->state(), PairingState::WaitingForApproval);
}

TEST_F(PairingFlowTest, IntervalIsClamped) {
  d.http.script.push_back(jsonReply(201, "{\"request_id\":\"" + std::string(kRequestId) + "\",\"user_code\":\"ABCD-EFGH\",\"expires_in\":300,\"interval\":0}"));
  ASSERT_TRUE(flow->start());
  EXPECT_EQ(flow->intervalSec(), 3u);
  flow->cancelFlow();
  d.store->creds().pairing = PendingPairing{};
  d.http.script.push_back(jsonReply(201, "{\"request_id\":\"" + std::string(kRequestId) + "\",\"user_code\":\"ABCD-EFGH\",\"expires_in\":300,\"interval\":99999}"));
  ASSERT_TRUE(flow->start());
  EXPECT_EQ(flow->intervalSec(), 60u);
}

TEST_F(PairingFlowTest, ApprovedClaimsOnceAndFinishesAlone) {
  ASSERT_TRUE(startFlow());
  d.http.handler = [](const RecordedRequest& r) {
    if (r.path == "/api/devices/pairing/status") return jsonReply(200, statusReply("approved"));
    if (r.path == "/api/devices/pairing/claim") return jsonReply(200, claimReply());
    return apiError(500, "unexpected");
  };
  advance(5);
  EXPECT_EQ(flow->step(), PairingState::Done);
  EXPECT_EQ(d.http.countPath("/api/devices/pairing/claim"), 1u);

  EXPECT_TRUE(d.store->creds().paired());
  EXPECT_STREQ(d.store->creds().token, "toto_PAIRED_TOKEN");
  EXPECT_STREQ(d.store->creds().deviceId, kSelfId);
  EXPECT_STREQ(d.store->creds().accountId, "account-1");
  EXPECT_FALSE(d.store->creds().pairing.active());

  // Durable.
  d.store = std::make_unique<Store>(d.fs, testKey(), 6);
  ASSERT_TRUE(d.store->begin());
  EXPECT_TRUE(d.store->creds().paired());
  EXPECT_STREQ(d.store->creds().token, "toto_PAIRED_TOKEN");

  const size_t requests = d.http.log.size();
  advance(60);
  flow->step();
  EXPECT_EQ(d.http.log.size(), requests) << "finished means finished";
}

TEST_F(PairingFlowTest, NeverClaimsBeforeApproved) {
  ASSERT_TRUE(startFlow());
  d.http.handler = [](const RecordedRequest&) { return jsonReply(200, statusReply("pending")); };
  for (int i = 0; i < 5; ++i) {
    advance(5);
    flow->step();
  }
  EXPECT_EQ(d.http.countPath("/api/devices/pairing/claim"), 0u);
}

TEST_F(PairingFlowTest, StatusAndClaimCarryTheProof) {
  ASSERT_TRUE(startFlow());
  JsonDocument first;
  ASSERT_FALSE(deserializeJson(first, d.http.log[0].body));
  const std::string nonce = first["device_nonce"].as<std::string>();
  d.http.handler = [](const RecordedRequest& r) {
    return r.path == "/api/devices/pairing/status" ? jsonReply(200, statusReply("approved")) : jsonReply(200, claimReply());
  };
  advance(5);
  flow->step();
  for (size_t i = 1; i < d.http.log.size(); ++i) {
    JsonDocument doc;
    ASSERT_FALSE(deserializeJson(doc, d.http.log[i].body));
    EXPECT_EQ(doc["request_id"], kRequestId);
    EXPECT_EQ(doc["device_nonce"].as<std::string>(), nonce);
  }
}

TEST_F(PairingFlowTest, Rejected) {
  ASSERT_TRUE(startFlow());
  d.http.script.push_back(jsonReply(200, statusReply("rejected")));
  advance(5);
  EXPECT_EQ(flow->step(), PairingState::Rejected);
  EXPECT_FALSE(d.store->creds().paired());
  EXPECT_FALSE(d.store->creds().pairing.active());
}

TEST_F(PairingFlowTest, RejectedAtClaimTime) {
  ASSERT_TRUE(startFlow());
  d.http.script.push_back(jsonReply(200, statusReply("approved")));
  d.http.script.push_back(apiError(403, "pairing_rejected"));
  advance(5);
  EXPECT_EQ(flow->step(), PairingState::Rejected);
}

TEST_F(PairingFlowTest, ExpiredByTheServer) {
  ASSERT_TRUE(startFlow());
  d.http.script.push_back(jsonReply(200, statusReply("expired")));
  advance(5);
  EXPECT_EQ(flow->step(), PairingState::Expired);
  EXPECT_FALSE(d.store->creds().pairing.active());
}

TEST_F(PairingFlowTest, ExpiredWithA410OrAnUnknownRequest) {
  for (int status : {410, 404}) {
    d.store->creds().pairing = PendingPairing{};
    flow->cancelFlow();
    ASSERT_TRUE(startFlow());
    d.http.script.push_back(apiError(status, status == 410 ? "pairing_expired" : "pairing_not_found"));
    advance(5);
    EXPECT_EQ(flow->step(), PairingState::Expired) << status;
  }
}

TEST_F(PairingFlowTest, ExpiresLocallyWithoutAskingAnyone) {
  ASSERT_TRUE(startFlow());
  const size_t requests = d.http.log.size();
  advance(301);
  EXPECT_EQ(flow->step(), PairingState::Expired);
  EXPECT_EQ(d.http.log.size(), requests);
}

TEST_F(PairingFlowTest, ExpiredCanStartOver) {
  ASSERT_TRUE(startFlow());
  advance(301);
  flow->step();
  ASSERT_TRUE(startFlow());
  EXPECT_EQ(flow->state(), PairingState::WaitingForApproval);
  EXPECT_EQ(d.http.countPath("/api/devices/pairing/request"), 2u);
}

TEST_F(PairingFlowTest, RateLimitAtRequestWaitsAndRetries) {
  Reply limited = apiError(429, "rate_limited");
  limited.hasRetryAfter = true;
  limited.retryAfter = 120;
  d.http.script.push_back(limited);
  EXPECT_FALSE(flow->start());
  EXPECT_EQ(flow->state(), PairingState::Idle);
  EXPECT_TRUE(flow->rateLimited());
  EXPECT_STREQ(flow->lastError().code, "rate_limited");

  flow->step();
  advance(100);
  flow->step();
  EXPECT_EQ(d.http.log.size(), 1u);
  advance(21);
  d.http.script.push_back(jsonReply(201, kRequestReply));
  EXPECT_EQ(flow->step(), PairingState::WaitingForApproval);
}

TEST_F(PairingFlowTest, AHugeRetryAfterCannotPostponeTheRetryBeyondTheCap) {
  Reply limited = apiError(429, "rate_limited");
  limited.hasRetryAfter = true;
  limited.retryAfter = 0xFFFFFFFFu;
  d.http.script.push_back(limited);
  EXPECT_FALSE(flow->start());
  EXPECT_LE(flow->nextPollAtMs(), d.clock.mono + 6ull * 3600 * 1000);
}

TEST_F(PairingFlowTest, RateLimitWhilePollingStretchesTheWait) {
  ASSERT_TRUE(startFlow());
  Reply limited = apiError(429, "rate_limited");
  limited.hasRetryAfter = true;
  limited.retryAfter = 30;
  d.http.script.push_back(limited);
  advance(5);
  EXPECT_EQ(flow->step(), PairingState::WaitingForApproval);
  EXPECT_TRUE(flow->rateLimited());
  const size_t requests = d.http.log.size();
  advance(29);
  flow->step();
  EXPECT_EQ(d.http.log.size(), requests);
  d.http.script.push_back(jsonReply(200, statusReply("pending")));
  advance(2);
  flow->step();
  EXPECT_EQ(d.http.log.size(), requests + 1);
  EXPECT_FALSE(flow->rateLimited());
}

TEST_F(PairingFlowTest, NetworkTroubleKeepsWaitingAndBacksOff) {
  ASSERT_TRUE(startFlow());
  d.http.handler = [](const RecordedRequest&) { return dropped(); };
  advance(5);
  EXPECT_EQ(flow->step(), PairingState::WaitingForApproval);
  const uint64_t firstWait = flow->nextPollAtMs() - d.clock.mono;
  EXPECT_EQ(firstWait, 10000u);
  advance(10);
  EXPECT_EQ(flow->step(), PairingState::WaitingForApproval);
  EXPECT_EQ(flow->nextPollAtMs() - d.clock.mono, 20000u);
  EXPECT_STREQ(flow->lastError().code, "transport");

  d.http.handler = [](const RecordedRequest&) { return jsonReply(200, statusReply("pending")); };
  advance(20);
  flow->step();
  EXPECT_EQ(flow->nextPollAtMs() - d.clock.mono, 5000u) << "recovers its pace";
}

TEST_F(PairingFlowTest, AuthorizationPendingAtClaimGoesBackToWaiting) {
  ASSERT_TRUE(startFlow());
  d.http.script.push_back(jsonReply(200, statusReply("approved")));
  Reply pending = apiError(409, "authorization_pending");
  pending.hasRetryAfter = true;
  pending.retryAfter = 5;
  d.http.script.push_back(pending);
  advance(5);
  EXPECT_EQ(flow->step(), PairingState::WaitingForApproval);
  EXPECT_FALSE(d.store->creds().paired());
}

TEST_F(PairingFlowTest, LostClaimResponseIsReportedForANewCode) {
  ASSERT_TRUE(startFlow());
  d.http.script.push_back(jsonReply(200, statusReply("approved")));
  d.http.script.push_back(apiError(409, "pairing_already_claimed"));
  advance(5);
  EXPECT_EQ(flow->step(), PairingState::ClaimLost);
  EXPECT_FALSE(d.store->creds().pairing.active());

  // Same thing arriving through status, after a reboot mid-claim.
  ASSERT_TRUE(startFlow());
  d.http.script.push_back(jsonReply(200, statusReply("claimed")));
  advance(5);
  EXPECT_EQ(flow->step(), PairingState::ClaimLost);
}

TEST_F(PairingFlowTest, MalformedAnswersDoNotCrashOrPair) {
  d.http.script.push_back(jsonReply(201, "not json"));
  EXPECT_FALSE(flow->start());
  EXPECT_EQ(flow->state(), PairingState::Failed);

  d.http.script.push_back(jsonReply(201, "{\"request_id\":\"../../x\",\"user_code\":\"ABCD-EFGH\"}"));
  EXPECT_FALSE(flow->start());

  ASSERT_TRUE(startFlow());
  d.http.script.push_back(jsonReply(200, "{{{"));
  advance(5);
  EXPECT_EQ(flow->step(), PairingState::WaitingForApproval);

  flow->step();
  d.http.script.push_back(jsonReply(200, statusReply("approved")));
  d.http.script.push_back(jsonReply(200, "{\"credential\":{\"id\":\"no-token\"}}"));
  advance(60);
  EXPECT_EQ(flow->step(), PairingState::ClaimLost);
  EXPECT_FALSE(d.store->creds().paired());
}

TEST_F(PairingFlowTest, ResumesAStoredCodeAfterAReboot) {
  ASSERT_TRUE(startFlow());
  JsonDocument first;
  ASSERT_FALSE(deserializeJson(first, d.http.log[0].body));

  d.boot(false);
  PairingFlow resumed(*d.store, *d.outbox, d.fs, d.http, d.clock, d.random, d.cancel, config);
  ASSERT_TRUE(resumed.start());
  EXPECT_EQ(d.http.countPath("/api/devices/pairing/request"), 1u) << "no new code";
  EXPECT_EQ(resumed.state(), PairingState::WaitingForApproval);
  EXPECT_STREQ(resumed.qrUrl(), "https://borges.runadev.com/devices/pair?code=ABCD-EFGH");

  d.http.script.push_back(jsonReply(200, statusReply("pending")));
  resumed.step();
  JsonDocument status;
  ASSERT_FALSE(deserializeJson(status, d.http.log.back().body));
  EXPECT_EQ(status["device_nonce"].as<std::string>(), first["device_nonce"].as<std::string>());
}

TEST_F(PairingFlowTest, NonceIsNotInTheClearOnTheCard) {
  ASSERT_TRUE(startFlow());
  JsonDocument first;
  ASSERT_FALSE(deserializeJson(first, d.http.log[0].body));
  const std::string nonce = first["device_nonce"].as<std::string>();
  for (const std::string& path : d.fs.paths()) {
    EXPECT_EQ(d.fs.content(path).find(nonce), std::string::npos) << path;
  }
}

TEST_F(PairingFlowTest, PairingUnderAnotherAccountStartsClean) {
  copyText(d.store->creds().accountId, sizeof(d.store->creds().accountId), "account-old");
  ASSERT_EQ(d.appendRaw("from-the-old-account"), Outbox::AppendResult::Ok);
  d.store->state().cursor = 44;
  ASSERT_TRUE(startFlow());
  d.http.script.push_back(jsonReply(200, statusReply("approved")));
  d.http.script.push_back(jsonReply(200, claimReply("account-new")));
  advance(5);
  EXPECT_EQ(flow->step(), PairingState::Done);
  EXPECT_EQ(d.outbox->depth(), 0u);
  EXPECT_EQ(d.store->state().cursor, 0u);
  EXPECT_STREQ(d.store->creds().accountId, "account-new");
  EXPECT_EQ(d.outbox->nextSequence(), 2u) << "the device row is reused: keep counting";
}

TEST_F(PairingFlowTest, PairingUnderTheSameAccountKeepsTheQueue) {
  copyText(d.store->creds().accountId, sizeof(d.store->creds().accountId), "account-1");
  ASSERT_EQ(d.appendRaw("kept"), Outbox::AppendResult::Ok);
  d.store->state().cursor = 44;
  d.store->state().needsRelink = true;
  ASSERT_TRUE(startFlow());
  d.http.script.push_back(jsonReply(200, statusReply("approved")));
  d.http.script.push_back(jsonReply(200, claimReply("account-1")));
  advance(5);
  EXPECT_EQ(flow->step(), PairingState::Done);
  EXPECT_EQ(d.outbox->depth(), 1u);
  EXPECT_EQ(d.store->state().cursor, 44u);
  EXPECT_FALSE(d.store->state().needsRelink) << "\"Volve a conectar\" is resolved";
}

TEST_F(PairingFlowTest, AlreadyPairedIsDone) {
  copyText(d.store->creds().deviceId, sizeof(d.store->creds().deviceId), kSelfId);
  copyText(d.store->creds().token, sizeof(d.store->creds().token), "t");
  EXPECT_TRUE(flow->start());
  EXPECT_EQ(flow->state(), PairingState::Done);
  EXPECT_TRUE(d.http.log.empty());
}

TEST(PairingIdentity, ExternalIdIsStablePrefixedAndHidesTheMac) {
  const uint8_t mac[6] = {0x30, 0xAE, 0xA4, 0x12, 0x34, 0x56};
  char a[20], b[20];
  makeExternalId(mac, a);
  makeExternalId(mac, b);
  EXPECT_STREQ(a, b);
  EXPECT_EQ(strncmp(a, "x4-", 3), 0);
  EXPECT_EQ(strlen(a), 19u);
  EXPECT_EQ(std::string(a).find("30aea4"), std::string::npos);
  const uint8_t other[6] = {0x30, 0xAE, 0xA4, 0x12, 0x34, 0x57};
  makeExternalId(other, b);
  EXPECT_STRNE(a, b);
  for (const char* p = a; *p; ++p) {
    EXPECT_TRUE(isalnum(static_cast<unsigned char>(*p)) || *p == '-') << *p;
  }
}

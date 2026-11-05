#pragma once

// IHttp over the SDK's SecureHttpClient: strict verification against BorgesTrustAnchors,
// dialed by IP with SNI and certificate name on the hostname, one keep-alive connection
// per session, streaming into the sink. Not bound to any task.
//
// A cancel that fires is the only thing that interrupts a blocking step, and IHttp has
// no budget argument, so pass a DeadlineCancel (BorgesDevice.h) to bound a session.

#include <SecureHttpClient.h>

#include "Ports.h"

namespace borges {
namespace net {

class BorgesClock;
class BorgesDns;

class BorgesHttp : public IHttp {
 public:
  // clock may be null (no tls_clock repair). userAgent must outlive the object.
  BorgesHttp(IFs& fs, BorgesDns& dns, BorgesClock* clock, const char* userAgent);

  void perform(const HttpRequest& request, IHttpSink& sink, ICancel& abort, HttpResponse& out) override;
  void disconnect() override;

 private:
  IFs& fs_;
  BorgesDns& dns_;
  BorgesClock* clock_;
  const char* userAgent_;
  freeink::SecureHttpClient http_;
  bool clockRepairUsed_ = false;
};

}  // namespace net
}  // namespace borges

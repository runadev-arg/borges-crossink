#pragma once

// The Wi-Fi radio for one Borges session. Picks the network itself from
// WifiCredentialStore; callers never pass an SSID. Rule 13: every path that does
// not return Connected has already turned the radio off, disconnect() always does,
// and the destructor calls it.
//
// Not bound to any task: call it from whichever task runs the session, but only one
// at a time.

#include <cstdint>

#include "Ports.h"
#include "net/NetFailure.h"

namespace borges {
namespace net {

class BorgesLink {
 public:
  explicit BorgesLink(IFs& fs);
  ~BorgesLink();
  BorgesLink(const BorgesLink&) = delete;
  BorgesLink& operator=(const BorgesLink&) = delete;

  // Blocks, polling cancel about every 50 ms, for at most budgetMs. A known network
  // uses its stored channel and BSSID (~1-2 s); otherwise a scan of the saved list.
  // Leaves power save off (WIFI_PS_NONE) for the exchange. A failure is also written
  // to the NetDiag file.
  LinkResult connect(uint32_t budgetMs, ICancel& cancel);

  // Always: disconnect, station off, WIFI_OFF. Safe to call twice.
  void disconnect();
  bool connected() const;

 private:
  IFs& fs_;
};

}  // namespace net
}  // namespace borges

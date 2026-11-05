#pragma once

// The HTTP Date header, and the window a wall clock must fall in to be believed.

#include <cstddef>
#include <cstdint>

namespace borges {
namespace net {

constexpr int64_t kMinTrustedEpoch = 1767225600;  // 2026-01-01T00:00:00Z
constexpr int64_t kMaxTrustedEpoch = 2398377599;  // 2045-12-31T23:59:59Z
inline bool epochInBounds(const int64_t epoch) { return epoch >= kMinTrustedEpoch && epoch <= kMaxTrustedEpoch; }

// RFC 9110 IMF-fixdate only: "Sun, 06 Nov 1994 08:49:37 GMT". The obsolete forms are
// refused, as is a weekday that does not match the date, so that a captive portal's
// creative header cannot set the clock. False leaves out untouched.
bool parseHttpDate(const char* text, size_t len, int64_t& out);

}  // namespace net
}  // namespace borges

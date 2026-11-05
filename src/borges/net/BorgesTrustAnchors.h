#pragma once

// The roots Borges trusts, as PEM in flash. Fetched from each authority; see the .cpp
// for the fingerprints.

#include <cstddef>

namespace borges {
namespace net {

constexpr size_t kTrustAnchorCount = 5;
const char* const* trustAnchors();
const char* trustAnchorName(size_t index);

}  // namespace net
}  // namespace borges

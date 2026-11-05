#pragma once

// DNS over TCP (RFC 1035 4.2.2, RFC 7766): the query and the answer, with no sockets.

#include <cstddef>
#include <cstdint>

namespace borges {
namespace net {

constexpr size_t kDnsMaxMessage = 512;
constexpr size_t kDnsMaxQuery = 2 + 12 + 255 + 4;  // Length prefix, header, QNAME, QTYPE+QCLASS.

enum class DnsParse : uint8_t { Ok, TooShort, BadId, NotAResponse, ServerError, Malformed, WrongQuestion, NoAddress };

// A query for the A record, recursion desired, preceded by the two-byte length.
// Returns the bytes written, 0 when the name is invalid or out is too small.
size_t buildDnsQuery(const char* host, uint16_t id, uint8_t* out, size_t cap);

// A message without its length prefix. Writes the first A/IN answer. The question must
// echo the name we asked for: an answer about another name is not an answer.
DnsParse parseDnsAnswer(const uint8_t* message, size_t len, uint16_t id, const char* host, uint8_t addr[4]);

// False for addresses no public server can have: 0/8, 127/8, 169.254/16, multicast and
// reserved. Private ranges pass, because a self-hosted server can live on one.
bool plausibleServerAddress(const uint8_t addr[4]);

}  // namespace net
}  // namespace borges

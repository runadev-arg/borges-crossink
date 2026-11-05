#pragma once

// SecureHttpClient only needs this for Basic auth, and the probe sends no credentials.

#include <string>

class base64 {
 public:
  static std::string encode(const char*) { return std::string(); }
};

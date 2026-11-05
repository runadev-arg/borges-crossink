#pragma once

// A real TCP client on Winsock, with the semantics of NetworkClient that SecureClient
// relies on: non-blocking read, available(), connected() that notices a closed peer.
// It also reads the TLS record headers going past, which no wolfSSL API reports.

#include <algorithm>

#include "Client.h"

struct ProbeNet {
  static inline int connects = 0;       // TCP connects since reset.
  static inline uint16_t blocked[4] = {0, 0, 0, 0};  // Ports that refuse, to simulate a filtered network.
  static inline size_t maxRecordIn = 0;
  static inline size_t recordsIn = 0;
  static inline size_t bytesIn = 0;
  static inline size_t bytesOut = 0;
  static inline SOCKET lastSocket = INVALID_SOCKET;
  static bool isBlocked(uint16_t port) {
    return std::find(std::begin(blocked), std::end(blocked), port) != std::end(blocked);
  }
  static void resetCounters() {
    connects = 0;
    maxRecordIn = recordsIn = bytesIn = bytesOut = 0;
  }
  // Pulls the rug out from under the live connection, as a server closing would.
  static void dropLive() {
    if (lastSocket != INVALID_SOCKET) shutdown(lastSocket, SD_BOTH);
  }
};

class WiFiClient : public Client {
 public:
  ~WiFiClient() override { stop(); }

  void setConnectionTimeout(unsigned long ms) { connectTimeoutMs_ = ms; }

  int connect(IPAddress ip, uint16_t port) override {
    stop();
    if (ProbeNet::isBlocked(port)) return 0;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    const uint8_t octets[4] = {ip[0], ip[1], ip[2], ip[3]};
    memcpy(&addr.sin_addr, octets, 4);
    return dial(addr);
  }

  int connect(const char* host, uint16_t port) override {
    stop();
    if (ProbeNet::isBlocked(port)) return 0;
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* found = nullptr;
    if (getaddrinfo(host, nullptr, &hints, &found) != 0 || found == nullptr) return 0;
    sockaddr_in addr = *reinterpret_cast<const sockaddr_in*>(found->ai_addr);
    addr.sin_port = htons(port);
    freeaddrinfo(found);
    return dial(addr);
  }

  size_t write(uint8_t b) override { return write(&b, 1); }
  size_t write(const uint8_t* buf, size_t size) override {
    if (fd_ == INVALID_SOCKET) return 0;
    size_t sent = 0;
    const unsigned long start = millis();
    while (sent < size) {
      const int n = send(fd_, reinterpret_cast<const char*>(buf + sent), static_cast<int>(size - sent), 0);
      if (n > 0) {
        sent += static_cast<size_t>(n);
      } else if (WSAGetLastError() == WSAEWOULDBLOCK && millis() - start < connectTimeoutMs_) {
        Sleep(1);
      } else {
        break;
      }
    }
    ProbeNet::bytesOut += sent;
    return sent;
  }
  int available() override {
    if (fd_ == INVALID_SOCKET) return 0;
    u_long pending = 0;
    ioctlsocket(fd_, FIONREAD, &pending);
    return static_cast<int>(pending);
  }
  int read() override {
    uint8_t b;
    return read(&b, 1) == 1 ? b : -1;
  }
  int read(uint8_t* buf, size_t size) override {
    if (fd_ == INVALID_SOCKET) return -1;
    const int n = recv(fd_, reinterpret_cast<char*>(buf), static_cast<int>(size), 0);
    if (n > 0) sniff(buf, static_cast<size_t>(n));
    return n > 0 ? n : -1;
  }
  int peek() override { return -1; }
  void flush() override {}
  void stop() override {
    if (fd_ != INVALID_SOCKET) {
      closesocket(fd_);
      if (ProbeNet::lastSocket == fd_) ProbeNet::lastSocket = INVALID_SOCKET;
      fd_ = INVALID_SOCKET;
    }
  }
  uint8_t connected() override {
    if (fd_ == INVALID_SOCKET) return 0;
    char c;
    const int n = recv(fd_, &c, 1, MSG_PEEK);
    if (n > 0) return 1;
    if (n == 0) return 0;
    return WSAGetLastError() == WSAEWOULDBLOCK ? 1 : 0;
  }
  operator bool() override { return connected() != 0; }

 private:
  int dial(const sockaddr_in& addr) {
    fd_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd_ == INVALID_SOCKET) return 0;
    u_long nonblocking = 1;
    ioctlsocket(fd_, FIONBIO, &nonblocking);
    if (::connect(fd_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0 &&
        WSAGetLastError() != WSAEWOULDBLOCK) {
      stop();
      return 0;
    }
    fd_set writable, failed;
    FD_ZERO(&writable);
    FD_ZERO(&failed);
    FD_SET(fd_, &writable);
    FD_SET(fd_, &failed);
    timeval tv{static_cast<long>(connectTimeoutMs_ / 1000), static_cast<long>((connectTimeoutMs_ % 1000) * 1000)};
    if (select(0, nullptr, &writable, &failed, &tv) <= 0 || FD_ISSET(fd_, &failed)) {
      stop();
      return 0;
    }
    int one = 1;
    setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
    ++ProbeNet::connects;
    ProbeNet::lastSocket = fd_;
    headerGot_ = 0;
    bodyLeft_ = 0;
    return 1;
  }

  void sniff(const uint8_t* data, size_t len) {
    ProbeNet::bytesIn += len;
    while (len > 0) {
      if (bodyLeft_ > 0) {
        const size_t take = std::min(bodyLeft_, len);
        bodyLeft_ -= take;
        data += take;
        len -= take;
        continue;
      }
      header_[headerGot_++] = *data++;
      --len;
      if (headerGot_ == 5) {
        bodyLeft_ = (static_cast<size_t>(header_[3]) << 8) | header_[4];
        ProbeNet::maxRecordIn = std::max(ProbeNet::maxRecordIn, bodyLeft_);
        ++ProbeNet::recordsIn;
        headerGot_ = 0;
      }
    }
  }

  SOCKET fd_ = INVALID_SOCKET;
  unsigned long connectTimeoutMs_ = 5000;
  uint8_t header_[5] = {0, 0, 0, 0, 0};
  size_t headerGot_ = 0;
  size_t bodyLeft_ = 0;
};

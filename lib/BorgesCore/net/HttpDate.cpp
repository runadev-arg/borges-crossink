#include "net/HttpDate.h"

#include <cstring>

namespace borges {
namespace net {
namespace {

bool digits(const char* s, const int count, int& out) {
  int value = 0;
  for (int i = 0; i < count; ++i) {
    if (s[i] < '0' || s[i] > '9') return false;
    value = value * 10 + (s[i] - '0');
  }
  out = value;
  return true;
}

// Howard Hinnant's days_from_civil: days since 1970-01-01.
int64_t daysFromCivil(int y, const int m, const int d) {
  y -= m <= 2 ? 1 : 0;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const int yoe = y - era * 400;
  const int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return static_cast<int64_t>(era) * 146097 + doe - 719468;
}

int daysInMonth(const int y, const int m) {
  static constexpr int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
  return kDays[m - 1];
}

}  // namespace

bool parseHttpDate(const char* text, size_t len, int64_t& out) {
  while (len > 0 && (*text == ' ' || *text == '\t')) {
    ++text;
    --len;
  }
  while (len > 0 && (text[len - 1] == ' ' || text[len - 1] == '\t' || text[len - 1] == '\r')) --len;
  // 0         1         2
  // 01234567890123456789012345678
  // Sun, 06 Nov 1994 08:49:37 GMT
  if (len != 29) return false;
  if (text[3] != ',' || text[4] != ' ' || text[7] != ' ' || text[11] != ' ' || text[16] != ' ' || text[19] != ':' ||
      text[22] != ':' || text[25] != ' ' || memcmp(text + 26, "GMT", 3) != 0) {
    return false;
  }

  static constexpr char kMonths[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
  int month = 0;
  for (int i = 0; i < 12; ++i) {
    if (memcmp(text + 8, kMonths + i * 3, 3) == 0) month = i + 1;
  }
  int day, year, hour, minute, second;
  if (month == 0 || !digits(text + 5, 2, day) || !digits(text + 12, 4, year) || !digits(text + 17, 2, hour) ||
      !digits(text + 20, 2, minute) || !digits(text + 23, 2, second)) {
    return false;
  }
  if (day < 1 || day > daysInMonth(year, month) || hour > 23 || minute > 59 || second > 60) return false;

  const int64_t days = daysFromCivil(year, month, day);
  // 1970-01-01 was a Thursday.
  static constexpr char kWeekdays[] = "ThuFriSatSunMonTueWed";
  const int weekday = static_cast<int>(((days % 7) + 7) % 7);
  if (memcmp(text, kWeekdays + weekday * 3, 3) != 0) return false;

  out = days * 86400 + hour * 3600 + minute * 60 + second;
  return true;
}

}  // namespace net
}  // namespace borges

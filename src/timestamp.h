// Internal: ISO 8601 (RFC 3339) timestamps, to and from counts of seconds,
// milliseconds, microseconds or nanoseconds since 1970-01-01T00:00:00Z.
#ifndef SIMDJSON_ARROW_TIMESTAMP_H
#define SIMDJSON_ARROW_TIMESTAMP_H

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace simdjson_arrow {
namespace internal {

// Units per second, by unit (second, millisecond, microsecond, nanosecond).
constexpr int64_t kUnitsPerSecond[4] = {1, 1000, 1000000, 1000000000};
constexpr int kFractionDigits[4] = {0, 3, 6, 9};

// Days from 1970-01-01 to the date (proleptic Gregorian calendar), after
// Howard Hinnant's days_from_civil.
constexpr int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

// The inverse of days_from_civil.
inline void civil_from_days(int64_t z, int64_t* year, unsigned* month, unsigned* day) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  *day = doy - (153 * mp + 2) / 5 + 1;
  *month = mp < 10 ? mp + 3 : mp - 9;
  *year = static_cast<int64_t>(yoe) + era * 400 + (*month <= 2);
}

inline unsigned days_in_month(int64_t y, unsigned m) {
  static const unsigned days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
  return m == 2 && leap ? 29 : days[m - 1];
}

// Reads n digits at s[*i] into *out.
inline bool read_digits(std::string_view s, size_t* i, int n, unsigned* out) {
  if (s.size() - *i < static_cast<size_t>(n)) return false;
  unsigned v = 0;
  for (int k = 0; k < n; k++) {
    const unsigned c = static_cast<unsigned char>(s[*i + k]) - '0';
    if (c > 9) return false;
    v = v * 10 + c;
  }
  *i += n;
  *out = v;
  return true;
}

// Parses a timestamp: a date YYYY-MM-DD, then optionally T (or t, or a space)
// and a time HH, HH:MM, HH:MM:SS or HH:MM:SS.fraction (with . or ,), then
// optionally Z (or z) or an offset +HH:MM, +HHMM or +HH (or with -). Without
// Z or an offset, the time is taken as UTC. Gives the count of units (0 to 3:
// seconds to nanoseconds) since the epoch. Returns false if s is not such a
// timestamp, if its fraction has more digits than the unit (as Arrow's JSON
// reader does: we do not lose precision quietly), or if the count does not
// fit in 64 bits.
inline bool parse_timestamp(std::string_view s, int unit, int64_t* out) {
  size_t i = 0;
  unsigned year, month, day, hour = 0, minute = 0, second = 0;
  if (!read_digits(s, &i, 4, &year) || i >= s.size() || s[i++] != '-' ||
      !read_digits(s, &i, 2, &month) || i >= s.size() || s[i++] != '-' ||
      !read_digits(s, &i, 2, &day)) {
    return false;
  }
  if (month < 1 || month > 12 || day < 1 || day > days_in_month(year, month)) return false;
  int64_t fraction = 0;  // in units
  int64_t offset = 0;    // in seconds, to subtract
  if (i < s.size() && (s[i] == 'T' || s[i] == 't' || s[i] == ' ')) {
    i++;
    if (!read_digits(s, &i, 2, &hour)) return false;
    if (i < s.size() && s[i] == ':') {
      i++;
      if (!read_digits(s, &i, 2, &minute)) return false;
      if (i < s.size() && s[i] == ':') {
        i++;
        if (!read_digits(s, &i, 2, &second)) return false;
        if (i < s.size() && (s[i] == '.' || s[i] == ',')) {
          i++;
          const size_t start = i;
          int64_t scale = kUnitsPerSecond[unit];
          while (i < s.size() && static_cast<unsigned>(s[i] - '0') <= 9) {
            if (i - start == static_cast<size_t>(kFractionDigits[unit])) return false;
            scale /= 10;
            fraction += (s[i] - '0') * scale;
            i++;
          }
          if (i == start) return false;
        }
      }
    }
    if (hour > 23 || minute > 59 || second > 59) return false;
    if (i < s.size() && (s[i] == 'Z' || s[i] == 'z')) {
      i++;
    } else if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
      const bool negative = s[i++] == '-';
      unsigned oh, om = 0;
      if (!read_digits(s, &i, 2, &oh)) return false;
      if (i < s.size() && s[i] == ':') {
        i++;
        if (!read_digits(s, &i, 2, &om)) return false;
      } else if (i < s.size()) {
        if (!read_digits(s, &i, 2, &om)) return false;
      }
      if (oh > 23 || om > 59) return false;
      offset = (negative ? -1 : 1) * static_cast<int64_t>(oh * 3600 + om * 60);
    }
  }
  if (i != s.size()) return false;
  const int64_t seconds = days_from_civil(year, month, day) * 86400 +
                          static_cast<int64_t>(hour * 3600 + minute * 60 + second) - offset;
  const int64_t per = kUnitsPerSecond[unit];
  if (seconds > (INT64_MAX - fraction) / per) return false;
  if (seconds < INT64_MIN / per) {
    // Only the second just below INT64_MIN / per can still fit, with a fraction.
    if (seconds < INT64_MIN / per - 1) return false;
    const int64_t above = (seconds + 1) * per;
    if (above - INT64_MIN < per - fraction) return false;
    *out = above - (per - fraction);
    return true;
  }
  *out = seconds * per + fraction;
  return true;
}

inline char* write_two(char* p, unsigned v) {
  p[0] = static_cast<char>('0' + v / 10);
  p[1] = static_cast<char>('0' + v % 10);
  return p + 2;
}

// Writes the date, given in days since the epoch, as YYYY-MM-DD (with more
// digits, or a minus sign, for years outside 0 to 9999). Returns the end of
// what it wrote: at most 22 bytes.
inline char* write_date(char* p, int64_t days) {
  int64_t year;
  unsigned month, day;
  civil_from_days(days, &year, &month, &day);
  uint64_t y = static_cast<uint64_t>(year);
  if (year < 0) {
    *p++ = '-';
    y = 0 - y;
  }
  char digits[20];
  int n = 0;
  do {
    digits[n++] = static_cast<char>('0' + y % 10);
    y /= 10;
  } while (y != 0 || n < 4);
  while (n > 0) *p++ = digits[--n];
  *p++ = '-';
  p = write_two(p, month);
  *p++ = '-';
  return write_two(p, day);
}

// Splits a count of units since the epoch into days and units within the day.
inline void split_days(int64_t value, int64_t per_day, int64_t* days, int64_t* in_day) {
  *days = value / per_day;
  *in_day = value % per_day;
  if (*in_day < 0) {
    *in_day += per_day;
    --*days;
  }
}

// Writes the timestamp (a count of units since the epoch) as
// YYYY-MM-DDTHH:MM:SS, with the fraction of a second (as many digits as the
// unit has) when it is not zero, then Z when utc. Returns the end of what it
// wrote: at most 40 bytes.
inline char* write_timestamp(char* p, int64_t value, int unit, bool utc) {
  const int64_t per = kUnitsPerSecond[unit];
  int64_t seconds, fraction, days, in_day;
  split_days(value, per, &seconds, &fraction);
  split_days(seconds, 86400, &days, &in_day);
  p = write_date(p, days);
  *p++ = 'T';
  p = write_two(p, static_cast<unsigned>(in_day / 3600));
  *p++ = ':';
  p = write_two(p, static_cast<unsigned>(in_day / 60 % 60));
  *p++ = ':';
  p = write_two(p, static_cast<unsigned>(in_day % 60));
  if (fraction != 0) {
    *p++ = '.';
    for (int k = kFractionDigits[unit] - 1; k >= 0; k--) {
      p[k] = static_cast<char>('0' + fraction % 10);
      fraction /= 10;
    }
    p += kFractionDigits[unit];
  }
  if (utc) *p++ = 'Z';
  return p;
}

}  // namespace internal
}  // namespace simdjson_arrow

#endif  // SIMDJSON_ARROW_TIMESTAMP_H

//
// Copyright (C) 1999-2005 Google, Inc.
//

#include "strutil.h"

#include <ctype.h>
#include <errno.h>
#include <float.h>          // for DBL_DIG and FLT_DIG
#include <math.h>           // for HUGE_VAL
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>           // for FastTimeToBuffer()

#include <algorithm>
using std::min;
using std::max;
using std::swap;
using std::reverse;

#include "hash.h"

#include <iterator>
#include <limits>
using std::numeric_limits;

#include <set>
using std::set;
using std::multiset;

#include <string>
using std::string;

#include <vector>
using std::vector;


#include "base/logging.h"
#include "base/scoped_ptr.h"
#include "split.h"

#ifdef OS_WINDOWS
#ifdef min  // windows.h defines this to something silly
#undef min
#endif
#endif

// ----------------------------------------------------------------------
// FpToString()
// FloatToString()
// IntToString()
//    Convert various types to their string representation.  These
//    all do the obvious, trivial thing.
// ----------------------------------------------------------------------

string FpToString(Fprint fp) {
  char buf[17];
  snprintf(buf, sizeof(buf), "%016llx", fp);
  return string(buf);
}

string FloatToString(float f, const char* format) {
  char buf[80];
  snprintf(buf, sizeof(buf), format, f);
  return string(buf);
}

string IntToString(int i, const char* format) {
  char buf[80];
  snprintf(buf, sizeof(buf), format, i);
  return string(buf);
}

string Int64ToString(int64 i64, const char* format) {
  char buf[80];
  snprintf(buf, sizeof(buf), format, i64);
  return string(buf);
}

string UInt64ToString(uint64 ui64, const char* format) {
  char buf[80];
  snprintf(buf, sizeof(buf), format, ui64);
  return string(buf);
}

// Default arguments
string FloatToString(float f)   { return FloatToString(f, "%7f"); }
string IntToString(int i)       { return IntToString(i, "%7d"); }
string Int64ToString(int64 i64) {
  return Int64ToString(i64, "%7" GG_LL_FORMAT "d");
}
string UInt64ToString(uint64 ui64) {
  return UInt64ToString(ui64, "%7" GG_LL_FORMAT "u");
}

// ----------------------------------------------------------------------
// FastIntToBuffer()
// FastInt64ToBuffer()
// FastHexToBuffer()
// FastHex64ToBuffer()
// FastHex32ToBuffer()
// FastTimeToBuffer()
//    These are intended for speed.  FastHexToBuffer() assumes the
//    integer is non-negative.  FastHexToBuffer() puts output in
//    hex rather than decimal.  FastTimeToBuffer() puts the output
//    into RFC822 format.  If time is 0, uses the current time.
//
//    FastHex64ToBuffer() puts a 64-bit unsigned value in hex-format,
//    padded to exactly 16 bytes (plus one byte for '\0')
//
//    FastHex32ToBuffer() puts a 32-bit unsigned value in hex-format,
//    padded to exactly 8 bytes (plus one byte for '\0')
//
//       All functions take the output buffer as an arg.  FastInt()
//    uses at most 22 bytes, FastTime() uses exactly 30 bytes.
//    They all return a pointer to the beginning of the output,
//    which may not be the beginning of the input buffer.  (Though
//    for FastTimeToBuffer(), we guarantee that it is.)
// ----------------------------------------------------------------------

char *FastInt64ToBuffer(int64 i, char* buffer) {
  FastInt64ToBufferLeft(i, buffer);
  return buffer;
}

// Sigh, also not actually defined here, copied from:
// https://github.com/splitfeed/android-market-api-php/blob/master/proto/protoc-gen-php/strutil.cc

static const char two_ASCII_digits[100][2] = {
  {'0','0'}, {'0','1'}, {'0','2'}, {'0','3'}, {'0','4'},
  {'0','5'}, {'0','6'}, {'0','7'}, {'0','8'}, {'0','9'},
  {'1','0'}, {'1','1'}, {'1','2'}, {'1','3'}, {'1','4'},
  {'1','5'}, {'1','6'}, {'1','7'}, {'1','8'}, {'1','9'},
  {'2','0'}, {'2','1'}, {'2','2'}, {'2','3'}, {'2','4'},
  {'2','5'}, {'2','6'}, {'2','7'}, {'2','8'}, {'2','9'},
  {'3','0'}, {'3','1'}, {'3','2'}, {'3','3'}, {'3','4'},
  {'3','5'}, {'3','6'}, {'3','7'}, {'3','8'}, {'3','9'},
  {'4','0'}, {'4','1'}, {'4','2'}, {'4','3'}, {'4','4'},
  {'4','5'}, {'4','6'}, {'4','7'}, {'4','8'}, {'4','9'},
  {'5','0'}, {'5','1'}, {'5','2'}, {'5','3'}, {'5','4'},
  {'5','5'}, {'5','6'}, {'5','7'}, {'5','8'}, {'5','9'},
  {'6','0'}, {'6','1'}, {'6','2'}, {'6','3'}, {'6','4'},
  {'6','5'}, {'6','6'}, {'6','7'}, {'6','8'}, {'6','9'},
  {'7','0'}, {'7','1'}, {'7','2'}, {'7','3'}, {'7','4'},
  {'7','5'}, {'7','6'}, {'7','7'}, {'7','8'}, {'7','9'},
  {'8','0'}, {'8','1'}, {'8','2'}, {'8','3'}, {'8','4'},
  {'8','5'}, {'8','6'}, {'8','7'}, {'8','8'}, {'8','9'},
  {'9','0'}, {'9','1'}, {'9','2'}, {'9','3'}, {'9','4'},
  {'9','5'}, {'9','6'}, {'9','7'}, {'9','8'}, {'9','9'}
};

char* FastUInt32ToBufferLeft(uint32 u, char* buffer) {
  int digits;
  const char *ASCII_digits = NULL;
  // The idea of this implementation is to trim the number of divides to as few
  // as possible by using multiplication and subtraction rather than mod (%),
  // and by outputting two digits at a time rather than one.
  // The huge-number case is first, in the hopes that the compiler will output
  // that case in one branch-free block of code, and only output conditional
  // branches into it from below.
  if (u >= 1000000000) {  // >= 1,000,000,000
    digits = u / 100000000;  // 100,000,000
    ASCII_digits = two_ASCII_digits[digits];
    buffer[0] = ASCII_digits[0];
    buffer[1] = ASCII_digits[1];
    buffer += 2;
sublt100_000_000:
    u -= digits * 100000000;  // 100,000,000
lt100_000_000:
    digits = u / 1000000;  // 1,000,000
    ASCII_digits = two_ASCII_digits[digits];
    buffer[0] = ASCII_digits[0];
    buffer[1] = ASCII_digits[1];
    buffer += 2;
sublt1_000_000:
    u -= digits * 1000000;  // 1,000,000
lt1_000_000:
    digits = u / 10000;  // 10,000
    ASCII_digits = two_ASCII_digits[digits];
    buffer[0] = ASCII_digits[0];
    buffer[1] = ASCII_digits[1];
    buffer += 2;
sublt10_000:
    u -= digits * 10000;  // 10,000
lt10_000:
    digits = u / 100;
    ASCII_digits = two_ASCII_digits[digits];
    buffer[0] = ASCII_digits[0];
    buffer[1] = ASCII_digits[1];
    buffer += 2;
sublt100:
    u -= digits * 100;
lt100:
    digits = u;
    ASCII_digits = two_ASCII_digits[digits];
    buffer[0] = ASCII_digits[0];
    buffer[1] = ASCII_digits[1];
    buffer += 2;
done:
    *buffer = 0;
    return buffer;
  }

  if (u < 100) {
    digits = u;
    if (u >= 10) goto lt100;
    *buffer++ = '0' + digits;
    goto done;
  }
  if (u  <  10000) {   // 10,000
    if (u >= 1000) goto lt10_000;
    digits = u / 100;
    *buffer++ = '0' + digits;
    goto sublt100;
  }
  if (u  <  1000000) {   // 1,000,000
    if (u >= 100000) goto lt1_000_000;
    digits = u / 10000;  //    10,000
    *buffer++ = '0' + digits;
    goto sublt10_000;
  }
  if (u  <  100000000) {   // 100,000,000
    if (u >= 10000000) goto lt100_000_000;
    digits = u / 1000000;  //   1,000,000
    *buffer++ = '0' + digits;
    goto sublt1_000_000;
  }
  // we already know that u < 1,000,000,000
  digits = u / 100000000;   // 100,000,000
  *buffer++ = '0' + digits;
  goto sublt100_000_000;
}
char* FastUInt64ToBufferLeft(uint64 u64, char* buffer) {
  int digits;
  const char *ASCII_digits = NULL;

  uint32 u = static_cast<uint32>(u64);
  if (u == u64) return FastUInt32ToBufferLeft(u, buffer);

  uint64 top_11_digits = u64 / 1000000000;
  buffer = FastUInt64ToBufferLeft(top_11_digits, buffer);
  u = u64 - (top_11_digits * 1000000000);

  digits = u / 10000000;  // 10,000,000
  DCHECK_LT(digits, 100);
  ASCII_digits = two_ASCII_digits[digits];
  buffer[0] = ASCII_digits[0];
  buffer[1] = ASCII_digits[1];
  buffer += 2;
  u -= digits * 10000000;  // 10,000,000
  digits = u / 100000;  // 100,000
  ASCII_digits = two_ASCII_digits[digits];
  buffer[0] = ASCII_digits[0];
  buffer[1] = ASCII_digits[1];
  buffer += 2;
  u -= digits * 100000;  // 100,000
  digits = u / 1000;  // 1,000
  ASCII_digits = two_ASCII_digits[digits];
  buffer[0] = ASCII_digits[0];
  buffer[1] = ASCII_digits[1];
  buffer += 2;
  u -= digits * 1000;  // 1,000
  digits = u / 10;
  ASCII_digits = two_ASCII_digits[digits];
  buffer[0] = ASCII_digits[0];
  buffer[1] = ASCII_digits[1];
  buffer += 2;
  u -= digits * 10;
  digits = u;
  *buffer++ = '0' + digits;
  *buffer = 0;
  return buffer;
}

char* FastInt64ToBufferLeft(int64 i, char* buffer) {
  uint64 u = i;
  if (i < 0) {
    *buffer++ = '-';
    u = -i;
  }
  return FastUInt64ToBufferLeft(u, buffer);
}

// Offset into buffer where FastInt32ToBuffer places the end of string
// null character.  Also used by FastInt32ToBufferLeft
static const int kFastInt32ToBufferOffset = 11;

// This used to call out to FastInt32ToBufferLeft but that wasn't defined.
// Copied from http://gears.googlecode.com/svn-history/r395/trunk/gears/base/common/string16.cc
char *FastInt32ToBuffer(int32 i, char* buffer) {
  // We could collapse the positive and negative sections, but that
  // would be slightly slower for positive numbers...
  // 12 bytes is enough to store -2**32, -4294967296.
  char* p = buffer + kFastInt32ToBufferOffset;
  *p-- = '\0';
  if (i >= 0) {
    do {
      *p-- = '0' + i % 10;
      i /= 10;
    } while (i > 0);
    return p + 1;
  } else {
    // On different platforms, % and / have different behaviors for
    // negative numbers, so we need to jump through hoops to make sure
    // we don't divide negative numbers.
    if (i > -10) {
      i = -i;
      *p-- = '0' + i;
      *p = '-';
      return p;
    } else {
      // Make sure we aren't at MIN_INT, in which case we can't say i = -i
      i = i + 10;
      i = -i;
      *p-- = '0' + i % 10;
      // Undo what we did a moment ago
      i = i / 10 + 1;
      do {
        *p-- = '0' + i % 10;
        i /= 10;
      } while (i > 0);
      *p = '-';
      return p;
    }
  }
}

char *FastHexToBuffer(int i, char* buffer) {
  CHECK(i >= 0) << "FastHexToBuffer() wants non-negative integers, not " << i;

  static const char *hexdigits = "0123456789abcdef";
  char *p = buffer + 21;
  *p-- = '\0';
  do {
    *p-- = hexdigits[i & 15];   // mod by 16
    i >>= 4;                    // divide by 16
  } while (i > 0);
  return p + 1;
}

char *InternalFastHexToBuffer(uint64 value, char* buffer, int num_byte) {
  static const char *hexdigits = "0123456789abcdef";
  buffer[num_byte] = '\0';
  for (int i = num_byte - 1; i >= 0; i--) {
    buffer[i] = hexdigits[uint32(value) & 0xf];
    value >>= 4;
  }
  return buffer;
}

char *FastHex64ToBuffer(uint64 value, char* buffer) {
  return InternalFastHexToBuffer(value, buffer, 16);
}

char *FastHex32ToBuffer(uint32 value, char* buffer) {
  return InternalFastHexToBuffer(value, buffer, 8);
}

static inline void PutTwoDigits(int i, char* p) {
  DCHECK_GE(i, 0);
  DCHECK_LT(i, 100);
  p[0] = two_ASCII_digits[i][0];
  p[1] = two_ASCII_digits[i][1];
}

#if 0
char* FastTimeToBuffer(time_t s, char* buffer) {
  if (s == 0) {
    time(&s);
  }

  struct tm tm;
  if (gmtime_r(&s, &tm) == NULL) {
    // Error message must fit in 30-char buffer.
    memcpy(buffer, "Invalid:", sizeof("Invalid:"));
    FastInt64ToBufferLeft(s, buffer+strlen(buffer));
    return buffer;
  }

  // strftime format: "%a, %d %b %Y %H:%M:%S GMT",
  // but strftime does locale stuff which we do not want
  // plus strftime takes > 10x the time of hard code

  const char* weekday_name = "Xxx";
  switch (tm.tm_wday) {
    default: { DCHECK(false); } break;
    case 0:  weekday_name = "Sun"; break;
    case 1:  weekday_name = "Mon"; break;
    case 2:  weekday_name = "Tue"; break;
    case 3:  weekday_name = "Wed"; break;
    case 4:  weekday_name = "Thu"; break;
    case 5:  weekday_name = "Fri"; break;
    case 6:  weekday_name = "Sat"; break;
  }

  const char* month_name = "Xxx";
  switch (tm.tm_mon) {
    default:  { DCHECK(false); } break;
    case 0:   month_name = "Jan"; break;
    case 1:   month_name = "Feb"; break;
    case 2:   month_name = "Mar"; break;
    case 3:   month_name = "Apr"; break;
    case 4:   month_name = "May"; break;
    case 5:   month_name = "Jun"; break;
    case 6:   month_name = "Jul"; break;
    case 7:   month_name = "Aug"; break;
    case 8:   month_name = "Sep"; break;
    case 9:   month_name = "Oct"; break;
    case 10:  month_name = "Nov"; break;
    case 11:  month_name = "Dec"; break;
  }

  // Write out the buffer.

  memcpy(buffer+0, weekday_name, 3);
  buffer[3] = ',';
  buffer[4] = ' ';

  PutTwoDigits(tm.tm_mday, buffer+5);
  buffer[7] = ' ';

  memcpy(buffer+8, month_name, 3);
  buffer[11] = ' ';

  int32 year = tm.tm_year + 1900;
  PutTwoDigits(year/100, buffer+12);
  PutTwoDigits(year%100, buffer+14);
  buffer[16] = ' ';

  PutTwoDigits(tm.tm_hour, buffer+17);
  buffer[19] = ':';

  PutTwoDigits(tm.tm_min, buffer+20);
  buffer[22] = ':';

  PutTwoDigits(tm.tm_sec, buffer+23);

  // includes ending NUL
  memcpy(buffer+25, " GMT", 5);

  return buffer;
}
#endif

// ----------------------------------------------------------------------
// ParseLeadingUInt64Value
// ParseLeadingInt64Value
// ParseLeadingHex64Value
//    A simple parser for long long values. Returns the parsed value if a
//    valid integer is found; else returns deflt
//    UInt64 and Int64 cannot handle decimal numbers with leading 0s.
// --------------------------------------------------------------------
uint64 ParseLeadingUInt64Value(const char *str, uint64 deflt) {
  char *error = NULL;
  const uint64 value = strtoull(str, &error, 0);
  return (error == str) ? deflt : value;
}

int64 ParseLeadingInt64Value(const char *str, int64 deflt) {
  char *error = NULL;
  const int64 value = strtoll(str, &error, 0);
  return (error == str) ? deflt : value;
}

uint64 ParseLeadingHex64Value(const char *str, uint64 deflt) {
  char *error = NULL;
  const uint64 value = strtoull(str, &error, 16);
  return (error == str) ? deflt : value;
}

// ----------------------------------------------------------------------
// ParseLeadingDec64Value
// ParseLeadingUDec64Value
//    A simple parser for [u]int64 values. Returns the parsed value
//    if a valid value is found; else returns deflt
//    The string passed in is treated as *10 based*.
//    This can handle strings with leading 0s.
// --------------------------------------------------------------------

int64 ParseLeadingDec64Value(const char *str, int64 deflt) {
  char *error = NULL;
  const int64 value = strtoll(str, &error, 10);
  return (error == str) ? deflt : value;
}

uint64 ParseLeadingUDec64Value(const char *str, uint64 deflt) {
  char *error = NULL;
  const uint64 value = strtoull(str, &error, 10);
  return (error == str) ? deflt : value;
}

bool DictionaryParse(const string& encoded_str,
                      vector<pair<string, string> >* items) {
  vector<string> entries;
  SplitStringUsing(encoded_str, ",", &entries);
  for (size_t i = 0; i < entries.size(); ++i) {
    vector<string> fields;
    SplitStringAllowEmpty(entries[i], ":", &fields);
    if (fields.size() != 2) // parsing error
      return false;
    items->push_back(make_pair(fields[0], fields[1]));
  }
  return true;
}

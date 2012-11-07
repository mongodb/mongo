//
// Copyright 1999-2006 and onwards Google, Inc.
//
// Useful string functions and so forth.  This is a grab-bag file.
//
// You might also want to look at memutil.h, which holds mem*()
// equivalents of a lot of the str*() functions in string.h,
// eg memstr, mempbrk, etc.
//
// If you need to process UTF8 strings, take a look at files in i18n/utf8.

#ifndef STRINGS_STRUTIL_H_
#define STRINGS_STRUTIL_H_

#include "base/definer.h"
#include "hash.h"

#include <functional>
using std::less;
#include <set>
using std::set;
using std::multiset;

#include <string>
using std::string;

#include <utility>
using std::pair;
using std::make_pair;

#include <vector>
using std::vector;

#include <string.h>
#include <stdlib.h>

// for strcasecmp (check SuSv3 -- this is the only header it's in!)
// MSVC doesn't have <strings.h>. Luckily, it defines equivalent
// functions (see port.h)
#ifndef _WIN32
#include <strings.h>
#endif
#include <ctype.h>      // not needed, but removing it will break the build

using namespace std;

// A buffer size which is large enough for all the FastToBuffer functions, as
// well as DoubleToBuffer and FloatToBuffer.  We define this here in case other
// string headers depend on it.
static const int kFastToBufferSize =       32;

#include "base/basictypes.h"
#include "base/logging.h"  // for CHECK
#include "base/strtoint.h"
#include "base/int128.h"
#include "ascii_ctype.h"
//#include "charset.h"
//#include "escaping.h"
//#include "host_port.h"
#include "stringprintf.h"
#include "base/port.h"
#include "util/endian/endian.h"

// ----------------------------------------------------------------------
// FpToString()
// FloatToString()
// IntToString()
// Int64ToString()
// UInt64ToString()
//    Convert various types to their string representation, possibly padded
//    with spaces, using snprintf format specifiers.
//    "Fp" here stands for fingerprint: a 64-bit entity
//    represented in 16 hex digits.
// ----------------------------------------------------------------------

string FpToString(Fprint fp);
string FloatToString(float f, const char* format);
string IntToString(int i, const char* format);
string Int64ToString(int64 i64, const char* format);
string UInt64ToString(uint64 ui64, const char* format);

// The default formats are %7f, %7d, and %7u respectively
string FloatToString(float f);
string IntToString(int i);
string Int64ToString(int64 i64);
string UInt64ToString(uint64 ui64);

// ----------------------------------------------------------------------
// FastIntToBuffer()
// FastHexToBuffer()
// FastHex64ToBuffer()
// FastHex32ToBuffer()
// FastTimeToBuffer()
//    These are intended for speed.  FastIntToBuffer() assumes the
//    integer is non-negative.  FastHexToBuffer() puts output in
//    hex rather than decimal.  FastTimeToBuffer() puts the output
//    into RFC822 format.
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
//
//    NOTE: In 64-bit land, sizeof(time_t) is 8, so it is possible
//    to pass to FastTimeToBuffer() a time whose year cannot be
//    represented in 4 digits. In this case, the output buffer
//    will contain the string "Invalid:<value>"
// ----------------------------------------------------------------------

// Previously documented minimums -- the buffers provided must be at least this
// long, though these numbers are subject to change:
//     Int32, UInt32:        12 bytes
//     Int64, UInt64, Hex:   22 bytes
//     Time:                 30 bytes
//     Hex32:                 9 bytes
//     Hex64:                17 bytes
// Use kFastToBufferSize rather than hardcoding constants.

char* FastInt32ToBuffer(int32 i, char* buffer);
char* FastInt64ToBuffer(int64 i, char* buffer);
char* FastUInt32ToBuffer(uint32 i, char* buffer);
char* FastUInt64ToBuffer(uint64 i, char* buffer);
char* FastHexToBuffer(int i, char* buffer);
char* FastTimeToBuffer(time_t t, char* buffer);
char* FastHex64ToBuffer(uint64 i, char* buffer);
char* FastHex32ToBuffer(uint32 i, char* buffer);

// at least 22 bytes long
inline char* FastIntToBuffer(int i, char* buffer) {
  return (sizeof(i) == 4 ?
          FastInt32ToBuffer(i, buffer) : FastInt64ToBuffer(i, buffer));
}
inline char* FastUIntToBuffer(unsigned int i, char* buffer) {
  return (sizeof(i) == 4 ?
          FastUInt32ToBuffer(i, buffer) : FastUInt64ToBuffer(i, buffer));
}
inline char* FastLongToBuffer(long i, char* buffer) {
  return (sizeof(i) == 4 ?
          FastInt32ToBuffer(i, buffer) : FastInt64ToBuffer(i, buffer));
}
inline char* FastULongToBuffer(unsigned long i, char* buffer) {
  return (sizeof(i) == 4 ?
          FastUInt32ToBuffer(i, buffer) : FastUInt64ToBuffer(i, buffer));
}

// A generic "number type" to buffer template and specializations.
//
// The specialization of FastNumToBuffer<>() should always be made explicit:
//    FastNumToBuffer<int32>(mynums);  // yes
//    FastNumToBuffer(mynums);         // no
template<typename T> char* FastNumToBuffer(T, char*);
template<> inline char* FastNumToBuffer<int32>(int32 i, char* buffer) {
  return FastInt32ToBuffer(i, buffer);
}
template<> inline char* FastNumToBuffer<int64>(int64 i, char* buffer) {
  return FastInt64ToBuffer(i, buffer);
}
template<> inline char* FastNumToBuffer<uint32>(uint32 i, char* buffer) {
  return FastUInt32ToBuffer(i, buffer);
}
template<> inline char* FastNumToBuffer<uint64>(uint64 i, char* buffer) {
  return FastUInt64ToBuffer(i, buffer);
}

// ----------------------------------------------------------------------
// FastInt32ToBufferLeft()
// FastUInt32ToBufferLeft()
// FastInt64ToBufferLeft()
// FastUInt64ToBufferLeft()
//
// Like the Fast*ToBuffer() functions above, these are intended for speed.
// Unlike the Fast*ToBuffer() functions, however, these functions write
// their output to the beginning of the buffer (hence the name, as the
// output is left-aligned).  The caller is responsible for ensuring that
// the buffer has enough space to hold the output.
//
// Returns a pointer to the end of the string (i.e. the null character
// terminating the string).
// ----------------------------------------------------------------------

char* FastInt32ToBufferLeft(int32 i, char* buffer);    // at least 12 bytes
char* FastUInt32ToBufferLeft(uint32 i, char* buffer);    // at least 12 bytes
char* FastInt64ToBufferLeft(int64 i, char* buffer);    // at least 22 bytes
char* FastUInt64ToBufferLeft(uint64 i, char* buffer);    // at least 22 bytes

// Just define these in terms of the above.
inline char* FastUInt32ToBuffer(uint32 i, char* buffer) {
  FastUInt32ToBufferLeft(i, buffer);
  return buffer;
}
inline char* FastUInt64ToBuffer(uint64 i, char* buffer) {
  FastUInt64ToBufferLeft(i, buffer);
  return buffer;
}

// ----------------------------------------------------------------------
// ConsumeStrayLeadingZeroes
//    Eliminates all leading zeroes (unless the string itself is composed
//    of nothing but zeroes, in which case one is kept: 0...0 becomes 0).
void ConsumeStrayLeadingZeroes(string* str);

// ----------------------------------------------------------------------
// ParseLeadingInt32Value
//    A simple parser for int32 values. Returns the parsed value
//    if a valid integer is found; else returns deflt. It does not
//    check if str is entirely consumed.
//    This cannot handle decimal numbers with leading 0s, since they will be
//    treated as octal.  If you know it's decimal, use ParseLeadingDec32Value.
// --------------------------------------------------------------------
int32 ParseLeadingInt32Value(const char* str, int32 deflt);
inline int32 ParseLeadingInt32Value(const string& str, int32 deflt) {
  return ParseLeadingInt32Value(str.c_str(), deflt);
}

// ParseLeadingUInt32Value
//    A simple parser for uint32 values. Returns the parsed value
//    if a valid integer is found; else returns deflt. It does not
//    check if str is entirely consumed.
//    This cannot handle decimal numbers with leading 0s, since they will be
//    treated as octal.  If you know it's decimal, use ParseLeadingUDec32Value.
// --------------------------------------------------------------------
uint32 ParseLeadingUInt32Value(const char* str, uint32 deflt);
inline uint32 ParseLeadingUInt32Value(const string& str, uint32 deflt) {
  return ParseLeadingUInt32Value(str.c_str(), deflt);
}

// ----------------------------------------------------------------------
// ParseLeadingDec32Value
//    A simple parser for decimal int32 values. Returns the parsed value
//    if a valid integer is found; else returns deflt. It does not
//    check if str is entirely consumed.
//    The string passed in is treated as *10 based*.
//    This can handle strings with leading 0s.
//    See also: ParseLeadingDec64Value
// --------------------------------------------------------------------
int32 ParseLeadingDec32Value(const char* str, int32 deflt);
inline int32 ParseLeadingDec32Value(const string& str, int32 deflt) {
  return ParseLeadingDec32Value(str.c_str(), deflt);
}

// ParseLeadingUDec32Value
//    A simple parser for decimal uint32 values. Returns the parsed value
//    if a valid integer is found; else returns deflt. It does not
//    check if str is entirely consumed.
//    The string passed in is treated as *10 based*.
//    This can handle strings with leading 0s.
//    See also: ParseLeadingUDec64Value
// --------------------------------------------------------------------
uint32 ParseLeadingUDec32Value(const char* str, uint32 deflt);
inline uint32 ParseLeadingUDec32Value(const string& str, uint32 deflt) {
  return ParseLeadingUDec32Value(str.c_str(), deflt);
}

// ----------------------------------------------------------------------
// ParseLeadingUInt64Value
// ParseLeadingInt64Value
// ParseLeadingHex64Value
// ParseLeadingDec64Value
// ParseLeadingUDec64Value
//    A simple parser for long long values.
//    Returns the parsed value if a
//    valid integer is found; else returns deflt
// --------------------------------------------------------------------
uint64 ParseLeadingUInt64Value(const char* str, uint64 deflt);
inline uint64 ParseLeadingUInt64Value(const string& str, uint64 deflt) {
  return ParseLeadingUInt64Value(str.c_str(), deflt);
}
int64 ParseLeadingInt64Value(const char* str, int64 deflt);
inline int64 ParseLeadingInt64Value(const string& str, int64 deflt) {
  return ParseLeadingInt64Value(str.c_str(), deflt);
}
uint64 ParseLeadingHex64Value(const char* str, uint64 deflt);
inline uint64 ParseLeadingHex64Value(const string& str, uint64 deflt) {
  return ParseLeadingHex64Value(str.c_str(), deflt);
}
int64 ParseLeadingDec64Value(const char* str, int64 deflt);
inline int64 ParseLeadingDec64Value(const string& str, int64 deflt) {
  return ParseLeadingDec64Value(str.c_str(), deflt);
}
uint64 ParseLeadingUDec64Value(const char* str, uint64 deflt);
inline uint64 ParseLeadingUDec64Value(const string& str, uint64 deflt) {
  return ParseLeadingUDec64Value(str.c_str(), deflt);
}

// -------------------------------------------------------------------------
// DictionaryParse
//   This routine parses a common dictionary format (key and value separated
//   by ':', entries separated by commas). This format is used for many
//   complex commandline flags. It is also used to encode dictionaries for
//   exporting them or writing them to a checkpoint. Returns a vector of
//   <key, value> pairs. Returns true if there if no error in parsing, false
//    otherwise.
// -------------------------------------------------------------------------
bool DictionaryParse(const string& encoded_str,
                      vector<pair<string, string> >* items);

#endif   /* #ifndef STRINGS_STRUTIL_H_ */

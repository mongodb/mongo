// Copyright 2008 Google Inc. All Rights Reserved.
//
// Architecture-neutral plug compatible replacements for strtol() friends.
//
// Long's have different lengths on ILP-32 and LP-64 platforms, and so overflow
// behavior across the two varies when strtol() and similar are used to parse
// 32-bit integers.  Similar problems exist with atoi(), because although it
// has an all-integer interface, it uses strtol() internally, and so suffers
// from the same narrowing problems on assignments to int.
//
// Examples:
//   errno = 0;
//   i = strtol("3147483647", NULL, 10);
//   printf("%d, errno %d\n", i, errno);
//   //   32-bit platform: 2147483647, errno 34
//   //   64-bit platform: -1147483649, errno 0
//
//   printf("%d\n", atoi("3147483647"));
//   //   32-bit platform: 2147483647
//   //   64-bit platform: -1147483649
//
// A way round this is to define local replacements for these, and use them
// instead of the standard libc functions.
//
// In most 32-bit cases the replacements can be inlined away to a call to the
// libc function.  In a couple of 64-bit cases, however, adapters are required,
// to provide the right overflow and errno behavior.
//

#ifndef BASE_STRTOINT_H_
#define BASE_STRTOINT_H_

#include <stdlib.h> // For strtol* functions.
#include <string>
using std::string;

#include "base/port.h"
#include "base/basictypes.h"

// Adapter functions for handling overflow and errno.
int32 strto32_adapter(const char *nptr, char **endptr, int base);
uint32 strtou32_adapter(const char *nptr, char **endptr, int base);

// Conversions to a 32-bit integer can pass the call to strto[u]l on 32-bit
// platforms, but need a little extra work on 64-bit platforms.
inline int32 strto32(const char *nptr, char **endptr, int base) {
  if (sizeof(int32) == sizeof(long))
    return strtol(nptr, endptr, base);
  else
    return strto32_adapter(nptr, endptr, base);
}

inline uint32 strtou32(const char *nptr, char **endptr, int base) {
  if (sizeof(uint32) == sizeof(unsigned long))
    return strtoul(nptr, endptr, base);
  else
    return strtou32_adapter(nptr, endptr, base);
}

// For now, long long is 64-bit on all the platforms we care about, so these
// functions can simply pass the call to strto[u]ll.
inline int64 strto64(const char *nptr, char **endptr, int base) {
  COMPILE_ASSERT(sizeof(int64) == sizeof(long long),
                 sizeof_int64_is_not_sizeof_long_long);
  return strtoll(nptr, endptr, base);
}

inline uint64 strtou64(const char *nptr, char **endptr, int base) {
  COMPILE_ASSERT(sizeof(uint64) == sizeof(unsigned long long),
                 sizeof_uint64_is_not_sizeof_long_long);
  return strtoull(nptr, endptr, base);
}

// Although it returns an int, atoi() is implemented in terms of strtol, and
// so has differing overflow and underflow behavior.  atol is the same.
inline int32 atoi32(const char *nptr) {
  return strto32(nptr, NULL, 10);
}

inline int64 atoi64(const char *nptr) {
  return strto64(nptr, NULL, 10);
}

// Convenience versions of the above that take a string argument.
inline int32 atoi32(const string &s) {
  return atoi32(s.c_str());
}

inline int64 atoi64(const string &s) {
  return atoi64(s.c_str());
}

#endif  // BASE_STRTOINT_H_

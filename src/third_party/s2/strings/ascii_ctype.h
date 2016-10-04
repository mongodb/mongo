// Copyright 2007 Google Inc. All Rights Reserved.

#ifndef STRINGS_ASCII_CTYPE_H__
#define STRINGS_ASCII_CTYPE_H__

#include "base/basictypes.h"

// ----------------------------------------------------------------------
// ascii_isalpha()
// ascii_isdigit()
// ascii_isalnum()
// ascii_isspace()
// ascii_ispunct()
// ascii_isblank()
// ascii_iscntrl()
// ascii_isxdigit()
// ascii_isprint()
// ascii_isgraph()
// ascii_isupper()
// ascii_islower()
// ascii_tolower()
// ascii_toupper()
//     The ctype.h versions of these routines are slow with some
//     compilers and/or architectures, perhaps because of locale
//     issues.  These versions work for ascii only: they return
//     false for everything above \x7f (which means they return
//     false for any byte from any non-ascii UTF8 character).
//
// The individual bits do not have names because the array definition
// is already tightly coupled to this, and names would make it harder
// to read and debug.
//
// This is an example of the benchmark times from the unittest:
// $ ascii_ctype_test --benchmarks=all --heap_check=
// Benchmark           Time(ns)    CPU(ns) Iterations
// --------------------------------------------------
// BM_Identity              121        120    5785985 2027.0 MB/s
// BM_isalpha              1603       1597     511027  152.9 MB/s
// BM_ascii_isalpha         223        224    3111595 1088.5 MB/s
// BM_isdigit               181        183    3825722 1336.4 MB/s
// BM_ascii_isdigit         236        239    2929312 1023.3 MB/s
// BM_isalnum              1623       1615     460596  151.2 MB/s
// BM_ascii_isalnum         253        255    2745518  959.1 MB/s
// BM_isspace              1264       1258     555639  194.1 MB/s
// BM_ascii_isspace         253        255    2745507  959.1 MB/s
// BM_ispunct              1324       1317     555639  185.3 MB/s
// BM_ascii_ispunct         252        255    2745507  959.1 MB/s
// BM_isblank              1433       1426     511027  171.2 MB/s
// BM_ascii_isblank         253        254    2745518  960.5 MB/s
// BM_iscntrl              1643       1634     530383  149.4 MB/s
// BM_ascii_iscntrl         252        255    2745518  959.1 MB/s
// BM_isxdigit             1826       1817     414265  134.3 MB/s
// BM_ascii_isxdigit        258        260    2692712  939.3 MB/s
// BM_isprint              1677       1669     419224  146.2 MB/s
// BM_ascii_isprint         237        239    2929312 1021.8 MB/s
// BM_isgraph              1436       1429     507324  170.9 MB/s
// BM_ascii_isgraph         237        239    2929312 1021.8 MB/s
// BM_isupper              1550       1544     463647  158.1 MB/s
// BM_ascii_isupper         237        239    2929312 1021.8 MB/s
// BM_islower              1301       1294     538544  188.7 MB/s
// BM_ascii_islower         237        239    2929312 1023.3 MB/s
// BM_isascii               182        181    3846746 1345.7 MB/s
// BM_ascii_isascii         209        211    3318039 1159.1 MB/s
// BM_tolower              1743       1764     397786  138.4 MB/s
// BM_ascii_tolower         210        211    3318039 1155.8 MB/s
// BM_toupper              1742       1764     397788  138.4 MB/s
// BM_ascii_toupper         212        211    3302401 1156.9 MB/s
//
// ----------------------------------------------------------------------

#define kApb kAsciiPropertyBits
extern const uint8 kAsciiPropertyBits[256];
static inline bool ascii_isalpha(unsigned char c) { return kApb[c] & 0x01; }
static inline bool ascii_isalnum(unsigned char c) { return kApb[c] & 0x04; }
static inline bool ascii_isspace(unsigned char c) { return kApb[c] & 0x08; }
static inline bool ascii_ispunct(unsigned char c) { return kApb[c] & 0x10; }
static inline bool ascii_isblank(unsigned char c) { return kApb[c] & 0x20; }
static inline bool ascii_iscntrl(unsigned char c) { return kApb[c] & 0x40; }
static inline bool ascii_isxdigit(unsigned char c) { return kApb[c] & 0x80; }
static inline bool ascii_isdigit(unsigned char c) { return c >= '0' && c <= '9'; }
static inline bool ascii_isprint(unsigned char c) { return c >= 32 && c < 127; }
static inline bool ascii_isgraph(unsigned char c) { return c >  32 && c < 127; }
static inline bool ascii_isupper(unsigned char c) { return c >= 'A' && c <= 'Z'; }
static inline bool ascii_islower(unsigned char c) { return c >= 'a' && c <= 'z'; }
static inline bool ascii_isascii(unsigned char c) {
  return static_cast<signed char>(c) >= 0;
}
#undef kApb

extern const char kAsciiToLower[256];
static inline char ascii_tolower(unsigned char c) { return kAsciiToLower[c]; }
extern const char kAsciiToUpper[256];
static inline char ascii_toupper(unsigned char c) { return kAsciiToUpper[c]; }

#endif  // STRINGS_ASCII_CTYPE_H__

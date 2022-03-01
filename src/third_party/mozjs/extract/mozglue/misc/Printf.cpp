/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Portable safe sprintf code.
 *
 * Author: Kipp E.B. Hickman
 */

#include "double-conversion/double-conversion.h"
#include "mozilla/AllocPolicy.h"
#include "mozilla/Likely.h"
#include "mozilla/Printf.h"
#include "mozilla/Sprintf.h"
#include "mozilla/UniquePtrExtensions.h"
#include "mozilla/Vector.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(XP_WIN)
#  include <windows.h>
#endif

using double_conversion::DoubleToStringConverter;
using DTSC = DoubleToStringConverter;

/*
 * Note: on some platforms va_list is defined as an array,
 * and requires array notation.
 */
#ifdef HAVE_VA_COPY
#  define VARARGS_ASSIGN(foo, bar) VA_COPY(foo, bar)
#elif defined(HAVE_VA_LIST_AS_ARRAY)
#  define VARARGS_ASSIGN(foo, bar) foo[0] = bar[0]
#else
#  define VARARGS_ASSIGN(foo, bar) (foo) = (bar)
#endif

/*
 * Numbered Argument State
 */
struct NumArgState {
  int type;    // type of the current ap
  va_list ap;  // point to the corresponding position on ap
};

using NumArgStateVector =
    mozilla::Vector<NumArgState, 20, mozilla::MallocAllocPolicy>;

// For values up to and including TYPE_DOUBLE, the lowest bit indicates
// whether the type is signed (0) or unsigned (1).
#define TYPE_SHORT 0
#define TYPE_USHORT 1
#define TYPE_INTN 2
#define TYPE_UINTN 3
#define TYPE_LONG 4
#define TYPE_ULONG 5
#define TYPE_LONGLONG 6
#define TYPE_ULONGLONG 7
#define TYPE_DOUBLE 8
#define TYPE_STRING 9
#define TYPE_INTSTR 10
#define TYPE_POINTER 11
#if defined(XP_WIN)
#  define TYPE_WSTRING 12
#endif
#define TYPE_SCHAR 14
#define TYPE_UCHAR 15
#define TYPE_UNKNOWN 20

#define FLAG_LEFT 0x1
#define FLAG_SIGNED 0x2
#define FLAG_SPACED 0x4
#define FLAG_ZEROS 0x8
#define FLAG_NEG 0x10

static const char hex[] = "0123456789abcdef";
static const char HEX[] = "0123456789ABCDEF";

// Fill into the buffer using the data in src
bool mozilla::PrintfTarget::fill2(const char* src, int srclen, int width,
                                  int flags) {
  char space = ' ';

  width -= srclen;
  if (width > 0 && (flags & FLAG_LEFT) == 0) {  // Right adjusting
    if (flags & FLAG_ZEROS) {
      space = '0';
    }
    while (--width >= 0) {
      if (!emit(&space, 1)) {
        return false;
      }
    }
  }

  // Copy out the source data
  if (!emit(src, srclen)) {
    return false;
  }

  if (width > 0 && (flags & FLAG_LEFT) != 0) {  // Left adjusting
    while (--width >= 0) {
      if (!emit(&space, 1)) {
        return false;
      }
    }
  }
  return true;
}

/*
 * Fill a number. The order is: optional-sign zero-filling conversion-digits
 */
bool mozilla::PrintfTarget::fill_n(const char* src, int srclen, int width,
                                   int prec, int type, int flags) {
  int zerowidth = 0;
  int precwidth = 0;
  int signwidth = 0;
  int leftspaces = 0;
  int rightspaces = 0;
  int cvtwidth;
  char sign;

  if ((type & 1) == 0) {
    if (flags & FLAG_NEG) {
      sign = '-';
      signwidth = 1;
    } else if (flags & FLAG_SIGNED) {
      sign = '+';
      signwidth = 1;
    } else if (flags & FLAG_SPACED) {
      sign = ' ';
      signwidth = 1;
    }
  }
  cvtwidth = signwidth + srclen;

  if (prec > 0 && (type != TYPE_DOUBLE)) {
    if (prec > srclen) {
      precwidth = prec - srclen;  // Need zero filling
      cvtwidth += precwidth;
    }
  }

  if ((flags & FLAG_ZEROS) && ((type == TYPE_DOUBLE) || (prec < 0))) {
    if (width > cvtwidth) {
      zerowidth = width - cvtwidth;  // Zero filling
      cvtwidth += zerowidth;
    }
  }

  if (flags & FLAG_LEFT) {
    if (width > cvtwidth) {
      // Space filling on the right (i.e. left adjusting)
      rightspaces = width - cvtwidth;
    }
  } else {
    if (width > cvtwidth) {
      // Space filling on the left (i.e. right adjusting)
      leftspaces = width - cvtwidth;
    }
  }
  while (--leftspaces >= 0) {
    if (!emit(" ", 1)) {
      return false;
    }
  }
  if (signwidth) {
    if (!emit(&sign, 1)) {
      return false;
    }
  }
  while (--precwidth >= 0) {
    if (!emit("0", 1)) {
      return false;
    }
  }
  while (--zerowidth >= 0) {
    if (!emit("0", 1)) {
      return false;
    }
  }
  if (!emit(src, uint32_t(srclen))) {
    return false;
  }
  while (--rightspaces >= 0) {
    if (!emit(" ", 1)) {
      return false;
    }
  }
  return true;
}

// All that the cvt_* functions care about as far as the TYPE_* constants is
// that the low bit is set to indicate unsigned, or unset to indicate signed.
// So we don't try to hard to ensure that the passed TYPE_* constant lines
// up with the actual size of the number being printed here.  The main printf
// code, below, does have to care so that the correct bits are extracted from
// the varargs list.
bool mozilla::PrintfTarget::appendIntDec(int32_t num) {
  int flags = 0;
  long n = num;
  if (n < 0) {
    n = -n;
    flags |= FLAG_NEG;
  }
  return cvt_l(n, -1, -1, 10, TYPE_INTN, flags, hex);
}

bool mozilla::PrintfTarget::appendIntDec(uint32_t num) {
  return cvt_l(num, -1, -1, 10, TYPE_UINTN, 0, hex);
}

bool mozilla::PrintfTarget::appendIntOct(uint32_t num) {
  return cvt_l(num, -1, -1, 8, TYPE_UINTN, 0, hex);
}

bool mozilla::PrintfTarget::appendIntHex(uint32_t num) {
  return cvt_l(num, -1, -1, 16, TYPE_UINTN, 0, hex);
}

bool mozilla::PrintfTarget::appendIntDec(int64_t num) {
  int flags = 0;
  if (num < 0) {
    num = -num;
    flags |= FLAG_NEG;
  }
  return cvt_ll(num, -1, -1, 10, TYPE_INTN, flags, hex);
}

bool mozilla::PrintfTarget::appendIntDec(uint64_t num) {
  return cvt_ll(num, -1, -1, 10, TYPE_UINTN, 0, hex);
}

bool mozilla::PrintfTarget::appendIntOct(uint64_t num) {
  return cvt_ll(num, -1, -1, 8, TYPE_UINTN, 0, hex);
}

bool mozilla::PrintfTarget::appendIntHex(uint64_t num) {
  return cvt_ll(num, -1, -1, 16, TYPE_UINTN, 0, hex);
}

/* Convert a long into its printable form. */
bool mozilla::PrintfTarget::cvt_l(long num, int width, int prec, int radix,
                                  int type, int flags, const char* hexp) {
  char cvtbuf[100];
  char* cvt;
  int digits;

  // according to the man page this needs to happen
  if ((prec == 0) && (num == 0)) {
    return fill_n("", 0, width, prec, type, flags);
  }

  // Converting decimal is a little tricky. In the unsigned case we
  // need to stop when we hit 10 digits. In the signed case, we can
  // stop when the number is zero.
  cvt = cvtbuf + sizeof(cvtbuf);
  digits = 0;
  while (num) {
    int digit = (((unsigned long)num) % radix) & 0xF;
    *--cvt = hexp[digit];
    digits++;
    num = (long)(((unsigned long)num) / radix);
  }
  if (digits == 0) {
    *--cvt = '0';
    digits++;
  }

  // Now that we have the number converted without its sign, deal with
  // the sign and zero padding.
  return fill_n(cvt, digits, width, prec, type, flags);
}

/* Convert a 64-bit integer into its printable form. */
bool mozilla::PrintfTarget::cvt_ll(int64_t num, int width, int prec, int radix,
                                   int type, int flags, const char* hexp) {
  // According to the man page, this needs to happen.
  if (prec == 0 && num == 0) {
    return fill_n("", 0, width, prec, type, flags);
  }

  // Converting decimal is a little tricky. In the unsigned case we
  // need to stop when we hit 10 digits. In the signed case, we can
  // stop when the number is zero.
  int64_t rad = int64_t(radix);
  char cvtbuf[100];
  char* cvt = cvtbuf + sizeof(cvtbuf);
  int digits = 0;
  while (num != 0) {
    int64_t quot = uint64_t(num) / rad;
    int64_t rem = uint64_t(num) % rad;
    int32_t digit = int32_t(rem);
    *--cvt = hexp[digit & 0xf];
    digits++;
    num = quot;
  }
  if (digits == 0) {
    *--cvt = '0';
    digits++;
  }

  // Now that we have the number converted without its sign, deal with
  // the sign and zero padding.
  return fill_n(cvt, digits, width, prec, type, flags);
}

template <size_t N>
constexpr static size_t lengthof(const char (&)[N]) {
  return N - 1;
}

// Longest possible output from ToFixed for positive numbers:
// [0-9]{kMaxFixedDigitsBeforePoint}\.[0-9]{kMaxFixedDigitsAfterPoint}?
constexpr int FIXED_MAX_CHARS =
    DTSC::kMaxFixedDigitsBeforePoint + 1 + DTSC::kMaxFixedDigitsAfterPoint;

// Longest possible output from ToExponential:
// [1-9]\.[0-9]{kMaxExponentialDigits}e[+-][0-9]{1,3}
// (std::numeric_limits<double>::max() has exponent 308).
constexpr int EXPONENTIAL_MAX_CHARS =
    lengthof("1.") + DTSC::kMaxExponentialDigits + lengthof("e+999");

// Longest possible output from ToPrecise:
// [0-9\.]{kMaxPrecisionDigits+1} or
// [1-9]\.[0-9]{kMaxPrecisionDigits-1}e[+-][0-9]{1,3}
constexpr int PRECISE_MAX_CHARS =
    lengthof("1.") + DTSC::kMaxPrecisionDigits - 1 + lengthof("e+999");

constexpr int DTSC_MAX_CHARS =
    std::max({FIXED_MAX_CHARS, EXPONENTIAL_MAX_CHARS, PRECISE_MAX_CHARS});

/*
 * Convert a double precision floating point number into its printable
 * form.
 */
bool mozilla::PrintfTarget::cvt_f(double d, char c, int width, int prec,
                                  int flags) {
  bool lower = islower(c);
  const char* inf = lower ? "inf" : "INF";
  const char* nan = lower ? "nan" : "NAN";
  char e = lower ? 'e' : 'E';
  DoubleToStringConverter converter(DTSC::UNIQUE_ZERO | DTSC::NO_TRAILING_ZERO |
                                        DTSC::EMIT_POSITIVE_EXPONENT_SIGN,
                                    inf, nan, e, 0, 0, 4, 0, 2);
  // Longest of the above cases, plus space for a terminal nul character.
  char buf[DTSC_MAX_CHARS + 1];
  double_conversion::StringBuilder builder(buf, sizeof(buf));
  bool success = false;
  if (std::signbit(d)) {
    d = std::abs(d);
    flags |= FLAG_NEG;
  }
  if (!std::isfinite(d)) {
    flags &= ~FLAG_ZEROS;
  }
  // "If the precision is missing, it shall be taken as 6."
  if (prec < 0) {
    prec = 6;
  }
  switch (c) {
    case 'e':
    case 'E':
      success = converter.ToExponential(d, prec, &builder);
      break;
    case 'f':
    case 'F':
      success = converter.ToFixed(d, prec, &builder);
      break;
    case 'g':
    case 'G':
      // "If an explicit precision is zero, it shall be taken as 1."
      success = converter.ToPrecision(d, prec ? prec : 1, &builder);
      break;
  }
  if (!success) {
    return false;
  }
  int len = builder.position();
  char* cvt = builder.Finalize();
  return fill_n(cvt, len, width, prec, TYPE_DOUBLE, flags);
}

/*
 * Convert a string into its printable form.  "width" is the output
 * width. "prec" is the maximum number of characters of "s" to output,
 * where -1 means until NUL.
 */
bool mozilla::PrintfTarget::cvt_s(const char* s, int width, int prec,
                                  int flags) {
  if (prec == 0) {
    return true;
  }
  if (!s) {
    s = "(null)";
  }

  // Limit string length by precision value
  int slen = int(strlen(s));
  if (0 < prec && prec < slen) {
    slen = prec;
  }

  // and away we go
  return fill2(s, slen, width, flags);
}

/*
 * BuildArgArray stands for Numbered Argument list Sprintf
 * for example,
 *      fmp = "%4$i, %2$d, %3s, %1d";
 * the number must start from 1, and no gap among them
 */
static bool BuildArgArray(const char* fmt, va_list ap, NumArgStateVector& nas) {
  size_t number = 0, cn = 0, i;
  const char* p;
  char c;

  // First pass:
  // Detemine how many legal % I have got, then allocate space.

  p = fmt;
  i = 0;
  while ((c = *p++) != 0) {
    if (c != '%') {
      continue;
    }
    if ((c = *p++) == '%') {  // skip %% case
      continue;
    }

    while (c != 0) {
      if (c > '9' || c < '0') {
        if (c == '$') {  // numbered argument case
          if (i > 0) {
            MOZ_CRASH("Bad format string");
          }
          number++;
        } else {  // non-numbered argument case
          if (number > 0) {
            MOZ_CRASH("Bad format string");
          }
          i = 1;
        }
        break;
      }

      c = *p++;
    }
  }

  if (number == 0) {
    return true;
  }

  // Only allow a limited number of arguments.
  MOZ_RELEASE_ASSERT(number <= 20);

  if (!nas.growByUninitialized(number)) {
    return false;
  }

  for (i = 0; i < number; i++) {
    nas[i].type = TYPE_UNKNOWN;
  }

  // Second pass:
  // Set nas[].type.

  p = fmt;
  while ((c = *p++) != 0) {
    if (c != '%') {
      continue;
    }
    c = *p++;
    if (c == '%') {
      continue;
    }

    cn = 0;
    while (c && c != '$') {  // should improve error check later
      cn = cn * 10 + c - '0';
      c = *p++;
    }

    if (!c || cn < 1 || cn > number) {
      MOZ_CRASH("Bad format string");
    }

    // nas[cn] starts from 0, and make sure nas[cn].type is not assigned.
    cn--;
    if (nas[cn].type != TYPE_UNKNOWN) {
      continue;
    }

    c = *p++;

    // flags
    while ((c == '-') || (c == '+') || (c == ' ') || (c == '0')) {
      c = *p++;
    }

    // width
    if (c == '*') {
      // not supported feature, for the argument is not numbered
      MOZ_CRASH("Bad format string");
    }

    while ((c >= '0') && (c <= '9')) {
      c = *p++;
    }

    // precision
    if (c == '.') {
      c = *p++;
      if (c == '*') {
        // not supported feature, for the argument is not numbered
        MOZ_CRASH("Bad format string");
      }

      while ((c >= '0') && (c <= '9')) {
        c = *p++;
      }
    }

    // size
    nas[cn].type = TYPE_INTN;
    if (c == 'h') {
      nas[cn].type = TYPE_SHORT;
      c = *p++;
      if (c == 'h') {
        nas[cn].type = TYPE_SCHAR;
        c = *p++;
      }
    } else if (c == 'L') {
      nas[cn].type = TYPE_LONGLONG;
      c = *p++;
    } else if (c == 'l') {
      nas[cn].type = TYPE_LONG;
      c = *p++;
      if (c == 'l') {
        nas[cn].type = TYPE_LONGLONG;
        c = *p++;
      }
    } else if (c == 'z' || c == 'I') {
      static_assert(sizeof(size_t) == sizeof(int) ||
                        sizeof(size_t) == sizeof(long) ||
                        sizeof(size_t) == sizeof(long long),
                    "size_t is not one of the expected sizes");
      nas[cn].type = sizeof(size_t) == sizeof(int)    ? TYPE_INTN
                     : sizeof(size_t) == sizeof(long) ? TYPE_LONG
                                                      : TYPE_LONGLONG;
      c = *p++;
    } else if (c == 't') {
      static_assert(sizeof(ptrdiff_t) == sizeof(int) ||
                        sizeof(ptrdiff_t) == sizeof(long) ||
                        sizeof(ptrdiff_t) == sizeof(long long),
                    "ptrdiff_t is not one of the expected sizes");
      nas[cn].type = sizeof(ptrdiff_t) == sizeof(int)    ? TYPE_INTN
                     : sizeof(ptrdiff_t) == sizeof(long) ? TYPE_LONG
                                                         : TYPE_LONGLONG;
      c = *p++;
    } else if (c == 'j') {
      static_assert(sizeof(intmax_t) == sizeof(int) ||
                        sizeof(intmax_t) == sizeof(long) ||
                        sizeof(intmax_t) == sizeof(long long),
                    "intmax_t is not one of the expected sizes");
      nas[cn].type = sizeof(intmax_t) == sizeof(int)    ? TYPE_INTN
                     : sizeof(intmax_t) == sizeof(long) ? TYPE_LONG
                                                        : TYPE_LONGLONG;
      c = *p++;
    }

    // format
    switch (c) {
      case 'd':
      case 'c':
      case 'i':
        break;

      case 'o':
      case 'u':
      case 'x':
      case 'X':
        // Mark as unsigned type.
        nas[cn].type |= 1;
        break;

      case 'e':
      case 'E':
      case 'f':
      case 'F':
      case 'g':
      case 'G':
        nas[cn].type = TYPE_DOUBLE;
        break;

      case 'p':
        nas[cn].type = TYPE_POINTER;
        break;

      case 'S':
#if defined(XP_WIN)
        nas[cn].type = TYPE_WSTRING;
#else
        MOZ_ASSERT(0);
        nas[cn].type = TYPE_UNKNOWN;
#endif
        break;

      case 's':
#if defined(XP_WIN)
        if (nas[cn].type == TYPE_LONG) {
          nas[cn].type = TYPE_WSTRING;
          break;
        }
#endif
        // Other type sizes are not supported here.
        MOZ_ASSERT(nas[cn].type == TYPE_INTN);
        nas[cn].type = TYPE_STRING;
        break;

      case 'n':
        nas[cn].type = TYPE_INTSTR;
        break;

      default:
        MOZ_ASSERT(0);
        nas[cn].type = TYPE_UNKNOWN;
        break;
    }

    // get a legal para.
    if (nas[cn].type == TYPE_UNKNOWN) {
      MOZ_CRASH("Bad format string");
    }
  }

  // Third pass:
  // Fill nas[].ap.

  cn = 0;
  while (cn < number) {
    // A TYPE_UNKNOWN here means that the format asked for a
    // positional argument without specifying the meaning of some
    // earlier argument.
    MOZ_ASSERT(nas[cn].type != TYPE_UNKNOWN);

    VARARGS_ASSIGN(nas[cn].ap, ap);

    switch (nas[cn].type) {
      case TYPE_SCHAR:
      case TYPE_UCHAR:
      case TYPE_SHORT:
      case TYPE_USHORT:
      case TYPE_INTN:
      case TYPE_UINTN:
        (void)va_arg(ap, int);
        break;
      case TYPE_LONG:
        (void)va_arg(ap, long);
        break;
      case TYPE_ULONG:
        (void)va_arg(ap, unsigned long);
        break;
      case TYPE_LONGLONG:
        (void)va_arg(ap, long long);
        break;
      case TYPE_ULONGLONG:
        (void)va_arg(ap, unsigned long long);
        break;
      case TYPE_STRING:
        (void)va_arg(ap, char*);
        break;
      case TYPE_INTSTR:
        (void)va_arg(ap, int*);
        break;
      case TYPE_DOUBLE:
        (void)va_arg(ap, double);
        break;
      case TYPE_POINTER:
        (void)va_arg(ap, void*);
        break;
#if defined(XP_WIN)
      case TYPE_WSTRING:
        (void)va_arg(ap, wchar_t*);
        break;
#endif

      default:
        MOZ_CRASH();
    }

    cn++;
  }

  return true;
}

mozilla::PrintfTarget::PrintfTarget() : mEmitted(0) {}

bool mozilla::PrintfTarget::vprint(const char* fmt, va_list ap) {
  char c;
  int flags, width, prec, radix, type;
  union {
    char ch;
    int i;
    long l;
    long long ll;
    double d;
    const char* s;
    int* ip;
    void* p;
#if defined(XP_WIN)
    const wchar_t* ws;
#endif
  } u{};
  const char* hexp;
  int i;

  // Build an argument array, IF the fmt is numbered argument
  // list style, to contain the Numbered Argument list pointers.

  NumArgStateVector nas;
  if (!BuildArgArray(fmt, ap, nas)) {
    // the fmt contains error Numbered Argument format, jliu@netscape.com
    MOZ_CRASH("Bad format string");
  }

  while ((c = *fmt++) != 0) {
    if (c != '%') {
      if (!emit(fmt - 1, 1)) {
        return false;
      }

      continue;
    }

    // Gobble up the % format string. Hopefully we have handled all
    // of the strange cases!
    flags = 0;
    c = *fmt++;
    if (c == '%') {
      // quoting a % with %%
      if (!emit(fmt - 1, 1)) {
        return false;
      }

      continue;
    }

    if (!nas.empty()) {
      // the fmt contains the Numbered Arguments feature
      i = 0;
      while (c && c != '$') {  // should improve error check later
        i = (i * 10) + (c - '0');
        c = *fmt++;
      }

      if (nas[i - 1].type == TYPE_UNKNOWN) {
        MOZ_CRASH("Bad format string");
      }

      ap = nas[i - 1].ap;
      c = *fmt++;
    }

    // Examine optional flags.  Note that we do not implement the
    // '#' flag of sprintf().  The ANSI C spec. of the '#' flag is
    // somewhat ambiguous and not ideal, which is perhaps why
    // the various sprintf() implementations are inconsistent
    // on this feature.
    while ((c == '-') || (c == '+') || (c == ' ') || (c == '0')) {
      if (c == '-') {
        flags |= FLAG_LEFT;
      }
      if (c == '+') {
        flags |= FLAG_SIGNED;
      }
      if (c == ' ') {
        flags |= FLAG_SPACED;
      }
      if (c == '0') {
        flags |= FLAG_ZEROS;
      }
      c = *fmt++;
    }
    if (flags & FLAG_SIGNED) {
      flags &= ~FLAG_SPACED;
    }
    if (flags & FLAG_LEFT) {
      flags &= ~FLAG_ZEROS;
    }

    // width
    if (c == '*') {
      c = *fmt++;
      width = va_arg(ap, int);
      if (width < 0) {
        width = -width;
        flags |= FLAG_LEFT;
        flags &= ~FLAG_ZEROS;
      }
    } else {
      width = 0;
      while ((c >= '0') && (c <= '9')) {
        width = (width * 10) + (c - '0');
        c = *fmt++;
      }
    }

    // precision
    prec = -1;
    if (c == '.') {
      c = *fmt++;
      if (c == '*') {
        c = *fmt++;
        prec = va_arg(ap, int);
      } else {
        prec = 0;
        while ((c >= '0') && (c <= '9')) {
          prec = (prec * 10) + (c - '0');
          c = *fmt++;
        }
      }
    }

    // size
    type = TYPE_INTN;
    if (c == 'h') {
      type = TYPE_SHORT;
      c = *fmt++;
      if (c == 'h') {
        type = TYPE_SCHAR;
        c = *fmt++;
      }
    } else if (c == 'L') {
      type = TYPE_LONGLONG;
      c = *fmt++;
    } else if (c == 'l') {
      type = TYPE_LONG;
      c = *fmt++;
      if (c == 'l') {
        type = TYPE_LONGLONG;
        c = *fmt++;
      }
    } else if (c == 'z' || c == 'I') {
      static_assert(sizeof(size_t) == sizeof(int) ||
                        sizeof(size_t) == sizeof(long) ||
                        sizeof(size_t) == sizeof(long long),
                    "size_t is not one of the expected sizes");
      type = sizeof(size_t) == sizeof(int)    ? TYPE_INTN
             : sizeof(size_t) == sizeof(long) ? TYPE_LONG
                                              : TYPE_LONGLONG;
      c = *fmt++;
    } else if (c == 't') {
      static_assert(sizeof(ptrdiff_t) == sizeof(int) ||
                        sizeof(ptrdiff_t) == sizeof(long) ||
                        sizeof(ptrdiff_t) == sizeof(long long),
                    "ptrdiff_t is not one of the expected sizes");
      type = sizeof(ptrdiff_t) == sizeof(int)    ? TYPE_INTN
             : sizeof(ptrdiff_t) == sizeof(long) ? TYPE_LONG
                                                 : TYPE_LONGLONG;
      c = *fmt++;
    } else if (c == 'j') {
      static_assert(sizeof(intmax_t) == sizeof(int) ||
                        sizeof(intmax_t) == sizeof(long) ||
                        sizeof(intmax_t) == sizeof(long long),
                    "intmax_t is not one of the expected sizes");
      type = sizeof(intmax_t) == sizeof(int)    ? TYPE_INTN
             : sizeof(intmax_t) == sizeof(long) ? TYPE_LONG
                                                : TYPE_LONGLONG;
      c = *fmt++;
    }

    // format
    hexp = hex;
    switch (c) {
      case 'd':
      case 'i':  // decimal/integer
        radix = 10;
        goto fetch_and_convert;

      case 'o':  // octal
        radix = 8;
        type |= 1;
        goto fetch_and_convert;

      case 'u':  // unsigned decimal
        radix = 10;
        type |= 1;
        goto fetch_and_convert;

      case 'x':  // unsigned hex
        radix = 16;
        type |= 1;
        goto fetch_and_convert;

      case 'X':  // unsigned HEX
        radix = 16;
        hexp = HEX;
        type |= 1;
        goto fetch_and_convert;

      fetch_and_convert:
        switch (type) {
          case TYPE_SCHAR:
            u.l = (signed char)va_arg(ap, int);
            if (u.l < 0) {
              u.l = -u.l;
              flags |= FLAG_NEG;
            }
            goto do_long;
          case TYPE_UCHAR:
            u.l = (unsigned char)va_arg(ap, unsigned int);
            goto do_long;
          case TYPE_SHORT:
            u.l = (short)va_arg(ap, int);
            if (u.l < 0) {
              u.l = -u.l;
              flags |= FLAG_NEG;
            }
            goto do_long;
          case TYPE_USHORT:
            u.l = (unsigned short)va_arg(ap, unsigned int);
            goto do_long;
          case TYPE_INTN:
            u.l = va_arg(ap, int);
            if (u.l < 0) {
              u.l = -u.l;
              flags |= FLAG_NEG;
            }
            goto do_long;
          case TYPE_UINTN:
            u.l = (long)va_arg(ap, unsigned int);
            goto do_long;

          case TYPE_LONG:
            u.l = va_arg(ap, long);
            if (u.l < 0) {
              u.l = -u.l;
              flags |= FLAG_NEG;
            }
            goto do_long;
          case TYPE_ULONG:
            u.l = (long)va_arg(ap, unsigned long);
          do_long:
            if (!cvt_l(u.l, width, prec, radix, type, flags, hexp)) {
              return false;
            }

            break;

          case TYPE_LONGLONG:
            u.ll = va_arg(ap, long long);
            if (u.ll < 0) {
              u.ll = -u.ll;
              flags |= FLAG_NEG;
            }
            goto do_longlong;
          case TYPE_POINTER:
            u.ll = (uintptr_t)va_arg(ap, void*);
            goto do_longlong;
          case TYPE_ULONGLONG:
            u.ll = va_arg(ap, unsigned long long);
          do_longlong:
            if (!cvt_ll(u.ll, width, prec, radix, type, flags, hexp)) {
              return false;
            }

            break;
        }
        break;

      case 'e':
      case 'E':
      case 'f':
      case 'F':
      case 'g':
      case 'G':
        u.d = va_arg(ap, double);
        if (!cvt_f(u.d, c, width, prec, flags)) {
          return false;
        }

        break;

      case 'c':
        if ((flags & FLAG_LEFT) == 0) {
          while (width-- > 1) {
            if (!emit(" ", 1)) {
              return false;
            }
          }
        }
        switch (type) {
          case TYPE_SHORT:
          case TYPE_INTN:
            u.ch = va_arg(ap, int);
            if (!emit(&u.ch, 1)) {
              return false;
            }
            break;
        }
        if (flags & FLAG_LEFT) {
          while (width-- > 1) {
            if (!emit(" ", 1)) {
              return false;
            }
          }
        }
        break;

      case 'p':
        type = TYPE_POINTER;
        radix = 16;
        goto fetch_and_convert;

      case 's':
        if (type == TYPE_INTN) {
          u.s = va_arg(ap, const char*);
          if (!cvt_s(u.s, width, prec, flags)) {
            return false;
          }
          break;
        }
        MOZ_ASSERT(type == TYPE_LONG);
        [[fallthrough]];
      case 'S':
#if defined(XP_WIN)
      {
        u.ws = va_arg(ap, const wchar_t*);

        int rv = WideCharToMultiByte(CP_ACP, 0, u.ws, -1, NULL, 0, NULL, NULL);
        if (rv == 0 && GetLastError() == ERROR_NO_UNICODE_TRANSLATION) {
          if (!cvt_s("<unicode errors in string>", width, prec, flags)) {
            return false;
          }
        } else {
          if (rv == 0) {
            rv = 1;
          }
          UniqueFreePtr<char[]> buf((char*)malloc(rv));
          WideCharToMultiByte(CP_ACP, 0, u.ws, -1, buf.get(), rv, NULL, NULL);
          buf[rv - 1] = '\0';

          if (!cvt_s(buf.get(), width, prec, flags)) {
            return false;
          }
        }
      }
#else
        // Not supported here.
        MOZ_ASSERT(0);
#endif
      break;

      case 'n':
        u.ip = va_arg(ap, int*);
        if (u.ip) {
          *u.ip = mEmitted;
        }
        break;

      default:
        // Not a % token after all... skip it
        if (!emit("%", 1)) {
          return false;
        }
        if (!emit(fmt - 1, 1)) {
          return false;
        }
    }
  }

  return true;
}

/************************************************************************/

bool mozilla::PrintfTarget::print(const char* format, ...) {
  va_list ap;

  va_start(ap, format);
  bool result = vprint(format, ap);
  va_end(ap);
  return result;
}

#undef TYPE_SHORT
#undef TYPE_USHORT
#undef TYPE_INTN
#undef TYPE_UINTN
#undef TYPE_LONG
#undef TYPE_ULONG
#undef TYPE_LONGLONG
#undef TYPE_ULONGLONG
#undef TYPE_STRING
#undef TYPE_DOUBLE
#undef TYPE_INTSTR
#undef TYPE_POINTER
#undef TYPE_WSTRING
#undef TYPE_UNKNOWN
#undef TYPE_SCHAR
#undef TYPE_UCHAR

#undef FLAG_LEFT
#undef FLAG_SIGNED
#undef FLAG_SPACED
#undef FLAG_ZEROS
#undef FLAG_NEG

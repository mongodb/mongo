/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Portable safe sprintf code.
 *
 * Author: Kipp E.B. Hickman
 */

#include "mozilla/AllocPolicy.h"
#include "mozilla/Printf.h"
#include "mozilla/Sprintf.h"
#include "mozilla/UniquePtrExtensions.h"
#include "mozilla/Vector.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(XP_WIN)
#include <windows.h>
#endif

/*
 * Note: on some platforms va_list is defined as an array,
 * and requires array notation.
 */
#ifdef HAVE_VA_COPY
#define VARARGS_ASSIGN(foo, bar)        VA_COPY(foo, bar)
#elif defined(HAVE_VA_LIST_AS_ARRAY)
#define VARARGS_ASSIGN(foo, bar)        foo[0] = bar[0]
#else
#define VARARGS_ASSIGN(foo, bar)        (foo) = (bar)
#endif

/*
 * Numbered Argument State
 */
struct NumArgState
{
    int type;       // type of the current ap
    va_list ap;     // point to the corresponding position on ap
};

typedef mozilla::Vector<NumArgState, 20, mozilla::MallocAllocPolicy> NumArgStateVector;


#define TYPE_SHORT      0
#define TYPE_USHORT     1
#define TYPE_INTN       2
#define TYPE_UINTN      3
#define TYPE_LONG       4
#define TYPE_ULONG      5
#define TYPE_LONGLONG   6
#define TYPE_ULONGLONG  7
#define TYPE_STRING     8
#define TYPE_DOUBLE     9
#define TYPE_INTSTR     10
#define TYPE_POINTER    11
#if defined(XP_WIN)
#define TYPE_WSTRING    12
#endif
#define TYPE_UNKNOWN    20

#define FLAG_LEFT       0x1
#define FLAG_SIGNED     0x2
#define FLAG_SPACED     0x4
#define FLAG_ZEROS      0x8
#define FLAG_NEG        0x10

// Fill into the buffer using the data in src
bool
mozilla::PrintfTarget::fill2(const char* src, int srclen, int width, int flags)
{
    char space = ' ';

    width -= srclen;
    if (width > 0 && (flags & FLAG_LEFT) == 0) {    // Right adjusting
        if (flags & FLAG_ZEROS)
            space = '0';
        while (--width >= 0) {
            if (!emit(&space, 1))
                return false;
        }
    }

    // Copy out the source data
    if (!emit(src, srclen))
        return false;

    if (width > 0 && (flags & FLAG_LEFT) != 0) {    // Left adjusting
        while (--width >= 0) {
            if (!emit(&space, 1))
                return false;
        }
    }
    return true;
}

/*
 * Fill a number. The order is: optional-sign zero-filling conversion-digits
 */
bool
mozilla::PrintfTarget::fill_n(const char* src, int srclen, int width, int prec, int type, int flags)
{
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

    if (prec > 0) {
        if (prec > srclen) {
            precwidth = prec - srclen;          // Need zero filling
            cvtwidth += precwidth;
        }
    }

    if ((flags & FLAG_ZEROS) && (prec < 0)) {
        if (width > cvtwidth) {
            zerowidth = width - cvtwidth;       // Zero filling
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
        if (!emit(" ", 1))
            return false;
    }
    if (signwidth) {
        if (!emit(&sign, 1))
            return false;
    }
    while (--precwidth >= 0) {
        if (!emit("0", 1))
            return false;
    }
    while (--zerowidth >= 0) {
        if (!emit("0", 1))
            return false;
    }
    if (!emit(src, uint32_t(srclen)))
        return false;
    while (--rightspaces >= 0) {
        if (!emit(" ", 1))
            return false;
    }
    return true;
}

/* Convert a long into its printable form. */
bool
mozilla::PrintfTarget::cvt_l(long num, int width, int prec, int radix,
                             int type, int flags, const char* hexp)
{
    char cvtbuf[100];
    char* cvt;
    int digits;

    // according to the man page this needs to happen
    if ((prec == 0) && (num == 0))
        return true;

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
bool
mozilla::PrintfTarget::cvt_ll(int64_t num, int width, int prec, int radix,
                              int type, int flags, const char* hexp)
{
    // According to the man page, this needs to happen.
    if (prec == 0 && num == 0)
        return true;

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

/*
 * Convert a double precision floating point number into its printable
 * form.
 */
bool
mozilla::PrintfTarget::cvt_f(double d, const char* fmt0, const char* fmt1)
{
    char fin[20];
    // The size is chosen such that we can print DBL_MAX.  See bug#1350097.
    char fout[320];
    int amount = fmt1 - fmt0;

    MOZ_ASSERT((amount > 0) && (amount < (int)sizeof(fin)));
    if (amount >= (int)sizeof(fin)) {
        // Totally bogus % command to sprintf. Just ignore it
        return true;
    }
    memcpy(fin, fmt0, (size_t)amount);
    fin[amount] = 0;

    // Convert floating point using the native snprintf code
#ifdef DEBUG
    {
        const char* p = fin;
        while (*p) {
            MOZ_ASSERT(*p != 'L');
            p++;
        }
    }
#endif
    size_t len = SprintfLiteral(fout, fin, d);
    MOZ_RELEASE_ASSERT(len <= sizeof(fout));

    return emit(fout, len);
}

/*
 * Convert a string into its printable form.  "width" is the output
 * width. "prec" is the maximum number of characters of "s" to output,
 * where -1 means until NUL.
 */
bool
mozilla::PrintfTarget::cvt_s(const char* s, int width, int prec, int flags)
{
    if (prec == 0)
        return true;
    if (!s)
        s = "(null)";

    // Limit string length by precision value
    int slen = int(strlen(s));
    if (0 < prec && prec < slen)
        slen = prec;

    // and away we go
    return fill2(s, slen, width, flags);
}

/*
 * BuildArgArray stands for Numbered Argument list Sprintf
 * for example,
 *      fmp = "%4$i, %2$d, %3s, %1d";
 * the number must start from 1, and no gap among them
 */
static bool
BuildArgArray(const char* fmt, va_list ap, NumArgStateVector& nas)
{
    size_t number = 0, cn = 0, i;
    const char* p;
    char c;


    // First pass:
    // Detemine how many legal % I have got, then allocate space.

    p = fmt;
    i = 0;
    while ((c = *p++) != 0) {
        if (c != '%')
            continue;
        if ((c = *p++) == '%')          // skip %% case
            continue;

        while (c != 0) {
            if (c > '9' || c < '0') {
                if (c == '$') {         // numbered argument case
                    if (i > 0)
                        MOZ_CRASH("Bad format string");
                    number++;
                } else {                // non-numbered argument case
                    if (number > 0)
                        MOZ_CRASH("Bad format string");
                    i = 1;
                }
                break;
            }

            c = *p++;
        }
    }

    if (number == 0)
        return true;

    // Only allow a limited number of arguments.
    MOZ_RELEASE_ASSERT(number <= 20);

    if (!nas.growByUninitialized(number))
        return false;

    for (i = 0; i < number; i++)
        nas[i].type = TYPE_UNKNOWN;


    // Second pass:
    // Set nas[].type.

    p = fmt;
    while ((c = *p++) != 0) {
        if (c != '%')
            continue;
        c = *p++;
        if (c == '%')
            continue;

        cn = 0;
        while (c && c != '$') {     // should improve error check later
            cn = cn*10 + c - '0';
            c = *p++;
        }

        if (!c || cn < 1 || cn > number)
            MOZ_CRASH("Bad format string");

        // nas[cn] starts from 0, and make sure nas[cn].type is not assigned.
        cn--;
        if (nas[cn].type != TYPE_UNKNOWN)
            continue;

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
            static_assert(sizeof(size_t) == sizeof(int) || sizeof(size_t) == sizeof(long) ||
                          sizeof(size_t) == sizeof(long long),
                          "size_t is not one of the expected sizes");
            nas[cn].type = sizeof(size_t) == sizeof(int) ? TYPE_INTN :
                sizeof(size_t) == sizeof(long) ? TYPE_LONG : TYPE_LONGLONG;
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
        case 'f':
        case 'g':
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
            MOZ_ASSERT (nas[cn].type == TYPE_INTN);
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
        if (nas[cn].type == TYPE_UNKNOWN)
            MOZ_CRASH("Bad format string");
    }


    // Third pass:
    // Fill nas[].ap.

    cn = 0;
    while (cn < number) {
        // A TYPE_UNKNOWN here means that the format asked for a
        // positional argument without specifying the meaning of some
        // earlier argument.
        MOZ_ASSERT (nas[cn].type != TYPE_UNKNOWN);

        VARARGS_ASSIGN(nas[cn].ap, ap);

        switch (nas[cn].type) {
        case TYPE_SHORT:
        case TYPE_USHORT:
        case TYPE_INTN:
        case TYPE_UINTN:        (void) va_arg(ap, int);         break;
        case TYPE_LONG:         (void) va_arg(ap, long);        break;
        case TYPE_ULONG:        (void) va_arg(ap, unsigned long); break;
        case TYPE_LONGLONG:     (void) va_arg(ap, long long);   break;
        case TYPE_ULONGLONG:    (void) va_arg(ap, unsigned long long); break;
        case TYPE_STRING:       (void) va_arg(ap, char*);       break;
        case TYPE_INTSTR:       (void) va_arg(ap, int*);        break;
        case TYPE_DOUBLE:       (void) va_arg(ap, double);      break;
        case TYPE_POINTER:      (void) va_arg(ap, void*);       break;
#if defined(XP_WIN)
        case TYPE_WSTRING:      (void) va_arg(ap, wchar_t*);    break;
#endif

        default: MOZ_CRASH();
        }

        cn++;
    }

    return true;
}

mozilla::PrintfTarget::PrintfTarget()
  : mEmitted(0)
{
}

bool
mozilla::PrintfTarget::vprint(const char* fmt, va_list ap)
{
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
    } u;
    const char* fmt0;
    static const char hex[] = "0123456789abcdef";
    static const char HEX[] = "0123456789ABCDEF";
    const char* hexp;
    int i;
    char pattern[20];
    const char* dolPt = nullptr;  // in "%4$.2f", dolPt will point to '.'

    // Build an argument array, IF the fmt is numbered argument
    // list style, to contain the Numbered Argument list pointers.

    NumArgStateVector nas;
    if (!BuildArgArray(fmt, ap, nas)) {
        // the fmt contains error Numbered Argument format, jliu@netscape.com
        MOZ_CRASH("Bad format string");
    }

    while ((c = *fmt++) != 0) {
        if (c != '%') {
            if (!emit(fmt - 1, 1))
                return false;

            continue;
        }
        fmt0 = fmt - 1;

        // Gobble up the % format string. Hopefully we have handled all
        // of the strange cases!
        flags = 0;
        c = *fmt++;
        if (c == '%') {
            // quoting a % with %%
            if (!emit(fmt - 1, 1))
                return false;

            continue;
        }

        if (!nas.empty()) {
            // the fmt contains the Numbered Arguments feature
            i = 0;
            while (c && c != '$') {         // should improve error check later
                i = (i * 10) + (c - '0');
                c = *fmt++;
            }

            if (nas[i - 1].type == TYPE_UNKNOWN)
                MOZ_CRASH("Bad format string");

            ap = nas[i - 1].ap;
            dolPt = fmt;
            c = *fmt++;
        }

        // Examine optional flags.  Note that we do not implement the
        // '#' flag of sprintf().  The ANSI C spec. of the '#' flag is
        // somewhat ambiguous and not ideal, which is perhaps why
        // the various sprintf() implementations are inconsistent
        // on this feature.
        while ((c == '-') || (c == '+') || (c == ' ') || (c == '0')) {
            if (c == '-') flags |= FLAG_LEFT;
            if (c == '+') flags |= FLAG_SIGNED;
            if (c == ' ') flags |= FLAG_SPACED;
            if (c == '0') flags |= FLAG_ZEROS;
            c = *fmt++;
        }
        if (flags & FLAG_SIGNED) flags &= ~FLAG_SPACED;
        if (flags & FLAG_LEFT) flags &= ~FLAG_ZEROS;

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
            static_assert(sizeof(size_t) == sizeof(int) || sizeof(size_t) == sizeof(long) ||
                          sizeof(size_t) == sizeof(long long),
                          "size_t is not one of the expected sizes");
            type = sizeof(size_t) == sizeof(int) ? TYPE_INTN :
                sizeof(size_t) == sizeof(long) ? TYPE_LONG : TYPE_LONGLONG;
            c = *fmt++;
        }

        // format
        hexp = hex;
        switch (c) {
          case 'd': case 'i':                   // decimal/integer
            radix = 10;
            goto fetch_and_convert;

          case 'o':                             // octal
            radix = 8;
            type |= 1;
            goto fetch_and_convert;

          case 'u':                             // unsigned decimal
            radix = 10;
            type |= 1;
            goto fetch_and_convert;

          case 'x':                             // unsigned hex
            radix = 16;
            type |= 1;
            goto fetch_and_convert;

          case 'X':                             // unsigned HEX
            radix = 16;
            hexp = HEX;
            type |= 1;
            goto fetch_and_convert;

          fetch_and_convert:
            switch (type) {
              case TYPE_SHORT:
                u.l = va_arg(ap, int);
                if (u.l < 0) {
                    u.l = -u.l;
                    flags |= FLAG_NEG;
                }
                goto do_long;
              case TYPE_USHORT:
                u.l = (unsigned short) va_arg(ap, unsigned int);
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
                if (!cvt_l(u.l, width, prec, radix, type, flags, hexp))
                    return false;

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
                if (!cvt_ll(u.ll, width, prec, radix, type, flags, hexp))
                    return false;

                break;
            }
            break;

          case 'e':
          case 'E':
          case 'f':
          case 'g':
            u.d = va_arg(ap, double);
            if (!nas.empty()) {
                i = fmt - dolPt;
                if (i < int(sizeof(pattern))) {
                    pattern[0] = '%';
                    memcpy(&pattern[1], dolPt, size_t(i));
                    if (!cvt_f(u.d, pattern, &pattern[i + 1]))
                        return false;
                }
            } else {
                if (!cvt_f(u.d, fmt0, fmt))
                    return false;
            }

            break;

          case 'c':
            if ((flags & FLAG_LEFT) == 0) {
                while (width-- > 1) {
                    if (!emit(" ", 1))
                        return false;
                }
            }
            switch (type) {
              case TYPE_SHORT:
              case TYPE_INTN:
                u.ch = va_arg(ap, int);
                if (!emit(&u.ch, 1))
                    return false;
                break;
            }
            if (flags & FLAG_LEFT) {
                while (width-- > 1) {
                    if (!emit(" ", 1))
                        return false;
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
                if (!cvt_s(u.s, width, prec, flags))
                    return false;
                break;
            }
            MOZ_ASSERT(type == TYPE_LONG);
            MOZ_FALLTHROUGH;
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
            if (!emit("%", 1))
                return false;
            if (!emit(fmt - 1, 1))
                return false;
        }
    }

    return true;
}

/************************************************************************/

bool
mozilla::PrintfTarget::print(const char* format, ...)
{
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

#undef FLAG_LEFT
#undef FLAG_SIGNED
#undef FLAG_SPACED
#undef FLAG_ZEROS
#undef FLAG_NEG

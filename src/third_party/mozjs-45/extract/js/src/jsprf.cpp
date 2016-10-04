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

#include "jsprf.h"

#include "mozilla/Snprintf.h"
#include "mozilla/Vector.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jsalloc.h"
#include "jspubtd.h"
#include "jsstr.h"
#include "jsutil.h"

using namespace js;

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

struct SprintfState
{
    bool (*stuff)(SprintfState* ss, const char* sp, size_t len);

    char* base;
    char* cur;
    size_t maxlen;

    int (*func)(void* arg, const char* sp, uint32_t len);
    void* arg;
};

/*
 * Numbered Argument State
 */
struct NumArgState
{
    int type;       // type of the current ap
    va_list ap;     // point to the corresponding position on ap
};

typedef mozilla::Vector<NumArgState, 20, js::SystemAllocPolicy> NumArgStateVector;


#define TYPE_INT16      0
#define TYPE_UINT16     1
#define TYPE_INTN       2
#define TYPE_UINTN      3
#define TYPE_INT32      4
#define TYPE_UINT32     5
#define TYPE_INT64      6
#define TYPE_UINT64     7
#define TYPE_STRING     8
#define TYPE_DOUBLE     9
#define TYPE_INTSTR     10
#define TYPE_WSTRING    11
#define TYPE_UNKNOWN    20

#define FLAG_LEFT       0x1
#define FLAG_SIGNED     0x2
#define FLAG_SPACED     0x4
#define FLAG_ZEROS      0x8
#define FLAG_NEG        0x10

inline bool
generic_write(SprintfState* ss, const char* src, size_t srclen)
{
    return (*ss->stuff)(ss, src, srclen);
}

inline bool
generic_write(SprintfState* ss, const char16_t* src, size_t srclen)
{
    const size_t CHUNK_SIZE = 64;
    char chunk[CHUNK_SIZE];

    size_t j = 0;
    size_t i = 0;
    while (i < srclen) {
        // FIXME: truncates characters to 8 bits
        chunk[j++] = char(src[i++]);

        if (j == CHUNK_SIZE || i == srclen) {
            if (!(*ss->stuff)(ss, chunk, j))
                return false;
            j = 0;
        }
    }
    return true;
}

// Fill into the buffer using the data in src
template <typename Char>
static bool
fill2(SprintfState* ss, const Char* src, int srclen, int width, int flags)
{
    char space = ' ';

    width -= srclen;
    if (width > 0 && (flags & FLAG_LEFT) == 0) {    // Right adjusting
        if (flags & FLAG_ZEROS)
            space = '0';
        while (--width >= 0) {
            if (!(*ss->stuff)(ss, &space, 1))
                return false;
        }
    }

    // Copy out the source data
    if (!generic_write(ss, src, srclen))
        return false;

    if (width > 0 && (flags & FLAG_LEFT) != 0) {    // Left adjusting
        while (--width >= 0) {
            if (!(*ss->stuff)(ss, &space, 1))
                return false;
        }
    }
    return true;
}

/*
 * Fill a number. The order is: optional-sign zero-filling conversion-digits
 */
static bool
fill_n(SprintfState* ss, const char* src, int srclen, int width, int prec, int type, int flags)
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
        if (!(*ss->stuff)(ss, " ", 1))
            return false;
    }
    if (signwidth) {
        if (!(*ss->stuff)(ss, &sign, 1))
            return false;
    }
    while (--precwidth >= 0) {
        if (!(*ss->stuff)(ss, "0", 1))
            return false;
    }
    while (--zerowidth >= 0) {
        if (!(*ss->stuff)(ss, "0", 1))
            return false;
    }
    if (!(*ss->stuff)(ss, src, uint32_t(srclen)))
        return false;
    while (--rightspaces >= 0) {
        if (!(*ss->stuff)(ss, " ", 1))
            return false;
    }
    return true;
}

/* Convert a long into its printable form. */
static bool cvt_l(SprintfState* ss, long num, int width, int prec, int radix,
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
    return fill_n(ss, cvt, digits, width, prec, type, flags);
}

/* Convert a 64-bit integer into its printable form. */
static bool cvt_ll(SprintfState* ss, int64_t num, int width, int prec, int radix,
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
    return fill_n(ss, cvt, digits, width, prec, type, flags);
}

/*
 * Convert a double precision floating point number into its printable
 * form.
 */
static bool cvt_f(SprintfState* ss, double d, const char* fmt0, const char* fmt1)
{
    char fin[20];
    char fout[300];
    int amount = fmt1 - fmt0;

    MOZ_ASSERT((amount > 0) && (amount < (int)sizeof(fin)));
    if (amount >= (int)sizeof(fin)) {
        // Totally bogus % command to sprintf. Just ignore it
        return true;
    }
    js_memcpy(fin, fmt0, (size_t)amount);
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
    snprintf_literal(fout, fin, d);

    return (*ss->stuff)(ss, fout, strlen(fout));
}

static inline const char* generic_null_str(const char*) { return "(null)"; }
static inline const char16_t* generic_null_str(const char16_t*) { return MOZ_UTF16("(null)"); }

static inline size_t generic_strlen(const char* s) { return strlen(s); }
static inline size_t generic_strlen(const char16_t* s) { return js_strlen(s); }

/*
 * Convert a string into its printable form.  "width" is the output
 * width. "prec" is the maximum number of characters of "s" to output,
 * where -1 means until NUL.
 */
template <typename Char>
static bool
cvt_s(SprintfState* ss, const Char* s, int width, int prec, int flags)
{
    if (prec == 0)
        return true;
    if (!s)
        s = generic_null_str(s);

    // Limit string length by precision value
    int slen = int(generic_strlen(s));
    if (0 < prec && prec < slen)
        slen = prec;

    // and away we go
    return fill2(ss, s, slen, width, flags);
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
            nas[cn].type = TYPE_INT16;
            c = *p++;
        } else if (c == 'L') {
            // XXX not quite sure here
            nas[cn].type = TYPE_INT64;
            c = *p++;
        } else if (c == 'l') {
            nas[cn].type = TYPE_INT32;
            c = *p++;
            if (c == 'l') {
                nas[cn].type = TYPE_INT64;
                c = *p++;
            }
        } else if (c == 'z' || c == 'I') {
            static_assert(sizeof(size_t) == sizeof(int32_t) || sizeof(size_t) == sizeof(int64_t),
                          "size_t is not one of the expected sizes");
            nas[cn].type = sizeof(size_t) == sizeof(int64_t) ? TYPE_INT64 : TYPE_INT32;
            c = *p++;
        }

        // format
        switch (c) {
        case 'd':
        case 'c':
        case 'i':
        case 'o':
        case 'u':
        case 'x':
        case 'X':
            break;

        case 'e':
        case 'f':
        case 'g':
            nas[cn].type = TYPE_DOUBLE;
            break;

        case 'p':
            // XXX should use cpp
            if (sizeof(void*) == sizeof(int32_t)) {
                nas[cn].type = TYPE_UINT32;
            } else if (sizeof(void*) == sizeof(int64_t)) {
                nas[cn].type = TYPE_UINT64;
            } else if (sizeof(void*) == sizeof(int)) {
                nas[cn].type = TYPE_UINTN;
            } else {
                nas[cn].type = TYPE_UNKNOWN;
            }
            break;

        case 'C':
        case 'S':
        case 'E':
        case 'G':
            // XXX not supported I suppose
            MOZ_ASSERT(0);
            nas[cn].type = TYPE_UNKNOWN;
            break;

        case 's':
            nas[cn].type = (nas[cn].type == TYPE_UINT16) ? TYPE_WSTRING : TYPE_STRING;
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
        if (nas[cn].type == TYPE_UNKNOWN) {
            cn++;
            continue;
        }

        VARARGS_ASSIGN(nas[cn].ap, ap);

        switch (nas[cn].type) {
        case TYPE_INT16:
        case TYPE_UINT16:
        case TYPE_INTN:
        case TYPE_UINTN:        (void) va_arg(ap, int);         break;
        case TYPE_INT32:        (void) va_arg(ap, int32_t);     break;
        case TYPE_UINT32:       (void) va_arg(ap, uint32_t);    break;
        case TYPE_INT64:        (void) va_arg(ap, int64_t);     break;
        case TYPE_UINT64:       (void) va_arg(ap, uint64_t);    break;
        case TYPE_STRING:       (void) va_arg(ap, char*);       break;
        case TYPE_WSTRING:      (void) va_arg(ap, char16_t*);   break;
        case TYPE_INTSTR:       (void) va_arg(ap, int*);        break;
        case TYPE_DOUBLE:       (void) va_arg(ap, double);      break;

        default: MOZ_CRASH();
        }

        cn++;
    }

    return true;
}

/*
 * The workhorse sprintf code.
 */
static bool
dosprintf(SprintfState* ss, const char* fmt, va_list ap)
{
    char c;
    int flags, width, prec, radix, type;
    union {
        char ch;
        char16_t wch;
        int i;
        long l;
        int64_t ll;
        double d;
        const char* s;
        const char16_t* ws;
        int* ip;
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
            if (!(*ss->stuff)(ss, fmt - 1, 1))
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
            if (!(*ss->stuff)(ss, fmt - 1, 1))
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
            type = TYPE_INT16;
            c = *fmt++;
        } else if (c == 'L') {
            // XXX not quite sure here
            type = TYPE_INT64;
            c = *fmt++;
        } else if (c == 'l') {
            type = TYPE_INT32;
            c = *fmt++;
            if (c == 'l') {
                type = TYPE_INT64;
                c = *fmt++;
            }
        } else if (c == 'z' || c == 'I') {
            static_assert(sizeof(size_t) == sizeof(int32_t) || sizeof(size_t) == sizeof(int64_t),
                          "size_t is not one of the expected sizes");
            type = sizeof(size_t) == sizeof(int64_t) ? TYPE_INT64 : TYPE_INT32;
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
              case TYPE_INT16:
                u.l = va_arg(ap, int);
                if (u.l < 0) {
                    u.l = -u.l;
                    flags |= FLAG_NEG;
                }
                goto do_long;
              case TYPE_UINT16:
                u.l = va_arg(ap, int) & 0xffff;
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

              case TYPE_INT32:
                u.l = va_arg(ap, int32_t);
                if (u.l < 0) {
                    u.l = -u.l;
                    flags |= FLAG_NEG;
                }
                goto do_long;
              case TYPE_UINT32:
                u.l = (long)va_arg(ap, uint32_t);
              do_long:
                if (!cvt_l(ss, u.l, width, prec, radix, type, flags, hexp))
                    return false;

                break;

              case TYPE_INT64:
                u.ll = va_arg(ap, int64_t);
                if (u.ll < 0) {
                    u.ll = -u.ll;
                    flags |= FLAG_NEG;
                }
                goto do_longlong;
              case TYPE_UINT64:
                u.ll = va_arg(ap, uint64_t);
              do_longlong:
                if (!cvt_ll(ss, u.ll, width, prec, radix, type, flags, hexp))
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
                    js_memcpy(&pattern[1], dolPt, size_t(i));
                    if (!cvt_f(ss, u.d, pattern, &pattern[i + 1]))
                        return false;
                }
            } else {
                if (!cvt_f(ss, u.d, fmt0, fmt))
                    return false;
            }

            break;

          case 'c':
            if ((flags & FLAG_LEFT) == 0) {
                while (width-- > 1) {
                    if (!(*ss->stuff)(ss, " ", 1))
                        return false;
                }
            }
            switch (type) {
              case TYPE_INT16:
              case TYPE_INTN:
                u.ch = va_arg(ap, int);
                if (!(*ss->stuff)(ss, &u.ch, 1))
                    return false;
                break;
            }
            if (flags & FLAG_LEFT) {
                while (width-- > 1) {
                    if (!(*ss->stuff)(ss, " ", 1))
                        return false;
                }
            }
            break;

          case 'p':
            if (sizeof(void*) == sizeof(int32_t)) {
                type = TYPE_UINT32;
            } else if (sizeof(void*) == sizeof(int64_t)) {
                type = TYPE_UINT64;
            } else if (sizeof(void*) == sizeof(int)) {
                type = TYPE_UINTN;
            } else {
                MOZ_ASSERT(0);
                break;
            }
            radix = 16;
            goto fetch_and_convert;

#if 0
          case 'C':
          case 'S':
          case 'E':
          case 'G':
            // XXX not supported I suppose
            MOZ_ASSERT(0);
            break;
#endif

          case 's':
            if(type == TYPE_INT16) {
                u.ws = va_arg(ap, const char16_t*);
                if (!cvt_s(ss, u.ws, width, prec, flags))
                    return false;
            } else {
                u.s = va_arg(ap, const char*);
                if (!cvt_s(ss, u.s, width, prec, flags))
                    return false;
            }
            break;

          case 'n':
            u.ip = va_arg(ap, int*);
            if (u.ip) {
                *u.ip = ss->cur - ss->base;
            }
            break;

          default:
            // Not a % token after all... skip it
#if 0
            MOZ_ASSERT(0);
#endif
            if (!(*ss->stuff)(ss, "%", 1))
                return false;
            if (!(*ss->stuff)(ss, fmt - 1, 1))
                return false;
        }
    }

    // Stuff trailing NUL
    if (!(*ss->stuff)(ss, "\0", 1))
        return false;

    return true;
}

/************************************************************************/

/*
 * Stuff routine that automatically grows the js_malloc'd output buffer
 * before it overflows.
 */
static bool
GrowStuff(SprintfState* ss, const char* sp, size_t len)
{
    ptrdiff_t off;
    char* newbase;
    size_t newlen;

    off = ss->cur - ss->base;
    if (off + len >= ss->maxlen) {
        /* Grow the buffer */
        newlen = ss->maxlen + ((len > 32) ? len : 32);
        newbase = static_cast<char*>(js_realloc(ss->base, newlen));
        if (!newbase) {
            /* Ran out of memory */
            return false;
        }
        ss->base = newbase;
        ss->maxlen = newlen;
        ss->cur = ss->base + off;
    }

    /* Copy data */
    while (len) {
        --len;
        *ss->cur++ = *sp++;
    }
    MOZ_ASSERT(size_t(ss->cur - ss->base) <= ss->maxlen);
    return true;
}

/*
 * sprintf into a js_malloc'd buffer
 */
JS_PUBLIC_API(char*)
JS_smprintf(const char* fmt, ...)
{
    va_list ap;
    char* rv;

    va_start(ap, fmt);
    rv = JS_vsmprintf(fmt, ap);
    va_end(ap);
    return rv;
}

/*
 * Free memory allocated, for the caller, by JS_smprintf
 */
JS_PUBLIC_API(void)
JS_smprintf_free(char* mem)
{
    js_free(mem);
}

JS_PUBLIC_API(char*)
JS_vsmprintf(const char* fmt, va_list ap)
{
    SprintfState ss;

    ss.stuff = GrowStuff;
    ss.base = 0;
    ss.cur = 0;
    ss.maxlen = 0;
    if (!dosprintf(&ss, fmt, ap)) {
        js_free(ss.base);
        return 0;
    }
    return ss.base;
}

/*
 * Stuff routine that discards overflow data
 */
static bool
LimitStuff(SprintfState* ss, const char* sp, size_t len)
{
    size_t limit = ss->maxlen - (ss->cur - ss->base);

    if (len > limit)
        len = limit;
    while (len) {
        --len;
        *ss->cur++ = *sp++;
    }
    return true;
}

/*
 * sprintf into a fixed size buffer. Make sure there is a NUL at the end
 * when finished.
 */
JS_PUBLIC_API(uint32_t)
JS_snprintf(char* out, uint32_t outlen, const char* fmt, ...)
{
    va_list ap;
    int rv;

    MOZ_ASSERT(int32_t(outlen) > 0);
    if (int32_t(outlen) <= 0)
        return 0;

    va_start(ap, fmt);
    rv = JS_vsnprintf(out, outlen, fmt, ap);
    va_end(ap);
    return rv;
}

JS_PUBLIC_API(uint32_t)
JS_vsnprintf(char* out, uint32_t outlen, const char* fmt, va_list ap)
{
    SprintfState ss;

    if (outlen == 0)
        return 0;

    ss.stuff = LimitStuff;
    ss.base = out;
    ss.cur = out;
    ss.maxlen = outlen;
    (void) dosprintf(&ss, fmt, ap);

    uint32_t charsWritten = ss.cur - ss.base;
    MOZ_ASSERT(charsWritten > 0);

    // If we didn't append a null then we must have hit the buffer limit. Write
    // a null terminator now and return a value indicating that we failed.
    if (ss.cur[-1] != '\0') {
        ss.cur[-1] = '\0';
        return outlen;
    }

    // Success: return the number of character written excluding the null
    // terminator.
    return charsWritten - 1;
}

JS_PUBLIC_API(char*)
JS_sprintf_append(char* last, const char* fmt, ...)
{
    va_list ap;
    char* rv;

    va_start(ap, fmt);
    rv = JS_vsprintf_append(last, fmt, ap);
    va_end(ap);
    return rv;
}

JS_PUBLIC_API(char*)
JS_vsprintf_append(char* last, const char* fmt, va_list ap)
{
    SprintfState ss;

    ss.stuff = GrowStuff;
    if (last) {
        size_t lastlen = strlen(last);
        ss.base = last;
        ss.cur = last + lastlen;
        ss.maxlen = lastlen;
    } else {
        ss.base = 0;
        ss.cur = 0;
        ss.maxlen = 0;
    }
    if (!dosprintf(&ss, fmt, ap)) {
        js_free(ss.base);
        return 0;
    }
    return ss.base;
}

#undef TYPE_INT16
#undef TYPE_UINT16
#undef TYPE_INTN
#undef TYPE_UINTN
#undef TYPE_INT32
#undef TYPE_UINT32
#undef TYPE_INT64
#undef TYPE_UINT64
#undef TYPE_STRING
#undef TYPE_DOUBLE
#undef TYPE_INTSTR
#undef TYPE_WSTRING
#undef TYPE_UNKNOWN

#undef FLAG_LEFT
#undef FLAG_SIGNED
#undef FLAG_SPACED
#undef FLAG_ZEROS
#undef FLAG_NEG

/***************************************************************************************************

  Zyan Core Library (Zycore-C)

  Original Author : Florian Bernd, Joel Hoener

 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.

***************************************************************************************************/

/**
 * @file
 * Provides a simple LibC abstraction and fallback routines.
 */

#ifndef ZYCORE_LIBC_H
#define ZYCORE_LIBC_H

#ifndef ZYAN_CUSTOM_LIBC

// Include a custom LibC header and define `ZYAN_CUSTOM_LIBC` to provide your own LibC
// replacement functions

#ifndef ZYAN_NO_LIBC

/* ============================================================================================== */
/* LibC is available                                                                              */
/* ============================================================================================== */

/* ---------------------------------------------------------------------------------------------- */
/* errno.h                                                                                        */
/* ---------------------------------------------------------------------------------------------- */

#include <errno.h>

#define ZYAN_ERRNO  errno

/* ---------------------------------------------------------------------------------------------- */
/* stdarg.h                                                                                       */
/* ---------------------------------------------------------------------------------------------- */

#include <stdarg.h>

/**
 * Defines the `ZyanVAList` datatype.
 */
typedef va_list ZyanVAList;

#define ZYAN_VA_START               va_start
#define ZYAN_VA_ARG                 va_arg
#define ZYAN_VA_END                 va_end
#define ZYAN_VA_COPY(dest, source)  va_copy((dest), (source))

/* ---------------------------------------------------------------------------------------------- */
/* stdio.h                                                                                        */
/* ---------------------------------------------------------------------------------------------- */

#include <stdio.h>

#define ZYAN_FPUTS      fputs
#define ZYAN_FPUTC      fputc
#define ZYAN_FPRINTF    fprintf
#define ZYAN_PRINTF     printf
#define ZYAN_PUTC       putc
#define ZYAN_PUTS       puts
#define ZYAN_SCANF      scanf
#define ZYAN_SSCANF     sscanf
#define ZYAN_VSNPRINTF  vsnprintf

/**
 * Defines the `ZyanFile` datatype.
 */
typedef FILE ZyanFile;

#define ZYAN_STDIN      stdin
#define ZYAN_STDOUT     stdout
#define ZYAN_STDERR     stderr

/* ---------------------------------------------------------------------------------------------- */
/* stdlib.h                                                                                       */
/* ---------------------------------------------------------------------------------------------- */

#include <stdlib.h>
#define ZYAN_CALLOC     calloc
#define ZYAN_FREE       free
#define ZYAN_MALLOC     malloc
#define ZYAN_REALLOC    realloc

/* ---------------------------------------------------------------------------------------------- */
/* string.h                                                                                       */
/* ---------------------------------------------------------------------------------------------- */

#include <string.h>
#define ZYAN_MEMCHR     memchr
#define ZYAN_MEMCMP     memcmp
#define ZYAN_MEMCPY     memcpy
#define ZYAN_MEMMOVE    memmove
#define ZYAN_MEMSET     memset
#define ZYAN_STRCAT     strcat
#define ZYAN_STRCHR     strchr
#define ZYAN_STRCMP     strcmp
#define ZYAN_STRCOLL    strcoll
#define ZYAN_STRCPY     strcpy
#define ZYAN_STRCSPN    strcspn
#define ZYAN_STRLEN     strlen
#define ZYAN_STRNCAT    strncat
#define ZYAN_STRNCMP    strncmp
#define ZYAN_STRNCPY    strncpy
#define ZYAN_STRPBRK    strpbrk
#define ZYAN_STRRCHR    strrchr
#define ZYAN_STRSPN     strspn
#define ZYAN_STRSTR     strstr
#define ZYAN_STRTOK     strtok
#define ZYAN_STRXFRM    strxfrm

/* ---------------------------------------------------------------------------------------------- */

#else  // if ZYAN_NO_LIBC

/* ============================================================================================== */
/* No LibC available, use our own functions                                                       */
/* ============================================================================================== */

#include "zydis/Zycore/Defines.h"
#include "zydis/Zycore/Types.h"

/*
 * These implementations are by no means optimized and will be outperformed by pretty much any
 * libc implementation out there. We do not aim towards providing competetive implementations here,
 * but towards providing a last resort fallback for environments without a working libc.
 */

/* ---------------------------------------------------------------------------------------------- */
/* stdarg.h                                                                                       */
/* ---------------------------------------------------------------------------------------------- */

#if defined(ZYAN_MSVC) || defined(ZYAN_ICC)

/**
 * Defines the `ZyanVAList` datatype.
 */
typedef char* ZyanVAList;

#   define ZYAN_VA_START __crt_va_start
#   define ZYAN_VA_ARG   __crt_va_arg
#   define ZYAN_VA_END   __crt_va_end
#   define ZYAN_VA_COPY(destination, source) ((destination) = (source))

#elif defined(ZYAN_GNUC)

/**
 * Defines the `ZyanVAList` datatype.
 */
typedef __builtin_va_list  ZyanVAList;

#   define ZYAN_VA_START(v, l)  __builtin_va_start(v, l)
#   define ZYAN_VA_END(v)       __builtin_va_end(v)
#   define ZYAN_VA_ARG(v, l)    __builtin_va_arg(v, l)
#   define ZYAN_VA_COPY(d, s)   __builtin_va_copy(d, s)

#else
#   error "Unsupported compiler for no-libc mode."
#endif

/* ---------------------------------------------------------------------------------------------- */
/* stdio.h                                                                                        */
/* ---------------------------------------------------------------------------------------------- */

// ZYAN_INLINE int ZYAN_VSNPRINTF (char* const buffer, ZyanUSize const count,
//     char const* const format, ZyanVAList args)
// {
//      // We cant provide a fallback implementation for this function
//     ZYAN_UNUSED(buffer);
//     ZYAN_UNUSED(count);
//     ZYAN_UNUSED(format);
//     ZYAN_UNUSED(args);
//     return ZYAN_NULL;
// }

/* ---------------------------------------------------------------------------------------------- */
/* stdlib.h                                                                                       */
/* ---------------------------------------------------------------------------------------------- */

// ZYAN_INLINE void* ZYAN_CALLOC(ZyanUSize nitems, ZyanUSize size)
// {
//      // We cant provide a fallback implementation for this function
//     ZYAN_UNUSED(nitems);
//     ZYAN_UNUSED(size);
//     return ZYAN_NULL;
// }
//
// ZYAN_INLINE void ZYAN_FREE(void *p)
// {
//      // We cant provide a fallback implementation for this function
//     ZYAN_UNUSED(p);
// }
//
// ZYAN_INLINE void* ZYAN_MALLOC(ZyanUSize n)
// {
//     // We cant provide a fallback implementation for this function
//     ZYAN_UNUSED(n);
//     return ZYAN_NULL;
// }
//
// ZYAN_INLINE void* ZYAN_REALLOC(void* p, ZyanUSize n)
// {
//      // We cant provide a fallback implementation for this function
//     ZYAN_UNUSED(p);
//     ZYAN_UNUSED(n);
//     return ZYAN_NULL;
// }

/* ---------------------------------------------------------------------------------------------- */
/* string.h                                                                                       */
/* ---------------------------------------------------------------------------------------------- */

ZYAN_INLINE void* ZYAN_MEMCHR(const void* str, int c, ZyanUSize n)
{
    const ZyanU8* p = (ZyanU8*)str;
    while (n--)
    {
        if (*p != (ZyanU8)c)
        {
            p++;
        } else
        {
            return (void*)p;
        }
    }
    return 0;
}

ZYAN_INLINE int ZYAN_MEMCMP(const void* s1, const void* s2, ZyanUSize n)
{
    const ZyanU8* p1 = s1, *p2 = s2;
    while (n--)
    {
        if (*p1 != *p2)
        {
            return *p1 - *p2;
        }
        p1++, p2++;
    }
    return 0;
}

ZYAN_INLINE void* ZYAN_MEMCPY(void* dst, const void* src, ZyanUSize n)
{
    volatile ZyanU8* dp = dst;
    const ZyanU8* sp = src;
    while (n--)
    {
        *dp++ = *sp++;
    }
    return dst;
}

ZYAN_INLINE void* ZYAN_MEMMOVE(void* dst, const void* src, ZyanUSize n)
{
    volatile ZyanU8* pd = dst;
    const ZyanU8* ps = src;
    if (ps < pd)
    {
        for (pd += n, ps += n; n--;)
        {
            *--pd = *--ps;
        }
    } else
    {
        while (n--)
        {
            *pd++ = *ps++;
        }
    }
    return dst;
}

ZYAN_INLINE void* ZYAN_MEMSET(void* dst, int val, ZyanUSize n)
{
    volatile ZyanU8* p = dst;
    while (n--)
    {
        *p++ = (unsigned char)val;
    }
    return dst;
}

ZYAN_INLINE char* ZYAN_STRCAT(char* dest, const char* src)
{
    char* ret = dest;
    while (*dest)
    {
        dest++;
    }
    while ((*dest++ = *src++));
    return ret;
}

ZYAN_INLINE char* ZYAN_STRCHR(const char* s, int c)
{
    while (*s != (char)c)
    {
        if (!*s++)
        {
            return 0;
        }
    }
    return (char*)s;
}

ZYAN_INLINE int ZYAN_STRCMP(const char* s1, const char* s2)
{
    while (*s1 && (*s1 == *s2))
    {
        s1++, s2++;
    }
    return *(const ZyanU8*)s1 - *(const ZyanU8*)s2;
}

ZYAN_INLINE int ZYAN_STRCOLL(const char *s1, const char *s2)
{
    // TODO: Implement

    ZYAN_UNUSED(s1);
    ZYAN_UNUSED(s2);

    return 0;
}

ZYAN_INLINE char* ZYAN_STRCPY(char* dest, const char* src)
{
    char* ret = dest;
    while ((*dest++ = *src++));
    return ret;
}

ZYAN_INLINE ZyanUSize ZYAN_STRCSPN(const char *s1, const char *s2)
{
    ZyanUSize ret = 0;
    while (*s1)
    {
        if (ZYAN_STRCHR(s2, *s1))
        {
            return ret;
        }
        s1++, ret++;
    }
    return ret;
}

ZYAN_INLINE ZyanUSize ZYAN_STRLEN(const char* str)
{
    const char* p = str;
    while (*str)
    {
        ++str;
    }
    return str - p;
}

ZYAN_INLINE char* ZYAN_STRNCAT(char* dest, const char* src, ZyanUSize n)
{
    char* ret = dest;
    while (*dest)
    {
        dest++;
    }
    while (n--)
    {
        if (!(*dest++ = *src++))
        {
            return ret;
        }
    }
    *dest = 0;
    return ret;
}

ZYAN_INLINE int ZYAN_STRNCMP(const char* s1, const char* s2, ZyanUSize n)
{
    while (n--)
    {
        if (*s1++ != *s2++)
        {
            return *(unsigned char*)(s1 - 1) - *(unsigned char*)(s2 - 1);
        }
    }
    return 0;
}

ZYAN_INLINE char* ZYAN_STRNCPY(char* dest, const char* src, ZyanUSize n)
{
    char* ret = dest;
    do
    {
        if (!n--)
        {
            return ret;
        }
    } while ((*dest++ = *src++));
    while (n--)
    {
        *dest++ = 0;
    }
    return ret;
}

ZYAN_INLINE char* ZYAN_STRPBRK(const char* s1, const char* s2)
{
    while (*s1)
    {
        if(ZYAN_STRCHR(s2, *s1++))
        {
            return (char*)--s1;
        }
    }
    return 0;
}

ZYAN_INLINE char* ZYAN_STRRCHR(const char* s, int c)
{
    char* ret = 0;
    do
    {
        if (*s == (char)c)
        {
            ret = (char*)s;
        }
    } while (*s++);
    return ret;
}

ZYAN_INLINE ZyanUSize ZYAN_STRSPN(const char* s1, const char* s2)
{
    ZyanUSize ret = 0;
    while (*s1 && ZYAN_STRCHR(s2, *s1++))
    {
        ret++;
    }
    return ret;
}

ZYAN_INLINE char* ZYAN_STRSTR(const char* s1, const char* s2)
{
    const ZyanUSize n = ZYAN_STRLEN(s2);
    while (*s1)
    {
        if (!ZYAN_MEMCMP(s1++, s2, n))
        {
            return (char*)(s1 - 1);
        }
    }
    return 0;
}

ZYAN_INLINE char* ZYAN_STRTOK(char* str, const char* delim)
{
    static char* p = 0;
    if (str)
    {
        p = str;
    } else
    if (!p)
    {
        return 0;
    }
    str = p + ZYAN_STRSPN(p, delim);
    p = str + ZYAN_STRCSPN(str, delim);
    if (p == str)
    {
        return p = 0;
    }
    p = *p ? *p = 0, p + 1 : 0;
    return str;
}

ZYAN_INLINE ZyanUSize ZYAN_STRXFRM(char* dest, const char* src, ZyanUSize n)
{
    const ZyanUSize n2 = ZYAN_STRLEN(src);
    if (n > n2)
    {
        ZYAN_STRCPY(dest, src);
    }
    return n2;
}

/* ---------------------------------------------------------------------------------------------- */

#endif

#endif

/* ============================================================================================== */

#endif /* ZYCORE_LIBC_H */

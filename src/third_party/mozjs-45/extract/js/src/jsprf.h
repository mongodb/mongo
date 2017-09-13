/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jsprf_h
#define jsprf_h

/*
** API for PR printf like routines. Supports the following formats
**      %d - decimal
**      %u - unsigned decimal
**      %x - unsigned hex
**      %X - unsigned uppercase hex
**      %o - unsigned octal
**      %hd, %hu, %hx, %hX, %ho - 16-bit versions of above
**      %ld, %lu, %lx, %lX, %lo - 32-bit versions of above
**      %lld, %llu, %llx, %llX, %llo - 64 bit versions of above
**      %s - ascii string
**      %hs - ucs2 string
**      %c - character
**      %p - pointer (deals with machine dependent pointer size)
**      %f - float
**      %g - float
*/

#include <stdarg.h>

#include "jstypes.h"

/*
** sprintf into a fixed size buffer. Guarantees that a NUL is at the end
** of the buffer. Returns the length of the written output, NOT including
** the NUL, or (uint32_t)-1 if an error occurs.
*/
extern JS_PUBLIC_API(uint32_t) JS_snprintf(char* out, uint32_t outlen, const char* fmt, ...);

/*
** sprintf into a malloc'd buffer. Return a pointer to the malloc'd
** buffer on success, nullptr on failure. Call "JS_smprintf_free" to release
** the memory returned.
*/
extern JS_PUBLIC_API(char*) JS_smprintf(const char* fmt, ...);

/*
** Free the memory allocated, for the caller, by JS_smprintf
*/
extern JS_PUBLIC_API(void) JS_smprintf_free(char* mem);

/*
** "append" sprintf into a malloc'd buffer. "last" is the last value of
** the malloc'd buffer. sprintf will append data to the end of last,
** growing it as necessary using realloc. If last is nullptr, JS_sprintf_append
** will allocate the initial string. The return value is the new value of
** last for subsequent calls, or nullptr if there is a malloc failure.
*/
extern JS_PUBLIC_API(char*) JS_sprintf_append(char* last, const char* fmt, ...);

/*
** va_list forms of the above.
*/
extern JS_PUBLIC_API(uint32_t) JS_vsnprintf(char* out, uint32_t outlen, const char* fmt, va_list ap);
extern JS_PUBLIC_API(char*) JS_vsmprintf(const char* fmt, va_list ap);
extern JS_PUBLIC_API(char*) JS_vsprintf_append(char* last, const char* fmt, va_list ap);

#endif /* jsprf_h */

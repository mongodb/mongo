/*
 * $Id: printbuf.h,v 1.4 2006/01/26 02:16:28 mclark Exp $
 *
 * Copyright (c) 2004, 2005 Metaparadigm Pte. Ltd.
 * Michael Clark <michael@metaparadigm.com>
 *
 * This library is free software; you can redistribute it and/or modify
 * it under the terms of the MIT license. See COPYING for details.
 *
 *
 * Copyright (c) 2008-2009 Yahoo! Inc.  All rights reserved.
 * The copyrights to the contents of this file are licensed under the MIT License
 * (https://www.opensource.org/licenses/mit-license.php)
 */

/**
 * @file
 * @brief Internal string buffer handling.  Unless you're writing a
 *        json_object_to_json_string_fn implementation for use with
 *        json_object_set_serializer() direct use of this is not
 *        recommended.
 */
#ifndef _json_c_printbuf_h_
#define _json_c_printbuf_h_

#ifndef JSON_EXPORT
#if defined(_MSC_VER) && defined(JSON_C_DLL)
#define JSON_EXPORT __declspec(dllexport)
#else
#define JSON_EXPORT extern
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

struct printbuf
{
	char *buf;
	int bpos;
	int size;
};
typedef struct printbuf printbuf;

JSON_EXPORT struct printbuf *printbuf_new(void);

/* As an optimization, printbuf_memappend_fast() is defined as a macro
 * that handles copying data if the buffer is large enough; otherwise
 * it invokes printbuf_memappend() which performs the heavy
 * lifting of realloc()ing the buffer and copying data.
 *
 * Your code should not use printbuf_memappend() directly unless it
 * checks the return code. Use printbuf_memappend_fast() instead.
 */
JSON_EXPORT int printbuf_memappend(struct printbuf *p, const char *buf, int size);

#define printbuf_memappend_fast(p, bufptr, bufsize)                  \
	do                                                           \
	{                                                            \
		if ((p->size - p->bpos) > bufsize)                   \
		{                                                    \
			memcpy(p->buf + p->bpos, (bufptr), bufsize); \
			p->bpos += bufsize;                          \
			p->buf[p->bpos] = '\0';                      \
		}                                                    \
		else                                                 \
		{                                                    \
			printbuf_memappend(p, (bufptr), bufsize);    \
		}                                                    \
	} while (0)

#define printbuf_length(p) ((p)->bpos)

/**
 * Results in a compile error if the argument is not a string literal.
 */
#define _printbuf_check_literal(mystr) ("" mystr)

/**
 * This is an optimization wrapper around printbuf_memappend() that is useful
 * for appending string literals. Since the size of string constants is known
 * at compile time, using this macro can avoid a costly strlen() call. This is
 * especially helpful when a constant string must be appended many times. If
 * you got here because of a compilation error caused by passing something
 * other than a string literal, use printbuf_memappend_fast() in conjunction
 * with strlen().
 *
 * See also:
 *   printbuf_memappend_fast()
 *   printbuf_memappend()
 *   sprintbuf()
 */
#define printbuf_strappend(pb, str) \
	printbuf_memappend((pb), _printbuf_check_literal(str), sizeof(str) - 1)

/**
 * Set len bytes of the buffer to charvalue, starting at offset offset.
 * Similar to calling memset(x, charvalue, len);
 *
 * The memory allocated for the buffer is extended as necessary.
 *
 * If offset is -1, this starts at the end of the current data in the buffer.
 */
JSON_EXPORT int printbuf_memset(struct printbuf *pb, int offset, int charvalue, int len);

/**
 * Formatted print to printbuf.
 *
 * This function is the most expensive of the available functions for appending
 * string data to a printbuf and should be used only where convenience is more
 * important than speed. Avoid using this function in high performance code or
 * tight loops; in these scenarios, consider using snprintf() with a static
 * buffer in conjunction with one of the printbuf_*append() functions.
 *
 * See also:
 *   printbuf_memappend_fast()
 *   printbuf_memappend()
 *   printbuf_strappend()
 */
JSON_EXPORT int sprintbuf(struct printbuf *p, const char *msg, ...);

JSON_EXPORT void printbuf_reset(struct printbuf *p);

JSON_EXPORT void printbuf_free(struct printbuf *p);

#ifdef __cplusplus
}
#endif

#endif

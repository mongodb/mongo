/*
 * string.c : an XML string utilities module
 *
 * This module provides various utility functions for manipulating
 * the xmlChar* type. All functions named xmlStr* have been moved here
 * from the parser.c file (their original home).
 *
 * See Copyright for the status of this software.
 *
 * UTF8 string routines from: William Brack
 *
 * Author: Daniel Veillard
 */

#define IN_LIBXML
#include "libxml.h"

#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <libxml/xmlmemory.h>
#include <libxml/parserInternals.h>
#include <libxml/xmlstring.h>

#include "private/parser.h"
#include "private/string.h"

#ifndef va_copy
  #ifdef __va_copy
    #define va_copy(dest, src) __va_copy(dest, src)
  #else
    #define va_copy(dest, src) memcpy(&(dest), &(src), sizeof(va_list))
  #endif
#endif

/************************************************************************
 *                                                                      *
 *                Commodity functions to handle xmlChars                *
 *                                                                      *
 ************************************************************************/

/**
 * a strndup for array of xmlChar's
 *
 * @param cur  the input xmlChar *
 * @param len  the len of `cur`
 * @returns a new xmlChar * or NULL
 */
xmlChar *
xmlStrndup(const xmlChar *cur, int len) {
    xmlChar *ret;

    if ((cur == NULL) || (len < 0)) return(NULL);
    ret = xmlMalloc((size_t) len + 1);
    if (ret == NULL) {
        return(NULL);
    }
    memcpy(ret, cur, len);
    ret[len] = 0;
    return(ret);
}

/**
 * a strdup for array of xmlChar's. Since they are supposed to be
 * encoded in UTF-8 or an encoding with 8bit based chars, we assume
 * a termination mark of '0'.
 *
 * @param cur  the input xmlChar *
 * @returns a new xmlChar * or NULL
 */
xmlChar *
xmlStrdup(const xmlChar *cur) {
    const xmlChar *p = cur;

    if (cur == NULL) return(NULL);
    while (*p != 0) p++; /* non input consuming */
    return(xmlStrndup(cur, p - cur));
}

/**
 * a strndup for char's to xmlChar's
 *
 * @param cur  the input char *
 * @param len  the len of `cur`
 * @returns a new xmlChar * or NULL
 */

xmlChar *
xmlCharStrndup(const char *cur, int len) {
    int i;
    xmlChar *ret;

    if ((cur == NULL) || (len < 0)) return(NULL);
    ret = xmlMalloc((size_t) len + 1);
    if (ret == NULL) {
        return(NULL);
    }
    for (i = 0;i < len;i++) {
        /* Explicit sign change */
        ret[i] = (xmlChar) cur[i];
        if (ret[i] == 0) return(ret);
    }
    ret[len] = 0;
    return(ret);
}

/**
 * a strdup for char's to xmlChar's
 *
 * @param cur  the input char *
 * @returns a new xmlChar * or NULL
 */

xmlChar *
xmlCharStrdup(const char *cur) {
    const char *p = cur;

    if (cur == NULL) return(NULL);
    while (*p != '\0') p++; /* non input consuming */
    return(xmlCharStrndup(cur, p - cur));
}

/**
 * a strcmp for xmlChar's
 *
 * @param str1  the first xmlChar *
 * @param str2  the second xmlChar *
 * @returns the integer result of the comparison
 */

int
xmlStrcmp(const xmlChar *str1, const xmlChar *str2) {
    if (str1 == str2) return(0);
    if (str1 == NULL) return(-1);
    if (str2 == NULL) return(1);
#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
    return(strcmp((const char *)str1, (const char *)str2));
#else
    do {
        int tmp = *str1++ - *str2;
        if (tmp != 0) return(tmp);
    } while (*str2++ != 0);
    return 0;
#endif
}

/**
 * Check if both strings are equal of have same content.
 * Should be a bit more readable and faster than #xmlStrcmp
 *
 * @param str1  the first xmlChar *
 * @param str2  the second xmlChar *
 * @returns 1 if they are equal, 0 if they are different
 */

int
xmlStrEqual(const xmlChar *str1, const xmlChar *str2) {
    if (str1 == str2) return(1);
    if (str1 == NULL) return(0);
    if (str2 == NULL) return(0);
#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
    return(strcmp((const char *)str1, (const char *)str2) == 0);
#else
    do {
        if (*str1++ != *str2) return(0);
    } while (*str2++);
    return(1);
#endif
}

/**
 * Check if a QName is Equal to a given string
 *
 * @param pref  the prefix of the QName
 * @param name  the localname of the QName
 * @param str  the second xmlChar *
 * @returns 1 if they are equal, 0 if they are different
 */

int
xmlStrQEqual(const xmlChar *pref, const xmlChar *name, const xmlChar *str) {
    if (pref == NULL) return(xmlStrEqual(name, str));
    if (name == NULL) return(0);
    if (str == NULL) return(0);

    do {
        if (*pref++ != *str) return(0);
    } while ((*str++) && (*pref));
    if (*str++ != ':') return(0);
    do {
        if (*name++ != *str) return(0);
    } while (*str++);
    return(1);
}

/**
 * a strncmp for xmlChar's
 *
 * @param str1  the first xmlChar *
 * @param str2  the second xmlChar *
 * @param len  the max comparison length
 * @returns the integer result of the comparison
 */

int
xmlStrncmp(const xmlChar *str1, const xmlChar *str2, int len) {
    if (len <= 0) return(0);
    if (str1 == str2) return(0);
    if (str1 == NULL) return(-1);
    if (str2 == NULL) return(1);
#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
    return(strncmp((const char *)str1, (const char *)str2, len));
#else
    do {
        int tmp = *str1++ - *str2;
        if (tmp != 0 || --len == 0) return(tmp);
    } while (*str2++ != 0);
    return 0;
#endif
}

static const xmlChar casemap[256] = {
    0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
    0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F,
    0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,
    0x18,0x19,0x1A,0x1B,0x1C,0x1D,0x1E,0x1F,
    0x20,0x21,0x22,0x23,0x24,0x25,0x26,0x27,
    0x28,0x29,0x2A,0x2B,0x2C,0x2D,0x2E,0x2F,
    0x30,0x31,0x32,0x33,0x34,0x35,0x36,0x37,
    0x38,0x39,0x3A,0x3B,0x3C,0x3D,0x3E,0x3F,
    0x40,0x61,0x62,0x63,0x64,0x65,0x66,0x67,
    0x68,0x69,0x6A,0x6B,0x6C,0x6D,0x6E,0x6F,
    0x70,0x71,0x72,0x73,0x74,0x75,0x76,0x77,
    0x78,0x79,0x7A,0x7B,0x5C,0x5D,0x5E,0x5F,
    0x60,0x61,0x62,0x63,0x64,0x65,0x66,0x67,
    0x68,0x69,0x6A,0x6B,0x6C,0x6D,0x6E,0x6F,
    0x70,0x71,0x72,0x73,0x74,0x75,0x76,0x77,
    0x78,0x79,0x7A,0x7B,0x7C,0x7D,0x7E,0x7F,
    0x80,0x81,0x82,0x83,0x84,0x85,0x86,0x87,
    0x88,0x89,0x8A,0x8B,0x8C,0x8D,0x8E,0x8F,
    0x90,0x91,0x92,0x93,0x94,0x95,0x96,0x97,
    0x98,0x99,0x9A,0x9B,0x9C,0x9D,0x9E,0x9F,
    0xA0,0xA1,0xA2,0xA3,0xA4,0xA5,0xA6,0xA7,
    0xA8,0xA9,0xAA,0xAB,0xAC,0xAD,0xAE,0xAF,
    0xB0,0xB1,0xB2,0xB3,0xB4,0xB5,0xB6,0xB7,
    0xB8,0xB9,0xBA,0xBB,0xBC,0xBD,0xBE,0xBF,
    0xC0,0xC1,0xC2,0xC3,0xC4,0xC5,0xC6,0xC7,
    0xC8,0xC9,0xCA,0xCB,0xCC,0xCD,0xCE,0xCF,
    0xD0,0xD1,0xD2,0xD3,0xD4,0xD5,0xD6,0xD7,
    0xD8,0xD9,0xDA,0xDB,0xDC,0xDD,0xDE,0xDF,
    0xE0,0xE1,0xE2,0xE3,0xE4,0xE5,0xE6,0xE7,
    0xE8,0xE9,0xEA,0xEB,0xEC,0xED,0xEE,0xEF,
    0xF0,0xF1,0xF2,0xF3,0xF4,0xF5,0xF6,0xF7,
    0xF8,0xF9,0xFA,0xFB,0xFC,0xFD,0xFE,0xFF
};

/**
 * a strcasecmp for xmlChar's
 *
 * @param str1  the first xmlChar *
 * @param str2  the second xmlChar *
 * @returns the integer result of the comparison
 */

int
xmlStrcasecmp(const xmlChar *str1, const xmlChar *str2) {
    register int tmp;

    if (str1 == str2) return(0);
    if (str1 == NULL) return(-1);
    if (str2 == NULL) return(1);
    do {
        tmp = casemap[*str1++] - casemap[*str2];
        if (tmp != 0) return(tmp);
    } while (*str2++ != 0);
    return 0;
}

/**
 * a strncasecmp for xmlChar's
 *
 * @param str1  the first xmlChar *
 * @param str2  the second xmlChar *
 * @param len  the max comparison length
 * @returns the integer result of the comparison
 */

int
xmlStrncasecmp(const xmlChar *str1, const xmlChar *str2, int len) {
    register int tmp;

    if (len <= 0) return(0);
    if (str1 == str2) return(0);
    if (str1 == NULL) return(-1);
    if (str2 == NULL) return(1);
    do {
        tmp = casemap[*str1++] - casemap[*str2];
        if (tmp != 0 || --len == 0) return(tmp);
    } while (*str2++ != 0);
    return 0;
}

/**
 * a strchr for xmlChar's
 *
 * @param str  the xmlChar * array
 * @param val  the xmlChar to search
 * @returns the xmlChar * for the first occurrence or NULL.
 */

const xmlChar *
xmlStrchr(const xmlChar *str, xmlChar val) {
    if (str == NULL) return(NULL);
    while (*str != 0) { /* non input consuming */
        if (*str == val) return((xmlChar *) str);
        str++;
    }
    return(NULL);
}

/**
 * a strstr for xmlChar's
 *
 * @param str  the xmlChar * array (haystack)
 * @param val  the xmlChar to search (needle)
 * @returns the xmlChar * for the first occurrence or NULL.
 */

const xmlChar *
xmlStrstr(const xmlChar *str, const xmlChar *val) {
    int n;

    if (str == NULL) return(NULL);
    if (val == NULL) return(NULL);
    n = xmlStrlen(val);

    if (n == 0) return(str);
    while (*str != 0) { /* non input consuming */
        if (*str == *val) {
            if (!xmlStrncmp(str, val, n)) return((const xmlChar *) str);
        }
        str++;
    }
    return(NULL);
}

/**
 * a case-ignoring strstr for xmlChar's
 *
 * @param str  the xmlChar * array (haystack)
 * @param val  the xmlChar to search (needle)
 * @returns the xmlChar * for the first occurrence or NULL.
 */

const xmlChar *
xmlStrcasestr(const xmlChar *str, const xmlChar *val) {
    int n;

    if (str == NULL) return(NULL);
    if (val == NULL) return(NULL);
    n = xmlStrlen(val);

    if (n == 0) return(str);
    while (*str != 0) { /* non input consuming */
        if (casemap[*str] == casemap[*val])
            if (!xmlStrncasecmp(str, val, n)) return(str);
        str++;
    }
    return(NULL);
}

/**
 * Extract a substring of a given string
 *
 * @param str  the xmlChar * array (haystack)
 * @param start  the index of the first char (zero based)
 * @param len  the length of the substring
 * @returns the xmlChar * for the first occurrence or NULL.
 */

xmlChar *
xmlStrsub(const xmlChar *str, int start, int len) {
    int i;

    if (str == NULL) return(NULL);
    if (start < 0) return(NULL);
    if (len < 0) return(NULL);

    for (i = 0;i < start;i++) {
        if (*str == 0) return(NULL);
        str++;
    }
    if (*str == 0) return(NULL);
    return(xmlStrndup(str, len));
}

/**
 * length of a xmlChar's string
 *
 * @param str  the xmlChar * array
 * @returns the number of xmlChar contained in the ARRAY.
 */

int
xmlStrlen(const xmlChar *str) {
    size_t len = str ? strlen((const char *)str) : 0;
    return(len > INT_MAX ? 0 : len);
}

/**
 * a strncat for array of xmlChar's, it will extend `cur` with the len
 * first bytes of `add`. Note that if `len` < 0 then this is an API error
 * and NULL will be returned.
 *
 * @param cur  the original xmlChar * array
 * @param add  the xmlChar * array added
 * @param len  the length of `add`
 * @returns a new xmlChar *, the original `cur` is reallocated and should
 * not be freed.
 */

xmlChar *
xmlStrncat(xmlChar *cur, const xmlChar *add, int len) {
    int size;
    xmlChar *ret;

    if ((add == NULL) || (len == 0))
        return(cur);
    if (len < 0)
	return(NULL);
    if (cur == NULL)
        return(xmlStrndup(add, len));

    size = xmlStrlen(cur);
    if ((size < 0) || (size > INT_MAX - len))
        return(NULL);
    ret = (xmlChar *) xmlRealloc(cur, (size_t) size + len + 1);
    if (ret == NULL) {
        xmlFree(cur);
        return(NULL);
    }
    memcpy(&ret[size], add, len);
    ret[size + len] = 0;
    return(ret);
}

/**
 * same as #xmlStrncat, but creates a new string.  The original
 * two strings are not freed. If `len` is < 0 then the length
 * will be calculated automatically.
 *
 * @param str1  first xmlChar string
 * @param str2  second xmlChar string
 * @param len  the len of `str2` or < 0
 * @returns a new xmlChar * or NULL
 */
xmlChar *
xmlStrncatNew(const xmlChar *str1, const xmlChar *str2, int len) {
    int size;
    xmlChar *ret;

    if (len < 0) {
        len = xmlStrlen(str2);
        if (len < 0)
            return(NULL);
    }
    if (str1 == NULL)
        return(xmlStrndup(str2, len));
    if ((str2 == NULL) || (len == 0))
        return(xmlStrdup(str1));

    size = xmlStrlen(str1);
    if ((size < 0) || (size > INT_MAX - len))
        return(NULL);
    ret = (xmlChar *) xmlMalloc((size_t) size + len + 1);
    if (ret == NULL)
        return(NULL);
    memcpy(ret, str1, size);
    memcpy(&ret[size], str2, len);
    ret[size + len] = 0;
    return(ret);
}

/**
 * a strcat for array of xmlChar's. Since they are supposed to be
 * encoded in UTF-8 or an encoding with 8bit based chars, we assume
 * a termination mark of '0'.
 *
 * @param cur  the original xmlChar * array
 * @param add  the xmlChar * array added
 * @returns a new xmlChar * containing the concatenated string. The original
 * `cur` is reallocated and should not be freed.
 */
xmlChar *
xmlStrcat(xmlChar *cur, const xmlChar *add) {
    const xmlChar *p = add;

    if (add == NULL) return(cur);
    if (cur == NULL)
        return(xmlStrdup(add));

    while (*p != 0) p++; /* non input consuming */
    return(xmlStrncat(cur, add, p - add));
}

/**
 * Formats `msg` and places result into `buf`.
 *
 * @param buf  the result buffer.
 * @param len  the result buffer length.
 * @param msg  the message with printf formatting.
 * @param ...   extra parameters for the message.
 * @returns the number of characters written to `buf` or -1 if an error occurs.
 */
int
xmlStrPrintf(xmlChar *buf, int len, const char *msg, ...) {
    va_list args;
    int ret;

    if((buf == NULL) || (msg == NULL)) {
        return(-1);
    }

    va_start(args, msg);
    ret = vsnprintf((char *) buf, len, (const char *) msg, args);
    va_end(args);
    buf[len - 1] = 0; /* be safe ! */

    return(ret);
}

/**
 * Formats `msg` and places result into `buf`.
 *
 * @param buf  the result buffer.
 * @param len  the result buffer length.
 * @param msg  the message with printf formatting.
 * @param ap  extra parameters for the message.
 * @returns the number of characters written to `buf` or -1 if an error occurs.
 */
int
xmlStrVPrintf(xmlChar *buf, int len, const char *msg, va_list ap) {
    int ret;

    if((buf == NULL) || (msg == NULL)) {
        return(-1);
    }

    ret = vsnprintf((char *) buf, len, (const char *) msg, ap);
    buf[len - 1] = 0; /* be safe ! */

    return(ret);
}

/**
 * Creates a newly allocated string according to format.
 *
 * @param out  pointer to the resulting string
 * @param maxSize  maximum size of the output buffer
 * @param msg  printf format string
 * @param ap  arguments for format string
 * @returns 0 on success, 1 if the result was truncated or on other
 * errors, -1 if a memory allocation failed.
 */
int
xmlStrVASPrintf(xmlChar **out, int maxSize, const char *msg, va_list ap) {
    char empty[1];
    va_list copy;
    xmlChar *buf;
    int res, size;
    int truncated = 0;

    if (out == NULL)
        return(1);
    *out = NULL;
    if (msg == NULL)
        return(1);
    if (maxSize < 32)
        maxSize = 32;

    va_copy(copy, ap);
    res = vsnprintf(empty, 1, msg, copy);
    va_end(copy);

    if (res > 0) {
        /* snprintf seems to work according to C99. */

        if (res < maxSize) {
            size = res + 1;
        } else {
            size = maxSize;
            truncated = 1;
        }
        buf = xmlMalloc(size);
        if (buf == NULL)
            return(-1);
        if (vsnprintf((char *) buf, size, msg, ap) < 0) {
            xmlFree(buf);
            return(1);
        }
    } else {
        /*
         * Unfortunately, older snprintf implementations don't follow the
         * C99 spec. If the output exceeds the size of the buffer, they can
         * return -1, 0 or the number of characters written instead of the
         * needed size. Older MSCVRT also won't write a terminating null
         * byte if the buffer is too small.
         *
         * If the value returned is non-negative and strictly less than
         * the buffer size (without terminating null), the result should
         * have been written completely, so we double the buffer size
         * until this condition is true. This assumes that snprintf will
         * eventually return a non-negative value. Otherwise, we will
         * allocate more and more memory until we run out.
         *
         * Note that this code path is also executed on conforming
         * platforms if the output is the empty string.
         */

        buf = NULL;
        size = 32;
        while (1) {
            buf = xmlMalloc(size);
            if (buf == NULL)
                return(-1);

            va_copy(copy, ap);
            res = vsnprintf((char *) buf, size, msg, copy);
            va_end(copy);
            if ((res >= 0) && (res < size - 1))
                break;

            if (size >= maxSize) {
                truncated = 1;
                break;
            }

            xmlFree(buf);

            if (size > maxSize / 2)
                size = maxSize;
            else
                size *= 2;
        }
    }

    /*
     * If the output was truncated, make sure that the buffer doesn't
     * end with a truncated UTF-8 sequence.
     */
    if (truncated != 0) {
        int i = size - 1;

        while (i > 0) {
            /* Break after ASCII */
            if (buf[i-1] < 0x80)
                break;
            i -= 1;
            /* Break before non-ASCII */
            if (buf[i] >= 0xc0)
                break;
        }

        buf[i] = 0;
    }

    *out = (xmlChar *) buf;
    return(truncated);
}

/**
 * See xmlStrVASPrintf.
 *
 * @param out  pointer to the resulting string
 * @param maxSize  maximum size of the output buffer
 * @param msg  printf format string
 * @param ...  arguments for format string
 * @returns 0 on success, 1 if the result was truncated or on other
 * errors, -1 if a memory allocation failed.
 */
int
xmlStrASPrintf(xmlChar **out, int maxSize, const char *msg, ...) {
    va_list ap;
    int ret;

    va_start(ap, msg);
    ret = xmlStrVASPrintf(out, maxSize, msg, ap);
    va_end(ap);

    return(ret);
}

/************************************************************************
 *                                                                      *
 *              Generic UTF8 handling routines                          *
 *                                                                      *
 * From rfc2044: encoding of the Unicode values on UTF-8:               *
 *                                                                      *
 * UCS-4 range (hex.)           UTF-8 octet sequence (binary)           *
 * 0000 0000-0000 007F   0xxxxxxx                                       *
 * 0000 0080-0000 07FF   110xxxxx 10xxxxxx                              *
 * 0000 0800-0000 FFFF   1110xxxx 10xxxxxx 10xxxxxx                     *
 *                                                                      *
 * I hope we won't use values > 0xFFFF anytime soon !                   *
 *                                                                      *
 ************************************************************************/


/**
 * calculates the internal size of a UTF8 character
 *
 * @param utf  pointer to the UTF8 character
 * @returns the numbers of bytes in the character, -1 on format error
 */
int
xmlUTF8Size(const xmlChar *utf) {
    xmlChar mask;
    int len;

    if (utf == NULL)
        return -1;
    if (*utf < 0x80)
        return 1;
    /* check valid UTF8 character */
    if (!(*utf & 0x40))
        return -1;
    /* determine number of bytes in char */
    len = 2;
    for (mask=0x20; mask != 0; mask>>=1) {
        if (!(*utf & mask))
            return len;
        len++;
    }
    return -1;
}

/**
 * compares the two UCS4 values
 *
 * @param utf1  pointer to first UTF8 char
 * @param utf2  pointer to second UTF8 char
 * @returns result of the compare as with #xmlStrncmp
 */
int
xmlUTF8Charcmp(const xmlChar *utf1, const xmlChar *utf2) {

    if (utf1 == NULL ) {
        if (utf2 == NULL)
            return 0;
        return -1;
    }
    return xmlStrncmp(utf1, utf2, xmlUTF8Size(utf1));
}

/**
 * compute the length of an UTF8 string, it doesn't do a full UTF8
 * checking of the content of the string.
 *
 * @param utf  a sequence of UTF-8 encoded bytes
 * @returns the number of characters in the string or -1 in case of error
 */
int
xmlUTF8Strlen(const xmlChar *utf) {
    size_t ret = 0;

    if (utf == NULL)
        return(-1);

    while (*utf != 0) {
        if (utf[0] & 0x80) {
            if ((utf[1] & 0xc0) != 0x80)
                return(-1);
            if ((utf[0] & 0xe0) == 0xe0) {
                if ((utf[2] & 0xc0) != 0x80)
                    return(-1);
                if ((utf[0] & 0xf0) == 0xf0) {
                    if ((utf[0] & 0xf8) != 0xf0 || (utf[3] & 0xc0) != 0x80)
                        return(-1);
                    utf += 4;
                } else {
                    utf += 3;
                }
            } else {
                utf += 2;
            }
        } else {
            utf++;
        }
        ret++;
    }
    return(ret > INT_MAX ? 0 : ret);
}

/**
 * Read the first UTF8 character from `utf`
 *
 * @param utf  a sequence of UTF-8 encoded bytes
 * @param len  a pointer to the minimum number of bytes present in
 *        the sequence.  This is used to assure the next character
 *        is completely contained within the sequence.
 * @returns the char value or -1 in case of error, and sets *len to
 *        the actual number of bytes consumed (0 in case of error)
 */
int
xmlGetUTF8Char(const unsigned char *utf, int *len) {
    unsigned int c;

    if (utf == NULL)
        goto error;
    if (len == NULL)
        goto error;

    c = utf[0];
    if (c < 0x80) {
        if (*len < 1)
            goto error;
        /* 1-byte code */
        *len = 1;
    } else {
        if ((*len < 2) || ((utf[1] & 0xc0) != 0x80))
            goto error;
        if (c < 0xe0) {
            if (c < 0xc2)
                goto error;
            /* 2-byte code */
            *len = 2;
            c = (c & 0x1f) << 6;
            c |= utf[1] & 0x3f;
        } else {
            if ((*len < 3) || ((utf[2] & 0xc0) != 0x80))
                goto error;
            if (c < 0xf0) {
                /* 3-byte code */
                *len = 3;
                c = (c & 0xf) << 12;
                c |= (utf[1] & 0x3f) << 6;
                c |= utf[2] & 0x3f;
                if ((c < 0x800) || ((c >= 0xd800) && (c < 0xe000)))
                    goto error;
            } else {
                if ((*len < 4) || ((utf[3] & 0xc0) != 0x80))
                    goto error;
                *len = 4;
                /* 4-byte code */
                c = (c & 0x7) << 18;
                c |= (utf[1] & 0x3f) << 12;
                c |= (utf[2] & 0x3f) << 6;
                c |= utf[3] & 0x3f;
                if ((c < 0x10000) || (c >= 0x110000))
                    goto error;
            }
        }
    }
    return(c);

error:
    if (len != NULL)
	*len = 0;
    return(-1);
}

/**
 * Checks `utf` for being valid UTF-8. `utf` is assumed to be
 * null-terminated. This function is not super-strict, as it will
 * allow longer UTF-8 sequences than necessary. Note that Java is
 * capable of producing these sequences if provoked. Also note, this
 * routine checks for the 4-byte maximum size, but does not check for
 * 0x10ffff maximum value.
 *
 * @param utf  Pointer to putative UTF-8 encoded string.
 * @returns value: true if `utf` is valid.
 **/
int
xmlCheckUTF8(const unsigned char *utf)
{
    int ix;
    unsigned char c;

    if (utf == NULL)
        return(0);
    /*
     * utf is a string of 1, 2, 3 or 4 bytes.  The valid strings
     * are as follows (in "bit format"):
     *    0xxxxxxx                                      valid 1-byte
     *    110xxxxx 10xxxxxx                             valid 2-byte
     *    1110xxxx 10xxxxxx 10xxxxxx                    valid 3-byte
     *    11110xxx 10xxxxxx 10xxxxxx 10xxxxxx           valid 4-byte
     */
    while ((c = utf[0])) {      /* string is 0-terminated */
        ix = 0;
        if ((c & 0x80) == 0x00) {	/* 1-byte code, starts with 10 */
            ix = 1;
	} else if ((c & 0xe0) == 0xc0) {/* 2-byte code, starts with 110 */
	    if ((utf[1] & 0xc0 ) != 0x80)
	        return 0;
	    ix = 2;
	} else if ((c & 0xf0) == 0xe0) {/* 3-byte code, starts with 1110 */
	    if (((utf[1] & 0xc0) != 0x80) ||
	        ((utf[2] & 0xc0) != 0x80))
		    return 0;
	    ix = 3;
	} else if ((c & 0xf8) == 0xf0) {/* 4-byte code, starts with 11110 */
	    if (((utf[1] & 0xc0) != 0x80) ||
	        ((utf[2] & 0xc0) != 0x80) ||
		((utf[3] & 0xc0) != 0x80))
		    return 0;
	    ix = 4;
	} else				/* unknown encoding */
	    return 0;
        utf += ix;
      }
      return(1);
}

/**
 * storage size of an UTF8 string
 * the behaviour is not guaranteed if the input string is not UTF-8
 *
 * @param utf  a sequence of UTF-8 encoded bytes
 * @param len  the number of characters in the array
 * @returns the storage size of
 * the first 'len' characters of ARRAY
 */

int
xmlUTF8Strsize(const xmlChar *utf, int len) {
    const xmlChar *ptr=utf;
    int ch;
    size_t ret;

    if (utf == NULL)
        return(0);

    if (len <= 0)
        return(0);

    while ( len-- > 0) {
        if ( !*ptr )
            break;
        ch = *ptr++;
        if ((ch & 0x80))
            while ((ch<<=1) & 0x80 ) {
		if (*ptr == 0) break;
                ptr++;
	    }
    }
    ret = ptr - utf;
    return (ret > INT_MAX ? 0 : ret);
}


/**
 * a strndup for array of UTF8's
 *
 * @param utf  the input UTF8 *
 * @param len  the len of `utf` (in chars)
 * @returns a new UTF8 * or NULL
 */
xmlChar *
xmlUTF8Strndup(const xmlChar *utf, int len) {
    xmlChar *ret;
    int i;

    if ((utf == NULL) || (len < 0)) return(NULL);
    i = xmlUTF8Strsize(utf, len);
    ret = xmlMalloc((size_t) i + 1);
    if (ret == NULL) {
        return(NULL);
    }
    memcpy(ret, utf, i);
    ret[i] = 0;
    return(ret);
}

/**
 * a function to provide the equivalent of fetching a
 * character from a string array
 *
 * @param utf  the input UTF8 *
 * @param pos  the position of the desired UTF8 char (in chars)
 * @returns a pointer to the UTF8 character or NULL
 */
const xmlChar *
xmlUTF8Strpos(const xmlChar *utf, int pos) {
    int ch;

    if (utf == NULL) return(NULL);
    if (pos < 0)
        return(NULL);
    while (pos--) {
        ch = *utf++;
        if (ch == 0)
            return(NULL);
        if ( ch & 0x80 ) {
            /* if not simple ascii, verify proper format */
            if ( (ch & 0xc0) != 0xc0 )
                return(NULL);
            /* then skip over remaining bytes for this char */
            while ( (ch <<= 1) & 0x80 )
                if ( (*utf++ & 0xc0) != 0x80 )
                    return(NULL);
        }
    }
    return((xmlChar *)utf);
}

/**
 * a function to provide the relative location of a UTF8 char
 *
 * @param utf  the input UTF8 *
 * @param utfchar  the UTF8 character to be found
 * @returns the relative character position of the desired char
 * or -1 if not found
 */
int
xmlUTF8Strloc(const xmlChar *utf, const xmlChar *utfchar) {
    size_t i;
    int size;
    int ch;

    if (utf==NULL || utfchar==NULL) return -1;
    size = xmlUTF8Strsize(utfchar, 1);
        for(i=0; (ch=*utf) != 0; i++) {
            if (xmlStrncmp(utf, utfchar, size)==0)
                return(i > INT_MAX ? 0 : i);
            utf++;
            if ( ch & 0x80 ) {
                /* if not simple ascii, verify proper format */
                if ( (ch & 0xc0) != 0xc0 )
                    return(-1);
                /* then skip over remaining bytes for this char */
                while ( (ch <<= 1) & 0x80 )
                    if ( (*utf++ & 0xc0) != 0x80 )
                        return(-1);
            }
        }

    return(-1);
}
/**
 * Create a substring from a given UTF-8 string
 * Note:  positions are given in units of UTF-8 chars
 *
 * @param utf  a sequence of UTF-8 encoded bytes
 * @param start  relative pos of first char
 * @param len  total number to copy
 * @returns a pointer to a newly created string or NULL if the
 * start index is out of bounds or a memory allocation failed.
 * If len is too large, the result is truncated.
 */

xmlChar *
xmlUTF8Strsub(const xmlChar *utf, int start, int len) {
    int i;
    int ch;

    if (utf == NULL) return(NULL);
    if (start < 0) return(NULL);
    if (len < 0) return(NULL);

    /*
     * Skip over any leading chars
     */
    for (i = 0; i < start; i++) {
        ch = *utf++;
        if (ch == 0)
            return(NULL);
        /* skip over remaining bytes for this char */
        if (ch & 0x80) {
            ch <<= 1;
            while (ch & 0x80) {
                if (*utf++ == 0)
                    return(NULL);
                ch <<= 1;
            }
        }
    }

    return(xmlUTF8Strndup(utf, len));
}

/**
 * Replaces a string with an escaped string.
 *
 * `msg` must be a heap-allocated buffer created by libxml2 that may be
 * returned, or that may be freed and replaced.
 *
 * @param msg  a pointer to the string in which to escape '%' characters.
 * @returns the same string with all '%' characters escaped.
 */
xmlChar *
xmlEscapeFormatString(xmlChar **msg)
{
    xmlChar *msgPtr = NULL;
    xmlChar *result = NULL;
    xmlChar *resultPtr = NULL;
    size_t count = 0;
    size_t msgLen = 0;
    size_t resultLen = 0;

    if (!msg || !*msg)
        return(NULL);

    for (msgPtr = *msg; *msgPtr != '\0'; ++msgPtr) {
        ++msgLen;
        if (*msgPtr == '%')
            ++count;
    }

    if (count == 0)
        return(*msg);

    if ((count > INT_MAX) || (msgLen > INT_MAX - count))
        return(NULL);
    resultLen = msgLen + count + 1;
    result = xmlMalloc(resultLen);
    if (result == NULL) {
        /* Clear *msg to prevent format string vulnerabilities in
           out-of-memory situations. */
        xmlFree(*msg);
        *msg = NULL;
        return(NULL);
    }

    for (msgPtr = *msg, resultPtr = result; *msgPtr != '\0'; ++msgPtr, ++resultPtr) {
        *resultPtr = *msgPtr;
        if (*msgPtr == '%')
            *(++resultPtr) = '%';
    }
    result[resultLen - 1] = '\0';

    xmlFree(*msg);
    *msg = result;

    return *msg;
}


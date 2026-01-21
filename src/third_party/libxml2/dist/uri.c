/**
 * uri.c: set of generic URI related routines
 *
 * Reference: RFCs 3986, 2732 and 2373
 *
 * See Copyright for the status of this software.
 *
 * Author: Daniel Veillard
 */

#define IN_LIBXML
#include "libxml.h"

#include <limits.h>
#include <string.h>

#include <libxml/xmlmemory.h>
#include <libxml/uri.h>
#include <libxml/xmlerror.h>

#include "private/error.h"
#include "private/memory.h"

/**
 * The definition of the URI regexp in the above RFC has no size limit
 * In practice they are usually relatively short except for the
 * data URI scheme as defined in RFC 2397. Even for data URI the usual
 * maximum size before hitting random practical limits is around 64 KB
 * and 4KB is usually a maximum admitted limit for proper operations.
 * The value below is more a security limit than anything else and
 * really should never be hit by 'normal' operations
 * Set to 1 MByte in 2012, this is only enforced on output
 */
#define MAX_URI_LENGTH 1024 * 1024

#define PORT_EMPTY           0
#define PORT_EMPTY_SERVER   -1

static void xmlCleanURI(xmlURIPtr uri);

/*
 * Old rule from 2396 used in legacy handling code
 * alpha    = lowalpha | upalpha
 */
#define IS_ALPHA(x) (IS_LOWALPHA(x) || IS_UPALPHA(x))


/*
 * lowalpha = "a" | "b" | "c" | "d" | "e" | "f" | "g" | "h" | "i" | "j" |
 *            "k" | "l" | "m" | "n" | "o" | "p" | "q" | "r" | "s" | "t" |
 *            "u" | "v" | "w" | "x" | "y" | "z"
 */
#define IS_LOWALPHA(x) (((x) >= 'a') && ((x) <= 'z'))

/*
 * upalpha = "A" | "B" | "C" | "D" | "E" | "F" | "G" | "H" | "I" | "J" |
 *           "K" | "L" | "M" | "N" | "O" | "P" | "Q" | "R" | "S" | "T" |
 *           "U" | "V" | "W" | "X" | "Y" | "Z"
 */
#define IS_UPALPHA(x) (((x) >= 'A') && ((x) <= 'Z'))

#ifdef IS_DIGIT
#undef IS_DIGIT
#endif
/*
 * digit = "0" | "1" | "2" | "3" | "4" | "5" | "6" | "7" | "8" | "9"
 */
#define IS_DIGIT(x) (((x) >= '0') && ((x) <= '9'))

/*
 * alphanum = alpha | digit
 */
#define IS_ALPHANUM(x) (IS_ALPHA(x) || IS_DIGIT(x))

/*
 * mark = "-" | "_" | "." | "!" | "~" | "*" | "'" | "(" | ")"
 */

#define IS_MARK(x) (((x) == '-') || ((x) == '_') || ((x) == '.') ||     \
    ((x) == '!') || ((x) == '~') || ((x) == '*') || ((x) == '\'') ||    \
    ((x) == '(') || ((x) == ')'))

/*
 * unwise = "{" | "}" | "|" | "\" | "^" | "`"
 */
#define IS_UNWISE(p)                                                    \
      (((*(p) == '{')) || ((*(p) == '}')) || ((*(p) == '|')) ||         \
       ((*(p) == '\\')) || ((*(p) == '^')) || ((*(p) == '[')) ||        \
       ((*(p) == ']')) || ((*(p) == '`')))

/*
 * reserved = ";" | "/" | "?" | ":" | "@" | "&" | "=" | "+" | "$" | "," |
 *            "[" | "]"
 */
#define IS_RESERVED(x) (((x) == ';') || ((x) == '/') || ((x) == '?') || \
        ((x) == ':') || ((x) == '@') || ((x) == '&') || ((x) == '=') || \
        ((x) == '+') || ((x) == '$') || ((x) == ',') || ((x) == '[') || \
        ((x) == ']'))

/*
 * unreserved = alphanum | mark
 */
#define IS_UNRESERVED(x) (IS_ALPHANUM(x) || IS_MARK(x))

/*
 * Skip to next pointer char, handle escaped sequences
 */
#define NEXT(p) ((*p == '%')? p += 3 : p++)

/*
 * Productions from the spec.
 *
 *    authority     = server | reg_name
 *    reg_name      = 1*( unreserved | escaped | "$" | "," |
 *                        ";" | ":" | "@" | "&" | "=" | "+" )
 *
 * path          = [ abs_path | opaque_part ]
 */
#define STRNDUP(s, n) (char *) xmlStrndup((const xmlChar *)(s), (n))

/************************************************************************
 *									*
 *                         RFC 3986 parser				*
 *									*
 ************************************************************************/

#define ISA_DIGIT(p) ((*(p) >= '0') && (*(p) <= '9'))
#define ISA_ALPHA(p) (((*(p) >= 'a') && (*(p) <= 'z')) ||		\
                      ((*(p) >= 'A') && (*(p) <= 'Z')))
#define ISA_HEXDIG(p)							\
       (ISA_DIGIT(p) || ((*(p) >= 'a') && (*(p) <= 'f')) ||		\
        ((*(p) >= 'A') && (*(p) <= 'F')))

/*
 *    sub-delims    = "!" / "$" / "&" / "'" / "(" / ")"
 *                     / "*" / "+" / "," / ";" / "="
 */
#define ISA_SUB_DELIM(p)						\
      (((*(p) == '!')) || ((*(p) == '$')) || ((*(p) == '&')) ||		\
       ((*(p) == '(')) || ((*(p) == ')')) || ((*(p) == '*')) ||		\
       ((*(p) == '+')) || ((*(p) == ',')) || ((*(p) == ';')) ||		\
       ((*(p) == '=')) || ((*(p) == '\'')))

/*
 *    gen-delims    = ":" / "/" / "?" / "\#" / "[" / "]" / "@"
 */
#define ISA_GEN_DELIM(p)						\
      (((*(p) == ':')) || ((*(p) == '/')) || ((*(p) == '?')) ||         \
       ((*(p) == '#')) || ((*(p) == '[')) || ((*(p) == ']')) ||         \
       ((*(p) == '@')))

/*
 *    reserved      = gen-delims / sub-delims
 */
#define ISA_RESERVED(p) (ISA_GEN_DELIM(p) || (ISA_SUB_DELIM(p)))

/*
 *    unreserved    = ALPHA / DIGIT / "-" / "." / "_" / "~"
 */
#define ISA_STRICTLY_UNRESERVED(p)					\
      ((ISA_ALPHA(p)) || (ISA_DIGIT(p)) || ((*(p) == '-')) ||		\
       ((*(p) == '.')) || ((*(p) == '_')) || ((*(p) == '~')))

/*
 *    pct-encoded   = "%" HEXDIG HEXDIG
 */
#define ISA_PCT_ENCODED(p)						\
     ((*(p) == '%') && (ISA_HEXDIG(p + 1)) && (ISA_HEXDIG(p + 2)))

/*
 *    pchar         = unreserved / pct-encoded / sub-delims / ":" / "@"
 */
#define ISA_PCHAR(u, p)							\
     (ISA_UNRESERVED(u, p) || ISA_PCT_ENCODED(p) || ISA_SUB_DELIM(p) ||	\
      ((*(p) == ':')) || ((*(p) == '@')))

/*
 * From https://www.w3.org/TR/leiri/
 *
 * " " / "<" / ">" / '"' / "{" / "}" / "|"
 * / "\" / "^" / "`" / %x0-1F / %x7F-D7FF
 * / %xE000-FFFD / %x10000-10FFFF
 */
#define ISA_UCSCHAR(p) \
    ((*(p) <= 0x20) || (*(p) >= 0x7F) || (*(p) == '<') || (*(p) == '>') || \
     (*(p) == '"')  || (*(p) == '{')  || (*(p) == '}') || (*(p) == '|') || \
     (*(p) == '\\') || (*(p) == '^')  || (*(p) == '`'))

#define ISA_UNRESERVED(u, p) (xmlIsUnreserved(u, p))

#define XML_URI_ALLOW_UNWISE    1
#define XML_URI_NO_UNESCAPE     2
#define XML_URI_ALLOW_UCSCHAR   4

static int
xmlIsUnreserved(xmlURIPtr uri, const char *cur) {
    if (uri == NULL)
        return(0);

    if (ISA_STRICTLY_UNRESERVED(cur))
        return(1);

    if (uri->cleanup & XML_URI_ALLOW_UNWISE) {
        if (IS_UNWISE(cur))
            return(1);
    } else if (uri->cleanup & XML_URI_ALLOW_UCSCHAR) {
        if (ISA_UCSCHAR(cur))
            return(1);
    }

    return(0);
}

/**
 * Parse an URI scheme
 *
 * ALPHA *( ALPHA / DIGIT / "+" / "-" / "." )
 *
 * @param uri  pointer to an URI structure
 * @param str  pointer to the string to analyze
 * @returns 0 or the error code
 */
static int
xmlParse3986Scheme(xmlURIPtr uri, const char **str) {
    const char *cur;

    cur = *str;
    if (!ISA_ALPHA(cur))
	return(1);
    cur++;

#if defined(_WIN32) || defined(__CYGWIN__)
    /*
     * Don't treat Windows drive letters as scheme.
     */
    if (*cur == ':')
        return(1);
#endif

    while (ISA_ALPHA(cur) || ISA_DIGIT(cur) ||
           (*cur == '+') || (*cur == '-') || (*cur == '.')) cur++;
    if (uri != NULL) {
	if (uri->scheme != NULL) xmlFree(uri->scheme);
	uri->scheme = STRNDUP(*str, cur - *str);
        if (uri->scheme == NULL)
            return(-1);
    }
    *str = cur;
    return(0);
}

/**
 * Parse the query part of an URI
 *
 * fragment      = *( pchar / "/" / "?" )
 * NOTE: the strict syntax as defined by 3986 does not allow '[' and ']'
 *       in the fragment identifier but this is used very broadly for
 *       xpointer scheme selection, so we are allowing it here to not break
 *       for example all the DocBook processing chains.
 *
 * @param uri  pointer to an URI structure
 * @param str  pointer to the string to analyze
 * @returns 0 or the error code
 */
static int
xmlParse3986Fragment(xmlURIPtr uri, const char **str)
{
    const char *cur;

    cur = *str;

    while ((ISA_PCHAR(uri, cur)) || (*cur == '/') || (*cur == '?') ||
           (*cur == '[') || (*cur == ']'))
        NEXT(cur);
    if (uri != NULL) {
        if (uri->fragment != NULL)
            xmlFree(uri->fragment);
	if (uri->cleanup & XML_URI_NO_UNESCAPE)
	    uri->fragment = STRNDUP(*str, cur - *str);
	else
	    uri->fragment = xmlURIUnescapeString(*str, cur - *str, NULL);
        if (uri->fragment == NULL)
            return (-1);
    }
    *str = cur;
    return (0);
}

/**
 * Parse the query part of an URI
 *
 * query = *uric
 *
 * @param uri  pointer to an URI structure
 * @param str  pointer to the string to analyze
 * @returns 0 or the error code
 */
static int
xmlParse3986Query(xmlURIPtr uri, const char **str)
{
    const char *cur;

    cur = *str;

    while ((ISA_PCHAR(uri, cur)) || (*cur == '/') || (*cur == '?'))
        NEXT(cur);
    if (uri != NULL) {
        if (uri->query != NULL)
            xmlFree(uri->query);
	if (uri->cleanup & XML_URI_NO_UNESCAPE)
	    uri->query = STRNDUP(*str, cur - *str);
	else
	    uri->query = xmlURIUnescapeString(*str, cur - *str, NULL);
        if (uri->query == NULL)
            return (-1);

	/* Save the raw bytes of the query as well.
	 * See: http://mail.gnome.org/archives/xml/2007-April/thread.html#00114
	 */
	if (uri->query_raw != NULL)
	    xmlFree (uri->query_raw);
	uri->query_raw = STRNDUP (*str, cur - *str);
        if (uri->query_raw == NULL)
            return (-1);
    }
    *str = cur;
    return (0);
}

/**
 * Parse a port part and fills in the appropriate fields
 * of the `uri` structure
 *
 * port          = *DIGIT
 *
 * @param uri  pointer to an URI structure
 * @param str  the string to analyze
 * @returns 0 or the error code
 */
static int
xmlParse3986Port(xmlURIPtr uri, const char **str)
{
    const char *cur = *str;
    int port = 0;

    if (ISA_DIGIT(cur)) {
	while (ISA_DIGIT(cur)) {
            int digit = *cur - '0';

            if (port > INT_MAX / 10)
                return(1);
            port *= 10;
            if (port > INT_MAX - digit)
                return(1);
	    port += digit;

	    cur++;
	}
	if (uri != NULL)
	    uri->port = port;
	*str = cur;
	return(0);
    }
    return(1);
}

/**
 * Parse an user information part and fills in the appropriate fields
 * of the `uri` structure
 *
 * userinfo      = *( unreserved / pct-encoded / sub-delims / ":" )
 *
 * @param uri  pointer to an URI structure
 * @param str  the string to analyze
 * @returns 0 or the error code
 */
static int
xmlParse3986Userinfo(xmlURIPtr uri, const char **str)
{
    const char *cur;

    cur = *str;
    while (ISA_UNRESERVED(uri, cur) || ISA_PCT_ENCODED(cur) ||
           ISA_SUB_DELIM(cur) || (*cur == ':'))
	NEXT(cur);
    if (*cur == '@') {
	if (uri != NULL) {
	    if (uri->user != NULL) xmlFree(uri->user);
	    if (uri->cleanup & XML_URI_NO_UNESCAPE)
		uri->user = STRNDUP(*str, cur - *str);
	    else
		uri->user = xmlURIUnescapeString(*str, cur - *str, NULL);
            if (uri->user == NULL)
                return(-1);
	}
	*str = cur;
	return(0);
    }
    return(1);
}

/**
 *    dec-octet     = DIGIT                 ; 0-9
 *                  / %x31-39 DIGIT         ; 10-99
 *                  / "1" 2DIGIT            ; 100-199
 *                  / "2" %x30-34 DIGIT     ; 200-249
 *                  / "25" %x30-35          ; 250-255
 *
 * Skip a dec-octet.
 *
 * @param str  the string to analyze
 * @returns 0 if found and skipped, 1 otherwise
 */
static int
xmlParse3986DecOctet(const char **str) {
    const char *cur = *str;

    if (!(ISA_DIGIT(cur)))
        return(1);
    if (!ISA_DIGIT(cur+1))
	cur++;
    else if ((*cur != '0') && (ISA_DIGIT(cur + 1)) && (!ISA_DIGIT(cur+2)))
	cur += 2;
    else if ((*cur == '1') && (ISA_DIGIT(cur + 1)) && (ISA_DIGIT(cur + 2)))
	cur += 3;
    else if ((*cur == '2') && (*(cur + 1) >= '0') &&
	     (*(cur + 1) <= '4') && (ISA_DIGIT(cur + 2)))
	cur += 3;
    else if ((*cur == '2') && (*(cur + 1) == '5') &&
	     (*(cur + 2) >= '0') && (*(cur + 1) <= '5'))
	cur += 3;
    else
        return(1);
    *str = cur;
    return(0);
}
/**
 * Parse an host part and fills in the appropriate fields
 * of the `uri` structure
 *
 * host          = IP-literal / IPv4address / reg-name
 * IP-literal    = "[" ( IPv6address / IPvFuture  ) "]"
 * IPv4address   = dec-octet "." dec-octet "." dec-octet "." dec-octet
 * reg-name      = *( unreserved / pct-encoded / sub-delims )
 *
 * @param uri  pointer to an URI structure
 * @param str  the string to analyze
 * @returns 0 or the error code
 */
static int
xmlParse3986Host(xmlURIPtr uri, const char **str)
{
    const char *cur = *str;
    const char *host;

    host = cur;
    /*
     * IPv6 and future addressing scheme are enclosed between brackets
     */
    if (*cur == '[') {
        cur++;
	while ((*cur != ']') && (*cur != 0))
	    cur++;
	if (*cur != ']')
	    return(1);
	cur++;
	goto found;
    }
    /*
     * try to parse an IPv4
     */
    if (ISA_DIGIT(cur)) {
        if (xmlParse3986DecOctet(&cur) != 0)
	    goto not_ipv4;
	if (*cur != '.')
	    goto not_ipv4;
	cur++;
        if (xmlParse3986DecOctet(&cur) != 0)
	    goto not_ipv4;
	if (*cur != '.')
	    goto not_ipv4;
        if (xmlParse3986DecOctet(&cur) != 0)
	    goto not_ipv4;
	if (*cur != '.')
	    goto not_ipv4;
        if (xmlParse3986DecOctet(&cur) != 0)
	    goto not_ipv4;
	goto found;
not_ipv4:
        cur = *str;
    }
    /*
     * then this should be a hostname which can be empty
     */
    while (ISA_UNRESERVED(uri, cur) ||
           ISA_PCT_ENCODED(cur) || ISA_SUB_DELIM(cur))
        NEXT(cur);
found:
    if (uri != NULL) {
	if (uri->authority != NULL) xmlFree(uri->authority);
	uri->authority = NULL;
	if (uri->server != NULL) xmlFree(uri->server);
	if (cur != host) {
	    if (uri->cleanup & XML_URI_NO_UNESCAPE)
		uri->server = STRNDUP(host, cur - host);
	    else
		uri->server = xmlURIUnescapeString(host, cur - host, NULL);
            if (uri->server == NULL)
                return(-1);
	} else
	    uri->server = NULL;
    }
    *str = cur;
    return(0);
}

/**
 * Parse an authority part and fills in the appropriate fields
 * of the `uri` structure
 *
 * authority     = [ userinfo "@" ] host [ ":" port ]
 *
 * @param uri  pointer to an URI structure
 * @param str  the string to analyze
 * @returns 0 or the error code
 */
static int
xmlParse3986Authority(xmlURIPtr uri, const char **str)
{
    const char *cur;
    int ret;

    cur = *str;
    /*
     * try to parse an userinfo and check for the trailing @
     */
    ret = xmlParse3986Userinfo(uri, &cur);
    if (ret < 0)
        return(ret);
    if ((ret != 0) || (*cur != '@'))
        cur = *str;
    else
        cur++;
    ret = xmlParse3986Host(uri, &cur);
    if (ret != 0) return(ret);
    if (*cur == ':') {
        cur++;
        ret = xmlParse3986Port(uri, &cur);
	if (ret != 0) return(ret);
    }
    *str = cur;
    return(0);
}

/**
 * Parse a segment and fills in the appropriate fields
 * of the `uri` structure
 *
 * segment       = *pchar
 * segment-nz    = 1*pchar
 * segment-nz-nc = 1*( unreserved / pct-encoded / sub-delims / "@" )
 *               ; non-zero-length segment without any colon ":"
 *
 * @param uri  the URI
 * @param str  the string to analyze
 * @param forbid  an optional forbidden character
 * @param empty  allow an empty segment
 * @returns 0 or the error code
 */
static int
xmlParse3986Segment(xmlURIPtr uri, const char **str, char forbid, int empty)
{
    const char *cur;

    cur = *str;
    if (!ISA_PCHAR(uri, cur) || (*cur == forbid)) {
        if (empty)
	    return(0);
	return(1);
    }
    NEXT(cur);

#if defined(_WIN32) || defined(__CYGWIN__)
    /*
     * Allow Windows drive letters.
     */
    if ((forbid == ':') && (*cur == forbid))
        NEXT(cur);
#endif

    while (ISA_PCHAR(uri, cur) && (*cur != forbid))
        NEXT(cur);
    *str = cur;
    return (0);
}

/**
 * Parse an path absolute or empty and fills in the appropriate fields
 * of the `uri` structure
 *
 * path-abempty  = *( "/" segment )
 *
 * @param uri  pointer to an URI structure
 * @param str  the string to analyze
 * @returns 0 or the error code
 */
static int
xmlParse3986PathAbEmpty(xmlURIPtr uri, const char **str)
{
    const char *cur;
    int ret;

    cur = *str;

    while (*cur == '/') {
        cur++;
	ret = xmlParse3986Segment(uri, &cur, 0, 1);
	if (ret != 0) return(ret);
    }
    if (uri != NULL) {
	if (uri->path != NULL) xmlFree(uri->path);
        if (*str != cur) {
            if (uri->cleanup & XML_URI_NO_UNESCAPE)
                uri->path = STRNDUP(*str, cur - *str);
            else
                uri->path = xmlURIUnescapeString(*str, cur - *str, NULL);
            if (uri->path == NULL)
                return (-1);
        } else {
            uri->path = NULL;
        }
    }
    *str = cur;
    return (0);
}

/**
 * Parse an path absolute and fills in the appropriate fields
 * of the `uri` structure
 *
 * path-absolute = "/" [ segment-nz *( "/" segment ) ]
 *
 * @param uri  pointer to an URI structure
 * @param str  the string to analyze
 * @returns 0 or the error code
 */
static int
xmlParse3986PathAbsolute(xmlURIPtr uri, const char **str)
{
    const char *cur;
    int ret;

    cur = *str;

    if (*cur != '/')
        return(1);
    cur++;
    ret = xmlParse3986Segment(uri, &cur, 0, 0);
    if (ret == 0) {
	while (*cur == '/') {
	    cur++;
	    ret = xmlParse3986Segment(uri, &cur, 0, 1);
	    if (ret != 0) return(ret);
	}
    }
    if (uri != NULL) {
	if (uri->path != NULL) xmlFree(uri->path);
        if (cur != *str) {
            if (uri->cleanup & XML_URI_NO_UNESCAPE)
                uri->path = STRNDUP(*str, cur - *str);
            else
                uri->path = xmlURIUnescapeString(*str, cur - *str, NULL);
            if (uri->path == NULL)
                return (-1);
        } else {
            uri->path = NULL;
        }
    }
    *str = cur;
    return (0);
}

/**
 * Parse an path without root and fills in the appropriate fields
 * of the `uri` structure
 *
 * path-rootless = segment-nz *( "/" segment )
 *
 * @param uri  pointer to an URI structure
 * @param str  the string to analyze
 * @returns 0 or the error code
 */
static int
xmlParse3986PathRootless(xmlURIPtr uri, const char **str)
{
    const char *cur;
    int ret;

    cur = *str;

    ret = xmlParse3986Segment(uri, &cur, 0, 0);
    if (ret != 0) return(ret);
    while (*cur == '/') {
        cur++;
	ret = xmlParse3986Segment(uri, &cur, 0, 1);
	if (ret != 0) return(ret);
    }
    if (uri != NULL) {
	if (uri->path != NULL) xmlFree(uri->path);
        if (cur != *str) {
            if (uri->cleanup & XML_URI_NO_UNESCAPE)
                uri->path = STRNDUP(*str, cur - *str);
            else
                uri->path = xmlURIUnescapeString(*str, cur - *str, NULL);
            if (uri->path == NULL)
                return (-1);
        } else {
            uri->path = NULL;
        }
    }
    *str = cur;
    return (0);
}

/**
 * Parse an path which is not a scheme and fills in the appropriate fields
 * of the `uri` structure
 *
 * path-noscheme = segment-nz-nc *( "/" segment )
 *
 * @param uri  pointer to an URI structure
 * @param str  the string to analyze
 * @returns 0 or the error code
 */
static int
xmlParse3986PathNoScheme(xmlURIPtr uri, const char **str)
{
    const char *cur;
    int ret;

    cur = *str;

    ret = xmlParse3986Segment(uri, &cur, ':', 0);
    if (ret != 0) return(ret);
    while (*cur == '/') {
        cur++;
	ret = xmlParse3986Segment(uri, &cur, 0, 1);
	if (ret != 0) return(ret);
    }
    if (uri != NULL) {
	if (uri->path != NULL) xmlFree(uri->path);
        if (cur != *str) {
            if (uri->cleanup & XML_URI_NO_UNESCAPE)
                uri->path = STRNDUP(*str, cur - *str);
            else
                uri->path = xmlURIUnescapeString(*str, cur - *str, NULL);
            if (uri->path == NULL)
                return (-1);
        } else {
            uri->path = NULL;
        }
    }
    *str = cur;
    return (0);
}

/**
 * Parse an hierarchical part and fills in the appropriate fields
 * of the `uri` structure
 *
 * hier-part     = "//" authority path-abempty
 *                / path-absolute
 *                / path-rootless
 *                / path-empty
 *
 * @param uri  pointer to an URI structure
 * @param str  the string to analyze
 * @returns 0 or the error code
 */
static int
xmlParse3986HierPart(xmlURIPtr uri, const char **str)
{
    const char *cur;
    int ret;

    cur = *str;

    if ((*cur == '/') && (*(cur + 1) == '/')) {
        cur += 2;
	ret = xmlParse3986Authority(uri, &cur);
	if (ret != 0) return(ret);
        /*
         * An empty server is marked with a special URI value.
         */
	if ((uri->server == NULL) && (uri->port == PORT_EMPTY))
	    uri->port = PORT_EMPTY_SERVER;
	ret = xmlParse3986PathAbEmpty(uri, &cur);
	if (ret != 0) return(ret);
	*str = cur;
	return(0);
    } else if (*cur == '/') {
        ret = xmlParse3986PathAbsolute(uri, &cur);
	if (ret != 0) return(ret);
    } else if (ISA_PCHAR(uri, cur)) {
        ret = xmlParse3986PathRootless(uri, &cur);
	if (ret != 0) return(ret);
    } else {
	/* path-empty is effectively empty */
	if (uri != NULL) {
	    if (uri->path != NULL) xmlFree(uri->path);
	    uri->path = NULL;
	}
    }
    *str = cur;
    return (0);
}

/**
 * Parse an URI string and fills in the appropriate fields
 * of the `uri` structure
 *
 * relative-ref  = relative-part [ "?" query ] [ "\#" fragment ]
 * relative-part = "//" authority path-abempty
 *               / path-absolute
 *               / path-noscheme
 *               / path-empty
 *
 * @param uri  pointer to an URI structure
 * @param str  the string to analyze
 * @returns 0 or the error code
 */
static int
xmlParse3986RelativeRef(xmlURIPtr uri, const char *str) {
    int ret;

    if ((*str == '/') && (*(str + 1) == '/')) {
        str += 2;
	ret = xmlParse3986Authority(uri, &str);
	if (ret != 0) return(ret);
	ret = xmlParse3986PathAbEmpty(uri, &str);
	if (ret != 0) return(ret);
    } else if (*str == '/') {
	ret = xmlParse3986PathAbsolute(uri, &str);
	if (ret != 0) return(ret);
    } else if (ISA_PCHAR(uri, str)) {
        ret = xmlParse3986PathNoScheme(uri, &str);
	if (ret != 0) return(ret);
    } else {
	/* path-empty is effectively empty */
	if (uri != NULL) {
	    if (uri->path != NULL) xmlFree(uri->path);
	    uri->path = NULL;
	}
    }

    if (*str == '?') {
	str++;
	ret = xmlParse3986Query(uri, &str);
	if (ret != 0) return(ret);
    }
    if (*str == '#') {
	str++;
	ret = xmlParse3986Fragment(uri, &str);
	if (ret != 0) return(ret);
    }
    if (*str != 0) {
	xmlCleanURI(uri);
	return(1);
    }
    return(0);
}


/**
 * Parse an URI string and fills in the appropriate fields
 * of the `uri` structure
 *
 * scheme ":" hier-part [ "?" query ] [ "\#" fragment ]
 *
 * @param uri  pointer to an URI structure
 * @param str  the string to analyze
 * @returns 0 or the error code
 */
static int
xmlParse3986URI(xmlURIPtr uri, const char *str) {
    int ret;

    ret = xmlParse3986Scheme(uri, &str);
    if (ret != 0) return(ret);
    if (*str != ':') {
	return(1);
    }
    str++;
    ret = xmlParse3986HierPart(uri, &str);
    if (ret != 0) return(ret);
    if (*str == '?') {
	str++;
	ret = xmlParse3986Query(uri, &str);
	if (ret != 0) return(ret);
    }
    if (*str == '#') {
	str++;
	ret = xmlParse3986Fragment(uri, &str);
	if (ret != 0) return(ret);
    }
    if (*str != 0) {
	xmlCleanURI(uri);
	return(1);
    }
    return(0);
}

/**
 * Parse an URI reference string and fills in the appropriate fields
 * of the `uri` structure
 *
 * URI-reference = URI / relative-ref
 *
 * @param uri  pointer to an URI structure
 * @param str  the string to analyze
 * @returns 0 or the error code
 */
static int
xmlParse3986URIReference(xmlURIPtr uri, const char *str) {
    int ret;

    if (str == NULL)
	return(-1);
    xmlCleanURI(uri);

    /*
     * Try first to parse absolute refs, then fallback to relative if
     * it fails.
     */
    ret = xmlParse3986URI(uri, str);
    if (ret < 0)
        return(ret);
    if (ret != 0) {
	xmlCleanURI(uri);
        ret = xmlParse3986RelativeRef(uri, str);
	if (ret != 0) {
	    xmlCleanURI(uri);
	    return(ret);
	}
    }
    return(0);
}

/**
 * Parse an URI based on RFC 3986
 *
 * URI-reference = [ absoluteURI | relativeURI ] [ "\#" fragment ]
 *
 * @since 2.13.0
 *
 * @param str  the URI string to analyze
 * @param uriOut  optional pointer to parsed URI
 * @returns 0 on success, an error code (typically 1) if the URI is invalid
 * or -1 if a memory allocation failed.
 */
int
xmlParseURISafe(const char *str, xmlURI **uriOut) {
    xmlURIPtr uri;
    int ret;

    if (uriOut == NULL)
        return(1);
    *uriOut = NULL;
    if (str == NULL)
	return(1);

    uri = xmlCreateURI();
    if (uri == NULL)
        return(-1);

    ret = xmlParse3986URIReference(uri, str);
    if (ret) {
        xmlFreeURI(uri);
        return(ret);
    }

    *uriOut = uri;
    return(0);
}

/**
 * Parse an URI based on RFC 3986
 *
 * URI-reference = [ absoluteURI | relativeURI ] [ "\#" fragment ]
 *
 * @param str  the URI string to analyze
 * @returns a newly built xmlURI or NULL in case of error
 */
xmlURI *
xmlParseURI(const char *str) {
    xmlURIPtr uri;
    xmlParseURISafe(str, &uri);
    return(uri);
}

/**
 * Parse an URI reference string based on RFC 3986 and fills in the
 * appropriate fields of the `uri` structure
 *
 * URI-reference = URI / relative-ref
 *
 * @param uri  pointer to an URI structure
 * @param str  the string to analyze
 * @returns 0 or the error code
 */
int
xmlParseURIReference(xmlURI *uri, const char *str) {
    return(xmlParse3986URIReference(uri, str));
}

/**
 * Parse an URI but allows to keep intact the original fragments.
 *
 * URI-reference = URI / relative-ref
 *
 * @param str  the URI string to analyze
 * @param raw  if 1 unescaping of URI pieces are disabled
 * @returns a newly built xmlURI or NULL in case of error
 */
xmlURI *
xmlParseURIRaw(const char *str, int raw) {
    xmlURIPtr uri;
    int ret;

    if (str == NULL)
	return(NULL);
    uri = xmlCreateURI();
    if (uri != NULL) {
        if (raw) {
	    uri->cleanup |= XML_URI_NO_UNESCAPE;
	}
	ret = xmlParseURIReference(uri, str);
        if (ret) {
	    xmlFreeURI(uri);
	    return(NULL);
	}
    }
    return(uri);
}

/************************************************************************
 *									*
 *			Generic URI structure functions			*
 *									*
 ************************************************************************/

/**
 * Simply creates an empty xmlURI
 *
 * @returns the new structure or NULL in case of error
 */
xmlURI *
xmlCreateURI(void) {
    xmlURIPtr ret;

    ret = (xmlURIPtr) xmlMalloc(sizeof(xmlURI));
    if (ret == NULL)
	return(NULL);
    memset(ret, 0, sizeof(xmlURI));
    ret->port = PORT_EMPTY;
    return(ret);
}

/**
 * Function to handle properly a reallocation when saving an URI
 * Also imposes some limit on the length of an URI string output
 */
static xmlChar *
xmlSaveUriRealloc(xmlChar *ret, int *max) {
    xmlChar *temp;
    int newSize;

    newSize = xmlGrowCapacity(*max, 1, 80, MAX_URI_LENGTH);
    if (newSize < 0)
        return(NULL);
    temp = xmlRealloc(ret, newSize + 1);
    if (temp == NULL)
        return(NULL);
    *max = newSize;
    return(temp);
}

/**
 * Save the URI as an escaped string
 *
 * @param uri  pointer to an xmlURI
 * @returns a new string (to be deallocated by caller)
 */
xmlChar *
xmlSaveUri(xmlURI *uri) {
    xmlChar *ret = NULL;
    xmlChar *temp;
    const char *p;
    int len;
    int max;

    if (uri == NULL) return(NULL);


    max = 80;
    ret = xmlMalloc(max + 1);
    if (ret == NULL)
	return(NULL);
    len = 0;

    if (uri->scheme != NULL) {
	p = uri->scheme;
	while (*p != 0) {
	    if (len >= max) {
                temp = xmlSaveUriRealloc(ret, &max);
                if (temp == NULL) goto mem_error;
		ret = temp;
	    }
	    ret[len++] = *p++;
	}
	if (len >= max) {
            temp = xmlSaveUriRealloc(ret, &max);
            if (temp == NULL) goto mem_error;
            ret = temp;
	}
	ret[len++] = ':';
    }
    if (uri->opaque != NULL) {
	p = uri->opaque;
	while (*p != 0) {
	    if (len + 3 >= max) {
                temp = xmlSaveUriRealloc(ret, &max);
                if (temp == NULL) goto mem_error;
                ret = temp;
	    }
	    if (IS_RESERVED(*(p)) || IS_UNRESERVED(*(p)))
		ret[len++] = *p++;
	    else {
		int val = *(unsigned char *)p++;
		int hi = val / 0x10, lo = val % 0x10;
		ret[len++] = '%';
		ret[len++] = hi + (hi > 9? 'A'-10 : '0');
		ret[len++] = lo + (lo > 9? 'A'-10 : '0');
	    }
	}
    } else {
	if ((uri->server != NULL) || (uri->port != PORT_EMPTY)) {
	    if (len + 3 >= max) {
                temp = xmlSaveUriRealloc(ret, &max);
                if (temp == NULL) goto mem_error;
                ret = temp;
	    }
	    ret[len++] = '/';
	    ret[len++] = '/';
	    if (uri->user != NULL) {
		p = uri->user;
		while (*p != 0) {
		    if (len + 3 >= max) {
                        temp = xmlSaveUriRealloc(ret, &max);
                        if (temp == NULL) goto mem_error;
                        ret = temp;
		    }
		    if ((IS_UNRESERVED(*(p))) ||
			((*(p) == ';')) || ((*(p) == ':')) ||
			((*(p) == '&')) || ((*(p) == '=')) ||
			((*(p) == '+')) || ((*(p) == '$')) ||
			((*(p) == ',')))
			ret[len++] = *p++;
		    else {
			int val = *(unsigned char *)p++;
			int hi = val / 0x10, lo = val % 0x10;
			ret[len++] = '%';
			ret[len++] = hi + (hi > 9? 'A'-10 : '0');
			ret[len++] = lo + (lo > 9? 'A'-10 : '0');
		    }
		}
		if (len + 3 >= max) {
                    temp = xmlSaveUriRealloc(ret, &max);
                    if (temp == NULL) goto mem_error;
                    ret = temp;
		}
		ret[len++] = '@';
	    }
	    if (uri->server != NULL) {
		p = uri->server;
		while (*p != 0) {
		    if (len >= max) {
			temp = xmlSaveUriRealloc(ret, &max);
			if (temp == NULL) goto mem_error;
			ret = temp;
		    }
                    /* TODO: escaping? */
		    ret[len++] = (xmlChar) *p++;
		}
	    }
            if (uri->port > 0) {
                if (len + 10 >= max) {
                    temp = xmlSaveUriRealloc(ret, &max);
                    if (temp == NULL) goto mem_error;
                    ret = temp;
                }
                len += snprintf((char *) &ret[len], max - len, ":%d", uri->port);
            }
	} else if (uri->authority != NULL) {
	    if (len + 3 >= max) {
                temp = xmlSaveUriRealloc(ret, &max);
                if (temp == NULL) goto mem_error;
                ret = temp;
	    }
	    ret[len++] = '/';
	    ret[len++] = '/';
	    p = uri->authority;
	    while (*p != 0) {
		if (len + 3 >= max) {
                    temp = xmlSaveUriRealloc(ret, &max);
                    if (temp == NULL) goto mem_error;
                    ret = temp;
		}
		if ((IS_UNRESERVED(*(p))) ||
                    ((*(p) == '$')) || ((*(p) == ',')) || ((*(p) == ';')) ||
                    ((*(p) == ':')) || ((*(p) == '@')) || ((*(p) == '&')) ||
                    ((*(p) == '=')) || ((*(p) == '+')))
		    ret[len++] = *p++;
		else {
		    int val = *(unsigned char *)p++;
		    int hi = val / 0x10, lo = val % 0x10;
		    ret[len++] = '%';
		    ret[len++] = hi + (hi > 9? 'A'-10 : '0');
		    ret[len++] = lo + (lo > 9? 'A'-10 : '0');
		}
	    }
	} else if (uri->scheme != NULL) {
	    if (len + 3 >= max) {
                temp = xmlSaveUriRealloc(ret, &max);
                if (temp == NULL) goto mem_error;
                ret = temp;
	    }
	}
	if (uri->path != NULL) {
	    p = uri->path;
	    /*
	     * the colon in file:///d: should not be escaped or
	     * Windows accesses fail later.
	     */
	    if ((uri->scheme != NULL) &&
		(p[0] == '/') &&
		(((p[1] >= 'a') && (p[1] <= 'z')) ||
		 ((p[1] >= 'A') && (p[1] <= 'Z'))) &&
		(p[2] == ':') &&
	        (xmlStrEqual(BAD_CAST uri->scheme, BAD_CAST "file"))) {
		if (len + 3 >= max) {
                    temp = xmlSaveUriRealloc(ret, &max);
                    if (temp == NULL) goto mem_error;
                    ret = temp;
		}
		ret[len++] = *p++;
		ret[len++] = *p++;
		ret[len++] = *p++;
	    }
	    while (*p != 0) {
		if (len + 3 >= max) {
                    temp = xmlSaveUriRealloc(ret, &max);
                    if (temp == NULL) goto mem_error;
                    ret = temp;
		}
		if ((IS_UNRESERVED(*(p))) || ((*(p) == '/')) ||
                    ((*(p) == ';')) || ((*(p) == '@')) || ((*(p) == '&')) ||
	            ((*(p) == '=')) || ((*(p) == '+')) || ((*(p) == '$')) ||
	            ((*(p) == ',')))
		    ret[len++] = *p++;
		else {
		    int val = *(unsigned char *)p++;
		    int hi = val / 0x10, lo = val % 0x10;
		    ret[len++] = '%';
		    ret[len++] = hi + (hi > 9? 'A'-10 : '0');
		    ret[len++] = lo + (lo > 9? 'A'-10 : '0');
		}
	    }
	}
	if (uri->query_raw != NULL) {
	    if (len + 1 >= max) {
                temp = xmlSaveUriRealloc(ret, &max);
                if (temp == NULL) goto mem_error;
                ret = temp;
	    }
	    ret[len++] = '?';
	    p = uri->query_raw;
	    while (*p != 0) {
		if (len + 1 >= max) {
                    temp = xmlSaveUriRealloc(ret, &max);
                    if (temp == NULL) goto mem_error;
                    ret = temp;
		}
		ret[len++] = *p++;
	    }
	} else if (uri->query != NULL) {
	    if (len + 3 >= max) {
                temp = xmlSaveUriRealloc(ret, &max);
                if (temp == NULL) goto mem_error;
                ret = temp;
	    }
	    ret[len++] = '?';
	    p = uri->query;
	    while (*p != 0) {
		if (len + 3 >= max) {
                    temp = xmlSaveUriRealloc(ret, &max);
                    if (temp == NULL) goto mem_error;
                    ret = temp;
		}
		if ((IS_UNRESERVED(*(p))) || (IS_RESERVED(*(p))))
		    ret[len++] = *p++;
		else {
		    int val = *(unsigned char *)p++;
		    int hi = val / 0x10, lo = val % 0x10;
		    ret[len++] = '%';
		    ret[len++] = hi + (hi > 9? 'A'-10 : '0');
		    ret[len++] = lo + (lo > 9? 'A'-10 : '0');
		}
	    }
	}
    }
    if (uri->fragment != NULL) {
	if (len + 3 >= max) {
            temp = xmlSaveUriRealloc(ret, &max);
            if (temp == NULL) goto mem_error;
            ret = temp;
	}
	ret[len++] = '#';
	p = uri->fragment;
	while (*p != 0) {
	    if (len + 3 >= max) {
                temp = xmlSaveUriRealloc(ret, &max);
                if (temp == NULL) goto mem_error;
                ret = temp;
	    }
	    if ((IS_UNRESERVED(*(p))) || (IS_RESERVED(*(p))))
		ret[len++] = *p++;
	    else {
		int val = *(unsigned char *)p++;
		int hi = val / 0x10, lo = val % 0x10;
		ret[len++] = '%';
		ret[len++] = hi + (hi > 9? 'A'-10 : '0');
		ret[len++] = lo + (lo > 9? 'A'-10 : '0');
	    }
	}
    }
    if (len >= max) {
        temp = xmlSaveUriRealloc(ret, &max);
        if (temp == NULL) goto mem_error;
        ret = temp;
    }
    ret[len] = 0;
    return(ret);

mem_error:
    xmlFree(ret);
    return(NULL);
}

/**
 * Prints the URI in the stream `stream`.
 *
 * @param stream  a FILE* for the output
 * @param uri  pointer to an xmlURI
 */
void
xmlPrintURI(FILE *stream, xmlURI *uri) {
    xmlChar *out;

    out = xmlSaveUri(uri);
    if (out != NULL) {
	fprintf(stream, "%s", (char *) out);
	xmlFree(out);
    }
}

/**
 * Make sure the xmlURI struct is free of content
 *
 * @param uri  pointer to an xmlURI
 */
static void
xmlCleanURI(xmlURIPtr uri) {
    if (uri == NULL) return;

    if (uri->scheme != NULL) xmlFree(uri->scheme);
    uri->scheme = NULL;
    if (uri->server != NULL) xmlFree(uri->server);
    uri->server = NULL;
    if (uri->user != NULL) xmlFree(uri->user);
    uri->user = NULL;
    if (uri->path != NULL) xmlFree(uri->path);
    uri->path = NULL;
    if (uri->fragment != NULL) xmlFree(uri->fragment);
    uri->fragment = NULL;
    if (uri->opaque != NULL) xmlFree(uri->opaque);
    uri->opaque = NULL;
    if (uri->authority != NULL) xmlFree(uri->authority);
    uri->authority = NULL;
    if (uri->query != NULL) xmlFree(uri->query);
    uri->query = NULL;
    if (uri->query_raw != NULL) xmlFree(uri->query_raw);
    uri->query_raw = NULL;
}

/**
 * Free up the xmlURI struct
 *
 * @param uri  pointer to an xmlURI
 */
void
xmlFreeURI(xmlURI *uri) {
    if (uri == NULL) return;

    if (uri->scheme != NULL) xmlFree(uri->scheme);
    if (uri->server != NULL) xmlFree(uri->server);
    if (uri->user != NULL) xmlFree(uri->user);
    if (uri->path != NULL) xmlFree(uri->path);
    if (uri->fragment != NULL) xmlFree(uri->fragment);
    if (uri->opaque != NULL) xmlFree(uri->opaque);
    if (uri->authority != NULL) xmlFree(uri->authority);
    if (uri->query != NULL) xmlFree(uri->query);
    if (uri->query_raw != NULL) xmlFree(uri->query_raw);
    xmlFree(uri);
}

/************************************************************************
 *									*
 *			Helper functions				*
 *									*
 ************************************************************************/

static int
xmlIsPathSeparator(int c, int isFile) {
    (void) isFile;

    if (c == '/')
        return(1);

#if defined(_WIN32) || defined(__CYGWIN__)
    if (isFile && (c == '\\'))
        return(1);
#endif

    return(0);
}

/**
 * Normalize a filesystem path or URI.
 *
 * @param path  pointer to the path string
 * @param isFile  true for filesystem paths, false for URIs
 * @returns 0 or an error code
 */
static int
xmlNormalizePath(char *path, int isFile) {
    char *cur, *out;
    int numSeg = 0;

    if (path == NULL)
	return(-1);

    cur = path;
    out = path;

    if (*cur == 0)
        return(0);

    if (xmlIsPathSeparator(*cur, isFile)) {
        cur++;
        *out++ = '/';
    }

    while (*cur != 0) {
        /*
         * At this point, out is either empty or ends with a separator.
         * Collapse multiple separators first.
         */
        while (xmlIsPathSeparator(*cur, isFile)) {
#if defined(_WIN32) || defined(__CYGWIN__)
            /* Allow two separators at start of path */
            if ((isFile) && (out == path + 1))
                *out++ = '/';
#endif
            cur++;
        }

        if (*cur == '.') {
            if (cur[1] == 0) {
                /* Ignore "." at end of path */
                break;
            } else if (xmlIsPathSeparator(cur[1], isFile)) {
                /* Skip "./" */
                cur += 2;
                continue;
            } else if ((cur[1] == '.') &&
                       ((cur[2] == 0) || xmlIsPathSeparator(cur[2], isFile))) {
                if (numSeg > 0) {
                    /* Handle ".." by removing last segment */
                    do {
                        out--;
                    } while ((out > path) &&
                             !xmlIsPathSeparator(out[-1], isFile));
                    numSeg--;

                    if (cur[2] == 0)
                        break;
                    cur += 3;
                    continue;
                } else if (out[0] == '/') {
                    /* Ignore extraneous ".." in absolute paths */
                    if (cur[2] == 0)
                        break;
                    cur += 3;
                    continue;
                } else {
                    /* Keep "../" at start of relative path */
                    numSeg--;
                }
            }
        }

        /* Copy segment */
        while ((*cur != 0) && !xmlIsPathSeparator(*cur, isFile)) {
            *out++ = *cur++;
        }

        /* Copy separator */
        if (*cur != 0) {
            cur++;
            *out++ = '/';
        }

        numSeg++;
    }

    /* Keep "." if output is empty and it's a file */
    if ((isFile) && (out <= path))
        *out++ = '.';
    *out = 0;

    return(0);
}

/**
 * Applies the 5 normalization steps to a path string--that is, RFC 2396
 * Section 5.2, steps 6.c through 6.g.
 *
 * Normalization occurs directly on the string, no new allocation is done
 *
 * @param path  pointer to the path string
 * @returns 0 or an error code
 */
int
xmlNormalizeURIPath(char *path) {
    return(xmlNormalizePath(path, 0));
}

static int is_hex(char c) {
    if (((c >= '0') && (c <= '9')) ||
        ((c >= 'a') && (c <= 'f')) ||
        ((c >= 'A') && (c <= 'F')))
	return(1);
    return(0);
}

/**
 * Unescaping routine, but does not check that the string is an URI. The
 * output is a direct unsigned char translation of %XX values (no encoding)
 * Note that the length of the result can only be smaller or same size as
 * the input string.
 *
 * @param str  the string to unescape
 * @param len  the length in bytes to unescape (or <= 0 to indicate full string)
 * @param target  optional destination buffer
 * @returns a copy of the string, but unescaped, will return NULL only in case
 * of error
 */
char *
xmlURIUnescapeString(const char *str, int len, char *target) {
    char *ret, *out;
    const char *in;

    if (str == NULL)
	return(NULL);
    if (len <= 0) len = strlen(str);
    if (len < 0) return(NULL);

    if (target == NULL) {
	ret = xmlMalloc(len + 1);
	if (ret == NULL)
	    return(NULL);
    } else
	ret = target;
    in = str;
    out = ret;
    while(len > 0) {
	if ((len > 2) && (*in == '%') && (is_hex(in[1])) && (is_hex(in[2]))) {
            int c = 0;
	    in++;
	    if ((*in >= '0') && (*in <= '9'))
	        c = (*in - '0');
	    else if ((*in >= 'a') && (*in <= 'f'))
	        c = (*in - 'a') + 10;
	    else if ((*in >= 'A') && (*in <= 'F'))
	        c = (*in - 'A') + 10;
	    in++;
	    if ((*in >= '0') && (*in <= '9'))
	        c = c * 16 + (*in - '0');
	    else if ((*in >= 'a') && (*in <= 'f'))
	        c = c * 16 + (*in - 'a') + 10;
	    else if ((*in >= 'A') && (*in <= 'F'))
	        c = c * 16 + (*in - 'A') + 10;
	    in++;
	    len -= 3;
            /* Explicit sign change */
	    *out++ = (char) c;
	} else {
	    *out++ = *in++;
	    len--;
	}
    }
    *out = 0;
    return(ret);
}

/**
 * This routine escapes a string to hex, ignoring unreserved characters
 * a-z, A-Z, 0-9, "-._~", a few sub-delims "!*'()", the gen-delim "@"
 * (why?) and the characters in the exception list.
 *
 * @param str  string to escape
 * @param list  exception list string of chars not to escape
 * @returns a new escaped string or NULL in case of error.
 */
xmlChar *
xmlURIEscapeStr(const xmlChar *str, const xmlChar *list) {
    xmlChar *ret, ch;
    const xmlChar *in;
    int len, out;

    if (str == NULL)
	return(NULL);
    if (str[0] == 0)
	return(xmlStrdup(str));
    len = xmlStrlen(str);

    len += 20;
    ret = xmlMalloc(len);
    if (ret == NULL)
	return(NULL);
    in = (const xmlChar *) str;
    out = 0;
    while(*in != 0) {
	if (len - out <= 3) {
            xmlChar *temp;
            int newSize;

            newSize = xmlGrowCapacity(len, 1, 1, XML_MAX_ITEMS);
            if (newSize < 0) {
		xmlFree(ret);
                return(NULL);
            }
            temp = xmlRealloc(ret, newSize);
	    if (temp == NULL) {
		xmlFree(ret);
		return(NULL);
	    }
	    ret = temp;
            len = newSize;
	}

	ch = *in;

	if ((ch != '@') && (!IS_UNRESERVED(ch)) && (!xmlStrchr(list, ch))) {
	    unsigned char val;
	    ret[out++] = '%';
	    val = ch >> 4;
	    if (val <= 9)
		ret[out++] = '0' + val;
	    else
		ret[out++] = 'A' + val - 0xA;
	    val = ch & 0xF;
	    if (val <= 9)
		ret[out++] = '0' + val;
	    else
		ret[out++] = 'A' + val - 0xA;
	    in++;
	} else {
	    ret[out++] = *in++;
	}

    }
    ret[out] = 0;
    return(ret);
}

/**
 * Escaping routine, does not do validity checks !
 * It will try to escape the chars needing this, but this is heuristic
 * based it's impossible to be sure.
 *
 * 25 May 2001
 * Uses #xmlParseURI and #xmlURIEscapeStr to try to escape correctly
 * according to RFC2396.
 *   - Carl Douglas
 * @param str  the string of the URI to escape
 * @returns an copy of the string, but escaped
 */
xmlChar *
xmlURIEscape(const xmlChar * str)
{
    xmlChar *ret, *segment = NULL;
    xmlURIPtr uri;
    int ret2;

    if (str == NULL)
        return (NULL);

    uri = xmlCreateURI();
    if (uri != NULL) {
	/*
	 * Allow escaping errors in the unescaped form
	 */
        uri->cleanup = XML_URI_ALLOW_UNWISE;
        ret2 = xmlParseURIReference(uri, (const char *)str);
        if (ret2) {
            xmlFreeURI(uri);
            return (NULL);
        }
    }

    if (!uri)
        return NULL;

    ret = NULL;

#define NULLCHK(p) if(!p) { \
         xmlFreeURI(uri); \
         xmlFree(ret); \
         return NULL; } \

    if (uri->scheme) {
        segment = xmlURIEscapeStr(BAD_CAST uri->scheme, BAD_CAST "+-.");
        NULLCHK(segment)
        ret = xmlStrcat(ret, segment);
        ret = xmlStrcat(ret, BAD_CAST ":");
        xmlFree(segment);
    }

    if (uri->authority) {
        segment =
            xmlURIEscapeStr(BAD_CAST uri->authority, BAD_CAST "/?;:@");
        NULLCHK(segment)
        ret = xmlStrcat(ret, BAD_CAST "//");
        ret = xmlStrcat(ret, segment);
        xmlFree(segment);
    }

    if (uri->user) {
        segment = xmlURIEscapeStr(BAD_CAST uri->user, BAD_CAST ";:&=+$,");
        NULLCHK(segment)
        ret = xmlStrcat(ret,BAD_CAST "//");
        ret = xmlStrcat(ret, segment);
        ret = xmlStrcat(ret, BAD_CAST "@");
        xmlFree(segment);
    }

    if (uri->server) {
        segment = xmlURIEscapeStr(BAD_CAST uri->server, BAD_CAST "/?;:@");
        NULLCHK(segment)
        if (uri->user == NULL)
            ret = xmlStrcat(ret, BAD_CAST "//");
        ret = xmlStrcat(ret, segment);
        xmlFree(segment);
    }

    if (uri->port > 0) {
        xmlChar port[11];

        snprintf((char *) port, 11, "%d", uri->port);
        ret = xmlStrcat(ret, BAD_CAST ":");
        ret = xmlStrcat(ret, port);
    }

    if (uri->path) {
        segment =
            xmlURIEscapeStr(BAD_CAST uri->path, BAD_CAST ":@&=+$,/?;");
        NULLCHK(segment)
        ret = xmlStrcat(ret, segment);
        xmlFree(segment);
    }

    if (uri->query_raw) {
        ret = xmlStrcat(ret, BAD_CAST "?");
        ret = xmlStrcat(ret, BAD_CAST uri->query_raw);
    }
    else if (uri->query) {
        segment =
            xmlURIEscapeStr(BAD_CAST uri->query, BAD_CAST ";/?:@&=+,$");
        NULLCHK(segment)
        ret = xmlStrcat(ret, BAD_CAST "?");
        ret = xmlStrcat(ret, segment);
        xmlFree(segment);
    }

    if (uri->opaque) {
        segment = xmlURIEscapeStr(BAD_CAST uri->opaque, BAD_CAST "");
        NULLCHK(segment)
        ret = xmlStrcat(ret, segment);
        xmlFree(segment);
    }

    if (uri->fragment) {
        segment = xmlURIEscapeStr(BAD_CAST uri->fragment, BAD_CAST "#");
        NULLCHK(segment)
        ret = xmlStrcat(ret, BAD_CAST "#");
        ret = xmlStrcat(ret, segment);
        xmlFree(segment);
    }

    xmlFreeURI(uri);
#undef NULLCHK

    return (ret);
}

/************************************************************************
 *									*
 *			Public functions				*
 *									*
 ************************************************************************/

static int
xmlIsAbsolutePath(const xmlChar *path) {
    int c = path[0];

    if (xmlIsPathSeparator(c, 1))
        return(1);

#if defined(_WIN32) || defined(__CYGWIN__)
    if ((((c >= 'A') && (c <= 'Z')) ||
         ((c >= 'a') && (c <= 'z'))) &&
        (path[1] == ':'))
        return(1);
#endif

    return(0);
}

/**
 * Resolves a filesystem path from a base path.
 *
 * @param escRef  the filesystem path
 * @param base  the base value
 * @param out  pointer to result URI
 * @returns 0 on success, -1 if a memory allocation failed or an error
 * code if URI or base are invalid.
 */
static int
xmlResolvePath(const xmlChar *escRef, const xmlChar *base, xmlChar **out) {
    const xmlChar *fragment;
    xmlChar *tmp = NULL;
    xmlChar *ref = NULL;
    xmlChar *result = NULL;
    int ret = -1;
    int i;

    if (out == NULL)
        return(1);
    *out = NULL;

    if ((escRef == NULL) || (escRef[0] == 0)) {
        if ((base == NULL) || (base[0] == 0))
            return(1);
        ref = xmlStrdup(base);
        if (ref == NULL)
            goto err_memory;
        *out = ref;
        return(0);
    }

    /*
     * If a URI is resolved, we can assume it is a valid URI and not
     * a filesystem path. This means we have to unescape the part
     * before the fragment.
     */
    fragment = xmlStrchr(escRef, '#');
    if (fragment != NULL) {
        tmp = xmlStrndup(escRef, fragment - escRef);
        if (tmp == NULL)
            goto err_memory;
        escRef = tmp;
    }

    ref = (xmlChar *) xmlURIUnescapeString((char *) escRef, -1, NULL);
    if (ref == NULL)
        goto err_memory;

    if ((base == NULL) || (base[0] == 0))
        goto done;

    if (xmlIsAbsolutePath(ref))
        goto done;

    /*
     * Remove last segment from base
     */
    i = xmlStrlen(base);
    while ((i > 0) && !xmlIsPathSeparator(base[i-1], 1))
        i--;

    /*
     * Concatenate base and ref
     */
    if (i > 0) {
        int refLen = xmlStrlen(ref);

        result = xmlMalloc(i + refLen + 1);
        if (result == NULL)
            goto err_memory;

        memcpy(result, base, i);
        memcpy(result + i, ref, refLen + 1);
    }

    /*
     * Normalize
     */
    xmlNormalizePath((char *) result, 1);

done:
    if (result == NULL) {
        result = ref;
        ref = NULL;
    }

    if (fragment != NULL) {
        result = xmlStrcat(result, fragment);
        if (result == NULL)
            goto err_memory;
    }

    *out = result;
    ret = 0;

err_memory:
    xmlFree(tmp);
    xmlFree(ref);
    return(ret);
}

/**
 * Computes he final URI of the reference done by checking that
 * the given URI is valid, and building the final URI using the
 * base URI. This is processed according to section 5.2 of the
 * RFC 2396
 *
 * 5.2. Resolving Relative References to Absolute Form
 *
 * @since 2.13.0
 *
 * @param URI  the URI instance found in the document
 * @param base  the base value
 * @param valPtr  pointer to result URI
 * @returns 0 on success, -1 if a memory allocation failed or an error
 * code if URI or base are invalid.
 */
int
xmlBuildURISafe(const xmlChar *URI, const xmlChar *base, xmlChar **valPtr) {
    xmlChar *val = NULL;
    int ret, len, indx, cur, out;
    xmlURIPtr ref = NULL;
    xmlURIPtr bas = NULL;
    xmlURIPtr res = NULL;

    if (valPtr == NULL)
        return(1);
    *valPtr = NULL;

    if (URI == NULL)
        return(1);

    if (base == NULL) {
        val = xmlStrdup(URI);
        if (val == NULL)
            return(-1);
        *valPtr = val;
        return(0);
    }

    /*
     * 1) The URI reference is parsed into the potential four components and
     *    fragment identifier, as described in Section 4.3.
     *
     *    NOTE that a completely empty URI is treated by modern browsers
     *    as a reference to "." rather than as a synonym for the current
     *    URI.  Should we do that here?
     */
    if (URI[0] != 0)
        ret = xmlParseURISafe((const char *) URI, &ref);
    else
        ret = 0;
    if (ret != 0)
	goto done;
    if ((ref != NULL) && (ref->scheme != NULL)) {
	/*
	 * The URI is absolute don't modify.
	 */
	val = xmlStrdup(URI);
        if (val == NULL)
            ret = -1;
	goto done;
    }

    /*
     * If base has no scheme or authority, it is assumed to be a
     * filesystem path.
     */
    if (xmlStrstr(base, BAD_CAST "://") == NULL) {
        xmlFreeURI(ref);
        return(xmlResolvePath(URI, base, valPtr));
    }

#if defined(_WIN32) || defined(__CYGWIN__)
    /*
     * Resolve paths with a Windows drive letter as filesystem path
     * even if base has a scheme.
     */
    if ((ref != NULL) && (ref->path != NULL)) {
        int c = ref->path[0];

        if ((((c >= 'A') && (c <= 'Z')) ||
             ((c >= 'a') && (c <= 'z'))) &&
            (ref->path[1] == ':')) {
            xmlFreeURI(ref);
            return(xmlResolvePath(URI, base, valPtr));
        }
    }
#endif

    ret = xmlParseURISafe((const char *) base, &bas);
    if (ret < 0)
        goto done;
    if (ret != 0) {
	if (ref) {
            ret = 0;
	    val = xmlSaveUri(ref);
            if (val == NULL)
                ret = -1;
        }
	goto done;
    }
    if (ref == NULL) {
	/*
	 * the base fragment must be ignored
	 */
	if (bas->fragment != NULL) {
	    xmlFree(bas->fragment);
	    bas->fragment = NULL;
	}
	val = xmlSaveUri(bas);
        if (val == NULL)
            ret = -1;
	goto done;
    }

    /*
     * 2) If the path component is empty and the scheme, authority, and
     *    query components are undefined, then it is a reference to the
     *    current document and we are done.  Otherwise, the reference URI's
     *    query and fragment components are defined as found (or not found)
     *    within the URI reference and not inherited from the base URI.
     *
     *    NOTE that in modern browsers, the parsing differs from the above
     *    in the following aspect:  the query component is allowed to be
     *    defined while still treating this as a reference to the current
     *    document.
     */
    ret = -1;
    res = xmlCreateURI();
    if (res == NULL)
	goto done;
    if ((ref->scheme == NULL) && (ref->path == NULL) &&
	((ref->authority == NULL) && (ref->server == NULL) &&
         (ref->port == PORT_EMPTY))) {
	if (bas->scheme != NULL) {
	    res->scheme = xmlMemStrdup(bas->scheme);
            if (res->scheme == NULL)
                goto done;
        }
	if (bas->authority != NULL) {
	    res->authority = xmlMemStrdup(bas->authority);
            if (res->authority == NULL)
                goto done;
        } else {
	    if (bas->server != NULL) {
		res->server = xmlMemStrdup(bas->server);
                if (res->server == NULL)
                    goto done;
            }
	    if (bas->user != NULL) {
		res->user = xmlMemStrdup(bas->user);
                if (res->user == NULL)
                    goto done;
            }
	    res->port = bas->port;
	}
	if (bas->path != NULL) {
	    res->path = xmlMemStrdup(bas->path);
            if (res->path == NULL)
                goto done;
        }
	if (ref->query_raw != NULL) {
	    res->query_raw = xmlMemStrdup (ref->query_raw);
            if (res->query_raw == NULL)
                goto done;
        } else if (ref->query != NULL) {
	    res->query = xmlMemStrdup(ref->query);
            if (res->query == NULL)
                goto done;
        } else if (bas->query_raw != NULL) {
	    res->query_raw = xmlMemStrdup(bas->query_raw);
            if (res->query_raw == NULL)
                goto done;
        } else if (bas->query != NULL) {
	    res->query = xmlMemStrdup(bas->query);
            if (res->query == NULL)
                goto done;
        }
	if (ref->fragment != NULL) {
	    res->fragment = xmlMemStrdup(ref->fragment);
            if (res->fragment == NULL)
                goto done;
        }
	goto step_7;
    }

    /*
     * 3) If the scheme component is defined, indicating that the reference
     *    starts with a scheme name, then the reference is interpreted as an
     *    absolute URI and we are done.  Otherwise, the reference URI's
     *    scheme is inherited from the base URI's scheme component.
     */
    if (ref->scheme != NULL) {
	val = xmlSaveUri(ref);
        if (val != NULL)
            ret = 0;
	goto done;
    }
    if (bas->scheme != NULL) {
	res->scheme = xmlMemStrdup(bas->scheme);
        if (res->scheme == NULL)
            goto done;
    }

    if (ref->query_raw != NULL) {
	res->query_raw = xmlMemStrdup(ref->query_raw);
        if (res->query_raw == NULL)
            goto done;
    } else if (ref->query != NULL) {
	res->query = xmlMemStrdup(ref->query);
        if (res->query == NULL)
            goto done;
    }
    if (ref->fragment != NULL) {
	res->fragment = xmlMemStrdup(ref->fragment);
        if (res->fragment == NULL)
            goto done;
    }

    /*
     * 4) If the authority component is defined, then the reference is a
     *    network-path and we skip to step 7.  Otherwise, the reference
     *    URI's authority is inherited from the base URI's authority
     *    component, which will also be undefined if the URI scheme does not
     *    use an authority component.
     */
    if ((ref->authority != NULL) || (ref->server != NULL) ||
         (ref->port != PORT_EMPTY)) {
	if (ref->authority != NULL) {
	    res->authority = xmlMemStrdup(ref->authority);
            if (res->authority == NULL)
                goto done;
        } else {
            if (ref->server != NULL) {
                res->server = xmlMemStrdup(ref->server);
                if (res->server == NULL)
                    goto done;
            }
	    if (ref->user != NULL) {
		res->user = xmlMemStrdup(ref->user);
                if (res->user == NULL)
                    goto done;
            }
            res->port = ref->port;
	}
	if (ref->path != NULL) {
	    res->path = xmlMemStrdup(ref->path);
            if (res->path == NULL)
                goto done;
        }
	goto step_7;
    }
    if (bas->authority != NULL) {
	res->authority = xmlMemStrdup(bas->authority);
        if (res->authority == NULL)
            goto done;
    } else if ((bas->server != NULL) || (bas->port != PORT_EMPTY)) {
	if (bas->server != NULL) {
	    res->server = xmlMemStrdup(bas->server);
            if (res->server == NULL)
                goto done;
        }
	if (bas->user != NULL) {
	    res->user = xmlMemStrdup(bas->user);
            if (res->user == NULL)
                goto done;
        }
	res->port = bas->port;
    }

    /*
     * 5) If the path component begins with a slash character ("/"), then
     *    the reference is an absolute-path and we skip to step 7.
     */
    if ((ref->path != NULL) && (ref->path[0] == '/')) {
	res->path = xmlMemStrdup(ref->path);
        if (res->path == NULL)
            goto done;
	goto step_7;
    }


    /*
     * 6) If this step is reached, then we are resolving a relative-path
     *    reference.  The relative path needs to be merged with the base
     *    URI's path.  Although there are many ways to do this, we will
     *    describe a simple method using a separate string buffer.
     *
     * Allocate a buffer large enough for the result string.
     */
    len = 2; /* extra / and 0 */
    if (ref->path != NULL)
	len += strlen(ref->path);
    if (bas->path != NULL)
	len += strlen(bas->path);
    res->path = xmlMalloc(len);
    if (res->path == NULL)
	goto done;
    res->path[0] = 0;

    /*
     * a) All but the last segment of the base URI's path component is
     *    copied to the buffer.  In other words, any characters after the
     *    last (right-most) slash character, if any, are excluded.
     */
    cur = 0;
    out = 0;
    if (bas->path != NULL) {
	while (bas->path[cur] != 0) {
	    while ((bas->path[cur] != 0) && (bas->path[cur] != '/'))
		cur++;
	    if (bas->path[cur] == 0)
		break;

	    cur++;
	    while (out < cur) {
		res->path[out] = bas->path[out];
		out++;
	    }
	}
    }
    res->path[out] = 0;

    /*
     * b) The reference's path component is appended to the buffer
     *    string.
     */
    if (ref->path != NULL && ref->path[0] != 0) {
	indx = 0;
	/*
	 * Ensure the path includes a '/'
	 */
	if ((out == 0) && ((bas->server != NULL) || bas->port != PORT_EMPTY))
	    res->path[out++] = '/';
	while (ref->path[indx] != 0) {
	    res->path[out++] = ref->path[indx++];
	}
    }
    res->path[out] = 0;

    /*
     * Steps c) to h) are really path normalization steps
     */
    xmlNormalizeURIPath(res->path);

step_7:

    /*
     * 7) The resulting URI components, including any inherited from the
     *    base URI, are recombined to give the absolute form of the URI
     *    reference.
     */
    val = xmlSaveUri(res);
    if (val != NULL)
        ret = 0;

done:
    if (ref != NULL)
	xmlFreeURI(ref);
    if (bas != NULL)
	xmlFreeURI(bas);
    if (res != NULL)
	xmlFreeURI(res);
    *valPtr = val;
    return(ret);
}

/**
 * Computes he final URI of the reference done by checking that
 * the given URI is valid, and building the final URI using the
 * base URI. This is processed according to section 5.2 of the
 * RFC 2396
 *
 * 5.2. Resolving Relative References to Absolute Form
 *
 * @param URI  the URI instance found in the document
 * @param base  the base value
 * @returns a new URI string (to be freed by the caller) or NULL in case
 *         of error.
 */
xmlChar *
xmlBuildURI(const xmlChar *URI, const xmlChar *base) {
    xmlChar *out;

    xmlBuildURISafe(URI, base, &out);
    return(out);
}

static int
xmlParseUriOrPath(const char *str, xmlURIPtr *out, int *drive) {
    xmlURIPtr uri;
    char *buf = NULL;
    int ret;

    *out = NULL;
    *drive = 0;

    uri = xmlCreateURI();
    if (uri == NULL) {
        ret = -1;
	goto done;
    }

    if (xmlStrstr(BAD_CAST str, BAD_CAST "://") == NULL) {
        const char *path;
        size_t pathSize;
        int prependSlash = 0;

        buf = xmlMemStrdup(str);
        if (buf == NULL) {
            ret = -1;
            goto done;
        }
        xmlNormalizePath(buf, /* isFile */ 1);

        path = buf;

        if (xmlIsAbsolutePath(BAD_CAST buf)) {
#if defined(_WIN32) || defined(__CYGWIN__)
            const char *server = NULL;
            int isFileScheme = 0;
#endif

#if defined(_WIN32) || defined(__CYGWIN__)
            if (strncmp(buf, "//?/UNC/", 8) == 0) {
                server = buf + 8;
                isFileScheme = 1;
            } else if (strncmp(buf, "//?/", 4) == 0) {
                path = buf + 3;
                isFileScheme = 1;
            } else if (strncmp(buf, "//", 2) == 0) {
                server = buf + 2;
                isFileScheme = 1;
            }

            if (server != NULL) {
                const char *end = strchr(server, '/');

                if (end == NULL) {
                    uri->server = xmlMemStrdup(server);
                    path = "/";
                } else {
                    uri->server = (char *) xmlStrndup(BAD_CAST server,
                                                      end - server);
                    path = end;
                }
                if (uri->server == NULL) {
                    ret = -1;
                    goto done;
                }
            }

            if ((((path[0] >= 'A') && (path[0] <= 'Z')) ||
                 ((path[0] >= 'a') && (path[0] <= 'z'))) &&
                (path[1] == ':')) {
                prependSlash = 1;
                isFileScheme = 1;
            }

            if (isFileScheme) {
                uri->scheme = xmlMemStrdup("file");
                if (uri->scheme == NULL) {
                    ret = -1;
                    goto done;
                }

                if (uri->server == NULL)
                    uri->port = PORT_EMPTY_SERVER;
            }
#endif
        }

        pathSize = strlen(path);
        uri->path = xmlMalloc(pathSize + prependSlash + 1);
        if (uri->path == NULL) {
            ret = -1;
            goto done;
        }
        if (prependSlash) {
            uri->path[0] = '/';
            memcpy(uri->path + 1, path, pathSize + 1);
        } else {
            memcpy(uri->path, path, pathSize + 1);
        }
    } else {
	ret = xmlParseURIReference(uri, str);
	if (ret != 0)
	    goto done;

        xmlNormalizePath(uri->path, /* isFile */ 0);
    }

#if defined(_WIN32) || defined(__CYGWIN__)
    if ((uri->path[0] == '/') &&
        (((uri->path[1] >= 'A') && (uri->path[1] <= 'Z')) ||
         ((uri->path[1] >= 'a') && (uri->path[1] <= 'z'))) &&
        (uri->path[2] == ':'))
        *drive = uri->path[1];
#endif

    *out = uri;
    uri = NULL;
    ret = 0;

done:
    xmlFreeURI(uri);
    xmlFree(buf);

    return(ret);
}

/**
 * Expresses the URI of the reference in terms relative to the
 * base. Some examples of this operation include:
 *
 *     base = "http://site1.com/docs/book1.html"
 *        URI input                        URI returned
 *     http://site1.com/docs/pic1.gif   pic1.gif
 *     http://site2.com/docs/pic1.gif   http://site2.com/docs/pic1.gif
 *
 *     base = "docs/book1.html"
 *        URI input                        URI returned
 *     docs/pic1.gif                    pic1.gif
 *     docs/img/pic1.gif                img/pic1.gif
 *     img/pic1.gif                     ../img/pic1.gif
 *     http://site1.com/docs/pic1.gif   http://site1.com/docs/pic1.gif
 *
 * @since 2.13.0
 *
 * @param URI  the URI reference under consideration
 * @param base  the base value
 * @param valPtr  pointer to result URI
 * @returns 0 on success, -1 if a memory allocation failed or an error
 * code if URI or base are invalid.
 */
int
xmlBuildRelativeURISafe(const xmlChar * URI, const xmlChar * base,
                        xmlChar **valPtr)
{
    xmlChar *val = NULL;
    int ret = 0;
    int ix;
    int nbslash = 0;
    int len;
    xmlURIPtr ref = NULL;
    xmlURIPtr bas = NULL;
    const xmlChar *bptr, *uptr, *rptr;
    xmlChar *vptr;
    int remove_path = 0;
    int refDrive, baseDrive;

    if (valPtr == NULL)
        return(1);
    *valPtr = NULL;
    if ((URI == NULL) || (*URI == 0))
	return(1);

    ret = xmlParseUriOrPath((char *) URI, &ref, &refDrive);
    if (ret < 0)
        goto done;
    if (ret != 0) {
        /* Return URI if URI is invalid */
        ret = 0;
        val = xmlStrdup(URI);
        if (val == NULL)
            ret = -1;
        goto done;
    }

    /* Return URI if base is empty */
    if ((base == NULL) || (*base == 0))
        goto done;

    ret = xmlParseUriOrPath((char *) base, &bas, &baseDrive);
    if (ret < 0)
        goto done;
    if (ret != 0) {
        /* Return URI if base is invalid */
        ret = 0;
        goto done;
    }

    /*
     * If the scheme / server on the URI differs from the base,
     * just return the URI
     */
    if ((xmlStrcmp ((xmlChar *)bas->scheme, (xmlChar *)ref->scheme)) ||
	(xmlStrcmp ((xmlChar *)bas->server, (xmlChar *)ref->server)) ||
        (bas->port != ref->port) ||
        (baseDrive != refDrive)) {
	goto done;
    }
    if (xmlStrEqual((xmlChar *)bas->path, (xmlChar *)ref->path)) {
	val = xmlStrdup(BAD_CAST "");
        if (val == NULL)
            ret = -1;
	goto done;
    }
    if (bas->path == NULL) {
	val = xmlStrdup((xmlChar *)ref->path);
        if (val == NULL) {
            ret = -1;
            goto done;
        }
	goto escape;
    }
    if (ref->path == NULL) {
        ref->path = (char *) "/";
	remove_path = 1;
    }

    bptr = (xmlChar *) bas->path;
    rptr = (xmlChar *) ref->path;

    /*
     * Return URI if URI and base aren't both absolute or relative.
     */
    if ((bptr[0] == '/') != (rptr[0] == '/'))
        goto done;

    /*
     * At this point we can compare the two paths
     */
    {
        int pos = 0;

        /*
         * Next we compare the two strings and find where they first differ
         */
	while ((bptr[pos] == rptr[pos]) && (bptr[pos] != 0))
	    pos++;

	if (bptr[pos] == rptr[pos]) {
	    val = xmlStrdup(BAD_CAST "");
            if (val == NULL)
                ret = -1;
	    goto done;		/* (I can't imagine why anyone would do this) */
	}

	/*
	 * In URI, "back up" to the last '/' encountered.  This will be the
	 * beginning of the "unique" suffix of URI
	 */
	ix = pos;
	for (; ix > 0; ix--) {
	    if (rptr[ix - 1] == '/')
		break;
	}
	uptr = (xmlChar *)&rptr[ix];

	/*
	 * In base, count the number of '/' from the differing point
	 */
	for (; bptr[ix] != 0; ix++) {
	    if (bptr[ix] == '/')
		nbslash++;
	}

	/*
	 * e.g: URI="foo/" base="foo/bar" -> "./"
	 */
	if (nbslash == 0 && !uptr[0]) {
	    val = xmlStrdup(BAD_CAST "./");
            if (val == NULL)
                ret = -1;
	    goto done;
	}

	len = xmlStrlen (uptr) + 1;
    }

    if (nbslash == 0) {
	if (uptr != NULL) {
	    /* exception characters from xmlSaveUri */
	    val = xmlURIEscapeStr(uptr, BAD_CAST "/;&=+$,");
            if (val == NULL)
                ret = -1;
        }
	goto done;
    }

    /*
     * Allocate just enough space for the returned string -
     * length of the remainder of the URI, plus enough space
     * for the "../" groups, plus one for the terminator
     */
    val = (xmlChar *) xmlMalloc (len + 3 * nbslash);
    if (val == NULL) {
        ret = -1;
	goto done;
    }
    vptr = val;
    /*
     * Put in as many "../" as needed
     */
    for (; nbslash>0; nbslash--) {
	*vptr++ = '.';
	*vptr++ = '.';
	*vptr++ = '/';
    }
    /*
     * Finish up with the end of the URI
     */
    if (uptr != NULL) {
        if ((vptr > val) && (len > 0) &&
	    (uptr[0] == '/') && (vptr[-1] == '/')) {
	    memcpy (vptr, uptr + 1, len - 1);
	    vptr[len - 2] = 0;
	} else {
	    memcpy (vptr, uptr, len);
	    vptr[len - 1] = 0;
	}
    } else {
	vptr[len - 1] = 0;
    }

escape:
    /* escape the freshly-built path */
    vptr = val;
    /* exception characters from xmlSaveUri */
    val = xmlURIEscapeStr(vptr, BAD_CAST "/;&=+$,");
    if (val == NULL)
        ret = -1;
    else
        ret = 0;
    xmlFree(vptr);

done:
    if ((ret == 0) && (val == NULL)) {
        val = xmlSaveUri(ref);
        if (val == NULL)
            ret = -1;
    }

    /*
     * Free the working variables
     */
    if (remove_path != 0)
        ref->path = NULL;
    if (ref != NULL)
	xmlFreeURI (ref);
    if (bas != NULL)
	xmlFreeURI (bas);
    if (ret != 0) {
        xmlFree(val);
        val = NULL;
    }

    *valPtr = val;
    return(ret);
}

/**
 * See #xmlBuildRelativeURISafe.
 *
 * @param URI  the URI reference under consideration
 * @param base  the base value
 * @returns a new URI string (to be freed by the caller) or NULL in case
 * error.
 */
xmlChar *
xmlBuildRelativeURI(const xmlChar * URI, const xmlChar * base)
{
    xmlChar *val;

    xmlBuildRelativeURISafe(URI, base, &val);
    return(val);
}

/**
 * Prepares a path.
 *
 * If the path contains the substring "://", it is considered a
 * Legacy Extended IRI. Characters which aren't allowed in URIs are
 * escaped.
 *
 * Otherwise, the path is considered a filesystem path which is
 * copied without modification.
 *
 * The caller is responsible for freeing the memory occupied
 * by the returned string. If there is insufficient memory available, or the
 * argument is NULL, the function returns NULL.
 *
 * @param path  the resource locator in a filesystem notation
 * @returns the escaped path.
 */
xmlChar *
xmlCanonicPath(const xmlChar *path)
{
    xmlChar *ret;

    if (path == NULL)
	return(NULL);

    /* Check if this is an "absolute uri" */
    if (xmlStrstr(path, BAD_CAST "://") != NULL) {
	/*
         * Escape all characters except reserved, unreserved and the
         * percent sign.
         *
         * xmlURIEscapeStr already keeps unreserved characters, so we
         * pass gen-delims, sub-delims and "%" to ignore.
         */
        ret = xmlURIEscapeStr(path, BAD_CAST ":/?#[]@!$&()*+,;='%");
    } else {
        ret = xmlStrdup((const xmlChar *) path);
    }

    return(ret);
}

/**
 * Constructs an URI expressing the existing path
 *
 * @param path  the resource locator in a filesystem notation
 * @returns a new URI, or a duplicate of the path parameter if the
 * construction fails. The caller is responsible for freeing the memory
 * occupied by the returned string. If there is insufficient memory available,
 * or the argument is NULL, the function returns NULL.
 */
xmlChar *
xmlPathToURI(const xmlChar *path)
{
    return(xmlCanonicPath(path));
}

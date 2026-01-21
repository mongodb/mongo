/*
 * encoding.c : implements the encoding conversion functions needed for XML
 *
 * Related specs:
 * rfc2044        (UTF-8 and UTF-16) F. Yergeau Alis Technologies
 * rfc2781        UTF-16, an encoding of ISO 10646, P. Hoffman, F. Yergeau
 * [ISO-10646]    UTF-8 and UTF-16 in Annexes
 * [ISO-8859-1]   ISO Latin-1 characters codes.
 * [UNICODE]      The Unicode Consortium, "The Unicode Standard --
 *                Worldwide Character Encoding -- Version 1.0", Addison-
 *                Wesley, Volume 1, 1991, Volume 2, 1992.  UTF-8 is
 *                described in Unicode Technical Report \#4.
 * [US-ASCII]     Coded Character Set--7-bit American Standard Code for
 *                Information Interchange, ANSI X3.4-1986.
 *
 * See Copyright for the status of this software.
 *
 * Author: Daniel Veillard
 *
 * Original code for IsoLatin1 and UTF-16 by Martin J. Duerst
 */

#define IN_LIBXML
#include "libxml.h"

#include <string.h>
#include <limits.h>
#include <ctype.h>
#include <stdlib.h>

#ifdef LIBXML_ICONV_ENABLED
#include <iconv.h>
#include <errno.h>
#endif

#include <libxml/encoding.h>
#include <libxml/xmlmemory.h>
#include <libxml/parser.h>
#ifdef LIBXML_HTML_ENABLED
#include <libxml/HTMLparser.h>
#endif
#include <libxml/xmlerror.h>

#include "private/buf.h"
#include "private/enc.h"
#include "private/error.h"
#include "private/memory.h"

#ifdef LIBXML_ICU_ENABLED
#include <unicode/ucnv.h>
#endif

#define XML_HANDLER_STATIC (1 << 0)
#define XML_HANDLER_LEGACY (1 << 1)

typedef struct _xmlCharEncodingAlias xmlCharEncodingAlias;
typedef xmlCharEncodingAlias *xmlCharEncodingAliasPtr;
struct _xmlCharEncodingAlias {
    const char *name;
    const char *alias;
};

static xmlCharEncodingAliasPtr xmlCharEncodingAliases = NULL;
static int xmlCharEncodingAliasesNb = 0;
static int xmlCharEncodingAliasesMax = 0;

static int xmlLittleEndian = 1;

typedef struct {
    const char *name;
    xmlCharEncoding enc;
} xmlEncTableEntry;

static const xmlEncTableEntry xmlEncTable[] = {
    { "ansi_x3.4-1968", XML_CHAR_ENCODING_ASCII },
    { "arabic", XML_CHAR_ENCODING_8859_6 },
    { "ascii", XML_CHAR_ENCODING_ASCII },
    { "asmo-708", XML_CHAR_ENCODING_8859_6 },
    { "cp1252", XML_CHAR_ENCODING_WINDOWS_1252 },
    { "cp819", XML_CHAR_ENCODING_8859_1 },
    { "cseucpkdfmtjapanese", XML_CHAR_ENCODING_EUC_JP },
    { "csiso2022jp", XML_CHAR_ENCODING_2022_JP },
    { "csiso88596e", XML_CHAR_ENCODING_8859_6 },
    { "csiso88596i", XML_CHAR_ENCODING_8859_6 },
    { "csiso88598e", XML_CHAR_ENCODING_8859_8 },
    { "csiso88598i", XML_CHAR_ENCODING_8859_8 },
    { "csisolatin1", XML_CHAR_ENCODING_8859_1 },
    { "csisolatin2", XML_CHAR_ENCODING_8859_2 },
    { "csisolatin3", XML_CHAR_ENCODING_8859_3 },
    { "csisolatin4", XML_CHAR_ENCODING_8859_4 },
    { "csisolatin5", XML_CHAR_ENCODING_8859_9 },
    { "csisolatin6", XML_CHAR_ENCODING_8859_10 },
    { "csisolatin9", XML_CHAR_ENCODING_8859_15 },
    { "csisolatinarabic", XML_CHAR_ENCODING_8859_6 },
    { "csisolatincyrillic", XML_CHAR_ENCODING_8859_5 },
    { "csisolatingreek", XML_CHAR_ENCODING_8859_7 },
    { "csisolatinhebrew", XML_CHAR_ENCODING_8859_8 },
    { "csshiftjis", XML_CHAR_ENCODING_SHIFT_JIS },
    { "csunicode", XML_CHAR_ENCODING_UTF16 },
    { "cyrillic", XML_CHAR_ENCODING_8859_5 },
    { "ecma-114", XML_CHAR_ENCODING_8859_6 },
    { "ecma-118", XML_CHAR_ENCODING_8859_7 },
    { "elot_928", XML_CHAR_ENCODING_8859_7 },
    { "euc-jp", XML_CHAR_ENCODING_EUC_JP },
    { "greek", XML_CHAR_ENCODING_8859_7 },
    { "greek8", XML_CHAR_ENCODING_8859_7 },
    { "html", XML_CHAR_ENCODING_HTML },
    { "ibm819", XML_CHAR_ENCODING_8859_1 },
    { "iso latin 1", XML_CHAR_ENCODING_8859_1 },
    { "iso latin 2", XML_CHAR_ENCODING_8859_2 },
    { "iso-10646-ucs-2", XML_CHAR_ENCODING_UCS2 },
    { "iso-10646-ucs-4", XML_CHAR_ENCODING_UCS4LE },
    { "iso-2022-jp", XML_CHAR_ENCODING_2022_JP },
    { "iso-8859-1", XML_CHAR_ENCODING_8859_1 },
    { "iso-8859-10", XML_CHAR_ENCODING_8859_10 },
    { "iso-8859-11", XML_CHAR_ENCODING_8859_11 },
    { "iso-8859-13", XML_CHAR_ENCODING_8859_13 },
    { "iso-8859-14", XML_CHAR_ENCODING_8859_14 },
    { "iso-8859-15", XML_CHAR_ENCODING_8859_15 },
    { "iso-8859-16", XML_CHAR_ENCODING_8859_16 },
    { "iso-8859-2", XML_CHAR_ENCODING_8859_2 },
    { "iso-8859-3", XML_CHAR_ENCODING_8859_3 },
    { "iso-8859-4", XML_CHAR_ENCODING_8859_4 },
    { "iso-8859-5", XML_CHAR_ENCODING_8859_5 },
    { "iso-8859-6", XML_CHAR_ENCODING_8859_6 },
    { "iso-8859-6-e", XML_CHAR_ENCODING_8859_6 },
    { "iso-8859-6-i", XML_CHAR_ENCODING_8859_6 },
    { "iso-8859-7", XML_CHAR_ENCODING_8859_7 },
    { "iso-8859-8", XML_CHAR_ENCODING_8859_8 },
    { "iso-8859-8-i", XML_CHAR_ENCODING_8859_8 },
    { "iso-8859-9", XML_CHAR_ENCODING_8859_9 },
    { "iso-ir-100", XML_CHAR_ENCODING_8859_1 },
    { "iso-ir-101", XML_CHAR_ENCODING_8859_2 },
    { "iso-ir-109", XML_CHAR_ENCODING_8859_3 },
    { "iso-ir-110", XML_CHAR_ENCODING_8859_4 },
    { "iso-ir-126", XML_CHAR_ENCODING_8859_7 },
    { "iso-ir-127", XML_CHAR_ENCODING_8859_6 },
    { "iso-ir-138", XML_CHAR_ENCODING_8859_8 },
    { "iso-ir-144", XML_CHAR_ENCODING_8859_5 },
    { "iso-ir-148", XML_CHAR_ENCODING_8859_9 },
    { "iso-ir-157", XML_CHAR_ENCODING_8859_10 },
    { "iso-latin-1", XML_CHAR_ENCODING_8859_1 },
    { "iso-latin-2", XML_CHAR_ENCODING_8859_2 },
    { "iso8859-1", XML_CHAR_ENCODING_8859_1 },
    { "iso8859-10", XML_CHAR_ENCODING_8859_1 },
    { "iso8859-13", XML_CHAR_ENCODING_8859_1 },
    { "iso8859-14", XML_CHAR_ENCODING_8859_1 },
    { "iso8859-15", XML_CHAR_ENCODING_8859_1 },
    { "iso8859-2", XML_CHAR_ENCODING_8859_2 },
    { "iso8859-3", XML_CHAR_ENCODING_8859_3 },
    { "iso8859-4", XML_CHAR_ENCODING_8859_4 },
    { "iso8859-5", XML_CHAR_ENCODING_8859_5 },
    { "iso8859-6", XML_CHAR_ENCODING_8859_6 },
    { "iso8859-7", XML_CHAR_ENCODING_8859_7 },
    { "iso8859-8", XML_CHAR_ENCODING_8859_8 },
    { "iso8859-9", XML_CHAR_ENCODING_8859_9 },
    { "iso88591", XML_CHAR_ENCODING_8859_1 },
    { "iso885910", XML_CHAR_ENCODING_8859_10 },
    { "iso885913", XML_CHAR_ENCODING_8859_13 },
    { "iso885914", XML_CHAR_ENCODING_8859_14 },
    { "iso885915", XML_CHAR_ENCODING_8859_15 },
    { "iso88592", XML_CHAR_ENCODING_8859_2 },
    { "iso88593", XML_CHAR_ENCODING_8859_3 },
    { "iso88594", XML_CHAR_ENCODING_8859_4 },
    { "iso88595", XML_CHAR_ENCODING_8859_5 },
    { "iso88596", XML_CHAR_ENCODING_8859_6 },
    { "iso88597", XML_CHAR_ENCODING_8859_7 },
    { "iso88598", XML_CHAR_ENCODING_8859_8 },
    { "iso88599", XML_CHAR_ENCODING_8859_9 },
    { "iso_8859-1", XML_CHAR_ENCODING_8859_1 },
    { "iso_8859-1:1987", XML_CHAR_ENCODING_8859_1 },
    { "iso_8859-2", XML_CHAR_ENCODING_8859_2 },
    { "iso_8859-2:1987", XML_CHAR_ENCODING_8859_2 },
    { "iso_8859-3", XML_CHAR_ENCODING_8859_3 },
    { "iso_8859-3:1988", XML_CHAR_ENCODING_8859_3 },
    { "iso_8859-4", XML_CHAR_ENCODING_8859_4 },
    { "iso_8859-4:1988", XML_CHAR_ENCODING_8859_4 },
    { "iso_8859-5", XML_CHAR_ENCODING_8859_5 },
    { "iso_8859-5:1988", XML_CHAR_ENCODING_8859_5 },
    { "iso_8859-6", XML_CHAR_ENCODING_8859_6 },
    { "iso_8859-6:1987", XML_CHAR_ENCODING_8859_6 },
    { "iso_8859-7", XML_CHAR_ENCODING_8859_7 },
    { "iso_8859-7:1987", XML_CHAR_ENCODING_8859_7 },
    { "iso_8859-8", XML_CHAR_ENCODING_8859_8 },
    { "iso_8859-8:1988", XML_CHAR_ENCODING_8859_8 },
    { "iso_8859-9", XML_CHAR_ENCODING_8859_9 },
    { "iso_8859-9:1989", XML_CHAR_ENCODING_8859_9 },
    { "l1", XML_CHAR_ENCODING_8859_1 },
    { "l2", XML_CHAR_ENCODING_8859_2 },
    { "l3", XML_CHAR_ENCODING_8859_3 },
    { "l4", XML_CHAR_ENCODING_8859_4 },
    { "l5", XML_CHAR_ENCODING_8859_9 },
    { "l6", XML_CHAR_ENCODING_8859_10 },
    { "l9", XML_CHAR_ENCODING_8859_15 },
    { "latin1", XML_CHAR_ENCODING_8859_1 },
    { "latin2", XML_CHAR_ENCODING_8859_2 },
    { "latin3", XML_CHAR_ENCODING_8859_3 },
    { "latin4", XML_CHAR_ENCODING_8859_4 },
    { "latin5", XML_CHAR_ENCODING_8859_9 },
    { "latin6", XML_CHAR_ENCODING_8859_10 },
    { "logical", XML_CHAR_ENCODING_8859_8 },
    { "ms932", XML_CHAR_ENCODING_SHIFT_JIS },
    { "ms_kanji", XML_CHAR_ENCODING_SHIFT_JIS },
    { "shift-jis", XML_CHAR_ENCODING_SHIFT_JIS },
    { "shift_jis", XML_CHAR_ENCODING_SHIFT_JIS },
    { "sjis", XML_CHAR_ENCODING_SHIFT_JIS },
    { "sun_eu_greek", XML_CHAR_ENCODING_8859_7 },
    { "ucs-2", XML_CHAR_ENCODING_UCS2 },
    { "ucs-4", XML_CHAR_ENCODING_UCS4LE },
    { "ucs2", XML_CHAR_ENCODING_UCS2 },
    { "ucs4", XML_CHAR_ENCODING_UCS4LE },
    { "unicode", XML_CHAR_ENCODING_UTF16 },
    { "unicode-1-1-utf-8", XML_CHAR_ENCODING_UTF8 },
    { "unicode11utf8", XML_CHAR_ENCODING_UTF8 },
    { "unicode20utf8", XML_CHAR_ENCODING_UTF8 },
    { "unicodefffe", XML_CHAR_ENCODING_UTF16BE },
    { "unicodefeff", XML_CHAR_ENCODING_UTF16LE },
    { "us-ascii", XML_CHAR_ENCODING_ASCII },
    { "utf-16", XML_CHAR_ENCODING_UTF16 },
    { "utf-16be", XML_CHAR_ENCODING_UTF16BE },
    { "utf-16le", XML_CHAR_ENCODING_UTF16LE },
    { "utf-8", XML_CHAR_ENCODING_UTF8 },
    { "utf16", XML_CHAR_ENCODING_UTF16 },
    { "utf8", XML_CHAR_ENCODING_UTF8 },
    { "visual", XML_CHAR_ENCODING_8859_8 },
    { "windows-1252", XML_CHAR_ENCODING_WINDOWS_1252 },
    { "windows-31j", XML_CHAR_ENCODING_SHIFT_JIS },
    { "x-cp1252", XML_CHAR_ENCODING_WINDOWS_1252 },
    { "x-euc-jp", XML_CHAR_ENCODING_EUC_JP },
    { "x-sjis", XML_CHAR_ENCODING_SHIFT_JIS },
    { "x-unicode20utf8", XML_CHAR_ENCODING_UTF8 }
};

static xmlCharEncError
asciiToAscii(void *vctxt, unsigned char* out, int *outlen,
             const unsigned char* in, int *inlen, int flush);
static xmlCharEncError
UTF8ToUTF8(void *vctxt, unsigned char* out, int *outlen,
           const unsigned char* inb, int *inlenb, int flush);
static xmlCharEncError
latin1ToUTF8(void *vctxt, unsigned char* out, int *outlen,
             const unsigned char* in, int *inlen, int flush);
static xmlCharEncError
UTF16LEToUTF8(void *vctxt, unsigned char* out, int *outlen,
              const unsigned char* inb, int *inlenb, int flush);
static xmlCharEncError
UTF16BEToUTF8(void *vctxt, unsigned char* out, int *outlen,
              const unsigned char* inb, int *inlenb, int flush);

#ifdef LIBXML_OUTPUT_ENABLED

static xmlCharEncError
UTF8ToLatin1(void *vctxt, unsigned char* outb, int *outlen,
             const unsigned char* in, int *inlen, int flush);
static xmlCharEncError
UTF8ToUTF16(void *vctxt, unsigned char* outb, int *outlen,
            const unsigned char* in, int *inlen, int flush);
static xmlCharEncError
UTF8ToUTF16LE(void *vctxt, unsigned char* outb, int *outlen,
              const unsigned char* in, int *inlen, int flush);
static xmlCharEncError
UTF8ToUTF16BE(void *vctxt, unsigned char* outb, int *outlen,
              const unsigned char* in, int *inlen, int flush);

#else /* LIBXML_OUTPUT_ENABLED */

#define UTF8ToLatin1 NULL
#define UTF8ToUTF16 NULL
#define UTF8ToUTF16LE NULL
#define UTF8ToUTF16BE NULL

#endif /* LIBXML_OUTPUT_ENABLED */

#if defined(LIBXML_OUTPUT_ENABLED) && defined(LIBXML_HTML_ENABLED)
static xmlCharEncError
UTF8ToHtmlWrapper(void *vctxt, unsigned char *out, int *outlen,
                  const unsigned char *in, int *inlen, int flush);
#else
#define UTF8ToHtmlWrapper NULL
#endif

#include "codegen/charset.inc"

static xmlCharEncError
EightBitToUtf8(void *vctxt, unsigned char* out, int *outlen,
               const unsigned char* in, int *inlen, int flush);
static xmlCharEncError
Utf8ToEightBit(void *vctxt, unsigned char *out, int *outlen,
               const unsigned char *in, int *inlen, int flush);

#define MAKE_8BIT_HANDLER(name, table) \
    { (char *) name, { EightBitToUtf8 }, { Utf8ToEightBit }, \
      (void *) xmlunicodetable_##table, \
      (void *) xmltranscodetable_##table, \
      NULL, XML_HANDLER_STATIC }

#if !defined(LIBXML_ICONV_ENABLED) && !defined(LIBXML_ICU_ENABLED) && \
    defined(LIBXML_ISO8859X_ENABLED)

#define MAKE_ISO_HANDLER(name, n) MAKE_8BIT_HANDLER(name, ISO8859_##n)

#else /* LIBXML_ISO8859X_ENABLED */

#define MAKE_ISO_HANDLER(name, n) \
    { (char *) name, { NULL }, { NULL }, NULL, NULL, NULL, \
      XML_HANDLER_STATIC }

#endif /* LIBXML_ISO8859X_ENABLED */

#define MAKE_HANDLER(name, in, out) \
    { (char *) name, { in }, { out }, NULL, NULL, NULL, XML_HANDLER_STATIC }

/*
 * The layout must match enum xmlCharEncoding.
 *
 * Names should match the IANA registry if possible:
 * https://www.iana.org/assignments/character-sets/character-sets.xhtml
 */
static const xmlCharEncodingHandler defaultHandlers[32] = {
    MAKE_HANDLER(NULL, NULL, NULL), /* NONE */
    MAKE_HANDLER("UTF-8", UTF8ToUTF8, UTF8ToUTF8),
    MAKE_HANDLER("UTF-16LE", UTF16LEToUTF8, UTF8ToUTF16LE),
    MAKE_HANDLER("UTF-16BE", UTF16BEToUTF8, UTF8ToUTF16BE),
    MAKE_HANDLER("UCS-4LE", NULL, NULL),
    MAKE_HANDLER("UCS-4BE", NULL, NULL),
    MAKE_HANDLER("IBM037", NULL, NULL),
    MAKE_HANDLER(NULL, NULL, NULL), /* UCS4_2143 */
    MAKE_HANDLER(NULL, NULL, NULL), /* UCS4_3412 */
    MAKE_HANDLER("UCS-2", NULL, NULL),
    MAKE_HANDLER("ISO-8859-1", latin1ToUTF8, UTF8ToLatin1),
    MAKE_ISO_HANDLER("ISO-8859-2", 2),
    MAKE_ISO_HANDLER("ISO-8859-3", 3),
    MAKE_ISO_HANDLER("ISO-8859-4", 4),
    MAKE_ISO_HANDLER("ISO-8859-5", 5),
    MAKE_ISO_HANDLER("ISO-8859-6", 6),
    MAKE_ISO_HANDLER("ISO-8859-7", 7),
    MAKE_ISO_HANDLER("ISO-8859-8", 8),
    MAKE_ISO_HANDLER("ISO-8859-9", 9),
    MAKE_HANDLER("ISO-2022-JP", NULL, NULL),
    MAKE_HANDLER("Shift_JIS", NULL, NULL),
    MAKE_HANDLER("EUC-JP", NULL, NULL),
    MAKE_HANDLER("US-ASCII", asciiToAscii, asciiToAscii),
    MAKE_HANDLER("UTF-16", UTF16LEToUTF8, UTF8ToUTF16),
    MAKE_HANDLER("HTML", NULL, UTF8ToHtmlWrapper),
    MAKE_ISO_HANDLER("ISO-8859-10", 10),
    MAKE_ISO_HANDLER("ISO-8859-11", 11),
    MAKE_ISO_HANDLER("ISO-8859-13", 13),
    MAKE_ISO_HANDLER("ISO-8859-14", 14),
    MAKE_ISO_HANDLER("ISO-8859-15", 15),
    MAKE_ISO_HANDLER("ISO-8859-16", 16),
    MAKE_8BIT_HANDLER("windows-1252", windows_1252)
};

#define NUM_DEFAULT_HANDLERS \
    (sizeof(defaultHandlers) / sizeof(defaultHandlers[0]))

/* the size should be growable, but it's not a big deal ... */
#define MAX_ENCODING_HANDLERS 50
static xmlCharEncodingHandlerPtr *globalHandlers = NULL;
static int nbCharEncodingHandler = 0;

#ifdef LIBXML_ICONV_ENABLED
static xmlParserErrors
xmlCharEncIconv(const char *name, xmlCharEncFlags flags,
                xmlCharEncodingHandler **out);
#endif

#ifdef LIBXML_ICU_ENABLED
static xmlParserErrors
xmlCharEncUconv(const char *name, xmlCharEncFlags flags,
                xmlCharEncodingHandler **out);
#endif

/************************************************************************
 *									*
 *		Generic encoding handling routines			*
 *									*
 ************************************************************************/

/**
 * Guess the encoding of the entity using the first bytes of the entity content
 * according to the non-normative appendix F of the XML-1.0 recommendation.
 *
 * @param in  a pointer to the first bytes of the XML entity, must be at least
 *            2 bytes long (at least 4 if encoding is UTF4 variant).
 * @param len  pointer to the length of the buffer
 * @returns a xmlCharEncoding value.
 */
xmlCharEncoding
xmlDetectCharEncoding(const unsigned char* in, int len)
{
    if (in == NULL)
        return(XML_CHAR_ENCODING_NONE);
    if (len >= 4) {
	if ((in[0] == 0x00) && (in[1] == 0x00) &&
	    (in[2] == 0x00) && (in[3] == 0x3C))
	    return(XML_CHAR_ENCODING_UCS4BE);
	if ((in[0] == 0x3C) && (in[1] == 0x00) &&
	    (in[2] == 0x00) && (in[3] == 0x00))
	    return(XML_CHAR_ENCODING_UCS4LE);
	if ((in[0] == 0x4C) && (in[1] == 0x6F) &&
	    (in[2] == 0xA7) && (in[3] == 0x94))
	    return(XML_CHAR_ENCODING_EBCDIC);
	if ((in[0] == 0x3C) && (in[1] == 0x3F) &&
	    (in[2] == 0x78) && (in[3] == 0x6D))
	    return(XML_CHAR_ENCODING_UTF8);
	/*
	 * Although not part of the recommendation, we also
	 * attempt an "auto-recognition" of UTF-16LE and
	 * UTF-16BE encodings.
	 */
	if ((in[0] == 0x3C) && (in[1] == 0x00) &&
	    (in[2] == 0x3F) && (in[3] == 0x00))
	    return(XML_CHAR_ENCODING_UTF16LE);
	if ((in[0] == 0x00) && (in[1] == 0x3C) &&
	    (in[2] == 0x00) && (in[3] == 0x3F))
	    return(XML_CHAR_ENCODING_UTF16BE);
    }
    if (len >= 3) {
	/*
	 * Errata on XML-1.0 June 20 2001
	 * We now allow an UTF8 encoded BOM
	 */
	if ((in[0] == 0xEF) && (in[1] == 0xBB) &&
	    (in[2] == 0xBF))
	    return(XML_CHAR_ENCODING_UTF8);
    }
    /* For UTF-16 we can recognize by the BOM */
    if (len >= 2) {
	if ((in[0] == 0xFE) && (in[1] == 0xFF))
	    return(XML_CHAR_ENCODING_UTF16BE);
	if ((in[0] == 0xFF) && (in[1] == 0xFE))
	    return(XML_CHAR_ENCODING_UTF16LE);
    }
    return(XML_CHAR_ENCODING_NONE);
}

/**
 * Unregisters all aliases.
 *
 * @deprecated This function modifies global state and is not
 * thread-safe. See #xmlCtxtSetCharEncConvImpl for an alternative.
 *
 */
void
xmlCleanupEncodingAliases(void) {
    int i;

    if (xmlCharEncodingAliases == NULL)
	return;

    for (i = 0;i < xmlCharEncodingAliasesNb;i++) {
	if (xmlCharEncodingAliases[i].name != NULL)
	    xmlFree((char *) xmlCharEncodingAliases[i].name);
	if (xmlCharEncodingAliases[i].alias != NULL)
	    xmlFree((char *) xmlCharEncodingAliases[i].alias);
    }
    xmlCharEncodingAliasesNb = 0;
    xmlCharEncodingAliasesMax = 0;
    xmlFree(xmlCharEncodingAliases);
    xmlCharEncodingAliases = NULL;
}

/**
 * Lookup an encoding name for the given alias.
 *
 * @deprecated This function is not thread-safe.
 *
 * @param alias  the alias name as parsed, in UTF-8 format (ASCII actually)
 * @returns NULL if not found, otherwise the original name.
 */
const char *
xmlGetEncodingAlias(const char *alias) {
    int i;
    char upper[100];

    if (alias == NULL)
	return(NULL);

    if (xmlCharEncodingAliases == NULL)
	return(NULL);

    for (i = 0;i < 99;i++) {
        upper[i] = (char) toupper((unsigned char) alias[i]);
	if (upper[i] == 0) break;
    }
    upper[i] = 0;

    /*
     * Walk down the list looking for a definition of the alias
     */
    for (i = 0;i < xmlCharEncodingAliasesNb;i++) {
	if (!strcmp(xmlCharEncodingAliases[i].alias, upper)) {
	    return(xmlCharEncodingAliases[i].name);
	}
    }
    return(NULL);
}

/**
 * Registers an alias `alias` for an encoding named `name`. Existing
 * aliases will be overwritten.
 *
 * @deprecated This function modifies global state and is not
 * thread-safe. See #xmlCtxtSetCharEncConvImpl for an alternative.
 *
 * @param name  the encoding name as parsed, in UTF-8 format (ASCII actually)
 * @param alias  the alias name as parsed, in UTF-8 format (ASCII actually)
 * @returns 0 in case of success, -1 in case of error.
 */
int
xmlAddEncodingAlias(const char *name, const char *alias) {
    int i;
    char upper[100];
    char *nameCopy, *aliasCopy;

    if ((name == NULL) || (alias == NULL))
	return(-1);

    for (i = 0;i < 99;i++) {
        upper[i] = (char) toupper((unsigned char) alias[i]);
	if (upper[i] == 0) break;
    }
    upper[i] = 0;

    if (xmlCharEncodingAliasesNb >= xmlCharEncodingAliasesMax) {
        xmlCharEncodingAliasPtr tmp;
        int newSize;

        newSize = xmlGrowCapacity(xmlCharEncodingAliasesMax, sizeof(tmp[0]),
                                  20, XML_MAX_ITEMS);
        if (newSize < 0)
            return(-1);
        tmp = xmlRealloc(xmlCharEncodingAliases, newSize * sizeof(tmp[0]));
        if (tmp == NULL)
            return(-1);
        xmlCharEncodingAliases = tmp;
        xmlCharEncodingAliasesMax = newSize;
    }

    /*
     * Walk down the list looking for a definition of the alias
     */
    for (i = 0;i < xmlCharEncodingAliasesNb;i++) {
	if (!strcmp(xmlCharEncodingAliases[i].alias, upper)) {
	    /*
	     * Replace the definition.
	     */
	    nameCopy = xmlMemStrdup(name);
            if (nameCopy == NULL)
                return(-1);
	    xmlFree((char *) xmlCharEncodingAliases[i].name);
	    xmlCharEncodingAliases[i].name = nameCopy;
	    return(0);
	}
    }
    /*
     * Add the definition
     */
    nameCopy = xmlMemStrdup(name);
    if (nameCopy == NULL)
        return(-1);
    aliasCopy = xmlMemStrdup(upper);
    if (aliasCopy == NULL) {
        xmlFree(nameCopy);
        return(-1);
    }
    xmlCharEncodingAliases[xmlCharEncodingAliasesNb].name = nameCopy;
    xmlCharEncodingAliases[xmlCharEncodingAliasesNb].alias = aliasCopy;
    xmlCharEncodingAliasesNb++;
    return(0);
}

/**
 * Unregisters an encoding alias.
 *
 * @deprecated This function modifies global state and is not
 * thread-safe. See #xmlCtxtSetCharEncConvImpl for an alternative.
 *
 * @param alias  the alias name as parsed, in UTF-8 format (ASCII actually)
 * @returns 0 in case of success, -1 in case of error.
 */
int
xmlDelEncodingAlias(const char *alias) {
    int i;

    if (alias == NULL)
	return(-1);

    if (xmlCharEncodingAliases == NULL)
	return(-1);
    /*
     * Walk down the list looking for a definition of the alias
     */
    for (i = 0;i < xmlCharEncodingAliasesNb;i++) {
	if (!strcmp(xmlCharEncodingAliases[i].alias, alias)) {
	    xmlFree((char *) xmlCharEncodingAliases[i].name);
	    xmlFree((char *) xmlCharEncodingAliases[i].alias);
	    xmlCharEncodingAliasesNb--;
	    memmove(&xmlCharEncodingAliases[i], &xmlCharEncodingAliases[i + 1],
		    sizeof(xmlCharEncodingAlias) * (xmlCharEncodingAliasesNb - i));
	    return(0);
	}
    }
    return(-1);
}

static int
xmlCompareEncTableEntries(const void *vkey, const void *ventry) {
    const char *key = vkey;
    const xmlEncTableEntry *entry = ventry;

    return(xmlStrcasecmp(BAD_CAST key, BAD_CAST entry->name));
}

static xmlCharEncoding
xmlParseCharEncodingInternal(const char *name)
{
    const xmlEncTableEntry *entry;

    if (name == NULL)
       return(XML_CHAR_ENCODING_NONE);

    entry = bsearch(name, xmlEncTable,
                    sizeof(xmlEncTable) / sizeof(xmlEncTable[0]),
                    sizeof(xmlEncTable[0]), xmlCompareEncTableEntries);
    if (entry != NULL)
        return(entry->enc);

    return(XML_CHAR_ENCODING_ERROR);
}

/**
 * Compare the string to the encoding schemes already known. Note
 * that the comparison is case insensitive accordingly to the section
 * [XML] 4.3.3 Character Encoding in Entities.
 *
 * @param name  the encoding name as parsed, in UTF-8 format (ASCII actually)
 * @returns one of the xmlCharEncoding values or XML_CHAR_ENCODING_NONE
 * if not recognized.
 */
xmlCharEncoding
xmlParseCharEncoding(const char *name)
{
    xmlCharEncoding enc = xmlParseCharEncodingInternal(name);

    /* Backward compatibility */
    if (enc == XML_CHAR_ENCODING_UTF16)
        enc = XML_CHAR_ENCODING_UTF16LE;

    return(enc);
}

/**
 * The "canonical" name for XML encoding.
 * C.f. http://www.w3.org/TR/REC-xml#charencoding
 * Section 4.3.3  Character Encoding in Entities
 *
 * @param enc  the encoding
 * @returns the canonical name for the given encoding.
 */
const char*
xmlGetCharEncodingName(xmlCharEncoding enc) {
    switch (enc) {
        case XML_CHAR_ENCODING_UTF16LE:
	    return("UTF-16");
        case XML_CHAR_ENCODING_UTF16BE:
	    return("UTF-16");
        case XML_CHAR_ENCODING_UCS4LE:
            return("UCS-4");
        case XML_CHAR_ENCODING_UCS4BE:
            return("UCS-4");
        default:
            break;
    }

    if ((enc <= 0) || ((size_t) enc >= NUM_DEFAULT_HANDLERS))
        return(NULL);

    return(defaultHandlers[enc].name);
}

/************************************************************************
 *									*
 *			Char encoding handlers				*
 *									*
 ************************************************************************/

/**
 * Create and registers an xmlCharEncodingHandler.
 *
 * @deprecated This function modifies global state and is not
 * thread-safe. See #xmlCtxtSetCharEncConvImpl for an alternative.
 *
 * @param name  the encoding name, in UTF-8 format (ASCII actually)
 * @param input  the xmlCharEncodingInputFunc to read that encoding
 * @param output  the xmlCharEncodingOutputFunc to write that encoding
 * @returns the xmlCharEncodingHandler created (or NULL in case of error).
 */
xmlCharEncodingHandler *
xmlNewCharEncodingHandler(const char *name,
                          xmlCharEncodingInputFunc input,
                          xmlCharEncodingOutputFunc output) {
    xmlCharEncodingHandlerPtr handler;
    const char *alias;
    char upper[500];
    int i;
    char *up = NULL;

    /*
     * Do the alias resolution
     */
    alias = xmlGetEncodingAlias(name);
    if (alias != NULL)
	name = alias;

    /*
     * Keep only the uppercase version of the encoding.
     */
    if (name == NULL)
	return(NULL);
    for (i = 0;i < 499;i++) {
        upper[i] = (char) toupper((unsigned char) name[i]);
	if (upper[i] == 0) break;
    }
    upper[i] = 0;
    up = xmlMemStrdup(upper);
    if (up == NULL)
	return(NULL);

    /*
     * allocate and fill-up an handler block.
     */
    handler = (xmlCharEncodingHandlerPtr)
              xmlMalloc(sizeof(xmlCharEncodingHandler));
    if (handler == NULL) {
        xmlFree(up);
	return(NULL);
    }
    memset(handler, 0, sizeof(xmlCharEncodingHandler));
    handler->input.legacyFunc = input;
    handler->output.legacyFunc = output;
    handler->name = up;
    handler->flags = XML_HANDLER_STATIC | XML_HANDLER_LEGACY;

    /*
     * registers and returns the handler.
     */
    xmlRegisterCharEncodingHandler(handler);
    return(handler);
}

/**
 * Create a custom xmlCharEncodingHandler.
 *
 * @param name  the encoding name
 * @param input  input callback which converts to UTF-8
 * @param output  output callback which converts from UTF-8
 * @param ctxtDtor  context destructor
 * @param inputCtxt  context for input callback
 * @param outputCtxt  context for output callback
 * @param out  pointer to resulting handler
 * @returns an xmlParserErrors code.
 */
xmlParserErrors
xmlCharEncNewCustomHandler(const char *name,
                           xmlCharEncConvFunc input, xmlCharEncConvFunc output,
                           xmlCharEncConvCtxtDtor ctxtDtor,
                           void *inputCtxt, void *outputCtxt,
                           xmlCharEncodingHandler **out) {
    xmlCharEncodingHandler *handler;

    if (out == NULL)
        return(XML_ERR_ARGUMENT);

    handler = xmlMalloc(sizeof(*handler));
    if (handler == NULL)
        goto error;
    memset(handler, 0, sizeof(*handler));

    if (name != NULL) {
        handler->name = xmlMemStrdup(name);
        if (handler->name == NULL)
            goto error;
    }

    handler->input.func = input;
    handler->output.func = output;
    handler->ctxtDtor = ctxtDtor;
    handler->inputCtxt = inputCtxt;
    handler->outputCtxt = outputCtxt;

    *out = handler;
    return(XML_ERR_OK);

error:
    xmlFree(handler);

    if (ctxtDtor != NULL) {
        if (inputCtxt != NULL)
            ctxtDtor(inputCtxt);
        if (outputCtxt != NULL)
            ctxtDtor(outputCtxt);
    }

    return(XML_ERR_NO_MEMORY);
}

/**
 * @deprecated Alias for #xmlInitParser.
 */
void
xmlInitCharEncodingHandlers(void) {
    xmlInitParser();
}

/**
 * Initialize the char encoding support.
 */
void
xmlInitEncodingInternal(void) {
    unsigned short int tst = 0x1234;
    unsigned char *ptr = (unsigned char *) &tst;

    if (*ptr == 0x12) xmlLittleEndian = 0;
    else xmlLittleEndian = 1;
}

/**
 * Cleanup the memory allocated for the char encoding support, it
 * unregisters all the encoding handlers and the aliases.
 *
 * @deprecated This function will be made private. Call #xmlCleanupParser
 * to free global state but see the warnings there. #xmlCleanupParser
 * should be only called once at program exit. In most cases, you don't
 * have call cleanup functions at all.
 *
 */
void
xmlCleanupCharEncodingHandlers(void) {
    xmlCleanupEncodingAliases();

    if (globalHandlers == NULL) return;

    for (;nbCharEncodingHandler > 0;) {
        xmlCharEncodingHandler *handler;

        nbCharEncodingHandler--;
        handler = globalHandlers[nbCharEncodingHandler];
	if (handler != NULL) {
	    if (handler->name != NULL)
		xmlFree(handler->name);
	    xmlFree(handler);
	}
    }
    xmlFree(globalHandlers);
    globalHandlers = NULL;
    nbCharEncodingHandler = 0;
}

/**
 * Register the char encoding handler.
 *
 * @deprecated This function modifies global state and is not
 * thread-safe. See #xmlCtxtSetCharEncConvImpl for an alternative.
 *
 * @param handler  the xmlCharEncodingHandler handler block
 */
void
xmlRegisterCharEncodingHandler(xmlCharEncodingHandler *handler) {
    if (handler == NULL)
        return;
    if (globalHandlers == NULL) {
        globalHandlers = xmlMalloc(
                MAX_ENCODING_HANDLERS * sizeof(globalHandlers[0]));
        if (globalHandlers == NULL)
            goto free_handler;
    }

    if (nbCharEncodingHandler >= MAX_ENCODING_HANDLERS)
        goto free_handler;
    globalHandlers[nbCharEncodingHandler++] = handler;
    return;

free_handler:
    if (handler != NULL) {
        if (handler->name != NULL) {
            xmlFree(handler->name);
        }
        xmlFree(handler);
    }
}

/**
 * Search the non-default handlers for an exact match.
 *
 * @param norig  name of the char encoding
 * @param name  potentially aliased name of the encoding
 * @param flags  bit mask of flags
 * @param impl  a conversion implementation (optional)
 * @param implCtxt  user data for conversion implementation (optional)
 * @param out  pointer to resulting handler
 * @returns an xmlParserErrors error code.
 */
static xmlParserErrors
xmlFindExtraHandler(const char *norig, const char *name, xmlCharEncFlags flags,
                    xmlCharEncConvImpl impl, void *implCtxt,
                    xmlCharEncodingHandler **out) {
    /*
     * Try custom implementation before deprecated global handlers.
     *
     * Note that we pass the original name without deprecated
     * alias resolution.
     */
    if (impl != NULL)
        return(impl(implCtxt, norig, flags, out));

    /*
     * Deprecated
     */
    if (globalHandlers != NULL) {
        int i;

        for (i = 0; i < nbCharEncodingHandler; i++) {
            xmlCharEncodingHandler *h = globalHandlers[i];

            if (!xmlStrcasecmp((const xmlChar *) name,
                               (const xmlChar *) h->name)) {
                if ((((flags & XML_ENC_INPUT) == 0) || (h->input.func)) &&
                    (((flags & XML_ENC_OUTPUT) == 0) || (h->output.func))) {
                    *out = h;
                    return(XML_ERR_OK);
                }
            }
        }
    }

#ifdef LIBXML_ICONV_ENABLED
    {
        int ret = xmlCharEncIconv(name, flags, out);

        if (ret == XML_ERR_OK)
            return(XML_ERR_OK);
        if (ret != XML_ERR_UNSUPPORTED_ENCODING)
            return(ret);
    }
#endif /* LIBXML_ICONV_ENABLED */

#ifdef LIBXML_ICU_ENABLED
    {
        int ret = xmlCharEncUconv(name, flags, out);

        if (ret == XML_ERR_OK)
            return(XML_ERR_OK);
        if (ret != XML_ERR_UNSUPPORTED_ENCODING)
            return(ret);
    }
#endif /* LIBXML_ICU_ENABLED */

    return(XML_ERR_UNSUPPORTED_ENCODING);
}

/**
 * Find or create a handler matching the encoding. The following
 * converters are looked up in order:
 *
 * - Built-in handler (UTF-8, UTF-16, ISO-8859-1, ASCII)
 * - User-registered global handler (deprecated)
 * - iconv if enabled
 * - ICU if enabled
 *
 * The handler must be closed with #xmlCharEncCloseFunc.
 *
 * If the encoding is UTF-8, a NULL handler and no error code will
 * be returned.
 *
 * @since 2.13.0
 *
 * @param enc  an xmlCharEncoding value.
 * @param out  pointer to result
 * @returns XML_ERR_OK, XML_ERR_UNSUPPORTED_ENCODING or another
 * xmlParserErrors error code.
 */
xmlParserErrors
xmlLookupCharEncodingHandler(xmlCharEncoding enc,
                             xmlCharEncodingHandler **out) {
    const xmlCharEncodingHandler *handler;

    if (out == NULL)
        return(XML_ERR_ARGUMENT);
    *out = NULL;

    if ((enc <= 0) || ((size_t) enc >= NUM_DEFAULT_HANDLERS))
        return(XML_ERR_UNSUPPORTED_ENCODING);

    /* Return NULL handler for UTF-8 */
    if ((enc == XML_CHAR_ENCODING_UTF8) ||
        (enc == XML_CHAR_ENCODING_NONE))
        return(XML_ERR_OK);

    handler = &defaultHandlers[enc];
    if ((handler->input.func != NULL) || (handler->output.func != NULL)) {
        *out = (xmlCharEncodingHandler *) handler;
        return(XML_ERR_OK);
    }

    if (handler->name != NULL) {
        xmlCharEncFlags flags = XML_ENC_INPUT;

#ifdef LIBXML_OUTPUT_ENABLED
        flags |= XML_ENC_OUTPUT;
#endif
        return(xmlFindExtraHandler(handler->name, handler->name, flags,
                                   NULL, NULL, out));
    }

    return(XML_ERR_UNSUPPORTED_ENCODING);
}

/**
 * @deprecated Use #xmlLookupCharEncodingHandler which has better error
 * reporting.
 *
 * @param enc  an xmlCharEncoding value.
 * @returns the handler or NULL if no handler was found or an error
 * occurred.
 */
xmlCharEncodingHandler *
xmlGetCharEncodingHandler(xmlCharEncoding enc) {
    xmlCharEncodingHandler *ret;

    xmlLookupCharEncodingHandler(enc, &ret);
    return(ret);
}

/**
 * Find or create a handler matching the encoding. The following
 * converters are looked up in order:
 *
 * - Built-in handler (UTF-8, UTF-16, ISO-8859-1, ASCII)
 * - Custom implementation if provided
 * - User-registered global handler (deprecated)
 * - iconv if enabled
 * - ICU if enabled
 *
 * The handler must be closed with #xmlCharEncCloseFunc.
 *
 * If the encoding is UTF-8, a NULL handler and no error code will
 * be returned.
 *
 * `flags` can contain XML_ENC_INPUT, XML_ENC_OUTPUT or both.
 *
 * @since 2.14.0
 *
 * @param name  a string describing the char encoding.
 * @param flags  bit mask of flags
 * @param impl  a conversion implementation (optional)
 * @param implCtxt  user data for conversion implementation (optional)
 * @param out  pointer to result
 * @returns XML_ERR_OK, XML_ERR_UNSUPPORTED_ENCODING or another
 * xmlParserErrors error code.
 */
xmlParserErrors
xmlCreateCharEncodingHandler(const char *name, xmlCharEncFlags flags,
                             xmlCharEncConvImpl impl, void *implCtxt,
                             xmlCharEncodingHandler **out) {
    const xmlCharEncodingHandler *handler;
    const char *norig, *nalias;
    xmlCharEncoding enc;

    if (out == NULL)
        return(XML_ERR_ARGUMENT);
    *out = NULL;

    if ((name == NULL) || (flags == 0))
        return(XML_ERR_ARGUMENT);

    norig = name;
    nalias = xmlGetEncodingAlias(name);
    if (nalias != NULL)
	name = nalias;

    enc = xmlParseCharEncodingInternal(name);

    /* Return NULL handler for UTF-8 */
    if (enc == XML_CHAR_ENCODING_UTF8)
        return(XML_ERR_OK);

    if ((enc > 0) && ((size_t) enc < NUM_DEFAULT_HANDLERS)) {
        if (flags & XML_ENC_HTML) {
            /*
             * TODO: HTML5 only allows a fixed set of charset
             * labels. We should add an option to enable or
             * disable this restriction.
             *
             * TODO: Map ISO-8859-9 to windows-1254.
             */
            switch (enc) {
                case XML_CHAR_ENCODING_ASCII:
                case XML_CHAR_ENCODING_8859_1:
                    enc = XML_CHAR_ENCODING_WINDOWS_1252;
                    break;
                case XML_CHAR_ENCODING_UCS2:
                case XML_CHAR_ENCODING_UTF16:
                    enc = XML_CHAR_ENCODING_UTF16LE;
                    break;
                default:
                    break;
            }
        }

        handler = &defaultHandlers[enc];
        if ((((flags & XML_ENC_INPUT) == 0) || (handler->input.func)) &&
            (((flags & XML_ENC_OUTPUT) == 0) || (handler->output.func))) {
            xmlCharEncodingHandler *ret;

            /*
             * Return a copy of the handler with the original name.
             */

            ret = xmlMalloc(sizeof(*ret));
            if (ret == NULL)
                return(XML_ERR_NO_MEMORY);
            memset(ret, 0, sizeof(*ret));

            ret->name = xmlMemStrdup(norig);
            if (ret->name == NULL) {
                xmlFree(ret);
                return(XML_ERR_NO_MEMORY);
            }
            ret->input = handler->input;
            ret->output = handler->output;
            ret->inputCtxt = handler->inputCtxt;
            ret->outputCtxt = handler->outputCtxt;
            ret->ctxtDtor = handler->ctxtDtor;

            *out = ret;
            return(XML_ERR_OK);
        }
    }

    return(xmlFindExtraHandler(norig, name, flags, impl, implCtxt, out));
}

/**
 * Find or create a handler matching the encoding. The following
 * converters are looked up in order:
 *
 * - Built-in handler (UTF-8, UTF-16, ISO-8859-1, ASCII)
 * - User-registered global handler (deprecated)
 * - iconv if enabled
 * - ICU if enabled
 *
 * The handler must be closed with #xmlCharEncCloseFunc.
 *
 * If the encoding is UTF-8, a NULL handler and no error code will
 * be returned.
 *
 * @since 2.13.0
 *
 * @param name  a string describing the char encoding.
 * @param output  boolean, use handler for output
 * @param out  pointer to result
 * @returns XML_ERR_OK, XML_ERR_UNSUPPORTED_ENCODING or another
 * xmlParserErrors error code.
 */
xmlParserErrors
xmlOpenCharEncodingHandler(const char *name, int output,
                           xmlCharEncodingHandler **out) {
    xmlCharEncFlags flags = output ? XML_ENC_OUTPUT : XML_ENC_INPUT;

    return(xmlCreateCharEncodingHandler(name, flags, NULL, NULL, out));
}

/**
 * If the encoding is UTF-8, this will return a no-op handler that
 * shouldn't be used.
 *
 * @deprecated Use #xmlOpenCharEncodingHandler which has better error
 * reporting.
 *
 * @param name  a string describing the char encoding.
 * @returns the handler or NULL if no handler was found or an error
 * occurred.
 */
xmlCharEncodingHandler *
xmlFindCharEncodingHandler(const char *name) {
    xmlCharEncodingHandler *ret;
    xmlCharEncFlags flags;

    /*
     * This handler shouldn't be used, but we must return a non-NULL
     * handler.
     */
    if ((xmlStrcasecmp(BAD_CAST name, BAD_CAST "UTF-8") == 0) ||
        (xmlStrcasecmp(BAD_CAST name, BAD_CAST "UTF8") == 0))
        return((xmlCharEncodingHandlerPtr)
                &defaultHandlers[XML_CHAR_ENCODING_UTF8]);

    flags = XML_ENC_INPUT;
#ifdef LIBXML_OUTPUT_ENABLED
    flags |= XML_ENC_OUTPUT;
#endif
    xmlCreateCharEncodingHandler(name, flags, NULL, NULL, &ret);
    return(ret);
}

/************************************************************************
 *									*
 *		ICONV based generic conversion functions		*
 *									*
 ************************************************************************/

#ifdef LIBXML_ICONV_ENABLED
typedef struct {
    iconv_t cd;
} xmlIconvCtxt;

/**
 * The value of `inlen` after return is the number of bytes consumed.
 * The value of `outlen` after return is the number of bytes produced.
 *
 * @param vctxt  conversion context
 * @param out  a pointer to an array of bytes to store the result
 * @param outlen  the length of `out`
 * @param in  a pointer to an array of input bytes
 * @param inlen  the length of `in`
 * @param flush  end of input
 * @returns an xmlCharEncError code.
 */
static xmlCharEncError
xmlIconvConvert(void *vctxt, unsigned char *out, int *outlen,
                const unsigned char *in, int *inlen,
                int flush ATTRIBUTE_UNUSED) {
    xmlIconvCtxt *ctxt = vctxt;
    size_t icv_inlen, icv_outlen;
    const char *icv_in = (const char *) in;
    char *icv_out = (char *) out;
    size_t ret;

    if ((out == NULL) || (outlen == NULL) || (inlen == NULL) || (in == NULL)) {
        if (outlen != NULL) *outlen = 0;
        return(XML_ENC_ERR_INTERNAL);
    }
    icv_inlen = *inlen;
    icv_outlen = *outlen;
    /*
     * Some versions take const, other versions take non-const input.
     */
    ret = iconv(ctxt->cd, (void *) &icv_in, &icv_inlen, &icv_out, &icv_outlen);
    *inlen -= icv_inlen;
    *outlen -= icv_outlen;
    if (ret == (size_t) -1) {
        if (errno == EILSEQ)
            return(XML_ENC_ERR_INPUT);
        if (errno == E2BIG)
            return(XML_ENC_ERR_SPACE);
        /*
         * EINVAL means a truncated multi-byte sequence at the end
         * of the input buffer. We treat this as success.
         */
        if (errno == EINVAL)
            return(XML_ENC_ERR_SUCCESS);
#ifdef __APPLE__
        /*
         * Apple's new libiconv can return EOPNOTSUPP under
         * unknown circumstances (detected when fuzzing).
         */
        if (errno == EOPNOTSUPP)
            return(XML_ENC_ERR_INPUT);
#endif
        return(XML_ENC_ERR_INTERNAL);
    }
    return(XML_ENC_ERR_SUCCESS);
}

static void
xmlIconvFree(void *vctxt) {
    xmlIconvCtxt *ctxt = vctxt;

    if (ctxt == NULL)
        return;

    if (ctxt->cd != (iconv_t) -1)
        iconv_close(ctxt->cd);

    xmlFree(ctxt);
}

#if defined(FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION) && \
    defined(__GLIBC__)
#include <libxml/parserInternals.h>

static int
xmlEncodingMatch(const char *name1, const char *name2) {
    /*
     * Fuzzy match for encoding names
     */
    while (1) {
        while ((*name1 != 0) && (!IS_ASCII_LETTER(*name1)))
            name1 += 1;
        while ((*name2 != 0) && (!IS_ASCII_LETTER(*name2)))
            name2 += 1;
        if ((*name1 == 0) || (*name2 == 0))
            break;
        if ((*name1 | 0x20) != (*name2 | 0x20))
            return(0);
        name1 += 1;
        name2 += 1;
    }

    return((*name1 == 0) && (*name2 == 0));
}
#endif /* FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION */

static xmlParserErrors
xmlCharEncIconv(const char *name, xmlCharEncFlags flags,
                xmlCharEncodingHandler **out) {
    xmlCharEncConvFunc inFunc = NULL, outFunc = NULL;
    xmlIconvCtxt *inputCtxt = NULL, *outputCtxt = NULL;
    iconv_t icv_in;
    iconv_t icv_out;
    xmlParserErrors ret;

    /*
     * POSIX allows "indicator suffixes" like "//IGNORE" to be
     * passed to iconv_open. This can change the behavior in
     * unexpected ways.
     *
     * Many iconv implementations also support non-standard
     * codesets like "wchar_t", "char" or the empty string "".
     * It would make sense to disallow them, but codeset names
     * are matched fuzzily, so a string like "w-C.hA_rt" could
     * be interpreted as "wchar_t".
     *
     * When escaping characters that aren't supported in the
     * target encoding, we also rely on GNU libiconv behavior to
     * stop conversion without trying any kind of fallback.
     * This violates the POSIX spec which says:
     *
     * > If iconv() encounters a character in the input buffer
     * > that is valid, but for which an identical character does
     * > not exist in the output codeset [...] iconv() shall
     * > perform an implementation-defined conversion on the
     * > character.
     *
     * See: https://sourceware.org/bugzilla/show_bug.cgi?id=29913
     *
     * Unfortunately, strict POSIX compliance makes it impossible
     * to detect untranslatable characters.
     */
    if (strstr(name, "//") != NULL) {
        ret = XML_ERR_UNSUPPORTED_ENCODING;
        goto error;
    }

#if defined(FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION) && \
    defined(__GLIBC__)
    /*
     * This glibc bug can lead to unpredictable results with the
     * push parser.
     *
     * https://sourceware.org/bugzilla/show_bug.cgi?id=32633
     */
    if ((xmlEncodingMatch(name, "TSCII")) ||
        (xmlEncodingMatch(name, "BIG5-HKSCS"))) {
        ret = XML_ERR_UNSUPPORTED_ENCODING;
        goto error;
    }
#endif

    if (flags & XML_ENC_INPUT) {
        inputCtxt = xmlMalloc(sizeof(xmlIconvCtxt));
        if (inputCtxt == NULL) {
            ret = XML_ERR_NO_MEMORY;
            goto error;
        }
        inputCtxt->cd = (iconv_t) -1;

        icv_in = iconv_open("UTF-8", name);
        if (icv_in == (iconv_t) -1) {
            if (errno == EINVAL)
                ret = XML_ERR_UNSUPPORTED_ENCODING;
            else if (errno == ENOMEM)
                ret = XML_ERR_NO_MEMORY;
            else
                ret = XML_ERR_SYSTEM;
            goto error;
        }
        inputCtxt->cd = icv_in;

        inFunc = xmlIconvConvert;
    }

    if (flags & XML_ENC_OUTPUT) {
        outputCtxt = xmlMalloc(sizeof(xmlIconvCtxt));
        if (outputCtxt == NULL) {
            ret = XML_ERR_NO_MEMORY;
            goto error;
        }
        outputCtxt->cd = (iconv_t) -1;

        icv_out = iconv_open(name, "UTF-8");
        if (icv_out == (iconv_t) -1) {
            if (errno == EINVAL)
                ret = XML_ERR_UNSUPPORTED_ENCODING;
            else if (errno == ENOMEM)
                ret = XML_ERR_NO_MEMORY;
            else
                ret = XML_ERR_SYSTEM;
            goto error;
        }
        outputCtxt->cd = icv_out;

        outFunc = xmlIconvConvert;
    }

    return(xmlCharEncNewCustomHandler(name, inFunc, outFunc, xmlIconvFree,
                                      inputCtxt, outputCtxt, out));

error:
    if (inputCtxt != NULL)
        xmlIconvFree(inputCtxt);
    if (outputCtxt != NULL)
        xmlIconvFree(outputCtxt);
    return(ret);
}
#endif /* LIBXML_ICONV_ENABLED */

/************************************************************************
 *									*
 *		ICU based generic conversion functions		*
 *									*
 ************************************************************************/

#ifdef LIBXML_ICU_ENABLED
/* Size of pivot buffer, same as icu/source/common/ucnv.cpp CHUNK_SIZE */
#define ICU_PIVOT_BUF_SIZE 1024

typedef struct _uconv_t xmlUconvCtxt;
struct _uconv_t {
  UConverter *uconv; /* for conversion between an encoding and UTF-16 */
  UConverter *utf8; /* for conversion between UTF-8 and UTF-16 */
  UChar      *pivot_source;
  UChar      *pivot_target;
  int        isInput;
  UChar      pivot_buf[ICU_PIVOT_BUF_SIZE];
};

/**
 * The value of `inlen` after return is the number of bytes consumed.
 * The value of `outlen` after return is the number of bytes produced.
 *
 * @param vctxt  conversion context
 * @param out  a pointer to an array of bytes to store the result
 * @param outlen  the length of `out`
 * @param in  a pointer to an array of input bytes
 * @param inlen  the length of `in`
 * @param flush  end of input
 * @returns an xmlCharEncError code.
 */
static xmlCharEncError
xmlUconvConvert(void *vctxt, unsigned char *out, int *outlen,
                const unsigned char *in, int *inlen, int flush) {
    xmlUconvCtxt *cd = vctxt;
    const char *ucv_in = (const char *) in;
    char *ucv_out = (char *) out;
    UConverter *target, *source;
    UErrorCode err = U_ZERO_ERROR;
    int ret;

    if ((out == NULL) || (outlen == NULL) || (inlen == NULL) || (in == NULL)) {
        if (outlen != NULL)
            *outlen = 0;
        return(XML_ENC_ERR_INTERNAL);
    }

    /*
     * The ICU API can consume input, including partial sequences,
     * even if the output buffer would overflow. The remaining input
     * must be processed by calling ucnv_convertEx with a possibly
     * empty input buffer.
     */
    if (cd->isInput) {
        source = cd->uconv;
        target = cd->utf8;
    } else {
        source = cd->utf8;
        target = cd->uconv;
    }

    ucnv_convertEx(target, source, &ucv_out, ucv_out + *outlen,
                   &ucv_in, ucv_in + *inlen, cd->pivot_buf,
                   &cd->pivot_source, &cd->pivot_target,
                   cd->pivot_buf + ICU_PIVOT_BUF_SIZE,
                   /* reset */ 0, flush, &err);

    *inlen = ucv_in - (const char*) in;
    *outlen = ucv_out - (char *) out;

    if (U_SUCCESS(err)) {
        ret = XML_ENC_ERR_SUCCESS;
    } else {
        switch (err) {
            case U_TRUNCATED_CHAR_FOUND:
                /* Should only happen with flush */
                ret = XML_ENC_ERR_INPUT;
                break;

            case U_BUFFER_OVERFLOW_ERROR:
                ret = XML_ENC_ERR_SPACE;
                break;

            case U_INVALID_CHAR_FOUND:
            case U_ILLEGAL_CHAR_FOUND:
            case U_ILLEGAL_ESCAPE_SEQUENCE:
            case U_UNSUPPORTED_ESCAPE_SEQUENCE:
                ret = XML_ENC_ERR_INPUT;
                break;

            case U_MEMORY_ALLOCATION_ERROR:
                ret = XML_ENC_ERR_MEMORY;
                break;

            default:
                ret = XML_ENC_ERR_INTERNAL;
                break;
        }
    }

    return(ret);
}

static xmlParserErrors
openIcuConverter(const char* name, int isInput, xmlUconvCtxt **out)
{
    UErrorCode status;
    xmlUconvCtxt *conv;

    *out = NULL;

    conv = (xmlUconvCtxt *) xmlMalloc(sizeof(xmlUconvCtxt));
    if (conv == NULL)
        return(XML_ERR_NO_MEMORY);

    conv->isInput = isInput;
    conv->pivot_source = conv->pivot_buf;
    conv->pivot_target = conv->pivot_buf;

    status = U_ZERO_ERROR;
    conv->uconv = ucnv_open(name, &status);
    if (U_FAILURE(status))
        goto error;

    status = U_ZERO_ERROR;
    if (isInput) {
        ucnv_setToUCallBack(conv->uconv, UCNV_TO_U_CALLBACK_STOP,
                                                NULL, NULL, NULL, &status);
    }
    else {
        ucnv_setFromUCallBack(conv->uconv, UCNV_FROM_U_CALLBACK_STOP,
                                                NULL, NULL, NULL, &status);
    }
    if (U_FAILURE(status))
        goto error;

    status = U_ZERO_ERROR;
    conv->utf8 = ucnv_open("UTF-8", &status);
    if (U_FAILURE(status))
        goto error;

    *out = conv;
    return(XML_ERR_OK);

error:
    if (conv->uconv)
        ucnv_close(conv->uconv);
    xmlFree(conv);

    if (status == U_FILE_ACCESS_ERROR)
        return(XML_ERR_UNSUPPORTED_ENCODING);
    if (status == U_MEMORY_ALLOCATION_ERROR)
        return(XML_ERR_NO_MEMORY);
    return(XML_ERR_SYSTEM);
}

static void
closeIcuConverter(xmlUconvCtxt *conv)
{
    if (conv == NULL)
        return;
    ucnv_close(conv->uconv);
    ucnv_close(conv->utf8);
    xmlFree(conv);
}

static void
xmlUconvFree(void *vctxt) {
    closeIcuConverter(vctxt);
}

static xmlParserErrors
xmlCharEncUconv(const char *name, xmlCharEncFlags flags,
                xmlCharEncodingHandler **out) {
    xmlCharEncConvFunc inFunc = NULL, outFunc = NULL;
    xmlUconvCtxt *ucv_in = NULL;
    xmlUconvCtxt *ucv_out = NULL;
    int ret;

    if (flags & XML_ENC_INPUT) {
        ret = openIcuConverter(name, 1, &ucv_in);
        if (ret != 0)
            goto error;
        inFunc = xmlUconvConvert;
    }

    if (flags & XML_ENC_OUTPUT) {
        ret = openIcuConverter(name, 0, &ucv_out);
        if (ret != 0)
            goto error;
        outFunc = xmlUconvConvert;
    }

    return(xmlCharEncNewCustomHandler(name, inFunc, outFunc, xmlUconvFree,
                                      ucv_in, ucv_out, out));

error:
    if (ucv_in != NULL)
        closeIcuConverter(ucv_in);
    if (ucv_out != NULL)
        closeIcuConverter(ucv_out);
    return(ret);
}
#endif /* LIBXML_ICU_ENABLED */

/************************************************************************
 *									*
 *		The real API used by libxml for on-the-fly conversion	*
 *									*
 ************************************************************************/

/**
 * Convert xmlCharEncError to xmlParserErrors code.
 *
 * @param code  xmlCharEncError code
 */
static xmlParserErrors
xmlEncConvertError(xmlCharEncError code) {
    xmlParserErrors ret;

    switch (code) {
        case XML_ENC_ERR_SUCCESS:
            ret = XML_ERR_OK;
            break;
        case XML_ENC_ERR_INPUT:
            ret = XML_ERR_INVALID_ENCODING;
            break;
        case XML_ENC_ERR_MEMORY:
            ret = XML_ERR_NO_MEMORY;
            break;
        default:
            ret = XML_ERR_INTERNAL_ERROR;
            break;
    }

    return(ret);
}

/**
 * The value of `inlen` after return is the number of bytes consumed.
 * The value of `outlen` after return is the number of bytes produced.
 *
 * @param handler  encoding handler
 * @param out  a pointer to an array of bytes to store the result
 * @param outlen  the length of `out`
 * @param in  a pointer to an array of input bytes
 * @param inlen  the length of `in`
 * @param flush  end of input
 * @returns an xmlCharEncError code.
 */
xmlCharEncError
xmlEncInputChunk(xmlCharEncodingHandler *handler, unsigned char *out,
                 int *outlen, const unsigned char *in, int *inlen,
                 int flush) {
    xmlCharEncError ret;

    if (handler->flags & XML_HANDLER_LEGACY) {
        xmlCharEncodingInputFunc func = handler->input.legacyFunc;

        if (func == NULL) {
            *outlen = 0;
            *inlen = 0;
            return(XML_ENC_ERR_INTERNAL);
        }

        ret = func(out, outlen, in, inlen);
    } else {
        xmlCharEncConvFunc func = handler->input.func;
        int oldInlen;

        if (func == NULL) {
            *outlen = 0;
            *inlen = 0;
            return(XML_ENC_ERR_INTERNAL);
        }

        oldInlen = *inlen;
        ret = func(handler->inputCtxt, out, outlen, in, inlen, flush);

        /*
         * Check for truncated multi-byte sequence.
         */
        if ((flush) && (ret == XML_ENC_ERR_SUCCESS) && (*inlen != oldInlen))
            ret = XML_ENC_ERR_INPUT;
    }

    if (ret > 0)
        ret = XML_ENC_ERR_SUCCESS;

    return(ret);
}

/**
 * The value of `inlen` after return is the number of bytes consumed.
 * The value of `outlen` after return is the number of bytes produced.
 *
 * @param handler  encoding handler
 * @param out  a pointer to an array of bytes to store the result
 * @param outlen  the length of `out`
 * @param in  a pointer to an array of input bytes
 * @param inlen  the length of `in`
 * @returns an xmlCharEncError code.
 */
static xmlCharEncError
xmlEncOutputChunk(xmlCharEncodingHandler *handler, unsigned char *out,
                  int *outlen, const unsigned char *in, int *inlen) {
    xmlCharEncError ret;

    if (handler->flags & XML_HANDLER_LEGACY) {
        xmlCharEncodingOutputFunc func = handler->output.legacyFunc;

        if (func == NULL) {
            *outlen = 0;
            *inlen = 0;
            return(XML_ENC_ERR_INTERNAL);
        }

        ret = func(out, outlen, in, inlen);
    } else {
        xmlCharEncConvFunc func = handler->output.func;

        if (func == NULL) {
            *outlen = 0;
            *inlen = 0;
            return(XML_ENC_ERR_INTERNAL);
        }

        ret = func(handler->outputCtxt, out, outlen, in, inlen, /* flush */ 0);
    }

    if (ret > 0)
        ret = XML_ENC_ERR_SUCCESS;

    return(ret);
}

/**
 * DEPERECATED: Don't use.
 *
 * @param handler  encoding handler
 * @param out  an xmlBuffer for the output.
 * @param in  an xmlBuffer for the input
 * @returns the number of bytes written or an xmlCharEncError code.
 */
int
xmlCharEncFirstLine(xmlCharEncodingHandler *handler, struct _xmlBuffer *out,
                    struct _xmlBuffer *in) {
    return(xmlCharEncInFunc(handler, out, in));
}

/**
 * Generic front-end for input encoding conversion.
 *
 * `sizeOut` should be set to the maximum output size (or SIZE_MAX).
 * After return, it is set to the number of bytes written.
 *
 * @param input  a parser input buffer
 * @param sizeOut  pointer to output size
 * @param flush  end of input
 * @returns an xmlCharEncError code.
 */
xmlCharEncError
xmlCharEncInput(xmlParserInputBuffer *input, size_t *sizeOut, int flush)
{
    xmlBufPtr out, in;
    const xmlChar *dataIn;
    size_t availIn;
    size_t maxOut;
    size_t totalIn, totalOut;
    xmlCharEncError ret;

    out = input->buffer;
    in = input->raw;

    maxOut = *sizeOut;
    totalOut = 0;

    *sizeOut = 0;

    availIn = xmlBufUse(in);
    if ((availIn == 0) && (!flush))
        return(0);
    dataIn = xmlBufContent(in);
    totalIn = 0;

    while (1) {
        size_t availOut;
        int completeOut, completeIn;
        int c_out, c_in;

        availOut = xmlBufAvail(out);
        if (availOut > INT_MAX / 2)
            availOut = INT_MAX / 2;

        if (availOut < maxOut) {
            c_out = availOut;
            completeOut = 0;
        } else {
            c_out = maxOut;
            completeOut = 1;
        }

        if (availIn > INT_MAX / 2) {
            c_in = INT_MAX / 2;
            completeIn = 0;
        } else {
            c_in = availIn;
            completeIn = 1;
        }

        ret = xmlEncInputChunk(input->encoder, xmlBufEnd(out), &c_out,
                               dataIn, &c_in, flush && completeIn);

        totalIn += c_in;
        dataIn += c_in;
        availIn -= c_in;

        totalOut += c_out;
        maxOut -= c_out;
        xmlBufAddLen(out, c_out);

        if ((ret != XML_ENC_ERR_SUCCESS) && (ret != XML_ENC_ERR_SPACE)) {
            input->error = xmlEncConvertError(ret);
            return(ret);
        }

        if ((completeOut) && (completeIn))
            break;
        if ((completeOut) && (ret == XML_ENC_ERR_SPACE))
            break;
        if ((completeIn) && (ret == XML_ENC_ERR_SUCCESS))
            break;

        if (ret == XML_ENC_ERR_SPACE) {
            if (xmlBufGrow(out, 4096) < 0) {
                input->error = XML_ERR_NO_MEMORY;
                return(XML_ENC_ERR_MEMORY);
            }
        }
    }

    xmlBufShrink(in, totalIn);

    if (input->rawconsumed > ULONG_MAX - (unsigned long) totalIn)
        input->rawconsumed = ULONG_MAX;
    else
        input->rawconsumed += totalIn;

    *sizeOut = totalOut;
    return(XML_ENC_ERR_SUCCESS);
}

/**
 * Generic front-end for input encoding conversion.
 *
 * @param handler  encoding handler
 * @param out  an xmlBuffer for the output.
 * @param in  an xmlBuffer for the input
 * @returns the number of bytes written or an xmlCharEncError code.
 */
int
xmlCharEncInFunc(xmlCharEncodingHandler * handler, struct _xmlBuffer *out,
                 struct _xmlBuffer *in)
{
    int ret;
    int written;
    int toconv;

    if (handler == NULL)
        return(XML_ENC_ERR_INTERNAL);
    if (out == NULL)
        return(XML_ENC_ERR_INTERNAL);
    if (in == NULL)
        return(XML_ENC_ERR_INTERNAL);

    toconv = in->use;
    if (toconv == 0)
        return (0);
    written = out->size - out->use -1; /* count '\0' */
    if (toconv * 2 >= written) {
        xmlBufferGrow(out, out->size + toconv * 2);
        written = out->size - out->use - 1;
    }
    ret = xmlEncInputChunk(handler, &out->content[out->use], &written,
                           in->content, &toconv, /* flush */ 0);
    xmlBufferShrink(in, toconv);
    out->use += written;
    out->content[out->use] = 0;

    return (written? written : ret);
}

/*
 * @param buf  a char buffer
 * @param val  a codepoint
 *
 * Serializes a decimal char ref like `&#38;`.
 *
 * Writes at most 10 bytes. Does not include a terminating zero byte.
 *
 * @returns the number of bytes written.
 */
static int
xmlSerializeDecCharRef(char *buf, int val) {
    char *out = buf;
    int len, i;

    *out++ = '&';
    *out++ = '#';

    if (val < 100) {
        len = (val < 10) ? 1 : 2;
    } else if (val < 10000) {
        len = (val < 1000) ? 3 : 4;
    } else if (val < 1000000) {
        len = (val < 100000) ? 5 : 6;
    } else {
        len = 7;
    }

    for (i = len - 1; i >= 0; i--) {
        out[i] = '0' + val % 10;
        val /= 10;
    }

    out[len] = ';';

    return(len + 3);
}

#ifdef LIBXML_OUTPUT_ENABLED
/**
 * Generic front-end for output encoding conversion.
 *
 * A first call with `init` set to 1 has to be made to write a BOM.
 *
 * When using GNU libiconv, unsupported characters in the output
 * encoding will be automatically replaced with a numeric character
 * reference.
 *
 * @param output  a parser output buffer
 * @param init  is this an initialization call without data
 * @returns the number of bytes written or an xmlCharEncError code.
 */
int
xmlCharEncOutput(xmlOutputBuffer *output, int init)
{
    int ret;
    size_t written;
    int writtentot = 0;
    size_t toconv;
    int c_in;
    int c_out;
    xmlBufPtr in;
    xmlBufPtr out;

    if ((output == NULL) || (output->encoder == NULL) ||
        (output->buffer == NULL) || (output->conv == NULL))
        return(XML_ENC_ERR_INTERNAL);
    out = output->conv;
    in = output->buffer;

retry:

    written = xmlBufAvail(out);

    /*
     * First specific handling of the initialization call
     */
    if (init) {
        c_in = 0;
        c_out = written;
        /* TODO: Check return value. */
        xmlEncOutputChunk(output->encoder, xmlBufEnd(out), &c_out,
                          NULL, &c_in);
        xmlBufAddLen(out, c_out);
        return(c_out);
    }

    /*
     * Conversion itself.
     */
    toconv = xmlBufUse(in);
    if (toconv > 64 * 1024)
        toconv = 64 * 1024;
    if (toconv * 4 >= written) {
        if (xmlBufGrow(out, toconv * 4) < 0) {
            ret = XML_ENC_ERR_MEMORY;
            goto error;
        }
        written = xmlBufAvail(out);
    }
    if (written > 256 * 1024)
        written = 256 * 1024;

    c_in = toconv;
    c_out = written;
    ret = xmlEncOutputChunk(output->encoder, xmlBufEnd(out), &c_out,
                            xmlBufContent(in), &c_in);
    xmlBufShrink(in, c_in);
    xmlBufAddLen(out, c_out);
    writtentot += c_out;

    if (ret == XML_ENC_ERR_SPACE)
        goto retry;

    /*
     * Attempt to handle error cases
     */
    if (ret == XML_ENC_ERR_INPUT) {
        xmlChar charref[20];
        int len = xmlBufUse(in);
        xmlChar *content = xmlBufContent(in);
        int cur, charrefLen;

        cur = xmlGetUTF8Char(content, &len);
        if (cur <= 0)
            goto error;

        /*
         * Removes the UTF8 sequence, and replace it by a charref
         * and continue the transcoding phase, hoping the error
         * did not mangle the encoder state.
         */
        charrefLen = xmlSerializeDecCharRef((char *) charref, cur);
        xmlBufGrow(out, charrefLen * 4);
        c_out = xmlBufAvail(out);
        c_in = charrefLen;
        ret = xmlEncOutputChunk(output->encoder, xmlBufEnd(out), &c_out,
                                charref, &c_in);
        if ((ret < 0) || (c_in != charrefLen)) {
            ret = XML_ENC_ERR_INTERNAL;
            goto error;
        }

        xmlBufShrink(in, len);
        xmlBufAddLen(out, c_out);
        writtentot += c_out;
        goto retry;
    }

error:
    if (((writtentot <= 0) && (ret != 0)) ||
        (ret == XML_ENC_ERR_MEMORY)) {
        if (output->error == 0)
            output->error = xmlEncConvertError(ret);
        return(ret);
    }

    return(writtentot);
}
#endif

/**
 * Generic front-end for output encoding conversion.
 *
 * A first call with `in` set to NULL has to be made to write a BOM.
 *
 * When using GNU libiconv, unsupported characters in the output
 * encoding will be automatically replaced with a numeric character
 * reference.
 *
 * @param handler  encoding handler
 * @param out  an xmlBuffer for the output.
 * @param in  an xmlBuffer for the input
 * @returns the number of bytes written or an xmlCharEncError code.
 */
int
xmlCharEncOutFunc(xmlCharEncodingHandler *handler, struct _xmlBuffer *out,
                  struct _xmlBuffer *in) {
    int ret;
    int written;
    int writtentot = 0;
    int toconv;

    if (handler == NULL) return(XML_ENC_ERR_INTERNAL);
    if (out == NULL) return(XML_ENC_ERR_INTERNAL);

retry:

    written = out->size - out->use;

    if (written > 0)
	written--; /* Gennady: count '/0' */

    /*
     * First specific handling of in = NULL, i.e. the initialization call
     */
    if (in == NULL) {
        toconv = 0;
        /* TODO: Check return value. */
        xmlEncOutputChunk(handler, &out->content[out->use], &written,
                          NULL, &toconv);
        out->use += written;
        out->content[out->use] = 0;
        return(0);
    }

    /*
     * Conversion itself.
     */
    toconv = in->use;
    if (toconv * 4 >= written) {
        xmlBufferGrow(out, toconv * 4);
	written = out->size - out->use - 1;
    }
    ret = xmlEncOutputChunk(handler, &out->content[out->use], &written,
                            in->content, &toconv);
    xmlBufferShrink(in, toconv);
    out->use += written;
    writtentot += written;
    out->content[out->use] = 0;

    if (ret == XML_ENC_ERR_SPACE)
        goto retry;

    /*
     * Attempt to handle error cases
     */
    if (ret == XML_ENC_ERR_INPUT) {
        xmlChar charref[20];
        int len = in->use;
        const xmlChar *utf = (const xmlChar *) in->content;
        int cur, charrefLen;

        cur = xmlGetUTF8Char(utf, &len);
        if (cur <= 0)
            return(ret);

        /*
         * Removes the UTF8 sequence, and replace it by a charref
         * and continue the transcoding phase, hoping the error
         * did not mangle the encoder state.
         */
        charrefLen = xmlSerializeDecCharRef((char *) charref, cur);
        xmlBufferShrink(in, len);
        xmlBufferGrow(out, charrefLen * 4);
        written = out->size - out->use - 1;
        toconv = charrefLen;
        ret = xmlEncOutputChunk(handler, &out->content[out->use], &written,
                                charref, &toconv);
        if ((ret < 0) || (toconv != charrefLen))
            return(XML_ENC_ERR_INTERNAL);

        out->use += written;
        writtentot += written;
        out->content[out->use] = 0;
        goto retry;
    }
    return(writtentot ? writtentot : ret);
}

/**
 * Releases an xmlCharEncodingHandler. Must be called after
 * a handler is no longer in use.
 *
 * @param handler  encoding handler
 * @returns 0.
 */
int
xmlCharEncCloseFunc(xmlCharEncodingHandler *handler) {
    if (handler == NULL)
        return(0);

    if (handler->flags & XML_HANDLER_STATIC)
        return(0);

    xmlFree(handler->name);
    if (handler->ctxtDtor != NULL) {
        handler->ctxtDtor(handler->inputCtxt);
        handler->ctxtDtor(handler->outputCtxt);
    }
    xmlFree(handler);
    return(0);
}

/**
 * This function provides the current index of the parser relative
 * to the start of the current entity. This function is computed in
 * bytes from the beginning starting at zero and finishing at the
 * size in byte of the file if parsing a file. The function is
 * of constant cost if the input is UTF-8 but can be costly if run
 * on non-UTF-8 input.
 *
 * @deprecated Don't use.
 *
 * @param ctxt  an XML parser context
 * @returns the index in bytes from the beginning of the entity or -1
 *         in case the index could not be computed.
 */
long
xmlByteConsumed(xmlParserCtxt *ctxt) {
    xmlParserInputPtr in;

    if (ctxt == NULL)
        return(-1);
    in = ctxt->input;
    if (in == NULL)
        return(-1);

    if ((in->buf != NULL) && (in->buf->encoder != NULL)) {
        int unused = 0;
	xmlCharEncodingHandler * handler = in->buf->encoder;

        /*
	 * Encoding conversion, compute the number of unused original
	 * bytes from the input not consumed and subtract that from
	 * the raw consumed value, this is not a cheap operation
	 */
        if (in->end - in->cur > 0) {
	    unsigned char *convbuf;
	    const unsigned char *cur = (const unsigned char *)in->cur;
	    int toconv, ret;

            convbuf = xmlMalloc(32000);
            if (convbuf == NULL)
                return(-1);

            toconv = in->end - cur;
            unused = 32000;
            ret = xmlEncOutputChunk(handler, convbuf, &unused, cur, &toconv);

            xmlFree(convbuf);

            if (ret != XML_ENC_ERR_SUCCESS)
                return(-1);
	}

	if (in->buf->rawconsumed < (unsigned long) unused)
	    return(-1);
	return(in->buf->rawconsumed - unused);
    }

    return(in->consumed + (in->cur - in->base));
}

/************************************************************************
 *									*
 *		Conversions To/From UTF8 encoding			*
 *									*
 ************************************************************************/

static xmlCharEncError
asciiToAscii(void *vctxt ATTRIBUTE_UNUSED,
             unsigned char* out, int *poutlen,
             const unsigned char* in, int *pinlen,
             int flush ATTRIBUTE_UNUSED) {
    const unsigned char *inend;
    const unsigned char *instart = in;
    int inlen, outlen, ret;

    if (in == NULL) {
        *pinlen = 0;
        *poutlen = 0;
        return(XML_ENC_ERR_SUCCESS);
    }

    inlen = *pinlen;
    outlen = *poutlen;

    if (outlen < inlen) {
        inlen = outlen;
        ret = XML_ENC_ERR_SPACE;
    } else {
        ret = inlen;
    }

    inend = in + inlen;
    *poutlen = inlen;
    *pinlen = inlen;

    while (in < inend) {
	unsigned c = *in;

        if (c >= 0x80) {
	    *poutlen = in - instart;
	    *pinlen = in - instart;
	    return(XML_ENC_ERR_INPUT);
	}

        in++;
	*out++ = c;
    }

    return(ret);
}

static xmlCharEncError
latin1ToUTF8(void *vctxt ATTRIBUTE_UNUSED,
             unsigned char* out, int *outlen,
             const unsigned char* in, int *inlen,
             int flush ATTRIBUTE_UNUSED) {
    unsigned char* outstart = out;
    const unsigned char* instart = in;
    unsigned char* outend;
    const unsigned char* inend;
    int ret = XML_ENC_ERR_SPACE;

    if ((out == NULL) || (in == NULL) || (outlen == NULL) || (inlen == NULL))
	return(XML_ENC_ERR_INTERNAL);

    outend = out + *outlen;
    inend = in + *inlen;

    while (in < inend) {
        unsigned c = *in;

	if (c < 0x80) {
            if (out >= outend)
                goto done;
            *out++ = c;
	} else {
            if (outend - out < 2)
                goto done;
	    *out++ = (c >> 6) | 0xC0;
            *out++ = (c & 0x3F) | 0x80;
        }

        in++;
    }

    ret = out - outstart;

done:
    *outlen = out - outstart;
    *inlen = in - instart;
    return(ret);
}

/**
 * Take a block of ISO Latin 1 chars in and try to convert it to an UTF-8
 * block of chars out.
 *
 * The value of `inlen` after return is the number of bytes consumed.
 * The value of `outlen` after return is the number of bytes produced.
 *
 * @param out  a pointer to an array of bytes to store the result
 * @param outlen  the length of `out`
 * @param in  a pointer to an array of ISO Latin 1 chars
 * @param inlen  the length of `in`
 * @returns the number of bytes written or an xmlCharEncError code.
 */
int
xmlIsolat1ToUTF8(unsigned char* out, int *outlen,
                 const unsigned char* in, int *inlen) {
    return(latin1ToUTF8(/* ctxt */ NULL, out, outlen, in, inlen,
                        /* flush */ 0));
}

static xmlCharEncError
UTF8ToUTF8(void *vctxt ATTRIBUTE_UNUSED,
           unsigned char* out, int *outlen,
           const unsigned char* in, int *inlen,
           int flush ATTRIBUTE_UNUSED) {
    int len;
    int ret;

    if (in == NULL) {
        *inlen = 0;
        *outlen = 0;
        return(XML_ENC_ERR_SUCCESS);
    }

    if (*outlen < *inlen) {
	len = *outlen;
        ret = XML_ENC_ERR_SPACE;
    } else {
	len = *inlen;
        ret = len;
    }

    memcpy(out, in, len);

    *outlen = len;
    *inlen = len;
    return(ret);
}


#ifdef LIBXML_OUTPUT_ENABLED
static xmlCharEncError
UTF8ToLatin1(void *vctxt ATTRIBUTE_UNUSED,
             unsigned char* out, int *outlen,
             const unsigned char* in, int *inlen,
             int flush ATTRIBUTE_UNUSED) {
    const unsigned char* outend;
    const unsigned char* outstart = out;
    const unsigned char* instart = in;
    const unsigned char* inend;
    unsigned c;
    int ret = XML_ENC_ERR_SPACE;

    if ((out == NULL) || (outlen == NULL) || (inlen == NULL))
        return(XML_ENC_ERR_INTERNAL);

    if (in == NULL) {
        *inlen = 0;
        *outlen = 0;
        return(XML_ENC_ERR_SUCCESS);
    }

    inend = in + *inlen;
    outend = out + *outlen;
    while (in < inend) {
        if (out >= outend)
            goto done;

	c = *in;

        if (c < 0x80) {
            *out++ = c;
        } else if ((c >= 0xC2) && (c <= 0xC3)) {
            if (inend - in < 2)
                break;
            in++;
            *out++ = (unsigned char) ((c << 6) | (*in & 0x3F));
        } else {
            ret = XML_ENC_ERR_INPUT;
            goto done;
	}

        in++;
    }

    ret = out - outstart;

done:
    *outlen = out - outstart;
    *inlen = in - instart;
    return(ret);
}

/**
 * Take a block of UTF-8 chars in and try to convert it to an ISO Latin 1
 * block of chars out.
 *
 * The value of `inlen` after return is the number of bytes consumed.
 * The value of `outlen` after return is the number of bytes produced.
 *
 * @param out  a pointer to an array of bytes to store the result
 * @param outlen  the length of `out`
 * @param in  a pointer to an array of UTF-8 chars
 * @param inlen  the length of `in`
 * @returns the number of bytes written or an xmlCharEncError code.
 */
int
xmlUTF8ToIsolat1(unsigned char* out, int *outlen,
              const unsigned char* in, int *inlen) {
    if ((out == NULL) || (outlen == NULL) || (in == NULL) || (inlen == NULL))
        return(XML_ENC_ERR_INTERNAL);

    return(UTF8ToLatin1(/* ctxt */ NULL, out, outlen, in, inlen,
                        /* flush */ 0));
}
#endif /* LIBXML_OUTPUT_ENABLED */

static xmlCharEncError
UTF16LEToUTF8(void *vctxt ATTRIBUTE_UNUSED,
              unsigned char *out, int *outlen,
              const unsigned char *in, int *inlen,
              int flush ATTRIBUTE_UNUSED) {
    const unsigned char *instart = in;
    const unsigned char *inend = in + (*inlen & ~1);
    unsigned char *outstart = out;
    unsigned char *outend = out + *outlen;
    unsigned c, d;
    int ret = XML_ENC_ERR_SPACE;

    while (in < inend) {
        c = in[0] | (in[1] << 8);

        if (c < 0x80) {
            if (out >= outend)
                goto done;
            out[0] = c;
            in += 2;
            out += 1;
        } else if (c < 0x800) {
            if (outend - out < 2)
                goto done;
            out[0] = (c >> 6)   | 0xC0;
            out[1] = (c & 0x3F) | 0x80;
            in += 2;
            out += 2;
        } else if ((c & 0xF800) != 0xD800) {
            if (outend - out < 3)
                goto done;
            out[0] =  (c >> 12)         | 0xE0;
            out[1] = ((c >>  6) & 0x3F) | 0x80;
            out[2] =  (c        & 0x3F) | 0x80;
            in += 2;
            out += 3;
        } else {
            /* Surrogate pair */
            if ((c & 0xFC00) != 0xD800) {
                ret = XML_ENC_ERR_INPUT;
                goto done;
            }
	    if (inend - in < 4)
		break;
            d = in[2] | (in[3] << 8);
            if ((d & 0xFC00) != 0xDC00) {
                ret = XML_ENC_ERR_INPUT;
                goto done;
            }
	    if (outend - out < 4)
		goto done;
            c = (c << 10) + d - ((0xD800 << 10) + 0xDC00 - 0x10000);
            out[0] =  (c >> 18)         | 0xF0;
            out[1] = ((c >> 12) & 0x3F) | 0x80;
            out[2] = ((c >>  6) & 0x3F) | 0x80;
            out[3] =  (c        & 0x3F) | 0x80;
            in += 4;
            out += 4;
        }
    }

    ret = out - outstart;

done:
    *outlen = out - outstart;
    *inlen = in - instart;
    return(ret);
}

#ifdef LIBXML_OUTPUT_ENABLED
static xmlCharEncError
UTF8ToUTF16LE(void *vctxt ATTRIBUTE_UNUSED,
              unsigned char *out, int *outlen,
              const unsigned char *in, int *inlen,
              int flush ATTRIBUTE_UNUSED) {
    const unsigned char *instart = in;
    const unsigned char *inend;
    unsigned char *outstart = out;
    unsigned char *outend;
    unsigned c, d;
    int ret = XML_ENC_ERR_SPACE;

    /* UTF16LE encoding has no BOM */
    if ((out == NULL) || (outlen == NULL) || (inlen == NULL))
        return(XML_ENC_ERR_INTERNAL);
    if (in == NULL) {
	*outlen = 0;
	*inlen = 0;
	return(0);
    }
    inend = in + *inlen;
    outend = out + (*outlen & ~1);
    while (in < inend) {
        c = in[0];

        if (c < 0x80) {
            if (out >= outend)
                goto done;
            out[0] = c;
            out[1] = 0;
            in += 1;
            out += 2;
        } else {
            int i, len;
            unsigned min;

            if (c < 0xE0) {
                if (c < 0xC2) {
                    ret = XML_ENC_ERR_INPUT;
                    goto done;
                }
                c &= 0x1F;
                len = 2;
                min = 0x80;
            } else if (c < 0xF0) {
                c &= 0x0F;
                len = 3;
                min = 0x800;
            } else {
                c &= 0x0F;
                len = 4;
                min = 0x10000;
            }

            if (inend - in < len)
                break;

            for (i = 1; i < len; i++) {
                if ((in[i] & 0xC0) != 0x80) {
                    ret = XML_ENC_ERR_INPUT;
                    goto done;
                }
                c = (c << 6) | (in[i] & 0x3F);
            }

            if ((c < min) ||
                ((c >= 0xD800) && (c <= 0xDFFF)) ||
                (c > 0x10FFFF)) {
                ret = XML_ENC_ERR_INPUT;
                goto done;
            }

            if (c < 0x10000) {
                if (out >= outend)
                    goto done;
                out[0] = c & 0xFF;
                out[1] = c >> 8;
                out += 2;
            } else {
                if (outend - out < 4)
                    goto done;
                c -= 0x10000;
                d = (c & 0x03FF) | 0xDC00;
                c = (c >> 10)    | 0xD800;
                out[0] = c & 0xFF;
                out[1] = c >> 8;
                out[2] = d & 0xFF;
                out[3] = d >> 8;
                out += 4;
            }

            in += len;
        }
    }

    ret = out - outstart;

done:
    *outlen = out - outstart;
    *inlen = in - instart;
    return(ret);
}

static xmlCharEncError
UTF8ToUTF16(void *vctxt,
            unsigned char* outb, int *outlen,
            const unsigned char* in, int *inlen,
            int flush) {
    if (in == NULL) {
	/*
	 * initialization, add the Byte Order Mark for UTF-16LE
	 */
        if (*outlen >= 2) {
	    outb[0] = 0xFF;
	    outb[1] = 0xFE;
	    *outlen = 2;
	    *inlen = 0;
	    return(2);
	}
	*outlen = 0;
	*inlen = 0;
	return(0);
    }
    return (UTF8ToUTF16LE(vctxt, outb, outlen, in, inlen, flush));
}
#endif /* LIBXML_OUTPUT_ENABLED */

static xmlCharEncError
UTF16BEToUTF8(void *vctxt ATTRIBUTE_UNUSED,
              unsigned char *out, int *outlen,
              const unsigned char *in, int *inlen,
              int flush ATTRIBUTE_UNUSED) {
    const unsigned char *instart = in;
    const unsigned char *inend = in + (*inlen & ~1);
    unsigned char *outstart = out;
    unsigned char *outend = out + *outlen;
    unsigned c, d;
    int ret = XML_ENC_ERR_SPACE;

    while (in < inend) {
        c = (in[0] << 8) | in[1];

        if (c < 0x80) {
            if (out >= outend)
                goto done;
            out[0] = c;
            in += 2;
            out += 1;
        } else if (c < 0x800) {
            if (outend - out < 2)
                goto done;
            out[0] = (c >> 6)   | 0xC0;
            out[1] = (c & 0x3F) | 0x80;
            in += 2;
            out += 2;
        } else if ((c & 0xF800) != 0xD800) {
            if (outend - out < 3)
                goto done;
            out[0] =  (c >> 12)         | 0xE0;
            out[1] = ((c >>  6) & 0x3F) | 0x80;
            out[2] =  (c        & 0x3F) | 0x80;
            in += 2;
            out += 3;
        } else {
            /* Surrogate pair */
            if ((c & 0xFC00) != 0xD800) {
                ret = XML_ENC_ERR_INPUT;
                goto done;
            }
	    if (inend - in < 4)
		break;
            d = (in[2] << 8) | in[3];
            if ((d & 0xFC00) != 0xDC00) {
                ret = XML_ENC_ERR_INPUT;
                goto done;
            }
	    if (outend - out < 4)
		goto done;
            c = (c << 10) + d - ((0xD800 << 10) + 0xDC00 - 0x10000);
            out[0] =  (c >> 18)         | 0xF0;
            out[1] = ((c >> 12) & 0x3F) | 0x80;
            out[2] = ((c >>  6) & 0x3F) | 0x80;
            out[3] =  (c        & 0x3F) | 0x80;
            in += 4;
            out += 4;
        }
    }

    ret = out - outstart;

done:
    *outlen = out - outstart;
    *inlen = in - instart;
    return(ret);
}

#ifdef LIBXML_OUTPUT_ENABLED
static xmlCharEncError
UTF8ToUTF16BE(void *vctxt ATTRIBUTE_UNUSED,
              unsigned char *out, int *outlen,
              const unsigned char *in, int *inlen,
              int flush ATTRIBUTE_UNUSED) {
    const unsigned char *instart = in;
    const unsigned char *inend;
    unsigned char *outstart = out;
    unsigned char *outend;
    unsigned c, d;
    int ret = XML_ENC_ERR_SPACE;

    /* UTF-16BE has no BOM */
    if ((out == NULL) || (outlen == NULL) || (inlen == NULL)) return(-1);
    if (in == NULL) {
	*outlen = 0;
	*inlen = 0;
	return(0);
    }
    inend = in + *inlen;
    outend = out + (*outlen & ~1);
    while (in < inend) {
        c = in[0];

        if (c < 0x80) {
            if (out >= outend)
                goto done;
            out[0] = 0;
            out[1] = c;
            in += 1;
            out += 2;
        } else {
            int i, len;
            unsigned min;

            if (c < 0xE0) {
                if (c < 0xC2) {
                    ret = XML_ENC_ERR_INPUT;
                    goto done;
                }
                c &= 0x1F;
                len = 2;
                min = 0x80;
            } else if (c < 0xF0) {
                c &= 0x0F;
                len = 3;
                min = 0x800;
            } else {
                c &= 0x0F;
                len = 4;
                min = 0x10000;
            }

            if (inend - in < len)
                break;

            for (i = 1; i < len; i++) {
                if ((in[i] & 0xC0) != 0x80) {
                    ret = XML_ENC_ERR_INPUT;
                    goto done;
                }
                c = (c << 6) | (in[i] & 0x3F);
            }

            if ((c < min) ||
                ((c >= 0xD800) && (c <= 0xDFFF)) ||
                (c > 0x10FFFF)) {
                ret = XML_ENC_ERR_INPUT;
                goto done;
            }

            if (c < 0x10000) {
                if (out >= outend)
                    goto done;
                out[0] = c >> 8;
                out[1] = c & 0xFF;
                out += 2;
            } else {
                if (outend - out < 4)
                    goto done;
                c -= 0x10000;
                d = (c & 0x03FF) | 0xDC00;
                c = (c >> 10)    | 0xD800;
                out[0] = c >> 8;
                out[1] = c & 0xFF;
                out[2] = d >> 8;
                out[3] = d & 0xFF;
                out += 4;
            }

            in += len;
        }
    }

    ret = out - outstart;

done:
    *outlen = out - outstart;
    *inlen = in - instart;
    return(ret);
}
#endif /* LIBXML_OUTPUT_ENABLED */

#if defined(LIBXML_OUTPUT_ENABLED) && defined(LIBXML_HTML_ENABLED)
static xmlCharEncError
UTF8ToHtmlWrapper(void *vctxt ATTRIBUTE_UNUSED,
                  unsigned char *out, int *outlen,
                  const unsigned char *in, int *inlen,
                  int flush ATTRIBUTE_UNUSED) {
    return(htmlUTF8ToHtml(out, outlen, in, inlen));
}
#endif

static xmlCharEncError
Utf8ToEightBit(void *vctxt,
               unsigned char *out, int *outlen,
               const unsigned char *in, int *inlen,
               int flush ATTRIBUTE_UNUSED) {
    const unsigned char *xlattable = vctxt;
    const unsigned char *instart = in;
    const unsigned char *inend;
    unsigned char *outstart = out;
    unsigned char *outend;
    int ret = XML_ENC_ERR_SPACE;

    if (in == NULL) {
        /*
        * initialization nothing to do
        */
        *outlen = 0;
        *inlen = 0;
        return(XML_ENC_ERR_SUCCESS);
    }

    inend = in + *inlen;
    outend = out + *outlen;
    while (in < inend) {
        unsigned d = *in;

        if  (d < 0x80)  {
            if (out >= outend)
                goto done;
            in += 1;
        } else if (d < 0xE0) {
            unsigned c;

            if (inend - in < 2)
                break;
            c = in[1] & 0x3F;
            d = d & 0x1F;
            d = xlattable [48 + c + xlattable [d] * 64];
            if (d == 0) {
                /* not in character set */
                ret = XML_ENC_ERR_INPUT;
                goto done;
            }
            if (out >= outend)
                goto done;
            in += 2;
        } else if (d < 0xF0) {
            unsigned c1;
            unsigned c2;

            if (inend - in < 3)
                break;
            c1 = in[1] & 0x3F;
            c2 = in[2] & 0x3F;
	    d = d & 0x0F;
	    d = xlattable [48 + c2 + xlattable [48 + c1 +
			xlattable [32 + d] * 64] * 64];
            if (d == 0) {
                /* not in character set */
                ret = XML_ENC_ERR_INPUT;
                goto done;
            }
            if (out >= outend)
                goto done;
            in += 3;
        } else {
            /* cannot transcode >= U+010000 */
                ret = XML_ENC_ERR_INPUT;
                goto done;
        }

        *out++ = d;
    }

    ret = out - outstart;

done:
    *outlen = out - outstart;
    *inlen = in - instart;
    return(ret);
}

static xmlCharEncError
EightBitToUtf8(void *vctxt,
               unsigned char* out, int *outlen,
               const unsigned char* in, int *inlen,
               int flush ATTRIBUTE_UNUSED) {
    unsigned short const *unicodetable = vctxt;
    const unsigned char* instart = in;
    const unsigned char* inend;
    unsigned char* outstart = out;
    unsigned char* outend;
    int ret = XML_ENC_ERR_SPACE;

    outend = out + *outlen;
    inend = in + *inlen;

    while (in < inend) {
        unsigned c = *in;

        if (c < 0x80) {
            if (out >= outend)
                goto done;
            *out++ = c;
        } else {
            c = unicodetable[c - 0x80];
            if (c == 0) {
                /* undefined code point */
                ret = XML_ENC_ERR_INPUT;
                goto done;
            }
            if (c < 0x800) {
                if (outend - out < 2)
                    goto done;
                *out++ = ((c >>  6) & 0x1F) | 0xC0;
                *out++ = (c & 0x3F) | 0x80;
            } else {
                if (outend - out < 3)
                    goto done;
                *out++ = ((c >>  12) & 0x0F) | 0xE0;
                *out++ = ((c >>  6) & 0x3F) | 0x80;
                *out++ = (c & 0x3F) | 0x80;
            }
        }

        in += 1;
    }

    ret = out - outstart;

done:
    *outlen = out - outstart;
    *inlen = in - instart;
    return(ret);
}


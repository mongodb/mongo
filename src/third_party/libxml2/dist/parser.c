/*
 * parser.c : an XML 1.0 parser, namespaces and validity support are mostly
 *            implemented on top of the SAX interfaces
 *
 * References:
 *   The XML specification:
 *     http://www.w3.org/TR/REC-xml
 *   Original 1.0 version:
 *     http://www.w3.org/TR/1998/REC-xml-19980210
 *   XML second edition working draft
 *     http://www.w3.org/TR/2000/WD-xml-2e-20000814
 *
 * Okay this is a big file, the parser core is around 7000 lines, then it
 * is followed by the progressive parser top routines, then the various
 * high level APIs to call the parser and a few miscellaneous functions.
 * A number of helper functions and deprecated ones have been moved to
 * parserInternals.c to reduce this file size.
 * As much as possible the functions are associated with their relative
 * production in the XML specification. A few productions defining the
 * different ranges of character are actually implanted either in
 * parserInternals.h or parserInternals.c
 * The DOM tree build is realized from the default SAX callbacks in
 * the module SAX2.c.
 * The routines doing the validation checks are in valid.c and called either
 * from the SAX callbacks or as standalone functions using a preparsed
 * document.
 *
 * See Copyright for the status of this software.
 *
 * Author: Daniel Veillard
 */

/* To avoid EBCDIC trouble when parsing on zOS */
#if defined(__MVS__)
#pragma convert("ISO8859-1")
#endif

#define IN_LIBXML
#include "libxml.h"

#if defined(_WIN32)
#define XML_DIR_SEP '\\'
#else
#define XML_DIR_SEP '/'
#endif

#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <stdarg.h>
#include <stddef.h>
#include <ctype.h>
#include <stdlib.h>
#include <libxml/parser.h>
#include <libxml/xmlmemory.h>
#include <libxml/tree.h>
#include <libxml/parserInternals.h>
#include <libxml/valid.h>
#include <libxml/entities.h>
#include <libxml/xmlerror.h>
#include <libxml/encoding.h>
#include <libxml/xmlIO.h>
#include <libxml/uri.h>
#include <libxml/SAX2.h>
#include <libxml/HTMLparser.h>
#ifdef LIBXML_CATALOG_ENABLED
#include <libxml/catalog.h>
#endif

#include "private/buf.h"
#include "private/dict.h"
#include "private/entities.h"
#include "private/error.h"
#include "private/html.h"
#include "private/io.h"
#include "private/memory.h"
#include "private/parser.h"
#include "private/tree.h"

#define NS_INDEX_EMPTY  INT_MAX
#define NS_INDEX_XML    (INT_MAX - 1)
#define URI_HASH_EMPTY  0xD943A04E
#define URI_HASH_XML    0xF0451F02

#ifndef STDIN_FILENO
  #define STDIN_FILENO 0
#endif

#ifndef SIZE_MAX
  #define SIZE_MAX ((size_t) -1)
#endif

#define XML_MAX_ATTRS 100000000 /* 100 million */

#define XML_SPECIAL_EXTERNAL    (1 << 20)
#define XML_SPECIAL_TYPE_MASK   (XML_SPECIAL_EXTERNAL - 1)

#define XML_ATTVAL_ALLOC        (1 << 0)
#define XML_ATTVAL_NORM_CHANGE  (1 << 1)

struct _xmlStartTag {
    const xmlChar *prefix;
    const xmlChar *URI;
    int line;
    int nsNr;
};

typedef struct {
    void *saxData;
    unsigned prefixHashValue;
    unsigned uriHashValue;
    unsigned elementId;
    int oldIndex;
} xmlParserNsExtra;

typedef struct {
    unsigned hashValue;
    int index;
} xmlParserNsBucket;

struct _xmlParserNsData {
    xmlParserNsExtra *extra;

    unsigned hashSize;
    unsigned hashElems;
    xmlParserNsBucket *hash;

    unsigned elementId;
    int defaultNsIndex;
    int minNsIndex;
};

static int
xmlParseElementStart(xmlParserCtxtPtr ctxt);

static void
xmlParseElementEnd(xmlParserCtxtPtr ctxt);

static xmlEntityPtr
xmlLookupGeneralEntity(xmlParserCtxtPtr ctxt, const xmlChar *name, int inAttr);

static const xmlChar *
xmlParseEntityRefInternal(xmlParserCtxtPtr ctxt);

/************************************************************************
 *									*
 *	Arbitrary limits set in the parser. See XML_PARSE_HUGE		*
 *									*
 ************************************************************************/

#define XML_PARSER_BIG_ENTITY 1000
#define XML_PARSER_LOT_ENTITY 5000

/*
 * Constants for protection against abusive entity expansion
 * ("billion laughs").
 */

/*
 * A certain amount of entity expansion which is always allowed.
 */
#define XML_PARSER_ALLOWED_EXPANSION 1000000

/*
 * Fixed cost for each entity reference. This crudely models processing time
 * as well to protect, for example, against exponential expansion of empty
 * or very short entities.
 */
#define XML_ENT_FIXED_COST 20

#define XML_PARSER_BIG_BUFFER_SIZE 300
#define XML_PARSER_BUFFER_SIZE 100
#define SAX_COMPAT_MODE BAD_CAST "SAX compatibility mode document"

/**
 * XML_PARSER_CHUNK_SIZE
 *
 * When calling GROW that's the minimal amount of data
 * the parser expected to have received. It is not a hard
 * limit but an optimization when reading strings like Names
 * It is not strictly needed as long as inputs available characters
 * are followed by 0, which should be provided by the I/O level
 */
#define XML_PARSER_CHUNK_SIZE 100

/**
 * Constant string describing the version of the library used at
 * run-time.
 */
const char *const
xmlParserVersion = LIBXML_VERSION_STRING LIBXML_VERSION_EXTRA;

/*
 * List of XML prefixed PI allowed by W3C specs
 */

static const char* const xmlW3CPIs[] = {
    "xml-stylesheet",
    "xml-model",
    NULL
};


/* DEPR void xmlParserHandleReference(xmlParserCtxtPtr ctxt); */
static xmlEntityPtr xmlParseStringPEReference(xmlParserCtxtPtr ctxt,
                                              const xmlChar **str);

static void
xmlCtxtParseEntity(xmlParserCtxtPtr ctxt, xmlEntityPtr ent);

static int
xmlLoadEntityContent(xmlParserCtxtPtr ctxt, xmlEntityPtr entity);

static void
xmlParsePERefInternal(xmlParserCtxt *ctxt, int markupDecl);

/************************************************************************
 *									*
 *		Some factorized error routines				*
 *									*
 ************************************************************************/

static void
xmlErrMemory(xmlParserCtxtPtr ctxt) {
    xmlCtxtErrMemory(ctxt);
}

/**
 * Handle a redefinition of attribute error
 *
 * @param ctxt  an XML parser context
 * @param prefix  the attribute prefix
 * @param localname  the attribute localname
 */
static void
xmlErrAttributeDup(xmlParserCtxtPtr ctxt, const xmlChar * prefix,
                   const xmlChar * localname)
{
    if (prefix == NULL)
        xmlCtxtErr(ctxt, NULL, XML_FROM_PARSER, XML_ERR_ATTRIBUTE_REDEFINED,
                   XML_ERR_FATAL, localname, NULL, NULL, 0,
                   "Attribute %s redefined\n", localname);
    else
        xmlCtxtErr(ctxt, NULL, XML_FROM_PARSER, XML_ERR_ATTRIBUTE_REDEFINED,
                   XML_ERR_FATAL, prefix, localname, NULL, 0,
                   "Attribute %s:%s redefined\n", prefix, localname);
}

/**
 * Handle a fatal parser error, i.e. violating Well-Formedness constraints
 *
 * @param ctxt  an XML parser context
 * @param error  the error number
 * @param msg  the error message
 */
static void LIBXML_ATTR_FORMAT(3,0)
xmlFatalErrMsg(xmlParserCtxtPtr ctxt, xmlParserErrors error,
               const char *msg)
{
    xmlCtxtErr(ctxt, NULL, XML_FROM_PARSER, error, XML_ERR_FATAL,
               NULL, NULL, NULL, 0, "%s", msg);
}

/**
 * Handle a warning.
 *
 * @param ctxt  an XML parser context
 * @param error  the error number
 * @param msg  the error message
 * @param str1  extra data
 * @param str2  extra data
 */
void LIBXML_ATTR_FORMAT(3,0)
xmlWarningMsg(xmlParserCtxtPtr ctxt, xmlParserErrors error,
              const char *msg, const xmlChar *str1, const xmlChar *str2)
{
    xmlCtxtErr(ctxt, NULL, XML_FROM_PARSER, error, XML_ERR_WARNING,
               str1, str2, NULL, 0, msg, str1, str2);
}

#ifdef LIBXML_VALID_ENABLED
/**
 * Handle a validity error.
 *
 * @param ctxt  an XML parser context
 * @param error  the error number
 * @param msg  the error message
 * @param str1  extra data
 * @param str2  extra data
 */
static void LIBXML_ATTR_FORMAT(3,0)
xmlValidityError(xmlParserCtxtPtr ctxt, xmlParserErrors error,
              const char *msg, const xmlChar *str1, const xmlChar *str2)
{
    ctxt->valid = 0;

    xmlCtxtErr(ctxt, NULL, XML_FROM_DTD, error, XML_ERR_ERROR,
               str1, str2, NULL, 0, msg, str1, str2);
}
#endif

/**
 * Handle a fatal parser error, i.e. violating Well-Formedness constraints
 *
 * @param ctxt  an XML parser context
 * @param error  the error number
 * @param msg  the error message
 * @param val  an integer value
 */
static void LIBXML_ATTR_FORMAT(3,0)
xmlFatalErrMsgInt(xmlParserCtxtPtr ctxt, xmlParserErrors error,
                  const char *msg, int val)
{
    xmlCtxtErr(ctxt, NULL, XML_FROM_PARSER, error, XML_ERR_FATAL,
               NULL, NULL, NULL, val, msg, val);
}

/**
 * Handle a fatal parser error, i.e. violating Well-Formedness constraints
 *
 * @param ctxt  an XML parser context
 * @param error  the error number
 * @param msg  the error message
 * @param str1  an string info
 * @param val  an integer value
 * @param str2  an string info
 */
static void LIBXML_ATTR_FORMAT(3,0)
xmlFatalErrMsgStrIntStr(xmlParserCtxtPtr ctxt, xmlParserErrors error,
                  const char *msg, const xmlChar *str1, int val,
		  const xmlChar *str2)
{
    xmlCtxtErr(ctxt, NULL, XML_FROM_PARSER, error, XML_ERR_FATAL,
               str1, str2, NULL, val, msg, str1, val, str2);
}

/**
 * Handle a fatal parser error, i.e. violating Well-Formedness constraints
 *
 * @param ctxt  an XML parser context
 * @param error  the error number
 * @param msg  the error message
 * @param val  a string value
 */
static void LIBXML_ATTR_FORMAT(3,0)
xmlFatalErrMsgStr(xmlParserCtxtPtr ctxt, xmlParserErrors error,
                  const char *msg, const xmlChar * val)
{
    xmlCtxtErr(ctxt, NULL, XML_FROM_PARSER, error, XML_ERR_FATAL,
               val, NULL, NULL, 0, msg, val);
}

/**
 * Handle a non fatal parser error
 *
 * @param ctxt  an XML parser context
 * @param error  the error number
 * @param msg  the error message
 * @param val  a string value
 */
static void LIBXML_ATTR_FORMAT(3,0)
xmlErrMsgStr(xmlParserCtxtPtr ctxt, xmlParserErrors error,
                  const char *msg, const xmlChar * val)
{
    xmlCtxtErr(ctxt, NULL, XML_FROM_PARSER, error, XML_ERR_ERROR,
               val, NULL, NULL, 0, msg, val);
}

/**
 * Handle a fatal parser error, i.e. violating Well-Formedness constraints
 *
 * @param ctxt  an XML parser context
 * @param error  the error number
 * @param msg  the message
 * @param info1  extra information string
 * @param info2  extra information string
 * @param info3  extra information string
 */
static void LIBXML_ATTR_FORMAT(3,0)
xmlNsErr(xmlParserCtxtPtr ctxt, xmlParserErrors error,
         const char *msg,
         const xmlChar * info1, const xmlChar * info2,
         const xmlChar * info3)
{
    ctxt->nsWellFormed = 0;

    xmlCtxtErr(ctxt, NULL, XML_FROM_NAMESPACE, error, XML_ERR_ERROR,
               info1, info2, info3, 0, msg, info1, info2, info3);
}

/**
 * Handle a namespace warning error
 *
 * @param ctxt  an XML parser context
 * @param error  the error number
 * @param msg  the message
 * @param info1  extra information string
 * @param info2  extra information string
 * @param info3  extra information string
 */
static void LIBXML_ATTR_FORMAT(3,0)
xmlNsWarn(xmlParserCtxtPtr ctxt, xmlParserErrors error,
         const char *msg,
         const xmlChar * info1, const xmlChar * info2,
         const xmlChar * info3)
{
    xmlCtxtErr(ctxt, NULL, XML_FROM_NAMESPACE, error, XML_ERR_WARNING,
               info1, info2, info3, 0, msg, info1, info2, info3);
}

/**
 * Check for non-linear entity expansion behaviour.
 *
 * In some cases like xmlExpandEntityInAttValue, this function is called
 * for each, possibly nested entity and its unexpanded content length.
 *
 * In other cases like #xmlParseReference, it's only called for each
 * top-level entity with its unexpanded content length plus the sum of
 * the unexpanded content lengths (plus fixed cost) of all nested
 * entities.
 *
 * Summing the unexpanded lengths also adds the length of the reference.
 * This is by design. Taking the length of the entity name into account
 * discourages attacks that try to waste CPU time with abusively long
 * entity names. See test/recurse/lol6.xml for example. Each call also
 * adds some fixed cost XML_ENT_FIXED_COST to discourage attacks with
 * short entities.
 *
 * @param ctxt  parser context
 * @param extra  sum of unexpanded entity sizes
 * @returns 1 on error, 0 on success.
 */
static int
xmlParserEntityCheck(xmlParserCtxtPtr ctxt, unsigned long extra)
{
    unsigned long consumed;
    unsigned long *expandedSize;
    xmlParserInputPtr input = ctxt->input;
    xmlEntityPtr entity = input->entity;

    if ((entity) && (entity->flags & XML_ENT_CHECKED))
        return(0);

    /*
     * Compute total consumed bytes so far, including input streams of
     * external entities.
     */
    consumed = input->consumed;
    xmlSaturatedAddSizeT(&consumed, input->cur - input->base);
    xmlSaturatedAdd(&consumed, ctxt->sizeentities);

    if (entity)
        expandedSize = &entity->expandedSize;
    else
        expandedSize = &ctxt->sizeentcopy;

    /*
     * Add extra cost and some fixed cost.
     */
    xmlSaturatedAdd(expandedSize, extra);
    xmlSaturatedAdd(expandedSize, XML_ENT_FIXED_COST);

    /*
     * It's important to always use saturation arithmetic when tracking
     * entity sizes to make the size checks reliable. If "sizeentcopy"
     * overflows, we have to abort.
     */
    if ((*expandedSize > XML_PARSER_ALLOWED_EXPANSION) &&
        ((*expandedSize >= ULONG_MAX) ||
         (*expandedSize / ctxt->maxAmpl > consumed))) {
        xmlFatalErrMsg(ctxt, XML_ERR_RESOURCE_LIMIT,
                       "Maximum entity amplification factor exceeded, see "
                       "xmlCtxtSetMaxAmplification.\n");
        return(1);
    }

    return(0);
}

/************************************************************************
 *									*
 *		Library wide options					*
 *									*
 ************************************************************************/

/**
 * Examines if the library has been compiled with a given feature.
 *
 * @param feature  the feature to be examined
 * @returns zero (0) if the feature does not exist or an unknown
 * feature is requested, non-zero otherwise.
 */
int
xmlHasFeature(xmlFeature feature)
{
    switch (feature) {
	case XML_WITH_THREAD:
#ifdef LIBXML_THREAD_ENABLED
	    return(1);
#else
	    return(0);
#endif
        case XML_WITH_TREE:
            return(1);
        case XML_WITH_OUTPUT:
#ifdef LIBXML_OUTPUT_ENABLED
            return(1);
#else
            return(0);
#endif
        case XML_WITH_PUSH:
#ifdef LIBXML_PUSH_ENABLED
            return(1);
#else
            return(0);
#endif
        case XML_WITH_READER:
#ifdef LIBXML_READER_ENABLED
            return(1);
#else
            return(0);
#endif
        case XML_WITH_PATTERN:
#ifdef LIBXML_PATTERN_ENABLED
            return(1);
#else
            return(0);
#endif
        case XML_WITH_WRITER:
#ifdef LIBXML_WRITER_ENABLED
            return(1);
#else
            return(0);
#endif
        case XML_WITH_SAX1:
#ifdef LIBXML_SAX1_ENABLED
            return(1);
#else
            return(0);
#endif
        case XML_WITH_HTTP:
            return(0);
        case XML_WITH_VALID:
#ifdef LIBXML_VALID_ENABLED
            return(1);
#else
            return(0);
#endif
        case XML_WITH_HTML:
#ifdef LIBXML_HTML_ENABLED
            return(1);
#else
            return(0);
#endif
        case XML_WITH_LEGACY:
            return(0);
        case XML_WITH_C14N:
#ifdef LIBXML_C14N_ENABLED
            return(1);
#else
            return(0);
#endif
        case XML_WITH_CATALOG:
#ifdef LIBXML_CATALOG_ENABLED
            return(1);
#else
            return(0);
#endif
        case XML_WITH_XPATH:
#ifdef LIBXML_XPATH_ENABLED
            return(1);
#else
            return(0);
#endif
        case XML_WITH_XPTR:
#ifdef LIBXML_XPTR_ENABLED
            return(1);
#else
            return(0);
#endif
        case XML_WITH_XINCLUDE:
#ifdef LIBXML_XINCLUDE_ENABLED
            return(1);
#else
            return(0);
#endif
        case XML_WITH_ICONV:
#ifdef LIBXML_ICONV_ENABLED
            return(1);
#else
            return(0);
#endif
        case XML_WITH_ISO8859X:
#ifdef LIBXML_ISO8859X_ENABLED
            return(1);
#else
            return(0);
#endif
        case XML_WITH_UNICODE:
            return(0);
        case XML_WITH_REGEXP:
#ifdef LIBXML_REGEXP_ENABLED
            return(1);
#else
            return(0);
#endif
        case XML_WITH_AUTOMATA:
#ifdef LIBXML_REGEXP_ENABLED
            return(1);
#else
            return(0);
#endif
        case XML_WITH_EXPR:
            return(0);
        case XML_WITH_RELAXNG:
#ifdef LIBXML_RELAXNG_ENABLED
            return(1);
#else
            return(0);
#endif
        case XML_WITH_SCHEMAS:
#ifdef LIBXML_SCHEMAS_ENABLED
            return(1);
#else
            return(0);
#endif
        case XML_WITH_SCHEMATRON:
#ifdef LIBXML_SCHEMATRON_ENABLED
            return(1);
#else
            return(0);
#endif
        case XML_WITH_MODULES:
#ifdef LIBXML_MODULES_ENABLED
            return(1);
#else
            return(0);
#endif
        case XML_WITH_DEBUG:
#ifdef LIBXML_DEBUG_ENABLED
            return(1);
#else
            return(0);
#endif
        case XML_WITH_DEBUG_MEM:
            return(0);
        case XML_WITH_ZLIB:
#ifdef LIBXML_ZLIB_ENABLED
            return(1);
#else
            return(0);
#endif
        case XML_WITH_LZMA:
            return(0);
        case XML_WITH_ICU:
#ifdef LIBXML_ICU_ENABLED
            return(1);
#else
            return(0);
#endif
        default:
	    break;
     }
     return(0);
}

/************************************************************************
 *									*
 *			Simple string buffer				*
 *									*
 ************************************************************************/

typedef struct {
    xmlChar *mem;
    unsigned size;
    unsigned cap; /* size < cap */
    unsigned max; /* size <= max */
    xmlParserErrors code;
} xmlSBuf;

static void
xmlSBufInit(xmlSBuf *buf, unsigned max) {
    buf->mem = NULL;
    buf->size = 0;
    buf->cap = 0;
    buf->max = max;
    buf->code = XML_ERR_OK;
}

static int
xmlSBufGrow(xmlSBuf *buf, unsigned len) {
    xmlChar *mem;
    unsigned cap;

    if (len >= UINT_MAX / 2 - buf->size) {
        if (buf->code == XML_ERR_OK)
            buf->code = XML_ERR_RESOURCE_LIMIT;
        return(-1);
    }

    cap = (buf->size + len) * 2;
    if (cap < 240)
        cap = 240;

    mem = xmlRealloc(buf->mem, cap);
    if (mem == NULL) {
        buf->code = XML_ERR_NO_MEMORY;
        return(-1);
    }

    buf->mem = mem;
    buf->cap = cap;

    return(0);
}

static void
xmlSBufAddString(xmlSBuf *buf, const xmlChar *str, unsigned len) {
    if (buf->max - buf->size < len) {
        if (buf->code == XML_ERR_OK)
            buf->code = XML_ERR_RESOURCE_LIMIT;
        return;
    }

    if (buf->cap - buf->size <= len) {
        if (xmlSBufGrow(buf, len) < 0)
            return;
    }

    if (len > 0)
        memcpy(buf->mem + buf->size, str, len);
    buf->size += len;
}

static void
xmlSBufAddCString(xmlSBuf *buf, const char *str, unsigned len) {
    xmlSBufAddString(buf, (const xmlChar *) str, len);
}

static void
xmlSBufAddChar(xmlSBuf *buf, int c) {
    xmlChar *end;

    if (buf->max - buf->size < 4) {
        if (buf->code == XML_ERR_OK)
            buf->code = XML_ERR_RESOURCE_LIMIT;
        return;
    }

    if (buf->cap - buf->size <= 4) {
        if (xmlSBufGrow(buf, 4) < 0)
            return;
    }

    end = buf->mem + buf->size;

    if (c < 0x80) {
        *end = (xmlChar) c;
        buf->size += 1;
    } else {
        buf->size += xmlCopyCharMultiByte(end, c);
    }
}

static void
xmlSBufAddReplChar(xmlSBuf *buf) {
    xmlSBufAddCString(buf, "\xEF\xBF\xBD", 3);
}

static void
xmlSBufReportError(xmlSBuf *buf, xmlParserCtxtPtr ctxt, const char *errMsg) {
    if (buf->code == XML_ERR_NO_MEMORY)
        xmlCtxtErrMemory(ctxt);
    else
        xmlFatalErr(ctxt, buf->code, errMsg);
}

static xmlChar *
xmlSBufFinish(xmlSBuf *buf, int *sizeOut, xmlParserCtxtPtr ctxt,
              const char *errMsg) {
    if (buf->mem == NULL) {
        buf->mem = xmlMalloc(1);
        if (buf->mem == NULL) {
            buf->code = XML_ERR_NO_MEMORY;
        } else {
            buf->mem[0] = 0;
        }
    } else {
        buf->mem[buf->size] = 0;
    }

    if (buf->code == XML_ERR_OK) {
        if (sizeOut != NULL)
            *sizeOut = buf->size;
        return(buf->mem);
    }

    xmlSBufReportError(buf, ctxt, errMsg);

    xmlFree(buf->mem);

    if (sizeOut != NULL)
        *sizeOut = 0;
    return(NULL);
}

static void
xmlSBufCleanup(xmlSBuf *buf, xmlParserCtxtPtr ctxt, const char *errMsg) {
    if (buf->code != XML_ERR_OK)
        xmlSBufReportError(buf, ctxt, errMsg);

    xmlFree(buf->mem);
}

static int
xmlUTF8MultibyteLen(xmlParserCtxtPtr ctxt, const xmlChar *str,
                    const char *errMsg) {
    int c = str[0];
    int c1 = str[1];

    if ((c1 & 0xC0) != 0x80)
        goto encoding_error;

    if (c < 0xE0) {
        /* 2-byte sequence */
        if (c < 0xC2)
            goto encoding_error;

        return(2);
    } else {
        int c2 = str[2];

        if ((c2 & 0xC0) != 0x80)
            goto encoding_error;

        if (c < 0xF0) {
            /* 3-byte sequence */
            if (c == 0xE0) {
                /* overlong */
                if (c1 < 0xA0)
                    goto encoding_error;
            } else if (c == 0xED) {
                /* surrogate */
                if (c1 >= 0xA0)
                    goto encoding_error;
            } else if (c == 0xEF) {
                /* U+FFFE and U+FFFF are invalid Chars */
                if ((c1 == 0xBF) && (c2 >= 0xBE))
                    xmlFatalErrMsg(ctxt, XML_ERR_INVALID_CHAR, errMsg);
            }

            return(3);
        } else {
            /* 4-byte sequence */
            if ((str[3] & 0xC0) != 0x80)
                goto encoding_error;
            if (c == 0xF0) {
                /* overlong */
                if (c1 < 0x90)
                    goto encoding_error;
            } else if (c >= 0xF4) {
                /* greater than 0x10FFFF */
                if ((c > 0xF4) || (c1 >= 0x90))
                    goto encoding_error;
            }

            return(4);
        }
    }

encoding_error:
    /* Only report the first error */
    if ((ctxt->input->flags & XML_INPUT_ENCODING_ERROR) == 0) {
        xmlCtxtErrIO(ctxt, XML_ERR_INVALID_ENCODING, NULL);
        ctxt->input->flags |= XML_INPUT_ENCODING_ERROR;
    }

    return(0);
}

/************************************************************************
 *									*
 *		SAX2 defaulted attributes handling			*
 *									*
 ************************************************************************/

/**
 * Final initialization of the parser context before starting to parse.
 *
 * This accounts for users modifying struct members of parser context
 * directly.
 *
 * @param ctxt  an XML parser context
 */
static void
xmlCtxtInitializeLate(xmlParserCtxtPtr ctxt) {
    xmlSAXHandlerPtr sax;

    /* Avoid unused variable warning if features are disabled. */
    (void) sax;

    /*
     * Changing the SAX struct directly is still widespread practice
     * in internal and external code.
     */
    if (ctxt == NULL) return;
    sax = ctxt->sax;
#ifdef LIBXML_SAX1_ENABLED
    /*
     * Only enable SAX2 if there SAX2 element handlers, except when there
     * are no element handlers at all.
     */
    if (((ctxt->options & XML_PARSE_SAX1) == 0) &&
        (sax) &&
        (sax->initialized == XML_SAX2_MAGIC) &&
        ((sax->startElementNs != NULL) ||
         (sax->endElementNs != NULL) ||
         ((sax->startElement == NULL) && (sax->endElement == NULL))))
        ctxt->sax2 = 1;
#else
    ctxt->sax2 = 1;
#endif /* LIBXML_SAX1_ENABLED */

    /*
     * Some users replace the dictionary directly in the context struct.
     * We really need an API function to do that cleanly.
     */
    ctxt->str_xml = xmlDictLookup(ctxt->dict, BAD_CAST "xml", 3);
    ctxt->str_xmlns = xmlDictLookup(ctxt->dict, BAD_CAST "xmlns", 5);
    ctxt->str_xml_ns = xmlDictLookup(ctxt->dict, XML_XML_NAMESPACE, 36);
    if ((ctxt->str_xml==NULL) || (ctxt->str_xmlns==NULL) ||
		(ctxt->str_xml_ns == NULL)) {
        xmlErrMemory(ctxt);
    }

    xmlDictSetLimit(ctxt->dict,
                    (ctxt->options & XML_PARSE_HUGE) ?
                        0 :
                        XML_MAX_DICTIONARY_LIMIT);

#ifdef LIBXML_VALID_ENABLED
    if (ctxt->validate)
        ctxt->vctxt.flags |= XML_VCTXT_VALIDATE;
    else
        ctxt->vctxt.flags &= ~XML_VCTXT_VALIDATE;
#endif /* LIBXML_VALID_ENABLED */
}

typedef struct {
    xmlHashedString prefix;
    xmlHashedString name;
    xmlHashedString value;
    const xmlChar *valueEnd;
    int external;
    int expandedSize;
} xmlDefAttr;

typedef struct _xmlDefAttrs xmlDefAttrs;
typedef xmlDefAttrs *xmlDefAttrsPtr;
struct _xmlDefAttrs {
    int nbAttrs;	/* number of defaulted attributes on that element */
    int maxAttrs;       /* the size of the array */
#if __STDC_VERSION__ >= 199901L
    /* Using a C99 flexible array member avoids UBSan errors. */
    xmlDefAttr attrs[] ATTRIBUTE_COUNTED_BY(maxAttrs);
#else
    xmlDefAttr attrs[1];
#endif
};

/**
 * Normalize the space in non CDATA attribute values:
 * If the attribute type is not CDATA, then the XML processor MUST further
 * process the normalized attribute value by discarding any leading and
 * trailing space (\#x20) characters, and by replacing sequences of space
 * (\#x20) characters by a single space (\#x20) character.
 * Note that the size of dst need to be at least src, and if one doesn't need
 * to preserve dst (and it doesn't come from a dictionary or read-only) then
 * passing src as dst is just fine.
 *
 * @param src  the source string
 * @param dst  the target string
 * @returns a pointer to the normalized value (dst) or NULL if no conversion
 *         is needed.
 */
static xmlChar *
xmlAttrNormalizeSpace(const xmlChar *src, xmlChar *dst)
{
    if ((src == NULL) || (dst == NULL))
        return(NULL);

    while (*src == 0x20) src++;
    while (*src != 0) {
	if (*src == 0x20) {
	    while (*src == 0x20) src++;
	    if (*src != 0)
		*dst++ = 0x20;
	} else {
	    *dst++ = *src++;
	}
    }
    *dst = 0;
    if (dst == src)
       return(NULL);
    return(dst);
}

/**
 * Add a defaulted attribute for an element
 *
 * @param ctxt  an XML parser context
 * @param fullname  the element fullname
 * @param fullattr  the attribute fullname
 * @param value  the attribute value
 */
static void
xmlAddDefAttrs(xmlParserCtxtPtr ctxt,
               const xmlChar *fullname,
               const xmlChar *fullattr,
               const xmlChar *value) {
    xmlDefAttrsPtr defaults;
    xmlDefAttr *attr;
    int len, expandedSize;
    xmlHashedString name;
    xmlHashedString prefix;
    xmlHashedString hvalue;
    const xmlChar *localname;

    /*
     * Allows to detect attribute redefinitions
     */
    if (ctxt->attsSpecial != NULL) {
        if (xmlHashLookup2(ctxt->attsSpecial, fullname, fullattr) != NULL)
	    return;
    }

    if (ctxt->attsDefault == NULL) {
        ctxt->attsDefault = xmlHashCreateDict(10, ctxt->dict);
	if (ctxt->attsDefault == NULL)
	    goto mem_error;
    }

    /*
     * split the element name into prefix:localname , the string found
     * are within the DTD and then not associated to namespace names.
     */
    localname = xmlSplitQName3(fullname, &len);
    if (localname == NULL) {
        name = xmlDictLookupHashed(ctxt->dict, fullname, -1);
	prefix.name = NULL;
    } else {
        name = xmlDictLookupHashed(ctxt->dict, localname, -1);
	prefix = xmlDictLookupHashed(ctxt->dict, fullname, len);
        if (prefix.name == NULL)
            goto mem_error;
    }
    if (name.name == NULL)
        goto mem_error;

    /*
     * make sure there is some storage
     */
    defaults = xmlHashLookup2(ctxt->attsDefault, name.name, prefix.name);
    if ((defaults == NULL) ||
        (defaults->nbAttrs >= defaults->maxAttrs)) {
        xmlDefAttrsPtr temp;
        int newSize;

        if (defaults == NULL) {
            newSize = 4;
        } else {
            if ((defaults->maxAttrs >= XML_MAX_ATTRS) ||
                ((size_t) defaults->maxAttrs >
                     SIZE_MAX / 2 / sizeof(temp[0]) - sizeof(*defaults)))
                goto mem_error;

            if (defaults->maxAttrs > XML_MAX_ATTRS / 2)
                newSize = XML_MAX_ATTRS;
            else
                newSize = defaults->maxAttrs * 2;
        }
        temp = xmlRealloc(defaults,
                          sizeof(*defaults) + newSize * sizeof(xmlDefAttr));
	if (temp == NULL)
	    goto mem_error;
        if (defaults == NULL)
            temp->nbAttrs = 0;
	temp->maxAttrs = newSize;
        defaults = temp;
	if (xmlHashUpdateEntry2(ctxt->attsDefault, name.name, prefix.name,
	                        defaults, NULL) < 0) {
	    xmlFree(defaults);
	    goto mem_error;
	}
    }

    /*
     * Split the attribute name into prefix:localname , the string found
     * are within the DTD and hen not associated to namespace names.
     */
    localname = xmlSplitQName3(fullattr, &len);
    if (localname == NULL) {
        name = xmlDictLookupHashed(ctxt->dict, fullattr, -1);
	prefix.name = NULL;
    } else {
        name = xmlDictLookupHashed(ctxt->dict, localname, -1);
	prefix = xmlDictLookupHashed(ctxt->dict, fullattr, len);
        if (prefix.name == NULL)
            goto mem_error;
    }
    if (name.name == NULL)
        goto mem_error;

    /* intern the string and precompute the end */
    len = strlen((const char *) value);
    hvalue = xmlDictLookupHashed(ctxt->dict, value, len);
    if (hvalue.name == NULL)
        goto mem_error;

    expandedSize = strlen((const char *) name.name);
    if (prefix.name != NULL)
        expandedSize += strlen((const char *) prefix.name);
    expandedSize += len;

    attr = &defaults->attrs[defaults->nbAttrs++];
    attr->name = name;
    attr->prefix = prefix;
    attr->value = hvalue;
    attr->valueEnd = hvalue.name + len;
    attr->external = PARSER_EXTERNAL(ctxt);
    attr->expandedSize = expandedSize;

    return;

mem_error:
    xmlErrMemory(ctxt);
}

/**
 * Register this attribute type
 *
 * @param ctxt  an XML parser context
 * @param fullname  the element fullname
 * @param fullattr  the attribute fullname
 * @param type  the attribute type
 */
static void
xmlAddSpecialAttr(xmlParserCtxtPtr ctxt,
		  const xmlChar *fullname,
		  const xmlChar *fullattr,
		  int type)
{
    if (ctxt->attsSpecial == NULL) {
        ctxt->attsSpecial = xmlHashCreateDict(10, ctxt->dict);
	if (ctxt->attsSpecial == NULL)
	    goto mem_error;
    }

    if (PARSER_EXTERNAL(ctxt))
        type |= XML_SPECIAL_EXTERNAL;

    if (xmlHashAdd2(ctxt->attsSpecial, fullname, fullattr,
                    XML_INT_TO_PTR(type)) < 0)
        goto mem_error;
    return;

mem_error:
    xmlErrMemory(ctxt);
}

/**
 * Removes CDATA attributes from the special attribute table
 */
static void
xmlCleanSpecialAttrCallback(void *payload, void *data,
                            const xmlChar *fullname, const xmlChar *fullattr,
                            const xmlChar *unused ATTRIBUTE_UNUSED) {
    xmlParserCtxtPtr ctxt = (xmlParserCtxtPtr) data;

    if (XML_PTR_TO_INT(payload) == XML_ATTRIBUTE_CDATA) {
        xmlHashRemoveEntry2(ctxt->attsSpecial, fullname, fullattr, NULL);
    }
}

/**
 * Trim the list of attributes defined to remove all those of type
 * CDATA as they are not special. This call should be done when finishing
 * to parse the DTD and before starting to parse the document root.
 *
 * @param ctxt  an XML parser context
 */
static void
xmlCleanSpecialAttr(xmlParserCtxtPtr ctxt)
{
    if (ctxt->attsSpecial == NULL)
        return;

    xmlHashScanFull(ctxt->attsSpecial, xmlCleanSpecialAttrCallback, ctxt);

    if (xmlHashSize(ctxt->attsSpecial) == 0) {
        xmlHashFree(ctxt->attsSpecial, NULL);
        ctxt->attsSpecial = NULL;
    }
}

/**
 * Checks that the value conforms to the LanguageID production:
 *
 * @deprecated Internal function, do not use.
 *
 * NOTE: this is somewhat deprecated, those productions were removed from
 * the XML Second edition.
 *
 *     [33] LanguageID ::= Langcode ('-' Subcode)*
 *     [34] Langcode ::= ISO639Code |  IanaCode |  UserCode
 *     [35] ISO639Code ::= ([a-z] | [A-Z]) ([a-z] | [A-Z])
 *     [36] IanaCode ::= ('i' | 'I') '-' ([a-z] | [A-Z])+
 *     [37] UserCode ::= ('x' | 'X') '-' ([a-z] | [A-Z])+
 *     [38] Subcode ::= ([a-z] | [A-Z])+
 *
 * The current REC reference the successors of RFC 1766, currently 5646
 *
 * http://www.rfc-editor.org/rfc/rfc5646.txt
 *
 *     langtag       = language
 *                     ["-" script]
 *                     ["-" region]
 *                     *("-" variant)
 *                     *("-" extension)
 *                     ["-" privateuse]
 *     language      = 2*3ALPHA            ; shortest ISO 639 code
 *                     ["-" extlang]       ; sometimes followed by
 *                                         ; extended language subtags
 *                   / 4ALPHA              ; or reserved for future use
 *                   / 5*8ALPHA            ; or registered language subtag
 *
 *     extlang       = 3ALPHA              ; selected ISO 639 codes
 *                     *2("-" 3ALPHA)      ; permanently reserved
 *
 *     script        = 4ALPHA              ; ISO 15924 code
 *
 *     region        = 2ALPHA              ; ISO 3166-1 code
 *                   / 3DIGIT              ; UN M.49 code
 *
 *     variant       = 5*8alphanum         ; registered variants
 *                   / (DIGIT 3alphanum)
 *
 *     extension     = singleton 1*("-" (2*8alphanum))
 *
 *                                         ; Single alphanumerics
 *                                         ; "x" reserved for private use
 *     singleton     = DIGIT               ; 0 - 9
 *                   / %x41-57             ; A - W
 *                   / %x59-5A             ; Y - Z
 *                   / %x61-77             ; a - w
 *                   / %x79-7A             ; y - z
 *
 * it sounds right to still allow Irregular i-xxx IANA and user codes too
 * The parser below doesn't try to cope with extension or privateuse
 * that could be added but that's not interoperable anyway
 *
 * @param lang  pointer to the string value
 * @returns 1 if correct 0 otherwise
 **/
int
xmlCheckLanguageID(const xmlChar * lang)
{
    const xmlChar *cur = lang, *nxt;

    if (cur == NULL)
        return (0);
    if (((cur[0] == 'i') && (cur[1] == '-')) ||
        ((cur[0] == 'I') && (cur[1] == '-')) ||
        ((cur[0] == 'x') && (cur[1] == '-')) ||
        ((cur[0] == 'X') && (cur[1] == '-'))) {
        /*
         * Still allow IANA code and user code which were coming
         * from the previous version of the XML-1.0 specification
         * it's deprecated but we should not fail
         */
        cur += 2;
        while (((cur[0] >= 'A') && (cur[0] <= 'Z')) ||
               ((cur[0] >= 'a') && (cur[0] <= 'z')))
            cur++;
        return(cur[0] == 0);
    }
    nxt = cur;
    while (((nxt[0] >= 'A') && (nxt[0] <= 'Z')) ||
           ((nxt[0] >= 'a') && (nxt[0] <= 'z')))
           nxt++;
    if (nxt - cur >= 4) {
        /*
         * Reserved
         */
        if ((nxt - cur > 8) || (nxt[0] != 0))
            return(0);
        return(1);
    }
    if (nxt - cur < 2)
        return(0);
    /* we got an ISO 639 code */
    if (nxt[0] == 0)
        return(1);
    if (nxt[0] != '-')
        return(0);

    nxt++;
    cur = nxt;
    /* now we can have extlang or script or region or variant */
    if ((nxt[0] >= '0') && (nxt[0] <= '9'))
        goto region_m49;

    while (((nxt[0] >= 'A') && (nxt[0] <= 'Z')) ||
           ((nxt[0] >= 'a') && (nxt[0] <= 'z')))
           nxt++;
    if (nxt - cur == 4)
        goto script;
    if (nxt - cur == 2)
        goto region;
    if ((nxt - cur >= 5) && (nxt - cur <= 8))
        goto variant;
    if (nxt - cur != 3)
        return(0);
    /* we parsed an extlang */
    if (nxt[0] == 0)
        return(1);
    if (nxt[0] != '-')
        return(0);

    nxt++;
    cur = nxt;
    /* now we can have script or region or variant */
    if ((nxt[0] >= '0') && (nxt[0] <= '9'))
        goto region_m49;

    while (((nxt[0] >= 'A') && (nxt[0] <= 'Z')) ||
           ((nxt[0] >= 'a') && (nxt[0] <= 'z')))
           nxt++;
    if (nxt - cur == 2)
        goto region;
    if ((nxt - cur >= 5) && (nxt - cur <= 8))
        goto variant;
    if (nxt - cur != 4)
        return(0);
    /* we parsed a script */
script:
    if (nxt[0] == 0)
        return(1);
    if (nxt[0] != '-')
        return(0);

    nxt++;
    cur = nxt;
    /* now we can have region or variant */
    if ((nxt[0] >= '0') && (nxt[0] <= '9'))
        goto region_m49;

    while (((nxt[0] >= 'A') && (nxt[0] <= 'Z')) ||
           ((nxt[0] >= 'a') && (nxt[0] <= 'z')))
           nxt++;

    if ((nxt - cur >= 5) && (nxt - cur <= 8))
        goto variant;
    if (nxt - cur != 2)
        return(0);
    /* we parsed a region */
region:
    if (nxt[0] == 0)
        return(1);
    if (nxt[0] != '-')
        return(0);

    nxt++;
    cur = nxt;
    /* now we can just have a variant */
    while (((nxt[0] >= 'A') && (nxt[0] <= 'Z')) ||
           ((nxt[0] >= 'a') && (nxt[0] <= 'z')))
           nxt++;

    if ((nxt - cur < 5) || (nxt - cur > 8))
        return(0);

    /* we parsed a variant */
variant:
    if (nxt[0] == 0)
        return(1);
    if (nxt[0] != '-')
        return(0);
    /* extensions and private use subtags not checked */
    return (1);

region_m49:
    if (((nxt[1] >= '0') && (nxt[1] <= '9')) &&
        ((nxt[2] >= '0') && (nxt[2] <= '9'))) {
        nxt += 3;
        goto region;
    }
    return(0);
}

/************************************************************************
 *									*
 *		Parser stacks related functions and macros		*
 *									*
 ************************************************************************/

static xmlChar *
xmlParseStringEntityRef(xmlParserCtxtPtr ctxt, const xmlChar **str);

/**
 * Create a new namespace database.
 *
 * @returns the new obejct.
 */
xmlParserNsData *
xmlParserNsCreate(void) {
    xmlParserNsData *nsdb = xmlMalloc(sizeof(*nsdb));

    if (nsdb == NULL)
        return(NULL);
    memset(nsdb, 0, sizeof(*nsdb));
    nsdb->defaultNsIndex = INT_MAX;

    return(nsdb);
}

/**
 * Free a namespace database.
 *
 * @param nsdb  namespace database
 */
void
xmlParserNsFree(xmlParserNsData *nsdb) {
    if (nsdb == NULL)
        return;

    xmlFree(nsdb->extra);
    xmlFree(nsdb->hash);
    xmlFree(nsdb);
}

/**
 * Reset a namespace database.
 *
 * @param nsdb  namespace database
 */
static void
xmlParserNsReset(xmlParserNsData *nsdb) {
    if (nsdb == NULL)
        return;

    nsdb->hashElems = 0;
    nsdb->elementId = 0;
    nsdb->defaultNsIndex = INT_MAX;

    if (nsdb->hash)
        memset(nsdb->hash, 0, nsdb->hashSize * sizeof(nsdb->hash[0]));
}

/**
 * Signal that a new element has started.
 *
 * @param nsdb  namespace database
 * @returns 0 on success, -1 if the element counter overflowed.
 */
static int
xmlParserNsStartElement(xmlParserNsData *nsdb) {
    if (nsdb->elementId == UINT_MAX)
        return(-1);
    nsdb->elementId++;

    return(0);
}

/**
 * Lookup namespace with given prefix. If `bucketPtr` is non-NULL, it will
 * be set to the matching bucket, or the first empty bucket if no match
 * was found.
 *
 * @param ctxt  parser context
 * @param prefix  namespace prefix
 * @param bucketPtr  optional bucket (return value)
 * @returns the namespace index on success, INT_MAX if no namespace was
 * found.
 */
static int
xmlParserNsLookup(xmlParserCtxtPtr ctxt, const xmlHashedString *prefix,
                  xmlParserNsBucket **bucketPtr) {
    xmlParserNsBucket *bucket, *tombstone;
    unsigned index, hashValue;

    if (prefix->name == NULL)
        return(ctxt->nsdb->defaultNsIndex);

    if (ctxt->nsdb->hashSize == 0)
        return(INT_MAX);

    hashValue = prefix->hashValue;
    index = hashValue & (ctxt->nsdb->hashSize - 1);
    bucket = &ctxt->nsdb->hash[index];
    tombstone = NULL;

    while (bucket->hashValue) {
        if (bucket->index == INT_MAX) {
            if (tombstone == NULL)
                tombstone = bucket;
        } else if (bucket->hashValue == hashValue) {
            if (ctxt->nsTab[bucket->index * 2] == prefix->name) {
                if (bucketPtr != NULL)
                    *bucketPtr = bucket;
                return(bucket->index);
            }
        }

        index++;
        bucket++;
        if (index == ctxt->nsdb->hashSize) {
            index = 0;
            bucket = ctxt->nsdb->hash;
        }
    }

    if (bucketPtr != NULL)
        *bucketPtr = tombstone ? tombstone : bucket;
    return(INT_MAX);
}

/**
 * Lookup namespace URI with given prefix.
 *
 * @param ctxt  parser context
 * @param prefix  namespace prefix
 * @returns the namespace URI on success, NULL if no namespace was found.
 */
static const xmlChar *
xmlParserNsLookupUri(xmlParserCtxtPtr ctxt, const xmlHashedString *prefix) {
    const xmlChar *ret;
    int nsIndex;

    if (prefix->name == ctxt->str_xml)
        return(ctxt->str_xml_ns);

    /*
     * minNsIndex is used when building an entity tree. We must
     * ignore namespaces declared outside the entity.
     */
    nsIndex = xmlParserNsLookup(ctxt, prefix, NULL);
    if ((nsIndex == INT_MAX) || (nsIndex < ctxt->nsdb->minNsIndex))
        return(NULL);

    ret = ctxt->nsTab[nsIndex * 2 + 1];
    if (ret[0] == 0)
        ret = NULL;
    return(ret);
}

/**
 * Lookup extra data for the given prefix. This returns data stored
 * with xmlParserNsUdpateSax.
 *
 * @param ctxt  parser context
 * @param prefix  namespace prefix
 * @returns the data on success, NULL if no namespace was found.
 */
void *
xmlParserNsLookupSax(xmlParserCtxt *ctxt, const xmlChar *prefix) {
    xmlHashedString hprefix;
    int nsIndex;

    if (prefix == ctxt->str_xml)
        return(NULL);

    hprefix.name = prefix;
    if (prefix != NULL)
        hprefix.hashValue = xmlDictComputeHash(ctxt->dict, prefix);
    else
        hprefix.hashValue = 0;
    nsIndex = xmlParserNsLookup(ctxt, &hprefix, NULL);
    if ((nsIndex == INT_MAX) || (nsIndex < ctxt->nsdb->minNsIndex))
        return(NULL);

    return(ctxt->nsdb->extra[nsIndex].saxData);
}

/**
 * Sets or updates extra data for the given prefix. This value will be
 * returned by xmlParserNsLookupSax as long as the namespace with the
 * given prefix is in scope.
 *
 * @param ctxt  parser context
 * @param prefix  namespace prefix
 * @param saxData  extra data for SAX handler
 * @returns the data on success, NULL if no namespace was found.
 */
int
xmlParserNsUpdateSax(xmlParserCtxt *ctxt, const xmlChar *prefix,
                     void *saxData) {
    xmlHashedString hprefix;
    int nsIndex;

    if (prefix == ctxt->str_xml)
        return(-1);

    hprefix.name = prefix;
    if (prefix != NULL)
        hprefix.hashValue = xmlDictComputeHash(ctxt->dict, prefix);
    else
        hprefix.hashValue = 0;
    nsIndex = xmlParserNsLookup(ctxt, &hprefix, NULL);
    if ((nsIndex == INT_MAX) || (nsIndex < ctxt->nsdb->minNsIndex))
        return(-1);

    ctxt->nsdb->extra[nsIndex].saxData = saxData;
    return(0);
}

/**
 * Grows the namespace tables.
 *
 * @param ctxt  parser context
 * @returns 0 on success, -1 if a memory allocation failed.
 */
static int
xmlParserNsGrow(xmlParserCtxtPtr ctxt) {
    const xmlChar **table;
    xmlParserNsExtra *extra;
    int newSize;

    newSize = xmlGrowCapacity(ctxt->nsMax,
                              sizeof(table[0]) + sizeof(extra[0]),
                              16, XML_MAX_ITEMS);
    if (newSize < 0)
        goto error;

    table = xmlRealloc(ctxt->nsTab, 2 * newSize * sizeof(table[0]));
    if (table == NULL)
        goto error;
    ctxt->nsTab = table;

    extra = xmlRealloc(ctxt->nsdb->extra, newSize * sizeof(extra[0]));
    if (extra == NULL)
        goto error;
    ctxt->nsdb->extra = extra;

    ctxt->nsMax = newSize;
    return(0);

error:
    xmlErrMemory(ctxt);
    return(-1);
}

/**
 * Push a new namespace on the table.
 *
 * @param ctxt  parser context
 * @param prefix  prefix with hash value
 * @param uri  uri with hash value
 * @param saxData  extra data for SAX handler
 * @param defAttr  whether the namespace comes from a default attribute
 * @returns 1 if the namespace was pushed, 0 if the namespace was ignored,
 * -1 if a memory allocation failed.
 */
static int
xmlParserNsPush(xmlParserCtxtPtr ctxt, const xmlHashedString *prefix,
                const xmlHashedString *uri, void *saxData, int defAttr) {
    xmlParserNsBucket *bucket = NULL;
    xmlParserNsExtra *extra;
    const xmlChar **ns;
    unsigned hashValue, nsIndex, oldIndex;

    if ((prefix != NULL) && (prefix->name == ctxt->str_xml))
        return(0);

    if ((ctxt->nsNr >= ctxt->nsMax) && (xmlParserNsGrow(ctxt) < 0)) {
        xmlErrMemory(ctxt);
        return(-1);
    }

    /*
     * Default namespace and 'xml' namespace
     */
    if ((prefix == NULL) || (prefix->name == NULL)) {
        oldIndex = ctxt->nsdb->defaultNsIndex;

        if (oldIndex != INT_MAX) {
            extra = &ctxt->nsdb->extra[oldIndex];

            if (extra->elementId == ctxt->nsdb->elementId) {
                if (defAttr == 0)
                    xmlErrAttributeDup(ctxt, NULL, BAD_CAST "xmlns");
                return(0);
            }

            if ((ctxt->options & XML_PARSE_NSCLEAN) &&
                (uri->name == ctxt->nsTab[oldIndex * 2 + 1]))
                return(0);
        }

        ctxt->nsdb->defaultNsIndex = ctxt->nsNr;
        goto populate_entry;
    }

    /*
     * Hash table lookup
     */
    oldIndex = xmlParserNsLookup(ctxt, prefix, &bucket);
    if (oldIndex != INT_MAX) {
        extra = &ctxt->nsdb->extra[oldIndex];

        /*
         * Check for duplicate definitions on the same element.
         */
        if (extra->elementId == ctxt->nsdb->elementId) {
            if (defAttr == 0)
                xmlErrAttributeDup(ctxt, BAD_CAST "xmlns", prefix->name);
            return(0);
        }

        if ((ctxt->options & XML_PARSE_NSCLEAN) &&
            (uri->name == ctxt->nsTab[bucket->index * 2 + 1]))
            return(0);

        bucket->index = ctxt->nsNr;
        goto populate_entry;
    }

    /*
     * Insert new bucket
     */

    hashValue = prefix->hashValue;

    /*
     * Grow hash table, 50% fill factor
     */
    if (ctxt->nsdb->hashElems + 1 > ctxt->nsdb->hashSize / 2) {
        xmlParserNsBucket *newHash;
        unsigned newSize, i, index;

        if (ctxt->nsdb->hashSize > UINT_MAX / 2) {
            xmlErrMemory(ctxt);
            return(-1);
        }
        newSize = ctxt->nsdb->hashSize ? ctxt->nsdb->hashSize * 2 : 16;
        newHash = xmlMalloc(newSize * sizeof(newHash[0]));
        if (newHash == NULL) {
            xmlErrMemory(ctxt);
            return(-1);
        }
        memset(newHash, 0, newSize * sizeof(newHash[0]));

        for (i = 0; i < ctxt->nsdb->hashSize; i++) {
            unsigned hv = ctxt->nsdb->hash[i].hashValue;
            unsigned newIndex;

            if ((hv == 0) || (ctxt->nsdb->hash[i].index == INT_MAX))
                continue;
            newIndex = hv & (newSize - 1);

            while (newHash[newIndex].hashValue != 0) {
                newIndex++;
                if (newIndex == newSize)
                    newIndex = 0;
            }

            newHash[newIndex] = ctxt->nsdb->hash[i];
        }

        xmlFree(ctxt->nsdb->hash);
        ctxt->nsdb->hash = newHash;
        ctxt->nsdb->hashSize = newSize;

        /*
         * Relookup
         */
        index = hashValue & (newSize - 1);

        while (newHash[index].hashValue != 0) {
            index++;
            if (index == newSize)
                index = 0;
        }

        bucket = &newHash[index];
    }

    bucket->hashValue = hashValue;
    bucket->index = ctxt->nsNr;
    ctxt->nsdb->hashElems++;
    oldIndex = INT_MAX;

populate_entry:
    nsIndex = ctxt->nsNr;

    ns = &ctxt->nsTab[nsIndex * 2];
    ns[0] = prefix ? prefix->name : NULL;
    ns[1] = uri->name;

    extra = &ctxt->nsdb->extra[nsIndex];
    extra->saxData = saxData;
    extra->prefixHashValue = prefix ? prefix->hashValue : 0;
    extra->uriHashValue = uri->hashValue;
    extra->elementId = ctxt->nsdb->elementId;
    extra->oldIndex = oldIndex;

    ctxt->nsNr++;

    return(1);
}

/**
 * Pops the top `nr` namespaces and restores the hash table.
 *
 * @param ctxt  an XML parser context
 * @param nr  the number to pop
 * @returns the number of namespaces popped.
 */
static int
xmlParserNsPop(xmlParserCtxtPtr ctxt, int nr)
{
    int i;

    /* assert(nr <= ctxt->nsNr); */

    for (i = ctxt->nsNr - 1; i >= ctxt->nsNr - nr; i--) {
        const xmlChar *prefix = ctxt->nsTab[i * 2];
        xmlParserNsExtra *extra = &ctxt->nsdb->extra[i];

        if (prefix == NULL) {
            ctxt->nsdb->defaultNsIndex = extra->oldIndex;
        } else {
            xmlHashedString hprefix;
            xmlParserNsBucket *bucket = NULL;

            hprefix.name = prefix;
            hprefix.hashValue = extra->prefixHashValue;
            xmlParserNsLookup(ctxt, &hprefix, &bucket);
            /* assert(bucket && bucket->hashValue); */
            bucket->index = extra->oldIndex;
        }
    }

    ctxt->nsNr -= nr;
    return(nr);
}

static int
xmlCtxtGrowAttrs(xmlParserCtxtPtr ctxt) {
    const xmlChar **atts;
    unsigned *attallocs;
    int newSize;

    newSize = xmlGrowCapacity(ctxt->maxatts / 5,
                              sizeof(atts[0]) * 5 + sizeof(attallocs[0]),
                              10, XML_MAX_ATTRS);
    if (newSize < 0) {
        xmlFatalErr(ctxt, XML_ERR_RESOURCE_LIMIT,
                    "Maximum number of attributes exceeded");
        return(-1);
    }

    atts = xmlRealloc(ctxt->atts, newSize * sizeof(atts[0]) * 5);
    if (atts == NULL)
        goto mem_error;
    ctxt->atts = atts;

    attallocs = xmlRealloc(ctxt->attallocs,
                           newSize * sizeof(attallocs[0]));
    if (attallocs == NULL)
        goto mem_error;
    ctxt->attallocs = attallocs;

    ctxt->maxatts = newSize * 5;

    return(0);

mem_error:
    xmlErrMemory(ctxt);
    return(-1);
}

/**
 * Pushes a new parser input on top of the input stack
 *
 * @param ctxt  an XML parser context
 * @param value  the parser input
 * @returns -1 in case of error, the index in the stack otherwise
 */
int
xmlCtxtPushInput(xmlParserCtxt *ctxt, xmlParserInput *value)
{
    char *directory = NULL;
    int maxDepth;

    if ((ctxt == NULL) || (value == NULL))
        return(-1);

    maxDepth = (ctxt->options & XML_PARSE_HUGE) ? 40 : 20;

    if (ctxt->inputNr >= ctxt->inputMax) {
        xmlParserInputPtr *tmp;
        int newSize;

        newSize = xmlGrowCapacity(ctxt->inputMax, sizeof(tmp[0]),
                                  5, maxDepth);
        if (newSize < 0) {
            xmlFatalErrMsg(ctxt, XML_ERR_RESOURCE_LIMIT,
                           "Maximum entity nesting depth exceeded");
            return(-1);
        }
        tmp = xmlRealloc(ctxt->inputTab, newSize * sizeof(tmp[0]));
        if (tmp == NULL) {
            xmlErrMemory(ctxt);
            return(-1);
        }
        ctxt->inputTab = tmp;
        ctxt->inputMax = newSize;
    }

    if ((ctxt->inputNr == 0) && (value->filename != NULL)) {
        directory = xmlParserGetDirectory(value->filename);
        if (directory == NULL) {
            xmlErrMemory(ctxt);
            return(-1);
        }
    }

    if (ctxt->input_id >= INT_MAX) {
        xmlFatalErrMsg(ctxt, XML_ERR_RESOURCE_LIMIT, "Input ID overflow\n");
        return(-1);
    }

    ctxt->inputTab[ctxt->inputNr] = value;
    ctxt->input = value;

    if (ctxt->inputNr == 0) {
        xmlFree(ctxt->directory);
        ctxt->directory = directory;
    }

    /*
     * The input ID is unused internally, but there are entity
     * loaders in downstream code that detect the main document
     * by checking for "input_id == 1".
     */
    value->id = ctxt->input_id++;

    return(ctxt->inputNr++);
}

/**
 * Pops the top parser input from the input stack
 *
 * @param ctxt  an XML parser context
 * @returns the input just removed
 */
xmlParserInput *
xmlCtxtPopInput(xmlParserCtxt *ctxt)
{
    xmlParserInputPtr ret;

    if (ctxt == NULL)
        return(NULL);
    if (ctxt->inputNr <= 0)
        return (NULL);
    ctxt->inputNr--;
    if (ctxt->inputNr > 0)
        ctxt->input = ctxt->inputTab[ctxt->inputNr - 1];
    else
        ctxt->input = NULL;
    ret = ctxt->inputTab[ctxt->inputNr];
    ctxt->inputTab[ctxt->inputNr] = NULL;
    return (ret);
}

/**
 * Pushes a new element node on top of the node stack
 *
 * @deprecated Internal function, do not use.
 *
 * @param ctxt  an XML parser context
 * @param value  the element node
 * @returns -1 in case of error, the index in the stack otherwise
 */
int
nodePush(xmlParserCtxt *ctxt, xmlNode *value)
{
    if (ctxt == NULL)
        return(0);

    if (ctxt->nodeNr >= ctxt->nodeMax) {
        int maxDepth = (ctxt->options & XML_PARSE_HUGE) ? 2048 : 256;
        xmlNodePtr *tmp;
        int newSize;

        newSize = xmlGrowCapacity(ctxt->nodeMax, sizeof(tmp[0]),
                                  10, maxDepth);
        if (newSize < 0) {
            xmlFatalErrMsgInt(ctxt, XML_ERR_RESOURCE_LIMIT,
                    "Excessive depth in document: %d,"
                    " use XML_PARSE_HUGE option\n",
                    ctxt->nodeNr);
            return(-1);
        }

	tmp = xmlRealloc(ctxt->nodeTab, newSize * sizeof(tmp[0]));
        if (tmp == NULL) {
            xmlErrMemory(ctxt);
            return (-1);
        }
        ctxt->nodeTab = tmp;
	ctxt->nodeMax = newSize;
    }

    ctxt->nodeTab[ctxt->nodeNr] = value;
    ctxt->node = value;
    return (ctxt->nodeNr++);
}

/**
 * Pops the top element node from the node stack
 *
 * @deprecated Internal function, do not use.
 *
 * @param ctxt  an XML parser context
 * @returns the node just removed
 */
xmlNode *
nodePop(xmlParserCtxt *ctxt)
{
    xmlNodePtr ret;

    if (ctxt == NULL) return(NULL);
    if (ctxt->nodeNr <= 0)
        return (NULL);
    ctxt->nodeNr--;
    if (ctxt->nodeNr > 0)
        ctxt->node = ctxt->nodeTab[ctxt->nodeNr - 1];
    else
        ctxt->node = NULL;
    ret = ctxt->nodeTab[ctxt->nodeNr];
    ctxt->nodeTab[ctxt->nodeNr] = NULL;
    return (ret);
}

/**
 * Pushes a new element name/prefix/URL on top of the name stack
 *
 * @param ctxt  an XML parser context
 * @param value  the element name
 * @param prefix  the element prefix
 * @param URI  the element namespace name
 * @param line  the current line number for error messages
 * @param nsNr  the number of namespaces pushed on the namespace table
 * @returns -1 in case of error, the index in the stack otherwise
 */
static int
nameNsPush(xmlParserCtxtPtr ctxt, const xmlChar * value,
           const xmlChar *prefix, const xmlChar *URI, int line, int nsNr)
{
    xmlStartTag *tag;

    if (ctxt->nameNr >= ctxt->nameMax) {
        const xmlChar **tmp;
        xmlStartTag *tmp2;
        int newSize;

        newSize = xmlGrowCapacity(ctxt->nameMax,
                                  sizeof(tmp[0]) + sizeof(tmp2[0]),
                                  10, XML_MAX_ITEMS);
        if (newSize < 0)
            goto mem_error;

        tmp = xmlRealloc(ctxt->nameTab, newSize * sizeof(tmp[0]));
        if (tmp == NULL)
	    goto mem_error;
	ctxt->nameTab = tmp;

        tmp2 = xmlRealloc(ctxt->pushTab, newSize * sizeof(tmp2[0]));
        if (tmp2 == NULL)
	    goto mem_error;
	ctxt->pushTab = tmp2;

        ctxt->nameMax = newSize;
    } else if (ctxt->pushTab == NULL) {
        ctxt->pushTab = xmlMalloc(ctxt->nameMax * sizeof(ctxt->pushTab[0]));
        if (ctxt->pushTab == NULL)
            goto mem_error;
    }
    ctxt->nameTab[ctxt->nameNr] = value;
    ctxt->name = value;
    tag = &ctxt->pushTab[ctxt->nameNr];
    tag->prefix = prefix;
    tag->URI = URI;
    tag->line = line;
    tag->nsNr = nsNr;
    return (ctxt->nameNr++);
mem_error:
    xmlErrMemory(ctxt);
    return (-1);
}
#ifdef LIBXML_PUSH_ENABLED
/**
 * Pops the top element/prefix/URI name from the name stack
 *
 * @param ctxt  an XML parser context
 * @returns the name just removed
 */
static const xmlChar *
nameNsPop(xmlParserCtxtPtr ctxt)
{
    const xmlChar *ret;

    if (ctxt->nameNr <= 0)
        return (NULL);
    ctxt->nameNr--;
    if (ctxt->nameNr > 0)
        ctxt->name = ctxt->nameTab[ctxt->nameNr - 1];
    else
        ctxt->name = NULL;
    ret = ctxt->nameTab[ctxt->nameNr];
    ctxt->nameTab[ctxt->nameNr] = NULL;
    return (ret);
}
#endif /* LIBXML_PUSH_ENABLED */

/**
 * Pops the top element name from the name stack
 *
 * @deprecated Internal function, do not use.
 *
 * @param ctxt  an XML parser context
 * @returns the name just removed
 */
static const xmlChar *
namePop(xmlParserCtxtPtr ctxt)
{
    const xmlChar *ret;

    if ((ctxt == NULL) || (ctxt->nameNr <= 0))
        return (NULL);
    ctxt->nameNr--;
    if (ctxt->nameNr > 0)
        ctxt->name = ctxt->nameTab[ctxt->nameNr - 1];
    else
        ctxt->name = NULL;
    ret = ctxt->nameTab[ctxt->nameNr];
    ctxt->nameTab[ctxt->nameNr] = NULL;
    return (ret);
}

static int spacePush(xmlParserCtxtPtr ctxt, int val) {
    if (ctxt->spaceNr >= ctxt->spaceMax) {
        int *tmp;
        int newSize;

        newSize = xmlGrowCapacity(ctxt->spaceMax, sizeof(tmp[0]),
                                  10, XML_MAX_ITEMS);
        if (newSize < 0) {
	    xmlErrMemory(ctxt);
	    return(-1);
        }

        tmp = xmlRealloc(ctxt->spaceTab, newSize * sizeof(tmp[0]));
        if (tmp == NULL) {
	    xmlErrMemory(ctxt);
	    return(-1);
	}
	ctxt->spaceTab = tmp;

        ctxt->spaceMax = newSize;
    }
    ctxt->spaceTab[ctxt->spaceNr] = val;
    ctxt->space = &ctxt->spaceTab[ctxt->spaceNr];
    return(ctxt->spaceNr++);
}

static int spacePop(xmlParserCtxtPtr ctxt) {
    int ret;
    if (ctxt->spaceNr <= 0) return(0);
    ctxt->spaceNr--;
    if (ctxt->spaceNr > 0)
	ctxt->space = &ctxt->spaceTab[ctxt->spaceNr - 1];
    else
        ctxt->space = &ctxt->spaceTab[0];
    ret = ctxt->spaceTab[ctxt->spaceNr];
    ctxt->spaceTab[ctxt->spaceNr] = -1;
    return(ret);
}

/*
 * Macros for accessing the content. Those should be used only by the parser,
 * and not exported.
 *
 * Dirty macros, i.e. one often need to make assumption on the context to
 * use them
 *
 *   CUR_PTR return the current pointer to the xmlChar to be parsed.
 *           To be used with extreme caution since operations consuming
 *           characters may move the input buffer to a different location !
 *   CUR     returns the current xmlChar value, i.e. a 8 bit value if compiled
 *           This should be used internally by the parser
 *           only to compare to ASCII values otherwise it would break when
 *           running with UTF-8 encoding.
 *   RAW     same as CUR but in the input buffer, bypass any token
 *           extraction that may have been done
 *   NXT(n)  returns the n'th next xmlChar. Same as CUR is should be used only
 *           to compare on ASCII based substring.
 *   SKIP(n) Skip n xmlChar, and must also be used only to skip ASCII defined
 *           strings without newlines within the parser.
 *   NEXT1(l) Skip 1 xmlChar, and must also be used only to skip 1 non-newline ASCII
 *           defined char within the parser.
 * Clean macros, not dependent of an ASCII context, expect UTF-8 encoding
 *
 *   NEXT    Skip to the next character, this does the proper decoding
 *           in UTF-8 mode. It also pop-up unfinished entities on the fly.
 *   NEXTL(l) Skip the current unicode character of l xmlChars long.
 *   COPY_BUF  copy the current unicode char to the target buffer, increment
 *            the index
 *   GROW, SHRINK  handling of input buffers
 */

#define RAW (*ctxt->input->cur)
#define CUR (*ctxt->input->cur)
#define NXT(val) ctxt->input->cur[(val)]
#define CUR_PTR ctxt->input->cur
#define BASE_PTR ctxt->input->base

#define CMP4( s, c1, c2, c3, c4 ) \
  ( ((unsigned char *) s)[ 0 ] == c1 && ((unsigned char *) s)[ 1 ] == c2 && \
    ((unsigned char *) s)[ 2 ] == c3 && ((unsigned char *) s)[ 3 ] == c4 )
#define CMP5( s, c1, c2, c3, c4, c5 ) \
  ( CMP4( s, c1, c2, c3, c4 ) && ((unsigned char *) s)[ 4 ] == c5 )
#define CMP6( s, c1, c2, c3, c4, c5, c6 ) \
  ( CMP5( s, c1, c2, c3, c4, c5 ) && ((unsigned char *) s)[ 5 ] == c6 )
#define CMP7( s, c1, c2, c3, c4, c5, c6, c7 ) \
  ( CMP6( s, c1, c2, c3, c4, c5, c6 ) && ((unsigned char *) s)[ 6 ] == c7 )
#define CMP8( s, c1, c2, c3, c4, c5, c6, c7, c8 ) \
  ( CMP7( s, c1, c2, c3, c4, c5, c6, c7 ) && ((unsigned char *) s)[ 7 ] == c8 )
#define CMP9( s, c1, c2, c3, c4, c5, c6, c7, c8, c9 ) \
  ( CMP8( s, c1, c2, c3, c4, c5, c6, c7, c8 ) && \
    ((unsigned char *) s)[ 8 ] == c9 )
#define CMP10( s, c1, c2, c3, c4, c5, c6, c7, c8, c9, c10 ) \
  ( CMP9( s, c1, c2, c3, c4, c5, c6, c7, c8, c9 ) && \
    ((unsigned char *) s)[ 9 ] == c10 )

#define SKIP(val) do {							\
    ctxt->input->cur += (val),ctxt->input->col+=(val);			\
    if (*ctxt->input->cur == 0)						\
        xmlParserGrow(ctxt);						\
  } while (0)

#define SKIPL(val) do {							\
    int skipl;								\
    for(skipl=0; skipl<val; skipl++) {					\
	if (*(ctxt->input->cur) == '\n') {				\
	ctxt->input->line++; ctxt->input->col = 1;			\
	} else ctxt->input->col++;					\
	ctxt->input->cur++;						\
    }									\
    if (*ctxt->input->cur == 0)						\
        xmlParserGrow(ctxt);						\
  } while (0)

#define SHRINK \
    if (!PARSER_PROGRESSIVE(ctxt)) \
	xmlParserShrink(ctxt);

#define GROW \
    if ((!PARSER_PROGRESSIVE(ctxt)) && \
        (ctxt->input->end - ctxt->input->cur < INPUT_CHUNK)) \
	xmlParserGrow(ctxt);

#define SKIP_BLANKS xmlSkipBlankChars(ctxt)

#define SKIP_BLANKS_PE xmlSkipBlankCharsPE(ctxt)

#define NEXT xmlNextChar(ctxt)

#define NEXT1 {								\
	ctxt->input->col++;						\
	ctxt->input->cur++;						\
	if (*ctxt->input->cur == 0)					\
	    xmlParserGrow(ctxt);						\
    }

#define NEXTL(l) do {							\
    if (*(ctxt->input->cur) == '\n') {					\
	ctxt->input->line++; ctxt->input->col = 1;			\
    } else ctxt->input->col++;						\
    ctxt->input->cur += l;				\
  } while (0)

#define COPY_BUF(b, i, v)						\
    if (v < 0x80) b[i++] = v;						\
    else i += xmlCopyCharMultiByte(&b[i],v)

static int
xmlCurrentCharRecover(xmlParserCtxtPtr ctxt, int *len) {
    int c = xmlCurrentChar(ctxt, len);

    if (c == XML_INVALID_CHAR)
        c = 0xFFFD; /* replacement character */

    return(c);
}

/**
 * Skip whitespace in the input stream.
 *
 * @deprecated Internal function, do not use.
 *
 * @param ctxt  the XML parser context
 * @returns the number of space chars skipped
 */
int
xmlSkipBlankChars(xmlParserCtxt *ctxt) {
    const xmlChar *cur;
    int res = 0;

    cur = ctxt->input->cur;
    while (IS_BLANK_CH(*cur)) {
        if (*cur == '\n') {
            ctxt->input->line++; ctxt->input->col = 1;
        } else {
            ctxt->input->col++;
        }
        cur++;
        if (res < INT_MAX)
            res++;
        if (*cur == 0) {
            ctxt->input->cur = cur;
            xmlParserGrow(ctxt);
            cur = ctxt->input->cur;
        }
    }
    ctxt->input->cur = cur;

    if (res > 4)
        GROW;

    return(res);
}

static void
xmlPopPE(xmlParserCtxtPtr ctxt) {
    unsigned long consumed;
    xmlEntityPtr ent;

    ent = ctxt->input->entity;

    ent->flags &= ~XML_ENT_EXPANDING;

    if ((ent->flags & XML_ENT_CHECKED) == 0) {
        int result;

        /*
         * Read the rest of the stream in case of errors. We want
         * to account for the whole entity size.
         */
        do {
            ctxt->input->cur = ctxt->input->end;
            xmlParserShrink(ctxt);
            result = xmlParserGrow(ctxt);
        } while (result > 0);

        consumed = ctxt->input->consumed;
        xmlSaturatedAddSizeT(&consumed,
                             ctxt->input->end - ctxt->input->base);

        xmlSaturatedAdd(&ent->expandedSize, consumed);

        /*
         * Add to sizeentities when parsing an external entity
         * for the first time.
         */
        if (ent->etype == XML_EXTERNAL_PARAMETER_ENTITY) {
            xmlSaturatedAdd(&ctxt->sizeentities, consumed);
        }

        ent->flags |= XML_ENT_CHECKED;
    }

    xmlFreeInputStream(xmlCtxtPopInput(ctxt));

    xmlParserEntityCheck(ctxt, ent->expandedSize);

    GROW;
}

/**
 * Skip whitespace in the input stream, also handling parameter
 * entities.
 *
 * @param ctxt  the XML parser context
 * @returns the number of space chars skipped
 */
static int
xmlSkipBlankCharsPE(xmlParserCtxtPtr ctxt) {
    int res = 0;
    int inParam;
    int expandParam;

    inParam = PARSER_IN_PE(ctxt);
    expandParam = PARSER_EXTERNAL(ctxt);

    if (!inParam && !expandParam)
        return(xmlSkipBlankChars(ctxt));

    /*
     * It's Okay to use CUR/NEXT here since all the blanks are on
     * the ASCII range.
     */
    while (PARSER_STOPPED(ctxt) == 0) {
        if (IS_BLANK_CH(CUR)) { /* CHECKED tstblanks.xml */
            NEXT;
        } else if (CUR == '%') {
            if ((expandParam == 0) ||
                (IS_BLANK_CH(NXT(1))) || (NXT(1) == 0))
                break;

            /*
             * Expand parameter entity. We continue to consume
             * whitespace at the start of the entity and possible
             * even consume the whole entity and pop it. We might
             * even pop multiple PEs in this loop.
             */
            xmlParsePERefInternal(ctxt, 0);

            inParam = PARSER_IN_PE(ctxt);
            expandParam = PARSER_EXTERNAL(ctxt);
        } else if (CUR == 0) {
            if (inParam == 0)
                break;

            /*
             * Don't pop parameter entities that start a markup
             * declaration to detect Well-formedness constraint:
             * PE Between Declarations.
             */
            if (ctxt->input->flags & XML_INPUT_MARKUP_DECL)
                break;

            xmlPopPE(ctxt);

            inParam = PARSER_IN_PE(ctxt);
            expandParam = PARSER_EXTERNAL(ctxt);
        } else {
            break;
        }

        /*
         * Also increase the counter when entering or exiting a PERef.
         * The spec says: "When a parameter-entity reference is recognized
         * in the DTD and included, its replacement text MUST be enlarged
         * by the attachment of one leading and one following space (#x20)
         * character."
         */
        if (res < INT_MAX)
            res++;
    }

    return(res);
}

/************************************************************************
 *									*
 *		Commodity functions to handle entities			*
 *									*
 ************************************************************************/

/**
 * @deprecated Internal function, don't use.
 *
 * @param ctxt  an XML parser context
 * @returns the current xmlChar in the parser context
 */
xmlChar
xmlPopInput(xmlParserCtxt *ctxt) {
    xmlParserInputPtr input;

    if ((ctxt == NULL) || (ctxt->inputNr <= 1)) return(0);
    input = xmlCtxtPopInput(ctxt);
    xmlFreeInputStream(input);
    if (*ctxt->input->cur == 0)
        xmlParserGrow(ctxt);
    return(CUR);
}

/**
 * Push an input stream onto the stack.
 *
 * @deprecated Internal function, don't use.
 *
 * @param ctxt  an XML parser context
 * @param input  an XML parser input fragment (entity, XML fragment ...).
 * @returns -1 in case of error or the index in the input stack
 */
int
xmlPushInput(xmlParserCtxt *ctxt, xmlParserInput *input) {
    int ret;

    if ((ctxt == NULL) || (input == NULL))
        return(-1);

    ret = xmlCtxtPushInput(ctxt, input);
    if (ret >= 0)
        GROW;
    return(ret);
}

/**
 * Parse a numeric character reference. Always consumes '&'.
 *
 * @deprecated Internal function, don't use.
 *
 *     [66] CharRef ::= '&#' [0-9]+ ';' |
 *                      '&#x' [0-9a-fA-F]+ ';'
 *
 * [ WFC: Legal Character ]
 * Characters referred to using character references must match the
 * production for Char.
 *
 * @param ctxt  an XML parser context
 * @returns the value parsed (as an int), 0 in case of error
 */
int
xmlParseCharRef(xmlParserCtxt *ctxt) {
    int val = 0;
    int count = 0;

    /*
     * Using RAW/CUR/NEXT is okay since we are working on ASCII range here
     */
    if ((RAW == '&') && (NXT(1) == '#') &&
        (NXT(2) == 'x')) {
	SKIP(3);
	GROW;
	while ((RAW != ';') && (PARSER_STOPPED(ctxt) == 0)) {
	    if (count++ > 20) {
		count = 0;
		GROW;
	    }
	    if ((RAW >= '0') && (RAW <= '9'))
	        val = val * 16 + (CUR - '0');
	    else if ((RAW >= 'a') && (RAW <= 'f') && (count < 20))
	        val = val * 16 + (CUR - 'a') + 10;
	    else if ((RAW >= 'A') && (RAW <= 'F') && (count < 20))
	        val = val * 16 + (CUR - 'A') + 10;
	    else {
		xmlFatalErr(ctxt, XML_ERR_INVALID_HEX_CHARREF, NULL);
		val = 0;
		break;
	    }
	    if (val > 0x110000)
	        val = 0x110000;

	    NEXT;
	    count++;
	}
	if (RAW == ';') {
	    /* on purpose to avoid reentrancy problems with NEXT and SKIP */
	    ctxt->input->col++;
	    ctxt->input->cur++;
	}
    } else if  ((RAW == '&') && (NXT(1) == '#')) {
	SKIP(2);
	GROW;
	while (RAW != ';') { /* loop blocked by count */
	    if (count++ > 20) {
		count = 0;
		GROW;
	    }
	    if ((RAW >= '0') && (RAW <= '9'))
	        val = val * 10 + (CUR - '0');
	    else {
		xmlFatalErr(ctxt, XML_ERR_INVALID_DEC_CHARREF, NULL);
		val = 0;
		break;
	    }
	    if (val > 0x110000)
	        val = 0x110000;

	    NEXT;
	    count++;
	}
	if (RAW == ';') {
	    /* on purpose to avoid reentrancy problems with NEXT and SKIP */
	    ctxt->input->col++;
	    ctxt->input->cur++;
	}
    } else {
        if (RAW == '&')
            SKIP(1);
        xmlFatalErr(ctxt, XML_ERR_INVALID_CHARREF, NULL);
    }

    /*
     * [ WFC: Legal Character ]
     * Characters referred to using character references must match the
     * production for Char.
     */
    if (val >= 0x110000) {
        xmlFatalErrMsgInt(ctxt, XML_ERR_INVALID_CHAR,
                "xmlParseCharRef: character reference out of bounds\n",
	        val);
        val = 0xFFFD;
    } else if (!IS_CHAR(val)) {
        xmlFatalErrMsgInt(ctxt, XML_ERR_INVALID_CHAR,
                          "xmlParseCharRef: invalid xmlChar value %d\n",
	                  val);
    }
    return(val);
}

/**
 * Parse Reference declarations, variant parsing from a string rather
 * than an an input flow.
 *
 *     [66] CharRef ::= '&#' [0-9]+ ';' |
 *                      '&#x' [0-9a-fA-F]+ ';'
 *
 * [ WFC: Legal Character ]
 * Characters referred to using character references must match the
 * production for Char.
 *
 * @param ctxt  an XML parser context
 * @param str  a pointer to an index in the string
 * @returns the value parsed (as an int), 0 in case of error, str will be
 *         updated to the current value of the index
 */
static int
xmlParseStringCharRef(xmlParserCtxtPtr ctxt, const xmlChar **str) {
    const xmlChar *ptr;
    xmlChar cur;
    int val = 0;

    if ((str == NULL) || (*str == NULL)) return(0);
    ptr = *str;
    cur = *ptr;
    if ((cur == '&') && (ptr[1] == '#') && (ptr[2] == 'x')) {
	ptr += 3;
	cur = *ptr;
	while (cur != ';') { /* Non input consuming loop */
	    if ((cur >= '0') && (cur <= '9'))
	        val = val * 16 + (cur - '0');
	    else if ((cur >= 'a') && (cur <= 'f'))
	        val = val * 16 + (cur - 'a') + 10;
	    else if ((cur >= 'A') && (cur <= 'F'))
	        val = val * 16 + (cur - 'A') + 10;
	    else {
		xmlFatalErr(ctxt, XML_ERR_INVALID_HEX_CHARREF, NULL);
		val = 0;
		break;
	    }
	    if (val > 0x110000)
	        val = 0x110000;

	    ptr++;
	    cur = *ptr;
	}
	if (cur == ';')
	    ptr++;
    } else if  ((cur == '&') && (ptr[1] == '#')){
	ptr += 2;
	cur = *ptr;
	while (cur != ';') { /* Non input consuming loops */
	    if ((cur >= '0') && (cur <= '9'))
	        val = val * 10 + (cur - '0');
	    else {
		xmlFatalErr(ctxt, XML_ERR_INVALID_DEC_CHARREF, NULL);
		val = 0;
		break;
	    }
	    if (val > 0x110000)
	        val = 0x110000;

	    ptr++;
	    cur = *ptr;
	}
	if (cur == ';')
	    ptr++;
    } else {
	xmlFatalErr(ctxt, XML_ERR_INVALID_CHARREF, NULL);
	return(0);
    }
    *str = ptr;

    /*
     * [ WFC: Legal Character ]
     * Characters referred to using character references must match the
     * production for Char.
     */
    if (val >= 0x110000) {
        xmlFatalErrMsgInt(ctxt, XML_ERR_INVALID_CHAR,
                "xmlParseStringCharRef: character reference out of bounds\n",
                val);
    } else if (IS_CHAR(val)) {
        return(val);
    } else {
        xmlFatalErrMsgInt(ctxt, XML_ERR_INVALID_CHAR,
			  "xmlParseStringCharRef: invalid xmlChar value %d\n",
			  val);
    }
    return(0);
}

/**
 *     [69] PEReference ::= '%' Name ';'
 *
 * @deprecated Internal function, do not use.
 *
 * [ WFC: No Recursion ]
 * A parsed entity must not contain a recursive
 * reference to itself, either directly or indirectly.
 *
 * [ WFC: Entity Declared ]
 * In a document without any DTD, a document with only an internal DTD
 * subset which contains no parameter entity references, or a document
 * with "standalone='yes'", ...  ... The declaration of a parameter
 * entity must precede any reference to it...
 *
 * [ VC: Entity Declared ]
 * In a document with an external subset or external parameter entities
 * with "standalone='no'", ...  ... The declaration of a parameter entity
 * must precede any reference to it...
 *
 * [ WFC: In DTD ]
 * Parameter-entity references may only appear in the DTD.
 * NOTE: misleading but this is handled.
 *
 * A PEReference may have been detected in the current input stream
 * the handling is done accordingly to
 *      http://www.w3.org/TR/REC-xml#entproc
 * i.e.
 *   - Included in literal in entity values
 *   - Included as Parameter Entity reference within DTDs
 * @param ctxt  the parser context
 */
void
xmlParserHandlePEReference(xmlParserCtxt *ctxt) {
    xmlParsePERefInternal(ctxt, 0);
}

/**
 * @deprecated Internal function, don't use.
 *
 * @param ctxt  the parser context
 * @param str  the input string
 * @param len  the string length
 * @param what  combination of XML_SUBSTITUTE_REF and XML_SUBSTITUTE_PEREF
 * @param end  an end marker xmlChar, 0 if none
 * @param end2  an end marker xmlChar, 0 if none
 * @param end3  an end marker xmlChar, 0 if none
 * @returns A newly allocated string with the substitution done. The caller
 *      must deallocate it !
 */
xmlChar *
xmlStringLenDecodeEntities(xmlParserCtxt *ctxt, const xmlChar *str, int len,
                           int what ATTRIBUTE_UNUSED,
                           xmlChar end, xmlChar end2, xmlChar end3) {
    if ((ctxt == NULL) || (str == NULL) || (len < 0))
        return(NULL);

    if ((str[len] != 0) ||
        (end != 0) || (end2 != 0) || (end3 != 0))
        return(NULL);

    return(xmlExpandEntitiesInAttValue(ctxt, str, 0));
}

/**
 * @deprecated Internal function, don't use.
 *
 * @param ctxt  the parser context
 * @param str  the input string
 * @param what  combination of XML_SUBSTITUTE_REF and XML_SUBSTITUTE_PEREF
 * @param end  an end marker xmlChar, 0 if none
 * @param end2  an end marker xmlChar, 0 if none
 * @param end3  an end marker xmlChar, 0 if none
 * @returns A newly allocated string with the substitution done. The caller
 *      must deallocate it !
 */
xmlChar *
xmlStringDecodeEntities(xmlParserCtxt *ctxt, const xmlChar *str,
                        int what ATTRIBUTE_UNUSED,
		        xmlChar end, xmlChar  end2, xmlChar end3) {
    if ((ctxt == NULL) || (str == NULL))
        return(NULL);

    if ((end != 0) || (end2 != 0) || (end3 != 0))
        return(NULL);

    return(xmlExpandEntitiesInAttValue(ctxt, str, 0));
}

/************************************************************************
 *									*
 *		Commodity functions, cleanup needed ?			*
 *									*
 ************************************************************************/

/**
 * Is this a sequence of blank chars that one can ignore ?
 *
 * @param ctxt  an XML parser context
 * @param str  a xmlChar *
 * @param len  the size of `str`
 * @param blank_chars  we know the chars are blanks
 * @returns 1 if ignorable 0 otherwise.
 */

static int areBlanks(xmlParserCtxtPtr ctxt, const xmlChar *str, int len,
                     int blank_chars) {
    int i;
    xmlNodePtr lastChild;

    /*
     * Check for xml:space value.
     */
    if ((ctxt->space == NULL) || (*(ctxt->space) == 1) ||
        (*(ctxt->space) == -2))
	return(0);

    /*
     * Check that the string is made of blanks
     */
    if (blank_chars == 0) {
	for (i = 0;i < len;i++)
	    if (!(IS_BLANK_CH(str[i]))) return(0);
    }

    /*
     * Look if the element is mixed content in the DTD if available
     */
    if (ctxt->node == NULL) return(0);
    if (ctxt->myDoc != NULL) {
        xmlElementPtr elemDecl = NULL;
        xmlDocPtr doc = ctxt->myDoc;
        const xmlChar *prefix = NULL;

        if (ctxt->node->ns)
            prefix = ctxt->node->ns->prefix;
        if (doc->intSubset != NULL)
            elemDecl = xmlHashLookup2(doc->intSubset->elements, ctxt->node->name,
                                      prefix);
        if ((elemDecl == NULL) && (doc->extSubset != NULL))
            elemDecl = xmlHashLookup2(doc->extSubset->elements, ctxt->node->name,
                                      prefix);
        if (elemDecl != NULL) {
            if (elemDecl->etype == XML_ELEMENT_TYPE_ELEMENT)
                return(1);
            if ((elemDecl->etype == XML_ELEMENT_TYPE_ANY) ||
                (elemDecl->etype == XML_ELEMENT_TYPE_MIXED))
                return(0);
        }
    }

    /*
     * Otherwise, heuristic :-\
     *
     * When push parsing, we could be at the end of a chunk.
     * This makes the look-ahead and consequently the NOBLANKS
     * option unreliable.
     */
    if ((RAW != '<') && (RAW != 0xD)) return(0);
    if ((ctxt->node->children == NULL) &&
	(RAW == '<') && (NXT(1) == '/')) return(0);

    lastChild = xmlGetLastChild(ctxt->node);
    if (lastChild == NULL) {
        if ((ctxt->node->type != XML_ELEMENT_NODE) &&
            (ctxt->node->content != NULL)) return(0);
    } else if (xmlNodeIsText(lastChild))
        return(0);
    else if ((ctxt->node->children != NULL) &&
             (xmlNodeIsText(ctxt->node->children)))
        return(0);
    return(1);
}

/************************************************************************
 *									*
 *		Extra stuff for namespace support			*
 *	Relates to http://www.w3.org/TR/WD-xml-names			*
 *									*
 ************************************************************************/

/**
 * Parse an UTF8 encoded XML qualified name string
 *
 * @deprecated Don't use.
 *
 * @param ctxt  an XML parser context
 * @param name  an XML parser context
 * @param prefixOut  a xmlChar **
 * @returns the local part, and prefix is updated
 *   to get the Prefix if any.
 */

xmlChar *
xmlSplitQName(xmlParserCtxt *ctxt, const xmlChar *name, xmlChar **prefixOut) {
    xmlChar *ret;
    const xmlChar *localname;

    localname = xmlSplitQName4(name, prefixOut);
    if (localname == NULL) {
        xmlCtxtErrMemory(ctxt);
        return(NULL);
    }

    ret = xmlStrdup(localname);
    if (ret == NULL) {
        xmlCtxtErrMemory(ctxt);
        xmlFree(*prefixOut);
    }

    return(ret);
}

/************************************************************************
 *									*
 *			The parser itself				*
 *	Relates to http://www.w3.org/TR/REC-xml				*
 *									*
 ************************************************************************/

/************************************************************************
 *									*
 *	Routines to parse Name, NCName and NmToken			*
 *									*
 ************************************************************************/

/*
 * The two following functions are related to the change of accepted
 * characters for Name and NmToken in the Revision 5 of XML-1.0
 * They correspond to the modified production [4] and the new production [4a]
 * changes in that revision. Also note that the macros used for the
 * productions Letter, Digit, CombiningChar and Extender are not needed
 * anymore.
 * We still keep compatibility to pre-revision5 parsing semantic if the
 * new XML_PARSE_OLD10 option is given to the parser.
 */

static int
xmlIsNameStartCharNew(int c) {
    /*
     * Use the new checks of production [4] [4a] amd [5] of the
     * Update 5 of XML-1.0
     */
    if ((c != ' ') && (c != '>') && (c != '/') && /* accelerators */
        (((c >= 'a') && (c <= 'z')) ||
         ((c >= 'A') && (c <= 'Z')) ||
         (c == '_') || (c == ':') ||
         ((c >= 0xC0) && (c <= 0xD6)) ||
         ((c >= 0xD8) && (c <= 0xF6)) ||
         ((c >= 0xF8) && (c <= 0x2FF)) ||
         ((c >= 0x370) && (c <= 0x37D)) ||
         ((c >= 0x37F) && (c <= 0x1FFF)) ||
         ((c >= 0x200C) && (c <= 0x200D)) ||
         ((c >= 0x2070) && (c <= 0x218F)) ||
         ((c >= 0x2C00) && (c <= 0x2FEF)) ||
         ((c >= 0x3001) && (c <= 0xD7FF)) ||
         ((c >= 0xF900) && (c <= 0xFDCF)) ||
         ((c >= 0xFDF0) && (c <= 0xFFFD)) ||
         ((c >= 0x10000) && (c <= 0xEFFFF))))
        return(1);
    return(0);
}

static int
xmlIsNameCharNew(int c) {
    /*
     * Use the new checks of production [4] [4a] amd [5] of the
     * Update 5 of XML-1.0
     */
    if ((c != ' ') && (c != '>') && (c != '/') && /* accelerators */
        (((c >= 'a') && (c <= 'z')) ||
         ((c >= 'A') && (c <= 'Z')) ||
         ((c >= '0') && (c <= '9')) || /* !start */
         (c == '_') || (c == ':') ||
         (c == '-') || (c == '.') || (c == 0xB7) || /* !start */
         ((c >= 0xC0) && (c <= 0xD6)) ||
         ((c >= 0xD8) && (c <= 0xF6)) ||
         ((c >= 0xF8) && (c <= 0x2FF)) ||
         ((c >= 0x300) && (c <= 0x36F)) || /* !start */
         ((c >= 0x370) && (c <= 0x37D)) ||
         ((c >= 0x37F) && (c <= 0x1FFF)) ||
         ((c >= 0x200C) && (c <= 0x200D)) ||
         ((c >= 0x203F) && (c <= 0x2040)) || /* !start */
         ((c >= 0x2070) && (c <= 0x218F)) ||
         ((c >= 0x2C00) && (c <= 0x2FEF)) ||
         ((c >= 0x3001) && (c <= 0xD7FF)) ||
         ((c >= 0xF900) && (c <= 0xFDCF)) ||
         ((c >= 0xFDF0) && (c <= 0xFFFD)) ||
         ((c >= 0x10000) && (c <= 0xEFFFF))))
         return(1);
    return(0);
}

static int
xmlIsNameStartCharOld(int c) {
    if ((c != ' ') && (c != '>') && (c != '/') && /* accelerators */
        ((IS_LETTER(c) || (c == '_') || (c == ':'))))
        return(1);
    return(0);
}

static int
xmlIsNameCharOld(int c) {
    if ((c != ' ') && (c != '>') && (c != '/') && /* accelerators */
        ((IS_LETTER(c)) || (IS_DIGIT(c)) ||
         (c == '.') || (c == '-') ||
         (c == '_') || (c == ':') ||
         (IS_COMBINING(c)) ||
         (IS_EXTENDER(c))))
        return(1);
    return(0);
}

static int
xmlIsNameStartChar(int c, int old10) {
    if (!old10)
        return(xmlIsNameStartCharNew(c));
    else
        return(xmlIsNameStartCharOld(c));
}

static int
xmlIsNameChar(int c, int old10) {
    if (!old10)
        return(xmlIsNameCharNew(c));
    else
        return(xmlIsNameCharOld(c));
}

/*
 * Scan an XML Name, NCName or Nmtoken.
 *
 * Returns a pointer to the end of the name on success. If the
 * name is invalid, returns `ptr`. If the name is longer than
 * `maxSize` bytes, returns NULL.
 *
 * @param ptr  pointer to the start of the name
 * @param maxSize  maximum size in bytes
 * @param flags  XML_SCAN_* flags
 * @returns a pointer to the end of the name or NULL
 */
const xmlChar *
xmlScanName(const xmlChar *ptr, size_t maxSize, int flags) {
    int stop = flags & XML_SCAN_NC ? ':' : 0;
    int old10 = flags & XML_SCAN_OLD10 ? 1 : 0;

    while (1) {
        int c, len;

        c = *ptr;
        if (c < 0x80) {
            if (c == stop)
                break;
            len = 1;
        } else {
            len = 4;
            c = xmlGetUTF8Char(ptr, &len);
            if (c < 0)
                break;
        }

        if (flags & XML_SCAN_NMTOKEN ?
                !xmlIsNameChar(c, old10) :
                !xmlIsNameStartChar(c, old10))
            break;

        if ((size_t) len > maxSize)
            return(NULL);
        ptr += len;
        maxSize -= len;
        flags |= XML_SCAN_NMTOKEN;
    }

    return(ptr);
}

static const xmlChar *
xmlParseNameComplex(xmlParserCtxtPtr ctxt) {
    const xmlChar *ret;
    int len = 0, l;
    int c;
    int maxLength = (ctxt->options & XML_PARSE_HUGE) ?
                    XML_MAX_TEXT_LENGTH :
                    XML_MAX_NAME_LENGTH;
    int old10 = (ctxt->options & XML_PARSE_OLD10) ? 1 : 0;

    /*
     * Handler for more complex cases
     */
    c = xmlCurrentChar(ctxt, &l);
    if (!xmlIsNameStartChar(c, old10))
        return(NULL);
    len += l;
    NEXTL(l);
    c = xmlCurrentChar(ctxt, &l);
    while (xmlIsNameChar(c, old10)) {
        if (len <= INT_MAX - l)
            len += l;
        NEXTL(l);
        c = xmlCurrentChar(ctxt, &l);
    }
    if (len > maxLength) {
        xmlFatalErr(ctxt, XML_ERR_NAME_TOO_LONG, "Name");
        return(NULL);
    }
    if (ctxt->input->cur - ctxt->input->base < len) {
        /*
         * There were a couple of bugs where PERefs lead to to a change
         * of the buffer. Check the buffer size to avoid passing an invalid
         * pointer to xmlDictLookup.
         */
        xmlFatalErr(ctxt, XML_ERR_INTERNAL_ERROR,
                    "unexpected change of input buffer");
        return (NULL);
    }
    if ((*ctxt->input->cur == '\n') && (ctxt->input->cur[-1] == '\r'))
        ret = xmlDictLookup(ctxt->dict, ctxt->input->cur - (len + 1), len);
    else
        ret = xmlDictLookup(ctxt->dict, ctxt->input->cur - len, len);
    if (ret == NULL)
        xmlErrMemory(ctxt);
    return(ret);
}

/**
 * Parse an XML name.
 *
 * @deprecated Internal function, don't use.
 *
 *     [4] NameChar ::= Letter | Digit | '.' | '-' | '_' | ':' |
 *                      CombiningChar | Extender
 *
 *     [5] Name ::= (Letter | '_' | ':') (NameChar)*
 *
 *     [6] Names ::= Name (#x20 Name)*
 *
 * @param ctxt  an XML parser context
 * @returns the Name parsed or NULL
 */

const xmlChar *
xmlParseName(xmlParserCtxt *ctxt) {
    const xmlChar *in;
    const xmlChar *ret;
    size_t count = 0;
    size_t maxLength = (ctxt->options & XML_PARSE_HUGE) ?
                       XML_MAX_TEXT_LENGTH :
                       XML_MAX_NAME_LENGTH;

    GROW;

    /*
     * Accelerator for simple ASCII names
     */
    in = ctxt->input->cur;
    if (((*in >= 0x61) && (*in <= 0x7A)) ||
	((*in >= 0x41) && (*in <= 0x5A)) ||
	(*in == '_') || (*in == ':')) {
	in++;
	while (((*in >= 0x61) && (*in <= 0x7A)) ||
	       ((*in >= 0x41) && (*in <= 0x5A)) ||
	       ((*in >= 0x30) && (*in <= 0x39)) ||
	       (*in == '_') || (*in == '-') ||
	       (*in == ':') || (*in == '.'))
	    in++;
	if ((*in > 0) && (*in < 0x80)) {
	    count = in - ctxt->input->cur;
            if (count > maxLength) {
                xmlFatalErr(ctxt, XML_ERR_NAME_TOO_LONG, "Name");
                return(NULL);
            }
	    ret = xmlDictLookup(ctxt->dict, ctxt->input->cur, count);
	    ctxt->input->cur = in;
	    ctxt->input->col += count;
	    if (ret == NULL)
	        xmlErrMemory(ctxt);
	    return(ret);
	}
    }
    /* accelerator for special cases */
    return(xmlParseNameComplex(ctxt));
}

static xmlHashedString
xmlParseNCNameComplex(xmlParserCtxtPtr ctxt) {
    xmlHashedString ret;
    int len = 0, l;
    int c;
    int maxLength = (ctxt->options & XML_PARSE_HUGE) ?
                    XML_MAX_TEXT_LENGTH :
                    XML_MAX_NAME_LENGTH;
    int old10 = (ctxt->options & XML_PARSE_OLD10) ? 1 : 0;
    size_t startPosition = 0;

    ret.name = NULL;
    ret.hashValue = 0;

    /*
     * Handler for more complex cases
     */
    startPosition = CUR_PTR - BASE_PTR;
    c = xmlCurrentChar(ctxt, &l);
    if ((c == ' ') || (c == '>') || (c == '/') || /* accelerators */
	(!xmlIsNameStartChar(c, old10) || (c == ':'))) {
	return(ret);
    }

    while ((c != ' ') && (c != '>') && (c != '/') && /* test bigname.xml */
	   (xmlIsNameChar(c, old10) && (c != ':'))) {
        if (len <= INT_MAX - l)
	    len += l;
	NEXTL(l);
	c = xmlCurrentChar(ctxt, &l);
    }
    if (len > maxLength) {
        xmlFatalErr(ctxt, XML_ERR_NAME_TOO_LONG, "NCName");
        return(ret);
    }
    ret = xmlDictLookupHashed(ctxt->dict, (BASE_PTR + startPosition), len);
    if (ret.name == NULL)
        xmlErrMemory(ctxt);
    return(ret);
}

/**
 * Parse an XML name.
 *
 *     [4NS] NCNameChar ::= Letter | Digit | '.' | '-' | '_' |
 *                          CombiningChar | Extender
 *
 *     [5NS] NCName ::= (Letter | '_') (NCNameChar)*
 *
 * @param ctxt  an XML parser context
 * @returns the Name parsed or NULL
 */

static xmlHashedString
xmlParseNCName(xmlParserCtxtPtr ctxt) {
    const xmlChar *in, *e;
    xmlHashedString ret;
    size_t count = 0;
    size_t maxLength = (ctxt->options & XML_PARSE_HUGE) ?
                       XML_MAX_TEXT_LENGTH :
                       XML_MAX_NAME_LENGTH;

    ret.name = NULL;

    /*
     * Accelerator for simple ASCII names
     */
    in = ctxt->input->cur;
    e = ctxt->input->end;
    if ((((*in >= 0x61) && (*in <= 0x7A)) ||
	 ((*in >= 0x41) && (*in <= 0x5A)) ||
	 (*in == '_')) && (in < e)) {
	in++;
	while ((((*in >= 0x61) && (*in <= 0x7A)) ||
	        ((*in >= 0x41) && (*in <= 0x5A)) ||
	        ((*in >= 0x30) && (*in <= 0x39)) ||
	        (*in == '_') || (*in == '-') ||
	        (*in == '.')) && (in < e))
	    in++;
	if (in >= e)
	    goto complex;
	if ((*in > 0) && (*in < 0x80)) {
	    count = in - ctxt->input->cur;
            if (count > maxLength) {
                xmlFatalErr(ctxt, XML_ERR_NAME_TOO_LONG, "NCName");
                return(ret);
            }
	    ret = xmlDictLookupHashed(ctxt->dict, ctxt->input->cur, count);
	    ctxt->input->cur = in;
	    ctxt->input->col += count;
	    if (ret.name == NULL) {
	        xmlErrMemory(ctxt);
	    }
	    return(ret);
	}
    }
complex:
    return(xmlParseNCNameComplex(ctxt));
}

/**
 * Parse an XML name and compares for match
 * (specialized for endtag parsing)
 *
 * @param ctxt  an XML parser context
 * @param other  the name to compare with
 * @returns NULL for an illegal name, (xmlChar*) 1 for success
 * and the name for mismatch
 */

static const xmlChar *
xmlParseNameAndCompare(xmlParserCtxtPtr ctxt, xmlChar const *other) {
    register const xmlChar *cmp = other;
    register const xmlChar *in;
    const xmlChar *ret;

    GROW;

    in = ctxt->input->cur;
    while (*in != 0 && *in == *cmp) {
	++in;
	++cmp;
    }
    if (*cmp == 0 && (*in == '>' || IS_BLANK_CH (*in))) {
	/* success */
	ctxt->input->col += in - ctxt->input->cur;
	ctxt->input->cur = in;
	return (const xmlChar*) 1;
    }
    /* failure (or end of input buffer), check with full function */
    ret = xmlParseName (ctxt);
    /* strings coming from the dictionary direct compare possible */
    if (ret == other) {
	return (const xmlChar*) 1;
    }
    return ret;
}

/**
 * Parse an XML name.
 *
 * @param ctxt  an XML parser context
 * @param str  a pointer to the string pointer (IN/OUT)
 * @returns the Name parsed or NULL. The `str` pointer
 * is updated to the current location in the string.
 */

static xmlChar *
xmlParseStringName(xmlParserCtxtPtr ctxt, const xmlChar** str) {
    xmlChar *ret;
    const xmlChar *cur = *str;
    int flags = 0;
    int maxLength = (ctxt->options & XML_PARSE_HUGE) ?
                    XML_MAX_TEXT_LENGTH :
                    XML_MAX_NAME_LENGTH;

    if (ctxt->options & XML_PARSE_OLD10)
        flags |= XML_SCAN_OLD10;

    cur = xmlScanName(*str, maxLength, flags);
    if (cur == NULL) {
        xmlFatalErr(ctxt, XML_ERR_NAME_TOO_LONG, "NCName");
        return(NULL);
    }
    if (cur == *str)
        return(NULL);

    ret = xmlStrndup(*str, cur - *str);
    if (ret == NULL)
        xmlErrMemory(ctxt);
    *str = cur;
    return(ret);
}

/**
 * Parse an XML Nmtoken.
 *
 * @deprecated Internal function, don't use.
 *
 *     [7] Nmtoken ::= (NameChar)+
 *
 *     [8] Nmtokens ::= Nmtoken (#x20 Nmtoken)*
 *
 * @param ctxt  an XML parser context
 * @returns the Nmtoken parsed or NULL
 */

xmlChar *
xmlParseNmtoken(xmlParserCtxt *ctxt) {
    xmlChar buf[XML_MAX_NAMELEN + 5];
    xmlChar *ret;
    int len = 0, l;
    int c;
    int maxLength = (ctxt->options & XML_PARSE_HUGE) ?
                    XML_MAX_TEXT_LENGTH :
                    XML_MAX_NAME_LENGTH;
    int old10 = (ctxt->options & XML_PARSE_OLD10) ? 1 : 0;

    c = xmlCurrentChar(ctxt, &l);

    while (xmlIsNameChar(c, old10)) {
	COPY_BUF(buf, len, c);
	NEXTL(l);
	c = xmlCurrentChar(ctxt, &l);
	if (len >= XML_MAX_NAMELEN) {
	    /*
	     * Okay someone managed to make a huge token, so he's ready to pay
	     * for the processing speed.
	     */
	    xmlChar *buffer;
	    int max = len * 2;

	    buffer = xmlMalloc(max);
	    if (buffer == NULL) {
	        xmlErrMemory(ctxt);
		return(NULL);
	    }
	    memcpy(buffer, buf, len);
	    while (xmlIsNameChar(c, old10)) {
		if (len + 10 > max) {
		    xmlChar *tmp;
                    int newSize;

                    newSize = xmlGrowCapacity(max, 1, 1, maxLength);
                    if (newSize < 0) {
                        xmlFatalErr(ctxt, XML_ERR_NAME_TOO_LONG, "NmToken");
                        xmlFree(buffer);
                        return(NULL);
                    }
		    tmp = xmlRealloc(buffer, newSize);
		    if (tmp == NULL) {
			xmlErrMemory(ctxt);
			xmlFree(buffer);
			return(NULL);
		    }
		    buffer = tmp;
                    max = newSize;
		}
		COPY_BUF(buffer, len, c);
		NEXTL(l);
		c = xmlCurrentChar(ctxt, &l);
	    }
	    buffer[len] = 0;
	    return(buffer);
	}
    }
    if (len == 0)
        return(NULL);
    if (len > maxLength) {
        xmlFatalErr(ctxt, XML_ERR_NAME_TOO_LONG, "NmToken");
        return(NULL);
    }
    ret = xmlStrndup(buf, len);
    if (ret == NULL)
        xmlErrMemory(ctxt);
    return(ret);
}

/**
 * Validate an entity value and expand parameter entities.
 *
 * @param ctxt  parser context
 * @param buf  string buffer
 * @param str  entity value
 * @param length  size of entity value
 * @param depth  nesting depth
 */
static void
xmlExpandPEsInEntityValue(xmlParserCtxtPtr ctxt, xmlSBuf *buf,
                          const xmlChar *str, int length, int depth) {
    int maxDepth = (ctxt->options & XML_PARSE_HUGE) ? 40 : 20;
    const xmlChar *end, *chunk;
    int c, l;

    if (str == NULL)
        return;

    depth += 1;
    if (depth > maxDepth) {
	xmlFatalErrMsg(ctxt, XML_ERR_RESOURCE_LIMIT,
                       "Maximum entity nesting depth exceeded");
	return;
    }

    end = str + length;
    chunk = str;

    while ((str < end) && (!PARSER_STOPPED(ctxt))) {
        c = *str;

        if (c >= 0x80) {
            l = xmlUTF8MultibyteLen(ctxt, str,
                    "invalid character in entity value\n");
            if (l == 0) {
                if (chunk < str)
                    xmlSBufAddString(buf, chunk, str - chunk);
                xmlSBufAddReplChar(buf);
                str += 1;
                chunk = str;
            } else {
                str += l;
            }
        } else if (c == '&') {
            if (str[1] == '#') {
                if (chunk < str)
                    xmlSBufAddString(buf, chunk, str - chunk);

                c = xmlParseStringCharRef(ctxt, &str);
                if (c == 0)
                    return;

                xmlSBufAddChar(buf, c);

                chunk = str;
            } else {
                xmlChar *name;

                /*
                 * General entity references are checked for
                 * syntactic validity.
                 */
                str++;
                name = xmlParseStringName(ctxt, &str);

                if ((name == NULL) || (*str++ != ';')) {
                    xmlFatalErrMsg(ctxt, XML_ERR_ENTITY_CHAR_ERROR,
                            "EntityValue: '&' forbidden except for entities "
                            "references\n");
                    xmlFree(name);
                    return;
                }

                xmlFree(name);
            }
        } else if (c == '%') {
            xmlEntityPtr ent;

            if (chunk < str)
                xmlSBufAddString(buf, chunk, str - chunk);

            ent = xmlParseStringPEReference(ctxt, &str);
            if (ent == NULL)
                return;

            if (!PARSER_EXTERNAL(ctxt)) {
                xmlFatalErr(ctxt, XML_ERR_ENTITY_PE_INTERNAL, NULL);
                return;
            }

            if (ent->content == NULL) {
                /*
                 * Note: external parsed entities will not be loaded,
                 * it is not required for a non-validating parser to
                 * complete external PEReferences coming from the
                 * internal subset
                 */
                if (((ctxt->options & XML_PARSE_NO_XXE) == 0) &&
                    ((ctxt->replaceEntities) ||
                     (ctxt->validate))) {
                    xmlLoadEntityContent(ctxt, ent);
                } else {
                    xmlWarningMsg(ctxt, XML_ERR_ENTITY_PROCESSING,
                                  "not validating will not read content for "
                                  "PE entity %s\n", ent->name, NULL);
                }
            }

            /*
             * TODO: Skip if ent->content is still NULL.
             */

            if (xmlParserEntityCheck(ctxt, ent->length))
                return;

            if (ent->flags & XML_ENT_EXPANDING) {
                xmlFatalErr(ctxt, XML_ERR_ENTITY_LOOP, NULL);
                return;
            }

            ent->flags |= XML_ENT_EXPANDING;
            xmlExpandPEsInEntityValue(ctxt, buf, ent->content, ent->length,
                                      depth);
            ent->flags &= ~XML_ENT_EXPANDING;

            chunk = str;
        } else {
            /* Normal ASCII char */
            if (!IS_BYTE_CHAR(c)) {
                xmlFatalErrMsg(ctxt, XML_ERR_INVALID_CHAR,
                        "invalid character in entity value\n");
                if (chunk < str)
                    xmlSBufAddString(buf, chunk, str - chunk);
                xmlSBufAddReplChar(buf);
                str += 1;
                chunk = str;
            } else {
                str += 1;
            }
        }
    }

    if (chunk < str)
        xmlSBufAddString(buf, chunk, str - chunk);
}

/**
 * Parse a value for ENTITY declarations
 *
 * @deprecated Internal function, don't use.
 *
 *     [9] EntityValue ::= '"' ([^%&"] | PEReference | Reference)* '"' |
 *                         "'" ([^%&'] | PEReference | Reference)* "'"
 *
 * @param ctxt  an XML parser context
 * @param orig  if non-NULL store a copy of the original entity value
 * @returns the EntityValue parsed with reference substituted or NULL
 */
xmlChar *
xmlParseEntityValue(xmlParserCtxt *ctxt, xmlChar **orig) {
    unsigned maxLength = (ctxt->options & XML_PARSE_HUGE) ?
                         XML_MAX_HUGE_LENGTH :
                         XML_MAX_TEXT_LENGTH;
    xmlSBuf buf;
    const xmlChar *start;
    int quote, length;

    xmlSBufInit(&buf, maxLength);

    GROW;

    quote = CUR;
    if ((quote != '"') && (quote != '\'')) {
	xmlFatalErr(ctxt, XML_ERR_ATTRIBUTE_NOT_STARTED, NULL);
	return(NULL);
    }
    CUR_PTR++;

    length = 0;

    /*
     * Copy raw content of the entity into a buffer
     */
    while (1) {
        int c;

        if (PARSER_STOPPED(ctxt))
            goto error;

        if (CUR_PTR >= ctxt->input->end) {
            xmlFatalErrMsg(ctxt, XML_ERR_ENTITY_NOT_FINISHED, NULL);
            goto error;
        }

        c = CUR;

        if (c == 0) {
            xmlFatalErrMsg(ctxt, XML_ERR_INVALID_CHAR,
                    "invalid character in entity value\n");
            goto error;
        }
        if (c == quote)
            break;
        NEXTL(1);
        length += 1;

        /*
         * TODO: Check growth threshold
         */
        if (ctxt->input->end - CUR_PTR < 10)
            GROW;
    }

    start = CUR_PTR - length;

    if (orig != NULL) {
        *orig = xmlStrndup(start, length);
        if (*orig == NULL)
            xmlErrMemory(ctxt);
    }

    xmlExpandPEsInEntityValue(ctxt, &buf, start, length, ctxt->inputNr);

    NEXTL(1);

    return(xmlSBufFinish(&buf, NULL, ctxt, "entity length too long"));

error:
    xmlSBufCleanup(&buf, ctxt, "entity length too long");
    return(NULL);
}

/**
 * Check an entity reference in an attribute value for validity
 * without expanding it.
 *
 * @param ctxt  parser context
 * @param pent  entity
 * @param depth  nesting depth
 */
static void
xmlCheckEntityInAttValue(xmlParserCtxtPtr ctxt, xmlEntityPtr pent, int depth) {
    int maxDepth = (ctxt->options & XML_PARSE_HUGE) ? 40 : 20;
    const xmlChar *str;
    unsigned long expandedSize = pent->length;
    int c, flags;

    depth += 1;
    if (depth > maxDepth) {
	xmlFatalErrMsg(ctxt, XML_ERR_RESOURCE_LIMIT,
                       "Maximum entity nesting depth exceeded");
	return;
    }

    if (pent->flags & XML_ENT_EXPANDING) {
        xmlFatalErr(ctxt, XML_ERR_ENTITY_LOOP, NULL);
        return;
    }

    /*
     * If we're parsing a default attribute value in DTD content,
     * the entity might reference other entities which weren't
     * defined yet, so the check isn't reliable.
     */
    if (ctxt->inSubset == 0)
        flags = XML_ENT_CHECKED | XML_ENT_VALIDATED;
    else
        flags = XML_ENT_VALIDATED;

    str = pent->content;
    if (str == NULL)
        goto done;

    /*
     * Note that entity values are already validated. We only check
     * for illegal less-than signs and compute the expanded size
     * of the entity. No special handling for multi-byte characters
     * is needed.
     */
    while (!PARSER_STOPPED(ctxt)) {
        c = *str;

	if (c != '&') {
            if (c == 0)
                break;

            if (c == '<')
                xmlFatalErrMsgStr(ctxt, XML_ERR_LT_IN_ATTRIBUTE,
                        "'<' in entity '%s' is not allowed in attributes "
                        "values\n", pent->name);

            str += 1;
        } else if (str[1] == '#') {
            int val;

	    val = xmlParseStringCharRef(ctxt, &str);
	    if (val == 0) {
                pent->content[0] = 0;
                break;
            }
	} else {
            xmlChar *name;
            xmlEntityPtr ent;

	    name = xmlParseStringEntityRef(ctxt, &str);
	    if (name == NULL) {
                pent->content[0] = 0;
                break;
            }

            ent = xmlLookupGeneralEntity(ctxt, name, /* inAttr */ 1);
            xmlFree(name);

            if ((ent != NULL) &&
                (ent->etype != XML_INTERNAL_PREDEFINED_ENTITY)) {
                if ((ent->flags & flags) != flags) {
                    pent->flags |= XML_ENT_EXPANDING;
                    xmlCheckEntityInAttValue(ctxt, ent, depth);
                    pent->flags &= ~XML_ENT_EXPANDING;
                }

                xmlSaturatedAdd(&expandedSize, ent->expandedSize);
                xmlSaturatedAdd(&expandedSize, XML_ENT_FIXED_COST);
            }
        }
    }

done:
    if (ctxt->inSubset == 0)
        pent->expandedSize = expandedSize;

    pent->flags |= flags;
}

/**
 * Expand general entity references in an entity or attribute value.
 * Perform attribute value normalization.
 *
 * @param ctxt  parser context
 * @param buf  string buffer
 * @param str  entity or attribute value
 * @param pent  entity for entity value, NULL for attribute values
 * @param normalize  whether to collapse whitespace
 * @param inSpace  whitespace state
 * @param depth  nesting depth
 * @param check  whether to check for amplification
 * @returns  whether there was a normalization change
 */
static int
xmlExpandEntityInAttValue(xmlParserCtxtPtr ctxt, xmlSBuf *buf,
                          const xmlChar *str, xmlEntityPtr pent, int normalize,
                          int *inSpace, int depth, int check) {
    int maxDepth = (ctxt->options & XML_PARSE_HUGE) ? 40 : 20;
    int c, chunkSize;
    int normChange = 0;

    if (str == NULL)
        return(0);

    depth += 1;
    if (depth > maxDepth) {
	xmlFatalErrMsg(ctxt, XML_ERR_RESOURCE_LIMIT,
                       "Maximum entity nesting depth exceeded");
	return(0);
    }

    if (pent != NULL) {
        if (pent->flags & XML_ENT_EXPANDING) {
            xmlFatalErr(ctxt, XML_ERR_ENTITY_LOOP, NULL);
            return(0);
        }

        if (check) {
            if (xmlParserEntityCheck(ctxt, pent->length))
                return(0);
        }
    }

    chunkSize = 0;

    /*
     * Note that entity values are already validated. No special
     * handling for multi-byte characters is needed.
     */
    while (!PARSER_STOPPED(ctxt)) {
        c = *str;

	if (c != '&') {
            if (c == 0)
                break;

            /*
             * If this function is called without an entity, it is used to
             * expand entities in an attribute content where less-than was
             * already unscaped and is allowed.
             */
            if ((pent != NULL) && (c == '<')) {
                xmlFatalErrMsgStr(ctxt, XML_ERR_LT_IN_ATTRIBUTE,
                        "'<' in entity '%s' is not allowed in attributes "
                        "values\n", pent->name);
                break;
            }

            if (c <= 0x20) {
                if ((normalize) && (*inSpace)) {
                    /* Skip char */
                    if (chunkSize > 0) {
                        xmlSBufAddString(buf, str - chunkSize, chunkSize);
                        chunkSize = 0;
                    }
                    normChange = 1;
                } else if (c < 0x20) {
                    if (chunkSize > 0) {
                        xmlSBufAddString(buf, str - chunkSize, chunkSize);
                        chunkSize = 0;
                    }

                    xmlSBufAddCString(buf, " ", 1);
                } else {
                    chunkSize += 1;
                }

                *inSpace = 1;
            } else {
                chunkSize += 1;
                *inSpace = 0;
            }

            str += 1;
        } else if (str[1] == '#') {
            int val;

            if (chunkSize > 0) {
                xmlSBufAddString(buf, str - chunkSize, chunkSize);
                chunkSize = 0;
            }

	    val = xmlParseStringCharRef(ctxt, &str);
	    if (val == 0) {
                if (pent != NULL)
                    pent->content[0] = 0;
                break;
            }

            if (val == ' ') {
                if ((normalize) && (*inSpace))
                    normChange = 1;
                else
                    xmlSBufAddCString(buf, " ", 1);
                *inSpace = 1;
            } else {
                xmlSBufAddChar(buf, val);
                *inSpace = 0;
            }
	} else {
            xmlChar *name;
            xmlEntityPtr ent;

            if (chunkSize > 0) {
                xmlSBufAddString(buf, str - chunkSize, chunkSize);
                chunkSize = 0;
            }

	    name = xmlParseStringEntityRef(ctxt, &str);
            if (name == NULL) {
                if (pent != NULL)
                    pent->content[0] = 0;
                break;
            }

            ent = xmlLookupGeneralEntity(ctxt, name, /* inAttr */ 1);
            xmlFree(name);

	    if ((ent != NULL) &&
		(ent->etype == XML_INTERNAL_PREDEFINED_ENTITY)) {
		if (ent->content == NULL) {
		    xmlFatalErrMsg(ctxt, XML_ERR_INTERNAL_ERROR,
			    "predefined entity has no content\n");
                    break;
                }

                xmlSBufAddString(buf, ent->content, ent->length);

                *inSpace = 0;
	    } else if ((ent != NULL) && (ent->content != NULL)) {
                if (pent != NULL)
                    pent->flags |= XML_ENT_EXPANDING;
		normChange |= xmlExpandEntityInAttValue(ctxt, buf,
                        ent->content, ent, normalize, inSpace, depth, check);
                if (pent != NULL)
                    pent->flags &= ~XML_ENT_EXPANDING;
	    }
        }
    }

    if (chunkSize > 0)
        xmlSBufAddString(buf, str - chunkSize, chunkSize);

    return(normChange);
}

/**
 * Expand general entity references in an entity or attribute value.
 * Perform attribute value normalization.
 *
 * @param ctxt  parser context
 * @param str  entity or attribute value
 * @param normalize  whether to collapse whitespace
 * @returns the expanded attribtue value.
 */
xmlChar *
xmlExpandEntitiesInAttValue(xmlParserCtxt *ctxt, const xmlChar *str,
                            int normalize) {
    unsigned maxLength = (ctxt->options & XML_PARSE_HUGE) ?
                         XML_MAX_HUGE_LENGTH :
                         XML_MAX_TEXT_LENGTH;
    xmlSBuf buf;
    int inSpace = 1;

    xmlSBufInit(&buf, maxLength);

    xmlExpandEntityInAttValue(ctxt, &buf, str, NULL, normalize, &inSpace,
                              ctxt->inputNr, /* check */ 0);

    if ((normalize) && (inSpace) && (buf.size > 0))
        buf.size--;

    return(xmlSBufFinish(&buf, NULL, ctxt, "AttValue length too long"));
}

/**
 * Parse a value for an attribute.
 *
 * NOTE: if no normalization is needed, the routine will return pointers
 * directly from the data buffer.
 *
 * 3.3.3 Attribute-Value Normalization:
 *
 * Before the value of an attribute is passed to the application or
 * checked for validity, the XML processor must normalize it as follows:
 *
 * - a character reference is processed by appending the referenced
 *   character to the attribute value
 * - an entity reference is processed by recursively processing the
 *   replacement text of the entity
 * - a whitespace character (\#x20, \#xD, \#xA, \#x9) is processed by
 *   appending \#x20 to the normalized value, except that only a single
 *   \#x20 is appended for a "#xD#xA" sequence that is part of an external
 *   parsed entity or the literal entity value of an internal parsed entity
 * - other characters are processed by appending them to the normalized value
 *
 * If the declared value is not CDATA, then the XML processor must further
 * process the normalized attribute value by discarding any leading and
 * trailing space (\#x20) characters, and by replacing sequences of space
 * (\#x20) characters by a single space (\#x20) character.
 * All attributes for which no declaration has been read should be treated
 * by a non-validating parser as if declared CDATA.
 *
 * @param ctxt  an XML parser context
 * @param attlen  attribute len result
 * @param outFlags  resulting XML_ATTVAL_* flags
 * @param special  value from attsSpecial
 * @param isNamespace  whether this is a namespace declaration
 * @returns the AttValue parsed or NULL. The value has to be freed by the
 *     caller if it was copied, this can be detected by val[*len] == 0.
 */
static xmlChar *
xmlParseAttValueInternal(xmlParserCtxtPtr ctxt, int *attlen, int *outFlags,
                         int special, int isNamespace) {
    unsigned maxLength = (ctxt->options & XML_PARSE_HUGE) ?
                         XML_MAX_HUGE_LENGTH :
                         XML_MAX_TEXT_LENGTH;
    xmlSBuf buf;
    xmlChar *ret;
    int c, l, quote, entFlags, chunkSize;
    int inSpace = 1;
    int replaceEntities;
    int normalize = (special & XML_SPECIAL_TYPE_MASK) > XML_ATTRIBUTE_CDATA;
    int attvalFlags = 0;

    /* Always expand namespace URIs */
    replaceEntities = (ctxt->replaceEntities) || (isNamespace);

    xmlSBufInit(&buf, maxLength);

    GROW;

    quote = CUR;
    if ((quote != '"') && (quote != '\'')) {
	xmlFatalErr(ctxt, XML_ERR_ATTRIBUTE_NOT_STARTED, NULL);
	return(NULL);
    }
    NEXTL(1);

    if (ctxt->inSubset == 0)
        entFlags = XML_ENT_CHECKED | XML_ENT_VALIDATED;
    else
        entFlags = XML_ENT_VALIDATED;

    inSpace = 1;
    chunkSize = 0;

    while (1) {
        if (PARSER_STOPPED(ctxt))
            goto error;

        if (CUR_PTR >= ctxt->input->end) {
            xmlFatalErrMsg(ctxt, XML_ERR_ATTRIBUTE_NOT_FINISHED,
                           "AttValue: ' expected\n");
            goto error;
        }

        /*
         * TODO: Check growth threshold
         */
        if (ctxt->input->end - CUR_PTR < 10)
            GROW;

        c = CUR;

        if (c >= 0x80) {
            l = xmlUTF8MultibyteLen(ctxt, CUR_PTR,
                    "invalid character in attribute value\n");
            if (l == 0) {
                if (chunkSize > 0) {
                    xmlSBufAddString(&buf, CUR_PTR - chunkSize, chunkSize);
                    chunkSize = 0;
                }
                xmlSBufAddReplChar(&buf);
                NEXTL(1);
            } else {
                chunkSize += l;
                NEXTL(l);
            }

            inSpace = 0;
        } else if (c != '&') {
            if (c > 0x20) {
                if (c == quote)
                    break;

                if (c == '<')
                    xmlFatalErr(ctxt, XML_ERR_LT_IN_ATTRIBUTE, NULL);

                chunkSize += 1;
                inSpace = 0;
            } else if (!IS_BYTE_CHAR(c)) {
                xmlFatalErrMsg(ctxt, XML_ERR_INVALID_CHAR,
                        "invalid character in attribute value\n");
                if (chunkSize > 0) {
                    xmlSBufAddString(&buf, CUR_PTR - chunkSize, chunkSize);
                    chunkSize = 0;
                }
                xmlSBufAddReplChar(&buf);
                inSpace = 0;
            } else {
                /* Whitespace */
                if ((normalize) && (inSpace)) {
                    /* Skip char */
                    if (chunkSize > 0) {
                        xmlSBufAddString(&buf, CUR_PTR - chunkSize, chunkSize);
                        chunkSize = 0;
                    }
                    attvalFlags |= XML_ATTVAL_NORM_CHANGE;
                } else if (c < 0x20) {
                    /* Convert to space */
                    if (chunkSize > 0) {
                        xmlSBufAddString(&buf, CUR_PTR - chunkSize, chunkSize);
                        chunkSize = 0;
                    }

                    xmlSBufAddCString(&buf, " ", 1);
                } else {
                    chunkSize += 1;
                }

                inSpace = 1;

                if ((c == 0xD) && (NXT(1) == 0xA))
                    CUR_PTR++;
            }

            NEXTL(1);
        } else if (NXT(1) == '#') {
            int val;

            if (chunkSize > 0) {
                xmlSBufAddString(&buf, CUR_PTR - chunkSize, chunkSize);
                chunkSize = 0;
            }

            val = xmlParseCharRef(ctxt);
            if (val == 0)
                goto error;

            if ((val == '&') && (!replaceEntities)) {
                /*
                 * The reparsing will be done in xmlNodeParseContent()
                 * called from SAX2.c
                 */
                xmlSBufAddCString(&buf, "&#38;", 5);
                inSpace = 0;
            } else if (val == ' ') {
                if ((normalize) && (inSpace))
                    attvalFlags |= XML_ATTVAL_NORM_CHANGE;
                else
                    xmlSBufAddCString(&buf, " ", 1);
                inSpace = 1;
            } else {
                xmlSBufAddChar(&buf, val);
                inSpace = 0;
            }
        } else {
            const xmlChar *name;
            xmlEntityPtr ent;

            if (chunkSize > 0) {
                xmlSBufAddString(&buf, CUR_PTR - chunkSize, chunkSize);
                chunkSize = 0;
            }

            name = xmlParseEntityRefInternal(ctxt);
            if (name == NULL) {
                /*
                 * Probably a literal '&' which wasn't escaped.
                 * TODO: Handle gracefully in recovery mode.
                 */
                continue;
            }

            ent = xmlLookupGeneralEntity(ctxt, name, /* isAttr */ 1);
            if (ent == NULL)
                continue;

            if (ent->etype == XML_INTERNAL_PREDEFINED_ENTITY) {
                if ((ent->content[0] == '&') && (!replaceEntities))
                    xmlSBufAddCString(&buf, "&#38;", 5);
                else
                    xmlSBufAddString(&buf, ent->content, ent->length);
                inSpace = 0;
            } else if (replaceEntities) {
                if (xmlExpandEntityInAttValue(ctxt, &buf,
                        ent->content, ent, normalize, &inSpace, ctxt->inputNr,
                        /* check */ 1) > 0)
                    attvalFlags |= XML_ATTVAL_NORM_CHANGE;
            } else {
                if ((ent->flags & entFlags) != entFlags)
                    xmlCheckEntityInAttValue(ctxt, ent, ctxt->inputNr);

                if (xmlParserEntityCheck(ctxt, ent->expandedSize)) {
                    ent->content[0] = 0;
                    goto error;
                }

                /*
                 * Just output the reference
                 */
                xmlSBufAddCString(&buf, "&", 1);
                xmlSBufAddString(&buf, ent->name, xmlStrlen(ent->name));
                xmlSBufAddCString(&buf, ";", 1);

                inSpace = 0;
            }
	}
    }

    if ((buf.mem == NULL) && (outFlags != NULL)) {
        ret = (xmlChar *) CUR_PTR - chunkSize;

        if (attlen != NULL)
            *attlen = chunkSize;
        if ((normalize) && (inSpace) && (chunkSize > 0)) {
            attvalFlags |= XML_ATTVAL_NORM_CHANGE;
            *attlen -= 1;
        }

        /* Report potential error */
        xmlSBufCleanup(&buf, ctxt, "AttValue length too long");
    } else {
        if (chunkSize > 0)
            xmlSBufAddString(&buf, CUR_PTR - chunkSize, chunkSize);

        if ((normalize) && (inSpace) && (buf.size > 0)) {
            attvalFlags |= XML_ATTVAL_NORM_CHANGE;
            buf.size--;
        }

        ret = xmlSBufFinish(&buf, attlen, ctxt, "AttValue length too long");
        attvalFlags |= XML_ATTVAL_ALLOC;

        if (ret != NULL) {
            if (attlen != NULL)
                *attlen = buf.size;
        }
    }

    if (outFlags != NULL)
        *outFlags = attvalFlags;

    NEXTL(1);

    return(ret);

error:
    xmlSBufCleanup(&buf, ctxt, "AttValue length too long");
    return(NULL);
}

/**
 * Parse a value for an attribute
 * Note: the parser won't do substitution of entities here, this
 * will be handled later in #xmlStringGetNodeList
 *
 * @deprecated Internal function, don't use.
 *
 *     [10] AttValue ::= '"' ([^<&"] | Reference)* '"' |
 *                       "'" ([^<&'] | Reference)* "'"
 *
 * 3.3.3 Attribute-Value Normalization:
 *
 * Before the value of an attribute is passed to the application or
 * checked for validity, the XML processor must normalize it as follows:
 *
 * - a character reference is processed by appending the referenced
 *   character to the attribute value
 * - an entity reference is processed by recursively processing the
 *   replacement text of the entity
 * - a whitespace character (\#x20, \#xD, \#xA, \#x9) is processed by
 *   appending \#x20 to the normalized value, except that only a single
 *   \#x20 is appended for a "#xD#xA" sequence that is part of an external
 *   parsed entity or the literal entity value of an internal parsed entity
 * - other characters are processed by appending them to the normalized value
 *
 * If the declared value is not CDATA, then the XML processor must further
 * process the normalized attribute value by discarding any leading and
 * trailing space (\#x20) characters, and by replacing sequences of space
 * (\#x20) characters by a single space (\#x20) character.
 * All attributes for which no declaration has been read should be treated
 * by a non-validating parser as if declared CDATA.
 *
 * @param ctxt  an XML parser context
 * @returns the AttValue parsed or NULL. The value has to be freed by the
 * caller.
 */
xmlChar *
xmlParseAttValue(xmlParserCtxt *ctxt) {
    if ((ctxt == NULL) || (ctxt->input == NULL)) return(NULL);
    return(xmlParseAttValueInternal(ctxt, NULL, NULL, 0, 0));
}

/**
 * Parse an XML Literal
 *
 * @deprecated Internal function, don't use.
 *
 *     [11] SystemLiteral ::= ('"' [^"]* '"') | ("'" [^']* "'")
 *
 * @param ctxt  an XML parser context
 * @returns the SystemLiteral parsed or NULL
 */

xmlChar *
xmlParseSystemLiteral(xmlParserCtxt *ctxt) {
    xmlChar *buf = NULL;
    int len = 0;
    int size = XML_PARSER_BUFFER_SIZE;
    int cur, l;
    int maxLength = (ctxt->options & XML_PARSE_HUGE) ?
                    XML_MAX_TEXT_LENGTH :
                    XML_MAX_NAME_LENGTH;
    xmlChar stop;

    if (RAW == '"') {
        NEXT;
	stop = '"';
    } else if (RAW == '\'') {
        NEXT;
	stop = '\'';
    } else {
	xmlFatalErr(ctxt, XML_ERR_LITERAL_NOT_STARTED, NULL);
	return(NULL);
    }

    buf = xmlMalloc(size);
    if (buf == NULL) {
        xmlErrMemory(ctxt);
	return(NULL);
    }
    cur = xmlCurrentCharRecover(ctxt, &l);
    while ((IS_CHAR(cur)) && (cur != stop)) { /* checked */
	if (len + 5 >= size) {
	    xmlChar *tmp;
            int newSize;

            newSize = xmlGrowCapacity(size, 1, 1, maxLength);
            if (newSize < 0) {
                xmlFatalErr(ctxt, XML_ERR_NAME_TOO_LONG, "SystemLiteral");
                xmlFree(buf);
                return(NULL);
            }
	    tmp = xmlRealloc(buf, newSize);
	    if (tmp == NULL) {
	        xmlFree(buf);
		xmlErrMemory(ctxt);
		return(NULL);
	    }
	    buf = tmp;
            size = newSize;
	}
	COPY_BUF(buf, len, cur);
	NEXTL(l);
	cur = xmlCurrentCharRecover(ctxt, &l);
    }
    buf[len] = 0;
    if (!IS_CHAR(cur)) {
	xmlFatalErr(ctxt, XML_ERR_LITERAL_NOT_FINISHED, NULL);
    } else {
	NEXT;
    }
    return(buf);
}

/**
 * Parse an XML public literal
 *
 * @deprecated Internal function, don't use.
 *
 *     [12] PubidLiteral ::= '"' PubidChar* '"' | "'" (PubidChar - "'")* "'"
 *
 * @param ctxt  an XML parser context
 * @returns the PubidLiteral parsed or NULL.
 */

xmlChar *
xmlParsePubidLiteral(xmlParserCtxt *ctxt) {
    xmlChar *buf = NULL;
    int len = 0;
    int size = XML_PARSER_BUFFER_SIZE;
    int maxLength = (ctxt->options & XML_PARSE_HUGE) ?
                    XML_MAX_TEXT_LENGTH :
                    XML_MAX_NAME_LENGTH;
    xmlChar cur;
    xmlChar stop;

    if (RAW == '"') {
        NEXT;
	stop = '"';
    } else if (RAW == '\'') {
        NEXT;
	stop = '\'';
    } else {
	xmlFatalErr(ctxt, XML_ERR_LITERAL_NOT_STARTED, NULL);
	return(NULL);
    }
    buf = xmlMalloc(size);
    if (buf == NULL) {
	xmlErrMemory(ctxt);
	return(NULL);
    }
    cur = CUR;
    while ((IS_PUBIDCHAR_CH(cur)) && (cur != stop) &&
           (PARSER_STOPPED(ctxt) == 0)) { /* checked */
	if (len + 1 >= size) {
	    xmlChar *tmp;
            int newSize;

	    newSize = xmlGrowCapacity(size, 1, 1, maxLength);
            if (newSize < 0) {
                xmlFatalErr(ctxt, XML_ERR_NAME_TOO_LONG, "Public ID");
                xmlFree(buf);
                return(NULL);
            }
	    tmp = xmlRealloc(buf, newSize);
	    if (tmp == NULL) {
		xmlErrMemory(ctxt);
		xmlFree(buf);
		return(NULL);
	    }
	    buf = tmp;
            size = newSize;
	}
	buf[len++] = cur;
	NEXT;
	cur = CUR;
    }
    buf[len] = 0;
    if (cur != stop) {
	xmlFatalErr(ctxt, XML_ERR_LITERAL_NOT_FINISHED, NULL);
    } else {
	NEXTL(1);
    }
    return(buf);
}

static void xmlParseCharDataComplex(xmlParserCtxtPtr ctxt, int partial);

/*
 * used for the test in the inner loop of the char data testing
 */
static const unsigned char test_char_data[256] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x09, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* 0x9, CR/LF separated */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x00, 0x27, /* & */
    0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F,
    0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
    0x38, 0x39, 0x3A, 0x3B, 0x00, 0x3D, 0x3E, 0x3F, /* < */
    0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47,
    0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F,
    0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57,
    0x58, 0x59, 0x5A, 0x5B, 0x5C, 0x00, 0x5E, 0x5F, /* ] */
    0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67,
    0x68, 0x69, 0x6A, 0x6B, 0x6C, 0x6D, 0x6E, 0x6F,
    0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77,
    0x78, 0x79, 0x7A, 0x7B, 0x7C, 0x7D, 0x7E, 0x7F,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* non-ascii */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

static void
xmlCharacters(xmlParserCtxtPtr ctxt, const xmlChar *buf, int size,
              int isBlank) {
    int checkBlanks;

    if ((ctxt->sax == NULL) || (ctxt->disableSAX))
        return;

    checkBlanks = (!ctxt->keepBlanks) ||
                  (ctxt->sax->ignorableWhitespace != ctxt->sax->characters);

    /*
     * Calling areBlanks with only parts of a text node
     * is fundamentally broken, making the NOBLANKS option
     * essentially unusable.
     */
    if ((checkBlanks) &&
        (areBlanks(ctxt, buf, size, isBlank))) {
        if ((ctxt->sax->ignorableWhitespace != NULL) &&
            (ctxt->keepBlanks))
            ctxt->sax->ignorableWhitespace(ctxt->userData, buf, size);
    } else {
        if (ctxt->sax->characters != NULL)
            ctxt->sax->characters(ctxt->userData, buf, size);

        /*
         * The old code used to update this value for "complex" data
         * even if checkBlanks was false. This was probably a bug.
         */
        if ((checkBlanks) && (*ctxt->space == -1))
            *ctxt->space = -2;
    }
}

/**
 * Parse character data. Always makes progress if the first char isn't
 * '<' or '&'.
 *
 * The right angle bracket (>) may be represented using the string "&gt;",
 * and must, for compatibility, be escaped using "&gt;" or a character
 * reference when it appears in the string "]]>" in content, when that
 * string is not marking the end of a CDATA section.
 *
 *     [14] CharData ::= [^<&]* - ([^<&]* ']]>' [^<&]*)
 * @param ctxt  an XML parser context
 * @param partial  buffer may contain partial UTF-8 sequences
 */
static void
xmlParseCharDataInternal(xmlParserCtxtPtr ctxt, int partial) {
    const xmlChar *in;
    int line = ctxt->input->line;
    int col = ctxt->input->col;
    int ccol;
    int terminate = 0;

    GROW;
    /*
     * Accelerated common case where input don't need to be
     * modified before passing it to the handler.
     */
    in = ctxt->input->cur;
    do {
get_more_space:
        while (*in == 0x20) { in++; ctxt->input->col++; }
        if (*in == 0xA) {
            do {
                ctxt->input->line++; ctxt->input->col = 1;
                in++;
            } while (*in == 0xA);
            goto get_more_space;
        }
        if (*in == '<') {
            while (in > ctxt->input->cur) {
                const xmlChar *tmp = ctxt->input->cur;
                size_t nbchar = in - tmp;

                if (nbchar > XML_MAX_ITEMS)
                    nbchar = XML_MAX_ITEMS;
                ctxt->input->cur += nbchar;

                xmlCharacters(ctxt, tmp, nbchar, 1);
            }
            return;
        }

get_more:
        ccol = ctxt->input->col;
        while (test_char_data[*in]) {
            in++;
            ccol++;
        }
        ctxt->input->col = ccol;
        if (*in == 0xA) {
            do {
                ctxt->input->line++; ctxt->input->col = 1;
                in++;
            } while (*in == 0xA);
            goto get_more;
        }
        if (*in == ']') {
            size_t avail = ctxt->input->end - in;

            if (partial && avail < 2) {
                terminate = 1;
                goto invoke_callback;
            }
            if (in[1] == ']') {
                if (partial && avail < 3) {
                    terminate = 1;
                    goto invoke_callback;
                }
                if (in[2] == '>')
                    xmlFatalErr(ctxt, XML_ERR_MISPLACED_CDATA_END, NULL);
            }

            in++;
            ctxt->input->col++;
            goto get_more;
        }

invoke_callback:
        while (in > ctxt->input->cur) {
            const xmlChar *tmp = ctxt->input->cur;
            size_t nbchar = in - tmp;

            if (nbchar > XML_MAX_ITEMS)
                nbchar = XML_MAX_ITEMS;
            ctxt->input->cur += nbchar;

            xmlCharacters(ctxt, tmp, nbchar, 0);

            line = ctxt->input->line;
            col = ctxt->input->col;
        }
        ctxt->input->cur = in;
        if (*in == 0xD) {
            in++;
            if (*in == 0xA) {
                ctxt->input->cur = in;
                in++;
                ctxt->input->line++; ctxt->input->col = 1;
                continue; /* while */
            }
            in--;
        }
        if (*in == '<') {
            return;
        }
        if (*in == '&') {
            return;
        }
        if (terminate) {
            return;
        }
        SHRINK;
        GROW;
        in = ctxt->input->cur;
    } while (((*in >= 0x20) && (*in <= 0x7F)) ||
             (*in == 0x09) || (*in == 0x0a));
    ctxt->input->line = line;
    ctxt->input->col = col;
    xmlParseCharDataComplex(ctxt, partial);
}

/**
 * Always makes progress if the first char isn't '<' or '&'.
 *
 * parse a CharData section.this is the fallback function
 * of #xmlParseCharData when the parsing requires handling
 * of non-ASCII characters.
 *
 * @param ctxt  an XML parser context
 * @param partial  whether the input can end with truncated UTF-8
 */
static void
xmlParseCharDataComplex(xmlParserCtxtPtr ctxt, int partial) {
    xmlChar buf[XML_PARSER_BIG_BUFFER_SIZE + 5];
    int nbchar = 0;
    int cur, l;

    cur = xmlCurrentCharRecover(ctxt, &l);
    while ((cur != '<') && /* checked */
           (cur != '&') &&
	   (IS_CHAR(cur))) {
        if (cur == ']') {
            size_t avail = ctxt->input->end - ctxt->input->cur;

            if (partial && avail < 2)
                break;
            if (NXT(1) == ']') {
                if (partial && avail < 3)
                    break;
                if (NXT(2) == '>')
                    xmlFatalErr(ctxt, XML_ERR_MISPLACED_CDATA_END, NULL);
            }
        }

	COPY_BUF(buf, nbchar, cur);
	/* move current position before possible calling of ctxt->sax->characters */
	NEXTL(l);
	if (nbchar >= XML_PARSER_BIG_BUFFER_SIZE) {
	    buf[nbchar] = 0;

            xmlCharacters(ctxt, buf, nbchar, 0);
	    nbchar = 0;
            SHRINK;
	}
	cur = xmlCurrentCharRecover(ctxt, &l);
    }
    if (nbchar != 0) {
        buf[nbchar] = 0;

        xmlCharacters(ctxt, buf, nbchar, 0);
    }
    /*
     * cur == 0 can mean
     *
     * - End of buffer.
     * - An actual 0 character.
     * - An incomplete UTF-8 sequence. This is allowed if partial is set.
     */
    if (ctxt->input->cur < ctxt->input->end) {
        if ((cur == 0) && (CUR != 0)) {
            if (partial == 0) {
                xmlFatalErrMsgInt(ctxt, XML_ERR_INVALID_CHAR,
                        "Incomplete UTF-8 sequence starting with %02X\n", CUR);
                NEXTL(1);
            }
        } else if ((cur != '<') && (cur != '&') && (cur != ']')) {
            /* Generate the error and skip the offending character */
            xmlFatalErrMsgInt(ctxt, XML_ERR_INVALID_CHAR,
                              "PCDATA invalid Char value %d\n", cur);
            NEXTL(l);
        }
    }
}

/**
 * @deprecated Internal function, don't use.
 * @param ctxt  an XML parser context
 * @param cdata  unused
 */
void
xmlParseCharData(xmlParserCtxt *ctxt, ATTRIBUTE_UNUSED int cdata) {
    xmlParseCharDataInternal(ctxt, 0);
}

/**
 * Parse an External ID or a Public ID
 *
 * @deprecated Internal function, don't use.
 *
 * NOTE: Productions [75] and [83] interact badly since [75] can generate
 * `'PUBLIC' S PubidLiteral S SystemLiteral`
 *
 *     [75] ExternalID ::= 'SYSTEM' S SystemLiteral
 *                       | 'PUBLIC' S PubidLiteral S SystemLiteral
 *
 *     [83] PublicID ::= 'PUBLIC' S PubidLiteral
 *
 * @param ctxt  an XML parser context
 * @param publicId  a xmlChar** receiving PubidLiteral
 * @param strict  indicate whether we should restrict parsing to only
 *          production [75], see NOTE below
 * @returns the function returns SystemLiteral and in the second
 *                case publicID receives PubidLiteral, is strict is off
 *                it is possible to return NULL and have publicID set.
 */

xmlChar *
xmlParseExternalID(xmlParserCtxt *ctxt, xmlChar **publicId, int strict) {
    xmlChar *URI = NULL;

    *publicId = NULL;
    if (CMP6(CUR_PTR, 'S', 'Y', 'S', 'T', 'E', 'M')) {
        SKIP(6);
	if (SKIP_BLANKS == 0) {
	    xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED,
	                   "Space required after 'SYSTEM'\n");
	}
	URI = xmlParseSystemLiteral(ctxt);
	if (URI == NULL) {
	    xmlFatalErr(ctxt, XML_ERR_URI_REQUIRED, NULL);
        }
    } else if (CMP6(CUR_PTR, 'P', 'U', 'B', 'L', 'I', 'C')) {
        SKIP(6);
	if (SKIP_BLANKS == 0) {
	    xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED,
		    "Space required after 'PUBLIC'\n");
	}
	*publicId = xmlParsePubidLiteral(ctxt);
	if (*publicId == NULL) {
	    xmlFatalErr(ctxt, XML_ERR_PUBID_REQUIRED, NULL);
	}
	if (strict) {
	    /*
	     * We don't handle [83] so "S SystemLiteral" is required.
	     */
	    if (SKIP_BLANKS == 0) {
		xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED,
			"Space required after the Public Identifier\n");
	    }
	} else {
	    /*
	     * We handle [83] so we return immediately, if
	     * "S SystemLiteral" is not detected. We skip blanks if no
             * system literal was found, but this is harmless since we must
             * be at the end of a NotationDecl.
	     */
	    if (SKIP_BLANKS == 0) return(NULL);
	    if ((CUR != '\'') && (CUR != '"')) return(NULL);
	}
	URI = xmlParseSystemLiteral(ctxt);
	if (URI == NULL) {
	    xmlFatalErr(ctxt, XML_ERR_URI_REQUIRED, NULL);
        }
    }
    return(URI);
}

/**
 * Skip an XML (SGML) comment <!-- .... -->
 *  The spec says that "For compatibility, the string "--" (double-hyphen)
 *  must not occur within comments. "
 * This is the slow routine in case the accelerator for ascii didn't work
 *
 *     [15] Comment ::= '<!--' ((Char - '-') | ('-' (Char - '-')))* '-->'
 * @param ctxt  an XML parser context
 * @param buf  the already parsed part of the buffer
 * @param len  number of bytes in the buffer
 * @param size  allocated size of the buffer
 */
static void
xmlParseCommentComplex(xmlParserCtxtPtr ctxt, xmlChar *buf,
                       size_t len, size_t size) {
    int q, ql;
    int r, rl;
    int cur, l;
    int maxLength = (ctxt->options & XML_PARSE_HUGE) ?
                    XML_MAX_HUGE_LENGTH :
                    XML_MAX_TEXT_LENGTH;

    if (buf == NULL) {
        len = 0;
	size = XML_PARSER_BUFFER_SIZE;
	buf = xmlMalloc(size);
	if (buf == NULL) {
	    xmlErrMemory(ctxt);
	    return;
	}
    }
    q = xmlCurrentCharRecover(ctxt, &ql);
    if (q == 0)
        goto not_terminated;
    if (!IS_CHAR(q)) {
        xmlFatalErrMsgInt(ctxt, XML_ERR_INVALID_CHAR,
                          "xmlParseComment: invalid xmlChar value %d\n",
	                  q);
	xmlFree (buf);
	return;
    }
    NEXTL(ql);
    r = xmlCurrentCharRecover(ctxt, &rl);
    if (r == 0)
        goto not_terminated;
    if (!IS_CHAR(r)) {
        xmlFatalErrMsgInt(ctxt, XML_ERR_INVALID_CHAR,
                          "xmlParseComment: invalid xmlChar value %d\n",
	                  r);
	xmlFree (buf);
	return;
    }
    NEXTL(rl);
    cur = xmlCurrentCharRecover(ctxt, &l);
    if (cur == 0)
        goto not_terminated;
    while (IS_CHAR(cur) && /* checked */
           ((cur != '>') ||
	    (r != '-') || (q != '-'))) {
	if ((r == '-') && (q == '-')) {
	    xmlFatalErr(ctxt, XML_ERR_HYPHEN_IN_COMMENT, NULL);
	}
	if (len + 5 >= size) {
	    xmlChar *tmp;
            int newSize;

	    newSize = xmlGrowCapacity(size, 1, 1, maxLength);
            if (newSize < 0) {
                xmlFatalErrMsgStr(ctxt, XML_ERR_COMMENT_NOT_FINISHED,
                             "Comment too big found", NULL);
                xmlFree (buf);
                return;
            }
	    tmp = xmlRealloc(buf, newSize);
	    if (tmp == NULL) {
		xmlErrMemory(ctxt);
		xmlFree(buf);
		return;
	    }
	    buf = tmp;
            size = newSize;
	}
	COPY_BUF(buf, len, q);

	q = r;
	ql = rl;
	r = cur;
	rl = l;

	NEXTL(l);
	cur = xmlCurrentCharRecover(ctxt, &l);

    }
    buf[len] = 0;
    if (cur == 0) {
	xmlFatalErrMsgStr(ctxt, XML_ERR_COMMENT_NOT_FINISHED,
	                     "Comment not terminated \n<!--%.50s\n", buf);
    } else if (!IS_CHAR(cur)) {
        xmlFatalErrMsgInt(ctxt, XML_ERR_INVALID_CHAR,
                          "xmlParseComment: invalid xmlChar value %d\n",
	                  cur);
    } else {
        NEXT;
	if ((ctxt->sax != NULL) && (ctxt->sax->comment != NULL) &&
	    (!ctxt->disableSAX))
	    ctxt->sax->comment(ctxt->userData, buf);
    }
    xmlFree(buf);
    return;
not_terminated:
    xmlFatalErrMsgStr(ctxt, XML_ERR_COMMENT_NOT_FINISHED,
			 "Comment not terminated\n", NULL);
    xmlFree(buf);
}

/**
 * Parse an XML (SGML) comment. Always consumes '<!'.
 *
 * @deprecated Internal function, don't use.
 *
 *  The spec says that "For compatibility, the string "--" (double-hyphen)
 *  must not occur within comments. "
 *
 *     [15] Comment ::= '<!--' ((Char - '-') | ('-' (Char - '-')))* '-->'
 * @param ctxt  an XML parser context
 */
void
xmlParseComment(xmlParserCtxt *ctxt) {
    xmlChar *buf = NULL;
    size_t size = XML_PARSER_BUFFER_SIZE;
    size_t len = 0;
    size_t maxLength = (ctxt->options & XML_PARSE_HUGE) ?
                       XML_MAX_HUGE_LENGTH :
                       XML_MAX_TEXT_LENGTH;
    const xmlChar *in;
    size_t nbchar = 0;
    int ccol;

    /*
     * Check that there is a comment right here.
     */
    if ((RAW != '<') || (NXT(1) != '!'))
        return;
    SKIP(2);
    if ((RAW != '-') || (NXT(1) != '-'))
        return;
    SKIP(2);
    GROW;

    /*
     * Accelerated common case where input don't need to be
     * modified before passing it to the handler.
     */
    in = ctxt->input->cur;
    do {
	if (*in == 0xA) {
	    do {
		ctxt->input->line++; ctxt->input->col = 1;
		in++;
	    } while (*in == 0xA);
	}
get_more:
        ccol = ctxt->input->col;
	while (((*in > '-') && (*in <= 0x7F)) ||
	       ((*in >= 0x20) && (*in < '-')) ||
	       (*in == 0x09)) {
		    in++;
		    ccol++;
	}
	ctxt->input->col = ccol;
	if (*in == 0xA) {
	    do {
		ctxt->input->line++; ctxt->input->col = 1;
		in++;
	    } while (*in == 0xA);
	    goto get_more;
	}
	nbchar = in - ctxt->input->cur;
	/*
	 * save current set of data
	 */
	if (nbchar > 0) {
            if (nbchar > maxLength - len) {
                xmlFatalErrMsgStr(ctxt, XML_ERR_COMMENT_NOT_FINISHED,
                                  "Comment too big found", NULL);
                xmlFree(buf);
                return;
            }
            if (buf == NULL) {
                if ((*in == '-') && (in[1] == '-'))
                    size = nbchar + 1;
                else
                    size = XML_PARSER_BUFFER_SIZE + nbchar;
                buf = xmlMalloc(size);
                if (buf == NULL) {
                    xmlErrMemory(ctxt);
                    return;
                }
                len = 0;
            } else if (len + nbchar + 1 >= size) {
                xmlChar *new_buf;
                size += len + nbchar + XML_PARSER_BUFFER_SIZE;
                new_buf = xmlRealloc(buf, size);
                if (new_buf == NULL) {
                    xmlErrMemory(ctxt);
                    xmlFree(buf);
                    return;
                }
                buf = new_buf;
            }
            memcpy(&buf[len], ctxt->input->cur, nbchar);
            len += nbchar;
            buf[len] = 0;
	}
	ctxt->input->cur = in;
	if (*in == 0xA) {
	    in++;
	    ctxt->input->line++; ctxt->input->col = 1;
	}
	if (*in == 0xD) {
	    in++;
	    if (*in == 0xA) {
		ctxt->input->cur = in;
		in++;
		ctxt->input->line++; ctxt->input->col = 1;
		goto get_more;
	    }
	    in--;
	}
	SHRINK;
	GROW;
	in = ctxt->input->cur;
	if (*in == '-') {
	    if (in[1] == '-') {
	        if (in[2] == '>') {
		    SKIP(3);
		    if ((ctxt->sax != NULL) && (ctxt->sax->comment != NULL) &&
		        (!ctxt->disableSAX)) {
			if (buf != NULL)
			    ctxt->sax->comment(ctxt->userData, buf);
			else
			    ctxt->sax->comment(ctxt->userData, BAD_CAST "");
		    }
		    if (buf != NULL)
		        xmlFree(buf);
		    return;
		}
		if (buf != NULL) {
		    xmlFatalErrMsgStr(ctxt, XML_ERR_HYPHEN_IN_COMMENT,
		                      "Double hyphen within comment: "
                                      "<!--%.50s\n",
				      buf);
		} else
		    xmlFatalErrMsgStr(ctxt, XML_ERR_HYPHEN_IN_COMMENT,
		                      "Double hyphen within comment\n", NULL);
		in++;
		ctxt->input->col++;
	    }
	    in++;
	    ctxt->input->col++;
	    goto get_more;
	}
    } while (((*in >= 0x20) && (*in <= 0x7F)) || (*in == 0x09) || (*in == 0x0a));
    xmlParseCommentComplex(ctxt, buf, len, size);
}


/**
 * Parse the name of a PI
 *
 * @deprecated Internal function, don't use.
 *
 *     [17] PITarget ::= Name - (('X' | 'x') ('M' | 'm') ('L' | 'l'))
 *
 * @param ctxt  an XML parser context
 * @returns the PITarget name or NULL
 */

const xmlChar *
xmlParsePITarget(xmlParserCtxt *ctxt) {
    const xmlChar *name;

    name = xmlParseName(ctxt);
    if ((name != NULL) &&
        ((name[0] == 'x') || (name[0] == 'X')) &&
        ((name[1] == 'm') || (name[1] == 'M')) &&
        ((name[2] == 'l') || (name[2] == 'L'))) {
	int i;
	if ((name[0] == 'x') && (name[1] == 'm') &&
	    (name[2] == 'l') && (name[3] == 0)) {
	    xmlFatalErrMsg(ctxt, XML_ERR_RESERVED_XML_NAME,
		 "XML declaration allowed only at the start of the document\n");
	    return(name);
	} else if (name[3] == 0) {
	    xmlFatalErr(ctxt, XML_ERR_RESERVED_XML_NAME, NULL);
	    return(name);
	}
	for (i = 0;;i++) {
	    if (xmlW3CPIs[i] == NULL) break;
	    if (xmlStrEqual(name, (const xmlChar *)xmlW3CPIs[i]))
	        return(name);
	}
	xmlWarningMsg(ctxt, XML_ERR_RESERVED_XML_NAME,
		      "xmlParsePITarget: invalid name prefix 'xml'\n",
		      NULL, NULL);
    }
    if ((name != NULL) && (xmlStrchr(name, ':') != NULL)) {
	xmlNsErr(ctxt, XML_NS_ERR_COLON,
		 "colons are forbidden from PI names '%s'\n", name, NULL, NULL);
    }
    return(name);
}

#ifdef LIBXML_CATALOG_ENABLED
/**
 * Parse an XML Catalog Processing Instruction.
 *
 * <?oasis-xml-catalog catalog="http://example.com/catalog.xml"?>
 *
 * Occurs only if allowed by the user and if happening in the Misc
 * part of the document before any doctype information
 * This will add the given catalog to the parsing context in order
 * to be used if there is a resolution need further down in the document
 *
 * @param ctxt  an XML parser context
 * @param catalog  the PI value string
 */

static void
xmlParseCatalogPI(xmlParserCtxtPtr ctxt, const xmlChar *catalog) {
    xmlChar *URL = NULL;
    const xmlChar *tmp, *base;
    xmlChar marker;

    tmp = catalog;
    while (IS_BLANK_CH(*tmp)) tmp++;
    if (xmlStrncmp(tmp, BAD_CAST"catalog", 7))
	goto error;
    tmp += 7;
    while (IS_BLANK_CH(*tmp)) tmp++;
    if (*tmp != '=') {
	return;
    }
    tmp++;
    while (IS_BLANK_CH(*tmp)) tmp++;
    marker = *tmp;
    if ((marker != '\'') && (marker != '"'))
	goto error;
    tmp++;
    base = tmp;
    while ((*tmp != 0) && (*tmp != marker)) tmp++;
    if (*tmp == 0)
	goto error;
    URL = xmlStrndup(base, tmp - base);
    tmp++;
    while (IS_BLANK_CH(*tmp)) tmp++;
    if (*tmp != 0)
	goto error;

    if (URL != NULL) {
        /*
         * Unfortunately, the catalog API doesn't report OOM errors.
         * xmlGetLastError isn't very helpful since we don't know
         * where the last error came from. We'd have to reset it
         * before this call and restore it afterwards.
         */
	ctxt->catalogs = xmlCatalogAddLocal(ctxt->catalogs, URL);
	xmlFree(URL);
    }
    return;

error:
    xmlWarningMsg(ctxt, XML_WAR_CATALOG_PI,
	          "Catalog PI syntax error: %s\n",
		  catalog, NULL);
    if (URL != NULL)
	xmlFree(URL);
}
#endif

/**
 * Parse an XML Processing Instruction.
 *
 * @deprecated Internal function, don't use.
 *
 *     [16] PI ::= '<?' PITarget (S (Char* - (Char* '?>' Char*)))? '?>'
 *
 * The processing is transferred to SAX once parsed.
 *
 * @param ctxt  an XML parser context
 */

void
xmlParsePI(xmlParserCtxt *ctxt) {
    xmlChar *buf = NULL;
    size_t len = 0;
    size_t size = XML_PARSER_BUFFER_SIZE;
    size_t maxLength = (ctxt->options & XML_PARSE_HUGE) ?
                       XML_MAX_HUGE_LENGTH :
                       XML_MAX_TEXT_LENGTH;
    int cur, l;
    const xmlChar *target;

    if ((RAW == '<') && (NXT(1) == '?')) {
	/*
	 * this is a Processing Instruction.
	 */
	SKIP(2);

	/*
	 * Parse the target name and check for special support like
	 * namespace.
	 */
        target = xmlParsePITarget(ctxt);
	if (target != NULL) {
	    if ((RAW == '?') && (NXT(1) == '>')) {
		SKIP(2);

		/*
		 * SAX: PI detected.
		 */
		if ((ctxt->sax) && (!ctxt->disableSAX) &&
		    (ctxt->sax->processingInstruction != NULL))
		    ctxt->sax->processingInstruction(ctxt->userData,
		                                     target, NULL);
		return;
	    }
	    buf = xmlMalloc(size);
	    if (buf == NULL) {
		xmlErrMemory(ctxt);
		return;
	    }
	    if (SKIP_BLANKS == 0) {
		xmlFatalErrMsgStr(ctxt, XML_ERR_SPACE_REQUIRED,
			  "ParsePI: PI %s space expected\n", target);
	    }
	    cur = xmlCurrentCharRecover(ctxt, &l);
	    while (IS_CHAR(cur) && /* checked */
		   ((cur != '?') || (NXT(1) != '>'))) {
		if (len + 5 >= size) {
		    xmlChar *tmp;
                    int newSize;

                    newSize = xmlGrowCapacity(size, 1, 1, maxLength);
                    if (newSize < 0) {
                        xmlFatalErrMsgStr(ctxt, XML_ERR_PI_NOT_FINISHED,
                                          "PI %s too big found", target);
                        xmlFree(buf);
                        return;
                    }
		    tmp = xmlRealloc(buf, newSize);
		    if (tmp == NULL) {
			xmlErrMemory(ctxt);
			xmlFree(buf);
			return;
		    }
		    buf = tmp;
                    size = newSize;
		}
		COPY_BUF(buf, len, cur);
		NEXTL(l);
		cur = xmlCurrentCharRecover(ctxt, &l);
	    }
	    buf[len] = 0;
	    if (cur != '?') {
		xmlFatalErrMsgStr(ctxt, XML_ERR_PI_NOT_FINISHED,
		      "ParsePI: PI %s never end ...\n", target);
	    } else {
		SKIP(2);

#ifdef LIBXML_CATALOG_ENABLED
		if ((ctxt->inSubset == 0) &&
		    (xmlStrEqual(target, XML_CATALOG_PI))) {
		    xmlCatalogAllow allow = xmlCatalogGetDefaults();

		    if ((ctxt->options & XML_PARSE_CATALOG_PI) &&
                        ((allow == XML_CATA_ALLOW_DOCUMENT) ||
			 (allow == XML_CATA_ALLOW_ALL)))
			xmlParseCatalogPI(ctxt, buf);
		}
#endif

		/*
		 * SAX: PI detected.
		 */
		if ((ctxt->sax) && (!ctxt->disableSAX) &&
		    (ctxt->sax->processingInstruction != NULL))
		    ctxt->sax->processingInstruction(ctxt->userData,
		                                     target, buf);
	    }
	    xmlFree(buf);
	} else {
	    xmlFatalErr(ctxt, XML_ERR_PI_NOT_STARTED, NULL);
	}
    }
}

/**
 * Parse a notation declaration. Always consumes '<!'.
 *
 * @deprecated Internal function, don't use.
 *
 *     [82] NotationDecl ::= '<!NOTATION' S Name S (ExternalID |  PublicID)
 *                           S? '>'
 *
 * Hence there is actually 3 choices:
 *
 *     'PUBLIC' S PubidLiteral
 *     'PUBLIC' S PubidLiteral S SystemLiteral
 *     'SYSTEM' S SystemLiteral
 *
 * See the NOTE on #xmlParseExternalID.
 *
 * @param ctxt  an XML parser context
 */

void
xmlParseNotationDecl(xmlParserCtxt *ctxt) {
    const xmlChar *name;
    xmlChar *Pubid;
    xmlChar *Systemid;

    if ((CUR != '<') || (NXT(1) != '!'))
        return;
    SKIP(2);

    if (CMP8(CUR_PTR, 'N', 'O', 'T', 'A', 'T', 'I', 'O', 'N')) {
#ifdef LIBXML_VALID_ENABLED
	int oldInputNr = ctxt->inputNr;
#endif

	SKIP(8);
	if (SKIP_BLANKS_PE == 0) {
	    xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED,
			   "Space required after '<!NOTATION'\n");
	    return;
	}

        name = xmlParseName(ctxt);
	if (name == NULL) {
	    xmlFatalErr(ctxt, XML_ERR_NOTATION_NOT_STARTED, NULL);
	    return;
	}
	if (xmlStrchr(name, ':') != NULL) {
	    xmlNsErr(ctxt, XML_NS_ERR_COLON,
		     "colons are forbidden from notation names '%s'\n",
		     name, NULL, NULL);
	}
	if (SKIP_BLANKS_PE == 0) {
	    xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED,
		     "Space required after the NOTATION name'\n");
	    return;
	}

	/*
	 * Parse the IDs.
	 */
	Systemid = xmlParseExternalID(ctxt, &Pubid, 0);
	SKIP_BLANKS_PE;

	if (RAW == '>') {
#ifdef LIBXML_VALID_ENABLED
	    if ((ctxt->validate) && (ctxt->inputNr > oldInputNr)) {
		xmlValidityError(ctxt, XML_ERR_ENTITY_BOUNDARY,
	                         "Notation declaration doesn't start and stop"
                                 " in the same entity\n",
                                 NULL, NULL);
	    }
#endif
	    NEXT;
	    if ((ctxt->sax != NULL) && (!ctxt->disableSAX) &&
		(ctxt->sax->notationDecl != NULL))
		ctxt->sax->notationDecl(ctxt->userData, name, Pubid, Systemid);
	} else {
	    xmlFatalErr(ctxt, XML_ERR_NOTATION_NOT_FINISHED, NULL);
	}
	if (Systemid != NULL) xmlFree(Systemid);
	if (Pubid != NULL) xmlFree(Pubid);
    }
}

/**
 * Parse an entity declaration. Always consumes '<!'.
 *
 * @deprecated Internal function, don't use.
 *
 *     [70] EntityDecl ::= GEDecl | PEDecl
 *
 *     [71] GEDecl ::= '<!ENTITY' S Name S EntityDef S? '>'
 *
 *     [72] PEDecl ::= '<!ENTITY' S '%' S Name S PEDef S? '>'
 *
 *     [73] EntityDef ::= EntityValue | (ExternalID NDataDecl?)
 *
 *     [74] PEDef ::= EntityValue | ExternalID
 *
 *     [76] NDataDecl ::= S 'NDATA' S Name
 *
 * [ VC: Notation Declared ]
 * The Name must match the declared name of a notation.
 *
 * @param ctxt  an XML parser context
 */

void
xmlParseEntityDecl(xmlParserCtxt *ctxt) {
    const xmlChar *name = NULL;
    xmlChar *value = NULL;
    xmlChar *URI = NULL, *literal = NULL;
    const xmlChar *ndata = NULL;
    int isParameter = 0;
    xmlChar *orig = NULL;

    if ((CUR != '<') || (NXT(1) != '!'))
        return;
    SKIP(2);

    /* GROW; done in the caller */
    if (CMP6(CUR_PTR, 'E', 'N', 'T', 'I', 'T', 'Y')) {
#ifdef LIBXML_VALID_ENABLED
	int oldInputNr = ctxt->inputNr;
#endif

	SKIP(6);
	if (SKIP_BLANKS_PE == 0) {
	    xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED,
			   "Space required after '<!ENTITY'\n");
	}

	if (RAW == '%') {
	    NEXT;
	    if (SKIP_BLANKS_PE == 0) {
		xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED,
			       "Space required after '%%'\n");
	    }
	    isParameter = 1;
	}

        name = xmlParseName(ctxt);
	if (name == NULL) {
	    xmlFatalErrMsg(ctxt, XML_ERR_NAME_REQUIRED,
	                   "xmlParseEntityDecl: no name\n");
            return;
	}
	if (xmlStrchr(name, ':') != NULL) {
	    xmlNsErr(ctxt, XML_NS_ERR_COLON,
		     "colons are forbidden from entities names '%s'\n",
		     name, NULL, NULL);
	}
	if (SKIP_BLANKS_PE == 0) {
	    xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED,
			   "Space required after the entity name\n");
	}

	/*
	 * handle the various case of definitions...
	 */
	if (isParameter) {
	    if ((RAW == '"') || (RAW == '\'')) {
	        value = xmlParseEntityValue(ctxt, &orig);
		if (value) {
		    if ((ctxt->sax != NULL) &&
			(!ctxt->disableSAX) && (ctxt->sax->entityDecl != NULL))
			ctxt->sax->entityDecl(ctxt->userData, name,
		                    XML_INTERNAL_PARAMETER_ENTITY,
				    NULL, NULL, value);
		}
	    } else {
	        URI = xmlParseExternalID(ctxt, &literal, 1);
		if ((URI == NULL) && (literal == NULL)) {
		    xmlFatalErr(ctxt, XML_ERR_VALUE_REQUIRED, NULL);
		}
		if (URI) {
                    if (xmlStrchr(URI, '#')) {
                        xmlFatalErr(ctxt, XML_ERR_URI_FRAGMENT, NULL);
                    } else {
                        if ((ctxt->sax != NULL) &&
                            (!ctxt->disableSAX) &&
                            (ctxt->sax->entityDecl != NULL))
                            ctxt->sax->entityDecl(ctxt->userData, name,
                                        XML_EXTERNAL_PARAMETER_ENTITY,
                                        literal, URI, NULL);
                    }
		}
	    }
	} else {
	    if ((RAW == '"') || (RAW == '\'')) {
	        value = xmlParseEntityValue(ctxt, &orig);
		if ((ctxt->sax != NULL) &&
		    (!ctxt->disableSAX) && (ctxt->sax->entityDecl != NULL))
		    ctxt->sax->entityDecl(ctxt->userData, name,
				XML_INTERNAL_GENERAL_ENTITY,
				NULL, NULL, value);
		/*
		 * For expat compatibility in SAX mode.
		 */
		if ((ctxt->myDoc == NULL) ||
		    (xmlStrEqual(ctxt->myDoc->version, SAX_COMPAT_MODE))) {
		    if (ctxt->myDoc == NULL) {
			ctxt->myDoc = xmlNewDoc(SAX_COMPAT_MODE);
			if (ctxt->myDoc == NULL) {
			    xmlErrMemory(ctxt);
			    goto done;
			}
			ctxt->myDoc->properties = XML_DOC_INTERNAL;
		    }
		    if (ctxt->myDoc->intSubset == NULL) {
			ctxt->myDoc->intSubset = xmlNewDtd(ctxt->myDoc,
					    BAD_CAST "fake", NULL, NULL);
                        if (ctxt->myDoc->intSubset == NULL) {
                            xmlErrMemory(ctxt);
                            goto done;
                        }
                    }

		    xmlSAX2EntityDecl(ctxt, name, XML_INTERNAL_GENERAL_ENTITY,
			              NULL, NULL, value);
		}
	    } else {
	        URI = xmlParseExternalID(ctxt, &literal, 1);
		if ((URI == NULL) && (literal == NULL)) {
		    xmlFatalErr(ctxt, XML_ERR_VALUE_REQUIRED, NULL);
		}
		if (URI) {
                    if (xmlStrchr(URI, '#')) {
                        xmlFatalErr(ctxt, XML_ERR_URI_FRAGMENT, NULL);
                    }
		}
		if ((RAW != '>') && (SKIP_BLANKS_PE == 0)) {
		    xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED,
				   "Space required before 'NDATA'\n");
		}
		if (CMP5(CUR_PTR, 'N', 'D', 'A', 'T', 'A')) {
		    SKIP(5);
		    if (SKIP_BLANKS_PE == 0) {
			xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED,
				       "Space required after 'NDATA'\n");
		    }
		    ndata = xmlParseName(ctxt);
		    if ((ctxt->sax != NULL) && (!ctxt->disableSAX) &&
		        (ctxt->sax->unparsedEntityDecl != NULL))
			ctxt->sax->unparsedEntityDecl(ctxt->userData, name,
				    literal, URI, ndata);
		} else {
		    if ((ctxt->sax != NULL) &&
		        (!ctxt->disableSAX) && (ctxt->sax->entityDecl != NULL))
			ctxt->sax->entityDecl(ctxt->userData, name,
				    XML_EXTERNAL_GENERAL_PARSED_ENTITY,
				    literal, URI, NULL);
		    /*
		     * For expat compatibility in SAX mode.
		     * assuming the entity replacement was asked for
		     */
		    if ((ctxt->replaceEntities != 0) &&
			((ctxt->myDoc == NULL) ||
			(xmlStrEqual(ctxt->myDoc->version, SAX_COMPAT_MODE)))) {
			if (ctxt->myDoc == NULL) {
			    ctxt->myDoc = xmlNewDoc(SAX_COMPAT_MODE);
			    if (ctxt->myDoc == NULL) {
			        xmlErrMemory(ctxt);
				goto done;
			    }
			    ctxt->myDoc->properties = XML_DOC_INTERNAL;
			}

			if (ctxt->myDoc->intSubset == NULL) {
			    ctxt->myDoc->intSubset = xmlNewDtd(ctxt->myDoc,
						BAD_CAST "fake", NULL, NULL);
                            if (ctxt->myDoc->intSubset == NULL) {
                                xmlErrMemory(ctxt);
                                goto done;
                            }
                        }
			xmlSAX2EntityDecl(ctxt, name,
				          XML_EXTERNAL_GENERAL_PARSED_ENTITY,
				          literal, URI, NULL);
		    }
		}
	    }
	}
	SKIP_BLANKS_PE;
	if (RAW != '>') {
	    xmlFatalErrMsgStr(ctxt, XML_ERR_ENTITY_NOT_FINISHED,
	            "xmlParseEntityDecl: entity %s not terminated\n", name);
	} else {
#ifdef LIBXML_VALID_ENABLED
	    if ((ctxt->validate) && (ctxt->inputNr > oldInputNr)) {
		xmlValidityError(ctxt, XML_ERR_ENTITY_BOUNDARY,
	                         "Entity declaration doesn't start and stop in"
                                 " the same entity\n",
                                 NULL, NULL);
	    }
#endif
	    NEXT;
	}
	if (orig != NULL) {
	    /*
	     * Ugly mechanism to save the raw entity value.
	     */
	    xmlEntityPtr cur = NULL;

	    if (isParameter) {
	        if ((ctxt->sax != NULL) &&
		    (ctxt->sax->getParameterEntity != NULL))
		    cur = ctxt->sax->getParameterEntity(ctxt->userData, name);
	    } else {
	        if ((ctxt->sax != NULL) &&
		    (ctxt->sax->getEntity != NULL))
		    cur = ctxt->sax->getEntity(ctxt->userData, name);
		if ((cur == NULL) && (ctxt->userData==ctxt)) {
		    cur = xmlSAX2GetEntity(ctxt, name);
		}
	    }
            if ((cur != NULL) && (cur->orig == NULL)) {
		cur->orig = orig;
                orig = NULL;
	    }
	}

done:
	if (value != NULL) xmlFree(value);
	if (URI != NULL) xmlFree(URI);
	if (literal != NULL) xmlFree(literal);
        if (orig != NULL) xmlFree(orig);
    }
}

/**
 * Parse an attribute default declaration
 *
 * @deprecated Internal function, don't use.
 *
 *     [60] DefaultDecl ::= '#REQUIRED' | '#IMPLIED' | (('#FIXED' S)? AttValue)
 *
 * [ VC: Required Attribute ]
 * if the default declaration is the keyword \#REQUIRED, then the
 * attribute must be specified for all elements of the type in the
 * attribute-list declaration.
 *
 * [ VC: Attribute Default Legal ]
 * The declared default value must meet the lexical constraints of
 * the declared attribute type c.f. #xmlValidateAttributeDecl
 *
 * [ VC: Fixed Attribute Default ]
 * if an attribute has a default value declared with the \#FIXED
 * keyword, instances of that attribute must match the default value.
 *
 * [ WFC: No < in Attribute Values ]
 * handled in #xmlParseAttValue
 *
 * @param ctxt  an XML parser context
 * @param value  Receive a possible fixed default value for the attribute
 * @returns XML_ATTRIBUTE_NONE, XML_ATTRIBUTE_REQUIRED, XML_ATTRIBUTE_IMPLIED
 *          or XML_ATTRIBUTE_FIXED.
 */

int
xmlParseDefaultDecl(xmlParserCtxt *ctxt, xmlChar **value) {
    int val;
    xmlChar *ret;

    *value = NULL;
    if (CMP9(CUR_PTR, '#', 'R', 'E', 'Q', 'U', 'I', 'R', 'E', 'D')) {
	SKIP(9);
	return(XML_ATTRIBUTE_REQUIRED);
    }
    if (CMP8(CUR_PTR, '#', 'I', 'M', 'P', 'L', 'I', 'E', 'D')) {
	SKIP(8);
	return(XML_ATTRIBUTE_IMPLIED);
    }
    val = XML_ATTRIBUTE_NONE;
    if (CMP6(CUR_PTR, '#', 'F', 'I', 'X', 'E', 'D')) {
	SKIP(6);
	val = XML_ATTRIBUTE_FIXED;
	if (SKIP_BLANKS_PE == 0) {
	    xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED,
			   "Space required after '#FIXED'\n");
	}
    }
    ret = xmlParseAttValue(ctxt);
    if (ret == NULL) {
	xmlFatalErrMsg(ctxt, (xmlParserErrors)ctxt->errNo,
		       "Attribute default value declaration error\n");
    } else
        *value = ret;
    return(val);
}

/**
 * Parse an Notation attribute type.
 *
 * @deprecated Internal function, don't use.
 *
 * Note: the leading 'NOTATION' S part has already being parsed...
 *
 *     [58] NotationType ::= 'NOTATION' S '(' S? Name (S? '|' S? Name)* S? ')'
 *
 * [ VC: Notation Attributes ]
 * Values of this type must match one of the notation names included
 * in the declaration; all notation names in the declaration must be declared.
 *
 * @param ctxt  an XML parser context
 * @returns the notation attribute tree built while parsing
 */

xmlEnumeration *
xmlParseNotationType(xmlParserCtxt *ctxt) {
    const xmlChar *name;
    xmlEnumerationPtr ret = NULL, last = NULL, cur, tmp;

    if (RAW != '(') {
	xmlFatalErr(ctxt, XML_ERR_NOTATION_NOT_STARTED, NULL);
	return(NULL);
    }
    do {
        NEXT;
	SKIP_BLANKS_PE;
        name = xmlParseName(ctxt);
	if (name == NULL) {
	    xmlFatalErrMsg(ctxt, XML_ERR_NAME_REQUIRED,
			   "Name expected in NOTATION declaration\n");
            xmlFreeEnumeration(ret);
	    return(NULL);
	}
        tmp = NULL;
#ifdef LIBXML_VALID_ENABLED
        if (ctxt->validate) {
            tmp = ret;
            while (tmp != NULL) {
                if (xmlStrEqual(name, tmp->name)) {
                    xmlValidityError(ctxt, XML_DTD_DUP_TOKEN,
              "standalone: attribute notation value token %s duplicated\n",
                                     name, NULL);
                    if (!xmlDictOwns(ctxt->dict, name))
                        xmlFree((xmlChar *) name);
                    break;
                }
                tmp = tmp->next;
            }
        }
#endif /* LIBXML_VALID_ENABLED */
	if (tmp == NULL) {
	    cur = xmlCreateEnumeration(name);
	    if (cur == NULL) {
                xmlErrMemory(ctxt);
                xmlFreeEnumeration(ret);
                return(NULL);
            }
	    if (last == NULL) ret = last = cur;
	    else {
		last->next = cur;
		last = cur;
	    }
	}
	SKIP_BLANKS_PE;
    } while (RAW == '|');
    if (RAW != ')') {
	xmlFatalErr(ctxt, XML_ERR_NOTATION_NOT_FINISHED, NULL);
        xmlFreeEnumeration(ret);
	return(NULL);
    }
    NEXT;
    return(ret);
}

/**
 * Parse an Enumeration attribute type.
 *
 * @deprecated Internal function, don't use.
 *
 *     [59] Enumeration ::= '(' S? Nmtoken (S? '|' S? Nmtoken)* S? ')'
 *
 * [ VC: Enumeration ]
 * Values of this type must match one of the Nmtoken tokens in
 * the declaration
 *
 * @param ctxt  an XML parser context
 * @returns the enumeration attribute tree built while parsing
 */

xmlEnumeration *
xmlParseEnumerationType(xmlParserCtxt *ctxt) {
    xmlChar *name;
    xmlEnumerationPtr ret = NULL, last = NULL, cur, tmp;

    if (RAW != '(') {
	xmlFatalErr(ctxt, XML_ERR_ATTLIST_NOT_STARTED, NULL);
	return(NULL);
    }
    do {
        NEXT;
	SKIP_BLANKS_PE;
        name = xmlParseNmtoken(ctxt);
	if (name == NULL) {
	    xmlFatalErr(ctxt, XML_ERR_NMTOKEN_REQUIRED, NULL);
	    return(ret);
	}
        tmp = NULL;
#ifdef LIBXML_VALID_ENABLED
        if (ctxt->validate) {
            tmp = ret;
            while (tmp != NULL) {
                if (xmlStrEqual(name, tmp->name)) {
                    xmlValidityError(ctxt, XML_DTD_DUP_TOKEN,
              "standalone: attribute enumeration value token %s duplicated\n",
                                     name, NULL);
                    if (!xmlDictOwns(ctxt->dict, name))
                        xmlFree(name);
                    break;
                }
                tmp = tmp->next;
            }
        }
#endif /* LIBXML_VALID_ENABLED */
	if (tmp == NULL) {
	    cur = xmlCreateEnumeration(name);
	    if (!xmlDictOwns(ctxt->dict, name))
		xmlFree(name);
	    if (cur == NULL) {
                xmlErrMemory(ctxt);
                xmlFreeEnumeration(ret);
                return(NULL);
            }
	    if (last == NULL) ret = last = cur;
	    else {
		last->next = cur;
		last = cur;
	    }
	}
	SKIP_BLANKS_PE;
    } while (RAW == '|');
    if (RAW != ')') {
	xmlFatalErr(ctxt, XML_ERR_ATTLIST_NOT_FINISHED, NULL);
	return(ret);
    }
    NEXT;
    return(ret);
}

/**
 * Parse an Enumerated attribute type.
 *
 * @deprecated Internal function, don't use.
 *
 *     [57] EnumeratedType ::= NotationType | Enumeration
 *
 *     [58] NotationType ::= 'NOTATION' S '(' S? Name (S? '|' S? Name)* S? ')'
 *
 * @param ctxt  an XML parser context
 * @param tree  the enumeration tree built while parsing
 * @returns XML_ATTRIBUTE_ENUMERATION or XML_ATTRIBUTE_NOTATION
 */

int
xmlParseEnumeratedType(xmlParserCtxt *ctxt, xmlEnumeration **tree) {
    if (CMP8(CUR_PTR, 'N', 'O', 'T', 'A', 'T', 'I', 'O', 'N')) {
	SKIP(8);
	if (SKIP_BLANKS_PE == 0) {
	    xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED,
			   "Space required after 'NOTATION'\n");
	    return(0);
	}
	*tree = xmlParseNotationType(ctxt);
	if (*tree == NULL) return(0);
	return(XML_ATTRIBUTE_NOTATION);
    }
    *tree = xmlParseEnumerationType(ctxt);
    if (*tree == NULL) return(0);
    return(XML_ATTRIBUTE_ENUMERATION);
}

/**
 * Parse the Attribute list def for an element
 *
 * @deprecated Internal function, don't use.
 *
 *     [54] AttType ::= StringType | TokenizedType | EnumeratedType
 *
 *     [55] StringType ::= 'CDATA'
 *
 *     [56] TokenizedType ::= 'ID' | 'IDREF' | 'IDREFS' | 'ENTITY' |
 *                            'ENTITIES' | 'NMTOKEN' | 'NMTOKENS'
 *
 * Validity constraints for attribute values syntax are checked in
 * #xmlValidateAttributeValue
 *
 * [ VC: ID ]
 * Values of type ID must match the Name production. A name must not
 * appear more than once in an XML document as a value of this type;
 * i.e., ID values must uniquely identify the elements which bear them.
 *
 * [ VC: One ID per Element Type ]
 * No element type may have more than one ID attribute specified.
 *
 * [ VC: ID Attribute Default ]
 * An ID attribute must have a declared default of \#IMPLIED or \#REQUIRED.
 *
 * [ VC: IDREF ]
 * Values of type IDREF must match the Name production, and values
 * of type IDREFS must match Names; each IDREF Name must match the value
 * of an ID attribute on some element in the XML document; i.e. IDREF
 * values must match the value of some ID attribute.
 *
 * [ VC: Entity Name ]
 * Values of type ENTITY must match the Name production, values
 * of type ENTITIES must match Names; each Entity Name must match the
 * name of an unparsed entity declared in the DTD.
 *
 * [ VC: Name Token ]
 * Values of type NMTOKEN must match the Nmtoken production; values
 * of type NMTOKENS must match Nmtokens.
 *
 * @param ctxt  an XML parser context
 * @param tree  the enumeration tree built while parsing
 * @returns the attribute type
 */
int
xmlParseAttributeType(xmlParserCtxt *ctxt, xmlEnumeration **tree) {
    if (CMP5(CUR_PTR, 'C', 'D', 'A', 'T', 'A')) {
	SKIP(5);
	return(XML_ATTRIBUTE_CDATA);
     } else if (CMP6(CUR_PTR, 'I', 'D', 'R', 'E', 'F', 'S')) {
	SKIP(6);
	return(XML_ATTRIBUTE_IDREFS);
     } else if (CMP5(CUR_PTR, 'I', 'D', 'R', 'E', 'F')) {
	SKIP(5);
	return(XML_ATTRIBUTE_IDREF);
     } else if ((RAW == 'I') && (NXT(1) == 'D')) {
        SKIP(2);
	return(XML_ATTRIBUTE_ID);
     } else if (CMP6(CUR_PTR, 'E', 'N', 'T', 'I', 'T', 'Y')) {
	SKIP(6);
	return(XML_ATTRIBUTE_ENTITY);
     } else if (CMP8(CUR_PTR, 'E', 'N', 'T', 'I', 'T', 'I', 'E', 'S')) {
	SKIP(8);
	return(XML_ATTRIBUTE_ENTITIES);
     } else if (CMP8(CUR_PTR, 'N', 'M', 'T', 'O', 'K', 'E', 'N', 'S')) {
	SKIP(8);
	return(XML_ATTRIBUTE_NMTOKENS);
     } else if (CMP7(CUR_PTR, 'N', 'M', 'T', 'O', 'K', 'E', 'N')) {
	SKIP(7);
	return(XML_ATTRIBUTE_NMTOKEN);
     }
     return(xmlParseEnumeratedType(ctxt, tree));
}

/**
 * Parse an attribute list declaration for an element. Always consumes '<!'.
 *
 * @deprecated Internal function, don't use.
 *
 *     [52] AttlistDecl ::= '<!ATTLIST' S Name AttDef* S? '>'
 *
 *     [53] AttDef ::= S Name S AttType S DefaultDecl
 * @param ctxt  an XML parser context
 */
void
xmlParseAttributeListDecl(xmlParserCtxt *ctxt) {
    const xmlChar *elemName;
    const xmlChar *attrName;
    xmlEnumerationPtr tree;

    if ((CUR != '<') || (NXT(1) != '!'))
        return;
    SKIP(2);

    if (CMP7(CUR_PTR, 'A', 'T', 'T', 'L', 'I', 'S', 'T')) {
#ifdef LIBXML_VALID_ENABLED
	int oldInputNr = ctxt->inputNr;
#endif

	SKIP(7);
	if (SKIP_BLANKS_PE == 0) {
	    xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED,
		                 "Space required after '<!ATTLIST'\n");
	}
        elemName = xmlParseName(ctxt);
	if (elemName == NULL) {
	    xmlFatalErrMsg(ctxt, XML_ERR_NAME_REQUIRED,
			   "ATTLIST: no name for Element\n");
	    return;
	}
	SKIP_BLANKS_PE;
	GROW;
	while ((RAW != '>') && (PARSER_STOPPED(ctxt) == 0)) {
	    int type;
	    int def;
	    xmlChar *defaultValue = NULL;

	    GROW;
            tree = NULL;
	    attrName = xmlParseName(ctxt);
	    if (attrName == NULL) {
		xmlFatalErrMsg(ctxt, XML_ERR_NAME_REQUIRED,
			       "ATTLIST: no name for Attribute\n");
		break;
	    }
	    GROW;
	    if (SKIP_BLANKS_PE == 0) {
		xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED,
		        "Space required after the attribute name\n");
		break;
	    }

	    type = xmlParseAttributeType(ctxt, &tree);
	    if (type <= 0) {
	        break;
	    }

	    GROW;
	    if (SKIP_BLANKS_PE == 0) {
		xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED,
			       "Space required after the attribute type\n");
	        if (tree != NULL)
		    xmlFreeEnumeration(tree);
		break;
	    }

	    def = xmlParseDefaultDecl(ctxt, &defaultValue);
	    if (def <= 0) {
                if (defaultValue != NULL)
		    xmlFree(defaultValue);
	        if (tree != NULL)
		    xmlFreeEnumeration(tree);
	        break;
	    }
	    if ((type != XML_ATTRIBUTE_CDATA) && (defaultValue != NULL))
	        xmlAttrNormalizeSpace(defaultValue, defaultValue);

	    GROW;
            if (RAW != '>') {
		if (SKIP_BLANKS_PE == 0) {
		    xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED,
			"Space required after the attribute default value\n");
		    if (defaultValue != NULL)
			xmlFree(defaultValue);
		    if (tree != NULL)
			xmlFreeEnumeration(tree);
		    break;
		}
	    }
	    if ((ctxt->sax != NULL) && (!ctxt->disableSAX) &&
		(ctxt->sax->attributeDecl != NULL))
		ctxt->sax->attributeDecl(ctxt->userData, elemName, attrName,
	                        type, def, defaultValue, tree);
	    else if (tree != NULL)
		xmlFreeEnumeration(tree);

	    if ((ctxt->sax2) && (defaultValue != NULL) &&
	        (def != XML_ATTRIBUTE_IMPLIED) &&
		(def != XML_ATTRIBUTE_REQUIRED)) {
		xmlAddDefAttrs(ctxt, elemName, attrName, defaultValue);
	    }
	    if (ctxt->sax2) {
		xmlAddSpecialAttr(ctxt, elemName, attrName, type);
	    }
	    if (defaultValue != NULL)
	        xmlFree(defaultValue);
	    GROW;
	}
	if (RAW == '>') {
#ifdef LIBXML_VALID_ENABLED
	    if ((ctxt->validate) && (ctxt->inputNr > oldInputNr)) {
		xmlValidityError(ctxt, XML_ERR_ENTITY_BOUNDARY,
                                 "Attribute list declaration doesn't start and"
                                 " stop in the same entity\n",
                                 NULL, NULL);
	    }
#endif
	    NEXT;
	}
    }
}

/**
 * Handle PEs and check that we don't pop the entity that started
 * a balanced group.
 *
 * @param ctxt  parser context
 * @param openInputNr  input nr of the entity with opening '('
 */
static void
xmlSkipBlankCharsPEBalanced(xmlParserCtxt *ctxt, int openInputNr) {
    SKIP_BLANKS;
    GROW;

    (void) openInputNr;

    if (!PARSER_EXTERNAL(ctxt) && !PARSER_IN_PE(ctxt))
        return;

    while (!PARSER_STOPPED(ctxt)) {
        if (ctxt->input->cur >= ctxt->input->end) {
#ifdef LIBXML_VALID_ENABLED
            if ((ctxt->validate) && (ctxt->inputNr <= openInputNr)) {
                xmlValidityError(ctxt, XML_ERR_ENTITY_BOUNDARY,
                                 "Element content declaration doesn't start "
                                 "and stop in the same entity\n",
                                 NULL, NULL);
            }
#endif
            if (PARSER_IN_PE(ctxt))
                xmlPopPE(ctxt);
            else
                break;
        } else if (RAW == '%') {
            xmlParsePERefInternal(ctxt, 0);
        } else {
            break;
        }

        SKIP_BLANKS;
        GROW;
    }
}

/**
 * Parse the declaration for a Mixed Element content
 * The leading '(' and spaces have been skipped in #xmlParseElementContentDecl
 *
 * @deprecated Internal function, don't use.
 *
 *     [51] Mixed ::= '(' S? '#PCDATA' (S? '|' S? Name)* S? ')*' |
 *                    '(' S? '#PCDATA' S? ')'
 *
 * [ VC: Proper Group/PE Nesting ] applies to [51] too (see [49])
 *
 * [ VC: No Duplicate Types ]
 * The same name must not appear more than once in a single
 * mixed-content declaration.
 *
 * @param ctxt  an XML parser context
 * @param openInputNr  the input used for the current entity, needed for
 * boundary checks
 * @returns the list of the xmlElementContent describing the element choices
 */
xmlElementContent *
xmlParseElementMixedContentDecl(xmlParserCtxt *ctxt, int openInputNr) {
    xmlElementContentPtr ret = NULL, cur = NULL, n;
    const xmlChar *elem = NULL;

    GROW;
    if (CMP7(CUR_PTR, '#', 'P', 'C', 'D', 'A', 'T', 'A')) {
	SKIP(7);
        xmlSkipBlankCharsPEBalanced(ctxt, openInputNr);
	if (RAW == ')') {
#ifdef LIBXML_VALID_ENABLED
	    if ((ctxt->validate) && (ctxt->inputNr > openInputNr)) {
		xmlValidityError(ctxt, XML_ERR_ENTITY_BOUNDARY,
                                 "Element content declaration doesn't start "
                                 "and stop in the same entity\n",
                                 NULL, NULL);
	    }
#endif
	    NEXT;
	    ret = xmlNewDocElementContent(ctxt->myDoc, NULL, XML_ELEMENT_CONTENT_PCDATA);
	    if (ret == NULL)
                goto mem_error;
	    if (RAW == '*') {
		ret->ocur = XML_ELEMENT_CONTENT_MULT;
		NEXT;
	    }
	    return(ret);
	}
	if ((RAW == '(') || (RAW == '|')) {
	    ret = cur = xmlNewDocElementContent(ctxt->myDoc, NULL, XML_ELEMENT_CONTENT_PCDATA);
	    if (ret == NULL)
                goto mem_error;
	}
	while ((RAW == '|') && (PARSER_STOPPED(ctxt) == 0)) {
	    NEXT;
            n = xmlNewDocElementContent(ctxt->myDoc, NULL, XML_ELEMENT_CONTENT_OR);
            if (n == NULL)
                goto mem_error;
	    if (elem == NULL) {
		n->c1 = cur;
		if (cur != NULL)
		    cur->parent = n;
		ret = cur = n;
	    } else {
	        cur->c2 = n;
		n->parent = cur;
		n->c1 = xmlNewDocElementContent(ctxt->myDoc, elem, XML_ELEMENT_CONTENT_ELEMENT);
                if (n->c1 == NULL)
                    goto mem_error;
		n->c1->parent = n;
		cur = n;
	    }
            xmlSkipBlankCharsPEBalanced(ctxt, openInputNr);
	    elem = xmlParseName(ctxt);
	    if (elem == NULL) {
		xmlFatalErrMsg(ctxt, XML_ERR_NAME_REQUIRED,
			"xmlParseElementMixedContentDecl : Name expected\n");
		xmlFreeDocElementContent(ctxt->myDoc, ret);
		return(NULL);
	    }
            xmlSkipBlankCharsPEBalanced(ctxt, openInputNr);
	}
	if ((RAW == ')') && (NXT(1) == '*')) {
	    if (elem != NULL) {
		cur->c2 = xmlNewDocElementContent(ctxt->myDoc, elem,
		                               XML_ELEMENT_CONTENT_ELEMENT);
		if (cur->c2 == NULL)
                    goto mem_error;
		cur->c2->parent = cur;
            }
            if (ret != NULL)
                ret->ocur = XML_ELEMENT_CONTENT_MULT;
#ifdef LIBXML_VALID_ENABLED
	    if ((ctxt->validate) && (ctxt->inputNr > openInputNr)) {
		xmlValidityError(ctxt, XML_ERR_ENTITY_BOUNDARY,
                                 "Element content declaration doesn't start "
                                 "and stop in the same entity\n",
                                 NULL, NULL);
	    }
#endif
	    SKIP(2);
	} else {
	    xmlFreeDocElementContent(ctxt->myDoc, ret);
	    xmlFatalErr(ctxt, XML_ERR_MIXED_NOT_STARTED, NULL);
	    return(NULL);
	}

    } else {
	xmlFatalErr(ctxt, XML_ERR_PCDATA_REQUIRED, NULL);
    }
    return(ret);

mem_error:
    xmlErrMemory(ctxt);
    xmlFreeDocElementContent(ctxt->myDoc, ret);
    return(NULL);
}

/**
 * Parse the declaration for a Mixed Element content
 * The leading '(' and spaces have been skipped in #xmlParseElementContentDecl
 *
 *     [47] children ::= (choice | seq) ('?' | '*' | '+')?
 *
 *     [48] cp ::= (Name | choice | seq) ('?' | '*' | '+')?
 *
 *     [49] choice ::= '(' S? cp ( S? '|' S? cp )* S? ')'
 *
 *     [50] seq ::= '(' S? cp ( S? ',' S? cp )* S? ')'
 *
 * [ VC: Proper Group/PE Nesting ] applies to [49] and [50]
 * TODO Parameter-entity replacement text must be properly nested
 *	with parenthesized groups. That is to say, if either of the
 *	opening or closing parentheses in a choice, seq, or Mixed
 *	construct is contained in the replacement text for a parameter
 *	entity, both must be contained in the same replacement text. For
 *	interoperability, if a parameter-entity reference appears in a
 *	choice, seq, or Mixed construct, its replacement text should not
 *	be empty, and neither the first nor last non-blank character of
 *	the replacement text should be a connector (| or ,).
 *
 * @param ctxt  an XML parser context
 * @param openInputNr  the input used for the current entity, needed for
 * boundary checks
 * @param depth  the level of recursion
 * @returns the tree of xmlElementContent describing the element
 *          hierarchy.
 */
static xmlElementContentPtr
xmlParseElementChildrenContentDeclPriv(xmlParserCtxtPtr ctxt, int openInputNr,
                                       int depth) {
    int maxDepth = (ctxt->options & XML_PARSE_HUGE) ? 2048 : 256;
    xmlElementContentPtr ret = NULL, cur = NULL, last = NULL, op = NULL;
    const xmlChar *elem;
    xmlChar type = 0;

    if (depth > maxDepth) {
        xmlFatalErrMsgInt(ctxt, XML_ERR_RESOURCE_LIMIT,
                "xmlParseElementChildrenContentDecl : depth %d too deep, "
                "use XML_PARSE_HUGE\n", depth);
	return(NULL);
    }
    xmlSkipBlankCharsPEBalanced(ctxt, openInputNr);
    if (RAW == '(') {
        int newInputNr = ctxt->inputNr;

        /* Recurse on first child */
	NEXT;
        cur = ret = xmlParseElementChildrenContentDeclPriv(ctxt, newInputNr,
                                                           depth + 1);
        if (cur == NULL)
            return(NULL);
    } else {
	elem = xmlParseName(ctxt);
	if (elem == NULL) {
	    xmlFatalErr(ctxt, XML_ERR_ELEMCONTENT_NOT_STARTED, NULL);
	    return(NULL);
	}
        cur = ret = xmlNewDocElementContent(ctxt->myDoc, elem, XML_ELEMENT_CONTENT_ELEMENT);
	if (cur == NULL) {
	    xmlErrMemory(ctxt);
	    return(NULL);
	}
	GROW;
	if (RAW == '?') {
	    cur->ocur = XML_ELEMENT_CONTENT_OPT;
	    NEXT;
	} else if (RAW == '*') {
	    cur->ocur = XML_ELEMENT_CONTENT_MULT;
	    NEXT;
	} else if (RAW == '+') {
	    cur->ocur = XML_ELEMENT_CONTENT_PLUS;
	    NEXT;
	} else {
	    cur->ocur = XML_ELEMENT_CONTENT_ONCE;
	}
	GROW;
    }
    while (!PARSER_STOPPED(ctxt)) {
        xmlSkipBlankCharsPEBalanced(ctxt, openInputNr);
        if (RAW == ')')
            break;
        /*
	 * Each loop we parse one separator and one element.
	 */
        if (RAW == ',') {
	    if (type == 0) type = CUR;

	    /*
	     * Detect "Name | Name , Name" error
	     */
	    else if (type != CUR) {
		xmlFatalErrMsgInt(ctxt, XML_ERR_SEPARATOR_REQUIRED,
		    "xmlParseElementChildrenContentDecl : '%c' expected\n",
		                  type);
		if ((last != NULL) && (last != ret))
		    xmlFreeDocElementContent(ctxt->myDoc, last);
		if (ret != NULL)
		    xmlFreeDocElementContent(ctxt->myDoc, ret);
		return(NULL);
	    }
	    NEXT;

	    op = xmlNewDocElementContent(ctxt->myDoc, NULL, XML_ELEMENT_CONTENT_SEQ);
	    if (op == NULL) {
                xmlErrMemory(ctxt);
		if ((last != NULL) && (last != ret))
		    xmlFreeDocElementContent(ctxt->myDoc, last);
	        xmlFreeDocElementContent(ctxt->myDoc, ret);
		return(NULL);
	    }
	    if (last == NULL) {
		op->c1 = ret;
		if (ret != NULL)
		    ret->parent = op;
		ret = cur = op;
	    } else {
	        cur->c2 = op;
		if (op != NULL)
		    op->parent = cur;
		op->c1 = last;
		if (last != NULL)
		    last->parent = op;
		cur =op;
		last = NULL;
	    }
	} else if (RAW == '|') {
	    if (type == 0) type = CUR;

	    /*
	     * Detect "Name , Name | Name" error
	     */
	    else if (type != CUR) {
		xmlFatalErrMsgInt(ctxt, XML_ERR_SEPARATOR_REQUIRED,
		    "xmlParseElementChildrenContentDecl : '%c' expected\n",
				  type);
		if ((last != NULL) && (last != ret))
		    xmlFreeDocElementContent(ctxt->myDoc, last);
		if (ret != NULL)
		    xmlFreeDocElementContent(ctxt->myDoc, ret);
		return(NULL);
	    }
	    NEXT;

	    op = xmlNewDocElementContent(ctxt->myDoc, NULL, XML_ELEMENT_CONTENT_OR);
	    if (op == NULL) {
                xmlErrMemory(ctxt);
		if ((last != NULL) && (last != ret))
		    xmlFreeDocElementContent(ctxt->myDoc, last);
		if (ret != NULL)
		    xmlFreeDocElementContent(ctxt->myDoc, ret);
		return(NULL);
	    }
	    if (last == NULL) {
		op->c1 = ret;
		if (ret != NULL)
		    ret->parent = op;
		ret = cur = op;
	    } else {
	        cur->c2 = op;
		if (op != NULL)
		    op->parent = cur;
		op->c1 = last;
		if (last != NULL)
		    last->parent = op;
		cur =op;
		last = NULL;
	    }
	} else {
	    xmlFatalErr(ctxt, XML_ERR_ELEMCONTENT_NOT_FINISHED, NULL);
	    if ((last != NULL) && (last != ret))
	        xmlFreeDocElementContent(ctxt->myDoc, last);
	    if (ret != NULL)
		xmlFreeDocElementContent(ctxt->myDoc, ret);
	    return(NULL);
	}
        xmlSkipBlankCharsPEBalanced(ctxt, openInputNr);
        if (RAW == '(') {
            int newInputNr = ctxt->inputNr;

	    /* Recurse on second child */
	    NEXT;
	    last = xmlParseElementChildrenContentDeclPriv(ctxt, newInputNr,
                                                          depth + 1);
            if (last == NULL) {
		if (ret != NULL)
		    xmlFreeDocElementContent(ctxt->myDoc, ret);
		return(NULL);
            }
	} else {
	    elem = xmlParseName(ctxt);
	    if (elem == NULL) {
		xmlFatalErr(ctxt, XML_ERR_ELEMCONTENT_NOT_STARTED, NULL);
		if (ret != NULL)
		    xmlFreeDocElementContent(ctxt->myDoc, ret);
		return(NULL);
	    }
	    last = xmlNewDocElementContent(ctxt->myDoc, elem, XML_ELEMENT_CONTENT_ELEMENT);
	    if (last == NULL) {
                xmlErrMemory(ctxt);
		if (ret != NULL)
		    xmlFreeDocElementContent(ctxt->myDoc, ret);
		return(NULL);
	    }
	    if (RAW == '?') {
		last->ocur = XML_ELEMENT_CONTENT_OPT;
		NEXT;
	    } else if (RAW == '*') {
		last->ocur = XML_ELEMENT_CONTENT_MULT;
		NEXT;
	    } else if (RAW == '+') {
		last->ocur = XML_ELEMENT_CONTENT_PLUS;
		NEXT;
	    } else {
		last->ocur = XML_ELEMENT_CONTENT_ONCE;
	    }
	}
    }
    if ((cur != NULL) && (last != NULL)) {
        cur->c2 = last;
	if (last != NULL)
	    last->parent = cur;
    }
#ifdef LIBXML_VALID_ENABLED
    if ((ctxt->validate) && (ctxt->inputNr > openInputNr)) {
        xmlValidityError(ctxt, XML_ERR_ENTITY_BOUNDARY,
                         "Element content declaration doesn't start "
                         "and stop in the same entity\n",
                         NULL, NULL);
    }
#endif
    NEXT;
    if (RAW == '?') {
	if (ret != NULL) {
	    if ((ret->ocur == XML_ELEMENT_CONTENT_PLUS) ||
	        (ret->ocur == XML_ELEMENT_CONTENT_MULT))
	        ret->ocur = XML_ELEMENT_CONTENT_MULT;
	    else
	        ret->ocur = XML_ELEMENT_CONTENT_OPT;
	}
	NEXT;
    } else if (RAW == '*') {
	if (ret != NULL) {
	    ret->ocur = XML_ELEMENT_CONTENT_MULT;
	    cur = ret;
	    /*
	     * Some normalization:
	     * (a | b* | c?)* == (a | b | c)*
	     */
	    while ((cur != NULL) && (cur->type == XML_ELEMENT_CONTENT_OR)) {
		if ((cur->c1 != NULL) &&
	            ((cur->c1->ocur == XML_ELEMENT_CONTENT_OPT) ||
		     (cur->c1->ocur == XML_ELEMENT_CONTENT_MULT)))
		    cur->c1->ocur = XML_ELEMENT_CONTENT_ONCE;
		if ((cur->c2 != NULL) &&
	            ((cur->c2->ocur == XML_ELEMENT_CONTENT_OPT) ||
		     (cur->c2->ocur == XML_ELEMENT_CONTENT_MULT)))
		    cur->c2->ocur = XML_ELEMENT_CONTENT_ONCE;
		cur = cur->c2;
	    }
	}
	NEXT;
    } else if (RAW == '+') {
	if (ret != NULL) {
	    int found = 0;

	    if ((ret->ocur == XML_ELEMENT_CONTENT_OPT) ||
	        (ret->ocur == XML_ELEMENT_CONTENT_MULT))
	        ret->ocur = XML_ELEMENT_CONTENT_MULT;
	    else
	        ret->ocur = XML_ELEMENT_CONTENT_PLUS;
	    /*
	     * Some normalization:
	     * (a | b*)+ == (a | b)*
	     * (a | b?)+ == (a | b)*
	     */
	    while ((cur != NULL) && (cur->type == XML_ELEMENT_CONTENT_OR)) {
		if ((cur->c1 != NULL) &&
	            ((cur->c1->ocur == XML_ELEMENT_CONTENT_OPT) ||
		     (cur->c1->ocur == XML_ELEMENT_CONTENT_MULT))) {
		    cur->c1->ocur = XML_ELEMENT_CONTENT_ONCE;
		    found = 1;
		}
		if ((cur->c2 != NULL) &&
	            ((cur->c2->ocur == XML_ELEMENT_CONTENT_OPT) ||
		     (cur->c2->ocur == XML_ELEMENT_CONTENT_MULT))) {
		    cur->c2->ocur = XML_ELEMENT_CONTENT_ONCE;
		    found = 1;
		}
		cur = cur->c2;
	    }
	    if (found)
		ret->ocur = XML_ELEMENT_CONTENT_MULT;
	}
	NEXT;
    }
    return(ret);
}

/**
 * Parse the declaration for a Mixed Element content
 * The leading '(' and spaces have been skipped in #xmlParseElementContentDecl
 *
 * @deprecated Internal function, don't use.
 *
 *     [47] children ::= (choice | seq) ('?' | '*' | '+')?
 *
 *     [48] cp ::= (Name | choice | seq) ('?' | '*' | '+')?
 *
 *     [49] choice ::= '(' S? cp ( S? '|' S? cp )* S? ')'
 *
 *     [50] seq ::= '(' S? cp ( S? ',' S? cp )* S? ')'
 *
 * [ VC: Proper Group/PE Nesting ] applies to [49] and [50]
 * TODO Parameter-entity replacement text must be properly nested
 *	with parenthesized groups. That is to say, if either of the
 *	opening or closing parentheses in a choice, seq, or Mixed
 *	construct is contained in the replacement text for a parameter
 *	entity, both must be contained in the same replacement text. For
 *	interoperability, if a parameter-entity reference appears in a
 *	choice, seq, or Mixed construct, its replacement text should not
 *	be empty, and neither the first nor last non-blank character of
 *	the replacement text should be a connector (| or ,).
 *
 * @param ctxt  an XML parser context
 * @param inputchk  the input used for the current entity, needed for boundary checks
 * @returns the tree of xmlElementContent describing the element
 *          hierarchy.
 */
xmlElementContent *
xmlParseElementChildrenContentDecl(xmlParserCtxt *ctxt, int inputchk) {
    /* stub left for API/ABI compat */
    return(xmlParseElementChildrenContentDeclPriv(ctxt, inputchk, 1));
}

/**
 * Parse the declaration for an Element content either Mixed or Children,
 * the cases EMPTY and ANY are handled directly in #xmlParseElementDecl
 *
 * @deprecated Internal function, don't use.
 *
 *     [46] contentspec ::= 'EMPTY' | 'ANY' | Mixed | children
 *
 * @param ctxt  an XML parser context
 * @param name  the name of the element being defined.
 * @param result  the Element Content pointer will be stored here if any
 * @returns an xmlElementTypeVal value or -1 on error
 */

int
xmlParseElementContentDecl(xmlParserCtxt *ctxt, const xmlChar *name,
                           xmlElementContent **result) {

    xmlElementContentPtr tree = NULL;
    int openInputNr = ctxt->inputNr;
    int res;

    *result = NULL;

    if (RAW != '(') {
	xmlFatalErrMsgStr(ctxt, XML_ERR_ELEMCONTENT_NOT_STARTED,
		"xmlParseElementContentDecl : %s '(' expected\n", name);
	return(-1);
    }
    NEXT;
    xmlSkipBlankCharsPEBalanced(ctxt, openInputNr);
    if (CMP7(CUR_PTR, '#', 'P', 'C', 'D', 'A', 'T', 'A')) {
        tree = xmlParseElementMixedContentDecl(ctxt, openInputNr);
	res = XML_ELEMENT_TYPE_MIXED;
    } else {
        tree = xmlParseElementChildrenContentDeclPriv(ctxt, openInputNr, 1);
	res = XML_ELEMENT_TYPE_ELEMENT;
    }
    if (tree == NULL)
        return(-1);
    SKIP_BLANKS_PE;
    *result = tree;
    return(res);
}

/**
 * Parse an element declaration. Always consumes '<!'.
 *
 * @deprecated Internal function, don't use.
 *
 *     [45] elementdecl ::= '<!ELEMENT' S Name S contentspec S? '>'
 *
 * [ VC: Unique Element Type Declaration ]
 * No element type may be declared more than once
 *
 * @param ctxt  an XML parser context
 * @returns the type of the element, or -1 in case of error
 */
int
xmlParseElementDecl(xmlParserCtxt *ctxt) {
    const xmlChar *name;
    int ret = -1;
    xmlElementContentPtr content  = NULL;

    if ((CUR != '<') || (NXT(1) != '!'))
        return(ret);
    SKIP(2);

    /* GROW; done in the caller */
    if (CMP7(CUR_PTR, 'E', 'L', 'E', 'M', 'E', 'N', 'T')) {
#ifdef LIBXML_VALID_ENABLED
	int oldInputNr = ctxt->inputNr;
#endif

	SKIP(7);
	if (SKIP_BLANKS_PE == 0) {
	    xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED,
		           "Space required after 'ELEMENT'\n");
	    return(-1);
	}
        name = xmlParseName(ctxt);
	if (name == NULL) {
	    xmlFatalErrMsg(ctxt, XML_ERR_NAME_REQUIRED,
			   "xmlParseElementDecl: no name for Element\n");
	    return(-1);
	}
	if (SKIP_BLANKS_PE == 0) {
	    xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED,
			   "Space required after the element name\n");
	}
	if (CMP5(CUR_PTR, 'E', 'M', 'P', 'T', 'Y')) {
	    SKIP(5);
	    /*
	     * Element must always be empty.
	     */
	    ret = XML_ELEMENT_TYPE_EMPTY;
	} else if ((RAW == 'A') && (NXT(1) == 'N') &&
	           (NXT(2) == 'Y')) {
	    SKIP(3);
	    /*
	     * Element is a generic container.
	     */
	    ret = XML_ELEMENT_TYPE_ANY;
	} else if (RAW == '(') {
	    ret = xmlParseElementContentDecl(ctxt, name, &content);
            if (ret <= 0)
                return(-1);
	} else {
	    /*
	     * [ WFC: PEs in Internal Subset ] error handling.
	     */
            xmlFatalErrMsg(ctxt, XML_ERR_ELEMCONTENT_NOT_STARTED,
                  "xmlParseElementDecl: 'EMPTY', 'ANY' or '(' expected\n");
	    return(-1);
	}

	SKIP_BLANKS_PE;

	if (RAW != '>') {
	    xmlFatalErr(ctxt, XML_ERR_GT_REQUIRED, NULL);
	    if (content != NULL) {
		xmlFreeDocElementContent(ctxt->myDoc, content);
	    }
	} else {
#ifdef LIBXML_VALID_ENABLED
	    if ((ctxt->validate) && (ctxt->inputNr > oldInputNr)) {
		xmlValidityError(ctxt, XML_ERR_ENTITY_BOUNDARY,
                                 "Element declaration doesn't start and stop in"
                                 " the same entity\n",
                                 NULL, NULL);
	    }
#endif

	    NEXT;
	    if ((ctxt->sax != NULL) && (!ctxt->disableSAX) &&
		(ctxt->sax->elementDecl != NULL)) {
		if (content != NULL)
		    content->parent = NULL;
	        ctxt->sax->elementDecl(ctxt->userData, name, ret,
		                       content);
		if ((content != NULL) && (content->parent == NULL)) {
		    /*
		     * this is a trick: if xmlAddElementDecl is called,
		     * instead of copying the full tree it is plugged directly
		     * if called from the parser. Avoid duplicating the
		     * interfaces or change the API/ABI
		     */
		    xmlFreeDocElementContent(ctxt->myDoc, content);
		}
	    } else if (content != NULL) {
		xmlFreeDocElementContent(ctxt->myDoc, content);
	    }
	}
    }
    return(ret);
}

/**
 * Parse a conditional section. Always consumes '<!['.
 *
 *     [61] conditionalSect ::= includeSect | ignoreSect
 *     [62] includeSect ::= '<![' S? 'INCLUDE' S? '[' extSubsetDecl ']]>'
 *     [63] ignoreSect ::= '<![' S? 'IGNORE' S? '[' ignoreSectContents* ']]>'
 *     [64] ignoreSectContents ::= Ignore ('<![' ignoreSectContents ']]>'
 *                                 Ignore)*
 *     [65] Ignore ::= Char* - (Char* ('<![' | ']]>') Char*)
 * @param ctxt  an XML parser context
 */

static void
xmlParseConditionalSections(xmlParserCtxtPtr ctxt) {
    size_t depth = 0;
    int isFreshPE = 0;
    int oldInputNr = ctxt->inputNr;
    int declInputNr = ctxt->inputNr;

    while (!PARSER_STOPPED(ctxt)) {
        if (ctxt->input->cur >= ctxt->input->end) {
            if (ctxt->inputNr <= oldInputNr) {
                xmlFatalErr(ctxt, XML_ERR_EXT_SUBSET_NOT_FINISHED, NULL);
                return;
            }

            xmlPopPE(ctxt);
            declInputNr = ctxt->inputNr;
        } else if ((RAW == '<') && (NXT(1) == '!') && (NXT(2) == '[')) {
            SKIP(3);
            SKIP_BLANKS_PE;

            isFreshPE = 0;

            if (CMP7(CUR_PTR, 'I', 'N', 'C', 'L', 'U', 'D', 'E')) {
                SKIP(7);
                SKIP_BLANKS_PE;
                if (RAW != '[') {
                    xmlFatalErr(ctxt, XML_ERR_CONDSEC_INVALID, NULL);
                    return;
                }
#ifdef LIBXML_VALID_ENABLED
                if ((ctxt->validate) && (ctxt->inputNr > declInputNr)) {
		    xmlValidityError(ctxt, XML_ERR_ENTITY_BOUNDARY,
                                     "All markup of the conditional section is"
                                     " not in the same entity\n",
                                     NULL, NULL);
                }
#endif
                NEXT;

                depth++;
            } else if (CMP6(CUR_PTR, 'I', 'G', 'N', 'O', 'R', 'E')) {
                size_t ignoreDepth = 0;

                SKIP(6);
                SKIP_BLANKS_PE;
                if (RAW != '[') {
                    xmlFatalErr(ctxt, XML_ERR_CONDSEC_INVALID, NULL);
                    return;
                }
#ifdef LIBXML_VALID_ENABLED
                if ((ctxt->validate) && (ctxt->inputNr > declInputNr)) {
		    xmlValidityError(ctxt, XML_ERR_ENTITY_BOUNDARY,
                                     "All markup of the conditional section is"
                                     " not in the same entity\n",
                                     NULL, NULL);
                }
#endif
                NEXT;

                while (PARSER_STOPPED(ctxt) == 0) {
                    if (RAW == 0) {
                        xmlFatalErr(ctxt, XML_ERR_CONDSEC_NOT_FINISHED, NULL);
                        return;
                    }
                    if ((RAW == '<') && (NXT(1) == '!') && (NXT(2) == '[')) {
                        SKIP(3);
                        ignoreDepth++;
                        /* Check for integer overflow */
                        if (ignoreDepth == 0) {
                            xmlErrMemory(ctxt);
                            return;
                        }
                    } else if ((RAW == ']') && (NXT(1) == ']') &&
                               (NXT(2) == '>')) {
                        SKIP(3);
                        if (ignoreDepth == 0)
                            break;
                        ignoreDepth--;
                    } else {
                        NEXT;
                    }
                }

#ifdef LIBXML_VALID_ENABLED
                if ((ctxt->validate) && (ctxt->inputNr > declInputNr)) {
		    xmlValidityError(ctxt, XML_ERR_ENTITY_BOUNDARY,
                                     "All markup of the conditional section is"
                                     " not in the same entity\n",
                                     NULL, NULL);
                }
#endif
            } else {
                xmlFatalErr(ctxt, XML_ERR_CONDSEC_INVALID_KEYWORD, NULL);
                return;
            }
        } else if ((depth > 0) &&
                   (RAW == ']') && (NXT(1) == ']') && (NXT(2) == '>')) {
            if (isFreshPE) {
                xmlFatalErrMsg(ctxt, XML_ERR_CONDSEC_INVALID,
                               "Parameter entity must match "
                               "extSubsetDecl\n");
                return;
            }

            depth--;
#ifdef LIBXML_VALID_ENABLED
            if ((ctxt->validate) && (ctxt->inputNr > declInputNr)) {
		xmlValidityError(ctxt, XML_ERR_ENTITY_BOUNDARY,
                                 "All markup of the conditional section is not"
                                 " in the same entity\n",
                                 NULL, NULL);
            }
#endif
            SKIP(3);
        } else if ((RAW == '<') && ((NXT(1) == '!') || (NXT(1) == '?'))) {
            isFreshPE = 0;
            xmlParseMarkupDecl(ctxt);
        } else if (RAW == '%') {
            xmlParsePERefInternal(ctxt, 1);
            if (ctxt->inputNr > declInputNr) {
                isFreshPE = 1;
                declInputNr = ctxt->inputNr;
            }
        } else {
            xmlFatalErr(ctxt, XML_ERR_EXT_SUBSET_NOT_FINISHED, NULL);
            return;
        }

        if (depth == 0)
            break;

        SKIP_BLANKS;
        SHRINK;
        GROW;
    }
}

/**
 * Parse markup declarations. Always consumes '<!' or '<?'.
 *
 * @deprecated Internal function, don't use.
 *
 *     [29] markupdecl ::= elementdecl | AttlistDecl | EntityDecl |
 *                         NotationDecl | PI | Comment
 *
 * [ VC: Proper Declaration/PE Nesting ]
 * Parameter-entity replacement text must be properly nested with
 * markup declarations. That is to say, if either the first character
 * or the last character of a markup declaration (markupdecl above) is
 * contained in the replacement text for a parameter-entity reference,
 * both must be contained in the same replacement text.
 *
 * [ WFC: PEs in Internal Subset ]
 * In the internal DTD subset, parameter-entity references can occur
 * only where markup declarations can occur, not within markup declarations.
 * (This does not apply to references that occur in external parameter
 * entities or to the external subset.)
 *
 * @param ctxt  an XML parser context
 */
void
xmlParseMarkupDecl(xmlParserCtxt *ctxt) {
    GROW;
    if (CUR == '<') {
        if (NXT(1) == '!') {
	    switch (NXT(2)) {
	        case 'E':
		    if (NXT(3) == 'L')
			xmlParseElementDecl(ctxt);
		    else if (NXT(3) == 'N')
			xmlParseEntityDecl(ctxt);
                    else
                        SKIP(2);
		    break;
	        case 'A':
		    xmlParseAttributeListDecl(ctxt);
		    break;
	        case 'N':
		    xmlParseNotationDecl(ctxt);
		    break;
	        case '-':
		    xmlParseComment(ctxt);
		    break;
		default:
                    xmlFatalErr(ctxt,
                                ctxt->inSubset == 2 ?
                                    XML_ERR_EXT_SUBSET_NOT_FINISHED :
                                    XML_ERR_INT_SUBSET_NOT_FINISHED,
                                NULL);
                    SKIP(2);
		    break;
	    }
	} else if (NXT(1) == '?') {
	    xmlParsePI(ctxt);
	}
    }
}

/**
 * Parse an XML declaration header for external entities
 *
 * @deprecated Internal function, don't use.
 *
 *     [77] TextDecl ::= '<?xml' VersionInfo? EncodingDecl S? '?>'
 * @param ctxt  an XML parser context
 */

void
xmlParseTextDecl(xmlParserCtxt *ctxt) {
    xmlChar *version;

    /*
     * We know that '<?xml' is here.
     */
    if ((CMP5(CUR_PTR, '<', '?', 'x', 'm', 'l')) && (IS_BLANK_CH(NXT(5)))) {
	SKIP(5);
    } else {
	xmlFatalErr(ctxt, XML_ERR_XMLDECL_NOT_STARTED, NULL);
	return;
    }

    if (SKIP_BLANKS == 0) {
	xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED,
		       "Space needed after '<?xml'\n");
    }

    /*
     * We may have the VersionInfo here.
     */
    version = xmlParseVersionInfo(ctxt);
    if (version == NULL) {
	version = xmlCharStrdup(XML_DEFAULT_VERSION);
        if (version == NULL) {
            xmlErrMemory(ctxt);
            return;
        }
    } else {
	if (SKIP_BLANKS == 0) {
	    xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED,
		           "Space needed here\n");
	}
    }
    ctxt->input->version = version;

    /*
     * We must have the encoding declaration
     */
    xmlParseEncodingDecl(ctxt);

    SKIP_BLANKS;
    if ((RAW == '?') && (NXT(1) == '>')) {
        SKIP(2);
    } else if (RAW == '>') {
        /* Deprecated old WD ... */
	xmlFatalErr(ctxt, XML_ERR_XMLDECL_NOT_FINISHED, NULL);
	NEXT;
    } else {
        int c;

	xmlFatalErr(ctxt, XML_ERR_XMLDECL_NOT_FINISHED, NULL);
        while ((PARSER_STOPPED(ctxt) == 0) && ((c = CUR) != 0)) {
            NEXT;
            if (c == '>')
                break;
        }
    }
}

/**
 * Parse Markup declarations from an external subset
 *
 * @deprecated Internal function, don't use.
 *
 *     [30] extSubset ::= textDecl? extSubsetDecl
 *
 *     [31] extSubsetDecl ::= (markupdecl | conditionalSect |
 *                             PEReference | S) *
 * @param ctxt  an XML parser context
 * @param publicId  the public identifier
 * @param systemId  the system identifier (URL)
 */
void
xmlParseExternalSubset(xmlParserCtxt *ctxt, const xmlChar *publicId,
                       const xmlChar *systemId) {
    int oldInputNr;

    xmlCtxtInitializeLate(ctxt);

    xmlDetectEncoding(ctxt);

    if (CMP5(CUR_PTR, '<', '?', 'x', 'm', 'l')) {
	xmlParseTextDecl(ctxt);
    }
    if (ctxt->myDoc == NULL) {
        ctxt->myDoc = xmlNewDoc(BAD_CAST "1.0");
	if (ctxt->myDoc == NULL) {
	    xmlErrMemory(ctxt);
	    return;
	}
	ctxt->myDoc->properties = XML_DOC_INTERNAL;
    }
    if ((ctxt->myDoc->intSubset == NULL) &&
        (xmlCreateIntSubset(ctxt->myDoc, NULL, publicId, systemId) == NULL)) {
        xmlErrMemory(ctxt);
    }

    ctxt->inSubset = 2;
    oldInputNr = ctxt->inputNr;

    SKIP_BLANKS;
    while (!PARSER_STOPPED(ctxt)) {
        if (ctxt->input->cur >= ctxt->input->end) {
            if (ctxt->inputNr <= oldInputNr) {
                xmlParserCheckEOF(ctxt, XML_ERR_EXT_SUBSET_NOT_FINISHED);
                break;
            }

            xmlPopPE(ctxt);
        } else if ((RAW == '<') && (NXT(1) == '!') && (NXT(2) == '[')) {
            xmlParseConditionalSections(ctxt);
        } else if ((RAW == '<') && ((NXT(1) == '!') || (NXT(1) == '?'))) {
            xmlParseMarkupDecl(ctxt);
        } else if (RAW == '%') {
            xmlParsePERefInternal(ctxt, 1);
        } else {
            xmlFatalErr(ctxt, XML_ERR_EXT_SUBSET_NOT_FINISHED, NULL);

            while (ctxt->inputNr > oldInputNr)
                xmlPopPE(ctxt);
            break;
        }
        SKIP_BLANKS;
        SHRINK;
        GROW;
    }
}

/**
 * Parse and handle entity references in content, depending on the SAX
 * interface, this may end-up in a call to character() if this is a
 * CharRef, a predefined entity, if there is no reference() callback.
 * or if the parser was asked to switch to that mode.
 *
 * @deprecated Internal function, don't use.
 *
 * Always consumes '&'.
 *
 *     [67] Reference ::= EntityRef | CharRef
 * @param ctxt  an XML parser context
 */
void
xmlParseReference(xmlParserCtxt *ctxt) {
    xmlEntityPtr ent = NULL;
    const xmlChar *name;
    xmlChar *val;

    if (RAW != '&')
        return;

    /*
     * Simple case of a CharRef
     */
    if (NXT(1) == '#') {
	int i = 0;
	xmlChar out[16];
	int value = xmlParseCharRef(ctxt);

	if (value == 0)
	    return;

        /*
         * Just encode the value in UTF-8
         */
        COPY_BUF(out, i, value);
        out[i] = 0;
        if ((ctxt->sax != NULL) && (ctxt->sax->characters != NULL) &&
            (!ctxt->disableSAX))
            ctxt->sax->characters(ctxt->userData, out, i);
	return;
    }

    /*
     * We are seeing an entity reference
     */
    name = xmlParseEntityRefInternal(ctxt);
    if (name == NULL)
        return;
    ent = xmlLookupGeneralEntity(ctxt, name, /* isAttr */ 0);
    if (ent == NULL) {
        /*
         * Create a reference for undeclared entities.
         */
        if ((ctxt->replaceEntities == 0) &&
            (ctxt->sax != NULL) &&
            (ctxt->disableSAX == 0) &&
            (ctxt->sax->reference != NULL)) {
            ctxt->sax->reference(ctxt->userData, name);
        }
        return;
    }
    if (!ctxt->wellFormed)
	return;

    /* special case of predefined entities */
    if ((ent->name == NULL) ||
        (ent->etype == XML_INTERNAL_PREDEFINED_ENTITY)) {
	val = ent->content;
	if (val == NULL) return;
	/*
	 * inline the entity.
	 */
	if ((ctxt->sax != NULL) && (ctxt->sax->characters != NULL) &&
	    (!ctxt->disableSAX))
	    ctxt->sax->characters(ctxt->userData, val, xmlStrlen(val));
	return;
    }

    /*
     * Some users try to parse entities on their own and used to set
     * the renamed "checked" member. Fix the flags to cover this
     * case.
     */
    if (((ent->flags & XML_ENT_PARSED) == 0) && (ent->children != NULL))
        ent->flags |= XML_ENT_PARSED;

    /*
     * The first reference to the entity trigger a parsing phase
     * where the ent->children is filled with the result from
     * the parsing.
     * Note: external parsed entities will not be loaded, it is not
     * required for a non-validating parser, unless the parsing option
     * of validating, or substituting entities were given. Doing so is
     * far more secure as the parser will only process data coming from
     * the document entity by default.
     *
     * FIXME: This doesn't work correctly since entities can be
     * expanded with different namespace declarations in scope.
     * For example:
     *
     * <!DOCTYPE doc [
     *   <!ENTITY ent "<ns:elem/>">
     * ]>
     * <doc>
     *   <decl1 xmlns:ns="urn:ns1">
     *     &ent;
     *   </decl1>
     *   <decl2 xmlns:ns="urn:ns2">
     *     &ent;
     *   </decl2>
     * </doc>
     *
     * Proposed fix:
     *
     * - Ignore current namespace declarations when parsing the
     *   entity. If a prefix can't be resolved, don't report an error
     *   but mark it as unresolved.
     * - Try to resolve these prefixes when expanding the entity.
     *   This will require a specialized version of xmlStaticCopyNode
     *   which can also make use of the namespace hash table to avoid
     *   quadratic behavior.
     *
     * Alternatively, we could simply reparse the entity on each
     * expansion like we already do with custom SAX callbacks.
     * External entity content should be cached in this case.
     */
    if ((ent->etype == XML_INTERNAL_GENERAL_ENTITY) ||
        (((ctxt->options & XML_PARSE_NO_XXE) == 0) &&
         ((ctxt->replaceEntities) ||
          (ctxt->validate)))) {
        if ((ent->flags & XML_ENT_PARSED) == 0) {
            xmlCtxtParseEntity(ctxt, ent);
        } else if (ent->children == NULL) {
            /*
             * Probably running in SAX mode and the callbacks don't
             * build the entity content. Parse the entity again.
             *
             * This will also be triggered in normal tree builder mode
             * if an entity happens to be empty, causing unnecessary
             * reloads. It's hard to come up with a reliable check in
             * which mode we're running.
             */
            xmlCtxtParseEntity(ctxt, ent);
        }
    }

    /*
     * We also check for amplification if entities aren't substituted.
     * They might be expanded later.
     */
    if (xmlParserEntityCheck(ctxt, ent->expandedSize))
        return;

    if ((ctxt->sax == NULL) || (ctxt->disableSAX))
        return;

    if (ctxt->replaceEntities == 0) {
	/*
	 * Create a reference
	 */
        if (ctxt->sax->reference != NULL)
	    ctxt->sax->reference(ctxt->userData, ent->name);
    } else if ((ent->children != NULL) && (ctxt->node != NULL)) {
        xmlNodePtr copy, cur;

        /*
         * Seems we are generating the DOM content, copy the tree
	 */
        cur = ent->children;

        /*
         * Handle first text node with SAX to coalesce text efficiently
         */
        if ((cur->type == XML_TEXT_NODE) ||
            (cur->type == XML_CDATA_SECTION_NODE)) {
            int len = xmlStrlen(cur->content);

            if ((cur->type == XML_TEXT_NODE) ||
                (ctxt->options & XML_PARSE_NOCDATA)) {
                if (ctxt->sax->characters != NULL)
                    ctxt->sax->characters(ctxt, cur->content, len);
            } else {
                if (ctxt->sax->cdataBlock != NULL)
                    ctxt->sax->cdataBlock(ctxt, cur->content, len);
            }

            cur = cur->next;
        }

        while (cur != NULL) {
            xmlNodePtr last;

            /*
             * Handle last text node with SAX to coalesce text efficiently
             */
            if ((cur->next == NULL) &&
                ((cur->type == XML_TEXT_NODE) ||
                 (cur->type == XML_CDATA_SECTION_NODE))) {
                int len = xmlStrlen(cur->content);

                if ((cur->type == XML_TEXT_NODE) ||
                    (ctxt->options & XML_PARSE_NOCDATA)) {
                    if (ctxt->sax->characters != NULL)
                        ctxt->sax->characters(ctxt, cur->content, len);
                } else {
                    if (ctxt->sax->cdataBlock != NULL)
                        ctxt->sax->cdataBlock(ctxt, cur->content, len);
                }

                break;
            }

            /*
             * Reset coalesce buffer stats only for non-text nodes.
             */
            ctxt->nodemem = 0;
            ctxt->nodelen = 0;

            copy = xmlDocCopyNode(cur, ctxt->myDoc, 1);

            if (copy == NULL) {
                xmlErrMemory(ctxt);
                break;
            }

            if (ctxt->parseMode == XML_PARSE_READER) {
                /* Needed for reader */
                copy->extra = cur->extra;
                /* Maybe needed for reader */
                copy->_private = cur->_private;
            }

            copy->parent = ctxt->node;
            last = ctxt->node->last;
            if (last == NULL) {
                ctxt->node->children = copy;
            } else {
                last->next = copy;
                copy->prev = last;
            }
            ctxt->node->last = copy;

            cur = cur->next;
        }
    }
}

static void
xmlHandleUndeclaredEntity(xmlParserCtxtPtr ctxt, const xmlChar *name) {
    /*
     * [ WFC: Entity Declared ]
     * In a document without any DTD, a document with only an
     * internal DTD subset which contains no parameter entity
     * references, or a document with "standalone='yes'", the
     * Name given in the entity reference must match that in an
     * entity declaration, except that well-formed documents
     * need not declare any of the following entities: amp, lt,
     * gt, apos, quot.
     * The declaration of a parameter entity must precede any
     * reference to it.
     * Similarly, the declaration of a general entity must
     * precede any reference to it which appears in a default
     * value in an attribute-list declaration. Note that if
     * entities are declared in the external subset or in
     * external parameter entities, a non-validating processor
     * is not obligated to read and process their declarations;
     * for such documents, the rule that an entity must be
     * declared is a well-formedness constraint only if
     * standalone='yes'.
     */
    if ((ctxt->standalone == 1) ||
        ((ctxt->hasExternalSubset == 0) &&
         (ctxt->hasPErefs == 0))) {
        xmlFatalErrMsgStr(ctxt, XML_ERR_UNDECLARED_ENTITY,
                          "Entity '%s' not defined\n", name);
#ifdef LIBXML_VALID_ENABLED
    } else if (ctxt->validate) {
        /*
         * [ VC: Entity Declared ]
         * In a document with an external subset or external
         * parameter entities with "standalone='no'", ...
         * ... The declaration of a parameter entity must
         * precede any reference to it...
         */
        xmlValidityError(ctxt, XML_ERR_UNDECLARED_ENTITY,
                         "Entity '%s' not defined\n", name, NULL);
#endif
    } else if ((ctxt->loadsubset & ~XML_SKIP_IDS) ||
               ((ctxt->replaceEntities) &&
                ((ctxt->options & XML_PARSE_NO_XXE) == 0))) {
        /*
         * Also raise a non-fatal error
         *
         * - if the external subset is loaded and all entity declarations
         *   should be available, or
         * - entity substition was requested without restricting
         *   external entity access.
         */
        xmlErrMsgStr(ctxt, XML_WAR_UNDECLARED_ENTITY,
                     "Entity '%s' not defined\n", name);
    } else {
        xmlWarningMsg(ctxt, XML_WAR_UNDECLARED_ENTITY,
                      "Entity '%s' not defined\n", name, NULL);
    }

    ctxt->valid = 0;
}

static xmlEntityPtr
xmlLookupGeneralEntity(xmlParserCtxtPtr ctxt, const xmlChar *name, int inAttr) {
    xmlEntityPtr ent = NULL;

    /*
     * Predefined entities override any extra definition
     */
    if ((ctxt->options & XML_PARSE_OLDSAX) == 0) {
        ent = xmlGetPredefinedEntity(name);
        if (ent != NULL)
            return(ent);
    }

    /*
     * Ask first SAX for entity resolution, otherwise try the
     * entities which may have stored in the parser context.
     */
    if (ctxt->sax != NULL) {
	if (ctxt->sax->getEntity != NULL)
	    ent = ctxt->sax->getEntity(ctxt->userData, name);
	if ((ctxt->wellFormed == 1 ) && (ent == NULL) &&
	    (ctxt->options & XML_PARSE_OLDSAX))
	    ent = xmlGetPredefinedEntity(name);
	if ((ctxt->wellFormed == 1 ) && (ent == NULL) &&
	    (ctxt->userData==ctxt)) {
	    ent = xmlSAX2GetEntity(ctxt, name);
	}
    }

    if (ent == NULL) {
        xmlHandleUndeclaredEntity(ctxt, name);
    }

    /*
     * [ WFC: Parsed Entity ]
     * An entity reference must not contain the name of an
     * unparsed entity
     */
    else if (ent->etype == XML_EXTERNAL_GENERAL_UNPARSED_ENTITY) {
	xmlFatalErrMsgStr(ctxt, XML_ERR_UNPARSED_ENTITY,
		 "Entity reference to unparsed entity %s\n", name);
        ent = NULL;
    }

    /*
     * [ WFC: No External Entity References ]
     * Attribute values cannot contain direct or indirect
     * entity references to external entities.
     */
    else if (ent->etype == XML_EXTERNAL_GENERAL_PARSED_ENTITY) {
        if (inAttr) {
            xmlFatalErrMsgStr(ctxt, XML_ERR_ENTITY_IS_EXTERNAL,
                 "Attribute references external entity '%s'\n", name);
            ent = NULL;
        }
    }

    return(ent);
}

/**
 * Parse an entity reference. Always consumes '&'.
 *
 *     [68] EntityRef ::= '&' Name ';'
 *
 * @param ctxt  an XML parser context
 * @returns the name, or NULL in case of error.
 */
static const xmlChar *
xmlParseEntityRefInternal(xmlParserCtxtPtr ctxt) {
    const xmlChar *name;

    GROW;

    if (RAW != '&')
        return(NULL);
    NEXT;
    name = xmlParseName(ctxt);
    if (name == NULL) {
	xmlFatalErrMsg(ctxt, XML_ERR_NAME_REQUIRED,
		       "xmlParseEntityRef: no name\n");
        return(NULL);
    }
    if (RAW != ';') {
	xmlFatalErr(ctxt, XML_ERR_ENTITYREF_SEMICOL_MISSING, NULL);
	return(NULL);
    }
    NEXT;

    return(name);
}

/**
 * @deprecated Internal function, don't use.
 *
 * @param ctxt  an XML parser context
 * @returns the xmlEntity if found, or NULL otherwise.
 */
xmlEntity *
xmlParseEntityRef(xmlParserCtxt *ctxt) {
    const xmlChar *name;

    if (ctxt == NULL)
        return(NULL);

    name = xmlParseEntityRefInternal(ctxt);
    if (name == NULL)
        return(NULL);

    return(xmlLookupGeneralEntity(ctxt, name, /* inAttr */ 0));
}

/**
 * Parse ENTITY references declarations, but this version parses it from
 * a string value.
 *
 *     [68] EntityRef ::= '&' Name ';'
 *
 * [ WFC: Entity Declared ]
 * In a document without any DTD, a document with only an internal DTD
 * subset which contains no parameter entity references, or a document
 * with "standalone='yes'", the Name given in the entity reference
 * must match that in an entity declaration, except that well-formed
 * documents need not declare any of the following entities: amp, lt,
 * gt, apos, quot.  The declaration of a parameter entity must precede
 * any reference to it.  Similarly, the declaration of a general entity
 * must precede any reference to it which appears in a default value in an
 * attribute-list declaration. Note that if entities are declared in the
 * external subset or in external parameter entities, a non-validating
 * processor is not obligated to read and process their declarations;
 * for such documents, the rule that an entity must be declared is a
 * well-formedness constraint only if standalone='yes'.
 *
 * [ WFC: Parsed Entity ]
 * An entity reference must not contain the name of an unparsed entity
 *
 * @param ctxt  an XML parser context
 * @param str  a pointer to an index in the string
 * @returns the xmlEntity if found, or NULL otherwise. The str pointer
 * is updated to the current location in the string.
 */
static xmlChar *
xmlParseStringEntityRef(xmlParserCtxtPtr ctxt, const xmlChar ** str) {
    xmlChar *name;
    const xmlChar *ptr;
    xmlChar cur;

    if ((str == NULL) || (*str == NULL))
        return(NULL);
    ptr = *str;
    cur = *ptr;
    if (cur != '&')
	return(NULL);

    ptr++;
    name = xmlParseStringName(ctxt, &ptr);
    if (name == NULL) {
	xmlFatalErrMsg(ctxt, XML_ERR_NAME_REQUIRED,
		       "xmlParseStringEntityRef: no name\n");
	*str = ptr;
	return(NULL);
    }
    if (*ptr != ';') {
	xmlFatalErr(ctxt, XML_ERR_ENTITYREF_SEMICOL_MISSING, NULL);
        xmlFree(name);
	*str = ptr;
	return(NULL);
    }
    ptr++;

    *str = ptr;
    return(name);
}

/**
 * Parse a parameter entity reference. Always consumes '%'.
 *
 * The entity content is handled directly by pushing it's content as
 * a new input stream.
 *
 *     [69] PEReference ::= '%' Name ';'
 *
 * [ WFC: No Recursion ]
 * A parsed entity must not contain a recursive
 * reference to itself, either directly or indirectly.
 *
 * [ WFC: Entity Declared ]
 * In a document without any DTD, a document with only an internal DTD
 * subset which contains no parameter entity references, or a document
 * with "standalone='yes'", ...  ... The declaration of a parameter
 * entity must precede any reference to it...
 *
 * [ VC: Entity Declared ]
 * In a document with an external subset or external parameter entities
 * with "standalone='no'", ...  ... The declaration of a parameter entity
 * must precede any reference to it...
 *
 * [ WFC: In DTD ]
 * Parameter-entity references may only appear in the DTD.
 * NOTE: misleading but this is handled.
 *
 * @param ctxt  an XML parser context
 * @param markupDecl  whether the PERef starts a markup declaration
 */
static void
xmlParsePERefInternal(xmlParserCtxt *ctxt, int markupDecl) {
    const xmlChar *name;
    xmlEntityPtr entity = NULL;
    xmlParserInputPtr input;

    if (RAW != '%')
        return;
    NEXT;
    name = xmlParseName(ctxt);
    if (name == NULL) {
	xmlFatalErrMsg(ctxt, XML_ERR_PEREF_NO_NAME, "PEReference: no name\n");
	return;
    }
    if (RAW != ';') {
	xmlFatalErr(ctxt, XML_ERR_PEREF_SEMICOL_MISSING, NULL);
        return;
    }

    NEXT;

    /* Must be set before xmlHandleUndeclaredEntity */
    ctxt->hasPErefs = 1;

    /*
     * Request the entity from SAX
     */
    if ((ctxt->sax != NULL) &&
	(ctxt->sax->getParameterEntity != NULL))
	entity = ctxt->sax->getParameterEntity(ctxt->userData, name);

    if (entity == NULL) {
        xmlHandleUndeclaredEntity(ctxt, name);
    } else {
	/*
	 * Internal checking in case the entity quest barfed
	 */
	if ((entity->etype != XML_INTERNAL_PARAMETER_ENTITY) &&
	    (entity->etype != XML_EXTERNAL_PARAMETER_ENTITY)) {
	    xmlWarningMsg(ctxt, XML_WAR_UNDECLARED_ENTITY,
		  "Internal: %%%s; is not a parameter entity\n",
			  name, NULL);
	} else {
	    if ((entity->etype == XML_EXTERNAL_PARAMETER_ENTITY) &&
                ((ctxt->options & XML_PARSE_NO_XXE) ||
		 (((ctxt->loadsubset & ~XML_SKIP_IDS) == 0) &&
		  (ctxt->replaceEntities == 0) &&
		  (ctxt->validate == 0))))
		return;

            if (entity->flags & XML_ENT_EXPANDING) {
                xmlFatalErr(ctxt, XML_ERR_ENTITY_LOOP, NULL);
                return;
            }

	    input = xmlNewEntityInputStream(ctxt, entity);
	    if (xmlCtxtPushInput(ctxt, input) < 0) {
                xmlFreeInputStream(input);
		return;
            }

            entity->flags |= XML_ENT_EXPANDING;

            if (markupDecl)
                input->flags |= XML_INPUT_MARKUP_DECL;

            GROW;

	    if (entity->etype == XML_EXTERNAL_PARAMETER_ENTITY) {
                xmlDetectEncoding(ctxt);

                if ((CMP5(CUR_PTR, '<', '?', 'x', 'm', 'l')) &&
                    (IS_BLANK_CH(NXT(5)))) {
                    xmlParseTextDecl(ctxt);
                }
            }
	}
    }
}

/**
 * Parse a parameter entity reference.
 *
 * @deprecated Internal function, don't use.
 *
 * @param ctxt  an XML parser context
 */
void
xmlParsePEReference(xmlParserCtxt *ctxt) {
    xmlParsePERefInternal(ctxt, 0);
}

/**
 * Load the content of an entity.
 *
 * @param ctxt  an XML parser context
 * @param entity  an unloaded system entity
 * @returns 0 in case of success and -1 in case of failure
 */
static int
xmlLoadEntityContent(xmlParserCtxtPtr ctxt, xmlEntityPtr entity) {
    xmlParserInputPtr oldinput, input = NULL;
    xmlParserInputPtr *oldinputTab;
    xmlChar *oldencoding;
    xmlChar *content = NULL;
    xmlResourceType rtype;
    size_t length, i;
    int oldinputNr, oldinputMax;
    int ret = -1;
    int res;

    if ((ctxt == NULL) || (entity == NULL) ||
        ((entity->etype != XML_EXTERNAL_PARAMETER_ENTITY) &&
	 (entity->etype != XML_EXTERNAL_GENERAL_PARSED_ENTITY)) ||
	(entity->content != NULL)) {
	xmlFatalErr(ctxt, XML_ERR_ARGUMENT,
	            "xmlLoadEntityContent parameter error");
        return(-1);
    }

    if (entity->etype == XML_EXTERNAL_PARAMETER_ENTITY)
        rtype = XML_RESOURCE_PARAMETER_ENTITY;
    else
        rtype = XML_RESOURCE_GENERAL_ENTITY;

    input = xmlLoadResource(ctxt, (char *) entity->URI,
                            (char *) entity->ExternalID, rtype);
    if (input == NULL)
        return(-1);

    oldinput = ctxt->input;
    oldinputNr = ctxt->inputNr;
    oldinputMax = ctxt->inputMax;
    oldinputTab = ctxt->inputTab;
    oldencoding = ctxt->encoding;

    ctxt->input = NULL;
    ctxt->inputNr = 0;
    ctxt->inputMax = 1;
    ctxt->encoding = NULL;
    ctxt->inputTab = xmlMalloc(sizeof(xmlParserInputPtr));
    if (ctxt->inputTab == NULL) {
        xmlErrMemory(ctxt);
        xmlFreeInputStream(input);
        goto error;
    }

    xmlBufResetInput(input->buf->buffer, input);

    if (xmlCtxtPushInput(ctxt, input) < 0) {
        xmlFreeInputStream(input);
        goto error;
    }

    xmlDetectEncoding(ctxt);

    /*
     * Parse a possible text declaration first
     */
    if ((CMP5(CUR_PTR, '<', '?', 'x', 'm', 'l')) && (IS_BLANK_CH(NXT(5)))) {
	xmlParseTextDecl(ctxt);
        /*
         * An XML-1.0 document can't reference an entity not XML-1.0
         */
        if ((xmlStrEqual(ctxt->version, BAD_CAST "1.0")) &&
            (!xmlStrEqual(ctxt->input->version, BAD_CAST "1.0"))) {
            xmlFatalErrMsg(ctxt, XML_ERR_VERSION_MISMATCH,
                           "Version mismatch between document and entity\n");
        }
    }

    length = input->cur - input->base;
    xmlBufShrink(input->buf->buffer, length);
    xmlSaturatedAdd(&ctxt->sizeentities, length);

    while ((res = xmlParserInputBufferGrow(input->buf, 4096)) > 0)
        ;

    xmlBufResetInput(input->buf->buffer, input);

    if (res < 0) {
        xmlCtxtErrIO(ctxt, input->buf->error, NULL);
        goto error;
    }

    length = xmlBufUse(input->buf->buffer);
    if (length > INT_MAX) {
        xmlErrMemory(ctxt);
        goto error;
    }

    content = xmlStrndup(xmlBufContent(input->buf->buffer), length);
    if (content == NULL) {
        xmlErrMemory(ctxt);
        goto error;
    }

    for (i = 0; i < length; ) {
        int clen = length - i;
        int c = xmlGetUTF8Char(content + i, &clen);

        if ((c < 0) || (!IS_CHAR(c))) {
            xmlFatalErrMsgInt(ctxt, XML_ERR_INVALID_CHAR,
                              "xmlLoadEntityContent: invalid char value %d\n",
                              content[i]);
            goto error;
        }
        i += clen;
    }

    xmlSaturatedAdd(&ctxt->sizeentities, length);
    entity->content = content;
    entity->length = length;
    content = NULL;
    ret = 0;

error:
    while (ctxt->inputNr > 0)
        xmlFreeInputStream(xmlCtxtPopInput(ctxt));
    xmlFree(ctxt->inputTab);
    xmlFree(ctxt->encoding);

    ctxt->input = oldinput;
    ctxt->inputNr = oldinputNr;
    ctxt->inputMax = oldinputMax;
    ctxt->inputTab = oldinputTab;
    ctxt->encoding = oldencoding;

    xmlFree(content);

    return(ret);
}

/**
 * Parse PEReference declarations
 *
 *     [69] PEReference ::= '%' Name ';'
 *
 * [ WFC: No Recursion ]
 * A parsed entity must not contain a recursive
 * reference to itself, either directly or indirectly.
 *
 * [ WFC: Entity Declared ]
 * In a document without any DTD, a document with only an internal DTD
 * subset which contains no parameter entity references, or a document
 * with "standalone='yes'", ...  ... The declaration of a parameter
 * entity must precede any reference to it...
 *
 * [ VC: Entity Declared ]
 * In a document with an external subset or external parameter entities
 * with "standalone='no'", ...  ... The declaration of a parameter entity
 * must precede any reference to it...
 *
 * [ WFC: In DTD ]
 * Parameter-entity references may only appear in the DTD.
 * NOTE: misleading but this is handled.
 *
 * @param ctxt  an XML parser context
 * @param str  a pointer to an index in the string
 * @returns the string of the entity content.
 *         str is updated to the current value of the index
 */
static xmlEntityPtr
xmlParseStringPEReference(xmlParserCtxtPtr ctxt, const xmlChar **str) {
    const xmlChar *ptr;
    xmlChar cur;
    xmlChar *name;
    xmlEntityPtr entity = NULL;

    if ((str == NULL) || (*str == NULL)) return(NULL);
    ptr = *str;
    cur = *ptr;
    if (cur != '%')
        return(NULL);
    ptr++;
    name = xmlParseStringName(ctxt, &ptr);
    if (name == NULL) {
	xmlFatalErrMsg(ctxt, XML_ERR_NAME_REQUIRED,
		       "xmlParseStringPEReference: no name\n");
	*str = ptr;
	return(NULL);
    }
    cur = *ptr;
    if (cur != ';') {
	xmlFatalErr(ctxt, XML_ERR_ENTITYREF_SEMICOL_MISSING, NULL);
	xmlFree(name);
	*str = ptr;
	return(NULL);
    }
    ptr++;

    /* Must be set before xmlHandleUndeclaredEntity */
    ctxt->hasPErefs = 1;

    /*
     * Request the entity from SAX
     */
    if ((ctxt->sax != NULL) &&
	(ctxt->sax->getParameterEntity != NULL))
	entity = ctxt->sax->getParameterEntity(ctxt->userData, name);

    if (entity == NULL) {
        xmlHandleUndeclaredEntity(ctxt, name);
    } else {
	/*
	 * Internal checking in case the entity quest barfed
	 */
	if ((entity->etype != XML_INTERNAL_PARAMETER_ENTITY) &&
	    (entity->etype != XML_EXTERNAL_PARAMETER_ENTITY)) {
	    xmlWarningMsg(ctxt, XML_WAR_UNDECLARED_ENTITY,
			  "%%%s; is not a parameter entity\n",
			  name, NULL);
	}
    }

    xmlFree(name);
    *str = ptr;
    return(entity);
}

/**
 * Parse a DOCTYPE declaration
 *
 * @deprecated Internal function, don't use.
 *
 *     [28] doctypedecl ::= '<!DOCTYPE' S Name (S ExternalID)? S?
 *                          ('[' (markupdecl | PEReference | S)* ']' S?)? '>'
 *
 * [ VC: Root Element Type ]
 * The Name in the document type declaration must match the element
 * type of the root element.
 *
 * @param ctxt  an XML parser context
 */

void
xmlParseDocTypeDecl(xmlParserCtxt *ctxt) {
    const xmlChar *name = NULL;
    xmlChar *publicId = NULL;
    xmlChar *URI = NULL;

    /*
     * We know that '<!DOCTYPE' has been detected.
     */
    SKIP(9);

    if (SKIP_BLANKS == 0) {
        xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED,
                       "Space required after 'DOCTYPE'\n");
    }

    /*
     * Parse the DOCTYPE name.
     */
    name = xmlParseName(ctxt);
    if (name == NULL) {
	xmlFatalErrMsg(ctxt, XML_ERR_NAME_REQUIRED,
		       "xmlParseDocTypeDecl : no DOCTYPE name !\n");
    }
    ctxt->intSubName = name;

    SKIP_BLANKS;

    /*
     * Check for public and system identifier (URI)
     */
    URI = xmlParseExternalID(ctxt, &publicId, 1);

    if ((URI != NULL) || (publicId != NULL)) {
        ctxt->hasExternalSubset = 1;
    }
    ctxt->extSubURI = URI;
    ctxt->extSubSystem = publicId;

    SKIP_BLANKS;

    /*
     * Create and update the internal subset.
     */
    if ((ctxt->sax != NULL) && (ctxt->sax->internalSubset != NULL) &&
	(!ctxt->disableSAX))
	ctxt->sax->internalSubset(ctxt->userData, name, publicId, URI);

    if ((RAW != '[') && (RAW != '>')) {
	xmlFatalErr(ctxt, XML_ERR_DOCTYPE_NOT_FINISHED, NULL);
    }
}

/**
 * Parse the internal subset declaration
 *
 *     [28 end] ('[' (markupdecl | PEReference | S)* ']' S?)? '>'
 * @param ctxt  an XML parser context
 */

static void
xmlParseInternalSubset(xmlParserCtxtPtr ctxt) {
    /*
     * Is there any DTD definition ?
     */
    if (RAW == '[') {
        int oldInputNr = ctxt->inputNr;

        NEXT;
	/*
	 * Parse the succession of Markup declarations and
	 * PEReferences.
	 * Subsequence (markupdecl | PEReference | S)*
	 */
	SKIP_BLANKS;
        while (1) {
            if (PARSER_STOPPED(ctxt)) {
                return;
            } else if (ctxt->input->cur >= ctxt->input->end) {
                if (ctxt->inputNr <= oldInputNr) {
                xmlFatalErr(ctxt, XML_ERR_INT_SUBSET_NOT_FINISHED, NULL);
                    return;
                }
                xmlPopPE(ctxt);
            } else if ((RAW == ']') && (ctxt->inputNr <= oldInputNr)) {
                NEXT;
                SKIP_BLANKS;
                break;
            } else if ((PARSER_EXTERNAL(ctxt)) &&
                       (RAW == '<') && (NXT(1) == '!') && (NXT(2) == '[')) {
                /*
                 * Conditional sections are allowed in external entities
                 * included by PE References in the internal subset.
                 */
                xmlParseConditionalSections(ctxt);
            } else if ((RAW == '<') && ((NXT(1) == '!') || (NXT(1) == '?'))) {
                xmlParseMarkupDecl(ctxt);
            } else if (RAW == '%') {
                xmlParsePERefInternal(ctxt, 1);
            } else {
                xmlFatalErr(ctxt, XML_ERR_INT_SUBSET_NOT_FINISHED, NULL);

                while (ctxt->inputNr > oldInputNr)
                    xmlPopPE(ctxt);
                return;
            }
            SKIP_BLANKS;
            SHRINK;
            GROW;
        }
    }

    /*
     * We should be at the end of the DOCTYPE declaration.
     */
    if (RAW != '>') {
        xmlFatalErr(ctxt, XML_ERR_DOCTYPE_NOT_FINISHED, NULL);
        return;
    }
    NEXT;
}

#ifdef LIBXML_SAX1_ENABLED
/**
 * Parse an attribute
 *
 * @deprecated Internal function, don't use.
 *
 *     [41] Attribute ::= Name Eq AttValue
 *
 * [ WFC: No External Entity References ]
 * Attribute values cannot contain direct or indirect entity references
 * to external entities.
 *
 * [ WFC: No < in Attribute Values ]
 * The replacement text of any entity referred to directly or indirectly in
 * an attribute value (other than "&lt;") must not contain a <.
 *
 * [ VC: Attribute Value Type ]
 * The attribute must have been declared; the value must be of the type
 * declared for it.
 *
 *     [25] Eq ::= S? '=' S?
 *
 * With namespace:
 *
 *     [NS 11] Attribute ::= QName Eq AttValue
 *
 * Also the case QName == xmlns:??? is handled independently as a namespace
 * definition.
 *
 * @param ctxt  an XML parser context
 * @param value  a xmlChar ** used to store the value of the attribute
 * @returns the attribute name, and the value in *value.
 */

const xmlChar *
xmlParseAttribute(xmlParserCtxt *ctxt, xmlChar **value) {
    const xmlChar *name;
    xmlChar *val;

    *value = NULL;
    GROW;
    name = xmlParseName(ctxt);
    if (name == NULL) {
	xmlFatalErrMsg(ctxt, XML_ERR_NAME_REQUIRED,
	               "error parsing attribute name\n");
        return(NULL);
    }

    /*
     * read the value
     */
    SKIP_BLANKS;
    if (RAW == '=') {
        NEXT;
	SKIP_BLANKS;
	val = xmlParseAttValue(ctxt);
    } else {
	xmlFatalErrMsgStr(ctxt, XML_ERR_ATTRIBUTE_WITHOUT_VALUE,
	       "Specification mandates value for attribute %s\n", name);
	return(name);
    }

    /*
     * Check that xml:lang conforms to the specification
     * No more registered as an error, just generate a warning now
     * since this was deprecated in XML second edition
     */
    if ((ctxt->pedantic) && (xmlStrEqual(name, BAD_CAST "xml:lang"))) {
	if (!xmlCheckLanguageID(val)) {
	    xmlWarningMsg(ctxt, XML_WAR_LANG_VALUE,
		          "Malformed value for xml:lang : %s\n",
			  val, NULL);
	}
    }

    /*
     * Check that xml:space conforms to the specification
     */
    if (xmlStrEqual(name, BAD_CAST "xml:space")) {
	if (xmlStrEqual(val, BAD_CAST "default"))
	    *(ctxt->space) = 0;
	else if (xmlStrEqual(val, BAD_CAST "preserve"))
	    *(ctxt->space) = 1;
	else {
		xmlWarningMsg(ctxt, XML_WAR_SPACE_VALUE,
"Invalid value \"%s\" for xml:space : \"default\" or \"preserve\" expected\n",
                                 val, NULL);
	}
    }

    *value = val;
    return(name);
}

/**
 * Parse a start tag. Always consumes '<'.
 *
 * @deprecated Internal function, don't use.
 *
 *     [40] STag ::= '<' Name (S Attribute)* S? '>'
 *
 * [ WFC: Unique Att Spec ]
 * No attribute name may appear more than once in the same start-tag or
 * empty-element tag.
 *
 *     [44] EmptyElemTag ::= '<' Name (S Attribute)* S? '/>'
 *
 * [ WFC: Unique Att Spec ]
 * No attribute name may appear more than once in the same start-tag or
 * empty-element tag.
 *
 * With namespace:
 *
 *     [NS 8] STag ::= '<' QName (S Attribute)* S? '>'
 *
 *     [NS 10] EmptyElement ::= '<' QName (S Attribute)* S? '/>'
 *
 * @param ctxt  an XML parser context
 * @returns the element name parsed
 */

const xmlChar *
xmlParseStartTag(xmlParserCtxt *ctxt) {
    const xmlChar *name;
    const xmlChar *attname;
    xmlChar *attvalue;
    const xmlChar **atts = ctxt->atts;
    int nbatts = 0;
    int maxatts = ctxt->maxatts;
    int i;

    if (RAW != '<') return(NULL);
    NEXT1;

    name = xmlParseName(ctxt);
    if (name == NULL) {
	xmlFatalErrMsg(ctxt, XML_ERR_NAME_REQUIRED,
	     "xmlParseStartTag: invalid element name\n");
        return(NULL);
    }

    /*
     * Now parse the attributes, it ends up with the ending
     *
     * (S Attribute)* S?
     */
    SKIP_BLANKS;
    GROW;

    while (((RAW != '>') &&
	   ((RAW != '/') || (NXT(1) != '>')) &&
	   (IS_BYTE_CHAR(RAW))) && (PARSER_STOPPED(ctxt) == 0)) {
	attname = xmlParseAttribute(ctxt, &attvalue);
        if (attname == NULL)
	    break;
        if (attvalue != NULL) {
	    /*
	     * [ WFC: Unique Att Spec ]
	     * No attribute name may appear more than once in the same
	     * start-tag or empty-element tag.
	     */
	    for (i = 0; i < nbatts;i += 2) {
	        if (xmlStrEqual(atts[i], attname)) {
		    xmlErrAttributeDup(ctxt, NULL, attname);
		    goto failed;
		}
	    }
	    /*
	     * Add the pair to atts
	     */
	    if (nbatts + 4 > maxatts) {
	        const xmlChar **n;
                int newSize;

                newSize = xmlGrowCapacity(maxatts, sizeof(n[0]) * 2,
                                          11, XML_MAX_ATTRS);
                if (newSize < 0) {
		    xmlErrMemory(ctxt);
		    goto failed;
		}
#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
                if (newSize < 2)
                    newSize = 2;
#endif
	        n = xmlRealloc(atts, newSize * sizeof(n[0]) * 2);
		if (n == NULL) {
		    xmlErrMemory(ctxt);
		    goto failed;
		}
		atts = n;
                maxatts = newSize * 2;
		ctxt->atts = atts;
		ctxt->maxatts = maxatts;
	    }

	    atts[nbatts++] = attname;
	    atts[nbatts++] = attvalue;
	    atts[nbatts] = NULL;
	    atts[nbatts + 1] = NULL;

            attvalue = NULL;
	}

failed:

        if (attvalue != NULL)
            xmlFree(attvalue);

	GROW
	if ((RAW == '>') || (((RAW == '/') && (NXT(1) == '>'))))
	    break;
	if (SKIP_BLANKS == 0) {
	    xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED,
			   "attributes construct error\n");
	}
	SHRINK;
        GROW;
    }

    /*
     * SAX: Start of Element !
     */
    if ((ctxt->sax != NULL) && (ctxt->sax->startElement != NULL) &&
	(!ctxt->disableSAX)) {
	if (nbatts > 0)
	    ctxt->sax->startElement(ctxt->userData, name, atts);
	else
	    ctxt->sax->startElement(ctxt->userData, name, NULL);
    }

    if (atts != NULL) {
        /* Free only the content strings */
        for (i = 1;i < nbatts;i+=2)
	    if (atts[i] != NULL)
	       xmlFree((xmlChar *) atts[i]);
    }
    return(name);
}

/**
 * Parse an end tag. Always consumes '</'.
 *
 *     [42] ETag ::= '</' Name S? '>'
 *
 * With namespace
 *
 *     [NS 9] ETag ::= '</' QName S? '>'
 * @param ctxt  an XML parser context
 * @param line  line of the start tag
 */

static void
xmlParseEndTag1(xmlParserCtxtPtr ctxt, int line) {
    const xmlChar *name;

    GROW;
    if ((RAW != '<') || (NXT(1) != '/')) {
	xmlFatalErrMsg(ctxt, XML_ERR_LTSLASH_REQUIRED,
		       "xmlParseEndTag: '</' not found\n");
	return;
    }
    SKIP(2);

    name = xmlParseNameAndCompare(ctxt,ctxt->name);

    /*
     * We should definitely be at the ending "S? '>'" part
     */
    GROW;
    SKIP_BLANKS;
    if ((!IS_BYTE_CHAR(RAW)) || (RAW != '>')) {
	xmlFatalErr(ctxt, XML_ERR_GT_REQUIRED, NULL);
    } else
	NEXT1;

    /*
     * [ WFC: Element Type Match ]
     * The Name in an element's end-tag must match the element type in the
     * start-tag.
     *
     */
    if (name != (xmlChar*)1) {
        if (name == NULL) name = BAD_CAST "unparsable";
        xmlFatalErrMsgStrIntStr(ctxt, XML_ERR_TAG_NAME_MISMATCH,
		     "Opening and ending tag mismatch: %s line %d and %s\n",
		                ctxt->name, line, name);
    }

    /*
     * SAX: End of Tag
     */
    if ((ctxt->sax != NULL) && (ctxt->sax->endElement != NULL) &&
	(!ctxt->disableSAX))
        ctxt->sax->endElement(ctxt->userData, ctxt->name);

    namePop(ctxt);
    spacePop(ctxt);
}

/**
 * Parse an end of tag
 *
 * @deprecated Internal function, don't use.
 *
 *     [42] ETag ::= '</' Name S? '>'
 *
 * With namespace
 *
 *     [NS 9] ETag ::= '</' QName S? '>'
 * @param ctxt  an XML parser context
 */

void
xmlParseEndTag(xmlParserCtxt *ctxt) {
    xmlParseEndTag1(ctxt, 0);
}
#endif /* LIBXML_SAX1_ENABLED */

/************************************************************************
 *									*
 *		      SAX 2 specific operations				*
 *									*
 ************************************************************************/

/**
 * Parse an XML Namespace QName
 *
 *     [6]  QName  ::= (Prefix ':')? LocalPart
 *     [7]  Prefix  ::= NCName
 *     [8]  LocalPart  ::= NCName
 *
 * @param ctxt  an XML parser context
 * @param prefix  pointer to store the prefix part
 * @returns the Name parsed or NULL
 */

static xmlHashedString
xmlParseQNameHashed(xmlParserCtxtPtr ctxt, xmlHashedString *prefix) {
    xmlHashedString l, p;
    int start, isNCName = 0;

    l.name = NULL;
    p.name = NULL;

    GROW;
    start = CUR_PTR - BASE_PTR;

    l = xmlParseNCName(ctxt);
    if (l.name != NULL) {
        isNCName = 1;
        if (CUR == ':') {
            NEXT;
            p = l;
            l = xmlParseNCName(ctxt);
        }
    }
    if ((l.name == NULL) || (CUR == ':')) {
        xmlChar *tmp;

        l.name = NULL;
        p.name = NULL;
        if ((isNCName == 0) && (CUR != ':'))
            return(l);
        tmp = xmlParseNmtoken(ctxt);
        if (tmp != NULL)
            xmlFree(tmp);
        l = xmlDictLookupHashed(ctxt->dict, BASE_PTR + start,
                                CUR_PTR - (BASE_PTR + start));
        if (l.name == NULL) {
            xmlErrMemory(ctxt);
            return(l);
        }
        xmlNsErr(ctxt, XML_NS_ERR_QNAME,
                 "Failed to parse QName '%s'\n", l.name, NULL, NULL);
    }

    *prefix = p;
    return(l);
}

/**
 * Parse an XML Namespace QName
 *
 *     [6]  QName  ::= (Prefix ':')? LocalPart
 *     [7]  Prefix  ::= NCName
 *     [8]  LocalPart  ::= NCName
 *
 * @param ctxt  an XML parser context
 * @param prefix  pointer to store the prefix part
 * @returns the Name parsed or NULL
 */

static const xmlChar *
xmlParseQName(xmlParserCtxtPtr ctxt, const xmlChar **prefix) {
    xmlHashedString n, p;

    n = xmlParseQNameHashed(ctxt, &p);
    if (n.name == NULL)
        return(NULL);
    *prefix = p.name;
    return(n.name);
}

/**
 * Parse an XML name and compares for match
 * (specialized for endtag parsing)
 *
 * @param ctxt  an XML parser context
 * @param name  the localname
 * @param prefix  the prefix, if any.
 * @returns NULL for an illegal name, (xmlChar*) 1 for success
 * and the name for mismatch
 */

static const xmlChar *
xmlParseQNameAndCompare(xmlParserCtxtPtr ctxt, xmlChar const *name,
                        xmlChar const *prefix) {
    const xmlChar *cmp;
    const xmlChar *in;
    const xmlChar *ret;
    const xmlChar *prefix2;

    if (prefix == NULL) return(xmlParseNameAndCompare(ctxt, name));

    GROW;
    in = ctxt->input->cur;

    cmp = prefix;
    while (*in != 0 && *in == *cmp) {
	++in;
	++cmp;
    }
    if ((*cmp == 0) && (*in == ':')) {
        in++;
	cmp = name;
	while (*in != 0 && *in == *cmp) {
	    ++in;
	    ++cmp;
	}
	if (*cmp == 0 && (*in == '>' || IS_BLANK_CH (*in))) {
	    /* success */
            ctxt->input->col += in - ctxt->input->cur;
	    ctxt->input->cur = in;
	    return((const xmlChar*) 1);
	}
    }
    /*
     * all strings coms from the dictionary, equality can be done directly
     */
    ret = xmlParseQName (ctxt, &prefix2);
    if (ret == NULL)
        return(NULL);
    if ((ret == name) && (prefix == prefix2))
	return((const xmlChar*) 1);
    return ret;
}

/**
 * Parse an attribute in the new SAX2 framework.
 *
 * @param ctxt  an XML parser context
 * @param pref  the element prefix
 * @param elem  the element name
 * @param hprefix  resulting attribute prefix
 * @param value  resulting value of the attribute
 * @param len  resulting length of the attribute
 * @param alloc  resulting indicator if the attribute was allocated
 * @returns the attribute name, and the value in *value, .
 */

static xmlHashedString
xmlParseAttribute2(xmlParserCtxtPtr ctxt,
                   const xmlChar * pref, const xmlChar * elem,
                   xmlHashedString * hprefix, xmlChar ** value,
                   int *len, int *alloc)
{
    xmlHashedString hname;
    const xmlChar *prefix, *name;
    xmlChar *val = NULL, *internal_val = NULL;
    int special = 0;
    int isNamespace;
    int flags;

    *value = NULL;
    GROW;
    hname = xmlParseQNameHashed(ctxt, hprefix);
    if (hname.name == NULL) {
        xmlFatalErrMsg(ctxt, XML_ERR_NAME_REQUIRED,
                       "error parsing attribute name\n");
        return(hname);
    }
    name = hname.name;
    prefix = hprefix->name;

    /*
     * get the type if needed
     */
    if (ctxt->attsSpecial != NULL) {
        special = XML_PTR_TO_INT(xmlHashQLookup2(ctxt->attsSpecial, pref, elem,
                                              prefix, name));
    }

    /*
     * read the value
     */
    SKIP_BLANKS;
    if (RAW != '=') {
        xmlFatalErrMsgStr(ctxt, XML_ERR_ATTRIBUTE_WITHOUT_VALUE,
                          "Specification mandates value for attribute %s\n",
                          name);
        goto error;
    }


    NEXT;
    SKIP_BLANKS;
    flags = 0;
    isNamespace = (((prefix == NULL) && (name == ctxt->str_xmlns)) ||
                   (prefix == ctxt->str_xmlns));
    val = xmlParseAttValueInternal(ctxt, len, &flags, special,
                                   isNamespace);
    if (val == NULL)
        goto error;

    *alloc = (flags & XML_ATTVAL_ALLOC) != 0;

#ifdef LIBXML_VALID_ENABLED
    if ((ctxt->validate) &&
        (ctxt->standalone == 1) &&
        (special & XML_SPECIAL_EXTERNAL) &&
        (flags & XML_ATTVAL_NORM_CHANGE)) {
        xmlValidityError(ctxt, XML_DTD_NOT_STANDALONE,
                         "standalone: normalization of attribute %s on %s "
                         "by external subset declaration\n",
                         name, elem);
    }
#endif

    if (prefix == ctxt->str_xml) {
        /*
         * Check that xml:lang conforms to the specification
         * No more registered as an error, just generate a warning now
         * since this was deprecated in XML second edition
         */
        if ((ctxt->pedantic) && (xmlStrEqual(name, BAD_CAST "lang"))) {
            internal_val = xmlStrndup(val, *len);
            if (internal_val == NULL)
                goto mem_error;
            if (!xmlCheckLanguageID(internal_val)) {
                xmlWarningMsg(ctxt, XML_WAR_LANG_VALUE,
                              "Malformed value for xml:lang : %s\n",
                              internal_val, NULL);
            }
        }

        /*
         * Check that xml:space conforms to the specification
         */
        if (xmlStrEqual(name, BAD_CAST "space")) {
            internal_val = xmlStrndup(val, *len);
            if (internal_val == NULL)
                goto mem_error;
            if (xmlStrEqual(internal_val, BAD_CAST "default"))
                *(ctxt->space) = 0;
            else if (xmlStrEqual(internal_val, BAD_CAST "preserve"))
                *(ctxt->space) = 1;
            else {
                xmlWarningMsg(ctxt, XML_WAR_SPACE_VALUE,
                              "Invalid value \"%s\" for xml:space : \"default\" or \"preserve\" expected\n",
                              internal_val, NULL);
            }
        }
        if (internal_val) {
            xmlFree(internal_val);
        }
    }

    *value = val;
    return (hname);

mem_error:
    xmlErrMemory(ctxt);
error:
    if ((val != NULL) && (*alloc != 0))
        xmlFree(val);
    return(hname);
}

/**
 * Inserts a new attribute into the hash table.
 *
 * @param ctxt  parser context
 * @param size  size of the hash table
 * @param name  attribute name
 * @param uri  namespace uri
 * @param hashValue  combined hash value of name and uri
 * @param aindex  attribute index (this is a multiple of 5)
 * @returns INT_MAX if no existing attribute was found, the attribute
 * index if an attribute was found, -1 if a memory allocation failed.
 */
static int
xmlAttrHashInsert(xmlParserCtxtPtr ctxt, unsigned size, const xmlChar *name,
                  const xmlChar *uri, unsigned hashValue, int aindex) {
    xmlAttrHashBucket *table = ctxt->attrHash;
    xmlAttrHashBucket *bucket;
    unsigned hindex;

    hindex = hashValue & (size - 1);
    bucket = &table[hindex];

    while (bucket->index >= 0) {
        const xmlChar **atts = &ctxt->atts[bucket->index];

        if (name == atts[0]) {
            int nsIndex = XML_PTR_TO_INT(atts[2]);

            if ((nsIndex == NS_INDEX_EMPTY) ? (uri == NULL) :
                (nsIndex == NS_INDEX_XML) ? (uri == ctxt->str_xml_ns) :
                (uri == ctxt->nsTab[nsIndex * 2 + 1]))
                return(bucket->index);
        }

        hindex++;
        bucket++;
        if (hindex >= size) {
            hindex = 0;
            bucket = table;
        }
    }

    bucket->index = aindex;

    return(INT_MAX);
}

static int
xmlAttrHashInsertQName(xmlParserCtxtPtr ctxt, unsigned size,
                       const xmlChar *name, const xmlChar *prefix,
                       unsigned hashValue, int aindex) {
    xmlAttrHashBucket *table = ctxt->attrHash;
    xmlAttrHashBucket *bucket;
    unsigned hindex;

    hindex = hashValue & (size - 1);
    bucket = &table[hindex];

    while (bucket->index >= 0) {
        const xmlChar **atts = &ctxt->atts[bucket->index];

        if ((name == atts[0]) && (prefix == atts[1]))
            return(bucket->index);

        hindex++;
        bucket++;
        if (hindex >= size) {
            hindex = 0;
            bucket = table;
        }
    }

    bucket->index = aindex;

    return(INT_MAX);
}
/**
 * Parse a start tag. Always consumes '<'.
 *
 * This routine is called when running SAX2 parsing
 *
 *     [40] STag ::= '<' Name (S Attribute)* S? '>'
 *
 * [ WFC: Unique Att Spec ]
 * No attribute name may appear more than once in the same start-tag or
 * empty-element tag.
 *
 *     [44] EmptyElemTag ::= '<' Name (S Attribute)* S? '/>'
 *
 * [ WFC: Unique Att Spec ]
 * No attribute name may appear more than once in the same start-tag or
 * empty-element tag.
 *
 * With namespace:
 *
 *     [NS 8] STag ::= '<' QName (S Attribute)* S? '>'
 *
 *     [NS 10] EmptyElement ::= '<' QName (S Attribute)* S? '/>'
 *
 * @param ctxt  an XML parser context
 * @param pref  resulting namespace prefix
 * @param URI  resulting namespace URI
 * @param nbNsPtr  resulting number of namespace declarations
 * @returns the element name parsed
 */

static const xmlChar *
xmlParseStartTag2(xmlParserCtxtPtr ctxt, const xmlChar **pref,
                  const xmlChar **URI, int *nbNsPtr) {
    xmlHashedString hlocalname;
    xmlHashedString hprefix;
    xmlHashedString hattname;
    xmlHashedString haprefix;
    const xmlChar *localname;
    const xmlChar *prefix;
    const xmlChar *attname;
    const xmlChar *aprefix;
    const xmlChar *uri;
    xmlChar *attvalue = NULL;
    const xmlChar **atts = ctxt->atts;
    unsigned attrHashSize = 0;
    int maxatts = ctxt->maxatts;
    int nratts, nbatts, nbdef;
    int i, j, nbNs, nbTotalDef, attval, nsIndex, maxAtts;
    int alloc = 0;
    int numNsErr = 0;
    int numDupErr = 0;

    if (RAW != '<') return(NULL);
    NEXT1;

    nbatts = 0;
    nratts = 0;
    nbdef = 0;
    nbNs = 0;
    nbTotalDef = 0;
    attval = 0;

    if (xmlParserNsStartElement(ctxt->nsdb) < 0) {
        xmlErrMemory(ctxt);
        return(NULL);
    }

    hlocalname = xmlParseQNameHashed(ctxt, &hprefix);
    if (hlocalname.name == NULL) {
	xmlFatalErrMsg(ctxt, XML_ERR_NAME_REQUIRED,
		       "StartTag: invalid element name\n");
        return(NULL);
    }
    localname = hlocalname.name;
    prefix = hprefix.name;

    /*
     * Now parse the attributes, it ends up with the ending
     *
     * (S Attribute)* S?
     */
    SKIP_BLANKS;
    GROW;

    /*
     * The ctxt->atts array will be ultimately passed to the SAX callback
     * containing five xmlChar pointers for each attribute:
     *
     * [0] attribute name
     * [1] attribute prefix
     * [2] namespace URI
     * [3] attribute value
     * [4] end of attribute value
     *
     * To save memory, we reuse this array temporarily and store integers
     * in these pointer variables.
     *
     * [0] attribute name
     * [1] attribute prefix
     * [2] hash value of attribute prefix, and later namespace index
     * [3] for non-allocated values: ptrdiff_t offset into input buffer
     * [4] for non-allocated values: ptrdiff_t offset into input buffer
     *
     * The ctxt->attallocs array contains an additional unsigned int for
     * each attribute, containing the hash value of the attribute name
     * and the alloc flag in bit 31.
     */

    while (((RAW != '>') &&
	   ((RAW != '/') || (NXT(1) != '>')) &&
	   (IS_BYTE_CHAR(RAW))) && (PARSER_STOPPED(ctxt) == 0)) {
	int len = -1;

	hattname = xmlParseAttribute2(ctxt, prefix, localname,
                                          &haprefix, &attvalue, &len,
                                          &alloc);
        if (hattname.name == NULL)
	    break;
        if (attvalue == NULL)
            goto next_attr;
        attname = hattname.name;
        aprefix = haprefix.name;
	if (len < 0) len = xmlStrlen(attvalue);

        if ((attname == ctxt->str_xmlns) && (aprefix == NULL)) {
            xmlHashedString huri;
            xmlURIPtr parsedUri;

            huri = xmlDictLookupHashed(ctxt->dict, attvalue, len);
            uri = huri.name;
            if (uri == NULL) {
                xmlErrMemory(ctxt);
                goto next_attr;
            }
            if (*uri != 0) {
                if (xmlParseURISafe((const char *) uri, &parsedUri) < 0) {
                    xmlErrMemory(ctxt);
                    goto next_attr;
                }
                if (parsedUri == NULL) {
                    xmlNsErr(ctxt, XML_WAR_NS_URI,
                             "xmlns: '%s' is not a valid URI\n",
                                       uri, NULL, NULL);
                } else {
                    if (parsedUri->scheme == NULL) {
                        xmlNsWarn(ctxt, XML_WAR_NS_URI_RELATIVE,
                                  "xmlns: URI %s is not absolute\n",
                                  uri, NULL, NULL);
                    }
                    xmlFreeURI(parsedUri);
                }
                if (uri == ctxt->str_xml_ns) {
                    if (attname != ctxt->str_xml) {
                        xmlNsErr(ctxt, XML_NS_ERR_XML_NAMESPACE,
                     "xml namespace URI cannot be the default namespace\n",
                                 NULL, NULL, NULL);
                    }
                    goto next_attr;
                }
                if ((len == 29) &&
                    (xmlStrEqual(uri,
                             BAD_CAST "http://www.w3.org/2000/xmlns/"))) {
                    xmlNsErr(ctxt, XML_NS_ERR_XML_NAMESPACE,
                         "reuse of the xmlns namespace name is forbidden\n",
                             NULL, NULL, NULL);
                    goto next_attr;
                }
            }

            if (xmlParserNsPush(ctxt, NULL, &huri, NULL, 0) > 0)
                nbNs++;
        } else if (aprefix == ctxt->str_xmlns) {
            xmlHashedString huri;
            xmlURIPtr parsedUri;

            huri = xmlDictLookupHashed(ctxt->dict, attvalue, len);
            uri = huri.name;
            if (uri == NULL) {
                xmlErrMemory(ctxt);
                goto next_attr;
            }

            if (attname == ctxt->str_xml) {
                if (uri != ctxt->str_xml_ns) {
                    xmlNsErr(ctxt, XML_NS_ERR_XML_NAMESPACE,
                             "xml namespace prefix mapped to wrong URI\n",
                             NULL, NULL, NULL);
                }
                /*
                 * Do not keep a namespace definition node
                 */
                goto next_attr;
            }
            if (uri == ctxt->str_xml_ns) {
                if (attname != ctxt->str_xml) {
                    xmlNsErr(ctxt, XML_NS_ERR_XML_NAMESPACE,
                             "xml namespace URI mapped to wrong prefix\n",
                             NULL, NULL, NULL);
                }
                goto next_attr;
            }
            if (attname == ctxt->str_xmlns) {
                xmlNsErr(ctxt, XML_NS_ERR_XML_NAMESPACE,
                         "redefinition of the xmlns prefix is forbidden\n",
                         NULL, NULL, NULL);
                goto next_attr;
            }
            if ((len == 29) &&
                (xmlStrEqual(uri,
                             BAD_CAST "http://www.w3.org/2000/xmlns/"))) {
                xmlNsErr(ctxt, XML_NS_ERR_XML_NAMESPACE,
                         "reuse of the xmlns namespace name is forbidden\n",
                         NULL, NULL, NULL);
                goto next_attr;
            }
            if ((uri == NULL) || (uri[0] == 0)) {
                xmlNsErr(ctxt, XML_NS_ERR_XML_NAMESPACE,
                         "xmlns:%s: Empty XML namespace is not allowed\n",
                              attname, NULL, NULL);
                goto next_attr;
            } else {
                if (xmlParseURISafe((const char *) uri, &parsedUri) < 0) {
                    xmlErrMemory(ctxt);
                    goto next_attr;
                }
                if (parsedUri == NULL) {
                    xmlNsErr(ctxt, XML_WAR_NS_URI,
                         "xmlns:%s: '%s' is not a valid URI\n",
                                       attname, uri, NULL);
                } else {
                    if ((ctxt->pedantic) && (parsedUri->scheme == NULL)) {
                        xmlNsWarn(ctxt, XML_WAR_NS_URI_RELATIVE,
                                  "xmlns:%s: URI %s is not absolute\n",
                                  attname, uri, NULL);
                    }
                    xmlFreeURI(parsedUri);
                }
            }

            if (xmlParserNsPush(ctxt, &hattname, &huri, NULL, 0) > 0)
                nbNs++;
        } else {
            /*
             * Populate attributes array, see above for repurposing
             * of xmlChar pointers.
             */
            if ((atts == NULL) || (nbatts + 5 > maxatts)) {
                int res = xmlCtxtGrowAttrs(ctxt);

                maxatts = ctxt->maxatts;
                atts = ctxt->atts;

                if (res < 0)
                    goto next_attr;
            }
            ctxt->attallocs[nratts++] = (hattname.hashValue & 0x7FFFFFFF) |
                                        ((unsigned) alloc << 31);
            atts[nbatts++] = attname;
            atts[nbatts++] = aprefix;
            atts[nbatts++] = XML_INT_TO_PTR(haprefix.hashValue);
            if (alloc) {
                atts[nbatts++] = attvalue;
                attvalue += len;
                atts[nbatts++] = attvalue;
            } else {
                /*
                 * attvalue points into the input buffer which can be
                 * reallocated. Store differences to input->base instead.
                 * The pointers will be reconstructed later.
                 */
                atts[nbatts++] = XML_INT_TO_PTR(attvalue - BASE_PTR);
                attvalue += len;
                atts[nbatts++] = XML_INT_TO_PTR(attvalue - BASE_PTR);
            }
            /*
             * tag if some deallocation is needed
             */
            if (alloc != 0) attval = 1;
            attvalue = NULL; /* moved into atts */
        }

next_attr:
        if ((attvalue != NULL) && (alloc != 0)) {
            xmlFree(attvalue);
            attvalue = NULL;
        }

	GROW
	if ((RAW == '>') || (((RAW == '/') && (NXT(1) == '>'))))
	    break;
	if (SKIP_BLANKS == 0) {
	    xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED,
			   "attributes construct error\n");
	    break;
	}
        GROW;
    }

    /*
     * Namespaces from default attributes
     */
    if (ctxt->attsDefault != NULL) {
        xmlDefAttrsPtr defaults;

	defaults = xmlHashLookup2(ctxt->attsDefault, localname, prefix);
	if (defaults != NULL) {
	    for (i = 0; i < defaults->nbAttrs; i++) {
                xmlDefAttr *attr = &defaults->attrs[i];

	        attname = attr->name.name;
		aprefix = attr->prefix.name;

		if ((attname == ctxt->str_xmlns) && (aprefix == NULL)) {
                    xmlParserEntityCheck(ctxt, attr->expandedSize);

                    if (xmlParserNsPush(ctxt, NULL, &attr->value, NULL, 1) > 0)
                        nbNs++;
		} else if (aprefix == ctxt->str_xmlns) {
                    xmlParserEntityCheck(ctxt, attr->expandedSize);

                    if (xmlParserNsPush(ctxt, &attr->name, &attr->value,
                                      NULL, 1) > 0)
                        nbNs++;
		} else {
                    if (nratts + nbTotalDef >= XML_MAX_ATTRS) {
                        xmlFatalErr(ctxt, XML_ERR_RESOURCE_LIMIT,
                                    "Maximum number of attributes exceeded");
                        break;
                    }
                    nbTotalDef += 1;
                }
	    }
	}
    }

    /*
     * Resolve attribute namespaces
     */
    for (i = 0; i < nbatts; i += 5) {
        attname = atts[i];
        aprefix = atts[i+1];

        /*
	* The default namespace does not apply to attribute names.
	*/
	if (aprefix == NULL) {
            nsIndex = NS_INDEX_EMPTY;
        } else if (aprefix == ctxt->str_xml) {
            nsIndex = NS_INDEX_XML;
        } else {
            haprefix.name = aprefix;
            haprefix.hashValue = (size_t) atts[i+2];
            nsIndex = xmlParserNsLookup(ctxt, &haprefix, NULL);

	    if ((nsIndex == INT_MAX) || (nsIndex < ctxt->nsdb->minNsIndex)) {
                xmlNsErr(ctxt, XML_NS_ERR_UNDEFINED_NAMESPACE,
		    "Namespace prefix %s for %s on %s is not defined\n",
		    aprefix, attname, localname);
                nsIndex = NS_INDEX_EMPTY;
            }
        }

        atts[i+2] = XML_INT_TO_PTR(nsIndex);
    }

    /*
     * Maximum number of attributes including default attributes.
     */
    maxAtts = nratts + nbTotalDef;

    /*
     * Verify that attribute names are unique.
     */
    if (maxAtts > 1) {
        attrHashSize = 4;
        while (attrHashSize / 2 < (unsigned) maxAtts)
            attrHashSize *= 2;

        if (attrHashSize > ctxt->attrHashMax) {
            xmlAttrHashBucket *tmp;

            tmp = xmlRealloc(ctxt->attrHash, attrHashSize * sizeof(tmp[0]));
            if (tmp == NULL) {
                xmlErrMemory(ctxt);
                goto done;
            }

            ctxt->attrHash = tmp;
            ctxt->attrHashMax = attrHashSize;
        }

        memset(ctxt->attrHash, -1, attrHashSize * sizeof(ctxt->attrHash[0]));

        for (i = 0, j = 0; j < nratts; i += 5, j++) {
            const xmlChar *nsuri;
            unsigned hashValue, nameHashValue, uriHashValue;
            int res;

            attname = atts[i];
            aprefix = atts[i+1];
            nsIndex = XML_PTR_TO_INT(atts[i+2]);
            /* Hash values always have bit 31 set, see dict.c */
            nameHashValue = ctxt->attallocs[j] | 0x80000000;

            if (nsIndex == NS_INDEX_EMPTY) {
                /*
                 * Prefix with empty namespace means an undeclared
                 * prefix which was already reported above.
                 */
                if (aprefix != NULL)
                    continue;
                nsuri = NULL;
                uriHashValue = URI_HASH_EMPTY;
            } else if (nsIndex == NS_INDEX_XML) {
                nsuri = ctxt->str_xml_ns;
                uriHashValue = URI_HASH_XML;
            } else {
                nsuri = ctxt->nsTab[nsIndex * 2 + 1];
                uriHashValue = ctxt->nsdb->extra[nsIndex].uriHashValue;
            }

            hashValue = xmlDictCombineHash(nameHashValue, uriHashValue);
            res = xmlAttrHashInsert(ctxt, attrHashSize, attname, nsuri,
                                    hashValue, i);
            if (res < 0)
                continue;

            /*
             * [ WFC: Unique Att Spec ]
             * No attribute name may appear more than once in the same
             * start-tag or empty-element tag.
             * As extended by the Namespace in XML REC.
             */
            if (res < INT_MAX) {
                if (aprefix == atts[res+1]) {
                    xmlErrAttributeDup(ctxt, aprefix, attname);
                    numDupErr += 1;
                } else {
                    xmlNsErr(ctxt, XML_NS_ERR_ATTRIBUTE_REDEFINED,
                             "Namespaced Attribute %s in '%s' redefined\n",
                             attname, nsuri, NULL);
                    numNsErr += 1;
                }
            }
        }
    }

    /*
     * Default attributes
     */
    if (ctxt->attsDefault != NULL) {
        xmlDefAttrsPtr defaults;

	defaults = xmlHashLookup2(ctxt->attsDefault, localname, prefix);
	if (defaults != NULL) {
	    for (i = 0; i < defaults->nbAttrs; i++) {
                xmlDefAttr *attr = &defaults->attrs[i];
                const xmlChar *nsuri = NULL;
                unsigned hashValue, uriHashValue = 0;
                int res;

	        attname = attr->name.name;
		aprefix = attr->prefix.name;

		if ((attname == ctxt->str_xmlns) && (aprefix == NULL))
                    continue;
		if (aprefix == ctxt->str_xmlns)
                    continue;

                if (aprefix == NULL) {
                    nsIndex = NS_INDEX_EMPTY;
                    nsuri = NULL;
                    uriHashValue = URI_HASH_EMPTY;
                } else if (aprefix == ctxt->str_xml) {
                    nsIndex = NS_INDEX_XML;
                    nsuri = ctxt->str_xml_ns;
                    uriHashValue = URI_HASH_XML;
                } else {
                    nsIndex = xmlParserNsLookup(ctxt, &attr->prefix, NULL);
                    if ((nsIndex == INT_MAX) ||
                        (nsIndex < ctxt->nsdb->minNsIndex)) {
                        xmlNsErr(ctxt, XML_NS_ERR_UNDEFINED_NAMESPACE,
                                 "Namespace prefix %s for %s on %s is not "
                                 "defined\n",
                                 aprefix, attname, localname);
                        nsIndex = NS_INDEX_EMPTY;
                        nsuri = NULL;
                        uriHashValue = URI_HASH_EMPTY;
                    } else {
                        nsuri = ctxt->nsTab[nsIndex * 2 + 1];
                        uriHashValue = ctxt->nsdb->extra[nsIndex].uriHashValue;
                    }
                }

                /*
                 * Check whether the attribute exists
                 */
                if (maxAtts > 1) {
                    hashValue = xmlDictCombineHash(attr->name.hashValue,
                                                   uriHashValue);
                    res = xmlAttrHashInsert(ctxt, attrHashSize, attname, nsuri,
                                            hashValue, nbatts);
                    if (res < 0)
                        continue;
                    if (res < INT_MAX) {
                        if (aprefix == atts[res+1])
                            continue;
                        xmlNsErr(ctxt, XML_NS_ERR_ATTRIBUTE_REDEFINED,
                                 "Namespaced Attribute %s in '%s' redefined\n",
                                 attname, nsuri, NULL);
                    }
                }

                xmlParserEntityCheck(ctxt, attr->expandedSize);

                if ((atts == NULL) || (nbatts + 5 > maxatts)) {
                    res = xmlCtxtGrowAttrs(ctxt);

                    maxatts = ctxt->maxatts;
                    atts = ctxt->atts;

                    if (res < 0) {
                        localname = NULL;
                        goto done;
                    }
                }

                atts[nbatts++] = attname;
                atts[nbatts++] = aprefix;
                atts[nbatts++] = XML_INT_TO_PTR(nsIndex);
                atts[nbatts++] = attr->value.name;
                atts[nbatts++] = attr->valueEnd;

#ifdef LIBXML_VALID_ENABLED
                /*
                 * This should be moved to valid.c, but we don't keep track
                 * whether an attribute was defaulted.
                 */
                if ((ctxt->validate) &&
                    (ctxt->standalone == 1) &&
                    (attr->external != 0)) {
                    xmlValidityError(ctxt, XML_DTD_STANDALONE_DEFAULTED,
                            "standalone: attribute %s on %s defaulted "
                            "from external subset\n",
                            attname, localname);
                }
#endif
                nbdef++;
	    }
	}
    }

    /*
     * Using a single hash table for nsUri/localName pairs cannot
     * detect duplicate QNames reliably. The following example will
     * only result in two namespace errors.
     *
     * <doc xmlns:a="a" xmlns:b="a">
     *   <elem a:a="" b:a="" b:a=""/>
     * </doc>
     *
     * If we saw more than one namespace error but no duplicate QNames
     * were found, we have to scan for duplicate QNames.
     */
    if ((numDupErr == 0) && (numNsErr > 1)) {
        memset(ctxt->attrHash, -1,
               attrHashSize * sizeof(ctxt->attrHash[0]));

        for (i = 0, j = 0; j < nratts; i += 5, j++) {
            unsigned hashValue, nameHashValue, prefixHashValue;
            int res;

            aprefix = atts[i+1];
            if (aprefix == NULL)
                continue;

            attname = atts[i];
            /* Hash values always have bit 31 set, see dict.c */
            nameHashValue = ctxt->attallocs[j] | 0x80000000;
            prefixHashValue = xmlDictComputeHash(ctxt->dict, aprefix);

            hashValue = xmlDictCombineHash(nameHashValue, prefixHashValue);
            res = xmlAttrHashInsertQName(ctxt, attrHashSize, attname,
                                         aprefix, hashValue, i);
            if (res < INT_MAX)
                xmlErrAttributeDup(ctxt, aprefix, attname);
        }
    }

    /*
     * Reconstruct attribute pointers
     */
    for (i = 0, j = 0; i < nbatts; i += 5, j++) {
        /* namespace URI */
        nsIndex = XML_PTR_TO_INT(atts[i+2]);
        if (nsIndex == INT_MAX)
            atts[i+2] = NULL;
        else if (nsIndex == INT_MAX - 1)
            atts[i+2] = ctxt->str_xml_ns;
        else
            atts[i+2] = ctxt->nsTab[nsIndex * 2 + 1];

        if ((j < nratts) && (ctxt->attallocs[j] & 0x80000000) == 0) {
            atts[i+3] = BASE_PTR + XML_PTR_TO_INT(atts[i+3]);  /* value */
            atts[i+4] = BASE_PTR + XML_PTR_TO_INT(atts[i+4]);  /* valuend */
        }
    }

    uri = xmlParserNsLookupUri(ctxt, &hprefix);
    if ((prefix != NULL) && (uri == NULL)) {
	xmlNsErr(ctxt, XML_NS_ERR_UNDEFINED_NAMESPACE,
	         "Namespace prefix %s on %s is not defined\n",
		 prefix, localname, NULL);
    }
    *pref = prefix;
    *URI = uri;

    /*
     * SAX callback
     */
    if ((ctxt->sax != NULL) && (ctxt->sax->startElementNs != NULL) &&
	(!ctxt->disableSAX)) {
	if (nbNs > 0)
	    ctxt->sax->startElementNs(ctxt->userData, localname, prefix, uri,
                          nbNs, ctxt->nsTab + 2 * (ctxt->nsNr - nbNs),
			  nbatts / 5, nbdef, atts);
	else
	    ctxt->sax->startElementNs(ctxt->userData, localname, prefix, uri,
                          0, NULL, nbatts / 5, nbdef, atts);
    }

done:
    /*
     * Free allocated attribute values
     */
    if (attval != 0) {
	for (i = 0, j = 0; j < nratts; i += 5, j++)
	    if (ctxt->attallocs[j] & 0x80000000)
	        xmlFree((xmlChar *) atts[i+3]);
    }

    *nbNsPtr = nbNs;
    return(localname);
}

/**
 * Parse an end tag. Always consumes '</'.
 *
 *     [42] ETag ::= '</' Name S? '>'
 *
 * With namespace
 *
 *     [NS 9] ETag ::= '</' QName S? '>'
 * @param ctxt  an XML parser context
 * @param tag  the corresponding start tag
 */

static void
xmlParseEndTag2(xmlParserCtxtPtr ctxt, const xmlStartTag *tag) {
    const xmlChar *name;

    GROW;
    if ((RAW != '<') || (NXT(1) != '/')) {
	xmlFatalErr(ctxt, XML_ERR_LTSLASH_REQUIRED, NULL);
	return;
    }
    SKIP(2);

    if (tag->prefix == NULL)
        name = xmlParseNameAndCompare(ctxt, ctxt->name);
    else
        name = xmlParseQNameAndCompare(ctxt, ctxt->name, tag->prefix);

    /*
     * We should definitely be at the ending "S? '>'" part
     */
    GROW;
    SKIP_BLANKS;
    if ((!IS_BYTE_CHAR(RAW)) || (RAW != '>')) {
	xmlFatalErr(ctxt, XML_ERR_GT_REQUIRED, NULL);
    } else
	NEXT1;

    /*
     * [ WFC: Element Type Match ]
     * The Name in an element's end-tag must match the element type in the
     * start-tag.
     *
     */
    if (name != (xmlChar*)1) {
        if (name == NULL) name = BAD_CAST "unparsable";
        xmlFatalErrMsgStrIntStr(ctxt, XML_ERR_TAG_NAME_MISMATCH,
		     "Opening and ending tag mismatch: %s line %d and %s\n",
		                ctxt->name, tag->line, name);
    }

    /*
     * SAX: End of Tag
     */
    if ((ctxt->sax != NULL) && (ctxt->sax->endElementNs != NULL) &&
	(!ctxt->disableSAX))
	ctxt->sax->endElementNs(ctxt->userData, ctxt->name, tag->prefix,
                                tag->URI);

    spacePop(ctxt);
    if (tag->nsNr != 0)
	xmlParserNsPop(ctxt, tag->nsNr);
}

/**
 * Parse escaped pure raw content. Always consumes '<!['.
 *
 * @deprecated Internal function, don't use.
 *
 *     [18] CDSect ::= CDStart CData CDEnd
 *
 *     [19] CDStart ::= '<![CDATA['
 *
 *     [20] Data ::= (Char* - (Char* ']]>' Char*))
 *
 *     [21] CDEnd ::= ']]>'
 * @param ctxt  an XML parser context
 */
void
xmlParseCDSect(xmlParserCtxt *ctxt) {
    xmlChar *buf = NULL;
    int len = 0;
    int size = XML_PARSER_BUFFER_SIZE;
    int r, rl;
    int	s, sl;
    int cur, l;
    int maxLength = (ctxt->options & XML_PARSE_HUGE) ?
                    XML_MAX_HUGE_LENGTH :
                    XML_MAX_TEXT_LENGTH;

    if ((CUR != '<') || (NXT(1) != '!') || (NXT(2) != '['))
        return;
    SKIP(3);

    if (!CMP6(CUR_PTR, 'C', 'D', 'A', 'T', 'A', '['))
        return;
    SKIP(6);

    r = xmlCurrentCharRecover(ctxt, &rl);
    if (!IS_CHAR(r)) {
	xmlFatalErr(ctxt, XML_ERR_CDATA_NOT_FINISHED, NULL);
        goto out;
    }
    NEXTL(rl);
    s = xmlCurrentCharRecover(ctxt, &sl);
    if (!IS_CHAR(s)) {
	xmlFatalErr(ctxt, XML_ERR_CDATA_NOT_FINISHED, NULL);
        goto out;
    }
    NEXTL(sl);
    cur = xmlCurrentCharRecover(ctxt, &l);
    buf = xmlMalloc(size);
    if (buf == NULL) {
	xmlErrMemory(ctxt);
        goto out;
    }
    while (IS_CHAR(cur) &&
           ((r != ']') || (s != ']') || (cur != '>'))) {
	if (len + 5 >= size) {
	    xmlChar *tmp;
            int newSize;

            newSize = xmlGrowCapacity(size, 1, 1, maxLength);
            if (newSize < 0) {
                xmlFatalErrMsg(ctxt, XML_ERR_CDATA_NOT_FINISHED,
                               "CData section too big found\n");
                goto out;
            }
	    tmp = xmlRealloc(buf, newSize);
	    if (tmp == NULL) {
		xmlErrMemory(ctxt);
                goto out;
	    }
	    buf = tmp;
	    size = newSize;
	}
	COPY_BUF(buf, len, r);
	r = s;
	rl = sl;
	s = cur;
	sl = l;
	NEXTL(l);
	cur = xmlCurrentCharRecover(ctxt, &l);
    }
    buf[len] = 0;
    if (cur != '>') {
	xmlFatalErrMsgStr(ctxt, XML_ERR_CDATA_NOT_FINISHED,
	                     "CData section not finished\n%.50s\n", buf);
        goto out;
    }
    NEXTL(l);

    /*
     * OK the buffer is to be consumed as cdata.
     */
    if ((ctxt->sax != NULL) && (!ctxt->disableSAX)) {
        if ((ctxt->sax->cdataBlock != NULL) &&
            ((ctxt->options & XML_PARSE_NOCDATA) == 0)) {
            ctxt->sax->cdataBlock(ctxt->userData, buf, len);
        } else if (ctxt->sax->characters != NULL) {
            ctxt->sax->characters(ctxt->userData, buf, len);
        }
    }

out:
    xmlFree(buf);
}

/**
 * Parse a content sequence. Stops at EOF or '</'. Leaves checking of
 * unexpected EOF to the caller.
 *
 * @param ctxt  an XML parser context
 */

static void
xmlParseContentInternal(xmlParserCtxtPtr ctxt) {
    int oldNameNr = ctxt->nameNr;
    int oldSpaceNr = ctxt->spaceNr;
    int oldNodeNr = ctxt->nodeNr;

    GROW;
    while ((ctxt->input->cur < ctxt->input->end) &&
	   (PARSER_STOPPED(ctxt) == 0)) {
	const xmlChar *cur = ctxt->input->cur;

	/*
	 * First case : a Processing Instruction.
	 */
	if ((*cur == '<') && (cur[1] == '?')) {
	    xmlParsePI(ctxt);
	}

	/*
	 * Second case : a CDSection
	 */
	/* 2.6.0 test was *cur not RAW */
	else if (CMP9(CUR_PTR, '<', '!', '[', 'C', 'D', 'A', 'T', 'A', '[')) {
	    xmlParseCDSect(ctxt);
	}

	/*
	 * Third case :  a comment
	 */
	else if ((*cur == '<') && (NXT(1) == '!') &&
		 (NXT(2) == '-') && (NXT(3) == '-')) {
	    xmlParseComment(ctxt);
	}

	/*
	 * Fourth case :  a sub-element.
	 */
	else if (*cur == '<') {
            if (NXT(1) == '/') {
                if (ctxt->nameNr <= oldNameNr)
                    break;
	        xmlParseElementEnd(ctxt);
            } else {
	        xmlParseElementStart(ctxt);
            }
	}

	/*
	 * Fifth case : a reference. If if has not been resolved,
	 *    parsing returns it's Name, create the node
	 */

	else if (*cur == '&') {
	    xmlParseReference(ctxt);
	}

	/*
	 * Last case, text. Note that References are handled directly.
	 */
	else {
	    xmlParseCharDataInternal(ctxt, 0);
	}

	SHRINK;
	GROW;
    }

    if ((ctxt->nameNr > oldNameNr) &&
        (ctxt->input->cur >= ctxt->input->end) &&
        (ctxt->wellFormed)) {
        const xmlChar *name = ctxt->nameTab[ctxt->nameNr - 1];
        int line = ctxt->pushTab[ctxt->nameNr - 1].line;
        xmlFatalErrMsgStrIntStr(ctxt, XML_ERR_TAG_NOT_FINISHED,
                "Premature end of data in tag %s line %d\n",
                name, line, NULL);
    }

    /*
     * Clean up in error case
     */

    while (ctxt->nodeNr > oldNodeNr)
        nodePop(ctxt);

    while (ctxt->nameNr > oldNameNr) {
        xmlStartTag *tag = &ctxt->pushTab[ctxt->nameNr - 1];

        if (tag->nsNr != 0)
            xmlParserNsPop(ctxt, tag->nsNr);

        namePop(ctxt);
    }

    while (ctxt->spaceNr > oldSpaceNr)
        spacePop(ctxt);
}

/**
 * Parse XML element content. This is useful if you're only interested
 * in custom SAX callbacks. If you want a node list, use
 * #xmlCtxtParseContent.
 *
 * @param ctxt  an XML parser context
 */
void
xmlParseContent(xmlParserCtxt *ctxt) {
    if ((ctxt == NULL) || (ctxt->input == NULL))
        return;

    xmlCtxtInitializeLate(ctxt);

    xmlParseContentInternal(ctxt);

    xmlParserCheckEOF(ctxt, XML_ERR_NOT_WELL_BALANCED);
}

/**
 * Parse an XML element
 *
 * @deprecated Internal function, don't use.
 *
 *     [39] element ::= EmptyElemTag | STag content ETag
 *
 * [ WFC: Element Type Match ]
 * The Name in an element's end-tag must match the element type in the
 * start-tag.
 *
 * @param ctxt  an XML parser context
 */

void
xmlParseElement(xmlParserCtxt *ctxt) {
    if (xmlParseElementStart(ctxt) != 0)
        return;

    xmlParseContentInternal(ctxt);

    if (ctxt->input->cur >= ctxt->input->end) {
        if (ctxt->wellFormed) {
            const xmlChar *name = ctxt->nameTab[ctxt->nameNr - 1];
            int line = ctxt->pushTab[ctxt->nameNr - 1].line;
            xmlFatalErrMsgStrIntStr(ctxt, XML_ERR_TAG_NOT_FINISHED,
                    "Premature end of data in tag %s line %d\n",
                    name, line, NULL);
        }
        return;
    }

    xmlParseElementEnd(ctxt);
}

/**
 * Parse the start of an XML element. Returns -1 in case of error, 0 if an
 * opening tag was parsed, 1 if an empty element was parsed.
 *
 * Always consumes '<'.
 *
 * @param ctxt  an XML parser context
 */
static int
xmlParseElementStart(xmlParserCtxtPtr ctxt) {
    int maxDepth = (ctxt->options & XML_PARSE_HUGE) ? 2048 : 256;
    const xmlChar *name;
    const xmlChar *prefix = NULL;
    const xmlChar *URI = NULL;
    xmlParserNodeInfo node_info;
    int line;
    xmlNodePtr cur;
    int nbNs = 0;

    if (ctxt->nameNr > maxDepth) {
        xmlFatalErrMsgInt(ctxt, XML_ERR_RESOURCE_LIMIT,
                "Excessive depth in document: %d use XML_PARSE_HUGE option\n",
                ctxt->nameNr);
	return(-1);
    }

    /* Capture start position */
    if (ctxt->record_info) {
        node_info.begin_pos = ctxt->input->consumed +
                          (CUR_PTR - ctxt->input->base);
	node_info.begin_line = ctxt->input->line;
    }

    if (ctxt->spaceNr == 0)
	spacePush(ctxt, -1);
    else if (*ctxt->space == -2)
	spacePush(ctxt, -1);
    else
	spacePush(ctxt, *ctxt->space);

    line = ctxt->input->line;
#ifdef LIBXML_SAX1_ENABLED
    if (ctxt->sax2)
#endif /* LIBXML_SAX1_ENABLED */
        name = xmlParseStartTag2(ctxt, &prefix, &URI, &nbNs);
#ifdef LIBXML_SAX1_ENABLED
    else
	name = xmlParseStartTag(ctxt);
#endif /* LIBXML_SAX1_ENABLED */
    if (name == NULL) {
	spacePop(ctxt);
        return(-1);
    }
    nameNsPush(ctxt, name, prefix, URI, line, nbNs);
    cur = ctxt->node;

#ifdef LIBXML_VALID_ENABLED
    /*
     * [ VC: Root Element Type ]
     * The Name in the document type declaration must match the element
     * type of the root element.
     */
    if (ctxt->validate && ctxt->wellFormed && ctxt->myDoc &&
        ctxt->node && (ctxt->node == ctxt->myDoc->children))
        ctxt->valid &= xmlValidateRoot(&ctxt->vctxt, ctxt->myDoc);
#endif /* LIBXML_VALID_ENABLED */

    /*
     * Check for an Empty Element.
     */
    if ((RAW == '/') && (NXT(1) == '>')) {
        SKIP(2);
	if (ctxt->sax2) {
	    if ((ctxt->sax != NULL) && (ctxt->sax->endElementNs != NULL) &&
		(!ctxt->disableSAX))
		ctxt->sax->endElementNs(ctxt->userData, name, prefix, URI);
#ifdef LIBXML_SAX1_ENABLED
	} else {
	    if ((ctxt->sax != NULL) && (ctxt->sax->endElement != NULL) &&
		(!ctxt->disableSAX))
		ctxt->sax->endElement(ctxt->userData, name);
#endif /* LIBXML_SAX1_ENABLED */
	}
	namePop(ctxt);
	spacePop(ctxt);
	if (nbNs > 0)
	    xmlParserNsPop(ctxt, nbNs);
	if (cur != NULL && ctxt->record_info) {
            node_info.node = cur;
            node_info.end_pos = ctxt->input->consumed +
                                (CUR_PTR - ctxt->input->base);
            node_info.end_line = ctxt->input->line;
            xmlParserAddNodeInfo(ctxt, &node_info);
	}
	return(1);
    }
    if (RAW == '>') {
        NEXT1;
        if (cur != NULL && ctxt->record_info) {
            node_info.node = cur;
            node_info.end_pos = 0;
            node_info.end_line = 0;
            xmlParserAddNodeInfo(ctxt, &node_info);
        }
    } else {
        xmlFatalErrMsgStrIntStr(ctxt, XML_ERR_GT_REQUIRED,
		     "Couldn't find end of Start Tag %s line %d\n",
		                name, line, NULL);

	/*
	 * end of parsing of this node.
	 */
	nodePop(ctxt);
	namePop(ctxt);
	spacePop(ctxt);
	if (nbNs > 0)
	    xmlParserNsPop(ctxt, nbNs);
	return(-1);
    }

    return(0);
}

/**
 * Parse the end of an XML element. Always consumes '</'.
 *
 * @param ctxt  an XML parser context
 */
static void
xmlParseElementEnd(xmlParserCtxtPtr ctxt) {
    xmlNodePtr cur = ctxt->node;

    if (ctxt->nameNr <= 0) {
        if ((RAW == '<') && (NXT(1) == '/'))
            SKIP(2);
        return;
    }

    /*
     * parse the end of tag: '</' should be here.
     */
    if (ctxt->sax2) {
	xmlParseEndTag2(ctxt, &ctxt->pushTab[ctxt->nameNr - 1]);
	namePop(ctxt);
    }
#ifdef LIBXML_SAX1_ENABLED
    else
	xmlParseEndTag1(ctxt, 0);
#endif /* LIBXML_SAX1_ENABLED */

    /*
     * Capture end position
     */
    if (cur != NULL && ctxt->record_info) {
        xmlParserNodeInfoPtr node_info;

        node_info = (xmlParserNodeInfoPtr) xmlParserFindNodeInfo(ctxt, cur);
        if (node_info != NULL) {
            node_info->end_pos = ctxt->input->consumed +
                                 (CUR_PTR - ctxt->input->base);
            node_info->end_line = ctxt->input->line;
        }
    }
}

/**
 * Parse the XML version value.
 *
 * @deprecated Internal function, don't use.
 *
 *     [26] VersionNum ::= '1.' [0-9]+
 *
 * In practice allow [0-9].[0-9]+ at that level
 *
 * @param ctxt  an XML parser context
 * @returns the string giving the XML version number, or NULL
 */
xmlChar *
xmlParseVersionNum(xmlParserCtxt *ctxt) {
    xmlChar *buf = NULL;
    int len = 0;
    int size = 10;
    int maxLength = (ctxt->options & XML_PARSE_HUGE) ?
                    XML_MAX_TEXT_LENGTH :
                    XML_MAX_NAME_LENGTH;
    xmlChar cur;

    buf = xmlMalloc(size);
    if (buf == NULL) {
	xmlErrMemory(ctxt);
	return(NULL);
    }
    cur = CUR;
    if (!((cur >= '0') && (cur <= '9'))) {
	xmlFree(buf);
	return(NULL);
    }
    buf[len++] = cur;
    NEXT;
    cur=CUR;
    if (cur != '.') {
	xmlFree(buf);
	return(NULL);
    }
    buf[len++] = cur;
    NEXT;
    cur=CUR;
    while ((cur >= '0') && (cur <= '9')) {
	if (len + 1 >= size) {
	    xmlChar *tmp;
            int newSize;

            newSize = xmlGrowCapacity(size, 1, 1, maxLength);
            if (newSize < 0) {
                xmlFatalErr(ctxt, XML_ERR_NAME_TOO_LONG, "VersionNum");
                xmlFree(buf);
                return(NULL);
            }
	    tmp = xmlRealloc(buf, newSize);
	    if (tmp == NULL) {
		xmlErrMemory(ctxt);
	        xmlFree(buf);
		return(NULL);
	    }
	    buf = tmp;
            size = newSize;
	}
	buf[len++] = cur;
	NEXT;
	cur=CUR;
    }
    buf[len] = 0;
    return(buf);
}

/**
 * Parse the XML version.
 *
 * @deprecated Internal function, don't use.
 *
 *     [24] VersionInfo ::= S 'version' Eq (' VersionNum ' | " VersionNum ")
 *
 *     [25] Eq ::= S? '=' S?
 *
 * @param ctxt  an XML parser context
 * @returns the version string, e.g. "1.0"
 */

xmlChar *
xmlParseVersionInfo(xmlParserCtxt *ctxt) {
    xmlChar *version = NULL;

    if (CMP7(CUR_PTR, 'v', 'e', 'r', 's', 'i', 'o', 'n')) {
	SKIP(7);
	SKIP_BLANKS;
	if (RAW != '=') {
	    xmlFatalErr(ctxt, XML_ERR_EQUAL_REQUIRED, NULL);
	    return(NULL);
        }
	NEXT;
	SKIP_BLANKS;
	if (RAW == '"') {
	    NEXT;
	    version = xmlParseVersionNum(ctxt);
	    if (RAW != '"') {
		xmlFatalErr(ctxt, XML_ERR_STRING_NOT_CLOSED, NULL);
	    } else
	        NEXT;
	} else if (RAW == '\''){
	    NEXT;
	    version = xmlParseVersionNum(ctxt);
	    if (RAW != '\'') {
		xmlFatalErr(ctxt, XML_ERR_STRING_NOT_CLOSED, NULL);
	    } else
	        NEXT;
	} else {
	    xmlFatalErr(ctxt, XML_ERR_STRING_NOT_STARTED, NULL);
	}
    }
    return(version);
}

/**
 * Parse the XML encoding name
 *
 * @deprecated Internal function, don't use.
 *
 *     [81] EncName ::= [A-Za-z] ([A-Za-z0-9._] | '-')*
 *
 * @param ctxt  an XML parser context
 * @returns the encoding name value or NULL
 */
xmlChar *
xmlParseEncName(xmlParserCtxt *ctxt) {
    xmlChar *buf = NULL;
    int len = 0;
    int size = 10;
    int maxLength = (ctxt->options & XML_PARSE_HUGE) ?
                    XML_MAX_TEXT_LENGTH :
                    XML_MAX_NAME_LENGTH;
    xmlChar cur;

    cur = CUR;
    if (((cur >= 'a') && (cur <= 'z')) ||
        ((cur >= 'A') && (cur <= 'Z'))) {
	buf = xmlMalloc(size);
	if (buf == NULL) {
	    xmlErrMemory(ctxt);
	    return(NULL);
	}

	buf[len++] = cur;
	NEXT;
	cur = CUR;
	while (((cur >= 'a') && (cur <= 'z')) ||
	       ((cur >= 'A') && (cur <= 'Z')) ||
	       ((cur >= '0') && (cur <= '9')) ||
	       (cur == '.') || (cur == '_') ||
	       (cur == '-')) {
	    if (len + 1 >= size) {
	        xmlChar *tmp;
                int newSize;

                newSize = xmlGrowCapacity(size, 1, 1, maxLength);
                if (newSize < 0) {
                    xmlFatalErr(ctxt, XML_ERR_NAME_TOO_LONG, "EncName");
                    xmlFree(buf);
                    return(NULL);
                }
		tmp = xmlRealloc(buf, newSize);
		if (tmp == NULL) {
		    xmlErrMemory(ctxt);
		    xmlFree(buf);
		    return(NULL);
		}
		buf = tmp;
                size = newSize;
	    }
	    buf[len++] = cur;
	    NEXT;
	    cur = CUR;
        }
	buf[len] = 0;
    } else {
	xmlFatalErr(ctxt, XML_ERR_ENCODING_NAME, NULL);
    }
    return(buf);
}

/**
 * Parse the XML encoding declaration
 *
 * @deprecated Internal function, don't use.
 *
 *     [80] EncodingDecl ::= S 'encoding' Eq ('"' EncName '"' | 
 *                           "'" EncName "'")
 *
 * this setups the conversion filters.
 *
 * @param ctxt  an XML parser context
 * @returns the encoding value or NULL
 */

const xmlChar *
xmlParseEncodingDecl(xmlParserCtxt *ctxt) {
    xmlChar *encoding = NULL;

    SKIP_BLANKS;
    if (CMP8(CUR_PTR, 'e', 'n', 'c', 'o', 'd', 'i', 'n', 'g') == 0)
        return(NULL);

    SKIP(8);
    SKIP_BLANKS;
    if (RAW != '=') {
        xmlFatalErr(ctxt, XML_ERR_EQUAL_REQUIRED, NULL);
        return(NULL);
    }
    NEXT;
    SKIP_BLANKS;
    if (RAW == '"') {
        NEXT;
        encoding = xmlParseEncName(ctxt);
        if (RAW != '"') {
            xmlFatalErr(ctxt, XML_ERR_STRING_NOT_CLOSED, NULL);
            xmlFree(encoding);
            return(NULL);
        } else
            NEXT;
    } else if (RAW == '\''){
        NEXT;
        encoding = xmlParseEncName(ctxt);
        if (RAW != '\'') {
            xmlFatalErr(ctxt, XML_ERR_STRING_NOT_CLOSED, NULL);
            xmlFree(encoding);
            return(NULL);
        } else
            NEXT;
    } else {
        xmlFatalErr(ctxt, XML_ERR_STRING_NOT_STARTED, NULL);
    }

    if (encoding == NULL)
        return(NULL);

    xmlSetDeclaredEncoding(ctxt, encoding);

    return(ctxt->encoding);
}

/**
 * Parse the XML standalone declaration
 *
 * @deprecated Internal function, don't use.
 *
 *     [32] SDDecl ::= S 'standalone' Eq
 *                     (("'" ('yes' | 'no') "'") | ('"' ('yes' | 'no')'"'))
 *
 * [ VC: Standalone Document Declaration ]
 * TODO The standalone document declaration must have the value "no"
 * if any external markup declarations contain declarations of:
 *  - attributes with default values, if elements to which these
 *    attributes apply appear in the document without specifications
 *    of values for these attributes, or
 *  - entities (other than amp, lt, gt, apos, quot), if references
 *    to those entities appear in the document, or
 *  - attributes with values subject to normalization, where the
 *    attribute appears in the document with a value which will change
 *    as a result of normalization, or
 *  - element types with element content, if white space occurs directly
 *    within any instance of those types.
 *
 * @param ctxt  an XML parser context
 * @returns
 *   1 if standalone="yes"
 *   0 if standalone="no"
 *  -2 if standalone attribute is missing or invalid
 *	  (A standalone value of -2 means that the XML declaration was found,
 *	   but no value was specified for the standalone attribute).
 */

int
xmlParseSDDecl(xmlParserCtxt *ctxt) {
    int standalone = -2;

    SKIP_BLANKS;
    if (CMP10(CUR_PTR, 's', 't', 'a', 'n', 'd', 'a', 'l', 'o', 'n', 'e')) {
	SKIP(10);
        SKIP_BLANKS;
	if (RAW != '=') {
	    xmlFatalErr(ctxt, XML_ERR_EQUAL_REQUIRED, NULL);
	    return(standalone);
        }
	NEXT;
	SKIP_BLANKS;
        if (RAW == '\''){
	    NEXT;
	    if ((RAW == 'n') && (NXT(1) == 'o')) {
	        standalone = 0;
                SKIP(2);
	    } else if ((RAW == 'y') && (NXT(1) == 'e') &&
	               (NXT(2) == 's')) {
	        standalone = 1;
		SKIP(3);
            } else {
		xmlFatalErr(ctxt, XML_ERR_STANDALONE_VALUE, NULL);
	    }
	    if (RAW != '\'') {
		xmlFatalErr(ctxt, XML_ERR_STRING_NOT_CLOSED, NULL);
	    } else
	        NEXT;
	} else if (RAW == '"'){
	    NEXT;
	    if ((RAW == 'n') && (NXT(1) == 'o')) {
	        standalone = 0;
		SKIP(2);
	    } else if ((RAW == 'y') && (NXT(1) == 'e') &&
	               (NXT(2) == 's')) {
	        standalone = 1;
                SKIP(3);
            } else {
		xmlFatalErr(ctxt, XML_ERR_STANDALONE_VALUE, NULL);
	    }
	    if (RAW != '"') {
		xmlFatalErr(ctxt, XML_ERR_STRING_NOT_CLOSED, NULL);
	    } else
	        NEXT;
	} else {
	    xmlFatalErr(ctxt, XML_ERR_STRING_NOT_STARTED, NULL);
        }
    }
    return(standalone);
}

/**
 * Parse an XML declaration header
 *
 * @deprecated Internal function, don't use.
 *
 *     [23] XMLDecl ::= '<?xml' VersionInfo EncodingDecl? SDDecl? S? '?>'
 * @param ctxt  an XML parser context
 */

void
xmlParseXMLDecl(xmlParserCtxt *ctxt) {
    xmlChar *version;

    /*
     * This value for standalone indicates that the document has an
     * XML declaration but it does not have a standalone attribute.
     * It will be overwritten later if a standalone attribute is found.
     */

    ctxt->standalone = -2;

    /*
     * We know that '<?xml' is here.
     */
    SKIP(5);

    if (!IS_BLANK_CH(RAW)) {
	xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED,
	               "Blank needed after '<?xml'\n");
    }
    SKIP_BLANKS;

    /*
     * We must have the VersionInfo here.
     */
    version = xmlParseVersionInfo(ctxt);
    if (version == NULL) {
	xmlFatalErr(ctxt, XML_ERR_VERSION_MISSING, NULL);
    } else {
	if (!xmlStrEqual(version, (const xmlChar *) XML_DEFAULT_VERSION)) {
	    /*
	     * Changed here for XML-1.0 5th edition
	     */
	    if (ctxt->options & XML_PARSE_OLD10) {
		xmlFatalErrMsgStr(ctxt, XML_ERR_UNKNOWN_VERSION,
			          "Unsupported version '%s'\n",
			          version);
	    } else {
	        if ((version[0] == '1') && ((version[1] == '.'))) {
		    xmlWarningMsg(ctxt, XML_WAR_UNKNOWN_VERSION,
		                  "Unsupported version '%s'\n",
				  version, NULL);
		} else {
		    xmlFatalErrMsgStr(ctxt, XML_ERR_UNKNOWN_VERSION,
				      "Unsupported version '%s'\n",
				      version);
		}
	    }
	}
	if (ctxt->version != NULL)
	    xmlFree(ctxt->version);
	ctxt->version = version;
    }

    /*
     * We may have the encoding declaration
     */
    if (!IS_BLANK_CH(RAW)) {
        if ((RAW == '?') && (NXT(1) == '>')) {
	    SKIP(2);
	    return;
	}
	xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED, "Blank needed here\n");
    }
    xmlParseEncodingDecl(ctxt);

    /*
     * We may have the standalone status.
     */
    if ((ctxt->encoding != NULL) && (!IS_BLANK_CH(RAW))) {
        if ((RAW == '?') && (NXT(1) == '>')) {
	    SKIP(2);
	    return;
	}
	xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED, "Blank needed here\n");
    }

    /*
     * We can grow the input buffer freely at that point
     */
    GROW;

    SKIP_BLANKS;
    ctxt->standalone = xmlParseSDDecl(ctxt);

    SKIP_BLANKS;
    if ((RAW == '?') && (NXT(1) == '>')) {
        SKIP(2);
    } else if (RAW == '>') {
        /* Deprecated old WD ... */
	xmlFatalErr(ctxt, XML_ERR_XMLDECL_NOT_FINISHED, NULL);
	NEXT;
    } else {
        int c;

	xmlFatalErr(ctxt, XML_ERR_XMLDECL_NOT_FINISHED, NULL);
        while ((PARSER_STOPPED(ctxt) == 0) &&
               ((c = CUR) != 0)) {
            NEXT;
            if (c == '>')
                break;
        }
    }
}

/**
 * @since 2.14.0
 *
 * @param ctxt  parser context
 * @returns the version from the XML declaration.
 */
const xmlChar *
xmlCtxtGetVersion(xmlParserCtxt *ctxt) {
    if (ctxt == NULL)
        return(NULL);

    return(ctxt->version);
}

/**
 * @since 2.14.0
 *
 * @param ctxt  parser context
 * @returns the value from the standalone document declaration.
 */
int
xmlCtxtGetStandalone(xmlParserCtxt *ctxt) {
    if (ctxt == NULL)
        return(0);

    return(ctxt->standalone);
}

/**
 * Parse an XML Misc* optional field.
 *
 * @deprecated Internal function, don't use.
 *
 *     [27] Misc ::= Comment | PI |  S
 * @param ctxt  an XML parser context
 */

void
xmlParseMisc(xmlParserCtxt *ctxt) {
    while (PARSER_STOPPED(ctxt) == 0) {
        SKIP_BLANKS;
        GROW;
        if ((RAW == '<') && (NXT(1) == '?')) {
	    xmlParsePI(ctxt);
        } else if (CMP4(CUR_PTR, '<', '!', '-', '-')) {
	    xmlParseComment(ctxt);
        } else {
            break;
        }
    }
}

static void
xmlFinishDocument(xmlParserCtxtPtr ctxt) {
    xmlDocPtr doc;

    /*
     * SAX: end of the document processing.
     */
    if ((ctxt->sax) && (ctxt->sax->endDocument != NULL))
        ctxt->sax->endDocument(ctxt->userData);

    /*
     * Remove locally kept entity definitions if the tree was not built
     */
    doc = ctxt->myDoc;
    if ((doc != NULL) &&
        (xmlStrEqual(doc->version, SAX_COMPAT_MODE))) {
        xmlFreeDoc(doc);
        ctxt->myDoc = NULL;
    }
}

/**
 * Parse an XML document and invoke the SAX handlers. This is useful
 * if you're only interested in custom SAX callbacks. If you want a
 * document tree, use #xmlCtxtParseDocument.
 *
 * @param ctxt  an XML parser context
 * @returns 0, -1 in case of error.
 */

int
xmlParseDocument(xmlParserCtxt *ctxt) {
    if ((ctxt == NULL) || (ctxt->input == NULL))
        return(-1);

    GROW;

    /*
     * SAX: detecting the level.
     */
    xmlCtxtInitializeLate(ctxt);

    if ((ctxt->sax) && (ctxt->sax->setDocumentLocator)) {
        ctxt->sax->setDocumentLocator(ctxt->userData,
                (xmlSAXLocator *) &xmlDefaultSAXLocator);
    }

    xmlDetectEncoding(ctxt);

    if (CUR == 0) {
	xmlFatalErr(ctxt, XML_ERR_DOCUMENT_EMPTY, NULL);
	return(-1);
    }

    GROW;
    if ((CMP5(CUR_PTR, '<', '?', 'x', 'm', 'l')) && (IS_BLANK_CH(NXT(5)))) {

	/*
	 * Note that we will switch encoding on the fly.
	 */
	xmlParseXMLDecl(ctxt);
	SKIP_BLANKS;
    } else {
	ctxt->version = xmlCharStrdup(XML_DEFAULT_VERSION);
        if (ctxt->version == NULL) {
            xmlErrMemory(ctxt);
            return(-1);
        }
    }
    if ((ctxt->sax) && (ctxt->sax->startDocument) && (!ctxt->disableSAX))
        ctxt->sax->startDocument(ctxt->userData);
    if ((ctxt->myDoc != NULL) && (ctxt->input != NULL) &&
        (ctxt->input->buf != NULL) && (ctxt->input->buf->compressed >= 0)) {
	ctxt->myDoc->compression = ctxt->input->buf->compressed;
    }

    /*
     * The Misc part of the Prolog
     */
    xmlParseMisc(ctxt);

    /*
     * Then possibly doc type declaration(s) and more Misc
     * (doctypedecl Misc*)?
     */
    GROW;
    if (CMP9(CUR_PTR, '<', '!', 'D', 'O', 'C', 'T', 'Y', 'P', 'E')) {

	ctxt->inSubset = 1;
	xmlParseDocTypeDecl(ctxt);
	if (RAW == '[') {
	    xmlParseInternalSubset(ctxt);
	} else if (RAW == '>') {
            NEXT;
        }

	/*
	 * Create and update the external subset.
	 */
	ctxt->inSubset = 2;
	if ((ctxt->sax != NULL) && (ctxt->sax->externalSubset != NULL) &&
	    (!ctxt->disableSAX))
	    ctxt->sax->externalSubset(ctxt->userData, ctxt->intSubName,
	                              ctxt->extSubSystem, ctxt->extSubURI);
	ctxt->inSubset = 0;

        xmlCleanSpecialAttr(ctxt);

	xmlParseMisc(ctxt);
    }

    /*
     * Time to start parsing the tree itself
     */
    GROW;
    if (RAW != '<') {
        if (ctxt->wellFormed)
            xmlFatalErrMsg(ctxt, XML_ERR_DOCUMENT_EMPTY,
                           "Start tag expected, '<' not found\n");
    } else {
	xmlParseElement(ctxt);

	/*
	 * The Misc part at the end
	 */
	xmlParseMisc(ctxt);

        xmlParserCheckEOF(ctxt, XML_ERR_DOCUMENT_END);
    }

    ctxt->instate = XML_PARSER_EOF;
    xmlFinishDocument(ctxt);

    if (! ctxt->wellFormed) {
	ctxt->valid = 0;
	return(-1);
    }

    return(0);
}

/**
 * Parse a general parsed entity
 * An external general parsed entity is well-formed if it matches the
 * production labeled extParsedEnt.
 *
 * @deprecated Internal function, don't use.
 *
 *     [78] extParsedEnt ::= TextDecl? content
 *
 * @param ctxt  an XML parser context
 * @returns 0, -1 in case of error. the parser context is augmented
 *                as a result of the parsing.
 */

int
xmlParseExtParsedEnt(xmlParserCtxt *ctxt) {
    if ((ctxt == NULL) || (ctxt->input == NULL))
        return(-1);

    xmlCtxtInitializeLate(ctxt);

    if ((ctxt->sax) && (ctxt->sax->setDocumentLocator)) {
        ctxt->sax->setDocumentLocator(ctxt->userData,
                (xmlSAXLocator *) &xmlDefaultSAXLocator);
    }

    xmlDetectEncoding(ctxt);

    if (CUR == 0) {
	xmlFatalErr(ctxt, XML_ERR_DOCUMENT_EMPTY, NULL);
    }

    /*
     * Check for the XMLDecl in the Prolog.
     */
    GROW;
    if ((CMP5(CUR_PTR, '<', '?', 'x', 'm', 'l')) && (IS_BLANK_CH(NXT(5)))) {

	/*
	 * Note that we will switch encoding on the fly.
	 */
	xmlParseXMLDecl(ctxt);
	SKIP_BLANKS;
    } else {
	ctxt->version = xmlCharStrdup(XML_DEFAULT_VERSION);
    }
    if ((ctxt->sax) && (ctxt->sax->startDocument) && (!ctxt->disableSAX))
        ctxt->sax->startDocument(ctxt->userData);

    /*
     * Doing validity checking on chunk doesn't make sense
     */
    ctxt->options &= ~XML_PARSE_DTDVALID;
    ctxt->validate = 0;
    ctxt->depth = 0;

    xmlParseContentInternal(ctxt);

    if (ctxt->input->cur < ctxt->input->end)
	xmlFatalErr(ctxt, XML_ERR_NOT_WELL_BALANCED, NULL);

    /*
     * SAX: end of the document processing.
     */
    if ((ctxt->sax) && (ctxt->sax->endDocument != NULL))
        ctxt->sax->endDocument(ctxt->userData);

    if (! ctxt->wellFormed) return(-1);
    return(0);
}

#ifdef LIBXML_PUSH_ENABLED
/************************************************************************
 *									*
 *		Progressive parsing interfaces				*
 *									*
 ************************************************************************/

/**
 * Check whether the input buffer contains a character.
 *
 * @param ctxt  an XML parser context
 * @param c  character
 */
static int
xmlParseLookupChar(xmlParserCtxtPtr ctxt, int c) {
    const xmlChar *cur;

    if (ctxt->checkIndex == 0) {
        cur = ctxt->input->cur + 1;
    } else {
        cur = ctxt->input->cur + ctxt->checkIndex;
    }

    if (memchr(cur, c, ctxt->input->end - cur) == NULL) {
        size_t index = ctxt->input->end - ctxt->input->cur;

        if (index > LONG_MAX) {
            ctxt->checkIndex = 0;
            return(1);
        }
        ctxt->checkIndex = index;
        return(0);
    } else {
        ctxt->checkIndex = 0;
        return(1);
    }
}

/**
 * Check whether the input buffer contains a string.
 *
 * @param ctxt  an XML parser context
 * @param startDelta  delta to apply at the start
 * @param str  string
 * @param strLen  length of string
 */
static const xmlChar *
xmlParseLookupString(xmlParserCtxtPtr ctxt, size_t startDelta,
                     const char *str, size_t strLen) {
    const xmlChar *cur, *term;

    if (ctxt->checkIndex == 0) {
        cur = ctxt->input->cur + startDelta;
    } else {
        cur = ctxt->input->cur + ctxt->checkIndex;
    }

    term = BAD_CAST strstr((const char *) cur, str);
    if (term == NULL) {
        const xmlChar *end = ctxt->input->end;
        size_t index;

        /* Rescan (strLen - 1) characters. */
        if ((size_t) (end - cur) < strLen)
            end = cur;
        else
            end -= strLen - 1;
        index = end - ctxt->input->cur;
        if (index > LONG_MAX) {
            ctxt->checkIndex = 0;
            return(ctxt->input->end - strLen);
        }
        ctxt->checkIndex = index;
    } else {
        ctxt->checkIndex = 0;
    }

    return(term);
}

/**
 * Check whether the input buffer contains terminated char data.
 *
 * @param ctxt  an XML parser context
 */
static int
xmlParseLookupCharData(xmlParserCtxtPtr ctxt) {
    const xmlChar *cur = ctxt->input->cur + ctxt->checkIndex;
    const xmlChar *end = ctxt->input->end;
    size_t index;

    while (cur < end) {
        if ((*cur == '<') || (*cur == '&')) {
            ctxt->checkIndex = 0;
            return(1);
        }
        cur++;
    }

    index = cur - ctxt->input->cur;
    if (index > LONG_MAX) {
        ctxt->checkIndex = 0;
        return(1);
    }
    ctxt->checkIndex = index;
    return(0);
}

/**
 * Check whether there's enough data in the input buffer to finish parsing
 * a start tag. This has to take quotes into account.
 *
 * @param ctxt  an XML parser context
 */
static int
xmlParseLookupGt(xmlParserCtxtPtr ctxt) {
    const xmlChar *cur;
    const xmlChar *end = ctxt->input->end;
    int state = ctxt->endCheckState;
    size_t index;

    if (ctxt->checkIndex == 0)
        cur = ctxt->input->cur + 1;
    else
        cur = ctxt->input->cur + ctxt->checkIndex;

    while (cur < end) {
        if (state) {
            if (*cur == state)
                state = 0;
        } else if (*cur == '\'' || *cur == '"') {
            state = *cur;
        } else if (*cur == '>') {
            ctxt->checkIndex = 0;
            ctxt->endCheckState = 0;
            return(1);
        }
        cur++;
    }

    index = cur - ctxt->input->cur;
    if (index > LONG_MAX) {
        ctxt->checkIndex = 0;
        ctxt->endCheckState = 0;
        return(1);
    }
    ctxt->checkIndex = index;
    ctxt->endCheckState = state;
    return(0);
}

/**
 * Check whether there's enough data in the input buffer to finish parsing
 * the internal subset.
 *
 * @param ctxt  an XML parser context
 */
static int
xmlParseLookupInternalSubset(xmlParserCtxtPtr ctxt) {
    /*
     * Sorry, but progressive parsing of the internal subset is not
     * supported. We first check that the full content of the internal
     * subset is available and parsing is launched only at that point.
     * Internal subset ends with "']' S? '>'" in an unescaped section and
     * not in a ']]>' sequence which are conditional sections.
     */
    const xmlChar *cur, *start;
    const xmlChar *end = ctxt->input->end;
    int state = ctxt->endCheckState;
    size_t index;

    if (ctxt->checkIndex == 0) {
        cur = ctxt->input->cur + 1;
    } else {
        cur = ctxt->input->cur + ctxt->checkIndex;
    }
    start = cur;

    while (cur < end) {
        if (state == '-') {
            if ((*cur == '-') &&
                (cur[1] == '-') &&
                (cur[2] == '>')) {
                state = 0;
                cur += 3;
                start = cur;
                continue;
            }
        }
        else if (state == ']') {
            if (*cur == '>') {
                ctxt->checkIndex = 0;
                ctxt->endCheckState = 0;
                return(1);
            }
            if (IS_BLANK_CH(*cur)) {
                state = ' ';
            } else if (*cur != ']') {
                state = 0;
                start = cur;
                continue;
            }
        }
        else if (state == ' ') {
            if (*cur == '>') {
                ctxt->checkIndex = 0;
                ctxt->endCheckState = 0;
                return(1);
            }
            if (!IS_BLANK_CH(*cur)) {
                state = 0;
                start = cur;
                continue;
            }
        }
        else if (state != 0) {
            if (*cur == state) {
                state = 0;
                start = cur + 1;
            }
        }
        else if (*cur == '<') {
            if ((cur[1] == '!') &&
                (cur[2] == '-') &&
                (cur[3] == '-')) {
                state = '-';
                cur += 4;
                /* Don't treat <!--> as comment */
                start = cur;
                continue;
            }
        }
        else if ((*cur == '"') || (*cur == '\'') || (*cur == ']')) {
            state = *cur;
        }

        cur++;
    }

    /*
     * Rescan the three last characters to detect "<!--" and "-->"
     * split across chunks.
     */
    if ((state == 0) || (state == '-')) {
        if (cur - start < 3)
            cur = start;
        else
            cur -= 3;
    }
    index = cur - ctxt->input->cur;
    if (index > LONG_MAX) {
        ctxt->checkIndex = 0;
        ctxt->endCheckState = 0;
        return(1);
    }
    ctxt->checkIndex = index;
    ctxt->endCheckState = state;
    return(0);
}

/**
 * Try to progress on parsing
 *
 * @param ctxt  an XML parser context
 * @param terminate  last chunk indicator
 * @returns zero if no parsing was possible
 */
static int
xmlParseTryOrFinish(xmlParserCtxtPtr ctxt, int terminate) {
    int ret = 0;
    size_t avail;
    xmlChar cur, next;

    if (ctxt->input == NULL)
        return(0);

    if ((ctxt->input != NULL) &&
        (ctxt->input->cur - ctxt->input->base > 4096)) {
        xmlParserShrink(ctxt);
    }

    while (ctxt->disableSAX == 0) {
        avail = ctxt->input->end - ctxt->input->cur;
        if (avail < 1)
	    goto done;
        switch (ctxt->instate) {
            case XML_PARSER_EOF:
	        /*
		 * Document parsing is done !
		 */
	        goto done;
            case XML_PARSER_START:
                /*
                 * Very first chars read from the document flow.
                 */
                if ((!terminate) && (avail < 4))
                    goto done;

                /*
                 * We need more bytes to detect EBCDIC code pages.
                 * See xmlDetectEBCDIC.
                 */
                if ((CMP4(CUR_PTR, 0x4C, 0x6F, 0xA7, 0x94)) &&
                    (!terminate) && (avail < 200))
                    goto done;

                xmlDetectEncoding(ctxt);
                ctxt->instate = XML_PARSER_XML_DECL;
		break;

            case XML_PARSER_XML_DECL:
		if ((!terminate) && (avail < 2))
		    goto done;
		cur = ctxt->input->cur[0];
		next = ctxt->input->cur[1];
	        if ((cur == '<') && (next == '?')) {
		    /* PI or XML decl */
		    if ((!terminate) &&
                        (!xmlParseLookupString(ctxt, 2, "?>", 2)))
			goto done;
		    if ((ctxt->input->cur[2] == 'x') &&
			(ctxt->input->cur[3] == 'm') &&
			(ctxt->input->cur[4] == 'l') &&
			(IS_BLANK_CH(ctxt->input->cur[5]))) {
			ret += 5;
			xmlParseXMLDecl(ctxt);
		    } else {
			ctxt->version = xmlCharStrdup(XML_DEFAULT_VERSION);
                        if (ctxt->version == NULL) {
                            xmlErrMemory(ctxt);
                            break;
                        }
		    }
		} else {
		    ctxt->version = xmlCharStrdup(XML_DEFAULT_VERSION);
		    if (ctxt->version == NULL) {
		        xmlErrMemory(ctxt);
			break;
		    }
		}
                if ((ctxt->sax) && (ctxt->sax->setDocumentLocator)) {
                    ctxt->sax->setDocumentLocator(ctxt->userData,
                            (xmlSAXLocator *) &xmlDefaultSAXLocator);
                }
                if ((ctxt->sax) && (ctxt->sax->startDocument) &&
                    (!ctxt->disableSAX))
                    ctxt->sax->startDocument(ctxt->userData);
                ctxt->instate = XML_PARSER_MISC;
		break;
            case XML_PARSER_START_TAG: {
	        const xmlChar *name;
		const xmlChar *prefix = NULL;
		const xmlChar *URI = NULL;
                int line = ctxt->input->line;
		int nbNs = 0;

		if ((!terminate) && (avail < 2))
		    goto done;
		cur = ctxt->input->cur[0];
	        if (cur != '<') {
		    xmlFatalErrMsg(ctxt, XML_ERR_DOCUMENT_EMPTY,
                                   "Start tag expected, '<' not found");
                    ctxt->instate = XML_PARSER_EOF;
                    xmlFinishDocument(ctxt);
		    goto done;
		}
		if ((!terminate) && (!xmlParseLookupGt(ctxt)))
                    goto done;
		if (ctxt->spaceNr == 0)
		    spacePush(ctxt, -1);
		else if (*ctxt->space == -2)
		    spacePush(ctxt, -1);
		else
		    spacePush(ctxt, *ctxt->space);
#ifdef LIBXML_SAX1_ENABLED
		if (ctxt->sax2)
#endif /* LIBXML_SAX1_ENABLED */
		    name = xmlParseStartTag2(ctxt, &prefix, &URI, &nbNs);
#ifdef LIBXML_SAX1_ENABLED
		else
		    name = xmlParseStartTag(ctxt);
#endif /* LIBXML_SAX1_ENABLED */
		if (name == NULL) {
		    spacePop(ctxt);
                    ctxt->instate = XML_PARSER_EOF;
                    xmlFinishDocument(ctxt);
		    goto done;
		}
#ifdef LIBXML_VALID_ENABLED
		/*
		 * [ VC: Root Element Type ]
		 * The Name in the document type declaration must match
		 * the element type of the root element.
		 */
		if (ctxt->validate && ctxt->wellFormed && ctxt->myDoc &&
		    ctxt->node && (ctxt->node == ctxt->myDoc->children))
		    ctxt->valid &= xmlValidateRoot(&ctxt->vctxt, ctxt->myDoc);
#endif /* LIBXML_VALID_ENABLED */

		/*
		 * Check for an Empty Element.
		 */
		if ((RAW == '/') && (NXT(1) == '>')) {
		    SKIP(2);

		    if (ctxt->sax2) {
			if ((ctxt->sax != NULL) &&
			    (ctxt->sax->endElementNs != NULL) &&
			    (!ctxt->disableSAX))
			    ctxt->sax->endElementNs(ctxt->userData, name,
			                            prefix, URI);
			if (nbNs > 0)
			    xmlParserNsPop(ctxt, nbNs);
#ifdef LIBXML_SAX1_ENABLED
		    } else {
			if ((ctxt->sax != NULL) &&
			    (ctxt->sax->endElement != NULL) &&
			    (!ctxt->disableSAX))
			    ctxt->sax->endElement(ctxt->userData, name);
#endif /* LIBXML_SAX1_ENABLED */
		    }
		    spacePop(ctxt);
		} else if (RAW == '>') {
		    NEXT;
                    nameNsPush(ctxt, name, prefix, URI, line, nbNs);
		} else {
		    xmlFatalErrMsgStr(ctxt, XML_ERR_GT_REQUIRED,
					 "Couldn't find end of Start Tag %s\n",
					 name);
		    nodePop(ctxt);
		    spacePop(ctxt);
                    if (nbNs > 0)
                        xmlParserNsPop(ctxt, nbNs);
		}

                if (ctxt->nameNr == 0)
                    ctxt->instate = XML_PARSER_EPILOG;
                else
                    ctxt->instate = XML_PARSER_CONTENT;
                break;
	    }
            case XML_PARSER_CONTENT: {
		cur = ctxt->input->cur[0];

		if (cur == '<') {
                    if ((!terminate) && (avail < 2))
                        goto done;
		    next = ctxt->input->cur[1];

                    if (next == '/') {
                        ctxt->instate = XML_PARSER_END_TAG;
                        break;
                    } else if (next == '?') {
                        if ((!terminate) &&
                            (!xmlParseLookupString(ctxt, 2, "?>", 2)))
                            goto done;
                        xmlParsePI(ctxt);
                        ctxt->instate = XML_PARSER_CONTENT;
                        break;
                    } else if (next == '!') {
                        if ((!terminate) && (avail < 3))
                            goto done;
                        next = ctxt->input->cur[2];

                        if (next == '-') {
                            if ((!terminate) && (avail < 4))
                                goto done;
                            if (ctxt->input->cur[3] == '-') {
                                if ((!terminate) &&
                                    (!xmlParseLookupString(ctxt, 4, "-->", 3)))
                                    goto done;
                                xmlParseComment(ctxt);
                                ctxt->instate = XML_PARSER_CONTENT;
                                break;
                            }
                        } else if (next == '[') {
                            if ((!terminate) && (avail < 9))
                                goto done;
                            if ((ctxt->input->cur[2] == '[') &&
                                (ctxt->input->cur[3] == 'C') &&
                                (ctxt->input->cur[4] == 'D') &&
                                (ctxt->input->cur[5] == 'A') &&
                                (ctxt->input->cur[6] == 'T') &&
                                (ctxt->input->cur[7] == 'A') &&
                                (ctxt->input->cur[8] == '[')) {
                                if ((!terminate) &&
                                    (!xmlParseLookupString(ctxt, 9, "]]>", 3)))
                                    goto done;
                                ctxt->instate = XML_PARSER_CDATA_SECTION;
                                xmlParseCDSect(ctxt);
                                ctxt->instate = XML_PARSER_CONTENT;
                                break;
                            }
                        }
                    }
		} else if (cur == '&') {
		    if ((!terminate) && (!xmlParseLookupChar(ctxt, ';')))
			goto done;
		    xmlParseReference(ctxt);
                    break;
		} else {
		    /* TODO Avoid the extra copy, handle directly !!! */
		    /*
		     * Goal of the following test is:
		     *  - minimize calls to the SAX 'character' callback
		     *    when they are mergeable
		     *  - handle an problem for isBlank when we only parse
		     *    a sequence of blank chars and the next one is
		     *    not available to check against '<' presence.
		     *  - tries to homogenize the differences in SAX
		     *    callbacks between the push and pull versions
		     *    of the parser.
		     */
		    if (avail < XML_PARSER_BIG_BUFFER_SIZE) {
			if ((!terminate) && (!xmlParseLookupCharData(ctxt)))
			    goto done;
                    }
                    ctxt->checkIndex = 0;
		    xmlParseCharDataInternal(ctxt, !terminate);
                    break;
		}

                ctxt->instate = XML_PARSER_START_TAG;
		break;
	    }
            case XML_PARSER_END_TAG:
		if ((!terminate) && (!xmlParseLookupChar(ctxt, '>')))
		    goto done;
		if (ctxt->sax2) {
	            xmlParseEndTag2(ctxt, &ctxt->pushTab[ctxt->nameNr - 1]);
		    nameNsPop(ctxt);
		}
#ifdef LIBXML_SAX1_ENABLED
		  else
		    xmlParseEndTag1(ctxt, 0);
#endif /* LIBXML_SAX1_ENABLED */
		if (ctxt->nameNr == 0) {
		    ctxt->instate = XML_PARSER_EPILOG;
		} else {
		    ctxt->instate = XML_PARSER_CONTENT;
		}
		break;
            case XML_PARSER_MISC:
            case XML_PARSER_PROLOG:
            case XML_PARSER_EPILOG:
		SKIP_BLANKS;
                avail = ctxt->input->end - ctxt->input->cur;
		if (avail < 1)
		    goto done;
		if (ctxt->input->cur[0] == '<') {
                    if ((!terminate) && (avail < 2))
                        goto done;
                    next = ctxt->input->cur[1];
                    if (next == '?') {
                        if ((!terminate) &&
                            (!xmlParseLookupString(ctxt, 2, "?>", 2)))
                            goto done;
                        xmlParsePI(ctxt);
                        break;
                    } else if (next == '!') {
                        if ((!terminate) && (avail < 3))
                            goto done;

                        if (ctxt->input->cur[2] == '-') {
                            if ((!terminate) && (avail < 4))
                                goto done;
                            if (ctxt->input->cur[3] == '-') {
                                if ((!terminate) &&
                                    (!xmlParseLookupString(ctxt, 4, "-->", 3)))
                                    goto done;
                                xmlParseComment(ctxt);
                                break;
                            }
                        } else if (ctxt->instate == XML_PARSER_MISC) {
                            if ((!terminate) && (avail < 9))
                                goto done;
                            if ((ctxt->input->cur[2] == 'D') &&
                                (ctxt->input->cur[3] == 'O') &&
                                (ctxt->input->cur[4] == 'C') &&
                                (ctxt->input->cur[5] == 'T') &&
                                (ctxt->input->cur[6] == 'Y') &&
                                (ctxt->input->cur[7] == 'P') &&
                                (ctxt->input->cur[8] == 'E')) {
                                if ((!terminate) && (!xmlParseLookupGt(ctxt)))
                                    goto done;
                                ctxt->inSubset = 1;
                                xmlParseDocTypeDecl(ctxt);
                                if (RAW == '[') {
                                    ctxt->instate = XML_PARSER_DTD;
                                } else {
                                    if (RAW == '>')
                                        NEXT;
                                    /*
                                     * Create and update the external subset.
                                     */
                                    ctxt->inSubset = 2;
                                    if ((ctxt->sax != NULL) &&
                                        (!ctxt->disableSAX) &&
                                        (ctxt->sax->externalSubset != NULL))
                                        ctxt->sax->externalSubset(
                                                ctxt->userData,
                                                ctxt->intSubName,
                                                ctxt->extSubSystem,
                                                ctxt->extSubURI);
                                    ctxt->inSubset = 0;
                                    xmlCleanSpecialAttr(ctxt);
                                    ctxt->instate = XML_PARSER_PROLOG;
                                }
                                break;
                            }
                        }
                    }
                }

                if (ctxt->instate == XML_PARSER_EPILOG) {
                    if (ctxt->errNo == XML_ERR_OK)
                        xmlFatalErr(ctxt, XML_ERR_DOCUMENT_END, NULL);
		    ctxt->instate = XML_PARSER_EOF;
                    xmlFinishDocument(ctxt);
                } else {
		    ctxt->instate = XML_PARSER_START_TAG;
		}
		break;
            case XML_PARSER_DTD: {
                if ((!terminate) && (!xmlParseLookupInternalSubset(ctxt)))
                    goto done;
		xmlParseInternalSubset(ctxt);
		ctxt->inSubset = 2;
		if ((ctxt->sax != NULL) && (!ctxt->disableSAX) &&
		    (ctxt->sax->externalSubset != NULL))
		    ctxt->sax->externalSubset(ctxt->userData, ctxt->intSubName,
			    ctxt->extSubSystem, ctxt->extSubURI);
		ctxt->inSubset = 0;
		xmlCleanSpecialAttr(ctxt);
		ctxt->instate = XML_PARSER_PROLOG;
                break;
	    }
            default:
                xmlFatalErrMsg(ctxt, XML_ERR_INTERNAL_ERROR,
			"PP: internal error\n");
		ctxt->instate = XML_PARSER_EOF;
		break;
	}
    }
done:
    return(ret);
}

/**
 * Parse a chunk of memory in push parser mode.
 *
 * Assumes that the parser context was initialized with
 * #xmlCreatePushParserCtxt.
 *
 * The last chunk, which will often be empty, must be marked with
 * the `terminate` flag. With the default SAX callbacks, the resulting
 * document will be available in ctxt->myDoc. This pointer will not
 * be freed when calling #xmlFreeParserCtxt and must be freed by the
 * caller. If the document isn't well-formed, it will still be returned
 * in ctxt->myDoc.
 *
 * As an exception, #xmlCtxtResetPush will free the document in
 * ctxt->myDoc. So ctxt->myDoc should be set to NULL after extracting
 * the document.
 *
 * Since 2.14.0, #xmlCtxtGetDocument can be used to retrieve the
 * result document.
 *
 * @param ctxt  an XML parser context
 * @param chunk  chunk of memory
 * @param size  size of chunk in bytes
 * @param terminate  last chunk indicator
 * @returns an xmlParserErrors code (0 on success).
 */
int
xmlParseChunk(xmlParserCtxt *ctxt, const char *chunk, int size,
              int terminate) {
    size_t curBase;
    size_t maxLength;
    size_t pos;
    int end_in_lf = 0;
    int res;

    if ((ctxt == NULL) || (size < 0))
        return(XML_ERR_ARGUMENT);
    if ((chunk == NULL) && (size > 0))
        return(XML_ERR_ARGUMENT);
    if ((ctxt->input == NULL) || (ctxt->input->buf == NULL))
        return(XML_ERR_ARGUMENT);
    if (ctxt->disableSAX != 0)
        return(ctxt->errNo);

    ctxt->input->flags |= XML_INPUT_PROGRESSIVE;
    if (ctxt->instate == XML_PARSER_START)
        xmlCtxtInitializeLate(ctxt);
    if ((size > 0) && (chunk != NULL) && (!terminate) &&
        (chunk[size - 1] == '\r')) {
	end_in_lf = 1;
	size--;
    }

    /*
     * Also push an empty chunk to make sure that the raw buffer
     * will be flushed if there is an encoder.
     */
    pos = ctxt->input->cur - ctxt->input->base;
    res = xmlParserInputBufferPush(ctxt->input->buf, size, chunk);
    xmlBufUpdateInput(ctxt->input->buf->buffer, ctxt->input, pos);
    if (res < 0) {
        xmlCtxtErrIO(ctxt, ctxt->input->buf->error, NULL);
        return(ctxt->errNo);
    }

    xmlParseTryOrFinish(ctxt, terminate);

    curBase = ctxt->input->cur - ctxt->input->base;
    maxLength = (ctxt->options & XML_PARSE_HUGE) ?
                XML_MAX_HUGE_LENGTH :
                XML_MAX_LOOKUP_LIMIT;
    if (curBase > maxLength) {
        xmlFatalErr(ctxt, XML_ERR_RESOURCE_LIMIT,
                    "Buffer size limit exceeded, try XML_PARSE_HUGE\n");
    }

    if ((ctxt->errNo != XML_ERR_OK) && (ctxt->disableSAX != 0))
        return(ctxt->errNo);

    if (end_in_lf == 1) {
	pos = ctxt->input->cur - ctxt->input->base;
	res = xmlParserInputBufferPush(ctxt->input->buf, 1, "\r");
	xmlBufUpdateInput(ctxt->input->buf->buffer, ctxt->input, pos);
        if (res < 0) {
            xmlCtxtErrIO(ctxt, ctxt->input->buf->error, NULL);
            return(ctxt->errNo);
        }
    }
    if (terminate) {
	/*
	 * Check for termination
	 */
        if ((ctxt->instate != XML_PARSER_EOF) &&
            (ctxt->instate != XML_PARSER_EPILOG)) {
            if (ctxt->nameNr > 0) {
                const xmlChar *name = ctxt->nameTab[ctxt->nameNr - 1];
                int line = ctxt->pushTab[ctxt->nameNr - 1].line;
                xmlFatalErrMsgStrIntStr(ctxt, XML_ERR_TAG_NOT_FINISHED,
                        "Premature end of data in tag %s line %d\n",
                        name, line, NULL);
            } else if (ctxt->instate == XML_PARSER_START) {
                xmlFatalErr(ctxt, XML_ERR_DOCUMENT_EMPTY, NULL);
            } else {
                xmlFatalErrMsg(ctxt, XML_ERR_DOCUMENT_EMPTY,
                               "Start tag expected, '<' not found\n");
            }
        } else {
            xmlParserCheckEOF(ctxt, XML_ERR_DOCUMENT_END);
        }
	if (ctxt->instate != XML_PARSER_EOF) {
            ctxt->instate = XML_PARSER_EOF;
            xmlFinishDocument(ctxt);
	}
    }
    if (ctxt->wellFormed == 0)
	return((xmlParserErrors) ctxt->errNo);
    else
        return(0);
}

/************************************************************************
 *									*
 *		I/O front end functions to the parser			*
 *									*
 ************************************************************************/

/**
 * Create a parser context for using the XML parser in push mode.
 * See #xmlParseChunk.
 *
 * Passing an initial chunk is useless and deprecated.
 *
 * The push parser doesn't support recovery mode or the
 * XML_PARSE_NOBLANKS option.
 *
 * `filename` is used as base URI to fetch external entities and for
 * error reports.
 *
 * @param sax  a SAX handler (optional)
 * @param user_data  user data for SAX callbacks (optional)
 * @param chunk  initial chunk (optional, deprecated)
 * @param size  size of initial chunk in bytes
 * @param filename  file name or URI (optional)
 * @returns the new parser context or NULL if a memory allocation
 * failed.
 */

xmlParserCtxt *
xmlCreatePushParserCtxt(xmlSAXHandler *sax, void *user_data,
                        const char *chunk, int size, const char *filename) {
    xmlParserCtxtPtr ctxt;
    xmlParserInputPtr input;

    ctxt = xmlNewSAXParserCtxt(sax, user_data);
    if (ctxt == NULL)
	return(NULL);

    ctxt->options &= ~XML_PARSE_NODICT;
    ctxt->dictNames = 1;

    input = xmlNewPushInput(filename, chunk, size);
    if (input == NULL) {
	xmlFreeParserCtxt(ctxt);
	return(NULL);
    }
    if (xmlCtxtPushInput(ctxt, input) < 0) {
        xmlFreeInputStream(input);
        xmlFreeParserCtxt(ctxt);
        return(NULL);
    }

    return(ctxt);
}
#endif /* LIBXML_PUSH_ENABLED */

/**
 * Blocks further parser processing
 *
 * @param ctxt  an XML parser context
 */
void
xmlStopParser(xmlParserCtxt *ctxt) {
    if (ctxt == NULL)
        return;

    /* This stops the parser */
    ctxt->disableSAX = 2;

    /*
     * xmlStopParser is often called from error handlers,
     * so we can't raise an error here to avoid infinite
     * loops. Just make sure that an error condition is
     * reported.
     */
    if (ctxt->errNo == XML_ERR_OK) {
        ctxt->errNo = XML_ERR_USER_STOP;
        ctxt->lastError.code = XML_ERR_USER_STOP;
        ctxt->wellFormed = 0;
    }
}

/**
 * Create a parser context for using the XML parser with an existing
 * I/O stream
 *
 * @param sax  a SAX handler (optional)
 * @param user_data  user data for SAX callbacks (optional)
 * @param ioread  an I/O read function
 * @param ioclose  an I/O close function (optional)
 * @param ioctx  an I/O handler
 * @param enc  the charset encoding if known (deprecated)
 * @returns the new parser context or NULL
 */
xmlParserCtxt *
xmlCreateIOParserCtxt(xmlSAXHandler *sax, void *user_data,
                      xmlInputReadCallback ioread,
                      xmlInputCloseCallback ioclose,
                      void *ioctx, xmlCharEncoding enc) {
    xmlParserCtxtPtr ctxt;
    xmlParserInputPtr input;
    const char *encoding;

    ctxt = xmlNewSAXParserCtxt(sax, user_data);
    if (ctxt == NULL)
	return(NULL);

    encoding = xmlGetCharEncodingName(enc);
    input = xmlCtxtNewInputFromIO(ctxt, NULL, ioread, ioclose, ioctx,
                                  encoding, 0);
    if (input == NULL) {
	xmlFreeParserCtxt(ctxt);
        return (NULL);
    }
    if (xmlCtxtPushInput(ctxt, input) < 0) {
        xmlFreeInputStream(input);
        xmlFreeParserCtxt(ctxt);
        return(NULL);
    }

    return(ctxt);
}

#ifdef LIBXML_VALID_ENABLED
/************************************************************************
 *									*
 *		Front ends when parsing a DTD				*
 *									*
 ************************************************************************/

/**
 * Parse a DTD.
 *
 * Option XML_PARSE_DTDLOAD should be enabled in the parser context
 * to make external entities work.
 *
 * @since 2.14.0
 *
 * @param ctxt  a parser context
 * @param input  a parser input
 * @param publicId  public ID of the DTD (optional)
 * @param systemId  system ID of the DTD (optional)
 * @returns the resulting xmlDtd or NULL in case of error.
 * `input` will be freed by the function in any case.
 */
xmlDtd *
xmlCtxtParseDtd(xmlParserCtxt *ctxt, xmlParserInput *input,
                const xmlChar *publicId, const xmlChar *systemId) {
    xmlDtdPtr ret = NULL;

    if ((ctxt == NULL) || (input == NULL)) {
        xmlFatalErr(ctxt, XML_ERR_ARGUMENT, NULL);
        xmlFreeInputStream(input);
        return(NULL);
    }

    if (xmlCtxtPushInput(ctxt, input) < 0) {
        xmlFreeInputStream(input);
        return(NULL);
    }

    if (publicId == NULL)
        publicId = BAD_CAST "none";
    if (systemId == NULL)
        systemId = BAD_CAST "none";

    ctxt->myDoc = xmlNewDoc(BAD_CAST "1.0");
    if (ctxt->myDoc == NULL) {
        xmlErrMemory(ctxt);
        goto error;
    }
    ctxt->myDoc->properties = XML_DOC_INTERNAL;
    ctxt->myDoc->extSubset = xmlNewDtd(ctxt->myDoc, BAD_CAST "none",
                                       publicId, systemId);
    if (ctxt->myDoc->extSubset == NULL) {
        xmlErrMemory(ctxt);
        xmlFreeDoc(ctxt->myDoc);
        goto error;
    }

    xmlParseExternalSubset(ctxt, publicId, systemId);

    if (ctxt->wellFormed) {
        ret = ctxt->myDoc->extSubset;
        ctxt->myDoc->extSubset = NULL;
        if (ret != NULL) {
            xmlNodePtr tmp;

            ret->doc = NULL;
            tmp = ret->children;
            while (tmp != NULL) {
                tmp->doc = NULL;
                tmp = tmp->next;
            }
        }
    } else {
        ret = NULL;
    }
    xmlFreeDoc(ctxt->myDoc);
    ctxt->myDoc = NULL;

error:
    xmlFreeInputStream(xmlCtxtPopInput(ctxt));

    return(ret);
}

/**
 * Load and parse a DTD
 *
 * @deprecated Use #xmlCtxtParseDtd.
 *
 * @param sax  the SAX handler block or NULL
 * @param input  an Input Buffer
 * @param enc  the charset encoding if known
 * @returns the resulting xmlDtd or NULL in case of error.
 * `input` will be freed by the function in any case.
 */

xmlDtd *
xmlIOParseDTD(xmlSAXHandler *sax, xmlParserInputBuffer *input,
	      xmlCharEncoding enc) {
    xmlDtdPtr ret = NULL;
    xmlParserCtxtPtr ctxt;
    xmlParserInputPtr pinput = NULL;

    if (input == NULL)
	return(NULL);

    ctxt = xmlNewSAXParserCtxt(sax, NULL);
    if (ctxt == NULL) {
        xmlFreeParserInputBuffer(input);
	return(NULL);
    }
    xmlCtxtSetOptions(ctxt, XML_PARSE_DTDLOAD);

    /*
     * generate a parser input from the I/O handler
     */

    pinput = xmlNewIOInputStream(ctxt, input, XML_CHAR_ENCODING_NONE);
    if (pinput == NULL) {
        xmlFreeParserInputBuffer(input);
	xmlFreeParserCtxt(ctxt);
	return(NULL);
    }

    if (enc != XML_CHAR_ENCODING_NONE) {
        xmlSwitchEncoding(ctxt, enc);
    }

    ret = xmlCtxtParseDtd(ctxt, pinput, NULL, NULL);

    xmlFreeParserCtxt(ctxt);
    return(ret);
}

/**
 * Load and parse an external subset.
 *
 * @deprecated Use #xmlCtxtParseDtd.
 *
 * @param sax  the SAX handler block
 * @param publicId  public identifier of the DTD (optional)
 * @param systemId  system identifier (URL) of the DTD
 * @returns the resulting xmlDtd or NULL in case of error.
 */

xmlDtd *
xmlSAXParseDTD(xmlSAXHandler *sax, const xmlChar *publicId,
               const xmlChar *systemId) {
    xmlDtdPtr ret = NULL;
    xmlParserCtxtPtr ctxt;
    xmlParserInputPtr input = NULL;
    xmlChar* systemIdCanonic;

    if ((publicId == NULL) && (systemId == NULL)) return(NULL);

    ctxt = xmlNewSAXParserCtxt(sax, NULL);
    if (ctxt == NULL) {
	return(NULL);
    }
    xmlCtxtSetOptions(ctxt, XML_PARSE_DTDLOAD);

    /*
     * Canonicalise the system ID
     */
    systemIdCanonic = xmlCanonicPath(systemId);
    if ((systemId != NULL) && (systemIdCanonic == NULL)) {
	xmlFreeParserCtxt(ctxt);
	return(NULL);
    }

    /*
     * Ask the Entity resolver to load the damn thing
     */

    if ((ctxt->sax != NULL) && (ctxt->sax->resolveEntity != NULL))
	input = ctxt->sax->resolveEntity(ctxt->userData, publicId,
	                                 systemIdCanonic);
    if (input == NULL) {
	xmlFreeParserCtxt(ctxt);
	if (systemIdCanonic != NULL)
	    xmlFree(systemIdCanonic);
	return(NULL);
    }

    if (input->filename == NULL)
	input->filename = (char *) systemIdCanonic;
    else
	xmlFree(systemIdCanonic);

    ret = xmlCtxtParseDtd(ctxt, input, publicId, systemId);

    xmlFreeParserCtxt(ctxt);
    return(ret);
}


/**
 * Load and parse an external subset.
 *
 * @param publicId  public identifier of the DTD (optional)
 * @param systemId  system identifier (URL) of the DTD
 * @returns the resulting xmlDtd or NULL in case of error.
 */

xmlDtd *
xmlParseDTD(const xmlChar *publicId, const xmlChar *systemId) {
    return(xmlSAXParseDTD(NULL, publicId, systemId));
}
#endif /* LIBXML_VALID_ENABLED */

/************************************************************************
 *									*
 *		Front ends when parsing an Entity			*
 *									*
 ************************************************************************/

static xmlNodePtr
xmlCtxtParseContentInternal(xmlParserCtxtPtr ctxt, xmlParserInputPtr input,
                            int hasTextDecl, int buildTree) {
    xmlNodePtr root = NULL;
    xmlNodePtr list = NULL;
    xmlChar *rootName = BAD_CAST "#root";
    int result;

    if (buildTree) {
        root = xmlNewDocNode(ctxt->myDoc, NULL, rootName, NULL);
        if (root == NULL) {
            xmlErrMemory(ctxt);
            goto error;
        }
    }

    if (xmlCtxtPushInput(ctxt, input) < 0)
        goto error;

    nameNsPush(ctxt, rootName, NULL, NULL, 0, 0);
    spacePush(ctxt, -1);

    if (buildTree)
        nodePush(ctxt, root);

    if (hasTextDecl) {
        xmlDetectEncoding(ctxt);

        /*
         * Parse a possible text declaration first
         */
        if ((CMP5(CUR_PTR, '<', '?', 'x', 'm', 'l')) &&
            (IS_BLANK_CH(NXT(5)))) {
            xmlParseTextDecl(ctxt);
            /*
             * An XML-1.0 document can't reference an entity not XML-1.0
             */
            if ((xmlStrEqual(ctxt->version, BAD_CAST "1.0")) &&
                (!xmlStrEqual(ctxt->input->version, BAD_CAST "1.0"))) {
                xmlFatalErrMsg(ctxt, XML_ERR_VERSION_MISMATCH,
                               "Version mismatch between document and "
                               "entity\n");
            }
        }
    }

    xmlParseContentInternal(ctxt);

    if (ctxt->input->cur < ctxt->input->end)
	xmlFatalErr(ctxt, XML_ERR_NOT_WELL_BALANCED, NULL);

    if ((ctxt->wellFormed) ||
        ((ctxt->recovery) && (!xmlCtxtIsCatastrophicError(ctxt)))) {
        if (root != NULL) {
            xmlNodePtr cur;

            /*
             * Unlink newly created node list.
             */
            list = root->children;
            root->children = NULL;
            root->last = NULL;
            for (cur = list; cur != NULL; cur = cur->next)
                cur->parent = NULL;
        }
    }

    /*
     * Read the rest of the stream in case of errors. We want
     * to account for the whole entity size.
     */
    do {
        ctxt->input->cur = ctxt->input->end;
        xmlParserShrink(ctxt);
        result = xmlParserGrow(ctxt);
    } while (result > 0);

    if (buildTree)
        nodePop(ctxt);

    namePop(ctxt);
    spacePop(ctxt);

    xmlCtxtPopInput(ctxt);

error:
    xmlFreeNode(root);

    return(list);
}

static void
xmlCtxtParseEntity(xmlParserCtxtPtr ctxt, xmlEntityPtr ent) {
    xmlParserInputPtr input;
    xmlNodePtr list;
    unsigned long consumed;
    int isExternal;
    int buildTree;
    int oldMinNsIndex;
    int oldNodelen, oldNodemem;

    isExternal = (ent->etype == XML_EXTERNAL_GENERAL_PARSED_ENTITY);
    buildTree = (ctxt->node != NULL);

    /*
     * Recursion check
     */
    if (ent->flags & XML_ENT_EXPANDING) {
        xmlFatalErr(ctxt, XML_ERR_ENTITY_LOOP, NULL);
        goto error;
    }

    /*
     * Load entity
     */
    input = xmlNewEntityInputStream(ctxt, ent);
    if (input == NULL)
        goto error;

    /*
     * When building a tree, we need to limit the scope of namespace
     * declarations, so that entities don't reference xmlNs structs
     * from the parent of a reference.
     */
    oldMinNsIndex = ctxt->nsdb->minNsIndex;
    if (buildTree)
        ctxt->nsdb->minNsIndex = ctxt->nsNr;

    oldNodelen = ctxt->nodelen;
    oldNodemem = ctxt->nodemem;
    ctxt->nodelen = 0;
    ctxt->nodemem = 0;

    /*
     * Parse content
     *
     * This initiates a recursive call chain:
     *
     * - xmlCtxtParseContentInternal
     * - xmlParseContentInternal
     * - xmlParseReference
     * - xmlCtxtParseEntity
     *
     * The nesting depth is limited by the maximum number of inputs,
     * see xmlCtxtPushInput.
     *
     * It's possible to make this non-recursive (minNsIndex must be
     * stored in the input struct) at the expense of code readability.
     */

    ent->flags |= XML_ENT_EXPANDING;

    list = xmlCtxtParseContentInternal(ctxt, input, isExternal, buildTree);

    ent->flags &= ~XML_ENT_EXPANDING;

    ctxt->nsdb->minNsIndex = oldMinNsIndex;
    ctxt->nodelen = oldNodelen;
    ctxt->nodemem = oldNodemem;

    /*
     * Entity size accounting
     */
    consumed = input->consumed;
    xmlSaturatedAddSizeT(&consumed, input->end - input->base);

    if ((ent->flags & XML_ENT_CHECKED) == 0)
        xmlSaturatedAdd(&ent->expandedSize, consumed);

    if ((ent->flags & XML_ENT_PARSED) == 0) {
        if (isExternal)
            xmlSaturatedAdd(&ctxt->sizeentities, consumed);

        ent->children = list;

        while (list != NULL) {
            list->parent = (xmlNodePtr) ent;

            /*
             * Downstream code like the nginx xslt module can set
             * ctxt->myDoc->extSubset to a separate DTD, so the entity
             * might have a different or a NULL document.
             */
            if (list->doc != ent->doc)
                xmlSetTreeDoc(list, ent->doc);

            if (list->next == NULL)
                ent->last = list;
            list = list->next;
        }
    } else {
        xmlFreeNodeList(list);
    }

    xmlFreeInputStream(input);

error:
    ent->flags |= XML_ENT_PARSED | XML_ENT_CHECKED;
}

/**
 * Parse an external general entity within an existing parsing context
 * An external general parsed entity is well-formed if it matches the
 * production labeled extParsedEnt.
 *
 *     [78] extParsedEnt ::= TextDecl? content
 *
 * @param ctxt  the existing parsing context
 * @param URL  the URL for the entity to load
 * @param ID  the System ID for the entity to load
 * @param listOut  the return value for the set of parsed nodes
 * @returns 0 if the entity is well formed, -1 in case of args problem and
 *    the parser error code otherwise
 */

int
xmlParseCtxtExternalEntity(xmlParserCtxt *ctxt, const xmlChar *URL,
                           const xmlChar *ID, xmlNode **listOut) {
    xmlParserInputPtr input;
    xmlNodePtr list;

    if (listOut != NULL)
        *listOut = NULL;

    if (ctxt == NULL)
        return(XML_ERR_ARGUMENT);

    input = xmlLoadResource(ctxt, (char *) URL, (char *) ID,
                            XML_RESOURCE_GENERAL_ENTITY);
    if (input == NULL)
        return(ctxt->errNo);

    xmlCtxtInitializeLate(ctxt);

    list = xmlCtxtParseContentInternal(ctxt, input, /* hasTextDecl */ 1, 1);
    if (listOut != NULL)
        *listOut = list;
    else
        xmlFreeNodeList(list);

    xmlFreeInputStream(input);
    return(ctxt->errNo);
}

#ifdef LIBXML_SAX1_ENABLED
/**
 * Parse an external general entity
 * An external general parsed entity is well-formed if it matches the
 * production labeled extParsedEnt.
 *
 * This function uses deprecated global variables to set parser options
 * which default to XML_PARSE_NODICT.
 *
 * @deprecated Use #xmlParseCtxtExternalEntity.
 *
 *     [78] extParsedEnt ::= TextDecl? content
 *
 * @param doc  the document the chunk pertains to
 * @param sax  the SAX handler block (possibly NULL)
 * @param user_data  The user data returned on SAX callbacks (possibly NULL)
 * @param depth  Used for loop detection, use 0
 * @param URL  the URL for the entity to load
 * @param ID  the System ID for the entity to load
 * @param list  the return value for the set of parsed nodes
 * @returns 0 if the entity is well formed, -1 in case of args problem and
 *    the parser error code otherwise
 */

int
xmlParseExternalEntity(xmlDoc *doc, xmlSAXHandler *sax, void *user_data,
	  int depth, const xmlChar *URL, const xmlChar *ID, xmlNode **list) {
    xmlParserCtxtPtr ctxt;
    int ret;

    if (list != NULL)
        *list = NULL;

    if (doc == NULL)
        return(XML_ERR_ARGUMENT);

    ctxt = xmlNewSAXParserCtxt(sax, user_data);
    if (ctxt == NULL)
        return(XML_ERR_NO_MEMORY);

    ctxt->depth = depth;
    ctxt->myDoc = doc;
    ret = xmlParseCtxtExternalEntity(ctxt, URL, ID, list);

    xmlFreeParserCtxt(ctxt);
    return(ret);
}

/**
 * Parse a well-balanced chunk of an XML document
 * called by the parser
 * The allowed sequence for the Well Balanced Chunk is the one defined by
 * the content production in the XML grammar:
 *
 *     [43] content ::= (element | CharData | Reference | CDSect | PI |
 *                       Comment)*
 *
 * This function uses deprecated global variables to set parser options
 * which default to XML_PARSE_NODICT.
 *
 * @param doc  the document the chunk pertains to (must not be NULL)
 * @param sax  the SAX handler block (possibly NULL)
 * @param user_data  The user data returned on SAX callbacks (possibly NULL)
 * @param depth  Used for loop detection, use 0
 * @param string  the input string in UTF8 or ISO-Latin (zero terminated)
 * @param lst  the return value for the set of parsed nodes
 * @returns 0 if the chunk is well balanced, -1 in case of args problem and
 *    the parser error code otherwise
 */

int
xmlParseBalancedChunkMemory(xmlDoc *doc, xmlSAXHandler *sax,
     void *user_data, int depth, const xmlChar *string, xmlNode **lst) {
    return xmlParseBalancedChunkMemoryRecover( doc, sax, user_data,
                                                depth, string, lst, 0 );
}
#endif /* LIBXML_SAX1_ENABLED */

/**
 * Parse a well-balanced chunk of XML matching the 'content' production.
 *
 * Namespaces in scope of `node` and entities of `node`'s document are
 * recognized. When validating, the DTD of `node`'s document is used.
 *
 * Always consumes `input` even in error case.
 *
 * @since 2.14.0
 *
 * @param ctxt  parser context
 * @param input  parser input
 * @param node  target node or document
 * @param hasTextDecl  whether to parse text declaration
 * @returns a node list or NULL in case of error.
 */
xmlNode *
xmlCtxtParseContent(xmlParserCtxt *ctxt, xmlParserInput *input,
                    xmlNode *node, int hasTextDecl) {
    xmlDocPtr doc;
    xmlNodePtr cur, list = NULL;
    int nsnr = 0;
    xmlDictPtr oldDict;
    int oldOptions, oldDictNames, oldLoadSubset;

    if ((ctxt == NULL) || (input == NULL) || (node == NULL)) {
        xmlFatalErr(ctxt, XML_ERR_ARGUMENT, NULL);
        goto exit;
    }

    doc = node->doc;
    if (doc == NULL) {
        xmlFatalErr(ctxt, XML_ERR_ARGUMENT, NULL);
        goto exit;
    }

    switch (node->type) {
        case XML_ELEMENT_NODE:
        case XML_DOCUMENT_NODE:
        case XML_HTML_DOCUMENT_NODE:
            break;

        case XML_ATTRIBUTE_NODE:
        case XML_TEXT_NODE:
        case XML_CDATA_SECTION_NODE:
        case XML_ENTITY_REF_NODE:
        case XML_PI_NODE:
        case XML_COMMENT_NODE:
            for (cur = node->parent; cur != NULL; cur = node->parent) {
                if ((cur->type == XML_ELEMENT_NODE) ||
                    (cur->type == XML_DOCUMENT_NODE) ||
                    (cur->type == XML_HTML_DOCUMENT_NODE)) {
                    node = cur;
                    break;
                }
            }
            break;

        default:
            xmlFatalErr(ctxt, XML_ERR_ARGUMENT, NULL);
            goto exit;
    }

    xmlCtxtReset(ctxt);

    oldDict = ctxt->dict;
    oldOptions = ctxt->options;
    oldDictNames = ctxt->dictNames;
    oldLoadSubset = ctxt->loadsubset;

    /*
     * Use input doc's dict if present, else assure XML_PARSE_NODICT is set.
     */
    if (doc->dict != NULL) {
        ctxt->dict = doc->dict;
    } else {
        ctxt->options |= XML_PARSE_NODICT;
        ctxt->dictNames = 0;
    }

    /*
     * Disable IDs
     */
    ctxt->loadsubset |= XML_SKIP_IDS;
    ctxt->options |= XML_PARSE_SKIP_IDS;

    ctxt->myDoc = doc;

#ifdef LIBXML_HTML_ENABLED
    if (ctxt->html) {
        /*
         * When parsing in context, it makes no sense to add implied
         * elements like html/body/etc...
         */
        ctxt->options |= HTML_PARSE_NOIMPLIED;

        list = htmlCtxtParseContentInternal(ctxt, input);
    } else
#endif
    {
        xmlCtxtInitializeLate(ctxt);

        /*
         * initialize the SAX2 namespaces stack
         */
        cur = node;
        while ((cur != NULL) && (cur->type == XML_ELEMENT_NODE)) {
            xmlNsPtr ns = cur->nsDef;
            xmlHashedString hprefix, huri;

            while (ns != NULL) {
                hprefix = xmlDictLookupHashed(ctxt->dict, ns->prefix, -1);
                huri = xmlDictLookupHashed(ctxt->dict, ns->href, -1);
                if (xmlParserNsPush(ctxt, &hprefix, &huri, ns, 1) > 0)
                    nsnr++;
                ns = ns->next;
            }
            cur = cur->parent;
        }

        list = xmlCtxtParseContentInternal(ctxt, input, hasTextDecl, 1);

        if (nsnr > 0)
            xmlParserNsPop(ctxt, nsnr);
    }

    ctxt->dict = oldDict;
    ctxt->options = oldOptions;
    ctxt->dictNames = oldDictNames;
    ctxt->loadsubset = oldLoadSubset;
    ctxt->myDoc = NULL;
    ctxt->node = NULL;

exit:
    xmlFreeInputStream(input);
    return(list);
}

/**
 * Parse a well-balanced chunk of an XML document
 * within the context (DTD, namespaces, etc ...) of the given node.
 *
 * The allowed sequence for the data is a Well Balanced Chunk defined by
 * the content production in the XML grammar:
 *
 *     [43] content ::= (element | CharData | Reference | CDSect | PI |
 *                       Comment)*
 *
 * This function assumes the encoding of `node`'s document which is
 * typically not what you want. A better alternative is
 * #xmlCtxtParseContent.
 *
 * @param node  the context node
 * @param data  the input string
 * @param datalen  the input string length in bytes
 * @param options  a combination of xmlParserOption
 * @param listOut  the return value for the set of parsed nodes
 * @returns XML_ERR_OK if the chunk is well balanced, and the parser
 * error code otherwise
 */
xmlParserErrors
xmlParseInNodeContext(xmlNode *node, const char *data, int datalen,
                      int options, xmlNode **listOut) {
    xmlParserCtxtPtr ctxt;
    xmlParserInputPtr input;
    xmlDocPtr doc;
    xmlNodePtr list;
    xmlParserErrors ret;

    if (listOut == NULL)
        return(XML_ERR_INTERNAL_ERROR);
    *listOut = NULL;

    if ((node == NULL) || (data == NULL) || (datalen < 0))
        return(XML_ERR_INTERNAL_ERROR);

    doc = node->doc;
    if (doc == NULL)
        return(XML_ERR_INTERNAL_ERROR);

#ifdef LIBXML_HTML_ENABLED
    if (doc->type == XML_HTML_DOCUMENT_NODE) {
        ctxt = htmlNewParserCtxt();
    }
    else
#endif
        ctxt = xmlNewParserCtxt();

    if (ctxt == NULL)
        return(XML_ERR_NO_MEMORY);

    input = xmlCtxtNewInputFromMemory(ctxt, NULL, data, datalen,
                                      (const char *) doc->encoding,
                                      XML_INPUT_BUF_STATIC);
    if (input == NULL) {
        xmlFreeParserCtxt(ctxt);
        return(XML_ERR_NO_MEMORY);
    }

    xmlCtxtUseOptions(ctxt, options);

    list = xmlCtxtParseContent(ctxt, input, node, /* hasTextDecl */ 0);

    if (list == NULL) {
        ret = ctxt->errNo;
        if (ret == XML_ERR_ARGUMENT)
            ret = XML_ERR_INTERNAL_ERROR;
    } else {
        ret = XML_ERR_OK;
        *listOut = list;
    }

    xmlFreeParserCtxt(ctxt);

    return(ret);
}

#ifdef LIBXML_SAX1_ENABLED
/**
 * Parse a well-balanced chunk of an XML document
 *
 * The allowed sequence for the Well Balanced Chunk is the one defined by
 * the content production in the XML grammar:
 *
 *     [43] content ::= (element | CharData | Reference | CDSect | PI |
 *                       Comment)*
 *
 * In case recover is set to 1, the nodelist will not be empty even if
 * the parsed chunk is not well balanced, assuming the parsing succeeded to
 * some extent.
 *
 * This function uses deprecated global variables to set parser options
 * which default to XML_PARSE_NODICT.
 *
 * @param doc  the document the chunk pertains to (must not be NULL)
 * @param sax  the SAX handler block (possibly NULL)
 * @param user_data  The user data returned on SAX callbacks (possibly NULL)
 * @param depth  Used for loop detection, use 0
 * @param string  the input string in UTF8 or ISO-Latin (zero terminated)
 * @param listOut  the return value for the set of parsed nodes
 * @param recover  return nodes even if the data is broken (use 0)
 * @returns 0 if the chunk is well balanced, or thehe parser error code
 * otherwise.
 */
int
xmlParseBalancedChunkMemoryRecover(xmlDoc *doc, xmlSAXHandler *sax,
     void *user_data, int depth, const xmlChar *string, xmlNode **listOut,
     int recover) {
    xmlParserCtxtPtr ctxt;
    xmlParserInputPtr input;
    xmlNodePtr list;
    int ret;

    if (listOut != NULL)
        *listOut = NULL;

    if (string == NULL)
        return(XML_ERR_ARGUMENT);

    ctxt = xmlNewSAXParserCtxt(sax, user_data);
    if (ctxt == NULL)
        return(XML_ERR_NO_MEMORY);

    xmlCtxtInitializeLate(ctxt);

    ctxt->depth = depth;
    ctxt->myDoc = doc;
    if (recover) {
        ctxt->options |= XML_PARSE_RECOVER;
        ctxt->recovery = 1;
    }

    input = xmlNewStringInputStream(ctxt, string);
    if (input == NULL) {
        ret = ctxt->errNo;
        goto error;
    }

    list = xmlCtxtParseContentInternal(ctxt, input, /* hasTextDecl */ 0, 1);
    if (listOut != NULL)
        *listOut = list;
    else
        xmlFreeNodeList(list);

    if (!ctxt->wellFormed)
        ret = ctxt->errNo;
    else
        ret = XML_ERR_OK;

error:
    xmlFreeInputStream(input);
    xmlFreeParserCtxt(ctxt);
    return(ret);
}

/**
 * Parse an XML external entity out of context and build a tree.
 * It use the given SAX function block to handle the parsing callback.
 * If sax is NULL, fallback to the default DOM tree building routines.
 *
 * @deprecated Don't use.
 *
 *     [78] extParsedEnt ::= TextDecl? content
 *
 * This correspond to a "Well Balanced" chunk
 *
 * This function uses deprecated global variables to set parser options
 * which default to XML_PARSE_NODICT.
 *
 * @param sax  the SAX handler block
 * @param filename  the filename
 * @returns the resulting document tree
 */

xmlDoc *
xmlSAXParseEntity(xmlSAXHandler *sax, const char *filename) {
    xmlDocPtr ret;
    xmlParserCtxtPtr ctxt;

    ctxt = xmlCreateFileParserCtxt(filename);
    if (ctxt == NULL) {
	return(NULL);
    }
    if (sax != NULL) {
        if (sax->initialized == XML_SAX2_MAGIC) {
            *ctxt->sax = *sax;
        } else {
            memset(ctxt->sax, 0, sizeof(*ctxt->sax));
            memcpy(ctxt->sax, sax, sizeof(xmlSAXHandlerV1));
        }
        ctxt->userData = NULL;
    }

    xmlParseExtParsedEnt(ctxt);

    if (ctxt->wellFormed) {
	ret = ctxt->myDoc;
    } else {
        ret = NULL;
        xmlFreeDoc(ctxt->myDoc);
    }

    xmlFreeParserCtxt(ctxt);

    return(ret);
}

/**
 * Parse an XML external entity out of context and build a tree.
 *
 *     [78] extParsedEnt ::= TextDecl? content
 *
 * This correspond to a "Well Balanced" chunk
 *
 * This function uses deprecated global variables to set parser options
 * which default to XML_PARSE_NODICT.
 *
 * @deprecated Don't use.
 *
 * @param filename  the filename
 * @returns the resulting document tree
 */

xmlDoc *
xmlParseEntity(const char *filename) {
    return(xmlSAXParseEntity(NULL, filename));
}
#endif /* LIBXML_SAX1_ENABLED */

/**
 * Create a parser context for an external entity
 * Automatic support for ZLIB/Compress compressed document is provided
 * by default if found at compile-time.
 *
 * @deprecated Don't use.
 *
 * @param URL  the entity URL
 * @param ID  the entity PUBLIC ID
 * @param base  a possible base for the target URI
 * @returns the new parser context or NULL
 */
xmlParserCtxt *
xmlCreateEntityParserCtxt(const xmlChar *URL, const xmlChar *ID,
	                  const xmlChar *base) {
    xmlParserCtxtPtr ctxt;
    xmlParserInputPtr input;
    xmlChar *uri = NULL;

    ctxt = xmlNewParserCtxt();
    if (ctxt == NULL)
	return(NULL);

    if (base != NULL) {
        if (xmlBuildURISafe(URL, base, &uri) < 0)
            goto error;
        if (uri != NULL)
            URL = uri;
    }

    input = xmlLoadResource(ctxt, (char *) URL, (char *) ID,
                            XML_RESOURCE_UNKNOWN);
    if (input == NULL)
        goto error;

    if (xmlCtxtPushInput(ctxt, input) < 0) {
        xmlFreeInputStream(input);
        goto error;
    }

    xmlFree(uri);
    return(ctxt);

error:
    xmlFree(uri);
    xmlFreeParserCtxt(ctxt);
    return(NULL);
}

/************************************************************************
 *									*
 *		Front ends when parsing from a file			*
 *									*
 ************************************************************************/

/**
 * Create a parser context for a file or URL content.
 * Automatic support for ZLIB/Compress compressed document is provided
 * by default if found at compile-time and for file accesses
 *
 * @deprecated Use #xmlNewParserCtxt and #xmlCtxtReadFile.
 *
 * @param filename  the filename or URL
 * @param options  a combination of xmlParserOption
 * @returns the new parser context or NULL
 */
xmlParserCtxt *
xmlCreateURLParserCtxt(const char *filename, int options)
{
    xmlParserCtxtPtr ctxt;
    xmlParserInputPtr input;

    ctxt = xmlNewParserCtxt();
    if (ctxt == NULL)
	return(NULL);

    xmlCtxtUseOptions(ctxt, options);

    input = xmlLoadResource(ctxt, filename, NULL, XML_RESOURCE_MAIN_DOCUMENT);
    if (input == NULL) {
	xmlFreeParserCtxt(ctxt);
	return(NULL);
    }
    if (xmlCtxtPushInput(ctxt, input) < 0) {
        xmlFreeInputStream(input);
        xmlFreeParserCtxt(ctxt);
        return(NULL);
    }

    return(ctxt);
}

/**
 * Create a parser context for a file content.
 * Automatic support for ZLIB/Compress compressed document is provided
 * by default if found at compile-time.
 *
 * @deprecated Use #xmlNewParserCtxt and #xmlCtxtReadFile.
 *
 * @param filename  the filename
 * @returns the new parser context or NULL
 */
xmlParserCtxt *
xmlCreateFileParserCtxt(const char *filename)
{
    return(xmlCreateURLParserCtxt(filename, 0));
}

#ifdef LIBXML_SAX1_ENABLED
/**
 * Parse an XML file and build a tree. Automatic support for ZLIB/Compress
 * compressed document is provided by default if found at compile-time.
 * It use the given SAX function block to handle the parsing callback.
 * If sax is NULL, fallback to the default DOM tree building routines.
 *
 * This function uses deprecated global variables to set parser options
 * which default to XML_PARSE_NODICT.
 *
 * @deprecated Use #xmlNewSAXParserCtxt and #xmlCtxtReadFile.
 *
 * User data (void *) is stored within the parser context in the
 * context's _private member, so it is available nearly everywhere in libxml
 *
 * @param sax  the SAX handler block
 * @param filename  the filename
 * @param recovery  work in recovery mode, i.e. tries to read no Well Formed
 *             documents
 * @param data  the userdata
 * @returns the resulting document tree
 */

xmlDoc *
xmlSAXParseFileWithData(xmlSAXHandler *sax, const char *filename,
                        int recovery, void *data) {
    xmlDocPtr ret = NULL;
    xmlParserCtxtPtr ctxt;
    xmlParserInputPtr input;

    ctxt = xmlNewSAXParserCtxt(sax, NULL);
    if (ctxt == NULL)
	return(NULL);

    if (data != NULL)
	ctxt->_private = data;

    if (recovery) {
        ctxt->options |= XML_PARSE_RECOVER;
        ctxt->recovery = 1;
    }

    if ((filename != NULL) && (filename[0] == '-') && (filename[1] == 0))
        input = xmlCtxtNewInputFromFd(ctxt, filename, STDIN_FILENO, NULL, 0);
    else
        input = xmlCtxtNewInputFromUrl(ctxt, filename, NULL, NULL, 0);

    if (input != NULL)
        ret = xmlCtxtParseDocument(ctxt, input);

    xmlFreeParserCtxt(ctxt);
    return(ret);
}

/**
 * Parse an XML file and build a tree. Automatic support for ZLIB/Compress
 * compressed document is provided by default if found at compile-time.
 * It use the given SAX function block to handle the parsing callback.
 * If sax is NULL, fallback to the default DOM tree building routines.
 *
 * This function uses deprecated global variables to set parser options
 * which default to XML_PARSE_NODICT.
 *
 * @deprecated Use #xmlNewSAXParserCtxt and #xmlCtxtReadFile.
 *
 * @param sax  the SAX handler block
 * @param filename  the filename
 * @param recovery  work in recovery mode, i.e. tries to read no Well Formed
 *             documents
 * @returns the resulting document tree
 */

xmlDoc *
xmlSAXParseFile(xmlSAXHandler *sax, const char *filename,
                          int recovery) {
    return(xmlSAXParseFileWithData(sax,filename,recovery,NULL));
}

/**
 * Parse an XML in-memory document and build a tree.
 * In the case the document is not Well Formed, a attempt to build a
 * tree is tried anyway
 *
 * This function uses deprecated global variables to set parser options
 * which default to XML_PARSE_NODICT | XML_PARSE_RECOVER.
 *
 * @deprecated Use #xmlReadDoc with XML_PARSE_RECOVER.
 *
 * @param cur  a pointer to an array of xmlChar
 * @returns the resulting document tree or NULL in case of failure
 */

xmlDoc *
xmlRecoverDoc(const xmlChar *cur) {
    return(xmlSAXParseDoc(NULL, cur, 1));
}

/**
 * Parse an XML file and build a tree. Automatic support for ZLIB/Compress
 * compressed document is provided by default if found at compile-time.
 *
 * This function uses deprecated global variables to set parser options
 * which default to XML_PARSE_NODICT.
 *
 * @deprecated Use #xmlReadFile.
 *
 * @param filename  the filename
 * @returns the resulting document tree if the file was wellformed,
 * NULL otherwise.
 */

xmlDoc *
xmlParseFile(const char *filename) {
    return(xmlSAXParseFile(NULL, filename, 0));
}

/**
 * Parse an XML file and build a tree. Automatic support for ZLIB/Compress
 * compressed document is provided by default if found at compile-time.
 * In the case the document is not Well Formed, it attempts to build
 * a tree anyway
 *
 * This function uses deprecated global variables to set parser options
 * which default to XML_PARSE_NODICT | XML_PARSE_RECOVER.
 *
 * @deprecated Use #xmlReadFile with XML_PARSE_RECOVER.
 *
 * @param filename  the filename
 * @returns the resulting document tree or NULL in case of failure
 */

xmlDoc *
xmlRecoverFile(const char *filename) {
    return(xmlSAXParseFile(NULL, filename, 1));
}


/**
 * Setup the parser context to parse a new buffer; Clears any prior
 * contents from the parser context. The buffer parameter must not be
 * NULL, but the filename parameter can be
 *
 * @deprecated Don't use.
 *
 * @param ctxt  an XML parser context
 * @param buffer  a xmlChar * buffer
 * @param filename  a file name
 */
void
xmlSetupParserForBuffer(xmlParserCtxt *ctxt, const xmlChar* buffer,
                             const char* filename)
{
    xmlParserInputPtr input;

    if ((ctxt == NULL) || (buffer == NULL))
        return;

    xmlCtxtReset(ctxt);

    input = xmlCtxtNewInputFromString(ctxt, filename, (const char *) buffer,
                                      NULL, 0);
    if (input == NULL)
        return;
    if (xmlCtxtPushInput(ctxt, input) < 0)
        xmlFreeInputStream(input);
}

/**
 * Parse an XML file and call the given SAX handler routines.
 * Automatic support for ZLIB/Compress compressed document is provided
 *
 * This function uses deprecated global variables to set parser options
 * which default to XML_PARSE_NODICT.
 *
 * @deprecated Use #xmlNewSAXParserCtxt and #xmlCtxtReadFile.
 *
 * @param sax  a SAX handler
 * @param user_data  The user data returned on SAX callbacks
 * @param filename  a file name
 * @returns 0 in case of success or a error number otherwise
 */
int
xmlSAXUserParseFile(xmlSAXHandler *sax, void *user_data,
                    const char *filename) {
    int ret = 0;
    xmlParserCtxtPtr ctxt;

    ctxt = xmlCreateFileParserCtxt(filename);
    if (ctxt == NULL) return -1;
    if (sax != NULL) {
        if (sax->initialized == XML_SAX2_MAGIC) {
            *ctxt->sax = *sax;
        } else {
            memset(ctxt->sax, 0, sizeof(*ctxt->sax));
            memcpy(ctxt->sax, sax, sizeof(xmlSAXHandlerV1));
        }
	ctxt->userData = user_data;
    }

    xmlParseDocument(ctxt);

    if (ctxt->wellFormed)
	ret = 0;
    else {
        if (ctxt->errNo != 0)
	    ret = ctxt->errNo;
	else
	    ret = -1;
    }
    if (ctxt->myDoc != NULL) {
        xmlFreeDoc(ctxt->myDoc);
	ctxt->myDoc = NULL;
    }
    xmlFreeParserCtxt(ctxt);

    return ret;
}
#endif /* LIBXML_SAX1_ENABLED */

/************************************************************************
 *									*
 *		Front ends when parsing from memory			*
 *									*
 ************************************************************************/

/**
 * Create a parser context for an XML in-memory document. The input buffer
 * must not contain a terminating null byte.
 *
 * @param buffer  a pointer to a char array
 * @param size  the size of the array
 * @returns the new parser context or NULL
 */
xmlParserCtxt *
xmlCreateMemoryParserCtxt(const char *buffer, int size) {
    xmlParserCtxtPtr ctxt;
    xmlParserInputPtr input;

    if (size < 0)
	return(NULL);

    ctxt = xmlNewParserCtxt();
    if (ctxt == NULL)
	return(NULL);

    input = xmlCtxtNewInputFromMemory(ctxt, NULL, buffer, size, NULL, 0);
    if (input == NULL) {
	xmlFreeParserCtxt(ctxt);
	return(NULL);
    }
    if (xmlCtxtPushInput(ctxt, input) < 0) {
        xmlFreeInputStream(input);
        xmlFreeParserCtxt(ctxt);
        return(NULL);
    }

    return(ctxt);
}

#ifdef LIBXML_SAX1_ENABLED
/**
 * Parse an XML in-memory block and use the given SAX function block
 * to handle the parsing callback. If sax is NULL, fallback to the default
 * DOM tree building routines.
 *
 * This function uses deprecated global variables to set parser options
 * which default to XML_PARSE_NODICT.
 *
 * @deprecated Use #xmlNewSAXParserCtxt and #xmlCtxtReadMemory.
 *
 * User data (void *) is stored within the parser context in the
 * context's _private member, so it is available nearly everywhere in libxml
 *
 * @param sax  the SAX handler block
 * @param buffer  an pointer to a char array
 * @param size  the size of the array
 * @param recovery  work in recovery mode, i.e. tries to read no Well Formed
 *             documents
 * @param data  the userdata
 * @returns the resulting document tree
 */

xmlDoc *
xmlSAXParseMemoryWithData(xmlSAXHandler *sax, const char *buffer,
                          int size, int recovery, void *data) {
    xmlDocPtr ret = NULL;
    xmlParserCtxtPtr ctxt;
    xmlParserInputPtr input;

    if (size < 0)
        return(NULL);

    ctxt = xmlNewSAXParserCtxt(sax, NULL);
    if (ctxt == NULL)
        return(NULL);

    if (data != NULL)
	ctxt->_private=data;

    if (recovery) {
        ctxt->options |= XML_PARSE_RECOVER;
        ctxt->recovery = 1;
    }

    input = xmlCtxtNewInputFromMemory(ctxt, NULL, buffer, size, NULL,
                                      XML_INPUT_BUF_STATIC);

    if (input != NULL)
        ret = xmlCtxtParseDocument(ctxt, input);

    xmlFreeParserCtxt(ctxt);
    return(ret);
}

/**
 * Parse an XML in-memory block and use the given SAX function block
 * to handle the parsing callback. If sax is NULL, fallback to the default
 * DOM tree building routines.
 *
 * This function uses deprecated global variables to set parser options
 * which default to XML_PARSE_NODICT.
 *
 * @deprecated Use #xmlNewSAXParserCtxt and #xmlCtxtReadMemory.
 *
 * @param sax  the SAX handler block
 * @param buffer  an pointer to a char array
 * @param size  the size of the array
 * @param recovery  work in recovery mode, i.e. tries to read not Well Formed
 *             documents
 * @returns the resulting document tree
 */
xmlDoc *
xmlSAXParseMemory(xmlSAXHandler *sax, const char *buffer,
	          int size, int recovery) {
    return xmlSAXParseMemoryWithData(sax, buffer, size, recovery, NULL);
}

/**
 * Parse an XML in-memory block and build a tree.
 *
 * This function uses deprecated global variables to set parser options
 * which default to XML_PARSE_NODICT.
 *
 * @deprecated Use #xmlReadMemory.
 *
 * @param buffer  an pointer to a char array
 * @param size  the size of the array
 * @returns the resulting document tree
 */

xmlDoc *xmlParseMemory(const char *buffer, int size) {
   return(xmlSAXParseMemory(NULL, buffer, size, 0));
}

/**
 * Parse an XML in-memory block and build a tree.
 * In the case the document is not Well Formed, an attempt to
 * build a tree is tried anyway
 *
 * This function uses deprecated global variables to set parser options
 * which default to XML_PARSE_NODICT | XML_PARSE_RECOVER.
 *
 * @deprecated Use #xmlReadMemory with XML_PARSE_RECOVER.
 *
 * @param buffer  an pointer to a char array
 * @param size  the size of the array
 * @returns the resulting document tree or NULL in case of error
 */

xmlDoc *xmlRecoverMemory(const char *buffer, int size) {
   return(xmlSAXParseMemory(NULL, buffer, size, 1));
}

/**
 * Parse an XML in-memory buffer and call the given SAX handler routines.
 *
 * This function uses deprecated global variables to set parser options
 * which default to XML_PARSE_NODICT.
 *
 * @deprecated Use #xmlNewSAXParserCtxt and #xmlCtxtReadMemory.
 *
 * @param sax  a SAX handler
 * @param user_data  The user data returned on SAX callbacks
 * @param buffer  an in-memory XML document input
 * @param size  the length of the XML document in bytes
 * @returns 0 in case of success or a error number otherwise
 */
int xmlSAXUserParseMemory(xmlSAXHandler *sax, void *user_data,
			  const char *buffer, int size) {
    int ret = 0;
    xmlParserCtxtPtr ctxt;

    ctxt = xmlCreateMemoryParserCtxt(buffer, size);
    if (ctxt == NULL) return -1;
    if (sax != NULL) {
        if (sax->initialized == XML_SAX2_MAGIC) {
            *ctxt->sax = *sax;
        } else {
            memset(ctxt->sax, 0, sizeof(*ctxt->sax));
            memcpy(ctxt->sax, sax, sizeof(xmlSAXHandlerV1));
        }
	ctxt->userData = user_data;
    }

    xmlParseDocument(ctxt);

    if (ctxt->wellFormed)
	ret = 0;
    else {
        if (ctxt->errNo != 0)
	    ret = ctxt->errNo;
	else
	    ret = -1;
    }
    if (ctxt->myDoc != NULL) {
        xmlFreeDoc(ctxt->myDoc);
	ctxt->myDoc = NULL;
    }
    xmlFreeParserCtxt(ctxt);

    return ret;
}
#endif /* LIBXML_SAX1_ENABLED */

/**
 * Creates a parser context for an XML in-memory document.
 *
 * @param str  a pointer to an array of xmlChar
 * @returns the new parser context or NULL
 */
xmlParserCtxt *
xmlCreateDocParserCtxt(const xmlChar *str) {
    xmlParserCtxtPtr ctxt;
    xmlParserInputPtr input;

    ctxt = xmlNewParserCtxt();
    if (ctxt == NULL)
	return(NULL);

    input = xmlCtxtNewInputFromString(ctxt, NULL, (const char *) str, NULL, 0);
    if (input == NULL) {
	xmlFreeParserCtxt(ctxt);
	return(NULL);
    }
    if (xmlCtxtPushInput(ctxt, input) < 0) {
        xmlFreeInputStream(input);
        xmlFreeParserCtxt(ctxt);
        return(NULL);
    }

    return(ctxt);
}

#ifdef LIBXML_SAX1_ENABLED
/**
 * Parse an XML in-memory document and build a tree.
 * It use the given SAX function block to handle the parsing callback.
 * If sax is NULL, fallback to the default DOM tree building routines.
 *
 * This function uses deprecated global variables to set parser options
 * which default to XML_PARSE_NODICT.
 *
 * @deprecated Use #xmlNewSAXParserCtxt and #xmlCtxtReadDoc.
 *
 * @param sax  the SAX handler block
 * @param cur  a pointer to an array of xmlChar
 * @param recovery  work in recovery mode, i.e. tries to read no Well Formed
 *             documents
 * @returns the resulting document tree
 */

xmlDoc *
xmlSAXParseDoc(xmlSAXHandler *sax, const xmlChar *cur, int recovery) {
    xmlDocPtr ret;
    xmlParserCtxtPtr ctxt;
    xmlSAXHandlerPtr oldsax = NULL;

    if (cur == NULL) return(NULL);


    ctxt = xmlCreateDocParserCtxt(cur);
    if (ctxt == NULL) return(NULL);
    if (sax != NULL) {
        oldsax = ctxt->sax;
        ctxt->sax = sax;
        ctxt->userData = NULL;
    }

    xmlParseDocument(ctxt);
    if ((ctxt->wellFormed) || recovery) ret = ctxt->myDoc;
    else {
       ret = NULL;
       xmlFreeDoc(ctxt->myDoc);
       ctxt->myDoc = NULL;
    }
    if (sax != NULL)
	ctxt->sax = oldsax;
    xmlFreeParserCtxt(ctxt);

    return(ret);
}

/**
 * Parse an XML in-memory document and build a tree.
 *
 * This function uses deprecated global variables to set parser options
 * which default to XML_PARSE_NODICT.
 *
 * @deprecated Use #xmlReadDoc.
 *
 * @param cur  a pointer to an array of xmlChar
 * @returns the resulting document tree
 */

xmlDoc *
xmlParseDoc(const xmlChar *cur) {
    return(xmlSAXParseDoc(NULL, cur, 0));
}
#endif /* LIBXML_SAX1_ENABLED */

/************************************************************************
 *									*
 *	New set (2.6.0) of simpler and more flexible APIs		*
 *									*
 ************************************************************************/

/**
 * Reset a parser context
 *
 * @param ctxt  an XML parser context
 */
void
xmlCtxtReset(xmlParserCtxt *ctxt)
{
    xmlParserInputPtr input;

    if (ctxt == NULL)
        return;

    while ((input = xmlCtxtPopInput(ctxt)) != NULL) { /* Non consuming */
        xmlFreeInputStream(input);
    }
    ctxt->inputNr = 0;
    ctxt->input = NULL;

    ctxt->spaceNr = 0;
    if (ctxt->spaceTab != NULL) {
	ctxt->spaceTab[0] = -1;
	ctxt->space = &ctxt->spaceTab[0];
    } else {
        ctxt->space = NULL;
    }


    ctxt->nodeNr = 0;
    ctxt->node = NULL;

    ctxt->nameNr = 0;
    ctxt->name = NULL;

    ctxt->nsNr = 0;
    xmlParserNsReset(ctxt->nsdb);

    if (ctxt->version != NULL) {
        xmlFree(ctxt->version);
        ctxt->version = NULL;
    }
    if (ctxt->encoding != NULL) {
        xmlFree(ctxt->encoding);
        ctxt->encoding = NULL;
    }
    if (ctxt->extSubURI != NULL) {
        xmlFree(ctxt->extSubURI);
        ctxt->extSubURI = NULL;
    }
    if (ctxt->extSubSystem != NULL) {
        xmlFree(ctxt->extSubSystem);
        ctxt->extSubSystem = NULL;
    }
    if (ctxt->directory != NULL) {
        xmlFree(ctxt->directory);
        ctxt->directory = NULL;
    }

    if (ctxt->myDoc != NULL)
        xmlFreeDoc(ctxt->myDoc);
    ctxt->myDoc = NULL;

    ctxt->standalone = -1;
    ctxt->hasExternalSubset = 0;
    ctxt->hasPErefs = 0;
    ctxt->html = ctxt->html ? 1 : 0;
    ctxt->instate = XML_PARSER_START;

    ctxt->wellFormed = 1;
    ctxt->nsWellFormed = 1;
    ctxt->disableSAX = 0;
    ctxt->valid = 1;
    ctxt->record_info = 0;
    ctxt->checkIndex = 0;
    ctxt->endCheckState = 0;
    ctxt->inSubset = 0;
    ctxt->errNo = XML_ERR_OK;
    ctxt->depth = 0;
    ctxt->catalogs = NULL;
    ctxt->sizeentities = 0;
    ctxt->sizeentcopy = 0;
    xmlInitNodeInfoSeq(&ctxt->node_seq);

    if (ctxt->attsDefault != NULL) {
        xmlHashFree(ctxt->attsDefault, xmlHashDefaultDeallocator);
        ctxt->attsDefault = NULL;
    }
    if (ctxt->attsSpecial != NULL) {
        xmlHashFree(ctxt->attsSpecial, NULL);
        ctxt->attsSpecial = NULL;
    }

#ifdef LIBXML_CATALOG_ENABLED
    if (ctxt->catalogs != NULL)
	xmlCatalogFreeLocal(ctxt->catalogs);
#endif
    ctxt->nbErrors = 0;
    ctxt->nbWarnings = 0;
    if (ctxt->lastError.code != XML_ERR_OK)
        xmlResetError(&ctxt->lastError);
}

/**
 * Reset a push parser context
 *
 * @param ctxt  an XML parser context
 * @param chunk  a pointer to an array of chars
 * @param size  number of chars in the array
 * @param filename  an optional file name or URI
 * @param encoding  the document encoding, or NULL
 * @returns 0 in case of success and 1 in case of error
 */
int
xmlCtxtResetPush(xmlParserCtxt *ctxt, const char *chunk,
                 int size, const char *filename, const char *encoding)
{
    xmlParserInputPtr input;

    if (ctxt == NULL)
        return(1);

    xmlCtxtReset(ctxt);

    input = xmlNewPushInput(filename, chunk, size);
    if (input == NULL)
        return(1);

    if (xmlCtxtPushInput(ctxt, input) < 0) {
        xmlFreeInputStream(input);
        return(1);
    }

    if (encoding != NULL)
        xmlSwitchEncodingName(ctxt, encoding);

    return(0);
}

static int
xmlCtxtSetOptionsInternal(xmlParserCtxtPtr ctxt, int options, int keepMask)
{
    int allMask;

    if (ctxt == NULL)
        return(-1);

    /*
     * XInclude options aren't handled by the parser.
     *
     * XML_PARSE_XINCLUDE
     * XML_PARSE_NOXINCNODE
     * XML_PARSE_NOBASEFIX
     */
    allMask = XML_PARSE_RECOVER |
              XML_PARSE_NOENT |
              XML_PARSE_DTDLOAD |
              XML_PARSE_DTDATTR |
              XML_PARSE_DTDVALID |
              XML_PARSE_NOERROR |
              XML_PARSE_NOWARNING |
              XML_PARSE_PEDANTIC |
              XML_PARSE_NOBLANKS |
#ifdef LIBXML_SAX1_ENABLED
              XML_PARSE_SAX1 |
#endif
              XML_PARSE_NONET |
              XML_PARSE_NODICT |
              XML_PARSE_NSCLEAN |
              XML_PARSE_NOCDATA |
              XML_PARSE_COMPACT |
              XML_PARSE_OLD10 |
              XML_PARSE_HUGE |
              XML_PARSE_OLDSAX |
              XML_PARSE_IGNORE_ENC |
              XML_PARSE_BIG_LINES |
              XML_PARSE_NO_XXE |
              XML_PARSE_UNZIP |
              XML_PARSE_NO_SYS_CATALOG |
              XML_PARSE_CATALOG_PI;

    ctxt->options = (ctxt->options & keepMask) | (options & allMask);

    /*
     * For some options, struct members are historically the source
     * of truth. The values are initalized from global variables and
     * old code could also modify them directly. Several older API
     * functions that don't take an options argument rely on these
     * deprecated mechanisms.
     *
     * Once public access to struct members and the globals are
     * disabled, we can use the options bitmask as source of
     * truth, making all these struct members obsolete.
     *
     * The XML_DETECT_IDS flags is misnamed. It simply enables
     * loading of the external subset.
     */
    ctxt->recovery = (options & XML_PARSE_RECOVER) ? 1 : 0;
    ctxt->replaceEntities = (options & XML_PARSE_NOENT) ? 1 : 0;
    ctxt->loadsubset = (options & XML_PARSE_DTDLOAD) ? XML_DETECT_IDS : 0;
    ctxt->loadsubset |= (options & XML_PARSE_DTDATTR) ? XML_COMPLETE_ATTRS : 0;
    ctxt->loadsubset |= (options & XML_PARSE_SKIP_IDS) ? XML_SKIP_IDS : 0;
    ctxt->validate = (options & XML_PARSE_DTDVALID) ? 1 : 0;
    ctxt->pedantic = (options & XML_PARSE_PEDANTIC) ? 1 : 0;
    ctxt->keepBlanks = (options & XML_PARSE_NOBLANKS) ? 0 : 1;
    ctxt->dictNames = (options & XML_PARSE_NODICT) ? 0 : 1;

    return(options & ~allMask);
}

/**
 * Applies the options to the parser context. Unset options are
 * cleared.
 *
 * @since 2.13.0
 *
 * With older versions, you can use #xmlCtxtUseOptions.
 *
 * @param ctxt  an XML parser context
 * @param options  a bitmask of xmlParserOption values
 * @returns 0 in case of success, the set of unknown or unimplemented options
 *         in case of error.
 */
int
xmlCtxtSetOptions(xmlParserCtxt *ctxt, int options)
{
#ifdef LIBXML_HTML_ENABLED
    if ((ctxt != NULL) && (ctxt->html))
        return(htmlCtxtSetOptions(ctxt, options));
#endif

    return(xmlCtxtSetOptionsInternal(ctxt, options, 0));
}

/**
 * Get the current options of the parser context.
 *
 * @since 2.14.0
 *
 * @param ctxt  an XML parser context
 * @returns the current options set in the parser context, or -1 if ctxt is NULL.
 */
int
xmlCtxtGetOptions(xmlParserCtxt *ctxt)
{
    if (ctxt == NULL)
        return(-1);

    return(ctxt->options);
}

/**
 * Applies the options to the parser context. The following options
 * are never cleared and can only be enabled:
 *
 * - XML_PARSE_NOERROR
 * - XML_PARSE_NOWARNING
 * - XML_PARSE_NONET
 * - XML_PARSE_NSCLEAN
 * - XML_PARSE_NOCDATA
 * - XML_PARSE_COMPACT
 * - XML_PARSE_OLD10
 * - XML_PARSE_HUGE
 * - XML_PARSE_OLDSAX
 * - XML_PARSE_IGNORE_ENC
 * - XML_PARSE_BIG_LINES
 *
 * @deprecated Use #xmlCtxtSetOptions.
 *
 * @param ctxt  an XML parser context
 * @param options  a combination of xmlParserOption
 * @returns 0 in case of success, the set of unknown or unimplemented options
 *         in case of error.
 */
int
xmlCtxtUseOptions(xmlParserCtxt *ctxt, int options)
{
    int keepMask;

#ifdef LIBXML_HTML_ENABLED
    if ((ctxt != NULL) && (ctxt->html))
        return(htmlCtxtUseOptions(ctxt, options));
#endif

    /*
     * For historic reasons, some options can only be enabled.
     */
    keepMask = XML_PARSE_NOERROR |
               XML_PARSE_NOWARNING |
               XML_PARSE_NONET |
               XML_PARSE_NSCLEAN |
               XML_PARSE_NOCDATA |
               XML_PARSE_COMPACT |
               XML_PARSE_OLD10 |
               XML_PARSE_HUGE |
               XML_PARSE_OLDSAX |
               XML_PARSE_IGNORE_ENC |
               XML_PARSE_BIG_LINES;

    return(xmlCtxtSetOptionsInternal(ctxt, options, keepMask));
}

/**
 * To protect against exponential entity expansion ("billion laughs"), the
 * size of serialized output is (roughly) limited to the input size
 * multiplied by this factor. The default value is 5.
 *
 * When working with documents making heavy use of entity expansion, it can
 * be necessary to increase the value. For security reasons, this should only
 * be considered when processing trusted input.
 *
 * @param ctxt  an XML parser context
 * @param maxAmpl  maximum amplification factor
 */
void
xmlCtxtSetMaxAmplification(xmlParserCtxt *ctxt, unsigned maxAmpl)
{
    if (ctxt == NULL)
        return;
    ctxt->maxAmpl = maxAmpl;
}

/**
 * Parse an XML document and return the resulting document tree.
 * Takes ownership of the input object.
 *
 * @since 2.13.0
 *
 * @param ctxt  an XML parser context
 * @param input  parser input
 * @returns the resulting document tree or NULL
 */
xmlDoc *
xmlCtxtParseDocument(xmlParserCtxt *ctxt, xmlParserInput *input)
{
    xmlDocPtr ret = NULL;

    if ((ctxt == NULL) || (input == NULL)) {
        xmlFatalErr(ctxt, XML_ERR_ARGUMENT, NULL);
        xmlFreeInputStream(input);
        return(NULL);
    }

    /* assert(ctxt->inputNr == 0); */
    while (ctxt->inputNr > 0)
        xmlFreeInputStream(xmlCtxtPopInput(ctxt));

    if (xmlCtxtPushInput(ctxt, input) < 0) {
        xmlFreeInputStream(input);
        return(NULL);
    }

    xmlParseDocument(ctxt);

    ret = xmlCtxtGetDocument(ctxt);

    /* assert(ctxt->inputNr == 1); */
    while (ctxt->inputNr > 0)
        xmlFreeInputStream(xmlCtxtPopInput(ctxt));

    return(ret);
}

/**
 * Convenience function to parse an XML document from a
 * zero-terminated string.
 *
 * See #xmlCtxtReadDoc for details.
 *
 * @param cur  a pointer to a zero terminated string
 * @param URL  base URL (optional)
 * @param encoding  the document encoding (optional)
 * @param options  a combination of xmlParserOption
 * @returns the resulting document tree
 */
xmlDoc *
xmlReadDoc(const xmlChar *cur, const char *URL, const char *encoding,
           int options)
{
    xmlParserCtxtPtr ctxt;
    xmlParserInputPtr input;
    xmlDocPtr doc = NULL;

    ctxt = xmlNewParserCtxt();
    if (ctxt == NULL)
        return(NULL);

    xmlCtxtUseOptions(ctxt, options);

    input = xmlCtxtNewInputFromString(ctxt, URL, (const char *) cur, encoding,
                                      XML_INPUT_BUF_STATIC);

    if (input != NULL)
        doc = xmlCtxtParseDocument(ctxt, input);

    xmlFreeParserCtxt(ctxt);
    return(doc);
}

/**
 * Convenience function to parse an XML file from the filesystem
 * or a global, user-defined resource loader.
 *
 * If a "-" filename is passed, the function will read from stdin.
 * This feature is potentially insecure and might be removed from
 * later versions.
 *
 * See #xmlCtxtReadFile for details.
 *
 * @param filename  a file or URL
 * @param encoding  the document encoding (optional)
 * @param options  a combination of xmlParserOption
 * @returns the resulting document tree
 */
xmlDoc *
xmlReadFile(const char *filename, const char *encoding, int options)
{
    xmlParserCtxtPtr ctxt;
    xmlParserInputPtr input;
    xmlDocPtr doc = NULL;

    ctxt = xmlNewParserCtxt();
    if (ctxt == NULL)
        return(NULL);

    xmlCtxtUseOptions(ctxt, options);

    /*
     * Backward compatibility for users of command line utilities like
     * xmlstarlet expecting "-" to mean stdin. This is dangerous and
     * should be removed at some point.
     */
    if ((filename != NULL) && (filename[0] == '-') && (filename[1] == 0))
        input = xmlCtxtNewInputFromFd(ctxt, filename, STDIN_FILENO,
                                      encoding, 0);
    else
        input = xmlCtxtNewInputFromUrl(ctxt, filename, NULL, encoding, 0);

    if (input != NULL)
        doc = xmlCtxtParseDocument(ctxt, input);

    xmlFreeParserCtxt(ctxt);
    return(doc);
}

/**
 * Parse an XML in-memory document and build a tree. The input buffer must
 * not contain a terminating null byte.
 *
 * See #xmlCtxtReadMemory for details.
 *
 * @param buffer  a pointer to a char array
 * @param size  the size of the array
 * @param url  base URL (optional)
 * @param encoding  the document encoding (optional)
 * @param options  a combination of xmlParserOption
 * @returns the resulting document tree
 */
xmlDoc *
xmlReadMemory(const char *buffer, int size, const char *url,
              const char *encoding, int options)
{
    xmlParserCtxtPtr ctxt;
    xmlParserInputPtr input;
    xmlDocPtr doc = NULL;

    if (size < 0)
	return(NULL);

    ctxt = xmlNewParserCtxt();
    if (ctxt == NULL)
        return(NULL);

    xmlCtxtUseOptions(ctxt, options);

    input = xmlCtxtNewInputFromMemory(ctxt, url, buffer, size, encoding,
                                      XML_INPUT_BUF_STATIC);

    if (input != NULL)
        doc = xmlCtxtParseDocument(ctxt, input);

    xmlFreeParserCtxt(ctxt);
    return(doc);
}

/**
 * Parse an XML from a file descriptor and build a tree.
 *
 * See #xmlCtxtReadFd for details.
 *
 * NOTE that the file descriptor will not be closed when the
 * context is freed or reset.
 *
 * @param fd  an open file descriptor
 * @param URL  base URL (optional)
 * @param encoding  the document encoding (optional)
 * @param options  a combination of xmlParserOption
 * @returns the resulting document tree
 */
xmlDoc *
xmlReadFd(int fd, const char *URL, const char *encoding, int options)
{
    xmlParserCtxtPtr ctxt;
    xmlParserInputPtr input;
    xmlDocPtr doc = NULL;

    ctxt = xmlNewParserCtxt();
    if (ctxt == NULL)
        return(NULL);

    xmlCtxtUseOptions(ctxt, options);

    input = xmlCtxtNewInputFromFd(ctxt, URL, fd, encoding, 0);

    if (input != NULL)
        doc = xmlCtxtParseDocument(ctxt, input);

    xmlFreeParserCtxt(ctxt);
    return(doc);
}

/**
 * Parse an XML document from I/O functions and context and build a tree.
 *
 * See #xmlCtxtReadIO for details.
 *
 * @param ioread  an I/O read function
 * @param ioclose  an I/O close function (optional)
 * @param ioctx  an I/O handler
 * @param URL  base URL (optional)
 * @param encoding  the document encoding (optional)
 * @param options  a combination of xmlParserOption
 * @returns the resulting document tree
 */
xmlDoc *
xmlReadIO(xmlInputReadCallback ioread, xmlInputCloseCallback ioclose,
          void *ioctx, const char *URL, const char *encoding, int options)
{
    xmlParserCtxtPtr ctxt;
    xmlParserInputPtr input;
    xmlDocPtr doc = NULL;

    ctxt = xmlNewParserCtxt();
    if (ctxt == NULL)
        return(NULL);

    xmlCtxtUseOptions(ctxt, options);

    input = xmlCtxtNewInputFromIO(ctxt, URL, ioread, ioclose, ioctx,
                                  encoding, 0);

    if (input != NULL)
        doc = xmlCtxtParseDocument(ctxt, input);

    xmlFreeParserCtxt(ctxt);
    return(doc);
}

/**
 * Parse an XML in-memory document and build a tree.
 *
 * `URL` is used as base to resolve external entities and for error
 * reporting.
 *
 * @param ctxt  an XML parser context
 * @param str  a pointer to a zero terminated string
 * @param URL  base URL (optional)
 * @param encoding  the document encoding (optional)
 * @param options  a combination of xmlParserOption
 * @returns the resulting document tree
 */
xmlDoc *
xmlCtxtReadDoc(xmlParserCtxt *ctxt, const xmlChar *str,
               const char *URL, const char *encoding, int options)
{
    xmlParserInputPtr input;

    if (ctxt == NULL)
        return(NULL);

    xmlCtxtReset(ctxt);
    xmlCtxtUseOptions(ctxt, options);

    input = xmlCtxtNewInputFromString(ctxt, URL, (const char *) str, encoding,
                                      XML_INPUT_BUF_STATIC);
    if (input == NULL)
        return(NULL);

    return(xmlCtxtParseDocument(ctxt, input));
}

/**
 * Parse an XML file from the filesystem or a global, user-defined
 * resource loader.
 *
 * @param ctxt  an XML parser context
 * @param filename  a file or URL
 * @param encoding  the document encoding (optional)
 * @param options  a combination of xmlParserOption
 * @returns the resulting document tree
 */
xmlDoc *
xmlCtxtReadFile(xmlParserCtxt *ctxt, const char *filename,
                const char *encoding, int options)
{
    xmlParserInputPtr input;

    if (ctxt == NULL)
        return(NULL);

    xmlCtxtReset(ctxt);
    xmlCtxtUseOptions(ctxt, options);

    input = xmlCtxtNewInputFromUrl(ctxt, filename, NULL, encoding, 0);
    if (input == NULL)
        return(NULL);

    return(xmlCtxtParseDocument(ctxt, input));
}

/**
 * Parse an XML in-memory document and build a tree. The input buffer must
 * not contain a terminating null byte.
 *
 * `URL` is used as base to resolve external entities and for error
 * reporting.
 *
 * @param ctxt  an XML parser context
 * @param buffer  a pointer to a char array
 * @param size  the size of the array
 * @param URL  base URL (optional)
 * @param encoding  the document encoding (optional)
 * @param options  a combination of xmlParserOption
 * @returns the resulting document tree
 */
xmlDoc *
xmlCtxtReadMemory(xmlParserCtxt *ctxt, const char *buffer, int size,
                  const char *URL, const char *encoding, int options)
{
    xmlParserInputPtr input;

    if ((ctxt == NULL) || (size < 0))
        return(NULL);

    xmlCtxtReset(ctxt);
    xmlCtxtUseOptions(ctxt, options);

    input = xmlCtxtNewInputFromMemory(ctxt, URL, buffer, size, encoding,
                                      XML_INPUT_BUF_STATIC);
    if (input == NULL)
        return(NULL);

    return(xmlCtxtParseDocument(ctxt, input));
}

/**
 * Parse an XML document from a file descriptor and build a tree.
 *
 * NOTE that the file descriptor will not be closed when the
 * context is freed or reset.
 *
 * `URL` is used as base to resolve external entities and for error
 * reporting.
 *
 * @param ctxt  an XML parser context
 * @param fd  an open file descriptor
 * @param URL  base URL (optional)
 * @param encoding  the document encoding (optional)
 * @param options  a combination of xmlParserOption
 * @returns the resulting document tree
 */
xmlDoc *
xmlCtxtReadFd(xmlParserCtxt *ctxt, int fd,
              const char *URL, const char *encoding, int options)
{
    xmlParserInputPtr input;

    if (ctxt == NULL)
        return(NULL);

    xmlCtxtReset(ctxt);
    xmlCtxtUseOptions(ctxt, options);

    input = xmlCtxtNewInputFromFd(ctxt, URL, fd, encoding, 0);
    if (input == NULL)
        return(NULL);

    return(xmlCtxtParseDocument(ctxt, input));
}

/**
 * Parse an XML document from I/O functions and source and build a tree.
 * This reuses the existing `ctxt` parser context
 *
 * `URL` is used as base to resolve external entities and for error
 * reporting.
 *
 * @param ctxt  an XML parser context
 * @param ioread  an I/O read function
 * @param ioclose  an I/O close function
 * @param ioctx  an I/O handler
 * @param URL  the base URL to use for the document
 * @param encoding  the document encoding, or NULL
 * @param options  a combination of xmlParserOption
 * @returns the resulting document tree
 */
xmlDoc *
xmlCtxtReadIO(xmlParserCtxt *ctxt, xmlInputReadCallback ioread,
              xmlInputCloseCallback ioclose, void *ioctx,
	      const char *URL,
              const char *encoding, int options)
{
    xmlParserInputPtr input;

    if (ctxt == NULL)
        return(NULL);

    xmlCtxtReset(ctxt);
    xmlCtxtUseOptions(ctxt, options);

    input = xmlCtxtNewInputFromIO(ctxt, URL, ioread, ioclose, ioctx,
                                  encoding, 0);
    if (input == NULL)
        return(NULL);

    return(xmlCtxtParseDocument(ctxt, input));
}


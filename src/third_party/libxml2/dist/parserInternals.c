/*
 * parserInternals.c : Internal routines (and obsolete ones) needed for the
 *                     XML and HTML parsers.
 *
 * See Copyright for the status of this software.
 *
 * Author: Daniel Veillard
 */

#define IN_LIBXML
#include "libxml.h"

#if defined(_WIN32)
#define XML_DIR_SEP '\\'
#else
#define XML_DIR_SEP '/'
#endif

#include <string.h>
#include <ctype.h>
#include <stdlib.h>

#include <libxml/xmlmemory.h>
#include <libxml/tree.h>
#include <libxml/parser.h>
#include <libxml/parserInternals.h>
#include <libxml/entities.h>
#include <libxml/xmlerror.h>
#include <libxml/encoding.h>
#include <libxml/xmlIO.h>
#include <libxml/uri.h>
#include <libxml/dict.h>
#include <libxml/xmlsave.h>
#ifdef LIBXML_CATALOG_ENABLED
#include <libxml/catalog.h>
#endif
#include <libxml/chvalid.h>

#define CUR(ctxt) ctxt->input->cur
#define END(ctxt) ctxt->input->end

#include "private/buf.h"
#include "private/enc.h"
#include "private/error.h"
#include "private/globals.h"
#include "private/io.h"
#include "private/memory.h"
#include "private/parser.h"

#ifndef SIZE_MAX
  #define SIZE_MAX ((size_t) -1)
#endif

#define XML_MAX_ERRORS 100

/*
 * XML_MAX_AMPLIFICATION_DEFAULT is the default maximum allowed amplification
 * factor of serialized output after entity expansion.
 */
#define XML_MAX_AMPLIFICATION_DEFAULT 5

/*
 * Various global defaults for parsing
 */

/**
 * check the compiled lib version against the include one.
 *
 * @param version  the include version number
 */
void
xmlCheckVersion(int version) {
    int myversion = LIBXML_VERSION;

    xmlInitParser();

    if ((myversion / 10000) != (version / 10000)) {
	xmlPrintErrorMessage(
		"Fatal: program compiled against libxml %d using libxml %d\n",
		(version / 10000), (myversion / 10000));
    } else if ((myversion / 100) < (version / 100)) {
	xmlPrintErrorMessage(
		"Warning: program compiled against libxml %d using older %d\n",
		(version / 100), (myversion / 100));
    }
}


/************************************************************************
 *									*
 *		Some factorized error routines				*
 *									*
 ************************************************************************/


/**
 * Register a callback function that will be called on errors and
 * warnings. If handler is NULL, the error handler will be deactivated.
 *
 * If you only want to disable parser errors being printed to
 * stderr, use xmlParserOption XML_PARSE_NOERROR.
 *
 * This is the recommended way to collect errors from the parser and
 * takes precedence over all other error reporting mechanisms.
 * These are (in order of precedence):
 *
 * - per-context structured handler (#xmlCtxtSetErrorHandler)
 * - per-context structured "serror" SAX handler
 * - global structured handler (#xmlSetStructuredErrorFunc)
 * - per-context generic "error" and "warning" SAX handlers
 * - global generic handler (#xmlSetGenericErrorFunc)
 * - print to stderr
 *
 * @since 2.13.0
 * @param ctxt  an XML parser context
 * @param handler  error handler
 * @param data  data for error handler
 */
void
xmlCtxtSetErrorHandler(xmlParserCtxt *ctxt, xmlStructuredErrorFunc handler,
                       void *data)
{
    if (ctxt == NULL)
        return;
    ctxt->errorHandler = handler;
    ctxt->errorCtxt = data;
}

/**
 * Get the last error raised.
 *
 * Note that the XML parser typically doesn't stop after
 * encountering an error and will often report multiple errors.
 * Most of the time, the last error isn't useful. Future
 * versions might return the first parser error instead.
 *
 * @param ctx  an XML parser context
 * @returns NULL if no error occurred or a pointer to the error
 */
const xmlError *
xmlCtxtGetLastError(void *ctx)
{
    xmlParserCtxtPtr ctxt = (xmlParserCtxtPtr) ctx;

    if (ctxt == NULL)
        return (NULL);
    if (ctxt->lastError.code == XML_ERR_OK)
        return (NULL);
    return (&ctxt->lastError);
}

/**
 * Reset the last parser error to success. This does not change
 * the well-formedness status.
 *
 * @param ctx  an XML parser context
 */
void
xmlCtxtResetLastError(void *ctx)
{
    xmlParserCtxtPtr ctxt = (xmlParserCtxtPtr) ctx;

    if (ctxt == NULL)
        return;
    ctxt->errNo = XML_ERR_OK;
    if (ctxt->lastError.code == XML_ERR_OK)
        return;
    xmlResetError(&ctxt->lastError);
}

/**
 * Handle an out-of-memory error.
 *
 * @since 2.13.0
 * @param ctxt  an XML parser context
 */
void
xmlCtxtErrMemory(xmlParserCtxt *ctxt)
{
    xmlStructuredErrorFunc schannel = NULL;
    xmlGenericErrorFunc channel = NULL;
    void *data;

    if (ctxt == NULL) {
        xmlRaiseMemoryError(NULL, NULL, NULL, XML_FROM_PARSER, NULL);
        return;
    }

    ctxt->errNo = XML_ERR_NO_MEMORY;
    ctxt->instate = XML_PARSER_EOF; /* TODO: Remove after refactoring */
    ctxt->wellFormed = 0;
    ctxt->disableSAX = 2;

    if (ctxt->errorHandler) {
        schannel = ctxt->errorHandler;
        data = ctxt->errorCtxt;
    } else if ((ctxt->sax->initialized == XML_SAX2_MAGIC) &&
        (ctxt->sax->serror != NULL)) {
        schannel = ctxt->sax->serror;
        data = ctxt->userData;
    } else {
        channel = ctxt->sax->error;
        data = ctxt->userData;
    }

    xmlRaiseMemoryError(schannel, channel, data, XML_FROM_PARSER,
                        &ctxt->lastError);
}

/**
 * If filename is empty, use the one from context input if available.
 *
 * Report an IO error to the parser context.
 *
 * @param ctxt  parser context
 * @param code  xmlParserErrors code
 * @param uri  filename or URI (optional)
 */
void
xmlCtxtErrIO(xmlParserCtxt *ctxt, int code, const char *uri)
{
    const char *errstr, *msg, *str1, *str2;
    xmlErrorLevel level;

    if (ctxt == NULL)
        return;

    if (((code == XML_IO_ENOENT) ||
         (code == XML_IO_UNKNOWN))) {
        /*
         * Only report a warning if a file could not be found. This should
         * only be done for external entities, but the external entity loader
         * of xsltproc can try multiple paths and assumes that ENOENT doesn't
         * raise an error and aborts parsing.
         */
        if (ctxt->validate == 0)
            level = XML_ERR_WARNING;
        else
            level = XML_ERR_ERROR;
    } else if (code == XML_IO_NETWORK_ATTEMPT) {
        level = XML_ERR_ERROR;
    } else {
        level = XML_ERR_FATAL;
    }

    errstr = xmlErrString(code);

    if (uri == NULL) {
        msg = "%s\n";
        str1 = errstr;
        str2 = NULL;
    } else {
        msg = "failed to load \"%s\": %s\n";
        str1 = uri;
        str2 = errstr;
    }

    xmlCtxtErr(ctxt, NULL, XML_FROM_IO, code, level,
               (const xmlChar *) uri, NULL, NULL, 0,
               msg, str1, str2);
}

/**
 * @param ctxt  parser context
 * @returns true if the last error is catastrophic.
 */
int
xmlCtxtIsCatastrophicError(xmlParserCtxt *ctxt) {
    if (ctxt == NULL)
        return(1);

    return(xmlIsCatastrophicError(ctxt->lastError.level,
                                  ctxt->lastError.code));
}

/**
 * Raise a parser error.
 *
 * @param ctxt  a parser context
 * @param node  the current node or NULL
 * @param domain  the domain for the error
 * @param code  the code for the error
 * @param level  the xmlErrorLevel for the error
 * @param str1  extra string info
 * @param str2  extra string info
 * @param str3  extra string info
 * @param int1  extra int info
 * @param msg  the message to display/transmit
 * @param ap  extra parameters for the message display
 */
void
xmlCtxtVErr(xmlParserCtxt *ctxt, xmlNode *node, xmlErrorDomain domain,
            xmlParserErrors code, xmlErrorLevel level,
            const xmlChar *str1, const xmlChar *str2, const xmlChar *str3,
            int int1, const char *msg, va_list ap)
{
    xmlStructuredErrorFunc schannel = NULL;
    xmlGenericErrorFunc channel = NULL;
    void *data = NULL;
    const char *file = NULL;
    int line = 0;
    int col = 0;
    int res;

    if (code == XML_ERR_NO_MEMORY) {
        xmlCtxtErrMemory(ctxt);
        return;
    }

    if (ctxt == NULL) {
        res = xmlVRaiseError(NULL, NULL, NULL, NULL, node, domain, code,
                             level, NULL, 0, (const char *) str1,
                             (const char *) str2, (const char *) str3,
                             int1, 0, msg, ap);
        if (res < 0)
            xmlRaiseMemoryError(NULL, NULL, NULL, XML_FROM_PARSER, NULL);

        return;
    }

    if (PARSER_STOPPED(ctxt))
	return;

    /* Don't overwrite catastrophic errors */
    if (xmlCtxtIsCatastrophicError(ctxt))
        return;

    if (level == XML_ERR_WARNING) {
        if (ctxt->nbWarnings >= XML_MAX_ERRORS)
            return;
        ctxt->nbWarnings += 1;
    } else {
        /*
         * By long-standing design, the parser isn't completely
         * stopped on well-formedness errors. Only SAX callbacks
         * are disabled.
         *
         * In some situations, we really want to abort as fast
         * as possible.
         */
        if (xmlIsCatastrophicError(level, code) ||
            code == XML_ERR_RESOURCE_LIMIT ||
            code == XML_ERR_ENTITY_LOOP) {
            ctxt->disableSAX = 2; /* really stop parser */
        } else {
            /* Report at least one fatal error. */
            if (ctxt->nbErrors >= XML_MAX_ERRORS &&
                (level < XML_ERR_FATAL || ctxt->wellFormed == 0))
                return;

            if (level == XML_ERR_FATAL && ctxt->recovery == 0)
                ctxt->disableSAX = 1;
        }

        if (level == XML_ERR_FATAL)
            ctxt->wellFormed = 0;
        ctxt->errNo = code;
        ctxt->nbErrors += 1;
    }

    if (((ctxt->options & XML_PARSE_NOERROR) == 0) &&
        ((level != XML_ERR_WARNING) ||
         ((ctxt->options & XML_PARSE_NOWARNING) == 0))) {
        if (ctxt->errorHandler) {
            schannel = ctxt->errorHandler;
            data = ctxt->errorCtxt;
        } else if ((ctxt->sax->initialized == XML_SAX2_MAGIC) &&
            (ctxt->sax->serror != NULL)) {
            schannel = ctxt->sax->serror;
            data = ctxt->userData;
        } else if ((domain == XML_FROM_VALID) || (domain == XML_FROM_DTD)) {
            if (level == XML_ERR_WARNING)
                channel = ctxt->vctxt.warning;
            else
                channel = ctxt->vctxt.error;
            data = ctxt->vctxt.userData;
        } else {
            if (level == XML_ERR_WARNING)
                channel = ctxt->sax->warning;
            else
                channel = ctxt->sax->error;
            data = ctxt->userData;
        }
    }

    if (ctxt->input != NULL) {
        xmlParserInputPtr input = ctxt->input;

        if ((input->filename == NULL) &&
            (ctxt->inputNr > 1)) {
            input = ctxt->inputTab[ctxt->inputNr - 2];
        }
        file = input->filename;
        line = input->line;
        col = input->col;
    }

    res = xmlVRaiseError(schannel, channel, data, ctxt, node, domain, code,
                         level, file, line, (const char *) str1,
                         (const char *) str2, (const char *) str3, int1, col,
                         msg, ap);

    if (res < 0) {
        xmlCtxtErrMemory(ctxt);
        return;
    }
}

/**
 * Raise a parser error.
 *
 * @param ctxt  a parser context
 * @param node  the current node or NULL
 * @param domain  the domain for the error
 * @param code  the code for the error
 * @param level  the xmlErrorLevel for the error
 * @param str1  extra string info
 * @param str2  extra string info
 * @param str3  extra string info
 * @param int1  extra int info
 * @param msg  the message to display/transmit
 * @param ...  extra parameters for the message display
 */
void
xmlCtxtErr(xmlParserCtxt *ctxt, xmlNode *node, xmlErrorDomain domain,
           xmlParserErrors code, xmlErrorLevel level,
           const xmlChar *str1, const xmlChar *str2, const xmlChar *str3,
           int int1, const char *msg, ...)
{
    va_list ap;

    va_start(ap, msg);
    xmlCtxtVErr(ctxt, node, domain, code, level,
                str1, str2, str3, int1, msg, ap);
    va_end(ap);
}

/**
 * Get well-formedness and validation status after parsing. Also
 * reports catastrophic errors which are not related to parsing
 * like out-of-memory, I/O or other errors.
 *
 * @since 2.14.0
 *
 * @param ctxt  an XML parser context
 * @returns a bitmask of XML_STATUS_* flags ORed together.
 */
xmlParserStatus
xmlCtxtGetStatus(xmlParserCtxt *ctxt) {
    xmlParserStatus bits = 0;

    if (xmlCtxtIsCatastrophicError(ctxt)) {
        bits |= XML_STATUS_CATASTROPHIC_ERROR |
                XML_STATUS_NOT_WELL_FORMED |
                XML_STATUS_NOT_NS_WELL_FORMED;
        if ((ctxt != NULL) && (ctxt->validate))
            bits |= XML_STATUS_DTD_VALIDATION_FAILED;

        return(bits);
    }

    if (!ctxt->wellFormed)
        bits |= XML_STATUS_NOT_WELL_FORMED;
    if (!ctxt->nsWellFormed)
        bits |= XML_STATUS_NOT_NS_WELL_FORMED;
    if ((ctxt->validate) && (!ctxt->valid))
        bits |= XML_STATUS_DTD_VALIDATION_FAILED;

    return(bits);
}

/**
 * Handle a fatal parser error, i.e. violating Well-Formedness constraints
 *
 * @param ctxt  an XML parser context
 * @param code  the error number
 * @param info  extra information string
 */
void
xmlFatalErr(xmlParserCtxt *ctxt, xmlParserErrors code, const char *info)
{
    const char *errmsg;
    xmlErrorDomain domain = XML_FROM_PARSER;
    xmlErrorLevel level = XML_ERR_FATAL;

    errmsg = xmlErrString(code);

    if ((ctxt != NULL) && (ctxt->html)) {
        domain = XML_FROM_HTML;

        /* Continue if encoding is unsupported */
        if (code == XML_ERR_UNSUPPORTED_ENCODING)
            level = XML_ERR_ERROR;
    }

    if (info == NULL) {
        xmlCtxtErr(ctxt, NULL, domain, code, level,
                   NULL, NULL, NULL, 0, "%s\n", errmsg);
    } else {
        xmlCtxtErr(ctxt, NULL, domain, code, level,
                   (const xmlChar *) info, NULL, NULL, 0,
                   "%s: %s\n", errmsg, info);
    }
}

/**
 * Return window into current parser data.
 *
 * @param input  parser input
 * @param startOut  start of window (output)
 * @param sizeInOut  maximum size of window (in)
 *                   actual size of window (out)
 * @param offsetOut  offset of current position inside
 *                   window (out)
 */
void
xmlParserInputGetWindow(xmlParserInput *input, const xmlChar **startOut,
                        int *sizeInOut, int *offsetOut) {
    const xmlChar *cur, *base, *start;
    int n, col;
    int size = *sizeInOut;

    cur = input->cur;
    base = input->base;
    /* skip backwards over any end-of-lines */
    while ((cur > base) && ((*(cur) == '\n') || (*(cur) == '\r'))) {
	cur--;
    }
    n = 0;
    /* search backwards for beginning-of-line (to max buff size) */
    while ((n < size) && (cur > base) &&
	   (*cur != '\n') && (*cur != '\r')) {
        cur--;
        n++;
    }
    if ((n > 0) && ((*cur == '\n') || (*cur == '\r'))) {
        cur++;
    } else {
        /* skip over continuation bytes */
        while ((cur < input->cur) && ((*cur & 0xC0) == 0x80))
            cur++;
    }
    /* calculate the error position in terms of the current position */
    col = input->cur - cur;
    /* search forward for end-of-line (to max buff size) */
    n = 0;
    start = cur;
    /* copy selected text to our buffer */
    while ((*cur != 0) && (*(cur) != '\n') && (*(cur) != '\r')) {
        int len = input->end - cur;
        int c = xmlGetUTF8Char(cur, &len);

        if ((c < 0) || (n + len > size))
            break;
        cur += len;
	n += len;
    }

    /*
     * col can only point to the end of the buffer if
     * there's space for a marker.
     */
    if (col >= n)
        col = n < size ? n : size - 1;

    *startOut = start;
    *sizeInOut = n;
    *offsetOut = col;
}

/**
 * Check whether the character is allowed by the production
 *
 * @deprecated Internal function, don't use.
 *
 * ```
 * [84] Letter ::= BaseChar | Ideographic
 * ```
 *
 * @param c  an unicode character (int)
 * @returns 0 if not, non-zero otherwise
 */
int
xmlIsLetter(int c) {
    return(IS_BASECHAR(c) || IS_IDEOGRAPHIC(c));
}

/************************************************************************
 *									*
 *		Input handling functions for progressive parsing	*
 *									*
 ************************************************************************/

/* we need to keep enough input to show errors in context */
#define LINE_LEN        80

/**
 * @deprecated This function was internal and is deprecated.
 *
 * @param in  an XML parser input
 * @param len  an indicative size for the lookahead
 * @returns -1 as this is an error to use it.
 */
int
xmlParserInputRead(xmlParserInput *in ATTRIBUTE_UNUSED, int len ATTRIBUTE_UNUSED) {
    return(-1);
}

/**
 * Grow the input buffer.
 *
 * @param ctxt  an XML parser context
 * @returns the number of bytes read or -1 in case of error.
 */
int
xmlParserGrow(xmlParserCtxt *ctxt) {
    xmlParserInputPtr in = ctxt->input;
    xmlParserInputBufferPtr buf = in->buf;
    size_t curEnd = in->end - in->cur;
    size_t curBase = in->cur - in->base;
    size_t maxLength = (ctxt->options & XML_PARSE_HUGE) ?
                       XML_MAX_HUGE_LENGTH :
                       XML_MAX_LOOKUP_LIMIT;
    int ret;

    if (buf == NULL)
        return(0);
    /* Don't grow push parser buffer. */
    if (PARSER_PROGRESSIVE(ctxt))
        return(0);
    /* Don't grow memory buffers. */
    if ((buf->encoder == NULL) && (buf->readcallback == NULL))
        return(0);
    if (buf->error != 0)
        return(-1);

    if (curBase > maxLength) {
        xmlFatalErr(ctxt, XML_ERR_RESOURCE_LIMIT,
                    "Buffer size limit exceeded, try XML_PARSE_HUGE\n");
	return(-1);
    }

    if (curEnd >= INPUT_CHUNK)
        return(0);

    ret = xmlParserInputBufferGrow(buf, INPUT_CHUNK);
    xmlBufUpdateInput(buf->buffer, in, curBase);

    if (ret < 0) {
        xmlCtxtErrIO(ctxt, buf->error, NULL);
    }

    return(ret);
}

/**
 * Raises an error with `code` if the input wasn't consumed
 * completely.
 *
 * @param ctxt  parser ctxt
 * @param code  error code
 */
void
xmlParserCheckEOF(xmlParserCtxt *ctxt, xmlParserErrors code) {
    xmlParserInputPtr in = ctxt->input;
    xmlParserInputBufferPtr buf;

    if (ctxt->errNo != XML_ERR_OK)
        return;

    if (in->cur < in->end) {
        xmlFatalErr(ctxt, code, NULL);
        return;
    }

    buf = in->buf;
    if ((buf != NULL) && (buf->encoder != NULL)) {
        size_t curBase = in->cur - in->base;
        size_t sizeOut = 64;
        xmlCharEncError ret;

        /*
         * Check for truncated multi-byte sequence
         */
        ret = xmlCharEncInput(buf, &sizeOut, /* flush */ 1);
        xmlBufUpdateInput(buf->buffer, in, curBase);
        if (ret != XML_ENC_ERR_SUCCESS) {
            xmlCtxtErrIO(ctxt, buf->error, NULL);
            return;
        }

        /* Shouldn't happen */
        if (in->cur < in->end)
            xmlFatalErr(ctxt, XML_ERR_INTERNAL_ERROR, "expected EOF");
    }
}

/**
 * This function increase the input for the parser. It tries to
 * preserve pointers to the input buffer, and keep already read data
 *
 * @deprecated Don't use.
 *
 * @param in  an XML parser input
 * @param len  an indicative size for the lookahead
 * @returns the amount of char read, or -1 in case of error, 0 indicate the
 * end of this entity
 */
int
xmlParserInputGrow(xmlParserInput *in, int len) {
    int ret;
    size_t indx;

    if ((in == NULL) || (len < 0)) return(-1);
    if (in->buf == NULL) return(-1);
    if (in->base == NULL) return(-1);
    if (in->cur == NULL) return(-1);
    if (in->buf->buffer == NULL) return(-1);

    /* Don't grow memory buffers. */
    if ((in->buf->encoder == NULL) && (in->buf->readcallback == NULL))
        return(0);

    indx = in->cur - in->base;
    if (xmlBufUse(in->buf->buffer) > (unsigned int) indx + INPUT_CHUNK) {
        return(0);
    }
    ret = xmlParserInputBufferGrow(in->buf, len);

    in->base = xmlBufContent(in->buf->buffer);
    if (in->base == NULL) {
        in->base = BAD_CAST "";
        in->cur = in->base;
        in->end = in->base;
        return(-1);
    }
    in->cur = in->base + indx;
    in->end = xmlBufEnd(in->buf->buffer);

    return(ret);
}

/**
 * Shrink the input buffer.
 *
 * @param ctxt  an XML parser context
 */
void
xmlParserShrink(xmlParserCtxt *ctxt) {
    xmlParserInputPtr in = ctxt->input;
    xmlParserInputBufferPtr buf = in->buf;
    size_t used, res;

    if (buf == NULL)
        return;

    used = in->cur - in->base;

    if (used > LINE_LEN) {
        res = xmlBufShrink(buf->buffer, used - LINE_LEN);

        if (res > 0) {
            used -= res;
            xmlSaturatedAddSizeT(&in->consumed, res);
        }

        xmlBufUpdateInput(buf->buffer, in, used);
    }
}

/**
 * This function removes used input for the parser.
 *
 * @deprecated Don't use.
 *
 * @param in  an XML parser input
 */
void
xmlParserInputShrink(xmlParserInput *in) {
    size_t used;
    size_t ret;

    if (in == NULL) return;
    if (in->buf == NULL) return;
    if (in->base == NULL) return;
    if (in->cur == NULL) return;
    if (in->buf->buffer == NULL) return;

    used = in->cur - in->base;

    if (used > LINE_LEN) {
	ret = xmlBufShrink(in->buf->buffer, used - LINE_LEN);
	if (ret > 0) {
            used -= ret;
            xmlSaturatedAddSizeT(&in->consumed, ret);
	}

        xmlBufUpdateInput(in->buf->buffer, in, used);
    }
}

/************************************************************************
 *									*
 *		UTF8 character input and related functions		*
 *									*
 ************************************************************************/

/**
 * Skip to the next char input char.
 *
 * @deprecated Internal function, do not use.
 *
 * @param ctxt  the XML parser context
 */

void
xmlNextChar(xmlParserCtxt *ctxt)
{
    const unsigned char *cur;
    size_t avail;
    int c;

    if ((ctxt == NULL) || (ctxt->input == NULL))
        return;

    avail = ctxt->input->end - ctxt->input->cur;

    if (avail < INPUT_CHUNK) {
        xmlParserGrow(ctxt);
        if (ctxt->input->cur >= ctxt->input->end)
            return;
        avail = ctxt->input->end - ctxt->input->cur;
    }

    cur = ctxt->input->cur;
    c = *cur;

    if (c < 0x80) {
        if (c == '\n') {
            ctxt->input->cur++;
            ctxt->input->line++;
            ctxt->input->col = 1;
        } else if (c == '\r') {
            /*
             *   2.11 End-of-Line Handling
             *   the literal two-character sequence "#xD#xA" or a standalone
             *   literal #xD, an XML processor must pass to the application
             *   the single character #xA.
             */
            ctxt->input->cur += ((cur[1] == '\n') ? 2 : 1);
            ctxt->input->line++;
            ctxt->input->col = 1;
            return;
        } else {
            ctxt->input->cur++;
            ctxt->input->col++;
        }
    } else {
        ctxt->input->col++;

        if ((avail < 2) || (cur[1] & 0xc0) != 0x80)
            goto encoding_error;

        if (c < 0xe0) {
            /* 2-byte code */
            if (c < 0xc2)
                goto encoding_error;
            ctxt->input->cur += 2;
        } else {
            unsigned int val = (c << 8) | cur[1];

            if ((avail < 3) || (cur[2] & 0xc0) != 0x80)
                goto encoding_error;

            if (c < 0xf0) {
                /* 3-byte code */
                if ((val < 0xe0a0) || ((val >= 0xeda0) && (val < 0xee00)))
                    goto encoding_error;
                ctxt->input->cur += 3;
            } else {
                if ((avail < 4) || ((cur[3] & 0xc0) != 0x80))
                    goto encoding_error;

                /* 4-byte code */
                if ((val < 0xf090) || (val >= 0xf490))
                    goto encoding_error;
                ctxt->input->cur += 4;
            }
        }
    }

    return;

encoding_error:
    /* Only report the first error */
    if ((ctxt->input->flags & XML_INPUT_ENCODING_ERROR) == 0) {
        xmlCtxtErrIO(ctxt, XML_ERR_INVALID_ENCODING, NULL);
        ctxt->input->flags |= XML_INPUT_ENCODING_ERROR;
    }
    ctxt->input->cur++;
}

/**
 * The current char value, if using UTF-8 this may actually span multiple
 * bytes in the input buffer. Implement the end of line normalization:
 *
 * @deprecated Internal function, do not use.
 *
 * 2.11 End-of-Line Handling
 *
 * Wherever an external parsed entity or the literal entity value
 * of an internal parsed entity contains either the literal two-character
 * sequence "#xD#xA" or a standalone literal \#xD, an XML processor
 * must pass to the application the single character \#xA.
 * This behavior can conveniently be produced by normalizing all
 * line breaks to \#xA on input, before parsing.)
 *
 * @param ctxt  the XML parser context
 * @param len  pointer to the length of the char read
 * @returns the current char value and its length
 */

int
xmlCurrentChar(xmlParserCtxt *ctxt, int *len) {
    const unsigned char *cur;
    size_t avail;
    int c;

    if ((ctxt == NULL) || (len == NULL) || (ctxt->input == NULL)) return(0);

    avail = ctxt->input->end - ctxt->input->cur;

    if (avail < INPUT_CHUNK) {
        xmlParserGrow(ctxt);
        avail = ctxt->input->end - ctxt->input->cur;
    }

    cur = ctxt->input->cur;
    c = *cur;

    if (c < 0x80) {
	/* 1-byte code */
        if (c < 0x20) {
            /*
             *   2.11 End-of-Line Handling
             *   the literal two-character sequence "#xD#xA" or a standalone
             *   literal #xD, an XML processor must pass to the application
             *   the single character #xA.
             */
            if (c == '\r') {
                /*
                 * TODO: This function shouldn't change the 'cur' pointer
                 * as side effect, but the NEXTL macro in parser.c relies
                 * on this behavior when incrementing line numbers.
                 */
                if (cur[1] == '\n')
                    ctxt->input->cur++;
                *len = 1;
                c = '\n';
            } else if (c == 0) {
                if (ctxt->input->cur >= ctxt->input->end) {
                    *len = 0;
                } else {
                    *len = 1;
                    /*
                     * TODO: Null bytes should be handled by callers,
                     * but this can be tricky.
                     */
                    xmlFatalErr(ctxt, XML_ERR_INVALID_CHAR,
                            "Char 0x0 out of allowed range\n");
                }
            } else {
                *len = 1;
            }
        } else {
            *len = 1;
        }

        return(c);
    } else {
        int val;

        if (avail < 2)
            goto incomplete_sequence;
        if ((cur[1] & 0xc0) != 0x80)
            goto encoding_error;

        if (c < 0xe0) {
            /* 2-byte code */
            if (c < 0xc2)
                goto encoding_error;
            val = (c & 0x1f) << 6;
            val |= cur[1] & 0x3f;
            *len = 2;
        } else {
            if (avail < 3)
                goto incomplete_sequence;
            if ((cur[2] & 0xc0) != 0x80)
                goto encoding_error;

            if (c < 0xf0) {
                /* 3-byte code */
                val = (c & 0xf) << 12;
                val |= (cur[1] & 0x3f) << 6;
                val |= cur[2] & 0x3f;
                if ((val < 0x800) || ((val >= 0xd800) && (val < 0xe000)))
                    goto encoding_error;
                *len = 3;
            } else {
                if (avail < 4)
                    goto incomplete_sequence;
                if ((cur[3] & 0xc0) != 0x80)
                    goto encoding_error;

                /* 4-byte code */
                val = (c & 0x0f) << 18;
                val |= (cur[1] & 0x3f) << 12;
                val |= (cur[2] & 0x3f) << 6;
                val |= cur[3] & 0x3f;
                if ((val < 0x10000) || (val >= 0x110000))
                    goto encoding_error;
                *len = 4;
            }
        }

        return(val);
    }

encoding_error:
    /* Only report the first error */
    if ((ctxt->input->flags & XML_INPUT_ENCODING_ERROR) == 0) {
        xmlCtxtErrIO(ctxt, XML_ERR_INVALID_ENCODING, NULL);
        ctxt->input->flags |= XML_INPUT_ENCODING_ERROR;
    }
    *len = 1;
    return(XML_INVALID_CHAR);

incomplete_sequence:
    /*
     * An encoding problem may arise from a truncated input buffer
     * splitting a character in the middle. In that case do not raise
     * an error but return 0. This should only happen when push parsing
     * char data.
     */
    *len = 0;
    return(0);
}

/**
 * The current char value, if using UTF-8 this may actually span multiple
 * bytes in the input buffer.
 *
 * @deprecated Internal function, do not use.
 *
 * @param ctxt  the XML parser context
 * @param cur  pointer to the beginning of the char
 * @param len  pointer to the length of the char read
 * @returns the current char value and its length
 */

int
xmlStringCurrentChar(xmlParserCtxt *ctxt ATTRIBUTE_UNUSED,
                     const xmlChar *cur, int *len) {
    int c;

    if ((cur == NULL) || (len == NULL))
        return(0);

    /* cur is zero-terminated, so we can lie about its length. */
    *len = 4;
    c = xmlGetUTF8Char(cur, len);

    return((c < 0) ? 0 : c);
}

/**
 * append the char value in the array
 *
 * @deprecated Internal function, don't use.
 *
 * @param out  pointer to an array of xmlChar
 * @param val  the char value
 * @returns the number of xmlChar written
 */
int
xmlCopyCharMultiByte(xmlChar *out, int val) {
    if ((out == NULL) || (val < 0)) return(0);
    /*
     * We are supposed to handle UTF8, check it's valid
     * From rfc2044: encoding of the Unicode values on UTF-8:
     *
     * UCS-4 range (hex.)           UTF-8 octet sequence (binary)
     * 0000 0000-0000 007F   0xxxxxxx
     * 0000 0080-0000 07FF   110xxxxx 10xxxxxx
     * 0000 0800-0000 FFFF   1110xxxx 10xxxxxx 10xxxxxx
     */
    if  (val >= 0x80) {
	xmlChar *savedout = out;
	int bits;
	if (val <   0x800) { *out++= (val >>  6) | 0xC0;  bits=  0; }
	else if (val < 0x10000) { *out++= (val >> 12) | 0xE0;  bits=  6;}
	else if (val < 0x110000)  { *out++= (val >> 18) | 0xF0;  bits=  12; }
	else {
#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
            xmlAbort("xmlCopyCharMultiByte: codepoint out of range\n");
#endif
	    return(0);
	}
	for ( ; bits >= 0; bits-= 6)
	    *out++= ((val >> bits) & 0x3F) | 0x80 ;
	return (out - savedout);
    }
    *out = val;
    return 1;
}

/**
 * append the char value in the array
 *
 * @deprecated Don't use.
 *
 * @param len  Ignored, compatibility
 * @param out  pointer to an array of xmlChar
 * @param val  the char value
 * @returns the number of xmlChar written
 */

int
xmlCopyChar(int len ATTRIBUTE_UNUSED, xmlChar *out, int val) {
    if ((out == NULL) || (val < 0)) return(0);
    /* the len parameter is ignored */
    if  (val >= 0x80) {
	return(xmlCopyCharMultiByte (out, val));
    }
    *out = val;
    return 1;
}

/************************************************************************
 *									*
 *		Commodity functions to switch encodings			*
 *									*
 ************************************************************************/

/**
 * Installs a custom implementation to convert between character
 * encodings.
 *
 * This bypasses legacy feature like global encoding handlers or
 * encoding aliases.
 *
 * @since 2.14.0
 * @param ctxt  parser context
 * @param impl  callback
 * @param vctxt  user data
 */
void
xmlCtxtSetCharEncConvImpl(xmlParserCtxt *ctxt, xmlCharEncConvImpl impl,
                          void *vctxt) {
    if (ctxt == NULL)
        return;

    ctxt->convImpl = impl;
    ctxt->convCtxt = vctxt;
}

static xmlParserErrors
xmlDetectEBCDIC(xmlParserCtxtPtr ctxt, xmlCharEncodingHandlerPtr *hout) {
    xmlChar out[200];
    xmlParserInputPtr input = ctxt->input;
    xmlCharEncodingHandlerPtr handler;
    int inlen, outlen, i;
    xmlParserErrors code;
    xmlCharEncError res;

    *hout = NULL;

    /*
     * To detect the EBCDIC code page, we convert the first 200 bytes
     * to IBM037 (EBCDIC-US) and try to find the encoding declaration.
     */
    code = xmlCreateCharEncodingHandler("IBM037", XML_ENC_INPUT,
            ctxt->convImpl, ctxt->convCtxt, &handler);
    if (code != XML_ERR_OK)
        return(code);
    outlen = sizeof(out) - 1;
    inlen = input->end - input->cur;
    res = xmlEncInputChunk(handler, out, &outlen, input->cur, &inlen,
                           /* flush */ 0);
    /*
     * Return the EBCDIC handler if decoding failed. The error will
     * be reported later.
     */
    if (res < 0)
        goto done;
    out[outlen] = 0;

    for (i = 0; i < outlen; i++) {
        if (out[i] == '>')
            break;
        if ((out[i] == 'e') &&
            (xmlStrncmp(out + i, BAD_CAST "encoding", 8) == 0)) {
            int start, cur, quote;

            i += 8;
            while (IS_BLANK_CH(out[i]))
                i += 1;
            if (out[i++] != '=')
                break;
            while (IS_BLANK_CH(out[i]))
                i += 1;
            quote = out[i++];
            if ((quote != '\'') && (quote != '"'))
                break;
            start = i;
            cur = out[i];
            while (((cur >= 'a') && (cur <= 'z')) ||
                   ((cur >= 'A') && (cur <= 'Z')) ||
                   ((cur >= '0') && (cur <= '9')) ||
                   (cur == '.') || (cur == '_') ||
                   (cur == '-'))
                cur = out[++i];
            if (cur != quote)
                break;
            out[i] = 0;
            xmlCharEncCloseFunc(handler);
            code = xmlCreateCharEncodingHandler((char *) out + start,
                    XML_ENC_INPUT, ctxt->convImpl, ctxt->convCtxt,
                    &handler);
            if (code != XML_ERR_OK)
                return(code);
            *hout = handler;
            return(XML_ERR_OK);
        }
    }

done:
    /*
     * Encoding handlers are stateful, so we have to recreate them.
     */
    xmlCharEncCloseFunc(handler);
    code = xmlCreateCharEncodingHandler("IBM037", XML_ENC_INPUT,
            ctxt->convImpl, ctxt->convCtxt, &handler);
    if (code != XML_ERR_OK)
        return(code);
    *hout = handler;
    return(XML_ERR_OK);
}

/**
 * Use encoding specified by enum to decode input data. This overrides
 * the encoding found in the XML declaration.
 *
 * This function can also be used to override the encoding of chunks
 * passed to #xmlParseChunk.
 *
 * @param ctxt  the parser context
 * @param enc  the encoding value (number)
 * @returns 0 in case of success, -1 otherwise
 */
int
xmlSwitchEncoding(xmlParserCtxt *ctxt, xmlCharEncoding enc)
{
    xmlCharEncodingHandlerPtr handler = NULL;
    int ret;
    xmlParserErrors code;

    if ((ctxt == NULL) || (ctxt->input == NULL))
        return(-1);

    code = xmlLookupCharEncodingHandler(enc, &handler);
    if (code != 0) {
        xmlFatalErr(ctxt, code, NULL);
        return(-1);
    }

    ret = xmlSwitchToEncoding(ctxt, handler);

    if ((ret >= 0) && (enc == XML_CHAR_ENCODING_NONE)) {
        ctxt->input->flags &= ~XML_INPUT_HAS_ENCODING;
    }

    return(ret);
}

/**
 * @param ctxt  the parser context
 * @param input  the input strea,
 * @param encoding  the encoding name
 * @returns 0 in case of success, -1 otherwise
 */
static int
xmlSwitchInputEncodingName(xmlParserCtxtPtr ctxt, xmlParserInputPtr input,
                           const char *encoding) {
    xmlCharEncodingHandlerPtr handler;
    xmlParserErrors res;

    if (encoding == NULL)
        return(-1);

    res = xmlCreateCharEncodingHandler(encoding, XML_ENC_INPUT,
            ctxt->convImpl, ctxt->convCtxt, &handler);
    if (res == XML_ERR_UNSUPPORTED_ENCODING) {
        xmlWarningMsg(ctxt, XML_ERR_UNSUPPORTED_ENCODING,
                      "Unsupported encoding: %s\n", BAD_CAST encoding, NULL);
        return(-1);
    } else if (res != XML_ERR_OK) {
        xmlFatalErr(ctxt, res, encoding);
        return(-1);
    }

    res  = xmlInputSetEncodingHandler(input, handler);
    if (res != XML_ERR_OK) {
        xmlCtxtErrIO(ctxt, res, NULL);
        return(-1);
    }

    return(0);
}

/**
 * Use specified encoding to decode input data. This overrides the
 * encoding found in the XML declaration.
 *
 * This function can also be used to override the encoding of chunks
 * passed to #xmlParseChunk.
 *
 * @since 2.13.0
 *
 * @param ctxt  the parser context
 * @param encoding  the encoding name
 * @returns 0 in case of success, -1 otherwise
 */
int
xmlSwitchEncodingName(xmlParserCtxt *ctxt, const char *encoding) {
    if (ctxt == NULL)
        return(-1);

    return(xmlSwitchInputEncodingName(ctxt, ctxt->input, encoding));
}

/**
 * Use encoding handler to decode input data.
 *
 * Closes the handler on error.
 *
 * @param input  the input stream
 * @param handler  the encoding handler
 * @returns an xmlParserErrors code.
 */
xmlParserErrors
xmlInputSetEncodingHandler(xmlParserInput *input,
                           xmlCharEncodingHandler *handler) {
    xmlParserInputBufferPtr in;
    xmlBufPtr buf;
    xmlParserErrors code = XML_ERR_OK;

    if ((input == NULL) || (input->buf == NULL)) {
        xmlCharEncCloseFunc(handler);
	return(XML_ERR_ARGUMENT);
    }
    in = input->buf;

    input->flags |= XML_INPUT_HAS_ENCODING;

    /*
     * UTF-8 requires no encoding handler.
     */
    if ((handler != NULL) &&
        (xmlStrcasecmp(BAD_CAST handler->name, BAD_CAST "UTF-8") == 0)) {
        xmlCharEncCloseFunc(handler);
        handler = NULL;
    }

    if (in->encoder == handler)
        return(XML_ERR_OK);

    if (in->encoder != NULL) {
        /*
         * Switching encodings during parsing is a really bad idea,
         * but Chromium can switch between ISO-8859-1 and UTF-16 before
         * separate calls to xmlParseChunk.
         *
         * TODO: We should check whether the "raw" input buffer is empty and
         * convert the old content using the old encoder.
         */

        xmlCharEncCloseFunc(in->encoder);
        in->encoder = handler;
        return(XML_ERR_OK);
    }

    buf = xmlBufCreate(XML_IO_BUFFER_SIZE);
    if (buf == NULL) {
        xmlCharEncCloseFunc(handler);
        return(XML_ERR_NO_MEMORY);
    }

    in->encoder = handler;
    in->raw = in->buffer;
    in->buffer = buf;

    /*
     * Is there already some content down the pipe to convert ?
     */
    if (input->end > input->base) {
        size_t processed;
        size_t nbchars;
        xmlCharEncError res;

        /*
         * Shrink the current input buffer.
         * Move it as the raw buffer and create a new input buffer
         */
        processed = input->cur - input->base;
        xmlBufShrink(in->raw, processed);
        input->consumed += processed;
        in->rawconsumed = processed;

        /*
         * If we're push-parsing, we must convert the whole buffer.
         *
         * If we're pull-parsing, we could be parsing from a huge
         * memory buffer which we don't want to convert completely.
         */
        if (input->flags & XML_INPUT_PROGRESSIVE)
            nbchars = SIZE_MAX;
        else
            nbchars = 4000 /* MINLEN */;
        res = xmlCharEncInput(in, &nbchars, /* flush */ 0);
        if (res != XML_ENC_ERR_SUCCESS)
            code = in->error;
    }

    xmlBufResetInput(in->buffer, input);

    return(code);
}

/**
 * Use encoding handler to decode input data.
 *
 * @deprecated Internal function, don't use.
 *
 * @param ctxt  the parser context, only for error reporting
 * @param input  the input stream
 * @param handler  the encoding handler
 * @returns 0 in case of success, -1 otherwise
 */
int
xmlSwitchInputEncoding(xmlParserCtxt *ctxt, xmlParserInput *input,
                       xmlCharEncodingHandler *handler) {
    xmlParserErrors code = xmlInputSetEncodingHandler(input, handler);

    if (code != XML_ERR_OK) {
        xmlCtxtErrIO(ctxt, code, NULL);
        return(-1);
    }

    return(0);
}

/**
 * Use encoding handler to decode input data.
 *
 * This function can be used to enforce the encoding of chunks passed
 * to #xmlParseChunk.
 *
 * @param ctxt  the parser context
 * @param handler  the encoding handler
 * @returns 0 in case of success, -1 otherwise
 */
int
xmlSwitchToEncoding(xmlParserCtxt *ctxt, xmlCharEncodingHandler *handler)
{
    xmlParserErrors code;

    if (ctxt == NULL)
        return(-1);

    code = xmlInputSetEncodingHandler(ctxt->input, handler);
    if (code != XML_ERR_OK) {
        xmlCtxtErrIO(ctxt, code, NULL);
        return(-1);
    }

    return(0);
}

/**
 * Handle optional BOM, detect and switch to encoding.
 *
 * Assumes that there are at least four bytes in the input buffer.
 *
 * @param ctxt  the parser context
 */
void
xmlDetectEncoding(xmlParserCtxt *ctxt) {
    const xmlChar *in;
    xmlCharEncoding enc;
    int bomSize;
    int autoFlag = 0;

    if (xmlParserGrow(ctxt) < 0)
        return;
    in = ctxt->input->cur;
    if (ctxt->input->end - in < 4)
        return;

    if (ctxt->input->flags & XML_INPUT_HAS_ENCODING) {
        /*
         * If the encoding was already set, only skip the BOM which was
         * possibly decoded to UTF-8.
         */
        if ((in[0] == 0xEF) && (in[1] == 0xBB) && (in[2] == 0xBF)) {
            ctxt->input->cur += 3;
        }

        return;
    }

    enc = XML_CHAR_ENCODING_NONE;
    bomSize = 0;

    /*
     * BOM sniffing and detection of initial bytes of an XML
     * declaration.
     *
     * The HTML5 spec doesn't cover UTF-32 (UCS-4) or EBCDIC.
     */
    switch (in[0]) {
        case 0x00:
            if ((!ctxt->html) &&
                (in[1] == 0x00) && (in[2] == 0x00) && (in[3] == 0x3C)) {
                enc = XML_CHAR_ENCODING_UCS4BE;
                autoFlag = XML_INPUT_AUTO_OTHER;
            } else if ((in[1] == 0x3C) && (in[2] == 0x00) && (in[3] == 0x3F)) {
                /*
                 * TODO: The HTML5 spec requires to check that the
                 * next codepoint is an 'x'.
                 */
                enc = XML_CHAR_ENCODING_UTF16BE;
                autoFlag = XML_INPUT_AUTO_UTF16BE;
            }
            break;

        case 0x3C:
            if (in[1] == 0x00) {
                if ((!ctxt->html) &&
                    (in[2] == 0x00) && (in[3] == 0x00)) {
                    enc = XML_CHAR_ENCODING_UCS4LE;
                    autoFlag = XML_INPUT_AUTO_OTHER;
                } else if ((in[2] == 0x3F) && (in[3] == 0x00)) {
                    /*
                     * TODO: The HTML5 spec requires to check that the
                     * next codepoint is an 'x'.
                     */
                    enc = XML_CHAR_ENCODING_UTF16LE;
                    autoFlag = XML_INPUT_AUTO_UTF16LE;
                }
            }
            break;

        case 0x4C:
	    if ((!ctxt->html) &&
                (in[1] == 0x6F) && (in[2] == 0xA7) && (in[3] == 0x94)) {
	        enc = XML_CHAR_ENCODING_EBCDIC;
                autoFlag = XML_INPUT_AUTO_OTHER;
            }
            break;

        case 0xEF:
            if ((in[1] == 0xBB) && (in[2] == 0xBF)) {
                enc = XML_CHAR_ENCODING_UTF8;
                autoFlag = XML_INPUT_AUTO_UTF8;
                bomSize = 3;
            }
            break;

        case 0xFE:
            if (in[1] == 0xFF) {
                enc = XML_CHAR_ENCODING_UTF16BE;
                autoFlag = XML_INPUT_AUTO_UTF16BE;
                bomSize = 2;
            }
            break;

        case 0xFF:
            if (in[1] == 0xFE) {
                enc = XML_CHAR_ENCODING_UTF16LE;
                autoFlag = XML_INPUT_AUTO_UTF16LE;
                bomSize = 2;
            }
            break;
    }

    if (bomSize > 0) {
        ctxt->input->cur += bomSize;
    }

    if (enc != XML_CHAR_ENCODING_NONE) {
        ctxt->input->flags |= autoFlag;

        if (enc == XML_CHAR_ENCODING_EBCDIC) {
            xmlCharEncodingHandlerPtr handler;
            xmlParserErrors res;

            res = xmlDetectEBCDIC(ctxt, &handler);
            if (res != XML_ERR_OK) {
                xmlFatalErr(ctxt, res, "detecting EBCDIC\n");
            } else {
                xmlSwitchToEncoding(ctxt, handler);
            }
        } else {
            xmlSwitchEncoding(ctxt, enc);
        }
    }
}

/**
 * Set the encoding from a declaration in the document.
 *
 * If no encoding was set yet, switch the encoding. Otherwise, only warn
 * about encoding mismatches.
 *
 * Takes ownership of 'encoding'.
 *
 * @param ctxt  the parser context
 * @param encoding  declared encoding
 */
void
xmlSetDeclaredEncoding(xmlParserCtxt *ctxt, xmlChar *encoding) {
    if (((ctxt->input->flags & XML_INPUT_HAS_ENCODING) == 0) &&
        ((ctxt->options & XML_PARSE_IGNORE_ENC) == 0)) {
        xmlCharEncodingHandlerPtr handler;
        xmlParserErrors res;
        xmlCharEncFlags flags = XML_ENC_INPUT;

        /*
         * xmlSwitchEncodingName treats unsupported encodings as
         * warnings, but we want it to be an error in an encoding
         * declaration.
         */
        if (ctxt->html)
            flags |= XML_ENC_HTML;
        res = xmlCreateCharEncodingHandler((const char *) encoding,
                flags, ctxt->convImpl, ctxt->convCtxt, &handler);
        if (res != XML_ERR_OK) {
            xmlFatalErr(ctxt, res, (const char *) encoding);
            xmlFree(encoding);
            return;
        }

        res  = xmlInputSetEncodingHandler(ctxt->input, handler);
        if (res != XML_ERR_OK) {
            xmlCtxtErrIO(ctxt, res, NULL);
            xmlFree(encoding);
            return;
        }

        ctxt->input->flags |= XML_INPUT_USES_ENC_DECL;
    } else if (ctxt->input->flags & XML_INPUT_AUTO_ENCODING) {
        static const char *allowedUTF8[] = {
            "UTF-8", "UTF8", NULL
        };
        static const char *allowedUTF16LE[] = {
            "UTF-16", "UTF-16LE", "UTF16", NULL
        };
        static const char *allowedUTF16BE[] = {
            "UTF-16", "UTF-16BE", "UTF16", NULL
        };
        const char **allowed = NULL;
        const char *autoEnc = NULL;

        switch (ctxt->input->flags & XML_INPUT_AUTO_ENCODING) {
            case XML_INPUT_AUTO_UTF8:
                allowed = allowedUTF8;
                autoEnc = "UTF-8";
                break;
            case XML_INPUT_AUTO_UTF16LE:
                allowed = allowedUTF16LE;
                autoEnc = "UTF-16LE";
                break;
            case XML_INPUT_AUTO_UTF16BE:
                allowed = allowedUTF16BE;
                autoEnc = "UTF-16BE";
                break;
        }

        if (allowed != NULL) {
            const char **p;
            int match = 0;

            for (p = allowed; *p != NULL; p++) {
                if (xmlStrcasecmp(encoding, BAD_CAST *p) == 0) {
                    match = 1;
                    break;
                }
            }

            if (match == 0) {
                xmlWarningMsg(ctxt, XML_WAR_ENCODING_MISMATCH,
                              "Encoding '%s' doesn't match "
                              "auto-detected '%s'\n",
                              encoding, BAD_CAST autoEnc);
                xmlFree(encoding);
                encoding = xmlStrdup(BAD_CAST autoEnc);
                if (encoding == NULL)
                    xmlCtxtErrMemory(ctxt);
            }
        }
    }

    if (ctxt->encoding != NULL)
        xmlFree(ctxt->encoding);
    ctxt->encoding = encoding;
}

/**
 * @since 2.14.0
 *
 * @param ctxt  parser context
 * @returns the encoding from the encoding declaration. This can differ
 * from the actual encoding.
 */
const xmlChar *
xmlCtxtGetDeclaredEncoding(xmlParserCtxt *ctxt) {
    if (ctxt == NULL)
        return(NULL);

    return(ctxt->encoding);
}

/**
 * @param ctxt  the parser context
 * @returns the actual used to parse the document. This can differ from
 * the declared encoding.
 */
const xmlChar *
xmlGetActualEncoding(xmlParserCtxt *ctxt) {
    const xmlChar *encoding = NULL;

    if ((ctxt->input->flags & XML_INPUT_USES_ENC_DECL) ||
        (ctxt->input->flags & XML_INPUT_AUTO_ENCODING)) {
        /* Preserve encoding exactly */
        encoding = ctxt->encoding;
    } else if ((ctxt->input->buf) && (ctxt->input->buf->encoder)) {
        encoding = BAD_CAST ctxt->input->buf->encoder->name;
    } else if (ctxt->input->flags & XML_INPUT_HAS_ENCODING) {
        encoding = BAD_CAST "UTF-8";
    }

    return(encoding);
}

/************************************************************************
 *									*
 *	Commodity functions to handle entities processing		*
 *									*
 ************************************************************************/

/**
 * Free up an input stream.
 *
 * @param input  an xmlParserInput
 */
void
xmlFreeInputStream(xmlParserInput *input) {
    if (input == NULL) return;

    if (input->filename != NULL) xmlFree((char *) input->filename);
    if (input->version != NULL) xmlFree((char *) input->version);
    if ((input->free != NULL) && (input->base != NULL))
        input->free((xmlChar *) input->base);
    if (input->buf != NULL)
        xmlFreeParserInputBuffer(input->buf);
    xmlFree(input);
}

/**
 * Create a new input stream structure.
 *
 * @deprecated Use #xmlNewInputFromUrl or similar functions.
 *
 * @param ctxt  an XML parser context
 * @returns the new input stream or NULL
 */
xmlParserInput *
xmlNewInputStream(xmlParserCtxt *ctxt) {
    xmlParserInputPtr input;

    input = (xmlParserInputPtr) xmlMalloc(sizeof(xmlParserInput));
    if (input == NULL) {
        xmlCtxtErrMemory(ctxt);
	return(NULL);
    }
    memset(input, 0, sizeof(xmlParserInput));
    input->line = 1;
    input->col = 1;

    return(input);
}

/**
 * Creates a new parser input from the filesystem, the network or
 * a user-defined resource loader.
 *
 * @param ctxt  parser context
 * @param url  filename or URL
 * @param publicId  publid ID from doctype (optional)
 * @param encoding  character encoding (optional)
 * @param flags  unused, pass 0
 * @returns a new parser input.
 */
xmlParserInput *
xmlCtxtNewInputFromUrl(xmlParserCtxt *ctxt, const char *url,
                       const char *publicId, const char *encoding,
                       xmlParserInputFlags flags ATTRIBUTE_UNUSED) {
    xmlParserInputPtr input;

    if ((ctxt == NULL) || (url == NULL))
	return(NULL);

    input = xmlLoadResource(ctxt, url, publicId, XML_RESOURCE_MAIN_DOCUMENT);
    if (input == NULL)
        return(NULL);

    if (encoding != NULL)
        xmlSwitchInputEncodingName(ctxt, input, encoding);

    return(input);
}

/**
 * Internal helper function.
 *
 * @param buf  parser input buffer
 * @param filename  filename or URL
 * @returns a new parser input.
 */
static xmlParserInputPtr
xmlNewInputInternal(xmlParserInputBufferPtr buf, const char *filename) {
    xmlParserInputPtr input;

    input = (xmlParserInputPtr) xmlMalloc(sizeof(xmlParserInput));
    if (input == NULL) {
	xmlFreeParserInputBuffer(buf);
	return(NULL);
    }
    memset(input, 0, sizeof(xmlParserInput));
    input->line = 1;
    input->col = 1;

    input->buf = buf;
    xmlBufResetInput(input->buf->buffer, input);

    if (filename != NULL) {
        input->filename = xmlMemStrdup(filename);
        if (input->filename == NULL) {
            xmlFreeInputStream(input);
            return(NULL);
        }
    }

    return(input);
}

/**
 * Creates a new parser input to read from a memory area.
 *
 * `url` is used as base to resolve external entities and for
 * error reporting.
 *
 * If the XML_INPUT_BUF_STATIC flag is set, the memory area must
 * stay unchanged until parsing has finished. This can avoid
 * temporary copies.
 *
 * If the XML_INPUT_BUF_ZERO_TERMINATED flag is set, the memory
 * area must contain a zero byte after the buffer at position `size`.
 * This can avoid temporary copies.
 *
 * @since 2.14.0
 *
 * @param url  base URL (optional)
 * @param mem  pointer to char array
 * @param size  size of array
 * @param flags  optimization hints
 * @returns a new parser input or NULL if a memory allocation failed.
 */
xmlParserInput *
xmlNewInputFromMemory(const char *url, const void *mem, size_t size,
                      xmlParserInputFlags flags) {
    xmlParserInputBufferPtr buf;

    if (mem == NULL)
	return(NULL);

    buf = xmlNewInputBufferMemory(mem, size, flags, XML_CHAR_ENCODING_NONE);
    if (buf == NULL)
        return(NULL);

    return(xmlNewInputInternal(buf, url));
}

/**
 * @param ctxt  parser context
 * @param url  base URL (optional)
 * @param mem  pointer to char array
 * @param size  size of array
 * @param encoding  character encoding (optional)
 * @param flags  optimization hints
 * @returns a new parser input or NULL in case of error.
 */
xmlParserInput *
xmlCtxtNewInputFromMemory(xmlParserCtxt *ctxt, const char *url,
                          const void *mem, size_t size,
                          const char *encoding, xmlParserInputFlags flags) {
    xmlParserInputPtr input;

    if ((ctxt == NULL) || (mem == NULL))
	return(NULL);

    input = xmlNewInputFromMemory(url, mem, size, flags);
    if (input == NULL) {
        xmlCtxtErrMemory(ctxt);
        return(NULL);
    }

    if (encoding != NULL)
        xmlSwitchInputEncodingName(ctxt, input, encoding);

    return(input);
}

/**
 * Creates a new parser input to read from a zero-terminated string.
 *
 * `url` is used as base to resolve external entities and for
 * error reporting.
 *
 * If the XML_INPUT_BUF_STATIC flag is set, the string must
 * stay unchanged until parsing has finished. This can avoid
 * temporary copies.
 *
 * @since 2.14.0
 *
 * @param url  base URL (optional)
 * @param str  zero-terminated string
 * @param flags  optimization hints
 * @returns a new parser input or NULL if a memory allocation failed.
 */
xmlParserInput *
xmlNewInputFromString(const char *url, const char *str,
                      xmlParserInputFlags flags) {
    xmlParserInputBufferPtr buf;

    if (str == NULL)
	return(NULL);

    buf = xmlNewInputBufferString(str, flags);
    if (buf == NULL)
        return(NULL);

    return(xmlNewInputInternal(buf, url));
}

/**
 * @param ctxt  parser context
 * @param url  base URL (optional)
 * @param str  zero-terminated string
 * @param encoding  character encoding (optional)
 * @param flags  optimization hints
 * @returns a new parser input.
 */
xmlParserInput *
xmlCtxtNewInputFromString(xmlParserCtxt *ctxt, const char *url,
                          const char *str, const char *encoding,
                          xmlParserInputFlags flags) {
    xmlParserInputPtr input;

    if ((ctxt == NULL) || (str == NULL))
	return(NULL);

    input = xmlNewInputFromString(url, str, flags);
    if (input == NULL) {
        xmlCtxtErrMemory(ctxt);
        return(NULL);
    }

    if (encoding != NULL)
        xmlSwitchInputEncodingName(ctxt, input, encoding);

    return(input);
}

/**
 * Creates a new parser input to read from a file descriptor.
 *
 * `url` is used as base to resolve external entities and for
 * error reporting.
 *
 * `fd` is closed after parsing has finished.
 *
 * Supported `flags` are XML_INPUT_UNZIP to decompress data
 * automatically. This feature is deprecated and will be removed
 * in a future release.
 *
 * @since 2.14.0
 *
 * @param url  base URL (optional)
 * @param fd  file descriptor
 * @param flags  input flags
 * @returns a new parser input or NULL if a memory allocation failed.
 */
xmlParserInput *
xmlNewInputFromFd(const char *url, int fd, xmlParserInputFlags flags) {
    xmlParserInputBufferPtr buf;

    if (fd < 0)
	return(NULL);

    buf = xmlAllocParserInputBuffer(XML_CHAR_ENCODING_NONE);
    if (buf == NULL)
        return(NULL);

    if (xmlInputFromFd(buf, fd, flags) != XML_ERR_OK) {
        xmlFreeParserInputBuffer(buf);
        return(NULL);
    }

    return(xmlNewInputInternal(buf, url));
}

/**
 * @param ctxt  parser context
 * @param url  base URL (optional)
 * @param fd  file descriptor
 * @param encoding  character encoding (optional)
 * @param flags  unused, pass 0
 * @returns a new parser input.
 */
xmlParserInput *
xmlCtxtNewInputFromFd(xmlParserCtxt *ctxt, const char *url,
                      int fd, const char *encoding,
                      xmlParserInputFlags flags) {
    xmlParserInputPtr input;

    if ((ctxt == NULL) || (fd < 0))
	return(NULL);

    if (ctxt->options & XML_PARSE_UNZIP)
        flags |= XML_INPUT_UNZIP;

    input = xmlNewInputFromFd(url, fd, flags);
    if (input == NULL) {
	xmlCtxtErrMemory(ctxt);
        return(NULL);
    }

    if (encoding != NULL)
        xmlSwitchInputEncodingName(ctxt, input, encoding);

    return(input);
}

/**
 * Creates a new parser input to read from input callbacks and
 * context.
 *
 * `url` is used as base to resolve external entities and for
 * error reporting.
 *
 * `ioRead` is called to read new data into a provided buffer.
 * It must return the number of bytes written into the buffer
 * ot a negative xmlParserErrors code on failure.
 *
 * `ioClose` is called after parsing has finished.
 *
 * `ioCtxt` is an opaque pointer passed to the callbacks.
 *
 * @since 2.14.0
 *
 * @param url  base URL (optional)
 * @param ioRead  read callback
 * @param ioClose  close callback (optional)
 * @param ioCtxt  IO context
 * @param flags  unused, pass 0
 * @returns a new parser input or NULL if a memory allocation failed.
 */
xmlParserInput *
xmlNewInputFromIO(const char *url, xmlInputReadCallback ioRead,
                  xmlInputCloseCallback ioClose, void *ioCtxt,
                  xmlParserInputFlags flags ATTRIBUTE_UNUSED) {
    xmlParserInputBufferPtr buf;

    if (ioRead == NULL)
	return(NULL);

    buf = xmlAllocParserInputBuffer(XML_CHAR_ENCODING_NONE);
    if (buf == NULL) {
        if (ioClose != NULL)
            ioClose(ioCtxt);
        return(NULL);
    }

    buf->context = ioCtxt;
    buf->readcallback = ioRead;
    buf->closecallback = ioClose;

    return(xmlNewInputInternal(buf, url));
}

/**
 * @param ctxt  parser context
 * @param url  base URL (optional)
 * @param ioRead  read callback
 * @param ioClose  close callback (optional)
 * @param ioCtxt  IO context
 * @param encoding  character encoding (optional)
 * @param flags  unused, pass 0
 * @returns a new parser input.
 */
xmlParserInput *
xmlCtxtNewInputFromIO(xmlParserCtxt *ctxt, const char *url,
                      xmlInputReadCallback ioRead,
                      xmlInputCloseCallback ioClose,
                      void *ioCtxt, const char *encoding,
                      xmlParserInputFlags flags) {
    xmlParserInputPtr input;

    if ((ctxt == NULL) || (ioRead == NULL))
	return(NULL);

    input = xmlNewInputFromIO(url, ioRead, ioClose, ioCtxt, flags);
    if (input == NULL) {
        xmlCtxtErrMemory(ctxt);
        return(NULL);
    }

    if (encoding != NULL)
        xmlSwitchInputEncodingName(ctxt, input, encoding);

    return(input);
}

/**
 * Creates a new parser input for a push parser.
 *
 * @param url  base URL (optional)
 * @param chunk  pointer to char array
 * @param size  size of array
 * @returns a new parser input or NULL if a memory allocation failed.
 */
xmlParserInput *
xmlNewPushInput(const char *url, const char *chunk, int size) {
    xmlParserInputBufferPtr buf;
    xmlParserInputPtr input;

    buf = xmlAllocParserInputBuffer(XML_CHAR_ENCODING_NONE);
    if (buf == NULL)
        return(NULL);

    input = xmlNewInputInternal(buf, url);
    if (input == NULL)
	return(NULL);

    input->flags |= XML_INPUT_PROGRESSIVE;

    if ((size > 0) && (chunk != NULL)) {
        int res;

	res = xmlParserInputBufferPush(input->buf, size, chunk);
        xmlBufResetInput(input->buf->buffer, input);
        if (res < 0) {
            xmlFreeInputStream(input);
            return(NULL);
        }
    }

    return(input);
}

/**
 * Create a new input stream structure encapsulating the `input` into
 * a stream suitable for the parser.
 *
 * @param ctxt  an XML parser context
 * @param buf  an input buffer
 * @param enc  the charset encoding if known
 * @returns the new input stream or NULL
 */
xmlParserInput *
xmlNewIOInputStream(xmlParserCtxt *ctxt, xmlParserInputBuffer *buf,
	            xmlCharEncoding enc) {
    xmlParserInputPtr input;
    const char *encoding;

    if ((ctxt == NULL) || (buf == NULL))
        return(NULL);

    input = xmlNewInputInternal(buf, NULL);
    if (input == NULL) {
        xmlCtxtErrMemory(ctxt);
	return(NULL);
    }

    encoding = xmlGetCharEncodingName(enc);
    if (encoding != NULL)
        xmlSwitchInputEncodingName(ctxt, input, encoding);

    return(input);
}

/**
 * Create a new input stream based on an xmlEntity
 *
 * @deprecated Internal function, do not use.
 *
 * @param ctxt  an XML parser context
 * @param ent  an Entity pointer
 * @returns the new input stream or NULL
 */
xmlParserInput *
xmlNewEntityInputStream(xmlParserCtxt *ctxt, xmlEntity *ent) {
    xmlParserInputPtr input;

    if ((ctxt == NULL) || (ent == NULL))
	return(NULL);

    if (ent->content != NULL) {
        input = xmlCtxtNewInputFromString(ctxt, NULL,
                (const char *) ent->content, NULL, XML_INPUT_BUF_STATIC);
    } else if (ent->URI != NULL) {
        xmlResourceType rtype;

        if (ent->etype == XML_EXTERNAL_PARAMETER_ENTITY)
            rtype = XML_RESOURCE_PARAMETER_ENTITY;
        else
            rtype = XML_RESOURCE_GENERAL_ENTITY;

        input = xmlLoadResource(ctxt, (char *) ent->URI,
                                (char *) ent->ExternalID, rtype);
    } else {
        return(NULL);
    }

    if (input == NULL)
        return(NULL);

    input->entity = ent;

    return(input);
}

/**
 * Create a new input stream based on a memory buffer.
 *
 * @deprecated Use #xmlNewInputFromString.
 *
 * @param ctxt  an XML parser context
 * @param buffer  an memory buffer
 * @returns the new input stream
 */
xmlParserInput *
xmlNewStringInputStream(xmlParserCtxt *ctxt, const xmlChar *buffer) {
    return(xmlCtxtNewInputFromString(ctxt, NULL, (const char *) buffer,
                                     NULL, 0));
}


/****************************************************************
 *								*
 *		External entities loading			*
 *								*
 ****************************************************************/

#ifdef LIBXML_CATALOG_ENABLED

/**
 * Resolves an external ID or URL against the appropriate catalog.
 *
 * @param url  the URL or system ID for the entity to load
 * @param publicId  the public ID for the entity to load (optional)
 * @param localCatalogs  local catalogs (optional)
 * @param allowGlobal  allow global system catalog
 * @param out  resulting resource or NULL
 * @returns an xmlParserErrors code
 */
static xmlParserErrors
xmlResolveFromCatalog(const char *url, const char *publicId,
                      void *localCatalogs, int allowGlobal, char **out) {
    xmlError oldError;
    xmlError *lastError;
    char *resource = NULL;
    xmlParserErrors code;

    if (out == NULL)
        return(XML_ERR_ARGUMENT);
    *out = NULL;
    if ((localCatalogs == NULL) && (!allowGlobal))
        return(XML_ERR_OK);

    /*
     * Don't try to resolve if local file exists.
     *
     * TODO: This is somewhat non-deterministic.
     */
    if (xmlNoNetExists(url))
        return(XML_ERR_OK);

    /* Backup and reset last error */
    lastError = xmlGetLastErrorInternal();
    oldError = *lastError;
    lastError->code = XML_ERR_OK;

    /*
     * Do a local lookup
     */
    if (localCatalogs != NULL) {
        resource = (char *) xmlCatalogLocalResolve(localCatalogs,
                                                   BAD_CAST publicId,
                                                   BAD_CAST url);
    }
    /*
     * Try a global lookup
     */
    if ((resource == NULL) && (allowGlobal)) {
        resource = (char *) xmlCatalogResolve(BAD_CAST publicId,
                                              BAD_CAST url);
    }

    /*
     * Try to resolve url using URI rules.
     *
     * TODO: We should consider using only a single resolution
     * mechanism depending on resource type. Either by external ID
     * or by URI.
     */
    if ((resource == NULL) && (url != NULL)) {
        if (localCatalogs != NULL) {
            resource = (char *) xmlCatalogLocalResolveURI(localCatalogs,
                                                          BAD_CAST url);
        }
        if ((resource == NULL) && (allowGlobal)) {
            resource = (char *) xmlCatalogResolveURI(BAD_CAST url);
        }
    }

    code = lastError->code;
    if (code == XML_ERR_OK) {
        *out = resource;
    } else {
        xmlFree(resource);
    }

    *lastError = oldError;

    return(code);
}

static char *
xmlCtxtResolveFromCatalog(xmlParserCtxtPtr ctxt, const char *url,
                          const char *publicId) {
    char *resource;
    void *localCatalogs = NULL;
    int allowGlobal = 1;
    xmlParserErrors code;

    if (ctxt != NULL) {
        /*
         * Loading of HTML documents shouldn't use XML catalogs.
         */
        if (ctxt->html)
            return(NULL);

        localCatalogs = ctxt->catalogs;

        if (ctxt->options & XML_PARSE_NO_SYS_CATALOG)
            allowGlobal = 0;
    }

    switch (xmlCatalogGetDefaults()) {
        case XML_CATA_ALLOW_NONE:
            return(NULL);
        case XML_CATA_ALLOW_DOCUMENT:
            allowGlobal = 0;
            break;
        case XML_CATA_ALLOW_GLOBAL:
            localCatalogs = NULL;
            break;
        case XML_CATA_ALLOW_ALL:
            break;
    }

    code = xmlResolveFromCatalog(url, publicId, localCatalogs,
                                 allowGlobal, &resource);
    if (code != XML_ERR_OK)
        xmlCtxtErr(ctxt, NULL, XML_FROM_CATALOG, code, XML_ERR_ERROR,
                   BAD_CAST url, BAD_CAST publicId, NULL, 0,
                   "%s\n", xmlErrString(code), NULL);

    return(resource);
}

#endif

/**
 * @deprecated Internal function, don't use.
 *
 * @param ctxt  an XML parser context
 * @param ret  an XML parser input
 * @returns NULL.
 */
xmlParserInput *
xmlCheckHTTPInput(xmlParserCtxt *ctxt ATTRIBUTE_UNUSED,
                  xmlParserInput *ret ATTRIBUTE_UNUSED) {
    return(NULL);
}

/**
 * Create a new input stream based on a file or a URL.
 *
 * The flag XML_INPUT_UNZIP allows decompression.
 *
 * The flag XML_INPUT_NETWORK allows network access.
 *
 * The following resource loaders will be called if they were
 * registered (in order of precedence):
 *
 * - the per-thread #xmlParserInputBufferCreateFilenameFunc set with
 *   #xmlParserInputBufferCreateFilenameDefault (deprecated)
 * - the default loader which will return
 *   - the result from a matching global input callback set with
 *     #xmlRegisterInputCallbacks (deprecated)
 *   - a file opened from the filesystem, with automatic detection
 *     of compressed files if support is compiled in.
 *
 * @since 2.14.0
 *
 * @param url  the filename to use as entity
 * @param flags  XML_INPUT flags
 * @param out  pointer to new parser input
 * @returns an xmlParserErrors code.
 */
xmlParserErrors
xmlNewInputFromUrl(const char *url, xmlParserInputFlags flags,
                   xmlParserInput **out) {
    char *resource = NULL;
    xmlParserInputBufferPtr buf;
    xmlParserInputPtr input;
    xmlParserErrors code = XML_ERR_OK;

    if (out == NULL)
        return(XML_ERR_ARGUMENT);
    *out = NULL;
    if (url == NULL)
        return(XML_ERR_ARGUMENT);

#ifdef LIBXML_CATALOG_ENABLED
    if (flags & XML_INPUT_USE_SYS_CATALOG) {
        code = xmlResolveFromCatalog(url, NULL, NULL, 1, &resource);
        if (code != XML_ERR_OK)
            return(code);
        if (resource != NULL)
            url = resource;
    }
#endif

    if (xmlParserInputBufferCreateFilenameValue != NULL) {
        buf = xmlParserInputBufferCreateFilenameValue(url,
                XML_CHAR_ENCODING_NONE);
        if (buf == NULL)
            code = XML_IO_ENOENT;
    } else {
        code = xmlParserInputBufferCreateUrl(url, XML_CHAR_ENCODING_NONE,
                                             flags, &buf);
    }

    if (code == XML_ERR_OK) {
        input = xmlNewInputInternal(buf, url);
        if (input == NULL)
            code = XML_ERR_NO_MEMORY;

        *out = input;
    }

    if (resource != NULL)
        xmlFree(resource);
    return(code);
}

/**
 * Create a new input stream based on a file or an URL.
 *
 * Unlike the default external entity loader, this function
 * doesn't use XML catalogs.
 *
 * @deprecated Use #xmlNewInputFromUrl.
 *
 * @param ctxt  an XML parser context
 * @param filename  the filename to use as entity
 * @returns the new input stream or NULL in case of error
 */
xmlParserInput *
xmlNewInputFromFile(xmlParserCtxt *ctxt, const char *filename) {
    xmlParserInputPtr input;
    xmlParserInputFlags flags = 0;
    xmlParserErrors code;

    if ((ctxt == NULL) || (filename == NULL))
        return(NULL);

    if (ctxt->options & XML_PARSE_UNZIP)
        flags |= XML_INPUT_UNZIP;
    if ((ctxt->options & XML_PARSE_NONET) == 0)
        flags |= XML_INPUT_NETWORK;

    code = xmlNewInputFromUrl(filename, flags, &input);
    if (code != XML_ERR_OK) {
        xmlCtxtErrIO(ctxt, code, filename);
        return(NULL);
    }

    return(input);
}

/**
 * By default we don't load external entities, yet.
 *
 * @param url  the URL or system ID for the entity to load
 * @param publicId  the public ID for the entity to load (optional)
 * @param ctxt  the context in which the entity is called or NULL
 * @returns a new allocated xmlParserInput, or NULL.
 */
static xmlParserInputPtr
xmlDefaultExternalEntityLoader(const char *url, const char *publicId,
                               xmlParserCtxtPtr ctxt)
{
    xmlParserInputPtr input = NULL;
    char *resource = NULL;

    (void) publicId;

    if (url == NULL)
        return(NULL);

#ifdef LIBXML_CATALOG_ENABLED
    resource = xmlCtxtResolveFromCatalog(ctxt, url, publicId);
    if (resource != NULL)
	url = resource;
#endif

    /*
     * Several downstream test suites expect this error whenever
     * an http URI is passed and NONET is set.
     */
    if ((ctxt != NULL) &&
        (ctxt->options & XML_PARSE_NONET) &&
        (xmlStrncasecmp(BAD_CAST url, BAD_CAST "http://", 7) == 0)) {
        xmlCtxtErrIO(ctxt, XML_IO_NETWORK_ATTEMPT, url);
    } else {
        input = xmlNewInputFromFile(ctxt, url);
    }

    if (resource != NULL)
	xmlFree(resource);
    return(input);
}

/**
 * A specific entity loader disabling network accesses, though still
 * allowing local catalog accesses for resolution.
 *
 * @deprecated Use XML_PARSE_NONET.
 *
 * @param URL  the URL or system ID for the entity to load
 * @param publicId  the public ID for the entity to load
 * @param ctxt  the context in which the entity is called or NULL
 * @returns a new allocated xmlParserInput, or NULL.
 */
xmlParserInput *
xmlNoNetExternalEntityLoader(const char *URL, const char *publicId,
                             xmlParserCtxt *ctxt) {
    int oldOptions = 0;
    xmlParserInputPtr input;

    if (ctxt != NULL) {
        oldOptions = ctxt->options;
        ctxt->options |= XML_PARSE_NONET;
    }

    input = xmlDefaultExternalEntityLoader(URL, publicId, ctxt);

    if (ctxt != NULL)
        ctxt->options = oldOptions;

    return(input);
}

/*
 * This global has to die eventually
 */
static xmlExternalEntityLoader
xmlCurrentExternalEntityLoader = xmlDefaultExternalEntityLoader;

/**
 * Changes the default external entity resolver function for the
 * application.
 *
 * @deprecated This is a global setting and not thread-safe. Use
 * #xmlCtxtSetResourceLoader or similar functions.
 *
 * @param f  the new entity resolver function
 */
void
xmlSetExternalEntityLoader(xmlExternalEntityLoader f) {
    xmlCurrentExternalEntityLoader = f;
}

/**
 * Get the default external entity resolver function for the application
 *
 * @deprecated See #xmlSetExternalEntityLoader.
 *
 * @returns the #xmlExternalEntityLoader function pointer
 */
xmlExternalEntityLoader
xmlGetExternalEntityLoader(void) {
    return(xmlCurrentExternalEntityLoader);
}

/**
 * Installs a custom callback to load documents, DTDs or external
 * entities.
 *
 * If `vctxt` is NULL, the parser context will be passed.
 *
 * @since 2.14.0
 * @param ctxt  parser context
 * @param loader  callback
 * @param vctxt  user data (optional)
 */
void
xmlCtxtSetResourceLoader(xmlParserCtxt *ctxt, xmlResourceLoader loader,
                         void *vctxt) {
    if (ctxt == NULL)
        return;

    ctxt->resourceLoader = loader;
    ctxt->resourceCtxt = vctxt;
}

/**
 * @param ctxt  parser context
 * @param url  the URL or system ID for the entity to load
 * @param publicId  the public ID for the entity to load (optional)
 * @param type  resource type
 * @returns the xmlParserInput or NULL in case of error.
 */
xmlParserInput *
xmlLoadResource(xmlParserCtxt *ctxt, const char *url, const char *publicId,
                xmlResourceType type) {
    char *canonicFilename;
    xmlParserInputPtr ret;

    if (url == NULL)
        return(NULL);

    if ((ctxt != NULL) && (ctxt->resourceLoader != NULL)) {
        char *resource = NULL;
        void *userData;
        xmlParserInputFlags flags = 0;
        int code;

#ifdef LIBXML_CATALOG_ENABLED
        resource = xmlCtxtResolveFromCatalog(ctxt, url, publicId);
        if (resource != NULL)
            url = resource;
#endif

        if (ctxt->options & XML_PARSE_UNZIP)
            flags |= XML_INPUT_UNZIP;
        if ((ctxt->options & XML_PARSE_NONET) == 0)
            flags |= XML_INPUT_NETWORK;

        userData = ctxt->resourceCtxt;
        if (userData == NULL)
            userData = ctxt;

        code = ctxt->resourceLoader(userData, url, publicId, type,
                                    flags, &ret);
        if (code != XML_ERR_OK) {
            xmlCtxtErrIO(ctxt, code, url);
            ret = NULL;
        }
        if (resource != NULL)
            xmlFree(resource);
        return(ret);
    }

    canonicFilename = (char *) xmlCanonicPath((const xmlChar *) url);
    if (canonicFilename == NULL) {
        xmlCtxtErrMemory(ctxt);
        return(NULL);
    }

    ret = xmlCurrentExternalEntityLoader(canonicFilename, publicId, ctxt);
    xmlFree(canonicFilename);
    return(ret);
}

/**
 * `URL` is a filename or URL. If if contains the substring "://",
 * it is assumed to be a Legacy Extended IRI. Otherwise, it is
 * treated as a filesystem path.
 *
 * `publicId` is an optional XML public ID, typically from a doctype
 * declaration. It is used for catalog lookups.
 *
 * If catalog lookup is enabled (default is yes) and URL or ID are
 * found in system or local XML catalogs, URL is replaced with the
 * result. Then the following resource loaders will be called if
 * they were registered (in order of precedence):
 *
 * - the resource loader set with #xmlCtxtSetResourceLoader
 * - the global external entity loader set with
 *   #xmlSetExternalEntityLoader (without catalog resolution,
 *   deprecated)
 * - the per-thread #xmlParserInputBufferCreateFilenameFunc set with
 *   #xmlParserInputBufferCreateFilenameDefault (deprecated)
 * - the default loader which will return
 *   - the result from a matching global input callback set with
 *     #xmlRegisterInputCallbacks (deprecated)
 *   - a file opened from the filesystem, with automatic detection
 *     of compressed files if support is compiled in.
 *
 * @param URL  the URL or system ID for the entity to load
 * @param publicId  the public ID for the entity to load (optional)
 * @param ctxt  the context in which the entity is called or NULL
 * @returns the xmlParserInput or NULL
 */
xmlParserInput *
xmlLoadExternalEntity(const char *URL, const char *publicId,
                      xmlParserCtxt *ctxt) {
    return(xmlLoadResource(ctxt, URL, publicId, XML_RESOURCE_UNKNOWN));
}

/************************************************************************
 *									*
 *		Commodity functions to handle parser contexts		*
 *									*
 ************************************************************************/

/**
 * Initialize a SAX parser context
 *
 * @param ctxt  XML parser context
 * @param sax  SAX handlert
 * @param userData  user data
 * @returns 0 in case of success and -1 in case of error
 */

static int
xmlInitSAXParserCtxt(xmlParserCtxtPtr ctxt, const xmlSAXHandler *sax,
                     void *userData)
{
    xmlParserInputPtr input;
#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
    size_t initialNodeTabSize = 1;
#else
    size_t initialNodeTabSize = 10;
#endif

    if (ctxt == NULL)
        return(-1);

    if (ctxt->dict == NULL)
	ctxt->dict = xmlDictCreate();
    if (ctxt->dict == NULL)
	return(-1);

    if (ctxt->sax == NULL)
	ctxt->sax = (xmlSAXHandler *) xmlMalloc(sizeof(xmlSAXHandler));
    if (ctxt->sax == NULL)
	return(-1);
    if (sax == NULL) {
	memset(ctxt->sax, 0, sizeof(xmlSAXHandler));
        xmlSAXVersion(ctxt->sax, 2);
        ctxt->userData = ctxt;
    } else {
	if (sax->initialized == XML_SAX2_MAGIC) {
	    memcpy(ctxt->sax, sax, sizeof(xmlSAXHandler));
        } else {
	    memset(ctxt->sax, 0, sizeof(xmlSAXHandler));
	    memcpy(ctxt->sax, sax, sizeof(xmlSAXHandlerV1));
        }
        ctxt->userData = userData ? userData : ctxt;
    }

    ctxt->maxatts = 0;
    ctxt->atts = NULL;
    /* Allocate the Input stack */
    if (ctxt->inputTab == NULL) {
#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
        size_t initialSize = 1;
#else
        size_t initialSize = 5;
#endif

	ctxt->inputTab = xmlMalloc(initialSize * sizeof(xmlParserInputPtr));
	ctxt->inputMax = initialSize;
    }
    if (ctxt->inputTab == NULL)
	return(-1);
    while ((input = xmlCtxtPopInput(ctxt)) != NULL) { /* Non consuming */
        xmlFreeInputStream(input);
    }
    ctxt->inputNr = 0;
    ctxt->input = NULL;

    ctxt->version = NULL;
    ctxt->encoding = NULL;
    ctxt->standalone = -1;
    ctxt->hasExternalSubset = 0;
    ctxt->hasPErefs = 0;
    ctxt->html = 0;
    ctxt->instate = XML_PARSER_START;

    /* Allocate the Node stack */
    if (ctxt->nodeTab == NULL) {
	ctxt->nodeTab = xmlMalloc(initialNodeTabSize * sizeof(xmlNodePtr));
	ctxt->nodeMax = initialNodeTabSize;
    }
    if (ctxt->nodeTab == NULL)
	return(-1);
    ctxt->nodeNr = 0;
    ctxt->node = NULL;

    /* Allocate the Name stack */
    if (ctxt->nameTab == NULL) {
	ctxt->nameTab = xmlMalloc(initialNodeTabSize * sizeof(xmlChar *));
	ctxt->nameMax = initialNodeTabSize;
    }
    if (ctxt->nameTab == NULL)
	return(-1);
    ctxt->nameNr = 0;
    ctxt->name = NULL;

    /* Allocate the space stack */
    if (ctxt->spaceTab == NULL) {
	ctxt->spaceTab = xmlMalloc(initialNodeTabSize * sizeof(int));
	ctxt->spaceMax = initialNodeTabSize;
    }
    if (ctxt->spaceTab == NULL)
	return(-1);
    ctxt->spaceNr = 1;
    ctxt->spaceTab[0] = -1;
    ctxt->space = &ctxt->spaceTab[0];
    ctxt->myDoc = NULL;
    ctxt->wellFormed = 1;
    ctxt->nsWellFormed = 1;
    ctxt->valid = 1;

    ctxt->options = XML_PARSE_NODICT;

    /*
     * Initialize some parser options from deprecated global variables.
     * Note that the "modern" API taking options arguments or
     * xmlCtxtSetOptions will ignore these defaults. They're only
     * relevant if old API functions like xmlParseFile are used.
     */
    ctxt->loadsubset = xmlLoadExtDtdDefaultValue;
    if (ctxt->loadsubset) {
        ctxt->options |= XML_PARSE_DTDLOAD;
    }
    ctxt->validate = xmlDoValidityCheckingDefaultValue;
    if (ctxt->validate) {
        ctxt->options |= XML_PARSE_DTDVALID;
    }
    ctxt->pedantic = xmlPedanticParserDefaultValue;
    if (ctxt->pedantic) {
        ctxt->options |= XML_PARSE_PEDANTIC;
    }
    ctxt->keepBlanks = xmlKeepBlanksDefaultValue;
    if (ctxt->keepBlanks == 0) {
	ctxt->sax->ignorableWhitespace = xmlSAX2IgnorableWhitespace;
	ctxt->options |= XML_PARSE_NOBLANKS;
    }
    ctxt->replaceEntities = xmlSubstituteEntitiesDefaultValue;
    if (ctxt->replaceEntities) {
        ctxt->options |= XML_PARSE_NOENT;
    }
    if (xmlGetWarningsDefaultValue == 0)
        ctxt->options |= XML_PARSE_NOWARNING;

    ctxt->vctxt.flags = XML_VCTXT_USE_PCTXT;
    ctxt->vctxt.userData = ctxt;
    ctxt->vctxt.error = xmlParserValidityError;
    ctxt->vctxt.warning = xmlParserValidityWarning;

    ctxt->record_info = 0;
    ctxt->checkIndex = 0;
    ctxt->inSubset = 0;
    ctxt->errNo = XML_ERR_OK;
    ctxt->depth = 0;
    ctxt->catalogs = NULL;
    ctxt->sizeentities = 0;
    ctxt->sizeentcopy = 0;
    ctxt->input_id = 1;
    ctxt->maxAmpl = XML_MAX_AMPLIFICATION_DEFAULT;
    xmlInitNodeInfoSeq(&ctxt->node_seq);

    if (ctxt->nsdb == NULL) {
        ctxt->nsdb = xmlParserNsCreate();
        if (ctxt->nsdb == NULL)
            return(-1);
    }

    return(0);
}

/**
 * Initialize a parser context
 *
 * @deprecated Internal function which will be made private in a future
 * version.
 *
 * @param ctxt  an XML parser context
 * @returns 0 in case of success and -1 in case of error
 */

int
xmlInitParserCtxt(xmlParserCtxt *ctxt)
{
    return(xmlInitSAXParserCtxt(ctxt, NULL, NULL));
}

/**
 * Free all the memory used by a parser context. However the parsed
 * document in ctxt->myDoc is not freed.
 *
 * @param ctxt  an XML parser context
 */

void
xmlFreeParserCtxt(xmlParserCtxt *ctxt)
{
    xmlParserInputPtr input;

    if (ctxt == NULL) return;

    while ((input = xmlCtxtPopInput(ctxt)) != NULL) { /* Non consuming */
        xmlFreeInputStream(input);
    }
    if (ctxt->spaceTab != NULL) xmlFree(ctxt->spaceTab);
    if (ctxt->nameTab != NULL) xmlFree((xmlChar * *)ctxt->nameTab);
    if (ctxt->nodeTab != NULL) xmlFree(ctxt->nodeTab);
    if (ctxt->nodeInfoTab != NULL) xmlFree(ctxt->nodeInfoTab);
    if (ctxt->inputTab != NULL) xmlFree(ctxt->inputTab);
    if (ctxt->version != NULL) xmlFree(ctxt->version);
    if (ctxt->encoding != NULL) xmlFree(ctxt->encoding);
    if (ctxt->extSubURI != NULL) xmlFree(ctxt->extSubURI);
    if (ctxt->extSubSystem != NULL) xmlFree(ctxt->extSubSystem);
#ifdef LIBXML_SAX1_ENABLED
    if ((ctxt->sax != NULL) &&
        (ctxt->sax != (xmlSAXHandlerPtr) &xmlDefaultSAXHandler))
#else
    if (ctxt->sax != NULL)
#endif /* LIBXML_SAX1_ENABLED */
        xmlFree(ctxt->sax);
    if (ctxt->directory != NULL) xmlFree(ctxt->directory);
    if (ctxt->vctxt.nodeTab != NULL) xmlFree(ctxt->vctxt.nodeTab);
    if (ctxt->atts != NULL) xmlFree((xmlChar * *)ctxt->atts);
    if (ctxt->dict != NULL) xmlDictFree(ctxt->dict);
    if (ctxt->nsTab != NULL) xmlFree(ctxt->nsTab);
    if (ctxt->nsdb != NULL) xmlParserNsFree(ctxt->nsdb);
    if (ctxt->attrHash != NULL) xmlFree(ctxt->attrHash);
    if (ctxt->pushTab != NULL) xmlFree(ctxt->pushTab);
    if (ctxt->attallocs != NULL) xmlFree(ctxt->attallocs);
    if (ctxt->attsDefault != NULL)
        xmlHashFree(ctxt->attsDefault, xmlHashDefaultDeallocator);
    if (ctxt->attsSpecial != NULL)
        xmlHashFree(ctxt->attsSpecial, NULL);
    if (ctxt->freeElems != NULL) {
        xmlNodePtr cur, next;

	cur = ctxt->freeElems;
	while (cur != NULL) {
	    next = cur->next;
	    xmlFree(cur);
	    cur = next;
	}
    }
    if (ctxt->freeAttrs != NULL) {
        xmlAttrPtr cur, next;

	cur = ctxt->freeAttrs;
	while (cur != NULL) {
	    next = cur->next;
	    xmlFree(cur);
	    cur = next;
	}
    }
    /*
     * cleanup the error strings
     */
    if (ctxt->lastError.message != NULL)
        xmlFree(ctxt->lastError.message);
    if (ctxt->lastError.file != NULL)
        xmlFree(ctxt->lastError.file);
    if (ctxt->lastError.str1 != NULL)
        xmlFree(ctxt->lastError.str1);
    if (ctxt->lastError.str2 != NULL)
        xmlFree(ctxt->lastError.str2);
    if (ctxt->lastError.str3 != NULL)
        xmlFree(ctxt->lastError.str3);

#ifdef LIBXML_CATALOG_ENABLED
    if (ctxt->catalogs != NULL)
	xmlCatalogFreeLocal(ctxt->catalogs);
#endif
    xmlFree(ctxt);
}

/**
 * Allocate and initialize a new parser context.
 *
 * @returns the xmlParserCtxt or NULL
 */

xmlParserCtxt *
xmlNewParserCtxt(void)
{
    return(xmlNewSAXParserCtxt(NULL, NULL));
}

/**
 * Allocate and initialize a new SAX parser context. If userData is NULL,
 * the parser context will be passed as user data.
 *
 * @since 2.11.0
 *
 * If you want support older versions,
 * it's best to invoke #xmlNewParserCtxt and set ctxt->sax with
 * struct assignment.
 *
 * @param sax  SAX handler
 * @param userData  user data
 * @returns the xmlParserCtxt or NULL if memory allocation failed.
 */

xmlParserCtxt *
xmlNewSAXParserCtxt(const xmlSAXHandler *sax, void *userData)
{
    xmlParserCtxtPtr ctxt;

    xmlInitParser();

    ctxt = (xmlParserCtxtPtr) xmlMalloc(sizeof(xmlParserCtxt));
    if (ctxt == NULL)
	return(NULL);
    memset(ctxt, 0, sizeof(xmlParserCtxt));
    if (xmlInitSAXParserCtxt(ctxt, sax, userData) < 0) {
        xmlFreeParserCtxt(ctxt);
	return(NULL);
    }
    return(ctxt);
}

/**
 * @since 2.14.0
 *
 * @param ctxt  parser context
 * @returns the private application data.
 */
void *
xmlCtxtGetPrivate(xmlParserCtxt *ctxt) {
    if (ctxt == NULL)
        return(NULL);

    return(ctxt->_private);
}

/**
 * Set the private application data.
 *
 * @since 2.14.0
 *
 * @param ctxt  parser context
 * @param priv  private application data
 */
void
xmlCtxtSetPrivate(xmlParserCtxt *ctxt, void *priv) {
    if (ctxt == NULL)
        return;

    ctxt->_private = priv;
}

/**
 * @since 2.14.0
 *
 * @param ctxt  parser context
 * @returns the local catalogs.
 */
void *
xmlCtxtGetCatalogs(xmlParserCtxt *ctxt) {
    if (ctxt == NULL)
        return(NULL);

    return(ctxt->catalogs);
}

/**
 * Set the local catalogs.
 *
 * @since 2.14.0
 *
 * @param ctxt  parser context
 * @param catalogs  catalogs pointer
 */
void
xmlCtxtSetCatalogs(xmlParserCtxt *ctxt, void *catalogs) {
    if (ctxt == NULL)
        return;

    ctxt->catalogs = catalogs;
}

/**
 * @since 2.14.0
 *
 * @param ctxt  parser context
 * @returns the dictionary.
 */
xmlDict *
xmlCtxtGetDict(xmlParserCtxt *ctxt) {
    if (ctxt == NULL)
        return(NULL);

    return(ctxt->dict);
}

/**
 * Set the dictionary. This should only be done immediately after
 * creating a parser context.
 *
 * @since 2.14.0
 *
 * @param ctxt  parser context
 * @param dict  dictionary
 */
void
xmlCtxtSetDict(xmlParserCtxt *ctxt, xmlDict *dict) {
    if (ctxt == NULL)
        return;

    if (ctxt->dict != NULL)
        xmlDictFree(ctxt->dict);

    xmlDictReference(dict);
    ctxt->dict = dict;
}

/**
 * @since 2.14.0
 *
 * @param ctxt  parser context
 * @returns the SAX handler struct. This is not a copy and must not
 * be freed. Handlers can be updated.
 */
xmlSAXHandler *
xmlCtxtGetSaxHandler(xmlParserCtxt *ctxt) {
    if (ctxt == NULL)
        return(NULL);

    return(ctxt->sax);
}

/**
 * Set the SAX handler struct to a copy of `sax`.
 *
 * @since 2.14.0
 *
 * @param ctxt  parser context
 * @param sax  SAX handler
 * @returns 0 on success or -1 if arguments are invalid or a memory
 * allocation failed.
 */
int
xmlCtxtSetSaxHandler(xmlParserCtxt *ctxt, const xmlSAXHandler *sax) {
    xmlSAXHandler *copy;

    if ((ctxt == NULL) || (sax == NULL))
        return(-1);

    copy = xmlMalloc(sizeof(*copy));
    if (copy == NULL)
        return(-1);

    memcpy(copy, sax, sizeof(*copy));
    ctxt->sax = copy;

    return(0);
}

/**
 * @since 2.14.0
 *
 * @param ctxt  parser context
 * @returns the parsed document or NULL if a fatal error occurred when
 * parsing. The document must be freed by the caller. Resets the
 * context's document to NULL.
 */
xmlDoc *
xmlCtxtGetDocument(xmlParserCtxt *ctxt) {
    xmlDocPtr doc;

    if (ctxt == NULL)
        return(NULL);

    if ((ctxt->wellFormed) ||
        (((ctxt->recovery) || (ctxt->html)) &&
         (!xmlCtxtIsCatastrophicError(ctxt)))) {
        doc = ctxt->myDoc;
    } else {
        if (ctxt->errNo == XML_ERR_OK)
            xmlFatalErr(ctxt, XML_ERR_INTERNAL_ERROR, "unknown error");
        doc = NULL;
        xmlFreeDoc(ctxt->myDoc);
    }
    ctxt->myDoc = NULL;

    return(doc);
}

/**
 * @since 2.14.0
 *
 * @param ctxt  parser context
 * @returns 1 if this is a HTML parser context, 0 otherwise.
 */
int
xmlCtxtIsHtml(xmlParserCtxt *ctxt) {
    if (ctxt == NULL)
        return(0);

    return(ctxt->html ? 1 : 0);
}

/**
 * Check whether the parser is stopped.
 *
 * The parser is stopped on fatal (non-wellformedness) errors or
 * on user request with #xmlStopParser.
 *
 * @since 2.14.0
 *
 * @param ctxt  parser context
 * @returns 1 if the parser is stopped, 0 otherwise.
 */
int
xmlCtxtIsStopped(xmlParserCtxt *ctxt) {
    if (ctxt == NULL)
        return(0);

    return(ctxt->disableSAX != 0);
}

/**
 * Check whether a DTD subset is being parsed.
 *
 * Should only be used by SAX callbacks.
 *
 * Return values are
 *
 * - 0: not in DTD
 * - 1: in internal DTD subset
 * - 2: in external DTD subset
 *
 * @since 2.15.0
 *
 * @param ctxt  parser context
 * @returns the subset status
 */
int
xmlCtxtIsInSubset(xmlParserCtxt *ctxt) {
    if (ctxt == NULL)
        return(0);

    return(ctxt->inSubset);
}

#ifdef LIBXML_VALID_ENABLED
/**
 * @since 2.14.0
 *
 * @param ctxt  parser context
 * @returns the validation context.
 */
xmlValidCtxt *
xmlCtxtGetValidCtxt(xmlParserCtxt *ctxt) {
    if (ctxt == NULL)
        return(NULL);

    return(&ctxt->vctxt);
}
#endif

/**
 * Return user data.
 *
 * Return user data of a custom SAX parser or the parser context
 * itself if unset.
 *
 * @since 2.15.0
 *
 * @param ctxt  parser context
 * @returns the user data.
 */
void *
xmlCtxtGetUserData(xmlParserCtxt *ctxt) {
    if (ctxt == NULL)
        return NULL;

    return ctxt->userData;
}

/**
 * Return the current node being parsed.
 *
 * This is only useful if the default SAX callbacks which build
 * a document tree are intercepted. This mode of operation is
 * fragile and discouraged.
 *
 * Returns the current element node, or the document node if no
 * element was parsed yet.
 *
 * @since 2.15.0
 *
 * @param ctxt  parser context
 * @returns the current node.
 */
xmlNode *
xmlCtxtGetNode(xmlParserCtxt *ctxt) {
    if (ctxt == NULL)
        return NULL;

    if (ctxt->node != NULL)
        return ctxt->node;
    return (xmlNode *) ctxt->myDoc;
}

/**
 * Return data from the doctype declaration.
 *
 * Should only be used by SAX callbacks.
 *
 * @since 2.15.0
 *
 * @param ctxt  parser context
 * @param name  name of the root element (output)
 * @param systemId  system ID (URI) of the external subset (output)
 * @param publicId  public ID of the external subset (output)
 * @returns 0 on success, -1 if argument is invalid
 */
int
xmlCtxtGetDocTypeDecl(xmlParserCtxt *ctxt,
                      const xmlChar **name,
                      const xmlChar **systemId,
                      const xmlChar **publicId) {
    if (ctxt == NULL)
        return -1;

    if (name != NULL)
        *name = ctxt->intSubName;
    if (systemId != NULL)
        *systemId = ctxt->extSubURI;
    if (publicId != NULL)
        *publicId = ctxt->extSubSystem; /* The member is misnamed */

    return 0;
}

/**
 * Return input position.
 *
 * Should only be used by error handlers or SAX callbacks.
 *
 * Because of entities, there can be multiple inputs. Non-negative
 * values of `inputIndex` (0, 1, 2, ...)  select inputs starting
 * from the outermost input. Negative values (-1, -2, ...) select
 * inputs starting from the innermost input.
 *
 * The byte position is counted in possibly decoded UTF-8 bytes,
 * so it won't match the position in the raw input data.
 *
 * @since 2.15.0
 *
 * @param ctxt  parser context
 * @param inputIndex  input index
 * @param filename  filename (output)
 * @param line  line number (output)
 * @param col  column number (output)
 * @param utf8BytePos  byte position (output)
 * @returns 0 on success, -1 if arguments are invalid
 */
int
xmlCtxtGetInputPosition(xmlParserCtxt *ctxt, int inputIndex,
                        const char **filename, int *line, int *col,
                        unsigned long *utf8BytePos) {
    xmlParserInput *input;

    if (ctxt == NULL)
        return -1;

    if (inputIndex < 0) {
        inputIndex += ctxt->inputNr;
        if (inputIndex < 0)
            return -1;
    }
    if (inputIndex >= ctxt->inputNr)
        return -1;

    input = ctxt->inputTab[inputIndex];

    if (filename != NULL)
        *filename = input->filename;
    if (line != NULL)
        *line = input->line;
    if (col != NULL)
        *col = input->col;

    if (utf8BytePos != NULL) {
        unsigned long consumed;

        consumed = input->consumed;
        xmlSaturatedAddSizeT(&consumed, input->cur - input->base);
        *utf8BytePos = consumed;
    }

    return 0;
}

/**
 * Return window into input data.
 *
 * Should only be used by error handlers or SAX callbacks.
 * The returned pointer is only valid until the callback returns.
 *
 * Because of entities, there can be multiple inputs. Non-negative
 * values of `inputIndex` (0, 1, 2, ...)  select inputs starting
 * from the outermost input. Negative values (-1, -2, ...) select
 * inputs starting from the innermost input.
 *
 * @since 2.15.0
 *
 * @param ctxt  parser context
 * @param inputIndex  input index
 * @param startOut  start of window (output)
 * @param sizeInOut  maximum size of window (in)
 *                   actual size of window (out)
 * @param offsetOut  offset of current position inside
 *                   window (out)
 * @returns 0 on success, -1 if arguments are invalid
 */
int
xmlCtxtGetInputWindow(xmlParserCtxt *ctxt, int inputIndex,
                      const xmlChar **startOut,
                      int *sizeInOut, int *offsetOut) {
    xmlParserInput *input;

    if (ctxt == NULL || startOut == NULL || sizeInOut == NULL ||
        offsetOut == NULL)
        return -1;

    if (inputIndex < 0) {
        inputIndex += ctxt->inputNr;
        if (inputIndex < 0)
            return -1;
    }
    if (inputIndex >= ctxt->inputNr)
        return -1;

    input = ctxt->inputTab[inputIndex];

    xmlParserInputGetWindow(input, startOut, sizeInOut, offsetOut);

    return 0;
}

/************************************************************************
 *									*
 *		Handling of node information				*
 *									*
 ************************************************************************/

/**
 * Same as #xmlCtxtReset
 *
 * @deprecated Use #xmlCtxtReset
 *
 * @param ctxt  an XML parser context
 */
void
xmlClearParserCtxt(xmlParserCtxt *ctxt)
{
    xmlCtxtReset(ctxt);
}


/**
 * Find the parser node info struct for a given node
 *
 * @deprecated Don't use.
 *
 * @param ctx  an XML parser context
 * @param node  an XML node within the tree
 * @returns an xmlParserNodeInfo block pointer or NULL
 */
const xmlParserNodeInfo *
xmlParserFindNodeInfo(xmlParserCtxt *ctx, xmlNode *node)
{
    unsigned long pos;

    if ((ctx == NULL) || (node == NULL))
        return (NULL);
    /* Find position where node should be at */
    pos = xmlParserFindNodeInfoIndex(&ctx->node_seq, node);
    if (pos < ctx->node_seq.length
        && ctx->node_seq.buffer[pos].node == node)
        return &ctx->node_seq.buffer[pos];
    else
        return NULL;
}


/**
 * Initialize (set to initial state) node info sequence
 *
 * @deprecated Don't use.
 *
 * @param seq  a node info sequence pointer
 */
void
xmlInitNodeInfoSeq(xmlParserNodeInfoSeq *seq)
{
    if (seq == NULL)
        return;
    seq->length = 0;
    seq->maximum = 0;
    seq->buffer = NULL;
}

/**
 * Clear (release memory and reinitialize) node info sequence
 *
 * @deprecated Don't use.
 *
 * @param seq  a node info sequence pointer
 */
void
xmlClearNodeInfoSeq(xmlParserNodeInfoSeq *seq)
{
    if (seq == NULL)
        return;
    if (seq->buffer != NULL)
        xmlFree(seq->buffer);
    xmlInitNodeInfoSeq(seq);
}

/**
 * Find the index that the info record for the given node is or
 * should be at in a sorted sequence.
 *
 * @deprecated Don't use.
 *
 * @param seq  a node info sequence pointer
 * @param node  an XML node pointer
 * @returns a long indicating the position of the record
 */
unsigned long
xmlParserFindNodeInfoIndex(xmlParserNodeInfoSeq *seq,
                           xmlNode *node)
{
    unsigned long upper, lower, middle;
    int found = 0;

    if ((seq == NULL) || (node == NULL))
        return ((unsigned long) -1);

    /* Do a binary search for the key */
    lower = 1;
    upper = seq->length;
    middle = 0;
    while (lower <= upper && !found) {
        middle = lower + (upper - lower) / 2;
        if (node == seq->buffer[middle - 1].node)
            found = 1;
        else if (node < seq->buffer[middle - 1].node)
            upper = middle - 1;
        else
            lower = middle + 1;
    }

    /* Return position */
    if (middle == 0 || seq->buffer[middle - 1].node < node)
        return middle;
    else
        return middle - 1;
}


/**
 * Insert node info record into the sorted sequence
 *
 * @deprecated Don't use.
 *
 * @param ctxt  an XML parser context
 * @param info  a node info sequence pointer
 */
void
xmlParserAddNodeInfo(xmlParserCtxt *ctxt,
                     xmlParserNodeInfo *info)
{
    unsigned long pos;

    if ((ctxt == NULL) || (info == NULL)) return;

    /* Find pos and check to see if node is already in the sequence */
    pos = xmlParserFindNodeInfoIndex(&ctxt->node_seq, (xmlNodePtr)
                                     info->node);

    if ((pos < ctxt->node_seq.length) &&
        (ctxt->node_seq.buffer != NULL) &&
        (ctxt->node_seq.buffer[pos].node == info->node)) {
        ctxt->node_seq.buffer[pos] = *info;
    }

    /* Otherwise, we need to add new node to buffer */
    else {
        if (ctxt->node_seq.length + 1 > ctxt->node_seq.maximum) {
            xmlParserNodeInfo *tmp;
            int newSize;

            newSize = xmlGrowCapacity(ctxt->node_seq.maximum, sizeof(tmp[0]),
                                      4, XML_MAX_ITEMS);
            if (newSize < 0) {
		xmlCtxtErrMemory(ctxt);
                return;
            }
            tmp = xmlRealloc(ctxt->node_seq.buffer, newSize * sizeof(tmp[0]));
            if (tmp == NULL) {
		xmlCtxtErrMemory(ctxt);
                return;
            }
            ctxt->node_seq.buffer = tmp;
            ctxt->node_seq.maximum = newSize;
        }

        /* If position is not at end, move elements out of the way */
        if (pos != ctxt->node_seq.length) {
            unsigned long i;

            for (i = ctxt->node_seq.length; i > pos; i--)
                ctxt->node_seq.buffer[i] = ctxt->node_seq.buffer[i - 1];
        }

        /* Copy element and increase length */
        ctxt->node_seq.buffer[pos] = *info;
        ctxt->node_seq.length++;
    }
}

/************************************************************************
 *									*
 *		Defaults settings					*
 *									*
 ************************************************************************/
/**
 * Set and return the previous value for enabling pedantic warnings.
 *
 * @deprecated Use the modern options API with XML_PARSE_PEDANTIC.
 *
 * @param val  int 0 or 1
 * @returns the last value for 0 for no substitution, 1 for substitution.
 */

int
xmlPedanticParserDefault(int val) {
    int old = xmlPedanticParserDefaultValue;

    xmlPedanticParserDefaultValue = val;
    return(old);
}

/**
 * Has no effect.
 *
 * @deprecated Line numbers are always enabled.
 *
 * @param val  int 0 or 1
 * @returns 1
 */

int
xmlLineNumbersDefault(int val ATTRIBUTE_UNUSED) {
    return(1);
}

/**
 * Set and return the previous value for default entity support.
 *
 * @deprecated Use the modern options API with XML_PARSE_NOENT.
 *
 * @param val  int 0 or 1
 * @returns the last value for 0 for no substitution, 1 for substitution.
 */

int
xmlSubstituteEntitiesDefault(int val) {
    int old = xmlSubstituteEntitiesDefaultValue;

    xmlSubstituteEntitiesDefaultValue = val;
    return(old);
}

/**
 * Set and return the previous value for default blanks text nodes support.
 *
 * @deprecated Use the modern options API with XML_PARSE_NOBLANKS.
 *
 * @param val  int 0 or 1
 * @returns the last value for 0 for no substitution, 1 for substitution.
 */

int
xmlKeepBlanksDefault(int val) {
    int old = xmlKeepBlanksDefaultValue;

    xmlKeepBlanksDefaultValue = val;
#ifdef LIBXML_OUTPUT_ENABLED
    if (!val)
        xmlIndentTreeOutput = 1;
#endif
    return(old);
}


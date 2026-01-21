
/*
 * xmlwriter.c: XML text writer implementation
 *
 * For license and disclaimer see the license and disclaimer of
 * libxml2.
 *
 * Author: Alfred Mickautsch
 */

#define IN_LIBXML
#include "libxml.h"
#include <string.h>
#include <stdarg.h>

#include <libxml/xmlmemory.h>
#include <libxml/parser.h>
#include <libxml/parserInternals.h>
#include <libxml/uri.h>
#include <libxml/HTMLtree.h>
#include <libxml/SAX2.h>

#ifdef LIBXML_WRITER_ENABLED

#include <libxml/xmlwriter.h>

#include "private/buf.h"
#include "private/enc.h"
#include "private/error.h"
#include "private/save.h"

#define B64LINELEN 72
#define B64CRLF "\r\n"

#ifndef va_copy
  #ifdef __va_copy
    #define va_copy(dest, src) __va_copy(dest, src)
  #else
    #define va_copy(dest, src) memcpy(&(dest), &(src), sizeof(va_list))
  #endif
#endif

/*
 * Types are kept private
 */
typedef enum {
    XML_TEXTWRITER_NONE = 0,
    XML_TEXTWRITER_NAME,
    XML_TEXTWRITER_ATTRIBUTE,
    XML_TEXTWRITER_TEXT,
    XML_TEXTWRITER_PI,
    XML_TEXTWRITER_PI_TEXT,
    XML_TEXTWRITER_CDATA,
    XML_TEXTWRITER_DTD,
    XML_TEXTWRITER_DTD_TEXT,
    XML_TEXTWRITER_DTD_ELEM,
    XML_TEXTWRITER_DTD_ELEM_TEXT,
    XML_TEXTWRITER_DTD_ATTL,
    XML_TEXTWRITER_DTD_ATTL_TEXT,
    XML_TEXTWRITER_DTD_ENTY,    /* entity */
    XML_TEXTWRITER_DTD_ENTY_TEXT,
    XML_TEXTWRITER_DTD_PENT,    /* parameter entity */
    XML_TEXTWRITER_COMMENT
} xmlTextWriterState;

typedef struct _xmlTextWriterStackEntry xmlTextWriterStackEntry;

struct _xmlTextWriterStackEntry {
    xmlChar *name;
    xmlTextWriterState state;
};

typedef struct _xmlTextWriterNsStackEntry xmlTextWriterNsStackEntry;
struct _xmlTextWriterNsStackEntry {
    xmlChar *prefix;
    xmlChar *uri;
    xmlLinkPtr elem;
};

struct _xmlTextWriter {
    xmlOutputBufferPtr out;     /* output buffer */
    xmlListPtr nodes;           /* element name stack */
    xmlListPtr nsstack;         /* name spaces stack */
    int level;
    int indent;                 /* enable indent */
    int doindent;               /* internal indent flag */
    xmlChar *ichar;             /* indent character */
    char qchar;                 /* character used for quoting attribute values */
    xmlParserCtxtPtr ctxt;
    int no_doc_free;
    xmlDocPtr doc;
};

static void xmlFreeTextWriterStackEntry(xmlLinkPtr lk);
static int xmlCmpTextWriterStackEntry(const void *data0,
                                      const void *data1);
static int xmlTextWriterOutputNSDecl(xmlTextWriterPtr writer);
static void xmlFreeTextWriterNsStackEntry(xmlLinkPtr lk);
static int xmlCmpTextWriterNsStackEntry(const void *data0,
                                        const void *data1);
static int xmlTextWriterWriteDocCallback(void *context,
                                         const char *str, int len);
static int xmlTextWriterCloseDocCallback(void *context);

static xmlChar *xmlTextWriterVSprintf(const char *format, va_list argptr) LIBXML_ATTR_FORMAT(1,0);
static int xmlOutputBufferWriteBase64(xmlOutputBufferPtr out, int len,
                                      const unsigned char *data);
static void xmlTextWriterStartDocumentCallback(void *ctx);
static int xmlTextWriterWriteIndent(xmlTextWriterPtr writer);
static int
  xmlTextWriterHandleStateDependencies(xmlTextWriterPtr writer,
                                       xmlTextWriterStackEntry * p);

/**
 * Handle a writer error
 *
 * @param ctxt  a writer context
 * @param error  the error number
 * @param msg  the error message
 */
static void
xmlWriterErrMsg(xmlTextWriterPtr ctxt, xmlParserErrors error,
               const char *msg)
{
    xmlParserCtxtPtr pctxt = NULL;

    if (ctxt != NULL)
        pctxt = ctxt->ctxt;

    xmlRaiseError(NULL, NULL, NULL, pctxt,
                  NULL, XML_FROM_WRITER, error, XML_ERR_FATAL,
                  NULL, 0, NULL, NULL, NULL, 0, 0, "%s", msg);
}

/**
 * Handle a writer error
 *
 * @param ctxt  a writer context
 * @param error  the error number
 * @param msg  the error message
 * @param val  an int
 */
static void LIBXML_ATTR_FORMAT(3,0)
xmlWriterErrMsgInt(xmlTextWriterPtr ctxt, xmlParserErrors error,
               const char *msg, int val)
{
    xmlParserCtxtPtr pctxt = NULL;

    if (ctxt != NULL)
        pctxt = ctxt->ctxt;

    xmlRaiseError(NULL, NULL, NULL, pctxt,
	          NULL, XML_FROM_WRITER, error, XML_ERR_FATAL,
		  NULL, 0, NULL, NULL, NULL, val, 0, msg, val);
}

/**
 * Create a new xmlTextWriter structure using an xmlOutputBuffer
 * NOTE: the `out` parameter will be deallocated when the writer is closed
 *       (if the call succeed.)
 *
 * @param out  an xmlOutputBuffer
 * @returns the new xmlTextWriter or NULL in case of error
 */
xmlTextWriter *
xmlNewTextWriter(xmlOutputBuffer *out)
{
    xmlTextWriterPtr ret;

    ret = (xmlTextWriterPtr) xmlMalloc(sizeof(xmlTextWriter));
    if (ret == NULL) {
        xmlWriterErrMsg(NULL, XML_ERR_NO_MEMORY,
                        "xmlNewTextWriter : out of memory!\n");
        return NULL;
    }
    memset(ret, 0, sizeof(xmlTextWriter));

    ret->nodes = xmlListCreate(xmlFreeTextWriterStackEntry,
                               xmlCmpTextWriterStackEntry);
    if (ret->nodes == NULL) {
        xmlWriterErrMsg(NULL, XML_ERR_NO_MEMORY,
                        "xmlNewTextWriter : out of memory!\n");
        xmlFree(ret);
        return NULL;
    }

    ret->nsstack = xmlListCreate(xmlFreeTextWriterNsStackEntry,
                                 xmlCmpTextWriterNsStackEntry);
    if (ret->nsstack == NULL) {
        xmlWriterErrMsg(NULL, XML_ERR_NO_MEMORY,
                        "xmlNewTextWriter : out of memory!\n");
        xmlListDelete(ret->nodes);
        xmlFree(ret);
        return NULL;
    }

    ret->out = out;
    ret->ichar = xmlStrdup(BAD_CAST " ");
    ret->qchar = '"';

    if (!ret->ichar) {
        xmlListDelete(ret->nodes);
        xmlListDelete(ret->nsstack);
        xmlFree(ret);
        xmlWriterErrMsg(NULL, XML_ERR_NO_MEMORY,
                        "xmlNewTextWriter : out of memory!\n");
        return NULL;
    }

    ret->doc = xmlNewDoc(NULL);

    ret->no_doc_free = 0;

    return ret;
}

/**
 * Create a new xmlTextWriter structure with `uri` as output
 *
 * @param uri  the URI of the resource for the output
 * @param compression  compress the output?
 * @returns the new xmlTextWriter or NULL in case of error
 */
xmlTextWriter *
xmlNewTextWriterFilename(const char *uri, int compression)
{
    xmlTextWriterPtr ret;
    xmlOutputBufferPtr out;

    out = xmlOutputBufferCreateFilename(uri, NULL, compression);
    if (out == NULL) {
        xmlWriterErrMsg(NULL, XML_IO_EIO,
                        "xmlNewTextWriterFilename : cannot open uri\n");
        return NULL;
    }

    ret = xmlNewTextWriter(out);
    if (ret == NULL) {
        xmlWriterErrMsg(NULL, XML_ERR_NO_MEMORY,
                        "xmlNewTextWriterFilename : out of memory!\n");
        xmlOutputBufferClose(out);
        return NULL;
    }

    ret->indent = 0;
    ret->doindent = 0;
    return ret;
}

/**
 * Create a new xmlTextWriter structure with `buf` as output
 * TODO: handle compression
 *
 * @param buf  xmlBuffer
 * @param compression  compress the output?
 * @returns the new xmlTextWriter or NULL in case of error
 */
xmlTextWriter *
xmlNewTextWriterMemory(xmlBuffer *buf, int compression ATTRIBUTE_UNUSED)
{
    xmlTextWriterPtr ret;
    xmlOutputBufferPtr out;

/*::todo handle compression */
    out = xmlOutputBufferCreateBuffer(buf, NULL);

    if (out == NULL) {
        xmlWriterErrMsg(NULL, XML_ERR_NO_MEMORY,
                        "xmlNewTextWriterMemory : out of memory!\n");
        return NULL;
    }

    ret = xmlNewTextWriter(out);
    if (ret == NULL) {
        xmlWriterErrMsg(NULL, XML_ERR_NO_MEMORY,
                        "xmlNewTextWriterMemory : out of memory!\n");
        xmlOutputBufferClose(out);
        return NULL;
    }

    return ret;
}

/**
 * Create a new xmlTextWriter structure with `ctxt` as output
 * NOTE: the `ctxt` context will be freed with the resulting writer
 *       (if the call succeeds).
 * TODO: handle compression
 *
 * @param ctxt  xmlParserCtxt to hold the new XML document tree
 * @param compression  compress the output?
 * @returns the new xmlTextWriter or NULL in case of error
 */
xmlTextWriter *
xmlNewTextWriterPushParser(xmlParserCtxt *ctxt,
                           int compression ATTRIBUTE_UNUSED)
{
    xmlTextWriterPtr ret;
    xmlOutputBufferPtr out;

    if (ctxt == NULL) {
        xmlWriterErrMsg(NULL, XML_ERR_INTERNAL_ERROR,
                        "xmlNewTextWriterPushParser : invalid context!\n");
        return NULL;
    }

    out = xmlOutputBufferCreateIO(xmlTextWriterWriteDocCallback,
                                  xmlTextWriterCloseDocCallback,
                                  (void *) ctxt, NULL);
    if (out == NULL) {
        xmlWriterErrMsg(NULL, XML_ERR_INTERNAL_ERROR,
                        "xmlNewTextWriterPushParser : error at xmlOutputBufferCreateIO!\n");
        return NULL;
    }

    ret = xmlNewTextWriter(out);
    if (ret == NULL) {
        xmlWriterErrMsg(NULL, XML_ERR_INTERNAL_ERROR,
                        "xmlNewTextWriterPushParser : error at xmlNewTextWriter!\n");
        xmlOutputBufferClose(out);
        return NULL;
    }

    ret->ctxt = ctxt;

    return ret;
}

/**
 * Create a new xmlTextWriter structure with `doc` as output
 *
 * @param doc  address of a xmlDoc to hold the new XML document tree
 * @param compression  compress the output?
 * @returns the new xmlTextWriter or NULL in case of error
 */
xmlTextWriter *
xmlNewTextWriterDoc(xmlDoc ** doc, int compression)
{
    xmlTextWriterPtr ret;
    xmlSAXHandler saxHandler;
    xmlParserCtxtPtr ctxt;

    memset(&saxHandler, '\0', sizeof(saxHandler));
    xmlSAX2InitDefaultSAXHandler(&saxHandler, 1);
    saxHandler.startDocument = xmlTextWriterStartDocumentCallback;

    ctxt = xmlCreatePushParserCtxt(&saxHandler, NULL, NULL, 0, NULL);
    if (ctxt == NULL) {
        xmlWriterErrMsg(NULL, XML_ERR_INTERNAL_ERROR,
                "xmlNewTextWriterDoc : error at xmlCreatePushParserCtxt!\n");
        return NULL;
    }
    /*
     * For some reason this seems to completely break if node names
     * are interned.
     */
    ctxt->dictNames = 0;

    ctxt->myDoc = xmlNewDoc(BAD_CAST XML_DEFAULT_VERSION);
    if (ctxt->myDoc == NULL) {
        xmlFreeParserCtxt(ctxt);
        xmlWriterErrMsg(NULL, XML_ERR_INTERNAL_ERROR,
                        "xmlNewTextWriterDoc : error at xmlNewDoc!\n");
        return NULL;
    }

    ret = xmlNewTextWriterPushParser(ctxt, compression);
    if (ret == NULL) {
        xmlFreeDoc(ctxt->myDoc);
        xmlFreeParserCtxt(ctxt);
        xmlWriterErrMsg(NULL, XML_ERR_INTERNAL_ERROR,
                "xmlNewTextWriterDoc : error at xmlNewTextWriterPushParser!\n");
        return NULL;
    }

    xmlSetDocCompressMode(ctxt->myDoc, compression);

    if (doc != NULL) {
        *doc = ctxt->myDoc;
	ret->no_doc_free = 1;
    }

    return ret;
}

/**
 * Create a new xmlTextWriter structure with `doc` as output
 * starting at `node`
 *
 * @param doc  xmlDoc
 * @param node  xmlNode or NULL for doc->children
 * @param compression  compress the output?
 * @returns the new xmlTextWriter or NULL in case of error
 */
xmlTextWriter *
xmlNewTextWriterTree(xmlDoc *doc, xmlNode *node, int compression)
{
    xmlTextWriterPtr ret;
    xmlSAXHandler saxHandler;
    xmlParserCtxtPtr ctxt;

    if (doc == NULL) {
        xmlWriterErrMsg(NULL, XML_ERR_INTERNAL_ERROR,
                        "xmlNewTextWriterTree : invalid document tree!\n");
        return NULL;
    }

    memset(&saxHandler, '\0', sizeof(saxHandler));
    xmlSAX2InitDefaultSAXHandler(&saxHandler, 1);
    saxHandler.startDocument = xmlTextWriterStartDocumentCallback;

    ctxt = xmlCreatePushParserCtxt(&saxHandler, NULL, NULL, 0, NULL);
    if (ctxt == NULL) {
        xmlWriterErrMsg(NULL, XML_ERR_INTERNAL_ERROR,
                        "xmlNewTextWriterDoc : error at xmlCreatePushParserCtxt!\n");
        return NULL;
    }
    /*
     * For some reason this seems to completely break if node names
     * are interned.
     */
    ctxt->dictNames = 0;

    ret = xmlNewTextWriterPushParser(ctxt, compression);
    if (ret == NULL) {
        xmlFreeParserCtxt(ctxt);
        xmlWriterErrMsg(NULL, XML_ERR_INTERNAL_ERROR,
                        "xmlNewTextWriterDoc : error at xmlNewTextWriterPushParser!\n");
        return NULL;
    }

    ctxt->myDoc = doc;
    ctxt->node = node;
    ret->no_doc_free = 1;

    xmlSetDocCompressMode(doc, compression);

    return ret;
}

/**
 * Deallocate all the resources associated to the writer
 *
 * @param writer  the xmlTextWriter
 */
void
xmlFreeTextWriter(xmlTextWriter *writer)
{
    if (writer == NULL)
        return;

    if (writer->out != NULL)
        xmlOutputBufferClose(writer->out);

    if (writer->nodes != NULL)
        xmlListDelete(writer->nodes);

    if (writer->nsstack != NULL)
        xmlListDelete(writer->nsstack);

    if (writer->ctxt != NULL) {
        if ((writer->ctxt->myDoc != NULL) && (writer->no_doc_free == 0)) {
	    xmlFreeDoc(writer->ctxt->myDoc);
	    writer->ctxt->myDoc = NULL;
	}
        xmlFreeParserCtxt(writer->ctxt);
    }

    if (writer->doc != NULL)
        xmlFreeDoc(writer->doc);

    if (writer->ichar != NULL)
        xmlFree(writer->ichar);
    xmlFree(writer);
}

/**
 * Start a new xml document
 *
 * @param writer  the xmlTextWriter
 * @param version  the xml version ("1.0") or NULL for default ("1.0")
 * @param encoding  the encoding or NULL for default
 * @param standalone  "yes" or "no" or NULL for default
 * @returns the bytes written (may be 0 because of buffering) or -1 in case of error
 */
int
xmlTextWriterStartDocument(xmlTextWriter *writer, const char *version,
                           const char *encoding, const char *standalone)
{
    int count;
    int sum;
    xmlLinkPtr lk;
    xmlCharEncodingHandlerPtr encoder;

    if ((writer == NULL) || (writer->out == NULL)) {
        xmlWriterErrMsg(writer, XML_ERR_INTERNAL_ERROR,
                        "xmlTextWriterStartDocument : invalid writer!\n");
        return -1;
    }

    lk = xmlListFront(writer->nodes);
    if ((lk != NULL) && (xmlLinkGetData(lk) != NULL)) {
        xmlWriterErrMsg(writer, XML_ERR_INTERNAL_ERROR,
                        "xmlTextWriterStartDocument : not allowed in this context!\n");
        return -1;
    }

    encoder = NULL;
    if (encoding != NULL) {
        encoder = xmlFindCharEncodingHandler(encoding);
        if (encoder == NULL) {
            xmlWriterErrMsg(writer, XML_ERR_UNSUPPORTED_ENCODING,
                            "xmlTextWriterStartDocument : unsupported encoding\n");
            return -1;
        }
    }

    writer->out->encoder = encoder;
    if (encoder != NULL) {
	if (writer->out->conv == NULL) {
	    writer->out->conv = xmlBufCreate(4000);
	}
        xmlCharEncOutput(writer->out, 1);
        if ((writer->doc != NULL) && (writer->doc->encoding == NULL))
            writer->doc->encoding = xmlStrdup((xmlChar *)writer->out->encoder->name);
    } else
        writer->out->conv = NULL;

    sum = 0;
    count = xmlOutputBufferWriteString(writer->out, "<?xml version=");
    if (count < 0)
        return -1;
    sum += count;
    count = xmlOutputBufferWrite(writer->out, 1, &writer->qchar);
    if (count < 0)
        return -1;
    sum += count;
    if (version != 0)
        count = xmlOutputBufferWriteString(writer->out, version);
    else
        count = xmlOutputBufferWriteString(writer->out, "1.0");
    if (count < 0)
        return -1;
    sum += count;
    count = xmlOutputBufferWrite(writer->out, 1, &writer->qchar);
    if (count < 0)
        return -1;
    sum += count;
    if (writer->out->encoder != 0) {
        count = xmlOutputBufferWriteString(writer->out, " encoding=");
        if (count < 0)
            return -1;
        sum += count;
        count = xmlOutputBufferWrite(writer->out, 1, &writer->qchar);
        if (count < 0)
            return -1;
        sum += count;
        count =
            xmlOutputBufferWriteString(writer->out,
                                       writer->out->encoder->name);
        if (count < 0)
            return -1;
        sum += count;
        count = xmlOutputBufferWrite(writer->out, 1, &writer->qchar);
        if (count < 0)
            return -1;
        sum += count;
    }

    if (standalone != 0) {
        count = xmlOutputBufferWriteString(writer->out, " standalone=");
        if (count < 0)
            return -1;
        sum += count;
        count = xmlOutputBufferWrite(writer->out, 1, &writer->qchar);
        if (count < 0)
            return -1;
        sum += count;
        count = xmlOutputBufferWriteString(writer->out, standalone);
        if (count < 0)
            return -1;
        sum += count;
        count = xmlOutputBufferWrite(writer->out, 1, &writer->qchar);
        if (count < 0)
            return -1;
        sum += count;
    }

    count = xmlOutputBufferWriteString(writer->out, "?>\n");
    if (count < 0)
        return -1;
    sum += count;

    return sum;
}

/**
 * End an xml document. All open elements are closed, and
 * the content is flushed to the output.
 *
 * @param writer  the xmlTextWriter
 * @returns the bytes written or -1 in case of error
 */
int
xmlTextWriterEndDocument(xmlTextWriter *writer)
{
    int count;
    int sum;
    xmlLinkPtr lk;
    xmlTextWriterStackEntry *p;

    if (writer == NULL) {
        xmlWriterErrMsg(writer, XML_ERR_INTERNAL_ERROR,
                        "xmlTextWriterEndDocument : invalid writer!\n");
        return -1;
    }

    sum = 0;
    while ((lk = xmlListFront(writer->nodes)) != NULL) {
        p = (xmlTextWriterStackEntry *) xmlLinkGetData(lk);
        if (p == 0)
            break;
        switch (p->state) {
            case XML_TEXTWRITER_NAME:
            case XML_TEXTWRITER_ATTRIBUTE:
            case XML_TEXTWRITER_TEXT:
                count = xmlTextWriterEndElement(writer);
                if (count < 0)
                    return -1;
                sum += count;
                break;
            case XML_TEXTWRITER_PI:
            case XML_TEXTWRITER_PI_TEXT:
                count = xmlTextWriterEndPI(writer);
                if (count < 0)
                    return -1;
                sum += count;
                break;
            case XML_TEXTWRITER_CDATA:
                count = xmlTextWriterEndCDATA(writer);
                if (count < 0)
                    return -1;
                sum += count;
                break;
            case XML_TEXTWRITER_DTD:
            case XML_TEXTWRITER_DTD_TEXT:
            case XML_TEXTWRITER_DTD_ELEM:
            case XML_TEXTWRITER_DTD_ELEM_TEXT:
            case XML_TEXTWRITER_DTD_ATTL:
            case XML_TEXTWRITER_DTD_ATTL_TEXT:
            case XML_TEXTWRITER_DTD_ENTY:
            case XML_TEXTWRITER_DTD_ENTY_TEXT:
            case XML_TEXTWRITER_DTD_PENT:
                count = xmlTextWriterEndDTD(writer);
                if (count < 0)
                    return -1;
                sum += count;
                break;
            case XML_TEXTWRITER_COMMENT:
                count = xmlTextWriterEndComment(writer);
                if (count < 0)
                    return -1;
                sum += count;
                break;
            default:
                break;
        }
    }

    if (!writer->indent) {
        count = xmlOutputBufferWriteString(writer->out, "\n");
        if (count < 0)
            return -1;
        sum += count;
    }

    count = xmlTextWriterFlush(writer);
    if (count < 0)
        return -1;
    sum += count;


    return sum;
}

/**
 * Start an xml comment.
 *
 * @param writer  the xmlTextWriter
 * @returns the bytes written (may be 0 because of buffering) or -1 in case of error
 */
int
xmlTextWriterStartComment(xmlTextWriter *writer)
{
    int count;
    int sum;
    xmlLinkPtr lk;
    xmlTextWriterStackEntry *p;

    if (writer == NULL) {
        xmlWriterErrMsg(writer, XML_ERR_INTERNAL_ERROR,
                        "xmlTextWriterStartComment : invalid writer!\n");
        return -1;
    }

    sum = 0;
    lk = xmlListFront(writer->nodes);
    if (lk != 0) {
        p = (xmlTextWriterStackEntry *) xmlLinkGetData(lk);
        if (p != 0) {
            switch (p->state) {
                case XML_TEXTWRITER_TEXT:
                case XML_TEXTWRITER_NONE:
                    break;
                case XML_TEXTWRITER_NAME:
                    /* Output namespace declarations */
                    count = xmlTextWriterOutputNSDecl(writer);
                    if (count < 0)
                        return -1;
                    sum += count;
                    count = xmlOutputBufferWriteString(writer->out, ">");
                    if (count < 0)
                        return -1;
                    sum += count;
                    if (writer->indent) {
                        count =
                            xmlOutputBufferWriteString(writer->out, "\n");
                        if (count < 0)
                            return -1;
                        sum += count;
                    }
                    p->state = XML_TEXTWRITER_TEXT;
                    break;
                default:
                    return -1;
            }
        }
    }

    p = (xmlTextWriterStackEntry *)
        xmlMalloc(sizeof(xmlTextWriterStackEntry));
    if (p == 0) {
        xmlWriterErrMsg(writer, XML_ERR_NO_MEMORY,
                        "xmlTextWriterStartElement : out of memory!\n");
        return -1;
    }

    p->name = NULL;
    p->state = XML_TEXTWRITER_COMMENT;

    xmlListPushFront(writer->nodes, p);

    if (writer->indent) {
        count = xmlTextWriterWriteIndent(writer);
        if (count < 0)
            return -1;
        sum += count;
    }

    count = xmlOutputBufferWriteString(writer->out, "<!--");
    if (count < 0)
        return -1;
    sum += count;

    return sum;
}

/**
 * End the current xml comment.
 *
 * @param writer  the xmlTextWriter
 * @returns the bytes written (may be 0 because of buffering) or -1 in case of error
 */
int
xmlTextWriterEndComment(xmlTextWriter *writer)
{
    int count;
    int sum;
    xmlLinkPtr lk;
    xmlTextWriterStackEntry *p;

    if (writer == NULL) {
        xmlWriterErrMsg(writer, XML_ERR_INTERNAL_ERROR,
                        "xmlTextWriterEndComment : invalid writer!\n");
        return -1;
    }

    lk = xmlListFront(writer->nodes);
    if (lk == 0) {
        xmlWriterErrMsg(writer, XML_ERR_INTERNAL_ERROR,
                        "xmlTextWriterEndComment : not allowed in this context!\n");
        return -1;
    }

    p = (xmlTextWriterStackEntry *) xmlLinkGetData(lk);
    if (p == 0)
        return -1;

    sum = 0;
    switch (p->state) {
        case XML_TEXTWRITER_COMMENT:
            count = xmlOutputBufferWriteString(writer->out, "-->");
            if (count < 0)
                return -1;
            sum += count;
            break;
        default:
            return -1;
    }

    if (writer->indent) {
        count = xmlOutputBufferWriteString(writer->out, "\n");
        if (count < 0)
            return -1;
        sum += count;
    }

    xmlListPopFront(writer->nodes);
    return sum;
}

/**
 * Write an xml comment.
 *
 * @param writer  the xmlTextWriter
 * @param format  format string (see printf)
 * @param ...  extra parameters for the format
 * @returns the bytes written (may be 0 because of buffering) or -1 in case of error
 */
int
xmlTextWriterWriteFormatComment(xmlTextWriter *writer,
                                const char *format, ...)
{
    int rc;
    va_list ap;

    va_start(ap, format);

    rc = xmlTextWriterWriteVFormatComment(writer, format, ap);

    va_end(ap);
    return rc;
}

/**
 * Write an xml comment.
 *
 * @param writer  the xmlTextWriter
 * @param format  format string (see printf)
 * @param argptr  pointer to the first member of the variable argument list.
 * @returns the bytes written (may be 0 because of buffering) or -1 in case of error
 */
int
xmlTextWriterWriteVFormatComment(xmlTextWriter *writer,
                                 const char *format, va_list argptr)
{
    int rc;
    xmlChar *buf;

    if (writer == NULL) {
        xmlWriterErrMsg(writer, XML_ERR_INTERNAL_ERROR,
                        "xmlTextWriterWriteVFormatComment : invalid writer!\n");
        return -1;
    }

    buf = xmlTextWriterVSprintf(format, argptr);
    if (buf == NULL)
        return -1;

    rc = xmlTextWriterWriteComment(writer, buf);

    xmlFree(buf);
    return rc;
}

/**
 * Write an xml comment.
 *
 * @param writer  the xmlTextWriter
 * @param content  comment string
 * @returns the bytes written (may be 0 because of buffering) or -1 in case of error
 */
int
xmlTextWriterWriteComment(xmlTextWriter *writer, const xmlChar * content)
{
    int count;
    int sum;

    sum = 0;
    count = xmlTextWriterStartComment(writer);
    if (count < 0)
        return -1;
    sum += count;
    count = xmlTextWriterWriteString(writer, content);
    if (count < 0)
        return -1;
    sum += count;
    count = xmlTextWriterEndComment(writer);
    if (count < 0)
        return -1;
    sum += count;

    return sum;
}

/**
 * Start an xml element.
 *
 * @param writer  the xmlTextWriter
 * @param name  element name
 * @returns the bytes written (may be 0 because of buffering) or -1 in case of error
 */
int
xmlTextWriterStartElement(xmlTextWriter *writer, const xmlChar * name)
{
    int count;
    int sum;
    xmlLinkPtr lk;
    xmlTextWriterStackEntry *p;

    if ((writer == NULL) || (name == NULL) || (*name == '\0'))
        return -1;

    sum = 0;
    lk = xmlListFront(writer->nodes);
    if (lk != 0) {
        p = (xmlTextWriterStackEntry *) xmlLinkGetData(lk);
        if (p != 0) {
            switch (p->state) {
                case XML_TEXTWRITER_PI:
                case XML_TEXTWRITER_PI_TEXT:
                    return -1;
                case XML_TEXTWRITER_NONE:
                    break;
				case XML_TEXTWRITER_ATTRIBUTE:
					count = xmlTextWriterEndAttribute(writer);
					if (count < 0)
						return -1;
					sum += count;
					/* fallthrough */
                case XML_TEXTWRITER_NAME:
                    /* Output namespace declarations */
                    count = xmlTextWriterOutputNSDecl(writer);
                    if (count < 0)
                        return -1;
                    sum += count;
                    count = xmlOutputBufferWriteString(writer->out, ">");
                    if (count < 0)
                        return -1;
                    sum += count;
                    if (writer->indent)
                        count =
                            xmlOutputBufferWriteString(writer->out, "\n");
                    p->state = XML_TEXTWRITER_TEXT;
                    break;
                default:
                    break;
            }
        }
    }

    p = (xmlTextWriterStackEntry *)
        xmlMalloc(sizeof(xmlTextWriterStackEntry));
    if (p == 0) {
        xmlWriterErrMsg(writer, XML_ERR_NO_MEMORY,
                        "xmlTextWriterStartElement : out of memory!\n");
        return -1;
    }

    p->name = xmlStrdup(name);
    if (p->name == 0) {
        xmlWriterErrMsg(writer, XML_ERR_NO_MEMORY,
                        "xmlTextWriterStartElement : out of memory!\n");
        xmlFree(p);
        return -1;
    }
    p->state = XML_TEXTWRITER_NAME;

    xmlListPushFront(writer->nodes, p);

    if (writer->indent) {
        count = xmlTextWriterWriteIndent(writer);
        sum += count;
    }

    count = xmlOutputBufferWriteString(writer->out, "<");
    if (count < 0)
        return -1;
    sum += count;
    count =
        xmlOutputBufferWriteString(writer->out, (const char *) p->name);
    if (count < 0)
        return -1;
    sum += count;

    return sum;
}

/**
 * Start an xml element with namespace support.
 *
 * @param writer  the xmlTextWriter
 * @param prefix  namespace prefix or NULL
 * @param name  element local name
 * @param namespaceURI  namespace URI or NULL
 * @returns the bytes written (may be 0 because of buffering) or -1 in case of error
 */
int
xmlTextWriterStartElementNS(xmlTextWriter *writer,
                            const xmlChar * prefix, const xmlChar * name,
                            const xmlChar * namespaceURI)
{
    int count;
    int sum;
    xmlChar *buf;

    if ((writer == NULL) || (name == NULL) || (*name == '\0'))
        return -1;

    buf = NULL;
    if (prefix != 0) {
        buf = xmlStrdup(prefix);
        buf = xmlStrcat(buf, BAD_CAST ":");
    }
    buf = xmlStrcat(buf, name);

    sum = 0;
    count = xmlTextWriterStartElement(writer, buf);
    xmlFree(buf);
    if (count < 0)
        return -1;
    sum += count;

    if (namespaceURI != 0) {
        xmlTextWriterNsStackEntry *p = (xmlTextWriterNsStackEntry *)
        xmlMalloc(sizeof(xmlTextWriterNsStackEntry));
        if (p == 0) {
            xmlWriterErrMsg(writer, XML_ERR_NO_MEMORY,
                            "xmlTextWriterStartElementNS : out of memory!\n");
            return -1;
        }

        buf = xmlStrdup(BAD_CAST "xmlns");
        if (prefix != 0) {
            buf = xmlStrcat(buf, BAD_CAST ":");
            buf = xmlStrcat(buf, prefix);
        }

        p->prefix = buf;
        p->uri = xmlStrdup(namespaceURI);
        if (p->uri == 0) {
            xmlWriterErrMsg(writer, XML_ERR_NO_MEMORY,
                            "xmlTextWriterStartElementNS : out of memory!\n");
            xmlFree(p);
            return -1;
        }
        p->elem = xmlListFront(writer->nodes);

        xmlListPushFront(writer->nsstack, p);
    }

    return sum;
}

/**
 * End the current xml element.
 *
 * @param writer  the xmlTextWriter
 * @returns the bytes written (may be 0 because of buffering) or -1 in case of error
 */
int
xmlTextWriterEndElement(xmlTextWriter *writer)
{
    int count;
    int sum;
    xmlLinkPtr lk;
    xmlTextWriterStackEntry *p;

    if (writer == NULL)
        return -1;

    lk = xmlListFront(writer->nodes);
    if (lk == 0) {
        xmlListDelete(writer->nsstack);
        writer->nsstack = NULL;
        return -1;
    }

    p = (xmlTextWriterStackEntry *) xmlLinkGetData(lk);
    if (p == 0) {
        xmlListDelete(writer->nsstack);
        writer->nsstack = NULL;
        return -1;
    }

    sum = 0;
    switch (p->state) {
        case XML_TEXTWRITER_ATTRIBUTE:
            count = xmlTextWriterEndAttribute(writer);
            if (count < 0) {
                xmlListDelete(writer->nsstack);
                writer->nsstack = NULL;
                return -1;
            }
            sum += count;
            /* fallthrough */
        case XML_TEXTWRITER_NAME:
            /* Output namespace declarations */
            count = xmlTextWriterOutputNSDecl(writer);
            if (count < 0)
                return -1;
            sum += count;

            if (writer->indent) /* next element needs indent */
                writer->doindent = 1;
            count = xmlOutputBufferWriteString(writer->out, "/>");
            if (count < 0)
                return -1;
            sum += count;
            break;
        case XML_TEXTWRITER_TEXT:
            if ((writer->indent) && (writer->doindent)) {
                count = xmlTextWriterWriteIndent(writer);
                sum += count;
                writer->doindent = 1;
            } else
                writer->doindent = 1;
            count = xmlOutputBufferWriteString(writer->out, "</");
            if (count < 0)
                return -1;
            sum += count;
            count = xmlOutputBufferWriteString(writer->out,
                                               (const char *) p->name);
            if (count < 0)
                return -1;
            sum += count;
            count = xmlOutputBufferWriteString(writer->out, ">");
            if (count < 0)
                return -1;
            sum += count;
            break;
        default:
            return -1;
    }

    if (writer->indent) {
        count = xmlOutputBufferWriteString(writer->out, "\n");
        sum += count;
    }

    xmlListPopFront(writer->nodes);
    return sum;
}

/**
 * End the current xml element. Writes an end tag even if the element is empty
 *
 * @param writer  the xmlTextWriter
 * @returns the bytes written (may be 0 because of buffering) or -1 in case of error
 */
int
xmlTextWriterFullEndElement(xmlTextWriter *writer)
{
    int count;
    int sum;
    xmlLinkPtr lk;
    xmlTextWriterStackEntry *p;

    if (writer == NULL)
        return -1;

    lk = xmlListFront(writer->nodes);
    if (lk == 0)
        return -1;

    p = (xmlTextWriterStackEntry *) xmlLinkGetData(lk);
    if (p == 0)
        return -1;

    sum = 0;
    switch (p->state) {
        case XML_TEXTWRITER_ATTRIBUTE:
            count = xmlTextWriterEndAttribute(writer);
            if (count < 0)
                return -1;
            sum += count;
            /* fallthrough */
        case XML_TEXTWRITER_NAME:
            /* Output namespace declarations */
            count = xmlTextWriterOutputNSDecl(writer);
            if (count < 0)
                return -1;
            sum += count;

            count = xmlOutputBufferWriteString(writer->out, ">");
            if (count < 0)
                return -1;
            sum += count;
            if (writer->indent)
                writer->doindent = 0;
            /* fallthrough */
        case XML_TEXTWRITER_TEXT:
            if ((writer->indent) && (writer->doindent)) {
                count = xmlTextWriterWriteIndent(writer);
                sum += count;
                writer->doindent = 1;
            } else
                writer->doindent = 1;
            count = xmlOutputBufferWriteString(writer->out, "</");
            if (count < 0)
                return -1;
            sum += count;
            count = xmlOutputBufferWriteString(writer->out,
                                               (const char *) p->name);
            if (count < 0)
                return -1;
            sum += count;
            count = xmlOutputBufferWriteString(writer->out, ">");
            if (count < 0)
                return -1;
            sum += count;
            break;
        default:
            return -1;
    }

    if (writer->indent) {
        count = xmlOutputBufferWriteString(writer->out, "\n");
        sum += count;
    }

    xmlListPopFront(writer->nodes);
    return sum;
}

/**
 * Write a formatted raw xml text.
 *
 * @param writer  the xmlTextWriter
 * @param format  format string (see printf)
 * @param ...  extra parameters for the format
 * @returns the bytes written (may be 0 because of buffering) or -1 in case of error
 */
int
xmlTextWriterWriteFormatRaw(xmlTextWriter *writer, const char *format,
                            ...)
{
    int rc;
    va_list ap;

    va_start(ap, format);

    rc = xmlTextWriterWriteVFormatRaw(writer, format, ap);

    va_end(ap);
    return rc;
}

/**
 * Write a formatted raw xml text.
 *
 * @param writer  the xmlTextWriter
 * @param format  format string (see printf)
 * @param argptr  pointer to the first member of the variable argument list.
 * @returns the bytes written (may be 0 because of buffering) or -1 in case of error
 */
int
xmlTextWriterWriteVFormatRaw(xmlTextWriter *writer, const char *format,
                             va_list argptr)
{
    int rc;
    xmlChar *buf;

    if (writer == NULL)
        return -1;

    buf = xmlTextWriterVSprintf(format, argptr);
    if (buf == NULL)
        return -1;

    rc = xmlTextWriterWriteRaw(writer, buf);

    xmlFree(buf);
    return rc;
}

/**
 * Write an xml text.
 * TODO: what about entities and special chars??
 *
 * @param writer  the xmlTextWriter
 * @param content  text string
 * @param len  length of the text string
 * @returns the bytes written (may be 0 because of buffering) or -1 in case of error
 */
int
xmlTextWriterWriteRawLen(xmlTextWriter *writer, const xmlChar * content,
                         int len)
{
    int count;
    int sum;
    xmlLinkPtr lk;
    xmlTextWriterStackEntry *p;

    if (writer == NULL) {
        xmlWriterErrMsg(writer, XML_ERR_INTERNAL_ERROR,
                        "xmlTextWriterWriteRawLen : invalid writer!\n");
        return -1;
    }

    if ((content == NULL) || (len < 0)) {
        xmlWriterErrMsg(writer, XML_ERR_INTERNAL_ERROR,
                        "xmlTextWriterWriteRawLen : invalid content!\n");
        return -1;
    }

    sum = 0;
    lk = xmlListFront(writer->nodes);
    if (lk != 0) {
        p = (xmlTextWriterStackEntry *) xmlLinkGetData(lk);
        count = xmlTextWriterHandleStateDependencies(writer, p);
        if (count < 0)
            return -1;
        sum += count;
    }

    if (writer->indent)
        writer->doindent = 0;

    if (content != NULL) {
        count =
            xmlOutputBufferWrite(writer->out, len, (const char *) content);
        if (count < 0)
            return -1;
        sum += count;
    }

    return sum;
}

/**
 * Write a raw xml text.
 *
 * @param writer  the xmlTextWriter
 * @param content  text string
 * @returns the bytes written (may be 0 because of buffering) or -1 in case of error
 */
int
xmlTextWriterWriteRaw(xmlTextWriter *writer, const xmlChar * content)
{
    return xmlTextWriterWriteRawLen(writer, content, xmlStrlen(content));
}

/**
 * Write a formatted xml text.
 *
 * @param writer  the xmlTextWriter
 * @param format  format string (see printf)
 * @param ...  extra parameters for the format
 * @returns the bytes written (may be 0 because of buffering) or -1 in case of error
 */
int
xmlTextWriterWriteFormatString(xmlTextWriter *writer, const char *format,
                               ...)
{
    int rc;
    va_list ap;

    if ((writer == NULL) || (format == NULL))
        return -1;

    va_start(ap, format);

    rc = xmlTextWriterWriteVFormatString(writer, format, ap);

    va_end(ap);
    return rc;
}

/**
 * Write a formatted xml text.
 *
 * @param writer  the xmlTextWriter
 * @param format  format string (see printf)
 * @param argptr  pointer to the first member of the variable argument list.
 * @returns the bytes written (may be 0 because of buffering) or -1 in case of error
 */
int
xmlTextWriterWriteVFormatString(xmlTextWriter *writer,
                                const char *format, va_list argptr)
{
    int rc;
    xmlChar *buf;

    if ((writer == NULL) || (format == NULL))
        return -1;

    buf = xmlTextWriterVSprintf(format, argptr);
    if (buf == NULL)
        return -1;

    rc = xmlTextWriterWriteString(writer, buf);

    xmlFree(buf);
    return rc;
}

/**
 * Write an xml text.
 *
 * @param writer  the xmlTextWriter
 * @param content  text string
 * @returns the bytes written (may be 0 because of buffering) or -1 in case of error
 */
int
xmlTextWriterWriteString(xmlTextWriter *writer, const xmlChar * content)
{
    int count;
    int sum;
    xmlLinkPtr lk;
    xmlTextWriterStackEntry *p;
    xmlChar *buf;

    if ((writer == NULL) || (content == NULL))
        return -1;

    sum = 0;
    buf = (xmlChar *) content;
    lk = xmlListFront(writer->nodes);
    if (lk != 0) {
        p = (xmlTextWriterStackEntry *) xmlLinkGetData(lk);
        if (p != 0) {
            switch (p->state) {
                case XML_TEXTWRITER_NAME:
                case XML_TEXTWRITER_TEXT:
                    /*
                     * TODO: Use xmlSerializeText
                     */
                    buf = xmlEncodeSpecialChars(NULL, content);
                    break;
                case XML_TEXTWRITER_ATTRIBUTE:
                    buf = NULL;
                    xmlBufAttrSerializeTxtContent(writer->out, writer->doc,
                                                  content);
                    break;
		default:
		    break;
            }
        }
    }

    if (buf != NULL) {
        count = xmlTextWriterWriteRaw(writer, buf);

        if (buf != content)     /* buf was allocated by us, so free it */
            xmlFree(buf);

        if (count < 0)
            return -1;
        sum += count;
    }

    return sum;
}

/**
 * Write base64 encoded data to an xmlOutputBuffer.
 * Adapted from John Walker's base64.c (http://www.fourmilab.ch/).
 *
 * @param out  the xmlOutputBuffer
 * @param data  binary data
 * @param len  the number of bytes to encode
 * @returns the bytes written (may be 0 because of buffering) or -1 in case of error
 */
static int
xmlOutputBufferWriteBase64(xmlOutputBufferPtr out, int len,
                           const unsigned char *data)
{
    static const unsigned char dtable[64] =
            {'A','B','C','D','E','F','G','H','I','J','K','L','M',
	     'N','O','P','Q','R','S','T','U','V','W','X','Y','Z',
	     'a','b','c','d','e','f','g','h','i','j','k','l','m',
	     'n','o','p','q','r','s','t','u','v','w','x','y','z',
	     '0','1','2','3','4','5','6','7','8','9','+','/'};

    int i;
    int linelen;
    int count;
    int sum;

    if ((out == NULL) || (len < 0) || (data == NULL))
        return(-1);

    linelen = 0;
    sum = 0;

    i = 0;
    while (1) {
        unsigned char igroup[3];
        unsigned char ogroup[4];
        int c;
        int n;

        igroup[0] = igroup[1] = igroup[2] = 0;
        for (n = 0; n < 3 && i < len; n++, i++) {
            c = data[i];
            igroup[n] = (unsigned char) c;
        }

        if (n > 0) {
            ogroup[0] = dtable[igroup[0] >> 2];
            ogroup[1] = dtable[((igroup[0] & 3) << 4) | (igroup[1] >> 4)];
            ogroup[2] =
                dtable[((igroup[1] & 0xF) << 2) | (igroup[2] >> 6)];
            ogroup[3] = dtable[igroup[2] & 0x3F];

            if (n < 3) {
                ogroup[3] = '=';
                if (n < 2) {
                    ogroup[2] = '=';
                }
            }

            if (linelen >= B64LINELEN) {
                count = xmlOutputBufferWrite(out, 2, B64CRLF);
                if (count == -1)
                    return -1;
                sum += count;
                linelen = 0;
            }
            count = xmlOutputBufferWrite(out, 4, (const char *) ogroup);
            if (count == -1)
                return -1;
            sum += count;

            linelen += 4;
        }

        if (i >= len)
            break;
    }

    return sum;
}

/**
 * Write an base64 encoded xml text.
 *
 * @param writer  the xmlTextWriter
 * @param data  binary data
 * @param start  the position within the data of the first byte to encode
 * @param len  the number of bytes to encode
 * @returns the bytes written (may be 0 because of buffering) or -1 in case of error
 */
int
xmlTextWriterWriteBase64(xmlTextWriter *writer, const char *data,
                         int start, int len)
{
    int count;
    int sum;
    xmlLinkPtr lk;
    xmlTextWriterStackEntry *p;

    if ((writer == NULL) || (data == NULL) || (start < 0) || (len < 0))
        return -1;

    sum = 0;
    lk = xmlListFront(writer->nodes);
    if (lk != 0) {
        p = (xmlTextWriterStackEntry *) xmlLinkGetData(lk);
        if (p != 0) {
            count = xmlTextWriterHandleStateDependencies(writer, p);
            if (count < 0)
                return -1;
            sum += count;
        }
    }

    if (writer->indent)
        writer->doindent = 0;

    count =
        xmlOutputBufferWriteBase64(writer->out, len,
                                   (unsigned char *) data + start);
    if (count < 0)
        return -1;
    sum += count;

    return sum;
}

/**
 * Write hqx encoded data to an xmlOutputBuffer.
 *
 * @param out  the xmlOutputBuffer
 * @param data  binary data
 * @param len  the number of bytes to encode
 * @returns the bytes written (may be 0 because of buffering)
 * or -1 in case of error
 */
static int
xmlOutputBufferWriteBinHex(xmlOutputBufferPtr out,
                           int len, const unsigned char *data)
{
    int count;
    int sum;
    static const char hex[16] =
	{'0','1','2','3','4','5','6','7','8','9','A','B','C','D','E','F'};
    int i;

    if ((out == NULL) || (data == NULL) || (len < 0)) {
        return -1;
    }

    sum = 0;
    for (i = 0; i < len; i++) {
        count =
            xmlOutputBufferWrite(out, 1,
                                 (const char *) &hex[data[i] >> 4]);
        if (count == -1)
            return -1;
        sum += count;
        count =
            xmlOutputBufferWrite(out, 1,
                                 (const char *) &hex[data[i] & 0xF]);
        if (count == -1)
            return -1;
        sum += count;
    }

    return sum;
}

/**
 * Write a BinHex encoded xml text.
 *
 * @param writer  the xmlTextWriter
 * @param data  binary data
 * @param start  the position within the data of the first byte to encode
 * @param len  the number of bytes to encode
 * @returns the bytes written (may be 0 because of buffering) or -1 in case of error
 */
int
xmlTextWriterWriteBinHex(xmlTextWriter *writer, const char *data,
                         int start, int len)
{
    int count;
    int sum;
    xmlLinkPtr lk;
    xmlTextWriterStackEntry *p;

    if ((writer == NULL) || (data == NULL) || (start < 0) || (len < 0))
        return -1;

    sum = 0;
    lk = xmlListFront(writer->nodes);
    if (lk != 0) {
        p = (xmlTextWriterStackEntry *) xmlLinkGetData(lk);
        if (p != 0) {
            count = xmlTextWriterHandleStateDependencies(writer, p);
            if (count < 0)
                return -1;
            sum += count;
        }
    }

    if (writer->indent)
        writer->doindent = 0;

    count =
        xmlOutputBufferWriteBinHex(writer->out, len,
                                   (unsigned char *) data + start);
    if (count < 0)
        return -1;
    sum += count;

    return sum;
}

/**
 * Start an xml attribute.
 *
 * @param writer  the xmlTextWriter
 * @param name  element name
 * @returns the bytes written (may be 0 because of buffering) or -1 in case of error
 */
int
xmlTextWriterStartAttribute(xmlTextWriter *writer, const xmlChar * name)
{
    int count;
    int sum;
    xmlLinkPtr lk;
    xmlTextWriterStackEntry *p;

    if ((writer == NULL) || (name == NULL) || (*name == '\0'))
        return -1;

    sum = 0;
    lk = xmlListFront(writer->nodes);
    if (lk == 0)
        return -1;

    p = (xmlTextWriterStackEntry *) xmlLinkGetData(lk);
    if (p == 0)
        return -1;

    switch (p->state) {
        case XML_TEXTWRITER_ATTRIBUTE:
            count = xmlTextWriterEndAttribute(writer);
            if (count < 0)
                return -1;
            sum += count;
            /* fallthrough */
        case XML_TEXTWRITER_NAME:
            count = xmlOutputBufferWriteString(writer->out, " ");
            if (count < 0)
                return -1;
            sum += count;
            count =
                xmlOutputBufferWriteString(writer->out,
                                           (const char *) name);
            if (count < 0)
                return -1;
            sum += count;
            count = xmlOutputBufferWriteString(writer->out, "=");
            if (count < 0)
                return -1;
            sum += count;
            count = xmlOutputBufferWrite(writer->out, 1, &writer->qchar);
            if (count < 0)
                return -1;
            sum += count;
            p->state = XML_TEXTWRITER_ATTRIBUTE;
            break;
        default:
            return -1;
    }

    return sum;
}

/**
 * Start an xml attribute with namespace support.
 *
 * @param writer  the xmlTextWriter
 * @param prefix  namespace prefix or NULL
 * @param name  element local name
 * @param namespaceURI  namespace URI or NULL
 * @returns the bytes written (may be 0 because of buffering) or -1 in case of error
 */
int
xmlTextWriterStartAttributeNS(xmlTextWriter *writer,
                              const xmlChar * prefix, const xmlChar * name,
                              const xmlChar * namespaceURI)
{
    int count;
    int sum;
    xmlChar *buf;
    xmlTextWriterNsStackEntry *p;

    if ((writer == NULL) || (name == NULL) || (*name == '\0'))
        return -1;

    /* Handle namespace first in case of error */
    if (namespaceURI != 0) {
        xmlTextWriterNsStackEntry nsentry, *curns;

        buf = xmlStrdup(BAD_CAST "xmlns");
        if (prefix != 0) {
            buf = xmlStrcat(buf, BAD_CAST ":");
            buf = xmlStrcat(buf, prefix);
        }

        nsentry.prefix = buf;
        nsentry.uri = (xmlChar *)namespaceURI;
        nsentry.elem = xmlListFront(writer->nodes);

        curns = (xmlTextWriterNsStackEntry *)xmlListSearch(writer->nsstack,
                                                           (void *)&nsentry);
        if ((curns != NULL)) {
            xmlFree(buf);
            if (xmlStrcmp(curns->uri, namespaceURI) == 0) {
                /* Namespace already defined on element skip */
                buf = NULL;
            } else {
                /* Prefix mismatch so error out */
                return -1;
            }
        }

        /* Do not add namespace decl to list - it is already there */
        if (buf != NULL) {
            p = (xmlTextWriterNsStackEntry *)
                xmlMalloc(sizeof(xmlTextWriterNsStackEntry));
            if (p == 0) {
                xmlWriterErrMsg(writer, XML_ERR_NO_MEMORY,
								        "xmlTextWriterStartAttributeNS : out of memory!\n");
                return -1;
            }

            p->prefix = buf;
            p->uri = xmlStrdup(namespaceURI);
            if (p->uri == 0) {
                xmlWriterErrMsg(writer, XML_ERR_NO_MEMORY,
                        "xmlTextWriterStartAttributeNS : out of memory!\n");
                xmlFree(p);
                return -1;
            }
            p->elem = xmlListFront(writer->nodes);

            xmlListPushFront(writer->nsstack, p);
        }
    }

    buf = NULL;
    if (prefix != 0) {
        buf = xmlStrdup(prefix);
        buf = xmlStrcat(buf, BAD_CAST ":");
    }
    buf = xmlStrcat(buf, name);

    sum = 0;
    count = xmlTextWriterStartAttribute(writer, buf);
    xmlFree(buf);
    if (count < 0)
        return -1;
    sum += count;

    return sum;
}

/**
 * End the current xml element.
 *
 * @param writer  the xmlTextWriter
 * @returns the bytes written (may be 0 because of buffering) or -1 in case of error
 */
int
xmlTextWriterEndAttribute(xmlTextWriter *writer)
{
    int count;
    int sum;
    xmlLinkPtr lk;
    xmlTextWriterStackEntry *p;

    if (writer == NULL)
        return -1;

    lk = xmlListFront(writer->nodes);
    if (lk == 0) {
        return -1;
    }

    p = (xmlTextWriterStackEntry *) xmlLinkGetData(lk);
    if (p == 0) {
        return -1;
    }

    sum = 0;
    switch (p->state) {
        case XML_TEXTWRITER_ATTRIBUTE:
            p->state = XML_TEXTWRITER_NAME;

            count = xmlOutputBufferWrite(writer->out, 1, &writer->qchar);
            if (count < 0) {
                return -1;
            }
            sum += count;
            break;
        default:
            return -1;
    }

    return sum;
}

/**
 * Write a formatted xml attribute.
 *
 * @param writer  the xmlTextWriter
 * @param name  attribute name
 * @param format  format string (see printf)
 * @param ...  extra parameters for the format
 * @returns the bytes written (may be 0 because of buffering) or -1 in case of error
 */
int
xmlTextWriterWriteFormatAttribute(xmlTextWriter *writer,
                                  const xmlChar * name, const char *format,
                                  ...)
{
    int rc;
    va_list ap;

    va_start(ap, format);

    rc = xmlTextWriterWriteVFormatAttribute(writer, name, format, ap);

    va_end(ap);
    return rc;
}

/**
 * Write a formatted xml attribute.
 *
 * @param writer  the xmlTextWriter
 * @param name  attribute name
 * @param format  format string (see printf)
 * @param argptr  pointer to the first member of the variable argument list.
 * @returns the bytes written (may be 0 because of buffering) or -1 in case of error
 */
int
xmlTextWriterWriteVFormatAttribute(xmlTextWriter *writer,
                                   const xmlChar * name,
                                   const char *format, va_list argptr)
{
    int rc;
    xmlChar *buf;

    if (writer == NULL)
        return -1;

    buf = xmlTextWriterVSprintf(format, argptr);
    if (buf == NULL)
        return -1;

    rc = xmlTextWriterWriteAttribute(writer, name, buf);

    xmlFree(buf);
    return rc;
}

/**
 * Write an xml attribute.
 *
 * @param writer  the xmlTextWriter
 * @param name  attribute name
 * @param content  attribute content
 * @returns the bytes written (may be 0 because of buffering) or -1 in case of error
 */
int
xmlTextWriterWriteAttribute(xmlTextWriter *writer, const xmlChar * name,
                            const xmlChar * content)
{
    int count;
    int sum;

    sum = 0;
    count = xmlTextWriterStartAttribute(writer, name);
    if (count < 0)
        return -1;
    sum += count;
    count = xmlTextWriterWriteString(writer, content);
    if (count < 0)
        return -1;
    sum += count;
    count = xmlTextWriterEndAttribute(writer);
    if (count < 0)
        return -1;
    sum += count;

    return sum;
}

/**
 * Write a formatted xml attribute.with namespace support
 *
 * @param writer  the xmlTextWriter
 * @param prefix  namespace prefix
 * @param name  attribute local name
 * @param namespaceURI  namespace URI
 * @param format  format string (see printf)
 * @param ...  extra parameters for the format
 * @returns the bytes written (may be 0 because of buffering) or -1 in case of error
 */
int
xmlTextWriterWriteFormatAttributeNS(xmlTextWriter *writer,
                                    const xmlChar * prefix,
                                    const xmlChar * name,
                                    const xmlChar * namespaceURI,
                                    const char *format, ...)
{
    int rc;
    va_list ap;

    va_start(ap, format);

    rc = xmlTextWriterWriteVFormatAttributeNS(writer, prefix, name,
                                              namespaceURI, format, ap);

    va_end(ap);
    return rc;
}

/**
 * Write a formatted xml attribute.with namespace support
 *
 * @param writer  the xmlTextWriter
 * @param prefix  namespace prefix
 * @param name  attribute local name
 * @param namespaceURI  namespace URI
 * @param format  format string (see printf)
 * @param argptr  pointer to the first member of the variable argument list.
 * @returns the bytes written (may be 0 because of buffering) or -1 in case of error
 */
int
xmlTextWriterWriteVFormatAttributeNS(xmlTextWriter *writer,
                                     const xmlChar * prefix,
                                     const xmlChar * name,
                                     const xmlChar * namespaceURI,
                                     const char *format, va_list argptr)
{
    int rc;
    xmlChar *buf;

    if (writer == NULL)
        return -1;

    buf = xmlTextWriterVSprintf(format, argptr);
    if (buf == NULL)
        return -1;

    rc = xmlTextWriterWriteAttributeNS(writer, prefix, name, namespaceURI,
                                       buf);

    xmlFree(buf);
    return rc;
}

/**
 * Write an xml attribute.
 *
 * @param writer  the xmlTextWriter
 * @param prefix  namespace prefix
 * @param name  attribute local name
 * @param namespaceURI  namespace URI
 * @param content  attribute content
 * @returns the bytes written (may be 0 because of buffering) or -1 in case of error
 */
int
xmlTextWriterWriteAttributeNS(xmlTextWriter *writer,
                              const xmlChar * prefix, const xmlChar * name,
                              const xmlChar * namespaceURI,
                              const xmlChar * content)
{
    int count;
    int sum;

    if ((writer == NULL) || (name == NULL) || (*name == '\0'))
        return -1;

    sum = 0;
    count = xmlTextWriterStartAttributeNS(writer, prefix, name, namespaceURI);
    if (count < 0)
        return -1;
    sum += count;
    count = xmlTextWriterWriteString(writer, content);
    if (count < 0)
        return -1;
    sum += count;
    count = xmlTextWriterEndAttribute(writer);
    if (count < 0)
        return -1;
    sum += count;

    return sum;
}

/**
 * Write a formatted xml element.
 *
 * @param writer  the xmlTextWriter
 * @param name  element name
 * @param format  format string (see printf)
 * @param ...  extra parameters for the format
 * @returns the bytes written (may be 0 because of buffering) or -1 in case of error
 */
int
xmlTextWriterWriteFormatElement(xmlTextWriter *writer,
                                const xmlChar * name, const char *format,
                                ...)
{
    int rc;
    va_list ap;

    va_start(ap, format);

    rc = xmlTextWriterWriteVFormatElement(writer, name, format, ap);

    va_end(ap);
    return rc;
}

/**
 * Write a formatted xml element.
 *
 * @param writer  the xmlTextWriter
 * @param name  element name
 * @param format  format string (see printf)
 * @param argptr  pointer to the first member of the variable argument list.
 * @returns the bytes written (may be 0 because of buffering) or -1 in case of error
 */
int
xmlTextWriterWriteVFormatElement(xmlTextWriter *writer,
                                 const xmlChar * name, const char *format,
                                 va_list argptr)
{
    int rc;
    xmlChar *buf;

    if (writer == NULL)
        return -1;

    buf = xmlTextWriterVSprintf(format, argptr);
    if (buf == NULL)
        return -1;

    rc = xmlTextWriterWriteElement(writer, name, buf);

    xmlFree(buf);
    return rc;
}

/**
 * Write an xml element.
 *
 * @param writer  the xmlTextWriter
 * @param name  element name
 * @param content  element content
 * @returns the bytes written (may be 0 because of buffering) or -1 in case of error
 */
int
xmlTextWriterWriteElement(xmlTextWriter *writer, const xmlChar * name,
                          const xmlChar * content)
{
    int count;
    int sum;

    sum = 0;
    count = xmlTextWriterStartElement(writer, name);
    if (count == -1)
        return -1;
    sum += count;
    if (content != NULL) {
	count = xmlTextWriterWriteString(writer, content);
	if (count == -1)
	    return -1;
	sum += count;
    }
    count = xmlTextWriterEndElement(writer);
    if (count == -1)
        return -1;
    sum += count;

    return sum;
}

/**
 * Write a formatted xml element with namespace support.
 *
 * @param writer  the xmlTextWriter
 * @param prefix  namespace prefix
 * @param name  element local name
 * @param namespaceURI  namespace URI
 * @param format  format string (see printf)
 * @param ...  extra parameters for the format
 * @returns the bytes written (may be 0 because of buffering) or -1 in case of error
 */
int
xmlTextWriterWriteFormatElementNS(xmlTextWriter *writer,
                                  const xmlChar * prefix,
                                  const xmlChar * name,
                                  const xmlChar * namespaceURI,
                                  const char *format, ...)
{
    int rc;
    va_list ap;

    va_start(ap, format);

    rc = xmlTextWriterWriteVFormatElementNS(writer, prefix, name,
                                            namespaceURI, format, ap);

    va_end(ap);
    return rc;
}

/**
 * Write a formatted xml element with namespace support.
 *
 * @param writer  the xmlTextWriter
 * @param prefix  namespace prefix
 * @param name  element local name
 * @param namespaceURI  namespace URI
 * @param format  format string (see printf)
 * @param argptr  pointer to the first member of the variable argument list.
 * @returns the bytes written (may be 0 because of buffering) or -1 in case of error
 */
int
xmlTextWriterWriteVFormatElementNS(xmlTextWriter *writer,
                                   const xmlChar * prefix,
                                   const xmlChar * name,
                                   const xmlChar * namespaceURI,
                                   const char *format, va_list argptr)
{
    int rc;
    xmlChar *buf;

    if (writer == NULL)
        return -1;

    buf = xmlTextWriterVSprintf(format, argptr);
    if (buf == NULL)
        return -1;

    rc = xmlTextWriterWriteElementNS(writer, prefix, name, namespaceURI,
                                     buf);

    xmlFree(buf);
    return rc;
}

/**
 * Write an xml element with namespace support.
 *
 * @param writer  the xmlTextWriter
 * @param prefix  namespace prefix
 * @param name  element local name
 * @param namespaceURI  namespace URI
 * @param content  element content
 * @returns the bytes written (may be 0 because of buffering) or -1 in case of error
 */
int
xmlTextWriterWriteElementNS(xmlTextWriter *writer,
                            const xmlChar * prefix, const xmlChar * name,
                            const xmlChar * namespaceURI,
                            const xmlChar * content)
{
    int count;
    int sum;

    if ((writer == NULL) || (name == NULL) || (*name == '\0'))
        return -1;

    sum = 0;
    count =
        xmlTextWriterStartElementNS(writer, prefix, name, namespaceURI);
    if (count < 0)
        return -1;
    sum += count;
    count = xmlTextWriterWriteString(writer, content);
    if (count == -1)
        return -1;
    sum += count;
    count = xmlTextWriterEndElement(writer);
    if (count == -1)
        return -1;
    sum += count;

    return sum;
}

/**
 * Start an xml PI.
 *
 * @param writer  the xmlTextWriter
 * @param target  PI target
 * @returns the bytes written (may be 0 because of buffering) or -1 in case of error
 */
int
xmlTextWriterStartPI(xmlTextWriter *writer, const xmlChar * target)
{
    int count;
    int sum;
    xmlLinkPtr lk;
    xmlTextWriterStackEntry *p;

    if ((writer == NULL) || (target == NULL) || (*target == '\0'))
        return -1;

    if (xmlStrcasecmp(target, (const xmlChar *) "xml") == 0) {
        xmlWriterErrMsg(writer, XML_ERR_INTERNAL_ERROR,
                        "xmlTextWriterStartPI : target name [Xx][Mm][Ll] is reserved for xml standardization!\n");
        return -1;
    }

    sum = 0;
    lk = xmlListFront(writer->nodes);
    if (lk != 0) {
        p = (xmlTextWriterStackEntry *) xmlLinkGetData(lk);
        if (p != 0) {
            switch (p->state) {
                case XML_TEXTWRITER_ATTRIBUTE:
                    count = xmlTextWriterEndAttribute(writer);
                    if (count < 0)
                        return -1;
                    sum += count;
                    /* fallthrough */
                case XML_TEXTWRITER_NAME:
                    /* Output namespace declarations */
                    count = xmlTextWriterOutputNSDecl(writer);
                    if (count < 0)
                        return -1;
                    sum += count;
                    count = xmlOutputBufferWriteString(writer->out, ">");
                    if (count < 0)
                        return -1;
                    sum += count;
                    p->state = XML_TEXTWRITER_TEXT;
                    break;
                case XML_TEXTWRITER_NONE:
                case XML_TEXTWRITER_TEXT:
                case XML_TEXTWRITER_DTD:
                    break;
                case XML_TEXTWRITER_PI:
                case XML_TEXTWRITER_PI_TEXT:
                    xmlWriterErrMsg(writer, XML_ERR_INTERNAL_ERROR,
                                    "xmlTextWriterStartPI : nested PI!\n");
                    return -1;
                default:
                    return -1;
            }
        }
    }

    p = (xmlTextWriterStackEntry *)
        xmlMalloc(sizeof(xmlTextWriterStackEntry));
    if (p == 0) {
        xmlWriterErrMsg(writer, XML_ERR_NO_MEMORY,
                        "xmlTextWriterStartPI : out of memory!\n");
        return -1;
    }

    p->name = xmlStrdup(target);
    if (p->name == 0) {
        xmlWriterErrMsg(writer, XML_ERR_NO_MEMORY,
                        "xmlTextWriterStartPI : out of memory!\n");
        xmlFree(p);
        return -1;
    }
    p->state = XML_TEXTWRITER_PI;

    xmlListPushFront(writer->nodes, p);

    count = xmlOutputBufferWriteString(writer->out, "<?");
    if (count < 0)
        return -1;
    sum += count;
    count =
        xmlOutputBufferWriteString(writer->out, (const char *) p->name);
    if (count < 0)
        return -1;
    sum += count;

    return sum;
}

/**
 * End the current xml PI.
 *
 * @param writer  the xmlTextWriter
 * @returns the bytes written (may be 0 because of buffering) or -1 in case of error
 */
int
xmlTextWriterEndPI(xmlTextWriter *writer)
{
    int count;
    int sum;
    xmlLinkPtr lk;
    xmlTextWriterStackEntry *p;

    if (writer == NULL)
        return -1;

    lk = xmlListFront(writer->nodes);
    if (lk == 0)
        return 0;

    p = (xmlTextWriterStackEntry *) xmlLinkGetData(lk);
    if (p == 0)
        return 0;

    sum = 0;
    switch (p->state) {
        case XML_TEXTWRITER_PI:
        case XML_TEXTWRITER_PI_TEXT:
            count = xmlOutputBufferWriteString(writer->out, "?>");
            if (count < 0)
                return -1;
            sum += count;
            break;
        default:
            return -1;
    }

    if (writer->indent) {
        count = xmlOutputBufferWriteString(writer->out, "\n");
	if (count < 0)
	return -1;
        sum += count;
    }

    xmlListPopFront(writer->nodes);
    return sum;
}

/**
 * Write a formatted PI.
 *
 * @param writer  the xmlTextWriter
 * @param target  PI target
 * @param format  format string (see printf)
 * @param ...  extra parameters for the format
 * @returns the bytes written (may be 0 because of buffering) or -1 in case of error
 */
int
xmlTextWriterWriteFormatPI(xmlTextWriter *writer, const xmlChar * target,
                           const char *format, ...)
{
    int rc;
    va_list ap;

    va_start(ap, format);

    rc = xmlTextWriterWriteVFormatPI(writer, target, format, ap);

    va_end(ap);
    return rc;
}

/**
 * Write a formatted xml PI.
 *
 * @param writer  the xmlTextWriter
 * @param target  PI target
 * @param format  format string (see printf)
 * @param argptr  pointer to the first member of the variable argument list.
 * @returns the bytes written (may be 0 because of buffering) or -1 in case of error
 */
int
xmlTextWriterWriteVFormatPI(xmlTextWriter *writer,
                            const xmlChar * target, const char *format,
                            va_list argptr)
{
    int rc;
    xmlChar *buf;

    if (writer == NULL)
        return -1;

    buf = xmlTextWriterVSprintf(format, argptr);
    if (buf == NULL)
        return -1;

    rc = xmlTextWriterWritePI(writer, target, buf);

    xmlFree(buf);
    return rc;
}

/**
 * Write an xml PI.
 *
 * @param writer  the xmlTextWriter
 * @param target  PI target
 * @param content  PI content
 * @returns the bytes written (may be 0 because of buffering) or -1 in case of error
 */
int
xmlTextWriterWritePI(xmlTextWriter *writer, const xmlChar * target,
                     const xmlChar * content)
{
    int count;
    int sum;

    sum = 0;
    count = xmlTextWriterStartPI(writer, target);
    if (count == -1)
        return -1;
    sum += count;
    if (content != 0) {
        count = xmlTextWriterWriteString(writer, content);
        if (count == -1)
            return -1;
        sum += count;
    }
    count = xmlTextWriterEndPI(writer);
    if (count == -1)
        return -1;
    sum += count;

    return sum;
}

/**
 * Start an xml CDATA section.
 *
 * @param writer  the xmlTextWriter
 * @returns the bytes written (may be 0 because of buffering) or -1 in case of error
 */
int
xmlTextWriterStartCDATA(xmlTextWriter *writer)
{
    int count;
    int sum;
    xmlLinkPtr lk;
    xmlTextWriterStackEntry *p;

    if (writer == NULL)
        return -1;

    sum = 0;
    lk = xmlListFront(writer->nodes);
    if (lk != 0) {
        p = (xmlTextWriterStackEntry *) xmlLinkGetData(lk);
        if (p != 0) {
            switch (p->state) {
                case XML_TEXTWRITER_NONE:
		case XML_TEXTWRITER_TEXT:
                case XML_TEXTWRITER_PI:
                case XML_TEXTWRITER_PI_TEXT:
                    break;
                case XML_TEXTWRITER_ATTRIBUTE:
                    count = xmlTextWriterEndAttribute(writer);
                    if (count < 0)
                        return -1;
                    sum += count;
                    /* fallthrough */
                case XML_TEXTWRITER_NAME:
                    /* Output namespace declarations */
                    count = xmlTextWriterOutputNSDecl(writer);
                    if (count < 0)
                        return -1;
                    sum += count;
                    count = xmlOutputBufferWriteString(writer->out, ">");
                    if (count < 0)
                        return -1;
                    sum += count;
                    p->state = XML_TEXTWRITER_TEXT;
                    break;
                case XML_TEXTWRITER_CDATA:
                    xmlWriterErrMsg(writer, XML_ERR_INTERNAL_ERROR,
                                    "xmlTextWriterStartCDATA : CDATA not allowed in this context!\n");
                    return -1;
                default:
                    return -1;
            }
        }
    }

    p = (xmlTextWriterStackEntry *)
        xmlMalloc(sizeof(xmlTextWriterStackEntry));
    if (p == 0) {
        xmlWriterErrMsg(writer, XML_ERR_NO_MEMORY,
                        "xmlTextWriterStartCDATA : out of memory!\n");
        return -1;
    }

    p->name = NULL;
    p->state = XML_TEXTWRITER_CDATA;

    xmlListPushFront(writer->nodes, p);

    count = xmlOutputBufferWriteString(writer->out, "<![CDATA[");
    if (count < 0)
        return -1;
    sum += count;

    return sum;
}

/**
 * End an xml CDATA section.
 *
 * @param writer  the xmlTextWriter
 * @returns the bytes written (may be 0 because of buffering) or -1 in case of error
 */
int
xmlTextWriterEndCDATA(xmlTextWriter *writer)
{
    int count;
    int sum;
    xmlLinkPtr lk;
    xmlTextWriterStackEntry *p;

    if (writer == NULL)
        return -1;

    lk = xmlListFront(writer->nodes);
    if (lk == 0)
        return -1;

    p = (xmlTextWriterStackEntry *) xmlLinkGetData(lk);
    if (p == 0)
        return -1;

    sum = 0;
    switch (p->state) {
        case XML_TEXTWRITER_CDATA:
            count = xmlOutputBufferWriteString(writer->out, "]]>");
            if (count < 0)
                return -1;
            sum += count;
            break;
        default:
            return -1;
    }

    xmlListPopFront(writer->nodes);
    return sum;
}

/**
 * Write a formatted xml CDATA.
 *
 * @param writer  the xmlTextWriter
 * @param format  format string (see printf)
 * @param ...  extra parameters for the format
 * @returns the bytes written (may be 0 because of buffering) or -1 in case of error
 */
int
xmlTextWriterWriteFormatCDATA(xmlTextWriter *writer, const char *format,
                              ...)
{
    int rc;
    va_list ap;

    va_start(ap, format);

    rc = xmlTextWriterWriteVFormatCDATA(writer, format, ap);

    va_end(ap);
    return rc;
}

/**
 * Write a formatted xml CDATA.
 *
 * @param writer  the xmlTextWriter
 * @param format  format string (see printf)
 * @param argptr  pointer to the first member of the variable argument list.
 * @returns the bytes written (may be 0 because of buffering) or -1 in case of error
 */
int
xmlTextWriterWriteVFormatCDATA(xmlTextWriter *writer, const char *format,
                               va_list argptr)
{
    int rc;
    xmlChar *buf;

    if (writer == NULL)
        return -1;

    buf = xmlTextWriterVSprintf(format, argptr);
    if (buf == NULL)
        return -1;

    rc = xmlTextWriterWriteCDATA(writer, buf);

    xmlFree(buf);
    return rc;
}

/**
 * Write an xml CDATA.
 *
 * @param writer  the xmlTextWriter
 * @param content  CDATA content
 * @returns the bytes written (may be 0 because of buffering) or -1 in case of error
 */
int
xmlTextWriterWriteCDATA(xmlTextWriter *writer, const xmlChar * content)
{
    int count;
    int sum;

    sum = 0;
    count = xmlTextWriterStartCDATA(writer);
    if (count == -1)
        return -1;
    sum += count;
    if (content != 0) {
        count = xmlTextWriterWriteString(writer, content);
        if (count == -1)
            return -1;
        sum += count;
    }
    count = xmlTextWriterEndCDATA(writer);
    if (count == -1)
        return -1;
    sum += count;

    return sum;
}

/**
 * Start an xml DTD.
 *
 * @param writer  the xmlTextWriter
 * @param name  the name of the DTD
 * @param pubid  the public identifier, which is an alternative to the system identifier
 * @param sysid  the system identifier, which is the URI of the DTD
 * @returns the bytes written (may be 0 because of buffering) or -1 in case of error
 */
int
xmlTextWriterStartDTD(xmlTextWriter *writer,
                      const xmlChar * name,
                      const xmlChar * pubid, const xmlChar * sysid)
{
    int count;
    int sum;
    xmlLinkPtr lk;
    xmlTextWriterStackEntry *p;

    if (writer == NULL || name == NULL || *name == '\0')
        return -1;

    sum = 0;
    lk = xmlListFront(writer->nodes);
    if ((lk != NULL) && (xmlLinkGetData(lk) != NULL)) {
        xmlWriterErrMsg(writer, XML_ERR_INTERNAL_ERROR,
                        "xmlTextWriterStartDTD : DTD allowed only in prolog!\n");
        return -1;
    }

    p = (xmlTextWriterStackEntry *)
        xmlMalloc(sizeof(xmlTextWriterStackEntry));
    if (p == 0) {
        xmlWriterErrMsg(writer, XML_ERR_NO_MEMORY,
                        "xmlTextWriterStartDTD : out of memory!\n");
        return -1;
    }

    p->name = xmlStrdup(name);
    if (p->name == 0) {
        xmlWriterErrMsg(writer, XML_ERR_NO_MEMORY,
                        "xmlTextWriterStartDTD : out of memory!\n");
        xmlFree(p);
        return -1;
    }
    p->state = XML_TEXTWRITER_DTD;

    xmlListPushFront(writer->nodes, p);

    count = xmlOutputBufferWriteString(writer->out, "<!DOCTYPE ");
    if (count < 0)
        return -1;
    sum += count;
    count = xmlOutputBufferWriteString(writer->out, (const char *) name);
    if (count < 0)
        return -1;
    sum += count;

    if (pubid != 0) {
        if (sysid == 0) {
            xmlWriterErrMsg(writer, XML_ERR_INTERNAL_ERROR,
                            "xmlTextWriterStartDTD : system identifier needed!\n");
            return -1;
        }

        if (writer->indent)
            count = xmlOutputBufferWrite(writer->out, 1, "\n");
        else
            count = xmlOutputBufferWrite(writer->out, 1, " ");
        if (count < 0)
            return -1;
        sum += count;

        count = xmlOutputBufferWriteString(writer->out, "PUBLIC ");
        if (count < 0)
            return -1;
        sum += count;

        count = xmlOutputBufferWrite(writer->out, 1, &writer->qchar);
        if (count < 0)
            return -1;
        sum += count;

        count =
            xmlOutputBufferWriteString(writer->out, (const char *) pubid);
        if (count < 0)
            return -1;
        sum += count;

        count = xmlOutputBufferWrite(writer->out, 1, &writer->qchar);
        if (count < 0)
            return -1;
        sum += count;
    }

    if (sysid != 0) {
        if (pubid == 0) {
            if (writer->indent)
                count = xmlOutputBufferWrite(writer->out, 1, "\n");
            else
                count = xmlOutputBufferWrite(writer->out, 1, " ");
            if (count < 0)
                return -1;
            sum += count;
            count = xmlOutputBufferWriteString(writer->out, "SYSTEM ");
            if (count < 0)
                return -1;
            sum += count;
        } else {
			if (writer->indent)
            count = xmlOutputBufferWriteString(writer->out, "\n       ");
            else
                count = xmlOutputBufferWrite(writer->out, 1, " ");
            if (count < 0)
                return -1;
            sum += count;
        }

        count = xmlOutputBufferWrite(writer->out, 1, &writer->qchar);
        if (count < 0)
            return -1;
        sum += count;

        count =
            xmlOutputBufferWriteString(writer->out, (const char *) sysid);
        if (count < 0)
            return -1;
        sum += count;

        count = xmlOutputBufferWrite(writer->out, 1, &writer->qchar);
        if (count < 0)
            return -1;
        sum += count;
    }

    return sum;
}

/**
 * End an xml DTD.
 *
 * @param writer  the xmlTextWriter
 * @returns the bytes written (may be 0 because of buffering) or -1 in case of error
 */
int
xmlTextWriterEndDTD(xmlTextWriter *writer)
{
    int loop;
    int count;
    int sum;
    xmlLinkPtr lk;
    xmlTextWriterStackEntry *p;

    if (writer == NULL)
        return -1;

    sum = 0;
    loop = 1;
    while (loop) {
        lk = xmlListFront(writer->nodes);
        if (lk == NULL)
            break;
        p = (xmlTextWriterStackEntry *) xmlLinkGetData(lk);
        if (p == 0)
            break;
        switch (p->state) {
            case XML_TEXTWRITER_DTD_TEXT:
                count = xmlOutputBufferWriteString(writer->out, "]");
                if (count < 0)
                    return -1;
                sum += count;
                /* fallthrough */
            case XML_TEXTWRITER_DTD:
                count = xmlOutputBufferWriteString(writer->out, ">");

                if (writer->indent) {
                    if (count < 0)
                        return -1;
                    sum += count;
                    count = xmlOutputBufferWriteString(writer->out, "\n");
                }

                xmlListPopFront(writer->nodes);
                break;
            case XML_TEXTWRITER_DTD_ELEM:
            case XML_TEXTWRITER_DTD_ELEM_TEXT:
                count = xmlTextWriterEndDTDElement(writer);
                break;
            case XML_TEXTWRITER_DTD_ATTL:
            case XML_TEXTWRITER_DTD_ATTL_TEXT:
                count = xmlTextWriterEndDTDAttlist(writer);
                break;
            case XML_TEXTWRITER_DTD_ENTY:
            case XML_TEXTWRITER_DTD_PENT:
            case XML_TEXTWRITER_DTD_ENTY_TEXT:
                count = xmlTextWriterEndDTDEntity(writer);
                break;
            case XML_TEXTWRITER_COMMENT:
                count = xmlTextWriterEndComment(writer);
                break;
            default:
                loop = 0;
                continue;
        }

        if (count < 0)
            return -1;
        sum += count;
    }

    return sum;
}

/**
 * Write a DTD with a formatted markup declarations part.
 *
 * @param writer  the xmlTextWriter
 * @param name  the name of the DTD
 * @param pubid  the public identifier, which is an alternative to the system identifier
 * @param sysid  the system identifier, which is the URI of the DTD
 * @param format  format string (see printf)
 * @param ...  extra parameters for the format
 * @returns the bytes written (may be 0 because of buffering) or -1 in case of error
 */
int
xmlTextWriterWriteFormatDTD(xmlTextWriter *writer,
                            const xmlChar * name,
                            const xmlChar * pubid,
                            const xmlChar * sysid, const char *format, ...)
{
    int rc;
    va_list ap;

    va_start(ap, format);

    rc = xmlTextWriterWriteVFormatDTD(writer, name, pubid, sysid, format,
                                      ap);

    va_end(ap);
    return rc;
}

/**
 * Write a DTD with a formatted markup declarations part.
 *
 * @param writer  the xmlTextWriter
 * @param name  the name of the DTD
 * @param pubid  the public identifier, which is an alternative to the system identifier
 * @param sysid  the system identifier, which is the URI of the DTD
 * @param format  format string (see printf)
 * @param argptr  pointer to the first member of the variable argument list.
 * @returns the bytes written (may be 0 because of buffering) or -1 in case of error
 */
int
xmlTextWriterWriteVFormatDTD(xmlTextWriter *writer,
                             const xmlChar * name,
                             const xmlChar * pubid,
                             const xmlChar * sysid,
                             const char *format, va_list argptr)
{
    int rc;
    xmlChar *buf;

    if (writer == NULL)
        return -1;

    buf = xmlTextWriterVSprintf(format, argptr);
    if (buf == NULL)
        return -1;

    rc = xmlTextWriterWriteDTD(writer, name, pubid, sysid, buf);

    xmlFree(buf);
    return rc;
}

/**
 * Write a DTD.
 *
 * @param writer  the xmlTextWriter
 * @param name  the name of the DTD
 * @param pubid  the public identifier, which is an alternative to the system identifier
 * @param sysid  the system identifier, which is the URI of the DTD
 * @param subset  string content of the DTD
 * @returns the bytes written (may be 0 because of buffering) or -1 in case of error
 */
int
xmlTextWriterWriteDTD(xmlTextWriter *writer,
                      const xmlChar * name,
                      const xmlChar * pubid,
                      const xmlChar * sysid, const xmlChar * subset)
{
    int count;
    int sum;

    sum = 0;
    count = xmlTextWriterStartDTD(writer, name, pubid, sysid);
    if (count == -1)
        return -1;
    sum += count;
    if (subset != 0) {
        count = xmlTextWriterWriteString(writer, subset);
        if (count == -1)
            return -1;
        sum += count;
    }
    count = xmlTextWriterEndDTD(writer);
    if (count == -1)
        return -1;
    sum += count;

    return sum;
}

/**
 * Start an xml DTD element.
 *
 * @param writer  the xmlTextWriter
 * @param name  the name of the DTD element
 * @returns the bytes written (may be 0 because of buffering) or -1 in case of error
 */
int
xmlTextWriterStartDTDElement(xmlTextWriter *writer, const xmlChar * name)
{
    int count;
    int sum;
    xmlLinkPtr lk;
    xmlTextWriterStackEntry *p;

    if (writer == NULL || name == NULL || *name == '\0')
        return -1;

    sum = 0;
    lk = xmlListFront(writer->nodes);
    if (lk == 0) {
        return -1;
    }

    p = (xmlTextWriterStackEntry *) xmlLinkGetData(lk);
    if (p != 0) {
        switch (p->state) {
            case XML_TEXTWRITER_DTD:
                count = xmlOutputBufferWriteString(writer->out, " [");
                if (count < 0)
                    return -1;
                sum += count;
                if (writer->indent) {
                    count = xmlOutputBufferWriteString(writer->out, "\n");
                    if (count < 0)
                        return -1;
                    sum += count;
                }
                p->state = XML_TEXTWRITER_DTD_TEXT;
                /* fallthrough */
            case XML_TEXTWRITER_DTD_TEXT:
            case XML_TEXTWRITER_NONE:
                break;
            default:
                return -1;
        }
    }

    p = (xmlTextWriterStackEntry *)
        xmlMalloc(sizeof(xmlTextWriterStackEntry));
    if (p == 0) {
        xmlWriterErrMsg(writer, XML_ERR_NO_MEMORY,
                        "xmlTextWriterStartDTDElement : out of memory!\n");
        return -1;
    }

    p->name = xmlStrdup(name);
    if (p->name == 0) {
        xmlWriterErrMsg(writer, XML_ERR_NO_MEMORY,
                        "xmlTextWriterStartDTDElement : out of memory!\n");
        xmlFree(p);
        return -1;
    }
    p->state = XML_TEXTWRITER_DTD_ELEM;

    xmlListPushFront(writer->nodes, p);

    if (writer->indent) {
        count = xmlTextWriterWriteIndent(writer);
        if (count < 0)
            return -1;
        sum += count;
    }

    count = xmlOutputBufferWriteString(writer->out, "<!ELEMENT ");
    if (count < 0)
        return -1;
    sum += count;
    count = xmlOutputBufferWriteString(writer->out, (const char *) name);
    if (count < 0)
        return -1;
    sum += count;

    return sum;
}

/**
 * End an xml DTD element.
 *
 * @param writer  the xmlTextWriter
 * @returns the bytes written (may be 0 because of buffering) or -1 in case of error
 */
int
xmlTextWriterEndDTDElement(xmlTextWriter *writer)
{
    int count;
    int sum;
    xmlLinkPtr lk;
    xmlTextWriterStackEntry *p;

    if (writer == NULL)
        return -1;

    sum = 0;
    lk = xmlListFront(writer->nodes);
    if (lk == 0)
        return -1;

    p = (xmlTextWriterStackEntry *) xmlLinkGetData(lk);
    if (p == 0)
        return -1;

    switch (p->state) {
        case XML_TEXTWRITER_DTD_ELEM:
        case XML_TEXTWRITER_DTD_ELEM_TEXT:
            count = xmlOutputBufferWriteString(writer->out, ">");
            if (count < 0)
                return -1;
            sum += count;
            break;
        default:
            return -1;
    }

    if (writer->indent) {
        count = xmlOutputBufferWriteString(writer->out, "\n");
        if (count < 0)
            return -1;
        sum += count;
    }

    xmlListPopFront(writer->nodes);
    return sum;
}

/**
 * Write a formatted DTD element.
 *
 * @param writer  the xmlTextWriter
 * @param name  the name of the DTD element
 * @param format  format string (see printf)
 * @param ...  extra parameters for the format
 * @returns the bytes written (may be 0 because of buffering) or -1 in case of error
 */
int
xmlTextWriterWriteFormatDTDElement(xmlTextWriter *writer,
                                   const xmlChar * name,
                                   const char *format, ...)
{
    int rc;
    va_list ap;

    va_start(ap, format);

    rc = xmlTextWriterWriteVFormatDTDElement(writer, name, format, ap);

    va_end(ap);
    return rc;
}

/**
 * Write a formatted DTD element.
 *
 * @param writer  the xmlTextWriter
 * @param name  the name of the DTD element
 * @param format  format string (see printf)
 * @param argptr  pointer to the first member of the variable argument list.
 * @returns the bytes written (may be 0 because of buffering) or -1 in case of error
 */
int
xmlTextWriterWriteVFormatDTDElement(xmlTextWriter *writer,
                                    const xmlChar * name,
                                    const char *format, va_list argptr)
{
    int rc;
    xmlChar *buf;

    if (writer == NULL)
        return -1;

    buf = xmlTextWriterVSprintf(format, argptr);
    if (buf == NULL)
        return -1;

    rc = xmlTextWriterWriteDTDElement(writer, name, buf);

    xmlFree(buf);
    return rc;
}

/**
 * Write a DTD element.
 *
 * @param writer  the xmlTextWriter
 * @param name  the name of the DTD element
 * @param content  content of the element
 * @returns the bytes written (may be 0 because of buffering) or -1 in case of error
 */
int
xmlTextWriterWriteDTDElement(xmlTextWriter *writer,
                             const xmlChar * name, const xmlChar * content)
{
    int count;
    int sum;

    if (content == NULL)
        return -1;

    sum = 0;
    count = xmlTextWriterStartDTDElement(writer, name);
    if (count == -1)
        return -1;
    sum += count;

    count = xmlTextWriterWriteString(writer, content);
    if (count == -1)
        return -1;
    sum += count;

    count = xmlTextWriterEndDTDElement(writer);
    if (count == -1)
        return -1;
    sum += count;

    return sum;
}

/**
 * Start an xml DTD ATTLIST.
 *
 * @param writer  the xmlTextWriter
 * @param name  the name of the DTD ATTLIST
 * @returns the bytes written (may be 0 because of buffering) or -1 in case of error
 */
int
xmlTextWriterStartDTDAttlist(xmlTextWriter *writer, const xmlChar * name)
{
    int count;
    int sum;
    xmlLinkPtr lk;
    xmlTextWriterStackEntry *p;

    if (writer == NULL || name == NULL || *name == '\0')
        return -1;

    sum = 0;
    lk = xmlListFront(writer->nodes);
    if (lk == 0) {
        return -1;
    }

    p = (xmlTextWriterStackEntry *) xmlLinkGetData(lk);
    if (p != 0) {
        switch (p->state) {
            case XML_TEXTWRITER_DTD:
                count = xmlOutputBufferWriteString(writer->out, " [");
                if (count < 0)
                    return -1;
                sum += count;
                if (writer->indent) {
                    count = xmlOutputBufferWriteString(writer->out, "\n");
                    if (count < 0)
                        return -1;
                    sum += count;
                }
                p->state = XML_TEXTWRITER_DTD_TEXT;
                /* fallthrough */
            case XML_TEXTWRITER_DTD_TEXT:
            case XML_TEXTWRITER_NONE:
                break;
            default:
                return -1;
        }
    }

    p = (xmlTextWriterStackEntry *)
        xmlMalloc(sizeof(xmlTextWriterStackEntry));
    if (p == 0) {
        xmlWriterErrMsg(writer, XML_ERR_NO_MEMORY,
                        "xmlTextWriterStartDTDAttlist : out of memory!\n");
        return -1;
    }

    p->name = xmlStrdup(name);
    if (p->name == 0) {
        xmlWriterErrMsg(writer, XML_ERR_NO_MEMORY,
                        "xmlTextWriterStartDTDAttlist : out of memory!\n");
        xmlFree(p);
        return -1;
    }
    p->state = XML_TEXTWRITER_DTD_ATTL;

    xmlListPushFront(writer->nodes, p);

    if (writer->indent) {
        count = xmlTextWriterWriteIndent(writer);
        if (count < 0)
            return -1;
        sum += count;
    }

    count = xmlOutputBufferWriteString(writer->out, "<!ATTLIST ");
    if (count < 0)
        return -1;
    sum += count;
    count = xmlOutputBufferWriteString(writer->out, (const char *) name);
    if (count < 0)
        return -1;
    sum += count;

    return sum;
}

/**
 * End an xml DTD attribute list.
 *
 * @param writer  the xmlTextWriter
 * @returns the bytes written (may be 0 because of buffering) or -1 in case of error
 */
int
xmlTextWriterEndDTDAttlist(xmlTextWriter *writer)
{
    int count;
    int sum;
    xmlLinkPtr lk;
    xmlTextWriterStackEntry *p;

    if (writer == NULL)
        return -1;

    sum = 0;
    lk = xmlListFront(writer->nodes);
    if (lk == 0)
        return -1;

    p = (xmlTextWriterStackEntry *) xmlLinkGetData(lk);
    if (p == 0)
        return -1;

    switch (p->state) {
        case XML_TEXTWRITER_DTD_ATTL:
        case XML_TEXTWRITER_DTD_ATTL_TEXT:
            count = xmlOutputBufferWriteString(writer->out, ">");
            if (count < 0)
                return -1;
            sum += count;
            break;
        default:
            return -1;
    }

    if (writer->indent) {
        count = xmlOutputBufferWriteString(writer->out, "\n");
        if (count < 0)
            return -1;
        sum += count;
    }

    xmlListPopFront(writer->nodes);
    return sum;
}

/**
 * Write a formatted DTD ATTLIST.
 *
 * @param writer  the xmlTextWriter
 * @param name  the name of the DTD ATTLIST
 * @param format  format string (see printf)
 * @param ...  extra parameters for the format
 * @returns the bytes written (may be 0 because of buffering) or -1 in case of error
 */
int
xmlTextWriterWriteFormatDTDAttlist(xmlTextWriter *writer,
                                   const xmlChar * name,
                                   const char *format, ...)
{
    int rc;
    va_list ap;

    va_start(ap, format);

    rc = xmlTextWriterWriteVFormatDTDAttlist(writer, name, format, ap);

    va_end(ap);
    return rc;
}

/**
 * Write a formatted DTD ATTLIST.
 *
 * @param writer  the xmlTextWriter
 * @param name  the name of the DTD ATTLIST
 * @param format  format string (see printf)
 * @param argptr  pointer to the first member of the variable argument list.
 * @returns the bytes written (may be 0 because of buffering) or -1 in case of error
 */
int
xmlTextWriterWriteVFormatDTDAttlist(xmlTextWriter *writer,
                                    const xmlChar * name,
                                    const char *format, va_list argptr)
{
    int rc;
    xmlChar *buf;

    if (writer == NULL)
        return -1;

    buf = xmlTextWriterVSprintf(format, argptr);
    if (buf == NULL)
        return -1;

    rc = xmlTextWriterWriteDTDAttlist(writer, name, buf);

    xmlFree(buf);
    return rc;
}

/**
 * Write a DTD ATTLIST.
 *
 * @param writer  the xmlTextWriter
 * @param name  the name of the DTD ATTLIST
 * @param content  content of the ATTLIST
 * @returns the bytes written (may be 0 because of buffering) or -1 in case of error
 */
int
xmlTextWriterWriteDTDAttlist(xmlTextWriter *writer,
                             const xmlChar * name, const xmlChar * content)
{
    int count;
    int sum;

    if (content == NULL)
        return -1;

    sum = 0;
    count = xmlTextWriterStartDTDAttlist(writer, name);
    if (count == -1)
        return -1;
    sum += count;

    count = xmlTextWriterWriteString(writer, content);
    if (count == -1)
        return -1;
    sum += count;

    count = xmlTextWriterEndDTDAttlist(writer);
    if (count == -1)
        return -1;
    sum += count;

    return sum;
}

/**
 * Start an xml DTD ATTLIST.
 *
 * @param writer  the xmlTextWriter
 * @param pe  TRUE if this is a parameter entity, FALSE if not
 * @param name  the name of the DTD ATTLIST
 * @returns the bytes written (may be 0 because of buffering) or -1 in case of error
 */
int
xmlTextWriterStartDTDEntity(xmlTextWriter *writer,
                            int pe, const xmlChar * name)
{
    int count;
    int sum;
    xmlLinkPtr lk;
    xmlTextWriterStackEntry *p;

    if (writer == NULL || name == NULL || *name == '\0')
        return -1;

    sum = 0;
    lk = xmlListFront(writer->nodes);
    if (lk != 0) {

        p = (xmlTextWriterStackEntry *) xmlLinkGetData(lk);
        if (p != 0) {
            switch (p->state) {
                case XML_TEXTWRITER_DTD:
                    count = xmlOutputBufferWriteString(writer->out, " [");
                    if (count < 0)
                        return -1;
                    sum += count;
                    if (writer->indent) {
                        count =
                            xmlOutputBufferWriteString(writer->out, "\n");
                        if (count < 0)
                            return -1;
                        sum += count;
                    }
                    p->state = XML_TEXTWRITER_DTD_TEXT;
                    /* fallthrough */
                case XML_TEXTWRITER_DTD_TEXT:
                case XML_TEXTWRITER_NONE:
                    break;
                default:
                    return -1;
            }
        }
    }

    p = (xmlTextWriterStackEntry *)
        xmlMalloc(sizeof(xmlTextWriterStackEntry));
    if (p == 0) {
        xmlWriterErrMsg(writer, XML_ERR_NO_MEMORY,
                        "xmlTextWriterStartDTDElement : out of memory!\n");
        return -1;
    }

    p->name = xmlStrdup(name);
    if (p->name == 0) {
        xmlWriterErrMsg(writer, XML_ERR_NO_MEMORY,
                        "xmlTextWriterStartDTDElement : out of memory!\n");
        xmlFree(p);
        return -1;
    }

    if (pe != 0)
        p->state = XML_TEXTWRITER_DTD_PENT;
    else
        p->state = XML_TEXTWRITER_DTD_ENTY;

    xmlListPushFront(writer->nodes, p);

    if (writer->indent) {
        count = xmlTextWriterWriteIndent(writer);
        if (count < 0)
            return -1;
        sum += count;
    }

    count = xmlOutputBufferWriteString(writer->out, "<!ENTITY ");
    if (count < 0)
        return -1;
    sum += count;

    if (pe != 0) {
        count = xmlOutputBufferWriteString(writer->out, "% ");
        if (count < 0)
            return -1;
        sum += count;
    }

    count = xmlOutputBufferWriteString(writer->out, (const char *) name);
    if (count < 0)
        return -1;
    sum += count;

    return sum;
}

/**
 * End an xml DTD entity.
 *
 * @param writer  the xmlTextWriter
 * @returns the bytes written (may be 0 because of buffering) or -1 in case of error
 */
int
xmlTextWriterEndDTDEntity(xmlTextWriter *writer)
{
    int count;
    int sum;
    xmlLinkPtr lk;
    xmlTextWriterStackEntry *p;

    if (writer == NULL)
        return -1;

    sum = 0;
    lk = xmlListFront(writer->nodes);
    if (lk == 0)
        return -1;

    p = (xmlTextWriterStackEntry *) xmlLinkGetData(lk);
    if (p == 0)
        return -1;

    switch (p->state) {
        case XML_TEXTWRITER_DTD_ENTY_TEXT:
            count = xmlOutputBufferWrite(writer->out, 1, &writer->qchar);
            if (count < 0)
                return -1;
            sum += count;
            /* Falls through. */
        case XML_TEXTWRITER_DTD_ENTY:
        case XML_TEXTWRITER_DTD_PENT:
            count = xmlOutputBufferWriteString(writer->out, ">");
            if (count < 0)
                return -1;
            sum += count;
            break;
        default:
            return -1;
    }

    if (writer->indent) {
        count = xmlOutputBufferWriteString(writer->out, "\n");
        if (count < 0)
            return -1;
        sum += count;
    }

    xmlListPopFront(writer->nodes);
    return sum;
}

/**
 * Write a formatted DTD internal entity.
 *
 * @param writer  the xmlTextWriter
 * @param pe  TRUE if this is a parameter entity, FALSE if not
 * @param name  the name of the DTD entity
 * @param format  format string (see printf)
 * @param ...  extra parameters for the format
 * @returns the bytes written (may be 0 because of buffering) or -1 in case of error
 */
int
xmlTextWriterWriteFormatDTDInternalEntity(xmlTextWriter *writer,
                                          int pe,
                                          const xmlChar * name,
                                          const char *format, ...)
{
    int rc;
    va_list ap;

    va_start(ap, format);

    rc = xmlTextWriterWriteVFormatDTDInternalEntity(writer, pe, name,
                                                    format, ap);

    va_end(ap);
    return rc;
}

/**
 * Write a formatted DTD internal entity.
 *
 * @param writer  the xmlTextWriter
 * @param pe  TRUE if this is a parameter entity, FALSE if not
 * @param name  the name of the DTD entity
 * @param format  format string (see printf)
 * @param argptr  pointer to the first member of the variable argument list.
 * @returns the bytes written (may be 0 because of buffering) or -1 in case of error
 */
int
xmlTextWriterWriteVFormatDTDInternalEntity(xmlTextWriter *writer,
                                           int pe,
                                           const xmlChar * name,
                                           const char *format,
                                           va_list argptr)
{
    int rc;
    xmlChar *buf;

    if (writer == NULL)
        return -1;

    buf = xmlTextWriterVSprintf(format, argptr);
    if (buf == NULL)
        return -1;

    rc = xmlTextWriterWriteDTDInternalEntity(writer, pe, name, buf);

    xmlFree(buf);
    return rc;
}

/**
 * Write a DTD entity.
 *
 * @param writer  the xmlTextWriter
 * @param pe  TRUE if this is a parameter entity, FALSE if not
 * @param name  the name of the DTD entity
 * @param pubid  the public identifier, which is an alternative to the system identifier
 * @param sysid  the system identifier, which is the URI of the DTD
 * @param ndataid  the xml notation name.
 * @param content  content of the entity
 * @returns the bytes written (may be 0 because of buffering) or -1 in case of error
 */
int
xmlTextWriterWriteDTDEntity(xmlTextWriter *writer,
                            int pe,
                            const xmlChar * name,
                            const xmlChar * pubid,
                            const xmlChar * sysid,
                            const xmlChar * ndataid,
                            const xmlChar * content)
{
    if ((content == NULL) && (pubid == NULL) && (sysid == NULL))
        return -1;
    if ((pe != 0) && (ndataid != NULL))
        return -1;

    if ((pubid == NULL) && (sysid == NULL))
        return xmlTextWriterWriteDTDInternalEntity(writer, pe, name,
                                                   content);

    return xmlTextWriterWriteDTDExternalEntity(writer, pe, name, pubid,
                                               sysid, ndataid);
}

/**
 * Write a DTD internal entity.
 *
 * @param writer  the xmlTextWriter
 * @param pe  TRUE if this is a parameter entity, FALSE if not
 * @param name  the name of the DTD entity
 * @param content  content of the entity
 * @returns the bytes written (may be 0 because of buffering) or -1 in case of error
 */
int
xmlTextWriterWriteDTDInternalEntity(xmlTextWriter *writer,
                                    int pe,
                                    const xmlChar * name,
                                    const xmlChar * content)
{
    int count;
    int sum;

    if ((name == NULL) || (*name == '\0') || (content == NULL))
        return -1;

    sum = 0;
    count = xmlTextWriterStartDTDEntity(writer, pe, name);
    if (count == -1)
        return -1;
    sum += count;

    count = xmlTextWriterWriteString(writer, content);
    if (count == -1)
        return -1;
    sum += count;

    count = xmlTextWriterEndDTDEntity(writer);
    if (count == -1)
        return -1;
    sum += count;

    return sum;
}

/**
 * Write a DTD external entity. The entity must have been started with #xmlTextWriterStartDTDEntity
 *
 * @param writer  the xmlTextWriter
 * @param pe  TRUE if this is a parameter entity, FALSE if not
 * @param name  the name of the DTD entity
 * @param pubid  the public identifier, which is an alternative to the system identifier
 * @param sysid  the system identifier, which is the URI of the DTD
 * @param ndataid  the xml notation name.
 * @returns the bytes written (may be 0 because of buffering) or -1 in case of error
 */
int
xmlTextWriterWriteDTDExternalEntity(xmlTextWriter *writer,
                                    int pe,
                                    const xmlChar * name,
                                    const xmlChar * pubid,
                                    const xmlChar * sysid,
                                    const xmlChar * ndataid)
{
    int count;
    int sum;

    if (((pubid == NULL) && (sysid == NULL)))
        return -1;
    if ((pe != 0) && (ndataid != NULL))
        return -1;

    sum = 0;
    count = xmlTextWriterStartDTDEntity(writer, pe, name);
    if (count == -1)
        return -1;
    sum += count;

    count =
        xmlTextWriterWriteDTDExternalEntityContents(writer, pubid, sysid,
                                                    ndataid);
    if (count < 0)
        return -1;
    sum += count;

    count = xmlTextWriterEndDTDEntity(writer);
    if (count == -1)
        return -1;
    sum += count;

    return sum;
}

/**
 * Write the contents of a DTD external entity.
 *
 * @param writer  the xmlTextWriter
 * @param pubid  the public identifier, which is an alternative to the system identifier
 * @param sysid  the system identifier, which is the URI of the DTD
 * @param ndataid  the xml notation name.
 * @returns the bytes written (may be 0 because of buffering) or -1 in case of error
 */
int
xmlTextWriterWriteDTDExternalEntityContents(xmlTextWriter *writer,
                                            const xmlChar * pubid,
                                            const xmlChar * sysid,
                                            const xmlChar * ndataid)
{
    int count;
    int sum;
    xmlLinkPtr lk;
    xmlTextWriterStackEntry *p;

    if (writer == NULL) {
        xmlWriterErrMsg(writer, XML_ERR_INTERNAL_ERROR,
                        "xmlTextWriterWriteDTDExternalEntityContents: xmlTextWriterPtr invalid!\n");
        return -1;
    }

    sum = 0;
    lk = xmlListFront(writer->nodes);
    if (lk == 0) {
        xmlWriterErrMsg(writer, XML_ERR_INTERNAL_ERROR,
                        "xmlTextWriterWriteDTDExternalEntityContents: you must call xmlTextWriterStartDTDEntity before the call to this function!\n");
        return -1;
    }

    p = (xmlTextWriterStackEntry *) xmlLinkGetData(lk);
    if (p == 0)
        return -1;

    switch (p->state) {
        case XML_TEXTWRITER_DTD_ENTY:
            break;
        case XML_TEXTWRITER_DTD_PENT:
            if (ndataid != NULL) {
                xmlWriterErrMsg(writer, XML_ERR_INTERNAL_ERROR,
                                "xmlTextWriterWriteDTDExternalEntityContents: notation not allowed with parameter entities!\n");
                return -1;
            }
            break;
        default:
            xmlWriterErrMsg(writer, XML_ERR_INTERNAL_ERROR,
                            "xmlTextWriterWriteDTDExternalEntityContents: you must call xmlTextWriterStartDTDEntity before the call to this function!\n");
            return -1;
    }

    if (pubid != 0) {
        if (sysid == 0) {
            xmlWriterErrMsg(writer, XML_ERR_INTERNAL_ERROR,
                            "xmlTextWriterWriteDTDExternalEntityContents: system identifier needed!\n");
            return -1;
        }

        count = xmlOutputBufferWriteString(writer->out, " PUBLIC ");
        if (count < 0)
            return -1;
        sum += count;

        count = xmlOutputBufferWrite(writer->out, 1, &writer->qchar);
        if (count < 0)
            return -1;
        sum += count;

        count =
            xmlOutputBufferWriteString(writer->out, (const char *) pubid);
        if (count < 0)
            return -1;
        sum += count;

        count = xmlOutputBufferWrite(writer->out, 1, &writer->qchar);
        if (count < 0)
            return -1;
        sum += count;
    }

    if (sysid != 0) {
        if (pubid == 0) {
            count = xmlOutputBufferWriteString(writer->out, " SYSTEM");
            if (count < 0)
                return -1;
            sum += count;
        }

        count = xmlOutputBufferWriteString(writer->out, " ");
        if (count < 0)
            return -1;
        sum += count;

        count = xmlOutputBufferWrite(writer->out, 1, &writer->qchar);
        if (count < 0)
            return -1;
        sum += count;

        count =
            xmlOutputBufferWriteString(writer->out, (const char *) sysid);
        if (count < 0)
            return -1;
        sum += count;

        count = xmlOutputBufferWrite(writer->out, 1, &writer->qchar);
        if (count < 0)
            return -1;
        sum += count;
    }

    if (ndataid != NULL) {
        count = xmlOutputBufferWriteString(writer->out, " NDATA ");
        if (count < 0)
            return -1;
        sum += count;

        count =
            xmlOutputBufferWriteString(writer->out,
                                       (const char *) ndataid);
        if (count < 0)
            return -1;
        sum += count;
    }

    return sum;
}

/**
 * Write a DTD entity.
 *
 * @param writer  the xmlTextWriter
 * @param name  the name of the xml notation
 * @param pubid  the public identifier, which is an alternative to the system identifier
 * @param sysid  the system identifier, which is the URI of the DTD
 * @returns the bytes written (may be 0 because of buffering) or -1 in case of error
 */
int
xmlTextWriterWriteDTDNotation(xmlTextWriter *writer,
                              const xmlChar * name,
                              const xmlChar * pubid, const xmlChar * sysid)
{
    int count;
    int sum;
    xmlLinkPtr lk;
    xmlTextWriterStackEntry *p;

    if (writer == NULL || name == NULL || *name == '\0')
        return -1;

    sum = 0;
    lk = xmlListFront(writer->nodes);
    if (lk == 0) {
        return -1;
    }

    p = (xmlTextWriterStackEntry *) xmlLinkGetData(lk);
    if (p != 0) {
        switch (p->state) {
            case XML_TEXTWRITER_DTD:
                count = xmlOutputBufferWriteString(writer->out, " [");
                if (count < 0)
                    return -1;
                sum += count;
                if (writer->indent) {
                    count = xmlOutputBufferWriteString(writer->out, "\n");
                    if (count < 0)
                        return -1;
                    sum += count;
                }
                p->state = XML_TEXTWRITER_DTD_TEXT;
                /* fallthrough */
            case XML_TEXTWRITER_DTD_TEXT:
                break;
            default:
                return -1;
        }
    }

    if (writer->indent) {
        count = xmlTextWriterWriteIndent(writer);
        if (count < 0)
            return -1;
        sum += count;
    }

    count = xmlOutputBufferWriteString(writer->out, "<!NOTATION ");
    if (count < 0)
        return -1;
    sum += count;
    count = xmlOutputBufferWriteString(writer->out, (const char *) name);
    if (count < 0)
        return -1;
    sum += count;

    if (pubid != 0) {
        count = xmlOutputBufferWriteString(writer->out, " PUBLIC ");
        if (count < 0)
            return -1;
        sum += count;
        count = xmlOutputBufferWrite(writer->out, 1, &writer->qchar);
        if (count < 0)
            return -1;
        sum += count;
        count =
            xmlOutputBufferWriteString(writer->out, (const char *) pubid);
        if (count < 0)
            return -1;
        sum += count;
        count = xmlOutputBufferWrite(writer->out, 1, &writer->qchar);
        if (count < 0)
            return -1;
        sum += count;
    }

    if (sysid != 0) {
        if (pubid == 0) {
            count = xmlOutputBufferWriteString(writer->out, " SYSTEM");
            if (count < 0)
                return -1;
            sum += count;
        }
        count = xmlOutputBufferWriteString(writer->out, " ");
        if (count < 0)
            return -1;
        sum += count;
        count = xmlOutputBufferWrite(writer->out, 1, &writer->qchar);
        if (count < 0)
            return -1;
        sum += count;
        count =
            xmlOutputBufferWriteString(writer->out, (const char *) sysid);
        if (count < 0)
            return -1;
        sum += count;
        count = xmlOutputBufferWrite(writer->out, 1, &writer->qchar);
        if (count < 0)
            return -1;
        sum += count;
    }

    count = xmlOutputBufferWriteString(writer->out, ">");
    if (count < 0)
        return -1;
    sum += count;

    return sum;
}

/**
 * Flush the output buffer.
 *
 * @param writer  the xmlTextWriter
 * @returns the bytes written (may be 0 because of buffering) or -1 in case of error
 */
int
xmlTextWriterFlush(xmlTextWriter *writer)
{
    int count;

    if (writer == NULL)
        return -1;

    if (writer->out == NULL)
        count = 0;
    else
        count = xmlOutputBufferFlush(writer->out);

    return count;
}

/**
 * Flushes and closes the output buffer.
 *
 * @since 2.13.0
 *
 * @param writer  the xmlTextWriter
 * @returns an xmlParserErrors code.
 */
int
xmlTextWriterClose(xmlTextWriter *writer)
{
    int result;

    if ((writer == NULL) || (writer->out == NULL))
        return XML_ERR_ARGUMENT;

    result = xmlOutputBufferClose(writer->out);
    writer->out = NULL;

    if (result >= 0)
        result = XML_ERR_OK;
    else
        result = -result;

    return result;
}

/**
 * misc
 */

/**
 * Free callback for the xmlList.
 *
 * @param lk  the xmlLink
 */
static void
xmlFreeTextWriterStackEntry(xmlLinkPtr lk)
{
    xmlTextWriterStackEntry *p;

    p = (xmlTextWriterStackEntry *) xmlLinkGetData(lk);
    if (p == 0)
        return;

    if (p->name != 0)
        xmlFree(p->name);
    xmlFree(p);
}

/**
 * Compare callback for the xmlList.
 *
 * @param data0  the first data
 * @param data1  the second data
 * @returns -1, 0, 1
 */
static int
xmlCmpTextWriterStackEntry(const void *data0, const void *data1)
{
    xmlTextWriterStackEntry *p0;
    xmlTextWriterStackEntry *p1;

    if (data0 == data1)
        return 0;

    if (data0 == 0)
        return -1;

    if (data1 == 0)
        return 1;

    p0 = (xmlTextWriterStackEntry *) data0;
    p1 = (xmlTextWriterStackEntry *) data1;

    return xmlStrcmp(p0->name, p1->name);
}

/**
 * misc
 */

/**
 * Output the current namespace declarations.
 *
 * @param writer  the xmlTextWriter
 */
static int
xmlTextWriterOutputNSDecl(xmlTextWriterPtr writer)
{
    xmlLinkPtr lk;
    xmlTextWriterNsStackEntry *np;
    int count;
    int sum;

    sum = 0;
    while (!xmlListEmpty(writer->nsstack)) {
        xmlChar *namespaceURI = NULL;
        xmlChar *prefix = NULL;

        lk = xmlListFront(writer->nsstack);
        np = (xmlTextWriterNsStackEntry *) xmlLinkGetData(lk);

        if (np != 0) {
            namespaceURI = xmlStrdup(np->uri);
            prefix = xmlStrdup(np->prefix);
        }

        xmlListPopFront(writer->nsstack);

        if (np != 0) {
            count = xmlTextWriterWriteAttribute(writer, prefix, namespaceURI);
            xmlFree(namespaceURI);
            xmlFree(prefix);

            if (count < 0) {
                xmlListDelete(writer->nsstack);
                writer->nsstack = NULL;
                return -1;
            }
            sum += count;
        }
    }
    return sum;
}

/**
 * Free callback for the xmlList.
 *
 * @param lk  the xmlLink
 */
static void
xmlFreeTextWriterNsStackEntry(xmlLinkPtr lk)
{
    xmlTextWriterNsStackEntry *p;

    p = (xmlTextWriterNsStackEntry *) xmlLinkGetData(lk);
    if (p == 0)
        return;

    if (p->prefix != 0)
        xmlFree(p->prefix);
    if (p->uri != 0)
        xmlFree(p->uri);

    xmlFree(p);
}

/**
 * Compare callback for the xmlList.
 *
 * @param data0  the first data
 * @param data1  the second data
 * @returns -1, 0, 1
 */
static int
xmlCmpTextWriterNsStackEntry(const void *data0, const void *data1)
{
    xmlTextWriterNsStackEntry *p0;
    xmlTextWriterNsStackEntry *p1;
    int rc;

    if (data0 == data1)
        return 0;

    if (data0 == 0)
        return -1;

    if (data1 == 0)
        return 1;

    p0 = (xmlTextWriterNsStackEntry *) data0;
    p1 = (xmlTextWriterNsStackEntry *) data1;

    rc = xmlStrcmp(p0->prefix, p1->prefix);

    if ((rc != 0) || (p0->elem != p1->elem))
        rc = -1;

    return rc;
}

/**
 * Write callback for the xmlOutputBuffer with target xmlBuffer
 *
 * @param context  the xmlBuffer
 * @param str  the data to write
 * @param len  the length of the data
 * @returns -1, 0, 1
 */
static int
xmlTextWriterWriteDocCallback(void *context, const char *str, int len)
{
    xmlParserCtxtPtr ctxt = (xmlParserCtxtPtr) context;
    int rc;

    rc = xmlParseChunk(ctxt, str, len, 0);
    if (rc != 0) {
        xmlWriterErrMsgInt(NULL, XML_ERR_INTERNAL_ERROR,
                        "xmlTextWriterWriteDocCallback : XML error %d !\n",
                        rc);
        return -1;
    }

    return len;
}

/**
 * Close callback for the xmlOutputBuffer with target xmlBuffer
 *
 * @param context  the xmlBuffer
 * @returns -1, 0, 1
 */
static int
xmlTextWriterCloseDocCallback(void *context)
{
    xmlParserCtxtPtr ctxt = (xmlParserCtxtPtr) context;
    int rc;

    rc = xmlParseChunk(ctxt, NULL, 0, 1);
    if (rc != 0) {
        xmlWriterErrMsgInt(NULL, XML_ERR_INTERNAL_ERROR,
                        "xmlTextWriterCloseDocCallback : XML error %d !\n",
                        rc);
        return -1;
    }

    return 0;
}

/**
 * Utility function for formatted output
 *
 * @param format  see printf
 * @param argptr  pointer to the first member of the variable argument list.
 * @returns a new xmlChar buffer with the data or NULL on error. This buffer must be freed.
 */
static xmlChar *
xmlTextWriterVSprintf(const char *format, va_list argptr)
{
    int size;
    int count;
    xmlChar *buf;
    va_list locarg;

    size = BUFSIZ;
    buf = (xmlChar *) xmlMalloc(size);
    if (buf == NULL) {
        xmlWriterErrMsg(NULL, XML_ERR_NO_MEMORY,
                        "xmlTextWriterVSprintf : out of memory!\n");
        return NULL;
    }

    va_copy(locarg, argptr);
    while (((count = vsnprintf((char *) buf, size, format, locarg)) < 0)
           || (count == size - 1) || (count == size) || (count > size)) {
	va_end(locarg);
        xmlFree(buf);
        size += BUFSIZ;
        buf = (xmlChar *) xmlMalloc(size);
        if (buf == NULL) {
            xmlWriterErrMsg(NULL, XML_ERR_NO_MEMORY,
                            "xmlTextWriterVSprintf : out of memory!\n");
            return NULL;
        }
	va_copy(locarg, argptr);
    }
    va_end(locarg);

    return buf;
}

/**
 * called at the start of document processing.
 *
 * @param ctx  the user data (XML parser context)
 */
static void
xmlTextWriterStartDocumentCallback(void *ctx)
{
    xmlParserCtxtPtr ctxt = (xmlParserCtxtPtr) ctx;
    xmlDocPtr doc;

#ifdef LIBXML_HTML_ENABLED
    if (ctxt->html) {
        if (ctxt->myDoc == NULL)
            ctxt->myDoc = htmlNewDocNoDtD(NULL, NULL);
        if (ctxt->myDoc == NULL) {
            xmlCtxtErrMemory(ctxt);
            return;
        }
    } else
#endif
    {
        doc = ctxt->myDoc;
        if (doc == NULL)
            doc = ctxt->myDoc = xmlNewDoc(ctxt->version);
        if (doc != NULL) {
            if (doc->children == NULL) {
                if (ctxt->encoding != NULL)
                    doc->encoding = xmlStrdup(ctxt->encoding);
                else
                    doc->encoding = NULL;
                doc->standalone = ctxt->standalone;
            }
        } else {
            xmlCtxtErrMemory(ctxt);
            return;
        }
    }
    if ((ctxt->myDoc != NULL) && (ctxt->myDoc->URL == NULL) &&
        (ctxt->input != NULL) && (ctxt->input->filename != NULL)) {
        ctxt->myDoc->URL =
            xmlCanonicPath((const xmlChar *) ctxt->input->filename);
        if (ctxt->myDoc->URL == NULL)
            ctxt->myDoc->URL =
                xmlStrdup((const xmlChar *) ctxt->input->filename);
    }
}

/**
 * Set indentation output. indent = 0 do not indentation. indent > 0 do indentation.
 *
 * @param writer  the xmlTextWriter
 * @param indent  do indentation?
 * @returns -1 on error or 0 otherwise.
 */
int
xmlTextWriterSetIndent(xmlTextWriter *writer, int indent)
{
    if ((writer == NULL) || (indent < 0))
        return -1;

    writer->indent = indent;
    writer->doindent = 1;

    return 0;
}

/**
 * Set string indentation.
 *
 * @param writer  the xmlTextWriter
 * @param str  the xmlChar string
 * @returns -1 on error or 0 otherwise.
 */
int
xmlTextWriterSetIndentString(xmlTextWriter *writer, const xmlChar * str)
{
    if ((writer == NULL) || (!str))
        return -1;

    if (writer->ichar != NULL)
        xmlFree(writer->ichar);
    writer->ichar = xmlStrdup(str);

    if (!writer->ichar)
        return -1;
    else
        return 0;
}

/**
 * Set the character used for quoting attributes.
 *
 * @param writer  the xmlTextWriter
 * @param quotechar  the quote character
 * @returns -1 on error or 0 otherwise.
 */
int
xmlTextWriterSetQuoteChar(xmlTextWriter *writer, xmlChar quotechar)
{
    if ((writer == NULL) || ((quotechar != '\'') && (quotechar != '"')))
        return -1;

    writer->qchar = quotechar;

    return 0;
}

/**
 * Write indent string.
 *
 * @param writer  the xmlTextWriter
 * @returns -1 on error or the number of strings written.
 */
static int
xmlTextWriterWriteIndent(xmlTextWriterPtr writer)
{
    int lksize;
    int i;
    int ret;

    lksize = xmlListSize(writer->nodes);
    if (lksize < 1)
        return (-1);            /* list is empty */
    for (i = 0; i < (lksize - 1); i++) {
        ret = xmlOutputBufferWriteString(writer->out,
                                         (const char *) writer->ichar);
        if (ret == -1)
            return (-1);
    }

    return (lksize - 1);
}

/**
 * Write state dependent strings.
 *
 * @param writer  the xmlTextWriter
 * @param p  the xmlTextWriterStackEntry
 * @returns -1 on error or the number of characters written.
 */
static int
xmlTextWriterHandleStateDependencies(xmlTextWriterPtr writer,
                                     xmlTextWriterStackEntry * p)
{
    int count;
    int sum;
    char extra[3];

    if (writer == NULL)
        return -1;

    if (p == NULL)
        return 0;

    sum = 0;
    extra[0] = extra[1] = extra[2] = '\0';
    if (p != 0) {
        sum = 0;
        switch (p->state) {
            case XML_TEXTWRITER_NAME:
                /* Output namespace declarations */
                count = xmlTextWriterOutputNSDecl(writer);
                if (count < 0)
                    return -1;
                sum += count;
                extra[0] = '>';
                p->state = XML_TEXTWRITER_TEXT;
                break;
            case XML_TEXTWRITER_PI:
                extra[0] = ' ';
                p->state = XML_TEXTWRITER_PI_TEXT;
                break;
            case XML_TEXTWRITER_DTD:
                extra[0] = ' ';
                extra[1] = '[';
                p->state = XML_TEXTWRITER_DTD_TEXT;
                break;
            case XML_TEXTWRITER_DTD_ELEM:
                extra[0] = ' ';
                p->state = XML_TEXTWRITER_DTD_ELEM_TEXT;
                break;
            case XML_TEXTWRITER_DTD_ATTL:
                extra[0] = ' ';
                p->state = XML_TEXTWRITER_DTD_ATTL_TEXT;
                break;
            case XML_TEXTWRITER_DTD_ENTY:
            case XML_TEXTWRITER_DTD_PENT:
                extra[0] = ' ';
                extra[1] = writer->qchar;
                p->state = XML_TEXTWRITER_DTD_ENTY_TEXT;
                break;
            default:
                break;
        }
    }

    if (*extra != '\0') {
        count = xmlOutputBufferWriteString(writer->out, extra);
        if (count < 0)
            return -1;
        sum += count;
    }

    return sum;
}

#endif

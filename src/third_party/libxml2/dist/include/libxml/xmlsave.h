/**
 * @file
 * 
 * @brief XML/HTML serializer
 * 
 * API to save documents or subtrees of documents.
 *
 * @copyright See Copyright for the status of this software.
 *
 * @author Daniel Veillard
 */

#ifndef __XML_XMLSAVE_H__
#define __XML_XMLSAVE_H__

#include <libxml/xmlversion.h>
#include <libxml/tree.h>
#include <libxml/encoding.h>
#include <libxml/xmlIO.h>

#ifdef LIBXML_OUTPUT_ENABLED
#ifdef __cplusplus
extern "C" {
#endif

/**
 * This is the set of XML save options that can be passed down
 * to the #xmlSaveToFd and similar calls.
 */
typedef enum {
    /**
     * Format output. This adds newlines and enables indenting
     * by default.
     */
    XML_SAVE_FORMAT     = 1<<0,
    /**
     * Don't emit an XML declaration.
     */
    XML_SAVE_NO_DECL    = 1<<1,
    /**
     * Don't emit empty tags.
     */
    XML_SAVE_NO_EMPTY	= 1<<2,
    /**
     * Don't serialize as XHTML.
     */
    XML_SAVE_NO_XHTML	= 1<<3,
    /**
     * Always serialize as XHTML.
     */
    XML_SAVE_XHTML	= 1<<4,
    /**
     * Serialize HTML documents as XML.
     */
    XML_SAVE_AS_XML     = 1<<5,
    /**
     * Serialize XML documents as HTML.
     */
    XML_SAVE_AS_HTML    = 1<<6,
    /**
     * Format with non-significant whitespace.
     * TODO: What does this mean?
     */
    XML_SAVE_WSNONSIG   = 1<<7,
    /**
     * Always emit empty tags. This is the default unless the
     * deprecated thread-local setting xmlSaveNoEmptyTags is
     * set to 1.
     *
     * @since 2.14
     */
    XML_SAVE_EMPTY      = 1<<8,
    /**
     * Don't indent output when formatting.
     *
     * @since 2.14
     */
    XML_SAVE_NO_INDENT  = 1<<9,
    /**
     * Always indent output when formatting. This is the default
     * unless the deprecated thread-local setting
     * xmlIndentTreeOutput is set to 0.
     *
     * @since 2.14
     */
    XML_SAVE_INDENT     = 1<<10
} xmlSaveOption;

/** XML and HTML serializer */
typedef struct _xmlSaveCtxt xmlSaveCtxt;
typedef xmlSaveCtxt *xmlSaveCtxtPtr;

XMLPUBFUN xmlSaveCtxt *
		xmlSaveToFd		(int fd,
					 const char *encoding,
					 int options);
XMLPUBFUN xmlSaveCtxt *
		xmlSaveToFilename	(const char *filename,
					 const char *encoding,
					 int options);

XMLPUBFUN xmlSaveCtxt *
		xmlSaveToBuffer		(xmlBuffer *buffer,
					 const char *encoding,
					 int options);

XMLPUBFUN xmlSaveCtxt *
		xmlSaveToIO		(xmlOutputWriteCallback iowrite,
					 xmlOutputCloseCallback ioclose,
					 void *ioctx,
					 const char *encoding,
					 int options);

XMLPUBFUN long
		xmlSaveDoc		(xmlSaveCtxt *ctxt,
					 xmlDoc *doc);
XMLPUBFUN long
		xmlSaveTree		(xmlSaveCtxt *ctxt,
					 xmlNode *node);

XMLPUBFUN int
		xmlSaveFlush		(xmlSaveCtxt *ctxt);
XMLPUBFUN int
		xmlSaveClose		(xmlSaveCtxt *ctxt);
XMLPUBFUN xmlParserErrors
		xmlSaveFinish		(xmlSaveCtxt *ctxt);
XMLPUBFUN int
		xmlSaveSetIndentString	(xmlSaveCtxt *ctxt,
					 const char *indent);
XML_DEPRECATED
XMLPUBFUN int
		xmlSaveSetEscape	(xmlSaveCtxt *ctxt,
					 xmlCharEncodingOutputFunc escape);
XML_DEPRECATED
XMLPUBFUN int
		xmlSaveSetAttrEscape	(xmlSaveCtxt *ctxt,
					 xmlCharEncodingOutputFunc escape);

XML_DEPRECATED
XMLPUBFUN int
                xmlThrDefIndentTreeOutput(int v);
XML_DEPRECATED
XMLPUBFUN const char *
                xmlThrDefTreeIndentString(const char * v);
XML_DEPRECATED
XMLPUBFUN int
                xmlThrDefSaveNoEmptyTags(int v);

#ifdef __cplusplus
}
#endif

#endif /* LIBXML_OUTPUT_ENABLED */
#endif /* __XML_XMLSAVE_H__ */



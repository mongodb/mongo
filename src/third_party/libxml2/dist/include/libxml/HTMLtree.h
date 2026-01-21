/**
 * @file
 * 
 * @brief HTML documents
 * 
 * This modules implements functions to work with HTML documents,
 * most of them related to serialization.
 *
 * @copyright See Copyright for the status of this software.
 *
 * @author Daniel Veillard
 */

#ifndef __HTML_TREE_H__
#define __HTML_TREE_H__

#include <stdio.h>
#include <libxml/xmlversion.h>
#include <libxml/tree.h>
#include <libxml/HTMLparser.h>

#ifdef LIBXML_HTML_ENABLED

#ifdef __cplusplus
extern "C" {
#endif

/* Deprecated */
/** @cond ignore */
#define HTML_TEXT_NODE		XML_TEXT_NODE
#define HTML_ENTITY_REF_NODE	XML_ENTITY_REF_NODE
#define HTML_COMMENT_NODE	XML_COMMENT_NODE
#define HTML_PRESERVE_NODE	XML_CDATA_SECTION_NODE
#define HTML_PI_NODE		XML_PI_NODE
/** @endcond */

XMLPUBFUN xmlDoc *
		htmlNewDoc		(const xmlChar *URI,
					 const xmlChar *ExternalID);
XMLPUBFUN xmlDoc *
		htmlNewDocNoDtD		(const xmlChar *URI,
					 const xmlChar *ExternalID);
XMLPUBFUN const xmlChar *
		htmlGetMetaEncoding	(xmlDoc *doc);
XMLPUBFUN int
		htmlSetMetaEncoding	(xmlDoc *doc,
					 const xmlChar *encoding);
#ifdef LIBXML_OUTPUT_ENABLED
XMLPUBFUN void
		htmlDocDumpMemory	(xmlDoc *cur,
					 xmlChar **mem,
					 int *size);
XMLPUBFUN void
		htmlDocDumpMemoryFormat	(xmlDoc *cur,
					 xmlChar **mem,
					 int *size,
					 int format);
XMLPUBFUN int
		htmlSaveFile		(const char *filename,
					 xmlDoc *cur);
XMLPUBFUN int
		htmlSaveFileEnc		(const char *filename,
					 xmlDoc *cur,
					 const char *encoding);
XMLPUBFUN int
		htmlSaveFileFormat	(const char *filename,
					 xmlDoc *cur,
					 const char *encoding,
					 int format);
XMLPUBFUN int
		htmlNodeDump		(xmlBuffer *buf,
					 xmlDoc *doc,
					 xmlNode *cur);
XMLPUBFUN int
		htmlDocDump		(FILE *f,
					 xmlDoc *cur);
XMLPUBFUN void
		htmlNodeDumpFile	(FILE *out,
					 xmlDoc *doc,
					 xmlNode *cur);
XMLPUBFUN int
		htmlNodeDumpFileFormat	(FILE *out,
					 xmlDoc *doc,
					 xmlNode *cur,
					 const char *encoding,
					 int format);

XMLPUBFUN void
		htmlNodeDumpOutput	(xmlOutputBuffer *buf,
					 xmlDoc *doc,
					 xmlNode *cur,
					 const char *encoding);
XMLPUBFUN void
		htmlNodeDumpFormatOutput(xmlOutputBuffer *buf,
					 xmlDoc *doc,
					 xmlNode *cur,
					 const char *encoding,
					 int format);
XMLPUBFUN void
		htmlDocContentDumpOutput(xmlOutputBuffer *buf,
					 xmlDoc *cur,
					 const char *encoding);
XMLPUBFUN void
		htmlDocContentDumpFormatOutput(xmlOutputBuffer *buf,
					 xmlDoc *cur,
					 const char *encoding,
					 int format);

#endif /* LIBXML_OUTPUT_ENABLED */

XML_DEPRECATED
XMLPUBFUN int
		htmlIsBooleanAttr	(const xmlChar *name);


#ifdef __cplusplus
}
#endif

#endif /* LIBXML_HTML_ENABLED */

#endif /* __HTML_TREE_H__ */


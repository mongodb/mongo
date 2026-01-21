/**
 * @file
 * 
 * @brief Tree debugging APIs
 * 
 * Interfaces to a set of routines used for debugging the tree
 *              produced by the XML parser.
 *
 * @copyright See Copyright for the status of this software.
 *
 * @author Daniel Veillard
 */

#ifndef __DEBUG_XML__
#define __DEBUG_XML__
#include <stdio.h>
#include <libxml/xmlversion.h>
#include <libxml/tree.h>

#ifdef LIBXML_DEBUG_ENABLED

#include <libxml/xpath.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The standard Dump routines.
 */
XMLPUBFUN void
	xmlDebugDumpString	(FILE *output,
				 const xmlChar *str);
XMLPUBFUN void
	xmlDebugDumpAttr	(FILE *output,
				 xmlAttr *attr,
				 int depth);
XMLPUBFUN void
	xmlDebugDumpAttrList	(FILE *output,
				 xmlAttr *attr,
				 int depth);
XMLPUBFUN void
	xmlDebugDumpOneNode	(FILE *output,
				 xmlNode *node,
				 int depth);
XMLPUBFUN void
	xmlDebugDumpNode	(FILE *output,
				 xmlNode *node,
				 int depth);
XMLPUBFUN void
	xmlDebugDumpNodeList	(FILE *output,
				 xmlNode *node,
				 int depth);
XMLPUBFUN void
	xmlDebugDumpDocumentHead(FILE *output,
				 xmlDoc *doc);
XMLPUBFUN void
	xmlDebugDumpDocument	(FILE *output,
				 xmlDoc *doc);
XMLPUBFUN void
	xmlDebugDumpDTD		(FILE *output,
				 xmlDtd *dtd);
XMLPUBFUN void
	xmlDebugDumpEntities	(FILE *output,
				 xmlDoc *doc);

/****************************************************************
 *								*
 *			Checking routines			*
 *								*
 ****************************************************************/

XMLPUBFUN int
	xmlDebugCheckDocument	(FILE * output,
				 xmlDoc *doc);

#ifdef __cplusplus
}
#endif

#endif /* LIBXML_DEBUG_ENABLED */
#endif /* __DEBUG_XML__ */

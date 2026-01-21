/**
 * @file
 *
 * @brief XPointer framework and schemes
 *
 * API to evaluate XPointer expressions. The following schemes are
 * supported:
 *
 * - element()
 * - xmlns()
 * - xpath1()
 *
 * xpointer() is an alias for the xpath1() scheme. The point and
 * range extensions are not supported.
 *
 * @copyright See Copyright for the status of this software.
 *
 * @author Daniel Veillard
 */

#ifndef __XML_XPTR_H__
#define __XML_XPTR_H__

#include <libxml/xmlversion.h>

#ifdef LIBXML_XPTR_ENABLED

#include <libxml/tree.h>
#include <libxml/xpath.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Functions.
 */
XML_DEPRECATED
XMLPUBFUN xmlXPathContext *
		    xmlXPtrNewContext		(xmlDoc *doc,
						 xmlNode *here,
						 xmlNode *origin);
XMLPUBFUN xmlXPathObject *
		    xmlXPtrEval			(const xmlChar *str,
						 xmlXPathContext *ctx);

#ifdef __cplusplus
}
#endif

#endif /* LIBXML_XPTR_ENABLED */
#endif /* __XML_XPTR_H__ */

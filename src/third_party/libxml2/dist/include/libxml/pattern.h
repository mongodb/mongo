/**
 * @file
 * 
 * @brief pattern expression handling
 * 
 * allows to compile and test pattern expressions for nodes
 *              either in a tree or based on a parser state.
 *
 * @copyright See Copyright for the status of this software.
 *
 * @author Daniel Veillard
 */

#ifndef __XML_PATTERN_H__
#define __XML_PATTERN_H__

#include <libxml/xmlversion.h>
#include <libxml/tree.h>
#include <libxml/dict.h>

#ifdef LIBXML_PATTERN_ENABLED

#ifdef __cplusplus
extern "C" {
#endif

/**
 * A compiled (XPath based) pattern to select nodes
 */
typedef struct _xmlPattern xmlPattern;
typedef xmlPattern *xmlPatternPtr;

/**
 * Internal type. This is the set of options affecting the behaviour
 * of pattern matching with this module.
 */
typedef enum {
    XML_PATTERN_DEFAULT		= 0,	/* simple pattern match */
    XML_PATTERN_XPATH		= 1<<0,	/* standard XPath pattern */
    XML_PATTERN_XSSEL		= 1<<1,	/* XPath subset for schema selector */
    XML_PATTERN_XSFIELD		= 1<<2	/* XPath subset for schema field */
} xmlPatternFlags;

XMLPUBFUN void
			xmlFreePattern		(xmlPattern *comp);

XMLPUBFUN void
			xmlFreePatternList	(xmlPattern *comp);

XMLPUBFUN xmlPattern *
			xmlPatterncompile	(const xmlChar *pattern,
						 xmlDict *dict,
						 int flags,
						 const xmlChar **namespaces);
XMLPUBFUN int
			xmlPatternCompileSafe	(const xmlChar *pattern,
						 xmlDict *dict,
						 int flags,
						 const xmlChar **namespaces,
						 xmlPattern **patternOut);
XMLPUBFUN int
			xmlPatternMatch		(xmlPattern *comp,
						 xmlNode *node);

/** State object for streaming interface */
typedef struct _xmlStreamCtxt xmlStreamCtxt;
typedef xmlStreamCtxt *xmlStreamCtxtPtr;

XMLPUBFUN int
			xmlPatternStreamable	(xmlPattern *comp);
XMLPUBFUN int
			xmlPatternMaxDepth	(xmlPattern *comp);
XMLPUBFUN int
			xmlPatternMinDepth	(xmlPattern *comp);
XMLPUBFUN int
			xmlPatternFromRoot	(xmlPattern *comp);
XMLPUBFUN xmlStreamCtxt *
			xmlPatternGetStreamCtxt	(xmlPattern *comp);
XMLPUBFUN void
			xmlFreeStreamCtxt	(xmlStreamCtxt *stream);
XMLPUBFUN int
			xmlStreamPushNode	(xmlStreamCtxt *stream,
						 const xmlChar *name,
						 const xmlChar *ns,
						 int nodeType);
XMLPUBFUN int
			xmlStreamPush		(xmlStreamCtxt *stream,
						 const xmlChar *name,
						 const xmlChar *ns);
XMLPUBFUN int
			xmlStreamPushAttr	(xmlStreamCtxt *stream,
						 const xmlChar *name,
						 const xmlChar *ns);
XMLPUBFUN int
			xmlStreamPop		(xmlStreamCtxt *stream);
XMLPUBFUN int
			xmlStreamWantsAnyNode	(xmlStreamCtxt *stream);
#ifdef __cplusplus
}
#endif

#endif /* LIBXML_PATTERN_ENABLED */

#endif /* __XML_PATTERN_H__ */

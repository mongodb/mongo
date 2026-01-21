/*
 * xlink.c : implementation of the hyperlinks detection module
 *           This version supports both XML XLinks and HTML simple links
 *
 * See Copyright for the status of this software.
 *
 * Author: Daniel Veillard
 */


#define IN_LIBXML
#include "libxml.h"

#ifdef LIBXML_XPTR_ENABLED
#include <string.h> /* for memset() only */
#include <ctype.h>
#include <stdlib.h>

#include <libxml/xmlmemory.h>
#include <libxml/tree.h>
#include <libxml/parser.h>
#include <libxml/xlink.h>

#define XLINK_NAMESPACE (BAD_CAST "http://www.w3.org/1999/xlink/namespace/")
#define XHTML_NAMESPACE (BAD_CAST "http://www.w3.org/1999/xhtml/")

/****************************************************************
 *								*
 *           Default setting and related functions		*
 *								*
 ****************************************************************/

static xlinkHandlerPtr xlinkDefaultHandler = NULL;
static xlinkNodeDetectFunc	xlinkDefaultDetect = NULL;

/**
 * Get the default xlink handler.
 *
 * @deprecated Don't use.
 *
 * @returns the current xlinkHandler value.
 */
xlinkHandler *
xlinkGetDefaultHandler(void) {
    return(xlinkDefaultHandler);
}


/**
 * Set the default xlink handlers
 *
 * @deprecated Don't use.
 *
 * @param handler  the new value for the xlink handler block
 */
void
xlinkSetDefaultHandler(xlinkHandler *handler) {
    xlinkDefaultHandler = handler;
}

/**
 * Get the default xlink detection routine
 *
 * @deprecated Don't use.
 *
 * @returns the current function or NULL;
 */
xlinkNodeDetectFunc
xlinkGetDefaultDetect	(void) {
    return(xlinkDefaultDetect);
}

/**
 * Set the default xlink detection routine
 *
 * @deprecated Don't use.
 *
 * @param func  pointer to the new detection routine.
 */
void
xlinkSetDefaultDetect	(xlinkNodeDetectFunc func) {
    xlinkDefaultDetect = func;
}

/****************************************************************
 *								*
 *                  The detection routines			*
 *								*
 ****************************************************************/


/**
 * Check whether the given node carries the attributes needed
 * to be a link element (or is one of the linking elements issued
 * from the (X)HTML DtDs).
 * This routine don't try to do full checking of the link validity
 * but tries to detect and return the appropriate link type.
 *
 * @deprecated The XLink code was never finished.
 *
 * @param doc  the document containing the node
 * @param node  the node pointer itself
 * @returns the xlinkType of the node (XLINK_TYPE_NONE if there is no
 *         link detected.
 */
xlinkType
xlinkIsLink	(xmlDoc *doc, xmlNode *node) {
    xmlChar *type = NULL, *role = NULL;
    xlinkType ret = XLINK_TYPE_NONE;

    if (node == NULL) return(XLINK_TYPE_NONE);
    if (doc == NULL) doc = node->doc;
    if ((doc != NULL) && (doc->type == XML_HTML_DOCUMENT_NODE)) {
        /*
	 * This is an HTML document.
	 */
    } else if ((node->ns != NULL) &&
               (xmlStrEqual(node->ns->href, XHTML_NAMESPACE))) {
	/*
	 * !!!! We really need an IS_XHTML_ELEMENT function from HTMLtree.h @@@
	 */
        /*
	 * This is an XHTML element within an XML document
	 * Check whether it's one of the element able to carry links
	 * and in that case if it holds the attributes.
	 */
    }

    /*
     * We don't prevent a-priori having XML Linking constructs on
     * XHTML elements
     */
    type = xmlGetNsProp(node, BAD_CAST"type", XLINK_NAMESPACE);
    if (type != NULL) {
	if (xmlStrEqual(type, BAD_CAST "simple")) {
            ret = XLINK_TYPE_SIMPLE;
	} else if (xmlStrEqual(type, BAD_CAST "extended")) {
	    role = xmlGetNsProp(node, BAD_CAST "role", XLINK_NAMESPACE);
	    if (role != NULL) {
		xmlNsPtr xlink;
		xlink = xmlSearchNs(doc, node, XLINK_NAMESPACE);
		if (xlink == NULL) {
		    /* Humm, fallback method */
		    if (xmlStrEqual(role, BAD_CAST"xlink:external-linkset"))
			ret = XLINK_TYPE_EXTENDED_SET;
		} else {
		    xmlChar buf[200];
		    snprintf((char *) buf, sizeof(buf), "%s:external-linkset",
			     (char *) xlink->prefix);
                    buf[sizeof(buf) - 1] = 0;
		    if (xmlStrEqual(role, buf))
			ret = XLINK_TYPE_EXTENDED_SET;

		}

	    }
	    ret = XLINK_TYPE_EXTENDED;
	}
    }

    if (type != NULL) xmlFree(type);
    if (role != NULL) xmlFree(role);
    return(ret);
}
#endif /* LIBXML_XPTR_ENABLED */

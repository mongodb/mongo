/*
 * xinclude.c : Code to implement XInclude processing
 *
 * World Wide Web Consortium W3C Last Call Working Draft 10 November 2003
 * http://www.w3.org/TR/2003/WD-xinclude-20031110
 *
 * See Copyright for the status of this software.
 *
 * Author: Daniel Veillard
 */

#define IN_LIBXML
#include "libxml.h"

#include <string.h>
#include <libxml/xmlmemory.h>
#include <libxml/tree.h>
#include <libxml/parser.h>
#include <libxml/uri.h>
#include <libxml/xpath.h>
#include <libxml/xpointer.h>
#include <libxml/parserInternals.h>
#include <libxml/xmlerror.h>
#include <libxml/encoding.h>

#ifdef LIBXML_XINCLUDE_ENABLED
#include <libxml/xinclude.h>

#include "private/buf.h"
#include "private/error.h"
#include "private/memory.h"
#include "private/parser.h"
#include "private/tree.h"
#include "private/xinclude.h"

#define XINCLUDE_MAX_DEPTH 40

/************************************************************************
 *									*
 *			XInclude context handling			*
 *									*
 ************************************************************************/

/*
 * An XInclude context
 */
typedef xmlChar *xmlURL;

typedef struct _xmlXIncludeRef xmlXIncludeRef;
typedef xmlXIncludeRef *xmlXIncludeRefPtr;
struct _xmlXIncludeRef {
    xmlChar              *URI; /* the fully resolved resource URL */
    xmlChar         *fragment; /* the fragment in the URI */
    xmlChar             *base; /* base URI of xi:include element */
    xmlNodePtr           elem; /* the xi:include element */
    xmlNodePtr            inc; /* the included copy */
    int                   xml; /* xml or txt */
    int	             fallback; /* fallback was loaded */
    int		    expanding; /* flag to detect inclusion loops */
    int		      replace; /* should the node be replaced? */
};

typedef struct _xmlXIncludeDoc xmlXIncludeDoc;
typedef xmlXIncludeDoc *xmlXIncludeDocPtr;
struct _xmlXIncludeDoc {
    xmlDocPtr             doc; /* the parsed document */
    xmlChar              *url; /* the URL */
    int             expanding; /* flag to detect inclusion loops */
};

typedef struct _xmlXIncludeTxt xmlXIncludeTxt;
typedef xmlXIncludeTxt *xmlXIncludeTxtPtr;
struct _xmlXIncludeTxt {
    xmlChar		*text; /* text string */
    xmlChar              *url; /* the URL */
};

struct _xmlXIncludeCtxt {
    xmlDocPtr             doc; /* the source document */
    int                 incNr; /* number of includes */
    int                incMax; /* size of includes tab */
    xmlXIncludeRefPtr *incTab; /* array of included references */

    int                 txtNr; /* number of unparsed documents */
    int                txtMax; /* size of unparsed documents tab */
    xmlXIncludeTxt    *txtTab; /* array of unparsed documents */

    int                 urlNr; /* number of documents stacked */
    int                urlMax; /* size of document stack */
    xmlXIncludeDoc    *urlTab; /* document stack */

    int              nbErrors; /* the number of errors detected */
    int              fatalErr; /* abort processing */
    int                 errNo; /* error code */
    int                legacy; /* using XINCLUDE_OLD_NS */
    int            parseFlags; /* the flags used for parsing XML documents */

    void            *_private; /* application data */

#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
    unsigned long    incTotal; /* total number of processed inclusions */
#endif
    int			depth; /* recursion depth */
    int		     isStream; /* streaming mode */

#ifdef LIBXML_XPTR_ENABLED
    xmlXPathContextPtr xpctxt;
#endif

    xmlStructuredErrorFunc errorHandler;
    void *errorCtxt;

    xmlResourceLoader resourceLoader;
    void *resourceCtxt;
};

static xmlXIncludeRefPtr
xmlXIncludeExpandNode(xmlXIncludeCtxtPtr ctxt, xmlNodePtr node);

static int
xmlXIncludeLoadNode(xmlXIncludeCtxtPtr ctxt, xmlXIncludeRefPtr ref);

static int
xmlXIncludeDoProcess(xmlXIncludeCtxtPtr ctxt, xmlNodePtr tree);


/************************************************************************
 *									*
 *			XInclude error handler				*
 *									*
 ************************************************************************/

/**
 * Handle an out of memory condition
 *
 * @param ctxt  an XInclude context
 */
static void
xmlXIncludeErrMemory(xmlXIncludeCtxtPtr ctxt)
{
    ctxt->errNo = XML_ERR_NO_MEMORY;
    ctxt->fatalErr = 1;
    ctxt->nbErrors++;

    xmlRaiseMemoryError(ctxt->errorHandler, NULL, ctxt->errorCtxt,
                        XML_FROM_XINCLUDE, NULL);
}

/**
 * Handle an XInclude error
 *
 * @param ctxt  the XInclude context
 * @param node  the context node
 * @param error  the error code
 * @param msg  the error message
 * @param extra  extra information
 */
static void LIBXML_ATTR_FORMAT(4,0)
xmlXIncludeErr(xmlXIncludeCtxtPtr ctxt, xmlNodePtr node, int error,
               const char *msg, const xmlChar *extra)
{
    xmlStructuredErrorFunc schannel = NULL;
    xmlGenericErrorFunc channel = NULL;
    void *data = NULL;
    int res;

    if (error == XML_ERR_NO_MEMORY) {
        xmlXIncludeErrMemory(ctxt);
        return;
    }

    if (ctxt->fatalErr != 0)
        return;
    ctxt->nbErrors++;

    schannel = ctxt->errorHandler;
    data = ctxt->errorCtxt;

    if (schannel == NULL) {
        channel = xmlGenericError;
        data = xmlGenericErrorContext;
    }

    res = xmlRaiseError(schannel, channel, data, ctxt, node,
                        XML_FROM_XINCLUDE, error, XML_ERR_ERROR,
                        NULL, 0, (const char *) extra, NULL, NULL, 0, 0,
                        msg, (const char *) extra);
    if (res < 0) {
        ctxt->errNo = XML_ERR_NO_MEMORY;
        ctxt->fatalErr = 1;
    } else {
        ctxt->errNo = error;
        /*
         * Note that we treat IO errors except ENOENT as fatal
         * although the XInclude spec could be interpreted in a
         * way that at least some IO errors should be handled
         * gracefully.
         */
        if (xmlIsCatastrophicError(XML_ERR_FATAL, error))
            ctxt->fatalErr = 1;
    }
}

/**
 * Get an XInclude attribute
 *
 * @param ctxt  the XInclude context
 * @param cur  the node
 * @param name  the attribute name
 * @returns the value (to be freed) or NULL if not found
 */
static xmlChar *
xmlXIncludeGetProp(xmlXIncludeCtxtPtr ctxt, xmlNodePtr cur,
                   const xmlChar *name) {
    xmlChar *ret;

    if (xmlNodeGetAttrValue(cur, name, XINCLUDE_NS, &ret) < 0)
        xmlXIncludeErrMemory(ctxt);
    if (ret != NULL)
        return(ret);

    if (ctxt->legacy != 0) {
        if (xmlNodeGetAttrValue(cur, name, XINCLUDE_OLD_NS, &ret) < 0)
            xmlXIncludeErrMemory(ctxt);
        if (ret != NULL)
            return(ret);
    }

    if (xmlNodeGetAttrValue(cur, name, NULL, &ret) < 0)
        xmlXIncludeErrMemory(ctxt);
    return(ret);
}
/**
 * Free an XInclude reference
 *
 * @param ref  the XInclude reference
 */
static void
xmlXIncludeFreeRef(xmlXIncludeRefPtr ref) {
    if (ref == NULL)
	return;
    if (ref->URI != NULL)
	xmlFree(ref->URI);
    if (ref->fragment != NULL)
	xmlFree(ref->fragment);
    if (ref->base != NULL)
	xmlFree(ref->base);
    xmlFree(ref);
}

/**
 * Creates a new XInclude context
 *
 * @param doc  an XML Document
 * @returns the new set
 */
xmlXIncludeCtxt *
xmlXIncludeNewContext(xmlDoc *doc) {
    xmlXIncludeCtxtPtr ret;

    if (doc == NULL)
	return(NULL);
    ret = (xmlXIncludeCtxtPtr) xmlMalloc(sizeof(xmlXIncludeCtxt));
    if (ret == NULL)
	return(NULL);
    memset(ret, 0, sizeof(xmlXIncludeCtxt));
    ret->doc = doc;
    ret->incNr = 0;
    ret->incMax = 0;
    ret->incTab = NULL;
    ret->nbErrors = 0;
    return(ret);
}

/**
 * Free an XInclude context
 *
 * @param ctxt  the XInclude context
 */
void
xmlXIncludeFreeContext(xmlXIncludeCtxt *ctxt) {
    int i;

    if (ctxt == NULL)
	return;
    if (ctxt->urlTab != NULL) {
	for (i = 0; i < ctxt->urlNr; i++) {
	    xmlFreeDoc(ctxt->urlTab[i].doc);
	    xmlFree(ctxt->urlTab[i].url);
	}
	xmlFree(ctxt->urlTab);
    }
    for (i = 0;i < ctxt->incNr;i++) {
	if (ctxt->incTab[i] != NULL)
	    xmlXIncludeFreeRef(ctxt->incTab[i]);
    }
    if (ctxt->incTab != NULL)
	xmlFree(ctxt->incTab);
    if (ctxt->txtTab != NULL) {
	for (i = 0;i < ctxt->txtNr;i++) {
	    xmlFree(ctxt->txtTab[i].text);
	    xmlFree(ctxt->txtTab[i].url);
	}
	xmlFree(ctxt->txtTab);
    }
#ifdef LIBXML_XPTR_ENABLED
    if (ctxt->xpctxt != NULL)
	xmlXPathFreeContext(ctxt->xpctxt);
#endif
    xmlFree(ctxt);
}

/**
 * parse a document for XInclude
 *
 * @param ctxt  the XInclude context
 * @param URL  the URL or file path
 */
static xmlDocPtr
xmlXIncludeParseFile(xmlXIncludeCtxtPtr ctxt, const char *URL) {
    xmlDocPtr ret = NULL;
    xmlParserCtxtPtr pctxt;
    xmlParserInputPtr inputStream;

    xmlInitParser();

    pctxt = xmlNewParserCtxt();
    if (pctxt == NULL) {
	xmlXIncludeErrMemory(ctxt);
	return(NULL);
    }
    if (ctxt->errorHandler != NULL)
        xmlCtxtSetErrorHandler(pctxt, ctxt->errorHandler, ctxt->errorCtxt);
    if (ctxt->resourceLoader != NULL)
        xmlCtxtSetResourceLoader(pctxt, ctxt->resourceLoader,
                                 ctxt->resourceCtxt);

    /*
     * pass in the application data to the parser context.
     */
    pctxt->_private = ctxt->_private;

    /*
     * try to ensure that new documents included are actually
     * built with the same dictionary as the including document.
     */
    if ((ctxt->doc != NULL) && (ctxt->doc->dict != NULL)) {
       if (pctxt->dict != NULL)
            xmlDictFree(pctxt->dict);
	pctxt->dict = ctxt->doc->dict;
	xmlDictReference(pctxt->dict);
    }

    /*
     * We set DTDLOAD to make sure that ID attributes declared in
     * external DTDs are detected.
     */
    xmlCtxtUseOptions(pctxt, ctxt->parseFlags | XML_PARSE_DTDLOAD);

    inputStream = xmlLoadResource(pctxt, URL, NULL, XML_RESOURCE_XINCLUDE);
    if (inputStream == NULL)
        goto error;

    if (xmlCtxtPushInput(pctxt, inputStream) < 0) {
        xmlFreeInputStream(inputStream);
        goto error;
    }

    xmlParseDocument(pctxt);

    if (pctxt->wellFormed) {
        ret = pctxt->myDoc;
    }
    else {
        ret = NULL;
	if (pctxt->myDoc != NULL)
	    xmlFreeDoc(pctxt->myDoc);
        pctxt->myDoc = NULL;
    }

error:
    if (xmlCtxtIsCatastrophicError(pctxt))
        xmlXIncludeErr(ctxt, NULL, pctxt->errNo, "parser error", NULL);
    xmlFreeParserCtxt(pctxt);

    return(ret);
}

/**
 * Add a new node to process to an XInclude context
 *
 * @param ctxt  the XInclude context
 * @param cur  the new node
 */
static xmlXIncludeRefPtr
xmlXIncludeAddNode(xmlXIncludeCtxtPtr ctxt, xmlNodePtr cur) {
    xmlXIncludeRefPtr ref = NULL;
    xmlXIncludeRefPtr ret = NULL;
    xmlURIPtr uri = NULL;
    xmlChar *href = NULL;
    xmlChar *parse = NULL;
    xmlChar *fragment = NULL;
    xmlChar *base = NULL;
    xmlChar *tmp;
    int xml = 1;
    int local = 0;
    int res;

    if (ctxt == NULL)
	return(NULL);
    if (cur == NULL)
	return(NULL);

    /*
     * read the attributes
     */

    fragment = xmlXIncludeGetProp(ctxt, cur, XINCLUDE_PARSE_XPOINTER);

    href = xmlXIncludeGetProp(ctxt, cur, XINCLUDE_HREF);
    if (href == NULL) {
        if (fragment == NULL) {
	    xmlXIncludeErr(ctxt, cur, XML_XINCLUDE_NO_HREF,
	                   "href or xpointer must be present\n", parse);
	    goto error;
        }

	href = xmlStrdup(BAD_CAST ""); /* @@@@ href is now optional */
	if (href == NULL) {
            xmlXIncludeErrMemory(ctxt);
	    goto error;
        }
    } else if (xmlStrlen(href) > XML_MAX_URI_LENGTH) {
        xmlXIncludeErr(ctxt, cur, XML_XINCLUDE_HREF_URI, "URI too long\n",
                       NULL);
        goto error;
    }

    parse = xmlXIncludeGetProp(ctxt, cur, XINCLUDE_PARSE);
    if (parse != NULL) {
	if (xmlStrEqual(parse, XINCLUDE_PARSE_XML))
	    xml = 1;
	else if (xmlStrEqual(parse, XINCLUDE_PARSE_TEXT))
	    xml = 0;
	else {
	    xmlXIncludeErr(ctxt, cur, XML_XINCLUDE_PARSE_VALUE,
	                   "invalid value %s for 'parse'\n", parse);
	    goto error;
	}
    }

    /*
     * Check the URL and remove any fragment identifier
     */
    res = xmlParseURISafe((const char *)href, &uri);
    if (uri == NULL) {
        if (res < 0)
            xmlXIncludeErrMemory(ctxt);
        else
            xmlXIncludeErr(ctxt, cur, XML_XINCLUDE_HREF_URI,
                           "invalid value href %s\n", href);
        goto error;
    }

    if (uri->fragment != NULL) {
        if (ctxt->legacy != 0) {
	    if (fragment == NULL) {
		fragment = (xmlChar *) uri->fragment;
	    } else {
		xmlFree(uri->fragment);
	    }
	} else {
	    xmlXIncludeErr(ctxt, cur, XML_XINCLUDE_FRAGMENT_ID,
       "Invalid fragment identifier in URI %s use the xpointer attribute\n",
                           href);
	    goto error;
	}
	uri->fragment = NULL;
    }
    tmp = xmlSaveUri(uri);
    if (tmp == NULL) {
	xmlXIncludeErrMemory(ctxt);
	goto error;
    }
    xmlFree(href);
    href = tmp;

    /*
     * Resolve URI
     */

    if (xmlNodeGetBaseSafe(ctxt->doc, cur, &base) < 0) {
        xmlXIncludeErrMemory(ctxt);
        goto error;
    }

    if (href[0] != 0) {
        if (xmlBuildURISafe(href, base, &tmp) < 0) {
            xmlXIncludeErrMemory(ctxt);
            goto error;
        }
        if (tmp == NULL) {
            xmlXIncludeErr(ctxt, cur, XML_XINCLUDE_HREF_URI,
                           "failed build URL\n", NULL);
            goto error;
        }
        xmlFree(href);
        href = tmp;

        if (xmlStrEqual(href, ctxt->doc->URL))
            local = 1;
    } else {
        local = 1;
    }

    /*
     * If local and xml then we need a fragment
     */
    if ((local == 1) && (xml == 1) &&
        ((fragment == NULL) || (fragment[0] == 0))) {
	xmlXIncludeErr(ctxt, cur, XML_XINCLUDE_RECURSION,
	               "detected a local recursion with no xpointer in %s\n",
		       href);
	goto error;
    }

    ref = (xmlXIncludeRefPtr) xmlMalloc(sizeof(xmlXIncludeRef));
    if (ref == NULL) {
        xmlXIncludeErrMemory(ctxt);
        goto error;
    }
    memset(ref, 0, sizeof(xmlXIncludeRef));

    ref->elem = cur;
    ref->xml = xml;
    ref->URI = href;
    href = NULL;
    ref->fragment = fragment;
    fragment = NULL;

    /*
     * xml:base fixup
     */
    if (((ctxt->parseFlags & XML_PARSE_NOBASEFIX) == 0) &&
        (cur->doc != NULL) &&
        ((cur->doc->parseFlags & XML_PARSE_NOBASEFIX) == 0)) {
        if (base != NULL) {
            ref->base = base;
            base = NULL;
        } else {
            ref->base = xmlStrdup(BAD_CAST "");
            if (ref->base == NULL) {
	        xmlXIncludeErrMemory(ctxt);
                goto error;
            }
        }
    }

    if (ctxt->incNr >= ctxt->incMax) {
        xmlXIncludeRefPtr *table;
        int newSize;

        newSize = xmlGrowCapacity(ctxt->incMax, sizeof(table[0]),
                                  4, XML_MAX_ITEMS);
        if (newSize < 0) {
	    xmlXIncludeErrMemory(ctxt);
	    goto error;
	}
        table = xmlRealloc(ctxt->incTab, newSize * sizeof(table[0]));
        if (table == NULL) {
	    xmlXIncludeErrMemory(ctxt);
	    goto error;
	}
        ctxt->incTab = table;
        ctxt->incMax = newSize;
    }
    ctxt->incTab[ctxt->incNr++] = ref;

    ret = ref;
    ref = NULL;

error:
    xmlXIncludeFreeRef(ref);
    xmlFreeURI(uri);
    xmlFree(href);
    xmlFree(parse);
    xmlFree(fragment);
    xmlFree(base);
    return(ret);
}

/**
 * The XInclude recursive nature is handled at this point.
 *
 * @param ctxt  the XInclude context
 * @param doc  the new document
 */
static void
xmlXIncludeRecurseDoc(xmlXIncludeCtxtPtr ctxt, xmlDocPtr doc) {
    xmlDocPtr oldDoc;
    xmlXIncludeRefPtr *oldIncTab;
    int oldIncMax, oldIncNr, oldIsStream;
    int i;

    oldDoc = ctxt->doc;
    oldIncMax = ctxt->incMax;
    oldIncNr = ctxt->incNr;
    oldIncTab = ctxt->incTab;
    oldIsStream = ctxt->isStream;
    ctxt->doc = doc;
    ctxt->incMax = 0;
    ctxt->incNr = 0;
    ctxt->incTab = NULL;
    ctxt->isStream = 0;

    xmlXIncludeDoProcess(ctxt, xmlDocGetRootElement(doc));

    if (ctxt->incTab != NULL) {
        for (i = 0; i < ctxt->incNr; i++)
            xmlXIncludeFreeRef(ctxt->incTab[i]);
        xmlFree(ctxt->incTab);
    }

    ctxt->doc = oldDoc;
    ctxt->incMax = oldIncMax;
    ctxt->incNr = oldIncNr;
    ctxt->incTab = oldIncTab;
    ctxt->isStream = oldIsStream;
}

/************************************************************************
 *									*
 *			Node copy with specific semantic		*
 *									*
 ************************************************************************/

static void
xmlXIncludeBaseFixup(xmlXIncludeCtxtPtr ctxt, xmlNodePtr cur, xmlNodePtr copy,
                     const xmlChar *targetBase) {
    xmlChar *base = NULL;
    xmlChar *relBase = NULL;
    xmlNs ns;
    int res;

    if (cur->type != XML_ELEMENT_NODE)
        return;

    if (xmlNodeGetBaseSafe(cur->doc, cur, &base) < 0)
        xmlXIncludeErrMemory(ctxt);

    if ((base != NULL) && !xmlStrEqual(base, targetBase)) {
        if ((xmlStrlen(base) > XML_MAX_URI_LENGTH) ||
            (xmlStrlen(targetBase) > XML_MAX_URI_LENGTH)) {
            relBase = xmlStrdup(base);
            if (relBase == NULL) {
                xmlXIncludeErrMemory(ctxt);
                goto done;
            }
        } else if (xmlBuildRelativeURISafe(base, targetBase, &relBase) < 0) {
            xmlXIncludeErrMemory(ctxt);
            goto done;
        }
        if (relBase == NULL) {
            xmlXIncludeErr(ctxt, cur,
                    XML_XINCLUDE_HREF_URI,
                    "Building relative URI failed: %s\n",
                    base);
            goto done;
        }

        /*
         * If the new base doesn't contain a slash, it can be omitted.
         */
        if (xmlStrchr(relBase, '/') != NULL) {
            res = xmlNodeSetBase(copy, relBase);
            if (res < 0)
                xmlXIncludeErrMemory(ctxt);
            goto done;
        }
    }

    /*
     * Delete existing xml:base if bases are equal
     */
    memset(&ns, 0, sizeof(ns));
    ns.href = XML_XML_NAMESPACE;
    xmlUnsetNsProp(copy, &ns, BAD_CAST "base");

done:
    xmlFree(base);
    xmlFree(relBase);
}

/**
 * Make a copy of the node while expanding nested XIncludes.
 *
 * @param ctxt  the XInclude context
 * @param elem  the element
 * @param copyChildren  copy children instead of node if true
 * @param targetBase  the xml:base of the target node
 * @returns a node list, not a single node.
 */
static xmlNodePtr
xmlXIncludeCopyNode(xmlXIncludeCtxtPtr ctxt, xmlNodePtr elem,
                    int copyChildren, const xmlChar *targetBase) {
    xmlNodePtr result = NULL;
    xmlNodePtr insertParent = NULL;
    xmlNodePtr insertLast = NULL;
    xmlNodePtr cur;
    xmlNodePtr item;
    int depth = 0;

    if (copyChildren) {
        cur = elem->children;
        if (cur == NULL)
            return(NULL);
    } else {
        cur = elem;
    }

    while (1) {
        xmlNodePtr copy = NULL;
        int recurse = 0;

        if ((cur->type == XML_DOCUMENT_NODE) ||
            (cur->type == XML_DTD_NODE)) {
            ;
        } else if ((cur->type == XML_ELEMENT_NODE) &&
                   (cur->ns != NULL) &&
                   (xmlStrEqual(cur->name, XINCLUDE_NODE)) &&
                   ((xmlStrEqual(cur->ns->href, XINCLUDE_NS)) ||
                    (xmlStrEqual(cur->ns->href, XINCLUDE_OLD_NS)))) {
            xmlXIncludeRefPtr ref = xmlXIncludeExpandNode(ctxt, cur);

            if (ref == NULL)
                goto error;
            /*
             * TODO: Insert XML_XINCLUDE_START and XML_XINCLUDE_END nodes
             */
            for (item = ref->inc; item != NULL; item = item->next) {
                copy = xmlStaticCopyNode(item, ctxt->doc, insertParent, 1);
                if (copy == NULL) {
                    xmlXIncludeErrMemory(ctxt);
                    goto error;
                }

                if (result == NULL)
                    result = copy;
                if (insertLast != NULL) {
                    insertLast->next = copy;
                    copy->prev = insertLast;
                } else if (insertParent != NULL) {
                    insertParent->children = copy;
                }
                insertLast = copy;

                if ((depth == 0) && (targetBase != NULL))
                    xmlXIncludeBaseFixup(ctxt, item, copy, targetBase);
            }
        } else {
            copy = xmlStaticCopyNode(cur, ctxt->doc, insertParent, 2);
            if (copy == NULL) {
                xmlXIncludeErrMemory(ctxt);
                goto error;
            }

            if (result == NULL)
                result = copy;
            if (insertLast != NULL) {
                insertLast->next = copy;
                copy->prev = insertLast;
            } else if (insertParent != NULL) {
                insertParent->children = copy;
            }
            insertLast = copy;

            if ((depth == 0) && (targetBase != NULL))
                xmlXIncludeBaseFixup(ctxt, cur, copy, targetBase);

            recurse = (cur->type != XML_ENTITY_REF_NODE) &&
                      (cur->children != NULL);
        }

        if (recurse) {
            cur = cur->children;
            insertParent = insertLast;
            insertLast = NULL;
            depth += 1;
            continue;
        }

        if (cur == elem)
            return(result);

        while (cur->next == NULL) {
            if (insertParent != NULL)
                insertParent->last = insertLast;
            cur = cur->parent;
            if (cur == elem)
                return(result);
            insertLast = insertParent;
            insertParent = insertParent->parent;
            depth -= 1;
        }

        cur = cur->next;
    }

error:
    xmlFreeNodeList(result);
    return(NULL);
}

#ifdef LIBXML_XPTR_ENABLED
/**
 * Build a node list tree copy of the XPointer result.
 * This will drop Attributes and Namespace declarations.
 *
 * @param ctxt  the XInclude context
 * @param obj  the XPointer result from the evaluation.
 * @param targetBase  the xml:base of the target node
 * @returns an xmlNode list or NULL.
 *         the caller has to free the node tree.
 */
static xmlNodePtr
xmlXIncludeCopyXPointer(xmlXIncludeCtxtPtr ctxt, xmlXPathObjectPtr obj,
                        const xmlChar *targetBase) {
    xmlNodePtr list = NULL, last = NULL, copy;
    int i;

    if ((ctxt == NULL) || (obj == NULL))
	return(NULL);
    switch (obj->type) {
        case XPATH_NODESET: {
	    xmlNodeSetPtr set = obj->nodesetval;
	    if (set == NULL)
		break;
	    for (i = 0;i < set->nodeNr;i++) {
                xmlNodePtr node;

		if (set->nodeTab[i] == NULL)
		    continue;
		switch (set->nodeTab[i]->type) {
		    case XML_DOCUMENT_NODE:
		    case XML_HTML_DOCUMENT_NODE:
                        node = xmlDocGetRootElement(
                                (xmlDocPtr) set->nodeTab[i]);
                        if (node == NULL) {
                            xmlXIncludeErr(ctxt, set->nodeTab[i],
                                           XML_ERR_INTERNAL_ERROR,
                                          "document without root\n", NULL);
                            continue;
                        }
                        break;
                    case XML_TEXT_NODE:
		    case XML_CDATA_SECTION_NODE:
		    case XML_ELEMENT_NODE:
		    case XML_PI_NODE:
		    case XML_COMMENT_NODE:
                        node = set->nodeTab[i];
			break;
                    default:
                        xmlXIncludeErr(ctxt, set->nodeTab[i],
                                       XML_XINCLUDE_XPTR_RESULT,
                                       "invalid node type in XPtr result\n",
                                       NULL);
			continue; /* for */
		}
                /*
                 * OPTIMIZE TODO: External documents should already be
                 * expanded, so xmlDocCopyNode should work as well.
                 * xmlXIncludeCopyNode is only required for the initial
                 * document.
                 */
		copy = xmlXIncludeCopyNode(ctxt, node, 0, targetBase);
                if (copy == NULL) {
                    xmlFreeNodeList(list);
                    return(NULL);
                }
		if (last == NULL) {
                    list = copy;
                } else {
                    while (last->next != NULL)
                        last = last->next;
                    copy->prev = last;
                    last->next = copy;
		}
                last = copy;
	    }
	    break;
	}
	default:
	    break;
    }
    return(list);
}
#endif

/************************************************************************
 *									*
 *			XInclude I/O handling				*
 *									*
 ************************************************************************/

typedef struct _xmlXIncludeMergeData xmlXIncludeMergeData;
typedef xmlXIncludeMergeData *xmlXIncludeMergeDataPtr;
struct _xmlXIncludeMergeData {
    xmlDocPtr doc;
    xmlXIncludeCtxtPtr ctxt;
};

/**
 * Implements the merge of one entity
 *
 * @param payload  the entity
 * @param vdata  the merge data
 * @param name  unused
 */
static void
xmlXIncludeMergeEntity(void *payload, void *vdata,
	               const xmlChar *name ATTRIBUTE_UNUSED) {
    xmlEntityPtr ent = (xmlEntityPtr) payload;
    xmlXIncludeMergeDataPtr data = (xmlXIncludeMergeDataPtr) vdata;
    xmlEntityPtr ret, prev;
    xmlDocPtr doc;
    xmlXIncludeCtxtPtr ctxt;

    if ((ent == NULL) || (data == NULL))
	return;
    ctxt = data->ctxt;
    doc = data->doc;
    if ((ctxt == NULL) || (doc == NULL))
	return;
    switch (ent->etype) {
        case XML_INTERNAL_PARAMETER_ENTITY:
        case XML_EXTERNAL_PARAMETER_ENTITY:
        case XML_INTERNAL_PREDEFINED_ENTITY:
	    return;
        case XML_INTERNAL_GENERAL_ENTITY:
        case XML_EXTERNAL_GENERAL_PARSED_ENTITY:
        case XML_EXTERNAL_GENERAL_UNPARSED_ENTITY:
	    break;
    }
    prev = xmlGetDocEntity(doc, ent->name);
    if (prev == NULL) {
        ret = xmlAddDocEntity(doc, ent->name, ent->etype, ent->ExternalID,
                              ent->SystemID, ent->content);
        if (ret == NULL) {
            xmlXIncludeErrMemory(ctxt);
            return;
        }
	if (ent->URI != NULL) {
	    ret->URI = xmlStrdup(ent->URI);
            if (ret->URI == 0)
                xmlXIncludeErrMemory(ctxt);
        }
    } else {
        if (ent->etype != prev->etype)
            goto error;

        if ((ent->SystemID != NULL) && (prev->SystemID != NULL)) {
            if (!xmlStrEqual(ent->SystemID, prev->SystemID))
                goto error;
        } else if ((ent->ExternalID != NULL) &&
                   (prev->ExternalID != NULL)) {
            if (!xmlStrEqual(ent->ExternalID, prev->ExternalID))
                goto error;
        } else if ((ent->content != NULL) && (prev->content != NULL)) {
            if (!xmlStrEqual(ent->content, prev->content))
                goto error;
        } else {
            goto error;
        }
    }
    return;
error:
    switch (ent->etype) {
        case XML_INTERNAL_PARAMETER_ENTITY:
        case XML_EXTERNAL_PARAMETER_ENTITY:
        case XML_INTERNAL_PREDEFINED_ENTITY:
        case XML_INTERNAL_GENERAL_ENTITY:
        case XML_EXTERNAL_GENERAL_PARSED_ENTITY:
	    return;
        case XML_EXTERNAL_GENERAL_UNPARSED_ENTITY:
	    break;
    }
    xmlXIncludeErr(ctxt, (xmlNodePtr) ent, XML_XINCLUDE_ENTITY_DEF_MISMATCH,
                   "mismatch in redefinition of entity %s\n",
		   ent->name);
}

/**
 * Implements the entity merge
 *
 * @param ctxt  an XInclude context
 * @param doc  the including doc
 * @param from  the included doc
 * @returns 0 if merge succeeded, -1 if some processing failed
 */
static int
xmlXIncludeMergeEntities(xmlXIncludeCtxtPtr ctxt, xmlDocPtr doc,
	                 xmlDocPtr from) {
    xmlNodePtr cur;
    xmlDtdPtr target, source;

    if (ctxt == NULL)
	return(-1);

    if ((from == NULL) || (from->intSubset == NULL))
	return(0);

    target = doc->intSubset;
    if (target == NULL) {
	cur = xmlDocGetRootElement(doc);
	if (cur == NULL)
	    return(-1);
        target = xmlCreateIntSubset(doc, cur->name, NULL, NULL);
	if (target == NULL) {
            xmlXIncludeErrMemory(ctxt);
	    return(-1);
        }
    }

    source = from->intSubset;
    if ((source != NULL) && (source->entities != NULL)) {
	xmlXIncludeMergeData data;

	data.ctxt = ctxt;
	data.doc = doc;

	xmlHashScan((xmlHashTablePtr) source->entities,
		    xmlXIncludeMergeEntity, &data);
    }
    source = from->extSubset;
    if ((source != NULL) && (source->entities != NULL)) {
	xmlXIncludeMergeData data;

	data.ctxt = ctxt;
	data.doc = doc;

	/*
	 * don't duplicate existing stuff when external subsets are the same
	 */
	if ((!xmlStrEqual(target->ExternalID, source->ExternalID)) &&
	    (!xmlStrEqual(target->SystemID, source->SystemID))) {
	    xmlHashScan((xmlHashTablePtr) source->entities,
			xmlXIncludeMergeEntity, &data);
	}
    }
    return(0);
}

/**
 * Load the document, and store the result in the XInclude context
 *
 * @param ctxt  the XInclude context
 * @param ref  an XMLXincludeRefPtr
 * @returns 0 in case of success, -1 in case of failure
 */
static int
xmlXIncludeLoadDoc(xmlXIncludeCtxtPtr ctxt, xmlXIncludeRefPtr ref) {
    xmlXIncludeDocPtr cache;
    xmlDocPtr doc;
    const xmlChar *url = ref->URI;
    const xmlChar *fragment = ref->fragment;
    int i = 0;
    int ret = -1;
    int cacheNr;
#ifdef LIBXML_XPTR_ENABLED
    int saveFlags;
#endif

    /*
     * Handling of references to the local document are done
     * directly through ctxt->doc.
     */
    if ((url[0] == 0) || (url[0] == '#') ||
	((ctxt->doc != NULL) && (xmlStrEqual(url, ctxt->doc->URL)))) {
	doc = ctxt->doc;
        goto loaded;
    }

    /*
     * Prevent reloading the document twice.
     */
    for (i = 0; i < ctxt->urlNr; i++) {
	if (xmlStrEqual(url, ctxt->urlTab[i].url)) {
            if (ctxt->urlTab[i].expanding) {
                xmlXIncludeErr(ctxt, ref->elem, XML_XINCLUDE_RECURSION,
                               "inclusion loop detected\n", NULL);
                goto error;
            }
	    doc = ctxt->urlTab[i].doc;
            if (doc == NULL)
                goto error;
	    goto loaded;
	}
    }

    /*
     * Load it.
     */
#ifdef LIBXML_XPTR_ENABLED
    /*
     * If this is an XPointer evaluation, we want to assure that
     * all entities have been resolved prior to processing the
     * referenced document
     */
    saveFlags = ctxt->parseFlags;
    if (fragment != NULL) {	/* if this is an XPointer eval */
	ctxt->parseFlags |= XML_PARSE_NOENT;
    }
#endif

    doc = xmlXIncludeParseFile(ctxt, (const char *)url);
#ifdef LIBXML_XPTR_ENABLED
    ctxt->parseFlags = saveFlags;
#endif

    /* Also cache NULL docs */
    if (ctxt->urlNr >= ctxt->urlMax) {
        xmlXIncludeDoc *tmp;
        int newSize;

        newSize = xmlGrowCapacity(ctxt->urlMax, sizeof(tmp[0]),
                                  8, XML_MAX_ITEMS);
        if (newSize < 0) {
            xmlXIncludeErrMemory(ctxt);
            xmlFreeDoc(doc);
            goto error;
        }
        tmp = xmlRealloc(ctxt->urlTab, newSize * sizeof(tmp[0]));
        if (tmp == NULL) {
            xmlXIncludeErrMemory(ctxt);
            xmlFreeDoc(doc);
            goto error;
        }
        ctxt->urlMax = newSize;
        ctxt->urlTab = tmp;
    }
    cache = &ctxt->urlTab[ctxt->urlNr];
    cache->doc = doc;
    cache->url = xmlStrdup(url);
    if (cache->url == NULL) {
        xmlXIncludeErrMemory(ctxt);
        xmlFreeDoc(doc);
        goto error;
    }
    cache->expanding = 0;
    cacheNr = ctxt->urlNr++;

    if (doc == NULL)
        goto error;
    /*
     * It's possible that the requested URL has been mapped to a
     * completely different location (e.g. through a catalog entry).
     * To check for this, we compare the URL with that of the doc
     * and change it if they disagree (bug 146988).
     */
    if ((doc->URL != NULL) && (!xmlStrEqual(url, doc->URL)))
        url = doc->URL;

    /*
     * Make sure we have all entities fixed up
     */
    xmlXIncludeMergeEntities(ctxt, ctxt->doc, doc);

    /*
     * We don't need the DTD anymore, free up space
    if (doc->intSubset != NULL) {
	xmlUnlinkNode((xmlNodePtr) doc->intSubset);
	xmlFreeNode((xmlNodePtr) doc->intSubset);
	doc->intSubset = NULL;
    }
    if (doc->extSubset != NULL) {
	xmlUnlinkNode((xmlNodePtr) doc->extSubset);
	xmlFreeNode((xmlNodePtr) doc->extSubset);
	doc->extSubset = NULL;
    }
     */
    cache->expanding = 1;
    xmlXIncludeRecurseDoc(ctxt, doc);
    /* urlTab might be reallocated. */
    cache = &ctxt->urlTab[cacheNr];
    cache->expanding = 0;

loaded:
    if (fragment == NULL) {
        xmlNodePtr root;

        root = xmlDocGetRootElement(doc);
        if (root == NULL) {
            xmlXIncludeErr(ctxt, ref->elem, XML_ERR_INTERNAL_ERROR,
                           "document without root\n", NULL);
            goto error;
        }

        ref->inc = xmlDocCopyNode(root, ctxt->doc, 1);
        if (ref->inc == NULL) {
            xmlXIncludeErrMemory(ctxt);
            goto error;
        }

        if (ref->base != NULL)
            xmlXIncludeBaseFixup(ctxt, root, ref->inc, ref->base);
    }
#ifdef LIBXML_XPTR_ENABLED
    else {
	/*
	 * Computes the XPointer expression and make a copy used
	 * as the replacement copy.
	 */
	xmlXPathObjectPtr xptr;
	xmlNodeSetPtr set;

        if (ctxt->isStream && doc == ctxt->doc) {
	    xmlXIncludeErr(ctxt, ref->elem, XML_XINCLUDE_XPTR_FAILED,
			   "XPointer expressions not allowed in streaming"
                           " mode\n", NULL);
            goto error;
        }

        if (ctxt->xpctxt == NULL) {
            ctxt->xpctxt = xmlXPathNewContext(doc);
            if (ctxt->xpctxt == NULL) {
                xmlXIncludeErrMemory(ctxt);
                goto error;
            }
            if (ctxt->errorHandler != NULL)
                xmlXPathSetErrorHandler(ctxt->xpctxt, ctxt->errorHandler,
                                        ctxt->errorCtxt);
#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
            ctxt->xpctxt->opLimit = 100000;
#endif
        } else {
            ctxt->xpctxt->doc = doc;
        }
	xptr = xmlXPtrEval(fragment, ctxt->xpctxt);
	if (ctxt->xpctxt->lastError.code != XML_ERR_OK) {
            if (ctxt->xpctxt->lastError.code == XML_ERR_NO_MEMORY)
                xmlXIncludeErrMemory(ctxt);
            else
                xmlXIncludeErr(ctxt, ref->elem, XML_XINCLUDE_XPTR_FAILED,
                               "XPointer evaluation failed: #%s\n",
                               fragment);
            goto error;
	}
        if (xptr == NULL)
            goto done;
	switch (xptr->type) {
	    case XPATH_UNDEFINED:
	    case XPATH_BOOLEAN:
	    case XPATH_NUMBER:
	    case XPATH_STRING:
	    case XPATH_USERS:
	    case XPATH_XSLT_TREE:
		xmlXIncludeErr(ctxt, ref->elem, XML_XINCLUDE_XPTR_RESULT,
			       "XPointer is not a range: #%s\n",
			       fragment);
                xmlXPathFreeObject(xptr);
                goto error;
	    case XPATH_NODESET:
                break;

	}
	set = xptr->nodesetval;
	if (set != NULL) {
	    for (i = 0;i < set->nodeNr;i++) {
		if (set->nodeTab[i] == NULL) /* shouldn't happen */
		    continue;
		switch (set->nodeTab[i]->type) {
		    case XML_ELEMENT_NODE:
		    case XML_TEXT_NODE:
		    case XML_CDATA_SECTION_NODE:
		    case XML_ENTITY_REF_NODE:
		    case XML_ENTITY_NODE:
		    case XML_PI_NODE:
		    case XML_COMMENT_NODE:
		    case XML_DOCUMENT_NODE:
		    case XML_HTML_DOCUMENT_NODE:
			continue;

		    case XML_ATTRIBUTE_NODE:
			xmlXIncludeErr(ctxt, ref->elem,
			               XML_XINCLUDE_XPTR_RESULT,
				       "XPointer selects an attribute: #%s\n",
				       fragment);
			goto xptr_error;
		    case XML_NAMESPACE_DECL:
			xmlXIncludeErr(ctxt, ref->elem,
			               XML_XINCLUDE_XPTR_RESULT,
				       "XPointer selects a namespace: #%s\n",
				       fragment);
			goto xptr_error;
		    case XML_DOCUMENT_TYPE_NODE:
		    case XML_DOCUMENT_FRAG_NODE:
		    case XML_NOTATION_NODE:
		    case XML_DTD_NODE:
		    case XML_ELEMENT_DECL:
		    case XML_ATTRIBUTE_DECL:
		    case XML_ENTITY_DECL:
		    case XML_XINCLUDE_START:
		    case XML_XINCLUDE_END:
                        /* shouldn't happen */
			xmlXIncludeErr(ctxt, ref->elem,
			               XML_XINCLUDE_XPTR_RESULT,
				   "XPointer selects unexpected nodes: #%s\n",
				       fragment);
			goto xptr_error;
		}
	    }
	}
        ref->inc = xmlXIncludeCopyXPointer(ctxt, xptr, ref->base);
xptr_error:
        xmlXPathFreeObject(xptr);
    }

done:
#endif

    ret = 0;

error:
    return(ret);
}

/**
 * Load the content, and store the result in the XInclude context
 *
 * @param ctxt  the XInclude context
 * @param ref  an XMLXincludeRefPtr
 * @returns 0 in case of success, -1 in case of failure
 */
static int
xmlXIncludeLoadTxt(xmlXIncludeCtxtPtr ctxt, xmlXIncludeRefPtr ref) {
    xmlParserInputBufferPtr buf;
    xmlNodePtr node = NULL;
    const xmlChar *url = ref->URI;
    int i;
    int ret = -1;
    xmlChar *encoding = NULL;
    xmlCharEncodingHandlerPtr handler = NULL;
    xmlParserCtxtPtr pctxt = NULL;
    xmlParserInputPtr inputStream = NULL;
    int len;
    int res;
    const xmlChar *content;

    /*
     * Handling of references to the local document are done
     * directly through ctxt->doc.
     */
    if (url[0] == 0) {
	xmlXIncludeErr(ctxt, ref->elem, XML_XINCLUDE_TEXT_DOCUMENT,
		       "text serialization of document not available\n", NULL);
	goto error;
    }

    /*
     * Prevent reloading the document twice.
     */
    for (i = 0; i < ctxt->txtNr; i++) {
	if (xmlStrEqual(url, ctxt->txtTab[i].url)) {
            node = xmlNewDocText(ctxt->doc, ctxt->txtTab[i].text);
            if (node == NULL)
                xmlXIncludeErrMemory(ctxt);
	    goto loaded;
	}
    }

    /*
     * Try to get the encoding if available
     */
    if (ref->elem != NULL) {
	encoding = xmlXIncludeGetProp(ctxt, ref->elem, XINCLUDE_PARSE_ENCODING);
    }
    if (encoding != NULL) {
        xmlParserErrors code;

        code = xmlOpenCharEncodingHandler((const char *) encoding,
                                          /* output */ 0, &handler);

        if (code != XML_ERR_OK) {
            if (code == XML_ERR_NO_MEMORY) {
                xmlXIncludeErrMemory(ctxt);
            } else if (code == XML_ERR_UNSUPPORTED_ENCODING) {
                xmlXIncludeErr(ctxt, ref->elem, XML_XINCLUDE_UNKNOWN_ENCODING,
                               "encoding %s not supported\n", encoding);
                goto error;
            } else {
                xmlXIncludeErr(ctxt, ref->elem, code,
                               "unexpected error from iconv or ICU\n", NULL);
                goto error;
            }
        }
    }

    /*
     * Load it.
     */
    pctxt = xmlNewParserCtxt();
    if (pctxt == NULL) {
        xmlXIncludeErrMemory(ctxt);
        goto error;
    }
    if (ctxt->errorHandler != NULL)
        xmlCtxtSetErrorHandler(pctxt, ctxt->errorHandler, ctxt->errorCtxt);
    if (ctxt->resourceLoader != NULL)
        xmlCtxtSetResourceLoader(pctxt, ctxt->resourceLoader,
                                 ctxt->resourceCtxt);

    inputStream = xmlLoadResource(pctxt, (const char*) url, NULL,
                                  XML_RESOURCE_XINCLUDE_TEXT);
    if (inputStream == NULL) {
        /*
         * ENOENT only produces a warning which isn't reflected in errNo.
         */
        if (pctxt->errNo == XML_ERR_NO_MEMORY)
            xmlXIncludeErrMemory(ctxt);
        else if ((pctxt->errNo != XML_ERR_OK) &&
                 (pctxt->errNo != XML_IO_ENOENT) &&
                 (pctxt->errNo != XML_IO_UNKNOWN))
            xmlXIncludeErr(ctxt, NULL, pctxt->errNo, "load error", NULL);
	goto error;
    }
    buf = inputStream->buf;
    if (buf == NULL)
	goto error;
    if (buf->encoder)
	xmlCharEncCloseFunc(buf->encoder);
    buf->encoder = handler;
    handler = NULL;

    node = xmlNewDocText(ctxt->doc, NULL);
    if (node == NULL) {
        xmlXIncludeErrMemory(ctxt);
	goto error;
    }

    /*
     * Scan all chars from the resource and add the to the node
     */
    do {
        res = xmlParserInputBufferRead(buf, 4096);
    } while (res > 0);
    if (res < 0) {
        if (buf->error == XML_ERR_NO_MEMORY)
            xmlXIncludeErrMemory(ctxt);
        else
            xmlXIncludeErr(ctxt, NULL, buf->error, "read error", NULL);
        goto error;
    }

    content = xmlBufContent(buf->buffer);
    len = xmlBufUse(buf->buffer);
    for (i = 0; i < len;) {
        int cur;
        int l;

        l = len - i;
        cur = xmlGetUTF8Char(&content[i], &l);
        if ((cur < 0) || (!IS_CHAR(cur))) {
            xmlXIncludeErr(ctxt, ref->elem, XML_XINCLUDE_INVALID_CHAR,
                           "%s contains invalid char\n", url);
            goto error;
        }

        i += l;
    }

    if (xmlNodeAddContentLen(node, content, len) < 0)
        xmlXIncludeErrMemory(ctxt);

    if (ctxt->txtNr >= ctxt->txtMax) {
        xmlXIncludeTxt *tmp;
        int newSize;

        newSize = xmlGrowCapacity(ctxt->txtMax, sizeof(tmp[0]),
                                  8, XML_MAX_ITEMS);
        if (newSize < 0) {
            xmlXIncludeErrMemory(ctxt);
	    goto error;
        }
        tmp = xmlRealloc(ctxt->txtTab, newSize * sizeof(tmp[0]));
        if (tmp == NULL) {
            xmlXIncludeErrMemory(ctxt);
	    goto error;
        }
        ctxt->txtMax = newSize;
        ctxt->txtTab = tmp;
    }
    ctxt->txtTab[ctxt->txtNr].text = xmlStrdup(node->content);
    if ((node->content != NULL) &&
        (ctxt->txtTab[ctxt->txtNr].text == NULL)) {
        xmlXIncludeErrMemory(ctxt);
        goto error;
    }
    ctxt->txtTab[ctxt->txtNr].url = xmlStrdup(url);
    if (ctxt->txtTab[ctxt->txtNr].url == NULL) {
        xmlXIncludeErrMemory(ctxt);
        xmlFree(ctxt->txtTab[ctxt->txtNr].text);
        goto error;
    }
    ctxt->txtNr++;

loaded:
    /*
     * Add the element as the replacement copy.
     */
    ref->inc = node;
    node = NULL;
    ret = 0;

error:
    xmlFreeNode(node);
    xmlFreeInputStream(inputStream);
    xmlFreeParserCtxt(pctxt);
    xmlCharEncCloseFunc(handler);
    xmlFree(encoding);
    return(ret);
}

/**
 * Load the content of the fallback node, and store the result
 * in the XInclude context
 *
 * @param ctxt  the XInclude context
 * @param fallback  the fallback node
 * @param ref  an XMLXincludeRefPtr
 * @returns 0 in case of success, -1 in case of failure
 */
static int
xmlXIncludeLoadFallback(xmlXIncludeCtxtPtr ctxt, xmlNodePtr fallback,
                        xmlXIncludeRefPtr ref) {
    int ret = 0;
    int oldNbErrors;

    if ((fallback == NULL) || (fallback->type == XML_NAMESPACE_DECL) ||
        (ctxt == NULL))
	return(-1);
    if (fallback->children != NULL) {
	/*
	 * It's possible that the fallback also has 'includes'
	 * (Bug 129969), so we re-process the fallback just in case
	 */
        oldNbErrors = ctxt->nbErrors;
	ref->inc = xmlXIncludeCopyNode(ctxt, fallback, 1, ref->base);
	if (ctxt->nbErrors > oldNbErrors)
	    ret = -1;
    } else {
        ref->inc = NULL;
    }
    ref->fallback = 1;
    return(ret);
}

/************************************************************************
 *									*
 *			XInclude Processing				*
 *									*
 ************************************************************************/

/**
 * If the XInclude node wasn't processed yet, create a new RefPtr,
 * add it to ctxt->incTab and load the included items.
 *
 * @param ctxt  an XInclude context
 * @param node  an XInclude node
 * @returns the new or existing xmlXIncludeRef, or NULL in case of error.
 */
static xmlXIncludeRefPtr
xmlXIncludeExpandNode(xmlXIncludeCtxtPtr ctxt, xmlNodePtr node) {
    xmlXIncludeRefPtr ref;
    int i;

    if (ctxt->fatalErr)
        return(NULL);
    if (ctxt->depth >= XINCLUDE_MAX_DEPTH) {
        xmlXIncludeErr(ctxt, node, XML_XINCLUDE_RECURSION,
                       "maximum recursion depth exceeded\n", NULL);
        ctxt->fatalErr = 1;
        return(NULL);
    }

#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
    /*
     * The XInclude engine offers no protection against exponential
     * expansion attacks similar to "billion laughs". Avoid timeouts by
     * limiting the total number of replacements when fuzzing.
     *
     * Unfortuately, a single XInclude can already result in quadratic
     * behavior:
     *
     *     <doc xmlns:xi="http://www.w3.org/2001/XInclude">
     *       <xi:include xpointer="xpointer(//e)"/>
     *       <e>
     *         <e>
     *           <e>
     *             <!-- more nested elements -->
     *           </e>
     *         </e>
     *       </e>
     *     </doc>
     */
    if (ctxt->incTotal >= 20)
        return(NULL);
    ctxt->incTotal++;
#endif

    for (i = 0; i < ctxt->incNr; i++) {
        if (ctxt->incTab[i]->elem == node) {
            if (ctxt->incTab[i]->expanding) {
                xmlXIncludeErr(ctxt, node, XML_XINCLUDE_RECURSION,
                               "inclusion loop detected\n", NULL);
                return(NULL);
            }
            return(ctxt->incTab[i]);
        }
    }

    ref = xmlXIncludeAddNode(ctxt, node);
    if (ref == NULL)
        return(NULL);
    ref->expanding = 1;
    ctxt->depth++;
    xmlXIncludeLoadNode(ctxt, ref);
    ctxt->depth--;
    ref->expanding = 0;

    return(ref);
}

/**
 * Find and load the infoset replacement for the given node.
 *
 * @param ctxt  an XInclude context
 * @param ref  an xmlXIncludeRef
 * @returns 0 if substitution succeeded, -1 if some processing failed
 */
static int
xmlXIncludeLoadNode(xmlXIncludeCtxtPtr ctxt, xmlXIncludeRefPtr ref) {
    xmlNodePtr cur;
    int ret;

    if ((ctxt == NULL) || (ref == NULL))
	return(-1);
    cur = ref->elem;
    if (cur == NULL)
	return(-1);

    if (ref->xml) {
	ret = xmlXIncludeLoadDoc(ctxt, ref);
	/* xmlXIncludeGetFragment(ctxt, cur, URI); */
    } else {
	ret = xmlXIncludeLoadTxt(ctxt, ref);
    }

    if (ret < 0) {
	xmlNodePtr children;

	/*
	 * Time to try a fallback if available
	 */
	children = cur->children;
	while (children != NULL) {
	    if ((children->type == XML_ELEMENT_NODE) &&
		(children->ns != NULL) &&
		(xmlStrEqual(children->name, XINCLUDE_FALLBACK)) &&
		((xmlStrEqual(children->ns->href, XINCLUDE_NS)) ||
		 (xmlStrEqual(children->ns->href, XINCLUDE_OLD_NS)))) {
		ret = xmlXIncludeLoadFallback(ctxt, children, ref);
		break;
	    }
	    children = children->next;
	}
    }
    if (ret < 0) {
	xmlXIncludeErr(ctxt, cur, XML_XINCLUDE_NO_FALLBACK,
		       "could not load %s, and no fallback was found\n",
		       ref->URI);
    }

    return(0);
}

/**
 * Implement the infoset replacement for the given node
 *
 * @param ctxt  an XInclude context
 * @param ref  an xmlXIncludeRef
 * @returns 0 if substitution succeeded, -1 if some processing failed
 */
static int
xmlXIncludeIncludeNode(xmlXIncludeCtxtPtr ctxt, xmlXIncludeRefPtr ref) {
    xmlNodePtr cur, end, list, tmp;

    if ((ctxt == NULL) || (ref == NULL))
	return(-1);
    cur = ref->elem;
    if ((cur == NULL) || (cur->type == XML_NAMESPACE_DECL))
	return(-1);

    list = ref->inc;
    ref->inc = NULL;

    /*
     * Check against the risk of generating a multi-rooted document
     */
    if ((cur->parent != NULL) &&
	(cur->parent->type != XML_ELEMENT_NODE)) {
	int nb_elem = 0;

	tmp = list;
	while (tmp != NULL) {
	    if (tmp->type == XML_ELEMENT_NODE)
		nb_elem++;
	    tmp = tmp->next;
	}
        if (nb_elem != 1) {
            if (nb_elem > 1)
                xmlXIncludeErr(ctxt, ref->elem, XML_XINCLUDE_MULTIPLE_ROOT,
                               "XInclude error: would result in multiple root "
                               "nodes\n", NULL);
            else
                xmlXIncludeErr(ctxt, ref->elem, XML_XINCLUDE_MULTIPLE_ROOT,
                               "XInclude error: would result in no root "
                               "node\n", NULL);
            xmlFreeNodeList(list);
	    return(-1);
	}
    }

    if (ctxt->parseFlags & XML_PARSE_NOXINCNODE) {
	/*
	 * Add the list of nodes
         *
         * TODO: Coalesce text nodes unless we are streaming mode.
	 */
	while (list != NULL) {
	    end = list;
	    list = list->next;

	    if (xmlAddPrevSibling(cur, end) == NULL) {
                xmlUnlinkNode(end);
                xmlFreeNode(end);
                goto err_memory;
            }
	}
	xmlUnlinkNode(cur);
	xmlFreeNode(cur);
    } else {
        xmlNodePtr child, next;

	/*
	 * Change the current node as an XInclude start one, and add an
	 * XInclude end one
	 */
        if (ref->fallback)
            xmlUnsetProp(cur, BAD_CAST "href");
	cur->type = XML_XINCLUDE_START;
        /* Remove fallback children */
        for (child = cur->children; child != NULL; child = next) {
            next = child->next;
            xmlUnlinkNode(child);
            xmlFreeNode(child);
        }
	end = xmlNewDocNode(cur->doc, cur->ns, cur->name, NULL);
	if (end == NULL)
            goto err_memory;
	end->type = XML_XINCLUDE_END;
	if (xmlAddNextSibling(cur, end) == NULL) {
            xmlFreeNode(end);
            goto err_memory;
        }

	/*
	 * Add the list of nodes
	 */
	while (list != NULL) {
	    cur = list;
	    list = list->next;

	    if (xmlAddPrevSibling(end, cur) == NULL) {
                xmlUnlinkNode(cur);
                xmlFreeNode(cur);
                goto err_memory;
            }
	}
    }


    return(0);

err_memory:
    xmlXIncludeErrMemory(ctxt);
    xmlFreeNodeList(list);
    return(-1);
}

/**
 * test if the node is an XInclude node
 *
 * @param ctxt  the XInclude processing context
 * @param node  an XInclude node
 * @returns 1 true, 0 otherwise
 */
static int
xmlXIncludeTestNode(xmlXIncludeCtxtPtr ctxt, xmlNodePtr node) {
    if (node == NULL)
	return(0);
    if (node->type != XML_ELEMENT_NODE)
	return(0);
    if (node->ns == NULL)
	return(0);
    if ((xmlStrEqual(node->ns->href, XINCLUDE_NS)) ||
        (xmlStrEqual(node->ns->href, XINCLUDE_OLD_NS))) {
	if (xmlStrEqual(node->ns->href, XINCLUDE_OLD_NS)) {
	    if (ctxt->legacy == 0) {
	        ctxt->legacy = 1;
	    }
	}
	if (xmlStrEqual(node->name, XINCLUDE_NODE)) {
	    xmlNodePtr child = node->children;
	    int nb_fallback = 0;

	    while (child != NULL) {
		if ((child->type == XML_ELEMENT_NODE) &&
		    (child->ns != NULL) &&
		    ((xmlStrEqual(child->ns->href, XINCLUDE_NS)) ||
		     (xmlStrEqual(child->ns->href, XINCLUDE_OLD_NS)))) {
		    if (xmlStrEqual(child->name, XINCLUDE_NODE)) {
			xmlXIncludeErr(ctxt, node,
			               XML_XINCLUDE_INCLUDE_IN_INCLUDE,
				       "%s has an 'include' child\n",
				       XINCLUDE_NODE);
			return(0);
		    }
		    if (xmlStrEqual(child->name, XINCLUDE_FALLBACK)) {
			nb_fallback++;
		    }
		}
		child = child->next;
	    }
	    if (nb_fallback > 1) {
		xmlXIncludeErr(ctxt, node, XML_XINCLUDE_FALLBACKS_IN_INCLUDE,
			       "%s has multiple fallback children\n",
		               XINCLUDE_NODE);
		return(0);
	    }
	    return(1);
	}
	if (xmlStrEqual(node->name, XINCLUDE_FALLBACK)) {
	    if ((node->parent == NULL) ||
		(node->parent->type != XML_ELEMENT_NODE) ||
		(node->parent->ns == NULL) ||
		((!xmlStrEqual(node->parent->ns->href, XINCLUDE_NS)) &&
		 (!xmlStrEqual(node->parent->ns->href, XINCLUDE_OLD_NS))) ||
		(!xmlStrEqual(node->parent->name, XINCLUDE_NODE))) {
		xmlXIncludeErr(ctxt, node,
		               XML_XINCLUDE_FALLBACK_NOT_IN_INCLUDE,
			       "%s is not the child of an 'include'\n",
			       XINCLUDE_FALLBACK);
	    }
	}
    }
    return(0);
}

/**
 * Implement the XInclude substitution on the XML document `doc`
 *
 * @param ctxt  the XInclude processing context
 * @param tree  the top of the tree to process
 * @returns 0 if no substitution were done, -1 if some processing failed
 *    or the number of substitutions done.
 */
static int
xmlXIncludeDoProcess(xmlXIncludeCtxtPtr ctxt, xmlNodePtr tree) {
    xmlXIncludeRefPtr ref;
    xmlNodePtr cur;
    int ret = 0;
    int i, start;

    /*
     * First phase: lookup the elements in the document
     */
    start = ctxt->incNr;
    cur = tree;
    do {
	/* TODO: need to work on entities -> stack */
        if (xmlXIncludeTestNode(ctxt, cur) == 1) {
            ref = xmlXIncludeExpandNode(ctxt, cur);
            /*
             * Mark direct includes.
             */
            if (ref != NULL)
                ref->replace = 1;
        } else if ((cur->children != NULL) &&
                   ((cur->type == XML_DOCUMENT_NODE) ||
                    (cur->type == XML_ELEMENT_NODE))) {
            cur = cur->children;
            continue;
        }
        do {
            if (cur == tree)
                break;
            if (cur->next != NULL) {
                cur = cur->next;
                break;
            }
            cur = cur->parent;
        } while (cur != NULL);
    } while ((cur != NULL) && (cur != tree));

    /*
     * Second phase: extend the original document infoset.
     */
    for (i = start; i < ctxt->incNr; i++) {
	if (ctxt->incTab[i]->replace != 0) {
            xmlXIncludeIncludeNode(ctxt, ctxt->incTab[i]);
            ctxt->incTab[i]->replace = 0;
        } else {
            /*
             * Ignore includes which were added indirectly, for example
             * inside xi:fallback elements.
             */
            if (ctxt->incTab[i]->inc != NULL) {
                xmlFreeNodeList(ctxt->incTab[i]->inc);
                ctxt->incTab[i]->inc = NULL;
            }
        }
	ret++;
    }

    if (ctxt->isStream) {
        /*
         * incTab references nodes which will eventually be deleted in
         * streaming mode. The table is only required for XPointer
         * expressions which aren't allowed in streaming mode.
         */
        for (i = 0;i < ctxt->incNr;i++) {
            xmlXIncludeFreeRef(ctxt->incTab[i]);
        }
        ctxt->incNr = 0;
    }

    return(ret);
}

/**
 * Implement the XInclude substitution on the XML document `doc`
 *
 * @param ctxt  the XInclude processing context
 * @param tree  the top of the tree to process
 * @returns 0 if no substitution were done, -1 if some processing failed
 *    or the number of substitutions done.
 */
static int
xmlXIncludeDoProcessRoot(xmlXIncludeCtxtPtr ctxt, xmlNodePtr tree) {
    if ((tree == NULL) || (tree->type == XML_NAMESPACE_DECL))
	return(-1);
    if (ctxt == NULL)
	return(-1);

    return(xmlXIncludeDoProcess(ctxt, tree));
}

/**
 * @since 2.13.0
 *
 * @param ctxt  an XInclude processing context
 * @returns the last error code.
 */
int
xmlXIncludeGetLastError(xmlXIncludeCtxt *ctxt) {
    if (ctxt == NULL)
        return(XML_ERR_ARGUMENT);
    return(ctxt->errNo);
}

/**
 * Register a callback function that will be called on errors and
 * warnings. If handler is NULL, the error handler will be deactivated.
 *
 * @since 2.13.0
 * @param ctxt  an XInclude processing context
 * @param handler  error handler
 * @param data  user data which will be passed to the handler
 */
void
xmlXIncludeSetErrorHandler(xmlXIncludeCtxt *ctxt,
                           xmlStructuredErrorFunc handler, void *data) {
    if (ctxt == NULL)
        return;
    ctxt->errorHandler = handler;
    ctxt->errorCtxt = data;
}

/**
 * Register a callback function that will be called to load included
 * documents.
 *
 * @since 2.14.0
 * @param ctxt  an XInclude processing context
 * @param loader  resource loader
 * @param data  user data which will be passed to the loader
 */
void
xmlXIncludeSetResourceLoader(xmlXIncludeCtxt *ctxt,
                             xmlResourceLoader loader, void *data) {
    if (ctxt == NULL)
        return;
    ctxt->resourceLoader = loader;
    ctxt->resourceCtxt = data;
}

/**
 * Set the flags used for further processing of XML resources.
 *
 * @param ctxt  an XInclude processing context
 * @param flags  a set of xmlParserOption used for parsing XML includes
 * @returns 0 in case of success and -1 in case of error.
 */
int
xmlXIncludeSetFlags(xmlXIncludeCtxt *ctxt, int flags) {
    if (ctxt == NULL)
        return(-1);
    ctxt->parseFlags = flags;
    return(0);
}

/**
 * In streaming mode, XPointer expressions aren't allowed.
 *
 * @param ctxt  an XInclude processing context
 * @param mode  whether streaming mode should be enabled
 * @returns 0 in case of success and -1 in case of error.
 */
int
xmlXIncludeSetStreamingMode(xmlXIncludeCtxt *ctxt, int mode) {
    if (ctxt == NULL)
        return(-1);
    ctxt->isStream = !!mode;
    return(0);
}

/**
 * Implement the XInclude substitution on the XML node `tree`
 *
 * @param tree  an XML node
 * @param flags  a set of xmlParserOption used for parsing XML includes
 * @param data  application data that will be passed to the parser context
 *        in the _private field of the parser context(s)
 * @returns 0 if no substitution were done, -1 if some processing failed
 *    or the number of substitutions done.
 */

int
xmlXIncludeProcessTreeFlagsData(xmlNode *tree, int flags, void *data) {
    xmlXIncludeCtxtPtr ctxt;
    int ret = 0;

    if ((tree == NULL) || (tree->type == XML_NAMESPACE_DECL) ||
        (tree->doc == NULL))
        return(-1);

    ctxt = xmlXIncludeNewContext(tree->doc);
    if (ctxt == NULL)
        return(-1);
    ctxt->_private = data;
    xmlXIncludeSetFlags(ctxt, flags);
    ret = xmlXIncludeDoProcessRoot(ctxt, tree);
    if ((ret >= 0) && (ctxt->nbErrors > 0))
        ret = -1;

    xmlXIncludeFreeContext(ctxt);
    return(ret);
}

/**
 * Implement the XInclude substitution on the XML document `doc`
 *
 * @param doc  an XML document
 * @param flags  a set of xmlParserOption used for parsing XML includes
 * @param data  application data that will be passed to the parser context
 *        in the _private field of the parser context(s)
 * @returns 0 if no substitution were done, -1 if some processing failed
 *    or the number of substitutions done.
 */
int
xmlXIncludeProcessFlagsData(xmlDoc *doc, int flags, void *data) {
    xmlNodePtr tree;

    if (doc == NULL)
	return(-1);
    tree = xmlDocGetRootElement(doc);
    if (tree == NULL)
	return(-1);
    return(xmlXIncludeProcessTreeFlagsData(tree, flags, data));
}

/**
 * Implement the XInclude substitution on the XML document `doc`
 *
 * @param doc  an XML document
 * @param flags  a set of xmlParserOption used for parsing XML includes
 * @returns 0 if no substitution were done, -1 if some processing failed
 *    or the number of substitutions done.
 */
int
xmlXIncludeProcessFlags(xmlDoc *doc, int flags) {
    return xmlXIncludeProcessFlagsData(doc, flags, NULL);
}

/**
 * Implement the XInclude substitution on the XML document `doc`
 *
 * @param doc  an XML document
 * @returns 0 if no substitution were done, -1 if some processing failed
 *    or the number of substitutions done.
 */
int
xmlXIncludeProcess(xmlDoc *doc) {
    return(xmlXIncludeProcessFlags(doc, 0));
}

/**
 * Implement the XInclude substitution for the given subtree
 *
 * @param tree  a node in an XML document
 * @param flags  a set of xmlParserOption used for parsing XML includes
 * @returns 0 if no substitution were done, -1 if some processing failed
 *    or the number of substitutions done.
 */
int
xmlXIncludeProcessTreeFlags(xmlNode *tree, int flags) {
    xmlXIncludeCtxtPtr ctxt;
    int ret = 0;

    if ((tree == NULL) || (tree->type == XML_NAMESPACE_DECL) ||
        (tree->doc == NULL))
	return(-1);
    ctxt = xmlXIncludeNewContext(tree->doc);
    if (ctxt == NULL)
	return(-1);
    xmlXIncludeSetFlags(ctxt, flags);
    ret = xmlXIncludeDoProcessRoot(ctxt, tree);
    if ((ret >= 0) && (ctxt->nbErrors > 0))
	ret = -1;

    xmlXIncludeFreeContext(ctxt);
    return(ret);
}

/**
 * Implement the XInclude substitution for the given subtree
 *
 * @param tree  a node in an XML document
 * @returns 0 if no substitution were done, -1 if some processing failed
 *    or the number of substitutions done.
 */
int
xmlXIncludeProcessTree(xmlNode *tree) {
    return(xmlXIncludeProcessTreeFlags(tree, 0));
}

/**
 * Implement the XInclude substitution for the given subtree reusing
 * the information and data coming from the given context.
 *
 * @param ctxt  an existing XInclude context
 * @param node  a node in an XML document
 * @returns 0 if no substitution were done, -1 if some processing failed
 *    or the number of substitutions done.
 */
int
xmlXIncludeProcessNode(xmlXIncludeCtxt *ctxt, xmlNode *node) {
    int ret = 0;

    if ((node == NULL) || (node->type == XML_NAMESPACE_DECL) ||
        (node->doc == NULL) || (ctxt == NULL))
	return(-1);
    ret = xmlXIncludeDoProcessRoot(ctxt, node);
    if ((ret >= 0) && (ctxt->nbErrors > 0))
	ret = -1;
    return(ret);
}

#else /* !LIBXML_XINCLUDE_ENABLED */
#endif

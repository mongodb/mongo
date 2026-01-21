/*
 * tree.c : implementation of access function for an XML tree.
 *
 * References:
 *   XHTML 1.0 W3C REC: http://www.w3.org/TR/2002/REC-xhtml1-20020801/
 *
 * See Copyright for the status of this software.
 *
 * Author: Daniel Veillard
 *
 */

/* To avoid EBCDIC trouble when parsing on zOS */
#if defined(__MVS__)
#pragma convert("ISO8859-1")
#endif

#define IN_LIBXML
#include "libxml.h"

#include <string.h> /* for memset() only ! */
#include <stddef.h>
#include <limits.h>
#include <ctype.h>
#include <stdlib.h>

#ifdef LIBXML_ZLIB_ENABLED
#include <zlib.h>
#endif

#include <libxml/tree.h>
#include <libxml/xmlmemory.h>
#include <libxml/parser.h>
#include <libxml/uri.h>
#include <libxml/entities.h>
#include <libxml/xmlerror.h>
#include <libxml/parserInternals.h>
#ifdef LIBXML_HTML_ENABLED
#include <libxml/HTMLtree.h>
#endif

#include "private/buf.h"
#include "private/entities.h"
#include "private/error.h"
#include "private/memory.h"
#include "private/io.h"
#include "private/parser.h"
#include "private/tree.h"

#ifndef SIZE_MAX
  #define SIZE_MAX ((size_t) -1)
#endif

/*
 * Internal variable indicating whether a callback has been registered
 * for node creation/destruction. This avoids looking up thread-local
 * data if no callback was ever registered.
 */
int xmlRegisterCallbacks = 0;

/************************************************************************
 *									*
 *		Forward declarations					*
 *									*
 ************************************************************************/

static xmlNodePtr
xmlNewEntityRef(xmlDocPtr doc, xmlChar *name);

static xmlNsPtr
xmlNewReconciledNs(xmlNodePtr tree, xmlNsPtr ns);

static xmlAttrPtr
xmlGetPropNodeInternal(const xmlNode *node, const xmlChar *name,
		       const xmlChar *nsName, int useDTD);

static xmlChar* xmlGetPropNodeValueInternal(const xmlAttr *prop);

static void
xmlBufGetChildContent(xmlBufPtr buf, const xmlNode *tree);

static void
xmlUnlinkNodeInternal(xmlNodePtr cur);

/************************************************************************
 *									*
 *		A few static variables and macros			*
 *									*
 ************************************************************************/
/* #undef xmlStringText */
const xmlChar xmlStringText[] = { 't', 'e', 'x', 't', 0 };
/* #undef xmlStringTextNoenc */
const xmlChar xmlStringTextNoenc[] =
              { 't', 'e', 'x', 't', 'n', 'o', 'e', 'n', 'c', 0 };
/* #undef xmlStringComment */
const xmlChar xmlStringComment[] = { 'c', 'o', 'm', 'm', 'e', 'n', 't', 0 };

static int xmlCompressMode = 0;

#define IS_STR_XML(str) ((str != NULL) && (str[0] == 'x') && \
  (str[1] == 'm') && (str[2] == 'l') && (str[3] == 0))

/************************************************************************
 *									*
 *		Functions to move to entities.c once the		*
 *		API freeze is smoothen and they can be made public.	*
 *									*
 ************************************************************************/
#include <libxml/hash.h>

/**
 * Do an entity lookup in the DTD entity hash table and
 *
 * @param dtd  A pointer to the DTD to search
 * @param name  The entity name
 * @returns the corresponding entity, if found.
 */
static xmlEntityPtr
xmlGetEntityFromDtd(const xmlDtd *dtd, const xmlChar *name) {
    xmlEntitiesTablePtr table;

    if((dtd != NULL) && (dtd->entities != NULL)) {
	table = (xmlEntitiesTablePtr) dtd->entities;
	return((xmlEntityPtr) xmlHashLookup(table, name));
	/* return(xmlGetEntityFromTable(table, name)); */
    }
    return(NULL);
}
/**
 * Do an entity lookup in the DTD parameter entity hash table and
 *
 * @param dtd  A pointer to the DTD to search
 * @param name  The entity name
 * @returns the corresponding entity, if found.
 */
static xmlEntityPtr
xmlGetParameterEntityFromDtd(const xmlDtd *dtd, const xmlChar *name) {
    xmlEntitiesTablePtr table;

    if ((dtd != NULL) && (dtd->pentities != NULL)) {
	table = (xmlEntitiesTablePtr) dtd->pentities;
	return((xmlEntityPtr) xmlHashLookup(table, name));
	/* return(xmlGetEntityFromTable(table, name)); */
    }
    return(NULL);
}

/************************************************************************
 *									*
 *			QName handling helper				*
 *									*
 ************************************************************************/

/**
 * Build a QName from prefix and local name.
 *
 * Builds the QName `prefix:ncname` in `memory` if there is enough space
 * and prefix is not NULL nor empty, otherwise allocate a new string.
 * If prefix is NULL or empty it returns ncname.
 *
 * @param ncname  the Name
 * @param prefix  the prefix
 * @param memory  preallocated memory
 * @param len  preallocated memory length
 * @returns the new string which must be freed by the caller if different from
 *         `memory` and `ncname` or NULL in case of error
 */
xmlChar *
xmlBuildQName(const xmlChar *ncname, const xmlChar *prefix,
	      xmlChar *memory, int len) {
    size_t lenn, lenp;
    xmlChar *ret;

    if ((ncname == NULL) || (len < 0)) return(NULL);
    if (prefix == NULL) return((xmlChar *) ncname);

#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
    /* Make allocation more likely */
    if (len > 8)
        len = 8;
#endif

    lenn = strlen((char *) ncname);
    lenp = strlen((char *) prefix);
    if (lenn >= SIZE_MAX - lenp - 1)
        return(NULL);

    if ((memory == NULL) || ((size_t) len < lenn + lenp + 2)) {
	ret = xmlMalloc(lenn + lenp + 2);
	if (ret == NULL)
	    return(NULL);
    } else {
	ret = memory;
    }
    memcpy(&ret[0], prefix, lenp);
    ret[lenp] = ':';
    memcpy(&ret[lenp + 1], ncname, lenn);
    ret[lenn + lenp + 1] = 0;
    return(ret);
}

/**
 * Parse an XML qualified name.
 *
 * @deprecated This function doesn't report malloc failures.
 *
 *     [NS 5] QName ::= (Prefix ':')? LocalPart
 *
 *     [NS 6] Prefix ::= NCName
 *
 *     [NS 7] LocalPart ::= NCName
 *
 * @param name  the full QName
 * @param prefix  a xmlChar **
 * @returns NULL if the name doesn't have a prefix. Otherwise, returns the
 * local part, and prefix is updated to get the Prefix. Both the return value
 * and the prefix must be freed by the caller.
 */
xmlChar *
xmlSplitQName2(const xmlChar *name, xmlChar **prefix) {
    int len = 0;
    xmlChar *ret = NULL;

    if (prefix == NULL) return(NULL);
    *prefix = NULL;
    if (name == NULL) return(NULL);

    /* nasty but valid */
    if (name[0] == ':')
	return(NULL);

    /*
     * we are not trying to validate but just to cut, and yes it will
     * work even if this is as set of UTF-8 encoded chars
     */
    while ((name[len] != 0) && (name[len] != ':'))
	len++;

    if ((name[len] == 0) || (name[len+1] == 0))
	return(NULL);

    *prefix = xmlStrndup(name, len);
    if (*prefix == NULL)
	return(NULL);
    ret = xmlStrdup(&name[len + 1]);
    if (ret == NULL) {
	if (*prefix != NULL) {
	    xmlFree(*prefix);
	    *prefix = NULL;
	}
	return(NULL);
    }

    return(ret);
}

/**
 * Parse an XML qualified name.
 *
 * @param name  the full QName
 * @param len  an int *
 * @returns NULL if it is not a Qualified Name, otherwise, update len
 *         with the length in byte of the prefix and return a pointer
 *         to the start of the name without the prefix
 */

const xmlChar *
xmlSplitQName3(const xmlChar *name, int *len) {
    int l = 0;

    if (name == NULL) return(NULL);
    if (len == NULL) return(NULL);

    /* nasty but valid */
    if (name[0] == ':')
	return(NULL);

    /*
     * we are not trying to validate but just to cut, and yes it will
     * work even if this is as set of UTF-8 encoded chars
     */
    while ((name[l] != 0) && (name[l] != ':'))
	l++;

    if ((name[l] == 0) || (name[l+1] == 0))
	return(NULL);

    *len = l;

    return(&name[l+1]);
}

/**
 * Parse a QName.
 *
 * The return value points to the start of the local
 * name in the input string. If the QName has a prefix, it will be
 * allocated and stored in `prefixPtr`. This string must be freed by
 * the caller. If there's no prefix, `prefixPtr` is set to NULL.
 *
 * @param name  the full QName
 * @param prefixPtr  pointer to resulting prefix
 * @returns the local name or NULL if a memory allocation failed.
 */
const xmlChar *
xmlSplitQName4(const xmlChar *name, xmlChar **prefixPtr) {
    xmlChar *prefix;
    int l = 0;

    if ((name == NULL) || (prefixPtr == NULL))
        return(NULL);

    *prefixPtr = NULL;

    /* nasty but valid */
    if (name[0] == ':')
	return(name);

    /*
     * we are not trying to validate but just to cut, and yes it will
     * work even if this is as set of UTF-8 encoded chars
     */
    while ((name[l] != 0) && (name[l] != ':'))
	l++;

    /*
     * TODO: What about names with multiple colons?
     */
    if ((name[l] == 0) || (name[l+1] == 0))
	return(name);

    prefix = xmlStrndup(name, l);
    if (prefix == NULL)
        return(NULL);

    *prefixPtr = prefix;
    return(&name[l+1]);
}

/************************************************************************
 *									*
 *		Check Name, NCName and QName strings			*
 *									*
 ************************************************************************/

/**
 * Check that a value conforms to the lexical space of NCName
 *
 * @param value  the value to check
 * @param space  allow spaces in front and end of the string
 * @returns 0 if this validates, a positive error code number otherwise
 *         and -1 in case of internal or API error.
 */
int
xmlValidateNCName(const xmlChar *value, int space) {
    const xmlChar *cur;

    if (value == NULL)
        return(-1);

    cur = value;

    if (space) {
	while (IS_BLANK_CH(*cur))
            cur++;
    }

    value = cur;
    cur = xmlScanName(value, SIZE_MAX, XML_SCAN_NC);
    if ((cur == NULL) || (cur == value))
        return(1);

    if (space) {
	while (IS_BLANK_CH(*cur))
            cur++;
    }

    return(*cur != 0);
}

/**
 * Check that a value conforms to the lexical space of QName
 *
 * @param value  the value to check
 * @param space  allow spaces in front and end of the string
 * @returns 0 if this validates, a positive error code number otherwise
 *         and -1 in case of internal or API error.
 */
int
xmlValidateQName(const xmlChar *value, int space) {
    const xmlChar *cur;

    if (value == NULL)
        return(-1);

    cur = value;

    if (space) {
	while (IS_BLANK_CH(*cur))
            cur++;
    }

    value = cur;
    cur = xmlScanName(value, SIZE_MAX, XML_SCAN_NC);
    if ((cur == NULL) || (cur == value))
        return(1);

    if (*cur == ':') {
        cur += 1;
        value = cur;
        cur = xmlScanName(value, SIZE_MAX, XML_SCAN_NC);
        if ((cur == NULL) || (cur == value))
            return(1);
    }

    if (space) {
	while (IS_BLANK_CH(*cur))
            cur++;
    }

    return(*cur != 0);
}

/**
 * Check that a value conforms to the lexical space of Name
 *
 * @param value  the value to check
 * @param space  allow spaces in front and end of the string
 * @returns 0 if this validates, a positive error code number otherwise
 *         and -1 in case of internal or API error.
 */
int
xmlValidateName(const xmlChar *value, int space) {
    const xmlChar *cur;

    if (value == NULL)
        return(-1);

    cur = value;

    if (space) {
	while (IS_BLANK_CH(*cur))
            cur++;
    }

    value = cur;
    cur = xmlScanName(value, SIZE_MAX, 0);
    if ((cur == NULL) || (cur == value))
        return(1);

    if (space) {
	while (IS_BLANK_CH(*cur))
            cur++;
    }

    return(*cur != 0);
}

/**
 * Check that a value conforms to the lexical space of NMToken
 *
 * @param value  the value to check
 * @param space  allow spaces in front and end of the string
 * @returns 0 if this validates, a positive error code number otherwise
 *         and -1 in case of internal or API error.
 */
int
xmlValidateNMToken(const xmlChar *value, int space) {
    const xmlChar *cur;

    if (value == NULL)
        return(-1);

    cur = value;

    if (space) {
	while (IS_BLANK_CH(*cur))
            cur++;
    }

    value = cur;
    cur = xmlScanName(value, SIZE_MAX, XML_SCAN_NMTOKEN);
    if ((cur == NULL) || (cur == value))
        return(1);

    if (space) {
	while (IS_BLANK_CH(*cur))
            cur++;
    }

    return(*cur != 0);
}

/************************************************************************
 *									*
 *		Allocation and deallocation of basic structures		*
 *									*
 ************************************************************************/

/**
 * Create a new namespace. For a default namespace, `prefix` should be
 * NULL. The namespace URI in `href` is not checked. You should make sure
 * to pass a valid URI.
 *
 * If `node` is provided, it must be an element node. The namespace will
 * be appended to the node's namespace declarations. It is an error if
 * the node already has a definition for the prefix or default
 * namespace.
 *
 * @param node  the element carrying the namespace (optional)
 * @param href  the URI associated
 * @param prefix  the prefix for the namespace (optional)
 * @returns a new namespace pointer or NULL if arguments are invalid,
 * the prefix is already in use or a memory allocation failed.
 */
xmlNs *
xmlNewNs(xmlNode *node, const xmlChar *href, const xmlChar *prefix) {
    xmlNsPtr cur;

    if ((node != NULL) && (node->type != XML_ELEMENT_NODE))
	return(NULL);

    /*
     * Allocate a new Namespace and fill the fields.
     */
    cur = (xmlNsPtr) xmlMalloc(sizeof(xmlNs));
    if (cur == NULL)
	return(NULL);
    memset(cur, 0, sizeof(xmlNs));
    cur->type = XML_LOCAL_NAMESPACE;

    if (href != NULL) {
	cur->href = xmlStrdup(href);
        if (cur->href == NULL)
            goto error;
    }
    if (prefix != NULL) {
	cur->prefix = xmlStrdup(prefix);
        if (cur->prefix == NULL)
            goto error;
    }

    /*
     * Add it at the end to preserve parsing order ...
     * and checks for existing use of the prefix
     */
    if (node != NULL) {
	if (node->nsDef == NULL) {
	    node->nsDef = cur;
	} else {
	    xmlNsPtr prev = node->nsDef;

	    if ((xmlStrEqual(prev->prefix, cur->prefix)) &&
                (prev->href != NULL))
                goto error;
	    while (prev->next != NULL) {
	        prev = prev->next;
		if ((xmlStrEqual(prev->prefix, cur->prefix)) &&
                    (prev->href != NULL))
                    goto error;
	    }
	    prev->next = cur;
	}
    }
    return(cur);

error:
    xmlFreeNs(cur);
    return(NULL);
}

/**
 * Set the namespace of an element or attribute node. Passing a NULL
 * namespace unsets the namespace.
 *
 * @param node  a node in the document
 * @param ns  a namespace pointer (optional)
 */
void
xmlSetNs(xmlNode *node, xmlNs *ns) {
    if (node == NULL) {
	return;
    }
    if ((node->type == XML_ELEMENT_NODE) ||
        (node->type == XML_ATTRIBUTE_NODE))
	node->ns = ns;
}

/**
 * Free an xmlNs object.
 *
 * @param cur  the namespace pointer
 */
void
xmlFreeNs(xmlNs *cur) {
    if (cur == NULL) {
	return;
    }
    if (cur->href != NULL) xmlFree((char *) cur->href);
    if (cur->prefix != NULL) xmlFree((char *) cur->prefix);
    xmlFree(cur);
}

/**
 * Free a list of xmlNs objects.
 *
 * @param cur  the first namespace pointer
 */
void
xmlFreeNsList(xmlNs *cur) {
    xmlNsPtr next;
    if (cur == NULL) {
	return;
    }
    while (cur != NULL) {
        next = cur->next;
        xmlFreeNs(cur);
	cur = next;
    }
}

/**
 * Create a DTD node.
 *
 * If a document is provided, it is an error if it already has an
 * external subset. If the document has no external subset, it
 * will be set to the created DTD.
 *
 * To create an internal subset, use #xmlCreateIntSubset.
 *
 * @param doc  the document pointer (optional)
 * @param name  the DTD name (optional)
 * @param publicId  public identifier of the DTD (optional)
 * @param systemId  system identifier (URL) of the DTD (optional)
 * @returns a pointer to the new DTD object or NULL if arguments are
 * invalid or a memory allocation failed.
 */
xmlDtd *
xmlNewDtd(xmlDoc *doc, const xmlChar *name, const xmlChar *publicId,
          const xmlChar *systemId) {
    xmlDtdPtr cur;

    if ((doc != NULL) && (doc->extSubset != NULL)) {
	return(NULL);
    }

    /*
     * Allocate a new DTD and fill the fields.
     */
    cur = (xmlDtdPtr) xmlMalloc(sizeof(xmlDtd));
    if (cur == NULL)
	return(NULL);
    memset(cur, 0 , sizeof(xmlDtd));
    cur->type = XML_DTD_NODE;

    if (name != NULL) {
	cur->name = xmlStrdup(name);
        if (cur->name == NULL)
            goto error;
    }
    if (publicId != NULL) {
	cur->ExternalID = xmlStrdup(publicId);
        if (cur->ExternalID == NULL)
            goto error;
    }
    if (systemId != NULL) {
	cur->SystemID = xmlStrdup(systemId);
        if (cur->SystemID == NULL)
            goto error;
    }
    if (doc != NULL)
	doc->extSubset = cur;
    cur->doc = doc;

    if ((xmlRegisterCallbacks) && (xmlRegisterNodeDefaultValue))
	xmlRegisterNodeDefaultValue((xmlNodePtr)cur);
    return(cur);

error:
    xmlFreeDtd(cur);
    return(NULL);
}

/**
 * Get the internal subset of a document.
 *
 * @param doc  the document pointer
 * @returns a pointer to the DTD object or NULL if not found.
 */
xmlDtd *
xmlGetIntSubset(const xmlDoc *doc) {
    xmlNodePtr cur;

    if (doc == NULL)
	return(NULL);
    cur = doc->children;
    while (cur != NULL) {
	if (cur->type == XML_DTD_NODE)
	    return((xmlDtdPtr) cur);
	cur = cur->next;
    }
    return((xmlDtdPtr) doc->intSubset);
}

/**
 * Create a DTD node.
 *
 * If a document is provided and it already has an internal subset,
 * the existing DTD object is returned without creating a new object.
 * If the document has no internal subset, it will be set to the
 * created DTD.
 *
 * @param doc  the document pointer (optional)
 * @param name  the DTD name (optional)
 * @param publicId  public identifier of the DTD (optional)
 * @param systemId  system identifier (URL) of the DTD (optional)
 * @returns a pointer to the new or existing DTD object or NULL if
 * arguments are invalid or a memory allocation failed.
 */
xmlDtd *
xmlCreateIntSubset(xmlDoc *doc, const xmlChar *name, const xmlChar *publicId,
                   const xmlChar *systemId) {
    xmlDtdPtr cur;

    if (doc != NULL) {
        cur = xmlGetIntSubset(doc);
        if (cur != NULL)
            return(cur);
    }

    /*
     * Allocate a new DTD and fill the fields.
     */
    cur = (xmlDtdPtr) xmlMalloc(sizeof(xmlDtd));
    if (cur == NULL)
	return(NULL);
    memset(cur, 0, sizeof(xmlDtd));
    cur->type = XML_DTD_NODE;

    if (name != NULL) {
	cur->name = xmlStrdup(name);
	if (cur->name == NULL)
            goto error;
    }
    if (publicId != NULL) {
	cur->ExternalID = xmlStrdup(publicId);
	if (cur->ExternalID  == NULL)
            goto error;
    }
    if (systemId != NULL) {
	cur->SystemID = xmlStrdup(systemId);
	if (cur->SystemID == NULL)
            goto error;
    }
    if (doc != NULL) {
	doc->intSubset = cur;
	cur->parent = doc;
	cur->doc = doc;
	if (doc->children == NULL) {
	    doc->children = (xmlNodePtr) cur;
	    doc->last = (xmlNodePtr) cur;
	} else {
	    if (doc->type == XML_HTML_DOCUMENT_NODE) {
		xmlNodePtr prev;

		prev = doc->children;
		prev->prev = (xmlNodePtr) cur;
		cur->next = prev;
		doc->children = (xmlNodePtr) cur;
	    } else {
		xmlNodePtr next;

		next = doc->children;
		while ((next != NULL) && (next->type != XML_ELEMENT_NODE))
		    next = next->next;
		if (next == NULL) {
		    cur->prev = doc->last;
		    cur->prev->next = (xmlNodePtr) cur;
		    cur->next = NULL;
		    doc->last = (xmlNodePtr) cur;
		} else {
		    cur->next = next;
		    cur->prev = next->prev;
		    if (cur->prev == NULL)
			doc->children = (xmlNodePtr) cur;
		    else
			cur->prev->next = (xmlNodePtr) cur;
		    next->prev = (xmlNodePtr) cur;
		}
	    }
	}
    }

    if ((xmlRegisterCallbacks) && (xmlRegisterNodeDefaultValue))
	xmlRegisterNodeDefaultValue((xmlNodePtr)cur);
    return(cur);

error:
    xmlFreeDtd(cur);
    return(NULL);
}

/**
 * Free a string if it is not owned by the "dict" dictionary in the
 * current scope
 *
 * @param str  a string
 */
#define DICT_FREE(str)						\
	if ((str) && ((!dict) ||				\
	    (xmlDictOwns(dict, (const xmlChar *)(str)) == 0)))	\
	    xmlFree((char *)(str));

/**
 * Free a DTD structure.
 *
 * @param cur  the DTD structure to free up
 */
void
xmlFreeDtd(xmlDtd *cur) {
    xmlDictPtr dict = NULL;

    if (cur == NULL) {
	return;
    }
    if (cur->doc != NULL) dict = cur->doc->dict;

    if ((xmlRegisterCallbacks) && (xmlDeregisterNodeDefaultValue))
	xmlDeregisterNodeDefaultValue((xmlNodePtr)cur);

    if (cur->children != NULL) {
	xmlNodePtr next, c = cur->children;

	/*
	 * Cleanup all nodes which are not part of the specific lists
	 * of notations, elements, attributes and entities.
	 */
        while (c != NULL) {
	    next = c->next;
	    if ((c->type != XML_ELEMENT_DECL) &&
		(c->type != XML_ATTRIBUTE_DECL) &&
		(c->type != XML_ENTITY_DECL)) {
		xmlUnlinkNodeInternal(c);
		xmlFreeNode(c);
	    }
	    c = next;
	}
    }

    DICT_FREE(cur->name)

    if (cur->SystemID != NULL)
        xmlFree(cur->SystemID);
    if (cur->ExternalID != NULL)
        xmlFree(cur->ExternalID);

    /* TODO !!! */
    if (cur->notations != NULL)
        xmlFreeNotationTable((xmlNotationTablePtr) cur->notations);

    if (cur->elements != NULL)
        xmlFreeElementTable((xmlElementTablePtr) cur->elements);
    if (cur->attributes != NULL)
        xmlFreeAttributeTable((xmlAttributeTablePtr) cur->attributes);
    if (cur->entities != NULL)
        xmlFreeEntitiesTable((xmlEntitiesTablePtr) cur->entities);
    if (cur->pentities != NULL)
        xmlFreeEntitiesTable((xmlEntitiesTablePtr) cur->pentities);

    xmlFree(cur);
}

/**
 * Creates a new XML document. If version is NULL, `"1.0"` is used.
 *
 * @param version  XML version string like `"1.0"` (optional)
 * @returns a new document or NULL if a memory allocation failed.
 */
xmlDoc *
xmlNewDoc(const xmlChar *version) {
    xmlDocPtr cur;

    if (version == NULL)
	version = (const xmlChar *) "1.0";

    /*
     * Allocate a new document and fill the fields.
     */
    cur = (xmlDocPtr) xmlMalloc(sizeof(xmlDoc));
    if (cur == NULL)
	return(NULL);
    memset(cur, 0, sizeof(xmlDoc));
    cur->type = XML_DOCUMENT_NODE;

    cur->version = xmlStrdup(version);
    if (cur->version == NULL) {
	xmlFree(cur);
	return(NULL);
    }
    cur->standalone = -1;
    cur->compression = -1; /* not initialized */
    cur->doc = cur;
    cur->parseFlags = 0;
    cur->properties = XML_DOC_USERBUILT;
    /*
     * The in memory encoding is always UTF8
     * This field will never change and would
     * be obsolete if not for binary compatibility.
     */
    cur->charset = XML_CHAR_ENCODING_UTF8;

    if ((xmlRegisterCallbacks) && (xmlRegisterNodeDefaultValue))
	xmlRegisterNodeDefaultValue((xmlNodePtr)cur);
    return(cur);
}

/**
 * Free a document including all children and associated DTDs.
 *
 * @param cur  pointer to the document
 */
void
xmlFreeDoc(xmlDoc *cur) {
    xmlDtdPtr extSubset, intSubset;
    xmlDictPtr dict = NULL;

    if (cur == NULL) {
	return;
    }

    dict = cur->dict;

    if ((xmlRegisterCallbacks) && (xmlDeregisterNodeDefaultValue))
	xmlDeregisterNodeDefaultValue((xmlNodePtr)cur);

    /*
     * Do this before freeing the children list to avoid ID lookups
     */
    if (cur->ids != NULL) xmlFreeIDTable((xmlIDTablePtr) cur->ids);
    cur->ids = NULL;
    if (cur->refs != NULL) xmlFreeRefTable((xmlRefTablePtr) cur->refs);
    cur->refs = NULL;
    extSubset = cur->extSubset;
    intSubset = cur->intSubset;
    if (intSubset == extSubset)
	extSubset = NULL;
    if (extSubset != NULL) {
	xmlUnlinkNodeInternal((xmlNodePtr) cur->extSubset);
	cur->extSubset = NULL;
	xmlFreeDtd(extSubset);
    }
    if (intSubset != NULL) {
	xmlUnlinkNodeInternal((xmlNodePtr) cur->intSubset);
	cur->intSubset = NULL;
	xmlFreeDtd(intSubset);
    }

    if (cur->children != NULL) xmlFreeNodeList(cur->children);
    if (cur->oldNs != NULL) xmlFreeNsList(cur->oldNs);

    DICT_FREE(cur->name)

    if (cur->version != NULL)
        xmlFree(cur->version);
    if (cur->encoding != NULL)
        xmlFree(cur->encoding);
    if (cur->URL != NULL)
        xmlFree(cur->URL);

    xmlFree(cur);
    if (dict) xmlDictFree(dict);
}

/**
 * Parse an attribute value and replace the node's children with
 * the resulting node list.
 *
 * `content` is expected to be a valid XML attribute value possibly
 * containing character and entity references. Syntax errors
 * and references to undeclared entities are ignored silently.
 * Only references are handled, nested elements, comments or PIs are
 * not.
 *
 * This function is used by the SAX parser to parse attribute
 * values a second time if entities aren't substituted. It's also
 * used to parse node content in the tree API, but this is a
 * historical mistake.
 *
 * @param doc  a document (optional)
 * @param attr  an attribute or element (optional)
 * @param value  an attribute value
 * @param len  maximum length of the attribute value
 * @param listPtr  pointer to the resulting node list (optional)
 * @returns 0 on success, -1 if a memory allocation failed.
 */
int
xmlNodeParseAttValue(const xmlDoc *doc, xmlAttr *attr,
                     const xmlChar *value, size_t len, xmlNode **listPtr) {
    xmlNodePtr head = NULL, last = NULL;
    xmlNodePtr node;
    xmlChar *val = NULL;
    const xmlChar *cur;
    const xmlChar *q;
    xmlEntityPtr ent;
    xmlBufPtr buf;
    size_t remaining = len;

    if (listPtr != NULL)
        *listPtr = NULL;

    if ((value == NULL) || (value[0] == 0))
        goto done;

    cur = value;

    buf = xmlBufCreate(50);
    if (buf == NULL)
        return(-1);

    q = cur;
    while ((remaining > 0) && (*cur != 0)) {
	if (cur[0] == '&') {
	    int charval = 0;

	    /*
	     * Save the current text.
	     */
            if (cur != q) {
		if (xmlBufAdd(buf, q, cur - q))
		    goto out;
	        q = cur;
	    }

	    if ((remaining > 2) && (cur[1] == '#') && (cur[2] == 'x')) {
	        int tmp = 0;

		cur += 3;
                remaining -= 3;
		while ((remaining > 0) && ((tmp = *cur) != ';')) {
		    if ((tmp >= '0') && (tmp <= '9'))
			charval = charval * 16 + (tmp - '0');
		    else if ((tmp >= 'a') && (tmp <= 'f'))
			charval = charval * 16 + (tmp - 'a') + 10;
		    else if ((tmp >= 'A') && (tmp <= 'F'))
			charval = charval * 16 + (tmp - 'A') + 10;
		    else {
			charval = 0;
			break;
		    }
                    if (charval > 0x110000)
                        charval = 0x110000;
		    cur++;
                    remaining--;
		}
		if (tmp == ';') {
		    cur++;
                    remaining--;
                }
		q = cur;
	    } else if ((remaining > 1) && (cur[1] == '#')) {
	        int tmp = 0;

		cur += 2;
                remaining -= 2;
		while ((remaining > 0) && ((tmp = *cur) != ';')) {
		    if ((tmp >= '0') && (tmp <= '9'))
			charval = charval * 10 + (tmp - '0');
		    else {
			charval = 0;
			break;
		    }
                    if (charval > 0x110000)
                        charval = 0x110000;
		    cur++;
                    remaining--;
		}
		if (tmp == ';') {
		    cur++;
                    remaining--;
                }
		q = cur;
	    } else {
		/*
		 * Read the entity string
		 */
		cur++;
                remaining--;
		q = cur;
		while ((remaining > 0) && (*cur != 0) && (*cur != ';')) {
                    cur++;
                    remaining--;
                }
		if ((remaining <= 0) || (*cur == 0))
		    break;
		if (cur != q) {
		    val = xmlStrndup(q, cur - q);
                    if (val == NULL)
                        goto out;
		    ent = xmlGetDocEntity(doc, val);
		    if ((ent != NULL) &&
			(ent->etype == XML_INTERNAL_PREDEFINED_ENTITY)) {
                        /*
                         * Predefined entities don't generate nodes
                         */
			if (xmlBufCat(buf, ent->content))
			    goto out;
                    } else if (ent == NULL ||
                               (ent->flags & XML_ENT_EXPANDING) == 0) {
			/*
			 * Flush buffer so far
			 */
			if (!xmlBufIsEmpty(buf)) {
			    node = xmlNewDocText(doc, NULL);
			    if (node == NULL)
				goto out;
			    node->content = xmlBufDetach(buf);
                            node->parent = (xmlNode *) attr;

			    if (last == NULL) {
				head = node;
			    } else {
                                last->next = node;
                                node->prev = last;
			    }
                            last = node;
			}

			if ((ent != NULL) &&
                            ((ent->flags & XML_ENT_PARSED) == 0) &&
                            (ent->content != NULL)) {
                            int res;

                            ent->flags |= XML_ENT_EXPANDING;
                            res = xmlNodeParseAttValue(doc, (xmlAttr *) ent,
                                                       ent->content, SIZE_MAX,
                                                       NULL);
                            ent->flags &= ~XML_ENT_EXPANDING;
                            if (res < 0)
                                goto out;
                            ent->flags |= XML_ENT_PARSED;
			}

			/*
			 * Create a new REFERENCE_REF node
			 */
			node = xmlNewEntityRef((xmlDocPtr) doc, val);
                        val = NULL;
			if (node == NULL)
			    goto out;
                        node->parent = (xmlNode *) attr;
                        node->last = (xmlNodePtr) ent;
                        if (ent != NULL) {
                            node->children = (xmlNodePtr) ent;
                            node->content = ent->content;
                        }

			if (last == NULL) {
			    head = node;
			} else {
                            last->next = node;
                            node->prev = last;
			}
                        last = node;
		    }
		    xmlFree(val);
                    val = NULL;
		}
		cur++;
                remaining--;
		q = cur;
	    }
	    if (charval != 0) {
		xmlChar buffer[10];
		int l;

                if (charval >= 0x110000)
                    charval = 0xFFFD; /* replacement character */

		l = xmlCopyCharMultiByte(buffer, charval);
		buffer[l] = 0;

		if (xmlBufCat(buf, buffer))
		    goto out;
	    }
	} else {
	    cur++;
            remaining--;
        }
    }

    if (cur != q) {
        /*
	 * Handle the last piece of text.
	 */
	if (xmlBufAdd(buf, q, cur - q))
	    goto out;
    }

    if (!xmlBufIsEmpty(buf)) {
	node = xmlNewDocText(doc, NULL);
	if (node == NULL)
            goto out;
        node->parent = (xmlNode *) attr;
	node->content = xmlBufDetach(buf);

	if (last == NULL) {
	    head = node;
	} else {
            last->next = node;
            node->prev = last;
	}
        last = node;
    } else if (head == NULL) {
        head = xmlNewDocText(doc, BAD_CAST "");
        if (head == NULL)
            goto out;
        head->parent = (xmlNode *) attr;
        last = head;
    }

    xmlBufFree(buf);

done:
    if (attr != NULL) {
        if (attr->children != NULL)
            xmlFreeNodeList(attr->children);
        attr->children = head;
        attr->last = last;
    }

    if (listPtr != NULL)
        *listPtr = head;

    return(0);

out:
    xmlBufFree(buf);
    if (val != NULL)
        xmlFree(val);
    if (head != NULL)
        xmlFreeNodeList(head);
    return(-1);
}

/**
 * See #xmlStringGetNodeList.
 *
 * @deprecated Internal function, don't use.
 *
 * @param doc  a document (optional)
 * @param value  an attribute value
 * @param len  maximum length of the attribute value
 * @returns a pointer to the first child or NULL if the value if empty
 * or a memory allocation failed.
 */
xmlNode *
xmlStringLenGetNodeList(const xmlDoc *doc, const xmlChar *value, int len) {
    xmlNodePtr ret;
    size_t maxSize = len < 0 ? SIZE_MAX : (size_t) len;

    xmlNodeParseAttValue(doc, NULL, value, maxSize, &ret);
    return(ret);
}

/**
 * Parse an attribute value and build a node list with
 * text and entity reference nodes. The resulting nodes will be
 * associated with the document if provided. The document is also
 * used to look up entities.
 *
 * @deprecated Internal function, don't use.
 *
 * The input is not validated. Syntax errors or references to
 * undeclared entities will be ignored silently with unspecified
 * results.
 *
 * @param doc  a document (optional)
 * @param value  an attribute value
 * @returns a pointer to the first child or NULL if the value if empty
 * or a memory allocation failed.
 */
xmlNode *
xmlStringGetNodeList(const xmlDoc *doc, const xmlChar *value) {
    xmlNodePtr ret;

    xmlNodeParseAttValue(doc, NULL, value, SIZE_MAX, &ret);
    return(ret);
}

/**
 * @param node  a node list
 * @param escape  whether to escape characters and keep entity refs
 * @param flags  escape flags
 * @returns a pointer to the string.
 */
xmlChar *
xmlNodeListGetStringInternal(const xmlNode *node, int escape, int flags) {
    xmlBufPtr buf;
    xmlChar *ret;

    if (node == NULL)
        return(xmlStrdup(BAD_CAST ""));

    if ((escape == 0) &&
        ((node->type == XML_TEXT_NODE) ||
         (node->type == XML_CDATA_SECTION_NODE)) &&
        (node->next == NULL)) {
        if (node->content == NULL)
            return(xmlStrdup(BAD_CAST ""));
        return(xmlStrdup(node->content));
    }

    buf = xmlBufCreate(50);
    if (buf == NULL)
        return(NULL);

    while (node != NULL) {
        if ((node->type == XML_TEXT_NODE) ||
            (node->type == XML_CDATA_SECTION_NODE)) {
            if (node->content != NULL) {
                if (escape == 0) {
                    xmlBufCat(buf, node->content);
                } else {
                    xmlChar *encoded;

                    encoded = xmlEscapeText(node->content, flags);
                    if (encoded == NULL)
                        goto error;
                    xmlBufCat(buf, encoded);
                    xmlFree(encoded);
                }
            }
        } else if (node->type == XML_ENTITY_REF_NODE) {
            if (escape == 0) {
                xmlBufGetNodeContent(buf, node);
            } else {
                xmlBufAdd(buf, BAD_CAST "&", 1);
                xmlBufCat(buf, node->name);
                xmlBufAdd(buf, BAD_CAST ";", 1);
            }
        }

        node = node->next;
    }

    ret = xmlBufDetach(buf);
    xmlBufFree(buf);
    return(ret);

error:
    xmlBufFree(buf);
    return(NULL);
}

/**
 * Serializes attribute children (text and entity reference nodes)
 * into a string.
 *
 * If `inLine` is true, entity references will be substituted.
 * Otherwise, entity references will be kept and special characters
 * like `&` as well as non-ASCII chars will be escaped. See
 * #xmlEncodeEntitiesReentrant for details. If `list` is the child
 * of an attribute, escaping rules apply are adjusted.
 *
 * See #xmlNodeListGetRawString for an alternative option.
 *
 * @param doc  a document (optional)
 * @param list  a node list of attribute children
 * @param inLine  whether entity references are substituted
 * @returns a string or NULL if a memory allocation failed.
 */
xmlChar *
xmlNodeListGetString(xmlDoc *doc, const xmlNode *list, int inLine)
{
    int flags = 0;
    int escape = 0;

    /* backward compatibility */
    if (list == NULL)
        return(NULL);

    if (!inLine) {
        escape = 1;

        if ((doc != NULL) && (doc->type == XML_HTML_DOCUMENT_NODE))
            flags |= XML_ESCAPE_HTML;
        else if ((doc == NULL) || (doc->encoding == NULL))
            flags |= XML_ESCAPE_NON_ASCII;

        if ((list->parent != NULL) &&
            (list->parent->type == XML_ATTRIBUTE_NODE))
            flags |= XML_ESCAPE_ATTR;
    }

    return(xmlNodeListGetStringInternal(list, escape, flags));
}

/**
 * Serializes attribute children (text and entity reference nodes)
 * into a string.
 *
 * If `inLine` is true, entity references will be substituted.
 * Otherwise, entity references will be kept and special characters
 * like `&` will be escaped. See #xmlEncodeSpecialChars for
 * details.
 *
 * @param doc  a document (unused)
 * @param list  a node list of attribute children
 * @param inLine  whether entity references are substituted
 * @returns a string or NULL if a memory allocation failed.
 */
xmlChar *
xmlNodeListGetRawString(const xmlDoc *doc ATTRIBUTE_UNUSED,
                        const xmlNode *list, int inLine)
{
    int escape = 0;
    int flags = 0;

    /* backward compatibility */
    if (list == NULL)
        return(NULL);

    if (!inLine) {
        escape = 1;
        flags = XML_ESCAPE_QUOT;
    }

    return(xmlNodeListGetStringInternal(list, escape, flags));
}

static xmlAttrPtr
xmlNewPropInternal(xmlNodePtr node, xmlNsPtr ns,
                   const xmlChar * name, const xmlChar * value,
                   int eatname)
{
    xmlAttrPtr cur;
    xmlDocPtr doc = NULL;

    if ((node != NULL) && (node->type != XML_ELEMENT_NODE)) {
        if ((eatname == 1) &&
	    ((node->doc == NULL) || (node->doc->dict == NULL) ||
	     (!(xmlDictOwns(node->doc->dict, name)))))
            xmlFree((xmlChar *) name);
        return (NULL);
    }

    /*
     * Allocate a new property and fill the fields.
     */
    cur = (xmlAttrPtr) xmlMalloc(sizeof(xmlAttr));
    if (cur == NULL) {
        if ((eatname == 1) &&
	    ((node == NULL) || (node->doc == NULL) ||
             (node->doc->dict == NULL) ||
	     (!(xmlDictOwns(node->doc->dict, name)))))
            xmlFree((xmlChar *) name);
        return (NULL);
    }
    memset(cur, 0, sizeof(xmlAttr));
    cur->type = XML_ATTRIBUTE_NODE;

    cur->parent = node;
    if (node != NULL) {
        doc = node->doc;
        cur->doc = doc;
    }
    cur->ns = ns;

    if (eatname == 0) {
        if ((doc != NULL) && (doc->dict != NULL))
            cur->name = (xmlChar *) xmlDictLookup(doc->dict, name, -1);
        else
            cur->name = xmlStrdup(name);
        if (cur->name == NULL)
            goto error;
    } else
        cur->name = name;

    if (value != NULL) {
        xmlNodePtr tmp;

        cur->children = xmlNewDocText(doc, value);
        if (cur->children == NULL)
            goto error;
        cur->last = NULL;
        tmp = cur->children;
        while (tmp != NULL) {
            tmp->parent = (xmlNodePtr) cur;
            if (tmp->next == NULL)
                cur->last = tmp;
            tmp = tmp->next;
        }

        if (doc != NULL) {
            int res = xmlIsID(doc, node, cur);

            if (res < 0)
                goto error;
            if ((res == 1) && (xmlAddIDSafe(cur, value) < 0))
                goto error;
        }
    }

    /*
     * Add it at the end to preserve parsing order ...
     */
    if (node != NULL) {
        if (node->properties == NULL) {
            node->properties = cur;
        } else {
            xmlAttrPtr prev = node->properties;

            while (prev->next != NULL)
                prev = prev->next;
            prev->next = cur;
            cur->prev = prev;
        }
    }

    if ((xmlRegisterCallbacks) && (xmlRegisterNodeDefaultValue))
        xmlRegisterNodeDefaultValue((xmlNodePtr) cur);
    return (cur);

error:
    xmlFreeProp(cur);
    return(NULL);
}

/**
 * Create an attribute node.
 *
 * If provided, `value` should be a raw, unescaped string.
 *
 * If `node` is provided, the created attribute will be appended without
 * checking for duplicate names. It is an error if `node` is not an
 * element.
 *
 * @param node  the parent node (optional)
 * @param name  the name of the attribute
 * @param value  the value of the attribute (optional)
 * @returns a pointer to the attribute or NULL if arguments are invalid
 * or a memory allocation failed.
 */
xmlAttr *
xmlNewProp(xmlNode *node, const xmlChar *name, const xmlChar *value) {

    if (name == NULL) {
	return(NULL);
    }

	return xmlNewPropInternal(node, NULL, name, value, 0);
}

/**
 * Create an attribute node.
 *
 * If provided, `value` should be a raw, unescaped string.
 *
 * If `node` is provided, the created attribute will be appended without
 * checking for duplicate names. It is an error if `node` is not an
 * element.
 *
 * @param node  the parent node (optional)
 * @param ns  the namespace (optional)
 * @param name  the local name of the attribute
 * @param value  the value of the attribute (optional)
 * @returns a pointer to the attribute or NULL if arguments are invalid
 * or a memory allocation failed.
 */
xmlAttr *
xmlNewNsProp(xmlNode *node, xmlNs *ns, const xmlChar *name,
           const xmlChar *value) {

    if (name == NULL) {
	return(NULL);
    }

    return xmlNewPropInternal(node, ns, name, value, 0);
}

/**
 * Create an attribute node.
 *
 * Like #xmlNewNsProp, but the `name` string will be used directly
 * without making a copy. Takes ownership of `name` which will also
 * be freed on error.
 *
 * @param node  the parent node (optional)
 * @param ns  the namespace (optional)
 * @param name  the local name of the attribute
 * @param value  the value of the attribute (optional)
 * @returns a pointer to the attribute or NULL if arguments are invalid
 * or a memory allocation failed.
 */
xmlAttr *
xmlNewNsPropEatName(xmlNode *node, xmlNs *ns, xmlChar *name,
           const xmlChar *value) {

    if (name == NULL) {
	return(NULL);
    }

    return xmlNewPropInternal(node, ns, name, value, 1);
}

/**
 * Create an attribute node.
 *
 * If provided, `value` is expected to be a valid XML attribute value
 * possibly containing character and entity references. Syntax errors
 * and references to undeclared entities are ignored silently.
 * If you want to pass a raw string, see #xmlNewProp.
 *
 * @param doc  the target document (optional)
 * @param name  the name of the attribute
 * @param value  attribute value with XML references (optional)
 * @returns a pointer to the attribute or NULL if arguments are invalid
 * or a memory allocation failed.
 */
xmlAttr *
xmlNewDocProp(xmlDoc *doc, const xmlChar *name, const xmlChar *value) {
    xmlAttrPtr cur;

    if (name == NULL) {
	return(NULL);
    }

    /*
     * Allocate a new property and fill the fields.
     */
    cur = (xmlAttrPtr) xmlMalloc(sizeof(xmlAttr));
    if (cur == NULL)
	return(NULL);
    memset(cur, 0, sizeof(xmlAttr));
    cur->type = XML_ATTRIBUTE_NODE;

    if ((doc != NULL) && (doc->dict != NULL))
	cur->name = xmlDictLookup(doc->dict, name, -1);
    else
	cur->name = xmlStrdup(name);
    if (cur->name == NULL)
        goto error;
    cur->doc = doc;
    if (value != NULL) {
        /*
         * We shouldn't parse the attribute value here,
         * but the API can't be changed.
         */
        if (xmlNodeParseAttValue(doc, cur, value, SIZE_MAX, NULL) < 0)
            goto error;
    }

    if ((xmlRegisterCallbacks) && (xmlRegisterNodeDefaultValue))
	xmlRegisterNodeDefaultValue((xmlNodePtr)cur);
    return(cur);

error:
    xmlFreeProp(cur);
    return(NULL);
}

/**
 * Free an attribute list including all children.
 *
 * @param cur  the first attribute in the list
 */
void
xmlFreePropList(xmlAttr *cur) {
    xmlAttrPtr next;
    if (cur == NULL) return;
    while (cur != NULL) {
        next = cur->next;
        xmlFreeProp(cur);
	cur = next;
    }
}

/**
 * Free an attribute including all children.
 *
 * @param cur  an attribute
 */
void
xmlFreeProp(xmlAttr *cur) {
    xmlDictPtr dict = NULL;
    if (cur == NULL) return;

    if (cur->doc != NULL) dict = cur->doc->dict;

    if ((xmlRegisterCallbacks) && (xmlDeregisterNodeDefaultValue))
	xmlDeregisterNodeDefaultValue((xmlNodePtr)cur);

    /* Check for ID removal -> leading to invalid references ! */
    if (cur->doc != NULL && cur->id != NULL) {
        xmlRemoveID(cur->doc, cur);
    }
    if (cur->children != NULL) xmlFreeNodeList(cur->children);
    DICT_FREE(cur->name)
    xmlFree(cur);
}

/**
 * Unlink and free an attribute including all children.
 *
 * Note this doesn't work for namespace declarations.
 *
 * The attribute must have a non-NULL parent pointer.
 *
 * @param cur  an attribute
 * @returns 0 on success or -1 if the attribute was not found or
 * arguments are invalid.
 */
int
xmlRemoveProp(xmlAttr *cur) {
    xmlAttrPtr tmp;
    if (cur == NULL) {
	return(-1);
    }
    if (cur->parent == NULL) {
	return(-1);
    }
    tmp = cur->parent->properties;
    if (tmp == cur) {
        cur->parent->properties = cur->next;
		if (cur->next != NULL)
			cur->next->prev = NULL;
	xmlFreeProp(cur);
	return(0);
    }
    while (tmp != NULL) {
	if (tmp->next == cur) {
	    tmp->next = cur->next;
	    if (tmp->next != NULL)
		tmp->next->prev = tmp;
	    xmlFreeProp(cur);
	    return(0);
	}
        tmp = tmp->next;
    }
    return(-1);
}

/**
 * Create a processing instruction object.
 *
 * @param doc  the target document (optional)
 * @param name  the processing instruction target
 * @param content  the PI content (optional)
 * @returns a pointer to the new node object or NULL if arguments are
 * invalid or a memory allocation failed.
 */
xmlNode *
xmlNewDocPI(xmlDoc *doc, const xmlChar *name, const xmlChar *content) {
    xmlNodePtr cur;

    if (name == NULL) {
	return(NULL);
    }

    /*
     * Allocate a new node and fill the fields.
     */
    cur = (xmlNodePtr) xmlMalloc(sizeof(xmlNode));
    if (cur == NULL)
	return(NULL);
    memset(cur, 0, sizeof(xmlNode));
    cur->type = XML_PI_NODE;
    cur->doc = doc;

    if ((doc != NULL) && (doc->dict != NULL))
        cur->name = xmlDictLookup(doc->dict, name, -1);
    else
	cur->name = xmlStrdup(name);
    if (cur->name == NULL)
        goto error;
    if (content != NULL) {
	cur->content = xmlStrdup(content);
        if (cur->content == NULL)
            goto error;
    }

    if ((xmlRegisterCallbacks) && (xmlRegisterNodeDefaultValue))
	xmlRegisterNodeDefaultValue((xmlNodePtr)cur);
    return(cur);

error:
    xmlFreeNode(cur);
    return(NULL);
}

/**
 * Create a processing instruction node.
 *
 * Use of this function is DISCOURAGED in favor of #xmlNewDocPI.
 *
 * @param name  the processing instruction target
 * @param content  the PI content (optional)
 * @returns a pointer to the new node object or NULL if arguments are
 * invalid or a memory allocation failed.
 */
xmlNode *
xmlNewPI(const xmlChar *name, const xmlChar *content) {
    return(xmlNewDocPI(NULL, name, content));
}

/**
 * Create an element node.
 *
 * Use of this function is DISCOURAGED in favor of #xmlNewDocNode.
 *
 * @param ns  namespace (optional)
 * @param name  the node name
 * @returns a pointer to the new node object or NULL if arguments are
 * invalid or a memory allocation failed.
 */
xmlNode *
xmlNewNode(xmlNs *ns, const xmlChar *name) {
    return(xmlNewDocNode(NULL, ns, name, NULL));
}

/**
 * Create an element node.
 *
 * Use of this function is DISCOURAGED in favor of #xmlNewDocNodeEatName.
 *
 * Like #xmlNewNode, but the `name` string will be used directly
 * without making a copy. Takes ownership of `name` which will also
 * be freed on error.
 *
 * @param ns  namespace (optional)
 * @param name  the node name
 * @returns a pointer to the new node object or NULL if arguments are
 * invalid or a memory allocation failed.
 */
xmlNode *
xmlNewNodeEatName(xmlNs *ns, xmlChar *name) {
    return(xmlNewDocNodeEatName(NULL, ns, name, NULL));
}

static xmlNodePtr
xmlNewElem(xmlDocPtr doc, xmlNsPtr ns, const xmlChar *name,
           const xmlChar *content) {
    xmlNodePtr cur;

    cur = (xmlNodePtr) xmlMalloc(sizeof(xmlNode));
    if (cur == NULL)
	return(NULL);
    memset(cur, 0, sizeof(xmlNode));

    cur->type = XML_ELEMENT_NODE;
    cur->doc = doc;
    cur->name = name;
    cur->ns = ns;

    if (content != NULL) {
        /*
         * We shouldn't parse the content as attribute value here,
         * but the API can't be changed.
         */
        if (xmlNodeParseAttValue(doc, (xmlAttr *) cur, content, SIZE_MAX,
                                 NULL) < 0) {
            /* Don't free name on error */
            xmlFree(cur);
            return(NULL);
        }
    }

    if ((xmlRegisterCallbacks) && (xmlRegisterNodeDefaultValue))
	xmlRegisterNodeDefaultValue((xmlNodePtr)cur);

    return(cur);
}

/**
 * Create an element node.
 *
 * If provided, `content` is expected to be a valid XML attribute value
 * possibly containing character and entity references. Syntax errors
 * and references to undeclared entities are ignored silently.
 * Only references are handled, nested elements, comments or PIs are
 * not. See #xmlNewDocRawNode for an alternative.
 *
 * General notes on object creation:
 *
 * Each node and all its children are associated with the same
 * document. The document should be provided when creating nodes to
 * avoid a performance penalty when adding the node to a document
 * tree. Note that a document only owns nodes reachable from the root
 * node. Unlinked subtrees must be freed manually.
 *
 * @param doc  the target document
 * @param ns  namespace (optional)
 * @param name  the node name
 * @param content  text content with XML references (optional)
 * @returns a pointer to the new node object or NULL if arguments are
 * invalid or a memory allocation failed.
 */
xmlNode *
xmlNewDocNode(xmlDoc *doc, xmlNs *ns,
              const xmlChar *name, const xmlChar *content) {
    xmlNodePtr cur;
    xmlChar *copy;

    if (name == NULL)
        return(NULL);

    if ((doc != NULL) && (doc->dict != NULL)) {
        const xmlChar *dictName = xmlDictLookup(doc->dict, name, -1);

        if (dictName == NULL)
            return(NULL);
        return(xmlNewElem(doc, ns, dictName, content));
    }

    copy = xmlStrdup(name);
    if (copy == NULL)
        return(NULL);

    cur = xmlNewElem(doc, ns, copy, content);
    if (cur == NULL) {
        xmlFree(copy);
        return(NULL);
    }

    return(cur);
}

/**
 * Create an element node.
 *
 * Like #xmlNewDocNode, but the `name` string will be used directly
 * without making a copy. Takes ownership of `name` which will also
 * be freed on error.
 *
 * @param doc  the target document
 * @param ns  namespace (optional)
 * @param name  the node name
 * @param content  text content with XML references (optional)
 * @returns a pointer to the new node object or NULL if arguments are
 * invalid or a memory allocation failed.
 */
xmlNode *
xmlNewDocNodeEatName(xmlDoc *doc, xmlNs *ns,
                     xmlChar *name, const xmlChar *content) {
    xmlNodePtr cur;

    if (name == NULL)
        return(NULL);

    cur = xmlNewElem(doc, ns, name, content);
    if (cur == NULL) {
        /* if name doesn't come from the doc dictionary free it here */
        if ((doc == NULL) ||
            (doc->dict == NULL) ||
            (!xmlDictOwns(doc->dict, name)))
            xmlFree(name);
        return(NULL);
    }

    return(cur);
}

/**
 * Create an element node.
 *
 * If provided, `value` should be a raw, unescaped string.
 *
 * @param doc  the target document
 * @param ns  namespace (optional)
 * @param name  the node name
 * @param content  raw text content (optional)
 * @returns a pointer to the new node object or NULL if arguments are
 * invalid or a memory allocation failed.
 */
xmlNode *
xmlNewDocRawNode(xmlDoc *doc, xmlNs *ns,
                 const xmlChar *name, const xmlChar *content) {
    xmlNodePtr cur;

    cur = xmlNewDocNode(doc, ns, name, NULL);
    if (cur != NULL) {
        cur->doc = doc;
	if (content != NULL) {
            xmlNodePtr text;

	    text = xmlNewDocText(doc, content);
            if (text == NULL) {
                xmlFreeNode(cur);
                return(NULL);
            }

            cur->children = text;
            cur->last = text;
            text->parent = cur;
	}
    }
    return(cur);
}

/**
 * Create a document fragment node.
 *
 * @param doc  the target document (optional)
 * @returns a pointer to the new node object or NULL if a memory
 * allocation failed.
 */
xmlNode *
xmlNewDocFragment(xmlDoc *doc) {
    xmlNodePtr cur;

    /*
     * Allocate a new DocumentFragment node and fill the fields.
     */
    cur = (xmlNodePtr) xmlMalloc(sizeof(xmlNode));
    if (cur == NULL)
	return(NULL);
    memset(cur, 0, sizeof(xmlNode));
    cur->type = XML_DOCUMENT_FRAG_NODE;

    cur->doc = doc;

    if ((xmlRegisterCallbacks) && (xmlRegisterNodeDefaultValue))
	xmlRegisterNodeDefaultValue(cur);
    return(cur);
}

/**
 * Create a text node.
 *
 * Use of this function is DISCOURAGED in favor of #xmlNewDocText.
 *
 * @param content  raw text content (optional)
 * @returns a pointer to the new node object or NULL if a memory
 * allocation failed.
 */
xmlNode *
xmlNewText(const xmlChar *content) {
    xmlNodePtr cur;

    /*
     * Allocate a new node and fill the fields.
     */
    cur = (xmlNodePtr) xmlMalloc(sizeof(xmlNode));
    if (cur == NULL)
	return(NULL);
    memset(cur, 0, sizeof(xmlNode));
    cur->type = XML_TEXT_NODE;

    cur->name = xmlStringText;
    if (content != NULL) {
	cur->content = xmlStrdup(content);
        if (cur->content == NULL)
            goto error;
    }

    if ((xmlRegisterCallbacks) && (xmlRegisterNodeDefaultValue))
	xmlRegisterNodeDefaultValue(cur);
    return(cur);

error:
    xmlFreeNode(cur);
    return(NULL);
}

/**
 * Create a new child element and append it to a parent element.
 *
 * If `ns` is NULL, the newly created element inherits the namespace
 * of the parent.
 *
 * If `content` is provided, a text node will be added to the child
 * element, see #xmlNewDocRawNode.
 *
 * @param parent  the parent node
 * @param ns  a namespace (optional)
 * @param name  the name of the child
 * @param content  raw text content of the child (optional)
 * @returns a pointer to the new node object or NULL if arguments
 * are invalid or a memory allocation failed.
 */
xmlNode *
xmlNewTextChild(xmlNode *parent, xmlNs *ns,
            const xmlChar *name, const xmlChar *content) {
    xmlNodePtr cur, prev;

    if ((parent == NULL) || (name == NULL))
	return(NULL);

    switch (parent->type) {
        case XML_DOCUMENT_NODE:
        case XML_HTML_DOCUMENT_NODE:
        case XML_DOCUMENT_FRAG_NODE:
            break;

        case XML_ELEMENT_NODE:
            if (ns == NULL)
                ns = parent->ns;
            break;

        default:
            return(NULL);
    }

    cur = xmlNewDocRawNode(parent->doc, ns, name, content);
    if (cur == NULL)
        return(NULL);

    /*
     * add the new element at the end of the children list.
     */
    cur->parent = parent;
    if (parent->children == NULL) {
        parent->children = cur;
	parent->last = cur;
    } else {
        prev = parent->last;
	prev->next = cur;
	cur->prev = prev;
	parent->last = cur;
    }

    return(cur);
}

/**
 * Create an empty entity reference node. This function doesn't attempt
 * to look up the entity in `doc`.
 *
 * `name` is consumed.
 *
 * @param doc  the target document (optional)
 * @param name  the entity name
 * @returns a pointer to the new node object or NULL if arguments are
 * invalid or a memory allocation failed.
 */
static xmlNodePtr
xmlNewEntityRef(xmlDocPtr doc, xmlChar *name) {
    xmlNodePtr cur;

    /*
     * Allocate a new node and fill the fields.
     */
    cur = (xmlNodePtr) xmlMalloc(sizeof(xmlNode));
    if (cur == NULL) {
        xmlFree(name);
	return(NULL);
    }
    memset(cur, 0, sizeof(xmlNode));
    cur->type = XML_ENTITY_REF_NODE;
    cur->doc = doc;
    cur->name = name;

    if ((xmlRegisterCallbacks) && (xmlRegisterNodeDefaultValue))
	xmlRegisterNodeDefaultValue(cur);

    return(cur);
}

/**
 * Create an empty entity reference node.
 *
 * This function is MISNAMED. It doesn't create a character reference
 * but an entity reference.
 *
 * This function doesn't attempt to look up the entity in `doc`.
 *
 * Entity names like `&entity;` are handled as well.
 *
 * @param doc  the target document (optional)
 * @param name  the entity name
 * @returns a pointer to the new node object or NULL if arguments are
 * invalid or a memory allocation failed.
 */
xmlNode *
xmlNewCharRef(xmlDoc *doc, const xmlChar *name) {
    xmlChar *copy;

    if (name == NULL)
        return(NULL);

    if (name[0] == '&') {
        int len;
        name++;
	len = xmlStrlen(name);
	if (name[len - 1] == ';')
	    copy = xmlStrndup(name, len - 1);
	else
	    copy = xmlStrndup(name, len);
    } else
	copy = xmlStrdup(name);
    if (copy == NULL)
        return(NULL);

    return(xmlNewEntityRef(doc, copy));
}

/**
 * Create a new entity reference node, linking the result with the
 * entity in `doc` if found.
 *
 * Entity names like `&entity;` are handled as well.
 *
 * @param doc  the target document (optional)
 * @param name  the entity name
 * @returns a pointer to the new node object or NULL if arguments are
 * invalid or a memory allocation failed.
 */
xmlNode *
xmlNewReference(const xmlDoc *doc, const xmlChar *name) {
    xmlNodePtr cur;
    xmlEntityPtr ent;

    if (name == NULL)
        return(NULL);

    /*
     * Allocate a new node and fill the fields.
     */
    cur = (xmlNodePtr) xmlMalloc(sizeof(xmlNode));
    if (cur == NULL)
	return(NULL);
    memset(cur, 0, sizeof(xmlNode));
    cur->type = XML_ENTITY_REF_NODE;

    cur->doc = (xmlDoc *)doc;
    if (name[0] == '&') {
        int len;
        name++;
	len = xmlStrlen(name);
	if (name[len - 1] == ';')
	    cur->name = xmlStrndup(name, len - 1);
	else
	    cur->name = xmlStrndup(name, len);
    } else
	cur->name = xmlStrdup(name);
    if (cur->name == NULL)
        goto error;

    ent = xmlGetDocEntity(doc, cur->name);
    if (ent != NULL) {
	cur->content = ent->content;
	/*
	 * The parent pointer in entity is a DTD pointer and thus is NOT
	 * updated.  Not sure if this is 100% correct.
	 *  -George
	 */
	cur->children = (xmlNodePtr) ent;
	cur->last = (xmlNodePtr) ent;
    }

    if ((xmlRegisterCallbacks) && (xmlRegisterNodeDefaultValue))
	xmlRegisterNodeDefaultValue(cur);
    return(cur);

error:
    xmlFreeNode(cur);
    return(NULL);
}

/**
 * Create a new text node.
 *
 * @param doc  the target document
 * @param content  raw text content (optional)
 * @returns a pointer to the new node object or NULL if a memory
 * allocation failed.
 */
xmlNode *
xmlNewDocText(const xmlDoc *doc, const xmlChar *content) {
    xmlNodePtr cur;

    cur = xmlNewText(content);
    if (cur != NULL) cur->doc = (xmlDoc *)doc;
    return(cur);
}

/**
 * Create a new text node.
 *
 * Use of this function is DISCOURAGED in favor of #xmlNewDocTextLen.
 *
 * @param content  raw text content (optional)
 * @param len  size of text content
 * @returns a pointer to the new node object or NULL if a memory
 * allocation failed.
 */
xmlNode *
xmlNewTextLen(const xmlChar *content, int len) {
    xmlNodePtr cur;

    /*
     * Allocate a new node and fill the fields.
     */
    cur = (xmlNodePtr) xmlMalloc(sizeof(xmlNode));
    if (cur == NULL)
	return(NULL);
    memset(cur, 0, sizeof(xmlNode));
    cur->type = XML_TEXT_NODE;

    cur->name = xmlStringText;
    if (content != NULL) {
	cur->content = xmlStrndup(content, len);
        if (cur->content == NULL) {
            xmlFreeNode(cur);
            return(NULL);
        }
    }

    if ((xmlRegisterCallbacks) && (xmlRegisterNodeDefaultValue))
	xmlRegisterNodeDefaultValue(cur);
    return(cur);
}

/**
 * Create a new text node.
 *
 * @param doc  the target document
 * @param content  raw text content (optional)
 * @param len  size of text content
 * @returns a pointer to the new node object or NULL if a memory
 * allocation failed.
 */
xmlNode *
xmlNewDocTextLen(xmlDoc *doc, const xmlChar *content, int len) {
    xmlNodePtr cur;

    cur = xmlNewTextLen(content, len);
    if (cur != NULL) cur->doc = doc;
    return(cur);
}

/**
 * Use of this function is DISCOURAGED in favor of #xmlNewDocComment.
 *
 * Create a comment node.
 *
 * @param content  the comment content (optional)
 * @returns a pointer to the new node object or NULL if a memory
 * allocation failed.
 */
xmlNode *
xmlNewComment(const xmlChar *content) {
    xmlNodePtr cur;

    /*
     * Allocate a new node and fill the fields.
     */
    cur = (xmlNodePtr) xmlMalloc(sizeof(xmlNode));
    if (cur == NULL)
	return(NULL);
    memset(cur, 0, sizeof(xmlNode));
    cur->type = XML_COMMENT_NODE;

    cur->name = xmlStringComment;
    if (content != NULL) {
	cur->content = xmlStrdup(content);
        if (cur->content == NULL)
            goto error;
    }

    if ((xmlRegisterCallbacks) && (xmlRegisterNodeDefaultValue))
	xmlRegisterNodeDefaultValue(cur);
    return(cur);

error:
    xmlFreeNode(cur);
    return(NULL);
}

/**
 * Create a CDATA section node.
 *
 * @param doc  the target document (optional)
 * @param content  raw text content (optional)
 * @param len  size of text content
 * @returns a pointer to the new node object or NULL if a memory
 * allocation failed.
 */
xmlNode *
xmlNewCDataBlock(xmlDoc *doc, const xmlChar *content, int len) {
    xmlNodePtr cur;

    /*
     * Allocate a new node and fill the fields.
     */
    cur = (xmlNodePtr) xmlMalloc(sizeof(xmlNode));
    if (cur == NULL)
	return(NULL);
    memset(cur, 0, sizeof(xmlNode));
    cur->type = XML_CDATA_SECTION_NODE;
    cur->doc = doc;

    if (content != NULL) {
	cur->content = xmlStrndup(content, len);
        if (cur->content == NULL) {
            xmlFree(cur);
            return(NULL);
        }
    }

    if ((xmlRegisterCallbacks) && (xmlRegisterNodeDefaultValue))
	xmlRegisterNodeDefaultValue(cur);
    return(cur);
}

/**
 * Create a comment node.
 *
 * @param doc  the document
 * @param content  the comment content
 * @returns a pointer to the new node object or NULL if a memory
 * allocation failed.
 */
xmlNode *
xmlNewDocComment(xmlDoc *doc, const xmlChar *content) {
    xmlNodePtr cur;

    cur = xmlNewComment(content);
    if (cur != NULL) cur->doc = doc;
    return(cur);
}

static void
xmlRemoveEntity(xmlEntityPtr ent) {
    xmlDocPtr doc = ent->doc;
    xmlDtdPtr intSubset, extSubset;

    if (doc == NULL)
        return;
    intSubset = doc->intSubset;
    extSubset = doc->extSubset;

    if ((ent->etype == XML_INTERNAL_GENERAL_ENTITY) ||
        (ent->etype == XML_EXTERNAL_GENERAL_PARSED_ENTITY) ||
        (ent->etype == XML_EXTERNAL_GENERAL_UNPARSED_ENTITY)) {
        if (intSubset != NULL) {
            if (xmlHashLookup(intSubset->entities, ent->name) == ent)
                xmlHashRemoveEntry(intSubset->entities, ent->name, NULL);
        }
        if (extSubset != NULL) {
            if (xmlHashLookup(extSubset->entities, ent->name) == ent)
                xmlHashRemoveEntry(extSubset->entities, ent->name, NULL);
        }
    } else if ((ent->etype == XML_INTERNAL_PARAMETER_ENTITY) ||
               (ent->etype == XML_EXTERNAL_PARAMETER_ENTITY)) {
        if (intSubset != NULL) {
            if (xmlHashLookup(intSubset->pentities, ent->name) == ent)
                xmlHashRemoveEntry(intSubset->entities, ent->name, NULL);
        }
        if (extSubset != NULL) {
            if (xmlHashLookup(extSubset->pentities, ent->name) == ent)
                xmlHashRemoveEntry(extSubset->entities, ent->name, NULL);
        }
    }
}

static int
xmlNodeSetDoc(xmlNodePtr node, xmlDocPtr doc) {
    xmlDocPtr oldDoc;
    xmlDictPtr oldDict, newDict;
    int ret = 0;

    /*
     * Remove name and content from old dictionary
     */

    oldDoc = node->doc;
    oldDict = oldDoc ? oldDoc->dict : NULL;
    newDict = doc ? doc->dict : NULL;

    if ((oldDict != NULL) && (oldDict != newDict)) {
        if ((node->name != NULL) &&
            ((node->type == XML_ELEMENT_NODE) ||
             (node->type == XML_ATTRIBUTE_NODE) ||
             (node->type == XML_PI_NODE) ||
             (node->type == XML_ENTITY_REF_NODE)) &&
            (xmlDictOwns(oldDict, node->name))) {
            if (newDict)
                node->name = xmlDictLookup(newDict, node->name, -1);
            else
                node->name = xmlStrdup(node->name);
            if (node->name == NULL)
                ret = -1;
        }

        if ((node->content != NULL) &&
            ((node->type == XML_TEXT_NODE) ||
             (node->type == XML_CDATA_SECTION_NODE)) &&
            (xmlDictOwns(oldDict, node->content))) {
            node->content = xmlStrdup(node->content);
            if (node->content == NULL)
                ret = -1;
        }
    }

    switch (node->type) {
        case XML_ATTRIBUTE_NODE: {
            xmlAttrPtr attr = (xmlAttrPtr) node;

            /*
             * Handle IDs
             *
             * TODO: ID attributes should also be added to the new
             * document, but it's not clear how to handle clashes.
             */
            if (attr->id != NULL)
                xmlRemoveID(oldDoc, attr);

            break;
        }

        case XML_ENTITY_REF_NODE:
            /*
             * Handle entity references
             */
            node->children = NULL;
            node->last = NULL;
            node->content = NULL;

            if ((doc != NULL) &&
                ((doc->intSubset != NULL) || (doc->extSubset != NULL))) {
                xmlEntityPtr ent;

                /*
                * Assign new entity node if available
                */
                ent = xmlGetDocEntity(doc, node->name);
                if (ent != NULL) {
                    node->children = (xmlNodePtr) ent;
                    node->last = (xmlNodePtr) ent;
                    node->content = ent->content;
                }
            }

            break;

        case XML_DTD_NODE:
            if (oldDoc != NULL) {
                if (oldDoc->intSubset == (xmlDtdPtr) node)
                    oldDoc->intSubset = NULL;
                if (oldDoc->extSubset == (xmlDtdPtr) node)
                    oldDoc->extSubset = NULL;
            }

            break;

        case XML_ENTITY_DECL:
            xmlRemoveEntity((xmlEntityPtr) node);
            break;

        /*
         * TODO:
         * - Remove element decls from doc->elements
         * - Remove attribtue decls form doc->attributes
         */

        default:
            break;
    }

    /*
     * Set new document
     */
    node->doc = doc;

    return(ret);
}

/**
 * Associate all nodes in a tree with a new document.
 *
 * This is an internal function which shouldn't be used. It is
 * invoked by functions like #xmlAddChild, #xmlAddSibling or
 * #xmlReplaceNode. `tree` must be the root node of an unlinked
 * subtree.
 *
 * Also copy strings from the old document's dictionary and
 * remove ID attributes from the old ID table.
 *
 * @param tree  root of a subtree
 * @param doc  new document
 * @returns 0 on success. If a memory allocation fails, returns -1.
 * The whole tree will be updated on failure but some strings
 * may be lost.
 */
int
xmlSetTreeDoc(xmlNode *tree, xmlDoc *doc) {
    int ret = 0;

    if ((tree == NULL) || (tree->type == XML_NAMESPACE_DECL))
	return(0);
    if (tree->doc == doc)
        return(0);

    if (tree->type == XML_ELEMENT_NODE) {
        xmlAttrPtr prop = tree->properties;

        while (prop != NULL) {
            if (prop->children != NULL) {
                if (xmlSetListDoc(prop->children, doc) < 0)
                    ret = -1;
            }

            if (xmlNodeSetDoc((xmlNodePtr) prop, doc) < 0)
                ret = -1;

            prop = prop->next;
        }
    }

    if ((tree->children != NULL) &&
        (tree->type != XML_ENTITY_REF_NODE)) {
        if (xmlSetListDoc(tree->children, doc) < 0)
            ret = -1;
    }

    if (xmlNodeSetDoc(tree, doc) < 0)
        ret = -1;

    return(ret);
}

/**
 * Associate all subtrees in `list` with a new document.
 *
 * Internal function, see #xmlSetTreeDoc.
 *
 * @param list  a node list
 * @param doc  new document
 * @returns 0 on success. If a memory allocation fails, returns -1.
 * All subtrees will be updated on failure but some strings
 * may be lost.
 */
int
xmlSetListDoc(xmlNode *list, xmlDoc *doc) {
    xmlNodePtr cur;
    int ret = 0;

    if ((list == NULL) || (list->type == XML_NAMESPACE_DECL))
	return(0);

    cur = list;
    while (cur != NULL) {
	if (cur->doc != doc) {
	    if (xmlSetTreeDoc(cur, doc) < 0)
                ret = -1;
        }
	cur = cur->next;
    }

    return(ret);
}

/**
 * Create a new child element and append it to a parent element.
 *
 * If `ns` is NULL, the newly created element inherits the namespace
 * of the parent.
 *
 * If provided, `content` is expected to be a valid XML attribute
 * value possibly containing character and entity references. Text
 * and entity reference node will be added to the child element,
 * see #xmlNewDocNode.
 *
 * @param parent  the parent node
 * @param ns  a namespace (optional)
 * @param name  the name of the child
 * @param content  text content with XML references (optional)
 * @returns a pointer to the new node object or NULL if arguments
 * are invalid or a memory allocation failed.
 */
xmlNode *
xmlNewChild(xmlNode *parent, xmlNs *ns,
            const xmlChar *name, const xmlChar *content) {
    xmlNodePtr cur, prev;

    if ((parent == NULL) || (name == NULL))
	return(NULL);

    switch (parent->type) {
        case XML_DOCUMENT_NODE:
        case XML_HTML_DOCUMENT_NODE:
        case XML_DOCUMENT_FRAG_NODE:
            break;

        case XML_ELEMENT_NODE:
            if (ns == NULL)
                ns = parent->ns;
            break;

        default:
            return(NULL);
    }

    cur = xmlNewDocNode(parent->doc, ns, name, content);
    if (cur == NULL)
        return(NULL);

    /*
     * add the new element at the end of the children list.
     */
    cur->parent = parent;
    if (parent->children == NULL) {
        parent->children = cur;
	parent->last = cur;
    } else {
        prev = parent->last;
	prev->next = cur;
	cur->prev = prev;
	parent->last = cur;
    }

    return(cur);
}

static void
xmlTextSetContent(xmlNodePtr text, xmlChar *content) {
    if ((text->content != NULL) &&
        (text->content != (xmlChar *) &text->properties)) {
        xmlDocPtr doc = text->doc;

        if ((doc == NULL) ||
            (doc->dict == NULL) ||
            (!xmlDictOwns(doc->dict, text->content)))
            xmlFree(text->content);
    }

    text->content = content;
    text->properties = NULL;
}

static int
xmlTextAddContent(xmlNodePtr text, const xmlChar *content, int len) {
    xmlChar *merged;

    if (content == NULL)
        return(0);

    merged = xmlStrncatNew(text->content, content, len);
    if (merged == NULL)
        return(-1);

    xmlTextSetContent(text, merged);
    return(0);
}

static xmlNodePtr
xmlInsertProp(xmlDocPtr doc, xmlNodePtr cur, xmlNodePtr parent,
              xmlNodePtr prev, xmlNodePtr next) {
    xmlAttrPtr attr;

    if (((prev != NULL) && (prev->type != XML_ATTRIBUTE_NODE)) ||
        ((next != NULL) && (next->type != XML_ATTRIBUTE_NODE)))
        return(NULL);

    /* check if an attribute with the same name exists */
    attr = xmlGetPropNodeInternal(parent, cur->name,
                                  cur->ns ? cur->ns->href : NULL, 0);

    xmlUnlinkNodeInternal(cur);

    if (cur->doc != doc) {
        if (xmlSetTreeDoc(cur, doc) < 0)
            return(NULL);
    }

    cur->parent = parent;
    cur->prev = prev;
    cur->next = next;

    if (prev == NULL) {
        if (parent != NULL)
            parent->properties = (xmlAttrPtr) cur;
    } else {
        prev->next = cur;
    }
    if (next != NULL) {
        next->prev = cur;
    }

    if ((attr != NULL) && (attr != (xmlAttrPtr) cur)) {
        /* different instance, destroy it (attributes must be unique) */
        xmlRemoveProp((xmlAttrPtr) attr);
    }

    return cur;
}

static xmlNodePtr
xmlInsertNode(xmlDocPtr doc, xmlNodePtr cur, xmlNodePtr parent,
              xmlNodePtr prev, xmlNodePtr next, int coalesce) {
    xmlNodePtr oldParent;

    if (cur->type == XML_ATTRIBUTE_NODE)
	return xmlInsertProp(doc, cur, parent, prev, next);

    /*
     * Coalesce text nodes
     */
    if ((coalesce) && (cur->type == XML_TEXT_NODE)) {
	if ((prev != NULL) && (prev->type == XML_TEXT_NODE) &&
            (prev->name == cur->name)) {
            if (xmlTextAddContent(prev, cur->content, -1) < 0)
                return(NULL);
            xmlUnlinkNodeInternal(cur);
	    xmlFreeNode(cur);
	    return(prev);
	}

	if ((next != NULL) && (next->type == XML_TEXT_NODE) &&
            (next->name == cur->name)) {
            if (cur->content != NULL) {
	        xmlChar *merged;

                merged = xmlStrncatNew(cur->content, next->content, -1);
                if (merged == NULL)
                    return(NULL);
                xmlTextSetContent(next, merged);
            }

            xmlUnlinkNodeInternal(cur);
	    xmlFreeNode(cur);
	    return(next);
	}
    }

    /* Unlink */
    oldParent = cur->parent;
    if (oldParent != NULL) {
        if (oldParent->children == cur)
            oldParent->children = cur->next;
        if (oldParent->last == cur)
            oldParent->last = cur->prev;
    }
    if (cur->next != NULL)
        cur->next->prev = cur->prev;
    if (cur->prev != NULL)
        cur->prev->next = cur->next;

    if (cur->doc != doc) {
	if (xmlSetTreeDoc(cur, doc) < 0) {
            /*
             * We shouldn't make any modifications to the inserted
             * tree if a memory allocation fails, but that's hard to
             * implement. The tree has been moved to the target
             * document now but some contents are corrupted.
             * Unlinking is the best we can do.
             */
            cur->parent = NULL;
            cur->prev = NULL;
            cur->next = NULL;
            return(NULL);
        }
    }

    cur->parent = parent;
    cur->prev = prev;
    cur->next = next;

    if (prev == NULL) {
        if (parent != NULL)
            parent->children = cur;
    } else {
        prev->next = cur;
    }
    if (next == NULL) {
        if (parent != NULL)
            parent->last = cur;
    } else {
        next->prev = cur;
    }

    return(cur);
}

/**
 * Unlinks `cur` and inserts it as next sibling after `prev`.
 *
 * Unlike #xmlAddChild this function does not merge text nodes.
 *
 * If `cur` is an attribute node, it is inserted after attribute
 * `prev`. If the attribute list contains an attribute with a name
 * matching `cur`, the old attribute is destroyed.
 *
 * See the notes in #xmlAddChild.
 *
 * @param prev  the target node
 * @param cur  the new node
 * @returns `cur` or a sibling if `cur` was merged. Returns NULL
 * if arguments are invalid or a memory allocation failed.
 */
xmlNode *
xmlAddNextSibling(xmlNode *prev, xmlNode *cur) {
    if ((prev == NULL) || (prev->type == XML_NAMESPACE_DECL) ||
        (cur == NULL) || (cur->type == XML_NAMESPACE_DECL) ||
        (cur == prev))
	return(NULL);

    if (cur == prev->next)
        return(cur);

    return(xmlInsertNode(prev->doc, cur, prev->parent, prev, prev->next, 0));
}

/**
 * Unlinks `cur` and inserts it as previous sibling before `next`.
 *
 * Unlike #xmlAddChild this function does not merge text nodes.
 *
 * If `cur` is an attribute node, it is inserted before attribute
 * `next`. If the attribute list contains an attribute with a name
 * matching `cur`, the old attribute is destroyed.
 *
 * See the notes in #xmlAddChild.
 *
 * @param next  the target node
 * @param cur  the new node
 * @returns `cur` or a sibling if `cur` was merged. Returns NULL
 * if arguments are invalid or a memory allocation failed.
 */
xmlNode *
xmlAddPrevSibling(xmlNode *next, xmlNode *cur) {
    if ((next == NULL) || (next->type == XML_NAMESPACE_DECL) ||
        (cur == NULL) || (cur->type == XML_NAMESPACE_DECL) ||
        (cur == next))
	return(NULL);

    if (cur == next->prev)
        return(cur);

    return(xmlInsertNode(next->doc, cur, next->parent, next->prev, next, 0));
}

/**
 * Unlinks `cur` and inserts it as last sibling of `node`.
 *
 * If `cur` is a text node, it may be merged with an adjacent text
 * node and freed. In this case the text node containing the merged
 * content is returned.
 *
 * If `cur` is an attribute node, it is appended to the attribute
 * list containing `node`. If the attribute list contains an attribute
 * with a name matching `cur`, the old attribute is destroyed.
 *
 * See the notes in #xmlAddChild.
 *
 * @param node  the target node
 * @param cur  the new node
 * @returns `cur` or a sibling if `cur` was merged. Returns NULL
 * if arguments are invalid or a memory allocation failed.
 */
xmlNode *
xmlAddSibling(xmlNode *node, xmlNode *cur) {
    if ((node == NULL) || (node->type == XML_NAMESPACE_DECL) ||
        (cur == NULL) || (cur->type == XML_NAMESPACE_DECL) ||
        (cur == node))
	return(NULL);

    /*
     * Constant time is we can rely on the ->parent->last to find
     * the last sibling.
     */
    if ((node->type != XML_ATTRIBUTE_NODE) && (node->parent != NULL)) {
        if (node->parent->last != NULL)
	    node = node->parent->last;
    } else {
	while (node->next != NULL)
            node = node->next;
    }

    if (cur == node)
        return(cur);

    return(xmlInsertNode(node->doc, cur, node->parent, node, NULL, 1));
}

/**
 * Append a node list to another node.
 *
 * See #xmlAddChild.
 *
 * @param parent  the parent node
 * @param cur  the first node in the list
 * @returns the last child or NULL in case of error.
 */
xmlNode *
xmlAddChildList(xmlNode *parent, xmlNode *cur) {
    xmlNodePtr iter;
    xmlNodePtr prev;
    int oom;

    if ((parent == NULL) || (parent->type == XML_NAMESPACE_DECL)) {
	return(NULL);
    }

    if ((cur == NULL) || (cur->type == XML_NAMESPACE_DECL)) {
	return(NULL);
    }

    oom = 0;
    for (iter = cur; iter != NULL; iter = iter->next) {
	if (iter->doc != parent->doc) {
	    if (xmlSetTreeDoc(iter, parent->doc) < 0)
                oom = 1;
	}
    }
    if (oom)
        return(NULL);

    /*
     * add the first element at the end of the children list.
     */

    if (parent->children == NULL) {
        parent->children = cur;
    } else {
        prev = parent->last;

	/*
	 * If cur and parent->last both are TEXT nodes, then merge them.
	 */
	if ((cur->type == XML_TEXT_NODE) &&
	    (prev->type == XML_TEXT_NODE) &&
	    (cur->name == prev->name)) {
            xmlNodePtr next;

            if (xmlTextAddContent(prev, cur->content, -1) < 0)
                return(NULL);
            next = cur->next;
	    xmlFreeNode(cur);
	    /*
	     * if it's the only child, nothing more to be done.
	     */
	    if (next == NULL)
		return(prev);
	    cur = next;
	}

	prev->next = cur;
	cur->prev = prev;
    }
    while (cur->next != NULL) {
	cur->parent = parent;
        cur = cur->next;
    }
    cur->parent = parent;
    parent->last = cur;

    return(cur);
}

/**
 * Unlink `cur` and append it to the children of `parent`.
 *
 * If `cur` is a text node, it may be merged with an adjacent text
 * node and freed. In this case the text node containing the merged
 * content is returned.
 *
 * If `cur` is an attribute node, it is appended to the attributes of
 * `parent`. If the attribute list contains an attribute with a name
 * matching `cur`, the old attribute is destroyed.
 *
 * Before version 2.13, this function didn't unlink `cur` before
 * moving it. Callers must unlink the node manually if it has
 * siblings.
 *
 * General notes:
 *
 * Move operations like #xmlAddChild can cause element or attribute
 * nodes to reference namespaces that aren't declared in one of
 * their ancestors. This can lead to use-after-free errors if the
 * elements containing the declarations are freed later, especially
 * when moving nodes from one document to another. You should
 * consider calling #xmlReconciliateNs after a move operation to
 * normalize namespaces. Another option is to call
 * #xmlDOMWrapAdoptNode with the target parent before moving a node.
 *
 * For the most part, move operations don't check whether the
 * resulting tree structure is valid. Users must make sure that
 * parent nodes only receive children of valid types. Inserted
 * child nodes must never be an ancestor of the parent node to
 * avoid cycles in the tree structure. In general, only
 * document, document fragments, elements and attributes
 * should be used as parent nodes.
 *
 * When moving a node between documents and a memory allocation
 * fails, the node's content will be corrupted and it will be
 * unlinked. In this case, the node must be freed manually.
 *
 * Moving DTDs between documents isn't supported.
 *
 * @param parent  the parent node
 * @param cur  the child node
 * @returns `cur` or a sibling if `cur` was merged. Returns NULL
 * if arguments are invalid or a memory allocation failed.
 */
xmlNode *
xmlAddChild(xmlNode *parent, xmlNode *cur) {
    xmlNodePtr prev;

    if ((parent == NULL) || (parent->type == XML_NAMESPACE_DECL) ||
        (cur == NULL) || (cur->type == XML_NAMESPACE_DECL) ||
        (parent == cur))
        return(NULL);

    /*
     * If parent is a text node, call xmlTextAddContent. This
     * undocumented quirk should probably be removed.
     */
    if (parent->type == XML_TEXT_NODE) {
        if (xmlTextAddContent(parent, cur->content, -1) < 0)
            return(NULL);
        xmlFreeNode(cur);
        return(parent);
    }

    if (cur->type == XML_ATTRIBUTE_NODE) {
        prev = (xmlNodePtr) parent->properties;
        if (prev != NULL) {
            while (prev->next != NULL)
                prev = prev->next;
        }
    } else {
        prev = parent->last;
    }

    if (cur == prev)
        return(cur);

    return(xmlInsertNode(parent->doc, cur, parent, prev, NULL, 1));
}

/**
 * Find the last child of a node.
 *
 * @param parent  the parent node
 * @returns the last child or NULL if parent has no children.
 */
xmlNode *
xmlGetLastChild(const xmlNode *parent) {
    if ((parent == NULL) || (parent->type == XML_NAMESPACE_DECL)) {
	return(NULL);
    }
    return(parent->last);
}

/*
 * 5 interfaces from DOM ElementTraversal
 */

/**
 * Count the number of child nodes which are elements.
 *
 * Note that entity references are not expanded.
 *
 * @param parent  the parent node
 * @returns the number of element children or 0 if arguments are
 * invalid.
 */
unsigned long
xmlChildElementCount(xmlNode *parent) {
    unsigned long ret = 0;
    xmlNodePtr cur = NULL;

    if (parent == NULL)
        return(0);
    switch (parent->type) {
        case XML_ELEMENT_NODE:
        case XML_DOCUMENT_NODE:
        case XML_DOCUMENT_FRAG_NODE:
        case XML_HTML_DOCUMENT_NODE:
        case XML_ENTITY_DECL:
            cur = parent->children;
            break;
        default:
            return(0);
    }
    while (cur != NULL) {
        if (cur->type == XML_ELEMENT_NODE)
            ret++;
        cur = cur->next;
    }
    return(ret);
}

/**
 * Find the first child node which is an element.
 *
 * Note that entity references are not expanded.
 *
 * @param parent  the parent node
 * @returns the first element or NULL if parent has no children.
 */
xmlNode *
xmlFirstElementChild(xmlNode *parent) {
    xmlNodePtr cur = NULL;

    if (parent == NULL)
        return(NULL);
    switch (parent->type) {
        case XML_ELEMENT_NODE:
        case XML_DOCUMENT_NODE:
        case XML_DOCUMENT_FRAG_NODE:
        case XML_HTML_DOCUMENT_NODE:
        case XML_ENTITY_DECL:
            cur = parent->children;
            break;
        default:
            return(NULL);
    }
    while (cur != NULL) {
        if (cur->type == XML_ELEMENT_NODE)
            return(cur);
        cur = cur->next;
    }
    return(NULL);
}

/**
 * Find the last child node which is an element.
 *
 * Note that entity references are not expanded.
 *
 * @param parent  the parent node
 * @returns the last element or NULL if parent has no children.
 */
xmlNode *
xmlLastElementChild(xmlNode *parent) {
    xmlNodePtr cur = NULL;

    if (parent == NULL)
        return(NULL);
    switch (parent->type) {
        case XML_ELEMENT_NODE:
        case XML_DOCUMENT_NODE:
        case XML_DOCUMENT_FRAG_NODE:
        case XML_HTML_DOCUMENT_NODE:
        case XML_ENTITY_DECL:
            cur = parent->last;
            break;
        default:
            return(NULL);
    }
    while (cur != NULL) {
        if (cur->type == XML_ELEMENT_NODE)
            return(cur);
        cur = cur->prev;
    }
    return(NULL);
}

/**
 * Find the closest preceding sibling which is a element.
 *
 * Note that entity references are not expanded.
 *
 * @param node  the current node
 * @returns the sibling or NULL if no sibling was found.
 */
xmlNode *
xmlPreviousElementSibling(xmlNode *node) {
    if (node == NULL)
        return(NULL);
    switch (node->type) {
        case XML_ELEMENT_NODE:
        case XML_TEXT_NODE:
        case XML_CDATA_SECTION_NODE:
        case XML_ENTITY_REF_NODE:
        case XML_PI_NODE:
        case XML_COMMENT_NODE:
        case XML_XINCLUDE_START:
        case XML_XINCLUDE_END:
            node = node->prev;
            break;
        default:
            return(NULL);
    }
    while (node != NULL) {
        if (node->type == XML_ELEMENT_NODE)
            return(node);
        node = node->prev;
    }
    return(NULL);
}

/**
 * Find the closest following sibling which is a element.
 *
 * Note that entity references are not expanded.
 *
 * @param node  the current node
 * @returns the sibling or NULL if no sibling was found.
 */
xmlNode *
xmlNextElementSibling(xmlNode *node) {
    if (node == NULL)
        return(NULL);
    switch (node->type) {
        case XML_ELEMENT_NODE:
        case XML_TEXT_NODE:
        case XML_CDATA_SECTION_NODE:
        case XML_ENTITY_REF_NODE:
        case XML_PI_NODE:
        case XML_COMMENT_NODE:
        case XML_DTD_NODE:
        case XML_XINCLUDE_START:
        case XML_XINCLUDE_END:
            node = node->next;
            break;
        default:
            return(NULL);
    }
    while (node != NULL) {
        if (node->type == XML_ELEMENT_NODE)
            return(node);
        node = node->next;
    }
    return(NULL);
}

/**
 * Free a node list including all children.
 *
 * @param cur  the first node in the list
 */
void
xmlFreeNodeList(xmlNode *cur) {
    xmlNodePtr next;
    xmlNodePtr parent;
    xmlDictPtr dict = NULL;
    size_t depth = 0;

    if (cur == NULL) return;
    if (cur->type == XML_NAMESPACE_DECL) {
	xmlFreeNsList((xmlNsPtr) cur);
	return;
    }
    if (cur->doc != NULL) dict = cur->doc->dict;
    while (1) {
        while ((cur->children != NULL) &&
               (cur->type != XML_DOCUMENT_NODE) &&
               (cur->type != XML_HTML_DOCUMENT_NODE) &&
               (cur->type != XML_DTD_NODE) &&
               (cur->type != XML_ENTITY_REF_NODE)) {
            cur = cur->children;
            depth += 1;
        }

        next = cur->next;
        parent = cur->parent;
	if ((cur->type == XML_DOCUMENT_NODE) ||
            (cur->type == XML_HTML_DOCUMENT_NODE)) {
            xmlFreeDoc((xmlDocPtr) cur);
        } else if (cur->type == XML_DTD_NODE) {
            /*
             * TODO: We should consider freeing the DTD if it isn't
             * referenced from doc->intSubset or doc->extSubset.
             */
            cur->prev = NULL;
            cur->next = NULL;
        } else {
	    if ((xmlRegisterCallbacks) && (xmlDeregisterNodeDefaultValue))
		xmlDeregisterNodeDefaultValue(cur);

	    if (((cur->type == XML_ELEMENT_NODE) ||
		 (cur->type == XML_XINCLUDE_START) ||
		 (cur->type == XML_XINCLUDE_END)) &&
		(cur->properties != NULL))
		xmlFreePropList(cur->properties);
	    if ((cur->type != XML_ELEMENT_NODE) &&
		(cur->type != XML_XINCLUDE_START) &&
		(cur->type != XML_XINCLUDE_END) &&
		(cur->type != XML_ENTITY_REF_NODE) &&
		(cur->content != (xmlChar *) &(cur->properties))) {
		DICT_FREE(cur->content)
	    }
	    if (((cur->type == XML_ELEMENT_NODE) ||
	         (cur->type == XML_XINCLUDE_START) ||
		 (cur->type == XML_XINCLUDE_END)) &&
		(cur->nsDef != NULL))
		xmlFreeNsList(cur->nsDef);

	    /*
	     * When a node is a text node or a comment, it uses a global static
	     * variable for the name of the node.
	     * Otherwise the node name might come from the document's
	     * dictionary
	     */
	    if ((cur->name != NULL) &&
		(cur->type != XML_TEXT_NODE) &&
		(cur->type != XML_COMMENT_NODE))
		DICT_FREE(cur->name)
	    xmlFree(cur);
	}

        if (next != NULL) {
	    cur = next;
        } else {
            if ((depth == 0) || (parent == NULL))
                break;
            depth -= 1;
            cur = parent;
            cur->children = NULL;
        }
    }
}

/**
 * Free a node including all the children.
 *
 * This doesn't unlink the node from the tree. Call #xmlUnlinkNode first
 * unless `cur` is a root node.
 *
 * @param cur  the node
 */
void
xmlFreeNode(xmlNode *cur) {
    xmlDictPtr dict = NULL;

    if (cur == NULL) return;

    /* use xmlFreeDtd for DTD nodes */
    if (cur->type == XML_DTD_NODE) {
	xmlFreeDtd((xmlDtdPtr) cur);
	return;
    }
    if (cur->type == XML_NAMESPACE_DECL) {
	xmlFreeNs((xmlNsPtr) cur);
        return;
    }
    if (cur->type == XML_ATTRIBUTE_NODE) {
	xmlFreeProp((xmlAttrPtr) cur);
	return;
    }
    if (cur->type == XML_ENTITY_DECL) {
        xmlFreeEntity((xmlEntityPtr) cur);
        return;
    }

    if ((xmlRegisterCallbacks) && (xmlDeregisterNodeDefaultValue))
	xmlDeregisterNodeDefaultValue(cur);

    if (cur->doc != NULL) dict = cur->doc->dict;

    if ((cur->children != NULL) &&
	(cur->type != XML_ENTITY_REF_NODE))
	xmlFreeNodeList(cur->children);

    if ((cur->type == XML_ELEMENT_NODE) ||
        (cur->type == XML_XINCLUDE_START) ||
        (cur->type == XML_XINCLUDE_END)) {
        if (cur->properties != NULL)
            xmlFreePropList(cur->properties);
        if (cur->nsDef != NULL)
            xmlFreeNsList(cur->nsDef);
    } else if ((cur->content != NULL) &&
               (cur->type != XML_ENTITY_REF_NODE) &&
               (cur->content != (xmlChar *) &(cur->properties))) {
        DICT_FREE(cur->content)
    }

    /*
     * When a node is a text node or a comment, it uses a global static
     * variable for the name of the node.
     * Otherwise the node name might come from the document's dictionary
     */
    if ((cur->name != NULL) &&
        (cur->type != XML_TEXT_NODE) &&
        (cur->type != XML_COMMENT_NODE))
	DICT_FREE(cur->name)

    xmlFree(cur);
}

/**
 * Unlink a node from its tree.
 *
 * This function only unlinks the node from the tree. It doesn't
 * clear references to DTD nodes.
 *
 * @param cur  the node
 */
static void
xmlUnlinkNodeInternal(xmlNodePtr cur) {
    if (cur->parent != NULL) {
	xmlNodePtr parent;
	parent = cur->parent;
	if (cur->type == XML_ATTRIBUTE_NODE) {
	    if (parent->properties == (xmlAttrPtr) cur)
		parent->properties = ((xmlAttrPtr) cur)->next;
	} else {
	    if (parent->children == cur)
		parent->children = cur->next;
	    if (parent->last == cur)
		parent->last = cur->prev;
	}
	cur->parent = NULL;
    }

    if (cur->next != NULL)
        cur->next->prev = cur->prev;
    if (cur->prev != NULL)
        cur->prev->next = cur->next;
    cur->next = NULL;
    cur->prev = NULL;
}

/**
 * Unlink a node from its tree.
 *
 * The node is not freed. Unless it is reinserted, it must be managed
 * manually and freed eventually by calling #xmlFreeNode.
 *
 * @param cur  the node
 */
void
xmlUnlinkNode(xmlNode *cur) {
    if (cur == NULL)
	return;

    if (cur->type == XML_NAMESPACE_DECL)
        return;

    if (cur->type == XML_DTD_NODE) {
	xmlDocPtr doc = cur->doc;

	if (doc != NULL) {
	    if (doc->intSubset == (xmlDtdPtr) cur)
		doc->intSubset = NULL;
	    if (doc->extSubset == (xmlDtdPtr) cur)
		doc->extSubset = NULL;
	}
    }

    if (cur->type == XML_ENTITY_DECL)
        xmlRemoveEntity((xmlEntityPtr) cur);

    xmlUnlinkNodeInternal(cur);
}

/**
 * Unlink the old node. If `cur` is provided, it is unlinked and
 * inserted in place of `old`.
 *
 * It is an error if `old` has no parent.
 *
 * Unlike #xmlAddChild, this function doesn't merge text nodes or
 * delete duplicate attributes.
 *
 * See the notes in #xmlAddChild.
 *
 * @param old  the old node
 * @param cur  the node (optional)
 * @returns `old` or NULL if arguments are invalid or a memory
 * allocation failed.
 */
xmlNode *
xmlReplaceNode(xmlNode *old, xmlNode *cur) {
    if (old == cur) return(NULL);
    if ((old == NULL) || (old->type == XML_NAMESPACE_DECL) ||
        (old->parent == NULL)) {
	return(NULL);
    }
    if ((cur == NULL) || (cur->type == XML_NAMESPACE_DECL)) {
        /* Don't call xmlUnlinkNodeInternal to handle DTDs. */
	xmlUnlinkNode(old);
	return(old);
    }
    if ((old->type==XML_ATTRIBUTE_NODE) && (cur->type!=XML_ATTRIBUTE_NODE)) {
	return(old);
    }
    if ((cur->type==XML_ATTRIBUTE_NODE) && (old->type!=XML_ATTRIBUTE_NODE)) {
	return(old);
    }
    xmlUnlinkNodeInternal(cur);
    if (xmlSetTreeDoc(cur, old->doc) < 0)
        return(NULL);
    cur->parent = old->parent;
    cur->next = old->next;
    if (cur->next != NULL)
	cur->next->prev = cur;
    cur->prev = old->prev;
    if (cur->prev != NULL)
	cur->prev->next = cur;
    if (cur->parent != NULL) {
	if (cur->type == XML_ATTRIBUTE_NODE) {
	    if (cur->parent->properties == (xmlAttrPtr)old)
		cur->parent->properties = ((xmlAttrPtr) cur);
	} else {
	    if (cur->parent->children == old)
		cur->parent->children = cur;
	    if (cur->parent->last == old)
		cur->parent->last = cur;
	}
    }
    old->next = old->prev = NULL;
    old->parent = NULL;
    return(old);
}

/************************************************************************
 *									*
 *		Copy operations						*
 *									*
 ************************************************************************/

/**
 * Copy a namespace.
 *
 * @param cur  the namespace
 * @returns the copied namespace or NULL if a memory allocation
 * failed.
 */
xmlNs *
xmlCopyNamespace(xmlNs *cur) {
    xmlNsPtr ret;

    if (cur == NULL) return(NULL);
    switch (cur->type) {
	case XML_LOCAL_NAMESPACE:
	    ret = xmlNewNs(NULL, cur->href, cur->prefix);
	    break;
	default:
	    return(NULL);
    }
    return(ret);
}

/**
 * Copy a namespace list.
 *
 * @param cur  the first namespace
 * @returns the head of the copied list or NULL if a memory
 * allocation failed.
 */
xmlNs *
xmlCopyNamespaceList(xmlNs *cur) {
    xmlNsPtr ret = NULL;
    xmlNsPtr p = NULL,q;

    while (cur != NULL) {
        q = xmlCopyNamespace(cur);
        if (q == NULL) {
            xmlFreeNsList(ret);
            return(NULL);
        }
	if (p == NULL) {
	    ret = p = q;
	} else {
	    p->next = q;
	    p = q;
	}
	cur = cur->next;
    }
    return(ret);
}

static xmlAttrPtr
xmlCopyPropInternal(xmlDocPtr doc, xmlNodePtr target, xmlAttrPtr cur) {
    xmlAttrPtr ret = NULL;

    if (cur == NULL) return(NULL);
    if ((target != NULL) && (target->type != XML_ELEMENT_NODE))
        return(NULL);
    if (target != NULL)
	ret = xmlNewDocProp(target->doc, cur->name, NULL);
    else if (doc != NULL)
	ret = xmlNewDocProp(doc, cur->name, NULL);
    else if (cur->parent != NULL)
	ret = xmlNewDocProp(cur->parent->doc, cur->name, NULL);
    else if (cur->children != NULL)
	ret = xmlNewDocProp(cur->children->doc, cur->name, NULL);
    else
	ret = xmlNewDocProp(NULL, cur->name, NULL);
    if (ret == NULL) return(NULL);
    ret->parent = target;

    if ((cur->ns != NULL) && (target != NULL)) {
      xmlNsPtr ns;
      int res;

      res = xmlSearchNsSafe(target, cur->ns->prefix, &ns);
      if (res < 0)
          goto error;
      if (ns == NULL) {
        /*
         * Humm, we are copying an element whose namespace is defined
         * out of the new tree scope. Search it in the original tree
         * and add it at the top of the new tree
         */
        res = xmlSearchNsSafe(cur->parent, cur->ns->prefix, &ns);
        if (res < 0)
          goto error;
        if (ns != NULL) {
          xmlNodePtr root = target;
          xmlNodePtr pred = NULL;

          while (root->parent != NULL) {
            pred = root;
            root = root->parent;
          }
          if (root == (xmlNodePtr) target->doc) {
            /* correct possibly cycling above the document elt */
            root = pred;
          }
          ret->ns = xmlNewNs(root, ns->href, ns->prefix);
          if (ret->ns == NULL)
              goto error;
        }
      } else {
        /*
         * we have to find something appropriate here since
         * we can't be sure, that the namespace we found is identified
         * by the prefix
         */
        if (xmlStrEqual(ns->href, cur->ns->href)) {
          /* this is the nice case */
          ret->ns = ns;
        } else {
          /*
           * we are in trouble: we need a new reconciled namespace.
           * This is expensive
           */
          ret->ns = xmlNewReconciledNs(target, cur->ns);
          if (ret->ns == NULL)
              goto error;
        }
      }

    } else
        ret->ns = NULL;

    if (cur->children != NULL) {
	xmlNodePtr tmp;

	ret->children = xmlStaticCopyNodeList(cur->children, ret->doc, (xmlNodePtr) ret);
        if (ret->children == NULL)
            goto error;
	ret->last = NULL;
	tmp = ret->children;
	while (tmp != NULL) {
	    /* tmp->parent = (xmlNodePtr)ret; */
	    if (tmp->next == NULL)
	        ret->last = tmp;
	    tmp = tmp->next;
	}
    }
    /*
     * Try to handle IDs
     */
    if ((target != NULL) && (cur != NULL) &&
	(target->doc != NULL) && (cur->doc != NULL) &&
        (cur->parent != NULL) &&
        (cur->children != NULL)) {
        int res = xmlIsID(cur->doc, cur->parent, cur);

        if (res < 0)
            goto error;
	if (res != 0) {
	    xmlChar *id;

	    id = xmlNodeGetContent((xmlNodePtr) cur);
	    if (id == NULL)
                goto error;
            res = xmlAddIDSafe(ret, id);
	    xmlFree(id);
            if (res < 0)
                goto error;
	}
    }
    return(ret);

error:
    xmlFreeProp(ret);
    return(NULL);
}

/**
 * Create a copy of the attribute. This function sets the parent
 * pointer of the copy to `target` but doesn't set the attribute on
 * the target element. Users should consider to set the attribute
 * by calling #xmlAddChild afterwards or reset the parent pointer to
 * NULL.
 *
 * @param target  the element where the attribute will be grafted
 * @param cur  the attribute
 * @returns the copied attribute or NULL if a memory allocation
 * failed.
 */
xmlAttr *
xmlCopyProp(xmlNode *target, xmlAttr *cur) {
	return xmlCopyPropInternal(NULL, target, cur);
}

/**
 * Create a copy of an attribute list. This function sets the
 * parent pointers of the copied attributes to `target` but doesn't
 * set the attributes on the target element.
 *
 * @param target  the element where the attributes will be grafted
 * @param cur  the first attribute
 * @returns the head of the copied list or NULL if a memory
 * allocation failed.
 */
xmlAttr *
xmlCopyPropList(xmlNode *target, xmlAttr *cur) {
    xmlAttrPtr ret = NULL;
    xmlAttrPtr p = NULL,q;

    if ((target != NULL) && (target->type != XML_ELEMENT_NODE))
        return(NULL);
    while (cur != NULL) {
        q = xmlCopyProp(target, cur);
	if (q == NULL) {
            xmlFreePropList(ret);
	    return(NULL);
        }
	if (p == NULL) {
	    ret = p = q;
	} else {
	    p->next = q;
	    q->prev = p;
	    p = q;
	}
	cur = cur->next;
    }
    return(ret);
}

/*
 * NOTE about the CopyNode operations !
 *
 * They are split into external and internal parts for one
 * tricky reason: namespaces. Doing a direct copy of a node
 * say RPM:Copyright without changing the namespace pointer to
 * something else can produce stale links. One way to do it is
 * to keep a reference counter but this doesn't work as soon
 * as one moves the element or the subtree out of the scope of
 * the existing namespace. The actual solution seems to be to add
 * a copy of the namespace at the top of the copied tree if
 * not available in the subtree.
 * Hence two functions, the public front-end call the inner ones
 * The argument "recursive" normally indicates a recursive copy
 * of the node with values 0 (no) and 1 (yes).  For XInclude,
 * however, we allow a value of 2 to indicate copy properties and
 * namespace info, but don't recurse on children.
 */

/**
 * Copy a node.
 *
 * @param node  source node
 * @param doc  target document
 * @param parent  target parent
 * @param extended  flags
 * @returns the copy or NULL if a memory allocation failed.
 */
xmlNode *
xmlStaticCopyNode(xmlNode *node, xmlDoc *doc, xmlNode *parent,
                  int extended) {
    xmlNodePtr ret;

    if (node == NULL) return(NULL);
    switch (node->type) {
        case XML_TEXT_NODE:
        case XML_CDATA_SECTION_NODE:
        case XML_ELEMENT_NODE:
        case XML_DOCUMENT_FRAG_NODE:
        case XML_ENTITY_REF_NODE:
        case XML_PI_NODE:
        case XML_COMMENT_NODE:
        case XML_XINCLUDE_START:
        case XML_XINCLUDE_END:
	    break;
        case XML_ATTRIBUTE_NODE:
		return((xmlNodePtr) xmlCopyPropInternal(doc, parent, (xmlAttrPtr) node));
        case XML_NAMESPACE_DECL:
	    return((xmlNodePtr) xmlCopyNamespaceList((xmlNsPtr) node));

        case XML_DOCUMENT_NODE:
        case XML_HTML_DOCUMENT_NODE:
	    return((xmlNodePtr) xmlCopyDoc((xmlDocPtr) node, extended));
        default:
            return(NULL);
    }

    /*
     * Allocate a new node and fill the fields.
     */
    ret = (xmlNodePtr) xmlMalloc(sizeof(xmlNode));
    if (ret == NULL)
	return(NULL);
    memset(ret, 0, sizeof(xmlNode));
    ret->type = node->type;

    ret->doc = doc;
    ret->parent = parent;
    if (node->name == xmlStringText)
	ret->name = xmlStringText;
    else if (node->name == xmlStringTextNoenc)
	ret->name = xmlStringTextNoenc;
    else if (node->name == xmlStringComment)
	ret->name = xmlStringComment;
    else if (node->name != NULL) {
        if ((doc != NULL) && (doc->dict != NULL))
	    ret->name = xmlDictLookup(doc->dict, node->name, -1);
	else
	    ret->name = xmlStrdup(node->name);
        if (ret->name == NULL)
            goto error;
    }
    if ((node->type != XML_ELEMENT_NODE) &&
	(node->content != NULL) &&
	(node->type != XML_ENTITY_REF_NODE) &&
	(node->type != XML_XINCLUDE_END) &&
	(node->type != XML_XINCLUDE_START)) {
	ret->content = xmlStrdup(node->content);
        if (ret->content == NULL)
            goto error;
    }else{
      if (node->type == XML_ELEMENT_NODE)
        ret->line = node->line;
    }

    if (!extended)
	goto out;
    if (((node->type == XML_ELEMENT_NODE) ||
         (node->type == XML_XINCLUDE_START)) && (node->nsDef != NULL)) {
        ret->nsDef = xmlCopyNamespaceList(node->nsDef);
        if (ret->nsDef == NULL)
            goto error;
    }

    if ((node->type == XML_ELEMENT_NODE) && (node->ns != NULL)) {
        xmlNsPtr ns = NULL;
        int res;

	res = xmlSearchNsSafe(ret, node->ns->prefix, &ns);
        if (res < 0)
            goto error;
	if (ns == NULL) {
	    /*
	     * Humm, we are copying an element whose namespace is defined
	     * out of the new tree scope. Search it in the original tree
	     * and add it at the top of the new tree.
             *
             * TODO: Searching the original tree seems unnecessary. We
             * already have a namespace URI.
	     */
	    res = xmlSearchNsSafe(node, node->ns->prefix, &ns);
            if (res < 0)
                goto error;
	    if (ns != NULL) {
	        xmlNodePtr root = ret;

		while (root->parent != NULL) root = root->parent;
		ret->ns = xmlNewNs(root, ns->href, ns->prefix);
            } else {
                ret->ns = xmlNewReconciledNs(ret, node->ns);
	    }
            if (ret->ns == NULL)
                goto error;
	} else {
	    /*
	     * reference the existing namespace definition in our own tree.
	     */
	    ret->ns = ns;
	}
    }
    if ((node->type == XML_ELEMENT_NODE) && (node->properties != NULL)) {
        ret->properties = xmlCopyPropList(ret, node->properties);
        if (ret->properties == NULL)
            goto error;
    }
    if (node->type == XML_ENTITY_REF_NODE) {
	if ((doc == NULL) || (node->doc != doc)) {
	    /*
	     * The copied node will go into a separate document, so
	     * to avoid dangling references to the ENTITY_DECL node
	     * we cannot keep the reference. Try to find it in the
	     * target document.
	     */
	    ret->children = (xmlNodePtr) xmlGetDocEntity(doc, ret->name);
	} else {
            ret->children = node->children;
	}
	ret->last = ret->children;
    } else if ((node->children != NULL) && (extended != 2)) {
        xmlNodePtr cur, insert;

        cur = node->children;
        insert = ret;
        while (cur != NULL) {
            xmlNodePtr copy = xmlStaticCopyNode(cur, doc, insert, 2);
            if (copy == NULL)
                goto error;

            /* Check for coalesced text nodes */
            if (insert->last != copy) {
                if (insert->last == NULL) {
                    insert->children = copy;
                } else {
                    copy->prev = insert->last;
                    insert->last->next = copy;
                }
                insert->last = copy;
            }

            if ((cur->type != XML_ENTITY_REF_NODE) &&
                (cur->children != NULL)) {
                cur = cur->children;
                insert = copy;
                continue;
            }

            while (1) {
                if (cur->next != NULL) {
                    cur = cur->next;
                    break;
                }

                cur = cur->parent;
                insert = insert->parent;
                if (cur == node) {
                    cur = NULL;
                    break;
                }
            }
        }
    }

out:
    if ((xmlRegisterCallbacks) && (xmlRegisterNodeDefaultValue))
	xmlRegisterNodeDefaultValue((xmlNodePtr)ret);
    return(ret);

error:
    xmlFreeNode(ret);
    return(NULL);
}

/**
 * Copy a node list. If `parent` is provided, sets the parent pointer
 * of the copied nodes, but doesn't update the children and last
 * pointer of `parent`.
 *
 * @param node  node to copy
 * @param doc  target document
 * @param parent  target node (optional)
 * @returns a the copy or NULL in case of error.
 */
xmlNode *
xmlStaticCopyNodeList(xmlNode *node, xmlDoc *doc, xmlNode *parent) {
    xmlNodePtr ret = NULL;
    xmlNodePtr p = NULL,q;
    xmlDtdPtr newSubset = NULL;
    int linkedSubset = 0;

    while (node != NULL) {
        xmlNodePtr next = node->next;

	if (node->type == XML_DTD_NODE ) {
	    if (doc == NULL) {
		node = next;
		continue;
	    }
	    if ((doc->intSubset == NULL) && (newSubset == NULL)) {
		q = (xmlNodePtr) xmlCopyDtd( (xmlDtdPtr) node );
		if (q == NULL) goto error;
                /* Can't fail on DTD */
		xmlSetTreeDoc(q, doc);
		q->parent = parent;
		newSubset = (xmlDtdPtr) q;
	    } else {
                /*
                 * We don't allow multiple internal subsets in a document,
                 * so we move the DTD instead of creating a copy.
                 */
                linkedSubset = 1;
		q = (xmlNodePtr) doc->intSubset;
                /* Unlink */
                if (q->prev == NULL) {
                    if (q->parent != NULL)
                        q->parent->children = q->next;
                } else {
                    q->prev->next = q->next;
                }
                if (q->next == NULL) {
                    if (q->parent != NULL)
                        q->parent->last = q->prev;
                } else {
                    q->next->prev = q->prev;
                }
                q->parent = parent;
                q->next = NULL;
                q->prev = NULL;
	    }
	} else
	    q = xmlStaticCopyNode(node, doc, parent, 1);
	if (q == NULL) goto error;
	if (ret == NULL) {
	    q->prev = NULL;
	    ret = p = q;
	} else if (p != q) {
	/* the test is required if xmlStaticCopyNode coalesced 2 text nodes */
	    p->next = q;
	    q->prev = p;
	    p = q;
	}
	node = next;
    }
    if ((doc != NULL) && (newSubset != NULL))
        doc->intSubset = newSubset;
    return(ret);
error:
    xmlFreeNodeList(ret);
    if (newSubset != NULL)
        xmlFreeDtd(newSubset);
    if (linkedSubset != 0) {
        doc->intSubset->next = NULL;
        doc->intSubset->prev = NULL;
    }
    return(NULL);
}

/**
 * Copy a node.
 *
 * If `extended` is 0, make a shallow copy.
 *
 * If `extended` is 1, make a deep copy (properties, namespaces
 * and children when applicable).
 *
 * If `extended` is 2, make a shallow copy including properties and
 * namespaces of elements.
 *
 * Use of this function is DISCOURAGED in favor of #xmlDocCopyNode.
 *
 * @param node  the node
 * @param extended  mode of operation
 * @returns the copied node or NULL if a memory allocation failed.
 */
xmlNode *
xmlCopyNode(xmlNode *node, int extended) {
    xmlNodePtr ret;

    ret = xmlStaticCopyNode(node, NULL, NULL, extended);
    return(ret);
}

/**
 * Copy a node into another document.
 *
 * If `extended` is 0, make a shallow copy.
 *
 * If `extended` is 1, make a deep copy (properties, namespaces
 * and children when applicable).
 *
 * If `extended` is 2, make a shallow copy including properties and
 * namespaces of elements.
 *
 * @param node  the node
 * @param doc  the document
 * @param extended  mode of operation
 * @returns the copied node or NULL if a memory allocation failed.
 */
xmlNode *
xmlDocCopyNode(xmlNode *node, xmlDoc *doc, int extended) {
    xmlNodePtr ret;

    ret = xmlStaticCopyNode(node, doc, NULL, extended);
    return(ret);
}

/**
 * Copy a node list and all children into a new document.
 *
 * @param doc  the target document
 * @param node  the first node in the list.
 * @returns the head of the copied list or NULL if a memory
 * allocation failed.
 */
xmlNode *xmlDocCopyNodeList(xmlDoc *doc, xmlNode *node) {
    xmlNodePtr ret = xmlStaticCopyNodeList(node, doc, NULL);
    return(ret);
}

/**
 * Copy a node list and all children.
 *
 * Use of this function is DISCOURAGED in favor of #xmlDocCopyNodeList.
 *
 * @param node  the first node in the list.
 * @returns the head of the copied list or NULL if a memory
 * allocation failed.
 */
xmlNode *xmlCopyNodeList(xmlNode *node) {
    xmlNodePtr ret = xmlStaticCopyNodeList(node, NULL, NULL);
    return(ret);
}

/**
 * Copy a DTD.
 *
 * @param dtd  the DTD
 * @returns the copied DTD or NULL if a memory allocation failed.
 */
xmlDtd *
xmlCopyDtd(xmlDtd *dtd) {
    xmlDtdPtr ret;
    xmlNodePtr cur, p = NULL, q;

    if (dtd == NULL) return(NULL);
    ret = xmlNewDtd(NULL, dtd->name, dtd->ExternalID, dtd->SystemID);
    if (ret == NULL) return(NULL);
    if (dtd->entities != NULL) {
        ret->entities = (void *) xmlCopyEntitiesTable(
	                    (xmlEntitiesTablePtr) dtd->entities);
        if (ret->entities == NULL)
            goto error;
    }
    if (dtd->notations != NULL) {
        ret->notations = (void *) xmlCopyNotationTable(
	                    (xmlNotationTablePtr) dtd->notations);
        if (ret->notations == NULL)
            goto error;
    }
    if (dtd->elements != NULL) {
        ret->elements = (void *) xmlCopyElementTable(
	                    (xmlElementTablePtr) dtd->elements);
        if (ret->elements == NULL)
            goto error;
    }
    if (dtd->attributes != NULL) {
        ret->attributes = (void *) xmlCopyAttributeTable(
	                    (xmlAttributeTablePtr) dtd->attributes);
        if (ret->attributes == NULL)
            goto error;
    }
    if (dtd->pentities != NULL) {
	ret->pentities = (void *) xmlCopyEntitiesTable(
			    (xmlEntitiesTablePtr) dtd->pentities);
        if (ret->pentities == NULL)
            goto error;
    }

    cur = dtd->children;
    while (cur != NULL) {
	q = NULL;

	if (cur->type == XML_ENTITY_DECL) {
	    xmlEntityPtr tmp = (xmlEntityPtr) cur;
	    switch (tmp->etype) {
		case XML_INTERNAL_GENERAL_ENTITY:
		case XML_EXTERNAL_GENERAL_PARSED_ENTITY:
		case XML_EXTERNAL_GENERAL_UNPARSED_ENTITY:
		    q = (xmlNodePtr) xmlGetEntityFromDtd(ret, tmp->name);
		    break;
		case XML_INTERNAL_PARAMETER_ENTITY:
		case XML_EXTERNAL_PARAMETER_ENTITY:
		    q = (xmlNodePtr)
			xmlGetParameterEntityFromDtd(ret, tmp->name);
		    break;
		case XML_INTERNAL_PREDEFINED_ENTITY:
		    break;
	    }
	} else if (cur->type == XML_ELEMENT_DECL) {
	    xmlElementPtr tmp = (xmlElementPtr) cur;
	    q = (xmlNodePtr)
		xmlGetDtdQElementDesc(ret, tmp->name, tmp->prefix);
	} else if (cur->type == XML_ATTRIBUTE_DECL) {
	    xmlAttributePtr tmp = (xmlAttributePtr) cur;
	    q = (xmlNodePtr)
		xmlGetDtdQAttrDesc(ret, tmp->elem, tmp->name, tmp->prefix);
	} else if (cur->type == XML_COMMENT_NODE) {
	    q = xmlCopyNode(cur, 0);
            if (q == NULL)
                goto error;
	}

	if (q == NULL) {
	    cur = cur->next;
	    continue;
	}

	if (p == NULL)
	    ret->children = q;
	else
	    p->next = q;

	q->prev = p;
	q->parent = (xmlNodePtr) ret;
	q->next = NULL;
	ret->last = q;
	p = q;
	cur = cur->next;
    }

    return(ret);

error:
    xmlFreeDtd(ret);
    return(NULL);
}

/**
 * Copy a document. If recursive, the content tree will
 * be copied too as well as DTD, namespaces and entities.
 *
 * @param doc  the document
 * @param recursive  if not zero do a recursive copy.
 * @returns the copied document or NULL if a memory allocation
 * failed.
 */
xmlDoc *
xmlCopyDoc(xmlDoc *doc, int recursive) {
    xmlDocPtr ret;

    if (doc == NULL) return(NULL);
    ret = xmlNewDoc(doc->version);
    if (ret == NULL) return(NULL);
    ret->type = doc->type;
    if (doc->name != NULL) {
        ret->name = xmlMemStrdup(doc->name);
        if (ret->name == NULL)
            goto error;
    }
    if (doc->encoding != NULL) {
        ret->encoding = xmlStrdup(doc->encoding);
        if (ret->encoding == NULL)
            goto error;
    }
    if (doc->URL != NULL) {
        ret->URL = xmlStrdup(doc->URL);
        if (ret->URL == NULL)
            goto error;
    }
    ret->charset = doc->charset;
    ret->compression = doc->compression;
    ret->standalone = doc->standalone;
    if (!recursive) return(ret);

    ret->last = NULL;
    ret->children = NULL;
    if (doc->intSubset != NULL) {
        ret->intSubset = xmlCopyDtd(doc->intSubset);
	if (ret->intSubset == NULL)
            goto error;
        /* Can't fail on DTD */
	xmlSetTreeDoc((xmlNodePtr)ret->intSubset, ret);
    }
    if (doc->oldNs != NULL) {
        ret->oldNs = xmlCopyNamespaceList(doc->oldNs);
        if (ret->oldNs == NULL)
            goto error;
    }
    if (doc->children != NULL) {
	xmlNodePtr tmp;

	ret->children = xmlStaticCopyNodeList(doc->children, ret,
		                               (xmlNodePtr)ret);
        if (ret->children == NULL)
            goto error;
	ret->last = NULL;
	tmp = ret->children;
	while (tmp != NULL) {
	    if (tmp->next == NULL)
	        ret->last = tmp;
	    tmp = tmp->next;
	}
    }
    return(ret);

error:
    xmlFreeDoc(ret);
    return(NULL);
}

/************************************************************************
 *									*
 *		Content access functions				*
 *									*
 ************************************************************************/

/**
 * Get line number of `node`. Try to work around the limitation of
 * line numbers being stored as 16 bits ints. Requires xmlParserOption
 * XML_PARSE_BIG_LINES to be set when parsing.
 *
 * @param node  valid node
 * @param depth  used to limit any risk of recursion
 * @returns the line number if successful, -1 otherwise
 */
static long
xmlGetLineNoInternal(const xmlNode *node, int depth)
{
    long result = -1;

    if (depth >= 5)
        return(-1);

    if (!node)
        return result;
    if ((node->type == XML_ELEMENT_NODE) ||
        (node->type == XML_TEXT_NODE) ||
	(node->type == XML_COMMENT_NODE) ||
	(node->type == XML_PI_NODE)) {
	if (node->line == 65535) {
	    if ((node->type == XML_TEXT_NODE) && (node->psvi != NULL))
	        result = XML_PTR_TO_INT(node->psvi);
	    else if ((node->type == XML_ELEMENT_NODE) &&
	             (node->children != NULL))
	        result = xmlGetLineNoInternal(node->children, depth + 1);
	    else if (node->next != NULL)
	        result = xmlGetLineNoInternal(node->next, depth + 1);
	    else if (node->prev != NULL)
	        result = xmlGetLineNoInternal(node->prev, depth + 1);
	}
	if ((result == -1) || (result == 65535))
	    result = node->line;
    } else if ((node->prev != NULL) &&
             ((node->prev->type == XML_ELEMENT_NODE) ||
	      (node->prev->type == XML_TEXT_NODE) ||
	      (node->prev->type == XML_COMMENT_NODE) ||
	      (node->prev->type == XML_PI_NODE)))
        result = xmlGetLineNoInternal(node->prev, depth + 1);
    else if ((node->parent != NULL) &&
             (node->parent->type == XML_ELEMENT_NODE))
        result = xmlGetLineNoInternal(node->parent, depth + 1);

    return result;
}

/**
 * Get line number of `node`. Try to work around the limitation of
 * line numbers being stored as 16 bits ints. Requires xmlParserOption
 * XML_PARSE_BIG_LINES to be set when parsing.
 *
 * @param node  valid node
 * @returns the line number if successful, -1 otherwise
 */
long
xmlGetLineNo(const xmlNode *node)
{
    return(xmlGetLineNoInternal(node, 0));
}

/**
 * Build a structure based Path for the given node
 *
 * @param node  a node
 * @returns the new path or NULL in case of error. The caller must free
 *     the returned string
 */
xmlChar *
xmlGetNodePath(const xmlNode *node)
{
    const xmlNode *cur, *tmp;
    const xmlNode **nodes = NULL;
    xmlChar *ret = NULL;
    xmlBuf *buf;
    size_t numNodes, i;

    if ((node == NULL) || (node->type == XML_NAMESPACE_DECL))
        return(NULL);

    buf = xmlBufCreate(50);
    if (buf == NULL)
        return(NULL);

    /*
     * Get list of ancestors
     */
    numNodes = 0;
    for (cur = node; cur != NULL; cur = cur->parent)
        numNodes += 1;
    if (numNodes > SIZE_MAX / sizeof(nodes[0]))
        goto error;
    nodes = xmlMalloc(numNodes * sizeof(nodes[0]));
    if (nodes == NULL)
        goto error;
    i = 0;
    for (cur = node; cur != NULL && i < numNodes; cur = cur->parent)
        nodes[i++] = cur;

    /*
     * Iterate in reverse to start at root
     */
    while (i > 0) {
        int occur = 0;

        i -= 1;
        cur = nodes[i];

        if ((cur->type == XML_DOCUMENT_NODE) ||
            (cur->type == XML_HTML_DOCUMENT_NODE)) {
            if (i == 0)
                xmlBufCat(buf, BAD_CAST "/");
        } else if (cur->type == XML_ELEMENT_NODE) {
            int generic = 0;

            xmlBufCat(buf, BAD_CAST "/");

            if (cur->ns) {
		if (cur->ns->prefix != NULL) {
                    xmlBufCat(buf, cur->ns->prefix);
                    xmlBufCat(buf, BAD_CAST ":");
                    xmlBufCat(buf, cur->name);
		} else {
		    /*
		    * We cannot express named elements in the default
		    * namespace, so use "*".
		    */
		    generic = 1;
                    xmlBufCat(buf, BAD_CAST "*");
		}
            } else {
                xmlBufCat(buf, cur->name);
            }

            /*
             * Thumbler index computation
	     * TODO: the occurrence test seems bogus for namespaced names
             */
            tmp = cur->prev;
            while (tmp != NULL) {
                if ((tmp->type == XML_ELEMENT_NODE) &&
		    (generic ||
		     (xmlStrEqual(cur->name, tmp->name) &&
		     ((tmp->ns == cur->ns) ||
		      ((tmp->ns != NULL) && (cur->ns != NULL) &&
		       (xmlStrEqual(cur->ns->prefix, tmp->ns->prefix)))))))
                    occur++;
                tmp = tmp->prev;
            }
            if (occur == 0) {
                tmp = cur->next;
                while (tmp != NULL && occur == 0) {
                    if ((tmp->type == XML_ELEMENT_NODE) &&
			(generic ||
			 (xmlStrEqual(cur->name, tmp->name) &&
			 ((tmp->ns == cur->ns) ||
			  ((tmp->ns != NULL) && (cur->ns != NULL) &&
			   (xmlStrEqual(cur->ns->prefix, tmp->ns->prefix)))))))
                        occur++;
                    tmp = tmp->next;
                }
                if (occur != 0)
                    occur = 1;
            } else
                occur++;
        } else if (cur->type == XML_COMMENT_NODE) {
            xmlBufCat(buf, BAD_CAST "/comment()");

            /*
             * Thumbler index computation
             */
            tmp = cur->prev;
            while (tmp != NULL) {
                if (tmp->type == XML_COMMENT_NODE)
		    occur++;
                tmp = tmp->prev;
            }
            if (occur == 0) {
                tmp = cur->next;
                while (tmp != NULL && occur == 0) {
		    if (tmp->type == XML_COMMENT_NODE)
		        occur++;
                    tmp = tmp->next;
                }
                if (occur != 0)
                    occur = 1;
            } else
                occur++;
        } else if ((cur->type == XML_TEXT_NODE) ||
                   (cur->type == XML_CDATA_SECTION_NODE)) {
            xmlBufCat(buf, BAD_CAST "/text()");

            /*
             * Thumbler index computation
             */
            tmp = cur->prev;
            while (tmp != NULL) {
                if ((tmp->type == XML_TEXT_NODE) ||
		    (tmp->type == XML_CDATA_SECTION_NODE))
		    occur++;
                tmp = tmp->prev;
            }
	    /*
	    * Evaluate if this is the only text- or CDATA-section-node;
	    * if yes, then we'll get "text()", otherwise "text()[1]".
	    */
            if (occur == 0) {
                tmp = cur->next;
                while (tmp != NULL) {
		    if ((tmp->type == XML_TEXT_NODE) ||
			(tmp->type == XML_CDATA_SECTION_NODE))
		    {
			occur = 1;
			break;
		    }
		    tmp = tmp->next;
		}
            } else
                occur++;
        } else if (cur->type == XML_PI_NODE) {
            xmlBufCat(buf, BAD_CAST "/processing-instruction('");
            xmlBufCat(buf, cur->name);
            xmlBufCat(buf, BAD_CAST "')");

            /*
             * Thumbler index computation
             */
            tmp = cur->prev;
            while (tmp != NULL) {
                if ((tmp->type == XML_PI_NODE) &&
		    (xmlStrEqual(cur->name, tmp->name)))
                    occur++;
                tmp = tmp->prev;
            }
            if (occur == 0) {
                tmp = cur->next;
                while (tmp != NULL && occur == 0) {
                    if ((tmp->type == XML_PI_NODE) &&
			(xmlStrEqual(cur->name, tmp->name)))
                        occur++;
                    tmp = tmp->next;
                }
                if (occur != 0)
                    occur = 1;
            } else
                occur++;

        } else if (cur->type == XML_ATTRIBUTE_NODE) {
            xmlBufCat(buf, BAD_CAST "/@");
            if (cur->ns && cur->ns->prefix != NULL) {
                xmlBufCat(buf, cur->ns->prefix);
                xmlBufCat(buf, BAD_CAST ":");
            }
            xmlBufCat(buf, cur->name);
        } else {
            goto error;
        }

        if (occur > 0) {
            char tmpbuf[30];

            snprintf(tmpbuf, sizeof(tmpbuf), "[%d]", occur);
            xmlBufCat(buf, BAD_CAST tmpbuf);
        }
    }

    ret = xmlBufDetach(buf);

error:
    xmlBufFree(buf);
    xmlFree(nodes);
    return(ret);
}

/**
 * Get the root element of the document.
 *
 * Searches the document's children for the root element. The first
 * child is not necessarily the root element, but could also be a
 * DTD, comment or PI.
 *
 * @param doc  the document
 * @returns the root element or NULL if no element was found.
 */
xmlNode *
xmlDocGetRootElement(const xmlDoc *doc) {
    xmlNodePtr ret;

    if (doc == NULL) return(NULL);
    ret = doc->children;
    while (ret != NULL) {
	if (ret->type == XML_ELEMENT_NODE)
	    return(ret);
        ret = ret->next;
    }
    return(ret);
}

/**
 * If `root` is NULL no action is taken. To remove a node from a
 * document, use #xmlUnlinkNode instead.
 *
 * Set the root element of the document (`doc->children` is a list
 * containing possibly comments, PIs, etc ...).
 *
 * `root` must be an element node. It is unlinked before insertion.
 *
 * @param doc  the document
 * @param root  the new document root element
 * @returns the unlinked old root element or NULL if the document
 * didn't have a root element or a memory allocation failed.
 */
xmlNode *
xmlDocSetRootElement(xmlDoc *doc, xmlNode *root) {
    xmlNodePtr old = NULL;

    if (doc == NULL) return(NULL);
    if ((root == NULL) || (root->type == XML_NAMESPACE_DECL))
	return(NULL);
    old = doc->children;
    while (old != NULL) {
	if (old->type == XML_ELEMENT_NODE)
	    break;
        old = old->next;
    }
    if (old == root)
        return(old);
    xmlUnlinkNodeInternal(root);
    if (xmlSetTreeDoc(root, doc) < 0)
        return(NULL);
    root->parent = (xmlNodePtr) doc;
    if (old == NULL) {
	if (doc->children == NULL) {
	    doc->children = root;
	    doc->last = root;
	} else {
	    xmlAddSibling(doc->children, root);
	}
    } else {
	xmlReplaceNode(old, root);
    }
    return(old);
}

/**
 * Set the `xml:lang` attribute of a node.
 *
 * @param cur  the node being changed
 * @param lang  the language description
 * @returns 0 on success, 1 if arguments are invalid, -1 if a
 * memory allocation failed.
 */
int
xmlNodeSetLang(xmlNode *cur, const xmlChar *lang) {
    xmlNsPtr ns;
    xmlAttrPtr attr;
    int res;

    if ((cur == NULL) || (cur->type != XML_ELEMENT_NODE))
        return(1);

    res = xmlSearchNsByHrefSafe(cur, XML_XML_NAMESPACE, &ns);
    if (res != 0)
        return(res);
    attr = xmlSetNsProp(cur, ns, BAD_CAST "lang", lang);
    if (attr == NULL)
        return(-1);

    return(0);
}

/**
 * Find the `xml:lang` of a node.
 *
 * Look up the value of the `xml:lang` attribute or the one carried
 * by the nearest ancestor.
 *
 * @param cur  the node being checked
 * @returns a pointer to the lang value, or NULL if not found
 *     It's up to the caller to free the memory with #xmlFree.
 */
xmlChar *
xmlNodeGetLang(const xmlNode *cur) {
    xmlChar *lang;
    int res;

    if ((cur == NULL) || (cur->type == XML_NAMESPACE_DECL))
        return(NULL);

    while (cur != NULL) {
        res = xmlNodeGetAttrValue(cur, BAD_CAST "lang", XML_XML_NAMESPACE,
                                  &lang);
        if (res < 0)
            return(NULL);
	if (lang != NULL)
	    return(lang);

	cur = cur->parent;
    }

    return(NULL);
}


/**
 * Set the `xml:space` attribute of a node.
 *
 * @param cur  the node being changed
 * @param val  the xml:space value ("0": default, 1: "preserve")
 * @returns 0 on success, 1 if arguments are invalid, -1 if a
 * memory allocation failed.
 */
int
xmlNodeSetSpacePreserve(xmlNode *cur, int val) {
    xmlNsPtr ns;
    xmlAttrPtr attr;
    const char *string;
    int res;

    if ((cur == NULL) || (cur->type != XML_ELEMENT_NODE))
        return(1);

    res = xmlSearchNsByHrefSafe(cur, XML_XML_NAMESPACE, &ns);
    if (res != 0)
	return(res);

    if (val == 0)
        string = "default";
    else
        string = "preserve";

    attr = xmlSetNsProp(cur, ns, BAD_CAST "space", BAD_CAST string);
    if (attr == NULL)
        return(-1);

    return(0);
}

/**
 * Find the `xml:space` of a node.
 *
 * Look up the value of the `xml:space` attribute or the one carried
 * by the nearest ancestor.
 *
 * @param cur  the node being checked
 * @returns -1 if xml:space is not inherited, 0 if "default", 1 if "preserve"
 */
int
xmlNodeGetSpacePreserve(const xmlNode *cur) {
    xmlChar *space;
        int res;

    if ((cur == NULL) || (cur->type != XML_ELEMENT_NODE))
        return(-1);

    while (cur != NULL) {
	res = xmlNodeGetAttrValue(cur, BAD_CAST "space", XML_XML_NAMESPACE,
                                  &space);
        if (res < 0)
            return(-1);
	if (space != NULL) {
	    if (xmlStrEqual(space, BAD_CAST "preserve")) {
		xmlFree(space);
		return(1);
	    }
	    if (xmlStrEqual(space, BAD_CAST "default")) {
		xmlFree(space);
		return(0);
	    }
	    xmlFree(space);
	}

	cur = cur->parent;
    }

    return(-1);
}

/**
 * Set (or reset) the name of a node.
 *
 * @param cur  the node being changed
 * @param name  the new tag name
 */
void
xmlNodeSetName(xmlNode *cur, const xmlChar *name) {
    xmlDocPtr doc;
    xmlDictPtr dict;
    const xmlChar *copy;
    const xmlChar *oldName;

    if (cur == NULL) return;
    if (name == NULL) return;
    switch(cur->type) {
        case XML_ELEMENT_NODE:
        case XML_ATTRIBUTE_NODE:
        case XML_PI_NODE:
        case XML_ENTITY_REF_NODE:
	    break;
        default:
            return;
    }

    doc = cur->doc;
    if (doc != NULL)
	dict = doc->dict;
    else
        dict = NULL;

    if (dict != NULL)
        copy = xmlDictLookup(dict, name, -1);
    else
        copy = xmlStrdup(name);
    if (copy == NULL)
        return;

    oldName = cur->name;
    cur->name = copy;
    if ((oldName != NULL) &&
        ((dict == NULL) || (!xmlDictOwns(dict, oldName))))
        xmlFree((xmlChar *) oldName);
}

/**
 * Set (or reset) the base URI of a node, i.e. the value of the
 * `xml:base` attribute.
 *
 * @param cur  the node being changed
 * @param uri  the new base URI
 * @returns 0 on success, -1 on error.
 */
int
xmlNodeSetBase(xmlNode *cur, const xmlChar* uri) {
    xmlNsPtr ns;
    xmlChar* fixed;

    if (cur == NULL)
        return(-1);
    switch(cur->type) {
        case XML_ELEMENT_NODE:
        case XML_ATTRIBUTE_NODE:
	    break;
        case XML_DOCUMENT_NODE:
        case XML_HTML_DOCUMENT_NODE: {
	    xmlDocPtr doc = (xmlDocPtr) cur;

	    if (doc->URL != NULL)
		xmlFree(doc->URL);
	    if (uri == NULL) {
		doc->URL = NULL;
            } else {
		doc->URL = xmlPathToURI(uri);
                if (doc->URL == NULL)
                    return(-1);
            }
	    return(0);
	}
        default:
	    return(-1);
    }

    xmlSearchNsByHrefSafe(cur, XML_XML_NAMESPACE, &ns);
    if (ns == NULL)
	return(-1);
    fixed = xmlPathToURI(uri);
    if (fixed == NULL)
        return(-1);
    if (xmlSetNsProp(cur, ns, BAD_CAST "base", fixed) == NULL) {
        xmlFree(fixed);
        return(-1);
    }
    xmlFree(fixed);

    return(0);
}

/**
 * Searches for the base URI. The code should work on both XML
 * and HTML document even if base mechanisms are completely different.
 * It returns the base as defined in RFC 2396 sections "5.1.1. Base
 * URI within Document Content" and "5.1.2. Base URI from the
 * Encapsulating Entity". However it does not return the document base
 * (5.1.3), use `doc->URL` in this case.
 *
 * @since 2.13.0
 *
 * @param doc  the document the node pertains to
 * @param cur  the node being checked
 * @param baseOut  pointer to base
 * @returns 0 in case of success, 1 if a URI or argument is invalid, -1 if a
 * memory allocation failed.
 */
int
xmlNodeGetBaseSafe(const xmlDoc *doc, const xmlNode *cur, xmlChar **baseOut) {
    xmlChar *ret = NULL;
    xmlChar *base, *newbase;
    int res;

    if (baseOut == NULL)
        return(1);
    *baseOut = NULL;
    if ((cur == NULL) && (doc == NULL))
        return(1);
    if ((cur != NULL) && (cur->type == XML_NAMESPACE_DECL))
        return(1);
    if (doc == NULL)
        doc = cur->doc;

    if ((doc != NULL) && (doc->type == XML_HTML_DOCUMENT_NODE)) {
        cur = doc->children;
	while ((cur != NULL) && (cur->name != NULL)) {
	    if (cur->type != XML_ELEMENT_NODE) {
	        cur = cur->next;
		continue;
	    }
	    if (!xmlStrcasecmp(cur->name, BAD_CAST "html")) {
	        cur = cur->children;
		continue;
	    }
	    if (!xmlStrcasecmp(cur->name, BAD_CAST "head")) {
	        cur = cur->children;
		continue;
	    }
	    if (!xmlStrcasecmp(cur->name, BAD_CAST "base")) {
                if (xmlNodeGetAttrValue(cur, BAD_CAST "href", NULL, &ret) < 0)
                    return(-1);
                if (ret == NULL)
                    return(1);
                goto found;
	    }
	    cur = cur->next;
	}
	return(0);
    }

    while (cur != NULL) {
	if (cur->type == XML_ENTITY_DECL) {
	    xmlEntityPtr ent = (xmlEntityPtr) cur;

            if (ent->URI == NULL)
                break;
            xmlFree(ret);
	    ret = xmlStrdup(ent->URI);
            if (ret == NULL)
                return(-1);
            goto found;
	}
	if (cur->type == XML_ELEMENT_NODE) {
	    if (xmlNodeGetAttrValue(cur, BAD_CAST "base", XML_XML_NAMESPACE,
                                    &base) < 0) {
                xmlFree(ret);
                return(-1);
            }
	    if (base != NULL) {
		if (ret != NULL) {
		    res = xmlBuildURISafe(ret, base, &newbase);
                    xmlFree(ret);
                    xmlFree(base);
                    if (res != 0)
                        return(res);
		    ret = newbase;
		} else {
		    ret = base;
		}
		if ((!xmlStrncmp(ret, BAD_CAST "http://", 7)) ||
		    (!xmlStrncmp(ret, BAD_CAST "ftp://", 6)) ||
		    (!xmlStrncmp(ret, BAD_CAST "urn:", 4)))
                    goto found;
	    }
	}
	cur = cur->parent;
    }

    if ((doc != NULL) && (doc->URL != NULL)) {
	if (ret == NULL) {
	    ret = xmlStrdup(doc->URL);
            if (ret == NULL)
                return(-1);
        } else {
            res = xmlBuildURISafe(ret, doc->URL, &newbase);
            xmlFree(ret);
            if (res != 0)
                return(res);
            ret = newbase;
        }
    }

found:
    *baseOut = ret;
    return(0);
}

/**
 * See #xmlNodeGetBaseSafe. This function doesn't allow to distinguish
 * memory allocation failures from a non-existing base.
 *
 * @param doc  the document the node pertains to
 * @param cur  the node being checked
 * @returns a pointer to the base URL, or NULL if not found
 *     It's up to the caller to free the memory with #xmlFree.
 */
xmlChar *
xmlNodeGetBase(const xmlDoc *doc, const xmlNode *cur) {
    xmlChar *base;

    xmlNodeGetBaseSafe(doc, cur, &base);
    return(base);
}

/**
 * Append the string value of a node to `buffer`. For text nodes,
 * the string value is the text content. Otherwise, the string value
 * is the concatenation of the string values of the node's
 * descendants.
 *
 * Entity references are substituted.
 *
 * @param buffer  a buffer
 * @param cur  the node being read
 * @returns 0 in case of success and -1 in case of error.
 */
int
xmlNodeBufGetContent(xmlBuffer *buffer, const xmlNode *cur)
{
    xmlBufPtr buf;
    int ret1, ret2;

    if ((cur == NULL) || (buffer == NULL)) return(-1);
    buf = xmlBufFromBuffer(buffer);
    ret1 = xmlBufGetNodeContent(buf, cur);
    ret2 = xmlBufBackToBuffer(buf, buffer);
    if ((ret1 < 0) || (ret2 < 0))
        return(-1);
    return(0);
}

static void
xmlBufGetEntityRefContent(xmlBufPtr buf, const xmlNode *ref) {
    xmlEntityPtr ent;

    if (ref->children != NULL) {
        ent = (xmlEntityPtr) ref->children;
    } else {
        /* lookup entity declaration */
        ent = xmlGetDocEntity(ref->doc, ref->name);
        if (ent == NULL)
            return;
    }

    /*
     * The parser should always expand predefined entities but it's
     * possible to create references to predefined entities using
     * the tree API.
     */
    if (ent->etype == XML_INTERNAL_PREDEFINED_ENTITY) {
        xmlBufCat(buf, ent->content);
        return;
    }

    if (ent->flags & XML_ENT_EXPANDING)
        return;

    ent->flags |= XML_ENT_EXPANDING;
    xmlBufGetChildContent(buf, (xmlNodePtr) ent);
    ent->flags &= ~XML_ENT_EXPANDING;
}

static void
xmlBufGetChildContent(xmlBufPtr buf, const xmlNode *tree) {
    const xmlNode *cur = tree->children;

    while (cur != NULL) {
        switch (cur->type) {
            case XML_TEXT_NODE:
            case XML_CDATA_SECTION_NODE:
                xmlBufCat(buf, cur->content);
                break;

            case XML_ENTITY_REF_NODE:
                xmlBufGetEntityRefContent(buf, cur);
                break;

            default:
                if (cur->children != NULL) {
                    cur = cur->children;
                    continue;
                }
                break;
        }

        while (cur->next == NULL) {
            cur = cur->parent;
            if (cur == tree)
                return;
        }
        cur = cur->next;
    }
}

/**
 * Append the string value of a node to `buf`. For text nodes,
 * the string value is the text content. Otherwise, the string value
 * is the concatenation of the string values of the node's
 * descendants.
 *
 * Entity references are substituted.
 *
 * @param buf  a buffer xmlBuf
 * @param cur  the node being read
 * @returns 0 in case of success and -1 in case of error.
 */
int
xmlBufGetNodeContent(xmlBuf *buf, const xmlNode *cur)
{
    if ((cur == NULL) || (buf == NULL))
        return(-1);

    switch (cur->type) {
        case XML_DOCUMENT_NODE:
        case XML_HTML_DOCUMENT_NODE:
        case XML_DOCUMENT_FRAG_NODE:
        case XML_ELEMENT_NODE:
        case XML_ATTRIBUTE_NODE:
        case XML_ENTITY_DECL:
            xmlBufGetChildContent(buf, cur);
            break;

        case XML_CDATA_SECTION_NODE:
        case XML_TEXT_NODE:
        case XML_COMMENT_NODE:
        case XML_PI_NODE:
	    xmlBufCat(buf, cur->content);
            break;

        case XML_ENTITY_REF_NODE:
            xmlBufGetEntityRefContent(buf, cur);
            break;

        case XML_NAMESPACE_DECL:
	    xmlBufCat(buf, ((xmlNsPtr) cur)->href);
	    break;

        default:
            break;
    }

    return(0);
}

/**
 * Returns the string value of a node. For text nodes, the string
 * value is the text content. Otherwise, the string value is the
 * concatenation of the string values of the node's descendants.
 *
 * Entity references are substituted.
 *
 * It's up to the caller to free the result with #xmlFree.
 *
 * @param cur  the node being read
 * @returns a new string or NULL if arguments are invalid or a
 * memory allocation failed.
 */
xmlChar *
xmlNodeGetContent(const xmlNode *cur)
{
    xmlBufPtr buf;
    xmlChar *ret;

    if (cur == NULL)
        return (NULL);

    switch (cur->type) {
        case XML_DOCUMENT_NODE:
        case XML_HTML_DOCUMENT_NODE:
        case XML_ENTITY_REF_NODE:
            break;

        case XML_DOCUMENT_FRAG_NODE:
        case XML_ELEMENT_NODE:
        case XML_ATTRIBUTE_NODE:
        case XML_ENTITY_DECL: {
            xmlNodePtr children = cur->children;

            if (children == NULL)
                return(xmlStrdup(BAD_CAST ""));

            /* Optimization for single text children */
            if (((children->type == XML_TEXT_NODE) ||
                 (children->type == XML_CDATA_SECTION_NODE)) &&
                (children->next == NULL)) {
                if (children->content == NULL)
                    return(xmlStrdup(BAD_CAST ""));
                return(xmlStrdup(children->content));
            }

            break;
        }

        case XML_CDATA_SECTION_NODE:
        case XML_TEXT_NODE:
        case XML_COMMENT_NODE:
        case XML_PI_NODE:
            if (cur->content != NULL)
                return(xmlStrdup(cur->content));
            else
                return(xmlStrdup(BAD_CAST ""));

        case XML_NAMESPACE_DECL:
	    return(xmlStrdup(((xmlNsPtr) cur)->href));

        default:
            return(NULL);
    }

    buf = xmlBufCreate(50);
    if (buf == NULL)
        return (NULL);
    xmlBufGetNodeContent(buf, cur);
    ret = xmlBufDetach(buf);
    xmlBufFree(buf);

    return(ret);
}

static int
xmlNodeSetContentInternal(xmlNodePtr cur, const xmlChar *content, int len) {
    if (cur == NULL) {
	return(1);
    }
    switch (cur->type) {
        case XML_DOCUMENT_FRAG_NODE:
        case XML_ELEMENT_NODE:
        case XML_ATTRIBUTE_NODE: {
            size_t maxSize = len < 0 ? SIZE_MAX : (size_t) len;

            /*
             * We shouldn't parse the content as attribute value here,
             * but the API can't be changed.
             */
            if (xmlNodeParseAttValue(cur->doc, (xmlAttr *) cur,
                                     content, maxSize, NULL) < 0)
                return(-1);
	    break;
        }

        case XML_TEXT_NODE:
        case XML_CDATA_SECTION_NODE:
        case XML_PI_NODE:
        case XML_COMMENT_NODE: {
            xmlChar *copy = NULL;

	    if (content != NULL) {
                if (len < 0)
                    copy = xmlStrdup(content);
                else
		    copy = xmlStrndup(content, len);
                if (copy == NULL)
                    return(-1);
	    }

            xmlTextSetContent(cur, copy);
	    break;
        }

        default:
            break;
    }

    return(0);
}

/**
 * Replace the text content of a node.
 *
 * Sets the raw text content of text, CDATA, comment or PI nodes.
 *
 * For element and attribute nodes, removes all children and
 * replaces them by parsing `content` which is expected to be a
 * valid XML attribute value possibly containing character and
 * entity references. Syntax errors and references to undeclared
 * entities are ignored silently. Unfortunately, there isn't an
 * API to pass raw content directly. An inefficient work-around
 * is to escape the content with #xmlEncodeSpecialChars before
 * passing it. A better trick is clearing the old content
 * with `xmlNodeSetContent(node, NULL)` first and then calling
 * `xmlNodeAddContent(node, content)`. Unlike this function,
 * #xmlNodeAddContent accepts raw text.
 *
 * @param cur  the node being modified
 * @param content  the new value of the content
 * @returns 0 on success, 1 on error, -1 if a memory allocation failed.
 */
int
xmlNodeSetContent(xmlNode *cur, const xmlChar *content) {
    return(xmlNodeSetContentInternal(cur, content, -1));
}

/**
 * See #xmlNodeSetContent.
 *
 * @param cur  the node being modified
 * @param content  the new value of the content
 * @param len  the size of `content`
 * @returns 0 on success, 1 on error, -1 if a memory allocation failed.
 */
int
xmlNodeSetContentLen(xmlNode *cur, const xmlChar *content, int len) {
    return(xmlNodeSetContentInternal(cur, content, len));
}

/**
 * Append the extra substring to the node content.
 *
 * NOTE: In contrast to #xmlNodeSetContentLen, `content` is supposed
 * to be raw text, so unescaped XML special chars are allowed, entity
 * references are not supported.
 *
 * This doesn't work on attributes before version 2.15.
 *
 * @param cur  the node being modified
 * @param content  extra content
 * @param len  the size of `content`
 * @returns 0 on success, 1 on error, -1 if a memory allocation failed.
 */
int
xmlNodeAddContentLen(xmlNode *cur, const xmlChar *content, int len) {
    if (cur == NULL)
	return(1);
    if ((content == NULL) || (len <= 0))
        return(0);

    switch (cur->type) {
        case XML_DOCUMENT_FRAG_NODE:
        case XML_ELEMENT_NODE:
        case XML_ATTRIBUTE_NODE: {
	    xmlNodePtr newNode, tmp;

	    newNode = xmlNewDocTextLen(cur->doc, content, len);
	    if (newNode == NULL)
                return(-1);
            tmp = xmlAddChild(cur, newNode);
            if (tmp == NULL) {
                xmlFreeNode(newNode);
                return(-1);
            }
	    break;
	}
	    break;
        case XML_TEXT_NODE:
        case XML_CDATA_SECTION_NODE:
        case XML_PI_NODE:
        case XML_COMMENT_NODE:
            return(xmlTextAddContent(cur, content, len));
        default:
            break;
    }

    return(0);
}

/**
 * Append the extra substring to the node content.
 *
 * NOTE: In contrast to #xmlNodeSetContent, `content` is supposed
 * to be raw text, so unescaped XML special chars are allowed, entity
 * references are not supported.
 *
 * This doesn't work on attributes before version 2.15.
 *
 * @param cur  the node being modified
 * @param content  extra content
 * @returns 0 on success, 1 on error, -1 if a memory allocation failed.
 */
int
xmlNodeAddContent(xmlNode *cur, const xmlChar *content) {
    return(xmlNodeAddContentLen(cur, content, xmlStrlen(content)));
}

/**
 * Merge the second text node into the first. If `first` is NULL,
 * `second` is returned. Otherwise, the second node is unlinked and
 * freed.
 *
 * @param first  the first text node
 * @param second  the second text node being merged
 * @returns the first text node augmented or NULL in case of error.
 */
xmlNode *
xmlTextMerge(xmlNode *first, xmlNode *second) {
    if (first == NULL)
        return(second);
    if (second == NULL)
        return(first);

    if ((first->type != XML_TEXT_NODE) ||
        (second->type != XML_TEXT_NODE) ||
        (first == second) ||
        (first->name != second->name))
	return(NULL);

    if (xmlTextAddContent(first, second->content, -1) < 0)
        return(NULL);

    xmlUnlinkNodeInternal(second);
    xmlFreeNode(second);
    return(first);
}

/**
 * Find all in-scope namespaces of a node. `out` returns a NULL
 * terminated array of namespace pointers that must be freed by
 * the caller.
 *
 * @since 2.13.0
 *
 * @param doc  the document
 * @param node  the current node
 * @param out  the returned namespace array
 * @returns 0 on success, 1 if no namespaces were found, -1 if a
 * memory allocation failed.
 */
int
xmlGetNsListSafe(const xmlDoc *doc ATTRIBUTE_UNUSED, const xmlNode *node,
                 xmlNs ***out)
{
    xmlNsPtr cur;
    xmlNsPtr *namespaces = NULL;
    int nbns = 0;
    int maxns = 0;
    int i;

    if (out == NULL)
        return(1);
    *out = NULL;
    if ((node == NULL) || (node->type == XML_NAMESPACE_DECL))
        return(1);

    while (node != NULL) {
        if (node->type == XML_ELEMENT_NODE) {
            cur = node->nsDef;
            while (cur != NULL) {
                for (i = 0; i < nbns; i++) {
                    if ((cur->prefix == namespaces[i]->prefix) ||
                        (xmlStrEqual(cur->prefix, namespaces[i]->prefix)))
                        break;
                }
                if (i >= nbns) {
                    if (nbns >= maxns) {
                        xmlNsPtr *tmp;
                        int newSize;

                        newSize = xmlGrowCapacity(maxns, sizeof(tmp[0]),
                                                  10, XML_MAX_ITEMS);
                        if (newSize < 0) {
                            xmlFree(namespaces);
                            return(-1);
                        }
#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
                        if (newSize < 2)
                            newSize = 2;
#endif
                        tmp = xmlRealloc(namespaces,
                                         (newSize + 1) * sizeof(tmp[0]));
                        if (tmp == NULL) {
                            xmlFree(namespaces);
                            return(-1);
                        }
                        namespaces = tmp;
                        maxns = newSize;
                    }
                    namespaces[nbns++] = cur;
                    namespaces[nbns] = NULL;
                }

                cur = cur->next;
            }
        }
        node = node->parent;
    }

    *out = namespaces;
    return((namespaces == NULL) ? 1 : 0);
}

/**
 * Find all in-scope namespaces of a node.
 *
 * Use #xmlGetNsListSafe for better error reporting.
 *
 * @param doc  the document
 * @param node  the current node
 * @returns a NULL terminated array of namespace pointers that must
 * be freed by the caller or NULL if no namespaces were found or
 * a memory allocation failed.
 */
xmlNs **
xmlGetNsList(const xmlDoc *doc, const xmlNode *node)
{
    xmlNsPtr *ret;

    xmlGetNsListSafe(doc, node, &ret);
    return(ret);
}

static xmlNsPtr
xmlNewXmlNs(void) {
    xmlNsPtr ns;

    ns = (xmlNsPtr) xmlMalloc(sizeof(xmlNs));
    if (ns == NULL)
        return(NULL);
    memset(ns, 0, sizeof(xmlNs));
    ns->type = XML_LOCAL_NAMESPACE;
    ns->href = xmlStrdup(XML_XML_NAMESPACE);
    if (ns->href == NULL) {
        xmlFreeNs(ns);
        return(NULL);
    }
    ns->prefix = xmlStrdup(BAD_CAST "xml");
    if (ns->prefix == NULL) {
        xmlFreeNs(ns);
        return(NULL);
    }

    return(ns);
}

/*
* xmlTreeEnsureXMLDecl:
* @doc: the doc
*
* Ensures that there is an XML namespace declaration on the doc.
*
* Returns the XML ns-struct or NULL if a memory allocation failed.
*/
static xmlNsPtr
xmlTreeEnsureXMLDecl(xmlDocPtr doc)
{
    xmlNsPtr ns;

    ns = doc->oldNs;
    if (ns != NULL)
	return (ns);

    ns = xmlNewXmlNs();
    doc->oldNs = ns;

    return(ns);
}

/**
 * Search for a namespace with `prefix` in scope of `node`.
 *
 * @param node  a node
 * @param prefix  a namespace prefix
 * @param out  pointer to resulting namespace
 * @returns 0 on success, -1 if a memory allocation failed, 1 on
 * other errors.
 */
int
xmlSearchNsSafe(xmlNode *node, const xmlChar *prefix,
                xmlNs **out) {
    xmlNsPtr cur;
    xmlDocPtr doc;
    xmlNodePtr orig = node;
    xmlNodePtr parent;

    if (out == NULL)
        return(1);
    *out = NULL;
    if ((node == NULL) || (node->type == XML_NAMESPACE_DECL))
        return(1);

    doc = node->doc;

    if ((doc != NULL) && (IS_STR_XML(prefix))) {
        cur = xmlTreeEnsureXMLDecl(doc);
        if (cur == NULL)
            return(-1);
        *out = cur;
        return(0);
    }

    while (node->type != XML_ELEMENT_NODE) {
        node = node->parent;
        if (node == NULL)
            return(0);
    }

    parent = node;

    while ((node != NULL) && (node->type == XML_ELEMENT_NODE)) {
        cur = node->nsDef;
        while (cur != NULL) {
            if ((xmlStrEqual(cur->prefix, prefix)) &&
                (cur->href != NULL)) {
                *out = cur;
                return(0);
            }
            cur = cur->next;
        }
        if (orig != node) {
            cur = node->ns;
            if ((cur != NULL) &&
                (xmlStrEqual(cur->prefix, prefix)) &&
                (cur->href != NULL)) {
                *out = cur;
                return(0);
            }
        }

	node = node->parent;
    }

    /*
     * The XML-1.0 namespace is normally held on the document
     * element. In this case exceptionally create it on the
     * node element.
     */
    if ((doc == NULL) && (IS_STR_XML(prefix))) {
        cur = xmlNewXmlNs();
        if (cur == NULL)
            return(-1);
        cur->next = parent->nsDef;
        parent->nsDef = cur;
        *out = cur;
    }

    return(0);
}

/**
 * Search for a namespace with `prefix` in scope of `node`.
 *
 * Recurse on the parents until it finds the defined namespace
 * or return NULL otherwise.
 *
 * If `nameSpace` is NULL, the default namespace is looked up.
 *
 * Namespace search doesn't cross entity boundaries.
 *
 * @param doc  the document
 * @param node  the current node
 * @param nameSpace  the namespace prefix
 * @returns the namespace pointer or NULL if no namespace was found or
 * a memory allocation failed. Allocations can only fail if the `xml`
 * namespace is queried.
 */
xmlNs *
xmlSearchNs(xmlDoc *doc ATTRIBUTE_UNUSED, xmlNode *node,
            const xmlChar *nameSpace) {
    xmlNsPtr cur;

    xmlSearchNsSafe(node, nameSpace, &cur);
    return(cur);
}

/**
 * Verify that the given namespace held on `ancestor` is still in scope
 * on node.
 *
 * @param doc  the document
 * @param node  the current node
 * @param ancestor  the ancestor carrying the namespace
 * @param prefix  the namespace prefix
 * @returns 1 if true, 0 if false and -1 in case of error.
 */
static int
xmlNsInScope(xmlDocPtr doc ATTRIBUTE_UNUSED, xmlNodePtr node,
             xmlNodePtr ancestor, const xmlChar * prefix)
{
    xmlNsPtr tst;

    while ((node != NULL) && (node != ancestor)) {
        if ((node->type == XML_ENTITY_REF_NODE) ||
            (node->type == XML_ENTITY_DECL))
            return (-1);
        if (node->type == XML_ELEMENT_NODE) {
            tst = node->nsDef;
            while (tst != NULL) {
                if ((tst->prefix == NULL)
                    && (prefix == NULL))
                    return (0);
                if ((tst->prefix != NULL)
                    && (prefix != NULL)
                    && (xmlStrEqual(tst->prefix, prefix)))
                    return (0);
                tst = tst->next;
            }
        }
        node = node->parent;
    }
    if (node != ancestor)
        return (-1);
    return (1);
}

/**
 * Search for a namespace matching `URI` in scope of `node`.
 *
 * @param node  a node
 * @param href  a namespace URI
 * @param out  pointer to resulting namespace
 * @returns 0 on success, -1 if a memory allocation failed, 1 on
 * other errors.
 */
int
xmlSearchNsByHrefSafe(xmlNode *node, const xmlChar *href,
                      xmlNs **out) {
    xmlNsPtr cur;
    xmlDocPtr doc;
    xmlNodePtr orig = node;
    xmlNodePtr parent;
    int is_attr;

    if (out == NULL)
        return(1);
    *out = NULL;
    if ((node == NULL) || (node->type == XML_NAMESPACE_DECL))
        return(1);

    doc = node->doc;

    if ((doc != NULL) && (xmlStrEqual(href, XML_XML_NAMESPACE))) {
        cur = xmlTreeEnsureXMLDecl(doc);
        if (cur == NULL)
            return(-1);
        *out = cur;
        return(0);
    }

    is_attr = (node->type == XML_ATTRIBUTE_NODE);

    while (node->type != XML_ELEMENT_NODE) {
        node = node->parent;
        if (node == NULL)
            return(0);
    }

    parent = node;

    while ((node != NULL) && (node->type == XML_ELEMENT_NODE)) {
        cur = node->nsDef;
        while (cur != NULL) {
            if (xmlStrEqual(cur->href, href)) {
                if (((!is_attr) || (cur->prefix != NULL)) &&
                    (xmlNsInScope(doc, orig, node, cur->prefix) == 1)) {
                    *out = cur;
                    return(0);
                }
            }
            cur = cur->next;
        }
        if (orig != node) {
            cur = node->ns;
            if (cur != NULL) {
                if (xmlStrEqual(cur->href, href)) {
                    if (((!is_attr) || (cur->prefix != NULL)) &&
                        (xmlNsInScope(doc, orig, node,
                                      cur->prefix) == 1)) {
                        *out = cur;
                        return(0);
                    }
                }
            }
        }

        node = node->parent;
    }

    /*
     * The XML-1.0 namespace is normally held on the document
     * element. In this case exceptionally create it on the
     * node element.
     */
    if ((doc == NULL) && (xmlStrEqual(href, XML_XML_NAMESPACE))) {
        cur = xmlNewXmlNs();
        if (cur == NULL)
            return(-1);
        cur->next = parent->nsDef;
        parent->nsDef = cur;
        *out = cur;
    }

    return(0);
}

/**
 * Search for a namespace matching `URI` in scope of `node`.
 *
 * @param doc  the document
 * @param node  the current node
 * @param href  the namespace value
 * @returns the namespace pointer or NULL if no namespace was found or
 * a memory allocation failed. Allocations can only fail if the `xml`
 * namespace is queried.
 */
xmlNs *
xmlSearchNsByHref(xmlDoc *doc ATTRIBUTE_UNUSED, xmlNode *node,
                  const xmlChar * href) {
    xmlNsPtr cur;

    xmlSearchNsByHrefSafe(node, href, &cur);
    return(cur);
}

/**
 * Fix up namespace declarations.
 *
 * This function tries to locate a namespace definition in a tree
 * ancestors, or create a new namespace definition node similar to
 * `ns` trying to reuse the same prefix. However if the given prefix is
 * null (default namespace) or reused within the subtree defined by
 * `tree` or on one of its ancestors then a new prefix is generated.
 *
 * @param tree  a node expected to hold the new namespace
 * @param ns  the original namespace
 * @returns the (new) namespace definition or NULL in case of error
 */
static xmlNsPtr
xmlNewReconciledNs(xmlNodePtr tree, xmlNsPtr ns) {
    xmlNsPtr def;
    xmlChar prefix[50];
    int counter = 1;
    int res;

    if ((tree == NULL) || (tree->type != XML_ELEMENT_NODE)) {
	return(NULL);
    }
    if ((ns == NULL) || (ns->type != XML_NAMESPACE_DECL)) {
	return(NULL);
    }
    /*
     * Search an existing namespace definition inherited.
     */
    res = xmlSearchNsByHrefSafe(tree, ns->href, &def);
    if (res < 0)
        return(NULL);
    if (def != NULL)
        return(def);

    /*
     * Find a close prefix which is not already in use.
     * Let's strip namespace prefixes longer than 20 chars !
     */
    if (ns->prefix == NULL)
	snprintf((char *) prefix, sizeof(prefix), "default");
    else
	snprintf((char *) prefix, sizeof(prefix), "%.20s", (char *)ns->prefix);

    res = xmlSearchNsSafe(tree, prefix, &def);
    if (res < 0)
        return(NULL);
    while (def != NULL) {
        if (counter > 1000) return(NULL);
	if (ns->prefix == NULL)
	    snprintf((char *) prefix, sizeof(prefix), "default%d", counter++);
	else
	    snprintf((char *) prefix, sizeof(prefix), "%.20s%d",
		(char *)ns->prefix, counter++);
	res = xmlSearchNsSafe(tree, prefix, &def);
        if (res < 0)
            return(NULL);
    }

    /*
     * OK, now we are ready to create a new one.
     */
    def = xmlNewNs(tree, ns->href, prefix);
    return(def);
}

typedef struct {
    xmlNsPtr oldNs;
    xmlNsPtr newNs;
} xmlNsCache;

static int
xmlGrowNsCache(xmlNsCache **cache, int *capacity) {
    xmlNsCache *tmp;
    int newSize;

    newSize = xmlGrowCapacity(*capacity, sizeof(tmp[0]),
                              10, XML_MAX_ITEMS);
    if (newSize < 0)
        return(-1);
    tmp = xmlRealloc(*cache, newSize * sizeof(tmp[0]));
    if (tmp == NULL)
        return(-1);
    *cache = tmp;
    *capacity = newSize;

    return(0);
}

/**
 * This function checks that all the namespaces declared within the given
 * tree are properly declared. This is needed for example after copy or cut
 * and then paste operations. The subtree may still hold pointers to
 * namespace declarations outside the subtree or invalid/masked. As much
 * as possible the function tries to reuse the existing namespaces found in
 * the new environment. If not possible the new namespaces are redeclared
 * on `tree` at the top of the given subtree.
 *
 * @param doc  the document
 * @param tree  a node defining the subtree to reconciliate
 * @returns 0 on success or -1 in case of error.
 */
int
xmlReconciliateNs(xmlDoc *doc, xmlNode *tree) {
    xmlNsCache *cache = NULL;
    int sizeCache = 0;
    int nbCache = 0;

    xmlNsPtr n;
    xmlNodePtr node = tree;
    xmlAttrPtr attr;
    int ret = 0, i;

    if ((node == NULL) || (node->type != XML_ELEMENT_NODE)) return(-1);
    if (node->doc != doc) return(-1);
    while (node != NULL) {
        /*
	 * Reconciliate the node namespace
	 */
	if (node->ns != NULL) {
	    for (i = 0; i < nbCache; i++) {
	        if (cache[i].oldNs == node->ns) {
		    node->ns = cache[i].newNs;
		    break;
		}
	    }
	    if (i == nbCache) {
	        /*
		 * OK we need to recreate a new namespace definition
		 */
		n = xmlNewReconciledNs(tree, node->ns);
		if (n == NULL) {
                    ret = -1;
                } else {
		    /*
		     * check if we need to grow the cache buffers.
		     */
		    if ((sizeCache <= nbCache) &&
                        (xmlGrowNsCache(&cache, &sizeCache) < 0)) {
                        ret = -1;
		    } else {
                        cache[nbCache].newNs = n;
                        cache[nbCache++].oldNs = node->ns;
                    }
                }
		node->ns = n;
	    }
	}
	/*
	 * now check for namespace held by attributes on the node.
	 */
	if (node->type == XML_ELEMENT_NODE) {
	    attr = node->properties;
	    while (attr != NULL) {
		if (attr->ns != NULL) {
		    for (i = 0; i < nbCache; i++) {
			if (cache[i].oldNs == attr->ns) {
			    attr->ns = cache[i].newNs;
			    break;
			}
		    }
		    if (i == nbCache) {
			/*
			 * OK we need to recreate a new namespace definition
			 */
			n = xmlNewReconciledNs(tree, attr->ns);
			if (n == NULL) {
                            ret = -1;
                        } else {
			    /*
			     * check if we need to grow the cache buffers.
			     */
                            if ((sizeCache <= nbCache) &&
                                (xmlGrowNsCache(&cache, &sizeCache) < 0)) {
                                ret = -1;
                            } else {
                                cache[nbCache].newNs = n;
                                cache[nbCache++].oldNs = attr->ns;
			    }
			}
			attr->ns = n;
		    }
		}
		attr = attr->next;
	    }
	}

	/*
	 * Browse the full subtree, deep first
	 */
        if ((node->children != NULL) && (node->type != XML_ENTITY_REF_NODE)) {
	    /* deep first */
	    node = node->children;
	} else if ((node != tree) && (node->next != NULL)) {
	    /* then siblings */
	    node = node->next;
	} else if (node != tree) {
	    /* go up to parents->next if needed */
	    while (node != tree) {
	        if (node->parent != NULL)
		    node = node->parent;
		if ((node != tree) && (node->next != NULL)) {
		    node = node->next;
		    break;
		}
		if (node->parent == NULL) {
		    node = NULL;
		    break;
		}
	    }
	    /* exit condition */
	    if (node == tree)
	        node = NULL;
	} else
	    break;
    }
    if (cache != NULL)
	xmlFree(cache);
    return(ret);
}

static xmlAttrPtr
xmlGetPropNodeInternal(const xmlNode *node, const xmlChar *name,
		       const xmlChar *nsName, int useDTD)
{
    xmlAttrPtr prop;

    /* Avoid unused variable warning if features are disabled. */
    (void) useDTD;

    if ((node == NULL) || (node->type != XML_ELEMENT_NODE) || (name == NULL))
	return(NULL);

    if (node->properties != NULL) {
	prop = node->properties;
	if (nsName == NULL) {
	    /*
	    * We want the attr to be in no namespace.
	    */
	    do {
		if ((prop->ns == NULL) && xmlStrEqual(prop->name, name)) {
		    return(prop);
		}
		prop = prop->next;
	    } while (prop != NULL);
	} else {
	    /*
	    * We want the attr to be in the specified namespace.
	    */
	    do {
		if ((prop->ns != NULL) && xmlStrEqual(prop->name, name) &&
		    ((prop->ns->href == nsName) ||
		     xmlStrEqual(prop->ns->href, nsName)))
		{
		    return(prop);
		}
		prop = prop->next;
	    } while (prop != NULL);
	}
    }

    if (! useDTD)
	return(NULL);
    /*
     * Check if there is a default/fixed attribute declaration in
     * the internal or external subset.
     */
    if ((node->doc != NULL) && (node->doc->intSubset != NULL)) {
	xmlDocPtr doc = node->doc;
	xmlAttributePtr attrDecl = NULL;
	xmlChar *elemQName, *tmpstr = NULL;

	/*
	* We need the QName of the element for the DTD-lookup.
	*/
	if ((node->ns != NULL) && (node->ns->prefix != NULL)) {
	    tmpstr = xmlStrdup(node->ns->prefix);
	    if (tmpstr == NULL)
		return(NULL);
	    tmpstr = xmlStrcat(tmpstr, BAD_CAST ":");
	    if (tmpstr == NULL)
		return(NULL);
	    tmpstr = xmlStrcat(tmpstr, node->name);
	    if (tmpstr == NULL)
		return(NULL);
	    elemQName = tmpstr;
	} else
	    elemQName = (xmlChar *) node->name;
	if (nsName == NULL) {
	    /*
	    * The common and nice case: Attr in no namespace.
	    */
	    attrDecl = xmlGetDtdQAttrDesc(doc->intSubset,
		elemQName, name, NULL);
	    if ((attrDecl == NULL) && (doc->extSubset != NULL)) {
		attrDecl = xmlGetDtdQAttrDesc(doc->extSubset,
		    elemQName, name, NULL);
	    }
        } else if (xmlStrEqual(nsName, XML_XML_NAMESPACE)) {
	    /*
	    * The XML namespace must be bound to prefix 'xml'.
	    */
	    attrDecl = xmlGetDtdQAttrDesc(doc->intSubset,
		elemQName, name, BAD_CAST "xml");
	    if ((attrDecl == NULL) && (doc->extSubset != NULL)) {
		attrDecl = xmlGetDtdQAttrDesc(doc->extSubset,
		    elemQName, name, BAD_CAST "xml");
	    }
	} else {
	    xmlNsPtr *nsList, *cur;

	    /*
	    * The ugly case: Search using the prefixes of in-scope
	    * ns-decls corresponding to @nsName.
	    */
	    nsList = xmlGetNsList(node->doc, node);
	    if (nsList == NULL) {
		if (tmpstr != NULL)
		    xmlFree(tmpstr);
		return(NULL);
	    }
	    cur = nsList;
	    while (*cur != NULL) {
		if (xmlStrEqual((*cur)->href, nsName)) {
		    attrDecl = xmlGetDtdQAttrDesc(doc->intSubset, elemQName,
			name, (*cur)->prefix);
		    if (attrDecl)
			break;
		    if (doc->extSubset != NULL) {
			attrDecl = xmlGetDtdQAttrDesc(doc->extSubset, elemQName,
			    name, (*cur)->prefix);
			if (attrDecl)
			    break;
		    }
		}
		cur++;
	    }
	    xmlFree(nsList);
	}
	if (tmpstr != NULL)
	    xmlFree(tmpstr);
	/*
	* Only default/fixed attrs are relevant.
	*/
	if ((attrDecl != NULL) && (attrDecl->defaultValue != NULL))
	    return((xmlAttrPtr) attrDecl);
    }

    return(NULL);
}

static xmlChar*
xmlGetPropNodeValueInternal(const xmlAttr *prop)
{
    if (prop == NULL)
	return(NULL);
    if (prop->type == XML_ATTRIBUTE_NODE) {
	return(xmlNodeGetContent((xmlNodePtr) prop));
    } else if (prop->type == XML_ATTRIBUTE_DECL) {
	return(xmlStrdup(((xmlAttributePtr)prop)->defaultValue));
    }
    return(NULL);
}

/**
 * Search for an attribute of an element.
 *
 * This function also looks in DTD attribute declaration for \#FIXED or
 * default declaration values.
 *
 * @param node  the element
 * @param name  the attribute name
 * @returns the attribute or the attribute declaration or NULL if
 * neither was found. Also returns NULL if a memory allocation failed,
 * making this function unreliable.
 */
xmlAttr *
xmlHasProp(const xmlNode *node, const xmlChar *name) {
    xmlAttrPtr prop;
    xmlDocPtr doc;

    if ((node == NULL) || (node->type != XML_ELEMENT_NODE) || (name == NULL))
        return(NULL);
    /*
     * Check on the properties attached to the node
     */
    prop = node->properties;
    while (prop != NULL) {
        if (xmlStrEqual(prop->name, name))  {
	    return(prop);
        }
	prop = prop->next;
    }

    /*
     * Check if there is a default declaration in the internal
     * or external subsets
     */
    doc =  node->doc;
    if (doc != NULL) {
        xmlAttributePtr attrDecl;
        if (doc->intSubset != NULL) {
	    attrDecl = xmlGetDtdAttrDesc(doc->intSubset, node->name, name);
	    if ((attrDecl == NULL) && (doc->extSubset != NULL))
		attrDecl = xmlGetDtdAttrDesc(doc->extSubset, node->name, name);
            if ((attrDecl != NULL) && (attrDecl->defaultValue != NULL))
              /* return attribute declaration only if a default value is given
                 (that includes #FIXED declarations) */
		return((xmlAttrPtr) attrDecl);
	}
    }
    return(NULL);
}

/**
 * Search for an attribute of an element.
 *
 * The attribute has to match the specified namespace. A namespace of
 * NULL means that the attribute must have no namespace.
 *
 * This function also looks in DTD attribute declaration for \#FIXED or
 * default declaration values.
 *
 * @param node  the element
 * @param name  the attribute name
 * @param nameSpace  the URI of the namespace
 * @returns the attribute or the attribute declaration or NULL if
 * neither was found. Also returns NULL if a memory allocation failed
 * making this function unreliable.
 */
xmlAttr *
xmlHasNsProp(const xmlNode *node, const xmlChar *name, const xmlChar *nameSpace) {

    return(xmlGetPropNodeInternal(node, name, nameSpace, 1));
}

/**
 * Look up the value of an element's attribute.
 *
 * The attribute has to match the specified namespace. A namespace of
 * NULL means that the attribute must have no namespace.
 *
 * Entities are substituted. The returned value must be freed by the
 * caller.
 *
 * @since 2.13.0
 *
 * @param node  the element
 * @param name  the attribute name
 * @param nsUri  the URI of the namespace
 * @param out  the returned string
 * @returns 0 on success, 1 if no attribute was found, -1 if a
 * memory allocation failed.
 */
int
xmlNodeGetAttrValue(const xmlNode *node, const xmlChar *name,
                    const xmlChar *nsUri, xmlChar **out) {
    xmlAttrPtr prop;

    if (out == NULL)
        return(1);
    *out = NULL;

    prop = xmlGetPropNodeInternal(node, name, nsUri, 0);
    if (prop == NULL)
	return(1);

    *out = xmlGetPropNodeValueInternal(prop);
    if (*out == NULL)
        return(-1);
    return(0);
}

/**
 * Look up the value of an element's attribute.
 *
 * Entities are substituted. The returned value must be freed by the
 * caller.
 *
 * This function looks in DTD attribute declarations for \#FIXED or
 * default declaration values.
 *
 * NOTE: This function is ignores namespaces. Use #xmlGetNsProp or
 * #xmlGetNoNsProp for namespace aware processing.
 *
 * NOTE: This function doesn't allow to distinguish malloc failures from
 * missing attributes.
 *
 * @param node  the element
 * @param name  the attribute name
 * @returns the attribute value or NULL if not found or a memory allocation
 * failed.
 */
xmlChar *
xmlGetProp(const xmlNode *node, const xmlChar *name) {
    xmlAttrPtr prop;

    prop = xmlHasProp(node, name);
    if (prop == NULL)
	return(NULL);
    return(xmlGetPropNodeValueInternal(prop));
}

/**
 * Look up the value of an element's attribute.
 *
 * Entities are substituted. The returned value must be freed by the
 * caller.
 *
 * This function looks in DTD attribute declarations for \#FIXED or
 * default declaration values.
 *
 * This function is similar to #xmlGetProp except it will accept only
 * an attribute in no namespace.
 *
 * NOTE: This function doesn't allow to distinguish malloc failures from
 * missing attributes. It's more robust to use #xmlNodeGetAttrValue.
 *
 * @param node  the element
 * @param name  the attribute name
 * @returns the attribute value or NULL if not found or a memory allocation
 * failed.
 */
xmlChar *
xmlGetNoNsProp(const xmlNode *node, const xmlChar *name) {
    xmlAttrPtr prop;

    prop = xmlGetPropNodeInternal(node, name, NULL, 1);
    if (prop == NULL)
	return(NULL);
    return(xmlGetPropNodeValueInternal(prop));
}

/**
 * Look up the value of an element's attribute.
 *
 * The attribute has to match the specified namespace. A namespace of
 * NULL means that the attribute must have no namespace.
 *
 * Entities are substituted. The returned value must be freed by the
 * caller.
 *
 * This function looks in DTD attribute declaration for \#FIXED or
 * default declaration values.
 *
 * NOTE: This function doesn't allow to distinguish malloc failures from
 * missing attributes. It's more robust to use #xmlNodeGetAttrValue.
 *
 * @param node  the element
 * @param name  the attribute name
 * @param nameSpace  the URI of the namespace
 * @returns the attribute value or NULL if not found or a memory allocation
 * failed.
 */
xmlChar *
xmlGetNsProp(const xmlNode *node, const xmlChar *name, const xmlChar *nameSpace) {
    xmlAttrPtr prop;

    prop = xmlGetPropNodeInternal(node, name, nameSpace, 1);
    if (prop == NULL)
	return(NULL);
    return(xmlGetPropNodeValueInternal(prop));
}

/**
 * Remove an attribute of an element.
 * This handles only attributes in no namespace.
 *
 * @param node  the element
 * @param name  the attribute name
 * @returns 0 if successful, -1 if not found
 */
int
xmlUnsetProp(xmlNode *node, const xmlChar *name) {
    xmlAttrPtr prop;

    prop = xmlGetPropNodeInternal(node, name, NULL, 0);
    if (prop == NULL)
	return(-1);
    xmlUnlinkNodeInternal((xmlNodePtr) prop);
    xmlFreeProp(prop);
    return(0);
}

/**
 * Remove an attribute of an element.
 *
 * @param node  the element
 * @param ns  the namespace definition
 * @param name  the attribute name
 * @returns 0 if successful, -1 if not found
 */
int
xmlUnsetNsProp(xmlNode *node, xmlNs *ns, const xmlChar *name) {
    xmlAttrPtr prop;

    prop = xmlGetPropNodeInternal(node, name,
                                  (ns != NULL) ? ns->href : NULL, 0);
    if (prop == NULL)
	return(-1);
    xmlUnlinkNodeInternal((xmlNodePtr) prop);
    xmlFreeProp(prop);
    return(0);
}

/**
 * Set (or reset) an element's attribute. If `name` has a prefix,
 * the corresponding namespace will be used. It is an error if
 * there's no such binding for the prefix in scope.
 *
 * @param node  the node
 * @param name  the attribute name (a QName)
 * @param value  the attribute value
 * @returns the attribute pointer.
 */
xmlAttr *
xmlSetProp(xmlNode *node, const xmlChar *name, const xmlChar *value) {
    xmlNsPtr ns = NULL;
    const xmlChar *localname;
    xmlChar *prefix;
    int res;

    if ((node == NULL) || (name == NULL) || (node->type != XML_ELEMENT_NODE))
	return(NULL);

    /*
     * handle QNames
     */
    localname = xmlSplitQName4(name, &prefix);
    if (localname == NULL)
        return(NULL);

    if (prefix != NULL) {
	res = xmlSearchNsSafe(node, prefix, &ns);
	xmlFree(prefix);
        if (res < 0)
            return(NULL);
        if (ns != NULL)
            return(xmlSetNsProp(node, ns, localname, value));
    }

    return(xmlSetNsProp(node, NULL, name, value));
}

/**
 * Set (or reset) an element's attribute.
 *
 * The namespace must be in scope.
 *
 * @param node  the node
 * @param ns  the namespace definition
 * @param name  the attribute name
 * @param value  the attribute value
 * @returns the attribute pointer.
 */
xmlAttr *
xmlSetNsProp(xmlNode *node, xmlNs *ns, const xmlChar *name,
	     const xmlChar *value)
{
    xmlAttrPtr prop;

    if (ns && (ns->href == NULL))
	return(NULL);
    if (name == NULL)
        return(NULL);
    prop = xmlGetPropNodeInternal(node, name,
                                  (ns != NULL) ? ns->href : NULL, 0);
    if (prop != NULL) {
        xmlNodePtr children = NULL;

	/*
	* Modify the attribute's value.
	*/
        if (value != NULL) {
	    children = xmlNewDocText(node->doc, value);
            if (children == NULL)
                return(NULL);
        }

	if (prop->id != NULL) {
	    xmlRemoveID(node->doc, prop);
	    prop->atype = XML_ATTRIBUTE_ID;
	}
	if (prop->children != NULL)
	    xmlFreeNodeList(prop->children);
	prop->children = NULL;
	prop->last = NULL;
	prop->ns = ns;
	if (value != NULL) {
	    xmlNodePtr tmp;

	    prop->children = children;
	    prop->last = NULL;
	    tmp = prop->children;
	    while (tmp != NULL) {
		tmp->parent = (xmlNodePtr) prop;
		if (tmp->next == NULL)
		    prop->last = tmp;
		tmp = tmp->next;
	    }
	}
	if ((prop->atype == XML_ATTRIBUTE_ID) &&
	    (xmlAddIDSafe(prop, value) < 0)) {
            return(NULL);
        }
	return(prop);
    }
    /*
    * No equal attr found; create a new one.
    */
    return(xmlNewPropInternal(node, ns, name, value, 0));
}

/**
 * Check whether the node is a text node.
 *
 * @param node  the node
 * @returns 1 if yes, 0 if no
 */
int
xmlNodeIsText(const xmlNode *node) {
    if (node == NULL) return(0);

    if (node->type == XML_TEXT_NODE) return(1);
    return(0);
}

/**
 * Checks whether this node is an empty or whitespace-only
 * text node.
 *
 * @param node  the node
 * @returns 1 if yes, 0 if no
 */
int
xmlIsBlankNode(const xmlNode *node) {
    const xmlChar *cur;
    if (node == NULL) return(0);

    if ((node->type != XML_TEXT_NODE) &&
        (node->type != XML_CDATA_SECTION_NODE))
	return(0);
    if (node->content == NULL) return(1);
    cur = node->content;
    while (*cur != 0) {
	if (!IS_BLANK_CH(*cur)) return(0);
	cur++;
    }

    return(1);
}

/**
 * Concat the given string at the end of the existing node content.
 *
 * If `len` is -1, the string length will be calculated.
 *
 * @param node  the node
 * @param content  the content
 * @param len  `content` length
 * @returns -1 in case of error, 0 otherwise
 */

int
xmlTextConcat(xmlNode *node, const xmlChar *content, int len) {
    if (node == NULL)
        return(-1);

    if ((node->type != XML_TEXT_NODE) &&
        (node->type != XML_CDATA_SECTION_NODE) &&
	(node->type != XML_COMMENT_NODE) &&
	(node->type != XML_PI_NODE))
        return(-1);

    return(xmlTextAddContent(node, content, len));
}

/**
 * Get the compression level of a document, ZLIB based.
 *
 * @param doc  the document
 * @returns 0 (uncompressed) to 9 (max compression)
 */
int
xmlGetDocCompressMode (const xmlDoc *doc) {
    if (doc == NULL) return(-1);
    return(doc->compression);
}

/**
 * Set the compression level of a document, ZLIB based.
 *
 * Correct values: 0 (uncompressed) to 9 (max compression)
 *
 * @param doc  the document
 * @param mode  the compression ratio
 */
void
xmlSetDocCompressMode (xmlDoc *doc, int mode) {
    if (doc == NULL) return;
    if (mode < 0) doc->compression = 0;
    else if (mode > 9) doc->compression = 9;
    else doc->compression = mode;
}

/**
 * Get the global compression level, ZLIB based.
 *
 * @deprecated Use #xmlGetDocCompressMode
 *
 * @returns 0 (uncompressed) to 9 (max compression)
 */
int
xmlGetCompressMode(void)
{
    return (xmlCompressMode);
}

/**
 * Set the global compression level, ZLIB based.
 *
 * @deprecated Use #xmlSetDocCompressMode
 *
 * Correct values: 0 (uncompressed) to 9 (max compression)
 *
 * @param mode  the compression ratio
 */
void
xmlSetCompressMode(int mode) {
    if (mode < 0) xmlCompressMode = 0;
    else if (mode > 9) xmlCompressMode = 9;
    else xmlCompressMode = mode;
}

#define XML_TREE_NSMAP_PARENT -1
#define XML_TREE_NSMAP_XML -2
#define XML_TREE_NSMAP_DOC -3
#define XML_TREE_NSMAP_CUSTOM -4

typedef struct xmlNsMapItem *xmlNsMapItemPtr;
struct xmlNsMapItem {
    xmlNsMapItemPtr next;
    xmlNsMapItemPtr prev;
    xmlNsPtr oldNs; /* old ns decl reference */
    xmlNsPtr newNs; /* new ns decl reference */
    int shadowDepth; /* Shadowed at this depth */
    /*
    * depth:
    * >= 0 == @node's ns-decls
    * -1   == @parent's ns-decls
    * -2   == the doc->oldNs XML ns-decl
    * -3   == the doc->oldNs storage ns-decls
    * -4   == ns-decls provided via custom ns-handling
    */
    int depth;
};

typedef struct xmlNsMap *xmlNsMapPtr;
struct xmlNsMap {
    xmlNsMapItemPtr first;
    xmlNsMapItemPtr last;
    xmlNsMapItemPtr pool;
};

#define XML_NSMAP_NOTEMPTY(m) (((m) != NULL) && ((m)->first != NULL))
#define XML_NSMAP_FOREACH(m, i) for (i = (m)->first; i != NULL; i = (i)->next)
#define XML_NSMAP_POP(m, i) \
    i = (m)->last; \
    (m)->last = (i)->prev; \
    if ((m)->last == NULL) \
	(m)->first = NULL; \
    else \
	(m)->last->next = NULL; \
    (i)->next = (m)->pool; \
    (m)->pool = i;

/**
 * Frees the ns-map
 *
 * @param nsmap  the ns-map
 */
static void
xmlDOMWrapNsMapFree(xmlNsMapPtr nsmap)
{
    xmlNsMapItemPtr cur, tmp;

    if (nsmap == NULL)
	return;
    cur = nsmap->pool;
    while (cur != NULL) {
	tmp = cur;
	cur = cur->next;
	xmlFree(tmp);
    }
    cur = nsmap->first;
    while (cur != NULL) {
	tmp = cur;
	cur = cur->next;
	xmlFree(tmp);
    }
    xmlFree(nsmap);
}

/**
 * Adds an ns-mapping item.
 *
 * @param nsmap  the ns-map
 * @param position  position
 * @param oldNs  the old ns-struct
 * @param newNs  the new ns-struct
 * @param depth  depth and ns-kind information
 * @returns the added item.
 */
static xmlNsMapItemPtr
xmlDOMWrapNsMapAddItem(xmlNsMapPtr *nsmap, int position,
		       xmlNsPtr oldNs, xmlNsPtr newNs, int depth)
{
    xmlNsMapItemPtr ret;
    xmlNsMapPtr map;

    if (nsmap == NULL)
	return(NULL);
    if ((position != -1) && (position != 0))
	return(NULL);
    map = *nsmap;

    if (map == NULL) {
	/*
	* Create the ns-map.
	*/
	map = (xmlNsMapPtr) xmlMalloc(sizeof(struct xmlNsMap));
	if (map == NULL)
	    return(NULL);
	memset(map, 0, sizeof(struct xmlNsMap));
	*nsmap = map;
    }

    if (map->pool != NULL) {
	/*
	* Reuse an item from the pool.
	*/
	ret = map->pool;
	map->pool = ret->next;
	memset(ret, 0, sizeof(struct xmlNsMapItem));
    } else {
	/*
	* Create a new item.
	*/
	ret = (xmlNsMapItemPtr) xmlMalloc(sizeof(struct xmlNsMapItem));
	if (ret == NULL)
	    return(NULL);
	memset(ret, 0, sizeof(struct xmlNsMapItem));
    }

    if (map->first == NULL) {
	/*
	* First ever.
	*/
	map->first = ret;
	map->last = ret;
    } else if (position == -1) {
	/*
	* Append.
	*/
	ret->prev = map->last;
	map->last->next = ret;
	map->last = ret;
    } else if (position == 0) {
	/*
	* Set on first position.
	*/
	map->first->prev = ret;
	ret->next = map->first;
	map->first = ret;
    }

    ret->oldNs = oldNs;
    ret->newNs = newNs;
    ret->shadowDepth = -1;
    ret->depth = depth;
    return (ret);
}

/**
 * Creates or reuses an xmlNs struct on doc->oldNs with
 * the given prefix and namespace name.
 *
 * @param doc  the doc
 * @param nsName  the namespace name
 * @param prefix  the prefix
 * @returns the acquired ns struct or NULL in case of an API
 *          or internal error.
 */
static xmlNsPtr
xmlDOMWrapStoreNs(xmlDocPtr doc,
		   const xmlChar *nsName,
		   const xmlChar *prefix)
{
    xmlNsPtr ns;

    if (doc == NULL)
	return (NULL);
    ns = xmlTreeEnsureXMLDecl(doc);
    if (ns == NULL)
	return (NULL);
    if (ns->next != NULL) {
	/* Reuse. */
	ns = ns->next;
	while (ns != NULL) {
	    if (((ns->prefix == prefix) ||
		xmlStrEqual(ns->prefix, prefix)) &&
		xmlStrEqual(ns->href, nsName)) {
		return (ns);
	    }
	    if (ns->next == NULL)
		break;
	    ns = ns->next;
	}
    }
    /* Create. */
    if (ns != NULL) {
        ns->next = xmlNewNs(NULL, nsName, prefix);
        return (ns->next);
    }
    return(NULL);
}

/**
 * Allocates and initializes a new DOM-wrapper context.
 *
 * @returns the xmlDOMWrapCtxt or NULL in case of an internal error.
 */
xmlDOMWrapCtxt *
xmlDOMWrapNewCtxt(void)
{
    xmlDOMWrapCtxtPtr ret;

    ret = xmlMalloc(sizeof(xmlDOMWrapCtxt));
    if (ret == NULL)
	return (NULL);
    memset(ret, 0, sizeof(xmlDOMWrapCtxt));
    return (ret);
}

/**
 * Frees the DOM-wrapper context.
 *
 * @param ctxt  the DOM-wrapper context
 */
void
xmlDOMWrapFreeCtxt(xmlDOMWrapCtxt *ctxt)
{
    if (ctxt == NULL)
	return;
    if (ctxt->namespaceMap != NULL)
	xmlDOMWrapNsMapFree((xmlNsMapPtr) ctxt->namespaceMap);
    /*
    * TODO: Store the namespace map in the context.
    */
    xmlFree(ctxt);
}

/**
 * Searches for a ns-decl with the given prefix in `nsList`.
 *
 * @param nsList  a list of ns-structs
 * @param prefix  the searched prefix
 * @returns the ns-decl if found, NULL if not found and on
 *          API errors.
 */
static xmlNsPtr
xmlTreeNSListLookupByPrefix(xmlNsPtr nsList, const xmlChar *prefix)
{
    if (nsList == NULL)
	return (NULL);
    {
	xmlNsPtr ns;
	ns = nsList;
	do {
	    if ((prefix == ns->prefix) ||
		xmlStrEqual(prefix, ns->prefix)) {
		return (ns);
	    }
	    ns = ns->next;
	} while (ns != NULL);
    }
    return (NULL);
}

/**
 * Puts in-scope namespaces into the ns-map.
 *
 * @param map  the namespace map
 * @param node  the node to start with
 * @returns 0 on success, -1 on API or internal errors.
 */
static int
xmlDOMWrapNSNormGatherInScopeNs(xmlNsMapPtr *map,
				xmlNodePtr node)
{
    xmlNodePtr cur;
    xmlNsPtr ns;
    xmlNsMapItemPtr mi;
    int shadowed;

    if ((map == NULL) || (*map != NULL))
	return (-1);
    if ((node == NULL) || (node->type == XML_NAMESPACE_DECL))
        return (-1);
    /*
    * Get in-scope ns-decls of @parent.
    */
    cur = node;
    while ((cur != NULL) && (cur != (xmlNodePtr) cur->doc)) {
	if (cur->type == XML_ELEMENT_NODE) {
	    if (cur->nsDef != NULL) {
		ns = cur->nsDef;
		do {
		    shadowed = 0;
		    if (XML_NSMAP_NOTEMPTY(*map)) {
			/*
			* Skip shadowed prefixes.
			*/
			XML_NSMAP_FOREACH(*map, mi) {
			    if ((ns->prefix == mi->newNs->prefix) ||
				xmlStrEqual(ns->prefix, mi->newNs->prefix)) {
				shadowed = 1;
				break;
			    }
			}
		    }
		    /*
		    * Insert mapping.
		    */
		    mi = xmlDOMWrapNsMapAddItem(map, 0, NULL,
			ns, XML_TREE_NSMAP_PARENT);
		    if (mi == NULL)
			return (-1);
		    if (shadowed)
			mi->shadowDepth = 0;
		    ns = ns->next;
		} while (ns != NULL);
	    }
	}
	cur = cur->parent;
    }
    return (0);
}

/*
 * For internal use. Adds a ns-decl mapping.
 *
 * Returns 0 on success, -1 on internal errors.
 */
static int
xmlDOMWrapNSNormAddNsMapItem2(xmlNsPtr **list, int *size, int *number,
			xmlNsPtr oldNs, xmlNsPtr newNs)
{
    if (*number >= *size) {
        xmlNsPtr *tmp;
        int newSize;

        newSize = xmlGrowCapacity(*size, 2 * sizeof(tmp[0]), 3, XML_MAX_ITEMS);
        if (newSize < 0)
            return(-1);
        tmp = xmlRealloc(*list, newSize * 2 * sizeof(tmp[0]));
        if (tmp == NULL)
            return(-1);
        *list = tmp;
        *size = newSize;
    }

    (*list)[2 * (*number)] = oldNs;
    (*list)[2 * (*number) +1] = newNs;
    (*number)++;
    return (0);
}

/**
 * Unlinks the given node from its owner.
 *
 * This will substitute ns-references to node->nsDef for
 * ns-references to doc->oldNs, thus ensuring the removed
 * branch to be autark wrt ns-references.
 *
 * NOTE: This function was not intensively tested.
 *
 * @param ctxt  a DOM wrapper context
 * @param doc  the doc
 * @param node  the node to be removed.
 * @param options  set of options, unused at the moment
 * @returns 0 on success, 1 if the node is not supported,
 *          -1 on API and internal errors.
 */
int
xmlDOMWrapRemoveNode(xmlDOMWrapCtxt *ctxt, xmlDoc *doc,
		     xmlNode *node, int options ATTRIBUTE_UNUSED)
{
    xmlNsPtr *list = NULL;
    int sizeList = 0, nbList = 0, ret = 0, i, j;
    xmlNsPtr ns;

    if ((node == NULL) || (doc == NULL) || (node->doc != doc))
	return (-1);

    /* TODO: 0 or -1 ? */
    if (node->parent == NULL)
	return (0);

    switch (node->type) {
	case XML_TEXT_NODE:
	case XML_CDATA_SECTION_NODE:
	case XML_ENTITY_REF_NODE:
	case XML_PI_NODE:
	case XML_COMMENT_NODE:
	    xmlUnlinkNodeInternal(node);
	    return (0);
	case XML_ELEMENT_NODE:
	case XML_ATTRIBUTE_NODE:
	    break;
	default:
	    return (1);
    }
    xmlUnlinkNodeInternal(node);
    /*
    * Save out-of-scope ns-references in doc->oldNs.
    */
    do {
	switch (node->type) {
	    case XML_ELEMENT_NODE:
		if ((ctxt == NULL) && (node->nsDef != NULL)) {
		    ns = node->nsDef;
		    do {
			if (xmlDOMWrapNSNormAddNsMapItem2(&list, &sizeList,
			    &nbList, ns, ns) == -1)
			    ret = -1;
			ns = ns->next;
		    } while (ns != NULL);
		}
                /* Falls through. */
	    case XML_ATTRIBUTE_NODE:
		if (node->ns != NULL) {
		    /*
		    * Find a mapping.
		    */
		    if (list != NULL) {
			for (i = 0, j = 0; i < nbList; i++, j += 2) {
			    if (node->ns == list[j]) {
				node->ns = list[++j];
				goto next_node;
			    }
			}
		    }
		    ns = NULL;
		    if (ctxt != NULL) {
			/*
			* User defined.
			*/
		    } else {
			/*
			* Add to doc's oldNs.
			*/
			ns = xmlDOMWrapStoreNs(doc, node->ns->href,
			    node->ns->prefix);
			if (ns == NULL)
			    ret = -1;
		    }
		    if (ns != NULL) {
			/*
			* Add mapping.
			*/
			if (xmlDOMWrapNSNormAddNsMapItem2(&list, &sizeList,
			    &nbList, node->ns, ns) == -1)
			    ret = -1;
		    }
		    node->ns = ns;
		}
		if ((node->type == XML_ELEMENT_NODE) &&
		    (node->properties != NULL)) {
		    node = (xmlNodePtr) node->properties;
		    continue;
		}
		break;
	    default:
		goto next_sibling;
	}
next_node:
	if ((node->type == XML_ELEMENT_NODE) &&
	    (node->children != NULL)) {
	    node = node->children;
	    continue;
	}
next_sibling:
	if (node == NULL)
	    break;
	if (node->next != NULL)
	    node = node->next;
	else {
            int type = node->type;

	    node = node->parent;
            if ((type == XML_ATTRIBUTE_NODE) &&
                (node != NULL) &&
                (node->children != NULL)) {
                node = node->children;
            } else {
	        goto next_sibling;
            }
	}
    } while (node != NULL);

    if (list != NULL)
	xmlFree(list);
    return (ret);
}

/**
 * Dynamically searches for a ns-declaration which matches
 * the given `nsName` in the ancestor-or-self axis of `node`.
 *
 * @param doc  the document
 * @param node  the start node
 * @param nsName  the searched namespace name
 * @param retNs  the resulting ns-decl
 * @param prefixed  if the found ns-decl must have a prefix
 *                  (for attributes)
 * @returns 1 if a ns-decl was found, 0 if not and -1 on API
 *          and internal errors.
 */
static int
xmlSearchNsByNamespaceStrict(xmlDocPtr doc, xmlNodePtr node,
			     const xmlChar* nsName,
			     xmlNsPtr *retNs, int prefixed)
{
    xmlNodePtr cur, prev = NULL, out = NULL;
    xmlNsPtr ns, prevns;

    if ((doc == NULL) || (nsName == NULL) || (retNs == NULL))
	return (-1);
    if ((node == NULL) || (node->type == XML_NAMESPACE_DECL))
        return(-1);

    *retNs = NULL;
    if (xmlStrEqual(nsName, XML_XML_NAMESPACE)) {
	*retNs = xmlTreeEnsureXMLDecl(doc);
	if (*retNs == NULL)
	    return (-1);
	return (1);
    }
    cur = node;
    do {
	if (cur->type == XML_ELEMENT_NODE) {
	    if (cur->nsDef != NULL) {
		for (ns = cur->nsDef; ns != NULL; ns = ns->next) {
		    if (prefixed && (ns->prefix == NULL))
			continue;
		    if (prev != NULL) {
			/*
			* Check the last level of ns-decls for a
			* shadowing prefix.
			*/
			prevns = prev->nsDef;
			do {
			    if ((prevns->prefix == ns->prefix) ||
				((prevns->prefix != NULL) &&
				(ns->prefix != NULL) &&
				xmlStrEqual(prevns->prefix, ns->prefix))) {
				/*
				* Shadowed.
				*/
				break;
			    }
			    prevns = prevns->next;
			} while (prevns != NULL);
			if (prevns != NULL)
			    continue;
		    }
		    /*
		    * Ns-name comparison.
		    */
		    if ((nsName == ns->href) ||
			xmlStrEqual(nsName, ns->href)) {
			/*
			* At this point the prefix can only be shadowed,
			* if we are the the (at least) 3rd level of
			* ns-decls.
			*/
			if (out) {
			    int ret;

			    ret = xmlNsInScope(doc, node, prev, ns->prefix);
			    if (ret < 0)
				return (-1);
			    /*
			    * TODO: Should we try to find a matching ns-name
			    * only once? This here keeps on searching.
			    * I think we should try further since, there might
			    * be an other matching ns-decl with an unshadowed
			    * prefix.
			    */
			    if (! ret)
				continue;
			}
			*retNs = ns;
			return (1);
		    }
		}
		out = prev;
		prev = cur;
	    }
	} else if (cur->type == XML_ENTITY_DECL)
	    return (0);
	cur = cur->parent;
    } while ((cur != NULL) && (cur->doc != (xmlDocPtr) cur));
    return (0);
}

/**
 * Dynamically searches for a ns-declaration which matches
 * the given `nsName` in the ancestor-or-self axis of `node`.
 *
 * @param doc  the document
 * @param node  the start node
 * @param prefix  the searched namespace prefix
 * @param retNs  the resulting ns-decl
 * @returns 1 if a ns-decl was found, 0 if not and -1 on API
 *          and internal errors.
 */
static int
xmlSearchNsByPrefixStrict(xmlDocPtr doc, xmlNodePtr node,
			  const xmlChar* prefix,
			  xmlNsPtr *retNs)
{
    xmlNodePtr cur;
    xmlNsPtr ns;

    if ((doc == NULL) || (node == NULL) || (node->type == XML_NAMESPACE_DECL))
        return(-1);

    if (retNs)
	*retNs = NULL;
    if (IS_STR_XML(prefix)) {
	if (retNs) {
	    *retNs = xmlTreeEnsureXMLDecl(doc);
	    if (*retNs == NULL)
		return (-1);
	}
	return (1);
    }
    cur = node;
    do {
	if (cur->type == XML_ELEMENT_NODE) {
	    if (cur->nsDef != NULL) {
		ns = cur->nsDef;
		do {
		    if ((prefix == ns->prefix) ||
			xmlStrEqual(prefix, ns->prefix))
		    {
			/*
			* Disabled namespaces, e.g. xmlns:abc="".
			*/
			if (ns->href == NULL)
			    return(0);
			if (retNs)
			    *retNs = ns;
			return (1);
		    }
		    ns = ns->next;
		} while (ns != NULL);
	    }
	} else if (cur->type == XML_ENTITY_DECL)
	    return (0);
	cur = cur->parent;
    } while ((cur != NULL) && (cur->doc != (xmlDocPtr) cur));
    return (0);
}

/**
 * Declares a new namespace on `elem`. It tries to use the
 * given `prefix`. If a ns-decl with the given prefix is already existent
 * on `elem`, it will generate an other prefix.
 *
 * @param doc  the doc
 * @param elem  the element-node to declare on
 * @param nsName  the namespace-name of the ns-decl
 * @param prefix  the preferred prefix of the ns-decl
 * @param checkShadow  ensure that the new ns-decl doesn't shadow
 *                     ancestor ns-decls
 * @returns 1 if a ns-decl was found, 0 if not and -1 on API
 *          and internal errors.
 */
static xmlNsPtr
xmlDOMWrapNSNormDeclareNsForced(xmlDocPtr doc,
				xmlNodePtr elem,
				const xmlChar *nsName,
				const xmlChar *prefix,
				int checkShadow)
{

    xmlNsPtr ret;
    char buf[50];
    const xmlChar *pref;
    int counter = 0;

    if ((doc == NULL) || (elem == NULL) || (elem->type != XML_ELEMENT_NODE))
        return(NULL);
    /*
    * Create a ns-decl on @anchor.
    */
    pref = prefix;
    while (1) {
	/*
	* Lookup whether the prefix is unused in elem's ns-decls.
	*/
	if ((elem->nsDef != NULL) &&
	    (xmlTreeNSListLookupByPrefix(elem->nsDef, pref) != NULL))
	    goto ns_next_prefix;
	if (checkShadow && elem->parent &&
	    ((xmlNodePtr) elem->parent->doc != elem->parent)) {
	    /*
	    * Does it shadow ancestor ns-decls?
	    */
	    if (xmlSearchNsByPrefixStrict(doc, elem->parent, pref, NULL) == 1)
		goto ns_next_prefix;
	}
	ret = xmlNewNs(NULL, nsName, pref);
	if (ret == NULL)
	    return (NULL);
	if (elem->nsDef == NULL)
	    elem->nsDef = ret;
	else {
	    xmlNsPtr ns2 = elem->nsDef;
	    while (ns2->next != NULL)
		ns2 = ns2->next;
	    ns2->next = ret;
	}
	return (ret);
ns_next_prefix:
	counter++;
	if (counter > 1000)
	    return (NULL);
	if (prefix == NULL) {
	    snprintf((char *) buf, sizeof(buf),
		"ns_%d", counter);
	} else
	    snprintf((char *) buf, sizeof(buf),
	    "%.30s_%d", (char *)prefix, counter);
	pref = BAD_CAST buf;
    }
}

/**
 * Searches for a matching ns-name in the ns-decls of `nsMap`, if not
 * found it will either declare it on `elem`, or store it in `doc->oldNs`.
 * If a new ns-decl needs to be declared on `elem`, it tries to use the
 * `ns->prefix` for it, if this prefix is already in use on `elem`, it will
 * change the prefix or the new ns-decl.
 *
 * @param doc  the doc
 * @param elem  the element-node to declare namespaces on
 * @param ns  the ns-struct to use for the search
 * @param retNs  the found/created ns-struct
 * @param nsMap  the ns-map
 * @param depth  the current tree depth
 * @param ancestorsOnly  search in ancestor ns-decls only
 * @param prefixed  if the searched ns-decl must have a prefix
 *                  (for attributes)
 * @returns 0 if succeeded, -1 otherwise and on API/internal errors.
 */
static int
xmlDOMWrapNSNormAcquireNormalizedNs(xmlDocPtr doc,
				   xmlNodePtr elem,
				   xmlNsPtr ns,
				   xmlNsPtr *retNs,
				   xmlNsMapPtr *nsMap,

				   int depth,
				   int ancestorsOnly,
				   int prefixed)
{
    xmlNsMapItemPtr mi;

    if ((doc == NULL) || (ns == NULL) || (retNs == NULL) ||
	(nsMap == NULL))
	return (-1);

    *retNs = NULL;
    /*
    * Handle XML namespace.
    */
    if (IS_STR_XML(ns->prefix)) {
	/*
	* Insert XML namespace mapping.
	*/
	*retNs = xmlTreeEnsureXMLDecl(doc);
	if (*retNs == NULL)
	    return (-1);
	return (0);
    }
    /*
    * If the search should be done in ancestors only and no
    * @elem (the first ancestor) was specified, then skip the search.
    */
    if ((XML_NSMAP_NOTEMPTY(*nsMap)) &&
	(! (ancestorsOnly && (elem == NULL))))
    {
	/*
	* Try to find an equal ns-name in in-scope ns-decls.
	*/
	XML_NSMAP_FOREACH(*nsMap, mi) {
	    if ((mi->depth >= XML_TREE_NSMAP_PARENT) &&
		/*
		* ancestorsOnly: This should be turned on to gain speed,
		* if one knows that the branch itself was already
		* ns-wellformed and no stale references existed.
		* I.e. it searches in the ancestor axis only.
		*/
		((! ancestorsOnly) || (mi->depth == XML_TREE_NSMAP_PARENT)) &&
		/* Skip shadowed prefixes. */
		(mi->shadowDepth == -1) &&
		/* Skip xmlns="" or xmlns:foo="". */
		((mi->newNs->href != NULL) &&
		(mi->newNs->href[0] != 0)) &&
		/* Ensure a prefix if wanted. */
		((! prefixed) || (mi->newNs->prefix != NULL)) &&
		/* Equal ns name */
		((mi->newNs->href == ns->href) ||
		xmlStrEqual(mi->newNs->href, ns->href))) {
		/* Set the mapping. */
		mi->oldNs = ns;
		*retNs = mi->newNs;
		return (0);
	    }
	}
    }
    /*
    * No luck, the namespace is out of scope or shadowed.
    */
    if (elem == NULL) {
	xmlNsPtr tmpns;

	/*
	* Store ns-decls in "oldNs" of the document-node.
	*/
	tmpns = xmlDOMWrapStoreNs(doc, ns->href, ns->prefix);
	if (tmpns == NULL)
	    return (-1);
	/*
	* Insert mapping.
	*/
	if (xmlDOMWrapNsMapAddItem(nsMap, -1, ns,
		tmpns, XML_TREE_NSMAP_DOC) == NULL) {
	    return (-1);
	}
	*retNs = tmpns;
    } else {
	xmlNsPtr tmpns;

	tmpns = xmlDOMWrapNSNormDeclareNsForced(doc, elem, ns->href,
	    ns->prefix, 0);
	if (tmpns == NULL)
	    return (-1);

	if (*nsMap != NULL) {
	    /*
	    * Does it shadow ancestor ns-decls?
	    */
	    XML_NSMAP_FOREACH(*nsMap, mi) {
		if ((mi->depth < depth) &&
		    (mi->shadowDepth == -1) &&
		    ((ns->prefix == mi->newNs->prefix) ||
		    xmlStrEqual(ns->prefix, mi->newNs->prefix))) {
		    /*
		    * Shadows.
		    */
		    mi->shadowDepth = depth;
		    break;
		}
	    }
	}
	if (xmlDOMWrapNsMapAddItem(nsMap, -1, ns, tmpns, depth) == NULL) {
	    return (-1);
	}
	*retNs = tmpns;
    }
    return (0);
}

typedef enum {
    XML_DOM_RECONNS_REMOVEREDUND = 1<<0
} xmlDOMReconcileNSOptions;

/**
 * Fix up namespaces.
 *
 * Ensures that ns-references point to ns-decls hold on element-nodes.
 * Ensures that the tree is namespace wellformed by creating additional
 * ns-decls where needed. Note that, since prefixes of already existent
 * ns-decls can be shadowed by this process, it could break QNames in
 * attribute values or element content.
 *
 * NOTE: This function was not intensively tested.
 *
 * @param ctxt  DOM wrapper context, unused at the moment
 * @param elem  the element-node
 * @param options  option flags
 * @returns 0 if succeeded, -1 otherwise and on API/internal errors.
 */
int
xmlDOMWrapReconcileNamespaces(xmlDOMWrapCtxt *ctxt ATTRIBUTE_UNUSED,
			      xmlNode *elem,
			      int options)
{
    int depth = -1, adoptns = 0, parnsdone = 0;
    xmlNsPtr ns, prevns;
    xmlDocPtr doc;
    xmlNodePtr cur, curElem = NULL;
    xmlNsMapPtr nsMap = NULL;
    xmlNsMapItemPtr /* topmi = NULL, */ mi;
    /* @ancestorsOnly should be set by an option flag. */
    int ancestorsOnly = 0;
    int optRemoveRedundantNS =
	((xmlDOMReconcileNSOptions) options & XML_DOM_RECONNS_REMOVEREDUND) ? 1 : 0;
    xmlNsPtr *listRedund = NULL;
    int sizeRedund = 0, nbRedund = 0, ret = 0, i, j;

    if ((elem == NULL) || (elem->doc == NULL) ||
	(elem->type != XML_ELEMENT_NODE))
	return (-1);

    doc = elem->doc;
    cur = elem;
    do {
	switch (cur->type) {
	    case XML_ELEMENT_NODE:
		adoptns = 1;
		curElem = cur;
		depth++;
		/*
		* Namespace declarations.
		*/
		if (cur->nsDef != NULL) {
		    prevns = NULL;
		    ns = cur->nsDef;
		    while (ns != NULL) {
			if (! parnsdone) {
			    if ((elem->parent) &&
				((xmlNodePtr) elem->parent->doc != elem->parent)) {
				/*
				* Gather ancestor in-scope ns-decls.
				*/
				if (xmlDOMWrapNSNormGatherInScopeNs(&nsMap,
				    elem->parent) == -1)
				    ret = -1;
			    }
			    parnsdone = 1;
			}

			/*
			* Lookup the ns ancestor-axis for equal ns-decls in scope.
			*/
			if (optRemoveRedundantNS && XML_NSMAP_NOTEMPTY(nsMap)) {
			    XML_NSMAP_FOREACH(nsMap, mi) {
				if ((mi->depth >= XML_TREE_NSMAP_PARENT) &&
				    (mi->shadowDepth == -1) &&
				    ((ns->prefix == mi->newNs->prefix) ||
				      xmlStrEqual(ns->prefix, mi->newNs->prefix)) &&
				    ((ns->href == mi->newNs->href) ||
				      xmlStrEqual(ns->href, mi->newNs->href)))
				{
				    /*
				    * A redundant ns-decl was found.
				    * Add it to the list of redundant ns-decls.
				    */
				    if (xmlDOMWrapNSNormAddNsMapItem2(&listRedund,
					&sizeRedund, &nbRedund, ns, mi->newNs) == -1) {
					ret = -1;
                                    } else {
                                        /*
                                        * Remove the ns-decl from the element-node.
                                        */
                                        if (prevns)
                                            prevns->next = ns->next;
                                        else
                                            cur->nsDef = ns->next;
                                        goto next_ns_decl;
                                    }
				}
			    }
			}

			/*
			* Skip ns-references handling if the referenced
			* ns-decl is declared on the same element.
			*/
			if ((cur->ns != NULL) && adoptns && (cur->ns == ns))
			    adoptns = 0;
			/*
			* Does it shadow any ns-decl?
			*/
			if (XML_NSMAP_NOTEMPTY(nsMap)) {
			    XML_NSMAP_FOREACH(nsMap, mi) {
				if ((mi->depth >= XML_TREE_NSMAP_PARENT) &&
				    (mi->shadowDepth == -1) &&
				    ((ns->prefix == mi->newNs->prefix) ||
				    xmlStrEqual(ns->prefix, mi->newNs->prefix))) {

				    mi->shadowDepth = depth;
				}
			    }
			}
			/*
			* Push mapping.
			*/
			if (xmlDOMWrapNsMapAddItem(&nsMap, -1, ns, ns,
			    depth) == NULL)
			    ret = -1;

			prevns = ns;
next_ns_decl:
			ns = ns->next;
		    }
		}
		if (! adoptns)
		    goto ns_end;
                /* Falls through. */
	    case XML_ATTRIBUTE_NODE:
		/* No ns, no fun. */
		if (cur->ns == NULL)
		    goto ns_end;

		if (! parnsdone) {
		    if ((elem->parent) &&
			((xmlNodePtr) elem->parent->doc != elem->parent)) {
			if (xmlDOMWrapNSNormGatherInScopeNs(&nsMap,
				elem->parent) == -1)
			    ret = -1;
		    }
		    parnsdone = 1;
		}
		/*
		* Adjust the reference if this was a redundant ns-decl.
		*/
		if (listRedund) {
		   for (i = 0, j = 0; i < nbRedund; i++, j += 2) {
		       if (cur->ns == listRedund[j]) {
			   cur->ns = listRedund[++j];
			   break;
		       }
		   }
		}
		/*
		* Adopt ns-references.
		*/
		if (XML_NSMAP_NOTEMPTY(nsMap)) {
		    /*
		    * Search for a mapping.
		    */
		    XML_NSMAP_FOREACH(nsMap, mi) {
			if ((mi->shadowDepth == -1) &&
			    (cur->ns == mi->oldNs)) {

			    cur->ns = mi->newNs;
			    goto ns_end;
			}
		    }
		}
		/*
		* Acquire a normalized ns-decl and add it to the map.
		*/
		if (xmlDOMWrapNSNormAcquireNormalizedNs(doc, curElem,
			cur->ns, &ns,
			&nsMap, depth,
			ancestorsOnly,
			(cur->type == XML_ATTRIBUTE_NODE) ? 1 : 0) == -1)
		    ret = -1;
		cur->ns = ns;

ns_end:
		if ((cur->type == XML_ELEMENT_NODE) &&
		    (cur->properties != NULL)) {
		    /*
		    * Process attributes.
		    */
		    cur = (xmlNodePtr) cur->properties;
		    continue;
		}
		break;
	    default:
		goto next_sibling;
	}
into_content:
	if ((cur->type == XML_ELEMENT_NODE) &&
	    (cur->children != NULL)) {
	    /*
	    * Process content of element-nodes only.
	    */
	    cur = cur->children;
	    continue;
	}
next_sibling:
	if (cur == elem)
	    break;
	if (cur->type == XML_ELEMENT_NODE) {
	    if (XML_NSMAP_NOTEMPTY(nsMap)) {
		/*
		* Pop mappings.
		*/
		while ((nsMap->last != NULL) &&
		    (nsMap->last->depth >= depth))
		{
		    XML_NSMAP_POP(nsMap, mi)
		}
		/*
		* Unshadow.
		*/
		XML_NSMAP_FOREACH(nsMap, mi) {
		    if (mi->shadowDepth >= depth)
			mi->shadowDepth = -1;
		}
	    }
	    depth--;
	}
	if (cur->next != NULL)
	    cur = cur->next;
	else {
	    if (cur->type == XML_ATTRIBUTE_NODE) {
		cur = cur->parent;
		goto into_content;
	    }
	    cur = cur->parent;
	    goto next_sibling;
	}
    } while (cur != NULL);

    if (listRedund) {
	for (i = 0, j = 0; i < nbRedund; i++, j += 2) {
	    xmlFreeNs(listRedund[j]);
	}
	xmlFree(listRedund);
    }
    if (nsMap != NULL)
	xmlDOMWrapNsMapFree(nsMap);
    return (ret);
}

/**
 * Ensures that ns-references point to `destDoc`: either to
 * `elements->nsDef` entries if `destParent` is given, or to
 * `destDoc->oldNs` otherwise.
 *
 * If `destParent` is given, it ensures that the tree is namespace
 * wellformed by creating additional ns-decls where needed.
 * Note that, since prefixes of already existent ns-decls can be
 * shadowed by this process, it could break QNames in attribute
 * values or element content.
 *
 * NOTE: This function was not intensively tested.
 *
 * @param ctxt  the optional context for custom processing
 * @param sourceDoc  the optional sourceDoc
 * @param node  the element-node to start with
 * @param destDoc  the destination doc for adoption
 * @param destParent  the optional new parent of `node` in `destDoc`
 * @param options  option flags
 * @returns 0 if succeeded, -1 otherwise and on API/internal errors.
 */
static int
xmlDOMWrapAdoptBranch(xmlDOMWrapCtxtPtr ctxt,
		      xmlDocPtr sourceDoc ATTRIBUTE_UNUSED,
		      xmlNodePtr node,
		      xmlDocPtr destDoc,
		      xmlNodePtr destParent,
		      int options ATTRIBUTE_UNUSED)
{
    int ret = 0;
    xmlNodePtr cur, curElem = NULL;
    xmlNsMapPtr nsMap = NULL;
    xmlNsMapItemPtr mi;
    xmlNsPtr ns = NULL;
    int depth = -1;
    /* gather @parent's ns-decls. */
    int parnsdone;
    /* @ancestorsOnly should be set per option. */
    int ancestorsOnly = 0;

    /*
    * Get the ns-map from the context if available.
    */
    if (ctxt)
	nsMap = (xmlNsMapPtr) ctxt->namespaceMap;
    /*
    * Disable search for ns-decls in the parent-axis of the
    * destination element, if:
    * 1) there's no destination parent
    * 2) custom ns-reference handling is used
    */
    if ((destParent == NULL) ||
	(ctxt && ctxt->getNsForNodeFunc))
    {
	parnsdone = 1;
    } else
	parnsdone = 0;

    cur = node;

    while (cur != NULL) {
        if (cur->doc != destDoc) {
            if (xmlNodeSetDoc(cur, destDoc) < 0)
                ret = -1;
        }

	switch (cur->type) {
	    case XML_XINCLUDE_START:
	    case XML_XINCLUDE_END:
		/*
		* TODO
		*/
		ret = -1;
                goto leave_node;
	    case XML_ELEMENT_NODE:
		curElem = cur;
		depth++;
		/*
		* Namespace declarations.
		* - ns->href and ns->prefix are never in the dict, so
		*   we need not move the values over to the destination dict.
		* - Note that for custom handling of ns-references,
		*   the ns-decls need not be stored in the ns-map,
		*   since they won't be referenced by node->ns.
		*/
		if ((cur->nsDef) &&
		    ((ctxt == NULL) || (ctxt->getNsForNodeFunc == NULL)))
		{
		    if (! parnsdone) {
			/*
			* Gather @parent's in-scope ns-decls.
			*/
			if (xmlDOMWrapNSNormGatherInScopeNs(&nsMap,
			    destParent) == -1)
			    ret = -1;
			parnsdone = 1;
		    }
		    for (ns = cur->nsDef; ns != NULL; ns = ns->next) {
			/*
			* NOTE: ns->prefix and ns->href are never in the dict.
			*/
			/*
			* Does it shadow any ns-decl?
			*/
			if (XML_NSMAP_NOTEMPTY(nsMap)) {
			    XML_NSMAP_FOREACH(nsMap, mi) {
				if ((mi->depth >= XML_TREE_NSMAP_PARENT) &&
				    (mi->shadowDepth == -1) &&
				    ((ns->prefix == mi->newNs->prefix) ||
				    xmlStrEqual(ns->prefix,
				    mi->newNs->prefix))) {

				    mi->shadowDepth = depth;
				}
			    }
			}
			/*
			* Push mapping.
			*/
			if (xmlDOMWrapNsMapAddItem(&nsMap, -1,
			    ns, ns, depth) == NULL)
			    ret = -1;
		    }
		}
                /* Falls through. */
	    case XML_ATTRIBUTE_NODE:
		/* No namespace, no fun. */
		if (cur->ns == NULL)
		    goto ns_end;

		if (! parnsdone) {
		    if (xmlDOMWrapNSNormGatherInScopeNs(&nsMap,
			destParent) == -1)
			ret = -1;
		    parnsdone = 1;
		}
		/*
		* Adopt ns-references.
		*/
		if (XML_NSMAP_NOTEMPTY(nsMap)) {
		    /*
		    * Search for a mapping.
		    */
		    XML_NSMAP_FOREACH(nsMap, mi) {
			if ((mi->shadowDepth == -1) &&
			    (cur->ns == mi->oldNs)) {

			    cur->ns = mi->newNs;
			    goto ns_end;
			}
		    }
		}
		/*
		* No matching namespace in scope. We need a new one.
		*/
		if ((ctxt) && (ctxt->getNsForNodeFunc)) {
		    /*
		    * User-defined behaviour.
		    */
		    ns = ctxt->getNsForNodeFunc(ctxt, cur,
			cur->ns->href, cur->ns->prefix);
		    /*
		    * Insert mapping if ns is available; it's the users fault
		    * if not.
		    */
		    if (xmlDOMWrapNsMapAddItem(&nsMap, -1,
			    cur->ns, ns, XML_TREE_NSMAP_CUSTOM) == NULL)
			ret = -1;
		    cur->ns = ns;
		} else {
		    /*
		    * Acquire a normalized ns-decl and add it to the map.
		    */
		    if (xmlDOMWrapNSNormAcquireNormalizedNs(destDoc,
			/* ns-decls on curElem or on destDoc->oldNs */
			destParent ? curElem : NULL,
			cur->ns, &ns,
			&nsMap, depth,
			ancestorsOnly,
			/* ns-decls must be prefixed for attributes. */
			(cur->type == XML_ATTRIBUTE_NODE) ? 1 : 0) == -1)
			ret = -1;
		    cur->ns = ns;
		}

ns_end:
		if (cur->type == XML_ELEMENT_NODE) {
		    cur->psvi = NULL;
		    cur->line = 0;
		    cur->extra = 0;
		    /*
		    * Walk attributes.
		    */
		    if (cur->properties != NULL) {
			/*
			* Process first attribute node.
			*/
			cur = (xmlNodePtr) cur->properties;
			continue;
		    }
		}
		break;
	    case XML_TEXT_NODE:
	    case XML_CDATA_SECTION_NODE:
	    case XML_PI_NODE:
	    case XML_COMMENT_NODE:
	    case XML_ENTITY_REF_NODE:
		goto leave_node;
	    default:
		ret = -1;
	}
	/*
	* Walk the tree.
	*/
	if (cur->children != NULL) {
	    cur = cur->children;
	    continue;
	}

leave_node:
	if (cur == node)
	    break;
	if ((cur->type == XML_ELEMENT_NODE) ||
	    (cur->type == XML_XINCLUDE_START) ||
	    (cur->type == XML_XINCLUDE_END))
	{
	    /*
	    * TODO: Do we expect nsDefs on XML_XINCLUDE_START?
	    */
	    if (XML_NSMAP_NOTEMPTY(nsMap)) {
		/*
		* Pop mappings.
		*/
		while ((nsMap->last != NULL) &&
		    (nsMap->last->depth >= depth))
		{
		    XML_NSMAP_POP(nsMap, mi)
		}
		/*
		* Unshadow.
		*/
		XML_NSMAP_FOREACH(nsMap, mi) {
		    if (mi->shadowDepth >= depth)
			mi->shadowDepth = -1;
		}
	    }
	    depth--;
	}
	if (cur->next != NULL)
	    cur = cur->next;
	else if ((cur->type == XML_ATTRIBUTE_NODE) &&
	    (cur->parent->children != NULL))
	{
	    cur = cur->parent->children;
	} else {
	    cur = cur->parent;
	    goto leave_node;
	}
    }

    /*
    * Cleanup.
    */
    if (nsMap != NULL) {
	if ((ctxt) && (ctxt->namespaceMap == nsMap)) {
	    /*
	    * Just cleanup the map but don't free.
	    */
	    if (nsMap->first) {
		if (nsMap->pool)
		    nsMap->last->next = nsMap->pool;
		nsMap->pool = nsMap->first;
		nsMap->first = NULL;
	    }
	} else
	    xmlDOMWrapNsMapFree(nsMap);
    }
    return(ret);
}

/**
 * Clone a node and fix namespaces.
 *
 * References of out-of scope ns-decls are remapped to point to `destDoc`.
 * If `destParent` is given, then nsDef entries on element-nodes are used.
 * If *no* `destParent` is given, then `destDoc->oldNs` entries are used.
 * This is the case when you don't know already where the cloned branch
 * will be added to.
 *
 * If `destParent` is given, it ensures that the tree is namespace
 * wellformed by creating additional ns-decls where needed.
 * Note that, since prefixes of already existent ns-decls can be
 * shadowed by this process, it could break QNames in attribute
 * values or element content.
 *
 * @param ctxt  the optional context for custom processing
 * @param sourceDoc  the optional sourceDoc
 * @param node  the node to start with
 * @param resNode  the clone of the given `node`
 * @param destDoc  the destination doc
 * @param destParent  the optional new parent of `node` in `destDoc`
 * @param deep  descend into child if set
 * @param options  option flags
 * @returns 0 if the operation succeeded,
 *          1 if a node of unsupported (or not yet supported) type was given,
 *          -1 on API/internal errors.
 */
int
xmlDOMWrapCloneNode(xmlDOMWrapCtxt *ctxt,
		      xmlDoc *sourceDoc,
		      xmlNode *node,
		      xmlNode **resNode,
		      xmlDoc *destDoc,
		      xmlNode *destParent,
		      int deep,
		      int options ATTRIBUTE_UNUSED)
{
    int ret = 0;
    xmlNodePtr cur, cloneElem = NULL;
    xmlNsMapPtr nsMap = NULL;
    xmlNsMapItemPtr mi;
    xmlNsPtr ns;
    int depth = -1;
    /* int adoptStr = 1; */
    /* gather @parent's ns-decls. */
    int parnsdone = 0;
    /*
    * @ancestorsOnly:
    * TODO: @ancestorsOnly should be set per option.
    *
    */
    int ancestorsOnly = 0;
    xmlNodePtr resultClone = NULL, clone = NULL, parentClone = NULL, prevClone = NULL;
    xmlNsPtr cloneNs = NULL, *cloneNsDefSlot = NULL;
    xmlDictPtr dict; /* The destination dict */

    if ((node == NULL) || (resNode == NULL) || (destDoc == NULL) ||
	((destParent != NULL) && (destParent->doc != destDoc)))
	return(-1);
    /*
    * TODO: Initially we support only element-nodes.
    */
    if (node->type != XML_ELEMENT_NODE)
	return(1);
    /*
    * Check node->doc sanity.
    */
    if ((node->doc != NULL) && (sourceDoc != NULL) &&
	(node->doc != sourceDoc)) {
	/*
	* Might be an XIncluded node.
	*/
	return (-1);
    }
    if (sourceDoc == NULL)
	sourceDoc = node->doc;
    if (sourceDoc == NULL)
        return (-1);

    dict = destDoc->dict;
    /*
    * Reuse the namespace map of the context.
    */
    if (ctxt)
	nsMap = (xmlNsMapPtr) ctxt->namespaceMap;

    *resNode = NULL;

    cur = node;
    while (cur != NULL) {
	if (cur->doc != sourceDoc) {
	    /*
	    * We'll assume XIncluded nodes if the doc differs.
	    * TODO: Do we need to reconciliate XIncluded nodes?
	    * TODO: This here returns -1 in this case.
	    */
	    goto internal_error;
	}
	/*
	* Create a new node.
	*/
	switch (cur->type) {
	    case XML_XINCLUDE_START:
	    case XML_XINCLUDE_END:
		/*
		* TODO: What to do with XInclude?
		*/
		goto internal_error;
		break;
	    case XML_ELEMENT_NODE:
	    case XML_TEXT_NODE:
	    case XML_CDATA_SECTION_NODE:
	    case XML_COMMENT_NODE:
	    case XML_PI_NODE:
	    case XML_DOCUMENT_FRAG_NODE:
	    case XML_ENTITY_REF_NODE:
		/*
		* Nodes of xmlNode structure.
		*/
		clone = (xmlNodePtr) xmlMalloc(sizeof(xmlNode));
		if (clone == NULL)
		    goto internal_error;
		memset(clone, 0, sizeof(xmlNode));
		/*
		* Set hierarchical links.
		*/
		if (resultClone != NULL) {
		    clone->parent = parentClone;
		    if (prevClone) {
			prevClone->next = clone;
			clone->prev = prevClone;
		    } else
			parentClone->children = clone;
                    parentClone->last = clone;
		} else
		    resultClone = clone;

		break;
	    case XML_ATTRIBUTE_NODE:
		/*
		* Attributes (xmlAttr).
		*/
		clone = xmlMalloc(sizeof(xmlAttr));
		if (clone == NULL)
		    goto internal_error;
		memset(clone, 0, sizeof(xmlAttr));
		/*
		* Set hierarchical links.
		* TODO: Change this to add to the end of attributes.
		*/
		if (resultClone != NULL) {
		    clone->parent = parentClone;
		    if (prevClone) {
			prevClone->next = clone;
			clone->prev = prevClone;
		    } else
			parentClone->properties = (xmlAttrPtr) clone;
		} else
		    resultClone = clone;
		break;
	    default:
		/*
		* TODO QUESTION: Any other nodes expected?
		*/
		goto internal_error;
	}

	clone->type = cur->type;
	clone->doc = destDoc;

	/*
	* Clone the name of the node if any.
	*/
	if (cur->name == xmlStringText)
	    clone->name = xmlStringText;
	else if (cur->name == xmlStringTextNoenc)
	    /*
	    * NOTE: Although xmlStringTextNoenc is never assigned to a node
	    *   in tree.c, it might be set in Libxslt via
	    *   "xsl:disable-output-escaping".
	    */
	    clone->name = xmlStringTextNoenc;
	else if (cur->name == xmlStringComment)
	    clone->name = xmlStringComment;
	else if (cur->name != NULL) {
            if (dict != NULL)
                clone->name = xmlDictLookup(dict, cur->name, -1);
            else
                clone->name = xmlStrdup(cur->name);
            if (clone->name == NULL)
                goto internal_error;
	}

	switch (cur->type) {
	    case XML_XINCLUDE_START:
	    case XML_XINCLUDE_END:
		/*
		* TODO
		*/
		return (-1);
	    case XML_ELEMENT_NODE:
		cloneElem = clone;
		depth++;
		/*
		* Namespace declarations.
		*/
		if (cur->nsDef != NULL) {
		    if (! parnsdone) {
			if (destParent && (ctxt == NULL)) {
			    /*
			    * Gather @parent's in-scope ns-decls.
			    */
			    if (xmlDOMWrapNSNormGatherInScopeNs(&nsMap,
				destParent) == -1)
				goto internal_error;
			}
			parnsdone = 1;
		    }
		    /*
		    * Clone namespace declarations.
		    */
		    cloneNsDefSlot = &(clone->nsDef);
		    for (ns = cur->nsDef; ns != NULL; ns = ns->next) {
			/*
			* Create a new xmlNs.
			*/
			cloneNs = (xmlNsPtr) xmlMalloc(sizeof(xmlNs));
			if (cloneNs == NULL)
			    goto internal_error;
			memset(cloneNs, 0, sizeof(xmlNs));
			cloneNs->type = XML_LOCAL_NAMESPACE;

			if (ns->href != NULL) {
			    cloneNs->href = xmlStrdup(ns->href);
                            if (cloneNs->href == NULL) {
                                xmlFreeNs(cloneNs);
                                goto internal_error;
                            }
                        }
			if (ns->prefix != NULL) {
			    cloneNs->prefix = xmlStrdup(ns->prefix);
                            if (cloneNs->prefix == NULL) {
                                xmlFreeNs(cloneNs);
                                goto internal_error;
                            }
                        }

			*cloneNsDefSlot = cloneNs;
			cloneNsDefSlot = &(cloneNs->next);

			/*
			* Note that for custom handling of ns-references,
			* the ns-decls need not be stored in the ns-map,
			* since they won't be referenced by node->ns.
			*/
			if ((ctxt == NULL) ||
			    (ctxt->getNsForNodeFunc == NULL))
			{
			    /*
			    * Does it shadow any ns-decl?
			    */
			    if (XML_NSMAP_NOTEMPTY(nsMap)) {
				XML_NSMAP_FOREACH(nsMap, mi) {
				    if ((mi->depth >= XML_TREE_NSMAP_PARENT) &&
					(mi->shadowDepth == -1) &&
					((ns->prefix == mi->newNs->prefix) ||
					xmlStrEqual(ns->prefix,
					mi->newNs->prefix))) {
					/*
					* Mark as shadowed at the current
					* depth.
					*/
					mi->shadowDepth = depth;
				    }
				}
			    }
			    /*
			    * Push mapping.
			    */
			    if (xmlDOMWrapNsMapAddItem(&nsMap, -1,
				ns, cloneNs, depth) == NULL)
				goto internal_error;
			}
		    }
		}
		/* cur->ns will be processed further down. */
		break;
	    case XML_ATTRIBUTE_NODE:
		/* IDs will be processed further down. */
		/* cur->ns will be processed further down. */
		break;
	    case XML_PI_NODE:
	    case XML_COMMENT_NODE:
	    case XML_TEXT_NODE:
	    case XML_CDATA_SECTION_NODE:
		/*
		* Note that this will also cover the values of attributes.
		*/
                if (cur->content != NULL) {
                    clone->content = xmlStrdup(cur->content);
                    if (clone->content == NULL)
                        goto internal_error;
                }
		goto leave_node;
	    case XML_ENTITY_REF_NODE:
		if (sourceDoc != destDoc) {
		    if ((destDoc->intSubset) || (destDoc->extSubset)) {
			xmlEntityPtr ent;
			/*
			* Different doc: Assign new entity-node if available.
			*/
			ent = xmlGetDocEntity(destDoc, cur->name);
			if (ent != NULL) {
			    clone->content = ent->content;
			    clone->children = (xmlNodePtr) ent;
			    clone->last = (xmlNodePtr) ent;
			}
		    }
		} else {
		    /*
		    * Same doc: Use the current node's entity declaration
		    * and value.
		    */
		    clone->content = cur->content;
		    clone->children = cur->children;
		    clone->last = cur->last;
		}
		goto leave_node;
	    default:
		goto internal_error;
	}

	if (cur->ns == NULL)
	    goto end_ns_reference;

/* handle_ns_reference: */
	/*
	** The following will take care of references to ns-decls ********
	** and is intended only for element- and attribute-nodes.
	**
	*/
	if (! parnsdone) {
	    if (destParent && (ctxt == NULL)) {
		if (xmlDOMWrapNSNormGatherInScopeNs(&nsMap, destParent) == -1)
		    goto internal_error;
	    }
	    parnsdone = 1;
	}
	/*
	* Adopt ns-references.
	*/
	if (XML_NSMAP_NOTEMPTY(nsMap)) {
	    /*
	    * Search for a mapping.
	    */
	    XML_NSMAP_FOREACH(nsMap, mi) {
		if ((mi->shadowDepth == -1) &&
		    (cur->ns == mi->oldNs)) {
		    /*
		    * This is the nice case: a mapping was found.
		    */
		    clone->ns = mi->newNs;
		    goto end_ns_reference;
		}
	    }
	}
	/*
	* No matching namespace in scope. We need a new one.
	*/
	if ((ctxt != NULL) && (ctxt->getNsForNodeFunc != NULL)) {
	    /*
	    * User-defined behaviour.
	    */
	    ns = ctxt->getNsForNodeFunc(ctxt, cur,
		cur->ns->href, cur->ns->prefix);
	    /*
	    * Add user's mapping.
	    */
	    if (xmlDOMWrapNsMapAddItem(&nsMap, -1,
		cur->ns, ns, XML_TREE_NSMAP_CUSTOM) == NULL)
		goto internal_error;
	    clone->ns = ns;
	} else {
	    /*
	    * Acquire a normalized ns-decl and add it to the map.
	    */
	    if (xmlDOMWrapNSNormAcquireNormalizedNs(destDoc,
		/* ns-decls on cloneElem or on destDoc->oldNs */
		destParent ? cloneElem : NULL,
		cur->ns, &ns,
		&nsMap, depth,
		/* if we need to search only in the ancestor-axis */
		ancestorsOnly,
		/* ns-decls must be prefixed for attributes. */
		(cur->type == XML_ATTRIBUTE_NODE) ? 1 : 0) == -1)
		goto internal_error;
	    clone->ns = ns;
	}

end_ns_reference:

	/*
	* Some post-processing.
	*
	* Handle ID attributes.
	*/
	if ((clone->type == XML_ATTRIBUTE_NODE) &&
	    (clone->parent != NULL))
	{
            int res;

	    res = xmlIsID(destDoc, clone->parent, (xmlAttrPtr) clone);
            if (res < 0)
                goto internal_error;
            if (res == 1) {
		xmlChar *idVal;

		idVal = xmlNodeGetContent(cur);
                if (idVal == NULL)
                    goto internal_error;
                if (xmlAddIDSafe((xmlAttrPtr) cur, idVal) < 0) {
                    xmlFree(idVal);
                    goto internal_error;
                }
                xmlFree(idVal);
	    }
	}
	/*
	**
	** The following will traverse the tree **************************
	**
	*
	* Walk the element's attributes before descending into child-nodes.
	*/
	if ((cur->type == XML_ELEMENT_NODE) && (cur->properties != NULL)) {
	    prevClone = NULL;
	    parentClone = clone;
	    cur = (xmlNodePtr) cur->properties;
	    continue;
	}
into_content:
	/*
	* Descend into child-nodes.
	*/
	if (cur->children != NULL) {
	    if (deep || (cur->type == XML_ATTRIBUTE_NODE)) {
		prevClone = NULL;
		parentClone = clone;
		cur = cur->children;
		continue;
	    }
	}

leave_node:
	/*
	* At this point we are done with the node, its content
	* and an element-nodes's attribute-nodes.
	*/
	if (cur == node)
	    break;
	if ((cur->type == XML_ELEMENT_NODE) ||
	    (cur->type == XML_XINCLUDE_START) ||
	    (cur->type == XML_XINCLUDE_END)) {
	    /*
	    * TODO: Do we expect nsDefs on XML_XINCLUDE_START?
	    */
	    if (XML_NSMAP_NOTEMPTY(nsMap)) {
		/*
		* Pop mappings.
		*/
		while ((nsMap->last != NULL) &&
		    (nsMap->last->depth >= depth))
		{
		    XML_NSMAP_POP(nsMap, mi)
		}
		/*
		* Unshadow.
		*/
		XML_NSMAP_FOREACH(nsMap, mi) {
		    if (mi->shadowDepth >= depth)
			mi->shadowDepth = -1;
		}
	    }
	    depth--;
	}
	if (cur->next != NULL) {
	    prevClone = clone;
	    cur = cur->next;
	} else if (cur->type != XML_ATTRIBUTE_NODE) {
	    clone = clone->parent;
	    if (clone != NULL)
		parentClone = clone->parent;
	    /*
	    * Process parent --> next;
	    */
	    cur = cur->parent;
	    goto leave_node;
	} else {
	    /* This is for attributes only. */
	    clone = clone->parent;
	    parentClone = clone->parent;
	    /*
	    * Process parent-element --> children.
	    */
	    cur = cur->parent;
	    goto into_content;
	}
    }
    goto exit;

internal_error:
    ret = -1;

exit:
    /*
    * Cleanup.
    */
    if (nsMap != NULL) {
	if ((ctxt) && (ctxt->namespaceMap == nsMap)) {
	    /*
	    * Just cleanup the map but don't free.
	    */
	    if (nsMap->first) {
		if (nsMap->pool)
		    nsMap->last->next = nsMap->pool;
		nsMap->pool = nsMap->first;
		nsMap->first = NULL;
	    }
	} else
	    xmlDOMWrapNsMapFree(nsMap);
    }
    /*
    * TODO: Should we try a cleanup of the cloned node in case of a
    * fatal error?
    */
    *resNode = resultClone;
    return (ret);
}

/**
 * `attr` is adopted by `destDoc`.
 * Ensures that ns-references point to `destDoc`: either to
 * `elements->nsDef` entries if `destParent` is given, or to
 * `destDoc->oldNs` otherwise.
 *
 * @param ctxt  the optional context for custom processing
 * @param sourceDoc  unused
 * @param attr  the attribute-node to be adopted
 * @param destDoc  the destination doc for adoption
 * @param destParent  the optional new parent of `attr` in `destDoc`
 * @param options  option flags (unused)
 * @returns 0 if succeeded, -1 otherwise and on API/internal errors.
 */
static int
xmlDOMWrapAdoptAttr(xmlDOMWrapCtxtPtr ctxt,
		    xmlDocPtr sourceDoc ATTRIBUTE_UNUSED,
		    xmlAttrPtr attr,
		    xmlDocPtr destDoc,
		    xmlNodePtr destParent,
		    int options ATTRIBUTE_UNUSED)
{
    int ret = 0;

    if ((attr == NULL) || (destDoc == NULL))
	return (-1);

    if (attr->doc != destDoc) {
        if (xmlSetTreeDoc((xmlNodePtr) attr, destDoc) < 0)
            ret = -1;
    }

    if (attr->ns != NULL) {
	xmlNsPtr ns = NULL;

	if (ctxt != NULL) {
	    /* TODO: User defined. */
	}
	/* XML Namespace. */
	if (IS_STR_XML(attr->ns->prefix)) {
	    ns = xmlTreeEnsureXMLDecl(destDoc);
	} else if (destParent == NULL) {
	    /*
	    * Store in @destDoc->oldNs.
	    */
	    ns = xmlDOMWrapStoreNs(destDoc, attr->ns->href, attr->ns->prefix);
	} else {
	    /*
	    * Declare on @destParent.
	    */
	    if (xmlSearchNsByNamespaceStrict(destDoc, destParent, attr->ns->href,
		&ns, 1) == -1)
		ret = -1;
	    if (ns == NULL) {
		ns = xmlDOMWrapNSNormDeclareNsForced(destDoc, destParent,
		    attr->ns->href, attr->ns->prefix, 1);
	    }
	}
	if (ns == NULL)
	    ret = -1;
	attr->ns = ns;
    }

    return (ret);
}

/**
 * Fix up namespaces before moving a node.
 *
 * References of out-of scope ns-decls are remapped to point to `destDoc`:
 * If `destParent` is given, then nsDef entries on element-nodes are used.
 * If *no* `destParent` is given, then `destDoc->oldNs` entries are used
 * This is the case when you have an unlinked node and just want to move it
 * to the context of.
 *
 * If `destParent` is given, it ensures that the tree is namespace
 * wellformed by creating additional ns-decls where needed.
 * Note that, since prefixes of already existent ns-decls can be
 * shadowed by this process, it could break QNames in attribute
 * values or element content.
 *
 * NOTE: This function was not intensively tested.
 *
 * @param ctxt  the optional context for custom processing
 * @param sourceDoc  the optional sourceDoc
 * @param node  the node to start with
 * @param destDoc  the destination doc
 * @param destParent  the optional new parent of `node` in `destDoc`
 * @param options  option flags
 * @returns 0 if the operation succeeded,
 *          1 if a node of unsupported type was given,
 *          2 if a node of not yet supported type was given and
 *          -1 on API/internal errors.
 */
int
xmlDOMWrapAdoptNode(xmlDOMWrapCtxt *ctxt,
		    xmlDoc *sourceDoc,
		    xmlNode *node,
		    xmlDoc *destDoc,
		    xmlNode *destParent,
		    int options)
{
    int ret = 0;

    if ((node == NULL) || (node->type == XML_NAMESPACE_DECL) ||
        (destDoc == NULL) ||
	((destParent != NULL) && (destParent->doc != destDoc)))
	return(-1);
    /*
    * Check node->doc sanity.
    */
    if (sourceDoc == NULL) {
        sourceDoc = node->doc;
    } else if (node->doc != sourceDoc) {
	return (-1);
    }

    /*
     * TODO: Shouldn't this be allowed?
     */
    if (sourceDoc == destDoc)
	return (-1);

    switch (node->type) {
	case XML_ELEMENT_NODE:
	case XML_ATTRIBUTE_NODE:
	case XML_TEXT_NODE:
	case XML_CDATA_SECTION_NODE:
	case XML_ENTITY_REF_NODE:
	case XML_PI_NODE:
	case XML_COMMENT_NODE:
	    break;
	case XML_DOCUMENT_FRAG_NODE:
	    /* TODO: Support document-fragment-nodes. */
	    return (2);
	default:
	    return (1);
    }
    /*
    * Unlink only if @node was not already added to @destParent.
    */
    if ((node->parent != NULL) && (destParent != node->parent))
	xmlUnlinkNodeInternal(node);

    if (node->type == XML_ELEMENT_NODE) {
	    return (xmlDOMWrapAdoptBranch(ctxt, sourceDoc, node,
		    destDoc, destParent, options));
    } else if (node->type == XML_ATTRIBUTE_NODE) {
	    return (xmlDOMWrapAdoptAttr(ctxt, sourceDoc,
		(xmlAttrPtr) node, destDoc, destParent, options));
    } else {
        if (node->doc != destDoc) {
            if (xmlNodeSetDoc(node, destDoc) < 0)
                ret = -1;
        }
    }
    return (ret);
}

/************************************************************************
 *									*
 *			XHTML detection					*
 *									*
 ************************************************************************/

#define XHTML_STRICT_PUBLIC_ID BAD_CAST \
   "-//W3C//DTD XHTML 1.0 Strict//EN"
#define XHTML_STRICT_SYSTEM_ID BAD_CAST \
   "http://www.w3.org/TR/xhtml1/DTD/xhtml1-strict.dtd"
#define XHTML_FRAME_PUBLIC_ID BAD_CAST \
   "-//W3C//DTD XHTML 1.0 Frameset//EN"
#define XHTML_FRAME_SYSTEM_ID BAD_CAST \
   "http://www.w3.org/TR/xhtml1/DTD/xhtml1-frameset.dtd"
#define XHTML_TRANS_PUBLIC_ID BAD_CAST \
   "-//W3C//DTD XHTML 1.0 Transitional//EN"
#define XHTML_TRANS_SYSTEM_ID BAD_CAST \
   "http://www.w3.org/TR/xhtml1/DTD/xhtml1-transitional.dtd"

/**
 * Try to find if the document correspond to an XHTML DTD
 *
 * @param systemID  the system identifier
 * @param publicID  the public identifier
 * @returns 1 if true, 0 if not and -1 in case of error
 */
int
xmlIsXHTML(const xmlChar *systemID, const xmlChar *publicID) {
    if ((systemID == NULL) && (publicID == NULL))
	return(-1);
    if (publicID != NULL) {
	if (xmlStrEqual(publicID, XHTML_STRICT_PUBLIC_ID)) return(1);
	if (xmlStrEqual(publicID, XHTML_FRAME_PUBLIC_ID)) return(1);
	if (xmlStrEqual(publicID, XHTML_TRANS_PUBLIC_ID)) return(1);
    }
    if (systemID != NULL) {
	if (xmlStrEqual(systemID, XHTML_STRICT_SYSTEM_ID)) return(1);
	if (xmlStrEqual(systemID, XHTML_FRAME_SYSTEM_ID)) return(1);
	if (xmlStrEqual(systemID, XHTML_TRANS_SYSTEM_ID)) return(1);
    }
    return(0);
}

/************************************************************************
 *									*
 *			Node callbacks					*
 *									*
 ************************************************************************/

/**
 * Registers a callback for node creation
 *
 * @deprecated don't use
 *
 * @param func  function pointer to the new RegisterNodeFunc
 * @returns the old value of the registration function
 */
xmlRegisterNodeFunc
xmlRegisterNodeDefault(xmlRegisterNodeFunc func)
{
    xmlRegisterNodeFunc old = xmlRegisterNodeDefaultValue;

    xmlRegisterCallbacks = 1;
    xmlRegisterNodeDefaultValue = func;
    return(old);
}

/**
 * Registers a callback for node destruction
 *
 * @deprecated don't use
 *
 * @param func  function pointer to the new DeregisterNodeFunc
 * @returns the previous value of the deregistration function
 */
xmlDeregisterNodeFunc
xmlDeregisterNodeDefault(xmlDeregisterNodeFunc func)
{
    xmlDeregisterNodeFunc old = xmlDeregisterNodeDefaultValue;

    xmlRegisterCallbacks = 1;
    xmlDeregisterNodeDefaultValue = func;
    return(old);
}


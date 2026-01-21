/**
 * catalog.c: set of generic Catalog related routines
 *
 * Reference:  SGML Open Technical Resolution TR9401:1997.
 *             http://www.jclark.com/sp/catalog.htm
 *
 *             XML Catalogs Working Draft 06 August 2001
 *             http://www.oasis-open.org/committees/entity/spec-2001-08-06.html
 *
 * See Copyright for the status of this software.
 *
 * Author: Daniel Veillard
 */

#define IN_LIBXML
#include "libxml.h"

#ifdef LIBXML_CATALOG_ENABLED
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fcntl.h>
#include <sys/stat.h>

#ifdef _WIN32
  #include <io.h>
#else
  #include <unistd.h>
#endif

#include <libxml/xmlmemory.h>
#include <libxml/hash.h>
#include <libxml/uri.h>
#include <libxml/parserInternals.h>
#include <libxml/catalog.h>
#include <libxml/xmlerror.h>
#include <libxml/threads.h>

#include "private/cata.h"
#include "private/buf.h"
#include "private/error.h"
#include "private/memory.h"
#include "private/threads.h"

#define MAX_DELEGATE	50
#define MAX_CATAL_DEPTH	50

#ifdef _WIN32
# define PATH_SEPARATOR ';'
#else
# define PATH_SEPARATOR ':'
#endif

#define XML_URN_PUBID "urn:publicid:"
#define XML_CATAL_BREAK ((xmlChar *) -1)
#ifndef XML_XML_DEFAULT_CATALOG
#define XML_XML_DEFAULT_CATALOG "file://" XML_SYSCONFDIR "/xml/catalog"
#endif

#ifdef LIBXML_SGML_CATALOG_ENABLED
#ifndef XML_SGML_DEFAULT_CATALOG
#define XML_SGML_DEFAULT_CATALOG "file://" XML_SYSCONFDIR "/sgml/catalog"
#endif
#endif

static xmlChar *xmlCatalogNormalizePublic(const xmlChar *pubID);
static int xmlExpandCatalog(xmlCatalogPtr catal, const char *filename);

/************************************************************************
 *									*
 *			Types, all private				*
 *									*
 ************************************************************************/

typedef enum {
    XML_CATA_REMOVED = -1,
    XML_CATA_NONE = 0,
    XML_CATA_CATALOG,
    XML_CATA_BROKEN_CATALOG,
    XML_CATA_NEXT_CATALOG,
    XML_CATA_GROUP,
    XML_CATA_PUBLIC,
    XML_CATA_SYSTEM,
    XML_CATA_REWRITE_SYSTEM,
    XML_CATA_DELEGATE_PUBLIC,
    XML_CATA_DELEGATE_SYSTEM,
    XML_CATA_URI,
    XML_CATA_REWRITE_URI,
    XML_CATA_DELEGATE_URI
#ifdef LIBXML_SGML_CATALOG_ENABLED
    ,
    SGML_CATA_SYSTEM,
    SGML_CATA_PUBLIC,
    SGML_CATA_ENTITY,
    SGML_CATA_PENTITY,
    SGML_CATA_DOCTYPE,
    SGML_CATA_LINKTYPE,
    SGML_CATA_NOTATION,
    SGML_CATA_DELEGATE,
    SGML_CATA_BASE,
    SGML_CATA_CATALOG,
    SGML_CATA_DOCUMENT,
    SGML_CATA_SGMLDECL
#endif
} xmlCatalogEntryType;

typedef struct _xmlCatalogEntry xmlCatalogEntry;
typedef xmlCatalogEntry *xmlCatalogEntryPtr;
struct _xmlCatalogEntry {
    struct _xmlCatalogEntry *next;
    struct _xmlCatalogEntry *parent;
    struct _xmlCatalogEntry *children;
    xmlCatalogEntryType type;
    xmlChar *name;
    xmlChar *value;
    xmlChar *URL;  /* The expanded URL using the base */
    xmlCatalogPrefer prefer;
    int dealloc;
    int depth;
    struct _xmlCatalogEntry *group;
};

typedef enum {
    XML_XML_CATALOG_TYPE = 1
#ifdef LIBXML_SGML_CATALOG_ENABLED
    ,
    XML_SGML_CATALOG_TYPE
#endif
} xmlCatalogType;

#ifdef LIBXML_SGML_CATALOG_ENABLED
#define XML_MAX_SGML_CATA_DEPTH 10
#endif
struct _xmlCatalog {
    xmlCatalogType type;	/* either XML or SGML */

#ifdef LIBXML_SGML_CATALOG_ENABLED
    /*
     * SGML Catalogs are stored as a simple hash table of catalog entries
     * Catalog stack to check against overflows when building the
     * SGML catalog
     */
    char *catalTab[XML_MAX_SGML_CATA_DEPTH];	/* stack of catals */
    int          catalNr;	/* Number of current catal streams */
    int          catalMax;	/* Max number of catal streams */
    xmlHashTablePtr sgml;
#endif

    /*
     * XML Catalogs are stored as a tree of Catalog entries
     */
    xmlCatalogPrefer prefer;
    xmlCatalogEntryPtr xml;
};

/************************************************************************
 *									*
 *			Global variables				*
 *									*
 ************************************************************************/

/*
 * Those are preferences
 */
static int xmlDebugCatalogs = 0;   /* used for debugging */
static xmlCatalogAllow xmlCatalogDefaultAllow = XML_CATA_ALLOW_ALL;
static xmlCatalogPrefer xmlCatalogDefaultPrefer = XML_CATA_PREFER_PUBLIC;

/*
 * Hash table containing all the trees of XML catalogs parsed by
 * the application.
 */
static xmlHashTablePtr xmlCatalogXMLFiles = NULL;

/*
 * The default catalog in use by the application
 */
static xmlCatalogPtr xmlDefaultCatalog = NULL;

/*
 * A mutex for modifying the shared global catalog(s)
 * xmlDefaultCatalog tree.
 * It also protects xmlCatalogXMLFiles
 * The core of this readers/writer scheme is in #xmlFetchXMLCatalogFile
 */
static xmlRMutex xmlCatalogMutex;

/*
 * Whether the default system catalog was initialized.
 */
static int xmlCatalogInitialized = 0;

/************************************************************************
 *									*
 *			Catalog error handlers				*
 *									*
 ************************************************************************/

/**
 * Handle an out of memory condition
 */
static void
xmlCatalogErrMemory(void)
{
    xmlRaiseMemoryError(NULL, NULL, NULL, XML_FROM_CATALOG, NULL);
}

/**
 * Handle a catalog error
 *
 * @param catal  the Catalog entry
 * @param node  the context node
 * @param error  the error code
 * @param msg  the error message
 * @param str1  error string 1
 * @param str2  error string 2
 * @param str3  error string 3
 */
static void LIBXML_ATTR_FORMAT(4,0)
xmlCatalogErr(xmlCatalogEntryPtr catal, xmlNodePtr node, int error,
               const char *msg, const xmlChar *str1, const xmlChar *str2,
	       const xmlChar *str3)
{
    int res;

    res = xmlRaiseError(NULL, NULL, NULL, catal, node,
                        XML_FROM_CATALOG, error, XML_ERR_ERROR, NULL, 0,
                        (const char *) str1, (const char *) str2,
                        (const char *) str3, 0, 0,
                        msg, str1, str2, str3);
    if (res < 0)
        xmlCatalogErrMemory();
}

static void
xmlCatalogPrintDebug(const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    xmlVPrintErrorMessage(fmt, ap);
    va_end(ap);
}

/************************************************************************
 *									*
 *			Allocation and Freeing				*
 *									*
 ************************************************************************/

/**
 * create a new Catalog entry, this type is shared both by XML and
 * SGML catalogs, but the acceptable types values differs.
 *
 * @param type  type of entry
 * @param name  name of the entry
 * @param value  value of the entry
 * @param URL  URL of the entry
 * @param prefer  the PUBLIC vs. SYSTEM current preference value
 * @param group  for members of a group, the group entry
 * @returns the xmlCatalogEntry or NULL in case of error
 */
static xmlCatalogEntryPtr
xmlNewCatalogEntry(xmlCatalogEntryType type, const xmlChar *name,
	   const xmlChar *value, const xmlChar *URL, xmlCatalogPrefer prefer,
	   xmlCatalogEntryPtr group) {
    xmlCatalogEntryPtr ret;
    xmlChar *normid = NULL;

    ret = (xmlCatalogEntryPtr) xmlMalloc(sizeof(xmlCatalogEntry));
    if (ret == NULL) {
        xmlCatalogErrMemory();
	return(NULL);
    }
    ret->next = NULL;
    ret->parent = NULL;
    ret->children = NULL;
    ret->type = type;
    if (type == XML_CATA_PUBLIC || type == XML_CATA_DELEGATE_PUBLIC) {
        normid = xmlCatalogNormalizePublic(name);
        if (normid != NULL)
            name = (*normid != 0 ? normid : NULL);
    }
    if (name != NULL)
	ret->name = xmlStrdup(name);
    else
	ret->name = NULL;
    if (normid != NULL)
        xmlFree(normid);
    if (value != NULL)
	ret->value = xmlStrdup(value);
    else
	ret->value = NULL;
    if (URL == NULL)
	URL = value;
    if (URL != NULL)
	ret->URL = xmlStrdup(URL);
    else
	ret->URL = NULL;
    ret->prefer = prefer;
    ret->dealloc = 0;
    ret->depth = 0;
    ret->group = group;
    return(ret);
}

static void
xmlFreeCatalogEntryList(xmlCatalogEntryPtr ret);

/**
 * Free the memory allocated to a Catalog entry
 *
 * @param payload  a Catalog entry
 * @param name  unused
 */
static void
xmlFreeCatalogEntry(void *payload, const xmlChar *name ATTRIBUTE_UNUSED) {
    xmlCatalogEntryPtr ret = (xmlCatalogEntryPtr) payload;
    if (ret == NULL)
	return;
    /*
     * Entries stored in the file hash must be deallocated
     * only by the file hash cleaner !
     */
    if (ret->dealloc == 1)
	return;

    if (xmlDebugCatalogs) {
	if (ret->name != NULL)
	    xmlCatalogPrintDebug(
		    "Free catalog entry %s\n", ret->name);
	else if (ret->value != NULL)
	    xmlCatalogPrintDebug(
		    "Free catalog entry %s\n", ret->value);
	else
	    xmlCatalogPrintDebug(
		    "Free catalog entry\n");
    }

    if (ret->name != NULL)
	xmlFree(ret->name);
    if (ret->value != NULL)
	xmlFree(ret->value);
    if (ret->URL != NULL)
	xmlFree(ret->URL);
    xmlFree(ret);
}

/**
 * Free the memory allocated to a full chained list of Catalog entries
 *
 * @param ret  a Catalog entry list
 */
static void
xmlFreeCatalogEntryList(xmlCatalogEntryPtr ret) {
    xmlCatalogEntryPtr next;

    while (ret != NULL) {
	next = ret->next;
	xmlFreeCatalogEntry(ret, NULL);
	ret = next;
    }
}

/**
 * Free the memory allocated to list of Catalog entries from the
 * catalog file hash.
 *
 * @param payload  a Catalog entry list
 * @param name  unused
 */
static void
xmlFreeCatalogHashEntryList(void *payload,
                            const xmlChar *name ATTRIBUTE_UNUSED) {
    xmlCatalogEntryPtr catal = (xmlCatalogEntryPtr) payload;
    xmlCatalogEntryPtr children, next;

    if (catal == NULL)
	return;

    children = catal->children;
    while (children != NULL) {
	next = children->next;
	children->dealloc = 0;
	children->children = NULL;
	xmlFreeCatalogEntry(children, NULL);
	children = next;
    }
    catal->dealloc = 0;
    xmlFreeCatalogEntry(catal, NULL);
}

/**
 * create a new Catalog, this type is shared both by XML and
 * SGML catalogs, but the acceptable types values differs.
 *
 * @param type  type of catalog
 * @param prefer  the PUBLIC vs. SYSTEM current preference value
 * @returns the xmlCatalog or NULL in case of error
 */
static xmlCatalogPtr
xmlCreateNewCatalog(xmlCatalogType type, xmlCatalogPrefer prefer) {
    xmlCatalogPtr ret;

    ret = (xmlCatalogPtr) xmlMalloc(sizeof(xmlCatalog));
    if (ret == NULL) {
        xmlCatalogErrMemory();
	return(NULL);
    }
    memset(ret, 0, sizeof(xmlCatalog));
    ret->type = type;
    ret->prefer = prefer;
#ifdef LIBXML_SGML_CATALOG_ENABLED
    ret->catalNr = 0;
    ret->catalMax = XML_MAX_SGML_CATA_DEPTH;
    if (ret->type == XML_SGML_CATALOG_TYPE)
	ret->sgml = xmlHashCreate(10);
#endif
    return(ret);
}

/**
 * Free the memory allocated to a Catalog
 *
 * @deprecated Internal function, don't use
 *
 * @param catal  a Catalog
 */
void
xmlFreeCatalog(xmlCatalog *catal) {
    if (catal == NULL)
	return;
    if (catal->xml != NULL)
	xmlFreeCatalogEntryList(catal->xml);
#ifdef LIBXML_SGML_CATALOG_ENABLED
    if (catal->sgml != NULL)
	xmlHashFree(catal->sgml, xmlFreeCatalogEntry);
#endif
    xmlFree(catal);
}

/************************************************************************
 *									*
 *			Serializing Catalogs				*
 *									*
 ************************************************************************/

#ifdef LIBXML_OUTPUT_ENABLED
#ifdef LIBXML_SGML_CATALOG_ENABLED
/**
 * Serialize an SGML Catalog entry
 *
 * @param payload  the catalog entry
 * @param data  the file.
 * @param name  unused
 */
static void
xmlCatalogDumpEntry(void *payload, void *data,
                    const xmlChar *name ATTRIBUTE_UNUSED) {
    xmlCatalogEntryPtr entry = (xmlCatalogEntryPtr) payload;
    FILE *out = (FILE *) data;
    if ((entry == NULL) || (out == NULL))
	return;
    switch (entry->type) {
	case SGML_CATA_ENTITY:
	    fprintf(out, "ENTITY "); break;
	case SGML_CATA_PENTITY:
	    fprintf(out, "ENTITY %%"); break;
	case SGML_CATA_DOCTYPE:
	    fprintf(out, "DOCTYPE "); break;
	case SGML_CATA_LINKTYPE:
	    fprintf(out, "LINKTYPE "); break;
	case SGML_CATA_NOTATION:
	    fprintf(out, "NOTATION "); break;
	case SGML_CATA_PUBLIC:
	    fprintf(out, "PUBLIC "); break;
	case SGML_CATA_SYSTEM:
	    fprintf(out, "SYSTEM "); break;
	case SGML_CATA_DELEGATE:
	    fprintf(out, "DELEGATE "); break;
	case SGML_CATA_BASE:
	    fprintf(out, "BASE "); break;
	case SGML_CATA_CATALOG:
	    fprintf(out, "CATALOG "); break;
	case SGML_CATA_DOCUMENT:
	    fprintf(out, "DOCUMENT "); break;
	case SGML_CATA_SGMLDECL:
	    fprintf(out, "SGMLDECL "); break;
	default:
	    return;
    }
    switch (entry->type) {
	case SGML_CATA_ENTITY:
	case SGML_CATA_PENTITY:
	case SGML_CATA_DOCTYPE:
	case SGML_CATA_LINKTYPE:
	case SGML_CATA_NOTATION:
	    fprintf(out, "%s", (const char *) entry->name); break;
	case SGML_CATA_PUBLIC:
	case SGML_CATA_SYSTEM:
	case SGML_CATA_SGMLDECL:
	case SGML_CATA_DOCUMENT:
	case SGML_CATA_CATALOG:
	case SGML_CATA_BASE:
	case SGML_CATA_DELEGATE:
	    fprintf(out, "\"%s\"", entry->name); break;
	default:
	    break;
    }
    switch (entry->type) {
	case SGML_CATA_ENTITY:
	case SGML_CATA_PENTITY:
	case SGML_CATA_DOCTYPE:
	case SGML_CATA_LINKTYPE:
	case SGML_CATA_NOTATION:
	case SGML_CATA_PUBLIC:
	case SGML_CATA_SYSTEM:
	case SGML_CATA_DELEGATE:
	    fprintf(out, " \"%s\"", entry->value); break;
	default:
	    break;
    }
    fprintf(out, "\n");
}
#endif /* LIBXML_SGML_CATALOG_ENABLED */

/**
 * Serializes a Catalog entry, called by xmlDumpXMLCatalog and recursively
 * for group entries
 *
 * @param catal  top catalog entry
 * @param catalog  pointer to the xml tree
 * @param doc  the containing document
 * @param ns  the current namespace
 * @param cgroup  group node for group members
 */
static void xmlDumpXMLCatalogNode(xmlCatalogEntryPtr catal, xmlNodePtr catalog,
		    xmlDocPtr doc, xmlNsPtr ns, xmlCatalogEntryPtr cgroup) {
    xmlNodePtr node;
    xmlCatalogEntryPtr cur;
    /*
     * add all the catalog entries
     */
    cur = catal;
    while (cur != NULL) {
        if (cur->group == cgroup) {
	    switch (cur->type) {
	        case XML_CATA_REMOVED:
		    break;
	        case XML_CATA_BROKEN_CATALOG:
	        case XML_CATA_CATALOG:
		    if (cur == catal) {
			cur = cur->children;
		        continue;
		    }
		    break;
		case XML_CATA_NEXT_CATALOG:
		    node = xmlNewDocNode(doc, ns, BAD_CAST "nextCatalog", NULL);
		    xmlSetProp(node, BAD_CAST "catalog", cur->value);
		    xmlAddChild(catalog, node);
                    break;
		case XML_CATA_NONE:
		    break;
		case XML_CATA_GROUP:
		    node = xmlNewDocNode(doc, ns, BAD_CAST "group", NULL);
		    xmlSetProp(node, BAD_CAST "id", cur->name);
		    if (cur->value != NULL) {
		        xmlNsPtr xns;
			xns = xmlSearchNsByHref(doc, node, XML_XML_NAMESPACE);
			if (xns != NULL)
			    xmlSetNsProp(node, xns, BAD_CAST "base",
					 cur->value);
		    }
		    switch (cur->prefer) {
			case XML_CATA_PREFER_NONE:
		            break;
			case XML_CATA_PREFER_PUBLIC:
		            xmlSetProp(node, BAD_CAST "prefer", BAD_CAST "public");
			    break;
			case XML_CATA_PREFER_SYSTEM:
		            xmlSetProp(node, BAD_CAST "prefer", BAD_CAST "system");
			    break;
		    }
		    xmlDumpXMLCatalogNode(cur->next, node, doc, ns, cur);
		    xmlAddChild(catalog, node);
	            break;
		case XML_CATA_PUBLIC:
		    node = xmlNewDocNode(doc, ns, BAD_CAST "public", NULL);
		    xmlSetProp(node, BAD_CAST "publicId", cur->name);
		    xmlSetProp(node, BAD_CAST "uri", cur->value);
		    xmlAddChild(catalog, node);
		    break;
		case XML_CATA_SYSTEM:
		    node = xmlNewDocNode(doc, ns, BAD_CAST "system", NULL);
		    xmlSetProp(node, BAD_CAST "systemId", cur->name);
		    xmlSetProp(node, BAD_CAST "uri", cur->value);
		    xmlAddChild(catalog, node);
		    break;
		case XML_CATA_REWRITE_SYSTEM:
		    node = xmlNewDocNode(doc, ns, BAD_CAST "rewriteSystem", NULL);
		    xmlSetProp(node, BAD_CAST "systemIdStartString", cur->name);
		    xmlSetProp(node, BAD_CAST "rewritePrefix", cur->value);
		    xmlAddChild(catalog, node);
		    break;
		case XML_CATA_DELEGATE_PUBLIC:
		    node = xmlNewDocNode(doc, ns, BAD_CAST "delegatePublic", NULL);
		    xmlSetProp(node, BAD_CAST "publicIdStartString", cur->name);
		    xmlSetProp(node, BAD_CAST "catalog", cur->value);
		    xmlAddChild(catalog, node);
		    break;
		case XML_CATA_DELEGATE_SYSTEM:
		    node = xmlNewDocNode(doc, ns, BAD_CAST "delegateSystem", NULL);
		    xmlSetProp(node, BAD_CAST "systemIdStartString", cur->name);
		    xmlSetProp(node, BAD_CAST "catalog", cur->value);
		    xmlAddChild(catalog, node);
		    break;
		case XML_CATA_URI:
		    node = xmlNewDocNode(doc, ns, BAD_CAST "uri", NULL);
		    xmlSetProp(node, BAD_CAST "name", cur->name);
		    xmlSetProp(node, BAD_CAST "uri", cur->value);
		    xmlAddChild(catalog, node);
		    break;
		case XML_CATA_REWRITE_URI:
		    node = xmlNewDocNode(doc, ns, BAD_CAST "rewriteURI", NULL);
		    xmlSetProp(node, BAD_CAST "uriStartString", cur->name);
		    xmlSetProp(node, BAD_CAST "rewritePrefix", cur->value);
		    xmlAddChild(catalog, node);
		    break;
		case XML_CATA_DELEGATE_URI:
		    node = xmlNewDocNode(doc, ns, BAD_CAST "delegateURI", NULL);
		    xmlSetProp(node, BAD_CAST "uriStartString", cur->name);
		    xmlSetProp(node, BAD_CAST "catalog", cur->value);
		    xmlAddChild(catalog, node);
		    break;
                default:
		    break;
	    }
        }
	cur = cur->next;
    }
}

static int
xmlDumpXMLCatalog(FILE *out, xmlCatalogEntryPtr catal) {
    int ret;
    xmlDocPtr doc;
    xmlNsPtr ns;
    xmlDtdPtr dtd;
    xmlNodePtr catalog;
    xmlOutputBufferPtr buf;

    /*
     * Rebuild a catalog
     */
    doc = xmlNewDoc(NULL);
    if (doc == NULL)
	return(-1);
    dtd = xmlNewDtd(doc, BAD_CAST "catalog",
	       BAD_CAST "-//OASIS//DTD Entity Resolution XML Catalog V1.0//EN",
BAD_CAST "http://www.oasis-open.org/committees/entity/release/1.0/catalog.dtd");

    xmlAddChild((xmlNodePtr) doc, (xmlNodePtr) dtd);

    ns = xmlNewNs(NULL, XML_CATALOGS_NAMESPACE, NULL);
    if (ns == NULL) {
	xmlFreeDoc(doc);
	return(-1);
    }
    catalog = xmlNewDocNode(doc, ns, BAD_CAST "catalog", NULL);
    if (catalog == NULL) {
	xmlFreeNs(ns);
	xmlFreeDoc(doc);
	return(-1);
    }
    catalog->nsDef = ns;
    xmlAddChild((xmlNodePtr) doc, catalog);

    xmlDumpXMLCatalogNode(catal, catalog, doc, ns, NULL);

    /*
     * reserialize it
     */
    buf = xmlOutputBufferCreateFile(out, NULL);
    if (buf == NULL) {
	xmlFreeDoc(doc);
	return(-1);
    }
    ret = xmlSaveFormatFileTo(buf, doc, NULL, 1);

    /*
     * Free it
     */
    xmlFreeDoc(doc);

    return(ret);
}
#endif /* LIBXML_OUTPUT_ENABLED */

/************************************************************************
 *									*
 *			Converting SGML Catalogs to XML			*
 *									*
 ************************************************************************/

#ifdef LIBXML_SGML_CATALOG_ENABLED

/**
 * Convert one entry from the catalog
 *
 * @param payload  the entry
 * @param data  pointer to the catalog being converted
 * @param name  unused
 */
static void
xmlCatalogConvertEntry(void *payload, void *data,
                       const xmlChar *name ATTRIBUTE_UNUSED) {
    xmlCatalogEntryPtr entry = (xmlCatalogEntryPtr) payload;
    xmlCatalogPtr catal = (xmlCatalogPtr) data;
    if ((entry == NULL) || (catal == NULL) || (catal->sgml == NULL) ||
	(catal->xml == NULL))
	return;
    switch (entry->type) {
	case SGML_CATA_ENTITY:
	    entry->type = XML_CATA_PUBLIC;
	    break;
	case SGML_CATA_PENTITY:
	    entry->type = XML_CATA_PUBLIC;
	    break;
	case SGML_CATA_DOCTYPE:
	    entry->type = XML_CATA_PUBLIC;
	    break;
	case SGML_CATA_LINKTYPE:
	    entry->type = XML_CATA_PUBLIC;
	    break;
	case SGML_CATA_NOTATION:
	    entry->type = XML_CATA_PUBLIC;
	    break;
	case SGML_CATA_PUBLIC:
	    entry->type = XML_CATA_PUBLIC;
	    break;
	case SGML_CATA_SYSTEM:
	    entry->type = XML_CATA_SYSTEM;
	    break;
	case SGML_CATA_DELEGATE:
	    entry->type = XML_CATA_DELEGATE_PUBLIC;
	    break;
	case SGML_CATA_CATALOG:
	    entry->type = XML_CATA_CATALOG;
	    break;
	default:
	    xmlHashRemoveEntry(catal->sgml, entry->name, xmlFreeCatalogEntry);
	    return;
    }
    /*
     * Conversion successful, remove from the SGML catalog
     * and add it to the default XML one
     */
    xmlHashRemoveEntry(catal->sgml, entry->name, NULL);
    entry->parent = catal->xml;
    entry->next = NULL;
    if (catal->xml->children == NULL)
	catal->xml->children = entry;
    else {
	xmlCatalogEntryPtr prev;

	prev = catal->xml->children;
	while (prev->next != NULL)
	    prev = prev->next;
	prev->next = entry;
    }
}

/**
 * Convert all the SGML catalog entries as XML ones
 *
 * @deprecated Internal function, don't use
 *
 * @param catal  the catalog
 * @returns the number of entries converted if successful, -1 otherwise
 */
int
xmlConvertSGMLCatalog(xmlCatalog *catal) {

    if ((catal == NULL) || (catal->type != XML_SGML_CATALOG_TYPE))
	return(-1);

    if (xmlDebugCatalogs) {
	xmlCatalogPrintDebug(
		"Converting SGML catalog to XML\n");
    }
    xmlHashScan(catal->sgml, xmlCatalogConvertEntry, &catal);
    return(0);
}

#endif /* LIBXML_SGML_CATALOG_ENABLED */

/************************************************************************
 *									*
 *			Helper function					*
 *									*
 ************************************************************************/

/**
 * Expand the URN into the equivalent Public Identifier
 *
 * @param urn  an "urn:publicid:" to unwrap
 * @returns the new identifier or NULL, the string must be deallocated
 *         by the caller.
 */
static xmlChar *
xmlCatalogUnWrapURN(const xmlChar *urn) {
    xmlChar result[2000];
    unsigned int i = 0;

    if (xmlStrncmp(urn, BAD_CAST XML_URN_PUBID, sizeof(XML_URN_PUBID) - 1))
	return(NULL);
    urn += sizeof(XML_URN_PUBID) - 1;

    while (*urn != 0) {
	if (i > sizeof(result) - 4)
	    break;
	if (*urn == '+') {
	    result[i++] = ' ';
	    urn++;
	} else if (*urn == ':') {
	    result[i++] = '/';
	    result[i++] = '/';
	    urn++;
	} else if (*urn == ';') {
	    result[i++] = ':';
	    result[i++] = ':';
	    urn++;
	} else if (*urn == '%') {
	    if ((urn[1] == '2') && (urn[2] == 'B'))
		result[i++] = '+';
	    else if ((urn[1] == '3') && (urn[2] == 'A'))
		result[i++] = ':';
	    else if ((urn[1] == '2') && (urn[2] == 'F'))
		result[i++] = '/';
	    else if ((urn[1] == '3') && (urn[2] == 'B'))
		result[i++] = ';';
	    else if ((urn[1] == '2') && (urn[2] == '7'))
		result[i++] = '\'';
	    else if ((urn[1] == '3') && (urn[2] == 'F'))
		result[i++] = '?';
	    else if ((urn[1] == '2') && (urn[2] == '3'))
		result[i++] = '#';
	    else if ((urn[1] == '2') && (urn[2] == '5'))
		result[i++] = '%';
	    else {
		result[i++] = *urn;
		urn++;
		continue;
	    }
	    urn += 3;
	} else {
	    result[i++] = *urn;
	    urn++;
	}
    }
    result[i] = 0;

    return(xmlStrdup(result));
}

/**
 * parse an XML file and build a tree. It's like #xmlParseFile
 * except it bypass all catalog lookups.
 *
 * @deprecated Internal function, don't use
 *
 * @param filename  the filename
 * @returns the resulting document tree or NULL in case of error
 */

xmlDoc *
xmlParseCatalogFile(const char *filename) {
    xmlDocPtr ret;
    xmlParserCtxtPtr ctxt;
    xmlParserInputPtr inputStream;
    xmlParserInputBufferPtr buf;

    ctxt = xmlNewParserCtxt();
    if (ctxt == NULL) {
        xmlCatalogErrMemory();
	return(NULL);
    }

    buf = xmlParserInputBufferCreateFilename(filename, XML_CHAR_ENCODING_NONE);
    if (buf == NULL) {
	xmlFreeParserCtxt(ctxt);
	return(NULL);
    }

    inputStream = xmlNewInputStream(ctxt);
    if (inputStream == NULL) {
	xmlFreeParserInputBuffer(buf);
	xmlFreeParserCtxt(ctxt);
	return(NULL);
    }

    inputStream->filename = (char *) xmlCanonicPath((const xmlChar *)filename);
    inputStream->buf = buf;
    xmlBufResetInput(buf->buffer, inputStream);

    if (xmlCtxtPushInput(ctxt, inputStream) < 0) {
        xmlFreeInputStream(inputStream);
        xmlFreeParserCtxt(ctxt);
        return(NULL);
    }

    ctxt->valid = 0;
    ctxt->validate = 0;
    ctxt->loadsubset = 0;
    ctxt->pedantic = 0;
    ctxt->dictNames = 1;

    xmlParseDocument(ctxt);

    if (ctxt->wellFormed)
	ret = ctxt->myDoc;
    else {
        ret = NULL;
        xmlFreeDoc(ctxt->myDoc);
        ctxt->myDoc = NULL;
    }
    xmlFreeParserCtxt(ctxt);

    return(ret);
}

/**
 * Load a file content into memory.
 *
 * @param filename  a file path
 * @returns a pointer to the 0 terminated string or NULL in case of error
 */
static xmlChar *
xmlLoadFileContent(const char *filename)
{
    int fd;
    int len;
    long size;

    struct stat info;
    xmlChar *content;

    if (filename == NULL)
        return (NULL);

    if (stat(filename, &info) < 0)
        return (NULL);

    fd = open(filename, O_RDONLY);
    if (fd  < 0)
    {
        return (NULL);
    }
    size = info.st_size;
    content = xmlMalloc(size + 10);
    if (content == NULL) {
        xmlCatalogErrMemory();
	close(fd);
        return (NULL);
    }
    len = read(fd, content, size);
    close(fd);
    if (len < 0) {
        xmlFree(content);
        return (NULL);
    }
    content[len] = 0;

    return(content);
}

/**
 *  Normalizes the Public Identifier
 *
 * Implements 6.2. Public Identifier Normalization
 * from http://www.oasis-open.org/committees/entity/spec-2001-08-06.html
 *
 * @param pubID  the public ID string
 * @returns the new string or NULL, the string must be deallocated
 *         by the caller.
 */
static xmlChar *
xmlCatalogNormalizePublic(const xmlChar *pubID)
{
    int ok = 1;
    int white;
    const xmlChar *p;
    xmlChar *ret;
    xmlChar *q;

    if (pubID == NULL)
        return(NULL);

    white = 1;
    for (p = pubID;*p != 0 && ok;p++) {
        if (!xmlIsBlank_ch(*p))
            white = 0;
        else if (*p == 0x20 && !white)
            white = 1;
        else
            ok = 0;
    }
    if (ok && !white)	/* is normalized */
        return(NULL);

    ret = xmlStrdup(pubID);
    q = ret;
    white = 0;
    for (p = pubID;*p != 0;p++) {
        if (xmlIsBlank_ch(*p)) {
            if (q != ret)
                white = 1;
        } else {
            if (white) {
                *(q++) = 0x20;
                white = 0;
            }
            *(q++) = *p;
        }
    }
    *q = 0;
    return(ret);
}

/************************************************************************
 *									*
 *			The XML Catalog parser				*
 *									*
 ************************************************************************/

static xmlCatalogEntryPtr
xmlParseXMLCatalogFile(xmlCatalogPrefer prefer, const xmlChar *filename);
static void
xmlParseXMLCatalogNodeList(xmlNodePtr cur, xmlCatalogPrefer prefer,
	                   xmlCatalogEntryPtr parent, xmlCatalogEntryPtr cgroup);
static xmlChar *
xmlCatalogListXMLResolve(xmlCatalogEntryPtr catal, const xmlChar *pubID,
	              const xmlChar *sysID);
static xmlChar *
xmlCatalogListXMLResolveURI(xmlCatalogEntryPtr catal, const xmlChar *URI);


/**
 * lookup the internal type associated to an XML catalog entry name
 *
 * @param name  the name
 * @returns the type associated with that name
 */
static xmlCatalogEntryType
xmlGetXMLCatalogEntryType(const xmlChar *name) {
    xmlCatalogEntryType type = XML_CATA_NONE;
    if (xmlStrEqual(name, (const xmlChar *) "system"))
	type = XML_CATA_SYSTEM;
    else if (xmlStrEqual(name, (const xmlChar *) "public"))
	type = XML_CATA_PUBLIC;
    else if (xmlStrEqual(name, (const xmlChar *) "rewriteSystem"))
	type = XML_CATA_REWRITE_SYSTEM;
    else if (xmlStrEqual(name, (const xmlChar *) "delegatePublic"))
	type = XML_CATA_DELEGATE_PUBLIC;
    else if (xmlStrEqual(name, (const xmlChar *) "delegateSystem"))
	type = XML_CATA_DELEGATE_SYSTEM;
    else if (xmlStrEqual(name, (const xmlChar *) "uri"))
	type = XML_CATA_URI;
    else if (xmlStrEqual(name, (const xmlChar *) "rewriteURI"))
	type = XML_CATA_REWRITE_URI;
    else if (xmlStrEqual(name, (const xmlChar *) "delegateURI"))
	type = XML_CATA_DELEGATE_URI;
    else if (xmlStrEqual(name, (const xmlChar *) "nextCatalog"))
	type = XML_CATA_NEXT_CATALOG;
    else if (xmlStrEqual(name, (const xmlChar *) "catalog"))
	type = XML_CATA_CATALOG;
    return(type);
}

/**
 * Finishes the examination of an XML tree node of a catalog and build
 * a Catalog entry from it.
 *
 * @param cur  the XML node
 * @param type  the type of Catalog entry
 * @param name  the name of the node
 * @param attrName  the attribute holding the value
 * @param uriAttrName  the attribute holding the URI-Reference
 * @param prefer  the PUBLIC vs. SYSTEM current preference value
 * @param cgroup  the group which includes this node
 * @returns the new Catalog entry node or NULL in case of error.
 */
static xmlCatalogEntryPtr
xmlParseXMLCatalogOneNode(xmlNodePtr cur, xmlCatalogEntryType type,
			  const xmlChar *name, const xmlChar *attrName,
			  const xmlChar *uriAttrName, xmlCatalogPrefer prefer,
			  xmlCatalogEntryPtr cgroup) {
    int ok = 1;
    xmlChar *uriValue;
    xmlChar *nameValue = NULL;
    xmlChar *base = NULL;
    xmlChar *URL = NULL;
    xmlCatalogEntryPtr ret = NULL;

    if (attrName != NULL) {
	nameValue = xmlGetProp(cur, attrName);
	if (nameValue == NULL) {
	    xmlCatalogErr(ret, cur, XML_CATALOG_MISSING_ATTR,
			  "%s entry lacks '%s'\n", name, attrName, NULL);
	    ok = 0;
	}
    }
    uriValue = xmlGetProp(cur, uriAttrName);
    if (uriValue == NULL) {
	xmlCatalogErr(ret, cur, XML_CATALOG_MISSING_ATTR,
		"%s entry lacks '%s'\n", name, uriAttrName, NULL);
	ok = 0;
    }
    if (!ok) {
	if (nameValue != NULL)
	    xmlFree(nameValue);
	if (uriValue != NULL)
	    xmlFree(uriValue);
	return(NULL);
    }

    base = xmlNodeGetBase(cur->doc, cur);
    URL = xmlBuildURI(uriValue, base);
    if (URL != NULL) {
	if (xmlDebugCatalogs > 1) {
	    if (nameValue != NULL)
		xmlCatalogPrintDebug(
			"Found %s: '%s' '%s'\n", name, nameValue, URL);
	    else
		xmlCatalogPrintDebug(
			"Found %s: '%s'\n", name, URL);
	}
	ret = xmlNewCatalogEntry(type, nameValue, uriValue, URL, prefer, cgroup);
    } else {
	xmlCatalogErr(ret, cur, XML_CATALOG_ENTRY_BROKEN,
		"%s entry '%s' broken ?: %s\n", name, uriAttrName, uriValue);
    }
    if (nameValue != NULL)
	xmlFree(nameValue);
    if (uriValue != NULL)
	xmlFree(uriValue);
    if (base != NULL)
	xmlFree(base);
    if (URL != NULL)
	xmlFree(URL);
    return(ret);
}

/**
 * Examines an XML tree node of a catalog and build
 * a Catalog entry from it adding it to its parent. The examination can
 * be recursive.
 *
 * @param cur  the XML node
 * @param prefer  the PUBLIC vs. SYSTEM current preference value
 * @param parent  the parent Catalog entry
 * @param cgroup  the group which includes this node
 */
static void
xmlParseXMLCatalogNode(xmlNodePtr cur, xmlCatalogPrefer prefer,
	               xmlCatalogEntryPtr parent, xmlCatalogEntryPtr cgroup)
{
    xmlChar *base = NULL;
    xmlCatalogEntryPtr entry = NULL;

    if (cur == NULL)
        return;
    if (xmlStrEqual(cur->name, BAD_CAST "group")) {
        xmlChar *prop;
	xmlCatalogPrefer pref = XML_CATA_PREFER_NONE;

        prop = xmlGetProp(cur, BAD_CAST "prefer");
        if (prop != NULL) {
            if (xmlStrEqual(prop, BAD_CAST "system")) {
                prefer = XML_CATA_PREFER_SYSTEM;
            } else if (xmlStrEqual(prop, BAD_CAST "public")) {
                prefer = XML_CATA_PREFER_PUBLIC;
            } else {
		xmlCatalogErr(parent, cur, XML_CATALOG_PREFER_VALUE,
                              "Invalid value for prefer: '%s'\n",
			      prop, NULL, NULL);
            }
            xmlFree(prop);
	    pref = prefer;
        }
	prop = xmlGetProp(cur, BAD_CAST "id");
	base = xmlGetNsProp(cur, BAD_CAST "base", XML_XML_NAMESPACE);
	entry = xmlNewCatalogEntry(XML_CATA_GROUP, prop, base, NULL, pref, cgroup);
	xmlFree(prop);
    } else if (xmlStrEqual(cur->name, BAD_CAST "public")) {
	entry = xmlParseXMLCatalogOneNode(cur, XML_CATA_PUBLIC,
		BAD_CAST "public", BAD_CAST "publicId", BAD_CAST "uri", prefer, cgroup);
    } else if (xmlStrEqual(cur->name, BAD_CAST "system")) {
	entry = xmlParseXMLCatalogOneNode(cur, XML_CATA_SYSTEM,
		BAD_CAST "system", BAD_CAST "systemId", BAD_CAST "uri", prefer, cgroup);
    } else if (xmlStrEqual(cur->name, BAD_CAST "rewriteSystem")) {
	entry = xmlParseXMLCatalogOneNode(cur, XML_CATA_REWRITE_SYSTEM,
		BAD_CAST "rewriteSystem", BAD_CAST "systemIdStartString",
		BAD_CAST "rewritePrefix", prefer, cgroup);
    } else if (xmlStrEqual(cur->name, BAD_CAST "delegatePublic")) {
	entry = xmlParseXMLCatalogOneNode(cur, XML_CATA_DELEGATE_PUBLIC,
		BAD_CAST "delegatePublic", BAD_CAST "publicIdStartString",
		BAD_CAST "catalog", prefer, cgroup);
    } else if (xmlStrEqual(cur->name, BAD_CAST "delegateSystem")) {
	entry = xmlParseXMLCatalogOneNode(cur, XML_CATA_DELEGATE_SYSTEM,
		BAD_CAST "delegateSystem", BAD_CAST "systemIdStartString",
		BAD_CAST "catalog", prefer, cgroup);
    } else if (xmlStrEqual(cur->name, BAD_CAST "uri")) {
	entry = xmlParseXMLCatalogOneNode(cur, XML_CATA_URI,
		BAD_CAST "uri", BAD_CAST "name",
		BAD_CAST "uri", prefer, cgroup);
    } else if (xmlStrEqual(cur->name, BAD_CAST "rewriteURI")) {
	entry = xmlParseXMLCatalogOneNode(cur, XML_CATA_REWRITE_URI,
		BAD_CAST "rewriteURI", BAD_CAST "uriStartString",
		BAD_CAST "rewritePrefix", prefer, cgroup);
    } else if (xmlStrEqual(cur->name, BAD_CAST "delegateURI")) {
	entry = xmlParseXMLCatalogOneNode(cur, XML_CATA_DELEGATE_URI,
		BAD_CAST "delegateURI", BAD_CAST "uriStartString",
		BAD_CAST "catalog", prefer, cgroup);
    } else if (xmlStrEqual(cur->name, BAD_CAST "nextCatalog")) {
	entry = xmlParseXMLCatalogOneNode(cur, XML_CATA_NEXT_CATALOG,
		BAD_CAST "nextCatalog", NULL,
		BAD_CAST "catalog", prefer, cgroup);
    }
    if (entry != NULL) {
        if (parent != NULL) {
	    entry->parent = parent;
	    if (parent->children == NULL)
		parent->children = entry;
	    else {
		xmlCatalogEntryPtr prev;

		prev = parent->children;
		while (prev->next != NULL)
		    prev = prev->next;
		prev->next = entry;
	    }
	}
	if (entry->type == XML_CATA_GROUP) {
	    /*
	     * Recurse to propagate prefer to the subtree
	     * (xml:base handling is automated)
	     */
            xmlParseXMLCatalogNodeList(cur->children, prefer, parent, entry);
	}
    }
    if (base != NULL)
	xmlFree(base);
}

/**
 * Examines a list of XML sibling nodes of a catalog and build
 * a list of Catalog entry from it adding it to the parent.
 * The examination will recurse to examine node subtrees.
 *
 * @param cur  the XML node list of siblings
 * @param prefer  the PUBLIC vs. SYSTEM current preference value
 * @param parent  the parent Catalog entry
 * @param cgroup  the group which includes this list
 */
static void
xmlParseXMLCatalogNodeList(xmlNodePtr cur, xmlCatalogPrefer prefer,
	                   xmlCatalogEntryPtr parent, xmlCatalogEntryPtr cgroup) {
    while (cur != NULL) {
	if ((cur->ns != NULL) && (cur->ns->href != NULL) &&
	    (xmlStrEqual(cur->ns->href, XML_CATALOGS_NAMESPACE))) {
	    xmlParseXMLCatalogNode(cur, prefer, parent, cgroup);
	}
	cur = cur->next;
    }
    /* TODO: sort the list according to REWRITE lengths and prefer value */
}

/**
 * Parses the catalog file to extract the XML tree and then analyze the
 * tree to build a list of Catalog entries corresponding to this catalog
 *
 * @param prefer  the PUBLIC vs. SYSTEM current preference value
 * @param filename  the filename for the catalog
 * @returns the resulting Catalog entries list
 */
static xmlCatalogEntryPtr
xmlParseXMLCatalogFile(xmlCatalogPrefer prefer, const xmlChar *filename) {
    xmlDocPtr doc;
    xmlNodePtr cur;
    xmlChar *prop;
    xmlCatalogEntryPtr parent = NULL;

    if (filename == NULL)
        return(NULL);

    doc = xmlParseCatalogFile((const char *) filename);
    if (doc == NULL) {
	if (xmlDebugCatalogs)
	    xmlCatalogPrintDebug(
		    "Failed to parse catalog %s\n", filename);
	return(NULL);
    }

    if (xmlDebugCatalogs)
	xmlCatalogPrintDebug(
		"Parsing catalog %s\n", filename);

    cur = xmlDocGetRootElement(doc);
    if ((cur != NULL) && (xmlStrEqual(cur->name, BAD_CAST "catalog")) &&
	(cur->ns != NULL) && (cur->ns->href != NULL) &&
	(xmlStrEqual(cur->ns->href, XML_CATALOGS_NAMESPACE))) {

	parent = xmlNewCatalogEntry(XML_CATA_CATALOG, NULL,
				    (const xmlChar *)filename, NULL, prefer, NULL);
        if (parent == NULL) {
	    xmlFreeDoc(doc);
	    return(NULL);
	}

	prop = xmlGetProp(cur, BAD_CAST "prefer");
	if (prop != NULL) {
	    if (xmlStrEqual(prop, BAD_CAST "system")) {
		prefer = XML_CATA_PREFER_SYSTEM;
	    } else if (xmlStrEqual(prop, BAD_CAST "public")) {
		prefer = XML_CATA_PREFER_PUBLIC;
	    } else {
		xmlCatalogErr(NULL, cur, XML_CATALOG_PREFER_VALUE,
			      "Invalid value for prefer: '%s'\n",
			      prop, NULL, NULL);
	    }
	    xmlFree(prop);
	}
	cur = cur->children;
	xmlParseXMLCatalogNodeList(cur, prefer, parent, NULL);
    } else {
	xmlCatalogErr(NULL, (xmlNodePtr) doc, XML_CATALOG_NOT_CATALOG,
		      "File %s is not an XML Catalog\n",
		      filename, NULL, NULL);
	xmlFreeDoc(doc);
	return(NULL);
    }
    xmlFreeDoc(doc);
    return(parent);
}

/**
 * Fetch and parse the subcatalog referenced by an entry
 *
 * @param catal  an existing but incomplete catalog entry
 * @returns 0 in case of success, -1 otherwise
 */
static int
xmlFetchXMLCatalogFile(xmlCatalogEntryPtr catal) {
    xmlCatalogEntryPtr doc;

    if (catal == NULL)
	return(-1);
    if (catal->URL == NULL)
	return(-1);

    /*
     * lock the whole catalog for modification
     */
    xmlRMutexLock(&xmlCatalogMutex);
    if (catal->children != NULL) {
	/* Okay someone else did it in the meantime */
	xmlRMutexUnlock(&xmlCatalogMutex);
	return(0);
    }

    if (xmlCatalogXMLFiles != NULL) {
	doc = (xmlCatalogEntryPtr)
	    xmlHashLookup(xmlCatalogXMLFiles, catal->URL);
	if (doc != NULL) {
	    if (xmlDebugCatalogs)
		xmlCatalogPrintDebug(
		    "Found %s in file hash\n", catal->URL);

	    if (catal->type == XML_CATA_CATALOG)
		catal->children = doc->children;
	    else
		catal->children = doc;
	    catal->dealloc = 0;
	    xmlRMutexUnlock(&xmlCatalogMutex);
	    return(0);
	}
	if (xmlDebugCatalogs)
	    xmlCatalogPrintDebug(
		"%s not found in file hash\n", catal->URL);
    }

    /*
     * Fetch and parse. Note that xmlParseXMLCatalogFile does not
     * use the existing catalog, there is no recursion allowed at
     * that level.
     */
    doc = xmlParseXMLCatalogFile(catal->prefer, catal->URL);
    if (doc == NULL) {
	catal->type = XML_CATA_BROKEN_CATALOG;
	xmlRMutexUnlock(&xmlCatalogMutex);
	return(-1);
    }

    if (catal->type == XML_CATA_CATALOG)
	catal->children = doc->children;
    else
	catal->children = doc;

    doc->dealloc = 1;

    if (xmlCatalogXMLFiles == NULL)
	xmlCatalogXMLFiles = xmlHashCreate(10);
    if (xmlCatalogXMLFiles != NULL) {
	if (xmlDebugCatalogs)
	    xmlCatalogPrintDebug(
		"%s added to file hash\n", catal->URL);
	xmlHashAddEntry(xmlCatalogXMLFiles, catal->URL, doc);
    }
    xmlRMutexUnlock(&xmlCatalogMutex);
    return(0);
}

/************************************************************************
 *									*
 *			XML Catalog handling				*
 *									*
 ************************************************************************/

/**
 * Add an entry in the XML catalog, it may overwrite existing but
 * different entries.
 *
 * @param catal  top of an XML catalog
 * @param type  the type of record to add to the catalog
 * @param orig  the system, public or prefix to match (or NULL)
 * @param replace  the replacement value for the match
 * @returns 0 if successful, -1 otherwise
 */
static int
xmlAddXMLCatalog(xmlCatalogEntryPtr catal, const xmlChar *type,
	      const xmlChar *orig, const xmlChar *replace) {
    xmlCatalogEntryPtr cur;
    xmlCatalogEntryType typ;
    int doregister = 0;

    if ((catal == NULL) ||
	((catal->type != XML_CATA_CATALOG) &&
	 (catal->type != XML_CATA_BROKEN_CATALOG)))
	return(-1);
    if (catal->children == NULL) {
	xmlFetchXMLCatalogFile(catal);
    }
    if (catal->children == NULL)
	doregister = 1;

    typ = xmlGetXMLCatalogEntryType(type);
    if (typ == XML_CATA_NONE) {
	if (xmlDebugCatalogs)
	    xmlCatalogPrintDebug(
		    "Failed to add unknown element %s to catalog\n", type);
	return(-1);
    }

    cur = catal->children;
    /*
     * Might be a simple "update in place"
     */
    if (cur != NULL) {
	while (cur != NULL) {
	    if ((orig != NULL) && (cur->type == typ) &&
		(xmlStrEqual(orig, cur->name))) {
		if (xmlDebugCatalogs)
		    xmlCatalogPrintDebug(
			    "Updating element %s to catalog\n", type);
		if (cur->value != NULL)
		    xmlFree(cur->value);
		if (cur->URL != NULL)
		    xmlFree(cur->URL);
		cur->value = xmlStrdup(replace);
		cur->URL = xmlStrdup(replace);
		return(0);
	    }
	    if (cur->next == NULL)
		break;
	    cur = cur->next;
	}
    }
    if (xmlDebugCatalogs)
	xmlCatalogPrintDebug(
		"Adding element %s to catalog\n", type);
    if (cur == NULL)
	catal->children = xmlNewCatalogEntry(typ, orig, replace,
		                             NULL, catal->prefer, NULL);
    else
	cur->next = xmlNewCatalogEntry(typ, orig, replace,
		                       NULL, catal->prefer, NULL);
    if (doregister) {
        catal->type = XML_CATA_CATALOG;
	cur = (xmlCatalogEntryPtr)xmlHashLookup(xmlCatalogXMLFiles, catal->URL);
	if (cur != NULL)
	    cur->children = catal->children;
    }

    return(0);
}

/**
 * Remove entries in the XML catalog where the value or the URI
 * is equal to `value`
 *
 * @param catal  top of an XML catalog
 * @param value  the value to remove from the catalog
 * @returns the number of entries removed if successful, -1 otherwise
 */
static int
xmlDelXMLCatalog(xmlCatalogEntryPtr catal, const xmlChar *value) {
    xmlCatalogEntryPtr cur;
    int ret = 0;

    if ((catal == NULL) ||
	((catal->type != XML_CATA_CATALOG) &&
	 (catal->type != XML_CATA_BROKEN_CATALOG)))
	return(-1);
    if (value == NULL)
	return(-1);
    if (catal->children == NULL) {
	xmlFetchXMLCatalogFile(catal);
    }

    /*
     * Scan the children
     */
    cur = catal->children;
    while (cur != NULL) {
	if (((cur->name != NULL) && (xmlStrEqual(value, cur->name))) ||
	    (xmlStrEqual(value, cur->value))) {
	    if (xmlDebugCatalogs) {
		if (cur->name != NULL)
		    xmlCatalogPrintDebug(
			    "Removing element %s from catalog\n", cur->name);
		else
		    xmlCatalogPrintDebug(
			    "Removing element %s from catalog\n", cur->value);
	    }
	    cur->type = XML_CATA_REMOVED;
	}
	cur = cur->next;
    }
    return(ret);
}

/**
 * Do a complete resolution lookup of an External Identifier for a
 * list of catalog entries.
 *
 * Implements (or tries to) 7.1. External Identifier Resolution
 * from http://www.oasis-open.org/committees/entity/spec-2001-08-06.html
 *
 * @param catal  a catalog list
 * @param pubID  the public ID string
 * @param sysID  the system ID string
 * @returns the URI of the resource or NULL if not found
 */
static xmlChar *
xmlCatalogXMLResolve(xmlCatalogEntryPtr catal, const xmlChar *pubID,
	              const xmlChar *sysID) {
    xmlChar *ret = NULL;
    xmlCatalogEntryPtr cur;
    int haveDelegate = 0;
    int haveNext = 0;

    /*
     * protection against loops
     */
    if (catal->depth > MAX_CATAL_DEPTH) {
	xmlCatalogErr(catal, NULL, XML_CATALOG_RECURSION,
		      "Detected recursion in catalog %s\n",
		      catal->name, NULL, NULL);
	return(NULL);
    }
    catal->depth++;

    /*
     * First tries steps 2/ 3/ 4/ if a system ID is provided.
     */
    if (sysID != NULL) {
	xmlCatalogEntryPtr rewrite = NULL;
	int lenrewrite = 0, len;
	cur = catal;
	haveDelegate = 0;
	while (cur != NULL) {
	    switch (cur->type) {
		case XML_CATA_SYSTEM:
		    if (xmlStrEqual(sysID, cur->name)) {
			if (xmlDebugCatalogs)
			    xmlCatalogPrintDebug(
				    "Found system match %s, using %s\n",
				            cur->name, cur->URL);
			catal->depth--;
			return(xmlStrdup(cur->URL));
		    }
		    break;
		case XML_CATA_REWRITE_SYSTEM:
		    len = xmlStrlen(cur->name);
		    if ((len > lenrewrite) &&
			(!xmlStrncmp(sysID, cur->name, len))) {
			lenrewrite = len;
			rewrite = cur;
		    }
		    break;
		case XML_CATA_DELEGATE_SYSTEM:
		    if (!xmlStrncmp(sysID, cur->name, xmlStrlen(cur->name)))
			haveDelegate++;
		    break;
		case XML_CATA_NEXT_CATALOG:
		    haveNext++;
		    break;
		default:
		    break;
	    }
	    cur = cur->next;
	}
	if (rewrite != NULL) {
	    if (xmlDebugCatalogs)
		xmlCatalogPrintDebug(
			"Using rewriting rule %s\n", rewrite->name);
	    ret = xmlStrdup(rewrite->URL);
	    if (ret != NULL)
		ret = xmlStrcat(ret, &sysID[lenrewrite]);
	    catal->depth--;
	    return(ret);
	}
	if (haveDelegate) {
	    const xmlChar *delegates[MAX_DELEGATE];
	    int nbList = 0, i;

	    /*
	     * Assume the entries have been sorted by decreasing substring
	     * matches when the list was produced.
	     */
	    cur = catal;
	    while (cur != NULL) {
		if ((cur->type == XML_CATA_DELEGATE_SYSTEM) &&
		    (!xmlStrncmp(sysID, cur->name, xmlStrlen(cur->name)))) {
		    for (i = 0;i < nbList;i++)
			if (xmlStrEqual(cur->URL, delegates[i]))
			    break;
		    if (i < nbList) {
			cur = cur->next;
			continue;
		    }
		    if (nbList < MAX_DELEGATE)
			delegates[nbList++] = cur->URL;

		    if (cur->children == NULL) {
			xmlFetchXMLCatalogFile(cur);
		    }
		    if (cur->children != NULL) {
			if (xmlDebugCatalogs)
			    xmlCatalogPrintDebug(
				    "Trying system delegate %s\n", cur->URL);
			ret = xmlCatalogListXMLResolve(
				cur->children, NULL, sysID);
			if (ret != NULL) {
			    catal->depth--;
			    return(ret);
			}
		    }
		}
		cur = cur->next;
	    }
	    /*
	     * Apply the cut algorithm explained in 4/
	     */
	    catal->depth--;
	    return(XML_CATAL_BREAK);
	}
    }
    /*
     * Then tries 5/ 6/ if a public ID is provided
     */
    if (pubID != NULL) {
	cur = catal;
	haveDelegate = 0;
	while (cur != NULL) {
	    switch (cur->type) {
		case XML_CATA_PUBLIC:
		    if (xmlStrEqual(pubID, cur->name)) {
			if (xmlDebugCatalogs)
			    xmlCatalogPrintDebug(
				    "Found public match %s\n", cur->name);
			catal->depth--;
			return(xmlStrdup(cur->URL));
		    }
		    break;
		case XML_CATA_DELEGATE_PUBLIC:
		    if (!xmlStrncmp(pubID, cur->name, xmlStrlen(cur->name)) &&
			(cur->prefer == XML_CATA_PREFER_PUBLIC))
			haveDelegate++;
		    break;
		case XML_CATA_NEXT_CATALOG:
		    if (sysID == NULL)
			haveNext++;
		    break;
		default:
		    break;
	    }
	    cur = cur->next;
	}
	if (haveDelegate) {
	    const xmlChar *delegates[MAX_DELEGATE];
	    int nbList = 0, i;

	    /*
	     * Assume the entries have been sorted by decreasing substring
	     * matches when the list was produced.
	     */
	    cur = catal;
	    while (cur != NULL) {
		if ((cur->type == XML_CATA_DELEGATE_PUBLIC) &&
		    (cur->prefer == XML_CATA_PREFER_PUBLIC) &&
		    (!xmlStrncmp(pubID, cur->name, xmlStrlen(cur->name)))) {

		    for (i = 0;i < nbList;i++)
			if (xmlStrEqual(cur->URL, delegates[i]))
			    break;
		    if (i < nbList) {
			cur = cur->next;
			continue;
		    }
		    if (nbList < MAX_DELEGATE)
			delegates[nbList++] = cur->URL;

		    if (cur->children == NULL) {
			xmlFetchXMLCatalogFile(cur);
		    }
		    if (cur->children != NULL) {
			if (xmlDebugCatalogs)
			    xmlCatalogPrintDebug(
				    "Trying public delegate %s\n", cur->URL);
			ret = xmlCatalogListXMLResolve(
				cur->children, pubID, NULL);
			if (ret != NULL) {
			    catal->depth--;
			    return(ret);
			}
		    }
		}
		cur = cur->next;
	    }
	    /*
	     * Apply the cut algorithm explained in 4/
	     */
	    catal->depth--;
	    return(XML_CATAL_BREAK);
	}
    }
    if (haveNext) {
	cur = catal;
	while (cur != NULL) {
	    if (cur->type == XML_CATA_NEXT_CATALOG) {
		if (cur->children == NULL) {
		    xmlFetchXMLCatalogFile(cur);
		}
		if (cur->children != NULL) {
		    ret = xmlCatalogListXMLResolve(cur->children, pubID, sysID);
		    if (ret != NULL) {
			catal->depth--;
			return(ret);
		    } else if (catal->depth > MAX_CATAL_DEPTH) {
		        return(NULL);
		    }
		}
	    }
	    cur = cur->next;
	}
    }

    catal->depth--;
    return(NULL);
}

/**
 * Do a complete resolution lookup of an External Identifier for a
 * list of catalog entries.
 *
 * Implements (or tries to) 7.2.2. URI Resolution
 * from http://www.oasis-open.org/committees/entity/spec-2001-08-06.html
 *
 * @param catal  a catalog list
 * @param URI  the URI
 * @returns the URI of the resource or NULL if not found
 */
static xmlChar *
xmlCatalogXMLResolveURI(xmlCatalogEntryPtr catal, const xmlChar *URI) {
    xmlChar *ret = NULL;
    xmlCatalogEntryPtr cur;
    int haveDelegate = 0;
    int haveNext = 0;
    xmlCatalogEntryPtr rewrite = NULL;
    int lenrewrite = 0, len;

    if (catal == NULL)
	return(NULL);

    if (URI == NULL)
	return(NULL);

    if (catal->depth > MAX_CATAL_DEPTH) {
	xmlCatalogErr(catal, NULL, XML_CATALOG_RECURSION,
		      "Detected recursion in catalog %s\n",
		      catal->name, NULL, NULL);
	return(NULL);
    }

    /*
     * First tries steps 2/ 3/ 4/ if a system ID is provided.
     */
    cur = catal;
    haveDelegate = 0;
    while (cur != NULL) {
	switch (cur->type) {
	    case XML_CATA_URI:
		if (xmlStrEqual(URI, cur->name)) {
		    if (xmlDebugCatalogs)
			xmlCatalogPrintDebug(
				"Found URI match %s\n", cur->name);
		    return(xmlStrdup(cur->URL));
		}
		break;
	    case XML_CATA_REWRITE_URI:
		len = xmlStrlen(cur->name);
		if ((len > lenrewrite) &&
		    (!xmlStrncmp(URI, cur->name, len))) {
		    lenrewrite = len;
		    rewrite = cur;
		}
		break;
	    case XML_CATA_DELEGATE_URI:
		if (!xmlStrncmp(URI, cur->name, xmlStrlen(cur->name)))
		    haveDelegate++;
		break;
	    case XML_CATA_NEXT_CATALOG:
		haveNext++;
		break;
	    default:
		break;
	}
	cur = cur->next;
    }
    if (rewrite != NULL) {
	if (xmlDebugCatalogs)
	    xmlCatalogPrintDebug(
		    "Using rewriting rule %s\n", rewrite->name);
	ret = xmlStrdup(rewrite->URL);
	if (ret != NULL)
	    ret = xmlStrcat(ret, &URI[lenrewrite]);
	return(ret);
    }
    if (haveDelegate) {
	const xmlChar *delegates[MAX_DELEGATE];
	int nbList = 0, i;

	/*
	 * Assume the entries have been sorted by decreasing substring
	 * matches when the list was produced.
	 */
	cur = catal;
	while (cur != NULL) {
	    if (((cur->type == XML_CATA_DELEGATE_SYSTEM) ||
	         (cur->type == XML_CATA_DELEGATE_URI)) &&
		(!xmlStrncmp(URI, cur->name, xmlStrlen(cur->name)))) {
		for (i = 0;i < nbList;i++)
		    if (xmlStrEqual(cur->URL, delegates[i]))
			break;
		if (i < nbList) {
		    cur = cur->next;
		    continue;
		}
		if (nbList < MAX_DELEGATE)
		    delegates[nbList++] = cur->URL;

		if (cur->children == NULL) {
		    xmlFetchXMLCatalogFile(cur);
		}
		if (cur->children != NULL) {
		    if (xmlDebugCatalogs)
			xmlCatalogPrintDebug(
				"Trying URI delegate %s\n", cur->URL);
		    ret = xmlCatalogListXMLResolveURI(
			    cur->children, URI);
		    if (ret != NULL)
			return(ret);
		}
	    }
	    cur = cur->next;
	}
	/*
	 * Apply the cut algorithm explained in 4/
	 */
	return(XML_CATAL_BREAK);
    }
    if (haveNext) {
	cur = catal;
	while (cur != NULL) {
	    if (cur->type == XML_CATA_NEXT_CATALOG) {
		if (cur->children == NULL) {
		    xmlFetchXMLCatalogFile(cur);
		}
		if (cur->children != NULL) {
		    ret = xmlCatalogListXMLResolveURI(cur->children, URI);
		    if (ret != NULL)
			return(ret);
		}
	    }
	    cur = cur->next;
	}
    }

    return(NULL);
}

/**
 * Do a complete resolution lookup of an External Identifier for a
 * list of catalogs
 *
 * Implements (or tries to) 7.1. External Identifier Resolution
 * from http://www.oasis-open.org/committees/entity/spec-2001-08-06.html
 *
 * @param catal  a catalog list
 * @param pubID  the public ID string
 * @param sysID  the system ID string
 * @returns the URI of the resource or NULL if not found
 */
static xmlChar *
xmlCatalogListXMLResolve(xmlCatalogEntryPtr catal, const xmlChar *pubID,
	              const xmlChar *sysID) {
    xmlChar *ret = NULL;
    xmlChar *urnID = NULL;
    xmlChar *normid;

    if (catal == NULL)
        return(NULL);
    if ((pubID == NULL) && (sysID == NULL))
	return(NULL);

    normid = xmlCatalogNormalizePublic(pubID);
    if (normid != NULL)
        pubID = (*normid != 0 ? normid : NULL);

    if (!xmlStrncmp(pubID, BAD_CAST XML_URN_PUBID, sizeof(XML_URN_PUBID) - 1)) {
	urnID = xmlCatalogUnWrapURN(pubID);
	if (xmlDebugCatalogs) {
	    if (urnID == NULL)
		xmlCatalogPrintDebug(
			"Public URN ID %s expanded to NULL\n", pubID);
	    else
		xmlCatalogPrintDebug(
			"Public URN ID expanded to %s\n", urnID);
	}
	ret = xmlCatalogListXMLResolve(catal, urnID, sysID);
	if (urnID != NULL)
	    xmlFree(urnID);
	if (normid != NULL)
	    xmlFree(normid);
	return(ret);
    }
    if (!xmlStrncmp(sysID, BAD_CAST XML_URN_PUBID, sizeof(XML_URN_PUBID) - 1)) {
	urnID = xmlCatalogUnWrapURN(sysID);
	if (xmlDebugCatalogs) {
	    if (urnID == NULL)
		xmlCatalogPrintDebug(
			"System URN ID %s expanded to NULL\n", sysID);
	    else
		xmlCatalogPrintDebug(
			"System URN ID expanded to %s\n", urnID);
	}
	if (pubID == NULL)
	    ret = xmlCatalogListXMLResolve(catal, urnID, NULL);
	else if (xmlStrEqual(pubID, urnID))
	    ret = xmlCatalogListXMLResolve(catal, pubID, NULL);
	else {
	    ret = xmlCatalogListXMLResolve(catal, pubID, urnID);
	}
	if (urnID != NULL)
	    xmlFree(urnID);
	if (normid != NULL)
	    xmlFree(normid);
	return(ret);
    }
    while (catal != NULL) {
	if (catal->type == XML_CATA_CATALOG) {
	    if (catal->children == NULL) {
		xmlFetchXMLCatalogFile(catal);
	    }
	    if (catal->children != NULL) {
		ret = xmlCatalogXMLResolve(catal->children, pubID, sysID);
		if (ret != NULL) {
		    break;
                } else if (catal->children->depth > MAX_CATAL_DEPTH) {
	            ret = NULL;
		    break;
	        }
	    }
	}
	catal = catal->next;
    }
    if (normid != NULL)
	xmlFree(normid);
    return(ret);
}

/**
 * Do a complete resolution lookup of an URI for a list of catalogs
 *
 * Implements (or tries to) 7.2. URI Resolution
 * from http://www.oasis-open.org/committees/entity/spec-2001-08-06.html
 *
 * @param catal  a catalog list
 * @param URI  the URI
 * @returns the URI of the resource or NULL if not found
 */
static xmlChar *
xmlCatalogListXMLResolveURI(xmlCatalogEntryPtr catal, const xmlChar *URI) {
    xmlChar *ret = NULL;
    xmlChar *urnID = NULL;

    if (catal == NULL)
        return(NULL);
    if (URI == NULL)
	return(NULL);

    if (!xmlStrncmp(URI, BAD_CAST XML_URN_PUBID, sizeof(XML_URN_PUBID) - 1)) {
	urnID = xmlCatalogUnWrapURN(URI);
	if (xmlDebugCatalogs) {
	    if (urnID == NULL)
		xmlCatalogPrintDebug(
			"URN ID %s expanded to NULL\n", URI);
	    else
		xmlCatalogPrintDebug(
			"URN ID expanded to %s\n", urnID);
	}
	ret = xmlCatalogListXMLResolve(catal, urnID, NULL);
	if (urnID != NULL)
	    xmlFree(urnID);
	return(ret);
    }
    while (catal != NULL) {
	if (catal->type == XML_CATA_CATALOG) {
	    if (catal->children == NULL) {
		xmlFetchXMLCatalogFile(catal);
	    }
	    if (catal->children != NULL) {
		ret = xmlCatalogXMLResolveURI(catal->children, URI);
		if (ret != NULL)
		    return(ret);
	    }
	}
	catal = catal->next;
    }
    return(ret);
}

#ifdef LIBXML_SGML_CATALOG_ENABLED

/************************************************************************
 *									*
 *			The SGML Catalog parser				*
 *									*
 ************************************************************************/


#define RAW *cur
#define NEXT cur++;
#define SKIP(x) cur += x;

#define SKIP_BLANKS while (IS_BLANK_CH(*cur)) NEXT;

/**
 * Skip a comment in an SGML catalog
 *
 * @param cur  the current character
 * @returns new current character
 */
static const xmlChar *
xmlParseSGMLCatalogComment(const xmlChar *cur) {
    if ((cur[0] != '-') || (cur[1] != '-'))
	return(cur);
    SKIP(2);
    while ((cur[0] != 0) && ((cur[0] != '-') || ((cur[1] != '-'))))
	NEXT;
    if (cur[0] == 0) {
	return(NULL);
    }
    return(cur + 2);
}

/**
 * Parse an SGML catalog ID
 *
 * @param cur  the current character
 * @param id  the return location
 * @returns new current character and store the value in `id`
 */
static const xmlChar *
xmlParseSGMLCatalogPubid(const xmlChar *cur, xmlChar **id) {
    xmlChar *buf = NULL;
    int len = 0;
    int size = 50;
    xmlChar stop;

    *id = NULL;

    if (RAW == '"') {
        NEXT;
	stop = '"';
    } else if (RAW == '\'') {
        NEXT;
	stop = '\'';
    } else {
	stop = ' ';
    }
    buf = xmlMalloc(size);
    if (buf == NULL) {
        xmlCatalogErrMemory();
	return(NULL);
    }
    while (IS_PUBIDCHAR_CH(*cur) || (*cur == '?')) {
	if ((*cur == stop) && (stop != ' '))
	    break;
	if ((stop == ' ') && (IS_BLANK_CH(*cur)))
	    break;
	if (len + 1 >= size) {
            xmlChar *tmp;
            int newSize;

            newSize = xmlGrowCapacity(size, 1, 1, XML_MAX_ITEMS);
            if (newSize < 0) {
		xmlCatalogErrMemory();
		xmlFree(buf);
		return(NULL);
            }
	    tmp = xmlRealloc(buf, newSize);
	    if (tmp == NULL) {
		xmlCatalogErrMemory();
		xmlFree(buf);
		return(NULL);
	    }
	    buf = tmp;
            size = newSize;
	}
	buf[len++] = *cur;
	NEXT;
    }
    buf[len] = 0;
    if (stop == ' ') {
	if (!IS_BLANK_CH(*cur)) {
	    xmlFree(buf);
	    return(NULL);
	}
    } else {
	if (*cur != stop) {
	    xmlFree(buf);
	    return(NULL);
	}
	NEXT;
    }
    *id = buf;
    return(cur);
}

/**
 * Parse an SGML catalog name
 *
 * @param cur  the current character
 * @param name  the return location
 * @returns new current character and store the value in `name`
 */
static const xmlChar *
xmlParseSGMLCatalogName(const xmlChar *cur, xmlChar **name) {
    xmlChar buf[XML_MAX_NAMELEN + 5];
    int len = 0;
    int c;

    *name = NULL;

    /*
     * Handler for more complex cases
     */
    c = *cur;
    if ((!IS_LETTER(c) && (c != '_') && (c != ':'))) {
	return(NULL);
    }

    while (((IS_LETTER(c)) || (IS_DIGIT(c)) ||
            (c == '.') || (c == '-') ||
	    (c == '_') || (c == ':'))) {
	buf[len++] = c;
	cur++;
	c = *cur;
	if (len >= XML_MAX_NAMELEN)
	    return(NULL);
    }
    *name = xmlStrndup(buf, len);
    return(cur);
}

/**
 * Get the Catalog entry type for a given SGML Catalog name
 *
 * @param name  the entry name
 * @returns Catalog entry type
 */
static xmlCatalogEntryType
xmlGetSGMLCatalogEntryType(const xmlChar *name) {
    xmlCatalogEntryType type = XML_CATA_NONE;
    if (xmlStrEqual(name, (const xmlChar *) "SYSTEM"))
	type = SGML_CATA_SYSTEM;
    else if (xmlStrEqual(name, (const xmlChar *) "PUBLIC"))
	type = SGML_CATA_PUBLIC;
    else if (xmlStrEqual(name, (const xmlChar *) "DELEGATE"))
	type = SGML_CATA_DELEGATE;
    else if (xmlStrEqual(name, (const xmlChar *) "ENTITY"))
	type = SGML_CATA_ENTITY;
    else if (xmlStrEqual(name, (const xmlChar *) "DOCTYPE"))
	type = SGML_CATA_DOCTYPE;
    else if (xmlStrEqual(name, (const xmlChar *) "LINKTYPE"))
	type = SGML_CATA_LINKTYPE;
    else if (xmlStrEqual(name, (const xmlChar *) "NOTATION"))
	type = SGML_CATA_NOTATION;
    else if (xmlStrEqual(name, (const xmlChar *) "SGMLDECL"))
	type = SGML_CATA_SGMLDECL;
    else if (xmlStrEqual(name, (const xmlChar *) "DOCUMENT"))
	type = SGML_CATA_DOCUMENT;
    else if (xmlStrEqual(name, (const xmlChar *) "CATALOG"))
	type = SGML_CATA_CATALOG;
    else if (xmlStrEqual(name, (const xmlChar *) "BASE"))
	type = SGML_CATA_BASE;
    return(type);
}

/**
 * Parse an SGML catalog content and fill up the `catal` hash table with
 * the new entries found.
 *
 * @param catal  the SGML Catalog
 * @param value  the content of the SGML Catalog serialization
 * @param file  the filepath for the catalog
 * @param super  should this be handled as a Super Catalog in which case
 *          parsing is not recursive
 * @returns 0 in case of success, -1 in case of error.
 */
static int
xmlParseSGMLCatalog(xmlCatalogPtr catal, const xmlChar *value,
	            const char *file, int super) {
    const xmlChar *cur = value;
    xmlChar *base = NULL;
    int res;

    if ((cur == NULL) || (file == NULL))
        return(-1);
    base = xmlStrdup((const xmlChar *) file);

    while ((cur != NULL) && (cur[0] != 0)) {
	SKIP_BLANKS;
	if (cur[0] == 0)
	    break;
	if ((cur[0] == '-') && (cur[1] == '-')) {
	    cur = xmlParseSGMLCatalogComment(cur);
	    if (cur == NULL) {
		/* error */
		break;
	    }
	} else {
	    xmlChar *sysid = NULL;
	    xmlChar *name = NULL;
	    xmlCatalogEntryType type = XML_CATA_NONE;

	    cur = xmlParseSGMLCatalogName(cur, &name);
	    if (cur == NULL || name == NULL) {
		/* error */
		break;
	    }
	    if (!IS_BLANK_CH(*cur)) {
		/* error */
		xmlFree(name);
		break;
	    }
	    SKIP_BLANKS;
	    if (xmlStrEqual(name, (const xmlChar *) "SYSTEM"))
                type = SGML_CATA_SYSTEM;
	    else if (xmlStrEqual(name, (const xmlChar *) "PUBLIC"))
                type = SGML_CATA_PUBLIC;
	    else if (xmlStrEqual(name, (const xmlChar *) "DELEGATE"))
                type = SGML_CATA_DELEGATE;
	    else if (xmlStrEqual(name, (const xmlChar *) "ENTITY"))
                type = SGML_CATA_ENTITY;
	    else if (xmlStrEqual(name, (const xmlChar *) "DOCTYPE"))
                type = SGML_CATA_DOCTYPE;
	    else if (xmlStrEqual(name, (const xmlChar *) "LINKTYPE"))
                type = SGML_CATA_LINKTYPE;
	    else if (xmlStrEqual(name, (const xmlChar *) "NOTATION"))
                type = SGML_CATA_NOTATION;
	    else if (xmlStrEqual(name, (const xmlChar *) "SGMLDECL"))
                type = SGML_CATA_SGMLDECL;
	    else if (xmlStrEqual(name, (const xmlChar *) "DOCUMENT"))
                type = SGML_CATA_DOCUMENT;
	    else if (xmlStrEqual(name, (const xmlChar *) "CATALOG"))
                type = SGML_CATA_CATALOG;
	    else if (xmlStrEqual(name, (const xmlChar *) "BASE"))
                type = SGML_CATA_BASE;
	    else if (xmlStrEqual(name, (const xmlChar *) "OVERRIDE")) {
		xmlFree(name);
		cur = xmlParseSGMLCatalogName(cur, &name);
		if (name == NULL) {
		    /* error */
		    break;
		}
		xmlFree(name);
		continue;
	    }
	    xmlFree(name);
	    name = NULL;

	    switch(type) {
		case SGML_CATA_ENTITY:
		    if (*cur == '%')
			type = SGML_CATA_PENTITY;
                    /* Falls through. */
		case SGML_CATA_PENTITY:
		case SGML_CATA_DOCTYPE:
		case SGML_CATA_LINKTYPE:
		case SGML_CATA_NOTATION:
		    cur = xmlParseSGMLCatalogName(cur, &name);
		    if (cur == NULL) {
			/* error */
			break;
		    }
		    if (!IS_BLANK_CH(*cur)) {
			/* error */
			break;
		    }
		    SKIP_BLANKS;
		    cur = xmlParseSGMLCatalogPubid(cur, &sysid);
		    if (cur == NULL) {
			/* error */
			break;
		    }
		    break;
		case SGML_CATA_PUBLIC:
		case SGML_CATA_SYSTEM:
		case SGML_CATA_DELEGATE:
		    cur = xmlParseSGMLCatalogPubid(cur, &name);
		    if (cur == NULL) {
			/* error */
			break;
		    }
		    if (type != SGML_CATA_SYSTEM) {
		        xmlChar *normid;

		        normid = xmlCatalogNormalizePublic(name);
		        if (normid != NULL) {
		            if (name != NULL)
		                xmlFree(name);
		            if (*normid != 0)
		                name = normid;
		            else {
		                xmlFree(normid);
		                name = NULL;
		            }
		        }
		    }
		    if (!IS_BLANK_CH(*cur)) {
			/* error */
			break;
		    }
		    SKIP_BLANKS;
		    cur = xmlParseSGMLCatalogPubid(cur, &sysid);
		    if (cur == NULL) {
			/* error */
			break;
		    }
		    break;
		case SGML_CATA_BASE:
		case SGML_CATA_CATALOG:
		case SGML_CATA_DOCUMENT:
		case SGML_CATA_SGMLDECL:
		    cur = xmlParseSGMLCatalogPubid(cur, &sysid);
		    if (cur == NULL) {
			/* error */
			break;
		    }
		    break;
		default:
		    break;
	    }
	    if (cur == NULL) {
		if (name != NULL)
		    xmlFree(name);
		if (sysid != NULL)
		    xmlFree(sysid);
		break;
	    } else if (type == SGML_CATA_BASE) {
		if (base != NULL)
		    xmlFree(base);
		base = xmlStrdup(sysid);
	    } else if ((type == SGML_CATA_PUBLIC) ||
		       (type == SGML_CATA_SYSTEM)) {
		xmlChar *filename;

		filename = xmlBuildURI(sysid, base);
		if (filename != NULL) {
		    xmlCatalogEntryPtr entry;

		    entry = xmlNewCatalogEntry(type, name, filename,
			                       NULL, XML_CATA_PREFER_NONE, NULL);
		    res = xmlHashAddEntry(catal->sgml, name, entry);
		    if (res < 0) {
			xmlFreeCatalogEntry(entry, NULL);
		    }
		    xmlFree(filename);
		}

	    } else if (type == SGML_CATA_CATALOG) {
		if (super) {
		    xmlCatalogEntryPtr entry;

		    entry = xmlNewCatalogEntry(type, sysid, NULL, NULL,
			                       XML_CATA_PREFER_NONE, NULL);
		    res = xmlHashAddEntry(catal->sgml, sysid, entry);
		    if (res < 0) {
			xmlFreeCatalogEntry(entry, NULL);
		    }
		} else {
		    xmlChar *filename;

		    filename = xmlBuildURI(sysid, base);
		    if (filename != NULL) {
			xmlExpandCatalog(catal, (const char *)filename);
			xmlFree(filename);
		    }
		}
	    }
	    /*
	     * drop anything else we won't handle it
	     */
	    if (name != NULL)
		xmlFree(name);
	    if (sysid != NULL)
		xmlFree(sysid);
	}
    }
    if (base != NULL)
	xmlFree(base);
    if (cur == NULL)
	return(-1);
    return(0);
}

/************************************************************************
 *									*
 *			SGML Catalog handling				*
 *									*
 ************************************************************************/

/**
 * Try to lookup the catalog local reference associated to a public ID
 *
 * @param catal  an SGML catalog hash
 * @param pubID  the public ID string
 * @returns the local resource if found or NULL otherwise.
 */
static const xmlChar *
xmlCatalogGetSGMLPublic(xmlHashTablePtr catal, const xmlChar *pubID) {
    xmlCatalogEntryPtr entry;
    xmlChar *normid;

    if (catal == NULL)
	return(NULL);

    normid = xmlCatalogNormalizePublic(pubID);
    if (normid != NULL)
        pubID = (*normid != 0 ? normid : NULL);

    entry = (xmlCatalogEntryPtr) xmlHashLookup(catal, pubID);
    if (entry == NULL) {
	if (normid != NULL)
	    xmlFree(normid);
	return(NULL);
    }
    if (entry->type == SGML_CATA_PUBLIC) {
	if (normid != NULL)
	    xmlFree(normid);
	return(entry->URL);
    }
    if (normid != NULL)
        xmlFree(normid);
    return(NULL);
}

/**
 * Try to lookup the catalog local reference for a system ID
 *
 * @param catal  an SGML catalog hash
 * @param sysID  the system ID string
 * @returns the local resource if found or NULL otherwise.
 */
static const xmlChar *
xmlCatalogGetSGMLSystem(xmlHashTablePtr catal, const xmlChar *sysID) {
    xmlCatalogEntryPtr entry;

    if (catal == NULL)
	return(NULL);

    entry = (xmlCatalogEntryPtr) xmlHashLookup(catal, sysID);
    if (entry == NULL)
	return(NULL);
    if (entry->type == SGML_CATA_SYSTEM)
	return(entry->URL);
    return(NULL);
}

/**
 * Do a complete resolution lookup of an External Identifier
 *
 * @param catal  the SGML catalog
 * @param pubID  the public ID string
 * @param sysID  the system ID string
 * @returns the URI of the resource or NULL if not found
 */
static const xmlChar *
xmlCatalogSGMLResolve(xmlCatalogPtr catal, const xmlChar *pubID,
	              const xmlChar *sysID) {
    const xmlChar *ret = NULL;

    if (catal->sgml == NULL)
	return(NULL);

    if (pubID != NULL)
	ret = xmlCatalogGetSGMLPublic(catal->sgml, pubID);
    if (ret != NULL)
	return(ret);
    if (sysID != NULL)
	ret = xmlCatalogGetSGMLSystem(catal->sgml, sysID);
    if (ret != NULL)
	return(ret);
    return(NULL);
}

#endif /* LIBXML_SGML_CATALOG_ENABLED */

/************************************************************************
 *									*
 *			Specific Public interfaces			*
 *									*
 ************************************************************************/

#ifdef LIBXML_SGML_CATALOG_ENABLED
/**
 * Load an SGML super catalog. It won't expand CATALOG or DELEGATE
 * references. This is only needed for manipulating SGML Super Catalogs
 * like adding and removing CATALOG or DELEGATE entries.
 *
 * @deprecated Internal function, don't use
 *
 * @param filename  a file path
 * @returns the catalog parsed or NULL in case of error
 */
xmlCatalog *
xmlLoadSGMLSuperCatalog(const char *filename)
{
    xmlChar *content;
    xmlCatalogPtr catal;
    int ret;

    content = xmlLoadFileContent(filename);
    if (content == NULL)
        return(NULL);

    catal = xmlCreateNewCatalog(XML_SGML_CATALOG_TYPE, xmlCatalogDefaultPrefer);
    if (catal == NULL) {
	xmlFree(content);
	return(NULL);
    }

    ret = xmlParseSGMLCatalog(catal, content, filename, 1);
    xmlFree(content);
    if (ret < 0) {
	xmlFreeCatalog(catal);
	return(NULL);
    }
    return (catal);
}
#endif /* LIBXML_SGML_CATALOG_ENABLED */

/**
 * Load the catalog and build the associated data structures.
 * This can be either an XML Catalog or an SGML Catalog
 * It will recurse in SGML CATALOG entries. On the other hand XML
 * Catalogs are not handled recursively.
 *
 * @deprecated Internal function, don't use
 *
 * @param filename  a file path
 * @returns the catalog parsed or NULL in case of error
 */
xmlCatalog *
xmlLoadACatalog(const char *filename)
{
    xmlChar *content;
    xmlChar *first;
    xmlCatalogPtr catal;

    content = xmlLoadFileContent(filename);
    if (content == NULL)
        return(NULL);


    first = content;

    while ((*first != 0) && (*first != '-') && (*first != '<') &&
	   (!(((*first >= 'A') && (*first <= 'Z')) ||
	      ((*first >= 'a') && (*first <= 'z')))))
	first++;

#ifdef LIBXML_SGML_CATALOG_ENABLED
    if (*first != '<') {
        int ret;

	catal = xmlCreateNewCatalog(XML_SGML_CATALOG_TYPE, xmlCatalogDefaultPrefer);
	if (catal == NULL) {
	    xmlFree(content);
	    return(NULL);
	}
        ret = xmlParseSGMLCatalog(catal, content, filename, 0);
	if (ret < 0) {
	    xmlFreeCatalog(catal);
	    xmlFree(content);
	    return(NULL);
	}
    } else
#endif /* LIBXML_SGML_CATALOG_ENABLED */
    {
	catal = xmlCreateNewCatalog(XML_XML_CATALOG_TYPE, xmlCatalogDefaultPrefer);
	if (catal == NULL) {
	    xmlFree(content);
	    return(NULL);
	}
        catal->xml = xmlNewCatalogEntry(XML_CATA_CATALOG, NULL,
		       NULL, BAD_CAST filename, xmlCatalogDefaultPrefer, NULL);
    }
    xmlFree(content);
    return (catal);
}

/**
 * Load the catalog and expand the existing catal structure.
 * This can be either an XML Catalog or an SGML Catalog
 *
 * @param catal  a catalog
 * @param filename  a file path
 * @returns 0 in case of success, -1 in case of error
 */
static int
xmlExpandCatalog(xmlCatalogPtr catal, const char *filename)
{
    if ((catal == NULL) || (filename == NULL))
	return(-1);

#ifdef LIBXML_SGML_CATALOG_ENABLED
    if (catal->type == XML_SGML_CATALOG_TYPE) {
	xmlChar *content;
        int ret;

	content = xmlLoadFileContent(filename);
	if (content == NULL)
	    return(-1);

        ret = xmlParseSGMLCatalog(catal, content, filename, 0);
	if (ret < 0) {
	    xmlFree(content);
	    return(-1);
	}
	xmlFree(content);
    } else
#endif /* LIBXML_SGML_CATALOG_ENABLED */
    {
	xmlCatalogEntryPtr tmp, cur;
	tmp = xmlNewCatalogEntry(XML_CATA_CATALOG, NULL,
		       NULL, BAD_CAST filename, xmlCatalogDefaultPrefer, NULL);

	cur = catal->xml;
	if (cur == NULL) {
	    catal->xml = tmp;
	} else {
	    while (cur->next != NULL) cur = cur->next;
	    cur->next = tmp;
	}
    }
    return (0);
}

/**
 * Try to lookup the catalog resource for a system ID
 *
 * @deprecated Internal function, don't use
 *
 * @param catal  a Catalog
 * @param sysID  the system ID string
 * @returns the resource if found or NULL otherwise, the value returned
 *      must be freed by the caller.
 */
xmlChar *
xmlACatalogResolveSystem(xmlCatalog *catal, const xmlChar *sysID) {
    xmlChar *ret = NULL;

    if ((sysID == NULL) || (catal == NULL))
	return(NULL);

    if (xmlDebugCatalogs)
	xmlCatalogPrintDebug(
		"Resolve sysID %s\n", sysID);

#ifdef LIBXML_SGML_CATALOG_ENABLED
    if (catal->type == XML_SGML_CATALOG_TYPE) {
	const xmlChar *sgml;

	sgml = xmlCatalogGetSGMLSystem(catal->sgml, sysID);
	if (sgml != NULL)
	    ret = xmlStrdup(sgml);
    } else
#endif /* LIBXML_SGML_CATALOG_ENABLED */
    {
	ret = xmlCatalogListXMLResolve(catal->xml, NULL, sysID);
	if (ret == XML_CATAL_BREAK)
	    ret = NULL;
    }
    return(ret);
}

/**
 * Try to lookup the catalog local reference associated to a public ID in that catalog
 *
 * @deprecated Internal function, don't use
 *
 * @param catal  a Catalog
 * @param pubID  the public ID string
 * @returns the local resource if found or NULL otherwise, the value returned
 *      must be freed by the caller.
 */
xmlChar *
xmlACatalogResolvePublic(xmlCatalog *catal, const xmlChar *pubID) {
    xmlChar *ret = NULL;

    if ((pubID == NULL) || (catal == NULL))
	return(NULL);

    if (xmlDebugCatalogs)
	xmlCatalogPrintDebug(
		"Resolve pubID %s\n", pubID);

#ifdef LIBXML_SGML_CATALOG_ENABLED
    if (catal->type == XML_SGML_CATALOG_TYPE) {
	const xmlChar *sgml;

	sgml = xmlCatalogGetSGMLPublic(catal->sgml, pubID);
	if (sgml != NULL)
	    ret = xmlStrdup(sgml);
    } else
#endif /* LIBXML_SGML_CATALOG_ENABLED */
    {
	ret = xmlCatalogListXMLResolve(catal->xml, pubID, NULL);
	if (ret == XML_CATAL_BREAK)
	    ret = NULL;
    }
    return(ret);
}

/**
 * Do a complete resolution lookup of an External Identifier
 *
 * @deprecated Internal function, don't use
 *
 * @param catal  a Catalog
 * @param pubID  the public ID string
 * @param sysID  the system ID string
 * @returns the URI of the resource or NULL if not found, it must be freed
 *      by the caller.
 */
xmlChar *
xmlACatalogResolve(xmlCatalog *catal, const xmlChar * pubID,
                   const xmlChar * sysID)
{
    xmlChar *ret = NULL;

    if (((pubID == NULL) && (sysID == NULL)) || (catal == NULL))
        return (NULL);

    if (xmlDebugCatalogs) {
         if ((pubID != NULL) && (sysID != NULL)) {
             xmlCatalogPrintDebug(
                             "Resolve: pubID %s sysID %s\n", pubID, sysID);
         } else if (pubID != NULL) {
             xmlCatalogPrintDebug(
                             "Resolve: pubID %s\n", pubID);
         } else {
             xmlCatalogPrintDebug(
                             "Resolve: sysID %s\n", sysID);
         }
    }

#ifdef LIBXML_SGML_CATALOG_ENABLED
    if (catal->type == XML_SGML_CATALOG_TYPE) {
        const xmlChar *sgml;

        sgml = xmlCatalogSGMLResolve(catal, pubID, sysID);
        if (sgml != NULL)
            ret = xmlStrdup(sgml);
    } else
#endif /* LIBXML_SGML_CATALOG_ENABLED */
    {
        ret = xmlCatalogListXMLResolve(catal->xml, pubID, sysID);
	if (ret == XML_CATAL_BREAK)
	    ret = NULL;
    }
    return (ret);
}

/**
 * Do a complete resolution lookup of an URI
 *
 * @deprecated Internal function, don't use
 *
 * @param catal  a Catalog
 * @param URI  the URI
 * @returns the URI of the resource or NULL if not found, it must be freed
 *      by the caller.
 */
xmlChar *
xmlACatalogResolveURI(xmlCatalog *catal, const xmlChar *URI) {
    xmlChar *ret = NULL;

    if ((URI == NULL) || (catal == NULL))
	return(NULL);

    if (xmlDebugCatalogs)
	xmlCatalogPrintDebug(
		"Resolve URI %s\n", URI);

#ifdef LIBXML_SGML_CATALOG_ENABLED
    if (catal->type == XML_SGML_CATALOG_TYPE) {
	const xmlChar *sgml;

	sgml = xmlCatalogSGMLResolve(catal, NULL, URI);
	if (sgml != NULL)
            ret = xmlStrdup(sgml);
    } else
#endif /* LIBXML_SGML_CATALOG_ENABLED */
    {
	ret = xmlCatalogListXMLResolveURI(catal->xml, URI);
	if (ret == XML_CATAL_BREAK)
	    ret = NULL;
    }
    return(ret);
}

#ifdef LIBXML_OUTPUT_ENABLED
/**
 * Dump the given catalog to the given file.
 *
 * @deprecated Internal function, don't use
 *
 * @param catal  a Catalog
 * @param out  the file.
 */
void
xmlACatalogDump(xmlCatalog *catal, FILE *out) {
    if ((out == NULL) || (catal == NULL))
	return;

#ifdef LIBXML_SGML_CATALOG_ENABLED
    if (catal->type == XML_SGML_CATALOG_TYPE) {
	xmlHashScan(catal->sgml, xmlCatalogDumpEntry, out);
    } else
#endif /* LIBXML_SGML_CATALOG_ENABLED */
    {
	xmlDumpXMLCatalog(out, catal->xml);
    }
}
#endif /* LIBXML_OUTPUT_ENABLED */

/**
 * Add an entry in the catalog, it may overwrite existing but
 * different entries.
 *
 * @deprecated Internal function, don't use
 *
 * @param catal  a Catalog
 * @param type  the type of record to add to the catalog
 * @param orig  the system, public or prefix to match
 * @param replace  the replacement value for the match
 * @returns 0 if successful, -1 otherwise
 */
int
xmlACatalogAdd(xmlCatalog *catal, const xmlChar * type,
              const xmlChar * orig, const xmlChar * replace)
{
    int res = -1;

    if (catal == NULL)
	return(-1);

#ifdef LIBXML_SGML_CATALOG_ENABLED
    if (catal->type == XML_SGML_CATALOG_TYPE) {
        xmlCatalogEntryType cattype;

        cattype = xmlGetSGMLCatalogEntryType(type);
        if (cattype != XML_CATA_NONE) {
            xmlCatalogEntryPtr entry;

            entry = xmlNewCatalogEntry(cattype, orig, replace, NULL,
                                       XML_CATA_PREFER_NONE, NULL);
	    if (catal->sgml == NULL)
		catal->sgml = xmlHashCreate(10);
            res = xmlHashAddEntry(catal->sgml, orig, entry);
            if (res < 0)
                xmlFreeCatalogEntry(entry, NULL);
        }
    } else
#endif /* LIBXML_SGML_CATALOG_ENABLED */
    {
        res = xmlAddXMLCatalog(catal->xml, type, orig, replace);
    }
    return (res);
}

/**
 * Remove an entry from the catalog
 *
 * @deprecated Internal function, don't use
 *
 * @param catal  a Catalog
 * @param value  the value to remove
 * @returns the number of entries removed if successful, -1 otherwise
 */
int
xmlACatalogRemove(xmlCatalog *catal, const xmlChar *value) {
    int res = -1;

    if ((catal == NULL) || (value == NULL))
	return(-1);

#ifdef LIBXML_SGML_CATALOG_ENABLED
    if (catal->type == XML_SGML_CATALOG_TYPE) {
	res = xmlHashRemoveEntry(catal->sgml, value, xmlFreeCatalogEntry);
	if (res == 0)
	    res = 1;
    } else
#endif /* LIBXML_SGML_CATALOG_ENABLED */
    {
	res = xmlDelXMLCatalog(catal->xml, value);
    }
    return(res);
}

/**
 * create a new Catalog.
 *
 * @deprecated Internal function, don't use
 *
 * @param sgml  should this create an SGML catalog
 * @returns the xmlCatalog or NULL in case of error
 */
xmlCatalog *
xmlNewCatalog(int sgml) {
    xmlCatalogPtr catal = NULL;

    (void) sgml;

#ifdef LIBXML_SGML_CATALOG_ENABLED
    if (sgml) {
	catal = xmlCreateNewCatalog(XML_SGML_CATALOG_TYPE,
		                    xmlCatalogDefaultPrefer);
        if ((catal != NULL) && (catal->sgml == NULL))
	    catal->sgml = xmlHashCreate(10);
    } else
#endif /* LIBXML_SGML_CATALOG_ENABLED */
    {
	catal = xmlCreateNewCatalog(XML_XML_CATALOG_TYPE,
		                    xmlCatalogDefaultPrefer);
    }
    return(catal);
}

/**
 * Check is a catalog is empty
 *
 * @deprecated Internal function, don't use
 *
 * @param catal  should this create an SGML catalog
 * @returns 1 if the catalog is empty, 0 if not, amd -1 in case of error.
 */
int
xmlCatalogIsEmpty(xmlCatalog *catal) {
    if (catal == NULL)
	return(-1);

#ifdef LIBXML_SGML_CATALOG_ENABLED
    if (catal->type == XML_SGML_CATALOG_TYPE) {
	int res;

	if (catal->sgml == NULL)
	    return(1);
	res = xmlHashSize(catal->sgml);
	if (res == 0)
	    return(1);
	if (res < 0)
	    return(-1);
    } else
#endif /* LIBXML_SGML_CATALOG_ENABLED */
    {
	if (catal->xml == NULL)
	    return(1);
	if ((catal->xml->type != XML_CATA_CATALOG) &&
	    (catal->xml->type != XML_CATA_BROKEN_CATALOG))
	    return(-1);
	if (catal->xml->children == NULL)
	    return(1);
        return(0);
    }
    return(0);
}

/************************************************************************
 *									*
 *   Public interfaces manipulating the global shared default catalog	*
 *									*
 ************************************************************************/

/**
 * Do the catalog initialization only of global data, doesn't try to load
 * any catalog actually.
 */
void
xmlInitCatalogInternal(void) {
    if (getenv("XML_DEBUG_CATALOG"))
	xmlDebugCatalogs = 1;
    xmlInitRMutex(&xmlCatalogMutex);
}

/**
 * Load the default system catalog.
 */
void
xmlInitializeCatalog(void) {
    if (xmlCatalogInitialized != 0)
	return;

    xmlInitParser();

    xmlRMutexLock(&xmlCatalogMutex);

    if (xmlDefaultCatalog == NULL) {
	const char *catalogs;
	char *path;
	const char *cur, *paths;
	xmlCatalogPtr catal;
	xmlCatalogEntryPtr *nextent;

	catalogs = (const char *) getenv("XML_CATALOG_FILES");
	if (catalogs == NULL)
	    catalogs = XML_XML_DEFAULT_CATALOG;

	catal = xmlCreateNewCatalog(XML_XML_CATALOG_TYPE,
		xmlCatalogDefaultPrefer);
	if (catal != NULL) {
	    /* the XML_CATALOG_FILES envvar is allowed to contain a
	       space-separated list of entries. */
	    cur = catalogs;
	    nextent = &catal->xml;
	    while (*cur != '\0') {
		while (xmlIsBlank_ch(*cur))
		    cur++;
		if (*cur != 0) {
		    paths = cur;
		    while ((*cur != 0) && (!xmlIsBlank_ch(*cur)))
			cur++;
		    path = (char *) xmlStrndup((const xmlChar *)paths, cur - paths);
		    if (path != NULL) {
			*nextent = xmlNewCatalogEntry(XML_CATA_CATALOG, NULL,
				NULL, BAD_CAST path, xmlCatalogDefaultPrefer, NULL);
			if (*nextent != NULL)
			    nextent = &((*nextent)->next);
			xmlFree(path);
		    }
		}
	    }
	    xmlDefaultCatalog = catal;
	}
    }

    xmlRMutexUnlock(&xmlCatalogMutex);

    xmlCatalogInitialized = 1;
}


/**
 * Load the catalog and makes its definitions effective for the default
 * external entity loader. It will recurse in SGML CATALOG entries.
 * this function is not thread safe, catalog initialization should
 * preferably be done once at startup
 *
 * @param filename  a file path
 * @returns 0 in case of success -1 in case of error
 */
int
xmlLoadCatalog(const char *filename)
{
    int ret;
    xmlCatalogPtr catal;

    xmlInitParser();

    xmlRMutexLock(&xmlCatalogMutex);

    if (xmlDefaultCatalog == NULL) {
	catal = xmlLoadACatalog(filename);
	if (catal == NULL) {
	    xmlRMutexUnlock(&xmlCatalogMutex);
	    return(-1);
	}

	xmlDefaultCatalog = catal;
	xmlRMutexUnlock(&xmlCatalogMutex);
        xmlCatalogInitialized = 1;
	return(0);
    }

    ret = xmlExpandCatalog(xmlDefaultCatalog, filename);
    xmlRMutexUnlock(&xmlCatalogMutex);
    return(ret);
}

/**
 * Load the catalogs and makes their definitions effective for the default
 * external entity loader.
 * this function is not thread safe, catalog initialization should
 * preferably be done once at startup
 *
 * @param pathss  a list of directories separated by a colon or a space.
 */
void
xmlLoadCatalogs(const char *pathss) {
    const char *cur;
    const char *paths;
    xmlChar *path;
#ifdef _WIN32
    int i, iLen;
#endif

    if (pathss == NULL)
	return;

    cur = pathss;
    while (*cur != 0) {
	while (xmlIsBlank_ch(*cur)) cur++;
	if (*cur != 0) {
	    paths = cur;
	    while ((*cur != 0) && (*cur != PATH_SEPARATOR) && (!xmlIsBlank_ch(*cur)))
		cur++;
	    path = xmlStrndup((const xmlChar *)paths, cur - paths);
	    if (path != NULL) {
#ifdef _WIN32
        iLen = strlen((const char*)path);
        for(i = 0; i < iLen; i++) {
            if(path[i] == '\\') {
                path[i] = '/';
            }
        }
#endif
		xmlLoadCatalog((const char *) path);
		xmlFree(path);
	    }
	}
	while (*cur == PATH_SEPARATOR)
	    cur++;
    }
}

/**
 * Free up all the memory associated with catalogs
 */
void
xmlCatalogCleanup(void) {
    xmlRMutexLock(&xmlCatalogMutex);
    if (xmlDebugCatalogs)
	xmlCatalogPrintDebug(
		"Catalogs cleanup\n");
    if (xmlCatalogXMLFiles != NULL)
	xmlHashFree(xmlCatalogXMLFiles, xmlFreeCatalogHashEntryList);
    xmlCatalogXMLFiles = NULL;
    if (xmlDefaultCatalog != NULL)
	xmlFreeCatalog(xmlDefaultCatalog);
    xmlDefaultCatalog = NULL;
    xmlDebugCatalogs = 0;
    xmlCatalogInitialized = 0;
    xmlRMutexUnlock(&xmlCatalogMutex);
}

/**
 * Free global data.
 */
void
xmlCleanupCatalogInternal(void) {
    xmlCleanupRMutex(&xmlCatalogMutex);
}

/**
 * Try to lookup the catalog resource for a system ID
 *
 * @param sysID  the system ID string
 * @returns the resource if found or NULL otherwise, the value returned
 *      must be freed by the caller.
 */
xmlChar *
xmlCatalogResolveSystem(const xmlChar *sysID) {
    xmlChar *ret;

    if (!xmlCatalogInitialized)
	xmlInitializeCatalog();

    ret = xmlACatalogResolveSystem(xmlDefaultCatalog, sysID);
    return(ret);
}

/**
 * Try to lookup the catalog reference associated to a public ID
 *
 * @param pubID  the public ID string
 * @returns the resource if found or NULL otherwise, the value returned
 *      must be freed by the caller.
 */
xmlChar *
xmlCatalogResolvePublic(const xmlChar *pubID) {
    xmlChar *ret;

    if (!xmlCatalogInitialized)
	xmlInitializeCatalog();

    ret = xmlACatalogResolvePublic(xmlDefaultCatalog, pubID);
    return(ret);
}

/**
 * Do a complete resolution lookup of an External Identifier
 *
 * @param pubID  the public ID string
 * @param sysID  the system ID string
 * @returns the URI of the resource or NULL if not found, it must be freed
 *      by the caller.
 */
xmlChar *
xmlCatalogResolve(const xmlChar *pubID, const xmlChar *sysID) {
    xmlChar *ret;

    if (!xmlCatalogInitialized)
	xmlInitializeCatalog();

    ret = xmlACatalogResolve(xmlDefaultCatalog, pubID, sysID);
    return(ret);
}

/**
 * Do a complete resolution lookup of an URI
 *
 * @param URI  the URI
 * @returns the URI of the resource or NULL if not found, it must be freed
 *      by the caller.
 */
xmlChar *
xmlCatalogResolveURI(const xmlChar *URI) {
    xmlChar *ret;

    if (!xmlCatalogInitialized)
	xmlInitializeCatalog();

    ret = xmlACatalogResolveURI(xmlDefaultCatalog, URI);
    return(ret);
}

#ifdef LIBXML_OUTPUT_ENABLED
/**
 * Dump all the global catalog content to the given file.
 *
 * @param out  the file.
 */
void
xmlCatalogDump(FILE *out) {
    if (out == NULL)
	return;

    if (!xmlCatalogInitialized)
	xmlInitializeCatalog();

    xmlACatalogDump(xmlDefaultCatalog, out);
}
#endif /* LIBXML_OUTPUT_ENABLED */

/**
 * Add an entry in the catalog, it may overwrite existing but
 * different entries.
 * If called before any other catalog routine, allows to override the
 * default shared catalog put in place by #xmlInitializeCatalog;
 *
 * @param type  the type of record to add to the catalog
 * @param orig  the system, public or prefix to match
 * @param replace  the replacement value for the match
 * @returns 0 if successful, -1 otherwise
 */
int
xmlCatalogAdd(const xmlChar *type, const xmlChar *orig, const xmlChar *replace) {
    int res = -1;

    xmlInitParser();

    xmlRMutexLock(&xmlCatalogMutex);
    /*
     * Specific case where one want to override the default catalog
     * put in place by xmlInitializeCatalog();
     */
    if ((xmlDefaultCatalog == NULL) &&
	(xmlStrEqual(type, BAD_CAST "catalog"))) {
	xmlDefaultCatalog = xmlCreateNewCatalog(XML_XML_CATALOG_TYPE,
		                          xmlCatalogDefaultPrefer);
	if (xmlDefaultCatalog != NULL) {
	   xmlDefaultCatalog->xml = xmlNewCatalogEntry(XML_CATA_CATALOG, NULL,
				    orig, NULL,  xmlCatalogDefaultPrefer, NULL);
	}
	xmlRMutexUnlock(&xmlCatalogMutex);
        xmlCatalogInitialized = 1;
	return(0);
    }

    res = xmlACatalogAdd(xmlDefaultCatalog, type, orig, replace);
    xmlRMutexUnlock(&xmlCatalogMutex);
    return(res);
}

/**
 * Remove an entry from the catalog
 *
 * @param value  the value to remove
 * @returns the number of entries removed if successful, -1 otherwise
 */
int
xmlCatalogRemove(const xmlChar *value) {
    int res;

    if (!xmlCatalogInitialized)
	xmlInitializeCatalog();

    xmlRMutexLock(&xmlCatalogMutex);
    res = xmlACatalogRemove(xmlDefaultCatalog, value);
    xmlRMutexUnlock(&xmlCatalogMutex);
    return(res);
}

#ifdef LIBXML_SGML_CATALOG_ENABLED
/**
 * Convert all the SGML catalog entries as XML ones
 *
 * @returns the number of entries converted if successful, -1 otherwise
 */
int
xmlCatalogConvert(void) {
    int res = -1;

    if (!xmlCatalogInitialized)
	xmlInitializeCatalog();

    xmlRMutexLock(&xmlCatalogMutex);
    res = xmlConvertSGMLCatalog(xmlDefaultCatalog);
    xmlRMutexUnlock(&xmlCatalogMutex);
    return(res);
}
#endif /* LIBXML_SGML_CATALOG_ENABLED */

/************************************************************************
 *									*
 *	Public interface manipulating the common preferences		*
 *									*
 ************************************************************************/

/**
 * Used to get the user preference w.r.t. to what catalogs should
 * be accepted
 *
 * @deprecated Use XML_PARSE_NO_SYS_CATALOG and
 * XML_PARSE_CATALOG_PI.
 *
 * @returns the current xmlCatalogAllow value
 */
xmlCatalogAllow
xmlCatalogGetDefaults(void) {
    return(xmlCatalogDefaultAllow);
}

/**
 * Used to set the user preference w.r.t. to what catalogs should
 * be accepted
 *
 * @deprecated Use XML_PARSE_NO_SYS_CATALOG and
 * XML_PARSE_CATALOG_PI.
 *
 * @param allow  what catalogs should be accepted
 */
void
xmlCatalogSetDefaults(xmlCatalogAllow allow) {
    if (xmlDebugCatalogs) {
	switch (allow) {
	    case XML_CATA_ALLOW_NONE:
		xmlCatalogPrintDebug(
			"Disabling catalog usage\n");
		break;
	    case XML_CATA_ALLOW_GLOBAL:
		xmlCatalogPrintDebug(
			"Allowing only global catalogs\n");
		break;
	    case XML_CATA_ALLOW_DOCUMENT:
		xmlCatalogPrintDebug(
			"Allowing only catalogs from the document\n");
		break;
	    case XML_CATA_ALLOW_ALL:
		xmlCatalogPrintDebug(
			"Allowing all catalogs\n");
		break;
	}
    }
    xmlCatalogDefaultAllow = allow;
}

/**
 * Allows to set the preference between public and system for deletion
 * in XML Catalog resolution. C.f. section 4.1.1 of the spec
 * Values accepted are XML_CATA_PREFER_PUBLIC or XML_CATA_PREFER_SYSTEM
 *
 * @deprecated This setting is global and not thread-safe.
 *
 * @param prefer  the default preference for delegation
 * @returns the previous value of the default preference for delegation
 */
xmlCatalogPrefer
xmlCatalogSetDefaultPrefer(xmlCatalogPrefer prefer) {
    xmlCatalogPrefer ret = xmlCatalogDefaultPrefer;

    if (prefer == XML_CATA_PREFER_NONE)
	return(ret);

    if (xmlDebugCatalogs) {
	switch (prefer) {
	    case XML_CATA_PREFER_PUBLIC:
		xmlCatalogPrintDebug(
			"Setting catalog preference to PUBLIC\n");
		break;
	    case XML_CATA_PREFER_SYSTEM:
		xmlCatalogPrintDebug(
			"Setting catalog preference to SYSTEM\n");
		break;
	    default:
		return(ret);
	}
    }
    xmlCatalogDefaultPrefer = prefer;
    return(ret);
}

/**
 * Used to set the debug level for catalog operation, 0 disable
 * debugging, 1 enable it
 *
 * @param level  the debug level of catalogs required
 * @returns the previous value of the catalog debugging level
 */
int
xmlCatalogSetDebug(int level) {
    int ret = xmlDebugCatalogs;

    if (level <= 0)
        xmlDebugCatalogs = 0;
    else
	xmlDebugCatalogs = level;
    return(ret);
}

/************************************************************************
 *									*
 *   Minimal interfaces used for per-document catalogs by the parser	*
 *									*
 ************************************************************************/

/**
 * Free up the memory associated to the catalog list
 *
 * @param catalogs  a document's list of catalogs
 */
void
xmlCatalogFreeLocal(void *catalogs) {
    xmlCatalogEntryPtr catal;

    catal = (xmlCatalogEntryPtr) catalogs;
    if (catal != NULL)
	xmlFreeCatalogEntryList(catal);
}


/**
 * Add the new entry to the catalog list
 *
 * @param catalogs  a document's list of catalogs
 * @param URL  the URL to a new local catalog
 * @returns the updated list
 */
void *
xmlCatalogAddLocal(void *catalogs, const xmlChar *URL) {
    xmlCatalogEntryPtr catal, add;

    xmlInitParser();

    if (URL == NULL)
	return(catalogs);

    if (xmlDebugCatalogs)
	xmlCatalogPrintDebug(
		"Adding document catalog %s\n", URL);

    add = xmlNewCatalogEntry(XML_CATA_CATALOG, NULL, URL, NULL,
	                     xmlCatalogDefaultPrefer, NULL);
    if (add == NULL)
	return(catalogs);

    catal = (xmlCatalogEntryPtr) catalogs;
    if (catal == NULL)
	return((void *) add);

    while (catal->next != NULL)
	catal = catal->next;
    catal->next = add;
    return(catalogs);
}

/**
 * Do a complete resolution lookup of an External Identifier using a
 * document's private catalog list
 *
 * @param catalogs  a document's list of catalogs
 * @param pubID  the public ID string
 * @param sysID  the system ID string
 * @returns the URI of the resource or NULL if not found, it must be freed
 *      by the caller.
 */
xmlChar *
xmlCatalogLocalResolve(void *catalogs, const xmlChar *pubID,
	               const xmlChar *sysID) {
    xmlCatalogEntryPtr catal;
    xmlChar *ret;

    if ((pubID == NULL) && (sysID == NULL))
	return(NULL);

    if (xmlDebugCatalogs) {
        if ((pubID != NULL) && (sysID != NULL)) {
            xmlCatalogPrintDebug(
                            "Local Resolve: pubID %s sysID %s\n", pubID, sysID);
        } else if (pubID != NULL) {
            xmlCatalogPrintDebug(
                            "Local Resolve: pubID %s\n", pubID);
        } else {
            xmlCatalogPrintDebug(
                            "Local Resolve: sysID %s\n", sysID);
        }
    }

    catal = (xmlCatalogEntryPtr) catalogs;
    if (catal == NULL)
	return(NULL);
    ret = xmlCatalogListXMLResolve(catal, pubID, sysID);
    if ((ret != NULL) && (ret != XML_CATAL_BREAK))
	return(ret);
    return(NULL);
}

/**
 * Do a complete resolution lookup of an URI using a
 * document's private catalog list
 *
 * @param catalogs  a document's list of catalogs
 * @param URI  the URI
 * @returns the URI of the resource or NULL if not found, it must be freed
 *      by the caller.
 */
xmlChar *
xmlCatalogLocalResolveURI(void *catalogs, const xmlChar *URI) {
    xmlCatalogEntryPtr catal;
    xmlChar *ret;

    if (URI == NULL)
	return(NULL);

    if (xmlDebugCatalogs)
	xmlCatalogPrintDebug(
		"Resolve URI %s\n", URI);

    catal = (xmlCatalogEntryPtr) catalogs;
    if (catal == NULL)
	return(NULL);
    ret = xmlCatalogListXMLResolveURI(catal, URI);
    if ((ret != NULL) && (ret != XML_CATAL_BREAK))
	return(ret);
    return(NULL);
}

/************************************************************************
 *									*
 *			Deprecated interfaces				*
 *									*
 ************************************************************************/
/**
 * Try to lookup the catalog reference associated to a system ID
 *
 * @deprecated use #xmlCatalogResolveSystem
 *
 * @param sysID  the system ID string
 * @returns the resource if found or NULL otherwise.
 */
const xmlChar *
xmlCatalogGetSystem(const xmlChar *sysID) {
    xmlChar *ret;
    static xmlChar result[1000];
    static int msg = 0;

    if (!xmlCatalogInitialized)
	xmlInitializeCatalog();

    if (msg == 0) {
	xmlPrintErrorMessage(
		"Use of deprecated xmlCatalogGetSystem() call\n");
	msg++;
    }

    if (sysID == NULL)
	return(NULL);

    /*
     * Check first the XML catalogs
     */
    if (xmlDefaultCatalog != NULL) {
	ret = xmlCatalogListXMLResolve(xmlDefaultCatalog->xml, NULL, sysID);
	if ((ret != NULL) && (ret != XML_CATAL_BREAK)) {
	    snprintf((char *) result, sizeof(result) - 1, "%s", (char *) ret);
	    result[sizeof(result) - 1] = 0;
	    return(result);
	}
    }

#ifdef LIBXML_SGML_CATALOG_ENABLED
    if (xmlDefaultCatalog != NULL)
	return(xmlCatalogGetSGMLSystem(xmlDefaultCatalog->sgml, sysID));
#endif
    return(NULL);
}

/**
 * Try to lookup the catalog reference associated to a public ID
 *
 * @deprecated use #xmlCatalogResolvePublic
 *
 * @param pubID  the public ID string
 * @returns the resource if found or NULL otherwise.
 */
const xmlChar *
xmlCatalogGetPublic(const xmlChar *pubID) {
    xmlChar *ret;
    static xmlChar result[1000];
    static int msg = 0;

    if (!xmlCatalogInitialized)
	xmlInitializeCatalog();

    if (msg == 0) {
	xmlPrintErrorMessage(
		"Use of deprecated xmlCatalogGetPublic() call\n");
	msg++;
    }

    if (pubID == NULL)
	return(NULL);

    /*
     * Check first the XML catalogs
     */
    if (xmlDefaultCatalog != NULL) {
	ret = xmlCatalogListXMLResolve(xmlDefaultCatalog->xml, pubID, NULL);
	if ((ret != NULL) && (ret != XML_CATAL_BREAK)) {
	    snprintf((char *) result, sizeof(result) - 1, "%s", (char *) ret);
	    result[sizeof(result) - 1] = 0;
	    return(result);
	}
    }

#ifdef LIBXML_SGML_CATALOG_ENABLED
    if (xmlDefaultCatalog != NULL)
	return(xmlCatalogGetSGMLPublic(xmlDefaultCatalog->sgml, pubID));
#endif
    return(NULL);
}

#endif /* LIBXML_CATALOG_ENABLED */

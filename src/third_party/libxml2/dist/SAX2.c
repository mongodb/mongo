/*
 * SAX2.c : Default SAX2 handler to build a tree.
 *
 * See Copyright for the status of this software.
 *
 * Author: Daniel Veillard
 */


#define IN_LIBXML
#include "libxml.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <stddef.h>
#include <libxml/SAX2.h>
#include <libxml/xmlmemory.h>
#include <libxml/tree.h>
#include <libxml/parser.h>
#include <libxml/parserInternals.h>
#include <libxml/valid.h>
#include <libxml/entities.h>
#include <libxml/xmlerror.h>
#include <libxml/xmlIO.h>
#include <libxml/uri.h>
#include <libxml/valid.h>
#include <libxml/HTMLtree.h>

#include "private/error.h"
#include "private/parser.h"
#include "private/tree.h"

#ifndef SIZE_MAX
  #define SIZE_MAX ((size_t) -1)
#endif

/*
 * @param ctxt  an XML validation parser context
 * @param msg  a string to accompany the error message
 */
static void
xmlSAX2ErrMemory(xmlParserCtxtPtr ctxt) {
    xmlCtxtErrMemory(ctxt);
}

#ifdef LIBXML_VALID_ENABLED
/**
 * Handle a validation error
 *
 * @param ctxt  an XML validation parser context
 * @param error  the error number
 * @param msg  the error message
 * @param str1  extra data
 * @param str2  extra data
 */
static void LIBXML_ATTR_FORMAT(3,0)
xmlErrValid(xmlParserCtxtPtr ctxt, xmlParserErrors error,
            const char *msg, const xmlChar *str1, const xmlChar *str2)
{
    xmlCtxtErr(ctxt, NULL, XML_FROM_DTD, error, XML_ERR_ERROR,
               str1, str2, NULL, 0, msg, str1, str2);
    if (ctxt != NULL)
	ctxt->valid = 0;
}
#endif /* LIBXML_VALID_ENABLED */

/**
 * Handle a fatal parser error, i.e. violating Well-Formedness constraints
 *
 * @param ctxt  an XML parser context
 * @param error  the error number
 * @param msg  the error message
 * @param str1  an error string
 * @param str2  an error string
 */
static void LIBXML_ATTR_FORMAT(3,0)
xmlFatalErrMsg(xmlParserCtxtPtr ctxt, xmlParserErrors error,
               const char *msg, const xmlChar *str1, const xmlChar *str2)
{
    xmlCtxtErr(ctxt, NULL, XML_FROM_PARSER, error, XML_ERR_FATAL,
               str1, str2, NULL, 0, msg, str1, str2);
}

/**
 * Handle an xml:id error
 *
 * @param ctxt  an XML validation parser context
 * @param error  the error number
 * @param msg  the error message
 * @param str1  extra data
 */
static void LIBXML_ATTR_FORMAT(3,0)
xmlErrId(xmlParserCtxtPtr ctxt, xmlParserErrors error, const char *msg,
         const xmlChar *str1)
{
    xmlCtxtErr(ctxt, NULL, XML_FROM_PARSER, error, XML_ERR_ERROR,
               str1, NULL, NULL, 0, msg, str1);
}

/**
 * Handle a parser warning
 *
 * @param ctxt  an XML parser context
 * @param error  the error number
 * @param msg  the error message
 * @param str1  an error string
 */
static void LIBXML_ATTR_FORMAT(3,0)
xmlWarnMsg(xmlParserCtxtPtr ctxt, xmlParserErrors error,
               const char *msg, const xmlChar *str1)
{
    xmlCtxtErr(ctxt, NULL, XML_FROM_PARSER, error, XML_ERR_WARNING,
               str1, NULL, NULL, 0, msg, str1);
}

/**
 * Handle a namespace warning
 *
 * @param ctxt  an XML parser context
 * @param error  the error number
 * @param msg  the error message
 * @param str1  an error string
 * @param str2  an error string
 */
static void LIBXML_ATTR_FORMAT(3,0)
xmlNsWarnMsg(xmlParserCtxtPtr ctxt, xmlParserErrors error,
             const char *msg, const xmlChar *str1, const xmlChar *str2)
{
    xmlCtxtErr(ctxt, NULL, XML_FROM_NAMESPACE, error, XML_ERR_WARNING,
               str1, str2, NULL, 0, msg, str1, str2);
}

/**
 * Provides the public ID e.g. "-//SGMLSOURCE//DTD DEMO//EN"
 *
 * @param ctx  the user data (XML parser context)
 * @returns a xmlChar *
 */
const xmlChar *
xmlSAX2GetPublicId(void *ctx ATTRIBUTE_UNUSED)
{
    /* xmlParserCtxtPtr ctxt = (xmlParserCtxtPtr) ctx; */
    return(NULL);
}

/**
 * Provides the system ID, basically URL or filename e.g.
 * http://www.sgmlsource.com/dtds/memo.dtd
 *
 * @param ctx  the user data (XML parser context)
 * @returns a xmlChar *
 */
const xmlChar *
xmlSAX2GetSystemId(void *ctx)
{
    xmlParserCtxtPtr ctxt = (xmlParserCtxtPtr) ctx;
    if ((ctx == NULL) || (ctxt->input == NULL)) return(NULL);
    return((const xmlChar *) ctxt->input->filename);
}

/**
 * Provide the line number of the current parsing point.
 *
 * @param ctx  the user data (XML parser context)
 * @returns an int
 */
int
xmlSAX2GetLineNumber(void *ctx)
{
    xmlParserCtxtPtr ctxt = (xmlParserCtxtPtr) ctx;
    if ((ctx == NULL) || (ctxt->input == NULL)) return(0);
    return(ctxt->input->line);
}

/**
 * Provide the column number of the current parsing point.
 *
 * @param ctx  the user data (XML parser context)
 * @returns an int
 */
int
xmlSAX2GetColumnNumber(void *ctx)
{
    xmlParserCtxtPtr ctxt = (xmlParserCtxtPtr) ctx;
    if ((ctx == NULL) || (ctxt->input == NULL)) return(0);
    return(ctxt->input->col);
}

/**
 * Is this document tagged standalone ?
 *
 * @param ctx  the user data (XML parser context)
 * @returns 1 if true
 */
int
xmlSAX2IsStandalone(void *ctx)
{
    xmlParserCtxtPtr ctxt = (xmlParserCtxtPtr) ctx;
    if ((ctx == NULL) || (ctxt->myDoc == NULL)) return(0);
    return(ctxt->myDoc->standalone == 1);
}

/**
 * Does this document has an internal subset
 *
 * @param ctx  the user data (XML parser context)
 * @returns 1 if true
 */
int
xmlSAX2HasInternalSubset(void *ctx)
{
    xmlParserCtxtPtr ctxt = (xmlParserCtxtPtr) ctx;
    if ((ctxt == NULL) || (ctxt->myDoc == NULL)) return(0);
    return(ctxt->myDoc->intSubset != NULL);
}

/**
 * Does this document has an external subset
 *
 * @param ctx  the user data (XML parser context)
 * @returns 1 if true
 */
int
xmlSAX2HasExternalSubset(void *ctx)
{
    xmlParserCtxtPtr ctxt = (xmlParserCtxtPtr) ctx;
    if ((ctxt == NULL) || (ctxt->myDoc == NULL)) return(0);
    return(ctxt->myDoc->extSubset != NULL);
}

/**
 * Callback on internal subset declaration.
 *
 * @param ctx  the user data (XML parser context)
 * @param name  the root element name
 * @param publicId  public identifier of the DTD (optional)
 * @param systemId  system identifier (URL) of the DTD
 */
void
xmlSAX2InternalSubset(void *ctx, const xmlChar *name,
	       const xmlChar *publicId, const xmlChar *systemId)
{
    xmlParserCtxtPtr ctxt = (xmlParserCtxtPtr) ctx;
    xmlDtdPtr dtd;
    if (ctx == NULL) return;

    if (ctxt->myDoc == NULL)
	return;
    if ((ctxt->html) && (ctxt->instate != XML_PARSER_MISC))
        return;
    dtd = xmlGetIntSubset(ctxt->myDoc);
    if (dtd != NULL) {
	xmlUnlinkNode((xmlNodePtr) dtd);
	xmlFreeDtd(dtd);
	ctxt->myDoc->intSubset = NULL;
    }
    ctxt->myDoc->intSubset =
	xmlCreateIntSubset(ctxt->myDoc, name, publicId, systemId);
    if (ctxt->myDoc->intSubset == NULL)
        xmlSAX2ErrMemory(ctxt);
}

/**
 * Callback on external subset declaration.
 *
 * @param ctx  the user data (XML parser context)
 * @param name  the root element name
 * @param publicId  public identifier of the DTD (optional)
 * @param systemId  system identifier (URL) of the DTD
 */
void
xmlSAX2ExternalSubset(void *ctx, const xmlChar *name,
	       const xmlChar *publicId, const xmlChar *systemId)
{
    xmlParserCtxtPtr ctxt = (xmlParserCtxtPtr) ctx;
    if (ctx == NULL) return;
    if ((systemId != NULL) &&
        ((ctxt->options & XML_PARSE_NO_XXE) == 0) &&
        (((ctxt->validate) || (ctxt->loadsubset & ~XML_SKIP_IDS)) &&
	 (ctxt->wellFormed && ctxt->myDoc))) {
	/*
	 * Try to fetch and parse the external subset.
	 */
	xmlParserInputPtr oldinput;
	int oldinputNr;
	int oldinputMax;
	xmlParserInputPtr *oldinputTab;
	xmlParserInputPtr input = NULL;
	xmlChar *oldencoding;
        unsigned long consumed;
        size_t buffered;
#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
        int inputMax = 1;
#else
        int inputMax = 5;
#endif

	/*
	 * Ask the Entity resolver to load the damn thing
	 */
	if ((ctxt->sax != NULL) && (ctxt->sax->resolveEntity != NULL))
	    input = ctxt->sax->resolveEntity(ctxt->userData, publicId,
	                                     systemId);
	if (input == NULL) {
	    return;
	}

	if (xmlNewDtd(ctxt->myDoc, name, publicId, systemId) == NULL) {
            xmlSAX2ErrMemory(ctxt);
            xmlFreeInputStream(input);
            return;
        }

	/*
	 * make sure we won't destroy the main document context
	 */
	oldinput = ctxt->input;
	oldinputNr = ctxt->inputNr;
	oldinputMax = ctxt->inputMax;
	oldinputTab = ctxt->inputTab;
	oldencoding = ctxt->encoding;
	ctxt->encoding = NULL;

	ctxt->inputTab = xmlMalloc(inputMax * sizeof(xmlParserInputPtr));
	if (ctxt->inputTab == NULL) {
	    xmlSAX2ErrMemory(ctxt);
            goto error;
	}
	ctxt->inputNr = 0;
	ctxt->inputMax = inputMax;
	ctxt->input = NULL;
	if (xmlCtxtPushInput(ctxt, input) < 0)
            goto error;

	if (input->filename == NULL)
	    input->filename = (char *) xmlCanonicPath(systemId);
	input->line = 1;
	input->col = 1;
	input->base = ctxt->input->cur;
	input->cur = ctxt->input->cur;
	input->free = NULL;

	/*
	 * let's parse that entity knowing it's an external subset.
	 */
	xmlParseExternalSubset(ctxt, publicId, systemId);

        /*
	 * Free up the external entities
	 */

	while (ctxt->inputNr > 1)
	    xmlFreeInputStream(xmlCtxtPopInput(ctxt));

        consumed = ctxt->input->consumed;
        buffered = ctxt->input->cur - ctxt->input->base;
        if (buffered > ULONG_MAX - consumed)
            consumed = ULONG_MAX;
        else
            consumed += buffered;
        if (consumed > ULONG_MAX - ctxt->sizeentities)
            ctxt->sizeentities = ULONG_MAX;
        else
            ctxt->sizeentities += consumed;

error:
	xmlFreeInputStream(input);
        xmlFree(ctxt->inputTab);

	/*
	 * Restore the parsing context of the main entity
	 */
	ctxt->input = oldinput;
	ctxt->inputNr = oldinputNr;
	ctxt->inputMax = oldinputMax;
	ctxt->inputTab = oldinputTab;
	if (ctxt->encoding != NULL)
	    xmlFree(ctxt->encoding);
	ctxt->encoding = oldencoding;
	/* ctxt->wellFormed = oldwellFormed; */
    }
}

/**
 * This is only used to load DTDs. The preferred way to install
 * custom resolvers is #xmlCtxtSetResourceLoader.
 *
 * @param ctx  the user data (XML parser context)
 * @param publicId  The public ID of the entity
 * @param systemId  The system ID (URL) of the entity
 * @returns a parser input.
 */
xmlParserInput *
xmlSAX2ResolveEntity(void *ctx, const xmlChar *publicId,
                     const xmlChar *systemId)
{
    xmlParserCtxtPtr ctxt = (xmlParserCtxtPtr) ctx;
    xmlParserInputPtr ret = NULL;
    xmlChar *URI = NULL;

    if (ctx == NULL) return(NULL);

    if (systemId != NULL) {
        const xmlChar *base = NULL;
        int res;

        if (ctxt->input != NULL)
            base = BAD_CAST ctxt->input->filename;

        /*
         * We don't really need the 'directory' struct member, but some
         * users set it manually to a base URI for memory streams.
         */
        if (base == NULL)
            base = BAD_CAST ctxt->directory;

        if ((xmlStrlen(systemId) > XML_MAX_URI_LENGTH) ||
            (xmlStrlen(base) > XML_MAX_URI_LENGTH)) {
            xmlFatalErr(ctxt, XML_ERR_RESOURCE_LIMIT, "URI too long");
            return(NULL);
        }
        res = xmlBuildURISafe(systemId, base, &URI);
        if (URI == NULL) {
            if (res < 0)
                xmlSAX2ErrMemory(ctxt);
            else
                xmlWarnMsg(ctxt, XML_ERR_INVALID_URI,
                           "Can't resolve URI: %s\n", systemId);
            return(NULL);
        }
        if (xmlStrlen(URI) > XML_MAX_URI_LENGTH) {
            xmlFatalErr(ctxt, XML_ERR_RESOURCE_LIMIT, "URI too long");
            xmlFree(URI);
            return(NULL);
        }
    }

    ret = xmlLoadResource(ctxt, (const char *) URI,
                          (const char *) publicId, XML_RESOURCE_DTD);

    xmlFree(URI);
    return(ret);
}

/**
 * Get an entity by name
 *
 * @param ctx  the user data (XML parser context)
 * @param name  The entity name
 * @returns the xmlEntity if found.
 */
xmlEntity *
xmlSAX2GetEntity(void *ctx, const xmlChar *name)
{
    xmlParserCtxtPtr ctxt = (xmlParserCtxtPtr) ctx;
    xmlEntityPtr ret = NULL;

    if (ctx == NULL) return(NULL);

    if (ctxt->inSubset == 0) {
	ret = xmlGetPredefinedEntity(name);
	if (ret != NULL)
	    return(ret);
    }
    if ((ctxt->myDoc != NULL) && (ctxt->myDoc->standalone == 1)) {
	if (ctxt->inSubset == 2) {
	    ctxt->myDoc->standalone = 0;
	    ret = xmlGetDocEntity(ctxt->myDoc, name);
	    ctxt->myDoc->standalone = 1;
	} else {
	    ret = xmlGetDocEntity(ctxt->myDoc, name);
	    if (ret == NULL) {
		ctxt->myDoc->standalone = 0;
		ret = xmlGetDocEntity(ctxt->myDoc, name);
		if (ret != NULL) {
		    xmlFatalErrMsg(ctxt, XML_ERR_NOT_STANDALONE,
	 "Entity(%s) document marked standalone but requires external subset\n",
				   name, NULL);
		}
		ctxt->myDoc->standalone = 1;
	    }
	}
    } else {
	ret = xmlGetDocEntity(ctxt->myDoc, name);
    }
    return(ret);
}

/**
 * Get a parameter entity by name
 *
 * @param ctx  the user data (XML parser context)
 * @param name  The entity name
 * @returns the xmlEntity if found.
 */
xmlEntity *
xmlSAX2GetParameterEntity(void *ctx, const xmlChar *name)
{
    xmlParserCtxtPtr ctxt = (xmlParserCtxtPtr) ctx;
    xmlEntityPtr ret;

    if (ctx == NULL) return(NULL);

    ret = xmlGetParameterEntity(ctxt->myDoc, name);
    return(ret);
}


/**
 * An entity definition has been parsed
 *
 * @param ctx  the user data (XML parser context)
 * @param name  the entity name
 * @param type  the entity type
 * @param publicId  The public ID of the entity
 * @param systemId  The system ID of the entity
 * @param content  the entity value (without processing).
 */
void
xmlSAX2EntityDecl(void *ctx, const xmlChar *name, int type,
          const xmlChar *publicId, const xmlChar *systemId, xmlChar *content)
{
    xmlEntityPtr ent;
    xmlParserCtxtPtr ctxt = (xmlParserCtxtPtr) ctx;
    int extSubset;
    int res;

    if ((ctxt == NULL) || (ctxt->myDoc == NULL))
        return;

    extSubset = ctxt->inSubset == 2;
    res = xmlAddEntity(ctxt->myDoc, extSubset, name, type, publicId, systemId,
                       content, &ent);
    switch (res) {
        case XML_ERR_OK:
            break;
        case XML_ERR_NO_MEMORY:
            xmlSAX2ErrMemory(ctxt);
            return;
        case XML_WAR_ENTITY_REDEFINED:
            if (ctxt->pedantic) {
                if (extSubset)
                    xmlWarnMsg(ctxt, res, "Entity(%s) already defined in the"
                               " external subset\n", name);
                else
                    xmlWarnMsg(ctxt, res, "Entity(%s) already defined in the"
                               " internal subset\n", name);
            }
            return;
        case XML_ERR_REDECL_PREDEF_ENTITY:
            /*
             * Technically an error but it's a common mistake to get double
             * escaping according to "4.6 Predefined Entities" wrong.
             */
            xmlWarnMsg(ctxt, res, "Invalid redeclaration of predefined"
                       " entity '%s'", name);
            return;
        default:
            xmlFatalErrMsg(ctxt, XML_ERR_INTERNAL_ERROR,
                           "Unexpected error code from xmlAddEntity\n",
                           NULL, NULL);
            return;
    }

    if ((ent->URI == NULL) && (systemId != NULL)) {
        xmlChar *URI;
        const char *base = NULL;
        int i;

        for (i = ctxt->inputNr - 1; i >= 0; i--) {
            if (ctxt->inputTab[i]->filename != NULL) {
                base = ctxt->inputTab[i]->filename;
                break;
            }
        }

        /*
         * We don't really need the 'directory' struct member, but some
         * users set it manually to a base URI for memory streams.
         */
        if (base == NULL)
            base = ctxt->directory;

        res = xmlBuildURISafe(systemId, (const xmlChar *) base, &URI);

        if (URI == NULL) {
            if (res < 0) {
                xmlSAX2ErrMemory(ctxt);
            } else {
                xmlWarnMsg(ctxt, XML_ERR_INVALID_URI,
                           "Can't resolve URI: %s\n", systemId);
            }
        } else if (xmlStrlen(URI) > XML_MAX_URI_LENGTH) {
            xmlFatalErr(ctxt, XML_ERR_RESOURCE_LIMIT, "URI too long");
            xmlFree(URI);
        } else {
            ent->URI = URI;
        }
    }
}

/**
 * An attribute definition has been parsed
 *
 * @param ctx  the user data (XML parser context)
 * @param elem  the name of the element
 * @param fullname  the attribute name
 * @param type  the attribute type
 * @param def  the type of default value
 * @param defaultValue  the attribute default value
 * @param tree  the tree of enumerated value set
 */
void
xmlSAX2AttributeDecl(void *ctx, const xmlChar *elem, const xmlChar *fullname,
              int type, int def, const xmlChar *defaultValue,
	      xmlEnumeration *tree)
{
    xmlParserCtxtPtr ctxt = (xmlParserCtxtPtr) ctx;
    xmlAttributePtr attr;
    const xmlChar *name = NULL;
    xmlChar *prefix = NULL;

    /* Avoid unused variable warning if features are disabled. */
    (void) attr;

    if ((ctxt == NULL) || (ctxt->myDoc == NULL))
        return;

    if ((xmlStrEqual(fullname, BAD_CAST "xml:id")) &&
        (type != XML_ATTRIBUTE_ID)) {
	xmlErrId(ctxt, XML_DTD_XMLID_TYPE,
	      "xml:id : attribute type should be ID\n", NULL);
    }
    name = xmlSplitQName4(fullname, &prefix);
    if (name == NULL)
        xmlSAX2ErrMemory(ctxt);
    ctxt->vctxt.valid = 1;
    if (ctxt->inSubset == 1)
	attr = xmlAddAttributeDecl(&ctxt->vctxt, ctxt->myDoc->intSubset, elem,
	       name, prefix, (xmlAttributeType) type,
	       (xmlAttributeDefault) def, defaultValue, tree);
    else if (ctxt->inSubset == 2)
	attr = xmlAddAttributeDecl(&ctxt->vctxt, ctxt->myDoc->extSubset, elem,
	   name, prefix, (xmlAttributeType) type,
	   (xmlAttributeDefault) def, defaultValue, tree);
    else {
        xmlFatalErrMsg(ctxt, XML_ERR_INTERNAL_ERROR,
	     "SAX.xmlSAX2AttributeDecl(%s) called while not in subset\n",
	               name, NULL);
	xmlFree(prefix);
	xmlFreeEnumeration(tree);
	return;
    }
#ifdef LIBXML_VALID_ENABLED
    if (ctxt->vctxt.valid == 0)
	ctxt->valid = 0;
    if ((attr != NULL) && (ctxt->validate) && (ctxt->wellFormed) &&
        (ctxt->myDoc->intSubset != NULL))
	ctxt->valid &= xmlValidateAttributeDecl(&ctxt->vctxt, ctxt->myDoc,
	                                        attr);
#endif /* LIBXML_VALID_ENABLED */
    if (prefix != NULL)
	xmlFree(prefix);
}

/**
 * An element definition has been parsed
 *
 * @param ctx  the user data (XML parser context)
 * @param name  the element name
 * @param type  the element type
 * @param content  the element value tree
 */
void
xmlSAX2ElementDecl(void *ctx, const xmlChar * name, int type,
            xmlElementContent *content)
{
    xmlParserCtxtPtr ctxt = (xmlParserCtxtPtr) ctx;
    xmlElementPtr elem = NULL;

    /* Avoid unused variable warning if features are disabled. */
    (void) elem;

    if ((ctxt == NULL) || (ctxt->myDoc == NULL))
        return;

    if (ctxt->inSubset == 1)
        elem = xmlAddElementDecl(&ctxt->vctxt, ctxt->myDoc->intSubset,
                                 name, (xmlElementTypeVal) type, content);
    else if (ctxt->inSubset == 2)
        elem = xmlAddElementDecl(&ctxt->vctxt, ctxt->myDoc->extSubset,
                                 name, (xmlElementTypeVal) type, content);
    else {
        xmlFatalErrMsg(ctxt, XML_ERR_INTERNAL_ERROR,
	     "SAX.xmlSAX2ElementDecl(%s) called while not in subset\n",
	               name, NULL);
        return;
    }
#ifdef LIBXML_VALID_ENABLED
    if (elem == NULL)
        ctxt->valid = 0;
    if (ctxt->validate && ctxt->wellFormed &&
        ctxt->myDoc && ctxt->myDoc->intSubset)
        ctxt->valid &=
            xmlValidateElementDecl(&ctxt->vctxt, ctxt->myDoc, elem);
#endif /* LIBXML_VALID_ENABLED */
}

/**
 * What to do when a notation declaration has been parsed.
 *
 * @param ctx  the user data (XML parser context)
 * @param name  The name of the notation
 * @param publicId  The public ID of the entity
 * @param systemId  The system ID of the entity
 */
void
xmlSAX2NotationDecl(void *ctx, const xmlChar *name,
	     const xmlChar *publicId, const xmlChar *systemId)
{
    xmlParserCtxtPtr ctxt = (xmlParserCtxtPtr) ctx;
    xmlNotationPtr nota = NULL;

    /* Avoid unused variable warning if features are disabled. */
    (void) nota;

    if ((ctxt == NULL) || (ctxt->myDoc == NULL))
        return;

    if ((publicId == NULL) && (systemId == NULL)) {
	xmlFatalErrMsg(ctxt, XML_ERR_NOTATION_PROCESSING,
	     "SAX.xmlSAX2NotationDecl(%s) externalID or PublicID missing\n",
	               name, NULL);
	return;
    } else if (ctxt->inSubset == 1)
	nota = xmlAddNotationDecl(&ctxt->vctxt, ctxt->myDoc->intSubset, name,
                              publicId, systemId);
    else if (ctxt->inSubset == 2)
	nota = xmlAddNotationDecl(&ctxt->vctxt, ctxt->myDoc->extSubset, name,
                              publicId, systemId);
    else {
	xmlFatalErrMsg(ctxt, XML_ERR_NOTATION_PROCESSING,
	     "SAX.xmlSAX2NotationDecl(%s) called while not in subset\n",
	               name, NULL);
	return;
    }
#ifdef LIBXML_VALID_ENABLED
    if (nota == NULL) ctxt->valid = 0;
    if ((ctxt->validate) && (ctxt->wellFormed) &&
        (ctxt->myDoc->intSubset != NULL))
	ctxt->valid &= xmlValidateNotationDecl(&ctxt->vctxt, ctxt->myDoc,
	                                       nota);
#endif /* LIBXML_VALID_ENABLED */
}

/**
 * What to do when an unparsed entity declaration is parsed
 *
 * @param ctx  the user data (XML parser context)
 * @param name  The name of the entity
 * @param publicId  The public ID of the entity
 * @param systemId  The system ID of the entity
 * @param notationName  the name of the notation
 */
void
xmlSAX2UnparsedEntityDecl(void *ctx, const xmlChar *name,
		   const xmlChar *publicId, const xmlChar *systemId,
		   const xmlChar *notationName)
{
    xmlSAX2EntityDecl(ctx, name, XML_EXTERNAL_GENERAL_UNPARSED_ENTITY,
                      publicId, systemId, (xmlChar *) notationName);
}

/**
 * Receive the document locator at startup, actually xmlDefaultSAXLocator
 * Everything is available on the context, so this is useless in our case.
 *
 * @param ctx  the user data (XML parser context)
 * @param loc  A SAX Locator
 */
void
xmlSAX2SetDocumentLocator(void *ctx ATTRIBUTE_UNUSED, xmlSAXLocator *loc ATTRIBUTE_UNUSED)
{
}

/**
 * called when the document start being processed.
 *
 * @param ctx  the user data (XML parser context)
 */
void
xmlSAX2StartDocument(void *ctx)
{
    xmlParserCtxtPtr ctxt = (xmlParserCtxtPtr) ctx;
    xmlDocPtr doc;

    if (ctx == NULL) return;

#ifdef LIBXML_HTML_ENABLED
    if (ctxt->html) {
	if (ctxt->myDoc == NULL)
	    ctxt->myDoc = htmlNewDocNoDtD(NULL, NULL);
	if (ctxt->myDoc == NULL) {
	    xmlSAX2ErrMemory(ctxt);
	    return;
	}
	ctxt->myDoc->properties = XML_DOC_HTML;
	ctxt->myDoc->parseFlags = ctxt->options;
    } else
#endif
    {
	doc = ctxt->myDoc = xmlNewDoc(ctxt->version);
	if (doc != NULL) {
	    doc->properties = 0;
	    if (ctxt->options & XML_PARSE_OLD10)
	        doc->properties |= XML_DOC_OLD10;
	    doc->parseFlags = ctxt->options;
	    doc->standalone = ctxt->standalone;
	} else {
	    xmlSAX2ErrMemory(ctxt);
	    return;
	}
	if ((ctxt->dictNames) && (doc != NULL)) {
	    doc->dict = ctxt->dict;
	    xmlDictReference(doc->dict);
	}
    }
    if ((ctxt->myDoc != NULL) && (ctxt->myDoc->URL == NULL) &&
	(ctxt->input != NULL) && (ctxt->input->filename != NULL)) {
	ctxt->myDoc->URL = xmlPathToURI((const xmlChar *)ctxt->input->filename);
	if (ctxt->myDoc->URL == NULL)
	    xmlSAX2ErrMemory(ctxt);
    }
}

/**
 * called when the document end has been detected.
 *
 * @param ctx  the user data (XML parser context)
 */
void
xmlSAX2EndDocument(void *ctx)
{
    xmlParserCtxtPtr ctxt = (xmlParserCtxtPtr) ctx;
    xmlDocPtr doc;

    if (ctx == NULL) return;
#ifdef LIBXML_VALID_ENABLED
    if (ctxt->validate && ctxt->wellFormed &&
        ctxt->myDoc && ctxt->myDoc->intSubset)
	ctxt->valid &= xmlValidateDocumentFinal(&ctxt->vctxt, ctxt->myDoc);
#endif /* LIBXML_VALID_ENABLED */

    doc = ctxt->myDoc;
    if (doc == NULL)
        return;

    if (doc->encoding == NULL) {
        const xmlChar *encoding = xmlGetActualEncoding(ctxt);

        if (encoding != NULL) {
            doc->encoding = xmlStrdup(encoding);
            if (doc->encoding == NULL)
                xmlSAX2ErrMemory(ctxt);
        }
    }

#ifdef LIBXML_HTML_ENABLED
    if (ctxt->html) {
        if (((ctxt->options & HTML_PARSE_NODEFDTD) == 0) &&
            (doc->intSubset == NULL)) {
            doc->intSubset = xmlCreateIntSubset(doc, BAD_CAST "html",
                    BAD_CAST "-//W3C//DTD HTML 4.0 Transitional//EN",
                    BAD_CAST "http://www.w3.org/TR/REC-html40/loose.dtd");
            if (doc->intSubset == NULL)
                xmlSAX2ErrMemory(ctxt);
        }
    } else
#endif /* LIBXML_HTML_ENABLED */
    {
        if (ctxt->wellFormed) {
            doc->properties |= XML_DOC_WELLFORMED;
            if (ctxt->valid)
                doc->properties |= XML_DOC_DTDVALID;
            if (ctxt->nsWellFormed)
                doc->properties |= XML_DOC_NSVALID;
        }

        if (ctxt->options & XML_PARSE_OLD10)
            doc->properties |= XML_DOC_OLD10;
    }
}

static void
xmlSAX2AppendChild(xmlParserCtxtPtr ctxt, xmlNodePtr node) {
    xmlNodePtr parent;
    xmlNodePtr last;

    if (ctxt->inSubset == 1) {
	parent = (xmlNodePtr) ctxt->myDoc->intSubset;
    } else if (ctxt->inSubset == 2) {
	parent = (xmlNodePtr) ctxt->myDoc->extSubset;
    } else {
        parent = ctxt->node;
        if (parent == NULL)
            parent = (xmlNodePtr) ctxt->myDoc;
    }

    last = parent->last;
    if (last == NULL) {
        parent->children = node;
    } else {
        last->next = node;
        node->prev = last;
    }

    parent->last = node;
    node->parent = parent;

    if ((node->type != XML_TEXT_NODE) &&
	(ctxt->input != NULL)) {
        if ((unsigned) ctxt->input->line < (unsigned) USHRT_MAX)
            node->line = ctxt->input->line;
        else
            node->line = USHRT_MAX;
    }
}

#if defined(LIBXML_SAX1_ENABLED)
/**
 * Handle a namespace error
 *
 * @param ctxt  an XML parser context
 * @param error  the error number
 * @param msg  the error message
 * @param str1  an error string
 * @param str2  an error string
 */
static void LIBXML_ATTR_FORMAT(3,0)
xmlNsErrMsg(xmlParserCtxtPtr ctxt, xmlParserErrors error,
            const char *msg, const xmlChar *str1, const xmlChar *str2)
{
    xmlCtxtErr(ctxt, NULL, XML_FROM_NAMESPACE, error, XML_ERR_ERROR,
               str1, str2, NULL, 0, msg, str1, str2);
}

/**
 * Handle an attribute that has been read by the parser.
 *
 * Deprecated SAX1 interface.
 *
 * @param ctxt  the parser context
 * @param fullname  the attribute name, including namespace prefix
 * @param value  the attribute value
 * @param prefix  the namespace prefix
 */
static void
xmlSAX1Attribute(xmlParserCtxtPtr ctxt, const xmlChar *fullname,
                 const xmlChar *value, const xmlChar *prefix)
{
    xmlAttrPtr ret;
    const xmlChar *name;
    xmlChar *ns;
    xmlNsPtr namespace;

    /*
     * Split the full name into a namespace prefix and the tag name
     */
    name = xmlSplitQName4(fullname, &ns);
    if (name == NULL) {
        xmlSAX2ErrMemory(ctxt);
        return;
    }

    /*
     * Check whether it's a namespace definition
     */
    if ((ns == NULL) &&
        (name[0] == 'x') && (name[1] == 'm') && (name[2] == 'l') &&
        (name[3] == 'n') && (name[4] == 's') && (name[5] == 0)) {
	xmlNsPtr nsret;
	xmlChar *val;

        /* Avoid unused variable warning if features are disabled. */
        (void) nsret;

        if (!ctxt->replaceEntities) {
            /* TODO: normalize if needed */
	    val = xmlExpandEntitiesInAttValue(ctxt, value, /* normalize */ 0);
	    if (val == NULL) {
	        xmlSAX2ErrMemory(ctxt);
		return;
	    }
	} else {
	    val = (xmlChar *) value;
	}

	if (val[0] != 0) {
	    xmlURIPtr uri;

	    if (xmlParseURISafe((const char *)val, &uri) < 0)
                xmlSAX2ErrMemory(ctxt);
	    if (uri == NULL) {
                xmlNsWarnMsg(ctxt, XML_WAR_NS_URI,
                             "xmlns:%s: %s not a valid URI\n", name, value);
	    } else {
		if (uri->scheme == NULL) {
                    xmlNsWarnMsg(ctxt, XML_WAR_NS_URI_RELATIVE,
                                 "xmlns:%s: URI %s is not absolute\n",
                                 name, value);
		}
		xmlFreeURI(uri);
	    }
	}

	/* a default namespace definition */
	nsret = xmlNewNs(ctxt->node, val, NULL);
        if (nsret == NULL) {
            xmlSAX2ErrMemory(ctxt);
        }
#ifdef LIBXML_VALID_ENABLED
	/*
	 * Validate also for namespace decls, they are attributes from
	 * an XML-1.0 perspective
	 */
        else if (ctxt->validate && ctxt->wellFormed &&
                 ctxt->myDoc && ctxt->myDoc->intSubset) {
	    ctxt->valid &= xmlValidateOneNamespace(&ctxt->vctxt, ctxt->myDoc,
					   ctxt->node, prefix, nsret, val);
        }
#endif /* LIBXML_VALID_ENABLED */
	if (val != value)
	    xmlFree(val);
	return;
    }
    if ((ns != NULL) && (ns[0] == 'x') && (ns[1] == 'm') && (ns[2] == 'l') &&
        (ns[3] == 'n') && (ns[4] == 's') && (ns[5] == 0)) {
	xmlNsPtr nsret;
	xmlChar *val;

        /* Avoid unused variable warning if features are disabled. */
        (void) nsret;

        if (!ctxt->replaceEntities) {
            /* TODO: normalize if needed */
	    val = xmlExpandEntitiesInAttValue(ctxt, value, /* normalize */ 0);
	    if (val == NULL) {
	        xmlSAX2ErrMemory(ctxt);
	        xmlFree(ns);
		return;
	    }
	} else {
	    val = (xmlChar *) value;
	}

	if (val[0] == 0) {
	    xmlNsErrMsg(ctxt, XML_NS_ERR_EMPTY,
		        "Empty namespace name for prefix %s\n", name, NULL);
	}
	if ((ctxt->pedantic != 0) && (val[0] != 0)) {
	    xmlURIPtr uri;

	    if (xmlParseURISafe((const char *)val, &uri) < 0)
                xmlSAX2ErrMemory(ctxt);
	    if (uri == NULL) {
	        xmlNsWarnMsg(ctxt, XML_WAR_NS_URI,
			 "xmlns:%s: %s not a valid URI\n", name, value);
	    } else {
		if (uri->scheme == NULL) {
		    xmlNsWarnMsg(ctxt, XML_WAR_NS_URI_RELATIVE,
			   "xmlns:%s: URI %s is not absolute\n", name, value);
		}
		xmlFreeURI(uri);
	    }
	}

	/* a standard namespace definition */
	nsret = xmlNewNs(ctxt->node, val, name);
	xmlFree(ns);

        if (nsret == NULL) {
            xmlSAX2ErrMemory(ctxt);
        }
#ifdef LIBXML_VALID_ENABLED
	/*
	 * Validate also for namespace decls, they are attributes from
	 * an XML-1.0 perspective
	 */
        else if (ctxt->validate && ctxt->wellFormed &&
	         ctxt->myDoc && ctxt->myDoc->intSubset) {
	    ctxt->valid &= xmlValidateOneNamespace(&ctxt->vctxt, ctxt->myDoc,
					   ctxt->node, prefix, nsret, value);
        }
#endif /* LIBXML_VALID_ENABLED */
	if (val != value)
	    xmlFree(val);
	return;
    }

    if (ns != NULL) {
        int res;

	res = xmlSearchNsSafe(ctxt->node, ns, &namespace);
        if (res < 0)
            xmlSAX2ErrMemory(ctxt);

	if (namespace == NULL) {
	    xmlNsErrMsg(ctxt, XML_NS_ERR_UNDEFINED_NAMESPACE,
		    "Namespace prefix %s of attribute %s is not defined\n",
		             ns, name);
	} else {
            xmlAttrPtr prop;

            prop = ctxt->node->properties;
            while (prop != NULL) {
                if (prop->ns != NULL) {
                    if ((xmlStrEqual(name, prop->name)) &&
                        ((namespace == prop->ns) ||
                         (xmlStrEqual(namespace->href, prop->ns->href)))) {
                        xmlCtxtErr(ctxt, NULL, XML_FROM_PARSER,
                                   XML_ERR_ATTRIBUTE_REDEFINED, XML_ERR_FATAL,
                                   name, NULL, NULL, 0,
                                   "Attribute %s in %s redefined\n",
                                   name, namespace->href);
                        goto error;
                    }
                }
                prop = prop->next;
            }
        }
    } else {
	namespace = NULL;
    }

    /* !!!!!! <a toto:arg="" xmlns:toto="http://toto.com"> */
    ret = xmlNewNsProp(ctxt->node, namespace, name, NULL);
    if (ret == NULL) {
        xmlSAX2ErrMemory(ctxt);
        goto error;
    }

    if (ctxt->replaceEntities == 0) {
        if (xmlNodeParseAttValue(ret->doc, ret, value, SIZE_MAX, NULL) < 0)
            xmlSAX2ErrMemory(ctxt);
    } else if (value != NULL) {
        ret->children = xmlNewDocText(ctxt->myDoc, value);
        if (ret->children == NULL) {
            xmlSAX2ErrMemory(ctxt);
        } else {
            ret->last = ret->children;
            ret->children->parent = (xmlNodePtr) ret;
        }
    }

#ifdef LIBXML_VALID_ENABLED
    if (ctxt->validate && ctxt->wellFormed &&
        ctxt->myDoc && ctxt->myDoc->intSubset) {

	/*
	 * If we don't substitute entities, the validation should be
	 * done on a value with replaced entities anyway.
	 */
        if (!ctxt->replaceEntities) {
	    xmlChar *val;

            /* TODO: normalize if needed */
	    val = xmlExpandEntitiesInAttValue(ctxt, value, /* normalize */ 0);

	    if (val == NULL)
		ctxt->valid &= xmlValidateOneAttribute(&ctxt->vctxt,
				ctxt->myDoc, ctxt->node, ret, value);
	    else {
		xmlChar *nvalnorm;

		/*
		 * Do the last stage of the attribute normalization
		 * It need to be done twice ... it's an extra burden related
		 * to the ability to keep xmlSAX2References in attributes
		 */
                nvalnorm = xmlValidCtxtNormalizeAttributeValue(
                                 &ctxt->vctxt, ctxt->myDoc,
                                 ctxt->node, fullname, val);
		if (nvalnorm != NULL) {
		    xmlFree(val);
		    val = nvalnorm;
		}

		ctxt->valid &= xmlValidateOneAttribute(&ctxt->vctxt,
			        ctxt->myDoc, ctxt->node, ret, val);
                xmlFree(val);
	    }
	} else {
            /*
             * When replacing entities, make sure that IDs in
             * entities aren't registered. This also shouldn't be
             * done when entities aren't replaced, but this would
             * require to rework IDREF checks.
             */
            if (ctxt->input->entity != NULL)
                ctxt->vctxt.flags |= XML_VCTXT_IN_ENTITY;

	    ctxt->valid &= xmlValidateOneAttribute(&ctxt->vctxt, ctxt->myDoc,
					       ctxt->node, ret, value);

            ctxt->vctxt.flags &= ~XML_VCTXT_IN_ENTITY;
	}
    } else
#endif /* LIBXML_VALID_ENABLED */
           if (((ctxt->loadsubset & XML_SKIP_IDS) == 0) &&
               (ctxt->input->entity == NULL) &&
               /* Don't create IDs containing entity references */
               (ret->children != NULL) &&
               (ret->children->type == XML_TEXT_NODE) &&
               (ret->children->next == NULL)) {
        xmlChar *content = ret->children->content;
        /*
	 * when validating, the ID registration is done at the attribute
	 * validation level. Otherwise we have to do specific handling here.
	 */
	if (xmlStrEqual(fullname, BAD_CAST "xml:id")) {
	    /*
	     * Add the xml:id value
	     *
	     * Open issue: normalization of the value.
	     */
	    if (xmlValidateNCName(content, 1) != 0) {
	        xmlErrId(ctxt, XML_DTD_XMLID_VALUE,
		         "xml:id : attribute value %s is not an NCName\n",
		         content);
	    }
	    xmlAddID(&ctxt->vctxt, ctxt->myDoc, content, ret);
	} else {
            int res = xmlIsID(ctxt->myDoc, ctxt->node, ret);

            if (res < 0)
                xmlCtxtErrMemory(ctxt);
            else if (res > 0)
                xmlAddID(&ctxt->vctxt, ctxt->myDoc, content, ret);
            else if (xmlIsRef(ctxt->myDoc, ctxt->node, ret))
                xmlAddRef(&ctxt->vctxt, ctxt->myDoc, content, ret);
        }
    }

error:
    if (ns != NULL)
	xmlFree(ns);
}

/*
 *
 * Check defaulted attributes from the DTD
 *
 * Deprecated SAX1 interface.
 */
static void
xmlCheckDefaultedAttributes(xmlParserCtxtPtr ctxt, const xmlChar *name,
	const xmlChar *prefix, const xmlChar **atts) {
    xmlElementPtr elemDecl;
    const xmlChar *att;
    int internal = 1;
    int i;

    elemDecl = xmlGetDtdQElementDesc(ctxt->myDoc->intSubset, name, prefix);
    if (elemDecl == NULL) {
	elemDecl = xmlGetDtdQElementDesc(ctxt->myDoc->extSubset, name, prefix);
	internal = 0;
    }

process_external_subset:

    if (elemDecl != NULL) {
	xmlAttributePtr attr = elemDecl->attributes;

#ifdef LIBXML_VALID_ENABLED
        /*
         * Check against defaulted attributes from the external subset
         * if the document is stamped as standalone.
         *
         * This should be moved to valid.c, but we don't keep track
         * whether an attribute was defaulted.
         */
	if ((ctxt->myDoc->standalone == 1) &&
	    (ctxt->myDoc->extSubset != NULL) &&
	    (ctxt->validate)) {
	    while (attr != NULL) {
		if ((attr->defaultValue != NULL) &&
		    (xmlGetDtdQAttrDesc(ctxt->myDoc->extSubset,
					attr->elem, attr->name,
					attr->prefix) == attr) &&
		    (xmlGetDtdQAttrDesc(ctxt->myDoc->intSubset,
					attr->elem, attr->name,
					attr->prefix) == NULL)) {
		    xmlChar *fulln;

		    if (attr->prefix != NULL) {
			fulln = xmlStrdup(attr->prefix);
                        if (fulln != NULL)
			    fulln = xmlStrcat(fulln, BAD_CAST ":");
                        if (fulln != NULL)
			    fulln = xmlStrcat(fulln, attr->name);
		    } else {
			fulln = xmlStrdup(attr->name);
		    }
                    if (fulln == NULL) {
                        xmlSAX2ErrMemory(ctxt);
                        break;
                    }

		    /*
		     * Check that the attribute is not declared in the
		     * serialization
		     */
		    att = NULL;
		    if (atts != NULL) {
			i = 0;
			att = atts[i];
			while (att != NULL) {
			    if (xmlStrEqual(att, fulln))
				break;
			    i += 2;
			    att = atts[i];
			}
		    }
		    if (att == NULL) {
		        xmlErrValid(ctxt, XML_DTD_STANDALONE_DEFAULTED,
      "standalone: attribute %s on %s defaulted from external subset\n",
				    fulln,
				    attr->elem);
		    }
                    xmlFree(fulln);
		}
		attr = attr->nexth;
	    }
	}
#endif

	/*
	 * Actually insert defaulted values when needed
	 */
	attr = elemDecl->attributes;
	while (attr != NULL) {
	    /*
	     * Make sure that attributes redefinition occurring in the
	     * internal subset are not overridden by definitions in the
	     * external subset.
	     */
	    if (attr->defaultValue != NULL) {
		/*
		 * the element should be instantiated in the tree if:
		 *  - this is a namespace prefix
		 *  - the user required for completion in the tree
		 *    like XSLT
		 *  - there isn't already an attribute definition
		 *    in the internal subset overriding it.
		 */
		if (((attr->prefix != NULL) &&
		     (xmlStrEqual(attr->prefix, BAD_CAST "xmlns"))) ||
		    ((attr->prefix == NULL) &&
		     (xmlStrEqual(attr->name, BAD_CAST "xmlns"))) ||
		    (ctxt->loadsubset & XML_COMPLETE_ATTRS)) {
		    xmlAttributePtr tst;

		    tst = xmlGetDtdQAttrDesc(ctxt->myDoc->intSubset,
					     attr->elem, attr->name,
					     attr->prefix);
		    if ((tst == attr) || (tst == NULL)) {
		        xmlChar fn[50];
			xmlChar *fulln;

                        fulln = xmlBuildQName(attr->name, attr->prefix, fn, 50);
			if (fulln == NULL) {
			    xmlSAX2ErrMemory(ctxt);
			    return;
			}

			/*
			 * Check that the attribute is not declared in the
			 * serialization
			 */
			att = NULL;
			if (atts != NULL) {
			    i = 0;
			    att = atts[i];
			    while (att != NULL) {
				if (xmlStrEqual(att, fulln))
				    break;
				i += 2;
				att = atts[i];
			    }
			}
			if (att == NULL) {
			    xmlSAX1Attribute(ctxt, fulln,
					     attr->defaultValue, prefix);
			}
			if ((fulln != fn) && (fulln != attr->name))
			    xmlFree(fulln);
		    }
		}
	    }
	    attr = attr->nexth;
	}
	if (internal == 1) {
	    elemDecl = xmlGetDtdQElementDesc(ctxt->myDoc->extSubset,
		                             name, prefix);
	    internal = 0;
	    goto process_external_subset;
	}
    }
}

/**
 * called when an opening tag has been processed.
 *
 * Deprecated SAX1 interface.
 *
 * @param ctx  the user data (XML parser context)
 * @param fullname  The element name, including namespace prefix
 * @param atts  An array of name/value attributes pairs, NULL terminated
 */
static void
xmlSAX1StartElement(void *ctx, const xmlChar *fullname, const xmlChar **atts)
{
    xmlParserCtxtPtr ctxt = (xmlParserCtxtPtr) ctx;
    xmlNodePtr ret;
    xmlNodePtr parent;
    xmlNsPtr ns;
    const xmlChar *name;
    xmlChar *prefix;
    const xmlChar *att;
    const xmlChar *value;
    int i, res;

    if ((ctx == NULL) || (fullname == NULL) || (ctxt->myDoc == NULL)) return;

#ifdef LIBXML_VALID_ENABLED
    /*
     * First check on validity:
     */
    if (ctxt->validate && (ctxt->myDoc->extSubset == NULL) &&
        ((ctxt->myDoc->intSubset == NULL) ||
	 ((ctxt->myDoc->intSubset->notations == NULL) &&
	  (ctxt->myDoc->intSubset->elements == NULL) &&
	  (ctxt->myDoc->intSubset->attributes == NULL) &&
	  (ctxt->myDoc->intSubset->entities == NULL)))) {
	xmlErrValid(ctxt, XML_ERR_NO_DTD,
	  "Validation failed: no DTD found !", NULL, NULL);
	ctxt->validate = 0;
    }
#endif

    /*
     * Split the full name into a namespace prefix and the tag name
     */
    name = xmlSplitQName4(fullname, &prefix);
    if (name == NULL) {
        xmlSAX2ErrMemory(ctxt);
        return;
    }

    /*
     * Note : the namespace resolution is deferred until the end of the
     *        attributes parsing, since local namespace can be defined as
     *        an attribute at this level.
     */
    ret = xmlNewDocNode(ctxt->myDoc, NULL, name, NULL);
    if (ret == NULL) {
	xmlFree(prefix);
	xmlSAX2ErrMemory(ctxt);
        return;
    }
    ctxt->nodemem = -1;

    /* Initialize parent before pushing node */
    parent = ctxt->node;
    if (parent == NULL)
        parent = (xmlNodePtr) ctxt->myDoc;

    /*
     * Link the child element
     */
    xmlSAX2AppendChild(ctxt, ret);

    /*
     * We are parsing a new node.
     */
    if (nodePush(ctxt, ret) < 0) {
        xmlUnlinkNode(ret);
        xmlFreeNode(ret);
        if (prefix != NULL)
            xmlFree(prefix);
        return;
    }

    /*
     * Insert all the defaulted attributes from the DTD especially
     * namespaces
     */
    if ((ctxt->myDoc->intSubset != NULL) ||
        (ctxt->myDoc->extSubset != NULL)) {
        xmlCheckDefaultedAttributes(ctxt, name, prefix, atts);
    }

    /*
     * process all the attributes whose name start with "xmlns"
     */
    if (atts != NULL) {
        i = 0;
        att = atts[i++];
        value = atts[i++];
        while ((att != NULL) && (value != NULL)) {
            if ((att[0] == 'x') && (att[1] == 'm') && (att[2] == 'l') &&
                (att[3] == 'n') && (att[4] == 's'))
                xmlSAX1Attribute(ctxt, att, value, prefix);

            att = atts[i++];
            value = atts[i++];
        }
    }

    /*
     * Search the namespace, note that since the attributes have been
     * processed, the local namespaces are available.
     */
    res = xmlSearchNsSafe(ret, prefix, &ns);
    if (res < 0)
        xmlSAX2ErrMemory(ctxt);
    if ((ns == NULL) && (parent != NULL)) {
        res = xmlSearchNsSafe(parent, prefix, &ns);
        if (res < 0)
            xmlSAX2ErrMemory(ctxt);
    }
    if ((prefix != NULL) && (ns == NULL)) {
        xmlNsWarnMsg(ctxt, XML_NS_ERR_UNDEFINED_NAMESPACE,
                     "Namespace prefix %s is not defined\n",
                     prefix, NULL);
        ns = xmlNewNs(ret, NULL, prefix);
        if (ns == NULL)
            xmlSAX2ErrMemory(ctxt);
    }

    /*
     * set the namespace node, making sure that if the default namespace
     * is unbound on a parent we simply keep it NULL
     */
    if ((ns != NULL) && (ns->href != NULL) &&
        ((ns->href[0] != 0) || (ns->prefix != NULL)))
        xmlSetNs(ret, ns);

    /*
     * process all the other attributes
     */
    if (atts != NULL) {
        i = 0;
	att = atts[i++];
	value = atts[i++];
        while ((att != NULL) && (value != NULL)) {
            if ((att[0] != 'x') || (att[1] != 'm') || (att[2] != 'l') ||
                (att[3] != 'n') || (att[4] != 's'))
                xmlSAX1Attribute(ctxt, att, value, NULL);

            /*
             * Next ones
             */
            att = atts[i++];
            value = atts[i++];
        }
    }

#ifdef LIBXML_VALID_ENABLED
    /*
     * If it's the Document root, finish the DTD validation and
     * check the document root element for validity
     */
    if ((ctxt->validate) &&
        ((ctxt->vctxt.flags & XML_VCTXT_DTD_VALIDATED) == 0)) {
	int chk;

	chk = xmlValidateDtdFinal(&ctxt->vctxt, ctxt->myDoc);
	if (chk <= 0)
	    ctxt->valid = 0;
	if (chk < 0)
	    ctxt->wellFormed = 0;
	ctxt->valid &= xmlValidateRoot(&ctxt->vctxt, ctxt->myDoc);
	ctxt->vctxt.flags |= XML_VCTXT_DTD_VALIDATED;
    }
#endif /* LIBXML_VALID_ENABLED */

    if (prefix != NULL)
	xmlFree(prefix);

}
#endif /* LIBXML_SAX1_ENABLED */

#ifdef LIBXML_HTML_ENABLED
static void
xmlSAX2HtmlAttribute(xmlParserCtxtPtr ctxt, const xmlChar *fullname,
                     const xmlChar *value) {
    xmlAttrPtr ret;
    xmlChar *nval = NULL;

    ret = xmlNewNsProp(ctxt->node, NULL, fullname, NULL);
    if (ret == NULL) {
        xmlSAX2ErrMemory(ctxt);
        return;
    }

    if ((value == NULL) && (htmlIsBooleanAttr(fullname))) {
        nval = xmlStrdup(fullname);
        if (nval == NULL) {
            xmlSAX2ErrMemory(ctxt);
            return;
        }
        value = nval;
    }

    if (value != NULL) {
        ret->children = xmlNewDocText(ctxt->myDoc, value);
        if (ret->children == NULL) {
            xmlSAX2ErrMemory(ctxt);
        } else {
            ret->last = ret->children;
            ret->children->parent = (xmlNodePtr) ret;
        }
    }

    if (((ctxt->loadsubset & XML_SKIP_IDS) == 0) &&
        /*
         * Don't create IDs containing entity references (should
         * be always the case with HTML)
         */
        (ret->children != NULL) &&
        (ret->children->type == XML_TEXT_NODE) &&
        (ret->children->next == NULL)) {
        int res = xmlIsID(ctxt->myDoc, ctxt->node, ret);

        if (res < 0)
            xmlCtxtErrMemory(ctxt);
        else if (res > 0)
            xmlAddID(&ctxt->vctxt, ctxt->myDoc, ret->children->content, ret);
    }

    if (nval != NULL)
        xmlFree(nval);
}

/**
 * Called when an opening tag has been processed.
 *
 * @param ctxt  parser context
 * @param fullname  The element name, including namespace prefix
 * @param atts  An array of name/value attributes pairs, NULL terminated
 */
static void
xmlSAX2StartHtmlElement(xmlParserCtxtPtr ctxt, const xmlChar *fullname,
                        const xmlChar **atts) {
    xmlNodePtr ret;
    xmlNodePtr parent;
    const xmlChar *att;
    const xmlChar *value;
    int i;

    ret = xmlNewDocNode(ctxt->myDoc, NULL, fullname, NULL);
    if (ret == NULL) {
	xmlSAX2ErrMemory(ctxt);
        return;
    }
    ctxt->nodemem = -1;

    /* Initialize parent before pushing node */
    parent = ctxt->node;
    if (parent == NULL)
        parent = (xmlNodePtr) ctxt->myDoc;

    /*
     * Link the child element
     */
    xmlSAX2AppendChild(ctxt, ret);

    /*
     * We are parsing a new node.
     */
    if (nodePush(ctxt, ret) < 0) {
        xmlUnlinkNode(ret);
        xmlFreeNode(ret);
        return;
    }

    if (atts != NULL) {
        i = 0;
	att = atts[i++];
	value = atts[i++];
        while (att != NULL) {
            xmlSAX2HtmlAttribute(ctxt, att, value);
            att = atts[i++];
            value = atts[i++];
        }
    }
}
#endif /* LIBXML_HTML_ENABLED */

/**
 * Called when an opening tag has been processed.
 *
 * @deprecated Don't call this function directly.
 *
 * Used for HTML and SAX1.
 *
 * @param ctx  the user data (XML parser context)
 * @param fullname  The element name, including namespace prefix
 * @param atts  An array of name/value attributes pairs, NULL terminated
 */
void
xmlSAX2StartElement(void *ctx, const xmlChar *fullname, const xmlChar **atts) {
    xmlParserCtxtPtr ctxt = (xmlParserCtxtPtr) ctx;

    (void) atts;

    if ((ctxt == NULL) || (fullname == NULL) || (ctxt->myDoc == NULL))
        return;

#ifdef LIBXML_SAX1_ENABLED
    if (!ctxt->html) {
        xmlSAX1StartElement(ctxt, fullname, atts);
        return;
    }
#endif

#ifdef LIBXML_HTML_ENABLED
    if (ctxt->html) {
        xmlSAX2StartHtmlElement(ctxt, fullname, atts);
        return;
    }
#endif
}

/**
 * called when the end of an element has been detected.
 *
 * @deprecated Don't call this function directly.
 *
 * Used for HTML and SAX1.
 *
 * @param ctx  the user data (XML parser context)
 * @param name  The element name
 */
void
xmlSAX2EndElement(void *ctx, const xmlChar *name ATTRIBUTE_UNUSED)
{
    xmlParserCtxtPtr ctxt = (xmlParserCtxtPtr) ctx;

    if (ctxt == NULL)
        return;

#if defined(LIBXML_SAX1_ENABLED) && defined(LIBXML_VALID_ENABLED)
    if (!ctxt->html && ctxt->validate && ctxt->wellFormed &&
        ctxt->myDoc && ctxt->myDoc->intSubset)
        ctxt->valid &= xmlValidateOneElement(&ctxt->vctxt, ctxt->myDoc,
					     ctxt->node);
#endif /* LIBXML_VALID_ENABLED */

#if defined(LIBXML_SAX1_ENABLED) || defined(LIBXML_HTML_ENABLED)
    ctxt->nodemem = -1;

    /*
     * end of parsing of this node.
     */
    nodePop(ctxt);
#endif
}

/*
 * @param ctxt  the parser context
 * @param doc  the document
 * @param str  the input string
 * @param len  the string length
 *
 * Callback for a text node
 *
 * @returns the newly allocated string or NULL if not needed or error
 */
static xmlNodePtr
xmlSAX2TextNode(xmlParserCtxtPtr ctxt, xmlDocPtr doc, const xmlChar *str,
                int len) {
    xmlNodePtr ret;
    const xmlChar *intern = NULL;

    /*
     * Allocate
     */
    if (ctxt->freeElems != NULL) {
	ret = ctxt->freeElems;
	ctxt->freeElems = ret->next;
	ctxt->freeElemsNr--;
    } else {
	ret = (xmlNodePtr) xmlMalloc(sizeof(xmlNode));
    }
    if (ret == NULL) {
        xmlCtxtErrMemory(ctxt);
	return(NULL);
    }
    memset(ret, 0, sizeof(xmlNode));
    /*
     * intern the formatting blanks found between tags, or the
     * very short strings
     */
    if ((!ctxt->html) && (ctxt->dictNames)) {
        xmlChar cur = str[len];

	if ((len < (int) (2 * sizeof(void *))) &&
	    (ctxt->options & XML_PARSE_COMPACT)) {
	    /* store the string in the node overriding properties and nsDef */
	    xmlChar *tmp = (xmlChar *) &(ret->properties);
	    memcpy(tmp, str, len);
	    tmp[len] = 0;
	    intern = tmp;
	} else if ((len <= 3) && ((cur == '"') || (cur == '\'') ||
	    ((cur == '<') && (str[len + 1] != '!')))) {
	    intern = xmlDictLookup(ctxt->dict, str, len);
            if (intern == NULL) {
                xmlSAX2ErrMemory(ctxt);
                xmlFree(ret);
                return(NULL);
            }
	} else if (IS_BLANK_CH(*str) && (len < 60) && (cur == '<') &&
	           (str[len + 1] != '!')) {
	    int i;

	    for (i = 1;i < len;i++) {
		if (!IS_BLANK_CH(str[i])) goto skip;
	    }
	    intern = xmlDictLookup(ctxt->dict, str, len);
            if (intern == NULL) {
                xmlSAX2ErrMemory(ctxt);
                xmlFree(ret);
                return(NULL);
            }
	}
    }
skip:
    ret->type = XML_TEXT_NODE;
    ret->doc = doc;

    ret->name = xmlStringText;
    if (intern == NULL) {
	ret->content = xmlStrndup(str, len);
	if (ret->content == NULL) {
	    xmlSAX2ErrMemory(ctxt);
	    xmlFree(ret);
	    return(NULL);
	}
    } else
	ret->content = (xmlChar *) intern;

    if ((xmlRegisterCallbacks) && (xmlRegisterNodeDefaultValue))
	xmlRegisterNodeDefaultValue(ret);
    return(ret);
}

#ifdef LIBXML_VALID_ENABLED
/*
 * @param ctxt  the parser context
 * @param str  the input string
 * @param len  the string length
 *
 * Remove the entities from an attribute value
 *
 * @returns the newly allocated string or NULL if not needed or error
 */
static xmlChar *
xmlSAX2DecodeAttrEntities(xmlParserCtxtPtr ctxt, const xmlChar *str,
                          const xmlChar *end) {
    const xmlChar *in;

    in = str;
    while (in < end)
        if (*in++ == '&')
	    goto decode;
    return(NULL);
decode:
    /*
     * If the value contains '&', we can be sure it was allocated and is
     * zero-terminated.
     */
    /* TODO: normalize if needed */
    return(xmlExpandEntitiesInAttValue(ctxt, str, /* normalize */ 0));
}
#endif /* LIBXML_VALID_ENABLED */

/**
 * Handle an attribute that has been read by the parser.
 * The default handling is to convert the attribute into an
 * DOM subtree and past it in a new xmlAttr element added to
 * the element.
 *
 * @param ctxt  the parser context
 * @param localname  the local name of the attribute
 * @param prefix  the attribute namespace prefix if available
 * @param value  start of the attribute value
 * @param valueend  end of the attribute value
 * @returns the new attribute or NULL in case of error.
 */
static xmlAttrPtr
xmlSAX2AttributeNs(xmlParserCtxtPtr ctxt,
                   const xmlChar * localname,
                   const xmlChar * prefix,
		   const xmlChar * value,
		   const xmlChar * valueend)
{
    xmlAttrPtr ret;
    xmlNsPtr namespace = NULL;
    xmlChar *dup = NULL;

    /*
     * Note: if prefix == NULL, the attribute is not in the default namespace
     */
    if (prefix != NULL) {
	namespace = xmlParserNsLookupSax(ctxt, prefix);
	if ((namespace == NULL) && (xmlStrEqual(prefix, BAD_CAST "xml"))) {
            int res;

	    res = xmlSearchNsSafe(ctxt->node, prefix, &namespace);
            if (res < 0)
                xmlSAX2ErrMemory(ctxt);
	}
    }

    /*
     * allocate the node
     */
    if (ctxt->freeAttrs != NULL) {
        ret = ctxt->freeAttrs;
	ctxt->freeAttrs = ret->next;
	ctxt->freeAttrsNr--;
    } else {
        ret = xmlMalloc(sizeof(*ret));
        if (ret == NULL) {
            xmlSAX2ErrMemory(ctxt);
            return(NULL);
        }
    }

    memset(ret, 0, sizeof(xmlAttr));
    ret->type = XML_ATTRIBUTE_NODE;

    /*
     * xmlParseBalancedChunkMemoryRecover had a bug that could result in
     * a mismatch between ctxt->node->doc and ctxt->myDoc. We use
     * ctxt->node->doc here, but we should somehow make sure that the
     * document pointers match.
     */

    /* assert(ctxt->node->doc == ctxt->myDoc); */

    ret->parent = ctxt->node;
    ret->doc = ctxt->node->doc;
    ret->ns = namespace;

    if (ctxt->dictNames) {
        ret->name = localname;
    } else {
        ret->name = xmlStrdup(localname);
        if (ret->name == NULL)
            xmlSAX2ErrMemory(ctxt);
    }

    if ((xmlRegisterCallbacks) && (xmlRegisterNodeDefaultValue))
        xmlRegisterNodeDefaultValue((xmlNodePtr)ret);

    if ((ctxt->replaceEntities == 0) && (!ctxt->html)) {
	xmlNodePtr tmp;

	/*
	 * We know that if there is an entity reference, then
	 * the string has been dup'ed and terminates with 0
	 * otherwise with ' or "
	 */
	if (*valueend != 0) {
	    tmp = xmlSAX2TextNode(ctxt, ret->doc, value, valueend - value);
	    ret->children = tmp;
	    ret->last = tmp;
	    if (tmp != NULL) {
		tmp->parent = (xmlNodePtr) ret;
	    }
	} else if (valueend > value) {
            if (xmlNodeParseAttValue(ret->doc, ret, value, valueend - value,
                                     NULL) < 0)
                xmlSAX2ErrMemory(ctxt);
	}
    } else if (value != NULL) {
	xmlNodePtr tmp;

	tmp = xmlSAX2TextNode(ctxt, ret->doc, value, valueend - value);
	ret->children = tmp;
	ret->last = tmp;
	if (tmp != NULL) {
	    tmp->parent = (xmlNodePtr) ret;
	}
    }

#ifdef LIBXML_VALID_ENABLED
    if ((!ctxt->html) && ctxt->validate && ctxt->wellFormed &&
        ctxt->myDoc && ctxt->myDoc->intSubset) {
	/*
	 * If we don't substitute entities, the validation should be
	 * done on a value with replaced entities anyway.
	 */
        if (!ctxt->replaceEntities) {
	    dup = xmlSAX2DecodeAttrEntities(ctxt, value, valueend);
	    if (dup == NULL) {
	        if (*valueend == 0) {
		    ctxt->valid &= xmlValidateOneAttribute(&ctxt->vctxt,
				    ctxt->myDoc, ctxt->node, ret, value);
		} else {
		    /*
		     * That should already be normalized.
		     * cheaper to finally allocate here than duplicate
		     * entry points in the full validation code
		     */
		    dup = xmlStrndup(value, valueend - value);
                    if (dup == NULL)
                        xmlSAX2ErrMemory(ctxt);

		    ctxt->valid &= xmlValidateOneAttribute(&ctxt->vctxt,
				    ctxt->myDoc, ctxt->node, ret, dup);
		}
	    } else {
	        /*
		 * dup now contains a string of the flattened attribute
		 * content with entities substituted. Check if we need to
		 * apply an extra layer of normalization.
		 * It need to be done twice ... it's an extra burden related
		 * to the ability to keep references in attributes
		 */
		if (ctxt->attsSpecial != NULL) {
		    xmlChar *nvalnorm;
		    xmlChar fn[50];
		    xmlChar *fullname;

		    fullname = xmlBuildQName(localname, prefix, fn, 50);
                    if (fullname == NULL) {
                        xmlSAX2ErrMemory(ctxt);
                    } else {
			ctxt->vctxt.valid = 1;
		        nvalnorm = xmlValidCtxtNormalizeAttributeValue(
			                 &ctxt->vctxt, ctxt->myDoc,
					 ctxt->node, fullname, dup);
			if (ctxt->vctxt.valid != 1)
			    ctxt->valid = 0;

			if ((fullname != fn) && (fullname != localname))
			    xmlFree(fullname);
			if (nvalnorm != NULL) {
			    xmlFree(dup);
			    dup = nvalnorm;
			}
		    }
		}

		ctxt->valid &= xmlValidateOneAttribute(&ctxt->vctxt,
			        ctxt->myDoc, ctxt->node, ret, dup);
	    }
	} else {
	    /*
	     * if entities already have been substituted, then
	     * the attribute as passed is already normalized
	     */
	    dup = xmlStrndup(value, valueend - value);
            if (dup == NULL)
                xmlSAX2ErrMemory(ctxt);

            /*
             * When replacing entities, make sure that IDs in
             * entities aren't registered. This also shouldn't be
             * done when entities aren't replaced, but this would
             * require to rework IDREF checks.
             */
            if (ctxt->input->entity != NULL)
                ctxt->vctxt.flags |= XML_VCTXT_IN_ENTITY;

	    ctxt->valid &= xmlValidateOneAttribute(&ctxt->vctxt,
	                             ctxt->myDoc, ctxt->node, ret, dup);

            ctxt->vctxt.flags &= ~XML_VCTXT_IN_ENTITY;
	}
    } else
#endif /* LIBXML_VALID_ENABLED */
           if (((ctxt->loadsubset & XML_SKIP_IDS) == 0) &&
               (ctxt->input->entity == NULL) &&
               /* Don't create IDs containing entity references */
               (ret->children != NULL) &&
               (ret->children->type == XML_TEXT_NODE) &&
               (ret->children->next == NULL)) {
        xmlChar *content = ret->children->content;
        /*
	 * when validating, the ID registration is done at the attribute
	 * validation level. Otherwise we have to do specific handling here.
	 */
        if ((prefix == ctxt->str_xml) &&
	           (localname[0] == 'i') && (localname[1] == 'd') &&
		   (localname[2] == 0)) {
	    /*
	     * Add the xml:id value
	     *
	     * Open issue: normalization of the value.
	     */
	    if (xmlValidateNCName(content, 1) != 0) {
	        xmlErrId(ctxt, XML_DTD_XMLID_VALUE,
                         "xml:id : attribute value %s is not an NCName\n",
                         content);
	    }
	    xmlAddID(&ctxt->vctxt, ctxt->myDoc, content, ret);
	} else {
            int res = xmlIsID(ctxt->myDoc, ctxt->node, ret);

            if (res < 0)
                xmlCtxtErrMemory(ctxt);
            else if (res > 0)
                xmlAddID(&ctxt->vctxt, ctxt->myDoc, content, ret);
            else if (xmlIsRef(ctxt->myDoc, ctxt->node, ret))
                xmlAddRef(&ctxt->vctxt, ctxt->myDoc, content, ret);
	}
    }
    if (dup != NULL)
	xmlFree(dup);

    return(ret);
}

/**
 * SAX2 callback when an element start has been detected by the parser.
 * It provides the namespace information for the element, as well as
 * the new namespace declarations on the element.
 *
 * @param ctx  the user data (XML parser context)
 * @param localname  the local name of the element
 * @param prefix  the element namespace prefix if available
 * @param URI  the element namespace name if available
 * @param nb_namespaces  number of namespace definitions on that node
 * @param namespaces  pointer to the array of prefix/URI pairs namespace definitions
 * @param nb_attributes  the number of attributes on that node
 * @param nb_defaulted  the number of defaulted attributes.
 * @param attributes  pointer to the array of (localname/prefix/URI/value/end)
 *               attribute values.
 */
void
xmlSAX2StartElementNs(void *ctx,
                      const xmlChar *localname,
		      const xmlChar *prefix,
		      const xmlChar *URI,
		      int nb_namespaces,
		      const xmlChar **namespaces,
		      int nb_attributes,
		      int nb_defaulted,
		      const xmlChar **attributes)
{
    xmlParserCtxtPtr ctxt = (xmlParserCtxtPtr) ctx;
    xmlNodePtr ret;
    xmlNsPtr last = NULL, ns;
    const xmlChar *uri, *pref;
    xmlChar *lname = NULL;
    int i, j;

    if (ctx == NULL) return;

#ifdef LIBXML_VALID_ENABLED
    /*
     * First check on validity:
     */
    if (ctxt->validate &&
        ((ctxt->myDoc == NULL) ||
         ((ctxt->myDoc->extSubset == NULL) &&
          ((ctxt->myDoc->intSubset == NULL) ||
	   ((ctxt->myDoc->intSubset->notations == NULL) &&
	    (ctxt->myDoc->intSubset->elements == NULL) &&
	    (ctxt->myDoc->intSubset->attributes == NULL) &&
	    (ctxt->myDoc->intSubset->entities == NULL)))))) {
	xmlErrValid(ctxt, XML_DTD_NO_DTD,
	  "Validation failed: no DTD found !", NULL, NULL);
	ctxt->validate = 0;
    }
#endif /* LIBXML_VALID_ENABLED */

    /*
     * Take care of the rare case of an undefined namespace prefix
     */
    if ((prefix != NULL) && (URI == NULL)) {
        if (ctxt->dictNames) {
	    const xmlChar *fullname;

	    fullname = xmlDictQLookup(ctxt->dict, prefix, localname);
	    if (fullname == NULL) {
                xmlSAX2ErrMemory(ctxt);
                return;
            }
	    localname = fullname;
	} else {
	    lname = xmlBuildQName(localname, prefix, NULL, 0);
            if (lname == NULL) {
                xmlSAX2ErrMemory(ctxt);
                return;
            }
	}
    }
    /*
     * allocate the node
     */
    if (ctxt->freeElems != NULL) {
        ret = ctxt->freeElems;
	ctxt->freeElems = ret->next;
	ctxt->freeElemsNr--;
	memset(ret, 0, sizeof(xmlNode));
        ret->doc = ctxt->myDoc;
	ret->type = XML_ELEMENT_NODE;

	if (ctxt->dictNames)
	    ret->name = localname;
	else {
	    if (lname == NULL)
		ret->name = xmlStrdup(localname);
	    else
	        ret->name = lname;
	    if (ret->name == NULL) {
	        xmlSAX2ErrMemory(ctxt);
                xmlFree(ret);
		return;
	    }
	}
	if ((xmlRegisterCallbacks) && (xmlRegisterNodeDefaultValue))
	    xmlRegisterNodeDefaultValue(ret);
    } else {
	if (ctxt->dictNames)
	    ret = xmlNewDocNodeEatName(ctxt->myDoc, NULL,
	                               (xmlChar *) localname, NULL);
	else if (lname == NULL)
	    ret = xmlNewDocNode(ctxt->myDoc, NULL, localname, NULL);
	else
	    ret = xmlNewDocNodeEatName(ctxt->myDoc, NULL, lname, NULL);
	if (ret == NULL) {
	    xmlSAX2ErrMemory(ctxt);
	    return;
	}
    }

    /*
     * Build the namespace list
     */
    for (i = 0,j = 0;j < nb_namespaces;j++) {
        pref = namespaces[i++];
	uri = namespaces[i++];
	ns = xmlNewNs(NULL, uri, pref);
	if (ns != NULL) {
	    if (last == NULL) {
	        ret->nsDef = last = ns;
	    } else {
	        last->next = ns;
		last = ns;
	    }
	    if ((URI != NULL) && (prefix == pref))
		ret->ns = ns;
	} else {
            xmlSAX2ErrMemory(ctxt);
	    continue;
	}

        xmlParserNsUpdateSax(ctxt, pref, ns);

#ifdef LIBXML_VALID_ENABLED
	if ((!ctxt->html) && ctxt->validate && ctxt->wellFormed &&
	    ctxt->myDoc && ctxt->myDoc->intSubset) {
	    ctxt->valid &= xmlValidateOneNamespace(&ctxt->vctxt, ctxt->myDoc,
	                                           ret, prefix, ns, uri);
	}
#endif /* LIBXML_VALID_ENABLED */
    }
    ctxt->nodemem = -1;

    /*
     * Link the child element
     */
    xmlSAX2AppendChild(ctxt, ret);

    /*
     * We are parsing a new node.
     */
    if (nodePush(ctxt, ret) < 0) {
        xmlUnlinkNode(ret);
        xmlFreeNode(ret);
        return;
    }

    /*
     * Insert the defaulted attributes from the DTD only if requested:
     */
    if ((nb_defaulted != 0) &&
        ((ctxt->loadsubset & XML_COMPLETE_ATTRS) == 0))
	nb_attributes -= nb_defaulted;

    /*
     * Search the namespace if it wasn't already found
     * Note that, if prefix is NULL, this searches for the default Ns
     */
    if ((URI != NULL) && (ret->ns == NULL)) {
        ret->ns = xmlParserNsLookupSax(ctxt, prefix);
	if ((ret->ns == NULL) && (xmlStrEqual(prefix, BAD_CAST "xml"))) {
            int res;

	    res = xmlSearchNsSafe(ret, prefix, &ret->ns);
            if (res < 0)
                xmlSAX2ErrMemory(ctxt);
	}
	if (ret->ns == NULL) {
	    ns = xmlNewNs(ret, NULL, prefix);
	    if (ns == NULL) {

	        xmlSAX2ErrMemory(ctxt);
		return;
	    }
            if (prefix != NULL)
                xmlNsWarnMsg(ctxt, XML_NS_ERR_UNDEFINED_NAMESPACE,
                             "Namespace prefix %s was not found\n",
                             prefix, NULL);
            else
                xmlNsWarnMsg(ctxt, XML_NS_ERR_UNDEFINED_NAMESPACE,
                             "Namespace default prefix was not found\n",
                             NULL, NULL);
	}
    }

    /*
     * process all the other attributes
     */
    if (nb_attributes > 0) {
        xmlAttrPtr prev = NULL;

        for (j = 0,i = 0;i < nb_attributes;i++,j+=5) {
            xmlAttrPtr attr = NULL;

	    /*
	     * Handle the rare case of an undefined attribute prefix
	     */
	    if ((attributes[j+1] != NULL) && (attributes[j+2] == NULL)) {
		if (ctxt->dictNames) {
		    const xmlChar *fullname;

		    fullname = xmlDictQLookup(ctxt->dict, attributes[j+1],
		                              attributes[j]);
		    if (fullname == NULL) {
                        xmlSAX2ErrMemory(ctxt);
                        return;
                    }
                    attr = xmlSAX2AttributeNs(ctxt, fullname, NULL,
                                              attributes[j+3],
                                              attributes[j+4]);
                    goto have_attr;
		} else {
		    lname = xmlBuildQName(attributes[j], attributes[j+1],
		                          NULL, 0);
		    if (lname == NULL) {
                        xmlSAX2ErrMemory(ctxt);
                        return;
                    }
                    attr = xmlSAX2AttributeNs(ctxt, lname, NULL,
                                              attributes[j+3],
                                              attributes[j+4]);
                    xmlFree(lname);
                    goto have_attr;
		}
	    }
            attr = xmlSAX2AttributeNs(ctxt, attributes[j], attributes[j+1],
                                      attributes[j+3], attributes[j+4]);
have_attr:
            if (attr == NULL)
                continue;

            /* link at the end to preserve order */
            if (prev == NULL) {
                ctxt->node->properties = attr;
            } else {
                prev->next = attr;
                attr->prev = prev;
            }

            prev = attr;
	}
    }

#ifdef LIBXML_VALID_ENABLED
    /*
     * If it's the Document root, finish the DTD validation and
     * check the document root element for validity
     */
    if ((ctxt->validate) &&
        ((ctxt->vctxt.flags & XML_VCTXT_DTD_VALIDATED) == 0)) {
	int chk;

	chk = xmlValidateDtdFinal(&ctxt->vctxt, ctxt->myDoc);
	if (chk <= 0)
	    ctxt->valid = 0;
	if (chk < 0)
	    ctxt->wellFormed = 0;
	ctxt->valid &= xmlValidateRoot(&ctxt->vctxt, ctxt->myDoc);
	ctxt->vctxt.flags |= XML_VCTXT_DTD_VALIDATED;
    }
#endif /* LIBXML_VALID_ENABLED */
}

/**
 * SAX2 callback when an element end has been detected by the parser.
 * It provides the namespace information for the element.
 *
 * @param ctx  the user data (XML parser context)
 * @param localname  the local name of the element
 * @param prefix  the element namespace prefix if available
 * @param URI  the element namespace name if available
 */
void
xmlSAX2EndElementNs(void *ctx,
                    const xmlChar * localname ATTRIBUTE_UNUSED,
                    const xmlChar * prefix ATTRIBUTE_UNUSED,
		    const xmlChar * URI ATTRIBUTE_UNUSED)
{
    xmlParserCtxtPtr ctxt = (xmlParserCtxtPtr) ctx;

    if (ctx == NULL) return;
    ctxt->nodemem = -1;

#ifdef LIBXML_VALID_ENABLED
    if (ctxt->validate && ctxt->wellFormed &&
        ctxt->myDoc && ctxt->myDoc->intSubset)
        ctxt->valid &= xmlValidateOneElement(&ctxt->vctxt, ctxt->myDoc,
                                             ctxt->node);
#endif /* LIBXML_VALID_ENABLED */

    /*
     * end of parsing of this node.
     */
    nodePop(ctxt);
}

/**
 * called when an entity #xmlSAX2Reference is detected.
 *
 * @param ctx  the user data (XML parser context)
 * @param name  The entity name
 */
void
xmlSAX2Reference(void *ctx, const xmlChar *name)
{
    xmlParserCtxtPtr ctxt = (xmlParserCtxtPtr) ctx;
    xmlNodePtr ret;

    if (ctx == NULL) return;
    ret = xmlNewReference(ctxt->myDoc, name);
    if (ret == NULL) {
        xmlSAX2ErrMemory(ctxt);
        return;
    }

    xmlSAX2AppendChild(ctxt, ret);
}

/**
 * Append characters.
 *
 * @param ctxt  the parser context
 * @param ch  a xmlChar string
 * @param len  the number of xmlChar
 * @param type  text or cdata
 */
static void
xmlSAX2Text(xmlParserCtxtPtr ctxt, const xmlChar *ch, int len,
            xmlElementType type)
{
    xmlNodePtr lastChild;
    xmlNodePtr parent;

    if (ctxt == NULL)
        return;

    parent = ctxt->node;
    if (parent == NULL)
        return;
    lastChild = parent->last;

    /*
     * Try to merge with previous text node using size and capacity
     * stored in the parser context to avoid naive concatenation.
     *
     * Don't merge CDATA sections. In HTML mode, CDATA is used for
     * raw text which should be merged.
     */
    if ((lastChild == NULL) ||
        (lastChild->type != type) ||
        ((!ctxt->html) && (type != XML_TEXT_NODE))) {
        xmlNode *node;

        if (type == XML_TEXT_NODE)
            node = xmlSAX2TextNode(ctxt, parent->doc, ch, len);
        else
            node = xmlNewCDataBlock(parent->doc, ch, len);
	if (node == NULL) {
	    xmlSAX2ErrMemory(ctxt);
	    return;
	}

        if (lastChild == NULL) {
            parent->children = node;
            parent->last = node;
            node->parent = parent;
        } else {
            xmlSAX2AppendChild(ctxt, node);
        }

        ctxt->nodelen = len;
        ctxt->nodemem = len + 1;
        lastChild = node;
    } else {
        xmlChar *content;
        int oldSize, newSize, capacity;
        int maxSize = (ctxt->options & XML_PARSE_HUGE) ?
                      XML_MAX_HUGE_LENGTH :
                      XML_MAX_TEXT_LENGTH;

        content = lastChild->content;
        oldSize = ctxt->nodelen;
        capacity = ctxt->nodemem;

        /* Shouldn't happen */
        if ((content == NULL) || (capacity <= 0)) {
            xmlFatalErr(ctxt, XML_ERR_INTERNAL_ERROR,
                        "xmlSAX2Text: no content");
            return;
        }

        if ((len > maxSize) || (oldSize > maxSize - len)) {
            xmlFatalErr(ctxt, XML_ERR_RESOURCE_LIMIT,
                        "Text node too long, try XML_PARSE_HUGE");
            return;
        }

        newSize = oldSize + len;

        if (newSize >= capacity) {
            if (newSize <= 20)
                capacity = 40;
            else
                capacity = newSize > INT_MAX / 2 ? INT_MAX : newSize * 2;

            /*
             * If the content was stored in properties or in
             * the dictionary, don't realloc.
             */
            if ((content == (xmlChar *) &lastChild->properties) ||
                ((ctxt->nodemem == oldSize + 1) &&
                 (xmlDictOwns(ctxt->dict, content)))) {
                xmlChar *newContent;

                newContent = xmlMalloc(capacity);
                if (newContent == NULL) {
                    xmlSAX2ErrMemory(ctxt);
                    return;
                }

                memcpy(newContent, content, oldSize);
                lastChild->properties = NULL;
                content = newContent;
            } else {
                content = xmlRealloc(content, capacity);
                if (content == NULL) {
                    xmlSAX2ErrMemory(ctxt);
                    return;
                }
            }

            ctxt->nodemem = capacity;
            lastChild->content = content;
        }

        memcpy(&content[oldSize], ch, len);
        content[newSize] = 0;
        ctxt->nodelen = newSize;
    }

    if ((lastChild != NULL) &&
        (type == XML_TEXT_NODE) &&
        (ctxt->input != NULL)) {
        if ((unsigned) ctxt->input->line < (unsigned) USHRT_MAX)
            lastChild->line = ctxt->input->line;
        else {
            lastChild->line = USHRT_MAX;
            if (ctxt->options & XML_PARSE_BIG_LINES)
                lastChild->psvi = XML_INT_TO_PTR(ctxt->input->line);
        }
    }
}

/**
 * receiving some chars from the parser.
 *
 * @param ctx  the user data (XML parser context)
 * @param ch  a xmlChar string
 * @param len  the number of xmlChar
 */
void
xmlSAX2Characters(void *ctx, const xmlChar *ch, int len)
{
    xmlSAX2Text((xmlParserCtxtPtr) ctx, ch, len, XML_TEXT_NODE);
}

/**
 * receiving some ignorable whitespaces from the parser.
 * UNUSED: by default the DOM building will use #xmlSAX2Characters
 *
 * @param ctx  the user data (XML parser context)
 * @param ch  a xmlChar string
 * @param len  the number of xmlChar
 */
void
xmlSAX2IgnorableWhitespace(void *ctx ATTRIBUTE_UNUSED, const xmlChar *ch ATTRIBUTE_UNUSED, int len ATTRIBUTE_UNUSED)
{
}

/**
 * A processing instruction has been parsed.
 *
 * @param ctx  the user data (XML parser context)
 * @param target  the target name
 * @param data  the PI data's
 */
void
xmlSAX2ProcessingInstruction(void *ctx, const xmlChar *target,
                      const xmlChar *data)
{
    xmlParserCtxtPtr ctxt = (xmlParserCtxtPtr) ctx;
    xmlNodePtr ret;

    if (ctx == NULL) return;

    ret = xmlNewDocPI(ctxt->myDoc, target, data);
    if (ret == NULL) {
        xmlSAX2ErrMemory(ctxt);
        return;
    }

    xmlSAX2AppendChild(ctxt, ret);
}

/**
 * A #xmlSAX2Comment has been parsed.
 *
 * @param ctx  the user data (XML parser context)
 * @param value  the #xmlSAX2Comment content
 */
void
xmlSAX2Comment(void *ctx, const xmlChar *value)
{
    xmlParserCtxtPtr ctxt = (xmlParserCtxtPtr) ctx;
    xmlNodePtr ret;

    if (ctx == NULL) return;

    ret = xmlNewDocComment(ctxt->myDoc, value);
    if (ret == NULL) {
        xmlSAX2ErrMemory(ctxt);
        return;
    }

    xmlSAX2AppendChild(ctxt, ret);
}

/**
 * called when a pcdata block has been parsed
 *
 * @param ctx  the user data (XML parser context)
 * @param value  The pcdata content
 * @param len  the block length
 */
void
xmlSAX2CDataBlock(void *ctx, const xmlChar *value, int len)
{
    xmlSAX2Text((xmlParserCtxtPtr) ctx, value, len, XML_CDATA_SECTION_NODE);
}

#ifdef LIBXML_SAX1_ENABLED
/**
 * Has no effect.
 *
 * @deprecated Use parser option XML_PARSE_SAX1.
 *
 * @param version  the version, must be 2
 * @returns 2 in case of success and -1 in case of error.
 */
int
xmlSAXDefaultVersion(int version)
{
    if (version != 2)
        return(-1);
    return(2);
}
#endif /* LIBXML_SAX1_ENABLED */

/**
 * Initialize the default XML SAX handler according to the version
 *
 * @param hdlr  the SAX handler
 * @param version  the version, 1 or 2
 * @returns 0 in case of success and -1 in case of error.
 */
int
xmlSAXVersion(xmlSAXHandler *hdlr, int version)
{
    if (hdlr == NULL) return(-1);
    if (version == 2) {
	hdlr->startElementNs = xmlSAX2StartElementNs;
	hdlr->endElementNs = xmlSAX2EndElementNs;
	hdlr->serror = NULL;
	hdlr->initialized = XML_SAX2_MAGIC;
#ifdef LIBXML_SAX1_ENABLED
    } else if (version == 1) {
	hdlr->initialized = 1;
#endif /* LIBXML_SAX1_ENABLED */
    } else
        return(-1);
#ifdef LIBXML_SAX1_ENABLED
    hdlr->startElement = xmlSAX2StartElement;
    hdlr->endElement = xmlSAX2EndElement;
#else
    hdlr->startElement = NULL;
    hdlr->endElement = NULL;
#endif /* LIBXML_SAX1_ENABLED */
    hdlr->internalSubset = xmlSAX2InternalSubset;
    hdlr->externalSubset = xmlSAX2ExternalSubset;
    hdlr->isStandalone = xmlSAX2IsStandalone;
    hdlr->hasInternalSubset = xmlSAX2HasInternalSubset;
    hdlr->hasExternalSubset = xmlSAX2HasExternalSubset;
    hdlr->resolveEntity = xmlSAX2ResolveEntity;
    hdlr->getEntity = xmlSAX2GetEntity;
    hdlr->getParameterEntity = xmlSAX2GetParameterEntity;
    hdlr->entityDecl = xmlSAX2EntityDecl;
    hdlr->attributeDecl = xmlSAX2AttributeDecl;
    hdlr->elementDecl = xmlSAX2ElementDecl;
    hdlr->notationDecl = xmlSAX2NotationDecl;
    hdlr->unparsedEntityDecl = xmlSAX2UnparsedEntityDecl;
    hdlr->setDocumentLocator = xmlSAX2SetDocumentLocator;
    hdlr->startDocument = xmlSAX2StartDocument;
    hdlr->endDocument = xmlSAX2EndDocument;
    hdlr->reference = xmlSAX2Reference;
    hdlr->characters = xmlSAX2Characters;
    hdlr->cdataBlock = xmlSAX2CDataBlock;
    hdlr->ignorableWhitespace = xmlSAX2Characters;
    hdlr->processingInstruction = xmlSAX2ProcessingInstruction;
    hdlr->comment = xmlSAX2Comment;
    hdlr->warning = xmlParserWarning;
    hdlr->error = xmlParserError;
    hdlr->fatalError = xmlParserError;

    return(0);
}

/**
 * Initialize the default XML SAX2 handler
 *
 * @param hdlr  the SAX handler
 * @param warning  flag if non-zero sets the handler warning procedure
 */
void
xmlSAX2InitDefaultSAXHandler(xmlSAXHandler *hdlr, int warning)
{
    if ((hdlr == NULL) || (hdlr->initialized != 0))
	return;

    xmlSAXVersion(hdlr, 2);
    if (warning == 0)
	hdlr->warning = NULL;
}

/**
 * Initialize the default SAX2 handler
 *
 * @deprecated This function is a no-op. Call #xmlInitParser to
 * initialize the library.
 *
 */
void
xmlDefaultSAXHandlerInit(void)
{
}

#ifdef LIBXML_HTML_ENABLED

/**
 * Initialize the default HTML SAX2 handler
 *
 * @param hdlr  the SAX handler
 */
void
xmlSAX2InitHtmlDefaultSAXHandler(xmlSAXHandler *hdlr)
{
    if ((hdlr == NULL) || (hdlr->initialized != 0))
	return;

    hdlr->internalSubset = xmlSAX2InternalSubset;
    hdlr->externalSubset = NULL;
    hdlr->isStandalone = NULL;
    hdlr->hasInternalSubset = NULL;
    hdlr->hasExternalSubset = NULL;
    hdlr->resolveEntity = NULL;
    hdlr->getEntity = xmlSAX2GetEntity;
    hdlr->getParameterEntity = NULL;
    hdlr->entityDecl = NULL;
    hdlr->attributeDecl = NULL;
    hdlr->elementDecl = NULL;
    hdlr->notationDecl = NULL;
    hdlr->unparsedEntityDecl = NULL;
    hdlr->setDocumentLocator = xmlSAX2SetDocumentLocator;
    hdlr->startDocument = xmlSAX2StartDocument;
    hdlr->endDocument = xmlSAX2EndDocument;
    hdlr->startElement = xmlSAX2StartElement;
    hdlr->endElement = xmlSAX2EndElement;
    hdlr->reference = NULL;
    hdlr->characters = xmlSAX2Characters;
    hdlr->cdataBlock = xmlSAX2CDataBlock;
    hdlr->ignorableWhitespace = xmlSAX2IgnorableWhitespace;
    hdlr->processingInstruction = xmlSAX2ProcessingInstruction;
    hdlr->comment = xmlSAX2Comment;
    hdlr->warning = xmlParserWarning;
    hdlr->error = xmlParserError;
    hdlr->fatalError = xmlParserError;

    hdlr->initialized = 1;
}

/**
 * @deprecated This function is a no-op. Call #xmlInitParser to
 * initialize the library.
 */
void
htmlDefaultSAXHandlerInit(void)
{
}

#endif /* LIBXML_HTML_ENABLED */

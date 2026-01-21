/**
 * @file
 * 
 * @brief incomplete XML Schemas structure implementation
 * 
 * interface to the XML Schemas handling and schema validity
 *              checking, it is incomplete right now.
 *
 * @copyright See Copyright for the status of this software.
 *
 * @author Daniel Veillard
 */


#ifndef __XML_SCHEMA_H__
#define __XML_SCHEMA_H__

#include <libxml/xmlversion.h>

#ifdef LIBXML_SCHEMAS_ENABLED

#include <stdio.h>
#include <libxml/encoding.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xmlerror.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * This error codes are obsolete; not used any more.
 */
typedef enum {
    XML_SCHEMAS_ERR_OK		= 0,
    XML_SCHEMAS_ERR_NOROOT	= 1,
    XML_SCHEMAS_ERR_UNDECLAREDELEM,
    XML_SCHEMAS_ERR_NOTTOPLEVEL,
    XML_SCHEMAS_ERR_MISSING,
    XML_SCHEMAS_ERR_WRONGELEM,
    XML_SCHEMAS_ERR_NOTYPE,
    XML_SCHEMAS_ERR_NOROLLBACK,
    XML_SCHEMAS_ERR_ISABSTRACT,
    XML_SCHEMAS_ERR_NOTEMPTY,
    XML_SCHEMAS_ERR_ELEMCONT,
    XML_SCHEMAS_ERR_HAVEDEFAULT,
    XML_SCHEMAS_ERR_NOTNILLABLE,
    XML_SCHEMAS_ERR_EXTRACONTENT,
    XML_SCHEMAS_ERR_INVALIDATTR,
    XML_SCHEMAS_ERR_INVALIDELEM,
    XML_SCHEMAS_ERR_NOTDETERMINIST,
    XML_SCHEMAS_ERR_CONSTRUCT,
    XML_SCHEMAS_ERR_INTERNAL,
    XML_SCHEMAS_ERR_NOTSIMPLE,
    XML_SCHEMAS_ERR_ATTRUNKNOWN,
    XML_SCHEMAS_ERR_ATTRINVALID,
    XML_SCHEMAS_ERR_VALUE,
    XML_SCHEMAS_ERR_FACET,
    XML_SCHEMAS_ERR_,
    XML_SCHEMAS_ERR_XXX
} xmlSchemaValidError;

/*
* ATTENTION: Change xmlSchemaSetValidOptions's check
* for invalid values, if adding to the validation
* options below.
*/
/**
 * This is the set of XML Schema validation options.
 */
typedef enum {
    XML_SCHEMA_VAL_VC_I_CREATE			= 1<<0
	/* Default/fixed: create an attribute node
	* or an element's text node on the instance.
	*/
} xmlSchemaValidOption;

/*
    XML_SCHEMA_VAL_XSI_ASSEMBLE			= 1<<1,
	* assemble schemata using
	* xsi:schemaLocation and
	* xsi:noNamespaceSchemaLocation
*/

/** XML schema */
typedef struct _xmlSchema xmlSchema;
typedef xmlSchema *xmlSchemaPtr;

/**
 * Signature of an error callback from an XSD validation
 *
 * @param ctx  the validation context
 * @param msg  the message
 * @param ... extra arguments
 */
typedef void (*xmlSchemaValidityErrorFunc)
                 (void *ctx, const char *msg, ...) LIBXML_ATTR_FORMAT(2,3);

/**
 * Signature of a warning callback from an XSD validation
 *
 * @param ctx  the validation context
 * @param msg  the message
 * @param ... extra arguments
 */
typedef void (*xmlSchemaValidityWarningFunc)
                 (void *ctx, const char *msg, ...) LIBXML_ATTR_FORMAT(2,3);

/** Schema parser context */
typedef struct _xmlSchemaParserCtxt xmlSchemaParserCtxt;
typedef xmlSchemaParserCtxt *xmlSchemaParserCtxtPtr;

/** Schema validation context */
typedef struct _xmlSchemaValidCtxt xmlSchemaValidCtxt;
typedef xmlSchemaValidCtxt *xmlSchemaValidCtxtPtr;

/**
 * A schemas validation locator, a callback called by the validator.
 * This is used when file or node information are not available
 * to find out what file and line number are affected
 *
 * @param ctx  user provided context
 * @param file  returned file information
 * @param line  returned line information
 * @returns 0 in case of success and -1 in case of error
 */

typedef int (*xmlSchemaValidityLocatorFunc) (void *ctx,
                           const char **file, unsigned long *line);

/*
 * Interfaces for parsing.
 */
XMLPUBFUN xmlSchemaParserCtxt *
	    xmlSchemaNewParserCtxt	(const char *URL);
XMLPUBFUN xmlSchemaParserCtxt *
	    xmlSchemaNewMemParserCtxt	(const char *buffer,
					 int size);
XMLPUBFUN xmlSchemaParserCtxt *
	    xmlSchemaNewDocParserCtxt	(xmlDoc *doc);
XMLPUBFUN void
	    xmlSchemaFreeParserCtxt	(xmlSchemaParserCtxt *ctxt);
XMLPUBFUN void
	    xmlSchemaSetParserErrors	(xmlSchemaParserCtxt *ctxt,
					 xmlSchemaValidityErrorFunc err,
					 xmlSchemaValidityWarningFunc warn,
					 void *ctx);
XMLPUBFUN void
	    xmlSchemaSetParserStructuredErrors(xmlSchemaParserCtxt *ctxt,
					 xmlStructuredErrorFunc serror,
					 void *ctx);
XMLPUBFUN int
	    xmlSchemaGetParserErrors	(xmlSchemaParserCtxt *ctxt,
					xmlSchemaValidityErrorFunc * err,
					xmlSchemaValidityWarningFunc * warn,
					void **ctx);
XMLPUBFUN void
	    xmlSchemaSetResourceLoader	(xmlSchemaParserCtxt *ctxt,
					 xmlResourceLoader loader,
					 void *data);
XMLPUBFUN int
	    xmlSchemaIsValid		(xmlSchemaValidCtxt *ctxt);

XMLPUBFUN xmlSchema *
	    xmlSchemaParse		(xmlSchemaParserCtxt *ctxt);
XMLPUBFUN void
	    xmlSchemaFree		(xmlSchema *schema);
#ifdef LIBXML_DEBUG_ENABLED
XMLPUBFUN void
	    xmlSchemaDump		(FILE *output,
					 xmlSchema *schema);
#endif /* LIBXML_DEBUG_ENABLED */
/*
 * Interfaces for validating
 */
XMLPUBFUN void
	    xmlSchemaSetValidErrors	(xmlSchemaValidCtxt *ctxt,
					 xmlSchemaValidityErrorFunc err,
					 xmlSchemaValidityWarningFunc warn,
					 void *ctx);
XMLPUBFUN void
	    xmlSchemaSetValidStructuredErrors(xmlSchemaValidCtxt *ctxt,
					 xmlStructuredErrorFunc serror,
					 void *ctx);
XMLPUBFUN int
	    xmlSchemaGetValidErrors	(xmlSchemaValidCtxt *ctxt,
					 xmlSchemaValidityErrorFunc *err,
					 xmlSchemaValidityWarningFunc *warn,
					 void **ctx);
XMLPUBFUN int
	    xmlSchemaSetValidOptions	(xmlSchemaValidCtxt *ctxt,
					 int options);
XMLPUBFUN void
            xmlSchemaValidateSetFilename(xmlSchemaValidCtxt *vctxt,
	                                 const char *filename);
XMLPUBFUN int
	    xmlSchemaValidCtxtGetOptions(xmlSchemaValidCtxt *ctxt);

XMLPUBFUN xmlSchemaValidCtxt *
	    xmlSchemaNewValidCtxt	(xmlSchema *schema);
XMLPUBFUN void
	    xmlSchemaFreeValidCtxt	(xmlSchemaValidCtxt *ctxt);
XMLPUBFUN int
	    xmlSchemaValidateDoc	(xmlSchemaValidCtxt *ctxt,
					 xmlDoc *instance);
XMLPUBFUN int
            xmlSchemaValidateOneElement (xmlSchemaValidCtxt *ctxt,
			                 xmlNode *elem);
XMLPUBFUN int
	    xmlSchemaValidateStream	(xmlSchemaValidCtxt *ctxt,
					 xmlParserInputBuffer *input,
					 xmlCharEncoding enc,
					 const xmlSAXHandler *sax,
					 void *user_data);
XMLPUBFUN int
	    xmlSchemaValidateFile	(xmlSchemaValidCtxt *ctxt,
					 const char * filename,
					 int options);

XMLPUBFUN xmlParserCtxt *
	    xmlSchemaValidCtxtGetParserCtxt(xmlSchemaValidCtxt *ctxt);

/**
 * Interface to insert Schemas SAX validation in a SAX stream
 */
typedef struct _xmlSchemaSAXPlug xmlSchemaSAXPlugStruct;
typedef xmlSchemaSAXPlugStruct *xmlSchemaSAXPlugPtr;

XMLPUBFUN xmlSchemaSAXPlugStruct *
            xmlSchemaSAXPlug		(xmlSchemaValidCtxt *ctxt,
					 xmlSAXHandler **sax,
					 void **user_data);
XMLPUBFUN int
            xmlSchemaSAXUnplug		(xmlSchemaSAXPlugStruct *plug);


XMLPUBFUN void
            xmlSchemaValidateSetLocator	(xmlSchemaValidCtxt *vctxt,
					 xmlSchemaValidityLocatorFunc f,
					 void *ctxt);

#ifdef __cplusplus
}
#endif

#endif /* LIBXML_SCHEMAS_ENABLED */
#endif /* __XML_SCHEMA_H__ */

/**
 * @file
 * 
 * @brief implementation of the Relax-NG validation
 * 
 * implementation of the Relax-NG validation
 *
 * @copyright See Copyright for the status of this software.
 *
 * @author Daniel Veillard
 */

#ifndef __XML_RELAX_NG__
#define __XML_RELAX_NG__

#include <libxml/xmlversion.h>
#include <libxml/xmlerror.h>
#include <libxml/xmlstring.h>
#include <libxml/tree.h>
#include <libxml/parser.h>

#ifdef LIBXML_RELAXNG_ENABLED

#ifdef __cplusplus
extern "C" {
#endif

/** RelaxNG schema */
typedef struct _xmlRelaxNG xmlRelaxNG;
typedef xmlRelaxNG *xmlRelaxNGPtr;


/**
 * Signature of an error callback from a Relax-NG validation
 *
 * @param ctx  the validation context
 * @param msg  the message
 * @param ... extra arguments
 */
typedef void (*xmlRelaxNGValidityErrorFunc) (void *ctx,
						      const char *msg,
						      ...) LIBXML_ATTR_FORMAT(2,3);

/**
 * Signature of a warning callback from a Relax-NG validation
 *
 * @param ctx  the validation context
 * @param msg  the message
 * @param ... extra arguments
 */
typedef void (*xmlRelaxNGValidityWarningFunc) (void *ctx,
							const char *msg,
							...) LIBXML_ATTR_FORMAT(2,3);

/** RelaxNG parser context */
typedef struct _xmlRelaxNGParserCtxt xmlRelaxNGParserCtxt;
typedef xmlRelaxNGParserCtxt *xmlRelaxNGParserCtxtPtr;

/** RelaxNG validation context */
typedef struct _xmlRelaxNGValidCtxt xmlRelaxNGValidCtxt;
typedef xmlRelaxNGValidCtxt *xmlRelaxNGValidCtxtPtr;

/**
 * List of possible Relax NG validation errors
 */
typedef enum {
    XML_RELAXNG_OK = 0,
    XML_RELAXNG_ERR_MEMORY,
    XML_RELAXNG_ERR_TYPE,
    XML_RELAXNG_ERR_TYPEVAL,
    XML_RELAXNG_ERR_DUPID,
    XML_RELAXNG_ERR_TYPECMP,
    XML_RELAXNG_ERR_NOSTATE,
    XML_RELAXNG_ERR_NODEFINE,
    XML_RELAXNG_ERR_LISTEXTRA,
    XML_RELAXNG_ERR_LISTEMPTY,
    XML_RELAXNG_ERR_INTERNODATA,
    XML_RELAXNG_ERR_INTERSEQ,
    XML_RELAXNG_ERR_INTEREXTRA,
    XML_RELAXNG_ERR_ELEMNAME,
    XML_RELAXNG_ERR_ATTRNAME,
    XML_RELAXNG_ERR_ELEMNONS,
    XML_RELAXNG_ERR_ATTRNONS,
    XML_RELAXNG_ERR_ELEMWRONGNS,
    XML_RELAXNG_ERR_ATTRWRONGNS,
    XML_RELAXNG_ERR_ELEMEXTRANS,
    XML_RELAXNG_ERR_ATTREXTRANS,
    XML_RELAXNG_ERR_ELEMNOTEMPTY,
    XML_RELAXNG_ERR_NOELEM,
    XML_RELAXNG_ERR_NOTELEM,
    XML_RELAXNG_ERR_ATTRVALID,
    XML_RELAXNG_ERR_CONTENTVALID,
    XML_RELAXNG_ERR_EXTRACONTENT,
    XML_RELAXNG_ERR_INVALIDATTR,
    XML_RELAXNG_ERR_DATAELEM,
    XML_RELAXNG_ERR_VALELEM,
    XML_RELAXNG_ERR_LISTELEM,
    XML_RELAXNG_ERR_DATATYPE,
    XML_RELAXNG_ERR_VALUE,
    XML_RELAXNG_ERR_LIST,
    XML_RELAXNG_ERR_NOGRAMMAR,
    XML_RELAXNG_ERR_EXTRADATA,
    XML_RELAXNG_ERR_LACKDATA,
    XML_RELAXNG_ERR_INTERNAL,
    XML_RELAXNG_ERR_ELEMWRONG,
    XML_RELAXNG_ERR_TEXTWRONG
} xmlRelaxNGValidErr;

/**
 * List of possible Relax NG Parser flags
 */
typedef enum {
    XML_RELAXNGP_NONE = 0,
    XML_RELAXNGP_FREE_DOC = 1,
    XML_RELAXNGP_CRNG = 2
} xmlRelaxNGParserFlag;

XMLPUBFUN int
		    xmlRelaxNGInitTypes		(void);
XML_DEPRECATED
XMLPUBFUN void
		    xmlRelaxNGCleanupTypes	(void);

/*
 * Interfaces for parsing.
 */
XMLPUBFUN xmlRelaxNGParserCtxt *
		    xmlRelaxNGNewParserCtxt	(const char *URL);
XMLPUBFUN xmlRelaxNGParserCtxt *
		    xmlRelaxNGNewMemParserCtxt	(const char *buffer,
						 int size);
XMLPUBFUN xmlRelaxNGParserCtxt *
		    xmlRelaxNGNewDocParserCtxt	(xmlDoc *doc);

XMLPUBFUN int
		    xmlRelaxParserSetFlag	(xmlRelaxNGParserCtxt *ctxt,
						 int flag);

XMLPUBFUN void
		    xmlRelaxNGFreeParserCtxt	(xmlRelaxNGParserCtxt *ctxt);
XMLPUBFUN void
		    xmlRelaxNGSetParserErrors(xmlRelaxNGParserCtxt *ctxt,
					 xmlRelaxNGValidityErrorFunc err,
					 xmlRelaxNGValidityWarningFunc warn,
					 void *ctx);
XMLPUBFUN int
		    xmlRelaxNGGetParserErrors(xmlRelaxNGParserCtxt *ctxt,
					 xmlRelaxNGValidityErrorFunc *err,
					 xmlRelaxNGValidityWarningFunc *warn,
					 void **ctx);
XMLPUBFUN void
		    xmlRelaxNGSetParserStructuredErrors(
					 xmlRelaxNGParserCtxt *ctxt,
					 xmlStructuredErrorFunc serror,
					 void *ctx);
XMLPUBFUN void
		    xmlRelaxNGSetResourceLoader	(xmlRelaxNGParserCtxt *ctxt,
						 xmlResourceLoader loader,
						 void *vctxt);
XMLPUBFUN xmlRelaxNG *
		    xmlRelaxNGParse		(xmlRelaxNGParserCtxt *ctxt);
XMLPUBFUN void
		    xmlRelaxNGFree		(xmlRelaxNG *schema);
#ifdef LIBXML_DEBUG_ENABLED
XMLPUBFUN void
		    xmlRelaxNGDump		(FILE *output,
					 xmlRelaxNG *schema);
#endif /* LIBXML_DEBUG_ENABLED */
#ifdef LIBXML_OUTPUT_ENABLED
XMLPUBFUN void
		    xmlRelaxNGDumpTree	(FILE * output,
					 xmlRelaxNG *schema);
#endif /* LIBXML_OUTPUT_ENABLED */
/*
 * Interfaces for validating
 */
XMLPUBFUN void
		    xmlRelaxNGSetValidErrors(xmlRelaxNGValidCtxt *ctxt,
					 xmlRelaxNGValidityErrorFunc err,
					 xmlRelaxNGValidityWarningFunc warn,
					 void *ctx);
XMLPUBFUN int
		    xmlRelaxNGGetValidErrors(xmlRelaxNGValidCtxt *ctxt,
					 xmlRelaxNGValidityErrorFunc *err,
					 xmlRelaxNGValidityWarningFunc *warn,
					 void **ctx);
XMLPUBFUN void
			xmlRelaxNGSetValidStructuredErrors(xmlRelaxNGValidCtxt *ctxt,
					  xmlStructuredErrorFunc serror, void *ctx);
XMLPUBFUN xmlRelaxNGValidCtxt *
		    xmlRelaxNGNewValidCtxt	(xmlRelaxNG *schema);
XMLPUBFUN void
		    xmlRelaxNGFreeValidCtxt	(xmlRelaxNGValidCtxt *ctxt);
XMLPUBFUN int
		    xmlRelaxNGValidateDoc	(xmlRelaxNGValidCtxt *ctxt,
						 xmlDoc *doc);
/*
 * Interfaces for progressive validation when possible
 */
XMLPUBFUN int
		    xmlRelaxNGValidatePushElement	(xmlRelaxNGValidCtxt *ctxt,
					 xmlDoc *doc,
					 xmlNode *elem);
XMLPUBFUN int
		    xmlRelaxNGValidatePushCData	(xmlRelaxNGValidCtxt *ctxt,
					 const xmlChar *data,
					 int len);
XMLPUBFUN int
		    xmlRelaxNGValidatePopElement	(xmlRelaxNGValidCtxt *ctxt,
					 xmlDoc *doc,
					 xmlNode *elem);
XMLPUBFUN int
		    xmlRelaxNGValidateFullElement	(xmlRelaxNGValidCtxt *ctxt,
					 xmlDoc *doc,
					 xmlNode *elem);
XMLPUBFUN void
                    xmlRelaxNGValidCtxtClearErrors(xmlRelaxNGValidCtxt* ctxt);

#ifdef __cplusplus
}
#endif

#endif /* LIBXML_RELAXNG_ENABLED */

#endif /* __XML_RELAX_NG__ */

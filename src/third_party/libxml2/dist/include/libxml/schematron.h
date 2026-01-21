/**
 * @file
 * 
 * @brief XML Schematron implementation
 * 
 * interface to the XML Schematron validity checking.
 *
 * @copyright See Copyright for the status of this software.
 *
 * @author Daniel Veillard
 */


#ifndef __XML_SCHEMATRON_H__
#define __XML_SCHEMATRON_H__

#include <libxml/xmlversion.h>

#ifdef LIBXML_SCHEMATRON_ENABLED

#include <libxml/xmlerror.h>
#include <libxml/tree.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Schematron validation options
 */
typedef enum {
    /** quiet no report */
    XML_SCHEMATRON_OUT_QUIET = 1 << 0,
    /** build a textual report */
    XML_SCHEMATRON_OUT_TEXT = 1 << 1,
    /** output SVRL */
    XML_SCHEMATRON_OUT_XML = 1 << 2,
    /** output via xmlStructuredErrorFunc */
    XML_SCHEMATRON_OUT_ERROR = 1 << 3,
    /** output to a file descriptor */
    XML_SCHEMATRON_OUT_FILE = 1 << 8,
    /** output to a buffer */
    XML_SCHEMATRON_OUT_BUFFER = 1 << 9,
    /** output to I/O mechanism */
    XML_SCHEMATRON_OUT_IO = 1 << 10
} xmlSchematronValidOptions;

/** Schematron schema */
typedef struct _xmlSchematron xmlSchematron;
typedef xmlSchematron *xmlSchematronPtr;

/**
 * Signature of an error callback from a Schematron validation
 *
 * @param ctx  the validation context
 * @param msg  the message
 * @param ... extra arguments
 */
typedef void (*xmlSchematronValidityErrorFunc) (void *ctx, const char *msg, ...);

/**
 * Signature of a warning callback from a Schematron validation
 *
 * @param ctx  the validation context
 * @param msg  the message
 * @param ... extra arguments
 */
typedef void (*xmlSchematronValidityWarningFunc) (void *ctx, const char *msg, ...);

/** Schematron parser context */
typedef struct _xmlSchematronParserCtxt xmlSchematronParserCtxt;
typedef xmlSchematronParserCtxt *xmlSchematronParserCtxtPtr;

/** Schematron validation context */
typedef struct _xmlSchematronValidCtxt xmlSchematronValidCtxt;
typedef xmlSchematronValidCtxt *xmlSchematronValidCtxtPtr;

/*
 * Interfaces for parsing.
 */
XMLPUBFUN xmlSchematronParserCtxt *
	    xmlSchematronNewParserCtxt	(const char *URL);
XMLPUBFUN xmlSchematronParserCtxt *
	    xmlSchematronNewMemParserCtxt(const char *buffer,
					 int size);
XMLPUBFUN xmlSchematronParserCtxt *
	    xmlSchematronNewDocParserCtxt(xmlDoc *doc);
XMLPUBFUN void
	    xmlSchematronFreeParserCtxt	(xmlSchematronParserCtxt *ctxt);
/*****
XMLPUBFUN void
	    xmlSchematronSetParserErrors(xmlSchematronParserCtxt *ctxt,
					 xmlSchematronValidityErrorFunc err,
					 xmlSchematronValidityWarningFunc warn,
					 void *ctx);
XMLPUBFUN int
		xmlSchematronGetParserErrors(xmlSchematronParserCtxt *ctxt,
					xmlSchematronValidityErrorFunc * err,
					xmlSchematronValidityWarningFunc * warn,
					void **ctx);
XMLPUBFUN int
		xmlSchematronIsValid	(xmlSchematronValidCtxt *ctxt);
 *****/
XMLPUBFUN xmlSchematron *
	    xmlSchematronParse		(xmlSchematronParserCtxt *ctxt);
XMLPUBFUN void
	    xmlSchematronFree		(xmlSchematron *schema);
/*
 * Interfaces for validating
 */
XMLPUBFUN void
	    xmlSchematronSetValidStructuredErrors(
	                                  xmlSchematronValidCtxt *ctxt,
					  xmlStructuredErrorFunc serror,
					  void *ctx);
/******
XMLPUBFUN void
	    xmlSchematronSetValidErrors	(xmlSchematronValidCtxt *ctxt,
					 xmlSchematronValidityErrorFunc err,
					 xmlSchematronValidityWarningFunc warn,
					 void *ctx);
XMLPUBFUN int
	    xmlSchematronGetValidErrors	(xmlSchematronValidCtxt *ctxt,
					 xmlSchematronValidityErrorFunc *err,
					 xmlSchematronValidityWarningFunc *warn,
					 void **ctx);
XMLPUBFUN int
	    xmlSchematronSetValidOptions(xmlSchematronValidCtxt *ctxt,
					 int options);
XMLPUBFUN int
	    xmlSchematronValidCtxtGetOptions(xmlSchematronValidCtxt *ctxt);
XMLPUBFUN int
            xmlSchematronValidateOneElement (xmlSchematronValidCtxt *ctxt,
			                 xmlNode *elem);
 *******/

XMLPUBFUN xmlSchematronValidCtxt *
	    xmlSchematronNewValidCtxt	(xmlSchematron *schema,
					 int options);
XMLPUBFUN void
	    xmlSchematronFreeValidCtxt	(xmlSchematronValidCtxt *ctxt);
XMLPUBFUN int
	    xmlSchematronValidateDoc	(xmlSchematronValidCtxt *ctxt,
					 xmlDoc *instance);

#ifdef __cplusplus
}
#endif

#endif /* LIBXML_SCHEMATRON_ENABLED */
#endif /* __XML_SCHEMATRON_H__ */

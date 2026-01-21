/**
 * @file
 * 
 * @brief Regular expressions
 * 
 * A regular expression engine used for DTD and XML Schema
 * validation.
 *
 * @copyright See Copyright for the status of this software.
 *
 * @author Daniel Veillard
 */

#ifndef __XML_REGEXP_H__
#define __XML_REGEXP_H__

#include <stdio.h>
#include <libxml/xmlversion.h>
#include <libxml/xmlstring.h>

#ifdef LIBXML_REGEXP_ENABLED

#ifdef __cplusplus
extern "C" {
#endif

/**
 * A libxml regular expression
 */
typedef struct _xmlRegexp xmlRegexp;
typedef xmlRegexp *xmlRegexpPtr;

/**
 * A libxml progressive regular expression evaluation context
 */
typedef struct _xmlRegExecCtxt xmlRegExecCtxt;
typedef xmlRegExecCtxt *xmlRegExecCtxtPtr;

/*
 * The POSIX like API
 */
XMLPUBFUN xmlRegexp *
		    xmlRegexpCompile	(const xmlChar *regexp);
XMLPUBFUN void			 xmlRegFreeRegexp(xmlRegexp *regexp);
XMLPUBFUN int
		    xmlRegexpExec	(xmlRegexp *comp,
					 const xmlChar *value);
XML_DEPRECATED
XMLPUBFUN void
		    xmlRegexpPrint	(FILE *output,
					 xmlRegexp *regexp);
XMLPUBFUN int
		    xmlRegexpIsDeterminist(xmlRegexp *comp);

/**
 * Callback function when doing a transition in the automata
 *
 * @param exec  the regular expression context
 * @param token  the current token string
 * @param transdata  transition data
 * @param inputdata  input data
 */
typedef void (*xmlRegExecCallbacks) (xmlRegExecCtxt *exec,
	                             const xmlChar *token,
				     void *transdata,
				     void *inputdata);

/*
 * The progressive API
 */
XML_DEPRECATED
XMLPUBFUN xmlRegExecCtxt *
		    xmlRegNewExecCtxt	(xmlRegexp *comp,
					 xmlRegExecCallbacks callback,
					 void *data);
XML_DEPRECATED
XMLPUBFUN void
		    xmlRegFreeExecCtxt	(xmlRegExecCtxt *exec);
XML_DEPRECATED
XMLPUBFUN int
		    xmlRegExecPushString(xmlRegExecCtxt *exec,
					 const xmlChar *value,
					 void *data);
XML_DEPRECATED
XMLPUBFUN int
		    xmlRegExecPushString2(xmlRegExecCtxt *exec,
					 const xmlChar *value,
					 const xmlChar *value2,
					 void *data);

XML_DEPRECATED
XMLPUBFUN int
		    xmlRegExecNextValues(xmlRegExecCtxt *exec,
					 int *nbval,
					 int *nbneg,
					 xmlChar **values,
					 int *terminal);
XML_DEPRECATED
XMLPUBFUN int
		    xmlRegExecErrInfo	(xmlRegExecCtxt *exec,
					 const xmlChar **string,
					 int *nbval,
					 int *nbneg,
					 xmlChar **values,
					 int *terminal);

#ifdef __cplusplus
}
#endif

#endif /* LIBXML_REGEXP_ENABLED */

#endif /*__XML_REGEXP_H__ */

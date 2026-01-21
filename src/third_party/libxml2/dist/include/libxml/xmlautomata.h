/**
 * @file
 * 
 * @brief API to build regexp automata
 * 
 * These are internal functions and shouldn't be used.
 *
 * @copyright See Copyright for the status of this software.
 *
 * @author Daniel Veillard
 */

#ifndef __XML_AUTOMATA_H__
#define __XML_AUTOMATA_H__

#include <libxml/xmlversion.h>

#ifdef LIBXML_REGEXP_ENABLED

#include <libxml/xmlstring.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * A libxml automata description
 *
 * It can be compiled into a regexp
 */
typedef struct _xmlAutomata xmlAutomata;
typedef xmlAutomata *xmlAutomataPtr;

/**
 * A state in the automata description
 */
typedef struct _xmlAutomataState xmlAutomataState;
typedef xmlAutomataState *xmlAutomataStatePtr;

/*
 * Building API
 */
XML_DEPRECATED
XMLPUBFUN xmlAutomata *
		    xmlNewAutomata		(void);
XML_DEPRECATED
XMLPUBFUN void
		    xmlFreeAutomata		(xmlAutomata *am);

XML_DEPRECATED
XMLPUBFUN xmlAutomataState *
		    xmlAutomataGetInitState	(xmlAutomata *am);
XML_DEPRECATED
XMLPUBFUN int
		    xmlAutomataSetFinalState	(xmlAutomata *am,
						 xmlAutomataState *state);
XML_DEPRECATED
XMLPUBFUN xmlAutomataState *
		    xmlAutomataNewState		(xmlAutomata *am);
XML_DEPRECATED
XMLPUBFUN xmlAutomataState *
		    xmlAutomataNewTransition	(xmlAutomata *am,
						 xmlAutomataState *from,
						 xmlAutomataState *to,
						 const xmlChar *token,
						 void *data);
XML_DEPRECATED
XMLPUBFUN xmlAutomataState *
		    xmlAutomataNewTransition2	(xmlAutomata *am,
						 xmlAutomataState *from,
						 xmlAutomataState *to,
						 const xmlChar *token,
						 const xmlChar *token2,
						 void *data);
XML_DEPRECATED
XMLPUBFUN xmlAutomataState *
                    xmlAutomataNewNegTrans	(xmlAutomata *am,
						 xmlAutomataState *from,
						 xmlAutomataState *to,
						 const xmlChar *token,
						 const xmlChar *token2,
						 void *data);

XML_DEPRECATED
XMLPUBFUN xmlAutomataState *
		    xmlAutomataNewCountTrans	(xmlAutomata *am,
						 xmlAutomataState *from,
						 xmlAutomataState *to,
						 const xmlChar *token,
						 int min,
						 int max,
						 void *data);
XML_DEPRECATED
XMLPUBFUN xmlAutomataState *
		    xmlAutomataNewCountTrans2	(xmlAutomata *am,
						 xmlAutomataState *from,
						 xmlAutomataState *to,
						 const xmlChar *token,
						 const xmlChar *token2,
						 int min,
						 int max,
						 void *data);
XML_DEPRECATED
XMLPUBFUN xmlAutomataState *
		    xmlAutomataNewOnceTrans	(xmlAutomata *am,
						 xmlAutomataState *from,
						 xmlAutomataState *to,
						 const xmlChar *token,
						 int min,
						 int max,
						 void *data);
XML_DEPRECATED
XMLPUBFUN xmlAutomataState *
		    xmlAutomataNewOnceTrans2	(xmlAutomata *am,
						 xmlAutomataState *from,
						 xmlAutomataState *to,
						 const xmlChar *token,
						 const xmlChar *token2,
						 int min,
						 int max,
						 void *data);
XML_DEPRECATED
XMLPUBFUN xmlAutomataState *
		    xmlAutomataNewAllTrans	(xmlAutomata *am,
						 xmlAutomataState *from,
						 xmlAutomataState *to,
						 int lax);
XML_DEPRECATED
XMLPUBFUN xmlAutomataState *
		    xmlAutomataNewEpsilon	(xmlAutomata *am,
						 xmlAutomataState *from,
						 xmlAutomataState *to);
XML_DEPRECATED
XMLPUBFUN xmlAutomataState *
		    xmlAutomataNewCountedTrans	(xmlAutomata *am,
						 xmlAutomataState *from,
						 xmlAutomataState *to,
						 int counter);
XML_DEPRECATED
XMLPUBFUN xmlAutomataState *
		    xmlAutomataNewCounterTrans	(xmlAutomata *am,
						 xmlAutomataState *from,
						 xmlAutomataState *to,
						 int counter);
XML_DEPRECATED
XMLPUBFUN int
		    xmlAutomataNewCounter	(xmlAutomata *am,
						 int min,
						 int max);

XML_DEPRECATED
XMLPUBFUN struct _xmlRegexp *
		    xmlAutomataCompile		(xmlAutomata *am);
XML_DEPRECATED
XMLPUBFUN int
		    xmlAutomataIsDeterminist	(xmlAutomata *am);

#ifdef __cplusplus
}
#endif

#endif /* LIBXML_REGEXP_ENABLED */

#endif /* __XML_AUTOMATA_H__ */

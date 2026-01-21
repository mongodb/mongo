/**
 * @file
 * 
 * @brief string dictionary
 * 
 * dictionary of reusable strings, just used to avoid allocation
 *         and freeing operations.
 *
 * @copyright See Copyright for the status of this software.
 *
 * @author Daniel Veillard
 */

#ifndef __XML_DICT_H__
#define __XML_DICT_H__

#include <stddef.h>
#include <libxml/xmlversion.h>
#include <libxml/xmlstring.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Dictionary (pool for interned strings)
 */
typedef struct _xmlDict xmlDict;
typedef xmlDict *xmlDictPtr;

/*
 * Initializer
 */
XML_DEPRECATED
XMLPUBFUN int  xmlInitializeDict(void);

/*
 * Constructor and destructor.
 */
XMLPUBFUN xmlDict *
			xmlDictCreate	(void);
XMLPUBFUN size_t
			xmlDictSetLimit	(xmlDict *dict,
                                         size_t limit);
XMLPUBFUN size_t
			xmlDictGetUsage (xmlDict *dict);
XMLPUBFUN xmlDict *
			xmlDictCreateSub(xmlDict *sub);
XMLPUBFUN int
			xmlDictReference(xmlDict *dict);
XMLPUBFUN void
			xmlDictFree	(xmlDict *dict);

/*
 * Lookup of entry in the dictionary.
 */
XMLPUBFUN const xmlChar *
			xmlDictLookup	(xmlDict *dict,
		                         const xmlChar *name,
		                         int len);
XMLPUBFUN const xmlChar *
			xmlDictExists	(xmlDict *dict,
		                         const xmlChar *name,
		                         int len);
XMLPUBFUN const xmlChar *
			xmlDictQLookup	(xmlDict *dict,
		                         const xmlChar *prefix,
		                         const xmlChar *name);
XMLPUBFUN int
			xmlDictOwns	(xmlDict *dict,
					 const xmlChar *str);
XMLPUBFUN int
			xmlDictSize	(xmlDict *dict);

/*
 * Cleanup function
 */
XML_DEPRECATED
XMLPUBFUN void
                        xmlDictCleanup  (void);

#ifdef __cplusplus
}
#endif
#endif /* ! __XML_DICT_H__ */

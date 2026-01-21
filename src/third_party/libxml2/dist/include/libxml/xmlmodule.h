/**
 * @file
 *
 * @brief Dynamic module loading
 *
 * API for dynamic module loading. Only used by old libxslt versions
 * and subject to removal.
 *
 * @copyright See Copyright for the status of this software.
 *
 * @author Joel W. Reed
 */

#ifndef __XML_MODULE_H__
#define __XML_MODULE_H__

#include <libxml/xmlversion.h>

#ifdef LIBXML_MODULES_ENABLED

#ifdef __cplusplus
extern "C" {
#endif

/**
 * A handle to a dynamically loaded module
 */
typedef struct _xmlModule xmlModule;
typedef xmlModule *xmlModulePtr;

/**
 * enumeration of options that can be passed down to #xmlModuleOpen
 */
typedef enum {
    XML_MODULE_LAZY = 1,	/* lazy binding */
    XML_MODULE_LOCAL= 2		/* local binding */
} xmlModuleOption;

XML_DEPRECATED
XMLPUBFUN xmlModule *xmlModuleOpen	(const char *filename,
						 int options);

XML_DEPRECATED
XMLPUBFUN int xmlModuleSymbol		(xmlModule *module,
						 const char* name,
						 void **result);

XML_DEPRECATED
XMLPUBFUN int xmlModuleClose		(xmlModule *module);

XML_DEPRECATED
XMLPUBFUN int xmlModuleFree		(xmlModule *module);

#ifdef __cplusplus
}
#endif

#endif /* LIBXML_MODULES_ENABLED */

#endif /*__XML_MODULE_H__ */

/**
 * @file
 *
 * @brief interfaces for thread handling
 * 
 * set of generic threading related routines
 *              should work with pthreads, Windows native or TLS threads
 *
 * @copyright See Copyright for the status of this software.
 *
 * @author Daniel Veillard
 */

#ifndef __XML_THREADS_H__
#define __XML_THREADS_H__

#include <libxml/xmlversion.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Mutual exclusion object */
typedef struct _xmlMutex xmlMutex;
typedef xmlMutex *xmlMutexPtr;

/** Reentrant mutual exclusion object */
typedef struct _xmlRMutex xmlRMutex;
typedef xmlRMutex *xmlRMutexPtr;

XMLPUBFUN int
			xmlCheckThreadLocalStorage(void);

XMLPUBFUN xmlMutex *
			xmlNewMutex	(void);
XMLPUBFUN void
			xmlMutexLock	(xmlMutex *tok);
XMLPUBFUN void
			xmlMutexUnlock	(xmlMutex *tok);
XMLPUBFUN void
			xmlFreeMutex	(xmlMutex *tok);

XMLPUBFUN xmlRMutex *
			xmlNewRMutex	(void);
XMLPUBFUN void
			xmlRMutexLock	(xmlRMutex *tok);
XMLPUBFUN void
			xmlRMutexUnlock	(xmlRMutex *tok);
XMLPUBFUN void
			xmlFreeRMutex	(xmlRMutex *tok);

/*
 * Library wide APIs.
 */
XML_DEPRECATED
XMLPUBFUN void
			xmlInitThreads	(void);
XMLPUBFUN void
			xmlLockLibrary	(void);
XMLPUBFUN void
			xmlUnlockLibrary(void);
XML_DEPRECATED
XMLPUBFUN void
			xmlCleanupThreads(void);

/** @cond IGNORE */
#if defined(LIBXML_THREAD_ENABLED) && defined(_WIN32) && \
    defined(LIBXML_STATIC_FOR_DLL)
int
xmlDllMain(void *hinstDLL, unsigned long fdwReason,
           void *lpvReserved);
#endif
/** @endcond */

#ifdef __cplusplus
}
#endif


#endif /* __XML_THREADS_H__ */

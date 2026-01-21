/**
 * @file
 * 
 * @brief interface for the memory allocator
 * 
 * provides interfaces for the memory allocator,
 *              including debugging capabilities.
 *
 * @copyright See Copyright for the status of this software.
 *
 * @author Daniel Veillard
 */


#ifndef __DEBUG_MEMORY_ALLOC__
#define __DEBUG_MEMORY_ALLOC__

#include <stdio.h>
#include <libxml/xmlversion.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The XML memory wrapper support 4 basic overloadable functions.
 */
/**
 * Signature for a free() implementation.
 *
 * @param mem  an already allocated block of memory
 */
typedef void (*xmlFreeFunc)(void *mem);
/**
 * Signature for a malloc() implementation.
 *
 * @param size  the size requested in bytes
 * @returns a pointer to the newly allocated block or NULL in case of error.
 */
typedef void *(*xmlMallocFunc)(size_t size) LIBXML_ATTR_ALLOC_SIZE(1);

/**
 * Signature for a realloc() implementation.
 *
 * @param mem  an already allocated block of memory
 * @param size  the new size requested in bytes
 * @returns a pointer to the newly reallocated block or NULL in case of error.
 */
typedef void *(*xmlReallocFunc)(void *mem, size_t size);

/**
 * Signature for an strdup() implementation.
 *
 * @param str  a zero terminated string
 * @returns the copy of the string or NULL in case of error.
 */
typedef char *(*xmlStrdupFunc)(const char *str);

/*
 * In general the memory allocation entry points are not kept
 * thread specific but this can be overridden by LIBXML_THREAD_ALLOC_ENABLED
 *    - xmlMalloc
 *    - xmlMallocAtomic
 *    - xmlRealloc
 *    - xmlMemStrdup
 *    - xmlFree
 */
#ifdef LIBXML_THREAD_ALLOC_ENABLED

XMLPUBFUN xmlMallocFunc *__xmlMalloc(void);
XMLPUBFUN xmlMallocFunc *__xmlMallocAtomic(void);
XMLPUBFUN xmlReallocFunc *__xmlRealloc(void);
XMLPUBFUN xmlFreeFunc *__xmlFree(void);
XMLPUBFUN xmlStrdupFunc *__xmlMemStrdup(void);

#ifndef XML_GLOBALS_NO_REDEFINITION
  #define xmlMalloc (*__xmlMalloc())
  #define xmlMallocAtomic (*__xmlMallocAtomic())
  #define xmlRealloc (*__xmlRealloc())
  #define xmlFree (*__xmlFree())
  #define xmlMemStrdup (*__xmlMemStrdup())
#endif

#else

/**
 * The variable holding the libxml malloc() implementation
 */
XMLPUBVAR xmlMallocFunc xmlMalloc;
/**
 * The variable holding the libxml malloc() implementation for atomic
 * data (i.e. blocks not containing pointers), useful when using a
 * garbage collecting allocator.
 *
 * @deprecated Use #xmlMalloc
 */
XMLPUBVAR xmlMallocFunc xmlMallocAtomic;
/**
 * The variable holding the libxml realloc() implementation
 */
XMLPUBVAR xmlReallocFunc xmlRealloc;
/**
 * The variable holding the libxml free() implementation
 */
XMLPUBVAR xmlFreeFunc xmlFree;
/**
 * The variable holding the libxml strdup() implementation
 */
XMLPUBVAR xmlStrdupFunc xmlMemStrdup;

#endif

/*
 * The way to overload the existing functions.
 * The xmlGc function have an extra entry for atomic block
 * allocations useful for garbage collected memory allocators
 */
XMLPUBFUN int
	xmlMemSetup	(xmlFreeFunc freeFunc,
			 xmlMallocFunc mallocFunc,
			 xmlReallocFunc reallocFunc,
			 xmlStrdupFunc strdupFunc);
XMLPUBFUN int
	xmlMemGet	(xmlFreeFunc *freeFunc,
			 xmlMallocFunc *mallocFunc,
			 xmlReallocFunc *reallocFunc,
			 xmlStrdupFunc *strdupFunc);
XML_DEPRECATED
XMLPUBFUN int
	xmlGcMemSetup	(xmlFreeFunc freeFunc,
			 xmlMallocFunc mallocFunc,
			 xmlMallocFunc mallocAtomicFunc,
			 xmlReallocFunc reallocFunc,
			 xmlStrdupFunc strdupFunc);
XML_DEPRECATED
XMLPUBFUN int
	xmlGcMemGet	(xmlFreeFunc *freeFunc,
			 xmlMallocFunc *mallocFunc,
			 xmlMallocFunc *mallocAtomicFunc,
			 xmlReallocFunc *reallocFunc,
			 xmlStrdupFunc *strdupFunc);

/*
 * Initialization of the memory layer.
 */
XML_DEPRECATED
XMLPUBFUN int
	xmlInitMemory	(void);

/*
 * Cleanup of the memory layer.
 */
XML_DEPRECATED
XMLPUBFUN void
                xmlCleanupMemory        (void);
/*
 * These are specific to the XML debug memory wrapper.
 */
XMLPUBFUN size_t
	xmlMemSize	(void *ptr);
XMLPUBFUN int
	xmlMemUsed	(void);
XMLPUBFUN int
	xmlMemBlocks	(void);
XML_DEPRECATED
XMLPUBFUN void
	xmlMemDisplay	(FILE *fp);
XML_DEPRECATED
XMLPUBFUN void
	xmlMemDisplayLast(FILE *fp, long nbBytes);
XML_DEPRECATED
XMLPUBFUN void
	xmlMemShow	(FILE *fp, int nr);
XML_DEPRECATED
XMLPUBFUN void
	xmlMemoryDump	(void);
XMLPUBFUN void *
	xmlMemMalloc	(size_t size) LIBXML_ATTR_ALLOC_SIZE(1);
XMLPUBFUN void *
	xmlMemRealloc	(void *ptr,size_t size);
XMLPUBFUN void
	xmlMemFree	(void *ptr);
XMLPUBFUN char *
	xmlMemoryStrdup	(const char *str);
XML_DEPRECATED
XMLPUBFUN void *
	xmlMallocLoc	(size_t size, const char *file, int line) LIBXML_ATTR_ALLOC_SIZE(1);
XML_DEPRECATED
XMLPUBFUN void *
	xmlReallocLoc	(void *ptr, size_t size, const char *file, int line);
XML_DEPRECATED
XMLPUBFUN void *
	xmlMallocAtomicLoc (size_t size, const char *file, int line) LIBXML_ATTR_ALLOC_SIZE(1);
XML_DEPRECATED
XMLPUBFUN char *
	xmlMemStrdupLoc	(const char *str, const char *file, int line);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif  /* __DEBUG_MEMORY_ALLOC__ */


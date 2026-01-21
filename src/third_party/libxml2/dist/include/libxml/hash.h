/**
 * @file
 * 
 * @brief Chained hash tables
 * 
 * This module implements the hash table support used in
 *		various places in the library.
 *
 * @copyright See Copyright for the status of this software.
 */

#ifndef __XML_HASH_H__
#define __XML_HASH_H__

#include <libxml/xmlversion.h>
#include <libxml/dict.h>
#include <libxml/xmlstring.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Hash table mapping strings to pointers
 *
 * Also supports lookup using two or three strings as key.
 */
typedef struct _xmlHashTable xmlHashTable;
typedef xmlHashTable *xmlHashTablePtr;

/*
 * Recent version of gcc produce a warning when a function pointer is assigned
 * to an object pointer, or vice versa.  The following macro is a dirty hack
 * to allow suppression of the warning.  If your architecture has function
 * pointers which are a different size than a void pointer, there may be some
 * serious trouble within the library.
 */
/**
 * Macro to do a casting from an object pointer to a
 * function pointer without encountering a warning from
 * gcc
 *
 * \#define XML_CAST_FPTR(fptr) (*(void **)(&fptr))
 * This macro violated ISO C aliasing rules (gcc4 on s390 broke)
 * so it is disabled now
 *
 * @param fptr  pointer to a function
 */

#define XML_CAST_FPTR(fptr) fptr

/*
 * function types:
 */
/**
 * Callback to free data from a hash.
 *
 * @param payload  the data in the hash
 * @param name  the name associated
 */
typedef void (*xmlHashDeallocator)(void *payload, const xmlChar *name);
/**
 * Callback to copy data from a hash.
 *
 * @param payload  the data in the hash
 * @param name  the name associated
 * @returns a copy of the data or NULL in case of error.
 */
typedef void *(*xmlHashCopier)(void *payload, const xmlChar *name);
/**
 * Callback when scanning data in a hash with the simple scanner.
 *
 * @param payload  the data in the hash
 * @param data  extra scanner data
 * @param name  the name associated
 */
typedef void (*xmlHashScanner)(void *payload, void *data, const xmlChar *name);
/**
 * Callback when scanning data in a hash with the full scanner.
 *
 * @param payload  the data in the hash
 * @param data  extra scanner data
 * @param name  the name associated
 * @param name2  the second name associated
 * @param name3  the third name associated
 */
typedef void (*xmlHashScannerFull)(void *payload, void *data,
				   const xmlChar *name, const xmlChar *name2,
				   const xmlChar *name3);

/*
 * Constructor and destructor.
 */
XMLPUBFUN xmlHashTable *
		xmlHashCreate		(int size);
XMLPUBFUN xmlHashTable *
		xmlHashCreateDict	(int size,
					 xmlDict *dict);
XMLPUBFUN void
		xmlHashFree		(xmlHashTable *hash,
					 xmlHashDeallocator dealloc);
XMLPUBFUN void
		xmlHashDefaultDeallocator(void *entry,
					 const xmlChar *name);

/*
 * Add a new entry to the hash table.
 */
XMLPUBFUN int
		xmlHashAdd		(xmlHashTable *hash,
		                         const xmlChar *name,
		                         void *userdata);
XMLPUBFUN int
		xmlHashAddEntry		(xmlHashTable *hash,
		                         const xmlChar *name,
		                         void *userdata);
XMLPUBFUN int
		xmlHashUpdateEntry	(xmlHashTable *hash,
		                         const xmlChar *name,
		                         void *userdata,
					 xmlHashDeallocator dealloc);
XMLPUBFUN int
		xmlHashAdd2		(xmlHashTable *hash,
		                         const xmlChar *name,
		                         const xmlChar *name2,
		                         void *userdata);
XMLPUBFUN int
		xmlHashAddEntry2	(xmlHashTable *hash,
		                         const xmlChar *name,
		                         const xmlChar *name2,
		                         void *userdata);
XMLPUBFUN int
		xmlHashUpdateEntry2	(xmlHashTable *hash,
		                         const xmlChar *name,
		                         const xmlChar *name2,
		                         void *userdata,
					 xmlHashDeallocator dealloc);
XMLPUBFUN int
		xmlHashAdd3		(xmlHashTable *hash,
		                         const xmlChar *name,
		                         const xmlChar *name2,
		                         const xmlChar *name3,
		                         void *userdata);
XMLPUBFUN int
		xmlHashAddEntry3	(xmlHashTable *hash,
		                         const xmlChar *name,
		                         const xmlChar *name2,
		                         const xmlChar *name3,
		                         void *userdata);
XMLPUBFUN int
		xmlHashUpdateEntry3	(xmlHashTable *hash,
		                         const xmlChar *name,
		                         const xmlChar *name2,
		                         const xmlChar *name3,
		                         void *userdata,
					 xmlHashDeallocator dealloc);

/*
 * Remove an entry from the hash table.
 */
XMLPUBFUN int
		xmlHashRemoveEntry	(xmlHashTable *hash,
					 const xmlChar *name,
					 xmlHashDeallocator dealloc);
XMLPUBFUN int
		xmlHashRemoveEntry2	(xmlHashTable *hash,
					 const xmlChar *name,
					 const xmlChar *name2,
					 xmlHashDeallocator dealloc);
XMLPUBFUN int 
		xmlHashRemoveEntry3	(xmlHashTable *hash,
					 const xmlChar *name,
					 const xmlChar *name2,
					 const xmlChar *name3,
					 xmlHashDeallocator dealloc);

/*
 * Retrieve the payload.
 */
XMLPUBFUN void *
		xmlHashLookup		(xmlHashTable *hash,
					 const xmlChar *name);
XMLPUBFUN void *
		xmlHashLookup2		(xmlHashTable *hash,
					 const xmlChar *name,
					 const xmlChar *name2);
XMLPUBFUN void *
		xmlHashLookup3		(xmlHashTable *hash,
					 const xmlChar *name,
					 const xmlChar *name2,
					 const xmlChar *name3);
XMLPUBFUN void *
		xmlHashQLookup		(xmlHashTable *hash,
					 const xmlChar *prefix,
					 const xmlChar *name);
XMLPUBFUN void *
		xmlHashQLookup2		(xmlHashTable *hash,
					 const xmlChar *prefix,
					 const xmlChar *name,
					 const xmlChar *prefix2,
					 const xmlChar *name2);
XMLPUBFUN void *
		xmlHashQLookup3		(xmlHashTable *hash,
					 const xmlChar *prefix,
					 const xmlChar *name,
					 const xmlChar *prefix2,
					 const xmlChar *name2,
					 const xmlChar *prefix3,
					 const xmlChar *name3);

/*
 * Helpers.
 */
XMLPUBFUN xmlHashTable *
		xmlHashCopySafe		(xmlHashTable *hash,
					 xmlHashCopier copy,
					 xmlHashDeallocator dealloc);
XMLPUBFUN xmlHashTable *
		xmlHashCopy		(xmlHashTable *hash,
					 xmlHashCopier copy);
XMLPUBFUN int
		xmlHashSize		(xmlHashTable *hash);
XMLPUBFUN void
		xmlHashScan		(xmlHashTable *hash,
					 xmlHashScanner scan,
					 void *data);
XMLPUBFUN void
		xmlHashScan3		(xmlHashTable *hash,
					 const xmlChar *name,
					 const xmlChar *name2,
					 const xmlChar *name3,
					 xmlHashScanner scan,
					 void *data);
XMLPUBFUN void
		xmlHashScanFull		(xmlHashTable *hash,
					 xmlHashScannerFull scan,
					 void *data);
XMLPUBFUN void
		xmlHashScanFull3	(xmlHashTable *hash,
					 const xmlChar *name,
					 const xmlChar *name2,
					 const xmlChar *name3,
					 xmlHashScannerFull scan,
					 void *data);
#ifdef __cplusplus
}
#endif
#endif /* ! __XML_HASH_H__ */

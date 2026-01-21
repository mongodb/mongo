/*
 * dict.c: dictionary of reusable strings, just used to avoid allocation
 *         and freeing operations.
 *
 * Copyright (C) 2003-2012 Daniel Veillard.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE. THE AUTHORS AND
 * CONTRIBUTORS ACCEPT NO RESPONSIBILITY IN ANY CONCEIVABLE MANNER.
 *
 * Author: Daniel Veillard
 */

#define IN_LIBXML
#include "libxml.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "private/dict.h"
#include "private/error.h"
#include "private/globals.h"
#include "private/threads.h"

#include <libxml/parser.h>
#include <libxml/dict.h>
#include <libxml/xmlmemory.h>
#include <libxml/xmlstring.h>

#ifndef SIZE_MAX
  #define SIZE_MAX ((size_t) -1)
#endif

#define MAX_FILL_NUM 7
#define MAX_FILL_DENOM 8
#define MIN_HASH_SIZE 8
#define MAX_HASH_SIZE (1u << 31)

typedef struct _xmlDictStrings xmlDictStrings;
typedef xmlDictStrings *xmlDictStringsPtr;
struct _xmlDictStrings {
    xmlDictStringsPtr next;
    xmlChar *free;
    xmlChar *end;
    size_t size;
    size_t nbStrings;
    xmlChar array[1];
};

typedef xmlHashedString xmlDictEntry;

/*
 * The entire dictionary
 */
struct _xmlDict {
    int ref_counter;

    xmlDictEntry *table;
    size_t size;
    unsigned int nbElems;
    xmlDictStringsPtr strings;

    struct _xmlDict *subdict;
    /* used for randomization */
    unsigned seed;
    /* used to impose a limit on size */
    size_t limit;
};

/*
 * A mutex for modifying the reference counter for shared
 * dictionaries.
 */
static xmlMutex xmlDictMutex;

/**
 * @deprecated Alias for #xmlInitParser.
 *
 * @returns 0.
 */
int
xmlInitializeDict(void) {
    xmlInitParser();
    return(0);
}

/**
 * Initialize mutex.
 */
void
xmlInitDictInternal(void) {
    xmlInitMutex(&xmlDictMutex);
}

/**
 * @deprecated This function is a no-op. Call #xmlCleanupParser
 * to free global state but see the warnings there. #xmlCleanupParser
 * should be only called once at program exit. In most cases, you don't
 * have call cleanup functions at all.
 */
void
xmlDictCleanup(void) {
}

/**
 * Free the dictionary mutex.
 */
void
xmlCleanupDictInternal(void) {
    xmlCleanupMutex(&xmlDictMutex);
}

/*
 * @param dict  the dictionary
 * @param name  the name of the userdata
 * @param len  the length of the name
 *
 * Add the string to the array[s]
 *
 * @returns the pointer of the local string, or NULL in case of error.
 */
static const xmlChar *
xmlDictAddString(xmlDictPtr dict, const xmlChar *name, unsigned int namelen) {
    xmlDictStringsPtr pool;
    const xmlChar *ret;
    size_t size = 0; /* + sizeof(_xmlDictStrings) == 1024 */
    size_t limit = 0;

    pool = dict->strings;
    while (pool != NULL) {
	if ((size_t)(pool->end - pool->free) > namelen)
	    goto found_pool;
	if (pool->size > size) size = pool->size;
        limit += pool->size;
	pool = pool->next;
    }
    /*
     * Not found, need to allocate
     */
    if (pool == NULL) {
        if ((dict->limit > 0) && (limit > dict->limit)) {
            return(NULL);
        }

        if (size == 0) {
            size = 1000;
        } else {
            if (size < (SIZE_MAX - sizeof(xmlDictStrings)) / 4)
                size *= 4; /* exponential growth */
            else
                size = SIZE_MAX - sizeof(xmlDictStrings);
        }
        if (size / 4 < namelen) {
            if ((size_t) namelen + 0 < (SIZE_MAX - sizeof(xmlDictStrings)) / 4)
                size = 4 * (size_t) namelen; /* just in case ! */
            else
                return(NULL);
        }
	pool = (xmlDictStringsPtr) xmlMalloc(sizeof(xmlDictStrings) + size);
	if (pool == NULL)
	    return(NULL);
	pool->size = size;
	pool->nbStrings = 0;
	pool->free = &pool->array[0];
	pool->end = &pool->array[size];
	pool->next = dict->strings;
	dict->strings = pool;
    }
found_pool:
    ret = pool->free;
    memcpy(pool->free, name, namelen);
    pool->free += namelen;
    *(pool->free++) = 0;
    pool->nbStrings++;
    return(ret);
}

/*
 * @param dict  the dictionary
 * @param prefix  the prefix of the userdata
 * @param plen  the prefix length
 * @param name  the name of the userdata
 * @param len  the length of the name
 *
 * Add the QName to the array[s]
 *
 * @returns the pointer of the local string, or NULL in case of error.
 */
static const xmlChar *
xmlDictAddQString(xmlDictPtr dict, const xmlChar *prefix, unsigned int plen,
                 const xmlChar *name, unsigned int namelen)
{
    xmlDictStringsPtr pool;
    const xmlChar *ret;
    size_t size = 0; /* + sizeof(_xmlDictStrings) == 1024 */
    size_t limit = 0;

    pool = dict->strings;
    while (pool != NULL) {
	if ((size_t)(pool->end - pool->free) > namelen + plen + 1)
	    goto found_pool;
	if (pool->size > size) size = pool->size;
        limit += pool->size;
	pool = pool->next;
    }
    /*
     * Not found, need to allocate
     */
    if (pool == NULL) {
        if ((dict->limit > 0) && (limit > dict->limit)) {
            return(NULL);
        }

        if (size == 0) size = 1000;
	else size *= 4; /* exponential growth */
        if (size < 4 * (namelen + plen + 1))
	    size = 4 * (namelen + plen + 1); /* just in case ! */
	pool = (xmlDictStringsPtr) xmlMalloc(sizeof(xmlDictStrings) + size);
	if (pool == NULL)
	    return(NULL);
	pool->size = size;
	pool->nbStrings = 0;
	pool->free = &pool->array[0];
	pool->end = &pool->array[size];
	pool->next = dict->strings;
	dict->strings = pool;
    }
found_pool:
    ret = pool->free;
    memcpy(pool->free, prefix, plen);
    pool->free += plen;
    *(pool->free++) = ':';
    memcpy(pool->free, name, namelen);
    pool->free += namelen;
    *(pool->free++) = 0;
    pool->nbStrings++;
    return(ret);
}

/**
 * Create a new dictionary
 *
 * @returns the newly created dictionary, or NULL if an error occurred.
 */
xmlDict *
xmlDictCreate(void) {
    xmlDictPtr dict;

    xmlInitParser();

    dict = xmlMalloc(sizeof(xmlDict));
    if (dict == NULL)
        return(NULL);
    dict->ref_counter = 1;
    dict->limit = 0;

    dict->size = 0;
    dict->nbElems = 0;
    dict->table = NULL;
    dict->strings = NULL;
    dict->subdict = NULL;
    dict->seed = xmlRandom();
#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
    dict->seed = 0;
#endif
    return(dict);
}

/**
 * Create a new dictionary, inheriting strings from the read-only
 * dictionary `sub`. On lookup, strings are first searched in the
 * new dictionary, then in `sub`, and if not found are created in the
 * new dictionary.
 *
 * @param sub  an existing dictionary
 * @returns the newly created dictionary, or NULL if an error occurred.
 */
xmlDict *
xmlDictCreateSub(xmlDict *sub) {
    xmlDictPtr dict = xmlDictCreate();

    if ((dict != NULL) && (sub != NULL)) {
        dict->seed = sub->seed;
        dict->subdict = sub;
	xmlDictReference(dict->subdict);
    }
    return(dict);
}

/**
 * Increment the reference counter of a dictionary
 *
 * @param dict  the dictionary
 * @returns 0 in case of success and -1 in case of error
 */
int
xmlDictReference(xmlDict *dict) {
    if (dict == NULL) return -1;
    xmlMutexLock(&xmlDictMutex);
    dict->ref_counter++;
    xmlMutexUnlock(&xmlDictMutex);
    return(0);
}

/**
 * Free the hash `dict` and its contents. The userdata is
 * deallocated with `f` if provided.
 *
 * @param dict  the dictionary
 */
void
xmlDictFree(xmlDict *dict) {
    xmlDictStringsPtr pool, nextp;

    if (dict == NULL)
	return;

    /* decrement the counter, it may be shared by a parser and docs */
    xmlMutexLock(&xmlDictMutex);
    dict->ref_counter--;
    if (dict->ref_counter > 0) {
        xmlMutexUnlock(&xmlDictMutex);
        return;
    }

    xmlMutexUnlock(&xmlDictMutex);

    if (dict->subdict != NULL) {
        xmlDictFree(dict->subdict);
    }

    if (dict->table) {
	xmlFree(dict->table);
    }
    pool = dict->strings;
    while (pool != NULL) {
        nextp = pool->next;
	xmlFree(pool);
	pool = nextp;
    }
    xmlFree(dict);
}

/**
 * check if a string is owned by the dictionary
 *
 * @param dict  the dictionary
 * @param str  the string
 * @returns 1 if true, 0 if false and -1 in case of error
 * -1 in case of error
 */
int
xmlDictOwns(xmlDict *dict, const xmlChar *str) {
    xmlDictStringsPtr pool;

    if ((dict == NULL) || (str == NULL))
	return(-1);
    pool = dict->strings;
    while (pool != NULL) {
        if ((str >= &pool->array[0]) && (str <= pool->free))
	    return(1);
	pool = pool->next;
    }
    if (dict->subdict)
        return(xmlDictOwns(dict->subdict, str));
    return(0);
}

/**
 * Query the number of elements installed in the hash `dict`.
 *
 * @param dict  the dictionary
 * @returns the number of elements in the dictionary or
 * -1 in case of error
 */
int
xmlDictSize(xmlDict *dict) {
    if (dict == NULL)
	return(-1);
    if (dict->subdict)
        return(dict->nbElems + dict->subdict->nbElems);
    return(dict->nbElems);
}

/**
 * Set a size limit for the dictionary
 * Added in 2.9.0
 *
 * @param dict  the dictionary
 * @param limit  the limit in bytes
 * @returns the previous limit of the dictionary or 0
 */
size_t
xmlDictSetLimit(xmlDict *dict, size_t limit) {
    size_t ret;

    if (dict == NULL)
	return(0);
    ret = dict->limit;
    dict->limit = limit;
    return(ret);
}

/**
 * Get how much memory is used by a dictionary for strings
 * Added in 2.9.0
 *
 * @param dict  the dictionary
 * @returns the amount of strings allocated
 */
size_t
xmlDictGetUsage(xmlDict *dict) {
    xmlDictStringsPtr pool;
    size_t limit = 0;

    if (dict == NULL)
	return(0);
    pool = dict->strings;
    while (pool != NULL) {
        limit += pool->size;
	pool = pool->next;
    }
    return(limit);
}

/*****************************************************************
 *
 * The code below was rewritten and is additionally licensed under
 * the main license in file 'Copyright'.
 *
 *****************************************************************/

ATTRIBUTE_NO_SANITIZE_INTEGER
static unsigned
xmlDictHashName(unsigned seed, const xmlChar* data, size_t maxLen,
                size_t *plen) {
    unsigned h1, h2;
    size_t i;

    HASH_INIT(h1, h2, seed);

    for (i = 0; i < maxLen && data[i]; i++) {
        HASH_UPDATE(h1, h2, data[i]);
    }

    HASH_FINISH(h1, h2);

    *plen = i;
    return(h2 | MAX_HASH_SIZE);
}

ATTRIBUTE_NO_SANITIZE_INTEGER
static unsigned
xmlDictHashQName(unsigned seed, const xmlChar *prefix, const xmlChar *name,
                 size_t *pplen, size_t *plen) {
    unsigned h1, h2;
    size_t i;

    HASH_INIT(h1, h2, seed);

    for (i = 0; prefix[i] != 0; i++) {
        HASH_UPDATE(h1, h2, prefix[i]);
    }
    *pplen = i;

    HASH_UPDATE(h1, h2, ':');

    for (i = 0; name[i] != 0; i++) {
        HASH_UPDATE(h1, h2, name[i]);
    }
    *plen = i;

    HASH_FINISH(h1, h2);

    /*
     * Always set the upper bit of hash values since 0 means an unoccupied
     * bucket.
     */
    return(h2 | MAX_HASH_SIZE);
}

/**
 * Compute the hash value of a C string.
 *
 * @param dict  dictionary
 * @param string  C string
 * @returns the hash value.
 */
unsigned
xmlDictComputeHash(const xmlDict *dict, const xmlChar *string) {
    size_t len;
    return(xmlDictHashName(dict->seed, string, SIZE_MAX, &len));
}

#define HASH_ROL31(x,n) ((x) << (n) | ((x) & 0x7FFFFFFF) >> (31 - (n)))

/**
 * Combine two hash values.
 *
 * @param v1  first hash value
 * @param v2  second hash value
 * @returns the combined hash value.
 */
ATTRIBUTE_NO_SANITIZE_INTEGER
unsigned
xmlDictCombineHash(unsigned v1, unsigned v2) {
    /*
     * The upper bit of hash values is always set, so we have to operate on
     * 31-bit hashes here.
     */
    v1 ^= v2;
    v1 += HASH_ROL31(v2, 5);

    return((v1 & 0xFFFFFFFF) | 0x80000000);
}

/**
 * Try to find a matching hash table entry. If an entry was found, set
 * `found` to 1 and return the entry. Otherwise, set `found` to 0 and return
 * the location where a new entry should be inserted.
 *
 * @param dict  dict
 * @param prefix  optional QName prefix
 * @param name  string
 * @param len  length of string
 * @param hashValue  valid hash value of string
 * @param pfound  result of search
 */
ATTRIBUTE_NO_SANITIZE_INTEGER
static xmlDictEntry *
xmlDictFindEntry(const xmlDict *dict, const xmlChar *prefix,
                 const xmlChar *name, int len, unsigned hashValue,
                 int *pfound) {
    xmlDictEntry *entry;
    unsigned mask, pos, displ;
    int found = 0;

    mask = dict->size - 1;
    pos = hashValue & mask;
    entry = &dict->table[pos];

    if (entry->hashValue != 0) {
        /*
         * Robin hood hashing: abort if the displacement of the entry
         * is smaller than the displacement of the key we look for.
         * This also stops at the correct position when inserting.
         */
        displ = 0;

        do {
            if (entry->hashValue == hashValue) {
                if (prefix == NULL) {
                    /*
                     * name is not necessarily null-terminated.
                     */
                    if ((strncmp((const char *) entry->name,
                                 (const char *) name, len) == 0) &&
                        (entry->name[len] == 0)) {
                        found = 1;
                        break;
                    }
                } else {
                    if (xmlStrQEqual(prefix, name, entry->name)) {
                        found = 1;
                        break;
                    }
                }
            }

            displ++;
            pos++;
            entry++;
            if ((pos & mask) == 0)
                entry = dict->table;
        } while ((entry->hashValue != 0) &&
                 (((pos - entry->hashValue) & mask) >= displ));
    }

    *pfound = found;
    return(entry);
}

/**
 * Resize the dictionary hash table.
 *
 * @param dict  dictionary
 * @param size  new size of the dictionary
 * @returns 0 in case of success, -1 if a memory allocation failed.
 */
static int
xmlDictGrow(xmlDictPtr dict, unsigned size) {
    const xmlDictEntry *oldentry, *oldend, *end;
    xmlDictEntry *table;
    unsigned oldsize, i;

    /* Add 0 to avoid spurious -Wtype-limits warning on 64-bit GCC */
    if ((size_t) size + 0 > SIZE_MAX / sizeof(table[0]))
        return(-1);
    table = xmlMalloc(size * sizeof(table[0]));
    if (table == NULL)
        return(-1);
    memset(table, 0, size * sizeof(table[0]));

    oldsize = dict->size;
    if (oldsize == 0)
        goto done;

    oldend = &dict->table[oldsize];
    end = &table[size];

    /*
     * Robin Hood sorting order is maintained if we
     *
     * - compute dict indices with modulo
     * - resize by an integer factor
     * - start to copy from the beginning of a probe sequence
     */
    oldentry = dict->table;
    while (oldentry->hashValue != 0) {
        if (++oldentry >= oldend)
            oldentry = dict->table;
    }

    for (i = 0; i < oldsize; i++) {
        if (oldentry->hashValue != 0) {
            xmlDictEntry *entry = &table[oldentry->hashValue & (size - 1)];

            while (entry->hashValue != 0) {
                if (++entry >= end)
                    entry = table;
            }
            *entry = *oldentry;
        }

        if (++oldentry >= oldend)
            oldentry = dict->table;
    }

    xmlFree(dict->table);

done:
    dict->table = table;
    dict->size = size;

    return(0);
}

/**
 * Internal lookup and update function.
 *
 * @param dict  dict
 * @param prefix  optional QName prefix
 * @param name  string
 * @param maybeLen  length of string or -1 if unknown
 * @param update  whether the string should be added
 */
ATTRIBUTE_NO_SANITIZE_INTEGER
static const xmlDictEntry *
xmlDictLookupInternal(xmlDict *dict, const xmlChar *prefix,
                      const xmlChar *name, int maybeLen, int update) {
    xmlDictEntry *entry = NULL;
    const xmlChar *ret;
    unsigned hashValue, newSize;
    size_t maxLen, len, plen, klen;
    int found = 0;

    if ((dict == NULL) || (name == NULL))
	return(NULL);

    maxLen = (maybeLen < 0) ? SIZE_MAX : (size_t) maybeLen;

    if (prefix == NULL) {
        hashValue = xmlDictHashName(dict->seed, name, maxLen, &len);
        if (len > INT_MAX / 2)
            return(NULL);
        klen = len;
    } else {
        hashValue = xmlDictHashQName(dict->seed, prefix, name, &plen, &len);
        if ((len > INT_MAX / 2) || (plen >= INT_MAX / 2 - len))
            return(NULL);
        klen = plen + 1 + len;
    }

    if ((dict->limit > 0) && (klen >= dict->limit))
        return(NULL);

    /*
     * Check for an existing entry
     */
    if (dict->size == 0) {
        newSize = MIN_HASH_SIZE;
    } else {
        entry = xmlDictFindEntry(dict, prefix, name, klen, hashValue, &found);
        if (found)
            return(entry);

        if (dict->nbElems + 1 > dict->size / MAX_FILL_DENOM * MAX_FILL_NUM) {
            if (dict->size >= MAX_HASH_SIZE)
                return(NULL);
            newSize = dict->size * 2;
        } else {
            newSize = 0;
        }
    }

    if ((dict->subdict != NULL) && (dict->subdict->size > 0)) {
        xmlDictEntry *subEntry;
        unsigned subHashValue;

        if (prefix == NULL)
            subHashValue = xmlDictHashName(dict->subdict->seed, name, len,
                                           &len);
        else
            subHashValue = xmlDictHashQName(dict->subdict->seed, prefix, name,
                                            &plen, &len);
        subEntry = xmlDictFindEntry(dict->subdict, prefix, name, klen,
                                    subHashValue, &found);
        if (found)
            return(subEntry);
    }

    if (!update)
        return(NULL);

    /*
     * Grow the hash table if needed
     */
    if (newSize > 0) {
        unsigned mask, displ, pos;

        if (xmlDictGrow(dict, newSize) != 0)
            return(NULL);

        /*
         * Find new entry
         */
        mask = dict->size - 1;
        displ = 0;
        pos = hashValue & mask;
        entry = &dict->table[pos];

        while ((entry->hashValue != 0) &&
               ((pos - entry->hashValue) & mask) >= displ) {
            displ++;
            pos++;
            entry++;
            if ((pos & mask) == 0)
                entry = dict->table;
        }
    }

    if (prefix == NULL)
        ret = xmlDictAddString(dict, name, len);
    else
        ret = xmlDictAddQString(dict, prefix, plen, name, len);
    if (ret == NULL)
        return(NULL);

    /*
     * Shift the remainder of the probe sequence to the right
     */
    if (entry->hashValue != 0) {
        const xmlDictEntry *end = &dict->table[dict->size];
        const xmlDictEntry *cur = entry;

        do {
            cur++;
            if (cur >= end)
                cur = dict->table;
        } while (cur->hashValue != 0);

        if (cur < entry) {
            /*
             * If we traversed the end of the buffer, handle the part
             * at the start of the buffer.
             */
            memmove(&dict->table[1], dict->table,
                    (char *) cur - (char *) dict->table);
            cur = end - 1;
            dict->table[0] = *cur;
        }

        memmove(&entry[1], entry, (char *) cur - (char *) entry);
    }

    /*
     * Populate entry
     */
    entry->hashValue = hashValue;
    entry->name = ret;

    dict->nbElems++;

    return(entry);
}

/**
 * Lookup a string and add it to the dictionary if it wasn't found.
 *
 * @param dict  dictionary
 * @param name  string key
 * @param len  length of the key, if -1 it is recomputed
 * @returns the interned copy of the string or NULL if a memory allocation
 * failed.
 */
const xmlChar *
xmlDictLookup(xmlDict *dict, const xmlChar *name, int len) {
    const xmlDictEntry *entry;

    entry = xmlDictLookupInternal(dict, NULL, name, len, 1);
    if (entry == NULL)
        return(NULL);
    return(entry->name);
}

/**
 * Lookup a dictionary entry and add the string to the dictionary if
 * it wasn't found.
 *
 * @param dict  dictionary
 * @param name  string key
 * @param len  length of the key, if -1 it is recomputed
 * @returns the dictionary entry.
 */
xmlHashedString
xmlDictLookupHashed(xmlDict *dict, const xmlChar *name, int len) {
    const xmlDictEntry *entry;
    xmlHashedString ret;

    entry = xmlDictLookupInternal(dict, NULL, name, len, 1);

    if (entry == NULL) {
        ret.name = NULL;
        ret.hashValue = 0;
    } else {
        ret = *entry;
    }

    return(ret);
}

/**
 * Check if a string exists in the dictionary.
 *
 * @param dict  the dictionary
 * @param name  the name of the userdata
 * @param len  the length of the name, if -1 it is recomputed
 * @returns the internal copy of the name or NULL if not found.
 */
const xmlChar *
xmlDictExists(xmlDict *dict, const xmlChar *name, int len) {
    const xmlDictEntry *entry;

    entry = xmlDictLookupInternal(dict, NULL, name, len, 0);
    if (entry == NULL)
        return(NULL);
    return(entry->name);
}

/**
 * Lookup the QName `prefix:name` and add it to the dictionary if
 * it wasn't found.
 *
 * @param dict  the dictionary
 * @param prefix  the prefix
 * @param name  the name
 * @returns the interned copy of the string or NULL if a memory allocation
 * failed.
 */
const xmlChar *
xmlDictQLookup(xmlDict *dict, const xmlChar *prefix, const xmlChar *name) {
    const xmlDictEntry *entry;

    entry = xmlDictLookupInternal(dict, prefix, name, -1, 1);
    if (entry == NULL)
        return(NULL);
    return(entry->name);
}

/*
 * Pseudo-random generator
 */

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <bcrypt.h>
#else
  #if HAVE_DECL_GETENTROPY
    /* POSIX 2024 */
    #include <unistd.h>
    /* Older platforms */
    #include <sys/random.h>
  #endif
  #include <time.h>
#endif

static xmlMutex xmlRngMutex;

static unsigned globalRngState[2];

/*
 *
 * Initialize the PRNG.
 */
ATTRIBUTE_NO_SANITIZE_INTEGER
void
xmlInitRandom(void) {
    xmlInitMutex(&xmlRngMutex);

    {
#ifdef _WIN32
        NTSTATUS status;

        /*
         * You can find many (recent as of 2025) discussions how
         * to get a pseudo-random seed on Windows in projects like
         * Golang, Rust, Chromium and Firefox.
         *
         * TODO: Support ProcessPrng available since Windows 10.
         */
        status = BCryptGenRandom(NULL, (unsigned char *) globalRngState,
                                 sizeof(globalRngState),
                                 BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        if (!BCRYPT_SUCCESS(status))
            xmlAbort("libxml2: BCryptGenRandom failed with error code %lu\n",
                     GetLastError());
#else
        int var;

#if HAVE_DECL_GETENTROPY
        while (1) {
            if (getentropy(globalRngState, sizeof(globalRngState)) == 0)
                return;

            /*
             * This most likely means that libxml2 was compiled on
             * a system supporting certain system calls and is running
             * on a system that doesn't support these calls, as can
             * be the case on Linux.
             */
            if (errno == ENOSYS)
                break;

            /*
             * We really don't want to fallback to the unsafe PRNG
             * for possibly accidental reasons, so we abort on any
             * unknown error.
             */
            if (errno != EINTR)
                xmlAbort("libxml2: getentropy failed with error code %d\n",
                         errno);
        }
#endif

        /*
         * TODO: Fallback to /dev/urandom for older POSIX systems.
         */
        globalRngState[0] =
                (unsigned) time(NULL) ^
                HASH_ROL((unsigned) ((size_t) &xmlInitRandom & 0xFFFFFFFF), 8);
        globalRngState[1] =
                HASH_ROL((unsigned) ((size_t) &xmlRngMutex & 0xFFFFFFFF), 16) ^
                HASH_ROL((unsigned) ((size_t) &var & 0xFFFFFFFF), 24);
#endif
    }
}

/*
 *
 * Clean up PRNG globals.
 */
void
xmlCleanupRandom(void) {
    xmlCleanupMutex(&xmlRngMutex);
}

ATTRIBUTE_NO_SANITIZE_INTEGER
static unsigned
xoroshiro64ss(unsigned *s) {
    unsigned s0 = s[0];
    unsigned s1 = s[1];
    unsigned result = HASH_ROL(s0 * 0x9E3779BB, 5) * 5;

    s1 ^= s0;
    s[0] = HASH_ROL(s0, 26) ^ s1 ^ (s1 << 9);
    s[1] = HASH_ROL(s1, 13);

    return(result & 0xFFFFFFFF);
}

/*
 *
 * Generate a pseudo-random value using the global PRNG.
 *
 * @returns a random value.
 */
unsigned
xmlGlobalRandom(void) {
    unsigned ret;

    xmlMutexLock(&xmlRngMutex);
    ret = xoroshiro64ss(globalRngState);
    xmlMutexUnlock(&xmlRngMutex);

    return(ret);
}

/*
 *
 * Generate a pseudo-random value using the thread-local PRNG.
 *
 * @returns a random value.
 */
unsigned
xmlRandom(void) {
    return(xoroshiro64ss(xmlGetLocalRngState()));
}


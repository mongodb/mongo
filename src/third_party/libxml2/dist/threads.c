/**
 * threads.c: set of generic threading related routines
 *
 * See Copyright for the status of this software.
 *
 * Author: Gary Pennington, Daniel Veillard
 */

#define IN_LIBXML
#include "libxml.h"

#include <string.h>
#include <stdarg.h>
#include <stdlib.h>

#include <libxml/threads.h>
#include <libxml/parser.h>
#ifdef LIBXML_CATALOG_ENABLED
#include <libxml/catalog.h>
#endif
#ifdef LIBXML_RELAXNG_ENABLED
#include <libxml/relaxng.h>
#endif
#ifdef LIBXML_SCHEMAS_ENABLED
#include <libxml/xmlschemastypes.h>
#endif

#if defined(SOLARIS)
#include <note.h>
#endif

#include "private/cata.h"
#include "private/dict.h"
#include "private/enc.h"
#include "private/error.h"
#include "private/globals.h"
#include "private/io.h"
#include "private/memory.h"
#include "private/threads.h"
#include "private/xpath.h"

/*
 * TODO: this module still uses malloc/free and not xmlMalloc/xmlFree
 *       to avoid some craziness since xmlMalloc/xmlFree may actually
 *       be hosted on allocated blocks needing them for the allocation ...
 */

static xmlRMutex xmlLibraryLock;

/**
 * Initialize a mutex.
 *
 * @param mutex  the mutex
 */
void
xmlInitMutex(xmlMutex *mutex)
{
#ifdef HAVE_POSIX_THREADS
    pthread_mutex_init(&mutex->lock, NULL);
#elif defined HAVE_WIN32_THREADS
    InitializeCriticalSection(&mutex->cs);
#else
    (void) mutex;
#endif
}

/**
 * #xmlNewMutex is used to allocate a libxml2 token struct for use in
 * synchronizing access to data.
 *
 * @returns a new simple mutex pointer or NULL in case of error
 */
xmlMutex *
xmlNewMutex(void)
{
    xmlMutexPtr tok;

    tok = malloc(sizeof(xmlMutex));
    if (tok == NULL)
        return (NULL);
    xmlInitMutex(tok);
    return (tok);
}

/**
 * Reclaim resources associated with a mutex.
 *
 * @param mutex  the simple mutex
 */
void
xmlCleanupMutex(xmlMutex *mutex)
{
#ifdef HAVE_POSIX_THREADS
    pthread_mutex_destroy(&mutex->lock);
#elif defined HAVE_WIN32_THREADS
    DeleteCriticalSection(&mutex->cs);
#else
    (void) mutex;
#endif
}

/**
 * Free a mutex.
 *
 * @param tok  the simple mutex
 */
void
xmlFreeMutex(xmlMutex *tok)
{
    if (tok == NULL)
        return;

    xmlCleanupMutex(tok);
    free(tok);
}

/**
 * #xmlMutexLock is used to lock a libxml2 token.
 *
 * @param tok  the simple mutex
 */
void
xmlMutexLock(xmlMutex *tok)
{
    if (tok == NULL)
        return;
#ifdef HAVE_POSIX_THREADS
    /*
     * This assumes that __libc_single_threaded won't change while the
     * lock is held.
     */
    pthread_mutex_lock(&tok->lock);
#elif defined HAVE_WIN32_THREADS
    EnterCriticalSection(&tok->cs);
#endif

}

/**
 * #xmlMutexUnlock is used to unlock a libxml2 token.
 *
 * @param tok  the simple mutex
 */
void
xmlMutexUnlock(xmlMutex *tok)
{
    if (tok == NULL)
        return;
#ifdef HAVE_POSIX_THREADS
    pthread_mutex_unlock(&tok->lock);
#elif defined HAVE_WIN32_THREADS
    LeaveCriticalSection(&tok->cs);
#endif
}

/**
 * Initialize the mutex.
 *
 * @param tok  mutex
 */
void
xmlInitRMutex(xmlRMutex *tok) {
    (void) tok;

#ifdef HAVE_POSIX_THREADS
    pthread_mutex_init(&tok->lock, NULL);
    tok->held = 0;
    tok->waiters = 0;
    pthread_cond_init(&tok->cv, NULL);
#elif defined HAVE_WIN32_THREADS
    InitializeCriticalSection(&tok->cs);
#endif
}

/**
 * Used to allocate a reentrant mutex for use in
 * synchronizing access to data. token_r is a re-entrant lock and thus useful
 * for synchronizing access to data structures that may be manipulated in a
 * recursive fashion.
 *
 * @returns the new reentrant mutex pointer or NULL in case of error
 */
xmlRMutex *
xmlNewRMutex(void)
{
    xmlRMutexPtr tok;

    tok = malloc(sizeof(xmlRMutex));
    if (tok == NULL)
        return (NULL);
    xmlInitRMutex(tok);
    return (tok);
}

/**
 * Cleanup the mutex.
 *
 * @param tok  mutex
 */
void
xmlCleanupRMutex(xmlRMutex *tok) {
    (void) tok;

#ifdef HAVE_POSIX_THREADS
    pthread_mutex_destroy(&tok->lock);
    pthread_cond_destroy(&tok->cv);
#elif defined HAVE_WIN32_THREADS
    DeleteCriticalSection(&tok->cs);
#endif
}

/**
 * Used to reclaim resources associated with a
 * reentrant mutex.
 *
 * @param tok  the reentrant mutex
 */
void
xmlFreeRMutex(xmlRMutex *tok)
{
    if (tok == NULL)
        return;
    xmlCleanupRMutex(tok);
    free(tok);
}

/**
 * #xmlRMutexLock is used to lock a libxml2 token_r.
 *
 * @param tok  the reentrant mutex
 */
void
xmlRMutexLock(xmlRMutex *tok)
{
    if (tok == NULL)
        return;
#ifdef HAVE_POSIX_THREADS
    pthread_mutex_lock(&tok->lock);
    if (tok->held) {
        if (pthread_equal(tok->tid, pthread_self())) {
            tok->held++;
            pthread_mutex_unlock(&tok->lock);
            return;
        } else {
            tok->waiters++;
            while (tok->held)
                pthread_cond_wait(&tok->cv, &tok->lock);
            tok->waiters--;
        }
    }
    tok->tid = pthread_self();
    tok->held = 1;
    pthread_mutex_unlock(&tok->lock);
#elif defined HAVE_WIN32_THREADS
    EnterCriticalSection(&tok->cs);
#endif
}

/**
 * #xmlRMutexUnlock is used to unlock a libxml2 token_r.
 *
 * @param tok  the reentrant mutex
 */
void
xmlRMutexUnlock(xmlRMutex *tok ATTRIBUTE_UNUSED)
{
    if (tok == NULL)
        return;
#ifdef HAVE_POSIX_THREADS
    pthread_mutex_lock(&tok->lock);
    tok->held--;
    if (tok->held == 0) {
        if (tok->waiters)
            pthread_cond_signal(&tok->cv);
        memset(&tok->tid, 0, sizeof(tok->tid));
    }
    pthread_mutex_unlock(&tok->lock);
#elif defined HAVE_WIN32_THREADS
    LeaveCriticalSection(&tok->cs);
#endif
}

/************************************************************************
 *									*
 *			Library wide thread interfaces			*
 *									*
 ************************************************************************/

/**
 * #xmlLockLibrary is used to take out a re-entrant lock on the libxml2
 * library.
 */
void
xmlLockLibrary(void)
{
    xmlRMutexLock(&xmlLibraryLock);
}

/**
 * #xmlUnlockLibrary is used to release a re-entrant lock on the libxml2
 * library.
 */
void
xmlUnlockLibrary(void)
{
    xmlRMutexUnlock(&xmlLibraryLock);
}

/**
 * @deprecated Alias for #xmlInitParser.
 */
void
xmlInitThreads(void)
{
    xmlInitParser();
}

/**
 * @deprecated This function is a no-op. Call #xmlCleanupParser
 * to free global state but see the warnings there. #xmlCleanupParser
 * should be only called once at program exit. In most cases, you don't
 * have call cleanup functions at all.
 */
void
xmlCleanupThreads(void)
{
}

static void
xmlInitThreadsInternal(void) {
    xmlInitRMutex(&xmlLibraryLock);
}

static void
xmlCleanupThreadsInternal(void) {
    xmlCleanupRMutex(&xmlLibraryLock);
}

/************************************************************************
 *									*
 *			Library wide initialization			*
 *									*
 ************************************************************************/

static int xmlParserInitialized = 0;

#ifdef HAVE_POSIX_THREADS
static pthread_once_t onceControl = PTHREAD_ONCE_INIT;
#elif defined HAVE_WIN32_THREADS
static INIT_ONCE onceControl = INIT_ONCE_STATIC_INIT;
#else
static int onceControl = 0;
#endif

static void
xmlInitParserInternal(void) {
    /*
     * Note that the initialization code must not make memory allocations.
     */
    xmlInitRandom(); /* Required by xmlInitGlobalsInternal */
    xmlInitMemoryInternal();
    xmlInitThreadsInternal();
    xmlInitGlobalsInternal();
    xmlInitDictInternal();
    xmlInitEncodingInternal();
#if defined(LIBXML_XPATH_ENABLED)
    xmlInitXPathInternal();
#endif
    xmlInitIOCallbacks();
#ifdef LIBXML_CATALOG_ENABLED
    xmlInitCatalogInternal();
#endif
#ifdef LIBXML_SCHEMAS_ENABLED
    xmlInitSchemasTypesInternal();
#endif
#ifdef LIBXML_RELAXNG_ENABLED
    xmlInitRelaxNGInternal();
#endif

    xmlParserInitialized = 1;
}

#if defined(HAVE_WIN32_THREADS)
static BOOL WINAPI
xmlInitParserWinWrapper(INIT_ONCE *initOnce ATTRIBUTE_UNUSED,
                        void *parameter ATTRIBUTE_UNUSED,
                        void **context ATTRIBUTE_UNUSED) {
    xmlInitParserInternal();
    return(TRUE);
}
#endif

/**
 * Initialization function for the XML parser.
 *
 * For older versions, it's recommended to call this function once
 * from the main thread before using the library in multithreaded
 * programs.
 *
 * Since 2.14.0, there's no distinction between threads. It should
 * be unnecessary to call this function.
 */
void
xmlInitParser(void) {
#ifdef HAVE_POSIX_THREADS
    pthread_once(&onceControl, xmlInitParserInternal);
#elif defined(HAVE_WIN32_THREADS)
    InitOnceExecuteOnce(&onceControl, xmlInitParserWinWrapper, NULL, NULL);
#else
    if (onceControl == 0) {
        xmlInitParserInternal();
        onceControl = 1;
    }
#endif
}

/**
 * Free global memory allocations.
 *
 * This function is named somewhat misleadingly. It does not clean up
 * parser state but frees global memory allocated by various components
 * of the library.
 *
 * Since 2.9.11, cleanup is performed automatically on most platforms
 * and there's no need at all for manual cleanup. This includes all
 * compilers and platforms that support GCC-style destructor attributes
 * as well as Windows DLLs.
 *
 * This function should only be used to avoid false positives from
 * memory leak checkers if automatic cleanup isn't possible, for
 * example with static builds on MSVC.
 *
 * WARNING: xmlCleanupParser is not thread-safe. If this function is
 * called and any threads that could make calls into libxml2 are
 * still running, memory corruption is likely to occur.
 *
 * No library calls must be made (from any thread) after calling this
 * function. In general, *this function should only be called right
 * before the whole process exits.* Calling this function too early
 * will lead to memory corruption.
 */
void
xmlCleanupParser(void) {
    /*
     * Unfortunately, some users call this function to fix memory
     * leaks on unload with versions before 2.9.11. This can result
     * in the library being reinitialized, so this use case must
     * be supported.
     */
    if (!xmlParserInitialized)
        return;

    xmlCleanupCharEncodingHandlers();
#ifdef LIBXML_CATALOG_ENABLED
    xmlCatalogCleanup();
    xmlCleanupCatalogInternal();
#endif
#ifdef LIBXML_SCHEMAS_ENABLED
    xmlSchemaCleanupTypes();
#endif
#ifdef LIBXML_RELAXNG_ENABLED
    xmlRelaxNGCleanupTypes();
#endif

#ifdef LIBXML_SCHEMAS_ENABLED
    /* Must be after xmlRelaxNGCleanupTypes */
    xmlCleanupSchemasTypesInternal();
#endif
#ifdef LIBXML_RELAXNG_ENABLED
    xmlCleanupRelaxNGInternal();
#endif

    xmlCleanupDictInternal();
    xmlCleanupRandom();
    xmlCleanupGlobalsInternal();
    xmlCleanupThreadsInternal();

    /*
     * Must come after all cleanup functions that call xmlFree which
     * uses xmlMemMutex in debug mode.
     */
    xmlCleanupMemoryInternal();

    xmlParserInitialized = 0;

    /*
     * This is a bit sketchy but should make reinitialization work.
     */
#ifdef HAVE_POSIX_THREADS
    {
        pthread_once_t tmp = PTHREAD_ONCE_INIT;
        memcpy(&onceControl, &tmp, sizeof(tmp));
    }
#elif defined(HAVE_WIN32_THREADS)
    {
        INIT_ONCE tmp = INIT_ONCE_STATIC_INIT;
        memcpy(&onceControl, &tmp, sizeof(tmp));
    }
#else
    onceControl = 0;
#endif
}

#if defined(HAVE_FUNC_ATTRIBUTE_DESTRUCTOR) && \
    !defined(LIBXML_THREAD_ALLOC_ENABLED) && \
    !defined(LIBXML_STATIC) && \
    !defined(_WIN32)
static void
ATTRIBUTE_DESTRUCTOR
xmlDestructor(void) {
    /*
     * Calling custom deallocation functions in a destructor can cause
     * problems, for example with Nokogiri.
     */
    if (xmlFree == free)
        xmlCleanupParser();
}
#endif

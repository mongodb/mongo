/*
 * globals.c: definition and handling of the set of global variables
 *            of the library
 *
 * See Copyright for the status of this software.
 */

#define IN_LIBXML
#include "libxml.h"

#include <stdlib.h>
#include <string.h>

#define XML_GLOBALS_NO_REDEFINITION
#include <libxml/xmlerror.h>
#include <libxml/xmlmemory.h>
#include <libxml/xmlIO.h>
#include <libxml/HTMLparser.h>
#include <libxml/parser.h>
#include <libxml/threads.h>
#include <libxml/tree.h>
#include <libxml/xmlsave.h>
#include <libxml/SAX2.h>

#include "private/dict.h"
#include "private/error.h"
#include "private/globals.h"
#include "private/threads.h"
#include "private/tree.h"

/*
 * Mutex to protect "ForNewThreads" variables
 */
static xmlMutex xmlThrDefMutex;

/*
 * Deprecated global setting which is unused since 2.15.0
 */
static int lineNumbersDefaultValue = 1;

/*
 * Thread-local storage emulation.
 *
 * This works by replacing a global variable
 *
 *     extern xmlError xmlLastError;
 *
 * with a macro that calls a function returning a pointer to the global in
 * thread-local storage:
 *
 *     xmlError *__xmlLastError(void);
 *     #define xmlError (*__xmlLastError());
 *
 * The code can operate in a multitude of ways depending on the environment.
 * First we support POSIX and Windows threads. Then we support both
 * thread-local storage provided by the compiler and older methods like
 * thread-specific data (pthreads) or TlsAlloc (Windows).
 *
 * To clean up thread-local storage, we use thread-specific data on POSIX.
 * On Windows, we either use DllMain when compiling a DLL or a registered
 * wait function for static builds.
 *
 * Compiler TLS isn't really useful for now. It can make allocation more
 * robust on some platforms but it also increases the memory consumption
 * of each thread by ~250 bytes whether it uses libxml2 or not. The main
 * problem is that we have to deallocate strings in xmlLastError and C
 * offers no simple way to deallocate dynamic data in _Thread_local
 * variables. In C++, one could simply use a thread_local variable with a
 * destructor.
 *
 * At some point, many of the deprecated globals can be removed,
 * although things like global error handlers will take a while.
 * Ultimately, the only crucial things seem to be xmlLastError and
 * RNG state. xmlLastError already involves dynamic allocation, so it
 * could be allocated dynamically as well, only storing a global
 * pointer.
 */

#ifdef LIBXML_THREAD_ENABLED

#ifdef HAVE_WIN32_THREADS
  #if defined(LIBXML_STATIC) && !defined(LIBXML_STATIC_FOR_DLL)
    #define USE_WAIT_DTOR
  #else
    #define USE_DLL_MAIN
  #endif
#endif

/*
 * On Darwin, thread-local storage destructors seem to be run before
 * pthread thread-specific data destructors. This causes ASan to
 * report a use-after-free.
 *
 * On Windows, we can't use TLS in static builds. The RegisterWait
 * callback would run after TLS was deallocated.
 */
#if defined(XML_THREAD_LOCAL) && \
    !defined(__APPLE__) && \
    !defined(USE_WAIT_DTOR)
#define USE_TLS
#endif

#ifdef HAVE_POSIX_THREADS

/*
 * On POSIX, we need thread-specific data even with thread-local storage
 * to destroy indirect references from global state (xmlLastError) at
 * thread exit.
 */
static pthread_key_t globalkey;

#elif defined HAVE_WIN32_THREADS

#ifndef USE_TLS
static DWORD globalkey = TLS_OUT_OF_INDEXES;
#endif

#endif /* HAVE_WIN32_THREADS */

static void
xmlFreeGlobalState(void *state);

#endif /* LIBXML_THREAD_ENABLED */

struct _xmlGlobalState {
#ifdef USE_TLS
    int initialized;
#endif

#ifdef USE_WAIT_DTOR
    void *threadHandle;
    void *waitHandle;
#endif

    unsigned localRngState[2];

    xmlError lastError;

#ifdef LIBXML_THREAD_ALLOC_ENABLED
    xmlMallocFunc malloc;
    xmlMallocFunc mallocAtomic;
    xmlReallocFunc realloc;
    xmlFreeFunc free;
    xmlStrdupFunc memStrdup;
#endif

    int doValidityCheckingDefaultValue;
    int getWarningsDefaultValue;
    int keepBlanksDefaultValue;
    int loadExtDtdDefaultValue;
    int pedanticParserDefaultValue;
    int substituteEntitiesDefaultValue;

#ifdef LIBXML_OUTPUT_ENABLED
    int indentTreeOutput;
    const char *treeIndentString;
    int saveNoEmptyTags;
#endif

    xmlGenericErrorFunc genericError;
    void *genericErrorContext;
    xmlStructuredErrorFunc structuredError;
    void *structuredErrorContext;

    xmlRegisterNodeFunc registerNodeDefaultValue;
    xmlDeregisterNodeFunc deregisterNodeDefaultValue;

    xmlParserInputBufferCreateFilenameFunc parserInputBufferCreateFilenameValue;
    xmlOutputBufferCreateFilenameFunc outputBufferCreateFilenameValue;
};

typedef struct _xmlGlobalState xmlGlobalState;
typedef xmlGlobalState *xmlGlobalStatePtr;

#ifdef LIBXML_THREAD_ENABLED

#ifdef USE_TLS
static XML_THREAD_LOCAL xmlGlobalState globalState;
#endif

#else /* LIBXML_THREAD_ENABLED */

static xmlGlobalState globalState;

#endif /* LIBXML_THREAD_ENABLED */

/************************************************************************
 *									*
 *	All the user accessible global variables of the library		*
 *									*
 ************************************************************************/

/**
 * a strdup implementation with a type signature matching POSIX
 *
 * @param cur  the input char *
 * @returns a new xmlChar * or NULL
 */
static char *
xmlPosixStrdup(const char *cur) {
    return((char*) xmlCharStrdup(cur));
}

/*
 * Memory allocation routines
 */

xmlFreeFunc xmlFree = free;
xmlMallocFunc xmlMalloc = malloc;
xmlMallocFunc xmlMallocAtomic = malloc;
xmlReallocFunc xmlRealloc = realloc;
xmlStrdupFunc xmlMemStrdup = xmlPosixStrdup;

/*
 * Parser defaults
 */

static int xmlDoValidityCheckingDefaultValueThrDef = 0;
static int xmlGetWarningsDefaultValueThrDef = 1;
static int xmlLoadExtDtdDefaultValueThrDef = 0;
static int xmlPedanticParserDefaultValueThrDef = 0;
static int xmlKeepBlanksDefaultValueThrDef = 1;
static int xmlSubstituteEntitiesDefaultValueThrDef = 0;

static xmlRegisterNodeFunc xmlRegisterNodeDefaultValueThrDef = NULL;
static xmlDeregisterNodeFunc xmlDeregisterNodeDefaultValueThrDef = NULL;

static xmlParserInputBufferCreateFilenameFunc
xmlParserInputBufferCreateFilenameValueThrDef = NULL;
static xmlOutputBufferCreateFilenameFunc
xmlOutputBufferCreateFilenameValueThrDef = NULL;

static xmlGenericErrorFunc xmlGenericErrorThrDef = xmlGenericErrorDefaultFunc;
static xmlStructuredErrorFunc xmlStructuredErrorThrDef = NULL;
static void *xmlGenericErrorContextThrDef = NULL;
static void *xmlStructuredErrorContextThrDef = NULL;

#ifdef LIBXML_OUTPUT_ENABLED
static int xmlIndentTreeOutputThrDef = 1;
static const char *xmlTreeIndentStringThrDef = "  ";
static int xmlSaveNoEmptyTagsThrDef = 0;
#endif /* LIBXML_OUTPUT_ENABLED */

#ifdef LIBXML_SAX1_ENABLED
/**
 * Default SAX version1 handler for XML, builds the DOM tree
 *
 * @deprecated This handler is unused and will be removed from future
 * versions.
 *
 */
const xmlSAXHandlerV1 xmlDefaultSAXHandler = {
    xmlSAX2InternalSubset,
    xmlSAX2IsStandalone,
    xmlSAX2HasInternalSubset,
    xmlSAX2HasExternalSubset,
    xmlSAX2ResolveEntity,
    xmlSAX2GetEntity,
    xmlSAX2EntityDecl,
    xmlSAX2NotationDecl,
    xmlSAX2AttributeDecl,
    xmlSAX2ElementDecl,
    xmlSAX2UnparsedEntityDecl,
    xmlSAX2SetDocumentLocator,
    xmlSAX2StartDocument,
    xmlSAX2EndDocument,
    xmlSAX2StartElement,
    xmlSAX2EndElement,
    xmlSAX2Reference,
    xmlSAX2Characters,
    xmlSAX2Characters,
    xmlSAX2ProcessingInstruction,
    xmlSAX2Comment,
    xmlParserWarning,
    xmlParserError,
    xmlParserError,
    xmlSAX2GetParameterEntity,
    xmlSAX2CDataBlock,
    xmlSAX2ExternalSubset,
    1,
};
#endif /* LIBXML_SAX1_ENABLED */

/**
 * The default SAX Locator
 * { getPublicId, getSystemId, getLineNumber, getColumnNumber}
 *
 * @deprecated Don't use
 *
 */
const xmlSAXLocator xmlDefaultSAXLocator = {
    xmlSAX2GetPublicId,
    xmlSAX2GetSystemId,
    xmlSAX2GetLineNumber,
    xmlSAX2GetColumnNumber
};

#if defined(LIBXML_HTML_ENABLED) && defined(LIBXML_SAX1_ENABLED)
/**
 * Default old SAX v1 handler for HTML, builds the DOM tree
 *
 * @deprecated This handler is unused and will be removed from future
 * versions.
 *
 */
const xmlSAXHandlerV1 htmlDefaultSAXHandler = {
    xmlSAX2InternalSubset,
    NULL,
    NULL,
    NULL,
    NULL,
    xmlSAX2GetEntity,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    xmlSAX2SetDocumentLocator,
    xmlSAX2StartDocument,
    xmlSAX2EndDocument,
    xmlSAX2StartElement,
    xmlSAX2EndElement,
    NULL,
    xmlSAX2Characters,
    xmlSAX2IgnorableWhitespace,
    xmlSAX2ProcessingInstruction,
    xmlSAX2Comment,
    xmlParserWarning,
    xmlParserError,
    xmlParserError,
    NULL,
    xmlSAX2CDataBlock,
    NULL,
    1,
};
#endif /* LIBXML_HTML_ENABLED */

static void
xmlInitGlobalState(xmlGlobalStatePtr gs);

/************************************************************************
 *									*
 *			Per thread global state handling		*
 *									*
 ************************************************************************/

/**
 * @deprecated Alias for #xmlInitParser.
 */
void xmlInitGlobals(void) {
    xmlInitParser();
}

/**
 * Additional initialisation for multi-threading
 */
void xmlInitGlobalsInternal(void) {
    xmlInitMutex(&xmlThrDefMutex);

#ifdef HAVE_POSIX_THREADS
    pthread_key_create(&globalkey, xmlFreeGlobalState);
#elif defined(HAVE_WIN32_THREADS)
#ifndef USE_TLS
    if (globalkey == TLS_OUT_OF_INDEXES)
        globalkey = TlsAlloc();
#endif
#else /* no thread support */
    xmlInitGlobalState(&globalState);
#endif
}

/**
 * @deprecated This function is a no-op. Call #xmlCleanupParser
 * to free global state but see the warnings there. #xmlCleanupParser
 * should be only called once at program exit. In most cases, you don't
 * have call cleanup functions at all.
 */
void xmlCleanupGlobals(void) {
}

/**
 * Additional cleanup for multi-threading
 */
void xmlCleanupGlobalsInternal(void) {
    /*
     * We assume that all other threads using the library have
     * terminated and the last remaining thread calls
     * xmlCleanupParser.
     */

#ifdef HAVE_POSIX_THREADS
    /*
     * Free thread-specific data of last thread before calling
     * pthread_key_delete.
     */
    xmlGlobalState *gs = pthread_getspecific(globalkey);
    if (gs != NULL)
        xmlFreeGlobalState(gs);
    pthread_key_delete(globalkey);
#elif defined(HAVE_WIN32_THREADS)
#if defined(USE_WAIT_DTOR) && !defined(USE_TLS)
    if (globalkey != TLS_OUT_OF_INDEXES) {
        TlsFree(globalkey);
        globalkey = TLS_OUT_OF_INDEXES;
    }
#endif
#else /* no thread support */
    xmlResetError(&globalState.lastError);
#endif

    xmlCleanupMutex(&xmlThrDefMutex);
}

#ifdef LIBXML_THREAD_ENABLED

static void
xmlFreeGlobalState(void *state)
{
    xmlGlobalState *gs = (xmlGlobalState *) state;

    /*
     * Free any memory allocated in the thread's error struct. If it
     * weren't for this indirect allocation, we wouldn't need
     * a destructor with thread-local storage at all!
     */
    xmlResetError(&gs->lastError);
#ifndef USE_TLS
    free(state);
#endif
}

#if defined(USE_WAIT_DTOR)
static void WINAPI
xmlGlobalStateDtor(void *ctxt, unsigned char timedOut ATTRIBUTE_UNUSED) {
    xmlGlobalStatePtr gs = ctxt;

    UnregisterWait(gs->waitHandle);
    CloseHandle(gs->threadHandle);
    xmlFreeGlobalState(gs);
}

static int
xmlRegisterGlobalStateDtor(xmlGlobalState *gs) {
    void *processHandle = GetCurrentProcess();
    void *threadHandle;
    void *waitHandle;

    if (DuplicateHandle(processHandle, GetCurrentThread(), processHandle,
                &threadHandle, 0, FALSE, DUPLICATE_SAME_ACCESS) == 0) {
        return(-1);
    }

    if (RegisterWaitForSingleObject(&waitHandle, threadHandle,
                xmlGlobalStateDtor, gs, INFINITE, WT_EXECUTEONLYONCE) == 0) {
        CloseHandle(threadHandle);
        return(-1);
    }

    gs->threadHandle = threadHandle;
    gs->waitHandle = waitHandle;
    return(0);
}
#endif /* USE_WAIT_DTOR */

#ifndef USE_TLS
/**
 * Allocates a global state. This structure is used to
 * hold all data for use by a thread when supporting backwards compatibility
 * of libxml2 to pre-thread-safe behaviour.
 *
 * @returns the newly allocated xmlGlobalState or NULL in case of error
 */
static xmlGlobalStatePtr
xmlNewGlobalState(int allowFailure)
{
    xmlGlobalState *gs;

    /*
     * We use malloc/free to allow accessing globals before setting
     * custom memory allocators.
     */
    gs = malloc(sizeof(xmlGlobalState));
    if (gs == NULL) {
        if (allowFailure)
            return(NULL);

        /*
         * If an application didn't call xmlCheckThreadLocalStorage to make
         * sure that global state could be allocated, it's too late to
         * handle the error.
         */
        xmlAbort("libxml2: Failed to allocate globals for thread\n"
                 "libxml2: See xmlCheckThreadLocalStorage\n");
    }

    memset(gs, 0, sizeof(xmlGlobalState));
    xmlInitGlobalState(gs);
    return (gs);
}
#endif

static xmlGlobalStatePtr
xmlGetThreadLocalStorage(int allowFailure) {
    xmlGlobalState *gs;

    (void) allowFailure;

    xmlInitParser();

#ifdef USE_TLS
    gs = &globalState;
    if (gs->initialized == 0)
        xmlInitGlobalState(gs);
#elif defined(HAVE_POSIX_THREADS)
    gs = (xmlGlobalState *) pthread_getspecific(globalkey);
    if (gs == NULL)
        gs = xmlNewGlobalState(allowFailure);
#elif defined(HAVE_WIN32_THREADS)
    gs = (xmlGlobalState *) TlsGetValue(globalkey);
    if (gs == NULL)
        gs = xmlNewGlobalState(allowFailure);
#else
    gs = NULL;
#endif

    return(gs);
}

#else /* LIBXML_THREAD_ENABLED */

static xmlGlobalStatePtr
xmlGetThreadLocalStorage(int allowFailure ATTRIBUTE_UNUSED) {
    return(&globalState);
}

#endif /* LIBXML_THREAD_ENABLED */

static void
xmlInitGlobalState(xmlGlobalStatePtr gs) {
    gs->localRngState[0] = xmlGlobalRandom();
    gs->localRngState[1] = xmlGlobalRandom();

    memset(&gs->lastError, 0, sizeof(xmlError));

#ifdef LIBXML_THREAD_ALLOC_ENABLED
    /* XML_GLOBALS_ALLOC */
    gs->free = free;
    gs->malloc = malloc;
    gs->mallocAtomic = malloc;
    gs->realloc = realloc;
    gs->memStrdup = xmlPosixStrdup;
#endif

    xmlMutexLock(&xmlThrDefMutex);

    /* XML_GLOBALS_PARSER */
    gs->doValidityCheckingDefaultValue =
         xmlDoValidityCheckingDefaultValueThrDef;
    gs->getWarningsDefaultValue = xmlGetWarningsDefaultValueThrDef;
    gs->keepBlanksDefaultValue = xmlKeepBlanksDefaultValueThrDef;
    gs->loadExtDtdDefaultValue = xmlLoadExtDtdDefaultValueThrDef;
    gs->pedanticParserDefaultValue = xmlPedanticParserDefaultValueThrDef;
    gs->substituteEntitiesDefaultValue =
        xmlSubstituteEntitiesDefaultValueThrDef;
#ifdef LIBXML_OUTPUT_ENABLED
    gs->indentTreeOutput = xmlIndentTreeOutputThrDef;
    gs->treeIndentString = xmlTreeIndentStringThrDef;
    gs->saveNoEmptyTags = xmlSaveNoEmptyTagsThrDef;
#endif

    /* XML_GLOBALS_ERROR */
    gs->genericError = xmlGenericErrorThrDef;
    gs->structuredError = xmlStructuredErrorThrDef;
    gs->genericErrorContext = xmlGenericErrorContextThrDef;
    gs->structuredErrorContext = xmlStructuredErrorContextThrDef;

    /* XML_GLOBALS_TREE */
    gs->registerNodeDefaultValue = xmlRegisterNodeDefaultValueThrDef;
    gs->deregisterNodeDefaultValue = xmlDeregisterNodeDefaultValueThrDef;

    /* XML_GLOBALS_IO */
    gs->parserInputBufferCreateFilenameValue =
        xmlParserInputBufferCreateFilenameValueThrDef;
    gs->outputBufferCreateFilenameValue =
        xmlOutputBufferCreateFilenameValueThrDef;

    xmlMutexUnlock(&xmlThrDefMutex);

#ifdef USE_TLS
    gs->initialized = 1;
#endif

#ifdef HAVE_POSIX_THREADS
    pthread_setspecific(globalkey, gs);
#elif defined HAVE_WIN32_THREADS
#ifndef USE_TLS
    TlsSetValue(globalkey, gs);
#endif
#ifdef USE_WAIT_DTOR
    xmlRegisterGlobalStateDtor(gs);
#endif
#endif
}

const xmlError *
__xmlLastError(void) {
    return(&xmlGetThreadLocalStorage(0)->lastError);
}

int *
__xmlDoValidityCheckingDefaultValue(void) {
    return(&xmlGetThreadLocalStorage(0)->doValidityCheckingDefaultValue);
}

int *
__xmlGetWarningsDefaultValue(void) {
    return(&xmlGetThreadLocalStorage(0)->getWarningsDefaultValue);
}

int *
__xmlKeepBlanksDefaultValue(void) {
    return(&xmlGetThreadLocalStorage(0)->keepBlanksDefaultValue);
}

int *
__xmlLineNumbersDefaultValue(void) {
    return(&lineNumbersDefaultValue);
}

int *
__xmlLoadExtDtdDefaultValue(void) {
    return(&xmlGetThreadLocalStorage(0)->loadExtDtdDefaultValue);
}

int *
__xmlPedanticParserDefaultValue(void) {
    return(&xmlGetThreadLocalStorage(0)->pedanticParserDefaultValue);
}

int *
__xmlSubstituteEntitiesDefaultValue(void) {
    return(&xmlGetThreadLocalStorage(0)->substituteEntitiesDefaultValue);
}

#ifdef LIBXML_OUTPUT_ENABLED
int *
__xmlIndentTreeOutput(void) {
    return(&xmlGetThreadLocalStorage(0)->indentTreeOutput);
}

const char **
__xmlTreeIndentString(void) {
    return(&xmlGetThreadLocalStorage(0)->treeIndentString);
}

int *
__xmlSaveNoEmptyTags(void) {
    return(&xmlGetThreadLocalStorage(0)->saveNoEmptyTags);
}
#endif

xmlGenericErrorFunc *
__xmlGenericError(void) {
    return(&xmlGetThreadLocalStorage(0)->genericError);
}

void **
__xmlGenericErrorContext(void) {
    return(&xmlGetThreadLocalStorage(0)->genericErrorContext);
}

xmlStructuredErrorFunc *
__xmlStructuredError(void) {
    return(&xmlGetThreadLocalStorage(0)->structuredError);
}

void **
__xmlStructuredErrorContext(void) {
    return(&xmlGetThreadLocalStorage(0)->structuredErrorContext);
}

xmlRegisterNodeFunc *
__xmlRegisterNodeDefaultValue(void) {
    return(&xmlGetThreadLocalStorage(0)->registerNodeDefaultValue);
}

xmlDeregisterNodeFunc *
__xmlDeregisterNodeDefaultValue(void) {
    return(&xmlGetThreadLocalStorage(0)->deregisterNodeDefaultValue);
}

xmlParserInputBufferCreateFilenameFunc *
__xmlParserInputBufferCreateFilenameValue(void) {
    return(&xmlGetThreadLocalStorage(0)->parserInputBufferCreateFilenameValue);
}

xmlOutputBufferCreateFilenameFunc *
__xmlOutputBufferCreateFilenameValue(void) {
    return(&xmlGetThreadLocalStorage(0)->outputBufferCreateFilenameValue);
}

#ifdef LIBXML_THREAD_ALLOC_ENABLED
xmlMallocFunc *
__xmlMalloc(void) {
    return(&xmlGetThreadLocalStorage(0)->malloc);
}

xmlMallocFunc *
__xmlMallocAtomic(void) {
    return(&xmlGetThreadLocalStorage(0)->mallocAtomic);
}

xmlReallocFunc *
__xmlRealloc(void) {
    return(&xmlGetThreadLocalStorage(0)->realloc);
}

xmlFreeFunc *
__xmlFree(void) {
    return(&xmlGetThreadLocalStorage(0)->free);
}

xmlStrdupFunc *
__xmlMemStrdup(void) {
    return(&xmlGetThreadLocalStorage(0)->memStrdup);
}
#endif /* LIBXML_THREAD_ALLOC_ENABLED */

/**
 * @returns the local RNG state.
 */
unsigned *
xmlGetLocalRngState(void) {
    return(xmlGetThreadLocalStorage(0)->localRngState);
}

/**
 * Check whether thread-local storage could be allocated.
 *
 * In cross-platform code running in multithreaded environments, this
 * function should be called once in each thread before calling other
 * library functions to make sure that thread-local storage was
 * allocated properly.
 *
 * @since 2.12.0
 * @returns 0 on success or -1 if a memory allocation failed. A failed
 * allocation signals a typically fatal and irrecoverable out-of-memory
 * situation. Don't call any library functions in this case.
 */
int
xmlCheckThreadLocalStorage(void) {
#if defined(LIBXML_THREAD_ENABLED) && !defined(USE_TLS)
    if (xmlGetThreadLocalStorage(1) == NULL)
        return(-1);
#endif
    return(0);
}

/**
 * @returns a pointer to the global error struct.
 */
xmlError *
xmlGetLastErrorInternal(void) {
    return(&xmlGetThreadLocalStorage(0)->lastError);
}

#ifdef USE_DLL_MAIN
/**
 * Entry point for Windows library. It is being used to free thread-specific
 * storage.
 *
 * @param hinstDLL  handle to DLL instance
 * @param fdwReason  Reason code for entry
 * @param lpvReserved  generic pointer (depends upon reason code)
 * @returns TRUE always
 */
#if defined(LIBXML_STATIC_FOR_DLL)
int
xmlDllMain(ATTRIBUTE_UNUSED void *hinstDLL, unsigned long fdwReason,
           ATTRIBUTE_UNUSED void *lpvReserved)
#else

/*
 * Declare to avoid "no previous prototype for 'DllMain'" warning.
 *
 * Note that we do NOT want to include this function declaration in
 * a public header because it's meant to be called by Windows itself,
 * not a program that uses this library.
 *
 * It is a mistake to export this function, but changing that seems
 * to break the ABI.
 */
XMLPUBFUN BOOL WINAPI
DllMain (HINSTANCE hinstDLL,
         DWORD     fdwReason,
         LPVOID    lpvReserved);

BOOL WINAPI
DllMain(ATTRIBUTE_UNUSED HINSTANCE hinstDLL, DWORD fdwReason,
        ATTRIBUTE_UNUSED LPVOID lpvReserved)
#endif
{
    if ((fdwReason == DLL_THREAD_DETACH) ||
        (fdwReason == DLL_PROCESS_DETACH)) {
#ifdef USE_TLS
        xmlFreeGlobalState(&globalState);
#else
        if (globalkey != TLS_OUT_OF_INDEXES) {
            xmlGlobalState *globalval;

            globalval = (xmlGlobalState *) TlsGetValue(globalkey);
            if (globalval) {
                xmlFreeGlobalState(globalval);
                TlsSetValue(globalkey, NULL);
            }
        }
#endif
    }

#ifndef LIBXML_THREAD_ALLOC_ENABLED
    if (fdwReason == DLL_PROCESS_DETACH) {
        if (xmlFree == free)
            xmlCleanupParser();
        if (globalkey != TLS_OUT_OF_INDEXES) {
            TlsFree(globalkey);
            globalkey = TLS_OUT_OF_INDEXES;
        }
    }
#endif

    return TRUE;
}
#endif /* USE_DLL_MAIN */

/**
 * Set per-thread default value.
 *
 * @deprecated Call #xmlSetGenericErrorFunc in each thread.
 *
 * @param ctx  user data
 * @param handler  error handler
 */
void
xmlThrDefSetGenericErrorFunc(void *ctx, xmlGenericErrorFunc handler) {
    xmlMutexLock(&xmlThrDefMutex);
    xmlGenericErrorContextThrDef = ctx;
    if (handler != NULL)
	xmlGenericErrorThrDef = handler;
    else
	xmlGenericErrorThrDef = xmlGenericErrorDefaultFunc;
    xmlMutexUnlock(&xmlThrDefMutex);
}

/**
 * Set per-thread default value.
 *
 * @deprecated Call #xmlSetStructuredErrorFunc in each thread.
 *
 * @param ctx  user data
 * @param handler  error handler
 */
void
xmlThrDefSetStructuredErrorFunc(void *ctx, xmlStructuredErrorFunc handler) {
    xmlMutexLock(&xmlThrDefMutex);
    xmlStructuredErrorContextThrDef = ctx;
    xmlStructuredErrorThrDef = handler;
    xmlMutexUnlock(&xmlThrDefMutex);
}

/**
 * Set per-thread default value.
 *
 * @deprecated Use xmlParserOption XML_PARSE_DTDVALID.
 *
 * @param v  new value
 * @returns the old value
 */
int xmlThrDefDoValidityCheckingDefaultValue(int v) {
    int ret;
    xmlMutexLock(&xmlThrDefMutex);
    ret = xmlDoValidityCheckingDefaultValueThrDef;
    xmlDoValidityCheckingDefaultValueThrDef = v;
    xmlMutexUnlock(&xmlThrDefMutex);
    return ret;
}

/**
 * Set per-thread default value.
 *
 * @deprecated Use xmlParserOption XML_PARSE_NOWARNING.
 *
 * @param v  new value
 * @returns the old value
 */
int xmlThrDefGetWarningsDefaultValue(int v) {
    int ret;
    xmlMutexLock(&xmlThrDefMutex);
    ret = xmlGetWarningsDefaultValueThrDef;
    xmlGetWarningsDefaultValueThrDef = v;
    xmlMutexUnlock(&xmlThrDefMutex);
    return ret;
}

#ifdef LIBXML_OUTPUT_ENABLED
/**
 * Set per-thread default value.
 *
 * @deprecated Indenting is enabled by default. Use the xmlsave.h API
 * and xmlSaveOption XML_SAVE_NO_INDENT to disable indenting.
 *
 * @param v  new value
 * @returns the old value
 */
int xmlThrDefIndentTreeOutput(int v) {
    int ret;
    xmlMutexLock(&xmlThrDefMutex);
    ret = xmlIndentTreeOutputThrDef;
    xmlIndentTreeOutputThrDef = v;
    xmlMutexUnlock(&xmlThrDefMutex);
    return ret;
}

/**
 * Set per-thread default value.
 *
 * @deprecated Use the xmlsave.h API and #xmlSaveSetIndentString.
 *
 * @param v  new value
 * @returns the old value
 */
const char * xmlThrDefTreeIndentString(const char * v) {
    const char * ret;
    xmlMutexLock(&xmlThrDefMutex);
    ret = xmlTreeIndentStringThrDef;
    xmlTreeIndentStringThrDef = v;
    xmlMutexUnlock(&xmlThrDefMutex);
    return ret;
}

/**
 * Set per-thread default value.
 *
 * @deprecated Use the xmlsave.h API and xmlSaveOption XML_SAVE_NO_EMPTY.
 *
 * @param v  new value
 * @returns the old value
 */
int xmlThrDefSaveNoEmptyTags(int v) {
    int ret;
    xmlMutexLock(&xmlThrDefMutex);
    ret = xmlSaveNoEmptyTagsThrDef;
    xmlSaveNoEmptyTagsThrDef = v;
    xmlMutexUnlock(&xmlThrDefMutex);
    return ret;
}
#endif

/**
 * Set per-thread default value.
 *
 * @deprecated Whitespace is kept by default. Use xmlParserOption
 * XML_PARSE_NOBLANKS to remove whitespace.
 *
 * @param v  new value
 * @returns the old value
 */
int xmlThrDefKeepBlanksDefaultValue(int v) {
    int ret;
    xmlMutexLock(&xmlThrDefMutex);
    ret = xmlKeepBlanksDefaultValueThrDef;
    xmlKeepBlanksDefaultValueThrDef = v;
    xmlMutexUnlock(&xmlThrDefMutex);
    return ret;
}

/**
 * Set per-thread default value.
 *
 * @deprecated Has no effect.
 *
 * @param v  unused
 * @returns 1
 */
int xmlThrDefLineNumbersDefaultValue(int v ATTRIBUTE_UNUSED) {
    return 1;
}

/**
 * Set per-thread default value.
 *
 * @deprecated Use xmlParserOption XML_PARSE_DTDLOAD.
 *
 * @param v  new value
 * @returns the old value
 */
int xmlThrDefLoadExtDtdDefaultValue(int v) {
    int ret;
    xmlMutexLock(&xmlThrDefMutex);
    ret = xmlLoadExtDtdDefaultValueThrDef;
    xmlLoadExtDtdDefaultValueThrDef = v;
    xmlMutexUnlock(&xmlThrDefMutex);
    return ret;
}

/**
 * Set per-thread default value.
 *
 * @deprecated Use xmlParserOption XML_PARSE_PEDANTIC.
 *
 * @param v  new value
 * @returns the old value
 */
int xmlThrDefPedanticParserDefaultValue(int v) {
    int ret;
    xmlMutexLock(&xmlThrDefMutex);
    ret = xmlPedanticParserDefaultValueThrDef;
    xmlPedanticParserDefaultValueThrDef = v;
    xmlMutexUnlock(&xmlThrDefMutex);
    return ret;
}

/**
 * Set per-thread default value.
 *
 * @deprecated Use xmlParserOption XML_PARSE_NOENT.
 *
 * @param v  new value
 * @returns the old value
 */
int xmlThrDefSubstituteEntitiesDefaultValue(int v) {
    int ret;
    xmlMutexLock(&xmlThrDefMutex);
    ret = xmlSubstituteEntitiesDefaultValueThrDef;
    xmlSubstituteEntitiesDefaultValueThrDef = v;
    xmlMutexUnlock(&xmlThrDefMutex);
    return ret;
}

/**
 * Set per-thread default value.
 *
 * @deprecated This feature will be removed.
 *
 * @param func  new value
 * @returns the old value
 */
xmlRegisterNodeFunc
xmlThrDefRegisterNodeDefault(xmlRegisterNodeFunc func)
{
    xmlRegisterNodeFunc old;

    xmlMutexLock(&xmlThrDefMutex);
    old = xmlRegisterNodeDefaultValueThrDef;

    xmlRegisterCallbacks = 1;
    xmlRegisterNodeDefaultValueThrDef = func;
    xmlMutexUnlock(&xmlThrDefMutex);

    return(old);
}

/**
 * Set per-thread default value.
 *
 * @deprecated This feature will be removed.
 *
 * @param func  new value
 * @returns the old value
 */
xmlDeregisterNodeFunc
xmlThrDefDeregisterNodeDefault(xmlDeregisterNodeFunc func)
{
    xmlDeregisterNodeFunc old;

    xmlMutexLock(&xmlThrDefMutex);
    old = xmlDeregisterNodeDefaultValueThrDef;

    xmlRegisterCallbacks = 1;
    xmlDeregisterNodeDefaultValueThrDef = func;
    xmlMutexUnlock(&xmlThrDefMutex);

    return(old);
}

/**
 * Set per-thread default value.
 *
 * @deprecated Call #xmlParserInputBufferCreateFilenameDefault
 * in each thread.
 *
 * @param func  new value
 * @returns the old value
 */
xmlParserInputBufferCreateFilenameFunc
xmlThrDefParserInputBufferCreateFilenameDefault(xmlParserInputBufferCreateFilenameFunc func)
{
    xmlParserInputBufferCreateFilenameFunc old;

    xmlMutexLock(&xmlThrDefMutex);
    old = xmlParserInputBufferCreateFilenameValueThrDef;
    if (old == NULL) {
		old = __xmlParserInputBufferCreateFilename;
	}

    xmlParserInputBufferCreateFilenameValueThrDef = func;
    xmlMutexUnlock(&xmlThrDefMutex);

    return(old);
}

/**
 * Set per-thread default value.
 *
 * @deprecated Call #xmlOutputBufferCreateFilenameDefault
 * in each thread.
 *
 * @param func  new value
 * @returns the old value
 */
xmlOutputBufferCreateFilenameFunc
xmlThrDefOutputBufferCreateFilenameDefault(xmlOutputBufferCreateFilenameFunc func)
{
    xmlOutputBufferCreateFilenameFunc old;

    xmlMutexLock(&xmlThrDefMutex);
    old = xmlOutputBufferCreateFilenameValueThrDef;
#ifdef LIBXML_OUTPUT_ENABLED
    if (old == NULL) {
		old = __xmlOutputBufferCreateFilename;
	}
#endif
    xmlOutputBufferCreateFilenameValueThrDef = func;
    xmlMutexUnlock(&xmlThrDefMutex);

    return(old);
}


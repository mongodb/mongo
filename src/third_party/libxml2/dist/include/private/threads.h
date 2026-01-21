#ifndef XML_THREADS_H_PRIVATE__
#define XML_THREADS_H_PRIVATE__

#include <libxml/threads.h>

#ifdef LIBXML_THREAD_ENABLED
  #ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #ifdef _WIN32_WINNT
      #undef _WIN32_WINNT
    #endif
    #define _WIN32_WINNT 0x0600
    #include <windows.h>
    #define HAVE_WIN32_THREADS
  #else
    #include <pthread.h>
    #define HAVE_POSIX_THREADS
  #endif
#endif

/*
 * xmlMutex are a simple mutual exception locks
 */
struct _xmlMutex {
#ifdef HAVE_POSIX_THREADS
    pthread_mutex_t lock;
#elif defined HAVE_WIN32_THREADS
    CRITICAL_SECTION cs;
#else
    int empty;
#endif
};

/*
 * xmlRMutex are reentrant mutual exception locks
 */
struct _xmlRMutex {
#ifdef HAVE_POSIX_THREADS
    pthread_mutex_t lock;
    unsigned int held;
    unsigned int waiters;
    pthread_t tid;
    pthread_cond_t cv;
#elif defined HAVE_WIN32_THREADS
    CRITICAL_SECTION cs;
#else
    int empty;
#endif
};

XML_HIDDEN void
xmlInitMutex(xmlMutex *mutex);
XML_HIDDEN void
xmlCleanupMutex(xmlMutex *mutex);

XML_HIDDEN void
xmlInitRMutex(xmlRMutex *mutex);
XML_HIDDEN void
xmlCleanupRMutex(xmlRMutex *mutex);

#ifdef LIBXML_SCHEMAS_ENABLED
XML_HIDDEN void
xmlInitSchemasTypesInternal(void);
XML_HIDDEN void
xmlCleanupSchemasTypesInternal(void);
#endif

#ifdef LIBXML_RELAXNG_ENABLED
XML_HIDDEN void
xmlInitRelaxNGInternal(void);
XML_HIDDEN void
xmlCleanupRelaxNGInternal(void);
#endif


#endif /* XML_THREADS_H_PRIVATE__ */

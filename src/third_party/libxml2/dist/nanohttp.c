/*
 * nanohttp.c: ABI compatibility stubs for removed HTTP client
 *
 * See Copyright for the status of this software.
 */

#define IN_LIBXML
#include "libxml.h"

#ifdef LIBXML_HTTP_STUBS_ENABLED

#include <stddef.h>

#include <libxml/nanohttp.h>
#include <libxml/xmlIO.h>

/**
 * @deprecated HTTP support was removed in 2.15.
 */
void
xmlNanoHTTPInit(void) {
}

/**
 * @deprecated HTTP support was removed in 2.15.
 */
void
xmlNanoHTTPCleanup(void) {
}

/**
 * @deprecated HTTP support was removed in 2.15.
 * @param URL  The proxy URL used to initialize the proxy context
 */
void
xmlNanoHTTPScanProxy(const char *URL ATTRIBUTE_UNUSED) {
}

/**
 * @deprecated HTTP support was removed in 2.15.
 *
 * @param URL  The URL to load
 * @param contentType  if available the Content-Type information will be
 *                returned at that location
 * @returns NULL.
 */
void*
xmlNanoHTTPOpen(const char *URL ATTRIBUTE_UNUSED, char **contentType) {
    if (contentType != NULL) *contentType = NULL;
    return(NULL);
}

/**
 * @deprecated HTTP support was removed in 2.15.
 *
 * @param URL  The URL to load
 * @param contentType  if available the Content-Type information will be
 *                returned at that location
 * @param redir  if available the redirected URL will be returned
 * @returns NULL.
 */
void*
xmlNanoHTTPOpenRedir(const char *URL ATTRIBUTE_UNUSED, char **contentType,
                     char **redir) {
    if (contentType != NULL) *contentType = NULL;
    if (redir != NULL) *redir = NULL;
    return(NULL);
}

/**
 * @deprecated HTTP support was removed in 2.15.
 *
 * @param ctx  the HTTP context
 * @param dest  a buffer
 * @param len  the buffer length
 * @returns -1.
 */
int
xmlNanoHTTPRead(void *ctx ATTRIBUTE_UNUSED, void *dest ATTRIBUTE_UNUSED,
                int len ATTRIBUTE_UNUSED) {
    return(-1);
}

/**
 * @deprecated HTTP support was removed in 2.15.
 * @param ctx  the HTTP context
 */
void
xmlNanoHTTPClose(void *ctx ATTRIBUTE_UNUSED) {
}

/**
 * @deprecated HTTP support was removed in 2.15.
 *
 * @param URL  The URL to load
 * @param method  the HTTP method to use
 * @param input  the input string if any
 * @param contentType  the Content-Type information IN and OUT
 * @param redir  the redirected URL OUT
 * @param headers  the extra headers
 * @param ilen  input length
 * @returns NULL.
 */
void*
xmlNanoHTTPMethodRedir(const char *URL ATTRIBUTE_UNUSED,
                       const char *method ATTRIBUTE_UNUSED,
                       const char *input ATTRIBUTE_UNUSED,
                       char **contentType, char **redir,
                       const char *headers ATTRIBUTE_UNUSED,
                       int ilen ATTRIBUTE_UNUSED) {
    if (contentType != NULL) *contentType = NULL;
    if (redir != NULL) *redir = NULL;
    return(NULL);
}

/**
 * @deprecated HTTP support was removed in 2.15.
 *
 * @param URL  The URL to load
 * @param method  the HTTP method to use
 * @param input  the input string if any
 * @param contentType  the Content-Type information IN and OUT
 * @param headers  the extra headers
 * @param ilen  input length
 * @returns NULL.
 */
void*
xmlNanoHTTPMethod(const char *URL ATTRIBUTE_UNUSED,
                  const char *method ATTRIBUTE_UNUSED,
                  const char *input ATTRIBUTE_UNUSED,
                  char **contentType, const char *headers ATTRIBUTE_UNUSED,
                  int ilen ATTRIBUTE_UNUSED) {
    if (contentType != NULL) *contentType = NULL;
    return(NULL);
}

/**
 * @deprecated HTTP support was removed in 2.15.
 *
 * @param URL  The URL to load
 * @param filename  the filename where the content should be saved
 * @param contentType  if available the Content-Type information will be
 *                returned at that location
 * @returns -1.
 */
int
xmlNanoHTTPFetch(const char *URL ATTRIBUTE_UNUSED,
                 const char *filename ATTRIBUTE_UNUSED, char **contentType) {
    if (contentType != NULL) *contentType = NULL;
    return(-1);
}

#ifdef LIBXML_OUTPUT_ENABLED
/**
 * @deprecated HTTP support was removed in 2.15.
 *
 * @param ctxt  the HTTP context
 * @param filename  the filename where the content should be saved
 * @returns -1.
 */
int
xmlNanoHTTPSave(void *ctxt ATTRIBUTE_UNUSED,
                const char *filename ATTRIBUTE_UNUSED) {
    return(-1);
}
#endif /* LIBXML_OUTPUT_ENABLED */

/**
 * @deprecated HTTP support was removed in 2.15.
 *
 * @param ctx  the HTTP context
 * @returns -1.
 */
int
xmlNanoHTTPReturnCode(void *ctx ATTRIBUTE_UNUSED) {
    return(-1);
}

/**
 * @deprecated HTTP support was removed in 2.15.
 *
 * @param ctx  the HTTP context
 * @returns NULL.
 */
const char *
xmlNanoHTTPAuthHeader(void *ctx ATTRIBUTE_UNUSED) {
    return(NULL);
}

/**
 * @deprecated HTTP support was removed in 2.15.
 *
 * @param ctx  the HTTP context
 * @returns -1.
 */
int
xmlNanoHTTPContentLength(void *ctx ATTRIBUTE_UNUSED) {
    return(-1);
}

/**
 * @deprecated HTTP support was removed in 2.15.
 *
 * @param ctx  the HTTP context
 * @returns NULL.
 */
const char *
xmlNanoHTTPRedir(void *ctx ATTRIBUTE_UNUSED) {
    return(NULL);
}

/**
 * @deprecated HTTP support was removed in 2.15.
 *
 * @param ctx  the HTTP context
 * @returns NULL.
 */
const char *
xmlNanoHTTPEncoding(void *ctx ATTRIBUTE_UNUSED) {
    return(NULL);
}

/**
 * @deprecated HTTP support was removed in 2.15.
 *
 * @param ctx  the HTTP context
 * @returns NULL.
 */
const char *
xmlNanoHTTPMimeType(void *ctx ATTRIBUTE_UNUSED) {
    return(NULL);
}

/**
 * @deprecated HTTP support was removed in 2.15.
 *
 * @param filename  the URI for matching
 * @returns 0.
 */
int
xmlIOHTTPMatch(const char *filename ATTRIBUTE_UNUSED) {
    return(0);
}

/**
 * @deprecated HTTP support was removed in 2.15.
 *
 * @param filename  the URI for matching
 * @returns NULL.
 */
void *
xmlIOHTTPOpen(const char *filename ATTRIBUTE_UNUSED) {
    return(NULL);
}

#ifdef LIBXML_OUTPUT_ENABLED
/**
 * @deprecated HTTP support was removed in 2.15.
 *
 * @param post_uri  The destination URI for the document
 * @param compression  The compression desired for the document.
 * @returns NULL.
 */
void *
xmlIOHTTPOpenW(const char *post_uri ATTRIBUTE_UNUSED,
               int compression ATTRIBUTE_UNUSED)
{
    return(NULL);
}
#endif /* LIBXML_OUTPUT_ENABLED */

/**
 * @deprecated HTTP support was removed in 2.15.
 *
 * @param context  the I/O context
 * @param buffer  where to drop data
 * @param len  number of bytes to write
 * @returns -1.
 */
int
xmlIOHTTPRead(void *context ATTRIBUTE_UNUSED, char *buffer ATTRIBUTE_UNUSED,
              int len ATTRIBUTE_UNUSED) {
    return(-1);
}

/**
 * @deprecated Internal function, don't use.
 *
 * @param context  the I/O context
 * @returns 0
 */
int
xmlIOHTTPClose (void *context ATTRIBUTE_UNUSED) {
    return 0;
}

/**
 * @deprecated HTTP support was removed in 2.15.
 */
void
xmlRegisterHTTPPostCallbacks(void) {
    xmlRegisterDefaultOutputCallbacks();
}

#endif /* LIBXML_HTTP_STUBS_ENABLED */

/*
 * buf.c: memory buffers for libxml2
 *
 * new buffer structures and entry points to simplify the maintenance
 * of libxml2 and ensure we keep good control over memory allocations
 * and stay 64 bits clean.
 * The new entry point use the xmlBuf opaque structure and
 * xmlBuf...() counterparts to the old xmlBuf...() functions
 *
 * See Copyright for the status of this software.
 *
 * Author: Daniel Veillard
 */

#define IN_LIBXML
#include "libxml.h"

#include <string.h>
#include <limits.h>

#include <libxml/parser.h>
#include <libxml/tree.h>

#include "private/buf.h"

#ifndef SIZE_MAX
#define SIZE_MAX ((size_t) -1)
#endif

#define BUF_FLAG_OOM        (1u << 0)
#define BUF_FLAG_OVERFLOW   (1u << 1)
#define BUF_FLAG_STATIC     (1u << 2)

#define BUF_ERROR(buf) ((buf)->flags & (BUF_FLAG_OOM | BUF_FLAG_OVERFLOW))
#define BUF_STATIC(buf) ((buf)->flags & BUF_FLAG_STATIC)

/**
 * A buffer structure. The base of the structure is somehow compatible
 * with struct _xmlBuffer to limit risks on application which accessed
 * directly the input->buf->buffer structures.
 */

struct _xmlBuf {
    xmlChar *content;		/* The buffer content UTF8 */
    xmlChar *mem;		/* Start of the allocation */
    size_t use;		        /* The buffer size used */
    size_t size;		/* The buffer size, excluding terminating 0 */
    size_t maxSize;             /* The maximum buffer size */
    unsigned flags;             /* flags */
};

/**
 * Handle an out of memory condition
 * To be improved...
 *
 * @param buf  the buffer
 */
static void
xmlBufMemoryError(xmlBufPtr buf)
{
    if (!BUF_ERROR(buf))
        buf->flags |= BUF_FLAG_OOM;
}

/**
 * Handle a buffer overflow error
 * To be improved...
 *
 * @param buf  the buffer
 */
static void
xmlBufOverflowError(xmlBufPtr buf)
{
    if (!BUF_ERROR(buf))
        buf->flags |= BUF_FLAG_OVERFLOW;
}

/**
 * Create a buffer.
 *
 * @param size  initial buffer size
 * @returns  the new structure
 */
xmlBuf *
xmlBufCreate(size_t size) {
    xmlBufPtr ret;

    if (size == SIZE_MAX)
        return(NULL);

    ret = xmlMalloc(sizeof(*ret));
    if (ret == NULL)
        return(NULL);

    ret->use = 0;
    ret->flags = 0;
    ret->size = size;
    ret->maxSize = SIZE_MAX - 1;

    ret->mem = xmlMalloc(ret->size + 1);
    if (ret->mem == NULL) {
        xmlFree(ret);
        return(NULL);
    }
    ret->content = ret->mem;
    ret->content[0] = 0;

    return(ret);
}

/**
 * Create a buffer initialized with memory.
 *
 * If `isStatic` is set, uses the memory area directly as backing store.
 * The memory must be zero-terminated and not be modified for the
 * lifetime of the buffer. A static buffer can't be grown, modified or
 * detached, but it can be shrunk.
 *
 * @param mem  a memory area
 * @param size  size of the buffer excluding terminator
 * @param isStatic  whether the memory area is static
 * @returns  a new buffer.
 */
xmlBuf *
xmlBufCreateMem(const xmlChar *mem, size_t size, int isStatic) {
    xmlBufPtr ret;

    if (mem == NULL)
        return(NULL);

    ret = xmlMalloc(sizeof(*ret));
    if (ret == NULL)
        return(NULL);

    if (isStatic) {
        /* Check that memory is zero-terminated */
        if (mem[size] != 0) {
            xmlFree(ret);
            return(NULL);
        }
        ret->flags = BUF_FLAG_STATIC;
        ret->mem = (xmlChar *) mem;
    } else {
        ret->flags = 0;
        ret->mem = xmlMalloc(size + 1);
        if (ret->mem == NULL) {
            xmlFree(ret);
            return(NULL);
        }
        memcpy(ret->mem, mem, size);
        ret->mem[size] = 0;
    }

    ret->use = size;
    ret->size = size;
    ret->maxSize = SIZE_MAX - 1;
    ret->content = ret->mem;

    return(ret);
}

/**
 * Extract buffer content.
 *
 * Return the content of the buffer as an `xmlChar` string,
 * clearing the buffer.
 *
 * This doesn't work with static buffers as they can't be reset.
 *
 * @param buf  the buffer
 * @returns  the buffer content
 */
xmlChar *
xmlBufDetach(xmlBuf *buf) {
    xmlChar *ret;

    if ((buf == NULL) || (BUF_ERROR(buf)) || (BUF_STATIC(buf)))
        return(NULL);

    if (buf->content != buf->mem) {
        ret = xmlStrndup(buf->content, buf->use);
        xmlFree(buf->mem);
    } else {
        ret = buf->mem;
    }

    buf->content = NULL;
    buf->mem = NULL;
    buf->size = 0;
    buf->use = 0;

    return ret;
}

/**
 * Free a buffer.
 *
 * @param buf  the buffer to free
 */
void
xmlBufFree(xmlBuf *buf) {
    if (buf == NULL)
	return;

    if (!BUF_STATIC(buf))
        xmlFree(buf->mem);
    xmlFree(buf);
}

/**
 * Empty a buffer.
 *
 * @param buf  the buffer
 */
void
xmlBufEmpty(xmlBuf *buf) {
    if ((buf == NULL) || (BUF_ERROR(buf)) || (BUF_STATIC(buf)))
        return;
    if (buf->mem == NULL)
        return;

    buf->use = 0;
    buf->size += buf->content - buf->mem;
    buf->content = buf->mem;
    buf->content[0] = 0;
}

/**
 * Discard bytes at the start of a buffer.
 *
 * NOTE that the return value differs from #xmlBufferShrink
 * as it will return 0 on error instead of -1 due to size_t being
 * used as the return type.
 *
 * @deprecated Internal function, don't use.
 *
 * @param buf  the buffer
 * @param len  the number of bytes to remove
 * @returns  the number of bytes removed or 0 in case of failure
 */
size_t
xmlBufShrink(xmlBuf *buf, size_t len) {
    if ((buf == NULL) || (BUF_ERROR(buf)))
        return(0);
    if (len == 0)
        return(0);

    if (len > buf->use)
        return(0);

    buf->use -= len;
    buf->content += len;
    buf->size -= len;

    return(len);
}

/**
 * Grow a buffer.
 *
 * Increase the capacity of a buffer. `len` is the amount of
 * free space required after the current end of the buffer.
 *
 * Assumes `len > buf->size - buf->use`.
 *
 * @param buf  the buffer
 * @param len  number of extra bytes to allocate
 * @returns  0 on success, -1 in case of error
 */
static int
xmlBufGrowInternal(xmlBufPtr buf, size_t len) {
    size_t size;
    size_t start;
    xmlChar *newbuf;

    /*
     * If there's enough space at the start of the buffer,
     * move the contents.
     */
    start = buf->content - buf->mem;
    if (len <= start + buf->size - buf->use) {
        memmove(buf->mem, buf->content, buf->use + 1);
        buf->size += start;
        buf->content = buf->mem;
        return(0);
    }

    if (len > buf->maxSize - buf->use) {
        xmlBufOverflowError(buf);
        return(-1);
    }

    if (buf->size > (size_t) len) {
        if (buf->size <= buf->maxSize / 2)
            size = buf->size * 2;
        else
            size = buf->maxSize;
    } else {
        size = buf->use + len;
        if (size <= buf->maxSize - 100)
            size += 100;
    }

    if (buf->content == buf->mem) {
        newbuf = xmlRealloc(buf->mem, size + 1);
        if (newbuf == NULL) {
            xmlBufMemoryError(buf);
            return(-1);
        }
    } else {
        newbuf = xmlMalloc(size + 1);
        if (newbuf == NULL) {
            xmlBufMemoryError(buf);
            return(-1);
        }
        if (buf->content != NULL)
            memcpy(newbuf, buf->content, buf->use + 1);
        xmlFree(buf->mem);
    }

    buf->mem = newbuf;
    buf->content = newbuf;
    buf->size = size;

    return(0);
}

/**
 * Grow a buffer.
 *
 * Increase the capacity of a buffer. `len` is the amount of
 * free space required after the current end of the buffer.
 *
 * @param buf  the buffer
 * @param len  number of extra bytes to allocate
 * @returns  0 on success, -1 in case of error
 */
int
xmlBufGrow(xmlBuf *buf, size_t len) {
    if ((buf == NULL) || (BUF_ERROR(buf)) || (BUF_STATIC(buf)))
        return(-1);

    if (len <= buf->size - buf->use)
        return(0);

    if (xmlBufGrowInternal(buf, len) < 0)
        return(-1);

    return(0);
}

/**
 * Get pointer into buffer content.
 *
 * @param buf  the buffer
 * @returns  the internal content or NULL in case of error
 */
xmlChar *
xmlBufContent(const xmlBuf *buf)
{
    if ((!buf) || (BUF_ERROR(buf)))
        return NULL;

    return(buf->content);
}

/**
 * Return a pointer to the end of the buffer content.
 *
 * @param buf  the buffer
 * @returns  the end of the internal content or NULL in case of error
 */
xmlChar *
xmlBufEnd(xmlBuf *buf)
{
    if ((!buf) || (BUF_ERROR(buf)))
        return NULL;

    return(&buf->content[buf->use]);
}

/**
 * Increase the size of the buffer content.
 *
 * If data was appended by writing directly into the content
 * array, this function increases the buffer size.
 *
 * @param buf  the buffer
 * @param len  number of bytes to add
 * @returns  0 on success, -1 in case of error
 */
int
xmlBufAddLen(xmlBuf *buf, size_t len) {
    if ((buf == NULL) || (BUF_ERROR(buf)) || (BUF_STATIC(buf)))
        return(-1);
    if (len > buf->size - buf->use)
        return(-1);
    buf->use += len;
    buf->content[buf->use] = 0;
    return(0);
}

/**
 * Return the size of the buffer content.
 *
 * @param buf  the buffer
 * @returns  size of buffer content in bytes
 */
size_t
xmlBufUse(xmlBuf *buf)
{
    if ((!buf) || (BUF_ERROR(buf)))
        return 0;

    return(buf->use);
}

/**
 * Return the size of available space at the end of a buffer.
 *
 * @param buf  the buffer
 * @returns  available space in bytes
 */
size_t
xmlBufAvail(xmlBuf *buf)
{
    if ((!buf) || (BUF_ERROR(buf)))
        return 0;

    return(buf->size - buf->use);
}

/**
 * Tell if a buffer is empty
 *
 * @param buf  the buffer
 * @returns  0 if no, 1 if yes and -1 in case of error
 */
int
xmlBufIsEmpty(xmlBuf *buf)
{
    if ((!buf) || (BUF_ERROR(buf)))
        return(-1);

    return(buf->use == 0);
}

/**
 * Append data to a buffer.
 *
 * If `len` is -1, `str` is expected to be zero-terminated.
 *
 * @param buf  the buffer
 * @param str  bytes to add
 * @param len  number of bytes
 * @returns  0 if successful, -1 in case of error.
 */
int
xmlBufAdd(xmlBuf *buf, const xmlChar *str, size_t len) {
    if ((buf == NULL) || (BUF_ERROR(buf)) || (BUF_STATIC(buf)))
        return(-1);
    if (len == 0)
        return(0);
    if (str == NULL)
	return(-1);

    if (len > buf->size - buf->use) {
        if (xmlBufGrowInternal(buf, len) < 0)
            return(-1);
    }

    memmove(&buf->content[buf->use], str, len);
    buf->use += len;
    buf->content[buf->use] = 0;

    return(0);
}

/**
 * Append a zero-terminated string to a buffer.
 *
 * @param buf  the buffer
 * @param str  string (optional)
 * @returns  0 if successful, -1 in case of error.
 */
int
xmlBufCat(xmlBuf *buf, const xmlChar *str) {
    if (str == NULL)
        return(0);
    return(xmlBufAdd(buf, str, strlen((const char *) str)));
}

/**
 * Helper routine to switch from the old buffer structures in use
 * in various APIs. It creates a wrapper xmlBuf which will be
 * used for internal processing until xmlBufBackToBuffer is
 * issued.
 *
 * @param buffer  incoming old buffer to convert to a new one
 * @returns  a new xmlBuf unless the call failed and NULL is returned
 */
xmlBuf *
xmlBufFromBuffer(xmlBuffer *buffer) {
    xmlBufPtr ret;

    if (buffer == NULL)
        return(NULL);

    ret = xmlMalloc(sizeof(xmlBuf));
    if (ret == NULL)
        return(NULL);

    ret->use = buffer->use;
    ret->flags = 0;
    ret->maxSize = SIZE_MAX - 1;

    if (buffer->content == NULL) {
        ret->size = 50;
        ret->mem = xmlMalloc(ret->size + 1);
        ret->content = ret->mem;
        if (ret->mem == NULL)
            xmlBufMemoryError(ret);
        else
            ret->content[0] = 0;
    } else {
        ret->size = buffer->size - 1;
        ret->content = buffer->content;
        if (buffer->alloc == XML_BUFFER_ALLOC_IO)
            ret->mem = buffer->contentIO;
        else
            ret->mem = buffer->content;
    }

    return(ret);
}

/**
 * Function to be called once internal processing had been done to
 * update back the buffer provided by the user. This can lead to
 * a failure in case the size accumulated in the xmlBuf is larger
 * than what an xmlBuffer can support on 64 bits (INT_MAX)
 * The xmlBuf `buf` wrapper is deallocated by this call in any case.
 *
 * @param buf  new buffer wrapping the old one
 * @param ret  old buffer
 * @returns  0 on success, -1 on error.
 */
int
xmlBufBackToBuffer(xmlBuf *buf, xmlBuffer *ret) {
    if ((buf == NULL) || (ret == NULL))
        return(-1);

    if ((BUF_ERROR(buf)) || (BUF_STATIC(buf)) ||
        (buf->use >= INT_MAX)) {
        xmlBufFree(buf);
        ret->content = NULL;
        ret->contentIO = NULL;
        ret->use = 0;
        ret->size = 0;
        return(-1);
    }

    ret->use = buf->use;
    if (buf->size >= INT_MAX) {
        /* Keep the buffer but provide a truncated size value. */
        ret->size = INT_MAX;
    } else {
        ret->size = buf->size + 1;
    }
    ret->alloc = XML_BUFFER_ALLOC_IO;
    ret->content = buf->content;
    ret->contentIO = buf->mem;
    xmlFree(buf);
    return(0);
}

/**
 * Update pointers in the parser input struct.
 *
 * Update `base`, `cur` and `end` to point into the buffer.
 * This is required after the content array was reallocated.
 *
 * @param buf  a buffer
 * @param input  a parser input
 * @returns  0 on success, -1 in case of error.
 */
int
xmlBufResetInput(xmlBuf *buf, xmlParserInput *input) {
    return(xmlBufUpdateInput(buf, input, 0));
}

/**
 * Update pointers in the parser input struct.
 *
 * Update `base`, `cur` and `end` to point into the buffer.
 * This is required after the content array was reallocated.
 *
 * @param buf  a buffer
 * @param input  a parser input
 * @param pos  the `cur` position relative to the start of the
 *             buffer
 * @returns  0 on success, -1 in case of error.
 */
int
xmlBufUpdateInput(xmlBuf *buf, xmlParserInput *input, size_t pos) {
    if ((buf == NULL) || (input == NULL))
        return(-1);
    input->base = buf->content;
    input->cur = input->base + pos;
    input->end = &buf->content[buf->use];
    return(0);
}

/************************************************************************
 *									*
 *			Old buffer implementation			*
 *									*
 ************************************************************************/

/**
 * Set the buffer allocation scheme.
 *
 * @deprecated No-op, allocation schemes were removed.
 *
 * @param scheme  allocation method to use
 */
void
xmlSetBufferAllocationScheme(xmlBufferAllocationScheme scheme ATTRIBUTE_UNUSED) {
}

/**
 * Get the buffer allocation scheme.
 *
 * @deprecated Allocation schemes were removed.
 *
 * @returns  the current allocation scheme
 */
xmlBufferAllocationScheme
xmlGetBufferAllocationScheme(void) {
    return(XML_BUFFER_ALLOC_EXACT);
}

/**
 * Create a buffer.
 *
 * The default initial size is 256.
 *
 * @returns  the new structure.
 */
xmlBuffer *
xmlBufferCreate(void) {
    xmlBufferPtr ret;

    ret = xmlMalloc(sizeof(*ret));
    if (ret == NULL)
        return(NULL);

    ret->use = 0;
    ret->size = 256;
    ret->alloc = XML_BUFFER_ALLOC_IO;
    ret->contentIO = xmlMalloc(ret->size);
    if (ret->contentIO == NULL) {
	xmlFree(ret);
        return(NULL);
    }
    ret->content = ret->contentIO;
    ret->content[0] = 0;

    return(ret);
}

/**
 * Create a buffer with an initial size.
 *
 * @param size  initial size of buffer
 * @returns  the new structure.
 */
xmlBuffer *
xmlBufferCreateSize(size_t size) {
    xmlBufferPtr ret;

    if (size >= INT_MAX)
        return(NULL);

    ret = xmlMalloc(sizeof(*ret));
    if (ret == NULL)
        return(NULL);

    ret->use = 0;
    ret->alloc = XML_BUFFER_ALLOC_IO;
    ret->size = (size ? size + 1 : 0);         /* +1 for ending null */

    if (ret->size) {
        ret->contentIO = xmlMalloc(ret->size);
        if (ret->contentIO == NULL) {
            xmlFree(ret);
            return(NULL);
        }
        ret->content = ret->contentIO;
        ret->content[0] = 0;
    } else {
        ret->contentIO = NULL;
	ret->content = NULL;
    }

    return(ret);
}

/**
 * Extract buffer content.
 *
 * Return the contents of the buffer as an `xmlChar` string,
 * clearing the buffer.
 *
 * This doesn't work with static buffers as they can't be reset.
 *
 * @param buf  the buffer
 * @returns  the buffer content
 */
xmlChar *
xmlBufferDetach(xmlBuffer *buf) {
    xmlChar *ret;

    if (buf == NULL)
        return(NULL);

    if ((buf->alloc == XML_BUFFER_ALLOC_IO) &&
        (buf->content != buf->contentIO)) {
        ret = xmlStrndup(buf->content, buf->use);
        xmlFree(buf->contentIO);
    } else {
        ret = buf->content;
    }

    buf->contentIO = NULL;
    buf->content = NULL;
    buf->size = 0;
    buf->use = 0;

    return ret;
}

/**
 * Create a static buffer.
 *
 * The memory must be zero-terminated and not be modified for the
 * lifetime of the buffer. A static buffer can't be grown, modified or
 * detached, but it can be shrunk.
 *
 * @param mem  the memory area
 * @param size  the size in bytes
 * @returns  a new buffer
 */
xmlBuffer *
xmlBufferCreateStatic(void *mem, size_t size) {
    xmlBufferPtr buf = xmlBufferCreateSize(size);

    xmlBufferAdd(buf, mem, size);
    return(buf);
}

/**
 * Set the allocation scheme of a buffer.
 *
 * For libxml2 before 2.14, it is recommended to set this to
 * XML_BUFFER_ALLOC_DOUBLE_IT. Has no effect on 2.14 or later.
 *
 * @param buf  the buffer to tune
 * @param scheme  allocation scheme to use
 */
void
xmlBufferSetAllocationScheme(xmlBuffer *buf ATTRIBUTE_UNUSED,
                             xmlBufferAllocationScheme scheme ATTRIBUTE_UNUSED) {
}

/**
 * Free a buffer.
 *
 * @param buf  the buffer to free
 */
void
xmlBufferFree(xmlBuffer *buf) {
    if (buf == NULL)
	return;

    if (buf->alloc == XML_BUFFER_ALLOC_IO)
        xmlFree(buf->contentIO);
    else
        xmlFree(buf->content);

    xmlFree(buf);
}

/**
 * Empty a buffer.
 *
 * @param buf  the buffer
 */
void
xmlBufferEmpty(xmlBuffer *buf) {
    if (buf == NULL)
        return;
    if (buf->content == NULL)
        return;

    buf->use = 0;

    if (buf->alloc == XML_BUFFER_ALLOC_IO) {
	buf->size += buf->content - buf->contentIO;
        buf->content = buf->contentIO;
        buf->content[0] = 0;
    } else {
        buf->content[0] = 0;
    }
}

/**
 * Discard bytes at the start of a buffer.
 *
 * @deprecated Internal function, don't use.
 *
 * @param buf  the buffer
 * @param len  the number of bytes to remove
 * @returns  the number of bytes removed, or -1 in case of failure.
 */
int
xmlBufferShrink(xmlBuffer *buf, unsigned int len) {
    if (buf == NULL)
        return(-1);
    if (len == 0)
        return(0);
    if (len > buf->use)
        return(-1);

    buf->use -= len;

    if (buf->alloc == XML_BUFFER_ALLOC_IO) {
        buf->content += len;
	buf->size -= len;
    } else {
	memmove(buf->content, &buf->content[len], buf->use + 1);
    }

    return(len);
}

/**
 * Grow a buffer.
 *
 * @deprecated Internal function, don't use.
 *
 * @param buf  the buffer
 * @param len  number of extra bytes to allocate
 * @returns  the new available space or -1 in case of error
 */
int
xmlBufferGrow(xmlBuffer *buf, unsigned int len) {
    unsigned int size;
    xmlChar *newbuf;

    if (buf == NULL)
        return(-1);

    if (len < buf->size - buf->use)
        return(0);
    if (len >= INT_MAX - buf->use)
        return(-1);

    if (buf->size > (size_t) len) {
        if (buf->size <= INT_MAX / 2)
            size = buf->size * 2;
        else
            size = INT_MAX;
    } else {
        size = buf->use + len + 1;
        if (size <= INT_MAX - 100)
            size += 100;
    }

    if ((buf->alloc == XML_BUFFER_ALLOC_IO) &&
        (buf->content != buf->contentIO)) {
        newbuf = xmlMalloc(size);
        if (newbuf == NULL)
            return(-1);
        if (buf->content != NULL)
            memcpy(newbuf, buf->content, buf->use + 1);
        xmlFree(buf->contentIO);
    } else {
        newbuf = xmlRealloc(buf->content, size);
        if (newbuf == NULL)
            return(-1);
    }

    if (buf->alloc == XML_BUFFER_ALLOC_IO)
        buf->contentIO = newbuf;
    buf->content = newbuf;
    buf->size = size;

    return(buf->size - buf->use - 1);
}

/**
 * Dump a buffer to a `FILE`.
 *
 * @param file  the output file
 * @param buf  the buffer
 * @returns  the number of bytes written
 */
int
xmlBufferDump(FILE *file, xmlBuffer *buf) {
    size_t ret;

    if (buf == NULL)
	return(0);
    if (buf->content == NULL)
	return(0);
    if (file == NULL)
	file = stdout;
    ret = fwrite(buf->content, 1, buf->use, file);
    return(ret > INT_MAX ? INT_MAX : ret);
}

/**
 * Get pointer into buffer content.
 *
 * @param buf  the buffer
 * @returns  the internal content
 */
const xmlChar *
xmlBufferContent(const xmlBuffer *buf)
{
    if(!buf)
        return NULL;

    return buf->content;
}

/**
 * Get the size of the buffer content.
 *
 * @param buf  the buffer
 * @returns  the size of the buffer content in bytes
 */
int
xmlBufferLength(const xmlBuffer *buf)
{
    if(!buf)
        return 0;

    return buf->use;
}

/**
 * Resize a buffer to a minimum size.
 *
 * @deprecated Internal function, don't use.
 *
 * @param buf  the buffer to resize
 * @param size  the desired size
 * @returns  1 on succes, 0 in case of error
 */
int
xmlBufferResize(xmlBuffer *buf, unsigned int size)
{
    int res;

    if (buf == NULL)
        return(0);
    if (size < buf->size)
        return(1);
    res = xmlBufferGrow(buf, size - buf->use);

    return(res < 0 ? 0 : 1);
}

/**
 * Append bytes to a buffer.
 *
 * If `len` is -1, `str` is assumed to be zero-terminated.
 *
 * @param buf  the buffer
 * @param str  bytes to add 
 * @param len  number of bytes
 * @returns  an xmlParserErrors code.
 */
int
xmlBufferAdd(xmlBuffer *buf, const xmlChar *str, int len) {
    if ((buf == NULL) || (str == NULL))
	return(XML_ERR_ARGUMENT);
    if (len < 0)
        len = xmlStrlen(str);
    if (len == 0)
        return(XML_ERR_OK);

    /* Note that both buf->size and buf->use can be zero here. */
    if ((unsigned) len >= buf->size - buf->use) {
        if (xmlBufferGrow(buf, len) < 0)
            return(XML_ERR_NO_MEMORY);
    }

    memmove(&buf->content[buf->use], str, len);
    buf->use += len;
    buf->content[buf->use] = 0;
    return(XML_ERR_OK);
}

/**
 * Prepend bytes to a buffer.
 *
 * If `len` is -1, `str` is assumed to be zero-terminated.
 *
 * @param buf  the buffer
 * @param str  bytes to prepend
 * @param len  number of bytes
 * @returns  an xmlParserErrors code.
 */
int
xmlBufferAddHead(xmlBuffer *buf, const xmlChar *str, int len) {
    unsigned start = 0;

    if ((buf == NULL) || (str == NULL))
	return(XML_ERR_ARGUMENT);
    if (len < 0)
        len = xmlStrlen(str);
    if (len == 0)
        return(XML_ERR_OK);

    if (buf->alloc == XML_BUFFER_ALLOC_IO) {
        start = buf->content - buf->contentIO;

        /*
         * We can add it in the space previously shrunk
         */
        if ((unsigned) len <= start) {
            buf->content -= len;
            memmove(&buf->content[0], str, len);
            buf->use += len;
            buf->size += len;
            return(0);
        }
        if ((unsigned) len < buf->size + start - buf->use) {
            memmove(&buf->contentIO[len], buf->content, buf->use + 1);
            memmove(buf->contentIO, str, len);
            buf->content = buf->contentIO;
            buf->use += len;
            buf->size += start;
            return(0);
        }
    }

    if ((unsigned) len >= buf->size - buf->use) {
        if (xmlBufferGrow(buf, len) < 0)
            return(-1);
    }

    memmove(&buf->content[len], buf->content, buf->use + 1);
    memmove(buf->content, str, len);
    buf->use += len;
    return (0);
}

/**
 * Append a zero-terminated string to a buffer.
 *
 * @param buf  the buffer
 * @param str  string to add
 * @returns  an xmlParserErrors code.
 */
int
xmlBufferCat(xmlBuffer *buf, const xmlChar *str) {
    return(xmlBufferAdd(buf, str, -1));
}

/**
 * Append a zero-terminated C string to a buffer.
 *
 * @param buf  the buffer
 * @param str  string to add
 * @returns  an xmlParserErrors code.
 */
int
xmlBufferCCat(xmlBuffer *buf, const char *str) {
    return(xmlBufferAdd(buf, (const xmlChar *) str, -1));
}

/**
 * Append a zero-terminated `xmlChar` string to a buffer.
 *
 * @param buf  the XML buffer
 * @param string  the string to add
 */
void
xmlBufferWriteCHAR(xmlBuffer *buf, const xmlChar *string) {
    xmlBufferAdd(buf, string, -1);
}

/**
 * Append a zero-terminated C string to a buffer.
 *
 * Same as #xmlBufferCCat.
 *
 * @param buf  the buffer
 * @param string  the string to add
 */
void
xmlBufferWriteChar(xmlBuffer *buf, const char *string) {
    xmlBufferAdd(buf, (const xmlChar *) string, -1);
}

/**
 * Append a quoted string to a buffer.
 *
 * Append a string quoted with single or double quotes. If the
 * string contains both single and double quotes, double quotes
 * are escaped with `&quot;`.
 *
 * @param buf  the buffer
 * @param string  the string to add
 */
void
xmlBufferWriteQuotedString(xmlBuffer *buf, const xmlChar *string) {
    const xmlChar *cur, *base;
    if (buf == NULL)
        return;
    if (xmlStrchr(string, '\"')) {
        if (xmlStrchr(string, '\'')) {
	    xmlBufferCCat(buf, "\"");
            base = cur = string;
            while(*cur != 0){
                if(*cur == '"'){
                    if (base != cur)
                        xmlBufferAdd(buf, base, cur - base);
                    xmlBufferAdd(buf, BAD_CAST "&quot;", 6);
                    cur++;
                    base = cur;
                }
                else {
                    cur++;
                }
            }
            if (base != cur)
                xmlBufferAdd(buf, base, cur - base);
	    xmlBufferCCat(buf, "\"");
	}
        else{
	    xmlBufferCCat(buf, "\'");
            xmlBufferCat(buf, string);
	    xmlBufferCCat(buf, "\'");
        }
    } else {
        xmlBufferCCat(buf, "\"");
        xmlBufferCat(buf, string);
        xmlBufferCCat(buf, "\"");
    }
}


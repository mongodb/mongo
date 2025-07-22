/*
 * librdkafka - Apache Kafka C library
 *
 * Copyright (c) 2017-2022, Magnus Edenhill
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef _RDBUF_H
#define _RDBUF_H

#ifndef _WIN32
/* for struct iovec */
#include <sys/socket.h>
#include <sys/types.h>
#endif

#include "rdsysqueue.h"


/**
 * @name Generic byte buffers
 *
 * @{
 *
 * A buffer is a list of segments, each segment having a memory pointer,
 * write offset, and capacity.
 *
 * The main buffer and segment structure is tailored for append-writing
 * or append-pushing foreign memory.
 *
 * Updates of previously written memory regions are possible through the
 * use of write_update() that takes an absolute offset.
 *
 * The write position is part of the buffer and segment structures, while
 * read is a separate object (rd_slice_t) that does not affect the buffer.
 */


/**
 * @brief Buffer segment
 */
typedef struct rd_segment_s {
        TAILQ_ENTRY(rd_segment_s) seg_link; /*<< rbuf_segments Link */
        char *seg_p;                        /**< Backing-store memory */
        size_t seg_of;                      /**< Current relative write-position
                                             *   (length of payload in this segment) */
        size_t seg_size;                    /**< Allocated size of seg_p */
        size_t seg_absof;          /**< Absolute offset of this segment's
                                    *   beginning in the grand rd_buf_t */
        void (*seg_free)(void *p); /**< Optional free function for seg_p */
        int seg_flags;             /**< Segment flags */
        size_t seg_erased;         /** Total number of bytes erased from
                                    *   this segment. */
#define RD_SEGMENT_F_RDONLY 0x1    /**< Read-only segment */
#define RD_SEGMENT_F_FREE                                                      \
        0x2 /**< Free segment on destroy,                                      \
             *   e.g, not a fixed segment. */
} rd_segment_t;



TAILQ_HEAD(rd_segment_head, rd_segment_s);

/**
 * @brief Buffer, containing a list of segments.
 */
typedef struct rd_buf_s {
        struct rd_segment_head rbuf_segments; /**< TAILQ list of segments */
        size_t rbuf_segment_cnt;              /**< Number of segments */

        rd_segment_t *rbuf_wpos; /**< Current write position seg */
        size_t rbuf_len;         /**< Current (written) length */
        size_t rbuf_erased;      /**< Total number of bytes
                                  *   erased from segments.
                                  *   This amount is taken into
                                  *   account when checking for
                                  *   writable space which is
                                  *   always at the end of the
                                  *   buffer and thus can't make
                                  *   use of the erased parts. */
        size_t rbuf_size;        /**< Total allocated size of
                                  *   all segments. */

        char *rbuf_extra;       /* Extra memory allocated for
                                 * use by segment structs,
                                 * buffer memory, etc. */
        size_t rbuf_extra_len;  /* Current extra memory used */
        size_t rbuf_extra_size; /* Total size of extra memory */
} rd_buf_t;



/**
 * @brief A read-only slice of a buffer.
 */
typedef struct rd_slice_s {
        const rd_buf_t *buf;     /**< Pointer to buffer */
        const rd_segment_t *seg; /**< Current read position segment.
                                  *   Will point to NULL when end of
                                  *   slice is reached. */
        size_t rof;              /**< Relative read offset in segment */
        size_t start;            /**< Slice start offset in buffer */
        size_t end;              /**< Slice end offset in buffer+1 */
} rd_slice_t;



/**
 * @returns the current write position (absolute offset)
 */
static RD_INLINE RD_UNUSED size_t rd_buf_write_pos(const rd_buf_t *rbuf) {
        const rd_segment_t *seg = rbuf->rbuf_wpos;

        if (unlikely(!seg)) {
#if ENABLE_DEVEL
                rd_assert(rbuf->rbuf_len == 0);
#endif
                return 0;
        }
#if ENABLE_DEVEL
        rd_assert(seg->seg_absof + seg->seg_of == rbuf->rbuf_len);
#endif
        return seg->seg_absof + seg->seg_of;
}


/**
 * @returns the number of bytes available for writing (before growing).
 */
static RD_INLINE RD_UNUSED size_t rd_buf_write_remains(const rd_buf_t *rbuf) {
        return rbuf->rbuf_size - (rbuf->rbuf_len + rbuf->rbuf_erased);
}



/**
 * @returns the number of bytes remaining to write to the given segment,
 *          and sets the \p *p pointer (unless NULL) to the start of
 *          the contiguous memory.
 */
static RD_INLINE RD_UNUSED size_t
rd_segment_write_remains(const rd_segment_t *seg, void **p) {
        if (unlikely((seg->seg_flags & RD_SEGMENT_F_RDONLY)))
                return 0;
        if (p)
                *p = (void *)(seg->seg_p + seg->seg_of);
        return seg->seg_size - seg->seg_of;
}



/**
 * @returns the last segment for the buffer.
 */
static RD_INLINE RD_UNUSED rd_segment_t *rd_buf_last(const rd_buf_t *rbuf) {
        return TAILQ_LAST(&rbuf->rbuf_segments, rd_segment_head);
}


/**
 * @returns the total written buffer length
 */
static RD_INLINE RD_UNUSED size_t rd_buf_len(const rd_buf_t *rbuf) {
        return rbuf->rbuf_len;
}


int rd_buf_write_seek(rd_buf_t *rbuf, size_t absof);


size_t rd_buf_write(rd_buf_t *rbuf, const void *payload, size_t size);
size_t rd_buf_write_slice(rd_buf_t *rbuf, rd_slice_t *slice);
size_t rd_buf_write_update(rd_buf_t *rbuf,
                           size_t absof,
                           const void *payload,
                           size_t size);
void rd_buf_push0(rd_buf_t *rbuf,
                  const void *payload,
                  size_t size,
                  void (*free_cb)(void *),
                  rd_bool_t writable);
#define rd_buf_push(rbuf, payload, size, free_cb)                              \
        rd_buf_push0(rbuf, payload, size, free_cb, rd_false /*not-writable*/)
#define rd_buf_push_writable(rbuf, payload, size, free_cb)                     \
        rd_buf_push0(rbuf, payload, size, free_cb, rd_true /*writable*/)

size_t rd_buf_erase(rd_buf_t *rbuf, size_t absof, size_t size);

size_t rd_buf_get_writable(rd_buf_t *rbuf, void **p);

void rd_buf_write_ensure_contig(rd_buf_t *rbuf, size_t size);

void rd_buf_write_ensure(rd_buf_t *rbuf, size_t min_size, size_t max_size);

size_t rd_buf_get_write_iov(const rd_buf_t *rbuf,
                            struct iovec *iovs,
                            size_t *iovcntp,
                            size_t iov_max,
                            size_t size_max);

void rd_buf_init(rd_buf_t *rbuf, size_t fixed_seg_cnt, size_t buf_size);
rd_buf_t *rd_buf_new(size_t fixed_seg_cnt, size_t buf_size);

void rd_buf_destroy(rd_buf_t *rbuf);
void rd_buf_destroy_free(rd_buf_t *rbuf);

void rd_buf_dump(const rd_buf_t *rbuf, int do_hexdump);

int unittest_rdbuf(void);


/**@}*/



/**
 * @name Buffer reads operate on slices of an rd_buf_t and does not
 *       modify the underlying rd_buf_t itself.
 *
 * @warning A slice will not be valid/safe after the buffer or
 *          segments have been modified by a buf write operation
 *          (write, update, write_seek, etc).
 * @{
 */


/**
 * @returns the remaining length in the slice
 */
#define rd_slice_remains(slice) ((slice)->end - rd_slice_abs_offset(slice))

/**
 * @returns the total size of the slice, regardless of current position.
 */
#define rd_slice_size(slice) ((slice)->end - (slice)->start)

/**
 * @returns the read position in the slice as a new slice.
 */
static RD_INLINE RD_UNUSED rd_slice_t rd_slice_pos(const rd_slice_t *slice) {
        rd_slice_t newslice = *slice;

        if (!slice->seg)
                return newslice;

        newslice.start = slice->seg->seg_absof + slice->rof;

        return newslice;
}

/**
 * @returns the read position as an absolute buffer byte offset.
 * @remark this is the buffer offset, not the slice's local offset.
 */
static RD_INLINE RD_UNUSED size_t rd_slice_abs_offset(const rd_slice_t *slice) {
        if (unlikely(!slice->seg)) /* reader has reached the end */
                return slice->end;

        return slice->seg->seg_absof + slice->rof;
}

/**
 * @returns the read position as a byte offset.
 * @remark this is the slice-local offset, not the backing buffer's offset.
 */
static RD_INLINE RD_UNUSED size_t rd_slice_offset(const rd_slice_t *slice) {
        if (unlikely(!slice->seg)) /* reader has reached the end */
                return rd_slice_size(slice);

        return (slice->seg->seg_absof + slice->rof) - slice->start;
}



int rd_slice_init_seg(rd_slice_t *slice,
                      const rd_buf_t *rbuf,
                      const rd_segment_t *seg,
                      size_t rof,
                      size_t size);
int rd_slice_init(rd_slice_t *slice,
                  const rd_buf_t *rbuf,
                  size_t absof,
                  size_t size);
void rd_slice_init_full(rd_slice_t *slice, const rd_buf_t *rbuf);

size_t rd_slice_reader(rd_slice_t *slice, const void **p);
size_t rd_slice_peeker(const rd_slice_t *slice, const void **p);

size_t rd_slice_read(rd_slice_t *slice, void *dst, size_t size);
size_t
rd_slice_peek(const rd_slice_t *slice, size_t offset, void *dst, size_t size);

size_t rd_slice_read_uvarint(rd_slice_t *slice, uint64_t *nump);

/**
 * @brief Read a zig-zag varint-encoded signed integer from \p slice,
 *        storing the decoded number in \p nump on success (return value > 0).
 *
 * @returns the number of bytes read on success or 0 in case of
 *          buffer underflow.
 */
static RD_UNUSED RD_INLINE size_t rd_slice_read_varint(rd_slice_t *slice,
                                                       int64_t *nump) {
        size_t r;
        uint64_t unum;

        r = rd_slice_read_uvarint(slice, &unum);
        if (likely(r > 0)) {
                /* Zig-zag decoding */
                *nump = (int64_t)((unum >> 1) ^ -(int64_t)(unum & 1));
        }

        return r;
}



const void *rd_slice_ensure_contig(rd_slice_t *slice, size_t size);

int rd_slice_seek(rd_slice_t *slice, size_t offset);

size_t rd_slice_get_iov(const rd_slice_t *slice,
                        struct iovec *iovs,
                        size_t *iovcntp,
                        size_t iov_max,
                        size_t size_max);


uint32_t rd_slice_crc32(rd_slice_t *slice);
uint32_t rd_slice_crc32c(rd_slice_t *slice);


int rd_slice_narrow(rd_slice_t *slice,
                    rd_slice_t *save_slice,
                    size_t size) RD_WARN_UNUSED_RESULT;
int rd_slice_narrow_relative(rd_slice_t *slice,
                             rd_slice_t *save_slice,
                             size_t relsize) RD_WARN_UNUSED_RESULT;
void rd_slice_widen(rd_slice_t *slice, const rd_slice_t *save_slice);
int rd_slice_narrow_copy(const rd_slice_t *orig,
                         rd_slice_t *new_slice,
                         size_t size) RD_WARN_UNUSED_RESULT;
int rd_slice_narrow_copy_relative(const rd_slice_t *orig,
                                  rd_slice_t *new_slice,
                                  size_t relsize) RD_WARN_UNUSED_RESULT;

void rd_slice_dump(const rd_slice_t *slice, int do_hexdump);


/**@}*/



#endif /* _RDBUF_H */

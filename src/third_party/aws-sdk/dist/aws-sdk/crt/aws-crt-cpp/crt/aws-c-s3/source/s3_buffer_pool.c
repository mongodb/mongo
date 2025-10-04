/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/private/s3_buffer_pool.h>

#include <aws/common/array_list.h>
#include <aws/common/mutex.h>
#include <aws/s3/private/s3_util.h>

/*
 * S3 Buffer Pool.
 * Fairly trivial implementation of "arena" style allocator.
 * Note: current implementation is not optimized and instead tries to be
 * as straightforward as possible. Given that pool manages a small number
 * of big allocations, performance impact is not that bad, but something we need
 * to look into on the next iteration.
 *
 * Basic approach is to divide acquires into primary and secondary.
 * User provides chunk size during construction. Acquires below 4 * chunks_size
 * are done from primary and the rest are from secondary.
 *
 * Primary storage consists of blocks that are each s_chunks_per_block *
 * chunk_size in size. blocks are created on demand as needed.
 * Acquire operation from primary basically works by determining how many chunks
 * are needed and then finding available space in existing blocks or creating a
 * new block. Acquire will always take over the whole chunk, so some space is
 * likely wasted.
 * Ex. say chunk_size is 8mb and s_chunks_per_block is 16, which makes block size 128mb.
 * acquires up to 32mb will be done from primary. So 1 block can hold 4 buffers
 * of 32mb (4 chunks) or 16 buffers of 8mb (1 chunk). If requested buffer size
 * is 12mb, 2 chunks are used for acquire and 4mb will be wasted.
 * Secondary storage delegates directly to system allocator.
 *
 * One complication is "forced" buffers. A forced buffer is one that
 * comes from primary or secondary storage as usual, but it is allowed to exceed
 * the memory limit. This is only used when we want to use memory from
 * the pool, but waiting for a normal ticket reservation could cause deadlock.
 */

struct aws_s3_buffer_pool_ticket {
    size_t size;
    uint8_t *ptr;
    size_t chunks_used;
    bool forced;
};

/* Default size for blocks array. Note: this is just for meta info, blocks
 * themselves are not preallocated. */
static size_t s_block_list_initial_capacity = 5;

/* Amount of mem reserved for use outside of buffer pool.
 * This is an optimistic upper bound on mem used as we dont track it.
 * Covers both usage outside of pool, i.e. all allocations done as part of s3
 * client as well as any allocations overruns due to memory waste in the pool. */
static const size_t s_buffer_pool_reserved_mem = MB_TO_BYTES(128);

/*
 * How many chunks make up a block in primary storage.
 */
static const size_t s_chunks_per_block = 16;

/*
 * Max size of chunks in primary.
 * Effectively if client part size is above the following number, primary
 * storage along with buffer reuse is disabled and all buffers are allocated
 * directly using allocator.
 */
static const size_t s_max_chunk_size_for_buffer_reuse = MB_TO_BYTES(64);

/* Forced buffers only count against the memory limit up to a certain percent.
 * For example: if mem_limit is 10GiB, and forced_use is 11GiB, and THIS number is 90(%),
 * we still consider 1GiB available for normal buffer usage. */
static const size_t s_max_impact_of_forced_buffers_on_memory_limit_as_percentage = 80;

struct aws_s3_buffer_pool {
    struct aws_allocator *base_allocator;
    struct aws_mutex mutex;

    size_t block_size;
    size_t chunk_size;
    /* size at which allocations should go to secondary */
    size_t primary_size_cutoff;

    /* NOTE: See aws_s3_buffer_pool_usage_stats for descriptions of most fields */

    size_t mem_limit;

    bool has_reservation_hold;

    size_t primary_allocated;
    size_t primary_reserved;
    size_t primary_used;

    size_t secondary_reserved;
    size_t secondary_used;

    size_t forced_used;

    struct aws_array_list blocks;
};

struct s3_buffer_pool_block {
    size_t block_size;
    uint8_t *block_ptr;
    uint16_t alloc_bit_mask;
};

/*
 * Sets n bits at position starting with LSB.
 * Note: n must be at most 8, but in practice will always be at most 4.
 * position + n should at most be 16
 */
static inline uint16_t s_set_bits(uint16_t num, size_t position, size_t n) {
    AWS_PRECONDITION(n <= 8);
    AWS_PRECONDITION(position + n <= 16);
    uint16_t mask = ((uint16_t)0x00FF) >> (8 - n);
    return num | (mask << position);
}

/*
 * Clears n bits at position starting with LSB.
 * Note: n must be at most 8, but in practice will always be at most 4.
 * position + n should at most be 16
 */
static inline uint16_t s_clear_bits(uint16_t num, size_t position, size_t n) {
    AWS_PRECONDITION(n <= 8);
    AWS_PRECONDITION(position + n <= 16);
    uint16_t mask = ((uint16_t)0x00FF) >> (8 - n);
    return num & ~(mask << position);
}

/*
 * Checks whether n bits are set at position starting with LSB.
 * Note: n must be at most 8, but in practice will always be at most 4.
 * position + n should at most be 16
 */
static inline bool s_check_bits(uint16_t num, size_t position, size_t n) {
    AWS_PRECONDITION(n <= 8);
    AWS_PRECONDITION(position + n <= 16);
    uint16_t mask = ((uint16_t)0x00FF) >> (8 - n);
    return (num >> position) & mask;
}

struct aws_s3_buffer_pool *aws_s3_buffer_pool_new(
    struct aws_allocator *allocator,
    size_t chunk_size,
    size_t mem_limit) {

    if (mem_limit < GB_TO_BYTES(1)) {
        AWS_LOGF_ERROR(
            AWS_LS_S3_CLIENT,
            "Failed to initialize buffer pool. "
            "Minimum supported value for Memory Limit is 1GB.");
        aws_raise_error(AWS_ERROR_S3_INVALID_MEMORY_LIMIT_CONFIG);
        return NULL;
    }

    if (chunk_size < (1024) || chunk_size % (4 * 1024) != 0) {
        AWS_LOGF_WARN(
            AWS_LS_S3_CLIENT,
            "Part size specified on the client can lead to suboptimal performance. "
            "Consider specifying size in multiples of 4KiB. Ideal part size for most transfers is "
            "1MiB multiple between 8MiB and 16MiB. Note: the client will automatically scale part size "
            "if its not sufficient to transfer data within the maximum number of parts");
    }

    size_t adjusted_mem_lim = mem_limit - s_buffer_pool_reserved_mem;

    /*
     * TODO: There is several things we can consider tweaking here:
     * - if chunk size is a weird number of bytes, force it to the closest page size?
     * - grow chunk size max based on overall mem lim (ex. for 4gb it might be
     *   64mb, but for 8gb it can be 128mb)
     * - align chunk size to better fill available mem? some chunk sizes can
     *   result in memory being wasted because overall limit does not divide
     *   nicely into chunks
     */
    if (chunk_size > s_max_chunk_size_for_buffer_reuse || chunk_size * s_chunks_per_block > adjusted_mem_lim) {
        AWS_LOGF_WARN(
            AWS_LS_S3_CLIENT,
            "Part size specified on the client is too large for automatic buffer reuse. "
            "Consider specifying a smaller part size to improve performance and memory utilization");
        chunk_size = 0;
    }

    struct aws_s3_buffer_pool *buffer_pool = aws_mem_calloc(allocator, 1, sizeof(struct aws_s3_buffer_pool));

    AWS_FATAL_ASSERT(buffer_pool != NULL);

    buffer_pool->base_allocator = allocator;
    buffer_pool->chunk_size = chunk_size;
    buffer_pool->block_size = s_chunks_per_block * chunk_size;
    /* Somewhat arbitrary number.
     * Tries to balance between how many allocations use buffer and buffer space
     * being wasted. */
    buffer_pool->primary_size_cutoff = chunk_size * 4;
    buffer_pool->mem_limit = adjusted_mem_lim;
    int mutex_error = aws_mutex_init(&buffer_pool->mutex);
    AWS_FATAL_ASSERT(mutex_error == AWS_OP_SUCCESS);

    aws_array_list_init_dynamic(
        &buffer_pool->blocks, allocator, s_block_list_initial_capacity, sizeof(struct s3_buffer_pool_block));

    return buffer_pool;
}

void aws_s3_buffer_pool_destroy(struct aws_s3_buffer_pool *buffer_pool) {
    if (buffer_pool == NULL) {
        return;
    }

    for (size_t i = 0; i < aws_array_list_length(&buffer_pool->blocks); ++i) {
        struct s3_buffer_pool_block *block;
        aws_array_list_get_at_ptr(&buffer_pool->blocks, (void **)&block, i);

        AWS_FATAL_ASSERT(block->alloc_bit_mask == 0 && "Allocator still has outstanding blocks");
        aws_mem_release(buffer_pool->base_allocator, block->block_ptr);
    }

    aws_array_list_clean_up(&buffer_pool->blocks);

    aws_mutex_clean_up(&buffer_pool->mutex);
    struct aws_allocator *base = buffer_pool->base_allocator;
    aws_mem_release(base, buffer_pool);
}

void s_buffer_pool_trim_synced(struct aws_s3_buffer_pool *buffer_pool) {
    for (size_t i = 0; i < aws_array_list_length(&buffer_pool->blocks);) {
        struct s3_buffer_pool_block *block;
        aws_array_list_get_at_ptr(&buffer_pool->blocks, (void **)&block, i);

        if (block->alloc_bit_mask == 0) {
            buffer_pool->primary_allocated -= block->block_size;
            aws_mem_release(buffer_pool->base_allocator, block->block_ptr);
            aws_array_list_erase(&buffer_pool->blocks, i);
            /* do not increment since we just released element */
        } else {
            ++i;
        }
    }
}

void aws_s3_buffer_pool_trim(struct aws_s3_buffer_pool *buffer_pool) {
    aws_mutex_lock(&buffer_pool->mutex);
    s_buffer_pool_trim_synced(buffer_pool);
    aws_mutex_unlock(&buffer_pool->mutex);
}

struct aws_s3_buffer_pool_ticket *aws_s3_buffer_pool_reserve(struct aws_s3_buffer_pool *buffer_pool, size_t size) {
    AWS_PRECONDITION(buffer_pool);

    if (buffer_pool->has_reservation_hold) {
        return NULL;
    }

    AWS_FATAL_ASSERT(size != 0);
    AWS_FATAL_ASSERT(size <= buffer_pool->mem_limit);

    struct aws_s3_buffer_pool_ticket *ticket = NULL;
    aws_mutex_lock(&buffer_pool->mutex);

    size_t overall_taken = buffer_pool->primary_used + buffer_pool->primary_reserved + buffer_pool->secondary_used +
                           buffer_pool->secondary_reserved;

    /*
     * If we are allocating from secondary and there is unused space in
     * primary, trim the primary in hopes we can free up enough memory.
     * TODO: something smarter, like partial trim?
     */
    if (size > buffer_pool->primary_size_cutoff && (size + overall_taken) > buffer_pool->mem_limit &&
        (buffer_pool->primary_allocated >
         (buffer_pool->primary_used + buffer_pool->primary_reserved + buffer_pool->block_size))) {
        s_buffer_pool_trim_synced(buffer_pool);
        overall_taken = buffer_pool->primary_used + buffer_pool->primary_reserved + buffer_pool->secondary_used +
                        buffer_pool->secondary_reserved;
    }

    /* Don't let forced buffers account for 100% of the memory limit */
    const size_t max_impact_of_forced_on_limit =
        (size_t)(buffer_pool->mem_limit * (s_max_impact_of_forced_buffers_on_memory_limit_as_percentage / 100.0));
    if (buffer_pool->forced_used > max_impact_of_forced_on_limit) {
        overall_taken -= buffer_pool->forced_used - max_impact_of_forced_on_limit;
    }

    if ((size + overall_taken) <= buffer_pool->mem_limit) {
        ticket = aws_mem_calloc(buffer_pool->base_allocator, 1, sizeof(struct aws_s3_buffer_pool_ticket));
        ticket->size = size;
        if (size <= buffer_pool->primary_size_cutoff) {
            buffer_pool->primary_reserved += size;
        } else {
            buffer_pool->secondary_reserved += size;
        }
    } else {
        buffer_pool->has_reservation_hold = true;
    }

    aws_mutex_unlock(&buffer_pool->mutex);

    if (ticket == NULL) {
        AWS_LOGF_TRACE(
            AWS_LS_S3_CLIENT,
            "Memory limit reached while trying to allocate buffer of size %zu. "
            "Putting new buffer reservations on hold...",
            size);
        aws_raise_error(AWS_ERROR_S3_EXCEEDS_MEMORY_LIMIT);
    }
    return ticket;
}

bool aws_s3_buffer_pool_has_reservation_hold(struct aws_s3_buffer_pool *buffer_pool) {
    AWS_PRECONDITION(buffer_pool);
    return buffer_pool->has_reservation_hold;
}

void aws_s3_buffer_pool_remove_reservation_hold(struct aws_s3_buffer_pool *buffer_pool) {
    AWS_PRECONDITION(buffer_pool);
    AWS_LOGF_TRACE(AWS_LS_S3_CLIENT, "Releasing buffer reservation hold.");
    buffer_pool->has_reservation_hold = false;
}

static uint8_t *s_primary_acquire_synced(
    struct aws_s3_buffer_pool *buffer_pool,
    struct aws_s3_buffer_pool_ticket *ticket) {

    uint8_t *alloc_ptr = NULL;

    size_t chunks_needed = ticket->size / buffer_pool->chunk_size;
    if (ticket->size % buffer_pool->chunk_size != 0) {
        ++chunks_needed; /* round up */
    }
    ticket->chunks_used = chunks_needed;

    /* Look for space in existing blocks */
    for (size_t i = 0; i < aws_array_list_length(&buffer_pool->blocks); ++i) {
        struct s3_buffer_pool_block *block;
        aws_array_list_get_at_ptr(&buffer_pool->blocks, (void **)&block, i);

        for (size_t chunk_i = 0; chunk_i < s_chunks_per_block - chunks_needed + 1; ++chunk_i) {
            if (!s_check_bits(block->alloc_bit_mask, chunk_i, chunks_needed)) {
                alloc_ptr = block->block_ptr + chunk_i * buffer_pool->chunk_size;
                block->alloc_bit_mask = s_set_bits(block->alloc_bit_mask, chunk_i, chunks_needed);
                goto on_allocated;
            }
        }
    }

    /* No space available. Allocate new block. */
    struct s3_buffer_pool_block block;
    block.alloc_bit_mask = s_set_bits(0, 0, chunks_needed);
    block.block_ptr = aws_mem_acquire(buffer_pool->base_allocator, buffer_pool->block_size);
    block.block_size = buffer_pool->block_size;
    aws_array_list_push_back(&buffer_pool->blocks, &block);
    alloc_ptr = block.block_ptr;

    buffer_pool->primary_allocated += buffer_pool->block_size;

on_allocated:
    buffer_pool->primary_used += ticket->size;

    /* forced buffers acquire immediately, without reserving first */
    if (ticket->forced == false) {
        buffer_pool->primary_reserved -= ticket->size;
    }

    return alloc_ptr;
}

static struct aws_byte_buf s_acquire_buffer_synced(
    struct aws_s3_buffer_pool *buffer_pool,
    struct aws_s3_buffer_pool_ticket *ticket);

struct aws_byte_buf aws_s3_buffer_pool_acquire_buffer(
    struct aws_s3_buffer_pool *buffer_pool,
    struct aws_s3_buffer_pool_ticket *ticket) {

    AWS_PRECONDITION(buffer_pool);
    AWS_PRECONDITION(ticket);

    if (ticket->ptr != NULL) {
        return aws_byte_buf_from_empty_array(ticket->ptr, ticket->size);
    }

    aws_mutex_lock(&buffer_pool->mutex);
    struct aws_byte_buf buf = s_acquire_buffer_synced(buffer_pool, ticket);
    aws_mutex_unlock(&buffer_pool->mutex);

    return buf;
}

static struct aws_byte_buf s_acquire_buffer_synced(
    struct aws_s3_buffer_pool *buffer_pool,
    struct aws_s3_buffer_pool_ticket *ticket) {

    AWS_PRECONDITION(ticket->ptr == NULL);

    if (ticket->size <= buffer_pool->primary_size_cutoff) {
        ticket->ptr = s_primary_acquire_synced(buffer_pool, ticket);
    } else {
        ticket->ptr = aws_mem_acquire(buffer_pool->base_allocator, ticket->size);
        buffer_pool->secondary_used += ticket->size;

        /* forced buffers acquire immediately, without reserving first */
        if (ticket->forced == false) {
            buffer_pool->secondary_reserved -= ticket->size;
        }
    }

    return aws_byte_buf_from_empty_array(ticket->ptr, ticket->size);
}

struct aws_byte_buf aws_s3_buffer_pool_acquire_forced_buffer(
    struct aws_s3_buffer_pool *buffer_pool,
    size_t size,
    struct aws_s3_buffer_pool_ticket **out_new_ticket) {

    AWS_PRECONDITION(buffer_pool);
    AWS_PRECONDITION(out_new_ticket);

    AWS_FATAL_ASSERT(size != 0);

    aws_mutex_lock(&buffer_pool->mutex);

    struct aws_s3_buffer_pool_ticket *ticket =
        aws_mem_calloc(buffer_pool->base_allocator, 1, sizeof(struct aws_s3_buffer_pool_ticket));
    ticket->size = size;
    ticket->forced = true;

    struct aws_byte_buf buf = s_acquire_buffer_synced(buffer_pool, ticket);

    buffer_pool->forced_used += size;

    aws_mutex_unlock(&buffer_pool->mutex);

    *out_new_ticket = ticket;
    return buf;
}

void aws_s3_buffer_pool_release_ticket(
    struct aws_s3_buffer_pool *buffer_pool,
    struct aws_s3_buffer_pool_ticket *ticket) {

    if (buffer_pool == NULL || ticket == NULL) {
        return;
    }

    if (ticket->ptr == NULL) {
        /* Ticket was never used, make sure to clean up reserved count. */
        aws_mutex_lock(&buffer_pool->mutex);
        if (ticket->size <= buffer_pool->primary_size_cutoff) {
            buffer_pool->primary_reserved -= ticket->size;
        } else {
            buffer_pool->secondary_reserved -= ticket->size;
        }
        aws_mutex_unlock(&buffer_pool->mutex);
        aws_mem_release(buffer_pool->base_allocator, ticket);
        return;
    }

    aws_mutex_lock(&buffer_pool->mutex);
    if (ticket->size <= buffer_pool->primary_size_cutoff) {

        size_t chunks_used = ticket->size / buffer_pool->chunk_size;
        if (ticket->size % buffer_pool->chunk_size != 0) {
            ++chunks_used; /* round up */
        }

        bool found = false;
        for (size_t i = 0; i < aws_array_list_length(&buffer_pool->blocks); ++i) {
            struct s3_buffer_pool_block *block;
            aws_array_list_get_at_ptr(&buffer_pool->blocks, (void **)&block, i);

            if (block->block_ptr <= ticket->ptr && block->block_ptr + block->block_size > ticket->ptr) {
                size_t alloc_i = (ticket->ptr - block->block_ptr) / buffer_pool->chunk_size;

                block->alloc_bit_mask = s_clear_bits(block->alloc_bit_mask, alloc_i, chunks_used);
                buffer_pool->primary_used -= ticket->size;

                found = true;
                break;
            }
        }

        AWS_FATAL_ASSERT(found);
    } else {
        aws_mem_release(buffer_pool->base_allocator, ticket->ptr);
        buffer_pool->secondary_used -= ticket->size;
    }

    if (ticket->forced) {
        buffer_pool->forced_used -= ticket->size;
    }

    aws_mem_release(buffer_pool->base_allocator, ticket);

    aws_mutex_unlock(&buffer_pool->mutex);
}

struct aws_s3_buffer_pool_usage_stats aws_s3_buffer_pool_get_usage(struct aws_s3_buffer_pool *buffer_pool) {
    aws_mutex_lock(&buffer_pool->mutex);

    struct aws_s3_buffer_pool_usage_stats ret = (struct aws_s3_buffer_pool_usage_stats){
        .mem_limit = buffer_pool->mem_limit,
        .primary_cutoff = buffer_pool->primary_size_cutoff,
        .primary_allocated = buffer_pool->primary_allocated,
        .primary_used = buffer_pool->primary_used,
        .primary_reserved = buffer_pool->primary_reserved,
        .primary_num_blocks = aws_array_list_length(&buffer_pool->blocks),
        .secondary_used = buffer_pool->secondary_used,
        .secondary_reserved = buffer_pool->secondary_reserved,
        .forced_used = buffer_pool->forced_used,
    };

    aws_mutex_unlock(&buffer_pool->mutex);
    return ret;
}

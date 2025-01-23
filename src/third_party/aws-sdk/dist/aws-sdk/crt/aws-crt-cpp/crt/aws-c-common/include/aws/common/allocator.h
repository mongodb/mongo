#ifndef AWS_COMMON_ALLOCATOR_H
#define AWS_COMMON_ALLOCATOR_H
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/macros.h>
#include <aws/common/stdbool.h>
#include <aws/common/stdint.h>

AWS_PUSH_SANE_WARNING_LEVEL
AWS_EXTERN_C_BEGIN

/*
 * Quick guide to allocators:
 *  CRT offers several flavours of allocators:
 * - default: basic allocator that invokes system one directly.
 * - aligned: basic allocator that aligns small allocations on 8 byte
 *   boundary and big buffers on 32/64 byte (system dependent) boundary.
 *   Aligned mem can improve perf on some operations, like memcpy or hashes.
 *   Depending on a system, can result in higher peak memory count in heavy
 *   acquire/free scenarios (ex. s3), due to memory fragmentation related to how
 *   aligned allocators work (over allocate, find aligned offset, release extra memory)
 * - wrapped_cf: wraps MacOS's Security Framework allocator.
 * - mem_tracer: wraps any allocator and provides tracing functionality to allocations
 * - small_block_allocator: pools smaller allocations into preallocated buckets.
 *   Not actively maintained. Avoid if possible.
 */

/* Allocator structure. An instance of this will be passed around for anything needing memory allocation */
struct aws_allocator {
    void *(*mem_acquire)(struct aws_allocator *allocator, size_t size);
    void (*mem_release)(struct aws_allocator *allocator, void *ptr);
    /* Optional method; if not supported, this pointer must be NULL */
    void *(*mem_realloc)(struct aws_allocator *allocator, void *oldptr, size_t oldsize, size_t newsize);
    /* Optional method; if not supported, this pointer must be NULL */
    void *(*mem_calloc)(struct aws_allocator *allocator, size_t num, size_t size);
    void *impl;
};

/**
 * Inexpensive (constant time) check of data-structure invariants.
 */
AWS_COMMON_API
bool aws_allocator_is_valid(const struct aws_allocator *alloc);

AWS_COMMON_API
struct aws_allocator *aws_default_allocator(void);

/*
 * Allocator that align small allocations on 8 byte boundary and big allocations
 * on 32/64 byte boundary.
 */
AWS_COMMON_API
struct aws_allocator *aws_aligned_allocator(void);

#ifdef __MACH__
/* Avoid pulling in CoreFoundation headers in a header file. */
struct __CFAllocator; /* NOLINT(bugprone-reserved-identifier) */
typedef const struct __CFAllocator *CFAllocatorRef;

/**
 * Wraps a CFAllocator around aws_allocator. For Mac only. Use this anytime you need a CFAllocatorRef for interacting
 * with Apple Frameworks. Unfortunately, it allocates memory so we can't make it static file scope, be sure to call
 * aws_wrapped_cf_allocator_destroy when finished.
 */
AWS_COMMON_API
CFAllocatorRef aws_wrapped_cf_allocator_new(struct aws_allocator *allocator);

/**
 * Cleans up any resources alloced in aws_wrapped_cf_allocator_new.
 */
AWS_COMMON_API
void aws_wrapped_cf_allocator_destroy(CFAllocatorRef allocator);
#endif

/**
 * Returns at least `size` of memory ready for usage. In versions v0.6.8 and prior, this function was allowed to return
 * NULL. In later versions, if allocator->mem_acquire() returns NULL, this function will assert and exit. To handle
 * conditions where OOM is not a fatal error, allocator->mem_acquire() is responsible for finding/reclaiming/running a
 * GC etc...before returning.
 */
AWS_COMMON_API
void *aws_mem_acquire(struct aws_allocator *allocator, size_t size);

/**
 * Allocates a block of memory for an array of num elements, each of them size bytes long, and initializes all its bits
 * to zero. In versions v0.6.8 and prior, this function was allowed to return NULL.
 * In later versions, if allocator->mem_calloc() returns NULL, this function will assert and exit. To handle
 * conditions where OOM is not a fatal error, allocator->mem_calloc() is responsible for finding/reclaiming/running a
 * GC etc...before returning.
 */
AWS_COMMON_API
void *aws_mem_calloc(struct aws_allocator *allocator, size_t num, size_t size);

/**
 * Allocates many chunks of bytes into a single block. Expects to be called with alternating void ** (dest), size_t
 * (size). The first void ** will be set to the root of the allocation. Alignment is assumed to be sizeof(intmax_t).
 *
 * This is useful for allocating structs using the pimpl pattern, as you may allocate the public object and impl object
 * in the same contiguous block of memory.
 *
 * Returns a pointer to the allocation.
 *
 * In versions v0.6.8 and prior, this function was allowed to return
 * NULL. In later versions, if allocator->mem_acquire() returns NULL, this function will assert and exit. To handle
 * conditions where OOM is not a fatal error, allocator->mem_acquire() is responsible for finding/reclaiming/running a
 * GC etc...before returning.
 */
AWS_COMMON_API
void *aws_mem_acquire_many(struct aws_allocator *allocator, size_t count, ...);

/**
 * Releases ptr back to whatever allocated it.
 * Nothing happens if ptr is NULL.
 */
AWS_COMMON_API
void aws_mem_release(struct aws_allocator *allocator, void *ptr);

/**
 * Attempts to adjust the size of the pointed-to memory buffer from oldsize to
 * newsize. The pointer (*ptr) may be changed if the memory needs to be
 * reallocated.
 *
 * In versions v0.6.8 and prior, this function was allowed to return
 * NULL. In later versions, if allocator->mem_realloc() returns NULL, this function will assert and exit. To handle
 * conditions where OOM is not a fatal error, allocator->mem_realloc() is responsible for finding/reclaiming/running a
 * GC etc...before returning.
 */
AWS_COMMON_API
int aws_mem_realloc(struct aws_allocator *allocator, void **ptr, size_t oldsize, size_t newsize);
/*
 * Maintainer note: The above function doesn't return the pointer (as with
 * standard C realloc) as this pattern becomes error-prone when OOMs occur.
 * In particular, we want to avoid losing the old pointer when an OOM condition
 * occurs, so we prefer to take the old pointer as an in/out reference argument
 * that we can leave unchanged on failure.
 */

enum aws_mem_trace_level {
    AWS_MEMTRACE_NONE = 0,   /* no tracing */
    AWS_MEMTRACE_BYTES = 1,  /* just track allocation sizes and total allocated */
    AWS_MEMTRACE_STACKS = 2, /* capture callstacks for each allocation */
};

/*
 * Wraps an allocator and tracks all external allocations. If aws_mem_trace_dump() is called
 * and there are still allocations active, they will be reported to the aws_logger at TRACE level.
 * allocator - The allocator to wrap
 * deprecated - Deprecated arg, ignored.
 * level - The level to track allocations at
 * frames_per_stack is how many frames to store per callstack if AWS_MEMTRACE_STACKS is in use,
 * otherwise it is ignored. 8 tends to be a pretty good number balancing storage space vs useful stacks.
 * Returns the tracer allocator, which should be used for all allocations that should be tracked.
 */
AWS_COMMON_API
struct aws_allocator *aws_mem_tracer_new(
    struct aws_allocator *allocator,
    struct aws_allocator *deprecated,
    enum aws_mem_trace_level level,
    size_t frames_per_stack);

/*
 * Unwraps the traced allocator and cleans up the tracer.
 * Returns the original allocator
 */
AWS_COMMON_API
struct aws_allocator *aws_mem_tracer_destroy(struct aws_allocator *trace_allocator);

/*
 * If there are outstanding allocations, dumps them to log, along with any information gathered
 * based on the trace level set when aws_mem_trace() was called.
 * Should be passed the tracer allocator returned from aws_mem_trace().
 */
AWS_COMMON_API
void aws_mem_tracer_dump(struct aws_allocator *trace_allocator);

/*
 * Returns the current number of bytes in outstanding allocations
 */
AWS_COMMON_API
size_t aws_mem_tracer_bytes(struct aws_allocator *trace_allocator);

/*
 * Returns the current number of outstanding allocations
 */
AWS_COMMON_API
size_t aws_mem_tracer_count(struct aws_allocator *trace_allocator);

/*
 * Creates a new Small Block Allocator which fronts the supplied parent allocator. The SBA will intercept
 * and handle small allocs, and will forward anything larger to the parent allocator.
 * If multi_threaded is true, the internal allocator will protect its internal data structures with a mutex
 */
AWS_COMMON_API
struct aws_allocator *aws_small_block_allocator_new(struct aws_allocator *allocator, bool multi_threaded);

/*
 * Destroys a Small Block Allocator instance and frees its memory to the parent allocator. The parent
 * allocator will otherwise be unaffected.
 */
AWS_COMMON_API
void aws_small_block_allocator_destroy(struct aws_allocator *sba_allocator);

/*
 * Returns the number of bytes currently active in the SBA
 */
AWS_COMMON_API
size_t aws_small_block_allocator_bytes_active(struct aws_allocator *sba_allocator);

/*
 * Returns the number of bytes reserved in pages/bins inside the SBA, e.g. the
 * current system memory used by the SBA
 */
AWS_COMMON_API
size_t aws_small_block_allocator_bytes_reserved(struct aws_allocator *sba_allocator);

/*
 * Returns the page size that the SBA is using
 */
AWS_COMMON_API
size_t aws_small_block_allocator_page_size(struct aws_allocator *sba_allocator);

/*
 * Returns the amount of memory in each page available to user allocations
 */
AWS_COMMON_API
size_t aws_small_block_allocator_page_size_available(struct aws_allocator *sba_allocator);

AWS_EXTERN_C_END
AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_COMMON_ALLOCATOR_H */

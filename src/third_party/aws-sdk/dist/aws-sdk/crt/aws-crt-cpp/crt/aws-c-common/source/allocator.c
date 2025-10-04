/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/assert.h>
#include <aws/common/common.h>
#include <aws/common/logging.h>
#include <aws/common/math.h>

#include <stdarg.h>
#include <stdlib.h>

#ifdef _WIN32
#    include <windows.h>
#endif

#ifdef __MACH__
#    include <CoreFoundation/CoreFoundation.h>
#endif

/* turn off unused named parameter warning on msvc.*/
#ifdef _MSC_VER
#    pragma warning(push)
#    pragma warning(disable : 4100)
#endif

#ifndef PAGE_SIZE
#    define PAGE_SIZE (4 * 1024)
#endif

bool aws_allocator_is_valid(const struct aws_allocator *alloc) {
    /* An allocator must define mem_acquire and mem_release.  All other fields are optional */
    return alloc && AWS_OBJECT_PTR_IS_READABLE(alloc) && alloc->mem_acquire && alloc->mem_release;
}

static void *s_aligned_malloc(struct aws_allocator *allocator, size_t size) {
    (void)allocator;
    /* larger allocations should be aligned so that AVX and friends can avoid
     * the extra preamble during unaligned versions of memcpy/memset on big buffers
     * This will also accelerate hardware CRC and SHA on ARM chips
     *
     * 64 byte alignment for > page allocations on 64 bit systems
     * 32 byte alignment for > page allocations on 32 bit systems
     * 16 byte alignment for <= page allocations on 64 bit systems
     * 8 byte alignment for <= page allocations on 32 bit systems
     *
     * We use PAGE_SIZE as the boundary because we are not aware of any allocations of
     * this size or greater that are not data buffers
     */
    const size_t alignment = sizeof(void *) * (size > (size_t)PAGE_SIZE ? 8 : 2);
#if !defined(_WIN32)
    void *result = NULL;
    int err = posix_memalign(&result, alignment, size);
    (void)err;
    AWS_PANIC_OOM(result, "posix_memalign failed to allocate memory");
    return result;
#else
    void *mem = _aligned_malloc(size, alignment);
    AWS_FATAL_POSTCONDITION(mem && "_aligned_malloc failed to allocate memory");
    return mem;
#endif
}

static void s_aligned_free(struct aws_allocator *allocator, void *ptr) {
    (void)allocator;
#if !defined(_WIN32)
    free(ptr);
#else
    _aligned_free(ptr);
#endif
}

static void *s_aligned_realloc(struct aws_allocator *allocator, void *ptr, size_t oldsize, size_t newsize) {
    (void)allocator;
    (void)oldsize;
    AWS_FATAL_PRECONDITION(newsize);

#if !defined(_WIN32)
    if (newsize <= oldsize) {
        return ptr;
    }

    /* newsize is > oldsize, need more memory */
    void *new_mem = s_aligned_malloc(allocator, newsize);
    AWS_PANIC_OOM(new_mem, "Unhandled OOM encountered in s_aligned_malloc");

    if (ptr) {
        memcpy(new_mem, ptr, oldsize);
        s_aligned_free(allocator, ptr);
    }

    return new_mem;
#else
    const size_t alignment = sizeof(void *) * (newsize > (size_t)PAGE_SIZE ? 8 : 2);
    void *new_mem = _aligned_realloc(ptr, newsize, alignment);
    AWS_PANIC_OOM(new_mem, "Unhandled OOM encountered in _aligned_realloc");
    return new_mem;
#endif
}

static void *s_aligned_calloc(struct aws_allocator *allocator, size_t num, size_t size) {
    void *mem = s_aligned_malloc(allocator, num * size);
    AWS_PANIC_OOM(mem, "Unhandled OOM encountered in s_aligned_calloc");
    memset(mem, 0, num * size);
    return mem;
}

static void *s_non_aligned_malloc(struct aws_allocator *allocator, size_t size) {
    (void)allocator;
    void *result = malloc(size);
    AWS_PANIC_OOM(result, "malloc failed to allocate memory");
    return result;
}

static void s_non_aligned_free(struct aws_allocator *allocator, void *ptr) {
    (void)allocator;
    free(ptr);
}

static void *s_non_aligned_realloc(struct aws_allocator *allocator, void *ptr, size_t oldsize, size_t newsize) {
    (void)allocator;
    (void)oldsize;
    AWS_FATAL_PRECONDITION(newsize);

    if (newsize <= oldsize) {
        return ptr;
    }

    /* newsize is > oldsize, need more memory */
    void *new_mem = s_non_aligned_malloc(allocator, newsize);
    AWS_PANIC_OOM(new_mem, "Unhandled OOM encountered in s_non_aligned_realloc");

    if (ptr) {
        memcpy(new_mem, ptr, oldsize);
        s_non_aligned_free(allocator, ptr);
    }

    return new_mem;
}

static void *s_non_aligned_calloc(struct aws_allocator *allocator, size_t num, size_t size) {
    (void)allocator;
    void *mem = calloc(num, size);
    AWS_PANIC_OOM(mem, "Unhandled OOM encountered in s_non_aligned_calloc");
    return mem;
}

static struct aws_allocator default_allocator = {
    .mem_acquire = s_non_aligned_malloc,
    .mem_release = s_non_aligned_free,
    .mem_realloc = s_non_aligned_realloc,
    .mem_calloc = s_non_aligned_calloc,
};

struct aws_allocator *aws_default_allocator(void) {
    return &default_allocator;
}

static struct aws_allocator aligned_allocator = {
    .mem_acquire = s_aligned_malloc,
    .mem_release = s_aligned_free,
    .mem_realloc = s_aligned_realloc,
    .mem_calloc = s_aligned_calloc,
};

struct aws_allocator *aws_aligned_allocator(void) {
    return &aligned_allocator;
}

void *aws_mem_acquire(struct aws_allocator *allocator, size_t size) {
    AWS_FATAL_PRECONDITION(allocator != NULL);
    AWS_FATAL_PRECONDITION(allocator->mem_acquire != NULL);
    /* Protect against https://wiki.sei.cmu.edu/confluence/display/c/MEM04-C.+Beware+of+zero-length+allocations */
    AWS_FATAL_PRECONDITION(size != 0);

    void *mem = allocator->mem_acquire(allocator, size);
    AWS_PANIC_OOM(mem, "Unhandled OOM encountered in aws_mem_acquire with allocator");

    return mem;
}

void *aws_mem_calloc(struct aws_allocator *allocator, size_t num, size_t size) {
    AWS_FATAL_PRECONDITION(allocator != NULL);
    AWS_FATAL_PRECONDITION(allocator->mem_calloc || allocator->mem_acquire);
    /* Protect against https://wiki.sei.cmu.edu/confluence/display/c/MEM04-C.+Beware+of+zero-length+allocations */
    AWS_FATAL_PRECONDITION(num != 0 && size != 0);

    /* Defensive check: never use calloc with size * num that would overflow
     * https://wiki.sei.cmu.edu/confluence/display/c/MEM07-C.+Ensure+that+the+arguments+to+calloc%28%29%2C+when+multiplied%2C+do+not+wrap
     */
    size_t required_bytes = 0;
    AWS_FATAL_POSTCONDITION(!aws_mul_size_checked(num, size, &required_bytes), "calloc computed size > SIZE_MAX");

    /* If there is a defined calloc, use it */
    if (allocator->mem_calloc) {
        void *mem = allocator->mem_calloc(allocator, num, size);
        AWS_PANIC_OOM(mem, "Unhandled OOM encountered in aws_mem_acquire with allocator");
        return mem;
    }

    /* Otherwise, emulate calloc */
    void *mem = allocator->mem_acquire(allocator, required_bytes);
    AWS_PANIC_OOM(mem, "Unhandled OOM encountered in aws_mem_acquire with allocator");

    memset(mem, 0, required_bytes);
    return mem;
}

#define AWS_ALIGN_ROUND_UP(value, alignment) (((value) + ((alignment) - 1)) & ~((alignment) - 1))

void *aws_mem_acquire_many(struct aws_allocator *allocator, size_t count, ...) {

    enum { S_ALIGNMENT = sizeof(intmax_t) };

    va_list args_size;
    va_start(args_size, count);
    va_list args_allocs;
    va_copy(args_allocs, args_size);

    size_t total_size = 0;
    for (size_t i = 0; i < count; ++i) {

        /* Ignore the pointer argument for now */
        va_arg(args_size, void **);

        size_t alloc_size = va_arg(args_size, size_t);
        total_size += AWS_ALIGN_ROUND_UP(alloc_size, S_ALIGNMENT);
    }
    va_end(args_size);

    void *allocation = NULL;

    if (total_size > 0) {

        allocation = aws_mem_acquire(allocator, total_size);
        AWS_PANIC_OOM(allocation, "Unhandled OOM encountered in aws_mem_acquire with allocator");

        uint8_t *current_ptr = allocation;

        for (size_t i = 0; i < count; ++i) {

            void **out_ptr = va_arg(args_allocs, void **);

            size_t alloc_size = va_arg(args_allocs, size_t);
            alloc_size = AWS_ALIGN_ROUND_UP(alloc_size, S_ALIGNMENT);

            *out_ptr = current_ptr;
            current_ptr += alloc_size;
        }
    }

    va_end(args_allocs);
    return allocation;
}

#undef AWS_ALIGN_ROUND_UP

void aws_mem_release(struct aws_allocator *allocator, void *ptr) {
    AWS_FATAL_PRECONDITION(allocator != NULL);
    AWS_FATAL_PRECONDITION(allocator->mem_release != NULL);

    if (ptr != NULL) {
        allocator->mem_release(allocator, ptr);
    }
}

int aws_mem_realloc(struct aws_allocator *allocator, void **ptr, size_t oldsize, size_t newsize) {
    AWS_FATAL_PRECONDITION(allocator != NULL);
    AWS_FATAL_PRECONDITION(allocator->mem_realloc || allocator->mem_acquire);
    AWS_FATAL_PRECONDITION(allocator->mem_release);

    /* Protect against https://wiki.sei.cmu.edu/confluence/display/c/MEM04-C.+Beware+of+zero-length+allocations */
    if (newsize == 0) {
        aws_mem_release(allocator, *ptr);
        *ptr = NULL;
        return AWS_OP_SUCCESS;
    }

    if (allocator->mem_realloc) {
        void *newptr = allocator->mem_realloc(allocator, *ptr, oldsize, newsize);
        AWS_PANIC_OOM(newptr, "Unhandled OOM encountered in aws_mem_acquire with allocator");

        *ptr = newptr;
        return AWS_OP_SUCCESS;
    }

    /* Since the allocator doesn't support realloc, we'll need to emulate it (inefficiently). */
    if (oldsize >= newsize) {
        return AWS_OP_SUCCESS;
    }

    void *newptr = allocator->mem_acquire(allocator, newsize);
    AWS_PANIC_OOM(newptr, "Unhandled OOM encountered in aws_mem_acquire with allocator");

    memcpy(newptr, *ptr, oldsize);
    memset((uint8_t *)newptr + oldsize, 0, newsize - oldsize);

    aws_mem_release(allocator, *ptr);

    *ptr = newptr;

    return AWS_OP_SUCCESS;
}

/* Wraps a CFAllocator around aws_allocator. For Mac only. */
#ifdef __MACH__

static CFStringRef s_cf_allocator_description = CFSTR("CFAllocator wrapping aws_allocator.");

/* note we don't have a standard specification stating sizeof(size_t) == sizeof(void *) so we have some extra casts */
static void *s_cf_allocator_allocate(CFIndex alloc_size, CFOptionFlags hint, void *info) {
    (void)hint;

    struct aws_allocator *allocator = info;

    void *mem = aws_mem_acquire(allocator, (size_t)alloc_size + sizeof(size_t));

    size_t allocation_size = (size_t)alloc_size + sizeof(size_t);
    memcpy(mem, &allocation_size, sizeof(size_t));
    return (void *)((uint8_t *)mem + sizeof(size_t));
}

static void s_cf_allocator_deallocate(void *ptr, void *info) {
    struct aws_allocator *allocator = info;

    void *original_allocation = (uint8_t *)ptr - sizeof(size_t);

    aws_mem_release(allocator, original_allocation);
}

static void *s_cf_allocator_reallocate(void *ptr, CFIndex new_size, CFOptionFlags hint, void *info) {
    (void)hint;

    struct aws_allocator *allocator = info;
    AWS_ASSERT(allocator->mem_realloc);

    void *original_allocation = (uint8_t *)ptr - sizeof(size_t);
    size_t original_size = 0;
    memcpy(&original_size, original_allocation, sizeof(size_t));

    aws_mem_realloc(allocator, &original_allocation, original_size, (size_t)new_size);
    AWS_FATAL_ASSERT(original_allocation);
    size_t new_allocation_size = (size_t)new_size;
    memcpy(original_allocation, &new_allocation_size, sizeof(size_t));

    return (void *)((uint8_t *)original_allocation + sizeof(size_t));
}

static CFStringRef s_cf_allocator_copy_description(const void *info) {
    (void)info;

    return s_cf_allocator_description;
}

static CFIndex s_cf_allocator_preferred_size(CFIndex size, CFOptionFlags hint, void *info) {
    (void)hint;
    (void)info;

    return (CFIndex)(size + sizeof(size_t));
}

CFAllocatorRef aws_wrapped_cf_allocator_new(struct aws_allocator *allocator) {
    CFAllocatorRef cf_allocator = NULL;

    CFAllocatorReallocateCallBack reallocate_callback = NULL;

    if (allocator->mem_realloc) {
        reallocate_callback = s_cf_allocator_reallocate;
    }

    CFAllocatorContext context = {
        .allocate = s_cf_allocator_allocate,
        .copyDescription = s_cf_allocator_copy_description,
        .deallocate = s_cf_allocator_deallocate,
        .reallocate = reallocate_callback,
        .info = allocator,
        .preferredSize = s_cf_allocator_preferred_size,
        .release = NULL,
        .retain = NULL,
        .version = 0,
    };

    cf_allocator = CFAllocatorCreate(NULL, &context);

    AWS_FATAL_ASSERT(cf_allocator && "creation of cf allocator failed!");

    return cf_allocator;
}

void aws_wrapped_cf_allocator_destroy(CFAllocatorRef allocator) {
    CFRelease(allocator);
}

#endif /*__MACH__ */

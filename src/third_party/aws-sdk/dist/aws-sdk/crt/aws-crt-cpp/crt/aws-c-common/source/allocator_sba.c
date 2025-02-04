/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/allocator.h>
#include <aws/common/array_list.h>
#include <aws/common/assert.h>
#include <aws/common/macros.h>
#include <aws/common/mutex.h>

/*
 * Small Block Allocator
 * This is a fairly standard approach, the idea is to always allocate aligned pages of memory so that for
 * any address you can round to the nearest page boundary to find the bookkeeping data. The idea is to reduce
 * overhead per alloc and greatly improve runtime speed by doing as little actual allocation work as possible,
 * preferring instead to re-use (hopefully still cached) chunks in FIFO order, or chunking up a page if there's
 * no free chunks. When all chunks in a page are freed, the page is returned to the OS.
 *
 * The allocator itself is simply an array of bins, each representing a power of 2 size from 32 - N (512 tends to be
 * a good upper bound). Thread safety is guaranteed by a mutex per bin, and locks are only necessary around the
 * lowest level alloc and free operations.
 *
 * Note: this allocator gets its internal memory for data structures from the parent allocator, but does not
 * use the parent to allocate pages. Pages are allocated directly from the OS-specific aligned malloc implementation,
 * which allows the OS to do address re-mapping for us instead of over-allocating to fulfill alignment.
 */

#ifdef _WIN32
#    include <malloc.h>
#elif __linux__ || __APPLE__
#    include <stdlib.h>
#endif

#if !defined(AWS_SBA_PAGE_SIZE)
#    if defined(PAGE_SIZE)
#        define AWS_SBA_PAGE_SIZE ((uintptr_t)(PAGE_SIZE))
#    else
#        define AWS_SBA_PAGE_SIZE ((uintptr_t)(4096))
#    endif
#endif

#define AWS_SBA_PAGE_MASK ((uintptr_t) ~(AWS_SBA_PAGE_SIZE - 1))
#define AWS_SBA_TAG_VALUE 0x736f6d6570736575ULL

/* list of sizes of bins, must be powers of 2, and less than AWS_SBA_PAGE_SIZE * 0.5 */
enum { AWS_SBA_BIN_COUNT = 5 };
static const size_t s_bin_sizes[AWS_SBA_BIN_COUNT] = {32, 64, 128, 256, 512};
static const size_t s_max_bin_size = 512;

struct sba_bin {
    size_t size;                        /* size of allocs in this bin */
    struct aws_mutex mutex;             /* lock protecting this bin */
    uint8_t *page_cursor;               /* pointer to working page, currently being chunked from */
    struct aws_array_list active_pages; /* all pages in use by this bin, could be optimized at scale by being a set */
    struct aws_array_list free_chunks;  /* free chunks available in this bin */
};

/* Header stored at the base of each page.
 * As long as this is under 32 bytes, all is well.
 * Above that, there's potentially more waste per page */
struct page_header {
    uint64_t tag;         /* marker to identify/validate pages */
    struct sba_bin *bin;  /* bin this page belongs to */
    uint32_t alloc_count; /* number of outstanding allocs from this page */
    uint64_t tag2;
};

/* This is the impl for the aws_allocator */
struct small_block_allocator {
    struct aws_allocator *allocator; /* parent allocator, for large allocs */
    struct sba_bin bins[AWS_SBA_BIN_COUNT];
    int (*lock)(struct aws_mutex *);
    int (*unlock)(struct aws_mutex *);
};

static int s_null_lock(struct aws_mutex *mutex) {
    (void)mutex;
    /* NO OP */
    return 0;
}

static int s_null_unlock(struct aws_mutex *mutex) {
    (void)mutex;
    /* NO OP */
    return 0;
}

static int s_mutex_lock(struct aws_mutex *mutex) {
    return aws_mutex_lock(mutex);
}

static int s_mutex_unlock(struct aws_mutex *mutex) {
    return aws_mutex_unlock(mutex);
}

static void *s_page_base(const void *addr) {
    /* mask off the address to round it to page alignment */
    uint8_t *page_base = (uint8_t *)(((uintptr_t)addr) & AWS_SBA_PAGE_MASK);
    return page_base;
}

static void *s_page_bind(void *addr, struct sba_bin *bin) {
    /* insert the header at the base of the page and advance past it */
    struct page_header *page = (struct page_header *)addr;
    page->tag = page->tag2 = AWS_SBA_TAG_VALUE;
    page->bin = bin;
    page->alloc_count = 0;
    return (uint8_t *)addr + sizeof(struct page_header);
}

/* Wraps OS-specific aligned malloc implementation */
static void *s_aligned_alloc(size_t size, size_t align) {
#ifdef _WIN32
    return _aligned_malloc(size, align);
#else
    void *mem = NULL;
    int return_code = posix_memalign(&mem, align, size);
    if (return_code) {
        aws_raise_error(AWS_ERROR_OOM);
        return NULL;
    }
    return mem;
#endif
}

/* wraps OS-specific aligned free implementation */
static void s_aligned_free(void *addr) {
#ifdef _WIN32
    _aligned_free(addr);
#else
    free(addr);
#endif
}

/* aws_allocator vtable template */
static void *s_sba_mem_acquire(struct aws_allocator *allocator, size_t size);
static void s_sba_mem_release(struct aws_allocator *allocator, void *ptr);
static void *s_sba_mem_realloc(struct aws_allocator *allocator, void *old_ptr, size_t old_size, size_t new_size);
static void *s_sba_mem_calloc(struct aws_allocator *allocator, size_t num, size_t size);

static struct aws_allocator s_sba_allocator = {
    .mem_acquire = s_sba_mem_acquire,
    .mem_release = s_sba_mem_release,
    .mem_realloc = s_sba_mem_realloc,
    .mem_calloc = s_sba_mem_calloc,
};

static int s_sba_init(struct small_block_allocator *sba, struct aws_allocator *allocator, bool multi_threaded) {
    sba->allocator = allocator;
    AWS_ZERO_ARRAY(sba->bins);
    sba->lock = multi_threaded ? s_mutex_lock : s_null_lock;
    sba->unlock = multi_threaded ? s_mutex_unlock : s_null_unlock;

    for (unsigned idx = 0; idx < AWS_SBA_BIN_COUNT; ++idx) {
        struct sba_bin *bin = &sba->bins[idx];
        bin->size = s_bin_sizes[idx];
        if (multi_threaded && aws_mutex_init(&bin->mutex)) {
            goto cleanup;
        }
        if (aws_array_list_init_dynamic(&bin->active_pages, sba->allocator, 16, sizeof(void *))) {
            goto cleanup;
        }
        /* start with enough chunks for 1 page */
        if (aws_array_list_init_dynamic(
                &bin->free_chunks, sba->allocator, aws_max_size(AWS_SBA_PAGE_SIZE / bin->size, 16), sizeof(void *))) {
            goto cleanup;
        }
    }

    return AWS_OP_SUCCESS;

cleanup:
    for (unsigned idx = 0; idx < AWS_SBA_BIN_COUNT; ++idx) {
        struct sba_bin *bin = &sba->bins[idx];
        aws_mutex_clean_up(&bin->mutex);
        aws_array_list_clean_up(&bin->active_pages);
        aws_array_list_clean_up(&bin->free_chunks);
    }
    return AWS_OP_ERR;
}

static void s_sba_clean_up(struct small_block_allocator *sba) {
    /* free all known pages, then free the working page */
    for (unsigned idx = 0; idx < AWS_SBA_BIN_COUNT; ++idx) {
        struct sba_bin *bin = &sba->bins[idx];
        for (size_t page_idx = 0; page_idx < bin->active_pages.length; ++page_idx) {
            void *page_addr = NULL;
            aws_array_list_get_at(&bin->active_pages, &page_addr, page_idx);
            struct page_header *page = page_addr;
            AWS_ASSERT(page->alloc_count == 0 && "Memory still allocated in aws_sba_allocator (bin)");
            s_aligned_free(page);
        }
        if (bin->page_cursor) {
            void *page_addr = s_page_base(bin->page_cursor);
            struct page_header *page = page_addr;
            AWS_ASSERT(page->alloc_count == 0 && "Memory still allocated in aws_sba_allocator (page)");
            s_aligned_free(page);
        }

        aws_array_list_clean_up(&bin->active_pages);
        aws_array_list_clean_up(&bin->free_chunks);
        aws_mutex_clean_up(&bin->mutex);
    }
}

struct aws_allocator *aws_small_block_allocator_new(struct aws_allocator *allocator, bool multi_threaded) {
    struct small_block_allocator *sba = NULL;
    struct aws_allocator *sba_allocator = NULL;
    aws_mem_acquire_many(
        allocator, 2, &sba, sizeof(struct small_block_allocator), &sba_allocator, sizeof(struct aws_allocator));

    if (!sba || !sba_allocator) {
        return NULL;
    }

    AWS_ZERO_STRUCT(*sba);
    AWS_ZERO_STRUCT(*sba_allocator);

    /* copy the template vtable */
    *sba_allocator = s_sba_allocator;
    sba_allocator->impl = sba;

    if (s_sba_init(sba, allocator, multi_threaded)) {
        s_sba_clean_up(sba);
        aws_mem_release(allocator, sba);
        return NULL;
    }
    return sba_allocator;
}

void aws_small_block_allocator_destroy(struct aws_allocator *sba_allocator) {
    if (!sba_allocator) {
        return;
    }
    struct small_block_allocator *sba = sba_allocator->impl;
    if (!sba) {
        return;
    }

    struct aws_allocator *allocator = sba->allocator;
    s_sba_clean_up(sba);
    aws_mem_release(allocator, sba);
}

size_t aws_small_block_allocator_bytes_active(struct aws_allocator *sba_allocator) {
    AWS_FATAL_ASSERT(sba_allocator && "aws_small_block_allocator_bytes_used requires a non-null allocator");
    struct small_block_allocator *sba = sba_allocator->impl;
    AWS_FATAL_ASSERT(sba && "aws_small_block_allocator_bytes_used: supplied allocator has invalid SBA impl");

    size_t used = 0;
    for (unsigned idx = 0; idx < AWS_SBA_BIN_COUNT; ++idx) {
        struct sba_bin *bin = &sba->bins[idx];
        sba->lock(&bin->mutex);
        for (size_t page_idx = 0; page_idx < bin->active_pages.length; ++page_idx) {
            void *page_addr = NULL;
            aws_array_list_get_at(&bin->active_pages, &page_addr, page_idx);
            struct page_header *page = page_addr;
            used += page->alloc_count * bin->size;
        }
        if (bin->page_cursor) {
            void *page_addr = s_page_base(bin->page_cursor);
            struct page_header *page = page_addr;
            used += page->alloc_count * bin->size;
        }
        sba->unlock(&bin->mutex);
    }

    return used;
}

size_t aws_small_block_allocator_bytes_reserved(struct aws_allocator *sba_allocator) {
    AWS_FATAL_ASSERT(sba_allocator && "aws_small_block_allocator_bytes_used requires a non-null allocator");
    struct small_block_allocator *sba = sba_allocator->impl;
    AWS_FATAL_ASSERT(sba && "aws_small_block_allocator_bytes_used: supplied allocator has invalid SBA impl");

    size_t used = 0;
    for (unsigned idx = 0; idx < AWS_SBA_BIN_COUNT; ++idx) {
        struct sba_bin *bin = &sba->bins[idx];
        sba->lock(&bin->mutex);
        used += (bin->active_pages.length + (bin->page_cursor != NULL)) * AWS_SBA_PAGE_SIZE;
        sba->unlock(&bin->mutex);
    }

    return used;
}

size_t aws_small_block_allocator_page_size(struct aws_allocator *sba_allocator) {
    (void)sba_allocator;
    return AWS_SBA_PAGE_SIZE;
}

size_t aws_small_block_allocator_page_size_available(struct aws_allocator *sba_allocator) {
    (void)sba_allocator;
    return AWS_SBA_PAGE_SIZE - sizeof(struct page_header);
}

/* NOTE: Expects the mutex to be held by the caller */
static void *s_sba_alloc_from_bin(struct sba_bin *bin) {
    /* check the free list, hand chunks out in FIFO order */
    if (bin->free_chunks.length > 0) {
        void *chunk = NULL;
        if (aws_array_list_back(&bin->free_chunks, &chunk)) {
            return NULL;
        }
        if (aws_array_list_pop_back(&bin->free_chunks)) {
            return NULL;
        }

        AWS_ASSERT(chunk);
        struct page_header *page = s_page_base(chunk);
        page->alloc_count++;
        return chunk;
    }

    /* If there is a working page to chunk from, use it */
    if (bin->page_cursor) {
        struct page_header *page = s_page_base(bin->page_cursor);
        AWS_ASSERT(page);
        size_t space_left = AWS_SBA_PAGE_SIZE - (bin->page_cursor - (uint8_t *)page);
        if (space_left >= bin->size) {
            void *chunk = bin->page_cursor;
            page->alloc_count++;
            bin->page_cursor += bin->size;
            space_left -= bin->size;
            if (space_left < bin->size) {
                aws_array_list_push_back(&bin->active_pages, &page);
                bin->page_cursor = NULL;
            }
            return chunk;
        }
    }

    /* Nothing free to use, allocate a page and restart */
    uint8_t *new_page = s_aligned_alloc(AWS_SBA_PAGE_SIZE, AWS_SBA_PAGE_SIZE);
    new_page = s_page_bind(new_page, bin);
    bin->page_cursor = new_page;
    return s_sba_alloc_from_bin(bin);
}

/* NOTE: Expects the mutex to be held by the caller */
static void s_sba_free_to_bin(struct sba_bin *bin, void *addr) {
    AWS_PRECONDITION(addr);
    struct page_header *page = s_page_base(addr);
    AWS_ASSERT(page->bin == bin);
    page->alloc_count--;
    if (page->alloc_count == 0 && page != s_page_base(bin->page_cursor)) { /* empty page, free it */
        uint8_t *page_start = (uint8_t *)page + sizeof(struct page_header);
        uint8_t *page_end = page_start + AWS_SBA_PAGE_SIZE;
        /* Remove all chunks in the page from the free list */
        intptr_t chunk_idx = (intptr_t)bin->free_chunks.length;
        for (; chunk_idx >= 0; --chunk_idx) {
            uint8_t *chunk = NULL;
            aws_array_list_get_at(&bin->free_chunks, &chunk, chunk_idx);
            if (chunk >= page_start && chunk < page_end) {
                aws_array_list_swap(&bin->free_chunks, chunk_idx, bin->free_chunks.length - 1);
                aws_array_list_pop_back(&bin->free_chunks);
            }
        }

        /* Find page in pages list and remove it */
        for (size_t page_idx = 0; page_idx < bin->active_pages.length; ++page_idx) {
            void *page_addr = NULL;
            aws_array_list_get_at(&bin->active_pages, &page_addr, page_idx);
            if (page_addr == page) {
                aws_array_list_swap(&bin->active_pages, page_idx, bin->active_pages.length - 1);
                aws_array_list_pop_back(&bin->active_pages);
                break;
            }
        }
        /* ensure that the page tag is erased, in case nearby memory is re-used */
        page->tag = page->tag2 = 0;
        s_aligned_free(page);
        return;
    }

    aws_array_list_push_back(&bin->free_chunks, &addr);
}

/* No lock required for this function, it's all read-only access to constant data */
static struct sba_bin *s_sba_find_bin(struct small_block_allocator *sba, size_t size) {
    AWS_PRECONDITION(size <= s_max_bin_size);

    /* map bits 5(32) to 9(512) to indices 0-4 */
    size_t next_pow2 = 0;
    aws_round_up_to_power_of_two(size, &next_pow2);
    size_t lz = aws_clz_i32((int32_t)next_pow2);
    size_t idx = aws_sub_size_saturating(31 - lz, 5);
    AWS_ASSERT(idx <= 4);
    struct sba_bin *bin = &sba->bins[idx];
    AWS_ASSERT(bin->size >= size);
    return bin;
}

static void *s_sba_alloc(struct small_block_allocator *sba, size_t size) {
    if (size <= s_max_bin_size) {
        struct sba_bin *bin = s_sba_find_bin(sba, size);
        AWS_FATAL_ASSERT(bin);
        /* BEGIN CRITICAL SECTION */
        sba->lock(&bin->mutex);
        void *mem = s_sba_alloc_from_bin(bin);
        sba->unlock(&bin->mutex);
        /* END CRITICAL SECTION */
        return mem;
    }
    return aws_mem_acquire(sba->allocator, size);
}

AWS_SUPPRESS_ASAN AWS_SUPPRESS_TSAN static void s_sba_free(struct small_block_allocator *sba, void *addr) {
    if (!addr) {
        return;
    }

    struct page_header *page = (struct page_header *)s_page_base(addr);
    /* Check to see if this page is tagged by the sba */
    /* this check causes a read of (possibly) memory we didn't allocate, but it will always be
     * heap memory, so should not cause any issues. TSan will see this as a data race, but it
     * is not, that's a false positive
     */
    if (page->tag == AWS_SBA_TAG_VALUE && page->tag2 == AWS_SBA_TAG_VALUE) {
        struct sba_bin *bin = page->bin;
        /* BEGIN CRITICAL SECTION */
        sba->lock(&bin->mutex);
        s_sba_free_to_bin(bin, addr);
        sba->unlock(&bin->mutex);
        /* END CRITICAL SECTION */
        return;
    }
    /* large alloc, give back to underlying allocator */
    aws_mem_release(sba->allocator, addr);
}

static void *s_sba_mem_acquire(struct aws_allocator *allocator, size_t size) {
    struct small_block_allocator *sba = allocator->impl;
    return s_sba_alloc(sba, size);
}

static void s_sba_mem_release(struct aws_allocator *allocator, void *ptr) {
    struct small_block_allocator *sba = allocator->impl;
    s_sba_free(sba, ptr);
}

static void *s_sba_mem_realloc(struct aws_allocator *allocator, void *old_ptr, size_t old_size, size_t new_size) {
    struct small_block_allocator *sba = allocator->impl;
    /* If both allocations come from the parent, let the parent do it */
    if (old_size > s_max_bin_size && new_size > s_max_bin_size) {
        void *ptr = old_ptr;
        if (aws_mem_realloc(sba->allocator, &ptr, old_size, new_size)) {
            return NULL;
        }
        return ptr;
    }

    if (new_size == 0) {
        s_sba_free(sba, old_ptr);
        return NULL;
    }

    if (old_size > new_size) {
        return old_ptr;
    }

    void *new_mem = s_sba_alloc(sba, new_size);
    if (old_ptr && old_size) {
        memcpy(new_mem, old_ptr, old_size);
        s_sba_free(sba, old_ptr);
    }

    return new_mem;
}

static void *s_sba_mem_calloc(struct aws_allocator *allocator, size_t num, size_t size) {
    struct small_block_allocator *sba = allocator->impl;
    void *mem = s_sba_alloc(sba, size * num);
    memset(mem, 0, size * num);
    return mem;
}

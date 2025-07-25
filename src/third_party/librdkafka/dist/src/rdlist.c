/*
 * librdkafka - Apache Kafka C library
 *
 * Copyright (c) 2012-2022, Magnus Edenhill
 *               2023, Confluent Inc.
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

#include "rd.h"
#include "rdlist.h"


void rd_list_dump(const char *what, const rd_list_t *rl) {
        int i;
        printf("%s: (rd_list_t*)%p cnt %d, size %d, elems %p:\n", what, rl,
               rl->rl_cnt, rl->rl_size, rl->rl_elems);
        for (i = 0; i < rl->rl_cnt; i++)
                printf("  #%d: %p at &%p\n", i, rl->rl_elems[i],
                       &rl->rl_elems[i]);
}

void rd_list_grow(rd_list_t *rl, size_t size) {
        rd_assert(!(rl->rl_flags & RD_LIST_F_FIXED_SIZE));
        rl->rl_size += (int)size;
        if (unlikely(rl->rl_size == 0))
                return; /* avoid zero allocations */
        rl->rl_elems =
            rd_realloc(rl->rl_elems, sizeof(*rl->rl_elems) * rl->rl_size);
}

rd_list_t *
rd_list_init(rd_list_t *rl, int initial_size, void (*free_cb)(void *)) {
        memset(rl, 0, sizeof(*rl));

        if (initial_size > 0)
                rd_list_grow(rl, initial_size);

        rl->rl_free_cb = free_cb;

        return rl;
}

rd_list_t *rd_list_init_copy(rd_list_t *dst, const rd_list_t *src) {

        if (src->rl_flags & RD_LIST_F_FIXED_SIZE) {
                /* Source was preallocated, prealloc new dst list */
                rd_list_init(dst, 0, src->rl_free_cb);

                rd_list_prealloc_elems(dst, src->rl_elemsize, src->rl_size,
                                       1 /*memzero*/);
        } else {
                /* Source is dynamic, initialize dst the same */
                rd_list_init(dst, rd_list_cnt(src), src->rl_free_cb);
        }

        return dst;
}

static RD_INLINE rd_list_t *rd_list_alloc(void) {
        return rd_malloc(sizeof(rd_list_t));
}

rd_list_t *rd_list_new(int initial_size, void (*free_cb)(void *)) {
        rd_list_t *rl = rd_list_alloc();
        rd_list_init(rl, initial_size, free_cb);
        rl->rl_flags |= RD_LIST_F_ALLOCATED;
        return rl;
}


void rd_list_prealloc_elems(rd_list_t *rl,
                            size_t elemsize,
                            size_t cnt,
                            int memzero) {
        size_t allocsize;
        char *p;
        size_t i;

        rd_assert(!rl->rl_elems);

        /* Allocation layout:
         *   void *ptrs[cnt];
         *   elems[elemsize][cnt];
         */

        allocsize = (sizeof(void *) * cnt) + (elemsize * cnt);
        if (memzero)
                rl->rl_elems = rd_calloc(1, allocsize);
        else
                rl->rl_elems = rd_malloc(allocsize);

        /* p points to first element's memory, unless elemsize is 0. */
        if (elemsize > 0)
                p = rl->rl_p = (char *)&rl->rl_elems[cnt];
        else
                p = rl->rl_p = NULL;

        /* Pointer -> elem mapping */
        for (i = 0; i < cnt; i++, p += elemsize)
                rl->rl_elems[i] = p;

        rl->rl_size = (int)cnt;
        rl->rl_cnt  = 0;
        rl->rl_flags |= RD_LIST_F_FIXED_SIZE;
        rl->rl_elemsize = (int)elemsize;
}


void rd_list_set_cnt(rd_list_t *rl, size_t cnt) {
        rd_assert(rl->rl_flags & RD_LIST_F_FIXED_SIZE);
        rd_assert((int)cnt <= rl->rl_size);
        rl->rl_cnt = (int)cnt;
}


void rd_list_free_cb(rd_list_t *rl, void *ptr) {
        if (rl->rl_free_cb && ptr)
                rl->rl_free_cb(ptr);
}


void *rd_list_add(rd_list_t *rl, void *elem) {
        if (rl->rl_cnt == rl->rl_size)
                rd_list_grow(rl, rl->rl_size ? rl->rl_size * 2 : 16);
        rl->rl_flags &= ~RD_LIST_F_SORTED;
        if (elem)
                rl->rl_elems[rl->rl_cnt] = elem;
        return rl->rl_elems[rl->rl_cnt++];
}


void rd_list_set(rd_list_t *rl, int idx, void *ptr) {
        if (idx >= rl->rl_size)
                rd_list_grow(rl, idx + 1);

        if (idx >= rl->rl_cnt) {
                memset(&rl->rl_elems[rl->rl_cnt], 0,
                       sizeof(*rl->rl_elems) * (idx - rl->rl_cnt));
                rl->rl_cnt = idx + 1;
        } else {
                /* Not allowed to replace existing element. */
                rd_assert(!rl->rl_elems[idx]);
        }

        rl->rl_elems[idx] = ptr;
}



void rd_list_remove_elem(rd_list_t *rl, int idx) {
        rd_assert(idx < rl->rl_cnt);

        if (idx + 1 < rl->rl_cnt)
                memmove(&rl->rl_elems[idx], &rl->rl_elems[idx + 1],
                        sizeof(*rl->rl_elems) * (rl->rl_cnt - (idx + 1)));
        rl->rl_cnt--;
}

void *rd_list_remove(rd_list_t *rl, void *match_elem) {
        void *elem;
        int i;

        RD_LIST_FOREACH(elem, rl, i) {
                if (elem == match_elem) {
                        rd_list_remove_elem(rl, i);
                        return elem;
                }
        }

        return NULL;
}


void *rd_list_remove_cmp(rd_list_t *rl,
                         void *match_elem,
                         int (*cmp)(void *_a, void *_b)) {
        void *elem;
        int i;

        RD_LIST_FOREACH(elem, rl, i) {
                if (elem == match_elem || !cmp(elem, match_elem)) {
                        rd_list_remove_elem(rl, i);
                        return elem;
                }
        }

        return NULL;
}


int rd_list_remove_multi_cmp(rd_list_t *rl,
                             void *match_elem,
                             int (*cmp)(void *_a, void *_b)) {

        void *elem;
        int i;
        int cnt = 0;

        /* Scan backwards to minimize memmoves */
        RD_LIST_FOREACH_REVERSE(elem, rl, i) {
                if (match_elem == cmp || !cmp(elem, match_elem)) {
                        rd_list_remove_elem(rl, i);
                        cnt++;
                }
        }

        return cnt;
}


void *rd_list_pop(rd_list_t *rl) {
        void *elem;
        int idx = rl->rl_cnt - 1;

        if (idx < 0)
                return NULL;

        elem = rl->rl_elems[idx];
        rd_list_remove_elem(rl, idx);

        return elem;
}


/**
 * Trampoline to avoid the double pointers in callbacks.
 *
 * rl_elems is a **, but to avoid having the application do the cumbersome
 * ** -> * casting we wrap this here and provide a simple * pointer to the
 * the callbacks.
 *
 * This is true for all list comparator uses, i.e., both sort() and find().
 */
static RD_TLS int (*rd_list_cmp_curr)(const void *, const void *);

static RD_INLINE int rd_list_cmp_trampoline(const void *_a, const void *_b) {
        const void *a = *(const void **)_a, *b = *(const void **)_b;

        return rd_list_cmp_curr(a, b);
}

void rd_list_sort(rd_list_t *rl, int (*cmp)(const void *, const void *)) {
        if (unlikely(rl->rl_elems == NULL))
                return;

        rd_list_cmp_curr = cmp;
        qsort(rl->rl_elems, rl->rl_cnt, sizeof(*rl->rl_elems),
              rd_list_cmp_trampoline);
        rl->rl_flags |= RD_LIST_F_SORTED;
}

static void rd_list_destroy_elems(rd_list_t *rl) {
        int i;

        if (!rl->rl_elems)
                return;

        if (rl->rl_free_cb) {
                /* Free in reverse order to allow deletions */
                for (i = rl->rl_cnt - 1; i >= 0; i--)
                        if (rl->rl_elems[i])
                                rl->rl_free_cb(rl->rl_elems[i]);
        }

        rd_free(rl->rl_elems);
        rl->rl_elems = NULL;
        rl->rl_cnt   = 0;
        rl->rl_size  = 0;
        rl->rl_flags &= ~RD_LIST_F_SORTED;
}


void rd_list_clear(rd_list_t *rl) {
        rd_list_destroy_elems(rl);
}


void rd_list_destroy(rd_list_t *rl) {
        rd_list_destroy_elems(rl);
        if (rl->rl_flags & RD_LIST_F_ALLOCATED)
                rd_free(rl);
}

void rd_list_destroy_free(void *rl) {
        rd_list_destroy((rd_list_t *)rl);
}

void *rd_list_elem(const rd_list_t *rl, int idx) {
        if (likely(idx < rl->rl_cnt))
                return (void *)rl->rl_elems[idx];
        return NULL;
}

int rd_list_index(const rd_list_t *rl,
                  const void *match,
                  int (*cmp)(const void *, const void *)) {
        int i;
        const void *elem;

        RD_LIST_FOREACH(elem, rl, i) {
                if (!cmp(match, elem))
                        return i;
        }

        return -1;
}


void *rd_list_find(const rd_list_t *rl,
                   const void *match,
                   int (*cmp)(const void *, const void *)) {
        int i;
        const void *elem;

        if (rl->rl_flags & RD_LIST_F_SORTED) {
                void **r;
                rd_list_cmp_curr = cmp;
                r = bsearch(&match /*ptrptr to match elems*/, rl->rl_elems,
                            rl->rl_cnt, sizeof(*rl->rl_elems),
                            rd_list_cmp_trampoline);
                return r ? *r : NULL;
        }

        RD_LIST_FOREACH(elem, rl, i) {
                if (!cmp(match, elem))
                        return (void *)elem;
        }

        return NULL;
}


void *rd_list_first(const rd_list_t *rl) {
        if (rl->rl_cnt == 0)
                return NULL;
        return rl->rl_elems[0];
}

void *rd_list_last(const rd_list_t *rl) {
        if (rl->rl_cnt == 0)
                return NULL;
        return rl->rl_elems[rl->rl_cnt - 1];
}


void *rd_list_find_duplicate(const rd_list_t *rl,
                             int (*cmp)(const void *, const void *)) {
        int i;

        rd_assert(rl->rl_flags & RD_LIST_F_SORTED);

        for (i = 1; i < rl->rl_cnt; i++) {
                if (!cmp(rl->rl_elems[i - 1], rl->rl_elems[i]))
                        return rl->rl_elems[i];
        }

        return NULL;
}

void rd_list_deduplicate(rd_list_t **rl,
                         int (*cmp)(const void *, const void *)) {
        rd_list_t *deduped = rd_list_new(0, (*rl)->rl_free_cb);
        void *elem;
        void *prev_elem = NULL;
        int i;

        if (!((*rl)->rl_flags & RD_LIST_F_SORTED))
                rd_list_sort(*rl, cmp);

        RD_LIST_FOREACH(elem, *rl, i) {
                if (prev_elem && cmp(elem, prev_elem) == 0) {
                        /* Skip this element, and destroy it */
                        rd_list_free_cb(*rl, elem);
                        continue;
                }
                rd_list_add(deduped, elem);
                prev_elem = elem;
        }
        /* The elements we want destroyed are already destroyed. */
        (*rl)->rl_free_cb = NULL;
        rd_list_destroy(*rl);

        /* The parent list was sorted, we can set this without re-sorting. */
        deduped->rl_flags |= RD_LIST_F_SORTED;
        *rl = deduped;
}

int rd_list_cmp(const rd_list_t *a,
                const rd_list_t *b,
                int (*cmp)(const void *, const void *)) {
        int i;

        i = RD_CMP(a->rl_cnt, b->rl_cnt);
        if (i)
                return i;

        for (i = 0; i < a->rl_cnt; i++) {
                int r = cmp(a->rl_elems[i], b->rl_elems[i]);
                if (r)
                        return r;
        }

        return 0;
}


/**
 * @brief Simple element pointer comparator
 */
int rd_list_cmp_ptr(const void *a, const void *b) {
        return RD_CMP(a, b);
}

int rd_list_cmp_str(const void *a, const void *b) {
        return strcmp((const char *)a, (const char *)b);
}

void rd_list_apply(rd_list_t *rl,
                   int (*cb)(void *elem, void *opaque),
                   void *opaque) {
        void *elem;
        int i;

        RD_LIST_FOREACH(elem, rl, i) {
                if (!cb(elem, opaque)) {
                        rd_list_remove_elem(rl, i);
                        i--;
                }
        }

        return;
}


/**
 * @brief Default element copier that simply assigns the original pointer.
 */
static void *rd_list_nocopy_ptr(const void *elem, void *opaque) {
        return (void *)elem;
}

rd_list_t *
rd_list_copy(const rd_list_t *src, rd_list_copy_cb_t *copy_cb, void *opaque) {
        rd_list_t *dst;

        dst = rd_list_new(src->rl_cnt, src->rl_free_cb);

        rd_list_copy_to(dst, src, copy_cb, opaque);
        return dst;
}


void rd_list_copy_to(rd_list_t *dst,
                     const rd_list_t *src,
                     void *(*copy_cb)(const void *elem, void *opaque),
                     void *opaque) {
        void *elem;
        int i;

        rd_assert(dst != src);

        if (!copy_cb)
                copy_cb = rd_list_nocopy_ptr;

        RD_LIST_FOREACH(elem, src, i) {
                void *celem = copy_cb(elem, opaque);
                if (celem)
                        rd_list_add(dst, celem);
        }
}


/**
 * @brief Copy elements of preallocated \p src to preallocated \p dst.
 *
 * @remark \p dst will be overwritten and initialized, but its
 *         flags will be retained.
 *
 * @returns \p dst
 */
static rd_list_t *rd_list_copy_preallocated0(rd_list_t *dst,
                                             const rd_list_t *src) {
        int dst_flags = dst->rl_flags & RD_LIST_F_ALLOCATED;

        rd_assert(dst != src);

        rd_list_init_copy(dst, src);
        dst->rl_flags |= dst_flags;

        rd_assert((dst->rl_flags & RD_LIST_F_FIXED_SIZE));
        rd_assert((src->rl_flags & RD_LIST_F_FIXED_SIZE));
        rd_assert(dst->rl_elemsize == src->rl_elemsize &&
                  dst->rl_size == src->rl_size);

        memcpy(dst->rl_p, src->rl_p, src->rl_elemsize * src->rl_size);
        dst->rl_cnt = src->rl_cnt;

        return dst;
}

void *rd_list_copy_preallocated(const void *elem, void *opaque) {
        return rd_list_copy_preallocated0(rd_list_new(0, NULL),
                                          (const rd_list_t *)elem);
}



void rd_list_move(rd_list_t *dst, rd_list_t *src) {
        rd_list_init_copy(dst, src);

        if (src->rl_flags & RD_LIST_F_FIXED_SIZE) {
                rd_list_copy_preallocated0(dst, src);
        } else {
                memcpy(dst->rl_elems, src->rl_elems,
                       src->rl_cnt * sizeof(*src->rl_elems));
                dst->rl_cnt = src->rl_cnt;
        }

        src->rl_cnt = 0;
}


/**
 * @name Misc helpers for common list types
 * @{
 *
 */
rd_list_t *rd_list_init_int32(rd_list_t *rl, int max_size) {
        int rl_flags = rl->rl_flags & RD_LIST_F_ALLOCATED;
        rd_list_init(rl, 0, NULL);
        rl->rl_flags |= rl_flags;
        rd_list_prealloc_elems(rl, sizeof(int32_t), max_size, 1 /*memzero*/);
        return rl;
}

void rd_list_set_int32(rd_list_t *rl, int idx, int32_t val) {
        rd_assert((rl->rl_flags & RD_LIST_F_FIXED_SIZE) &&
                  rl->rl_elemsize == sizeof(int32_t));
        rd_assert(idx < rl->rl_size);

        memcpy(rl->rl_elems[idx], &val, sizeof(int32_t));

        if (rl->rl_cnt <= idx)
                rl->rl_cnt = idx + 1;
}

int32_t rd_list_get_int32(const rd_list_t *rl, int idx) {
        rd_assert((rl->rl_flags & RD_LIST_F_FIXED_SIZE) &&
                  rl->rl_elemsize == sizeof(int32_t) && idx < rl->rl_cnt);
        return *(int32_t *)rl->rl_elems[idx];
}



/**@}*/

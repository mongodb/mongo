/*
 * librdkafka - Apache Kafka C library
 *
 * Copyright (c) 2012-2015, Magnus Edenhill
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

#ifndef _RDLIST_H_
#define _RDLIST_H_


/**
 *
 * Simple light-weight append-only list to be used as a collection convenience.
 *
 */

typedef struct rd_list_s {
        int rl_size;
        int rl_cnt;
        void **rl_elems;
        void (*rl_free_cb)(void *);
        int rl_flags;
#define RD_LIST_F_ALLOCATED                                                    \
        0x1 /* The rd_list_t is allocated,                                     \
             * will be free on destroy() */
#define RD_LIST_F_SORTED                                                       \
        0x2                      /* Set by sort(), cleared by any mutations.   \
                                  * When this flag is set bsearch() is used    \
                                  * by find(), otherwise a linear search. */
#define RD_LIST_F_FIXED_SIZE 0x4 /* Assert on grow, when prealloc()ed */
#define RD_LIST_F_UNIQUE                                                       \
        0x8              /* Don't allow duplicates:                            \
                          * ONLY ENFORCED BY CALLER. */
        int rl_elemsize; /**< Element size (when prealloc()ed) */
        void *rl_p;      /**< Start of prealloced elements,
                          *   the allocation itself starts at rl_elems
                          */
} rd_list_t;


/**
 * @brief Initialize a list, prepare for 'initial_size' elements
 *        (optional optimization).
 *        List elements will optionally be freed by \p free_cb.
 *
 * @returns \p rl
 */
rd_list_t *
rd_list_init(rd_list_t *rl, int initial_size, void (*free_cb)(void *));


/**
 * @brief Same as rd_list_init() but uses initial_size and free_cb
 *        from the provided \p src list.
 */
rd_list_t *rd_list_init_copy(rd_list_t *rl, const rd_list_t *src);

/**
 * @brief Allocate a new list pointer and initialize
 *        it according to rd_list_init().
 *
 *        This is the same as calling \c rd_list_init(rd_list_alloc(), ..));
 *
 * Use rd_list_destroy() to free.
 */
rd_list_t *rd_list_new(int initial_size, void (*free_cb)(void *));


/**
 * @brief Prepare list to for an additional \p size elements.
 *        This is an optimization to avoid incremental grows.
 */
void rd_list_grow(rd_list_t *rl, size_t size);

/**
 * @brief Preallocate elements to avoid having to pass an allocated pointer to
 *        rd_list_add(), instead pass NULL to rd_list_add() and use the returned
 *        pointer as the element.
 *
 * @param elemsize element size, or 0 if elements are allocated separately.
 * @param size number of elements
 * @param memzero initialize element memory to zeros.
 *
 * @remark Preallocated element lists can't grow past \p size.
 */
void rd_list_prealloc_elems(rd_list_t *rl,
                            size_t elemsize,
                            size_t size,
                            int memzero);

/**
 * @brief Set the number of valid elements, this must only be used
 *        with prealloc_elems() to make the preallocated elements directly
 *        usable.
 */
void rd_list_set_cnt(rd_list_t *rl, size_t cnt);


/**
 * @brief Free a pointer using the list's free_cb
 *
 * @remark If no free_cb is set, or \p ptr is NULL, dont do anything
 *
 * Typical use is rd_list_free_cb(rd_list_remove_cmp(....));
 */
void rd_list_free_cb(rd_list_t *rl, void *ptr);


/**
 * @brief Append element to list
 *
 * @returns \p elem. If \p elem is NULL the default element for that index
 *          will be returned (for use with set_elems).
 */
void *rd_list_add(rd_list_t *rl, void *elem);


/**
 * @brief Set element at \p idx to \p ptr.
 *
 * @remark MUST NOT overwrite an existing element.
 * @remark The list will be grown, if needed, any gaps between the current
 *         highest element and \p idx will be set to NULL.
 */
void rd_list_set(rd_list_t *rl, int idx, void *ptr);


/**
 * Remove element from list.
 * This is a slow O(n) + memmove operation.
 * Returns the removed element.
 */
void *rd_list_remove(rd_list_t *rl, void *match_elem);

/**
 * Remove element from list using comparator.
 * See rd_list_remove()
 */
void *rd_list_remove_cmp(rd_list_t *rl,
                         void *match_elem,
                         int (*cmp)(void *_a, void *_b));


/**
 * @brief Remove element at index \p idx.
 *
 * This is a O(1) + memmove operation
 */
void rd_list_remove_elem(rd_list_t *rl, int idx);


/**
 * @brief Remove and return the last element in the list.
 *
 * @returns the last element, or NULL if list is empty. */
void *rd_list_pop(rd_list_t *rl);


/**
 * @brief Remove all elements matching comparator.
 *
 * @returns the number of elements removed.
 *
 * @sa rd_list_remove()
 */
int rd_list_remove_multi_cmp(rd_list_t *rl,
                             void *match_elem,
                             int (*cmp)(void *_a, void *_b));


/**
 * @brief Sort list using comparator.
 *
 * To sort a list ascendingly the comparator should implement (a - b)
 * and for descending order implement (b - a).
 */
void rd_list_sort(rd_list_t *rl, int (*cmp)(const void *, const void *));


/**
 * Empties the list and frees elements (if there is a free_cb).
 */
void rd_list_clear(rd_list_t *rl);


/**
 * Empties the list, frees the element array, and optionally frees
 * each element using the registered \c rl->rl_free_cb.
 *
 * If the list was previously allocated with rd_list_new() it will be freed.
 */
void rd_list_destroy(rd_list_t *rl);

/**
 * @brief Wrapper for rd_list_destroy() that has same signature as free(3),
 *        allowing it to be used as free_cb for nested lists.
 */
void rd_list_destroy_free(void *rl);


/**
 * Returns the element at index 'idx', or NULL if out of range.
 *
 * Typical iteration is:
 *    int i = 0;
 *    my_type_t *obj;
 *    while ((obj = rd_list_elem(rl, i++)))
 *        do_something(obj);
 */
void *rd_list_elem(const rd_list_t *rl, int idx);

#define RD_LIST_FOREACH(elem, listp, idx)                                      \
        for (idx = 0; (elem = rd_list_elem(listp, idx)); idx++)

#define RD_LIST_FOREACH_REVERSE(elem, listp, idx)                              \
        for (idx = (listp)->rl_cnt - 1;                                        \
             idx >= 0 && (elem = rd_list_elem(listp, idx)); idx--)

/**
 * Returns the number of elements in list.
 */
static RD_INLINE RD_UNUSED int rd_list_cnt(const rd_list_t *rl) {
        return rl->rl_cnt;
}


/**
 * Returns true if list is empty
 */
#define rd_list_empty(rl) (rd_list_cnt(rl) == 0)


/**
 * @brief Find element index using comparator.
 *
 * \p match is the first argument to \p cmp, and each element (up to a match)
 * is the second argument to \p cmp.
 *
 * @remark this is a O(n) scan.
 * @returns the first matching element or NULL.
 */
int rd_list_index(const rd_list_t *rl,
                  const void *match,
                  int (*cmp)(const void *, const void *));

/**
 * @brief Find element using comparator
 *
 * \p match is the first argument to \p cmp, and each element (up to a match)
 * is the second argument to \p cmp.
 *
 * @remark if the list is sorted bsearch() is used, otherwise an O(n) scan.
 *
 * @returns the first matching element or NULL.
 */
void *rd_list_find(const rd_list_t *rl,
                   const void *match,
                   int (*cmp)(const void *, const void *));



/**
 * @returns the first element of the list, or NULL if list is empty.
 */
void *rd_list_first(const rd_list_t *rl);

/**
 * @returns the last element of the list, or NULL if list is empty.
 */
void *rd_list_last(const rd_list_t *rl);


/**
 * @returns the first duplicate in the list or NULL if no duplicates.
 *
 * @warning The list MUST be sorted.
 */
void *rd_list_find_duplicate(const rd_list_t *rl,
                             int (*cmp)(const void *, const void *));


/**
 * @brief Compare list \p a to \p b.
 *
 * @returns < 0 if a was "lesser" than b,
 *          > 0 if a was "greater" than b,
 *            0 if a and b are equal.
 */
int rd_list_cmp(const rd_list_t *a,
                const rd_list_t *b,
                int (*cmp)(const void *, const void *));

/**
 * @brief Simple element pointer comparator
 */
int rd_list_cmp_ptr(const void *a, const void *b);

/**
 * @brief strcmp comparator where the list elements are strings.
 */
int rd_list_cmp_str(const void *a, const void *b);


/**
 * @brief Apply \p cb to each element in list, if \p cb returns 0
 *        the element will be removed (but not freed).
 */
void rd_list_apply(rd_list_t *rl,
                   int (*cb)(void *elem, void *opaque),
                   void *opaque);



typedef void *(rd_list_copy_cb_t)(const void *elem, void *opaque);
/**
 * @brief Copy list \p src, returning a new list,
 *        using optional \p copy_cb (per elem)
 */
rd_list_t *
rd_list_copy(const rd_list_t *src, rd_list_copy_cb_t *copy_cb, void *opaque);


/**
 * @brief Copy list \p src to \p dst using optional \p copy_cb (per elem)
 * @remark The destination list is not initialized or copied by this function.
 * @remark copy_cb() may return NULL in which case no element is added,
 *                   but the copy callback might have done so itself.
 */
void rd_list_copy_to(rd_list_t *dst,
                     const rd_list_t *src,
                     void *(*copy_cb)(const void *elem, void *opaque),
                     void *opaque);


/**
 * @brief Copy callback to copy elements that are preallocated lists.
 */
void *rd_list_copy_preallocated(const void *elem, void *opaque);


/**
 * @brief String copier for rd_list_copy()
 */
static RD_UNUSED void *rd_list_string_copy(const void *elem, void *opaque) {
        return rd_strdup((const char *)elem);
}



/**
 * @brief Move elements from \p src to \p dst.
 *
 * @remark \p dst will be initialized first.
 * @remark \p src will be emptied.
 */
void rd_list_move(rd_list_t *dst, rd_list_t *src);


/**
 * @name Misc helpers for common list types
 * @{
 *
 */

/**
 * @brief Init a new list of int32_t's of maximum size \p max_size
 *        where each element is pre-allocated.
 *
 * @remark The allocation flag of the original \p rl is retained,
 *         do not pass an uninitialized \p rl to this function.
 */
rd_list_t *rd_list_init_int32(rd_list_t *rl, int max_size);


/**
 * Debugging: Print list to stdout.
 */
void rd_list_dump(const char *what, const rd_list_t *rl);



/**
 * @brief Set element at index \p idx to value \p val.
 *
 * @remark Must only be used with preallocated int32_t lists.
 * @remark Allows values to be overwritten.
 */
void rd_list_set_int32(rd_list_t *rl, int idx, int32_t val);

/**
 * @returns the int32_t element value at index \p idx
 *
 * @remark Must only be used with preallocated int32_t lists.
 */
int32_t rd_list_get_int32(const rd_list_t *rl, int idx);

/**@}*/

#endif /* _RDLIST_H_ */

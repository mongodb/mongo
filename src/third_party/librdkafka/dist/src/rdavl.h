/*
 * librd - Rapid Development C library
 *
 * Copyright (c) 2012-2016, Magnus Edenhill
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


/*
 * AVL tree.
 * Inspired by Ian Piumarta's tree.h implementation.
 */

#ifndef _RDAVL_H_
#define _RDAVL_H_

#include "tinycthread.h"


typedef enum {
        RD_AVL_LEFT,
        RD_AVL_RIGHT,
} rd_avl_dir_t;

/**
 * AVL tree node.
 * Add 'rd_avl_node_t ..' as field to your element's struct and
 * provide it as the 'field' argument in the API below.
 */
typedef struct rd_avl_node_s {
        struct rd_avl_node_s *ran_p[2]; /* RD_AVL_LEFT and RD_AVL_RIGHT */
        int ran_height;                 /* Sub-tree height */
        void *ran_elm;                  /* Backpointer to the containing
                                         * element. This could be considered
                                         * costly but is convenient for the
                                         * caller: RAM is cheap,
                                         * development time isn't*/
} rd_avl_node_t;



/**
 * Per-AVL application-provided element comparator.
 */
typedef int (*rd_avl_cmp_t)(const void *, const void *);


/**
 * AVL tree
 */
typedef struct rd_avl_s {
        rd_avl_node_t *ravl_root; /* Root node */
        rd_avl_cmp_t ravl_cmp;    /* Comparator */
        int ravl_flags;           /* Flags */
#define RD_AVL_F_LOCKS 0x1        /* Enable thread-safeness */
#define RD_AVL_F_OWNER 0x2        /* internal: rd_avl_init() allocated ravl */
        rwlock_t ravl_rwlock;     /* Mutex when .._F_LOCKS is set. */
} rd_avl_t;



/**
 *
 *
 * Public API
 *
 *
 */

/**
 * Insert 'elm' into AVL tree.
 * In case of collision the previous entry is overwritten by the
 * new one and the previous element is returned, else NULL.
 */
#define RD_AVL_INSERT(ravl, elm, field) rd_avl_insert(ravl, elm, &(elm)->field)


/**
 * Remove element by matching value 'elm' using compare function.
 */
#define RD_AVL_REMOVE_ELM(ravl, elm) rd_avl_remove_elm(ravl, elm)

/**
 * Search for (by value using compare function) and return matching elm.
 */
#define RD_AVL_FIND(ravl, elm) rd_avl_find(ravl, elm, 1)


/**
 * Search (by value using compare function) for and return matching elm.
 * Same as RD_AVL_FIND_NL() but assumes 'ravl' ís already locked
 * by 'rd_avl_*lock()'.
 *
 * NOTE: rd_avl_wrlock() must be held.
 */
#define RD_AVL_FIND_NL(ravl, elm)                                              \
        rd_avl_find_node(ravl, (ravl)->ravl_root, elm, 0)


/**
 * Search (by value using compare function) for elm and return its AVL node.
 *
 * NOTE: rd_avl_wrlock() must be held.
 */
#define RD_AVL_FIND_NODE_NL(ravl, elm) rd_avl_find(ravl, elm, 0)


/**
 * Changes the element pointer for an existing AVL node in the tree.
 * The new element must be identical (according to the comparator)
 * to the previous element.
 *
 * NOTE: rd_avl_wrlock() must be held.
 */
#define RD_AVL_ELM_SET_NL(ran, elm) ((ran)->ran_elm = (elm))

/**
 * Returns the current element pointer for an existing AVL node in the tree
 *
 * NOTE: rd_avl_*lock() must be held.
 */
#define RD_AVL_ELM_GET_NL(ran) ((ran)->ran_elm)



/**
 * Destroy previously initialized (by rd_avl_init()) AVL tree.
 */
void rd_avl_destroy(rd_avl_t *ravl);

/**
 * Initialize (and optionally allocate if 'ravl' is NULL) AVL tree.
 * 'cmp' is the comparison function that takes two const pointers
 * pointing to the elements being compared (rather than the avl_nodes).
 * 'flags' is zero or more of the RD_AVL_F_.. flags.
 *
 * For thread-safe AVL trees supply RD_AVL_F_LOCKS in 'flags'.
 */
rd_avl_t *rd_avl_init(rd_avl_t *ravl, rd_avl_cmp_t cmp, int flags);


/**
 * 'ravl' locking functions.
 * Locking is performed automatically for all methods except for
 * those with the "_NL"/"_nl" suffix ("not locked") which expects
 * either read or write lock to be held.
 *
 * rdavl utilizes rwlocks to allow multiple concurrent read threads.
 */
static RD_INLINE RD_UNUSED void rd_avl_rdlock(rd_avl_t *ravl) {
        if (ravl->ravl_flags & RD_AVL_F_LOCKS)
                rwlock_rdlock(&ravl->ravl_rwlock);
}

static RD_INLINE RD_UNUSED void rd_avl_wrlock(rd_avl_t *ravl) {
        if (ravl->ravl_flags & RD_AVL_F_LOCKS)
                rwlock_wrlock(&ravl->ravl_rwlock);
}

static RD_INLINE RD_UNUSED void rd_avl_rdunlock(rd_avl_t *ravl) {
        if (ravl->ravl_flags & RD_AVL_F_LOCKS)
                rwlock_rdunlock(&ravl->ravl_rwlock);
}

static RD_INLINE RD_UNUSED void rd_avl_wrunlock(rd_avl_t *ravl) {
        if (ravl->ravl_flags & RD_AVL_F_LOCKS)
                rwlock_wrunlock(&ravl->ravl_rwlock);
}



/**
 * Private API, dont use directly.
 */

rd_avl_node_t *rd_avl_insert_node(rd_avl_t *ravl,
                                  rd_avl_node_t *parent,
                                  rd_avl_node_t *ran,
                                  rd_avl_node_t **existing);

static RD_UNUSED void *
rd_avl_insert(rd_avl_t *ravl, void *elm, rd_avl_node_t *ran) {
        rd_avl_node_t *existing = NULL;

        memset(ran, 0, sizeof(*ran));
        ran->ran_elm = elm;

        rd_avl_wrlock(ravl);
        ravl->ravl_root =
            rd_avl_insert_node(ravl, ravl->ravl_root, ran, &existing);
        rd_avl_wrunlock(ravl);

        return existing ? existing->ran_elm : NULL;
}

rd_avl_node_t *
rd_avl_remove_elm0(rd_avl_t *ravl, rd_avl_node_t *parent, const void *elm);

static RD_INLINE RD_UNUSED void rd_avl_remove_elm(rd_avl_t *ravl,
                                                  const void *elm) {
        rd_avl_wrlock(ravl);
        ravl->ravl_root = rd_avl_remove_elm0(ravl, ravl->ravl_root, elm);
        rd_avl_wrunlock(ravl);
}


rd_avl_node_t *rd_avl_find_node(const rd_avl_t *ravl,
                                const rd_avl_node_t *begin,
                                const void *elm);


static RD_INLINE RD_UNUSED void *
rd_avl_find(rd_avl_t *ravl, const void *elm, int dolock) {
        const rd_avl_node_t *ran;
        void *ret;

        if (dolock)
                rd_avl_rdlock(ravl);

        ran = rd_avl_find_node(ravl, ravl->ravl_root, elm);
        ret = ran ? ran->ran_elm : NULL;

        if (dolock)
                rd_avl_rdunlock(ravl);

        return ret;
}

#endif /* _RDAVL_H_ */

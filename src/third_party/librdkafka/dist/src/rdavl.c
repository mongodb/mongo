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

#include "rdkafka_int.h"
#include "rdavl.h"

/*
 * AVL tree.
 * Inspired by Ian Piumarta's tree.h implementation.
 */

#define RD_AVL_NODE_HEIGHT(ran) ((ran) ? (ran)->ran_height : 0)

#define RD_AVL_NODE_DELTA(ran)                                                 \
        (RD_AVL_NODE_HEIGHT((ran)->ran_p[RD_AVL_LEFT]) -                       \
         RD_AVL_NODE_HEIGHT((ran)->ran_p[RD_AVL_RIGHT]))

#define RD_DELTA_MAX 1


static rd_avl_node_t *rd_avl_balance_node(rd_avl_node_t *ran);

static rd_avl_node_t *rd_avl_rotate(rd_avl_node_t *ran, rd_avl_dir_t dir) {
        rd_avl_node_t *n;
        static const rd_avl_dir_t odirmap[] = {/* opposite direction map */
                                               [RD_AVL_RIGHT] = RD_AVL_LEFT,
                                               [RD_AVL_LEFT]  = RD_AVL_RIGHT};
        const int odir                      = odirmap[dir];

        n                = ran->ran_p[odir];
        ran->ran_p[odir] = n->ran_p[dir];
        n->ran_p[dir]    = rd_avl_balance_node(ran);

        return rd_avl_balance_node(n);
}

static rd_avl_node_t *rd_avl_balance_node(rd_avl_node_t *ran) {
        const int d = RD_AVL_NODE_DELTA(ran);
        int h;

        if (d < -RD_DELTA_MAX) {
                if (RD_AVL_NODE_DELTA(ran->ran_p[RD_AVL_RIGHT]) > 0)
                        ran->ran_p[RD_AVL_RIGHT] = rd_avl_rotate(
                            ran->ran_p[RD_AVL_RIGHT], RD_AVL_RIGHT);
                return rd_avl_rotate(ran, RD_AVL_LEFT);

        } else if (d > RD_DELTA_MAX) {
                if (RD_AVL_NODE_DELTA(ran->ran_p[RD_AVL_LEFT]) < 0)
                        ran->ran_p[RD_AVL_LEFT] =
                            rd_avl_rotate(ran->ran_p[RD_AVL_LEFT], RD_AVL_LEFT);

                return rd_avl_rotate(ran, RD_AVL_RIGHT);
        }

        ran->ran_height = 0;

        if ((h = RD_AVL_NODE_HEIGHT(ran->ran_p[RD_AVL_LEFT])) > ran->ran_height)
                ran->ran_height = h;

        if ((h = RD_AVL_NODE_HEIGHT(ran->ran_p[RD_AVL_RIGHT])) >
            ran->ran_height)
                ran->ran_height = h;

        ran->ran_height++;

        return ran;
}

rd_avl_node_t *rd_avl_insert_node(rd_avl_t *ravl,
                                  rd_avl_node_t *parent,
                                  rd_avl_node_t *ran,
                                  rd_avl_node_t **existing) {
        rd_avl_dir_t dir;
        int r;

        if (!parent)
                return ran;

        if ((r = ravl->ravl_cmp(ran->ran_elm, parent->ran_elm)) == 0) {
                /* Replace existing node with new one. */
                ran->ran_p[RD_AVL_LEFT]  = parent->ran_p[RD_AVL_LEFT];
                ran->ran_p[RD_AVL_RIGHT] = parent->ran_p[RD_AVL_RIGHT];
                ran->ran_height          = parent->ran_height;
                *existing                = parent;
                return ran;
        }

        if (r < 0)
                dir = RD_AVL_LEFT;
        else
                dir = RD_AVL_RIGHT;

        parent->ran_p[dir] =
            rd_avl_insert_node(ravl, parent->ran_p[dir], ran, existing);
        return rd_avl_balance_node(parent);
}


static rd_avl_node_t *
rd_avl_move(rd_avl_node_t *dst, rd_avl_node_t *src, rd_avl_dir_t dir) {

        if (!dst)
                return src;

        dst->ran_p[dir] = rd_avl_move(dst->ran_p[dir], src, dir);

        return rd_avl_balance_node(dst);
}

static rd_avl_node_t *rd_avl_remove_node0(rd_avl_node_t *ran) {
        rd_avl_node_t *tmp;

        tmp = rd_avl_move(ran->ran_p[RD_AVL_LEFT], ran->ran_p[RD_AVL_RIGHT],
                          RD_AVL_RIGHT);

        ran->ran_p[RD_AVL_LEFT] = ran->ran_p[RD_AVL_RIGHT] = NULL;
        return tmp;
}


rd_avl_node_t *
rd_avl_remove_elm0(rd_avl_t *ravl, rd_avl_node_t *parent, const void *elm) {
        rd_avl_dir_t dir;
        int r;

        if (!parent)
                return NULL;


        if ((r = ravl->ravl_cmp(elm, parent->ran_elm)) == 0)
                return rd_avl_remove_node0(parent);
        else if (r < 0)
                dir = RD_AVL_LEFT;
        else /* > 0 */
                dir = RD_AVL_RIGHT;

        parent->ran_p[dir] = rd_avl_remove_elm0(ravl, parent->ran_p[dir], elm);

        return rd_avl_balance_node(parent);
}



rd_avl_node_t *rd_avl_find_node(const rd_avl_t *ravl,
                                const rd_avl_node_t *begin,
                                const void *elm) {
        int r;

        if (!begin)
                return NULL;
        else if (!(r = ravl->ravl_cmp(elm, begin->ran_elm)))
                return (rd_avl_node_t *)begin;
        else if (r < 0)
                return rd_avl_find_node(ravl, begin->ran_p[RD_AVL_LEFT], elm);
        else /* r > 0 */
                return rd_avl_find_node(ravl, begin->ran_p[RD_AVL_RIGHT], elm);
}



void rd_avl_destroy(rd_avl_t *ravl) {
        if (ravl->ravl_flags & RD_AVL_F_LOCKS)
                rwlock_destroy(&ravl->ravl_rwlock);

        if (ravl->ravl_flags & RD_AVL_F_OWNER)
                rd_free(ravl);
}

rd_avl_t *rd_avl_init(rd_avl_t *ravl, rd_avl_cmp_t cmp, int flags) {

        if (!ravl) {
                ravl = rd_calloc(1, sizeof(*ravl));
                flags |= RD_AVL_F_OWNER;
        } else {
                memset(ravl, 0, sizeof(*ravl));
        }

        ravl->ravl_flags = flags;
        ravl->ravl_cmp   = cmp;

        if (flags & RD_AVL_F_LOCKS)
                rwlock_init(&ravl->ravl_rwlock);

        return ravl;
}

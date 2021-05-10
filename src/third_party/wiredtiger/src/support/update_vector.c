/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_update_vector_init --
 *     Initialize a update vector.
 */
void
__wt_update_vector_init(WT_SESSION_IMPL *session, WT_UPDATE_VECTOR *updates)
{
    WT_CLEAR(*updates);
    updates->session = session;
    updates->listp = updates->list;
}

/*
 * __wt_update_vector_push --
 *     Push a update pointer to a update vector. If we exceed the allowed stack space in the vector,
 *     we'll be doing malloc here.
 */
int
__wt_update_vector_push(WT_UPDATE_VECTOR *updates, WT_UPDATE *upd)
{
    WT_DECL_RET;
    bool migrate_from_stack;

    migrate_from_stack = false;

    if (updates->size >= WT_UPDATE_VECTOR_STACK_SIZE) {
        if (updates->allocated_bytes == 0 && updates->size == WT_UPDATE_VECTOR_STACK_SIZE) {
            migrate_from_stack = true;
            updates->listp = NULL;
        }
        WT_ERR(__wt_realloc_def(
          updates->session, &updates->allocated_bytes, updates->size + 1, &updates->listp));
        if (migrate_from_stack)
            memcpy(updates->listp, updates->list, sizeof(updates->list));
    }
    updates->listp[updates->size++] = upd;
    return (0);

err:
    /*
     * This only happens when we're migrating from the stack to the heap but failed to allocate. In
     * that case, point back to the stack allocated memory and set the allocation to zero to
     * indicate that we don't have heap memory to free.
     *
     * If we're already on the heap, we have nothing to do. The realloc call above won't touch the
     * list pointer unless allocation is successful and we won't have incremented the size yet.
     */
    if (updates->listp == NULL) {
        WT_ASSERT(updates->session, updates->size == WT_UPDATE_VECTOR_STACK_SIZE);
        updates->listp = updates->list;
        updates->allocated_bytes = 0;
    }
    return (ret);
}

/*
 * __wt_update_vector_pop --
 *     Pop an update pointer off a update vector.
 */
void
__wt_update_vector_pop(WT_UPDATE_VECTOR *updates, WT_UPDATE **updp)
{
    WT_ASSERT(updates->session, updates->size > 0);

    *updp = updates->listp[--updates->size];
}

/*
 * __wt_update_vector_peek --
 *     Peek an update pointer off a update vector.
 */
void
__wt_update_vector_peek(WT_UPDATE_VECTOR *updates, WT_UPDATE **updp)
{
    WT_ASSERT(updates->session, updates->size > 0);

    *updp = updates->listp[updates->size - 1];
}

/*
 * __wt_update_vector_clear --
 *     Clear a update vector.
 */
void
__wt_update_vector_clear(WT_UPDATE_VECTOR *updates)
{
    updates->size = 0;
}

/*
 * __wt_update_vector_free --
 *     Free any resources associated with a update vector. If we exceeded the allowed stack space on
 *     the vector and had to fallback to dynamic allocations, we'll be doing a free here.
 */
void
__wt_update_vector_free(WT_UPDATE_VECTOR *updates)
{
    if (updates->allocated_bytes != 0)
        __wt_free(updates->session, updates->listp);
    __wt_update_vector_init(updates->session, updates);
}

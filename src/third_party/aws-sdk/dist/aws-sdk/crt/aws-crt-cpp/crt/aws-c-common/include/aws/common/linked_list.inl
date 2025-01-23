#ifndef AWS_COMMON_LINKED_LIST_INL
#define AWS_COMMON_LINKED_LIST_INL

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/common.h>
#include <aws/common/linked_list.h>
#include <stddef.h>

AWS_EXTERN_C_BEGIN

/**
 * Set node's next and prev pointers to NULL.
 */
AWS_STATIC_IMPL void aws_linked_list_node_reset(struct aws_linked_list_node *node) {
    AWS_PRECONDITION(node != NULL);
    AWS_ZERO_STRUCT(*node);
    AWS_POSTCONDITION(AWS_IS_ZEROED(*node));
}

/**
 * These functions need to be defined first as they are used in pre
 * and post conditions.
 */

/**
 * Tests if the list is empty.
 */
AWS_STATIC_IMPL bool aws_linked_list_empty(const struct aws_linked_list *list) {
    AWS_PRECONDITION(list);
    return list->head.next == &list->tail;
}

/**
 * Checks that a linked list is valid.
 */
AWS_STATIC_IMPL bool aws_linked_list_is_valid(const struct aws_linked_list *list) {
    if (list && list->head.next && list->head.prev == NULL && list->tail.prev && list->tail.next == NULL) {
#if defined(AWS_DEEP_CHECKS) && (AWS_DEEP_CHECKS == 1)
        return aws_linked_list_is_valid_deep(list);
#else
        return true;
#endif
    }
    return false;
}

/**
 * Checks that the prev of the next pointer of a node points to the
 * node. As this checks whether the [next] connection of a node is
 * bidirectional, it returns false if used for the list tail.
 */
AWS_STATIC_IMPL bool aws_linked_list_node_next_is_valid(const struct aws_linked_list_node *node) {
    return node && node->next && node->next->prev == node;
}

/**
 * Checks that the next of the prev pointer of a node points to the
 * node. Similarly to the above, this returns false if used for the
 * head of a list.
 */
AWS_STATIC_IMPL bool aws_linked_list_node_prev_is_valid(const struct aws_linked_list_node *node) {
    return node && node->prev && node->prev->next == node;
}

/**
 * Checks that a linked list satisfies double linked list connectivity
 * constraints. This check is O(n) as it traverses the whole linked
 * list to ensure that tail is reachable from head (and vice versa)
 * and that every connection is bidirectional.
 *
 * Note: This check *cannot* go into an infinite loop, because we
 * ensure that the connection to the next node is
 * bidirectional. Therefore, if a node's [a] a.next is a previous node
 * [b] in the list, b.prev != &a and so this check would fail, thus
 * terminating the loop.
 */
AWS_STATIC_IMPL bool aws_linked_list_is_valid_deep(const struct aws_linked_list *list) {
    if (!list) {
        return false;
    }
    /* This could go into an infinite loop for a circular list */
    const struct aws_linked_list_node *temp = &list->head;
    /* Head must reach tail by following next pointers */
    bool head_reaches_tail = false;
    /* By satisfying the above and that edges are bidirectional, we
     * also guarantee that tail reaches head by following prev
     * pointers */
    while (temp) {
        if (temp == &list->tail) {
            head_reaches_tail = true;
            break;
        } else if (!aws_linked_list_node_next_is_valid(temp)) {
            /* Next and prev pointers should connect the same nodes */
            return false;
        }
        temp = temp->next;
    }
    return head_reaches_tail;
}

/**
 * Initializes the list. List will be empty after this call.
 */
AWS_STATIC_IMPL void aws_linked_list_init(struct aws_linked_list *list) {
    AWS_PRECONDITION(list);
    list->head.next = &list->tail;
    list->head.prev = NULL;
    list->tail.prev = &list->head;
    list->tail.next = NULL;
    AWS_POSTCONDITION(aws_linked_list_is_valid(list));
    AWS_POSTCONDITION(aws_linked_list_empty(list));
}

/**
 * Returns an iteration pointer for the first element in the list.
 */
AWS_STATIC_IMPL struct aws_linked_list_node *aws_linked_list_begin(const struct aws_linked_list *list) {
    AWS_PRECONDITION(aws_linked_list_is_valid(list));
    struct aws_linked_list_node *rval = list->head.next;
    AWS_POSTCONDITION(aws_linked_list_is_valid(list));
    AWS_POSTCONDITION(rval == list->head.next);
    return rval;
}

/**
 * Returns an iteration pointer for one past the last element in the list.
 */
AWS_STATIC_IMPL const struct aws_linked_list_node *aws_linked_list_end(const struct aws_linked_list *list) {
    AWS_PRECONDITION(aws_linked_list_is_valid(list));
    const struct aws_linked_list_node *rval = &list->tail;
    AWS_POSTCONDITION(aws_linked_list_is_valid(list));
    AWS_POSTCONDITION(rval == &list->tail);
    return rval;
}

/**
 * Returns a pointer for the last element in the list.
 * Used to begin iterating the list in reverse. Ex:
 *   for (i = aws_linked_list_rbegin(list); i != aws_linked_list_rend(list); i = aws_linked_list_prev(i)) {...}
 */
AWS_STATIC_IMPL struct aws_linked_list_node *aws_linked_list_rbegin(const struct aws_linked_list *list) {
    AWS_PRECONDITION(aws_linked_list_is_valid(list));
    struct aws_linked_list_node *rval = list->tail.prev;
    AWS_POSTCONDITION(aws_linked_list_is_valid(list));
    AWS_POSTCONDITION(rval == list->tail.prev);
    return rval;
}

/**
 * Returns the pointer to one before the first element in the list.
 * Used to end iterating the list in reverse.
 */
AWS_STATIC_IMPL const struct aws_linked_list_node *aws_linked_list_rend(const struct aws_linked_list *list) {
    AWS_PRECONDITION(aws_linked_list_is_valid(list));
    const struct aws_linked_list_node *rval = &list->head;
    AWS_POSTCONDITION(aws_linked_list_is_valid(list));
    AWS_POSTCONDITION(rval == &list->head);
    return rval;
}

/**
 * Returns the next element in the list.
 */
AWS_STATIC_IMPL struct aws_linked_list_node *aws_linked_list_next(const struct aws_linked_list_node *node) {
    AWS_PRECONDITION(aws_linked_list_node_next_is_valid(node));
    struct aws_linked_list_node *rval = node->next;
    AWS_POSTCONDITION(aws_linked_list_node_next_is_valid(node));
    AWS_POSTCONDITION(aws_linked_list_node_prev_is_valid(rval));
    AWS_POSTCONDITION(rval == node->next);
    return rval;
}

/**
 * Returns the previous element in the list.
 */
AWS_STATIC_IMPL struct aws_linked_list_node *aws_linked_list_prev(const struct aws_linked_list_node *node) {
    AWS_PRECONDITION(aws_linked_list_node_prev_is_valid(node));
    struct aws_linked_list_node *rval = node->prev;
    AWS_POSTCONDITION(aws_linked_list_node_prev_is_valid(node));
    AWS_POSTCONDITION(aws_linked_list_node_next_is_valid(rval));
    AWS_POSTCONDITION(rval == node->prev);
    return rval;
}

/**
 * Inserts to_add immediately after after.
 */
AWS_STATIC_IMPL void aws_linked_list_insert_after(
    struct aws_linked_list_node *after,
    struct aws_linked_list_node *to_add) {
    AWS_PRECONDITION(aws_linked_list_node_next_is_valid(after));
    AWS_PRECONDITION(to_add != NULL);
    to_add->prev = after;
    to_add->next = after->next;
    after->next->prev = to_add;
    after->next = to_add;
    AWS_POSTCONDITION(aws_linked_list_node_next_is_valid(after));
    AWS_POSTCONDITION(aws_linked_list_node_prev_is_valid(to_add));
    AWS_POSTCONDITION(aws_linked_list_node_next_is_valid(to_add));
    AWS_POSTCONDITION(after->next == to_add);
}

/**
 * Swaps the order two nodes in the linked list.
 */
AWS_STATIC_IMPL void aws_linked_list_swap_nodes(struct aws_linked_list_node *a, struct aws_linked_list_node *b) {
    AWS_PRECONDITION(aws_linked_list_node_prev_is_valid(a));
    AWS_PRECONDITION(aws_linked_list_node_next_is_valid(a));
    AWS_PRECONDITION(aws_linked_list_node_prev_is_valid(b));
    AWS_PRECONDITION(aws_linked_list_node_next_is_valid(b));

    if (a == b) {
        return;
    }

    /* snapshot b's value to avoid clobbering its next/prev pointers if a/b are adjacent */
    struct aws_linked_list_node tmp = *b;
    a->prev->next = b;
    a->next->prev = b;

    tmp.prev->next = a;
    tmp.next->prev = a;

    tmp = *a;
    *a = *b;
    *b = tmp;

    AWS_POSTCONDITION(aws_linked_list_node_prev_is_valid(a));
    AWS_POSTCONDITION(aws_linked_list_node_next_is_valid(a));
    AWS_POSTCONDITION(aws_linked_list_node_prev_is_valid(b));
    AWS_POSTCONDITION(aws_linked_list_node_next_is_valid(b));
}

/**
 * Inserts to_add immediately before before.
 */
AWS_STATIC_IMPL void aws_linked_list_insert_before(
    struct aws_linked_list_node *before,
    struct aws_linked_list_node *to_add) {
    AWS_PRECONDITION(aws_linked_list_node_prev_is_valid(before));
    AWS_PRECONDITION(to_add != NULL);
    to_add->next = before;
    to_add->prev = before->prev;
    before->prev->next = to_add;
    before->prev = to_add;
    AWS_POSTCONDITION(aws_linked_list_node_prev_is_valid(before));
    AWS_POSTCONDITION(aws_linked_list_node_prev_is_valid(to_add));
    AWS_POSTCONDITION(aws_linked_list_node_next_is_valid(to_add));
    AWS_POSTCONDITION(before->prev == to_add);
}

/**
 * Removes the specified node from the list (prev/next point to each other) and
 * returns the next node in the list.
 */
AWS_STATIC_IMPL void aws_linked_list_remove(struct aws_linked_list_node *node) {
    AWS_PRECONDITION(aws_linked_list_node_prev_is_valid(node));
    AWS_PRECONDITION(aws_linked_list_node_next_is_valid(node));
    node->prev->next = node->next;
    node->next->prev = node->prev;
    aws_linked_list_node_reset(node);
    AWS_POSTCONDITION(node->next == NULL && node->prev == NULL);
}

/**
 * Append new_node.
 */
AWS_STATIC_IMPL void aws_linked_list_push_back(struct aws_linked_list *list, struct aws_linked_list_node *node) {
    AWS_PRECONDITION(aws_linked_list_is_valid(list));
    AWS_PRECONDITION(node != NULL);
    aws_linked_list_insert_before(&list->tail, node);
    AWS_POSTCONDITION(aws_linked_list_is_valid(list));
    AWS_POSTCONDITION(list->tail.prev == node, "[node] is the new last element of [list]");
}

/**
 * Returns the element in the back of the list.
 */
AWS_STATIC_IMPL struct aws_linked_list_node *aws_linked_list_back(const struct aws_linked_list *list) {
    AWS_PRECONDITION(aws_linked_list_is_valid(list));
    AWS_PRECONDITION(!aws_linked_list_empty(list));
    struct aws_linked_list_node *rval = list->tail.prev;
    AWS_POSTCONDITION(aws_linked_list_is_valid(list));
    AWS_POSTCONDITION(aws_linked_list_node_prev_is_valid(rval));
    AWS_POSTCONDITION(aws_linked_list_node_next_is_valid(rval));
    return rval;
}

/**
 * Returns the element in the back of the list and removes it
 */
AWS_STATIC_IMPL struct aws_linked_list_node *aws_linked_list_pop_back(struct aws_linked_list *list) {
    AWS_PRECONDITION(!aws_linked_list_empty(list));
    AWS_PRECONDITION(aws_linked_list_is_valid(list));
    struct aws_linked_list_node *back = aws_linked_list_back(list);
    aws_linked_list_remove(back);
    AWS_POSTCONDITION(back->next == NULL && back->prev == NULL);
    AWS_POSTCONDITION(aws_linked_list_is_valid(list));
    return back;
}

/**
 * Prepend new_node.
 */
AWS_STATIC_IMPL void aws_linked_list_push_front(struct aws_linked_list *list, struct aws_linked_list_node *node) {
    AWS_PRECONDITION(aws_linked_list_is_valid(list));
    AWS_PRECONDITION(node != NULL);
    aws_linked_list_insert_before(list->head.next, node);
    AWS_POSTCONDITION(aws_linked_list_is_valid(list));
    AWS_POSTCONDITION(list->head.next == node, "[node] is the new first element of [list]");
}

/**
 * Returns the element in the front of the list.
 */
AWS_STATIC_IMPL struct aws_linked_list_node *aws_linked_list_front(const struct aws_linked_list *list) {
    AWS_PRECONDITION(aws_linked_list_is_valid(list));
    AWS_PRECONDITION(!aws_linked_list_empty(list));
    struct aws_linked_list_node *rval = list->head.next;
    AWS_POSTCONDITION(aws_linked_list_is_valid(list));
    AWS_POSTCONDITION(aws_linked_list_node_prev_is_valid(rval));
    AWS_POSTCONDITION(aws_linked_list_node_next_is_valid(rval));
    return rval;
}

/**
 * Returns the element in the front of the list and removes it
 */
AWS_STATIC_IMPL struct aws_linked_list_node *aws_linked_list_pop_front(struct aws_linked_list *list) {
    AWS_PRECONDITION(!aws_linked_list_empty(list));
    AWS_PRECONDITION(aws_linked_list_is_valid(list));
    struct aws_linked_list_node *front = aws_linked_list_front(list);
    aws_linked_list_remove(front);
    AWS_POSTCONDITION(front->next == NULL && front->prev == NULL);
    AWS_POSTCONDITION(aws_linked_list_is_valid(list));
    return front;
}

AWS_STATIC_IMPL void aws_linked_list_swap_contents(
    struct aws_linked_list *AWS_RESTRICT a,
    struct aws_linked_list *AWS_RESTRICT b) {

    AWS_PRECONDITION(aws_linked_list_is_valid(a));
    AWS_PRECONDITION(aws_linked_list_is_valid(b));
    AWS_PRECONDITION(a != b);
    struct aws_linked_list_node *a_first = a->head.next;
    struct aws_linked_list_node *a_last = a->tail.prev;

    /* Move B's contents into A */
    if (aws_linked_list_empty(b)) {
        aws_linked_list_init(a);
    } else {
        a->head.next = b->head.next;
        a->head.next->prev = &a->head;
        a->tail.prev = b->tail.prev;
        a->tail.prev->next = &a->tail;
    }

    /* Move A's old contents into B */
    if (a_first == &a->tail) {
        aws_linked_list_init(b);
    } else {
        b->head.next = a_first;
        b->head.next->prev = &b->head;
        b->tail.prev = a_last;
        b->tail.prev->next = &b->tail;
    }
    AWS_POSTCONDITION(aws_linked_list_is_valid(a));
    AWS_POSTCONDITION(aws_linked_list_is_valid(b));
}

AWS_STATIC_IMPL void aws_linked_list_move_all_back(
    struct aws_linked_list *AWS_RESTRICT dst,
    struct aws_linked_list *AWS_RESTRICT src) {

    AWS_PRECONDITION(aws_linked_list_is_valid(src));
    AWS_PRECONDITION(aws_linked_list_is_valid(dst));
    AWS_PRECONDITION(dst != src);

    if (!aws_linked_list_empty(src)) {
        /* splice src nodes into dst, between the back and tail nodes */
        struct aws_linked_list_node *dst_back = dst->tail.prev;
        struct aws_linked_list_node *src_front = src->head.next;
        struct aws_linked_list_node *src_back = src->tail.prev;

        dst_back->next = src_front;
        src_front->prev = dst_back;

        dst->tail.prev = src_back;
        src_back->next = &dst->tail;

        /* reset src */
        src->head.next = &src->tail;
        src->tail.prev = &src->head;
    }

    AWS_POSTCONDITION(aws_linked_list_is_valid(src));
    AWS_POSTCONDITION(aws_linked_list_is_valid(dst));
}

AWS_STATIC_IMPL void aws_linked_list_move_all_front(
    struct aws_linked_list *AWS_RESTRICT dst,
    struct aws_linked_list *AWS_RESTRICT src) {

    AWS_PRECONDITION(aws_linked_list_is_valid(src));
    AWS_PRECONDITION(aws_linked_list_is_valid(dst));
    AWS_PRECONDITION(dst != src);

    if (!aws_linked_list_empty(src)) {
        /* splice src nodes into dst, between the head and front nodes */
        struct aws_linked_list_node *dst_front = dst->head.next;
        struct aws_linked_list_node *src_front = src->head.next;
        struct aws_linked_list_node *src_back = src->tail.prev;

        dst->head.next = src_front;
        src_front->prev = &dst->head;

        src_back->next = dst_front;
        dst_front->prev = src_back;

        /* reset src */
        src->head.next = &src->tail;
        src->tail.prev = &src->head;
    }

    AWS_POSTCONDITION(aws_linked_list_is_valid(src));
    AWS_POSTCONDITION(aws_linked_list_is_valid(dst));
}

AWS_STATIC_IMPL bool aws_linked_list_node_is_in_list(struct aws_linked_list_node *node) {
    return aws_linked_list_node_prev_is_valid(node) && aws_linked_list_node_next_is_valid(node);
}

AWS_EXTERN_C_END

#endif /* AWS_COMMON_LINKED_LIST_INL */

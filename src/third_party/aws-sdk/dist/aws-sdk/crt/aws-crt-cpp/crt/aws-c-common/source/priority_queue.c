/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/priority_queue.h>

#include <string.h>

#define PARENT_OF(index) (((index) & 1) ? (index) >> 1 : (index) > 1 ? ((index) - 2) >> 1 : 0)
#define LEFT_OF(index) (((index) << 1) + 1)
#define RIGHT_OF(index) (((index) << 1) + 2)

static void s_swap(struct aws_priority_queue *queue, size_t a, size_t b) {
    AWS_PRECONDITION(aws_priority_queue_is_valid(queue));
    AWS_PRECONDITION(a < queue->container.length);
    AWS_PRECONDITION(b < queue->container.length);
    AWS_PRECONDITION(aws_priority_queue_backpointer_index_valid(queue, a));
    AWS_PRECONDITION(aws_priority_queue_backpointer_index_valid(queue, b));

    aws_array_list_swap(&queue->container, a, b);

    /* Invariant: If the backpointer array is initialized, we have enough room for all elements */
    if (!AWS_IS_ZEROED(queue->backpointers)) {
        AWS_ASSERT(queue->backpointers.length > a);
        AWS_ASSERT(queue->backpointers.length > b);

        struct aws_priority_queue_node **bp_a = &((struct aws_priority_queue_node **)queue->backpointers.data)[a];
        struct aws_priority_queue_node **bp_b = &((struct aws_priority_queue_node **)queue->backpointers.data)[b];

        struct aws_priority_queue_node *tmp = *bp_a;
        *bp_a = *bp_b;
        *bp_b = tmp;

        if (*bp_a) {
            (*bp_a)->current_index = a;
        }

        if (*bp_b) {
            (*bp_b)->current_index = b;
        }
    }
    AWS_POSTCONDITION(aws_priority_queue_is_valid(queue));
    AWS_POSTCONDITION(aws_priority_queue_backpointer_index_valid(queue, a));
    AWS_POSTCONDITION(aws_priority_queue_backpointer_index_valid(queue, b));
}

/* Precondition: with the exception of the given root element, the container must be
 * in heap order */
static bool s_sift_down(struct aws_priority_queue *queue, size_t root) {
    AWS_PRECONDITION(aws_priority_queue_is_valid(queue));
    AWS_PRECONDITION(root < queue->container.length);

    bool did_move = false;

    size_t len = aws_array_list_length(&queue->container);

    while (LEFT_OF(root) < len) {
        size_t left = LEFT_OF(root);
        size_t right = RIGHT_OF(root);
        size_t first = root;
        void *first_item = NULL;
        void *other_item = NULL;

        aws_array_list_get_at_ptr(&queue->container, &first_item, root);
        aws_array_list_get_at_ptr(&queue->container, &other_item, left);

        if (queue->pred(first_item, other_item) > 0) {
            first = left;
            first_item = other_item;
        }

        if (right < len) {
            aws_array_list_get_at_ptr(&queue->container, &other_item, right);

            /* choose the larger/smaller of the two in case of a max/min heap
             * respectively */
            if (queue->pred(first_item, other_item) > 0) {
                first = right;
                first_item = other_item;
            }
        }

        if (first != root) {
            s_swap(queue, first, root);
            did_move = true;
            root = first;
        } else {
            break;
        }
    }

    AWS_POSTCONDITION(aws_priority_queue_is_valid(queue));
    return did_move;
}

/* Precondition: Elements prior to the specified index must be in heap order. */
static bool s_sift_up(struct aws_priority_queue *queue, size_t index) {
    AWS_PRECONDITION(aws_priority_queue_is_valid(queue));
    AWS_PRECONDITION(index < queue->container.length);

    bool did_move = false;

    void *parent_item = NULL;
    void *child_item = NULL;
    size_t parent = PARENT_OF(index);
    while (index) {
        /*
         * These get_ats are guaranteed to be successful; if they are not, we have
         * serious state corruption, so just abort.
         */

        if (aws_array_list_get_at_ptr(&queue->container, &parent_item, parent) ||
            aws_array_list_get_at_ptr(&queue->container, &child_item, index)) {
            abort();
        }

        if (queue->pred(parent_item, child_item) > 0) {
            s_swap(queue, index, parent);
            did_move = true;
            index = parent;
            parent = PARENT_OF(index);
        } else {
            break;
        }
    }

    AWS_POSTCONDITION(aws_priority_queue_is_valid(queue));
    return did_move;
}

/*
 * Precondition: With the exception of the given index, the heap condition holds for all elements.
 * In particular, the parent of the current index is a predecessor of all children of the current index.
 */
static void s_sift_either(struct aws_priority_queue *queue, size_t index) {
    AWS_PRECONDITION(aws_priority_queue_is_valid(queue));
    AWS_PRECONDITION(index < queue->container.length);

    if (!index || !s_sift_up(queue, index)) {
        s_sift_down(queue, index);
    }

    AWS_POSTCONDITION(aws_priority_queue_is_valid(queue));
}

int aws_priority_queue_init_dynamic(
    struct aws_priority_queue *queue,
    struct aws_allocator *alloc,
    size_t default_size,
    size_t item_size,
    aws_priority_queue_compare_fn *pred) {

    AWS_FATAL_PRECONDITION(queue != NULL);
    AWS_FATAL_PRECONDITION(alloc != NULL);
    AWS_FATAL_PRECONDITION(item_size > 0);

    queue->pred = pred;
    AWS_ZERO_STRUCT(queue->backpointers);

    int ret = aws_array_list_init_dynamic(&queue->container, alloc, default_size, item_size);
    if (ret == AWS_OP_SUCCESS) {
        AWS_POSTCONDITION(aws_priority_queue_is_valid(queue));
    } else {
        AWS_POSTCONDITION(AWS_IS_ZEROED(queue->container));
        AWS_POSTCONDITION(AWS_IS_ZEROED(queue->backpointers));
    }
    return ret;
}

void aws_priority_queue_init_static(
    struct aws_priority_queue *queue,
    void *heap,
    size_t item_count,
    size_t item_size,
    aws_priority_queue_compare_fn *pred) {

    AWS_FATAL_PRECONDITION(queue != NULL);
    AWS_FATAL_PRECONDITION(heap != NULL);
    AWS_FATAL_PRECONDITION(item_count > 0);
    AWS_FATAL_PRECONDITION(item_size > 0);

    queue->pred = pred;
    AWS_ZERO_STRUCT(queue->backpointers);

    aws_array_list_init_static(&queue->container, heap, item_count, item_size);

    AWS_POSTCONDITION(aws_priority_queue_is_valid(queue));
}

bool aws_priority_queue_backpointer_index_valid(const struct aws_priority_queue *const queue, size_t index) {
    if (AWS_IS_ZEROED(queue->backpointers)) {
        return true;
    }
    if (index < queue->backpointers.length) {
        struct aws_priority_queue_node *node = ((struct aws_priority_queue_node **)queue->backpointers.data)[index];
        return (node == NULL) || AWS_MEM_IS_WRITABLE(node, sizeof(struct aws_priority_queue_node));
    }
    return false;
}

bool aws_priority_queue_backpointers_valid_deep(const struct aws_priority_queue *const queue) {
    if (!queue) {
        return false;
    }
    for (size_t i = 0; i < queue->backpointers.length; i++) {
        if (!aws_priority_queue_backpointer_index_valid(queue, i)) {
            return false;
        }
    }
    return true;
}

bool aws_priority_queue_backpointers_valid(const struct aws_priority_queue *const queue) {
    if (!queue) {
        return false;
    }

    /* Internal container validity */
    bool backpointer_list_is_valid =
        (aws_array_list_is_valid(&queue->backpointers) && (queue->backpointers.current_size != 0) &&
         (queue->backpointers.data != NULL));

    /* Backpointer struct should either be zero or should be
     * initialized to be at most as long as the container, and having
     * as elements potentially null pointers to
     * aws_priority_queue_nodes */
    bool backpointer_list_item_size = queue->backpointers.item_size == sizeof(struct aws_priority_queue_node *);
    bool lists_equal_lengths = queue->backpointers.length == queue->container.length;
    bool backpointers_non_zero_current_size = queue->backpointers.current_size > 0;

    /* This check must be guarded, as it is not efficient, neither
     * when running tests nor CBMC */
#if (AWS_DEEP_CHECKS == 1)
    bool backpointers_valid_deep = aws_priority_queue_backpointers_valid_deep(queue);
#else
    bool backpointers_valid_deep = true;
#endif
    bool backpointers_zero =
        (queue->backpointers.current_size == 0 && queue->backpointers.length == 0 && queue->backpointers.data == NULL);
    bool backpointer_struct_is_valid =
        backpointers_zero || (backpointer_list_item_size && lists_equal_lengths && backpointers_non_zero_current_size &&
                              backpointers_valid_deep);

    return ((backpointer_list_is_valid && backpointer_struct_is_valid) || AWS_IS_ZEROED(queue->backpointers));
}

bool aws_priority_queue_is_valid(const struct aws_priority_queue *const queue) {
    /* Pointer validity checks */
    if (!queue) {
        return false;
    }
    bool pred_is_valid = (queue->pred != NULL);
    bool container_is_valid = aws_array_list_is_valid(&queue->container);

    bool backpointers_valid = aws_priority_queue_backpointers_valid(queue);
    return pred_is_valid && container_is_valid && backpointers_valid;
}

void aws_priority_queue_clean_up(struct aws_priority_queue *queue) {
    aws_array_list_clean_up(&queue->container);
    if (!AWS_IS_ZEROED(queue->backpointers)) {
        aws_array_list_clean_up(&queue->backpointers);
    }
}

int aws_priority_queue_push(struct aws_priority_queue *queue, void *item) {
    AWS_PRECONDITION(aws_priority_queue_is_valid(queue));
    AWS_PRECONDITION(item && AWS_MEM_IS_READABLE(item, queue->container.item_size));
    int rval = aws_priority_queue_push_ref(queue, item, NULL);
    AWS_POSTCONDITION(aws_priority_queue_is_valid(queue));
    return rval;
}

int aws_priority_queue_push_ref(
    struct aws_priority_queue *queue,
    void *item,
    struct aws_priority_queue_node *backpointer) {
    AWS_PRECONDITION(aws_priority_queue_is_valid(queue));
    AWS_PRECONDITION(item && AWS_MEM_IS_READABLE(item, queue->container.item_size));

    int err = aws_array_list_push_back(&queue->container, item);
    if (err) {
        AWS_POSTCONDITION(aws_priority_queue_is_valid(queue));
        return err;
    }
    size_t index = aws_array_list_length(&queue->container) - 1;

    if (backpointer && !queue->backpointers.alloc) {
        if (!queue->container.alloc) {
            aws_raise_error(AWS_ERROR_UNSUPPORTED_OPERATION);
            goto backpointer_update_failed;
        }

        if (aws_array_list_init_dynamic(
                &queue->backpointers, queue->container.alloc, index + 1, sizeof(struct aws_priority_queue_node *))) {
            goto backpointer_update_failed;
        }

        /* When we initialize the backpointers array we need to zero out all existing entries */
        memset(queue->backpointers.data, 0, queue->backpointers.current_size);
    }

    /*
     * Once we have any backpointers, we want to make sure we always have room in the backpointers array
     * for all elements; otherwise, sift_down gets complicated if it runs out of memory when sifting an
     * element with a backpointer down in the array.
     */
    if (!AWS_IS_ZEROED(queue->backpointers)) {
        if (aws_array_list_set_at(&queue->backpointers, &backpointer, index)) {
            goto backpointer_update_failed;
        }
    }

    if (backpointer) {
        backpointer->current_index = index;
    }

    s_sift_up(queue, aws_array_list_length(&queue->container) - 1);

    AWS_POSTCONDITION(aws_priority_queue_is_valid(queue));
    return AWS_OP_SUCCESS;

backpointer_update_failed:
    /* Failed to initialize or grow the backpointer array, back out the node addition */
    aws_array_list_pop_back(&queue->container);
    AWS_POSTCONDITION(aws_priority_queue_is_valid(queue));
    return AWS_OP_ERR;
}

static int s_remove_node(struct aws_priority_queue *queue, void *item, size_t item_index) {
    AWS_PRECONDITION(aws_priority_queue_is_valid(queue));
    AWS_PRECONDITION(item && AWS_MEM_IS_WRITABLE(item, queue->container.item_size));
    if (aws_array_list_get_at(&queue->container, item, item_index)) {
        /* shouldn't happen, but if it does we've already raised an error... */
        AWS_POSTCONDITION(aws_priority_queue_is_valid(queue));
        return AWS_OP_ERR;
    }

    size_t swap_with = aws_array_list_length(&queue->container) - 1;
    struct aws_priority_queue_node *backpointer = NULL;

    if (item_index != swap_with) {
        s_swap(queue, item_index, swap_with);
    }

    aws_array_list_pop_back(&queue->container);

    if (!AWS_IS_ZEROED(queue->backpointers)) {
        aws_array_list_get_at(&queue->backpointers, &backpointer, swap_with);
        if (backpointer) {
            backpointer->current_index = SIZE_MAX;
        }
        aws_array_list_pop_back(&queue->backpointers);
    }

    if (item_index != swap_with) {
        s_sift_either(queue, item_index);
    }

    AWS_POSTCONDITION(aws_priority_queue_is_valid(queue));
    return AWS_OP_SUCCESS;
}

int aws_priority_queue_remove(
    struct aws_priority_queue *queue,
    void *item,
    const struct aws_priority_queue_node *node) {
    AWS_PRECONDITION(aws_priority_queue_is_valid(queue));
    AWS_PRECONDITION(item && AWS_MEM_IS_WRITABLE(item, queue->container.item_size));
    AWS_PRECONDITION(node && AWS_MEM_IS_READABLE(node, sizeof(struct aws_priority_queue_node)));
    AWS_ERROR_PRECONDITION(
        node->current_index < aws_array_list_length(&queue->container), AWS_ERROR_PRIORITY_QUEUE_BAD_NODE);
    AWS_ERROR_PRECONDITION(queue->backpointers.data, AWS_ERROR_PRIORITY_QUEUE_BAD_NODE);

    int rval = s_remove_node(queue, item, node->current_index);
    AWS_POSTCONDITION(aws_priority_queue_is_valid(queue));
    return rval;
}

int aws_priority_queue_pop(struct aws_priority_queue *queue, void *item) {
    AWS_PRECONDITION(aws_priority_queue_is_valid(queue));
    AWS_PRECONDITION(item && AWS_MEM_IS_WRITABLE(item, queue->container.item_size));
    AWS_ERROR_PRECONDITION(aws_array_list_length(&queue->container) != 0, AWS_ERROR_PRIORITY_QUEUE_EMPTY);

    int rval = s_remove_node(queue, item, 0);
    AWS_POSTCONDITION(aws_priority_queue_is_valid(queue));
    return rval;
}

int aws_priority_queue_top(const struct aws_priority_queue *queue, void **item) {
    AWS_ERROR_PRECONDITION(aws_array_list_length(&queue->container) != 0, AWS_ERROR_PRIORITY_QUEUE_EMPTY);
    return aws_array_list_get_at_ptr(&queue->container, item, 0);
}

size_t aws_priority_queue_size(const struct aws_priority_queue *queue) {
    return aws_array_list_length(&queue->container);
}

size_t aws_priority_queue_capacity(const struct aws_priority_queue *queue) {
    return aws_array_list_capacity(&queue->container);
}

void aws_priority_queue_clear(struct aws_priority_queue *queue) {
    AWS_PRECONDITION(aws_priority_queue_is_valid(queue));
    size_t backpointer_count = aws_array_list_length(&queue->backpointers);
    for (size_t i = 0; i < backpointer_count; ++i) {
        struct aws_priority_queue_node *node = NULL;
        aws_array_list_get_at(&queue->backpointers, &node, i);
        if (node != NULL) {
            node->current_index = SIZE_MAX;
        }
    }

    aws_array_list_clear(&queue->backpointers);
    aws_array_list_clear(&queue->container);
    AWS_PRECONDITION(aws_priority_queue_is_valid(queue));
}

void aws_priority_queue_node_init(struct aws_priority_queue_node *node) {
    node->current_index = SIZE_MAX;
}

bool aws_priority_queue_node_is_in_queue(const struct aws_priority_queue_node *node) {
    return node->current_index != SIZE_MAX;
}

#ifndef AWS_COMMON_PRIORITY_QUEUE_H
#define AWS_COMMON_PRIORITY_QUEUE_H
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/array_list.h>
#include <aws/common/common.h>

AWS_PUSH_SANE_WARNING_LEVEL

/* The comparator should return a positive value if the second argument has a
 * higher priority than the first; Otherwise, it should return a negative value
 * or zero. NOTE: priority_queue pops its highest priority element first. For
 * example: int cmp(const void *a, const void *b) { return a < b; } would result
 * in a max heap, while: int cmp(const void *a, const void *b) { return a > b; }
 * would result in a min heap.
 */
typedef int(aws_priority_queue_compare_fn)(const void *a, const void *b);

struct aws_priority_queue {
    /**
     * predicate that determines the priority of the elements in the queue.
     */
    aws_priority_queue_compare_fn *pred;

    /**
     * The underlying container storing the queue elements.
     */
    struct aws_array_list container;

    /**
     * An array of pointers to backpointer elements. This array is initialized when
     * the first call to aws_priority_queue_push_bp is made, and is subsequently maintained
     * through any heap node manipulations.
     *
     * Each element is a struct aws_priority_queue_node *, pointing to a backpointer field
     * owned by the calling code, or a NULL. The backpointer field is continually updated
     * with information needed to locate and remove a specific node later on.
     */
    struct aws_array_list backpointers;
};

struct aws_priority_queue_node {
    /** The current index of the node in question, or SIZE_MAX if the node has been removed. */
    size_t current_index;
};

AWS_EXTERN_C_BEGIN

/**
 * Initializes a priority queue struct for use. This mode will grow memory automatically (exponential model)
 * Default size is the inital size of the queue
 * item_size is the size of each element in bytes. Mixing items types is not supported by this API.
 * pred is the function that will be used to determine priority.
 */
AWS_COMMON_API
int aws_priority_queue_init_dynamic(
    struct aws_priority_queue *queue,
    struct aws_allocator *alloc,
    size_t default_size,
    size_t item_size,
    aws_priority_queue_compare_fn *pred);

/**
 * Initializes a priority queue struct for use. This mode will not allocate any additional memory. When the heap fills
 * new enqueue operations will fail with AWS_ERROR_PRIORITY_QUEUE_FULL.
 *
 * Heaps initialized using this call do not support the aws_priority_queue_push_ref call with a non-NULL backpointer
 * parameter.
 *
 * heap is the raw memory allocated for this priority_queue
 * item_count is the maximum number of elements the raw heap can contain
 * item_size is the size of each element in bytes. Mixing items types is not supported by this API.
 * pred is the function that will be used to determine priority.
 */
AWS_COMMON_API
void aws_priority_queue_init_static(
    struct aws_priority_queue *queue,
    void *heap,
    size_t item_count,
    size_t item_size,
    aws_priority_queue_compare_fn *pred);

/**
 * Checks that the backpointer at a specific index of the queue is
 * NULL or points to a correctly allocated aws_priority_queue_node.
 */
bool aws_priority_queue_backpointer_index_valid(const struct aws_priority_queue *const queue, size_t index);

/**
 * Checks that the backpointers of the priority queue are either NULL
 * or correctly allocated to point at aws_priority_queue_nodes. This
 * check is O(n), as it accesses every backpointer in a loop, and thus
 * shouldn't be used carelessly.
 */
bool aws_priority_queue_backpointers_valid_deep(const struct aws_priority_queue *const queue);

/**
 * Checks that the backpointers of the priority queue satisfy validity
 * constraints.
 */
bool aws_priority_queue_backpointers_valid(const struct aws_priority_queue *const queue);

/**
 * Set of properties of a valid aws_priority_queue.
 */
AWS_COMMON_API
bool aws_priority_queue_is_valid(const struct aws_priority_queue *const queue);

/**
 * Cleans up any internally allocated memory and resets the struct for reuse or deletion.
 */
AWS_COMMON_API
void aws_priority_queue_clean_up(struct aws_priority_queue *queue);

/**
 * Copies item into the queue and places it in the proper priority order. Complexity: O(log(n)).
 */
AWS_COMMON_API
int aws_priority_queue_push(struct aws_priority_queue *queue, void *item);

/**
 * Copies item into the queue and places it in the proper priority order. Complexity: O(log(n)).
 *
 * If the backpointer parameter is non-null, the heap will continually update the pointed-to field
 * with information needed to remove the node later on. *backpointer must remain valid until the node
 * is removed from the heap, and may be updated on any mutating operation on the priority queue.
 *
 * If the node is removed, the backpointer will be set to a sentinel value that indicates that the
 * node has already been removed. It is safe (and a no-op) to call aws_priority_queue_remove with
 * such a sentinel value.
 */
AWS_COMMON_API
int aws_priority_queue_push_ref(
    struct aws_priority_queue *queue,
    void *item,
    struct aws_priority_queue_node *backpointer);

/**
 * Copies the element of the highest priority, and removes it from the queue.. Complexity: O(log(n)).
 * If queue is empty, AWS_ERROR_PRIORITY_QUEUE_EMPTY will be raised.
 */
AWS_COMMON_API
int aws_priority_queue_pop(struct aws_priority_queue *queue, void *item);

/**
 * Removes a specific node from the priority queue. Complexity: O(log(n))
 * After removing a node (using either _remove or _pop), the backpointer set at push_ref time is set
 * to a sentinel value. If this sentinel value is passed to aws_priority_queue_remove,
 * AWS_ERROR_PRIORITY_QUEUE_BAD_NODE will be raised. Note, however, that passing uninitialized
 * aws_priority_queue_nodes, or ones from different priority queues, results in undefined behavior.
 */
AWS_COMMON_API
int aws_priority_queue_remove(struct aws_priority_queue *queue, void *item, const struct aws_priority_queue_node *node);

/**
 * Obtains a pointer to the element of the highest priority. Complexity: constant time.
 * If queue is empty, AWS_ERROR_PRIORITY_QUEUE_EMPTY will be raised.
 */
AWS_COMMON_API
int aws_priority_queue_top(const struct aws_priority_queue *queue, void **item);

/**
 * Removes all elements from the queue, but does not free internal memory.
 */
AWS_COMMON_API
void aws_priority_queue_clear(struct aws_priority_queue *queue);

/**
 * Current number of elements in the queue
 */
AWS_COMMON_API
size_t aws_priority_queue_size(const struct aws_priority_queue *queue);

/**
 * Current allocated capacity for the queue, in dynamic mode this grows over time, in static mode, this will never
 * change.
 */
AWS_COMMON_API
size_t aws_priority_queue_capacity(const struct aws_priority_queue *queue);

/**
 * Initializes a queue node to a default value that indicates the node is not in the queue.
 *
 * @param node priority queue node to initialize with a default value
 */
AWS_COMMON_API
void aws_priority_queue_node_init(struct aws_priority_queue_node *node);

/**
 * Checks if a priority queue node is currently in a priority queue.
 *
 * @param node priority queue node to check usage for
 *
 * @return true if the node is in a queue, false otherwise
 */
AWS_COMMON_API
bool aws_priority_queue_node_is_in_queue(const struct aws_priority_queue_node *node);

AWS_EXTERN_C_END
AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_COMMON_PRIORITY_QUEUE_H */

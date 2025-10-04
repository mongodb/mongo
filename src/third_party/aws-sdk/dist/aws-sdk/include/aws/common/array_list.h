#ifndef AWS_COMMON_ARRAY_LIST_H
#define AWS_COMMON_ARRAY_LIST_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/common/common.h>
#include <aws/common/math.h>

#include <stdlib.h>

AWS_PUSH_SANE_WARNING_LEVEL

enum { AWS_ARRAY_LIST_DEBUG_FILL = 0xDD };

struct aws_array_list {
    struct aws_allocator *alloc;
    size_t current_size;
    size_t length;
    size_t item_size;
    void *data;
};

/**
 * Prototype for a comparator function for sorting elements.
 *
 * a and b should be cast to pointers to the element type held in the list
 * before being dereferenced. The function should compare the elements and
 * return a positive number if a > b, zero if a = b, and a negative number
 * if a < b.
 */
typedef int(aws_array_list_comparator_fn)(const void *a, const void *b);

AWS_EXTERN_C_BEGIN

/**
 * Initializes an array list with an array of size initial_item_allocation * item_size. In this mode, the array size
 * will grow by a factor of 2 upon insertion if space is not available. initial_item_allocation is the number of
 * elements you want space allocated for. item_size is the size of each element in bytes. Mixing items types is not
 * supported by this API.
 */
AWS_STATIC_IMPL
int aws_array_list_init_dynamic(
    struct aws_array_list *AWS_RESTRICT list,
    struct aws_allocator *alloc,
    size_t initial_item_allocation,
    size_t item_size);

/**
 * Initializes an array list with a preallocated array of void *. item_count is the number of elements in the array,
 * and item_size is the size in bytes of each element. Mixing items types is not supported
 * by this API. Once this list is full, new items will be rejected.
 */
AWS_STATIC_IMPL
void aws_array_list_init_static(
    struct aws_array_list *AWS_RESTRICT list,
    void *raw_array,
    size_t item_count,
    size_t item_size);

/**
 * Initializes an array list with a preallocated array of *already-initialized* elements. item_count is the number of
 * elements in the array, and item_size is the size in bytes of each element.
 *
 * Once initialized, nothing further can be added to the list, since it will be full and cannot resize.
 *
 * Primary use case is to treat an already-initialized C array as an array list.
 */
AWS_STATIC_IMPL
void aws_array_list_init_static_from_initialized(
    struct aws_array_list *AWS_RESTRICT list,
    void *raw_array,
    size_t item_count,
    size_t item_size);

/**
 * Set of properties of a valid aws_array_list.
 */
AWS_STATIC_IMPL
bool aws_array_list_is_valid(const struct aws_array_list *AWS_RESTRICT list);

/**
 * Deallocates any memory that was allocated for this list, and resets list for reuse or deletion.
 */
AWS_STATIC_IMPL
void aws_array_list_clean_up(struct aws_array_list *AWS_RESTRICT list);

/**
 * Erases and then deallocates any memory that was allocated for this list, and resets list for reuse or deletion.
 */
AWS_STATIC_IMPL
void aws_array_list_clean_up_secure(struct aws_array_list *AWS_RESTRICT list);

/**
 * Pushes the memory pointed to by val onto the end of internal list
 */
AWS_STATIC_IMPL
int aws_array_list_push_back(struct aws_array_list *AWS_RESTRICT list, const void *val);

/**
 * Copies the element at the front of the list if it exists. If list is empty, AWS_ERROR_LIST_EMPTY will be raised
 */
AWS_STATIC_IMPL
int aws_array_list_front(const struct aws_array_list *AWS_RESTRICT list, void *val);

/**
 * Pushes the memory pointed to by val onto the front of internal list.
 * This call results in shifting all of the elements in the list. Avoid this call unless that
 * is intended behavior.
 */
AWS_STATIC_IMPL
int aws_array_list_push_front(struct aws_array_list *AWS_RESTRICT list, const void *val);

/**
 * Deletes the element at the front of the list if it exists. If list is empty, AWS_ERROR_LIST_EMPTY will be raised.
 * This call results in shifting all of the elements at the end of the array to the front. Avoid this call unless that
 * is intended behavior.
 */
AWS_STATIC_IMPL
int aws_array_list_pop_front(struct aws_array_list *AWS_RESTRICT list);

/**
 * Delete N elements from the front of the list.
 * Remaining elements are shifted to the front of the list.
 * If the list has less than N elements, the list is cleared.
 * This call is more efficient than calling aws_array_list_pop_front() N times.
 */
AWS_STATIC_IMPL
void aws_array_list_pop_front_n(struct aws_array_list *AWS_RESTRICT list, size_t n);

/**
 * Deletes the element this index in the list if it exists.
 * If element does not exist, AWS_ERROR_INVALID_INDEX will be raised.
 * This call results in shifting all remaining elements towards the front.
 * Avoid this call unless that is intended behavior.
 */
AWS_STATIC_IMPL
int aws_array_list_erase(struct aws_array_list *AWS_RESTRICT list, size_t index);

/**
 * Copies the element at the end of the list if it exists. If list is empty, AWS_ERROR_LIST_EMPTY will be raised.
 */
AWS_STATIC_IMPL
int aws_array_list_back(const struct aws_array_list *AWS_RESTRICT list, void *val);

/**
 * Deletes the element at the end of the list if it exists. If list is empty, AWS_ERROR_LIST_EMPTY will be raised.
 */
AWS_STATIC_IMPL
int aws_array_list_pop_back(struct aws_array_list *AWS_RESTRICT list);

/**
 * Clears all elements in the array and resets length to zero. Size does not change in this operation.
 */
AWS_STATIC_IMPL
void aws_array_list_clear(struct aws_array_list *AWS_RESTRICT list);

/**
 * If in dynamic mode, shrinks the allocated array size to the minimum amount necessary to store its elements.
 */
AWS_COMMON_API
int aws_array_list_shrink_to_fit(struct aws_array_list *AWS_RESTRICT list);

/**
 * Copies the elements from from to to. If to is in static mode, it must at least be the same length as from. Any data
 * in to will be overwritten in this copy.
 */
AWS_COMMON_API
int aws_array_list_copy(const struct aws_array_list *AWS_RESTRICT from, struct aws_array_list *AWS_RESTRICT to);

/**
 * Swap contents between two dynamic lists. Both lists must use the same allocator.
 */
AWS_STATIC_IMPL
void aws_array_list_swap_contents(
    struct aws_array_list *AWS_RESTRICT list_a,
    struct aws_array_list *AWS_RESTRICT list_b);

/**
 * Returns the number of elements that can fit in the internal array. If list is initialized in dynamic mode,
 * the capacity changes over time.
 */
AWS_STATIC_IMPL
size_t aws_array_list_capacity(const struct aws_array_list *AWS_RESTRICT list);

/**
 * Returns the number of elements in the internal array.
 */
AWS_STATIC_IMPL
size_t aws_array_list_length(const struct aws_array_list *AWS_RESTRICT list);

/**
 * Copies the memory at index to val. If element does not exist, AWS_ERROR_INVALID_INDEX will be raised.
 */
AWS_STATIC_IMPL
int aws_array_list_get_at(const struct aws_array_list *AWS_RESTRICT list, void *val, size_t index);

/**
 * Copies the memory address of the element at index to *val. If element does not exist, AWS_ERROR_INVALID_INDEX will be
 * raised.
 */
AWS_STATIC_IMPL
int aws_array_list_get_at_ptr(const struct aws_array_list *AWS_RESTRICT list, void **val, size_t index);

/**
 * Ensures that the array list has enough capacity to store a value at the specified index. If there is not already
 * enough capacity, and the list is in dynamic mode, this function will attempt to allocate more memory, expanding the
 * list. In static mode, if 'index' is beyond the maximum index, AWS_ERROR_INVALID_INDEX will be raised.
 */
AWS_COMMON_API
int aws_array_list_ensure_capacity(struct aws_array_list *AWS_RESTRICT list, size_t index);

/**
 * Copies the the memory pointed to by val into the array at index. If in dynamic mode, the size will grow by a factor
 * of two when the array is full. In static mode, AWS_ERROR_INVALID_INDEX will be raised if the index is past the bounds
 * of the array.
 */
AWS_STATIC_IMPL
int aws_array_list_set_at(struct aws_array_list *AWS_RESTRICT list, const void *val, size_t index);

/**
 * Swap elements at the specified indices, which must be within the bounds of the array.
 */
AWS_COMMON_API
void aws_array_list_swap(struct aws_array_list *AWS_RESTRICT list, size_t a, size_t b);

/**
 * Sort elements in the list in-place according to the comparator function.
 */
AWS_COMMON_API
void aws_array_list_sort(struct aws_array_list *AWS_RESTRICT list, aws_array_list_comparator_fn *compare_fn);

AWS_EXTERN_C_END
#ifndef AWS_NO_STATIC_IMPL
#    include <aws/common/array_list.inl>
#endif /* AWS_NO_STATIC_IMPL */

AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_COMMON_ARRAY_LIST_H */

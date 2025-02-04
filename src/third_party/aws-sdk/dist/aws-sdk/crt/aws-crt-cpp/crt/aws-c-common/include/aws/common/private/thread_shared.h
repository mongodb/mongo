#ifndef AWS_COMMON_PRIVATE_THREAD_SHARED_H
#define AWS_COMMON_PRIVATE_THREAD_SHARED_H
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/thread.h>

struct aws_linked_list;
struct aws_linked_list_node;

/**
 * Iterates a list of thread wrappers, joining against each corresponding thread, and freeing the wrapper once
 * the join has completed.  Do not hold the managed thread lock when invoking this function, instead swap the
 * pending join list into a local and call this on the local.
 *
 * @param wrapper_list list of thread wrappers to join and free
 */
AWS_COMMON_API void aws_thread_join_and_free_wrapper_list(struct aws_linked_list *wrapper_list);

/**
 * Adds a thread (wrapper embedding a linked list node) to the global list of threads that have run to completion
 * and need a join in order to know that the OS has truly finished with the thread.
 * @param node linked list node embedded in the thread wrapper
 */
AWS_COMMON_API void aws_thread_pending_join_add(struct aws_linked_list_node *node);

/**
 * Initializes the managed thread system.  Called during library init.
 */
AWS_COMMON_API void aws_thread_initialize_thread_management(void);

/**
 * Gets the current managed thread count
 */
AWS_COMMON_API size_t aws_thread_get_managed_thread_count(void);

#endif /* AWS_COMMON_PRIVATE_THREAD_SHARED_H */

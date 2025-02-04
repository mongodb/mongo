/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/common/clock.h>
#include <aws/common/condition_variable.h>
#include <aws/common/mutex.h>
#include <aws/common/ref_count.h>
#include <aws/common/task_scheduler.h>
#include <aws/common/thread.h>
#include <aws/common/thread_scheduler.h>

struct aws_thread_scheduler {
    struct aws_allocator *allocator;
    struct aws_ref_count ref_count;
    struct aws_thread thread;
    struct aws_task_scheduler scheduler;
    struct aws_atomic_var should_exit;

    struct {
        struct aws_linked_list scheduling_queue;
        struct aws_linked_list cancel_queue;
        struct aws_mutex mutex;
        struct aws_condition_variable c_var;
    } thread_data;
};

struct cancellation_node {
    struct aws_task *task_to_cancel;
    struct aws_linked_list_node node;
};

static void s_destroy_callback(void *arg) {
    struct aws_thread_scheduler *scheduler = arg;
    aws_atomic_store_int(&scheduler->should_exit, 1U);
    aws_condition_variable_notify_all(&scheduler->thread_data.c_var);
    aws_thread_join(&scheduler->thread);
    aws_task_scheduler_clean_up(&scheduler->scheduler);
    aws_condition_variable_clean_up(&scheduler->thread_data.c_var);
    aws_mutex_clean_up(&scheduler->thread_data.mutex);
    aws_thread_clean_up(&scheduler->thread);
    aws_mem_release(scheduler->allocator, scheduler);
}

static bool s_thread_should_wake(void *arg) {
    struct aws_thread_scheduler *scheduler = arg;

    uint64_t current_time = 0;
    aws_high_res_clock_get_ticks(&current_time);

    uint64_t next_scheduled_task = 0;
    aws_task_scheduler_has_tasks(&scheduler->scheduler, &next_scheduled_task);
    return aws_atomic_load_int(&scheduler->should_exit) ||
           !aws_linked_list_empty(&scheduler->thread_data.scheduling_queue) ||
           !aws_linked_list_empty(&scheduler->thread_data.cancel_queue) || (next_scheduled_task <= current_time);
}

static void s_thread_fn(void *arg) {
    struct aws_thread_scheduler *scheduler = arg;

    while (!aws_atomic_load_int(&scheduler->should_exit)) {

        /* move tasks from the mutex protected list to the scheduler. This is because we don't want to hold the lock
         * for the scheduler during run_all and then try and acquire the lock from another thread to schedule something
         * because that potentially would block the calling thread. */
        struct aws_linked_list list_cpy;
        aws_linked_list_init(&list_cpy);
        struct aws_linked_list cancel_list_cpy;
        aws_linked_list_init(&cancel_list_cpy);

        AWS_FATAL_ASSERT(!aws_mutex_lock(&scheduler->thread_data.mutex) && "mutex lock failed!");
        aws_linked_list_swap_contents(&scheduler->thread_data.scheduling_queue, &list_cpy);
        aws_linked_list_swap_contents(&scheduler->thread_data.cancel_queue, &cancel_list_cpy);
        AWS_FATAL_ASSERT(!aws_mutex_unlock(&scheduler->thread_data.mutex) && "mutex unlock failed!");

        while (!aws_linked_list_empty(&list_cpy)) {
            struct aws_linked_list_node *node = aws_linked_list_pop_front(&list_cpy);
            struct aws_task *task = AWS_CONTAINER_OF(node, struct aws_task, node);
            if (task->timestamp) {
                aws_task_scheduler_schedule_future(&scheduler->scheduler, task, task->timestamp);
            } else {
                aws_task_scheduler_schedule_now(&scheduler->scheduler, task);
            }
        }

        /* now cancel the tasks. */
        while (!aws_linked_list_empty(&cancel_list_cpy)) {
            struct aws_linked_list_node *node = aws_linked_list_pop_front(&cancel_list_cpy);
            struct cancellation_node *cancellation_node = AWS_CONTAINER_OF(node, struct cancellation_node, node);
            aws_task_scheduler_cancel_task(&scheduler->scheduler, cancellation_node->task_to_cancel);
            aws_mem_release(scheduler->allocator, cancellation_node);
        }

        /* now run everything */
        uint64_t current_time = 0;
        aws_high_res_clock_get_ticks(&current_time);
        aws_task_scheduler_run_all(&scheduler->scheduler, current_time);

        uint64_t next_scheduled_task = 0;
        aws_task_scheduler_has_tasks(&scheduler->scheduler, &next_scheduled_task);

        int64_t timeout = 0;
        if (next_scheduled_task == UINT64_MAX) {
            /* at least wake up once per 30 seconds. */
            timeout = (int64_t)30 * (int64_t)AWS_TIMESTAMP_NANOS;
        } else {
            timeout = (int64_t)(next_scheduled_task - current_time);
        }

        if (timeout > 0) {
            AWS_FATAL_ASSERT(!aws_mutex_lock(&scheduler->thread_data.mutex) && "mutex lock failed!");

            aws_condition_variable_wait_for_pred(
                &scheduler->thread_data.c_var, &scheduler->thread_data.mutex, timeout, s_thread_should_wake, scheduler);
            AWS_FATAL_ASSERT(!aws_mutex_unlock(&scheduler->thread_data.mutex) && "mutex unlock failed!");
        }
    }
}

struct aws_thread_scheduler *aws_thread_scheduler_new(
    struct aws_allocator *allocator,
    const struct aws_thread_options *thread_options) {
    struct aws_thread_scheduler *scheduler = aws_mem_calloc(allocator, 1, sizeof(struct aws_thread_scheduler));

    if (!scheduler) {
        return NULL;
    }

    if (aws_thread_init(&scheduler->thread, allocator)) {
        goto clean_up;
    }

    AWS_FATAL_ASSERT(!aws_mutex_init(&scheduler->thread_data.mutex) && "mutex init failed!");
    AWS_FATAL_ASSERT(!aws_condition_variable_init(&scheduler->thread_data.c_var) && "condition variable init failed!");

    if (aws_task_scheduler_init(&scheduler->scheduler, allocator)) {
        goto thread_init;
    }

    scheduler->allocator = allocator;
    aws_atomic_init_int(&scheduler->should_exit, 0U);
    aws_ref_count_init(&scheduler->ref_count, scheduler, s_destroy_callback);
    aws_linked_list_init(&scheduler->thread_data.scheduling_queue);
    aws_linked_list_init(&scheduler->thread_data.cancel_queue);

    if (aws_thread_launch(&scheduler->thread, s_thread_fn, scheduler, thread_options)) {
        goto scheduler_init;
    }

    return scheduler;

scheduler_init:
    aws_task_scheduler_clean_up(&scheduler->scheduler);

thread_init:
    aws_condition_variable_clean_up(&scheduler->thread_data.c_var);
    aws_mutex_clean_up(&scheduler->thread_data.mutex);
    aws_thread_clean_up(&scheduler->thread);

clean_up:
    aws_mem_release(allocator, scheduler);

    return NULL;
}

void aws_thread_scheduler_acquire(struct aws_thread_scheduler *scheduler) {
    aws_ref_count_acquire(&scheduler->ref_count);
}

void aws_thread_scheduler_release(const struct aws_thread_scheduler *scheduler) {
    aws_ref_count_release((struct aws_ref_count *)&scheduler->ref_count);
}

void aws_thread_scheduler_schedule_future(
    struct aws_thread_scheduler *scheduler,
    struct aws_task *task,
    uint64_t time_to_run) {
    task->timestamp = time_to_run;
    AWS_FATAL_ASSERT(!aws_mutex_lock(&scheduler->thread_data.mutex) && "mutex lock failed!");
    aws_linked_list_push_back(&scheduler->thread_data.scheduling_queue, &task->node);
    AWS_FATAL_ASSERT(!aws_mutex_unlock(&scheduler->thread_data.mutex) && "mutex unlock failed!");
    aws_condition_variable_notify_one(&scheduler->thread_data.c_var);
}
void aws_thread_scheduler_schedule_now(struct aws_thread_scheduler *scheduler, struct aws_task *task) {
    aws_thread_scheduler_schedule_future(scheduler, task, 0U);
}

void aws_thread_scheduler_cancel_task(struct aws_thread_scheduler *scheduler, struct aws_task *task) {
    struct cancellation_node *cancellation_node =
        aws_mem_calloc(scheduler->allocator, 1, sizeof(struct cancellation_node));
    AWS_FATAL_ASSERT(cancellation_node && "allocation failed for cancellation node!");
    AWS_FATAL_ASSERT(!aws_mutex_lock(&scheduler->thread_data.mutex) && "mutex lock failed!");
    struct aws_task *found_task = NULL;

    /* remove tasks that are still in the scheduling queue, but haven't made it to the scheduler yet. */
    struct aws_linked_list_node *node = aws_linked_list_empty(&scheduler->thread_data.scheduling_queue)
                                            ? NULL
                                            : aws_linked_list_front(&scheduler->thread_data.scheduling_queue);
    while (node != NULL) {
        struct aws_task *potential_task = AWS_CONTAINER_OF(node, struct aws_task, node);

        if (potential_task == task) {
            found_task = potential_task;
            break;
        }

        if (aws_linked_list_node_next_is_valid(node)) {
            node = aws_linked_list_next(node);
        } else {
            node = NULL;
        }
    }

    if (found_task) {
        aws_linked_list_remove(&found_task->node);
    }

    cancellation_node->task_to_cancel = task;

    /* regardless put it in the cancel queue so the thread can call the task with canceled status. */
    aws_linked_list_push_back(&scheduler->thread_data.cancel_queue, &cancellation_node->node);
    AWS_FATAL_ASSERT(!aws_mutex_unlock(&scheduler->thread_data.mutex) && "mutex unlock failed!");
    /* notify so the loop knows to wakeup and process the cancellations. */
    aws_condition_variable_notify_one(&scheduler->thread_data.c_var);
}

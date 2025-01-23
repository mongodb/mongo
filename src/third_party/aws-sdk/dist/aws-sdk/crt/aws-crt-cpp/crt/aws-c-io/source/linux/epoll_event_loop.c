/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/cal/cal.h>
#include <aws/common/atomics.h>
#include <aws/common/clock.h>
#include <aws/common/mutex.h>
#include <aws/common/task_scheduler.h>
#include <aws/common/thread.h>
#include <aws/io/event_loop.h>
#include <aws/io/logging.h>
#include <aws/io/private/event_loop_impl.h>
#include <aws/io/private/tracing.h>

#include <sys/epoll.h>

#include <errno.h>
#include <limits.h>
#include <unistd.h>

#if !defined(COMPAT_MODE) && defined(__GLIBC__) && ((__GLIBC__ == 2 && __GLIBC_MINOR__ >= 8) || __GLIBC__ > 2)
#    define USE_EFD 1
#else
#    define USE_EFD 0
#endif

#if USE_EFD
#    include <aws/io/io.h>
#    include <sys/eventfd.h>

#else
#    include <aws/io/pipe.h>
#endif

/* This isn't defined on ancient linux distros (breaking the builds).
 * However, if this is a prebuild, we purposely build on an ancient system, but
 * we want the kernel calls to still be the same as a modern build since that's likely the target of the application
 * calling this code. Just define this if it isn't there already. GlibC and the kernel don't really care how the flag
 * gets passed as long as it does.
 */
#ifndef EPOLLRDHUP
#    define EPOLLRDHUP 0x2000
#endif

static void s_destroy(struct aws_event_loop *event_loop);
static int s_run(struct aws_event_loop *event_loop);
static int s_stop(struct aws_event_loop *event_loop);
static int s_wait_for_stop_completion(struct aws_event_loop *event_loop);
static void s_schedule_task_now(struct aws_event_loop *event_loop, struct aws_task *task);
static void s_schedule_task_future(struct aws_event_loop *event_loop, struct aws_task *task, uint64_t run_at_nanos);
static void s_cancel_task(struct aws_event_loop *event_loop, struct aws_task *task);
static int s_subscribe_to_io_events(
    struct aws_event_loop *event_loop,
    struct aws_io_handle *handle,
    int events,
    aws_event_loop_on_event_fn *on_event,
    void *user_data);
static int s_unsubscribe_from_io_events(struct aws_event_loop *event_loop, struct aws_io_handle *handle);
static void s_free_io_event_resources(void *user_data);
static bool s_is_on_callers_thread(struct aws_event_loop *event_loop);

static void aws_event_loop_thread(void *args);

static struct aws_event_loop_vtable s_vtable = {
    .destroy = s_destroy,
    .run = s_run,
    .stop = s_stop,
    .wait_for_stop_completion = s_wait_for_stop_completion,
    .schedule_task_now = s_schedule_task_now,
    .schedule_task_future = s_schedule_task_future,
    .cancel_task = s_cancel_task,
    .subscribe_to_io_events = s_subscribe_to_io_events,
    .unsubscribe_from_io_events = s_unsubscribe_from_io_events,
    .free_io_event_resources = s_free_io_event_resources,
    .is_on_callers_thread = s_is_on_callers_thread,
};

struct epoll_loop {
    struct aws_task_scheduler scheduler;
    struct aws_thread thread_created_on;
    struct aws_thread_options thread_options;
    aws_thread_id_t thread_joined_to;
    struct aws_atomic_var running_thread_id;
    struct aws_io_handle read_task_handle;
    struct aws_io_handle write_task_handle;
    struct aws_mutex task_pre_queue_mutex;
    struct aws_linked_list task_pre_queue;
    struct aws_task stop_task;
    struct aws_atomic_var stop_task_ptr;
    int epoll_fd;
    bool should_process_task_pre_queue;
    bool should_continue;
};

struct epoll_event_data {
    struct aws_allocator *alloc;
    struct aws_io_handle *handle;
    aws_event_loop_on_event_fn *on_event;
    void *user_data;
    struct aws_task cleanup_task;
    bool is_subscribed; /* false when handle is unsubscribed, but this struct hasn't been cleaned up yet */
};

/* default timeout is 100 seconds */
enum {
    DEFAULT_TIMEOUT = 100 * 1000,
    MAX_EVENTS = 100,
};

int aws_open_nonblocking_posix_pipe(int pipe_fds[2]);

/* Setup edge triggered epoll with a scheduler. */
struct aws_event_loop *aws_event_loop_new_default_with_options(
    struct aws_allocator *alloc,
    const struct aws_event_loop_options *options) {
    AWS_PRECONDITION(options);
    AWS_PRECONDITION(options->clock);

    struct aws_event_loop *loop = aws_mem_calloc(alloc, 1, sizeof(struct aws_event_loop));
    if (!loop) {
        return NULL;
    }

    AWS_LOGF_INFO(AWS_LS_IO_EVENT_LOOP, "id=%p: Initializing edge-triggered epoll", (void *)loop);
    if (aws_event_loop_init_base(loop, alloc, options->clock)) {
        goto clean_up_loop;
    }

    struct epoll_loop *epoll_loop = aws_mem_calloc(alloc, 1, sizeof(struct epoll_loop));
    if (!epoll_loop) {
        goto cleanup_base_loop;
    }

    if (options->thread_options) {
        epoll_loop->thread_options = *options->thread_options;
    } else {
        epoll_loop->thread_options = *aws_default_thread_options();
    }

    /* initialize thread id to NULL, it should be updated when the event loop thread starts. */
    aws_atomic_init_ptr(&epoll_loop->running_thread_id, NULL);

    aws_linked_list_init(&epoll_loop->task_pre_queue);
    epoll_loop->task_pre_queue_mutex = (struct aws_mutex)AWS_MUTEX_INIT;
    aws_atomic_init_ptr(&epoll_loop->stop_task_ptr, NULL);

    epoll_loop->epoll_fd = epoll_create(100);
    if (epoll_loop->epoll_fd < 0) {
        AWS_LOGF_FATAL(AWS_LS_IO_EVENT_LOOP, "id=%p: Failed to open epoll handle.", (void *)loop);
        aws_raise_error(AWS_ERROR_SYS_CALL_FAILURE);
        goto clean_up_epoll;
    }

    if (aws_thread_init(&epoll_loop->thread_created_on, alloc)) {
        goto clean_up_epoll;
    }

#if USE_EFD
    AWS_LOGF_INFO(AWS_LS_IO_EVENT_LOOP, "id=%p: Using eventfd for cross-thread notifications.", (void *)loop);
    int fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);

    if (fd < 0) {
        AWS_LOGF_FATAL(AWS_LS_IO_EVENT_LOOP, "id=%p: Failed to open eventfd handle.", (void *)loop);
        aws_raise_error(AWS_ERROR_SYS_CALL_FAILURE);
        goto clean_up_thread;
    }

    AWS_LOGF_TRACE(AWS_LS_IO_EVENT_LOOP, "id=%p: eventfd descriptor %d.", (void *)loop, fd);
    epoll_loop->write_task_handle = (struct aws_io_handle){.data.fd = fd, .additional_data = NULL};
    epoll_loop->read_task_handle = (struct aws_io_handle){.data.fd = fd, .additional_data = NULL};
#else
    AWS_LOGF_DEBUG(
        AWS_LS_IO_EVENT_LOOP,
        "id=%p: Eventfd not available, falling back to pipe for cross-thread notification.",
        (void *)loop);

    int pipe_fds[2] = {0};
    /* this pipe is for task scheduling. */
    if (aws_open_nonblocking_posix_pipe(pipe_fds)) {
        AWS_LOGF_FATAL(AWS_LS_IO_EVENT_LOOP, "id=%p: failed to open pipe handle.", (void *)loop);
        goto clean_up_thread;
    }

    AWS_LOGF_TRACE(
        AWS_LS_IO_EVENT_LOOP, "id=%p: pipe descriptors read %d, write %d.", (void *)loop, pipe_fds[0], pipe_fds[1]);
    epoll_loop->write_task_handle.data.fd = pipe_fds[1];
    epoll_loop->read_task_handle.data.fd = pipe_fds[0];
#endif

    if (aws_task_scheduler_init(&epoll_loop->scheduler, alloc)) {
        goto clean_up_pipe;
    }

    epoll_loop->should_continue = false;

    loop->impl_data = epoll_loop;
    loop->vtable = &s_vtable;

    return loop;

clean_up_pipe:
#if USE_EFD
    close(epoll_loop->write_task_handle.data.fd);
    epoll_loop->write_task_handle.data.fd = -1;
    epoll_loop->read_task_handle.data.fd = -1;
#else
    close(epoll_loop->read_task_handle.data.fd);
    close(epoll_loop->write_task_handle.data.fd);
#endif

clean_up_thread:
    aws_thread_clean_up(&epoll_loop->thread_created_on);

clean_up_epoll:
    if (epoll_loop->epoll_fd >= 0) {
        close(epoll_loop->epoll_fd);
    }

    aws_mem_release(alloc, epoll_loop);

cleanup_base_loop:
    aws_event_loop_clean_up_base(loop);

clean_up_loop:
    aws_mem_release(alloc, loop);

    return NULL;
}

static void s_destroy(struct aws_event_loop *event_loop) {
    AWS_LOGF_INFO(AWS_LS_IO_EVENT_LOOP, "id=%p: Destroying event_loop", (void *)event_loop);

    struct epoll_loop *epoll_loop = event_loop->impl_data;

    /* we don't know if stop() has been called by someone else,
     * just call stop() again and wait for event-loop to finish. */
    aws_event_loop_stop(event_loop);
    s_wait_for_stop_completion(event_loop);

    /* setting this so that canceled tasks don't blow up when asking if they're on the event-loop thread. */
    epoll_loop->thread_joined_to = aws_thread_current_thread_id();
    aws_atomic_store_ptr(&epoll_loop->running_thread_id, &epoll_loop->thread_joined_to);
    aws_task_scheduler_clean_up(&epoll_loop->scheduler);

    while (!aws_linked_list_empty(&epoll_loop->task_pre_queue)) {
        struct aws_linked_list_node *node = aws_linked_list_pop_front(&epoll_loop->task_pre_queue);
        struct aws_task *task = AWS_CONTAINER_OF(node, struct aws_task, node);
        task->fn(task, task->arg, AWS_TASK_STATUS_CANCELED);
    }

    aws_thread_clean_up(&epoll_loop->thread_created_on);
#if USE_EFD
    close(epoll_loop->write_task_handle.data.fd);
    epoll_loop->write_task_handle.data.fd = -1;
    epoll_loop->read_task_handle.data.fd = -1;
#else
    close(epoll_loop->read_task_handle.data.fd);
    close(epoll_loop->write_task_handle.data.fd);
#endif

    close(epoll_loop->epoll_fd);
    aws_mem_release(event_loop->alloc, epoll_loop);
    aws_event_loop_clean_up_base(event_loop);
    aws_mem_release(event_loop->alloc, event_loop);
}

static int s_run(struct aws_event_loop *event_loop) {
    struct epoll_loop *epoll_loop = event_loop->impl_data;

    AWS_LOGF_INFO(AWS_LS_IO_EVENT_LOOP, "id=%p: Starting event-loop thread.", (void *)event_loop);

    epoll_loop->should_continue = true;
    aws_thread_increment_unjoined_count();
    if (aws_thread_launch(
            &epoll_loop->thread_created_on, &aws_event_loop_thread, event_loop, &epoll_loop->thread_options)) {

        aws_thread_decrement_unjoined_count();
        AWS_LOGF_FATAL(AWS_LS_IO_EVENT_LOOP, "id=%p: thread creation failed.", (void *)event_loop);
        epoll_loop->should_continue = false;
        return AWS_OP_ERR;
    }

    return AWS_OP_SUCCESS;
}

static void s_stop_task(struct aws_task *task, void *args, enum aws_task_status status) {

    (void)task;
    struct aws_event_loop *event_loop = args;
    struct epoll_loop *epoll_loop = event_loop->impl_data;

    /* now okay to reschedule stop tasks. */
    aws_atomic_store_ptr(&epoll_loop->stop_task_ptr, NULL);
    if (status == AWS_TASK_STATUS_RUN_READY) {
        /*
         * this allows the event loop to invoke the callback once the event loop has completed.
         */
        epoll_loop->should_continue = false;
    }
}

static int s_stop(struct aws_event_loop *event_loop) {
    struct epoll_loop *epoll_loop = event_loop->impl_data;

    void *expected_ptr = NULL;
    bool update_succeeded =
        aws_atomic_compare_exchange_ptr(&epoll_loop->stop_task_ptr, &expected_ptr, &epoll_loop->stop_task);
    if (!update_succeeded) {
        /* the stop task is already scheduled. */
        return AWS_OP_SUCCESS;
    }
    AWS_LOGF_INFO(AWS_LS_IO_EVENT_LOOP, "id=%p: Stopping event-loop thread.", (void *)event_loop);
    aws_task_init(&epoll_loop->stop_task, s_stop_task, event_loop, "epoll_event_loop_stop");
    s_schedule_task_now(event_loop, &epoll_loop->stop_task);

    return AWS_OP_SUCCESS;
}

static int s_wait_for_stop_completion(struct aws_event_loop *event_loop) {
    struct epoll_loop *epoll_loop = event_loop->impl_data;
    int result = aws_thread_join(&epoll_loop->thread_created_on);
    aws_thread_decrement_unjoined_count();
    return result;
}

static void s_schedule_task_common(struct aws_event_loop *event_loop, struct aws_task *task, uint64_t run_at_nanos) {
    struct epoll_loop *epoll_loop = event_loop->impl_data;

    /* if event loop and the caller are the same thread, just schedule and be done with it. */
    if (s_is_on_callers_thread(event_loop)) {
        AWS_LOGF_TRACE(
            AWS_LS_IO_EVENT_LOOP,
            "id=%p: scheduling task %p in-thread for timestamp %llu",
            (void *)event_loop,
            (void *)task,
            (unsigned long long)run_at_nanos);
        if (run_at_nanos == 0) {
            /* zero denotes "now" task */
            aws_task_scheduler_schedule_now(&epoll_loop->scheduler, task);
        } else {
            aws_task_scheduler_schedule_future(&epoll_loop->scheduler, task, run_at_nanos);
        }
        return;
    }

    AWS_LOGF_TRACE(
        AWS_LS_IO_EVENT_LOOP,
        "id=%p: Scheduling task %p cross-thread for timestamp %llu",
        (void *)event_loop,
        (void *)task,
        (unsigned long long)run_at_nanos);
    task->timestamp = run_at_nanos;
    aws_mutex_lock(&epoll_loop->task_pre_queue_mutex);

    uint64_t counter = 1;

    bool is_first_task = aws_linked_list_empty(&epoll_loop->task_pre_queue);

    aws_linked_list_push_back(&epoll_loop->task_pre_queue, &task->node);

    /* if the list was not empty, we already have a pending read on the pipe/eventfd, no need to write again. */
    if (is_first_task) {
        AWS_LOGF_TRACE(AWS_LS_IO_EVENT_LOOP, "id=%p: Waking up event-loop thread", (void *)event_loop);

        /* If the write fails because the buffer is full, we don't actually care because that means there's a pending
         * read on the pipe/eventfd and thus the event loop will end up checking to see if something has been queued.*/
        ssize_t do_not_care = write(epoll_loop->write_task_handle.data.fd, (void *)&counter, sizeof(counter));
        (void)do_not_care;
    }

    aws_mutex_unlock(&epoll_loop->task_pre_queue_mutex);
}

static void s_schedule_task_now(struct aws_event_loop *event_loop, struct aws_task *task) {
    s_schedule_task_common(event_loop, task, 0 /* zero denotes "now" task */);
}

static void s_schedule_task_future(struct aws_event_loop *event_loop, struct aws_task *task, uint64_t run_at_nanos) {
    s_schedule_task_common(event_loop, task, run_at_nanos);
}

static void s_cancel_task(struct aws_event_loop *event_loop, struct aws_task *task) {
    AWS_LOGF_TRACE(AWS_LS_IO_EVENT_LOOP, "id=%p: cancelling task %p", (void *)event_loop, (void *)task);
    struct epoll_loop *epoll_loop = event_loop->impl_data;
    aws_task_scheduler_cancel_task(&epoll_loop->scheduler, task);
}

static int s_subscribe_to_io_events(
    struct aws_event_loop *event_loop,
    struct aws_io_handle *handle,
    int events,
    aws_event_loop_on_event_fn *on_event,
    void *user_data) {

    AWS_LOGF_TRACE(AWS_LS_IO_EVENT_LOOP, "id=%p: subscribing to events on fd %d", (void *)event_loop, handle->data.fd);
    struct epoll_event_data *epoll_event_data = aws_mem_calloc(event_loop->alloc, 1, sizeof(struct epoll_event_data));
    handle->additional_data = epoll_event_data;
    if (!epoll_event_data) {
        return AWS_OP_ERR;
    }

    struct epoll_loop *epoll_loop = event_loop->impl_data;
    epoll_event_data->alloc = event_loop->alloc;
    epoll_event_data->user_data = user_data;
    epoll_event_data->handle = handle;
    epoll_event_data->on_event = on_event;
    epoll_event_data->is_subscribed = true;

    /*everyone is always registered for edge-triggered, hang up, remote hang up, errors. */
    uint32_t event_mask = EPOLLET | EPOLLHUP | EPOLLRDHUP | EPOLLERR;

    if (events & AWS_IO_EVENT_TYPE_READABLE) {
        event_mask |= EPOLLIN;
    }

    if (events & AWS_IO_EVENT_TYPE_WRITABLE) {
        event_mask |= EPOLLOUT;
    }

    /* this guy is copied by epoll_ctl */
    struct epoll_event epoll_event = {
        .data = {.ptr = epoll_event_data},
        .events = event_mask,
    };

    if (epoll_ctl(epoll_loop->epoll_fd, EPOLL_CTL_ADD, handle->data.fd, &epoll_event)) {
        AWS_LOGF_ERROR(
            AWS_LS_IO_EVENT_LOOP, "id=%p: failed to subscribe to events on fd %d", (void *)event_loop, handle->data.fd);
        handle->additional_data = NULL;
        aws_mem_release(event_loop->alloc, epoll_event_data);
        return aws_raise_error(AWS_ERROR_SYS_CALL_FAILURE);
    }

    return AWS_OP_SUCCESS;
}

static void s_free_io_event_resources(void *user_data) {
    struct epoll_event_data *event_data = user_data;
    aws_mem_release(event_data->alloc, (void *)event_data);
}

static void s_unsubscribe_cleanup_task(struct aws_task *task, void *arg, enum aws_task_status status) {
    (void)task;
    (void)status;
    struct epoll_event_data *event_data = (struct epoll_event_data *)arg;
    s_free_io_event_resources(event_data);
}

static int s_unsubscribe_from_io_events(struct aws_event_loop *event_loop, struct aws_io_handle *handle) {
    AWS_LOGF_TRACE(
        AWS_LS_IO_EVENT_LOOP, "id=%p: un-subscribing from events on fd %d", (void *)event_loop, handle->data.fd);
    struct epoll_loop *epoll_loop = event_loop->impl_data;

    AWS_ASSERT(handle->additional_data);
    struct epoll_event_data *additional_handle_data = handle->additional_data;

    struct epoll_event dummy_event;

    if (AWS_UNLIKELY(epoll_ctl(epoll_loop->epoll_fd, EPOLL_CTL_DEL, handle->data.fd, &dummy_event /*ignored*/))) {
        AWS_LOGF_ERROR(
            AWS_LS_IO_EVENT_LOOP,
            "id=%p: failed to un-subscribe from events on fd %d",
            (void *)event_loop,
            handle->data.fd);
        return aws_raise_error(AWS_ERROR_SYS_CALL_FAILURE);
    }

    /* We can't clean up yet, because we have schedule tasks and more events to process,
     * mark it as unsubscribed and schedule a cleanup task. */
    additional_handle_data->is_subscribed = false;

    aws_task_init(
        &additional_handle_data->cleanup_task,
        s_unsubscribe_cleanup_task,
        additional_handle_data,
        "epoll_event_loop_unsubscribe_cleanup");
    s_schedule_task_now(event_loop, &additional_handle_data->cleanup_task);

    handle->additional_data = NULL;
    return AWS_OP_SUCCESS;
}

static bool s_is_on_callers_thread(struct aws_event_loop *event_loop) {
    struct epoll_loop *epoll_loop = event_loop->impl_data;

    aws_thread_id_t *thread_id = aws_atomic_load_ptr(&epoll_loop->running_thread_id);
    return thread_id && aws_thread_thread_id_equal(*thread_id, aws_thread_current_thread_id());
}

/* We treat the pipe fd with a subscription to io events just like any other managed file descriptor.
 * This is the event handler for events on that pipe.*/
static void s_on_tasks_to_schedule(
    struct aws_event_loop *event_loop,
    struct aws_io_handle *handle,
    int events,
    void *user_data) {

    (void)handle;
    (void)user_data;

    AWS_LOGF_TRACE(AWS_LS_IO_EVENT_LOOP, "id=%p: notified of cross-thread tasks to schedule", (void *)event_loop);
    struct epoll_loop *epoll_loop = event_loop->impl_data;
    if (events & AWS_IO_EVENT_TYPE_READABLE) {
        epoll_loop->should_process_task_pre_queue = true;
    }
}

static void s_process_task_pre_queue(struct aws_event_loop *event_loop) {
    struct epoll_loop *epoll_loop = event_loop->impl_data;

    if (!epoll_loop->should_process_task_pre_queue) {
        return;
    }

    AWS_LOGF_TRACE(AWS_LS_IO_EVENT_LOOP, "id=%p: processing cross-thread tasks", (void *)event_loop);
    epoll_loop->should_process_task_pre_queue = false;

    struct aws_linked_list task_pre_queue;
    aws_linked_list_init(&task_pre_queue);

    uint64_t count_ignore = 0;

    aws_mutex_lock(&epoll_loop->task_pre_queue_mutex);

    /* several tasks could theoretically have been written (though this should never happen), make sure we drain the
     * eventfd/pipe. */
    while (read(epoll_loop->read_task_handle.data.fd, &count_ignore, sizeof(count_ignore)) > -1) {
    }

    aws_linked_list_swap_contents(&epoll_loop->task_pre_queue, &task_pre_queue);

    aws_mutex_unlock(&epoll_loop->task_pre_queue_mutex);

    while (!aws_linked_list_empty(&task_pre_queue)) {
        struct aws_linked_list_node *node = aws_linked_list_pop_front(&task_pre_queue);
        struct aws_task *task = AWS_CONTAINER_OF(node, struct aws_task, node);
        AWS_LOGF_TRACE(
            AWS_LS_IO_EVENT_LOOP,
            "id=%p: task %p pulled to event-loop, scheduling now.",
            (void *)event_loop,
            (void *)task);
        /* Timestamp 0 is used to denote "now" tasks */
        if (task->timestamp == 0) {
            aws_task_scheduler_schedule_now(&epoll_loop->scheduler, task);
        } else {
            aws_task_scheduler_schedule_future(&epoll_loop->scheduler, task, task->timestamp);
        }
    }
}

/**
 * This just calls epoll_wait()
 *
 * We broke this out into its own function so that the stacktrace clearly shows
 * what this thread is doing. We've had a lot of cases where users think this
 * thread is deadlocked because it's stuck here. We want it to be clear
 * that it's doing nothing on purpose. It's waiting for events to happen...
 */
AWS_NO_INLINE
static int aws_event_loop_listen_for_io_events(int epoll_fd, struct epoll_event events[MAX_EVENTS], int timeout) {
    return epoll_wait(epoll_fd, events, MAX_EVENTS, timeout);
}

static void s_aws_epoll_cleanup_aws_lc_thread_local_state(void *user_data) {
    (void)user_data;

    aws_cal_thread_clean_up();
}

static void aws_event_loop_thread(void *args) {
    struct aws_event_loop *event_loop = args;
    AWS_LOGF_INFO(AWS_LS_IO_EVENT_LOOP, "id=%p: main loop started", (void *)event_loop);
    struct epoll_loop *epoll_loop = event_loop->impl_data;

    /* set thread id to the thread of the event loop */
    aws_atomic_store_ptr(&epoll_loop->running_thread_id, &epoll_loop->thread_created_on.thread_id);

    int err = s_subscribe_to_io_events(
        event_loop, &epoll_loop->read_task_handle, AWS_IO_EVENT_TYPE_READABLE, s_on_tasks_to_schedule, NULL);
    if (err) {
        return;
    }

    aws_thread_current_at_exit(s_aws_epoll_cleanup_aws_lc_thread_local_state, NULL);

    int timeout = DEFAULT_TIMEOUT;

    struct epoll_event events[MAX_EVENTS];

    AWS_LOGF_INFO(
        AWS_LS_IO_EVENT_LOOP,
        "id=%p: default timeout %d, and max events to process per tick %d",
        (void *)event_loop,
        timeout,
        MAX_EVENTS);

    /*
     * until stop is called,
     * call epoll_wait, if a task is scheduled, or a file descriptor has activity, it will
     * return.
     *
     * process all events,
     *
     * run all scheduled tasks.
     *
     * process queued subscription cleanups.
     */
    while (epoll_loop->should_continue) {

        AWS_LOGF_TRACE(AWS_LS_IO_EVENT_LOOP, "id=%p: waiting for a maximum of %d ms", (void *)event_loop, timeout);
        int event_count = aws_event_loop_listen_for_io_events(epoll_loop->epoll_fd, events, timeout);
        aws_event_loop_register_tick_start(event_loop);

        AWS_LOGF_TRACE(
            AWS_LS_IO_EVENT_LOOP, "id=%p: wake up with %d events to process.", (void *)event_loop, event_count);

        __itt_task_begin(io_tracing_domain, __itt_null, __itt_null, tracing_event_loop_events);
        for (int i = 0; i < event_count; ++i) {
            struct epoll_event_data *event_data = (struct epoll_event_data *)events[i].data.ptr;

            int event_mask = 0;
            if (events[i].events & EPOLLIN) {
                event_mask |= AWS_IO_EVENT_TYPE_READABLE;
            }

            if (events[i].events & EPOLLOUT) {
                event_mask |= AWS_IO_EVENT_TYPE_WRITABLE;
            }

            if (events[i].events & EPOLLRDHUP) {
                event_mask |= AWS_IO_EVENT_TYPE_REMOTE_HANG_UP;
            }

            if (events[i].events & EPOLLHUP) {
                event_mask |= AWS_IO_EVENT_TYPE_CLOSED;
            }

            if (events[i].events & EPOLLERR) {
                event_mask |= AWS_IO_EVENT_TYPE_ERROR;
            }

            if (event_data->is_subscribed) {
                AWS_LOGF_TRACE(
                    AWS_LS_IO_EVENT_LOOP,
                    "id=%p: activity on fd %d, invoking handler.",
                    (void *)event_loop,
                    event_data->handle->data.fd);
                __itt_task_begin(io_tracing_domain, __itt_null, __itt_null, tracing_event_loop_event);
                event_data->on_event(event_loop, event_data->handle, event_mask, event_data->user_data);
                __itt_task_end(io_tracing_domain);
            }
        }
        __itt_task_end(io_tracing_domain);

        /* run scheduled tasks */
        s_process_task_pre_queue(event_loop);

        uint64_t now_ns = 0;
        event_loop->clock(&now_ns); /* if clock fails, now_ns will be 0 and tasks scheduled for a specific time
                                       will not be run. That's ok, we'll handle them next time around. */
        AWS_LOGF_TRACE(AWS_LS_IO_EVENT_LOOP, "id=%p: running scheduled tasks.", (void *)event_loop);
        __itt_task_begin(io_tracing_domain, __itt_null, __itt_null, tracing_event_loop_run_tasks);
        aws_task_scheduler_run_all(&epoll_loop->scheduler, now_ns);
        __itt_task_end(io_tracing_domain);

        /* set timeout for next epoll_wait() call.
         * if clock fails, or scheduler has no tasks, use default timeout */
        bool use_default_timeout = false;

        if (event_loop->clock(&now_ns)) {
            use_default_timeout = true;
        }

        uint64_t next_run_time_ns;
        if (!aws_task_scheduler_has_tasks(&epoll_loop->scheduler, &next_run_time_ns)) {
            use_default_timeout = true;
        }

        if (use_default_timeout) {
            AWS_LOGF_TRACE(
                AWS_LS_IO_EVENT_LOOP, "id=%p: no more scheduled tasks using default timeout.", (void *)event_loop);
            timeout = DEFAULT_TIMEOUT;
        } else {
            /* Translate timestamp (in nanoseconds) to timeout (in milliseconds) */
            uint64_t timeout_ns = (next_run_time_ns > now_ns) ? (next_run_time_ns - now_ns) : 0;
            uint64_t timeout_ms64 = aws_timestamp_convert(timeout_ns, AWS_TIMESTAMP_NANOS, AWS_TIMESTAMP_MILLIS, NULL);
            timeout = timeout_ms64 > INT_MAX ? INT_MAX : (int)timeout_ms64;
            AWS_LOGF_TRACE(
                AWS_LS_IO_EVENT_LOOP,
                "id=%p: detected more scheduled tasks with the next occurring at "
                "%llu, using timeout of %d.",
                (void *)event_loop,
                (unsigned long long)timeout_ns,
                timeout);
        }

        aws_event_loop_register_tick_end(event_loop);
    }

    AWS_LOGF_DEBUG(AWS_LS_IO_EVENT_LOOP, "id=%p: exiting main loop", (void *)event_loop);
    s_unsubscribe_from_io_events(event_loop, &epoll_loop->read_task_handle);
    /* set thread id back to NULL. This should be updated again in destroy, before tasks are canceled. */
    aws_atomic_store_ptr(&epoll_loop->running_thread_id, NULL);
}

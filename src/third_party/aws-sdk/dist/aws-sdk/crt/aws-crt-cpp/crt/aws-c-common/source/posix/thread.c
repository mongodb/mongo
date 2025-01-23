/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#if !defined(__MACH__)
#    define _GNU_SOURCE /* NOLINT(bugprone-reserved-identifier) */
#endif

#include <aws/common/clock.h>
#include <aws/common/linked_list.h>
#include <aws/common/logging.h>
#include <aws/common/private/dlloads.h>
#include <aws/common/private/thread_shared.h>
#include <aws/common/string.h>
#include <aws/common/thread.h>

#include <dlfcn.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>

#if defined(__FreeBSD__) || defined(__NetBSD__)
#    include <pthread_np.h>
typedef cpuset_t cpu_set_t;
#elif defined(__OpenBSD__)
#    include <pthread_np.h>
#endif

#if !defined(AWS_AFFINITY_METHOD)
#    error "Must provide a method for setting thread affinity"
#endif

// Possible methods for setting thread affinity
#define AWS_AFFINITY_METHOD_NONE 0
#define AWS_AFFINITY_METHOD_PTHREAD_ATTR 1
#define AWS_AFFINITY_METHOD_PTHREAD 2

// Ensure provided affinity method matches one of the supported values
// clang-format off
#if AWS_AFFINITY_METHOD != AWS_AFFINITY_METHOD_NONE \
 && AWS_AFFINITY_METHOD != AWS_AFFINITY_METHOD_PTHREAD_ATTR \
 && AWS_AFFINITY_METHOD != AWS_AFFINITY_METHOD_PTHREAD
// clang-format on
#    error "Invalid thread affinity method"
#endif

static struct aws_thread_options s_default_options = {
    /* this will make sure platform default stack size is used. */
    .stack_size = 0,
    .cpu_id = -1,
    .join_strategy = AWS_TJS_MANUAL,
};

struct thread_atexit_callback {
    aws_thread_atexit_fn *callback;
    void *user_data;
    struct thread_atexit_callback *next;
};

struct thread_wrapper {
    struct aws_allocator *allocator;
    struct aws_linked_list_node node;
    void (*func)(void *arg);
    void *arg;
    struct thread_atexit_callback *atexit;
    void (*call_once)(void *);
    void *once_arg;
    struct aws_string *name;

    /*
     * The managed thread system does lazy joins on threads once finished via their wrapper.  For that to work
     * we need something to join against, so we keep a by-value copy of the original thread here.  The tricky part
     * is how to set the threadid/handle of this copy since the copy must be injected into the thread function before
     * the threadid/handle is known.  We get around that by just querying it at the top of the wrapper thread function.
     */
    struct aws_thread thread_copy;
    bool membind;
};

static AWS_THREAD_LOCAL struct thread_wrapper *tl_wrapper = NULL;

static void s_thread_wrapper_destroy(struct thread_wrapper *wrapper) {
    if (!wrapper) {
        return;
    }

    aws_string_destroy(wrapper->name);
    aws_mem_release(wrapper->allocator, wrapper);
}

/*
 * thread_wrapper is platform-dependent so this function ends up being duplicated in each thread implementation
 */
void aws_thread_join_and_free_wrapper_list(struct aws_linked_list *wrapper_list) {
    struct aws_linked_list_node *iter = aws_linked_list_begin(wrapper_list);
    while (iter != aws_linked_list_end(wrapper_list)) {

        struct thread_wrapper *join_thread_wrapper = AWS_CONTAINER_OF(iter, struct thread_wrapper, node);

        /*
         * Can't do a for-loop since we need to advance to the next wrapper before we free the wrapper
         */
        iter = aws_linked_list_next(iter);

        join_thread_wrapper->thread_copy.detach_state = AWS_THREAD_JOINABLE;
        aws_thread_join(&join_thread_wrapper->thread_copy);

        /*
         * This doesn't actually do anything when using posix threads, but it keeps us
         * in sync with the Windows version as well as the lifecycle contract we're
         * presenting for threads.
         */
        aws_thread_clean_up(&join_thread_wrapper->thread_copy);

        s_thread_wrapper_destroy(join_thread_wrapper);

        aws_thread_decrement_unjoined_count();
    }
}

/* This must be called from the thread itself.
 * (only necessary for Apple, but we'll do it that way on every platform for consistency) */
static void s_set_thread_name(pthread_t thread_id, const char *name) {
#if defined(__APPLE__)
    (void)thread_id;
    pthread_setname_np(name);
#elif defined(AWS_PTHREAD_SETNAME_TAKES_2ARGS)
    pthread_setname_np(thread_id, name);
#elif defined(AWS_PTHREAD_SET_NAME_TAKES_2ARGS)
    pthread_set_name_np(thread_id, name);
#elif defined(AWS_PTHREAD_SETNAME_TAKES_3ARGS)
    pthread_setname_np(thread_id, name, NULL);
#else
    (void)thread_id;
    (void)name;
#endif
}

static void *thread_fn(void *arg) {
    struct thread_wrapper *wrapper_ptr = arg;

    /*
     * Make sure the aws_thread copy has the right thread id stored in it.
     */
    wrapper_ptr->thread_copy.thread_id = aws_thread_current_thread_id();

    /* If there's a name, set it.
     * Then free the aws_string before we make copies of the wrapper struct */
    if (wrapper_ptr->name) {
        s_set_thread_name(wrapper_ptr->thread_copy.thread_id, aws_string_c_str(wrapper_ptr->name));
        aws_string_destroy(wrapper_ptr->name);
        wrapper_ptr->name = NULL;
    }

    struct thread_wrapper wrapper = *wrapper_ptr;
    struct aws_allocator *allocator = wrapper.allocator;
    tl_wrapper = &wrapper;

    if (wrapper.membind && g_set_mempolicy_ptr) {
        AWS_LOGF_INFO(
            AWS_LS_COMMON_THREAD,
            "a cpu affinity was specified when launching this thread and set_mempolicy() is available on this "
            "system. Setting the memory policy to MPOL_PREFERRED");
        /* if a user set a cpu id in their thread options, we're going to make sure the numa policy honors that
         * and makes sure the numa node of the cpu we launched this thread on is where memory gets allocated. However,
         * we don't want to fail the application if this fails, so make the call, and ignore the result. */
        long resp = g_set_mempolicy_ptr(AWS_MPOL_PREFERRED_ALIAS, NULL, 0);
        int errno_value = errno; /* Always cache errno before potential side-effect */
        if (resp) {
            AWS_LOGF_WARN(AWS_LS_COMMON_THREAD, "call to set_mempolicy() failed with errno %d", errno_value);
        }
    }
    wrapper.func(wrapper.arg);

    /*
     * Managed threads don't free the wrapper yet.  The thread management system does it later after the thread
     * is joined.
     */
    bool is_managed_thread = wrapper.thread_copy.detach_state == AWS_THREAD_MANAGED;
    if (!is_managed_thread) {
        s_thread_wrapper_destroy(wrapper_ptr);
        wrapper_ptr = NULL;
    }

    struct thread_atexit_callback *exit_callback_data = wrapper.atexit;
    while (exit_callback_data) {
        aws_thread_atexit_fn *exit_callback = exit_callback_data->callback;
        void *exit_callback_user_data = exit_callback_data->user_data;
        struct thread_atexit_callback *next_exit_callback_data = exit_callback_data->next;

        aws_mem_release(allocator, exit_callback_data);

        exit_callback(exit_callback_user_data);
        exit_callback_data = next_exit_callback_data;
    }
    tl_wrapper = NULL;

    /*
     * Release this thread to the managed thread system for lazy join.
     */
    if (is_managed_thread) {
        aws_thread_pending_join_add(&wrapper_ptr->node);
    }

    return NULL;
}

const struct aws_thread_options *aws_default_thread_options(void) {
    return &s_default_options;
}

void aws_thread_clean_up(struct aws_thread *thread) {
    if (thread->detach_state == AWS_THREAD_JOINABLE) {
        pthread_detach(thread->thread_id);
    }
}

static void s_call_once(void) {
    tl_wrapper->call_once(tl_wrapper->once_arg);
}

void aws_thread_call_once(aws_thread_once *flag, void (*call_once)(void *), void *user_data) {
    // If this is a non-aws_thread, then gin up a temp thread wrapper
    struct thread_wrapper temp_wrapper;
    if (!tl_wrapper) {
        tl_wrapper = &temp_wrapper;
    }

    tl_wrapper->call_once = call_once;
    tl_wrapper->once_arg = user_data;
    pthread_once(flag, s_call_once);

    if (tl_wrapper == &temp_wrapper) {
        tl_wrapper = NULL;
    }
}

int aws_thread_init(struct aws_thread *thread, struct aws_allocator *allocator) {
    *thread = (struct aws_thread){.allocator = allocator, .detach_state = AWS_THREAD_NOT_CREATED};

    return AWS_OP_SUCCESS;
}

int aws_thread_launch(
    struct aws_thread *thread,
    void (*func)(void *arg),
    void *arg,
    const struct aws_thread_options *options) {

    pthread_attr_t attributes;
    pthread_attr_t *attributes_ptr = NULL;
    int attr_return = 0;
    struct thread_wrapper *wrapper = NULL;
    bool is_managed_thread = options != NULL && options->join_strategy == AWS_TJS_MANAGED;
    if (is_managed_thread) {
        thread->detach_state = AWS_THREAD_MANAGED;
    }

    if (options) {
        attr_return = pthread_attr_init(&attributes);

        if (attr_return) {
            goto cleanup;
        }

        attributes_ptr = &attributes;

        if (options->stack_size > PTHREAD_STACK_MIN) {
            attr_return = pthread_attr_setstacksize(attributes_ptr, options->stack_size);

            if (attr_return) {
                goto cleanup;
            }
        } else if (!options->stack_size) {
            /**
             * On some systems, the default stack size is too low (128KB on musl at the time of writing this), which can
             * cause stack overflow when the dependency chain is long. Increase the stack size to at
             * least 1MB, which is the default on Windows.
             */
            size_t min_stack_size = (size_t)1 * 1024 * 1024;
            size_t current_stack_size;
            attr_return = pthread_attr_getstacksize(attributes_ptr, &current_stack_size);
            if (attr_return) {
                goto cleanup;
            }

            if (current_stack_size < min_stack_size) {
                attr_return = pthread_attr_setstacksize(attributes_ptr, min_stack_size);
                if (attr_return) {
                    goto cleanup;
                }
            }
        }

/* AFAIK you can't set thread affinity on apple platforms, and it doesn't really matter since all memory
 * NUMA or not is setup in interleave mode.
 * Thread affinity is also not supported on Android systems, and honestly, if you're running android on a NUMA
 * configuration, you've got bigger problems. */
#if AWS_AFFINITY_METHOD == AWS_AFFINITY_METHOD_PTHREAD_ATTR
        if (options->cpu_id >= 0) {
            AWS_LOGF_INFO(
                AWS_LS_COMMON_THREAD,
                "id=%p: cpu affinity of cpu_id %d was specified, attempting to honor the value.",
                (void *)thread,
                options->cpu_id);

            cpu_set_t cpuset;
            CPU_ZERO(&cpuset);
            CPU_SET((uint32_t)options->cpu_id, &cpuset);

            attr_return = pthread_attr_setaffinity_np(attributes_ptr, sizeof(cpuset), &cpuset);

            if (attr_return) {
                AWS_LOGF_WARN(
                    AWS_LS_COMMON_THREAD,
                    "id=%p: pthread_attr_setaffinity_np() failed with %d. Continuing without cpu affinity",
                    (void *)thread,
                    attr_return);
                goto cleanup;
            }
        }
#endif /* AWS_AFFINITY_METHOD == AWS_AFFINITY_METHOD_PTHREAD_ATTR */
    }

    wrapper = aws_mem_calloc(thread->allocator, 1, sizeof(struct thread_wrapper));

    if (options) {
        if (options->cpu_id >= 0) {
            wrapper->membind = true;
        }
        if (options->name.len > 0) {
            wrapper->name = aws_string_new_from_cursor(thread->allocator, &options->name);
        }
    }

    wrapper->thread_copy = *thread;
    wrapper->allocator = thread->allocator;
    wrapper->func = func;
    wrapper->arg = arg;

    /*
     * Increment the count prior to spawning the thread.  Decrement back if the create failed.
     */
    if (is_managed_thread) {
        aws_thread_increment_unjoined_count();
    }

    attr_return = pthread_create(&thread->thread_id, attributes_ptr, thread_fn, (void *)wrapper);

    if (attr_return) {
        AWS_LOGF_ERROR(AWS_LS_COMMON_THREAD, "id=%p: pthread_create() failed with %d", (void *)thread, attr_return);
        if (is_managed_thread) {
            aws_thread_decrement_unjoined_count();
        }
        goto cleanup;
    }

#if AWS_AFFINITY_METHOD == AWS_AFFINITY_METHOD_PTHREAD
    /* If we don't have pthread_attr_setaffinity_np, we may
     * still be able to set the thread affinity after creation. */
    if (options && options->cpu_id >= 0) {
        AWS_LOGF_INFO(
            AWS_LS_COMMON_THREAD,
            "id=%p: cpu affinity of cpu_id %d was specified, attempting to honor the value.",
            (void *)thread,
            options->cpu_id);

        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET((uint32_t)options->cpu_id, &cpuset);

        /* If this fails, just warn. We can't fail anymore, the thread has already launched. */
        int setaffinity_return = pthread_setaffinity_np(thread->thread_id, sizeof(cpuset), &cpuset);
        if (setaffinity_return) {
            AWS_LOGF_WARN(
                AWS_LS_COMMON_THREAD,
                "id=%p: pthread_setaffinity_np() failed with %d. Running thread without CPU affinity.",
                (void *)thread,
                setaffinity_return);
        }
    }
#endif /* AWS_AFFINITY_METHOD == AWS_AFFINITY_METHOD_PTHREAD */
    /*
     * Managed threads need to stay unjoinable from an external perspective.  We'll handle it after thread function
     * completion.
     */
    if (is_managed_thread) {
        aws_thread_clean_up(thread);
    } else {
        thread->detach_state = AWS_THREAD_JOINABLE;
    }

cleanup:
    if (attributes_ptr) {
        pthread_attr_destroy(attributes_ptr);
    }

    if (attr_return) {
        s_thread_wrapper_destroy(wrapper);
        if (options && options->cpu_id >= 0) {
            /*
             * `pthread_create` can fail with an `EINVAL` error or `EDEADLK` on freebasd if the `cpu_id` is
             * restricted/invalid. Since the pinning to a particular `cpu_id` is supposed to be best-effort, try to
             * launch a thread again without pinning to a specific cpu_id.
             */
            AWS_LOGF_INFO(
                AWS_LS_COMMON_THREAD,
                "id=%p: Attempting to launch the thread again without pinning to a cpu_id",
                (void *)thread);
            struct aws_thread_options new_options = *options;
            new_options.cpu_id = -1;
            return aws_thread_launch(thread, func, arg, &new_options);
        }
        switch (attr_return) {
            case EINVAL:
                return aws_raise_error(AWS_ERROR_THREAD_INVALID_SETTINGS);
            case EAGAIN:
                return aws_raise_error(AWS_ERROR_THREAD_INSUFFICIENT_RESOURCE);
            case EPERM:
                return aws_raise_error(AWS_ERROR_THREAD_NO_PERMISSIONS);
            case ENOMEM:
                return aws_raise_error(AWS_ERROR_OOM);
            default:
                return aws_raise_error(AWS_ERROR_UNKNOWN);
        }
    }
    return AWS_OP_SUCCESS;
}

aws_thread_id_t aws_thread_get_id(struct aws_thread *thread) {
    return thread->thread_id;
}

enum aws_thread_detach_state aws_thread_get_detach_state(struct aws_thread *thread) {
    return thread->detach_state;
}

int aws_thread_join(struct aws_thread *thread) {
    if (thread->detach_state == AWS_THREAD_JOINABLE) {
        int err_no = pthread_join(thread->thread_id, 0);

        if (err_no) {
            if (err_no == EINVAL) {
                return aws_raise_error(AWS_ERROR_THREAD_NOT_JOINABLE);
            }
            if (err_no == ESRCH) {
                return aws_raise_error(AWS_ERROR_THREAD_NO_SUCH_THREAD_ID);
            }
            if (err_no == EDEADLK) {
                return aws_raise_error(AWS_ERROR_THREAD_DEADLOCK_DETECTED);
            }
        }

        thread->detach_state = AWS_THREAD_JOIN_COMPLETED;
    }

    return AWS_OP_SUCCESS;
}

aws_thread_id_t aws_thread_current_thread_id(void) {
    return pthread_self();
}

bool aws_thread_thread_id_equal(aws_thread_id_t t1, aws_thread_id_t t2) {
    return pthread_equal(t1, t2) != 0;
}

void aws_thread_current_sleep(uint64_t nanos) {
    uint64_t nano = 0;
    time_t seconds = (time_t)aws_timestamp_convert(nanos, AWS_TIMESTAMP_NANOS, AWS_TIMESTAMP_SECS, &nano);

    struct timespec tm = {
        .tv_sec = seconds,
        .tv_nsec = (long)nano,
    };
    struct timespec output;

    nanosleep(&tm, &output);
}

int aws_thread_current_at_exit(aws_thread_atexit_fn *callback, void *user_data) {
    if (!tl_wrapper) {
        return aws_raise_error(AWS_ERROR_THREAD_NOT_JOINABLE);
    }

    struct thread_atexit_callback *cb = aws_mem_calloc(tl_wrapper->allocator, 1, sizeof(struct thread_atexit_callback));
    if (!cb) {
        return AWS_OP_ERR;
    }
    cb->callback = callback;
    cb->user_data = user_data;
    cb->next = tl_wrapper->atexit;
    tl_wrapper->atexit = cb;
    return AWS_OP_SUCCESS;
}

int aws_thread_current_name(struct aws_allocator *allocator, struct aws_string **out_name) {
    return aws_thread_name(allocator, aws_thread_current_thread_id(), out_name);
}

#define THREAD_NAME_BUFFER_SIZE 256
int aws_thread_name(struct aws_allocator *allocator, aws_thread_id_t thread_id, struct aws_string **out_name) {
    *out_name = NULL;
#if defined(AWS_PTHREAD_GETNAME_TAKES_2ARGS) || defined(AWS_PTHREAD_GETNAME_TAKES_3ARGS) ||                            \
    defined(AWS_PTHREAD_GET_NAME_TAKES_2_ARGS)
    char name[THREAD_NAME_BUFFER_SIZE] = {0};
#    ifdef AWS_PTHREAD_GETNAME_TAKES_3ARGS
    if (pthread_getname_np(thread_id, name, THREAD_NAME_BUFFER_SIZE)) {
#    elif AWS_PTHREAD_GETNAME_TAKES_2ARGS
    if (pthread_getname_np(thread_id, name)) {
#    elif AWS_PTHREAD_GET_NAME_TAKES_2ARGS
    if (pthread_get_name_np(thread_id, name)) {
#    endif

        return aws_raise_error(AWS_ERROR_SYS_CALL_FAILURE);
    }

    *out_name = aws_string_new_from_c_str(allocator, name);
    return AWS_OP_SUCCESS;
#else

    return aws_raise_error(AWS_ERROR_PLATFORM_NOT_SUPPORTED);
#endif
}

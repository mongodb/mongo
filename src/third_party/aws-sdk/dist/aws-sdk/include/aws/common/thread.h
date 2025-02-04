#ifndef AWS_COMMON_THREAD_H
#define AWS_COMMON_THREAD_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/common/byte_buf.h>
#include <aws/common/string.h>

#ifndef _WIN32
#    include <pthread.h>
#endif

AWS_PUSH_SANE_WARNING_LEVEL

enum aws_thread_detach_state {
    AWS_THREAD_NOT_CREATED = 1,
    AWS_THREAD_JOINABLE,
    AWS_THREAD_JOIN_COMPLETED,
    AWS_THREAD_MANAGED,
};

/**
 * Specifies the join strategy used on an aws_thread, which in turn controls whether or not a thread participates
 * in the managed thread system.  The managed thread system provides logic to guarantee a join on all participating
 * threads at the cost of laziness (the user cannot control when joins happen).
 *
 * Manual - thread does not participate in the managed thread system; any joins must be done by the user.  This
 * is the default.  The user must call aws_thread_clean_up(), but only after any desired join operation has completed.
 * Not doing so will cause the windows handle to leak.
 *
 * Managed - the managed thread system will automatically perform a join some time after the thread's run function
 * has completed.  It is an error to call aws_thread_join on a thread configured with the managed join strategy.  The
 * managed thread system will call aws_thread_clean_up() on the thread after the background join has completed.
 *
 * Additionally, an API exists, aws_thread_join_all_managed(), which blocks and returns when all outstanding threads
 * with the managed strategy have fully joined.  This API is useful for tests (rather than waiting for many individual
 * signals) and program shutdown or DLL unload.  This API is automatically invoked by the common library clean up
 * function.  If the common library clean up is called from a managed thread, this will cause deadlock.
 *
 * Lazy thread joining is done only when threads finish their run function or when the user calls
 * aws_thread_join_all_managed().  This means it may be a long time between thread function completion and the join
 * being applied, but the queue of unjoined threads is always one or fewer so there is no critical resource
 * backlog.
 *
 * Currently, only event loop group async cleanup and host resolver threads participate in the managed thread system.
 * Additionally, event loop threads will increment and decrement the pending join count (they are manually joined
 * internally) in order to have an accurate view of internal thread usage and also to prevent failure to release
 * an event loop group fully from allowing aws_thread_join_all_managed() from running to completion when its
 * intent is such that it should block instead.
 */
enum aws_thread_join_strategy {
    AWS_TJS_MANUAL = 0,
    AWS_TJS_MANAGED,
};

/**
 * Thread names should be 15 characters or less.
 * Longer names will not display on Linux.
 * This length does not include a null terminator.
 */
#define AWS_THREAD_NAME_RECOMMENDED_STRLEN 15

struct aws_thread_options {
    size_t stack_size;
    /* default is -1. If you set this to anything >= 0, and the platform supports it, the thread will be pinned to
     * that cpu. Also, we assume you're doing this for memory throughput purposes. On unix systems,
     * If libnuma.so is available, upon the thread launching, the memory policy for that thread will be set to
     * allocate on the numa node that cpu-core is on.
     *
     * On windows, this will cause the thread affinity to be set, but currently we don't do anything to tell the OS
     * how to allocate memory on a node.
     *
     * On Apple and Android platforms, this setting doesn't do anything at all.
     */
    int32_t cpu_id;

    enum aws_thread_join_strategy join_strategy;

    /**
     * Thread name, for debugging purpose.
     * The length should not exceed AWS_THREAD_NAME_RECOMMENDED_STRLEN(15)
     * if you want it to display properly on all platforms.
     */
    struct aws_byte_cursor name;
};

#ifdef _WIN32
typedef union {
    void *ptr;
} aws_thread_once;
#    define AWS_THREAD_ONCE_STATIC_INIT {NULL}
typedef unsigned long aws_thread_id_t;
#else
typedef pthread_once_t aws_thread_once;
#    define AWS_THREAD_ONCE_STATIC_INIT PTHREAD_ONCE_INIT
typedef pthread_t aws_thread_id_t;
#endif

/*
 * Buffer size needed to represent aws_thread_id_t as a string (2 hex chars per byte
 * plus '\0' terminator). Needed for portable printing because pthread_t is
 * opaque.
 */
#define AWS_THREAD_ID_T_REPR_BUFSZ (sizeof(aws_thread_id_t) * 2 + 1)

struct aws_thread {
    struct aws_allocator *allocator;
    enum aws_thread_detach_state detach_state;
#ifdef _WIN32
    void *thread_handle;
#endif
    aws_thread_id_t thread_id;
};

AWS_EXTERN_C_BEGIN

/**
 * Returns an instance of system default thread options.
 */
AWS_COMMON_API
const struct aws_thread_options *aws_default_thread_options(void);

AWS_COMMON_API void aws_thread_call_once(aws_thread_once *flag, void (*call_once)(void *), void *user_data);

/**
 * Initializes a new platform specific thread object struct (not the os-level
 * thread itself).
 */
AWS_COMMON_API
int aws_thread_init(struct aws_thread *thread, struct aws_allocator *allocator);

/**
 * Creates an OS level thread and associates it with func. context will be passed to func when it is executed.
 * options will be applied to the thread if they are applicable for the platform.
 *
 * After launch, you may join on the thread.  A successfully launched thread must have clean_up called on it in order
 * to avoid a handle leak.  If you do not join before calling clean_up, the thread will become detached.
 *
 * Managed threads must not have join or clean_up called on them by external code.
 */
AWS_COMMON_API
int aws_thread_launch(
    struct aws_thread *thread,
    void (*func)(void *arg),
    void *arg,
    const struct aws_thread_options *options);

/**
 * Gets the id of thread
 */
AWS_COMMON_API
aws_thread_id_t aws_thread_get_id(struct aws_thread *thread);

/**
 * Gets the detach state of the thread. For example, is it safe to call join on
 * this thread? Has it been detached()?
 */
AWS_COMMON_API
enum aws_thread_detach_state aws_thread_get_detach_state(struct aws_thread *thread);

/**
 * Joins the calling thread to a thread instance. Returns when thread is
 * finished.  Calling this from the associated OS thread will cause a deadlock.
 */
AWS_COMMON_API
int aws_thread_join(struct aws_thread *thread);

/**
 * Blocking call that waits for all managed threads to complete their join call.  This can only be called
 * from the main thread or a non-managed thread.
 *
 * This gets called automatically from library cleanup.
 *
 * By default the wait is unbounded, but that default can be overridden via aws_thread_set_managed_join_timeout_ns()
 */
AWS_COMMON_API
int aws_thread_join_all_managed(void);

/**
 * Overrides how long, in nanoseconds, that aws_thread_join_all_managed will wait for threads to complete.
 * A value of zero will result in an unbounded wait.
 */
AWS_COMMON_API
void aws_thread_set_managed_join_timeout_ns(uint64_t timeout_in_ns);

/**
 * Cleans up the thread handle. Don't call this on a managed thread.  If you wish to join the thread, you must join
 * before calling this function.
 */
AWS_COMMON_API
void aws_thread_clean_up(struct aws_thread *thread);

/**
 * Returns the thread id of the calling thread.
 */
AWS_COMMON_API
aws_thread_id_t aws_thread_current_thread_id(void);

/**
 * Compare thread ids.
 */
AWS_COMMON_API
bool aws_thread_thread_id_equal(aws_thread_id_t t1, aws_thread_id_t t2);

/**
 * Sleeps the current thread by nanos.
 */
AWS_COMMON_API
void aws_thread_current_sleep(uint64_t nanos);

typedef void(aws_thread_atexit_fn)(void *user_data);

/**
 * Adds a callback to the chain to be called when the current thread joins.
 * Callbacks are called from the current thread, in the reverse order they
 * were added, after the thread function returns.
 * If not called from within an aws_thread, has no effect.
 */
AWS_COMMON_API
int aws_thread_current_at_exit(aws_thread_atexit_fn *callback, void *user_data);

/**
 * Increments the count of unjoined threads in the managed thread system.  Used by managed threads and
 * event loop threads.  Additional usage requires the user to join corresponding threads themselves and
 * correctly increment/decrement even in the face of launch/join errors.
 *
 * aws_thread_join_all_managed() will not return until this count has gone to zero.
 */
AWS_COMMON_API void aws_thread_increment_unjoined_count(void);

/**
 * Decrements the count of unjoined threads in the managed thread system.  Used by managed threads and
 * event loop threads.  Additional usage requires the user to join corresponding threads themselves and
 * correctly increment/decrement even in the face of launch/join errors.
 *
 * aws_thread_join_all_managed() will not return until this count has gone to zero.
 */
AWS_COMMON_API void aws_thread_decrement_unjoined_count(void);

/**
 * Gets name of the current thread.
 * Caller is responsible for destroying returned string.
 * If thread does not have a name, AWS_OP_SUCCESS is returned and out_name is
 * set to NULL.
 * If underlying OS call fails,  AWS_ERROR_SYS_CALL_FAILURE will be raised
 * If OS does not support getting thread name, AWS_ERROR_PLATFORM_NOT_SUPPORTED
 * will be raised
 */
AWS_COMMON_API int aws_thread_current_name(struct aws_allocator *allocator, struct aws_string **out_name);

/**
 * Gets name of the thread.
 * Caller is responsible for destroying returned string.
 * If thread does not have a name, AWS_OP_SUCCESS is returned and out_name is
 * set to NULL.
 * If underlying OS call fails,  AWS_ERROR_SYS_CALL_FAILURE will be raised
 * If OS does not support getting thread name, AWS_ERROR_PLATFORM_NOT_SUPPORTED
 * will be raised
 */
AWS_COMMON_API int aws_thread_name(
    struct aws_allocator *allocator,
    aws_thread_id_t thread_id,
    struct aws_string **out_name);

AWS_EXTERN_C_END
AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_COMMON_THREAD_H */

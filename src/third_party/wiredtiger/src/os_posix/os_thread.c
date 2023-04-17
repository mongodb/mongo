/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

#ifdef __linux__

/*
 * __thread_set_name --
 *     Set the pthread-level thread name. If the session name is set, use that, truncated to fit. If
 *     the caller provides a non-zero thread number, append that to the session name to distinguish
 *     between multiple threads of the same type/name.
 */
static int
__thread_set_name(WT_SESSION_IMPL *session, uint32_t thread_num, pthread_t thread_id)
{
    char short_name[WT_THREAD_NAME_MAX_LEN] = {0}, thread_name[WT_THREAD_NAME_MAX_LEN] = {0};

    if (session != NULL && session->name != NULL) {
        if (thread_num == 0)
            strncpy(thread_name, session->name, WT_THREAD_NAME_MAX_LEN);
        else {
            strncpy(short_name, session->name, WT_THREAD_NAME_MAX_LEN - 4);

            if (thread_num < 100)
                WT_RET(__wt_snprintf(
                  thread_name, WT_THREAD_NAME_MAX_LEN, "%s %" PRIu32, short_name, thread_num));
            else
                WT_RET(__wt_snprintf(thread_name, WT_THREAD_NAME_MAX_LEN, "%s ++", short_name));
        }
        thread_name[WT_THREAD_NAME_MAX_LEN - 1] = '\0';
        WT_RET(pthread_setname_np(thread_id, thread_name));
    }
    return (0);
}
#endif

/*
 * __wt_thread_create --
 *     Create a new thread of control.
 */
int
__wt_thread_create(WT_SESSION_IMPL *session, wt_thread_t *tidret,
  WT_THREAD_CALLBACK (*func)(void *), void *arg) WT_GCC_FUNC_ATTRIBUTE((visibility("default")))
{
    WT_DECL_RET;

    /*
     * Creating a thread isn't a memory barrier, but WiredTiger commonly sets flags and or state and
     * then expects worker threads to start. Include a barrier to ensure safety in those cases.
     */
    WT_FULL_BARRIER();

    /* Spawn a new thread of control. */
    WT_SYSCALL_RETRY(pthread_create(&tidret->id, NULL, func, arg), ret);
    if (ret == 0) {
        tidret->created = true;
#ifdef __linux__
        WT_IGNORE_RET(__thread_set_name(session, tidret->name_index, tidret->id));
#endif
        return (0);
    }
    WT_RET_MSG(session, ret, "pthread_create");
}

/*
 * __wt_thread_join --
 *     Wait for a thread of control to exit.
 */
int
__wt_thread_join(WT_SESSION_IMPL *session, wt_thread_t *tid)
  WT_GCC_FUNC_ATTRIBUTE((visibility("default")))
{
    WT_DECL_RET;

    /* Only attempt to join if thread was created successfully */
    if (!tid->created)
        return (0);
    tid->created = false;

    /*
     * Joining a thread isn't a memory barrier, but WiredTiger commonly sets flags and or state and
     * then expects worker threads to halt. Include a barrier to ensure safety in those cases.
     */
    WT_FULL_BARRIER();

    WT_SYSCALL(pthread_join(tid->id, NULL), ret);
    if (ret == 0)
        return (0);

    WT_RET_MSG(session, ret, "pthread_join");
}

/*
 * __wt_thread_id --
 *     Return an arithmetic representation of a thread ID on POSIX.
 */
void
__wt_thread_id(uintmax_t *id) WT_GCC_FUNC_ATTRIBUTE((visibility("default")))
{
    pthread_t self;

    /*
     * POSIX 1003.1 allows pthread_t to be an opaque type; on systems where it's a pointer, print
     * the pointer to match gdb output.
     */
    self = pthread_self();
#ifdef __sun
    *id = (uintmax_t)self;
#else
    *id = (uintmax_t)(void *)self;
#endif
}

/*
 * __wt_thread_str --
 *     Fill in a printable version of the process and thread IDs.
 */
int
__wt_thread_str(char *buf, size_t buflen) WT_GCC_FUNC_ATTRIBUTE((visibility("default")))
{
    pthread_t self;

    /*
     * POSIX 1003.1 allows pthread_t to be an opaque type; on systems where it's a pointer, print
     * the pointer to match gdb output.
     */
    self = pthread_self();
#ifdef __sun
    return (__wt_snprintf(buf, buflen, "%" PRIuMAX ":%u", (uintmax_t)getpid(), self));
#else
    return (__wt_snprintf(buf, buflen, "%" PRIuMAX ":%p", (uintmax_t)getpid(), (void *)self));
#endif
}

/*
 * __wt_process_id --
 *     Return the process ID assigned by the operating system.
 */
uintmax_t
__wt_process_id(void)
{
    return ((uintmax_t)getpid());
}

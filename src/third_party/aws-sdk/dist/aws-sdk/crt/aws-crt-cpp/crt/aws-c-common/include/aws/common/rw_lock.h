#ifndef AWS_COMMON_RW_LOCK_H
#define AWS_COMMON_RW_LOCK_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/common.h>
#ifdef _WIN32
/* NOTE: Do not use this macro before including windows.h */
#    define AWSSRW_TO_WINDOWS(pCV) (PSRWLOCK) pCV
#else
#    include <pthread.h>
#endif

AWS_PUSH_SANE_WARNING_LEVEL

struct aws_rw_lock {
#ifdef _WIN32
    void *lock_handle;
#else
    pthread_rwlock_t lock_handle;
#endif
};

#ifdef _WIN32
#    define AWS_RW_LOCK_INIT {.lock_handle = NULL}
#else
#    define AWS_RW_LOCK_INIT {.lock_handle = PTHREAD_RWLOCK_INITIALIZER}
#endif

AWS_EXTERN_C_BEGIN

/**
 * Initializes a new platform instance of mutex.
 */
AWS_COMMON_API int aws_rw_lock_init(struct aws_rw_lock *lock);

/**
 * Cleans up internal resources.
 */
AWS_COMMON_API void aws_rw_lock_clean_up(struct aws_rw_lock *lock);

/**
 * Blocks until it acquires the lock. While on some platforms such as Windows,
 * this may behave as a reentrant mutex, you should not treat it like one. On
 * platforms it is possible for it to be non-reentrant, it will be.
 */
AWS_COMMON_API int aws_rw_lock_rlock(struct aws_rw_lock *lock);
AWS_COMMON_API int aws_rw_lock_wlock(struct aws_rw_lock *lock);

/**
 * Attempts to acquire the lock but returns immediately if it can not.
 * While on some platforms such as Windows, this may behave as a reentrant mutex,
 * you should not treat it like one. On platforms it is possible for it to be non-reentrant, it will be.
 * Note: For windows, minimum support server version is Windows Server 2008 R2 [desktop apps | UWP apps]
 */
AWS_COMMON_API int aws_rw_lock_try_rlock(struct aws_rw_lock *lock);
AWS_COMMON_API int aws_rw_lock_try_wlock(struct aws_rw_lock *lock);

/**
 * Releases the lock.
 */
AWS_COMMON_API int aws_rw_lock_runlock(struct aws_rw_lock *lock);
AWS_COMMON_API int aws_rw_lock_wunlock(struct aws_rw_lock *lock);

AWS_EXTERN_C_END
AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_COMMON_RW_LOCK_H */

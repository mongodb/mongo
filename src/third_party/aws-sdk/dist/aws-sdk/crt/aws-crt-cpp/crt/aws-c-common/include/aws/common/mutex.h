#ifndef AWS_COMMON_MUTEX_H
#define AWS_COMMON_MUTEX_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/common.h>
#ifdef _WIN32
/* NOTE: Do not use this macro before including windows.h */
#    define AWSMUTEX_TO_WINDOWS(pMutex) (PSRWLOCK) & (pMutex)->mutex_handle
#else
#    include <pthread.h>
#endif

AWS_PUSH_SANE_WARNING_LEVEL

struct aws_mutex {
#ifdef _WIN32
    void *mutex_handle;
#else
    pthread_mutex_t mutex_handle;
#endif
    bool initialized;
};

#ifdef _WIN32
#    define AWS_MUTEX_INIT {.mutex_handle = NULL, .initialized = true}
#else
#    define AWS_MUTEX_INIT {.mutex_handle = PTHREAD_MUTEX_INITIALIZER, .initialized = true}
#endif

AWS_EXTERN_C_BEGIN

/**
 * Initializes a new platform instance of mutex.
 */
AWS_COMMON_API
int aws_mutex_init(struct aws_mutex *mutex);

/**
 * Cleans up internal resources.
 */
AWS_COMMON_API
void aws_mutex_clean_up(struct aws_mutex *mutex);

/**
 * Blocks until it acquires the lock. While on some platforms such as Windows,
 * this may behave as a reentrant mutex, you should not treat it like one. On
 * platforms it is possible for it to be non-reentrant, it will be.
 */
AWS_COMMON_API
int aws_mutex_lock(struct aws_mutex *mutex);

/**
 * Attempts to acquire the lock but returns immediately if it can not.
 * While on some platforms such as Windows, this may behave as a reentrant mutex,
 * you should not treat it like one. On platforms it is possible for it to be non-reentrant, it will be.
 * Note: For windows, minimum support server version is Windows Server 2008 R2 [desktop apps | UWP apps]
 */
AWS_COMMON_API
int aws_mutex_try_lock(struct aws_mutex *mutex);

/**
 * Releases the lock.
 */
AWS_COMMON_API
int aws_mutex_unlock(struct aws_mutex *mutex);

AWS_EXTERN_C_END
AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_COMMON_MUTEX_H */

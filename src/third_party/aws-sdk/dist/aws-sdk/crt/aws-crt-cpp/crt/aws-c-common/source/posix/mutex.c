/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/mutex.h>
#include <aws/common/posix/common.inl>

#include <errno.h>

void aws_mutex_clean_up(struct aws_mutex *mutex) {
    AWS_PRECONDITION(mutex);
    if (mutex->initialized) {
        pthread_mutex_destroy(&mutex->mutex_handle);
    }
    AWS_ZERO_STRUCT(*mutex);
}

int aws_mutex_init(struct aws_mutex *mutex) {
    AWS_PRECONDITION(mutex);
    pthread_mutexattr_t attr;
    int err_code = pthread_mutexattr_init(&attr);
    int return_code = AWS_OP_SUCCESS;

    if (!err_code) {
        err_code = pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_NORMAL);
        if (!err_code) {
            err_code = pthread_mutex_init(&mutex->mutex_handle, &attr);
        }
        if (err_code) {
            return_code = aws_private_convert_and_raise_error_code(err_code);
        }
        pthread_mutexattr_destroy(&attr);
    } else {
        return_code = aws_private_convert_and_raise_error_code(err_code);
    }

    mutex->initialized = (return_code == AWS_OP_SUCCESS);
    return return_code;
}

int aws_mutex_lock(struct aws_mutex *mutex) {
    AWS_PRECONDITION(mutex && mutex->initialized);
    return aws_private_convert_and_raise_error_code(pthread_mutex_lock(&mutex->mutex_handle));
}

int aws_mutex_try_lock(struct aws_mutex *mutex) {
    AWS_PRECONDITION(mutex && mutex->initialized);
    return aws_private_convert_and_raise_error_code(pthread_mutex_trylock(&mutex->mutex_handle));
}

int aws_mutex_unlock(struct aws_mutex *mutex) {
    AWS_PRECONDITION(mutex && mutex->initialized);
    return aws_private_convert_and_raise_error_code(pthread_mutex_unlock(&mutex->mutex_handle));
}

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/atomics.h>
#include <aws/common/rw_lock.h>

#include <aws/common/posix/common.inl>

int aws_rw_lock_init(struct aws_rw_lock *lock) {

    return aws_private_convert_and_raise_error_code(pthread_rwlock_init(&lock->lock_handle, NULL));
}

void aws_rw_lock_clean_up(struct aws_rw_lock *lock) {

    pthread_rwlock_destroy(&lock->lock_handle);
}

int aws_rw_lock_rlock(struct aws_rw_lock *lock) {

    return aws_private_convert_and_raise_error_code(pthread_rwlock_rdlock(&lock->lock_handle));
}

int aws_rw_lock_wlock(struct aws_rw_lock *lock) {

    return aws_private_convert_and_raise_error_code(pthread_rwlock_wrlock(&lock->lock_handle));
}

int aws_rw_lock_try_rlock(struct aws_rw_lock *lock) {

    return aws_private_convert_and_raise_error_code(pthread_rwlock_tryrdlock(&lock->lock_handle));
}

int aws_rw_lock_try_wlock(struct aws_rw_lock *lock) {

    return aws_private_convert_and_raise_error_code(pthread_rwlock_trywrlock(&lock->lock_handle));
}

int aws_rw_lock_runlock(struct aws_rw_lock *lock) {

    return aws_private_convert_and_raise_error_code(pthread_rwlock_unlock(&lock->lock_handle));
}

int aws_rw_lock_wunlock(struct aws_rw_lock *lock) {

    return aws_private_convert_and_raise_error_code(pthread_rwlock_unlock(&lock->lock_handle));
}

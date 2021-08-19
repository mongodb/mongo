/* 
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>

int
main()
{
    int ret;
    pthread_condattr_t condattr;
    pthread_cond_t cond;
    pthread_mutex_t mtx;
    struct timespec ts;

    if ((ret = pthread_condattr_init(&condattr)) != 0)
        exit(1);
    if ((ret = pthread_condattr_setclock(&condattr, CLOCK_MONOTONIC)) != 0)
        exit(1);
    if ((ret = pthread_cond_init(&cond, &condattr)) != 0)
        exit(1);
    if ((ret = pthread_mutex_init(&mtx, NULL)) != 0)
        exit(1);
    if ((ret = clock_gettime(CLOCK_MONOTONIC, &ts)) != 0)
        exit(1);
    ts.tv_sec += 1;
    if ((ret = pthread_mutex_lock(&mtx)) != 0)
        exit(1);
    if ((ret = pthread_cond_timedwait(&cond, &mtx, &ts)) != 0 && ret != EINTR && ret != ETIMEDOUT)
        exit(1);

    exit(0);
}

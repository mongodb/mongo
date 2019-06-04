/*
 *             Copyright Andrey Semashev 2015.
 * Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file LICENSE_1_0.txt or copy at
 *          http://www.boost.org/LICENSE_1_0.txt)
 */

#include <errno.h>
#include <pthread.h>

int main(int, char*[])
{
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_NORMAL);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);

    pthread_mutex_t m;
    pthread_mutex_init(&m, &attr);
    pthread_mutexattr_destroy(&attr);

    int err = pthread_mutex_lock(&m);
    if (err == EOWNERDEAD)
    {
        err = pthread_mutex_consistent(&m);
    }

    if (err != ENOTRECOVERABLE)
    {
        pthread_mutex_unlock(&m);
    }

    pthread_mutex_destroy(&m);

    return 0;
}

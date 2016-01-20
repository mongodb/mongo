/*-
 * Public Domain 2014-2016 MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "windows_shim.h"

int
sleep(int seconds)
{
	Sleep(seconds * 1000);
	return (0);
}

int
usleep(useconds_t useconds)
{
	uint32_t milli;
	milli = useconds / 1000;

	if (milli == 0)
		milli++;

	Sleep(milli);

	return (0);
}

int
gettimeofday(struct timeval* tp, void* tzp)
{
	uint64_t ns100;
	FILETIME time;

	tzp = tzp;

	GetSystemTimeAsFileTime(&time);

	ns100 = (((int64_t)time.dwHighDateTime << 32) + time.dwLowDateTime)
	    - 116444736000000000LL;
	tp->tv_sec = ns100 / 10000000;
	tp->tv_usec = (long)((ns100 % 10000000) / 10);

	return (0);
}

int
pthread_rwlock_destroy(pthread_rwlock_t *lock)
{
	lock = lock;
	return (0);
}

int
pthread_rwlock_init(pthread_rwlock_t *rwlock,
    const pthread_rwlockattr_t *ignored)
{
	ignored = ignored;
	InitializeSRWLock(&rwlock->rwlock);
	rwlock->exclusive_locked = 0;

	return (0);
}

int
pthread_rwlock_rdlock(pthread_rwlock_t *rwlock)
{
	AcquireSRWLockShared(&rwlock->rwlock);
	return (0);
}

int
pthread_rwlock_unlock(pthread_rwlock_t *rwlock)
{
	if (rwlock->exclusive_locked != 0) {
		rwlock->exclusive_locked = 0;
		ReleaseSRWLockExclusive(&rwlock->rwlock);
	} else
		ReleaseSRWLockShared(&rwlock->rwlock);

	return (0);
}

int
pthread_rwlock_wrlock(pthread_rwlock_t *rwlock)
{
	AcquireSRWLockExclusive(&rwlock->rwlock);

	rwlock->exclusive_locked = GetCurrentThreadId();

	return (0);
}

#pragma warning( once : 4024 )
#pragma warning( once : 4047 )
int
pthread_create(pthread_t *tidret, const pthread_attr_t *ignored,
    void *(*func)(void *), void * arg)
{
	ignored = ignored;
	*tidret = CreateThread(NULL, 0, func, arg, 0, NULL);

	if (*tidret != NULL)
		return (0);

	return (1);
}

int
pthread_join(pthread_t thread, void **ignored)
{
	ignored = ignored;
	WaitForSingleObject(thread, INFINITE);
	return (0);
}

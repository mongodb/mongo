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

#ifdef _WIN32

#define	WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <direct.h>
#include <io.h>
#include <process.h>

#define	inline __inline

/* Define some POSIX types */
typedef int u_int;

/* Windows does not define constants for access() */
#define	R_OK 04
#define	X_OK R_OK

/* MSVC Doesn't provide __func__, it has __FUNCTION__ */
#ifdef _MSC_VER
#define	__func__ __FUNCTION__
#endif

/* snprintf does not exist on <= VS 2013 */
#if _MSC_VER < 1900
#define	snprintf _wt_snprintf

_Check_return_opt_ int __cdecl _wt_snprintf(
    _Out_writes_(_MaxCount) char * _DstBuf,
    _In_ size_t _MaxCount,
    _In_z_ _Printf_format_string_ const char * _Format, ...);
#endif

/*
 * Emulate <sys/stat.h>
 */
#define	mkdir(path, mode) _mkdir(path)

/*
 * Emulate <sys/time.h>
 */
struct timeval {
	time_t tv_sec;
	int64_t tv_usec;
};

int gettimeofday(struct timeval* tp, void* tzp);

/*
 * Emulate <unistd.h>
 */
typedef uint32_t useconds_t;

int
sleep(int seconds);

int
usleep(useconds_t useconds);

/*
 * Emulate the <pthread.h> support we need for the tests
 */
typedef CRITICAL_SECTION  pthread_mutex_t;
typedef CONDITION_VARIABLE pthread_cond_t;

struct rwlock_wrapper {
	SRWLOCK rwlock;
	int exclusive_locked;
};

struct rwlock_wrapper;
typedef struct rwlock_wrapper pthread_rwlock_t;

typedef HANDLE pthread_t;

typedef int pthread_rwlockattr_t;
typedef int pthread_attr_t;

int   pthread_rwlock_destroy(pthread_rwlock_t *);
int   pthread_rwlock_init(pthread_rwlock_t *,
    const pthread_rwlockattr_t *);
int   pthread_rwlock_rdlock(pthread_rwlock_t *);
int   pthread_rwlock_unlock(pthread_rwlock_t *);
int   pthread_rwlock_wrlock(pthread_rwlock_t *);

int   pthread_create(pthread_t *, const pthread_attr_t *,
    void *(*)(void *), void *);
int   pthread_join(pthread_t, void **);

#endif

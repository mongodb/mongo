/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/clock.h>

#include <time.h>

static const uint64_t NS_PER_SEC = 1000000000;

#if defined(CLOCK_MONOTONIC_RAW)
#    define HIGH_RES_CLOCK CLOCK_MONOTONIC_RAW
#else
#    define HIGH_RES_CLOCK CLOCK_MONOTONIC
#endif

/* This entire compilation branch has two goals. First, prior to OSX Sierra, clock_gettime does not exist on OSX, so we
 * already need to branch on that. Second, even if we compile on a newer OSX, which we will always do for bindings (e.g.
 * python, dotnet, java etc...), we have to worry about the same lib being loaded on an older version, and thus, we'd
 * get linker errors at runtime. To avoid this, we do a dynamic load
 * to keep the function out of linker tables and only use the symbol if the current running process has access to the
 * function. */
#if defined(__MACH__)
#    include <AvailabilityMacros.h>
#    include <aws/common/thread.h>
#    include <dlfcn.h>
#    include <sys/time.h>

static int s_legacy_get_time(uint64_t *timestamp) {
    struct timeval tv;
    int ret_val = gettimeofday(&tv, NULL);

    if (ret_val) {
        return aws_raise_error(AWS_ERROR_CLOCK_FAILURE);
    }

    uint64_t secs = (uint64_t)tv.tv_sec;
    uint64_t u_secs = (uint64_t)tv.tv_usec;
    *timestamp = (secs * NS_PER_SEC) + (u_secs * 1000);
    return AWS_OP_SUCCESS;
}

#    if MAC_OS_X_VERSION_MAX_ALLOWED >= 101200
static aws_thread_once s_thread_once_flag = AWS_THREAD_ONCE_STATIC_INIT;
static int (*s_gettime_fn)(clockid_t clock_id, struct timespec *tp) = NULL;

static void s_do_osx_loads(void *user_data) {
    (void)user_data;
    s_gettime_fn = (int (*)(clockid_t clock_id, struct timespec *tp))dlsym(RTLD_DEFAULT, "clock_gettime");
}

int aws_high_res_clock_get_ticks(uint64_t *timestamp) {
    aws_thread_call_once(&s_thread_once_flag, s_do_osx_loads, NULL);
    int ret_val = 0;

    if (s_gettime_fn) {
        struct timespec ts;
        ret_val = s_gettime_fn(HIGH_RES_CLOCK, &ts);

        if (ret_val) {
            return aws_raise_error(AWS_ERROR_CLOCK_FAILURE);
        }

        uint64_t secs = (uint64_t)ts.tv_sec;
        uint64_t n_secs = (uint64_t)ts.tv_nsec;
        *timestamp = (secs * NS_PER_SEC) + n_secs;
        return AWS_OP_SUCCESS;
    }

    return s_legacy_get_time(timestamp);
}

int aws_sys_clock_get_ticks(uint64_t *timestamp) {
    aws_thread_call_once(&s_thread_once_flag, s_do_osx_loads, NULL);
    int ret_val = 0;

    if (s_gettime_fn) {
        struct timespec ts;
        ret_val = s_gettime_fn(CLOCK_REALTIME, &ts);
        if (ret_val) {
            return aws_raise_error(AWS_ERROR_CLOCK_FAILURE);
        }

        uint64_t secs = (uint64_t)ts.tv_sec;
        uint64_t n_secs = (uint64_t)ts.tv_nsec;
        *timestamp = (secs * NS_PER_SEC) + n_secs;
        return AWS_OP_SUCCESS;
    }
    return s_legacy_get_time(timestamp);
}
#    else
int aws_high_res_clock_get_ticks(uint64_t *timestamp) {
    return s_legacy_get_time(timestamp);
}

int aws_sys_clock_get_ticks(uint64_t *timestamp) {
    return s_legacy_get_time(timestamp);
}

#    endif /* MAC_OS_X_VERSION_MAX_ALLOWED >= 101200 */
/* Everywhere else, just link clock_gettime in directly */
#else
int aws_high_res_clock_get_ticks(uint64_t *timestamp) {
    int ret_val = 0;

    struct timespec ts;

    ret_val = clock_gettime(HIGH_RES_CLOCK, &ts);

    if (ret_val) {
        return aws_raise_error(AWS_ERROR_CLOCK_FAILURE);
    }

    uint64_t secs = (uint64_t)ts.tv_sec;
    uint64_t n_secs = (uint64_t)ts.tv_nsec;
    *timestamp = (secs * NS_PER_SEC) + n_secs;
    return AWS_OP_SUCCESS;
}

int aws_sys_clock_get_ticks(uint64_t *timestamp) {
    int ret_val = 0;

    struct timespec ts;
    ret_val = clock_gettime(CLOCK_REALTIME, &ts);
    if (ret_val) {
        return aws_raise_error(AWS_ERROR_CLOCK_FAILURE);
    }

    uint64_t secs = (uint64_t)ts.tv_sec;
    uint64_t n_secs = (uint64_t)ts.tv_nsec;
    *timestamp = (secs * NS_PER_SEC) + n_secs;

    return AWS_OP_SUCCESS;
}
#endif /* defined(__MACH__) */

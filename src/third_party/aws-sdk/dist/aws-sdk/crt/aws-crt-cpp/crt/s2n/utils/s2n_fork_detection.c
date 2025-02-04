/*
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 *
 *  http://aws.amazon.com/apache2.0
 *
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */

/* force the internal header to be included first, since it modifies _GNU_SOURCE/_POSIX_C_SOURCE */
/* clang-format off */
#include "utils/s2n_fork_detection_features.h"
/* clang-format on */

#include "utils/s2n_fork_detection.h"

#include "error/s2n_errno.h"
#include "utils/s2n_safety.h"

#if defined(S2N_MADVISE_SUPPORTED) && defined(MADV_WIPEONFORK)
    #if (MADV_WIPEONFORK != 18)
        #error "MADV_WIPEONFORK is not 18"
    #endif
#else /* defined(S2N_MADVISE_SUPPORTED) && defined(MADV_WIPEONFORK) */
    #define MADV_WIPEONFORK 18
#endif

/* Sometimes (for example, on FreeBSD) MAP_INHERIT_ZERO is called INHERIT_ZERO */
#if !defined(MAP_INHERIT_ZERO) && defined(INHERIT_ZERO)
    #define MAP_INHERIT_ZERO INHERIT_ZERO
#endif

/* These variables are used to disable all fork detection mechanisms or at the
 * individual level during testing.
 */
static bool ignore_wipeonfork_or_inherit_zero_method_for_testing = false;
static bool ignore_pthread_atfork_method_for_testing = false;
static bool ignore_fork_detection_for_testing = false;

#define S2N_FORK_EVENT    0
#define S2N_NO_FORK_EVENT 1

struct FGN_STATE {
    /* The current cached fork generation number for this process */
    uint64_t current_fork_generation_number;

    /* Semaphore controlling access to the shared sentinel and signaling whether
     * fork detection is enabled or not. We could use zero_on_fork_addr, but
     * avoid overloading by using an explicit variable.
     */
    bool is_fork_detection_enabled;

    /* Sentinel that signals a fork event has occurred */
    volatile char *zero_on_fork_addr;

    pthread_once_t fork_detection_once;
    pthread_rwlock_t fork_detection_rw_lock;
};

/* We only need a single statically initialised state. Note, the state is
 * inherited by child processes.
 */
static struct FGN_STATE fgn_state = {
    .current_fork_generation_number = 0,
    .is_fork_detection_enabled = false,
    .zero_on_fork_addr = NULL,
    .fork_detection_once = PTHREAD_ONCE_INIT,
    .fork_detection_rw_lock = PTHREAD_RWLOCK_INITIALIZER,
};

/* Can currently never fail. See initialise_fork_detection_methods() for
 * motivation.
 */
static inline S2N_RESULT s2n_initialise_wipeonfork_best_effort(void *addr, long page_size)
{
#if defined(S2N_MADVISE_SUPPORTED)
    /* Return value ignored on purpose */
    madvise(addr, (size_t) page_size, MADV_WIPEONFORK);
#endif

    return S2N_RESULT_OK;
}

static inline S2N_RESULT s2n_initialise_inherit_zero(void *addr, long page_size)
{
#if defined(S2N_MINHERIT_SUPPORTED) && defined(MAP_INHERIT_ZERO)
    RESULT_ENSURE(minherit(addr, page_size, MAP_INHERIT_ZERO) == 0, S2N_ERR_FORK_DETECTION_INIT);
#endif

    return S2N_RESULT_OK;
}

static void s2n_pthread_atfork_on_fork(void)
{
    /* This zeroises the first byte of the memory page pointed to by
     * *zero_on_fork_addr. This is the same byte used as fork event detection
     * sentinel in s2n_get_fork_generation_number(). The same memory page, and in
     * turn, the byte, is also the memory zeroised by the MADV_WIPEONFORK fork
     * detection mechanism.
     *
     * Aquire locks to be on the safe side. We want to avoid the checks in
     * s2n_get_fork_generation_number() getting executed before setting the sentinel
     * flag. The write lock prevents any other thread from owning any other type
     * of lock.
     *
     * pthread_atfork_on_fork() cannot return errors. Hence, there is no way to
     * gracefully recover if [un]locking fails.
     */
    if (pthread_rwlock_wrlock(&fgn_state.fork_detection_rw_lock) != 0) {
        printf("pthread_rwlock_wrlock() failed. Aborting.\n");
        abort();
    }

    if (fgn_state.zero_on_fork_addr == NULL) {
        printf("fgn_state.zero_on_fork_addr is NULL. Aborting.\n");
        abort();
    }
    *fgn_state.zero_on_fork_addr = 0;

    if (pthread_rwlock_unlock(&fgn_state.fork_detection_rw_lock) != 0) {
        printf("pthread_rwlock_unlock() failed. Aborting.\n");
        abort();
    }
}

static S2N_RESULT s2n_inititalise_pthread_atfork(void)
{
    /* Register the fork handler pthread_atfork_on_fork that is executed in the
     * child process after a fork.
     */
    if (s2n_is_pthread_atfork_supported() == true) {
        RESULT_ENSURE(pthread_atfork(NULL, NULL, s2n_pthread_atfork_on_fork) == 0, S2N_ERR_FORK_DETECTION_INIT);
    }

    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_initialise_fork_detection_methods_try(void *addr, long page_size)
{
    RESULT_GUARD_PTR(addr);

    /* Some systems don't define MADV_WIPEONFORK in sys/mman.h but the kernel
     * still supports the mechanism (AL2 being a prime example). Likely because
     * glibc on the system is old. We might be able to include kernel header
     * files directly, that define MADV_WIPEONFORK, conditioning on specific
     * OS's. But it is a mess. A more reliable method is to probe the system, at
     * run-time, whether madvise supports the MADV_WIPEONFORK advice. However,
     * the method to probe for this feature is equivalent to actually attempting
     * to initialise the MADV_WIPEONFORK fork detection. Compare with
     * probe_madv_wipeonfork_support() (used for testing).
     *
     * Instead, we apply best-effort to initialise the MADV_WIPEONFORK fork
     * detection and otherwise always require pthread_atfork to be initialised.
     * We also currently always apply prediction resistance. So, this should be
     * a safe default.
     */
    if (ignore_wipeonfork_or_inherit_zero_method_for_testing == false) {
        RESULT_GUARD(s2n_initialise_wipeonfork_best_effort(addr, page_size));
    }

    if (ignore_wipeonfork_or_inherit_zero_method_for_testing == false) {
        RESULT_GUARD(s2n_initialise_inherit_zero(addr, page_size));
    }

    if (ignore_pthread_atfork_method_for_testing == false) {
        RESULT_GUARD(s2n_inititalise_pthread_atfork());
    }

    fgn_state.zero_on_fork_addr = addr;
    *fgn_state.zero_on_fork_addr = S2N_NO_FORK_EVENT;
    fgn_state.is_fork_detection_enabled = true;

    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_setup_mapping(void **addr, long *page_size)
{
    *page_size = sysconf(_SC_PAGESIZE);
    RESULT_ENSURE_GT(*page_size, 0);

    *addr = mmap(NULL, (size_t) *page_size, PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    RESULT_ENSURE_NE(*addr, MAP_FAILED);

    return S2N_RESULT_OK;
}

static void s2n_initialise_fork_detection_methods(void)
{
    void *addr = MAP_FAILED;
    long page_size = 0;

    /* Only used to disable fork detection mechanisms during testing. */
    if (ignore_wipeonfork_or_inherit_zero_method_for_testing == true && ignore_pthread_atfork_method_for_testing == true) {
        ignore_fork_detection_for_testing = true;
        return;
    }

    if (s2n_result_is_error(s2n_setup_mapping(&addr, &page_size)) == true) {
        return;
    }

    /* Now we know that we have some memory mapped. Try to initialise fork
     * detection methods. Unmap the memory if we fail for some reason.
     */
    if (s2n_result_is_error(s2n_initialise_fork_detection_methods_try(addr, page_size)) == true) {
        /* No reason to verify return value of munmap() since we can't use that
         * information for anything anyway. */
        munmap(addr, (size_t) page_size);
        addr = NULL;
        fgn_state.zero_on_fork_addr = NULL;
        fgn_state.is_fork_detection_enabled = false;
    }
}

/* s2n_get_fork_generation_number returns S2N_RESULT_OK on success and
 * S2N_RESULT_ERROR otherwise.
 *
 * On success, returns the current fork generation number in
 * return_fork_generation_number. Caller must synchronise access to
 * return_fork_generation_number.
 */
S2N_RESULT s2n_get_fork_generation_number(uint64_t *return_fork_generation_number)
{
    RESULT_ENSURE(pthread_once(&fgn_state.fork_detection_once, s2n_initialise_fork_detection_methods) == 0, S2N_ERR_FORK_DETECTION_INIT);

    if (ignore_fork_detection_for_testing == true) {
        /* Fork detection is meant to be disabled. Hence, return success.
         * This should only happen during testing.
         */
        RESULT_ENSURE(s2n_in_unit_test(), S2N_ERR_NOT_IN_UNIT_TEST);
        return S2N_RESULT_OK;
    }

    RESULT_ENSURE(fgn_state.is_fork_detection_enabled == true, S2N_ERR_FORK_DETECTION_INIT);

    /* In most cases, we would not need to increment the fork generation number.
     * So, it is cheaper, in the expected case, to take an optimistic read lock
     * and later aquire a write lock if needed.
     * Note that we set the returned fgn before checking for a fork event. We
     * need to do this because thread execution might change between releasing
     * the read lock and taking the write lock. In that time span, another
     * thread can reset the fork event detection sentinel and we return from
     * s2n_get_fork_generation_number() without setting the returned fgn
     * appropriately.
     */
    RESULT_ENSURE(pthread_rwlock_rdlock(&fgn_state.fork_detection_rw_lock) == 0, S2N_ERR_RETRIEVE_FORK_GENERATION_NUMBER);
    *return_fork_generation_number = fgn_state.current_fork_generation_number;
    if (*fgn_state.zero_on_fork_addr != S2N_FORK_EVENT) {
        /* No fork event detected. */
        RESULT_ENSURE(pthread_rwlock_unlock(&fgn_state.fork_detection_rw_lock) == 0, S2N_ERR_RETRIEVE_FORK_GENERATION_NUMBER);
        return S2N_RESULT_OK;
    }
    RESULT_ENSURE(pthread_rwlock_unlock(&fgn_state.fork_detection_rw_lock) == 0, S2N_ERR_RETRIEVE_FORK_GENERATION_NUMBER);

    /* We are mutating the process-global, cached fork generation number. Need
     * to acquire the write lock for that. Set returned fgn before checking the
     * if condition with the same reasons as above.
     */
    RESULT_ENSURE(pthread_rwlock_wrlock(&fgn_state.fork_detection_rw_lock) == 0, S2N_ERR_RETRIEVE_FORK_GENERATION_NUMBER);
    *return_fork_generation_number = fgn_state.current_fork_generation_number;
    if (*fgn_state.zero_on_fork_addr == S2N_FORK_EVENT) {
        /* Fork event has been detected; reset sentinel, increment cached fork
         * generation number (which is now "current" in this child process), and
         * write incremented fork generation number to the output parameter.
         */
        *fgn_state.zero_on_fork_addr = S2N_NO_FORK_EVENT;
        fgn_state.current_fork_generation_number = fgn_state.current_fork_generation_number + 1;
        *return_fork_generation_number = fgn_state.current_fork_generation_number;
    }
    RESULT_ENSURE(pthread_rwlock_unlock(&fgn_state.fork_detection_rw_lock) == 0, S2N_ERR_RETRIEVE_FORK_GENERATION_NUMBER);

    return S2N_RESULT_OK;
}

static void s2n_cleanup_cb_munmap(void **probe_addr)
{
    munmap(*probe_addr, (size_t) sysconf(_SC_PAGESIZE));
}

/* Run-time probe checking whether the system supports the MADV_WIPEONFORK fork
 * detection mechanism.
 */
static S2N_RESULT s2n_probe_madv_wipeonfork_support(void)
{
    bool result = false;

    /* It is not an error to call munmap on a range that does not contain any
     * mapped pages.
     */
    DEFER_CLEANUP(void *probe_addr = MAP_FAILED, s2n_cleanup_cb_munmap);
    long page_size = 0;

    RESULT_GUARD(s2n_setup_mapping(&probe_addr, &page_size));

#if defined(S2N_MADVISE_SUPPORTED)
    /* Some versions of qemu (up to at least 5.0.0-rc4, see
     * linux-user/syscall.c) ignore invalid advice arguments. Hence, we first
     * verify that madvise() rejects advice arguments it doesn't know about.
     */
    RESULT_ENSURE_NE(madvise(probe_addr, (size_t) page_size, -1), 0);
    RESULT_ENSURE_EQ(madvise(probe_addr, (size_t) page_size, MADV_WIPEONFORK), 0);

    result = true;
#endif

    RESULT_ENSURE_EQ(result, true);

    return S2N_RESULT_OK;
}

bool s2n_is_madv_wipeonfork_supported(void)
{
    return s2n_result_is_ok(s2n_probe_madv_wipeonfork_support());
}

bool s2n_is_map_inherit_zero_supported(void)
{
#if defined(S2N_MINHERIT_SUPPORTED) && defined(MAP_INHERIT_ZERO)
    return true;
#else
    return false;
#endif
}

bool s2n_is_pthread_atfork_supported(void)
{
    /*
     * There is a bug in OpenBSD's libc which is triggered by
     * multi-generational forking of multi-threaded processes which call
     * pthread_atfork(3). Under these conditions, a grandchild process will
     * deadlock when trying to fork a great-grandchild.
     * https://marc.info/?l=openbsd-tech&m=167047636422884&w=2
     */
#if defined(__OpenBSD__)
    return false;
#else
    return true;
#endif
}

/* Use for testing only */
S2N_RESULT s2n_ignore_wipeonfork_and_inherit_zero_for_testing(void)
{
    RESULT_ENSURE(s2n_in_unit_test(), S2N_ERR_NOT_IN_UNIT_TEST);

    ignore_wipeonfork_or_inherit_zero_method_for_testing = true;

    return S2N_RESULT_OK;
}

S2N_RESULT s2n_ignore_pthread_atfork_for_testing(void)
{
    RESULT_ENSURE(s2n_in_unit_test(), S2N_ERR_NOT_IN_UNIT_TEST);

    ignore_pthread_atfork_method_for_testing = true;

    return S2N_RESULT_OK;
}

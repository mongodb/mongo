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

/*
 * _XOPEN_SOURCE is needed for resolving the constant O_CLOEXEC in some
 * environments. We use _XOPEN_SOURCE instead of _GNU_SOURCE because
 * _GNU_SOURCE is not portable and breaks when attempting to build rust
 * bindings on MacOS.
 *
 * https://man7.org/linux/man-pages/man2/open.2.html
 * The O_CLOEXEC, O_DIRECTORY, and O_NOFOLLOW flags are not
 * specified in POSIX.1-2001, but are specified in POSIX.1-2008.
 * Since glibc 2.12, one can obtain their definitions by defining
 * either _POSIX_C_SOURCE with a value greater than or equal to
 * 200809L or _XOPEN_SOURCE with a value greater than or equal to
 * 700.  In glibc 2.11 and earlier, one obtains the definitions by
 * defining _GNU_SOURCE.
 *
 * We use two feature probes to detect the need to perform this workaround.
 * It is only applied if we can't get CLOEXEC without it and the build doesn't
 * fail with _XOPEN_SOURCE being defined.
 *
 * # Relevent Links
 *
 * - POSIX.1-2017: https://pubs.opengroup.org/onlinepubs/9699919799
 * - https://stackoverflow.com/a/5724485
 * - https://stackoverflow.com/a/5583764
 */
#if !defined(S2N_CLOEXEC_SUPPORTED) && defined(S2N_CLOEXEC_XOPEN_SUPPORTED) && !defined(_XOPEN_SOURCE)
    #define _XOPEN_SOURCE 700
    #include <fcntl.h>
    #undef _XOPEN_SOURCE
#else
    #include <fcntl.h>
#endif
#include <errno.h>
#include <limits.h>
#include <openssl/engine.h>
#include <openssl/rand.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#if defined(S2N_CPUID_AVAILABLE)
    #include <cpuid.h>
#endif

#include "api/s2n.h"
#include "crypto/s2n_drbg.h"
#include "crypto/s2n_fips.h"
#include "error/s2n_errno.h"
#include "s2n_io.h"
#include "stuffer/s2n_stuffer.h"
#include "utils/s2n_fork_detection.h"
#include "utils/s2n_init.h"
#include "utils/s2n_mem.h"
#include "utils/s2n_random.h"
#include "utils/s2n_result.h"
#include "utils/s2n_safety.h"

#if defined(O_CLOEXEC)
    #define ENTROPY_FLAGS O_RDONLY | O_CLOEXEC
#else
    #define ENTROPY_FLAGS O_RDONLY
#endif

/* See https://en.wikipedia.org/wiki/CPUID */
#define RDRAND_ECX_FLAG 0x40000000

/* One second in nanoseconds */
#define ONE_S INT64_C(1000000000)

/* Placeholder value for an uninitialized entropy file descriptor */
#define UNINITIALIZED_ENTROPY_FD -1

static struct s2n_rand_device s2n_dev_urandom = {
    .source = "/dev/urandom",
    .fd = UNINITIALIZED_ENTROPY_FD,
};

struct s2n_rand_state {
    uint64_t cached_fork_generation_number;
    struct s2n_drbg public_drbg;
    struct s2n_drbg private_drbg;
    bool drbgs_initialized;
};

/* Key which will control per-thread freeing of drbg memory */
static pthread_key_t s2n_per_thread_rand_state_key;
/* Needed to ensure key is initialized only once */
static pthread_once_t s2n_per_thread_rand_state_key_once = PTHREAD_ONCE_INIT;
/* Tracks if call to pthread_key_create failed */
static int pthread_key_create_result;

static __thread struct s2n_rand_state s2n_per_thread_rand_state = {
    .cached_fork_generation_number = 0,
    .public_drbg = { 0 },
    .private_drbg = { 0 },
    .drbgs_initialized = false
};

static int s2n_rand_init_cb_impl(void);
static int s2n_rand_cleanup_cb_impl(void);
static int s2n_rand_get_entropy_from_urandom(void *ptr, uint32_t size);
static int s2n_rand_get_entropy_from_rdrand(void *ptr, uint32_t size);

static s2n_rand_init_callback s2n_rand_init_cb = s2n_rand_init_cb_impl;
static s2n_rand_cleanup_callback s2n_rand_cleanup_cb = s2n_rand_cleanup_cb_impl;
static s2n_rand_seed_callback s2n_rand_seed_cb = s2n_rand_get_entropy_from_urandom;
static s2n_rand_mix_callback s2n_rand_mix_cb = s2n_rand_get_entropy_from_urandom;

static int s2n_rand_entropy_fd_close_ptr(int *fd)
{
    if (fd && *fd != UNINITIALIZED_ENTROPY_FD) {
        close(*fd);
    }
    return S2N_SUCCESS;
}

/* non-static for SAW proof */
bool s2n_cpu_supports_rdrand()
{
#if defined(S2N_CPUID_AVAILABLE)
    uint32_t eax, ebx, ecx, edx;
    if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
        return false;
    }

    if (ecx & RDRAND_ECX_FLAG) {
        return true;
    }
#endif
    return false;
}

int s2n_rand_set_callbacks(s2n_rand_init_callback rand_init_callback,
        s2n_rand_cleanup_callback rand_cleanup_callback,
        s2n_rand_seed_callback rand_seed_callback,
        s2n_rand_mix_callback rand_mix_callback)
{
    POSIX_ENSURE_REF(rand_init_callback);
    POSIX_ENSURE_REF(rand_cleanup_callback);
    POSIX_ENSURE_REF(rand_seed_callback);
    POSIX_ENSURE_REF(rand_mix_callback);
    s2n_rand_init_cb = rand_init_callback;
    s2n_rand_cleanup_cb = rand_cleanup_callback;
    s2n_rand_seed_cb = rand_seed_callback;
    s2n_rand_mix_cb = rand_mix_callback;

    return S2N_SUCCESS;
}

S2N_RESULT s2n_get_seed_entropy(struct s2n_blob *blob)
{
    RESULT_ENSURE_REF(blob);

    RESULT_ENSURE(s2n_rand_seed_cb(blob->data, blob->size) >= S2N_SUCCESS, S2N_ERR_CANCELLED);

    return S2N_RESULT_OK;
}

S2N_RESULT s2n_get_mix_entropy(struct s2n_blob *blob)
{
    RESULT_ENSURE_REF(blob);

    RESULT_GUARD_POSIX(s2n_rand_mix_cb(blob->data, blob->size));

    return S2N_RESULT_OK;
}

/* Deletes pthread key at process-exit */
static void __attribute__((destructor)) s2n_drbg_rand_state_key_cleanup(void)
{
    if (s2n_is_initialized()) {
        pthread_key_delete(s2n_per_thread_rand_state_key);
    }
}

static void s2n_drbg_destructor(void *_unused_argument)
{
    (void) _unused_argument;

    s2n_result_ignore(s2n_rand_cleanup_thread());
}

static void s2n_drbg_make_rand_state_key(void)
{
    /* We can't return the output of pthread_key_create due to the parameter constraints of pthread_once.
     * Here we store the result in a global variable that will be error checked later. */
    pthread_key_create_result = pthread_key_create(&s2n_per_thread_rand_state_key, s2n_drbg_destructor);
}

static S2N_RESULT s2n_init_drbgs(void)
{
    uint8_t s2n_public_drbg[] = "s2n public drbg";
    uint8_t s2n_private_drbg[] = "s2n private drbg";
    struct s2n_blob public = { 0 };
    RESULT_GUARD_POSIX(s2n_blob_init(&public, s2n_public_drbg, sizeof(s2n_public_drbg)));
    struct s2n_blob private = { 0 };
    RESULT_GUARD_POSIX(s2n_blob_init(&private, s2n_private_drbg, sizeof(s2n_private_drbg)));

    RESULT_ENSURE(pthread_once(&s2n_per_thread_rand_state_key_once, s2n_drbg_make_rand_state_key) == 0, S2N_ERR_DRBG);
    RESULT_ENSURE_EQ(pthread_key_create_result, 0);

    RESULT_GUARD(s2n_drbg_instantiate(&s2n_per_thread_rand_state.public_drbg, &public, S2N_AES_128_CTR_NO_DF_PR));
    RESULT_GUARD(s2n_drbg_instantiate(&s2n_per_thread_rand_state.private_drbg, &private, S2N_AES_256_CTR_NO_DF_PR));

    RESULT_ENSURE(pthread_setspecific(s2n_per_thread_rand_state_key, &s2n_per_thread_rand_state) == 0, S2N_ERR_DRBG);

    s2n_per_thread_rand_state.drbgs_initialized = true;

    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_ensure_initialized_drbgs(void)
{
    if (s2n_per_thread_rand_state.drbgs_initialized == false) {
        RESULT_GUARD(s2n_init_drbgs());

        /* Then cache the fork generation number. We just initialized the drbg
         * states with new entropy and forking is not an external event.
         */
        uint64_t returned_fork_generation_number = 0;
        RESULT_GUARD(s2n_get_fork_generation_number(&returned_fork_generation_number));
        s2n_per_thread_rand_state.cached_fork_generation_number = returned_fork_generation_number;
    }

    return S2N_RESULT_OK;
}

/* s2n_ensure_uniqueness() implements defenses against uniqueness
 * breaking events that might cause duplicated drbg states. Currently, only
 * implements fork detection.
 */
static S2N_RESULT s2n_ensure_uniqueness(void)
{
    uint64_t returned_fork_generation_number = 0;
    RESULT_GUARD(s2n_get_fork_generation_number(&returned_fork_generation_number));

    if (returned_fork_generation_number != s2n_per_thread_rand_state.cached_fork_generation_number) {
        /* This assumes that s2n_rand_cleanup_thread() doesn't mutate any other
         * state than the drbg states and it resets the drbg initialization
         * boolean to false. s2n_ensure_initialized_drbgs() will cache the new
         * fork generation number in the per thread state.
         */
        RESULT_GUARD(s2n_rand_cleanup_thread());
        RESULT_GUARD(s2n_ensure_initialized_drbgs());
    }

    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_get_libcrypto_random_data(struct s2n_blob *out_blob)
{
    RESULT_GUARD_PTR(out_blob);
    RESULT_GUARD_OSSL(RAND_bytes(out_blob->data, out_blob->size), S2N_ERR_DRBG);
    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_get_custom_random_data(struct s2n_blob *out_blob, struct s2n_drbg *drbg_state)
{
    RESULT_GUARD_PTR(out_blob);
    RESULT_GUARD_PTR(drbg_state);

    RESULT_ENSURE(!s2n_is_in_fips_mode(), S2N_ERR_DRBG);
    RESULT_GUARD(s2n_ensure_initialized_drbgs());
    RESULT_GUARD(s2n_ensure_uniqueness());

    uint32_t offset = 0;
    uint32_t remaining = out_blob->size;

    while (remaining) {
        struct s2n_blob slice = { 0 };

        RESULT_GUARD_POSIX(s2n_blob_slice(out_blob, &slice, offset, MIN(remaining, S2N_DRBG_GENERATE_LIMIT)));
        RESULT_GUARD(s2n_drbg_generate(drbg_state, &slice));

        remaining -= slice.size;
        offset += slice.size;
    }

    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_get_random_data(struct s2n_blob *blob, struct s2n_drbg *drbg_state)
{
    /* By default, s2n-tls uses a custom random implementation to generate random data for the TLS
     * handshake. When operating in FIPS mode, the FIPS-validated libcrypto implementation is used
     * instead.
     */
    if (s2n_is_in_fips_mode()) {
        RESULT_GUARD(s2n_get_libcrypto_random_data(blob));
        return S2N_RESULT_OK;
    }

    RESULT_GUARD(s2n_get_custom_random_data(blob, drbg_state));

    return S2N_RESULT_OK;
}

S2N_RESULT s2n_get_public_random_data(struct s2n_blob *blob)
{
    RESULT_GUARD(s2n_get_random_data(blob, &s2n_per_thread_rand_state.public_drbg));

    return S2N_RESULT_OK;
}

S2N_RESULT s2n_get_private_random_data(struct s2n_blob *blob)
{
    RESULT_GUARD(s2n_get_random_data(blob, &s2n_per_thread_rand_state.private_drbg));

    return S2N_RESULT_OK;
}

S2N_RESULT s2n_get_public_random_bytes_used(uint64_t *bytes_used)
{
    RESULT_GUARD(s2n_drbg_bytes_used(&s2n_per_thread_rand_state.public_drbg, bytes_used));
    return S2N_RESULT_OK;
}

S2N_RESULT s2n_get_private_random_bytes_used(uint64_t *bytes_used)
{
    RESULT_GUARD(s2n_drbg_bytes_used(&s2n_per_thread_rand_state.private_drbg, bytes_used));
    return S2N_RESULT_OK;
}

S2N_RESULT s2n_rand_get_urandom_for_test(struct s2n_rand_device **device)
{
    RESULT_ENSURE_REF(device);
    RESULT_ENSURE(s2n_in_unit_test(), S2N_ERR_NOT_IN_UNIT_TEST);
    *device = &s2n_dev_urandom;
    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_rand_device_open(struct s2n_rand_device *device)
{
    RESULT_ENSURE_REF(device);
    RESULT_ENSURE_REF(device->source);

    DEFER_CLEANUP(int fd = -1, s2n_rand_entropy_fd_close_ptr);
    S2N_IO_RETRY_EINTR(fd, open(device->source, ENTROPY_FLAGS));
    RESULT_ENSURE(fd >= 0, S2N_ERR_OPEN_RANDOM);

    struct stat st = { 0 };
    RESULT_ENSURE(fstat(fd, &st) == 0, S2N_ERR_OPEN_RANDOM);
    device->dev = st.st_dev;
    device->ino = st.st_ino;
    device->mode = st.st_mode;
    device->rdev = st.st_rdev;

    device->fd = fd;

    /* Disable closing the file descriptor with defer cleanup */
    fd = UNINITIALIZED_ENTROPY_FD;

    return S2N_RESULT_OK;
}

S2N_RESULT s2n_rand_device_validate(struct s2n_rand_device *device)
{
    RESULT_ENSURE_REF(device);
    RESULT_ENSURE_NE(device->fd, UNINITIALIZED_ENTROPY_FD);

    /* Ensure that the random device is still valid by comparing it to the current file descriptor
     * status. From:
     * https://github.com/openssl/openssl/blob/260d97229c467d17934ca3e2e0455b1b5c0994a6/providers/implementations/rands/seeding/rand_unix.c#L513
     */
    struct stat st = { 0 };
    RESULT_ENSURE(fstat(device->fd, &st) == 0, S2N_ERR_OPEN_RANDOM);
    RESULT_ENSURE_EQ(device->dev, st.st_dev);
    RESULT_ENSURE_EQ(device->ino, st.st_ino);
    RESULT_ENSURE_EQ(device->rdev, st.st_rdev);

    /* Ensure that the mode is the same (equal to 0 when xor'd), but don't check the permission bits. */
    mode_t permission_mask = ~(S_IRWXU | S_IRWXG | S_IRWXO);
    RESULT_ENSURE_EQ((device->mode ^ st.st_mode) & permission_mask, 0);

    return S2N_RESULT_OK;
}

static int s2n_rand_get_entropy_from_urandom(void *ptr, uint32_t size)
{
    POSIX_ENSURE_REF(ptr);
    POSIX_ENSURE(s2n_dev_urandom.fd != UNINITIALIZED_ENTROPY_FD, S2N_ERR_NOT_INITIALIZED);

    /* It's possible that the file descriptor pointing to /dev/urandom was closed or changed from
     * when it was last opened. Ensure that the file descriptor is still valid, and if it isn't,
     * re-open it before getting entropy.
     *
     * If the file descriptor is invalid and the process doesn't have access to /dev/urandom (as is
     * the case within a chroot tree), an error is raised here before attempting to indefinitely
     * read.
     */
    if (s2n_result_is_error(s2n_rand_device_validate(&s2n_dev_urandom))) {
        POSIX_GUARD_RESULT(s2n_rand_device_open(&s2n_dev_urandom));
    }

    uint8_t *data = ptr;
    uint32_t n = size;
    struct timespec sleep_time = { .tv_sec = 0, .tv_nsec = 0 };
    long backoff = 1;

    while (n) {
        errno = 0;
        int r = read(s2n_dev_urandom.fd, data, n);
        if (r <= 0) {
            /*
             * A non-blocking read() on /dev/urandom should "never" fail,
             * except for EINTR. If it does, briefly pause and use
             * exponential backoff to avoid creating a tight spinning loop.
             *
             * iteration          delay
             * ---------    -----------------
             *    1         10          nsec
             *    2         100         nsec
             *    3         1,000       nsec
             *    4         10,000      nsec
             *    5         100,000     nsec
             *    6         1,000,000   nsec
             *    7         10,000,000  nsec
             *    8         99,999,999  nsec
             *    9         99,999,999  nsec
             *    ...
             */
            if (errno != EINTR) {
                backoff = MIN(backoff * 10, ONE_S - 1);
                sleep_time.tv_nsec = backoff;
                do {
                    r = nanosleep(&sleep_time, &sleep_time);
                } while (r != 0);
            }

            continue;
        }

        data += r;
        n -= r;
    }

    return S2N_SUCCESS;
}

/*
 * Return a random number in the range [0, bound)
 */
S2N_RESULT s2n_public_random(int64_t bound, uint64_t *output)
{
    uint64_t r = 0;

    RESULT_ENSURE_GT(bound, 0);

    while (1) {
        struct s2n_blob blob = { 0 };
        RESULT_GUARD_POSIX(s2n_blob_init(&blob, (void *) &r, sizeof(r)));
        RESULT_GUARD(s2n_get_public_random_data(&blob));

        /* Imagine an int was one byte and UINT_MAX was 256. If the
         * caller asked for s2n_random(129, ...) we'd end up in
         * trouble. Each number in the range 0...127 would be twice
         * as likely as 128. That's because r == 0 % 129 -> 0, and
         * r == 129 % 129 -> 0, but only r == 128 returns 128,
         * r == 257 is out of range.
         *
         * To de-bias the dice, we discard values of r that are higher
         * that the highest multiple of 'bound' an int can support. If
         * bound is a uint, then in the worst case we discard 50% - 1 r's.
         * But since 'bound' is an int and INT_MAX is <= UINT_MAX / 2,
         * in the worst case we discard 25% - 1 r's.
         */
        if (r < (UINT64_MAX - (UINT64_MAX % bound))) {
            *output = r % bound;
            return S2N_RESULT_OK;
        }
    }
}

#if S2N_LIBCRYPTO_SUPPORTS_CUSTOM_RAND

    #define S2N_RAND_ENGINE_ID "s2n_rand"

int s2n_openssl_compat_rand(unsigned char *buf, int num)
{
    struct s2n_blob out = { 0 };
    POSIX_GUARD(s2n_blob_init(&out, buf, num));

    if (s2n_result_is_error(s2n_get_private_random_data(&out))) {
        return 0;
    }
    return 1;
}

int s2n_openssl_compat_status(void)
{
    return 1;
}

int s2n_openssl_compat_init(ENGINE *unused)
{
    return 1;
}

RAND_METHOD s2n_openssl_rand_method = {
    .seed = NULL,
    .bytes = s2n_openssl_compat_rand,
    .cleanup = NULL,
    .add = NULL,
    .pseudorand = s2n_openssl_compat_rand,
    .status = s2n_openssl_compat_status
};
#endif

static int s2n_rand_init_cb_impl(void)
{
    /* Currently, s2n-tls may mix in entropy from urandom into every generation of random data. The
     * file descriptor is opened on initialization for better performance reading from urandom, and
     * to ensure that urandom is accessible from within a chroot tree.
     */
    POSIX_GUARD_RESULT(s2n_rand_device_open(&s2n_dev_urandom));

    if (s2n_cpu_supports_rdrand()) {
        s2n_rand_mix_cb = s2n_rand_get_entropy_from_rdrand;
    }

    return S2N_SUCCESS;
}

S2N_RESULT s2n_rand_init(void)
{
    RESULT_ENSURE(s2n_rand_init_cb() >= S2N_SUCCESS, S2N_ERR_CANCELLED);

    RESULT_GUARD(s2n_ensure_initialized_drbgs());

#if S2N_LIBCRYPTO_SUPPORTS_CUSTOM_RAND
    if (s2n_is_in_fips_mode()) {
        return S2N_RESULT_OK;
    }

    /* Unset any existing random engine */
    RESULT_GUARD_OSSL(RAND_set_rand_engine(NULL), S2N_ERR_OPEN_RANDOM);

    /* Create an engine */
    ENGINE *e = ENGINE_new();

    RESULT_ENSURE(e != NULL, S2N_ERR_OPEN_RANDOM);
    RESULT_GUARD_OSSL(ENGINE_set_id(e, S2N_RAND_ENGINE_ID), S2N_ERR_OPEN_RANDOM);
    RESULT_GUARD_OSSL(ENGINE_set_name(e, "s2n entropy generator"), S2N_ERR_OPEN_RANDOM);
    RESULT_GUARD_OSSL(ENGINE_set_flags(e, ENGINE_FLAGS_NO_REGISTER_ALL), S2N_ERR_OPEN_RANDOM);
    RESULT_GUARD_OSSL(ENGINE_set_init_function(e, s2n_openssl_compat_init), S2N_ERR_OPEN_RANDOM);
    RESULT_GUARD_OSSL(ENGINE_set_RAND(e, &s2n_openssl_rand_method), S2N_ERR_OPEN_RANDOM);
    RESULT_GUARD_OSSL(ENGINE_add(e), S2N_ERR_OPEN_RANDOM);
    RESULT_GUARD_OSSL(ENGINE_free(e), S2N_ERR_OPEN_RANDOM);

    /* Use that engine for rand() */
    e = ENGINE_by_id(S2N_RAND_ENGINE_ID);
    RESULT_ENSURE(e != NULL, S2N_ERR_OPEN_RANDOM);
    RESULT_GUARD_OSSL(ENGINE_init(e), S2N_ERR_OPEN_RANDOM);
    RESULT_GUARD_OSSL(ENGINE_set_default(e, ENGINE_METHOD_RAND), S2N_ERR_OPEN_RANDOM);
    RESULT_GUARD_OSSL(ENGINE_free(e), S2N_ERR_OPEN_RANDOM);
#endif

    return S2N_RESULT_OK;
}

static int s2n_rand_cleanup_cb_impl(void)
{
    POSIX_ENSURE(s2n_dev_urandom.fd != UNINITIALIZED_ENTROPY_FD, S2N_ERR_NOT_INITIALIZED);

    if (s2n_result_is_ok(s2n_rand_device_validate(&s2n_dev_urandom))) {
        POSIX_GUARD(close(s2n_dev_urandom.fd));
    }
    s2n_dev_urandom.fd = UNINITIALIZED_ENTROPY_FD;

    return S2N_SUCCESS;
}

S2N_RESULT s2n_rand_cleanup(void)
{
    RESULT_ENSURE(s2n_rand_cleanup_cb() >= S2N_SUCCESS, S2N_ERR_CANCELLED);

#if S2N_LIBCRYPTO_SUPPORTS_CUSTOM_RAND
    /* Cleanup our rand ENGINE in libcrypto */
    ENGINE *rand_engine = ENGINE_by_id(S2N_RAND_ENGINE_ID);
    if (rand_engine) {
        ENGINE_remove(rand_engine);
        ENGINE_finish(rand_engine);
        ENGINE_unregister_RAND(rand_engine);
        ENGINE_free(rand_engine);
        ENGINE_cleanup();
        RAND_set_rand_engine(NULL);
        RAND_set_rand_method(NULL);
    }
#endif

    s2n_rand_init_cb = s2n_rand_init_cb_impl;
    s2n_rand_cleanup_cb = s2n_rand_cleanup_cb_impl;
    s2n_rand_seed_cb = s2n_rand_get_entropy_from_urandom;
    s2n_rand_mix_cb = s2n_rand_get_entropy_from_urandom;

    return S2N_RESULT_OK;
}

S2N_RESULT s2n_rand_cleanup_thread(void)
{
    /* Currently, it is only safe for this function to mutate the drbg states
     * in the per thread rand state. See s2n_ensure_uniqueness().
     */
    RESULT_GUARD(s2n_drbg_wipe(&s2n_per_thread_rand_state.private_drbg));
    RESULT_GUARD(s2n_drbg_wipe(&s2n_per_thread_rand_state.public_drbg));

    s2n_per_thread_rand_state.drbgs_initialized = false;

    /* Unset the thread-local destructor */
    if (s2n_is_initialized()) {
        pthread_setspecific(s2n_per_thread_rand_state_key, NULL);
    }

    return S2N_RESULT_OK;
}

/* This must only be used for unit tests. Any real use is dangerous and will be
 * overwritten in s2n_ensure_uniqueness() if it is forked. This was added to
 * support known answer tests that use OpenSSL and s2n_get_private_random_data
 * directly.
 */
S2N_RESULT s2n_set_private_drbg_for_test(struct s2n_drbg drbg)
{
    RESULT_ENSURE(s2n_in_unit_test(), S2N_ERR_NOT_IN_UNIT_TEST);
    RESULT_GUARD(s2n_drbg_wipe(&s2n_per_thread_rand_state.private_drbg));

    s2n_per_thread_rand_state.private_drbg = drbg;

    return S2N_RESULT_OK;
}

S2N_RESULT s2n_rand_set_urandom_for_test()
{
    RESULT_ENSURE(s2n_in_unit_test(), S2N_ERR_NOT_IN_UNIT_TEST);
    s2n_rand_mix_cb = s2n_rand_get_entropy_from_urandom;
    return S2N_RESULT_OK;
}

/*
 * volatile is important to prevent the compiler from
 * re-ordering or optimizing the use of RDRAND.
 */
static int s2n_rand_get_entropy_from_rdrand(void *data, uint32_t size)
{
#if defined(__x86_64__) || defined(__i386__)
    struct s2n_blob out = { 0 };
    POSIX_GUARD(s2n_blob_init(&out, data, size));
    size_t space_remaining = 0;
    struct s2n_stuffer stuffer = { 0 };
    union {
        uint64_t u64;
    #if defined(__i386__)
        struct {
            /* since we check first that we're on intel, we can safely assume little endian. */
            uint32_t u_low;
            uint32_t u_high;
        } i386_fields;
    #endif /* defined(__i386__) */
        uint8_t u8[8];
    } output;

    POSIX_GUARD(s2n_stuffer_init(&stuffer, &out));
    while ((space_remaining = s2n_stuffer_space_remaining(&stuffer))) {
        unsigned char success = 0;
        output.u64 = 0;

        for (int tries = 0; tries < 10; tries++) {
    #if defined(__i386__)
            /* execute the rdrand instruction, store the result in a general purpose register (it's assigned to
            * output.i386_fields.u_low). Check the carry bit, which will be set on success. Then clober the register and reset
            * the carry bit. Due to needing to support an ancient assembler we use the opcode syntax.
            * the %b1 is to force compilers to use c1 instead of ecx.
            * Here's a description of how the opcode is encoded:
            * 0x0fc7 (rdrand)
            * 0xf0 (store the result in eax).
            */
            unsigned char success_high = 0, success_low = 0;
            __asm__ __volatile__(
                    ".byte 0x0f, 0xc7, 0xf0;\n"
                    "setc %b1;\n"
                    : "=&a"(output.i386_fields.u_low), "=qm"(success_low)
                    :
                    : "cc");

            __asm__ __volatile__(
                    ".byte 0x0f, 0xc7, 0xf0;\n"
                    "setc %b1;\n"
                    : "=&a"(output.i386_fields.u_high), "=qm"(success_high)
                    :
                    : "cc");
            /* cppcheck-suppress knownConditionTrueFalse */
            success = success_high & success_low;

            /* Treat either all 1 or all 0 bits in either the high or low order
             * bits as failure */
            if (output.i386_fields.u_low == 0 || output.i386_fields.u_low == UINT32_MAX
                    || output.i386_fields.u_high == 0 || output.i386_fields.u_high == UINT32_MAX) {
                success = 0;
            }
    #else
            /* execute the rdrand instruction, store the result in a general purpose register (it's assigned to
            * output.u64). Check the carry bit, which will be set on success. Then clober the carry bit.
            * Due to needing to support an ancient assembler we use the opcode syntax.
            * the %b1 is to force compilers to use c1 instead of ecx.
            * Here's a description of how the opcode is encoded:
            * 0x48 (pick a 64-bit register it does more too, but that's all that matters there)
            * 0x0fc7 (rdrand)
            * 0xf0 (store the result in rax). */
            __asm__ __volatile__(
                    ".byte 0x48, 0x0f, 0xc7, 0xf0;\n"
                    "setc %b1;\n"
                    : "=&a"(output.u64), "=qm"(success)
                    :
                    : "cc");
    #endif /* defined(__i386__) */

            /* Some AMD CPUs will find that RDRAND "sticks" on all 1s but still reports success.
             * Some other very old CPUs use all 0s as an error condition while still reporting success.
             * If we encounter either of these suspicious values (a 1/2^63 chance) we'll treat them as
             * a failure and generate a new value.
             *
             * In the future we could add CPUID checks to detect processors with these known bugs,
             * however it does not appear worth it. The entropy loss is negligible and the
             * corresponding likelihood that a healthy CPU generates either of these values is also
             * negligible (1/2^63). Finally, adding processor specific logic would greatly
             * increase the complexity and would cause us to "miss" any unknown processors with
             * similar bugs. */
            if (output.u64 == UINT64_MAX || output.u64 == 0) {
                success = 0;
            }

            if (success) {
                break;
            }
        }

        POSIX_ENSURE(success, S2N_ERR_RDRAND_FAILED);

        size_t data_to_fill = MIN(sizeof(output), space_remaining);

        POSIX_GUARD(s2n_stuffer_write_bytes(&stuffer, output.u8, data_to_fill));
    }

    return S2N_SUCCESS;
#else
    POSIX_BAIL(S2N_ERR_UNSUPPORTED_CPU);
#endif
}

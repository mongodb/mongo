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
/* LibreSSL requires <openssl/rand.h> include.
 * https://github.com/aws/s2n-tls/issues/153#issuecomment-129651643
 */
#include <openssl/rand.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "api/s2n.h"
#include "crypto/s2n_fips.h"
#include "crypto/s2n_libcrypto.h"
#include "error/s2n_errno.h"
#include "s2n_io.h"
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

/* One second in nanoseconds */
#define ONE_S INT64_C(1000000000)

/* Placeholder value for an uninitialized entropy file descriptor */
#define UNINITIALIZED_ENTROPY_FD -1

static struct s2n_rand_device s2n_dev_urandom = {
    .source = "/dev/urandom",
    .fd = UNINITIALIZED_ENTROPY_FD,
};

static int s2n_rand_init_cb_impl(void);
static int s2n_rand_cleanup_cb_impl(void);
static int s2n_rand_get_entropy_from_urandom(void *ptr, uint32_t size);

static int s2n_rand_entropy_fd_close_ptr(int *fd)
{
    if (fd && *fd != UNINITIALIZED_ENTROPY_FD) {
        close(*fd);
    }
    return S2N_SUCCESS;
}

/*
 * Use libcrypto for randomness when the linked libcrypto supports at least
 * one of RAND_priv_bytes or RAND_public_bytes, or when the libcrypto is
 * AWS-LC (whose RAND_bytes is trusted even in older FIPS builds that lack
 * the distinct pub/priv APIs). For older libcryptos that lack both and are
 * not AWS-LC (e.g. OpenSSL 1.0.2), fall back to system random (/dev/urandom)
 * to avoid depending on the weaker single-stream PRNG.
 */
bool s2n_use_libcrypto_rand(void)
{
#if defined(S2N_LIBCRYPTO_SUPPORTS_PRIVATE_RAND) || defined(S2N_LIBCRYPTO_SUPPORTS_PUBLIC_RAND)
    return true;
#elif defined(OPENSSL_IS_AWSLC)
    /* Older AWS-LC FIPS builds (e.g. aws-lc-fips-2022) may lack
     * RAND_priv_bytes/RAND_public_bytes but still provide a trusted
     * RAND_bytes implementation that we can defer to.
     */
    return true;
#else
    return false;
#endif
}

static S2N_RESULT s2n_get_libcrypto_private_random_data(struct s2n_blob *out_blob)
{
    RESULT_GUARD_PTR(out_blob);
    RESULT_ENSURE_REF(out_blob->data);
#if S2N_LIBCRYPTO_SUPPORTS_PRIVATE_RAND
    RESULT_GUARD_OSSL(RAND_priv_bytes(out_blob->data, out_blob->size), S2N_ERR_RANDOM);
#else
    /* Libcryptos that support RAND_public_bytes but not RAND_priv_bytes still
     * provide RAND_bytes. OpenSSL 1.0.2 (which lacks both) is handled by
     * s2n_use_libcrypto_rand() returning false, so this path is only reached
     * by libcryptos that have at least RAND_public_bytes.
     */
    RESULT_GUARD_OSSL(RAND_bytes(out_blob->data, out_blob->size), S2N_ERR_RANDOM);
#endif
    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_get_libcrypto_public_random_data(struct s2n_blob *out_blob)
{
    RESULT_GUARD_PTR(out_blob);
    RESULT_ENSURE_REF(out_blob->data);
#if S2N_LIBCRYPTO_SUPPORTS_PUBLIC_RAND
    RESULT_GUARD_OSSL(RAND_public_bytes(out_blob->data, out_blob->size), S2N_ERR_RANDOM);
#else
    RESULT_GUARD_OSSL(RAND_bytes(out_blob->data, out_blob->size), S2N_ERR_RANDOM);
#endif
    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_get_system_random_data(struct s2n_blob *blob)
{
    RESULT_GUARD_PTR(blob);
    RESULT_GUARD_PTR(blob->data);

    /* This function sets s2n_errno on failure */
    RESULT_GUARD_POSIX(s2n_rand_get_entropy_from_urandom(blob->data, blob->size));

    return S2N_RESULT_OK;
}

S2N_RESULT s2n_get_public_random_data(struct s2n_blob *blob)
{
    if (s2n_use_libcrypto_rand()) {
        RESULT_GUARD(s2n_get_libcrypto_public_random_data(blob));
    } else {
        RESULT_GUARD(s2n_get_system_random_data(blob));
    }
    return S2N_RESULT_OK;
}

S2N_RESULT s2n_get_private_random_data(struct s2n_blob *blob)
{
    if (s2n_use_libcrypto_rand()) {
        RESULT_GUARD(s2n_get_libcrypto_private_random_data(blob));
    } else {
        RESULT_GUARD(s2n_get_system_random_data(blob));
    }
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

static int s2n_rand_init_cb_impl(void)
{
    POSIX_GUARD_RESULT(s2n_rand_device_open(&s2n_dev_urandom));

    return S2N_SUCCESS;
}

S2N_RESULT s2n_rand_init(void)
{
    /* Only open /dev/urandom when we actually need it for randomness.
     * When libcrypto handles randomness, avoid the extra syscall and fd.
     */
    if (!s2n_use_libcrypto_rand()) {
        RESULT_ENSURE(s2n_rand_init_cb_impl() >= S2N_SUCCESS, S2N_ERR_CANCELLED);
    }

    return S2N_RESULT_OK;
}

static int s2n_rand_cleanup_cb_impl(void)
{
    if (s2n_dev_urandom.fd == UNINITIALIZED_ENTROPY_FD) {
        return S2N_SUCCESS;
    }

    if (s2n_result_is_ok(s2n_rand_device_validate(&s2n_dev_urandom))) {
        POSIX_GUARD(close(s2n_dev_urandom.fd));
    }
    s2n_dev_urandom.fd = UNINITIALIZED_ENTROPY_FD;

    return S2N_SUCCESS;
}

S2N_RESULT s2n_rand_cleanup(void)
{
    if (!s2n_use_libcrypto_rand()) {
        RESULT_ENSURE(s2n_rand_cleanup_cb_impl() >= S2N_SUCCESS, S2N_ERR_CANCELLED);
    }

    return S2N_RESULT_OK;
}

int s2n_rand_set_callbacks(s2n_rand_init_callback rand_init_callback,
        s2n_rand_cleanup_callback rand_cleanup_callback,
        s2n_rand_seed_callback rand_seed_callback,
        s2n_rand_mix_callback rand_mix_callback)
{
    /* Custom random callbacks are no longer supported. Randomness is now
     * delegated directly to libcrypto or /dev/urandom. This stub is kept
     * for backwards compatibility.
     */
    (void) rand_init_callback;
    (void) rand_cleanup_callback;
    (void) rand_seed_callback;
    (void) rand_mix_callback;
    return S2N_SUCCESS;
}

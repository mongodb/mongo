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

#include "crypto/s2n_locking.h"

#include <openssl/crypto.h>
#include <pthread.h>

#include "crypto/s2n_openssl.h"
#include "utils/s2n_mem.h"
#include "utils/s2n_safety.h"

/* Writing multithreaded applications using Openssl-1.0.2
 * requires calling CRYPTO_set_locking_callback.
 * If the callback is not set, locks are no-ops and unexpected
 * behavior may occur, particularly for RSA and X509.
 *
 * In the past s2n-tls relied on customers setting the callback
 * themselves, but that seems unnecessary since other parts of
 * the library (like fork detection) already rely on the pthreads library.
 *
 * For more information:
 * https://www.openssl.org/blog/blog/2017/02/21/threads/
 * https://www.openssl.org/docs/man1.0.2/man3/threads.html
 */

#define S2N_MUTEXES(mem) ((pthread_mutex_t *) (void *) (mem).data)

/* While the locking-related APIs "exist" in later versions of
 * Openssl, they tend to be placeholders or hardcoded values like:
 * #define CRYPTO_get_locking_callback() (NULL)
 * So the code will compile with strange warnings / errors like
 * loop conditions always being false.
 */
#if !(S2N_OPENSSL_VERSION_AT_LEAST(1, 1, 0))

static struct s2n_blob mutexes_mem = { 0 };
static size_t mutexes_count = 0;

static void s2n_locking_cb(int mode, int n, char *file, int line)
{
    pthread_mutex_t *mutexes = S2N_MUTEXES(mutexes_mem);
    if (!mutexes_mem.data || n < 0 || (size_t) n >= mutexes_count) {
        return;
    }

    if (mode & CRYPTO_LOCK) {
        pthread_mutex_lock(&(mutexes[n]));
    } else {
        pthread_mutex_unlock(&(mutexes[n]));
    }
}

S2N_RESULT s2n_locking_init(void)
{
    if (CRYPTO_get_locking_callback() != NULL) {
        return S2N_RESULT_OK;
    }

    int num_locks = CRYPTO_num_locks();
    RESULT_ENSURE_GTE(num_locks, 0);

    RESULT_GUARD_POSIX(s2n_realloc(&mutexes_mem, num_locks * sizeof(pthread_mutex_t)));

    pthread_mutex_t *mutexes = S2N_MUTEXES(mutexes_mem);
    mutexes_count = 0;
    for (size_t i = 0; i < (size_t) num_locks; i++) {
        RESULT_ENSURE_EQ(pthread_mutex_init(&(mutexes[i]), NULL), 0);
        mutexes_count++;
    }

    CRYPTO_set_locking_callback((void (*)()) s2n_locking_cb);
    return S2N_RESULT_OK;
}

S2N_RESULT s2n_locking_cleanup(void)
{
    if (CRYPTO_get_locking_callback() == (void (*)()) s2n_locking_cb) {
        CRYPTO_set_locking_callback(NULL);
    }

    pthread_mutex_t *mutexes = S2N_MUTEXES(mutexes_mem);
    if (mutexes) {
        while (mutexes_count > 0) {
            RESULT_ENSURE_EQ(pthread_mutex_destroy(&(mutexes[mutexes_count - 1])), 0);
            mutexes_count--;
        }
        RESULT_GUARD_POSIX(s2n_free(&mutexes_mem));
    }

    return S2N_RESULT_OK;
}

#else

S2N_RESULT s2n_locking_init(void)
{
    return S2N_RESULT_OK;
}

S2N_RESULT s2n_locking_cleanup(void)
{
    return S2N_RESULT_OK;
}

#endif

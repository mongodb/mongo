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

#include "tls/s2n_handshake_hashes.h"

#include "crypto/s2n_fips.h"
#include "tls/s2n_connection.h"
#include "utils/s2n_blob.h"
#include "utils/s2n_mem.h"
#include "utils/s2n_safety.h"

static S2N_RESULT s2n_handshake_hashes_new_hashes(struct s2n_handshake_hashes *hashes)
{
    RESULT_ENSURE_REF(hashes);
    RESULT_GUARD_POSIX(s2n_hash_new(&hashes->md5));
    RESULT_GUARD_POSIX(s2n_hash_new(&hashes->sha1));
    RESULT_GUARD_POSIX(s2n_hash_new(&hashes->sha224));
    RESULT_GUARD_POSIX(s2n_hash_new(&hashes->sha256));
    RESULT_GUARD_POSIX(s2n_hash_new(&hashes->sha384));
    RESULT_GUARD_POSIX(s2n_hash_new(&hashes->sha512));
    RESULT_GUARD_POSIX(s2n_hash_new(&hashes->md5_sha1));
    RESULT_GUARD_POSIX(s2n_hash_new(&hashes->hash_workspace));
    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_handshake_hashes_reset_hashes(struct s2n_handshake_hashes *hashes)
{
    RESULT_ENSURE_REF(hashes);
    RESULT_GUARD_POSIX(s2n_hash_reset(&hashes->md5));
    RESULT_GUARD_POSIX(s2n_hash_reset(&hashes->sha1));
    RESULT_GUARD_POSIX(s2n_hash_reset(&hashes->sha224));
    RESULT_GUARD_POSIX(s2n_hash_reset(&hashes->sha256));
    RESULT_GUARD_POSIX(s2n_hash_reset(&hashes->sha384));
    RESULT_GUARD_POSIX(s2n_hash_reset(&hashes->sha512));
    RESULT_GUARD_POSIX(s2n_hash_reset(&hashes->md5_sha1));
    RESULT_GUARD_POSIX(s2n_hash_reset(&hashes->hash_workspace));
    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_handshake_hashes_free_hashes(struct s2n_handshake_hashes *hashes)
{
    if (!hashes) {
        return S2N_RESULT_OK;
    }
    RESULT_GUARD_POSIX(s2n_hash_free(&hashes->md5));
    RESULT_GUARD_POSIX(s2n_hash_free(&hashes->sha1));
    RESULT_GUARD_POSIX(s2n_hash_free(&hashes->sha224));
    RESULT_GUARD_POSIX(s2n_hash_free(&hashes->sha256));
    RESULT_GUARD_POSIX(s2n_hash_free(&hashes->sha384));
    RESULT_GUARD_POSIX(s2n_hash_free(&hashes->sha512));
    RESULT_GUARD_POSIX(s2n_hash_free(&hashes->md5_sha1));
    RESULT_GUARD_POSIX(s2n_hash_free(&hashes->hash_workspace));
    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_handshake_hashes_init_hashes(struct s2n_handshake_hashes *hashes)
{
    /* Allow MD5 for hash states that are used by the PRF. This is required
     * to comply with the TLS 1.0 and 1.1 RFCs and is approved as per
     * NIST Special Publication 800-52 Revision 1.
     */
    if (s2n_is_in_fips_mode()) {
        RESULT_GUARD_POSIX(s2n_hash_allow_md5_for_fips(&hashes->md5));

        /* Do not check s2n_hash_is_available before initialization. Allow MD5 and
         * SHA-1 for both fips and non-fips mode. This is required to perform the
         * signature checks in the CertificateVerify message in TLS 1.0 and TLS 1.1.
         * This is approved per Nist SP 800-52r1.*/
        RESULT_GUARD_POSIX(s2n_hash_allow_md5_for_fips(&hashes->md5_sha1));
    }

    RESULT_GUARD_POSIX(s2n_hash_init(&hashes->md5, S2N_HASH_MD5));
    RESULT_GUARD_POSIX(s2n_hash_init(&hashes->sha1, S2N_HASH_SHA1));
    RESULT_GUARD_POSIX(s2n_hash_init(&hashes->sha224, S2N_HASH_SHA224));
    RESULT_GUARD_POSIX(s2n_hash_init(&hashes->sha256, S2N_HASH_SHA256));
    RESULT_GUARD_POSIX(s2n_hash_init(&hashes->sha384, S2N_HASH_SHA384));
    RESULT_GUARD_POSIX(s2n_hash_init(&hashes->sha512, S2N_HASH_SHA512));
    RESULT_GUARD_POSIX(s2n_hash_init(&hashes->md5_sha1, S2N_HASH_MD5_SHA1));
    RESULT_GUARD_POSIX(s2n_hash_init(&hashes->hash_workspace, S2N_HASH_NONE));

    return S2N_RESULT_OK;
}

S2N_RESULT s2n_handshake_hashes_new(struct s2n_handshake_hashes **hashes)
{
    RESULT_ENSURE_REF(hashes);
    RESULT_ENSURE_EQ(*hashes, NULL);

    DEFER_CLEANUP(struct s2n_blob data = { 0 }, s2n_free);
    RESULT_GUARD_POSIX(s2n_realloc(&data, sizeof(struct s2n_handshake_hashes)));
    RESULT_GUARD_POSIX(s2n_blob_zero(&data));
    *hashes = (struct s2n_handshake_hashes *) (void *) data.data;
    ZERO_TO_DISABLE_DEFER_CLEANUP(data);

    RESULT_GUARD(s2n_handshake_hashes_new_hashes(*hashes));
    RESULT_GUARD(s2n_handshake_hashes_init_hashes(*hashes));

    return S2N_RESULT_OK;
}

S2N_RESULT s2n_handshake_hashes_wipe(struct s2n_handshake_hashes *hashes)
{
    RESULT_GUARD(s2n_handshake_hashes_reset_hashes(hashes));
    return S2N_RESULT_OK;
}

S2N_CLEANUP_RESULT s2n_handshake_hashes_free(struct s2n_handshake_hashes **hashes)
{
    RESULT_ENSURE_REF(hashes);
    RESULT_GUARD(s2n_handshake_hashes_free_hashes(*hashes));
    RESULT_GUARD_POSIX(s2n_free_object((uint8_t **) hashes, sizeof(struct s2n_handshake_hashes)));
    return S2N_RESULT_OK;
}

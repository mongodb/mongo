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

#include "crypto/s2n_dhe.h"

#include <openssl/bn.h>
#include <openssl/dh.h>
#include <openssl/evp.h>
#include <stdint.h>

#include "crypto/s2n_openssl.h"
#include "error/s2n_errno.h"
#include "stuffer/s2n_stuffer.h"
#include "utils/s2n_blob.h"
#include "utils/s2n_mem.h"
#include "utils/s2n_safety.h"

#define S2N_MIN_DH_PRIME_SIZE_BYTES (2048 / 8)

/* Caller is not responsible for freeing values returned by these accessors
 * Per https://www.openssl.org/docs/man1.1.0/crypto/DH_get0_pqg.html
 */
static const BIGNUM *s2n_get_Ys_dh_param(struct s2n_dh_params *dh_params)
{
    const BIGNUM *Ys = NULL;

/* DH made opaque in Openssl 1.1.0 */
#if S2N_OPENSSL_VERSION_AT_LEAST(1, 1, 0)
    DH_get0_key(dh_params->dh, &Ys, NULL);
#else
    Ys = dh_params->dh->pub_key;
#endif

    return Ys;
}

static const BIGNUM *s2n_get_p_dh_param(struct s2n_dh_params *dh_params)
{
    const BIGNUM *p = NULL;
#if S2N_OPENSSL_VERSION_AT_LEAST(1, 1, 0)
    DH_get0_pqg(dh_params->dh, &p, NULL, NULL);
#else
    p = dh_params->dh->p;
#endif

    return p;
}

static const BIGNUM *s2n_get_g_dh_param(struct s2n_dh_params *dh_params)
{
    const BIGNUM *g = NULL;
#if S2N_OPENSSL_VERSION_AT_LEAST(1, 1, 0)
    DH_get0_pqg(dh_params->dh, NULL, NULL, &g);
#else
    g = dh_params->dh->g;
#endif

    return g;
}

static int s2n_check_p_g_dh_params(struct s2n_dh_params *dh_params)
{
    POSIX_ENSURE_REF(dh_params);
    POSIX_ENSURE_REF(dh_params->dh);

    const BIGNUM *p = s2n_get_p_dh_param(dh_params);
    const BIGNUM *g = s2n_get_g_dh_param(dh_params);

    POSIX_ENSURE_REF(g);
    POSIX_ENSURE_REF(p);

    S2N_ERROR_IF(DH_size(dh_params->dh) < S2N_MIN_DH_PRIME_SIZE_BYTES, S2N_ERR_DH_PARAMS_CREATE);
    S2N_ERROR_IF(BN_is_zero(g), S2N_ERR_DH_PARAMS_CREATE);
    S2N_ERROR_IF(BN_is_zero(p), S2N_ERR_DH_PARAMS_CREATE);

    return S2N_SUCCESS;
}

static int s2n_check_pub_key_dh_params(struct s2n_dh_params *dh_params)
{
    const BIGNUM *pub_key = s2n_get_Ys_dh_param(dh_params);

    POSIX_ENSURE_REF(pub_key);

    S2N_ERROR_IF(BN_is_zero(pub_key), S2N_ERR_DH_PARAMS_CREATE);

    return S2N_SUCCESS;
}

static int s2n_set_p_g_Ys_dh_params(struct s2n_dh_params *dh_params, struct s2n_blob *p, struct s2n_blob *g,
        struct s2n_blob *Ys)
{
    POSIX_ENSURE(p->size <= INT_MAX, S2N_ERR_INTEGER_OVERFLOW);
    POSIX_ENSURE(g->size <= INT_MAX, S2N_ERR_INTEGER_OVERFLOW);
    POSIX_ENSURE(Ys->size <= INT_MAX, S2N_ERR_INTEGER_OVERFLOW);
    BIGNUM *bn_p = BN_bin2bn((const unsigned char *) p->data, p->size, NULL);
    BIGNUM *bn_g = BN_bin2bn((const unsigned char *) g->data, g->size, NULL);
    BIGNUM *bn_Ys = BN_bin2bn((const unsigned char *) Ys->data, Ys->size, NULL);

#if S2N_OPENSSL_VERSION_AT_LEAST(1, 1, 0)
    /* Per https://www.openssl.org/docs/man1.1.0/crypto/DH_get0_pqg.html:
	* values that have been passed in should not be freed directly after this function has been called
	*/
    POSIX_GUARD_OSSL(DH_set0_pqg(dh_params->dh, bn_p, NULL, bn_g), S2N_ERR_DH_PARAMS_CREATE);

    /* Same as DH_set0_pqg */
    POSIX_GUARD_OSSL(DH_set0_key(dh_params->dh, bn_Ys, NULL), S2N_ERR_DH_PARAMS_CREATE);
#else
    dh_params->dh->p = bn_p;
    dh_params->dh->g = bn_g;
    dh_params->dh->pub_key = bn_Ys;
#endif

    return S2N_SUCCESS;
}

int s2n_check_all_dh_params(struct s2n_dh_params *dh_params)
{
    POSIX_GUARD(s2n_check_p_g_dh_params(dh_params));
    POSIX_GUARD(s2n_check_pub_key_dh_params(dh_params));

    return S2N_SUCCESS;
}

int s2n_pkcs3_to_dh_params(struct s2n_dh_params *dh_params, struct s2n_blob *pkcs3)
{
    POSIX_ENSURE_REF(dh_params);
    POSIX_PRECONDITION(s2n_blob_validate(pkcs3));
    DEFER_CLEANUP(struct s2n_dh_params temp_dh_params = { 0 }, s2n_dh_params_free);

    uint8_t *original_ptr = pkcs3->data;
    temp_dh_params.dh = d2i_DHparams(NULL, (const unsigned char **) (void *) &pkcs3->data, pkcs3->size);

    POSIX_GUARD(s2n_check_p_g_dh_params(&temp_dh_params));

    if (pkcs3->data) {
        POSIX_ENSURE_GTE(pkcs3->data, original_ptr);
        POSIX_ENSURE((uint32_t) (pkcs3->data - original_ptr) == pkcs3->size, S2N_ERR_INVALID_PKCS3);
    }

    pkcs3->data = original_ptr;

    /* Require at least 2048 bits for the DH size */
    POSIX_ENSURE(DH_size(temp_dh_params.dh) >= S2N_MIN_DH_PRIME_SIZE_BYTES, S2N_ERR_DH_TOO_SMALL);

    /* Check the generator and prime */
    POSIX_GUARD(s2n_dh_params_check(&temp_dh_params));

    dh_params->dh = temp_dh_params.dh;

    ZERO_TO_DISABLE_DEFER_CLEANUP(temp_dh_params);

    return S2N_SUCCESS;
}

int s2n_dh_p_g_Ys_to_dh_params(struct s2n_dh_params *server_dh_params, struct s2n_blob *p, struct s2n_blob *g,
        struct s2n_blob *Ys)
{
    POSIX_ENSURE_REF(server_dh_params);
    POSIX_PRECONDITION(s2n_blob_validate(p));
    POSIX_PRECONDITION(s2n_blob_validate(g));
    POSIX_PRECONDITION(s2n_blob_validate(Ys));

    server_dh_params->dh = DH_new();
    POSIX_ENSURE(server_dh_params->dh != NULL, S2N_ERR_DH_PARAMS_CREATE);

    POSIX_GUARD(s2n_set_p_g_Ys_dh_params(server_dh_params, p, g, Ys));
    POSIX_GUARD(s2n_check_all_dh_params(server_dh_params));

    return S2N_SUCCESS;
}

int s2n_dh_params_to_p_g_Ys(struct s2n_dh_params *server_dh_params, struct s2n_stuffer *out, struct s2n_blob *output)
{
    POSIX_GUARD(s2n_check_all_dh_params(server_dh_params));
    POSIX_PRECONDITION(s2n_stuffer_validate(out));
    POSIX_PRECONDITION(s2n_blob_validate(output));

    const BIGNUM *bn_p = s2n_get_p_dh_param(server_dh_params);
    const BIGNUM *bn_g = s2n_get_g_dh_param(server_dh_params);
    const BIGNUM *bn_Ys = s2n_get_Ys_dh_param(server_dh_params);

    uint16_t p_size = BN_num_bytes(bn_p);
    uint16_t g_size = BN_num_bytes(bn_g);
    uint16_t Ys_size = BN_num_bytes(bn_Ys);
    uint8_t *p = NULL;
    uint8_t *g = NULL;
    uint8_t *Ys = NULL;

    output->data = s2n_stuffer_raw_write(out, 0);
    POSIX_ENSURE_REF(output->data);

    POSIX_GUARD(s2n_stuffer_write_uint16(out, p_size));
    p = s2n_stuffer_raw_write(out, p_size);
    POSIX_ENSURE_REF(p);
    POSIX_ENSURE(BN_bn2bin(bn_p, p) == p_size, S2N_ERR_DH_SERIALIZING);

    POSIX_GUARD(s2n_stuffer_write_uint16(out, g_size));
    g = s2n_stuffer_raw_write(out, g_size);
    POSIX_ENSURE_REF(g);
    POSIX_ENSURE(BN_bn2bin(bn_g, g) == g_size, S2N_ERR_DH_SERIALIZING);

    POSIX_GUARD(s2n_stuffer_write_uint16(out, Ys_size));
    Ys = s2n_stuffer_raw_write(out, Ys_size);
    POSIX_ENSURE_REF(Ys);
    POSIX_ENSURE(BN_bn2bin(bn_Ys, Ys) == Ys_size, S2N_ERR_DH_SERIALIZING);

    output->size = p_size + 2 + g_size + 2 + Ys_size + 2;

    return S2N_SUCCESS;
}

int s2n_dh_compute_shared_secret_as_client(struct s2n_dh_params *server_dh_params, struct s2n_stuffer *Yc_out,
        struct s2n_blob *shared_key)
{
    struct s2n_dh_params client_params = { 0 };
    uint8_t *client_pub_key = NULL;
    uint16_t client_pub_key_size = 0;
    int shared_key_size = 0;

    POSIX_GUARD(s2n_dh_params_check(server_dh_params));
    POSIX_GUARD(s2n_dh_params_copy(server_dh_params, &client_params));
    POSIX_GUARD(s2n_dh_generate_ephemeral_key(&client_params));
    POSIX_GUARD(s2n_alloc(shared_key, DH_size(server_dh_params->dh)));

    const BIGNUM *client_pub_key_bn = s2n_get_Ys_dh_param(&client_params);
    POSIX_ENSURE_REF(client_pub_key_bn);
    client_pub_key_size = BN_num_bytes(client_pub_key_bn);
    POSIX_GUARD(s2n_stuffer_write_uint16(Yc_out, client_pub_key_size));
    client_pub_key = s2n_stuffer_raw_write(Yc_out, client_pub_key_size);
    if (client_pub_key == NULL) {
        POSIX_GUARD(s2n_free(shared_key));
        POSIX_GUARD(s2n_dh_params_free(&client_params));
        POSIX_BAIL(S2N_ERR_DH_WRITING_PUBLIC_KEY);
    }

    if (BN_bn2bin(client_pub_key_bn, client_pub_key) != client_pub_key_size) {
        POSIX_GUARD(s2n_free(shared_key));
        POSIX_GUARD(s2n_dh_params_free(&client_params));
        POSIX_BAIL(S2N_ERR_DH_COPYING_PUBLIC_KEY);
    }

    /* server_dh_params already validated */
    const BIGNUM *server_pub_key_bn = s2n_get_Ys_dh_param(server_dh_params);
    shared_key_size = DH_compute_key(shared_key->data, server_pub_key_bn, client_params.dh);
    if (shared_key_size < 0) {
        POSIX_GUARD(s2n_free(shared_key));
        POSIX_GUARD(s2n_dh_params_free(&client_params));
        POSIX_BAIL(S2N_ERR_DH_SHARED_SECRET);
    }

    shared_key->size = shared_key_size;

    POSIX_GUARD(s2n_dh_params_free(&client_params));

    return S2N_SUCCESS;
}

int s2n_dh_compute_shared_secret_as_server(struct s2n_dh_params *server_dh_params, struct s2n_stuffer *Yc_in,
        struct s2n_blob *shared_key)
{
    uint16_t Yc_length = 0;
    struct s2n_blob Yc = { 0 };
    int shared_key_size = 0;
    BIGNUM *pub_key = NULL;

    POSIX_GUARD(s2n_check_all_dh_params(server_dh_params));

    POSIX_GUARD(s2n_stuffer_read_uint16(Yc_in, &Yc_length));
    Yc.size = Yc_length;
    Yc.data = s2n_stuffer_raw_read(Yc_in, Yc.size);
    POSIX_ENSURE_REF(Yc.data);

    pub_key = BN_bin2bn((const unsigned char *) Yc.data, Yc.size, NULL);
    POSIX_ENSURE_REF(pub_key);
    int server_dh_params_size = DH_size(server_dh_params->dh);
    POSIX_ENSURE(server_dh_params_size <= INT32_MAX, S2N_ERR_INTEGER_OVERFLOW);
    POSIX_GUARD(s2n_alloc(shared_key, server_dh_params_size));

    shared_key_size = DH_compute_key(shared_key->data, pub_key, server_dh_params->dh);
    if (shared_key_size <= 0) {
        BN_free(pub_key);
        POSIX_BAIL(S2N_ERR_DH_SHARED_SECRET);
    }

    shared_key->size = shared_key_size;

    BN_free(pub_key);

    return S2N_SUCCESS;
}

int s2n_dh_params_check(struct s2n_dh_params *dh_params)
{
    POSIX_ENSURE_REF(dh_params);
    POSIX_ENSURE_REF(dh_params->dh);
    int codes = 0;

    POSIX_GUARD_OSSL(DH_check(dh_params->dh, &codes), S2N_ERR_DH_PARAMETER_CHECK);
    POSIX_ENSURE(codes == 0, S2N_ERR_DH_PARAMETER_CHECK);

    return S2N_SUCCESS;
}

int s2n_dh_params_copy(struct s2n_dh_params *from, struct s2n_dh_params *to)
{
    POSIX_GUARD(s2n_check_p_g_dh_params(from));
    POSIX_ENSURE_REF(to);

    to->dh = DHparams_dup(from->dh);
    POSIX_ENSURE(to->dh != NULL, S2N_ERR_DH_COPYING_PARAMETERS);

    return S2N_SUCCESS;
}

int s2n_dh_generate_ephemeral_key(struct s2n_dh_params *dh_params)
{
    POSIX_GUARD(s2n_check_p_g_dh_params(dh_params));

    POSIX_GUARD_OSSL(DH_generate_key(dh_params->dh), S2N_ERR_DH_GENERATING_PARAMETERS);

    return S2N_SUCCESS;
}

int s2n_dh_params_free(struct s2n_dh_params *dh_params)
{
    POSIX_ENSURE_REF(dh_params);
    DH_free(dh_params->dh);
    dh_params->dh = NULL;

    return S2N_SUCCESS;
}

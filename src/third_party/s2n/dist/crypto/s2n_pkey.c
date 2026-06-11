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

#include "crypto/s2n_pkey.h"

#include <openssl/evp.h>

#include "crypto/s2n_mldsa.h"
#include "crypto/s2n_openssl_evp.h"
#include "crypto/s2n_openssl_x509.h"
#include "crypto/s2n_pkey_evp.h"
#include "crypto/s2n_rsa_pss.h"
#include "error/s2n_errno.h"
#include "utils/s2n_mem.h"
#include "utils/s2n_result.h"
#include "utils/s2n_safety.h"

#ifndef EVP_PKEY_RSA_PSS
    #define EVP_PKEY_RSA_PSS EVP_PKEY_NONE
#endif

int s2n_pkey_zero_init(struct s2n_pkey *pkey)
{
    pkey->pkey = NULL;
    pkey->size = NULL;
    pkey->sign = NULL;
    pkey->verify = NULL;
    pkey->encrypt = NULL;
    pkey->decrypt = NULL;
    return 0;
}

S2N_RESULT s2n_pkey_setup_for_type(struct s2n_pkey *pkey, s2n_pkey_type pkey_type)
{
    switch (pkey_type) {
        case S2N_PKEY_TYPE_RSA:
        case S2N_PKEY_TYPE_ECDSA:
        case S2N_PKEY_TYPE_RSA_PSS:
        case S2N_PKEY_TYPE_MLDSA:
            return s2n_pkey_evp_init(pkey);
        case S2N_PKEY_TYPE_SENTINEL:
        case S2N_PKEY_TYPE_UNKNOWN:
            RESULT_BAIL(S2N_ERR_CERT_TYPE_UNSUPPORTED);
    }
    RESULT_BAIL(S2N_ERR_CERT_TYPE_UNSUPPORTED);
}

int s2n_pkey_check_key_exists(const struct s2n_pkey *pkey)
{
    POSIX_ENSURE_REF(pkey);
    POSIX_ENSURE_REF(pkey->pkey);
    return S2N_SUCCESS;
}

S2N_RESULT s2n_pkey_size(const struct s2n_pkey *pkey, uint32_t *size_out)
{
    RESULT_ENSURE_REF(pkey);
    RESULT_ENSURE_REF(pkey->size);
    RESULT_ENSURE_REF(size_out);

    RESULT_GUARD(pkey->size(pkey, size_out));

    return S2N_RESULT_OK;
}

int s2n_pkey_sign(const struct s2n_pkey *pkey, s2n_signature_algorithm sig_alg,
        struct s2n_hash_state *digest, struct s2n_blob *signature)
{
    POSIX_ENSURE_REF(pkey->sign);

    return pkey->sign(pkey, sig_alg, digest, signature);
}

int s2n_pkey_verify(const struct s2n_pkey *pkey, s2n_signature_algorithm sig_alg,
        struct s2n_hash_state *digest, struct s2n_blob *signature)
{
    POSIX_ENSURE_REF(pkey);
    POSIX_ENSURE_REF(pkey->verify);

    return pkey->verify(pkey, sig_alg, digest, signature);
}

int s2n_pkey_encrypt(const struct s2n_pkey *pkey, struct s2n_blob *in, struct s2n_blob *out)
{
    POSIX_ENSURE_REF(pkey->encrypt);

    return pkey->encrypt(pkey, in, out);
}

int s2n_pkey_decrypt(const struct s2n_pkey *pkey, struct s2n_blob *in, struct s2n_blob *out)
{
    POSIX_ENSURE_REF(pkey->decrypt);

    return pkey->decrypt(pkey, in, out);
}

int s2n_pkey_match(const struct s2n_pkey *pub_key, const struct s2n_pkey *priv_key)
{
    POSIX_ENSURE_REF(pub_key);

    /* Minimally, both keys must be of the same type */
    s2n_pkey_type priv_type = 0, pub_type = 0;
    POSIX_GUARD_RESULT(s2n_pkey_get_type(priv_key->pkey, &priv_type));
    POSIX_GUARD_RESULT(s2n_pkey_get_type(pub_key->pkey, &pub_type));
    POSIX_ENSURE(priv_type == pub_type, S2N_ERR_KEY_MISMATCH);

    /* If both keys are of the same type, check that the public key
     * can verify a test signature from the private key.
     */

    uint8_t input[] = "key check";
    DEFER_CLEANUP(struct s2n_blob signature = { 0 }, s2n_free);

    /* Choose one signature algorithm to test each type of pkey.
     * For example, RSA certs can be used for either S2N_SIGNATURE_RSA (PKCS1)
     * or S2N_SIGNATURE_RSA_PSS_RSAE, but we only test with S2N_SIGNATURE_RSA.
     */
    s2n_signature_algorithm check_alg = S2N_SIGNATURE_ANONYMOUS;
    s2n_hash_algorithm hash_alg = S2N_HASH_SHA256;
    switch (priv_type) {
        case S2N_PKEY_TYPE_ECDSA:
            check_alg = S2N_SIGNATURE_ECDSA;
            break;
        case S2N_PKEY_TYPE_RSA:
            check_alg = S2N_SIGNATURE_RSA;
            break;
        case S2N_PKEY_TYPE_RSA_PSS:
            check_alg = S2N_SIGNATURE_RSA_PSS_PSS;
            break;
        case S2N_PKEY_TYPE_MLDSA:
            check_alg = S2N_SIGNATURE_MLDSA;
            hash_alg = S2N_HASH_SHAKE256_64;
            break;
        default:
            POSIX_BAIL(S2N_ERR_CERT_TYPE_UNSUPPORTED);
    }

    DEFER_CLEANUP(struct s2n_hash_state state_in = { 0 }, s2n_hash_free);
    POSIX_GUARD(s2n_hash_new(&state_in));
    POSIX_GUARD(s2n_hash_init(&state_in, hash_alg));
    POSIX_GUARD_RESULT(s2n_pkey_init_hash(pub_key, check_alg, &state_in));
    POSIX_GUARD(s2n_hash_update(&state_in, input, sizeof(input)));

    DEFER_CLEANUP(struct s2n_hash_state state_out = { 0 }, s2n_hash_free);
    POSIX_GUARD(s2n_hash_new(&state_out));
    POSIX_GUARD(s2n_hash_copy(&state_out, &state_in));

    uint32_t size = 0;
    POSIX_GUARD_RESULT(s2n_pkey_size(priv_key, &size));
    POSIX_GUARD(s2n_alloc(&signature, size));

    /* Note: The Libcrypto RSA EVP_PKEY will cache certain computations used for
     * RSA signing.
     * 
     * This means that the first RSA sign with an EVP_PKEY is ~300 us slower
     * than subsequent sign operations. The effect is much smaller for ECDSA signatures.
     * 
     * If this pkey_sign operation is moved out of config creation, then the
     * 300 us penalty will be paid by the first handshake done on the config.
     */
    POSIX_GUARD(s2n_pkey_sign(priv_key, check_alg, &state_in, &signature));
    POSIX_ENSURE(s2n_pkey_verify(pub_key, check_alg, &state_out, &signature) == S2N_SUCCESS,
            S2N_ERR_KEY_MISMATCH);

    return S2N_SUCCESS;
}

int s2n_pkey_free(struct s2n_pkey *key)
{
    if (key == NULL) {
        return S2N_SUCCESS;
    }
    if (key->pkey != NULL) {
        EVP_PKEY_free(key->pkey);
        key->pkey = NULL;
    }
    return S2N_SUCCESS;
}

S2N_RESULT s2n_asn1der_to_private_key(struct s2n_pkey *priv_key, struct s2n_blob *asn1der, int type_hint)
{
    const unsigned char *key_to_parse = asn1der->data;

    /* We use "d2i_AutoPrivateKey" instead of "PEM_read_bio_PrivateKey" because
     * s2n-tls prefers to perform its own custom PEM parsing. Historically,
     * openssl's PEM parsing tended to ignore invalid certificates rather than
     * error on them. We prefer to fail early rather than continue without
     * the full and correct chain intended by the application.
     */
    DEFER_CLEANUP(EVP_PKEY *evp_private_key = d2i_AutoPrivateKey(NULL, &key_to_parse, asn1der->size),
            EVP_PKEY_free_pointer);

    /* We have found cases where d2i_AutoPrivateKey fails to detect the type of
     * the key. For example, openssl fails to identify an EC key without the
     * optional publicKey field.
     *
     * If d2i_AutoPrivateKey fails, try once more with the type we parsed from the PEM.
     */
    if (evp_private_key == NULL) {
        evp_private_key = d2i_PrivateKey(type_hint, NULL, &key_to_parse, asn1der->size);
    }
    RESULT_ENSURE(evp_private_key, S2N_ERR_DECODE_PRIVATE_KEY);

    /* If key parsing is successful, d2i_AutoPrivateKey increments *key_to_parse to the byte following the parsed data */
    uint32_t parsed_len = key_to_parse - asn1der->data;
    RESULT_ENSURE(parsed_len == asn1der->size, S2N_ERR_DECODE_PRIVATE_KEY);

    /* Initialize s2n_pkey according to key type */
    s2n_pkey_type type = 0;
    RESULT_GUARD(s2n_pkey_get_type(evp_private_key, &type));
    RESULT_GUARD(s2n_pkey_setup_for_type(priv_key, type));

    priv_key->pkey = evp_private_key;
    ZERO_TO_DISABLE_DEFER_CLEANUP(evp_private_key);

    return S2N_RESULT_OK;
}

S2N_RESULT s2n_asn1der_to_public_key_and_type(struct s2n_pkey *pub_key,
        s2n_pkey_type *pkey_type_out, struct s2n_blob *asn1der)
{
    DEFER_CLEANUP(X509 *cert = NULL, X509_free_pointer);
    RESULT_GUARD(s2n_openssl_x509_parse(asn1der, &cert));
    RESULT_GUARD(s2n_pkey_from_x509(cert, pub_key, pkey_type_out));

    return S2N_RESULT_OK;
}

S2N_RESULT s2n_pkey_get_type(EVP_PKEY *evp_pkey, s2n_pkey_type *pkey_type)
{
    RESULT_ENSURE_REF(evp_pkey);
    RESULT_ENSURE_REF(pkey_type);
    *pkey_type = S2N_PKEY_TYPE_UNKNOWN;

    int type = EVP_PKEY_base_id(evp_pkey);
    switch (type) {
        case EVP_PKEY_RSA:
            *pkey_type = S2N_PKEY_TYPE_RSA;
            break;
        case EVP_PKEY_RSA_PSS:
            *pkey_type = S2N_PKEY_TYPE_RSA_PSS;
            break;
        case EVP_PKEY_EC:
            *pkey_type = S2N_PKEY_TYPE_ECDSA;
            break;
#if S2N_LIBCRYPTO_SUPPORTS_MLDSA
        case EVP_PKEY_PQDSA:
            *pkey_type = S2N_PKEY_TYPE_MLDSA;
            break;
#endif
        default:
            RESULT_BAIL(S2N_ERR_DECODE_CERTIFICATE);
    }

    return S2N_RESULT_OK;
}

S2N_RESULT s2n_pkey_from_x509(X509 *cert, struct s2n_pkey *pub_key_out,
        s2n_pkey_type *pkey_type_out)
{
    RESULT_ENSURE_REF(cert);
    RESULT_ENSURE_REF(pub_key_out);
    RESULT_ENSURE_REF(pkey_type_out);

    DEFER_CLEANUP(EVP_PKEY *evp_public_key = X509_get_pubkey(cert), EVP_PKEY_free_pointer);
    RESULT_ENSURE(evp_public_key != NULL, S2N_ERR_DECODE_CERTIFICATE);

    RESULT_GUARD(s2n_pkey_get_type(evp_public_key, pkey_type_out));
    RESULT_GUARD(s2n_pkey_setup_for_type(pub_key_out, *pkey_type_out));

    pub_key_out->pkey = evp_public_key;
    ZERO_TO_DISABLE_DEFER_CLEANUP(evp_public_key);

    return S2N_RESULT_OK;
}

S2N_RESULT s2n_pkey_init_hash(const struct s2n_pkey *pkey,
        s2n_signature_algorithm sig_alg, struct s2n_hash_state *hash)
{
    if (sig_alg == S2N_SIGNATURE_MLDSA) {
        RESULT_GUARD(s2n_mldsa_init_mu_hash(hash, pkey));
    }
    return S2N_RESULT_OK;
}

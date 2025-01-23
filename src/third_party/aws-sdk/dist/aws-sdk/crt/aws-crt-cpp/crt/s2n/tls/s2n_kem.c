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

#include "tls/s2n_kem.h"

#include "crypto/s2n_evp_kem.h"
#include "crypto/s2n_pq.h"
#include "stuffer/s2n_stuffer.h"
#include "tls/extensions/s2n_key_share.h"
#include "tls/s2n_tls_parameters.h"
#include "utils/s2n_mem.h"
#include "utils/s2n_safety.h"

const struct s2n_kem s2n_mlkem_768 = {
    .name = "mlkem768",
    .kem_nid = S2N_NID_MLKEM768,
    .kem_extension_id = 0, /* This is not used in TLS 1.2's KEM extension */
    .public_key_length = S2N_MLKEM_768_PUBLIC_KEY_BYTES,
    .private_key_length = S2N_MLKEM_768_SECRET_KEY_BYTES,
    .shared_secret_key_length = S2N_MLKEM_768_SHARED_SECRET_BYTES,
    .ciphertext_length = S2N_MLKEM_768_CIPHERTEXT_BYTES,
    .generate_keypair = &s2n_evp_kem_generate_keypair,
    .encapsulate = &s2n_evp_kem_encapsulate,
    .decapsulate = &s2n_evp_kem_decapsulate,
};

const struct s2n_kem s2n_kyber_512_r3 = {
    .name = "kyber512r3",
    .kem_nid = S2N_NID_KYBER512,
    .kem_extension_id = TLS_PQ_KEM_EXTENSION_ID_KYBER_512_R3,
    .public_key_length = S2N_KYBER_512_R3_PUBLIC_KEY_BYTES,
    .private_key_length = S2N_KYBER_512_R3_SECRET_KEY_BYTES,
    .shared_secret_key_length = S2N_KYBER_512_R3_SHARED_SECRET_BYTES,
    .ciphertext_length = S2N_KYBER_512_R3_CIPHERTEXT_BYTES,
    .generate_keypair = &s2n_evp_kem_generate_keypair,
    .encapsulate = &s2n_evp_kem_encapsulate,
    .decapsulate = &s2n_evp_kem_decapsulate,
};

const struct s2n_kem s2n_kyber_768_r3 = {
    .name = "kyber768r3",
    .kem_nid = S2N_NID_KYBER768,
    .kem_extension_id = 0, /* This is not used in TLS 1.2's KEM extension */
    .public_key_length = S2N_KYBER_768_R3_PUBLIC_KEY_BYTES,
    .private_key_length = S2N_KYBER_768_R3_SECRET_KEY_BYTES,
    .shared_secret_key_length = S2N_KYBER_768_R3_SHARED_SECRET_BYTES,
    .ciphertext_length = S2N_KYBER_768_R3_CIPHERTEXT_BYTES,
    .generate_keypair = &s2n_evp_kem_generate_keypair,
    .encapsulate = &s2n_evp_kem_encapsulate,
    .decapsulate = &s2n_evp_kem_decapsulate,
};

const struct s2n_kem s2n_kyber_1024_r3 = {
    .name = "kyber1024r3",
    .kem_nid = S2N_NID_KYBER1024,
    .kem_extension_id = 0, /* This is not used in TLS 1.2's KEM extension */
    .public_key_length = S2N_KYBER_1024_R3_PUBLIC_KEY_BYTES,
    .private_key_length = S2N_KYBER_1024_R3_SECRET_KEY_BYTES,
    .shared_secret_key_length = S2N_KYBER_1024_R3_SHARED_SECRET_BYTES,
    .ciphertext_length = S2N_KYBER_1024_R3_CIPHERTEXT_BYTES,
    .generate_keypair = &s2n_evp_kem_generate_keypair,
    .encapsulate = &s2n_evp_kem_encapsulate,
    .decapsulate = &s2n_evp_kem_decapsulate,
};

const struct s2n_kem *tls12_kyber_kems[] = {
    &s2n_kyber_512_r3,
};

const struct s2n_iana_to_kem kem_mapping[1] = {
    {
            .iana_value = { TLS_ECDHE_KYBER_RSA_WITH_AES_256_GCM_SHA384 },
            .kems = tls12_kyber_kems,
            .kem_count = s2n_array_len(tls12_kyber_kems),
    },
};

/* Specific assignments of KEM group IDs and names have not yet been
 * published in an RFC (or draft). There is consensus in the
 * community to use values in the proposed reserved range defined in
 * https://tools.ietf.org/html/draft-stebila-tls-hybrid-design.
 * Values for interoperability are defined in
 * https://github.com/open-quantum-safe/oqs-provider/blob/main/oqs-template/oqs-kem-info.md
 * and
 * https://www.iana.org/assignments/tls-parameters/tls-parameters.xhtml
 */

/*
 * ML-KEM based hybrid KEMs as specified by IETF and registered in IANA.
 *
 * https://www.iana.org/assignments/tls-parameters/tls-parameters.xhtml#tls-parameters-8
 * https://datatracker.ietf.org/doc/draft-kwiatkowski-tls-ecdhe-mlkem/
 */
const struct s2n_kem_group s2n_secp256r1_mlkem_768 = {
    .name = "SecP256r1MLKEM768",
    .iana_id = TLS_PQ_KEM_GROUP_ID_SECP256R1_MLKEM_768,
    .curve = &s2n_ecc_curve_secp256r1,
    .kem = &s2n_mlkem_768,
    .send_kem_first = 0,
};

const struct s2n_kem_group s2n_x25519_mlkem_768 = {
    .name = "X25519MLKEM768",
    .iana_id = TLS_PQ_KEM_GROUP_ID_X25519_MLKEM_768,
    .curve = &s2n_ecc_curve_x25519,
    .kem = &s2n_mlkem_768,
    /* ML-KEM KeyShare should always be sent first for X25519MLKEM768.
     * https://datatracker.ietf.org/doc/html/draft-kwiatkowski-tls-ecdhe-mlkem-02#name-negotiated-groups */
    .send_kem_first = 1,
};

const struct s2n_kem_group s2n_secp256r1_kyber_512_r3 = {
    .name = "secp256r1_kyber-512-r3",
    .iana_id = TLS_PQ_KEM_GROUP_ID_SECP256R1_KYBER_512_R3,
    .curve = &s2n_ecc_curve_secp256r1,
    .kem = &s2n_kyber_512_r3,
    .send_kem_first = 0,
};

const struct s2n_kem_group s2n_secp256r1_kyber_768_r3 = {
    .name = "SecP256r1Kyber768Draft00",
    .iana_id = TLS_PQ_KEM_GROUP_ID_SECP256R1_KYBER_768_R3,
    .curve = &s2n_ecc_curve_secp256r1,
    .kem = &s2n_kyber_768_r3,
    .send_kem_first = 0,
};

const struct s2n_kem_group s2n_secp384r1_kyber_768_r3 = {
    .name = "secp384r1_kyber-768-r3",
    .iana_id = TLS_PQ_KEM_GROUP_ID_SECP384R1_KYBER_768_R3,
    .curve = &s2n_ecc_curve_secp384r1,
    .kem = &s2n_kyber_768_r3,
    .send_kem_first = 0,
};

const struct s2n_kem_group s2n_secp521r1_kyber_1024_r3 = {
    .name = "secp521r1_kyber-1024-r3",
    .iana_id = TLS_PQ_KEM_GROUP_ID_SECP521R1_KYBER_1024_R3,
    .curve = &s2n_ecc_curve_secp521r1,
    .kem = &s2n_kyber_1024_r3,
    .send_kem_first = 0,
};

const struct s2n_kem_group s2n_x25519_kyber_512_r3 = {
    .name = "x25519_kyber-512-r3",
    .iana_id = TLS_PQ_KEM_GROUP_ID_X25519_KYBER_512_R3,
    .curve = &s2n_ecc_curve_x25519,
    .kem = &s2n_kyber_512_r3,
    .send_kem_first = 0,
};

const struct s2n_kem_group s2n_x25519_kyber_768_r3 = {
    .name = "X25519Kyber768Draft00",
    .iana_id = TLS_PQ_KEM_GROUP_ID_X25519_KYBER_768_R3,
    .curve = &s2n_ecc_curve_x25519,
    .kem = &s2n_kyber_768_r3,
    .send_kem_first = 0,
};

const struct s2n_kem_group *ALL_SUPPORTED_KEM_GROUPS[] = {
    &s2n_x25519_mlkem_768,
    &s2n_secp256r1_mlkem_768,
    &s2n_secp256r1_kyber_768_r3,
    &s2n_x25519_kyber_768_r3,
    &s2n_secp384r1_kyber_768_r3,
    &s2n_secp521r1_kyber_1024_r3,
    &s2n_secp256r1_kyber_512_r3,
    &s2n_x25519_kyber_512_r3,
};

/* Helper safety macro to call the NIST PQ KEM functions. The NIST
 * functions may return any non-zero value to indicate failure. */
#define GUARD_PQ_AS_RESULT(x) RESULT_ENSURE((x) == 0, S2N_ERR_PQ_CRYPTO)

S2N_RESULT s2n_kem_generate_keypair(struct s2n_kem_params *kem_params)
{
    RESULT_ENSURE_REF(kem_params);
    RESULT_ENSURE_REF(kem_params->kem);
    const struct s2n_kem *kem = kem_params->kem;
    RESULT_ENSURE_REF(kem->generate_keypair);

    RESULT_ENSURE_REF(kem_params->public_key.data);
    RESULT_ENSURE(kem_params->public_key.size == kem->public_key_length, S2N_ERR_SAFETY);

    /* Need to save the private key for decapsulation */
    RESULT_GUARD_POSIX(s2n_realloc(&kem_params->private_key, kem->private_key_length));

    GUARD_PQ_AS_RESULT(kem->generate_keypair(kem, kem_params->public_key.data, kem_params->private_key.data));
    return S2N_RESULT_OK;
}

S2N_RESULT s2n_kem_encapsulate(struct s2n_kem_params *kem_params, struct s2n_blob *ciphertext)
{
    RESULT_ENSURE_REF(kem_params);
    RESULT_ENSURE_REF(kem_params->kem);
    const struct s2n_kem *kem = kem_params->kem;
    RESULT_ENSURE_REF(kem->encapsulate);

    RESULT_ENSURE(kem_params->public_key.size == kem->public_key_length, S2N_ERR_SAFETY);
    RESULT_ENSURE_REF(kem_params->public_key.data);

    RESULT_ENSURE_REF(ciphertext);
    RESULT_ENSURE_REF(ciphertext->data);
    RESULT_ENSURE(ciphertext->size == kem->ciphertext_length, S2N_ERR_SAFETY);

    /* Need to save the shared secret for key derivation */
    RESULT_GUARD_POSIX(s2n_alloc(&(kem_params->shared_secret), kem->shared_secret_key_length));

    GUARD_PQ_AS_RESULT(kem->encapsulate(kem, ciphertext->data, kem_params->shared_secret.data, kem_params->public_key.data));
    return S2N_RESULT_OK;
}

S2N_RESULT s2n_kem_decapsulate(struct s2n_kem_params *kem_params, const struct s2n_blob *ciphertext)
{
    RESULT_ENSURE_REF(kem_params);
    RESULT_ENSURE_REF(kem_params->kem);
    const struct s2n_kem *kem = kem_params->kem;
    RESULT_ENSURE_REF(kem->decapsulate);

    RESULT_ENSURE(kem_params->private_key.size == kem->private_key_length, S2N_ERR_SAFETY);
    RESULT_ENSURE_REF(kem_params->private_key.data);

    RESULT_ENSURE_REF(ciphertext);
    RESULT_ENSURE_REF(ciphertext->data);
    RESULT_ENSURE(ciphertext->size == kem->ciphertext_length, S2N_ERR_SAFETY);

    /* Need to save the shared secret for key derivation */
    RESULT_GUARD_POSIX(s2n_alloc(&(kem_params->shared_secret), kem->shared_secret_key_length));

    GUARD_PQ_AS_RESULT(kem->decapsulate(kem, kem_params->shared_secret.data, ciphertext->data, kem_params->private_key.data));
    return S2N_RESULT_OK;
}

static int s2n_kem_check_kem_compatibility(const uint8_t iana_value[S2N_TLS_CIPHER_SUITE_LEN], const struct s2n_kem *candidate_kem,
        uint8_t *kem_is_compatible)
{
    const struct s2n_iana_to_kem *compatible_kems = NULL;
    POSIX_GUARD(s2n_cipher_suite_to_kem(iana_value, &compatible_kems));

    for (uint8_t i = 0; i < compatible_kems->kem_count; i++) {
        if (candidate_kem->kem_extension_id == compatible_kems->kems[i]->kem_extension_id) {
            *kem_is_compatible = 1;
            return S2N_SUCCESS;
        }
    }

    *kem_is_compatible = 0;
    return S2N_SUCCESS;
}

int s2n_choose_kem_with_peer_pref_list(const uint8_t iana_value[S2N_TLS_CIPHER_SUITE_LEN], struct s2n_blob *client_kem_ids,
        const struct s2n_kem *server_kem_pref_list[], const uint8_t num_server_supported_kems, const struct s2n_kem **chosen_kem)
{
    struct s2n_stuffer client_kem_ids_stuffer = { 0 };
    POSIX_GUARD(s2n_stuffer_init(&client_kem_ids_stuffer, client_kem_ids));
    POSIX_GUARD(s2n_stuffer_write(&client_kem_ids_stuffer, client_kem_ids));

    /* Each KEM ID is 2 bytes */
    uint8_t num_client_candidate_kems = client_kem_ids->size / 2;

    for (uint8_t i = 0; i < num_server_supported_kems; i++) {
        const struct s2n_kem *candidate_server_kem = (server_kem_pref_list[i]);

        uint8_t server_kem_is_compatible = 0;
        POSIX_GUARD(s2n_kem_check_kem_compatibility(iana_value, candidate_server_kem, &server_kem_is_compatible));

        if (!server_kem_is_compatible) {
            continue;
        }

        for (uint8_t j = 0; j < num_client_candidate_kems; j++) {
            kem_extension_size candidate_client_kem_id = 0;
            POSIX_GUARD(s2n_stuffer_read_uint16(&client_kem_ids_stuffer, &candidate_client_kem_id));

            if (candidate_server_kem->kem_extension_id == candidate_client_kem_id) {
                *chosen_kem = candidate_server_kem;
                return S2N_SUCCESS;
            }
        }
        POSIX_GUARD(s2n_stuffer_reread(&client_kem_ids_stuffer));
    }

    /* Client and server did not propose any mutually supported KEMs compatible with the ciphersuite */
    POSIX_BAIL(S2N_ERR_KEM_UNSUPPORTED_PARAMS);
}

int s2n_choose_kem_without_peer_pref_list(const uint8_t iana_value[S2N_TLS_CIPHER_SUITE_LEN], const struct s2n_kem *server_kem_pref_list[],
        const uint8_t num_server_supported_kems, const struct s2n_kem **chosen_kem)
{
    for (uint8_t i = 0; i < num_server_supported_kems; i++) {
        uint8_t kem_is_compatible = 0;
        POSIX_GUARD(s2n_kem_check_kem_compatibility(iana_value, server_kem_pref_list[i], &kem_is_compatible));
        if (kem_is_compatible) {
            *chosen_kem = server_kem_pref_list[i];
            return S2N_SUCCESS;
        }
    }

    /* The server preference list did not contain any KEM extensions compatible with the ciphersuite */
    POSIX_BAIL(S2N_ERR_KEM_UNSUPPORTED_PARAMS);
}

int s2n_kem_free(struct s2n_kem_params *kem_params)
{
    if (kem_params != NULL) {
        POSIX_GUARD(s2n_free_or_wipe(&kem_params->private_key));
        POSIX_GUARD(s2n_free_or_wipe(&kem_params->public_key));
        POSIX_GUARD(s2n_free_or_wipe(&kem_params->shared_secret));
    }
    return S2N_SUCCESS;
}

int s2n_kem_group_free(struct s2n_kem_group_params *kem_group_params)
{
    if (kem_group_params != NULL) {
        POSIX_GUARD(s2n_kem_free(&kem_group_params->kem_params));
        POSIX_GUARD(s2n_ecc_evp_params_free(&kem_group_params->ecc_params));
    }
    return S2N_SUCCESS;
}

int s2n_cipher_suite_to_kem(const uint8_t iana_value[S2N_TLS_CIPHER_SUITE_LEN], const struct s2n_iana_to_kem **compatible_params)
{
    for (size_t i = 0; i < s2n_array_len(kem_mapping); i++) {
        const struct s2n_iana_to_kem *candidate = &kem_mapping[i];
        if (s2n_constant_time_equals(iana_value, candidate->iana_value, S2N_TLS_CIPHER_SUITE_LEN)) {
            *compatible_params = candidate;
            return S2N_SUCCESS;
        }
    }
    POSIX_BAIL(S2N_ERR_KEM_UNSUPPORTED_PARAMS);
}

int s2n_get_kem_from_extension_id(kem_extension_size kem_id, const struct s2n_kem **kem)
{
    for (size_t i = 0; i < s2n_array_len(kem_mapping); i++) {
        const struct s2n_iana_to_kem *iana_to_kem = &kem_mapping[i];

        for (int j = 0; j < iana_to_kem->kem_count; j++) {
            const struct s2n_kem *candidate_kem = iana_to_kem->kems[j];
            if (candidate_kem->kem_extension_id == kem_id) {
                *kem = candidate_kem;
                return S2N_SUCCESS;
            }
        }
    }

    POSIX_BAIL(S2N_ERR_KEM_UNSUPPORTED_PARAMS);
}

int s2n_kem_send_public_key(struct s2n_stuffer *out, struct s2n_kem_params *kem_params)
{
    POSIX_ENSURE_REF(out);
    POSIX_ENSURE_REF(kem_params);
    POSIX_ENSURE_REF(kem_params->kem);

    const struct s2n_kem *kem = kem_params->kem;

    if (kem_params->len_prefixed) {
        POSIX_GUARD(s2n_stuffer_write_uint16(out, kem->public_key_length));
    }

    /* We don't need to store the public key after sending it.
     * We write it directly to *out. */
    kem_params->public_key.data = s2n_stuffer_raw_write(out, kem->public_key_length);
    POSIX_ENSURE_REF(kem_params->public_key.data);
    kem_params->public_key.size = kem->public_key_length;

    /* Saves the private key in kem_params */
    POSIX_GUARD_RESULT(s2n_kem_generate_keypair(kem_params));

    /* After using s2n_stuffer_raw_write() above to write the public
     * key to the stuffer, we want to ensure that kem_params->public_key.data
     * does not continue to point at *out, else we may unexpectedly
     * overwrite part of the stuffer when s2n_kem_free() is called. */
    kem_params->public_key.data = NULL;
    kem_params->public_key.size = 0;

    return S2N_SUCCESS;
}

int s2n_kem_recv_public_key(struct s2n_stuffer *in, struct s2n_kem_params *kem_params)
{
    POSIX_ENSURE_REF(in);
    POSIX_ENSURE_REF(kem_params);
    POSIX_ENSURE_REF(kem_params->kem);

    const struct s2n_kem *kem = kem_params->kem;

    if (kem_params->len_prefixed) {
        kem_public_key_size public_key_length = 0;
        POSIX_GUARD(s2n_stuffer_read_uint16(in, &public_key_length));
        POSIX_ENSURE(public_key_length == kem->public_key_length, S2N_ERR_BAD_MESSAGE);
    }

    /* Alloc memory for the public key; the peer receiving it will need it
     * later during the handshake to encapsulate the shared secret. */
    POSIX_GUARD(s2n_alloc(&(kem_params->public_key), kem->public_key_length));
    POSIX_GUARD(s2n_stuffer_read_bytes(in, kem_params->public_key.data, kem->public_key_length));

    return S2N_SUCCESS;
}

int s2n_kem_send_ciphertext(struct s2n_stuffer *out, struct s2n_kem_params *kem_params)
{
    POSIX_ENSURE_REF(out);
    POSIX_ENSURE_REF(kem_params);
    POSIX_ENSURE_REF(kem_params->kem);
    POSIX_ENSURE_REF(kem_params->public_key.data);

    const struct s2n_kem *kem = kem_params->kem;

    if (kem_params->len_prefixed) {
        POSIX_GUARD(s2n_stuffer_write_uint16(out, kem->ciphertext_length));
    }

    /* Ciphertext will get written to *out */
    struct s2n_blob ciphertext = { 0 };
    POSIX_GUARD(s2n_blob_init(&ciphertext, s2n_stuffer_raw_write(out, kem->ciphertext_length), kem->ciphertext_length));
    POSIX_ENSURE_REF(ciphertext.data);

    /* Saves the shared secret in kem_params */
    POSIX_GUARD_RESULT(s2n_kem_encapsulate(kem_params, &ciphertext));

    return S2N_SUCCESS;
}

int s2n_kem_recv_ciphertext(struct s2n_stuffer *in, struct s2n_kem_params *kem_params)
{
    POSIX_ENSURE_REF(in);
    POSIX_ENSURE_REF(kem_params);
    POSIX_ENSURE_REF(kem_params->kem);
    POSIX_ENSURE_REF(kem_params->private_key.data);

    const struct s2n_kem *kem = kem_params->kem;

    if (kem_params->len_prefixed) {
        kem_ciphertext_key_size ciphertext_length = 0;
        POSIX_GUARD(s2n_stuffer_read_uint16(in, &ciphertext_length));
        POSIX_ENSURE(ciphertext_length == kem->ciphertext_length, S2N_ERR_BAD_MESSAGE);
    }

    const struct s2n_blob ciphertext = { .data = s2n_stuffer_raw_read(in, kem->ciphertext_length), .size = kem->ciphertext_length };
    POSIX_ENSURE_REF(ciphertext.data);

    /* Saves the shared secret in kem_params */
    POSIX_GUARD_RESULT(s2n_kem_decapsulate(kem_params, &ciphertext));

    return S2N_SUCCESS;
}

bool s2n_kem_is_available(const struct s2n_kem *kem)
{
    if (kem == NULL || kem->kem_nid == NID_undef) {
        return false;
    }

    bool available = s2n_libcrypto_supports_evp_kem();

    /* Only newer versions of libcrypto have ML-KEM support. */
    if (kem == &s2n_mlkem_768) {
        available &= s2n_libcrypto_supports_mlkem();
    }

    return available;
}

bool s2n_kem_group_is_available(const struct s2n_kem_group *kem_group)
{
    /* Check for values that might be undefined when compiling for older libcrypto's */
    if (kem_group == NULL || kem_group->curve == NULL || kem_group->kem == NULL) {
        return false;
    }

    bool available = s2n_kem_is_available(kem_group->kem);

    /* x25519 based tls13_kem_groups require EVP_APIS_SUPPORTED */
    if (kem_group->curve == &s2n_ecc_curve_x25519) {
        available &= s2n_is_evp_apis_supported();
    }

    return available;
}

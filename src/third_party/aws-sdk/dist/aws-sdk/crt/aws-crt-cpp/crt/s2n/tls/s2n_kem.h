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

#pragma once

#include <stdint.h>

#include "crypto/s2n_ecc_evp.h"
#include "stuffer/s2n_stuffer.h"
#include "tls/s2n_crypto_constants.h"
#include "utils/s2n_blob.h"

typedef uint16_t kem_extension_size;
typedef uint16_t kem_public_key_size;
typedef uint16_t kem_private_key_size;
typedef uint16_t kem_shared_secret_size;
typedef uint16_t kem_ciphertext_key_size;

#define IN  /* Indicates a necessary function input */
#define OUT /* Indicates a function output */

#if defined(S2N_LIBCRYPTO_SUPPORTS_EVP_KEM)
    #define S2N_NID_KYBER512  NID_KYBER512_R3
    #define S2N_NID_KYBER768  NID_KYBER768_R3
    #define S2N_NID_KYBER1024 NID_KYBER1024_R3
#else
    #define S2N_NID_KYBER512  NID_undef
    #define S2N_NID_KYBER768  NID_undef
    #define S2N_NID_KYBER1024 NID_undef
#endif

#if defined(S2N_LIBCRYPTO_SUPPORTS_MLKEM)
    #define S2N_NID_MLKEM768 NID_MLKEM768
#else
    #define S2N_NID_MLKEM768 NID_undef
#endif

struct s2n_kem {
    const char *name;
    int kem_nid;
    const kem_extension_size kem_extension_id;
    const kem_public_key_size public_key_length;
    const kem_private_key_size private_key_length;
    const kem_shared_secret_size shared_secret_key_length;
    const kem_ciphertext_key_size ciphertext_length;
    /* NIST Post Quantum KEM submissions require the following API for compatibility */
    int (*generate_keypair)(IN const struct s2n_kem *kem, OUT uint8_t *public_key, OUT uint8_t *private_key);
    int (*encapsulate)(IN const struct s2n_kem *kem, OUT uint8_t *ciphertext, OUT uint8_t *shared_secret, IN const uint8_t *public_key);
    int (*decapsulate)(IN const struct s2n_kem *kem, OUT uint8_t *shared_secret, IN const uint8_t *ciphertext, IN const uint8_t *private_key);
};

struct s2n_kem_params {
    const struct s2n_kem *kem;
    struct s2n_blob public_key;
    struct s2n_blob private_key;
    struct s2n_blob shared_secret;
    /* Store whether the client included the length prefix of the PQ and ECC Shares in their ClientHello, so that the
     * server can match the client's behavior. For the client side, store whether it should send the length prefix. */
    bool len_prefixed;
};

struct s2n_iana_to_kem {
    const uint8_t iana_value[S2N_TLS_CIPHER_SUITE_LEN];
    const struct s2n_kem **kems;
    uint8_t kem_count;
};

struct s2n_kem_group {
    const char *name;
    uint16_t iana_id;
    const struct s2n_ecc_named_curve *curve;
    const struct s2n_kem *kem;

    /* Whether the PQ KeyShare should be sent before the ECC KeyShare. Only enabled for X25519MLKEM768.
     * See: https://datatracker.ietf.org/doc/html/draft-kwiatkowski-tls-ecdhe-mlkem-02#name-negotiated-groups */
    bool send_kem_first;
};

struct s2n_kem_group_params {
    const struct s2n_kem_group *kem_group;
    struct s2n_kem_params kem_params;
    struct s2n_ecc_evp_params ecc_params;
};

extern const struct s2n_kem s2n_mlkem_768;
extern const struct s2n_kem s2n_kyber_512_r3;
extern const struct s2n_kem s2n_kyber_768_r3;
extern const struct s2n_kem s2n_kyber_1024_r3;

#define S2N_KEM_GROUPS_COUNT 8
extern const struct s2n_kem_group *ALL_SUPPORTED_KEM_GROUPS[S2N_KEM_GROUPS_COUNT];

/* NIST curve KEM Groups */
extern const struct s2n_kem_group s2n_secp256r1_mlkem_768;
extern const struct s2n_kem_group s2n_secp256r1_kyber_512_r3;
extern const struct s2n_kem_group s2n_secp256r1_kyber_768_r3;
extern const struct s2n_kem_group s2n_secp384r1_kyber_768_r3;
extern const struct s2n_kem_group s2n_secp521r1_kyber_1024_r3;

/* x25519 KEM Groups */
extern const struct s2n_kem_group s2n_x25519_mlkem_768;
extern const struct s2n_kem_group s2n_x25519_kyber_512_r3;
extern const struct s2n_kem_group s2n_x25519_kyber_768_r3;

S2N_RESULT s2n_kem_generate_keypair(struct s2n_kem_params *kem_params);
S2N_RESULT s2n_kem_encapsulate(struct s2n_kem_params *kem_params, struct s2n_blob *ciphertext);
S2N_RESULT s2n_kem_decapsulate(struct s2n_kem_params *kem_params, const struct s2n_blob *ciphertext);
int s2n_choose_kem_with_peer_pref_list(const uint8_t iana_value[S2N_TLS_CIPHER_SUITE_LEN],
        struct s2n_blob *client_kem_ids, const struct s2n_kem *server_kem_pref_list[],
        const uint8_t num_server_supported_kems, const struct s2n_kem **chosen_kem);
int s2n_choose_kem_without_peer_pref_list(const uint8_t iana_value[S2N_TLS_CIPHER_SUITE_LEN],
        const struct s2n_kem *server_kem_pref_list[], const uint8_t num_server_supported_kems,
        const struct s2n_kem **chosen_kem);
int s2n_kem_free(struct s2n_kem_params *kem_params);
int s2n_kem_group_free(struct s2n_kem_group_params *kem_group_params);
int s2n_cipher_suite_to_kem(const uint8_t iana_value[S2N_TLS_CIPHER_SUITE_LEN],
        const struct s2n_iana_to_kem **supported_params);
int s2n_get_kem_from_extension_id(kem_extension_size kem_id, const struct s2n_kem **kem);
int s2n_kem_send_public_key(struct s2n_stuffer *out, struct s2n_kem_params *kem_params);
int s2n_kem_recv_public_key(struct s2n_stuffer *in, struct s2n_kem_params *kem_params);
int s2n_kem_send_ciphertext(struct s2n_stuffer *out, struct s2n_kem_params *kem_params);
int s2n_kem_recv_ciphertext(struct s2n_stuffer *in, struct s2n_kem_params *kem_params);
bool s2n_kem_is_available(const struct s2n_kem *kem);
bool s2n_kem_group_is_available(const struct s2n_kem_group *kem_group);

/* mlkem768 */
#define S2N_MLKEM_768_PUBLIC_KEY_BYTES    1184
#define S2N_MLKEM_768_SECRET_KEY_BYTES    2400
#define S2N_MLKEM_768_CIPHERTEXT_BYTES    1088
#define S2N_MLKEM_768_SHARED_SECRET_BYTES 32

/* kyber512r3 */
#define S2N_KYBER_512_R3_PUBLIC_KEY_BYTES    800
#define S2N_KYBER_512_R3_SECRET_KEY_BYTES    1632
#define S2N_KYBER_512_R3_CIPHERTEXT_BYTES    768
#define S2N_KYBER_512_R3_SHARED_SECRET_BYTES 32

/* kyber768r3 */
#define S2N_KYBER_768_R3_PUBLIC_KEY_BYTES    1184
#define S2N_KYBER_768_R3_SECRET_KEY_BYTES    2400
#define S2N_KYBER_768_R3_CIPHERTEXT_BYTES    1088
#define S2N_KYBER_768_R3_SHARED_SECRET_BYTES 32

/* kyber1024r3 */
#define S2N_KYBER_1024_R3_PUBLIC_KEY_BYTES    1568
#define S2N_KYBER_1024_R3_SECRET_KEY_BYTES    3168
#define S2N_KYBER_1024_R3_CIPHERTEXT_BYTES    1568
#define S2N_KYBER_1024_R3_SHARED_SECRET_BYTES 32

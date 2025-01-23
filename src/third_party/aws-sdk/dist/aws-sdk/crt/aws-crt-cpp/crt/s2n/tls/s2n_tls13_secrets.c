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

#include "tls/s2n_tls13_secrets.h"

#include "tls/s2n_connection.h"
#include "tls/s2n_key_log.h"
#include "tls/s2n_tls13_handshake.h"
#include "utils/s2n_bitmap.h"

#define S2N_MAX_HASHLEN SHA384_DIGEST_LENGTH

#define CONN_HMAC_ALG(conn) ((conn)->secure->cipher_suite->prf_alg)
#define CONN_SECRETS(conn)  ((conn)->secrets.version.tls13)
#define CONN_HASHES(conn)   ((conn)->handshake.hashes)

#define CONN_SECRET(conn, secret) ( \
        (struct s2n_blob){ .data = CONN_SECRETS(conn).secret, .size = s2n_get_hash_len(CONN_HMAC_ALG(conn)) })
#define CONN_HASH(conn, hash) ( \
        (struct s2n_blob){ .data = CONN_HASHES(conn)->hash, .size = s2n_get_hash_len(CONN_HMAC_ALG(conn)) })
#define CONN_FINISHED(conn, mode) ( \
        (struct s2n_blob){ .data = (conn)->handshake.mode##_finished, .size = s2n_get_hash_len(CONN_HMAC_ALG(conn)) })

/**
 *= https://www.rfc-editor.org/rfc/rfc8446#section-7.1
 *# If a given secret is not available, then the 0-value consisting of a
 *# string of Hash.length bytes set to zeros is used.
 */
static uint8_t zero_value_bytes[S2N_MAX_HASHLEN] = { 0 };
#define ZERO_VALUE(hmac_alg) ( \
        (const struct s2n_blob){ .data = zero_value_bytes, .size = s2n_get_hash_len(hmac_alg) })

/**
 * When an operation doesn't need an actual transcript hash,
 * it uses an empty transcript hash as an input instead.
 *
 *= https://www.rfc-editor.org/rfc/rfc8446#section-7.1
 *# Note that in some cases a zero-
 *# length Context (indicated by "") is passed to HKDF-Expand-Label
 */
#define EMPTY_CONTEXT(hmac_alg) ( \
        (const struct s2n_blob){ .data = s2n_get_empty_context(hmac_alg), .size = s2n_get_hash_len(hmac_alg) })

static uint8_t s2n_get_hash_len(s2n_hmac_algorithm hmac_alg)
{
    uint8_t hash_size = 0;
    if (s2n_hmac_digest_size(hmac_alg, &hash_size) != S2N_SUCCESS) {
        return 0;
    }
    return hash_size;
}

static uint8_t *s2n_get_empty_context(s2n_hmac_algorithm hmac_alg)
{
    static uint8_t sha256_empty_digest[S2N_MAX_HASHLEN] = { 0 };
    static uint8_t sha384_empty_digest[S2N_MAX_HASHLEN] = { 0 };

    switch (hmac_alg) {
        case S2N_HMAC_SHA256:
            return sha256_empty_digest;
        case S2N_HMAC_SHA384:
            return sha384_empty_digest;
        default:
            return NULL;
    }
}

static s2n_hmac_algorithm supported_hmacs[] = {
    S2N_HMAC_SHA256,
    S2N_HMAC_SHA384
};

S2N_RESULT s2n_tls13_empty_transcripts_init()
{
    DEFER_CLEANUP(struct s2n_hash_state hash = { 0 }, s2n_hash_free);
    RESULT_GUARD_POSIX(s2n_hash_new(&hash));

    s2n_hash_algorithm hash_alg = S2N_HASH_NONE;
    for (size_t i = 0; i < s2n_array_len(supported_hmacs); i++) {
        s2n_hmac_algorithm hmac_alg = supported_hmacs[i];
        struct s2n_blob digest = EMPTY_CONTEXT(hmac_alg);

        RESULT_GUARD_POSIX(s2n_hmac_hash_alg(hmac_alg, &hash_alg));
        RESULT_GUARD_POSIX(s2n_hash_init(&hash, hash_alg));
        RESULT_GUARD_POSIX(s2n_hash_digest(&hash, digest.data, digest.size));
    }

    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_calculate_transcript_digest(struct s2n_connection *conn)
{
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE_REF(conn->handshake.hashes);

    s2n_hash_algorithm hash_algorithm = S2N_HASH_NONE;
    RESULT_GUARD_POSIX(s2n_hmac_hash_alg(CONN_HMAC_ALG(conn), &hash_algorithm));

    uint8_t digest_size = 0;
    RESULT_GUARD_POSIX(s2n_hash_digest_size(hash_algorithm, &digest_size));

    struct s2n_blob digest = { 0 };
    RESULT_GUARD_POSIX(s2n_blob_init(&digest, CONN_HASHES(conn)->transcript_hash_digest, digest_size));

    struct s2n_hash_state *hash_state = &conn->handshake.hashes->hash_workspace;
    RESULT_GUARD(s2n_handshake_copy_hash_state(conn, hash_algorithm, hash_state));
    RESULT_GUARD_POSIX(s2n_hash_digest(hash_state, digest.data, digest.size));

    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_extract_secret(s2n_hmac_algorithm hmac_alg,
        const struct s2n_blob *previous_secret_material, const struct s2n_blob *new_secret_material,
        struct s2n_blob *output)
{
    /*
     * TODO: We should be able to reuse the prf_work_space rather
     * than allocating a new HMAC every time.
     * https://github.com/aws/s2n-tls/issues/3206
     */
    DEFER_CLEANUP(struct s2n_hmac_state hmac_state = { 0 }, s2n_hmac_free);
    RESULT_GUARD_POSIX(s2n_hmac_new(&hmac_state));

    RESULT_GUARD_POSIX(s2n_hkdf_extract(&hmac_state, hmac_alg,
            previous_secret_material, new_secret_material, output));
    return S2N_RESULT_OK;
}

/**
 *= https://www.rfc-editor.org/rfc/rfc8446#section-7.1
 *# Derive-Secret(Secret, Label, Messages) =
 *#      HKDF-Expand-Label(Secret, Label,
 *#                        Transcript-Hash(Messages), Hash.length)
 */
static S2N_RESULT s2n_derive_secret(s2n_hmac_algorithm hmac_alg,
        const struct s2n_blob *previous_secret_material, const struct s2n_blob *label, const struct s2n_blob *context,
        struct s2n_blob *output)
{
    /*
     * TODO: We should be able to reuse the prf_work_space rather
     * than allocating a new HMAC every time.
     * https://github.com/aws/s2n-tls/issues/3206
     */
    DEFER_CLEANUP(struct s2n_hmac_state hmac_state = { 0 }, s2n_hmac_free);
    RESULT_GUARD_POSIX(s2n_hmac_new(&hmac_state));

    output->size = s2n_get_hash_len(hmac_alg);
    RESULT_GUARD_POSIX(s2n_hkdf_expand_label(&hmac_state, hmac_alg,
            previous_secret_material, label, context, output));
    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_derive_secret_with_context(struct s2n_connection *conn,
        s2n_extract_secret_type_t input_secret_type, const struct s2n_blob *label, message_type_t transcript_end_msg,
        struct s2n_blob *output)
{
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE_REF(label);
    RESULT_ENSURE_REF(output);

    RESULT_ENSURE(conn->secrets.extract_secret_type == input_secret_type, S2N_ERR_SECRET_SCHEDULE_STATE);
    RESULT_ENSURE(s2n_conn_get_current_message_type(conn) == transcript_end_msg, S2N_ERR_SECRET_SCHEDULE_STATE);
    RESULT_GUARD(s2n_derive_secret(CONN_HMAC_ALG(conn), &CONN_SECRET(conn, extract_secret),
            label, &CONN_HASH(conn, transcript_hash_digest), output));
    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_derive_secret_without_context(struct s2n_connection *conn,
        s2n_extract_secret_type_t input_secret_type, struct s2n_blob *output)
{
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE_REF(output);

    RESULT_ENSURE(conn->secrets.extract_secret_type == input_secret_type, S2N_ERR_SECRET_SCHEDULE_STATE);
    RESULT_GUARD(s2n_derive_secret(CONN_HMAC_ALG(conn), &CONN_SECRET(conn, extract_secret),
            &s2n_tls13_label_derived_secret, &EMPTY_CONTEXT(CONN_HMAC_ALG(conn)), output));
    return S2N_RESULT_OK;
}

/**
 *= https://www.rfc-editor.org/rfc/rfc8446#section-4.4.4
 *# The key used to compute the Finished message is computed from the
 *# Base Key defined in Section 4.4 using HKDF (see Section 7.1).
 *# Specifically:
 *#
 *# finished_key =
 *#     HKDF-Expand-Label(BaseKey, "finished", "", Hash.length)
 **/
static S2N_RESULT s2n_tls13_compute_finished_key(struct s2n_connection *conn,
        const struct s2n_blob *base_key, struct s2n_blob *output)
{
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE_REF(base_key);
    RESULT_ENSURE_REF(output);

    RESULT_GUARD(s2n_handshake_set_finished_len(conn, output->size));

    /*
     * TODO: We should be able to reuse the prf_work_space rather
     * than allocating a new HMAC every time.
     */
    DEFER_CLEANUP(struct s2n_hmac_state hmac_state = { 0 }, s2n_hmac_free);
    RESULT_GUARD_POSIX(s2n_hmac_new(&hmac_state));

    RESULT_GUARD_POSIX(s2n_hkdf_expand_label(&hmac_state, CONN_HMAC_ALG(conn),
            base_key, &s2n_tls13_label_finished, &(struct s2n_blob){ 0 }, output));
    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_call_secret_callbacks(struct s2n_connection *conn,
        const struct s2n_blob *secret, s2n_secret_type_t secret_type)
{
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE_REF(secret);

    if (conn->secret_cb && (s2n_connection_is_quic_enabled(conn) || s2n_in_unit_test())) {
        RESULT_GUARD_POSIX(conn->secret_cb(conn->secret_cb_context, conn, secret_type,
                secret->data, secret->size));
    }
    s2n_result_ignore(s2n_key_log_tls13_secret(conn, secret, secret_type));
    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_trigger_secret_callbacks(struct s2n_connection *conn,
        const struct s2n_blob *secret, s2n_extract_secret_type_t secret_type, s2n_mode mode)
{
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE_REF(secret);

    static const s2n_secret_type_t conversions[][2] = {
        [S2N_EARLY_SECRET] = { S2N_CLIENT_EARLY_TRAFFIC_SECRET, S2N_CLIENT_EARLY_TRAFFIC_SECRET },
        [S2N_HANDSHAKE_SECRET] = { S2N_SERVER_HANDSHAKE_TRAFFIC_SECRET, S2N_CLIENT_HANDSHAKE_TRAFFIC_SECRET },
        [S2N_MASTER_SECRET] = { S2N_SERVER_APPLICATION_TRAFFIC_SECRET, S2N_CLIENT_APPLICATION_TRAFFIC_SECRET },
    };
    s2n_secret_type_t callback_secret_type = conversions[secret_type][mode];

    RESULT_GUARD(s2n_call_secret_callbacks(conn, secret, callback_secret_type));
    return S2N_RESULT_OK;
}

/**
 *= https://www.rfc-editor.org/rfc/rfc8446#section-7.1
 *#           0
 *#           |
 *#           v
 *# PSK ->  HKDF-Extract = Early Secret
 *
 *= https://www.rfc-editor.org/rfc/rfc8446#section-7.1
 *# There are multiple potential Early Secret values, depending on which
 *# PSK the server ultimately selects.  The client will need to compute
 *# one for each potential PSK
 */
S2N_RESULT s2n_extract_early_secret(struct s2n_psk *psk)
{
    RESULT_ENSURE_REF(psk);
    RESULT_GUARD_POSIX(s2n_realloc(&psk->early_secret, s2n_get_hash_len(psk->hmac_alg)));
    RESULT_GUARD(s2n_extract_secret(psk->hmac_alg,
            &ZERO_VALUE(psk->hmac_alg),
            &psk->secret,
            &psk->early_secret));
    return S2N_RESULT_OK;
}

/*
 * When we require an early secret to derive other secrets,
 * either retrieve the early secret stored on the chosen / early data PSK
 * or calculate one using a "zero" PSK.
 */
static S2N_RESULT s2n_extract_early_secret_for_schedule(struct s2n_connection *conn)
{
    RESULT_ENSURE_REF(conn);

    struct s2n_psk *psk = conn->psk_params.chosen_psk;
    s2n_hmac_algorithm hmac_alg = CONN_HMAC_ALG(conn);

    /*
     * If the client is sending early data, then the PSK is always assumed
     * to be the first PSK offered.
     */
    if (conn->mode == S2N_CLIENT && conn->early_data_state == S2N_EARLY_DATA_REQUESTED) {
        RESULT_GUARD(s2n_array_get(&conn->psk_params.psk_list, 0, (void **) &psk));
        RESULT_ENSURE_REF(psk);
    }

    /**
     *= https://www.rfc-editor.org/rfc/rfc8446#section-7.1
     *# if no PSK is selected, it will then need
     *# to compute the Early Secret corresponding to the zero PSK.
     */
    if (psk == NULL) {
        RESULT_GUARD(s2n_extract_secret(hmac_alg,
                &ZERO_VALUE(hmac_alg),
                &ZERO_VALUE(hmac_alg),
                &CONN_SECRET(conn, extract_secret)));
        return S2N_RESULT_OK;
    }

    /*
     * The early secret is required to generate or verify a PSK's binder,
     * so must have already been calculated if a valid PSK exists.
     * Use the early secret stored on the PSK.
     */
    RESULT_ENSURE_EQ(hmac_alg, psk->hmac_alg);
    RESULT_CHECKED_MEMCPY(CONN_SECRETS(conn).extract_secret, psk->early_secret.data, psk->early_secret.size);
    return S2N_RESULT_OK;
}

/**
 *= https://www.rfc-editor.org/rfc/rfc8446#section-7.1
 *#           |
 *#           +-----> Derive-Secret(., "ext binder" | "res binder", "")
 *#           |                     = binder_key
 */
S2N_RESULT s2n_derive_binder_key(struct s2n_psk *psk, struct s2n_blob *output)
{
    RESULT_ENSURE_REF(psk);
    RESULT_ENSURE_REF(output);

    const struct s2n_blob *label = &s2n_tls13_label_resumption_psk_binder_key;
    if (psk->type == S2N_PSK_TYPE_EXTERNAL) {
        label = &s2n_tls13_label_external_psk_binder_key;
    }
    RESULT_GUARD(s2n_extract_early_secret(psk));
    RESULT_GUARD(s2n_derive_secret(psk->hmac_alg,
            &psk->early_secret,
            label,
            &EMPTY_CONTEXT(psk->hmac_alg),
            output));
    return S2N_RESULT_OK;
}

/**
 *= https://www.rfc-editor.org/rfc/rfc8446#section-7.1
 *#           |
 *#           +-----> Derive-Secret(., "c e traffic", ClientHello)
 *#           |                     = client_early_traffic_secret
 */
static S2N_RESULT s2n_derive_client_early_traffic_secret(struct s2n_connection *conn, struct s2n_blob *output)
{
    RESULT_GUARD(s2n_derive_secret_with_context(conn,
            S2N_EARLY_SECRET,
            &s2n_tls13_label_client_early_traffic_secret,
            CLIENT_HELLO,
            output));
    return S2N_RESULT_OK;
}

/**
 *= https://www.rfc-editor.org/rfc/rfc8446#section-7.1
 *#           |
 *#           v
 *#     Derive-Secret(., "derived", "")
 *#           |
 *#           v
 *#     (EC)DHE -> HKDF-Extract = Handshake Secret
 */
static S2N_RESULT s2n_extract_handshake_secret(struct s2n_connection *conn)
{
    RESULT_ENSURE_REF(conn);

    struct s2n_blob derived_secret = { 0 };
    uint8_t derived_secret_bytes[S2N_TLS13_SECRET_MAX_LEN] = { 0 };
    RESULT_GUARD_POSIX(s2n_blob_init(&derived_secret, derived_secret_bytes, S2N_TLS13_SECRET_MAX_LEN));
    RESULT_GUARD(s2n_derive_secret_without_context(conn, S2N_EARLY_SECRET, &derived_secret));

    DEFER_CLEANUP(struct s2n_blob shared_secret = { 0 }, s2n_free_or_wipe);
    RESULT_GUARD_POSIX(s2n_tls13_compute_shared_secret(conn, &shared_secret));

    RESULT_GUARD(s2n_extract_secret(CONN_HMAC_ALG(conn),
            &derived_secret,
            &shared_secret,
            &CONN_SECRET(conn, extract_secret)));

    return S2N_RESULT_OK;
}

/**
 *= https://www.rfc-editor.org/rfc/rfc8446#section-7.1
 *#           |
 *#           +-----> Derive-Secret(., "c hs traffic",
 *#           |                     ClientHello...ServerHello)
 *#           |                     = client_handshake_traffic_secret
 */
static S2N_RESULT s2n_derive_client_handshake_traffic_secret(struct s2n_connection *conn, struct s2n_blob *output)
{
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE_REF(output);

    RESULT_GUARD(s2n_derive_secret_with_context(conn,
            S2N_HANDSHAKE_SECRET,
            &s2n_tls13_label_client_handshake_traffic_secret,
            SERVER_HELLO,
            output));

    /*
     * The client finished key needs to be calculated using the
     * same connection state as the client handshake secret.
     *
     *= https://www.rfc-editor.org/rfc/rfc8446#section-4.4.4
     *# The key used to compute the Finished message is computed from the
     *# Base Key defined in Section 4.4 using HKDF (see Section 7.1).
     */
    RESULT_GUARD(s2n_tls13_compute_finished_key(conn,
            output, &CONN_FINISHED(conn, client)));

    return S2N_RESULT_OK;
}

/**
 *= https://www.rfc-editor.org/rfc/rfc8446#section-7.1
 *#           |
 *#           +-----> Derive-Secret(., "s hs traffic",
 *#           |                     ClientHello...ServerHello)
 *#           |                     = server_handshake_traffic_secret
 */
static S2N_RESULT s2n_derive_server_handshake_traffic_secret(struct s2n_connection *conn, struct s2n_blob *output)
{
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE_REF(output);

    RESULT_GUARD(s2n_derive_secret_with_context(conn,
            S2N_HANDSHAKE_SECRET,
            &s2n_tls13_label_server_handshake_traffic_secret,
            SERVER_HELLO,
            output));

    /*
     * The server finished key needs to be calculated using the
     * same connection state as the server handshake secret.
     *
     *= https://www.rfc-editor.org/rfc/rfc8446#section-4.4.4
     *# The key used to compute the Finished message is computed from the
     *# Base Key defined in Section 4.4 using HKDF (see Section 7.1).
     */
    RESULT_GUARD(s2n_tls13_compute_finished_key(conn,
            output, &CONN_FINISHED(conn, server)));

    return S2N_RESULT_OK;
}

/**
 *= https://www.rfc-editor.org/rfc/rfc8446#section-7.1
 *#           v
 *#     Derive-Secret(., "derived", "")
 *#           |
 *#           v
 *# 0 -> HKDF-Extract = Master Secret
 */
static S2N_RESULT s2n_extract_master_secret(struct s2n_connection *conn)
{
    RESULT_ENSURE_REF(conn);

    struct s2n_blob derived_secret = { 0 };
    uint8_t derived_secret_bytes[S2N_TLS13_SECRET_MAX_LEN] = { 0 };
    RESULT_GUARD_POSIX(s2n_blob_init(&derived_secret, derived_secret_bytes, S2N_TLS13_SECRET_MAX_LEN));
    RESULT_GUARD(s2n_derive_secret_without_context(conn, S2N_HANDSHAKE_SECRET, &derived_secret));

    RESULT_GUARD(s2n_extract_secret(CONN_HMAC_ALG(conn),
            &derived_secret,
            &ZERO_VALUE(CONN_HMAC_ALG(conn)),
            &CONN_SECRET(conn, extract_secret)));
    return S2N_RESULT_OK;
}

/**
 *= https://www.rfc-editor.org/rfc/rfc8446#section-7.1
 *#           |
 *#           +-----> Derive-Secret(., "c ap traffic",
 *#           |                     ClientHello...server Finished)
 *#           |                     = client_application_traffic_secret_0
 */
static S2N_RESULT s2n_derive_client_application_traffic_secret(struct s2n_connection *conn, struct s2n_blob *output)
{
    RESULT_GUARD(s2n_derive_secret_with_context(conn,
            S2N_MASTER_SECRET,
            &s2n_tls13_label_client_application_traffic_secret,
            SERVER_FINISHED,
            output));
    return S2N_RESULT_OK;
}

/**
 *= https://www.rfc-editor.org/rfc/rfc8446#section-7.1
 *#           |
 *#           +-----> Derive-Secret(., "s ap traffic",
 *#           |                     ClientHello...server Finished)
 *#           |                     = server_application_traffic_secret_0
 */
static S2N_RESULT s2n_derive_server_application_traffic_secret(struct s2n_connection *conn, struct s2n_blob *output)
{
    RESULT_GUARD(s2n_derive_secret_with_context(conn,
            S2N_MASTER_SECRET,
            &s2n_tls13_label_server_application_traffic_secret,
            SERVER_FINISHED,
            output));
    return S2N_RESULT_OK;
}

/**
 *= https://www.rfc-editor.org/rfc/rfc8446#section-7.1
 *#           |
 *#           +-----> Derive-Secret(., "res master",
 *#                                 ClientHello...client Finished)
 *#                                 = resumption_master_secret
 */
S2N_RESULT s2n_derive_resumption_master_secret(struct s2n_connection *conn)
{
    RESULT_ENSURE_REF(conn);
    /* Secret derivation requires these fields to be non-null.  */
    RESULT_ENSURE_REF(conn->secure);
    RESULT_ENSURE_REF(conn->secure->cipher_suite);

    RESULT_GUARD(s2n_derive_secret_with_context(conn,
            S2N_MASTER_SECRET,
            &s2n_tls13_label_resumption_master_secret,
            CLIENT_FINISHED,
            &CONN_SECRET(conn, resumption_master_secret)));
    return S2N_RESULT_OK;
}

/**
 *= https://www.rfc-editor.org/rfc/rfc8446#section-7.1
 *#           |
 *#           +-----> Derive-Secret(., "exp master",
 *#           |                     ClientHello...server Finished)
 *#           |                     = exporter_master_secret
 */
S2N_RESULT s2n_derive_exporter_master_secret(struct s2n_connection *conn, struct s2n_blob *secret)
{
    RESULT_ENSURE_REF(conn);
    /* Secret derivation requires these fields to be non-null.  */
    RESULT_ENSURE_REF(conn->secure);
    RESULT_ENSURE_REF(conn->secure->cipher_suite);

    RESULT_GUARD(s2n_derive_secret_with_context(conn,
            S2N_MASTER_SECRET,
            &s2n_tls13_label_exporter_master_secret,
            SERVER_FINISHED,
            secret));

    RESULT_GUARD(s2n_call_secret_callbacks(conn, secret, S2N_EXPORTER_SECRET));

    return S2N_RESULT_OK;
}

static s2n_result (*extract_methods[])(struct s2n_connection *conn) = {
    [S2N_EARLY_SECRET] = &s2n_extract_early_secret_for_schedule,
    [S2N_HANDSHAKE_SECRET] = &s2n_extract_handshake_secret,
    [S2N_MASTER_SECRET] = &s2n_extract_master_secret,
};

S2N_RESULT s2n_tls13_extract_secret(struct s2n_connection *conn, s2n_extract_secret_type_t secret_type)
{
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE_REF(conn->secure);
    RESULT_ENSURE_REF(conn->secure->cipher_suite);
    RESULT_ENSURE_REF(conn->handshake.hashes);
    RESULT_ENSURE_NE(secret_type, S2N_NONE_SECRET);

    RESULT_ENSURE_GTE(secret_type, 0);
    RESULT_ENSURE_LT(secret_type, s2n_array_len(extract_methods));

    s2n_extract_secret_type_t next_secret_type = conn->secrets.extract_secret_type + 1;
    for (s2n_extract_secret_type_t i = next_secret_type; i <= secret_type; i++) {
        RESULT_ENSURE_REF(extract_methods[i]);
        RESULT_GUARD(extract_methods[i](conn));
        conn->secrets.extract_secret_type = i;
    }

    return S2N_RESULT_OK;
}

static s2n_result (*derive_methods[][2])(struct s2n_connection *conn, struct s2n_blob *secret) = {
    [S2N_EARLY_SECRET] = { &s2n_derive_client_early_traffic_secret, &s2n_derive_client_early_traffic_secret },
    [S2N_HANDSHAKE_SECRET] = { &s2n_derive_server_handshake_traffic_secret, &s2n_derive_client_handshake_traffic_secret },
    [S2N_MASTER_SECRET] = { &s2n_derive_server_application_traffic_secret, &s2n_derive_client_application_traffic_secret },
};

S2N_RESULT s2n_tls13_derive_secret(struct s2n_connection *conn, s2n_extract_secret_type_t secret_type,
        s2n_mode mode, struct s2n_blob *secret)
{
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE_REF(secret);
    RESULT_ENSURE_REF(conn->secure);
    RESULT_ENSURE_REF(conn->secure->cipher_suite);
    RESULT_ENSURE_REF(conn->handshake.hashes);
    RESULT_ENSURE_NE(secret_type, S2N_NONE_SECRET);

    RESULT_GUARD(s2n_tls13_extract_secret(conn, secret_type));

    RESULT_ENSURE_GTE(secret_type, 0);
    RESULT_ENSURE_LT(secret_type, s2n_array_len(derive_methods));
    RESULT_ENSURE_REF(derive_methods[secret_type][mode]);
    RESULT_GUARD(derive_methods[secret_type][mode](conn, secret));

    RESULT_GUARD(s2n_trigger_secret_callbacks(conn, secret, secret_type, mode));
    return S2N_RESULT_OK;
}

S2N_RESULT s2n_tls13_secrets_clean(struct s2n_connection *conn)
{
    RESULT_ENSURE_REF(conn);
    /* Secret clean requires these fields to be non-null.  */
    RESULT_ENSURE_REF(conn->secure);
    RESULT_ENSURE_REF(conn->secure->cipher_suite);

    if (conn->actual_protocol_version < S2N_TLS13) {
        return S2N_RESULT_OK;
    }

    /*
     * Wipe base secrets.
     * Not strictly necessary, but probably safer than leaving them.
     * A compromised secret additionally compromises all secrets derived from it,
     * so these are the most sensitive secrets.
     */
    RESULT_GUARD_POSIX(s2n_blob_zero(&CONN_SECRET(conn, extract_secret)));
    conn->secrets.extract_secret_type = S2N_NONE_SECRET;

    /* Wipe other secrets no longer needed */
    RESULT_GUARD_POSIX(s2n_blob_zero(&CONN_SECRET(conn, client_early_secret)));
    RESULT_GUARD_POSIX(s2n_blob_zero(&CONN_SECRET(conn, client_handshake_secret)));
    RESULT_GUARD_POSIX(s2n_blob_zero(&CONN_SECRET(conn, server_handshake_secret)));

    return S2N_RESULT_OK;
}

S2N_RESULT s2n_tls13_secrets_update(struct s2n_connection *conn)
{
    RESULT_ENSURE_REF(conn);
    if (s2n_connection_get_protocol_version(conn) < S2N_TLS13) {
        return S2N_RESULT_OK;
    }

    /* Secret update requires these fields to be non-null.  */
    RESULT_ENSURE_REF(conn->secure);
    RESULT_ENSURE_REF(conn->secure->cipher_suite);

    message_type_t message_type = s2n_conn_get_current_message_type(conn);
    switch (message_type) {
        case CLIENT_HELLO:
            if (conn->early_data_state == S2N_EARLY_DATA_REQUESTED
                    || conn->early_data_state == S2N_EARLY_DATA_ACCEPTED) {
                RESULT_GUARD(s2n_calculate_transcript_digest(conn));
                RESULT_GUARD(s2n_tls13_derive_secret(conn, S2N_EARLY_SECRET,
                        S2N_CLIENT, &CONN_SECRET(conn, client_early_secret)));
            }
            break;
        case SERVER_HELLO:
            RESULT_GUARD(s2n_calculate_transcript_digest(conn));
            RESULT_GUARD(s2n_tls13_derive_secret(conn, S2N_HANDSHAKE_SECRET,
                    S2N_CLIENT, &CONN_SECRET(conn, client_handshake_secret)));
            RESULT_GUARD(s2n_tls13_derive_secret(conn, S2N_HANDSHAKE_SECRET,
                    S2N_SERVER, &CONN_SECRET(conn, server_handshake_secret)));
            break;
        case SERVER_FINISHED:
            RESULT_GUARD(s2n_calculate_transcript_digest(conn));
            RESULT_GUARD(s2n_tls13_derive_secret(conn, S2N_MASTER_SECRET,
                    S2N_CLIENT, &CONN_SECRET(conn, client_app_secret)));
            RESULT_GUARD(s2n_tls13_derive_secret(conn, S2N_MASTER_SECRET,
                    S2N_SERVER, &CONN_SECRET(conn, server_app_secret)));
            RESULT_GUARD(s2n_derive_exporter_master_secret(conn,
                    &CONN_SECRET(conn, exporter_master_secret)));
            break;
        case CLIENT_FINISHED:
            RESULT_GUARD(s2n_calculate_transcript_digest(conn));
            RESULT_GUARD(s2n_derive_resumption_master_secret(conn));
            break;
        default:
            break;
    }
    return S2N_RESULT_OK;
}

S2N_RESULT s2n_tls13_secrets_get(struct s2n_connection *conn, s2n_extract_secret_type_t secret_type,
        s2n_mode mode, struct s2n_blob *secret)
{
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE_REF(secret);
    /* Getting secrets requires these fields to be non-null.  */
    RESULT_ENSURE_REF(conn->secure);
    RESULT_ENSURE_REF(conn->secure->cipher_suite);

    bool is_available = (secret_type <= conn->secrets.extract_secret_type)
            /* Unlike the other secrets, we don't wipe the master / app secrets */
            || (secret_type == S2N_MASTER_SECRET && s2n_handshake_is_complete(conn));
    RESULT_ENSURE(is_available, S2N_ERR_SAFETY);

    uint8_t *secrets[][2] = {
        [S2N_EARLY_SECRET] = { NULL, CONN_SECRETS(conn).client_early_secret },
        [S2N_HANDSHAKE_SECRET] = { CONN_SECRETS(conn).server_handshake_secret, CONN_SECRETS(conn).client_handshake_secret },
        [S2N_MASTER_SECRET] = { CONN_SECRETS(conn).server_app_secret, CONN_SECRETS(conn).client_app_secret },
    };
    RESULT_ENSURE_GT(secret_type, S2N_NONE_SECRET);
    RESULT_ENSURE_LT(secret_type, s2n_array_len(secrets));
    RESULT_ENSURE_REF(secrets[secret_type][mode]);

    secret->size = s2n_get_hash_len(CONN_HMAC_ALG(conn));
    RESULT_CHECKED_MEMCPY(secret->data, secrets[secret_type][mode], secret->size);
    RESULT_ENSURE_GT(secret->size, 0);
    return S2N_RESULT_OK;
}

/*
 *= https://www.rfc-editor.org/rfc/rfc8446#section-7.5
 *# The exporter value is computed as:
 *#
 *# TLS-Exporter(label, context_value, key_length) =
 *#     HKDF-Expand-Label(Derive-Secret(Secret, label, ""),
 *#                       "exporter", Hash(context_value), key_length)
 */
int s2n_connection_tls_exporter(struct s2n_connection *conn,
        const uint8_t *label_in, uint32_t label_length,
        const uint8_t *context, uint32_t context_length,
        uint8_t *output_in, uint32_t output_length)
{
    POSIX_ENSURE_REF(conn);
    POSIX_ENSURE_REF(output_in);
    POSIX_ENSURE_REF(label_in);
    POSIX_ENSURE_REF(context);
    POSIX_ENSURE(s2n_connection_get_protocol_version(conn) == S2N_TLS13, S2N_ERR_INVALID_STATE);

    POSIX_ENSURE(is_handshake_complete(conn), S2N_ERR_HANDSHAKE_NOT_COMPLETE);
    POSIX_ENSURE_REF(conn->secure);
    POSIX_ENSURE_REF(conn->secure->cipher_suite);
    s2n_hmac_algorithm hmac_alg = conn->secure->cipher_suite->prf_alg;

    uint8_t label_bytes[S2N_MAX_HKDF_EXPAND_LABEL_LENGTH] = { 0 };
    struct s2n_blob label = { 0 };
    POSIX_ENSURE_LTE(label_length, sizeof(label_bytes));
    POSIX_CHECKED_MEMCPY(label_bytes, label_in, label_length);
    POSIX_GUARD(s2n_blob_init(&label, label_bytes, label_length));

    uint8_t derived_secret_bytes[S2N_MAX_DIGEST_LEN] = { 0 };
    struct s2n_blob derived_secret = { 0 };
    POSIX_ENSURE_LTE(s2n_get_hash_len(CONN_HMAC_ALG(conn)), S2N_MAX_DIGEST_LEN);
    POSIX_GUARD(s2n_blob_init(&derived_secret,
            derived_secret_bytes, s2n_get_hash_len(CONN_HMAC_ALG(conn))));
    POSIX_GUARD_RESULT(s2n_derive_secret(hmac_alg, &CONN_SECRET(conn, exporter_master_secret),
            &label, &EMPTY_CONTEXT(hmac_alg), &derived_secret));

    DEFER_CLEANUP(struct s2n_hmac_state hmac_state = { 0 }, s2n_hmac_free);
    POSIX_GUARD(s2n_hmac_new(&hmac_state));

    DEFER_CLEANUP(struct s2n_hash_state hash = { 0 }, s2n_hash_free);
    POSIX_GUARD(s2n_hash_new(&hash));

    s2n_hash_algorithm hash_alg = { 0 };
    POSIX_GUARD(s2n_hmac_hash_alg(hmac_alg, &hash_alg));
    uint8_t digest_bytes[S2N_MAX_DIGEST_LEN] = { 0 };
    struct s2n_blob digest = { 0 };
    POSIX_ENSURE_LTE(s2n_get_hash_len(CONN_HMAC_ALG(conn)), S2N_MAX_DIGEST_LEN);
    POSIX_GUARD(s2n_blob_init(&digest, digest_bytes, s2n_get_hash_len(CONN_HMAC_ALG(conn))));

    POSIX_GUARD(s2n_hash_init(&hash, hash_alg));
    POSIX_GUARD(s2n_hash_update(&hash, context, context_length));
    POSIX_GUARD(s2n_hash_digest(&hash, digest.data, digest.size));

    struct s2n_blob output = { 0 };
    POSIX_GUARD(s2n_blob_init(&output, output_in, output_length));
    POSIX_GUARD(s2n_hkdf_expand_label(&hmac_state, hmac_alg,
            &derived_secret, &s2n_tls13_label_exporter, &digest, &output));

    return S2N_SUCCESS;
}

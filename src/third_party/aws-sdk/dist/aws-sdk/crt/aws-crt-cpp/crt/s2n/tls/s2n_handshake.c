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

#include <stdint.h>

#include "error/s2n_errno.h"
#include "stuffer/s2n_stuffer.h"
#include "tls/s2n_cipher_suites.h"
#include "tls/s2n_connection.h"
#include "tls/s2n_record.h"
#include "tls/s2n_tls.h"
#include "utils/s2n_map.h"
#include "utils/s2n_safety.h"

int s2n_handshake_write_header(struct s2n_stuffer *out, uint8_t message_type)
{
    S2N_ERROR_IF(s2n_stuffer_data_available(out), S2N_ERR_HANDSHAKE_STATE);

    /* Write the message header */
    POSIX_GUARD(s2n_stuffer_write_uint8(out, message_type));

    /* Leave the length blank for now */
    uint16_t length = 0;
    POSIX_GUARD(s2n_stuffer_write_uint24(out, length));

    return S2N_SUCCESS;
}

int s2n_handshake_finish_header(struct s2n_stuffer *out)
{
    uint16_t length = s2n_stuffer_data_available(out);
    S2N_ERROR_IF(length < TLS_HANDSHAKE_HEADER_LENGTH, S2N_ERR_SIZE_MISMATCH);

    uint16_t payload = length - TLS_HANDSHAKE_HEADER_LENGTH;

    /* Write the message header */
    POSIX_GUARD(s2n_stuffer_rewrite(out));
    POSIX_GUARD(s2n_stuffer_skip_write(out, 1));
    POSIX_GUARD(s2n_stuffer_write_uint24(out, payload));
    POSIX_GUARD(s2n_stuffer_skip_write(out, payload));

    return S2N_SUCCESS;
}

S2N_RESULT s2n_handshake_parse_header(struct s2n_stuffer *io, uint8_t *message_type, uint32_t *length)
{
    RESULT_ENSURE(s2n_stuffer_data_available(io) >= TLS_HANDSHAKE_HEADER_LENGTH, S2N_ERR_SIZE_MISMATCH);

    /* read the message header */
    RESULT_GUARD_POSIX(s2n_stuffer_read_uint8(io, message_type));
    RESULT_GUARD_POSIX(s2n_stuffer_read_uint24(io, length));

    return S2N_RESULT_OK;
}

static int s2n_handshake_get_hash_state_ptr(struct s2n_connection *conn, s2n_hash_algorithm hash_alg, struct s2n_hash_state **hash_state)
{
    POSIX_ENSURE_REF(conn);
    POSIX_ENSURE_REF(conn->handshake.hashes);

    switch (hash_alg) {
        case S2N_HASH_MD5:
            *hash_state = &conn->handshake.hashes->md5;
            break;
        case S2N_HASH_SHA1:
            *hash_state = &conn->handshake.hashes->sha1;
            break;
        case S2N_HASH_SHA224:
            *hash_state = &conn->handshake.hashes->sha224;
            break;
        case S2N_HASH_SHA256:
            *hash_state = &conn->handshake.hashes->sha256;
            break;
        case S2N_HASH_SHA384:
            *hash_state = &conn->handshake.hashes->sha384;
            break;
        case S2N_HASH_SHA512:
            *hash_state = &conn->handshake.hashes->sha512;
            break;
        case S2N_HASH_MD5_SHA1:
            *hash_state = &conn->handshake.hashes->md5_sha1;
            break;
        default:
            POSIX_BAIL(S2N_ERR_HASH_INVALID_ALGORITHM);
            break;
    }

    return S2N_SUCCESS;
}

S2N_RESULT s2n_handshake_reset_hash_state(struct s2n_connection *conn, s2n_hash_algorithm hash_alg)
{
    struct s2n_hash_state *hash_state = NULL;
    RESULT_GUARD_POSIX(s2n_handshake_get_hash_state_ptr(conn, hash_alg, &hash_state));
    RESULT_GUARD_POSIX(s2n_hash_reset(hash_state));
    return S2N_RESULT_OK;
}

S2N_RESULT s2n_handshake_copy_hash_state(struct s2n_connection *conn, s2n_hash_algorithm hash_alg, struct s2n_hash_state *copy)
{
    struct s2n_hash_state *hash_state = NULL;
    RESULT_GUARD_POSIX(s2n_handshake_get_hash_state_ptr(conn, hash_alg, &hash_state));
    RESULT_GUARD_POSIX(s2n_hash_copy(copy, hash_state));
    return S2N_RESULT_OK;
}

int s2n_handshake_require_all_hashes(struct s2n_handshake *handshake)
{
    memset(handshake->required_hash_algs, 1, sizeof(handshake->required_hash_algs));
    return S2N_SUCCESS;
}

static int s2n_handshake_require_hash(struct s2n_handshake *handshake, s2n_hash_algorithm hash_alg)
{
    handshake->required_hash_algs[hash_alg] = 1;
    return S2N_SUCCESS;
}

uint8_t s2n_handshake_is_hash_required(struct s2n_handshake *handshake, s2n_hash_algorithm hash_alg)
{
    return handshake->required_hash_algs[hash_alg];
}

/* Update the required handshake hash algs depending on current handshake session state.
 * This function must called at the end of a handshake message handler. Additionally it must be called after the
 * ClientHello or ServerHello is processed in client and server mode respectively. The relevant handshake parameters
 * are not available until those messages are processed.
 */
int s2n_conn_update_required_handshake_hashes(struct s2n_connection *conn)
{
    POSIX_ENSURE_REF(conn);
    POSIX_ENSURE_REF(conn->secure);

    /* Clear all of the required hashes */
    memset(conn->handshake.required_hash_algs, 0, sizeof(conn->handshake.required_hash_algs));

    message_type_t handshake_message = s2n_conn_get_current_message_type(conn);
    const uint8_t client_cert_verify_done = (handshake_message >= CLIENT_CERT_VERIFY) ? 1 : 0;
    s2n_cert_auth_type client_cert_auth_type;
    POSIX_GUARD(s2n_connection_get_client_auth_type(conn, &client_cert_auth_type));

    /* If client authentication is possible, all hashes are needed until we're past CLIENT_CERT_VERIFY. */
    if ((client_cert_auth_type != S2N_CERT_AUTH_NONE) && !client_cert_verify_done) {
        POSIX_GUARD(s2n_handshake_require_all_hashes(&conn->handshake));
        return S2N_SUCCESS;
    }

    /* We don't need all of the hashes. Set the hash alg(s) required for the PRF */
    switch (conn->actual_protocol_version) {
        case S2N_SSLv3:
        case S2N_TLS10:
        case S2N_TLS11:
            POSIX_GUARD(s2n_handshake_require_hash(&conn->handshake, S2N_HASH_MD5));
            POSIX_GUARD(s2n_handshake_require_hash(&conn->handshake, S2N_HASH_SHA1));
            break;
        case S2N_TLS12:
            /* fall through */
        case S2N_TLS13: {
            /* For TLS 1.2 and TLS 1.3, the cipher suite defines the PRF hash alg */
            s2n_hmac_algorithm prf_alg = conn->secure->cipher_suite->prf_alg;
            s2n_hash_algorithm hash_alg;
            POSIX_GUARD(s2n_hmac_hash_alg(prf_alg, &hash_alg));
            POSIX_GUARD(s2n_handshake_require_hash(&conn->handshake, hash_alg));
            break;
        }
    }

    return S2N_SUCCESS;
}

/*
 * Take a hostname and return a single "simple" wildcard domain name that matches it.
 * The output wildcard representation is meant to be compared directly against a wildcard domain in a certificate.
 * We take a restrictive definition of wildcard here to achieve a single unique wildcard representation
 * given any input hostname.
 * No embedded or trailing wildcards are supported. Additionally, we only support one level of wildcard matching.
 * Thus the output should be a single wildcard character in the first(left-most) DNS label.
 *
 * Example:
 * - my.domain.name -> *.domain.name
 *
 * Not supported:
 * - my.domain.name -> m*.domain.name
 * - my.domain.name -> my.*.name
 * etc.
 *
 * The motivation for using a constrained definition of wildcard:
 * - Support for issuing non-simple wildcard certificates is insignificant.
 * - Certificate selection can be implemented with a constant number of lookups(two).
 */
int s2n_create_wildcard_hostname(struct s2n_stuffer *hostname_stuffer, struct s2n_stuffer *output)
{
    /* Find the end of the first label */
    POSIX_GUARD(s2n_stuffer_skip_to_char(hostname_stuffer, '.'));

    /* No first label found */
    if (s2n_stuffer_data_available(hostname_stuffer) == 0) {
        return S2N_SUCCESS;
    }

    /* Slap a single wildcard character to be the first label in output */
    POSIX_GUARD(s2n_stuffer_write_uint8(output, '*'));

    /* Simply copy the rest of the input to the output. */
    POSIX_GUARD(s2n_stuffer_copy(hostname_stuffer, output, s2n_stuffer_data_available(hostname_stuffer)));

    return S2N_SUCCESS;
}

static int s2n_find_cert_matches(struct s2n_map *domain_name_to_cert_map,
        struct s2n_blob *dns_name,
        struct s2n_cert_chain_and_key *matches[S2N_CERT_TYPE_COUNT],
        uint8_t *match_exists)
{
    struct s2n_blob map_value = { 0 };
    bool key_found = false;
    POSIX_GUARD_RESULT(s2n_map_lookup(domain_name_to_cert_map, dns_name, &map_value, &key_found));
    if (key_found) {
        struct certs_by_type *value = (void *) map_value.data;
        for (int i = 0; i < S2N_CERT_TYPE_COUNT; i++) {
            matches[i] = value->certs[i];
        }
        *match_exists = 1;
    }

    return S2N_SUCCESS;
}

/* Find certificates that match the ServerName TLS extension sent by the client.
 * For a given ServerName there can be multiple matching certificates based on the
 * type of key in the certificate.
 *
 * A match is determined using s2n_map lookup by DNS name.
 * Wildcards that have a single * in the left most label are supported.
 */
int s2n_conn_find_name_matching_certs(struct s2n_connection *conn)
{
    if (!s2n_server_received_server_name(conn)) {
        return S2N_SUCCESS;
    }
    const char *name = conn->server_name;
    struct s2n_blob hostname_blob = { 0 };
    POSIX_GUARD(s2n_blob_init(&hostname_blob, (uint8_t *) (uintptr_t) name, strlen(name)));
    POSIX_ENSURE_LTE(hostname_blob.size, S2N_MAX_SERVER_NAME);
    char normalized_hostname[S2N_MAX_SERVER_NAME + 1] = { 0 };
    POSIX_CHECKED_MEMCPY(normalized_hostname, hostname_blob.data, hostname_blob.size);
    struct s2n_blob normalized_name = { 0 };
    POSIX_GUARD(s2n_blob_init(&normalized_name, (uint8_t *) normalized_hostname, hostname_blob.size));

    POSIX_GUARD(s2n_blob_char_to_lower(&normalized_name));
    struct s2n_stuffer normalized_hostname_stuffer = { 0 };
    POSIX_GUARD(s2n_stuffer_init(&normalized_hostname_stuffer, &normalized_name));
    POSIX_GUARD(s2n_stuffer_skip_write(&normalized_hostname_stuffer, normalized_name.size));

    /* Find the exact matches for the ServerName */
    POSIX_GUARD(s2n_find_cert_matches(conn->config->domain_name_to_cert_map,
            &normalized_name,
            conn->handshake_params.exact_sni_matches,
            &(conn->handshake_params.exact_sni_match_exists)));

    if (!conn->handshake_params.exact_sni_match_exists) {
        /* We have not yet found an exact domain match. Try to find wildcard matches. */
        char wildcard_hostname[S2N_MAX_SERVER_NAME + 1] = { 0 };
        struct s2n_blob wildcard_blob = { 0 };
        POSIX_GUARD(s2n_blob_init(&wildcard_blob, (uint8_t *) wildcard_hostname, sizeof(wildcard_hostname)));
        struct s2n_stuffer wildcard_stuffer = { 0 };
        POSIX_GUARD(s2n_stuffer_init(&wildcard_stuffer, &wildcard_blob));
        POSIX_GUARD(s2n_create_wildcard_hostname(&normalized_hostname_stuffer, &wildcard_stuffer));
        const uint32_t wildcard_len = s2n_stuffer_data_available(&wildcard_stuffer);

        /* Couldn't create a valid wildcard from the input */
        if (wildcard_len == 0) {
            return S2N_SUCCESS;
        }

        /* The client's SNI is wildcardified, do an exact match against the set of server certs. */
        wildcard_blob.size = wildcard_len;
        POSIX_GUARD(s2n_find_cert_matches(conn->config->domain_name_to_cert_map,
                &wildcard_blob,
                conn->handshake_params.wc_sni_matches,
                &(conn->handshake_params.wc_sni_match_exists)));
    }

    /* If we found a suitable cert, we should send back the ServerName extension.
     * Note that this may have already been set by the client hello callback, so we won't override its value
     */
    conn->server_name_used = conn->server_name_used
            || conn->handshake_params.exact_sni_match_exists
            || conn->handshake_params.wc_sni_match_exists;

    return S2N_SUCCESS;
}

/* Find the optimal certificate of a specific type.
 * The priority of set of certificates to choose from:
 * 1. Certificates that match the client's ServerName extension.
 * 2. Default certificates
 */
struct s2n_cert_chain_and_key *s2n_get_compatible_cert_chain_and_key(struct s2n_connection *conn, const s2n_pkey_type cert_type)
{
    if (conn->handshake_params.exact_sni_match_exists) {
        /* This may return NULL if there was an SNI match, but not a match the cipher_suite's authentication type. */
        return conn->handshake_params.exact_sni_matches[cert_type];
    }
    if (conn->handshake_params.wc_sni_match_exists) {
        return conn->handshake_params.wc_sni_matches[cert_type];
    } else {
        /* We don't have any name matches. Use the default certificate that works with the key type. */
        return conn->config->default_certs_by_type.certs[cert_type];
    }
}

/* This method will work when testing S2N, and for the EndOfEarlyData message.
 *
 * However, it will NOT work for arbitrary message types when potentially receiving records
 * that contain multiple messages, like when talking to a non-S2N TLS implementation. If the "end_message"
 * is not the first message in a multi-message record, negotiation will not stop.
 * (This is not an issue for EndOfEarlyData because encryption and message order requirements force
 * EndOfEarlyData to always be the first and only handshake message in its handshake record)
 */
S2N_RESULT s2n_negotiate_until_message(struct s2n_connection *conn, s2n_blocked_status *blocked, message_type_t end_message)
{
    RESULT_ENSURE_REF(conn);
    conn->handshake.end_of_messages = end_message;
    int r = s2n_negotiate(conn, blocked);
    conn->handshake.end_of_messages = APPLICATION_DATA;
    RESULT_GUARD_POSIX(r);
    return S2N_RESULT_OK;
}

S2N_RESULT s2n_handshake_validate(const struct s2n_handshake *s2n_handshake)
{
    RESULT_ENSURE_REF(s2n_handshake);
    RESULT_DEBUG_ENSURE(s2n_handshake->handshake_type < 256, S2N_ERR_SAFETY);
    RESULT_DEBUG_ENSURE(s2n_handshake->message_number >= 0 && s2n_handshake->message_number < 32, S2N_ERR_SAFETY);
    return S2N_RESULT_OK;
}

S2N_RESULT s2n_handshake_set_finished_len(struct s2n_connection *conn, uint8_t len)
{
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE_GT(len, 0);
    RESULT_ENSURE_LTE(len, sizeof(conn->handshake.server_finished));
    RESULT_ENSURE_LTE(len, sizeof(conn->handshake.client_finished));

    /*
     * We maintain a version of the "finished" / "verify_data" field
     * for both the client and server, so this method will be called
     * once for the client version and once for the server version.
     *
     * The lengths of both versions must match, or something has
     * gone wrong in our implementation.
     */
    uint8_t *finished_length = &conn->handshake.finished_len;
    if (*finished_length == 0) {
        *finished_length = len;
    }
    RESULT_ENSURE_EQ(*finished_length, len);

    return S2N_RESULT_OK;
}

bool s2n_handshake_is_renegotiation(struct s2n_connection *conn)
{
    return conn && conn->handshake.renegotiation;
}

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

#include "tls/extensions/s2n_extension_list.h"
#include "tls/s2n_fingerprint.h"
#include "utils/s2n_blob.h"
#include "utils/s2n_safety.h"

#define S2N_JA3_FIELD_DIV ','
#define S2N_JA3_LIST_DIV  '-'

/* UINT16_MAX == 65535 */
#define S2N_UINT16_STR_MAX_SIZE 5

static S2N_RESULT s2n_fingerprint_ja3_digest(struct s2n_fingerprint_hash *hash,
        struct s2n_stuffer *out)
{
    if (!s2n_fingerprint_hash_do_digest(hash)) {
        return S2N_RESULT_OK;
    }

    uint8_t digest_bytes[MD5_DIGEST_LENGTH] = { 0 };
    struct s2n_blob digest = { 0 };
    RESULT_GUARD_POSIX(s2n_blob_init(&digest, digest_bytes, sizeof(digest_bytes)));
    RESULT_GUARD(s2n_fingerprint_hash_digest(hash, &digest));
    RESULT_GUARD(s2n_stuffer_write_hex(out, &digest));

    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_fingerprint_ja3_iana(struct s2n_fingerprint_hash *hash,
        bool *is_list, uint16_t iana)
{
    if (s2n_fingerprint_is_grease_value(iana)) {
        return S2N_RESULT_OK;
    }

    /* If we have already written at least one value for this field,
     * then we are writing a list and need to prepend a list divider before
     * writing the next value.
     */
    if (*is_list) {
        RESULT_GUARD(s2n_fingerprint_hash_add_char(hash, S2N_JA3_LIST_DIV));
    } else {
        *is_list = true;
    }

    /* snprintf always appends a '\0' to the output,
     * but that extra '\0' is not included in the return value */
    char str[S2N_UINT16_STR_MAX_SIZE + 1] = { 0 };
    int written = snprintf(str, sizeof(str), "%u", iana);
    RESULT_ENSURE_GT(written, 0);
    RESULT_ENSURE_LTE(written, S2N_UINT16_STR_MAX_SIZE);

    RESULT_GUARD(s2n_fingerprint_hash_add_str(hash, str, written));
    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_fingerprint_ja3_version(struct s2n_fingerprint_hash *hash,
        struct s2n_client_hello *ch)
{
    uint16_t version = 0;
    RESULT_GUARD(s2n_fingerprint_get_legacy_version(ch, &version));

    bool is_list = false;
    RESULT_GUARD(s2n_fingerprint_ja3_iana(hash, &is_list, version));
    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_fingerprint_ja3_cipher_suites(struct s2n_fingerprint_hash *hash,
        struct s2n_client_hello *ch)
{
    RESULT_ENSURE_REF(ch);

    struct s2n_stuffer ciphers = { 0 };
    RESULT_GUARD_POSIX(s2n_stuffer_init_written(&ciphers, &ch->cipher_suites));

    bool found = false;
    while (s2n_stuffer_data_available(&ciphers)) {
        uint16_t iana = 0;
        RESULT_GUARD_POSIX(s2n_stuffer_read_uint16(&ciphers, &iana));
        RESULT_GUARD(s2n_fingerprint_ja3_iana(hash, &found, iana));
    }
    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_fingerprint_ja3_extensions(struct s2n_fingerprint_hash *hash,
        struct s2n_client_hello *ch)
{
    RESULT_ENSURE_REF(ch);

    /* We have to use the raw extensions instead of the parsed extensions
     * because s2n-tls both intentionally ignores any unknown extensions
     * and reorders the extensions when parsing the list.
     */
    struct s2n_stuffer extensions = { 0 };
    RESULT_GUARD_POSIX(s2n_stuffer_init_written(&extensions, &ch->extensions.raw));

    bool found = false;
    while (s2n_stuffer_data_available(&extensions)) {
        uint16_t iana = 0;
        RESULT_GUARD(s2n_fingerprint_parse_extension(&extensions, &iana));
        RESULT_GUARD(s2n_fingerprint_ja3_iana(hash, &found, iana));
    }
    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_fingerprint_ja3_elliptic_curves(struct s2n_fingerprint_hash *hash,
        struct s2n_client_hello *ch)
{
    RESULT_ENSURE_REF(ch);

    s2n_parsed_extension *extension = NULL;
    int result = s2n_client_hello_get_parsed_extension(S2N_EXTENSION_SUPPORTED_GROUPS,
            &ch->extensions, &extension);
    if (result != S2N_SUCCESS) {
        return S2N_RESULT_OK;
    }

    struct s2n_stuffer elliptic_curves = { 0 };
    RESULT_GUARD_POSIX(s2n_stuffer_init_written(&elliptic_curves, &extension->extension));
    RESULT_GUARD_POSIX(s2n_stuffer_skip_read(&elliptic_curves, sizeof(uint16_t)));

    bool found = false;
    while (s2n_stuffer_data_available(&elliptic_curves)) {
        uint16_t iana = 0;
        RESULT_GUARD_POSIX(s2n_stuffer_read_uint16(&elliptic_curves, &iana));
        RESULT_GUARD(s2n_fingerprint_ja3_iana(hash, &found, iana));
    }
    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_fingerprint_ja3_point_formats(struct s2n_fingerprint_hash *hash,
        struct s2n_client_hello *ch)
{
    RESULT_ENSURE_REF(ch);

    s2n_parsed_extension *extension = NULL;
    int result = s2n_client_hello_get_parsed_extension(S2N_EXTENSION_EC_POINT_FORMATS,
            &ch->extensions, &extension);
    if (result != S2N_SUCCESS) {
        return S2N_RESULT_OK;
    }

    struct s2n_stuffer point_formats = { 0 };
    RESULT_GUARD_POSIX(s2n_stuffer_init_written(&point_formats, &extension->extension));
    RESULT_GUARD_POSIX(s2n_stuffer_skip_read(&point_formats, sizeof(uint8_t)));

    bool found = false;
    while (s2n_stuffer_data_available(&point_formats)) {
        uint8_t iana = 0;
        RESULT_GUARD_POSIX(s2n_stuffer_read_uint8(&point_formats, &iana));
        RESULT_GUARD(s2n_fingerprint_ja3_iana(hash, &found, iana));
    }
    return S2N_RESULT_OK;
}

/* JA3 involves concatenating a set of fields from the ClientHello:
 *      SSLVersion,Cipher,SSLExtension,EllipticCurve,EllipticCurvePointFormat
 * For example:
 *      "769,47-53-5-10-49161-49162-49171-49172-50-56-19-4,0-10-11,23-24-25,0"
 * See https://github.com/salesforce/ja3
 */
static S2N_RESULT s2n_fingerprint_ja3_write(struct s2n_fingerprint_hash *hash,
        struct s2n_client_hello *ch)
{
    RESULT_GUARD(s2n_fingerprint_ja3_version(hash, ch));
    RESULT_GUARD(s2n_fingerprint_hash_add_char(hash, S2N_JA3_FIELD_DIV));
    RESULT_GUARD(s2n_fingerprint_ja3_cipher_suites(hash, ch));
    RESULT_GUARD(s2n_fingerprint_hash_add_char(hash, S2N_JA3_FIELD_DIV));
    RESULT_GUARD(s2n_fingerprint_ja3_extensions(hash, ch));
    RESULT_GUARD(s2n_fingerprint_hash_add_char(hash, S2N_JA3_FIELD_DIV));
    RESULT_GUARD(s2n_fingerprint_ja3_elliptic_curves(hash, ch));
    RESULT_GUARD(s2n_fingerprint_hash_add_char(hash, S2N_JA3_FIELD_DIV));
    RESULT_GUARD(s2n_fingerprint_ja3_point_formats(hash, ch));
    return S2N_RESULT_OK;
}

S2N_RESULT s2n_fingerprint_ja3(struct s2n_fingerprint *fingerprint,
        struct s2n_fingerprint_hash *hash, struct s2n_stuffer *output)
{
    RESULT_ENSURE_REF(fingerprint);
    RESULT_GUARD(s2n_fingerprint_ja3_write(hash, fingerprint->client_hello));
    RESULT_GUARD(s2n_fingerprint_ja3_digest(hash, output));

    if (s2n_fingerprint_hash_do_digest(hash)) {
        fingerprint->raw_size = hash->bytes_digested;
    } else {
        fingerprint->raw_size = s2n_stuffer_data_available(output);
    }
    return S2N_RESULT_OK;
}

struct s2n_fingerprint_method ja3_fingerprint = {
    /* The hash doesn't have to be cryptographically secure,
     * so the weakness of MD5 shouldn't be a problem. */
    .hash = S2N_HASH_MD5,
    /* The hash string is a single MD5 digest represented as hex */
    .hash_str_size = S2N_JA3_HASH_STR_SIZE,
    .fingerprint = s2n_fingerprint_ja3,
};

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

#include "tls/s2n_fingerprint.h"

#include "utils/s2n_blob.h"
#include "utils/s2n_mem.h"
#include "utils/s2n_safety.h"

static S2N_RESULT s2n_fingerprint_init(struct s2n_fingerprint *fingerprint,
        s2n_fingerprint_type type)
{
    RESULT_ENSURE_REF(fingerprint);

    switch (type) {
        case S2N_FINGERPRINT_JA3:
            fingerprint->method = &ja3_fingerprint;
            break;
        case S2N_FINGERPRINT_JA4:
            fingerprint->method = &ja4_fingerprint;
            break;
        default:
            RESULT_BAIL(S2N_ERR_INVALID_ARGUMENT);
    }

    const struct s2n_fingerprint_method *method = fingerprint->method;
    RESULT_ENSURE_REF(method);
    RESULT_GUARD_POSIX(s2n_hash_new(&fingerprint->hash));
    s2n_hash_allow_md5_for_fips(&fingerprint->hash);
    RESULT_GUARD_POSIX(s2n_hash_init(&fingerprint->hash, method->hash));
    return S2N_RESULT_OK;
}

struct s2n_fingerprint *s2n_fingerprint_new(s2n_fingerprint_type type)
{
    DEFER_CLEANUP(struct s2n_blob mem = { 0 }, s2n_free);
    PTR_GUARD_POSIX(s2n_alloc(&mem, sizeof(struct s2n_fingerprint)));
    PTR_GUARD_POSIX(s2n_blob_zero(&mem));
    struct s2n_fingerprint *fingerprint = (struct s2n_fingerprint *) (void *) mem.data;
    PTR_ENSURE_REF(fingerprint);
    PTR_GUARD_RESULT(s2n_fingerprint_init(fingerprint, type));
    ZERO_TO_DISABLE_DEFER_CLEANUP(mem);
    return fingerprint;
}

static S2N_CLEANUP_RESULT s2n_fingerprint_free_fields(struct s2n_fingerprint *fingerprint)
{
    if (!fingerprint) {
        return S2N_RESULT_OK;
    }
    RESULT_GUARD_POSIX(s2n_hash_free(&fingerprint->hash));
    RESULT_GUARD_POSIX(s2n_stuffer_free(&fingerprint->workspace));
    return S2N_RESULT_OK;
}

int s2n_fingerprint_free(struct s2n_fingerprint **fingerprint_ptr)
{
    if (!fingerprint_ptr) {
        return S2N_SUCCESS;
    }
    POSIX_GUARD_RESULT(s2n_fingerprint_free_fields(*fingerprint_ptr));
    POSIX_GUARD(s2n_free_object((uint8_t **) (void **) fingerprint_ptr,
            sizeof(struct s2n_fingerprint)));
    return S2N_SUCCESS;
}

int s2n_fingerprint_wipe(struct s2n_fingerprint *fingerprint)
{
    POSIX_ENSURE(fingerprint, S2N_ERR_INVALID_ARGUMENT);
    fingerprint->client_hello = NULL;
    fingerprint->raw_size = 0;
    return S2N_SUCCESS;
}

int s2n_fingerprint_set_client_hello(struct s2n_fingerprint *fingerprint, struct s2n_client_hello *ch)
{
    POSIX_ENSURE(fingerprint, S2N_ERR_INVALID_ARGUMENT);
    POSIX_ENSURE(ch, S2N_ERR_INVALID_ARGUMENT);
    POSIX_ENSURE(!ch->sslv2, S2N_ERR_PROTOCOL_VERSION_UNSUPPORTED);
    POSIX_GUARD(s2n_fingerprint_wipe(fingerprint));
    fingerprint->client_hello = ch;
    return S2N_SUCCESS;
}

int s2n_fingerprint_get_hash_size(const struct s2n_fingerprint *fingerprint, uint32_t *size)
{
    POSIX_ENSURE(fingerprint, S2N_ERR_INVALID_ARGUMENT);
    const struct s2n_fingerprint_method *method = fingerprint->method;
    POSIX_ENSURE_REF(method);
    POSIX_ENSURE(size, S2N_ERR_INVALID_ARGUMENT);
    *size = method->hash_str_size;
    return S2N_SUCCESS;
}

int s2n_fingerprint_get_hash(struct s2n_fingerprint *fingerprint,
        uint32_t max_output_size, uint8_t *output, uint32_t *output_size)
{
    POSIX_ENSURE(fingerprint, S2N_ERR_INVALID_ARGUMENT);
    const struct s2n_fingerprint_method *method = fingerprint->method;
    POSIX_ENSURE_REF(method);

    POSIX_ENSURE(max_output_size >= method->hash_str_size, S2N_ERR_INSUFFICIENT_MEM_SIZE);
    POSIX_ENSURE(output, S2N_ERR_INVALID_ARGUMENT);
    POSIX_ENSURE(output_size, S2N_ERR_INVALID_ARGUMENT);
    *output_size = 0;

    struct s2n_fingerprint_hash hash = {
        .hash = &fingerprint->hash,
    };
    POSIX_GUARD(s2n_hash_reset(&fingerprint->hash));

    struct s2n_stuffer output_stuffer = { 0 };
    POSIX_GUARD(s2n_blob_init(&output_stuffer.blob, output, max_output_size));

    POSIX_ENSURE(fingerprint->client_hello, S2N_ERR_INVALID_STATE);
    POSIX_GUARD_RESULT(method->fingerprint(fingerprint, &hash, &output_stuffer));

    *output_size = s2n_stuffer_data_available(&output_stuffer);
    return S2N_SUCCESS;
}

int s2n_fingerprint_get_raw_size(const struct s2n_fingerprint *fingerprint, uint32_t *size)
{
    POSIX_ENSURE(fingerprint, S2N_ERR_INVALID_ARGUMENT);
    POSIX_ENSURE(size, S2N_ERR_INVALID_ARGUMENT);
    /* A zero-length raw string is impossible for all fingerprinting methods
     * currently supported, so raw_size == 0 indicates that raw_size has not been
     * calculated yet.
     */
    POSIX_ENSURE(fingerprint->raw_size != 0, S2N_ERR_INVALID_STATE);
    *size = fingerprint->raw_size;
    return S2N_SUCCESS;
}

int s2n_fingerprint_get_raw(struct s2n_fingerprint *fingerprint,
        uint32_t max_output_size, uint8_t *output, uint32_t *output_size)
{
    POSIX_ENSURE(fingerprint, S2N_ERR_INVALID_ARGUMENT);
    const struct s2n_fingerprint_method *method = fingerprint->method;
    POSIX_ENSURE_REF(method);

    POSIX_ENSURE(max_output_size > 0, S2N_ERR_INSUFFICIENT_MEM_SIZE);
    POSIX_ENSURE(output, S2N_ERR_INVALID_ARGUMENT);
    POSIX_ENSURE(output_size, S2N_ERR_INVALID_ARGUMENT);
    *output_size = 0;

    struct s2n_stuffer output_stuffer = { 0 };
    POSIX_GUARD(s2n_blob_init(&output_stuffer.blob, output, max_output_size));
    struct s2n_fingerprint_hash hash = {
        .buffer = &output_stuffer,
    };

    POSIX_ENSURE(fingerprint->client_hello, S2N_ERR_INVALID_STATE);
    POSIX_GUARD_RESULT(method->fingerprint(fingerprint, &hash, &output_stuffer));
    *output_size = s2n_stuffer_data_available(&output_stuffer);
    return S2N_SUCCESS;
}

/* See https://datatracker.ietf.org/doc/html/rfc8701
 * for an explanation of GREASE and lists of the GREASE values.
 */
static S2N_RESULT s2n_assert_grease_value(uint16_t val)
{
    uint8_t byte1 = val >> 8;
    uint8_t byte2 = val & 0x00FF;
    /* Both bytes of the GREASE values are identical */
    RESULT_ENSURE_EQ(byte1, byte2);
    /* The GREASE value bytes all follow the format 0x[0-F]A.
     * So 0x0A, 0x1A, 0x2A etc, up to 0xFA. */
    RESULT_ENSURE_EQ((byte1 | 0xF0), 0xFA);
    return S2N_RESULT_OK;
}

/**
 *= https://raw.githubusercontent.com/FoxIO-LLC/ja4/df3c067/technical_details/JA4.md#details
 *# The program needs to ignore GREASE values anywhere it sees them
 */
bool s2n_fingerprint_is_grease_value(uint16_t val)
{
    return s2n_result_is_ok(s2n_assert_grease_value(val));
}

S2N_RESULT s2n_fingerprint_parse_extension(struct s2n_stuffer *input, uint16_t *iana)
{
    uint16_t size = 0;
    RESULT_GUARD_POSIX(s2n_stuffer_read_uint16(input, iana));
    RESULT_GUARD_POSIX(s2n_stuffer_read_uint16(input, &size));
    RESULT_GUARD_POSIX(s2n_stuffer_skip_read(input, size));
    return S2N_RESULT_OK;
}

S2N_RESULT s2n_fingerprint_get_legacy_version(struct s2n_client_hello *ch, uint16_t *version)
{
    RESULT_ENSURE_REF(ch);
    RESULT_ENSURE_REF(version);
    uint8_t high_byte = (ch->legacy_version / 10);
    uint8_t low_byte = (ch->legacy_version % 10);
    *version = high_byte << 8 | low_byte;
    return S2N_RESULT_OK;
}

S2N_RESULT s2n_fingerprint_hash_add_char(struct s2n_fingerprint_hash *hash, char c)
{
    RESULT_ENSURE_REF(hash);
    if (hash->hash) {
        RESULT_GUARD_POSIX(s2n_hash_update(hash->hash, &c, 1));
    } else {
        RESULT_ENSURE_REF(hash->buffer);
        RESULT_ENSURE(s2n_stuffer_space_remaining(hash->buffer) >= 1,
                S2N_ERR_INSUFFICIENT_MEM_SIZE);
        RESULT_GUARD_POSIX(s2n_stuffer_write_char(hash->buffer, c));
    }
    return S2N_RESULT_OK;
}

S2N_RESULT s2n_fingerprint_hash_add_str(struct s2n_fingerprint_hash *hash,
        const char *str, size_t str_size)
{
    return s2n_fingerprint_hash_add_bytes(hash, (const uint8_t *) str, str_size);
}

S2N_RESULT s2n_fingerprint_hash_add_bytes(struct s2n_fingerprint_hash *hash,
        const uint8_t *bytes, size_t size)
{
    RESULT_ENSURE_REF(hash);
    RESULT_ENSURE(S2N_MEM_IS_READABLE(bytes, size), S2N_ERR_NULL);
    if (hash->hash) {
        RESULT_GUARD_POSIX(s2n_hash_update(hash->hash, bytes, size));
    } else {
        RESULT_ENSURE_REF(hash->buffer);
        RESULT_ENSURE(s2n_stuffer_space_remaining(hash->buffer) >= size,
                S2N_ERR_INSUFFICIENT_MEM_SIZE);
        RESULT_GUARD_POSIX(s2n_stuffer_write_text(hash->buffer, bytes, size));
    }
    return S2N_RESULT_OK;
}

S2N_RESULT s2n_fingerprint_hash_digest(struct s2n_fingerprint_hash *hash, struct s2n_blob *out)
{
    RESULT_ENSURE_REF(hash);
    RESULT_ENSURE_REF(hash->hash);
    RESULT_ENSURE_REF(out);

    uint64_t bytes = 0;
    RESULT_GUARD_POSIX(s2n_hash_get_currently_in_hash_total(hash->hash, &bytes));
    hash->bytes_digested += bytes;

    RESULT_GUARD_POSIX(s2n_hash_digest(hash->hash, out->data, out->size));
    RESULT_GUARD_POSIX(s2n_hash_reset(hash->hash));
    return S2N_RESULT_OK;
}

bool s2n_fingerprint_hash_do_digest(struct s2n_fingerprint_hash *hash)
{
    return hash && hash->hash;
}

int s2n_client_hello_get_fingerprint_hash(struct s2n_client_hello *ch, s2n_fingerprint_type type,
        uint32_t max_output_size, uint8_t *output, uint32_t *output_size, uint32_t *str_size)
{
    POSIX_ENSURE(type == S2N_FINGERPRINT_JA3, S2N_ERR_INVALID_ARGUMENT);
    POSIX_ENSURE(max_output_size >= MD5_DIGEST_LENGTH, S2N_ERR_INSUFFICIENT_MEM_SIZE);
    POSIX_ENSURE(str_size, S2N_ERR_INVALID_ARGUMENT);
    POSIX_ENSURE(output_size, S2N_ERR_INVALID_ARGUMENT);
    POSIX_ENSURE(output, S2N_ERR_INVALID_ARGUMENT);

    DEFER_CLEANUP(struct s2n_fingerprint fingerprint = { 0 }, s2n_fingerprint_free_fields);
    POSIX_GUARD_RESULT(s2n_fingerprint_init(&fingerprint, type));
    POSIX_GUARD(s2n_fingerprint_set_client_hello(&fingerprint, ch));

    uint32_t hex_hash_size = 0;
    uint8_t hex_hash[S2N_JA3_HASH_STR_SIZE] = { 0 };
    POSIX_GUARD(s2n_fingerprint_get_hash(&fingerprint, sizeof(hex_hash), hex_hash, &hex_hash_size));

    /* s2n_client_hello_get_fingerprint_hash expects the raw bytes of the JA3 hash,
     * but s2n_fingerprint_get_hash returns a hex string instead.
     * We need to translate back to the raw bytes.
     */
    struct s2n_blob bytes_out = { 0 };
    POSIX_GUARD(s2n_blob_init(&bytes_out, output, MD5_DIGEST_LENGTH));
    struct s2n_stuffer hex_in = { 0 };
    POSIX_GUARD(s2n_blob_init(&hex_in.blob, hex_hash, hex_hash_size));
    POSIX_GUARD(s2n_stuffer_skip_write(&hex_in, hex_hash_size));
    POSIX_GUARD_RESULT(s2n_stuffer_read_hex(&hex_in, &bytes_out));
    *output_size = bytes_out.size;

    POSIX_GUARD(s2n_fingerprint_get_raw_size(&fingerprint, str_size));
    return S2N_SUCCESS;
}

int s2n_client_hello_get_fingerprint_string(struct s2n_client_hello *ch, s2n_fingerprint_type type,
        uint32_t max_output_size, uint8_t *output, uint32_t *output_size)
{
    POSIX_ENSURE(type == S2N_FINGERPRINT_JA3, S2N_ERR_INVALID_ARGUMENT);
    DEFER_CLEANUP(struct s2n_fingerprint fingerprint = { 0 },
            s2n_fingerprint_free_fields);
    POSIX_GUARD_RESULT(s2n_fingerprint_init(&fingerprint, type));
    POSIX_GUARD(s2n_fingerprint_set_client_hello(&fingerprint, ch));
    POSIX_GUARD(s2n_fingerprint_get_raw(&fingerprint, max_output_size, output, output_size));
    return S2N_SUCCESS;
}

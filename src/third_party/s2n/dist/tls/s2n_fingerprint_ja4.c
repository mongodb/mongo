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

#include <ctype.h>

#include "crypto/s2n_hash.h"
#include "stuffer/s2n_stuffer.h"
#include "tls/extensions/s2n_client_supported_versions.h"
#include "tls/extensions/s2n_extension_list.h"
#include "tls/s2n_client_hello.h"
#include "tls/s2n_fingerprint.h"
#include "tls/s2n_protocol_preferences.h"
#include "utils/s2n_blob.h"
#include "utils/s2n_safety.h"

#define S2N_JA4_LIST_DIV ','
#define S2N_JA4_PART_DIV '_'

/**
 *= https://raw.githubusercontent.com/FoxIO-LLC/ja4/df3c067/technical_details/JA4.md#number-of-ciphers
 *# 2 character number of cipher suites
 *
 *= https://raw.githubusercontent.com/FoxIO-LLC/ja4/df3c067/technical_details/JA4.md#number-of-extensions
 *# Same as counting ciphers.
 */
#define S2N_JA4_COUNT_SIZE 2

#define S2N_HEX_PER_BYTE              2
#define S2N_JA4_DIGEST_HEX_CHAR_LIMIT 12
#define S2N_JA4_DIGEST_BYTE_LIMIT     (S2N_JA4_DIGEST_HEX_CHAR_LIMIT / S2N_HEX_PER_BYTE)

#define S2N_JA4_A_SIZE 10
#define S2N_JA4_B_SIZE S2N_JA4_DIGEST_HEX_CHAR_LIMIT
#define S2N_JA4_C_SIZE S2N_JA4_DIGEST_HEX_CHAR_LIMIT
#define S2N_JA4_SIZE   (S2N_JA4_A_SIZE + 1 + S2N_JA4_B_SIZE + 1 + S2N_JA4_C_SIZE)

#define S2N_JA4_LIST_LIMIT      99
#define S2N_JA4_IANA_HEX_SIZE   (S2N_HEX_PER_BYTE * sizeof(uint16_t))
#define S2N_JA4_IANA_ENTRY_SIZE (S2N_JA4_IANA_HEX_SIZE + 1)
#define S2N_JA4_WORKSPACE_SIZE  ((S2N_JA4_LIST_LIMIT * (S2N_JA4_IANA_ENTRY_SIZE)))

const char *s2n_ja4_version_strings[] = {
    /**
     *= https://raw.githubusercontent.com/FoxIO-LLC/ja4/df3c067/technical_details/JA4.md#tls-and-dtls-version
     *# 0x0304 = TLS 1.3 = “13”
     *# 0x0303 = TLS 1.2 = “12”
     *# 0x0302 = TLS 1.1 = “11”
     *# 0x0301 = TLS 1.0 = “10”
     */
    [0x0304] = "13",
    [0x0303] = "12",
    [0x0302] = "11",
    [0x0301] = "10",
    /**
     *= https://raw.githubusercontent.com/FoxIO-LLC/ja4/df3c067/technical_details/JA4.md#tls-and-dtls-version
     *# 0x0300 = SSL 3.0 = “s3”
     *# 0x0002 = SSL 2.0 = “s2”
     */
    [0x0300] = "s3",
    [0x0002] = "s2",
};

/**
 *= https://raw.githubusercontent.com/FoxIO-LLC/ja4/df3c067/technical_details/JA4.md#tls-and-dtls-version
 *# Unknown = “00”
 */
#define S2N_JA4_UNKNOWN_STR "00"

DEFINE_POINTER_CLEANUP_FUNC(struct s2n_stuffer *, s2n_stuffer_wipe);

static int s2n_fingerprint_ja4_iana_compare(const void *a, const void *b)
{
    const uint8_t *iana_a = (const uint8_t *) a;
    const uint8_t *iana_b = (const uint8_t *) b;
    for (size_t i = 0; i < S2N_JA4_IANA_HEX_SIZE; i++) {
        if (iana_a[i] != iana_b[i]) {
            return iana_a[i] - iana_b[i];
        }
    }
    return 0;
}

static S2N_RESULT s2n_fingerprint_ja4_digest(struct s2n_fingerprint_hash *hash,
        struct s2n_stuffer *out)
{
    RESULT_ENSURE_REF(hash);
    if (!s2n_fingerprint_hash_do_digest(hash)) {
        return S2N_RESULT_OK;
    }

    /* Instead of hashing empty inputs, JA4 sets the output to a string of all zeroes.
     * (Actually hashing an empty input doesn't produce a digest of all zeroes)
     *
     *= https://raw.githubusercontent.com/FoxIO-LLC/ja4/df3c067/technical_details/JA4.md#cipher-hash
     *# If there are no ciphers in the sorted cipher list, then the value of
     *# JA4_b is set to `000000000000`
     *
     *= https://raw.githubusercontent.com/FoxIO-LLC/ja4/df3c067/technical_details/JA4.md#extension-hash
     *# If there are no extensions in the sorted extensions list, then the value of
     *# JA4_c is set to `000000000000`
     */
    uint64_t bytes = 0;
    RESULT_GUARD_POSIX(s2n_hash_get_currently_in_hash_total(hash->hash, &bytes));
    if (bytes == 0) {
        RESULT_GUARD_POSIX(s2n_stuffer_write_str(out, "000000000000"));
        return S2N_RESULT_OK;
    }

    uint8_t digest_bytes[SHA256_DIGEST_LENGTH] = { 0 };
    struct s2n_blob digest = { 0 };
    RESULT_GUARD_POSIX(s2n_blob_init(&digest, digest_bytes, sizeof(digest_bytes)));
    RESULT_GUARD(s2n_fingerprint_hash_digest(hash, &digest));

    /* JA4 digests are truncated */
    RESULT_ENSURE_LTE(S2N_JA4_DIGEST_BYTE_LIMIT, digest.size);
    digest.size = S2N_JA4_DIGEST_BYTE_LIMIT;
    RESULT_GUARD(s2n_stuffer_write_hex(out, &digest));
    return S2N_RESULT_OK;
}

/**
 *= https://raw.githubusercontent.com/FoxIO-LLC/ja4/df3c067/technical_details/JA4.md#number-of-ciphers
 *# 2 character number of cipher suites, so if there’s 6 cipher suites
 *# in the hello packet, then the value should be “06”.
 *
 *= https://raw.githubusercontent.com/FoxIO-LLC/ja4/df3c067/technical_details/JA4.md#number-of-extensions
 *# Same as counting ciphers.
 */
static S2N_RESULT s2n_fingerprint_ja4_count(struct s2n_blob *output, uint16_t count)
{
    RESULT_ENSURE_REF(output);

    /**
     *= https://raw.githubusercontent.com/FoxIO-LLC/ja4/df3c067/technical_details/JA4.md#number-of-ciphers
     *# If there’s > 99, which there should never be, then output “99”.
     *
     *= https://raw.githubusercontent.com/FoxIO-LLC/ja4/df3c067/technical_details/JA4.md#number-of-extensions
     *# Same as counting ciphers.
     */
    count = MIN(count, 99);

    RESULT_ENSURE_EQ(output->size, 2);
    output->data[0] = (count / 10) + '0';
    output->data[1] = (count % 10) + '0';
    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_fingerprint_get_extension_version(struct s2n_client_hello *ch,
        uint16_t *client_version)
{
    RESULT_ENSURE_REF(ch);
    RESULT_ENSURE_REF(client_version);

    s2n_parsed_extension *extension = NULL;
    RESULT_GUARD_POSIX(s2n_client_hello_get_parsed_extension(
            S2N_EXTENSION_SUPPORTED_VERSIONS, &ch->extensions, &extension));
    RESULT_ENSURE_REF(extension);

    struct s2n_stuffer supported_versions = { 0 };
    RESULT_GUARD_POSIX(s2n_stuffer_init_written(&supported_versions, &extension->extension));

    RESULT_GUARD_POSIX(s2n_stuffer_skip_read(&supported_versions, sizeof(uint8_t)));
    while (s2n_stuffer_data_available(&supported_versions)) {
        uint16_t version = 0;
        RESULT_GUARD_POSIX(s2n_stuffer_read_uint16(&supported_versions, &version));
        /**
         *= https://raw.githubusercontent.com/FoxIO-LLC/ja4/df3c067/technical_details/JA4.md#tls-and-dtls-version
         *# Remember to ignore GREASE values.
         */
        if (s2n_fingerprint_is_grease_value(version)) {
            continue;
        }
        /**
         *= https://raw.githubusercontent.com/FoxIO-LLC/ja4/df3c067/technical_details/JA4.md#tls-and-dtls-version
         *# If extension 0x002b exists (supported_versions), then the version is
         *# the highest value in the extension.
         */
        *client_version = MAX(*client_version, version);
    }
    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_fingerprint_ja4_version(struct s2n_stuffer *output,
        struct s2n_client_hello *ch)
{
    uint16_t client_version = 0;
    if (s2n_result_is_error(s2n_fingerprint_get_extension_version(ch, &client_version))) {
        /**
         *= https://raw.githubusercontent.com/FoxIO-LLC/ja4/df3c067/technical_details/JA4.md#tls-and-dtls-version
         *# If the extension doesn’t exist, then the TLS version is the value of
         *# the Protocol Version.
         */
        RESULT_GUARD(s2n_fingerprint_get_legacy_version(ch, &client_version));
    }

    /**
     *= https://raw.githubusercontent.com/FoxIO-LLC/ja4/df3c067/technical_details/JA4.md#tls-and-dtls-version
     *# Handshake version (located at the top of the packet) should be ignored.
     */

    const char *version_str = NULL;
    if (client_version < s2n_array_len(s2n_ja4_version_strings)) {
        version_str = s2n_ja4_version_strings[client_version];
    }
    if (version_str == NULL) {
        version_str = S2N_JA4_UNKNOWN_STR;
    }
    RESULT_GUARD_POSIX(s2n_stuffer_write_str(output, version_str));

    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_client_hello_get_first_alpn(struct s2n_client_hello *ch, struct s2n_blob *first)
{
    RESULT_ENSURE_REF(ch);

    s2n_parsed_extension *extension = NULL;
    RESULT_GUARD_POSIX(s2n_client_hello_get_parsed_extension(S2N_EXTENSION_ALPN,
            &ch->extensions, &extension));
    RESULT_ENSURE_REF(extension);

    struct s2n_stuffer protocols = { 0 };
    RESULT_GUARD_POSIX(s2n_stuffer_init_written(&protocols, &extension->extension));

    uint16_t list_size = 0;
    RESULT_GUARD_POSIX(s2n_stuffer_read_uint16(&protocols, &list_size));

    RESULT_GUARD(s2n_protocol_preferences_read(&protocols, first));
    return S2N_RESULT_OK;
}

/**
 *= https://raw.githubusercontent.com/FoxIO-LLC/ja4/df3c067/technical_details/JA4.md#alpn-extension-value
 *# The first and last alphanumeric characters of the ALPN (Application-Layer
 *# Protocol Negotiation) first value.
 */
static S2N_RESULT s2n_fingerprint_ja4_alpn(struct s2n_stuffer *output,
        struct s2n_client_hello *ch)
{
    struct s2n_blob protocol = { 0 };
    if (s2n_result_is_error(s2n_client_hello_get_first_alpn(ch, &protocol))) {
        protocol.size = 0;
    }

    /**
     *= https://raw.githubusercontent.com/FoxIO-LLC/ja4/df3c067/technical_details/JA4.md#alpn-extension-value
     *# If there is no ALPN extension, no ALPN values, or the first ALPN value
     *# is empty, then we print "00" as the value in the fingerprint.
     *
     *= https://raw.githubusercontent.com/FoxIO-LLC/ja4/df3c067/technical_details/JA4.md#alpn-extension-value
     *# If the first ALPN value is only a single character, then that character
     *# is treated as both the first and last character.
     */
    uint8_t first_char = '0', last_char = '0';
    if (protocol.size > 0) {
        first_char = protocol.data[0];
        last_char = protocol.data[protocol.size - 1];
    }

    /**
     *= https://raw.githubusercontent.com/FoxIO-LLC/ja4/df3c067/technical_details/JA4.md#alpn-extension-value
     *# If the first or last byte of the first ALPN is non-alphanumeric (meaning
     *# not `0x30-0x39`, `0x41-0x5A`, or `0x61-0x7A`), then we print the first and
     *# last characters of the hex representation of the first ALPN instead.
     */
    if (!isalnum(first_char) || !isalnum(last_char)) {
        RESULT_GUARD(s2n_hex_digit((first_char >> 4), &first_char));
        RESULT_GUARD(s2n_hex_digit((last_char & 0x0F), &last_char));
    }

    RESULT_GUARD_POSIX(s2n_stuffer_write_char(output, first_char));
    RESULT_GUARD_POSIX(s2n_stuffer_write_char(output, last_char));
    return S2N_RESULT_OK;
}

/* Part "a" of the fingerprint is a descriptive prefix.
 *
 * https://github.com/FoxIO-LLC/ja4/main/technical_details/JA4.md
 *# (QUIC=”q”, DTLS="d", or Normal TLS=”t”)
 *# (2 character TLS version)
 *# (SNI=”d” or no SNI=”i”)
 *# (2 character count of ciphers)
 *# (2 character count of extensions)
 *# (first and last characters of first ALPN extension value)
 */
static S2N_RESULT s2n_fingerprint_ja4_a(struct s2n_fingerprint *fingerprint,
        struct s2n_stuffer *output, struct s2n_blob *ciphers_count, struct s2n_blob *extensions_count)
{
    RESULT_ENSURE_REF(fingerprint);

    /**
     *= https://raw.githubusercontent.com/FoxIO-LLC/ja4/df3c067/technical_details/JA4.md#quic-and-dtls
     *# If the protocol is QUIC then the first character of the fingerprint is “q”,
     *# if DTLS it is "d", else it is “t”.
     *
     * s2n-tls only supports TLS and QUIC. DTLS is not supported.
     */
    bool is_quic = false;
    RESULT_GUARD_POSIX(s2n_client_hello_has_extension(fingerprint->client_hello,
            TLS_EXTENSION_QUIC_TRANSPORT_PARAMETERS, &is_quic));
    char protocol_char = (is_quic) ? 'q' : 't';
    RESULT_GUARD_POSIX(s2n_stuffer_write_char(output, protocol_char));

    RESULT_GUARD(s2n_fingerprint_ja4_version(output, fingerprint->client_hello));

    /**
     *= https://raw.githubusercontent.com/FoxIO-LLC/ja4/df3c067/technical_details/JA4.md#sni
     *# If the SNI extension (0x0000) exists, then the destination of the connection
     *# is a domain, or “d” in the fingerprint.
     *# If the SNI does not exist, then the destination is an IP address, or “i”.
     */
    bool has_sni = false;
    RESULT_GUARD_POSIX(s2n_client_hello_has_extension(fingerprint->client_hello,
            TLS_EXTENSION_SERVER_NAME, &has_sni));
    char sni_char = (has_sni) ? 'd' : 'i';
    RESULT_GUARD_POSIX(s2n_stuffer_write_char(output, sni_char));

    /* Reserve two characters for the "count of ciphers".
     * We'll calculate it later when we handle the cipher suite list for JA4_b.
     */
    uint8_t *ciphers_count_mem = s2n_stuffer_raw_write(output, S2N_JA4_COUNT_SIZE);
    RESULT_GUARD_PTR(ciphers_count_mem);
    RESULT_GUARD_POSIX(s2n_blob_init(ciphers_count, ciphers_count_mem, S2N_JA4_COUNT_SIZE));

    /* Reserve two characters for the "count of extensions".
     * We'll calculate it later when we handle the extensions list for JA4_c.
     */
    uint8_t *extensions_count_mem = s2n_stuffer_raw_write(output, S2N_JA4_COUNT_SIZE);
    RESULT_GUARD_PTR(extensions_count_mem);
    RESULT_GUARD_POSIX(s2n_blob_init(extensions_count, extensions_count_mem, S2N_JA4_COUNT_SIZE));

    RESULT_GUARD(s2n_fingerprint_ja4_alpn(output, fingerprint->client_hello));

    return S2N_RESULT_OK;
}

/**
 *= https://raw.githubusercontent.com/FoxIO-LLC/ja4/df3c067/technical_details/JA4.md#cipher-hash
 *# The list is created using the 4 character hex values of the ciphers,
 *# lower case, comma delimited, ignoring GREASE.
 */
static S2N_RESULT s2n_fingerprint_ja4_ciphers(struct s2n_fingerprint_hash *hash,
        struct s2n_client_hello *ch, struct s2n_stuffer *sort_space, uint16_t *ciphers_count)
{
    RESULT_ENSURE_REF(ch);
    RESULT_ENSURE_REF(sort_space);
    RESULT_ENSURE_REF(ciphers_count);

    struct s2n_stuffer cipher_suites = { 0 };
    RESULT_GUARD_POSIX(s2n_stuffer_init_written(&cipher_suites, &ch->cipher_suites));

    DEFER_CLEANUP(struct s2n_stuffer *iana_list = sort_space, s2n_stuffer_wipe_pointer);
    while (s2n_stuffer_data_available(&cipher_suites)) {
        uint16_t iana = 0;
        RESULT_GUARD_POSIX(s2n_stuffer_read_uint16(&cipher_suites, &iana));
        /**
         *= https://raw.githubusercontent.com/FoxIO-LLC/ja4/df3c067/technical_details/JA4.md#number-of-ciphers
         *# Remember, ignore GREASE values. They don’t count.
         */
        if (s2n_fingerprint_is_grease_value(iana)) {
            continue;
        }
        RESULT_GUARD(s2n_stuffer_write_uint16_hex(iana_list, iana));
        RESULT_GUARD_POSIX(s2n_stuffer_write_char(iana_list, S2N_JA4_LIST_DIV));
    }

    size_t iana_list_size = s2n_stuffer_data_available(iana_list);
    size_t iana_count = iana_list_size / S2N_JA4_IANA_ENTRY_SIZE;
    *ciphers_count = iana_count;
    if (iana_count == 0) {
        return S2N_RESULT_OK;
    }

    uint8_t *ianas = s2n_stuffer_raw_read(iana_list, iana_list_size);
    RESULT_ENSURE_REF(ianas);
    qsort(ianas, iana_count, S2N_JA4_IANA_ENTRY_SIZE, s2n_fingerprint_ja4_iana_compare);
    RESULT_GUARD(s2n_fingerprint_hash_add_bytes(hash, ianas, iana_list_size - 1));
    return S2N_RESULT_OK;
}

/**
 *= https://raw.githubusercontent.com/FoxIO-LLC/ja4/df3c067/technical_details/JA4.md#cipher-hash
 *# A 12 character truncated sha256 hash of the list of ciphers sorted in hex order,
 *# first 12 characters.
 */
static S2N_RESULT s2n_fingerprint_ja4_b(struct s2n_fingerprint *fingerprint,
        struct s2n_fingerprint_hash *hash, struct s2n_blob *ciphers_count,
        struct s2n_stuffer *output)
{
    RESULT_ENSURE_REF(fingerprint);

    uint16_t ciphers_count_value = 0;
    RESULT_GUARD(s2n_fingerprint_ja4_ciphers(hash, fingerprint->client_hello,
            &fingerprint->workspace, &ciphers_count_value));

    RESULT_GUARD(s2n_fingerprint_ja4_digest(hash, output));
    RESULT_GUARD(s2n_fingerprint_ja4_count(ciphers_count, ciphers_count_value));
    return S2N_RESULT_OK;
}

/**
 *= https://raw.githubusercontent.com/FoxIO-LLC/ja4/df3c067/technical_details/JA4.md#extension-hash
 *# The extension list is created using the 4 character hex values of the extensions,
 *# lower case, comma delimited, sorted (not in the order they appear).
 */
static S2N_RESULT s2n_fingerprint_ja4_extensions(struct s2n_fingerprint_hash *hash,
        struct s2n_client_hello *ch, struct s2n_stuffer *sort_space, uint16_t *extensions_count)
{
    RESULT_ENSURE_REF(ch);
    RESULT_ENSURE_REF(sort_space);
    RESULT_ENSURE_REF(extensions_count);

    struct s2n_stuffer extensions = { 0 };
    RESULT_GUARD_POSIX(s2n_stuffer_init_written(&extensions, &ch->extensions.raw));

    DEFER_CLEANUP(struct s2n_stuffer *iana_list = sort_space, s2n_stuffer_wipe_pointer);
    while (s2n_stuffer_data_available(&extensions)) {
        uint16_t iana = 0;
        RESULT_GUARD(s2n_fingerprint_parse_extension(&extensions, &iana));

        /**
         *= https://raw.githubusercontent.com/FoxIO-LLC/ja4/df3c067/technical_details/JA4.md#number-of-extensions
         *# Ignore GREASE.
         */
        if (s2n_fingerprint_is_grease_value(iana)) {
            continue;
        }

        /* SNI and ALPN are included in the extension count, but not in the extension list.
         *
         *= https://raw.githubusercontent.com/FoxIO-LLC/ja4/df3c067/technical_details/JA4.md#extension-hash
         *# Ignore the SNI extension (0000) and the ALPN extension (0010)
         *# as we’ve already captured them in the _a_ section of the fingerprint.
         *
         *= https://raw.githubusercontent.com/FoxIO-LLC/ja4/df3c067/technical_details/JA4.md#number-of-extensions
         *# Include SNI and ALPN.
         */
        (*extensions_count)++;
        if (iana == TLS_EXTENSION_SERVER_NAME || iana == S2N_EXTENSION_ALPN) {
            continue;
        }
        RESULT_GUARD(s2n_stuffer_write_uint16_hex(iana_list, iana));
        RESULT_GUARD_POSIX(s2n_stuffer_write_char(iana_list, S2N_JA4_LIST_DIV));
    }

    size_t iana_list_size = s2n_stuffer_data_available(iana_list);
    size_t iana_count = iana_list_size / S2N_JA4_IANA_ENTRY_SIZE;
    if (iana_count == 0) {
        return S2N_RESULT_OK;
    }

    uint8_t *ianas = s2n_stuffer_raw_read(iana_list, iana_list_size);
    RESULT_ENSURE_REF(ianas);
    qsort(ianas, iana_count, S2N_JA4_IANA_ENTRY_SIZE, s2n_fingerprint_ja4_iana_compare);
    RESULT_GUARD(s2n_fingerprint_hash_add_bytes(hash, ianas, iana_list_size - 1));
    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_fingerprint_ja4_sig_algs(struct s2n_fingerprint_hash *hash,
        struct s2n_client_hello *ch)
{
    RESULT_ENSURE_REF(ch);

    s2n_parsed_extension *extension = NULL;
    int result = s2n_client_hello_get_parsed_extension(S2N_EXTENSION_SIGNATURE_ALGORITHMS,
            &ch->extensions, &extension);
    if (result != S2N_SUCCESS) {
        return S2N_RESULT_OK;
    }
    RESULT_ENSURE_REF(extension);

    struct s2n_stuffer sig_algs = { 0 };
    RESULT_GUARD_POSIX(s2n_stuffer_init_written(&sig_algs, &extension->extension));

    uint8_t entry_bytes[S2N_JA4_IANA_ENTRY_SIZE] = { 0 };
    struct s2n_stuffer entry = { 0 };
    RESULT_GUARD_POSIX(s2n_blob_init(&entry.blob, entry_bytes, sizeof(entry_bytes)));

    bool is_first = true;
    if (s2n_stuffer_skip_read(&sig_algs, sizeof(uint16_t)) != S2N_SUCCESS) {
        return S2N_RESULT_OK;
    }
    while (s2n_stuffer_data_available(&sig_algs)) {
        uint16_t iana = 0;
        RESULT_GUARD_POSIX(s2n_stuffer_read_uint16(&sig_algs, &iana));
        if (s2n_fingerprint_is_grease_value(iana)) {
            continue;
        }
        if (is_first) {
            RESULT_GUARD(s2n_fingerprint_hash_add_char(hash, S2N_JA4_PART_DIV));
        } else {
            RESULT_GUARD_POSIX(s2n_stuffer_write_char(&entry, S2N_JA4_LIST_DIV));
        }
        RESULT_GUARD(s2n_stuffer_write_uint16_hex(&entry, iana));
        RESULT_GUARD(s2n_fingerprint_hash_add_bytes(hash, entry_bytes,
                s2n_stuffer_data_available(&entry)));
        RESULT_GUARD_POSIX(s2n_stuffer_rewrite(&entry));
        is_first = false;
    }
    return S2N_RESULT_OK;
}

/**
 *= https://raw.githubusercontent.com/FoxIO-LLC/ja4/df3c067/technical_details/JA4.md#extension-hash
 *# A 12 character truncated sha256 hash of the list of extensions, sorted by
 *# hex value, followed by the list of signature algorithms, in the order that
 *# they appear (not sorted).
 */
static S2N_RESULT s2n_fingerprint_ja4_c(struct s2n_fingerprint *fingerprint,
        struct s2n_fingerprint_hash *hash, struct s2n_blob *extensions_count,
        struct s2n_stuffer *output)
{
    RESULT_ENSURE_REF(fingerprint);

    uint16_t extensions_count_value = 0;
    RESULT_GUARD(s2n_fingerprint_ja4_extensions(hash, fingerprint->client_hello,
            &fingerprint->workspace, &extensions_count_value));

    /**
     *= https://raw.githubusercontent.com/FoxIO-LLC/ja4/df3c067/technical_details/JA4.md#extension-hash
     *# The signature algorithm hex values are then added to the end of the list
     *# in the order that they appear (not sorted) with an underscore delimiting
     *# the two lists.
     *
     *= https://raw.githubusercontent.com/FoxIO-LLC/ja4/df3c067/technical_details/JA4.md#extension-hash
     *# If there are no signature algorithms in the hello packet,
     *# then the string ends without an underscore and is hashed.
     *
     * s2n_fingerprint_ja4_sig_algs handles writing the underscore because we
     * need to skip writing it if there are no signature algorithms.
     */
    RESULT_GUARD(s2n_fingerprint_ja4_sig_algs(hash, fingerprint->client_hello));

    RESULT_GUARD(s2n_fingerprint_ja4_digest(hash, output));
    RESULT_GUARD(s2n_fingerprint_ja4_count(extensions_count, extensions_count_value));
    return S2N_RESULT_OK;
}

/* JA4 fingerprints are basically of the form a_b_c:
 *
 *= https://raw.githubusercontent.com/FoxIO-LLC/ja4/df3c067/technical_details/JA4.md#ja4-algorithm
 *# (QUIC=”q”, DTLS="d", or Normal TLS=”t”)
 *# (2 character TLS version)
 *# (SNI=”d” or no SNI=”i”)
 *# (2 character count of ciphers)
 *# (2 character count of extensions)
 *# (first and last characters of first ALPN extension value)
 *# _
 *# (sha256 hash of the list of cipher hex codes sorted in hex order, truncated to 12 characters)
 *# _
 *# (sha256 hash of (the list of extension hex codes sorted in hex order)_(the list of signature algorithms), truncated to 12 characters)
 *#
 *# The end result is a fingerprint that looks like:
 *# t13d1516h2_8daaf6152771_b186095e22b6
 */
static S2N_RESULT s2n_fingerprint_ja4(struct s2n_fingerprint *fingerprint,
        struct s2n_fingerprint_hash *hash, struct s2n_stuffer *output)
{
    RESULT_ENSURE_REF(fingerprint);
    RESULT_ENSURE_REF(hash);
    RESULT_ENSURE_REF(output);

    if (s2n_stuffer_is_freed(&fingerprint->workspace)) {
        RESULT_GUARD_POSIX(s2n_stuffer_growable_alloc(&fingerprint->workspace, S2N_JA4_WORKSPACE_SIZE));
    }

    struct s2n_blob ciphers_count = { 0 };
    struct s2n_blob extensions_count = { 0 };
    RESULT_GUARD(s2n_fingerprint_ja4_a(fingerprint, output, &ciphers_count, &extensions_count));
    RESULT_GUARD_POSIX(s2n_stuffer_write_char(output, S2N_JA4_PART_DIV));
    RESULT_GUARD(s2n_fingerprint_ja4_b(fingerprint, hash, &ciphers_count, output));
    RESULT_GUARD_POSIX(s2n_stuffer_write_char(output, S2N_JA4_PART_DIV));
    RESULT_GUARD(s2n_fingerprint_ja4_c(fingerprint, hash, &extensions_count, output));

    if (s2n_fingerprint_hash_do_digest(hash)) {
        /* The extra two bytes are for the characters separating the parts */
        fingerprint->raw_size = hash->bytes_digested + S2N_JA4_A_SIZE + 2;
    } else {
        fingerprint->raw_size = s2n_stuffer_data_available(output);
    }

    return S2N_RESULT_OK;
}

struct s2n_fingerprint_method ja4_fingerprint = {
    .hash = S2N_HASH_SHA256,
    .hash_str_size = S2N_JA4_SIZE,
    .fingerprint = s2n_fingerprint_ja4,
};

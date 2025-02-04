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

#include <s2n.h>

/**
 * @file fingerprint.h
 *
 * The following APIs enable applications to calculate fingerprints to
 * identify ClientHellos.
 *
 * The fingerprinting APIs are currently considered unstable. They will be finalized
 * and marked as stable after an initial customer integration and feedback.
 */

/* Available fingerprinting methods.
 *
 * The current recommendation is to use JA4. JA4 sorts some of the lists it includes
 * in the fingerprint, making it more resistant to the list reordering done by
 * Chrome and other clients.
 */
typedef enum {
    /* See https://engineering.salesforce.com/tls-fingerprinting-with-ja3-and-ja3s-247362855967 */
    S2N_FINGERPRINT_JA3,
    /* See https://github.com/FoxIO-LLC/ja4/tree/main */
    S2N_FINGERPRINT_JA4,
} s2n_fingerprint_type;

struct s2n_fingerprint;

/**
 * Create a reusable fingerprint structure.
 *
 * Fingerprinting is primarily used to identify malicious or abusive clients,
 * so fingerprinting needs to be efficient and require minimal resources.
 * The `s2n_client_hello_get_fingerprint_hash` and `s2n_client_hello_get_fingerprint_string`
 * methods may require additional memory to calculate the fingerprint. Reusing
 * the same `s2n_fingerprint` structure to calculate multiple fingerprints reduces
 * the cost of each individual fingerprint.
 *
 * @param type The algorithm to use for the fingerprint.
 * @returns S2N_SUCCESS on success, S2N_FAILURE on failure.
 */
S2N_API struct s2n_fingerprint *s2n_fingerprint_new(s2n_fingerprint_type type);

/**
 * Frees the memory allocated by `s2n_fingerprint_new` for a fingerprint structure.
 *
 * @param fingerprint The s2n_fingerprint structure to be freed.
 * @returns S2N_SUCCESS on success, S2N_FAILURE on failure.
 */
S2N_API int s2n_fingerprint_free(struct s2n_fingerprint **fingerprint);

/**
 * Resets the fingerprint for safe reuse with a different ClientHello.
 *
 * @param fingerprint The s2n_fingerprint structure to be reset.
 * @returns S2N_SUCCESS on success, S2N_FAILURE on failure.
 */
S2N_API int s2n_fingerprint_wipe(struct s2n_fingerprint *fingerprint);

/**
 * Sets the ClientHello to be fingerprinted.
 *
 * @param fingerprint The s2n_fingerprint to be modified
 * @param ch The client hello to be fingerprinted. It will not be copied, so needs
 * to live at least as long as this fingerprinting operation.
 * @returns S2N_SUCCESS on success, S2N_FAILURE on failure.
 */
S2N_API int s2n_fingerprint_set_client_hello(struct s2n_fingerprint *fingerprint, struct s2n_client_hello *ch);

/**
 * Get the size of the fingerprint hash.
 *
 * Fingerprint hashes should be a constant size, but that size will vary based
 * on the fingerprinting method used.
 *
 * @param fingerprint The s2n_fingerprint to be used for the hash
 * @param size Output variable to be set to the size of the hash
 * @returns S2N_SUCCESS on success, S2N_FAILURE on failure.
 */
S2N_API int s2n_fingerprint_get_hash_size(const struct s2n_fingerprint *fingerprint, uint32_t *size);

/**
 * Calculates a fingerprint hash.
 *
 * The output of this method depends on the type of fingerprint.
 *
 * JA3: A hex-encoded string representing the MD5 hash of the raw string.
 * - See https://engineering.salesforce.com/tls-fingerprinting-with-ja3-and-ja3s-247362855967
 * - Example: "c34a54599a1fbaf1786aa6d633545a60"
 *
 * JA4: A string consisting of three parts, separated by underscores: the prefix,
 * and the hex-encoded truncated SHA256 hashes of the other two parts of the raw string.
 * - See https://github.com/FoxIO-LLC/ja4/blob/df3c067/technical_details/JA4.md
 * - Example: "t13i310900_e8f1e7e78f70_1f22a2ca17c4"
 *
 * @param fingerprint The s2n_fingerprint to be used for the hash
 * @param max_output_size The maximum size of data that may be written to `output`.
 * If `output` is too small, an S2N_ERR_T_USAGE error will occur.
 * @param output The location that the requested hash will be written to.
 * @param output_size Output variable to be set to the actual size of the data
 * written to `output`.
 * @returns S2N_SUCCESS on success, S2N_FAILURE on failure.
 */
S2N_API int s2n_fingerprint_get_hash(struct s2n_fingerprint *fingerprint,
        uint32_t max_output_size, uint8_t *output, uint32_t *output_size);

/**
 * Get the size of the raw fingerprint string.
 *
 * The size of the raw string depends on the ClientHello and cannot be known
 * without calculating the fingerprint. Either `s2n_fingerprint_get_hash` or
 * `s2n_fingerprint_get_raw` must be called before this method.
 *
 * @param fingerprint The s2n_fingerprint to be used for the raw string
 * @param size Output variable to be set to the size of the raw string
 * @returns S2N_SUCCESS on success, S2N_FAILURE on failure.
 */
S2N_API int s2n_fingerprint_get_raw_size(const struct s2n_fingerprint *fingerprint, uint32_t *size);

/**
 * Calculates the raw string for a fingerprint.
 *
 * The output of this method depends on the type of fingerprint.
 *
 * JA3: A string consisting of lists of decimal values.
 * - See https://engineering.salesforce.com/tls-fingerprinting-with-ja3-and-ja3s-247362855967
 * - Example: "771,4866-4867-4865-49196-49200-159-52393-52392-52394-49195-49199-158-
 *             49188-49192-107-49187-49191-103-49162-49172-57-49161-49171-51-157-
 *             156-61-60-53-47-255,11-10-35-22-23-13-43-45-51,29-23-30-25-24,0-1-2"
 *
 * JA4: A string consisting of three parts: a prefix, and two lists of hex values.
 * - See https://github.com/FoxIO-LLC/ja4/blob/df3c067/technical_details/JA4.md
 * - Example: "t13i310900_002f,0033,0035,0039,003c,003d,0067,006b,009c,009d,009e,
 *             009f,00ff,1301,1302,1303,c009,c00a,c013,c014,c023,c024,c027,c028,
 *             c02b,c02c,c02f,c030,cca8,cca9,ccaa_000a,000b,000d,0016,0017,0023,
 *             002b,002d,0033_0403,0503,0603,0807,0808,0809,080a,080b,0804,0805,
 *             0806,0401,0501,0601,0303,0301,0302,0402,0502,0602"
 *
 * @param fingerprint The s2n_fingerprint to be used for the raw string
 * @param max_output_size The maximum size of data that may be written to `output`.
 * If `output` is too small, an S2N_ERR_T_USAGE error will occur.
 * @param output The location that the requested raw string will be written to.
 * @param output_size Output variable to be set to the actual size of the data
 * written to `output`.
 * @returns S2N_SUCCESS on success, S2N_FAILURE on failure.
 */
S2N_API int s2n_fingerprint_get_raw(struct s2n_fingerprint *fingerprint,
        uint32_t max_output_size, uint8_t *output, uint32_t *output_size);

/**
 * Calculates a fingerprint hash for a given ClientHello.
 *
 * Currently the only type supported is S2N_FINGERPRINT_JA3, which uses MD5 and
 * requires at least 16 bytes of memory.
 *
 * @param ch The ClientHello to fingerprint.
 * @param type The algorithm to use for the fingerprint. Currently only JA3 is supported.
 * @param max_hash_size The maximum size of data that may be written to `hash`.
 * If too small for the requested hash, an S2N_ERR_T_USAGE error will occur.
 * @param hash The location that the requested hash will be written to.
 * @param hash_size The actual size of the data written to `hash`.
 * @param str_size The actual size of the full string associated with this hash.
 * This size can be used to ensure that sufficient memory is provided for the
 * output of `s2n_client_hello_get_fingerprint_string`.
 * @returns S2N_SUCCESS on success, S2N_FAILURE on failure.
 */
S2N_API int s2n_client_hello_get_fingerprint_hash(struct s2n_client_hello *ch,
        s2n_fingerprint_type type, uint32_t max_hash_size,
        uint8_t *hash, uint32_t *hash_size, uint32_t *str_size);

/**
 * Calculates a full, variable-length fingerprint string for a given ClientHello.
 *
 * Because the length of the string is variable and unknown until the string is
 * calculated, `s2n_client_hello_get_fingerprint_hash` can be called first to
 * determine `max_size` and ensure `output` is sufficiently large.
 *
 * @param ch The ClientHello to fingerprint.
 * @param type The algorithm to use for the fingerprint. Currently only JA3 is supported.
 * @param max_size The maximum size of data that may be written to `output`.
 * If too small for the requested string, an S2N_ERR_T_USAGE error will occur.
 * @param output The location that the requested string will be written to.
 * @param output_size The actual size of the data written to `output`.
 * @returns S2N_SUCCESS on success, S2N_FAILURE on failure.
 */
S2N_API int s2n_client_hello_get_fingerprint_string(struct s2n_client_hello *ch,
        s2n_fingerprint_type type, uint32_t max_size,
        uint8_t *output, uint32_t *output_size);

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

struct s2n_security_policy;

/**
 * Security policy definitions for common use cases.
 * These policies are versioned, with updates added as new versions.
 */
typedef enum {
    /**
     * A security policy that supports a wide variety of common options.
     * It is intended for use cases where the peer in a connection is unknown,
     * but assumed reasonably modern and standard.
     * If you are unsure which policy to choose, this is a safe choice.
     */
    S2N_POLICY_COMPATIBLE = 1,
    /**
     * A security policy that supports a narrow selection of the most preferred options.
     * It is intended for use cases where the peer is known to support that narrow
     * selection of options, usually because the same owner maintains both the clients
     * and servers involved in connections.
     *
     * NOT usable with legacy libcryptos that do not fully support TLS1.3,
     * like openssl-1.0.2.
     */
    S2N_POLICY_STRICT,
} s2n_policy_name;

typedef enum {
    /**
     * Supports only TLS1.3.
     * Supports post-quantum key exchange (MLKEM) and signatures (MLDSA).
     * Supports MLDSA, EC, and RSA certificates.
     * Supports only AES-GCM encryption.
     * Supports p256, p384, and p521 named groups.
     * Supports only SHA256 and higher signatures.
     * Supports only RSA-PSS padding for RSA signatures.
     * Supports forward secrecy.
     */
    S2N_STRICT_2025_08_20 = 1,
    /**
     * The latest version of S2N_POLICY_STRICT. Currently S2N_STRICT_2025_08_20.
     * Confirmed to successfully handshake with all previous versions of S2N_POLICY_STRICT,
     * as well as all previous versions of "default_tls13" and "default_pq".
     * If a breaking change is introduced to S2N_POLICY_STRICT, then `S2N_STRICT_LATEST_1`
     * will be frozen and a new `S2N_STRICT_LATEST_2` version will track the latest.
     */
    S2N_STRICT_LATEST_1 = S2N_STRICT_2025_08_20,
} s2n_strict_policy_version;

typedef enum {
    /**
     * Supports TLS1.2 and TLS1.3.
     * Supports post-quantum key exchange (MLKEM) and signatures (MLDSA).
     * Supports MLDSA, EC, and RSA certificates.
     * Supports AES-GCM, AES-CBC, and ChaChaPoly encryption.
     * Supports p256, x25519, p384, and p521 named groups.
     * Supports only SHA256 and higher signatures.
     * Supports forward secrecy.
     */
    S2N_COMPAT_2025_08_20 = 1,
} s2n_compat_policy_version;

/**
 * Retrieves a security policy by name and version.
 *
 * @param policy The s2n_policy_name defining a policy.
 * @param version The specific version of the policy.
 * @returns A static library security policy
 */
const struct s2n_security_policy *s2n_security_policy_get(s2n_policy_name policy, uint64_t version);

/**
 * Sets the security policy on the config.
 *
 * "security policies" were previously known as "cipher preferences".
 * See `s2n_config_set_cipher_preferences`.
 *
 * @param config The config object being updated
 * @param policy The security policy being set
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
int s2n_config_set_security_policy(struct s2n_config *config, const struct s2n_security_policy *policy);

/**
 * Sets an override security policy on the connection.
 *
 * "security policies" were previously known as "cipher preferences".
 * See `s2n_connection_set_cipher_preferences`.
 *
 * @param conn The connection object being updated
 * @param policy The security policy being set
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
int s2n_connection_set_security_policy(struct s2n_connection *conn, const struct s2n_security_policy *policy);

/**
 * Output format types for verbose policy output.
 */
typedef enum {
    /**
     * Produces structured output with the following sections:
     * - min version: <minimum_protocol_version>
     * - rules:
     *   - <rule_name>: <yes|no>
     * - cipher suites:
     *   - <cipher_suite_name>
     * - signature schemes:
     *   - <signature_scheme_name>
     * - curves:
     *   - <curve_name>
     * - certificate signature schemes: (if present)
     *   - <cert_signature_scheme_name>
     * - certificate keys: (if present)
     *   - <certificate_key_name>
     * - pq: (if present)
     *   - revision: <pq_hybrid_draft_revision>
     *   - kems: (if present)
     *     -- <kem_name>
     *   - kem groups:
     *     -- <kem_group_name>
     */
    S2N_POLICY_FORMAT_DEBUG_V1 = 1,
} s2n_policy_format;

/**
 * Retrieves the length of the buffer needed for s2n_security_policy_write_bytes().
 * This function should be used to allocate enough memory for the policy output buffer before calling
 * s2n_security_policy_write_bytes().
 *
 * @note The size of the policy output depends on the specific policy configuration.
 * Do not expect the size to always remain the same across different policies.
 *
 * @param policy The security policy to get the buffer size for
 * @param format The output format to use
 * @param length Output parameter where the required buffer length will be written
 * @returns S2N_SUCCESS on success, S2N_FAILURE on failure
 */
int s2n_security_policy_write_length(const struct s2n_security_policy *policy,
        s2n_policy_format format, uint32_t *length);

/**
 * Writes output of a security policy to a user-provided buffer in the specified format.
 * 
 * @param policy The security policy to output
 * @param format The output format to use
 * @param buffer The buffer to write to
 * @param buffer_length The size of the buffer
 * @param output_size Output variable to be set to the actual number of bytes written to `buffer`
 *                    This value is only meaningful when the function returns S2N_SUCCESS
 * @returns S2N_SUCCESS on success, S2N_FAILURE on failure (e.g., if buffer is too small)
 */
int s2n_security_policy_write_bytes(const struct s2n_security_policy *policy,
        s2n_policy_format format, uint8_t *buffer, uint32_t buffer_length, uint32_t *output_size);

/**
 * Writes output of a security policy to a file descriptor in the specified format.
 * 
 * @param policy The security policy to output
 * @param format The output format to use  
 * @param fd The file descriptor to write to (e.g., STDOUT_FILENO or an open file)
 * @param output_size Output variable to be set to the actual number of bytes written to the file descriptor
 *                    This value is only meaningful when the function returns S2N_SUCCESS
 * @returns S2N_SUCCESS on success, S2N_FAILURE on failure
 */
int s2n_security_policy_write_fd(const struct s2n_security_policy *policy,
        s2n_policy_format format, int fd, uint32_t *output_size);

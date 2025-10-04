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

#include "tls/s2n_tls13.h"

#include "api/s2n.h"
#include "crypto/s2n_rsa_pss.h"
#include "crypto/s2n_rsa_signing.h"
#include "tls/s2n_tls.h"

bool s2n_use_default_tls13_config_flag = false;

bool s2n_use_default_tls13_config()
{
    return s2n_use_default_tls13_config_flag;
}

bool s2n_is_tls13_fully_supported()
{
    /* Older versions of Openssl (eg 1.0.2) do not support RSA PSS, which is required for TLS 1.3. */
    return s2n_is_rsa_pss_signing_supported() && s2n_is_rsa_pss_certs_supported();
}

int s2n_get_highest_fully_supported_tls_version()
{
    return s2n_is_tls13_fully_supported() ? S2N_TLS13 : S2N_TLS12;
}

/* Allow TLS1.3 to be negotiated, and use the default TLS1.3 security policy.
 * This is NOT the default behavior, and this method is deprecated.
 *
 * Please consider using the default behavior and configuring
 * TLS1.2/TLS1.3 via explicit security policy instead.
 */
int s2n_enable_tls13()
{
    return s2n_enable_tls13_in_test();
}

/* Allow TLS1.3 to be negotiated, and use the default TLS1.3 security policy.
 * This is NOT the default behavior, and this method is deprecated.
 *
 * Please consider using the default behavior and configuring
 * TLS1.2/TLS1.3 via explicit security policy instead.
 */
int s2n_enable_tls13_in_test()
{
    s2n_highest_protocol_version = S2N_TLS13;
    s2n_use_default_tls13_config_flag = true;
    return S2N_SUCCESS;
}

/* Do NOT allow TLS1.3 to be negotiated, regardless of security policy.
 * This is NOT the default behavior, and this method is deprecated.
 *
 * Please consider using the default behavior and configuring
 * TLS1.2/TLS1.3 via explicit security policy instead.
 */
int s2n_disable_tls13_in_test()
{
    POSIX_ENSURE(s2n_in_unit_test(), S2N_ERR_NOT_IN_UNIT_TEST);
    s2n_highest_protocol_version = S2N_TLS12;
    s2n_use_default_tls13_config_flag = false;
    return S2N_SUCCESS;
}

/* Reset S2N to the default protocol version behavior.
 *
 * This method is intended for use in existing unit tests when the APIs
 * to enable/disable TLS1.3 have already been called.
 */
int s2n_reset_tls13_in_test()
{
    POSIX_ENSURE(s2n_in_unit_test(), S2N_ERR_NOT_IN_UNIT_TEST);
    s2n_highest_protocol_version = S2N_TLS13;
    s2n_use_default_tls13_config_flag = false;
    return S2N_SUCCESS;
}

/* Returns whether a uint16 iana value is a valid TLS 1.3 cipher suite */
bool s2n_is_valid_tls13_cipher(const uint8_t version[2])
{
    /* Valid TLS 1.3 Ciphers are
     * 0x1301, 0x1302, 0x1303, 0x1304, 0x1305.
     * (https://tools.ietf.org/html/rfc8446#appendix-B.4)
     */
    return version[0] == 0x13 && version[1] >= 0x01 && version[1] <= 0x05;
}

/* Use middlebox compatibility mode for TLS1.3 by default.
 * For now, only disable it when QUIC support is enabled.
 */
bool s2n_is_middlebox_compat_enabled(struct s2n_connection *conn)
{
    return s2n_connection_get_protocol_version(conn) >= S2N_TLS13
            && !s2n_connection_is_quic_enabled(conn);
}

S2N_RESULT s2n_connection_validate_tls13_support(struct s2n_connection *conn)
{
    RESULT_ENSURE_REF(conn);

    /* If the underlying libcrypto supports all features of TLS1.3
     * (including RSA-PSS, which is unsupported by some libraries),
     * then we can always support TLS1.3.
     */
    if (s2n_is_tls13_fully_supported()) {
        return S2N_RESULT_OK;
    }

    /*
     * If the underlying libcrypto doesn't support all features...
     */

    /* There are some TLS servers in the wild that will choose options not offered by the client.
     * So a server might choose to use RSA-PSS even if even if the client does not advertise support for RSA-PSS.
     * Therefore, only servers can perform TLS1.3 without full feature support.
     */
    RESULT_ENSURE(conn->mode == S2N_SERVER, S2N_ERR_RSA_PSS_NOT_SUPPORTED);

    /* RSA signatures must use RSA-PSS in TLS1.3.
     * So RSA-PSS is required for TLS1.3 servers if an RSA certificate is used.
     */
    RESULT_ENSURE(!conn->config->is_rsa_cert_configured, S2N_ERR_RSA_PSS_NOT_SUPPORTED);

    /* RSA-PSS is also required for TLS1.3 servers if client auth is requested, because the
     * client might offer an RSA certificate.
     */
    s2n_cert_auth_type client_auth_status = S2N_CERT_AUTH_NONE;
    RESULT_GUARD_POSIX(s2n_connection_get_client_auth_type(conn, &client_auth_status));
    RESULT_ENSURE(client_auth_status == S2N_CERT_AUTH_NONE, S2N_ERR_RSA_PSS_NOT_SUPPORTED);

    return S2N_RESULT_OK;
}

bool s2n_connection_supports_tls13(struct s2n_connection *conn)
{
    return s2n_result_is_ok(s2n_connection_validate_tls13_support(conn));
}

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

#include "tls/s2n_certificate_keys.h"
#include "tls/s2n_cipher_preferences.h"
#include "tls/s2n_ecc_preferences.h"
#include "tls/s2n_kem_preferences.h"
#include "tls/s2n_security_rules.h"
#include "tls/s2n_signature_scheme.h"

/* Kept up-to-date by s2n_security_policies_test */
#define NUM_RSA_PSS_SCHEMES 6

/* The s2n_security_policy struct is used to define acceptable and available
 * algorithms for use in the TLS protocol. Note that the behavior of each field
 * likely differs between different TLS versions, as the mechanics of cipher
 * negotiation often have significant differences between TLS versions.
 *
 * In s2n-tls, the signature_algorithms extension only applies to signatures in
 * CertificateVerify messages. To specify acceptable signature algorithms for
 * certificates the certificate_signature_preferences field should be set in the
 * security policy.
 */
struct s2n_security_policy {
    uint8_t minimum_protocol_version;
    /* TLS 1.0 - 1.2 - cipher preference includes multiple elements such
     * as signature algorithms, record algorithms, and key exchange algorithms
     * TLS 1.3 - cipher preference only determines record encryption
     */
    const struct s2n_cipher_preferences *cipher_preferences;
    /* kem_preferences is only used for Post-Quantum cryptography */
    const struct s2n_kem_preferences *kem_preferences;
    /* This field roughly corresponds to the "signature_algorithms" extension.
     * The client serializes this field of the security_policy to populate the
     * extension, and it is also used by the server to choose an appropriate
     * entry from the options supplied by the client.
     * TLS 1.2 - optional extension to specify signature algorithms other than
     * default: https://www.rfc-editor.org/rfc/rfc5246#section-7.4.1.4.1
     * TLS 1.3 - required extension specifying signature algorithms
    */
    const struct s2n_signature_preferences *signature_preferences;
    /* When this field is set, the endpoint will ensure that the signatures on
     * the certificates in the peer's certificate chain are in the specified
     * list. Note that s2n-tls does not support the signature_algorithms_cert
     * extension. Unlike the signature_preferences field, this information is
     * never transmitted to a peer.
    */
    const struct s2n_signature_preferences *certificate_signature_preferences;
    /* This field roughly corresponds to the information in the
     * "supported_groups" extension.
     * TLS 1.0 - 1.2 - "elliptic_curves" extension indicates supported groups
     * for both key exchange and signature algorithms.
     * TLS 1.3 - the "supported_groups" extension indicates the named groups
     * which the client supports for key exchange
     * https://www.rfc-editor.org/rfc/rfc8446#section-4.2.7
     */
    const struct s2n_ecc_preferences *ecc_preferences;
    /* This field determines what public keys are allowed for use. It restricts
     * both the type of the key (Elliptic Curve, RSA w/ Encryption, RSA PSS) and
     * the size of the key. Note that this field structure is likely to change
     * until https://github.com/aws/s2n-tls/issues/4435 is closed.
     */
    const struct s2n_certificate_key_preferences *certificate_key_preferences;
    /* This field controls whether the certificate_signature_preferences apply 
     * to local certs loaded on configs.
     */
    bool certificate_preferences_apply_locally;
    bool rules[S2N_SECURITY_RULES_COUNT];
};

struct s2n_security_policy_selection {
    const char *version;
    const struct s2n_security_policy *security_policy;
    unsigned ecc_extension_required : 1;
    unsigned pq_kem_extension_required : 1;
    unsigned supports_tls13 : 1;
};

extern struct s2n_security_policy_selection security_policy_selection[];

/* Defaults as of 05/24 */
extern const struct s2n_security_policy security_policy_20240501;
extern const struct s2n_security_policy security_policy_20240502;
extern const struct s2n_security_policy security_policy_20240503;

extern const struct s2n_security_policy security_policy_20241106;
extern const struct s2n_security_policy security_policy_20140601;
extern const struct s2n_security_policy security_policy_20141001;
extern const struct s2n_security_policy security_policy_20150202;
extern const struct s2n_security_policy security_policy_20150214;
extern const struct s2n_security_policy security_policy_20150306;
extern const struct s2n_security_policy security_policy_20160411;
extern const struct s2n_security_policy security_policy_20160804;
extern const struct s2n_security_policy security_policy_20160824;
extern const struct s2n_security_policy security_policy_20170210;
extern const struct s2n_security_policy security_policy_20170328;
extern const struct s2n_security_policy security_policy_20170328_gcm;
extern const struct s2n_security_policy security_policy_20170405;
extern const struct s2n_security_policy security_policy_20170405_gcm;
extern const struct s2n_security_policy security_policy_20170718;
extern const struct s2n_security_policy security_policy_20170718_gcm;
extern const struct s2n_security_policy security_policy_20190214;
extern const struct s2n_security_policy security_policy_20190214_gcm;
extern const struct s2n_security_policy security_policy_20190801;
extern const struct s2n_security_policy security_policy_20190802;
extern const struct s2n_security_policy security_policy_20230317;
extern const struct s2n_security_policy security_policy_20240331;
extern const struct s2n_security_policy security_policy_20240417;
extern const struct s2n_security_policy security_policy_20240416;
extern const struct s2n_security_policy security_policy_20240603;
extern const struct s2n_security_policy security_policy_20240730;
extern const struct s2n_security_policy security_policy_20241001;
extern const struct s2n_security_policy security_policy_20241001_pq_mixed;

extern const struct s2n_security_policy security_policy_rfc9151;
extern const struct s2n_security_policy security_policy_test_all;

extern const struct s2n_security_policy security_policy_test_all_tls12;
extern const struct s2n_security_policy security_policy_test_all_fips;
extern const struct s2n_security_policy security_policy_test_all_ecdsa;
extern const struct s2n_security_policy security_policy_test_ecdsa_priority;
extern const struct s2n_security_policy security_policy_test_all_rsa_kex;
extern const struct s2n_security_policy security_policy_test_all_tls13;

/* See https://docs.aws.amazon.com/elasticloadbalancing/latest/application/create-https-listener.html */
extern const struct s2n_security_policy security_policy_elb_2015_04;
extern const struct s2n_security_policy security_policy_elb_2016_08;
extern const struct s2n_security_policy security_policy_elb_tls_1_2_2017_01;
extern const struct s2n_security_policy security_policy_elb_tls_1_1_2017_01;
extern const struct s2n_security_policy security_policy_elb_tls_1_2_ext_2018_06;
extern const struct s2n_security_policy security_policy_elb_fs_2018_06;
extern const struct s2n_security_policy security_policy_elb_fs_1_2_2019_08;
extern const struct s2n_security_policy security_policy_elb_fs_1_1_2019_08;
extern const struct s2n_security_policy security_policy_elb_fs_1_2_res_2019_08;

extern const struct s2n_security_policy security_policy_aws_crt_sdk_ssl_v3;
extern const struct s2n_security_policy security_policy_aws_crt_sdk_tls_10;
extern const struct s2n_security_policy security_policy_aws_crt_sdk_tls_11;
extern const struct s2n_security_policy security_policy_aws_crt_sdk_tls_12;
extern const struct s2n_security_policy security_policy_aws_crt_sdk_tls_12_06_23;
extern const struct s2n_security_policy security_policy_aws_crt_sdk_tls_12_06_23_pq;
extern const struct s2n_security_policy security_policy_aws_crt_sdk_tls_13;

extern const struct s2n_security_policy security_policy_kms_pq_tls_1_0_2019_06;
extern const struct s2n_security_policy security_policy_kms_pq_tls_1_0_2020_02;
extern const struct s2n_security_policy security_policy_kms_pq_tls_1_0_2020_07;
extern const struct s2n_security_policy security_policy_pq_sike_test_tls_1_0_2019_11;
extern const struct s2n_security_policy security_policy_pq_sike_test_tls_1_0_2020_02;
extern const struct s2n_security_policy security_policy_pq_tls_1_0_2020_12;
extern const struct s2n_security_policy security_policy_pq_tls_1_1_2021_05_17;
extern const struct s2n_security_policy security_policy_pq_tls_1_0_2021_05_18;
extern const struct s2n_security_policy security_policy_pq_tls_1_0_2021_05_19;
extern const struct s2n_security_policy security_policy_pq_tls_1_0_2021_05_20;
extern const struct s2n_security_policy security_policy_pq_tls_1_1_2021_05_21;
extern const struct s2n_security_policy security_policy_pq_tls_1_0_2021_05_22;
extern const struct s2n_security_policy security_policy_pq_tls_1_0_2021_05_23;
extern const struct s2n_security_policy security_policy_pq_tls_1_0_2021_05_24;
extern const struct s2n_security_policy security_policy_pq_tls_1_0_2021_05_25;
extern const struct s2n_security_policy security_policy_pq_tls_1_0_2021_05_26;
extern const struct s2n_security_policy security_policy_pq_tls_1_0_2023_01_24;
extern const struct s2n_security_policy security_policy_pq_tls_1_2_2023_04_07;
extern const struct s2n_security_policy security_policy_pq_tls_1_2_2023_04_08;
extern const struct s2n_security_policy security_policy_pq_tls_1_2_2023_04_09;
extern const struct s2n_security_policy security_policy_pq_tls_1_2_2023_04_10;
extern const struct s2n_security_policy security_policy_pq_tls_1_3_2023_06_01;
extern const struct s2n_security_policy security_policy_pq_tls_1_2_2023_10_07;
extern const struct s2n_security_policy security_policy_pq_tls_1_2_2023_10_08;
extern const struct s2n_security_policy security_policy_pq_tls_1_2_2023_10_09;
extern const struct s2n_security_policy security_policy_pq_tls_1_2_2023_10_10;
extern const struct s2n_security_policy security_policy_pq_tls_1_2_2024_10_07;
extern const struct s2n_security_policy security_policy_pq_tls_1_2_2024_10_08;
extern const struct s2n_security_policy security_policy_pq_tls_1_2_2024_10_08_gcm;
extern const struct s2n_security_policy security_policy_pq_tls_1_2_2024_10_09;

extern const struct s2n_security_policy security_policy_cloudfront_upstream;
extern const struct s2n_security_policy security_policy_cloudfront_upstream_tls10;
extern const struct s2n_security_policy security_policy_cloudfront_upstream_tls12;
extern const struct s2n_security_policy security_policy_cloudfront_ssl_v_3;
extern const struct s2n_security_policy security_policy_cloudfront_tls_1_0_2014;
extern const struct s2n_security_policy security_policy_cloudfront_tls_1_0_2016;
extern const struct s2n_security_policy security_policy_cloudfront_tls_1_1_2016;
extern const struct s2n_security_policy security_policy_cloudfront_tls_1_2_2017;
extern const struct s2n_security_policy security_policy_cloudfront_tls_1_2_2018;
extern const struct s2n_security_policy security_policy_cloudfront_tls_1_2_2019;
extern const struct s2n_security_policy security_policy_cloudfront_tls_1_2_2021;
extern const struct s2n_security_policy security_policy_cloudfront_tls_1_2_2021_chacha20_boosted;

extern const struct s2n_security_policy security_policy_kms_tls_1_0_2018_10;
extern const struct s2n_security_policy security_policy_kms_tls_1_2_2023_06;
extern const struct s2n_security_policy security_policy_kms_fips_tls_1_2_2018_10;
extern const struct s2n_security_policy security_policy_kms_fips_tls_1_2_2024_10;

extern const struct s2n_security_policy security_policy_20190120;
extern const struct s2n_security_policy security_policy_20190121;
extern const struct s2n_security_policy security_policy_20190122;

extern const struct s2n_security_policy security_policy_null;

int s2n_security_policies_init();
int s2n_config_set_cipher_preferences(struct s2n_config *config, const char *version);
int s2n_connection_set_cipher_preferences(struct s2n_connection *conn, const char *version);
bool s2n_ecc_is_extension_required(const struct s2n_security_policy *security_policy);
bool s2n_pq_kem_is_extension_required(const struct s2n_security_policy *security_policy);
bool s2n_security_policy_supports_tls13(const struct s2n_security_policy *security_policy);
int s2n_find_security_policy_from_version(const char *version, const struct s2n_security_policy **security_policy);
int s2n_validate_kem_preferences(const struct s2n_kem_preferences *kem_preferences, bool pq_kem_extension_required);
S2N_RESULT s2n_validate_certificate_signature_preferences(const struct s2n_signature_preferences *s2n_certificate_signature_preferences);
S2N_RESULT s2n_security_policy_get_version(const struct s2n_security_policy *security_policy,
        const char **version);
/* Checks to see if a certificate has a signature algorithm that's in our 
 * certificate_signature_preferences list 
 */
S2N_RESULT s2n_security_policy_validate_certificate_chain(const struct s2n_security_policy *security_policy,
        const struct s2n_cert_chain_and_key *cert_key_pair);
S2N_RESULT s2n_security_policy_validate_cert_signature(
        const struct s2n_security_policy *security_policy, const struct s2n_cert_info *info, s2n_error error);
S2N_RESULT s2n_security_policy_validate_cert_key(
        const struct s2n_security_policy *security_policy, const struct s2n_cert_info *info, s2n_error error);

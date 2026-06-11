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

#include "tls/s2n_cipher_suites.h"
#include "tls/s2n_kem.h"
#include "tls/s2n_tls13.h"

struct s2n_cipher_preferences {
    uint16_t count;
    struct s2n_cipher_suite **suites;
    bool allow_chacha20_boosting;
};

extern const struct s2n_cipher_preferences cipher_preferences_20230317;
extern const struct s2n_cipher_preferences cipher_preferences_20240331;
extern const struct s2n_cipher_preferences cipher_preferences_20140601;
extern const struct s2n_cipher_preferences cipher_preferences_20141001;
extern const struct s2n_cipher_preferences cipher_preferences_20150202;
extern const struct s2n_cipher_preferences cipher_preferences_20150214;
extern const struct s2n_cipher_preferences cipher_preferences_20150306;
extern const struct s2n_cipher_preferences cipher_preferences_20160411;
extern const struct s2n_cipher_preferences cipher_preferences_20160804;
extern const struct s2n_cipher_preferences cipher_preferences_20160824;
extern const struct s2n_cipher_preferences cipher_preferences_20170210;
extern const struct s2n_cipher_preferences cipher_preferences_20170328;
extern const struct s2n_cipher_preferences cipher_preferences_20170328_gcm;
extern const struct s2n_cipher_preferences cipher_preferences_20170405;
extern const struct s2n_cipher_preferences cipher_preferences_20170405_gcm;
extern const struct s2n_cipher_preferences cipher_preferences_20170718;
extern const struct s2n_cipher_preferences cipher_preferences_20170718_gcm;
extern const struct s2n_cipher_preferences cipher_preferences_20190214;
extern const struct s2n_cipher_preferences cipher_preferences_20190214_gcm;
extern const struct s2n_cipher_preferences cipher_preferences_20190801;
extern const struct s2n_cipher_preferences cipher_preferences_20190120;
extern const struct s2n_cipher_preferences cipher_preferences_20190121;
extern const struct s2n_cipher_preferences cipher_preferences_20190122;
extern const struct s2n_cipher_preferences cipher_preferences_20210816;
extern const struct s2n_cipher_preferences cipher_preferences_20210816_gcm;
extern const struct s2n_cipher_preferences cipher_preferences_20210825;
extern const struct s2n_cipher_preferences cipher_preferences_20210825_gcm;
extern const struct s2n_cipher_preferences cipher_preferences_20210831;
extern const struct s2n_cipher_preferences cipher_preferences_20231213;
extern const struct s2n_cipher_preferences cipher_preferences_20231214;
extern const struct s2n_cipher_preferences cipher_preferences_20240603;
extern const struct s2n_cipher_preferences cipher_preferences_20241008;
extern const struct s2n_cipher_preferences cipher_preferences_20241008_gcm;
extern const struct s2n_cipher_preferences cipher_preferences_20241009;
extern const struct s2n_cipher_preferences cipher_preferences_20250211;
extern const struct s2n_cipher_preferences cipher_preferences_20250429;
extern const struct s2n_cipher_preferences cipher_preferences_20251013;
extern const struct s2n_cipher_preferences cipher_preferences_20251014;
extern const struct s2n_cipher_preferences cipher_preferences_20251015;
extern const struct s2n_cipher_preferences cipher_preferences_20251113;
extern const struct s2n_cipher_preferences cipher_preferences_20251114;
extern const struct s2n_cipher_preferences cipher_preferences_20251115;
extern const struct s2n_cipher_preferences cipher_preferences_20251116;
extern const struct s2n_cipher_preferences cipher_preferences_20251117;
extern const struct s2n_cipher_preferences cipher_preferences_20260220;

extern const struct s2n_cipher_preferences cipher_preferences_default_fips;

extern const struct s2n_cipher_preferences cipher_preferences_test_all;

extern const struct s2n_cipher_preferences cipher_preferences_test_all_tls12;
extern const struct s2n_cipher_preferences cipher_preferences_test_all_fips;
extern const struct s2n_cipher_preferences cipher_preferences_test_all_ecdsa;
extern const struct s2n_cipher_preferences cipher_preferences_test_ecdsa_priority;
extern const struct s2n_cipher_preferences cipher_preferences_test_all_rsa_kex;
extern const struct s2n_cipher_preferences cipher_preferences_test_all_tls13;

/* See https://docs.aws.amazon.com/elasticloadbalancing/latest/application/create-https-listener.html */
extern const struct s2n_cipher_preferences elb_security_policy_2015_04;
extern const struct s2n_cipher_preferences elb_security_policy_2016_08;

extern const struct s2n_cipher_preferences elb_security_policy_tls_1_1_2017_01;
extern const struct s2n_cipher_preferences elb_security_policy_tls_1_2_2017_01;
extern const struct s2n_cipher_preferences elb_security_policy_tls_1_2_ext_2018_06;

extern const struct s2n_cipher_preferences elb_security_policy_fs_2018_06;
extern const struct s2n_cipher_preferences elb_security_policy_fs_1_2_2019_08;
extern const struct s2n_cipher_preferences elb_security_policy_fs_1_1_2019_08;
extern const struct s2n_cipher_preferences elb_security_policy_fs_1_2_Res_2019_08;
extern const struct s2n_cipher_preferences elb_security_policy_tls13_1_2_Ext2_2021_06;

/* CloudFront upstream */
extern const struct s2n_cipher_preferences cipher_preferences_cloudfront_upstream;
extern const struct s2n_cipher_preferences cipher_preferences_cloudfront_upstream_tls10;
extern const struct s2n_cipher_preferences cipher_preferences_cloudfront_upstream_tls11;
extern const struct s2n_cipher_preferences cipher_preferences_cloudfront_upstream_tls12;
extern const struct s2n_cipher_preferences cipher_preferences_cloudfront_upstream_2025_08_08;
extern const struct s2n_cipher_preferences cipher_preferences_cloudfront_upstream_2025_08_08_tls13;
/* CloudFront viewer facing */
extern const struct s2n_cipher_preferences cipher_preferences_cloudfront_ssl_v_3;
extern const struct s2n_cipher_preferences cipher_preferences_cloudfront_tls_1_0_2014;
extern const struct s2n_cipher_preferences cipher_preferences_cloudfront_tls_1_0_2014_sha256;
extern const struct s2n_cipher_preferences cipher_preferences_cloudfront_tls_1_0_2016;
extern const struct s2n_cipher_preferences cipher_preferences_cloudfront_tls_1_1_2016;
extern const struct s2n_cipher_preferences cipher_preferences_cloudfront_tls_1_2_2017;
extern const struct s2n_cipher_preferences cipher_preferences_cloudfront_tls_1_2_2018;
extern const struct s2n_cipher_preferences cipher_preferences_cloudfront_tls_1_2_2018_beta;
extern const struct s2n_cipher_preferences cipher_preferences_cloudfront_tls_1_2_2025;
extern const struct s2n_cipher_preferences cipher_preferences_cloudfront_tls_1_3_2025;
/* CloudFront undocumented policies for testing */
extern const struct s2n_cipher_preferences cipher_preferences_cloudfront_tls_1_2_2019;
extern const struct s2n_cipher_preferences cipher_preferences_cloudfront_tls_1_2_2021;
extern const struct s2n_cipher_preferences cipher_preferences_cloudfront_tls_1_2_2021_chacha20_boosted;

/* CloudFront viewer facing legacy TLS 1.2 policies */
extern const struct s2n_cipher_preferences cipher_preferences_cloudfront_ssl_v_3_legacy;
extern const struct s2n_cipher_preferences cipher_preferences_cloudfront_tls_1_0_2014_legacy;
extern const struct s2n_cipher_preferences cipher_preferences_cloudfront_tls_1_0_2016_legacy;
extern const struct s2n_cipher_preferences cipher_preferences_cloudfront_tls_1_1_2016_legacy;
extern const struct s2n_cipher_preferences cipher_preferences_cloudfront_tls_1_2_2018_legacy;
extern const struct s2n_cipher_preferences cipher_preferences_cloudfront_tls_1_2_2019_legacy;

/* AWS Common Runtime Cipher Preferences */
extern const struct s2n_cipher_preferences cipher_preferences_aws_crt_sdk_ssl_v3;
extern const struct s2n_cipher_preferences cipher_preferences_aws_crt_sdk_default;
extern const struct s2n_cipher_preferences cipher_preferences_aws_crt_sdk_tls_13;
extern const struct s2n_cipher_preferences cipher_preferences_aws_crt_sdk_2025;

/* AWS KMS Cipher Preferences */
extern const struct s2n_cipher_preferences cipher_preferences_kms_tls_1_0_2018_10;
extern const struct s2n_cipher_preferences cipher_preferences_kms_tls_1_0_2021_08;
extern const struct s2n_cipher_preferences cipher_preferences_kms_fips_tls_1_2_2018_10;
extern const struct s2n_cipher_preferences cipher_preferences_kms_fips_tls_1_2_2021_08;

extern const struct s2n_cipher_preferences cipher_preferences_null;

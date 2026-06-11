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

#include "tls/s2n_security_policies.h"

#include "api/s2n.h"
#include "crypto/s2n_pq.h"
#include "tls/s2n_certificate_keys.h"
#include "tls/s2n_connection.h"
#include "tls/s2n_supported_group_preferences.h"
#include "utils/s2n_safety.h"

/* Default as of 10/13 */
const struct s2n_security_policy security_policy_20251014 = {
    .minimum_protocol_version = S2N_TLS12,
    .cipher_preferences = &cipher_preferences_20251014,
    .kem_preferences = &kem_preferences_pq_tls_1_3_ietf_2025_07,
    .signature_preferences = &s2n_signature_preferences_20240501,
    .ecc_preferences = &s2n_ecc_preferences_20240501,
    .rules = {
            [S2N_PERFECT_FORWARD_SECRECY] = true,
    },
};

/* FIPS default as of 10/13 */
const struct s2n_security_policy security_policy_20251015 = {
    .minimum_protocol_version = S2N_TLS12,
    .cipher_preferences = &cipher_preferences_20251015,
    .kem_preferences = &kem_preferences_pq_tls_1_3_ietf_2025_07,
    .signature_preferences = &s2n_signature_preferences_20240501,
    .certificate_signature_preferences = &s2n_certificate_signature_preferences_20201110,
    .ecc_preferences = &s2n_ecc_preferences_20201021,
    .rules = {
            [S2N_PERFECT_FORWARD_SECRECY] = true,
            [S2N_FIPS_140_3] = true,
    },
};

const struct s2n_security_policy security_policy_20240501 = {
    .minimum_protocol_version = S2N_TLS12,
    .cipher_preferences = &cipher_preferences_20240331,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20240501,
    .ecc_preferences = &s2n_ecc_preferences_20240501,
    .rules = {
            [S2N_PERFECT_FORWARD_SECRECY] = true,
    },
};

const struct s2n_security_policy security_policy_20240502 = {
    .minimum_protocol_version = S2N_TLS12,
    .cipher_preferences = &cipher_preferences_20240331,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20240501,
    .certificate_signature_preferences = &s2n_certificate_signature_preferences_20201110,
    .ecc_preferences = &s2n_ecc_preferences_20201021,
    .rules = {
            [S2N_PERFECT_FORWARD_SECRECY] = true,
            [S2N_FIPS_140_3] = true,
    },
};

/* TLS1.3 default as of 05/24 */
const struct s2n_security_policy security_policy_20240503 = {
    .minimum_protocol_version = S2N_TLS12,
    .cipher_preferences = &cipher_preferences_cloudfront_tls_1_2_2019,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20240501,
    .certificate_signature_preferences = &s2n_certificate_signature_preferences_20201110,
    .ecc_preferences = &s2n_ecc_preferences_20240501,
    .rules = {
            [S2N_PERFECT_FORWARD_SECRECY] = true,
    },
};

const struct s2n_security_policy security_policy_20241001 = {
    .minimum_protocol_version = S2N_TLS12,
    .cipher_preferences = &cipher_preferences_cloudfront_tls_1_2_2019,
    .kem_preferences = &kem_preferences_pq_tls_1_3_ietf_2024_10,
    .signature_preferences = &s2n_signature_preferences_20240501,
    .certificate_signature_preferences = &s2n_certificate_signature_preferences_20201110,
    .ecc_preferences = &s2n_ecc_preferences_20240501,
    .rules = {
            [S2N_PERFECT_FORWARD_SECRECY] = true,
    },
};

/* 20241001, but with ML-DSA added */
const struct s2n_security_policy security_policy_20250512 = {
    .minimum_protocol_version = S2N_TLS12,
    .cipher_preferences = &cipher_preferences_cloudfront_tls_1_2_2019,
    .kem_preferences = &kem_preferences_pq_tls_1_3_ietf_2024_10,
    .signature_preferences = &s2n_signature_preferences_20250512,
    .certificate_signature_preferences = &s2n_certificate_signature_preferences_20250512,
    .ecc_preferences = &s2n_ecc_preferences_20240501,
    .rules = {
            [S2N_PERFECT_FORWARD_SECRECY] = true,
    },
};

const struct s2n_security_policy security_policy_20250721 = {
    .minimum_protocol_version = S2N_TLS12,
    .cipher_preferences = &cipher_preferences_cloudfront_tls_1_2_2019,
    .kem_preferences = &kem_preferences_pq_tls_1_3_ietf_2025_07,
    .signature_preferences = &s2n_signature_preferences_20250512,
    .certificate_signature_preferences = &s2n_certificate_signature_preferences_20250512,
    .ecc_preferences = &s2n_ecc_preferences_20240501,
    .rules = {
            [S2N_PERFECT_FORWARD_SECRECY] = true,
    },
};

const struct s2n_security_policy security_policy_20241001_pq_mixed = {
    .minimum_protocol_version = S2N_TLS12,
    .cipher_preferences = &cipher_preferences_cloudfront_tls_1_2_2019,
    .kem_preferences = &kem_preferences_pq_tls_1_3_ietf_2024_10,
    .signature_preferences = &s2n_signature_preferences_20240501,
    .certificate_signature_preferences = &s2n_certificate_signature_preferences_20201110,
    .ecc_preferences = &s2n_ecc_preferences_20240501,
    .rules = {
            [S2N_PERFECT_FORWARD_SECRECY] = true,
    },
};

const struct s2n_security_policy security_policy_20240603 = {
    .minimum_protocol_version = S2N_TLS12,
    .cipher_preferences = &cipher_preferences_20240603,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20240501,
    .certificate_signature_preferences = &s2n_certificate_signature_preferences_20201110,
    .ecc_preferences = &s2n_ecc_preferences_20240603,
};

const struct s2n_security_policy security_policy_20170210 = {
    .minimum_protocol_version = S2N_TLS10,
    .cipher_preferences = &cipher_preferences_20170210,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20140601,
    .ecc_preferences = &s2n_ecc_preferences_20140601,
};

const struct s2n_security_policy security_policy_20240417 = {
    .minimum_protocol_version = S2N_TLS10,
    .cipher_preferences = &cipher_preferences_20210831,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20200207,
    .certificate_signature_preferences = &s2n_certificate_signature_preferences_20201110,
    .ecc_preferences = &s2n_ecc_preferences_20200310,
};

/*
 * This security policy is derived from the following specification:
 * https://nvlpubs.nist.gov/nistpubs/SpecialPublications/NIST.SP.800-52r2.pdf
 *
 * Supports TLS1.2
 */
const struct s2n_security_policy security_policy_20240416 = {
    .minimum_protocol_version = S2N_TLS12,
    .cipher_preferences = &cipher_preferences_default_fips,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_default_fips,
    .certificate_signature_preferences = &s2n_signature_preferences_default_fips,
    .ecc_preferences = &s2n_ecc_preferences_default_fips,
    .rules = {
            [S2N_PERFECT_FORWARD_SECRECY] = true,
            [S2N_FIPS_140_3] = true,
    },
};

const struct s2n_security_policy security_policy_20230317 = {
    .minimum_protocol_version = S2N_TLS12,
    .cipher_preferences = &cipher_preferences_20230317,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20230317,
    .certificate_signature_preferences = &s2n_signature_preferences_20230317,
    .ecc_preferences = &s2n_ecc_preferences_20201021,
    .rules = {
            [S2N_PERFECT_FORWARD_SECRECY] = true,
            [S2N_FIPS_140_3] = true,
    },
};

const struct s2n_security_policy security_policy_20240331 = {
    .minimum_protocol_version = S2N_TLS12,
    .cipher_preferences = &cipher_preferences_20240331,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20230317,
    .certificate_signature_preferences = &s2n_signature_preferences_20230317,
    .ecc_preferences = &s2n_ecc_preferences_20201021,
    .rules = {
            [S2N_PERFECT_FORWARD_SECRECY] = true,
            [S2N_FIPS_140_3] = true,
    },
};

const struct s2n_security_policy security_policy_20190801 = {
    .minimum_protocol_version = S2N_TLS10,
    .cipher_preferences = &cipher_preferences_20190801,
    .kem_preferences = &kem_preferences_null,
    /* The discrepancy in the date exists because the signature preferences
     * were named when cipher preferences and signature preferences were
     * tracked separately, and we chose to keep the cipher preference
     * name because customers use it.
     */
    .signature_preferences = &s2n_signature_preferences_20200207,
    .ecc_preferences = &s2n_ecc_preferences_20200310,
};

const struct s2n_security_policy security_policy_20190802 = {
    .minimum_protocol_version = S2N_TLS10,
    .cipher_preferences = &cipher_preferences_20190801,
    .kem_preferences = &kem_preferences_null,
    /* The discrepancy in the date exists because the signature preferences
     * were named when cipher preferences and signature preferences were
     * tracked separately, and we chose to keep the cipher preference
     * name because customers use it.
     */
    .signature_preferences = &s2n_signature_preferences_20200207,
    .ecc_preferences = &s2n_ecc_preferences_20140601,
};

const struct s2n_security_policy security_policy_20170405 = {
    .minimum_protocol_version = S2N_TLS10,
    .cipher_preferences = &cipher_preferences_20170405,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20140601,
    .ecc_preferences = &s2n_ecc_preferences_20140601,
};

const struct s2n_security_policy security_policy_20170405_gcm = {
    .minimum_protocol_version = S2N_TLS10,
    .cipher_preferences = &cipher_preferences_20170405_gcm,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20140601,
    .ecc_preferences = &s2n_ecc_preferences_20140601,
};

const struct s2n_security_policy security_policy_elb_2015_04 = {
    .minimum_protocol_version = S2N_TLS10,
    .cipher_preferences = &elb_security_policy_2015_04,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20140601,
    .ecc_preferences = &s2n_ecc_preferences_20140601,
};

const struct s2n_security_policy security_policy_elb_2016_08 = {
    .minimum_protocol_version = S2N_TLS10,
    .cipher_preferences = &elb_security_policy_2016_08,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20140601,
    .ecc_preferences = &s2n_ecc_preferences_20140601,
};

const struct s2n_security_policy security_policy_elb_tls_1_1_2017_01 = {
    .minimum_protocol_version = S2N_TLS11,
    .cipher_preferences = &elb_security_policy_tls_1_1_2017_01,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20140601,
    .ecc_preferences = &s2n_ecc_preferences_20140601,
};

const struct s2n_security_policy security_policy_elb_tls_1_2_2017_01 = {
    .minimum_protocol_version = S2N_TLS12,
    .cipher_preferences = &elb_security_policy_tls_1_2_2017_01,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20140601,
    .ecc_preferences = &s2n_ecc_preferences_20140601,
};

const struct s2n_security_policy security_policy_elb_tls_1_2_ext_2018_06 = {
    .minimum_protocol_version = S2N_TLS12,
    .cipher_preferences = &elb_security_policy_tls_1_2_ext_2018_06,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20140601,
    .ecc_preferences = &s2n_ecc_preferences_20140601,
};

const struct s2n_security_policy security_policy_elb_fs_2018_06 = {
    .minimum_protocol_version = S2N_TLS10,
    .cipher_preferences = &elb_security_policy_fs_2018_06,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20140601,
    .ecc_preferences = &s2n_ecc_preferences_20140601,
    .rules = {
            [S2N_PERFECT_FORWARD_SECRECY] = true,
    },
};

const struct s2n_security_policy security_policy_elb_fs_1_2_2019_08 = {
    .minimum_protocol_version = S2N_TLS12,
    .cipher_preferences = &elb_security_policy_fs_1_2_2019_08,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20140601,
    .ecc_preferences = &s2n_ecc_preferences_20140601,
    .rules = {
            [S2N_PERFECT_FORWARD_SECRECY] = true,
    },
};

const struct s2n_security_policy security_policy_elb_fs_1_1_2019_08 = {
    .minimum_protocol_version = S2N_TLS11,
    .cipher_preferences = &elb_security_policy_fs_1_1_2019_08,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20140601,
    .ecc_preferences = &s2n_ecc_preferences_20140601,
    .rules = {
            [S2N_PERFECT_FORWARD_SECRECY] = true,
    },
};

const struct s2n_security_policy security_policy_elb_fs_1_2_Res_2019_08 = {
    .minimum_protocol_version = S2N_TLS12,
    .cipher_preferences = &elb_security_policy_fs_1_2_Res_2019_08,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20140601,
    .ecc_preferences = &s2n_ecc_preferences_20140601,
    .rules = {
            [S2N_PERFECT_FORWARD_SECRECY] = true,
    },
};

/* CloudFront upstream */
const struct s2n_security_policy security_policy_cloudfront_upstream = {
    .minimum_protocol_version = S2N_SSLv3,
    .cipher_preferences = &cipher_preferences_cloudfront_upstream,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20140601,
    .ecc_preferences = &s2n_ecc_preferences_20230623,
};

const struct s2n_security_policy security_policy_cloudfront_upstream_tls10 = {
    .minimum_protocol_version = S2N_TLS10,
    .cipher_preferences = &cipher_preferences_cloudfront_upstream_tls10,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20140601,
    .ecc_preferences = &s2n_ecc_preferences_20230623,
};

const struct s2n_security_policy security_policy_cloudfront_upstream_tls11 = {
    .minimum_protocol_version = S2N_TLS11,
    .cipher_preferences = &cipher_preferences_cloudfront_upstream_tls11,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20140601,
    .ecc_preferences = &s2n_ecc_preferences_20230623,
};

const struct s2n_security_policy security_policy_cloudfront_upstream_tls12 = {
    .minimum_protocol_version = S2N_TLS12,
    .cipher_preferences = &cipher_preferences_cloudfront_upstream_tls12,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20140601,
    .ecc_preferences = &s2n_ecc_preferences_20230623,
};

/* CloudFront upstream 2025 -- same as original upstream above, but with:
 * 1. TLSv1.3 enabled and
 * 2. signature preferences updated to 2020-10-21, expanding support for RSA
 *    PSS while preserving support for legacy signature algorithms
 */
const struct s2n_security_policy security_policy_cloudfront_upstream_2025_08_08 = {
    .minimum_protocol_version = S2N_SSLv3,
    .cipher_preferences = &cipher_preferences_cloudfront_upstream_2025_08_08,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20250820,
    .ecc_preferences = &s2n_ecc_preferences_20230623,
};

const struct s2n_security_policy security_policy_cloudfront_upstream_2025_08_08_tls10 = {
    .minimum_protocol_version = S2N_TLS10,
    .cipher_preferences = &cipher_preferences_cloudfront_upstream_2025_08_08,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20250820,
    .ecc_preferences = &s2n_ecc_preferences_20230623,
};

const struct s2n_security_policy security_policy_cloudfront_upstream_2025_08_08_tls11 = {
    .minimum_protocol_version = S2N_TLS11,
    .cipher_preferences = &cipher_preferences_cloudfront_upstream_2025_08_08,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20250820,
    .ecc_preferences = &s2n_ecc_preferences_20230623,
};

const struct s2n_security_policy security_policy_cloudfront_upstream_2025_08_08_tls12 = {
    .minimum_protocol_version = S2N_TLS12,
    .cipher_preferences = &cipher_preferences_cloudfront_upstream_2025_08_08,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20250820,
    .ecc_preferences = &s2n_ecc_preferences_20230623,
};

const struct s2n_security_policy security_policy_cloudfront_upstream_2025_08_08_tls13 = {
    .minimum_protocol_version = S2N_TLS13,
    .cipher_preferences = &cipher_preferences_cloudfront_upstream_2025_08_08_tls13,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20250820,
    .ecc_preferences = &s2n_ecc_preferences_20230623,
};

const struct s2n_security_policy security_policy_cloudfront_upstream_2025_08_08_pq = {
    .minimum_protocol_version = S2N_SSLv3,
    .cipher_preferences = &cipher_preferences_cloudfront_upstream_2025_08_08,
    .kem_preferences = &kem_preferences_pq_tls_1_3_ietf_2025_07,
    .signature_preferences = &s2n_signature_preferences_20250821,
    .ecc_preferences = &s2n_ecc_preferences_20230623,
};

const struct s2n_security_policy security_policy_cloudfront_upstream_2025_08_08_tls10_pq = {
    .minimum_protocol_version = S2N_TLS10,
    .cipher_preferences = &cipher_preferences_cloudfront_upstream_2025_08_08,
    .kem_preferences = &kem_preferences_pq_tls_1_3_ietf_2025_07,
    .signature_preferences = &s2n_signature_preferences_20250821,
    .ecc_preferences = &s2n_ecc_preferences_20230623,
};

const struct s2n_security_policy security_policy_cloudfront_upstream_2025_08_08_tls11_pq = {
    .minimum_protocol_version = S2N_TLS11,
    .cipher_preferences = &cipher_preferences_cloudfront_upstream_2025_08_08,
    .kem_preferences = &kem_preferences_pq_tls_1_3_ietf_2025_07,
    .signature_preferences = &s2n_signature_preferences_20250821,
    .ecc_preferences = &s2n_ecc_preferences_20230623,
};

const struct s2n_security_policy security_policy_cloudfront_upstream_2025_08_08_tls12_pq = {
    .minimum_protocol_version = S2N_TLS12,
    .cipher_preferences = &cipher_preferences_cloudfront_upstream_2025_08_08,
    .kem_preferences = &kem_preferences_pq_tls_1_3_ietf_2025_07,
    .signature_preferences = &s2n_signature_preferences_20250821,
    .ecc_preferences = &s2n_ecc_preferences_20230623,
};

const struct s2n_security_policy security_policy_cloudfront_upstream_2025_08_08_tls13_pq = {
    .minimum_protocol_version = S2N_TLS13,
    .cipher_preferences = &cipher_preferences_cloudfront_upstream_2025_08_08_tls13,
    .kem_preferences = &kem_preferences_pq_tls_1_3_ietf_2025_07,
    .signature_preferences = &s2n_signature_preferences_20250821,
    .ecc_preferences = &s2n_ecc_preferences_20230623,
};

/* CloudFront viewer facing */
const struct s2n_security_policy security_policy_cloudfront_ssl_v_3 = {
    .minimum_protocol_version = S2N_SSLv3,
    .cipher_preferences = &cipher_preferences_cloudfront_ssl_v_3,
    .kem_preferences = &kem_preferences_pq_tls_1_3_ietf_2024_10,
    .signature_preferences = &s2n_signature_preferences_20200207,
    .ecc_preferences = &s2n_ecc_preferences_20200310,
};

const struct s2n_security_policy security_policy_cloudfront_tls_1_0_2014 = {
    .minimum_protocol_version = S2N_TLS10,
    .cipher_preferences = &cipher_preferences_cloudfront_tls_1_0_2014,
    .kem_preferences = &kem_preferences_pq_tls_1_3_ietf_2024_10,
    .signature_preferences = &s2n_signature_preferences_20200207,
    .ecc_preferences = &s2n_ecc_preferences_20200310,
};

/* Same as security_policy_cloudfront_tls_1_0_2014, but with IETF standard KEM Groups  */
const struct s2n_security_policy security_policy_cloudfront_tls_1_0_2014_pq_beta = {
    .minimum_protocol_version = S2N_TLS10,
    .cipher_preferences = &cipher_preferences_cloudfront_tls_1_0_2014,
    .kem_preferences = &kem_preferences_pq_tls_1_3_ietf_2024_10,
    .signature_preferences = &s2n_signature_preferences_20200207,
    .ecc_preferences = &s2n_ecc_preferences_20200310,
};

const struct s2n_security_policy security_policy_cloudfront_tls_1_0_2014_sha256 = {
    .minimum_protocol_version = S2N_TLS10,
    .cipher_preferences = &cipher_preferences_cloudfront_tls_1_0_2014_sha256,
    .kem_preferences = &kem_preferences_pq_tls_1_3_ietf_2024_10,
    .signature_preferences = &s2n_signature_preferences_20200207,
    .ecc_preferences = &s2n_ecc_preferences_20200310,
};

const struct s2n_security_policy security_policy_cloudfront_tls_1_0_2016 = {
    .minimum_protocol_version = S2N_TLS10,
    .cipher_preferences = &cipher_preferences_cloudfront_tls_1_0_2016,
    .kem_preferences = &kem_preferences_pq_tls_1_3_ietf_2024_10,
    .signature_preferences = &s2n_signature_preferences_20200207,
    .ecc_preferences = &s2n_ecc_preferences_20200310,
};

/* Same as security_policy_cloudfront_tls_1_0_2016, but with TLS 1.2 as minimum */
const struct s2n_security_policy security_policy_20241106 = {
    .minimum_protocol_version = S2N_TLS12,
    .cipher_preferences = &cipher_preferences_cloudfront_tls_1_0_2016,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20200207,
    .ecc_preferences = &s2n_ecc_preferences_20200310,
};

const struct s2n_security_policy security_policy_cloudfront_tls_1_1_2016 = {
    .minimum_protocol_version = S2N_TLS11,
    .cipher_preferences = &cipher_preferences_cloudfront_tls_1_1_2016,
    .kem_preferences = &kem_preferences_pq_tls_1_3_ietf_2024_10,
    .signature_preferences = &s2n_signature_preferences_20200207,
    .ecc_preferences = &s2n_ecc_preferences_20200310,
};

const struct s2n_security_policy security_policy_cloudfront_tls_1_2_2017 = {
    .minimum_protocol_version = S2N_TLS12,
    .cipher_preferences = &cipher_preferences_cloudfront_tls_1_2_2017,
    .kem_preferences = &kem_preferences_pq_tls_1_3_ietf_2024_10,
    .signature_preferences = &s2n_signature_preferences_20200207,
    .ecc_preferences = &s2n_ecc_preferences_20200310,
};

const struct s2n_security_policy security_policy_cloudfront_tls_1_2_2018_no_sha1 = {
    .minimum_protocol_version = S2N_TLS12,
    .cipher_preferences = &cipher_preferences_cloudfront_tls_1_2_2018,
    .kem_preferences = &kem_preferences_pq_tls_1_3_ietf_2024_10,
    .signature_preferences = &s2n_signature_preferences_20200207_no_sha1,
    .ecc_preferences = &s2n_ecc_preferences_20200310,
};

const struct s2n_security_policy security_policy_cloudfront_tls_1_2_2018_beta = {
    .minimum_protocol_version = S2N_TLS12,
    .cipher_preferences = &cipher_preferences_cloudfront_tls_1_2_2018_beta,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20200207,
    .ecc_preferences = &s2n_ecc_preferences_20200310,
};

const struct s2n_security_policy security_policy_cloudfront_tls_1_2_2019_no_sha1 = {
    .minimum_protocol_version = S2N_TLS12,
    .cipher_preferences = &cipher_preferences_cloudfront_tls_1_2_2019,
    .kem_preferences = &kem_preferences_pq_tls_1_3_ietf_2024_10,
    .signature_preferences = &s2n_signature_preferences_20200207_no_sha1,
    .ecc_preferences = &s2n_ecc_preferences_20200310,
    .rules = {
            [S2N_PERFECT_FORWARD_SECRECY] = true,
    },
};

const struct s2n_security_policy security_policy_cloudfront_tls_1_2_2021_no_sha1 = {
    .minimum_protocol_version = S2N_TLS12,
    .cipher_preferences = &cipher_preferences_cloudfront_tls_1_2_2021,
    .kem_preferences = &kem_preferences_pq_tls_1_3_ietf_2024_10,
    .signature_preferences = &s2n_signature_preferences_20200207_no_sha1,
    .ecc_preferences = &s2n_ecc_preferences_20200310,
    .rules = {
            [S2N_PERFECT_FORWARD_SECRECY] = true,
    },
};

/* Same as security_policy_cloudfront_tls_1_2_2021_no_sha1, but with IETF standard KEM Groups  */
const struct s2n_security_policy security_policy_cloudfront_tls_1_2_2021_no_sha1_pq_beta = {
    .minimum_protocol_version = S2N_TLS12,
    .cipher_preferences = &cipher_preferences_cloudfront_tls_1_2_2021,
    .kem_preferences = &kem_preferences_pq_tls_1_3_ietf_2024_10,
    .signature_preferences = &s2n_signature_preferences_20200207_no_sha1,
    .ecc_preferences = &s2n_ecc_preferences_20200310,
    .rules = {
            [S2N_PERFECT_FORWARD_SECRECY] = true,
    },
};

const struct s2n_security_policy security_policy_cloudfront_tls_1_2_2021_chacha20_boosted = {
    .minimum_protocol_version = S2N_TLS12,
    .cipher_preferences = &cipher_preferences_cloudfront_tls_1_2_2021_chacha20_boosted,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20200207,
    .ecc_preferences = &s2n_ecc_preferences_20200310,
    .rules = {
            [S2N_PERFECT_FORWARD_SECRECY] = true,
    },
};

/* FIPS 140-3 compliant version of security_policy_cloudfront_tls_1_2_2021 */
const struct s2n_security_policy security_policy_cloudfront_tls_1_2_2025 = {
    .minimum_protocol_version = S2N_TLS12,
    .cipher_preferences = &cipher_preferences_cloudfront_tls_1_2_2025,
    .kem_preferences = &kem_preferences_pq_tls_1_3_ietf_2024_10,
    .signature_preferences = &s2n_signature_preferences_20250813,
    .ecc_preferences = &s2n_ecc_preferences_default_fips,
    .rules = {
            [S2N_PERFECT_FORWARD_SECRECY] = true,
            [S2N_FIPS_140_3] = true,
    },
};

const struct s2n_security_policy security_policy_cloudfront_tls_1_3_2025 = {
    .minimum_protocol_version = S2N_TLS13,
    .cipher_preferences = &cipher_preferences_cloudfront_tls_1_3_2025,
    .kem_preferences = &kem_preferences_pq_tls_1_3_ietf_2024_10,
    .signature_preferences = &s2n_signature_preferences_20250813,
    .ecc_preferences = &s2n_ecc_preferences_20200310,
    .rules = {
            [S2N_PERFECT_FORWARD_SECRECY] = true,
    },
};

/* CloudFront non-pq viewer facing policies */
const struct s2n_security_policy security_policy_cloudfront_ssl_v_3_no_pq = {
    .minimum_protocol_version = S2N_SSLv3,
    .cipher_preferences = &cipher_preferences_cloudfront_ssl_v_3,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20200207,
    .ecc_preferences = &s2n_ecc_preferences_20200310,
};

const struct s2n_security_policy security_policy_cloudfront_tls_1_0_2014_no_pq = {
    .minimum_protocol_version = S2N_TLS10,
    .cipher_preferences = &cipher_preferences_cloudfront_tls_1_0_2014,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20200207,
    .ecc_preferences = &s2n_ecc_preferences_20200310,
};

const struct s2n_security_policy security_policy_cloudfront_tls_1_0_2014_sha256_no_pq = {
    .minimum_protocol_version = S2N_TLS10,
    .cipher_preferences = &cipher_preferences_cloudfront_tls_1_0_2014_sha256,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20200207,
    .ecc_preferences = &s2n_ecc_preferences_20200310,
};

const struct s2n_security_policy security_policy_cloudfront_tls_1_0_2016_no_pq = {
    .minimum_protocol_version = S2N_TLS10,
    .cipher_preferences = &cipher_preferences_cloudfront_tls_1_0_2016,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20200207,
    .ecc_preferences = &s2n_ecc_preferences_20200310,
};

const struct s2n_security_policy security_policy_cloudfront_tls_1_1_2016_no_pq = {
    .minimum_protocol_version = S2N_TLS11,
    .cipher_preferences = &cipher_preferences_cloudfront_tls_1_1_2016,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20200207,
    .ecc_preferences = &s2n_ecc_preferences_20200310,
};

const struct s2n_security_policy security_policy_cloudfront_tls_1_2_2017_no_pq = {
    .minimum_protocol_version = S2N_TLS12,
    .cipher_preferences = &cipher_preferences_cloudfront_tls_1_2_2017,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20200207,
    .ecc_preferences = &s2n_ecc_preferences_20200310,
};

const struct s2n_security_policy security_policy_cloudfront_tls_1_2_2018_no_sha1_no_pq = {
    .minimum_protocol_version = S2N_TLS12,
    .cipher_preferences = &cipher_preferences_cloudfront_tls_1_2_2018,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20200207_no_sha1,
    .ecc_preferences = &s2n_ecc_preferences_20200310,
};

const struct s2n_security_policy security_policy_cloudfront_tls_1_2_2019_no_sha1_no_pq = {
    .minimum_protocol_version = S2N_TLS12,
    .cipher_preferences = &cipher_preferences_cloudfront_tls_1_2_2019,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20200207_no_sha1,
    .ecc_preferences = &s2n_ecc_preferences_20200310,
    .rules = {
            [S2N_PERFECT_FORWARD_SECRECY] = true,
    },
};

const struct s2n_security_policy security_policy_cloudfront_tls_1_2_2021_no_sha1_no_pq = {
    .minimum_protocol_version = S2N_TLS12,
    .cipher_preferences = &cipher_preferences_cloudfront_tls_1_2_2021,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20200207_no_sha1,
    .ecc_preferences = &s2n_ecc_preferences_20200310,
    .rules = {
            [S2N_PERFECT_FORWARD_SECRECY] = true,
    },
};

const struct s2n_security_policy security_policy_cloudfront_tls_1_2_2025_no_pq = {
    .minimum_protocol_version = S2N_TLS12,
    .cipher_preferences = &cipher_preferences_cloudfront_tls_1_2_2025,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20250813,
    .ecc_preferences = &s2n_ecc_preferences_default_fips,
    .rules = {
            [S2N_PERFECT_FORWARD_SECRECY] = true,
            [S2N_FIPS_140_3] = true,
    },
};

const struct s2n_security_policy security_policy_cloudfront_tls_1_3_2025_no_pq = {
    .minimum_protocol_version = S2N_TLS13,
    .cipher_preferences = &cipher_preferences_cloudfront_tls_1_3_2025,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20250813,
    .ecc_preferences = &s2n_ecc_preferences_20200310,
    .rules = {
            [S2N_PERFECT_FORWARD_SECRECY] = true,
    },
};

/* CloudFront viewer facing legacy policies */
const struct s2n_security_policy security_policy_cloudfront_tls_1_2_2018 = {
    .minimum_protocol_version = S2N_TLS12,
    .cipher_preferences = &cipher_preferences_cloudfront_tls_1_2_2018,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20200207,
    .ecc_preferences = &s2n_ecc_preferences_20200310,
};

const struct s2n_security_policy security_policy_cloudfront_tls_1_2_2019 = {
    .minimum_protocol_version = S2N_TLS12,
    .cipher_preferences = &cipher_preferences_cloudfront_tls_1_2_2019,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20200207,
    .ecc_preferences = &s2n_ecc_preferences_20200310,
    .rules = {
            [S2N_PERFECT_FORWARD_SECRECY] = true,
    },
};

const struct s2n_security_policy security_policy_cloudfront_tls_1_2_2021 = {
    .minimum_protocol_version = S2N_TLS12,
    .cipher_preferences = &cipher_preferences_cloudfront_tls_1_2_2021,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20200207,
    .ecc_preferences = &s2n_ecc_preferences_20200310,
    .rules = {
            [S2N_PERFECT_FORWARD_SECRECY] = true,
    },
};

const struct s2n_security_policy security_policy_cloudfront_ssl_v_3_legacy = {
    .minimum_protocol_version = S2N_SSLv3,
    .cipher_preferences = &cipher_preferences_cloudfront_ssl_v_3_legacy,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20140601,
    .ecc_preferences = &s2n_ecc_preferences_20140601,
};

const struct s2n_security_policy security_policy_cloudfront_tls_1_0_2014_legacy = {
    .minimum_protocol_version = S2N_TLS10,
    .cipher_preferences = &cipher_preferences_cloudfront_tls_1_0_2014_legacy,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20140601,
    .ecc_preferences = &s2n_ecc_preferences_20140601,
};

const struct s2n_security_policy security_policy_cloudfront_tls_1_0_2016_legacy = {
    .minimum_protocol_version = S2N_TLS10,
    .cipher_preferences = &cipher_preferences_cloudfront_tls_1_0_2016_legacy,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20140601,
    .ecc_preferences = &s2n_ecc_preferences_20140601,
};

const struct s2n_security_policy security_policy_cloudfront_tls_1_1_2016_legacy = {
    .minimum_protocol_version = S2N_TLS11,
    .cipher_preferences = &cipher_preferences_cloudfront_tls_1_1_2016_legacy,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20140601,
    .ecc_preferences = &s2n_ecc_preferences_20140601,
};

const struct s2n_security_policy security_policy_cloudfront_tls_1_2_2018_legacy = {
    .minimum_protocol_version = S2N_TLS12,
    .cipher_preferences = &cipher_preferences_cloudfront_tls_1_2_2018_legacy,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20140601,
    .ecc_preferences = &s2n_ecc_preferences_20140601,
};

const struct s2n_security_policy security_policy_cloudfront_tls_1_2_2019_legacy = {
    .minimum_protocol_version = S2N_TLS12,
    .cipher_preferences = &cipher_preferences_cloudfront_tls_1_2_2019_legacy,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20140601,
    .ecc_preferences = &s2n_ecc_preferences_20140601,
    .rules = {
            [S2N_PERFECT_FORWARD_SECRECY] = true,
    },
};

const struct s2n_security_policy security_policy_aws_crt_sdk_ssl_v3 = {
    .minimum_protocol_version = S2N_SSLv3,
    .cipher_preferences = &cipher_preferences_aws_crt_sdk_ssl_v3,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20200207,
    .ecc_preferences = &s2n_ecc_preferences_20200310,
};

const struct s2n_security_policy security_policy_aws_crt_sdk_tls_10 = {
    .minimum_protocol_version = S2N_TLS10,
    .cipher_preferences = &cipher_preferences_aws_crt_sdk_default,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20200207,
    .ecc_preferences = &s2n_ecc_preferences_20200310,
};

const struct s2n_security_policy security_policy_aws_crt_sdk_tls_11 = {
    .minimum_protocol_version = S2N_TLS11,
    .cipher_preferences = &cipher_preferences_aws_crt_sdk_default,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20200207,
    .ecc_preferences = &s2n_ecc_preferences_20200310,
};

const struct s2n_security_policy security_policy_aws_crt_sdk_tls_12 = {
    .minimum_protocol_version = S2N_TLS12,
    .cipher_preferences = &cipher_preferences_aws_crt_sdk_default,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20200207,
    .ecc_preferences = &s2n_ecc_preferences_20200310,
};

const struct s2n_security_policy security_policy_aws_crt_sdk_tls_13 = {
    .minimum_protocol_version = S2N_TLS13,
    .cipher_preferences = &cipher_preferences_aws_crt_sdk_tls_13,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20200207,
    .ecc_preferences = &s2n_ecc_preferences_20200310,
    .rules = {
            [S2N_PERFECT_FORWARD_SECRECY] = true,
    },
};

const struct s2n_security_policy security_policy_aws_crt_sdk_ssl_v3_06_23 = {
    .minimum_protocol_version = S2N_SSLv3,
    .cipher_preferences = &cipher_preferences_aws_crt_sdk_ssl_v3,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20200207,
    .ecc_preferences = &s2n_ecc_preferences_20230623,
};

const struct s2n_security_policy security_policy_aws_crt_sdk_tls_10_06_23 = {
    .minimum_protocol_version = S2N_TLS10,
    .cipher_preferences = &cipher_preferences_aws_crt_sdk_default,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20200207,
    .ecc_preferences = &s2n_ecc_preferences_20230623,
};

const struct s2n_security_policy security_policy_aws_crt_sdk_tls_11_06_23 = {
    .minimum_protocol_version = S2N_TLS11,
    .cipher_preferences = &cipher_preferences_aws_crt_sdk_default,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20200207,
    .ecc_preferences = &s2n_ecc_preferences_20230623,
};

const struct s2n_security_policy security_policy_aws_crt_sdk_tls_12_06_23 = {
    .minimum_protocol_version = S2N_TLS12,
    .cipher_preferences = &cipher_preferences_aws_crt_sdk_default,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20200207,
    .ecc_preferences = &s2n_ecc_preferences_20230623,
};

const struct s2n_security_policy security_policy_aws_crt_sdk_tls_30_06_25 = {
    .minimum_protocol_version = S2N_TLS12,
    .cipher_preferences = &cipher_preferences_aws_crt_sdk_2025,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20240501,
    .ecc_preferences = &s2n_ecc_preferences_20140601,
    .rules = {
            [S2N_PERFECT_FORWARD_SECRECY] = true,
            [S2N_FIPS_140_3] = true,
    },
};

const struct s2n_security_policy security_policy_aws_crt_sdk_tls_13_06_23 = {
    .minimum_protocol_version = S2N_TLS13,
    .cipher_preferences = &cipher_preferences_aws_crt_sdk_tls_13,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20200207,
    .ecc_preferences = &s2n_ecc_preferences_20230623,
    .rules = {
            [S2N_PERFECT_FORWARD_SECRECY] = true,
    },
};

const struct s2n_security_policy security_policy_kms_tls_1_0_2018_10 = {
    .minimum_protocol_version = S2N_TLS10,
    .cipher_preferences = &cipher_preferences_kms_tls_1_0_2018_10,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20140601,
    .ecc_preferences = &s2n_ecc_preferences_20140601,
    .rules = {
            [S2N_PERFECT_FORWARD_SECRECY] = true,
    },
};

const struct s2n_security_policy security_policy_kms_tls_1_0_2021_08 = {
    .minimum_protocol_version = S2N_TLS10,
    .cipher_preferences = &cipher_preferences_kms_tls_1_0_2021_08,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20200207,
    .ecc_preferences = &s2n_ecc_preferences_20200310,
    .rules = {
            [S2N_PERFECT_FORWARD_SECRECY] = true,
    },
};

const struct s2n_security_policy security_policy_kms_tls_1_2_2023_06 = {
    .minimum_protocol_version = S2N_TLS12,
    .cipher_preferences = &cipher_preferences_kms_tls_1_0_2021_08,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20200207,
    .ecc_preferences = &s2n_ecc_preferences_20200310,
    .rules = {
            [S2N_PERFECT_FORWARD_SECRECY] = true,
    },
};

/* Same as security_policy_aws_crt_sdk_tls_10_06_23 but with (IETF-standardized) ML-KEM Support */
const struct s2n_security_policy security_policy_aws_crt_sdk_tls_10_07_25_pq = {
    .minimum_protocol_version = S2N_TLS10,
    .cipher_preferences = &cipher_preferences_aws_crt_sdk_default,
    .kem_preferences = &kem_preferences_all,
    .signature_preferences = &s2n_signature_preferences_20200207,
    .ecc_preferences = &s2n_ecc_preferences_20230623,
};

/* Same as security_policy_aws_crt_sdk_tls_12_06_23 but with (IETF-standardized) ML-KEM Support */
const struct s2n_security_policy security_policy_aws_crt_sdk_tls_12_07_25_pq = {
    .minimum_protocol_version = S2N_TLS12,
    .cipher_preferences = &cipher_preferences_aws_crt_sdk_default,
    .kem_preferences = &kem_preferences_all,
    .signature_preferences = &s2n_signature_preferences_20200207,
    .ecc_preferences = &s2n_ecc_preferences_20230623,
};

/* Same as security_policy_aws_crt_sdk_tls_13_06_23 but with (IETF-standardized) ML-KEM Support */
const struct s2n_security_policy security_policy_aws_crt_sdk_tls_13_07_25_pq = {
    .minimum_protocol_version = S2N_TLS13,
    .cipher_preferences = &cipher_preferences_aws_crt_sdk_tls_13,
    .kem_preferences = &kem_preferences_all,
    .signature_preferences = &s2n_signature_preferences_20200207,
    .ecc_preferences = &s2n_ecc_preferences_20230623,
};

/* Same as security_policy_aws_crt_sdk_tls_12_06_23 but with ML-KEM Support */
const struct s2n_security_policy security_policy_aws_crt_sdk_tls_12_06_23_pq = {
    .minimum_protocol_version = S2N_TLS12,
    .cipher_preferences = &cipher_preferences_aws_crt_sdk_default,
    .kem_preferences = &kem_preferences_all,
    .signature_preferences = &s2n_signature_preferences_20200207,
    .ecc_preferences = &s2n_ecc_preferences_20230623,
};

/* Same as security_policy_pq_tls_1_2_2023_10_07, but with ML-KEM support */
const struct s2n_security_policy security_policy_pq_tls_1_2_2024_10_07 = {
    .minimum_protocol_version = S2N_TLS12,
    .cipher_preferences = &elb_security_policy_tls13_1_2_Ext2_2021_06,
    .kem_preferences = &kem_preferences_pq_tls_1_3_ietf_2024_10,
    .signature_preferences = &s2n_signature_preferences_20200207,
    .ecc_preferences = &s2n_ecc_preferences_20200310,
};

/* Same as security_policy_pq_tls_1_2_2023_10_08, but with 3DES  removed, and added ML-KEM support */
const struct s2n_security_policy security_policy_pq_tls_1_2_2024_10_08 = {
    .minimum_protocol_version = S2N_TLS12,
    .cipher_preferences = &cipher_preferences_20241008,
    .kem_preferences = &kem_preferences_pq_tls_1_3_ietf_2024_10,
    .signature_preferences = &s2n_signature_preferences_20200207,
    .ecc_preferences = &s2n_ecc_preferences_20200310,
};

/* Same as security_policy_pq_tls_1_2_2023_10_10, but with 3DES removed, and added ML-KEM support */
const struct s2n_security_policy security_policy_pq_tls_1_2_2024_10_08_gcm = {
    .minimum_protocol_version = S2N_TLS12,
    .cipher_preferences = &cipher_preferences_20241008_gcm,
    .kem_preferences = &kem_preferences_pq_tls_1_3_ietf_2024_10,
    .signature_preferences = &s2n_signature_preferences_20200207,
    .ecc_preferences = &s2n_ecc_preferences_20200310,
};

/* Same as security_policy_pq_tls_1_2_2023_10_09 but with 3DES removed, and added ML-KEM support */
const struct s2n_security_policy security_policy_pq_tls_1_2_2024_10_09 = {
    .minimum_protocol_version = S2N_TLS12,
    .cipher_preferences = &cipher_preferences_20241009,
    .kem_preferences = &kem_preferences_pq_tls_1_3_ietf_2024_10,
    .signature_preferences = &s2n_signature_preferences_20200207,
    .ecc_preferences = &s2n_ecc_preferences_20200310,
    .rules = {
            [S2N_PERFECT_FORWARD_SECRECY] = true,
    },
};
const struct s2n_security_policy security_policy_kms_fips_tls_1_2_2018_10 = {
    .minimum_protocol_version = S2N_TLS12,
    .cipher_preferences = &cipher_preferences_kms_fips_tls_1_2_2018_10,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20140601,
    .ecc_preferences = &s2n_ecc_preferences_20140601,
    .rules = {
            [S2N_PERFECT_FORWARD_SECRECY] = true,
    },
};

const struct s2n_security_policy security_policy_kms_fips_tls_1_2_2021_08 = {
    .minimum_protocol_version = S2N_TLS12,
    .cipher_preferences = &cipher_preferences_kms_fips_tls_1_2_2021_08,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20200207,
    .ecc_preferences = &s2n_ecc_preferences_20140601,
    .rules = {
            [S2N_PERFECT_FORWARD_SECRECY] = true,
    },
};

/* Same as security_policy_pq_20231215, but with only ML-KEM Support */
const struct s2n_security_policy security_policy_kms_fips_tls_1_2_2024_10 = {
    .minimum_protocol_version = S2N_TLS12,
    .cipher_preferences = &cipher_preferences_kms_fips_tls_1_2_2021_08,
    .kem_preferences = &kem_preferences_pq_tls_1_3_ietf_2024_10,
    .signature_preferences = &s2n_signature_preferences_20230317,
    .ecc_preferences = &s2n_ecc_preferences_20201021,
    .rules = {
            [S2N_PERFECT_FORWARD_SECRECY] = true,
            [S2N_FIPS_140_3] = true,
    },
};

const struct s2n_security_policy security_policy_20140601 = {
    .minimum_protocol_version = S2N_SSLv3,
    .cipher_preferences = &cipher_preferences_20140601,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20140601,
    .ecc_preferences = &s2n_ecc_preferences_20140601,
};

const struct s2n_security_policy security_policy_20141001 = {
    .minimum_protocol_version = S2N_TLS10,
    .cipher_preferences = &cipher_preferences_20141001,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20140601,
    .ecc_preferences = &s2n_ecc_preferences_20140601,
};

const struct s2n_security_policy security_policy_20150202 = {
    .minimum_protocol_version = S2N_TLS10,
    .cipher_preferences = &cipher_preferences_20150202,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20140601,
    .ecc_preferences = &s2n_ecc_preferences_20140601,
};

const struct s2n_security_policy security_policy_20150214 = {
    .minimum_protocol_version = S2N_TLS10,
    .cipher_preferences = &cipher_preferences_20150214,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20140601,
    .ecc_preferences = &s2n_ecc_preferences_20140601,
};

const struct s2n_security_policy security_policy_20160411 = {
    .minimum_protocol_version = S2N_TLS10,
    .cipher_preferences = &cipher_preferences_20160411,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20140601,
    .ecc_preferences = &s2n_ecc_preferences_20140601,
};

const struct s2n_security_policy security_policy_20150306 = {
    .minimum_protocol_version = S2N_TLS10,
    .cipher_preferences = &cipher_preferences_20150306,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20140601,
    .ecc_preferences = &s2n_ecc_preferences_20140601,
};

const struct s2n_security_policy security_policy_20160804 = {
    .minimum_protocol_version = S2N_TLS10,
    .cipher_preferences = &cipher_preferences_20160804,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20140601,
    .ecc_preferences = &s2n_ecc_preferences_20140601,
};

const struct s2n_security_policy security_policy_20160824 = {
    .minimum_protocol_version = S2N_TLS10,
    .cipher_preferences = &cipher_preferences_20160824,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20140601,
    .ecc_preferences = &s2n_ecc_preferences_20140601,
};

const struct s2n_security_policy security_policy_20190122 = {
    .minimum_protocol_version = S2N_TLS10,
    .cipher_preferences = &cipher_preferences_20190122,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20140601,
    .ecc_preferences = &s2n_ecc_preferences_20140601,
};

const struct s2n_security_policy security_policy_20190121 = {
    .minimum_protocol_version = S2N_TLS10,
    .cipher_preferences = &cipher_preferences_20190121,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20140601,
    .ecc_preferences = &s2n_ecc_preferences_20140601,
};

const struct s2n_security_policy security_policy_20190120 = {
    .minimum_protocol_version = S2N_TLS10,
    .cipher_preferences = &cipher_preferences_20190120,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20140601,
    .ecc_preferences = &s2n_ecc_preferences_20140601,
};

const struct s2n_security_policy security_policy_20190214 = {
    .minimum_protocol_version = S2N_TLS10,
    .cipher_preferences = &cipher_preferences_20190214,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20140601,
    .ecc_preferences = &s2n_ecc_preferences_20140601,
};

const struct s2n_security_policy security_policy_20190214_gcm = {
    .minimum_protocol_version = S2N_TLS10,
    .cipher_preferences = &cipher_preferences_20190214_gcm,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20140601,
    .ecc_preferences = &s2n_ecc_preferences_20140601,
};

const struct s2n_security_policy security_policy_20210825 = {
    .minimum_protocol_version = S2N_TLS10,
    .cipher_preferences = &cipher_preferences_20210825,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20200207,
    .ecc_preferences = &s2n_ecc_preferences_20200310,
};

const struct s2n_security_policy security_policy_20210825_gcm = {
    .minimum_protocol_version = S2N_TLS10,
    .cipher_preferences = &cipher_preferences_20210825_gcm,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20200207,
    .ecc_preferences = &s2n_ecc_preferences_20200310,
};

const struct s2n_security_policy security_policy_20170328 = {
    .minimum_protocol_version = S2N_TLS10,
    .cipher_preferences = &cipher_preferences_20170328,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20140601,
    .ecc_preferences = &s2n_ecc_preferences_20140601,
};

const struct s2n_security_policy security_policy_20170328_gcm = {
    .minimum_protocol_version = S2N_TLS10,
    .cipher_preferences = &cipher_preferences_20170328_gcm,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20140601,
    .ecc_preferences = &s2n_ecc_preferences_20140601,
};

const struct s2n_security_policy security_policy_20170718 = {
    .minimum_protocol_version = S2N_TLS10,
    .cipher_preferences = &cipher_preferences_20170718,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20140601,
    .ecc_preferences = &s2n_ecc_preferences_20140601,
};

const struct s2n_security_policy security_policy_20170718_gcm = {
    .minimum_protocol_version = S2N_TLS10,
    .cipher_preferences = &cipher_preferences_20170718_gcm,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20140601,
    .ecc_preferences = &s2n_ecc_preferences_20140601,
};

const struct s2n_security_policy security_policy_20201021 = {
    .minimum_protocol_version = S2N_TLS10,
    .cipher_preferences = &cipher_preferences_20190122,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20201021,
    .ecc_preferences = &s2n_ecc_preferences_20201021,
};

const struct s2n_security_policy security_policy_20210816 = {
    .minimum_protocol_version = S2N_TLS12,
    .cipher_preferences = &cipher_preferences_20210816,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20210816,
    .ecc_preferences = &s2n_ecc_preferences_20210816,
    .rules = {
            [S2N_PERFECT_FORWARD_SECRECY] = true,
            [S2N_FIPS_140_3] = true,
    },
};

const struct s2n_security_policy security_policy_20210816_gcm = {
    .minimum_protocol_version = S2N_TLS12,
    .cipher_preferences = &cipher_preferences_20210816_gcm,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20210816,
    .ecc_preferences = &s2n_ecc_preferences_20210816,
    .rules = {
            [S2N_PERFECT_FORWARD_SECRECY] = true,
            [S2N_FIPS_140_3] = true,
    },
};

/*
 * This security policy is derived from the following specification:
 * https://datatracker.ietf.org/doc/html/rfc9151
 */
const struct s2n_security_policy security_policy_20250429 = {
    .minimum_protocol_version = S2N_TLS12,
    .cipher_preferences = &cipher_preferences_20250429,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20250429,
    .certificate_signature_preferences = &s2n_certificate_signature_preferences_20250429,
    .certificate_key_preferences = &s2n_certificate_key_preferences_20250429,
    .ecc_preferences = &s2n_ecc_preferences_20210816,
    .certificate_preferences_apply_locally = true,
};

/*
 * This security policy is derived from the following specification:
 * https://datatracker.ietf.org/doc/html/rfc9151
 *
 * The following exceptions to this specification are made:
 * - RSA cipher suites are not supported to allow for perfect forward secrecy.
 * - DHE cipher suites are not supported to remove the possibility of improper Diffie-Hellman
 *   parameter configuration.
 */
const struct s2n_security_policy security_policy_20251013 = {
    .minimum_protocol_version = S2N_TLS12,
    .cipher_preferences = &cipher_preferences_20251013,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20250429,
    .certificate_signature_preferences = &s2n_certificate_signature_preferences_20250429,
    .certificate_key_preferences = &s2n_certificate_key_preferences_20250429,
    .ecc_preferences = &s2n_ecc_preferences_20210816,
    .certificate_preferences_apply_locally = true,
    .rules = {
            [S2N_PERFECT_FORWARD_SECRECY] = true,
            [S2N_FIPS_140_3] = true,
    },
};

/*
 * This security policy is a mix of default_tls13 (20240503) and rfc9151, with
 * a primary requirement that AES-256 is the ciphersuite chosen. Other
 * requirements are generally picked to raise minimum thresholds (e.g.,
 * requiring TLS 1.3) where possible without losing compatibility with modern
 * default_tls13 clients or servers.
 */
const struct s2n_security_policy security_policy_20250211 = {
    .minimum_protocol_version = S2N_TLS13,
    .cipher_preferences = &cipher_preferences_20250211,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20250429,
    .certificate_signature_preferences = &s2n_certificate_signature_preferences_20201110,
    .ecc_preferences = &s2n_ecc_preferences_20210816,
    .rules = {
            [S2N_PERFECT_FORWARD_SECRECY] = true,
    },
};

/*
 * This is essentially identical to 20250211, but fixes a bug which required
 * P-384 keys on certificates, which invalidated the compatibility promise for
 * that policy.
 */
const struct s2n_security_policy security_policy_20250414 = {
    .minimum_protocol_version = S2N_TLS13,
    .cipher_preferences = &cipher_preferences_20250211,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20240501,
    .certificate_signature_preferences = &s2n_certificate_signature_preferences_20201110,
    .ecc_preferences = &s2n_ecc_preferences_20210816,
    .rules = {
            [S2N_PERFECT_FORWARD_SECRECY] = true,
            [S2N_FIPS_140_3] = true,
    },
};

const struct s2n_security_policy security_policy_20251113 = {
    .minimum_protocol_version = S2N_TLS12,
    .cipher_preferences = &cipher_preferences_20251113,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20251113,
    .certificate_signature_preferences = &s2n_certificate_signature_preferences_20251113,
    .ecc_preferences = &s2n_ecc_preferences_20251113,
    .strongly_preferred_groups = &cnsa_1_strong_preference,
};

const struct s2n_security_policy security_policy_20251114 = {
    .minimum_protocol_version = S2N_TLS12,
    .cipher_preferences = &cipher_preferences_20251114,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20251113,
    .certificate_signature_preferences = &s2n_certificate_signature_preferences_20251113,
    .ecc_preferences = &s2n_ecc_preferences_20251113,
    .strongly_preferred_groups = &cnsa_1_strong_preference,
};

const struct s2n_security_policy security_policy_20251115 = {
    .minimum_protocol_version = S2N_TLS12,
    .cipher_preferences = &cipher_preferences_20251115,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20251113,
    .certificate_signature_preferences = &s2n_certificate_signature_preferences_20251113,
    .ecc_preferences = &s2n_ecc_preferences_20251113,
    .strongly_preferred_groups = &cnsa_1_strong_preference,
};

const struct s2n_security_policy security_policy_20251116 = {
    .minimum_protocol_version = S2N_TLS10,
    .cipher_preferences = &cipher_preferences_20251116,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20251113,
    .certificate_signature_preferences = &s2n_certificate_signature_preferences_20251113,
    .ecc_preferences = &s2n_ecc_preferences_20251113,
    .strongly_preferred_groups = &cnsa_1_strong_preference,
};

const struct s2n_security_policy security_policy_20251117 = {
    .minimum_protocol_version = S2N_TLS12,
    .cipher_preferences = &cipher_preferences_20251117,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20251113,
    .certificate_signature_preferences = &s2n_certificate_signature_preferences_20251113,
    .ecc_preferences = &s2n_ecc_preferences_20251113,
    .strongly_preferred_groups = &cnsa_1_strong_preference,
};

const struct s2n_security_policy security_policy_20260219 = {
    .minimum_protocol_version = S2N_TLS13,
    .cipher_preferences = &cipher_preferences_20250211,
    .kem_preferences = &kem_preferences_pq_tls_1_3_cnsa2_2026_02,
    .signature_preferences = &s2n_signature_preferences_20260219,
    .certificate_signature_preferences = &s2n_signature_preferences_20260219,
    .certificate_key_preferences = &s2n_certificate_key_preferences_20260219,
    .ecc_preferences = &s2n_ecc_preferences_null,
    .certificate_preferences_apply_locally = true,
    .rules = {
            [S2N_PERFECT_FORWARD_SECRECY] = true,
            [S2N_FIPS_140_3] = true,
    },
};

const struct s2n_security_policy security_policy_20260220 = {
    .minimum_protocol_version = S2N_TLS12,
    .cipher_preferences = &cipher_preferences_20260220,
    .kem_preferences = &kem_preferences_pq_tls_1_3_cnsa2_2026_02,
    .signature_preferences = &s2n_signature_preferences_20260220,
    .certificate_signature_preferences = &s2n_certificate_signature_preferences_20260220,
    .certificate_key_preferences = &s2n_certificate_key_preferences_20260220,
    .ecc_preferences = &s2n_ecc_preferences_20210816,
    .certificate_preferences_apply_locally = true,
    .rules = {
            [S2N_PERFECT_FORWARD_SECRECY] = true,
            [S2N_FIPS_140_3] = true,
    },
};

const struct s2n_security_policy security_policy_test_all = {
    .minimum_protocol_version = S2N_SSLv3,
    .cipher_preferences = &cipher_preferences_test_all,
    .kem_preferences = &kem_preferences_all,
    .signature_preferences = &s2n_signature_preferences_all,
    .ecc_preferences = &s2n_ecc_preferences_test_all,
};

const struct s2n_security_policy security_policy_test_all_tls12 = {
    .minimum_protocol_version = S2N_SSLv3,
    .cipher_preferences = &cipher_preferences_test_all_tls12,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20201021,
    .ecc_preferences = &s2n_ecc_preferences_20201021,
};

const struct s2n_security_policy security_policy_test_all_fips = {
    .minimum_protocol_version = S2N_TLS12,
    .cipher_preferences = &cipher_preferences_test_all_fips,
    .kem_preferences = &kem_preferences_all,
    .signature_preferences = &s2n_signature_preferences_test_all_fips,
    .ecc_preferences = &s2n_ecc_preferences_20201021,
    .rules = {
            [S2N_FIPS_140_3] = true,
    },
};

const struct s2n_security_policy security_policy_test_all_ecdsa = {
    .minimum_protocol_version = S2N_TLS10,
    .cipher_preferences = &cipher_preferences_test_all_ecdsa,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20201021,
    .ecc_preferences = &s2n_ecc_preferences_test_all,
    .rules = {
            [S2N_PERFECT_FORWARD_SECRECY] = true,
    },
};

const struct s2n_security_policy security_policy_test_all_rsa_kex = {
    .minimum_protocol_version = S2N_TLS10,
    .cipher_preferences = &cipher_preferences_test_all_rsa_kex,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20140601,
    .ecc_preferences = &s2n_ecc_preferences_20140601,
};

const struct s2n_security_policy security_policy_test_all_tls13 = {
    .minimum_protocol_version = S2N_SSLv3,
    .cipher_preferences = &cipher_preferences_test_all_tls13,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_all,
    .ecc_preferences = &s2n_ecc_preferences_test_all,
    .rules = {
            [S2N_PERFECT_FORWARD_SECRECY] = true,
    },
};

const struct s2n_security_policy security_policy_test_pq_only = {
    .minimum_protocol_version = S2N_TLS13,
    .cipher_preferences = &cipher_preferences_cloudfront_upstream_2025_08_08_tls13,
    .kem_preferences = &kem_preferences_all,
    .signature_preferences = &s2n_signature_preferences_20240501,
    .certificate_signature_preferences = &s2n_signature_preferences_20240501,
    .ecc_preferences = &s2n_ecc_preferences_null,
    .rules = {
            [S2N_PERFECT_FORWARD_SECRECY] = true,
    },
};

const struct s2n_security_policy security_policy_20200207 = {
    .minimum_protocol_version = S2N_SSLv3,
    .cipher_preferences = &cipher_preferences_test_all_tls13,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20201021,
    .ecc_preferences = &s2n_ecc_preferences_test_all,
    .rules = {
            [S2N_PERFECT_FORWARD_SECRECY] = true,
    },
};

const struct s2n_security_policy security_policy_test_ecdsa_priority = {
    .minimum_protocol_version = S2N_SSLv3,
    .cipher_preferences = &cipher_preferences_test_ecdsa_priority,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_20201021,
    .ecc_preferences = &s2n_ecc_preferences_test_all,
};

const struct s2n_security_policy security_policy_null = {
    .minimum_protocol_version = S2N_TLS10,
    .cipher_preferences = &cipher_preferences_null,
    .kem_preferences = &kem_preferences_null,
    .signature_preferences = &s2n_signature_preferences_null,
    .ecc_preferences = &s2n_ecc_preferences_null,
};

struct s2n_security_policy_selection security_policy_selection[] = {
    /* If changing named policies, please update the usage guide's docs on the corresponding policy.
     * You likely also want to update the compatibility unit tests in (tests/unit/s2n_security_rules_test.c).
     */
    { .version = "default", .security_policy = &security_policy_20251014, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "default_tls13", .security_policy = &security_policy_20240503, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "default_fips", .security_policy = &security_policy_20251015, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "default_pq", .security_policy = &security_policy_20250721, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "20241106", .security_policy = &security_policy_20241106, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "20240501", .security_policy = &security_policy_20240501, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "20240502", .security_policy = &security_policy_20240502, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "20240503", .security_policy = &security_policy_20240503, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "20230317", .security_policy = &security_policy_20230317, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "20240331", .security_policy = &security_policy_20240331, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "20240417", .security_policy = &security_policy_20240417, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "20240416", .security_policy = &security_policy_20240416, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "20241001", .security_policy = &security_policy_20241001, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "20250512", .security_policy = &security_policy_20250512, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "20250721", .security_policy = &security_policy_20250721, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "20251014", .security_policy = &security_policy_20251014, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "20251015", .security_policy = &security_policy_20251015, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "20241001_pq_mixed", .security_policy = &security_policy_20241001_pq_mixed, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "ELBSecurityPolicy-TLS-1-0-2015-04", .security_policy = &security_policy_elb_2015_04, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    /* Not a mistake. TLS-1-0-2015-05 and 2016-08 are equivalent */
    { .version = "ELBSecurityPolicy-TLS-1-0-2015-05", .security_policy = &security_policy_elb_2016_08, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "ELBSecurityPolicy-2016-08", .security_policy = &security_policy_elb_2016_08, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "ELBSecurityPolicy-TLS-1-1-2017-01", .security_policy = &security_policy_elb_tls_1_1_2017_01, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "ELBSecurityPolicy-TLS-1-2-2017-01", .security_policy = &security_policy_elb_tls_1_2_2017_01, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "ELBSecurityPolicy-TLS-1-2-Ext-2018-06", .security_policy = &security_policy_elb_tls_1_2_ext_2018_06, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "ELBSecurityPolicy-FS-2018-06", .security_policy = &security_policy_elb_fs_2018_06, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "ELBSecurityPolicy-FS-1-2-2019-08", .security_policy = &security_policy_elb_fs_1_2_2019_08, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "ELBSecurityPolicy-FS-1-1-2019-08", .security_policy = &security_policy_elb_fs_1_1_2019_08, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "ELBSecurityPolicy-FS-1-2-Res-2019-08", .security_policy = &security_policy_elb_fs_1_2_Res_2019_08, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "CloudFront-Upstream", .security_policy = &security_policy_cloudfront_upstream, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "CloudFront-Upstream-TLS-1-0", .security_policy = &security_policy_cloudfront_upstream_tls10, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "CloudFront-Upstream-TLS-1-1", .security_policy = &security_policy_cloudfront_upstream_tls11, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "CloudFront-Upstream-TLS-1-2", .security_policy = &security_policy_cloudfront_upstream_tls12, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "CloudFront-Upstream-2025", .security_policy = &security_policy_cloudfront_upstream_2025_08_08, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "CloudFront-Upstream-TLS-1-0-2025", .security_policy = &security_policy_cloudfront_upstream_2025_08_08_tls10, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "CloudFront-Upstream-TLS-1-1-2025", .security_policy = &security_policy_cloudfront_upstream_2025_08_08_tls11, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "CloudFront-Upstream-TLS-1-2-2025", .security_policy = &security_policy_cloudfront_upstream_2025_08_08_tls12, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "CloudFront-Upstream-TLS-1-3-2025", .security_policy = &security_policy_cloudfront_upstream_2025_08_08_tls13, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "CloudFront-Upstream-2025-PQ", .security_policy = &security_policy_cloudfront_upstream_2025_08_08_pq, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "CloudFront-Upstream-TLS-1-0-2025-PQ", .security_policy = &security_policy_cloudfront_upstream_2025_08_08_tls10_pq, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "CloudFront-Upstream-TLS-1-1-2025-PQ", .security_policy = &security_policy_cloudfront_upstream_2025_08_08_tls11_pq, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "CloudFront-Upstream-TLS-1-2-2025-PQ", .security_policy = &security_policy_cloudfront_upstream_2025_08_08_tls12_pq, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "CloudFront-Upstream-TLS-1-3-2025-PQ", .security_policy = &security_policy_cloudfront_upstream_2025_08_08_tls13_pq, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    /* CloudFront Viewer Facing */
    { .version = "CloudFront-SSL-v-3", .security_policy = &security_policy_cloudfront_ssl_v_3, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "CloudFront-TLS-1-0-2014", .security_policy = &security_policy_cloudfront_tls_1_0_2014, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "CloudFront-TLS-1-0-2014-sha256", .security_policy = &security_policy_cloudfront_tls_1_0_2014_sha256, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "CloudFront-TLS-1-0-2016", .security_policy = &security_policy_cloudfront_tls_1_0_2016, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "CloudFront-TLS-1-1-2016", .security_policy = &security_policy_cloudfront_tls_1_1_2016, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "CloudFront-TLS-1-2-2017", .security_policy = &security_policy_cloudfront_tls_1_2_2017, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "CloudFront-TLS-1-2-2018-no-sha1", .security_policy = &security_policy_cloudfront_tls_1_2_2018_no_sha1, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "CloudFront-TLS-1-2-2019-no-sha1", .security_policy = &security_policy_cloudfront_tls_1_2_2019_no_sha1, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "CloudFront-TLS-1-2-2021-no-sha1", .security_policy = &security_policy_cloudfront_tls_1_2_2021_no_sha1, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "CloudFront-TLS-1-2-2025", .security_policy = &security_policy_cloudfront_tls_1_2_2025, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "CloudFront-TLS-1-3-2025", .security_policy = &security_policy_cloudfront_tls_1_3_2025, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    /* CloudFront Non-PQ Viewer Facing */
    { .version = "CloudFront-SSL-v-3-no-pq", .security_policy = &security_policy_cloudfront_ssl_v_3_no_pq, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "CloudFront-TLS-1-0-2014-no-pq", .security_policy = &security_policy_cloudfront_tls_1_0_2014_no_pq, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "CloudFront-TLS-1-0-2014-sha256-no-pq", .security_policy = &security_policy_cloudfront_tls_1_0_2014_sha256_no_pq, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "CloudFront-TLS-1-0-2016-no-pq", .security_policy = &security_policy_cloudfront_tls_1_0_2016_no_pq, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "CloudFront-TLS-1-1-2016-no-pq", .security_policy = &security_policy_cloudfront_tls_1_1_2016_no_pq, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "CloudFront-TLS-1-2-2017-no-pq", .security_policy = &security_policy_cloudfront_tls_1_2_2017_no_pq, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "CloudFront-TLS-1-2-2018-no-sha1-no-pq", .security_policy = &security_policy_cloudfront_tls_1_2_2018_no_sha1_no_pq, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "CloudFront-TLS-1-2-2019-no-sha1-no-pq", .security_policy = &security_policy_cloudfront_tls_1_2_2019_no_sha1_no_pq, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "CloudFront-TLS-1-2-2021-no-sha1-no-pq", .security_policy = &security_policy_cloudfront_tls_1_2_2021_no_sha1_no_pq, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "CloudFront-TLS-1-2-2025-no-pq", .security_policy = &security_policy_cloudfront_tls_1_2_2025_no_pq, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "CloudFront-TLS-1-3-2025-no-pq", .security_policy = &security_policy_cloudfront_tls_1_3_2025_no_pq, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    /* CloudFront Unofficial Viewer Facing */
    { .version = "CloudFront-TLS-1-0-2014-PQ-Beta", .security_policy = &security_policy_cloudfront_tls_1_0_2014_pq_beta, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "CloudFront-TLS-1-2-2021-no-sha1-PQ-Beta", .security_policy = &security_policy_cloudfront_tls_1_2_2021_no_sha1_pq_beta, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "CloudFront-TLS-1-2-2018-Beta", .security_policy = &security_policy_cloudfront_tls_1_2_2018_beta, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "CloudFront-TLS-1-2-2021-Chacha20-Boosted", .security_policy = &security_policy_cloudfront_tls_1_2_2021_chacha20_boosted, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    /* CloudFront Legacy policies */
    { .version = "CloudFront-SSL-v-3-Legacy", .security_policy = &security_policy_cloudfront_ssl_v_3_legacy, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "CloudFront-TLS-1-0-2014-Legacy", .security_policy = &security_policy_cloudfront_tls_1_0_2014_legacy, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "CloudFront-TLS-1-0-2016-Legacy", .security_policy = &security_policy_cloudfront_tls_1_0_2016_legacy, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "CloudFront-TLS-1-1-2016-Legacy", .security_policy = &security_policy_cloudfront_tls_1_1_2016_legacy, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "CloudFront-TLS-1-2-2018-Legacy", .security_policy = &security_policy_cloudfront_tls_1_2_2018_legacy, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "CloudFront-TLS-1-2-2019-Legacy", .security_policy = &security_policy_cloudfront_tls_1_2_2019_legacy, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "CloudFront-TLS-1-2-2018", .security_policy = &security_policy_cloudfront_tls_1_2_2018, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "CloudFront-TLS-1-2-2019", .security_policy = &security_policy_cloudfront_tls_1_2_2019, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "CloudFront-TLS-1-2-2021", .security_policy = &security_policy_cloudfront_tls_1_2_2021, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    /* CRT allows users to choose the minimal TLS protocol they want to negotiate with. This translates to 5 different security policies in s2n */
    { .version = "AWS-CRT-SDK-SSLv3.0", .security_policy = &security_policy_aws_crt_sdk_ssl_v3, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "AWS-CRT-SDK-TLSv1.0", .security_policy = &security_policy_aws_crt_sdk_tls_10, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "AWS-CRT-SDK-TLSv1.1", .security_policy = &security_policy_aws_crt_sdk_tls_11, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "AWS-CRT-SDK-TLSv1.2", .security_policy = &security_policy_aws_crt_sdk_tls_12, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "AWS-CRT-SDK-TLSv1.3", .security_policy = &security_policy_aws_crt_sdk_tls_13, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "AWS-CRT-SDK-SSLv3.0-2023", .security_policy = &security_policy_aws_crt_sdk_ssl_v3_06_23, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "AWS-CRT-SDK-TLSv1.0-2023", .security_policy = &security_policy_aws_crt_sdk_tls_10_06_23, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "AWS-CRT-SDK-TLSv1.0-2025-PQ", .security_policy = &security_policy_aws_crt_sdk_tls_10_07_25_pq, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "AWS-CRT-SDK-TLSv1.1-2023", .security_policy = &security_policy_aws_crt_sdk_tls_11_06_23, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "AWS-CRT-SDK-TLSv1.2-2023", .security_policy = &security_policy_aws_crt_sdk_tls_12_06_23, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "AWS-CRT-SDK-TLSv1.2-2023-PQ", .security_policy = &security_policy_aws_crt_sdk_tls_12_06_23_pq, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "AWS-CRT-SDK-TLSv1.2-2025", .security_policy = &security_policy_aws_crt_sdk_tls_30_06_25, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "AWS-CRT-SDK-TLSv1.2-2025-PQ", .security_policy = &security_policy_aws_crt_sdk_tls_12_07_25_pq, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "AWS-CRT-SDK-TLSv1.3-2023", .security_policy = &security_policy_aws_crt_sdk_tls_13_06_23, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "AWS-CRT-SDK-TLSv1.3-2025-PQ", .security_policy = &security_policy_aws_crt_sdk_tls_13_07_25_pq, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    /* KMS TLS Policies*/
    { .version = "KMS-TLS-1-0-2018-10", .security_policy = &security_policy_kms_tls_1_0_2018_10, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "KMS-TLS-1-0-2021-08", .security_policy = &security_policy_kms_tls_1_0_2021_08, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "KMS-TLS-1-2-2023-06", .security_policy = &security_policy_kms_tls_1_2_2023_06, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "KMS-FIPS-TLS-1-2-2018-10", .security_policy = &security_policy_kms_fips_tls_1_2_2018_10, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "KMS-FIPS-TLS-1-2-2021-08", .security_policy = &security_policy_kms_fips_tls_1_2_2021_08, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "KMS-FIPS-TLS-1-2-2024-10", .security_policy = &security_policy_kms_fips_tls_1_2_2024_10, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "PQ-TLS-1-2-2024-10-07", .security_policy = &security_policy_pq_tls_1_2_2024_10_07, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "PQ-TLS-1-2-2024-10-08", .security_policy = &security_policy_pq_tls_1_2_2024_10_08, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "PQ-TLS-1-2-2024-10-08_gcm", .security_policy = &security_policy_pq_tls_1_2_2024_10_08_gcm, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "PQ-TLS-1-2-2024-10-09", .security_policy = &security_policy_pq_tls_1_2_2024_10_09, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "20140601", .security_policy = &security_policy_20140601, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "20141001", .security_policy = &security_policy_20141001, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "20150202", .security_policy = &security_policy_20150202, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "20150214", .security_policy = &security_policy_20150214, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "20150306", .security_policy = &security_policy_20150306, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "20160411", .security_policy = &security_policy_20160411, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "20160804", .security_policy = &security_policy_20160804, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "20160824", .security_policy = &security_policy_20160824, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "20170210", .security_policy = &security_policy_20170210, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "20170328", .security_policy = &security_policy_20170328, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "20170328_gcm", .security_policy = &security_policy_20170328_gcm, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "20190214", .security_policy = &security_policy_20190214, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "20190214_gcm", .security_policy = &security_policy_20190214_gcm, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "20210825", .security_policy = &security_policy_20210825, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "20210825_gcm", .security_policy = &security_policy_20210825_gcm, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "20170405", .security_policy = &security_policy_20170405, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "20170405_gcm", .security_policy = &security_policy_20170405_gcm, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "20170718", .security_policy = &security_policy_20170718, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "20170718_gcm", .security_policy = &security_policy_20170718_gcm, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "20190120", .security_policy = &security_policy_20190120, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "20190121", .security_policy = &security_policy_20190121, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "20190122", .security_policy = &security_policy_20190122, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "20190801", .security_policy = &security_policy_20190801, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "20190802", .security_policy = &security_policy_20190802, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "20200207", .security_policy = &security_policy_20200207, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "20201021", .security_policy = &security_policy_20201021, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "20210816", .security_policy = &security_policy_20210816, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "20210816_GCM", .security_policy = &security_policy_20210816_gcm, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "20240603", .security_policy = &security_policy_20240603, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "20250211", .security_policy = &security_policy_20250211, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "20250414", .security_policy = &security_policy_20250414, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "20250429", .security_policy = &security_policy_20250429, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "20251013", .security_policy = &security_policy_20251013, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "20251113", .security_policy = &security_policy_20251113, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "20251114", .security_policy = &security_policy_20251114, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "20251115", .security_policy = &security_policy_20251115, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "20251116", .security_policy = &security_policy_20251116, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    /* the same as 20251114, but without any SHA1 HMAC ciphers */
    { .version = "20251117", .security_policy = &security_policy_20251117, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    /* If changing this, please update the usage guide's docs on the corresponding policy. */
    { .version = "rfc9151", .security_policy = &security_policy_20251013, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "cnsa_1", .security_policy = &security_policy_20251013, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "cnsa_2", .security_policy = &security_policy_20260219, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "cnsa_1_2_interop", .security_policy = &security_policy_20260220, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "test_all", .security_policy = &security_policy_test_all, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "test_all_fips", .security_policy = &security_policy_test_all_fips, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "test_all_ecdsa", .security_policy = &security_policy_test_all_ecdsa, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "test_all_rsa_kex", .security_policy = &security_policy_test_all_rsa_kex, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "test_ecdsa_priority", .security_policy = &security_policy_test_ecdsa_priority, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "test_all_tls12", .security_policy = &security_policy_test_all_tls12, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "test_all_tls13", .security_policy = &security_policy_test_all_tls13, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "test_pq_only", .security_policy = &security_policy_test_pq_only, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = "null", .security_policy = &security_policy_null, .ecc_extension_required = 0, .pq_kem_extension_required = 0 },
    { .version = NULL, .security_policy = NULL, .ecc_extension_required = 0, .pq_kem_extension_required = 0 }
};

const char *deprecated_security_policies[] = {
    "KMS-PQ-TLS-1-0-2019-06",
    "KMS-PQ-TLS-1-0-2020-02",
    "KMS-PQ-TLS-1-0-2020-07",
    "PQ-TLS-1-0-2020-12",
    "PQ-TLS-1-1-2021-05-17",
    "PQ-TLS-1-0-2021-05-18",
    "PQ-TLS-1-0-2021-05-19",
    "PQ-TLS-1-0-2021-05-20",
    "PQ-TLS-1-1-2021-05-21",
    "PQ-TLS-1-0-2021-05-22",
    "PQ-TLS-1-0-2021-05-23",
    "PQ-TLS-1-0-2021-05-24",
    "PQ-TLS-1-0-2021-05-25",
    "PQ-TLS-1-0-2021-05-26",
    "PQ-TLS-1-0-2023-01-24",
    "PQ-TLS-1-2-2023-04-07",
    "PQ-TLS-1-2-2023-04-08",
    "PQ-TLS-1-2-2023-04-09",
    "PQ-TLS-1-2-2023-04-10",
    "PQ-TLS-1-3-2023-06-01",
    "PQ-TLS-1-2-2023-10-07",
    "PQ-TLS-1-2-2023-10-08",
    "PQ-TLS-1-2-2023-10-09",
    "PQ-TLS-1-2-2023-10-10",
    "PQ-TLS-1-2-2023-12-13",
    "PQ-TLS-1-2-2023-12-14",
    "PQ-TLS-1-2-2023-12-15",
    "PQ-SIKE-TEST-TLS-1-0-2019-11",
    "PQ-SIKE-TEST-TLS-1-0-2020-02",
    "20240730",
};
const size_t deprecated_security_policies_len = s2n_array_len(deprecated_security_policies);

int s2n_find_security_policy_from_version(const char *version, const struct s2n_security_policy **security_policy)
{
    POSIX_ENSURE_REF(version);
    POSIX_ENSURE_REF(security_policy);

    for (int i = 0; security_policy_selection[i].version != NULL; i++) {
        if (!strcasecmp(version, security_policy_selection[i].version)) {
            *security_policy = security_policy_selection[i].security_policy;
            return 0;
        }
    }

    for (size_t i = 0; i < deprecated_security_policies_len; i++) {
        if (!strcasecmp(version, deprecated_security_policies[i])) {
            POSIX_BAIL(S2N_ERR_DEPRECATED_SECURITY_POLICY);
        }
    }

    POSIX_BAIL(S2N_ERR_INVALID_SECURITY_POLICY);
}

static int s2n_config_validate_security_policy(struct s2n_config *config, const struct s2n_security_policy *security_policy)
{
    POSIX_ENSURE_REF(config);
    POSIX_ENSURE_REF(security_policy);
    POSIX_ENSURE_REF(security_policy->cipher_preferences);
    POSIX_ENSURE_REF(security_policy->kem_preferences);
    POSIX_ENSURE_REF(security_policy->signature_preferences);
    POSIX_ENSURE_REF(security_policy->ecc_preferences);

    /* If the security policy's minimum version is higher than what libcrypto supports, return an error. */
    POSIX_ENSURE((security_policy->minimum_protocol_version <= s2n_get_highest_fully_supported_tls_version()), S2N_ERR_PROTOCOL_VERSION_UNSUPPORTED);

    if (security_policy == &security_policy_null) {
        return S2N_SUCCESS;
    }

    /* Ensure that all strongly preferred groups are supported by our libcrypto. */
    for (size_t i = 0; security_policy->strongly_preferred_groups != NULL && i < security_policy->strongly_preferred_groups->count; i++) {
        const struct s2n_kem_group *strongly_preferred_kem_group = NULL;
        bool found_kem_group_from_iana = false;
        POSIX_GUARD(s2n_find_kem_group_from_iana_id(security_policy->strongly_preferred_groups->iana_ids[i], &strongly_preferred_kem_group, &found_kem_group_from_iana));

        if (found_kem_group_from_iana) {
            POSIX_ENSURE(s2n_kem_group_is_available(strongly_preferred_kem_group), S2N_ERR_INVALID_SECURITY_POLICY);
        }
    }

    /* Ensure that an ECC or PQ key exchange can occur. */
    uint32_t ecc_available = security_policy->ecc_preferences->count;
    uint32_t kem_groups_available = 0;
    POSIX_GUARD_RESULT(s2n_kem_preferences_groups_available(security_policy->kem_preferences, &kem_groups_available));
    POSIX_ENSURE(ecc_available + kem_groups_available > 0, S2N_ERR_INVALID_SECURITY_POLICY);

    /* If the config contains certificates violating the security policy cert preferences, return an error. */
    POSIX_GUARD_RESULT(s2n_config_validate_loaded_certificates(config, security_policy));
    return S2N_SUCCESS;
}

int s2n_config_set_security_policy(struct s2n_config *config, const struct s2n_security_policy *security_policy)
{
    POSIX_ENSURE_REF(config);
    POSIX_GUARD(s2n_config_validate_security_policy(config, security_policy));
    config->security_policy = security_policy;
    return 0;
}

int s2n_config_set_cipher_preferences(struct s2n_config *config, const char *version)
{
    const struct s2n_security_policy *security_policy = NULL;
    POSIX_GUARD(s2n_find_security_policy_from_version(version, &security_policy));
    POSIX_GUARD(s2n_config_set_security_policy(config, security_policy));
    return S2N_SUCCESS;
}

int s2n_connection_set_security_policy(struct s2n_connection *conn, const struct s2n_security_policy *security_policy)
{
    POSIX_ENSURE_REF(conn);
    POSIX_GUARD(s2n_config_validate_security_policy(conn->config, security_policy));
    conn->security_policy_override = security_policy;
    return 0;
}

int s2n_connection_set_cipher_preferences(struct s2n_connection *conn, const char *version)
{
    const struct s2n_security_policy *security_policy = NULL;
    POSIX_GUARD(s2n_find_security_policy_from_version(version, &security_policy));
    POSIX_GUARD(s2n_connection_set_security_policy(conn, security_policy));
    return S2N_SUCCESS;
}

int s2n_security_policies_init()
{
    for (int i = 0; security_policy_selection[i].version != NULL; i++) {
        const struct s2n_security_policy *security_policy = security_policy_selection[i].security_policy;
        POSIX_ENSURE_REF(security_policy);
        const struct s2n_cipher_preferences *cipher_preference = security_policy->cipher_preferences;
        POSIX_ENSURE_REF(cipher_preference);
        const struct s2n_kem_preferences *kem_preference = security_policy->kem_preferences;
        POSIX_ENSURE_REF(kem_preference);
        const struct s2n_ecc_preferences *ecc_preference = security_policy->ecc_preferences;
        POSIX_ENSURE_REF(ecc_preference);
        POSIX_GUARD(s2n_check_ecc_preferences_curves_list(ecc_preference));

        const struct s2n_signature_preferences *certificate_signature_preference = security_policy->certificate_signature_preferences;
        if (certificate_signature_preference != NULL) {
            POSIX_GUARD_RESULT(s2n_validate_certificate_signature_preferences(certificate_signature_preference));
        }

        if (security_policy != &security_policy_null) {
            /* All policies must have at least one ecc curve or PQ kem group configured. */
            bool ecc_kx_supported = ecc_preference->count > 0;
            bool pq_kx_supported = kem_preference->tls13_kem_group_count > 0;
            POSIX_ENSURE(ecc_kx_supported || pq_kx_supported, S2N_ERR_INVALID_SECURITY_POLICY);

            /* A PQ key exchange is only supported in TLS 1.3, so PQ-only policies must require TLS 1.3.*/
            if (!ecc_kx_supported) {
                POSIX_ENSURE(security_policy->minimum_protocol_version >= S2N_TLS13, S2N_ERR_INVALID_SECURITY_POLICY);
            }
        }

        for (int j = 0; j < cipher_preference->count; j++) {
            struct s2n_cipher_suite *cipher = cipher_preference->suites[j];
            POSIX_ENSURE_REF(cipher);

            const uint8_t *iana = cipher->iana_value;

            if (cipher->minimum_required_tls_version >= S2N_TLS13) {
                security_policy_selection[i].supports_tls13 = 1;
            }

            /* Sanity check that valid tls13 has minimum tls version set correctly */
            S2N_ERROR_IF(s2n_is_valid_tls13_cipher(iana) ^ (cipher->minimum_required_tls_version >= S2N_TLS13),
                    S2N_ERR_INVALID_SECURITY_POLICY);

            if (s2n_cipher_suite_requires_ecc_extension(cipher)) {
                security_policy_selection[i].ecc_extension_required = 1;
            }

            if (s2n_cipher_suite_requires_pq_extension(cipher) && kem_preference->kem_count > 0) {
                security_policy_selection[i].pq_kem_extension_required = 1;
            }
        }

        POSIX_GUARD(s2n_validate_kem_preferences(kem_preference, security_policy_selection[i].pq_kem_extension_required));

        /* Validate that security rules are correctly applied.
         * This should be checked by a unit test, but outside of unit tests we
         * check again here to cover the case where the unit tests are not run.
         */
        if (!s2n_in_unit_test()) {
            struct s2n_security_rule_result result = { 0 };
            POSIX_GUARD_RESULT(s2n_security_policy_validate_security_rules(security_policy, &result));
            POSIX_ENSURE(!result.found_error, S2N_ERR_INVALID_SECURITY_POLICY);
        }
    }
    return 0;
}

bool s2n_ecc_is_extension_required(const struct s2n_security_policy *security_policy)
{
    if (security_policy == NULL) {
        return false;
    }

    for (int i = 0; security_policy_selection[i].version != NULL; i++) {
        if (security_policy_selection[i].security_policy == security_policy) {
            return 1 == security_policy_selection[i].ecc_extension_required;
        }
    }

    /* If cipher preference is not in the official list, compute the result */
    const struct s2n_cipher_preferences *cipher_preferences = security_policy->cipher_preferences;
    if (cipher_preferences == NULL) {
        return false;
    }
    for (size_t i = 0; i < cipher_preferences->count; i++) {
        if (s2n_cipher_suite_requires_ecc_extension(cipher_preferences->suites[i])) {
            return true;
        }
    }

    return false;
}

bool s2n_pq_kem_is_extension_required(const struct s2n_security_policy *security_policy)
{
    if (security_policy == NULL) {
        return false;
    }

    for (int i = 0; security_policy_selection[i].version != NULL; i++) {
        if (security_policy_selection[i].security_policy == security_policy) {
            return 1 == security_policy_selection[i].pq_kem_extension_required;
        }
    }

    /* Preferences with no KEMs for the TLS 1.2 PQ KEM extension do not require that extension. */
    if (security_policy->kem_preferences && security_policy->kem_preferences->kem_count == 0) {
        return false;
    }

    /* If cipher preference is not in the official list, compute the result */
    const struct s2n_cipher_preferences *cipher_preferences = security_policy->cipher_preferences;
    if (cipher_preferences == NULL) {
        return false;
    }
    for (size_t i = 0; i < cipher_preferences->count; i++) {
        if (s2n_cipher_suite_requires_pq_extension(cipher_preferences->suites[i])) {
            return true;
        }
    }
    return false;
}

/* Checks whether cipher preference supports TLS 1.3 based on whether it is configured
 * with TLS 1.3 ciphers. Returns true or false.
 */
bool s2n_security_policy_supports_tls13(const struct s2n_security_policy *security_policy)
{
    if (security_policy == NULL) {
        return false;
    }

    for (size_t i = 0; security_policy_selection[i].version != NULL; i++) {
        if (security_policy_selection[i].security_policy == security_policy) {
            return security_policy_selection[i].supports_tls13 == 1;
        }
    }

    /* if cipher preference is not in the official list, compute the result */
    const struct s2n_cipher_preferences *cipher_preferences = security_policy->cipher_preferences;
    if (cipher_preferences == NULL) {
        return false;
    }

    for (size_t i = 0; i < cipher_preferences->count; i++) {
        if (cipher_preferences->suites[i]->minimum_required_tls_version >= S2N_TLS13) {
            return true;
        }
    }

    return false;
}

int s2n_connection_is_valid_for_cipher_preferences(struct s2n_connection *conn, const char *version)
{
    POSIX_ENSURE_REF(conn);
    POSIX_ENSURE_REF(version);
    POSIX_ENSURE_REF(conn->secure);
    POSIX_ENSURE_REF(conn->secure->cipher_suite);

    const struct s2n_security_policy *security_policy = NULL;
    POSIX_GUARD(s2n_find_security_policy_from_version(version, &security_policy));
    POSIX_ENSURE_REF(security_policy);

    /* make sure we don't use a tls version lower than that configured by the version */
    if (s2n_connection_get_actual_protocol_version(conn) < security_policy->minimum_protocol_version) {
        return 0;
    }

    struct s2n_cipher_suite *cipher = conn->secure->cipher_suite;
    POSIX_ENSURE_REF(cipher);
    for (int i = 0; i < security_policy->cipher_preferences->count; ++i) {
        if (s2n_constant_time_equals(security_policy->cipher_preferences->suites[i]->iana_value, cipher->iana_value, S2N_TLS_CIPHER_SUITE_LEN)) {
            return 1;
        }
    }

    return 0;
}

int s2n_validate_kem_preferences(const struct s2n_kem_preferences *kem_preferences, bool pq_kem_extension_required)
{
    POSIX_ENSURE_REF(kem_preferences);

    /* Basic sanity checks to assert that the count is 0 if and only if the associated list is NULL */
    POSIX_ENSURE(S2N_IFF(kem_preferences->tls13_kem_group_count == 0, kem_preferences->tls13_kem_groups == NULL),
            S2N_ERR_INVALID_SECURITY_POLICY);
    POSIX_ENSURE(S2N_IFF(kem_preferences->kem_count == 0, kem_preferences->kems == NULL),
            S2N_ERR_INVALID_SECURITY_POLICY);
    POSIX_ENSURE(kem_preferences->tls13_kem_group_count <= S2N_KEM_GROUPS_COUNT, S2N_ERR_ARRAY_INDEX_OOB);

    /* The PQ KEM extension is applicable only to TLS 1.2 */
    if (pq_kem_extension_required) {
        POSIX_ENSURE(kem_preferences->kem_count > 0, S2N_ERR_INVALID_SECURITY_POLICY);
        POSIX_ENSURE(kem_preferences->kems != NULL, S2N_ERR_INVALID_SECURITY_POLICY);
    } else {
        POSIX_ENSURE(kem_preferences->kem_count == 0, S2N_ERR_INVALID_SECURITY_POLICY);
        POSIX_ENSURE(kem_preferences->kems == NULL, S2N_ERR_INVALID_SECURITY_POLICY);
    }

    return S2N_SUCCESS;
}

S2N_RESULT s2n_validate_certificate_signature_preferences(const struct s2n_signature_preferences *certificate_signature_preferences)
{
    RESULT_ENSURE_REF(certificate_signature_preferences);

    size_t rsa_pss_scheme_count = 0;

    for (size_t i = 0; i < certificate_signature_preferences->count; i++) {
        if (certificate_signature_preferences->signature_schemes[i]->libcrypto_nid == NID_rsassaPss) {
            rsa_pss_scheme_count++;
        }
    }

    /*
     * https://github.com/aws/s2n-tls/issues/3435
     *
     * The Openssl function used to parse signatures off certificates does not differentiate between any rsa pss
     * signature schemes. Therefore a security policy with a certificate signatures preference list must include
     * all rsa_pss signature schemes. */
    RESULT_ENSURE(rsa_pss_scheme_count == NUM_RSA_PSS_SCHEMES || rsa_pss_scheme_count == 0, S2N_ERR_INVALID_SECURITY_POLICY);
    return S2N_RESULT_OK;
}

S2N_RESULT s2n_security_policy_get_version(const struct s2n_security_policy *security_policy, const char **version)
{
    RESULT_ENSURE_REF(version);
    *version = NULL;
    for (size_t i = 0; security_policy_selection[i].version != NULL; i++) {
        if (security_policy_selection[i].security_policy == security_policy) {
            *version = security_policy_selection[i].version;
            return S2N_RESULT_OK;
        }
    }
    RESULT_BAIL(S2N_ERR_INVALID_SECURITY_POLICY);
}

S2N_RESULT s2n_security_policy_validate_cert_signature(const struct s2n_security_policy *security_policy,
        const struct s2n_cert_info *info, s2n_error error)
{
    RESULT_ENSURE_REF(info);
    RESULT_ENSURE_REF(security_policy);
    const struct s2n_signature_preferences *sig_preferences = security_policy->certificate_signature_preferences;

    if (sig_preferences != NULL) {
        for (size_t i = 0; i < sig_preferences->count; i++) {
            if (sig_preferences->signature_schemes[i]->libcrypto_nid == info->signature_nid) {
                return S2N_RESULT_OK;
            }
        }

        RESULT_BAIL(error);
    }
    return S2N_RESULT_OK;
}

S2N_RESULT s2n_security_policy_validate_cert_key(const struct s2n_security_policy *security_policy,
        const struct s2n_cert_info *info, s2n_error error)
{
    RESULT_ENSURE_REF(info);
    RESULT_ENSURE_REF(security_policy);
    const struct s2n_certificate_key_preferences *key_preferences = security_policy->certificate_key_preferences;

    if (key_preferences != NULL) {
        for (size_t i = 0; i < key_preferences->count; i++) {
            if (key_preferences->certificate_keys[i]->public_key_libcrypto_nid == info->public_key_nid
                    && key_preferences->certificate_keys[i]->bits == info->public_key_bits) {
                return S2N_RESULT_OK;
            }
        }
        RESULT_BAIL(error);
    }
    return S2N_RESULT_OK;
}

S2N_RESULT s2n_security_policy_validate_certificate_chain(
        const struct s2n_security_policy *security_policy,
        const struct s2n_cert_chain_and_key *cert_key_pair)
{
    RESULT_ENSURE_REF(security_policy);
    RESULT_ENSURE_REF(cert_key_pair);
    RESULT_ENSURE_REF(cert_key_pair->cert_chain);

    if (!security_policy->certificate_preferences_apply_locally) {
        return S2N_RESULT_OK;
    }

    struct s2n_cert *current = cert_key_pair->cert_chain->head;
    while (current != NULL) {
        RESULT_GUARD(s2n_security_policy_validate_cert_key(security_policy, &current->info,
                S2N_ERR_SECURITY_POLICY_INCOMPATIBLE_CERT));
        RESULT_GUARD(s2n_security_policy_validate_cert_signature(security_policy, &current->info,
                S2N_ERR_SECURITY_POLICY_INCOMPATIBLE_CERT));
        current = current->next;
    }
    return S2N_RESULT_OK;
}

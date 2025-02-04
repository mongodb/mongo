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

#include <openssl/asn1.h>
#include <openssl/x509.h>
#include <stdint.h>

#include "crypto/s2n_certificate.h"
#include "utils/s2n_blob.h"
#include "utils/s2n_safety.h"

#define S2N_MAX_ALLOWED_CERT_TRAILING_BYTES 3

DEFINE_POINTER_CLEANUP_FUNC(X509 *, X509_free);

S2N_CLEANUP_RESULT s2n_openssl_x509_stack_pop_free(STACK_OF(X509) **cert_chain);

S2N_CLEANUP_RESULT s2n_openssl_asn1_time_free_pointer(ASN1_GENERALIZEDTIME **time);

/*
 * This function is used to convert an s2n_blob into an openssl X509 cert. It
 * will additionally ensure that there are 3 or fewer trailing bytes in
 * `asn1der`.
 */
S2N_RESULT s2n_openssl_x509_parse(struct s2n_blob *asn1der, X509 **cert_out);

/*
 * This function is used to convert an s2n_blob into an openssl X509 cert.
 * Unlike `s2n_openssl_x509_parse` no additional validation is done. This
 * function should only be used in places where it is necessary to maintain
 * compatability with previous permissive parsing behavior.
 */
S2N_RESULT s2n_openssl_x509_parse_without_length_validation(struct s2n_blob *asn1der, X509 **cert_out);

S2N_RESULT s2n_openssl_x509_get_cert_info(X509 *cert, struct s2n_cert_info *info);

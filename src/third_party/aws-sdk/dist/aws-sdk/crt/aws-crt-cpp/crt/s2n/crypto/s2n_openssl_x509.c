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

#include "crypto/s2n_openssl_x509.h"

#include "api/s2n.h"

DEFINE_POINTER_CLEANUP_FUNC(EVP_PKEY *, EVP_PKEY_free);
DEFINE_POINTER_CLEANUP_FUNC(EC_KEY *, EC_KEY_free);

S2N_CLEANUP_RESULT s2n_openssl_x509_stack_pop_free(STACK_OF(X509) **cert_chain)
{
    RESULT_ENSURE_REF(*cert_chain);
    sk_X509_pop_free(*cert_chain, X509_free);
    *cert_chain = NULL;
    return S2N_RESULT_OK;
}

S2N_CLEANUP_RESULT s2n_openssl_asn1_time_free_pointer(ASN1_GENERALIZEDTIME **time_ptr)
{
    /* The ANS1_*TIME structs are just typedef wrappers around ASN1_STRING
     *
     * The ASN1_TIME, ASN1_UTCTIME and ASN1_GENERALIZEDTIME structures are
     * represented as an ASN1_STRING internally and can be freed up using
     * ASN1_STRING_free().
     * https://www.openssl.org/docs/man1.1.1/man3/ASN1_TIME_to_tm.html
     */
    RESULT_ENSURE_REF(*time_ptr);
    ASN1_STRING_free((ASN1_STRING *) *time_ptr);
    *time_ptr = NULL;
    return S2N_RESULT_OK;
}

S2N_RESULT s2n_openssl_x509_parse_impl(struct s2n_blob *asn1der, X509 **cert_out, uint32_t *parsed_length)
{
    RESULT_ENSURE_REF(asn1der);
    RESULT_ENSURE_REF(asn1der->data);
    RESULT_ENSURE_REF(cert_out);
    RESULT_ENSURE_REF(parsed_length);

    uint8_t *cert_to_parse = asn1der->data;
    *cert_out = d2i_X509(NULL, (const unsigned char **) (void *) &cert_to_parse, asn1der->size);
    RESULT_ENSURE(*cert_out != NULL, S2N_ERR_DECODE_CERTIFICATE);

    /* If cert parsing is successful, d2i_X509 increments *cert_to_parse to the byte following the parsed data */
    *parsed_length = cert_to_parse - asn1der->data;

    return S2N_RESULT_OK;
}

S2N_RESULT s2n_openssl_x509_parse_without_length_validation(struct s2n_blob *asn1der, X509 **cert_out)
{
    RESULT_ENSURE_REF(asn1der);
    RESULT_ENSURE_REF(cert_out);

    uint32_t parsed_len = 0;
    RESULT_GUARD(s2n_openssl_x509_parse_impl(asn1der, cert_out, &parsed_len));

    return S2N_RESULT_OK;
}

S2N_RESULT s2n_openssl_x509_parse(struct s2n_blob *asn1der, X509 **cert_out)
{
    RESULT_ENSURE_REF(asn1der);
    RESULT_ENSURE_REF(cert_out);

    uint32_t parsed_len = 0;
    RESULT_GUARD(s2n_openssl_x509_parse_impl(asn1der, cert_out, &parsed_len));

    /* Some TLS clients in the wild send extra trailing bytes after the Certificate.
     * Allow this in s2n for backwards compatibility with existing clients. */
    uint32_t trailing_bytes = asn1der->size - parsed_len;
    RESULT_ENSURE(trailing_bytes <= S2N_MAX_ALLOWED_CERT_TRAILING_BYTES, S2N_ERR_DECODE_CERTIFICATE);

    return S2N_RESULT_OK;
}

S2N_RESULT s2n_openssl_x509_get_cert_info(X509 *cert, struct s2n_cert_info *info)
{
    RESULT_ENSURE_REF(cert);
    RESULT_ENSURE_REF(info);

    X509_NAME *issuer_name = X509_get_issuer_name(cert);
    RESULT_ENSURE_REF(issuer_name);

    X509_NAME *subject_name = X509_get_subject_name(cert);
    RESULT_ENSURE_REF(subject_name);

    if (X509_NAME_cmp(issuer_name, subject_name) == 0) {
        info->self_signed = true;
    } else {
        info->self_signed = false;
    }

#if defined(LIBRESSL_VERSION_NUMBER) && (LIBRESSL_VERSION_NUMBER < 0x02070000f)
    RESULT_ENSURE_REF(cert->sig_alg);
    info->signature_nid = OBJ_obj2nid(cert->sig_alg->algorithm);
#else
    info->signature_nid = X509_get_signature_nid(cert);
#endif
    /* These is no method to directly retrieve that signature digest from the X509*
     * that is available in all libcryptos, so instead we use find_sigid_algs. For
     * a signature NID_ecdsa_with_SHA256 this will return NID_SHA256 
     */
    RESULT_GUARD_OSSL(OBJ_find_sigid_algs(info->signature_nid, &info->signature_digest_nid, NULL),
            S2N_ERR_CERT_TYPE_UNSUPPORTED);

    DEFER_CLEANUP(EVP_PKEY *pubkey = X509_get_pubkey(cert), EVP_PKEY_free_pointer);
    RESULT_ENSURE(pubkey != NULL, S2N_ERR_DECODE_CERTIFICATE);

    info->public_key_bits = EVP_PKEY_bits(pubkey);
    RESULT_ENSURE(info->public_key_bits > 0, S2N_ERR_CERT_TYPE_UNSUPPORTED);

    if (EVP_PKEY_base_id(pubkey) == EVP_PKEY_EC) {
        DEFER_CLEANUP(EC_KEY *ec_key = EVP_PKEY_get1_EC_KEY(pubkey), EC_KEY_free_pointer);
        RESULT_ENSURE_REF(ec_key);
        const EC_GROUP *ec_group = EC_KEY_get0_group(ec_key);
        RESULT_ENSURE_REF(ec_group);
        info->public_key_nid = EC_GROUP_get_curve_name(ec_group);
    } else {
        info->public_key_nid = EVP_PKEY_id(pubkey);
    }
    RESULT_ENSURE(info->public_key_nid != NID_undef, S2N_ERR_CERT_TYPE_UNSUPPORTED);

    return S2N_RESULT_OK;
}

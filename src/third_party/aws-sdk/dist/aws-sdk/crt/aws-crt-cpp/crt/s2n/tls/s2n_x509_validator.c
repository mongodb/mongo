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

#include <arpa/inet.h>
#include <openssl/asn1.h>
#include <openssl/err.h>
#include <openssl/x509.h>
#include <sys/socket.h>

#include "crypto/s2n_libcrypto.h"
#include "crypto/s2n_openssl.h"
#include "crypto/s2n_openssl_x509.h"
#include "crypto/s2n_pkey.h"
#include "tls/extensions/s2n_extension_list.h"
#include "tls/s2n_config.h"
#include "tls/s2n_connection.h"
#include "tls/s2n_crl.h"
#include "tls/s2n_security_policies.h"
#include "utils/s2n_result.h"
#include "utils/s2n_rfc5952.h"
#include "utils/s2n_safety.h"

#if S2N_OCSP_STAPLING_SUPPORTED
    #include <openssl/ocsp.h>
DEFINE_POINTER_CLEANUP_FUNC(OCSP_RESPONSE *, OCSP_RESPONSE_free);
DEFINE_POINTER_CLEANUP_FUNC(OCSP_BASICRESP *, OCSP_BASICRESP_free);

#endif

#ifndef X509_V_FLAG_PARTIAL_CHAIN
    #define X509_V_FLAG_PARTIAL_CHAIN 0x80000
#endif

#define DEFAULT_MAX_CHAIN_DEPTH 7
/* Time used by default for nextUpdate if none provided in OCSP: 1 hour since thisUpdate. */
#define DEFAULT_OCSP_NEXT_UPDATE_PERIOD 3600

/* s2n's internal clock measures epoch-nanoseconds stored with a uint64_t. The
 * maximum representable timestamp is Sunday, July 21, 2554. time_t measures
 * epoch-seconds in a int64_t or int32_t (platform dependent). If time_t is an
 * int32_t, the maximum representable timestamp is January 19, 2038.
 *
 * This means that converting from the internal clock to a time_t is not safe,
 * because the internal clock might hold a value that is too large to represent
 * in a time_t. This constant represents the largest internal clock value that
 * can be safely represented as a time_t.
 */
#define MAX_32_TIMESTAMP_NANOS 2147483647 * ONE_SEC_IN_NANOS

#define OSSL_VERIFY_CALLBACK_IGNORE_ERROR 1

DEFINE_POINTER_CLEANUP_FUNC(STACK_OF(X509_CRL) *, sk_X509_CRL_free);
DEFINE_POINTER_CLEANUP_FUNC(STACK_OF(GENERAL_NAME) *, GENERAL_NAMES_free);

uint8_t s2n_x509_ocsp_stapling_supported(void)
{
    return S2N_OCSP_STAPLING_SUPPORTED;
}

void s2n_x509_trust_store_init_empty(struct s2n_x509_trust_store *store)
{
    store->trust_store = NULL;
}

uint8_t s2n_x509_trust_store_has_certs(struct s2n_x509_trust_store *store)
{
    return store->trust_store ? (uint8_t) 1 : (uint8_t) 0;
}

int s2n_x509_trust_store_add_pem(struct s2n_x509_trust_store *store, const char *pem)
{
    POSIX_ENSURE_REF(store);
    POSIX_ENSURE_REF(pem);

    if (!store->trust_store) {
        store->trust_store = X509_STORE_new();
    }

    DEFER_CLEANUP(struct s2n_stuffer pem_in_stuffer = { 0 }, s2n_stuffer_free);
    DEFER_CLEANUP(struct s2n_stuffer der_out_stuffer = { 0 }, s2n_stuffer_free);

    POSIX_GUARD(s2n_stuffer_alloc_ro_from_string(&pem_in_stuffer, pem));
    POSIX_GUARD(s2n_stuffer_growable_alloc(&der_out_stuffer, 2048));

    do {
        DEFER_CLEANUP(struct s2n_blob next_cert = { 0 }, s2n_free);

        POSIX_GUARD(s2n_stuffer_certificate_from_pem(&pem_in_stuffer, &der_out_stuffer));
        POSIX_GUARD(s2n_alloc(&next_cert, s2n_stuffer_data_available(&der_out_stuffer)));
        POSIX_GUARD(s2n_stuffer_read(&der_out_stuffer, &next_cert));

        const uint8_t *data = next_cert.data;
        DEFER_CLEANUP(X509 *ca_cert = d2i_X509(NULL, &data, next_cert.size), X509_free_pointer);
        S2N_ERROR_IF(ca_cert == NULL, S2N_ERR_DECODE_CERTIFICATE);

        if (!X509_STORE_add_cert(store->trust_store, ca_cert)) {
            unsigned long error = ERR_get_error();
            POSIX_ENSURE(ERR_GET_REASON(error) == X509_R_CERT_ALREADY_IN_HASH_TABLE, S2N_ERR_DECODE_CERTIFICATE);
        }
    } while (s2n_stuffer_data_available(&pem_in_stuffer));

    return 0;
}

int s2n_x509_trust_store_from_ca_file(struct s2n_x509_trust_store *store, const char *ca_pem_filename, const char *ca_dir)
{
    if (!store->trust_store) {
        store->trust_store = X509_STORE_new();
        POSIX_ENSURE_REF(store->trust_store);
    }

    int err_code = X509_STORE_load_locations(store->trust_store, ca_pem_filename, ca_dir);
    if (!err_code) {
        s2n_x509_trust_store_wipe(store);
        POSIX_BAIL(S2N_ERR_X509_TRUST_STORE);
    }

    return 0;
}

void s2n_x509_trust_store_wipe(struct s2n_x509_trust_store *store)
{
    if (store->trust_store) {
        X509_STORE_free(store->trust_store);
        store->trust_store = NULL;
        store->loaded_system_certs = false;
    }
}

int s2n_x509_validator_init_no_x509_validation(struct s2n_x509_validator *validator)
{
    POSIX_ENSURE_REF(validator);
    validator->trust_store = NULL;
    validator->store_ctx = NULL;
    validator->skip_cert_validation = 1;
    validator->check_stapled_ocsp = 0;
    validator->max_chain_depth = DEFAULT_MAX_CHAIN_DEPTH;
    validator->state = INIT;
    validator->cert_chain_from_wire = sk_X509_new_null();
    validator->crl_lookup_list = NULL;

    return 0;
}

int s2n_x509_validator_init(struct s2n_x509_validator *validator, struct s2n_x509_trust_store *trust_store, uint8_t check_ocsp)
{
    POSIX_ENSURE_REF(trust_store);
    validator->trust_store = trust_store;
    validator->skip_cert_validation = 0;
    validator->check_stapled_ocsp = check_ocsp;
    validator->max_chain_depth = DEFAULT_MAX_CHAIN_DEPTH;
    validator->store_ctx = NULL;
    if (validator->trust_store->trust_store) {
        validator->store_ctx = X509_STORE_CTX_new();
        POSIX_ENSURE_REF(validator->store_ctx);
    }
    validator->cert_chain_from_wire = sk_X509_new_null();
    validator->state = INIT;
    validator->crl_lookup_list = NULL;

    return 0;
}

static inline void wipe_cert_chain(STACK_OF(X509) *cert_chain)
{
    if (cert_chain) {
        sk_X509_pop_free(cert_chain, X509_free);
    }
}

int s2n_x509_validator_wipe(struct s2n_x509_validator *validator)
{
    if (validator->store_ctx) {
        X509_STORE_CTX_free(validator->store_ctx);
        validator->store_ctx = NULL;
    }
    wipe_cert_chain(validator->cert_chain_from_wire);
    validator->cert_chain_from_wire = NULL;
    validator->trust_store = NULL;
    validator->skip_cert_validation = 0;
    validator->state = UNINIT;
    validator->max_chain_depth = 0;
    if (validator->crl_lookup_list) {
        POSIX_GUARD_RESULT(s2n_array_free(validator->crl_lookup_list));
        validator->crl_lookup_list = NULL;
    }

    return S2N_SUCCESS;
}

int s2n_x509_validator_set_max_chain_depth(struct s2n_x509_validator *validator, uint16_t max_depth)
{
    POSIX_ENSURE_REF(validator);
    S2N_ERROR_IF(max_depth == 0, S2N_ERR_INVALID_ARGUMENT);

    validator->max_chain_depth = max_depth;
    return 0;
}

static S2N_RESULT s2n_verify_host_information_san_entry(struct s2n_connection *conn, GENERAL_NAME *current_name, bool *san_found)
{
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE_REF(current_name);
    RESULT_ENSURE_REF(san_found);

    if (current_name->type == GEN_DNS || current_name->type == GEN_URI) {
        *san_found = true;

        const char *name = (const char *) ASN1_STRING_data(current_name->d.ia5);
        RESULT_ENSURE_REF(name);
        int name_len = ASN1_STRING_length(current_name->d.ia5);
        RESULT_ENSURE_GT(name_len, 0);

        RESULT_ENSURE(conn->verify_host_fn(name, name_len, conn->data_for_verify_host), S2N_ERR_CERT_UNTRUSTED);

        return S2N_RESULT_OK;
    }

    if (current_name->type == GEN_IPADD) {
        *san_found = true;

        /* try to validate an IP address if it's in the subject alt name. */
        const unsigned char *ip_addr = current_name->d.iPAddress->data;
        RESULT_ENSURE_REF(ip_addr);
        int ip_addr_len = current_name->d.iPAddress->length;
        RESULT_ENSURE_GT(ip_addr_len, 0);

        RESULT_STACK_BLOB(address, INET6_ADDRSTRLEN + 1, INET6_ADDRSTRLEN + 1);

        if (ip_addr_len == 4) {
            RESULT_GUARD(s2n_inet_ntop(AF_INET, ip_addr, &address));
        } else if (ip_addr_len == 16) {
            RESULT_GUARD(s2n_inet_ntop(AF_INET6, ip_addr, &address));
        } else {
            /* we aren't able to parse this value so skip it */
            RESULT_BAIL(S2N_ERR_CERT_UNTRUSTED);
        }

        /* strlen should be safe here since we made sure we were null terminated AND that inet_ntop succeeded */
        const char *name = (const char *) address.data;
        size_t name_len = strlen(name);

        RESULT_ENSURE(conn->verify_host_fn(name, name_len, conn->data_for_verify_host), S2N_ERR_CERT_UNTRUSTED);

        return S2N_RESULT_OK;
    }

    /* we don't understand this entry type so skip it */
    RESULT_BAIL(S2N_ERR_CERT_UNTRUSTED);
}

static S2N_RESULT s2n_verify_host_information_san(struct s2n_connection *conn, X509 *public_cert, bool *san_found)
{
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE_REF(public_cert);
    RESULT_ENSURE_REF(san_found);

    *san_found = false;

    DEFER_CLEANUP(STACK_OF(GENERAL_NAME) *names_list = NULL, GENERAL_NAMES_free_pointer);
    names_list = X509_get_ext_d2i(public_cert, NID_subject_alt_name, NULL, NULL);
    RESULT_ENSURE(names_list, S2N_ERR_CERT_UNTRUSTED);

    int n = sk_GENERAL_NAME_num(names_list);
    RESULT_ENSURE(n > 0, S2N_ERR_CERT_UNTRUSTED);

    s2n_result result = S2N_RESULT_OK;
    for (int i = 0; i < n; i++) {
        GENERAL_NAME *current_name = sk_GENERAL_NAME_value(names_list, i);

        /* return success on the first entry that passes verification */
        result = s2n_verify_host_information_san_entry(conn, current_name, san_found);
        if (s2n_result_is_ok(result)) {
            return S2N_RESULT_OK;
        }
    }

    /* if an error was set by one of the entries, then just propagate the error from the last SAN entry call */
    RESULT_GUARD(result);

    RESULT_BAIL(S2N_ERR_CERT_UNTRUSTED);
}

static S2N_RESULT s2n_verify_host_information_common_name(struct s2n_connection *conn, X509 *public_cert, bool *cn_found)
{
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE_REF(public_cert);
    RESULT_ENSURE_REF(cn_found);

    X509_NAME *subject_name = X509_get_subject_name(public_cert);
    RESULT_ENSURE(subject_name, S2N_ERR_CERT_UNTRUSTED);

    int curr_idx = -1;
    while (true) {
        int next_idx = X509_NAME_get_index_by_NID(subject_name, NID_commonName, curr_idx);
        if (next_idx >= 0) {
            curr_idx = next_idx;
        } else {
            break;
        }
    }

    RESULT_ENSURE(curr_idx >= 0, S2N_ERR_CERT_UNTRUSTED);

    ASN1_STRING *common_name = X509_NAME_ENTRY_get_data(X509_NAME_get_entry(subject_name, curr_idx));
    RESULT_ENSURE(common_name, S2N_ERR_CERT_UNTRUSTED);

    /* X520CommonName allows the following ANSI string types per RFC 5280 Appendix A.1 */
    RESULT_ENSURE(ASN1_STRING_type(common_name) == V_ASN1_TELETEXSTRING
                    || ASN1_STRING_type(common_name) == V_ASN1_PRINTABLESTRING
                    || ASN1_STRING_type(common_name) == V_ASN1_UNIVERSALSTRING
                    || ASN1_STRING_type(common_name) == V_ASN1_UTF8STRING
                    || ASN1_STRING_type(common_name) == V_ASN1_BMPSTRING,
            S2N_ERR_CERT_UNTRUSTED);

    /* at this point we have a valid CN value */
    *cn_found = true;

    char peer_cn[255] = { 0 };
    int cn_len = ASN1_STRING_length(common_name);
    RESULT_ENSURE_GT(cn_len, 0);
    uint32_t len = (uint32_t) cn_len;
    RESULT_ENSURE_LTE(len, s2n_array_len(peer_cn) - 1);
    RESULT_CHECKED_MEMCPY(peer_cn, ASN1_STRING_data(common_name), len);
    RESULT_ENSURE(conn->verify_host_fn(peer_cn, len, conn->data_for_verify_host), S2N_ERR_CERT_UNTRUSTED);

    return S2N_RESULT_OK;
}

/*
 * For each name in the cert. Iterate them. Call the callback. If one returns true, then consider it validated,
 * if none of them return true, the cert is considered invalid.
 */
static S2N_RESULT s2n_verify_host_information(struct s2n_connection *conn, X509 *public_cert)
{
    bool entry_found = false;

    /* Check SubjectAltNames before CommonName as per RFC 6125 6.4.4 */
    s2n_result result = s2n_verify_host_information_san(conn, public_cert, &entry_found);

    /*
     *= https://www.rfc-editor.org/rfc/rfc6125#section-6.4.4
     *# As noted, a client MUST NOT seek a match for a reference identifier
     *# of CN-ID if the presented identifiers include a DNS-ID, SRV-ID,
     *# URI-ID, or any application-specific identifier types supported by the
     *# client.
     */
    if (entry_found) {
        return result;
    }

    /*
     *= https://www.rfc-editor.org/rfc/rfc6125#section-6.4.4
     *# Therefore, if and only if the presented identifiers do not include a
     *# DNS-ID, SRV-ID, URI-ID, or any application-specific identifier types
     *# supported by the client, then the client MAY as a last resort check
     *# for a string whose form matches that of a fully qualified DNS domain
     *# name in a Common Name field of the subject field (i.e., a CN-ID).
     */
    result = s2n_verify_host_information_common_name(conn, public_cert, &entry_found);
    if (entry_found) {
        return result;
    }

    /* make a null-terminated string in case the callback tries to use strlen */
    const char *name = "";
    size_t name_len = 0;

    /* at this point, we don't have anything to identify the certificate with so pass an empty string to the callback */
    RESULT_ENSURE(conn->verify_host_fn(name, name_len, conn->data_for_verify_host), S2N_ERR_CERT_UNTRUSTED);

    return S2N_RESULT_OK;
}

S2N_RESULT s2n_x509_validator_read_asn1_cert(struct s2n_stuffer *cert_chain_in_stuffer,
        struct s2n_blob *asn1_cert)
{
    uint32_t certificate_size = 0;

    RESULT_GUARD_POSIX(s2n_stuffer_read_uint24(cert_chain_in_stuffer, &certificate_size));
    RESULT_ENSURE(certificate_size > 0, S2N_ERR_CERT_INVALID);
    RESULT_ENSURE(certificate_size <= s2n_stuffer_data_available(cert_chain_in_stuffer), S2N_ERR_CERT_INVALID);

    asn1_cert->size = certificate_size;
    asn1_cert->data = s2n_stuffer_raw_read(cert_chain_in_stuffer, certificate_size);
    RESULT_ENSURE_REF(asn1_cert->data);

    return S2N_RESULT_OK;
}

/**
* Validates that each certificate in a peer's cert chain contains only signature algorithms in a security policy's
* certificate_signatures_preference list.
*/
S2N_RESULT s2n_x509_validator_check_cert_preferences(struct s2n_connection *conn, X509 *cert)
{
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE_REF(cert);

    const struct s2n_security_policy *security_policy = NULL;
    RESULT_GUARD_POSIX(s2n_connection_get_security_policy(conn, &security_policy));

    /**
     * We only restrict the signature algorithm on the certificates in the
     * peer's certificate chain if the certificate_signature_preferences field
     * is set in the security policy. This is contrary to the RFC, which
     * specifies that the signatures in the "signature_algorithms" extension
     * apply to signatures in the certificate chain in certain scenarios, so RFC
     * compliance would imply validating that the certificate chain signature
     * algorithm matches one of the algorithms specified in the
     * "signature_algorithms" extension.
     *
     *= https://www.rfc-editor.org/rfc/rfc5246#section-7.4.2
     *= type=exception
     *= reason=not implemented due to lack of utility
     *# If the client provided a "signature_algorithms" extension, then all
     *# certificates provided by the server MUST be signed by a
     *# hash/signature algorithm pair that appears in that extension.
     *
     *= https://www.rfc-editor.org/rfc/rfc8446#section-4.2.3
     *= type=exception
     *= reason=not implemented due to lack of utility
     *# If no "signature_algorithms_cert" extension is present, then the
     *# "signature_algorithms" extension also applies to signatures appearing in
     *# certificates.
     */
    struct s2n_cert_info info = { 0 };
    RESULT_GUARD(s2n_openssl_x509_get_cert_info(cert, &info));

    bool certificate_preferences_defined = security_policy->certificate_signature_preferences != NULL
            || security_policy->certificate_key_preferences != NULL;
    if (certificate_preferences_defined && !info.self_signed && conn->actual_protocol_version == S2N_TLS13) {
        /* Ensure that the certificate signature does not use SHA-1. While this check
         * would ideally apply to all connections, we only enforce it when certificate
         * preferences exist to stay backwards compatible.
         */
        RESULT_ENSURE(info.signature_digest_nid != NID_sha1, S2N_ERR_CERT_UNTRUSTED);
    }

    if (!info.self_signed) {
        RESULT_GUARD(s2n_security_policy_validate_cert_signature(security_policy, &info, S2N_ERR_CERT_UNTRUSTED));
    }
    RESULT_GUARD(s2n_security_policy_validate_cert_key(security_policy, &info, S2N_ERR_CERT_UNTRUSTED));

    return S2N_RESULT_OK;
}

/* Validates that the root certificate uses a key allowed by the security policy
 * certificate preferences.
 */
static S2N_RESULT s2n_x509_validator_check_root_cert(struct s2n_x509_validator *validator, struct s2n_connection *conn)
{
    RESULT_ENSURE_REF(validator);
    RESULT_ENSURE_REF(conn);

    const struct s2n_security_policy *security_policy = NULL;
    RESULT_GUARD_POSIX(s2n_connection_get_security_policy(conn, &security_policy));
    RESULT_ENSURE_REF(security_policy);

    RESULT_ENSURE_REF(validator->store_ctx);
    DEFER_CLEANUP(STACK_OF(X509) *cert_chain = X509_STORE_CTX_get1_chain(validator->store_ctx),
            s2n_openssl_x509_stack_pop_free);
    RESULT_ENSURE_REF(cert_chain);

    const int certs_in_chain = sk_X509_num(cert_chain);
    RESULT_ENSURE(certs_in_chain > 0, S2N_ERR_CERT_UNTRUSTED);
    X509 *root = sk_X509_value(cert_chain, certs_in_chain - 1);
    RESULT_ENSURE_REF(root);

    struct s2n_cert_info info = { 0 };
    RESULT_GUARD(s2n_openssl_x509_get_cert_info(root, &info));

    RESULT_GUARD(s2n_security_policy_validate_cert_key(security_policy, &info,
            S2N_ERR_SECURITY_POLICY_INCOMPATIBLE_CERT));

    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_x509_validator_read_cert_chain(struct s2n_x509_validator *validator, struct s2n_connection *conn,
        uint8_t *cert_chain_in, uint32_t cert_chain_len)
{
    RESULT_ENSURE(validator->skip_cert_validation || s2n_x509_trust_store_has_certs(validator->trust_store), S2N_ERR_CERT_UNTRUSTED);
    RESULT_ENSURE(validator->state == INIT, S2N_ERR_INVALID_CERT_STATE);

    struct s2n_blob cert_chain_blob = { 0 };
    RESULT_GUARD_POSIX(s2n_blob_init(&cert_chain_blob, cert_chain_in, cert_chain_len));
    DEFER_CLEANUP(struct s2n_stuffer cert_chain_in_stuffer = { 0 }, s2n_stuffer_free);

    RESULT_GUARD_POSIX(s2n_stuffer_init(&cert_chain_in_stuffer, &cert_chain_blob));
    RESULT_GUARD_POSIX(s2n_stuffer_write(&cert_chain_in_stuffer, &cert_chain_blob));

    while (s2n_stuffer_data_available(&cert_chain_in_stuffer)
            && sk_X509_num(validator->cert_chain_from_wire) < validator->max_chain_depth) {
        struct s2n_blob asn1_cert = { 0 };
        RESULT_GUARD(s2n_x509_validator_read_asn1_cert(&cert_chain_in_stuffer, &asn1_cert));

        /* We only do the trailing byte validation when parsing the leaf cert to
         * match historical s2n-tls behavior.
         */
        DEFER_CLEANUP(X509 *cert = NULL, X509_free_pointer);
        if (sk_X509_num(validator->cert_chain_from_wire) == 0) {
            RESULT_GUARD(s2n_openssl_x509_parse(&asn1_cert, &cert));
        } else {
            RESULT_GUARD(s2n_openssl_x509_parse_without_length_validation(&asn1_cert, &cert));
        }

        if (!validator->skip_cert_validation) {
            RESULT_GUARD(s2n_x509_validator_check_cert_preferences(conn, cert));
        }

        /* add the cert to the chain */
        RESULT_ENSURE(sk_X509_push(validator->cert_chain_from_wire, cert) > 0,
                S2N_ERR_INTERNAL_LIBCRYPTO_ERROR);

        /* After the cert is added to cert_chain_from_wire, it will be freed
         * with the call to s2n_x509_validator_wipe. We disable the cleanup
         * function since cleanup is no longer "owned" by cert.
         */
        ZERO_TO_DISABLE_DEFER_CLEANUP(cert);

        /* certificate extensions is a field in TLS 1.3 - https://tools.ietf.org/html/rfc8446#section-4.4.2 */
        if (conn->actual_protocol_version >= S2N_TLS13) {
            s2n_parsed_extensions_list parsed_extensions_list = { 0 };
            RESULT_GUARD_POSIX(s2n_extension_list_parse(&cert_chain_in_stuffer, &parsed_extensions_list));
        }
    }

    /* if this occurred we exceeded validator->max_chain_depth */
    RESULT_ENSURE(validator->skip_cert_validation || s2n_stuffer_data_available(&cert_chain_in_stuffer) == 0,
            S2N_ERR_CERT_MAX_CHAIN_DEPTH_EXCEEDED);
    RESULT_ENSURE(sk_X509_num(validator->cert_chain_from_wire) > 0, S2N_ERR_NO_CERT_FOUND);

    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_x509_validator_process_cert_chain(struct s2n_x509_validator *validator, struct s2n_connection *conn,
        uint8_t *cert_chain_in, uint32_t cert_chain_len)
{
    RESULT_ENSURE(validator->state == INIT, S2N_ERR_INVALID_CERT_STATE);

    RESULT_GUARD(s2n_x509_validator_read_cert_chain(validator, conn, cert_chain_in, cert_chain_len));

    if (validator->skip_cert_validation) {
        return S2N_RESULT_OK;
    }

    X509 *leaf = sk_X509_value(validator->cert_chain_from_wire, 0);
    RESULT_ENSURE_REF(leaf);

    if (conn->verify_host_fn) {
        RESULT_GUARD(s2n_verify_host_information(conn, leaf));
    }

    RESULT_GUARD_OSSL(X509_STORE_CTX_init(validator->store_ctx, validator->trust_store->trust_store, leaf,
                              validator->cert_chain_from_wire),
            S2N_ERR_INTERNAL_LIBCRYPTO_ERROR);

    if (conn->config->crl_lookup_cb) {
        RESULT_GUARD(s2n_crl_invoke_lookup_callbacks(conn, validator));
        RESULT_GUARD(s2n_crl_handle_lookup_callback_result(validator));
    }

    validator->state = READY_TO_VERIFY;

    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_x509_validator_set_no_check_time_flag(struct s2n_x509_validator *validator)
{
    RESULT_ENSURE_REF(validator);
    RESULT_ENSURE_REF(validator->store_ctx);

    X509_VERIFY_PARAM *param = X509_STORE_CTX_get0_param(validator->store_ctx);
    RESULT_ENSURE_REF(param);

#ifdef S2N_LIBCRYPTO_SUPPORTS_FLAG_NO_CHECK_TIME
    RESULT_GUARD_OSSL(X509_VERIFY_PARAM_set_flags(param, X509_V_FLAG_NO_CHECK_TIME),
            S2N_ERR_INTERNAL_LIBCRYPTO_ERROR);
#else
    RESULT_BAIL(S2N_ERR_UNIMPLEMENTED);
#endif

    return S2N_RESULT_OK;
}

int s2n_disable_time_validation_ossl_verify_callback(int default_ossl_ret, X509_STORE_CTX *ctx)
{
    int err = X509_STORE_CTX_get_error(ctx);
    switch (err) {
        case X509_V_ERR_CERT_NOT_YET_VALID:
        case X509_V_ERR_CERT_HAS_EXPIRED:
            return OSSL_VERIFY_CALLBACK_IGNORE_ERROR;
        default:
            break;
    }

    /* If CRL validation is enabled, setting the time validation verify callback will override the
     * CRL verify callback. The CRL verify callback is manually triggered to work around this
     * issue.
     *
     * The CRL verify callback ignores validation errors exclusively for CRL timestamp fields. So,
     * if CRL validation isn't enabled, the CRL verify callback is a no-op.
     */
    return s2n_crl_ossl_verify_callback(default_ossl_ret, ctx);
}

static S2N_RESULT s2n_x509_validator_disable_time_validation(struct s2n_connection *conn,
        struct s2n_x509_validator *validator)
{
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE_REF(conn->config);
    RESULT_ENSURE_REF(validator);
    RESULT_ENSURE_REF(validator->store_ctx);

    /* Setting an X509_STORE verify callback is not recommended with AWS-LC:
     * https://github.com/aws/aws-lc/blob/aa90e509f2e940916fbe9fdd469a4c90c51824f6/include/openssl/x509.h#L2980-L2990
     *
     * If the libcrypto supports the ability to disable time validation with an X509_VERIFY_PARAM
     * NO_CHECK_TIME flag, this method is preferred.
     *
     * However, older versions of AWS-LC and OpenSSL 1.0.2 do not support this flag. In this case,
     * an X509_STORE verify callback is used. This is acceptable in older versions of AWS-LC
     * because the versions are fixed, and updates to AWS-LC will not break the callback
     * implementation.
     */
    if (s2n_libcrypto_supports_flag_no_check_time()) {
        RESULT_GUARD(s2n_x509_validator_set_no_check_time_flag(validator));
    } else {
        X509_STORE_CTX_set_verify_cb(validator->store_ctx,
                s2n_disable_time_validation_ossl_verify_callback);
    }

    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_x509_validator_verify_cert_chain(struct s2n_x509_validator *validator, struct s2n_connection *conn)
{
    RESULT_ENSURE(validator->state == READY_TO_VERIFY, S2N_ERR_INVALID_CERT_STATE);

    X509_VERIFY_PARAM *param = X509_STORE_CTX_get0_param(validator->store_ctx);
    X509_VERIFY_PARAM_set_depth(param, validator->max_chain_depth);

    DEFER_CLEANUP(STACK_OF(X509_CRL) *crl_stack = NULL, sk_X509_CRL_free_pointer);

    if (conn->config->crl_lookup_cb) {
        X509_STORE_CTX_set_verify_cb(validator->store_ctx, s2n_crl_ossl_verify_callback);

        crl_stack = sk_X509_CRL_new_null();
        RESULT_GUARD(s2n_crl_get_crls_from_lookup_list(validator, crl_stack));

        /* Set the CRL list that the libcrypto will use to validate certificates with */
        X509_STORE_CTX_set0_crls(validator->store_ctx, crl_stack);

        /* Enable CRL validation for certificates in X509_verify_cert */
        RESULT_GUARD_OSSL(X509_VERIFY_PARAM_set_flags(param, X509_V_FLAG_CRL_CHECK),
                S2N_ERR_INTERNAL_LIBCRYPTO_ERROR);

        /* Enable CRL validation for all certificates, not just the leaf */
        RESULT_GUARD_OSSL(X509_VERIFY_PARAM_set_flags(param, X509_V_FLAG_CRL_CHECK_ALL),
                S2N_ERR_INTERNAL_LIBCRYPTO_ERROR);
    }

    /* Disabling time validation may set a NO_CHECK_TIME flag on the X509_STORE_CTX. Calling
     * X509_STORE_CTX_set_time will override this flag. To prevent this, X509_STORE_CTX_set_time is
     * only called if time validation is enabled.
     */
    if (conn->config->disable_x509_time_validation) {
        RESULT_GUARD(s2n_x509_validator_disable_time_validation(conn, validator));
    } else {
        uint64_t current_sys_time = 0;
        RESULT_GUARD(s2n_config_wall_clock(conn->config, &current_sys_time));
        if (sizeof(time_t) == 4) {
            /* cast value to uint64_t to prevent overflow errors */
            RESULT_ENSURE_LTE(current_sys_time, (uint64_t) MAX_32_TIMESTAMP_NANOS);
        }

        /* this wants seconds not nanoseconds */
        time_t current_time = (time_t) (current_sys_time / ONE_SEC_IN_NANOS);
        X509_STORE_CTX_set_time(validator->store_ctx, 0, current_time);
    }

    /* It's assumed that if a valid certificate chain is received with an issuer that's present in
     * the trust store, the certificate chain should be trusted. This should be the case even if
     * the issuer in the trust store isn't a root certificate. Setting the PARTIAL_CHAIN flag
     * allows the libcrypto to trust certificates in the trust store that aren't root certificates.
     */
    X509_STORE_CTX_set_flags(validator->store_ctx, X509_V_FLAG_PARTIAL_CHAIN);

    int verify_ret = X509_verify_cert(validator->store_ctx);
    if (verify_ret <= 0) {
        int ossl_error = X509_STORE_CTX_get_error(validator->store_ctx);
        switch (ossl_error) {
            case X509_V_ERR_CERT_NOT_YET_VALID:
                RESULT_BAIL(S2N_ERR_CERT_NOT_YET_VALID);
            case X509_V_ERR_CERT_HAS_EXPIRED:
                RESULT_BAIL(S2N_ERR_CERT_EXPIRED);
            case X509_V_ERR_CERT_REVOKED:
                RESULT_BAIL(S2N_ERR_CERT_REVOKED);
            case X509_V_ERR_UNABLE_TO_GET_CRL:
            case X509_V_ERR_DIFFERENT_CRL_SCOPE:
                RESULT_BAIL(S2N_ERR_CRL_LOOKUP_FAILED);
            case X509_V_ERR_CRL_SIGNATURE_FAILURE:
                RESULT_BAIL(S2N_ERR_CRL_SIGNATURE);
            case X509_V_ERR_UNABLE_TO_GET_CRL_ISSUER:
                RESULT_BAIL(S2N_ERR_CRL_ISSUER);
            case X509_V_ERR_UNHANDLED_CRITICAL_CRL_EXTENSION:
                RESULT_BAIL(S2N_ERR_CRL_UNHANDLED_CRITICAL_EXTENSION);
            default:
                RESULT_BAIL(S2N_ERR_CERT_UNTRUSTED);
        }
    }

    validator->state = VALIDATED;

    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_x509_validator_parse_leaf_certificate_extensions(struct s2n_connection *conn,
        uint8_t *cert_chain_in, uint32_t cert_chain_len,
        s2n_parsed_extensions_list *first_certificate_extensions)
{
    /* certificate extensions is a field in TLS 1.3 - https://tools.ietf.org/html/rfc8446#section-4.4.2 */
    RESULT_ENSURE_GTE(conn->actual_protocol_version, S2N_TLS13);

    struct s2n_blob cert_chain_blob = { 0 };
    RESULT_GUARD_POSIX(s2n_blob_init(&cert_chain_blob, cert_chain_in, cert_chain_len));
    DEFER_CLEANUP(struct s2n_stuffer cert_chain_in_stuffer = { 0 }, s2n_stuffer_free);

    RESULT_GUARD_POSIX(s2n_stuffer_init(&cert_chain_in_stuffer, &cert_chain_blob));
    RESULT_GUARD_POSIX(s2n_stuffer_write(&cert_chain_in_stuffer, &cert_chain_blob));

    struct s2n_blob asn1_cert = { 0 };
    RESULT_GUARD(s2n_x509_validator_read_asn1_cert(&cert_chain_in_stuffer, &asn1_cert));

    s2n_parsed_extensions_list parsed_extensions_list = { 0 };
    RESULT_GUARD_POSIX(s2n_extension_list_parse(&cert_chain_in_stuffer, &parsed_extensions_list));
    *first_certificate_extensions = parsed_extensions_list;

    return S2N_RESULT_OK;
}

S2N_RESULT s2n_x509_validator_validate_cert_chain(struct s2n_x509_validator *validator, struct s2n_connection *conn,
        uint8_t *cert_chain_in, uint32_t cert_chain_len, s2n_pkey_type *pkey_type, struct s2n_pkey *public_key_out)
{
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE_REF(conn->config);

    switch (validator->state) {
        case INIT:
            break;
        case AWAITING_CRL_CALLBACK:
            RESULT_GUARD(s2n_crl_handle_lookup_callback_result(validator));
            break;
        default:
            RESULT_BAIL(S2N_ERR_INVALID_CERT_STATE);
    }

    if (validator->state == INIT) {
        RESULT_GUARD(s2n_x509_validator_process_cert_chain(validator, conn, cert_chain_in, cert_chain_len));
    }

    if (validator->state == READY_TO_VERIFY) {
        RESULT_GUARD(s2n_x509_validator_verify_cert_chain(validator, conn));
        RESULT_GUARD(s2n_x509_validator_check_root_cert(validator, conn));
    }

    if (conn->actual_protocol_version >= S2N_TLS13) {
        /* Only process certificate extensions received in the first certificate. Extensions received in all other
         * certificates are ignored.
         *
         *= https://www.rfc-editor.org/rfc/rfc8446#section-4.4.2
         *# If an extension applies to the entire chain, it SHOULD be included in
         *# the first CertificateEntry.
         */
        s2n_parsed_extensions_list first_certificate_extensions = { 0 };
        RESULT_GUARD(s2n_x509_validator_parse_leaf_certificate_extensions(conn, cert_chain_in, cert_chain_len, &first_certificate_extensions));
        RESULT_GUARD_POSIX(s2n_extension_list_process(S2N_EXTENSION_LIST_CERTIFICATE, conn, &first_certificate_extensions));
    }

    if (conn->config->cert_validation_cb) {
        struct s2n_cert_validation_info info = { 0 };
        RESULT_ENSURE(conn->config->cert_validation_cb(conn, &info, conn->config->cert_validation_ctx) >= S2N_SUCCESS,
                S2N_ERR_CANCELLED);
        RESULT_ENSURE(info.finished, S2N_ERR_INVALID_STATE);
        RESULT_ENSURE(info.accepted, S2N_ERR_CERT_REJECTED);
    }

    /* retrieve information from leaf cert */
    RESULT_ENSURE_GT(sk_X509_num(validator->cert_chain_from_wire), 0);
    X509 *leaf_cert = sk_X509_value(validator->cert_chain_from_wire, 0);
    RESULT_ENSURE_REF(leaf_cert);
    DEFER_CLEANUP(struct s2n_pkey public_key = { 0 }, s2n_pkey_free);
    RESULT_GUARD(s2n_pkey_from_x509(leaf_cert, &public_key, pkey_type));

    *public_key_out = public_key;

    /* Reset the old struct, so we don't clean up public_key_out */
    ZERO_TO_DISABLE_DEFER_CLEANUP(public_key);

    return S2N_RESULT_OK;
}

S2N_RESULT s2n_x509_validator_validate_cert_stapled_ocsp_response(struct s2n_x509_validator *validator,
        struct s2n_connection *conn, const uint8_t *ocsp_response_raw, uint32_t ocsp_response_length)
{
    if (validator->skip_cert_validation || !validator->check_stapled_ocsp) {
        validator->state = OCSP_VALIDATED;
        return S2N_RESULT_OK;
    }

    RESULT_ENSURE(validator->state == VALIDATED, S2N_ERR_INVALID_CERT_STATE);

#if !S2N_OCSP_STAPLING_SUPPORTED
    /* Default to safety */
    RESULT_BAIL(S2N_ERR_CERT_UNTRUSTED);
#else

    RESULT_ENSURE_REF(ocsp_response_raw);

    DEFER_CLEANUP(OCSP_RESPONSE *ocsp_response = d2i_OCSP_RESPONSE(NULL, &ocsp_response_raw, ocsp_response_length),
            OCSP_RESPONSE_free_pointer);
    RESULT_ENSURE(ocsp_response != NULL, S2N_ERR_INVALID_OCSP_RESPONSE);

    int ocsp_status = OCSP_response_status(ocsp_response);
    RESULT_ENSURE(ocsp_status == OCSP_RESPONSE_STATUS_SUCCESSFUL, S2N_ERR_CERT_UNTRUSTED);

    DEFER_CLEANUP(OCSP_BASICRESP *basic_response = OCSP_response_get1_basic(ocsp_response), OCSP_BASICRESP_free_pointer);
    RESULT_ENSURE(basic_response != NULL, S2N_ERR_INVALID_OCSP_RESPONSE);

    /* X509_STORE_CTX_get0_chain() is better because it doesn't return a copy. But it's not available for Openssl 1.0.2.
     * Therefore, we call this variant and clean it up at the end of the function.
     * See the comments here:
     * https://www.openssl.org/docs/man1.0.2/man3/X509_STORE_CTX_get1_chain.html
     */
    DEFER_CLEANUP(STACK_OF(X509) *cert_chain = X509_STORE_CTX_get1_chain(validator->store_ctx),
            s2n_openssl_x509_stack_pop_free);
    RESULT_ENSURE_REF(cert_chain);

    const int certs_in_chain = sk_X509_num(cert_chain);
    RESULT_ENSURE(certs_in_chain > 0, S2N_ERR_NO_CERT_FOUND);

    /* leaf is the top: not the bottom. */
    X509 *subject = sk_X509_value(cert_chain, 0);
    X509 *issuer = NULL;
    /* find the issuer in the chain. If it's not there. Fail everything. */
    for (int i = 0; i < certs_in_chain; ++i) {
        X509 *issuer_candidate = sk_X509_value(cert_chain, i);
        const int issuer_value = X509_check_issued(issuer_candidate, subject);

        if (issuer_value == X509_V_OK) {
            issuer = issuer_candidate;
            break;
        }
    }
    RESULT_ENSURE(issuer != NULL, S2N_ERR_CERT_UNTRUSTED);

    /* Important: this checks that the stapled ocsp response CAN be verified, not that it has been verified. */
    const int ocsp_verify_res = OCSP_basic_verify(basic_response, cert_chain, validator->trust_store->trust_store, 0);
    RESULT_GUARD_OSSL(ocsp_verify_res, S2N_ERR_CERT_UNTRUSTED);

    /* do the crypto checks on the response.*/
    int status = 0;
    int reason = 0;

    /* SHA-1 is the only supported hash algorithm for the CertID due to its established use in 
     * OCSP responders. 
     */
    OCSP_CERTID *cert_id = OCSP_cert_to_id(EVP_sha1(), subject, issuer);
    RESULT_ENSURE_REF(cert_id);

    /**
     *= https://www.rfc-editor.org/rfc/rfc6960#section-2.4
     *#
     *# thisUpdate      The most recent time at which the status being
     *#                 indicated is known by the responder to have been
     *#                 correct.
     *#
     *# nextUpdate      The time at or before which newer information will be
     *#                 available about the status of the certificate.
     **/
    ASN1_GENERALIZEDTIME *revtime = NULL, *thisupd = NULL, *nextupd = NULL;
    /* Actual verification of the response */
    const int ocsp_resp_find_status_res = OCSP_resp_find_status(basic_response, cert_id, &status, &reason, &revtime, &thisupd, &nextupd);
    OCSP_CERTID_free(cert_id);
    RESULT_GUARD_OSSL(ocsp_resp_find_status_res, S2N_ERR_CERT_UNTRUSTED);

    uint64_t current_sys_time_nanoseconds = 0;
    RESULT_GUARD(s2n_config_wall_clock(conn->config, &current_sys_time_nanoseconds));
    if (sizeof(time_t) == 4) {
        /* cast value to uint64_t to prevent overflow errors */
        RESULT_ENSURE_LTE(current_sys_time_nanoseconds, (uint64_t) MAX_32_TIMESTAMP_NANOS);
    }
    /* convert the current_sys_time (which is in nanoseconds) to seconds */
    time_t current_sys_time_seconds = (time_t) (current_sys_time_nanoseconds / ONE_SEC_IN_NANOS);

    DEFER_CLEANUP(ASN1_GENERALIZEDTIME *current_sys_time = ASN1_GENERALIZEDTIME_set(NULL, current_sys_time_seconds), s2n_openssl_asn1_time_free_pointer);
    RESULT_ENSURE_REF(current_sys_time);

    /**
     * It is fine to use ASN1_TIME functions with ASN1_GENERALIZEDTIME structures
     * From openssl documentation:
     * It is recommended that functions starting with ASN1_TIME be used instead
     * of those starting with ASN1_UTCTIME or ASN1_GENERALIZEDTIME. The
     * functions starting with ASN1_UTCTIME and ASN1_GENERALIZEDTIME act only on
     * that specific time format. The functions starting with ASN1_TIME will
     * operate on either format.
     * https://www.openssl.org/docs/man1.1.1/man3/ASN1_TIME_to_generalizedtime.html
     *
     * ASN1_TIME_compare has a much nicer API, but is not available in Openssl
     * 1.0.1, so we use ASN1_TIME_diff.
     */
    int pday = 0;
    int psec = 0;
    RESULT_GUARD_OSSL(ASN1_TIME_diff(&pday, &psec, thisupd, current_sys_time), S2N_ERR_CERT_UNTRUSTED);
    /* ensure that current_time is after or the same as "this update" */
    RESULT_ENSURE(pday >= 0 && psec >= 0, S2N_ERR_CERT_INVALID);

    /* ensure that current_time is before or the same as "next update" */
    if (nextupd) {
        RESULT_GUARD_OSSL(ASN1_TIME_diff(&pday, &psec, current_sys_time, nextupd), S2N_ERR_CERT_UNTRUSTED);
        RESULT_ENSURE(pday >= 0 && psec >= 0, S2N_ERR_CERT_EXPIRED);
    } else {
        /**
         * if nextupd isn't present, assume that nextupd is
         * DEFAULT_OCSP_NEXT_UPDATE_PERIOD after thisupd. This means that if the
         * current time is more than DEFAULT_OCSP_NEXT_UPDATE_PERIOD
         * seconds ahead of thisupd, we consider it invalid. We already compared
         * current_sys_time to thisupd, so reuse those values
         */
        uint64_t seconds_after_thisupd = pday * (3600 * 24) + psec;
        RESULT_ENSURE(seconds_after_thisupd < DEFAULT_OCSP_NEXT_UPDATE_PERIOD, S2N_ERR_CERT_EXPIRED);
    }

    switch (status) {
        case V_OCSP_CERTSTATUS_GOOD:
            validator->state = OCSP_VALIDATED;
            return S2N_RESULT_OK;
        case V_OCSP_CERTSTATUS_REVOKED:
            RESULT_BAIL(S2N_ERR_CERT_REVOKED);
        default:
            RESULT_BAIL(S2N_ERR_CERT_UNTRUSTED);
    }
#endif /* S2N_OCSP_STAPLING_SUPPORTED */
}

bool s2n_x509_validator_is_cert_chain_validated(const struct s2n_x509_validator *validator)
{
    return validator && (validator->state == VALIDATED || validator->state == OCSP_VALIDATED);
}

int s2n_cert_validation_accept(struct s2n_cert_validation_info *info)
{
    POSIX_ENSURE_REF(info);
    POSIX_ENSURE(!info->finished, S2N_ERR_INVALID_STATE);

    info->finished = true;
    info->accepted = true;

    return S2N_SUCCESS;
}

int s2n_cert_validation_reject(struct s2n_cert_validation_info *info)
{
    POSIX_ENSURE_REF(info);
    POSIX_ENSURE(!info->finished, S2N_ERR_INVALID_STATE);

    info->finished = true;
    info->accepted = false;

    return S2N_SUCCESS;
}

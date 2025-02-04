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

#include <openssl/x509v3.h>

#include "api/s2n.h"
#include "tls/s2n_signature_scheme.h"

/* one day, BoringSSL may add ocsp stapling support. Let's future proof this a bit by grabbing a definition
 * that would have to be there when they add support */
#if defined(OPENSSL_IS_BORINGSSL) && !defined(OCSP_RESPONSE_STATUS_SUCCESSFUL)
    #define S2N_OCSP_STAPLING_SUPPORTED 0
#else
    #define S2N_OCSP_STAPLING_SUPPORTED 1
#endif /* defined(OPENSSL_IS_BORINGSSL) && !defined(OCSP_RESPONSE_STATUS_SUCCESSFUL) */

typedef enum {
    UNINIT,
    INIT,
    READY_TO_VERIFY,
    AWAITING_CRL_CALLBACK,
    VALIDATED,
    OCSP_VALIDATED,
} validator_state;

/** Return TRUE for trusted, FALSE for untrusted **/
typedef uint8_t (*verify_host)(const char *host_name, size_t host_name_len, void *data);
struct s2n_connection;

/**
 * Trust store simply contains the trust store each connection should validate certs against.
 * For most use cases, you only need one of these per application.
 */
struct s2n_x509_trust_store {
    X509_STORE *trust_store;

    /* Indicates whether system default certs have been loaded into the trust store */
    unsigned loaded_system_certs : 1;
};

/**
 * You should have one instance of this per connection.
 */
struct s2n_x509_validator {
    struct s2n_x509_trust_store *trust_store;
    X509_STORE_CTX *store_ctx;
    uint8_t skip_cert_validation;
    uint8_t check_stapled_ocsp;
    uint16_t max_chain_depth;
    STACK_OF(X509) *cert_chain_from_wire;
    int state;
    struct s2n_array *crl_lookup_list;
};

struct s2n_cert_validation_info {
    unsigned finished : 1;
    unsigned accepted : 1;
};

/** Some libcrypto implementations do not support OCSP validation. Returns 1 if supported, 0 otherwise. */
uint8_t s2n_x509_ocsp_stapling_supported(void);

/** Initialize the trust store to empty defaults (no allocations happen here) */
void s2n_x509_trust_store_init_empty(struct s2n_x509_trust_store *store);

/** Returns TRUE if the trust store has certificates installed, FALSE otherwise */
uint8_t s2n_x509_trust_store_has_certs(struct s2n_x509_trust_store *store);

/** Initialize trust store from a PEM. This will allocate memory, and load PEM into the Trust Store **/
int s2n_x509_trust_store_add_pem(struct s2n_x509_trust_store *store, const char *pem);

/** Initialize trust store from a CA file. This will allocate memory, and load each cert in the file into the trust store
 *  Returns 0 on success, or S2N error codes on failure. */
int s2n_x509_trust_store_from_ca_file(struct s2n_x509_trust_store *store, const char *ca_pem_filename, const char *ca_dir);

/** Cleans up, and frees any underlying memory in the trust store. */
void s2n_x509_trust_store_wipe(struct s2n_x509_trust_store *store);

/** Initialize the validator in unsafe mode. No validity checks for OCSP, host checks, or X.509 will be performed. */
int s2n_x509_validator_init_no_x509_validation(struct s2n_x509_validator *validator);

/** Initialize the validator in safe mode. Will use trust store to validate x.509 certificates, ocsp responses, and will call
 *  the verify host callback to determine if a subject name or alternative name from the cert should be trusted.
 *  Returns 0 on success, and an S2N_ERR_* on failure.
 */
int s2n_x509_validator_init(struct s2n_x509_validator *validator, struct s2n_x509_trust_store *trust_store, uint8_t check_ocsp);

/**
 * Sets the maximum depth for a cert chain that can be used at validation.
 */
int s2n_x509_validator_set_max_chain_depth(struct s2n_x509_validator *validator, uint16_t max_depth);

/** Cleans up underlying memory and data members. Struct can be reused afterwards. */
int s2n_x509_validator_wipe(struct s2n_x509_validator *validator);

/**
 * Validates a certificate chain against the configured trust store in safe mode. In unsafe mode, it will find the public key
 * and return it but not validate the certificates. Alternative Names and Subject Name will be passed to the host verification callback.
 * The verification callback will be possibly called multiple times depending on how many names are found.
 * If any of those calls return TRUE, that stage of the validation will continue, otherwise once all names are tried and none matched as
 * trusted, the chain will be considered UNTRUSTED.
 *
 * This function can only be called once per instance of an s2n_x509_validator. If must be called prior to calling
 * s2n_x509_validator_validate_cert_stapled_ocsp_response().
 */
S2N_RESULT s2n_x509_validator_validate_cert_chain(struct s2n_x509_validator *validator, struct s2n_connection *conn,
        uint8_t *cert_chain_in, uint32_t cert_chain_len, s2n_pkey_type *pkey_type,
        struct s2n_pkey *public_key_out);

/**
 * Validates an ocsp response against the most recent certificate chain. Also verifies the timestamps on the response. This function can only be
 * called once per instance of an s2n_x509_validator and only after a successful call to s2n_x509_validator_validate_cert_chain().
 */
S2N_RESULT s2n_x509_validator_validate_cert_stapled_ocsp_response(struct s2n_x509_validator *validator, struct s2n_connection *conn,
        const uint8_t *ocsp_response, uint32_t size);

/**
 * Checks whether the peer's certificate chain has been received and validated.
 * Should be verified before any use of the peer's certificate data.
 */
bool s2n_x509_validator_is_cert_chain_validated(const struct s2n_x509_validator *validator);

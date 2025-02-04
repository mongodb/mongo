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

#ifndef _S2N_PRELUDE_INCLUDED
    /* make sure s2n_prelude.h is includes as part of the compiler flags, if not then fail the build */
    #error "Expected s2n_prelude.h to be included as part of the compiler flags"
#endif

#include <strings.h>
#include <time.h>

#include "api/unstable/npn.h"
#include "crypto/s2n_certificate.h"
#include "crypto/s2n_fips.h"
#include "crypto/s2n_hkdf.h"
#include "crypto/s2n_pq.h"
#include "error/s2n_errno.h"
#include "tls/s2n_cipher_preferences.h"
#include "tls/s2n_internal.h"
#include "tls/s2n_ktls.h"
#include "tls/s2n_security_policies.h"
#include "tls/s2n_tls13.h"
#include "utils/s2n_blob.h"
#include "utils/s2n_map.h"
#include "utils/s2n_safety.h"

#if defined(CLOCK_MONOTONIC_RAW)
    #define S2N_CLOCK_HW CLOCK_MONOTONIC_RAW
#else
    #define S2N_CLOCK_HW CLOCK_MONOTONIC
#endif

#define S2N_CLOCK_SYS CLOCK_REALTIME

static int monotonic_clock(void *data, uint64_t *nanoseconds)
{
    struct timespec current_time = { 0 };

    POSIX_GUARD(clock_gettime(S2N_CLOCK_HW, &current_time));

    *nanoseconds = (uint64_t) current_time.tv_sec * 1000000000ull;
    *nanoseconds += current_time.tv_nsec;

    return 0;
}

static int wall_clock(void *data, uint64_t *nanoseconds)
{
    struct timespec current_time = { 0 };

    POSIX_GUARD(clock_gettime(S2N_CLOCK_SYS, &current_time));

    *nanoseconds = (uint64_t) current_time.tv_sec * 1000000000ull;
    *nanoseconds += current_time.tv_nsec;

    return 0;
}

static struct s2n_config s2n_default_config = { 0 };
static struct s2n_config s2n_default_fips_config = { 0 };
static struct s2n_config s2n_default_tls13_config = { 0 };

static int s2n_config_setup_default(struct s2n_config *config)
{
    POSIX_GUARD(s2n_config_set_cipher_preferences(config, "default"));
    return S2N_SUCCESS;
}

static int s2n_config_setup_tls13(struct s2n_config *config)
{
    POSIX_GUARD(s2n_config_set_cipher_preferences(config, "default_tls13"));
    return S2N_SUCCESS;
}

static int s2n_config_setup_fips(struct s2n_config *config)
{
    POSIX_GUARD(s2n_config_set_cipher_preferences(config, "default_fips"));
    return S2N_SUCCESS;
}

static int s2n_config_init(struct s2n_config *config)
{
    config->wall_clock = wall_clock;
    config->monotonic_clock = monotonic_clock;
    config->ct_type = S2N_CT_SUPPORT_NONE;
    config->mfl_code = S2N_TLS_MAX_FRAG_LEN_EXT_NONE;
    config->alert_behavior = S2N_ALERT_FAIL_ON_WARNINGS;
    config->session_state_lifetime_in_nanos = S2N_STATE_LIFETIME_IN_NANOS;
    config->encrypt_decrypt_key_lifetime_in_nanos = S2N_TICKET_ENCRYPT_DECRYPT_KEY_LIFETIME_IN_NANOS;
    config->decrypt_key_lifetime_in_nanos = S2N_TICKET_DECRYPT_KEY_LIFETIME_IN_NANOS;
    config->async_pkey_validation_mode = S2N_ASYNC_PKEY_VALIDATION_FAST;
    config->check_ocsp = 1;

    config->client_hello_cb_mode = S2N_CLIENT_HELLO_CB_BLOCKING;

    POSIX_GUARD(s2n_config_setup_default(config));
    if (s2n_use_default_tls13_config()) {
        POSIX_GUARD(s2n_config_setup_tls13(config));
    } else if (s2n_is_in_fips_mode()) {
        POSIX_GUARD(s2n_config_setup_fips(config));
    }

    POSIX_GUARD_PTR(config->domain_name_to_cert_map = s2n_map_new_with_initial_capacity(1));
    POSIX_GUARD_RESULT(s2n_map_complete(config->domain_name_to_cert_map));

    s2n_x509_trust_store_init_empty(&config->trust_store);

    return 0;
}

static int s2n_config_cleanup(struct s2n_config *config)
{
    s2n_x509_trust_store_wipe(&config->trust_store);
    config->check_ocsp = 0;

    POSIX_GUARD(s2n_config_free_session_ticket_keys(config));
    POSIX_GUARD(s2n_config_free_cert_chain_and_key(config));
    POSIX_GUARD(s2n_config_free_dhparams(config));
    POSIX_GUARD(s2n_free(&config->application_protocols));
    POSIX_GUARD(s2n_free(&config->cert_authorities));
    POSIX_GUARD_RESULT(s2n_map_free(config->domain_name_to_cert_map));

    POSIX_CHECKED_MEMSET(config, 0, sizeof(struct s2n_config));

    return 0;
}

static int s2n_config_update_domain_name_to_cert_map(struct s2n_config *config,
        struct s2n_blob *name,
        struct s2n_cert_chain_and_key *cert_key_pair)
{
    POSIX_ENSURE_REF(config);
    POSIX_ENSURE_REF(name);

    struct s2n_map *domain_name_to_cert_map = config->domain_name_to_cert_map;
    /* s2n_map does not allow zero-size key */
    if (name->size == 0) {
        return 0;
    }
    s2n_pkey_type cert_type = s2n_cert_chain_and_key_get_pkey_type(cert_key_pair);
    struct s2n_blob s2n_map_value = { 0 };
    bool key_found = false;
    POSIX_GUARD_RESULT(s2n_map_lookup(domain_name_to_cert_map, name, &s2n_map_value, &key_found));
    if (!key_found) {
        struct certs_by_type value = { { 0 } };
        value.certs[cert_type] = cert_key_pair;
        s2n_map_value.data = (uint8_t *) &value;
        s2n_map_value.size = sizeof(struct certs_by_type);

        POSIX_GUARD_RESULT(s2n_map_unlock(domain_name_to_cert_map));
        POSIX_GUARD_RESULT(s2n_map_add(domain_name_to_cert_map, name, &s2n_map_value));
        POSIX_GUARD_RESULT(s2n_map_complete(domain_name_to_cert_map));
    } else {
        struct certs_by_type *value = (void *) s2n_map_value.data;
        if (value->certs[cert_type] == NULL) {
            value->certs[cert_type] = cert_key_pair;
        } else if (config->cert_tiebreak_cb) {
            /* There's an existing certificate for this (domain_name, auth_method).
             * Run the application's tiebreaking callback to decide which cert should be used.
             * An application may have some context specific logic to resolve ties that are based
             * on factors like trust, expiry, etc.
             */
            struct s2n_cert_chain_and_key *winner = config->cert_tiebreak_cb(
                    value->certs[cert_type],
                    cert_key_pair,
                    name->data,
                    name->size);
            if (winner) {
                value->certs[cert_type] = winner;
            }
        }
    }

    return 0;
}

int s2n_config_build_domain_name_to_cert_map(struct s2n_config *config, struct s2n_cert_chain_and_key *cert_key_pair)
{
    uint32_t cn_len = 0;
    POSIX_GUARD_RESULT(s2n_array_num_elements(cert_key_pair->cn_names, &cn_len));
    uint32_t san_len = 0;
    POSIX_GUARD_RESULT(s2n_array_num_elements(cert_key_pair->san_names, &san_len));

    if (san_len == 0) {
        for (uint32_t i = 0; i < cn_len; i++) {
            struct s2n_blob *cn_name = NULL;
            POSIX_GUARD_RESULT(s2n_array_get(cert_key_pair->cn_names, i, (void **) &cn_name));
            POSIX_GUARD(s2n_config_update_domain_name_to_cert_map(config, cn_name, cert_key_pair));
        }
    } else {
        for (uint32_t i = 0; i < san_len; i++) {
            struct s2n_blob *san_name = NULL;
            POSIX_GUARD_RESULT(s2n_array_get(cert_key_pair->san_names, i, (void **) &san_name));
            POSIX_GUARD(s2n_config_update_domain_name_to_cert_map(config, san_name, cert_key_pair));
        }
    }

    return 0;
}

struct s2n_config *s2n_fetch_default_config(void)
{
    if (s2n_use_default_tls13_config()) {
        return &s2n_default_tls13_config;
    }
    if (s2n_is_in_fips_mode()) {
        return &s2n_default_fips_config;
    }
    return &s2n_default_config;
}

int s2n_config_set_unsafe_for_testing(struct s2n_config *config)
{
    POSIX_ENSURE(s2n_in_test(), S2N_ERR_NOT_IN_TEST);
    config->check_ocsp = 0;
    config->disable_x509_validation = 1;

    return S2N_SUCCESS;
}

int s2n_config_defaults_init(void)
{
    /* Set up fips defaults */
    if (s2n_is_in_fips_mode()) {
        POSIX_GUARD(s2n_config_init(&s2n_default_fips_config));
        POSIX_GUARD(s2n_config_setup_fips(&s2n_default_fips_config));
        POSIX_GUARD(s2n_config_load_system_certs(&s2n_default_fips_config));
    } else {
        /* Set up default */
        POSIX_GUARD(s2n_config_init(&s2n_default_config));
        POSIX_GUARD(s2n_config_setup_default(&s2n_default_config));
        POSIX_GUARD(s2n_config_load_system_certs(&s2n_default_config));
    }

    /* TLS 1.3 default config is only used in tests so avoid initialization costs in applications */
    POSIX_GUARD(s2n_config_init(&s2n_default_tls13_config));
    POSIX_GUARD(s2n_config_setup_tls13(&s2n_default_tls13_config));

    return S2N_SUCCESS;
}

S2N_RESULT s2n_config_testing_defaults_init_tls13_certs(void)
{
    RESULT_GUARD_POSIX(s2n_config_load_system_certs(&s2n_default_tls13_config));
    return S2N_RESULT_OK;
}

void s2n_wipe_static_configs(void)
{
    s2n_config_cleanup(&s2n_default_fips_config);
    s2n_config_cleanup(&s2n_default_config);
    s2n_config_cleanup(&s2n_default_tls13_config);
}

int s2n_config_load_system_certs(struct s2n_config *config)
{
    POSIX_ENSURE_REF(config);

    struct s2n_x509_trust_store *store = &config->trust_store;
    POSIX_ENSURE(!store->loaded_system_certs, S2N_ERR_X509_TRUST_STORE);

    if (!store->trust_store) {
        store->trust_store = X509_STORE_new();
        POSIX_ENSURE_REF(store->trust_store);
    }

    int err_code = X509_STORE_set_default_paths(store->trust_store);
    if (!err_code) {
        s2n_x509_trust_store_wipe(store);
        POSIX_BAIL(S2N_ERR_X509_TRUST_STORE);
    }
    store->loaded_system_certs = true;

    return S2N_SUCCESS;
}

struct s2n_config *s2n_config_new_minimal(void)
{
    struct s2n_blob allocator = { 0 };
    struct s2n_config *new_config = NULL;

    PTR_GUARD_POSIX(s2n_alloc(&allocator, sizeof(struct s2n_config)));
    PTR_GUARD_POSIX(s2n_blob_zero(&allocator));

    new_config = (struct s2n_config *) (void *) allocator.data;
    if (s2n_config_init(new_config) != S2N_SUCCESS) {
        s2n_free(&allocator);
        return NULL;
    }

    return new_config;
}

struct s2n_config *s2n_config_new(void)
{
    struct s2n_config *new_config = s2n_config_new_minimal();
    PTR_ENSURE_REF(new_config);

    /* For backwards compatibility, s2n_config_new loads system certs by default. */
    PTR_GUARD_POSIX(s2n_config_load_system_certs(new_config));

    return new_config;
}

static int s2n_config_store_ticket_key_comparator(const void *a, const void *b)
{
    if (((const struct s2n_ticket_key *) a)->intro_timestamp >= ((const struct s2n_ticket_key *) b)->intro_timestamp) {
        return S2N_GREATER_OR_EQUAL;
    } else {
        return S2N_LESS_THAN;
    }
}

static int s2n_verify_unique_ticket_key_comparator(const void *a, const void *b)
{
    return memcmp(a, b, SHA_DIGEST_LENGTH);
}

int s2n_config_init_session_ticket_keys(struct s2n_config *config)
{
    if (config->ticket_keys == NULL) {
        POSIX_ENSURE_REF(config->ticket_keys = s2n_set_new(sizeof(struct s2n_ticket_key), s2n_config_store_ticket_key_comparator));
    }

    if (config->ticket_key_hashes == NULL) {
        POSIX_ENSURE_REF(config->ticket_key_hashes = s2n_set_new(SHA_DIGEST_LENGTH, s2n_verify_unique_ticket_key_comparator));
    }

    return 0;
}

int s2n_config_free_session_ticket_keys(struct s2n_config *config)
{
    if (config->ticket_keys != NULL) {
        POSIX_GUARD_RESULT(s2n_set_free_p(&config->ticket_keys));
    }

    if (config->ticket_key_hashes != NULL) {
        POSIX_GUARD_RESULT(s2n_set_free_p(&config->ticket_key_hashes));
    }

    return 0;
}

int s2n_config_free_cert_chain_and_key(struct s2n_config *config)
{
    /* We track certificate ownership on the config itself because a config
     * CANNOT use a combination of owned and unowned chains.
     * If it does, and the unowned chains are freed before the config is,
     * then iterating over the chains to determine which are owned and need to be freed
     * will mean also reading the invalid, freed memory of any unowned certificates.
     * As of now, some tests free chains before the config, so that pattern may also
     * appear in application code.
     */
    if (config->cert_ownership != S2N_LIB_OWNED) {
        return S2N_SUCCESS;
    }

    /* Free the cert_chain_and_key since the application has no reference
     * to it. This is necessary until s2n_config_add_cert_chain_and_key is deprecated. */
    for (int i = 0; i < S2N_CERT_TYPE_COUNT; i++) {
        s2n_cert_chain_and_key_free(config->default_certs_by_type.certs[i]);
        config->default_certs_by_type.certs[i] = NULL;
    }

    config->cert_ownership = S2N_NOT_OWNED;
    return S2N_SUCCESS;
}

int s2n_config_free_dhparams(struct s2n_config *config)
{
    if (config->dhparams) {
        POSIX_GUARD(s2n_dh_params_free(config->dhparams));
    }

    POSIX_GUARD(s2n_free_object((uint8_t **) &config->dhparams, sizeof(struct s2n_dh_params)));
    return 0;
}

S2N_CLEANUP_RESULT s2n_config_ptr_free(struct s2n_config **config)
{
    RESULT_ENSURE_REF(config);
    RESULT_GUARD_POSIX(s2n_config_free(*config));
    *config = NULL;
    return S2N_RESULT_OK;
}

int s2n_config_free(struct s2n_config *config)
{
    s2n_config_cleanup(config);

    POSIX_GUARD(s2n_free_object((uint8_t **) &config, sizeof(struct s2n_config)));
    return 0;
}

int s2n_config_get_client_auth_type(struct s2n_config *config, s2n_cert_auth_type *client_auth_type)
{
    POSIX_ENSURE_REF(config);
    POSIX_ENSURE_REF(client_auth_type);
    *client_auth_type = config->client_cert_auth_type;
    return 0;
}

int s2n_config_set_client_auth_type(struct s2n_config *config, s2n_cert_auth_type client_auth_type)
{
    POSIX_ENSURE_REF(config);
    config->client_cert_auth_type_overridden = 1;
    config->client_cert_auth_type = client_auth_type;
    return 0;
}

int s2n_config_set_ct_support_level(struct s2n_config *config, s2n_ct_support_level type)
{
    POSIX_ENSURE_REF(config);
    config->ct_type = type;

    return 0;
}

int s2n_config_set_alert_behavior(struct s2n_config *config, s2n_alert_behavior alert_behavior)
{
    POSIX_ENSURE_REF(config);

    switch (alert_behavior) {
        case S2N_ALERT_FAIL_ON_WARNINGS:
        case S2N_ALERT_IGNORE_WARNINGS:
            config->alert_behavior = alert_behavior;
            break;
        default:
            POSIX_BAIL(S2N_ERR_INVALID_ARGUMENT);
    }

    return 0;
}

int s2n_config_set_verify_host_callback(struct s2n_config *config, s2n_verify_host_fn verify_host_fn, void *data)
{
    POSIX_ENSURE_REF(config);
    config->verify_host_fn = verify_host_fn;
    config->data_for_verify_host = data;
    return 0;
}

int s2n_config_set_check_stapled_ocsp_response(struct s2n_config *config, uint8_t check_ocsp)
{
    POSIX_ENSURE_REF(config);
    S2N_ERROR_IF(check_ocsp && !s2n_x509_ocsp_stapling_supported(), S2N_ERR_OCSP_NOT_SUPPORTED);
    config->check_ocsp = check_ocsp;
    return 0;
}

int s2n_config_disable_x509_time_verification(struct s2n_config *config)
{
    POSIX_ENSURE_REF(config);
    config->disable_x509_time_validation = true;
    return S2N_SUCCESS;
}

int s2n_config_disable_x509_verification(struct s2n_config *config)
{
    POSIX_ENSURE_REF(config);
    s2n_x509_trust_store_wipe(&config->trust_store);
    config->disable_x509_validation = 1;
    return 0;
}

int s2n_config_set_max_cert_chain_depth(struct s2n_config *config, uint16_t max_depth)
{
    POSIX_ENSURE_REF(config);
    S2N_ERROR_IF(max_depth == 0, S2N_ERR_INVALID_ARGUMENT);

    config->max_verify_cert_chain_depth = max_depth;
    config->max_verify_cert_chain_depth_set = 1;
    return 0;
}

int s2n_config_set_status_request_type(struct s2n_config *config, s2n_status_request_type type)
{
    S2N_ERROR_IF(type == S2N_STATUS_REQUEST_OCSP && !s2n_x509_ocsp_stapling_supported(), S2N_ERR_OCSP_NOT_SUPPORTED);

    POSIX_ENSURE_REF(config);
    config->ocsp_status_requested_by_user = (type == S2N_STATUS_REQUEST_OCSP);

    /* Reset the ocsp_status_requested_by_s2n flag if OCSP status requests were disabled. */
    if (type == S2N_STATUS_REQUEST_NONE) {
        config->ocsp_status_requested_by_s2n = false;
    }

    return 0;
}

int s2n_config_wipe_trust_store(struct s2n_config *config)
{
    POSIX_ENSURE_REF(config);

    s2n_x509_trust_store_wipe(&config->trust_store);

    return S2N_SUCCESS;
}

int s2n_config_add_pem_to_trust_store(struct s2n_config *config, const char *pem)
{
    POSIX_ENSURE_REF(config);
    POSIX_ENSURE_REF(pem);

    POSIX_GUARD(s2n_x509_trust_store_add_pem(&config->trust_store, pem));

    return 0;
}

int s2n_config_set_verification_ca_location(struct s2n_config *config, const char *ca_pem_filename, const char *ca_dir)
{
    POSIX_ENSURE_REF(config);
    int err_code = s2n_x509_trust_store_from_ca_file(&config->trust_store, ca_pem_filename, ca_dir);

    if (!err_code) {
        config->ocsp_status_requested_by_s2n = s2n_x509_ocsp_stapling_supported() ? S2N_STATUS_REQUEST_OCSP : S2N_STATUS_REQUEST_NONE;
    }

    return err_code;
}

static int s2n_config_add_cert_chain_and_key_impl(struct s2n_config *config, struct s2n_cert_chain_and_key *cert_key_pair)
{
    POSIX_ENSURE_REF(config->domain_name_to_cert_map);
    POSIX_ENSURE_REF(cert_key_pair);

    POSIX_GUARD_RESULT(s2n_security_policy_validate_certificate_chain(config->security_policy, cert_key_pair));

    s2n_pkey_type cert_type = s2n_cert_chain_and_key_get_pkey_type(cert_key_pair);
    config->is_rsa_cert_configured |= (cert_type == S2N_PKEY_TYPE_RSA);

    POSIX_GUARD(s2n_config_build_domain_name_to_cert_map(config, cert_key_pair));

    if (!config->default_certs_are_explicit) {
        POSIX_ENSURE(cert_type >= 0, S2N_ERR_CERT_TYPE_UNSUPPORTED);
        POSIX_ENSURE(cert_type < S2N_CERT_TYPE_COUNT, S2N_ERR_CERT_TYPE_UNSUPPORTED);
        /* Attempt to auto set default based on ordering. ie: first RSA cert is the default, first ECDSA cert is the
         * default, etc. */
        if (config->default_certs_by_type.certs[cert_type] == NULL) {
            config->default_certs_by_type.certs[cert_type] = cert_key_pair;
        } else {
            /* Because library-owned certificates are tracked and cleaned up via the
             * default_certs_by_type mapping, library-owned chains MUST be set as the default
             * to avoid a memory leak. If they're not the default, they're not freed.
             */
            POSIX_ENSURE(config->cert_ownership != S2N_LIB_OWNED,
                    S2N_ERR_MULTIPLE_DEFAULT_CERTIFICATES_PER_AUTH_TYPE);
        }
    }

    if (s2n_pkey_check_key_exists(cert_key_pair->private_key) != S2N_SUCCESS) {
        config->no_signing_key = true;
    }

    return S2N_SUCCESS;
}

S2N_RESULT s2n_config_validate_loaded_certificates(const struct s2n_config *config,
        const struct s2n_security_policy *security_policy)
{
    RESULT_ENSURE_REF(config);
    RESULT_ENSURE_REF(security_policy);

    if (security_policy->certificate_key_preferences == NULL
            && security_policy->certificate_signature_preferences == NULL) {
        return S2N_RESULT_OK;
    }

    /* Duplicates a check in s2n_security_policy_validate_certificate_chain.
     * If a large number of certificates are configured, even iterating
     * over the chains to call s2n_security_policy_validate_certificate_chain
     * could be prohibitively expensive.
     */
    if (!security_policy->certificate_preferences_apply_locally) {
        return S2N_RESULT_OK;
    }

    /* validate the default certs */
    for (int i = 0; i < S2N_CERT_TYPE_COUNT; i++) {
        struct s2n_cert_chain_and_key *cert = config->default_certs_by_type.certs[i];
        if (cert == NULL) {
            continue;
        }
        RESULT_GUARD(s2n_security_policy_validate_certificate_chain(security_policy, cert));
    }

    /* validate the certs in the domain map */
    if (config->domain_name_to_cert_map == NULL) {
        return S2N_RESULT_OK;
    }

    struct s2n_map_iterator iter = { 0 };
    RESULT_GUARD(s2n_map_iterator_init(&iter, config->domain_name_to_cert_map));

    while (s2n_map_iterator_has_next(&iter)) {
        struct s2n_blob value = { 0 };
        RESULT_GUARD(s2n_map_iterator_next(&iter, &value));

        struct certs_by_type *domain_certs = (void *) value.data;
        for (int i = 0; i < S2N_CERT_TYPE_COUNT; i++) {
            struct s2n_cert_chain_and_key *cert = domain_certs->certs[i];
            if (cert == NULL) {
                continue;
            }
            RESULT_GUARD(s2n_security_policy_validate_certificate_chain(security_policy, cert));
        }
    }
    return S2N_RESULT_OK;
}

/* Deprecated. Superseded by s2n_config_add_cert_chain_and_key_to_store */
int s2n_config_add_cert_chain_and_key(struct s2n_config *config, const char *cert_chain_pem, const char *private_key_pem)
{
    POSIX_ENSURE_REF(config);
    POSIX_ENSURE(config->cert_ownership != S2N_APP_OWNED, S2N_ERR_CERT_OWNERSHIP);

    DEFER_CLEANUP(struct s2n_cert_chain_and_key *chain_and_key = s2n_cert_chain_and_key_new(),
            s2n_cert_chain_and_key_ptr_free);
    POSIX_ENSURE_REF(chain_and_key);
    POSIX_GUARD(s2n_cert_chain_and_key_load_pem(chain_and_key, cert_chain_pem, private_key_pem));
    POSIX_GUARD(s2n_config_add_cert_chain_and_key_impl(config, chain_and_key));
    config->cert_ownership = S2N_LIB_OWNED;

    ZERO_TO_DISABLE_DEFER_CLEANUP(chain_and_key);
    return S2N_SUCCESS;
}

/* Only used in the Rust bindings. Superseded by s2n_config_add_cert_chain_and_key_to_store */
int s2n_config_add_cert_chain(struct s2n_config *config,
        uint8_t *cert_chain_pem, uint32_t cert_chain_pem_size)
{
    POSIX_ENSURE_REF(config);
    POSIX_ENSURE(config->cert_ownership != S2N_APP_OWNED, S2N_ERR_CERT_OWNERSHIP);

    DEFER_CLEANUP(struct s2n_cert_chain_and_key *chain_and_key = s2n_cert_chain_and_key_new(),
            s2n_cert_chain_and_key_ptr_free);
    POSIX_ENSURE_REF(chain_and_key);
    POSIX_GUARD(s2n_cert_chain_and_key_load_public_pem_bytes(chain_and_key,
            cert_chain_pem, cert_chain_pem_size));
    POSIX_GUARD(s2n_config_add_cert_chain_and_key_impl(config, chain_and_key));
    config->cert_ownership = S2N_LIB_OWNED;

    ZERO_TO_DISABLE_DEFER_CLEANUP(chain_and_key);
    return S2N_SUCCESS;
}

int s2n_config_add_cert_chain_and_key_to_store(struct s2n_config *config, struct s2n_cert_chain_and_key *cert_key_pair)
{
    POSIX_ENSURE_REF(config);
    POSIX_ENSURE(config->cert_ownership != S2N_LIB_OWNED, S2N_ERR_CERT_OWNERSHIP);

    POSIX_ENSURE_REF(cert_key_pair);
    POSIX_GUARD(s2n_config_add_cert_chain_and_key_impl(config, cert_key_pair));
    config->cert_ownership = S2N_APP_OWNED;

    return S2N_SUCCESS;
}

int s2n_config_set_async_pkey_callback(struct s2n_config *config, s2n_async_pkey_fn fn)
{
    POSIX_ENSURE_REF(config);

    config->async_pkey_cb = fn;

    return S2N_SUCCESS;
}

static int s2n_config_clear_default_certificates(struct s2n_config *config)
{
    POSIX_ENSURE_REF(config);

    /* Clearing library-owned chains would lead to a memory leak.
     * See s2n_config_free_cert_chain_and_key.
     */
    POSIX_ENSURE(config->cert_ownership != S2N_LIB_OWNED, S2N_ERR_CERT_OWNERSHIP);

    for (int i = 0; i < S2N_CERT_TYPE_COUNT; i++) {
        config->default_certs_by_type.certs[i] = NULL;
    }
    config->cert_ownership = S2N_NOT_OWNED;
    return 0;
}

int s2n_config_set_cert_chain_and_key_defaults(struct s2n_config *config,
        struct s2n_cert_chain_and_key **cert_key_pairs,
        uint32_t num_cert_key_pairs)
{
    POSIX_ENSURE_REF(config);
    POSIX_ENSURE_REF(cert_key_pairs);
    S2N_ERROR_IF(num_cert_key_pairs < 1 || num_cert_key_pairs > S2N_CERT_TYPE_COUNT,
            S2N_ERR_NUM_DEFAULT_CERTIFICATES);

    /* This method will set application-owned chains, so we must not already be using
     * any library owned chains. See s2n_config_free_cert_chain_and_key.
     */
    POSIX_ENSURE(config->cert_ownership != S2N_LIB_OWNED, S2N_ERR_CERT_OWNERSHIP);

    /* Validate certs being set before clearing auto-chosen defaults or previously set defaults */
    struct certs_by_type new_defaults = { { 0 } };
    for (size_t i = 0; i < num_cert_key_pairs; i++) {
        POSIX_ENSURE_REF(cert_key_pairs[i]);
        s2n_pkey_type cert_type = s2n_cert_chain_and_key_get_pkey_type(cert_key_pairs[i]);
        S2N_ERROR_IF(new_defaults.certs[cert_type] != NULL, S2N_ERR_MULTIPLE_DEFAULT_CERTIFICATES_PER_AUTH_TYPE);
        new_defaults.certs[cert_type] = cert_key_pairs[i];
    }

    POSIX_GUARD(s2n_config_clear_default_certificates(config));
    for (size_t i = 0; i < num_cert_key_pairs; i++) {
        s2n_pkey_type cert_type = s2n_cert_chain_and_key_get_pkey_type(cert_key_pairs[i]);
        config->is_rsa_cert_configured |= (cert_type == S2N_PKEY_TYPE_RSA);
        config->default_certs_by_type.certs[cert_type] = cert_key_pairs[i];
    }

    config->default_certs_are_explicit = 1;
    config->cert_ownership = S2N_APP_OWNED;
    return 0;
}

int s2n_config_add_dhparams(struct s2n_config *config, const char *dhparams_pem)
{
    DEFER_CLEANUP(struct s2n_stuffer dhparams_in_stuffer = { 0 }, s2n_stuffer_free);
    DEFER_CLEANUP(struct s2n_stuffer dhparams_out_stuffer = { 0 }, s2n_stuffer_free);
    struct s2n_blob dhparams_blob = { 0 };
    struct s2n_blob mem = { 0 };

    /* Allocate the memory for the chain and key struct */
    POSIX_GUARD(s2n_alloc(&mem, sizeof(struct s2n_dh_params)));
    config->dhparams = (struct s2n_dh_params *) (void *) mem.data;

    if (s2n_stuffer_alloc_ro_from_string(&dhparams_in_stuffer, dhparams_pem) != S2N_SUCCESS) {
        s2n_free(&mem);
        S2N_ERROR_PRESERVE_ERRNO();
    }
    if (s2n_stuffer_growable_alloc(&dhparams_out_stuffer, strlen(dhparams_pem)) != S2N_SUCCESS) {
        s2n_free(&mem);
        S2N_ERROR_PRESERVE_ERRNO();
    }

    /* Convert pem to asn1 and asn1 to the private key */
    POSIX_GUARD(s2n_stuffer_dhparams_from_pem(&dhparams_in_stuffer, &dhparams_out_stuffer));

    dhparams_blob.size = s2n_stuffer_data_available(&dhparams_out_stuffer);
    dhparams_blob.data = s2n_stuffer_raw_read(&dhparams_out_stuffer, dhparams_blob.size);
    POSIX_ENSURE_REF(dhparams_blob.data);

    POSIX_GUARD(s2n_pkcs3_to_dh_params(config->dhparams, &dhparams_blob));

    return 0;
}

int s2n_config_set_wall_clock(struct s2n_config *config, s2n_clock_time_nanoseconds clock_fn, void *ctx)
{
    POSIX_ENSURE_REF(clock_fn);

    config->wall_clock = clock_fn;
    config->sys_clock_ctx = ctx;

    return 0;
}

int s2n_config_set_monotonic_clock(struct s2n_config *config, s2n_clock_time_nanoseconds clock_fn, void *ctx)
{
    POSIX_ENSURE_REF(clock_fn);

    config->monotonic_clock = clock_fn;
    config->monotonic_clock_ctx = ctx;

    return 0;
}

int s2n_config_set_cache_store_callback(struct s2n_config *config, s2n_cache_store_callback cache_store_callback, void *data)
{
    POSIX_ENSURE_REF(cache_store_callback);

    config->cache_store = cache_store_callback;
    config->cache_store_data = data;

    return 0;
}

int s2n_config_set_cache_retrieve_callback(struct s2n_config *config, s2n_cache_retrieve_callback cache_retrieve_callback, void *data)
{
    POSIX_ENSURE_REF(cache_retrieve_callback);

    config->cache_retrieve = cache_retrieve_callback;
    config->cache_retrieve_data = data;

    return 0;
}

int s2n_config_set_cache_delete_callback(struct s2n_config *config, s2n_cache_delete_callback cache_delete_callback, void *data)
{
    POSIX_ENSURE_REF(cache_delete_callback);

    config->cache_delete = cache_delete_callback;
    config->cache_delete_data = data;

    return 0;
}

int s2n_config_set_extension_data(struct s2n_config *config, s2n_tls_extension_type type, const uint8_t *data, uint32_t length)
{
    POSIX_ENSURE_REF(config);

    if (s2n_config_get_num_default_certs(config) == 0) {
        POSIX_BAIL(S2N_ERR_UPDATING_EXTENSION);
    }
    struct s2n_cert_chain_and_key *config_chain_and_key = s2n_config_get_single_default_cert(config);
    POSIX_ENSURE_REF(config_chain_and_key);
    POSIX_ENSURE(config->cert_ownership == S2N_LIB_OWNED, S2N_ERR_CERT_OWNERSHIP);

    switch (type) {
        case S2N_EXTENSION_CERTIFICATE_TRANSPARENCY:
            POSIX_GUARD(s2n_cert_chain_and_key_set_sct_list(config_chain_and_key, data, length));
            break;
        case S2N_EXTENSION_OCSP_STAPLING:
            POSIX_GUARD(s2n_cert_chain_and_key_set_ocsp_data(config_chain_and_key, data, length));
            break;
        default:
            POSIX_BAIL(S2N_ERR_UNRECOGNIZED_EXTENSION);
    }

    return 0;
}

int s2n_config_set_client_hello_cb(struct s2n_config *config, s2n_client_hello_fn client_hello_cb, void *ctx)
{
    POSIX_ENSURE_REF(config);

    config->client_hello_cb = client_hello_cb;
    config->client_hello_cb_ctx = ctx;
    return 0;
}

int s2n_config_set_client_hello_cb_mode(struct s2n_config *config, s2n_client_hello_cb_mode cb_mode)
{
    POSIX_ENSURE_REF(config);
    POSIX_ENSURE(cb_mode == S2N_CLIENT_HELLO_CB_BLOCKING || cb_mode == S2N_CLIENT_HELLO_CB_NONBLOCKING, S2N_ERR_INVALID_STATE);

    config->client_hello_cb_mode = cb_mode;
    return S2N_SUCCESS;
}

int s2n_config_send_max_fragment_length(struct s2n_config *config, s2n_max_frag_len mfl_code)
{
    POSIX_ENSURE_REF(config);

    S2N_ERROR_IF(mfl_code > S2N_TLS_MAX_FRAG_LEN_4096, S2N_ERR_INVALID_MAX_FRAG_LEN);

    config->mfl_code = mfl_code;

    return 0;
}

int s2n_config_accept_max_fragment_length(struct s2n_config *config)
{
    POSIX_ENSURE_REF(config);

    config->accept_mfl = 1;

    return 0;
}

int s2n_config_set_session_state_lifetime(struct s2n_config *config,
        uint64_t lifetime_in_secs)
{
    POSIX_ENSURE_REF(config);

    config->session_state_lifetime_in_nanos = (lifetime_in_secs * ONE_SEC_IN_NANOS);
    return 0;
}

int s2n_config_set_session_tickets_onoff(struct s2n_config *config, uint8_t enabled)
{
    POSIX_ENSURE_REF(config);

    if (config->use_tickets == enabled) {
        return 0;
    }

    config->use_tickets = enabled;

    if (config->initial_tickets_to_send == 0) {
        /* Normally initial_tickets_to_send is set via s2n_config_set_initial_ticket_count.
         * However, s2n_config_set_initial_ticket_count calls this method.
         * So we set initial_tickets_to_send directly to avoid infinite recursion. */
        config->initial_tickets_to_send = 1;
    }

    /* session ticket || session id is enabled */
    if (enabled) {
        POSIX_GUARD(s2n_config_init_session_ticket_keys(config));
    } else if (!config->use_session_cache) {
        POSIX_GUARD(s2n_config_free_session_ticket_keys(config));
    }

    return 0;
}

int s2n_config_set_session_cache_onoff(struct s2n_config *config, uint8_t enabled)
{
    POSIX_ENSURE_REF(config);
    if (enabled && config->cache_store && config->cache_retrieve && config->cache_delete) {
        POSIX_GUARD(s2n_config_init_session_ticket_keys(config));
        config->use_session_cache = 1;
    } else {
        if (!config->use_tickets) {
            POSIX_GUARD(s2n_config_free_session_ticket_keys(config));
        }
        config->use_session_cache = 0;
    }
    return 0;
}

int s2n_config_set_ticket_encrypt_decrypt_key_lifetime(struct s2n_config *config,
        uint64_t lifetime_in_secs)
{
    POSIX_ENSURE_REF(config);

    config->encrypt_decrypt_key_lifetime_in_nanos = (lifetime_in_secs * ONE_SEC_IN_NANOS);
    return 0;
}

int s2n_config_set_ticket_decrypt_key_lifetime(struct s2n_config *config,
        uint64_t lifetime_in_secs)
{
    POSIX_ENSURE_REF(config);

    config->decrypt_key_lifetime_in_nanos = (lifetime_in_secs * ONE_SEC_IN_NANOS);
    return 0;
}

int s2n_config_add_ticket_crypto_key(struct s2n_config *config,
        const uint8_t *name, uint32_t name_len,
        uint8_t *key, uint32_t key_len,
        uint64_t intro_time_in_seconds_from_epoch)
{
    POSIX_ENSURE_REF(config);
    POSIX_ENSURE_REF(name);
    POSIX_ENSURE_REF(key);

    /* both session ticket and session cache encryption/decryption can use the same key mechanism */
    if (!config->use_tickets && !config->use_session_cache) {
        return 0;
    }

    POSIX_GUARD(s2n_config_wipe_expired_ticket_crypto_keys(config, -1));

    POSIX_ENSURE(key_len != 0, S2N_ERR_INVALID_TICKET_KEY_LENGTH);

    uint32_t ticket_keys_len = 0;
    POSIX_GUARD_RESULT(s2n_set_len(config->ticket_keys, &ticket_keys_len));
    POSIX_ENSURE(ticket_keys_len < S2N_MAX_TICKET_KEYS, S2N_ERR_TICKET_KEY_LIMIT);

    POSIX_ENSURE(name_len != 0, S2N_ERR_INVALID_TICKET_KEY_NAME_OR_NAME_LENGTH);
    POSIX_ENSURE(name_len <= S2N_TICKET_KEY_NAME_LEN, S2N_ERR_INVALID_TICKET_KEY_NAME_OR_NAME_LENGTH);

    /* Copy the name into a zero-padded array. */
    /* This ensures that all ticket names are equal in length, as the serialized name is fixed length */
    uint8_t name_data[S2N_TICKET_KEY_NAME_LEN] = { 0 };
    POSIX_CHECKED_MEMCPY(name_data, name, name_len);

    /* ensure the ticket name is not already present */
    POSIX_ENSURE(s2n_find_ticket_key(config, name_data) == NULL, S2N_ERR_INVALID_TICKET_KEY_NAME_OR_NAME_LENGTH);

    uint8_t output_pad[S2N_AES256_KEY_LEN + S2N_TICKET_AAD_IMPLICIT_LEN] = { 0 };
    struct s2n_blob out_key = { 0 };
    POSIX_GUARD(s2n_blob_init(&out_key, output_pad, s2n_array_len(output_pad)));
    struct s2n_blob in_key = { 0 };
    POSIX_GUARD(s2n_blob_init(&in_key, key, key_len));
    struct s2n_blob salt = { 0 };
    POSIX_GUARD(s2n_blob_init(&salt, NULL, 0));
    struct s2n_blob info = { 0 };
    POSIX_GUARD(s2n_blob_init(&info, NULL, 0));

    struct s2n_ticket_key *session_ticket_key = { 0 };
    DEFER_CLEANUP(struct s2n_blob allocator = { 0 }, s2n_free);
    POSIX_GUARD(s2n_alloc(&allocator, sizeof(struct s2n_ticket_key)));
    session_ticket_key = (struct s2n_ticket_key *) (void *) allocator.data;

    DEFER_CLEANUP(struct s2n_hmac_state hmac = { 0 }, s2n_hmac_free);

    POSIX_GUARD(s2n_hmac_new(&hmac));
    POSIX_GUARD(s2n_hkdf(&hmac, S2N_HMAC_SHA256, &salt, &in_key, &info, &out_key));

    DEFER_CLEANUP(struct s2n_hash_state hash = { 0 }, s2n_hash_free);
    uint8_t hash_output[SHA_DIGEST_LENGTH] = { 0 };

    POSIX_GUARD(s2n_hash_new(&hash));
    POSIX_GUARD(s2n_hash_init(&hash, S2N_HASH_SHA1));
    POSIX_GUARD(s2n_hash_update(&hash, out_key.data, out_key.size));
    POSIX_GUARD(s2n_hash_digest(&hash, hash_output, SHA_DIGEST_LENGTH));

    POSIX_GUARD_RESULT(s2n_set_len(config->ticket_keys, &ticket_keys_len));
    if (ticket_keys_len >= S2N_MAX_TICKET_KEY_HASHES) {
        POSIX_GUARD_RESULT(s2n_set_free_p(&config->ticket_key_hashes));
        POSIX_ENSURE_REF(config->ticket_key_hashes = s2n_set_new(SHA_DIGEST_LENGTH, s2n_verify_unique_ticket_key_comparator));
    }

    /* Insert hash key into a sorted array at known index */
    POSIX_GUARD_RESULT(s2n_set_add(config->ticket_key_hashes, hash_output));

    POSIX_CHECKED_MEMCPY(session_ticket_key->key_name, name_data, s2n_array_len(name_data));
    POSIX_CHECKED_MEMCPY(session_ticket_key->aes_key, out_key.data, S2N_AES256_KEY_LEN);
    out_key.data = output_pad + S2N_AES256_KEY_LEN;
    POSIX_CHECKED_MEMCPY(session_ticket_key->implicit_aad, out_key.data, S2N_TICKET_AAD_IMPLICIT_LEN);

    if (intro_time_in_seconds_from_epoch == 0) {
        uint64_t now = 0;
        POSIX_GUARD_RESULT(s2n_config_wall_clock(config, &now));
        session_ticket_key->intro_timestamp = now;
    } else {
        session_ticket_key->intro_timestamp = (intro_time_in_seconds_from_epoch * ONE_SEC_IN_NANOS);
    }

    POSIX_GUARD(s2n_config_store_ticket_key(config, session_ticket_key));

    return 0;
}

int s2n_config_require_ticket_forward_secrecy(struct s2n_config *config, bool enabled)
{
    POSIX_ENSURE_REF(config);

    config->ticket_forward_secrecy = enabled;

    return S2N_SUCCESS;
}

int s2n_config_set_cert_tiebreak_callback(struct s2n_config *config, s2n_cert_tiebreak_callback cert_tiebreak_cb)
{
    config->cert_tiebreak_cb = cert_tiebreak_cb;
    return 0;
}

struct s2n_cert_chain_and_key *s2n_config_get_single_default_cert(struct s2n_config *config)
{
    PTR_ENSURE_REF(config);
    struct s2n_cert_chain_and_key *cert = NULL;

    for (int i = S2N_CERT_TYPE_COUNT - 1; i >= 0; i--) {
        if (config->default_certs_by_type.certs[i] != NULL) {
            cert = config->default_certs_by_type.certs[i];
        }
    }
    return cert;
}

int s2n_config_get_num_default_certs(const struct s2n_config *config)
{
    POSIX_ENSURE_REF(config);
    int num_certs = 0;
    for (int i = 0; i < S2N_CERT_TYPE_COUNT; i++) {
        if (config->default_certs_by_type.certs[i] != NULL) {
            num_certs++;
        }
    }

    return num_certs;
}

int s2n_config_enable_cert_req_dss_legacy_compat(struct s2n_config *config)
{
    POSIX_ENSURE_REF(config);
    config->cert_req_dss_legacy_compat_enabled = 1;
    return S2N_SUCCESS;
}

int s2n_config_set_psk_selection_callback(struct s2n_config *config, s2n_psk_selection_callback cb, void *context)
{
    POSIX_ENSURE_REF(config);
    config->psk_selection_cb = cb;
    config->psk_selection_ctx = context;
    return S2N_SUCCESS;
}

int s2n_config_set_key_log_cb(struct s2n_config *config, s2n_key_log_fn callback, void *ctx)
{
    POSIX_ENSURE_MUT(config);

    config->key_log_cb = callback;
    config->key_log_ctx = ctx;

    return S2N_SUCCESS;
}

int s2n_config_set_async_pkey_validation_mode(struct s2n_config *config, s2n_async_pkey_validation_mode mode)
{
    POSIX_ENSURE_REF(config);

    switch (mode) {
        case S2N_ASYNC_PKEY_VALIDATION_FAST:
        case S2N_ASYNC_PKEY_VALIDATION_STRICT:
            config->async_pkey_validation_mode = mode;
            return S2N_SUCCESS;
    }

    POSIX_BAIL(S2N_ERR_INVALID_ARGUMENT);
}

int s2n_config_set_ctx(struct s2n_config *config, void *ctx)
{
    POSIX_ENSURE_REF(config);

    config->context = ctx;

    return S2N_SUCCESS;
}

int s2n_config_get_ctx(struct s2n_config *config, void **ctx)
{
    POSIX_ENSURE_REF(config);
    POSIX_ENSURE_REF(ctx);

    *ctx = config->context;

    return S2N_SUCCESS;
}

int s2n_config_set_send_buffer_size(struct s2n_config *config, uint32_t size)
{
    POSIX_ENSURE_REF(config);
    POSIX_ENSURE(size >= S2N_MIN_SEND_BUFFER_SIZE, S2N_ERR_INVALID_ARGUMENT);
    config->send_buffer_size_override = size;
    return S2N_SUCCESS;
}

int s2n_config_set_verify_after_sign(struct s2n_config *config, s2n_verify_after_sign mode)
{
    POSIX_ENSURE_REF(config);
    switch (mode) {
        case S2N_VERIFY_AFTER_SIGN_DISABLED:
            config->verify_after_sign = false;
            break;
        case S2N_VERIFY_AFTER_SIGN_ENABLED:
            config->verify_after_sign = true;
            break;
        default:
            POSIX_BAIL(S2N_ERR_INVALID_ARGUMENT);
    }
    return S2N_SUCCESS;
}

/*
 *= https://www.rfc-editor.org/rfc/rfc5746#5
 *# TLS implementations SHOULD provide a mechanism to disable and enable
 *# renegotiation.
 */
int s2n_config_set_renegotiate_request_cb(struct s2n_config *config, s2n_renegotiate_request_cb cb, void *ctx)
{
    POSIX_ENSURE_REF(config);

    /* This feature cannot be used with serialization currently */
    POSIX_ENSURE(config->serialized_connection_version == S2N_SERIALIZED_CONN_NONE, S2N_ERR_INVALID_STATE);

    config->renegotiate_request_cb = cb;
    config->renegotiate_request_ctx = ctx;
    return S2N_SUCCESS;
}

int s2n_config_set_npn(struct s2n_config *config, bool enable)
{
    POSIX_ENSURE_REF(config);

    config->npn_supported = enable;

    return S2N_SUCCESS;
}

/*
 * Wrapper for wall_clock callback. This wrapper will ensure right return of s2n_errno everytime wall_clock
 * callback is called.
 */
S2N_RESULT s2n_config_wall_clock(struct s2n_config *config, uint64_t *output)
{
    RESULT_ENSURE(config->wall_clock(config->sys_clock_ctx, output) >= S2N_SUCCESS, S2N_ERR_CANCELLED);
    return S2N_RESULT_OK;
}

int s2n_config_set_crl_lookup_cb(struct s2n_config *config, s2n_crl_lookup_callback cb, void *ctx)
{
    POSIX_ENSURE_REF(config);
    config->crl_lookup_cb = cb;
    config->crl_lookup_ctx = ctx;
    return S2N_SUCCESS;
}

int s2n_config_set_recv_multi_record(struct s2n_config *config, bool enabled)
{
    POSIX_ENSURE_REF(config);

    config->recv_multi_record = enabled;

    return S2N_SUCCESS;
}

int s2n_config_set_cert_validation_cb(struct s2n_config *config, s2n_cert_validation_callback cb, void *ctx)
{
    POSIX_ENSURE_REF(config);

    config->cert_validation_cb = cb;
    config->cert_validation_ctx = ctx;

    return S2N_SUCCESS;
}

int s2n_config_get_supported_groups(struct s2n_config *config, uint16_t *groups, uint16_t groups_count_max,
        uint16_t *groups_count_out)
{
    POSIX_ENSURE_REF(groups_count_out);
    *groups_count_out = 0;
    POSIX_ENSURE_REF(config);
    POSIX_ENSURE_REF(groups);

    const struct s2n_security_policy *security_policy = config->security_policy;
    POSIX_ENSURE_REF(security_policy);
    const struct s2n_kem_preferences *kem_preferences = security_policy->kem_preferences;
    POSIX_ENSURE_REF(kem_preferences);
    const struct s2n_ecc_preferences *ecc_preferences = security_policy->ecc_preferences;
    POSIX_ENSURE_REF(ecc_preferences);

    uint16_t groups_count = 0;
    for (uint8_t i = 0; i < kem_preferences->tls13_kem_group_count; i++) {
        const struct s2n_kem_group *kem_group = kem_preferences->tls13_kem_groups[i];
        POSIX_ENSURE_REF(kem_group);
        if (!s2n_kem_group_is_available(kem_group)) {
            continue;
        }

        POSIX_ENSURE(groups_count < groups_count_max, S2N_ERR_INSUFFICIENT_MEM_SIZE);
        groups[groups_count] = kem_group->iana_id;
        groups_count += 1;
    }

    for (uint8_t i = 0; i < ecc_preferences->count; i++) {
        const struct s2n_ecc_named_curve *ecc_curve = ecc_preferences->ecc_curves[i];
        POSIX_ENSURE_REF(ecc_curve);

        POSIX_ENSURE(groups_count < groups_count_max, S2N_ERR_INSUFFICIENT_MEM_SIZE);
        groups[groups_count] = ecc_curve->iana_id;
        groups_count += 1;
    }

    *groups_count_out = groups_count;

    return S2N_SUCCESS;
}

int s2n_config_set_serialization_version(struct s2n_config *config, s2n_serialization_version version)
{
    POSIX_ENSURE_REF(config);

    /* This feature cannot be used with renegotiation currently */
    POSIX_ENSURE(config->renegotiate_request_cb == NULL, S2N_ERR_INVALID_STATE);

    /* Currently there is only one format version supported */
    POSIX_ENSURE_EQ(version, S2N_SERIALIZED_CONN_V1);
    config->serialized_connection_version = version;

    return S2N_SUCCESS;
}

int s2n_config_set_max_blinding_delay(struct s2n_config *config, uint32_t seconds)
{
    POSIX_ENSURE_REF(config);

    config->custom_blinding_set = 1;
    config->max_blinding = seconds;

    return S2N_SUCCESS;
}

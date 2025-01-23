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

#include "s2n_crl.h"

#include "tls/s2n_connection.h"

struct s2n_crl *s2n_crl_new(void)
{
    DEFER_CLEANUP(struct s2n_blob mem = { 0 }, s2n_free);
    PTR_GUARD_POSIX(s2n_alloc(&mem, sizeof(struct s2n_crl)));
    PTR_GUARD_POSIX(s2n_blob_zero(&mem));

    struct s2n_crl *crl = (struct s2n_crl *) (void *) mem.data;

    ZERO_TO_DISABLE_DEFER_CLEANUP(mem);
    return crl;
}

int s2n_crl_load_pem(struct s2n_crl *crl, uint8_t *pem, size_t len)
{
    POSIX_ENSURE_REF(crl);
    POSIX_ENSURE(crl->crl == NULL, S2N_ERR_INVALID_ARGUMENT);

    struct s2n_blob pem_blob = { 0 };
    POSIX_GUARD(s2n_blob_init(&pem_blob, pem, len));

    struct s2n_stuffer pem_stuffer = { 0 };
    POSIX_GUARD(s2n_stuffer_init(&pem_stuffer, &pem_blob));
    POSIX_GUARD(s2n_stuffer_skip_write(&pem_stuffer, pem_blob.size));

    DEFER_CLEANUP(struct s2n_stuffer der_out_stuffer = { 0 }, s2n_stuffer_free);
    POSIX_GUARD(s2n_stuffer_growable_alloc(&der_out_stuffer, len));
    POSIX_GUARD(s2n_stuffer_crl_from_pem(&pem_stuffer, &der_out_stuffer));

    uint32_t data_size = s2n_stuffer_data_available(&der_out_stuffer);
    const uint8_t *data = s2n_stuffer_raw_read(&der_out_stuffer, data_size);
    POSIX_ENSURE_REF(data);
    crl->crl = d2i_X509_CRL(NULL, &data, data_size);
    POSIX_ENSURE(crl->crl != NULL, S2N_ERR_INVALID_PEM);

    return S2N_SUCCESS;
}

int s2n_crl_free(struct s2n_crl **crl)
{
    if (crl == NULL) {
        return S2N_SUCCESS;
    }
    if (*crl == NULL) {
        return S2N_SUCCESS;
    }

    if ((*crl)->crl != NULL) {
        X509_CRL_free((*crl)->crl);
        (*crl)->crl = NULL;
    }

    POSIX_GUARD(s2n_free_object((uint8_t **) crl, sizeof(struct s2n_crl)));

    *crl = NULL;

    return S2N_SUCCESS;
}

int s2n_crl_get_issuer_hash(struct s2n_crl *crl, uint64_t *hash)
{
    POSIX_ENSURE_REF(crl);
    POSIX_ENSURE_REF(crl->crl);
    POSIX_ENSURE_REF(hash);

    X509_NAME *crl_name = X509_CRL_get_issuer(crl->crl);
    POSIX_ENSURE_REF(crl_name);

    unsigned long temp_hash = X509_NAME_hash(crl_name);
    POSIX_ENSURE(temp_hash != 0, S2N_ERR_INTERNAL_LIBCRYPTO_ERROR);

    *hash = temp_hash;

    return S2N_SUCCESS;
}

int s2n_crl_validate_active(struct s2n_crl *crl)
{
    POSIX_ENSURE_REF(crl);
    POSIX_ENSURE_REF(crl->crl);

    ASN1_TIME *this_update = X509_CRL_get_lastUpdate(crl->crl);
    POSIX_ENSURE_REF(this_update);

    int ret = X509_cmp_time(this_update, NULL);
    POSIX_ENSURE(ret != 0, S2N_ERR_CRL_INVALID_THIS_UPDATE);
    POSIX_ENSURE(ret < 0, S2N_ERR_CRL_NOT_YET_VALID);

    return S2N_SUCCESS;
}

int s2n_crl_validate_not_expired(struct s2n_crl *crl)
{
    POSIX_ENSURE_REF(crl);
    POSIX_ENSURE_REF(crl->crl);

    ASN1_TIME *next_update = X509_CRL_get_nextUpdate(crl->crl);
    if (next_update == NULL) {
        /* If the CRL has no nextUpdate field, assume it will never expire */
        return S2N_SUCCESS;
    }

    int ret = X509_cmp_time(next_update, NULL);
    POSIX_ENSURE(ret != 0, S2N_ERR_CRL_INVALID_NEXT_UPDATE);
    POSIX_ENSURE(ret > 0, S2N_ERR_CRL_EXPIRED);

    return S2N_SUCCESS;
}

S2N_RESULT s2n_crl_get_crls_from_lookup_list(struct s2n_x509_validator *validator, STACK_OF(X509_CRL) *crl_stack)
{
    RESULT_ENSURE_REF(validator);
    RESULT_ENSURE_REF(validator->crl_lookup_list);
    RESULT_ENSURE_REF(crl_stack);

    uint32_t num_lookups = 0;
    RESULT_GUARD(s2n_array_num_elements(validator->crl_lookup_list, &num_lookups));
    for (uint32_t i = 0; i < num_lookups; i++) {
        struct s2n_crl_lookup *lookup = NULL;
        RESULT_GUARD(s2n_array_get(validator->crl_lookup_list, i, (void **) &lookup));
        RESULT_ENSURE_REF(lookup);

        if (lookup->crl == NULL) {
            /* A CRL was intentionally not returned from the callback. Don't add anything to the stack*/
            continue;
        }

        RESULT_ENSURE_REF(lookup->crl->crl);
        if (!sk_X509_CRL_push(crl_stack, lookup->crl->crl)) {
            RESULT_BAIL(S2N_ERR_INTERNAL_LIBCRYPTO_ERROR);
        }
    }

    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_crl_get_lookup_callback_status(struct s2n_x509_validator *validator, crl_lookup_callback_status *status)
{
    RESULT_ENSURE_REF(validator);
    RESULT_ENSURE_REF(validator->crl_lookup_list);

    uint32_t num_lookups = 0;
    RESULT_GUARD(s2n_array_num_elements(validator->crl_lookup_list, &num_lookups));
    for (uint32_t i = 0; i < num_lookups; i++) {
        struct s2n_crl_lookup *lookup = NULL;
        RESULT_GUARD(s2n_array_get(validator->crl_lookup_list, i, (void **) &lookup));
        RESULT_ENSURE_REF(lookup);

        if (lookup->status == AWAITING_RESPONSE) {
            *status = AWAITING_RESPONSE;
            return S2N_RESULT_OK;
        }
    }

    *status = FINISHED;
    return S2N_RESULT_OK;
}

S2N_RESULT s2n_crl_handle_lookup_callback_result(struct s2n_x509_validator *validator)
{
    RESULT_ENSURE_REF(validator);

    crl_lookup_callback_status status = 0;
    RESULT_GUARD(s2n_crl_get_lookup_callback_status(validator, &status));

    switch (status) {
        case FINISHED:
            validator->state = READY_TO_VERIFY;
            return S2N_RESULT_OK;
        case AWAITING_RESPONSE:
            validator->state = AWAITING_CRL_CALLBACK;
            RESULT_BAIL(S2N_ERR_ASYNC_BLOCKED);
        default:
            RESULT_BAIL(S2N_ERR_INVALID_CERT_STATE);
    }
}

S2N_RESULT s2n_crl_invoke_lookup_callbacks(struct s2n_connection *conn, struct s2n_x509_validator *validator)
{
    RESULT_ENSURE_REF(validator);
    RESULT_ENSURE_REF(validator->cert_chain_from_wire);

    int cert_count = sk_X509_num(validator->cert_chain_from_wire);
    DEFER_CLEANUP(struct s2n_array *crl_lookup_list = s2n_array_new_with_capacity(sizeof(struct s2n_crl_lookup), cert_count),
            s2n_array_free_p);
    RESULT_ENSURE_REF(crl_lookup_list);

    for (int i = 0; i < cert_count; ++i) {
        struct s2n_crl_lookup *lookup = NULL;
        RESULT_GUARD(s2n_array_pushback(crl_lookup_list, (void **) &lookup));

        X509 *cert = sk_X509_value(validator->cert_chain_from_wire, i);
        RESULT_ENSURE_REF(cert);
        lookup->cert = cert;
        lookup->cert_idx = i;
    }

    validator->crl_lookup_list = crl_lookup_list;
    ZERO_TO_DISABLE_DEFER_CLEANUP(crl_lookup_list);

    /* Invoke the crl lookup callbacks after the crl_lookup_list is stored on the validator. This ensures that if a
     * callback fails, the memory for all other callbacks that may still be running remains allocated */
    uint32_t num_lookups = 0;
    RESULT_GUARD(s2n_array_num_elements(validator->crl_lookup_list, &num_lookups));
    for (uint32_t i = 0; i < num_lookups; i++) {
        struct s2n_crl_lookup *lookup = NULL;
        RESULT_GUARD(s2n_array_get(validator->crl_lookup_list, i, (void **) &lookup));
        RESULT_ENSURE_REF(lookup);

        int result = conn->config->crl_lookup_cb(lookup, conn->config->crl_lookup_ctx);
        RESULT_ENSURE(result == S2N_SUCCESS, S2N_ERR_CANCELLED);
    }

    return S2N_RESULT_OK;
}

int s2n_crl_ossl_verify_callback(int default_ossl_ret, X509_STORE_CTX *ctx)
{
    int err = X509_STORE_CTX_get_error(ctx);
    switch (err) {
        case X509_V_ERR_CRL_NOT_YET_VALID:
        case X509_V_ERR_CRL_HAS_EXPIRED:
        case X509_V_ERR_ERROR_IN_CRL_LAST_UPDATE_FIELD:
        case X509_V_ERR_ERROR_IN_CRL_NEXT_UPDATE_FIELD:
            return 1;
        default:
            return default_ossl_ret;
    }
}

int s2n_crl_lookup_get_cert_issuer_hash(struct s2n_crl_lookup *lookup, uint64_t *hash)
{
    POSIX_ENSURE_REF(lookup);
    POSIX_ENSURE_REF(lookup->cert);
    POSIX_ENSURE_REF(hash);

    unsigned long temp_hash = X509_issuer_name_hash(lookup->cert);
    POSIX_ENSURE(temp_hash != 0, S2N_ERR_INTERNAL_LIBCRYPTO_ERROR);

    *hash = temp_hash;

    return S2N_SUCCESS;
}

int s2n_crl_lookup_set(struct s2n_crl_lookup *lookup, struct s2n_crl *crl)
{
    POSIX_ENSURE_REF(lookup);
    POSIX_ENSURE_REF(crl);
    lookup->crl = crl;
    lookup->status = FINISHED;
    return S2N_SUCCESS;
}

int s2n_crl_lookup_ignore(struct s2n_crl_lookup *lookup)
{
    POSIX_ENSURE_REF(lookup);
    lookup->crl = NULL;
    lookup->status = FINISHED;
    return S2N_SUCCESS;
}

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
#include "tls/extensions/s2n_cert_authorities.h"

#include <openssl/x509.h>

#include "utils/s2n_safety.h"

bool s2n_cert_authorities_supported_from_trust_store()
{
#if S2N_LIBCRYPTO_SUPPORTS_X509_STORE_LIST
    return true;
#else
    return false;
#endif
}

static S2N_RESULT s2n_cert_authorities_set_from_trust_store(struct s2n_config *config)
{
    RESULT_ENSURE_REF(config);

    if (!config->trust_store.trust_store) {
        return S2N_RESULT_OK;
    }

#if S2N_LIBCRYPTO_SUPPORTS_X509_STORE_LIST
    DEFER_CLEANUP(struct s2n_stuffer output = { 0 }, s2n_stuffer_free);
    RESULT_GUARD_POSIX(s2n_stuffer_growable_alloc(&output, 256));

    STACK_OF(X509_OBJECT) *objects = X509_STORE_get0_objects(config->trust_store.trust_store);
    RESULT_ENSURE(objects, S2N_ERR_INTERNAL_LIBCRYPTO_ERROR);

    int objects_count = sk_X509_OBJECT_num(objects);
    RESULT_ENSURE(objects_count >= 0, S2N_ERR_INTERNAL_LIBCRYPTO_ERROR);

    for (int i = 0; i < objects_count; i++) {
        X509_OBJECT *x509_object = sk_X509_OBJECT_value(objects, i);
        RESULT_ENSURE(x509_object, S2N_ERR_INTERNAL_LIBCRYPTO_ERROR);

        X509 *cert = X509_OBJECT_get0_X509(x509_object);
        if (cert == NULL) {
            /* X509_OBJECTs can also be CRLs, resulting in NULL here. Skip. */
            continue;
        }

        X509_NAME *name = X509_get_subject_name(cert);
        RESULT_ENSURE(name, S2N_ERR_INTERNAL_LIBCRYPTO_ERROR);

        const uint8_t *name_bytes = NULL;
        size_t name_size = 0;
        RESULT_GUARD_OSSL(X509_NAME_get0_der(name, &name_bytes, &name_size),
                S2N_ERR_INTERNAL_LIBCRYPTO_ERROR);

        RESULT_GUARD_POSIX(s2n_stuffer_write_uint16(&output, name_size));
        RESULT_GUARD_POSIX(s2n_stuffer_write_bytes(&output, name_bytes, name_size));
        RESULT_ENSURE(s2n_stuffer_data_available(&output) <= S2N_CERT_AUTHORITIES_MAX_SIZE,
                S2N_ERR_TOO_MANY_CAS);
    }

    RESULT_GUARD_POSIX(s2n_stuffer_extract_blob(&output, &config->cert_authorities));
    return S2N_RESULT_OK;
#else
    RESULT_BAIL(S2N_ERR_API_UNSUPPORTED_BY_LIBCRYPTO);
#endif
}

int s2n_config_set_cert_authorities_from_trust_store(struct s2n_config *config)
{
    POSIX_ENSURE_REF(config);
    POSIX_ENSURE(!config->trust_store.loaded_system_certs, S2N_ERR_INVALID_STATE);
    POSIX_GUARD_RESULT(s2n_cert_authorities_set_from_trust_store(config));
    return S2N_SUCCESS;
}

int s2n_cert_authorities_send(struct s2n_connection *conn, struct s2n_stuffer *out)
{
    POSIX_ENSURE_REF(conn);
    POSIX_ENSURE_REF(conn->config);
    POSIX_ENSURE_EQ(conn->mode, S2N_SERVER);
    struct s2n_blob *cert_authorities = &conn->config->cert_authorities;
    POSIX_GUARD(s2n_stuffer_write_uint16(out, cert_authorities->size));
    POSIX_GUARD(s2n_stuffer_write(out, cert_authorities));
    return S2N_SUCCESS;
}

static bool s2n_cert_authorities_should_send(struct s2n_connection *conn)
{
    return conn && conn->config && conn->config->cert_authorities.size > 0;
}

const s2n_extension_type s2n_cert_authorities_extension = {
    .iana_value = TLS_EXTENSION_CERT_AUTHORITIES,
    .minimum_version = S2N_TLS13,
    .is_response = false,
    .send = s2n_cert_authorities_send,
    .should_send = s2n_cert_authorities_should_send,
    /* s2n-tls supports sending the extension, but does not support parsing it.
     * If received, the extension is ignored.
     *
     *= https://www.rfc-editor.org/rfc/rfc8446#section-4.2.4
     *= type=exception
     *= reason=Extension ignored when received - No customer use case
     *# The "certificate_authorities" extension is used to indicate the
     *# certificate authorities (CAs) which an endpoint supports and which
     *# SHOULD be used by the receiving endpoint to guide certificate
     *# selection.
     */
    .recv = s2n_extension_recv_noop,
    .if_missing = s2n_extension_noop_if_missing,
};

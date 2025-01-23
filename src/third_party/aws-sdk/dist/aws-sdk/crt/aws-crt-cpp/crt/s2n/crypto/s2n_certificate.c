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

#ifndef _GNU_SOURCE
    #define _GNU_SOURCE
#endif

#include "crypto/s2n_certificate.h"

#include <openssl/pem.h>
#include <openssl/x509v3.h>
#include <string.h>
#include <strings.h>

#include "api/s2n.h"
#include "crypto/s2n_openssl_x509.h"
#include "tls/extensions/s2n_extension_list.h"
#include "tls/s2n_connection.h"
#include "utils/s2n_array.h"
#include "utils/s2n_mem.h"
#include "utils/s2n_safety.h"

int s2n_cert_set_cert_type(struct s2n_cert *cert, s2n_pkey_type pkey_type)
{
    POSIX_ENSURE_REF(cert);
    cert->pkey_type = pkey_type;
    POSIX_GUARD_RESULT(s2n_pkey_setup_for_type(&cert->public_key, pkey_type));
    return 0;
}

int s2n_create_cert_chain_from_stuffer(struct s2n_cert_chain *cert_chain_out, struct s2n_stuffer *chain_in_stuffer)
{
    DEFER_CLEANUP(struct s2n_stuffer cert_out_stuffer = { 0 }, s2n_stuffer_free);
    POSIX_GUARD(s2n_stuffer_growable_alloc(&cert_out_stuffer, 2048));

    struct s2n_cert **insert = &cert_chain_out->head;
    uint32_t chain_size = 0;
    do {
        struct s2n_cert *new_node = NULL;

        if (s2n_stuffer_certificate_from_pem(chain_in_stuffer, &cert_out_stuffer) < 0) {
            if (chain_size == 0) {
                POSIX_BAIL(S2N_ERR_NO_CERTIFICATE_IN_PEM);
            }
            break;
        }
        struct s2n_blob mem = { 0 };
        POSIX_GUARD(s2n_alloc(&mem, sizeof(struct s2n_cert)));
        POSIX_GUARD(s2n_blob_zero(&mem));
        new_node = (struct s2n_cert *) (void *) mem.data;

        if (s2n_alloc(&new_node->raw, s2n_stuffer_data_available(&cert_out_stuffer)) != S2N_SUCCESS) {
            POSIX_GUARD(s2n_free(&mem));
            S2N_ERROR_PRESERVE_ERRNO();
        }
        if (s2n_stuffer_read(&cert_out_stuffer, &new_node->raw) != S2N_SUCCESS) {
            POSIX_GUARD(s2n_free(&mem));
            S2N_ERROR_PRESERVE_ERRNO();
        }

        /* Additional 3 bytes for the length field in the protocol */
        chain_size += new_node->raw.size + 3;
        new_node->next = NULL;
        *insert = new_node;
        insert = &new_node->next;
    } while (s2n_stuffer_data_available(chain_in_stuffer));

    /* Leftover data at this point means one of two things:
     * A bug in s2n's PEM parsing OR a malformed PEM in the user's chain.
     * Be conservative and fail instead of using a partial chain.
     */
    S2N_ERROR_IF(s2n_stuffer_data_available(chain_in_stuffer) > 0, S2N_ERR_INVALID_PEM);

    cert_chain_out->chain_size = chain_size;

    return 0;
}

int s2n_cert_chain_and_key_set_cert_chain_from_stuffer(struct s2n_cert_chain_and_key *cert_and_key, struct s2n_stuffer *chain_in_stuffer)
{
    return s2n_create_cert_chain_from_stuffer(cert_and_key->cert_chain, chain_in_stuffer);
}

int s2n_cert_chain_and_key_set_cert_chain_bytes(struct s2n_cert_chain_and_key *cert_and_key, uint8_t *cert_chain_pem, uint32_t cert_chain_len)
{
    DEFER_CLEANUP(struct s2n_stuffer chain_in_stuffer = { 0 }, s2n_stuffer_free);

    POSIX_GUARD(s2n_stuffer_init_ro_from_string(&chain_in_stuffer, cert_chain_pem, cert_chain_len));
    POSIX_GUARD(s2n_cert_chain_and_key_set_cert_chain_from_stuffer(cert_and_key, &chain_in_stuffer));

    return S2N_SUCCESS;
}

int s2n_cert_chain_and_key_set_cert_chain(struct s2n_cert_chain_and_key *cert_and_key, const char *cert_chain_pem)
{
    DEFER_CLEANUP(struct s2n_stuffer chain_in_stuffer = { 0 }, s2n_stuffer_free);

    /* Turn the chain into a stuffer */
    POSIX_GUARD(s2n_stuffer_alloc_ro_from_string(&chain_in_stuffer, cert_chain_pem));
    POSIX_GUARD(s2n_cert_chain_and_key_set_cert_chain_from_stuffer(cert_and_key, &chain_in_stuffer));

    return S2N_SUCCESS;
}

int s2n_cert_chain_and_key_set_private_key_from_stuffer(struct s2n_cert_chain_and_key *cert_and_key,
        struct s2n_stuffer *key_in_stuffer, struct s2n_stuffer *key_out_stuffer)
{
    struct s2n_blob key_blob = { 0 };

    POSIX_GUARD(s2n_pkey_zero_init(cert_and_key->private_key));

    /* Convert pem to asn1 and asn1 to the private key. Handles both PKCS#1 and PKCS#8 formats */
    int type = 0;
    POSIX_GUARD(s2n_stuffer_private_key_from_pem(key_in_stuffer, key_out_stuffer, &type));
    key_blob.size = s2n_stuffer_data_available(key_out_stuffer);
    key_blob.data = s2n_stuffer_raw_read(key_out_stuffer, key_blob.size);
    POSIX_ENSURE_REF(key_blob.data);

    POSIX_GUARD_RESULT(s2n_asn1der_to_private_key(cert_and_key->private_key, &key_blob, type));
    return S2N_SUCCESS;
}

int s2n_cert_chain_and_key_set_private_key_bytes(struct s2n_cert_chain_and_key *cert_and_key, uint8_t *private_key_pem, uint32_t private_key_len)
{
    DEFER_CLEANUP(struct s2n_stuffer key_in_stuffer = { 0 }, s2n_stuffer_free);
    DEFER_CLEANUP(struct s2n_stuffer key_out_stuffer = { 0 }, s2n_stuffer_free);

    /* Put the private key pem in a stuffer */
    POSIX_GUARD(s2n_stuffer_init_ro_from_string(&key_in_stuffer, private_key_pem, private_key_len));
    POSIX_GUARD(s2n_stuffer_growable_alloc(&key_out_stuffer, private_key_len));

    POSIX_GUARD(s2n_cert_chain_and_key_set_private_key_from_stuffer(cert_and_key, &key_in_stuffer, &key_out_stuffer));

    return S2N_SUCCESS;
}

int s2n_cert_chain_and_key_set_private_key(struct s2n_cert_chain_and_key *cert_and_key, const char *private_key_pem)
{
    POSIX_ENSURE_REF(private_key_pem);

    DEFER_CLEANUP(struct s2n_stuffer key_in_stuffer = { 0 }, s2n_stuffer_free);
    DEFER_CLEANUP(struct s2n_stuffer key_out_stuffer = { 0 }, s2n_stuffer_free);

    /* Put the private key pem in a stuffer */
    POSIX_GUARD(s2n_stuffer_alloc_ro_from_string(&key_in_stuffer, private_key_pem));
    POSIX_GUARD(s2n_stuffer_growable_alloc(&key_out_stuffer, strlen(private_key_pem)));

    POSIX_GUARD(s2n_cert_chain_and_key_set_private_key_from_stuffer(cert_and_key, &key_in_stuffer, &key_out_stuffer));

    return S2N_SUCCESS;
}

int s2n_cert_chain_and_key_set_ocsp_data(struct s2n_cert_chain_and_key *chain_and_key, const uint8_t *data, uint32_t length)
{
    POSIX_ENSURE_REF(chain_and_key);
    POSIX_GUARD(s2n_free(&chain_and_key->ocsp_status));
    if (data && length) {
        POSIX_GUARD(s2n_alloc(&chain_and_key->ocsp_status, length));
        POSIX_CHECKED_MEMCPY(chain_and_key->ocsp_status.data, data, length);
    }
    return 0;
}

int s2n_cert_chain_and_key_set_sct_list(struct s2n_cert_chain_and_key *chain_and_key, const uint8_t *data, uint32_t length)
{
    POSIX_ENSURE_REF(chain_and_key);
    POSIX_GUARD(s2n_free(&chain_and_key->sct_list));
    if (data && length) {
        POSIX_GUARD(s2n_alloc(&chain_and_key->sct_list, length));
        POSIX_CHECKED_MEMCPY(chain_and_key->sct_list.data, data, length);
    }
    return 0;
}

struct s2n_cert_chain_and_key *s2n_cert_chain_and_key_new(void)
{
    DEFER_CLEANUP(struct s2n_blob chain_and_key_mem = { 0 }, s2n_free);
    PTR_GUARD_POSIX(s2n_alloc(&chain_and_key_mem, sizeof(struct s2n_cert_chain_and_key)));
    PTR_GUARD_POSIX(s2n_blob_zero(&chain_and_key_mem));

    DEFER_CLEANUP(struct s2n_blob cert_chain_mem = { 0 }, s2n_free);
    PTR_GUARD_POSIX(s2n_alloc(&cert_chain_mem, sizeof(struct s2n_cert_chain)));
    PTR_GUARD_POSIX(s2n_blob_zero(&cert_chain_mem));

    DEFER_CLEANUP(struct s2n_blob pkey_mem = { 0 }, s2n_free);
    PTR_GUARD_POSIX(s2n_alloc(&pkey_mem, sizeof(s2n_cert_private_key)));
    PTR_GUARD_POSIX(s2n_blob_zero(&pkey_mem));

    DEFER_CLEANUP(struct s2n_array *cn_names = NULL, s2n_array_free_p);
    cn_names = s2n_array_new(sizeof(struct s2n_blob));
    PTR_ENSURE_REF(cn_names);

    DEFER_CLEANUP(struct s2n_array *san_names = NULL, s2n_array_free_p);
    san_names = s2n_array_new(sizeof(struct s2n_blob));
    PTR_ENSURE_REF(san_names);

    struct s2n_cert_chain_and_key *chain_and_key = (struct s2n_cert_chain_and_key *) (void *) chain_and_key_mem.data;
    chain_and_key->cert_chain = (struct s2n_cert_chain *) (void *) cert_chain_mem.data;
    chain_and_key->private_key = (s2n_cert_private_key *) (void *) pkey_mem.data;
    chain_and_key->cn_names = cn_names;
    chain_and_key->san_names = san_names;

    ZERO_TO_DISABLE_DEFER_CLEANUP(chain_and_key_mem);
    ZERO_TO_DISABLE_DEFER_CLEANUP(cert_chain_mem);
    ZERO_TO_DISABLE_DEFER_CLEANUP(pkey_mem);
    ZERO_TO_DISABLE_DEFER_CLEANUP(cn_names);
    ZERO_TO_DISABLE_DEFER_CLEANUP(san_names);
    return chain_and_key;
}

DEFINE_POINTER_CLEANUP_FUNC(GENERAL_NAMES *, GENERAL_NAMES_free);

int s2n_cert_chain_and_key_load_sans(struct s2n_cert_chain_and_key *chain_and_key, X509 *x509_cert)
{
    POSIX_ENSURE_REF(chain_and_key->san_names);
    POSIX_ENSURE_REF(x509_cert);

    DEFER_CLEANUP(GENERAL_NAMES *san_names = X509_get_ext_d2i(x509_cert, NID_subject_alt_name, NULL, NULL), GENERAL_NAMES_free_pointer);
    if (san_names == NULL) {
        /* No SAN extension */
        return 0;
    }

    const int num_san_names = sk_GENERAL_NAME_num(san_names);
    for (int i = 0; i < num_san_names; i++) {
        GENERAL_NAME *san_name = sk_GENERAL_NAME_value(san_names, i);
        if (!san_name) {
            continue;
        }

        if (san_name->type == GEN_DNS) {
            /* Decoding isn't necessary here since a DNS SAN name is ASCII(type V_ASN1_IA5STRING) */
            unsigned char *san_str = san_name->d.dNSName->data;
            const size_t san_str_len = san_name->d.dNSName->length;
            struct s2n_blob *san_blob = NULL;
            POSIX_GUARD_RESULT(s2n_array_pushback(chain_and_key->san_names, (void **) &san_blob));
            if (!san_blob) {
                POSIX_BAIL(S2N_ERR_NULL_SANS);
            }

            if (s2n_alloc(san_blob, san_str_len)) {
                S2N_ERROR_PRESERVE_ERRNO();
            }

            POSIX_CHECKED_MEMCPY(san_blob->data, san_str, san_str_len);
            san_blob->size = san_str_len;
            /* normalize san_blob to lowercase */
            POSIX_GUARD(s2n_blob_char_to_lower(san_blob));
        }
    }

    return 0;
}

/* Parse CN names from the Subject of the leaf certificate. Technically there can by multiple CNs
 * in the Subject but practically very few certificates in the wild will have more than one CN.
 * Since the data for this certificate is coming from the application and not from an untrusted
 * source, we will try our best to parse all of the CNs.
 *
 * A recent CAB thread proposed removing support for multiple CNs:
 * https://cabforum.org/pipermail/public/2016-April/007242.html
 */

DEFINE_POINTER_CLEANUP_FUNC(unsigned char *, OPENSSL_free);

int s2n_cert_chain_and_key_load_cns(struct s2n_cert_chain_and_key *chain_and_key, X509 *x509_cert)
{
    POSIX_ENSURE_REF(chain_and_key->cn_names);
    POSIX_ENSURE_REF(x509_cert);

    X509_NAME *subject = X509_get_subject_name(x509_cert);
    if (!subject) {
        return 0;
    }

    int lastpos = -1;
    while ((lastpos = X509_NAME_get_index_by_NID(subject, NID_commonName, lastpos)) >= 0) {
        X509_NAME_ENTRY *name_entry = X509_NAME_get_entry(subject, lastpos);
        if (!name_entry) {
            continue;
        }

        ASN1_STRING *asn1_str = X509_NAME_ENTRY_get_data(name_entry);
        if (!asn1_str) {
            continue;
        }

        /* We need to try and decode the CN since it may be encoded as unicode with a
         * direct ASCII equivalent. Any non ASCII bytes in the string will fail later when we
         * actually compare hostnames.
         *
         * `ASN1_STRING_to_UTF8` allocates in both the success case and in the zero return case, but
         * not in the failure case (negative return value). Therefore, we use `ZERO_TO_DISABLE_DEFER_CLEANUP`
         * in the failure case to prevent double-freeing `utf8_str`. For the zero and success cases, `utf8_str`
         * will be freed by the `DEFER_CLEANUP`.
         */
        DEFER_CLEANUP(unsigned char *utf8_str, OPENSSL_free_pointer);
        const int utf8_out_len = ASN1_STRING_to_UTF8(&utf8_str, asn1_str);
        if (utf8_out_len < 0) {
            /* On failure, ASN1_STRING_to_UTF8 does not allocate any memory */
            ZERO_TO_DISABLE_DEFER_CLEANUP(utf8_str);
            continue;
        } else if (utf8_out_len == 0) {
            /* We still need to free memory for this case, so let the DEFER_CLEANUP free it
             * see https://cve.mitre.org/cgi-bin/cvename.cgi?name=CVE-2017-7521 and
             * https://security.archlinux.org/CVE-2017-7521
            */
        } else {
            struct s2n_blob *cn_name = NULL;
            POSIX_GUARD_RESULT(s2n_array_pushback(chain_and_key->cn_names, (void **) &cn_name));
            if (cn_name == NULL) {
                POSIX_BAIL(S2N_ERR_NULL_CN_NAME);
            }

            if (s2n_alloc(cn_name, utf8_out_len) < 0) {
                S2N_ERROR_PRESERVE_ERRNO();
            }
            POSIX_CHECKED_MEMCPY(cn_name->data, utf8_str, utf8_out_len);
            cn_name->size = utf8_out_len;
            /* normalize cn_name to lowercase */
            POSIX_GUARD(s2n_blob_char_to_lower(cn_name));
        }
    }

    return 0;
}

static int s2n_cert_chain_and_key_set_names(struct s2n_cert_chain_and_key *chain_and_key, X509 *cert)
{
    POSIX_GUARD(s2n_cert_chain_and_key_load_sans(chain_and_key, cert));
    /* For current use cases, we *could* avoid populating the common names if any sans were loaded in
     * s2n_cert_chain_and_key_load_sans. Let's unconditionally populate this field to avoid surprises
     * in the future.
     */
    POSIX_GUARD(s2n_cert_chain_and_key_load_cns(chain_and_key, cert));
    return 0;
}

int s2n_cert_chain_and_key_load(struct s2n_cert_chain_and_key *chain_and_key)
{
    POSIX_ENSURE_REF(chain_and_key);
    POSIX_ENSURE_REF(chain_and_key->cert_chain);
    POSIX_ENSURE_REF(chain_and_key->cert_chain->head);
    POSIX_ENSURE_REF(chain_and_key->private_key);
    struct s2n_cert *head = chain_and_key->cert_chain->head;

    DEFER_CLEANUP(X509 *leaf_cert = NULL, X509_free_pointer);
    POSIX_GUARD_RESULT(s2n_openssl_x509_parse(&head->raw, &leaf_cert));
    POSIX_GUARD_RESULT(s2n_openssl_x509_get_cert_info(leaf_cert, &head->info));

    /* Parse the leaf cert for the public key and certificate type */
    DEFER_CLEANUP(struct s2n_pkey public_key = { 0 }, s2n_pkey_free);
    s2n_pkey_type pkey_type = S2N_PKEY_TYPE_UNKNOWN;
    POSIX_GUARD_RESULT(s2n_pkey_from_x509(leaf_cert, &public_key, &pkey_type));

    POSIX_ENSURE(pkey_type != S2N_PKEY_TYPE_UNKNOWN, S2N_ERR_CERT_TYPE_UNSUPPORTED);
    POSIX_GUARD(s2n_cert_set_cert_type(head, pkey_type));

    /* Validate the leaf cert's public key matches the provided private key */
    if (s2n_pkey_check_key_exists(chain_and_key->private_key) == S2N_SUCCESS) {
        POSIX_GUARD(s2n_pkey_match(&public_key, chain_and_key->private_key));
    }

    /* Populate name information from the SAN/CN for the leaf certificate */
    POSIX_GUARD(s2n_cert_chain_and_key_set_names(chain_and_key, leaf_cert));

    /* populate libcrypto nid's required for cert restrictions */
    struct s2n_cert *current = head->next;
    while (current != NULL) {
        DEFER_CLEANUP(X509 *parsed_cert = NULL, X509_free_pointer);
        POSIX_GUARD_RESULT(s2n_openssl_x509_parse(&current->raw, &parsed_cert));
        POSIX_GUARD_RESULT(s2n_openssl_x509_get_cert_info(parsed_cert, &current->info));

        current = current->next;
    }

    return S2N_SUCCESS;
}

int s2n_cert_chain_and_key_load_pem(struct s2n_cert_chain_and_key *chain_and_key, const char *chain_pem, const char *private_key_pem)
{
    POSIX_ENSURE_REF(chain_and_key);

    POSIX_GUARD(s2n_cert_chain_and_key_set_cert_chain(chain_and_key, chain_pem));
    POSIX_GUARD(s2n_cert_chain_and_key_set_private_key(chain_and_key, private_key_pem));

    POSIX_GUARD(s2n_cert_chain_and_key_load(chain_and_key));

    return S2N_SUCCESS;
}

int s2n_cert_chain_and_key_load_public_pem_bytes(struct s2n_cert_chain_and_key *chain_and_key, uint8_t *chain_pem, uint32_t chain_pem_len)
{
    POSIX_GUARD(s2n_cert_chain_and_key_set_cert_chain_bytes(chain_and_key, chain_pem, chain_pem_len));
    POSIX_GUARD(s2n_cert_chain_and_key_load(chain_and_key));
    return S2N_SUCCESS;
}

int s2n_cert_chain_and_key_load_pem_bytes(struct s2n_cert_chain_and_key *chain_and_key, uint8_t *chain_pem,
        uint32_t chain_pem_len, uint8_t *private_key_pem, uint32_t private_key_pem_len)
{
    POSIX_ENSURE_REF(chain_and_key);

    POSIX_GUARD(s2n_cert_chain_and_key_set_cert_chain_bytes(chain_and_key, chain_pem, chain_pem_len));
    POSIX_GUARD(s2n_cert_chain_and_key_set_private_key_bytes(chain_and_key, private_key_pem, private_key_pem_len));

    POSIX_GUARD(s2n_cert_chain_and_key_load(chain_and_key));

    return S2N_SUCCESS;
}

S2N_CLEANUP_RESULT s2n_cert_chain_and_key_ptr_free(struct s2n_cert_chain_and_key **cert_and_key)
{
    RESULT_ENSURE_REF(cert_and_key);
    RESULT_GUARD_POSIX(s2n_cert_chain_and_key_free(*cert_and_key));
    *cert_and_key = NULL;
    return S2N_RESULT_OK;
}

int s2n_cert_chain_and_key_free(struct s2n_cert_chain_and_key *cert_and_key)
{
    if (cert_and_key == NULL) {
        return 0;
    }

    /* Walk the chain and free the certs */
    if (cert_and_key->cert_chain) {
        struct s2n_cert *node = cert_and_key->cert_chain->head;
        while (node) {
            /* Free the cert */
            POSIX_GUARD(s2n_free(&node->raw));
            /* update head so it won't point to freed memory */
            cert_and_key->cert_chain->head = node->next;
            /* Free the node */
            POSIX_GUARD(s2n_free_object((uint8_t **) &node, sizeof(struct s2n_cert)));
            node = cert_and_key->cert_chain->head;
        }

        POSIX_GUARD(s2n_free_object((uint8_t **) &cert_and_key->cert_chain, sizeof(struct s2n_cert_chain)));
    }

    if (cert_and_key->private_key) {
        POSIX_GUARD(s2n_pkey_free(cert_and_key->private_key));
        POSIX_GUARD(s2n_free_object((uint8_t **) &cert_and_key->private_key, sizeof(s2n_cert_private_key)));
    }

    uint32_t len = 0;

    if (cert_and_key->san_names) {
        POSIX_GUARD_RESULT(s2n_array_num_elements(cert_and_key->san_names, &len));
        for (uint32_t i = 0; i < len; i++) {
            struct s2n_blob *san_name = NULL;
            POSIX_GUARD_RESULT(s2n_array_get(cert_and_key->san_names, i, (void **) &san_name));
            POSIX_GUARD(s2n_free(san_name));
        }
        POSIX_GUARD_RESULT(s2n_array_free(cert_and_key->san_names));
        cert_and_key->san_names = NULL;
    }

    if (cert_and_key->cn_names) {
        POSIX_GUARD_RESULT(s2n_array_num_elements(cert_and_key->cn_names, &len));
        for (uint32_t i = 0; i < len; i++) {
            struct s2n_blob *cn_name = NULL;
            POSIX_GUARD_RESULT(s2n_array_get(cert_and_key->cn_names, i, (void **) &cn_name));
            POSIX_GUARD(s2n_free(cn_name));
        }
        POSIX_GUARD_RESULT(s2n_array_free(cert_and_key->cn_names));
        cert_and_key->cn_names = NULL;
    }

    POSIX_GUARD(s2n_free(&cert_and_key->ocsp_status));
    POSIX_GUARD(s2n_free(&cert_and_key->sct_list));

    POSIX_GUARD(s2n_free_object((uint8_t **) &cert_and_key, sizeof(struct s2n_cert_chain_and_key)));
    return 0;
}

int s2n_cert_chain_free(struct s2n_cert_chain *cert_chain)
{
    /* Walk the chain and free the certs/nodes allocated prior to failure */
    if (cert_chain) {
        struct s2n_cert *node = cert_chain->head;
        while (node) {
            /* Free the cert */
            POSIX_GUARD(s2n_free(&node->raw));
            /* update head so it won't point to freed memory */
            cert_chain->head = node->next;
            /* Free the node */
            POSIX_GUARD(s2n_free_object((uint8_t **) &node, sizeof(struct s2n_cert)));
            node = cert_chain->head;
        }
    }

    return S2N_SUCCESS;
}

int s2n_send_cert_chain(struct s2n_connection *conn, struct s2n_stuffer *out, struct s2n_cert_chain_and_key *chain_and_key)
{
    POSIX_ENSURE_REF(conn);
    POSIX_ENSURE_REF(out);
    POSIX_ENSURE_REF(chain_and_key);
    struct s2n_cert_chain *chain = chain_and_key->cert_chain;
    POSIX_ENSURE_REF(chain);
    struct s2n_cert *cur_cert = chain->head;
    POSIX_ENSURE_REF(cur_cert);

    struct s2n_stuffer_reservation cert_chain_size = { 0 };
    POSIX_GUARD(s2n_stuffer_reserve_uint24(out, &cert_chain_size));

    /* Send certs and extensions (in TLS 1.3) */
    bool first_entry = true;
    while (cur_cert) {
        POSIX_ENSURE_REF(cur_cert);
        POSIX_GUARD(s2n_stuffer_write_uint24(out, cur_cert->raw.size));
        POSIX_GUARD(s2n_stuffer_write_bytes(out, cur_cert->raw.data, cur_cert->raw.size));

        /* According to https://tools.ietf.org/html/rfc8446#section-4.4.2,
         * If an extension applies to the entire chain, it SHOULD be included in
         * the first CertificateEntry.
         * While the spec allow extensions to be included in other certificate
         * entries, only the first matter to use here */
        if (conn->actual_protocol_version >= S2N_TLS13) {
            if (first_entry) {
                POSIX_GUARD(s2n_extension_list_send(S2N_EXTENSION_LIST_CERTIFICATE, conn, out));
                first_entry = false;
            } else {
                POSIX_GUARD(s2n_extension_list_send(S2N_EXTENSION_LIST_EMPTY, conn, out));
            }
        }
        cur_cert = cur_cert->next;
    }

    POSIX_GUARD(s2n_stuffer_write_vector_size(&cert_chain_size));

    return 0;
}

int s2n_send_empty_cert_chain(struct s2n_stuffer *out)
{
    POSIX_ENSURE_REF(out);
    POSIX_GUARD(s2n_stuffer_write_uint24(out, 0));
    return 0;
}

static int s2n_does_cert_san_match_hostname(const struct s2n_cert_chain_and_key *chain_and_key, const struct s2n_blob *dns_name)
{
    POSIX_ENSURE_REF(chain_and_key);
    POSIX_ENSURE_REF(dns_name);

    struct s2n_array *san_names = chain_and_key->san_names;
    uint32_t len = 0;
    POSIX_GUARD_RESULT(s2n_array_num_elements(san_names, &len));
    for (uint32_t i = 0; i < len; i++) {
        struct s2n_blob *san_name = NULL;
        POSIX_GUARD_RESULT(s2n_array_get(san_names, i, (void **) &san_name));
        POSIX_ENSURE_REF(san_name);
        if ((dns_name->size == san_name->size) && (strncasecmp((const char *) dns_name->data, (const char *) san_name->data, dns_name->size) == 0)) {
            return 1;
        }
    }

    return 0;
}

static int s2n_does_cert_cn_match_hostname(const struct s2n_cert_chain_and_key *chain_and_key, const struct s2n_blob *dns_name)
{
    POSIX_ENSURE_REF(chain_and_key);
    POSIX_ENSURE_REF(dns_name);

    struct s2n_array *cn_names = chain_and_key->cn_names;
    uint32_t len = 0;
    POSIX_GUARD_RESULT(s2n_array_num_elements(cn_names, &len));
    for (uint32_t i = 0; i < len; i++) {
        struct s2n_blob *cn_name = NULL;
        POSIX_GUARD_RESULT(s2n_array_get(cn_names, i, (void **) &cn_name));
        POSIX_ENSURE_REF(cn_name);
        if ((dns_name->size == cn_name->size) && (strncasecmp((const char *) dns_name->data, (const char *) cn_name->data, dns_name->size) == 0)) {
            return 1;
        }
    }

    return 0;
}

int s2n_cert_chain_and_key_matches_dns_name(const struct s2n_cert_chain_and_key *chain_and_key, const struct s2n_blob *dns_name)
{
    uint32_t len = 0;
    POSIX_GUARD_RESULT(s2n_array_num_elements(chain_and_key->san_names, &len));
    if (len > 0) {
        if (s2n_does_cert_san_match_hostname(chain_and_key, dns_name)) {
            return 1;
        }
    } else {
        /* Per https://tools.ietf.org/html/rfc6125#section-6.4.4 we only will
         * consider the CN for matching if no valid DNS entries are provided
         * in a SAN.
         */
        if (s2n_does_cert_cn_match_hostname(chain_and_key, dns_name)) {
            return 1;
        }
    }

    return 0;
}

int s2n_cert_chain_and_key_set_ctx(struct s2n_cert_chain_and_key *cert_and_key, void *ctx)
{
    cert_and_key->context = ctx;
    return 0;
}

void *s2n_cert_chain_and_key_get_ctx(struct s2n_cert_chain_and_key *cert_and_key)
{
    return cert_and_key->context;
}

s2n_pkey_type s2n_cert_chain_and_key_get_pkey_type(struct s2n_cert_chain_and_key *chain_and_key)
{
    if (chain_and_key == NULL
            || chain_and_key->cert_chain == NULL
            || chain_and_key->cert_chain->head == NULL) {
        return S2N_PKEY_TYPE_UNKNOWN;
    }
    return chain_and_key->cert_chain->head->pkey_type;
}

s2n_cert_private_key *s2n_cert_chain_and_key_get_private_key(struct s2n_cert_chain_and_key *chain_and_key)
{
    PTR_ENSURE_REF(chain_and_key);
    return chain_and_key->private_key;
}

int s2n_cert_chain_get_length(const struct s2n_cert_chain_and_key *chain_and_key, uint32_t *cert_length)
{
    POSIX_ENSURE_REF(chain_and_key);
    POSIX_ENSURE_REF(cert_length);

    struct s2n_cert *head_cert = chain_and_key->cert_chain->head;
    POSIX_ENSURE_REF(head_cert);
    *cert_length = 1;
    struct s2n_cert *next_cert = head_cert->next;
    while (next_cert != NULL) {
        *cert_length += 1;
        next_cert = next_cert->next;
    }

    return S2N_SUCCESS;
}

int s2n_cert_chain_get_cert(const struct s2n_cert_chain_and_key *chain_and_key, struct s2n_cert **out_cert,
        const uint32_t cert_idx)
{
    POSIX_ENSURE_REF(chain_and_key);
    POSIX_ENSURE_REF(out_cert);

    struct s2n_cert *cur_cert = chain_and_key->cert_chain->head;
    POSIX_ENSURE_REF(cur_cert);
    uint32_t counter = 0;

    struct s2n_cert *next_cert = cur_cert->next;

    while ((next_cert != NULL) && (counter < cert_idx)) {
        cur_cert = next_cert;
        next_cert = next_cert->next;
        counter++;
    }

    POSIX_ENSURE(counter == cert_idx, S2N_ERR_NO_CERT_FOUND);
    POSIX_ENSURE(cur_cert != NULL, S2N_ERR_NO_CERT_FOUND);
    *out_cert = cur_cert;

    return S2N_SUCCESS;
}

int s2n_cert_get_der(const struct s2n_cert *cert, const uint8_t **out_cert_der, uint32_t *cert_length)
{
    POSIX_ENSURE_REF(cert);
    POSIX_ENSURE_REF(out_cert_der);
    POSIX_ENSURE_REF(cert_length);

    *cert_length = cert->raw.size;
    *out_cert_der = cert->raw.data;

    return S2N_SUCCESS;
}

static int s2n_asn1_obj_free(ASN1_OBJECT **data)
{
    if (*data != NULL) {
        ASN1_OBJECT_free(*data);
    }
    return S2N_SUCCESS;
}

static int s2n_asn1_string_free(ASN1_STRING **data)
{
    if (*data != NULL) {
        ASN1_STRING_free(*data);
    }
    return S2N_SUCCESS;
}

static int s2n_utf8_string_from_extension_data(const uint8_t *extension_data, uint32_t extension_len, uint8_t *out_data, uint32_t *out_len)
{
    DEFER_CLEANUP(ASN1_STRING *asn1_str = NULL, s2n_asn1_string_free);
    /* Note that d2i_ASN1_UTF8STRING increments *der_in to the byte following the parsed data.
     * Using a temporary variable is mandatory to prevent memory free-ing errors.
     * Ref to the warning section here for more information:
     * https://www.openssl.org/docs/man1.1.0/man3/d2i_ASN1_UTF8STRING.html.
     */
    const uint8_t *asn1_str_data = extension_data;
    asn1_str = d2i_ASN1_UTF8STRING(NULL, (const unsigned char **) (void *) &asn1_str_data, extension_len);
    POSIX_ENSURE(asn1_str != NULL, S2N_ERR_INVALID_X509_EXTENSION_TYPE);
    /* ASN1_STRING_type() returns the type of `asn1_str`, using standard constants such as V_ASN1_OCTET_STRING.
     * Ref: https://www.openssl.org/docs/man1.1.0/man3/ASN1_STRING_type.html.
     */
    int type = ASN1_STRING_type(asn1_str);
    POSIX_ENSURE(type == V_ASN1_UTF8STRING, S2N_ERR_INVALID_X509_EXTENSION_TYPE);

    int len = ASN1_STRING_length(asn1_str);
    if (out_data != NULL) {
        POSIX_ENSURE((int64_t) *out_len >= (int64_t) len, S2N_ERR_INSUFFICIENT_MEM_SIZE);
        /* ASN1_STRING_data() returns an internal pointer to the data.
        * Since this is an internal pointer it should not be freed or modified in any way.
        * Ref: https://www.openssl.org/docs/man1.0.2/man3/ASN1_STRING_data.html.
        */
        unsigned char *internal_data = ASN1_STRING_data(asn1_str);
        POSIX_ENSURE_REF(internal_data);
        POSIX_CHECKED_MEMCPY(out_data, internal_data, len);
    }
    *out_len = len;
    return S2N_SUCCESS;
}

int s2n_cert_get_utf8_string_from_extension_data_length(const uint8_t *extension_data, uint32_t extension_len, uint32_t *utf8_str_len)
{
    POSIX_ENSURE_REF(extension_data);
    POSIX_ENSURE_GT(extension_len, 0);
    POSIX_ENSURE_REF(utf8_str_len);

    POSIX_GUARD(s2n_utf8_string_from_extension_data(extension_data, extension_len, NULL, utf8_str_len));

    return S2N_SUCCESS;
}

int s2n_cert_get_utf8_string_from_extension_data(const uint8_t *extension_data, uint32_t extension_len, uint8_t *out_data, uint32_t *out_len)
{
    POSIX_ENSURE_REF(extension_data);
    POSIX_ENSURE_GT(extension_len, 0);
    POSIX_ENSURE_REF(out_data);
    POSIX_ENSURE_REF(out_len);

    POSIX_GUARD(s2n_utf8_string_from_extension_data(extension_data, extension_len, out_data, out_len));

    return S2N_SUCCESS;
}

static int s2n_parse_x509_extension(struct s2n_cert *cert, const uint8_t *oid,
        uint8_t *ext_value, uint32_t *ext_value_len, bool *critical)
{
    POSIX_ENSURE_REF(cert->raw.data);
    /* Obtain the openssl x509 cert from the ASN1 DER certificate input.
     * Note that d2i_X509 increments *der_in to the byte following the parsed data.
     * Using a temporary variable is mandatory to prevent memory free-ing errors.
     * Ref to the warning section here for more information:
     * https://www.openssl.org/docs/man1.1.0/man3/d2i_X509.html.
     */
    uint8_t *der_in = cert->raw.data;
    DEFER_CLEANUP(X509 *x509_cert = d2i_X509(NULL, (const unsigned char **) (void *) &der_in, cert->raw.size),
            X509_free_pointer);
    POSIX_ENSURE_REF(x509_cert);

    /* Retrieve the number of x509 extensions present in the certificate
     * X509_get_ext_count returns the number of extensions in the x509 certificate.
     * Ref: https://www.openssl.org/docs/man1.1.0/man3/X509_get_ext_count.html.
     */
    int ext_count_value = X509_get_ext_count(x509_cert);
    POSIX_ENSURE_GT(ext_count_value, 0);
    size_t ext_count = (size_t) ext_count_value;

    /* OBJ_txt2obj() converts the input text string into an ASN1_OBJECT structure.
     * If no_name is 0 then long names and short names will be interpreted as well as numerical forms.
     * If no_name is 1 only the numerical form is acceptable.
     * Ref: https://www.openssl.org/docs/man1.1.0/man3/OBJ_txt2obj.html.
     */
    DEFER_CLEANUP(ASN1_OBJECT *asn1_obj_in = OBJ_txt2obj((const char *) oid, 0), s2n_asn1_obj_free);
    POSIX_ENSURE_REF(asn1_obj_in);

    for (size_t loc = 0; loc < ext_count; loc++) {
        ASN1_OCTET_STRING *asn1_str = NULL;
        bool match_found = false;

        /* Retrieve the x509 extension at location loc.
         * X509_get_ext() retrieves extension loc from x.
         * The index loc can take any value from 0 to X509_get_ext_count(x) - 1.
         * The returned extension is an internal pointer which must not be freed up by the application.
         * Ref: https://www.openssl.org/docs/man1.1.0/man3/X509_get_ext.html.
         */
        X509_EXTENSION *x509_ext = X509_get_ext(x509_cert, loc);
        POSIX_ENSURE_REF(x509_ext);

        /* Retrieve the extension object/OID/extnId.
         * X509_EXTENSION_get_object() returns the extension type of `x509_ext` as an ASN1_OBJECT pointer.
         * The returned pointer is an internal value which must not be freed up.
         * Ref: https://www.openssl.org/docs/man1.1.0/man3/X509_EXTENSION_get_object.html.
         */
        ASN1_OBJECT *asn1_obj = X509_EXTENSION_get_object(x509_ext);
        POSIX_ENSURE_REF(asn1_obj);

        /* OBJ_cmp() compares two ASN1_OBJECT objects. If the two are identical 0 is returned.
         * Ref: https://www.openssl.org/docs/man1.1.0/man3/OBJ_cmp.html.
         */
        match_found = (0 == OBJ_cmp(asn1_obj_in, asn1_obj));

        /* If match found, retrieve the corresponding OID value for the x509 extension */
        if (match_found) {
            /* X509_EXTENSION_get_data() returns the data of extension `x509_ext`.
             * The returned pointer is an internal value which must not be freed up.
             * Ref: https://www.openssl.org/docs/man1.1.0/man3/X509_EXTENSION_get_data.html.
             */
            asn1_str = X509_EXTENSION_get_data(x509_ext);
            /* ASN1_STRING_length() returns the length of the content of `asn1_str`.
            * Ref: https://www.openssl.org/docs/man1.1.0/man3/ASN1_STRING_length.html.
            */
            int len = ASN1_STRING_length(asn1_str);
            if (ext_value != NULL) {
                POSIX_ENSURE_GTE(len, 0);
                POSIX_ENSURE(*ext_value_len >= (uint32_t) len, S2N_ERR_INSUFFICIENT_MEM_SIZE);
                /* ASN1_STRING_data() returns an internal pointer to the data.
                 * Since this is an internal pointer it should not be freed or modified in any way.
                 * Ref: https://www.openssl.org/docs/man1.0.2/man3/ASN1_STRING_data.html.
                 */
                unsigned char *internal_data = ASN1_STRING_data(asn1_str);
                POSIX_ENSURE_REF(internal_data);
                POSIX_CHECKED_MEMCPY(ext_value, internal_data, len);
            }
            if (critical != NULL) {
                /* Retrieve the x509 extension's critical value.
                 * X509_EXTENSION_get_critical() returns the criticality of extension `x509_ext`,
                 * it returns 1 for critical and 0 for non-critical.
                 * Ref: https://www.openssl.org/docs/man1.1.0/man3/X509_EXTENSION_get_critical.html.
                 */
                *critical = X509_EXTENSION_get_critical(x509_ext);
            }
            *ext_value_len = len;
            return S2N_SUCCESS;
        }
    }

    POSIX_BAIL(S2N_ERR_X509_EXTENSION_VALUE_NOT_FOUND);
}

int s2n_cert_get_x509_extension_value_length(struct s2n_cert *cert, const uint8_t *oid, uint32_t *ext_value_len)
{
    POSIX_ENSURE_REF(cert);
    POSIX_ENSURE_REF(oid);
    POSIX_ENSURE_REF(ext_value_len);

    POSIX_GUARD(s2n_parse_x509_extension(cert, oid, NULL, ext_value_len, NULL));

    return S2N_SUCCESS;
}

int s2n_cert_get_x509_extension_value(struct s2n_cert *cert, const uint8_t *oid,
        uint8_t *ext_value, uint32_t *ext_value_len, bool *critical)
{
    POSIX_ENSURE_REF(cert);
    POSIX_ENSURE_REF(oid);
    POSIX_ENSURE_REF(ext_value);
    POSIX_ENSURE_REF(ext_value_len);
    POSIX_ENSURE_REF(critical);

    POSIX_GUARD(s2n_parse_x509_extension(cert, oid, ext_value, ext_value_len, critical));

    return S2N_SUCCESS;
}

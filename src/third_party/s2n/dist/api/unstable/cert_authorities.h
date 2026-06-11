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

/**
 * @file cert_authorities.h
 *
 * This contains unstable APIs for selecting a client-sent certificate in
 * response to the CertificateRequest message, and the necessary server-side
 * work to support that selection (currently, just certificate_authorities).
 *
 * See also https://github.com/aws/s2n-tls/issues/5330. Please comment there if
 * you're using the feature and report back on your experience.
 */

#pragma once

#include <s2n.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/uio.h>

struct s2n_certificate_request;
struct s2n_certificate_authority_list;

/**
 * Get the certificate_authorities list from the certificate request.
 *
 * @returns The list, with a lifetime equivalent to the s2n_certificate_request.
 * This points into the certificate request (it is not a copy of the list).
 * Can be null if the list is not available.
 */
S2N_API struct s2n_certificate_authority_list *s2n_certificate_request_get_ca_list(struct s2n_certificate_request *request);

/**
 * Sets the certificate chain to return in response to the certificate request.
 *
 * @param chain must outlive the connection being served.
 *
 * @warning It is not recommended to free or modify the `chain`. It must
 * outlive this connection.
 *
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API int s2n_certificate_request_set_certificate(struct s2n_certificate_request *request, struct s2n_cert_chain_and_key *chain);

/**
 * Checks whether the offered list has a next entry.
 *
 * @param list A pointer to the certificate authority list being read.
 * @returns bool If true, there is a next entry.
 */
S2N_API bool s2n_certificate_authority_list_has_next(struct s2n_certificate_authority_list *list);

/**
 * Obtains the next entry in the certificate authority list.
 *
 * The lifetime of the name is bound to the lifetime of the list (in other
 * words, it must not be used past the certificate request callback).
 *
 * The name contains the bytes sent by the server. This should be a DER-encoded
 * DistinguishedName, but s2n-tls does not currently strictly enforce the
 * contents of the parsed name.
 *
 * See https://tools.ietf.org/rfc/rfc8446#section-4.2.4.
 *
 * @param list A pointer to the list being read.
 * @param name An out-pointer to the next name in the list.
 * @param length An out-pointer which is initialized to the length of the next name.
 *
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
S2N_API int s2n_certificate_authority_list_next(struct s2n_certificate_authority_list *list, uint8_t **name, uint16_t *length);

/**
 * Returns the certificate authority list to its original state.
 *
 * When `s2n_certificate_authority_list_reread` is called, the list is reset to the beginning.
 *
 * @param list The list to re-read.
 */
S2N_API int s2n_certificate_authority_list_reread(struct s2n_certificate_authority_list *list);

/* CertificateRequest callback.
 *
 * @param conn The connection object for the in-progress connection.
 * @param ctx A pointer to a user defined context provided in s2n_config_set_cert_request_callback.
 * @param request CertificateRequest, containing metadata about the incoming
 * request and a setter for the certificate to use.
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure
 */
typedef int (*s2n_cert_request_callback)(struct s2n_connection *conn, void *ctx, struct s2n_certificate_request *request);

/* Allows the caller to set a callback function that will be called after
 * CertificateRequest message is parsed.
 *
 * This is currently only called on the client side.
 *
 * @param config A pointer to the s2n_config object.
 * @param cert_req_callback The callback to be invoked.
 * @param ctx A pointer to a user defined context to be sent to `cert_req_callback`.
 * @returns S2N_SUCCESS on success. S2N_FAILURE on failure.
 */
S2N_API int s2n_config_set_cert_request_callback(struct s2n_config *config, s2n_cert_request_callback cert_req_callback, void *ctx);

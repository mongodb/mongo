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

#include "crypto/s2n_certificate.h"
#include "crypto/s2n_signature.h"
#include "tls/s2n_cipher_suites.h"

int s2n_get_auth_method_for_cert_type(s2n_pkey_type cert_type, s2n_authentication_method *auth_method);
int s2n_is_cipher_suite_valid_for_auth(struct s2n_connection *conn, struct s2n_cipher_suite *cipher_suite);
int s2n_is_sig_scheme_valid_for_auth(struct s2n_connection *conn, const struct s2n_signature_scheme *sig_scheme);
int s2n_is_cert_type_valid_for_auth(struct s2n_connection *conn, s2n_pkey_type cert_type);
int s2n_select_certs_for_server_auth(struct s2n_connection *conn, struct s2n_cert_chain_and_key **chosen_certs);

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

#include <stdint.h>

#include "api/s2n.h"
#include "stuffer/s2n_stuffer.h"
#include "tls/extensions/s2n_extension_list.h"
#include "utils/s2n_array.h"
/*
 * the 'data' pointers in the below blobs
 * point to data in the raw_message stuffer
 */
struct s2n_client_hello {
    struct s2n_blob raw_message;

    s2n_parsed_extensions_list extensions;
    struct s2n_blob cipher_suites;
    struct s2n_blob session_id;
    struct s2n_blob compression_methods;
    /* The protocol version as written in the client hello */
    uint8_t legacy_version;
    /* The protocol written on the record header containing the client hello */
    uint8_t legacy_record_version;
    /* Tracks if we have recorded the version in the first record */
    unsigned int record_version_recorded : 1;

    unsigned int callback_invoked : 1;
    unsigned int callback_async_blocked : 1;
    unsigned int callback_async_done : 1;
    /*
     * Marks if the client hello has been parsed.
     *
     * While a client_hello is only parsed once, it is possible to parse
     * two different client_hello during a single handshake if the server
     * issues a hello retry.
     */
    unsigned int parsed : 1;
    /*
     * SSLv2 ClientHellos have a different format.
     * Cipher suites are each three bytes instead of two.
     * And due to how s2n-tls parses the record,
     * the raw_message will not contain the protocol version.
     */
    unsigned int sslv2 : 1;
    /*
     * The memory for this structure can be either owned by the application
     * or tied to and managed by a connection.
     *
     * If owned by the application, it can be freed using s2n_client_hello_free.
     * Otherwise, it is freed with s2n_connection_free.
     *
     * We could simplify this by moving the client hello structure off of the
     * connection structure.
     */
    unsigned int alloced : 1;
};

int s2n_client_hello_free_raw_message(struct s2n_client_hello *client_hello);

struct s2n_client_hello *s2n_connection_get_client_hello(struct s2n_connection *conn);

ssize_t s2n_client_hello_get_raw_message_length(struct s2n_client_hello *ch);
ssize_t s2n_client_hello_get_raw_message(struct s2n_client_hello *ch, uint8_t *out, uint32_t max_length);

ssize_t s2n_client_hello_get_cipher_suites_length(struct s2n_client_hello *ch);
ssize_t s2n_client_hello_get_cipher_suites(struct s2n_client_hello *ch, uint8_t *out, uint32_t max_length);

int s2n_client_hello_get_parsed_extension(s2n_tls_extension_type extension_type,
        s2n_parsed_extensions_list *parsed_extension_list, s2n_parsed_extension **parsed_extension);
ssize_t s2n_client_hello_get_extensions_length(struct s2n_client_hello *ch);
ssize_t s2n_client_hello_get_extensions(struct s2n_client_hello *ch, uint8_t *out, uint32_t max_length);

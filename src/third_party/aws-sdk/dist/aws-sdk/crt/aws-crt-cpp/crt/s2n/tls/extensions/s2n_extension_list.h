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

#include "stuffer/s2n_stuffer.h"
#include "tls/extensions/s2n_extension_type.h"

#define S2N_PARSED_EXTENSIONS_COUNT S2N_SUPPORTED_EXTENSIONS_COUNT

typedef struct {
    struct s2n_blob extension;
    uint16_t extension_type;
    uint16_t wire_index;
    unsigned processed : 1;
} s2n_parsed_extension;

typedef struct {
    s2n_parsed_extension parsed_extensions[S2N_PARSED_EXTENSIONS_COUNT];
    struct s2n_blob raw; /* Needed by some ClientHello APIs */
    uint16_t count;
} s2n_parsed_extensions_list;

typedef enum {
    S2N_EXTENSION_LIST_CLIENT_HELLO = 0,
    S2N_EXTENSION_LIST_HELLO_RETRY_REQUEST,
    S2N_EXTENSION_LIST_SERVER_HELLO_DEFAULT,
    S2N_EXTENSION_LIST_SERVER_HELLO_TLS13,
    S2N_EXTENSION_LIST_ENCRYPTED_EXTENSIONS,
    S2N_EXTENSION_LIST_CERT_REQ,
    S2N_EXTENSION_LIST_CERTIFICATE,
    S2N_EXTENSION_LIST_NST,
    S2N_EXTENSION_LIST_ENCRYPTED_EXTENSIONS_TLS12,
    S2N_EXTENSION_LIST_EMPTY,
    S2N_EXTENSION_LIST_IDS_COUNT,
} s2n_extension_list_id;

int s2n_extension_list_send(s2n_extension_list_id list_type, struct s2n_connection *conn, struct s2n_stuffer *out);
int s2n_extension_list_recv(s2n_extension_list_id list_type, struct s2n_connection *conn, struct s2n_stuffer *in);

int s2n_extension_process(const s2n_extension_type *extension_type, struct s2n_connection *conn,
        s2n_parsed_extensions_list *parsed_extension_list);
int s2n_extension_list_process(s2n_extension_list_id list_type, struct s2n_connection *conn,
        s2n_parsed_extensions_list *parsed_extension_list);
int s2n_extension_list_parse(struct s2n_stuffer *in, s2n_parsed_extensions_list *parsed_extension_list);

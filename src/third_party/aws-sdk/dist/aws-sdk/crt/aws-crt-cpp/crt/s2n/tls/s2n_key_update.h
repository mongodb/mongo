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

#include "tls/s2n_connection.h"

#define S2N_KEY_UPDATE_MESSAGE_SIZE 5
#define S2N_KEY_UPDATE_LENGTH       1

typedef enum {
    SENDING = 0,
    RECEIVING
} keyupdate_status;

int s2n_key_update_recv(struct s2n_connection *conn, struct s2n_stuffer *request);
int s2n_key_update_send(struct s2n_connection *conn, s2n_blocked_status *blocked);

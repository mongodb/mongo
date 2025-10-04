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

#include "api/s2n.h"
#include "stuffer/s2n_stuffer.h"
#include "utils/s2n_blob.h"
#include "utils/s2n_result.h"

S2N_RESULT s2n_protocol_preferences_read(struct s2n_stuffer *protocol_preferences, struct s2n_blob *protocol);
S2N_RESULT s2n_protocol_preferences_contain(struct s2n_blob *protocol_preferences, struct s2n_blob *protocol, bool *contains);
S2N_RESULT s2n_select_server_preference_protocol(struct s2n_connection *conn, struct s2n_stuffer *server_list,
        struct s2n_blob *client_list);

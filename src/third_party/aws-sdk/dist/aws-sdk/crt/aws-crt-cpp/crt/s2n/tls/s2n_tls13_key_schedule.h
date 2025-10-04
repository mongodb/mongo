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

#include "tls/s2n_tls13_secrets.h"
#include "utils/s2n_result.h"

struct s2n_key_material;
S2N_RESULT s2n_tls13_key_schedule_generate_key_material(struct s2n_connection *conn,
        s2n_mode sender, struct s2n_key_material *key_material);

S2N_RESULT s2n_tls13_key_schedule_update(struct s2n_connection *conn);
S2N_RESULT s2n_tls13_key_schedule_reset(struct s2n_connection *conn);
S2N_RESULT s2n_tls13_key_schedule_set_key(struct s2n_connection *conn, s2n_extract_secret_type_t secret_type, s2n_mode mode);

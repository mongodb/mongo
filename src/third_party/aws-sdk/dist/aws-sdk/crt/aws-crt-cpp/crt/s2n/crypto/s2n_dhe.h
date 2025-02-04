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

#include <openssl/dh.h>

#include "stuffer/s2n_stuffer.h"
#include "utils/s2n_blob.h"

struct s2n_dh_params {
    DH *dh;
};

int s2n_pkcs3_to_dh_params(struct s2n_dh_params *dh_params, struct s2n_blob *pkcs3);
int s2n_dh_p_g_Ys_to_dh_params(struct s2n_dh_params *server_dh_params, struct s2n_blob *p, struct s2n_blob *g, struct s2n_blob *ys);
int s2n_dh_params_to_p_g_Ys(struct s2n_dh_params *server_dh_params, struct s2n_stuffer *out, struct s2n_blob *output);
int s2n_dh_compute_shared_secret_as_server(struct s2n_dh_params *server_dh_params, struct s2n_stuffer *Yc_in, struct s2n_blob *shared_key);
int s2n_dh_compute_shared_secret_as_client(struct s2n_dh_params *server_dh_params, struct s2n_stuffer *Yc_out, struct s2n_blob *shared_key);
int s2n_dh_params_copy(struct s2n_dh_params *from, struct s2n_dh_params *to);
int s2n_dh_params_check(struct s2n_dh_params *dh_params);
int s2n_dh_generate_ephemeral_key(struct s2n_dh_params *dh_params);
int s2n_dh_params_free(struct s2n_dh_params *dh_params);

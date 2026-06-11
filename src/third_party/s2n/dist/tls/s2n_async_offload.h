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

#include "api/unstable/async_offload.h"
#include "crypto/s2n_signature.h"
#include "tls/s2n_async_pkey.h"
#include "tls/s2n_handshake.h"
#include "utils/s2n_blob.h"
#include "utils/s2n_result.h"

/**
 * Macro to handle async re-entry in a handshake state handler that may invoke the async offloading callback.
 * Add this guard to the code that should only be executed in the initial entry (i.e. when async_state ==
 * S2N_ASYNC_NOT_INVOKED). If the async operation is invoked but not completed, we throw an error to indicate
 * the handshake is still blocked. After the async operation is completed and the user retries s2n_negotiate(),
 * we reset the async_offload_op object and proceed with the remaining code in the current state.
 */
#define S2N_ASYNC_OFFLOAD_POSIX_GUARD(conn, code)                         \
    POSIX_ENSURE_REF(conn);                                               \
    if (conn->async_offload_op.async_state == S2N_ASYNC_NOT_INVOKED) {    \
        code;                                                             \
    } else if (conn->async_offload_op.async_state == S2N_ASYNC_INVOKED) { \
        POSIX_BAIL(S2N_ERR_ASYNC_BLOCKED);                                \
    }                                                                     \
    POSIX_GUARD_RESULT(s2n_async_offload_op_reset(&conn->async_offload_op));

typedef S2N_RESULT (*s2n_async_offload_perform_fn)(struct s2n_async_offload_op *op);

typedef S2N_RESULT (*s2n_async_offload_op_data_free)(struct s2n_async_offload_op *op);

struct s2n_async_offload_op {
    s2n_async_offload_op_type type;
    s2n_async_state async_state;
    struct s2n_connection *conn;
    s2n_async_offload_perform_fn perform;
    s2n_async_offload_op_data_free op_data_free;
    /* Collect arguments required by each operation */
    union {
        struct s2n_async_pkey_verify_data async_pkey_verify;
        /* Add a new struct for each supported op type */
    } op_data;
};

S2N_RESULT s2n_async_offload_cb_invoke(struct s2n_connection *conn, struct s2n_async_offload_op *op);
S2N_RESULT s2n_async_offload_op_wipe(struct s2n_async_offload_op *op);
S2N_RESULT s2n_async_offload_op_reset(struct s2n_async_offload_op *op);
bool s2n_async_offload_op_is_in_allow_list(struct s2n_config *config, s2n_async_offload_op_type op_type);

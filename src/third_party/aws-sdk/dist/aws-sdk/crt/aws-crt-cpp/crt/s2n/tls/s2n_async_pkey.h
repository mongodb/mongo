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
#include "utils/s2n_blob.h"
#include "utils/s2n_result.h"

typedef int (*s2n_async_pkey_sign_complete)(struct s2n_connection *conn, struct s2n_blob *signature);
typedef int (*s2n_async_pkey_decrypt_complete)(struct s2n_connection *conn, bool rsa_failed, struct s2n_blob *decrypted);

struct s2n_async_pkey_op;

/* Guard to handle async states inside handler which uses async pkey operations. If async operation was not invoked
 * it means that we enter this handler for the first time and handler may or may not use async operation, so we let it
 * continue. If async operation is invoking or was invoked, but yet to be complete, we error out of the handler to let
 * s2n_handle_retry_state try again. If async operation was complete we clear the state and let s2n_handle_retry_state
 * proceed to the next handler */
#define S2N_ASYNC_PKEY_GUARD(conn)                                         \
    do {                                                                   \
        __typeof(conn) __tmp_conn = (conn);                                \
        POSIX_GUARD_PTR(__tmp_conn);                                       \
        switch (conn->handshake.async_state) {                             \
            case S2N_ASYNC_NOT_INVOKED:                                    \
                break;                                                     \
                                                                           \
            case S2N_ASYNC_INVOKED:                                        \
                POSIX_BAIL(S2N_ERR_ASYNC_BLOCKED);                         \
                                                                           \
            case S2N_ASYNC_COMPLETE:                                       \
                /* clean up state and return a success from handler */     \
                __tmp_conn->handshake.async_state = S2N_ASYNC_NOT_INVOKED; \
                return S2N_SUCCESS;                                        \
        }                                                                  \
    } while (0)

/* Macros for safe exection of async sign/decrypt.
 *
 * When operation is done asynchronously, we drop to s2n_negotiate loop with S2N_ERR_ASYNC_BLOCKED error and do not
 * perform any of the operations to follow after s2n_async* call. To enforce that there are no operations after the
 * call, we use a macro which directly returns the result of s2n_async* operation forcing compiler to error out on
 * unreachable code and forcing developer to use on_complete function instead */
#define S2N_ASYNC_PKEY_DECRYPT(conn, encrypted, init_decrypted, on_complete) \
    return s2n_result_is_ok(s2n_async_pkey_decrypt(conn, encrypted, init_decrypted, on_complete)) ? S2N_SUCCESS : S2N_FAILURE;

#define S2N_ASYNC_PKEY_SIGN(conn, sig_alg, digest, on_complete) \
    return s2n_result_is_ok(s2n_async_pkey_sign(conn, sig_alg, digest, on_complete)) ? S2N_SUCCESS : S2N_FAILURE;

int s2n_async_pkey_op_perform(struct s2n_async_pkey_op *op, s2n_cert_private_key *key);
int s2n_async_pkey_op_apply(struct s2n_async_pkey_op *op, struct s2n_connection *conn);
int s2n_async_pkey_op_free(struct s2n_async_pkey_op *op);

int s2n_async_pkey_op_get_op_type(struct s2n_async_pkey_op *op, s2n_async_pkey_op_type *type);
int s2n_async_pkey_op_get_input_size(struct s2n_async_pkey_op *op, uint32_t *data_len);
int s2n_async_pkey_op_get_input(struct s2n_async_pkey_op *op, uint8_t *data, uint32_t data_len);
int s2n_async_pkey_op_set_output(struct s2n_async_pkey_op *op, const uint8_t *data, uint32_t data_len);
int s2n_async_pkey_op_set_validation_mode(struct s2n_async_pkey_op *op, s2n_async_pkey_validation_mode mode);

S2N_RESULT s2n_async_pkey_verify_signature(struct s2n_connection *conn, s2n_signature_algorithm sig_alg,
        struct s2n_hash_state *digest, struct s2n_blob *signature);
S2N_RESULT s2n_async_pkey_decrypt(struct s2n_connection *conn, struct s2n_blob *encrypted, struct s2n_blob *init_decrypted,
        s2n_async_pkey_decrypt_complete on_complete);
S2N_RESULT s2n_async_pkey_sign(struct s2n_connection *conn, s2n_signature_algorithm sig_alg, struct s2n_hash_state *digest,
        s2n_async_pkey_sign_complete on_complete);

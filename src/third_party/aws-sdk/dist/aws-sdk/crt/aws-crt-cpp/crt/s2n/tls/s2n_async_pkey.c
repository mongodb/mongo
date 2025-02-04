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
#include "tls/s2n_async_pkey.h"

#include "api/s2n.h"
#include "crypto/s2n_hash.h"
#include "crypto/s2n_signature.h"
#include "error/s2n_errno.h"
#include "tls/s2n_connection.h"
#include "tls/s2n_handshake.h"
#include "utils/s2n_blob.h"
#include "utils/s2n_mem.h"
#include "utils/s2n_result.h"
#include "utils/s2n_safety.h"

struct s2n_async_pkey_decrypt_data {
    s2n_async_pkey_decrypt_complete on_complete;
    struct s2n_blob encrypted;
    struct s2n_blob decrypted;
    unsigned rsa_failed : 1;
};

struct s2n_async_pkey_sign_data {
    s2n_async_pkey_sign_complete on_complete;
    struct s2n_hash_state digest;
    s2n_signature_algorithm sig_alg;
    struct s2n_blob signature;
};

struct s2n_async_pkey_op {
    s2n_async_pkey_op_type type;
    struct s2n_connection *conn;
    s2n_async_pkey_validation_mode validation_mode;
    unsigned complete : 1;
    unsigned applied : 1;
    union {
        struct s2n_async_pkey_decrypt_data decrypt;
        struct s2n_async_pkey_sign_data sign;
    } op;
};

struct s2n_async_pkey_op_actions {
    S2N_RESULT (*perform)(struct s2n_async_pkey_op *op, s2n_cert_private_key *pkey);
    S2N_RESULT (*apply)(struct s2n_async_pkey_op *op, struct s2n_connection *conn);
    S2N_RESULT (*get_input_size)(struct s2n_async_pkey_op *op, uint32_t *data_len);
    S2N_RESULT (*get_input)(struct s2n_async_pkey_op *op, uint8_t *data, uint32_t data_len);
    S2N_RESULT (*set_output)(struct s2n_async_pkey_op *op, const uint8_t *data, uint32_t data_len);
    S2N_RESULT (*free)(struct s2n_async_pkey_op *op);
};

static S2N_RESULT s2n_async_get_actions(s2n_async_pkey_op_type type, const struct s2n_async_pkey_op_actions **actions);

static S2N_RESULT s2n_async_pkey_op_allocate(struct s2n_async_pkey_op **op);

static S2N_RESULT s2n_async_pkey_sign_async(struct s2n_connection *conn, s2n_signature_algorithm sig_alg,
        struct s2n_hash_state *digest, s2n_async_pkey_sign_complete on_complete);
static S2N_RESULT s2n_async_pkey_sign_sync(struct s2n_connection *conn, s2n_signature_algorithm sig_alg,
        struct s2n_hash_state *digest, s2n_async_pkey_sign_complete on_complete);

static S2N_RESULT s2n_async_pkey_decrypt_async(struct s2n_connection *conn, struct s2n_blob *encrypted,
        struct s2n_blob *init_decrypted,
        s2n_async_pkey_decrypt_complete on_complete);
static S2N_RESULT s2n_async_pkey_decrypt_sync(struct s2n_connection *conn, struct s2n_blob *encrypted,
        struct s2n_blob *init_decrypted,
        s2n_async_pkey_decrypt_complete on_complete);

static S2N_RESULT s2n_async_pkey_decrypt_perform(struct s2n_async_pkey_op *op, s2n_cert_private_key *pkey);
static S2N_RESULT s2n_async_pkey_decrypt_apply(struct s2n_async_pkey_op *op, struct s2n_connection *conn);
static S2N_RESULT s2n_async_pkey_get_input_size_decrypt(struct s2n_async_pkey_op *op, uint32_t *data_len);
static S2N_RESULT s2n_async_pkey_get_input_decrypt(struct s2n_async_pkey_op *op, uint8_t *data, uint32_t data_len);
static S2N_RESULT s2n_async_pkey_op_set_output_decrypt(struct s2n_async_pkey_op *op, const uint8_t *data, uint32_t data_len);
static S2N_RESULT s2n_async_pkey_decrypt_free(struct s2n_async_pkey_op *op);

static S2N_RESULT s2n_async_pkey_sign_perform(struct s2n_async_pkey_op *op, s2n_cert_private_key *pkey);
static S2N_RESULT s2n_async_pkey_sign_apply(struct s2n_async_pkey_op *op, struct s2n_connection *conn);
static S2N_RESULT s2n_async_pkey_get_input_size_sign(struct s2n_async_pkey_op *op, uint32_t *data_len);
static S2N_RESULT s2n_async_pkey_get_input_sign(struct s2n_async_pkey_op *op, uint8_t *data, uint32_t data_len);
static S2N_RESULT s2n_async_pkey_op_set_output_sign(struct s2n_async_pkey_op *op, const uint8_t *data, uint32_t data_len);
static S2N_RESULT s2n_async_pkey_sign_free(struct s2n_async_pkey_op *op);

static const struct s2n_async_pkey_op_actions s2n_async_pkey_decrypt_op = {
    .perform = &s2n_async_pkey_decrypt_perform,
    .apply = &s2n_async_pkey_decrypt_apply,
    .get_input_size = &s2n_async_pkey_get_input_size_decrypt,
    .get_input = &s2n_async_pkey_get_input_decrypt,
    .set_output = &s2n_async_pkey_op_set_output_decrypt,
    .free = &s2n_async_pkey_decrypt_free
};

static const struct s2n_async_pkey_op_actions s2n_async_pkey_sign_op = {
    .perform = &s2n_async_pkey_sign_perform,
    .apply = &s2n_async_pkey_sign_apply,
    .get_input_size = &s2n_async_pkey_get_input_size_sign,
    .get_input = &s2n_async_pkey_get_input_sign,
    .set_output = &s2n_async_pkey_op_set_output_sign,
    .free = &s2n_async_pkey_sign_free
};

DEFINE_POINTER_CLEANUP_FUNC(struct s2n_async_pkey_op *, s2n_async_pkey_op_free);

static S2N_RESULT s2n_async_get_actions(s2n_async_pkey_op_type type, const struct s2n_async_pkey_op_actions **actions)
{
    RESULT_ENSURE_REF(actions);

    switch (type) {
        case S2N_ASYNC_DECRYPT:
            *actions = &s2n_async_pkey_decrypt_op;
            return S2N_RESULT_OK;
        case S2N_ASYNC_SIGN:
            *actions = &s2n_async_pkey_sign_op;
            return S2N_RESULT_OK;
            /* No default for compiler warnings */
    }

    RESULT_BAIL(S2N_ERR_SAFETY);
}

static S2N_RESULT s2n_async_pkey_op_allocate(struct s2n_async_pkey_op **op)
{
    RESULT_ENSURE_REF(op);
    RESULT_ENSURE(*op == NULL, S2N_ERR_SAFETY);

    /* allocate memory */
    DEFER_CLEANUP(struct s2n_blob mem = { 0 }, s2n_free);
    RESULT_GUARD_POSIX(s2n_alloc(&mem, sizeof(struct s2n_async_pkey_op)));
    RESULT_GUARD_POSIX(s2n_blob_zero(&mem));

    *op = (void *) mem.data;
    ZERO_TO_DISABLE_DEFER_CLEANUP(mem);
    return S2N_RESULT_OK;
}

S2N_RESULT s2n_async_pkey_decrypt(struct s2n_connection *conn, struct s2n_blob *encrypted,
        struct s2n_blob *init_decrypted, s2n_async_pkey_decrypt_complete on_complete)
{
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE_REF(encrypted);
    RESULT_ENSURE_REF(init_decrypted);
    RESULT_ENSURE_REF(on_complete);

    if (conn->config->async_pkey_cb) {
        RESULT_GUARD(s2n_async_pkey_decrypt_async(conn, encrypted, init_decrypted, on_complete));
    } else {
        RESULT_GUARD(s2n_async_pkey_decrypt_sync(conn, encrypted, init_decrypted, on_complete));
    }

    return S2N_RESULT_OK;
}

S2N_RESULT s2n_async_cb_execute(struct s2n_connection *conn, struct s2n_async_pkey_op **owned_op)
{
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE_REF(owned_op);
    RESULT_ENSURE(conn->handshake.async_state == S2N_ASYNC_NOT_INVOKED, S2N_ERR_ASYNC_MORE_THAN_ONE);

    /* The callback now owns the operation, meaning we can't free it.
     * Wipe our version and pass a copy to the callback.
     */
    struct s2n_async_pkey_op *unowned_op = *owned_op;
    ZERO_TO_DISABLE_DEFER_CLEANUP(*owned_op);

    conn->handshake.async_state = S2N_ASYNC_INVOKED;
    RESULT_ENSURE(conn->config->async_pkey_cb(conn, unowned_op) == S2N_SUCCESS, S2N_ERR_ASYNC_CALLBACK_FAILED);

    /*
     * If the callback already completed the operation, continue.
     * Otherwise, we need to block s2n_negotiate and wait for the operation to complete.
     */
    if (conn->handshake.async_state == S2N_ASYNC_COMPLETE) {
        return S2N_RESULT_OK;
    }
    RESULT_BAIL(S2N_ERR_ASYNC_BLOCKED);
}

S2N_RESULT s2n_async_pkey_decrypt_async(struct s2n_connection *conn, struct s2n_blob *encrypted,
        struct s2n_blob *init_decrypted, s2n_async_pkey_decrypt_complete on_complete)
{
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE_REF(encrypted);
    RESULT_ENSURE_REF(init_decrypted);
    RESULT_ENSURE_REF(on_complete);

    DEFER_CLEANUP(struct s2n_async_pkey_op *op = NULL, s2n_async_pkey_op_free_pointer);
    RESULT_GUARD(s2n_async_pkey_op_allocate(&op));

    op->type = S2N_ASYNC_DECRYPT;
    op->conn = conn;
    op->validation_mode = conn->config->async_pkey_validation_mode;

    struct s2n_async_pkey_decrypt_data *decrypt = &op->op.decrypt;
    decrypt->on_complete = on_complete;

    RESULT_GUARD_POSIX(s2n_dup(encrypted, &decrypt->encrypted));
    RESULT_GUARD_POSIX(s2n_dup(init_decrypted, &decrypt->decrypted));

    RESULT_GUARD(s2n_async_cb_execute(conn, &op));
    return S2N_RESULT_OK;
}

S2N_RESULT s2n_async_pkey_decrypt_sync(struct s2n_connection *conn, struct s2n_blob *encrypted,
        struct s2n_blob *init_decrypted, s2n_async_pkey_decrypt_complete on_complete)
{
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE_REF(encrypted);
    RESULT_ENSURE_REF(init_decrypted);
    RESULT_ENSURE_REF(on_complete);

    const struct s2n_pkey *pkey = conn->handshake_params.our_chain_and_key->private_key;

    bool rsa_failed = s2n_pkey_decrypt(pkey, encrypted, init_decrypted) != S2N_SUCCESS;
    RESULT_GUARD_POSIX(on_complete(conn, rsa_failed, init_decrypted));

    return S2N_RESULT_OK;
}

S2N_RESULT s2n_async_pkey_sign(struct s2n_connection *conn, s2n_signature_algorithm sig_alg,
        struct s2n_hash_state *digest, s2n_async_pkey_sign_complete on_complete)
{
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE_REF(digest);
    RESULT_ENSURE_REF(on_complete);

    if (conn->config->async_pkey_cb) {
        RESULT_GUARD(s2n_async_pkey_sign_async(conn, sig_alg, digest, on_complete));
    } else {
        RESULT_GUARD(s2n_async_pkey_sign_sync(conn, sig_alg, digest, on_complete));
    }

    return S2N_RESULT_OK;
}

S2N_RESULT s2n_async_pkey_sign_async(struct s2n_connection *conn, s2n_signature_algorithm sig_alg,
        struct s2n_hash_state *digest, s2n_async_pkey_sign_complete on_complete)
{
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE_REF(digest);
    RESULT_ENSURE_REF(on_complete);

    DEFER_CLEANUP(struct s2n_async_pkey_op *op = NULL, s2n_async_pkey_op_free_pointer);
    RESULT_GUARD(s2n_async_pkey_op_allocate(&op));

    op->type = S2N_ASYNC_SIGN;
    op->conn = conn;
    op->validation_mode = conn->config->async_pkey_validation_mode;
    if (conn->config->verify_after_sign) {
        op->validation_mode = S2N_ASYNC_PKEY_VALIDATION_STRICT;
    }

    struct s2n_async_pkey_sign_data *sign = &op->op.sign;
    sign->on_complete = on_complete;
    sign->sig_alg = sig_alg;

    RESULT_GUARD_POSIX(s2n_hash_new(&sign->digest));
    RESULT_GUARD_POSIX(s2n_hash_copy(&sign->digest, digest));

    RESULT_GUARD(s2n_async_cb_execute(conn, &op));
    return S2N_RESULT_OK;
}

S2N_RESULT s2n_async_pkey_sign_sync(struct s2n_connection *conn, s2n_signature_algorithm sig_alg,
        struct s2n_hash_state *digest, s2n_async_pkey_sign_complete on_complete)
{
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE_REF(digest);
    RESULT_ENSURE_REF(on_complete);

    const struct s2n_pkey *pkey = conn->handshake_params.our_chain_and_key->private_key;
    DEFER_CLEANUP(struct s2n_blob signed_content = { 0 }, s2n_free);

    uint32_t maximum_signature_length = 0;
    RESULT_GUARD(s2n_pkey_size(pkey, &maximum_signature_length));
    RESULT_GUARD_POSIX(s2n_alloc(&signed_content, maximum_signature_length));

    RESULT_ENSURE_REF(conn->config);
    if (conn->config->verify_after_sign) {
        DEFER_CLEANUP(struct s2n_hash_state digest_for_verify, s2n_hash_free);
        RESULT_GUARD_POSIX(s2n_hash_new(&digest_for_verify));
        RESULT_GUARD_POSIX(s2n_hash_copy(&digest_for_verify, digest));
        RESULT_GUARD_POSIX(s2n_pkey_sign(pkey, sig_alg, digest, &signed_content));
        RESULT_GUARD(s2n_async_pkey_verify_signature(conn, sig_alg, &digest_for_verify, &signed_content));
    } else {
        RESULT_GUARD_POSIX(s2n_pkey_sign(pkey, sig_alg, digest, &signed_content));
    }

    RESULT_GUARD_POSIX(on_complete(conn, &signed_content));

    return S2N_RESULT_OK;
}

int s2n_async_pkey_op_perform(struct s2n_async_pkey_op *op, s2n_cert_private_key *key)
{
    POSIX_ENSURE_REF(op);
    POSIX_ENSURE_REF(key);
    POSIX_ENSURE(!op->complete, S2N_ERR_ASYNC_ALREADY_PERFORMED);

    const struct s2n_async_pkey_op_actions *actions = NULL;
    POSIX_GUARD_RESULT(s2n_async_get_actions(op->type, &actions));
    POSIX_ENSURE_REF(actions);

    POSIX_GUARD_RESULT(actions->perform(op, key));

    op->complete = true;

    return S2N_SUCCESS;
}

int s2n_async_pkey_op_apply(struct s2n_async_pkey_op *op, struct s2n_connection *conn)
{
    POSIX_ENSURE_REF(op);
    POSIX_ENSURE_REF(conn);
    POSIX_ENSURE(op->complete, S2N_ERR_ASYNC_NOT_PERFORMED);
    POSIX_ENSURE(!op->applied, S2N_ERR_ASYNC_ALREADY_APPLIED);
    /* We could have just used op->conn and removed a conn argument, but we want caller
     * to be explicit about connection it wants to resume. Plus this gives more
     * protections in cases if caller frees connection object and then tries to resume
     * the connection. */
    POSIX_ENSURE(op->conn == conn, S2N_ERR_ASYNC_WRONG_CONNECTION);
    POSIX_ENSURE(conn->handshake.async_state == S2N_ASYNC_INVOKED, S2N_ERR_ASYNC_WRONG_CONNECTION);

    const struct s2n_async_pkey_op_actions *actions = NULL;
    POSIX_GUARD_RESULT(s2n_async_get_actions(op->type, &actions));
    POSIX_ENSURE_REF(actions);

    POSIX_GUARD_RESULT(actions->apply(op, conn));

    op->applied = true;
    conn->handshake.async_state = S2N_ASYNC_COMPLETE;

    /* Free up the decrypt/sign structs to avoid storing secrets for too long */
    POSIX_GUARD_RESULT(actions->free(op));

    return S2N_SUCCESS;
}

int s2n_async_pkey_op_free(struct s2n_async_pkey_op *op)
{
    POSIX_ENSURE_REF(op);
    const struct s2n_async_pkey_op_actions *actions = NULL;
    POSIX_GUARD_RESULT(s2n_async_get_actions(op->type, &actions));
    POSIX_ENSURE_REF(actions);

    /* If applied the decrypt/sign structs were released in apply call */
    if (!op->applied) {
        POSIX_GUARD_RESULT(actions->free(op));
    }

    POSIX_GUARD(s2n_free_object((uint8_t **) &op, sizeof(struct s2n_async_pkey_op)));

    return S2N_SUCCESS;
}

S2N_RESULT s2n_async_pkey_decrypt_perform(struct s2n_async_pkey_op *op, s2n_cert_private_key *pkey)
{
    RESULT_ENSURE_REF(op);
    RESULT_ENSURE_REF(pkey);

    struct s2n_async_pkey_decrypt_data *decrypt = &op->op.decrypt;

    decrypt->rsa_failed = s2n_pkey_decrypt(pkey, &decrypt->encrypted, &decrypt->decrypted) != S2N_SUCCESS;

    return S2N_RESULT_OK;
}

S2N_RESULT s2n_async_pkey_decrypt_apply(struct s2n_async_pkey_op *op, struct s2n_connection *conn)
{
    RESULT_ENSURE_REF(op);
    RESULT_ENSURE_REF(conn);

    struct s2n_async_pkey_decrypt_data *decrypt = &op->op.decrypt;

    RESULT_GUARD_POSIX(decrypt->on_complete(conn, decrypt->rsa_failed, &decrypt->decrypted));

    return S2N_RESULT_OK;
}

S2N_RESULT s2n_async_pkey_decrypt_free(struct s2n_async_pkey_op *op)
{
    RESULT_ENSURE_REF(op);

    struct s2n_async_pkey_decrypt_data *decrypt = &op->op.decrypt;

    RESULT_GUARD_POSIX(s2n_blob_zero(&decrypt->decrypted));
    RESULT_GUARD_POSIX(s2n_blob_zero(&decrypt->encrypted));
    RESULT_GUARD_POSIX(s2n_free(&decrypt->decrypted));
    RESULT_GUARD_POSIX(s2n_free(&decrypt->encrypted));

    return S2N_RESULT_OK;
}

S2N_RESULT s2n_async_pkey_sign_perform(struct s2n_async_pkey_op *op, s2n_cert_private_key *pkey)
{
    RESULT_ENSURE_REF(op);
    RESULT_ENSURE_REF(op->conn);
    RESULT_ENSURE_REF(op->conn->config);
    RESULT_ENSURE_REF(pkey);

    struct s2n_async_pkey_sign_data *sign = &op->op.sign;

    uint32_t maximum_signature_length = 0;
    RESULT_GUARD(s2n_pkey_size(pkey, &maximum_signature_length));
    RESULT_GUARD_POSIX(s2n_alloc(&sign->signature, maximum_signature_length));

    /* If validation mode is S2N_ASYNC_PKEY_VALIDATION_STRICT
     * then use local hash copy to sign the signature */
    if (op->validation_mode == S2N_ASYNC_PKEY_VALIDATION_STRICT) {
        DEFER_CLEANUP(struct s2n_hash_state hash_state_copy, s2n_hash_free);
        RESULT_GUARD_POSIX(s2n_hash_new(&hash_state_copy));
        RESULT_GUARD_POSIX(s2n_hash_copy(&hash_state_copy, &sign->digest));

        RESULT_GUARD_POSIX(s2n_pkey_sign(pkey, sign->sig_alg, &hash_state_copy, &sign->signature));
    } else {
        RESULT_GUARD_POSIX(s2n_pkey_sign(pkey, sign->sig_alg, &sign->digest, &sign->signature));
    }

    return S2N_RESULT_OK;
}

S2N_RESULT s2n_async_pkey_sign_apply(struct s2n_async_pkey_op *op, struct s2n_connection *conn)
{
    RESULT_ENSURE_REF(op);
    RESULT_ENSURE_REF(conn);

    struct s2n_async_pkey_sign_data *sign = &op->op.sign;

    /* Perform signature validation only if validation feature is opt in */
    if (op->validation_mode == S2N_ASYNC_PKEY_VALIDATION_STRICT) {
        RESULT_GUARD(s2n_async_pkey_verify_signature(conn, sign->sig_alg, &sign->digest, &sign->signature));
    }

    RESULT_GUARD_POSIX(sign->on_complete(conn, &sign->signature));

    return S2N_RESULT_OK;
}

S2N_RESULT s2n_async_pkey_verify_signature(struct s2n_connection *conn, s2n_signature_algorithm sig_alg,
        struct s2n_hash_state *digest, struct s2n_blob *signature)
{
    RESULT_ENSURE_REF(conn);
    RESULT_ENSURE_REF(conn->handshake_params.our_chain_and_key);
    RESULT_ENSURE_REF(digest);
    RESULT_ENSURE_REF(signature);

    /* Parse public key for the cert */
    DEFER_CLEANUP(struct s2n_pkey public_key = { 0 }, s2n_pkey_free);
    s2n_pkey_type pkey_type = S2N_PKEY_TYPE_UNKNOWN;
    RESULT_GUARD(s2n_asn1der_to_public_key_and_type(&public_key, &pkey_type,
            &conn->handshake_params.our_chain_and_key->cert_chain->head->raw));
    RESULT_ENSURE(s2n_pkey_verify(&public_key, sig_alg, digest, signature) == S2N_SUCCESS, S2N_ERR_VERIFY_SIGNATURE);

    return S2N_RESULT_OK;
}

S2N_RESULT s2n_async_pkey_sign_free(struct s2n_async_pkey_op *op)
{
    RESULT_ENSURE_REF(op);

    struct s2n_async_pkey_sign_data *sign = &op->op.sign;

    RESULT_GUARD_POSIX(s2n_hash_free(&sign->digest));
    RESULT_GUARD_POSIX(s2n_free(&sign->signature));

    return S2N_RESULT_OK;
}

int s2n_async_pkey_op_set_validation_mode(struct s2n_async_pkey_op *op, s2n_async_pkey_validation_mode mode)
{
    POSIX_ENSURE_REF(op);

    switch (mode) {
        case S2N_ASYNC_PKEY_VALIDATION_FAST:
        case S2N_ASYNC_PKEY_VALIDATION_STRICT:
            op->validation_mode = mode;
            return S2N_SUCCESS;
    }

    POSIX_BAIL(S2N_ERR_INVALID_ARGUMENT);
}

int s2n_async_pkey_op_get_op_type(struct s2n_async_pkey_op *op, s2n_async_pkey_op_type *type)
{
    POSIX_ENSURE_REF(op);
    POSIX_ENSURE_REF(type);

    *type = op->type;

    return S2N_SUCCESS;
}

int s2n_async_pkey_op_get_input_size(struct s2n_async_pkey_op *op, uint32_t *data_len)
{
    POSIX_ENSURE_REF(op);
    POSIX_ENSURE_REF(data_len);

    const struct s2n_async_pkey_op_actions *actions = NULL;
    POSIX_GUARD_RESULT(s2n_async_get_actions(op->type, &actions));
    POSIX_ENSURE_REF(actions);

    POSIX_GUARD_RESULT(actions->get_input_size(op, data_len));

    return S2N_SUCCESS;
}

static S2N_RESULT s2n_async_pkey_get_input_size_decrypt(struct s2n_async_pkey_op *op, uint32_t *data_len)
{
    RESULT_ENSURE_REF(op);
    RESULT_ENSURE_REF(data_len);

    struct s2n_async_pkey_decrypt_data *decrypt = &op->op.decrypt;
    struct s2n_blob *in = &decrypt->encrypted;

    *data_len = in->size;
    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_async_pkey_get_input_size_sign(struct s2n_async_pkey_op *op, uint32_t *data_len)
{
    RESULT_ENSURE_REF(op);
    RESULT_ENSURE_REF(data_len);

    struct s2n_async_pkey_sign_data *sign = &op->op.sign;
    struct s2n_hash_state *digest = &sign->digest;

    uint8_t digest_length = 0;
    RESULT_GUARD_POSIX(s2n_hash_digest_size(digest->alg, &digest_length));

    *data_len = digest_length;

    return S2N_RESULT_OK;
}

int s2n_async_pkey_op_get_input(struct s2n_async_pkey_op *op, uint8_t *data, uint32_t data_len)
{
    POSIX_ENSURE_REF(op);
    POSIX_ENSURE_REF(data);

    const struct s2n_async_pkey_op_actions *actions = NULL;
    POSIX_GUARD_RESULT(s2n_async_get_actions(op->type, &actions));
    POSIX_ENSURE_REF(actions);

    POSIX_GUARD_RESULT(actions->get_input(op, data, data_len));

    return S2N_SUCCESS;
}

static S2N_RESULT s2n_async_pkey_get_input_decrypt(struct s2n_async_pkey_op *op, uint8_t *data, uint32_t data_len)
{
    RESULT_ENSURE_REF(op);
    RESULT_ENSURE_REF(data);

    struct s2n_async_pkey_decrypt_data *decrypt = &op->op.decrypt;
    struct s2n_blob *in = &decrypt->encrypted;

    RESULT_ENSURE_LTE(in->size, data_len);

    RESULT_CHECKED_MEMCPY(data, in->data, in->size);

    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_async_pkey_get_input_sign(struct s2n_async_pkey_op *op, uint8_t *data, uint32_t data_len)
{
    RESULT_ENSURE_REF(op);
    RESULT_ENSURE_REF(data);

    struct s2n_async_pkey_sign_data *sign = &op->op.sign;

    DEFER_CLEANUP(struct s2n_hash_state digest_copy = { 0 }, s2n_hash_free);
    RESULT_GUARD_POSIX(s2n_hash_new(&digest_copy));
    RESULT_GUARD_POSIX(s2n_hash_copy(&digest_copy, &sign->digest));

    uint8_t digest_length = 0;

    RESULT_GUARD_POSIX(s2n_hash_digest_size(digest_copy.alg, &digest_length));

    RESULT_ENSURE_LTE(digest_length, data_len);
    RESULT_GUARD_POSIX(s2n_hash_digest(&digest_copy, data, digest_length));

    return S2N_RESULT_OK;
}

int s2n_async_pkey_op_set_output(struct s2n_async_pkey_op *op, const uint8_t *data, uint32_t data_len)
{
    POSIX_ENSURE_REF(op);
    POSIX_ENSURE_REF(data);

    const struct s2n_async_pkey_op_actions *actions = NULL;
    POSIX_GUARD_RESULT(s2n_async_get_actions(op->type, &actions));
    POSIX_ENSURE_REF(actions);

    POSIX_GUARD_RESULT(actions->set_output(op, data, data_len));
    op->complete = true;

    return S2N_SUCCESS;
}

static S2N_RESULT s2n_async_pkey_op_set_output_decrypt(struct s2n_async_pkey_op *op, const uint8_t *data, uint32_t data_len)
{
    RESULT_ENSURE_REF(op);
    RESULT_ENSURE_REF(data);

    struct s2n_async_pkey_decrypt_data *decrypt = &op->op.decrypt;
    struct s2n_blob *out = &decrypt->decrypted;

    RESULT_GUARD_POSIX(s2n_realloc(out, data_len));
    RESULT_CHECKED_MEMCPY(out->data, data, data_len);

    return S2N_RESULT_OK;
}

static S2N_RESULT s2n_async_pkey_op_set_output_sign(struct s2n_async_pkey_op *op, const uint8_t *data, uint32_t data_len)
{
    RESULT_ENSURE_REF(op);
    RESULT_ENSURE_REF(data);

    struct s2n_async_pkey_sign_data *sign = &op->op.sign;
    struct s2n_blob *sigcopy = &sign->signature;

    RESULT_GUARD_POSIX(s2n_realloc(sigcopy, data_len));
    RESULT_CHECKED_MEMCPY(sigcopy->data, data, data_len);

    return S2N_RESULT_OK;
}

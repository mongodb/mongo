/*
 * Copyright 2019-present MongoDB, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "mc-fle-blob-subtype-private.h"
#include "mc-fle2-insert-update-payload-private-v2.h"
#include "mc-fle2-insert-update-payload-private.h"
#include "mc-fle2-payload-iev-private-v2.h"
#include "mc-fle2-payload-iev-private.h"
#include "mc-fle2-payload-uev-private.h"
#include "mc-fle2-payload-uev-v2-private.h"
#include "mongocrypt-ciphertext-private.h"
#include "mongocrypt-crypto-private.h"
#include "mongocrypt-ctx-private.h"
#include "mongocrypt-traverse-util-private.h"

#define CHECK_AND_RETURN(cond)                                                                                         \
    if (!(cond)) {                                                                                                     \
        goto fail;                                                                                                     \
    }

#define CHECK_AND_RETURN_STATUS(cond, msg)                                                                             \
    if (!(cond)) {                                                                                                     \
        CLIENT_ERR(msg);                                                                                               \
        goto fail;                                                                                                     \
    }

#define CHECK_AND_RETURN_KB_STATUS(cond)                                                                               \
    if (!(cond)) {                                                                                                     \
        _mongocrypt_key_broker_status(kb, status);                                                                     \
        goto fail;                                                                                                     \
    }

static bool _replace_FLE2IndexedEncryptedValue_with_plaintext(void *ctx,
                                                              _mongocrypt_buffer_t *in,
                                                              bson_value_t *out,
                                                              mongocrypt_status_t *providedStatus) {
    bool ret = false;
    _mongocrypt_key_broker_t *kb = ctx;
    mc_FLE2IndexedEncryptedValue_t *iev = mc_FLE2IndexedEncryptedValue_new();
    _mongocrypt_buffer_t S_Key = {0};
    _mongocrypt_buffer_t K_Key = {0};
    mongocrypt_status_t *status = providedStatus;

    BSON_ASSERT_PARAM(ctx);
    BSON_ASSERT_PARAM(in);
    BSON_ASSERT_PARAM(out);

    if (providedStatus == NULL) {
        // We accept a NULL status, but add_[SK]_Key require non-NULL.
        // Make a temporary status object as needed.
        status = mongocrypt_status_new();
    }

    // Parse the IEV payload to get S_KeyId.
    CHECK_AND_RETURN(mc_FLE2IndexedEncryptedValue_parse(iev, in, status));
    const _mongocrypt_buffer_t *S_KeyId = mc_FLE2IndexedEncryptedValue_get_S_KeyId(iev, status);
    CHECK_AND_RETURN(S_KeyId);

    CHECK_AND_RETURN_KB_STATUS(_mongocrypt_key_broker_decrypted_key_by_id(kb, S_KeyId, &S_Key));

    // Use S_Key to decrypt envelope and get to K_KeyId.
    CHECK_AND_RETURN(mc_FLE2IndexedEncryptedValue_add_S_Key(kb->crypt->crypto, iev, &S_Key, status));
    const _mongocrypt_buffer_t *K_KeyId = mc_FLE2IndexedEncryptedValue_get_K_KeyId(iev, status);
    CHECK_AND_RETURN(K_KeyId);

    CHECK_AND_RETURN_KB_STATUS(_mongocrypt_key_broker_decrypted_key_by_id(kb, K_KeyId, &K_Key));

    // Decrypt the actual data value using K_Key.
    CHECK_AND_RETURN(mc_FLE2IndexedEncryptedValue_add_K_Key(kb->crypt->crypto, iev, &K_Key, status));
    const _mongocrypt_buffer_t *clientValue = mc_FLE2IndexedEncryptedValue_get_ClientValue(iev, status);
    CHECK_AND_RETURN(clientValue);

    // Marshal BSON value and type into a usable bson_value_t.
    bson_type_t original_bson_type = mc_FLE2IndexedEncryptedValue_get_original_bson_type(iev, status);
    CHECK_AND_RETURN(original_bson_type != BSON_TYPE_EOD);
    CHECK_AND_RETURN_STATUS(
        _mongocrypt_buffer_to_bson_value((_mongocrypt_buffer_t *)clientValue, (uint8_t)original_bson_type, out),
        "decrypted clientValue is not valid BSON");

    ret = true;
fail:
    if (status != providedStatus) {
        mongocrypt_status_destroy(status);
    }
    _mongocrypt_buffer_cleanup(&K_Key);
    _mongocrypt_buffer_cleanup(&S_Key);
    mc_FLE2IndexedEncryptedValue_destroy(iev);
    return ret;
}

static bool _replace_FLE2IndexedEncryptedValueV2_with_plaintext(void *ctx,
                                                                _mongocrypt_buffer_t *in,
                                                                bson_value_t *out,
                                                                mongocrypt_status_t *providedStatus) {
    bool ret = false;
    _mongocrypt_key_broker_t *kb = ctx;
    mc_FLE2IndexedEncryptedValueV2_t *iev = mc_FLE2IndexedEncryptedValueV2_new();
    _mongocrypt_buffer_t S_Key = {0};
    _mongocrypt_buffer_t K_Key = {0};
    mongocrypt_status_t *status = providedStatus;

    BSON_ASSERT_PARAM(ctx);
    BSON_ASSERT_PARAM(in);
    BSON_ASSERT_PARAM(out);

    if (providedStatus == NULL) {
        // We accept a NULL status, but add_[SK]_Key require non-NULL.
        // Make a temporary status object as needed.
        status = mongocrypt_status_new();
    }

    // Parse the IEV payload to get S_KeyId.
    CHECK_AND_RETURN(mc_FLE2IndexedEncryptedValueV2_parse(iev, in, status));
    const _mongocrypt_buffer_t *S_KeyId = mc_FLE2IndexedEncryptedValueV2_get_S_KeyId(iev, status);
    CHECK_AND_RETURN(S_KeyId);
    CHECK_AND_RETURN_KB_STATUS(_mongocrypt_key_broker_decrypted_key_by_id(kb, S_KeyId, &S_Key));

    // Use S_Key to decrypt envelope and get to K_KeyId.
    CHECK_AND_RETURN(mc_FLE2IndexedEncryptedValueV2_add_S_Key(kb->crypt->crypto, iev, &S_Key, status));
    const _mongocrypt_buffer_t *K_KeyId = mc_FLE2IndexedEncryptedValueV2_get_K_KeyId(iev, status);
    CHECK_AND_RETURN(K_KeyId);
    CHECK_AND_RETURN_KB_STATUS(_mongocrypt_key_broker_decrypted_key_by_id(kb, K_KeyId, &K_Key));

    // Decrypt the actual data value using K_Key.
    CHECK_AND_RETURN(mc_FLE2IndexedEncryptedValueV2_add_K_Key(kb->crypt->crypto, iev, &K_Key, status));
    const _mongocrypt_buffer_t *clientValue = mc_FLE2IndexedEncryptedValueV2_get_ClientValue(iev, status);
    CHECK_AND_RETURN(clientValue);

    // Marshal BSON value and type into a usable bson_value_t.
    bson_type_t bson_type = mc_FLE2IndexedEncryptedValueV2_get_bson_value_type(iev, status);
    CHECK_AND_RETURN(bson_type != BSON_TYPE_EOD);
    CHECK_AND_RETURN_STATUS(
        _mongocrypt_buffer_to_bson_value((_mongocrypt_buffer_t *)clientValue, (uint8_t)bson_type, out),
        "decrypted clientValue is not valid BSON");

    ret = true;
fail:
    if (status != providedStatus) {
        mongocrypt_status_destroy(status);
    }
    _mongocrypt_buffer_cleanup(&K_Key);
    _mongocrypt_buffer_cleanup(&S_Key);
    mc_FLE2IndexedEncryptedValueV2_destroy(iev);
    return ret;
}

static bool _replace_FLE2UnindexedEncryptedValue_with_plaintext(void *ctx,
                                                                _mongocrypt_buffer_t *in,
                                                                bson_value_t *out,
                                                                mongocrypt_status_t *status) {
    bool ret = false;
    _mongocrypt_key_broker_t *kb = ctx;
    mc_FLE2UnindexedEncryptedValue_t *uev = mc_FLE2UnindexedEncryptedValue_new();
    _mongocrypt_buffer_t key = {0};

    BSON_ASSERT_PARAM(ctx);
    BSON_ASSERT_PARAM(in);
    BSON_ASSERT_PARAM(out);

    // Parse the UEV payload to get the encryption key.
    CHECK_AND_RETURN(mc_FLE2UnindexedEncryptedValue_parse(uev, in, status));

    const _mongocrypt_buffer_t *key_uuid = mc_FLE2UnindexedEncryptedValue_get_key_uuid(uev, status);
    CHECK_AND_RETURN(key_uuid);

    CHECK_AND_RETURN_KB_STATUS(_mongocrypt_key_broker_decrypted_key_by_id(kb, key_uuid, &key));

    // Decrypt the actual data value using encryption key.
    const _mongocrypt_buffer_t *plaintext =
        mc_FLE2UnindexedEncryptedValue_decrypt(kb->crypt->crypto, uev, &key, status);
    CHECK_AND_RETURN(plaintext);

    // Marshal BSON value and type into a usable bson_value_t.
    bson_type_t original_bson_type = mc_FLE2UnindexedEncryptedValue_get_original_bson_type(uev, status);
    CHECK_AND_RETURN(original_bson_type != BSON_TYPE_EOD);

    CHECK_AND_RETURN_STATUS(
        _mongocrypt_buffer_to_bson_value((_mongocrypt_buffer_t *)plaintext, (uint8_t)original_bson_type, out),
        "decrypted plaintext is not valid BSON");

    ret = true;
fail:
    _mongocrypt_buffer_cleanup(&key);
    mc_FLE2UnindexedEncryptedValue_destroy(uev);
    return ret;
}

static bool _replace_FLE2UnindexedEncryptedValueV2_with_plaintext(void *ctx,
                                                                  _mongocrypt_buffer_t *in,
                                                                  bson_value_t *out,
                                                                  mongocrypt_status_t *status) {
    bool ret = false;
    _mongocrypt_key_broker_t *kb = ctx;
    mc_FLE2UnindexedEncryptedValueV2_t *uev = mc_FLE2UnindexedEncryptedValueV2_new();
    _mongocrypt_buffer_t key = {0};

    BSON_ASSERT_PARAM(ctx);
    BSON_ASSERT_PARAM(in);
    BSON_ASSERT_PARAM(out);

    // Parse the UEV payload to get the encryption key.
    CHECK_AND_RETURN(mc_FLE2UnindexedEncryptedValueV2_parse(uev, in, status));

    const _mongocrypt_buffer_t *key_uuid = mc_FLE2UnindexedEncryptedValueV2_get_key_uuid(uev, status);
    CHECK_AND_RETURN(key_uuid);

    CHECK_AND_RETURN_KB_STATUS(_mongocrypt_key_broker_decrypted_key_by_id(kb, key_uuid, &key));

    // Decrypt the actual data value using encryption key.
    const _mongocrypt_buffer_t *plaintext =
        mc_FLE2UnindexedEncryptedValueV2_decrypt(kb->crypt->crypto, uev, &key, status);
    CHECK_AND_RETURN(plaintext);

    // Marshal BSON value and type into a usable bson_value_t.
    bson_type_t original_bson_type = mc_FLE2UnindexedEncryptedValueV2_get_original_bson_type(uev, status);
    CHECK_AND_RETURN(original_bson_type != BSON_TYPE_EOD);

    CHECK_AND_RETURN_STATUS(
        _mongocrypt_buffer_to_bson_value((_mongocrypt_buffer_t *)plaintext, (uint8_t)original_bson_type, out),
        "decrypted plaintext is not valid BSON");

    ret = true;
fail:
    _mongocrypt_buffer_cleanup(&key);
    mc_FLE2UnindexedEncryptedValueV2_destroy(uev);
    return ret;
}

static bool _replace_FLE2InsertUpdatePayload_with_plaintext(void *ctx,
                                                            _mongocrypt_buffer_t *in,
                                                            bson_value_t *out,
                                                            mongocrypt_status_t *status) {
    bool ret = false;
    _mongocrypt_key_broker_t *kb = ctx;
    mc_FLE2InsertUpdatePayload_t iup;
    _mongocrypt_buffer_t key = {0};

    BSON_ASSERT_PARAM(ctx);
    BSON_ASSERT_PARAM(in);
    BSON_ASSERT_PARAM(out);

    mc_FLE2InsertUpdatePayload_init(&iup);

    // Parse the IUP payload to get the encryption key.
    CHECK_AND_RETURN(mc_FLE2InsertUpdatePayload_parse(&iup, in, status));
    CHECK_AND_RETURN_KB_STATUS(_mongocrypt_key_broker_decrypted_key_by_id(kb, &iup.userKeyId, &key));

    // Decrypt the actual data value using encryption key.
    const _mongocrypt_buffer_t *plaintext = mc_FLE2InsertUpdatePayload_decrypt(kb->crypt->crypto, &iup, &key, status);
    CHECK_AND_RETURN(plaintext);

    // Marshal BSON value and type into a usable bson_value_t.
    bson_type_t original_bson_type = iup.valueType;
    CHECK_AND_RETURN_STATUS(
        _mongocrypt_buffer_to_bson_value((_mongocrypt_buffer_t *)plaintext, (uint8_t)original_bson_type, out),
        "decrypted plaintext is not valid BSON");

    ret = true;
fail:
    _mongocrypt_buffer_cleanup(&key);
    mc_FLE2InsertUpdatePayload_cleanup(&iup);
    return ret;
}

static bool _replace_FLE2InsertUpdatePayloadV2_with_plaintext(void *ctx,
                                                              _mongocrypt_buffer_t *in,
                                                              bson_value_t *out,
                                                              mongocrypt_status_t *status) {
    bool ret = false;
    _mongocrypt_key_broker_t *kb = ctx;
    mc_FLE2InsertUpdatePayloadV2_t iup;
    _mongocrypt_buffer_t key = {0};

    BSON_ASSERT_PARAM(ctx);
    BSON_ASSERT_PARAM(in);
    BSON_ASSERT_PARAM(out);

    mc_FLE2InsertUpdatePayloadV2_init(&iup);

    // Parse the IUP payload to get the encryption key.
    CHECK_AND_RETURN(mc_FLE2InsertUpdatePayloadV2_parse(&iup, in, status));
    CHECK_AND_RETURN_KB_STATUS(_mongocrypt_key_broker_decrypted_key_by_id(kb, &iup.userKeyId, &key));

    // Decrypt the actual data value using encryption key.
    const _mongocrypt_buffer_t *plaintext = mc_FLE2InsertUpdatePayloadV2_decrypt(kb->crypt->crypto, &iup, &key, status);
    CHECK_AND_RETURN(plaintext);

    // Marshal BSON value and type into a usable bson_value_t.
    bson_type_t original_bson_type = iup.valueType;
    CHECK_AND_RETURN_STATUS(
        _mongocrypt_buffer_to_bson_value((_mongocrypt_buffer_t *)plaintext, (uint8_t)original_bson_type, out),
        "decrypted plaintext is not valid BSON");

    ret = true;
fail:
    _mongocrypt_buffer_cleanup(&key);
    mc_FLE2InsertUpdatePayloadV2_cleanup(&iup);
    return ret;
}

static bool _replace_FLE1Payload_with_plaintext(void *ctx,
                                                _mongocrypt_buffer_t *in,
                                                bson_value_t *out,
                                                mongocrypt_status_t *status) {
    _mongocrypt_key_broker_t *kb;
    _mongocrypt_ciphertext_t ciphertext;
    _mongocrypt_buffer_t plaintext;
    _mongocrypt_buffer_t key_material;
    _mongocrypt_buffer_t associated_data;
    uint32_t bytes_written;
    bool ret = false;

    BSON_ASSERT_PARAM(ctx);
    BSON_ASSERT_PARAM(in);
    BSON_ASSERT_PARAM(out);
    BSON_ASSERT(in->data);

    _mongocrypt_buffer_init(&plaintext);
    _mongocrypt_buffer_init(&associated_data);
    _mongocrypt_buffer_init(&key_material);
    kb = (_mongocrypt_key_broker_t *)ctx;

    CHECK_AND_RETURN(_mongocrypt_ciphertext_parse_unowned(in, &ciphertext, status));

    /* look up the key */
    CHECK_AND_RETURN_STATUS(_mongocrypt_key_broker_decrypted_key_by_id(kb, &ciphertext.key_id, &key_material),
                            "key not found");

    const _mongocrypt_value_encryption_algorithm_t *fle1alg = _mcFLE1Algorithm();
    plaintext.len = fle1alg->get_plaintext_len(ciphertext.data.len, status);
    CHECK_AND_RETURN(plaintext.len != 0);
    plaintext.data = bson_malloc0(plaintext.len);
    BSON_ASSERT(plaintext.data);

    plaintext.owned = true;

    CHECK_AND_RETURN_STATUS(_mongocrypt_ciphertext_serialize_associated_data(&ciphertext, &associated_data),
                            "could not serialize associated data");

    CHECK_AND_RETURN(fle1alg->do_decrypt(kb->crypt->crypto,
                                         &associated_data,
                                         &key_material,
                                         &ciphertext.data,
                                         &plaintext,
                                         &bytes_written,
                                         status));

    plaintext.len = bytes_written;

    CHECK_AND_RETURN_STATUS(_mongocrypt_buffer_to_bson_value(&plaintext, ciphertext.original_bson_type, out),
                            "malformed encrypted bson");

    ret = true;
fail:
    _mongocrypt_buffer_cleanup(&plaintext);
    _mongocrypt_buffer_cleanup(&associated_data);
    _mongocrypt_buffer_cleanup(&key_material);
    return ret;
}

static bool _replace_ciphertext_with_plaintext(void *ctx,
                                               _mongocrypt_buffer_t *in,
                                               bson_value_t *out,
                                               mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(ctx);
    BSON_ASSERT_PARAM(in);
    BSON_ASSERT_PARAM(out);
    BSON_ASSERT(in->data);

    switch (in->data[0]) {
    // FLE2v2
    case MC_SUBTYPE_FLE2IndexedEqualityEncryptedValueV2:
    case MC_SUBTYPE_FLE2IndexedRangeEncryptedValueV2:
        return _replace_FLE2IndexedEncryptedValueV2_with_plaintext(ctx, in, out, status);
    case MC_SUBTYPE_FLE2InsertUpdatePayloadV2:
        return _replace_FLE2InsertUpdatePayloadV2_with_plaintext(ctx, in, out, status);
    case MC_SUBTYPE_FLE2UnindexedEncryptedValueV2:
        return _replace_FLE2UnindexedEncryptedValueV2_with_plaintext(ctx, in, out, status);

    // FLE2v1
    case MC_SUBTYPE_FLE2IndexedEqualityEncryptedValue:
    case MC_SUBTYPE_FLE2IndexedRangeEncryptedValue:
        return _replace_FLE2IndexedEncryptedValue_with_plaintext(ctx, in, out, status);
    case MC_SUBTYPE_FLE2InsertUpdatePayload:
        return _replace_FLE2InsertUpdatePayload_with_plaintext(ctx, in, out, status);
    case MC_SUBTYPE_FLE2UnindexedEncryptedValue:
        return _replace_FLE2UnindexedEncryptedValue_with_plaintext(ctx, in, out, status);

    // FLE1
    default: return _replace_FLE1Payload_with_plaintext(ctx, in, out, status);
    }
}

static bool _finalize(mongocrypt_ctx_t *ctx, mongocrypt_binary_t *out) {
    bson_t as_bson, final_bson;
    bson_iter_t iter;
    _mongocrypt_ctx_decrypt_t *dctx;
    bool res;

    if (!ctx) {
        return false;
    }

    if (!out) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "null out parameter");
    }

    dctx = (_mongocrypt_ctx_decrypt_t *)ctx;

    if (ctx->nothing_to_do) {
        _mongocrypt_buffer_to_binary(&dctx->original_doc, out);
        ctx->state = MONGOCRYPT_CTX_DONE;
        return true;
    }

    if (!_mongocrypt_buffer_to_bson(&dctx->original_doc, &as_bson)) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "malformed bson");
    }

    bson_iter_init(&iter, &as_bson);
    bson_init(&final_bson);
    res = _mongocrypt_transform_binary_in_bson(_replace_ciphertext_with_plaintext,
                                               &ctx->kb,
                                               TRAVERSE_MATCH_CIPHERTEXT,
                                               &iter,
                                               &final_bson,
                                               ctx->status);
    if (!res) {
        bson_destroy(&final_bson);
        return _mongocrypt_ctx_fail(ctx);
    }

    _mongocrypt_buffer_steal_from_bson(&dctx->decrypted_doc, &final_bson);
    out->data = dctx->decrypted_doc.data;
    out->len = dctx->decrypted_doc.len;
    ctx->state = MONGOCRYPT_CTX_DONE;
    return true;
}

static bool _collect_S_KeyID_from_FLE2IndexedEncryptedValue(void *ctx,
                                                            const _mongocrypt_buffer_t *in,
                                                            mongocrypt_status_t *status) {
    bool ret = false;
    _mongocrypt_key_broker_t *kb = ctx;
    BSON_ASSERT_PARAM(kb);
    BSON_ASSERT_PARAM(in);

    mc_FLE2IndexedEncryptedValue_t *iev = mc_FLE2IndexedEncryptedValue_new();
    CHECK_AND_RETURN(iev);
    CHECK_AND_RETURN(mc_FLE2IndexedEncryptedValue_parse(iev, in, status));
    const _mongocrypt_buffer_t *S_KeyId = mc_FLE2IndexedEncryptedValue_get_S_KeyId(iev, status);
    CHECK_AND_RETURN(S_KeyId);
    CHECK_AND_RETURN_KB_STATUS(_mongocrypt_key_broker_request_id(kb, S_KeyId));

    ret = true;
fail:
    mc_FLE2IndexedEncryptedValue_destroy(iev);
    return ret;
}

static bool _collect_S_KeyID_from_FLE2IndexedEncryptedValueV2(void *ctx,
                                                              const _mongocrypt_buffer_t *in,
                                                              mongocrypt_status_t *status) {
    bool ret = false;
    _mongocrypt_key_broker_t *kb = ctx;
    BSON_ASSERT_PARAM(kb);
    BSON_ASSERT_PARAM(in);

    mc_FLE2IndexedEncryptedValueV2_t *iev = mc_FLE2IndexedEncryptedValueV2_new();
    CHECK_AND_RETURN(iev);
    CHECK_AND_RETURN(mc_FLE2IndexedEncryptedValueV2_parse(iev, in, status));
    const _mongocrypt_buffer_t *S_KeyId = mc_FLE2IndexedEncryptedValueV2_get_S_KeyId(iev, status);
    CHECK_AND_RETURN(S_KeyId);
    CHECK_AND_RETURN_KB_STATUS(_mongocrypt_key_broker_request_id(kb, S_KeyId));

    ret = true;
fail:
    mc_FLE2IndexedEncryptedValueV2_destroy(iev);
    return ret;
}

static bool _collect_K_KeyID_from_FLE2IndexedEncryptedValueV2(void *ctx,
                                                              const _mongocrypt_buffer_t *in,
                                                              mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(ctx);
    BSON_ASSERT_PARAM(in);
    BSON_ASSERT(in->data);
    bool ret = false;

    BSON_ASSERT((in->data[0] == MC_SUBTYPE_FLE2IndexedEqualityEncryptedValueV2)
                || (in->data[0] == MC_SUBTYPE_FLE2IndexedRangeEncryptedValueV2));

    mc_FLE2IndexedEncryptedValueV2_t *iev = mc_FLE2IndexedEncryptedValueV2_new();
    CHECK_AND_RETURN(iev);
    CHECK_AND_RETURN(mc_FLE2IndexedEncryptedValueV2_parse(iev, in, status));

    const _mongocrypt_buffer_t *S_KeyId = mc_FLE2IndexedEncryptedValueV2_get_S_KeyId(iev, status);
    CHECK_AND_RETURN(S_KeyId);

    _mongocrypt_key_broker_t *kb = ctx;
    _mongocrypt_buffer_t S_Key = {0};
    CHECK_AND_RETURN_KB_STATUS(_mongocrypt_key_broker_decrypted_key_by_id(kb, S_KeyId, &S_Key));

    /* Decrypt InnerEncrypted to get K_KeyId. */
    CHECK_AND_RETURN(mc_FLE2IndexedEncryptedValueV2_add_S_Key(kb->crypt->crypto, iev, &S_Key, status));

    /* Add request for K_KeyId. */
    const _mongocrypt_buffer_t *K_KeyId = mc_FLE2IndexedEncryptedValueV2_get_K_KeyId(iev, status);
    CHECK_AND_RETURN(K_KeyId);

    CHECK_AND_RETURN_KB_STATUS(_mongocrypt_key_broker_request_id(kb, K_KeyId));

    ret = true;
fail:
    _mongocrypt_buffer_cleanup(&S_Key);
    mc_FLE2IndexedEncryptedValueV2_destroy(iev);
    return ret;
}

static bool _collect_K_KeyID_from_FLE2IndexedEncryptedValue(void *ctx,
                                                            const _mongocrypt_buffer_t *in,
                                                            mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(ctx);
    BSON_ASSERT_PARAM(in);
    BSON_ASSERT(in->data);
    bool ret = false;

    BSON_ASSERT((in->data[0] == MC_SUBTYPE_FLE2IndexedEqualityEncryptedValue)
                || (in->data[0] == MC_SUBTYPE_FLE2IndexedRangeEncryptedValue));

    mc_FLE2IndexedEncryptedValue_t *iev = mc_FLE2IndexedEncryptedValue_new();
    CHECK_AND_RETURN(iev);
    CHECK_AND_RETURN(mc_FLE2IndexedEncryptedValue_parse(iev, in, status));

    const _mongocrypt_buffer_t *S_KeyId = mc_FLE2IndexedEncryptedValue_get_S_KeyId(iev, status);
    CHECK_AND_RETURN(S_KeyId);

    _mongocrypt_key_broker_t *kb = ctx;
    _mongocrypt_buffer_t S_Key = {0};
    CHECK_AND_RETURN_KB_STATUS(_mongocrypt_key_broker_decrypted_key_by_id(kb, S_KeyId, &S_Key));

    /* Decrypt InnerEncrypted to get K_KeyId. */
    CHECK_AND_RETURN(mc_FLE2IndexedEncryptedValue_add_S_Key(kb->crypt->crypto, iev, &S_Key, status));

    /* Add request for K_KeyId. */
    const _mongocrypt_buffer_t *K_KeyId = mc_FLE2IndexedEncryptedValue_get_K_KeyId(iev, status);
    CHECK_AND_RETURN(K_KeyId);

    CHECK_AND_RETURN_KB_STATUS(_mongocrypt_key_broker_request_id(kb, K_KeyId));

    ret = true;
fail:
    _mongocrypt_buffer_cleanup(&S_Key);
    mc_FLE2IndexedEncryptedValue_destroy(iev);
    return ret;
}

static bool _collect_K_KeyIDs(void *ctx, _mongocrypt_buffer_t *in, mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(in);
    BSON_ASSERT(in->data);

    switch (in->data[0]) {
    // FLE2v2
    case MC_SUBTYPE_FLE2IndexedEqualityEncryptedValueV2:
    case MC_SUBTYPE_FLE2IndexedRangeEncryptedValueV2:
        return _collect_K_KeyID_from_FLE2IndexedEncryptedValueV2(ctx, in, status);

    // FLE2v1
    case MC_SUBTYPE_FLE2IndexedEqualityEncryptedValue:
    case MC_SUBTYPE_FLE2IndexedRangeEncryptedValue:
        return _collect_K_KeyID_from_FLE2IndexedEncryptedValue(ctx, in, status);

    default:
        // Ignore other types.
        return true;
    }
}

/* _check_for_K_KeyId must be called after requests for all S_KeyId are
 * satisfied. */
static bool _check_for_K_KeyId(mongocrypt_ctx_t *ctx) {
    BSON_ASSERT_PARAM(ctx);

    if (ctx->kb.state != KB_DONE) {
        return true;
    }

    if (!_mongocrypt_key_broker_restart(&ctx->kb)) {
        _mongocrypt_key_broker_status(&ctx->kb, ctx->status);
        return _mongocrypt_ctx_fail(ctx);
    }

    bson_t as_bson;
    bson_iter_t iter;
    _mongocrypt_ctx_decrypt_t *dctx = (_mongocrypt_ctx_decrypt_t *)ctx;
    if (!_mongocrypt_buffer_to_bson(&dctx->original_doc, &as_bson)) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "error converting original_doc to bson");
    }
    bson_iter_init(&iter, &as_bson);

    if (!_mongocrypt_traverse_binary_in_bson(_collect_K_KeyIDs,
                                             &ctx->kb,
                                             TRAVERSE_MATCH_CIPHERTEXT,
                                             &iter,
                                             ctx->status)) {
        return _mongocrypt_ctx_fail(ctx);
    }

    if (!_mongocrypt_key_broker_requests_done(&ctx->kb)) {
        _mongocrypt_key_broker_status(&ctx->kb, ctx->status);
        return _mongocrypt_ctx_fail(ctx);
    }
    return true;
}

static bool _collect_key_uuid_from_FLE2UnindexedEncryptedValue(void *ctx,
                                                               const _mongocrypt_buffer_t *in,
                                                               mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(ctx);
    BSON_ASSERT_PARAM(in);
    bool ret = false;

    mc_FLE2UnindexedEncryptedValue_t *uev = mc_FLE2UnindexedEncryptedValue_new();
    CHECK_AND_RETURN(uev);
    CHECK_AND_RETURN(mc_FLE2UnindexedEncryptedValue_parse(uev, in, status));

    const _mongocrypt_buffer_t *key_uuid = mc_FLE2UnindexedEncryptedValue_get_key_uuid(uev, status);
    CHECK_AND_RETURN(key_uuid);
    _mongocrypt_key_broker_t *kb = ctx;
    CHECK_AND_RETURN_KB_STATUS(_mongocrypt_key_broker_request_id(kb, key_uuid));

    ret = true;
fail:
    mc_FLE2UnindexedEncryptedValue_destroy(uev);
    return ret;
}

static bool _collect_key_uuid_from_FLE2UnindexedEncryptedValueV2(void *ctx,
                                                                 const _mongocrypt_buffer_t *in,
                                                                 mongocrypt_status_t *status) {
    bool ret = false;
    BSON_ASSERT_PARAM(ctx);
    BSON_ASSERT_PARAM(in);

    mc_FLE2UnindexedEncryptedValueV2_t *uev = mc_FLE2UnindexedEncryptedValueV2_new();
    CHECK_AND_RETURN(uev);
    CHECK_AND_RETURN(mc_FLE2UnindexedEncryptedValueV2_parse(uev, in, status));

    const _mongocrypt_buffer_t *key_uuid = mc_FLE2UnindexedEncryptedValueV2_get_key_uuid(uev, status);
    CHECK_AND_RETURN(key_uuid);
    _mongocrypt_key_broker_t *kb = ctx;
    CHECK_AND_RETURN_KB_STATUS(_mongocrypt_key_broker_request_id(kb, key_uuid));

    ret = true;
fail:
    mc_FLE2UnindexedEncryptedValueV2_destroy(uev);
    return ret;
}

static bool
_collect_key_uuid_from_FLE2InsertUpdatePayload(void *ctx, const _mongocrypt_buffer_t *in, mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(ctx);
    BSON_ASSERT_PARAM(in);
    bool ret = false;

    mc_FLE2InsertUpdatePayload_t iup;
    mc_FLE2InsertUpdatePayload_init(&iup);

    CHECK_AND_RETURN(mc_FLE2InsertUpdatePayload_parse(&iup, in, status));
    _mongocrypt_key_broker_t *kb = ctx;
    CHECK_AND_RETURN_KB_STATUS(_mongocrypt_key_broker_request_id(kb, &iup.userKeyId));

    ret = true;
fail:
    mc_FLE2InsertUpdatePayload_cleanup(&iup);
    return ret;
}

static bool _collect_key_uuid_from_FLE2InsertUpdatePayloadV2(void *ctx,
                                                             const _mongocrypt_buffer_t *in,
                                                             mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(ctx);
    BSON_ASSERT_PARAM(in);
    bool ret = false;

    mc_FLE2InsertUpdatePayloadV2_t iup;
    mc_FLE2InsertUpdatePayloadV2_init(&iup);

    CHECK_AND_RETURN(mc_FLE2InsertUpdatePayloadV2_parse(&iup, in, status));
    _mongocrypt_key_broker_t *kb = ctx;
    CHECK_AND_RETURN_KB_STATUS(_mongocrypt_key_broker_request_id(kb, &iup.userKeyId));

    ret = true;
fail:
    mc_FLE2InsertUpdatePayloadV2_cleanup(&iup);
    return ret;
}

static bool _collect_key_uuid_from_FLE1(void *ctx, _mongocrypt_buffer_t *in, mongocrypt_status_t *status) {
    _mongocrypt_ciphertext_t ciphertext;
    _mongocrypt_key_broker_t *kb = (_mongocrypt_key_broker_t *)ctx;

    CHECK_AND_RETURN(_mongocrypt_ciphertext_parse_unowned(in, &ciphertext, status));
    CHECK_AND_RETURN_KB_STATUS(_mongocrypt_key_broker_request_id(kb, &ciphertext.key_id));

    return true;
fail:
    return false;
}

static bool _collect_key_from_ciphertext(void *ctx, _mongocrypt_buffer_t *in, mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(ctx);
    BSON_ASSERT_PARAM(in);
    BSON_ASSERT(in->data);

    switch (in->data[0]) {
    // FLE2v2
    case MC_SUBTYPE_FLE2IndexedEqualityEncryptedValueV2:
    case MC_SUBTYPE_FLE2IndexedRangeEncryptedValueV2:
        return _collect_S_KeyID_from_FLE2IndexedEncryptedValueV2(ctx, in, status);
    case MC_SUBTYPE_FLE2UnindexedEncryptedValueV2:
        return _collect_key_uuid_from_FLE2UnindexedEncryptedValueV2(ctx, in, status);
    case MC_SUBTYPE_FLE2InsertUpdatePayloadV2: return _collect_key_uuid_from_FLE2InsertUpdatePayloadV2(ctx, in, status);

    // FLE2v1
    case MC_SUBTYPE_FLE2IndexedEqualityEncryptedValue:
    case MC_SUBTYPE_FLE2IndexedRangeEncryptedValue:
        return _collect_S_KeyID_from_FLE2IndexedEncryptedValue(ctx, in, status);
    case MC_SUBTYPE_FLE2UnindexedEncryptedValue:
        return _collect_key_uuid_from_FLE2UnindexedEncryptedValue(ctx, in, status);
    case MC_SUBTYPE_FLE2InsertUpdatePayload: return _collect_key_uuid_from_FLE2InsertUpdatePayload(ctx, in, status);

    // FLE1
    default: return _collect_key_uuid_from_FLE1(ctx, in, status);
    }
}

static void _cleanup(mongocrypt_ctx_t *ctx) {
    _mongocrypt_ctx_decrypt_t *dctx;

    if (!ctx) {
        return;
    }
    dctx = (_mongocrypt_ctx_decrypt_t *)ctx;
    _mongocrypt_buffer_cleanup(&dctx->original_doc);
    _mongocrypt_buffer_cleanup(&dctx->decrypted_doc);
}

bool mongocrypt_ctx_explicit_decrypt_init(mongocrypt_ctx_t *ctx, mongocrypt_binary_t *msg) {
    bson_iter_t iter;
    bson_t as_bson;

    if (!ctx) {
        return false;
    }

    if (!msg || !msg->data) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "invalid msg");
    }

    if (ctx->crypt->log.trace_enabled) {
        char *msg_val;
        msg_val = _mongocrypt_new_json_string_from_binary(msg);
        _mongocrypt_log(&ctx->crypt->log, MONGOCRYPT_LOG_LEVEL_TRACE, "%s (%s=\"%s\")", BSON_FUNC, "msg", msg_val);

        bson_free(msg_val);
    }

    /* Expect msg to be the BSON a document of the form:
       { "v" : (BSON BINARY value of subtype 6) }
    */
    if (!_mongocrypt_binary_to_bson(msg, &as_bson)) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "malformed bson");
    }

    if (!bson_iter_init_find(&iter, &as_bson, "v")) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "invalid msg, must contain 'v'");
    }

    if (!BSON_ITER_HOLDS_BINARY(&iter)) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "invalid msg, 'v' must contain a binary");
    }

    {
        bson_subtype_t subtype;
        const uint8_t *binary;
        uint32_t binary_len;
        mongocrypt_status_t *status = ctx->status;

        bson_iter_binary(&iter, &subtype, &binary_len, &binary);
        if (subtype != BSON_SUBTYPE_ENCRYPTED) {
            CLIENT_ERR("decryption expected BSON binary subtype %d, got %d", (int)BSON_SUBTYPE_ENCRYPTED, (int)subtype);
            return _mongocrypt_ctx_fail(ctx);
        }
    }

    if (!mongocrypt_ctx_decrypt_init(ctx, msg)) {
        return false;
    }
    return true;
}

static bool _mongo_done_keys(mongocrypt_ctx_t *ctx) {
    BSON_ASSERT_PARAM(ctx);

    (void)_mongocrypt_key_broker_docs_done(&ctx->kb);
    if (!_check_for_K_KeyId(ctx)) {
        return false;
    }
    return _mongocrypt_ctx_state_from_key_broker(ctx);
}

static bool _kms_done(mongocrypt_ctx_t *ctx) {
    _mongocrypt_opts_kms_providers_t *kms_providers;

    BSON_ASSERT_PARAM(ctx);

    kms_providers = _mongocrypt_ctx_kms_providers(ctx);

    if (!_mongocrypt_key_broker_kms_done(&ctx->kb, kms_providers)) {
        BSON_ASSERT(!_mongocrypt_key_broker_status(&ctx->kb, ctx->status));
        return _mongocrypt_ctx_fail(ctx);
    }
    if (!_check_for_K_KeyId(ctx)) {
        return false;
    }
    return _mongocrypt_ctx_state_from_key_broker(ctx);
}

bool mongocrypt_ctx_decrypt_init(mongocrypt_ctx_t *ctx, mongocrypt_binary_t *doc) {
    _mongocrypt_ctx_decrypt_t *dctx;
    bson_t as_bson;
    bson_iter_t iter;
    _mongocrypt_ctx_opts_spec_t opts_spec;

    memset(&opts_spec, 0, sizeof(opts_spec));
    if (!ctx) {
        return false;
    }

    if (!_mongocrypt_ctx_init(ctx, &opts_spec)) {
        return false;
    }

    if (!doc || !doc->data) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "invalid doc");
    }

    if (ctx->crypt->log.trace_enabled) {
        char *doc_val;
        doc_val = _mongocrypt_new_json_string_from_binary(doc);
        _mongocrypt_log(&ctx->crypt->log, MONGOCRYPT_LOG_LEVEL_TRACE, "%s (%s=\"%s\")", BSON_FUNC, "doc", doc_val);
        bson_free(doc_val);
    }
    dctx = (_mongocrypt_ctx_decrypt_t *)ctx;
    ctx->type = _MONGOCRYPT_TYPE_DECRYPT;
    ctx->vtable.finalize = _finalize;
    ctx->vtable.cleanup = _cleanup;
    ctx->vtable.mongo_done_keys = _mongo_done_keys;
    ctx->vtable.kms_done = _kms_done;

    _mongocrypt_buffer_copy_from_binary(&dctx->original_doc, doc);
    /* get keys. */
    if (!_mongocrypt_buffer_to_bson(&dctx->original_doc, &as_bson)) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "malformed bson");
    }

    bson_iter_init(&iter, &as_bson);
    if (!_mongocrypt_traverse_binary_in_bson(_collect_key_from_ciphertext,
                                             &ctx->kb,
                                             TRAVERSE_MATCH_CIPHERTEXT,
                                             &iter,
                                             ctx->status)) {
        return _mongocrypt_ctx_fail(ctx);
    }

    (void)_mongocrypt_key_broker_requests_done(&ctx->kb);

    if (!_check_for_K_KeyId(ctx)) {
        return false;
    }
    return _mongocrypt_ctx_state_from_key_broker(ctx);
}

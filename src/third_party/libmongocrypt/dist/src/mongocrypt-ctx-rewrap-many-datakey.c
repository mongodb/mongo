/*
 * Copyright 2022-present MongoDB, Inc.
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

#include "mongocrypt-ctx-private.h"

static bool _finalize(mongocrypt_ctx_t *ctx, mongocrypt_binary_t *out) {
    _mongocrypt_ctx_rewrap_many_datakey_t *const rmdctx = (_mongocrypt_ctx_rewrap_many_datakey_t *)ctx;

    bson_t doc = BSON_INITIALIZER;
    bson_t array = BSON_INITIALIZER;

    BSON_ASSERT_PARAM(ctx);
    BSON_ASSERT_PARAM(out);

    BSON_ASSERT(BSON_APPEND_ARRAY_BEGIN(&doc, "v", &array));
    {
        _mongocrypt_ctx_rmd_datakey_t *iter = NULL;
        size_t idx = 0u;

        for (iter = rmdctx->datakeys; iter; (iter = iter->next), (++idx)) {
            mongocrypt_binary_t bin;
            bson_t bson;
            bson_t elem = BSON_INITIALIZER;

            if (!mongocrypt_ctx_finalize(iter->dkctx, &bin)) {
                BSON_ASSERT(bson_append_array_end(&doc, &array));
                bson_destroy(&doc);
                bson_destroy(&elem);
                return _mongocrypt_ctx_fail_w_msg(ctx, "failed to encrypt datakey with new provider");
            }

            BSON_ASSERT(bson_init_static(&bson, bin.data, bin.len));

            /* Among all (possible) fields in key document, the only fields
             * required by caller to construct the corresponding bulk write
             * operations to update the key document with rewrapped key material
             * are:
             *   - _id (same as original key)
             *   - keyMaterial (updated)
             *   - masterKey (updated)
             * Which means the following fields can be excluded:
             *   - _id (discard new ID generated during rewrapping)
             *   - creationDate
             *   - updateDate (updated via the $currentDate operator)
             *   - status
             *   - keyAltNames
             */
            bson_copy_to_excluding_noinit(&bson,
                                          &elem,
                                          "_id",
                                          "creationDate",
                                          "updateDate",
                                          "status",
                                          "keyAltNames",
                                          NULL);

            /* Preserve key ID of original document. */
            BSON_ASSERT(iter->doc);
            BSON_ASSERT(BSON_APPEND_BINARY(&elem, "_id", BSON_SUBTYPE_UUID, iter->doc->id.data, iter->doc->id.len));

            /* Array indicies must be specified manually. */
            {
                char *idx_str = bson_strdup_printf("%zu", idx);
                BSON_ASSERT(BSON_APPEND_DOCUMENT(&array, idx_str, &elem));
                bson_free(idx_str);
            }

            bson_destroy(&elem);
        }
    }
    BSON_ASSERT(bson_append_array_end(&doc, &array));

    /* Extend lifetime of bson so it can be referenced by out parameter. */
    _mongocrypt_buffer_steal_from_bson(&rmdctx->results, &doc);

    out->data = rmdctx->results.data;
    out->len = rmdctx->results.len;

    ctx->state = MONGOCRYPT_CTX_DONE;

    return true;
}

static bool _kms_done_encrypt(mongocrypt_ctx_t *ctx) {
    _mongocrypt_ctx_rewrap_many_datakey_t *const rmdctx = (_mongocrypt_ctx_rewrap_many_datakey_t *)ctx;

    BSON_ASSERT_PARAM(ctx);

    {
        _mongocrypt_ctx_rmd_datakey_t *iter;

        for (iter = rmdctx->datakeys; iter; iter = iter->next) {
            if (iter->dkctx->state == MONGOCRYPT_CTX_NEED_KMS && !mongocrypt_ctx_kms_done(iter->dkctx)) {
                _mongocrypt_status_copy_to(iter->dkctx->status, ctx->status);
                return _mongocrypt_ctx_fail(ctx);
            }
        }
    }

    /* Some providers may require multiple rounds of KMS requests. Reiterate
     * through datakey contexts to verify if more work needs to be done. */
    rmdctx->datakeys_iter = rmdctx->datakeys;

    while (rmdctx->datakeys_iter && rmdctx->datakeys_iter->dkctx->state != MONGOCRYPT_CTX_NEED_KMS) {
        rmdctx->datakeys_iter = rmdctx->datakeys_iter->next;
    }

    if (rmdctx->datakeys_iter) {
        /* More work to be done, remain in MONGOCRYPT_CTX_NEED_KMS state. */
        return true;
    }

    /* All datakeys have been encrypted and are ready to be finalized. */
    ctx->state = MONGOCRYPT_CTX_READY;
    ctx->vtable.finalize = _finalize;

    return true;
}

static mongocrypt_kms_ctx_t *_next_kms_ctx_encrypt(mongocrypt_ctx_t *ctx) {
    _mongocrypt_ctx_rewrap_many_datakey_t *const rmdctx = (_mongocrypt_ctx_rewrap_many_datakey_t *)ctx;

    mongocrypt_ctx_t *dkctx = NULL;

    BSON_ASSERT_PARAM(ctx);

    /* No more datakey contexts requiring KMS. */
    if (!rmdctx->datakeys_iter) {
        return NULL;
    }

    dkctx = rmdctx->datakeys_iter->dkctx;

    /* Skip next iterator ahead to next datakey context that needs KMS. */
    do {
        rmdctx->datakeys_iter = rmdctx->datakeys_iter->next;
    } while (rmdctx->datakeys_iter && rmdctx->datakeys_iter->dkctx->state != MONGOCRYPT_CTX_NEED_KMS);

    return mongocrypt_ctx_next_kms_ctx(dkctx);
}

static bool _add_new_datakey(mongocrypt_ctx_t *ctx, key_returned_t *key) {
    _mongocrypt_ctx_rewrap_many_datakey_t *const rmdctx = (_mongocrypt_ctx_rewrap_many_datakey_t *)ctx;

    BSON_ASSERT_PARAM(ctx);
    BSON_ASSERT_PARAM(key);

    /* Datakey should be fully decrypted at this stage. */
    BSON_ASSERT(key->decrypted);

    _mongocrypt_ctx_rmd_datakey_t *const datakey = bson_malloc0(sizeof(_mongocrypt_ctx_rmd_datakey_t));

    datakey->dkctx = mongocrypt_ctx_new(ctx->crypt);
    datakey->next = rmdctx->datakeys;
    datakey->doc = key->doc;
    rmdctx->datakeys = datakey; /* Ownership transfer. */

    /* Set new provider and master key (rewrapManyDataKeyOpts). */
    if (ctx->opts.kek.kms_provider == MONGOCRYPT_KMS_PROVIDER_NONE) {
        /* Reuse current KMS provider if option not set. */
        _mongocrypt_kek_copy_to(&key->doc->kek, &datakey->dkctx->opts.kek);
    } else {
        /* Apply new KMS provider as given by options. */
        _mongocrypt_kek_copy_to(&ctx->opts.kek, &datakey->dkctx->opts.kek);
    }

    /* Preserve alt names. */
    datakey->dkctx->opts.key_alt_names = _mongocrypt_key_alt_name_copy_all(key->doc->key_alt_names);

    /* Preserve key material. */
    _mongocrypt_buffer_copy_to(&key->decrypted_key_material, &datakey->dkctx->opts.key_material);

    if (!mongocrypt_ctx_datakey_init(datakey->dkctx)) {
        _mongocrypt_status_copy_to(datakey->dkctx->status, ctx->status);
        return _mongocrypt_ctx_fail(ctx);
    }

    /* Reuse KMS credentials provided during decryption. */
    if (datakey->dkctx->state == MONGOCRYPT_CTX_NEED_KMS_CREDENTIALS) {
        memcpy(&datakey->dkctx->kms_providers,
               _mongocrypt_ctx_kms_providers(ctx),
               sizeof(_mongocrypt_opts_kms_providers_t));
        if (!datakey->dkctx->vtable.after_kms_credentials_provided(datakey->dkctx)) {
            _mongocrypt_status_copy_to(datakey->dkctx->status, ctx->status);
            return _mongocrypt_ctx_fail(ctx);
        }
    }

    return true;
}

static bool _start_kms_encrypt(mongocrypt_ctx_t *ctx) {
    _mongocrypt_ctx_rewrap_many_datakey_t *const rmdctx = (_mongocrypt_ctx_rewrap_many_datakey_t *)ctx;

    BSON_ASSERT_PARAM(ctx);

    /* Finish KMS requests for decryption if there were any. */
    if (ctx->state == MONGOCRYPT_CTX_NEED_KMS) {
        _mongocrypt_opts_kms_providers_t *const providers = _mongocrypt_ctx_kms_providers(ctx);

        if (!_mongocrypt_key_broker_kms_done(&ctx->kb, providers)) {
            _mongocrypt_status_copy_to(ctx->kb.status, ctx->status);
            return _mongocrypt_ctx_fail(ctx);
        }

        if (!_mongocrypt_ctx_state_from_key_broker(ctx)) {
            return _mongocrypt_ctx_fail(ctx);
        }

        /* Some providers may require multiple rounds of KMS requests. */
        if (ctx->state == MONGOCRYPT_CTX_NEED_KMS) {
            return true;
        }
    }

    /* All datakeys should be decrypted at this point. */
    BSON_ASSERT(ctx->state == MONGOCRYPT_CTX_READY);

    /* For all decrypted datakeys, initialize a corresponding datakey context. */
    {
        key_returned_t *key;

        /* Some decrypted keys may have been cached. */
        for (key = ctx->kb.keys_cached; key; key = key->next) {
            if (!_add_new_datakey(ctx, key)) {
                return _mongocrypt_ctx_fail(ctx);
            }
        }

        /* Remaining keys were decrypted via KMS requests. */
        for (key = ctx->kb.keys_returned; key; key = key->next) {
            if (!_add_new_datakey(ctx, key)) {
                return _mongocrypt_ctx_fail(ctx);
            }
        }
    }

    /* Prepare iterator used by _next_kms_ctx_encrypt. */
    rmdctx->datakeys_iter = rmdctx->datakeys;

    /* Skip datakeys that do not require a KMS request. */
    while (rmdctx->datakeys_iter && rmdctx->datakeys_iter->dkctx->state == MONGOCRYPT_CTX_READY) {
        rmdctx->datakeys_iter = rmdctx->datakeys_iter->next;
    }

    /* Skip to READY state if no KMS requests are required. */
    if (!rmdctx->datakeys_iter) {
        ctx->state = MONGOCRYPT_CTX_READY;
        ctx->vtable.finalize = _finalize;
        return true;
    }

    ctx->state = MONGOCRYPT_CTX_NEED_KMS;
    ctx->vtable.next_kms_ctx = _next_kms_ctx_encrypt;
    ctx->vtable.kms_done = _kms_done_encrypt;

    return true;
}

static bool _mongo_done_keys(mongocrypt_ctx_t *ctx) {
    BSON_ASSERT_PARAM(ctx);

    if (!_mongocrypt_key_broker_docs_done(&ctx->kb) || !_mongocrypt_ctx_state_from_key_broker(ctx)) {
        return _mongocrypt_ctx_fail(ctx);
    }

    /* No keys to rewrap, no work to be done. */
    if (!ctx->kb.key_requests) {
        ctx->state = MONGOCRYPT_CTX_DONE;
        return true;
    }

    /* No KMS required, skip straight to encryption. */
    if (ctx->state == MONGOCRYPT_CTX_READY) {
        return _start_kms_encrypt(ctx);
    }

    /* KMS requests needed to decrypt keys. */
    BSON_ASSERT(ctx->state == MONGOCRYPT_CTX_NEED_KMS);

    return true;
}

static bool _mongo_op_keys(mongocrypt_ctx_t *ctx, mongocrypt_binary_t *out) {
    _mongocrypt_ctx_rewrap_many_datakey_t *const rmdctx = (_mongocrypt_ctx_rewrap_many_datakey_t *)ctx;

    BSON_ASSERT_PARAM(ctx);
    BSON_ASSERT_PARAM(out);

    _mongocrypt_buffer_to_binary(&rmdctx->filter, out);

    return true;
}

static bool _kms_start_decrypt(mongocrypt_ctx_t *ctx) {
    BSON_ASSERT_PARAM(ctx);

    return _mongocrypt_key_broker_request_any(&ctx->kb) && _mongocrypt_ctx_state_from_key_broker(ctx);
}

static void _cleanup(mongocrypt_ctx_t *ctx) {
    _mongocrypt_ctx_rewrap_many_datakey_t *const rmdctx = (_mongocrypt_ctx_rewrap_many_datakey_t *)ctx;

    BSON_ASSERT_PARAM(ctx);

    _mongocrypt_buffer_cleanup(&rmdctx->results);

    while (rmdctx->datakeys) {
        _mongocrypt_ctx_rmd_datakey_t *result = rmdctx->datakeys;
        rmdctx->datakeys = result->next;

        mongocrypt_ctx_destroy(result->dkctx);
        bson_free(result);
    }

    _mongocrypt_kms_ctx_cleanup(&rmdctx->kms);
    _mongocrypt_buffer_cleanup(&rmdctx->filter);
}

bool mongocrypt_ctx_rewrap_many_datakey_init(mongocrypt_ctx_t *ctx, mongocrypt_binary_t *filter) {
    _mongocrypt_ctx_rewrap_many_datakey_t *const rmdctx = (_mongocrypt_ctx_rewrap_many_datakey_t *)ctx;

    if (!ctx) {
        return false;
    }

    if (!filter) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "filter must not be null");
    }

    {
        _mongocrypt_ctx_opts_spec_t opts_spec;
        memset(&opts_spec, 0, sizeof(opts_spec));

        opts_spec.kek = OPT_OPTIONAL;

        if (!_mongocrypt_ctx_init(ctx, &opts_spec)) {
            return _mongocrypt_ctx_fail(ctx);
        }
    }

    ctx->type = _MONGOCRYPT_TYPE_REWRAP_MANY_DATAKEY;
    ctx->state = MONGOCRYPT_CTX_NEED_MONGO_KEYS;
    ctx->vtable.cleanup = _cleanup;
    ctx->vtable.kms_done = _start_kms_encrypt;
    ctx->vtable.mongo_op_keys = _mongo_op_keys;
    ctx->vtable.mongo_done_keys = _mongo_done_keys;

    _mongocrypt_buffer_copy_from_binary(&rmdctx->filter, filter);

    /* Obtain KMS credentials for use during decryption and encryption. */
    if (_mongocrypt_needs_credentials(ctx->crypt)) {
        ctx->state = MONGOCRYPT_CTX_NEED_KMS_CREDENTIALS;
        ctx->vtable.after_kms_credentials_provided = _kms_start_decrypt;
        return true;
    }

    return _kms_start_decrypt(ctx);
}

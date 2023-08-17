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

#include <bson/bson.h>

#include "mongocrypt-ctx-private.h"
#include "mongocrypt-key-broker-private.h"

bool _mongocrypt_ctx_fail_w_msg(mongocrypt_ctx_t *ctx, const char *msg) {
    BSON_ASSERT_PARAM(ctx);
    BSON_ASSERT_PARAM(msg);

    _mongocrypt_set_error(ctx->status, MONGOCRYPT_STATUS_ERROR_CLIENT, MONGOCRYPT_GENERIC_ERROR_CODE, "%s", msg);
    return _mongocrypt_ctx_fail(ctx);
}

/* A failure status has already been set. */
bool _mongocrypt_ctx_fail(mongocrypt_ctx_t *ctx) {
    BSON_ASSERT_PARAM(ctx);

    if (mongocrypt_status_ok(ctx->status)) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "unexpected, failing but no error status set");
    }
    ctx->state = MONGOCRYPT_CTX_ERROR;
    return false;
}

static bool
_set_binary_opt(mongocrypt_ctx_t *ctx, mongocrypt_binary_t *binary, _mongocrypt_buffer_t *buf, bson_subtype_t subtype) {
    BSON_ASSERT_PARAM(ctx);

    if (ctx->state == MONGOCRYPT_CTX_ERROR) {
        return false;
    }

    if (!binary || !binary->data) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "option must be non-NULL");
    }

    if (!_mongocrypt_buffer_empty(buf)) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "option already set");
    }

    if (ctx->initialized) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "cannot set options after init");
    }

    if (subtype == BSON_SUBTYPE_UUID && binary->len != 16) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "expected 16 byte UUID");
    }

    _mongocrypt_buffer_copy_from_binary(buf, binary);
    buf->subtype = subtype;

    return true;
}

bool mongocrypt_ctx_setopt_key_id(mongocrypt_ctx_t *ctx, mongocrypt_binary_t *key_id) {
    if (!ctx) {
        return false;
    }

    if (ctx->crypt->log.trace_enabled && key_id && key_id->data) {
        char *key_id_val;
        /* this should never happen, so assert rather than return false */
        BSON_ASSERT(key_id->len <= INT_MAX);
        key_id_val = _mongocrypt_new_string_from_bytes(key_id->data, (int)key_id->len);
        _mongocrypt_log(&ctx->crypt->log,
                        MONGOCRYPT_LOG_LEVEL_TRACE,
                        "%s (%s=\"%s\")",
                        BSON_FUNC,
                        "key_id",
                        key_id_val);
        bson_free(key_id_val);
    }

    return _set_binary_opt(ctx, key_id, &ctx->opts.key_id, BSON_SUBTYPE_UUID);
}

bool mongocrypt_ctx_setopt_key_alt_name(mongocrypt_ctx_t *ctx, mongocrypt_binary_t *key_alt_name) {
    bson_t as_bson;
    bson_iter_t iter;
    _mongocrypt_key_alt_name_t *new_key_alt_name;
    const char *key;

    if (!ctx) {
        return false;
    }

    if (ctx->initialized) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "cannot set options after init");
    }

    if (ctx->state == MONGOCRYPT_CTX_ERROR) {
        return false;
    }

    if (!key_alt_name || !key_alt_name->data) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "option must be non-NULL");
    }

    if (!_mongocrypt_binary_to_bson(key_alt_name, &as_bson)) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "invalid keyAltName bson object");
    }

    if (!bson_iter_init(&iter, &as_bson) || !bson_iter_next(&iter)) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "invalid bson");
    }

    key = bson_iter_key(&iter);
    BSON_ASSERT(key);
    if (0 != strcmp(key, "keyAltName")) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "keyAltName must have field 'keyAltName'");
    }

    if (!BSON_ITER_HOLDS_UTF8(&iter)) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "keyAltName expected to be UTF8");
    }

    new_key_alt_name = _mongocrypt_key_alt_name_new(bson_iter_value(&iter));

    if (ctx->opts.key_alt_names && _mongocrypt_key_alt_name_intersects(ctx->opts.key_alt_names, new_key_alt_name)) {
        _mongocrypt_key_alt_name_destroy_all(new_key_alt_name);
        return _mongocrypt_ctx_fail_w_msg(ctx, "duplicate keyAltNames found");
    }
    new_key_alt_name->next = ctx->opts.key_alt_names;
    ctx->opts.key_alt_names = new_key_alt_name;

    if (bson_iter_next(&iter)) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "unrecognized field, only keyAltName expected");
    }

    return true;
}

bool mongocrypt_ctx_setopt_key_material(mongocrypt_ctx_t *ctx, mongocrypt_binary_t *key_material) {
    bson_t as_bson;
    bson_iter_t iter;
    const char *key;
    _mongocrypt_buffer_t buffer;

    if (!ctx) {
        return false;
    }

    if (ctx->initialized) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "cannot set options after init");
    }

    if (ctx->opts.key_material.owned) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "keyMaterial already set");
    }

    if (ctx->state == MONGOCRYPT_CTX_ERROR) {
        return false;
    }

    if (!key_material || !key_material->data) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "option must be non-NULL");
    }

    if (!_mongocrypt_binary_to_bson(key_material, &as_bson)) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "invalid keyMaterial bson object");
    }

    /* TODO: use _mongocrypt_parse_required_binary once MONGOCRYPT-380 is
     * resolved.*/
    if (!bson_iter_init(&iter, &as_bson) || !bson_iter_next(&iter)) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "invalid bson");
    }

    key = bson_iter_key(&iter);
    BSON_ASSERT(key);
    if (0 != strcmp(key, "keyMaterial")) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "keyMaterial must have field 'keyMaterial'");
    }

    if (!_mongocrypt_buffer_from_binary_iter(&buffer, &iter)) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "keyMaterial must be binary data");
    }

    if (buffer.len != MONGOCRYPT_KEY_LEN) {
        _mongocrypt_set_error(ctx->status,
                              MONGOCRYPT_STATUS_ERROR_CLIENT,
                              MONGOCRYPT_GENERIC_ERROR_CODE,
                              "keyMaterial should have length %d, but has length %" PRIu32,
                              MONGOCRYPT_KEY_LEN,
                              buffer.len);
        return _mongocrypt_ctx_fail(ctx);
    }

    _mongocrypt_buffer_steal(&ctx->opts.key_material, &buffer);

    if (bson_iter_next(&iter)) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "unrecognized field, only keyMaterial expected");
    }

    return true;
}

bool mongocrypt_ctx_setopt_algorithm(mongocrypt_ctx_t *ctx, const char *algorithm, int len) {
    if (!ctx) {
        return false;
    }

    if (ctx->initialized) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "cannot set options after init");
    }

    if (ctx->state == MONGOCRYPT_CTX_ERROR) {
        return false;
    }

    if (ctx->opts.algorithm != MONGOCRYPT_ENCRYPTION_ALGORITHM_NONE || ctx->opts.index_type.set) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "already set algorithm");
    }

    if (len < -1) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "invalid algorithm length");
    }

    if (!algorithm) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "passed null algorithm");
    }

    const size_t calculated_len = len == -1 ? strlen(algorithm) : (size_t)len;
    if (ctx->crypt->log.trace_enabled) {
        _mongocrypt_log(&ctx->crypt->log,
                        MONGOCRYPT_LOG_LEVEL_TRACE,
                        "%s (%s=\"%.*s\")",
                        BSON_FUNC,
                        "algorithm",
                        calculated_len <= (size_t)INT_MAX ? (int)calculated_len : INT_MAX,
                        algorithm);
    }

    mstr_view algo_str = mstrv_view_data(algorithm, calculated_len);
    if (mstr_eq_ignore_case(algo_str, mstrv_lit(MONGOCRYPT_ALGORITHM_DETERMINISTIC_STR))) {
        ctx->opts.algorithm = MONGOCRYPT_ENCRYPTION_ALGORITHM_DETERMINISTIC;
    } else if (mstr_eq_ignore_case(algo_str, mstrv_lit(MONGOCRYPT_ALGORITHM_RANDOM_STR))) {
        ctx->opts.algorithm = MONGOCRYPT_ENCRYPTION_ALGORITHM_RANDOM;
    } else if (mstr_eq_ignore_case(algo_str, mstrv_lit(MONGOCRYPT_ALGORITHM_INDEXED_STR))) {
        ctx->opts.index_type.value = MONGOCRYPT_INDEX_TYPE_EQUALITY;
        ctx->opts.index_type.set = true;
    } else if (mstr_eq_ignore_case(algo_str, mstrv_lit(MONGOCRYPT_ALGORITHM_UNINDEXED_STR))) {
        ctx->opts.index_type.value = MONGOCRYPT_INDEX_TYPE_NONE;
        ctx->opts.index_type.set = true;
    } else if (mstr_eq_ignore_case(algo_str, mstrv_lit(MONGOCRYPT_ALGORITHM_RANGEPREVIEW_STR))) {
        ctx->opts.index_type.value = MONGOCRYPT_INDEX_TYPE_RANGEPREVIEW;
        ctx->opts.index_type.set = true;
    } else {
        char *error = bson_strdup_printf("unsupported algorithm string \"%.*s\"",
                                         algo_str.len <= (size_t)INT_MAX ? (int)algo_str.len : INT_MAX,
                                         algo_str.data);
        _mongocrypt_ctx_fail_w_msg(ctx, error);
        bson_free(error);
        return false;
    }

    return true;
}

mongocrypt_ctx_t *mongocrypt_ctx_new(mongocrypt_t *crypt) {
    mongocrypt_ctx_t *ctx;
    size_t ctx_size;

    if (!crypt) {
        return NULL;
    }
    if (!crypt->initialized) {
        mongocrypt_status_t *status;

        status = crypt->status;
        CLIENT_ERR("cannot create context from uninitialized crypt");
        return NULL;
    }
    ctx_size = sizeof(_mongocrypt_ctx_encrypt_t);
    if (sizeof(_mongocrypt_ctx_decrypt_t) > ctx_size) {
        ctx_size = sizeof(_mongocrypt_ctx_decrypt_t);
    }
    if (sizeof(_mongocrypt_ctx_datakey_t) > ctx_size) {
        ctx_size = sizeof(_mongocrypt_ctx_datakey_t);
    }
    ctx = bson_malloc0(ctx_size);
    BSON_ASSERT(ctx);

    ctx->crypt = crypt;
    ctx->status = mongocrypt_status_new();
    ctx->opts.algorithm = MONGOCRYPT_ENCRYPTION_ALGORITHM_NONE;
    ctx->state = MONGOCRYPT_CTX_DONE;
    return ctx;
}

#define CHECK_AND_CALL(fn, ...)                                                                                        \
    do {                                                                                                               \
        if (!ctx->vtable.fn) {                                                                                         \
            return _mongocrypt_ctx_fail_w_msg(ctx, "not applicable to context");                                       \
        }                                                                                                              \
        return ctx->vtable.fn(__VA_ARGS__);                                                                            \
    } while (0)

/* Common to both encrypt and decrypt context. */
static bool _mongo_op_keys(mongocrypt_ctx_t *ctx, mongocrypt_binary_t *out) {
    BSON_ASSERT_PARAM(ctx);
    BSON_ASSERT_PARAM(out);

    /* Construct the find filter to fetch keys. */
    if (!_mongocrypt_key_broker_filter(&ctx->kb, out)) {
        BSON_ASSERT(!_mongocrypt_key_broker_status(&ctx->kb, ctx->status));
        return _mongocrypt_ctx_fail(ctx);
    }
    return true;
}

static bool _mongo_feed_keys(mongocrypt_ctx_t *ctx, mongocrypt_binary_t *in) {
    _mongocrypt_buffer_t buf;

    BSON_ASSERT_PARAM(ctx);
    BSON_ASSERT_PARAM(in);

    _mongocrypt_buffer_from_binary(&buf, in);
    if (!_mongocrypt_key_broker_add_doc(&ctx->kb, _mongocrypt_ctx_kms_providers(ctx), &buf)) {
        BSON_ASSERT(!_mongocrypt_key_broker_status(&ctx->kb, ctx->status));
        return _mongocrypt_ctx_fail(ctx);
    }
    return true;
}

static bool _mongo_done_keys(mongocrypt_ctx_t *ctx) {
    BSON_ASSERT_PARAM(ctx);

    (void)_mongocrypt_key_broker_docs_done(&ctx->kb);
    return _mongocrypt_ctx_state_from_key_broker(ctx);
}

static mongocrypt_kms_ctx_t *_next_kms_ctx(mongocrypt_ctx_t *ctx) {
    BSON_ASSERT_PARAM(ctx);

    return _mongocrypt_key_broker_next_kms(&ctx->kb);
}

static bool _kms_done(mongocrypt_ctx_t *ctx) {
    _mongocrypt_opts_kms_providers_t *kms_providers;

    BSON_ASSERT_PARAM(ctx);

    kms_providers = _mongocrypt_ctx_kms_providers(ctx);

    if (!_mongocrypt_key_broker_kms_done(&ctx->kb, kms_providers)) {
        BSON_ASSERT(!_mongocrypt_key_broker_status(&ctx->kb, ctx->status));
        return _mongocrypt_ctx_fail(ctx);
    }
    return _mongocrypt_ctx_state_from_key_broker(ctx);
}

bool mongocrypt_ctx_mongo_op(mongocrypt_ctx_t *ctx, mongocrypt_binary_t *out) {
    if (!ctx) {
        return false;
    }
    if (!ctx->initialized) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "ctx NULL or uninitialized");
    }

    if (!out) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "invalid NULL output");
    }

    switch (ctx->state) {
    case MONGOCRYPT_CTX_NEED_MONGO_COLLINFO: CHECK_AND_CALL(mongo_op_collinfo, ctx, out);
    case MONGOCRYPT_CTX_NEED_MONGO_MARKINGS: CHECK_AND_CALL(mongo_op_markings, ctx, out);
    case MONGOCRYPT_CTX_NEED_MONGO_KEYS: CHECK_AND_CALL(mongo_op_keys, ctx, out);
    case MONGOCRYPT_CTX_ERROR: return false;
    case MONGOCRYPT_CTX_DONE:
    case MONGOCRYPT_CTX_NEED_KMS_CREDENTIALS:
    case MONGOCRYPT_CTX_NEED_KMS:
    case MONGOCRYPT_CTX_READY:
    default: return _mongocrypt_ctx_fail_w_msg(ctx, "wrong state");
    }
}

bool mongocrypt_ctx_mongo_feed(mongocrypt_ctx_t *ctx, mongocrypt_binary_t *in) {
    if (!ctx) {
        return false;
    }
    if (!ctx->initialized) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "ctx NULL or uninitialized");
    }

    if (!in) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "invalid NULL input");
    }

    if (ctx->crypt->log.trace_enabled) {
        char *in_val;

        in_val = _mongocrypt_new_json_string_from_binary(in);
        _mongocrypt_log(&ctx->crypt->log, MONGOCRYPT_LOG_LEVEL_TRACE, "%s (%s=\"%s\")", BSON_FUNC, "in", in_val);
        bson_free(in_val);
    }

    switch (ctx->state) {
    case MONGOCRYPT_CTX_NEED_MONGO_COLLINFO: CHECK_AND_CALL(mongo_feed_collinfo, ctx, in);
    case MONGOCRYPT_CTX_NEED_MONGO_MARKINGS: CHECK_AND_CALL(mongo_feed_markings, ctx, in);
    case MONGOCRYPT_CTX_NEED_MONGO_KEYS: CHECK_AND_CALL(mongo_feed_keys, ctx, in);
    case MONGOCRYPT_CTX_ERROR: return false;
    case MONGOCRYPT_CTX_DONE:
    case MONGOCRYPT_CTX_NEED_KMS_CREDENTIALS:
    case MONGOCRYPT_CTX_NEED_KMS:
    case MONGOCRYPT_CTX_READY:
    default: return _mongocrypt_ctx_fail_w_msg(ctx, "wrong state");
    }
}

bool mongocrypt_ctx_mongo_done(mongocrypt_ctx_t *ctx) {
    if (!ctx) {
        return false;
    }
    if (!ctx->initialized) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "ctx NULL or uninitialized");
    }

    switch (ctx->state) {
    case MONGOCRYPT_CTX_NEED_MONGO_COLLINFO: CHECK_AND_CALL(mongo_done_collinfo, ctx);
    case MONGOCRYPT_CTX_NEED_MONGO_MARKINGS: CHECK_AND_CALL(mongo_done_markings, ctx);
    case MONGOCRYPT_CTX_NEED_MONGO_KEYS: CHECK_AND_CALL(mongo_done_keys, ctx);
    case MONGOCRYPT_CTX_ERROR: return false;
    case MONGOCRYPT_CTX_DONE:
    case MONGOCRYPT_CTX_NEED_KMS_CREDENTIALS:
    case MONGOCRYPT_CTX_NEED_KMS:
    case MONGOCRYPT_CTX_READY:
    default: return _mongocrypt_ctx_fail_w_msg(ctx, "wrong state");
    }
}

mongocrypt_ctx_state_t mongocrypt_ctx_state(mongocrypt_ctx_t *ctx) {
    if (!ctx) {
        return MONGOCRYPT_CTX_ERROR;
    }
    if (!ctx->initialized) {
        _mongocrypt_ctx_fail_w_msg(ctx, "ctx NULL or uninitialized");
        return MONGOCRYPT_CTX_ERROR;
    }

    return ctx->state;
}

mongocrypt_kms_ctx_t *mongocrypt_ctx_next_kms_ctx(mongocrypt_ctx_t *ctx) {
    if (!ctx) {
        return NULL;
    }
    if (!ctx->initialized) {
        _mongocrypt_ctx_fail_w_msg(ctx, "ctx NULL or uninitialized");
        return NULL;
    }

    if (!ctx->vtable.next_kms_ctx) {
        _mongocrypt_ctx_fail_w_msg(ctx, "not applicable to context");
        return NULL;
    }

    switch (ctx->state) {
    case MONGOCRYPT_CTX_NEED_KMS: return ctx->vtable.next_kms_ctx(ctx);
    case MONGOCRYPT_CTX_ERROR: return NULL;
    case MONGOCRYPT_CTX_DONE:
    case MONGOCRYPT_CTX_NEED_KMS_CREDENTIALS:
    case MONGOCRYPT_CTX_NEED_MONGO_COLLINFO:
    case MONGOCRYPT_CTX_NEED_MONGO_KEYS:
    case MONGOCRYPT_CTX_NEED_MONGO_MARKINGS:
    case MONGOCRYPT_CTX_READY:
    default: _mongocrypt_ctx_fail_w_msg(ctx, "wrong state"); return NULL;
    }
}

bool mongocrypt_ctx_provide_kms_providers(mongocrypt_ctx_t *ctx, mongocrypt_binary_t *kms_providers_definition) {
    if (!ctx) {
        return false;
    }

    if (!ctx->initialized) {
        _mongocrypt_ctx_fail_w_msg(ctx, "ctx NULL or uninitialized");
        return false;
    }

    if (ctx->state != MONGOCRYPT_CTX_NEED_KMS_CREDENTIALS) {
        _mongocrypt_ctx_fail_w_msg(ctx, "wrong state");
        return false;
    }

    if (!kms_providers_definition) {
        _mongocrypt_ctx_fail_w_msg(ctx, "KMS provider credential mapping not provided");
        return false;
    }

    if (!_mongocrypt_parse_kms_providers(kms_providers_definition,
                                         &ctx->per_ctx_kms_providers,
                                         ctx->status,
                                         &ctx->crypt->log)) {
        return _mongocrypt_ctx_fail(ctx);
    }

    if (!_mongocrypt_opts_kms_providers_validate(&ctx->crypt->opts, &ctx->per_ctx_kms_providers, ctx->status)) {
        /* Remove the parsed KMS providers if they are invalid */
        _mongocrypt_opts_kms_providers_cleanup(&ctx->per_ctx_kms_providers);
        memset(&ctx->per_ctx_kms_providers, 0, sizeof(ctx->per_ctx_kms_providers));
        return _mongocrypt_ctx_fail(ctx);
    }

    memcpy(&ctx->kms_providers, &ctx->crypt->opts.kms_providers, sizeof(_mongocrypt_opts_kms_providers_t));
    _mongocrypt_opts_merge_kms_providers(&ctx->kms_providers, &ctx->per_ctx_kms_providers);

    ctx->state = ctx->kb.state == KB_ADDING_DOCS ? MONGOCRYPT_CTX_NEED_MONGO_KEYS : MONGOCRYPT_CTX_NEED_KMS;
    if (ctx->vtable.after_kms_credentials_provided) {
        return ctx->vtable.after_kms_credentials_provided(ctx);
    }
    return true;
}

bool mongocrypt_ctx_kms_done(mongocrypt_ctx_t *ctx) {
    if (!ctx) {
        return false;
    }
    if (!ctx->initialized) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "ctx NULL or uninitialized");
    }

    if (!ctx->vtable.kms_done) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "not applicable to context");
    }

    switch (ctx->state) {
    case MONGOCRYPT_CTX_NEED_KMS: return ctx->vtable.kms_done(ctx);
    case MONGOCRYPT_CTX_ERROR: return false;
    case MONGOCRYPT_CTX_DONE:
    case MONGOCRYPT_CTX_NEED_KMS_CREDENTIALS:
    case MONGOCRYPT_CTX_NEED_MONGO_COLLINFO:
    case MONGOCRYPT_CTX_NEED_MONGO_KEYS:
    case MONGOCRYPT_CTX_NEED_MONGO_MARKINGS:
    case MONGOCRYPT_CTX_READY:
    default: return _mongocrypt_ctx_fail_w_msg(ctx, "wrong state");
    }
}

bool mongocrypt_ctx_finalize(mongocrypt_ctx_t *ctx, mongocrypt_binary_t *out) {
    if (!ctx) {
        return false;
    }
    if (!ctx->initialized) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "ctx NULL or uninitialized");
    }

    if (!out) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "invalid NULL output");
    }

    if (!ctx->vtable.finalize) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "not applicable to context");
    }

    switch (ctx->state) {
    case MONGOCRYPT_CTX_READY: return ctx->vtable.finalize(ctx, out);
    case MONGOCRYPT_CTX_ERROR: return false;
    case MONGOCRYPT_CTX_DONE:
    case MONGOCRYPT_CTX_NEED_KMS_CREDENTIALS:
    case MONGOCRYPT_CTX_NEED_KMS:
    case MONGOCRYPT_CTX_NEED_MONGO_COLLINFO:
    case MONGOCRYPT_CTX_NEED_MONGO_KEYS:
    case MONGOCRYPT_CTX_NEED_MONGO_MARKINGS:
    default: return _mongocrypt_ctx_fail_w_msg(ctx, "wrong state");
    }
}

bool mongocrypt_ctx_status(mongocrypt_ctx_t *ctx, mongocrypt_status_t *out) {
    if (!ctx) {
        return false;
    }

    if (!out) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "invalid NULL output");
    }

    if (!mongocrypt_status_ok(ctx->status)) {
        _mongocrypt_status_copy_to(ctx->status, out);
        return false;
    }
    _mongocrypt_status_reset(out);
    return true;
}

void mongocrypt_ctx_destroy(mongocrypt_ctx_t *ctx) {
    if (!ctx) {
        return;
    }

    if (ctx->vtable.cleanup) {
        ctx->vtable.cleanup(ctx);
    }

    mc_RangeOpts_cleanup(&ctx->opts.rangeopts.value);
    _mongocrypt_opts_kms_providers_cleanup(&ctx->per_ctx_kms_providers);
    _mongocrypt_kek_cleanup(&ctx->opts.kek);
    mongocrypt_status_destroy(ctx->status);
    _mongocrypt_key_broker_cleanup(&ctx->kb);
    _mongocrypt_buffer_cleanup(&ctx->opts.key_material);
    _mongocrypt_key_alt_name_destroy_all(ctx->opts.key_alt_names);
    _mongocrypt_buffer_cleanup(&ctx->opts.key_id);
    _mongocrypt_buffer_cleanup(&ctx->opts.index_key_id);
    bson_free(ctx);
    return;
}

bool mongocrypt_ctx_setopt_masterkey_aws(mongocrypt_ctx_t *ctx,
                                         const char *region,
                                         int32_t region_len,
                                         const char *cmk,
                                         int32_t cmk_len) {
    mongocrypt_binary_t *bin;
    bson_t as_bson;
    bool ret;
    char *temp = NULL;

    if (!ctx) {
        return false;
    }
    if (ctx->initialized) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "cannot set options after init");
    }

    if (ctx->state == MONGOCRYPT_CTX_ERROR) {
        return false;
    }

    if (ctx->opts.kek.kms_provider != MONGOCRYPT_KMS_PROVIDER_AWS
        && ctx->opts.kek.kms_provider != MONGOCRYPT_KMS_PROVIDER_NONE) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "master key already set");
    }

    if (ctx->opts.kek.kms_provider == MONGOCRYPT_KMS_PROVIDER_AWS && ctx->opts.kek.provider.aws.region) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "master key already set");
    }

    if (!_mongocrypt_validate_and_copy_string(region, region_len, &temp) || region_len == 0) {
        bson_free(temp);
        return _mongocrypt_ctx_fail_w_msg(ctx, "invalid region");
    }
    bson_free(temp);

    temp = NULL;
    if (!_mongocrypt_validate_and_copy_string(cmk, cmk_len, &temp) || cmk_len == 0) {
        bson_free(temp);
        return _mongocrypt_ctx_fail_w_msg(ctx, "invalid cmk");
    }
    bson_free(temp);

    bson_init(&as_bson);
    bson_append_utf8(&as_bson, MONGOCRYPT_STR_AND_LEN("provider"), MONGOCRYPT_STR_AND_LEN("aws"));
    BSON_ASSERT(region_len <= INT_MAX);
    bson_append_utf8(&as_bson, MONGOCRYPT_STR_AND_LEN("region"), region, region_len);
    BSON_ASSERT(cmk_len <= INT_MAX);
    bson_append_utf8(&as_bson, MONGOCRYPT_STR_AND_LEN("key"), cmk, cmk_len);
    bin = mongocrypt_binary_new_from_data((uint8_t *)bson_get_data(&as_bson), as_bson.len);

    ret = mongocrypt_ctx_setopt_key_encryption_key(ctx, bin);
    mongocrypt_binary_destroy(bin);
    bson_destroy(&as_bson);

    if (ctx->crypt->log.trace_enabled) {
        _mongocrypt_log(&ctx->crypt->log,
                        MONGOCRYPT_LOG_LEVEL_TRACE,
                        "%s (%s=\"%s\", %s=%d, %s=\"%s\", %s=%d)",
                        BSON_FUNC,
                        "region",
                        ctx->opts.kek.provider.aws.region,
                        "region_len",
                        region_len,
                        "cmk",
                        ctx->opts.kek.provider.aws.cmk,
                        "cmk_len",
                        cmk_len);
    }

    return ret;
}

bool mongocrypt_ctx_setopt_masterkey_local(mongocrypt_ctx_t *ctx) {
    if (!ctx) {
        return false;
    }
    if (ctx->initialized) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "cannot set options after init");
    }

    if (ctx->state == MONGOCRYPT_CTX_ERROR) {
        return false;
    }

    if (ctx->opts.kek.kms_provider) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "master key already set");
    }

    ctx->opts.kek.kms_provider = MONGOCRYPT_KMS_PROVIDER_LOCAL;
    return true;
}

bool _mongocrypt_ctx_init(mongocrypt_ctx_t *ctx, _mongocrypt_ctx_opts_spec_t *opts_spec) {
    bool has_id = false, has_alt_name = false, has_multiple_alt_names = false;

    BSON_ASSERT_PARAM(ctx);
    BSON_ASSERT_PARAM(opts_spec);

    // This condition is guarded in setopt_algorithm:
    BSON_ASSERT(!(ctx->opts.index_type.set && ctx->opts.algorithm != MONGOCRYPT_ENCRYPTION_ALGORITHM_NONE)
                && "Both an encryption algorithm and an index_type were set.");

    if (ctx->initialized) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "cannot double initialize");
    }
    ctx->initialized = true;

    if (ctx->state == MONGOCRYPT_CTX_ERROR) {
        return false;
    }
    /* Set some default functions */
    ctx->vtable.mongo_op_keys = _mongo_op_keys;
    ctx->vtable.mongo_feed_keys = _mongo_feed_keys;
    ctx->vtable.mongo_done_keys = _mongo_done_keys;
    ctx->vtable.next_kms_ctx = _next_kms_ctx;
    ctx->vtable.kms_done = _kms_done;

    /* Check that required options are included and prohibited options are
     * not.
     */

    if (opts_spec->kek == OPT_REQUIRED) {
        if (!ctx->opts.kek.kms_provider) {
            return _mongocrypt_ctx_fail_w_msg(ctx, "master key required");
        }
        if (!ctx->crypt->opts.use_need_kms_credentials_state
            && !((int)ctx->opts.kek.kms_provider & _mongocrypt_ctx_kms_providers(ctx)->configured_providers)) {
            return _mongocrypt_ctx_fail_w_msg(ctx, "requested kms provider not configured");
        }
    }

    if (opts_spec->kek == OPT_PROHIBITED && ctx->opts.kek.kms_provider) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "master key prohibited");
    }

    /* Check that the kms provider required by the datakey is configured.  */
    if (ctx->opts.kek.kms_provider) {
        if (!((ctx->crypt->opts.kms_providers.need_credentials | ctx->crypt->opts.kms_providers.configured_providers)
              & (int)ctx->opts.kek.kms_provider)) {
            return _mongocrypt_ctx_fail_w_msg(ctx, "kms provider required by datakey is not configured");
        }
    }

    /* Special case. key_descriptor applies to explicit encryption. It must be
     * either a key id or *one* key alt name, but not both.
     * key_alt_names applies to creating a data key. It may be one or multiple
     * key alt names.
     */
    has_id = !_mongocrypt_buffer_empty(&ctx->opts.key_id);
    has_alt_name = !!(ctx->opts.key_alt_names);
    has_multiple_alt_names = has_alt_name && !!(ctx->opts.key_alt_names->next);

    if (opts_spec->key_descriptor == OPT_REQUIRED) {
        if (!has_id && !has_alt_name) {
            return _mongocrypt_ctx_fail_w_msg(ctx, "either key id or key alt name required");
        }

        if (has_id && has_alt_name) {
            return _mongocrypt_ctx_fail_w_msg(ctx, "cannot have both key id and key alt name");
        }

        if (has_multiple_alt_names) {
            return _mongocrypt_ctx_fail_w_msg(ctx, "must not specify multiple key alt names");
        }
    }

    if (opts_spec->key_descriptor == OPT_PROHIBITED) {
        /* still okay if key_alt_names are allowed and only alt names were
         * specified. */
        if ((opts_spec->key_alt_names == OPT_PROHIBITED && has_alt_name) || has_id) {
            return _mongocrypt_ctx_fail_w_msg(ctx, "key id and alt name prohibited");
        }
    }

    if (opts_spec->key_material == OPT_PROHIBITED && ctx->opts.key_material.owned) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "key material prohibited");
    }

    if (opts_spec->algorithm == OPT_REQUIRED && ctx->opts.algorithm == MONGOCRYPT_ENCRYPTION_ALGORITHM_NONE) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "algorithm required");
    }

    if (opts_spec->algorithm == OPT_PROHIBITED && ctx->opts.algorithm != MONGOCRYPT_ENCRYPTION_ALGORITHM_NONE) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "algorithm prohibited");
    }

    if (opts_spec->rangeopts == OPT_PROHIBITED && ctx->opts.rangeopts.set) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "range opts are prohibited on this context");
    }

    _mongocrypt_key_broker_init(&ctx->kb, ctx->crypt);
    return true;
}

bool _mongocrypt_ctx_state_from_key_broker(mongocrypt_ctx_t *ctx) {
    _mongocrypt_key_broker_t *kb;
    mongocrypt_status_t *status;
    mongocrypt_ctx_state_t new_state = MONGOCRYPT_CTX_ERROR;
    bool ret = false;

    BSON_ASSERT_PARAM(ctx);

    status = ctx->status;
    kb = &ctx->kb;

    if (ctx->state == MONGOCRYPT_CTX_ERROR) {
        return false;
    }

    switch (kb->state) {
    case KB_ERROR:
        _mongocrypt_status_copy_to(kb->status, status);
        new_state = MONGOCRYPT_CTX_ERROR;
        ret = false;
        break;
    case KB_ADDING_DOCS:
        /* Encrypted keys need KMS, which need to be provided before
         * adding docs. */
        if (_mongocrypt_needs_credentials(ctx->crypt)) {
            new_state = MONGOCRYPT_CTX_NEED_KMS_CREDENTIALS;
        } else {
            /* Require key documents from driver. */
            new_state = MONGOCRYPT_CTX_NEED_MONGO_KEYS;
        }
        ret = true;
        break;
    case KB_ADDING_DOCS_ANY:
        /* Assume KMS credentials have been provided. */
        new_state = MONGOCRYPT_CTX_NEED_MONGO_KEYS;
        ret = true;
        break;
    case KB_AUTHENTICATING:
    case KB_DECRYPTING_KEY_MATERIAL:
        new_state = MONGOCRYPT_CTX_NEED_KMS;
        ret = true;
        break;
    case KB_DONE:
        new_state = MONGOCRYPT_CTX_READY;
        if (kb->key_requests == NULL) {
            /* No key requests were ever added. */
            ctx->nothing_to_do = true; /* nothing to encrypt/decrypt */
        }
        ret = true;
        break;
    /* As currently implemented, we do not expect to ever be in KB_REQUESTING
     * or KB_REQUESTING_ANY state when calling this function. */
    case KB_REQUESTING:
    default:
        CLIENT_ERR("key broker in unexpected state");
        new_state = MONGOCRYPT_CTX_ERROR;
        ret = false;
        break;
    }

    if (new_state != ctx->state) {
        ctx->state = new_state;
    }

    return ret;
}

bool mongocrypt_ctx_setopt_masterkey_aws_endpoint(mongocrypt_ctx_t *ctx, const char *endpoint, int32_t endpoint_len) {
    if (!ctx) {
        return false;
    }

    if (ctx->initialized) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "cannot set options after init");
    }

    if (ctx->state == MONGOCRYPT_CTX_ERROR) {
        return false;
    }

    if (ctx->opts.kek.kms_provider != MONGOCRYPT_KMS_PROVIDER_AWS
        && ctx->opts.kek.kms_provider != MONGOCRYPT_KMS_PROVIDER_NONE) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "endpoint prohibited");
    }

    ctx->opts.kek.kms_provider = MONGOCRYPT_KMS_PROVIDER_AWS;

    if (ctx->opts.kek.provider.aws.endpoint) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "already set masterkey endpoint");
    }

    ctx->opts.kek.provider.aws.endpoint =
        _mongocrypt_endpoint_new(endpoint, endpoint_len, NULL /* opts */, ctx->status);
    if (!ctx->opts.kek.provider.aws.endpoint) {
        return _mongocrypt_ctx_fail(ctx);
    }

    return true;
}

bool mongocrypt_ctx_setopt_key_encryption_key(mongocrypt_ctx_t *ctx, mongocrypt_binary_t *bin) {
    bson_t as_bson;

    if (!ctx) {
        return false;
    }

    if (ctx->initialized) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "cannot set options after init");
    }

    if (ctx->state == MONGOCRYPT_CTX_ERROR) {
        return false;
    }

    if (ctx->opts.kek.kms_provider) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "key encryption key already set");
    }

    if (!bin) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "invalid NULL key encryption key document");
    }

    if (!_mongocrypt_binary_to_bson(bin, &as_bson)) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "invalid BSON");
    }

    if (!_mongocrypt_kek_parse_owned(&as_bson, &ctx->opts.kek, ctx->status)) {
        return _mongocrypt_ctx_fail(ctx);
    }

    if (ctx->crypt->log.trace_enabled) {
        char *bin_str = bson_as_canonical_extended_json(&as_bson, NULL);
        _mongocrypt_log(&ctx->crypt->log, MONGOCRYPT_LOG_LEVEL_TRACE, "%s (%s=\"%s\")", BSON_FUNC, "bin", bin_str);
        bson_free(bin_str);
    }

    return true;
}

_mongocrypt_opts_kms_providers_t *_mongocrypt_ctx_kms_providers(mongocrypt_ctx_t *ctx) {
    BSON_ASSERT_PARAM(ctx);

    return ctx->kms_providers.configured_providers ? &ctx->kms_providers : &ctx->crypt->opts.kms_providers;
}

bool mongocrypt_ctx_setopt_contention_factor(mongocrypt_ctx_t *ctx, int64_t contention_factor) {
    if (!ctx) {
        return false;
    }
    ctx->opts.contention_factor.value = contention_factor;
    ctx->opts.contention_factor.set = true;
    return true;
}

bool mongocrypt_ctx_setopt_index_key_id(mongocrypt_ctx_t *ctx, mongocrypt_binary_t *key_id) {
    if (!ctx) {
        return false;
    }

    return _set_binary_opt(ctx, key_id, &ctx->opts.index_key_id, BSON_SUBTYPE_UUID);
}

bool mongocrypt_ctx_setopt_query_type(mongocrypt_ctx_t *ctx, const char *query_type, int len) {
    if (!ctx) {
        return false;
    }

    if (ctx->initialized) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "Cannot set options after init");
    }
    if (ctx->state == MONGOCRYPT_CTX_ERROR) {
        return false;
    }
    if (len < -1) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "Invalid query_type string length");
    }
    if (!query_type) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "Invalid null query_type string");
    }

    const size_t calc_len = len == -1 ? strlen(query_type) : (size_t)len;
    mstr_view qt_str = mstrv_view_data(query_type, calc_len);
    if (mstr_eq_ignore_case(qt_str, mstrv_lit(MONGOCRYPT_QUERY_TYPE_EQUALITY_STR))) {
        ctx->opts.query_type.value = MONGOCRYPT_QUERY_TYPE_EQUALITY;
        ctx->opts.query_type.set = true;
    } else if (mstr_eq_ignore_case(qt_str, mstrv_lit(MONGOCRYPT_QUERY_TYPE_RANGEPREVIEW_STR))) {
        ctx->opts.query_type.value = MONGOCRYPT_QUERY_TYPE_RANGEPREVIEW;
        ctx->opts.query_type.set = true;
    } else {
        /* don't check if qt_str.len fits in int; we want the diagnostic output */
        char *error = bson_strdup_printf("Unsupported query_type \"%.*s\"",
                                         qt_str.len <= (size_t)INT_MAX ? (int)qt_str.len : INT_MAX,
                                         qt_str.data);
        _mongocrypt_ctx_fail_w_msg(ctx, error);
        bson_free(error);
        return false;
    }
    return true;
}

const char *_mongocrypt_index_type_to_string(mongocrypt_index_type_t val) {
    switch (val) {
    case MONGOCRYPT_INDEX_TYPE_NONE: return "None";
    case MONGOCRYPT_INDEX_TYPE_EQUALITY: return "Equality";
    case MONGOCRYPT_INDEX_TYPE_RANGEPREVIEW: return "RangePreview";
    default: return "Unknown";
    }
}

const char *_mongocrypt_query_type_to_string(mongocrypt_query_type_t val) {
    switch (val) {
    case MONGOCRYPT_QUERY_TYPE_EQUALITY: return "Equality";
    case MONGOCRYPT_QUERY_TYPE_RANGEPREVIEW: return "RangePreview";
    default: return "Unknown";
    }
}

bool mongocrypt_ctx_setopt_algorithm_range(mongocrypt_ctx_t *ctx, mongocrypt_binary_t *opts) {
    bson_t as_bson;

    if (!ctx) {
        return false;
    }

    if (ctx->initialized) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "cannot set options after init");
    }

    if (ctx->state == MONGOCRYPT_CTX_ERROR) {
        return false;
    }

    if (ctx->opts.rangeopts.set) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "RangeOpts already set");
    }

    if (!_mongocrypt_binary_to_bson(opts, &as_bson)) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "invalid BSON");
    }

    if (!mc_RangeOpts_parse(&ctx->opts.rangeopts.value, &as_bson, ctx->status)) {
        return _mongocrypt_ctx_fail(ctx);
    }

    ctx->opts.rangeopts.set = true;
    return true;
}

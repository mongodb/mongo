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

#include "mc-efc-private.h"
#include "mc-fle2-rfds-private.h"
#include "mc-tokens-private.h"
#include "mongocrypt-ciphertext-private.h"
#include "mongocrypt-crypto-private.h"
#include "mongocrypt-ctx-private.h"
#include "mongocrypt-key-broker-private.h"
#include "mongocrypt-marking-private.h"
#include "mongocrypt-traverse-util-private.h"
#include "mongocrypt-util-private.h" // mc_iter_document_as_bson
#include "mongocrypt.h"

/* _fle2_append_encryptedFieldConfig copies encryptedFieldConfig and applies
 * default state collection names for escCollection, eccCollection, and
 * ecocCollection if required. */
static bool _fle2_append_encryptedFieldConfig(const mongocrypt_ctx_t *ctx,
                                              bson_t *dst,
                                              bson_t *encryptedFieldConfig,
                                              const char *target_coll,
                                              mongocrypt_status_t *status) {
    bson_iter_t iter;
    bool has_escCollection = false;
    bool has_eccCollection = false;
    bool has_ecocCollection = false;

    BSON_ASSERT_PARAM(dst);
    BSON_ASSERT_PARAM(encryptedFieldConfig);
    BSON_ASSERT_PARAM(target_coll);

    if (!bson_iter_init(&iter, encryptedFieldConfig)) {
        CLIENT_ERR("unable to iterate encryptedFieldConfig");
        return false;
    }

    while (bson_iter_next(&iter)) {
        if (strcmp(bson_iter_key(&iter), "escCollection") == 0) {
            has_escCollection = true;
        }
        if (strcmp(bson_iter_key(&iter), "eccCollection") == 0) {
            has_eccCollection = true;
        }
        if (strcmp(bson_iter_key(&iter), "ecocCollection") == 0) {
            has_ecocCollection = true;
        }
        if (!BSON_APPEND_VALUE(dst, bson_iter_key(&iter), bson_iter_value(&iter))) {
            CLIENT_ERR("unable to append field: %s", bson_iter_key(&iter));
            return false;
        }
    }

    if (!has_escCollection) {
        char *default_escCollection = bson_strdup_printf("enxcol_.%s.esc", target_coll);
        if (!BSON_APPEND_UTF8(dst, "escCollection", default_escCollection)) {
            CLIENT_ERR("unable to append escCollection");
            bson_free(default_escCollection);
            return false;
        }
        bson_free(default_escCollection);
    }
    if (!has_eccCollection && !ctx->crypt->opts.use_fle2_v2) {
        char *default_eccCollection = bson_strdup_printf("enxcol_.%s.ecc", target_coll);
        if (!BSON_APPEND_UTF8(dst, "eccCollection", default_eccCollection)) {
            CLIENT_ERR("unable to append eccCollection");
            bson_free(default_eccCollection);
            return false;
        }
        bson_free(default_eccCollection);
    }
    if (!has_ecocCollection) {
        char *default_ecocCollection = bson_strdup_printf("enxcol_.%s.ecoc", target_coll);
        if (!BSON_APPEND_UTF8(dst, "ecocCollection", default_ecocCollection)) {
            CLIENT_ERR("unable to append ecocCollection");
            bson_free(default_ecocCollection);
            return false;
        }
        bson_free(default_ecocCollection);
    }
    return true;
}

static bool _fle2_append_encryptionInformation(const mongocrypt_ctx_t *ctx,
                                               bson_t *dst,
                                               const char *target_ns,
                                               bson_t *encryptedFieldConfig,
                                               bson_t *deleteTokens,
                                               const char *target_coll,
                                               mongocrypt_status_t *status) {
    bson_t encryption_information_bson;
    bson_t schema_bson;
    bson_t encrypted_field_config_bson;

    BSON_ASSERT_PARAM(dst);
    BSON_ASSERT_PARAM(target_ns);
    BSON_ASSERT_PARAM(encryptedFieldConfig);
    /* deleteTokens may be NULL */
    BSON_ASSERT_PARAM(target_coll);

    if (!BSON_APPEND_DOCUMENT_BEGIN(dst, "encryptionInformation", &encryption_information_bson)) {
        CLIENT_ERR("unable to begin appending 'encryptionInformation'");
        return false;
    }
    if (!BSON_APPEND_INT32(&encryption_information_bson, "type", 1)) {
        CLIENT_ERR("unable to append type to 'encryptionInformation'");
        return false;
    }
    if (!BSON_APPEND_DOCUMENT_BEGIN(&encryption_information_bson, "schema", &schema_bson)) {
        CLIENT_ERR("unable to begin appending 'schema' to 'encryptionInformation'");
        return false;
    }

    if (!BSON_APPEND_DOCUMENT_BEGIN(&schema_bson, target_ns, &encrypted_field_config_bson)) {
        CLIENT_ERR("unable to begin appending 'encryptedFieldConfig' to "
                   "'encryptionInformation'.'schema'");
        return false;
    }

    if (!_fle2_append_encryptedFieldConfig(ctx,
                                           &encrypted_field_config_bson,
                                           encryptedFieldConfig,
                                           target_coll,
                                           status)) {
        return false;
    }

    if (!bson_append_document_end(&schema_bson, &encrypted_field_config_bson)) {
        CLIENT_ERR("unable to end appending 'encryptedFieldConfig' to "
                   "'encryptionInformation'.'schema'");
        return false;
    }
    if (!bson_append_document_end(&encryption_information_bson, &schema_bson)) {
        CLIENT_ERR("unable to end appending 'schema' to 'encryptionInformation'");
        return false;
    }

    if (deleteTokens != NULL) {
        bson_t delete_tokens_bson;
        if (!BSON_APPEND_DOCUMENT_BEGIN(&encryption_information_bson, "deleteTokens", &delete_tokens_bson)) {
            CLIENT_ERR("unable to begin appending 'deleteTokens' to "
                       "'encryptionInformation'");
            return false;
        }
        if (!BSON_APPEND_DOCUMENT(&delete_tokens_bson, target_ns, deleteTokens)) {
            CLIENT_ERR("unable to append '%s' to 'deleteTokens'", target_ns);
            return false;
        }
        if (!bson_append_document_end(&encryption_information_bson, &delete_tokens_bson)) {
            CLIENT_ERR("unable to end appending 'deleteTokens' to "
                       "'encryptionInformation'");
            return false;
        }
    }

    if (!bson_append_document_end(dst, &encryption_information_bson)) {
        CLIENT_ERR("unable to end appending 'encryptionInformation'");
        return false;
    }
    return true;
}

typedef enum { MC_TO_CSFLE, MC_TO_MONGOCRYPTD, MC_TO_MONGOD } mc_cmd_target_t;

/**
 * @brief Add "encryptionInformation" to a command.
 *
 * @param cmd_name The name of the command.
 * @param cmd The command being rewritten. It is an input and output.
 * @param target_ns The <db>.<collection> namespace for the command.
 * @param encryptedFieldConfig The "encryptedFields" document for the
 * collection.
 * @param deleteTokens Delete tokens to append to "encryptionInformation". May
 * be NULL.
 * @param target_coll The collection name.
 * @param cmd_target The intended destination of the command. csfle,
 * mongocryptd, and mongod have different requirements for the location of
 * "encryptionInformation".
 * @param status Output status.
 * @return true On success
 * @return false Otherwise. Sets a failing status message in this case.
 */
static bool _fle2_insert_encryptionInformation(const mongocrypt_ctx_t *ctx,
                                               const char *cmd_name,
                                               bson_t *cmd /* in and out */,
                                               const char *target_ns,
                                               bson_t *encryptedFieldConfig,
                                               bson_t *deleteTokens,
                                               const char *target_coll,
                                               mc_cmd_target_t cmd_target,
                                               mongocrypt_status_t *status) {
    bson_t out = BSON_INITIALIZER;
    bson_t explain = BSON_INITIALIZER;
    bson_iter_t iter;
    bool ok = false;

    BSON_ASSERT_PARAM(cmd_name);
    BSON_ASSERT_PARAM(cmd);
    BSON_ASSERT_PARAM(target_ns);
    BSON_ASSERT_PARAM(encryptedFieldConfig);
    /* deleteTokens may be NULL */
    BSON_ASSERT_PARAM(target_coll);

    // For `bulkWrite`, append `encryptionInformation` inside the `nsInfo.0` document.
    if (0 == strcmp(cmd_name, "bulkWrite")) {
        // Get the single `nsInfo` document from the input command.
        bson_t nsInfo; // Non-owning.
        {
            bson_iter_t nsInfo_iter;
            if (!bson_iter_init(&nsInfo_iter, cmd)) {
                CLIENT_ERR("failed to iterate command");
                goto fail;
            }
            if (!bson_iter_find_descendant(&nsInfo_iter, "nsInfo.0", &nsInfo_iter)) {
                CLIENT_ERR("expected one namespace in `bulkWrite`, but found zero.");
                goto fail;
            }
            if (bson_has_field(cmd, "nsInfo.1")) {
                CLIENT_ERR(
                    "expected one namespace in `bulkWrite`, but found more than one. Only one namespace is supported.");
                goto fail;
            }
            if (!mc_iter_document_as_bson(&nsInfo_iter, &nsInfo, status)) {
                goto fail;
            }
            // Ensure `nsInfo` does not already have an `encryptionInformation` field.
            if (bson_has_field(&nsInfo, "encryptionInformation")) {
                CLIENT_ERR("unexpected `encryptionInformation` present in input `nsInfo`.");
                goto fail;
            }
        }

        // Copy input and append `encryptionInformation` to `nsInfo`.
        {
            // Append everything from input except `nsInfo`.
            bson_copy_to_excluding_noinit(cmd, &out, "nsInfo", NULL);
            // Append `nsInfo` array.
            bson_t nsInfo_array;
            if (!BSON_APPEND_ARRAY_BEGIN(&out, "nsInfo", &nsInfo_array)) {
                CLIENT_ERR("unable to begin appending 'nsInfo' array");
                goto fail;
            }
            bson_t nsInfo_array_0;
            if (!BSON_APPEND_DOCUMENT_BEGIN(&nsInfo_array, "0", &nsInfo_array_0)) {
                CLIENT_ERR("unable to append 'nsInfo.0' document");
                goto fail;
            }
            // Copy everything from input `nsInfo`.
            bson_concat(&nsInfo_array_0, &nsInfo);
            // And append `encryptionInformation`.
            if (!_fle2_append_encryptionInformation(ctx,
                                                    &nsInfo_array_0,
                                                    target_ns,
                                                    encryptedFieldConfig,
                                                    deleteTokens,
                                                    target_coll,
                                                    status)) {
                goto fail;
            }
            if (!bson_append_document_end(&nsInfo_array, &nsInfo_array_0)) {
                CLIENT_ERR("unable to end appending 'nsInfo' document in array");
            }
            if (!bson_append_array_end(&out, &nsInfo_array)) {
                CLIENT_ERR("unable to end appending 'nsInfo' array");
                goto fail;
            }
            // Overwrite `cmd`.
            bson_destroy(cmd);
            if (!bson_steal(cmd, &out)) {
                CLIENT_ERR("failed to steal BSON with encryptionInformation");
                goto fail;
            }
        }

        goto success;
    }

    if (0 != strcmp(cmd_name, "explain") || cmd_target == MC_TO_MONGOCRYPTD) {
        // All commands except "explain" and "bulkWrite" expect "encryptionInformation"
        // at top-level. "explain" sent to mongocryptd expects
        // "encryptionInformation" at top-level.
        if (!_fle2_append_encryptionInformation(ctx,
                                                cmd,
                                                target_ns,
                                                encryptedFieldConfig,
                                                deleteTokens,
                                                target_coll,
                                                status)) {
            goto fail;
        }
        bson_destroy(&out);
        goto success;
    }

    // The "explain" command for csfle is a special case.
    // mongocryptd expects "encryptionInformation" to be a sibling of the
    // "explain" document. Example:
    // {
    //    "explain": { "find": "to-mongocryptd" },
    //    "encryptionInformation": {}
    // }
    // csfle and mongod expect "encryptionInformation" to be nested in the
    // "explain" document. Example:
    // {
    //    "explain": {
    //       "find": "to-csfle-or-mongod"
    //       "encryptionInformation": {}
    //    }
    // }
    BSON_ASSERT(bson_iter_init_find(&iter, cmd, "explain"));
    if (!BSON_ITER_HOLDS_DOCUMENT(&iter)) {
        CLIENT_ERR("expected 'explain' to be document");
        goto fail;
    }

    {
        bson_t tmp;
        if (!mc_iter_document_as_bson(&iter, &tmp, status)) {
            goto fail;
        }
        bson_destroy(&explain);
        bson_copy_to(&tmp, &explain);
    }

    if (!_fle2_append_encryptionInformation(ctx,
                                            &explain,
                                            target_ns,
                                            encryptedFieldConfig,
                                            deleteTokens,
                                            target_coll,
                                            status)) {
        goto fail;
    }

    if (!BSON_APPEND_DOCUMENT(&out, "explain", &explain)) {
        CLIENT_ERR("unable to append 'explain' document");
        goto fail;
    }

    bson_copy_to_excluding_noinit(cmd, &out, "explain", NULL);
    bson_destroy(cmd);
    if (!bson_steal(cmd, &out)) {
        CLIENT_ERR("failed to steal BSON with encryptionInformation");
        goto fail;
    }

success:
    ok = true;
fail:
    bson_destroy(&explain);
    if (!ok) {
        bson_destroy(&out);
    }
    return ok;
}

/* Construct the list collections command to send. */
static bool _mongo_op_collinfo(mongocrypt_ctx_t *ctx, mongocrypt_binary_t *out) {
    _mongocrypt_ctx_encrypt_t *ectx;
    bson_t *cmd;

    BSON_ASSERT_PARAM(ctx);
    BSON_ASSERT_PARAM(out);

    ectx = (_mongocrypt_ctx_encrypt_t *)ctx;
    cmd = BCON_NEW("name", BCON_UTF8(ectx->target_coll));
    CRYPT_TRACEF(&ectx->parent.crypt->log, "constructed: %s\n", tmp_json(cmd));
    _mongocrypt_buffer_steal_from_bson(&ectx->list_collections_filter, cmd);
    out->data = ectx->list_collections_filter.data;
    out->len = ectx->list_collections_filter.len;
    return true;
}

static bool _set_schema_from_collinfo(mongocrypt_ctx_t *ctx, bson_t *collinfo) {
    bson_iter_t iter;
    _mongocrypt_ctx_encrypt_t *ectx;
    bool found_jsonschema = false;

    BSON_ASSERT_PARAM(ctx);
    BSON_ASSERT_PARAM(collinfo);

    /* Parse out the schema. */
    ectx = (_mongocrypt_ctx_encrypt_t *)ctx;

    /* Disallow views. */
    if (bson_iter_init_find(&iter, collinfo, "type") && BSON_ITER_HOLDS_UTF8(&iter) && bson_iter_utf8(&iter, NULL)
        && 0 == strcmp("view", bson_iter_utf8(&iter, NULL))) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "cannot auto encrypt a view");
    }

    if (!bson_iter_init(&iter, collinfo)) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "BSON malformed");
    }

    if (bson_iter_find_descendant(&iter, "options.encryptedFields", &iter)) {
        if (!BSON_ITER_HOLDS_DOCUMENT(&iter)) {
            return _mongocrypt_ctx_fail_w_msg(ctx, "options.encryptedFields is not a BSON document");
        }
        if (!_mongocrypt_buffer_copy_from_document_iter(&ectx->encrypted_field_config, &iter)) {
            return _mongocrypt_ctx_fail_w_msg(ctx, "unable to copy options.encryptedFields");
        }
        bson_t efc_bson;
        if (!_mongocrypt_buffer_to_bson(&ectx->encrypted_field_config, &efc_bson)) {
            return _mongocrypt_ctx_fail_w_msg(ctx, "unable to create BSON from encrypted_field_config");
        }
        if (!mc_EncryptedFieldConfig_parse(&ectx->efc, &efc_bson, ctx->status, ctx->crypt->opts.use_range_v2)) {
            _mongocrypt_ctx_fail(ctx);
            return false;
        }
    } else if (0 == strcmp(ectx->cmd_name, "bulkWrite")) {
        ectx->used_empty_encryptedFields = true;
        // `bulkWrite` is a special case. Sending `bulkWrite` with `jsonSchema` to query analysis results in an error:
        // `The bulkWrite command only supports Queryable Encryption`
        //
        // Add an empty encryptedFields (rather than an empty JSON schema) to ensure `bulkWrite` can be sent to query
        // analysis.
        bson_t empty_encryptedFields = BSON_INITIALIZER;
        {
            char *escCollection = bson_strdup_printf("enxcol_.%s.esc", ectx->target_coll);
            char *ecocCollection = bson_strdup_printf("enxcol_.%s.ecoc", ectx->target_coll);
            bson_t empty_array = BSON_INITIALIZER;
            if (!BSON_APPEND_UTF8(&empty_encryptedFields, "escCollection", escCollection)) {
                return _mongocrypt_ctx_fail_w_msg(ctx, "failed to append `escCollection`");
            }
            if (!BSON_APPEND_UTF8(&empty_encryptedFields, "ecocCollection", ecocCollection)) {
                return _mongocrypt_ctx_fail_w_msg(ctx, "failed to append `ecocCollection`");
            }
            if (!BSON_APPEND_ARRAY(&empty_encryptedFields, "fields", &empty_array)) {
                return _mongocrypt_ctx_fail_w_msg(ctx, "failed to append `fields`");
            }

            bson_destroy(&empty_array);
            bson_free(escCollection);
            bson_free(ecocCollection);
        }

        if (!mc_EncryptedFieldConfig_parse(&ectx->efc,
                                           &empty_encryptedFields,
                                           ctx->status,
                                           ctx->crypt->opts.use_range_v2)) {
            bson_destroy(&empty_encryptedFields);
            _mongocrypt_ctx_fail(ctx);
            return false;
        }
        _mongocrypt_buffer_steal_from_bson(&ectx->encrypted_field_config, &empty_encryptedFields);
    }

    BSON_ASSERT(bson_iter_init(&iter, collinfo));

    if (bson_iter_find_descendant(&iter, "options.validator", &iter) && BSON_ITER_HOLDS_DOCUMENT(&iter)) {
        if (!bson_iter_recurse(&iter, &iter)) {
            return _mongocrypt_ctx_fail_w_msg(ctx, "BSON malformed");
        }
        while (bson_iter_next(&iter)) {
            const char *key;

            key = bson_iter_key(&iter);
            BSON_ASSERT(key);
            if (0 == strcmp("$jsonSchema", key)) {
                if (found_jsonschema) {
                    return _mongocrypt_ctx_fail_w_msg(ctx, "duplicate $jsonSchema fields found");
                }
                if (!_mongocrypt_buffer_copy_from_document_iter(&ectx->schema, &iter)) {
                    return _mongocrypt_ctx_fail_w_msg(ctx, "malformed $jsonSchema");
                }
                found_jsonschema = true;
            } else {
                ectx->collinfo_has_siblings = true;
            }
        }
    }

    if (!found_jsonschema) {
        bson_t empty = BSON_INITIALIZER;

        _mongocrypt_buffer_steal_from_bson(&ectx->schema, &empty);
    }

    return true;
}

/* get_command_name returns the name of a command. The command name is the first
 * field. For example, the command name of: {"find": "foo", "filter": {"bar":
 * 1}} is "find". */
static const char *get_command_name(_mongocrypt_buffer_t *cmd, mongocrypt_status_t *status) {
    bson_t cmd_bson;
    bson_iter_t iter;
    const char *cmd_name;

    BSON_ASSERT_PARAM(cmd);

    if (!_mongocrypt_buffer_to_bson(cmd, &cmd_bson)) {
        CLIENT_ERR("unable to convert command buffer to BSON");
        return NULL;
    }

    if (!bson_iter_init(&iter, &cmd_bson)) {
        CLIENT_ERR("unable to iterate over command BSON");
        return NULL;
    }

    /* The command name is the first key. */
    if (!bson_iter_next(&iter)) {
        CLIENT_ERR("unexpected empty BSON for command");
        return NULL;
    }

    cmd_name = bson_iter_key(&iter);
    if (!cmd_name) {
        CLIENT_ERR("unable to get command name from BSON");
        return NULL;
    }
    return cmd_name;
}

static bool command_needs_deleteTokens(mongocrypt_ctx_t *ctx, const char *command_name) {
    BSON_ASSERT_PARAM(ctx);
    BSON_ASSERT_PARAM(command_name);
    BSON_ASSERT(ctx->kb.crypt);

    if (ctx->crypt->opts.use_fle2_v2) {
        return false;
    }

    const char *cmds_needing_deleteTokens[] = {"delete", "update", "findAndModify"};

    BSON_ASSERT_PARAM(command_name);

    size_t i;
    for (i = 0; i < sizeof(cmds_needing_deleteTokens) / sizeof(cmds_needing_deleteTokens[0]); i++) {
        if (0 == strcmp(cmds_needing_deleteTokens[i], command_name)) {
            return true;
        }
    }

    return false;
}

/* context_uses_fle2 returns true if the context uses FLE 2 behavior.
 * If a collection has an encryptedFields document, it uses FLE 2.
 */
static bool context_uses_fle2(mongocrypt_ctx_t *ctx) {
    _mongocrypt_ctx_encrypt_t *ectx = (_mongocrypt_ctx_encrypt_t *)ctx;

    BSON_ASSERT_PARAM(ctx);

    return !_mongocrypt_buffer_empty(&ectx->encrypted_field_config);
}

/* _fle2_collect_keys_for_deleteTokens requests keys required to produce
 * deleteTokens. deleteTokens is only applicable to FLE 2. */
static bool _fle2_collect_keys_for_deleteTokens(mongocrypt_ctx_t *ctx) {
    _mongocrypt_ctx_encrypt_t *ectx = (_mongocrypt_ctx_encrypt_t *)ctx;

    BSON_ASSERT_PARAM(ctx);

    /* deleteTokens are only appended for FLE 2. */
    if (!context_uses_fle2(ctx)) {
        return true;
    }

    const char *cmd_name = ectx->cmd_name;

    if (!command_needs_deleteTokens(ctx, cmd_name)) {
        /* Command does not require deleteTokens. */
        return true;
    }

    mc_EncryptedField_t *field;

    for (field = ectx->efc.fields; field != NULL; field = field->next) {
        if (field->supported_queries) {
            if (!_mongocrypt_key_broker_request_id(&ctx->kb, &field->keyId)) {
                _mongocrypt_key_broker_status(&ctx->kb, ctx->status);
                _mongocrypt_ctx_fail(ctx);
                return false;
            }
        }
    }
    return true;
}

/* _fle2_collect_keys_for_compaction requests keys required to produce
 * compactionTokens or cleanupTokens.
 * compactionTokens and cleanupTokens are only applicable to FLE 2. */
static bool _fle2_collect_keys_for_compaction(mongocrypt_ctx_t *ctx) {
    _mongocrypt_ctx_encrypt_t *ectx = (_mongocrypt_ctx_encrypt_t *)ctx;

    BSON_ASSERT_PARAM(ctx);

    /* compactionTokens are only appended for FLE 2. */
    if (!context_uses_fle2(ctx)) {
        return true;
    }

    const char *cmd_name = ectx->cmd_name;

    if (0 != strcmp(cmd_name, "compactStructuredEncryptionData")
        && 0 != strcmp(cmd_name, "cleanupStructuredEncryptionData")) {
        return true;
    }

    /* (compact/cleanup)StructuredEncryptionData must not be sent to mongocryptd. */
    ectx->bypass_query_analysis = true;

    mc_EncryptedField_t *field;

    for (field = ectx->efc.fields; field != NULL; field = field->next) {
        if (!_mongocrypt_key_broker_request_id(&ctx->kb, &field->keyId)) {
            _mongocrypt_key_broker_status(&ctx->kb, ctx->status);
            _mongocrypt_ctx_fail(ctx);
            return false;
        }
    }
    return true;
}

static bool _mongo_feed_collinfo(mongocrypt_ctx_t *ctx, mongocrypt_binary_t *in) {
    bson_t as_bson;

    _mongocrypt_ctx_encrypt_t *ectx;

    BSON_ASSERT_PARAM(ctx);
    BSON_ASSERT_PARAM(in);

    ectx = (_mongocrypt_ctx_encrypt_t *)ctx;
    if (!bson_init_static(&as_bson, in->data, in->len)) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "BSON malformed");
    }

    /* Cache the received collinfo. */
    if (!_mongocrypt_cache_add_copy(&ctx->crypt->cache_collinfo, ectx->target_ns, &as_bson, ctx->status)) {
        return _mongocrypt_ctx_fail(ctx);
    }

    if (!_set_schema_from_collinfo(ctx, &as_bson)) {
        return false;
    }

    return true;
}

static bool _try_run_csfle_marking(mongocrypt_ctx_t *ctx);

static bool _mongo_done_collinfo(mongocrypt_ctx_t *ctx) {
    _mongocrypt_ctx_encrypt_t *ectx;

    BSON_ASSERT_PARAM(ctx);

    ectx = (_mongocrypt_ctx_encrypt_t *)ctx;
    if (_mongocrypt_buffer_empty(&ectx->schema)) {
        bson_t empty_collinfo = BSON_INITIALIZER;

        /* If no collinfo was fed, apply and cache an empty collinfo. */
        if (!_set_schema_from_collinfo(ctx, &empty_collinfo)) {
            bson_destroy(&empty_collinfo);
            return false;
        }
        if (!_mongocrypt_cache_add_copy(&ctx->crypt->cache_collinfo, ectx->target_ns, &empty_collinfo, ctx->status)) {
            bson_destroy(&empty_collinfo);
            return _mongocrypt_ctx_fail(ctx);
        }
        bson_destroy(&empty_collinfo);
    }

    if (!_fle2_collect_keys_for_deleteTokens(ctx)) {
        return false;
    }

    if (!_fle2_collect_keys_for_compaction(ctx)) {
        return false;
    }

    if (ectx->bypass_query_analysis) {
        /* Keys may have been requested for deleteTokens or compactionTokens.
         * Finish key requests. */
        _mongocrypt_key_broker_requests_done(&ctx->kb);
        return _mongocrypt_ctx_state_from_key_broker(ctx);
    }
    ectx->parent.state = MONGOCRYPT_CTX_NEED_MONGO_MARKINGS;
    return _try_run_csfle_marking(ctx);
}

static const char *_mongo_db_collinfo(mongocrypt_ctx_t *ctx) {
    _mongocrypt_ctx_encrypt_t *ectx;

    BSON_ASSERT_PARAM(ctx);

    ectx = (_mongocrypt_ctx_encrypt_t *)ctx;
    if (!ectx->target_db) {
        _mongocrypt_ctx_fail_w_msg(ctx, "Expected target database for `listCollections`, but none exists.");
        return NULL;
    }
    return ectx->target_db;
}

static bool _fle2_mongo_op_markings(mongocrypt_ctx_t *ctx, bson_t *out) {
    _mongocrypt_ctx_encrypt_t *ectx;
    bson_t cmd_bson = BSON_INITIALIZER, encrypted_field_config_bson = BSON_INITIALIZER;

    BSON_ASSERT_PARAM(ctx);
    BSON_ASSERT_PARAM(out);

    ectx = (_mongocrypt_ctx_encrypt_t *)ctx;

    BSON_ASSERT(ctx->state == MONGOCRYPT_CTX_NEED_MONGO_MARKINGS);
    BSON_ASSERT(context_uses_fle2(ctx));

    if (!_mongocrypt_buffer_to_bson(&ectx->original_cmd, &cmd_bson)) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "unable to convert original_cmd to BSON");
    }

    if (!_mongocrypt_buffer_to_bson(&ectx->encrypted_field_config, &encrypted_field_config_bson)) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "unable to convert encrypted_field_config to BSON");
    }

    const char *cmd_name = ectx->cmd_name;

    // If input command included $db, do not include it in the command to
    // mongocryptd. Drivers are expected to append $db in the RunCommand helper
    // used to send the command.
    bson_init(out);
    bson_copy_to_excluding_noinit(&cmd_bson, out, "$db", NULL);
    if (!_fle2_insert_encryptionInformation(ctx,
                                            cmd_name,
                                            out,
                                            ectx->target_ns,
                                            &encrypted_field_config_bson,
                                            NULL /* deleteTokens */,
                                            ectx->target_coll,
                                            ctx->crypt->csfle.okay ? MC_TO_CSFLE : MC_TO_MONGOCRYPTD,
                                            ctx->status)) {
        return _mongocrypt_ctx_fail(ctx);
    }
    return true;
}

/**
 * @brief Create the server-side command that contains information for
 * generating encryption markings via query analysis.
 *
 * @param ctx The encryption context.
 * @param out The destination of the generated BSON document
 * @return true On success
 * @return false Otherwise. Sets a failing status message in this case.
 */
static bool _create_markings_cmd_bson(mongocrypt_ctx_t *ctx, bson_t *out) {
    _mongocrypt_ctx_encrypt_t *ectx = (_mongocrypt_ctx_encrypt_t *)ctx;

    BSON_ASSERT_PARAM(ctx);
    BSON_ASSERT_PARAM(out);

    if (context_uses_fle2(ctx)) {
        // Defer to FLE2 to generate the markings command
        return _fle2_mongo_op_markings(ctx, out);
    }

    // For FLE1:
    // Get the original command document
    bson_t bson_view = BSON_INITIALIZER;
    if (!_mongocrypt_buffer_to_bson(&ectx->original_cmd, &bson_view)) {
        _mongocrypt_ctx_fail_w_msg(ctx, "invalid BSON cmd");
        return false;
    }

    // Copy the command to the output
    // If input command included $db, do not include it in the command to
    // mongocryptd. Drivers are expected to append $db in the RunCommand helper
    // used to send the command.
    bson_init(out);
    bson_copy_to_excluding_noinit(&bson_view, out, "$db", NULL);

    if (!_mongocrypt_buffer_empty(&ectx->schema)) {
        // We have a schema buffer. View it as BSON:
        if (!_mongocrypt_buffer_to_bson(&ectx->schema, &bson_view)) {
            _mongocrypt_ctx_fail_w_msg(ctx, "invalid BSON schema");
            return false;
        }
        // Append the jsonSchema to the output command
        BSON_APPEND_DOCUMENT(out, "jsonSchema", &bson_view);
    } else {
        bson_t empty = BSON_INITIALIZER;
        BSON_APPEND_DOCUMENT(out, "jsonSchema", &empty);
    }

    // if a local schema was not set, set isRemoteSchema=true
    BSON_APPEND_BOOL(out, "isRemoteSchema", !ectx->used_local_schema);
    return true;
}

static bool _mongo_op_markings(mongocrypt_ctx_t *ctx, mongocrypt_binary_t *out) {
    _mongocrypt_ctx_encrypt_t *ectx = (_mongocrypt_ctx_encrypt_t *)ctx;

    BSON_ASSERT_PARAM(ctx);
    BSON_ASSERT_PARAM(out);

    if (ectx->ismaster.needed) {
        if (_mongocrypt_buffer_empty(&ectx->ismaster.cmd)) {
            bson_t ismaster_cmd = BSON_INITIALIZER;
            // Store the generated command:
            BSON_APPEND_INT32(&ismaster_cmd, "isMaster", 1);
            _mongocrypt_buffer_steal_from_bson(&ectx->ismaster.cmd, &ismaster_cmd);
        }

        out->data = ectx->ismaster.cmd.data;
        out->len = ectx->ismaster.cmd.len;
        return true;
    }

    if (_mongocrypt_buffer_empty(&ectx->mongocryptd_cmd)) {
        // We need to generate the command document
        bson_t cmd_bson = BSON_INITIALIZER;
        if (!_create_markings_cmd_bson(ctx, &cmd_bson)) {
            // Failed
            bson_destroy(&cmd_bson);
            return false;
        }
        // Store the generated command:
        _mongocrypt_buffer_steal_from_bson(&ectx->mongocryptd_cmd, &cmd_bson);
    }

    // If we reach here, we have a valid mongocrypt_cmd
    out->data = ectx->mongocryptd_cmd.data;
    out->len = ectx->mongocryptd_cmd.len;
    return true;
}

static bool _collect_key_from_marking(void *ctx, _mongocrypt_buffer_t *in, mongocrypt_status_t *status) {
    _mongocrypt_marking_t marking;
    _mongocrypt_key_broker_t *kb;
    bool res;

    BSON_ASSERT_PARAM(ctx);
    BSON_ASSERT_PARAM(in);

    kb = (_mongocrypt_key_broker_t *)ctx;

    if (!_mongocrypt_marking_parse_unowned(in, &marking, status)) {
        _mongocrypt_marking_cleanup(&marking);
        return false;
    }

    if (marking.type == MONGOCRYPT_MARKING_FLE1_BY_ID) {
        res = _mongocrypt_key_broker_request_id(kb, &marking.key_id);
    } else if (marking.type == MONGOCRYPT_MARKING_FLE1_BY_ALTNAME) {
        res = _mongocrypt_key_broker_request_name(kb, &marking.key_alt_name);
    } else {
        BSON_ASSERT(marking.type == MONGOCRYPT_MARKING_FLE2_ENCRYPTION);
        res = _mongocrypt_key_broker_request_id(kb, &marking.fle2.index_key_id)
           && _mongocrypt_key_broker_request_id(kb, &marking.fle2.user_key_id);
    }

    if (!res) {
        _mongocrypt_key_broker_status(kb, status);
        _mongocrypt_marking_cleanup(&marking);
        return false;
    }

    _mongocrypt_marking_cleanup(&marking);

    return true;
}

static bool _mongo_feed_markings(mongocrypt_ctx_t *ctx, mongocrypt_binary_t *in) {
    /* Find keys. */
    bson_t as_bson;
    bson_iter_t iter = {0};
    _mongocrypt_ctx_encrypt_t *ectx;

    BSON_ASSERT_PARAM(ctx);
    BSON_ASSERT_PARAM(in);

    ectx = (_mongocrypt_ctx_encrypt_t *)ctx;
    if (!_mongocrypt_binary_to_bson(in, &as_bson)) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "malformed BSON");
    }

    if (ectx->ismaster.needed) {
        /* This is a response to the 'isMaster' command. */
        if (!bson_iter_init_find(&iter, &as_bson, "maxWireVersion")) {
            return _mongocrypt_ctx_fail_w_msg(ctx,
                                              "expected to find 'maxWireVersion' in isMaster response, but did "
                                              "not.");
        }
        if (!BSON_ITER_HOLDS_INT32(&iter)) {
            return _mongocrypt_ctx_fail_w_msg(ctx, "expected 'maxWireVersion' to be int32.");
        }
        ectx->ismaster.maxwireversion = bson_iter_int32(&iter);
        return true;
    }

    if (bson_iter_init_find(&iter, &as_bson, "schemaRequiresEncryption") && !bson_iter_as_bool(&iter)) {
        /* TODO: update cache: this schema does not require encryption. */

        /* If using a local schema, warn if there are no encrypted fields. */
        if (ectx->used_local_schema) {
            _mongocrypt_log(&ctx->crypt->log,
                            MONGOCRYPT_LOG_LEVEL_WARNING,
                            "local schema used but does not have encryption specifiers");
        }
        return true;
    } else {
        /* if the schema requires encryption, but has sibling validators, error.
         */
        if (ectx->collinfo_has_siblings) {
            return _mongocrypt_ctx_fail_w_msg(ctx,
                                              "schema requires encryption, "
                                              "but collection JSON schema "
                                              "validator has siblings");
        }
    }

    if (bson_iter_init_find(&iter, &as_bson, "hasEncryptedPlaceholders") && !bson_iter_as_bool(&iter)) {
        return true;
    }

    if (!bson_iter_init_find(&iter, &as_bson, "result")) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "malformed marking, no 'result'");
    }

    if (!_mongocrypt_buffer_copy_from_document_iter(&ectx->marked_cmd, &iter)) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "malformed marking, 'result' must be a document");
    }

    if (!bson_iter_recurse(&iter, &iter)) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "malformed marking, could not recurse into 'result'");
    }
    if (!_mongocrypt_traverse_binary_in_bson(_collect_key_from_marking,
                                             (void *)&ctx->kb,
                                             TRAVERSE_MATCH_MARKING,
                                             &iter,
                                             ctx->status)) {
        return _mongocrypt_ctx_fail(ctx);
    }

    return true;
}

static bool mongocrypt_ctx_encrypt_ismaster_done(mongocrypt_ctx_t *ctx);

static bool _mongo_done_markings(mongocrypt_ctx_t *ctx) {
    _mongocrypt_ctx_encrypt_t *ectx = (_mongocrypt_ctx_encrypt_t *)ctx;

    BSON_ASSERT_PARAM(ctx);

    if (ectx->ismaster.needed) {
        return mongocrypt_ctx_encrypt_ismaster_done(ctx);
    }
    (void)_mongocrypt_key_broker_requests_done(&ctx->kb);
    return _mongocrypt_ctx_state_from_key_broker(ctx);
}

/**
 * @brief Append $db to a command being passed to csfle.
 */
static bool _add_dollar_db(const char *cmd_name, bson_t *cmd, const char *cmd_db, mongocrypt_status_t *status) {
    bson_t out = BSON_INITIALIZER;
    bson_t explain = BSON_INITIALIZER;
    bson_iter_t iter;
    bool ok = false;

    BSON_ASSERT_PARAM(cmd_name);
    BSON_ASSERT_PARAM(cmd);
    BSON_ASSERT_PARAM(cmd_db);

    if (!bson_iter_init_find(&iter, cmd, "$db")) {
        if (!BSON_APPEND_UTF8(cmd, "$db", cmd_db)) {
            CLIENT_ERR("failed to append '$db'");
            goto fail;
        }
    }

    if (0 != strcmp(cmd_name, "explain")) {
        goto success;
    }

    // The "explain" command for csfle is a special case.
    // csfle expects "$db" to be nested in the "explain" document and match the
    // top-level "$db". Example:
    // {
    //    "explain": {
    //       "find": "to-csfle"
    //       "$db": "db"
    //    }
    //    "$db": "db"
    // }
    BSON_ASSERT(bson_iter_init_find(&iter, cmd, "explain"));
    if (!BSON_ITER_HOLDS_DOCUMENT(&iter)) {
        CLIENT_ERR("expected 'explain' to be document");
        goto fail;
    }

    {
        bson_t tmp;
        if (!mc_iter_document_as_bson(&iter, &tmp, status)) {
            goto fail;
        }
        bson_copy_to(&tmp, &explain);
    }

    if (!BSON_APPEND_UTF8(&explain, "$db", cmd_db)) {
        CLIENT_ERR("failed to append '$db'");
        goto fail;
    }

    if (!BSON_APPEND_DOCUMENT(&out, "explain", &explain)) {
        CLIENT_ERR("unable to append 'explain' document");
        goto fail;
    }

    bson_copy_to_excluding_noinit(cmd, &out, "explain", NULL);
    bson_destroy(cmd);
    if (!bson_steal(cmd, &out)) {
        CLIENT_ERR("failed to steal BSON without encryptionInformation");
        goto fail;
    }

success:
    ok = true;
fail:
    bson_destroy(&explain);
    if (!ok) {
        bson_destroy(&out);
    }
    return ok;
}

/**
 * @brief Attempt to generate csfle markings using a csfle dynamic library.
 *
 * @param ctx A context which has state NEED_MONGO_MARKINGS
 * @return true On success
 * @return false On error.
 *
 * This should be called only when we are ready for markings in the command
 * document. This function will only do anything if the csfle dynamic library
 * is loaded, otherwise it returns success immediately and leaves the state
 * as NEED_MONGO_MARKINGS.
 *
 * If csfle is loaded, this function will request the csfle library generate a
 * marked command document based on the caller's schema. If successful, the
 * state will be changed via @ref _mongo_done_markings().
 *
 * The purpose of this function is to short-circuit the phase of encryption
 * wherein we would normally return to the driver and give them the opportunity
 * to generate the markings by passing a special command to a mongocryptd daemon
 * process. Instead, we'll do it ourselves here, if possible.
 */
static bool _try_run_csfle_marking(mongocrypt_ctx_t *ctx) {
    BSON_ASSERT_PARAM(ctx);

    BSON_ASSERT(ctx->state == MONGOCRYPT_CTX_NEED_MONGO_MARKINGS
                && "_try_run_csfle_marking() should only be called when mongocrypt is "
                   "ready for markings");

    _mongocrypt_ctx_encrypt_t *ectx = (_mongocrypt_ctx_encrypt_t *)ctx;

    BSON_ASSERT(ctx->crypt);

    // We have a valid schema and just need to mark the fields for encryption
    if (!ctx->crypt->csfle.okay) {
        // We don't have a csfle library to use to obtain the markings. It's up to
        // caller to resolve them.
        return true;
    }

    _mongo_crypt_v1_vtable csfle = ctx->crypt->csfle;
    mongo_crypt_v1_lib *csfle_lib = ctx->crypt->csfle_lib;
    BSON_ASSERT(csfle_lib);
    bool okay = false;

    // Obtain the command for markings
    bson_t cmd = BSON_INITIALIZER;
    if (!_create_markings_cmd_bson(ctx, &cmd)) {
        goto fail_create_cmd;
    }

    const char *cmd_name = ectx->cmd_name;

    if (!_add_dollar_db(cmd_name, &cmd, ectx->cmd_db, ctx->status)) {
        _mongocrypt_ctx_fail(ctx);
        goto fail_create_cmd;
    }

#define CHECK_CSFLE_ERROR(Func, FailLabel)                                                                             \
    if (1) {                                                                                                           \
        if (csfle.status_get_error(status)) {                                                                          \
            _mongocrypt_set_error(ctx->status,                                                                         \
                                  MONGOCRYPT_STATUS_ERROR_CRYPT_SHARED,                                                \
                                  MONGOCRYPT_GENERIC_ERROR_CODE,                                                       \
                                  "csfle " #Func " failed: %s [Error %d, code %d]",                                    \
                                  csfle.status_get_explanation(status),                                                \
                                  csfle.status_get_error(status),                                                      \
                                  csfle.status_get_code(status));                                                      \
            _mongocrypt_ctx_fail(ctx);                                                                                 \
            goto FailLabel;                                                                                            \
        }                                                                                                              \
    } else                                                                                                             \
        ((void)0)

    mongo_crypt_v1_status *status = csfle.status_create();
    BSON_ASSERT(status);

    mongo_crypt_v1_query_analyzer *qa = csfle.query_analyzer_create(csfle_lib, status);
    CHECK_CSFLE_ERROR("query_analyzer_create", fail_qa_create);

    uint32_t marked_bson_len = 0;
    uint8_t *marked_bson = csfle.analyze_query(qa,
                                               bson_get_data(&cmd),
                                               ectx->target_ns,
                                               (uint32_t)strlen(ectx->target_ns),
                                               &marked_bson_len,
                                               status);
    CHECK_CSFLE_ERROR("analyze_query", fail_analyze_query);

    // Copy out the marked document.
    mongocrypt_binary_t *marked = mongocrypt_binary_new_from_data(marked_bson, marked_bson_len);
    if (!_mongo_feed_markings(ctx, marked)) {
        // Wrap error with additional information.
        _mongocrypt_set_error(ctx->status,
                              MONGOCRYPT_STATUS_ERROR_CLIENT,
                              MONGOCRYPT_GENERIC_ERROR_CODE,
                              "Consuming the generated csfle markings failed: %s",
                              mongocrypt_status_message(ctx->status, NULL /* len */));
        goto fail_feed_markings;
    }

    okay = _mongo_done_markings(ctx);
    if (!okay) {
        // Wrap error with additional information.
        _mongocrypt_set_error(ctx->status,
                              MONGOCRYPT_STATUS_ERROR_CLIENT,
                              MONGOCRYPT_GENERIC_ERROR_CODE,
                              "Finalizing the generated csfle markings failed: %s",
                              mongocrypt_status_message(ctx->status, NULL /* len */));
    }

fail_feed_markings:
    mongocrypt_binary_destroy(marked);
    csfle.bson_free(marked_bson);
fail_analyze_query:
    csfle.query_analyzer_destroy(qa);
fail_qa_create:
    csfle.status_destroy(status);
fail_create_cmd:
    bson_destroy(&cmd);
    return okay;
}

static bool _mongocrypt_fle2_insert_update_find(mc_fle_blob_subtype_t subtype) {
    return (subtype == MC_SUBTYPE_FLE2InsertUpdatePayload) || (subtype == MC_SUBTYPE_FLE2InsertUpdatePayloadV2)
        || (subtype == MC_SUBTYPE_FLE2FindEqualityPayload) || (subtype == MC_SUBTYPE_FLE2FindEqualityPayloadV2)
        || (subtype == MC_SUBTYPE_FLE2FindRangePayload) || (subtype == MC_SUBTYPE_FLE2FindRangePayloadV2);
}

static bool
_marking_to_bson_value(void *ctx, _mongocrypt_marking_t *marking, bson_value_t *out, mongocrypt_status_t *status) {
    _mongocrypt_ciphertext_t ciphertext;
    _mongocrypt_buffer_t serialized_ciphertext = {0};
    bool ret = false;

    BSON_ASSERT_PARAM(ctx);
    BSON_ASSERT_PARAM(marking);
    BSON_ASSERT_PARAM(out);

    _mongocrypt_ciphertext_init(&ciphertext);

    if (!_mongocrypt_marking_to_ciphertext(ctx, marking, &ciphertext, status)) {
        goto fail;
    }

    if (_mongocrypt_fle2_insert_update_find(ciphertext.blob_subtype)) {
        /* ciphertext_data is already a BSON object, just need to prepend
         * blob_subtype */
        if (ciphertext.data.len > UINT32_MAX - 1u) {
            CLIENT_ERR("ciphertext too long");
            goto fail;
        }
        _mongocrypt_buffer_init_size(&serialized_ciphertext, ciphertext.data.len + 1);
        /* ciphertext->blob_subtype is an enum and easily fits in uint8_t */
        serialized_ciphertext.data[0] = (uint8_t)ciphertext.blob_subtype;
        memcpy(serialized_ciphertext.data + 1, ciphertext.data.data, ciphertext.data.len);

    } else if (!_mongocrypt_serialize_ciphertext(&ciphertext, &serialized_ciphertext)) {
        CLIENT_ERR("malformed ciphertext");
        goto fail;
    };

    /* ownership of serialized_ciphertext is transferred to caller. */
    out->value_type = BSON_TYPE_BINARY;
    out->value.v_binary.data = serialized_ciphertext.data;
    out->value.v_binary.data_len = serialized_ciphertext.len;
    out->value.v_binary.subtype = (bson_subtype_t)BSON_SUBTYPE_ENCRYPTED;

    ret = true;

fail:
    _mongocrypt_ciphertext_cleanup(&ciphertext);
    return ret;
}

static bool
_replace_marking_with_ciphertext(void *ctx, _mongocrypt_buffer_t *in, bson_value_t *out, mongocrypt_status_t *status) {
    _mongocrypt_marking_t marking = {0};
    bool ret;

    BSON_ASSERT_PARAM(ctx);
    BSON_ASSERT_PARAM(in);

    if (!_mongocrypt_marking_parse_unowned(in, &marking, status)) {
        _mongocrypt_marking_cleanup(&marking);
        return false;
    }

    ret = _marking_to_bson_value(ctx, &marking, out, status);
    _mongocrypt_marking_cleanup(&marking);
    return ret;
}

/* generate_delete_tokens generates the 'deleteTokens' document to be appended
 * to 'encryptionInformation'. */
static bson_t *generate_delete_tokens(_mongocrypt_crypto_t *crypto,
                                      _mongocrypt_key_broker_t *kb,
                                      mc_EncryptedFieldConfig_t *efc,
                                      mongocrypt_status_t *status) {
    bool ret = false;
    bson_t *out = bson_new();
    mc_EncryptedField_t *ef;

    BSON_ASSERT_PARAM(crypto);
    BSON_ASSERT_PARAM(kb);
    BSON_ASSERT_PARAM(efc);

    for (ef = efc->fields; ef != NULL; ef = ef->next) {
        _mongocrypt_buffer_t IndexKey = {0};
        _mongocrypt_buffer_t TokenKey = {0};
        mc_ServerDataEncryptionLevel1Token_t *sdel1t = NULL;
        mc_CollectionsLevel1Token_t *cl1t = NULL;
        mc_ECOCToken_t *ecoc = NULL;
        bool loop_ok = false;
        /* deleteTokens are only necessary for indexed fields. */
        if (!ef->supported_queries) {
            goto loop_continue;
        }

        if (!_mongocrypt_key_broker_decrypted_key_by_id(kb, &ef->keyId, &IndexKey)) {
            _mongocrypt_key_broker_status(kb, status);
            goto loop_fail;
        }

        /* Get the TokenKey from the last 32 bytes of IndexKey */
        if (IndexKey.len < MONGOCRYPT_TOKEN_KEY_LEN) {
            CLIENT_ERR("IndexKey too short");
            goto loop_fail;
        }
        if (!_mongocrypt_buffer_from_subrange(&TokenKey,
                                              &IndexKey,
                                              IndexKey.len - MONGOCRYPT_TOKEN_KEY_LEN,
                                              MONGOCRYPT_TOKEN_KEY_LEN)) {
            CLIENT_ERR("generate_delete_tokens unable to parse TokenKey from IndexKey");
            goto loop_fail;
        }

        sdel1t = mc_ServerDataEncryptionLevel1Token_new(crypto, &TokenKey, status);
        if (!sdel1t) {
            goto loop_fail;
        }

        cl1t = mc_CollectionsLevel1Token_new(crypto, &TokenKey, status);
        if (!cl1t) {
            goto loop_fail;
        }

        ecoc = mc_ECOCToken_new(crypto, cl1t, status);
        if (!ecoc) {
            goto loop_fail;
        }

        bson_t field_bson;
        if (!BSON_APPEND_DOCUMENT_BEGIN(out, ef->path, &field_bson)) {
            CLIENT_ERR("failed to begin document for 'deleteTokens.%s'", ef->path);
            goto loop_fail;
        }

        if (!BSON_APPEND_BINARY(&field_bson,
                                "e",
                                BSON_SUBTYPE_BINARY,
                                mc_ServerDataEncryptionLevel1Token_get(sdel1t)->data,
                                mc_ServerDataEncryptionLevel1Token_get(sdel1t)->len)) {
            CLIENT_ERR("failed to append ServerDataEncryptionLevel1Token for %s", ef->path);
            goto loop_fail;
        }

        if (!BSON_APPEND_BINARY(&field_bson,
                                "o",
                                BSON_SUBTYPE_BINARY,
                                mc_ECOCToken_get(ecoc)->data,
                                mc_ECOCToken_get(ecoc)->len)) {
            CLIENT_ERR("failed to append ECOCToken for %s", ef->path);
            goto loop_fail;
        }

        if (!bson_append_document_end(out, &field_bson)) {
            CLIENT_ERR("failed to end document for 'deleteTokens.%s'", ef->path);
            goto loop_fail;
        }

    loop_continue:
        loop_ok = true;
    loop_fail:
        _mongocrypt_buffer_cleanup(&IndexKey);
        _mongocrypt_buffer_cleanup(&TokenKey);
        mc_ServerDataEncryptionLevel1Token_destroy(sdel1t);
        mc_CollectionsLevel1Token_destroy(cl1t);
        mc_ECOCToken_destroy(ecoc);
        if (!loop_ok) {
            goto fail;
        }
    }

    ret = true;
fail:
    if (!ret) {
        bson_destroy(out);
        return NULL;
    }
    return out;
}

static bool
_check_for_payload_requiring_encryptionInformation(void *ctx, _mongocrypt_buffer_t *in, mongocrypt_status_t *status) {
    bool *out = (bool *)ctx;

    BSON_ASSERT_PARAM(ctx);
    BSON_ASSERT_PARAM(in);

    if (in->len < 1) {
        CLIENT_ERR("unexpected empty FLE payload");
        return false;
    }

    mc_fle_blob_subtype_t subtype = (mc_fle_blob_subtype_t)in->data[0];
    if (_mongocrypt_fle2_insert_update_find(subtype)) {
        *out = true;
        return true;
    }

    return true;
}

typedef struct {
    bool must_omit;
    bool ok;
} moe_result;

// must_omit_encryptionInformation returns true if the command
// must omit the "encryptionInformation" field when sent to mongod / mongos.
static moe_result must_omit_encryptionInformation(const char *command_name,
                                                  const bson_t *command,
                                                  bool use_range_v2,
                                                  const mc_EncryptedFieldConfig_t *efc,
                                                  mongocrypt_status_t *status) {
    // eligible_commands may omit encryptionInformation if the command does not
    // contain payloads requiring encryption.
    const char *eligible_commands[] = {"find", "aggregate", "distinct", "count", "insert"};
    size_t i;
    bool found = false;

    // prohibited_commands prohibit encryptionInformation on mongod / mongos.
    const char *prohibited_commands[] = {"cleanupStructuredEncryptionData", "create", "collMod", "createIndexes"};

    BSON_ASSERT_PARAM(command_name);
    BSON_ASSERT_PARAM(command);
    BSON_ASSERT_PARAM(efc);

    if (0 == strcmp("compactStructuredEncryptionData", command_name)) {
        // `compactStructuredEncryptionData` is a special case:
        // - Server 7.0 prohibits `encryptionInformation`.
        // - Server 8.0 requires `encryptionInformation` if "range" fields are referenced. Otherwise ignores.
        // Only send `encryptionInformation` if "range" fields are present to support both server versions.
        bool uses_range_fields = false;
        if (use_range_v2) {
            for (const mc_EncryptedField_t *ef = efc->fields; ef != NULL; ef = ef->next) {
                if (ef->supported_queries & SUPPORTS_RANGE_QUERIES) {
                    uses_range_fields = true;
                    break;
                }
            }
        }
        return (moe_result){.ok = true, .must_omit = !uses_range_fields};
    }

    for (i = 0; i < sizeof(prohibited_commands) / sizeof(prohibited_commands[0]); i++) {
        if (0 == strcmp(prohibited_commands[i], command_name)) {
            return (moe_result){.ok = true, .must_omit = true};
        }
    }

    for (i = 0; i < sizeof(eligible_commands) / sizeof(eligible_commands[0]); i++) {
        if (0 == strcmp(eligible_commands[i], command_name)) {
            found = true;
            break;
        }
    }
    if (!found) {
        return (moe_result){.ok = true};
    }

    bool has_payload_requiring_encryptionInformation = false;
    bson_iter_t iter = {0};
    if (!bson_iter_init(&iter, command)) {
        CLIENT_ERR("unable to iterate command");
        return (moe_result){.ok = false};
    }
    if (!_mongocrypt_traverse_binary_in_bson(_check_for_payload_requiring_encryptionInformation,
                                             &has_payload_requiring_encryptionInformation,
                                             TRAVERSE_MATCH_SUBTYPE6,
                                             &iter,
                                             status)) {
        return (moe_result){.ok = false};
    }

    if (!has_payload_requiring_encryptionInformation) {
        return (moe_result){.ok = true, .must_omit = true};
    }
    return (moe_result){.ok = true, .must_omit = false};
}

/* _fle2_append_compactionTokens appends compactionTokens if command_name is
 * "compactStructuredEncryptionData" or cleanupTokens if command_name is
 * "cleanupStructuredEncryptionData"
 */
static bool _fle2_append_compactionTokens(mongocrypt_t *crypt,
                                          _mongocrypt_key_broker_t *kb,
                                          mc_EncryptedFieldConfig_t *efc,
                                          const char *command_name,
                                          bson_t *out,
                                          mongocrypt_status_t *status) {
    bson_t result_compactionTokens = BSON_INITIALIZER;
    bool ret = false;

    BSON_ASSERT_PARAM(crypt);
    BSON_ASSERT_PARAM(kb);
    BSON_ASSERT_PARAM(efc);
    BSON_ASSERT_PARAM(command_name);
    BSON_ASSERT_PARAM(out);
    _mongocrypt_crypto_t *crypto = crypt->crypto;

    bool cleanup = (0 == strcmp(command_name, "cleanupStructuredEncryptionData"));

    if (0 != strcmp(command_name, "compactStructuredEncryptionData") && !cleanup) {
        return true;
    }

    if (cleanup) {
        BSON_APPEND_DOCUMENT_BEGIN(out, "cleanupTokens", &result_compactionTokens);
    } else {
        BSON_APPEND_DOCUMENT_BEGIN(out, "compactionTokens", &result_compactionTokens);
    }

    mc_EncryptedField_t *ptr;
    for (ptr = efc->fields; ptr != NULL; ptr = ptr->next) {
        /* Append tokens. */
        _mongocrypt_buffer_t key = {0};
        _mongocrypt_buffer_t tokenkey = {0};
        mc_CollectionsLevel1Token_t *cl1t = NULL;
        mc_ECOCToken_t *ecoct = NULL;
        mc_ESCToken_t *esct = NULL;
        mc_AnchorPaddingTokenRoot_t *padt = NULL;
        bool ecoc_ok = false;

        if (!_mongocrypt_key_broker_decrypted_key_by_id(kb, &ptr->keyId, &key)) {
            _mongocrypt_key_broker_status(kb, status);
            goto ecoc_fail;
        }
        /* The last 32 bytes of the user key are the token key. */
        if (key.len < MONGOCRYPT_TOKEN_KEY_LEN) {
            CLIENT_ERR("key too short");
            goto ecoc_fail;
        }
        if (!_mongocrypt_buffer_from_subrange(&tokenkey,
                                              &key,
                                              key.len - MONGOCRYPT_TOKEN_KEY_LEN,
                                              MONGOCRYPT_TOKEN_KEY_LEN)) {
            CLIENT_ERR("unable to get TokenKey from Data Encryption Key");
            goto ecoc_fail;
        }
        cl1t = mc_CollectionsLevel1Token_new(crypto, &tokenkey, status);
        if (!cl1t) {
            goto ecoc_fail;
        }

        ecoct = mc_ECOCToken_new(crypto, cl1t, status);
        if (!ecoct) {
            goto ecoc_fail;
        }

        const _mongocrypt_buffer_t *ecoct_buf = mc_ECOCToken_get(ecoct);

        if (crypt->opts.use_range_v2 && (ptr->supported_queries & SUPPORTS_RANGE_QUERIES)) {
            // Append the document {ecoc: <ECOCToken>, anchorPaddingToken: <AnchorPaddingTokenRoot>}
            esct = mc_ESCToken_new(crypto, cl1t, status);
            if (!esct) {
                goto ecoc_fail;
            }
            padt = mc_AnchorPaddingTokenRoot_new(crypto, esct, status);
            if (!padt) {
                goto ecoc_fail;
            }
            const _mongocrypt_buffer_t *padt_buf = mc_AnchorPaddingTokenRoot_get(padt);
            bson_t tokenDoc;
            BSON_APPEND_DOCUMENT_BEGIN(&result_compactionTokens, ptr->path, &tokenDoc);
            BSON_APPEND_BINARY(&tokenDoc, "ecoc", BSON_SUBTYPE_BINARY, ecoct_buf->data, ecoct_buf->len);
            BSON_APPEND_BINARY(&tokenDoc, "anchorPaddingToken", BSON_SUBTYPE_BINARY, padt_buf->data, padt_buf->len);
            bson_append_document_end(&result_compactionTokens, &tokenDoc);
        } else {
            // Append just <ECOCToken>
            BSON_APPEND_BINARY(&result_compactionTokens,
                               ptr->path,
                               BSON_SUBTYPE_BINARY,
                               ecoct_buf->data,
                               ecoct_buf->len);
        }

        ecoc_ok = true;
    ecoc_fail:
        mc_AnchorPaddingTokenRoot_destroy(padt);
        mc_ESCToken_destroy(esct);
        mc_ECOCToken_destroy(ecoct);
        mc_CollectionsLevel1Token_destroy(cl1t);
        _mongocrypt_buffer_cleanup(&key);
        if (!ecoc_ok) {
            goto fail;
        }
    }

    bson_append_document_end(out, &result_compactionTokens);

    ret = true;
fail:
    return ret;
}

/**
 * @brief Removes "encryptionInformation" from cmd.
 */
static bool
_fle2_strip_encryptionInformation(const char *cmd_name, bson_t *cmd /* in and out */, mongocrypt_status_t *status) {
    bson_t stripped = BSON_INITIALIZER;
    bool ok = false;

    BSON_ASSERT_PARAM(cmd_name);
    BSON_ASSERT_PARAM(cmd);

    if (0 != strcmp(cmd_name, "explain") && 0 != strcmp(cmd_name, "bulkWrite")) {
        bson_copy_to_excluding_noinit(cmd, &stripped, "encryptionInformation", NULL);
        goto success;
    }

    if (0 == strcmp(cmd_name, "bulkWrite")) {
        // Get the single `nsInfo` document from the input command.
        bson_t nsInfo; // Non-owning.
        {
            bson_iter_t nsInfo_iter;
            if (!bson_iter_init(&nsInfo_iter, cmd)) {
                CLIENT_ERR("failed to iterate command");
                goto fail;
            }
            if (!bson_iter_find_descendant(&nsInfo_iter, "nsInfo.0", &nsInfo_iter)) {
                CLIENT_ERR("expected one namespace in `bulkWrite`, but found zero.");
                goto fail;
            }
            if (bson_has_field(cmd, "nsInfo.1")) {
                CLIENT_ERR(
                    "expected one namespace in `bulkWrite`, but found more than one. Only one namespace is supported.");
                goto fail;
            }
            if (!mc_iter_document_as_bson(&nsInfo_iter, &nsInfo, status)) {
                goto fail;
            }
        }

        // Copy input and exclude `encryptionInformation` from `nsInfo`.
        {
            // Append everything from input except `nsInfo`.
            bson_copy_to_excluding_noinit(cmd, &stripped, "nsInfo", NULL);
            // Append `nsInfo` array.
            bson_t nsInfo_array;
            if (!BSON_APPEND_ARRAY_BEGIN(&stripped, "nsInfo", &nsInfo_array)) {
                CLIENT_ERR("unable to begin appending 'nsInfo' array");
                goto fail;
            }
            bson_t nsInfo_array_0;
            if (!BSON_APPEND_DOCUMENT_BEGIN(&nsInfo_array, "0", &nsInfo_array_0)) {
                CLIENT_ERR("unable to append 'nsInfo.0' document");
                goto fail;
            }
            // Copy everything from input `nsInfo` and exclude `encryptionInformation`.
            bson_copy_to_excluding_noinit(&nsInfo, &nsInfo_array_0, "encryptionInformation", NULL);
            if (!bson_append_document_end(&nsInfo_array, &nsInfo_array_0)) {
                CLIENT_ERR("unable to end appending 'nsInfo' document in array");
            }
            if (!bson_append_array_end(&stripped, &nsInfo_array)) {
                CLIENT_ERR("unable to end appending 'nsInfo' array");
                goto fail;
            }
        }

        goto success;
    }

    // The 'explain' command is a special case.
    // 'encryptionInformation' is returned from mongocryptd and csfle nested
    // inside 'explain'. Example:
    // {
    //    "explain": {
    //       "find": "coll"
    //       "encryptionInformation": {}
    //    }
    // }
    bson_iter_t iter;
    bson_t explain;

    BSON_ASSERT(bson_iter_init_find(&iter, cmd, "explain"));
    if (!BSON_ITER_HOLDS_DOCUMENT(&iter)) {
        CLIENT_ERR("expected 'explain' to be document");
        goto fail;
    }

    {
        bson_t tmp;
        if (!mc_iter_document_as_bson(&iter, &tmp, status)) {
            goto fail;
        }
        bson_init(&explain);
        bson_copy_to_excluding_noinit(&tmp, &explain, "encryptionInformation", NULL);
    }

    if (!BSON_APPEND_DOCUMENT(&stripped, "explain", &explain)) {
        bson_destroy(&explain);
        CLIENT_ERR("unable to append 'explain'");
        goto fail;
    }
    bson_destroy(&explain);
    bson_copy_to_excluding_noinit(cmd, &stripped, "explain", NULL);

success:
    bson_destroy(cmd);
    if (!bson_steal(cmd, &stripped)) {
        CLIENT_ERR("failed to steal BSON without encryptionInformation");
        goto fail;
    }
    ok = true;
fail:
    if (!ok) {
        bson_destroy(&stripped);
    }
    return ok;
}

/* Process a call to mongocrypt_ctx_finalize when an encryptedFieldConfig is
 * associated with the command. */
static bool _fle2_finalize(mongocrypt_ctx_t *ctx, mongocrypt_binary_t *out) {
    bson_t converted;
    _mongocrypt_ctx_encrypt_t *ectx;
    bson_t encrypted_field_config_bson;
    bson_t original_cmd_bson;

    BSON_ASSERT_PARAM(ctx);
    BSON_ASSERT_PARAM(out);

    ectx = (_mongocrypt_ctx_encrypt_t *)ctx;

    BSON_ASSERT(context_uses_fle2(ctx));
    BSON_ASSERT(ctx->state == MONGOCRYPT_CTX_READY);

    if (ectx->explicit) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "explicit encryption is not yet supported. See MONGOCRYPT-409.");
    }

    if (!_mongocrypt_buffer_to_bson(&ectx->encrypted_field_config, &encrypted_field_config_bson)) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "malformed bson in encrypted_field_config_bson");
    }

    if (!_mongocrypt_buffer_to_bson(&ectx->original_cmd, &original_cmd_bson)) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "malformed bson in original_cmd");
    }

    /* If marked_cmd buffer is empty, there are no markings to encrypt. */
    if (_mongocrypt_buffer_empty(&ectx->marked_cmd)) {
        /* Append 'encryptionInformation' to the original command. */
        bson_copy_to(&original_cmd_bson, &converted);
    } else {
        bson_t as_bson;
        bson_iter_t iter = {0};

        if (!_mongocrypt_buffer_to_bson(&ectx->marked_cmd, &as_bson)) {
            return _mongocrypt_ctx_fail_w_msg(ctx, "malformed bson");
        }

        bson_iter_init(&iter, &as_bson);
        bson_init(&converted);
        if (!_mongocrypt_transform_binary_in_bson(_replace_marking_with_ciphertext,
                                                  &ctx->kb,
                                                  TRAVERSE_MATCH_MARKING,
                                                  &iter,
                                                  &converted,
                                                  ctx->status)) {
            bson_destroy(&converted);
            return _mongocrypt_ctx_fail(ctx);
        }
    }

    const char *command_name = ectx->cmd_name;

    /* Remove the 'encryptionInformation' field. It is appended in the response
     * from mongocryptd or csfle. */
    if (!_fle2_strip_encryptionInformation(command_name, &converted, ctx->status)) {
        bson_destroy(&converted);
        return _mongocrypt_ctx_fail(ctx);
    }

    bson_t *deleteTokens = NULL;
    if (command_needs_deleteTokens(ctx, command_name)) {
        deleteTokens = generate_delete_tokens(ctx->crypt->crypto, &ctx->kb, &ectx->efc, ctx->status);
        if (!deleteTokens) {
            bson_destroy(&converted);
            return _mongocrypt_ctx_fail(ctx);
        }
    }

    moe_result result = must_omit_encryptionInformation(command_name,
                                                        &converted,
                                                        ctx->crypt->opts.use_range_v2,
                                                        &ectx->efc,
                                                        ctx->status);
    if (!result.ok) {
        bson_destroy(&converted);
        bson_destroy(deleteTokens);
        return _mongocrypt_ctx_fail(ctx);
    }

    /* Append a new 'encryptionInformation'. */
    if (!result.must_omit && !ectx->used_empty_encryptedFields) {
        if (!_fle2_insert_encryptionInformation(ctx,
                                                command_name,
                                                &converted,
                                                ectx->target_ns,
                                                &encrypted_field_config_bson,
                                                deleteTokens,
                                                ectx->target_coll,
                                                MC_TO_MONGOD,
                                                ctx->status)) {
            bson_destroy(&converted);
            bson_destroy(deleteTokens);
            return _mongocrypt_ctx_fail(ctx);
        }
    }
    bson_destroy(deleteTokens);

    if (!_fle2_append_compactionTokens(ctx->crypt, &ctx->kb, &ectx->efc, command_name, &converted, ctx->status)) {
        bson_destroy(&converted);
        return _mongocrypt_ctx_fail(ctx);
    }

    // If input command has $db, ensure output command has $db.
    bson_iter_t iter;
    if (bson_iter_init_find(&iter, &original_cmd_bson, "$db")) {
        if (!bson_iter_init_find(&iter, &converted, "$db")) {
            BSON_APPEND_UTF8(&converted, "$db", ectx->cmd_db);
        }
    }

    _mongocrypt_buffer_steal_from_bson(&ectx->encrypted_cmd, &converted);
    _mongocrypt_buffer_to_binary(&ectx->encrypted_cmd, out);
    ctx->state = MONGOCRYPT_CTX_DONE;

    return true;
}

static bool FLE2RangeFindDriverSpec_to_ciphertexts(mongocrypt_ctx_t *ctx, mongocrypt_binary_t *out) {
    bool ok = false;
    _mongocrypt_ctx_encrypt_t *ectx = (_mongocrypt_ctx_encrypt_t *)ctx;

    BSON_ASSERT_PARAM(ctx);
    BSON_ASSERT_PARAM(out);

    bson_t with_placholders = BSON_INITIALIZER;
    bson_t with_ciphertexts = BSON_INITIALIZER;

    if (!ctx->opts.rangeopts.set) {
        _mongocrypt_ctx_fail_w_msg(ctx, "Expected RangeOpts to be set for Range Find");
        goto fail;
    }
    if (!ctx->opts.contention_factor.set) {
        _mongocrypt_ctx_fail_w_msg(ctx, "Expected Contention Factor to be set for Range Find");
        goto fail;
    }

    bson_t in_bson;
    if (!_mongocrypt_buffer_to_bson(&ectx->original_cmd, &in_bson)) {
        _mongocrypt_ctx_fail_w_msg(ctx, "unable to convert input to BSON");
        goto fail;
    }

    bson_t v_doc;
    // Parse 'v' document from input.
    {
        bson_iter_t v_iter;
        if (!bson_iter_init_find(&v_iter, &in_bson, "v")) {
            _mongocrypt_ctx_fail_w_msg(ctx, "invalid input BSON, must contain 'v'");
            goto fail;
        }
        if (!BSON_ITER_HOLDS_DOCUMENT(&v_iter)) {
            _mongocrypt_ctx_fail_w_msg(ctx, "invalid input BSON, expected 'v' to be document");
            goto fail;
        }
        if (!mc_iter_document_as_bson(&v_iter, &v_doc, ctx->status)) {
            _mongocrypt_ctx_fail(ctx);
            goto fail;
        }
    }

    // Parse FLE2RangeFindDriverSpec.
    {
        mc_FLE2RangeFindDriverSpec_t rfds;

        if (!mc_FLE2RangeFindDriverSpec_parse(&rfds, &v_doc, ctx->status)) {
            _mongocrypt_ctx_fail(ctx);
            goto fail;
        }

        // Convert FLE2RangeFindDriverSpec into a document with placeholders.
        if (!mc_FLE2RangeFindDriverSpec_to_placeholders(
                &rfds,
                &ctx->opts.rangeopts.value,
                ctx->opts.contention_factor.value,
                &ctx->opts.key_id,
                _mongocrypt_buffer_empty(&ctx->opts.index_key_id) ? &ctx->opts.key_id : &ctx->opts.index_key_id,
                mc_getNextPayloadId(),
                &with_placholders,
                ctx->status)) {
            _mongocrypt_ctx_fail(ctx);
            goto fail;
        }
    }

    // Convert document with placeholders into document with ciphertexts.
    {
        bson_iter_t iter;
        if (!bson_iter_init(&iter, &with_placholders)) {
            _mongocrypt_ctx_fail_w_msg(ctx, "unable to iterate into placeholder document");
            goto fail;
        }
        if (!_mongocrypt_transform_binary_in_bson(_replace_marking_with_ciphertext,
                                                  &ctx->kb,
                                                  TRAVERSE_MATCH_MARKING,
                                                  &iter,
                                                  &with_ciphertexts,
                                                  ctx->status)) {
            goto fail;
        }
    }

    // Wrap result in the document: { 'v': <result> }.
    {
        /* v_wrapped is the BSON document { 'v': <v_out> }. */
        bson_t v_wrapped = BSON_INITIALIZER;
        if (!bson_append_document(&v_wrapped, MONGOCRYPT_STR_AND_LEN("v"), &with_ciphertexts)) {
            _mongocrypt_ctx_fail_w_msg(ctx, "unable to append document to 'v'");
            goto fail;
        }
        _mongocrypt_buffer_steal_from_bson(&ectx->encrypted_cmd, &v_wrapped);
        _mongocrypt_buffer_to_binary(&ectx->encrypted_cmd, out);
        ctx->state = MONGOCRYPT_CTX_DONE;
    }

    ok = true;
fail:
    bson_destroy(&with_ciphertexts);
    bson_destroy(&with_placholders);
    return ok;
}

static bool _fle2_finalize_explicit(mongocrypt_ctx_t *ctx, mongocrypt_binary_t *out) {
    bool ret = false;
    _mongocrypt_marking_t marking;
    _mongocrypt_ctx_encrypt_t *ectx = (_mongocrypt_ctx_encrypt_t *)ctx;

    BSON_ASSERT_PARAM(ctx);
    BSON_ASSERT_PARAM(out);

    BSON_ASSERT(ctx->opts.index_type.set);

    if (ctx->opts.rangeopts.set && ctx->opts.query_type.set) {
        // RangeOpts with query type is a special case. The result contains two
        // ciphertext values.
        return FLE2RangeFindDriverSpec_to_ciphertexts(ctx, out);
    }

    bson_t new_v = BSON_INITIALIZER;

    _mongocrypt_marking_init(&marking);
    marking.type = MONGOCRYPT_MARKING_FLE2_ENCRYPTION;
    if (ctx->opts.query_type.set) {
        switch (ctx->opts.query_type.value) {
        case MONGOCRYPT_QUERY_TYPE_RANGEPREVIEW_DEPRECATED:
            if (ctx->crypt->opts.use_range_v2) {
                _mongocrypt_ctx_fail_w_msg(ctx, "Cannot use rangePreview query type with Range V2");
                goto fail;
            }
        // fallthrough
        case MONGOCRYPT_QUERY_TYPE_RANGE:
        case MONGOCRYPT_QUERY_TYPE_EQUALITY: marking.fle2.type = MONGOCRYPT_FLE2_PLACEHOLDER_TYPE_FIND; break;
        default: _mongocrypt_ctx_fail_w_msg(ctx, "Invalid value for EncryptOpts.queryType"); goto fail;
        }
    } else {
        marking.fle2.type = MONGOCRYPT_FLE2_PLACEHOLDER_TYPE_INSERT;
    }

    switch (ctx->opts.index_type.value) {
    case MONGOCRYPT_INDEX_TYPE_EQUALITY: marking.fle2.algorithm = MONGOCRYPT_FLE2_ALGORITHM_EQUALITY; break;
    case MONGOCRYPT_INDEX_TYPE_NONE: marking.fle2.algorithm = MONGOCRYPT_FLE2_ALGORITHM_UNINDEXED; break;
    case MONGOCRYPT_INDEX_TYPE_RANGEPREVIEW_DEPRECATED:
        if (ctx->crypt->opts.use_range_v2) {
            _mongocrypt_ctx_fail_w_msg(ctx, "Cannot use rangePreview index type with Range V2");
            goto fail;
        }
        // fallthrough
    case MONGOCRYPT_INDEX_TYPE_RANGE: marking.fle2.algorithm = MONGOCRYPT_FLE2_ALGORITHM_RANGE; break;
    default:
        // This might be unreachable because of other validation. Better safe than
        // sorry.
        _mongocrypt_ctx_fail_w_msg(ctx, "Invalid value for EncryptOpts.indexType");
        goto fail;
    }

    if (ctx->opts.rangeopts.set) {
        // Process the RangeOpts and the input 'v' document into a new 'v'.
        // The new 'v' document will be a FLE2RangeFindSpec or
        // FLE2RangeInsertSpec.
        bson_t old_v;

        if (!_mongocrypt_buffer_to_bson(&ectx->original_cmd, &old_v)) {
            _mongocrypt_ctx_fail_w_msg(ctx, "unable to convert input to BSON");
            goto fail;
        }

        // RangeOpts with query_type is handled above.
        BSON_ASSERT(!ctx->opts.query_type.set);
        if (!mc_RangeOpts_to_FLE2RangeInsertSpec(&ctx->opts.rangeopts.value,
                                                 &old_v,
                                                 &new_v,
                                                 ctx->crypt->opts.use_range_v2,
                                                 ctx->status)) {
            _mongocrypt_ctx_fail(ctx);
            goto fail;
        }

        if (!bson_iter_init_find(&marking.v_iter, &new_v, "v")) {
            _mongocrypt_ctx_fail_w_msg(ctx, "invalid input BSON, must contain 'v'");
            goto fail;
        }

        marking.fle2.sparsity = ctx->opts.rangeopts.value.sparsity;

    } else {
        bson_t as_bson;

        /* Get iterator to input 'v' BSON value. */
        if (!_mongocrypt_buffer_to_bson(&ectx->original_cmd, &as_bson)) {
            _mongocrypt_ctx_fail_w_msg(ctx, "unable to convert input to BSON");
            goto fail;
        }

        if (!bson_iter_init_find(&marking.v_iter, &as_bson, "v")) {
            _mongocrypt_ctx_fail_w_msg(ctx, "invalid input BSON, must contain 'v'");
            goto fail;
        }
    }

    _mongocrypt_buffer_copy_to(&ctx->opts.key_id, &marking.fle2.user_key_id);
    if (!_mongocrypt_buffer_empty(&ctx->opts.index_key_id)) {
        _mongocrypt_buffer_copy_to(&ctx->opts.index_key_id, &marking.fle2.index_key_id);
    } else {
        _mongocrypt_buffer_copy_to(&ctx->opts.key_id, &marking.fle2.index_key_id);
    }

    if (ctx->opts.contention_factor.set) {
        marking.fle2.maxContentionFactor = ctx->opts.contention_factor.value;
    } else if (ctx->opts.index_type.value == MONGOCRYPT_INDEX_TYPE_EQUALITY) {
        _mongocrypt_ctx_fail_w_msg(ctx, "contention factor required for indexed algorithm");
        goto fail;
    }

    /* Convert marking to ciphertext. */
    {
        bson_value_t v_out;
        /* v_wrapped is the BSON document { 'v': <v_out> }. */
        bson_t v_wrapped = BSON_INITIALIZER;

        if (!_marking_to_bson_value(&ctx->kb, &marking, &v_out, ctx->status)) {
            bson_destroy(&v_wrapped);
            _mongocrypt_ctx_fail(ctx);
            goto fail;
        }

        bson_append_value(&v_wrapped, MONGOCRYPT_STR_AND_LEN("v"), &v_out);
        _mongocrypt_buffer_steal_from_bson(&ectx->encrypted_cmd, &v_wrapped);
        _mongocrypt_buffer_to_binary(&ectx->encrypted_cmd, out);
        ctx->state = MONGOCRYPT_CTX_DONE;
        bson_value_destroy(&v_out);
    }

    ret = true;
fail:
    bson_destroy(&new_v);
    _mongocrypt_marking_cleanup(&marking);
    return ret;
}

static bool _finalize(mongocrypt_ctx_t *ctx, mongocrypt_binary_t *out) {
    bson_t as_bson, converted;
    bson_iter_t iter = {0};
    _mongocrypt_ctx_encrypt_t *ectx;
    bool res;

    BSON_ASSERT_PARAM(ctx);
    BSON_ASSERT_PARAM(out);

    ectx = (_mongocrypt_ctx_encrypt_t *)ctx;

    if (context_uses_fle2(ctx)) {
        return _fle2_finalize(ctx, out);
    } else if (ctx->opts.index_type.set) {
        return _fle2_finalize_explicit(ctx, out);
    }

    if (!ectx->explicit) {
        if (ctx->nothing_to_do) {
            _mongocrypt_buffer_to_binary(&ectx->original_cmd, out);
            ctx->state = MONGOCRYPT_CTX_DONE;
            return true;
        }
        if (!_mongocrypt_buffer_to_bson(&ectx->marked_cmd, &as_bson)) {
            return _mongocrypt_ctx_fail_w_msg(ctx, "malformed bson");
        }

        bson_iter_init(&iter, &as_bson);
        bson_init(&converted);
        if (!_mongocrypt_transform_binary_in_bson(_replace_marking_with_ciphertext,
                                                  &ctx->kb,
                                                  TRAVERSE_MATCH_MARKING,
                                                  &iter,
                                                  &converted,
                                                  ctx->status)) {
            bson_destroy(&converted);
            return _mongocrypt_ctx_fail(ctx);
        }

        bson_t original_cmd_bson;
        if (!_mongocrypt_buffer_to_bson(&ectx->original_cmd, &original_cmd_bson)) {
            bson_destroy(&converted);
            return _mongocrypt_ctx_fail_w_msg(ctx, "malformed bson in original_cmd");
        }

        // If input command has $db, ensure output command has $db.
        bson_iter_t iter;
        if (bson_iter_init_find(&iter, &original_cmd_bson, "$db")) {
            if (!bson_iter_init_find(&iter, &converted, "$db")) {
                BSON_APPEND_UTF8(&converted, "$db", ectx->cmd_db);
            }
        }
    } else {
        /* For explicit encryption, we have no marking, but we can fake one */
        _mongocrypt_marking_t marking;
        bson_value_t value;

        memset(&value, 0, sizeof(value));

        _mongocrypt_marking_init(&marking);

        if (!_mongocrypt_buffer_to_bson(&ectx->original_cmd, &as_bson)) {
            return _mongocrypt_ctx_fail_w_msg(ctx, "malformed bson");
        }

        if (!bson_iter_init_find(&iter, &as_bson, "v")) {
            return _mongocrypt_ctx_fail_w_msg(ctx, "invalid msg, must contain 'v'");
        }

        memcpy(&marking.v_iter, &iter, sizeof(bson_iter_t));
        marking.algorithm = ctx->opts.algorithm;
        _mongocrypt_buffer_set_to(&ctx->opts.key_id, &marking.key_id);
        if (ctx->opts.key_alt_names) {
            bson_value_copy(&ctx->opts.key_alt_names->value, &marking.key_alt_name);
            marking.type = MONGOCRYPT_MARKING_FLE1_BY_ALTNAME;
        }

        bson_init(&converted);
        res = _marking_to_bson_value(&ctx->kb, &marking, &value, ctx->status);
        if (res) {
            bson_append_value(&converted, MONGOCRYPT_STR_AND_LEN("v"), &value);
        }

        bson_value_destroy(&value);
        _mongocrypt_marking_cleanup(&marking);

        if (!res) {
            bson_destroy(&converted);
            return _mongocrypt_ctx_fail(ctx);
        }
    }

    _mongocrypt_buffer_steal_from_bson(&ectx->encrypted_cmd, &converted);
    _mongocrypt_buffer_to_binary(&ectx->encrypted_cmd, out);
    ctx->state = MONGOCRYPT_CTX_DONE;

    return true;
}

static void _cleanup(mongocrypt_ctx_t *ctx) {
    _mongocrypt_ctx_encrypt_t *ectx;

    if (!ctx) {
        return;
    }

    ectx = (_mongocrypt_ctx_encrypt_t *)ctx;
    bson_free(ectx->target_ns);
    bson_free(ectx->cmd_db);
    bson_free(ectx->target_db);
    bson_free(ectx->target_coll);
    _mongocrypt_buffer_cleanup(&ectx->list_collections_filter);
    _mongocrypt_buffer_cleanup(&ectx->schema);
    _mongocrypt_buffer_cleanup(&ectx->encrypted_field_config);
    _mongocrypt_buffer_cleanup(&ectx->original_cmd);
    _mongocrypt_buffer_cleanup(&ectx->mongocryptd_cmd);
    _mongocrypt_buffer_cleanup(&ectx->marked_cmd);
    _mongocrypt_buffer_cleanup(&ectx->encrypted_cmd);
    _mongocrypt_buffer_cleanup(&ectx->ismaster.cmd);
    mc_EncryptedFieldConfig_cleanup(&ectx->efc);
}

static bool _try_schema_from_schema_map(mongocrypt_ctx_t *ctx) {
    mongocrypt_t *crypt;
    _mongocrypt_ctx_encrypt_t *ectx;
    bson_t schema_map;
    bson_iter_t iter;

    BSON_ASSERT_PARAM(ctx);

    crypt = ctx->crypt;
    ectx = (_mongocrypt_ctx_encrypt_t *)ctx;

    if (_mongocrypt_buffer_empty(&crypt->opts.schema_map)) {
        /* No schema map set. */
        return true;
    }

    if (!_mongocrypt_buffer_to_bson(&crypt->opts.schema_map, &schema_map)) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "malformed schema map");
    }

    if (bson_iter_init_find(&iter, &schema_map, ectx->target_ns)) {
        if (!_mongocrypt_buffer_copy_from_document_iter(&ectx->schema, &iter)) {
            return _mongocrypt_ctx_fail_w_msg(ctx, "malformed schema map");
        }
        ectx->used_local_schema = true;
        ctx->state = MONGOCRYPT_CTX_NEED_MONGO_MARKINGS;
    }

    /* No schema found in map. */
    return true;
}

/* Check if the local encrypted field config map has an entry for this
 * collection.
 * If an encrypted field config is found, the context transitions to
 * MONGOCRYPT_CTX_NEED_MONGO_MARKINGS. */
static bool _fle2_try_encrypted_field_config_from_map(mongocrypt_ctx_t *ctx) {
    mongocrypt_t *crypt;
    _mongocrypt_ctx_encrypt_t *ectx;
    bson_t encrypted_field_config_map;
    bson_iter_t iter;

    BSON_ASSERT_PARAM(ctx);

    crypt = ctx->crypt;
    ectx = (_mongocrypt_ctx_encrypt_t *)ctx;

    if (_mongocrypt_buffer_empty(&crypt->opts.encrypted_field_config_map)) {
        /* No encrypted_field_config_map set. */
        return true;
    }

    if (!_mongocrypt_buffer_to_bson(&crypt->opts.encrypted_field_config_map, &encrypted_field_config_map)) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "unable to convert encrypted_field_config_map to BSON");
    }

    if (bson_iter_init_find(&iter, &encrypted_field_config_map, ectx->target_ns)) {
        if (!_mongocrypt_buffer_copy_from_document_iter(&ectx->encrypted_field_config, &iter)) {
            return _mongocrypt_ctx_fail_w_msg(ctx,
                                              "unable to copy encrypted_field_config from "
                                              "encrypted_field_config_map");
        }
        bson_t efc_bson;
        if (!_mongocrypt_buffer_to_bson(&ectx->encrypted_field_config, &efc_bson)) {
            return _mongocrypt_ctx_fail_w_msg(ctx, "unable to create BSON from encrypted_field_config");
        }
        if (!mc_EncryptedFieldConfig_parse(&ectx->efc, &efc_bson, ctx->status, ctx->crypt->opts.use_range_v2)) {
            _mongocrypt_ctx_fail(ctx);
            return false;
        }
        ctx->state = MONGOCRYPT_CTX_NEED_MONGO_MARKINGS;
    }

    /* No encrypted_field_config found in map. */
    return true;
}

static bool _try_schema_from_cache(mongocrypt_ctx_t *ctx) {
    _mongocrypt_ctx_encrypt_t *ectx;
    bson_t *collinfo = NULL;

    BSON_ASSERT_PARAM(ctx);

    ectx = (_mongocrypt_ctx_encrypt_t *)ctx;

    /* Otherwise, we need a remote schema. Check if we have a response to
     * listCollections cached. */
    if (!_mongocrypt_cache_get(&ctx->crypt->cache_collinfo,
                               ectx->target_ns /* null terminated */,
                               (void **)&collinfo)) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "failed to retrieve from cache");
    }

    if (collinfo) {
        if (!_set_schema_from_collinfo(ctx, collinfo)) {
            bson_destroy(collinfo);
            return _mongocrypt_ctx_fail(ctx);
        }
        ctx->state = MONGOCRYPT_CTX_NEED_MONGO_MARKINGS;
    } else {
        /* we need to get it. */
        ctx->state = MONGOCRYPT_CTX_NEED_MONGO_COLLINFO;
        if (ectx->target_db) {
            if (!ctx->crypt->opts.use_need_mongo_collinfo_with_db_state) {
                _mongocrypt_ctx_fail_w_msg(
                    ctx,
                    "Fetching remote collection information on separate databases is not supported. Try "
                    "upgrading driver, or specify a local schemaMap or encryptedFieldsMap.");
                return false;
            }
            // Target database may differ from command database. Request collection info from target database.
            ctx->state = MONGOCRYPT_CTX_NEED_MONGO_COLLINFO_WITH_DB;
        }
    }

    bson_destroy(collinfo);
    return true;
}

/* _try_empty_schema_for_create uses an empty JSON schema for the create
 * command. This is to avoid an unnecessary 'listCollections' command for
 * create. */
static bool _try_empty_schema_for_create(mongocrypt_ctx_t *ctx) {
    _mongocrypt_ctx_encrypt_t *ectx;

    BSON_ASSERT_PARAM(ctx);

    ectx = (_mongocrypt_ctx_encrypt_t *)ctx;
    /* As a special case, use an empty schema for the 'create' command. */
    const char *cmd_name = ectx->cmd_name;

    if (0 != strcmp(cmd_name, "create")) {
        return true;
    }

    bson_t empty = BSON_INITIALIZER;
    _mongocrypt_buffer_steal_from_bson(&ectx->schema, &empty);
    ctx->state = MONGOCRYPT_CTX_NEED_MONGO_MARKINGS;
    return true;
}

/* _try_schema_from_create_or_collMod_cmd tries to find a JSON schema included
 * in a create or collMod command by checking for "validator.$jsonSchema".
 * Example:
 * {
 *     "create" : "coll",
 *     "validator" : {
 *         "$jsonSchema" : {
 *             "properties" : { "a" : { "bsonType" : "number" } }
 *          }
 *     }
 * }
 * If the "create" command does not include a JSON schema, an empty JSON schema
 * is returned. This is to avoid an unnecessary 'listCollections' command for
 * create.
 *
 * If the "collMod" command does not include a JSON schema, a schema is later
 * requested by entering the MONGOCRYPT_CTX_NEED_MONGO_COLLINFO state.
 * This is because a "collMod" command may have sensitive data in the
 * "validator" field.
 */
static bool _try_schema_from_create_or_collMod_cmd(mongocrypt_ctx_t *ctx) {
    _mongocrypt_ctx_encrypt_t *ectx;
    mongocrypt_status_t *status;

    BSON_ASSERT_PARAM(ctx);

    status = ctx->status;

    ectx = (_mongocrypt_ctx_encrypt_t *)ctx;
    const char *cmd_name = ectx->cmd_name;

    if (0 != strcmp(cmd_name, "create") && 0 != strcmp(cmd_name, "collMod")) {
        return true;
    }

    bson_t cmd_bson;
    bson_iter_t iter;

    if (!_mongocrypt_buffer_to_bson(&ectx->original_cmd, &cmd_bson)) {
        CLIENT_ERR("unable to convert command buffer to BSON");
        _mongocrypt_ctx_fail(ctx);
        return false;
    }

    if (!bson_iter_init(&iter, &cmd_bson)) {
        CLIENT_ERR("unable to iterate over command BSON");
        _mongocrypt_ctx_fail(ctx);
        return false;
    }

    if (bson_iter_find_descendant(&iter, "validator.$jsonSchema", &iter)) {
        if (!_mongocrypt_buffer_copy_from_document_iter(&ectx->schema, &iter)) {
            CLIENT_ERR("failed to parse BSON document from create validator.$jsonSchema");
            _mongocrypt_ctx_fail(ctx);
            return false;
        }
        ctx->state = MONGOCRYPT_CTX_NEED_MONGO_MARKINGS;
        return true;
    }

    return true;
}

static bool
_permitted_for_encryption(bson_iter_t *iter, mongocrypt_encryption_algorithm_t algo, mongocrypt_status_t *status) {
    bson_type_t bson_type;
    const bson_value_t *bson_value;
    bool ret = false;

    BSON_ASSERT_PARAM(iter);

    bson_value = bson_iter_value(iter);
    if (!bson_value) {
        CLIENT_ERR("Unknown BSON type");
        goto fail;
    }
    bson_type = bson_value->value_type;
    switch (bson_type) {
    case BSON_TYPE_NULL:
    case BSON_TYPE_MINKEY:
    case BSON_TYPE_MAXKEY:
    case BSON_TYPE_UNDEFINED: CLIENT_ERR("BSON type invalid for encryption"); goto fail;
    case BSON_TYPE_BINARY:
        if (bson_value->value.v_binary.subtype == BSON_SUBTYPE_ENCRYPTED) {
            CLIENT_ERR("BSON binary subtype 6 is invalid for encryption");
            goto fail;
        }
        /* ok */
        break;
    case BSON_TYPE_DOUBLE:
    case BSON_TYPE_DOCUMENT:
    case BSON_TYPE_ARRAY:
    case BSON_TYPE_CODEWSCOPE:
    case BSON_TYPE_BOOL:
    case BSON_TYPE_DECIMAL128:
        if (algo == MONGOCRYPT_ENCRYPTION_ALGORITHM_DETERMINISTIC) {
            CLIENT_ERR("BSON type invalid for deterministic encryption");
            goto fail;
        }
        break;
    case BSON_TYPE_UTF8:
    case BSON_TYPE_OID:
    case BSON_TYPE_DATE_TIME:
    case BSON_TYPE_REGEX:
    case BSON_TYPE_DBPOINTER:
    case BSON_TYPE_CODE:
    case BSON_TYPE_SYMBOL:
    case BSON_TYPE_INT32:
    case BSON_TYPE_TIMESTAMP:
    case BSON_TYPE_INT64:
        /* ok */
        break;
    case BSON_TYPE_EOD:
    default: CLIENT_ERR("invalid BSON value type 00"); goto fail;
    }

    ret = true;
fail:
    return ret;
}

// explicit_encrypt_init is common code shared by
// mongocrypt_ctx_explicit_encrypt_init and
// mongocrypt_ctx_explicit_encrypt_expression_init.
static bool explicit_encrypt_init(mongocrypt_ctx_t *ctx, mongocrypt_binary_t *msg) {
    _mongocrypt_ctx_encrypt_t *ectx;
    bson_t as_bson;
    bson_iter_t iter;
    _mongocrypt_ctx_opts_spec_t opts_spec = {0};

    if (!ctx) {
        return false;
    }
    memset(&opts_spec, 0, sizeof(opts_spec));
    opts_spec.key_descriptor = OPT_REQUIRED;
    opts_spec.algorithm = OPT_OPTIONAL;
    opts_spec.rangeopts = OPT_OPTIONAL;

    if (!_mongocrypt_ctx_init(ctx, &opts_spec)) {
        return false;
    }

    /* Error if any mutually exclusive FLE 1 and FLE 2 options are set. */
    {
        /* key_alt_names is FLE 1 only. */
        if (ctx->opts.key_alt_names != NULL) {
            if (ctx->opts.index_type.set) {
                return _mongocrypt_ctx_fail_w_msg(ctx, "cannot set both key alt name and index type");
            }
            if (!_mongocrypt_buffer_empty(&ctx->opts.index_key_id)) {
                return _mongocrypt_ctx_fail_w_msg(ctx, "cannot set both key alt name and index key id");
            }
            if (ctx->opts.contention_factor.set) {
                return _mongocrypt_ctx_fail_w_msg(ctx, "cannot set both key alt name and contention factor");
            }
            if (ctx->opts.query_type.set) {
                return _mongocrypt_ctx_fail_w_msg(ctx, "cannot set both key alt name and query type");
            }
            if (ctx->opts.rangeopts.set) {
                return _mongocrypt_ctx_fail_w_msg(ctx, "cannot set both key alt name and range opts");
            }
        }
        /* algorithm is FLE 1 only. */
        if (ctx->opts.algorithm != MONGOCRYPT_ENCRYPTION_ALGORITHM_NONE) {
            if (!_mongocrypt_buffer_empty(&ctx->opts.index_key_id)) {
                return _mongocrypt_ctx_fail_w_msg(ctx, "cannot set both algorithm and index key id");
            }
            if (ctx->opts.contention_factor.set) {
                return _mongocrypt_ctx_fail_w_msg(ctx, "cannot set both algorithm and contention factor");
            }
            if (ctx->opts.query_type.set) {
                return _mongocrypt_ctx_fail_w_msg(ctx, "cannot set both algorithm and query type");
            }
            if (ctx->opts.rangeopts.set) {
                return _mongocrypt_ctx_fail_w_msg(ctx, "cannot set both algorithm and range opts");
            }
        }
    }

    if (ctx->opts.algorithm == MONGOCRYPT_ENCRYPTION_ALGORITHM_NONE && !ctx->opts.index_type.set) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "algorithm or index type required");
    }

    if (ctx->opts.contention_factor.set && ctx->opts.index_type.set
        && ctx->opts.index_type.value == MONGOCRYPT_INDEX_TYPE_NONE) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "cannot set contention factor with no index type");
    }

    if (ctx->opts.query_type.set && ctx->opts.index_type.set
        && ctx->opts.index_type.value == MONGOCRYPT_INDEX_TYPE_NONE) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "cannot set query type with no index type");
    }

    if (ctx->opts.rangeopts.set && ctx->opts.index_type.set) {
        if (ctx->opts.index_type.value == MONGOCRYPT_INDEX_TYPE_NONE) {
            return _mongocrypt_ctx_fail_w_msg(ctx, "cannot set range opts with no index type");
        }

        if (ctx->opts.index_type.value == MONGOCRYPT_INDEX_TYPE_EQUALITY) {
            return _mongocrypt_ctx_fail_w_msg(ctx, "cannot set range opts with equality index type");
        }
    }

    if (ctx->opts.contention_factor.set && !mc_validate_contention(ctx->opts.contention_factor.value, ctx->status)) {
        return _mongocrypt_ctx_fail(ctx);
    }

    if (ctx->opts.index_type.set && ctx->opts.index_type.value == MONGOCRYPT_INDEX_TYPE_EQUALITY
        && !ctx->opts.contention_factor.set) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "contention factor is required for indexed algorithm");
    }

    if (ctx->opts.index_type.set
        && (ctx->opts.index_type.value == MONGOCRYPT_INDEX_TYPE_RANGE
            || ctx->opts.index_type.value == MONGOCRYPT_INDEX_TYPE_RANGEPREVIEW_DEPRECATED)) {
        if (!ctx->opts.contention_factor.set) {
            return _mongocrypt_ctx_fail_w_msg(ctx, "contention factor is required for range indexed algorithm");
        }

        if (!ctx->opts.rangeopts.set) {
            return _mongocrypt_ctx_fail_w_msg(ctx, "range opts are required for range indexed algorithm");
        }
    }

    if (ctx->opts.rangeopts.set && !mc_validate_sparsity(ctx->opts.rangeopts.value.sparsity, ctx->status)) {
        return _mongocrypt_ctx_fail(ctx);
    }

    // If query type is set, it must match the index type.
    if (ctx->opts.query_type.set && ctx->opts.index_type.set) {
        mongocrypt_status_t *const status = ctx->status;
        bool matches = false;

        switch (ctx->opts.query_type.value) {
        case MONGOCRYPT_QUERY_TYPE_RANGEPREVIEW_DEPRECATED:
            // Don't allow deprecated query type if we are using new index type.
            matches = (ctx->opts.index_type.value == MONGOCRYPT_INDEX_TYPE_RANGEPREVIEW_DEPRECATED);
            break;
        case MONGOCRYPT_QUERY_TYPE_RANGE:
            // New query type is compatible with both new and old index types.
            matches = (ctx->opts.index_type.value == MONGOCRYPT_INDEX_TYPE_RANGEPREVIEW_DEPRECATED
                       || ctx->opts.index_type.value == MONGOCRYPT_INDEX_TYPE_RANGE);
            break;
        case MONGOCRYPT_QUERY_TYPE_EQUALITY:
            matches = (ctx->opts.index_type.value == MONGOCRYPT_INDEX_TYPE_EQUALITY);
            break;
        default:
            CLIENT_ERR("unsupported value for query_type: %d", ctx->opts.query_type.value);
            return _mongocrypt_ctx_fail(ctx);
        }

        if (!matches) {
            CLIENT_ERR("query_type (%s) must match index_type (%s)",
                       _mongocrypt_query_type_to_string(ctx->opts.query_type.value),
                       _mongocrypt_index_type_to_string(ctx->opts.index_type.value));
            return _mongocrypt_ctx_fail(ctx);
        }
    }

    ectx = (_mongocrypt_ctx_encrypt_t *)ctx;
    ctx->type = _MONGOCRYPT_TYPE_ENCRYPT;
    ectx->explicit = true;
    ctx->vtable.finalize = _finalize;
    ctx->vtable.cleanup = _cleanup;

    if (!msg || !msg->data) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "msg required for explicit encryption");
    }

    if (ctx->opts.key_alt_names) {
        if (!_mongocrypt_key_broker_request_name(&ctx->kb, &ctx->opts.key_alt_names->value)) {
            return _mongocrypt_ctx_fail(ctx);
        }
    } else {
        if (!_mongocrypt_key_broker_request_id(&ctx->kb, &ctx->opts.key_id)) {
            return _mongocrypt_ctx_fail(ctx);
        }
    }

    if (!_mongocrypt_buffer_empty(&ctx->opts.index_key_id)) {
        if (!_mongocrypt_key_broker_request_id(&ctx->kb, &ctx->opts.index_key_id)) {
            return _mongocrypt_ctx_fail(ctx);
        }
    }

    _mongocrypt_buffer_init(&ectx->original_cmd);

    _mongocrypt_buffer_copy_from_binary(&ectx->original_cmd, msg);
    if (!_mongocrypt_buffer_to_bson(&ectx->original_cmd, &as_bson)) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "msg must be bson");
    }

    if (ctx->crypt->log.trace_enabled) {
        char *cmd_val;
        cmd_val = _mongocrypt_new_json_string_from_binary(msg);
        _mongocrypt_log(&ctx->crypt->log, MONGOCRYPT_LOG_LEVEL_TRACE, "%s (%s=\"%s\")", BSON_FUNC, "msg", cmd_val);
        bson_free(cmd_val);
    }

    if (!bson_iter_init_find(&iter, &as_bson, "v")) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "invalid msg, must contain 'v'");
    }

    if (!_permitted_for_encryption(&iter, ctx->opts.algorithm, ctx->status)) {
        return _mongocrypt_ctx_fail(ctx);
    }

    (void)_mongocrypt_key_broker_requests_done(&ctx->kb);
    return _mongocrypt_ctx_state_from_key_broker(ctx);
}

bool mongocrypt_ctx_explicit_encrypt_init(mongocrypt_ctx_t *ctx, mongocrypt_binary_t *msg) {
    if (!explicit_encrypt_init(ctx, msg)) {
        return false;
    }
    if (ctx->opts.query_type.set
        && (ctx->opts.query_type.value == MONGOCRYPT_QUERY_TYPE_RANGE
            || ctx->opts.query_type.value == MONGOCRYPT_QUERY_TYPE_RANGEPREVIEW_DEPRECATED)) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "Encrypt may not be used for range queries. Use EncryptExpression.");
    }
    return true;
}

bool mongocrypt_ctx_explicit_encrypt_expression_init(mongocrypt_ctx_t *ctx, mongocrypt_binary_t *msg) {
    if (!explicit_encrypt_init(ctx, msg)) {
        return false;
    }
    if (!ctx->opts.query_type.set
        || !(ctx->opts.query_type.value == MONGOCRYPT_QUERY_TYPE_RANGE
             || ctx->opts.query_type.value == MONGOCRYPT_QUERY_TYPE_RANGEPREVIEW_DEPRECATED)) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "EncryptExpression may only be used for range queries.");
    }
    return true;
}

static bool _check_cmd_for_auto_encrypt_bulkWrite(mongocrypt_binary_t *cmd,
                                                  char **target_db,
                                                  char **target_coll,
                                                  mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(cmd);
    BSON_ASSERT_PARAM(target_db);
    BSON_ASSERT_PARAM(target_coll);

    bson_t as_bson;
    bson_iter_t cmd_iter = {0};

    if (!_mongocrypt_binary_to_bson(cmd, &as_bson) || !bson_iter_init(&cmd_iter, &as_bson)) {
        CLIENT_ERR("invalid command BSON");
        return false;
    }

    bson_iter_t ns_iter = cmd_iter;
    if (!bson_iter_find_descendant(&ns_iter, "nsInfo.0.ns", &ns_iter)) {
        CLIENT_ERR("failed to find namespace in `bulkWrite` command");
        return false;
    }

    if (!BSON_ITER_HOLDS_UTF8(&ns_iter)) {
        CLIENT_ERR("expected namespace to be UTF8, got: %s", mc_bson_type_to_string(bson_iter_type(&ns_iter)));
        return false;
    }

    const char *target_ns = bson_iter_utf8(&ns_iter, NULL /* length */);
    // Parse `target_ns` into "<db>.<coll>"
    const char *dot = strstr(target_ns, ".");
    if (!dot) {
        CLIENT_ERR("expected namespace to contain dot, got: %s", target_ns);
        return false;
    }
    *target_coll = bson_strdup(dot + 1);
    // Get the database from the `ns` field (which may differ from `cmd_db`).
    ptrdiff_t db_len = dot - target_ns;
    if ((uint64_t)db_len > SIZE_MAX) {
        CLIENT_ERR("unexpected database length exceeds %zu", SIZE_MAX);
        return false;
    }
    *target_db = bson_strndup(target_ns, (size_t)db_len);

    // Ensure only one `nsInfo` element is present.
    // Query analysis (mongocryptd/crypt_shared) currently only supports one namespace.
    if (bson_has_field(&as_bson, "nsInfo.1")) {
        CLIENT_ERR("expected one namespace in `bulkWrite`, but found more than one. Only one namespace is supported.");
        return false;
    }

    return true;
}

static bool
_check_cmd_for_auto_encrypt(mongocrypt_binary_t *cmd, bool *bypass, char **target_coll, mongocrypt_status_t *status) {
    bson_t as_bson;
    bson_iter_t iter = {0}, target_coll_iter;
    const char *cmd_name;
    bool eligible = false;

    BSON_ASSERT_PARAM(cmd);
    BSON_ASSERT_PARAM(bypass);
    BSON_ASSERT_PARAM(target_coll);

    *bypass = false;

    if (!_mongocrypt_binary_to_bson(cmd, &as_bson) || !bson_iter_init(&iter, &as_bson)) {
        CLIENT_ERR("invalid BSON");
        return false;
    }

    /* The command name is the first key. */
    if (!bson_iter_next(&iter)) {
        CLIENT_ERR("invalid empty BSON");
        return false;
    }

    cmd_name = bson_iter_key(&iter);
    BSON_ASSERT(cmd_name);

    /* get the collection name (or NULL if database/client command). */
    if (0 == strcmp(cmd_name, "explain")) {
        if (!BSON_ITER_HOLDS_DOCUMENT(&iter)) {
            CLIENT_ERR("explain value is not a document");
            return false;
        }
        if (!bson_iter_recurse(&iter, &target_coll_iter)) {
            CLIENT_ERR("malformed BSON for encrypt command");
            return false;
        }
        if (!bson_iter_next(&target_coll_iter)) {
            CLIENT_ERR("invalid empty BSON");
            return false;
        }
    } else {
        memcpy(&target_coll_iter, &iter, sizeof(iter));
    }

    if (BSON_ITER_HOLDS_UTF8(&target_coll_iter)) {
        *target_coll = bson_strdup(bson_iter_utf8(&target_coll_iter, NULL));
    } else {
        *target_coll = NULL;
    }

    /* check if command is eligible for auto encryption, bypassed, or ineligible.
     */
    if (0 == strcmp(cmd_name, "aggregate")) {
        /* collection level aggregate ok, database/client is not. */
        eligible = true;
    } else if (0 == strcmp(cmd_name, "count")) {
        eligible = true;
    } else if (0 == strcmp(cmd_name, "distinct")) {
        eligible = true;
    } else if (0 == strcmp(cmd_name, "delete")) {
        eligible = true;
    } else if (0 == strcmp(cmd_name, "find")) {
        eligible = true;
    } else if (0 == strcmp(cmd_name, "findAndModify")) {
        eligible = true;
    } else if (0 == strcmp(cmd_name, "getMore")) {
        *bypass = true;
    } else if (0 == strcmp(cmd_name, "insert")) {
        eligible = true;
    } else if (0 == strcmp(cmd_name, "update")) {
        eligible = true;
    } else if (0 == strcmp(cmd_name, "authenticate")) {
        *bypass = true;
    } else if (0 == strcmp(cmd_name, "getnonce")) {
        *bypass = true;
    } else if (0 == strcmp(cmd_name, "logout")) {
        *bypass = true;
    } else if (0 == bson_strcasecmp(cmd_name, "isMaster")) {
        /* use case insensitive compare for ismaster, since some drivers send
         * "ismaster" and others send "isMaster" */
        *bypass = true;
    } else if (0 == strcmp(cmd_name, "abortTransaction")) {
        *bypass = true;
    } else if (0 == strcmp(cmd_name, "commitTransaction")) {
        *bypass = true;
    } else if (0 == strcmp(cmd_name, "endSessions")) {
        *bypass = true;
    } else if (0 == strcmp(cmd_name, "startSession")) {
        *bypass = true;
    } else if (0 == strcmp(cmd_name, "create")) {
        eligible = true;
    } else if (0 == strcmp(cmd_name, "createIndexes")) {
        eligible = true;
    } else if (0 == strcmp(cmd_name, "drop")) {
        *bypass = true;
    } else if (0 == strcmp(cmd_name, "dropDatabase")) {
        *bypass = true;
    } else if (0 == strcmp(cmd_name, "dropIndexes")) {
        *bypass = true;
    } else if (0 == strcmp(cmd_name, "killCursors")) {
        *bypass = true;
    } else if (0 == strcmp(cmd_name, "listCollections")) {
        *bypass = true;
    } else if (0 == strcmp(cmd_name, "listDatabases")) {
        *bypass = true;
    } else if (0 == strcmp(cmd_name, "listIndexes")) {
        *bypass = true;
    } else if (0 == strcmp(cmd_name, "renameCollection")) {
        *bypass = true;
    } else if (0 == strcmp(cmd_name, "explain")) {
        eligible = true;
    } else if (0 == strcmp(cmd_name, "ping")) {
        *bypass = true;
    } else if (0 == strcmp(cmd_name, "saslStart")) {
        *bypass = true;
    } else if (0 == strcmp(cmd_name, "saslContinue")) {
        *bypass = true;
    } else if (0 == strcmp(cmd_name, "killAllSessions")) {
        *bypass = true;
    } else if (0 == strcmp(cmd_name, "killSessions")) {
        *bypass = true;
    } else if (0 == strcmp(cmd_name, "killAllSessionsByPattern")) {
        *bypass = true;
    } else if (0 == strcmp(cmd_name, "refreshSessions")) {
        *bypass = true;
    } else if (0 == strcmp(cmd_name, "compactStructuredEncryptionData")) {
        eligible = true;
    } else if (0 == strcmp(cmd_name, "cleanupStructuredEncryptionData")) {
        eligible = true;
    } else if (0 == strcmp(cmd_name, "collMod")) {
        eligible = true;
    } else if (0 == strcmp(cmd_name, "hello")) {
        *bypass = true;
    } else if (0 == strcmp(cmd_name, "buildInfo")) {
        *bypass = true;
    } else if (0 == strcmp(cmd_name, "getCmdLineOpts")) {
        *bypass = true;
    } else if (0 == strcmp(cmd_name, "getLog")) {
        *bypass = true;
    } else if (0 == strcmp(cmd_name, "createSearchIndexes")) {
        *bypass = true;
    } else if (0 == strcmp(cmd_name, "listSearchIndexes")) {
        *bypass = true;
    } else if (0 == strcmp(cmd_name, "dropSearchIndex")) {
        *bypass = true;
    } else if (0 == strcmp(cmd_name, "updateSearchIndex")) {
        *bypass = true;
    }

    /* database/client commands are ineligible. */
    if (eligible) {
        if (!*target_coll) {
            CLIENT_ERR("non-collection command not supported for auto encryption: %s", cmd_name);
            return false;
        }
        if (0 == strlen(*target_coll)) {
            CLIENT_ERR("empty collection name on command: %s", cmd_name);
            return false;
        }
    }

    if (eligible || *bypass) {
        return true;
    }

    CLIENT_ERR("command not supported for auto encryption: %s", cmd_name);
    return false;
}

static bool needs_ismaster_check(mongocrypt_ctx_t *ctx) {
    _mongocrypt_ctx_encrypt_t *ectx = (_mongocrypt_ctx_encrypt_t *)ctx;

    BSON_ASSERT_PARAM(ctx);

    bool using_mongocryptd = !ectx->bypass_query_analysis && !ctx->crypt->csfle.okay;
    // The "create" and "createIndexes" command require an isMaster check when
    // using mongocryptd. See MONGOCRYPT-429.
    return using_mongocryptd && (0 == strcmp(ectx->cmd_name, "create") || 0 == strcmp(ectx->cmd_name, "createIndexes"));
}

bool mongocrypt_ctx_encrypt_init(mongocrypt_ctx_t *ctx, const char *db, int32_t db_len, mongocrypt_binary_t *cmd) {
    _mongocrypt_ctx_encrypt_t *ectx;
    _mongocrypt_ctx_opts_spec_t opts_spec;

    if (!ctx) {
        return false;
    }

    if (!db) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "invalid db");
    }

    memset(&opts_spec, 0, sizeof(opts_spec));
    opts_spec.schema = OPT_OPTIONAL;
    if (!_mongocrypt_ctx_init(ctx, &opts_spec)) {
        return false;
    }

    ectx = (_mongocrypt_ctx_encrypt_t *)ctx;
    ctx->type = _MONGOCRYPT_TYPE_ENCRYPT;
    ectx->explicit = false;
    ctx->vtable.mongo_op_collinfo = _mongo_op_collinfo;
    ctx->vtable.mongo_feed_collinfo = _mongo_feed_collinfo;
    ctx->vtable.mongo_done_collinfo = _mongo_done_collinfo;
    ctx->vtable.mongo_db_collinfo = _mongo_db_collinfo;
    ctx->vtable.mongo_op_collinfo = _mongo_op_collinfo;
    ctx->vtable.mongo_op_markings = _mongo_op_markings;
    ctx->vtable.mongo_feed_markings = _mongo_feed_markings;
    ctx->vtable.mongo_done_markings = _mongo_done_markings;
    ctx->vtable.finalize = _finalize;
    ctx->vtable.cleanup = _cleanup;
    ectx->bypass_query_analysis = ctx->crypt->opts.bypass_query_analysis;

    if (!cmd || !cmd->data) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "invalid command");
    }

    _mongocrypt_buffer_copy_from_binary(&ectx->original_cmd, cmd);

    ectx->cmd_name = get_command_name(&ectx->original_cmd, ctx->status);
    if (!ectx->cmd_name) {
        return _mongocrypt_ctx_fail(ctx);
    }

    if (!_mongocrypt_validate_and_copy_string(db, db_len, &ectx->cmd_db) || 0 == strlen(ectx->cmd_db)) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "invalid db");
    }

    if (0 == strcmp(ectx->cmd_name, "bulkWrite")) {
        // Handle `bulkWrite` as a special case.
        // `bulkWrite` includes the target namespaces in an `nsInfo` field.
        // Only one target namespace is supported.
        if (!_check_cmd_for_auto_encrypt_bulkWrite(cmd, &ectx->target_db, &ectx->target_coll, ctx->status)) {
            return _mongocrypt_ctx_fail(ctx);
        }

        ectx->target_ns = bson_strdup_printf("%s.%s", ectx->target_db, ectx->target_coll);
    } else {
        bool bypass;
        if (!_check_cmd_for_auto_encrypt(cmd, &bypass, &ectx->target_coll, ctx->status)) {
            return _mongocrypt_ctx_fail(ctx);
        }

        if (bypass) {
            ctx->nothing_to_do = true;
            ctx->state = MONGOCRYPT_CTX_READY;
            return true;
        }

        /* if _check_cmd_for_auto_encrypt did not bypass or error, a collection name
         * must have been set. */
        if (!ectx->target_coll) {
            return _mongocrypt_ctx_fail_w_msg(ctx, "unexpected error: did not bypass or error but no collection name");
        }
        ectx->target_ns = bson_strdup_printf("%s.%s", ectx->cmd_db, ectx->target_coll);
    }

    if (ctx->opts.kek.provider.aws.region || ctx->opts.kek.provider.aws.cmk) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "aws masterkey options must not be set");
    }

    if (!_mongocrypt_buffer_empty(&ctx->opts.key_id)) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "key_id must not be set for auto encryption");
    }

    if (ctx->opts.algorithm != MONGOCRYPT_ENCRYPTION_ALGORITHM_NONE) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "algorithm must not be set for auto encryption");
    }

    if (ctx->crypt->log.trace_enabled) {
        char *cmd_val;
        cmd_val = _mongocrypt_new_json_string_from_binary(cmd);
        _mongocrypt_log(&ctx->crypt->log,
                        MONGOCRYPT_LOG_LEVEL_TRACE,
                        "%s (%s=\"%s\", %s=%d, %s=\"%s\")",
                        BSON_FUNC,
                        "db",
                        ectx->cmd_db,
                        "db_len",
                        db_len,
                        "cmd",
                        cmd_val);
        bson_free(cmd_val);
    }

    /* The "create" and "createIndexes" command require sending an isMaster
     * request to mongocryptd. */
    if (needs_ismaster_check(ctx)) {
        /* We are using mongocryptd. We need to ensure that mongocryptd
         * maxWireVersion >= 17. */
        ectx->ismaster.needed = true;
        ctx->state = MONGOCRYPT_CTX_NEED_MONGO_MARKINGS;
        return true;
    }

    return mongocrypt_ctx_encrypt_ismaster_done(ctx);
}

#define WIRE_VERSION_SERVER_6 17

/* mongocrypt_ctx_encrypt_ismaster_done is called when:
 * 1. The max wire version of mongocryptd is known.
 * 2. The max wire version of mongocryptd is not required for the command.
 */
static bool mongocrypt_ctx_encrypt_ismaster_done(mongocrypt_ctx_t *ctx) {
    _mongocrypt_ctx_encrypt_t *ectx = (_mongocrypt_ctx_encrypt_t *)ctx;

    BSON_ASSERT_PARAM(ctx);

    ectx->ismaster.needed = false;

    /* The "create" and "createIndexes" command require bypassing on mongocryptd
     * older than version 6.0. */
    if (needs_ismaster_check(ctx)) {
        if (ectx->ismaster.maxwireversion < WIRE_VERSION_SERVER_6) {
            /* Bypass. */
            ctx->nothing_to_do = true;
            ctx->state = MONGOCRYPT_CTX_READY;
            return true;
        }
    }

    /* Check if there is an encrypted field config in encrypted_field_config_map
     */
    if (!_fle2_try_encrypted_field_config_from_map(ctx)) {
        return false;
    }
    if (_mongocrypt_buffer_empty(&ectx->encrypted_field_config)) {
        if (!_try_schema_from_create_or_collMod_cmd(ctx)) {
            return false;
        }

        /* Check if we have a local schema from schema_map */
        if (_mongocrypt_buffer_empty(&ectx->schema)) {
            if (!_try_schema_from_schema_map(ctx)) {
                return false;
            }
        }

        /* If we didn't have a local schema, try the cache. */
        if (_mongocrypt_buffer_empty(&ectx->schema)) {
            if (!_try_schema_from_cache(ctx)) {
                return false;
            }
        }

        /* If we did not have a local or cached schema, check if this is a
         * "create" command. If it is a "create" command, do not run
         * "listCollections" to get a server-side schema. */
        if (_mongocrypt_buffer_empty(&ectx->schema) && !_try_empty_schema_for_create(ctx)) {
            return false;
        }

        /* Otherwise, we need the the driver to fetch the schema. */
        if (_mongocrypt_buffer_empty(&ectx->schema)) {
            ctx->state = MONGOCRYPT_CTX_NEED_MONGO_COLLINFO;
            if (ectx->target_db) {
                if (!ctx->crypt->opts.use_need_mongo_collinfo_with_db_state) {
                    _mongocrypt_ctx_fail_w_msg(
                        ctx,
                        "Fetching remote collection information on separate databases is not supported. Try "
                        "upgrading driver, or specify a local schemaMap or encryptedFieldsMap.");
                    return false;
                }
                // Target database may differ from command database. Request collection info from target database.
                ctx->state = MONGOCRYPT_CTX_NEED_MONGO_COLLINFO_WITH_DB;
            }
        }
    }

    /* If an encrypted_field_config was set, check if keys are required for
     * delete tokens. */
    if (!_fle2_collect_keys_for_deleteTokens(ctx)) {
        return false;
    }

    if (!_fle2_collect_keys_for_compaction(ctx)) {
        return false;
    }

    if (ctx->state == MONGOCRYPT_CTX_NEED_MONGO_MARKINGS) {
        if (ectx->bypass_query_analysis) {
            /* Keys may have been requested for deleteTokens or compactionTokens.
             * Finish key requests.
             */
            _mongocrypt_key_broker_requests_done(&ctx->kb);
            return _mongocrypt_ctx_state_from_key_broker(ctx);
        }
        // We're ready for markings. Try to generate them ourself.
        return _try_run_csfle_marking(ctx);
    } else {
        // Other state, return to caller.
        return true;
    }
}

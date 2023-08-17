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

#include "mongocrypt-crypto-private.h"
#include "mongocrypt-ctx-private.h"
#include "mongocrypt-private.h"
#include "mongocrypt.h"

static void _cleanup(mongocrypt_ctx_t *ctx) {
    _mongocrypt_ctx_datakey_t *dkctx;

    BSON_ASSERT_PARAM(ctx);

    dkctx = (_mongocrypt_ctx_datakey_t *)ctx;
    _mongocrypt_buffer_cleanup(&dkctx->key_doc);
    _mongocrypt_kms_ctx_cleanup(&dkctx->kms);
    _mongocrypt_buffer_cleanup(&dkctx->encrypted_key_material);
    _mongocrypt_buffer_cleanup(&dkctx->plaintext_key_material);
    _mongocrypt_buffer_cleanup(&dkctx->kmip_secretdata);
    bson_free((void *)dkctx->kmip_unique_identifier);
}

static mongocrypt_kms_ctx_t *_next_kms_ctx(mongocrypt_ctx_t *ctx) {
    _mongocrypt_ctx_datakey_t *dkctx;

    BSON_ASSERT_PARAM(ctx);

    dkctx = (_mongocrypt_ctx_datakey_t *)ctx;
    if (dkctx->kms_returned) {
        return NULL;
    }
    dkctx->kms_returned = true;
    return &dkctx->kms;
}

static bool _kms_kmip_start(mongocrypt_ctx_t *ctx) {
    bool ret = false;
    _mongocrypt_ctx_datakey_t *dkctx = (_mongocrypt_ctx_datakey_t *)ctx;
    char *user_supplied_keyid = NULL;
    _mongocrypt_endpoint_t *endpoint = NULL;
    mongocrypt_status_t *status = ctx->status;
    _mongocrypt_buffer_t secretdata = {0};

    BSON_ASSERT_PARAM(ctx);

    if (ctx->opts.kek.kms_provider != MONGOCRYPT_KMS_PROVIDER_KMIP) {
        CLIENT_ERR("KMS provider is not KMIP");
        goto fail;
    }

    user_supplied_keyid = ctx->opts.kek.provider.kmip.key_id;

    if (ctx->opts.kek.provider.kmip.endpoint) {
        endpoint = ctx->opts.kek.provider.kmip.endpoint;
    } else if (_mongocrypt_ctx_kms_providers(ctx)->kmip.endpoint) {
        endpoint = _mongocrypt_ctx_kms_providers(ctx)->kmip.endpoint;
    } else {
        CLIENT_ERR("endpoint not set for KMIP request");
        goto fail;
    }

    /* The KMIP createDataKey flow is the following:
     *
     * 1. Send a KMIP Register request with a new 96 byte key as a SecretData
     *    managed object. This returns a Unique Identifier.
     * 2. Send a KMIP Activate request with the Unique Identifier.
     *    This returns the same Unique Identifier.
     * 3. Send a KMIP Get request with the Unique Identifier.
     *    This returns the 96 byte SecretData.
     * 4. Use the 96 byte SecretData to encrypt a new DEK.
     *
     * If the user set a 'keyId' to use, the flow begins at step 3.
     */

    if (user_supplied_keyid && !dkctx->kmip_unique_identifier) {
        /* User set a 'keyId'. */
        dkctx->kmip_unique_identifier = bson_strdup(user_supplied_keyid);
        dkctx->kmip_activated = true;
        /* Fall through to Step 3. */
    }

    if (!dkctx->kmip_unique_identifier) {
        /* User did not set a 'keyId'. */
        /* Step 1. Send a KMIP Register request with a new 96 byte SecretData. */
        _mongocrypt_buffer_init(&secretdata);
        _mongocrypt_buffer_resize(&secretdata, MONGOCRYPT_KEY_LEN);
        if (!_mongocrypt_random(ctx->crypt->crypto, &secretdata, MONGOCRYPT_KEY_LEN, ctx->status)) {
            goto fail;
        }

        if (!_mongocrypt_kms_ctx_init_kmip_register(&dkctx->kms,
                                                    endpoint,
                                                    secretdata.data,
                                                    secretdata.len,
                                                    &ctx->crypt->log)) {
            mongocrypt_kms_ctx_status(&dkctx->kms, ctx->status);
            goto fail;
        }
        ctx->state = MONGOCRYPT_CTX_NEED_KMS;

        goto success;
    }

    if (!dkctx->kmip_activated) {
        /* Step 2. Send a KMIP Activate request. */
        if (!_mongocrypt_kms_ctx_init_kmip_activate(&dkctx->kms,
                                                    endpoint,
                                                    dkctx->kmip_unique_identifier,
                                                    &ctx->crypt->log)) {
            mongocrypt_kms_ctx_status(&dkctx->kms, ctx->status);
            goto fail;
        }
        ctx->state = MONGOCRYPT_CTX_NEED_KMS;
        goto success;
    }

    if (!dkctx->kmip_secretdata.data) {
        /* Step 3. Send a KMIP Get request with the Unique Identifier. */
        if (!_mongocrypt_kms_ctx_init_kmip_get(&dkctx->kms,
                                               endpoint,
                                               dkctx->kmip_unique_identifier,
                                               &ctx->crypt->log)) {
            mongocrypt_kms_ctx_status(&dkctx->kms, ctx->status);
            goto fail;
        }
        ctx->state = MONGOCRYPT_CTX_NEED_KMS;
        goto success;
    }

    /* Step 4. Use the 96 byte SecretData to encrypt a new DEK. */
    if (!_mongocrypt_wrap_key(ctx->crypt->crypto,
                              &dkctx->kmip_secretdata,
                              &dkctx->plaintext_key_material,
                              &dkctx->encrypted_key_material,
                              ctx->status)) {
        goto fail;
    }

    if (!ctx->opts.kek.provider.kmip.key_id) {
        /* If there was no user supplied key_id, set it from the
         * UniqueIdentifer of the newly registered SecretData. */
        ctx->opts.kek.provider.kmip.key_id = bson_strdup(dkctx->kmip_unique_identifier);
    }
    ctx->state = MONGOCRYPT_CTX_READY;

success:
    ret = true;
fail:
    if (!ret) {
        _mongocrypt_ctx_fail(ctx);
    }
    _mongocrypt_buffer_cleanup(&secretdata);
    return ret;
}

/* For local, immediately encrypt.
 * For AWS, create the KMS request to encrypt.
 * For Azure/GCP, auth first if needed, otherwise encrypt.
 */
static bool _kms_start(mongocrypt_ctx_t *ctx) {
    BSON_ASSERT_PARAM(ctx);

    bool ret = false;
    _mongocrypt_ctx_datakey_t *dkctx;
    char *access_token = NULL;
    _mongocrypt_opts_kms_providers_t *const kms_providers = _mongocrypt_ctx_kms_providers(ctx);

    dkctx = (_mongocrypt_ctx_datakey_t *)ctx;

    /* Clear out any pre-existing initialized KMS context, and zero it (so it is
     * safe to call cleanup again). */
    _mongocrypt_kms_ctx_cleanup(&dkctx->kms);
    memset(&dkctx->kms, 0, sizeof(dkctx->kms));
    dkctx->kms_returned = false;
    if (ctx->opts.kek.kms_provider == MONGOCRYPT_KMS_PROVIDER_LOCAL) {
        if (!_mongocrypt_wrap_key(ctx->crypt->crypto,
                                  &kms_providers->local.key,
                                  &dkctx->plaintext_key_material,
                                  &dkctx->encrypted_key_material,
                                  ctx->status)) {
            _mongocrypt_ctx_fail(ctx);
            goto done;
        }
        ctx->state = MONGOCRYPT_CTX_READY;
    } else if (ctx->opts.kek.kms_provider == MONGOCRYPT_KMS_PROVIDER_AWS) {
        /* For AWS provider, AWS credentials are supplied in
         * mongocrypt_setopt_kms_provider_aws. Data keys are encrypted with an
         * "encrypt" HTTP message to KMS. */
        if (!_mongocrypt_kms_ctx_init_aws_encrypt(&dkctx->kms,
                                                  kms_providers,
                                                  &ctx->opts,
                                                  &dkctx->plaintext_key_material,
                                                  &ctx->crypt->log,
                                                  ctx->crypt->crypto)) {
            mongocrypt_kms_ctx_status(&dkctx->kms, ctx->status);
            _mongocrypt_ctx_fail(ctx);
            goto done;
        }

        ctx->state = MONGOCRYPT_CTX_NEED_KMS;
    } else if (ctx->opts.kek.kms_provider == MONGOCRYPT_KMS_PROVIDER_AZURE) {
        if (ctx->kms_providers.azure.access_token) {
            access_token = bson_strdup(ctx->kms_providers.azure.access_token);
        } else {
            access_token = _mongocrypt_cache_oauth_get(ctx->crypt->cache_oauth_azure);
        }
        if (access_token) {
            if (!_mongocrypt_kms_ctx_init_azure_wrapkey(&dkctx->kms,
                                                        &ctx->crypt->log,
                                                        kms_providers,
                                                        &ctx->opts,
                                                        access_token,
                                                        &dkctx->plaintext_key_material)) {
                mongocrypt_kms_ctx_status(&dkctx->kms, ctx->status);
                _mongocrypt_ctx_fail(ctx);
                goto done;
            }
        } else {
            if (!_mongocrypt_kms_ctx_init_azure_auth(&dkctx->kms,
                                                     &ctx->crypt->log,
                                                     kms_providers,
                                                     ctx->opts.kek.provider.azure.key_vault_endpoint)) {
                mongocrypt_kms_ctx_status(&dkctx->kms, ctx->status);
                _mongocrypt_ctx_fail(ctx);
                goto done;
            }
        }
        ctx->state = MONGOCRYPT_CTX_NEED_KMS;
    } else if (ctx->opts.kek.kms_provider == MONGOCRYPT_KMS_PROVIDER_GCP) {
        if (NULL != ctx->kms_providers.gcp.access_token) {
            access_token = bson_strdup((const char *)ctx->kms_providers.gcp.access_token);
        } else {
            access_token = _mongocrypt_cache_oauth_get(ctx->crypt->cache_oauth_gcp);
        }
        if (access_token) {
            if (!_mongocrypt_kms_ctx_init_gcp_encrypt(&dkctx->kms,
                                                      &ctx->crypt->log,
                                                      kms_providers,
                                                      &ctx->opts,
                                                      access_token,
                                                      &dkctx->plaintext_key_material)) {
                mongocrypt_kms_ctx_status(&dkctx->kms, ctx->status);
                _mongocrypt_ctx_fail(ctx);
                goto done;
            }
        } else {
            if (!_mongocrypt_kms_ctx_init_gcp_auth(&dkctx->kms,
                                                   &ctx->crypt->log,
                                                   &ctx->crypt->opts,
                                                   kms_providers,
                                                   ctx->opts.kek.provider.gcp.endpoint)) {
                mongocrypt_kms_ctx_status(&dkctx->kms, ctx->status);
                _mongocrypt_ctx_fail(ctx);
                goto done;
            }
        }
        ctx->state = MONGOCRYPT_CTX_NEED_KMS;
    } else if (ctx->opts.kek.kms_provider == MONGOCRYPT_KMS_PROVIDER_KMIP) {
        if (!_kms_kmip_start(ctx)) {
            goto done;
        }
    } else {
        _mongocrypt_ctx_fail_w_msg(ctx, "unsupported KMS provider");
        goto done;
    }

    ret = true;
done:
    bson_free(access_token);
    return ret;
}

static bool _kms_done(mongocrypt_ctx_t *ctx) {
    _mongocrypt_ctx_datakey_t *dkctx;
    mongocrypt_status_t *status;

    BSON_ASSERT_PARAM(ctx);

    dkctx = (_mongocrypt_ctx_datakey_t *)ctx;
    status = ctx->status;
    if (!mongocrypt_kms_ctx_status(&dkctx->kms, ctx->status)) {
        return _mongocrypt_ctx_fail(ctx);
    }

    if (mongocrypt_kms_ctx_bytes_needed(&dkctx->kms) != 0) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "KMS response unfinished");
    }

    /* If this was an oauth request, store the response and proceed to encrypt.
     */
    if (dkctx->kms.req_type == MONGOCRYPT_KMS_AZURE_OAUTH) {
        bson_t oauth_response;

        BSON_ASSERT(_mongocrypt_buffer_to_bson(&dkctx->kms.result, &oauth_response));
        if (!_mongocrypt_cache_oauth_add(ctx->crypt->cache_oauth_azure, &oauth_response, status)) {
            return _mongocrypt_ctx_fail(ctx);
        }
        return _kms_start(ctx);
    } else if (dkctx->kms.req_type == MONGOCRYPT_KMS_GCP_OAUTH) {
        bson_t oauth_response;

        BSON_ASSERT(_mongocrypt_buffer_to_bson(&dkctx->kms.result, &oauth_response));
        if (!_mongocrypt_cache_oauth_add(ctx->crypt->cache_oauth_gcp, &oauth_response, status)) {
            return _mongocrypt_ctx_fail(ctx);
        }
        return _kms_start(ctx);
    } else if (dkctx->kms.req_type == MONGOCRYPT_KMS_KMIP_REGISTER) {
        dkctx->kmip_unique_identifier = bson_strdup((const char *)dkctx->kms.result.data);
        return _kms_start(ctx);
    } else if (dkctx->kms.req_type == MONGOCRYPT_KMS_KMIP_ACTIVATE) {
        dkctx->kmip_activated = true;
        return _kms_start(ctx);
    } else if (dkctx->kms.req_type == MONGOCRYPT_KMS_KMIP_GET) {
        _mongocrypt_buffer_copy_to(&dkctx->kms.result, &dkctx->kmip_secretdata);
        return _kms_start(ctx);
    }

    /* Store the result. */
    if (!_mongocrypt_kms_ctx_result(&dkctx->kms, &dkctx->encrypted_key_material)) {
        BSON_ASSERT(!mongocrypt_kms_ctx_status(&dkctx->kms, ctx->status));
        return _mongocrypt_ctx_fail(ctx);
    }

    /* The encrypted key material must be at least as large as the plaintext. */
    if (dkctx->encrypted_key_material.len < MONGOCRYPT_KEY_LEN) {
        return _mongocrypt_ctx_fail_w_msg(ctx, "key material not expected length");
    }

    ctx->state = MONGOCRYPT_CTX_READY;
    return true;
}

/* Append a UUID _id. Confer with libmongoc's `_mongoc_server_session_uuid`. */
static bool _append_id(mongocrypt_t *crypt, bson_t *bson, mongocrypt_status_t *status) {
    _mongocrypt_buffer_t uuid;

    BSON_ASSERT_PARAM(crypt);
    BSON_ASSERT_PARAM(bson);

    _mongocrypt_buffer_init(&uuid);
    uuid.data = bson_malloc(UUID_LEN);
    BSON_ASSERT(uuid.data);

    uuid.len = UUID_LEN;
    uuid.subtype = BSON_SUBTYPE_UUID;
    uuid.owned = true;

    if (!_mongocrypt_random(crypt->crypto, &uuid, UUID_LEN, status)) {
        _mongocrypt_buffer_cleanup(&uuid);
        return false;
    }

    uuid.data[6] = (uint8_t)(0x40 | (uuid.data[6] & 0xf));
    uuid.data[8] = (uint8_t)(0x80 | (uuid.data[8] & 0x3f));
    if (!_mongocrypt_buffer_append(&uuid, bson, "_id", 3)) {
        _mongocrypt_buffer_cleanup(&uuid);
        return false;
    }

    _mongocrypt_buffer_cleanup(&uuid);

    return true;
}

static bool _finalize(mongocrypt_ctx_t *ctx, mongocrypt_binary_t *out) {
    _mongocrypt_ctx_datakey_t *dkctx;
    bson_t key_doc, child;
    struct timeval tp;

    BSON_ASSERT_PARAM(ctx);
    BSON_ASSERT_PARAM(out);

#define BSON_CHECK(_stmt)                                                                                              \
    if (!(_stmt)) {                                                                                                    \
        bson_destroy(&key_doc);                                                                                        \
        return _mongocrypt_ctx_fail_w_msg(ctx, "unable to construct BSON doc");                                        \
    }

    dkctx = (_mongocrypt_ctx_datakey_t *)ctx;

    bson_init(&key_doc);
    if (!_append_id(ctx->crypt, &key_doc, ctx->status)) {
        return _mongocrypt_ctx_fail(ctx);
    }

    if (ctx->opts.key_alt_names) {
        _mongocrypt_key_alt_name_t *alt_name = ctx->opts.key_alt_names;
        int i;

        bson_append_array_begin(&key_doc, "keyAltNames", -1, &child);
        for (i = 0; alt_name; i++) {
            char *key = bson_strdup_printf("%d", i);
            bson_append_value(&child, key, -1, &alt_name->value);
            bson_free(key);
            alt_name = alt_name->next;
        }
        bson_append_array_end(&key_doc, &child);
    }
    if (!_mongocrypt_buffer_append(&dkctx->encrypted_key_material, &key_doc, MONGOCRYPT_STR_AND_LEN("keyMaterial"))) {
        bson_destroy(&key_doc);
        return _mongocrypt_ctx_fail_w_msg(ctx, "could not append keyMaterial");
    }
    bson_gettimeofday(&tp);
    BSON_CHECK(bson_append_timeval(&key_doc, MONGOCRYPT_STR_AND_LEN("creationDate"), &tp));
    BSON_CHECK(bson_append_timeval(&key_doc, MONGOCRYPT_STR_AND_LEN("updateDate"), &tp));
    BSON_CHECK(bson_append_int32(&key_doc, MONGOCRYPT_STR_AND_LEN("status"), 0)); /* 0 = enabled. */
    BSON_CHECK(bson_append_document_begin(&key_doc, MONGOCRYPT_STR_AND_LEN("masterKey"), &child));
    if (!_mongocrypt_kek_append(&ctx->opts.kek, &child, ctx->status)) {
        bson_destroy(&key_doc);
        return _mongocrypt_ctx_fail(ctx);
    }
    BSON_CHECK(bson_append_document_end(&key_doc, &child));
    _mongocrypt_buffer_steal_from_bson(&dkctx->key_doc, &key_doc);
    _mongocrypt_buffer_to_binary(&dkctx->key_doc, out);
    ctx->state = MONGOCRYPT_CTX_DONE;
    return true;
}

bool mongocrypt_ctx_datakey_init(mongocrypt_ctx_t *ctx) {
    _mongocrypt_ctx_datakey_t *dkctx;
    _mongocrypt_ctx_opts_spec_t opts_spec;
    bool ret;

    if (!ctx) {
        return false;
    }
    ret = false;
    memset(&opts_spec, 0, sizeof(opts_spec));
    opts_spec.kek = OPT_REQUIRED;
    opts_spec.key_alt_names = OPT_OPTIONAL;
    opts_spec.key_material = OPT_OPTIONAL;

    if (!_mongocrypt_ctx_init(ctx, &opts_spec)) {
        return false;
    }

    dkctx = (_mongocrypt_ctx_datakey_t *)ctx;
    ctx->type = _MONGOCRYPT_TYPE_CREATE_DATA_KEY;
    ctx->vtable.mongo_op_keys = NULL;
    ctx->vtable.mongo_feed_keys = NULL;
    ctx->vtable.mongo_done_keys = NULL;
    ctx->vtable.next_kms_ctx = _next_kms_ctx;
    ctx->vtable.after_kms_credentials_provided = _kms_start;
    ctx->vtable.kms_done = _kms_done;
    ctx->vtable.finalize = _finalize;
    ctx->vtable.cleanup = _cleanup;

    _mongocrypt_buffer_init(&dkctx->plaintext_key_material);

    if (ctx->opts.key_material.owned) {
        /* Use keyMaterial provided by user via DataKeyOpts. */
        _mongocrypt_buffer_steal(&dkctx->plaintext_key_material, &ctx->opts.key_material);
    } else {
        /* Generate keyMaterial instead. */
        dkctx->plaintext_key_material.data = bson_malloc(MONGOCRYPT_KEY_LEN);
        BSON_ASSERT(dkctx->plaintext_key_material.data);
        dkctx->plaintext_key_material.len = MONGOCRYPT_KEY_LEN;
        dkctx->plaintext_key_material.owned = true;
        if (!_mongocrypt_random(ctx->crypt->crypto, &dkctx->plaintext_key_material, MONGOCRYPT_KEY_LEN, ctx->status)) {
            _mongocrypt_ctx_fail(ctx);
            goto done;
        }
    }

    if (_mongocrypt_needs_credentials_for_provider(ctx->crypt, ctx->opts.kek.kms_provider)) {
        ctx->state = MONGOCRYPT_CTX_NEED_KMS_CREDENTIALS;
    } else if (!_kms_start(ctx)) {
        goto done;
    }

    ret = true;
done:
    return ret;
}

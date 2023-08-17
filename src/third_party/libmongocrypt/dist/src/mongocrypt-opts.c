/*
 * Copyright 2018-present MongoDB, Inc.
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

#include "mongocrypt-log-private.h"
#include "mongocrypt-opts-private.h"
#include "mongocrypt-private.h"

#include <kms_message/kms_b64.h>

void _mongocrypt_opts_init(_mongocrypt_opts_t *opts) {
    BSON_ASSERT_PARAM(opts);
    memset(opts, 0, sizeof(*opts));
}

static void _mongocrypt_opts_kms_provider_azure_cleanup(_mongocrypt_opts_kms_provider_azure_t *kms_provider_azure) {
    if (!kms_provider_azure) {
        return;
    }
    bson_free(kms_provider_azure->client_id);
    bson_free(kms_provider_azure->client_secret);
    bson_free(kms_provider_azure->tenant_id);
    bson_free(kms_provider_azure->access_token);
    _mongocrypt_endpoint_destroy(kms_provider_azure->identity_platform_endpoint);
}

static void _mongocrypt_opts_kms_provider_gcp_cleanup(_mongocrypt_opts_kms_provider_gcp_t *kms_provider_gcp) {
    if (!kms_provider_gcp) {
        return;
    }
    bson_free(kms_provider_gcp->email);
    _mongocrypt_endpoint_destroy(kms_provider_gcp->endpoint);
    _mongocrypt_buffer_cleanup(&kms_provider_gcp->private_key);
    bson_free(kms_provider_gcp->access_token);
}

void _mongocrypt_opts_kms_providers_cleanup(_mongocrypt_opts_kms_providers_t *kms_providers) {
    if (!kms_providers) {
        return;
    }
    bson_free(kms_providers->aws.secret_access_key);
    bson_free(kms_providers->aws.access_key_id);
    bson_free(kms_providers->aws.session_token);
    _mongocrypt_buffer_cleanup(&kms_providers->local.key);
    _mongocrypt_opts_kms_provider_azure_cleanup(&kms_providers->azure);
    _mongocrypt_opts_kms_provider_gcp_cleanup(&kms_providers->gcp);
    _mongocrypt_endpoint_destroy(kms_providers->kmip.endpoint);
}

void _mongocrypt_opts_merge_kms_providers(_mongocrypt_opts_kms_providers_t *dest,
                                          const _mongocrypt_opts_kms_providers_t *source) {
    BSON_ASSERT_PARAM(dest);
    BSON_ASSERT_PARAM(source);

    if (source->configured_providers & MONGOCRYPT_KMS_PROVIDER_AWS) {
        memcpy(&dest->aws, &source->aws, sizeof(source->aws));
        dest->configured_providers |= MONGOCRYPT_KMS_PROVIDER_AWS;
    }
    if (source->configured_providers & MONGOCRYPT_KMS_PROVIDER_LOCAL) {
        memcpy(&dest->local, &source->local, sizeof(source->local));
        dest->configured_providers |= MONGOCRYPT_KMS_PROVIDER_LOCAL;
    }
    if (source->configured_providers & MONGOCRYPT_KMS_PROVIDER_AZURE) {
        memcpy(&dest->azure, &source->azure, sizeof(source->azure));
        dest->configured_providers |= MONGOCRYPT_KMS_PROVIDER_AZURE;
    }
    if (source->configured_providers & MONGOCRYPT_KMS_PROVIDER_GCP) {
        memcpy(&dest->gcp, &source->gcp, sizeof(source->gcp));
        dest->configured_providers |= MONGOCRYPT_KMS_PROVIDER_GCP;
    }
    if (source->configured_providers & MONGOCRYPT_KMS_PROVIDER_KMIP) {
        memcpy(&dest->kmip, &source->kmip, sizeof(source->kmip));
        dest->configured_providers |= MONGOCRYPT_KMS_PROVIDER_KMIP;
    }
    /* ensure all providers were copied */
    BSON_ASSERT(!(source->configured_providers & ~dest->configured_providers));
}

void _mongocrypt_opts_cleanup(_mongocrypt_opts_t *opts) {
    if (!opts) {
        return;
    }
    _mongocrypt_opts_kms_providers_cleanup(&opts->kms_providers);
    _mongocrypt_buffer_cleanup(&opts->schema_map);
    _mongocrypt_buffer_cleanup(&opts->encrypted_field_config_map);
    // Free any lib search paths added by the caller
    for (int i = 0; i < opts->n_crypt_shared_lib_search_paths; ++i) {
        mstr_free(opts->crypt_shared_lib_search_paths[i]);
    }
    bson_free(opts->crypt_shared_lib_search_paths);
    mstr_free(opts->crypt_shared_lib_override_path);
}

bool _mongocrypt_opts_kms_providers_validate(_mongocrypt_opts_t *opts,
                                             _mongocrypt_opts_kms_providers_t *kms_providers,
                                             mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(opts);
    BSON_ASSERT_PARAM(kms_providers);

    if (!kms_providers->configured_providers && !kms_providers->need_credentials) {
        CLIENT_ERR("no kms provider set");
        return false;
    }

    if (kms_providers->configured_providers & MONGOCRYPT_KMS_PROVIDER_AWS) {
        if (!kms_providers->aws.access_key_id || !kms_providers->aws.secret_access_key) {
            CLIENT_ERR("aws credentials unset");
            return false;
        }
    }

    if (kms_providers->configured_providers & MONGOCRYPT_KMS_PROVIDER_LOCAL) {
        if (_mongocrypt_buffer_empty(&kms_providers->local.key)) {
            CLIENT_ERR("local data key unset");
            return false;
        }
    }

    if (kms_providers->need_credentials && !opts->use_need_kms_credentials_state) {
        CLIENT_ERR("on-demand credentials not enabled");
        return false;
    }

    return true;
}

/* _shares_bson_fields checks if @one or @two share any top-level field names.
 * Returns false on error and sets @status. Returns true if no error
 * occurred. Sets @found to the first shared field name found.
 * If no shared field names are found, @found is set to NULL.
 */
static bool _shares_bson_fields(bson_t *one, bson_t *two, const char **found, mongocrypt_status_t *status) {
    bson_iter_t iter1;
    bson_iter_t iter2;

    BSON_ASSERT_PARAM(one);
    BSON_ASSERT_PARAM(two);
    BSON_ASSERT_PARAM(found);
    *found = NULL;
    if (!bson_iter_init(&iter1, one)) {
        CLIENT_ERR("error iterating one BSON in _shares_bson_fields");
        return false;
    }
    while (bson_iter_next(&iter1)) {
        const char *key1 = bson_iter_key(&iter1);

        if (!bson_iter_init(&iter2, two)) {
            CLIENT_ERR("error iterating two BSON in _shares_bson_fields");
            return false;
        }
        while (bson_iter_next(&iter2)) {
            const char *key2 = bson_iter_key(&iter2);
            if (0 == strcmp(key1, key2)) {
                *found = key1;
                return true;
            }
        }
    }
    return true;
}

/* _validate_encrypted_field_config_map_and_schema_map validates that the same
 * namespace is not both in encrypted_field_config_map and schema_map. */
static bool _validate_encrypted_field_config_map_and_schema_map(_mongocrypt_buffer_t *encrypted_field_config_map,
                                                                _mongocrypt_buffer_t *schema_map,
                                                                mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(encrypted_field_config_map);
    BSON_ASSERT_PARAM(schema_map);

    const char *found;
    bson_t schema_map_bson;
    bson_t encrypted_field_config_map_bson;

    /* If either map is unset, there is nothing to validate. Return true to
     * signal no error. */
    if (_mongocrypt_buffer_empty(encrypted_field_config_map)) {
        return true;
    }
    if (_mongocrypt_buffer_empty(schema_map)) {
        return true;
    }

    if (!_mongocrypt_buffer_to_bson(schema_map, &schema_map_bson)) {
        CLIENT_ERR("error converting schema_map to BSON");
        return false;
    }
    if (!_mongocrypt_buffer_to_bson(encrypted_field_config_map, &encrypted_field_config_map_bson)) {
        CLIENT_ERR("error converting encrypted_field_config_map to BSON");
        return false;
    }
    if (!_shares_bson_fields(&schema_map_bson, &encrypted_field_config_map_bson, &found, status)) {
        return false;
    }
    if (found != NULL) {
        CLIENT_ERR("%s is present in both schema_map and encrypted_field_config_map", found);
        return false;
    }
    return true;
}

bool _mongocrypt_opts_validate(_mongocrypt_opts_t *opts, mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(opts);

    if (!_validate_encrypted_field_config_map_and_schema_map(&opts->encrypted_field_config_map,
                                                             &opts->schema_map,
                                                             status)) {
        return false;
    }
    return _mongocrypt_opts_kms_providers_validate(opts, &opts->kms_providers, status);
}

bool _mongocrypt_parse_optional_utf8(const bson_t *bson, const char *dotkey, char **out, mongocrypt_status_t *status) {
    bson_iter_t iter;
    bson_iter_t child;

    BSON_ASSERT_PARAM(bson);
    BSON_ASSERT_PARAM(dotkey);
    BSON_ASSERT_PARAM(out);

    *out = NULL;

    if (!bson_iter_init(&iter, bson)) {
        CLIENT_ERR("invalid BSON");
        return false;
    }
    if (!bson_iter_find_descendant(&iter, dotkey, &child)) {
        /* Not found. Not an error. */
        return true;
    }
    if (!BSON_ITER_HOLDS_UTF8(&child)) {
        CLIENT_ERR("expected UTF-8 %s", dotkey);
        return false;
    }

    *out = bson_strdup(bson_iter_utf8(&child, NULL));
    return true;
}

bool _mongocrypt_parse_required_utf8(const bson_t *bson, const char *dotkey, char **out, mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(bson);
    BSON_ASSERT_PARAM(dotkey);
    BSON_ASSERT_PARAM(out);

    if (!_mongocrypt_parse_optional_utf8(bson, dotkey, out, status)) {
        return false;
    }

    if (!*out) {
        CLIENT_ERR("expected UTF-8 %s", dotkey);
        return false;
    }

    return true;
}

bool _mongocrypt_parse_optional_endpoint(const bson_t *bson,
                                         const char *dotkey,
                                         _mongocrypt_endpoint_t **out,
                                         _mongocrypt_endpoint_parse_opts_t *opts,
                                         mongocrypt_status_t *status) {
    char *endpoint_raw;

    BSON_ASSERT_PARAM(bson);
    BSON_ASSERT_PARAM(dotkey);
    BSON_ASSERT_PARAM(out);

    *out = NULL;

    if (!_mongocrypt_parse_optional_utf8(bson, dotkey, &endpoint_raw, status)) {
        return false;
    }

    /* Not found. Not an error. */
    if (!endpoint_raw) {
        return true;
    }

    *out = _mongocrypt_endpoint_new(endpoint_raw, -1, opts, status);
    bson_free(endpoint_raw);
    return (*out) != NULL;
}

bool _mongocrypt_parse_required_endpoint(const bson_t *bson,
                                         const char *dotkey,
                                         _mongocrypt_endpoint_t **out,
                                         _mongocrypt_endpoint_parse_opts_t *opts,
                                         mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(bson);
    BSON_ASSERT_PARAM(dotkey);
    BSON_ASSERT_PARAM(out);

    if (!_mongocrypt_parse_optional_endpoint(bson, dotkey, out, opts, status)) {
        return false;
    }

    if (!*out) {
        CLIENT_ERR("expected endpoint %s", dotkey);
        return false;
    }

    return true;
}

bool _mongocrypt_parse_optional_binary(const bson_t *bson,
                                       const char *dotkey,
                                       _mongocrypt_buffer_t *out,
                                       mongocrypt_status_t *status) {
    bson_iter_t iter;
    bson_iter_t child;

    BSON_ASSERT_PARAM(bson);
    BSON_ASSERT_PARAM(dotkey);
    BSON_ASSERT_PARAM(out);

    _mongocrypt_buffer_init(out);

    if (!bson_iter_init(&iter, bson)) {
        CLIENT_ERR("invalid BSON");
        return false;
    }
    if (!bson_iter_find_descendant(&iter, dotkey, &child)) {
        /* Not found. Not an error. */
        return true;
    }
    if (BSON_ITER_HOLDS_UTF8(&child)) {
        size_t out_len;
        /* Attempt to base64 decode. */
        out->data = kms_message_b64_to_raw(bson_iter_utf8(&child, NULL), &out_len);
        if (!out->data) {
            CLIENT_ERR("unable to parse base64 from UTF-8 field %s", dotkey);
            return false;
        }
        BSON_ASSERT(out_len <= UINT32_MAX);
        out->len = (uint32_t)out_len;
        out->owned = true;
    } else if (BSON_ITER_HOLDS_BINARY(&child)) {
        if (!_mongocrypt_buffer_copy_from_binary_iter(out, &child)) {
            CLIENT_ERR("unable to parse binary from field %s", dotkey);
            return false;
        }
    } else {
        CLIENT_ERR("expected UTF-8 or binary %s", dotkey);
        return false;
    }

    return true;
}

bool _mongocrypt_parse_required_binary(const bson_t *bson,
                                       const char *dotkey,
                                       _mongocrypt_buffer_t *out,
                                       mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(bson);
    BSON_ASSERT_PARAM(dotkey);
    BSON_ASSERT_PARAM(out);

    if (!_mongocrypt_parse_optional_binary(bson, dotkey, out, status)) {
        return false;
    }

    if (out->len == 0) {
        CLIENT_ERR("expected UTF-8 or binary %s", dotkey);
        return false;
    }

    return true;
}

bool _mongocrypt_check_allowed_fields_va(const bson_t *bson, const char *dotkey, mongocrypt_status_t *status, ...) {
    va_list args;
    const char *field;
    bson_iter_t iter;

    BSON_ASSERT_PARAM(bson);

    if (dotkey) {
        bson_iter_t parent;

        bson_iter_init(&parent, bson);
        if (!bson_iter_find_descendant(&parent, dotkey, &iter) || !BSON_ITER_HOLDS_DOCUMENT(&iter)) {
            CLIENT_ERR("invalid BSON, expected %s", dotkey);
            return false;
        }
        bson_iter_recurse(&iter, &iter);
    } else {
        bson_iter_init(&iter, bson);
    }

    while (bson_iter_next(&iter)) {
        bool found = false;

        va_start(args, status);
        field = va_arg(args, const char *);
        while (field) {
            if (0 == strcmp(field, bson_iter_key(&iter))) {
                found = true;
                break;
            }
            field = va_arg(args, const char *);
        }
        va_end(args);

        if (!found) {
            CLIENT_ERR("Unexpected field: '%s'", bson_iter_key(&iter));
            return false;
        }
    }
    return true;
}

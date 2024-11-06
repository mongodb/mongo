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
#include <mongocrypt-util-private.h> // mc_iter_document_as_bson

#include <kms_message/kms_b64.h>

typedef struct {
    mc_kms_creds_t creds;
    char *kmsid;
} mc_kms_creds_with_id_t;

void _mongocrypt_opts_kms_providers_init(_mongocrypt_opts_kms_providers_t *kms_providers) {
    _mc_array_init(&kms_providers->named_mut, sizeof(mc_kms_creds_with_id_t));
}

void _mongocrypt_opts_init(_mongocrypt_opts_t *opts) {
    BSON_ASSERT_PARAM(opts);
    memset(opts, 0, sizeof(*opts));
    opts->use_range_v2 = true;
    _mongocrypt_opts_kms_providers_init(&opts->kms_providers);
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

static void _mongocrypt_opts_kms_provider_local_cleanup(_mongocrypt_opts_kms_provider_local_t *kms_provider_local) {
    _mongocrypt_buffer_cleanup(&kms_provider_local->key);
}

static void _mongocrypt_opts_kms_provider_aws_cleanup(_mongocrypt_opts_kms_provider_aws_t *kms_provider_aws) {
    bson_free(kms_provider_aws->secret_access_key);
    bson_free(kms_provider_aws->access_key_id);
    bson_free(kms_provider_aws->session_token);
}

static void _mongocrypt_opts_kms_provider_kmip_cleanup(_mongocrypt_opts_kms_provider_kmip_t *kms_provider_kmip) {
    _mongocrypt_endpoint_destroy(kms_provider_kmip->endpoint);
}

void _mongocrypt_opts_kms_providers_cleanup(_mongocrypt_opts_kms_providers_t *kms_providers) {
    if (!kms_providers) {
        return;
    }
    _mongocrypt_opts_kms_provider_aws_cleanup(&kms_providers->aws_mut);
    _mongocrypt_opts_kms_provider_local_cleanup(&kms_providers->local_mut);
    _mongocrypt_opts_kms_provider_azure_cleanup(&kms_providers->azure_mut);
    _mongocrypt_opts_kms_provider_gcp_cleanup(&kms_providers->gcp_mut);
    _mongocrypt_opts_kms_provider_kmip_cleanup(&kms_providers->kmip_mut);
    for (size_t i = 0; i < kms_providers->named_mut.len; i++) {
        mc_kms_creds_with_id_t kcwid = _mc_array_index(&kms_providers->named_mut, mc_kms_creds_with_id_t, i);
        switch (kcwid.creds.type) {
        default:
        case MONGOCRYPT_KMS_PROVIDER_NONE: break;
        case MONGOCRYPT_KMS_PROVIDER_AWS: {
            _mongocrypt_opts_kms_provider_aws_cleanup(&kcwid.creds.value.aws);
            break;
        }
        case MONGOCRYPT_KMS_PROVIDER_LOCAL: {
            _mongocrypt_opts_kms_provider_local_cleanup(&kcwid.creds.value.local);
            break;
        }
        case MONGOCRYPT_KMS_PROVIDER_AZURE: {
            _mongocrypt_opts_kms_provider_azure_cleanup(&kcwid.creds.value.azure);
            break;
        }
        case MONGOCRYPT_KMS_PROVIDER_GCP: {
            _mongocrypt_opts_kms_provider_gcp_cleanup(&kcwid.creds.value.gcp);
            break;
        }
        case MONGOCRYPT_KMS_PROVIDER_KMIP: {
            _mongocrypt_endpoint_destroy(kcwid.creds.value.kmip.endpoint);
            break;
        }
        }
        bson_free(kcwid.kmsid);
    }
    _mc_array_destroy(&kms_providers->named_mut);
}

void _mongocrypt_opts_merge_kms_providers(_mongocrypt_opts_kms_providers_t *dest,
                                          const _mongocrypt_opts_kms_providers_t *source) {
    BSON_ASSERT_PARAM(dest);
    BSON_ASSERT_PARAM(source);

    if (source->configured_providers & MONGOCRYPT_KMS_PROVIDER_AWS) {
        memcpy(&dest->aws_mut, &source->aws_mut, sizeof(source->aws_mut));
        dest->configured_providers |= MONGOCRYPT_KMS_PROVIDER_AWS;
    }
    if (source->configured_providers & MONGOCRYPT_KMS_PROVIDER_LOCAL) {
        memcpy(&dest->local_mut, &source->local_mut, sizeof(source->local_mut));
        dest->configured_providers |= MONGOCRYPT_KMS_PROVIDER_LOCAL;
    }
    if (source->configured_providers & MONGOCRYPT_KMS_PROVIDER_AZURE) {
        memcpy(&dest->azure_mut, &source->azure_mut, sizeof(source->azure_mut));
        dest->configured_providers |= MONGOCRYPT_KMS_PROVIDER_AZURE;
    }
    if (source->configured_providers & MONGOCRYPT_KMS_PROVIDER_GCP) {
        memcpy(&dest->gcp_mut, &source->gcp_mut, sizeof(source->gcp_mut));
        dest->configured_providers |= MONGOCRYPT_KMS_PROVIDER_GCP;
    }
    if (source->configured_providers & MONGOCRYPT_KMS_PROVIDER_KMIP) {
        memcpy(&dest->kmip_mut, &source->kmip_mut, sizeof(source->kmip_mut));
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

    if (!kms_providers->configured_providers && !kms_providers->need_credentials && kms_providers->named_mut.len == 0) {
        CLIENT_ERR("no kms provider set");
        return false;
    }

    if (kms_providers->configured_providers & MONGOCRYPT_KMS_PROVIDER_AWS) {
        if (!kms_providers->aws_mut.access_key_id || !kms_providers->aws_mut.secret_access_key) {
            CLIENT_ERR("aws credentials unset");
            return false;
        }
    }

    if (kms_providers->configured_providers & MONGOCRYPT_KMS_PROVIDER_LOCAL) {
        if (_mongocrypt_buffer_empty(&kms_providers->local_mut.key)) {
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

bool _mongocrypt_opts_kms_providers_lookup(const _mongocrypt_opts_kms_providers_t *kms_providers,
                                           const char *kmsid,
                                           mc_kms_creds_t *out) {
    *out = (mc_kms_creds_t){0};
    if (0 != (kms_providers->configured_providers & MONGOCRYPT_KMS_PROVIDER_AWS) && 0 == strcmp(kmsid, "aws")) {
        out->type = MONGOCRYPT_KMS_PROVIDER_AWS;
        out->value.aws = kms_providers->aws_mut;
        return true;
    }
    if (0 != (kms_providers->configured_providers & MONGOCRYPT_KMS_PROVIDER_AZURE) && 0 == strcmp(kmsid, "azure")) {
        out->type = MONGOCRYPT_KMS_PROVIDER_AZURE;
        out->value.azure = kms_providers->azure_mut;
        return true;
    }

    if (0 != (kms_providers->configured_providers & MONGOCRYPT_KMS_PROVIDER_GCP) && 0 == strcmp(kmsid, "gcp")) {
        out->type = MONGOCRYPT_KMS_PROVIDER_GCP;
        out->value.gcp = kms_providers->gcp_mut;
        return true;
    }

    if (0 != (kms_providers->configured_providers & MONGOCRYPT_KMS_PROVIDER_LOCAL) && 0 == strcmp(kmsid, "local")) {
        out->type = MONGOCRYPT_KMS_PROVIDER_LOCAL;
        out->value.local = kms_providers->local_mut;
        return true;
    }

    if (0 != (kms_providers->configured_providers & MONGOCRYPT_KMS_PROVIDER_KMIP) && 0 == strcmp(kmsid, "kmip")) {
        out->type = MONGOCRYPT_KMS_PROVIDER_KMIP;
        out->value.kmip = kms_providers->kmip_mut;
        return true;
    }

    // Check for KMS providers with a name.
    for (size_t i = 0; i < kms_providers->named_mut.len; i++) {
        mc_kms_creds_with_id_t kcwi = _mc_array_index(&kms_providers->named_mut, mc_kms_creds_with_id_t, i);
        if (0 == strcmp(kmsid, kcwi.kmsid)) {
            *out = kcwi.creds;
            return true;
        }
    }

    return false;
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

bool _mongocrypt_parse_optional_bool(const bson_t *bson, const char *dotkey, bool *out, mongocrypt_status_t *status) {
    bson_iter_t iter;
    bson_iter_t child;

    BSON_ASSERT_PARAM(bson);
    BSON_ASSERT_PARAM(dotkey);
    BSON_ASSERT_PARAM(out);

    *out = false;

    if (!bson_iter_init(&iter, bson)) {
        CLIENT_ERR("invalid BSON");
        return false;
    }
    if (!bson_iter_find_descendant(&iter, dotkey, &child)) {
        /* Not found. Not an error. */
        return true;
    }
    if (!BSON_ITER_HOLDS_BOOL(&child)) {
        CLIENT_ERR("expected bool %s", dotkey);
        return false;
    }

    *out = bson_iter_bool(&child);
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

#define KEY_HELP "Expected `<type>` or `<type>:<name>`. Example: `local` or `local:name`."

bool mc_kmsid_parse(const char *kmsid,
                    _mongocrypt_kms_provider_t *type_out,
                    const char **name_out,
                    mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(kmsid);
    BSON_ASSERT_PARAM(type_out);
    BSON_ASSERT_PARAM(name_out);
    BSON_ASSERT(status || true); // Optional.

    *type_out = MONGOCRYPT_KMS_PROVIDER_NONE;
    *name_out = NULL;

    const char *type_end = strstr(kmsid, ":");
    size_t type_nchars;

    if (type_end == NULL) {
        // Parse `kmsid` as `<type>`.
        type_nchars = strlen(kmsid);
    } else {
        // Parse `kmsid` as `<type>:<name>`.
        ptrdiff_t diff = type_end - kmsid;
        BSON_ASSERT(diff >= 0 && (uint64_t)diff < SIZE_MAX);
        type_nchars = (size_t)diff;
    }

    if (0 == strncmp("aws", kmsid, type_nchars)) {
        *type_out = MONGOCRYPT_KMS_PROVIDER_AWS;
    } else if (0 == strncmp("azure", kmsid, type_nchars)) {
        *type_out = MONGOCRYPT_KMS_PROVIDER_AZURE;
    } else if (0 == strncmp("gcp", kmsid, type_nchars)) {
        *type_out = MONGOCRYPT_KMS_PROVIDER_GCP;
    } else if (0 == strncmp("kmip", kmsid, type_nchars)) {
        *type_out = MONGOCRYPT_KMS_PROVIDER_KMIP;
    } else if (0 == strncmp("local", kmsid, type_nchars)) {
        *type_out = MONGOCRYPT_KMS_PROVIDER_LOCAL;
    } else {
        CLIENT_ERR("unrecognized KMS provider `%s`: unrecognized type. " KEY_HELP, kmsid);
        return false;
    }

    if (type_end != NULL) {
        // Parse name.
        *name_out = type_end + 1;
        if (0 == strlen(*name_out)) {
            CLIENT_ERR("unrecognized KMS provider `%s`: empty name. " KEY_HELP, kmsid);
            return false;
        }

        // Validate name only contains: [a-zA-Z0-9_]
        for (const char *cp = *name_out; *cp != '\0'; cp++) {
            char c = *cp;
            if (c >= 'a' && c <= 'z') {
                continue;
            }
            if (c >= 'A' && c <= 'Z') {
                continue;
            }
            if (c >= '0' && c <= '9') {
                continue;
            }
            if (c == '_') {
                continue;
            }
            CLIENT_ERR("unrecognized KMS provider `%s`: unsupported character `%c`. Must be of the form `<provider "
                       "type>:<name>` where `<name>` only contain characters [a-zA-Z0-9_]",
                       kmsid,
                       c);
            return false;
        }
    }
    return true;
}

static bool _mongocrypt_opts_kms_provider_local_parse(_mongocrypt_opts_kms_provider_local_t *local,
                                                      const char *kmsid,
                                                      const bson_t *def,
                                                      mongocrypt_status_t *status) {
    bool ok = false;
    if (!_mongocrypt_parse_required_binary(def, "key", &local->key, status)) {
        goto fail;
    }

    if (local->key.len != MONGOCRYPT_KEY_LEN) {
        CLIENT_ERR("local key must be %d bytes", MONGOCRYPT_KEY_LEN);
        goto fail;
    }

    if (!_mongocrypt_check_allowed_fields(def, NULL /* root */, status, "key")) {
        goto fail;
    }
    ok = true;
fail:
    if (!ok) {
        // Wrap error to identify the failing `kmsid`.
        CLIENT_ERR("Failed to parse KMS provider `%s`: %s", kmsid, mongocrypt_status_message(status, NULL /* len */));
    }
    return ok;
}

static bool _mongocrypt_opts_kms_provider_azure_parse(_mongocrypt_opts_kms_provider_azure_t *azure,
                                                      const char *kmsid,
                                                      const bson_t *def,
                                                      mongocrypt_status_t *status) {
    bool ok = false;
    if (!_mongocrypt_parse_optional_utf8(def, "accessToken", &azure->access_token, status)) {
        goto done;
    }

    if (azure->access_token) {
        // Caller provides an accessToken directly
        if (!_mongocrypt_check_allowed_fields(def, NULL /* root */, status, "accessToken")) {
            goto done;
        }
        ok = true;
        goto done;
    }

    // No accessToken given, so we'll need to look one up on our own later
    // using the Azure API

    if (!_mongocrypt_parse_required_utf8(def, "tenantId", &azure->tenant_id, status)) {
        goto done;
    }

    if (!_mongocrypt_parse_required_utf8(def, "clientId", &azure->client_id, status)) {
        goto done;
    }

    if (!_mongocrypt_parse_required_utf8(def, "clientSecret", &azure->client_secret, status)) {
        goto done;
    }

    if (!_mongocrypt_parse_optional_endpoint(def,
                                             "identityPlatformEndpoint",
                                             &azure->identity_platform_endpoint,
                                             NULL /* opts */,
                                             status)) {
        goto done;
    }

    if (!_mongocrypt_check_allowed_fields(def,
                                          NULL /* root */,
                                          status,
                                          "tenantId",
                                          "clientId",
                                          "clientSecret",
                                          "identityPlatformEndpoint")) {
        goto done;
    }

    ok = true;
done:
    if (!ok) {
        // Wrap error to identify the failing `kmsid`.
        CLIENT_ERR("Failed to parse KMS provider `%s`: %s", kmsid, mongocrypt_status_message(status, NULL /* len */));
    }
    return ok;
}

static bool _mongocrypt_opts_kms_provider_gcp_parse(_mongocrypt_opts_kms_provider_gcp_t *gcp,
                                                    const char *kmsid,
                                                    const bson_t *def,
                                                    mongocrypt_status_t *status) {
    bool ok = false;
    if (!_mongocrypt_parse_optional_utf8(def, "accessToken", &gcp->access_token, status)) {
        goto done;
    }

    if (gcp->access_token) {
        // Caller provides an accessToken directly
        if (!_mongocrypt_check_allowed_fields(def, NULL /* root */, status, "accessToken")) {
            goto done;
        }
        ok = true;
        goto done;
    }

    // No accessToken given, so we'll need to look one up on our own later
    // using the GCP API

    if (!_mongocrypt_parse_required_utf8(def, "email", &gcp->email, status)) {
        goto done;
    }

    if (!_mongocrypt_parse_required_binary(def, "privateKey", &gcp->private_key, status)) {
        goto done;
    }

    if (!_mongocrypt_parse_optional_endpoint(def, "endpoint", &gcp->endpoint, NULL /* opts */, status)) {
        goto done;
    }

    if (!_mongocrypt_check_allowed_fields(def, NULL /* root */, status, "email", "privateKey", "endpoint")) {
        goto done;
    }

    ok = true;
done:
    if (!ok) {
        // Wrap error to identify the failing `kmsid`.
        CLIENT_ERR("Failed to parse KMS provider `%s`: %s", kmsid, mongocrypt_status_message(status, NULL /* len */));
    }
    return ok;
}

static bool _mongocrypt_opts_kms_provider_aws_parse(_mongocrypt_opts_kms_provider_aws_t *aws,
                                                    const char *kmsid,
                                                    const bson_t *def,
                                                    mongocrypt_status_t *status) {
    bool ok = false;

    if (!_mongocrypt_parse_required_utf8(def, "accessKeyId", &aws->access_key_id, status)) {
        goto done;
    }
    if (!_mongocrypt_parse_required_utf8(def, "secretAccessKey", &aws->secret_access_key, status)) {
        goto done;
    }

    if (!_mongocrypt_parse_optional_utf8(def, "sessionToken", &aws->session_token, status)) {
        goto done;
    }

    if (!_mongocrypt_check_allowed_fields(def,
                                          NULL /* root */,
                                          status,
                                          "accessKeyId",
                                          "secretAccessKey",
                                          "sessionToken")) {
        goto done;
    }

    ok = true;
done:
    if (!ok) {
        // Wrap error to identify the failing `kmsid`.
        CLIENT_ERR("Failed to parse KMS provider `%s`: %s", kmsid, mongocrypt_status_message(status, NULL /* len */));
    }
    return ok;
}

static bool _mongocrypt_opts_kms_provider_kmip_parse(_mongocrypt_opts_kms_provider_kmip_t *kmip,
                                                     const char *kmsid,
                                                     const bson_t *def,
                                                     mongocrypt_status_t *status) {
    bool ok = false;

    _mongocrypt_endpoint_parse_opts_t opts = {0};

    opts.allow_empty_subdomain = true;
    if (!_mongocrypt_parse_required_endpoint(def, "endpoint", &kmip->endpoint, &opts, status)) {
        goto done;
    }

    if (!_mongocrypt_check_allowed_fields(def, NULL /* root */, status, "endpoint")) {
        goto done;
    }

    ok = true;
done:
    if (!ok) {
        // Wrap error to identify the failing `kmsid`.
        CLIENT_ERR("Failed to parse KMS provider `%s`: %s", kmsid, mongocrypt_status_message(status, NULL /* len */));
    }
    return ok;
}

bool _mongocrypt_parse_kms_providers(mongocrypt_binary_t *kms_providers_definition,
                                     _mongocrypt_opts_kms_providers_t *kms_providers,
                                     mongocrypt_status_t *status,
                                     _mongocrypt_log_t *log) {
    bson_t as_bson;
    bson_iter_t iter;

    BSON_ASSERT_PARAM(kms_providers_definition);
    BSON_ASSERT_PARAM(kms_providers);
    if (!_mongocrypt_binary_to_bson(kms_providers_definition, &as_bson) || !bson_iter_init(&iter, &as_bson)) {
        CLIENT_ERR("invalid BSON");
        return false;
    }

    while (bson_iter_next(&iter)) {
        const char *field_name;
        bson_t field_bson;

        field_name = bson_iter_key(&iter);
        if (!mc_iter_document_as_bson(&iter, &field_bson, status)) {
            return false;
        }

        const char *name;
        _mongocrypt_kms_provider_t type;
        if (!mc_kmsid_parse(field_name, &type, &name, status)) {
            return false;
        }

        if (name != NULL) {
            // Check if named provider already is configured.
            for (size_t i = 0; i < kms_providers->named_mut.len; i++) {
                mc_kms_creds_with_id_t kcwi = _mc_array_index(&kms_providers->named_mut, mc_kms_creds_with_id_t, i);
                if (0 == strcmp(kcwi.kmsid, field_name)) {
                    CLIENT_ERR("Got unexpected duplicate entry for KMS provider: `%s`", field_name);
                    return false;
                }
            }
            // Prohibit configuring with an empty document. Named KMS providers do not support on-demand credentials.
            if (bson_empty(&field_bson)) {
                CLIENT_ERR("Unexpected empty document for named KMS provider: '%s'. On-demand credentials are not "
                           "supported for named KMS providers.",
                           field_name);
                return false;
            }
            switch (type) {
            default:
            case MONGOCRYPT_KMS_PROVIDER_NONE: {
                CLIENT_ERR("Unexpected parsing KMS type: none");
                return false;
            }
            case MONGOCRYPT_KMS_PROVIDER_AWS: {
                _mongocrypt_opts_kms_provider_aws_t aws = {0};
                if (!_mongocrypt_opts_kms_provider_aws_parse(&aws, field_name, &field_bson, status)) {
                    _mongocrypt_opts_kms_provider_aws_cleanup(&aws);
                    return false;
                }
                mc_kms_creds_with_id_t kcwi = {.kmsid = bson_strdup(field_name),
                                               .creds = {.type = type, .value = {.aws = aws}}};
                _mc_array_append_val(&kms_providers->named_mut, kcwi);
                break;
            }
            case MONGOCRYPT_KMS_PROVIDER_LOCAL: {
                _mongocrypt_opts_kms_provider_local_t local = {
                    // specify .key to avoid erroneous missing-braces warning in GCC. Refer:
                    // https://gcc.gnu.org/bugzilla/show_bug.cgi?id=53119
                    .key = {0}};
                if (!_mongocrypt_opts_kms_provider_local_parse(&local, field_name, &field_bson, status)) {
                    _mongocrypt_opts_kms_provider_local_cleanup(&local);
                    return false;
                }
                mc_kms_creds_with_id_t kcwi = {.kmsid = bson_strdup(field_name),
                                               .creds = {.type = type, .value = {.local = local}}};
                _mc_array_append_val(&kms_providers->named_mut, kcwi);
                break;
            }
            case MONGOCRYPT_KMS_PROVIDER_AZURE: {
                _mongocrypt_opts_kms_provider_azure_t azure = {0};
                if (!_mongocrypt_opts_kms_provider_azure_parse(&azure, field_name, &field_bson, status)) {
                    _mongocrypt_opts_kms_provider_azure_cleanup(&azure);
                    return false;
                }
                mc_kms_creds_with_id_t kcwi = {.kmsid = bson_strdup(field_name),
                                               .creds = {.type = type, .value = {.azure = azure}}};
                _mc_array_append_val(&kms_providers->named_mut, kcwi);
                break;
            }
            case MONGOCRYPT_KMS_PROVIDER_GCP: {
                _mongocrypt_opts_kms_provider_gcp_t gcp = {0};
                if (!_mongocrypt_opts_kms_provider_gcp_parse(&gcp, field_name, &field_bson, status)) {
                    _mongocrypt_opts_kms_provider_gcp_cleanup(&gcp);
                    return false;
                }
                mc_kms_creds_with_id_t kcwi = {.kmsid = bson_strdup(field_name),
                                               .creds = {.type = type, .value = {.gcp = gcp}}};
                _mc_array_append_val(&kms_providers->named_mut, kcwi);
                break;
            }
            case MONGOCRYPT_KMS_PROVIDER_KMIP: {
                _mongocrypt_opts_kms_provider_kmip_t kmip = {0};
                if (!_mongocrypt_opts_kms_provider_kmip_parse(&kmip, field_name, &field_bson, status)) {
                    _mongocrypt_opts_kms_provider_kmip_cleanup(&kmip);
                    return false;
                }
                mc_kms_creds_with_id_t kcwi = {.kmsid = bson_strdup(field_name),
                                               .creds = {.type = type, .value = {.kmip = kmip}}};
                _mc_array_append_val(&kms_providers->named_mut, kcwi);
                break;
            }
            }
        } else if (0 == strcmp(field_name, "azure") && bson_empty(&field_bson)) {
            kms_providers->need_credentials |= MONGOCRYPT_KMS_PROVIDER_AZURE;
        } else if (0 == strcmp(field_name, "azure")) {
            if (0 != (kms_providers->configured_providers & MONGOCRYPT_KMS_PROVIDER_AZURE)) {
                CLIENT_ERR("azure KMS provider already set");
                return false;
            }

            if (!_mongocrypt_opts_kms_provider_azure_parse(&kms_providers->azure_mut,
                                                           field_name,
                                                           &field_bson,
                                                           status)) {
                return false;
            }
            kms_providers->configured_providers |= MONGOCRYPT_KMS_PROVIDER_AZURE;
        } else if (0 == strcmp(field_name, "gcp") && bson_empty(&field_bson)) {
            kms_providers->need_credentials |= MONGOCRYPT_KMS_PROVIDER_GCP;
        } else if (0 == strcmp(field_name, "gcp")) {
            if (0 != (kms_providers->configured_providers & MONGOCRYPT_KMS_PROVIDER_GCP)) {
                CLIENT_ERR("gcp KMS provider already set");
                return false;
            }
            if (!_mongocrypt_opts_kms_provider_gcp_parse(&kms_providers->gcp_mut, field_name, &field_bson, status)) {
                return false;
            }
            kms_providers->configured_providers |= MONGOCRYPT_KMS_PROVIDER_GCP;
        } else if (0 == strcmp(field_name, "local") && bson_empty(&field_bson)) {
            kms_providers->need_credentials |= MONGOCRYPT_KMS_PROVIDER_LOCAL;
        } else if (0 == strcmp(field_name, "local")) {
            if (0 != (kms_providers->configured_providers & MONGOCRYPT_KMS_PROVIDER_LOCAL)) {
                CLIENT_ERR("local KMS provider already set");
                return false;
            }
            if (!_mongocrypt_opts_kms_provider_local_parse(&kms_providers->local_mut,
                                                           field_name,
                                                           &field_bson,
                                                           status)) {
                return false;
            }
            kms_providers->configured_providers |= MONGOCRYPT_KMS_PROVIDER_LOCAL;
        } else if (0 == strcmp(field_name, "aws") && bson_empty(&field_bson)) {
            kms_providers->need_credentials |= MONGOCRYPT_KMS_PROVIDER_AWS;
        } else if (0 == strcmp(field_name, "aws")) {
            if (0 != (kms_providers->configured_providers & MONGOCRYPT_KMS_PROVIDER_AWS)) {
                CLIENT_ERR("aws KMS provider already set");
                return false;
            }
            if (!_mongocrypt_opts_kms_provider_aws_parse(&kms_providers->aws_mut, field_name, &field_bson, status)) {
                return false;
            }
            kms_providers->configured_providers |= MONGOCRYPT_KMS_PROVIDER_AWS;
        } else if (0 == strcmp(field_name, "kmip") && bson_empty(&field_bson)) {
            kms_providers->need_credentials |= MONGOCRYPT_KMS_PROVIDER_KMIP;
        } else if (0 == strcmp(field_name, "kmip")) {
            if (!_mongocrypt_opts_kms_provider_kmip_parse(&kms_providers->kmip_mut, field_name, &field_bson, status)) {
                return false;
            }
            kms_providers->configured_providers |= MONGOCRYPT_KMS_PROVIDER_KMIP;
        } else {
            CLIENT_ERR("unsupported KMS provider: %s", field_name);
            return false;
        }
    }

    if (log && log->trace_enabled) {
        char *as_str = bson_as_relaxed_extended_json(&as_bson, NULL);
        _mongocrypt_log(log, MONGOCRYPT_LOG_LEVEL_TRACE, "%s (%s=\"%s\")", BSON_FUNC, "kms_providers", as_str);
        bson_free(as_str);
    }

    return true;
}

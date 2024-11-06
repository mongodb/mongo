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

#ifndef MONGOCRYPT_OPTS_PRIVATE_H
#define MONGOCRYPT_OPTS_PRIVATE_H

#include <bson/bson.h>

#include "mlib/str.h"

#include "mongocrypt-buffer-private.h"
#include "mongocrypt-endpoint-private.h"
#include "mongocrypt-kek-private.h"
#include "mongocrypt-log-private.h"
#include "mongocrypt.h"
#include <mc-array-private.h>

typedef struct {
    char *tenant_id;
    char *client_id;
    char *client_secret;
    _mongocrypt_endpoint_t *identity_platform_endpoint;
    char *access_token;
} _mongocrypt_opts_kms_provider_azure_t;

typedef struct {
    char *email;
    _mongocrypt_buffer_t private_key;
    _mongocrypt_endpoint_t *endpoint;
    char *access_token;
} _mongocrypt_opts_kms_provider_gcp_t;

typedef struct {
    char *secret_access_key;
    char *access_key_id;
    char *session_token;
} _mongocrypt_opts_kms_provider_aws_t;

typedef struct {
    _mongocrypt_buffer_t key;
} _mongocrypt_opts_kms_provider_local_t;

typedef struct {
    _mongocrypt_endpoint_t *endpoint;
} _mongocrypt_opts_kms_provider_kmip_t;

typedef struct {
    // `type` identifies the set field in `value`.
    _mongocrypt_kms_provider_t type;

    union {
        _mongocrypt_opts_kms_provider_local_t local;
        _mongocrypt_opts_kms_provider_aws_t aws;
        _mongocrypt_opts_kms_provider_azure_t azure;
        _mongocrypt_opts_kms_provider_gcp_t gcp;
        _mongocrypt_opts_kms_provider_kmip_t kmip;
    } value;
} mc_kms_creds_t;

typedef struct {
    int configured_providers; /* A bit set of _mongocrypt_kms_provider_t */
    int need_credentials;     /* A bit set of _mongocrypt_kms_provider_t */
    // Fields suffixed with `_mut` are mutated when constructing the `_mongocrypt_opts_kms_providers_t`.
    // Prefer using `_mongocrypt_opts_kms_providers_lookup` to read the values.
    _mongocrypt_opts_kms_provider_local_t local_mut;
    _mongocrypt_opts_kms_provider_aws_t aws_mut;
    _mongocrypt_opts_kms_provider_azure_t azure_mut;
    _mongocrypt_opts_kms_provider_gcp_t gcp_mut;
    _mongocrypt_opts_kms_provider_kmip_t kmip_mut;
    // `named_mut` stores a list of named KMS providers.
    mc_array_t named_mut;
} _mongocrypt_opts_kms_providers_t;

void _mongocrypt_opts_kms_providers_init(_mongocrypt_opts_kms_providers_t *kms_providers);

bool _mongocrypt_parse_kms_providers(mongocrypt_binary_t *kms_providers_definition,
                                     _mongocrypt_opts_kms_providers_t *kms_providers,
                                     mongocrypt_status_t *status,
                                     _mongocrypt_log_t *log);

bool _mongocrypt_opts_kms_providers_lookup(const _mongocrypt_opts_kms_providers_t *kms_providers,
                                           const char *kmsid,
                                           mc_kms_creds_t *out);

typedef struct {
    mongocrypt_log_fn_t log_fn;
    void *log_ctx;
    _mongocrypt_buffer_t schema_map;
    _mongocrypt_buffer_t encrypted_field_config_map;

    _mongocrypt_opts_kms_providers_t kms_providers;
    mongocrypt_hmac_fn sign_rsaes_pkcs1_v1_5;
    void *sign_ctx;

    /// Keep an array of search paths for finding the crypt_shared library
    /// during mongocrypt_init()
    int n_crypt_shared_lib_search_paths;
    mstr *crypt_shared_lib_search_paths;
    /// Optionally, a user may override the default search behavior by specifying
    /// a specifiy path to the library. If this is set, this suppresses the
    /// search behavior.
    mstr crypt_shared_lib_override_path;

    bool use_need_kms_credentials_state;
    bool use_need_mongo_collinfo_with_db_state;
    bool bypass_query_analysis;

    // When creating new encrypted payloads,
    // use V2 variants of the FLE2 datatypes.
    bool use_fle2_v2;

    // Use the Queryable Encryption Range V2 protocol.
    bool use_range_v2;
} _mongocrypt_opts_t;

void _mongocrypt_opts_kms_providers_cleanup(_mongocrypt_opts_kms_providers_t *kms_providers);

/* Merge `source` into `dest`. Does not perform any memory ownership management;
 * values in `dest` will be overwritten with values from `source` without
 * being released. */
void _mongocrypt_opts_merge_kms_providers(_mongocrypt_opts_kms_providers_t *dest,
                                          const _mongocrypt_opts_kms_providers_t *source);

void _mongocrypt_opts_init(_mongocrypt_opts_t *opts);

void _mongocrypt_opts_cleanup(_mongocrypt_opts_t *opts);

bool _mongocrypt_opts_validate(_mongocrypt_opts_t *opts, mongocrypt_status_t *status) MONGOCRYPT_WARN_UNUSED_RESULT;

bool _mongocrypt_opts_kms_providers_validate(_mongocrypt_opts_t *opts,
                                             _mongocrypt_opts_kms_providers_t *kms_providers,
                                             mongocrypt_status_t *status) MONGOCRYPT_WARN_UNUSED_RESULT;

/*
 * Parse an optional UTF-8 value from BSON.
 * @dotkey may be a dot separated key like: "a.b.c".
 * @*out is set to a copy of the string if found, NULL otherwise. Caller must
 * clean up with bson_free (*out).
 * Returns true if no error occured.
 */
bool _mongocrypt_parse_optional_utf8(const bson_t *bson, const char *dotkey, char **out, mongocrypt_status_t *status);

/*
 * Parse an optional boolean value from BSON.
 * @dotkey may be a dot separated key like: "a.b.c".
 * @*out is set to a copy of the value if found, false otherwise.
 * Returns true if no error occured.
 */
bool _mongocrypt_parse_optional_bool(const bson_t *bson, const char *dotkey, bool *out, mongocrypt_status_t *status);

/*
 * Parse a required UTF-8 value from BSON.
 * @dotkey may be a dot separated key like: "a.b.c".
 * @*out is set to a copy of the string if found, NULL otherwise. Caller must
 * clean up with bson_free (*out).
 * Returns true if no error occured.
 */
bool _mongocrypt_parse_required_utf8(const bson_t *bson, const char *dotkey, char **out, mongocrypt_status_t *status);

/*
 * Parse an optional endpoint UTF-8 from BSON.
 * @dotkey may be a dot separated key like: "a.b.c".
 * @*out is set to a new _mongocrypt_endpoint_t of the if found, NULL otherwise.
 * @*opts may be set to configure endpoint parsing. It is optional and may be
 * NULL. Caller must clean up with _mongocrypt_endpoint_destroy (*out). Returns
 * true if no error occured.
 */
bool _mongocrypt_parse_optional_endpoint(const bson_t *bson,
                                         const char *dotkey,
                                         _mongocrypt_endpoint_t **out,
                                         _mongocrypt_endpoint_parse_opts_t *opts,
                                         mongocrypt_status_t *status);

/*
 * Parse a required endpoint UTF-8 from BSON.
 * @dotkey may be a dot separated key like: "a.b.c".
 * @*out is set to a new _mongocrypt_endpoint_t of the if found, NULL otherwise.
 * @*opts may be set to configure endpoint parsing. It is optional and may be
 * NULL. Caller must clean up with _mongocrypt_endpoint_destroy (*out). Returns
 * true if no error occured.
 */
bool _mongocrypt_parse_required_endpoint(const bson_t *bson,
                                         const char *dotkey,
                                         _mongocrypt_endpoint_t **out,
                                         _mongocrypt_endpoint_parse_opts_t *opts,
                                         mongocrypt_status_t *status);

/*
 * Parse an optional binary type from BSON.
 * The field parsed is accepted as:
 * - A BSON binary value (of any subtype).
 * - A BSON UTF-8 value, set to base64 encoded data.
 *
 * @dotkey may be a dot separated key like: "a.b.c"
 * @out is initialized with the parsed data, or initialized to empty on error.
 * Caller must clean up with _mongocrypt_buffer_cleanup (out).
 * Returns true if no error occurred.
 */
bool _mongocrypt_parse_optional_binary(const bson_t *bson,
                                       const char *dotkey,
                                       _mongocrypt_buffer_t *out,
                                       mongocrypt_status_t *status);

/*
 * Parse a required binary type from BSON.
 * The field parsed is accepted as:
 * - A BSON binary value (of any subtype).
 * - A BSON UTF-8 value, set to base64 encoded data.
 *
 * @dotkey may be a dot separated key like: "a.b.c"
 * @out is initialized with the parsed data, or initialized to empty on error.
 * Caller must clean up with _mongocrypt_buffer_cleanup (out).
 * Returns true if no error occurred.
 */
bool _mongocrypt_parse_required_binary(const bson_t *bson,
                                       const char *dotkey,
                                       _mongocrypt_buffer_t *out,
                                       mongocrypt_status_t *status);

/*
 * Checks for unrecognized fields in parsing @bson.
 * @dotkey is a dot separated path to a document field, like "a.b.c" or NULL.
 * Pass a list of allowed fields.
 * Returns true if no error occurred.
 */
bool _mongocrypt_check_allowed_fields_va(const bson_t *bson, const char *dotkey, mongocrypt_status_t *status, ...);

#define _mongocrypt_check_allowed_fields(bson, path, status, ...)                                                      \
    _mongocrypt_check_allowed_fields_va(bson, path, status, __VA_ARGS__, NULL)

bool mc_kmsid_parse(const char *kmsid,
                    _mongocrypt_kms_provider_t *type_out,
                    const char **name_out,
                    mongocrypt_status_t *status);
#endif /* MONGOCRYPT_OPTS_PRIVATE_H */

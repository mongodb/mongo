/*
 * Copyright 2020-present MongoDB, Inc.
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

#ifndef MONGOCRYPT_KEK_PRIVATE_H
#define MONGOCRYPT_KEK_PRIVATE_H

#include <bson/bson.h>

#include "mongocrypt-endpoint-private.h"
#include "mongocrypt.h"

/* Defines structs for Key Encryption Keys (KEKs)
 * The KEK is used as part of envelope encryption. It encrypts a Data Encryption
 * Key (DEK). A KEK is specified when creating a new data encryption key, or
 * when parsing a key document from the key vault.
 */

/* KMS providers are used in a bit set.
 *
 * Check for set membership using bitwise and:
 *   int kms_set = fn();
 *   if (kms_set & MONGOCRYPT_KMS_PROVIDER_AWS)
 * Add to a set using bitwise or:
 *   kms_set |= MONGOCRYPT_KMS_PROVIDER_LOCAL
 */
typedef enum {
    MONGOCRYPT_KMS_PROVIDER_NONE = 0,
    MONGOCRYPT_KMS_PROVIDER_AWS = 1 << 0,
    MONGOCRYPT_KMS_PROVIDER_LOCAL = 1 << 1,
    MONGOCRYPT_KMS_PROVIDER_AZURE = 1 << 2,
    MONGOCRYPT_KMS_PROVIDER_GCP = 1 << 3,
    MONGOCRYPT_KMS_PROVIDER_KMIP = 1 << 4
} _mongocrypt_kms_provider_t;

typedef struct {
    _mongocrypt_endpoint_t *key_vault_endpoint;
    char *key_name;
    char *key_version;
} _mongocrypt_azure_kek_t;

typedef struct {
    char *project_id;
    char *location;
    char *key_ring;
    char *key_name;
    char *key_version;                /* optional */
    _mongocrypt_endpoint_t *endpoint; /* optional. */
} _mongocrypt_gcp_kek_t;

typedef struct {
    char *region;
    char *cmk;
    _mongocrypt_endpoint_t *endpoint; /* optional. */
} _mongocrypt_aws_kek_t;

typedef struct {
    char *key_id;                     /* optional on parsing, required on appending. */
    _mongocrypt_endpoint_t *endpoint; /* optional. */
} _mongocrypt_kmip_kek_t;

typedef struct {
    _mongocrypt_kms_provider_t kms_provider;

    union {
        _mongocrypt_azure_kek_t azure;
        _mongocrypt_gcp_kek_t gcp;
        _mongocrypt_aws_kek_t aws;
        _mongocrypt_kmip_kek_t kmip;
    } provider;
} _mongocrypt_kek_t;

/* Parse a document describing a key encryption key.
 * This may can come from two places:
 * 1. The option passed for creating a data key via
 * mongocrypt_ctx_setopt_key_encryption_key
 * 2. The "masterKey" document from a data encryption key document.
 */
bool _mongocrypt_kek_parse_owned(const bson_t *bson,
                                 _mongocrypt_kek_t *out,
                                 mongocrypt_status_t *status) MONGOCRYPT_WARN_UNUSED_RESULT;

bool _mongocrypt_kek_append(const _mongocrypt_kek_t *kek,
                            bson_t *out,
                            mongocrypt_status_t *status) MONGOCRYPT_WARN_UNUSED_RESULT;

void _mongocrypt_kek_copy_to(const _mongocrypt_kek_t *src, _mongocrypt_kek_t *dst);

void _mongocrypt_kek_cleanup(_mongocrypt_kek_t *kek);

#endif /* MONGOCRYPT_KEK_PRIVATE_H */
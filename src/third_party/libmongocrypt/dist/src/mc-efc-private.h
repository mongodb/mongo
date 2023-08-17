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
#ifndef MC_EFC_PRIVATE_H
#define MC_EFC_PRIVATE_H

#include "mongocrypt-buffer-private.h"
#include <bson/bson.h>

typedef struct _mc_EncryptedField_t {
    bool has_queries;
    _mongocrypt_buffer_t keyId;
    const char *path;
    struct _mc_EncryptedField_t *next;
} mc_EncryptedField_t;

/* See
 * https://github.com/mongodb/mongo/blob/591f49a64e96cea68bf3501320de31c51c31f412/src/mongo/crypto/encryption_fields.idl#L48-L112
 * for the server IDL definition of EncryptedFieldConfig. */
typedef struct {
    mc_EncryptedField_t *fields;
} mc_EncryptedFieldConfig_t;

/* mc_EncryptedFieldConfig_parse parses a subset of the fields from @efc_bson
 * into @efc. Fields are copied from @efc_bson. It is OK to free efc_bson after
 * this call. Fields are appended in reverse order to @efc->fields. Extra
 * unrecognized fields are not considered an error for forward compatibility. */
bool mc_EncryptedFieldConfig_parse(mc_EncryptedFieldConfig_t *efc, const bson_t *efc_bson, mongocrypt_status_t *status);

void mc_EncryptedFieldConfig_cleanup(mc_EncryptedFieldConfig_t *efc);

#endif /* MC_EFC_PRIVATE_H */

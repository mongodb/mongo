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

#ifndef MC_SCHEMA_BROKER_PRIVATE_H
#define MC_SCHEMA_BROKER_PRIVATE_H

#include "mc-efc-private.h" // mc_EncryptedFieldConfig_t
#include "mongocrypt-cache-collinfo-private.h"
#include "mongocrypt.h"
#include <bson/bson.h>

// mc_schema_broker_t manages schemas for an auto encryption operation.
//
// A schema is either:
// - a JSON Schema for CSFLE operations, or
// - an encryptedFields document for QE operations.
//
typedef struct mc_schema_broker_t mc_schema_broker_t;

mc_schema_broker_t *mc_schema_broker_new(void);

void mc_schema_broker_destroy(mc_schema_broker_t *sb);

// mc_schema_broker_request requests a schema for a collection. Ignores duplicates.
// Returns error if two requests have different databases (not-yet supported).
bool mc_schema_broker_request(mc_schema_broker_t *sb, const char *db, const char *coll, mongocrypt_status_t *status);

// mc_schema_broker_has_multiple_requests returns true if there are requests for multiple unique collections
bool mc_schema_broker_has_multiple_requests(const mc_schema_broker_t *sb);

// mc_schema_broker_append_listCollections_filter appends a filter to use with the listCollections command.
// Example: { "name": { "$in": [ "coll1", "coll2" ] } }
// The filter matches all not-yet-satisfied collections.
bool mc_schema_broker_append_listCollections_filter(const mc_schema_broker_t *sb,
                                                    bson_t *out,
                                                    mongocrypt_status_t *status);

// mc_schema_broker_satisfy_from_collinfo satisfies a schema request with a result from listCollections.
// The result is cached.
bool mc_schema_broker_satisfy_from_collinfo(mc_schema_broker_t *sb,
                                            const bson_t *collinfo,
                                            _mongocrypt_cache_t *collinfo_cache,
                                            mongocrypt_status_t *status);

// mc_schema_broker_satisfy_from_schemaMap tries to satisfy schema requests with a schemaMap.
bool mc_schema_broker_satisfy_from_schemaMap(mc_schema_broker_t *sb,
                                             const bson_t *schema_map,
                                             mongocrypt_status_t *status);

// mc_schema_broker_satisfy_from_encryptedFieldsMap tries to satisfy schema requests with an encryptedFieldsMap.
bool mc_schema_broker_satisfy_from_encryptedFieldsMap(mc_schema_broker_t *sb,
                                                      const bson_t *ef_map,
                                                      mongocrypt_status_t *status);

// mc_schema_broker_satisfy_from_cache tries to satisfy schema requests with the cache of listCollections results.
bool mc_schema_broker_satisfy_from_cache(mc_schema_broker_t *sb,
                                         _mongocrypt_cache_t *collinfo_cache,
                                         mongocrypt_status_t *status);

// mc_schema_broker_satisfy_from_create_or_collMod tries to satisfy a schema request by inspecting the outgoing
// create/collMod commands.
bool mc_schema_broker_satisfy_from_create_or_collMod(mc_schema_broker_t *sb,
                                                     const bson_t *cmd,
                                                     mongocrypt_status_t *status);

// mc_schema_broker_satisfy_remaining_from_empty_schemas is called when a driver signals all listCollection results
// have been fed. Assumes remaining collections have no schema. If collinfo_cache is passed, the empty result is cached.
bool mc_schema_broker_satisfy_remaining_with_empty_schemas(mc_schema_broker_t *sb,
                                                           _mongocrypt_cache_t *collinfo_cache /* may be NULL */,
                                                           mongocrypt_status_t *status);

// mc_schema_broker_has_any_qe_schemas returns true if any collection has encryptedFields.
//
// Aborts if any unsatisfied schema requests. `mc_schema_broker_need_more_schemas(sb)` must be false.
//
bool mc_schema_broker_has_any_qe_schemas(const mc_schema_broker_t *sb);

// mc_schema_broker_need_more_schemas returns true if there are unsatisfied schema requests.
bool mc_schema_broker_need_more_schemas(const mc_schema_broker_t *sb);

// mc_schema_broker_get_encryptedFields returns encryptedFields for a collection.
//
// Returns NULL and sets error if `coll` is not found or has no encryptedFields.
//
// Aborts if any unsatisfied schema requests. `mc_schema_broker_need_more_schemas(sb)` must be false.
//
const mc_EncryptedFieldConfig_t *
mc_schema_broker_get_encryptedFields(const mc_schema_broker_t *sb, const char *coll, mongocrypt_status_t *status);

typedef enum {
    MC_CMD_SCHEMAS_FOR_CRYPT_SHARED, // target the crypt_shared library.
    MC_CMD_SCHEMAS_FOR_MONGOCRYPTD,  // target mongocryptd process.
    MC_CMD_SCHEMAS_FOR_SERVER        // target the server (mongod/mongos).
} mc_cmd_target_t;

// mc_schema_broker_add_schemas_to_cmd adds schema information to a command.
//
// Aborts if any unsatisfied schema requests. `mc_schema_broker_need_more_schemas(sb)` must be false.
//
// Schemas are added with the fields:
// - jsonSchema: for CSFLE with one schema.
// - csfleEncryptionSchemas: for CSFLE with multiple schemas.
// - encryptionInformation: for QE.
//
// Set cmd_target to the intended command destination. This impacts if/how schema information is added.
bool mc_schema_broker_add_schemas_to_cmd(const mc_schema_broker_t *sb,
                                         bson_t *cmd /* in and out */,
                                         mc_cmd_target_t cmd_target,
                                         mongocrypt_status_t *status);
#endif // MC_SCHEMA_BROKER_PRIVATE_H

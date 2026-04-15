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

#include "mc-schema-broker-private.h"

#include "mc-efc-private.h" // mc_EncryptedFieldConfig_t
#include "mongocrypt-buffer-private.h"
#include "mongocrypt-cache-collinfo-private.h"
#include "mongocrypt-key-broker-private.h"
#include "mongocrypt-private.h"
#include "mongocrypt-status-private.h"
#include "mongocrypt-util-private.h"
#include <bson/bson.h>

typedef struct mc_schema_entry_t {
    char *coll;

    struct {
        bool set;
        _mongocrypt_buffer_t buf; // Owns document.
        bson_t bson;              // Non-owning view into buf.
        bool is_remote;
    } jsonSchema;

    struct {
        bool set;
        _mongocrypt_buffer_t buf; // Owns document.
        bson_t bson;              // Non-owning view into buf.
        mc_EncryptedFieldConfig_t efc;
    } encryptedFields;

    struct mc_schema_entry_t *next;
    bool satisfied; // true once a schema is found and applied, or an empty schema is applied.
} mc_schema_entry_t;

// mc_schema_broker_t stores schemas for an auto encryption operation.
struct mc_schema_broker_t {
    char *db; // Database shared by all schemas.
    mc_schema_entry_t *ll;
    size_t ll_len;
    bool schema_mixing_is_supported;
};

mc_schema_broker_t *mc_schema_broker_new(void) {
    return bson_malloc0(sizeof(mc_schema_broker_t));
}

bool mc_schema_broker_request(mc_schema_broker_t *sb, const char *db, const char *coll, mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(sb);
    BSON_ASSERT_PARAM(db);
    BSON_ASSERT_PARAM(coll);

    if (sb->db && 0 != strcmp(sb->db, db)) {
        CLIENT_ERR("Cannot request schemas for different databases. Requested schemas for '%s' and '%s'.", sb->db, db);
        return false;
    }

    // Check for duplicates. Keep pointer to last node.
    mc_schema_entry_t *last = NULL;
    for (mc_schema_entry_t *it = sb->ll; it != NULL; it = it->next) {
        if (0 == strcmp(it->coll, coll)) {
            return true;
        }
        last = it;
    }

    mc_schema_entry_t *se = bson_malloc0(sizeof *se);
    se->coll = bson_strdup(coll);
    if (NULL == last) {
        sb->ll = se;
        sb->db = bson_strdup(db);
    } else {
        last->next = se;
    }
    sb->ll_len++;
    return true;
}

bool mc_schema_broker_has_multiple_requests(const mc_schema_broker_t *sb) {
    BSON_ASSERT_PARAM(sb);
    return sb->ll_len > 1;
}

void mc_schema_broker_support_mixing_schemas(mc_schema_broker_t *sb) {
    BSON_ASSERT_PARAM(sb);
    sb->schema_mixing_is_supported = true;
}

void mc_schema_broker_destroy(mc_schema_broker_t *sb) {
    if (!sb) {
        return;
    }
    mc_schema_entry_t *it = sb->ll;
    while (it != NULL) {
        bson_free(it->coll);
        // Always clean it->encryptedFields and it->jsonSchema. May be partially set.
        mc_EncryptedFieldConfig_cleanup(&it->encryptedFields.efc);
        _mongocrypt_buffer_cleanup(&it->encryptedFields.buf);
        _mongocrypt_buffer_cleanup(&it->jsonSchema.buf);
        mc_schema_entry_t *tmp = it->next;
        bson_free(it);
        it = tmp;
    }
    bson_free(sb->db);
    bson_free(sb);
    return;
}

bool mc_schema_broker_has_any_qe_schemas(const mc_schema_broker_t *sb) {
    BSON_ASSERT_PARAM(sb);
    for (mc_schema_entry_t *se = sb->ll; se != NULL; se = se->next) {
        BSON_ASSERT(se->satisfied);
        if (se->encryptedFields.set) {
            return true;
        }
    }
    return false;
}

// TRY_BSON_OR is a checks a BSON operation and sets a generic error status on failure.
#define TRY_BSON_OR(stmt) if (!try_or(stmt, "BSON failure", BSON_FUNC, status))

static bool try_or(bool val, const char *msg, const char *func, mongocrypt_status_t *status) {
    if (!val) {
        CLIENT_ERR("[%s] statement failed: %s", func, msg);
    }
    return val;
}

bool mc_schema_broker_append_listCollections_filter(const mc_schema_broker_t *sb,
                                                    bson_t *out,
                                                    mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(sb);
    BSON_ASSERT_PARAM(out);

    size_t num_unsatisfied = 0;
    for (mc_schema_entry_t *se = sb->ll; se != NULL; se = se->next) {
        if (!se->satisfied) {
            num_unsatisfied++;
        }
    }

    if (num_unsatisfied == 0) {
        CLIENT_ERR("Unexpected: attempting to create listCollections filter but no schemas needed");
        return false;
    } else if (num_unsatisfied == 1) {
        // One request. Append as: { "name": <name> }
        for (mc_schema_entry_t *se = sb->ll; se != NULL; se = se->next) {
            if (se->satisfied) {
                continue;
            }

            TRY_BSON_OR(BSON_APPEND_UTF8(out, "name", se->coll)) {
                return false;
            }
            break;
        }
    } else {
        // Multiple requests. Append as: { "name": { "$in": [ <name1>, <name2>, ... ] } }
        bson_t in, in_array;
        TRY_BSON_OR(BSON_APPEND_DOCUMENT_BEGIN(out, "name", &in)) {
            return false;
        }
        TRY_BSON_OR(BSON_APPEND_ARRAY_BEGIN(&in, "$in", &in_array)) {
            return false;
        }

        size_t idx = 0;
        for (mc_schema_entry_t *se = sb->ll; se != NULL; se = se->next) {
            if (se->satisfied) {
                continue;
            }

            char idx_str[32];
            int ret = bson_snprintf(idx_str, sizeof idx_str, "%zu", idx);
            BSON_ASSERT(ret > 0 && ret < (int)sizeof idx_str);

            TRY_BSON_OR(BSON_APPEND_UTF8(&in_array, idx_str, se->coll)) {
                return false;
            }

            idx++;
        }
        TRY_BSON_OR(bson_append_array_end(&in, &in_array)) {
            return false;
        }
        TRY_BSON_OR(bson_append_document_end(out, &in)) {
            return false;
        }
    }

    return true;
}

static inline bool mc_schema_entry_satisfy_from_collinfo(mc_schema_entry_t *se,
                                                         const bson_t *collinfo,
                                                         const char *coll,
                                                         const char *db,
                                                         mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(se);
    BSON_ASSERT_PARAM(collinfo);
    BSON_ASSERT_PARAM(coll);
    BSON_ASSERT_PARAM(db);

    if (se->satisfied) {
        CLIENT_ERR("unexpected duplicate collinfo result for collection: %s.%s", db, coll);
        return false;
    }

    bson_iter_t collinfo_iter;

    if (!bson_iter_init(&collinfo_iter, collinfo)) {
        CLIENT_ERR("failed to iterate collinfo for collection: %s.%s", db, coll);
        return false;
    }

    // Disallow views.
    bson_iter_t type_iter = collinfo_iter;
    if (bson_iter_find(&type_iter, "type") && BSON_ITER_HOLDS_UTF8(&type_iter) && bson_iter_utf8(&type_iter, NULL)
        && 0 == strcmp("view", bson_iter_utf8(&type_iter, NULL))) {
        CLIENT_ERR("cannot auto encrypt a view: %s.%s", db, coll);
        return false;
    }

    // Check if collection is configured for QE.
    bson_iter_t encryptedFields_iter = collinfo_iter;
    if (bson_iter_find_descendant(&encryptedFields_iter, "options.encryptedFields", &encryptedFields_iter)) {
        if (!BSON_ITER_HOLDS_DOCUMENT(&encryptedFields_iter)) {
            CLIENT_ERR("expected document for `options.encryptedFields` but got %s for collection %s.%s",
                       mc_bson_type_to_string(bson_iter_type(&encryptedFields_iter)),
                       db,
                       coll);
            return false;
        }
        if (!_mongocrypt_buffer_copy_from_document_iter(&se->encryptedFields.buf, &encryptedFields_iter)) {
            CLIENT_ERR("failed to copy `options.encryptedFields` for collection: %s.%s", db, coll);
            return false;
        }

        if (!_mongocrypt_buffer_to_bson(&se->encryptedFields.buf, &se->encryptedFields.bson)) {
            CLIENT_ERR("unable to create BSON from `options.encryptedFields` for collection: %s.%s", db, coll);
            return false;
        }

        if (!mc_EncryptedFieldConfig_parse(&se->encryptedFields.efc, &se->encryptedFields.bson, status)) {
            return false;
        }
        se->encryptedFields.set = true;
    }

    // Check if collection is configured for CSFLE.
    bool found_jsonSchema = false;
    bson_iter_t validator_iter = collinfo_iter;
    if (bson_iter_find_descendant(&validator_iter, "options.validator", &validator_iter)
        && BSON_ITER_HOLDS_DOCUMENT(&validator_iter)) {
        if (!bson_iter_recurse(&validator_iter, &validator_iter)) {
            CLIENT_ERR("failed to iterate `options.validator` for collection: %s.%s", db, coll);
            return false;
        }
        while (bson_iter_next(&validator_iter)) {
            const char *key = bson_iter_key(&validator_iter);
            if (0 == strcmp("$jsonSchema", key)) {
                if (found_jsonSchema) {
                    CLIENT_ERR("duplicate `$jsonSchema` fields found for collection: %s.%s", db, coll);
                    return false;
                }

                if (!_mongocrypt_buffer_copy_from_document_iter(&se->jsonSchema.buf, &validator_iter)) {
                    CLIENT_ERR("unable to copy `$jsonSchema` for collection: %s.%s", db, coll);
                    return false;
                }

                if (!_mongocrypt_buffer_to_bson(&se->jsonSchema.buf, &se->jsonSchema.bson)) {
                    CLIENT_ERR("unable to create BSON from `$jsonSchema` for collection: %s.%s", db, coll);
                    return false;
                }

                found_jsonSchema = true;
                BSON_ASSERT(!se->jsonSchema.set);
                se->jsonSchema.set = true;
                se->jsonSchema.is_remote = true;
            }
        }
    }

    se->satisfied = true;
    return true;
}

bool mc_schema_broker_satisfy_from_collinfo(mc_schema_broker_t *sb,
                                            const bson_t *collinfo,
                                            _mongocrypt_cache_t *collinfo_cache,
                                            mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(sb);
    BSON_ASSERT_PARAM(collinfo);
    BSON_ASSERT_PARAM(collinfo_cache);

    bson_iter_t collinfo_iter;

    if (!bson_iter_init(&collinfo_iter, collinfo)) {
        CLIENT_ERR("failed to iterate collinfo in database: %s", sb->db);
        return false;
    }

    // Parse the collection from the `collinfo`.
    const char *coll;
    {
        bson_iter_t name_iter = collinfo_iter;
        if (!bson_iter_find(&name_iter, "name") || !BSON_ITER_HOLDS_UTF8(&name_iter)) {
            CLIENT_ERR("failed to find 'name' in collinfo in database: %s", sb->db);
            return false;
        }
        coll = bson_iter_utf8(&name_iter, NULL);
    }

    // Cache the received collinfo.
    {
        char *ns = bson_strdup_printf("%s.%s", sb->db, coll);
        if (!_mongocrypt_cache_add_copy(collinfo_cache, ns, (void *)collinfo, status)) {
            bson_free(ns);
            return false;
        }
        bson_free(ns);
    }

    // Find matching entry.
    mc_schema_entry_t *se = NULL;
    {
        for (mc_schema_entry_t *it = sb->ll; it != NULL; it = it->next) {
            if (0 == strcmp(it->coll, coll)) {
                se = it;
                break;
            }
        }
        if (!se) {
            CLIENT_ERR("got unexpected collinfo result for collection: %s.%s", sb->db, coll);
            return false;
        }
    }

    if (!mc_schema_entry_satisfy_from_collinfo(se, collinfo, coll, sb->db, status)) {
        return false;
    }

    return true;
}

bool mc_schema_broker_satisfy_from_schemaMap(mc_schema_broker_t *sb,
                                             const bson_t *schema_map,
                                             mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(sb);
    BSON_ASSERT_PARAM(schema_map);

    for (mc_schema_entry_t *it = sb->ll; it != NULL; it = it->next) {
        if (it->satisfied) {
            continue;
        }

        bool loop_ok = false;
        char *ns = bson_strdup_printf("%s.%s", sb->db, it->coll);
        bson_iter_t iter;

        if (bson_iter_init_find(&iter, schema_map, ns)) {
            if (!_mongocrypt_buffer_copy_from_document_iter(&it->jsonSchema.buf, &iter)) {
                CLIENT_ERR("failed to read schema from schema map for collection: %s", ns);
                goto loop_fail;
            }

            if (!_mongocrypt_buffer_to_bson(&it->jsonSchema.buf, &it->jsonSchema.bson)) {
                CLIENT_ERR("unable to create BSON from schema map for collection: %s", ns);
                goto loop_fail;
            }

            BSON_ASSERT(!it->jsonSchema.set);
            it->jsonSchema.set = true;
            it->jsonSchema.is_remote = false;
            it->satisfied = true;
        }

        loop_ok = true;
    loop_fail:
        bson_free(ns);
        if (!loop_ok) {
            return false;
        }
    }
    return true;
}

bool mc_schema_broker_satisfy_from_encryptedFieldsMap(mc_schema_broker_t *sb,
                                                      const bson_t *ef_map,
                                                      mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(sb);
    BSON_ASSERT_PARAM(ef_map);

    for (mc_schema_entry_t *it = sb->ll; it != NULL; it = it->next) {
        if (it->satisfied) {
            continue;
        }

        bool loop_ok = false;
        char *ns = bson_strdup_printf("%s.%s", sb->db, it->coll);
        bson_iter_t iter;

        if (bson_iter_init_find(&iter, ef_map, ns)) {
            if (!_mongocrypt_buffer_copy_from_document_iter(&it->encryptedFields.buf, &iter)) {
                CLIENT_ERR("failed to read encryptedFields from encryptedFields map for collection: %s", ns);
                goto loop_fail;
            }

            if (!_mongocrypt_buffer_to_bson(&it->encryptedFields.buf, &it->encryptedFields.bson)) {
                CLIENT_ERR("failed to create BSON from encryptedFields map for collection: %s", ns);
                goto loop_fail;
            }

            if (!mc_EncryptedFieldConfig_parse(&it->encryptedFields.efc, &it->encryptedFields.bson, status)) {
                goto loop_fail;
            }

            BSON_ASSERT(!it->encryptedFields.set);
            it->encryptedFields.set = true;
            it->satisfied = true;
        }

        loop_ok = true;
    loop_fail:
        bson_free(ns);
        if (!loop_ok) {
            return false;
        }
    }

    return true;
}

bool mc_schema_broker_satisfy_from_cache(mc_schema_broker_t *sb,
                                         _mongocrypt_cache_t *listCollections_cache,
                                         mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(sb);
    BSON_ASSERT_PARAM(listCollections_cache);

    for (mc_schema_entry_t *it = sb->ll; it != NULL; it = it->next) {
        if (it->satisfied) {
            continue;
        }
        char *ns = bson_strdup_printf("%s.%s", sb->db, it->coll);

        // Check if there is a listCollections result cached.
        bool loop_ok = false;
        bson_t *collinfo = NULL;
        if (!_mongocrypt_cache_get(listCollections_cache, ns, (void **)&collinfo)) {
            CLIENT_ERR("failed to retrieve from listCollections cache for entry: %s", ns);
            goto loop_fail;
        }

        if (!collinfo) {
            goto loop_skip;
        }

        if (!mc_schema_entry_satisfy_from_collinfo(it, collinfo, sb->db, it->coll, status)) {
            goto loop_fail;
        }

    loop_skip:
        loop_ok = true;
    loop_fail:
        bson_destroy(collinfo);
        bson_free(ns);
        if (!loop_ok) {
            return false;
        }
    }
    return true;
}

bool mc_schema_broker_satisfy_from_create_or_collMod(mc_schema_broker_t *sb,
                                                     const bson_t *cmd,
                                                     mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(sb);
    BSON_ASSERT_PARAM(cmd);

    bson_iter_t iter;
    if (!bson_iter_init(&iter, cmd) || !bson_iter_next(&iter)) {
        CLIENT_ERR("Failed to get command name");
        return false;
    }

    const char *cmd_name = bson_iter_key(&iter);
    if (0 != strcmp(cmd_name, "create") && 0 != strcmp(cmd_name, "collMod")) {
        // Ignore other commands.
        return true;
    }

    if (!BSON_ITER_HOLDS_UTF8(&iter)) {
        CLIENT_ERR("Failed to get collection name from command");
        return false;
    }
    const char *coll = bson_iter_utf8(&iter, NULL);

    // Check if schema was requested.
    mc_schema_entry_t *found = NULL;
    for (mc_schema_entry_t *it = sb->ll; it != NULL; it = it->next) {
        if (0 == strcmp(it->coll, coll)) {
            found = it;
            break;
        }
    }

    if (!found) {
        // Command is for a collection that is not needed.
        return true;
    }

    if (found->satisfied) {
        return true;
    }

    if (bson_iter_find_descendant(&iter, "validator.$jsonSchema", &iter)) {
        if (!_mongocrypt_buffer_copy_from_document_iter(&found->jsonSchema.buf, &iter)) {
            CLIENT_ERR("failed to read schema from schema map for collection: %s", coll);
            return false;
        }

        if (!_mongocrypt_buffer_to_bson(&found->jsonSchema.buf, &found->jsonSchema.bson)) {
            CLIENT_ERR("unable to create BSON from schema map for collection: %s", coll);
            return false;
        }

        found->jsonSchema.set = true;
        found->jsonSchema.is_remote = true; // Mark remote. Schema may have non-CSFLE validators.
        found->satisfied = true;
        return true;
    }

    if (bson_iter_init_find(&iter, cmd, "encryptedFields")) {
        if (!_mongocrypt_buffer_copy_from_document_iter(&found->encryptedFields.buf, &iter)) {
            CLIENT_ERR("failed to read encryptedFields from command: %s", cmd_name);
            return false;
        }

        if (!_mongocrypt_buffer_to_bson(&found->encryptedFields.buf, &found->encryptedFields.bson)) {
            CLIENT_ERR("unable to create BSON from encryptedFields for command: %s", cmd_name);
            return false;
        }

        if (!mc_EncryptedFieldConfig_parse(&found->encryptedFields.efc, &found->encryptedFields.bson, status)) {
            return false;
        }

        found->encryptedFields.set = true;
        found->satisfied = true;
        return true;
    }

    // Command does not have a schema. Not an error.
    return true;
}

bool mc_schema_broker_satisfy_remaining_with_empty_schemas(mc_schema_broker_t *sb,
                                                           _mongocrypt_cache_t *collinfo_cache /* may be NULL */,
                                                           mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(sb);

    for (mc_schema_entry_t *it = sb->ll; it != NULL; it = it->next) {
        if (it->satisfied) {
            continue;
        }

        // Cache the received collinfo.
        if (collinfo_cache) {
            char *ns = bson_strdup_printf("%s.%s", sb->db, it->coll);
            bson_t empty = BSON_INITIALIZER;
            if (!_mongocrypt_cache_add_copy(collinfo_cache, ns, &empty, status)) {
                bson_destroy(&empty);
                bson_free(ns);
                return false;
            }
            bson_destroy(&empty);
            bson_free(ns);
        }

        it->satisfied = true;
    }

    return true;
}

bool mc_schema_broker_need_more_schemas(const mc_schema_broker_t *sb) {
    BSON_ASSERT_PARAM(sb);
    for (const mc_schema_entry_t *it = sb->ll; it != NULL; it = it->next) {
        if (!it->satisfied) {
            return true;
        }
    }
    return false;
}

const mc_EncryptedFieldConfig_t *
mc_schema_broker_get_encryptedFields(const mc_schema_broker_t *sb, const char *coll, mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(sb);
    BSON_ASSERT_PARAM(coll);

    for (const mc_schema_entry_t *it = sb->ll; it != NULL; it = it->next) {
        BSON_ASSERT(it->satisfied);
        if (0 != strcmp(it->coll, coll)) {
            continue;
        }
        if (!it->encryptedFields.set) {
            CLIENT_ERR("Expected encryptedFields for '%s', but none set", coll);
            return NULL;
        }
        return &it->encryptedFields.efc;
    }
    CLIENT_ERR("Expected encryptedFields for '%s', but did not find entry", coll);
    return NULL;
}

const mc_EncryptedFieldConfig_t *mc_schema_broker_maybe_get_encryptedFields(const mc_schema_broker_t *sb,
                                                                            const char *coll,
                                                                            mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(sb);
    BSON_ASSERT_PARAM(coll);

    for (const mc_schema_entry_t *it = sb->ll; it != NULL; it = it->next) {
        if (0 != strcmp(it->coll, coll)) {
            continue;
        }
        if (!it->encryptedFields.set) {
            return NULL;
        }
        return &it->encryptedFields.efc;
    }
    return NULL;
}

// translate_field_keyAltName_to_keyId translates one field. Returns false on error.
static bool translate_field_keyAltName_to_keyId(const bson_t *old_doc,
                                                bson_t *new_doc,
                                                _mongocrypt_key_broker_t *kb,
                                                int *found_keyAltName,
                                                mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(old_doc);
    BSON_ASSERT_PARAM(new_doc);
    BSON_ASSERT_PARAM(found_keyAltName);
    BSON_ASSERT_PARAM(kb);

    _mongocrypt_buffer_t unused = {0}, key_id_out = {0};
    bson_value_t key_alt_name_v = {0};
    bool ret = false;

    // Copy excluding "keyAltName"
    bson_copy_to_excluding_noinit(old_doc, new_doc, "keyAltName", NULL);

    // Translate keyAltName (if present) to keyId
    bson_iter_t keyAltName_iter;
    if (bson_iter_init_find(&keyAltName_iter, old_doc, "keyAltName") && BSON_ITER_HOLDS_UTF8(&keyAltName_iter)) {
        *found_keyAltName = 1;
        _bson_value_from_string(bson_iter_utf8(&keyAltName_iter, NULL), &key_alt_name_v);
        if (!_mongocrypt_key_broker_decrypted_key_by_name(kb, &key_alt_name_v, &unused, &key_id_out)) {
            _mongocrypt_key_broker_status(kb, status);
            goto fail;
        }
        TRY_BSON_OR(bson_append_binary(new_doc, "keyId", -1, key_id_out.subtype, key_id_out.data, key_id_out.len)) {
            goto fail;
        }
    }

    ret = true;
fail:
    _mongocrypt_buffer_cleanup(&unused);
    _mongocrypt_buffer_cleanup(&key_id_out);
    bson_value_destroy(&key_alt_name_v);
    return ret;
}

int mc_translate_fields_keyAltName_to_keyId(const bson_t *fields_bson,
                                            _mongocrypt_key_broker_t *kb,
                                            bson_t *out,
                                            mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(fields_bson);
    BSON_ASSERT_PARAM(kb);
    BSON_ASSERT_PARAM(out);
    int found_keyAltName = 0;

    bson_iter_t arr_it;
    if (!bson_iter_init(&arr_it, fields_bson)) {
        CLIENT_ERR("failed to iterate 'fields' array");
        return -1;
    }

    while (bson_iter_next(&arr_it)) {
        const char *idx_str = bson_iter_key(&arr_it);

        if (BSON_ITER_HOLDS_DOCUMENT(&arr_it)) {
            bson_t elem_doc;
            if (!mc_iter_document_as_bson(&arr_it, &elem_doc, status)) {
                return -1;
            }
            bson_t new_doc = BSON_INITIALIZER;
            if (!translate_field_keyAltName_to_keyId(&elem_doc, &new_doc, kb, &found_keyAltName, status)) {
                bson_destroy(&new_doc);
                return -1;
            }
            TRY_BSON_OR(bson_append_document(out, idx_str, -1, &new_doc)) {
                bson_destroy(&new_doc);
                return -1;
            }
            bson_destroy(&new_doc);
        } else {
            /* Non-document elements: copy as-is. */
            if (!BSON_APPEND_VALUE(out, idx_str, bson_iter_value(&arr_it))) {
                CLIENT_ERR("failed to append field value");
                return -1;
            }
        }
    }

    return found_keyAltName;
}

static bool append_encryptedFields(const bson_t *encryptedFields,
                                   _mongocrypt_key_broker_t *kb,
                                   const char *coll,
                                   uint8_t default_strEncodeVersion,
                                   bson_t *out,
                                   mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(encryptedFields); // May be an empty BSON document.
    BSON_ASSERT_PARAM(out);
    BSON_ASSERT_PARAM(coll);

    bool ok = false;

    bool has_escCollection = false;
    bool has_ecocCollection = false;
    bool has_fields = false;
    bool has_strEncodeVersion = false;

    char *default_escCollection = NULL;
    char *default_ecocCollection = NULL;

    bson_iter_t iter;
    TRY_BSON_OR(bson_iter_init(&iter, encryptedFields)) {
        goto fail;
    }

    // Copy all values. Check if state collections are present.
    while (bson_iter_next(&iter)) {
        const char *iter_key = bson_iter_key(&iter);
        if (strcmp(iter_key, "escCollection") == 0) {
            has_escCollection = true;
        }
        if (strcmp(bson_iter_key(&iter), "ecocCollection") == 0) {
            has_ecocCollection = true;
        }
        if (strcmp(bson_iter_key(&iter), "fields") == 0) {
            has_fields = true;
        }
        if (strcmp(bson_iter_key(&iter), "strEncodeVersion") == 0) {
            has_strEncodeVersion = true;
        }
        /* Special-case the "fields" array: translate any "keyAltName" to "keyId" */
        if (0 == strcmp(iter_key, "fields") && BSON_ITER_HOLDS_ARRAY(&iter) && kb) {
            uint32_t array_len = 0;
            const uint8_t *array_data = NULL;
            bson_t array_bson;

            bson_iter_array(&iter, &array_len, &array_data);
            TRY_BSON_OR(bson_init_static(&array_bson, array_data, array_len)) {
                goto fail;
            }

            bson_t new_array;
            TRY_BSON_OR(BSON_APPEND_ARRAY_BEGIN(out, "fields", &new_array)) {
                goto fail;
            }

            const int translated_keyAltName =
                mc_translate_fields_keyAltName_to_keyId(&array_bson, kb, &new_array, status);
            if (translated_keyAltName == -1) {
                bson_append_array_end(out, &new_array);
                goto fail;
            }

            TRY_BSON_OR(bson_append_array_end(out, &new_array)) {
                goto fail;
            }
        } else {
            TRY_BSON_OR(BSON_APPEND_VALUE(out, iter_key, bson_iter_value(&iter))) {
                goto fail;
            }
        }
    }

    if (!has_escCollection) {
        default_escCollection = bson_strdup_printf("enxcol_.%s.esc", coll);
        TRY_BSON_OR(BSON_APPEND_UTF8(out, "escCollection", default_escCollection)) {
            goto fail;
        }
    }

    if (!has_ecocCollection) {
        default_ecocCollection = bson_strdup_printf("enxcol_.%s.ecoc", coll);
        TRY_BSON_OR(BSON_APPEND_UTF8(out, "ecocCollection", default_ecocCollection)) {
            goto fail;
        }
    }

    if (!has_fields) {
        bson_t empty = BSON_INITIALIZER;
        TRY_BSON_OR(BSON_APPEND_ARRAY(out, "fields", &empty)) {
            goto fail;
        }
    }

    if (!has_strEncodeVersion && default_strEncodeVersion != 0) {
        // 0 indicates encryptedFields has no text fields and no set strEncodeVersion.
        // Only add strEncodeVersion if needed since older server components do not recognize it.
        TRY_BSON_OR(BSON_APPEND_INT32(out, "strEncodeVersion", (int32_t)default_strEncodeVersion)) {
            goto fail;
        }
    }

    ok = true;
fail:
    bson_free(default_escCollection);
    bson_free(default_ecocCollection);
    return ok;
}

static bool append_encryptionInformation(const mc_schema_broker_t *sb,
                                         _mongocrypt_key_broker_t *kb,
                                         const char *cmd_name,
                                         bson_t *out,
                                         mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(sb);
    BSON_ASSERT_PARAM(cmd_name);
    BSON_ASSERT_PARAM(out);

    bson_t encryption_information_bson;
    TRY_BSON_OR(BSON_APPEND_DOCUMENT_BEGIN(out, "encryptionInformation", &encryption_information_bson)) {
        return false;
    }
    TRY_BSON_OR(BSON_APPEND_INT32(&encryption_information_bson, "type", 1)) {
        return false;
    }

    bson_t schema_bson;
    TRY_BSON_OR(BSON_APPEND_DOCUMENT_BEGIN(&encryption_information_bson, "schema", &schema_bson)) {
        return false;
    }

    for (mc_schema_entry_t *se = sb->ll; se != NULL; se = se->next) {
        // encryptedFields is preferred over jsonSchema
        if (!se->encryptedFields.set && se->jsonSchema.set) {
            continue;
        }

        BSON_ASSERT(se->satisfied);
        bool loop_ok = false;
        char *ns = bson_strdup_printf("%s.%s", sb->db, se->coll);
        bson_t ns_to_schema_bson;
        TRY_BSON_OR(BSON_APPEND_DOCUMENT_BEGIN(&schema_bson, ns, &ns_to_schema_bson)) {
            goto loop_fail;
        }

        bson_t empty_encryptedFields = BSON_INITIALIZER; // Use empty encryptedFields if none set.
        const bson_t *encryptedFields = &empty_encryptedFields;
        uint8_t default_strEncodeVersion = 0;
        if (se->encryptedFields.set) {
            encryptedFields = &se->encryptedFields.bson;
            default_strEncodeVersion = se->encryptedFields.efc.str_encode_version;
        }
        if (!append_encryptedFields(encryptedFields,
                                    kb,
                                    se->coll,
                                    default_strEncodeVersion,
                                    &ns_to_schema_bson,
                                    status)) {
            goto loop_fail;
        }

        TRY_BSON_OR(bson_append_document_end(&schema_bson, &ns_to_schema_bson)) {
            goto loop_fail;
        }
        loop_ok = true;
    loop_fail:
        bson_free(ns);
        bson_destroy(&empty_encryptedFields);
        if (!loop_ok) {
            return false;
        }
    }
    TRY_BSON_OR(bson_append_document_end(&encryption_information_bson, &schema_bson)) {
        return false;
    }
    TRY_BSON_OR(bson_append_document_end(out, &encryption_information_bson)) {
        return false;
    }
    return true;
}

static const char *get_cmd_name(const bson_t *cmd, mongocrypt_status_t *status) {
    bson_iter_t iter;
    const char *cmd_name;

    BSON_ASSERT_PARAM(cmd);

    if (!bson_iter_init(&iter, cmd)) {
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

static bool insert_encryptionInformation(const mc_schema_broker_t *sb,
                                         _mongocrypt_key_broker_t *kb,
                                         const char *cmd_name,
                                         bson_t *cmd /* in and out */,
                                         mc_cmd_target_t cmd_target,
                                         mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(sb);
    BSON_ASSERT_PARAM(cmd_name);
    BSON_ASSERT_PARAM(cmd);

    bson_t out = BSON_INITIALIZER;
    bson_t explain = BSON_INITIALIZER;
    bson_iter_t iter;
    bool ok = false;

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
                CLIENT_ERR("expected one namespace in `bulkWrite`, but found more than one. Only one namespace is "
                           "supported.");
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
            if (!bson_concat(&nsInfo_array_0, &nsInfo)) {
                CLIENT_ERR("unable to concat to 'nsInfo.0' document");
                goto fail;
            }
            // And append `encryptionInformation`.
            if (!append_encryptionInformation(sb, kb, cmd_name, &nsInfo_array_0, status)) {
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

    if (0 == strcmp(cmd_name, "explain")
        && (cmd_target == MC_CMD_SCHEMAS_FOR_CRYPT_SHARED || cmd_target == MC_CMD_SCHEMAS_FOR_SERVER)) {
        // The "explain" command is a special case when sent to crypt_shared or the server, which expect
        // "encryptionInformation" nested in the "explain" document instead of at top-level. Example:
        // {
        //    "explain": {
        //       "find": "to-crypt_shared-or-server"
        //       "encryptionInformation": {}
        //    }
        // }
        if (!bson_iter_init_find(&iter, cmd, "explain")) {
            CLIENT_ERR("expected to find 'explain' field");
            goto fail;
        }

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

        if (!append_encryptionInformation(sb, kb, cmd_name, &explain, status)) {
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
        goto success;
    }

    // Default case: append "encryptionInformation" at top-level. Example:
    // {
    //    "<command name>": { ... }
    //    "encryptionInformation": {}
    // }
    if (!append_encryptionInformation(sb, kb, cmd_name, cmd, status)) {
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

static bool any_entry_includes_encryptedFields(mc_schema_entry_t *head) {
    for (mc_schema_entry_t *se = head; se != NULL; se = se->next) {
        if (se->encryptedFields.set) {
            return true;
        }
    }

    return false;
}

// insert_csfleEncryptionSchemas appends schema information to a command for CSFLE.
// Only consumed by query analysis (mongocryptd/crypt_shared).
// For one JSON schema, use `jsonSchema` for backwards compatibility.
// For multiple JSON schemas, use `csfleEncryptionSchemas` (added in server 8.1).
static bool insert_csfleEncryptionSchemas(const mc_schema_broker_t *sb,
                                          bson_t *cmd /* in/out */,
                                          mc_cmd_target_t cmd_target,
                                          mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(sb);
    BSON_ASSERT_PARAM(cmd);

    if (cmd_target != MC_CMD_SCHEMAS_FOR_CRYPT_SHARED && cmd_target != MC_CMD_SCHEMAS_FOR_MONGOCRYPTD) {
        // CSFLE schemas are only used for query analysis.
        return true;
    }

    if (sb->ll_len == 1) {
        // Append the only jsonSchema with the "jsonSchema" field.
        const mc_schema_entry_t *se = sb->ll;
        BSON_ASSERT(se);
        BSON_ASSERT(!se->next);
        BSON_ASSERT(se->satisfied);
        if (se->jsonSchema.set) {
            TRY_BSON_OR(BSON_APPEND_DOCUMENT(cmd, "jsonSchema", &se->jsonSchema.bson)) {
                return false;
            }
            TRY_BSON_OR(BSON_APPEND_BOOL(cmd, "isRemoteSchema", se->jsonSchema.is_remote)) {
                return false;
            }
        } else {
            bson_t empty = BSON_INITIALIZER;
            TRY_BSON_OR(BSON_APPEND_DOCUMENT(cmd, "jsonSchema", &empty)) {
                return false;
            }
            // Append isRemoteSchema:true to preserve existing value. But I expect it can/should be false.
            TRY_BSON_OR(BSON_APPEND_BOOL(cmd, "isRemoteSchema", true)) {
                return false;
            }
        }
        return true;
    }

    // Append multiple schemas as "csfleEncryptionSchemas"
    bson_t csfleEncryptionSchemas;
    TRY_BSON_OR(BSON_APPEND_DOCUMENT_BEGIN(cmd, "csfleEncryptionSchemas", &csfleEncryptionSchemas)) {
        return false;
    }

    const bool skip_empty_schemas = any_entry_includes_encryptedFields(sb->ll);

    for (mc_schema_entry_t *se = sb->ll; se != NULL; se = se->next) {
        if (se->encryptedFields.set || (!se->jsonSchema.set && skip_empty_schemas)) {
            continue;
        }

        BSON_ASSERT(se->satisfied);

        char *ns = bson_strdup_printf("%s.%s", sb->db, se->coll);
        bson_t ns_to_doc;
        TRY_BSON_OR(BSON_APPEND_DOCUMENT_BEGIN(&csfleEncryptionSchemas, ns, &ns_to_doc)) {
            bson_free(ns);
            return false;
        }
        bson_free(ns);

        if (!se->jsonSchema.set) {
            // Append as an empty document.
            bson_t empty = BSON_INITIALIZER;
            TRY_BSON_OR(BSON_APPEND_DOCUMENT(&ns_to_doc, "jsonSchema", &empty)) {
                return false;
            }
            TRY_BSON_OR(BSON_APPEND_BOOL(&ns_to_doc, "isRemoteSchema", false)) {
                return false;
            }
        } else {
            TRY_BSON_OR(BSON_APPEND_DOCUMENT(&ns_to_doc, "jsonSchema", &se->jsonSchema.bson)) {
                return false;
            }
            TRY_BSON_OR(BSON_APPEND_BOOL(&ns_to_doc, "isRemoteSchema", se->jsonSchema.is_remote)) {
                return false;
            }
        }
        TRY_BSON_OR(bson_append_document_end(&csfleEncryptionSchemas, &ns_to_doc)) {
            return false;
        }
    }

    TRY_BSON_OR(bson_append_document_end(cmd, &csfleEncryptionSchemas)) {
        return false;
    }

    return true;
}

bool mc_schema_broker_add_schemas_to_cmd(mc_schema_broker_t *sb,
                                         _mongocrypt_key_broker_t *kb,
                                         bson_t *cmd /* in and out */,
                                         mc_cmd_target_t cmd_target,
                                         mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(sb);
    BSON_ASSERT_PARAM(cmd);

    const char *cmd_name = get_cmd_name(cmd, status);
    if (!cmd_name) {
        return false;
    }

    bool has_encryptedFields = false;
    bool has_jsonSchema = false;
    const char *coll_with_encryptedFields = NULL;
    const char *coll_with_jsonSchema = NULL;
    for (mc_schema_entry_t *it = sb->ll; it != NULL; it = it->next) {
        BSON_ASSERT(it->satisfied);
        if (it->encryptedFields.set) {
            has_encryptedFields = true;
            coll_with_encryptedFields = it->coll;
            for (mc_EncryptedField_t *f = it->encryptedFields.efc.fields; f != NULL; f = f->next) {
                if (f->keyAltName && _mongocrypt_buffer_empty(&f->keyId)) {
                    bson_value_t key_alt_name;
                    _mongocrypt_buffer_t unused;
                    _bson_value_from_string(f->keyAltName, &key_alt_name);
                    const bool r = _mongocrypt_key_broker_decrypted_key_by_name(kb, &key_alt_name, &unused, &f->keyId);
                    bson_value_destroy(&key_alt_name);
                    _mongocrypt_buffer_cleanup(&unused);
                    if (!r) {
                        CLIENT_ERR("Could not find key by keyAltName: %s", f->keyAltName);
                        return false;
                    }
                }
            }
        } else if (it->jsonSchema.set) {
            has_jsonSchema = true;
            coll_with_jsonSchema = it->coll;
        }
    }

    if (has_encryptedFields && has_jsonSchema) {
        if (sb->schema_mixing_is_supported) {
            return insert_encryptionInformation(sb, kb, cmd_name, cmd, cmd_target, status)
                && insert_csfleEncryptionSchemas(sb, cmd, cmd_target, status);
        }

        CLIENT_ERR("Collection '%s' has an encryptedFields configured, but collection '%s' has a JSON schema "
                   "configured. This is not supported on mongocryptd/crypt_shared versions below 8.2. To ignore the "
                   "JSON schema, add an empty entry for '%s' to AutoEncryptionOpts.encryptedFieldsMap: \"%s\": { "
                   "\"fields\": [] }",
                   coll_with_encryptedFields,
                   coll_with_jsonSchema,
                   coll_with_jsonSchema,
                   coll_with_jsonSchema);
        return false;
    }

    if (has_encryptedFields) {
        // Use encryptionInformation.
        return insert_encryptionInformation(sb, kb, cmd_name, cmd, cmd_target, status);
    }

    if (has_jsonSchema) {
        // Use csfleEncryptionSchemas / jsonSchema only.
        return insert_csfleEncryptionSchemas(sb, cmd, cmd_target, status);
    }

    // Collections have no QE or CSFLE schemas.
    if (0 == strcmp(cmd_name, "bulkWrite")) {
        // "bulkWrite" does not support the jsonSchema field. Use encryptionInformation with empty schemas.
        return insert_encryptionInformation(sb, kb, cmd_name, cmd, cmd_target, status);
    }

    // Use csfleEncryptionSchemas / jsonSchema with empty schemas.
    return insert_csfleEncryptionSchemas(sb, cmd, cmd_target, status);
}

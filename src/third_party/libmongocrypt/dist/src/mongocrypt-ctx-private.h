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

#ifndef MONGOCRYPT_CTX_PRIVATE_H
#define MONGOCRYPT_CTX_PRIVATE_H

#include "mc-efc-private.h"
#include "mc-optional-private.h"
#include "mc-rangeopts-private.h"
#include "mc-schema-broker-private.h"
#include "mc-textopts-private.h"
#include "mongocrypt-buffer-private.h"
#include "mongocrypt-endpoint-private.h"
#include "mongocrypt-key-broker-private.h"
#include "mongocrypt-key-private.h"
#include "mongocrypt-private.h"
#include "mongocrypt.h"

typedef enum {
    _MONGOCRYPT_TYPE_NONE,
    _MONGOCRYPT_TYPE_ENCRYPT,
    _MONGOCRYPT_TYPE_DECRYPT,
    _MONGOCRYPT_TYPE_CREATE_DATA_KEY,
    _MONGOCRYPT_TYPE_REWRAP_MANY_DATAKEY,
    _MONGOCRYPT_TYPE_COMPACT,
} _mongocrypt_ctx_type_t;

const char *_mongocrypt_index_type_to_string(mongocrypt_index_type_t val);

typedef enum {
    MONGOCRYPT_QUERY_TYPE_EQUALITY = 1,
    MONGOCRYPT_QUERY_TYPE_RANGE = 2,
    MONGOCRYPT_QUERY_TYPE_RANGEPREVIEW_DEPRECATED = 3,
    MONGOCRYPT_QUERY_TYPE_PREFIXPREVIEW = 4,
    MONGOCRYPT_QUERY_TYPE_SUFFIXPREVIEW = 5,
    MONGOCRYPT_QUERY_TYPE_SUBSTRINGPREVIEW = 6,
} mongocrypt_query_type_t;

const char *_mongocrypt_query_type_to_string(mongocrypt_query_type_t val);

/* Option values are validated when set.
 * Different contexts accept/require different options,
 * validated when a context is initialized.
 */
typedef struct __mongocrypt_ctx_opts_t {
    _mongocrypt_buffer_t key_id;
    _mongocrypt_key_alt_name_t *key_alt_names;
    _mongocrypt_buffer_t key_material;
    mongocrypt_encryption_algorithm_t algorithm;
    _mongocrypt_kek_t kek;
    bool retry_enabled;

    struct {
        mongocrypt_index_type_t value;
        bool set;
    } index_type;

    _mongocrypt_buffer_t index_key_id;

    struct {
        int64_t value;
        bool set;
    } contention_factor;

    struct {
        mongocrypt_query_type_t value;
        bool set;
    } query_type;

    struct {
        mc_RangeOpts_t value;
        bool set;
    } rangeopts;

    struct {
        mc_TextOpts_t value;
        bool set;
    } textopts;
} _mongocrypt_ctx_opts_t;

// `_mongocrypt_ctx_opts_t` inherits extended alignment from libbson. To dynamically allocate, use
// aligned allocation (e.g. BSON_ALIGNED_ALLOC)
BSON_STATIC_ASSERT2(alignof__mongocrypt_ctx_opts_t,
                    BSON_ALIGNOF(_mongocrypt_ctx_opts_t)
                        >= BSON_MAX(BSON_ALIGNOF(_mongocrypt_key_alt_name_t), BSON_ALIGNOF(mc_RangeOpts_t)));

/* All derived contexts may override these methods. */
typedef struct {
    const char *(*mongo_db_collinfo)(mongocrypt_ctx_t *ctx);
    bool (*mongo_op_collinfo)(mongocrypt_ctx_t *ctx, mongocrypt_binary_t *out);
    bool (*mongo_feed_collinfo)(mongocrypt_ctx_t *ctx, mongocrypt_binary_t *in);
    bool (*mongo_done_collinfo)(mongocrypt_ctx_t *ctx);
    bool (*mongo_op_markings)(mongocrypt_ctx_t *ctx, mongocrypt_binary_t *out);
    bool (*mongo_feed_markings)(mongocrypt_ctx_t *ctx, mongocrypt_binary_t *in);
    bool (*mongo_done_markings)(mongocrypt_ctx_t *ctx);
    bool (*mongo_op_keys)(mongocrypt_ctx_t *ctx, mongocrypt_binary_t *out);
    bool (*mongo_feed_keys)(mongocrypt_ctx_t *ctx, mongocrypt_binary_t *in);
    bool (*mongo_done_keys)(mongocrypt_ctx_t *ctx);
    bool (*after_kms_credentials_provided)(mongocrypt_ctx_t *ctx);
    mongocrypt_kms_ctx_t *(*next_kms_ctx)(mongocrypt_ctx_t *ctx);
    bool (*kms_done)(mongocrypt_ctx_t *ctx);
    bool (*finalize)(mongocrypt_ctx_t *ctx, mongocrypt_binary_t *out);
    void (*cleanup)(mongocrypt_ctx_t *ctx);
} _mongocrypt_vtable_t;

struct _mongocrypt_ctx_t {
    mongocrypt_t *crypt;
    mongocrypt_ctx_state_t state;
    _mongocrypt_ctx_type_t type;
    mongocrypt_status_t *status;
    _mongocrypt_key_broker_t kb;
    _mongocrypt_vtable_t vtable;
    _mongocrypt_ctx_opts_t opts;
    _mongocrypt_opts_kms_providers_t per_ctx_kms_providers; /* owned */
    _mongocrypt_opts_kms_providers_t kms_providers;         /* not owned, is merged from per-ctx / per-mongocrypt_t */
    bool initialized;
    /* nothing_to_do is set to true under these conditions:
     * 1. No keys are requested
     * 2. The command is bypassed for automatic encryption (e.g. ping).
     * 3. bypass_query_analysis is true.
     * TODO (MONGOCRYPT-422) replace nothing_to_do.
     */
    bool nothing_to_do;
};

/* Transition to the error state. An error status must have been set. */
bool _mongocrypt_ctx_fail(mongocrypt_ctx_t *ctx);

/* Set an error status and transition to the error state. */
bool _mongocrypt_ctx_fail_w_msg(mongocrypt_ctx_t *ctx, const char *msg);

typedef struct {
    mongocrypt_ctx_t parent;
    bool explicit;

    // `cmd_db` is the command database (appended as `$db`).
    char *cmd_db;

    // `target_ns` is the target namespace "<target_db>.<target_coll>" for the operation. May be associated with
    // jsonSchema (CSFLE) or encryptedFields (QE). For `bulkWrite`, the target namespace database may differ from
    // `cmd_db`.
    char *target_ns;

    // `target_db` is the target database for the operation. For `bulkWrite`, the target namespace database may differ
    // from `cmd_db`. If `target_db` is NULL, the target namespace database is the same as `cmd_db`.
    char *target_db;

    // `target_coll` is the target namespace collection name.
    char *target_coll;

    _mongocrypt_buffer_t list_collections_filter;

    // `sb` manages encryption schemas (JSONSchema for CSFLE and encryptedFields for QE).
    mc_schema_broker_t *sb;

    /* TODO CDRIVER-3150: audit + rename these buffers.
     * original_cmd for explicit is {v: <BSON value>}, for auto is the command to
     * be encrypted.
     *
     * mongocryptd_cmd is only applicable for auto encryption. It is the original
     * command with JSONSchema appended.
     *
     * marked_cmd is the value of the 'result' field in mongocryptd response
     *
     * encrypted_cmd is the final output, the original command encrypted, or for
     * explicit, the {v: <ciphertext>} doc.
     */
    _mongocrypt_buffer_t original_cmd;
    _mongocrypt_buffer_t mongocryptd_cmd;
    _mongocrypt_buffer_t marked_cmd;
    _mongocrypt_buffer_t encrypted_cmd;
    _mongocrypt_buffer_t key_id;
    /* bypass_query_analysis is set to true to skip the
     * MONGOCRYPT_CTX_NEED_MONGO_MARKINGS state. */
    bool bypass_query_analysis;

    struct {
        bool needed;
        _mongocrypt_buffer_t cmd;
        int32_t maxwireversion;
    } ismaster;

    // cmd_name is the first BSON field in original_cmd for auto encryption.
    const char *cmd_name;
} _mongocrypt_ctx_encrypt_t;

// `_mongocrypt_ctx_encrypt_t` inherits extended alignment from libbson. To dynamically allocate, use
// aligned allocation (e.g. BSON_ALIGNED_ALLOC)
BSON_STATIC_ASSERT2(alignof__mongocrypt_ctx_encrypt_t,
                    BSON_ALIGNOF(_mongocrypt_ctx_encrypt_t) >= BSON_ALIGNOF(mongocrypt_ctx_t));

typedef struct {
    mongocrypt_ctx_t parent;
    /* TODO CDRIVER-3150: audit + rename these buffers.
     * Unlike ctx_encrypt, unwrapped_doc holds the binary value of the {v:
     * <ciphertext>} doc.
     * */
    _mongocrypt_buffer_t original_doc;
    _mongocrypt_buffer_t decrypted_doc;
} _mongocrypt_ctx_decrypt_t;

// `_mongocrypt_ctx_datakey_t` inherits extended alignment from libbson. To dynamically allocate, use
// aligned allocation (e.g. BSON_ALIGNED_ALLOC)
BSON_STATIC_ASSERT2(alignof__mongocrypt_ctx_decrypt_t,
                    BSON_ALIGNOF(_mongocrypt_ctx_decrypt_t) >= BSON_ALIGNOF(mongocrypt_ctx_t));

typedef struct {
    mongocrypt_ctx_t parent;
    mongocrypt_kms_ctx_t kms;
    bool kms_returned;
    _mongocrypt_buffer_t key_doc;
    _mongocrypt_buffer_t plaintext_key_material;
    _mongocrypt_buffer_t encrypted_key_material;

    const char *kmip_unique_identifier;
    bool kmip_activated;
    _mongocrypt_buffer_t kmip_secretdata;
} _mongocrypt_ctx_datakey_t;

// `_mongocrypt_ctx_datakey_t` inherits extended alignment from libbson. To dynamically allocate, use
// aligned allocation (e.g. BSON_ALIGNED_ALLOC)
BSON_STATIC_ASSERT2(alignof__mongocrypt_ctx_datakey_t,
                    BSON_ALIGNOF(_mongocrypt_ctx_datakey_t) >= BSON_ALIGNOF(mongocrypt_ctx_t));

typedef struct _mongocrypt_ctx_rmd_datakey_t _mongocrypt_ctx_rmd_datakey_t;

struct _mongocrypt_ctx_rmd_datakey_t {
    _mongocrypt_ctx_rmd_datakey_t *next;
    mongocrypt_ctx_t *dkctx;
    _mongocrypt_key_doc_t *doc;
};

typedef struct {
    mongocrypt_ctx_t parent;
    _mongocrypt_buffer_t filter;
    mongocrypt_kms_ctx_t kms;
    _mongocrypt_ctx_rmd_datakey_t *datakeys;
    _mongocrypt_ctx_rmd_datakey_t *datakeys_iter;
    _mongocrypt_buffer_t results;
} _mongocrypt_ctx_rewrap_many_datakey_t;

// `_mongocrypt_ctx_rewrap_many_datakey_t` inherits extended alignment from libbson. To dynamically allocate, use
// aligned allocation (e.g. BSON_ALIGNED_ALLOC)
BSON_STATIC_ASSERT2(alignof__mongocrypt_ctx_rewrap_many_datakey_t,
                    BSON_ALIGNOF(_mongocrypt_ctx_rewrap_many_datakey_t) >= BSON_ALIGNOF(mongocrypt_ctx_t));

typedef struct {
    mongocrypt_ctx_t parent;
    _mongocrypt_buffer_t result;
    mc_EncryptedFieldConfig_t efc;
} _mongocrypt_ctx_compact_t;

// `_mongocrypt_ctx_compact_t` inherits extended alignment from libbson. To dynamically allocate, use aligned
// allocation (e.g. BSON_ALIGNED_ALLOC)
BSON_STATIC_ASSERT2(alignof__mongocrypt_ctx_compact_t,
                    BSON_ALIGNOF(_mongocrypt_ctx_compact_t) >= BSON_ALIGNOF(mongocrypt_ctx_t));

#define MONGOCRYPT_CTX_ALLOC_SIZE                                                                                      \
    BSON_MAX(sizeof(_mongocrypt_ctx_encrypt_t),                                                                        \
             BSON_MAX(sizeof(_mongocrypt_ctx_decrypt_t),                                                               \
                      BSON_MAX(sizeof(_mongocrypt_ctx_datakey_t),                                                      \
                               BSON_MAX(sizeof(_mongocrypt_ctx_rewrap_many_datakey_t),                                 \
                                        sizeof(_mongocrypt_ctx_compact_t)))))

#define MONGOCRYPT_CTX_ALLOC_ALIGNMENT                                                                                 \
    BSON_MAX(BSON_ALIGNOF(_mongocrypt_ctx_encrypt_t),                                                                  \
             BSON_MAX(BSON_ALIGNOF(_mongocrypt_ctx_decrypt_t),                                                         \
                      BSON_MAX(BSON_ALIGNOF(_mongocrypt_ctx_datakey_t),                                                \
                               BSON_MAX(BSON_ALIGNOF(_mongocrypt_ctx_rewrap_many_datakey_t),                           \
                                        BSON_ALIGNOF(_mongocrypt_ctx_compact_t)))))

// `_mongocrypt_ctx_t` inherits extended alignment from libbson. To dynamically allocate, use
// aligned allocation (e.g. BSON_ALIGNED_ALLOC)
BSON_STATIC_ASSERT2(alignof_mongocrypt_ctx_t, BSON_ALIGNOF(mongocrypt_ctx_t) >= MONGOCRYPT_CTX_ALLOC_ALIGNMENT);

/* Used for option validation. True means required. False means prohibited. */
typedef enum { OPT_PROHIBITED = 0, OPT_REQUIRED, OPT_OPTIONAL } _mongocrypt_ctx_opt_spec_t;

typedef struct {
    _mongocrypt_ctx_opt_spec_t kek;
    _mongocrypt_ctx_opt_spec_t schema;
    _mongocrypt_ctx_opt_spec_t key_descriptor; /* a key_id or key_alt_name */
    _mongocrypt_ctx_opt_spec_t key_alt_names;
    _mongocrypt_ctx_opt_spec_t key_material;
    _mongocrypt_ctx_opt_spec_t algorithm;
    _mongocrypt_ctx_opt_spec_t rangeopts;
} _mongocrypt_ctx_opts_spec_t;

/* Common initialization. */
bool _mongocrypt_ctx_init(mongocrypt_ctx_t *ctx, _mongocrypt_ctx_opts_spec_t *opt_spec) MONGOCRYPT_WARN_UNUSED_RESULT;

/* Set the state of the context from the state of keys in the key broker. */
bool _mongocrypt_ctx_state_from_key_broker(mongocrypt_ctx_t *ctx) MONGOCRYPT_WARN_UNUSED_RESULT;

/* Get the KMS providers for the current context, fall back to the ones
 * from mongocrypt_t if none are provided for the context specifically. */
_mongocrypt_opts_kms_providers_t *_mongocrypt_ctx_kms_providers(mongocrypt_ctx_t *ctx);

#endif /* MONGOCRYPT_CTX_PRIVATE_H */

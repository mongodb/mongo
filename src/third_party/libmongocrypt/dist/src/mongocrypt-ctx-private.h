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

typedef enum {
    MONGOCRYPT_INDEX_TYPE_NONE = 1,
    MONGOCRYPT_INDEX_TYPE_EQUALITY = 2,
    MONGOCRYPT_INDEX_TYPE_RANGEPREVIEW = 3
} mongocrypt_index_type_t;

const char *_mongocrypt_index_type_to_string(mongocrypt_index_type_t val);

typedef enum { MONGOCRYPT_QUERY_TYPE_EQUALITY = 1, MONGOCRYPT_QUERY_TYPE_RANGEPREVIEW = 2 } mongocrypt_query_type_t;

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
} _mongocrypt_ctx_opts_t;

/* All derived contexts may override these methods. */
typedef struct {
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
    char *coll_name;
    char *db_name;
    char *ns;
    _mongocrypt_buffer_t list_collections_filter;
    _mongocrypt_buffer_t schema;
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
    bool used_local_schema;
    /* collinfo_has_siblings is true if the schema came from a remote JSON
     * schema, and there were siblings. */
    bool collinfo_has_siblings;
    /* encrypted_field_config is set when:
     * 1. <db_name>.<coll_name> is present in an encrypted_field_config_map.
     * 2. (TODO MONGOCRYPT-414) The collection has encryptedFields in the
     * response to listCollections. encrypted_field_config is true if and only if
     * encryption is using FLE 2.0.
     */
    _mongocrypt_buffer_t encrypted_field_config;
    mc_EncryptedFieldConfig_t efc;
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

typedef struct {
    mongocrypt_ctx_t parent;
    /* TODO CDRIVER-3150: audit + rename these buffers.
     * Unlike ctx_encrypt, unwrapped_doc holds the binary value of the {v:
     * <ciphertext>} doc.
     * */
    _mongocrypt_buffer_t original_doc;
    _mongocrypt_buffer_t decrypted_doc;
} _mongocrypt_ctx_decrypt_t;

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

typedef struct {
    mongocrypt_ctx_t parent;
    _mongocrypt_buffer_t result;
    mc_EncryptedFieldConfig_t efc;
} _mongocrypt_ctx_compact_t;

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

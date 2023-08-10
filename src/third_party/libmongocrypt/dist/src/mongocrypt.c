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

#include "mlib/error.h"
#include "mlib/path.h"
#include "mlib/thread.h"

#include <bson/bson.h>
#include <kms_message/kms_message.h>

#include "mongocrypt-binary-private.h"
#include "mongocrypt-cache-collinfo-private.h"
#include "mongocrypt-cache-key-private.h"
#include "mongocrypt-config.h"
#include "mongocrypt-crypto-private.h"
#include "mongocrypt-log-private.h"
#include "mongocrypt-mutex-private.h"
#include "mongocrypt-opts-private.h"
#include "mongocrypt-private.h"
#include "mongocrypt-status-private.h"
#include "mongocrypt-util-private.h"

/* Assert size for interop with wrapper purposes */
BSON_STATIC_ASSERT(sizeof(mongocrypt_log_level_t) == 4);

const char *mongocrypt_version(uint32_t *len) {
    if (len) {
        *len = (uint32_t)strlen(MONGOCRYPT_VERSION);
    }
    return MONGOCRYPT_VERSION;
}

void _mongocrypt_set_error(mongocrypt_status_t *status,
                           mongocrypt_status_type_t type,
                           uint32_t code,
                           const char *format,
                           ...) {
    va_list args;
    char *prepared_message;

    if (status) {
        va_start(args, format);
        prepared_message = bson_strdupv_printf(format, args);
        if (!prepared_message) {
            mongocrypt_status_set(status, type, code, "Out of memory", -1);
        } else {
            mongocrypt_status_set(status, type, code, prepared_message, -1);
            bson_free(prepared_message);
        }
        va_end(args);
    }
}

const char *tmp_json(const bson_t *bson) {
    static char storage[1024];
    char *json;

    BSON_ASSERT_PARAM(bson);

    memset(storage, 0, 1024);
    json = bson_as_canonical_extended_json(bson, NULL);
    bson_snprintf(storage, sizeof(storage), "%s", json);
    bson_free(json);
    return (const char *)storage;
}

const char *tmp_buf(const _mongocrypt_buffer_t *buf) {
    static char storage[1024];
    size_t i, n;

    BSON_ASSERT_PARAM(buf);

    memset(storage, 0, 1024);
    /* capped at two characters per byte, minus 1 for trailing \0 */
    n = sizeof(storage) / 2 - 1;
    if (buf->len < n) {
        n = buf->len;
    }

    for (i = 0; i < n; i++) {
        bson_snprintf(storage + (i * 2), 3, "%02x", buf->data[i]);
    }

    return (const char *)storage;
}

static void _mongocrypt_do_init(void) {
    (void)kms_message_init();
    _native_crypto_init();
}

mongocrypt_t *mongocrypt_new(void) {
    mongocrypt_t *crypt;

    crypt = bson_malloc0(sizeof(mongocrypt_t));
    BSON_ASSERT(crypt);
    crypt->crypto = bson_malloc0(sizeof(*crypt->crypto));
    BSON_ASSERT(crypt->crypto);

    _mongocrypt_mutex_init(&crypt->mutex);
    _mongocrypt_cache_collinfo_init(&crypt->cache_collinfo);
    _mongocrypt_cache_key_init(&crypt->cache_key);
    crypt->status = mongocrypt_status_new();
    _mongocrypt_opts_init(&crypt->opts);
    _mongocrypt_log_init(&crypt->log);
    // Default to using FLEv2 (aka QEv2)
    crypt->opts.use_fle2_v2 = true;
    crypt->ctx_counter = 1;
    crypt->cache_oauth_azure = _mongocrypt_cache_oauth_new();
    crypt->cache_oauth_gcp = _mongocrypt_cache_oauth_new();
    crypt->csfle = (_mongo_crypt_v1_vtable){.okay = false};

    static mlib_once_flag init_flag = MLIB_ONCE_INITIALIZER;

    if (!mlib_call_once(&init_flag, _mongocrypt_do_init) || !_native_crypto_initialized) {
        mongocrypt_status_t *status = crypt->status;

        CLIENT_ERR("failed to initialize");
        /* Return crypt with failure status so caller can obtain error when
         * calling mongocrypt_init */
    }

    return crypt;
}

#define ASSERT_MONGOCRYPT_PARAM_UNINIT(crypt)                                                                          \
    {                                                                                                                  \
        const mongocrypt_t *_crypt = (crypt);                                                                          \
        BSON_ASSERT_PARAM(_crypt);                                                                                     \
        if (_crypt->initialized) {                                                                                     \
            mongocrypt_status_t *status = _crypt->status;                                                              \
            CLIENT_ERR("options cannot be set after initialization");                                                  \
            return false;                                                                                              \
        }                                                                                                              \
    }

bool mongocrypt_setopt_fle2v2(mongocrypt_t *crypt, bool enable) {
    ASSERT_MONGOCRYPT_PARAM_UNINIT(crypt);

    crypt->opts.use_fle2_v2 = enable;
    return true;
}

bool mongocrypt_setopt_log_handler(mongocrypt_t *crypt, mongocrypt_log_fn_t log_fn, void *log_ctx) {
    ASSERT_MONGOCRYPT_PARAM_UNINIT(crypt);
    crypt->opts.log_fn = log_fn;
    crypt->opts.log_ctx = log_ctx;
    return true;
}

bool mongocrypt_setopt_kms_provider_aws(mongocrypt_t *crypt,
                                        const char *aws_access_key_id,
                                        int32_t aws_access_key_id_len,
                                        const char *aws_secret_access_key,
                                        int32_t aws_secret_access_key_len) {
    ASSERT_MONGOCRYPT_PARAM_UNINIT(crypt);

    mongocrypt_status_t *status = crypt->status;
    _mongocrypt_opts_kms_providers_t *const kms_providers = &crypt->opts.kms_providers;

    if (0 != (kms_providers->configured_providers & MONGOCRYPT_KMS_PROVIDER_AWS)) {
        CLIENT_ERR("aws kms provider already set");
        return false;
    }

    if (!_mongocrypt_validate_and_copy_string(aws_access_key_id,
                                              aws_access_key_id_len,
                                              &kms_providers->aws.access_key_id)) {
        CLIENT_ERR("invalid aws access key id");
        return false;
    }

    if (!_mongocrypt_validate_and_copy_string(aws_secret_access_key,
                                              aws_secret_access_key_len,
                                              &kms_providers->aws.secret_access_key)) {
        CLIENT_ERR("invalid aws secret access key");
        return false;
    }

    if (crypt->log.trace_enabled) {
        _mongocrypt_log(&crypt->log,
                        MONGOCRYPT_LOG_LEVEL_TRACE,
                        "%s (%s=\"%s\", %s=%d, %s=\"%s\", %s=%d)",
                        BSON_FUNC,
                        "aws_access_key_id",
                        kms_providers->aws.access_key_id,
                        "aws_access_key_id_len",
                        aws_access_key_id_len,
                        "aws_secret_access_key",
                        kms_providers->aws.secret_access_key,
                        "aws_secret_access_key_len",
                        aws_secret_access_key_len);
    }
    kms_providers->configured_providers |= MONGOCRYPT_KMS_PROVIDER_AWS;
    return true;
}

char *_mongocrypt_new_string_from_bytes(const void *in, int len) {
    const int max_bytes = 100;
    const int chars_per_byte = 2;
    int out_size = max_bytes * chars_per_byte;
    const unsigned char *src = in;
    char *out;
    char *ret;

    out_size += len > max_bytes ? (int)sizeof("...") : 1 /* for null */;
    out = bson_malloc0((size_t)out_size);
    BSON_ASSERT(out);

    ret = out;

    for (int i = 0; i < len && i < max_bytes; i++, out += chars_per_byte) {
        sprintf(out, "%02X", src[i]);
    }

    sprintf(out, (len > max_bytes) ? "..." : "");
    return ret;
}

char *_mongocrypt_new_json_string_from_binary(mongocrypt_binary_t *binary) {
    bson_t bson;
    uint32_t len;

    BSON_ASSERT_PARAM(binary);

    if (!_mongocrypt_binary_to_bson(binary, &bson) || !bson_validate(&bson, BSON_VALIDATE_NONE, NULL)) {
        char *hex;
        char *full_str;

        BSON_ASSERT(binary->len <= (uint32_t)INT_MAX);
        hex = _mongocrypt_new_string_from_bytes(binary->data, (int)binary->len);
        full_str = bson_strdup_printf("(malformed) %s", hex);
        bson_free(hex);
        return full_str;
    }
    return bson_as_canonical_extended_json(&bson, (size_t *)&len);
}

bool mongocrypt_setopt_schema_map(mongocrypt_t *crypt, mongocrypt_binary_t *schema_map) {
    ASSERT_MONGOCRYPT_PARAM_UNINIT(crypt);

    bson_t tmp;
    bson_error_t bson_err;
    mongocrypt_status_t *status = crypt->status;

    if (!schema_map || !mongocrypt_binary_data(schema_map)) {
        CLIENT_ERR("passed null schema map");
        return false;
    }

    if (!_mongocrypt_buffer_empty(&crypt->opts.schema_map)) {
        CLIENT_ERR("already set schema map");
        return false;
    }

    _mongocrypt_buffer_copy_from_binary(&crypt->opts.schema_map, schema_map);

    /* validate bson */
    if (!_mongocrypt_buffer_to_bson(&crypt->opts.schema_map, &tmp)) {
        CLIENT_ERR("invalid bson");
        return false;
    }

    if (!bson_validate_with_error(&tmp, BSON_VALIDATE_NONE, &bson_err)) {
        CLIENT_ERR("%s", bson_err.message);
        return false;
    }

    return true;
}

bool mongocrypt_setopt_encrypted_field_config_map(mongocrypt_t *crypt, mongocrypt_binary_t *efc_map) {
    ASSERT_MONGOCRYPT_PARAM_UNINIT(crypt);

    mongocrypt_status_t *status = crypt->status;
    bson_t as_bson;
    bson_error_t bson_err;

    if (!efc_map || !mongocrypt_binary_data(efc_map)) {
        CLIENT_ERR("passed null encrypted_field_config_map");
        return false;
    }

    if (!_mongocrypt_buffer_empty(&crypt->opts.encrypted_field_config_map)) {
        CLIENT_ERR("already set encrypted_field_config_map");
        return false;
    }

    _mongocrypt_buffer_copy_from_binary(&crypt->opts.encrypted_field_config_map, efc_map);

    /* validate bson */
    if (!_mongocrypt_buffer_to_bson(&crypt->opts.encrypted_field_config_map, &as_bson)) {
        CLIENT_ERR("invalid bson");
        return false;
    }

    if (!bson_validate_with_error(&as_bson, BSON_VALIDATE_NONE, &bson_err)) {
        CLIENT_ERR("%s", bson_err.message);
        return false;
    }

    return true;
}

bool mongocrypt_setopt_kms_provider_local(mongocrypt_t *crypt, mongocrypt_binary_t *key) {
    ASSERT_MONGOCRYPT_PARAM_UNINIT(crypt);

    mongocrypt_status_t *status = crypt->status;
    _mongocrypt_opts_kms_providers_t *const kms_providers = &crypt->opts.kms_providers;

    if (0 != (kms_providers->configured_providers & MONGOCRYPT_KMS_PROVIDER_LOCAL)) {
        CLIENT_ERR("local kms provider already set");
        return false;
    }

    if (!key) {
        CLIENT_ERR("passed null key");
        return false;
    }

    if (mongocrypt_binary_len(key) != MONGOCRYPT_KEY_LEN) {
        CLIENT_ERR("local key must be %d bytes", MONGOCRYPT_KEY_LEN);
        return false;
    }

    if (crypt->log.trace_enabled) {
        char *key_val;
        BSON_ASSERT(key->len <= (uint32_t)INT_MAX);
        key_val = _mongocrypt_new_string_from_bytes(key->data, (int)key->len);

        _mongocrypt_log(&crypt->log, MONGOCRYPT_LOG_LEVEL_TRACE, "%s (%s=\"%s\")", BSON_FUNC, "key", key_val);
        bson_free(key_val);
    }

    _mongocrypt_buffer_copy_from_binary(&kms_providers->local.key, key);
    kms_providers->configured_providers |= MONGOCRYPT_KMS_PROVIDER_LOCAL;
    return true;
}

typedef struct {
    /// Whether the load is successful
    bool okay;
    /// The DLL handle to the opened library.
    mcr_dll lib;
    /// A vtable for the functions in the DLL
    _mongo_crypt_v1_vtable vtable;
} _loaded_csfle;

/**
 * @brief Attempt to open the CSFLE dynamic library and initialize a vtable for
 * it.
 *
 * @param status is an optional status to set an error message if `mcr_dll_open` fails.
 */
static _loaded_csfle _try_load_csfle(const char *filepath, _mongocrypt_log_t *log, mongocrypt_status_t *status) {
    // Try to open the dynamic lib
    mcr_dll lib = mcr_dll_open(filepath);
    // Check for errors, which are represented by strings
    if (lib.error_string.data) {
        // Error opening candidate
        _mongocrypt_log(log,
                        MONGOCRYPT_LOG_LEVEL_WARNING,
                        "Error while opening candidate for CSFLE dynamic library [%s]: %s",
                        filepath,
                        lib.error_string.data);
        CLIENT_ERR("Error while opening candidate for CSFLE dynamic library [%s]: %s", filepath, lib.error_string.data);
        // Free resources, which will include the error string
        mcr_dll_close(lib);
        // Bad:
        return (_loaded_csfle){.okay = false};
    }

    // Successfully opened DLL
    _mongocrypt_log(log, MONGOCRYPT_LOG_LEVEL_TRACE, "Loading CSFLE dynamic library [%s]", filepath);

    // Construct the library vtable
    _mongo_crypt_v1_vtable vtable = {.okay = true};
#define X_FUNC(Name, RetType, ...)                                                                                     \
    {                                                                                                                  \
        /* Symbol names are qualified by the lib name and version: */                                                  \
        const char *symname = "mongo_crypt_v1_" #Name;                                                                 \
        vtable.Name = mcr_dll_sym(lib, symname);                                                                       \
        if (vtable.Name == NULL) {                                                                                     \
            /* The requested symbol is not present */                                                                  \
            _mongocrypt_log(log,                                                                                       \
                            MONGOCRYPT_LOG_LEVEL_ERROR,                                                                \
                            "Missing required symbol '%s' from CSFLE dynamic library [%s]",                            \
                            symname,                                                                                   \
                            filepath);                                                                                 \
            /* Mark the vtable as broken, but keep trying to load more symbols to                                      \
             * produce error messages for all missing symbols */                                                       \
            vtable.okay = false;                                                                                       \
        }                                                                                                              \
    }
    MONGOC_CSFLE_FUNCTIONS_X
#undef X_FUNC

    if (!vtable.okay) {
        mcr_dll_close(lib);
        _mongocrypt_log(log,
                        MONGOCRYPT_LOG_LEVEL_ERROR,
                        "One or more required symbols are missing from CSFLE dynamic library "
                        "[%s], so this dynamic library will not be used.",
                        filepath);
        return (_loaded_csfle){.okay = false};
    }

    // Success!
    _mongocrypt_log(log, MONGOCRYPT_LOG_LEVEL_INFO, "Opened CSFLE dynamic library [%s]", filepath);
    return (_loaded_csfle){.okay = true, .lib = lib, .vtable = vtable};
}

/**
 * @brief If the leading path element in `filepath` is $ORIGIN, replace that
 * with the directory containing the current executing module.
 *
 * @return true If no error occurred and the path is valid
 * @return false If there was an error and `filepath` cannot be processed
 */
static bool _try_replace_dollar_origin(mstr *filepath, _mongocrypt_log_t *log) {
    const mstr_view dollar_origin = mstrv_lit("$ORIGIN");

    BSON_ASSERT_PARAM(filepath);

    if (!mstr_starts_with(filepath->view, dollar_origin)) {
        // Nothing to replace
        return true;
    }
    // Check that the next char is a path separator or end-of-string:
    char peek = filepath->data[dollar_origin.len];
    if (peek != 0 && !mpath_is_sep(peek, MPATH_NATIVE)) {
        // Not a single path element
        return true;
    }
    // Replace $ORIGIN with the directory of the current module
    const current_module_result self_exe_r = current_module_path();
    if (self_exe_r.error) {
        // Failed to get the current module to load replace $ORIGIN
        mstr error = merror_system_error_string(self_exe_r.error);
        _mongocrypt_log(log,
                        MONGOCRYPT_LOG_LEVEL_WARNING,
                        "Error while loading the executable module path for "
                        "substitution of $ORIGIN in CSFLE search path [%s]: %s",
                        filepath->data,
                        error.data);
        mstr_free(error);
        return false;
    }
    const mstr_view self_dir = mpath_parent(self_exe_r.path.view, MPATH_NATIVE);
    mstr_inplace_splice(filepath, 0, dollar_origin.len, self_dir);
    mstr_free(self_exe_r.path);
    return true;
}

static _loaded_csfle _try_find_csfle(mongocrypt_t *crypt) {
    _loaded_csfle candidate_csfle = {0};
    mstr csfle_cand_filepath = MSTR_NULL;

    BSON_ASSERT_PARAM(crypt);

    if (crypt->opts.crypt_shared_lib_override_path.data) {
        // If an override path was specified, skip the library searching behavior
        csfle_cand_filepath = mstr_copy(crypt->opts.crypt_shared_lib_override_path.view);
        if (_try_replace_dollar_origin(&csfle_cand_filepath, &crypt->log)) {
            // Succesfully substituted $ORIGIN
            // Do not allow a plain filename to go through, as that will cause the
            // DLL load to search the system.
            mstr_assign(&csfle_cand_filepath, mpath_absolute(csfle_cand_filepath.view, MPATH_NATIVE));
            candidate_csfle = _try_load_csfle(csfle_cand_filepath.data, &crypt->log, crypt->status);
        }
    } else {
        // No override path was specified, so try to find it on the provided
        // search paths.
        for (int i = 0; i < crypt->opts.n_crypt_shared_lib_search_paths; ++i) {
            mstr_view cand_dir = crypt->opts.crypt_shared_lib_search_paths[i].view;
            mstr_view csfle_filename = mstrv_lit("mongo_crypt_v1" MCR_DLL_SUFFIX);
            if (mstr_eq(cand_dir, mstrv_lit("$SYSTEM"))) {
                // Caller wants us to search for the library on the system's default
                // library paths. Pass only the library's filename to cause dll_open
                // to search on the library paths.
                mstr_assign(&csfle_cand_filepath, mstr_copy(csfle_filename));
            } else {
                // Compose the candidate filepath:
                mstr_assign(&csfle_cand_filepath, mpath_join(cand_dir, csfle_filename, MPATH_NATIVE));
                if (!_try_replace_dollar_origin(&csfle_cand_filepath, &crypt->log)) {
                    // Error while substituting $ORIGIN
                    continue;
                }
            }
            // Try to load the file:
            candidate_csfle = _try_load_csfle(csfle_cand_filepath.data, &crypt->log, NULL /* status */);
            if (candidate_csfle.okay) {
                // Stop searching:
                break;
            }
        }
    }

    mstr_free(csfle_cand_filepath);
    return candidate_csfle;
}

/// Global state for the application's csfle library
typedef struct csfle_global_lib_state {
    /// Synchronization around the reference count:
    mongocrypt_mutex_t mtx;
    int refcount;
    /// The open library handle:
    mcr_dll dll;
    /// vtable for the APIs:
    _mongo_crypt_v1_vtable vtable;
    /// The global library state managed by the csfle library:
    mongo_crypt_v1_lib *csfle_lib;
} csfle_global_lib_state;

csfle_global_lib_state g_csfle_state;

static void init_csfle_state(void) {
    _mongocrypt_mutex_init(&g_csfle_state.mtx);
}

mlib_once_flag g_csfle_init_flag = MLIB_ONCE_INITIALIZER;

/**
 * @brief Verify that `found` refers to the same library that is globally loaded
 * for the application.
 *
 * @param crypt The requesting mongocrypt_t. Error information may be set
 * through here.
 * @param found The result of _try_load_csfle()
 * @return true If `found` matches the global state
 * @return false Otherwise
 *
 * @note This function assumes that the global csfle state is valid and will not
 * be destroyed by any other thread. (One must hold the reference count >= 1)
 */
static bool _validate_csfle_singleton(mongocrypt_t *crypt, _loaded_csfle found) {
    mongocrypt_status_t *status;

    BSON_ASSERT_PARAM(crypt);

    status = crypt->status;

    // Path to the existing loaded csfle:
    mcr_dll_path_result existing_path_ = mcr_dll_path(g_csfle_state.dll);
    assert(existing_path_.path.data && "Failed to get path to already-loaded csfle library");
    mstr_view existing_path = existing_path_.path.view;
    bool okay = true;
    if (!found.okay) {
        // There is one loaded, but we failed to find that same library. Error:
        CLIENT_ERR("An existing CSFLE library is loaded by the application at "
                   "[%s], but the current call to mongocrypt_init() failed to "
                   "find that same library.",
                   existing_path.data);
        okay = false;
    } else {
        // Get the path to what we found:
        mcr_dll_path_result found_path = mcr_dll_path(found.lib);
        assert(found_path.path.data
               && "Failed to get the dynamic library filepath of the library that "
                  "was loaded for csfle");
        if (!mstr_eq(found_path.path.view, existing_path)) {
            // Our find-result should only ever find the existing same library.
            // Error:
            CLIENT_ERR("An existing CSFLE library is loaded by the application at [%s], "
                       "but the current call to mongocrypt_init() attempted to load a "
                       "second CSFLE library from [%s]. This is not allowed.",
                       existing_path.data,
                       found_path.path.data);
            okay = false;
        }
        mstr_free(found_path.path);
        mstr_free(found_path.error_string);
    }

    mstr_free(existing_path_.path);
    mstr_free(existing_path_.error_string);
    return okay;
}

/**
 * @brief Drop a reference count to the global csfle loaded library.
 *
 * This should be called as part of mongocrypt_t destruction following a
 * successful loading of csfle.
 */
static void _csfle_drop_global_ref(void) {
    mlib_call_once(&g_csfle_init_flag, init_csfle_state);

    MONGOCRYPT_WITH_MUTEX(g_csfle_state.mtx) {
        assert(g_csfle_state.refcount > 0);
        int new_rc = --g_csfle_state.refcount;
        if (new_rc == 0) {
            mongo_crypt_v1_status *status = g_csfle_state.vtable.status_create();
            const int destroy_rc = g_csfle_state.vtable.lib_destroy(g_csfle_state.csfle_lib, status);
            if (destroy_rc != MONGO_CRYPT_V1_SUCCESS && status) {
                fprintf(stderr,
                        "csfle lib_destroy() failed: %s [Error %d, code %d]\n",
                        g_csfle_state.vtable.status_get_explanation(status),
                        g_csfle_state.vtable.status_get_error(status),
                        g_csfle_state.vtable.status_get_code(status));
            }
            g_csfle_state.vtable.status_destroy(status);
#ifndef __linux__
            mcr_dll_close(g_csfle_state.dll);
#else
            /// NOTE: On Linux, skip closing the CSFLE library itself, since a bug in
            /// the way ld-linux and GCC interact causes static destructors to not run
            /// during dlclose(). Still, free the error string:
            ///
            /// Please see: https://jira.mongodb.org/browse/SERVER-63710
            mstr_free(g_csfle_state.dll.error_string);
#endif
        }
    }
}

/**
 * @brief Following a call to _try_find_csfle, reconcile the result with the
 * current application-global csfle status.
 *
 * csfle contains global state that can only be loaded once for the entire
 * application. For this reason, there is a global object that manages the
 * loaded library. Attempts to create more than one mongocrypt_t that all
 * request csfle requires that all instances attempt to open the same csfle
 * library.
 *
 * This function checks if there is already a csfle loaded for the process. If
 * there is, we validate that the given find-result found the same library
 * that is already loaded. If not, then this function sets an error and returns
 * `false`.
 *
 * If there was no prior loaded csfle and the find-result indicates that it
 * found the library, this function will store the find-result in the global
 * state for later calls to mongocrypt_init() that request csfle.
 *
 * This function performs reference counting on the global state. Following a
 * successful call to this function (i.e. it returns `true`), one must have a
 * corresponding call to _csfle_drop_global_ref(), which will release the
 * resources acquired by this function.
 *
 * @param crypt The requesting mongocrypt_t instance. An error may be set
 * through this object.
 * @param found The result of _try_find_csfle().
 * @return true Upon success AND `found->okay`
 * @return false Otherwise.
 *
 * @note If there was no prior global state loaded, this function will steal
 * the library referenced by `found`. The caller should release `found->lib`
 * regardless.
 */
static bool _csfle_replace_or_take_validate_singleton(mongocrypt_t *crypt, _loaded_csfle *found) {
    mlib_call_once(&g_csfle_init_flag, init_csfle_state);

    // If we have a loaded library, create a csfle_status object to use with
    // lib_create
    mongo_crypt_v1_status *csfle_status = NULL;

    BSON_ASSERT_PARAM(crypt);
    BSON_ASSERT_PARAM(found);

    if (found->okay) {
        // Create the status. Note that this may fail, so do not assume
        // csfle_status is non-null.
        csfle_status = found->vtable.status_create();
    }

    /**
     * Atomically:
     *
     * 1. If there is an existing global library, increment its reference count.
     * 2. Otherwise, if we have successfully loaded a new csfle, replace the
     *    global library and set its reference count to 1.
     * 3. Otherwise, do nothing.
     */
    enum {
        TOOK_REFERENCE,
        DID_NOTHING,
        REPLACED_GLOBAL,
        LIB_CREATE_FAILED,
    } action;

    MONGOCRYPT_WITH_MUTEX(g_csfle_state.mtx) {
        if (g_csfle_state.refcount) {
            // Increment the refcount to prevent the global csfle library from
            // disappearing
            ++g_csfle_state.refcount;
            action = TOOK_REFERENCE;
        } else if (found->okay) {
            // We have found csfle, and no one else is holding one. Our result will
            // now become the global result.
            // Create the single csfle_lib object for the application:
            mongo_crypt_v1_lib *csfle_lib = found->vtable.lib_create(csfle_status);
            if (csfle_lib == NULL) {
                // Creation failed:
                action = LIB_CREATE_FAILED;
            } else {
                // Creation succeeded: Store the result:
                g_csfle_state.dll = found->lib;
                g_csfle_state.vtable = found->vtable;
                g_csfle_state.csfle_lib = csfle_lib;
                g_csfle_state.refcount = 1;
                action = REPLACED_GLOBAL;
            }
        } else {
            // We failed to load the library, and no one else has one either.
            // Nothing to do.
            action = DID_NOTHING;
        }
    }

    // Get the possible failure status information.
    mstr message = MSTR_NULL;
    int err = 0;
    int code = 0;
    if (csfle_status) {
        assert(found->okay);
        message = mstr_copy_cstr(found->vtable.status_get_explanation(csfle_status));
        err = found->vtable.status_get_error(csfle_status);
        code = found->vtable.status_get_code(csfle_status);
        found->vtable.status_destroy(csfle_status);
    }

    bool have_csfle = true;
    switch (action) {
    case TOOK_REFERENCE: {
        const bool is_valid = _validate_csfle_singleton(crypt, *found);
        if (!is_valid) {
            //  We've failed validation, so we're not going to continue to
            //  reference the global instance it. Drop it now:
            _csfle_drop_global_ref();
        }
        have_csfle = is_valid;
        break;
    }
    case REPLACED_GLOBAL:
        // Reset the library in the caller so they can't unload the DLL. The DLL
        // is now managed in the global variable.
        found->lib = MCR_DLL_NULL;
        _mongocrypt_log(&crypt->log, MONGOCRYPT_LOG_LEVEL_TRACE, "Loading new csfle library for the application.");
        have_csfle = true;
        break;
    case LIB_CREATE_FAILED:
        if (!message.data) {
            // We failed to obtain a message about the failure
            _mongocrypt_set_error(crypt->status,
                                  MONGOCRYPT_STATUS_ERROR_CRYPT_SHARED,
                                  MONGOCRYPT_GENERIC_ERROR_CODE,
                                  "csfle lib_create() failed");
        } else {
            // Record the message, error, and code from csfle about the failure
            _mongocrypt_set_error(crypt->status,
                                  MONGOCRYPT_STATUS_ERROR_CRYPT_SHARED,
                                  MONGOCRYPT_GENERIC_ERROR_CODE,
                                  "csfle lib_create() failed: %s [Error %d, code %d]",
                                  message.data,
                                  err,
                                  code);
        }
        have_csfle = false;
        break;
    case DID_NOTHING:
    default: have_csfle = false; break;
    }

    mstr_free(message);
    return have_csfle;
}

/**
 * @return true If the given mongocrypt wants csfle
 * @return false Otherwise
 *
 * @note "Requesting csfle" means that it has set at least one search path OR
 * has set the override path
 */
static bool _wants_csfle(mongocrypt_t *c) {
    BSON_ASSERT_PARAM(c);

    if (c->opts.bypass_query_analysis) {
        return false;
    }
    return c->opts.n_crypt_shared_lib_search_paths != 0 || c->opts.crypt_shared_lib_override_path.data != NULL;
}

/**
 * @brief Try to enable csfle for the given mongocrypt
 *
 * @param crypt The crypt object for which we should enable csfle
 * @return true If no errors occurred
 * @return false Otherwise
 *
 * @note Returns `true` even if loading fails to find the csfle library on the
 * requested paths. `false` is only for hard-errors, which includes failure to
 * load from the override path.
 */
static bool _try_enable_csfle(mongocrypt_t *crypt) {
    mongocrypt_status_t *status;
    _loaded_csfle found;

    BSON_ASSERT_PARAM(crypt);

    found = _try_find_csfle(crypt);

    status = crypt->status;

    // If a crypt_shared override path was specified, but we did not succeed in
    // loading crypt_shared, that is a hard-error.
    if (crypt->opts.crypt_shared_lib_override_path.data && !found.okay) {
        // Wrap error with additional information.
        CLIENT_ERR("A crypt_shared override path was specified [%s], but we failed to open a dynamic "
                   "library at that location. Load error: [%s]",
                   crypt->opts.crypt_shared_lib_override_path.data,
                   mongocrypt_status_message(crypt->status, NULL /* len */));
        return false;
    }

    // Attempt to validate the try-find result against the global state:
    const bool got_csfle = _csfle_replace_or_take_validate_singleton(crypt, &found);
    // Close the lib we found (may have been stolen in validate_singleton())
    mcr_dll_close(found.lib);

    if (got_csfle) {
        crypt->csfle = g_csfle_state.vtable;
        crypt->csfle_lib = g_csfle_state.csfle_lib;
    }
    // In cast of failure, validate_singleton() will set a non-ok status.
    return mongocrypt_status_type(status) == MONGOCRYPT_STATUS_OK;
}

bool mongocrypt_init(mongocrypt_t *crypt) {
    BSON_ASSERT_PARAM(crypt);

    mongocrypt_status_t *status = crypt->status;
    if (crypt->initialized) {
        CLIENT_ERR("already initialized");
        return false;
    }

    crypt->initialized = true;

    if (!mongocrypt_status_ok(crypt->status)) {
        return false;
    }

    if (!_mongocrypt_opts_validate(&crypt->opts, status)) {
        return false;
    }

    if (crypt->opts.log_fn) {
        _mongocrypt_log_set_fn(&crypt->log, crypt->opts.log_fn, crypt->opts.log_ctx);
    }

    if (!crypt->crypto) {
#ifndef MONGOCRYPT_ENABLE_CRYPTO
        CLIENT_ERR("libmongocrypt built with native crypto disabled. crypto "
                   "hooks required");
        return false;
#else
        /* set default hooks. */
        crypt->crypto = bson_malloc0(sizeof(*crypt->crypto));
        BSON_ASSERT(crypt->crypto);
#endif
    }

    if (!_wants_csfle(crypt)) {
        // User does not want csfle. Just succeed.
        return true;
    }

    return _try_enable_csfle(crypt);
}

bool mongocrypt_status(mongocrypt_t *crypt, mongocrypt_status_t *out) {
    BSON_ASSERT_PARAM(crypt);

    if (!out) {
        mongocrypt_status_t *status = crypt->status;
        CLIENT_ERR("argument 'out' is required");
        return false;
    }

    if (!mongocrypt_status_ok(crypt->status)) {
        _mongocrypt_status_copy_to(crypt->status, out);
        return false;
    }
    _mongocrypt_status_reset(out);
    return true;
}

void mongocrypt_destroy(mongocrypt_t *crypt) {
    if (!crypt) {
        return;
    }
    _mongocrypt_opts_cleanup(&crypt->opts);
    _mongocrypt_cache_cleanup(&crypt->cache_collinfo);
    _mongocrypt_cache_cleanup(&crypt->cache_key);
    _mongocrypt_mutex_cleanup(&crypt->mutex);
    _mongocrypt_log_cleanup(&crypt->log);
    mongocrypt_status_destroy(crypt->status);
    bson_free(crypt->crypto);
    _mongocrypt_cache_oauth_destroy(crypt->cache_oauth_azure);
    _mongocrypt_cache_oauth_destroy(crypt->cache_oauth_gcp);

    if (crypt->csfle.okay) {
        _csfle_drop_global_ref();
        crypt->csfle.okay = false;
    }

    bson_free(crypt);
}

const char *mongocrypt_crypt_shared_lib_version_string(const mongocrypt_t *crypt, uint32_t *len) {
    BSON_ASSERT_PARAM(crypt);

    if (!crypt->csfle.okay) {
        if (len) {
            *len = 0;
        }
        return NULL;
    }
    const char *version = crypt->csfle.get_version_str();
    if (len) {
        *len = (uint32_t)(strlen(version));
    }
    return version;
}

uint64_t mongocrypt_crypt_shared_lib_version(const mongocrypt_t *crypt) {
    BSON_ASSERT_PARAM(crypt);

    if (!crypt->csfle.okay) {
        return 0;
    }
    return crypt->csfle.get_version();
}

bool _mongocrypt_validate_and_copy_string(const char *in, int32_t in_len, char **out) {
    BSON_ASSERT_PARAM(out);

    if (!in || in_len < -1) {
        return false;
    }

    const size_t len = in_len < 0 ? strlen(in) : (size_t)in_len;

    if (!bson_utf8_validate(in, len, false)) {
        return false;
    }
    *out = bson_strndup(in, len);
    return true;
}

bool mongocrypt_setopt_crypto_hooks(mongocrypt_t *crypt,
                                    mongocrypt_crypto_fn aes_256_cbc_encrypt,
                                    mongocrypt_crypto_fn aes_256_cbc_decrypt,
                                    mongocrypt_random_fn random,
                                    mongocrypt_hmac_fn hmac_sha_512,
                                    mongocrypt_hmac_fn hmac_sha_256,
                                    mongocrypt_hash_fn sha_256,
                                    void *ctx) {
    ASSERT_MONGOCRYPT_PARAM_UNINIT(crypt);

    mongocrypt_status_t *status = crypt->status;

    if (!crypt->crypto) {
        crypt->crypto = bson_malloc0(sizeof(*crypt->crypto));
        BSON_ASSERT(crypt->crypto);
    }

    crypt->crypto->hooks_enabled = true;
    crypt->crypto->ctx = ctx;

    if (!aes_256_cbc_encrypt) {
        CLIENT_ERR("aes_256_cbc_encrypt not set");
        return false;
    }
    crypt->crypto->aes_256_cbc_encrypt = aes_256_cbc_encrypt;

    if (!aes_256_cbc_decrypt) {
        CLIENT_ERR("aes_256_cbc_decrypt not set");
        return false;
    }
    crypt->crypto->aes_256_cbc_decrypt = aes_256_cbc_decrypt;

    if (!random) {
        CLIENT_ERR("random not set");
        return false;
    }
    crypt->crypto->random = random;

    if (!hmac_sha_512) {
        CLIENT_ERR("hmac_sha_512 not set");
        return false;
    }
    crypt->crypto->hmac_sha_512 = hmac_sha_512;

    if (!hmac_sha_256) {
        CLIENT_ERR("hmac_sha_256 not set");
        return false;
    }
    crypt->crypto->hmac_sha_256 = hmac_sha_256;

    if (!sha_256) {
        CLIENT_ERR("sha_256 not set");
        return false;
    }
    crypt->crypto->sha_256 = sha_256;

    return true;
}

bool mongocrypt_setopt_crypto_hook_sign_rsaes_pkcs1_v1_5(mongocrypt_t *crypt,
                                                         mongocrypt_hmac_fn sign_rsaes_pkcs1_v1_5,
                                                         void *sign_ctx) {
    ASSERT_MONGOCRYPT_PARAM_UNINIT(crypt);

    if (crypt->opts.sign_rsaes_pkcs1_v1_5) {
        mongocrypt_status_t *status = crypt->status;
        CLIENT_ERR("signature hook already set");
        return false;
    }

    crypt->opts.sign_rsaes_pkcs1_v1_5 = sign_rsaes_pkcs1_v1_5;
    crypt->opts.sign_ctx = sign_ctx;
    return true;
}

bool mongocrypt_setopt_aes_256_ctr(mongocrypt_t *crypt,
                                   mongocrypt_crypto_fn aes_256_ctr_encrypt,
                                   mongocrypt_crypto_fn aes_256_ctr_decrypt,
                                   void *ctx) {
    ASSERT_MONGOCRYPT_PARAM_UNINIT(crypt);

    mongocrypt_status_t *status = crypt->status;

    if (!crypt->crypto) {
        crypt->crypto = bson_malloc0(sizeof(*crypt->crypto));
        BSON_ASSERT(crypt->crypto);
    }

    if (!aes_256_ctr_encrypt) {
        CLIENT_ERR("aes_256_ctr_encrypt not set");
        return false;
    }

    if (!aes_256_ctr_decrypt) {
        CLIENT_ERR("aes_256_ctr_decrypt not set");
        return false;
    }

    crypt->crypto->aes_256_ctr_encrypt = aes_256_ctr_encrypt;
    crypt->crypto->aes_256_ctr_decrypt = aes_256_ctr_decrypt;

    return true;
}

bool mongocrypt_setopt_aes_256_ecb(mongocrypt_t *crypt, mongocrypt_crypto_fn aes_256_ecb_encrypt, void *ctx) {
    ASSERT_MONGOCRYPT_PARAM_UNINIT(crypt);

    if (!crypt->crypto) {
        crypt->crypto = bson_malloc0(sizeof(*crypt->crypto));
        BSON_ASSERT(crypt->crypto);
    }

    if (!aes_256_ecb_encrypt) {
        mongocrypt_status_t *status = crypt->status;
        CLIENT_ERR("aes_256_ecb_encrypt not set");
        return false;
    }

    crypt->crypto->aes_256_ecb_encrypt = aes_256_ecb_encrypt;

    return true;
}

bool mongocrypt_setopt_kms_providers(mongocrypt_t *crypt, mongocrypt_binary_t *kms_providers_definition) {
    ASSERT_MONGOCRYPT_PARAM_UNINIT(crypt);
    BSON_ASSERT_PARAM(kms_providers_definition);

    return _mongocrypt_parse_kms_providers(kms_providers_definition,
                                           &crypt->opts.kms_providers,
                                           crypt->status,
                                           &crypt->log);
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

        if (0 == strcmp(field_name, "azure") && bson_empty(&field_bson)) {
            kms_providers->need_credentials |= MONGOCRYPT_KMS_PROVIDER_AZURE;
        } else if (0 == strcmp(field_name, "azure")) {
            if (0 != (kms_providers->configured_providers & MONGOCRYPT_KMS_PROVIDER_AZURE)) {
                CLIENT_ERR("azure KMS provider already set");
                return false;
            }

            if (!_mongocrypt_parse_optional_utf8(&as_bson,
                                                 "azure.accessToken",
                                                 &kms_providers->azure.access_token,
                                                 status)) {
                return false;
            }

            if (kms_providers->azure.access_token) {
                // Caller provides an accessToken directly
                if (!_mongocrypt_check_allowed_fields(&as_bson, "azure", status, "accessToken")) {
                    return false;
                }
                kms_providers->configured_providers |= MONGOCRYPT_KMS_PROVIDER_AZURE;
                continue;
            }

            // No accessToken given, so we'll need to look one up on our own later
            // using the Azure API

            if (!_mongocrypt_parse_required_utf8(&as_bson, "azure.tenantId", &kms_providers->azure.tenant_id, status)) {
                return false;
            }

            if (!_mongocrypt_parse_required_utf8(&as_bson, "azure.clientId", &kms_providers->azure.client_id, status)) {
                return false;
            }

            if (!_mongocrypt_parse_required_utf8(&as_bson,
                                                 "azure.clientSecret",
                                                 &kms_providers->azure.client_secret,
                                                 status)) {
                return false;
            }

            if (!_mongocrypt_parse_optional_endpoint(&as_bson,
                                                     "azure.identityPlatformEndpoint",
                                                     &kms_providers->azure.identity_platform_endpoint,
                                                     NULL /* opts */,
                                                     status)) {
                return false;
            }

            if (!_mongocrypt_check_allowed_fields(&as_bson,
                                                  "azure",
                                                  status,
                                                  "tenantId",
                                                  "clientId",
                                                  "clientSecret",
                                                  "identityPlatformEndpoint")) {
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

            if (!_mongocrypt_parse_optional_utf8(&as_bson,
                                                 "gcp.accessToken",
                                                 &kms_providers->gcp.access_token,
                                                 status)) {
                return false;
            }

            if (NULL != kms_providers->gcp.access_token) {
                /* "gcp" document has form:
                 * {
                 *    "accessToken": <required UTF-8>
                 * }
                 */
                if (!_mongocrypt_check_allowed_fields(&as_bson, "gcp", status, "accessToken")) {
                    return false;
                }
                kms_providers->configured_providers |= MONGOCRYPT_KMS_PROVIDER_GCP;
                continue;
            }

            /* "gcp" document has form:
             * {
             *    "email": <required UTF-8>
             *    "privateKey": <required UTF-8 or Binary>
             * }
             */
            if (!_mongocrypt_parse_required_utf8(&as_bson, "gcp.email", &kms_providers->gcp.email, status)) {
                return false;
            }

            if (!_mongocrypt_parse_required_binary(&as_bson,
                                                   "gcp.privateKey",
                                                   &kms_providers->gcp.private_key,
                                                   status)) {
                return false;
            }

            if (!_mongocrypt_parse_optional_endpoint(&as_bson,
                                                     "gcp.endpoint",
                                                     &kms_providers->gcp.endpoint,
                                                     NULL /* opts */,
                                                     status)) {
                return false;
            }

            if (!_mongocrypt_check_allowed_fields(&as_bson, "gcp", status, "email", "privateKey", "endpoint")) {
                return false;
            }
            kms_providers->configured_providers |= MONGOCRYPT_KMS_PROVIDER_GCP;
        } else if (0 == strcmp(field_name, "local") && bson_empty(&field_bson)) {
            kms_providers->need_credentials |= MONGOCRYPT_KMS_PROVIDER_LOCAL;
        } else if (0 == strcmp(field_name, "local")) {
            if (!_mongocrypt_parse_required_binary(&as_bson, "local.key", &kms_providers->local.key, status)) {
                return false;
            }

            if (kms_providers->local.key.len != MONGOCRYPT_KEY_LEN) {
                CLIENT_ERR("local key must be %d bytes", MONGOCRYPT_KEY_LEN);
                return false;
            }

            if (!_mongocrypt_check_allowed_fields(&as_bson, "local", status, "key")) {
                return false;
            }
            kms_providers->configured_providers |= MONGOCRYPT_KMS_PROVIDER_LOCAL;
        } else if (0 == strcmp(field_name, "aws") && bson_empty(&field_bson)) {
            kms_providers->need_credentials |= MONGOCRYPT_KMS_PROVIDER_AWS;
        } else if (0 == strcmp(field_name, "aws")) {
            if (!_mongocrypt_parse_required_utf8(&as_bson,
                                                 "aws.accessKeyId",
                                                 &kms_providers->aws.access_key_id,
                                                 status)) {
                return false;
            }
            if (!_mongocrypt_parse_required_utf8(&as_bson,
                                                 "aws.secretAccessKey",
                                                 &kms_providers->aws.secret_access_key,
                                                 status)) {
                return false;
            }

            if (!_mongocrypt_parse_optional_utf8(&as_bson,
                                                 "aws.sessionToken",
                                                 &kms_providers->aws.session_token,
                                                 status)) {
                return false;
            }

            if (!_mongocrypt_check_allowed_fields(&as_bson,
                                                  "aws",
                                                  status,
                                                  "accessKeyId",
                                                  "secretAccessKey",
                                                  "sessionToken")) {
                return false;
            }
            kms_providers->configured_providers |= MONGOCRYPT_KMS_PROVIDER_AWS;
        } else if (0 == strcmp(field_name, "kmip") && bson_empty(&field_bson)) {
            kms_providers->need_credentials |= MONGOCRYPT_KMS_PROVIDER_KMIP;
        } else if (0 == strcmp(field_name, "kmip")) {
            _mongocrypt_endpoint_parse_opts_t opts = {0};

            opts.allow_empty_subdomain = true;
            if (!_mongocrypt_parse_required_endpoint(&as_bson,
                                                     "kmip.endpoint",
                                                     &kms_providers->kmip.endpoint,
                                                     &opts,
                                                     status)) {
                return false;
            }

            if (!_mongocrypt_check_allowed_fields(&as_bson, "kmip", status, "endpoint")) {
                return false;
            }
            kms_providers->configured_providers |= MONGOCRYPT_KMS_PROVIDER_KMIP;
        } else {
            CLIENT_ERR("unsupported KMS provider: %s", field_name);
            return false;
        }
    }

    if (log && log->trace_enabled) {
        char *as_str = bson_as_json(&as_bson, NULL);
        _mongocrypt_log(log, MONGOCRYPT_LOG_LEVEL_TRACE, "%s (%s=\"%s\")", BSON_FUNC, "kms_providers", as_str);
        bson_free(as_str);
    }

    return true;
}

void mongocrypt_setopt_append_crypt_shared_lib_search_path(mongocrypt_t *crypt, const char *path) {
    BSON_ASSERT_PARAM(crypt);
    BSON_ASSERT_PARAM(path);

    // Dup the path string for us to manage
    mstr pathdup = mstr_copy_cstr(path);
    // Increase array len
    BSON_ASSERT(crypt->opts.n_crypt_shared_lib_search_paths < INT_MAX);
    const int new_len = crypt->opts.n_crypt_shared_lib_search_paths + 1;
    BSON_ASSERT(new_len > 0 && sizeof(mstr) <= SIZE_MAX / (size_t)new_len);
    mstr *const new_array = bson_realloc(crypt->opts.crypt_shared_lib_search_paths, sizeof(mstr) * (size_t)new_len);

    // Store the path
    new_array[new_len - 1] = pathdup;
    // Write back opts
    crypt->opts.crypt_shared_lib_search_paths = new_array;
    crypt->opts.n_crypt_shared_lib_search_paths = new_len;
}

void mongocrypt_setopt_use_need_kms_credentials_state(mongocrypt_t *crypt) {
    BSON_ASSERT_PARAM(crypt);

    crypt->opts.use_need_kms_credentials_state = true;
}

void mongocrypt_setopt_set_crypt_shared_lib_path_override(mongocrypt_t *crypt, const char *path) {
    BSON_ASSERT_PARAM(crypt);
    BSON_ASSERT_PARAM(path);

    mstr_assign(&crypt->opts.crypt_shared_lib_override_path, mstr_copy_cstr(path));
}

bool _mongocrypt_needs_credentials(mongocrypt_t *crypt) {
    BSON_ASSERT_PARAM(crypt);

    if (!crypt->opts.use_need_kms_credentials_state) {
        return false;
    }

    return crypt->opts.kms_providers.need_credentials != 0;
}

bool _mongocrypt_needs_credentials_for_provider(mongocrypt_t *crypt, _mongocrypt_kms_provider_t provider) {
    BSON_ASSERT_PARAM(crypt);

    if (!crypt->opts.use_need_kms_credentials_state) {
        return false;
    }

    return (crypt->opts.kms_providers.need_credentials & (int)provider) != 0;
}

void mongocrypt_setopt_bypass_query_analysis(mongocrypt_t *crypt) {
    BSON_ASSERT_PARAM(crypt);

    crypt->opts.bypass_query_analysis = true;
}

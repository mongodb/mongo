/**
 *    Copyright (C) 2026-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/scripting/mozjs/wasm/api.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/scripting/mozjs/shared/mozjs_error_types.h"
#include "mongo/scripting/mozjs/shared/mozjs_wasm_startup_options.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "engine.h"
#include "error.h"

namespace {

// Maximum JavaScript source code size (1 MB)
constexpr size_t kMaxJsSourceSize = 1 * 1024 * 1024;

// Maximum BSON document size (16mb)
constexpr size_t kMaxBsonSize = 16 * 1024 * 1024;

// Maximum number of functions that can be created
constexpr size_t kMaxFunctions = 10000;

// Default JS heap size in MB for the WASM engine.
constexpr uint32_t kDefaultHeapSizeMB = 100;

}  // namespace
namespace mongo {
namespace mozjs {
namespace wasm {

// Thread-safety: This module is designed for single-threaded use within a
// single WASM instance. Each WASM instance gets its own linear memory and
// thus its own copy of g_engine. Do not share a WASM instance across threads.
static MozJSScriptEngine g_engine;

}  // namespace wasm
}  // namespace mozjs
}  // namespace mongo

static void set_opt_string(api_option_string_t* out, const char* ptr, size_t len) {
    if (ptr && len > 0) {
        out->is_some = true;
        api_string_dup_n(&out->val, ptr, len);
    } else {
        out->is_some = false;
        out->val.ptr = nullptr;
        out->val.len = 0;
    }
}

static void fill_wit_error(exports_mongo_mozjs_mozjs_wasm_mozjs_error_t* out,
                           const mongo::mozjs::wasm::wasm_mozjs_error_t& in) {
    out->code = static_cast<exports_mongo_mozjs_mozjs_err_code_t>(in.code);

    set_opt_string(&out->msg, in.msg, in.msg_len);
    set_opt_string(&out->filename, in.filename, in.filename_len);
    set_opt_string(&out->stack, in.stack, in.stack_len);

    out->line = in.line;
    out->column = in.column;
}

static bool return_err(exports_mongo_mozjs_mozjs_wasm_mozjs_error_t* err,
                       mongo::mozjs::wasm::wasm_mozjs_error_t* native_err) {
    if (!native_err) {
        return false;
    }
    fill_wit_error(err, *native_err);
    mongo::mozjs::wasm::clear_error(native_err);
    // wit-bindgen C convention: return false means error (adapter does: is_err = !retval)
    return false;
}

// Returns false on allocation failure so callers can propagate OOM errors.
static bool list_u8_dup(api_list_u8_t* out, const uint8_t* data, size_t len) {
    if (len == 0) {
        out->ptr = nullptr;
        out->len = 0;
        return true;
    }
    auto* p = static_cast<uint8_t*>(std::malloc(len));
    if (!p) {
        out->ptr = nullptr;
        out->len = 0;
        return false;  // OOM
    }
    std::memcpy(p, data, len);
    out->ptr = p;
    out->len = len;
    return true;
}

// Validates BSON data before constructing BSONObj to prevent crashes from
// malformed input. This performs basic structural validation.

static bool validate_bson(const uint8_t* data,
                          size_t len,
                          mongo::mozjs::wasm::wasm_mozjs_error_t* err) {
    if (len < 5 || len > kMaxBsonSize) {
        if (err) {
            err->code = mongo::mozjs::wasm::SM_E_INVALID_ARG;
            mongo::mozjs::wasm::set_string(&err->msg,
                                           &err->msg_len,
                                           len < 5 ? "BSON too small (minimum 5 bytes)"
                                                   : "BSON exceeds 16 MB maximum");
        }
        return false;
    }

    int32_t declared_size;
    std::memcpy(&declared_size, data, sizeof(declared_size));

    if (declared_size < 5 || static_cast<size_t>(declared_size) > len ||
        data[declared_size - 1] != 0) {
        if (err) {
            err->code = mongo::mozjs::wasm::SM_E_INVALID_ARG;
            mongo::mozjs::wasm::set_string(
                &err->msg, &err->msg_len, "BSON header invalid (size mismatch or no terminator)");
        }
        return false;
    }

    return true;
}

// Track function count for limiting.
static size_t g_function_count = 0;

// Exported Functions from `mongo:mozjs/mozjs`

extern "C" bool exports_mongo_mozjs_mozjs_initialize_engine(
    exports_mongo_mozjs_mozjs_wasm_mozjs_startup_options_t* options,
    exports_mongo_mozjs_mozjs_ok_t* ret,
    exports_mongo_mozjs_mozjs_wasm_mozjs_error_t* err) {
    mongo::mozjs::wasm::wasm_mozjs_error_t e{};
    mongo::mozjs::wasm::wasm_mozjs_startup_options_t opt{};
    opt.heapSize = options->heap_size_mb > 0 ? options->heap_size_mb : kDefaultHeapSizeMB;

    int64_t rc = mongo::mozjs::wasm::g_engine.init(&opt, &e);

    if (rc == mongo::mozjs::wasm::SM_OK) {
        if (ret)
            *ret = 0;  // Set the ok value
        return true;
    }
    return return_err(err, &e);
}

extern "C" bool exports_mongo_mozjs_mozjs_shutdown_engine(
    exports_mongo_mozjs_mozjs_ok_t* ret, exports_mongo_mozjs_mozjs_wasm_mozjs_error_t* err) {
    mongo::mozjs::wasm::wasm_mozjs_error_t e{};
    int64_t rc = mongo::mozjs::wasm::g_engine.shutdown(&e);
    if (rc == mongo::mozjs::wasm::SM_OK) {
        g_function_count = 0;
        if (ret)
            *ret = 0;
        return true;
    }
    return return_err(err, &e);
}

extern "C" bool exports_mongo_mozjs_mozjs_interrupt_current_op(
    exports_mongo_mozjs_mozjs_ok_t* ret, exports_mongo_mozjs_mozjs_wasm_mozjs_error_t* err) {
    mongo::mozjs::wasm::wasm_mozjs_error_t e{};
    int64_t rc = mongo::mozjs::wasm::g_engine.interrupt(&e);
    if (rc == mongo::mozjs::wasm::SM_OK) {
        if (ret)
            *ret = 0;
        return true;
    }
    return return_err(err, &e);
}

extern "C" bool exports_mongo_mozjs_mozjs_create_function(
    api_list_u8_t* source,
    exports_mongo_mozjs_mozjs_function_handle_t* ret,
    exports_mongo_mozjs_mozjs_wasm_mozjs_error_t* err) {
    mongo::mozjs::wasm::wasm_mozjs_error_t e{};

    const uint8_t* bytes = source ? source->ptr : nullptr;
    size_t len = source ? source->len : 0;

    if (len > kMaxJsSourceSize) {
        e.code = mongo::mozjs::wasm::SM_E_INVALID_ARG;
        mongo::mozjs::wasm::set_string(&e.msg, &e.msg_len, "JS source exceeds maximum size (1 MB)");
        return return_err(err, &e);
    }

    if (g_function_count >= kMaxFunctions) {
        e.code = mongo::mozjs::wasm::SM_E_NOMEM;
        mongo::mozjs::wasm::set_string(
            &e.msg, &e.msg_len, "Maximum function count reached (10000)");
        return return_err(err, &e);
    }

    uint64_t handle = 0;
    int64_t rc = mongo::mozjs::wasm::g_engine.createFunction(bytes, len, &handle, &e);
    if (rc == mongo::mozjs::wasm::SM_OK) {
        g_function_count++;
        *ret = static_cast<exports_mongo_mozjs_mozjs_function_handle_t>(handle);
        return true;
    }
    return return_err(err, &e);
}

extern "C" bool exports_mongo_mozjs_mozjs_invoke_function(
    exports_mongo_mozjs_mozjs_function_handle_t handle,
    api_list_u8_t* bson,
    exports_mongo_mozjs_mozjs_ok_t* ret,
    exports_mongo_mozjs_mozjs_wasm_mozjs_error_t* err) {
    mongo::mozjs::wasm::wasm_mozjs_error_t e{};

    if (handle == 0) {
        e.code = mongo::mozjs::wasm::SM_E_INVALID_ARG;
        return return_err(err, &e);
    }

    mongo::BSONObj argsObj;
    if (bson && bson->ptr && bson->len > 0) {
        if (!validate_bson(bson->ptr, bson->len, &e)) {
            return return_err(err, &e);
        }
        argsObj = mongo::BSONObj(reinterpret_cast<const char*>(bson->ptr));
    }

    mongo::BSONObj outBson;
    auto rc = static_cast<int64_t>(mongo::mozjs::wasm::g_engine.invokeFunction(
        static_cast<uint64_t>(handle), std::move(argsObj), &outBson, &e));

    if (rc == mongo::mozjs::wasm::SM_OK) {
        if (ret)
            *ret = 0;
        return true;
    }
    return return_err(err, &e);
}

extern "C" bool exports_mongo_mozjs_mozjs_get_return_value_bson(
    api_list_u8_t* ret, exports_mongo_mozjs_mozjs_wasm_mozjs_error_t* err) {
    mongo::mozjs::wasm::wasm_mozjs_error_t e{};
    mongo::BSONObj out;

    auto rc = static_cast<int64_t>(mongo::mozjs::wasm::g_engine.getReturnValueBson(&out, &e));
    if (rc != mongo::mozjs::wasm::SM_OK) {
        return return_err(err, &e);
    }

    if (!list_u8_dup(ret, reinterpret_cast<const uint8_t*>(out.objdata()), out.objsize())) {
        e.code = mongo::mozjs::wasm::SM_E_NOMEM;
        mongo::mozjs::wasm::set_string(
            &e.msg, &e.msg_len, "OOM: failed to allocate BSON return buffer");
        return return_err(err, &e);
    }
    return true;
}

extern "C" bool exports_mongo_mozjs_mozjs_set_global(
    api_string_t* name,
    api_list_u8_t* bson_value,
    exports_mongo_mozjs_mozjs_ok_t* ret,
    exports_mongo_mozjs_mozjs_wasm_mozjs_error_t* err) {
    mongo::mozjs::wasm::wasm_mozjs_error_t e{};

    if (!name || name->len == 0) {
        e.code = mongo::mozjs::wasm::SM_E_INVALID_ARG;
        return return_err(err, &e);
    }

    mongo::BSONObj valueObj;
    if (bson_value && bson_value->ptr && bson_value->len > 0) {
        if (!validate_bson(bson_value->ptr, bson_value->len, &e)) {
            return return_err(err, &e);
        }
        valueObj = mongo::BSONObj(reinterpret_cast<const char*>(bson_value->ptr));
    }

    auto rc = static_cast<int64_t>(mongo::mozjs::wasm::g_engine.setGlobal(
        reinterpret_cast<const char*>(name->ptr), name->len, valueObj, &e));

    if (rc == mongo::mozjs::wasm::SM_OK) {
        if (ret)
            *ret = 0;
        return true;
    }
    return return_err(err, &e);
}

extern "C" bool exports_mongo_mozjs_mozjs_get_global(
    api_string_t* name, api_list_u8_t* ret, exports_mongo_mozjs_mozjs_wasm_mozjs_error_t* err) {
    mongo::mozjs::wasm::wasm_mozjs_error_t e{};

    if (!name || name->len == 0) {
        e.code = mongo::mozjs::wasm::SM_E_INVALID_ARG;
        return return_err(err, &e);
    }

    mongo::BSONObj out;
    auto rc = static_cast<int64_t>(mongo::mozjs::wasm::g_engine.getGlobal(
        reinterpret_cast<const char*>(name->ptr), name->len, &out, &e));

    if (rc != mongo::mozjs::wasm::SM_OK) {
        return return_err(err, &e);
    }

    if (!list_u8_dup(ret, reinterpret_cast<const uint8_t*>(out.objdata()), out.objsize())) {
        e.code = mongo::mozjs::wasm::SM_E_NOMEM;
        mongo::mozjs::wasm::set_string(
            &e.msg, &e.msg_len, "OOM: failed to allocate global return buffer");
        return return_err(err, &e);
    }
    return true;
}

extern "C" bool exports_mongo_mozjs_mozjs_set_global_value(
    api_string_t* name,
    api_list_u8_t* bson_element,
    exports_mongo_mozjs_mozjs_ok_t* ret,
    exports_mongo_mozjs_mozjs_wasm_mozjs_error_t* err) {
    mongo::mozjs::wasm::wasm_mozjs_error_t e{};

    if (!name || name->len == 0) {
        e.code = mongo::mozjs::wasm::SM_E_INVALID_ARG;
        return return_err(err, &e);
    }

    mongo::BSONObj valueObj;
    if (bson_element && bson_element->ptr && bson_element->len > 0) {
        if (!validate_bson(bson_element->ptr, bson_element->len, &e)) {
            return return_err(err, &e);
        }
        valueObj = mongo::BSONObj(reinterpret_cast<const char*>(bson_element->ptr));
    }

    auto rc = static_cast<int64_t>(mongo::mozjs::wasm::g_engine.setGlobalValue(
        reinterpret_cast<const char*>(name->ptr), name->len, valueObj, &e));

    if (rc == mongo::mozjs::wasm::SM_OK) {
        if (ret)
            *ret = 0;
        return true;
    }
    return return_err(err, &e);
}

extern "C" bool exports_mongo_mozjs_mozjs_setup_emit(
    int64_t* maybe_byte_limit,
    exports_mongo_mozjs_mozjs_ok_t* ret,
    exports_mongo_mozjs_mozjs_wasm_mozjs_error_t* err) {
    mongo::mozjs::wasm::wasm_mozjs_error_t e{};

    bool hasLimit = (maybe_byte_limit != nullptr);
    int64_t limit = hasLimit ? *maybe_byte_limit : 0;

    auto rc = static_cast<int64_t>(mongo::mozjs::wasm::g_engine.setupEmit(limit, hasLimit, &e));

    if (rc == mongo::mozjs::wasm::SM_OK) {
        if (ret)
            *ret = 0;
        return true;
    }
    return return_err(err, &e);
}

extern "C" bool exports_mongo_mozjs_mozjs_invoke_predicate(
    exports_mongo_mozjs_mozjs_function_handle_t handle,
    api_list_u8_t* document,
    bool* ret,
    exports_mongo_mozjs_mozjs_wasm_mozjs_error_t* err) {
    mongo::mozjs::wasm::wasm_mozjs_error_t e{};

    if (handle == 0) {
        e.code = mongo::mozjs::wasm::SM_E_INVALID_ARG;
        return return_err(err, &e);
    }

    mongo::BSONObj docObj;
    if (document && document->ptr && document->len > 0) {
        if (!validate_bson(document->ptr, document->len, &e)) {
            return return_err(err, &e);
        }
        docObj = mongo::BSONObj(reinterpret_cast<const char*>(document->ptr));
    }

    bool result = false;
    auto rc = static_cast<int64_t>(mongo::mozjs::wasm::g_engine.invokePredicate(
        static_cast<uint64_t>(handle), std::move(docObj), &result, &e));

    if (rc == mongo::mozjs::wasm::SM_OK) {
        if (ret)
            *ret = result;
        return true;
    }
    return return_err(err, &e);
}

extern "C" bool exports_mongo_mozjs_mozjs_invoke_map(
    exports_mongo_mozjs_mozjs_function_handle_t handle,
    api_list_u8_t* document,
    exports_mongo_mozjs_mozjs_ok_t* ret,
    exports_mongo_mozjs_mozjs_wasm_mozjs_error_t* err) {
    mongo::mozjs::wasm::wasm_mozjs_error_t e{};

    if (handle == 0) {
        e.code = mongo::mozjs::wasm::SM_E_INVALID_ARG;
        return return_err(err, &e);
    }

    mongo::BSONObj docObj;
    if (document && document->ptr && document->len > 0) {
        if (!validate_bson(document->ptr, document->len, &e)) {
            return return_err(err, &e);
        }
        docObj = mongo::BSONObj(reinterpret_cast<const char*>(document->ptr));
    }

    auto rc = static_cast<int64_t>(mongo::mozjs::wasm::g_engine.invokeMap(
        static_cast<uint64_t>(handle), std::move(docObj), &e));

    if (rc == mongo::mozjs::wasm::SM_OK) {
        if (ret)
            *ret = 0;
        return true;
    }
    return return_err(err, &e);
}

extern "C" bool exports_mongo_mozjs_mozjs_drain_emit_buffer(
    api_list_u8_t* ret, exports_mongo_mozjs_mozjs_wasm_mozjs_error_t* err) {
    mongo::mozjs::wasm::wasm_mozjs_error_t e{};
    mongo::BSONObj out;

    auto rc = static_cast<int64_t>(mongo::mozjs::wasm::g_engine.drainEmitBuffer(&out, &e));
    if (rc != mongo::mozjs::wasm::SM_OK) {
        return return_err(err, &e);
    }

    if (!list_u8_dup(ret, reinterpret_cast<const uint8_t*>(out.objdata()), out.objsize())) {
        e.code = mongo::mozjs::wasm::SM_E_NOMEM;
        mongo::mozjs::wasm::set_string(&e.msg, &e.msg_len, "OOM: failed to allocate emit buffer");
        return return_err(err, &e);
    }
    return true;
}

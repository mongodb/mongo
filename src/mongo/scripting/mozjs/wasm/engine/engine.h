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

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/scripting/mozjs/common/error.h"
#include "mongo/scripting/mozjs/common/global.h"
#include "mongo/scripting/mozjs/common/internedstring.h"
#include "mongo/scripting/mozjs/common/runtime.h"
#include "mongo/scripting/mozjs/common/types/bindata.h"
#include "mongo/scripting/mozjs/common/types/bson.h"
#include "mongo/scripting/mozjs/common/types/code.h"
#include "mongo/scripting/mozjs/common/types/dbpointer.h"
#include "mongo/scripting/mozjs/common/types/dbref.h"
#include "mongo/scripting/mozjs/common/types/maxkey.h"
#include "mongo/scripting/mozjs/common/types/minkey.h"
#include "mongo/scripting/mozjs/common/types/nativefunction.h"
#include "mongo/scripting/mozjs/common/types/numberdecimal.h"
#include "mongo/scripting/mozjs/common/types/numberint.h"
#include "mongo/scripting/mozjs/common/types/numberlong.h"
#include "mongo/scripting/mozjs/common/types/oid.h"
#include "mongo/scripting/mozjs/common/types/regexp.h"
#include "mongo/scripting/mozjs/common/types/status.h"
#include "mongo/scripting/mozjs/common/types/timestamp.h"
#include "mongo/scripting/mozjs/common/wraptype.h"
#include "mongo/scripting/mozjs/shared/mozjs_wasm_startup_options.h"
#include "mongo/util/assert_util.h"

#include <cstdint>
#include <memory>
#include <vector>

#include "error.h"

#include "js/RootingAPI.h"
#include "js/TypeDecls.h"

struct JSContext;
struct JSRuntime;

namespace mongo {
namespace mozjs {
namespace wasm {

extern uint32_t g_wasmJsHeapLimitMB;

class ExecutionCheck;

struct FunctionSlot {
    JS::PersistentRootedObject fn;
    explicit FunctionSlot(JSContext* cx) : fn(cx) {}
};

class MozJSPrototypeInstaller {
public:
    explicit MozJSPrototypeInstaller(JSContext* context)
        : _cx(context),
          _globalProto(_cx),
          _binDataProto(_cx),
          _bsonProto(_cx),
          _codeProto(_cx),
          _dbPointerProto(_cx),
          _dbRefProto(_cx),
          _errorProto(_cx),
          _maxKeyProto(_cx),
          _minKeyProto(_cx),
          _nativeFunctionProto(_cx),
          _numberDecimalProto(_cx),
          _numberIntProto(_cx),
          _numberLongProto(_cx),
          _oidProto(_cx),
          _regExpProto(_cx),
          _statusProto(_cx),
          _timestampProto(_cx) {
        invariant(_cx);
    }

    WrapType<GlobalInfo>& globalProto() {
        return _globalProto;
    }
    WrapType<BinDataInfo>& binDataProto() {
        return _binDataProto;
    }
    WrapType<BSONInfo>& bsonProto() {
        return _bsonProto;
    }
    WrapType<CodeInfo>& codeProto() {
        return _codeProto;
    }
    WrapType<DBPointerInfo>& dbPointerProto() {
        return _dbPointerProto;
    }
    WrapType<DBRefInfo>& dbRefProto() {
        return _dbRefProto;
    }
    WrapType<ErrorInfo>& errorProto() {
        return _errorProto;
    }
    WrapType<MaxKeyInfo>& maxKeyProto() {
        return _maxKeyProto;
    }
    WrapType<MinKeyInfo>& minKeyProto() {
        return _minKeyProto;
    }
    WrapType<NativeFunctionInfo>& nativeFunctionProto() {
        return _nativeFunctionProto;
    }
    WrapType<NumberDecimalInfo>& numberDecimalProto() {
        return _numberDecimalProto;
    }
    WrapType<NumberIntInfo>& numberIntProto() {
        return _numberIntProto;
    }
    WrapType<NumberLongInfo>& numberLongProto() {
        return _numberLongProto;
    }
    WrapType<OIDInfo>& oidProto() {
        return _oidProto;
    }
    WrapType<RegExpInfo>& regExpProto() {
        return _regExpProto;
    }
    WrapType<MongoStatusInfo>& statusProto() {
        return _statusProto;
    }
    WrapType<TimestampInfo>& timestampProto() {
        return _timestampProto;
    }


    void installTypes(JS::HandleObject global) {
        // GlobalInfo cannot use install() because JSCLASS_GLOBAL_FLAGS
        // prevents JS_InitClass on an existing global. Install its
        // freeFunctions (print, gc, sleep, etc.) directly.
        JS_DefineFunctions(_cx, global, GlobalInfo::freeFunctions);
        _binDataProto.install(global);
        _bsonProto.install(global);
        _codeProto.install(global);
        _dbPointerProto.install(global);
        _dbRefProto.install(global);
        _errorProto.install(global);
        _maxKeyProto.install(global);
        _minKeyProto.install(global);
        _nativeFunctionProto.install(global);
        _numberDecimalProto.install(global);
        _numberIntProto.install(global);
        _numberLongProto.install(global);
        _oidProto.install(global);
        _regExpProto.install(global);
        _timestampProto.install(global);
        _statusProto.install(global);
    }

private:
    JSContext* _cx = nullptr;
    WrapType<GlobalInfo> _globalProto;
    WrapType<BinDataInfo> _binDataProto;
    WrapType<BSONInfo> _bsonProto;
    WrapType<CodeInfo> _codeProto;
    WrapType<DBPointerInfo> _dbPointerProto;
    WrapType<DBRefInfo> _dbRefProto;
    WrapType<ErrorInfo> _errorProto;
    WrapType<MaxKeyInfo> _maxKeyProto;
    WrapType<MinKeyInfo> _minKeyProto;
    WrapType<NativeFunctionInfo> _nativeFunctionProto;
    WrapType<NumberDecimalInfo> _numberDecimalProto;
    WrapType<NumberIntInfo> _numberIntProto;
    WrapType<NumberLongInfo> _numberLongProto;
    WrapType<OIDInfo> _oidProto;
    WrapType<RegExpInfo> _regExpProto;
    WrapType<MongoStatusInfo> _statusProto;
    WrapType<TimestampInfo> _timestampProto;
};

/**
 * The SpiderMonkey JavaScript engine, compiled into the WASM module (mozjs_wasm_api.wasm). Runs
 * inside the Wasmtime sandbox on mongod. Exposes a C ABI (see wasm/engine/api.cpp and the
 * WIT-generated bindings) that the host-side WasmtimeScriptEngine (see wasm/wasmtime_engine.h)
 * calls through the bridge to compile and invoke JavaScript functions, get/set globals, and handle
 * map/reduce emit.
 *
 * This class runs entirely inside the WASM module. For the counterpart that runs on the host, see
 * wasm/wasmtime_engine.h.
 */
class MozJSScriptEngine : private MozJSCommonRuntimeInterface {
public:
    MozJSScriptEngine() = default;
    ~MozJSScriptEngine();

    MozJSScriptEngine(const MozJSScriptEngine&) = delete;
    MozJSScriptEngine& operator=(const MozJSScriptEngine&) = delete;

    err_code_t init(const wasm_mozjs_startup_options_t* opt, wasm_mozjs_error_t* err);
    err_code_t shutdown(wasm_mozjs_error_t* err);
    err_code_t interrupt(wasm_mozjs_error_t* err);
    err_code_t createFunction(const uint8_t* src,
                              size_t len,
                              uint64_t* out_handle,
                              wasm_mozjs_error_t* err);
    err_code_t invokeFunction(uint64_t handle,
                              mongo::BSONObj&& bsonArgs,
                              mongo::BSONObj* outBson,
                              wasm_mozjs_error_t* err);

    /// Invoke a predicate: document becomes `this` e.g `this.someVar`, returns bool.
    err_code_t invokePredicate(uint64_t handle,
                               mongo::BSONObj&& document,
                               bool* outResult,
                               wasm_mozjs_error_t* err);

    /// Invoke a map function: document becomes `this`, emits buffered.
    err_code_t invokeMap(uint64_t handle, mongo::BSONObj&& document, wasm_mozjs_error_t* err);
    err_code_t getReturnValueBson(mongo::BSONObj* out, wasm_mozjs_error_t* err);

    /// Set a named global variable from a BSON-encoded value.
    err_code_t setGlobal(const char* name,
                         size_t name_len,
                         const mongo::BSONObj& value,
                         wasm_mozjs_error_t* err);

    /// Get a named global variable as BSON-encoded bytes.
    err_code_t getGlobal(const char* name,
                         size_t name_len,
                         mongo::BSONObj* out,
                         wasm_mozjs_error_t* err);

    /// Set a named global to a single BSON element's JS value directly.
    err_code_t setGlobalValue(const char* name,
                              size_t name_len,
                              const mongo::BSONObj& singleElementDoc,
                              wasm_mozjs_error_t* err);

    /// Set up the emit() built-in for mapReduce. Resets the emit buffer.
    err_code_t setupEmit(int64_t byteLimit, bool hasByteLimit, wasm_mozjs_error_t* err);

    /// Drain the emit buffer: returns accumulated {k,v} pairs, then clears.
    err_code_t drainEmitBuffer(mongo::BSONObj* out, wasm_mozjs_error_t* err);

    // MozJSCommonRuntimeInterface implementation
    void gc() override;
    void sleep(Milliseconds ms) override;
    std::size_t getGeneration() const override;
    JS::HandleId getInternedStringId(InternedString name) override;
    std::int64_t* trackedNewInt64(std::int64_t value) override;
    WrapType<NumberLongInfo>& numberLongProto() override;
    WrapType<NumberIntInfo>& numberIntProto() override;
    WrapType<NumberDecimalInfo>& numberDecimalProto() override;
    WrapType<OIDInfo>& oidProto() override;
    WrapType<BinDataInfo>& binDataProto() override;
    WrapType<TimestampInfo>& timestampProto() override;
    WrapType<MaxKeyInfo>& maxKeyProto() override;
    WrapType<MinKeyInfo>& minKeyProto() override;
    WrapType<CodeInfo>& codeProto() override;
    WrapType<DBPointerInfo>& dbPointerProto() override;
    WrapType<NativeFunctionInfo>& nativeFunctionProto() override;
    WrapType<ErrorInfo>& errorProto() override;
    WrapType<MongoStatusInfo>& mongoStatusProto() override;
    WrapType<BSONInfo>& bsonProto() override;
    WrapType<DBRefInfo>& dbRefProto() override;
    WrapType<RegExpInfo>& regExpProto() override;

    void setStatus(Status status) override;
    bool isJavaScriptProtectionEnabled() const override;
    void newFunction(StringData code, JS::MutableHandleValue out) override;
    bool requiresOwnedObjects() const override;
    void trackNewPointer(void* ptr) override;
    void trackDeletePointer(void* ptr) override;

private:
    FunctionSlot* resolveHandle(uint64_t handle, wasm_mozjs_error_t* err);

    // Call __parseJSFunctionOrExpression (installed during init) with `raw` and write the
    // properly-wrapped function source into `*out`.  Must be called inside a JSAutoRealm.
    // Returns false and populates `err` (if non-null) on failure; a JS exception is left pending.
    bool _parseFunctionSource(StringData raw, std::string* out, wasm_mozjs_error_t* err);

    bool _initialized = false;

    JSContext* _cx = nullptr;
    JSRuntime* _rt = nullptr;
    JS::PersistentRootedObject _global;

    std::vector<FunctionSlot> _slots;
    std::unique_ptr<MozJSPrototypeInstaller> _prototypeInstaller;
    std::unique_ptr<InternedStringTable> _internedStrings;

    Status _status = Status::OK();

    std::vector<mongo::BSONObj> _emitBuffer;
    int64_t _emitBytesUsed = 0;
    int64_t _emitByteLimit = 16 * 1024 * 1024;  // default 16 MB

    static mongo::BSONObj _emitCallback(const mongo::BSONObj& args, void* data);
};

}  // namespace wasm
}  // namespace mozjs
}  // namespace mongo

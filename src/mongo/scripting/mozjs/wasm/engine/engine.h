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
#include "mongo/scripting/mozjs/common/types/object.h"
#include "mongo/scripting/mozjs/common/types/oid.h"
#include "mongo/scripting/mozjs/common/types/regexp.h"
#include "mongo/scripting/mozjs/common/types/status.h"
#include "mongo/scripting/mozjs/common/types/timestamp.h"
#include "mongo/scripting/mozjs/common/wraptype.h"
#include "mongo/scripting/mozjs/shared/mozjs_wasm_startup_options.h"
#include "mongo/util/assert_util.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
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
    JS::PersistentRootedValue fn;
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
          _objectProto(_cx),
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
    WrapType<ObjectInfo>& objectProto() {
        return _objectProto;
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
        _objectProto.install(global);
        _regExpProto.install(global);
        _timestampProto.install(global);
        _statusProto.install(global);
    }

    /**
     * Unroots every prototype/constructor while the JSContext is still alive, so this
     * installer (and the JSClasses its WrapTypes own) can be destroyed only AFTER
     * JS_DestroyContext has run the shutdown-GC finalizers. See MozJSScriptEngine::shutdown.
     */
    void dropRoots() {
        _globalProto.dropRoots();
        _binDataProto.dropRoots();
        _bsonProto.dropRoots();
        _codeProto.dropRoots();
        _dbPointerProto.dropRoots();
        _dbRefProto.dropRoots();
        _errorProto.dropRoots();
        _maxKeyProto.dropRoots();
        _minKeyProto.dropRoots();
        _nativeFunctionProto.dropRoots();
        _numberDecimalProto.dropRoots();
        _numberIntProto.dropRoots();
        _numberLongProto.dropRoots();
        _oidProto.dropRoots();
        _objectProto.dropRoots();
        _regExpProto.dropRoots();
        _timestampProto.dropRoots();
        _statusProto.dropRoots();
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
    WrapType<ObjectInfo> _objectProto;
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
    ~MozJSScriptEngine() override;

    MozJSScriptEngine(const MozJSScriptEngine&) = delete;
    MozJSScriptEngine& operator=(const MozJSScriptEngine&) = delete;

    err_code_t init(const wasm_mozjs_startup_options_t* opt, wasm_mozjs_error_t* err);
    err_code_t shutdown(wasm_mozjs_error_t* err);
    err_code_t interrupt(wasm_mozjs_error_t* err);
    bool exec(std::string_view code, const std::string& name);

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

    /// Reset JS state without destroying the Store or JSContext.
    /// Clears user-defined globals and emit buffer; preserves compiled function handles.
    err_code_t reset(wasm_mozjs_error_t* err);

    /// Create a fresh Realm (new global) on the existing JSContext, re-running
    /// InitRealmStandardClasses + type install + freeze but NOT InitSelfHostedCode.
    /// All cached function handles become invalid; g_function_count must be reset by caller.
    err_code_t resetRealm(wasm_mozjs_error_t* err);

    /// Set up the emit() built-in for mapReduce. Resets the emit buffer.
    err_code_t setupEmit(int64_t byteLimit, bool hasByteLimit, wasm_mozjs_error_t* err);

    /// Drain the emit buffer: returns accumulated {k,v} pairs, then clears.
    err_code_t drainEmitBuffer(mongo::BSONObj* out, wasm_mozjs_error_t* err);

    /// Diagnostic memory statistics as BSON:
    /// {linearMemoryBytes: long, gcHeapBytes: long, gcNumber: long}.
    /// linearMemoryBytes is the real WASM linear memory size (memory.size), which only
    /// ever grows; gcHeapBytes is the GC-managed portion bounded by the JS heap limit.
    err_code_t getMemoryStats(mongo::BSONObj* out, wasm_mozjs_error_t* err);

    void injectNative(const char* field, NativeFunction func, void* data = nullptr);

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
    void newFunction(std::string_view code, JS::MutableHandleValue out) override;
    bool requiresOwnedObjects() const override;
    void trackNewPointer(void* ptr) override;
    void trackDeletePointer(void* ptr) override;

private:
    FunctionSlot* resolveHandle(uint64_t handle, wasm_mozjs_error_t* err);

    // Call __parseJSFunctionOrExpression (installed during init) with `raw` and write the
    // properly-wrapped function source into `*out`.  Must be called inside a JSAutoRealm.
    // Returns false and populates `err` (if non-null) on failure; a JS exception is left pending.
    bool _parseFunctionSource(std::string_view raw, std::string* out, wasm_mozjs_error_t* err);

    // Create a new global object, enter its Realm, run InitRealmStandardClasses, install
    // MongoDB types, install Array helpers, snapshot init names/props, run freeze script.
    // On success _global is updated to the new global.  Caller must hold no JSAutoRealm.
    // Returns SM_OK on success; on failure the context is left in an uninitialized state
    // and the caller should propagate the error.
    err_code_t _setupNewGlobal(ExecutionCheck& chk, wasm_mozjs_error_t* err);

    // Create a lightweight child realm in the same compartment as _parentGlobal.
    // Cheaper than _setupNewGlobal: skips property snapshot, freeze script, and parse
    // helper install (parse runs in the parent realm; the helper is copied here).
    // On success _global is updated to the new child global.
    // Caller must hold no JSAutoRealm and _parentGlobal must be initialized.
    err_code_t _setupChildRealm(ExecutionCheck& chk, wasm_mozjs_error_t* err);

    bool _initialized = false;
    bool _javascriptProtection = false;

    // JS_Init() / JS_ShutDown() are once-per-runtime-lifetime calls. After the first
    // init(), subsequent shutdown()+init() cycles must skip them and only do
    // JS_DestroyContext / JS_NewContext so the SM runtime is not repeatedly torn down.
    bool _smRuntimeInitialized = false;

    JSContext* _cx = nullptr;
    JSRuntime* _rt = nullptr;

    // Parent realm: created once in init(), holds frozen built-ins, MongoDB types, Array
    // helpers, and the parse helper.  Never reset; lives for the lifetime of the JSContext.
    JS::PersistentRootedObject _parentGlobal;

    // Child realm: lives in the same compartment as _parentGlobal (no CCW overhead).
    // Holds user functions; replaced cheaply on resetRealm() without re-running
    // InitSelfHostedCode or the freeze/snapshot scripts.
    JS::PersistentRootedObject _global;

    std::vector<FunctionSlot> _slots;
    std::unique_ptr<MozJSPrototypeInstaller> _prototypeInstaller;
    std::unique_ptr<InternedStringTable> _internedStrings;

    Status _status = Status::OK();

    std::vector<mongo::BSONObj> _emitBuffer;
    int64_t _emitBytesUsed = 0;
    int64_t _emitByteLimit = 16 * 1024 * 1024;  // default 16 MB

    // Host-supplied BSON bytes pinned by live BSONHolder proxies since the last GC.
    // ValueReader wraps argument/global BSON in lazy proxies whose holders keep the
    // owned buffer alive until the proxy is finalized — which only happens at GC.
    // SpiderMonkey never sees these malloc bytes (no AddAssociatedMemory accounting),
    // so without help the GC feels no pressure and dead proxies pin their buffers
    // indefinitely. In WASM that pinned memory can never be returned to the OS (linear
    // memory only grows), so we count it ourselves and force a GC at the thresholds
    // below. See _notePinnedHostBytes().
    int64_t _pinnedHostBytesSinceGc = 0;

    // Mid-request bound: force a GC once this many argument bytes have been pinned.
    // ~32 MB keeps the worst-case dead-proxy backlog under 3% of the 1210 MB store cap
    // while amortising the ~1 ms full-GC cost over ~100 large-document invocations.
    static constexpr int64_t kPinnedBytesGcThreshold = 32 * 1024 * 1024;

    // Request-boundary bound: reset() skips its GC entirely for cheap scopes but runs
    // one when at least this much pinned garbage may exist, so parked (reused) bridges
    // return to a clean floor between requests.
    static constexpr int64_t kPinnedBytesResetGcThreshold = 1024 * 1024;

    // Adds nbytes to the pinned counter and runs a full GC at kPinnedBytesGcThreshold.
    void _notePinnedHostBytes(int64_t nbytes);

    // Snapshot of own property names on globalThis after init() completes.
    // These are engine-installed names that must survive reset().
    std::unordered_set<std::string> _initGlobalNames;

    // For each init-time function-valued globalThis property, the set of own property
    // names that function had at init time. reset() uses this to scrub only user-added
    // properties, leaving engine-installed ones (e.g. Object.keys, Array.from) intact.
    std::unordered_map<std::string, std::unordered_set<std::string>> _initFnProps;

    static mongo::BSONObj _emitCallback(const mongo::BSONObj& args, void* data);
};

}  // namespace wasm
}  // namespace mozjs
}  // namespace mongo

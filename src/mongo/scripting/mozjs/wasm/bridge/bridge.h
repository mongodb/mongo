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

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/platform/atomic.h"
#include "mongo/scripting/mozjs/wasm/bridge/wasm_helpers.h"

#include <string_view>

namespace mongo::mozjs::wasm {

struct WasmBridgeStats {
    WasmBridgeStats() = default;
    WasmBridgeStats(const WasmBridgeStats&) = delete;
    WasmBridgeStats& operator=(const WasmBridgeStats&) = delete;

    Atomic<uint64_t> setupEmitCallCount{0};
    Atomic<uint64_t> invokeFunctionCallCount{0};
    Atomic<uint64_t> invokeMapCallCount{0};
    Atomic<uint64_t> invokePredicateCallCount{0};
    Atomic<uint64_t> resetEngineCallCount{0};
    Atomic<uint64_t> resetRealmCallCount{0};
    Atomic<uint64_t> createFunctionCallCount{0};
    Atomic<uint64_t> drainEmitBufferCallCount{0};
    Atomic<uint64_t> setGlobalCallCount{0};
    Atomic<uint64_t> bytesInToWasm{0};
    Atomic<uint64_t> bytesOutFromWasm{0};
    Atomic<uint64_t> maxSingleCallBytesIn{0};
    Atomic<uint64_t> maxSingleCallBytesOut{0};
    Atomic<int64_t> maxEmitByteLimit{0};
    Atomic<uint64_t> killCount{0};
    // Wallclock time the bridge was instantiated (nanos since process start). The
    // trap warning subtracts to give an "age" — long-running bridges are more likely
    // to have accumulated SM-side malloc / JIT pressure.
    int64_t createdAtMonoNanos{0};

    BSONObj toBSON() const {
        BSONObjBuilder b;
        b.append("setupEmitCalls", static_cast<long long>(setupEmitCallCount.load()));
        b.append("invokeFunctionCalls", static_cast<long long>(invokeFunctionCallCount.load()));
        b.append("invokeMapCalls", static_cast<long long>(invokeMapCallCount.load()));
        b.append("invokePredicateCalls", static_cast<long long>(invokePredicateCallCount.load()));
        b.append("createFunctionCalls", static_cast<long long>(createFunctionCallCount.load()));
        b.append("drainEmitBufferCalls", static_cast<long long>(drainEmitBufferCallCount.load()));
        b.append("setGlobalCalls", static_cast<long long>(setGlobalCallCount.load()));
        b.append("resetEngineCalls", static_cast<long long>(resetEngineCallCount.load()));
        b.append("resetRealmCalls", static_cast<long long>(resetRealmCallCount.load()));
        b.append("bytesInToWasm", static_cast<long long>(bytesInToWasm.load()));
        b.append("bytesOutFromWasm", static_cast<long long>(bytesOutFromWasm.load()));
        b.append("maxSingleCallBytesIn", static_cast<long long>(maxSingleCallBytesIn.load()));
        b.append("maxSingleCallBytesOut", static_cast<long long>(maxSingleCallBytesOut.load()));
        b.append("maxEmitByteLimit", static_cast<long long>(maxEmitByteLimit.load()));
        b.append("killCount", static_cast<long long>(killCount.load()));
        return b.obj();
    }
};

struct WasmEngineContext {
    WasmEngineContext(const WasmEngineContext&) = delete;
    WasmEngineContext& operator=(const WasmEngineContext&) = delete;
    WasmEngineContext(WasmEngineContext&&) = delete;
    WasmEngineContext& operator=(WasmEngineContext&&) = delete;

    static std::shared_ptr<WasmEngineContext> createFromPrecompiled(const uint8_t* data,
                                                                    size_t size);

    // Destroys the wasmtime members under wasmLifecycleMutex() (see bridge.cpp).
    ~WasmEngineContext();

private:
    WasmEngineContext(wt::Engine engine, wc::Component component, wc::Linker linker)
        : _engine(std::move(engine)),
          _component(std::move(component)),
          _linker(std::move(linker)) {}

    friend class MozJSWasmBridge;
    // optional so the destructor can release them explicitly while holding the lifecycle lock.
    boost::optional<wt::Engine> _engine;
    boost::optional<wc::Component> _component;
    // Linker is constructed once with add_wasip2() so each bridge instantiation
    // can skip the per-call WASIP2 registration cost.
    boost::optional<wc::Linker> _linker;
};

// Wasmtime trap error code used by MozJSWasmBridge::_callFunc / _callFuncNoArgs.
constexpr int kWasmtimeTrapErrorCode = 11542340;

class MozJSWasmBridge {
public:
    MozJSWasmBridge() = delete;
    MozJSWasmBridge(MozJSWasmBridge&&) = delete;
    MozJSWasmBridge(const MozJSWasmBridge&) = delete;
    MozJSWasmBridge& operator=(MozJSWasmBridge&&) = delete;
    MozJSWasmBridge& operator=(const MozJSWasmBridge&) = delete;

    enum class State {
        Uninitialized,
        Initialized,
        OOM,
        Trapped,
    };

    struct Options {
        uint32_t jsHeapLimitMB = 0;
        uint32_t linearMemoryLimitMB = 0;
        bool javascriptProtection = false;
    };

    explicit MozJSWasmBridge(std::shared_ptr<WasmEngineContext> ctx, Options opts);

    // Tears down the Store/Instance under wasmLifecycleMutex() (see bridge.cpp).
    ~MozJSWasmBridge();

    bool initialize();
    void shutdown();

    // Signal that execution should be interrupted via Wasmtime epoch increment.
    // Safe to call from any thread. kill() provides a DeadlineMonitor-compatible interface.
    void kill();
    bool isKillPending() const;

    uint64_t createFunction(std::string_view source);

    StatusWith<BSONObj> invokeFunction(uint64_t handle, wc::Val bsonVal, bool ignoreReturn = false);
    bool invokePredicate(uint64_t handle, wc::Val bsonVal);
    void invokeMap(uint64_t handle, wc::Val bsonVal);

    void setGlobal(std::string_view name, const BSONObj& value);
    BSONObj getGlobal(std::string_view name, bool implicitNull = false);

    void setGlobalValue(std::string_view name, const BSONObj& value);
    BSONObj drainEmitBuffer();

    // Diagnostic memory statistics from inside the WASM instance:
    // {linearMemoryBytes: long, gcHeapBytes: long, gcNumber: long}.
    // linearMemoryBytes is the real linear-memory size (memory.size) counted against
    // the store limit; gcHeapBytes is the GC-managed portion bounded by jsHeapLimitMB.
    BSONObj getMemoryStats();
    void setupEmit(boost::optional<int64_t> byteLimit);

    // Reset JS state (clears user globals, function handles, emit buffer, runs GC)
    // without destroying the Store or JSContext. Much cheaper than shutdown+initialize.
    void resetEngine();

    // Create a fresh SpiderMonkey Realm (new global object) on the existing JSContext.
    // InitSelfHostedCode is NOT re-run; only InitRealmStandardClasses runs for the new
    // global. All cached function handles become invalid after this call.
    // Slower than resetEngine() (~InitRealmStandardClasses cost) but provides complete
    // constructor-level isolation: Array, Object, etc. are brand-new each call.
    void resetRealm();

    bool isInitialized() const {
        return _state.load() == State::Initialized;
    }

    // True after a wasmtime trap or a fatal WIT error (e-oom, e-internal).
    // The store is unusable once trapped and the caller should discard the bridge.
    bool hasTrapped() const {
        return _state.load() == State::Trapped;
    }

    // True when the JS heap ran out of memory (e.g. allocation-size-overflow).
    // For fatal OOM at the wasmtime store level (e-oom), hasTrapped() is true instead.
    bool hasOomError() const {
        return _state.load() == State::OOM;
    }

    // True iff the bridge is ready to accept new requests: initialized, not trapped,
    // not OOM, and no kill pending.
    bool isHealthy() const {
        return isInitialized() && !hasTrapped() && !hasOomError() && !isKillPending();
    }

    State getState() const {
        return _state.load();
    }

    // Returns the last JS function return value as {"__returnValue": val}, preserving array types.
    BSONObj getReturnValueWrapped();

    uint32_t getHeapLimitMB() const {
        return _jsHeapLimitMB;
    }
    uint32_t getStoreLimitMB() const {
        return _storeLimitMB;
    }

    // True iff setupEmit() has been called at least once on this bridge. Each setupEmit
    // allocates ~117 MB of WASM linear memory that can never be released for the lifetime
    // of the bridge (WASM linear memory only grows). If
    // the bridge is parked and reused for another MapReduce request, that second
    // setupEmit allocates a fresh ~117 MB on top of the now-orphaned old buffer,
    // and the third allocates more, etc. — quickly exhausting the 1210 MB store
    // cap and tripping the "cannot leave component instance" trap. Scopes use
    // this signal at teardown to refuse to park MR-heavy bridges.
    bool wasEmitConfigured() const {
        return _stats.setupEmitCallCount.load() > 0;
    }

private:
    // Resets the store's epoch deadline to current_epoch+1.  Called at the top of resetEngine()
    // and resetRealm(): if other threads triggered kills while this bridge was parked, the engine
    // epoch advanced past this store's old deadline and the next WASM call would trap immediately.
    void _refreshEpochDeadline() {
        _store->context().set_epoch_deadline(1);
    }

    BSONObj _getReturnValueBson();
    BSONObj _extractBSON(const wc::Val& result);
    wc::Func _getFunc(std::string_view funcName);

    // Asserts that the bridge is in a usable state (Initialized). Throws a
    // user-visible error if the engine has trapped, OOM'd, or was never initialized.
    void _assertUsable();

    // Triggers an epoch increment to interrupt WASM execution.
    void _signalInterrupt();

    // Inspects the WIT result and handles all error cases:
    //   - mongo C++ exception (mozJSErrorCode != JSInterpreterFailure): overrides code, throws
    //   - fatal WIT error (e-oom, e-internal): latches Trapped
    //   - non-fatal OOM (e-runtime + OOM msg): latches OOM
    // If errorPrefix is non-empty, uasserts with errorPrefix+translateMozJSError for any failure.
    // If errorPrefix is empty, returns false on failure so the caller can decide (e.g. log).
    bool _assertWitResult(const wc::Val& result,
                          std::string errorPrefix = {},
                          ErrorCodes::Error code = ErrorCodes::JSInterpreterFailure);

    // Calls a WASM function with the given arguments. Latches _state and
    // uasserts on wasmtime traps so callers don't need explicit trap handling.
    template <typename... Args>
    bool _callFunc(wc::Func& func, wc::Val* results, size_t numResults, Args&&... args) {
        std::array<wc::Val, sizeof...(Args)> argsArr = {std::forward<Args>(args)...};
        wt::Span<const wc::Val> argsSpan(argsArr.data(), argsArr.size());
        wt::Span<wc::Val> resultsSpan(results, numResults);
        wt::Result<std::monostate> callResult = func.call(getContext(), argsSpan, resultsSpan);
        if (!callResult) {
            _throwAfterTrap(callResult.err().message());
        }
        auto postResult = func.post_return(getContext());
        return static_cast<bool>(postResult);
    }

    // Calls a WASM function with no arguments. Same trap-latching as _callFunc.
    bool _callFuncNoArgs(wc::Func& func, wc::Val* results, size_t numResults);

    // Latches State::Trapped and throws a DBException whose ErrorCode reflects the
    // real reason execution stopped. When isKillPending() is true (the bridge was
    // killed via kill()), this resolves the current OperationContext's actual kill
    // status — e.g. MaxTimeMSExpired (50), CursorKilled (202), Interrupted (11601)
    // — so callers see the same mongo error code they would have seen on the
    // native MozJS engine, instead of a generic "wasm trap: interrupt" string.
    // Falls back to Interrupted when no opCtx is available (e.g. shell). For
    // traps without a pending kill, throws kWasmtimeTrapErrorCode.
    // [[noreturn]] is asserted via uasserted/iassert; this function always throws.
    [[noreturn]] void _throwAfterTrap(const std::string& trapMessage);

    inline wt::Store::Context getContext() {
        return _store->context();
    }

    Atomic<State> _state{State::Uninitialized};
    Atomic<bool> _killPending{false};
    uint32_t _jsHeapLimitMB{0};
    uint32_t _storeLimitMB{0};
    bool _javascriptProtection{false};

    // Per-bridge diagnostic counters; dumped in the CannotLeaveComponent trap warning (11542382).
    WasmBridgeStats _stats;

    // The engine and compiled component are shared across bridge instances.
    // Each bridge owns its own _store and _instance for execution isolation.
    std::shared_ptr<WasmEngineContext> _ctx;

    boost::optional<wt::Store> _store;
    boost::optional<wc::Instance> _instance;

    // WIT function handles cached at construction to avoid per-call string lookups.
    // std::optional because wc::Func has no default constructor.
    boost::optional<wc::Func> _initEngineFunc = boost::none;
    boost::optional<wc::Func> _shutdownEngineFunc = boost::none;
    boost::optional<wc::Func> _resetEngineFunc = boost::none;
    boost::optional<wc::Func> _resetRealmFunc = boost::none;
    boost::optional<wc::Func> _createFunctionFunc = boost::none;
    boost::optional<wc::Func> _invokeFunctionFunc = boost::none;
    boost::optional<wc::Func> _invokePredicateFunc = boost::none;
    boost::optional<wc::Func> _setGlobalFunc = boost::none;
    boost::optional<wc::Func> _setGlobalValueFunc = boost::none;
    boost::optional<wc::Func> _setupEmitFunc = boost::none;
    boost::optional<wc::Func> _invokeMapFunc = boost::none;
    boost::optional<wc::Func> _drainEmitBufferFunc = boost::none;
    boost::optional<wc::Func> _getMemoryStatsFunc = boost::none;
    boost::optional<wc::Func> _getGlobalFunc = boost::none;
    boost::optional<wc::Func> _getReturnValueBsonFunc = boost::none;
};

}  // namespace mongo::mozjs::wasm

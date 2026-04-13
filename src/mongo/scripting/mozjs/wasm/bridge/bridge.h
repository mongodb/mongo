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

#include "mongo/platform/atomic.h"
#include "mongo/scripting/mozjs/wasm/bridge/wasm_helpers.h"

namespace mongo::mozjs::wasm {


struct WasmEngineContext {
    WasmEngineContext(const WasmEngineContext&) = delete;
    WasmEngineContext& operator=(const WasmEngineContext&) = delete;
    WasmEngineContext(WasmEngineContext&&) = delete;
    WasmEngineContext& operator=(WasmEngineContext&&) = delete;

    static std::shared_ptr<WasmEngineContext> createFromPrecompiled(const uint8_t* data,
                                                                    size_t size);

private:
    WasmEngineContext(wt::Engine engine, wc::Component component)
        : _engine(std::move(engine)), _component(std::move(component)) {}

    friend class MozJSWasmBridge;
    wt::Engine _engine;
    wc::Component _component;
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
    };

    explicit MozJSWasmBridge(std::shared_ptr<WasmEngineContext> ctx, Options opts);

    bool initialize();
    void shutdown();

    // Signal that execution should be interrupted via Wasmtime epoch increment.
    // Safe to call from any thread. kill() provides a DeadlineMonitor-compatible interface.
    void kill();
    bool isKillPending() const;

    uint64_t createFunction(std::string_view source);
    StatusWith<BSONObj> invokeFunction(uint64_t handle,
                                       const BSONObj& args,
                                       bool ignoreReturn = false);

    void setGlobal(std::string_view name, const BSONObj& value);
    BSONObj getGlobal(std::string_view name, bool implicitNull = false);

    void setGlobalValue(std::string_view name, const BSONObj& value);

    bool invokePredicate(uint64_t handle, const BSONObj& value);

    void invokeMap(uint64_t handle, const BSONObj& value);
    BSONObj drainEmitBuffer();
    void setupEmit(boost::optional<int64_t> byteLimit);

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

    State getState() const {
        return _state.load();
    }

    // Returns the last JS function return value as {"__returnValue": val}, preserving array types.
    BSONObj getReturnValueWrapped();

private:
    BSONObj _getReturnValueBson();
    BSONObj _extractBSON(const wc::Val& result);
    wc::Func _getFunc(std::string_view funcName);

    // Asserts that the bridge is in a usable state (Initialized). Throws a
    // user-visible error if the engine has trapped, OOM'd, or was never initialized.
    void _assertUsable();

    // Triggers an epoch increment to interrupt WASM execution.
    void _signalInterrupt();

    // Checks the WIT result and latches _trapped when the error code is fatal
    // (e-oom or e-internal).
    bool _isResultOk(const wc::Val& result);

    // Calls a WASM function with the given arguments. Latches _state and
    // uasserts on wasmtime traps so callers don't need explicit trap handling.
    template <typename... Args>
    bool _callFunc(wc::Func& func, wc::Val* results, size_t numResults, Args&&... args) {
        std::array<wc::Val, sizeof...(Args)> argsArr = {std::forward<Args>(args)...};
        wt::Span<const wc::Val> argsSpan(argsArr.data(), argsArr.size());
        wt::Span<wc::Val> resultsSpan(results, numResults);
        wt::Result<std::monostate> callResult = func.call(getContext(), argsSpan, resultsSpan);
        if (!callResult) {
            _state.store(State::Trapped);
            uasserted(kWasmtimeTrapErrorCode, callResult.err().message());
        }
        auto postResult = func.post_return(getContext());
        return static_cast<bool>(postResult);
    }

    // Calls a WASM function with no arguments. Same trap-latching as _callFunc.
    bool _callFuncNoArgs(wc::Func& func, wc::Val* results, size_t numResults);

    inline wt::Store::Context getContext() {
        return _store->context();
    }

    Atomic<State> _state{State::Uninitialized};
    Atomic<bool> _killPending{false};
    uint32_t _jsHeapLimitMB{0};

    // The engine and compiled component are shared across bridge instances.
    // Each bridge owns its own _store and _instance for execution isolation.
    std::shared_ptr<WasmEngineContext> _ctx;

    // This resets the storage of Global, Memory, etc.
    // It contains the interface defined functions, and any
    // state produced during execution.
    boost::optional<wt::Store> _store;

    // The actual module with all the links.
    boost::optional<wc::Instance> _instance;

    // WIT function handles cached at construction to avoid per-call string lookups.
    // std::optional because wc::Func has no default constructor.
    boost::optional<wc::Func> _initEngineFunc = boost::none;
    boost::optional<wc::Func> _shutdownEngineFunc = boost::none;
    boost::optional<wc::Func> _createFunctionFunc = boost::none;
    boost::optional<wc::Func> _invokeFunctionFunc = boost::none;
    boost::optional<wc::Func> _invokePredicateFunc = boost::none;
    boost::optional<wc::Func> _setGlobalFunc = boost::none;
    boost::optional<wc::Func> _setGlobalValueFunc = boost::none;
    boost::optional<wc::Func> _setupEmitFunc = boost::none;
    boost::optional<wc::Func> _invokeMapFunc = boost::none;
    boost::optional<wc::Func> _drainEmitBufferFunc = boost::none;
    boost::optional<wc::Func> _getGlobalFunc = boost::none;
    boost::optional<wc::Func> _getReturnValueBsonFunc = boost::none;
};

}  // namespace mongo::mozjs::wasm

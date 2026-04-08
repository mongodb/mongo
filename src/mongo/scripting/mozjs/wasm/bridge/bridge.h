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

#include "mongo/scripting/mozjs/wasm/bridge/wasm_helpers.h"

#include <atomic>

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

class MozJSWasmBridge {
public:
    MozJSWasmBridge() = delete;
    MozJSWasmBridge(MozJSWasmBridge&&) = delete;
    MozJSWasmBridge(const MozJSWasmBridge&) = delete;
    MozJSWasmBridge& operator=(MozJSWasmBridge&&) = delete;
    MozJSWasmBridge& operator=(const MozJSWasmBridge&) = delete;

    struct Options {
        uint32_t heapSizeMB;
    };

    explicit MozJSWasmBridge(std::shared_ptr<WasmEngineContext> ctx, Options opts = {});

    bool initialize(const Options& options);
    void shutdown();

    // Signal that execution should be interrupted via Wasmtime epoch increment.
    // Safe to call from any thread. kill() provides a DeadlineMonitor-compatible interface.
    void kill();
    // Triggers an epoch increment to interrupt WASM execution.
    void signalInterrupt();
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
        return _engineInitialized;
    }

    // Returns the last JS function return value as {"__returnValue": val}, preserving array types.
    BSONObj getReturnValueWrapped();

private:
    BSONObj _getReturnValueBson();
    BSONObj _extractBSON(const wc::Val& result);
    wc::Func _getFunc(std::string_view funcName);

    inline wt::Store::Context getContext() {
        return _store->context();
    }

    mongo::Atomic<bool> _killPending{false};

    bool _engineInitialized = false;

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

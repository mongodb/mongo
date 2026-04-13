/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/scripting/engine.h"
#include "mongo/scripting/mozjs/wasm/bridge/bridge.h"
#include "mongo/util/modules.h"

#include <atomic>
#include <unordered_set>

#include <boost/optional.hpp>
namespace mongo::mozjs {

class WasmtimeImplScope : public Scope {
public:
    WasmtimeImplScope(std::shared_ptr<wasm::WasmEngineContext> wasmEngineCtx,
                      boost::optional<int> jsHeapLimitMB = boost::none);

    int invoke(ScriptingFunction func,
               const BSONObj* args,
               const BSONObj* recv,
               int timeoutMs = 0,
               bool ignoreReturn = false,
               bool readOnlyArgs = false,
               bool readOnlyRecv = false) override;

    void injectNative(const char* field, NativeFunction func, void* data) override;

    // --- Scope (engine.h) overrides ---
    void reset() override;
    void init(const BSONObj* data) override;
    void registerOperation(OperationContext* opCtx) override;
    void unregisterOperation() override;
    BSONObj getObject(const char* field) override;
    std::string getString(const char* field) override;
    bool getBoolean(const char* field) override;
    double getNumber(const char* field) override;
    int getNumberInt(const char* field) override;
    long long getNumberLongLong(const char* field) override;
    Decimal128 getNumberDecimal(const char* field) override;
    OID getOID(const char* field) override;
    void getBinData(const char* field,
                    std::function<void(const BSONBinData&)> withBinData) override;
    Timestamp getTimestamp(const char* field) override;
    JSRegEx getRegEx(const char* field) override;
    void setElement(const char* field, const BSONElement& e, const BSONObj& parent) override;
    void setNumber(const char* field, double val) override;
    void setString(const char* field, StringData val) override;
    void setObject(const char* field, const BSONObj& obj, bool readOnly = true) override;
    void setBoolean(const char* field, bool val) override;
    void setFunction(const char* field, const char* code) override;
    int type(const char* field) override;
    void rename(const char* from, const char* to) override;
    std::string getError() override;
    bool hasOutOfMemoryException() override;
    void kill() override;
    bool isKillPending() const override;

    // The following methods are not meaningful for Wasmtime.
    // Wasmtime does not have BSONObj wrapped into a JS object (BSONHolder) so these methods are
    // meaningless.
    void advanceGeneration() override {}
    void requireOwnedObjects() override {}

    // These methods are mongosh-specific and not needed as part of runtime. Probably could refactor
    // to a separate interface if we wanted to implement mongosh support in Wasmtime.
    bool exec(StringData code,
              const std::string& name,
              bool printResult,
              bool reportError,
              bool assertOnError,
              int timeoutMs = 0) override {
        MONGO_UNREACHABLE
    };
    std::string getBaseURL() const override { MONGO_UNREACHABLE };
    void externalSetup() override {
        MONGO_UNREACHABLE;
    }
    void gc() override {
        MONGO_UNREACHABLE;
    }

protected:
    ScriptingFunction _createFunction(const char* code) override;

private:
    const std::shared_ptr<wasm::WasmEngineContext> _wasmEngineCtx;
    const boost::optional<int> _jsHeapLimitMB;

    std::unique_ptr<wasm::MozJSWasmBridge> _bridge;
    void _drainEmitToCallback();
    void _installHelpers();
    BSONObj _resolveGlobal(const char* field) const;
    NativeFunction _emitCallback = nullptr;
    void* _emitCallbackData = nullptr;
    OperationContext* _opCtx = nullptr;

    // Cached return value from the last invoke call.  Avoids a WASM bridge round-trip in
    // getXxx("__returnValue"): invoke() stores the result here directly instead of calling
    // setGlobal("__returnValue", ...) and the getters read from this instead of getGlobal().
    BSONObj _lastReturnValue;
};

}  // namespace mongo::mozjs

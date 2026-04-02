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

namespace mongo {
namespace mozjs {

class WasmtimeImplScope;

/**
 * Host-side JavaScript engine that runs on mongod. Implements the ScriptEngine interface by
 * loading the MozJS JavaScript runtime as a pre-compiled AOT WASM module (mozjs_wasm_api.cwasm)
 * and executing JavaScript inside the Wasmtime sandbox. Each Scope, WasmtimeImplScope, communicates
 * with the in-WASM MozJSScriptEngine (see wasm/engine/engine.h) via the C ABI bridge in
 * MozJSWasmBridge (see wasm/bridge/bridge.h).
 *
 * This class runs entirely outside the WASM module. For the counterpart that runs inside the WASM
 * module, see wasm/engine/engine.h.
 */
class WasmtimeScriptEngine final : public mongo::ScriptEngine {
public:
    WasmtimeScriptEngine();
    ~WasmtimeScriptEngine() override;

    void runTest() override {}

    bool utf8Ok() const override {
        return true;
    }

    mongo::Scope* createScope() override;
    mongo::Scope* createScopeForCurrentThread(boost::optional<int> jsHeapLimitMB) override;

    void interrupt(ClientLock&, OperationContext*) override;

    void interruptAll(ServiceContextLock&) override;

    int getJSHeapLimitMB() const override;
    void setJSHeapLimitMB(int limit) override;

    bool getJSUseLegacyMemoryTracking() const override;
    void setJSUseLegacyMemoryTracking(bool shouldUseLegacy) override;

    // These are mongosh-specific and not needed as part of runtime, so we can just make them
    // unreachable for now.
    std::string getLoadPath() const override { MONGO_UNREACHABLE };
    void setLoadPath(const std::string& loadPath) override { MONGO_UNREACHABLE };
    void enableJavaScriptProtection(bool value) override { MONGO_UNREACHABLE };
    bool isJavaScriptProtectionEnabled() const override { MONGO_UNREACHABLE };
    std::string getInterpreterVersionString() const override { MONGO_UNREACHABLE };

    // Allow impl scopes to register with their OperationContext so interrupts can find them.
    void registerOperation(OperationContext* ctx, WasmtimeImplScope* scope);
    void unregisterOperation(OperationContext* opCtx);

private:
    std::shared_ptr<wasm::WasmEngineContext> _wasmEngineCtx;
};

}  // namespace mozjs
}  // namespace mongo

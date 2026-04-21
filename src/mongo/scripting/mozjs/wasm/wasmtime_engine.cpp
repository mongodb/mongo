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

#include "mongo/scripting/mozjs/wasm/wasmtime_engine.h"

#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/scripting/config_engine_gen.h"
#include "mongo/scripting/config_gen.h"
#include "mongo/scripting/engine.h"
#include "mongo/scripting/mozjs/wasm/bridge/bridge.h"
#include "mongo/scripting/mozjs/wasm/scope/scope.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

// Symbols produced by objcopy from the AOT-compiled mozjs_wasm_api.cwasm.
// See embed_mozjs_wasm_obj in BUILD.bazel.
extern "C" {
extern const uint8_t _binary_mozjs_wasm_api_cwasm_start[];
extern const uint8_t _binary_mozjs_wasm_api_cwasm_end[];
}

namespace mongo {

bool isExternalScriptingEnabled() {
    return gEnableExternalScripting;
}

void ScriptEngine::setup(ExecutionEnvironment environment) {
    if (getGlobalScriptEngine()) {
        return;
    }

    if (isExternalScriptingEnabled()) {
        if (!serverGlobalParams.quiet.load()) {
            LOGV2_INFO(11542361, "External scripting is enabled. Not setting up Wasmtime engine.");
        }
        return;
    }

    if (!serverGlobalParams.quiet.load()) {
        LOGV2_INFO(11542362, "Setting up Wasmtime engine.");
    }

    setGlobalScriptEngine(new mozjs::WasmtimeScriptEngine());

    if (hasGlobalServiceContext()) {
        getGlobalServiceContext()->registerKillOpListener(getGlobalScriptEngine());
    }
}

namespace {
auto operationWasmtimeScopeDecoration =
    OperationContext::declareDecoration<mozjs::WasmtimeImplScope*>();
}  // namespace

namespace mozjs {

WasmtimeScriptEngine::WasmtimeScriptEngine() {
    size_t size =
        static_cast<size_t>(_binary_mozjs_wasm_api_cwasm_end - _binary_mozjs_wasm_api_cwasm_start);
    _wasmEngineCtx =
        wasm::WasmEngineContext::createFromPrecompiled(_binary_mozjs_wasm_api_cwasm_start, size);
}

WasmtimeScriptEngine::~WasmtimeScriptEngine() {}

mongo::Scope* WasmtimeScriptEngine::createScope() {
    return createScopeForCurrentThread(boost::none);
}

mongo::Scope* WasmtimeScriptEngine::createScopeForCurrentThread(
    boost::optional<int> jsHeapLimitMB) {
    // Resolve the heap limit: use passed value if provided, otherwise use global config.
    // If a limit is passed, cap it at the global limit (like MozJS does).
    const auto resolvedLimit = jsHeapLimitMB ? *jsHeapLimitMB : getJSHeapLimitMB();
    return new WasmtimeImplScope(_wasmEngineCtx, resolvedLimit);
}

// TODO (SERVER-122128): Implement interrupt support
// Registering is primarily used for interrupt to find the right scope to kill.
void WasmtimeScriptEngine::interrupt(ClientLock&, OperationContext* opCtx) {
    if (opCtx && (*opCtx)[operationWasmtimeScopeDecoration]) {
        (*opCtx)[operationWasmtimeScopeDecoration]->kill();
        LOGV2_DEBUG(11542360, 2, "Interrupting Wasmtime op", "opId"_attr = opCtx->getOpID());
    }
}
void WasmtimeScriptEngine::interruptAll(ServiceContextLock& svcCtxLock) {
    ServiceContext::LockedClientsCursor cursor(&*svcCtxLock);
    while (auto client = cursor.next()) {
        std::lock_guard lk(*client);
        if (auto opCtx = client->getOperationContext();
            opCtx && (*opCtx)[operationWasmtimeScopeDecoration]) {
            (*opCtx)[operationWasmtimeScopeDecoration]->kill();
        }
    }
}
void WasmtimeScriptEngine::registerOperation(OperationContext* opCtx, WasmtimeImplScope* scope) {
    (*opCtx)[operationWasmtimeScopeDecoration] = scope;
}
void WasmtimeScriptEngine::unregisterOperation(OperationContext* opCtx) {
    (*opCtx)[operationWasmtimeScopeDecoration] = nullptr;
}

// TODO (SERVER-116056): Add memory tracking functionality
int WasmtimeScriptEngine::getJSHeapLimitMB() const {
    return gJSHeapLimitMB.load();
}
void WasmtimeScriptEngine::setJSHeapLimitMB(int limit) {
    gJSHeapLimitMB.store(limit);
}
bool WasmtimeScriptEngine::getJSUseLegacyMemoryTracking() const {
    return false;
}
void WasmtimeScriptEngine::setJSUseLegacyMemoryTracking(bool) {}

}  // namespace mozjs
}  // namespace mongo

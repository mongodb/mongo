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
#include "mongo/scripting/mozjs/wasm/embedded_wasm_resource.h"
#include "mongo/scripting/mozjs/wasm/scope/scope.h"

#include <mutex>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

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
        registerScriptEngineKillOpProxy(getGlobalServiceContext());
    }
}

namespace {
auto operationWasmtimeScopeDecoration =
    OperationContext::declareDecoration<mozjs::WasmtimeImplScope*>();
}  // namespace

namespace mozjs {

// One idle bridge per thread. Populated when a healthy WasmtimeImplScope is destroyed;
// consumed (reset) by the next createScopeForCurrentThread() call on the same thread.
static thread_local WasmtimeScriptEngine::IdleBridge tl_idleBridge;

WasmtimeScriptEngine::WasmtimeScriptEngine() {
    // Pre-warm the context pool so the first kContextPoolSize scope creations and post-kill
    // recoveries do not pay the full Engine+Component+Linker initialisation cost on the hot path.
    auto [data, size] = wasm::getEmbeddedWasmResource();
    std::lock_guard<std::mutex> lk(_contextPoolMutex);
    _contextPool.reserve(kContextPoolSize);
    for (size_t i = 0; i < kContextPoolSize; ++i) {
        _contextPool.push_back(wasm::WasmEngineContext::createFromPrecompiled(data, size));
    }
}

WasmtimeScriptEngine::~WasmtimeScriptEngine() {}

std::shared_ptr<wasm::WasmEngineContext> WasmtimeScriptEngine::createWasmEngineContext() const {
    // Serialise all context creation (pool hit and fallback) under a single lock.
    // Concurrent wasmtime_component_deserialize calls on the same pre-compiled bytes share
    // internal JIT allocations without Arc protection, causing an ASAN double-free when the
    // resulting WasmEngineContexts are later destroyed on separate threads.
    // Holding the pool mutex for the fallback is acceptable: the hot path (pool hit) is a fast
    // vector pop, and the cold path (fresh deserialise) is bounded by the number of concurrent
    // threads that have simultaneously exhausted their idle-bridge slot.
    std::lock_guard<std::mutex> lk(_contextPoolMutex);
    if (!_contextPool.empty()) {
        auto ctx = std::move(_contextPool.back());
        _contextPool.pop_back();
        return ctx;
    }
    auto [data, size] = wasm::getEmbeddedWasmResource();
    return wasm::WasmEngineContext::createFromPrecompiled(data, size);
}

mongo::Scope* WasmtimeScriptEngine::createScope() {
    return createScopeForCurrentThread(boost::none);
}

mongo::Scope* WasmtimeScriptEngine::createScopeForCurrentThread(
    boost::optional<int> jsHeapLimitMB) {
    // Resolve the heap limit: use passed value if provided, otherwise use global config.
    // If a limit is passed, cap it at the global limit (like MozJS does).
    const auto resolvedLimit = jsHeapLimitMB ? *jsHeapLimitMB : getJSHeapLimitMB();

    // Reuse idle bridge from a previous request on this thread if it is still healthy.
    // WasmtimeImplScope::init() will call resetRealm() on it instead of doing a full
    // ~30 ms WASM instantiation + SpiderMonkey init.
    if (tl_idleBridge.bridge && tl_idleBridge.bridge->isHealthy() &&
        std::chrono::steady_clock::now() - tl_idleBridge.parkedAt <= kMaxBridgeIdleTime &&
        tl_idleBridge.reuseCount <= kMaxBridgeReuseCount) {
        auto bridge = std::move(tl_idleBridge.bridge);
        auto ctx = std::move(tl_idleBridge.ctx);
        auto cache = std::move(tl_idleBridge.cachedFunctions);
        const auto reuseCount = tl_idleBridge.reuseCount;
        const auto parkedAt = tl_idleBridge.parkedAt;
        tl_idleBridge = {};
        const auto parkedForMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - parkedAt)
                                     .count();
        LOGV2_DEBUG(11542377,
                    2,
                    "WASM scope reviving parked bridge",
                    "reuseCount"_attr = reuseCount,
                    "parkedForMs"_attr = parkedForMs,
                    "cachedFunctions"_attr = static_cast<int>(cache.size()));
        return new WasmtimeImplScope(
            std::move(ctx), resolvedLimit, std::move(bridge), std::move(cache));
    }

    // Bridge was absent, unhealthy, idle for too long, or reuse limit reached — drop it to release
    // WASM linear memory and prevent JIT-cache accumulation from exhausting the store.
    if (tl_idleBridge.bridge) {
        const bool healthy = tl_idleBridge.bridge->isHealthy();
        const StringData reason = !healthy                    ? "unhealthy"_sd
            : tl_idleBridge.reuseCount > kMaxBridgeReuseCount ? "reuse_limit"_sd
                                                              : "expired"_sd;
        // Final memory footprint of the dropped bridge (best-effort; the bridge may be
        // trapped and unable to execute the stats call).
        long long linearMemoryMB = -1;
        long long gcHeapMB = -1;
        if (healthy) {
            try {
                auto stats = tl_idleBridge.bridge->getMemoryStats();
                if (!stats.isEmpty()) {
                    linearMemoryMB = stats["linearMemoryBytes"].numberLong() / (1024 * 1024);
                    gcHeapMB = stats["gcHeapBytes"].numberLong() / (1024 * 1024);
                }
            } catch (const DBException&) {
            }
        }
        LOGV2(11542378,
              "WASM scope dropping parked bridge",
              "reason"_attr = reason,
              "initialized"_attr = tl_idleBridge.bridge->isInitialized(),
              "trapped"_attr = tl_idleBridge.bridge->hasTrapped(),
              "oom"_attr = tl_idleBridge.bridge->hasOomError(),
              "killPending"_attr = tl_idleBridge.bridge->isKillPending(),
              "reuseCount"_attr = tl_idleBridge.reuseCount,
              "linearMemoryMB"_attr = linearMemoryMB,
              "gcHeapMB"_attr = gcHeapMB);
    }
    tl_idleBridge = {};
    LOGV2_DEBUG(
        11542379, 2, "WASM scope creating fresh bridge", "heapLimitMB"_attr = resolvedLimit);
    return new WasmtimeImplScope(createWasmEngineContext(), resolvedLimit);
}

void WasmtimeScriptEngine::parkBridgeForCurrentThread(std::unique_ptr<wasm::MozJSWasmBridge> bridge,
                                                      std::shared_ptr<wasm::WasmEngineContext> ctx,
                                                      FunctionCacheMap cachedFunctions) {
    tl_idleBridge.reuseCount++;
    // Sample real linear-memory usage at park time. Logged on the first park and then
    // whenever the bridge grew by another 64 MB, so the growth curve of a long-lived
    // bridge (and the gap between linear memory and the GC-bounded JS heap) is visible
    // in production logs without logging every park.
    try {
        auto stats = bridge->getMemoryStats();
        if (!stats.isEmpty()) {
            const auto linearBytes = static_cast<uint64_t>(stats["linearMemoryBytes"].numberLong());
            constexpr uint64_t kLogGrowthStepBytes = 64 * 1024 * 1024;
            if (linearBytes >= tl_idleBridge.lastLoggedLinearMemoryBytes + kLogGrowthStepBytes) {
                LOGV2(11542385,
                      "WASM bridge linear memory growth",
                      "reuseCount"_attr = tl_idleBridge.reuseCount,
                      "linearMemoryMB"_attr = linearBytes / (1024 * 1024),
                      "gcHeapMB"_attr = stats["gcHeapBytes"].numberLong() / (1024 * 1024),
                      "gcNumber"_attr = stats["gcNumber"].numberLong(),
                      "heapLimitMB"_attr = bridge->getHeapLimitMB(),
                      "storeLimitMB"_attr = bridge->getStoreLimitMB());
                tl_idleBridge.lastLoggedLinearMemoryBytes = linearBytes;
            }
        }
    } catch (const DBException&) {
        // Stats are best-effort diagnostics; never fail a park because of them.
    }
    LOGV2_DEBUG(11542380,
                2,
                "WASM scope parking bridge",
                "reuseCount"_attr = tl_idleBridge.reuseCount,
                "cachedFunctions"_attr = static_cast<int>(cachedFunctions.size()));
    tl_idleBridge.bridge = std::move(bridge);
    tl_idleBridge.ctx = std::move(ctx);
    tl_idleBridge.cachedFunctions = std::move(cachedFunctions);
    tl_idleBridge.parkedAt = std::chrono::steady_clock::now();
}

void WasmtimeScriptEngine::backdateIdleBridgeForTest(Milliseconds age) {
    tl_idleBridge.parkedAt =
        std::chrono::steady_clock::now() - std::chrono::milliseconds{age.count()};
}

bool WasmtimeScriptEngine::hasIdleBridgeForTest() {
    return static_cast<bool>(tl_idleBridge.bridge);
}

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
    std::lock_guard lk(*opCtx->getClient());
    (*opCtx)[operationWasmtimeScopeDecoration] = scope;

    if (auto status = opCtx->checkForInterruptNoAssert(); !status.isOK()) {
        scope->kill();
    }
}
void WasmtimeScriptEngine::unregisterOperation(OperationContext* opCtx) {
    std::lock_guard lk(*opCtx->getClient());
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

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

#include "mongo/base/error_codes.h"
#include "mongo/scripting/engine.h"
#include "mongo/scripting/mozjs/wasm/bridge/bridge.h"
#include "mongo/util/time_support.h"

#include <chrono>
#include <functional>
#include <mutex>
#include <vector>

namespace mongo {
namespace mozjs {

class WasmtimeImplScope;

/**
 * Derives the kill reason a JS scope should be killed with for 'opCtx', or ErrorCodes::OK if the
 * operation is not interrupted. Mirrors OperationContext::checkForInterruptNoAssert() priority:
 *   1. an expired deadline (honoring the maxTimeNeverTimeOut/maxTimeAlwaysTimeOut failpoints)
 *      outranks everything, e.g. MaxTimeMSExpired;
 *   2. then the recorded kill status (killOp, killCursors, shutdown, ...);
 *   3. then a disconnected client session (SERVER-130767: an op stuck inside JS never polls
 *      checkForInterrupt(), so nothing else ever discovers the disconnect).
 *
 * Concurrency: per getKillStatus()'s contract, the caller must either be the thread executing
 * on behalf of 'opCtx' or hold the lock on the Client owning it. The kill-op listener paths
 * (interrupt()/interruptAll()) are called with that lock held; WasmtimeImplScope's
 * kill()/isKillPending() take it before calling this from the DeadlineMonitor thread.
 */
ErrorCodes::Error scriptKillReasonFor(OperationContext* opCtx);

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

    void enableJavaScriptProtection(bool value) override;
    bool isJavaScriptProtectionEnabled() const override;

    int getJSHeapLimitMB() const override;
    void setJSHeapLimitMB(int limit) override;

    bool getJSUseLegacyMemoryTracking() const override;
    void setJSUseLegacyMemoryTracking(bool shouldUseLegacy) override;

    // These are mongosh-specific and not needed as part of runtime, so we can just make them
    // unreachable for now.
    std::string getLoadPath() const override { MONGO_UNREACHABLE };
    void setLoadPath(const std::string& loadPath) override { MONGO_UNREACHABLE };
    std::string getInterpreterVersionString() const override { MONGO_UNREACHABLE };

    // Allow impl scopes to register with their OperationContext so interrupts can find them.
    void registerOperation(OperationContext* ctx,
                           WasmtimeImplScope* scope,
                           std::function<void()> onTeardown);
    void unregisterOperation(OperationContext* opCtx);

    // Returns a pre-warmed WasmEngineContext from the pool if available, or creates a fresh one.
    // Each scope (and each post-kill rebuild) gets its own context so that the engine-wide
    // interrupt-epoch counter cannot leak across scopes.
    std::shared_ptr<wasm::WasmEngineContext> createWasmEngineContext() const;

    // Thread-local idle bridge (one per thread). Populated when a healthy scope is destroyed;
    // consumed by the next createScopeForCurrentThread() on the same thread.
    struct IdleBridge {
        std::unique_ptr<wasm::MozJSWasmBridge> bridge;
        std::shared_ptr<wasm::WasmEngineContext> ctx;
        // Function-handle cache from the previous scope. resetRealm() invalidates all
        // compiled function handles on revival, so this cache is cleared on reuse.
        FunctionCacheMap cachedFunctions;
        // steady_clock so NTP backward jumps cannot make the bridge appear fresh forever.
        std::chrono::steady_clock::time_point parkedAt;
        // Number of times this bridge has been parked (reused). SpiderMonkey's JIT code
        // and internal caches accumulate with each reuse. Discarding after kMaxBridgeReuseCount
        // reuses bounds the linear-memory growth over long passthrough runs.
        uint32_t reuseCount{0};
        // Linear-memory size (bytes) at the last "linear memory growth" log line for this
        // bridge. Lets parkBridgeForCurrentThread() log only when memory grew materially
        // instead of on every park.
        uint64_t lastLoggedLinearMemoryBytes{0};

        IdleBridge() = default;
        IdleBridge(IdleBridge&&) = default;
        IdleBridge& operator=(IdleBridge&&) = default;

        // On thread exit, all WASM linear memory is freed by wasmtime's Store
        // destructor when `bridge`'s unique_ptr fires — the in-WASM SpiderMonkey
        // destructor does not need to run explicitly. Calling bridge->shutdown()
        // here is unsafe: it would execute JIT code while other threads may be
        // concurrently destroying their own stores and flushing the shared JIT
        // icache, which causes use-after-free in the JIT executor on aarch64.
        ~IdleBridge() = default;
    };

    static constexpr std::chrono::seconds kMaxBridgeIdleTime{1};
    // Safety net: discard the idle bridge after this many reuses so that slow accumulation in
    // WASM linear memory (which never shrinks, even after GC) cannot build up indefinitely in a
    // long-lived bridge. Steady-state growth per reuse is small now that WIT argument buffers
    // are freed by the in-WASM bindings (see ArgListGuard in engine/api.cpp); this bound exists
    // to cap the blast radius of any future leak, not to work around a known one.
    static constexpr uint32_t kMaxBridgeReuseCount{100};

    void parkBridgeForCurrentThread(std::unique_ptr<wasm::MozJSWasmBridge> bridge,
                                    std::shared_ptr<wasm::WasmEngineContext> ctx,
                                    FunctionCacheMap cachedFunctions);

    // Test-only: rewind the current thread's idle bridge park time so expiry tests don't sleep.
    static void backdateIdleBridgeForTest(Milliseconds age);

    // Test-only: returns true if the current thread's idle bridge slot holds a parked bridge.
    // Used by regression tests to confirm that an emit-configured bridge is shut down (not
    // parked) on scope teardown — see WasmtimeImplScope::~WasmtimeImplScope.
    static bool hasIdleBridgeForTest();

private:
    // A small pool of lazily-warmed contexts reduces the cost of cold-start scope creation and
    // post-kill recovery by amortising Engine+Component+Linker initialisation over time.
    //
    // Pool size is intentionally small (2) to limit the virtual memory reserved at pool
    // warm-up time. Each WasmEngineContext holds a Wasmtime Engine + deserialized Component +
    // Linker, and each bridge created from one reserves ~1.2 GB of virtual address space for
    // WASM linear memory (wasmtimeStoreMemoryLimitMB). In environments where multiple mongod
    // processes run concurrently (e.g. ShardingTest with 3–5 nodes), the eager reservation of
    // 4 × 1.2 GB = 4.8 GB per mongod at first JS use can exhaust available virtual/physical
    // memory before any query runs.
    //
    // With size 2, the pool covers the common case of rapid sequential scope reuse (cold-start
    // on a fresh thread + one post-kill rebuild) while deferring further deserialization until
    // actually needed. Scopes beyond the pool limit are created on-demand under the pool mutex
    // (serialised to avoid a Wasmtime ASAN double-free on concurrent deserialisation of the
    // same pre-compiled bytes — see wasmtime_engine.cpp for details). The pool does NOT bound
    // the maximum number of concurrent scopes; it is purely a warm-up performance cache.
    mutable std::mutex _contextPoolMutex;
    mutable std::once_flag _poolOnce;
    mutable std::vector<std::shared_ptr<wasm::WasmEngineContext>> _contextPool;
    static constexpr size_t kContextPoolSize = 2;
};

}  // namespace mozjs
}  // namespace mongo

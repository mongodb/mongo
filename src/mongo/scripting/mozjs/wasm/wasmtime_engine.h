// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
    // Removes 'scope's entry from the registrations on 'opCtx'. Other scopes' overlapping
    // registrations are left intact: a stale unregister must never remove another registrant.
    void unregisterOperation(OperationContext* opCtx, WasmtimeImplScope* scope);

    // Returns the process-wide shared WasmEngineContext, deserializing it once on first use. Every
    // scope shares the same context; kills stay isolated per Store via each bridge's epoch-deadline
    // callback, so sharing the engine-wide interrupt-epoch counter is safe (see the _wasmContext
    // member and getWasmEngineContext()).
    std::shared_ptr<wasm::WasmEngineContext> getWasmEngineContext() const;

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
    // One long-lived WasmEngineContext (Engine + deserialized Component + Linker) shared by every
    // scope on this engine. A WasmEngineContext is structurally immutable after construction; the
    // engine's interrupt-epoch counter is shared mutable state, but it is isolated per Store by
    // each bridge's epoch-deadline callback (see bridge.cpp). The context is explicitly designed to
    // back many bridges: MozJSWasmBridge instantiates a fresh per-Store Instance from the shared
    // _component/_linker under wasmLifecycleMutex(). Deserializing the component once — instead of
    // per scope — eliminates the dominant cold-start cost (~90% of a cold scope creation; the rest
    // is per-Store instantiation + SpiderMonkey init). It also sidesteps the Wasmtime ASAN
    // double-free from concurrent wasmtime_component_deserialize of the same bytes, and the
    // cross-thread destruction hazard, because the context is deserialized exactly once and
    // destroyed exactly once at engine teardown. Per-scope WASM linear-memory reservation is
    // unaffected (it belongs to each Store, not the shared context).
    mutable std::once_flag _wasmContextOnce;
    mutable std::shared_ptr<wasm::WasmEngineContext> _wasmContext;
};

}  // namespace mozjs
}  // namespace mongo

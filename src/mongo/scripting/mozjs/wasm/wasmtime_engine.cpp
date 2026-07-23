// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
#include "mongo/util/timer.h"

#include <algorithm>
#include <mutex>
#include <vector>

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
// Decoration that keeps track of which WasmtimeImplScopes are active on an OperationContext.
// An operation can have several scopes registered with overlapping lifetimes (e.g. a JsExecution
// scope registered for the whole operation while a $where JsFunction re-registers a pooled scope
// per predicate call), so this holds one entry per live registrant rather than a single slot: a
// single slot silently displaces the earlier scope, losing both its interrupt delivery (a stuck-JS
// hang) and its teardown (a use-after-free of the opCtx in ~WasmtimeImplScope()).
//
// The struct (rather than raw pointers) exists so the destructor can clear each scope's _opCtx
// before the OperationContext's memory is freed, preventing ~WasmtimeImplScope() from reading
// freed memory when it calls unregisterOperation().
struct WasmtimeScopeRegistry {
    struct ScopeEntry {
        mozjs::WasmtimeImplScope* scope;
        // Clears the scope's _opCtx back-pointer; invoked when the opCtx is torn down.
        std::function<void()> onTeardown;
    };
    // Small: at most a handful of scopes are ever live on one operation.
    std::vector<ScopeEntry> scopeEntries;
    // Stored at registerOperation() time.  The Client outlives the OperationContext, so it is
    // safe to dereference here even though the OperationContext is being destroyed.
    Client* client = nullptr;

    ~WasmtimeScopeRegistry() {
        if (scopeEntries.empty() || !client)
            return;
        // Take the Client lock before touching the decoration so we serialise with both
        // interrupt() (which reads the entries under the lock) and unregisterOperation() (which
        // erases entries under the lock).  Clear the entries first so interrupt() cannot call
        // kill() on a scope whose bridge may be concurrently destroyed.  Then zero _opCtx in
        // each scope atomically so that ~WasmtimeImplScope()'s unregisterOperation() call will
        // see null and skip the engine->unregisterOperation(opCtx) call that would read freed
        // memory.
        std::lock_guard lk(*client);
        auto detached = std::move(scopeEntries);
        scopeEntries.clear();
        client = nullptr;
        for (auto& e : detached) {
            e.onTeardown();
        }
    }
};

auto operationWasmtimeScopeDecoration =
    OperationContext::declareDecoration<WasmtimeScopeRegistry>();
}  // namespace

namespace mozjs {

// One idle bridge per thread. Populated when a healthy WasmtimeImplScope is destroyed;
// consumed (reset) by the next createScopeForCurrentThread() call on the same thread.
static thread_local WasmtimeScriptEngine::IdleBridge tl_idleBridge;

WasmtimeScriptEngine::WasmtimeScriptEngine() {}

WasmtimeScriptEngine::~WasmtimeScriptEngine() {}

std::shared_ptr<wasm::WasmEngineContext> WasmtimeScriptEngine::getWasmEngineContext() const {
    // Deserialize the Engine + Component + Linker exactly once and share the resulting
    // WasmEngineContext with every scope. A context is immutable after construction and is designed
    // to back many bridges (each MozJSWasmBridge instantiates its own Store/Instance from the
    // shared component under wasmLifecycleMutex()).
    //
    // Deserializing once also sidesteps the concurrent-deserialise ASAN double-free and the
    // cross-thread destruction hazard the previous pool guarded against: the shared context is
    // built once (under call_once) and destroyed once at engine teardown.
    //
    // Built lazily on the first JS scope request so workloads that never execute JS pay no upfront
    // memory cost.
    std::call_once(_wasmContextOnce, [this] {
        auto [data, size] = wasm::getEmbeddedWasmResource();
        Timer timer;
        _wasmContext = wasm::WasmEngineContext::createFromPrecompiled(data, size);
        LOGV2(
            11600003, "Initialized shared WASM engine context", "durationMs"_attr = timer.millis());
    });
    return _wasmContext;
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
        const auto lastLoggedLinearMemoryBytes = tl_idleBridge.lastLoggedLinearMemoryBytes;
        tl_idleBridge = {};
        // Preserve the accumulated reuseCount so parkBridgeForCurrentThread's increment
        // correctly tracks lifetime reuse across revival cycles. Without this,
        // kMaxBridgeReuseCount eviction would never trigger.
        tl_idleBridge.reuseCount = reuseCount;
        // Preserve the last-logged watermark so the "WASM bridge linear memory growth" log
        // fires only on actual new growth, not on every revival of a steady-state bridge.
        tl_idleBridge.lastLoggedLinearMemoryBytes = lastLoggedLinearMemoryBytes;
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
        const std::string_view reason = !healthy              ? "unhealthy"
            : tl_idleBridge.reuseCount > kMaxBridgeReuseCount ? "reuse_limit"
                                                              : "expired";
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
    return new WasmtimeImplScope(getWasmEngineContext(), resolvedLimit);
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

namespace {
// Near-equivalent of OperationContext::hasDeadlineExpired() (which is private), built from
// public API: getRemainingMaxTimeMillis() returns Milliseconds::max() when there is no deadline
// and clamps to zero once the deadline has passed. The maxTimeNeverTimeOut/maxTimeAlwaysTimeOut
// failpoints suppress/force deadline expiry exactly as they do there (both only apply to ops
// that actually have a deadline).
//
// One deliberate divergence: hasDeadlineExpired() compares against the *extended* deadline
// (OverrideDeadlineGuard), while getRemainingMaxTimeMillis() uses the base deadline. The guard
// is only armed around egress RPCs (see network_interface_tl.cpp), which an op executing JS on
// its own thread is never inside, so the extension cannot be active when this decides a JS kill
// reason; at worst an extended op would be attributed its timeout error marginally early.
bool opCtxDeadlineHasExpired(OperationContext* opCtx) {
    const auto remaining = opCtx->getRemainingMaxTimeMillis();
    if (remaining == Milliseconds::max()) {
        return false;  // No deadline.
    }
    if (MONGO_unlikely(maxTimeNeverTimeOut.shouldFail())) {
        return false;
    }
    if (MONGO_unlikely(maxTimeAlwaysTimeOut.shouldFail())) {
        return true;
    }
    return remaining == Milliseconds::zero();
}
}  // namespace

ErrorCodes::Error scriptKillReasonFor(OperationContext* opCtx) {
    // An expired deadline outranks the recorded kill status, matching
    // checkForInterruptNoAssert() priority (see BF-44481: a late external kill on a timed-out op
    // must still surface as the deadline error).
    if (opCtxDeadlineHasExpired(opCtx)) {
        return opCtx->getTimeoutError();
    }
    if (auto killStatus = opCtx->getKillStatus(); killStatus != ErrorCodes::OK) {
        return killStatus;
    }
    // SERVER-130767: nothing proactively kills an operation whose client disconnected -- the
    // disconnect is only discovered inside checkForInterruptNoAssert() on the operation's own
    // thread, which a thread stuck inside JS never reaches. Check the session directly, exactly
    // as _checkClientConnected() does (Client::session() is immutable after Client construction
    // and Session::isConnected() is called from non-operation threads in production, so this is
    // safe from the DeadlineMonitor thread).
    //
    // One divergence from checkForInterruptNoAssert(): that path only consults the session when
    // the op opted in via markKillOnClientDisconnect(), but that flag has no public getter, so
    // this checks unconditionally. That is the desired behavior for the JS path -- a $where/
    // $function stuck in a loop after its client vanished is exactly the SERVER-130767 hang we
    // want to break -- but it does mean a JS op that deliberately did not opt into
    // disconnect-kill is still stopped on socket close.
    auto client = opCtx->getClient();
    if (!client->isInDirectClient()) {
        if (const auto& session = client->session(); session && !session->isConnected()) {
            return client->getDisconnectErrorCode();
        }
    }
    return ErrorCodes::OK;
}

void WasmtimeScriptEngine::interrupt(ClientLock&, OperationContext* opCtx) {
    if (opCtx && !(*opCtx)[operationWasmtimeScopeDecoration].scopeEntries.empty()) {
        // ServiceContext::killOperation() has already called opCtx->markKilled(killCode) by the
        // time it notifies us -- on this same thread, in the same call -- so the real reason is
        // known with certainty right here. Pass it straight through instead of losing it behind
        // a reason-less kill() and reconstructing it later from (possibly stale) opCtx state.
        //
        // One exception, mirroring checkForInterruptNoAssert() priority: if the opCtx's own
        // deadline has already expired, the deadline error (e.g. MaxTimeMSExpired) outranks the
        // killer's code. An op stuck inside JS never observes its deadline via
        // checkForInterrupt(), so the first kill it sees may be a later external one (e.g. a
        // killCursors after mongos gave up) -- reporting that code instead of MaxTimeMSExpired
        // breaks callers that dispatch on it, such as allowPartialResults (BF-44481).
        // killOperation() guarantees a kill status is recorded, so scriptKillReasonFor() cannot
        // return OK here; Interrupted is a defensive fallback (killWithReason() rejects OK).
        auto reason = scriptKillReasonFor(opCtx);
        // Fan the kill out to every scope registered on this operation: several scopes can have
        // overlapping registrations (e.g. a JsExecution scope plus a $where predicate scope),
        // and each of them may be the one currently executing JS.
        for (auto& entry : (*opCtx)[operationWasmtimeScopeDecoration].scopeEntries) {
            entry.scope->killWithReason(reason != ErrorCodes::OK ? reason
                                                                 : ErrorCodes::Interrupted);
        }
        LOGV2_DEBUG(11542360, 2, "Interrupting Wasmtime op", "opId"_attr = opCtx->getOpID());
    }
}
void WasmtimeScriptEngine::interruptAll(ServiceContextLock& svcCtxLock) {
    ServiceContext::LockedClientsCursor cursor(&*svcCtxLock);
    while (auto client = cursor.next()) {
        std::lock_guard lk(*client);
        if (auto opCtx = client->getOperationContext();
            opCtx && !(*opCtx)[operationWasmtimeScopeDecoration].scopeEntries.empty()) {
            // By the time ServiceContext broadcasts interruptAll() to listeners, every opCtx has
            // already been through killOperation(InterruptedAtShutdown) in
            // interruptOperations(), except clients excluded from that per-op loop
            // (shouldExcludeFromInterruptAtShutdown()). Fall back to InterruptedAtShutdown for
            // those rather than propagating a stale OK as the kill reason.
            // Same deadline-outranks-killer rule as interrupt() above.
            auto reason = scriptKillReasonFor(opCtx);
            for (auto& entry : (*opCtx)[operationWasmtimeScopeDecoration].scopeEntries) {
                entry.scope->killWithReason(
                    reason != ErrorCodes::OK ? reason : ErrorCodes::InterruptedAtShutdown);
            }
        }
    }
}
void WasmtimeScriptEngine::registerOperation(OperationContext* opCtx,
                                             WasmtimeImplScope* scope,
                                             std::function<void()> onTeardown) {
    std::lock_guard lk(*opCtx->getClient());
    auto& reg = (*opCtx)[operationWasmtimeScopeDecoration];
    reg.client = opCtx->getClient();
    // Re-registration of an already-registered scope (e.g. defensive double-registration)
    // just refreshes its teardown; otherwise add a new entry alongside any existing ones so
    // overlapping registrants (JsExecution scope + $where predicate scope) each keep their own
    // interrupt delivery and teardown.
    auto it = std::find_if(reg.scopeEntries.begin(), reg.scopeEntries.end(), [&](const auto& e) {
        return e.scope == scope;
    });
    if (it != reg.scopeEntries.end()) {
        it->onTeardown = std::move(onTeardown);
    } else {
        reg.scopeEntries.push_back({scope, std::move(onTeardown)});
    }

    // If the opCtx is already interrupted (e.g. maxTimeMS expired between the previous
    // predicate call and this one -- see JsFunction::runAsPredicate(), which re-registers on
    // every document), kill the scope now so the next invoke() traps immediately. Forward the
    // real reason: this runs synchronously on the operation's own thread, so status.code() is
    // exactly as authoritative as opCtx's kill code in WasmtimeScriptEngine::interrupt() above --
    // there is nothing to reconstruct later if we don't discard it here.
    if (auto status = opCtx->checkForInterruptNoAssert(); !status.isOK()) {
        scope->killWithReason(status.code());
    }
}
void WasmtimeScriptEngine::unregisterOperation(OperationContext* opCtx, WasmtimeImplScope* scope) {
    std::lock_guard lk(*opCtx->getClient());
    auto& reg = (*opCtx)[operationWasmtimeScopeDecoration];
    // Remove only the calling scope's entry. Other scopes with overlapping registrations must
    // keep theirs: blanket-nulling would lose their interrupt delivery (stuck-JS hang) and
    // orphan their _opCtx cleanup (use-after-free).
    std::erase_if(reg.scopeEntries, [&](const auto& e) { return e.scope == scope; });
    if (reg.scopeEntries.empty()) {
        reg.client = nullptr;
    }
}

void WasmtimeScriptEngine::enableJavaScriptProtection(bool value) {
    gJavascriptProtection.store(value);
}

bool WasmtimeScriptEngine::isJavaScriptProtectionEnabled() const {
    return gJavascriptProtection.load();
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

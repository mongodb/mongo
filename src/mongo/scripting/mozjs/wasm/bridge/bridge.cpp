// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/scripting/mozjs/wasm/bridge/bridge.h"

#include "mongo/logv2/log.h"
#include "mongo/scripting/config_engine_gen.h"
#include "mongo/util/fail_point.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string_view>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::mozjs::wasm {

#if defined(MONGO_CONFIG_DEBUG_BUILD)
// Test-only hook: when enabled, this bridge's epoch-deadline callback returns an error instead of
// continuing, producing a genuine non-kill wasmtime trap that originates inside the guest. Lets
// tests drive the real _callFunc -> _throwAfterTrap path (and verify its non-memory
// classification) without a SpiderMonkey MOZ_CRASH, which cannot be provoked deterministically
// from guest JS. Defined only in debug builds, so it is entirely absent from production.
MONGO_FAIL_POINT_DEFINE(wasmBridgeInjectEpochTrap);
#endif

constexpr std::string_view kMozjsWitInterface = "mongo:mozjs/mozjs";
constexpr std::string_view kInitEngine = "initialize-engine";
constexpr std::string_view kShutdownEngine = "shutdown-engine";
constexpr std::string_view kResetEngine = "reset-engine";
constexpr std::string_view kResetRealm = "reset-realm";
constexpr std::string_view kCreateFunction = "create-function";
constexpr std::string_view kInvokeFunction = "invoke-function";
constexpr std::string_view kSetGlobal = "set-global";
constexpr std::string_view kSetGlobalValue = "set-global-value";
constexpr std::string_view kInvokePredicate = "invoke-predicate";
constexpr std::string_view kSetupEmit = "setup-emit";
constexpr std::string_view kInvokeMap = "invoke-map";
constexpr std::string_view kDrainEmitBuffer = "drain-emit-buffer";
constexpr std::string_view kGetGlobal = "get-global";
constexpr std::string_view kGetReturnValueBson = "get-return-value-bson";
constexpr std::string_view kGetMemoryStats = "get-memory-stats";
constexpr std::string_view kReturnValue = "__returnValue";

namespace {
// Serialises all wasmtime Engine/Component/Store lifecycle ops (deserialize, instantiate,
// cold-start initialize/shutdown, and destruction) process-wide. wasmtime does not
// synchronise these against each other, so running a context/store teardown on one thread
// while another is mid-deserialize, instantiating, or executing cold-start JIT corrupts
// shared JIT state and segfaults (SEGV_MAPERR). Steady-state execution
// (invoke/createFunction/reset*) runs on a fully-built store and is left lock-free.
std::mutex& wasmLifecycleMutex() {
    static std::mutex m;
    return m;
}
}  // namespace

std::shared_ptr<WasmEngineContext> WasmEngineContext::createFromPrecompiled(const uint8_t* data,
                                                                            size_t size) {
    std::unique_ptr<WasmEngineContext> ctx;
    {
        std::lock_guard<std::mutex> lifecycleLock(wasmLifecycleMutex());
        wt::Config config;
        config.wasm_component_model(true);
        config.epoch_interruption(true);
        config.wasm_exceptions(true);

        wt::Engine engine(std::move(config));

        wt::Span<uint8_t> span(const_cast<uint8_t*>(data), size);
        auto result = wc::Component::deserialize(engine, span);
        invariant(result);

        // Build and configure the linker once per context. MozJSWasmBridge instances
        // reuse this linker for instantiation, avoiding the add_wasip2() cost per bridge.
        wc::Linker linker(engine);
        invariant(linker.add_wasip2());

        ctx.reset(new WasmEngineContext(std::move(engine), result.ok(), std::move(linker)));
    }

    // Wrap in shared_ptr *after* releasing the lifecycle lock: the shared_ptr control-block
    // allocation can throw (e.g. bad_alloc), and on failure it runs the held pointer's deleter
    // -> ~WasmEngineContext, which re-locks wasmLifecycleMutex(). Doing it under the lock would
    // self-deadlock on the non-recursive mutex.
    return std::shared_ptr<WasmEngineContext>(std::move(ctx));
}

WasmEngineContext::~WasmEngineContext() {
    std::lock_guard<std::mutex> lifecycleLock(wasmLifecycleMutex());
    // Release in reverse construction order under the lifecycle lock.
    _linker.reset();
    _component.reset();
    _engine.reset();
}

MozJSWasmBridge::MozJSWasmBridge(std::shared_ptr<WasmEngineContext> ctx, Options opts)
    : _javascriptProtection(opts.javascriptProtection), _ctx(std::move(ctx)) {
    // Serialise Store creation + instantiation against all other wasm lifecycle ops.
    std::lock_guard<std::mutex> lifecycleLock(wasmLifecycleMutex());
    // If construction fails partway, the Store/Instance must be torn down while we still
    // hold the lifecycle lock. ~MozJSWasmBridge does not run for a constructor that throws,
    // so without this the optional members would be destroyed during unwinding *after*
    // lifecycleLock has already released the mutex — exactly the unsynchronised store
    // teardown the lock exists to prevent.
    try {
        _store = wt::Store(*_ctx->_engine);
        _stats.createdAtMonoNanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::steady_clock::now().time_since_epoch())
                                        .count();

        invariant(opts.linearMemoryLimitMB > 0);

        // The JS heap lives inside WASM linear memory alongside SpiderMonkey's
        // runtime overhead (stack, GC metadata, self-hosted code, malloc arena).
        // The store limit must exceed the heap limit to leave room for that overhead.
        //
        // Resolve the effective limit: if a per-query override is set, take the
        // minimum of it and the global limit (matching MozJSImplScope::MozRuntime
        // semantics in the shell scripting engine). Otherwise use the global limit.
        const uint32_t globalLimitMB = static_cast<uint32_t>(gJSHeapLimitMB.load());
        _jsHeapLimitMB =
            opts.jsHeapLimitMB > 0 ? std::min(opts.jsHeapLimitMB, globalLimitMB) : globalLimitMB;
        _storeLimitMB = opts.linearMemoryLimitMB;
        uint32_t minOverheadMB = std::max(64u, _jsHeapLimitMB / 10);
        uint32_t minStoreMB = _jsHeapLimitMB + minOverheadMB;

        uassert(ErrorCodes::BadValue,
                fmt::format("wasmtimeStoreMemoryLimitMB ({}) must be at least jsHeapLimitMB ({}) + "
                            "overhead ({}) = {}",
                            opts.linearMemoryLimitMB,
                            _jsHeapLimitMB,
                            minOverheadMB,
                            minStoreMB),
                opts.linearMemoryLimitMB >= minStoreMB);

        // Register a callback-based ResourceLimiter so memory.grow denials are
        // recorded in _growDeniedByLimiter before the unreachable trap fires.
        // This lets _throwAfterTrap() classify OOM deterministically without
        // string matching or watermark heuristics.
        //
        // 'this' is safe to capture: MozJSWasmBridge is non-movable, and the
        // store is owned by this bridge and destroyed before the bridge is.
        //
        // Only the linear-memory byte ceiling is enforced here. The replaced
        // numeric wasmtime_store_limiter() also pinned memories=1 and left
        // instances/tables unbounded (-1); for MozJS-in-WASM (exactly one linear
        // memory, one instance, no growable tables) those extra ceilings never
        // bound anything, so dropping them is intentional and behavior-preserving.
        // The 'maximum' argument (the module-declared ceiling) is likewise ignored
        // on purpose: the configured store byte limit is authoritative.
        _store->resource_limiter(
            [](size_t /*current*/, size_t desired, int64_t /*maximum*/, void* data) -> bool {
                auto* bridge = static_cast<MozJSWasmBridge*>(data);
                const size_t limitBytes = static_cast<size_t>(bridge->_storeLimitMB) * 1024 * 1024;
                // Record the requested size so a subsequent trap can be logged against
                // the store limit even though we no longer track a live watermark.
                bridge->_growDeniedDesiredBytes.store(desired);
                if (desired > limitBytes) {
                    bridge->_growDeniedByLimiter.store(true);
                    return false;
                }
                return true;
            },
            this,
            /*finalizer=*/nullptr);

        auto storeCtx = _store->context();

        // Arm the epoch interrupt at current+1 so the first increment_epoch() fires.
        storeCtx.set_epoch_deadline(1);

        // Per-bridge epoch callback: only trap when THIS bridge's kill flag is set.
        // Without this, any kill (which calls increment_epoch() on the shared engine) would
        // interrupt every concurrently-running store, causing epoch bleed across unrelated
        // WASM scopes. With the callback, non-killed stores re-arm with delta=1 and continue.
        //
        // `this` is safe to capture: MozJSWasmBridge is non-movable and outlives `_store`.
        _store->epoch_deadline_callback(
            [this](wt::Store::Context, uint64_t& delta) -> wt::Result<wt::DeadlineKind> {
                if (isKillPending()) {
                    return wt::Error("wasm epoch interrupt: kill pending");
                }
#if defined(MONGO_CONFIG_DEBUG_BUILD)
                // Test-only: see wasmBridgeInjectEpochTrap. Fires only when a test both enables
                // the failpoint and calls _testTriggerEpochInterrupt() to bump the epoch.
                if (MONGO_unlikely(wasmBridgeInjectEpochTrap.shouldFail())) {
                    return wt::Error("injected non-kill wasm trap (wasmBridgeInjectEpochTrap)");
                }
#endif
                delta = 1;
                return wt::DeadlineKind::Continue;
            });

        // The default 128 MiB hostcall fuel cap is exhausted by long-running $accumulator /
        // mapReduce pipelines that pass multi-megabyte BSON state across WIT calls.
        // Disable it here; resource use is already bounded by the linear-memory limiter
        // (opts.linearMemoryLimitMB), internalQueryMaxJsEmitBytes, and BSON 16 MiB per object.
        storeCtx.set_hostcall_fuel(SIZE_MAX);

        wt::WasiConfig wasiConfig;
        wasiConfig.inherit_stdout();
        wasiConfig.inherit_stderr();
        invariant(storeCtx.set_wasi(std::move(wasiConfig)));

        // Use the per-context cached linker (already configured with add_wasip2).
        auto instanceResult = _ctx->_linker->instantiate(storeCtx, *_ctx->_component);
        if (!instanceResult) {
            uasserted(ErrorCodes::JSInterpreterFailure,
                      fmt::format("WASM component instantiation failed: {}",
                                  instanceResult.err().message()));
        }
        _instance = wc::Instance(instanceResult.ok());

        _initEngineFunc = _getFunc(kInitEngine);
        _shutdownEngineFunc = _getFunc(kShutdownEngine);
        _resetEngineFunc = _getFunc(kResetEngine);
        _createFunctionFunc = _getFunc(kCreateFunction);
        _invokeFunctionFunc = _getFunc(kInvokeFunction);
        _setGlobalFunc = _getFunc(kSetGlobal);
        _setGlobalValueFunc = _getFunc(kSetGlobalValue);
        _invokePredicateFunc = _getFunc(kInvokePredicate);
        _setupEmitFunc = _getFunc(kSetupEmit);
        _invokeMapFunc = _getFunc(kInvokeMap);
        _drainEmitBufferFunc = _getFunc(kDrainEmitBuffer);
        _getGlobalFunc = _getFunc(kGetGlobal);
        _getReturnValueBsonFunc = _getFunc(kGetReturnValueBson);

        // reset-realm is optional: older pre-built WASM modules may not export it.
        // Falls back gracefully to reset-engine when absent.
        invariant(_instance);
        _resetRealmFunc = wasm_helpers::getMozjsFuncOptional(
            *_instance, getContext(), kMozjsWitInterface, kResetRealm);

        // get-memory-stats is optional for the same reason; getMemoryStats() returns an
        // empty object when the module doesn't export it.
        _getMemoryStatsFunc = wasm_helpers::getMozjsFuncOptional(
            *_instance, getContext(), kMozjsWitInterface, kGetMemoryStats);
    } catch (...) {
        // Release the partially-built Store/Instance while we still hold lifecycleLock.
        _instance.reset();
        _store.reset();
        throw;
    }
}

MozJSWasmBridge::~MozJSWasmBridge() {
    {
        std::lock_guard<std::mutex> lifecycleLock(wasmLifecycleMutex());
        _instance.reset();
        _store.reset();
    }
    // Drop the shared context outside the lock: if this is the last reference,
    // ~WasmEngineContext takes the lock itself (the mutex is non-recursive).
    _ctx.reset();
}

void MozJSWasmBridge::_assertUsable() {
    uassert(ErrorCodes::JSInterpreterFailure,
            "WASM bridge is in a trapped state and cannot execute further",
            !hasTrapped());
    uassert(ErrorCodes::JSInterpreterFailure,
            "WASM bridge encountered an out-of-memory error and cannot execute further",
            !hasOomError());
    uassert(
        ErrorCodes::JSInterpreterFailure, "WASM bridge has not been initialized", isInitialized());
}

bool MozJSWasmBridge::_callFuncNoArgs(wc::Func& func, wc::Val* results, size_t numResults) {
    const wc::Val* emptyPtr = nullptr;
    wt::Span<const wc::Val> empty(emptyPtr, size_t{0});
    wt::Span<wc::Val> resultsSpan(results, numResults);
    wt::Result<std::monostate> callResult = func.call(getContext(), empty, resultsSpan);
    if (!callResult) {
        _throwAfterTrap(callResult.err().message());
    }
    auto postResult = func.post_return(getContext());
    return static_cast<bool>(postResult);
}

#if defined(MONGO_CONFIG_DEBUG_BUILD)
void MozJSWasmBridge::_testTriggerEpochInterrupt() {
    _ctx->_engine->increment_epoch();
}
#endif

void MozJSWasmBridge::_throwAfterTrap(const std::string& trapMessage) {
    _state.store(State::Trapped);

    // If kill() was called on this bridge, throw the reason recorded at that call -- the real
    // mongo error (MaxTimeMSExpired, CursorKilled, etc.) if the caller knew it (see kill()'s doc
    // comment), or plain Interrupted for the JS-fn-only-timeout case. No translation happens
    // above the bridge: by the time a trap reaches here, the correct code is already known.
    if (auto reason = _killReason.load(); reason != ErrorCodes::OK) {
        uasserted(reason, trapMessage);
    }

    // The ResourceLimiter callback sets _growDeniedByLimiter before memory.grow
    // returns -1 to the guest. SpiderMonkey then MOZ_CRASHes via `unreachable`,
    // which is what lands here. Snapshot the flag once so the classification and
    // the diagnostic log below agree on a single value.
    const bool memoryGrowDenied = _growDeniedByLimiter.load();
    const ErrorCodes::Error trapCode = memoryGrowDenied ? ErrorCodes::ExceededMemoryLimit
                                                        : ErrorCodes::Error{kWasmtimeTrapErrorCode};

    // Full state dump on any trap for diagnostics.
    int64_t ageNanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count() -
        _stats.createdAtMonoNanos;
    LOGV2_WARNING(11542382,
                  "WASM bridge trapped (no kill pending)",
                  "trap"_attr = trapMessage,
                  "trapCode"_attr = static_cast<int>(trapCode),
                  "memoryGrowDenied"_attr = memoryGrowDenied,
                  "lastGrowDesiredMB"_attr = _growDeniedDesiredBytes.load() / (1024 * 1024),
                  "state"_attr = static_cast<int>(_state.load()),
                  "heapLimitMB"_attr = _jsHeapLimitMB,
                  "storeLimitMB"_attr = _storeLimitMB,
                  "ageMillis"_attr = ageNanos / 1'000'000,
                  "stats"_attr = _stats.toBSON());
    uasserted(trapCode, trapMessage);
}

namespace {
inline void recordBytes(Atomic<uint64_t>& total, Atomic<uint64_t>& peak, uint64_t bytes) {
    total.fetchAndAdd(bytes);
    uint64_t prev = peak.load();
    while (bytes > prev && !peak.compareAndSwap(&prev, bytes)) {
    }
}
// For the raw-u8 fast path we get exact size; slow path falls back to 0.
inline uint64_t wcValByteSize(const wc::Val& v) {
    const auto* raw = wc::Val::to_capi(&v);
    if (raw->kind == WASMTIME_COMPONENT_RAW_U8_LIST) {
        return raw->of.raw_u8_list.size;
    }
    return 0;
}
}  // namespace

bool MozJSWasmBridge::initialize() {
    // Cold-start init runs freshly-built JIT; serialise it against lifecycle teardown.
    std::lock_guard<std::mutex> lifecycleLock(wasmLifecycleMutex());
    wc::Val result(wc::WitResult::ok(std::nullopt));
    LOGV2_DEBUG(11542332,
                2,
                "Wasm Bridge Initializing",
                "ok"_attr = isInitialized(),
                "heapLimitMB"_attr = _jsHeapLimitMB,
                "storeLimitMB"_attr = _storeLimitMB);
    wc::Val optionsArg(wc::Record({{"heap-size-mb", wc::Val(_jsHeapLimitMB)},
                                   {"javascript-protection", wc::Val(_javascriptProtection)}}));
    _callFunc(*_initEngineFunc, &result, 1, std::move(optionsArg));
    if (_assertWitResult(result)) {
        _state.store(State::Initialized);
    } else {
        LOGV2_DEBUG(11542356,
                    1,
                    "Wasm Bridge failed initialization",
                    "error"_attr =
                        wasm_helpers::translateMozJSError(*result.get_result().payload()));
    }
    LOGV2_DEBUG(11542331, 2, "Wasm Bridge Initialized", "ok"_attr = isInitialized());
    return isInitialized();
}

void MozJSWasmBridge::shutdown() {
    // Shutdown runs JIT (shutdown-engine); serialise it against lifecycle teardown.
    std::lock_guard<std::mutex> lifecycleLock(wasmLifecycleMutex());
    _assertUsable();
    wc::Val result(wc::WitResult::ok(std::nullopt));
    // Full state dump on shutdown to correlate per-bridge counters
    // with any trap warning (11542382) on a subsequent scope.
    int64_t ageNanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count() -
        _stats.createdAtMonoNanos;
    LOGV2_DEBUG(11542334,
                2,
                "Wasm Bridge Shutting Down",
                "ageMillis"_attr = ageNanos / 1'000'000,
                "stats"_attr = _stats.toBSON());
    _callFuncNoArgs(*_shutdownEngineFunc, &result, 1);
    _assertWitResult(result, "Wasm Bridge failed shutdown");
    LOGV2_DEBUG(11542333, 2, "Wasm Bridge Shutdown");
    _state.store(State::Uninitialized);
}

void MozJSWasmBridge::resetEngine() {
    _assertUsable();
    _refreshEpochDeadline();
    // Clear the memory-grow denial flag: it is latched by the limiter callback and
    // must not persist across reuse, or a later unrelated trap would be misclassified
    // as ExceededMemoryLimit. A denial that led to a trap discards the bridge (not
    // reused, see isHealthy()); this only matters if a denial is ever recovered from.
    _growDeniedByLimiter.store(false);
    _growDeniedDesiredBytes.store(0);
    const uint64_t seq = _stats.resetEngineCallCount.fetchAndAdd(1) + 1;
    LOGV2_DEBUG(11542374,
                2,
                "WASM bridge resetEngine",
                "seq"_attr = seq,
                "totalInvokes"_attr = _stats.invokeFunctionCallCount.load() +
                    _stats.invokeMapCallCount.load() + _stats.invokePredicateCallCount.load(),
                "bytesInToWasm"_attr = _stats.bytesInToWasm.load());
    wc::Val result(wc::WitResult::ok(std::nullopt));
    _callFuncNoArgs(*_resetEngineFunc, &result, 1);
    _assertWitResult(result, "Wasm Bridge failed reset-engine");
}

void MozJSWasmBridge::resetRealm() {
    _assertUsable();
    _refreshEpochDeadline();
    // See resetEngine(): clear the latched memory-grow denial flag on reuse.
    _growDeniedByLimiter.store(false);
    _growDeniedDesiredBytes.store(0);
    if (!_resetRealmFunc) {
        // Older WASM module without reset-realm: fall back to reset-engine.
        resetEngine();
        return;
    }
    const uint64_t seq = _stats.resetRealmCallCount.fetchAndAdd(1) + 1;
    LOGV2_DEBUG(11542375,
                2,
                "WASM bridge resetRealm",
                "seq"_attr = seq,
                "totalInvokes"_attr = _stats.invokeFunctionCallCount.load() +
                    _stats.invokeMapCallCount.load() + _stats.invokePredicateCallCount.load(),
                "bytesInToWasm"_attr = _stats.bytesInToWasm.load());
    wc::Val result(wc::WitResult::ok(std::nullopt));
    _callFuncNoArgs(*_resetRealmFunc, &result, 1);
    _assertWitResult(result, "Wasm Bridge failed reset-realm");
}

uint64_t MozJSWasmBridge::createFunction(std::string_view source) {
    _assertUsable();
    _stats.createFunctionCallCount.fetchAndAdd(1);
    wc::Val srcArg(wasm_helpers::makeListU8(source));
    wc::Val result(wc::WitResult::ok(std::nullopt));
    uassert(11542310,
            str::stream() << "Failed to call to create JS function " << std::string(source),
            _callFunc(*_createFunctionFunc, &result, 1, std::move(srcArg)));
    _assertWitResult(result,
                     str::stream()
                         << "Failed to create JS function :: source = " << std::string(source));
    const wc::Val* payload = result.get_result().payload();
    invariant(payload && payload->is_u64() && payload->get_u64());
    LOGV2_DEBUG(11542330,
                2,
                "Wasm Bridge Created function",
                "number"_attr = payload->get_u64(),
                "source"_attr = source);
    return payload->get_u64();
}

StatusWith<BSONObj> MozJSWasmBridge::invokeFunction(uint64_t handle,
                                                    wc::Val bsonVal,
                                                    bool ignoreReturn) {
    _assertUsable();
    _stats.invokeFunctionCallCount.fetchAndAdd(1);
    recordBytes(_stats.bytesInToWasm, _stats.maxSingleCallBytesIn, wcValByteSize(bsonVal));
    wc::Val arg0(handle);
    wc::Val result(wc::WitResult::ok(std::nullopt));
    if (!_callFunc(*_invokeFunctionFunc,
                   &result,
                   1,
                   std::move(arg0),
                   std::move(bsonVal),
                   wc::Val(ignoreReturn))) {
        return Status{ErrorCodes::Error{11542313},
                      str::stream() << "Failed to call to invoke JS function number " << handle};
    }
    _assertWitResult(result,
                     str::stream() << "Failed to invoke JS function :: function id = " << handle);
    if (ignoreReturn)
        return BSONObj();
    auto extracted = _extractBSON(result);
    recordBytes(_stats.bytesOutFromWasm,
                _stats.maxSingleCallBytesOut,
                static_cast<uint64_t>(extracted.objsize()));
    return extracted;
}

void MozJSWasmBridge::setGlobal(std::string_view name, const BSONObj& value) {
    _assertUsable();
    _stats.setGlobalCallCount.fetchAndAdd(1);
    recordBytes(
        _stats.bytesInToWasm, _stats.maxSingleCallBytesIn, static_cast<uint64_t>(value.objsize()));
    wc::Val nameArg = wasm_helpers::makeString(name);
    wc::Val valueArg = wasm_helpers::convertBsonToWcVal(value);
    wc::Val result(wc::WitResult::ok(std::nullopt));
    uassert(11542312,
            str::stream() << "Failed to call to set global JS variable " << std::string(name),
            _callFunc(*_setGlobalFunc, &result, 1, std::move(nameArg), std::move(valueArg)));
    _assertWitResult(result,
                     str::stream()
                         << "Failed to set global JS variable :: name = " << std::string(name),
                     ErrorCodes::Error{11542300});
}

void MozJSWasmBridge::setGlobalValue(std::string_view name, const BSONObj& value) {
    _assertUsable();
    invariant(value.nFields() == 1);
    _stats.setGlobalCallCount.fetchAndAdd(1);
    recordBytes(
        _stats.bytesInToWasm, _stats.maxSingleCallBytesIn, static_cast<uint64_t>(value.objsize()));
    wc::Val nameArg = wasm_helpers::makeString(name);
    wc::Val valueArg = wasm_helpers::convertBsonToWcVal(value);
    wc::Val result(wc::WitResult::ok(std::nullopt));
    uassert(11542316,
            str::stream() << "Failed to call to set global JS value variable " << std::string(name),
            _callFunc(*_setGlobalValueFunc, &result, 1, std::move(nameArg), std::move(valueArg)));
    _assertWitResult(result,
                     str::stream() << "Failed to set global JS value variable :: name = "
                                   << std::string(name),
                     ErrorCodes::Error{11542317});
}

void MozJSWasmBridge::setupEmit(boost::optional<int64_t> byteLimit) {
    _assertUsable();
    // Every setupEmit allocates ~117 MB of WASM linear memory. Log the
    // count so per-document regressions are detectable from logs alone.
    const uint64_t seq = _stats.setupEmitCallCount.fetchAndAdd(1) + 1;
    if (byteLimit) {
        int64_t prev = _stats.maxEmitByteLimit.load();
        while (*byteLimit > prev && !_stats.maxEmitByteLimit.compareAndSwap(&prev, *byteLimit)) {
        }
    }
    LOGV2_DEBUG(11542370,
                2,
                "WASM bridge setupEmit",
                "seq"_attr = seq,
                "byteLimit"_attr = byteLimit ? *byteLimit : -1,
                "maxEmitByteLimit"_attr = _stats.maxEmitByteLimit.load(),
                "heapLimitMB"_attr = _jsHeapLimitMB,
                "storeLimitMB"_attr = _storeLimitMB,
                "totalInvokes"_attr = _stats.invokeFunctionCallCount.load() +
                    _stats.invokeMapCallCount.load() + _stats.invokePredicateCallCount.load(),
                "bytesInToWasm"_attr = _stats.bytesInToWasm.load(),
                "bytesOutFromWasm"_attr = _stats.bytesOutFromWasm.load());
    wc::Val result(wc::WitResult::ok(std::nullopt));
    std::optional<int64_t> stdArg = std::nullopt;  // NOLINT
    if (byteLimit) {
        stdArg = *byteLimit;
    }
    wc::Val arg = wc::WitOption(stdArg);
    _callFunc(*_setupEmitFunc, &result, 1, std::move(arg));
    _assertWitResult(result, "Wasm Bridge failed to setup-emit");
}

void MozJSWasmBridge::invokeMap(uint64_t handle, wc::Val bsonVal) {
    _assertUsable();
    const uint64_t seq = _stats.invokeMapCallCount.fetchAndAdd(1) + 1;
    recordBytes(_stats.bytesInToWasm, _stats.maxSingleCallBytesIn, wcValByteSize(bsonVal));
    LOGV2_DEBUG(11542371,
                4,
                "WASM bridge invokeMap",
                "seq"_attr = seq,
                "func"_attr = handle,
                "bytesIn"_attr = wcValByteSize(bsonVal),
                "setupEmitCallCount"_attr = _stats.setupEmitCallCount.load());
    wc::Val arg0(handle);
    wc::Val result(wc::WitResult::ok(std::nullopt));
    uassert(11542319,
            str::stream() << "Failed to call to invoke JS function number " << handle,
            _callFunc(*_invokeMapFunc, &result, 1, std::move(arg0), std::move(bsonVal)));
    _assertWitResult(result,
                     str::stream() << "Failed to invoke JS function :: function id = " << handle);
}

bool MozJSWasmBridge::invokePredicate(uint64_t handle, wc::Val bsonVal) {
    _assertUsable();
    _stats.invokePredicateCallCount.fetchAndAdd(1);
    recordBytes(_stats.bytesInToWasm, _stats.maxSingleCallBytesIn, wcValByteSize(bsonVal));
    wc::Val arg0(handle);
    wc::Val result(wc::WitResult::ok(std::nullopt));
    uassert(11542339,
            str::stream() << "Failed to call to invoke JS function number " << handle,
            _callFunc(*_invokePredicateFunc, &result, 1, std::move(arg0), std::move(bsonVal)));
    _assertWitResult(result,
                     str::stream() << "Failed to invoke JS predicate :: function id = " << handle);
    const wc::Val* payload = result.get_result().payload();
    invariant(payload && payload->is_bool());
    return payload->get_bool();
}

void MozJSWasmBridge::kill(ErrorCodes::Error reason) {
    invariant(reason != ErrorCodes::OK, "kill() reason must not be OK -- that means 'not killed'");
    _signalInterrupt(reason);
}

void MozJSWasmBridge::_signalInterrupt(ErrorCodes::Error reason) {
    // First write wins: kill() can legitimately be called more than once on the same bridge --
    // notably DeadlineMonitor's own background thread periodically re-signals any task with
    // isKillPending() still set (see deadlineMonitorThread()'s re-arm loop), as a safety net for
    // kills the target hasn't yet noticed. That re-signal always goes through the plain,
    // reason-less kill() (defaulting to Interrupted), so if we stored unconditionally here, a
    // correctly-attributed first kill (e.g. registerOperation() forwarding a real
    // MaxTimeMSExpired) could be silently overwritten with a generic Interrupted moments later.
    // Only the reason recorded on the *first* call is authoritative; every call still bumps the
    // epoch so the re-arm's actual job (making sure the trap fires) is unaffected.
    auto expected = ErrorCodes::OK;
    _killReason.compareAndSwap(&expected, reason);
    _stats.killCount.fetchAndAdd(1);
    _ctx->_engine->increment_epoch();
    LOGV2(11542376,
          "WASM bridge kill signalled",
          "reason"_attr = reason,
          "recordedReason"_attr = _killReason.load(),
          "killCount"_attr = _stats.killCount.load(),
          "totalInvokes"_attr = _stats.invokeFunctionCallCount.load() +
              _stats.invokeMapCallCount.load() + _stats.invokePredicateCallCount.load(),
          "setupEmitCalls"_attr = _stats.setupEmitCallCount.load());
}

bool MozJSWasmBridge::isKillPending() const {
    return _killReason.load() != ErrorCodes::OK;
}

BSONObj MozJSWasmBridge::drainEmitBuffer() {
    _assertUsable();
    _stats.drainEmitBufferCallCount.fetchAndAdd(1);
    wc::Val result(wc::WitResult::ok(std::nullopt));
    _callFuncNoArgs(*_drainEmitBufferFunc, &result, 1);
    _assertWitResult(result, "Wasm Bridge failed to drain emit");
    auto extracted = _extractBSON(result);
    recordBytes(_stats.bytesOutFromWasm,
                _stats.maxSingleCallBytesOut,
                static_cast<uint64_t>(extracted.objsize()));
    return extracted;
}

BSONObj MozJSWasmBridge::getGlobal(std::string_view name, bool implicitNull) {
    _assertUsable();
    wc::Val nameArg = wasm_helpers::makeString(name);
    wc::Val result(wc::WitResult::ok(std::nullopt));
    uassert(11542304,
            str::stream() << "Failed to call to get global JS variable " << std::string(name),
            _callFunc(*_getGlobalFunc, &result, 1, std::move(nameArg)));

    auto isInvalidArg = [](const wc::Val& result) -> bool {
        if (wasm_helpers::isResultOk(result) || !result.get_result().payload())
            return false;

        return wasm_helpers::findField("code", result.get_result().payload()->get_record())
                   ->get_enum() == "e-invalid-arg";
    };

    // Implicitly return null if return value is unset
    if (implicitNull && isInvalidArg(result)) {
        BSONObjBuilder b;
        b.appendNull("__value");
        return b.obj();
    }
    _assertWitResult(result,
                     str::stream()
                         << "Failed to get global JS variable :: name = " << std::string(name),
                     ErrorCodes::Error{11542301});
    return _extractBSON(result);
}

bool MozJSWasmBridge::_assertWitResult(const wc::Val& result,
                                       std::string errorPrefix,
                                       ErrorCodes::Error code) {
    if (wasm_helpers::isResultOk(result))
        return true;
    const wc::Val* payload = result.get_result().payload();
    if (!payload) {
        return false;
    }

    if (wasm_helpers::isFatalWitError(*payload)) {
        _state.store(State::Trapped);
    } else if (wasm_helpers::isOomWitError(*payload)) {
        _state.store(State::OOM);
    }

    if (auto mongoCode = wasm_helpers::mozJSErrorCode(*payload);
        mongoCode != ErrorCodes::JSInterpreterFailure) {
        code = mongoCode;
    }

    if (!errorPrefix.empty()) {
        uasserted(code,
                  str::stream() << errorPrefix << ": "
                                << wasm_helpers::translateMozJSError(*payload));
    }
    return false;
}

wc::Func MozJSWasmBridge::_getFunc(std::string_view funcName) {
    invariant(_instance);
    return wasm_helpers::getMozjsFunc(*(_instance), getContext(), "mongo:mozjs/mozjs", funcName);
}

BSONObj MozJSWasmBridge::_extractBSON(const wc::Val& result) {
    const wc::Val* payload = result.get_result().payload();
    invariant(payload);

    // Fast path: with the MongoDB lift patch applied, list<u8> results arrive as
    // WASMTIME_COMPONENT_RAW_U8_LIST so we can memcpy the entire BSON in one go
    // rather than reading N×32-byte Val::U8 boxes.
    const auto* rawPayload = wc::Val::to_capi(payload);
    if (rawPayload->kind == WASMTIME_COMPONENT_RAW_U8_LIST) {
        const auto& bv = rawPayload->of.raw_u8_list;
        // The guest is untrusted: fully validate the bytes against the actual buffer size before
        // any downstream reader walks the document.
        return wasm_helpers::validatedBsonFromGuestBytes(reinterpret_cast<const uint8_t*>(bv.data),
                                                         bv.size);
    }

    const wc::List& list = payload->get_list();
    size_t n = list.size();
    // Write directly into the SharedBuffer — no intermediate std::vector allocation — then hand it
    // off for validation + ownership. The guest is untrusted, so validatedBsonFromGuestBuffer
    // checks the embedded lengths against the actual buffer size rather than trusting the forgeable
    // BSON header.
    auto buf = SharedBuffer::allocate(n);
    auto* dst = reinterpret_cast<uint8_t*>(buf.get());
    for (const wc::Val& elem : list) {
        *dst++ = elem.get_u8();
    }
    return wasm_helpers::validatedBsonFromGuestBuffer(std::move(buf), n);
}

BSONObj MozJSWasmBridge::_getReturnValueBson() {
    // getGlobal does not preserve JS array types (arrays become BSON objects with numeric keys).
    // Use getReturnValueWrapped() when array type preservation matters.
    return getGlobal(kReturnValue, true);
}

BSONObj MozJSWasmBridge::getMemoryStats() {
    _assertUsable();
    if (!_getMemoryStatsFunc) {
        return BSONObj();
    }
    wc::Val result(wc::WitResult::ok(std::nullopt));
    uassert(11542383,
            "Failed to call get-memory-stats",
            _callFuncNoArgs(*_getMemoryStatsFunc, &result, 1));
    _assertWitResult(result, "Failed to get memory stats", ErrorCodes::Error{11542384});
    auto stats = _extractBSON(result);
    return stats;
}

BSONObj MozJSWasmBridge::getReturnValueWrapped() {
    _assertUsable();
    wc::Val result(wc::WitResult::ok(std::nullopt));
    uassert(11542350,
            "Failed to call get-return-value-bson",
            _callFuncNoArgs(*_getReturnValueBsonFunc, &result, 1));
    _assertWitResult(result, "Failed to get return value BSON", ErrorCodes::Error{11542351});
    return _extractBSON(result);
}

}  // namespace mongo::mozjs::wasm

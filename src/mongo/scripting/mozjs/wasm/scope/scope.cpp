// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/scripting/mozjs/wasm/scope/scope.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/query_execution_knobs_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/scripting/config_engine_gen.h"
#include "mongo/scripting/config_engine_wasm_gen.h"
#include "mongo/scripting/deadline_monitor.h"
#include "mongo/scripting/mozjs/wasm/wasmtime_engine.h"
#include "mongo/util/str.h"

#include <string_view>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo::mozjs {

const std::string kReturnValueField = "__value";
constexpr std::string_view kReturnValueGlobal = "__returnValue";

WasmtimeImplScope::WasmtimeImplScope(std::shared_ptr<wasm::WasmEngineContext> wasmEngineCtx,
                                     boost::optional<int> jsHeapLimitMB)
    : _wasmEngineCtx(std::move(wasmEngineCtx)), _jsHeapLimitMB(jsHeapLimitMB) {
    init(nullptr);
}

WasmtimeImplScope::WasmtimeImplScope(std::shared_ptr<wasm::WasmEngineContext> wasmEngineCtx,
                                     boost::optional<int> jsHeapLimitMB,
                                     std::unique_ptr<wasm::MozJSWasmBridge> idleBridge,
                                     FunctionCacheMap /*cachedFunctions*/)
    : _wasmEngineCtx(std::move(wasmEngineCtx)),
      _jsHeapLimitMB(jsHeapLimitMB),
      _bridge(std::move(idleBridge)) {
    // Do NOT pre-populate _cachedFunctions here. init(useNewWasmRealm=true) calls
    // resetRealm() which invalidates all compiled function handles — any cache stored now
    // would be cleared by the useNewWasmRealm branch inside init(). Handles from the old
    // realm are stale after resetRealm() and cannot be reused.
    init(nullptr, /*useNewWasmRealm=*/true);
}

WasmtimeImplScope::~WasmtimeImplScope() {
    // Must unregister before _bridge is destroyed, otherwise a concurrent interrupt() reading the
    // OperationContext decoration could dereference a freed _bridge. This is because
    // unregisterOperation() takes the Client lock, serialising with respect to interrupt().
    unregisterOperation();

    LOGV2_DEBUG(11542368,
                2,
                "WASM scope destroying",
                "opId"_attr = _currentOpId(),
                "bridgePresent"_attr = static_cast<bool>(_bridge),
                "initialised"_attr = _bridge && _bridge->isInitialized(),
                "trapped"_attr = _bridge && _bridge->hasTrapped(),
                "oom"_attr = _bridge && _bridge->hasOomError(),
                "killPending"_attr = _bridge && _bridge->isKillPending(),
                "emitSetupBytes"_attr = _emitSetupBytes,
                "invokeCount"_attr = _invokeSeq);

    // Park a healthy bridge for reuse on this thread to avoid the ~30 ms WASM instantiation
    // + SpiderMonkey init cost on the next request.
    //
    // Do NOT park a bridge that has ever called setupEmit. Each setupEmit
    // allocates ~117 MB of WASM linear memory that cannot be released — WASM linear memory
    // only grows, and resetRealm() does not return the underlying pages. Reusing a parked
    // emit-configured bridge would call setupEmit() again, accumulating another ~117 MB.
    // After enough MapReduce operations on the same thread, cumulative linear memory exceeds
    // the store cap, causing a CannotLeaveComponent trap. Non-MR bridges still get parked.
    if (_bridge && _bridge->isHealthy() && !_bridge->wasEmitConfigured()) {
        if (auto* engine = dynamic_cast<WasmtimeScriptEngine*>(getGlobalScriptEngine())) {
            engine->parkBridgeForCurrentThread(
                std::move(_bridge), std::move(_wasmEngineCtx), std::move(_cachedFunctions));
            return;
        }
    }

    // Skip shutdown() when kill was pending — increment_epoch() may cause the WIT
    // shutdown call to trap if WASM is mid-execution.
    if (_bridge && _bridge->isInitialized() && !_bridge->isKillPending()) {
        _bridge->shutdown();
    }
}

void WasmtimeImplScope::reset() {
    // A live scope always holds its shared engine context: getWasmEngineContext() memoizes and
    // returns the process-wide context, and it is only cleared when the scope is parked/destroyed.
    // reset() must never rebuild it (a no-op now that the context is shared).
    invariant(_wasmEngineCtx);

    // Clear the decoration under the Client lock before tearing down _bridge, so a racing
    // interrupt() cannot call kill() on a dangling bridge.
    unregisterOperation();

    const bool wasKillPending = _bridge && _bridge->isKillPending();
    const bool bridgeHealthy = _bridge && _bridge->isHealthy();

    LOGV2_DEBUG(11542369,
                2,
                "WASM scope reset",
                "opId"_attr = _currentOpId(),
                "bridgeHealthy"_attr = bridgeHealthy,
                "killPending"_attr = wasKillPending,
                "trapped"_attr = _bridge && _bridge->hasTrapped(),
                "oom"_attr = _bridge && _bridge->hasOomError(),
                "emitSetupBytes"_attr = _emitSetupBytes,
                "invokeCount"_attr = _invokeSeq);

    if (!bridgeHealthy) {
        // Skip shutdown() when kill was pending — increment_epoch() may cause the WIT
        // shutdown call to trap if WASM is mid-execution.
        if (_bridge && _bridge->isInitialized() && !wasKillPending) {
            _bridge->shutdown();
        }
        _bridge = nullptr;
        _emitSetupBytes = 0;
        // Handles from the old bridge are invalid after recreation — must recompile.
        _cachedFunctions.clear();
    }
    init(nullptr);
    _emitCallback = nullptr;
    _emitCallbackData = nullptr;
    _lastReturnValue = BSONObj();
}

void WasmtimeImplScope::init(const BSONObj* data) {
    init(data, /*useNewWasmRealm=*/false);
}

void WasmtimeImplScope::init(const BSONObj* data, bool useNewWasmRealm) {
    const uint32_t storeLimit = static_cast<uint32_t>(gWasmtimeStoreMemoryLimitMB.load());
    // Use gJSHeapLimitMB as the default heap limit, matching the bridge constructor. Using
    // storeLimit here would cause the fast-path check to always fail (bridge->getHeapLimitMB()
    // returns gJSHeapLimitMB, not storeLimit).
    const uint32_t heapLimit = _jsHeapLimitMB ? static_cast<uint32_t>(*_jsHeapLimitMB)
                                              : static_cast<uint32_t>(gJSHeapLimitMB.load());
    if (_bridge && _bridge->isHealthy() && _bridge->getStoreLimitMB() == storeLimit &&
        _bridge->getHeapLimitMB() == heapLimit) {
        // Conservative: can't observe whether resetEngine() preserves the WASM-side
        // emit buffer, so always invalidate to ensure re-issue on the next injectNative().
        _emitSetupBytes = 0;
        _loadedVersion = 0;
        if (useNewWasmRealm) {
            _bridge->resetRealm();
            _cachedFunctions.clear();
        } else {
            _bridge->resetEngine();
        }
        _storeLinearMemBytes = static_cast<int64_t>(storeLimit) * 1024 * 1024;
        if (data) {
            BSONObjIterator i(*data);
            while (i.more()) {
                BSONElement e = i.next();
                BSONObjBuilder bob;
                bob.appendAs(e, "");
                _bridge->setGlobalValue(e.fieldName(), bob.obj());
            }
        }
        return;
    }

    // Slow path: full bridge teardown and recreation. Handles are invalid after teardown.
    if (_bridge && _bridge->isInitialized()) {
        _bridge->shutdown();
    }
    _cachedFunctions.clear();
    wasm::MozJSWasmBridge::Options opts{};
    if (_jsHeapLimitMB) {
        opts.jsHeapLimitMB = static_cast<uint32_t>(*_jsHeapLimitMB);
    }
    opts.linearMemoryLimitMB = gWasmtimeStoreMemoryLimitMB.load();
    _storeLinearMemBytes = static_cast<int64_t>(opts.linearMemoryLimitMB) * 1024 * 1024;
    opts.javascriptProtection = gJavascriptProtection.load();
    _bridge = std::make_unique<wasm::MozJSWasmBridge>(_wasmEngineCtx, opts);
    bool initialized = _bridge->initialize();
    uassert(ErrorCodes::BadValue, "MozJS WASM bridge failed to initialize", initialized);

    _installHelpers();
    if (data) {
        BSONObjIterator i(*data);
        while (i.more()) {
            BSONElement e = i.next();
            BSONObjBuilder bob;
            bob.appendAs(e, "");
            _bridge->setGlobalValue(e.fieldName(), bob.obj());
        }
    }
}

bool WasmtimeImplScope::execPredicate(ScriptingFunction func, const BSONObj& doc, int timeoutMs) {
    // Single WASM crossing instead of 3 (setObject + setBoolean + invoke).
    // obj/fullObject are set on the WASM side inside invokePredicate() — no extra host calls.
    auto bsonVal = wasm::wasm_helpers::convertBsonToWcVal(doc);
    return _invokeWithDeadlineMonitoring(
        timeoutMs, [&] { return _bridge->invokePredicate(func, std::move(bsonVal)); });
}

ScriptingFunction WasmtimeImplScope::_createFunction(const char* raw) {
    // Check the per-scope cache first.  Compiled function handles survive resetEngine() (the
    // per-document fast-path reset), so the cache is valid across pool round-trips as long as
    // no resetRealm() has occurred (_cachedFunctions is cleared there).  Skipping recompilation
    // eliminates the dominant cost of repeated $where queries on the same scope.
    std::string code(raw);
    auto [it, inserted] = _cachedFunctions.emplace(code, ScriptingFunction{});
    if (!inserted) {
        return it->second;
    }
    it->second = _bridge->createFunction(std::string_view(code));
    return it->second;
}

int WasmtimeImplScope::invoke(ScriptingFunction func,
                              const BSONObj* args,
                              const BSONObj* recv,
                              int timeoutMs,
                              bool ignoreReturn,
                              bool readOnlyArgs,
                              bool readOnlyRecv) {
    // TODO(SERVER-122738): readOnlyArgs and readOnlyRecv are silently ignored here. In the MozJS
    // implementation these flags cause SpiderMonkey to freeze the corresponding JS objects so that
    // JavaScript code cannot mutate them during execution.

    // Convert BSON to a wc::Val before starting the deadline so the O(N) host-side work is
    // not charged against the JS function timeout.
    const BSONObj& bsonArg =
        (recv && !recv->isEmpty()) ? *recv : (args ? *args : BSONObj::kEmptyObject);
    auto bsonVal = wasm::wasm_helpers::convertBsonToWcVal(bsonArg);

    const uint64_t seq = ++_invokeSeq;
    const uint64_t opId = _currentOpId();

    if (recv && !recv->isEmpty()) {
        if (_emitCallback) {
            uassert(ErrorCodes::BadValue,
                    "emit() cannot be used in a function that returns a value",
                    ignoreReturn);
            LOGV2_DEBUG(11542363,
                        3,
                        "WASM scope invoking map function",
                        "invokeSeq"_attr = seq,
                        "opId"_attr = opId,
                        "func"_attr = static_cast<uint64_t>(func),
                        "emitSetupBytes"_attr = _emitSetupBytes,
                        "argSize"_attr = bsonArg.objsize());
            _invokeWithDeadlineMonitoring(timeoutMs,
                                          [&] { _bridge->invokeMap(func, std::move(bsonVal)); });
            _drainEmitToCallback();
            return 0;
        }

        LOGV2_DEBUG(11542364,
                    3,
                    "WASM scope invoking predicate function",
                    "invokeSeq"_attr = seq,
                    "opId"_attr = opId,
                    "func"_attr = static_cast<uint64_t>(func),
                    "argSize"_attr = bsonArg.objsize());
        bool predicateResult;
        predicateResult = _invokeWithDeadlineMonitoring(
            timeoutMs, [&] { return _bridge->invokePredicate(func, std::move(bsonVal)); });
        if (!ignoreReturn) {
            _lastReturnValue = BSON(kReturnValueField << predicateResult);
        }
        return 0;
    }
    LOGV2_DEBUG(11542365,
                3,
                "WASM scope invoking function",
                "invokeSeq"_attr = seq,
                "opId"_attr = opId,
                "func"_attr = static_cast<uint64_t>(func),
                "argSize"_attr = bsonArg.objsize(),
                "ignoreReturn"_attr = ignoreReturn);
    StatusWith<BSONObj> result{BSONObj{}};
    result = _invokeWithDeadlineMonitoring(
        timeoutMs, [&] { return _bridge->invokeFunction(func, std::move(bsonVal), ignoreReturn); });
    uassertStatusOK(result.getStatus());
    // Detect any stale emit() calls made during this invocation. In the invokeMap path,
    // _drainEmitToCallback() is called explicitly. For regular invocations the buffer
    // should be empty; draining here will throw (via the callback) if emit() was misused.
    _drainEmitToCallback();
    if (!ignoreReturn) {
        BSONObj wrapped = result.getValue();
        BSONElement retVal = wrapped["__returnValue"];
        if (retVal.ok()) {
            BSONObjBuilder bob;
            bob.appendAs(retVal, kReturnValueField);
            _lastReturnValue = bob.obj();
        } else {
            _lastReturnValue = BSONObj();
        }
    }
    return 0;
}

void WasmtimeImplScope::injectNative(const char* field, NativeFunction func, void* data) {
    // For simplicity, we only support a single native injection point for now, which is used by
    // MapReduce to implement the emit() function. If we need more injection points in the future,
    // we can extend this by adding a registration mechanism for named native functions.
    uassert(ErrorCodes::BadValue,
            "Only 'emit' function can be injected",
            std::string_view(field) == "emit");

    // injectNative is called exactly once per $_internalJsEmit evaluation. The old pattern
    // of calling it twice (once to set up, once with data=nullptr to invalidate a stack
    // pointer) no longer applies: emitFromJS reads state via EmitStateGuard::get() instead.
    // Re-injection with the same function is allowed (scope reuse across evaluations);
    // injecting a different function is always a bug.
    tassert(9712402,
            "injectNative(emit) must not replace an active emit callback with a different function",
            !_emitCallback || _emitCallback == func);

    _emitCallback = func;
    _emitCallbackData = data;

    // Margin lets WASM buffer one over-limit doc so the host's EmitState sees
    // it during drain and can throw, instead of WASM silently dropping it.
    const int64_t emitBufBytes =
        static_cast<int64_t>(internalQueryMaxJsEmitBytes.load()) + BSONObjMaxInternalSize;
    uassert(ErrorCodes::BadValue,
            "internalQueryMaxJsEmitBytes exceeds wasmtimeStoreMemoryLimitMB: the emit buffer "
            "must fit within the WASM store's linear memory",
            emitBufBytes <= _storeLinearMemBytes);

    // setupEmit() is logically idempotent for a given byte limit but
    // allocates ~117 MB of WASM linear memory each call. Cache to call it at most once per
    // (bridge, byte-limit) tuple. _emitSetupBytes is reset to 0 on every path that discards
    // or resets the bridge so a new bridge always re-initialises its emit buffer.
    if (_emitSetupBytes == emitBufBytes) {
        LOGV2_DEBUG(11542366,
                    4,
                    "WASM scope reusing existing emit setup",
                    "opId"_attr = _currentOpId(),
                    "emitBufBytes"_attr = emitBufBytes);
        return;
    }

    // INFO so each setupEmit (~117 MB allocation) is recorded in CI logs.
    // Expected: ~1 per request. More = regression; zero before invokeMap = cache bug.
    LOGV2(11542367,
          "WASM scope calling setupEmit",
          "opId"_attr = _currentOpId(),
          "previousEmitSetupBytes"_attr = _emitSetupBytes,
          "newEmitSetupBytes"_attr = emitBufBytes,
          "storeLinearMemBytes"_attr = _storeLinearMemBytes);
    _bridge->setupEmit(emitBufBytes);
    _emitSetupBytes = emitBufBytes;
}

BSONObj WasmtimeImplScope::_resolveGlobal(const char* field) const {
    if (std::string_view(field) == kReturnValueGlobal)
        return _lastReturnValue;
    return _bridge->getGlobal(field);
}

BSONObj WasmtimeImplScope::getObject(const char* field) {
    BSONObj result = _resolveGlobal(field);
    BSONElement val = result[kReturnValueField];
    // invoke-function results are wrapped as {"__value": obj}; globals set via setObject()
    // are stored raw. Scope::append() calls getObject() for both BSONType::object and
    // BSONType::array, so both must be unwrapped.
    if (val.type() == BSONType::object || val.type() == BSONType::array) {
        // val.Obj() is a view into result's SharedBuffer. Return an owned copy: the next
        // invoke() replaces _lastReturnValue, freeing the buffer and leaving a dangling ptr.
        return val.Obj().getOwned();
    }
    return result;
}

std::string WasmtimeImplScope::getString(const char* field) {
    BSONObj obj = _resolveGlobal(field);
    return std::string(obj[kReturnValueField].valueStringData());
}

bool WasmtimeImplScope::getBoolean(const char* field) {
    BSONObj obj = _resolveGlobal(field);
    return obj[kReturnValueField].boolean();
}

double WasmtimeImplScope::getNumber(const char* field) {
    BSONObj obj = _resolveGlobal(field);
    return obj[kReturnValueField].numberDouble();
}

int WasmtimeImplScope::getNumberInt(const char* field) {
    BSONObj obj = _resolveGlobal(field);
    return obj[kReturnValueField].numberInt();
}

long long WasmtimeImplScope::getNumberLongLong(const char* field) {
    BSONObj obj = _resolveGlobal(field);
    BSONElement elem = obj[kReturnValueField];
    // Scope::append() calls getNumberLongLong() for BSONType::date expecting millis-since-epoch.
    // BSONElement::numberLong() returns 0 for non-numeric types, so without this branch
    // every Date return value becomes epoch 0.
    if (elem.type() == BSONType::date) {
        return elem.Date().toMillisSinceEpoch();
    }
    return elem.numberLong();
}

Decimal128 WasmtimeImplScope::getNumberDecimal(const char* field) {
    BSONObj obj = _resolveGlobal(field);
    return obj[kReturnValueField].numberDecimal();
}

OID WasmtimeImplScope::getOID(const char* field) {
    BSONObj obj = _resolveGlobal(field);
    return obj[kReturnValueField].OID();
}

void WasmtimeImplScope::getBinData(const char* field,
                                   std::function<void(const BSONBinData&)> withBinData) {
    BSONObj obj = _resolveGlobal(field);
    auto jsBinData = obj[kReturnValueField];
    int len = 0;
    auto binData = jsBinData.binData(len);
    auto subType = jsBinData.binDataType();
    withBinData(BSONBinData(binData, len, static_cast<BinDataType>(static_cast<int>(subType))));
}

Timestamp WasmtimeImplScope::getTimestamp(const char* field) {
    BSONObj obj = _resolveGlobal(field);
    return obj[kReturnValueField].timestamp();
}

JSRegEx WasmtimeImplScope::getRegEx(const char* field) {
    BSONObj obj = _resolveGlobal(field);
    BSONElement elem = obj[kReturnValueField];
    uassert(ErrorCodes::TypeMismatch,
            str::stream() << "field " << field << " is not a regex",
            elem.type() == BSONType::regEx);
    return JSRegEx{elem.regex(), elem.regexFlags()};
}

void WasmtimeImplScope::setElement(const char* field, const BSONElement& e, const BSONObj& parent) {
    BSONObjBuilder bob;
    bob.appendAs(e, "");
    _bridge->setGlobalValue(field, bob.obj());
}

void WasmtimeImplScope::setNumber(const char* field, double val) {
    _bridge->setGlobalValue(field, BSON("" << val));
}

void WasmtimeImplScope::setString(const char* field, std::string_view val) {
    _bridge->setGlobalValue(field, BSON("" << val));
}

void WasmtimeImplScope::setObject(const char* field, const BSONObj& obj, bool readOnly) {
    // readOnly is silently ignored: the WASM engine does not support freezing objects.
    _bridge->setGlobalValue(field, BSON("" << obj));
}

void WasmtimeImplScope::setBoolean(const char* field, bool val) {
    _bridge->setGlobalValue(field, BSON("" << val));
}

void WasmtimeImplScope::setFunction(const char* field, const char* code) {
    // BSONType::code triggers newFunction in ValueReader, which calls
    // __parseJSFunctionOrExpression — mirroring MozJSImplScope::setFunction behaviour.
    _bridge->setGlobalValue(field, BSON("" << BSONCode(code)));
}

int WasmtimeImplScope::type(const char* field) {
    BSONObj result = _resolveGlobal(field);
    BSONElement val = result[kReturnValueField];
    if (val.ok()) {
        return static_cast<int>(val.type());
    }
    // Globals set via setObject/setGlobal are stored without a __value wrapper.
    // Use the first field's type, or treat an empty result as object (matching MozJS).
    BSONElement first = result.firstElement();
    return first.ok() ? static_cast<int>(first.type()) : static_cast<int>(BSONType::object);
}

void WasmtimeImplScope::rename(const char* from, const char* to) {
    // This method is used to rename global variables in MozJS, but it is never actually used. Keep
    // as a stub for now.
    uasserted(11605400, "Hit unsuported MozJS WASM scope rename");
}

bool WasmtimeImplScope::exec(std::string_view code,
                             const std::string& name,
                             bool printResult,
                             bool reportError,
                             bool assertOnError,
                             int timeoutMs) {
    std::string wrapped = "function() { " + std::string(code) + " }";
    auto bsonVal = wasm::wasm_helpers::convertBsonToWcVal(BSONObj());
    ScriptingFunction func = _bridge->createFunction(wrapped);
    StatusWith<BSONObj> result = _invokeWithDeadlineMonitoring(timeoutMs, [&] {
        return _bridge->invokeFunction(func, std::move(bsonVal), /*ignoreReturn=*/true);
    });
    if (!result.isOK()) {
        if (reportError) {
            LOGV2_ERROR(11605401,
                        "Error executing script",
                        "name"_attr = name,
                        "error"_attr = result.getStatus());
        }
        if (assertOnError) {
            uassertStatusOK(result.getStatus());
        }
        return false;
    }
    return true;
}

std::string WasmtimeImplScope::getError() {
    // This is aligned with implscope's getError() contract, but ideally, we want to retrieve the
    // error from bridge directly.
    return "";
}

void WasmtimeImplScope::registerOperation(OperationContext* opCtx) {
    _opCtx.store(opCtx, std::memory_order_release);
    if (auto* engine = getGlobalScriptEngine()) {
        static_cast<WasmtimeScriptEngine*>(engine)->registerOperation(
            opCtx, this, [this] { _opCtx.store(nullptr, std::memory_order_release); });
    }
}
void WasmtimeImplScope::unregisterOperation() {
    // Atomically take ownership of _opCtx so we call engine->unregisterOperation() exactly once,
    // even if the onTeardown callback races with us from the OperationContext's destructor.
    auto* opCtx = _opCtx.exchange(nullptr, std::memory_order_acq_rel);
    if (opCtx) {
        if (auto* engine = getGlobalScriptEngine()) {
            static_cast<WasmtimeScriptEngine*>(engine)->unregisterOperation(opCtx, this);
        }
    }
}
void WasmtimeImplScope::kill() {
    // Reason-less kill: derive the real reason from the registered opCtx when there is one.
    // The DeadlineMonitor lands here both for its periodic isKillPending() pass (SERVER-130767:
    // that pass is what delivers opCtx kills -- killOp, maxTimeMS expiry, client disconnect --
    // to a thread stuck inside JS, since such a thread never polls checkForInterrupt()) and for
    // the JS-fn timeout, where no opCtx state is set and Interrupted is the correct reason.
    auto reason = ErrorCodes::Interrupted;
    if (auto* opCtx = _opCtx.load(std::memory_order_acquire)) {
        // Lock the Client, per getKillStatus()'s concurrency contract for non-operation
        // threads (this runs on the DeadlineMonitor thread). The opCtx cannot be destroyed
        // underneath us: _opCtx is only non-null between registerOperation() and
        // unregisterOperation(), i.e. while the operation's own thread is inside the
        // invocation, and the DeadlineMonitor only polls tasks whose deadline is still
        // registered (stopDeadline() serialises against the poll via the monitor mutex).
        ClientLock clientLock(opCtx->getClient());
        if (auto opReason = scriptKillReasonFor(opCtx); opReason != ErrorCodes::OK) {
            reason = opReason;
            // The direct kill-op listener path (WasmtimeScriptEngine::interrupt()) logs its own
            // delivery; log here too so the poll-driven path -- the only one that delivers a
            // silent deadline expiry or unrecorded client disconnect (SERVER-130767) -- is
            // equally observable.
            LOGV2_DEBUG(11542381,
                        2,
                        "Delivering opCtx kill to Wasmtime op via DeadlineMonitor poll",
                        "opId"_attr = opCtx->getOpID(),
                        "reason"_attr = reason);
        }
    }
    if (_bridge)
        _bridge->kill(reason);
}
void WasmtimeImplScope::killWithReason(ErrorCodes::Error reason) {
    if (_bridge)
        _bridge->kill(reason);
}
bool WasmtimeImplScope::isKillPending() const {
    if (_bridge && _bridge->isKillPending()) {
        return true;
    }
    // Also report a pending kill when the registered opCtx is interrupted but nobody has told
    // this scope yet (plain markKilled() does not notify kill-op listeners, and a client
    // disconnect or deadline expiry may not be recorded anywhere at all). The DeadlineMonitor
    // polls this every scriptingEngineInterruptIntervalMS and calls kill() when it turns true.
    // Client lock and opCtx lifetime: see kill() above.
    if (auto* opCtx = _opCtx.load(std::memory_order_acquire)) {
        ClientLock clientLock(opCtx->getClient());
        return scriptKillReasonFor(opCtx) != ErrorCodes::OK;
    }
    return false;
}

bool WasmtimeImplScope::hasOutOfMemoryException() {
    invariant(_bridge);
    return _bridge->hasTrapped() || _bridge->hasOomError();
}

void WasmtimeImplScope::_installHelpers() {
    // Array helpers are installed during engine init (_setupNewGlobal). No-op here so the
    // slow-path in init() can call it without branching.
}

void WasmtimeImplScope::_drainEmitToCallback() {
    if (!_emitCallback)
        return;

    BSONObj emitDoc = _bridge->drainEmitBuffer();
    auto emitsArr = emitDoc["emits"].Array();
    for (const auto& emitElem : emitsArr) {
        BSONObj pair = emitElem.Obj();
        _emitCallback(pair, _emitCallbackData);
    }
}

}  // namespace mongo::mozjs

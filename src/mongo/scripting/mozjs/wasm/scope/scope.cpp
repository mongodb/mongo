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

#include "mongo/scripting/mozjs/wasm/scope/scope.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/query_execution_knobs_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/scripting/config_engine_gen.h"
#include "mongo/scripting/config_engine_wasm_gen.h"
#include "mongo/scripting/deadline_monitor.h"
#include "mongo/scripting/mozjs/wasm/wasmtime_engine.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

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
        if (_bridge && _bridge->isHealthy()) {
            _bridge->shutdown();
        }
        _bridge = nullptr;
        _emitSetupBytes = 0;

        // Only recreate WasmEngineContext when a kill was pending. kill() calls
        // Engine::increment_epoch(), which is engine-wide state — a fresh Engine+Component avoids
        // epoch contamination on the next Store instantiation. For non-kill resets the existing
        // Engine is clean and a new Store can be safely created from it.
        if (wasKillPending || !_wasmEngineCtx) {
            _wasmEngineCtx.reset();
            if (auto* engine = getGlobalScriptEngine()) {
                _wasmEngineCtx =
                    static_cast<WasmtimeScriptEngine*>(engine)->createWasmEngineContext();
            }
        }
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
    opts.linearMemoryLimitMB = storeLimit;
    _storeLinearMemBytes = static_cast<int64_t>(storeLimit) * 1024 * 1024;
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

ScriptingFunction WasmtimeImplScope::_createFunction(const char* raw) {
    ScriptingFunction handle = _bridge->createFunction(std::string_view(raw));
    return handle;
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
            _deadlineMonitor.startDeadline(this, timeoutMs);
            {
                ScopeGuard mapGuard([&] { _deadlineMonitor.stopDeadline(this); });
                _bridge->invokeMap(func, std::move(bsonVal));
            }
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
        _deadlineMonitor.startDeadline(this, timeoutMs);
        {
            ScopeGuard predGuard([&] { _deadlineMonitor.stopDeadline(this); });
            predicateResult = _bridge->invokePredicate(func, std::move(bsonVal));
        }
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
    _deadlineMonitor.startDeadline(this, timeoutMs);
    {
        ScopeGuard funcGuard([&] { _deadlineMonitor.stopDeadline(this); });
        result = _bridge->invokeFunction(func, std::move(bsonVal), ignoreReturn);
    }
    uassertStatusOK(result.getStatus());
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

    _emitCallback = func;
    _emitCallbackData = data;

    // evaluate_javascript.cpp calls injectNative a second time with data=nullptr to
    // invalidate the emitState pointer after the map function returns. That call must NOT
    // trigger a fresh setupEmit: the buffer is already drained, and setupEmit allocates
    // ~117 MB of WASM linear memory — repeated calls accumulate heap pressure and can
    // exhaust the store limit (CannotLeaveComponent trap).
    if (!data) {
        return;
    }

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

void WasmtimeImplScope::setString(const char* field, StringData val) {
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
    MONGO_UNREACHABLE;
}

std::string WasmtimeImplScope::getError() {
    // This is aligned with implscope's getError() contract, but ideally, we want to retrieve the
    // error from bridge directly.
    return "";
}

void WasmtimeImplScope::registerOperation(OperationContext* opCtx) {
    _opCtx = opCtx;
    if (auto* engine = getGlobalScriptEngine()) {
        static_cast<WasmtimeScriptEngine*>(engine)->registerOperation(opCtx, this);
    }
}
void WasmtimeImplScope::unregisterOperation() {
    if (_opCtx) {
        if (auto* engine = getGlobalScriptEngine()) {
            static_cast<WasmtimeScriptEngine*>(engine)->unregisterOperation(_opCtx);
        }
        _opCtx = nullptr;
    }
}
void WasmtimeImplScope::kill() {
    if (_bridge)
        _bridge->kill();
}
bool WasmtimeImplScope::isKillPending() const {
    return _bridge && _bridge->isKillPending();
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

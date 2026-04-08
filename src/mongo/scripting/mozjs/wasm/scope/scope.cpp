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
#include "mongo/scripting/mozjs/wasm/wasmtime_engine.h"
#include "mongo/util/str.h"

namespace mongo::mozjs {

// Name of the field used to store function return values in the BSONObj returned by invoke().
const std::string kReturnValueField = "__value";

// Name of the JS global variable used as the invoke return slot.
// Used to identify getXxx calls that should be served from the C++ cache.
constexpr std::string_view kReturnValueGlobal = "__returnValue";

WasmtimeImplScope::WasmtimeImplScope(std::shared_ptr<wasm::WasmEngineContext> wasmEngineCtx,
                                     boost::optional<int> jsHeapLimitMB)
    : _wasmEngineCtx(std::move(wasmEngineCtx)), _jsHeapLimitMB(jsHeapLimitMB) {
    init(nullptr);
}

void WasmtimeImplScope::reset() {
    if (_bridge && _bridge->isInitialized()) {
        _bridge->shutdown();
        _bridge = nullptr;
    }

    _cachedFunctions.clear();

    init(nullptr);
    _emitCallback = nullptr;
    _emitCallbackData = nullptr;
    _lastReturnValue = BSONObj();
}

void WasmtimeImplScope::init(const BSONObj* data) {
    if (_bridge && _bridge->isInitialized()) {
        _bridge->shutdown();
    }
    wasm::MozJSWasmBridge::Options opts{};
    if (_jsHeapLimitMB) {
        opts.heapSizeMB = static_cast<uint32_t>(*_jsHeapLimitMB);
    }
    _bridge = std::make_unique<wasm::MozJSWasmBridge>(_wasmEngineCtx, opts);
    bool initialized = _bridge->initialize(opts);
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
    // The engine's createFunction calls __parseJSFunctionOrExpression (installed during init)
    // to correctly wrap the source — matching MozJSImplScope::_MozJSCreateFunction behavior.
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
    // TODO(SERVER-122128): enforce timeoutMs by running the function in a separate thread and
    // killing it if it runs too long.

    // TODO(SERVER-122738): readOnlyArgs and readOnlyRecv are silently ignored here. In the MozJS
    // implementation these flags cause SpiderMonkey to freeze the corresponding JS objects so that
    // JavaScript code cannot mutate them during execution.
    if (recv && !recv->isEmpty()) {
        if (_emitCallback) {
            uassert(ErrorCodes::BadValue,
                    "emit() cannot be used in a function that returns a value",
                    ignoreReturn);
            _bridge->invokeMap(func, *recv);
            _drainEmitToCallback();
            return 0;
        }

        bool predicateResult = _bridge->invokePredicate(func, *recv);
        if (!ignoreReturn) {
            _lastReturnValue = BSON(kReturnValueField << predicateResult);
        }
        return 0;
    }

    // TODO SERVER-119539 have invokeFunction return the value directly.
    // This would eliminate the extra round trip to the engine.
    auto result = _bridge->invokeFunction(func, args ? *args : BSONObj(), ignoreReturn);
    uassertStatusOK(result.getStatus());
    if (!ignoreReturn) {
        // invokeFunction's direct return goes through getGlobal which flattens JS arrays
        // into BSON objects. Use getReturnValueWrapped() instead, which returns
        // {"__returnValue": val} preserving array type info. Re-key as {"__value": val}
        // for the scope's getters.
        BSONObj wrapped = _bridge->getReturnValueWrapped();
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
    // Set 16 MB byte limit for the emit buffer,
    // which is allocated inside WASM linear memory.
    _bridge->setupEmit(16 * 1024 * 1024);
}

BSONObj WasmtimeImplScope::_resolveGlobal(const char* field) const {
    if (std::string_view(field) == kReturnValueGlobal)
        return _lastReturnValue;
    return _bridge->getGlobal(field);
}

BSONElement WasmtimeImplScope::_resolveGlobalToElement(const char* field) const {
    return _resolveGlobal(field)[kReturnValueField];
}

BSONObj WasmtimeImplScope::getObject(const char* field) {
    BSONObj result = _resolveGlobal(field);
    BSONElement val = result[kReturnValueField];
    // invoke-function results are stored as {"__value": obj} by the C++ invoke() normalisation.
    // Globals set via setObject() are stored raw by the WASM bridge (no __value wrapper).
    // Arrays must also be unwrapped here — Scope::append() calls getObject() for both
    // BSONType::object and BSONType::array (via appendArray), so both need extraction.
    if (val.type() == BSONType::object || val.type() == BSONType::array) {
        return val.Obj();
    }
    return result;
}

std::string WasmtimeImplScope::getString(const char* field) {
    return _resolveGlobalToElement(field).String();
}

bool WasmtimeImplScope::getBoolean(const char* field) {
    return _resolveGlobalToElement(field).boolean();
}

double WasmtimeImplScope::getNumber(const char* field) {
    return _resolveGlobalToElement(field).numberDouble();
}

int WasmtimeImplScope::getNumberInt(const char* field) {
    return _resolveGlobalToElement(field).numberInt();
}

long long WasmtimeImplScope::getNumberLongLong(const char* field) {
    return _resolveGlobalToElement(field).numberLong();
}

Decimal128 WasmtimeImplScope::getNumberDecimal(const char* field) {
    return _resolveGlobalToElement(field).numberDecimal();
}

OID WasmtimeImplScope::getOID(const char* field) {
    return _resolveGlobalToElement(field).OID();
}

void WasmtimeImplScope::getBinData(const char* field,
                                   std::function<void(const BSONBinData&)> withBinData) {
    auto jsBinData = _resolveGlobalToElement(field);
    int len = 0;
    auto binData = jsBinData.binData(len);
    auto subType = jsBinData.binDataType();
    withBinData(BSONBinData(binData, len, static_cast<BinDataType>(static_cast<int>(subType))));
}

Timestamp WasmtimeImplScope::getTimestamp(const char* field) {
    return _resolveGlobalToElement(field).timestamp();
}

JSRegEx WasmtimeImplScope::getRegEx(const char* field) {
    BSONElement elem = _resolveGlobalToElement(field);
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
    // readOnly is silently ignored since the WASM engine does not support freezing objects, but we
    // can still store the object in the global scope.
    _bridge->setGlobalValue(field, BSON("" << obj));
}

void WasmtimeImplScope::setBoolean(const char* field, bool val) {
    _bridge->setGlobalValue(field, BSON("" << val));
}

void WasmtimeImplScope::setFunction(const char* field, const char* code) {
    // Mirror MozJSImplScope::setFunction: parse code as a function expression via the engine's
    // newFunction path (BSONType::code triggers runtime->newFunction in ValueReader, which now
    // calls __parseJSFunctionOrExpression), then set it directly as a named global property.
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

// TODO (SERVER-122128): Implement interrupt support
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
    _bridge->kill();
}
bool WasmtimeImplScope::isKillPending() const {
    return _bridge->isKillPending();
}

// TODO (SERVER-116056): Add memory tracking functionality
bool WasmtimeImplScope::hasOutOfMemoryException() {
    return false;
}

void WasmtimeImplScope::_installHelpers() {
    auto h = _bridge->createFunction(
        "function() {"
        "  Array.sum = function(arr) {"
        "    if (arr.length == 0) return null;"
        "    var s = arr[0];"
        "    for (var i = 1; i < arr.length; i++) s += arr[i];"
        "    return s;"
        "  };"
        "  Array.avg = function(arr) {"
        "    if (arr.length == 0) return null;"
        "    return Array.sum(arr) / arr.length;"
        "  };"
        "  Array.contains = function(arr, obj) {"
        "    for (var i = 0; i < arr.length; i++) {"
        "      if (arr[i] === obj) return true;"
        "    }"
        "    return false;"
        "  };"
        "  Array.unique = function(arr) {"
        "    var r = [];"
        "    for (var i = 0; i < arr.length; i++) {"
        "      if (!Array.contains(r, arr[i])) r.push(arr[i]);"
        "    }"
        "    return r;"
        "  };"
        "  return null;"
        "}");
    uassertStatusOK(_bridge->invokeFunction(h, BSONObj()));
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

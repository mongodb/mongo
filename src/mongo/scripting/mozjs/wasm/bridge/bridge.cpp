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

#include "mongo/scripting/mozjs/wasm/bridge/bridge.h"

#include "mongo/base/error_codes.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::mozjs::wasm {

constexpr std::string_view kMozjsWitInterface = "mongo:mozjs/mozjs";
constexpr std::string_view kInitEngine = "initialize-engine";
constexpr std::string_view kShutdownEngine = "shutdown-engine";
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
constexpr std::string_view kReturnValue = "__returnValue";

std::shared_ptr<WasmEngineContext> WasmEngineContext::createFromPrecompiled(const uint8_t* data,
                                                                            size_t size) {
    wt::Config config;
    config.wasm_component_model(true);
    config.epoch_interruption(true);

    wt::Engine engine(std::move(config));

    wt::Span<uint8_t> span(const_cast<uint8_t*>(data), size);
    auto result = wc::Component::deserialize(engine, span);
    invariant(result);

    return std::shared_ptr<WasmEngineContext>(
        new WasmEngineContext(std::move(engine), result.ok()));
}

MozJSWasmBridge::MozJSWasmBridge(std::shared_ptr<WasmEngineContext> ctx, Options opts)
    : _ctx(std::move(ctx)) {
    _store = wt::Store(_ctx->_engine);
    auto storeCtx = _store->context();

    // This is used to signal process killing.
    storeCtx.set_epoch_deadline(1);

    wt::WasiConfig wasiConfig;
    wasiConfig.inherit_stdout();
    wasiConfig.inherit_stderr();
    invariant(storeCtx.set_wasi(std::move(wasiConfig)));

    wc::Linker linker(_ctx->_engine);
    invariant(linker.add_wasip2());

    // Instantiate directly from the shared compiled component -- no deserialization
    auto instanceResult = linker.instantiate(storeCtx, _ctx->_component);
    invariant(instanceResult);
    _instance = wc::Instance(instanceResult.ok());

    _initEngineFunc = _getFunc(kInitEngine);
    _shutdownEngineFunc = _getFunc(kShutdownEngine);
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
}

bool MozJSWasmBridge::initialize(const Options& options) {
    wc::Val result(wc::WitResult::ok(std::nullopt));
    LOGV2_DEBUG(11542332, 2, "Wasm Bridge Initializing", "ok"_attr = _engineInitialized);
    wc::Record record({
        {"heap-size-mb", wc::Val(uint32_t(options.heapSizeMB))},
    });

    wc::Val optArg = record;
    wasm_helpers::callFunc(*_initEngineFunc, getContext(), &result, 1, std::move(optArg));
    _engineInitialized = wasm_helpers::isResultOk(result);
    if (!_engineInitialized) {
        LOGV2_DEBUG(11542356,
                    1,
                    "Wasm Bridge failed initializaiton",
                    "error"_attr =
                        wasm_helpers::translateMozJSError(*result.get_result().payload()));
    }
    LOGV2_DEBUG(11542331, 2, "Wasm Bridge Initialized", "ok"_attr = _engineInitialized);
    return _engineInitialized;
}

void MozJSWasmBridge::shutdown() {
    wc::Val result(wc::WitResult::ok(std::nullopt));
    LOGV2_DEBUG(11542334, 2, "Wasm Bridge Shutting Down");
    wasm_helpers::callFuncNoArgs(*_shutdownEngineFunc, getContext(), &result, 1);
    if (!wasm_helpers::isResultOk(result)) {
        LOGV2_DEBUG(11542352,
                    1,
                    "Wasm Bridge failed shutdown",
                    "error"_attr =
                        wasm_helpers::translateMozJSError(*result.get_result().payload()));
    }
    invariant(wasm_helpers::isResultOk(result));
    LOGV2_DEBUG(11542333, 2, "Wasm Bridge Shutdown");
    _engineInitialized = false;
}

uint64_t MozJSWasmBridge::createFunction(std::string_view source) {
    wc::Val srcArg(wasm_helpers::makeListU8(source));
    wc::Val result(wc::WitResult::ok(std::nullopt));
    uassert(
        11542310,
        str::stream() << "Failed to call to create JS function " << std::string(source),
        wasm_helpers::callFunc(*_createFunctionFunc, getContext(), &result, 1, std::move(srcArg)));
    uassert(ErrorCodes::JSInterpreterFailure,
            str::stream() << "Failed to create JS function "
                          << wasm_helpers::translateMozJSError(*result.get_result().payload())
                          << " :: source = " << std::string(source),
            wasm_helpers::isResultOk(result));
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
                                                    const BSONObj& args,
                                                    bool ignoreReturn) {
    wc::Val arg0(handle);
    wc::Val arg1(wasm_helpers::makeListU8(args));
    wc::Val result(wc::WitResult::ok(std::nullopt));
    if (!wasm_helpers::callFunc(
            *_invokeFunctionFunc, getContext(), &result, 1, std::move(arg0), std::move(arg1))) {
        return Status{ErrorCodes::Error{11542313},
                      str::stream() << "Failed to call to invoke JS function number " << handle};
    }
    if (!wasm_helpers::isResultOk(result))
        return Status{ErrorCodes::JSInterpreterFailure,
                      str::stream()
                          << "Failed to invoke JS function "
                          << wasm_helpers::translateMozJSError(*result.get_result().payload())
                          << " :: function id = " << handle};
    if (ignoreReturn)
        return BSONObj();
    return _getReturnValueBson();
}

void MozJSWasmBridge::setGlobal(std::string_view name, const BSONObj& value) {
    wc::Val nameArg = wasm_helpers::makeString(name);
    wc::Val valueArg(wasm_helpers::makeListU8(value));
    wc::Val result(wc::WitResult::ok(std::nullopt));
    uassert(
        11542312,
        str::stream() << "Failed to call to set global JS variable " << std::string(name),
        wasm_helpers::callFunc(
            *_setGlobalFunc, getContext(), &result, 1, std::move(nameArg), std::move(valueArg)));
    uassert(11542300,
            str::stream() << "Failed to set global JS variable "
                          << wasm_helpers::translateMozJSError(*result.get_result().payload())
                          << " :: name = " << std::string(name),
            wasm_helpers::isResultOk(result));
}

void MozJSWasmBridge::setGlobalValue(std::string_view name, const BSONObj& value) {
    invariant(value.nFields() == 1);
    wc::Val nameArg = wasm_helpers::makeString(name);
    wc::Val valueArg(wasm_helpers::makeListU8(value));
    wc::Val result(wc::WitResult::ok(std::nullopt));
    uassert(11542316,
            str::stream() << "Failed to call to set global JS value variable " << std::string(name),
            wasm_helpers::callFunc(*_setGlobalValueFunc,
                                   getContext(),
                                   &result,
                                   1,
                                   std::move(nameArg),
                                   std::move(valueArg)));
    uassert(11542317,
            str::stream() << "Failed to set global JS value variable "
                          << wasm_helpers::translateMozJSError(*result.get_result().payload())
                          << " :: name = " << std::string(name),
            wasm_helpers::isResultOk(result));
}

void MozJSWasmBridge::setupEmit(boost::optional<int64_t> byteLimit) {
    wc::Val result(wc::WitResult::ok(std::nullopt));
    // Translate boost::optional to std::optional for WitOption.
    std::optional<int64_t> stdArg = std::nullopt;  // NOLINT
    if (byteLimit) {
        stdArg = *byteLimit;
    }
    wc::Val arg = wc::WitOption(stdArg);
    wasm_helpers::callFunc(*_setupEmitFunc, getContext(), &result, 1, std::move(arg));
    if (!wasm_helpers::isResultOk(result)) {
        LOGV2_DEBUG(11542353,
                    1,
                    "Wasm Bridge failed to setup-emit",
                    "error"_attr =
                        wasm_helpers::translateMozJSError(*result.get_result().payload()));
    }
    invariant(wasm_helpers::isResultOk(result));
}

void MozJSWasmBridge::invokeMap(uint64_t handle, const BSONObj& args) {
    wc::Val arg0(handle);
    wc::Val arg1(wasm_helpers::makeListU8(args));
    wc::Val result(wc::WitResult::ok(std::nullopt));
    uassert(11542319,
            str::stream() << "Failed to call to invoke JS function number " << handle,
            wasm_helpers::callFunc(
                *_invokeMapFunc, getContext(), &result, 1, std::move(arg0), std::move(arg1)));
    uassert(ErrorCodes::JSInterpreterFailure,
            str::stream() << "Failed to invoke JS function "
                          << wasm_helpers::translateMozJSError(*result.get_result().payload())
                          << " :: function id = " << handle,
            wasm_helpers::isResultOk(result));
}

bool MozJSWasmBridge::invokePredicate(uint64_t handle, const BSONObj& args) {
    wc::Val arg0(handle);
    wc::Val arg1(wasm_helpers::makeListU8(args));
    wc::Val result(wc::WitResult::ok(std::nullopt));
    uassert(11542339,
            str::stream() << "Failed to call to invoke JS function number " << handle,
            wasm_helpers::callFunc(
                *_invokePredicateFunc, getContext(), &result, 1, std::move(arg0), std::move(arg1)));
    uassert(ErrorCodes::JSInterpreterFailure,
            str::stream() << "Failed to invoke JS function "
                          << wasm_helpers::translateMozJSError(*result.get_result().payload())
                          << " :: function id = " << handle,
            wasm_helpers::isResultOk(result));
    const wc::Val* payload = result.get_result().payload();
    invariant(payload && payload->is_bool());
    return payload->get_bool();
}

void MozJSWasmBridge::kill() {
    signalInterrupt();
}

void MozJSWasmBridge::signalInterrupt() {
    _killPending.store(true);
    _ctx->_engine.increment_epoch();
}

bool MozJSWasmBridge::isKillPending() const {
    return _killPending.load();
}

BSONObj MozJSWasmBridge::drainEmitBuffer() {
    wc::Val result(wc::WitResult::ok(std::nullopt));
    wasm_helpers::callFuncNoArgs(*_drainEmitBufferFunc, getContext(), &result, 1);
    if (!wasm_helpers::isResultOk(result)) {
        LOGV2_DEBUG(11542355,
                    1,
                    "Wasm Bridge failed to drain emit",
                    "error"_attr =
                        wasm_helpers::translateMozJSError(*result.get_result().payload()));
    }
    invariant(wasm_helpers::isResultOk(result));
    return _extractBSON(result);
}

BSONObj MozJSWasmBridge::getGlobal(std::string_view name, bool implicitNull) {
    wc::Val nameArg = wasm_helpers::makeString(name);
    wc::Val result(wc::WitResult::ok(std::nullopt));
    uassert(11542304,
            str::stream() << "Failed to call to get global JS variable " << std::string(name),
            wasm_helpers::callFunc(*_getGlobalFunc, getContext(), &result, 1, std::move(nameArg)));

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
    uassert(11542301,
            str::stream() << "Failed to get global JS variable "
                          << wasm_helpers::translateMozJSError(*result.get_result().payload())
                          << " :: name = " << std::string(name),
            wasm_helpers::isResultOk(result));
    return _extractBSON(result);
}

wc::Func MozJSWasmBridge::_getFunc(std::string_view funcName) {
    invariant(_instance);
    return wasm_helpers::getMozjsFunc(*(_instance), getContext(), "mongo:mozjs/mozjs", funcName);
}

BSONObj MozJSWasmBridge::_extractBSON(const wc::Val& result) {
    const wc::Val* payload = result.get_result().payload();
    invariant(payload);
    auto bytes = wasm_helpers::extractListU8(*payload);
    invariant(bytes.size() >= BSONObj::kMinBSONLength);
    // Copy into owned buffer for BSONObj
    auto buf = SharedBuffer::allocate(bytes.size());
    std::memcpy(buf.get(), bytes.data(), bytes.size());
    auto returnVal = BSONObj(std::move(buf));
    invariant(returnVal.isValid());
    return returnVal;
}

BSONObj MozJSWasmBridge::_getReturnValueBson() {
    // Uses getGlobal which does not preserve JS array types (arrays become BSON objects with
    // numeric keys). Use getReturnValueWrapped() when array type preservation matters.
    // getGlobal also fails when the return value is undefined (e.g., a function with no return
    // statement). Return an empty object in that case to match MozJS behavior.
    try {
        return getGlobal(kReturnValue, true);
    } catch (const DBException&) {
        return BSONObj();
    }
}

BSONObj MozJSWasmBridge::getReturnValueWrapped() {
    wc::Val result(wc::WitResult::ok(std::nullopt));
    uassert(11542350,
            "Failed to call get-return-value-bson",
            wasm_helpers::callFuncNoArgs(*_getReturnValueBsonFunc, getContext(), &result, 1));
    uassert(11542351,
            str::stream() << "Failed to get return value BSON "
                          << wasm_helpers::translateMozJSError(*result.get_result().payload()),
            wasm_helpers::isResultOk(result));
    return _extractBSON(result);
}

}  // namespace mongo::mozjs::wasm

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

#include "engine.h"

#include "mongo/base/status.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/scripting/mozjs/common/error.h"
#include "mongo/scripting/mozjs/common/parse_function_helper.h"
#include "mongo/scripting/mozjs/common/runtime.h"
#include "mongo/scripting/mozjs/common/types/bindata.h"
#include "mongo/scripting/mozjs/common/types/bson.h"
#include "mongo/scripting/mozjs/common/types/code.h"
#include "mongo/scripting/mozjs/common/types/dbpointer.h"
#include "mongo/scripting/mozjs/common/types/dbref.h"
#include "mongo/scripting/mozjs/common/types/maxkey.h"
#include "mongo/scripting/mozjs/common/types/minkey.h"
#include "mongo/scripting/mozjs/common/types/nativefunction.h"
#include "mongo/scripting/mozjs/common/types/numberdecimal.h"
#include "mongo/scripting/mozjs/common/types/numberint.h"
#include "mongo/scripting/mozjs/common/types/numberlong.h"
#include "mongo/scripting/mozjs/common/types/oid.h"
#include "mongo/scripting/mozjs/common/types/regexp.h"
#include "mongo/scripting/mozjs/common/types/status.h"
#include "mongo/scripting/mozjs/common/types/timestamp.h"
#include "mongo/scripting/mozjs/common/valuereader.h"
#include "mongo/scripting/mozjs/common/valuewriter.h"
#include "mongo/util/assert_util.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>

#include "error.h"
#include "jsapi.h"
#include "jsfriendapi.h"

#include "js/CompilationAndEvaluation.h"
#include "js/Conversions.h"
#include "js/ErrorReport.h"
#include "js/Exception.h"
#include "js/GlobalObject.h"
#include "js/Initialization.h"
#include "js/Interrupt.h"
#include "js/Realm.h"
#include "js/SourceText.h"
#include "js/String.h"
#include "js/Value.h"
#include "js/ValueArray.h"
#include <mozilla/Utf8.h>

namespace mongo {
namespace mozjs {
namespace wasm {

uint32_t g_wasmJsHeapLimitMB = 0;

const char* const kInvokeResult = "__returnValue";

FunctionSlot* MozJSScriptEngine::resolveHandle(uint64_t handle, wasm_mozjs_error_t* err) {
    if (handle == 0 || handle > _slots.size()) {
        if (err) {
            err->code = SM_E_INVALID_ARG;
            set_string(&err->msg, &err->msg_len, "invalid function handle");
        }
        return nullptr;
    }
    FunctionSlot& slot = _slots[handle - 1];
    if (!slot.fn) {
        if (err) {
            err->code = SM_E_INVALID_ARG;
            set_string(&err->msg, &err->msg_len, "function handle has no function");
        }
        return nullptr;
    }
    return &slot;
}

MozJSScriptEngine::~MozJSScriptEngine() {
    if (_initialized) {
        shutdown(nullptr);
    }
}

err_code_t MozJSScriptEngine::init(const wasm_mozjs_startup_options_t* opt,
                                   wasm_mozjs_error_t* err) {
    clear_error(err);

    if (_initialized)
        return SM_OK;

    if (!JS_Init()) {
        if (err) {
            err->code = SM_E_INTERNAL;
            set_string(&err->msg, &err->msg_len, "JS_Init failed");
        }
        return SM_E_INTERNAL;
    }

    // Create context with reasonable heap size for WASM builds
    constexpr size_t kMaxHeapSizeMB = 2048;  // 2 GB upper bound
    if (opt->heapSize == 0 || opt->heapSize > kMaxHeapSizeMB) {
        if (err) {
            err->code = SM_E_INVALID_ARG;
            set_string(&err->msg, &err->msg_len, "heapSize out of valid range (1-2048 MB)");
        }
        JS_ShutDown();
        return SM_E_INVALID_ARG;
    }
    _cx = JS_NewContext(static_cast<size_t>(opt->heapSize) * 1024 * 1024);
    if (!_cx) {
        if (err) {
            err->code = SM_E_OOM;
            set_string(&err->msg, &err->msg_len, "JS_NewContext failed");
        }
        JS_ShutDown();
        return SM_E_OOM;
    }

    // Store configured heap limit for GlobalInfo::getJSHeapLimitMB.
    g_wasmJsHeapLimitMB = opt->heapSize;

    _rt = JS_GetRuntime(_cx);
    if (!_rt) {
        if (err) {
            err->code = SM_E_INTERNAL;
            set_string(&err->msg, &err->msg_len, "JS_GetRuntime returned null");
        }
        JS_DestroyContext(_cx);
        _cx = nullptr;
        JS_ShutDown();
        return SM_E_INTERNAL;
    }

    // Initialize self-hosted code (required before creating global)
    ExecutionCheck chk(_cx, err);
    if (!chk.ok(JS::InitSelfHostedCode(_cx), SM_E_INTERNAL)) {
        JS_DestroyContext(_cx);
        _cx = nullptr;
        JS_ShutDown();
        return err ? err->code : SM_E_INTERNAL;
    }

    // Create global object (must be done before setting interrupt callback)
    static const JSClass _globalclass = {
        "global", JSCLASS_GLOBAL_FLAGS, &JS::DefaultGlobalClassOps};

    JS::RealmOptions ro;
    _global.init(_cx);
    {
        JS::RootedObject g(_cx,
                           chk.okPtr(JS_NewGlobalObject(
                               _cx, &_globalclass, nullptr, JS::DontFireOnNewGlobalHook, ro)));
        if (!g) {
            shutdown(nullptr);
            return err ? err->code : SM_E_INTERNAL;
        }
        _global = g;
    }

    // Enter realm and initialize standard classes
    {
        JSAutoRealm ar(_cx, _global);
        if (!chk.ok(JS::InitRealmStandardClasses(_cx), SM_E_INTERNAL)) {
            shutdown(nullptr);
            return err ? err->code : SM_E_INTERNAL;
        }
    }

    // Fire the new global hook after initialization (as per SpiderMonkey docs)
    JS_FireOnNewGlobalObject(_cx, _global);
    // JS_FireOnNewGlobalObject doesn't return a value, but we check for exceptions
    if (!chk.ok(!JS_IsExceptionPending(_cx), SM_E_INTERNAL)) {
        shutdown(nullptr);
        return err ? err->code : SM_E_INTERNAL;
    }

    // Stash pointer so native callbacks can reach the engine instance.
    JS_SetContextPrivate(_cx, static_cast<MozJSCommonRuntimeInterface*>(this));

    _prototypeInstaller = std::make_unique<MozJSPrototypeInstaller>(_cx);

    {
        JSAutoRealm ar(_cx, _global);
        _internedStrings = std::make_unique<InternedStringTable>(_cx);
        _prototypeInstaller->installTypes(_global);

        if (!chk.ok(installParseJSFunctionHelper(_cx, _global), SM_E_INTERNAL)) {
            shutdown(nullptr);
            return err ? err->code : SM_E_INTERNAL;
        }
    }

    _initialized = true;
    return SM_OK;
}

err_code_t MozJSScriptEngine::shutdown(wasm_mozjs_error_t* err) {
    clear_error(err);

    if (!_initialized && !_cx)
        return SM_OK;

    // Drop all PersistentRooted objects while the context is alive.
    _slots.clear();
    _internedStrings.reset();
    _prototypeInstaller.reset();

    _global.reset();
    if (_cx) {
        // Do NOT null context-private before DestroyContext:
        // GC finalizers need getCommonRuntime() during teardown.
        JS_DestroyContext(_cx);
        _cx = nullptr;
    }

    JS_ShutDown();

    _rt = nullptr;
    _initialized = false;

    return SM_OK;
}

err_code_t MozJSScriptEngine::interrupt(wasm_mozjs_error_t* err) {
    clear_error(err);
    if (!_initialized || !_cx)
        return SM_E_BAD_STATE;

    ExecutionCheck chk(_cx, err);
    JS_RequestInterruptCallback(_cx);
    if (!chk.ok(!JS_IsExceptionPending(_cx), SM_E_INTERNAL)) {
        return err ? err->code : SM_E_INTERNAL;
    }
    return SM_OK;
}

bool MozJSScriptEngine::_parseFunctionSource(StringData raw,
                                             std::string* out,
                                             wasm_mozjs_error_t* err) {
    ExecutionCheck chk(_cx, err);
    if (!chk.ok(parseJSFunctionOrExpression(_cx, _global, raw, out), SM_E_COMPILE)) {
        if (err && err->code == SM_E_PENDING_EXCEPTION)
            err->code = SM_E_COMPILE;
        return false;
    }
    return true;
}

err_code_t MozJSScriptEngine::createFunction(const uint8_t* src,
                                             size_t len,
                                             uint64_t* out_handle,
                                             wasm_mozjs_error_t* err) {
    clear_error(err);
    if (!_initialized || !_cx || !_global)
        return SM_E_BAD_STATE;
    if (!src || len == 0 || !out_handle)
        return SM_E_INVALID_ARG;

    JSAutoRealm ar(_cx, _global);

    ExecutionCheck chk(_cx, err);

    // Pre-validate UTF-8 before _parseFunctionSource, which converts to a JS string
    // (silently accepting invalid bytes via SpiderMonkey's internal encoding).
    // JS::SourceText::init does NOT validate at init time; validation happens during
    // JS::Evaluate, but by then the source is already a JS string.
    if (!mozilla::IsUtf8(mozilla::Span(reinterpret_cast<const char*>(src), len))) {
        if (err) {
            err->code = SM_E_ENCODING;
            set_string(&err->msg, &err->msg_len, "createFunction: source is not valid UTF-8");
        }
        return SM_E_ENCODING;
    }

    std::string parsed;
    if (!_parseFunctionSource(StringData(reinterpret_cast<const char*>(src), len), &parsed, err))
        return err ? err->code : SM_E_COMPILE;

    std::string code_str;
    code_str.reserve(parsed.size() + 2);
    code_str += '(';
    code_str += parsed;
    code_str += ')';

    JS::CompileOptions opts(_cx);
    opts.setFileAndLine("wasm:function", 1);

    JS::SourceText<mozilla::Utf8Unit> text;
    // Use .data() and .size() like in implscope.cpp
    // The CharT overload accepts const char* for UTF-8
    if (!chk.ok(text.init(_cx, code_str.data(), code_str.size(), JS::SourceOwnership::Borrowed),
                SM_E_ENCODING)) {
        return err ? err->code : SM_E_ENCODING;
    }

    JS::RootedValue v(_cx);
    if (!chk.ok(JS::Evaluate(_cx, opts, text, &v), SM_E_COMPILE)) {
        // classify common path
        if (err && err->code == SM_E_PENDING_EXCEPTION)
            err->code = SM_E_COMPILE;
        return err ? err->code : SM_E_COMPILE;
    }

    if (!v.isObject() || !js::IsFunctionObject(v.toObjectOrNull())) {
        if (err) {
            err->code = SM_E_TYPE;
            set_string(
                &err->msg, &err->msg_len, "createFunction: evaluated value is not a function");
        }
        return SM_E_TYPE;
    }

    _slots.emplace_back(_cx);
    FunctionSlot& slot = _slots.back();
    slot.fn = &v.toObject();

    *out_handle = static_cast<uint64_t>(_slots.size());  // 1-based

    return SM_OK;
}

err_code_t MozJSScriptEngine::invokeFunction(uint64_t handle,
                                             mongo::BSONObj&& argsObject,
                                             mongo::BSONObj* outBson,
                                             wasm_mozjs_error_t* err) {
    clear_error(err);
    if (!_initialized || !_cx || !_global)
        return SM_E_BAD_STATE;

    auto* slot = resolveHandle(handle, err);
    if (!slot)
        return SM_E_INVALID_ARG;

    JSAutoRealm ar(_cx, _global);

    ExecutionCheck chk(_cx, err);

    const int nargs = argsObject.nFields();

    JS::RootedValueVector args(_cx);

    if (nargs) {
        BSONObjIterator it(argsObject);
        for (int i = 0; i < nargs; i++) {
            BSONElement next = it.next();

            JS::RootedValue value(_cx);
            ValueReader(_cx, &value).fromBSONElement(next, argsObject, false /*readOnlyArgs*/);

            if (!args.append(value)) {
                if (err) {
                    err->code = SM_E_INVALID_ARG;
                    set_string(&err->msg, &err->msg_len, "Failed to append property");
                }
                return SM_E_INVALID_ARG;
            }
        }
    }

    JS::RootedValue out(_cx);
    JS::RootedObject funcObj(_cx, slot->fn);
    JS::RootedValue funVal(_cx, JS::ObjectValue(*funcObj));
    bool success = JS::Call(_cx, _global, funVal, args, &out);

    if (!chk.ok(success, SM_E_RUNTIME)) {
        if (err && err->code == SM_E_PENDING_EXCEPTION)
            err->code = SM_E_RUNTIME;
        return err ? err->code : SM_E_RUNTIME;
    }

    if (_emitByteLimit > 0 && _emitBytesUsed > _emitByteLimit) {
        if (err) {
            err->code = SM_E_RUNTIME;
            set_string(&err->msg, &err->msg_len, "emit() exceeded memory limit");
        }
        return SM_E_RUNTIME;
    }

    // Store return value on global (same key as implscope) so getReturnValueBson can read it.
    ObjectWrapper(_cx, _global).setValue(kInvokeResult, out);

    if (outBson) {
        // Also write return value as BSON when caller provides a buffer (wrapped like implscope).
        JS::RootedObject rout(_cx, JS_NewPlainObject(_cx));
        if (rout) {
            ObjectWrapper wout(_cx, rout);
            wout.setValue(kInvokeResult, out);
            *outBson = wout.toBSON();
        }
    }

    return SM_OK;
}

err_code_t MozJSScriptEngine::getReturnValueBson(mongo::BSONObj* out, wasm_mozjs_error_t* err) {
    clear_error(err);
    if (!_initialized || !_cx || !_global)
        return SM_E_BAD_STATE;
    if (!out)
        return SM_E_INVALID_ARG;

    JSAutoRealm ar(_cx, _global);
    ExecutionCheck chk(_cx, err);

    JS::RootedValue rval(_cx);
    JS::RootedString key_str(_cx, chk.okPtr(JS_NewStringCopyZ(_cx, kInvokeResult)));
    if (!key_str) {
        return err ? err->code : SM_E_INTERNAL;
    }

    JS::RootedId rid(_cx);
    if (!chk.ok(JS_StringToId(_cx, key_str, &rid), SM_E_INTERNAL)) {
        return err ? err->code : SM_E_INTERNAL;
    }

    if (!chk.ok(JS_GetPropertyById(_cx, _global, rid, &rval), SM_E_INTERNAL)) {
        return err ? err->code : SM_E_INTERNAL;
    }

    // Wrap value in object for BSON (same pattern as implscope) so primitives serialize correctly.
    JS::RootedObject rout(_cx, JS_NewPlainObject(_cx));
    if (!rout) {
        if (err) {
            err->code = SM_E_INTERNAL;
            set_string(
                &err->msg, &err->msg_len, "getReturnValueBson: failed to create wrapper object");
        }
        return SM_E_INTERNAL;
    }
    ObjectWrapper wout(_cx, rout);
    wout.setValue(kInvokeResult, rval);
    *out = wout.toBSON();

    return SM_OK;
}

err_code_t MozJSScriptEngine::invokePredicate(uint64_t handle,
                                              mongo::BSONObj&& document,
                                              bool* outResult,
                                              wasm_mozjs_error_t* err) {
    clear_error(err);
    if (!_initialized || !_cx || !_global)
        return SM_E_BAD_STATE;
    if (!outResult)
        return SM_E_INVALID_ARG;

    auto* slot = resolveHandle(handle, err);
    if (!slot)
        return SM_E_INVALID_ARG;

    JSAutoRealm ar(_cx, _global);
    ExecutionCheck chk(_cx, err);

    JS::RootedValue smrecv(_cx);
    if (!document.isEmpty())
        ValueReader(_cx, &smrecv).fromBSON(document, nullptr, false);
    else
        smrecv.setObjectOrNull(_global);

    JS::RootedValueVector args(_cx);

    JS::RootedValue out(_cx);
    JS::RootedObject obj(_cx, smrecv.toObjectOrNull());
    JS::RootedObject funcObj(_cx, slot->fn);
    JS::RootedValue funVal(_cx, JS::ObjectValue(*funcObj));
    bool success = JS::Call(_cx, obj, funVal, args, &out);

    if (!chk.ok(success, SM_E_RUNTIME)) {
        if (err && err->code == SM_E_PENDING_EXCEPTION)
            err->code = SM_E_RUNTIME;
        return err ? err->code : SM_E_RUNTIME;
    }

    *outResult = JS::ToBoolean(out);
    return SM_OK;
}

err_code_t MozJSScriptEngine::invokeMap(uint64_t handle,
                                        mongo::BSONObj&& document,
                                        wasm_mozjs_error_t* err) {
    clear_error(err);
    if (!_initialized || !_cx || !_global)
        return SM_E_BAD_STATE;

    auto* slot = resolveHandle(handle, err);
    if (!slot)
        return SM_E_INVALID_ARG;

    JSAutoRealm ar(_cx, _global);
    ExecutionCheck chk(_cx, err);

    JS::RootedValue smrecv(_cx);
    if (!document.isEmpty())
        ValueReader(_cx, &smrecv).fromBSON(document, nullptr, false);
    else
        smrecv.setObjectOrNull(_global);

    JS::RootedValueVector args(_cx);

    JS::RootedValue out(_cx);
    JS::RootedObject obj(_cx, smrecv.toObjectOrNull());
    JS::RootedObject funcObj(_cx, slot->fn);
    JS::RootedValue funVal(_cx, JS::ObjectValue(*funcObj));
    bool success = JS::Call(_cx, obj, funVal, args, &out);

    if (!chk.ok(success, SM_E_RUNTIME)) {
        if (err && err->code == SM_E_PENDING_EXCEPTION)
            err->code = SM_E_RUNTIME;
        return err ? err->code : SM_E_RUNTIME;
    }

    if (_emitByteLimit > 0 && _emitBytesUsed > _emitByteLimit) {
        if (err) {
            err->code = SM_E_RUNTIME;
            set_string(&err->msg, &err->msg_len, "emit() exceeded memory limit");
        }
        return SM_E_RUNTIME;
    }

    return SM_OK;
}

err_code_t MozJSScriptEngine::setGlobal(const char* name,
                                        size_t name_len,
                                        const mongo::BSONObj& value,
                                        wasm_mozjs_error_t* err) {
    clear_error(err);
    if (!_initialized || !_cx || !_global)
        return SM_E_BAD_STATE;
    if (!name || name_len == 0)
        return SM_E_INVALID_ARG;

    JSAutoRealm ar(_cx, _global);
    ExecutionCheck chk(_cx, err);

    // Create a JS plain object and populate from BSON fields.
    JS::RootedObject jsObj(_cx, JS_NewPlainObject(_cx));
    if (!jsObj) {
        if (err) {
            err->code = SM_E_OOM;
            set_string(&err->msg, &err->msg_len, "setGlobal: JS_NewPlainObject failed");
        }
        return SM_E_OOM;
    }

    for (BSONObjIterator it(value); it.more();) {
        BSONElement elem = it.next();
        JS::RootedValue val(_cx);
        ValueReader(_cx, &val).fromBSONElement(elem, value, false /* readOnlyArgs */);

        if (!chk.ok(JS_SetProperty(_cx, jsObj, elem.fieldName(), val), SM_E_INTERNAL)) {
            return err ? err->code : SM_E_INTERNAL;
        }
    }

    // Set the constructed object as a property on the global.
    JS::RootedValue objVal(_cx, JS::ObjectValue(*jsObj));
    std::string nameStr(name, name_len);
    if (!chk.ok(JS_SetProperty(_cx, _global, nameStr.c_str(), objVal), SM_E_INTERNAL)) {
        return err ? err->code : SM_E_INTERNAL;
    }

    return SM_OK;
}

err_code_t MozJSScriptEngine::getGlobal(const char* name,
                                        size_t name_len,
                                        mongo::BSONObj* out,
                                        wasm_mozjs_error_t* err) {
    clear_error(err);
    if (!_initialized || !_cx || !_global)
        return SM_E_BAD_STATE;
    if (!name || name_len == 0 || !out)
        return SM_E_INVALID_ARG;

    JSAutoRealm ar(_cx, _global);
    ExecutionCheck chk(_cx, err);

    std::string nameStr(name, name_len);

    // Check if the property exists on the global object.
    bool found = false;
    if (!chk.ok(JS_HasProperty(_cx, _global, nameStr.c_str(), &found), SM_E_INTERNAL)) {
        return err ? err->code : SM_E_INTERNAL;
    }
    if (!found) {
        if (err) {
            err->code = SM_E_INVALID_ARG;
            set_string(&err->msg, &err->msg_len, "getGlobal: property not found");
        }
        return SM_E_INVALID_ARG;
    }

    JS::RootedValue rval(_cx);
    if (!chk.ok(JS_GetProperty(_cx, _global, nameStr.c_str(), &rval), SM_E_INTERNAL)) {
        return err ? err->code : SM_E_INTERNAL;
    }

    if (rval.isUndefined()) {
        if (err) {
            err->code = SM_E_INVALID_ARG;
            set_string(&err->msg, &err->msg_len, "getGlobal: property is undefined");
        }
        return SM_E_INVALID_ARG;
    }

    // Convert the JS value to BSON.
    if (rval.isObject()) {
        JS::RootedObject robj(_cx, &rval.toObject());
        ObjectWrapper wrapper(_cx, robj);
        *out = wrapper.toBSON();
    } else {
        // Wrap a non-object (primitive) value in a BSON document.
        JS::RootedObject wrapObj(_cx, JS_NewPlainObject(_cx));
        if (!wrapObj) {
            if (err) {
                err->code = SM_E_OOM;
            }
            return SM_E_OOM;
        }
        ObjectWrapper wout(_cx, wrapObj);
        wout.setValue("__value", rval);
        *out = wout.toBSON();
    }

    return SM_OK;
}

err_code_t MozJSScriptEngine::setGlobalValue(const char* name,
                                             size_t name_len,
                                             const mongo::BSONObj& singleElementDoc,
                                             wasm_mozjs_error_t* err) {
    clear_error(err);
    if (!_initialized || !_cx || !_global)
        return SM_E_BAD_STATE;
    if (!name || name_len == 0)
        return SM_E_INVALID_ARG;

    BSONObjIterator it(singleElementDoc);
    if (!it.more()) {
        if (err) {
            err->code = SM_E_INVALID_ARG;
            set_string(&err->msg, &err->msg_len, "setGlobalValue: empty BSON document");
        }
        return SM_E_INVALID_ARG;
    }

    BSONElement elem = it.next();

    JSAutoRealm ar(_cx, _global);
    ExecutionCheck chk(_cx, err);

    JS::RootedValue val(_cx);
    ValueReader(_cx, &val).fromBSONElement(elem, singleElementDoc, false);

    std::string nameStr(name, name_len);
    if (!chk.ok(JS_SetProperty(_cx, _global, nameStr.c_str(), val), SM_E_INTERNAL)) {
        return err ? err->code : SM_E_INTERNAL;
    }

    return SM_OK;
}

BSONObj MozJSScriptEngine::_emitCallback(const BSONObj& args, void* data) {
    auto* engine = static_cast<MozJSScriptEngine*>(data);

    BSONObjIterator it(args);
    BSONElement keyElem = it.more() ? it.next() : BSONElement();
    BSONElement valElem = it.more() ? it.next() : BSONElement();

    BSONObjBuilder b;
    if (keyElem.type() == BSONType::undefined || keyElem.eoo())
        b.appendNull("k");
    else
        b.appendAs(keyElem, "k");

    if (valElem.eoo())
        b.appendNull("v");
    else
        b.appendAs(valElem, "v");

    BSONObj doc = b.obj();
    engine->_emitBytesUsed += doc.objsize();
    if (engine->_emitBytesUsed <= engine->_emitByteLimit)
        engine->_emitBuffer.push_back(std::move(doc));

    return BSONObj();
}

err_code_t MozJSScriptEngine::setupEmit(int64_t byteLimit,
                                        bool hasByteLimit,
                                        wasm_mozjs_error_t* err) {
    clear_error(err);
    if (!_initialized || !_cx || !_global)
        return SM_E_BAD_STATE;

    _emitByteLimit = hasByteLimit ? byteLimit : (16 * 1024 * 1024);
    _emitBytesUsed = 0;
    _emitBuffer.clear();

    JSAutoRealm ar(_cx, _global);

    JS::RootedObject obj(_cx);
    NativeFunctionInfo::make(_cx, &obj, _emitCallback, this);

    JS::RootedValue value(_cx);
    value.setObjectOrNull(obj);
    ObjectWrapper(_cx, _global).setValue("emit", value);

    return SM_OK;
}

err_code_t MozJSScriptEngine::drainEmitBuffer(mongo::BSONObj* out, wasm_mozjs_error_t* err) {
    clear_error(err);
    if (!_initialized)
        return SM_E_BAD_STATE;
    if (!out)
        return SM_E_INVALID_ARG;

    BSONObjBuilder builder;
    BSONArrayBuilder arr(builder.subarrayStart("emits"));
    for (const auto& doc : _emitBuffer) {
        arr.append(doc);
    }
    arr.done();
    builder.append("bytesUsed", static_cast<long long>(_emitBytesUsed));

    *out = builder.obj();

    _emitBuffer.clear();
    _emitBytesUsed = 0;

    return SM_OK;
}

void MozJSScriptEngine::gc() {
    if (!_initialized || !_cx) {
        return;
    }
    JS_GC(_cx);
}

void MozJSScriptEngine::sleep(Milliseconds ms) {
    // Sleep in 1ms slices so that Wasmtime epoch interruption can fire between
    // slices. nanosleep is a blocking host call — the WASM epoch check only
    // triggers when control returns to WASM code. Slicing keeps sleep
    // interruptible by the DeadlineMonitor on the host side.
    auto remaining = ms;
    constexpr Milliseconds kSlice{1};
    while (remaining > Milliseconds{0}) {
        auto slice = std::min(remaining, kSlice);
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = static_cast<long>(slice.count() * 1'000'000L);
        nanosleep(&ts, nullptr);
        remaining -= slice;
    }
}

std::size_t MozJSScriptEngine::getGeneration() const {
    // Return a constant generation for WASM
    return 1;
}

JS::HandleId MozJSScriptEngine::getInternedStringId(InternedString name) {
    return _internedStrings->getInternedString(name);
}

std::int64_t* MozJSScriptEngine::trackedNewInt64(std::int64_t value) {
    auto* p = new std::int64_t(value);
    trackNewPointer(p);
    return p;
}

WrapType<NumberLongInfo>& MozJSScriptEngine::numberLongProto() {
    if (!_prototypeInstaller) {
        __builtin_trap();  // WASM trap: prototypeInstaller not initialized
    }
    return _prototypeInstaller->numberLongProto();
}

WrapType<NumberIntInfo>& MozJSScriptEngine::numberIntProto() {
    if (!_prototypeInstaller) {
        __builtin_trap();  // WASM trap: prototypeInstaller not initialized
    }
    return _prototypeInstaller->numberIntProto();
}

WrapType<NumberDecimalInfo>& MozJSScriptEngine::numberDecimalProto() {
    if (!_prototypeInstaller) {
        __builtin_trap();  // WASM trap: prototypeInstaller not initialized
    }
    return _prototypeInstaller->numberDecimalProto();
}

WrapType<OIDInfo>& MozJSScriptEngine::oidProto() {
    if (!_prototypeInstaller) {
        __builtin_trap();  // WASM trap: prototypeInstaller not initialized
    }
    return _prototypeInstaller->oidProto();
}

WrapType<BinDataInfo>& MozJSScriptEngine::binDataProto() {
    if (!_prototypeInstaller) {
        __builtin_trap();  // WASM trap: prototypeInstaller not initialized
    }
    return _prototypeInstaller->binDataProto();
}

WrapType<TimestampInfo>& MozJSScriptEngine::timestampProto() {
    if (!_prototypeInstaller) {
        __builtin_trap();  // WASM trap: prototypeInstaller not initialized
    }
    return _prototypeInstaller->timestampProto();
}

WrapType<MaxKeyInfo>& MozJSScriptEngine::maxKeyProto() {
    if (!_prototypeInstaller) {
        __builtin_trap();  // WASM trap: prototypeInstaller not initialized
    }
    return _prototypeInstaller->maxKeyProto();
}

WrapType<MinKeyInfo>& MozJSScriptEngine::minKeyProto() {
    if (!_prototypeInstaller) {
        __builtin_trap();  // WASM trap: prototypeInstaller not initialized
    }
    return _prototypeInstaller->minKeyProto();
}

void MozJSScriptEngine::trackNewPointer(void* ptr) {
    (void)ptr;
}

void MozJSScriptEngine::trackDeletePointer(void* ptr) {
    (void)ptr;
}

WrapType<CodeInfo>& MozJSScriptEngine::codeProto() {
    if (!_prototypeInstaller) {
        __builtin_trap();  // WASM trap: prototypeInstaller not initialized
    }
    return _prototypeInstaller->codeProto();
}

WrapType<DBPointerInfo>& MozJSScriptEngine::dbPointerProto() {
    if (!_prototypeInstaller) {
        __builtin_trap();  // WASM trap: prototypeInstaller not initialized
    }
    return _prototypeInstaller->dbPointerProto();
}

WrapType<NativeFunctionInfo>& MozJSScriptEngine::nativeFunctionProto() {
    if (!_prototypeInstaller) {
        __builtin_trap();  // WASM trap: prototypeInstaller not initialized
    }
    return _prototypeInstaller->nativeFunctionProto();
}

WrapType<ErrorInfo>& MozJSScriptEngine::errorProto() {
    if (!_prototypeInstaller) {
        __builtin_trap();  // WASM trap: prototypeInstaller not initialized
    }
    return _prototypeInstaller->errorProto();
}

WrapType<MongoStatusInfo>& MozJSScriptEngine::mongoStatusProto() {
    if (!_prototypeInstaller) {
        __builtin_trap();  // WASM trap: prototypeInstaller not initialized
    }
    return _prototypeInstaller->statusProto();
}

void MozJSScriptEngine::setStatus(Status status) {
    _status = std::move(status);
}

bool MozJSScriptEngine::isJavaScriptProtectionEnabled() const {
    // JavaScript protection (--enableJavaScriptProtection) is not applicable
    // in WASM builds where the engine runs in a sandboxed component.
    return false;
}

bool MozJSScriptEngine::requiresOwnedObjects() const {
    // WASM engine does not require BSON objects to be owned.
    return false;
}

void MozJSScriptEngine::newFunction(StringData raw, JS::MutableHandleValue out) {
    JS::RootedObject global(_cx, JS::CurrentGlobalOrNull(_cx));
    std::string parsed;
    if (!parseJSFunctionOrExpression(_cx, global, raw, &parsed))
        return;  // JS exception is pending

    std::string wrapped;
    wrapped.reserve(parsed.size() + 2);
    wrapped += '(';
    wrapped += parsed;
    wrapped += ')';

    JS::CompileOptions opts(_cx);
    opts.setFileAndLine("wasm:newFunction", 1);

    JS::SourceText<mozilla::Utf8Unit> text;
    if (!text.init(_cx, wrapped.data(), wrapped.size(), JS::SourceOwnership::Borrowed)) {
        return;  // JS exception is pending
    }

    JS::Evaluate(_cx, opts, text, out);
    // If Evaluate fails, JS exception is pending — caller handles it.
}

WrapType<BSONInfo>& MozJSScriptEngine::bsonProto() {
    if (!_prototypeInstaller) {
        __builtin_trap();  // WASM trap: prototypeInstaller not initialized
    }
    return _prototypeInstaller->bsonProto();
}

WrapType<DBRefInfo>& MozJSScriptEngine::dbRefProto() {
    if (!_prototypeInstaller) {
        __builtin_trap();  // WASM trap: prototypeInstaller not initialized
    }
    return _prototypeInstaller->dbRefProto();
}

WrapType<RegExpInfo>& MozJSScriptEngine::regExpProto() {
    if (!_prototypeInstaller) {
        __builtin_trap();  // WASM trap: prototypeInstaller not initialized
    }
    return _prototypeInstaller->regExpProto();
}

}  // namespace wasm

}  // namespace mozjs
}  // namespace mongo

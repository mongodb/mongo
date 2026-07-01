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
#include "mongo/scripting/mozjs/common/hex_md5.h"
#include "mongo/scripting/mozjs/common/jsfile.h"
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

#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <string_view>

#include "error.h"
#include "jsapi.h"
#include "jsfriendapi.h"

#include "js/CharacterEncoding.h"
#include "js/CompilationAndEvaluation.h"
#include "js/Conversions.h"
#include "js/ErrorReport.h"
#include "js/Exception.h"
#include "js/GlobalObject.h"
#include "js/Initialization.h"
#include "js/Interrupt.h"
#include "js/PropertyAndElement.h"
#include "js/PropertyDescriptor.h"
#include "js/Realm.h"
#include "js/RealmOptions.h"
#include "js/SourceText.h"
#include "js/Stack.h"
#include "js/String.h"
#include "js/Value.h"
#include "js/ValueArray.h"
#include <mozilla/Utf8.h>

namespace mongo {
namespace JSFiles {
extern const JSFile types;
extern const JSFile assert_wasm;
}  // namespace JSFiles

namespace mozjs {
namespace wasm {

uint32_t g_wasmJsHeapLimitMB = 0;

const char* const kInvokeResult = "__returnValue";

// Default emit byte limit when no explicit limit is configured. Matches the default for
// internalQueryMaxJsEmitBytes; reset by setupEmit() when a real limit is available.
constexpr int64_t kDefaultEmitByteLimitBytes = 16 * 1024 * 1024;

static const JSClass kGlobalClass = {"global", JSCLASS_GLOBAL_FLAGS, &JS::DefaultGlobalClassOps};

// Freeze script shared between parent realm and child realms. Freezing standard
// constructors and prototypes prevents user JS from mutating them across resets.
static const char* const kFreezeBuiltinsScript =
    "(function() {"
    "  var TypedArrayProto = Object.getPrototypeOf(Uint8Array.prototype);"
    "  var targets = ["
    "    Object, Array, Function,"
    "    String, Number, Boolean,"
    "    RegExp, Date, Error,"
    "    TypeError, RangeError, SyntaxError, ReferenceError, URIError, EvalError,"
    "    Object.prototype, Array.prototype, Function.prototype,"
    "    String.prototype, Number.prototype, Boolean.prototype,"
    "    RegExp.prototype, Date.prototype, Error.prototype,"
    "    TypeError.prototype, RangeError.prototype, SyntaxError.prototype,"
    "    ReferenceError.prototype, URIError.prototype, EvalError.prototype,"
    "    Math, JSON,"
    "    Map, Set, WeakMap, WeakSet,"
    "    Map.prototype, Set.prototype,"
    "    WeakMap.prototype, WeakSet.prototype,"
    "    Promise, Promise.prototype,"
    "    Symbol, Symbol.prototype,"
    "    Proxy, Reflect,"
    "    Int8Array, Uint8Array, Uint8ClampedArray,"
    "    Int16Array, Uint16Array,"
    "    Int32Array, Uint32Array,"
    "    Float32Array, Float64Array,"
    "    BigInt64Array, BigUint64Array,"
    "    Int8Array.prototype, Uint8Array.prototype, Uint8ClampedArray.prototype,"
    "    Int16Array.prototype, Uint16Array.prototype,"
    "    Int32Array.prototype, Uint32Array.prototype,"
    "    Float32Array.prototype, Float64Array.prototype,"
    "    BigInt64Array.prototype, BigUint64Array.prototype,"
    "    TypedArrayProto,"
    "    ArrayBuffer, ArrayBuffer.prototype,"
    "    DataView, DataView.prototype"
    "  ];"
    // Guard ES2020+ globals: the embedded SM build may not expose all of them.
    // Using typeof avoids a ReferenceError on absent globals.
    "  if (typeof BigInt !== 'undefined') { targets.push(BigInt, BigInt.prototype); }"
    "  if (typeof WeakRef !== 'undefined') { targets.push(WeakRef, WeakRef.prototype); }"
    "  if (typeof FinalizationRegistry !== 'undefined') {"
    "    targets.push(FinalizationRegistry, FinalizationRegistry.prototype);"
    "  }"
    "  if (typeof Atomics !== 'undefined') { targets.push(Atomics); }"
    // Freezing a target only locks its own property slots (no add/remove/reconfigure);
    // it does NOT freeze the object/function *values* those slots hold. Extension methods
    // types.js attaches to these built-ins (Array.tojson, RegExp.escape, ...) and copyProps()
    // mirrors by reference into every realm are therefore left mutable: one realm could add
    // an own property to e.g. Array.tojson and every other realm sharing that same function
    // object — including future ones — would see it. Freeze each own value (function or
    // object) one level deep, in addition to the container, to close that hole. Descriptors
    // are used (not direct gets) so accessor properties are skipped without invoking them.
    "  var keysOf = function(o) {"
    "    return Object.getOwnPropertyNames(o).concat(Object.getOwnPropertySymbols(o));"
    "  };"
    "  for (var i = 0; i < targets.length; i++) {"
    "    var t = targets[i];"
    "    var keys = keysOf(t);"
    "    for (var j = 0; j < keys.length; j++) {"
    "      var desc = Object.getOwnPropertyDescriptor(t, keys[j]);"
    "      if (!desc || !('value' in desc) || desc.value === null) continue;"
    "      if (typeof desc.value !== 'function' && typeof desc.value !== 'object') continue;"
    "      try { Object.freeze(desc.value); } catch (e) {}"
    "    }"
    "    Object.freeze(t);"
    "  }"
    "})();";

FunctionSlot* MozJSScriptEngine::resolveHandle(uint64_t handle, wasm_mozjs_error_t* err) {
    if (handle == 0 || handle > _slots.size()) {
        if (err) {
            err->code = SM_E_INVALID_ARG;
            set_string(&err->msg, &err->msg_len, "invalid function handle");
        }
        return nullptr;
    }
    FunctionSlot& slot = _slots[handle - 1];
    if (!slot.fn.get().isObject()) {
        if (err) {
            err->code = SM_E_INVALID_ARG;
            set_string(&err->msg, &err->msg_len, "function handle has no function");
        }
        return nullptr;
    }
    return &slot;
}

MozJSScriptEngine::~MozJSScriptEngine() {
    if (_initialized || _cx) {
        shutdown(nullptr);
    }
    if (_smRuntimeInitialized) {
        JS_ShutDown();
        _smRuntimeInitialized = false;
    }
}

err_code_t MozJSScriptEngine::_setupNewGlobal(ExecutionCheck& chk, wasm_mozjs_error_t* err) {
    JS::RealmOptions ro;
    {
        JS::RootedObject g(_cx,
                           chk.okPtr(JS_NewGlobalObject(
                               _cx, &kGlobalClass, nullptr, JS::DontFireOnNewGlobalHook, ro)));
        if (!g)
            return err ? err->code : SM_E_INTERNAL;
        _global = g;
    }

    {
        JSAutoRealm ar(_cx, _global);
        if (!chk.ok(JS::InitRealmStandardClasses(_cx), SM_E_INTERNAL))
            return err ? err->code : SM_E_INTERNAL;
    }

    JS_FireOnNewGlobalObject(_cx, _global);
    if (!chk.ok(!JS_IsExceptionPending(_cx), SM_E_INTERNAL))
        return err ? err->code : SM_E_INTERNAL;

    _prototypeInstaller = std::make_unique<MozJSPrototypeInstaller>(_cx);

    {
        JSAutoRealm ar(_cx, _global);
        _internedStrings = std::make_unique<InternedStringTable>(_cx);
        _prototypeInstaller->installTypes(_global);
        // TODO (SERVER-129747): Freeze MongoDB custom-type prototypes (Timestamp.prototype,
        // ObjectId.prototype, etc.) here after installTypes() so user JS cannot add properties
        // that survive reset(). Same freeze needed in _setupChildRealm() after its installTypes().

        if (!chk.ok(installParseJSFunctionHelper(_cx, _global), SM_E_INTERNAL))
            return err ? err->code : SM_E_INTERNAL;

        // Install MongoDB Array helpers and hex_md5 BEFORE the snapshot so they appear in
        // _initFnProps and survive reset() without re-installation. Also frozen below so
        // user JS cannot overwrite them.
        // hex_md5 is also provided by installGlobalUtils() in the legacy MozJS engine;
        // drivers and user JS rely on it inside $function bodies. Operates on UTF-8 bytes
        // to match the C++ md5_append()-based implementation exactly.
        const char* kArrayHelpers =
            "(function() {"
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
            // hex_md5: UTF-8 byte-oriented to match the C++ native_hex_md5 in utils.cpp.
            "  function hex_md5(s) {"
            "    function safeAdd(x,y){var "
            "l=(x&0xFFFF)+(y&0xFFFF);return((x>>16)+(y>>16)+(l>>16)<<16)|(l&0xFFFF);}"
            "    function rol(n,c){return(n<<c)|(n>>>(32-c));}"
            "    function cmn(q,a,b,x,s,t){return "
            "safeAdd(rol(safeAdd(safeAdd(a,q),safeAdd(x,t)),s),b);}"
            "    function ff(a,b,c,d,x,s,t){return cmn((b&c)|(~b&d),a,b,x,s,t);}"
            "    function gg(a,b,c,d,x,s,t){return cmn((b&d)|(c&~d),a,b,x,s,t);}"
            "    function hh(a,b,c,d,x,s,t){return cmn(b^c^d,a,b,x,s,t);}"
            "    function ii(a,b,c,d,x,s,t){return cmn(c^(b|~d),a,b,x,s,t);}"
            "    var bytes=[];"
            "    for(var i=0;i<s.length;i++){"
            "      var c=s.charCodeAt(i);"
            "      if(c<0x80){bytes.push(c);}"
            "      else if(c<0x800){bytes.push((c>>6)|0xC0,(c&0x3F)|0x80);}"
            "      else if(c>=0xD800&&c<=0xDBFF&&i+1<s.length){"
            "        var d=s.charCodeAt(++i);"
            "        var p=((c&0x3FF)<<10|(d&0x3FF))+0x10000;"
            "        bytes.push((p>>18)|0xF0,((p>>12)&0x3F)|0x80,((p>>6)&0x3F)|0x80,(p&0x3F)|0x80);"
            "      }else{bytes.push((c>>12)|0xE0,((c>>6)&0x3F)|0x80,(c&0x3F)|0x80);}"
            "    }"
            "    var len=bytes.length;"
            "    var extra=((len+8)>>>6<<4)+16;"
            "    var m=new Array(extra).fill(0);"
            "    for(var i=0;i<len;i++)m[i>>2]|=bytes[i]<<((i%4)*8);"
            "    m[len>>2]|=0x80<<((len%4)*8);"
            "    m[extra-2]=len*8;"
            "    var a=1732584193,b=-271733879,c=-1732584194,d=271733878;"
            "    for(var i=0;i<extra;i+=16){"
            "      var A=a,B=b,C=c,D=d;"
            "      a=ff(a,b,c,d,m[i],7,-680876936);d=ff(d,a,b,c,m[i+1],12,-389564586);"
            "      c=ff(c,d,a,b,m[i+2],17,606105819);b=ff(b,c,d,a,m[i+3],22,-1044525330);"
            "      a=ff(a,b,c,d,m[i+4],7,-176418897);d=ff(d,a,b,c,m[i+5],12,1200080426);"
            "      c=ff(c,d,a,b,m[i+6],17,-1473231341);b=ff(b,c,d,a,m[i+7],22,-45705983);"
            "      a=ff(a,b,c,d,m[i+8],7,1770035416);d=ff(d,a,b,c,m[i+9],12,-1958414417);"
            "      c=ff(c,d,a,b,m[i+10],17,-42063);b=ff(b,c,d,a,m[i+11],22,-1990404162);"
            "      a=ff(a,b,c,d,m[i+12],7,1804603682);d=ff(d,a,b,c,m[i+13],12,-40341101);"
            "      c=ff(c,d,a,b,m[i+14],17,-1502002290);b=ff(b,c,d,a,m[i+15],22,1236535329);"
            "      a=gg(a,b,c,d,m[i+1],5,-165796510);d=gg(d,a,b,c,m[i+6],9,-1069501632);"
            "      c=gg(c,d,a,b,m[i+11],14,643717713);b=gg(b,c,d,a,m[i],20,-373897302);"
            "      a=gg(a,b,c,d,m[i+5],5,-701558691);d=gg(d,a,b,c,m[i+10],9,38016083);"
            "      c=gg(c,d,a,b,m[i+15],14,-660478335);b=gg(b,c,d,a,m[i+4],20,-405537848);"
            "      a=gg(a,b,c,d,m[i+9],5,568446438);d=gg(d,a,b,c,m[i+14],9,-1019803690);"
            "      c=gg(c,d,a,b,m[i+3],14,-187363961);b=gg(b,c,d,a,m[i+8],20,1163531501);"
            "      a=gg(a,b,c,d,m[i+13],5,-1444681467);d=gg(d,a,b,c,m[i+2],9,-51403784);"
            "      c=gg(c,d,a,b,m[i+7],14,1735328473);b=gg(b,c,d,a,m[i+12],20,-1926607734);"
            "      a=hh(a,b,c,d,m[i+5],4,-378558);d=hh(d,a,b,c,m[i+8],11,-2022574463);"
            "      c=hh(c,d,a,b,m[i+11],16,1839030562);b=hh(b,c,d,a,m[i+14],23,-35309556);"
            "      a=hh(a,b,c,d,m[i+1],4,-1530992060);d=hh(d,a,b,c,m[i+4],11,1272893353);"
            "      c=hh(c,d,a,b,m[i+7],16,-155497632);b=hh(b,c,d,a,m[i+10],23,-1094730640);"
            "      a=hh(a,b,c,d,m[i+13],4,681279174);d=hh(d,a,b,c,m[i],11,-358537222);"
            "      c=hh(c,d,a,b,m[i+3],16,-722521979);b=hh(b,c,d,a,m[i+6],23,76029189);"
            "      a=hh(a,b,c,d,m[i+9],4,-640364487);d=hh(d,a,b,c,m[i+12],11,-421815835);"
            "      c=hh(c,d,a,b,m[i+15],16,530742520);b=hh(b,c,d,a,m[i+2],23,-995338651);"
            "      a=ii(a,b,c,d,m[i],6,-198630844);d=ii(d,a,b,c,m[i+7],10,1126891415);"
            "      c=ii(c,d,a,b,m[i+14],15,-1416354905);b=ii(b,c,d,a,m[i+5],21,-57434055);"
            "      a=ii(a,b,c,d,m[i+12],6,1700485571);d=ii(d,a,b,c,m[i+3],10,-1894986606);"
            "      c=ii(c,d,a,b,m[i+10],15,-1051523);b=ii(b,c,d,a,m[i+1],21,-2054922799);"
            "      a=ii(a,b,c,d,m[i+8],6,1873313359);d=ii(d,a,b,c,m[i+15],10,-30611744);"
            "      c=ii(c,d,a,b,m[i+6],15,-1560198380);b=ii(b,c,d,a,m[i+13],21,1309151649);"
            "      a=ii(a,b,c,d,m[i+4],6,-145523070);d=ii(d,a,b,c,m[i+11],10,-1120210379);"
            "      c=ii(c,d,a,b,m[i+2],15,718787259);b=ii(b,c,d,a,m[i+9],21,-343485551);"
            "      a=safeAdd(a,A);b=safeAdd(b,B);c=safeAdd(c,C);d=safeAdd(d,D);"
            "    }"
            "    function hexW(n){var h='',x='0123456789abcdef';"
            "      for(var j=0;j<4;j++)h+=x[(n>>j*8+4)&0xF]+x[(n>>j*8)&0xF];return h;}"
            "    return hexW(a)+hexW(b)+hexW(c)+hexW(d);"
            "  }"
            "  globalThis.hex_md5 = hex_md5;"
            "})();";
        JS::CompileOptions helperOpts(_cx);
        helperOpts.setFileAndLine("wasm:init-helpers", 1);
        JS::SourceText<mozilla::Utf8Unit> helperSrc;
        JS::RootedValue helperRval(_cx);
        if (!chk.ok(helperSrc.init(
                        _cx, kArrayHelpers, strlen(kArrayHelpers), JS::SourceOwnership::Borrowed),
                    SM_E_INTERNAL) ||
            !chk.ok(JS::Evaluate(_cx, helperOpts, helperSrc, &helperRval), SM_E_INTERNAL))
            return err ? err->code : SM_E_INTERNAL;

        // Install tojson (from types.js) and the WASM assert library BEFORE the snapshot so
        // they appear in _initGlobalNames and survive reset() in the child realm without
        // re-execution. Placed after installTypes() so BSON types (NumberLong, etc.) are
        // available when these scripts run. The child realm copies them via kGlobalHelpers.
        invariant(exec(JSFiles::types.source, JSFiles::types.name));
        invariant(exec(JSFiles::assert_wasm.source, JSFiles::assert_wasm.name));

        // Snapshot all own property keys (including non-enumerable and symbols) so
        // reset() can distinguish engine-installed names from user JS writes.
        _initGlobalNames.clear();
        JS::RootedVector<JS::PropertyKey> initIds(_cx);
        if (js::GetPropertyKeys(
                _cx, _global, JSITER_OWNONLY | JSITER_HIDDEN | JSITER_SYMBOLS, &initIds)) {
            JS::RootedId initId(_cx);
            for (size_t i = 0; i < initIds.length(); ++i) {
                initId.set(initIds[i]);
                if (initId.isString()) {
                    JS::RootedString rstr(_cx, initId.toString());
                    JS::UniqueChars name = JS_EncodeStringToUTF8(_cx, rstr);
                    if (name)
                        _initGlobalNames.insert(name.get());
                }
            }
        } else {
            // GetPropertyKeys failure during init (e.g. OOM) leaves _initGlobalNames
            // empty, which would cause reset() to skip scrubbing user-added globals —
            // a security regression. Treat this as a hard init failure.
            JS_ClearPendingException(_cx);
            return err ? err->code : SM_E_INTERNAL;
        }

        // Snapshot own properties of each init-time function on globalThis so reset() can
        // scrub only user-added properties, preserving engine-installed ones (Object.keys, etc.).
        _initFnProps.clear();
        {
            JS::RootedValue fnVal(_cx);
            JS::RootedObject fnObj(_cx);
            for (const auto& gname : _initGlobalNames) {
                if (!JS_GetProperty(_cx, _global, gname.c_str(), &fnVal))
                    continue;
                if (!fnVal.isObject())
                    continue;
                fnObj.set(&fnVal.toObject());
                if (!js::IsFunctionObject(fnObj))
                    continue;
                JS::RootedVector<JS::PropertyKey> fnIds(_cx);
                if (!js::GetPropertyKeys(
                        _cx, fnObj, JSITER_OWNONLY | JSITER_HIDDEN | JSITER_SYMBOLS, &fnIds)) {
                    JS_ClearPendingException(_cx);
                    continue;
                }
                auto& nameSet = _initFnProps[gname];
                JS::RootedId fnId(_cx);
                for (size_t i = 0; i < fnIds.length(); ++i) {
                    fnId.set(fnIds[i]);
                    if (!fnId.isString())
                        continue;
                    JS::RootedString rstr(_cx, fnId.toString());
                    JS::UniqueChars pname = JS_EncodeStringToUTF8(_cx, rstr);
                    if (pname)
                        nameSet.insert(pname.get());
                }
            }
        }

        // Freeze after Array helpers so Array.sum etc. become permanently immutable.
        // kFreezeBuiltinsScript is shared with _setupChildRealm.
        JS::CompileOptions freezeOpts(_cx);
        freezeOpts.setFileAndLine("wasm:init-freeze", 1);
        JS::SourceText<mozilla::Utf8Unit> freezeSrc;
        JS::RootedValue freezeRval(_cx);
        if (!chk.ok(freezeSrc.init(_cx,
                                   kFreezeBuiltinsScript,
                                   strlen(kFreezeBuiltinsScript),
                                   JS::SourceOwnership::Borrowed),
                    SM_E_INTERNAL) ||
            !chk.ok(JS::Evaluate(_cx, freezeOpts, freezeSrc, &freezeRval), SM_E_INTERNAL))
            return err ? err->code : SM_E_INTERNAL;
    }

    return SM_OK;
}

err_code_t MozJSScriptEngine::_setupChildRealm(ExecutionCheck& chk, wasm_mozjs_error_t* err) {
    // Same-compartment as parent: no CCW overhead when sharing frozen objects.
    JS::RealmOptions ro;
    ro.creationOptions().setExistingCompartment(_parentGlobal);

    {
        JS::RootedObject g(_cx,
                           chk.okPtr(JS_NewGlobalObject(
                               _cx, &kGlobalClass, nullptr, JS::DontFireOnNewGlobalHook, ro)));
        if (!g)
            return err ? err->code : SM_E_INTERNAL;
        _global = g;
    }

    {
        JSAutoRealm ar(_cx, _global);
        if (!chk.ok(JS::InitRealmStandardClasses(_cx), SM_E_INTERNAL))
            return err ? err->code : SM_E_INTERNAL;
    }

    JS_FireOnNewGlobalObject(_cx, _global);
    if (!chk.ok(!JS_IsExceptionPending(_cx), SM_E_INTERNAL))
        return err ? err->code : SM_E_INTERNAL;

    {
        JSAutoRealm ar(_cx, _global);

        _internedStrings = std::make_unique<InternedStringTable>(_cx);

        // Fresh prototype installer: _proto.init() must be called on a newly constructed
        // PersistentRooted — calling it twice would double-register the GC root.
        _prototypeInstaller = std::make_unique<MozJSPrototypeInstaller>(_cx);
        _prototypeInstaller->installTypes(_global);

        // Fresh per-realm parse helper: copying from the parent would share the object,
        // allowing user JS to attach own properties (e.g. __stash__) that persist across
        // resetRealm(). After freezing, Reflect and __parseJSFunctionOrExpression are
        // deleted from the child's global — they must not appear in user-visible scope.
        // _parseFunctionSource() reads __parseJSFunctionOrExpression from _parentGlobal.
        if (!chk.ok(installParseJSFunctionHelper(_cx, _global), SM_E_INTERNAL))
            return err ? err->code : SM_E_INTERNAL;

        // Mirror the parent realm's globals into the freshly-created child realm. The parent is
        // the single source of truth: it has run the hex_md5 inline script, types.js and
        // assert_wasm.js, and had installTypes() applied, so it holds every helper, Mongo type
        // and built-in extension the child must expose. InitRealmStandardClasses() and the
        // child's own installTypes() already gave it standard built-ins and Mongo type
        // constructors, so we copy only what the child is *missing*:
        //   - top-level globals it lacks (hex_md5, ISODate, tojson, assert, ...),
        //   - own properties types.js added to shared constructors/prototypes that the child's
        //     fresh copies lack (Array.tojson, RegExp.escape, Date.prototype.tojson, ...).
        // The parent is populated only by trusted init scripts, so "copy what the child lacks"
        // introduces nothing dangerous (eval/Function/Proxy/Reflect and the per-realm parse
        // helper already exist in the child and are skipped). Anything that must NOT be mirrored
        // goes in kChildRealmMirrorExclusions; the BuiltinExtensions/GlobalHelpers scope tests
        // fail loudly if a future parent global silently stops reaching the child.
        {
            static constexpr std::string_view kChildRealmMirrorExclusions[] = {
                // BSONAwareMap is the internal implementation of Map used by types.js. It must
                // not appear in user-visible scope; the user-facing name is "Map". The parent
                // realm keeps it as "BSONAwareMap" (unlike the native MozJS shell which renames
                // it to "Map") so that the parent's standard Map slot is unchanged, but the
                // child realm must not expose the implementation name.
                "BSONAwareMap",
            };
            auto isExcluded = [&](const char* name) {
                for (std::string_view e : kChildRealmMirrorExclusions)
                    if (e == name)
                        return true;
                return false;
            };

            // Copy own properties present on `from` but missing on `to`, by *descriptor*. Using
            // descriptors (not get/set) is essential: a value-based copy would invoke getters, and
            // some standard accessors are poison pills that throw on get (Function.prototype.caller
            // / .arguments). Existence is checked with HasOwnProperty so we never read a value
            // either. Members the child already has are left untouched; only the additions land.
            auto copyMissingProps = [&](JS::HandleObject from, JS::HandleObject to) -> bool {
                JS::RootedVector<JS::PropertyKey> ids(_cx);
                if (!js::GetPropertyKeys(_cx, from, JSITER_OWNONLY | JSITER_HIDDEN, &ids))
                    return false;
                JS::RootedId id(_cx);
                JS::Rooted<mozilla::Maybe<JS::PropertyDescriptor>> maybeDesc(_cx);
                for (size_t i = 0; i < ids.length(); ++i) {
                    id.set(ids[i]);
                    bool hasOwn = false;
                    if (!JS_HasOwnPropertyById(_cx, to, id, &hasOwn))
                        return false;
                    if (hasOwn)
                        continue;  // child already has its own — don't clobber standard members
                    if (!JS_GetOwnPropertyDescriptorById(_cx, from, id, &maybeDesc))
                        return false;
                    if (maybeDesc.isNothing())
                        continue;
                    JS::Rooted<JS::PropertyDescriptor> desc(_cx, *maybeDesc);
                    if (!JS_DefinePropertyById(_cx, to, id, desc))
                        return false;
                }
                return true;
            };

            JS::RootedVector<JS::PropertyKey> parentIds(_cx);
            if (!chk.ok(js::GetPropertyKeys(
                            _cx, _parentGlobal, JSITER_OWNONLY | JSITER_HIDDEN, &parentIds),
                        SM_E_JSAPI_FAIL))
                return err ? err->code : SM_E_JSAPI_FAIL;

            JS::RootedId id(_cx);
            JS::Rooted<mozilla::Maybe<JS::PropertyDescriptor>> maybeDesc(_cx);
            JS::RootedValue parentVal(_cx), childVal(_cx);
            for (size_t i = 0; i < parentIds.length(); ++i) {
                id.set(parentIds[i]);
                if (!id.isString())
                    continue;
                JS::RootedString rstr(_cx, id.toString());
                JS::UniqueChars name = JS_EncodeStringToUTF8(_cx, rstr);
                if (!name || isExcluded(name.get()))
                    continue;

                bool childHas = false;
                if (!chk.ok(JS_HasOwnPropertyById(_cx, _global, id, &childHas), SM_E_JSAPI_FAIL))
                    return err ? err->code : SM_E_JSAPI_FAIL;

                if (!childHas) {
                    // A top-level global the child lacks (helper, Mongo type, ...):
                    // copy it by descriptor.
                    if (!chk.ok(JS_GetOwnPropertyDescriptorById(_cx, _parentGlobal, id, &maybeDesc),
                                SM_E_JSAPI_FAIL))
                        return err ? err->code : SM_E_JSAPI_FAIL;
                    if (maybeDesc.isNothing())
                        continue;
                    JS::Rooted<JS::PropertyDescriptor> desc(_cx, *maybeDesc);
                    if (!chk.ok(JS_DefinePropertyById(_cx, _global, id, desc), SM_E_JSAPI_FAIL))
                        return err ? err->code : SM_E_JSAPI_FAIL;
                    continue;
                }

                // Present in both realms. If it's a shared constructor/object, copy the own props
                // types.js added that the child's fresh copy lacks, plus its prototype's. Reading
                // the constructor value is safe — globals are data properties, not accessors.
                if (!JS_GetPropertyById(_cx, _parentGlobal, id, &parentVal) ||
                    !parentVal.isObject())
                    continue;
                if (!JS_GetPropertyById(_cx, _global, id, &childVal) || !childVal.isObject())
                    continue;
                JS::RootedObject pObj(_cx, &parentVal.toObject()), cObj(_cx, &childVal.toObject());
                if (pObj == cObj || pObj == _parentGlobal)
                    continue;  // genuinely shared object, or the globalThis self-reference
                if (!chk.ok(copyMissingProps(pObj, cObj), SM_E_JSAPI_FAIL))
                    return err ? err->code : SM_E_JSAPI_FAIL;

                JS::RootedValue pProto(_cx), cProto(_cx);
                if (JS_GetProperty(_cx, pObj, "prototype", &pProto) && pProto.isObject() &&
                    JS_GetProperty(_cx, cObj, "prototype", &cProto) && cProto.isObject()) {
                    JS::RootedObject pP(_cx, &pProto.toObject()), cP(_cx, &cProto.toObject());
                    if (pP != cP && !chk.ok(copyMissingProps(pP, cP), SM_E_JSAPI_FAIL))
                        return err ? err->code : SM_E_JSAPI_FAIL;
                }
            }
        }

        // Freeze standard constructors and prototypes so mutations don't survive reset().
        // The child realm is dropped on resetRealm(); within a realm's lifetime, reset()
        // is the fast path so freezing is the only protection against cross-request leakage.
        JS::CompileOptions freezeOpts(_cx);
        freezeOpts.setFileAndLine("wasm:child-freeze", 1);
        JS::SourceText<mozilla::Utf8Unit> freezeSrc;
        JS::RootedValue freezeRval(_cx);
        if (!chk.ok(freezeSrc.init(_cx,
                                   kFreezeBuiltinsScript,
                                   strlen(kFreezeBuiltinsScript),
                                   JS::SourceOwnership::Borrowed),
                    SM_E_INTERNAL) ||
            !chk.ok(JS::Evaluate(_cx, freezeOpts, freezeSrc, &freezeRval), SM_E_INTERNAL))
            return err ? err->code : SM_E_INTERNAL;

        // Remove internal helpers from the child's user-visible scope. This runs after
        // the freeze script so that kFreezeBuiltinsScript can reference Reflect by name without
        // getting a ReferenceError.
        {
            JS::ObjectOpResult ignored;
            JS_DeleteProperty(_cx, _global, "__parseJSFunctionOrExpression", ignored);
            JS_DeleteProperty(_cx, _global, "Reflect", ignored);
        }
    }

    return SM_OK;
}

err_code_t MozJSScriptEngine::init(const wasm_mozjs_startup_options_t* opt,
                                   wasm_mozjs_error_t* err) {
    clear_error(err);

    if (_initialized)
        return SM_OK;

    // A shutdown()+init() cycle reuses this instance; don't let a stale pinned-bytes
    // count from the previous lifetime trigger a premature GC in the new one.
    _pinnedHostBytesSinceGc = 0;

    if (!_smRuntimeInitialized) {
        if (!JS_Init()) {
            if (err) {
                err->code = SM_E_INTERNAL;
                set_string(&err->msg, &err->msg_len, "JS_Init failed");
            }
            return SM_E_INTERNAL;
        }
        _smRuntimeInitialized = true;
    }

    constexpr size_t kMaxHeapSizeMB = 2048;
    if (opt->heapSize == 0 || opt->heapSize > kMaxHeapSizeMB) {
        if (err) {
            err->code = SM_E_INVALID_ARG;
            set_string(&err->msg, &err->msg_len, "heapSize out of valid range (1-2048 MB)");
        }
        return SM_E_INVALID_ARG;
    }
    _cx = JS_NewContext(static_cast<size_t>(opt->heapSize) * 1024 * 1024);
    if (!_cx) {
        if (err) {
            err->code = SM_E_OOM;
            set_string(&err->msg, &err->msg_len, "JS_NewContext failed");
        }
        return SM_E_OOM;
    }

    g_wasmJsHeapLimitMB = opt->heapSize;
    _javascriptProtection = opt->javascriptProtection;

    _rt = JS_GetRuntime(_cx);
    if (!_rt) {
        if (err) {
            err->code = SM_E_INTERNAL;
            set_string(&err->msg, &err->msg_len, "JS_GetRuntime returned null");
        }
        JS_DestroyContext(_cx);
        _cx = nullptr;
        return SM_E_INTERNAL;
    }

    ExecutionCheck chk(_cx, err);
    if (!chk.ok(JS::InitSelfHostedCode(_cx), SM_E_INTERNAL)) {
        JS_DestroyContext(_cx);
        _cx = nullptr;
        return err ? err->code : SM_E_INTERNAL;
    }

    JS_SetContextPrivate(_cx, static_cast<MozJSCommonRuntimeInterface*>(this));

    _global.init(_cx);
    _parentGlobal.init(_cx);

    // Parent realm holds frozen built-ins, MongoDB types, Array helpers, and the parse
    // helper. Lives for the lifetime of the JSContext.
    err_code_t rc = _setupNewGlobal(chk, err);
    if (rc != SM_OK) {
        shutdown(nullptr);
        return rc;
    }
    _parentGlobal.set(_global.get());

    // Child realm in the parent's compartment: where user functions are compiled and run.
    rc = _setupChildRealm(chk, err);
    if (rc != SM_OK) {
        shutdown(nullptr);
        return rc;
    }

    _initialized = true;
    return SM_OK;
}

err_code_t MozJSScriptEngine::shutdown(wasm_mozjs_error_t* err) {
    clear_error(err);

    if (!_initialized && !_cx)
        return SM_OK;

    // Drop all PersistentRooted objects while the context is alive (reset() unlinks them
    // from the runtime root list, which requires a live runtime).
    _slots.clear();
    _internedStrings.reset();
    _global.reset();
    _parentGlobal.reset();

    if (_prototypeInstaller)
        _prototypeInstaller->dropRoots();

    if (_cx) {
        // Do NOT null context-private before DestroyContext — GC finalizers need it.
        JS_DestroyContext(_cx);
        _cx = nullptr;
    }

    // All instances have now been finalized; it is safe to free the JSClasses.
    _prototypeInstaller.reset();

    // Do NOT call JS_ShutDown() here. JS_Init/JS_ShutDown are once-per-runtime-lifetime
    // calls; the global g_engine instance is reused across shutdown()+init() cycles (e.g.
    // between requests when a bridge is evicted and rebuilt). The destructor guards this
    // with _smRuntimeInitialized to call JS_ShutDown() exactly once at process exit.

    _rt = nullptr;
    _initialized = false;
    _initGlobalNames.clear();
    _initFnProps.clear();

    return SM_OK;
}

err_code_t MozJSScriptEngine::resetRealm(wasm_mozjs_error_t* err) {
    clear_error(err);
    if (!_initialized || !_cx)
        return SM_E_BAD_STATE;

    _slots.clear();
    _emitBuffer.clear();
    _emitBytesUsed = 0;
    _emitByteLimit = kDefaultEmitByteLimitBytes;
    _status = Status::OK();

    // Release all child-realm GC roots BEFORE allocating the new realm. Without this,
    // the live-set is nearly doubled during JS_NewGlobalObject, risking OOM under
    // sustained rapid resets.
    _prototypeInstaller.reset();
    _internedStrings.reset();
    _global.set(nullptr);

    // Full GC before allocating the new child realm. JS_MaybeGC may not collect the old
    // realm if the heap hasn't hit its trigger threshold; under sustained load this causes
    // JS_NewGlobalObject to OOM. JS_GC blocks ~1-5ms but resetRealm() is once per request.
    JS_GC(_cx);
    _pinnedHostBytesSinceGc = 0;

    // Recreate only the child realm; the parent realm (frozen built-ins, etc.) is preserved.
    ExecutionCheck chk(_cx, err);
    err_code_t rc = _setupChildRealm(chk, err);
    if (rc != SM_OK) {
        _initialized = false;
        return rc;
    }

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

bool MozJSScriptEngine::_parseFunctionSource(std::string_view raw,
                                             std::string* out,
                                             wasm_mozjs_error_t* err) {
    ExecutionCheck chk(_cx, err);
    // __parseJSFunctionOrExpression is deleted from the child realm to keep the
    // user-visible global scope clean; it lives in the parent realm.
    if (!chk.ok(parseJSFunctionOrExpression(_cx, _parentGlobal, raw, out), SM_E_COMPILE)) {
        if (err && err->code == SM_E_PENDING_EXCEPTION)
            err->code = SM_E_COMPILE;
        return false;
    }
    return true;
}

bool MozJSScriptEngine::exec(std::string_view code, const std::string& name) {
    JS::CompileOptions co(_cx);
    co.setFileAndLine(name.c_str(), 1);

    JS::SourceText<mozilla::Utf8Unit> srcBuf;
    bool success = srcBuf.init(_cx, code.data(), code.size(), JS::SourceOwnership::Borrowed);
    if (!success) {
        return false;
    }

    JSScript* scriptPtr = JS::Compile(_cx, co, srcBuf);
    success = scriptPtr != nullptr;

    if (!success) {
        return false;
    }

    JS::RootedValue out(_cx);
    JS::RootedScript script(_cx, scriptPtr);
    success = JS_ExecuteScript(_cx, script, &out);

    if (!success) {
        return false;
    }

    return true;
}

void MozJSScriptEngine::injectNative(const char* field, NativeFunction func, void* data) {
    JS::RootedObject obj(_cx);

    NativeFunctionInfo::make(_cx, &obj, func, data);

    JS::RootedValue value(_cx);
    value.setObjectOrNull(obj);
    ObjectWrapper(_cx, _global).setValue(field, value);
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

    // Pre-validate UTF-8: JS::SourceText::init does not validate; SpiderMonkey silently
    // accepts invalid bytes when converting to a JS string during Evaluate.
    if (!mozilla::IsUtf8(mozilla::Span(reinterpret_cast<const char*>(src), len))) {
        if (err) {
            err->code = SM_E_ENCODING;
            set_string(&err->msg, &err->msg_len, "createFunction: source is not valid UTF-8");
        }
        return SM_E_ENCODING;
    }

    std::string parsed;
    if (!_parseFunctionSource(
            std::string_view(reinterpret_cast<const char*>(src), len), &parsed, err))
        return err ? err->code : SM_E_COMPILE;

    std::string code_str;
    code_str.reserve(parsed.size() + 2);
    code_str += '(';
    code_str += parsed;
    code_str += ')';

    JS::CompileOptions opts(_cx);
    opts.setFileAndLine("wasm:function", 1);

    JS::SourceText<mozilla::Utf8Unit> text;
    if (!chk.ok(text.init(_cx, code_str.data(), code_str.size(), JS::SourceOwnership::Borrowed),
                SM_E_ENCODING)) {
        return err ? err->code : SM_E_ENCODING;
    }

    JS::RootedValue v(_cx);
    if (!chk.ok(JS::Evaluate(_cx, opts, text, &v), SM_E_COMPILE)) {
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
    slot.fn = v;

    *out_handle = static_cast<uint64_t>(_slots.size());  // 1-based

    return SM_OK;
}

void MozJSScriptEngine::_notePinnedHostBytes(int64_t nbytes) {
    _pinnedHostBytesSinceGc += nbytes;
    if (_pinnedHostBytesSinceGc >= kPinnedBytesGcThreshold && _cx) {
        JS_GC(_cx);
        _pinnedHostBytesSinceGc = 0;
    }
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

    _notePinnedHostBytes(argsObject.objsize());

    JSAutoRealm ar(_cx, _global);

    ExecutionCheck chk(_cx, err);

    // Advance the generation before wrapping args so any stale BSON-backed JS objects retained
    // from a prior invocation are detected when accessed during this one.
    ++_generation;

    const int nargs = argsObject.nFields();

    JS::RootedValueVector args(_cx);

    if (nargs) {
        if (!args.reserve(static_cast<size_t>(nargs))) {
            if (err) {
                err->code = SM_E_OOM;
                set_string(&err->msg, &err->msg_len, "invokeFunction: args reserve OOM");
            }
            return SM_E_OOM;
        }
        BSONObjIterator it(argsObject);
        for (int i = 0; i < nargs; i++) {
            BSONElement next = it.next();

            JS::RootedValue value(_cx);
            ValueReader(_cx, &value).fromBSONElementUnowned(next, false /*readOnlyArgs*/);

            if (!args.append(value)) {
                if (err) {
                    err->code = SM_E_INVALID_ARG;
                    set_string(&err->msg, &err->msg_len, "Failed to append property");
                }
                return SM_E_INVALID_ARG;
            }
        }
    }

    // Fresh plain object as `this` so Object.getOwnPropertyNames(this) returns [] inside
    // $function, not the full set of global properties.
    JS::RootedObject emptyThis(_cx, JS_NewPlainObject(_cx));
    if (!emptyThis) {
        if (err) {
            err->code = SM_E_OOM;
            set_string(&err->msg, &err->msg_len, "invokeFunction: failed to allocate 'this'");
        }
        return SM_E_OOM;
    }

    JS::RootedValue out(_cx);

    bool success = JS::Call(_cx, emptyThis, slot->fn, args, &out);

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

    ObjectWrapper(_cx, _global).setValue(kInvokeResult, out);

    if (outBson) {
        if (!out.isObject()) {
            BSONObjBuilder bob;
            ObjectWrapper::WriteFieldRecursionFrames frames;
            ValueWriter(_cx, out).writeThis(&bob, kInvokeResult, &frames);
            *outBson = bob.obj();
        } else {
            // ObjectWrapper::toBSON() processes the WriteFieldRecursionFrames loop; ValueWriter
            // alone only pushes the initial frame.
            JS::RootedObject rout(_cx, JS_NewPlainObject(_cx));
            if (!rout) {
                if (err) {
                    err->code = SM_E_INTERNAL;
                    set_string(&err->msg, &err->msg_len, "JS_NewPlainObject failed (OOM)");
                }
                return SM_E_INTERNAL;
            }
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

    _notePinnedHostBytes(document.objsize());

    JSAutoRealm ar(_cx, _global);
    ExecutionCheck chk(_cx, err);

    // Advance generation so any unowned BSONHolder retained from a prior invokeFunction call
    // is detected as stale if accessed during this predicate.
    ++_generation;

    JS::RootedValue smrecv(_cx);
    if (!document.isEmpty())
        ValueReader(_cx, &smrecv).fromBSON(document, nullptr, false);
    else
        smrecv.setObjectOrNull(_global);

    // Set globalThis.obj and globalThis.fullObject so predicates that access the document
    // via `obj.field` (instead of `this.field`) work without a separate host round-trip.
    // These are cheap JS property sets using the already-deserialized smrecv — no extra
    // BSON serialization needed.
    if (smrecv.isObject()) {
        JS_SetProperty(_cx, _global, "obj", smrecv);
        JS::RootedValue trueVal(_cx);
        trueVal.setBoolean(true);
        JS_SetProperty(_cx, _global, "fullObject", trueVal);
    }

    JS::RootedValueVector args(_cx);

    JS::RootedValue out(_cx);
    JS::RootedObject obj(_cx, smrecv.toObjectOrNull());
    bool success = JS::Call(_cx, obj, slot->fn, args, &out);

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

    _notePinnedHostBytes(document.objsize());

    JSAutoRealm ar(_cx, _global);
    ExecutionCheck chk(_cx, err);

    // Advance generation so any unowned BSONHolder retained from a prior invokeFunction call
    // is detected as stale if accessed during this map invocation.
    ++_generation;

    JS::RootedValue smrecv(_cx);
    if (!document.isEmpty())
        ValueReader(_cx, &smrecv).fromBSON(document, nullptr, false);
    else
        smrecv.setObjectOrNull(_global);

    JS::RootedValueVector args(_cx);

    JS::RootedValue out(_cx);
    JS::RootedObject obj(_cx, smrecv.toObjectOrNull());
    bool success = JS::Call(_cx, obj, slot->fn, args, &out);

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

    _notePinnedHostBytes(value.objsize());

    JSAutoRealm ar(_cx, _global);
    ExecutionCheck chk(_cx, err);

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

    if (rval.isObject()) {
        JS::RootedObject robj(_cx, &rval.toObject());
        ObjectWrapper wrapper(_cx, robj);
        *out = wrapper.toBSON();
    } else {
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

    _notePinnedHostBytes(singleElementDoc.objsize());

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

err_code_t MozJSScriptEngine::reset(wasm_mozjs_error_t* err) {
    clear_error(err);
    if (!_initialized || !_cx || !_global)
        return SM_E_BAD_STATE;

    bool needsRealmReset = false;
    {
        JSAutoRealm ar(_cx, _global);
        ExecutionCheck chk(_cx, err);

        // Delete all own properties of globalThis (including non-enumerable and symbols) not
        // present after init(). _initGlobalNames holds the post-init snapshot. If any property
        // is non-configurable (Object.defineProperty with configurable:false), JS_DeleteProperty
        // silently fails; we detect that via ObjectOpResult and fall back to resetRealm().
        {
            JS::RootedVector<JS::PropertyKey> ids(_cx);
            if (!chk.ok(js::GetPropertyKeys(
                    _cx, _global, JSITER_OWNONLY | JSITER_HIDDEN | JSITER_SYMBOLS, &ids)))
                return err ? err->code : SM_E_JSAPI_FAIL;
            JS::RootedId id(_cx);
            for (size_t i = 0; i < ids.length(); ++i) {
                id.set(ids[i]);
                if (id.isString()) {
                    JS::RootedString rstr(_cx, id.toString());
                    JS::UniqueChars name = JS_EncodeStringToUTF8(_cx, rstr);
                    if (name) {
                        if (_initGlobalNames.find(name.get()) == _initGlobalNames.end()) {
                            JS::ObjectOpResult result;
                            if (!JS_DeleteProperty(_cx, _global, name.get(), result) ||
                                !result.ok()) {
                                JS_ClearPendingException(_cx);
                                needsRealmReset = true;
                                break;
                            }
                        }
                    } else {
                        // Encoding failed (OOM) — fall back to by-id deletion.
                        JS::ObjectOpResult result;
                        if (!JS_DeletePropertyById(_cx, _global, id, result) || !result.ok()) {
                            JS_ClearPendingException(_cx);
                            needsRealmReset = true;
                            break;
                        }
                    }
                } else {
                    // Symbol-keyed property — not in _initGlobalNames, always delete.
                    JS::ObjectOpResult result;
                    if (!JS_DeletePropertyById(_cx, _global, id, result) || !result.ok()) {
                        JS_ClearPendingException(_cx);
                        needsRealmReset = true;
                        break;
                    }
                }
            }
        }

        // Scrub own properties attached to cached function objects in _slots.
        // User code can write properties onto JS functions (e.g. fn.__stash__ = 'secret')
        // which would otherwise survive reset(). Non-configurable properties (prototype,
        // length, name, arguments, caller) are intrinsic engine properties — skip them;
        // never trigger a realm reset from here since _slots must survive reset().
        if (!needsRealmReset) {
            JS::RootedValue fnVal(_cx);
            JS::RootedObject fnObj(_cx);
            for (auto& slot : _slots) {
                fnVal.set(slot.fn.get());
                if (!fnVal.isObject())
                    continue;
                fnObj.set(&fnVal.toObject());
                JS::RootedVector<JS::PropertyKey> fnIds(_cx);
                if (!js::GetPropertyKeys(
                        _cx, fnObj, JSITER_OWNONLY | JSITER_HIDDEN | JSITER_SYMBOLS, &fnIds)) {
                    JS_ClearPendingException(_cx);
                    continue;
                }
                JS::RootedId fid(_cx);
                for (size_t i = 0; i < fnIds.length(); ++i) {
                    fid.set(fnIds[i]);
                    JS::ObjectOpResult result;
                    if (!JS_DeletePropertyById(_cx, fnObj, fid, result) || !result.ok()) {
                        JS_ClearPendingException(_cx);
                        continue;
                    }
                }
            }
        }

        // Scrub user-added own properties from engine-installed function objects on globalThis.
        // Their names survive the deletion pass above (present in _initGlobalNames), but the
        // functions themselves can carry user-set properties. _initFnProps maps each such
        // function name to its init-time properties; only user-added ones are deleted.
        if (!needsRealmReset) {
            JS::RootedValue gval(_cx);
            JS::RootedObject gfnObj(_cx);
            for (const auto& [initName, initProps] : _initFnProps) {
                if (!JS_GetProperty(_cx, _global, initName.c_str(), &gval))
                    continue;
                if (!gval.isObject())
                    continue;
                gfnObj.set(&gval.toObject());
                if (!js::IsFunctionObject(gfnObj))
                    continue;
                JS::RootedVector<JS::PropertyKey> gfnIds(_cx);
                if (!js::GetPropertyKeys(
                        _cx, gfnObj, JSITER_OWNONLY | JSITER_HIDDEN | JSITER_SYMBOLS, &gfnIds)) {
                    JS_ClearPendingException(_cx);
                    continue;
                }
                JS::RootedId gfid(_cx);
                for (size_t i = 0; i < gfnIds.length(); ++i) {
                    gfid.set(gfnIds[i]);
                    if (gfid.isString()) {
                        JS::RootedString rstr(_cx, gfid.toString());
                        JS::UniqueChars pname = JS_EncodeStringToUTF8(_cx, rstr);
                        if (pname && initProps.find(pname.get()) != initProps.end())
                            continue;  // present at init time — keep it
                    }
                    // Symbol-keyed or string-keyed property not present at init — user added it.
                    JS::ObjectOpResult result;
                    if (!JS_DeletePropertyById(_cx, gfnObj, gfid, result) || !result.ok()) {
                        JS_ClearPendingException(_cx);
                        continue;
                    }
                }
            }
        }
    }  // JSAutoRealm ar destroyed here — old child realm exited before resetRealm().

    if (needsRealmReset)
        return resetRealm(err);

    // Compiled JS functions are preserved across resets — bytecodes are independent of
    // user globals, keeping _cachedFunctions valid and avoiding per-document recompilation.

    _emitBuffer.clear();
    _emitBytesUsed = 0;
    _emitByteLimit = kDefaultEmitByteLimitBytes;
    _status = Status::OK();

    // Skip the ~1 ms GC for cheap scopes, but collect when meaningful host-BSON garbage
    // may be pinned by dead proxies (see _notePinnedHostBytes) so a parked, reused bridge
    // returns to a clean linear-memory floor between requests.
    if (_pinnedHostBytesSinceGc >= kPinnedBytesResetGcThreshold) {
        JS_GC(_cx);
        _pinnedHostBytesSinceGc = 0;
    }

    ++_generation;
    return SM_OK;
}

BSONObj MozJSScriptEngine::_emitCallback(const BSONObj& args, void* data) {
    auto* engine = static_cast<MozJSScriptEngine*>(data);

    int nArgs = args.nFields();
    if (nArgs != 2) {
        constexpr int kEmitArgCountCode = 31220;
        uasserted(ErrorCodes::Error(kEmitArgCountCode), "emit takes 2 args");
    }

    BSONObjIterator it(args);
    BSONElement keyElem = it.next();
    BSONElement valElem = it.next();

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

    _emitByteLimit = hasByteLimit ? byteLimit : kDefaultEmitByteLimitBytes;
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

err_code_t MozJSScriptEngine::getMemoryStats(mongo::BSONObj* out, wasm_mozjs_error_t* err) {
    clear_error(err);
    if (!_initialized || !_cx)
        return SM_E_BAD_STATE;
    if (!out)
        return SM_E_INVALID_ARG;

    // memory.size: current size of this instance's linear memory in 64 KiB pages.
    constexpr uint64_t kWasmPageSize = 64 * 1024;
    const uint64_t linearMemoryBytes =
        static_cast<uint64_t>(__builtin_wasm_memory_size(0)) * kWasmPageSize;

    BSONObjBuilder builder;
    builder.append("linearMemoryBytes", static_cast<long long>(linearMemoryBytes));
    builder.append("gcHeapBytes", static_cast<long long>(JS_GetGCParameter(_cx, JSGC_BYTES)));
    builder.append("gcNumber", static_cast<long long>(JS_GetGCParameter(_cx, JSGC_NUMBER)));
    *out = builder.obj();

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
    return _generation;
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
    return _javascriptProtection;
}

bool MozJSScriptEngine::requiresOwnedObjects() const {
    return false;
}

void MozJSScriptEngine::newFunction(std::string_view raw, JS::MutableHandleValue out) {
    // Use the cached helper (not the free parseJSFunctionOrExpression) because
    // __parseJSFunctionOrExpression is removed from the global during init.
    JS::RootedObject global(_cx, JS::CurrentGlobalOrNull(_cx));

    std::string parsed;
    if (!_parseFunctionSource(raw, &parsed, nullptr))
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

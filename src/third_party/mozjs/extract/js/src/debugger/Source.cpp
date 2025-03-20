/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "debugger/Source.h"

#include "mozilla/Assertions.h"  // for AssertionConditionType, MOZ_ASSERT
#include "mozilla/Maybe.h"       // for Some, Maybe, Nothing
#include "mozilla/Variant.h"     // for AsVariant, Variant

#include <stdint.h>  // for uint32_t
#include <string.h>  // for memcpy
#include <utility>   // for move

#include "debugger/Debugger.h"         // for DebuggerSourceReferent, Debugger
#include "debugger/Script.h"           // for DebuggerScript
#include "frontend/FrontendContext.h"  // for AutoReportFrontendContext
#include "gc/Tracer.h"        // for TraceManuallyBarrieredCrossCompartmentEdge
#include "js/ColumnNumber.h"  // JS::WasmFunctionIndex, JS::ColumnNumberOneOrigin
#include "js/CompilationAndEvaluation.h"  // for Compile
#include "js/ErrorReport.h"  // for JS_ReportErrorASCII,  JS_ReportErrorNumberASCII
#include "js/experimental/TypedData.h"  // for JS_NewUint8Array
#include "js/friend/ErrorMessages.h"    // for GetErrorMessage, JSMSG_*
#include "js/GCVariant.h"               // for GCVariant
#include "js/SourceText.h"              // for JS::SourceOwnership
#include "js/String.h"                  // for JS_CopyStringCharsZ
#include "vm/BytecodeUtil.h"            // for JSDVG_SEARCH_STACK
#include "vm/JSContext.h"               // for JSContext (ptr only)
#include "vm/JSObject.h"                // for JSObject, RequireObject
#include "vm/JSScript.h"                // for ScriptSource, ScriptSourceObject
#include "vm/StringType.h"        // for NewStringCopyZ, JSString (ptr only)
#include "vm/TypedArrayObject.h"  // for TypedArrayObject, JSObject::is
#include "wasm/WasmCode.h"        // for Metadata
#include "wasm/WasmDebug.h"       // for DebugState
#include "wasm/WasmInstance.h"    // for Instance
#include "wasm/WasmJS.h"          // for WasmInstanceObject
#include "wasm/WasmTypeDecls.h"   // for Bytes, Rooted<WasmInstanceObject*>

#include "debugger/Debugger-inl.h"  // for Debugger::fromJSObject
#include "vm/JSObject-inl.h"        // for InitClass
#include "vm/NativeObject-inl.h"    // for NewTenuredObjectWithGivenProto
#include "wasm/WasmInstance-inl.h"

namespace js {
class GlobalObject;
}

using namespace js;

using mozilla::AsVariant;
using mozilla::Maybe;
using mozilla::Nothing;
using mozilla::Some;

const JSClassOps DebuggerSource::classOps_ = {
    nullptr,                          // addProperty
    nullptr,                          // delProperty
    nullptr,                          // enumerate
    nullptr,                          // newEnumerate
    nullptr,                          // resolve
    nullptr,                          // mayResolve
    nullptr,                          // finalize
    nullptr,                          // call
    nullptr,                          // construct
    CallTraceMethod<DebuggerSource>,  // trace
};

const JSClass DebuggerSource::class_ = {
    "Source", JSCLASS_HAS_RESERVED_SLOTS(RESERVED_SLOTS), &classOps_};

/* static */
NativeObject* DebuggerSource::initClass(JSContext* cx,
                                        Handle<GlobalObject*> global,
                                        HandleObject debugCtor) {
  return InitClass(cx, debugCtor, nullptr, nullptr, "Source", construct, 0,
                   properties_, methods_, nullptr, nullptr);
}

/* static */
DebuggerSource* DebuggerSource::create(JSContext* cx, HandleObject proto,
                                       Handle<DebuggerSourceReferent> referent,
                                       Handle<NativeObject*> debugger) {
  Rooted<DebuggerSource*> sourceObj(
      cx, NewTenuredObjectWithGivenProto<DebuggerSource>(cx, proto));
  if (!sourceObj) {
    return nullptr;
  }
  sourceObj->setReservedSlot(OWNER_SLOT, ObjectValue(*debugger));
  referent.get().match([&](auto sourceHandle) {
    sourceObj->setReservedSlotGCThingAsPrivate(SOURCE_SLOT, sourceHandle);
  });

  return sourceObj;
}

Debugger* DebuggerSource::owner() const {
  JSObject* dbgobj = &getReservedSlot(OWNER_SLOT).toObject();
  return Debugger::fromJSObject(dbgobj);
}

// For internal use only.
NativeObject* DebuggerSource::getReferentRawObject() const {
  return maybePtrFromReservedSlot<NativeObject>(SOURCE_SLOT);
}

DebuggerSourceReferent DebuggerSource::getReferent() const {
  if (NativeObject* referent = getReferentRawObject()) {
    if (referent->is<ScriptSourceObject>()) {
      return AsVariant(&referent->as<ScriptSourceObject>());
    }
    return AsVariant(&referent->as<WasmInstanceObject>());
  }
  return AsVariant(static_cast<ScriptSourceObject*>(nullptr));
}

void DebuggerSource::trace(JSTracer* trc) {
  // There is a barrier on private pointers, so the Unbarriered marking
  // is okay.
  if (JSObject* referent = getReferentRawObject()) {
    TraceManuallyBarrieredCrossCompartmentEdge(trc, this, &referent,
                                               "Debugger.Source referent");
    if (referent != getReferentRawObject()) {
      setReservedSlotGCThingAsPrivateUnbarriered(SOURCE_SLOT, referent);
    }
  }
}

/* static */
bool DebuggerSource::construct(JSContext* cx, unsigned argc, Value* vp) {
  JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_NO_CONSTRUCTOR,
                            "Debugger.Source");
  return false;
}

/* static */
DebuggerSource* DebuggerSource::check(JSContext* cx, HandleValue thisv) {
  JSObject* thisobj = RequireObject(cx, thisv);
  if (!thisobj) {
    return nullptr;
  }
  if (!thisobj->is<DebuggerSource>()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_INCOMPATIBLE_PROTO, "Debugger.Source",
                              "method", thisobj->getClass()->name);
    return nullptr;
  }

  return &thisobj->as<DebuggerSource>();
}

struct MOZ_STACK_CLASS DebuggerSource::CallData {
  JSContext* cx;
  const CallArgs& args;

  Handle<DebuggerSource*> obj;
  Rooted<DebuggerSourceReferent> referent;

  CallData(JSContext* cx, const CallArgs& args, Handle<DebuggerSource*> obj)
      : cx(cx), args(args), obj(obj), referent(cx, obj->getReferent()) {}

  bool getText();
  bool getBinary();
  bool getURL();
  bool getStartLine();
  bool getStartColumn();
  bool getId();
  bool getDisplayURL();
  bool getElementProperty();
  bool getIntroductionScript();
  bool getIntroductionOffset();
  bool getIntroductionType();
  bool setSourceMapURL();
  bool getSourceMapURL();
  bool reparse();

  using Method = bool (CallData::*)();

  template <Method MyMethod>
  static bool ToNative(JSContext* cx, unsigned argc, Value* vp);
};

template <DebuggerSource::CallData::Method MyMethod>
/* static */
bool DebuggerSource::CallData::ToNative(JSContext* cx, unsigned argc,
                                        Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  Rooted<DebuggerSource*> obj(cx, DebuggerSource::check(cx, args.thisv()));
  if (!obj) {
    return false;
  }

  CallData data(cx, args, obj);
  return (data.*MyMethod)();
}

class DebuggerSourceGetTextMatcher {
  JSContext* cx_;

 public:
  explicit DebuggerSourceGetTextMatcher(JSContext* cx) : cx_(cx) {}

  using ReturnType = JSString*;

  ReturnType match(Handle<ScriptSourceObject*> sourceObject) {
    ScriptSource* ss = sourceObject->source();
    bool hasSourceText;
    if (!ScriptSource::loadSource(cx_, ss, &hasSourceText)) {
      return nullptr;
    }
    if (!hasSourceText) {
      return NewStringCopyZ<CanGC>(cx_, "[no source]");
    }

    // In case of DOM event handler like <div onclick="foo()" the JS code is
    // wrapped into
    //   function onclick() {foo()}
    // We want to only return `foo()` here.
    // But only for event handlers, for `new Function("foo()")`, we want to
    // return:
    //   function anonymous() {foo()}
    if (ss->hasIntroductionType() &&
        strcmp(ss->introductionType(), "eventHandler") == 0 &&
        ss->isFunctionBody()) {
      return ss->functionBodyString(cx_);
    }

    return ss->substring(cx_, 0, ss->length());
  }

  ReturnType match(Handle<WasmInstanceObject*> instanceObj) {
    wasm::Instance& instance = instanceObj->instance();
    const char* msg;
    if (!instance.debugEnabled()) {
      msg = "Restart with developer tools open to view WebAssembly source.";
    } else {
      msg = "[debugger missing wasm binary-to-text conversion]";
    }
    return NewStringCopyZ<CanGC>(cx_, msg);
  }
};

bool DebuggerSource::CallData::getText() {
  Value textv = obj->getReservedSlot(TEXT_SLOT);
  if (!textv.isUndefined()) {
    MOZ_ASSERT(textv.isString());
    args.rval().set(textv);
    return true;
  }

  DebuggerSourceGetTextMatcher matcher(cx);
  JSString* str = referent.match(matcher);
  if (!str) {
    return false;
  }

  args.rval().setString(str);
  obj->setReservedSlot(TEXT_SLOT, args.rval());
  return true;
}

bool DebuggerSource::CallData::getBinary() {
  if (!referent.is<WasmInstanceObject*>()) {
    ReportValueError(cx, JSMSG_DEBUG_BAD_REFERENT, JSDVG_SEARCH_STACK,
                     args.thisv(), nullptr, "a wasm source");
    return false;
  }

  Rooted<WasmInstanceObject*> instanceObj(cx,
                                          referent.as<WasmInstanceObject*>());
  wasm::Instance& instance = instanceObj->instance();

  if (!instance.debugEnabled()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_DEBUG_NO_BINARY_SOURCE);
    return false;
  }

  const wasm::Bytes& bytecode = instance.debug().bytecode();
  RootedObject arr(cx, JS_NewUint8Array(cx, bytecode.length()));
  if (!arr) {
    return false;
  }

  memcpy(arr->as<TypedArrayObject>().dataPointerUnshared(), bytecode.begin(),
         bytecode.length());

  args.rval().setObject(*arr);
  return true;
}

class DebuggerSourceGetURLMatcher {
  JSContext* cx_;

 public:
  explicit DebuggerSourceGetURLMatcher(JSContext* cx) : cx_(cx) {}

  using ReturnType = Maybe<JSString*>;

  ReturnType match(Handle<ScriptSourceObject*> sourceObject) {
    ScriptSource* ss = sourceObject->source();
    MOZ_ASSERT(ss);
    if (const char* filename = ss->filename()) {
      JS::UTF8Chars utf8chars(filename, strlen(filename));
      JSString* str = NewStringCopyUTF8N(cx_, utf8chars);
      return Some(str);
    }
    return Nothing();
  }
  ReturnType match(Handle<WasmInstanceObject*> instanceObj) {
    return Some(instanceObj->instance().createDisplayURL(cx_));
  }
};

bool DebuggerSource::CallData::getURL() {
  DebuggerSourceGetURLMatcher matcher(cx);
  Maybe<JSString*> str = referent.match(matcher);
  if (str.isSome()) {
    if (!*str) {
      return false;
    }
    args.rval().setString(*str);
  } else {
    args.rval().setNull();
  }
  return true;
}

class DebuggerSourceGetStartLineMatcher {
 public:
  using ReturnType = uint32_t;

  ReturnType match(Handle<ScriptSourceObject*> sourceObject) {
    ScriptSource* ss = sourceObject->source();
    return ss->startLine();
  }
  ReturnType match(Handle<WasmInstanceObject*> instanceObj) { return 0; }
};

bool DebuggerSource::CallData::getStartLine() {
  DebuggerSourceGetStartLineMatcher matcher;
  uint32_t line = referent.match(matcher);
  args.rval().setNumber(line);
  return true;
}

class DebuggerSourceGetStartColumnMatcher {
 public:
  using ReturnType = JS::LimitedColumnNumberOneOrigin;

  ReturnType match(Handle<ScriptSourceObject*> sourceObject) {
    ScriptSource* ss = sourceObject->source();
    return ss->startColumn();
  }
  ReturnType match(Handle<WasmInstanceObject*> instanceObj) {
    return JS::LimitedColumnNumberOneOrigin(
        JS::WasmFunctionIndex::DefaultBinarySourceColumnNumberOneOrigin);
  }
};

bool DebuggerSource::CallData::getStartColumn() {
  DebuggerSourceGetStartColumnMatcher matcher;
  JS::LimitedColumnNumberOneOrigin column = referent.match(matcher);
  args.rval().setNumber(column.oneOriginValue());
  return true;
}

class DebuggerSourceGetIdMatcher {
 public:
  using ReturnType = uint32_t;

  ReturnType match(Handle<ScriptSourceObject*> sourceObject) {
    ScriptSource* ss = sourceObject->source();
    return ss->id();
  }
  ReturnType match(Handle<WasmInstanceObject*> instanceObj) { return 0; }
};

bool DebuggerSource::CallData::getId() {
  DebuggerSourceGetIdMatcher matcher;
  uint32_t id = referent.match(matcher);
  args.rval().setNumber(id);
  return true;
}

struct DebuggerSourceGetDisplayURLMatcher {
  using ReturnType = const char16_t*;
  ReturnType match(Handle<ScriptSourceObject*> sourceObject) {
    ScriptSource* ss = sourceObject->source();
    MOZ_ASSERT(ss);
    return ss->hasDisplayURL() ? ss->displayURL() : nullptr;
  }
  ReturnType match(Handle<WasmInstanceObject*> wasmInstance) {
    return wasmInstance->instance().metadata().displayURL();
  }
};

bool DebuggerSource::CallData::getDisplayURL() {
  DebuggerSourceGetDisplayURLMatcher matcher;
  if (const char16_t* displayURL = referent.match(matcher)) {
    JSString* str = JS_NewUCStringCopyZ(cx, displayURL);
    if (!str) {
      return false;
    }
    args.rval().setString(str);
  } else {
    args.rval().setNull();
  }
  return true;
}

struct DebuggerSourceGetElementPropertyMatcher {
  using ReturnType = Value;
  ReturnType match(Handle<ScriptSourceObject*> sourceObject) {
    return sourceObject->unwrappedElementAttributeName();
  }
  ReturnType match(Handle<WasmInstanceObject*> wasmInstance) {
    return UndefinedValue();
  }
};

bool DebuggerSource::CallData::getElementProperty() {
  DebuggerSourceGetElementPropertyMatcher matcher;
  args.rval().set(referent.match(matcher));
  return obj->owner()->wrapDebuggeeValue(cx, args.rval());
}

class DebuggerSourceGetIntroductionScriptMatcher {
  JSContext* cx_;
  Debugger* dbg_;
  MutableHandleValue rval_;

 public:
  DebuggerSourceGetIntroductionScriptMatcher(JSContext* cx, Debugger* dbg,
                                             MutableHandleValue rval)
      : cx_(cx), dbg_(dbg), rval_(rval) {}

  using ReturnType = bool;

  ReturnType match(Handle<ScriptSourceObject*> sourceObject) {
    Rooted<BaseScript*> script(cx_,
                               sourceObject->unwrappedIntroductionScript());
    if (script) {
      RootedObject scriptDO(cx_, dbg_->wrapScript(cx_, script));
      if (!scriptDO) {
        return false;
      }
      rval_.setObject(*scriptDO);
    } else {
      rval_.setUndefined();
    }
    return true;
  }

  ReturnType match(Handle<WasmInstanceObject*> wasmInstance) {
    RootedObject ds(cx_, dbg_->wrapWasmScript(cx_, wasmInstance));
    if (!ds) {
      return false;
    }
    rval_.setObject(*ds);
    return true;
  }
};

bool DebuggerSource::CallData::getIntroductionScript() {
  Debugger* dbg = obj->owner();
  DebuggerSourceGetIntroductionScriptMatcher matcher(cx, dbg, args.rval());
  return referent.match(matcher);
}

struct DebuggerGetIntroductionOffsetMatcher {
  using ReturnType = Value;
  ReturnType match(Handle<ScriptSourceObject*> sourceObject) {
    // Regardless of what's recorded in the ScriptSourceObject and
    // ScriptSource, only hand out the introduction offset if we also have
    // the script within which it applies.
    ScriptSource* ss = sourceObject->source();
    if (ss->hasIntroductionOffset() &&
        sourceObject->unwrappedIntroductionScript()) {
      return Int32Value(ss->introductionOffset());
    }
    return UndefinedValue();
  }
  ReturnType match(Handle<WasmInstanceObject*> wasmInstance) {
    return UndefinedValue();
  }
};

bool DebuggerSource::CallData::getIntroductionOffset() {
  DebuggerGetIntroductionOffsetMatcher matcher;
  args.rval().set(referent.match(matcher));
  return true;
}

struct DebuggerSourceGetIntroductionTypeMatcher {
  using ReturnType = const char*;
  ReturnType match(Handle<ScriptSourceObject*> sourceObject) {
    ScriptSource* ss = sourceObject->source();
    MOZ_ASSERT(ss);
    return ss->hasIntroductionType() ? ss->introductionType() : nullptr;
  }
  ReturnType match(Handle<WasmInstanceObject*> wasmInstance) { return "wasm"; }
};

bool DebuggerSource::CallData::getIntroductionType() {
  DebuggerSourceGetIntroductionTypeMatcher matcher;
  if (const char* introductionType = referent.match(matcher)) {
    JSString* str = NewStringCopyZ<CanGC>(cx, introductionType);
    if (!str) {
      return false;
    }
    args.rval().setString(str);
  } else {
    args.rval().setUndefined();
  }

  return true;
}

ScriptSourceObject* EnsureSourceObject(JSContext* cx,
                                       Handle<DebuggerSource*> obj) {
  if (!obj->getReferent().is<ScriptSourceObject*>()) {
    RootedValue v(cx, ObjectValue(*obj));
    ReportValueError(cx, JSMSG_DEBUG_BAD_REFERENT, JSDVG_SEARCH_STACK, v,
                     nullptr, "a JS source");
    return nullptr;
  }
  return obj->getReferent().as<ScriptSourceObject*>();
}

bool DebuggerSource::CallData::setSourceMapURL() {
  Rooted<ScriptSourceObject*> sourceObject(cx, EnsureSourceObject(cx, obj));
  if (!sourceObject) {
    return false;
  }
  ScriptSource* ss = sourceObject->source();
  MOZ_ASSERT(ss);

  if (!args.requireAtLeast(cx, "set sourceMapURL", 1)) {
    return false;
  }

  JSString* str = ToString<CanGC>(cx, args[0]);
  if (!str) {
    return false;
  }

  UniqueTwoByteChars chars = JS_CopyStringCharsZ(cx, str);
  if (!chars) {
    return false;
  }

  AutoReportFrontendContext fc(cx);
  if (!ss->setSourceMapURL(&fc, std::move(chars))) {
    return false;
  }

  args.rval().setUndefined();
  return true;
}

class DebuggerSourceGetSourceMapURLMatcher {
  JSContext* cx_;
  MutableHandleString result_;

 public:
  explicit DebuggerSourceGetSourceMapURLMatcher(JSContext* cx,
                                                MutableHandleString result)
      : cx_(cx), result_(result) {}

  using ReturnType = bool;
  ReturnType match(Handle<ScriptSourceObject*> sourceObject) {
    ScriptSource* ss = sourceObject->source();
    MOZ_ASSERT(ss);
    if (!ss->hasSourceMapURL()) {
      result_.set(nullptr);
      return true;
    }
    JSString* str = JS_NewUCStringCopyZ(cx_, ss->sourceMapURL());
    if (!str) {
      return false;
    }
    result_.set(str);
    return true;
  }
  ReturnType match(Handle<WasmInstanceObject*> instanceObj) {
    wasm::Instance& instance = instanceObj->instance();
    if (!instance.debugEnabled()) {
      result_.set(nullptr);
      return true;
    }

    RootedString str(cx_);
    if (!instance.debug().getSourceMappingURL(cx_, &str)) {
      return false;
    }

    result_.set(str);
    return true;
  }
};

bool DebuggerSource::CallData::getSourceMapURL() {
  RootedString result(cx);
  DebuggerSourceGetSourceMapURLMatcher matcher(cx, &result);
  if (!referent.match(matcher)) {
    return false;
  }
  if (result) {
    args.rval().setString(result);
  } else {
    args.rval().setNull();
  }
  return true;
}

template <typename Unit>
static JSScript* ReparseSource(JSContext* cx, Handle<ScriptSourceObject*> sso) {
  AutoRealm ar(cx, sso);
  ScriptSource* ss = sso->source();

  JS::CompileOptions options(cx);
  options.setHideScriptFromDebugger(true);
  options.setFileAndLine(ss->filename(), ss->startLine());
  options.setColumn(JS::ColumnNumberOneOrigin(ss->startColumn()));

  UncompressedSourceCache::AutoHoldEntry holder;

  ScriptSource::PinnedUnits<Unit> units(cx, ss, holder, 0, ss->length());
  if (!units.get()) {
    return nullptr;
  }

  JS::SourceText<Unit> srcBuf;
  if (!srcBuf.init(cx, units.get(), ss->length(),
                   JS::SourceOwnership::Borrowed)) {
    return nullptr;
  }

  return JS::Compile(cx, options, srcBuf);
}

bool DebuggerSource::CallData::reparse() {
  Rooted<ScriptSourceObject*> sourceObject(cx, EnsureSourceObject(cx, obj));
  if (!sourceObject) {
    return false;
  }

  if (!sourceObject->source()->hasSourceText()) {
    JS_ReportErrorASCII(cx, "Source object missing text");
    return false;
  }

  RootedScript script(cx);
  if (sourceObject->source()->hasSourceType<mozilla::Utf8Unit>()) {
    script = ReparseSource<mozilla::Utf8Unit>(cx, sourceObject);
  } else {
    script = ReparseSource<char16_t>(cx, sourceObject);
  }

  if (!script) {
    return false;
  }

  Debugger* dbg = obj->owner();
  RootedObject scriptDO(cx, dbg->wrapScript(cx, script));
  if (!scriptDO) {
    return false;
  }

  args.rval().setObject(*scriptDO);
  return true;
}

const JSPropertySpec DebuggerSource::properties_[] = {
    JS_DEBUG_PSG("text", getText),
    JS_DEBUG_PSG("binary", getBinary),
    JS_DEBUG_PSG("url", getURL),
    JS_DEBUG_PSG("startLine", getStartLine),
    JS_DEBUG_PSG("startColumn", getStartColumn),
    JS_DEBUG_PSG("id", getId),
    JS_DEBUG_PSG("displayURL", getDisplayURL),
    JS_DEBUG_PSG("introductionScript", getIntroductionScript),
    JS_DEBUG_PSG("introductionOffset", getIntroductionOffset),
    JS_DEBUG_PSG("introductionType", getIntroductionType),
    JS_DEBUG_PSG("elementAttributeName", getElementProperty),
    JS_DEBUG_PSGS("sourceMapURL", getSourceMapURL, setSourceMapURL),
    JS_PS_END};

const JSFunctionSpec DebuggerSource::methods_[] = {
    JS_DEBUG_FN("reparse", reparse, 0), JS_FS_END};

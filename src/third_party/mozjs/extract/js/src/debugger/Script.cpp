/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "debugger/Script-inl.h"

#include "mozilla/Maybe.h"   // for Some, Maybe
#include "mozilla/Span.h"    // for Span
#include "mozilla/Vector.h"  // for Vector

#include <stddef.h>  // for ptrdiff_t
#include <stdint.h>  // for uint32_t, UINT32_MAX, SIZE_MAX, int32_t

#include "jsnum.h"             // for ToNumber
#include "NamespaceImports.h"  // for CallArgs, RootedValue

#include "builtin/Array.h"         // for NewDenseEmptyArray
#include "debugger/Debugger.h"     // for DebuggerScriptReferent, Debugger
#include "debugger/DebugScript.h"  // for DebugScript
#include "debugger/Source.h"       // for DebuggerSource
#include "gc/GC.h"                 // for MemoryUse, MemoryUse::Breakpoint
#include "gc/Tracer.h"         // for TraceManuallyBarrieredCrossCompartmentEdge
#include "gc/Zone.h"           // for Zone
#include "gc/ZoneAllocator.h"  // for AddCellMemory
#include "js/CallArgs.h"       // for CallArgs, CallArgsFromVp
#include "js/ColumnNumber.h"  // JS::LimitedColumnNumberOneOrigin, JS::WasmFunctionIndex
#include "js/friend/ErrorMessages.h"  // for GetErrorMessage, JSMSG_*
#include "js/GCVariant.h"             // for GCVariant
#include "js/HeapAPI.h"               // for GCCellPtr
#include "js/RootingAPI.h"            // for Rooted
#include "js/Wrapper.h"               // for UncheckedUnwrap
#include "vm/ArrayObject.h"           // for ArrayObject
#include "vm/BytecodeUtil.h"          // for GET_JUMP_OFFSET
#include "vm/Compartment.h"           // for JS::Compartment
#include "vm/EnvironmentObject.h"     // for EnvironmentCoordinateNameSlow
#include "vm/GlobalObject.h"          // for GlobalObject
#include "vm/JSContext.h"             // for JSContext, ReportValueError
#include "vm/JSFunction.h"            // for JSFunction
#include "vm/JSObject.h"              // for RequireObject, JSObject
#include "vm/JSScript.h"              // for BaseScript
#include "vm/ObjectOperations.h"      // for DefineDataProperty, HasOwnProperty
#include "vm/PlainObject.h"           // for js::PlainObject
#include "vm/Realm.h"                 // for AutoRealm
#include "vm/Runtime.h"               // for JSAtomState, JSRuntime
#include "vm/StringType.h"            // for NameToId, PropertyName, JSAtom
#include "wasm/WasmDebug.h"           // for ExprLoc, DebugState
#include "wasm/WasmInstance.h"        // for Instance
#include "wasm/WasmJS.h"              // for WasmInstanceObject
#include "wasm/WasmTypeDecls.h"       // for Bytes

#include "vm/BytecodeUtil-inl.h"  // for BytecodeRangeWithPosition
#include "vm/JSAtomUtils-inl.h"   // for PrimitiveValueToId
#include "vm/JSObject-inl.h"  // for NewBuiltinClassInstance, NewObjectWithGivenProto, NewTenuredObjectWithGivenProto
#include "vm/JSScript-inl.h"  // for JSScript::global
#include "vm/ObjectOperations-inl.h"  // for GetProperty
#include "vm/Realm-inl.h"             // for AutoRealm::AutoRealm

using namespace js;

using mozilla::Maybe;
using mozilla::Some;

const JSClassOps DebuggerScript::classOps_ = {
    nullptr,                          // addProperty
    nullptr,                          // delProperty
    nullptr,                          // enumerate
    nullptr,                          // newEnumerate
    nullptr,                          // resolve
    nullptr,                          // mayResolve
    nullptr,                          // finalize
    nullptr,                          // call
    nullptr,                          // construct
    CallTraceMethod<DebuggerScript>,  // trace
};

const JSClass DebuggerScript::class_ = {
    "Script", JSCLASS_HAS_RESERVED_SLOTS(RESERVED_SLOTS), &classOps_};

void DebuggerScript::trace(JSTracer* trc) {
  // This comes from a private pointer, so no barrier needed.
  gc::Cell* cell = getReferentCell();
  if (cell) {
    if (cell->is<BaseScript>()) {
      BaseScript* script = cell->as<BaseScript>();
      TraceManuallyBarrieredCrossCompartmentEdge(
          trc, this, &script, "Debugger.Script script referent");
      if (script != cell->as<BaseScript>()) {
        setReservedSlotGCThingAsPrivateUnbarriered(SCRIPT_SLOT, script);
      }
    } else {
      JSObject* wasm = cell->as<JSObject>();
      TraceManuallyBarrieredCrossCompartmentEdge(
          trc, this, &wasm, "Debugger.Script wasm referent");
      if (wasm != cell->as<JSObject>()) {
        MOZ_ASSERT(wasm->is<WasmInstanceObject>());
        setReservedSlotGCThingAsPrivateUnbarriered(SCRIPT_SLOT, wasm);
      }
    }
  }
}

/* static */
NativeObject* DebuggerScript::initClass(JSContext* cx,
                                        Handle<GlobalObject*> global,
                                        HandleObject debugCtor) {
  return InitClass(cx, debugCtor, nullptr, nullptr, "Script", construct, 0,
                   properties_, methods_, nullptr, nullptr);
}

/* static */
DebuggerScript* DebuggerScript::create(JSContext* cx, HandleObject proto,
                                       Handle<DebuggerScriptReferent> referent,
                                       Handle<NativeObject*> debugger) {
  DebuggerScript* scriptobj =
      NewTenuredObjectWithGivenProto<DebuggerScript>(cx, proto);
  if (!scriptobj) {
    return nullptr;
  }

  scriptobj->setReservedSlot(DebuggerScript::OWNER_SLOT,
                             ObjectValue(*debugger));
  referent.get().match([&](auto& scriptHandle) {
    scriptobj->setReservedSlotGCThingAsPrivate(SCRIPT_SLOT, scriptHandle);
  });

  return scriptobj;
}

static JSScript* DelazifyScript(JSContext* cx, Handle<BaseScript*> script) {
  if (script->hasBytecode()) {
    return script->asJSScript();
  }
  MOZ_ASSERT(script->isFunction());

  // JSFunction::getOrCreateScript requires an enclosing scope. This requires
  // the enclosing script to be non-lazy.
  if (script->hasEnclosingScript()) {
    Rooted<BaseScript*> enclosingScript(cx, script->enclosingScript());
    if (!DelazifyScript(cx, enclosingScript)) {
      return nullptr;
    }

    if (!script->isReadyForDelazification()) {
      // It didn't work! Delazifying the enclosing script still didn't
      // delazify this script. This happens when the function
      // corresponding to this script was removed by constant folding.
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_DEBUG_OPTIMIZED_OUT_FUN);
      return nullptr;
    }
  }

  MOZ_ASSERT(script->enclosingScope());

  RootedFunction fun(cx, script->function());
  AutoRealm ar(cx, fun);
  return JSFunction::getOrCreateScript(cx, fun);
}

/* static */
DebuggerScript* DebuggerScript::check(JSContext* cx, HandleValue v) {
  JSObject* thisobj = RequireObject(cx, v);
  if (!thisobj) {
    return nullptr;
  }
  if (!thisobj->is<DebuggerScript>()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_INCOMPATIBLE_PROTO, "Debugger.Script",
                              "method", thisobj->getClass()->name);
    return nullptr;
  }

  return &thisobj->as<DebuggerScript>();
}

struct MOZ_STACK_CLASS DebuggerScript::CallData {
  JSContext* cx;
  const CallArgs& args;

  Handle<DebuggerScript*> obj;
  Rooted<DebuggerScriptReferent> referent;
  RootedScript script;

  CallData(JSContext* cx, const CallArgs& args, Handle<DebuggerScript*> obj)
      : cx(cx),
        args(args),
        obj(obj),
        referent(cx, obj->getReferent()),
        script(cx) {}

  [[nodiscard]] bool ensureScriptMaybeLazy() {
    if (!referent.is<BaseScript*>()) {
      ReportValueError(cx, JSMSG_DEBUG_BAD_REFERENT, JSDVG_SEARCH_STACK,
                       args.thisv(), nullptr, "a JS script");
      return false;
    }
    return true;
  }

  [[nodiscard]] bool ensureScript() {
    if (!ensureScriptMaybeLazy()) {
      return false;
    }
    script = DelazifyScript(cx, referent.as<BaseScript*>());
    if (!script) {
      return false;
    }
    return true;
  }

  bool getIsGeneratorFunction();
  bool getIsAsyncFunction();
  bool getIsFunction();
  bool getIsModule();
  bool getDisplayName();
  bool getParameterNames();
  bool getUrl();
  bool getStartLine();
  bool getStartColumn();
  bool getLineCount();
  bool getSource();
  bool getSourceStart();
  bool getSourceLength();
  bool getMainOffset();
  bool getGlobal();
  bool getFormat();
  bool getChildScripts();
  bool getPossibleBreakpoints();
  bool getPossibleBreakpointOffsets();
  bool getOffsetMetadata();
  bool getOffsetLocation();
  bool getEffectfulOffsets();
  bool getAllOffsets();
  bool getAllColumnOffsets();
  bool getLineOffsets();
  bool setBreakpoint();
  bool getBreakpoints();
  bool clearBreakpoint();
  bool clearAllBreakpoints();
  bool isInCatchScope();
  bool getOffsetsCoverage();

  using Method = bool (CallData::*)();

  template <Method MyMethod>
  static bool ToNative(JSContext* cx, unsigned argc, Value* vp);
};

template <DebuggerScript::CallData::Method MyMethod>
/* static */
bool DebuggerScript::CallData::ToNative(JSContext* cx, unsigned argc,
                                        Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  Rooted<DebuggerScript*> obj(cx, DebuggerScript::check(cx, args.thisv()));
  if (!obj) {
    return false;
  }

  CallData data(cx, args, obj);
  return (data.*MyMethod)();
}

bool DebuggerScript::CallData::getIsGeneratorFunction() {
  if (!ensureScriptMaybeLazy()) {
    return false;
  }
  args.rval().setBoolean(obj->getReferentScript()->isGenerator());
  return true;
}

bool DebuggerScript::CallData::getIsAsyncFunction() {
  if (!ensureScriptMaybeLazy()) {
    return false;
  }
  args.rval().setBoolean(obj->getReferentScript()->isAsync());
  return true;
}

bool DebuggerScript::CallData::getIsFunction() {
  if (!ensureScriptMaybeLazy()) {
    return false;
  }

  args.rval().setBoolean(obj->getReferentScript()->function());
  return true;
}

bool DebuggerScript::CallData::getIsModule() {
  if (!ensureScriptMaybeLazy()) {
    return false;
  }
  BaseScript* script = referent.as<BaseScript*>();

  args.rval().setBoolean(script->isModule());
  return true;
}

bool DebuggerScript::CallData::getDisplayName() {
  if (!ensureScriptMaybeLazy()) {
    return false;
  }

  JSFunction* func = obj->getReferentScript()->function();
  if (!func) {
    args.rval().setUndefined();
    return true;
  }

  JSAtom* name = func->fullDisplayAtom();
  if (!name) {
    args.rval().setUndefined();
    return true;
  }

  RootedValue namev(cx, StringValue(name));
  Debugger* dbg = obj->owner();
  if (!dbg->wrapDebuggeeValue(cx, &namev)) {
    return false;
  }
  args.rval().set(namev);
  return true;
}

bool DebuggerScript::CallData::getParameterNames() {
  if (!ensureScript()) {
    return false;
  }

  RootedFunction fun(cx, referent.as<BaseScript*>()->function());
  if (!fun) {
    args.rval().setUndefined();
    return true;
  }

  ArrayObject* arr = GetFunctionParameterNamesArray(cx, fun);
  if (!arr) {
    return false;
  }

  args.rval().setObject(*arr);
  return true;
}

bool DebuggerScript::CallData::getUrl() {
  if (!ensureScriptMaybeLazy()) {
    return false;
  }

  Rooted<BaseScript*> script(cx, referent.as<BaseScript*>());

  if (script->filename()) {
    JSString* str;
    if (const char* introducer = script->scriptSource()->introducerFilename()) {
      str =
          NewStringCopyUTF8N(cx, JS::UTF8Chars(introducer, strlen(introducer)));
    } else {
      const char* filename = script->filename();
      str = NewStringCopyUTF8N(cx, JS::UTF8Chars(filename, strlen(filename)));
    }
    if (!str) {
      return false;
    }
    args.rval().setString(str);
  } else {
    args.rval().setNull();
  }
  return true;
}

bool DebuggerScript::CallData::getStartLine() {
  args.rval().setNumber(
      referent.get().match([](BaseScript*& s) { return s->lineno(); },
                           [](WasmInstanceObject*&) { return (uint32_t)1; }));
  return true;
}

bool DebuggerScript::CallData::getStartColumn() {
  JS::LimitedColumnNumberOneOrigin column = referent.get().match(
      [](BaseScript*& s) { return s->column(); },
      [](WasmInstanceObject*&) {
        return JS::LimitedColumnNumberOneOrigin(
            JS::WasmFunctionIndex::DefaultBinarySourceColumnNumberOneOrigin);
      });
  args.rval().setNumber(column.oneOriginValue());
  return true;
}

struct DebuggerScript::GetLineCountMatcher {
  JSContext* cx_;
  double totalLines;

  explicit GetLineCountMatcher(JSContext* cx) : cx_(cx), totalLines(0.0) {}
  using ReturnType = bool;

  ReturnType match(Handle<BaseScript*> base) {
    RootedScript script(cx_, DelazifyScript(cx_, base));
    if (!script) {
      return false;
    }
    totalLines = double(GetScriptLineExtent(script));
    return true;
  }
  ReturnType match(Handle<WasmInstanceObject*> instanceObj) {
    wasm::Instance& instance = instanceObj->instance();
    if (instance.debugEnabled()) {
      totalLines = double(instance.debug().bytecode().length());
    } else {
      totalLines = 0;
    }
    return true;
  }
};

bool DebuggerScript::CallData::getLineCount() {
  GetLineCountMatcher matcher(cx);
  if (!referent.match(matcher)) {
    return false;
  }
  args.rval().setNumber(matcher.totalLines);
  return true;
}

class DebuggerScript::GetSourceMatcher {
  JSContext* cx_;
  Debugger* dbg_;

 public:
  GetSourceMatcher(JSContext* cx, Debugger* dbg) : cx_(cx), dbg_(dbg) {}

  using ReturnType = DebuggerSource*;

  ReturnType match(Handle<BaseScript*> script) {
    Rooted<ScriptSourceObject*> source(cx_, script->sourceObject());
    return dbg_->wrapSource(cx_, source);
  }
  ReturnType match(Handle<WasmInstanceObject*> wasmInstance) {
    return dbg_->wrapWasmSource(cx_, wasmInstance);
  }
};

bool DebuggerScript::CallData::getSource() {
  Debugger* dbg = obj->owner();

  GetSourceMatcher matcher(cx, dbg);
  Rooted<DebuggerSource*> sourceObject(cx, referent.match(matcher));
  if (!sourceObject) {
    return false;
  }

  args.rval().setObject(*sourceObject);
  return true;
}

bool DebuggerScript::CallData::getSourceStart() {
  if (!ensureScriptMaybeLazy()) {
    return false;
  }
  args.rval().setNumber(uint32_t(obj->getReferentScript()->sourceStart()));
  return true;
}

bool DebuggerScript::CallData::getSourceLength() {
  if (!ensureScriptMaybeLazy()) {
    return false;
  }
  args.rval().setNumber(uint32_t(obj->getReferentScript()->sourceLength()));
  return true;
}

bool DebuggerScript::CallData::getMainOffset() {
  if (!ensureScript()) {
    return false;
  }
  args.rval().setNumber(uint32_t(script->mainOffset()));
  return true;
}

bool DebuggerScript::CallData::getGlobal() {
  if (!ensureScript()) {
    return false;
  }
  Debugger* dbg = obj->owner();

  RootedValue v(cx, ObjectValue(script->global()));
  if (!dbg->wrapDebuggeeValue(cx, &v)) {
    return false;
  }
  args.rval().set(v);
  return true;
}

bool DebuggerScript::CallData::getFormat() {
  args.rval().setString(referent.get().match(
      [this](BaseScript*&) { return cx->names().js.get(); },
      [this](WasmInstanceObject*&) { return cx->names().wasm.get(); }));
  return true;
}

static bool PushFunctionScript(JSContext* cx, Debugger* dbg, HandleFunction fun,
                               HandleObject array) {
  // Ignore asm.js natives.
  if (!IsInterpretedNonSelfHostedFunction(fun)) {
    return true;
  }

  Rooted<BaseScript*> script(cx, fun->baseScript());
  MOZ_ASSERT(script);
  if (!script) {
    // If the function doesn't have script, ignore it.
    return true;
  }
  RootedObject wrapped(cx, dbg->wrapScript(cx, script));
  if (!wrapped) {
    return false;
  }

  return NewbornArrayPush(cx, array, ObjectValue(*wrapped));
}

static bool PushInnerFunctions(JSContext* cx, Debugger* dbg, HandleObject array,
                               mozilla::Span<const JS::GCCellPtr> gcThings) {
  RootedFunction fun(cx);

  for (JS::GCCellPtr gcThing : gcThings) {
    if (!gcThing.is<JSObject>()) {
      continue;
    }

    JSObject* obj = &gcThing.as<JSObject>();
    if (obj->is<JSFunction>()) {
      fun = &obj->as<JSFunction>();

      // Ignore any delazification placeholder functions. These should not be
      // exposed to debugger in any way.
      if (fun->isGhost()) {
        continue;
      }

      if (!PushFunctionScript(cx, dbg, fun, array)) {
        return false;
      }
    }
  }

  return true;
}

bool DebuggerScript::CallData::getChildScripts() {
  if (!ensureScriptMaybeLazy()) {
    return false;
  }
  Debugger* dbg = obj->owner();

  RootedObject result(cx, NewDenseEmptyArray(cx));
  if (!result) {
    return false;
  }

  Rooted<BaseScript*> script(cx, obj->getReferent().as<BaseScript*>());
  if (!PushInnerFunctions(cx, dbg, result, script->gcthings())) {
    return false;
  }

  args.rval().setObject(*result);
  return true;
}

static bool ScriptOffset(JSContext* cx, const Value& v, size_t* offsetp) {
  double d;
  size_t off;

  bool ok = v.isNumber();
  if (ok) {
    d = v.toNumber();
    off = size_t(d);
  }
  if (!ok || off != d) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_DEBUG_BAD_OFFSET);
    return false;
  }
  *offsetp = off;
  return true;
}

static bool EnsureScriptOffsetIsValid(JSContext* cx, JSScript* script,
                                      size_t offset) {
  if (IsValidBytecodeOffset(cx, script, offset)) {
    return true;
  }
  JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                            JSMSG_DEBUG_BAD_OFFSET);
  return false;
}

static bool IsGeneratorSlotInitialization(JSScript* script, size_t offset,
                                          JSContext* cx) {
  jsbytecode* pc = script->offsetToPC(offset);
  if (JSOp(*pc) != JSOp::SetAliasedVar) {
    return false;
  }

  PropertyName* name = EnvironmentCoordinateNameSlow(script, pc);
  return name == cx->names().dot_generator_;
}

static bool EnsureBreakpointIsAllowed(JSContext* cx, JSScript* script,
                                      size_t offset) {
  // Disallow breakpoint for `JSOp::SetAliasedVar` after `JSOp::Generator`.
  // Those 2 instructions are supposed to be atomic, and nothing should happen
  // in between them.
  //
  // Hitting a breakpoint there breaks the assumption around the existence of
  // the frame's `GeneratorInfo`.
  // (see `DebugAPI::slowPathOnNewGenerator` and `DebuggerFrame::create`)
  if (IsGeneratorSlotInitialization(script, offset, cx)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_DEBUG_BREAKPOINT_NOT_ALLOWED);
    return false;
  }

  return true;
}

template <bool OnlyOffsets>
class DebuggerScript::GetPossibleBreakpointsMatcher {
  JSContext* cx_;
  MutableHandleObject result_;

  Maybe<size_t> minOffset;
  Maybe<size_t> maxOffset;

  Maybe<uint32_t> minLine;
  JS::LimitedColumnNumberOneOrigin minColumn;
  Maybe<uint32_t> maxLine;
  JS::LimitedColumnNumberOneOrigin maxColumn;

  bool passesQuery(size_t offset, uint32_t lineno,
                   JS::LimitedColumnNumberOneOrigin colno) {
    // [minOffset, maxOffset) - Inclusive minimum and exclusive maximum.
    if ((minOffset && offset < *minOffset) ||
        (maxOffset && offset >= *maxOffset)) {
      return false;
    }

    if (minLine) {
      if (lineno < *minLine || (lineno == *minLine && colno < minColumn)) {
        return false;
      }
    }

    if (maxLine) {
      if (lineno > *maxLine || (lineno == *maxLine && colno >= maxColumn)) {
        return false;
      }
    }

    return true;
  }

  bool maybeAppendEntry(size_t offset, uint32_t lineno,
                        JS::LimitedColumnNumberOneOrigin colno,
                        bool isStepStart) {
    if (!passesQuery(offset, lineno, colno)) {
      return true;
    }

    if (OnlyOffsets) {
      if (!NewbornArrayPush(cx_, result_, NumberValue(offset))) {
        return false;
      }

      return true;
    }

    Rooted<PlainObject*> entry(cx_, NewPlainObject(cx_));
    if (!entry) {
      return false;
    }

    RootedValue value(cx_, NumberValue(offset));
    if (!DefineDataProperty(cx_, entry, cx_->names().offset, value)) {
      return false;
    }

    value = NumberValue(lineno);
    if (!DefineDataProperty(cx_, entry, cx_->names().lineNumber, value)) {
      return false;
    }

    value = NumberValue(colno.oneOriginValue());
    if (!DefineDataProperty(cx_, entry, cx_->names().columnNumber, value)) {
      return false;
    }

    value = BooleanValue(isStepStart);
    if (!DefineDataProperty(cx_, entry, cx_->names().isStepStart, value)) {
      return false;
    }

    if (!NewbornArrayPush(cx_, result_, ObjectValue(*entry))) {
      return false;
    }
    return true;
  }

  template <typename T>
  bool parseIntValueImpl(HandleValue value, T* result) {
    if (!value.isNumber()) {
      return false;
    }

    double doubleOffset = value.toNumber();
    if (doubleOffset < 0 || (unsigned int)doubleOffset != doubleOffset) {
      return false;
    }

    *result = doubleOffset;
    return true;
  }

  bool parseUint32Value(HandleValue value, uint32_t* result) {
    return parseIntValueImpl(value, result);
  }
  bool parseColumnValue(HandleValue value,
                        JS::LimitedColumnNumberOneOrigin* result) {
    uint32_t tmp;
    if (!parseIntValueImpl(value, &tmp)) {
      return false;
    }
    if (tmp == 0) {
      return false;
    }
    *result->addressOfValueForTranscode() = tmp;
    return true;
  }
  bool parseSizeTValue(HandleValue value, size_t* result) {
    return parseIntValueImpl(value, result);
  }

  template <typename T>
  bool parseIntValueMaybeImpl(HandleValue value, Maybe<T>* result) {
    T result_;
    if (!parseIntValueImpl(value, &result_)) {
      return false;
    }

    *result = Some(result_);
    return true;
  }

  bool parseUint32Value(HandleValue value, Maybe<uint32_t>* result) {
    return parseIntValueMaybeImpl(value, result);
  }
  bool parseSizeTValue(HandleValue value, Maybe<size_t>* result) {
    return parseIntValueMaybeImpl(value, result);
  }

 public:
  explicit GetPossibleBreakpointsMatcher(JSContext* cx,
                                         MutableHandleObject result)
      : cx_(cx), result_(result) {}

  bool parseQuery(HandleObject query) {
    RootedValue lineValue(cx_);
    if (!GetProperty(cx_, query, query, cx_->names().line, &lineValue)) {
      return false;
    }

    RootedValue minLineValue(cx_);
    if (!GetProperty(cx_, query, query, cx_->names().minLine, &minLineValue)) {
      return false;
    }

    RootedValue minColumnValue(cx_);
    if (!GetProperty(cx_, query, query, cx_->names().minColumn,
                     &minColumnValue)) {
      return false;
    }

    RootedValue minOffsetValue(cx_);
    if (!GetProperty(cx_, query, query, cx_->names().minOffset,
                     &minOffsetValue)) {
      return false;
    }

    RootedValue maxLineValue(cx_);
    if (!GetProperty(cx_, query, query, cx_->names().maxLine, &maxLineValue)) {
      return false;
    }

    RootedValue maxColumnValue(cx_);
    if (!GetProperty(cx_, query, query, cx_->names().maxColumn,
                     &maxColumnValue)) {
      return false;
    }

    RootedValue maxOffsetValue(cx_);
    if (!GetProperty(cx_, query, query, cx_->names().maxOffset,
                     &maxOffsetValue)) {
      return false;
    }

    if (!minOffsetValue.isUndefined()) {
      if (!parseSizeTValue(minOffsetValue, &minOffset)) {
        JS_ReportErrorNumberASCII(
            cx_, GetErrorMessage, nullptr, JSMSG_UNEXPECTED_TYPE,
            "getPossibleBreakpoints' 'minOffset'", "not an integer");
        return false;
      }
    }
    if (!maxOffsetValue.isUndefined()) {
      if (!parseSizeTValue(maxOffsetValue, &maxOffset)) {
        JS_ReportErrorNumberASCII(
            cx_, GetErrorMessage, nullptr, JSMSG_UNEXPECTED_TYPE,
            "getPossibleBreakpoints' 'maxOffset'", "not an integer");
        return false;
      }
    }

    if (!lineValue.isUndefined()) {
      if (!minLineValue.isUndefined() || !maxLineValue.isUndefined()) {
        JS_ReportErrorNumberASCII(cx_, GetErrorMessage, nullptr,
                                  JSMSG_UNEXPECTED_TYPE,
                                  "getPossibleBreakpoints' 'line'",
                                  "not allowed alongside 'minLine'/'maxLine'");
        return false;
      }

      uint32_t line;
      if (!parseUint32Value(lineValue, &line)) {
        JS_ReportErrorNumberASCII(
            cx_, GetErrorMessage, nullptr, JSMSG_UNEXPECTED_TYPE,
            "getPossibleBreakpoints' 'line'", "not an integer");
        return false;
      }

      // If no end column is given, we use the default of 0 and wrap to
      // the next line.
      minLine = Some(line);
      maxLine = Some(line + (maxColumnValue.isUndefined() ? 1 : 0));
    }

    if (!minLineValue.isUndefined()) {
      if (!parseUint32Value(minLineValue, &minLine)) {
        JS_ReportErrorNumberASCII(
            cx_, GetErrorMessage, nullptr, JSMSG_UNEXPECTED_TYPE,
            "getPossibleBreakpoints' 'minLine'", "not an integer");
        return false;
      }
    }

    if (!minColumnValue.isUndefined()) {
      if (!minLine) {
        JS_ReportErrorNumberASCII(cx_, GetErrorMessage, nullptr,
                                  JSMSG_UNEXPECTED_TYPE,
                                  "getPossibleBreakpoints' 'minColumn'",
                                  "not allowed without 'line' or 'minLine'");
        return false;
      }

      if (!parseColumnValue(minColumnValue, &minColumn)) {
        JS_ReportErrorNumberASCII(
            cx_, GetErrorMessage, nullptr, JSMSG_UNEXPECTED_TYPE,
            "getPossibleBreakpoints' 'minColumn'", "not a positive integer");
        return false;
      }
    }

    if (!maxLineValue.isUndefined()) {
      if (!parseUint32Value(maxLineValue, &maxLine)) {
        JS_ReportErrorNumberASCII(
            cx_, GetErrorMessage, nullptr, JSMSG_UNEXPECTED_TYPE,
            "getPossibleBreakpoints' 'maxLine'", "not an integer");
        return false;
      }
    }

    if (!maxColumnValue.isUndefined()) {
      if (!maxLine) {
        JS_ReportErrorNumberASCII(cx_, GetErrorMessage, nullptr,
                                  JSMSG_UNEXPECTED_TYPE,
                                  "getPossibleBreakpoints' 'maxColumn'",
                                  "not allowed without 'line' or 'maxLine'");
        return false;
      }

      if (!parseColumnValue(maxColumnValue, &maxColumn)) {
        JS_ReportErrorNumberASCII(
            cx_, GetErrorMessage, nullptr, JSMSG_UNEXPECTED_TYPE,
            "getPossibleBreakpoints' 'maxColumn'", "not a positive integer");
        return false;
      }
    }

    return true;
  }

  using ReturnType = bool;
  ReturnType match(Handle<BaseScript*> base) {
    RootedScript script(cx_, DelazifyScript(cx_, base));
    if (!script) {
      return false;
    }

    // Second pass: build the result array.
    result_.set(NewDenseEmptyArray(cx_));
    if (!result_) {
      return false;
    }

    for (BytecodeRangeWithPosition r(cx_, script); !r.empty(); r.popFront()) {
      if (!r.frontIsBreakablePoint()) {
        continue;
      }

      size_t offset = r.frontOffset();
      uint32_t lineno = r.frontLineNumber();
      JS::LimitedColumnNumberOneOrigin colno = r.frontColumnNumber();

      if (!maybeAppendEntry(offset, lineno, colno,
                            r.frontIsBreakableStepPoint())) {
        return false;
      }
    }

    return true;
  }
  ReturnType match(Handle<WasmInstanceObject*> instanceObj) {
    wasm::Instance& instance = instanceObj->instance();

    Vector<wasm::ExprLoc> offsets(cx_);
    if (instance.debugEnabled() &&
        !instance.debug().getAllColumnOffsets(&offsets)) {
      return false;
    }

    result_.set(NewDenseEmptyArray(cx_));
    if (!result_) {
      return false;
    }

    for (uint32_t i = 0; i < offsets.length(); i++) {
      uint32_t lineno = offsets[i].lineno;
      JS::LimitedColumnNumberOneOrigin column(offsets[i].column);
      size_t offset = offsets[i].offset;
      if (!maybeAppendEntry(offset, lineno, column, true)) {
        return false;
      }
    }
    return true;
  }
};

bool DebuggerScript::CallData::getPossibleBreakpoints() {
  RootedObject result(cx);
  GetPossibleBreakpointsMatcher<false> matcher(cx, &result);
  if (args.length() >= 1 && !args[0].isUndefined()) {
    RootedObject queryObject(cx, RequireObject(cx, args[0]));
    if (!queryObject || !matcher.parseQuery(queryObject)) {
      return false;
    }
  }
  if (!referent.match(matcher)) {
    return false;
  }

  args.rval().setObject(*result);
  return true;
}

bool DebuggerScript::CallData::getPossibleBreakpointOffsets() {
  RootedObject result(cx);
  GetPossibleBreakpointsMatcher<true> matcher(cx, &result);
  if (args.length() >= 1 && !args[0].isUndefined()) {
    RootedObject queryObject(cx, RequireObject(cx, args[0]));
    if (!queryObject || !matcher.parseQuery(queryObject)) {
      return false;
    }
  }
  if (!referent.match(matcher)) {
    return false;
  }

  args.rval().setObject(*result);
  return true;
}

class DebuggerScript::GetOffsetMetadataMatcher {
  JSContext* cx_;
  size_t offset_;
  MutableHandle<PlainObject*> result_;

 public:
  explicit GetOffsetMetadataMatcher(JSContext* cx, size_t offset,
                                    MutableHandle<PlainObject*> result)
      : cx_(cx), offset_(offset), result_(result) {}
  using ReturnType = bool;
  ReturnType match(Handle<BaseScript*> base) {
    RootedScript script(cx_, DelazifyScript(cx_, base));
    if (!script) {
      return false;
    }

    if (!EnsureScriptOffsetIsValid(cx_, script, offset_)) {
      return false;
    }

    result_.set(NewPlainObject(cx_));
    if (!result_) {
      return false;
    }

    BytecodeRangeWithPosition r(cx_, script);
    while (!r.empty() && r.frontOffset() < offset_) {
      r.popFront();
    }

    RootedValue value(cx_, NumberValue(r.frontLineNumber()));
    if (!DefineDataProperty(cx_, result_, cx_->names().lineNumber, value)) {
      return false;
    }

    value = NumberValue(r.frontColumnNumber().oneOriginValue());
    if (!DefineDataProperty(cx_, result_, cx_->names().columnNumber, value)) {
      return false;
    }

    value = BooleanValue(r.frontIsBreakablePoint());
    if (!DefineDataProperty(cx_, result_, cx_->names().isBreakpoint, value)) {
      return false;
    }

    value = BooleanValue(r.frontIsBreakableStepPoint());
    if (!DefineDataProperty(cx_, result_, cx_->names().isStepStart, value)) {
      return false;
    }

    return true;
  }
  ReturnType match(Handle<WasmInstanceObject*> instanceObj) {
    wasm::Instance& instance = instanceObj->instance();
    if (!instance.debugEnabled()) {
      JS_ReportErrorNumberASCII(cx_, GetErrorMessage, nullptr,
                                JSMSG_DEBUG_BAD_OFFSET);
      return false;
    }

    uint32_t lineno;
    JS::LimitedColumnNumberOneOrigin column;
    if (!instance.debug().getOffsetLocation(offset_, &lineno, &column)) {
      JS_ReportErrorNumberASCII(cx_, GetErrorMessage, nullptr,
                                JSMSG_DEBUG_BAD_OFFSET);
      return false;
    }

    result_.set(NewPlainObject(cx_));
    if (!result_) {
      return false;
    }

    RootedValue value(cx_, NumberValue(lineno));
    if (!DefineDataProperty(cx_, result_, cx_->names().lineNumber, value)) {
      return false;
    }

    value = NumberValue(column.oneOriginValue());
    if (!DefineDataProperty(cx_, result_, cx_->names().columnNumber, value)) {
      return false;
    }

    value.setBoolean(true);
    if (!DefineDataProperty(cx_, result_, cx_->names().isBreakpoint, value)) {
      return false;
    }

    value.setBoolean(true);
    if (!DefineDataProperty(cx_, result_, cx_->names().isStepStart, value)) {
      return false;
    }

    return true;
  }
};

bool DebuggerScript::CallData::getOffsetMetadata() {
  if (!args.requireAtLeast(cx, "Debugger.Script.getOffsetMetadata", 1)) {
    return false;
  }
  size_t offset;
  if (!ScriptOffset(cx, args[0], &offset)) {
    return false;
  }

  Rooted<PlainObject*> result(cx);
  GetOffsetMetadataMatcher matcher(cx, offset, &result);
  if (!referent.match(matcher)) {
    return false;
  }

  args.rval().setObject(*result);
  return true;
}

namespace {

/*
 * FlowGraphSummary::populate(cx, script) computes a summary of script's
 * control flow graph used by DebuggerScript_{getAllOffsets,getLineOffsets}.
 *
 * An instruction on a given line is an entry point for that line if it can be
 * reached from (an instruction on) a different line. We distinguish between the
 * following cases:
 *   - hasNoEdges:
 *       The instruction cannot be reached, so the instruction is not an entry
 *       point for the line it is on.
 *   - hasSingleEdge:
 *       The instruction can be reached from a single line. If this line is
 *       different from the line the instruction is on, the instruction is an
 *       entry point for that line.
 *
 * Similarly, an instruction on a given position (line/column pair) is an
 * entry point for that position if it can be reached from (an instruction on) a
 * different position. Again, we distinguish between the following cases:
 *   - hasNoEdges:
 *       The instruction cannot be reached, so the instruction is not an entry
 *       point for the position it is on.
 *   - hasSingleEdge:
 *       The instruction can be reached from a single position. If this line is
 *       different from the position the instruction is on, the instruction is
 *       an entry point for that position.
 */
class FlowGraphSummary {
 public:
  class Entry {
   public:
    static constexpr uint32_t Line_HasNoEdge = UINT32_MAX;
    static constexpr uint32_t Column_HasMultipleEdge = UINT32_MAX;

    // NOTE: column can be Column_HasMultipleEdge.
    static Entry createWithSingleEdgeOrMultipleEdge(uint32_t lineno,
                                                    uint32_t column) {
      return Entry(lineno, column);
    }

    static Entry createWithMultipleEdgesFromSingleLine(uint32_t lineno) {
      return Entry(lineno, Column_HasMultipleEdge);
    }

    static Entry createWithMultipleEdgesFromMultipleLines() {
      return Entry(Line_HasNoEdge, Column_HasMultipleEdge);
    }

    Entry() : lineno_(Line_HasNoEdge), column_(1) {}

    bool hasNoEdges() const {
      return lineno_ == Line_HasNoEdge && column_ != Column_HasMultipleEdge;
    }

    bool hasSingleEdge() const {
      return lineno_ != Line_HasNoEdge && column_ != Column_HasMultipleEdge;
    }

    uint32_t lineno() const { return lineno_; }

    // Returns 1-origin column number or the sentinel value
    // Column_HasMultipleEdge.
    uint32_t columnOrSentinel() const { return column_; }

    JS::LimitedColumnNumberOneOrigin column() const {
      MOZ_ASSERT(column_ != Column_HasMultipleEdge);
      return JS::LimitedColumnNumberOneOrigin(column_);
    }

   private:
    Entry(uint32_t lineno, uint32_t column)
        : lineno_(lineno), column_(column) {}

    // Line number (1-origin).
    // Line_HasNoEdge for no edge.
    uint32_t lineno_;

    // Column number in UTF-16 code units (1-origin).
    // Column_HasMultipleEdge for multiple edge.
    uint32_t column_;
  };

  explicit FlowGraphSummary(JSContext* cx) : entries_(cx) {}

  Entry& operator[](size_t index) { return entries_[index]; }

  bool populate(JSContext* cx, JSScript* script) {
    if (!entries_.growBy(script->length())) {
      return false;
    }
    unsigned mainOffset = script->pcToOffset(script->main());
    entries_[mainOffset] = Entry::createWithMultipleEdgesFromMultipleLines();

    // The following code uses uint32_t for column numbers.
    // The value is either 1-origin column number,
    // or Entry::Column_HasMultipleEdge.

    uint32_t prevLineno = script->lineno();
    uint32_t prevColumn = 1;
    JSOp prevOp = JSOp::Nop;
    for (BytecodeRangeWithPosition r(cx, script); !r.empty(); r.popFront()) {
      uint32_t lineno = prevLineno;
      uint32_t column = prevColumn;
      JSOp op = r.frontOpcode();

      if (BytecodeFallsThrough(prevOp)) {
        addEdge(prevLineno, prevColumn, r.frontOffset());
      }

      // If we visit the branch target before we visit the
      // branch op itself, just reuse the previous location.
      // This is reasonable for the time being because this
      // situation can currently only arise from loop heads,
      // where this assumption holds.
      if (BytecodeIsJumpTarget(op) && !entries_[r.frontOffset()].hasNoEdges()) {
        lineno = entries_[r.frontOffset()].lineno();
        column = entries_[r.frontOffset()].columnOrSentinel();
      }

      if (r.frontIsEntryPoint()) {
        lineno = r.frontLineNumber();
        column = r.frontColumnNumber().oneOriginValue();
      }

      if (IsJumpOpcode(op)) {
        addEdge(lineno, column, r.frontOffset() + GET_JUMP_OFFSET(r.frontPC()));
      } else if (op == JSOp::TableSwitch) {
        jsbytecode* const switchPC = r.frontPC();
        jsbytecode* pc = switchPC;
        size_t offset = r.frontOffset();
        ptrdiff_t step = JUMP_OFFSET_LEN;
        size_t defaultOffset = offset + GET_JUMP_OFFSET(pc);
        pc += step;
        addEdge(lineno, column, defaultOffset);

        int32_t low = GET_JUMP_OFFSET(pc);
        pc += JUMP_OFFSET_LEN;
        int ncases = GET_JUMP_OFFSET(pc) - low + 1;
        pc += JUMP_OFFSET_LEN;

        for (int i = 0; i < ncases; i++) {
          size_t target = script->tableSwitchCaseOffset(switchPC, i);
          addEdge(lineno, column, target);
        }
      } else if (op == JSOp::Try) {
        // As there is no literal incoming edge into the catch block, we
        // make a fake one by copying the JSOp::Try location, as-if this
        // was an incoming edge of the catch block. This is needed
        // because we only report offsets of entry points which have
        // valid incoming edges.
        for (const TryNote& tn : script->trynotes()) {
          if (tn.start == r.frontOffset() + JSOpLength_Try) {
            uint32_t catchOffset = tn.start + tn.length;
            if (tn.kind() == TryNoteKind::Catch ||
                tn.kind() == TryNoteKind::Finally) {
              addEdge(lineno, column, catchOffset);
            }
          }
        }
      }

      prevLineno = lineno;
      prevColumn = column;
      prevOp = op;
    }

    return true;
  }

 private:
  // sourceColumn is either 1-origin column number,
  // or Entry::Column_HasMultipleEdge.
  void addEdge(uint32_t sourceLineno, uint32_t sourceColumn,
               size_t targetOffset) {
    if (entries_[targetOffset].hasNoEdges()) {
      entries_[targetOffset] =
          Entry::createWithSingleEdgeOrMultipleEdge(sourceLineno, sourceColumn);
    } else if (entries_[targetOffset].lineno() != sourceLineno) {
      entries_[targetOffset] =
          Entry::createWithMultipleEdgesFromMultipleLines();
    } else if (entries_[targetOffset].columnOrSentinel() != sourceColumn) {
      entries_[targetOffset] =
          Entry::createWithMultipleEdgesFromSingleLine(sourceLineno);
    }
  }

  Vector<Entry> entries_;
};

} /* anonymous namespace */

class DebuggerScript::GetOffsetLocationMatcher {
  JSContext* cx_;
  size_t offset_;
  MutableHandle<PlainObject*> result_;

 public:
  explicit GetOffsetLocationMatcher(JSContext* cx, size_t offset,
                                    MutableHandle<PlainObject*> result)
      : cx_(cx), offset_(offset), result_(result) {}
  using ReturnType = bool;
  ReturnType match(Handle<BaseScript*> base) {
    RootedScript script(cx_, DelazifyScript(cx_, base));
    if (!script) {
      return false;
    }

    if (!EnsureScriptOffsetIsValid(cx_, script, offset_)) {
      return false;
    }

    FlowGraphSummary flowData(cx_);
    if (!flowData.populate(cx_, script)) {
      return false;
    }

    result_.set(NewPlainObject(cx_));
    if (!result_) {
      return false;
    }

    BytecodeRangeWithPosition r(cx_, script);
    while (!r.empty() && r.frontOffset() < offset_) {
      r.popFront();
    }

    size_t offset = r.frontOffset();
    bool isEntryPoint = r.frontIsEntryPoint();

    // Line numbers are only correctly defined on entry points. Thus looks
    // either for the next valid offset in the flowData, being the last entry
    // point flowing into the current offset, or for the next valid entry point.
    while (!r.frontIsEntryPoint() &&
           !flowData[r.frontOffset()].hasSingleEdge()) {
      r.popFront();
      MOZ_ASSERT(!r.empty());
    }

    // If this is an entry point, take the line number associated with the entry
    // point, otherwise settle on the next instruction and take the incoming
    // edge position.
    uint32_t lineno;
    JS::LimitedColumnNumberOneOrigin column;
    if (r.frontIsEntryPoint()) {
      lineno = r.frontLineNumber();
      column = r.frontColumnNumber();
    } else {
      MOZ_ASSERT(flowData[r.frontOffset()].hasSingleEdge());
      lineno = flowData[r.frontOffset()].lineno();
      column = flowData[r.frontOffset()].column();
    }

    RootedValue value(cx_, NumberValue(lineno));
    if (!DefineDataProperty(cx_, result_, cx_->names().lineNumber, value)) {
      return false;
    }

    value = NumberValue(column.oneOriginValue());
    if (!DefineDataProperty(cx_, result_, cx_->names().columnNumber, value)) {
      return false;
    }

    // The same entry point test that is used by getAllColumnOffsets.
    isEntryPoint = (isEntryPoint && !flowData[offset].hasNoEdges() &&
                    (flowData[offset].lineno() != r.frontLineNumber() ||
                     flowData[offset].columnOrSentinel() !=
                         r.frontColumnNumber().oneOriginValue()));
    value.setBoolean(isEntryPoint);
    if (!DefineDataProperty(cx_, result_, cx_->names().isEntryPoint, value)) {
      return false;
    }

    return true;
  }
  ReturnType match(Handle<WasmInstanceObject*> instanceObj) {
    wasm::Instance& instance = instanceObj->instance();
    if (!instance.debugEnabled()) {
      JS_ReportErrorNumberASCII(cx_, GetErrorMessage, nullptr,
                                JSMSG_DEBUG_BAD_OFFSET);
      return false;
    }

    uint32_t lineno;
    JS::LimitedColumnNumberOneOrigin column;
    if (!instance.debug().getOffsetLocation(offset_, &lineno, &column)) {
      JS_ReportErrorNumberASCII(cx_, GetErrorMessage, nullptr,
                                JSMSG_DEBUG_BAD_OFFSET);
      return false;
    }

    result_.set(NewPlainObject(cx_));
    if (!result_) {
      return false;
    }

    RootedValue value(cx_, NumberValue(lineno));
    if (!DefineDataProperty(cx_, result_, cx_->names().lineNumber, value)) {
      return false;
    }

    value = NumberValue(column.oneOriginValue());
    if (!DefineDataProperty(cx_, result_, cx_->names().columnNumber, value)) {
      return false;
    }

    value.setBoolean(true);
    if (!DefineDataProperty(cx_, result_, cx_->names().isEntryPoint, value)) {
      return false;
    }

    return true;
  }
};

bool DebuggerScript::CallData::getOffsetLocation() {
  if (!args.requireAtLeast(cx, "Debugger.Script.getOffsetLocation", 1)) {
    return false;
  }
  size_t offset;
  if (!ScriptOffset(cx, args[0], &offset)) {
    return false;
  }

  Rooted<PlainObject*> result(cx);
  GetOffsetLocationMatcher matcher(cx, offset, &result);
  if (!referent.match(matcher)) {
    return false;
  }

  args.rval().setObject(*result);
  return true;
}

// Return whether an opcode is considered effectful: it can have direct side
// effects that can be observed outside of the current frame. Opcodes are not
// effectful if they only modify the current frame's state, modify objects
// created by the current frame, or can potentially call other scripts or
// natives which could have side effects.
static bool BytecodeIsEffectful(JSScript* script, size_t offset) {
  jsbytecode* pc = script->offsetToPC(offset);
  JSOp op = JSOp(*pc);
  switch (op) {
    case JSOp::SetProp:
    case JSOp::StrictSetProp:
    case JSOp::SetPropSuper:
    case JSOp::StrictSetPropSuper:
    case JSOp::SetElem:
    case JSOp::StrictSetElem:
    case JSOp::SetElemSuper:
    case JSOp::StrictSetElemSuper:
    case JSOp::SetName:
    case JSOp::StrictSetName:
    case JSOp::SetGName:
    case JSOp::StrictSetGName:
    case JSOp::DelProp:
    case JSOp::StrictDelProp:
    case JSOp::DelElem:
    case JSOp::StrictDelElem:
    case JSOp::DelName:
    case JSOp::SetAliasedVar:
    case JSOp::InitHomeObject:
    case JSOp::SetIntrinsic:
    case JSOp::InitGLexical:
    case JSOp::GlobalOrEvalDeclInstantiation:
    case JSOp::SetFunName:
    case JSOp::MutateProto:
    case JSOp::DynamicImport:
    case JSOp::InitialYield:
    case JSOp::Yield:
    case JSOp::Await:
    case JSOp::CanSkipAwait:
#ifdef ENABLE_EXPLICIT_RESOURCE_MANAGEMENT
    case JSOp::AddDisposable:
    case JSOp::DisposeDisposables:
#endif
      return true;

    case JSOp::Nop:
    case JSOp::NopDestructuring:
    case JSOp::NopIsAssignOp:
    case JSOp::TryDestructuring:
    case JSOp::Lineno:
    case JSOp::JumpTarget:
    case JSOp::Undefined:
    case JSOp::JumpIfTrue:
    case JSOp::JumpIfFalse:
    case JSOp::Return:
    case JSOp::RetRval:
    case JSOp::And:
    case JSOp::Or:
    case JSOp::Coalesce:
    case JSOp::Try:
    case JSOp::Throw:
    case JSOp::ThrowWithStack:
    case JSOp::Goto:
    case JSOp::TableSwitch:
    case JSOp::Case:
    case JSOp::Default:
    case JSOp::BitNot:
    case JSOp::BitAnd:
    case JSOp::BitOr:
    case JSOp::BitXor:
    case JSOp::Lsh:
    case JSOp::Rsh:
    case JSOp::Ursh:
    case JSOp::Add:
    case JSOp::Sub:
    case JSOp::Mul:
    case JSOp::Div:
    case JSOp::Mod:
    case JSOp::Pow:
    case JSOp::Pos:
    case JSOp::ToNumeric:
    case JSOp::Neg:
    case JSOp::Inc:
    case JSOp::Dec:
    case JSOp::ToString:
    case JSOp::Eq:
    case JSOp::Ne:
    case JSOp::StrictEq:
    case JSOp::StrictNe:
    case JSOp::Lt:
    case JSOp::Le:
    case JSOp::Gt:
    case JSOp::Ge:
    case JSOp::Double:
    case JSOp::BigInt:
    case JSOp::String:
    case JSOp::Symbol:
    case JSOp::Zero:
    case JSOp::One:
    case JSOp::Null:
    case JSOp::Void:
    case JSOp::Hole:
    case JSOp::False:
    case JSOp::True:
    case JSOp::Arguments:
    case JSOp::Rest:
    case JSOp::GetArg:
    case JSOp::GetFrameArg:
    case JSOp::SetArg:
    case JSOp::GetLocal:
    case JSOp::SetLocal:
    case JSOp::GetActualArg:
    case JSOp::ArgumentsLength:
    case JSOp::ThrowSetConst:
    case JSOp::CheckLexical:
    case JSOp::CheckAliasedLexical:
    case JSOp::InitLexical:
    case JSOp::Uninitialized:
    case JSOp::Pop:
    case JSOp::PopN:
    case JSOp::DupAt:
    case JSOp::NewArray:
    case JSOp::NewInit:
    case JSOp::NewObject:
    case JSOp::InitElem:
    case JSOp::InitHiddenElem:
    case JSOp::InitLockedElem:
    case JSOp::InitElemInc:
    case JSOp::InitElemArray:
    case JSOp::InitProp:
    case JSOp::InitLockedProp:
    case JSOp::InitHiddenProp:
    case JSOp::InitPropGetter:
    case JSOp::InitHiddenPropGetter:
    case JSOp::InitPropSetter:
    case JSOp::InitHiddenPropSetter:
    case JSOp::InitElemGetter:
    case JSOp::InitHiddenElemGetter:
    case JSOp::InitElemSetter:
    case JSOp::InitHiddenElemSetter:
    case JSOp::SpreadCall:
    case JSOp::Call:
    case JSOp::CallContent:
    case JSOp::CallIgnoresRv:
    case JSOp::CallIter:
    case JSOp::CallContentIter:
    case JSOp::New:
    case JSOp::NewContent:
    case JSOp::Eval:
    case JSOp::StrictEval:
    case JSOp::Int8:
    case JSOp::Uint16:
    case JSOp::ResumeKind:
    case JSOp::GetGName:
    case JSOp::GetName:
    case JSOp::GetIntrinsic:
    case JSOp::GetImport:
    case JSOp::BindGName:
    case JSOp::BindName:
    case JSOp::BindVar:
    case JSOp::Dup:
    case JSOp::Dup2:
    case JSOp::Swap:
    case JSOp::Pick:
    case JSOp::Unpick:
    case JSOp::GetAliasedDebugVar:
    case JSOp::GetAliasedVar:
    case JSOp::Uint24:
    case JSOp::Int32:
    case JSOp::LoopHead:
    case JSOp::GetElem:
    case JSOp::Not:
    case JSOp::FunctionThis:
    case JSOp::GlobalThis:
    case JSOp::NonSyntacticGlobalThis:
    case JSOp::Callee:
    case JSOp::EnvCallee:
    case JSOp::SuperBase:
    case JSOp::GetPropSuper:
    case JSOp::GetElemSuper:
    case JSOp::GetProp:
    case JSOp::RegExp:
    case JSOp::CallSiteObj:
    case JSOp::Object:
    case JSOp::Typeof:
    case JSOp::TypeofExpr:
    case JSOp::TypeofEq:
    case JSOp::ToAsyncIter:
    case JSOp::ToPropertyKey:
    case JSOp::Lambda:
    case JSOp::PushLexicalEnv:
    case JSOp::PopLexicalEnv:
    case JSOp::FreshenLexicalEnv:
    case JSOp::RecreateLexicalEnv:
    case JSOp::PushClassBodyEnv:
    case JSOp::Iter:
    case JSOp::MoreIter:
    case JSOp::IsNoIter:
    case JSOp::EndIter:
    case JSOp::CloseIter:
    case JSOp::OptimizeGetIterator:
    case JSOp::IsNullOrUndefined:
    case JSOp::In:
    case JSOp::HasOwn:
    case JSOp::CheckPrivateField:
    case JSOp::NewPrivateName:
    case JSOp::SetRval:
    case JSOp::Instanceof:
    case JSOp::DebugLeaveLexicalEnv:
    case JSOp::Debugger:
    case JSOp::ImplicitThis:
    case JSOp::NewTarget:
    case JSOp::CheckIsObj:
    case JSOp::CheckObjCoercible:
    case JSOp::DebugCheckSelfHosted:
    case JSOp::IsConstructing:
    case JSOp::OptimizeSpreadCall:
    case JSOp::ImportMeta:
    case JSOp::EnterWith:
    case JSOp::LeaveWith:
    case JSOp::SpreadNew:
    case JSOp::SpreadEval:
    case JSOp::StrictSpreadEval:
    case JSOp::CheckClassHeritage:
    case JSOp::FunWithProto:
    case JSOp::ObjWithProto:
    case JSOp::BuiltinObject:
    case JSOp::CheckThis:
    case JSOp::CheckReturn:
    case JSOp::CheckThisReinit:
    case JSOp::SuperFun:
    case JSOp::SpreadSuperCall:
    case JSOp::SuperCall:
    case JSOp::PushVarEnv:
    case JSOp::GetBoundName:
    case JSOp::Exception:
    case JSOp::ExceptionAndStack:
    case JSOp::IsGenClosing:
    case JSOp::FinalYieldRval:
    case JSOp::Resume:
    case JSOp::CheckResumeKind:
    case JSOp::AfterYield:
    case JSOp::MaybeExtractAwaitValue:
    case JSOp::Generator:
    case JSOp::AsyncAwait:
    case JSOp::AsyncResolve:
    case JSOp::AsyncReject:
    case JSOp::Finally:
    case JSOp::GetRval:
    case JSOp::ThrowMsg:
    case JSOp::ForceInterpreter:
#ifdef ENABLE_RECORD_TUPLE
    case JSOp::InitRecord:
    case JSOp::AddRecordProperty:
    case JSOp::AddRecordSpread:
    case JSOp::FinishRecord:
    case JSOp::InitTuple:
    case JSOp::AddTupleElement:
    case JSOp::FinishTuple:
#endif
      return false;

    case JSOp::InitAliasedLexical: {
      uint32_t hops = EnvironmentCoordinate(pc).hops();
      if (hops == 0) {
        // Initializing aliased lexical in the current scope is almost same
        // as JSOp::InitLexical.
        return false;
      }

      // Otherwise this can touch an environment outside of the current scope.
      return true;
    }
  }

  MOZ_ASSERT_UNREACHABLE("Invalid opcode");
  return false;
}

bool DebuggerScript::CallData::getEffectfulOffsets() {
  if (!ensureScript()) {
    return false;
  }

  RootedObject result(cx, NewDenseEmptyArray(cx));
  if (!result) {
    return false;
  }
  for (BytecodeRange r(cx, script); !r.empty(); r.popFront()) {
    size_t offset = r.frontOffset();
    if (!BytecodeIsEffectful(script, offset)) {
      continue;
    }

    if (IsGeneratorSlotInitialization(script, offset, cx)) {
      // This is engine-internal operation and not visible outside the
      // currently executing frame.
      //
      // Also this offset is not allowed for setting breakpoint.
      continue;
    }

    if (!NewbornArrayPush(cx, result, NumberValue(offset))) {
      return false;
    }
  }

  args.rval().setObject(*result);
  return true;
}

bool DebuggerScript::CallData::getAllOffsets() {
  if (!ensureScript()) {
    return false;
  }

  // First pass: determine which offsets in this script are jump targets and
  // which line numbers jump to them.
  FlowGraphSummary flowData(cx);
  if (!flowData.populate(cx, script)) {
    return false;
  }

  // Second pass: build the result array.
  RootedObject result(cx, NewDenseEmptyArray(cx));
  if (!result) {
    return false;
  }
  for (BytecodeRangeWithPosition r(cx, script); !r.empty(); r.popFront()) {
    if (!r.frontIsEntryPoint()) {
      continue;
    }

    size_t offset = r.frontOffset();
    uint32_t lineno = r.frontLineNumber();

    // Make a note, if the current instruction is an entry point for the current
    // line.
    if (!flowData[offset].hasNoEdges() && flowData[offset].lineno() != lineno) {
      // Get the offsets array for this line.
      RootedObject offsets(cx);
      RootedValue offsetsv(cx);

      RootedId id(cx, PropertyKey::Int(lineno));

      bool found;
      if (!HasOwnProperty(cx, result, id, &found)) {
        return false;
      }
      if (found && !GetProperty(cx, result, result, id, &offsetsv)) {
        return false;
      }

      if (offsetsv.isObject()) {
        offsets = &offsetsv.toObject();
      } else {
        MOZ_ASSERT(offsetsv.isUndefined());

        // Create an empty offsets array for this line.
        // Store it in the result array.
        RootedId id(cx);
        RootedValue v(cx, NumberValue(lineno));
        offsets = NewDenseEmptyArray(cx);
        if (!offsets || !PrimitiveValueToId<CanGC>(cx, v, &id)) {
          return false;
        }

        RootedValue value(cx, ObjectValue(*offsets));
        if (!DefineDataProperty(cx, result, id, value)) {
          return false;
        }
      }

      // Append the current offset to the offsets array.
      if (!NewbornArrayPush(cx, offsets, NumberValue(offset))) {
        return false;
      }
    }
  }

  args.rval().setObject(*result);
  return true;
}

class DebuggerScript::GetAllColumnOffsetsMatcher {
  JSContext* cx_;
  MutableHandleObject result_;

  bool appendColumnOffsetEntry(uint32_t lineno,
                               JS::LimitedColumnNumberOneOrigin column,
                               size_t offset) {
    Rooted<PlainObject*> entry(cx_, NewPlainObject(cx_));
    if (!entry) {
      return false;
    }

    RootedValue value(cx_, NumberValue(lineno));
    if (!DefineDataProperty(cx_, entry, cx_->names().lineNumber, value)) {
      return false;
    }

    value = NumberValue(column.oneOriginValue());
    if (!DefineDataProperty(cx_, entry, cx_->names().columnNumber, value)) {
      return false;
    }

    value = NumberValue(offset);
    if (!DefineDataProperty(cx_, entry, cx_->names().offset, value)) {
      return false;
    }

    return NewbornArrayPush(cx_, result_, ObjectValue(*entry));
  }

 public:
  explicit GetAllColumnOffsetsMatcher(JSContext* cx, MutableHandleObject result)
      : cx_(cx), result_(result) {}
  using ReturnType = bool;
  ReturnType match(Handle<BaseScript*> base) {
    RootedScript script(cx_, DelazifyScript(cx_, base));
    if (!script) {
      return false;
    }

    // First pass: determine which offsets in this script are jump targets
    // and which positions jump to them.
    FlowGraphSummary flowData(cx_);
    if (!flowData.populate(cx_, script)) {
      return false;
    }

    // Second pass: build the result array.
    result_.set(NewDenseEmptyArray(cx_));
    if (!result_) {
      return false;
    }

    for (BytecodeRangeWithPosition r(cx_, script); !r.empty(); r.popFront()) {
      uint32_t lineno = r.frontLineNumber();
      JS::LimitedColumnNumberOneOrigin column = r.frontColumnNumber();
      size_t offset = r.frontOffset();

      // Make a note, if the current instruction is an entry point for
      // the current position.
      if (r.frontIsEntryPoint() && !flowData[offset].hasNoEdges() &&
          (flowData[offset].lineno() != lineno ||
           flowData[offset].columnOrSentinel() != column.oneOriginValue())) {
        if (!appendColumnOffsetEntry(lineno, column, offset)) {
          return false;
        }
      }
    }
    return true;
  }
  ReturnType match(Handle<WasmInstanceObject*> instanceObj) {
    wasm::Instance& instance = instanceObj->instance();

    Vector<wasm::ExprLoc> offsets(cx_);
    if (instance.debugEnabled() &&
        !instance.debug().getAllColumnOffsets(&offsets)) {
      return false;
    }

    result_.set(NewDenseEmptyArray(cx_));
    if (!result_) {
      return false;
    }

    for (uint32_t i = 0; i < offsets.length(); i++) {
      uint32_t lineno = offsets[i].lineno;
      JS::LimitedColumnNumberOneOrigin column(offsets[i].column);
      size_t offset = offsets[i].offset;
      if (!appendColumnOffsetEntry(lineno, column, offset)) {
        return false;
      }
    }
    return true;
  }
};

bool DebuggerScript::CallData::getAllColumnOffsets() {
  RootedObject result(cx);
  GetAllColumnOffsetsMatcher matcher(cx, &result);
  if (!referent.match(matcher)) {
    return false;
  }

  args.rval().setObject(*result);
  return true;
}

class DebuggerScript::GetLineOffsetsMatcher {
  JSContext* cx_;
  uint32_t lineno_;
  MutableHandleObject result_;

 public:
  explicit GetLineOffsetsMatcher(JSContext* cx, uint32_t lineno,
                                 MutableHandleObject result)
      : cx_(cx), lineno_(lineno), result_(result) {}
  using ReturnType = bool;
  ReturnType match(Handle<BaseScript*> base) {
    RootedScript script(cx_, DelazifyScript(cx_, base));
    if (!script) {
      return false;
    }

    // First pass: determine which offsets in this script are jump targets and
    // which line numbers jump to them.
    FlowGraphSummary flowData(cx_);
    if (!flowData.populate(cx_, script)) {
      return false;
    }

    result_.set(NewDenseEmptyArray(cx_));
    if (!result_) {
      return false;
    }

    // Second pass: build the result array.
    for (BytecodeRangeWithPosition r(cx_, script); !r.empty(); r.popFront()) {
      if (!r.frontIsEntryPoint()) {
        continue;
      }

      size_t offset = r.frontOffset();

      // If the op at offset is an entry point, append offset to result.
      if (r.frontLineNumber() == lineno_ && !flowData[offset].hasNoEdges() &&
          flowData[offset].lineno() != lineno_) {
        if (!NewbornArrayPush(cx_, result_, NumberValue(offset))) {
          return false;
        }
      }
    }

    return true;
  }
  ReturnType match(Handle<WasmInstanceObject*> instanceObj) {
    wasm::Instance& instance = instanceObj->instance();

    Vector<uint32_t> offsets(cx_);
    if (instance.debugEnabled() &&
        !instance.debug().getLineOffsets(lineno_, &offsets)) {
      return false;
    }

    result_.set(NewDenseEmptyArray(cx_));
    if (!result_) {
      return false;
    }

    for (uint32_t i = 0; i < offsets.length(); i++) {
      if (!NewbornArrayPush(cx_, result_, NumberValue(offsets[i]))) {
        return false;
      }
    }
    return true;
  }
};

bool DebuggerScript::CallData::getLineOffsets() {
  if (!args.requireAtLeast(cx, "Debugger.Script.getLineOffsets", 1)) {
    return false;
  }

  // Parse lineno argument.
  RootedValue linenoValue(cx, args[0]);
  uint32_t lineno;
  if (!ToNumber(cx, &linenoValue)) {
    return false;
  }
  {
    double d = linenoValue.toNumber();
    lineno = uint32_t(d);
    if (lineno != d) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_DEBUG_BAD_LINE);
      return false;
    }
  }

  RootedObject result(cx);
  GetLineOffsetsMatcher matcher(cx, lineno, &result);
  if (!referent.match(matcher)) {
    return false;
  }

  args.rval().setObject(*result);
  return true;
}

struct DebuggerScript::SetBreakpointMatcher {
  JSContext* cx_;
  Debugger* dbg_;
  size_t offset_;
  RootedObject handler_;
  RootedObject debuggerObject_;

  bool wrapCrossCompartmentEdges() {
    if (!cx_->compartment()->wrap(cx_, &handler_) ||
        !cx_->compartment()->wrap(cx_, &debuggerObject_)) {
      return false;
    }

    // If the Debugger's compartment has killed incoming wrappers, we may not
    // have gotten usable results from the 'wrap' calls. Treat it as a
    // failure.
    if (IsDeadProxyObject(handler_) || IsDeadProxyObject(debuggerObject_)) {
      ReportAccessDenied(cx_);
      return false;
    }

    return true;
  }

 public:
  explicit SetBreakpointMatcher(JSContext* cx, Debugger* dbg, size_t offset,
                                HandleObject handler)
      : cx_(cx),
        dbg_(dbg),
        offset_(offset),
        handler_(cx, handler),
        debuggerObject_(cx_, dbg_->toJSObject()) {}

  using ReturnType = bool;

  ReturnType match(Handle<BaseScript*> base) {
    RootedScript script(cx_, DelazifyScript(cx_, base));
    if (!script) {
      return false;
    }

    if (!dbg_->observesScript(script)) {
      JS_ReportErrorNumberASCII(cx_, GetErrorMessage, nullptr,
                                JSMSG_DEBUG_NOT_DEBUGGING);
      return false;
    }

    if (!EnsureScriptOffsetIsValid(cx_, script, offset_)) {
      return false;
    }

    if (!EnsureBreakpointIsAllowed(cx_, script, offset_)) {
      return false;
    }

    // Ensure observability *before* setting the breakpoint. If the script is
    // not already a debuggee, trying to ensure observability after setting
    // the breakpoint (and thus marking the script as a debuggee) will skip
    // actually ensuring observability.
    if (!dbg_->ensureExecutionObservabilityOfScript(cx_, script)) {
      return false;
    }

    // A Breakpoint belongs logically to its script's compartment, so its
    // references to its Debugger and handler must be properly wrapped.
    AutoRealm ar(cx_, script);
    if (!wrapCrossCompartmentEdges()) {
      return false;
    }

    jsbytecode* pc = script->offsetToPC(offset_);
    JSBreakpointSite* site =
        DebugScript::getOrCreateBreakpointSite(cx_, script, pc);
    if (!site) {
      return false;
    }

    if (!cx_->zone()->new_<Breakpoint>(dbg_, debuggerObject_, site, handler_)) {
      site->destroyIfEmpty(cx_->runtime()->gcContext());
      return false;
    }
    AddCellMemory(script, sizeof(Breakpoint), MemoryUse::Breakpoint);

    return true;
  }
  ReturnType match(Handle<WasmInstanceObject*> wasmInstance) {
    wasm::Instance& instance = wasmInstance->instance();
    if (!instance.debugEnabled() ||
        !instance.debug().hasBreakpointTrapAtOffset(offset_)) {
      JS_ReportErrorNumberASCII(cx_, GetErrorMessage, nullptr,
                                JSMSG_DEBUG_BAD_OFFSET);
      return false;
    }

    // A Breakpoint belongs logically to its Instance's compartment, so its
    // references to its Debugger and handler must be properly wrapped.
    AutoRealm ar(cx_, wasmInstance);
    if (!wrapCrossCompartmentEdges()) {
      return false;
    }

    WasmBreakpointSite* site = instance.getOrCreateBreakpointSite(cx_, offset_);
    if (!site) {
      return false;
    }

    if (!cx_->zone()->new_<Breakpoint>(dbg_, debuggerObject_, site, handler_)) {
      site->destroyIfEmpty(cx_->runtime()->gcContext());
      return false;
    }
    AddCellMemory(wasmInstance, sizeof(Breakpoint), MemoryUse::Breakpoint);

    return true;
  }
};

bool DebuggerScript::CallData::setBreakpoint() {
  if (!args.requireAtLeast(cx, "Debugger.Script.setBreakpoint", 2)) {
    return false;
  }
  Debugger* dbg = obj->owner();

  size_t offset;
  if (!ScriptOffset(cx, args[0], &offset)) {
    return false;
  }

  RootedObject handler(cx, RequireObject(cx, args[1]));
  if (!handler) {
    return false;
  }

  SetBreakpointMatcher matcher(cx, dbg, offset, handler);
  if (!referent.match(matcher)) {
    return false;
  }
  args.rval().setUndefined();
  return true;
}

bool DebuggerScript::CallData::getBreakpoints() {
  if (!ensureScript()) {
    return false;
  }
  Debugger* dbg = obj->owner();

  jsbytecode* pc;
  if (args.length() > 0) {
    size_t offset;
    if (!ScriptOffset(cx, args[0], &offset) ||
        !EnsureScriptOffsetIsValid(cx, script, offset)) {
      return false;
    }
    pc = script->offsetToPC(offset);
  } else {
    pc = nullptr;
  }

  RootedObject arr(cx, NewDenseEmptyArray(cx));
  if (!arr) {
    return false;
  }

  for (unsigned i = 0; i < script->length(); i++) {
    JSBreakpointSite* site =
        DebugScript::getBreakpointSite(script, script->offsetToPC(i));
    if (!site) {
      continue;
    }
    if (!pc || site->pc == pc) {
      for (Breakpoint* bp = site->firstBreakpoint(); bp;
           bp = bp->nextInSite()) {
        if (bp->debugger == dbg) {
          RootedObject handler(cx, bp->getHandler());
          if (!cx->compartment()->wrap(cx, &handler) ||
              !NewbornArrayPush(cx, arr, ObjectValue(*handler))) {
            return false;
          }
        }
      }
    }
  }
  args.rval().setObject(*arr);
  return true;
}

class DebuggerScript::ClearBreakpointMatcher {
  JSContext* cx_;
  Debugger* dbg_;
  RootedObject handler_;

 public:
  ClearBreakpointMatcher(JSContext* cx, Debugger* dbg, JSObject* handler)
      : cx_(cx), dbg_(dbg), handler_(cx, handler) {}
  using ReturnType = bool;

  ReturnType match(Handle<BaseScript*> base) {
    RootedScript script(cx_, DelazifyScript(cx_, base));
    if (!script) {
      return false;
    }

    // A Breakpoint belongs logically to its script's compartment, so it holds
    // its handler via a cross-compartment wrapper. But the handler passed to
    // `clearBreakpoint` is same-compartment with the Debugger. Wrap it here,
    // so that `DebugScript::clearBreakpointsIn` gets the right value to
    // search for.
    AutoRealm ar(cx_, script);
    if (!cx_->compartment()->wrap(cx_, &handler_)) {
      return false;
    }

    DebugScript::clearBreakpointsIn(cx_->runtime()->gcContext(), script, dbg_,
                                    handler_);
    return true;
  }
  ReturnType match(Handle<WasmInstanceObject*> instanceObj) {
    wasm::Instance& instance = instanceObj->instance();
    if (!instance.debugEnabled()) {
      return true;
    }

    // A Breakpoint belongs logically to its instance's compartment, so it
    // holds its handler via a cross-compartment wrapper. But the handler
    // passed to `clearBreakpoint` is same-compartment with the Debugger. Wrap
    // it here, so that `DebugState::clearBreakpointsIn` gets the right value
    // to search for.
    AutoRealm ar(cx_, instanceObj);
    if (!cx_->compartment()->wrap(cx_, &handler_)) {
      return false;
    }

    instance.debug().clearBreakpointsIn(cx_->runtime()->gcContext(),
                                        instanceObj, dbg_, handler_);
    return true;
  }
};

bool DebuggerScript::CallData::clearBreakpoint() {
  if (!args.requireAtLeast(cx, "Debugger.Script.clearBreakpoint", 1)) {
    return false;
  }
  Debugger* dbg = obj->owner();

  JSObject* handler = RequireObject(cx, args[0]);
  if (!handler) {
    return false;
  }

  ClearBreakpointMatcher matcher(cx, dbg, handler);
  if (!referent.match(matcher)) {
    return false;
  }

  args.rval().setUndefined();
  return true;
}

bool DebuggerScript::CallData::clearAllBreakpoints() {
  Debugger* dbg = obj->owner();
  ClearBreakpointMatcher matcher(cx, dbg, nullptr);
  if (!referent.match(matcher)) {
    return false;
  }
  args.rval().setUndefined();
  return true;
}

class DebuggerScript::IsInCatchScopeMatcher {
  JSContext* cx_;
  size_t offset_;
  bool isInCatch_;

 public:
  explicit IsInCatchScopeMatcher(JSContext* cx, size_t offset)
      : cx_(cx), offset_(offset), isInCatch_(false) {}
  using ReturnType = bool;

  inline bool isInCatch() const { return isInCatch_; }

  ReturnType match(Handle<BaseScript*> base) {
    RootedScript script(cx_, DelazifyScript(cx_, base));
    if (!script) {
      return false;
    }

    if (!EnsureScriptOffsetIsValid(cx_, script, offset_)) {
      return false;
    }

    MOZ_ASSERT(!isInCatch_);
    for (const TryNote& tn : script->trynotes()) {
      bool inRange = tn.start <= offset_ && offset_ < tn.start + tn.length;
      if (inRange && tn.kind() == TryNoteKind::Catch) {
        isInCatch_ = true;
      } else if (isInCatch_) {
        // For-of loops generate a synthetic catch block to handle
        // closing the iterator when throwing an exception. The
        // debugger should ignore these synthetic catch blocks, so
        // we skip any Catch trynote that is immediately followed
        // by a ForOf trynote.
        if (inRange && tn.kind() == TryNoteKind::ForOf) {
          isInCatch_ = false;
          continue;
        }
        return true;
      }
    }

    return true;
  }
  ReturnType match(Handle<WasmInstanceObject*> instance) {
    isInCatch_ = false;
    return true;
  }
};

bool DebuggerScript::CallData::isInCatchScope() {
  if (!args.requireAtLeast(cx, "Debugger.Script.isInCatchScope", 1)) {
    return false;
  }

  size_t offset;
  if (!ScriptOffset(cx, args[0], &offset)) {
    return false;
  }

  IsInCatchScopeMatcher matcher(cx, offset);
  if (!referent.match(matcher)) {
    return false;
  }
  args.rval().setBoolean(matcher.isInCatch());
  return true;
}

bool DebuggerScript::CallData::getOffsetsCoverage() {
  if (!ensureScript()) {
    return false;
  }

  Debugger* dbg = obj->owner();
  if (dbg->observesCoverage() != Debugger::Observing) {
    args.rval().setNull();
    return true;
  }

  // If the script has no coverage information, then skip this and return null
  // instead.
  if (!script->hasScriptCounts()) {
    args.rval().setNull();
    return true;
  }

  ScriptCounts* sc = &script->getScriptCounts();

  // If the main ever got visited, then assume that any code before main got
  // visited once.
  uint64_t hits = 0;
  const PCCounts* counts =
      sc->maybeGetPCCounts(script->pcToOffset(script->main()));
  if (counts->numExec()) {
    hits = 1;
  }

  // Build an array of objects which are composed of 4 properties:
  //  - offset          PC offset of the current opcode.
  //  - lineNumber      Line of the current opcode.
  //  - columnNumber    Column of the current opcode.
  //  - count           Number of times the instruction got executed.
  RootedObject result(cx, NewDenseEmptyArray(cx));
  if (!result) {
    return false;
  }

  RootedId offsetId(cx, NameToId(cx->names().offset));
  RootedId lineNumberId(cx, NameToId(cx->names().lineNumber));
  RootedId columnNumberId(cx, NameToId(cx->names().columnNumber));
  RootedId countId(cx, NameToId(cx->names().count));

  RootedObject item(cx);
  RootedValue offsetValue(cx);
  RootedValue lineNumberValue(cx);
  RootedValue columnNumberValue(cx);
  RootedValue countValue(cx);

  // Iterate linearly over the bytecode.
  for (BytecodeRangeWithPosition r(cx, script); !r.empty(); r.popFront()) {
    size_t offset = r.frontOffset();

    // The beginning of each non-branching sequences of instruction set the
    // number of execution of the current instruction and any following
    // instruction.
    counts = sc->maybeGetPCCounts(offset);
    if (counts) {
      hits = counts->numExec();
    }

    offsetValue.setNumber(double(offset));
    lineNumberValue.setNumber(double(r.frontLineNumber()));
    columnNumberValue.setNumber(double(r.frontColumnNumber().oneOriginValue()));
    countValue.setNumber(double(hits));

    // Create a new object with the offset, line number, column number, the
    // number of hit counts, and append it to the array.
    item = NewPlainObjectWithProto(cx, nullptr);
    if (!item || !DefineDataProperty(cx, item, offsetId, offsetValue) ||
        !DefineDataProperty(cx, item, lineNumberId, lineNumberValue) ||
        !DefineDataProperty(cx, item, columnNumberId, columnNumberValue) ||
        !DefineDataProperty(cx, item, countId, countValue) ||
        !NewbornArrayPush(cx, result, ObjectValue(*item))) {
      return false;
    }

    // If the current instruction has thrown, then decrement the hit counts
    // with the number of throws.
    counts = sc->maybeGetThrowCounts(offset);
    if (counts) {
      hits -= counts->numExec();
    }
  }

  args.rval().setObject(*result);
  return true;
}

/* static */
bool DebuggerScript::construct(JSContext* cx, unsigned argc, Value* vp) {
  JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_NO_CONSTRUCTOR,
                            "Debugger.Script");
  return false;
}

const JSPropertySpec DebuggerScript::properties_[] = {
    JS_DEBUG_PSG("isGeneratorFunction", getIsGeneratorFunction),
    JS_DEBUG_PSG("isAsyncFunction", getIsAsyncFunction),
    JS_DEBUG_PSG("isFunction", getIsFunction),
    JS_DEBUG_PSG("isModule", getIsModule),
    JS_DEBUG_PSG("displayName", getDisplayName),
    JS_DEBUG_PSG("parameterNames", getParameterNames),
    JS_DEBUG_PSG("url", getUrl),
    JS_DEBUG_PSG("startLine", getStartLine),
    JS_DEBUG_PSG("startColumn", getStartColumn),
    JS_DEBUG_PSG("lineCount", getLineCount),
    JS_DEBUG_PSG("source", getSource),
    JS_DEBUG_PSG("sourceStart", getSourceStart),
    JS_DEBUG_PSG("sourceLength", getSourceLength),
    JS_DEBUG_PSG("mainOffset", getMainOffset),
    JS_DEBUG_PSG("global", getGlobal),
    JS_DEBUG_PSG("format", getFormat),
    JS_PS_END};

const JSFunctionSpec DebuggerScript::methods_[] = {
    JS_DEBUG_FN("getChildScripts", getChildScripts, 0),
    JS_DEBUG_FN("getPossibleBreakpoints", getPossibleBreakpoints, 0),
    JS_DEBUG_FN("getPossibleBreakpointOffsets", getPossibleBreakpointOffsets,
                0),
    JS_DEBUG_FN("setBreakpoint", setBreakpoint, 2),
    JS_DEBUG_FN("getBreakpoints", getBreakpoints, 1),
    JS_DEBUG_FN("clearBreakpoint", clearBreakpoint, 1),
    JS_DEBUG_FN("clearAllBreakpoints", clearAllBreakpoints, 0),
    JS_DEBUG_FN("isInCatchScope", isInCatchScope, 1),
    JS_DEBUG_FN("getOffsetMetadata", getOffsetMetadata, 1),
    JS_DEBUG_FN("getOffsetsCoverage", getOffsetsCoverage, 0),
    JS_DEBUG_FN("getEffectfulOffsets", getEffectfulOffsets, 1),

    // The following APIs are deprecated due to their reliance on the
    // under-defined 'entrypoint' concept. Make use of getPossibleBreakpoints,
    // getPossibleBreakpointOffsets, or getOffsetMetadata instead.
    JS_DEBUG_FN("getAllOffsets", getAllOffsets, 0),
    JS_DEBUG_FN("getAllColumnOffsets", getAllColumnOffsets, 0),
    JS_DEBUG_FN("getLineOffsets", getLineOffsets, 1),
    JS_DEBUG_FN("getOffsetLocation", getOffsetLocation, 0), JS_FS_END};

/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * JavaScript API.
 */

#include "jsapi.h"

#include "mozilla/FloatingPoint.h"
#include "mozilla/Maybe.h"
#include "mozilla/PodOperations.h"
#include "mozilla/Sprintf.h"

#include <algorithm>
#ifdef __linux__
#  include <dlfcn.h>
#endif
#include <iterator>
#include <stdarg.h>
#include <string.h>

#include "jsdate.h"
#include "jsexn.h"
#include "jsfriendapi.h"
#include "jsmath.h"
#include "jsnum.h"
#include "jstypes.h"

#include "builtin/Array.h"
#include "builtin/AtomicsObject.h"
#include "builtin/Boolean.h"
#include "builtin/Eval.h"
#include "builtin/FinalizationRegistryObject.h"
#include "builtin/JSON.h"
#include "builtin/MapObject.h"
#include "builtin/Promise.h"
#include "builtin/Stream.h"
#include "builtin/Symbol.h"
#include "frontend/BytecodeCompilation.h"  // frontend::CompileGlobalScriptToStencil, frontend::InstantiateStencils
#include "frontend/BytecodeCompiler.h"
#include "frontend/CompilationStencil.h"  // frontend::CompilationStencil, frontend::CompilationGCOutput
#include "gc/FreeOp.h"
#include "gc/Marking.h"
#include "gc/Policy.h"
#include "gc/PublicIterators.h"
#include "gc/WeakMap.h"
#include "jit/JitCommon.h"
#include "jit/JitSpewer.h"
#include "js/CharacterEncoding.h"
#include "js/CompilationAndEvaluation.h"
#include "js/CompileOptions.h"
#include "js/ContextOptions.h"  // JS::ContextOptions{,Ref}
#include "js/Conversions.h"
#include "js/Date.h"
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/friend/StackLimits.h"    // js::AutoCheckRecursionLimit
#include "js/Initialization.h"
#include "js/JSON.h"
#include "js/LocaleSensitive.h"
#include "js/MemoryFunctions.h"
#include "js/Object.h"                      // JS::SetPrivate
#include "js/OffThreadScriptCompilation.h"  // js::UseOffThreadParseGlobal
#include "js/PropertySpec.h"
#include "js/Proxy.h"
#include "js/SliceBudget.h"
#include "js/SourceText.h"
#include "js/StableStringChars.h"
#include "js/String.h"  // JS::MaxStringLength
#include "js/StructuredClone.h"
#include "js/Symbol.h"
#include "js/Utility.h"
#include "js/WasmModule.h"
#include "js/Wrapper.h"
#include "proxy/DOMProxy.h"
#include "util/CompleteFile.h"
#include "util/DifferentialTesting.h"
#include "util/StringBuffer.h"
#include "util/Text.h"
#include "vm/AsyncFunction.h"
#include "vm/AsyncIteration.h"
#include "vm/DateObject.h"
#include "vm/EnvironmentObject.h"
#include "vm/ErrorObject.h"
#include "vm/ErrorReporting.h"
#include "vm/HelperThreads.h"
#include "vm/Interpreter.h"
#include "vm/Iteration.h"
#include "vm/JSAtom.h"
#include "vm/JSAtomState.h"
#include "vm/JSContext.h"
#include "vm/JSFunction.h"
#include "vm/JSObject.h"
#include "vm/JSScript.h"
#include "vm/PlainObject.h"    // js::PlainObject
#include "vm/PromiseObject.h"  // js::PromiseObject
#include "vm/Runtime.h"
#include "vm/SavedStacks.h"
#include "vm/SelfHosting.h"
#include "vm/Shape.h"
#include "vm/StringType.h"
#include "vm/SymbolType.h"
#include "vm/ToSource.h"
#include "vm/WrapperObject.h"
#include "vm/Xdr.h"
#include "wasm/WasmModule.h"
#include "wasm/WasmProcess.h"

#include "builtin/Promise-inl.h"
#include "debugger/DebugAPI-inl.h"
#include "vm/Compartment-inl.h"
#include "vm/Interpreter-inl.h"
#include "vm/IsGivenTypeObject-inl.h"  // js::IsGivenTypeObject
#include "vm/JSAtom-inl.h"
#include "vm/JSFunction-inl.h"
#include "vm/JSScript-inl.h"
#include "vm/NativeObject-inl.h"
#include "vm/SavedStacks-inl.h"
#include "vm/StringType-inl.h"

using namespace js;

using mozilla::Maybe;
using mozilla::PodCopy;
using mozilla::Some;

using JS::AutoStableStringChars;
using JS::CompileOptions;
using JS::ReadOnlyCompileOptions;
using JS::SourceText;

#ifdef HAVE_VA_LIST_AS_ARRAY
#  define JS_ADDRESSOF_VA_LIST(ap) ((va_list*)(ap))
#else
#  define JS_ADDRESSOF_VA_LIST(ap) (&(ap))
#endif

// See preprocessor definition of JS_BITS_PER_WORD in jstypes.h; make sure
// JS_64BIT (used internally) agrees with it
#ifdef JS_64BIT
static_assert(JS_BITS_PER_WORD == 64, "values must be in sync");
#else
static_assert(JS_BITS_PER_WORD == 32, "values must be in sync");
#endif

JS_PUBLIC_API void JS::CallArgs::reportMoreArgsNeeded(JSContext* cx,
                                                      const char* fnname,
                                                      unsigned required,
                                                      unsigned actual) {
  char requiredArgsStr[40];
  SprintfLiteral(requiredArgsStr, "%u", required);
  char actualArgsStr[40];
  SprintfLiteral(actualArgsStr, "%u", actual);
  JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                            JSMSG_MORE_ARGS_NEEDED, fnname, requiredArgsStr,
                            required == 1 ? "" : "s", actualArgsStr);
}

static bool ErrorTakesArguments(unsigned msg) {
  MOZ_ASSERT(msg < JSErr_Limit);
  unsigned argCount = js_ErrorFormatString[msg].argCount;
  MOZ_ASSERT(argCount <= 2);
  return argCount == 1 || argCount == 2;
}

static bool ErrorTakesObjectArgument(unsigned msg) {
  MOZ_ASSERT(msg < JSErr_Limit);
  unsigned argCount = js_ErrorFormatString[msg].argCount;
  MOZ_ASSERT(argCount <= 2);
  return argCount == 2;
}

bool JS::ObjectOpResult::reportError(JSContext* cx, HandleObject obj,
                                     HandleId id) {
  static_assert(unsigned(OkCode) == unsigned(JSMSG_NOT_AN_ERROR),
                "unsigned value of OkCode must not be an error code");
  MOZ_ASSERT(code_ != Uninitialized);
  MOZ_ASSERT(!ok());
  cx->check(obj);

  if (code_ == JSMSG_OBJECT_NOT_EXTENSIBLE) {
    RootedValue val(cx, ObjectValue(*obj));
    return ReportValueError(cx, code_, JSDVG_IGNORE_STACK, val, nullptr);
  }

  if (ErrorTakesArguments(code_)) {
    UniqueChars propName =
        IdToPrintableUTF8(cx, id, IdToPrintableBehavior::IdIsPropertyKey);
    if (!propName) {
      return false;
    }

    if (code_ == JSMSG_SET_NON_OBJECT_RECEIVER) {
      // We know that the original receiver was a primitive, so unbox it.
      RootedValue val(cx, ObjectValue(*obj));
      if (!obj->is<ProxyObject>()) {
        if (!Unbox(cx, obj, &val)) {
          return false;
        }
      }
      return ReportValueError(cx, code_, JSDVG_IGNORE_STACK, val, nullptr,
                              propName.get());
    }

    if (ErrorTakesObjectArgument(code_)) {
      JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr, code_,
                               obj->getClass()->name, propName.get());
      return false;
    }

    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr, code_,
                             propName.get());
    return false;
  }
  JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, code_);
  return false;
}

bool JS::ObjectOpResult::reportError(JSContext* cx, HandleObject obj) {
  MOZ_ASSERT(code_ != Uninitialized);
  MOZ_ASSERT(!ok());
  MOZ_ASSERT(!ErrorTakesArguments(code_));
  cx->check(obj);

  JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, code_);
  return false;
}

JS_PUBLIC_API bool JS::ObjectOpResult::failCantRedefineProp() {
  return fail(JSMSG_CANT_REDEFINE_PROP);
}

JS_PUBLIC_API bool JS::ObjectOpResult::failReadOnly() {
  return fail(JSMSG_READ_ONLY);
}

JS_PUBLIC_API bool JS::ObjectOpResult::failGetterOnly() {
  return fail(JSMSG_GETTER_ONLY);
}

JS_PUBLIC_API bool JS::ObjectOpResult::failCantDelete() {
  return fail(JSMSG_CANT_DELETE);
}

JS_PUBLIC_API bool JS::ObjectOpResult::failCantSetInterposed() {
  return fail(JSMSG_CANT_SET_INTERPOSED);
}

JS_PUBLIC_API bool JS::ObjectOpResult::failCantDefineWindowElement() {
  return fail(JSMSG_CANT_DEFINE_WINDOW_ELEMENT);
}

JS_PUBLIC_API bool JS::ObjectOpResult::failCantDeleteWindowElement() {
  return fail(JSMSG_CANT_DELETE_WINDOW_ELEMENT);
}

JS_PUBLIC_API bool JS::ObjectOpResult::failCantDefineWindowNamedProperty() {
  return fail(JSMSG_CANT_DEFINE_WINDOW_NAMED_PROPERTY);
}

JS_PUBLIC_API bool JS::ObjectOpResult::failCantDeleteWindowNamedProperty() {
  return fail(JSMSG_CANT_DELETE_WINDOW_NAMED_PROPERTY);
}

JS_PUBLIC_API bool JS::ObjectOpResult::failCantDefineWindowNonConfigurable() {
  return fail(JSMSG_CANT_DEFINE_WINDOW_NC);
}

JS_PUBLIC_API bool JS::ObjectOpResult::failCantPreventExtensions() {
  return fail(JSMSG_CANT_PREVENT_EXTENSIONS);
}

JS_PUBLIC_API bool JS::ObjectOpResult::failCantSetProto() {
  return fail(JSMSG_CANT_SET_PROTO);
}

JS_PUBLIC_API bool JS::ObjectOpResult::failNoNamedSetter() {
  return fail(JSMSG_NO_NAMED_SETTER);
}

JS_PUBLIC_API bool JS::ObjectOpResult::failNoIndexedSetter() {
  return fail(JSMSG_NO_INDEXED_SETTER);
}

JS_PUBLIC_API bool JS::ObjectOpResult::failNotDataDescriptor() {
  return fail(JSMSG_NOT_DATA_DESCRIPTOR);
}

JS_PUBLIC_API int64_t JS_Now() { return PRMJ_Now(); }

JS_PUBLIC_API Value JS_GetEmptyStringValue(JSContext* cx) {
  return StringValue(cx->runtime()->emptyString);
}

JS_PUBLIC_API JSString* JS_GetEmptyString(JSContext* cx) {
  MOZ_ASSERT(cx->emptyString());
  return cx->emptyString();
}

namespace js {

void AssertHeapIsIdle() { MOZ_ASSERT(!JS::RuntimeHeapIsBusy()); }

}  // namespace js

static void AssertHeapIsIdleOrIterating() {
  MOZ_ASSERT(!JS::RuntimeHeapIsCollecting());
}

JS_PUBLIC_API bool JS_ValueToObject(JSContext* cx, HandleValue value,
                                    MutableHandleObject objp) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(value);
  if (value.isNullOrUndefined()) {
    objp.set(nullptr);
    return true;
  }
  JSObject* obj = ToObject(cx, value);
  if (!obj) {
    return false;
  }
  objp.set(obj);
  return true;
}

JS_PUBLIC_API JSFunction* JS_ValueToFunction(JSContext* cx, HandleValue value) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(value);
  return ReportIfNotFunction(cx, value);
}

JS_PUBLIC_API JSFunction* JS_ValueToConstructor(JSContext* cx,
                                                HandleValue value) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(value);
  return ReportIfNotFunction(cx, value);
}

JS_PUBLIC_API JSString* JS_ValueToSource(JSContext* cx, HandleValue value) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(value);
  return ValueToSource(cx, value);
}

JS_PUBLIC_API bool JS_DoubleIsInt32(double d, int32_t* ip) {
  return mozilla::NumberIsInt32(d, ip);
}

JS_PUBLIC_API JSType JS_TypeOfValue(JSContext* cx, HandleValue value) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(value);
  return TypeOfValue(value);
}

JS_PUBLIC_API bool JS_IsBuiltinEvalFunction(JSFunction* fun) {
  return IsAnyBuiltinEval(fun);
}

JS_PUBLIC_API bool JS_IsBuiltinFunctionConstructor(JSFunction* fun) {
  return fun->isBuiltinFunctionConstructor();
}

JS_PUBLIC_API bool JS_IsFunctionBound(JSFunction* fun) {
  return fun->isBoundFunction();
}

JS_PUBLIC_API JSObject* JS_GetBoundFunctionTarget(JSFunction* fun) {
  return fun->isBoundFunction() ? fun->getBoundFunctionTarget() : nullptr;
}

/************************************************************************/

// Prevent functions from being discarded by linker, so that they are callable
// when debugging.
static void PreventDiscardingFunctions() {
  if (reinterpret_cast<uintptr_t>(&PreventDiscardingFunctions) == 1) {
    // Never executed.
    memset((void*)&js::debug::GetMarkInfo, 0, 1);
    memset((void*)&js::debug::GetMarkWordAddress, 0, 1);
    memset((void*)&js::debug::GetMarkMask, 0, 1);
  }
}

JS_PUBLIC_API JSContext* JS_NewContext(uint32_t maxbytes,
                                       JSRuntime* parentRuntime) {
  MOZ_ASSERT(JS::detail::libraryInitState == JS::detail::InitState::Running,
             "must call JS_Init prior to creating any JSContexts");

  // Prevent linker from discarding unused debug functions.
  PreventDiscardingFunctions();

  // Make sure that all parent runtimes are the topmost parent.
  while (parentRuntime && parentRuntime->parentRuntime) {
    parentRuntime = parentRuntime->parentRuntime;
  }

  return NewContext(maxbytes, parentRuntime);
}

JS_PUBLIC_API void JS_DestroyContext(JSContext* cx) { DestroyContext(cx); }

JS_PUBLIC_API void* JS_GetContextPrivate(JSContext* cx) { return cx->data; }

JS_PUBLIC_API void JS_SetContextPrivate(JSContext* cx, void* data) {
  cx->data = data;
}

JS_PUBLIC_API void JS_SetFutexCanWait(JSContext* cx) {
  cx->fx.setCanWait(true);
}

JS_PUBLIC_API JSRuntime* JS_GetParentRuntime(JSContext* cx) {
  return cx->runtime()->parentRuntime ? cx->runtime()->parentRuntime
                                      : cx->runtime();
}

JS_PUBLIC_API JSRuntime* JS_GetRuntime(JSContext* cx) { return cx->runtime(); }

JS_PUBLIC_API JS::ContextOptions& JS::ContextOptionsRef(JSContext* cx) {
  return cx->options();
}

JS::ContextOptions& JS::ContextOptions::setWasmCranelift(bool flag) {
#ifdef ENABLE_WASM_CRANELIFT
  wasmCranelift_ = flag;
#endif
  return *this;
}

JS::ContextOptions& JS::ContextOptions::setWasmSimdWormhole(bool flag) {
#ifdef ENABLE_WASM_SIMD_WORMHOLE
  wasmSimdWormhole_ = flag;
#endif
  return *this;
}

JS::ContextOptions& JS::ContextOptions::setFuzzing(bool flag) {
#ifdef FUZZING
  fuzzing_ = flag;
#endif
  return *this;
}

JS_PUBLIC_API const char* JS_GetImplementationVersion(void) {
  return "JavaScript-C" MOZILLA_VERSION;
}

JS_PUBLIC_API void JS_SetDestroyZoneCallback(JSContext* cx,
                                             JSDestroyZoneCallback callback) {
  cx->runtime()->destroyZoneCallback = callback;
}

JS_PUBLIC_API void JS_SetDestroyCompartmentCallback(
    JSContext* cx, JSDestroyCompartmentCallback callback) {
  cx->runtime()->destroyCompartmentCallback = callback;
}

JS_PUBLIC_API void JS_SetSizeOfIncludingThisCompartmentCallback(
    JSContext* cx, JSSizeOfIncludingThisCompartmentCallback callback) {
  cx->runtime()->sizeOfIncludingThisCompartmentCallback = callback;
}

JS_PUBLIC_API void JS_SetErrorInterceptorCallback(
    JSRuntime* rt, JSErrorInterceptor* callback) {
#if defined(NIGHTLY_BUILD)
  rt->errorInterception.interceptor = callback;
#endif  // defined(NIGHTLY_BUILD)
}

JS_PUBLIC_API JSErrorInterceptor* JS_GetErrorInterceptorCallback(
    JSRuntime* rt) {
#if defined(NIGHTLY_BUILD)
  return rt->errorInterception.interceptor;
#else   // !NIGHTLY_BUILD
  return nullptr;
#endif  // defined(NIGHTLY_BUILD)
}

JS_PUBLIC_API Maybe<JSExnType> JS_GetErrorType(const JS::Value& val) {
  // All errors are objects.
  if (!val.isObject()) {
    return mozilla::Nothing();
  }

  const JSObject& obj = val.toObject();

  // All errors are `ErrorObject`.
  if (!obj.is<js::ErrorObject>()) {
    // Not one of the primitive errors.
    return mozilla::Nothing();
  }

  const js::ErrorObject& err = obj.as<js::ErrorObject>();
  return mozilla::Some(err.type());
}

JS_PUBLIC_API void JS_SetWrapObjectCallbacks(
    JSContext* cx, const JSWrapObjectCallbacks* callbacks) {
  cx->runtime()->wrapObjectCallbacks = callbacks;
}

JS_PUBLIC_API Realm* JS::EnterRealm(JSContext* cx, JSObject* target) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);

  MOZ_DIAGNOSTIC_ASSERT(!js::IsCrossCompartmentWrapper(target));

  Realm* oldRealm = cx->realm();
  cx->enterRealmOf(target);
  return oldRealm;
}

JS_PUBLIC_API void JS::LeaveRealm(JSContext* cx, JS::Realm* oldRealm) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->leaveRealm(oldRealm);
}

JSAutoRealm::JSAutoRealm(JSContext* cx, JSObject* target)
    : cx_(cx), oldRealm_(cx->realm()) {
  MOZ_DIAGNOSTIC_ASSERT(!js::IsCrossCompartmentWrapper(target));
  AssertHeapIsIdleOrIterating();
  cx_->enterRealmOf(target);
}

JSAutoRealm::JSAutoRealm(JSContext* cx, JSScript* target)
    : cx_(cx), oldRealm_(cx->realm()) {
  AssertHeapIsIdleOrIterating();
  cx_->enterRealmOf(target);
}

JSAutoRealm::~JSAutoRealm() { cx_->leaveRealm(oldRealm_); }

JSAutoNullableRealm::JSAutoNullableRealm(JSContext* cx, JSObject* targetOrNull)
    : cx_(cx), oldRealm_(cx->realm()) {
  AssertHeapIsIdleOrIterating();
  if (targetOrNull) {
    MOZ_DIAGNOSTIC_ASSERT(!js::IsCrossCompartmentWrapper(targetOrNull));
    cx_->enterRealmOf(targetOrNull);
  } else {
    cx_->enterNullRealm();
  }
}

JSAutoNullableRealm::~JSAutoNullableRealm() { cx_->leaveRealm(oldRealm_); }

JS_PUBLIC_API void JS_SetCompartmentPrivate(JS::Compartment* compartment,
                                            void* data) {
  compartment->data = data;
}

JS_PUBLIC_API void* JS_GetCompartmentPrivate(JS::Compartment* compartment) {
  return compartment->data;
}

JS_PUBLIC_API void JS_MarkCrossZoneId(JSContext* cx, jsid id) {
  cx->markId(id);
}

JS_PUBLIC_API void JS_MarkCrossZoneIdValue(JSContext* cx, const Value& value) {
  cx->markAtomValue(value);
}

JS_PUBLIC_API void JS_SetZoneUserData(JS::Zone* zone, void* data) {
  zone->data = data;
}

JS_PUBLIC_API void* JS_GetZoneUserData(JS::Zone* zone) { return zone->data; }

JS_PUBLIC_API bool JS_WrapObject(JSContext* cx, MutableHandleObject objp) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  if (objp) {
    JS::ExposeObjectToActiveJS(objp);
  }
  return cx->compartment()->wrap(cx, objp);
}

JS_PUBLIC_API bool JS_WrapValue(JSContext* cx, MutableHandleValue vp) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  JS::ExposeValueToActiveJS(vp);
  return cx->compartment()->wrap(cx, vp);
}

static void ReleaseAssertObjectHasNoWrappers(JSContext* cx,
                                             HandleObject target) {
  for (CompartmentsIter c(cx->runtime()); !c.done(); c.next()) {
    if (c->lookupWrapper(target)) {
      MOZ_CRASH("wrapper found for target object");
    }
  }
}

/*
 * [SMDOC] Brain transplants.
 *
 * Not for beginners or the squeamish.
 *
 * Sometimes a web spec requires us to transplant an object from one
 * compartment to another, like when a DOM node is inserted into a document in
 * another window and thus gets "adopted". We cannot literally change the
 * `.compartment()` of a `JSObject`; that would break the compartment
 * invariants. However, as usual, we have a workaround using wrappers.
 *
 * Of all the wrapper-based workarounds we do, it's safe to say this is the
 * most spectacular and questionable.
 *
 * `JS_TransplantObject(cx, origobj, target)` changes `origobj` into a
 * simulacrum of `target`, using highly esoteric means. To JS code, the effect
 * is as if `origobj` magically "became" `target`, but most often what actually
 * happens is that `origobj` gets turned into a cross-compartment wrapper for
 * `target`. The old behavior and contents of `origobj` are overwritten or
 * discarded.
 *
 * Thus, to "transplant" an object from one compartment to another:
 *
 * 1.  Let `origobj` be the object that you want to move. First, create a
 *     clone of it, `target`, in the destination compartment.
 *
 *     In our DOM adoption example, `target` will be a Node of the same type as
 *     `origobj`, same content, but in the adopting document.  We're not done
 *     yet: the spec for DOM adoption requires that `origobj.ownerDocument`
 *     actually change. All we've done so far is make a copy.
 *
 * 2.  Call `JS_TransplantObject(cx, origobj, target)`. This typically turns
 *     `origobj` into a wrapper for `target`, so that any JS code that has a
 *     reference to `origobj` will observe it to have the behavior of `target`
 *     going forward. In addition, all existing wrappers for `origobj` are
 *     changed into wrappers for `target`, extending the illusion to those
 *     compartments as well.
 *
 * During navigation, we use the above technique to transplant the WindowProxy
 * into the new Window's compartment.
 *
 * A few rules:
 *
 * -   `origobj` and `target` must be two distinct objects of the same
 *     `JSClass`.  Some classes may not support transplantation; WindowProxy
 *     objects and DOM nodes are OK.
 *
 * -   `target` should be created specifically to be passed to this function.
 *     There must be no existing cross-compartment wrappers for it; ideally
 *     there shouldn't be any pointers to it at all, except the one passed in.
 *
 * -   `target` shouldn't be used afterwards. Instead, `JS_TransplantObject`
 *     returns a pointer to the transplanted object, which might be `target`
 *     but might be some other object in the same compartment. Use that.
 *
 * The reason for this last rule is that JS_TransplantObject does very strange
 * things in some cases, like swapping `target`'s brain with that of another
 * object. Leaving `target` behaving like its former self is not a goal.
 *
 * We don't have a good way to recover from failure in this function, so
 * we intentionally crash instead.
 */

static void CheckTransplantObject(JSObject* obj) {
#ifdef DEBUG
  MOZ_ASSERT(!obj->is<CrossCompartmentWrapperObject>());
  JS::AssertCellIsNotGray(obj);
#endif
}

JS_PUBLIC_API JSObject* JS_TransplantObject(JSContext* cx, HandleObject origobj,
                                            HandleObject target) {
  AssertHeapIsIdle();
  MOZ_ASSERT(origobj != target);
  CheckTransplantObject(origobj);
  CheckTransplantObject(target);
  ReleaseAssertObjectHasNoWrappers(cx, target);

  RootedObject newIdentity(cx);

  // Don't allow a compacting GC to observe any intermediate state.
  AutoDisableCompactingGC nocgc(cx);

  AutoDisableProxyCheck adpc;

  AutoEnterOOMUnsafeRegion oomUnsafe;

  JS::Compartment* destination = target->compartment();

  if (origobj->compartment() == destination) {
    // If the original object is in the same compartment as the
    // destination, then we know that we won't find a wrapper in the
    // destination's cross compartment map and that the same
    // object will continue to work.
    AutoRealm ar(cx, origobj);
    JSObject::swap(cx, origobj, target, oomUnsafe);
    newIdentity = origobj;
  } else if (ObjectWrapperMap::Ptr p = destination->lookupWrapper(origobj)) {
    // There might already be a wrapper for the original object in
    // the new compartment. If there is, we use its identity and swap
    // in the contents of |target|.
    newIdentity = p->value().get();

    // When we remove origv from the wrapper map, its wrapper, newIdentity,
    // must immediately cease to be a cross-compartment wrapper. Nuke it.
    destination->removeWrapper(p);
    NukeCrossCompartmentWrapper(cx, newIdentity);

    AutoRealm ar(cx, newIdentity);
    JSObject::swap(cx, newIdentity, target, oomUnsafe);
  } else {
    // Otherwise, we use |target| for the new identity object.
    newIdentity = target;
  }

  // Now, iterate through other scopes looking for references to the old
  // object, and update the relevant cross-compartment wrappers. We do this
  // even if origobj is in the same compartment as target and thus
  // `newIdentity == origobj`, because this process also clears out any
  // cached wrapper state.
  if (!RemapAllWrappersForObject(cx, origobj, newIdentity)) {
    oomUnsafe.crash("JS_TransplantObject");
  }

  // Lastly, update the original object to point to the new one.
  if (origobj->compartment() != destination) {
    RootedObject newIdentityWrapper(cx, newIdentity);
    AutoRealm ar(cx, origobj);
    if (!JS_WrapObject(cx, &newIdentityWrapper)) {
      MOZ_RELEASE_ASSERT(cx->isThrowingOutOfMemory() ||
                         cx->isThrowingOverRecursed());
      oomUnsafe.crash("JS_TransplantObject");
    }
    MOZ_ASSERT(Wrapper::wrappedObject(newIdentityWrapper) == newIdentity);
    JSObject::swap(cx, origobj, newIdentityWrapper, oomUnsafe);
    if (origobj->compartment()->lookupWrapper(newIdentity)) {
      MOZ_ASSERT(origobj->is<CrossCompartmentWrapperObject>());
      if (!origobj->compartment()->putWrapper(cx, newIdentity, origobj)) {
        oomUnsafe.crash("JS_TransplantObject");
      }
    }
  }

  // The new identity object might be one of several things. Return it to avoid
  // ambiguity.
  JS::AssertCellIsNotGray(newIdentity);
  return newIdentity;
}

JS_PUBLIC_API void js::RemapRemoteWindowProxies(
    JSContext* cx, CompartmentTransplantCallback* callback,
    MutableHandleObject target) {
  AssertHeapIsIdle();
  CheckTransplantObject(target);
  ReleaseAssertObjectHasNoWrappers(cx, target);

  // |target| can't be a remote proxy, because we expect it to get a CCW when
  // wrapped across compartments.
  MOZ_ASSERT(!js::IsDOMRemoteProxyObject(target));

  // Don't allow a compacting GC to observe any intermediate state.
  AutoDisableCompactingGC nocgc(cx);

  AutoDisableProxyCheck adpc;

  AutoEnterOOMUnsafeRegion oomUnsafe;

  AutoCheckRecursionLimit recursion(cx);
  if (!recursion.checkSystem(cx)) {
    oomUnsafe.crash("js::RemapRemoteWindowProxies");
  }

  RootedObject targetCompartmentProxy(cx);
  JS::RootedVector<JSObject*> otherProxies(cx);

  // Use the callback to find remote proxies in all compartments that match
  // whatever criteria callback uses.
  for (CompartmentsIter c(cx->runtime()); !c.done(); c.next()) {
    RootedObject remoteProxy(cx, callback->getObjectToTransplant(c));
    if (!remoteProxy) {
      continue;
    }
    // The object the callback returns should be a DOM remote proxy object in
    // the compartment c. We rely on it being a DOM remote proxy because that
    // means that it won't have any cross-compartment wrappers.
    MOZ_ASSERT(js::IsDOMRemoteProxyObject(remoteProxy));
    MOZ_ASSERT(remoteProxy->compartment() == c);
    CheckTransplantObject(remoteProxy);

    // Immediately turn the DOM remote proxy object into a dead proxy object
    // so we don't have to worry about anything weird going on with it.
    js::NukeNonCCWProxy(cx, remoteProxy);

    if (remoteProxy->compartment() == target->compartment()) {
      targetCompartmentProxy = remoteProxy;
    } else if (!otherProxies.append(remoteProxy)) {
      oomUnsafe.crash("js::RemapRemoteWindowProxies");
    }
  }

  // If there was a remote proxy in |target|'s compartment, we need to use it
  // instead of |target|, in case it had any references, so swap it. Do this
  // before any other compartment so that the target object will be set up
  // correctly before we start wrapping it into other compartments.
  if (targetCompartmentProxy) {
    AutoRealm ar(cx, targetCompartmentProxy);
    JSObject::swap(cx, targetCompartmentProxy, target, oomUnsafe);
    target.set(targetCompartmentProxy);
  }

  for (JSObject*& obj : otherProxies) {
    RootedObject deadWrapper(cx, obj);
    js::RemapDeadWrapper(cx, deadWrapper, target);
  }
}

/*
 * Recompute all cross-compartment wrappers for an object, resetting state.
 * Gecko uses this to clear Xray wrappers when doing a navigation that reuses
 * the inner window and global object.
 */
JS_PUBLIC_API bool JS_RefreshCrossCompartmentWrappers(JSContext* cx,
                                                      HandleObject obj) {
  return RemapAllWrappersForObject(cx, obj, obj);
}

typedef struct JSStdName {
  size_t atomOffset; /* offset of atom pointer in JSAtomState */
  JSProtoKey key;
  bool isDummy() const { return key == JSProto_Null; }
  bool isSentinel() const { return key == JSProto_LIMIT; }
} JSStdName;

static const JSStdName* LookupStdName(const JSAtomState& names, JSAtom* name,
                                      const JSStdName* table) {
  for (unsigned i = 0; !table[i].isSentinel(); i++) {
    if (table[i].isDummy()) {
      continue;
    }
    JSAtom* atom = AtomStateOffsetToName(names, table[i].atomOffset);
    MOZ_ASSERT(atom);
    if (name == atom) {
      return &table[i];
    }
  }

  return nullptr;
}

/*
 * Table of standard classes, indexed by JSProtoKey. For entries where the
 * JSProtoKey does not correspond to a class with a meaningful constructor, we
 * insert a null entry into the table.
 */
#define STD_NAME_ENTRY(name, clasp) {NAME_OFFSET(name), JSProto_##name},
#define STD_DUMMY_ENTRY(name, dummy) {0, JSProto_Null},
static const JSStdName standard_class_names[] = {
    JS_FOR_PROTOTYPES(STD_NAME_ENTRY, STD_DUMMY_ENTRY){0, JSProto_LIMIT}};

/*
 * Table of top-level function and constant names and the JSProtoKey of the
 * standard class that initializes them.
 */
static const JSStdName builtin_property_names[] = {
    {NAME_OFFSET(eval), JSProto_Object},

    /* Global properties and functions defined by the Number class. */
    {NAME_OFFSET(NaN), JSProto_Number},
    {NAME_OFFSET(Infinity), JSProto_Number},
    {NAME_OFFSET(isNaN), JSProto_Number},
    {NAME_OFFSET(isFinite), JSProto_Number},
    {NAME_OFFSET(parseFloat), JSProto_Number},
    {NAME_OFFSET(parseInt), JSProto_Number},

    /* String global functions. */
    {NAME_OFFSET(escape), JSProto_String},
    {NAME_OFFSET(unescape), JSProto_String},
    {NAME_OFFSET(decodeURI), JSProto_String},
    {NAME_OFFSET(encodeURI), JSProto_String},
    {NAME_OFFSET(decodeURIComponent), JSProto_String},
    {NAME_OFFSET(encodeURIComponent), JSProto_String},
    {NAME_OFFSET(uneval), JSProto_String},

    {0, JSProto_LIMIT}};

static bool SkipUneval(jsid id, JSContext* cx) {
  return !cx->realm()->creationOptions().getToSourceEnabled() &&
         id == NameToId(cx->names().uneval);
}

static bool SkipSharedArrayBufferConstructor(JSProtoKey key,
                                             GlobalObject* global) {
  if (key != JSProto_SharedArrayBuffer) {
    return false;
  }

  const JS::RealmCreationOptions& options = global->realm()->creationOptions();
  MOZ_ASSERT(options.getSharedMemoryAndAtomicsEnabled(),
             "shouldn't contemplate defining SharedArrayBuffer if shared "
             "memory is disabled");

  // On the web, it isn't presently possible to expose the global
  // "SharedArrayBuffer" property unless the page is cross-site-isolated.  Only
  // define this constructor if an option on the realm indicates that it should
  // be defined.
  return !options.defineSharedArrayBufferConstructor();
}

JS_PUBLIC_API bool JS_ResolveStandardClass(JSContext* cx, HandleObject obj,
                                           HandleId id, bool* resolved) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(obj, id);

  Handle<GlobalObject*> global = obj.as<GlobalObject>();
  *resolved = false;

  if (!id.isAtom()) {
    return true;
  }

  /* Check whether we're resolving 'undefined', and define it if so. */
  JSAtom* idAtom = id.toAtom();
  if (idAtom == cx->names().undefined) {
    *resolved = true;
    return DefineDataProperty(
        cx, global, id, UndefinedHandleValue,
        JSPROP_PERMANENT | JSPROP_READONLY | JSPROP_RESOLVING);
  }

  // Resolve a "globalThis" self-referential property if necessary.
  if (idAtom == cx->names().globalThis) {
    return GlobalObject::maybeResolveGlobalThis(cx, global, resolved);
  }

  do {
    // Try for class constructors/prototypes named by well-known atoms.
    const JSStdName* stdnm =
        LookupStdName(cx->names(), idAtom, standard_class_names);
    if (!stdnm) {
      // Try less frequently used top-level functions and constants.
      stdnm = LookupStdName(cx->names(), idAtom, builtin_property_names);
      if (!stdnm) {
        break;
      }
    }

    if (GlobalObject::skipDeselectedConstructor(cx, stdnm->key) ||
        SkipUneval(id, cx)) {
      break;
    }

    if (JSProtoKey key = stdnm->key; key != JSProto_Null) {
      // If this class is anonymous (or it's "SharedArrayBuffer" but that global
      // constructor isn't supposed to be defined), then it doesn't exist as a
      // global property, so we won't resolve anything.
      const JSClass* clasp = ProtoKeyToClass(key);
      if ((!clasp || clasp->specShouldDefineConstructor()) &&
          !SkipSharedArrayBufferConstructor(key, global)) {
        if (!GlobalObject::ensureConstructor(cx, global, key)) {
          return false;
        }

        *resolved = true;
        return true;
      }
    }
  } while (false);

  // There is no such property to resolve. An ordinary resolve hook would
  // just return true at this point. But the global object is special in one
  // more way: its prototype chain is lazily initialized. That is,
  // global->getProto() might be null right now because we haven't created
  // Object.prototype yet. Force it now.
  return GlobalObject::getOrCreateObjectPrototype(cx, global);
}

JS_PUBLIC_API bool JS_MayResolveStandardClass(const JSAtomState& names, jsid id,
                                              JSObject* maybeObj) {
  MOZ_ASSERT_IF(maybeObj, maybeObj->is<GlobalObject>());

  // The global object's resolve hook is special: JS_ResolveStandardClass
  // initializes the prototype chain lazily. Only attempt to optimize here
  // if we know the prototype chain has been initialized.
  if (!maybeObj || !maybeObj->staticPrototype()) {
    return true;
  }

  if (!id.isAtom()) {
    return false;
  }

  JSAtom* atom = id.toAtom();

  // This will return true even for deselected constructors.  (To do
  // better, we need a JSContext here; it's fine as it is.)

  return atom == names.undefined || atom == names.globalThis ||
         LookupStdName(names, atom, standard_class_names) ||
         LookupStdName(names, atom, builtin_property_names);
}

JS_PUBLIC_API bool JS_EnumerateStandardClasses(JSContext* cx,
                                               HandleObject obj) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(obj);
  Handle<GlobalObject*> global = obj.as<GlobalObject>();
  return GlobalObject::initStandardClasses(cx, global);
}

static bool EnumerateStandardClassesInTable(JSContext* cx,
                                            Handle<GlobalObject*> global,
                                            MutableHandleIdVector properties,
                                            const JSStdName* table,
                                            bool includeResolved) {
  for (unsigned i = 0; !table[i].isSentinel(); i++) {
    if (table[i].isDummy()) {
      continue;
    }

    JSProtoKey key = table[i].key;

    // If the standard class has been resolved, the properties have been
    // defined on the global so we don't need to add them here.
    if (!includeResolved && global->isStandardClassResolved(key)) {
      continue;
    }

    if (GlobalObject::skipDeselectedConstructor(cx, key)) {
      continue;
    }

    if (const JSClass* clasp = ProtoKeyToClass(key)) {
      if (!clasp->specShouldDefineConstructor() ||
          SkipSharedArrayBufferConstructor(key, global)) {
        continue;
      }
    }

    jsid id = NameToId(AtomStateOffsetToName(cx->names(), table[i].atomOffset));

    if (SkipUneval(id, cx)) {
      continue;
    }

    if (!properties.append(id)) {
      return false;
    }
  }

  return true;
}

static bool EnumerateStandardClasses(JSContext* cx, JS::HandleObject obj,
                                     JS::MutableHandleIdVector properties,
                                     bool enumerableOnly,
                                     bool includeResolved) {
  if (enumerableOnly) {
    // There are no enumerable standard classes and "undefined" is
    // not enumerable.
    return true;
  }

  Handle<GlobalObject*> global = obj.as<GlobalObject>();

  // It's fine to always append |undefined| here, it's non-configurable and
  // the enumeration code filters duplicates.
  if (!properties.append(NameToId(cx->names().undefined))) {
    return false;
  }

  bool resolved = false;
  if (!GlobalObject::maybeResolveGlobalThis(cx, global, &resolved)) {
    return false;
  }
  if (resolved || includeResolved) {
    if (!properties.append(NameToId(cx->names().globalThis))) {
      return false;
    }
  }

  if (!EnumerateStandardClassesInTable(cx, global, properties,
                                       standard_class_names, includeResolved)) {
    return false;
  }
  if (!EnumerateStandardClassesInTable(
          cx, global, properties, builtin_property_names, includeResolved)) {
    return false;
  }

  return true;
}

JS_PUBLIC_API bool JS_NewEnumerateStandardClasses(
    JSContext* cx, JS::HandleObject obj, JS::MutableHandleIdVector properties,
    bool enumerableOnly) {
  return EnumerateStandardClasses(cx, obj, properties, enumerableOnly, false);
}

JS_PUBLIC_API bool JS_NewEnumerateStandardClassesIncludingResolved(
    JSContext* cx, JS::HandleObject obj, JS::MutableHandleIdVector properties,
    bool enumerableOnly) {
  return EnumerateStandardClasses(cx, obj, properties, enumerableOnly, true);
}

JS_PUBLIC_API bool JS_GetClassObject(JSContext* cx, JSProtoKey key,
                                     MutableHandleObject objp) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  JSObject* obj = GlobalObject::getOrCreateConstructor(cx, key);
  if (!obj) {
    return false;
  }
  objp.set(obj);
  return true;
}

JS_PUBLIC_API bool JS_GetClassPrototype(JSContext* cx, JSProtoKey key,
                                        MutableHandleObject objp) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  JSObject* proto = GlobalObject::getOrCreatePrototype(cx, key);
  if (!proto) {
    return false;
  }
  objp.set(proto);
  return true;
}

namespace JS {

JS_PUBLIC_API void ProtoKeyToId(JSContext* cx, JSProtoKey key,
                                MutableHandleId idp) {
  idp.set(NameToId(ClassName(key, cx)));
}

} /* namespace JS */

JS_PUBLIC_API JSProtoKey JS_IdToProtoKey(JSContext* cx, HandleId id) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(id);

  if (!id.isAtom()) {
    return JSProto_Null;
  }

  JSAtom* atom = id.toAtom();
  const JSStdName* stdnm =
      LookupStdName(cx->names(), atom, standard_class_names);
  if (!stdnm) {
    return JSProto_Null;
  }

  if (GlobalObject::skipDeselectedConstructor(cx, stdnm->key)) {
    return JSProto_Null;
  }

  if (SkipSharedArrayBufferConstructor(stdnm->key, cx->global())) {
    MOZ_ASSERT(id == NameToId(cx->names().SharedArrayBuffer));
    return JSProto_Null;
  }

  if (SkipUneval(id, cx)) {
    return JSProto_Null;
  }

  static_assert(std::size(standard_class_names) == JSProto_LIMIT + 1);
  return static_cast<JSProtoKey>(stdnm - standard_class_names);
}

extern JS_PUBLIC_API bool JS_IsGlobalObject(JSObject* obj) {
  return obj->is<GlobalObject>();
}

extern JS_PUBLIC_API JSObject* JS_GlobalLexicalEnvironment(JSObject* obj) {
  return &obj->as<GlobalObject>().lexicalEnvironment();
}

extern JS_PUBLIC_API bool JS_HasExtensibleLexicalEnvironment(JSObject* obj) {
  return obj->is<GlobalObject>() ||
         ObjectRealm::get(obj).getNonSyntacticLexicalEnvironment(obj);
}

extern JS_PUBLIC_API JSObject* JS_ExtensibleLexicalEnvironment(JSObject* obj) {
  return ExtensibleLexicalEnvironmentObject::forVarEnvironment(obj);
}

JS_PUBLIC_API JSObject* JS::CurrentGlobalOrNull(JSContext* cx) {
  AssertHeapIsIdleOrIterating();
  CHECK_THREAD(cx);
  if (!cx->realm()) {
    return nullptr;
  }
  return cx->global();
}

JS_PUBLIC_API JSObject* JS::GetNonCCWObjectGlobal(JSObject* obj) {
  AssertHeapIsIdleOrIterating();
  MOZ_DIAGNOSTIC_ASSERT(!IsCrossCompartmentWrapper(obj));
  return &obj->nonCCWGlobal();
}

JS_PUBLIC_API bool JS::detail::ComputeThis(JSContext* cx, Value* vp,
                                           MutableHandleObject thisObject) {
  AssertHeapIsIdle();
  cx->check(vp[0], vp[1]);

  MutableHandleValue thisv = MutableHandleValue::fromMarkedLocation(&vp[1]);
  JSObject* obj = BoxNonStrictThis(cx, thisv);
  if (!obj) {
    return false;
  }

  thisObject.set(obj);
  return true;
}

static bool gProfileTimelineRecordingEnabled = false;

JS_PUBLIC_API void JS::SetProfileTimelineRecordingEnabled(bool enabled) {
  gProfileTimelineRecordingEnabled = enabled;
}

JS_PUBLIC_API bool JS::IsProfileTimelineRecordingEnabled() {
  return gProfileTimelineRecordingEnabled;
}

JS_PUBLIC_API void* JS_malloc(JSContext* cx, size_t nbytes) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  return static_cast<void*>(cx->maybe_pod_malloc<uint8_t>(nbytes));
}

JS_PUBLIC_API void* JS_realloc(JSContext* cx, void* p, size_t oldBytes,
                               size_t newBytes) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  return static_cast<void*>(cx->maybe_pod_realloc<uint8_t>(
      static_cast<uint8_t*>(p), oldBytes, newBytes));
}

JS_PUBLIC_API void JS_free(JSContext* cx, void* p) { return js_free(p); }

JS_PUBLIC_API void* JS_string_malloc(JSContext* cx, size_t nbytes) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  return static_cast<void*>(
      cx->maybe_pod_arena_malloc<uint8_t>(js::StringBufferArena, nbytes));
}

JS_PUBLIC_API void* JS_string_realloc(JSContext* cx, void* p, size_t oldBytes,
                                      size_t newBytes) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  return static_cast<void*>(cx->maybe_pod_arena_realloc<uint8_t>(
      js::StringBufferArena, static_cast<uint8_t*>(p), oldBytes, newBytes));
}

JS_PUBLIC_API void JS_string_free(JSContext* cx, void* p) { return js_free(p); }

JS_PUBLIC_API void JS_freeop(JSFreeOp* fop, void* p) {
  return fop->freeUntracked(p);
}

JS_PUBLIC_API void JS::AddAssociatedMemory(JSObject* obj, size_t nbytes,
                                           JS::MemoryUse use) {
  MOZ_ASSERT(obj);
  if (!nbytes) {
    return;
  }

  Zone* zone = obj->zone();
  MOZ_ASSERT(!IsInsideNursery(obj));
  zone->addCellMemory(obj, nbytes, js::MemoryUse(use));
  zone->maybeTriggerGCOnMalloc();
}

JS_PUBLIC_API void JS::RemoveAssociatedMemory(JSObject* obj, size_t nbytes,
                                              JS::MemoryUse use) {
  MOZ_ASSERT(obj);
  if (!nbytes) {
    return;
  }

  JSRuntime* rt = obj->runtimeFromAnyThread();
  rt->defaultFreeOp()->removeCellMemory(obj, nbytes, js::MemoryUse(use));
}

#undef JS_AddRoot

JS_PUBLIC_API bool JS_AddExtraGCRootsTracer(JSContext* cx,
                                            JSTraceDataOp traceOp, void* data) {
  return cx->runtime()->gc.addBlackRootsTracer(traceOp, data);
}

JS_PUBLIC_API void JS_RemoveExtraGCRootsTracer(JSContext* cx,
                                               JSTraceDataOp traceOp,
                                               void* data) {
  return cx->runtime()->gc.removeBlackRootsTracer(traceOp, data);
}

JS_PUBLIC_API bool JS::IsIdleGCTaskNeeded(JSRuntime* rt) {
  // Currently, we only collect nursery during idle time.
  return rt->gc.nursery().shouldCollect();
}

JS_PUBLIC_API void JS::RunIdleTimeGCTask(JSRuntime* rt) {
  gc::GCRuntime& gc = rt->gc;
  if (gc.nursery().shouldCollect()) {
    gc.minorGC(JS::GCReason::IDLE_TIME_COLLECTION);
  }
}

JS_PUBLIC_API void JS_GC(JSContext* cx, JS::GCReason reason) {
  AssertHeapIsIdle();
  JS::PrepareForFullGC(cx);
  cx->runtime()->gc.gc(JS::GCOptions::Normal, reason);
}

JS_PUBLIC_API void JS_MaybeGC(JSContext* cx) {
  AssertHeapIsIdle();
  cx->runtime()->gc.maybeGC();
}

JS_PUBLIC_API void JS_SetGCCallback(JSContext* cx, JSGCCallback cb,
                                    void* data) {
  AssertHeapIsIdle();
  cx->runtime()->gc.setGCCallback(cb, data);
}

JS_PUBLIC_API void JS_SetObjectsTenuredCallback(JSContext* cx,
                                                JSObjectsTenuredCallback cb,
                                                void* data) {
  AssertHeapIsIdle();
  cx->runtime()->gc.setObjectsTenuredCallback(cb, data);
}

JS_PUBLIC_API bool JS_AddFinalizeCallback(JSContext* cx, JSFinalizeCallback cb,
                                          void* data) {
  AssertHeapIsIdle();
  return cx->runtime()->gc.addFinalizeCallback(cb, data);
}

JS_PUBLIC_API void JS_RemoveFinalizeCallback(JSContext* cx,
                                             JSFinalizeCallback cb) {
  cx->runtime()->gc.removeFinalizeCallback(cb);
}

JS_PUBLIC_API void JS::SetHostCleanupFinalizationRegistryCallback(
    JSContext* cx, JSHostCleanupFinalizationRegistryCallback cb, void* data) {
  AssertHeapIsIdle();
  cx->runtime()->gc.setHostCleanupFinalizationRegistryCallback(cb, data);
}

JS_PUBLIC_API void JS::ClearKeptObjects(JSContext* cx) {
  gc::GCRuntime* gc = &cx->runtime()->gc;

  for (ZonesIter zone(gc, ZoneSelector::WithAtoms); !zone.done(); zone.next()) {
    zone->clearKeptObjects();
  }
}

JS_PUBLIC_API bool JS::ZoneIsCollecting(JS::Zone* zone) {
  return zone->wasGCStarted();
}

JS_PUBLIC_API bool JS::AtomsZoneIsCollecting(JSRuntime* runtime) {
  return runtime->activeGCInAtomsZone();
}

JS_PUBLIC_API bool JS::IsAtomsZone(JS::Zone* zone) {
  return zone->isAtomsZone();
}

JS_PUBLIC_API bool JS_AddWeakPointerZonesCallback(JSContext* cx,
                                                  JSWeakPointerZonesCallback cb,
                                                  void* data) {
  AssertHeapIsIdle();
  return cx->runtime()->gc.addWeakPointerZonesCallback(cb, data);
}

JS_PUBLIC_API void JS_RemoveWeakPointerZonesCallback(
    JSContext* cx, JSWeakPointerZonesCallback cb) {
  cx->runtime()->gc.removeWeakPointerZonesCallback(cb);
}

JS_PUBLIC_API bool JS_AddWeakPointerCompartmentCallback(
    JSContext* cx, JSWeakPointerCompartmentCallback cb, void* data) {
  AssertHeapIsIdle();
  return cx->runtime()->gc.addWeakPointerCompartmentCallback(cb, data);
}

JS_PUBLIC_API void JS_RemoveWeakPointerCompartmentCallback(
    JSContext* cx, JSWeakPointerCompartmentCallback cb) {
  cx->runtime()->gc.removeWeakPointerCompartmentCallback(cb);
}

JS_PUBLIC_API void JS_UpdateWeakPointerAfterGC(JS::Heap<JSObject*>* objp) {
  JS_UpdateWeakPointerAfterGCUnbarriered(objp->unsafeGet());
}

JS_PUBLIC_API void JS_UpdateWeakPointerAfterGCUnbarriered(JSObject** objp) {
  if (IsAboutToBeFinalizedUnbarriered(objp)) {
    *objp = nullptr;
  }
}

JS_PUBLIC_API void JS_SetGCParameter(JSContext* cx, JSGCParamKey key,
                                     uint32_t value) {
  MOZ_ALWAYS_TRUE(cx->runtime()->gc.setParameter(key, value));
}

JS_PUBLIC_API void JS_ResetGCParameter(JSContext* cx, JSGCParamKey key) {
  cx->runtime()->gc.resetParameter(key);
}

JS_PUBLIC_API uint32_t JS_GetGCParameter(JSContext* cx, JSGCParamKey key) {
  return cx->runtime()->gc.getParameter(key);
}

JS_PUBLIC_API void JS_SetGCParametersBasedOnAvailableMemory(JSContext* cx,
                                                            uint32_t availMem) {
  struct JSGCConfig {
    JSGCParamKey key;
    uint32_t value;
  };

  static const JSGCConfig minimal[] = {
      {JSGC_SLICE_TIME_BUDGET_MS, 30},
      {JSGC_HIGH_FREQUENCY_TIME_LIMIT, 1500},
      {JSGC_LARGE_HEAP_SIZE_MIN, 40},
      {JSGC_SMALL_HEAP_SIZE_MAX, 0},
      {JSGC_HIGH_FREQUENCY_SMALL_HEAP_GROWTH, 300},
      {JSGC_HIGH_FREQUENCY_LARGE_HEAP_GROWTH, 120},
      {JSGC_LOW_FREQUENCY_HEAP_GROWTH, 120},
      {JSGC_HIGH_FREQUENCY_TIME_LIMIT, 1500},
      {JSGC_HIGH_FREQUENCY_TIME_LIMIT, 1500},
      {JSGC_HIGH_FREQUENCY_TIME_LIMIT, 1500},
      {JSGC_ALLOCATION_THRESHOLD, 1}};

  static const JSGCConfig nominal[] = {
      {JSGC_SLICE_TIME_BUDGET_MS, 30},
      {JSGC_HIGH_FREQUENCY_TIME_LIMIT, 1000},
      {JSGC_LARGE_HEAP_SIZE_MIN, 500},
      {JSGC_SMALL_HEAP_SIZE_MAX, 100},
      {JSGC_HIGH_FREQUENCY_SMALL_HEAP_GROWTH, 300},
      {JSGC_HIGH_FREQUENCY_LARGE_HEAP_GROWTH, 150},
      {JSGC_LOW_FREQUENCY_HEAP_GROWTH, 150},
      {JSGC_HIGH_FREQUENCY_TIME_LIMIT, 1500},
      {JSGC_HIGH_FREQUENCY_TIME_LIMIT, 1500},
      {JSGC_HIGH_FREQUENCY_TIME_LIMIT, 1500},
      {JSGC_ALLOCATION_THRESHOLD, 30}};

  const auto& configSet = availMem > 512 ? nominal : minimal;
  for (const auto& config : configSet) {
    JS_SetGCParameter(cx, config.key, config.value);
  }
}

JS_PUBLIC_API JSString* JS_NewExternalString(
    JSContext* cx, const char16_t* chars, size_t length,
    const JSExternalStringCallbacks* callbacks) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  return JSExternalString::new_(cx, chars, length, callbacks);
}

JS_PUBLIC_API JSString* JS_NewMaybeExternalString(
    JSContext* cx, const char16_t* chars, size_t length,
    const JSExternalStringCallbacks* callbacks, bool* allocatedExternal) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  return NewMaybeExternalString(cx, chars, length, callbacks,
                                allocatedExternal);
}

extern JS_PUBLIC_API const JSExternalStringCallbacks*
JS_GetExternalStringCallbacks(JSString* str) {
  return str->asExternal().callbacks();
}

static void SetNativeStackLimit(JSContext* cx, JS::StackKind kind,
                                size_t stackSize) {
#ifdef __wasi__
  // WASI makes this easy: we build with the "stack-first" wasm-ld option, so
  // the stack grows downward toward zero. Let's set a limit just a bit above
  // this so that we catch an overflow before a Wasm trap occurs.
  cx->nativeStackLimit[kind] = 1024;
#else  // __wasi__
#  if JS_STACK_GROWTH_DIRECTION > 0
  if (stackSize == 0) {
    cx->nativeStackLimit[kind] = UINTPTR_MAX;
  } else {
    MOZ_ASSERT(cx->nativeStackBase() <= size_t(-1) - stackSize);
    cx->nativeStackLimit[kind] = cx->nativeStackBase() + stackSize - 1;
  }
#  else   // stack grows up
  if (stackSize == 0) {
    cx->nativeStackLimit[kind] = 0;
  } else {
    MOZ_ASSERT(cx->nativeStackBase() >= stackSize);
    cx->nativeStackLimit[kind] = cx->nativeStackBase() - (stackSize - 1);
  }
#  endif  // stack grows down
#endif    // !__wasi__
}

JS_PUBLIC_API void JS_SetNativeStackQuota(JSContext* cx,
                                          size_t systemCodeStackSize,
                                          size_t trustedScriptStackSize,
                                          size_t untrustedScriptStackSize) {
  MOZ_ASSERT(!cx->activation());

  if (!trustedScriptStackSize) {
    trustedScriptStackSize = systemCodeStackSize;
  } else {
    MOZ_ASSERT(trustedScriptStackSize < systemCodeStackSize);
  }

  if (!untrustedScriptStackSize) {
    untrustedScriptStackSize = trustedScriptStackSize;
  } else {
    MOZ_ASSERT(untrustedScriptStackSize < trustedScriptStackSize);
  }

  SetNativeStackLimit(cx, JS::StackForSystemCode, systemCodeStackSize);
  SetNativeStackLimit(cx, JS::StackForTrustedScript, trustedScriptStackSize);
  SetNativeStackLimit(cx, JS::StackForUntrustedScript,
                      untrustedScriptStackSize);

  if (cx->isMainThreadContext()) {
    cx->initJitStackLimit();
  }
}

/************************************************************************/

JS_PUBLIC_API bool JS_ValueToId(JSContext* cx, HandleValue value,
                                MutableHandleId idp) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(value);
  return ToPropertyKey(cx, value, idp);
}

JS_PUBLIC_API bool JS_StringToId(JSContext* cx, HandleString string,
                                 MutableHandleId idp) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(string);
  RootedValue value(cx, StringValue(string));
  return PrimitiveValueToId<CanGC>(cx, value, idp);
}

JS_PUBLIC_API bool JS_IdToValue(JSContext* cx, jsid id, MutableHandleValue vp) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(id);
  vp.set(IdToValue(id));
  cx->check(vp);
  return true;
}

JS_PUBLIC_API bool JS::ToPrimitive(JSContext* cx, HandleObject obj, JSType hint,
                                   MutableHandleValue vp) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(obj);
  MOZ_ASSERT(obj != nullptr);
  MOZ_ASSERT(hint == JSTYPE_UNDEFINED || hint == JSTYPE_STRING ||
             hint == JSTYPE_NUMBER);
  vp.setObject(*obj);
  return ToPrimitiveSlow(cx, hint, vp);
}

JS_PUBLIC_API bool JS::GetFirstArgumentAsTypeHint(JSContext* cx, CallArgs args,
                                                  JSType* result) {
  if (!args.get(0).isString()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_NOT_EXPECTED_TYPE, "Symbol.toPrimitive",
                              "\"string\", \"number\", or \"default\"",
                              InformalValueTypeName(args.get(0)));
    return false;
  }

  RootedString str(cx, args.get(0).toString());
  bool match;

  if (!EqualStrings(cx, str, cx->names().default_, &match)) {
    return false;
  }
  if (match) {
    *result = JSTYPE_UNDEFINED;
    return true;
  }

  if (!EqualStrings(cx, str, cx->names().string, &match)) {
    return false;
  }
  if (match) {
    *result = JSTYPE_STRING;
    return true;
  }

  if (!EqualStrings(cx, str, cx->names().number, &match)) {
    return false;
  }
  if (match) {
    *result = JSTYPE_NUMBER;
    return true;
  }

  UniqueChars bytes;
  const char* source = ValueToSourceForError(cx, args.get(0), bytes);
  if (!source) {
    ReportOutOfMemory(cx);
    return false;
  }

  JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                           JSMSG_NOT_EXPECTED_TYPE, "Symbol.toPrimitive",
                           "\"string\", \"number\", or \"default\"", source);
  return false;
}

JS_PUBLIC_API JSObject* JS_InitClass(JSContext* cx, HandleObject obj,
                                     HandleObject parent_proto,
                                     const JSClass* clasp, JSNative constructor,
                                     unsigned nargs, const JSPropertySpec* ps,
                                     const JSFunctionSpec* fs,
                                     const JSPropertySpec* static_ps,
                                     const JSFunctionSpec* static_fs) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(obj, parent_proto);
  return InitClass(cx, obj, parent_proto, clasp, constructor, nargs, ps, fs,
                   static_ps, static_fs);
}

JS_PUBLIC_API bool JS_LinkConstructorAndPrototype(JSContext* cx,
                                                  HandleObject ctor,
                                                  HandleObject proto) {
  return LinkConstructorAndPrototype(cx, ctor, proto);
}

JS_PUBLIC_API bool JS_InstanceOf(JSContext* cx, HandleObject obj,
                                 const JSClass* clasp, CallArgs* args) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
#ifdef DEBUG
  if (args) {
    cx->check(obj);
    cx->check(args->thisv(), args->calleev());
  }
#endif
  if (!obj || obj->getClass() != clasp) {
    if (args) {
      ReportIncompatibleMethod(cx, *args, clasp);
    }
    return false;
  }
  return true;
}

JS_PUBLIC_API bool JS_HasInstance(JSContext* cx, HandleObject obj,
                                  HandleValue value, bool* bp) {
  AssertHeapIsIdle();
  cx->check(obj, value);
  return HasInstance(cx, obj, value, bp);
}

void JS::SetPrivate(JSObject* obj, void* data) {
  /* This function can be called by a finalizer. */
  obj->as<NativeObject>().setPrivate(data);
}

JS_PUBLIC_API void* JS_GetInstancePrivate(JSContext* cx, HandleObject obj,
                                          const JSClass* clasp,
                                          CallArgs* args) {
  if (!JS_InstanceOf(cx, obj, clasp, args)) {
    return nullptr;
  }
  return obj->as<NativeObject>().getPrivate();
}

JS_PUBLIC_API JSObject* JS_GetConstructor(JSContext* cx, HandleObject proto) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(proto);

  RootedValue cval(cx);
  if (!GetProperty(cx, proto, proto, cx->names().constructor, &cval)) {
    return nullptr;
  }
  if (!IsFunctionObject(cval)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_NO_CONSTRUCTOR, proto->getClass()->name);
    return nullptr;
  }
  return &cval.toObject();
}

JS::RealmCreationOptions&
JS::RealmCreationOptions::setNewCompartmentInSystemZone() {
  compSpec_ = CompartmentSpecifier::NewCompartmentInSystemZone;
  comp_ = nullptr;
  return *this;
}

JS::RealmCreationOptions&
JS::RealmCreationOptions::setNewCompartmentInExistingZone(JSObject* obj) {
  compSpec_ = CompartmentSpecifier::NewCompartmentInExistingZone;
  zone_ = obj->zone();
  return *this;
}

JS::RealmCreationOptions& JS::RealmCreationOptions::setExistingCompartment(
    JSObject* obj) {
  compSpec_ = CompartmentSpecifier::ExistingCompartment;
  comp_ = obj->compartment();
  return *this;
}

JS::RealmCreationOptions& JS::RealmCreationOptions::setExistingCompartment(
    JS::Compartment* compartment) {
  compSpec_ = CompartmentSpecifier::ExistingCompartment;
  comp_ = compartment;
  return *this;
}

JS::RealmCreationOptions& JS::RealmCreationOptions::setNewCompartmentAndZone() {
  compSpec_ = CompartmentSpecifier::NewCompartmentAndZone;
  comp_ = nullptr;
  return *this;
}

JS::RealmCreationOptions&
JS::RealmCreationOptions::setNewCompartmentInSelfHostingZone() {
  compSpec_ = CompartmentSpecifier::NewCompartmentInSelfHostingZone;
  comp_ = nullptr;
  return *this;
}

const JS::RealmCreationOptions& JS::RealmCreationOptionsRef(Realm* realm) {
  return realm->creationOptions();
}

const JS::RealmCreationOptions& JS::RealmCreationOptionsRef(JSContext* cx) {
  return cx->realm()->creationOptions();
}

bool JS::RealmCreationOptions::getSharedMemoryAndAtomicsEnabled() const {
  return sharedMemoryAndAtomics_;
}

JS::RealmCreationOptions&
JS::RealmCreationOptions::setSharedMemoryAndAtomicsEnabled(bool flag) {
  sharedMemoryAndAtomics_ = flag;
  return *this;
}

bool JS::RealmCreationOptions::getCoopAndCoepEnabled() const {
  return coopAndCoep_;
}

JS::RealmCreationOptions& JS::RealmCreationOptions::setCoopAndCoepEnabled(
    bool flag) {
  coopAndCoep_ = flag;
  return *this;
}

const JS::RealmBehaviors& JS::RealmBehaviorsRef(JS::Realm* realm) {
  return realm->behaviors();
}

const JS::RealmBehaviors& JS::RealmBehaviorsRef(JSContext* cx) {
  return cx->realm()->behaviors();
}

void JS::SetRealmNonLive(Realm* realm) { realm->setNonLive(); }

JS_PUBLIC_API JSObject* JS_NewGlobalObject(JSContext* cx, const JSClass* clasp,
                                           JSPrincipals* principals,
                                           JS::OnNewGlobalHookOption hookOption,
                                           const JS::RealmOptions& options) {
  MOZ_RELEASE_ASSERT(
      cx->runtime()->hasInitializedSelfHosting(),
      "Must call JS::InitSelfHostedCode() before creating a global");

  AssertHeapIsIdle();
  CHECK_THREAD(cx);

  return GlobalObject::new_(cx, clasp, principals, hookOption, options);
}

JS_PUBLIC_API void JS_GlobalObjectTraceHook(JSTracer* trc, JSObject* global) {
  GlobalObject* globalObj = &global->as<GlobalObject>();
  Realm* globalRealm = globalObj->realm();

  // Off thread parsing and compilation tasks create a dummy global which is
  // then merged back into the host realm. Since it used to be a global, it
  // will still have this trace hook, but it does not have a meaning relative
  // to its new realm. We can safely skip it.
  //
  // Similarly, if we GC when creating the global, we may not have set that
  // global's realm's global pointer yet. In this case, the realm will not yet
  // contain anything that needs to be traced.
  if (globalRealm->unsafeUnbarrieredMaybeGlobal() != globalObj) {
    return;
  }

  // Trace the realm for any GC things that should only stick around if we
  // know the global is live.
  globalRealm->traceGlobal(trc);

  if (JSTraceOp trace = globalRealm->creationOptions().getTrace()) {
    trace(trc, global);
  }
}

const JSClassOps JS::DefaultGlobalClassOps = {
    nullptr,                         // addProperty
    nullptr,                         // delProperty
    nullptr,                         // enumerate
    JS_NewEnumerateStandardClasses,  // newEnumerate
    JS_ResolveStandardClass,         // resolve
    JS_MayResolveStandardClass,      // mayResolve
    nullptr,                         // finalize
    nullptr,                         // call
    nullptr,                         // hasInstance
    nullptr,                         // construct
    JS_GlobalObjectTraceHook,        // trace
};

JS_PUBLIC_API void JS_FireOnNewGlobalObject(JSContext* cx,
                                            JS::HandleObject global) {
  // This hook is infallible, because we don't really want arbitrary script
  // to be able to throw errors during delicate global creation routines.
  // This infallibility will eat OOM and slow script, but if that happens
  // we'll likely run up into them again soon in a fallible context.
  cx->check(global);
  Rooted<js::GlobalObject*> globalObject(cx, &global->as<GlobalObject>());
  DebugAPI::onNewGlobalObject(cx, globalObject);
  cx->runtime()->ensureRealmIsRecordingAllocations(globalObject);
}

JS_PUBLIC_API JSObject* JS_NewObject(JSContext* cx, const JSClass* clasp) {
  MOZ_ASSERT(!cx->zone()->isAtomsZone());
  AssertHeapIsIdle();
  CHECK_THREAD(cx);

  if (!clasp) {
    clasp = &PlainObject::class_; /* default class is Object */
  }

  MOZ_ASSERT(clasp != &JSFunction::class_);
  MOZ_ASSERT(!(clasp->flags & JSCLASS_IS_GLOBAL));

  return NewBuiltinClassInstance(cx, clasp);
}

JS_PUBLIC_API JSObject* JS_NewObjectWithGivenProto(JSContext* cx,
                                                   const JSClass* clasp,
                                                   HandleObject proto) {
  MOZ_ASSERT(!cx->zone()->isAtomsZone());
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(proto);

  if (!clasp) {
    clasp = &PlainObject::class_; /* default class is Object */
  }

  MOZ_ASSERT(clasp != &JSFunction::class_);
  MOZ_ASSERT(!(clasp->flags & JSCLASS_IS_GLOBAL));

  return NewObjectWithGivenProto(cx, clasp, proto);
}

JS_PUBLIC_API JSObject* JS_NewPlainObject(JSContext* cx) {
  MOZ_ASSERT(!cx->zone()->isAtomsZone());
  AssertHeapIsIdle();
  CHECK_THREAD(cx);

  return NewBuiltinClassInstance<PlainObject>(cx);
}

JS_PUBLIC_API JSObject* JS_NewObjectForConstructor(JSContext* cx,
                                                   const JSClass* clasp,
                                                   const CallArgs& args) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);

  if (!ThrowIfNotConstructing(cx, args, clasp->name)) {
    return nullptr;
  }

  RootedObject newTarget(cx, &args.newTarget().toObject());
  cx->check(newTarget);
  return CreateThis(cx, clasp, newTarget);
}

JS_PUBLIC_API bool JS_IsNative(JSObject* obj) {
  return obj->is<NativeObject>();
}

JS_PUBLIC_API void JS::AssertObjectBelongsToCurrentThread(JSObject* obj) {
  JSRuntime* rt = obj->compartment()->runtimeFromAnyThread();
  MOZ_RELEASE_ASSERT(CurrentThreadCanAccessRuntime(rt));
}

JS_PUBLIC_API void JS::SetFilenameValidationCallback(
    JS::FilenameValidationCallback cb) {
  js::gFilenameValidationCallback = cb;
}

/*** Standard internal methods **********************************************/

JS_PUBLIC_API bool JS_GetPrototype(JSContext* cx, HandleObject obj,
                                   MutableHandleObject result) {
  cx->check(obj);
  return GetPrototype(cx, obj, result);
}

JS_PUBLIC_API bool JS_SetPrototype(JSContext* cx, HandleObject obj,
                                   HandleObject proto) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(obj, proto);

  return SetPrototype(cx, obj, proto);
}

JS_PUBLIC_API bool JS_GetPrototypeIfOrdinary(JSContext* cx, HandleObject obj,
                                             bool* isOrdinary,
                                             MutableHandleObject result) {
  cx->check(obj);
  return GetPrototypeIfOrdinary(cx, obj, isOrdinary, result);
}

JS_PUBLIC_API bool JS_IsExtensible(JSContext* cx, HandleObject obj,
                                   bool* extensible) {
  cx->check(obj);
  return IsExtensible(cx, obj, extensible);
}

JS_PUBLIC_API bool JS_PreventExtensions(JSContext* cx, JS::HandleObject obj,
                                        ObjectOpResult& result) {
  cx->check(obj);
  return PreventExtensions(cx, obj, result);
}

JS_PUBLIC_API bool JS_SetImmutablePrototype(JSContext* cx, JS::HandleObject obj,
                                            bool* succeeded) {
  cx->check(obj);
  return SetImmutablePrototype(cx, obj, succeeded);
}

JS_PUBLIC_API bool JS_GetOwnPropertyDescriptorById(
    JSContext* cx, HandleObject obj, HandleId id,
    MutableHandle<Maybe<PropertyDescriptor>> desc) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(obj, id);
  return GetOwnPropertyDescriptor(cx, obj, id, desc);
}

JS_PUBLIC_API bool JS_GetOwnPropertyDescriptor(
    JSContext* cx, HandleObject obj, const char* name,
    MutableHandle<Maybe<PropertyDescriptor>> desc) {
  JSAtom* atom = Atomize(cx, name, strlen(name));
  if (!atom) {
    return false;
  }
  RootedId id(cx, AtomToId(atom));
  return JS_GetOwnPropertyDescriptorById(cx, obj, id, desc);
}

JS_PUBLIC_API bool JS_GetOwnUCPropertyDescriptor(
    JSContext* cx, HandleObject obj, const char16_t* name, size_t namelen,
    MutableHandle<Maybe<PropertyDescriptor>> desc) {
  JSAtom* atom = AtomizeChars(cx, name, namelen);
  if (!atom) {
    return false;
  }
  RootedId id(cx, AtomToId(atom));
  return JS_GetOwnPropertyDescriptorById(cx, obj, id, desc);
}

JS_PUBLIC_API bool JS_GetPropertyDescriptorById(
    JSContext* cx, HandleObject obj, HandleId id,
    MutableHandle<Maybe<PropertyDescriptor>> desc, MutableHandleObject holder) {
  cx->check(obj, id);
  return GetPropertyDescriptor(cx, obj, id, desc, holder);
}

JS_PUBLIC_API bool JS_GetPropertyDescriptor(
    JSContext* cx, HandleObject obj, const char* name,
    MutableHandle<Maybe<PropertyDescriptor>> desc, MutableHandleObject holder) {
  JSAtom* atom = Atomize(cx, name, strlen(name));
  if (!atom) {
    return false;
  }
  RootedId id(cx, AtomToId(atom));
  return JS_GetPropertyDescriptorById(cx, obj, id, desc, holder);
}

JS_PUBLIC_API bool JS_GetUCPropertyDescriptor(
    JSContext* cx, HandleObject obj, const char16_t* name, size_t namelen,
    MutableHandle<Maybe<PropertyDescriptor>> desc, MutableHandleObject holder) {
  JSAtom* atom = AtomizeChars(cx, name, namelen);
  if (!atom) {
    return false;
  }
  RootedId id(cx, AtomToId(atom));
  return JS_GetPropertyDescriptorById(cx, obj, id, desc, holder);
}

static bool DefinePropertyByDescriptor(JSContext* cx, HandleObject obj,
                                       HandleId id,
                                       Handle<PropertyDescriptor> desc,
                                       ObjectOpResult& result) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(obj, id, desc);
  return DefineProperty(cx, obj, id, desc, result);
}

JS_PUBLIC_API bool JS_DefinePropertyById(JSContext* cx, HandleObject obj,
                                         HandleId id,
                                         Handle<PropertyDescriptor> desc,
                                         ObjectOpResult& result) {
  return DefinePropertyByDescriptor(cx, obj, id, desc, result);
}

JS_PUBLIC_API bool JS_DefinePropertyById(JSContext* cx, HandleObject obj,
                                         HandleId id,
                                         Handle<PropertyDescriptor> desc) {
  ObjectOpResult result;
  return DefinePropertyByDescriptor(cx, obj, id, desc, result) &&
         result.checkStrict(cx, obj, id);
}

static bool DefineAccessorPropertyById(JSContext* cx, HandleObject obj,
                                       HandleId id, HandleObject getter,
                                       HandleObject setter, unsigned attrs) {
  // JSPROP_READONLY has no meaning when accessors are involved. Ideally we'd
  // throw if this happens, but we've accepted it for long enough that it's
  // not worth trying to make callers change their ways. Just flip it off on
  // its way through the API layer so that we can enforce this internally.
  attrs &= ~JSPROP_READONLY;

  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(obj, id, getter, setter);

  return js::DefineAccessorProperty(cx, obj, id, getter, setter, attrs);
}

static bool DefineAccessorPropertyById(JSContext* cx, HandleObject obj,
                                       HandleId id, const JSNativeWrapper& get,
                                       const JSNativeWrapper& set,
                                       unsigned attrs) {
  // Getter/setter are both possibly-null JSNatives. Wrap them in JSFunctions.

  RootedFunction getter(cx);
  if (get.op) {
    RootedAtom atom(cx, IdToFunctionName(cx, id, FunctionPrefixKind::Get));
    if (!atom) {
      return false;
    }
    getter = NewNativeFunction(cx, get.op, 0, atom);
    if (!getter) {
      return false;
    }

    if (get.info) {
      getter->setJitInfo(get.info);
    }
  }

  RootedFunction setter(cx);
  if (set.op) {
    RootedAtom atom(cx, IdToFunctionName(cx, id, FunctionPrefixKind::Set));
    if (!atom) {
      return false;
    }
    setter = NewNativeFunction(cx, set.op, 1, atom);
    if (!setter) {
      return false;
    }

    if (set.info) {
      setter->setJitInfo(set.info);
    }
  }

  return DefineAccessorPropertyById(cx, obj, id, getter, setter, attrs);
}

static bool DefineDataPropertyById(JSContext* cx, HandleObject obj, HandleId id,
                                   HandleValue value, unsigned attrs) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(obj, id, value);

  return js::DefineDataProperty(cx, obj, id, value, attrs);
}

/*
 * Wrapper functions to create wrappers with no corresponding JSJitInfo from API
 * function arguments.
 */
static JSNativeWrapper NativeOpWrapper(Native native) {
  JSNativeWrapper ret;
  ret.op = native;
  ret.info = nullptr;
  return ret;
}

JS_PUBLIC_API bool JS_DefinePropertyById(JSContext* cx, HandleObject obj,
                                         HandleId id, HandleValue value,
                                         unsigned attrs) {
  return DefineDataPropertyById(cx, obj, id, value, attrs);
}

JS_PUBLIC_API bool JS_DefinePropertyById(JSContext* cx, HandleObject obj,
                                         HandleId id, Native getter,
                                         Native setter, unsigned attrs) {
  return DefineAccessorPropertyById(cx, obj, id, NativeOpWrapper(getter),
                                    NativeOpWrapper(setter), attrs);
}

JS_PUBLIC_API bool JS_DefinePropertyById(JSContext* cx, HandleObject obj,
                                         HandleId id, HandleObject getter,
                                         HandleObject setter, unsigned attrs) {
  return DefineAccessorPropertyById(cx, obj, id, getter, setter, attrs);
}

JS_PUBLIC_API bool JS_DefinePropertyById(JSContext* cx, HandleObject obj,
                                         HandleId id, HandleObject valueArg,
                                         unsigned attrs) {
  RootedValue value(cx, ObjectValue(*valueArg));
  return DefineDataPropertyById(cx, obj, id, value, attrs);
}

JS_PUBLIC_API bool JS_DefinePropertyById(JSContext* cx, HandleObject obj,
                                         HandleId id, HandleString valueArg,
                                         unsigned attrs) {
  RootedValue value(cx, StringValue(valueArg));
  return DefineDataPropertyById(cx, obj, id, value, attrs);
}

JS_PUBLIC_API bool JS_DefinePropertyById(JSContext* cx, HandleObject obj,
                                         HandleId id, int32_t valueArg,
                                         unsigned attrs) {
  Value value = Int32Value(valueArg);
  return DefineDataPropertyById(cx, obj, id,
                                HandleValue::fromMarkedLocation(&value), attrs);
}

JS_PUBLIC_API bool JS_DefinePropertyById(JSContext* cx, HandleObject obj,
                                         HandleId id, uint32_t valueArg,
                                         unsigned attrs) {
  Value value = NumberValue(valueArg);
  return DefineDataPropertyById(cx, obj, id,
                                HandleValue::fromMarkedLocation(&value), attrs);
}

JS_PUBLIC_API bool JS_DefinePropertyById(JSContext* cx, HandleObject obj,
                                         HandleId id, double valueArg,
                                         unsigned attrs) {
  Value value = NumberValue(valueArg);
  return DefineDataPropertyById(cx, obj, id,
                                HandleValue::fromMarkedLocation(&value), attrs);
}

static bool DefineDataProperty(JSContext* cx, HandleObject obj,
                               const char* name, HandleValue value,
                               unsigned attrs) {
  JSAtom* atom = Atomize(cx, name, strlen(name));
  if (!atom) {
    return false;
  }
  RootedId id(cx, AtomToId(atom));

  return DefineDataPropertyById(cx, obj, id, value, attrs);
}

JS_PUBLIC_API bool JS_DefineProperty(JSContext* cx, HandleObject obj,
                                     const char* name, HandleValue value,
                                     unsigned attrs) {
  return DefineDataProperty(cx, obj, name, value, attrs);
}

JS_PUBLIC_API bool JS_DefineProperty(JSContext* cx, HandleObject obj,
                                     const char* name, Native getter,
                                     Native setter, unsigned attrs) {
  JSAtom* atom = Atomize(cx, name, strlen(name));
  if (!atom) {
    return false;
  }
  RootedId id(cx, AtomToId(atom));
  return DefineAccessorPropertyById(cx, obj, id, NativeOpWrapper(getter),
                                    NativeOpWrapper(setter), attrs);
}

JS_PUBLIC_API bool JS_DefineProperty(JSContext* cx, HandleObject obj,
                                     const char* name, HandleObject getter,
                                     HandleObject setter, unsigned attrs) {
  JSAtom* atom = Atomize(cx, name, strlen(name));
  if (!atom) {
    return false;
  }
  RootedId id(cx, AtomToId(atom));

  return DefineAccessorPropertyById(cx, obj, id, getter, setter, attrs);
}

JS_PUBLIC_API bool JS_DefineProperty(JSContext* cx, HandleObject obj,
                                     const char* name, HandleObject valueArg,
                                     unsigned attrs) {
  RootedValue value(cx, ObjectValue(*valueArg));
  return DefineDataProperty(cx, obj, name, value, attrs);
}

JS_PUBLIC_API bool JS_DefineProperty(JSContext* cx, HandleObject obj,
                                     const char* name, HandleString valueArg,
                                     unsigned attrs) {
  RootedValue value(cx, StringValue(valueArg));
  return DefineDataProperty(cx, obj, name, value, attrs);
}

JS_PUBLIC_API bool JS_DefineProperty(JSContext* cx, HandleObject obj,
                                     const char* name, int32_t valueArg,
                                     unsigned attrs) {
  Value value = Int32Value(valueArg);
  return DefineDataProperty(cx, obj, name,
                            HandleValue::fromMarkedLocation(&value), attrs);
}

JS_PUBLIC_API bool JS_DefineProperty(JSContext* cx, HandleObject obj,
                                     const char* name, uint32_t valueArg,
                                     unsigned attrs) {
  Value value = NumberValue(valueArg);
  return DefineDataProperty(cx, obj, name,
                            HandleValue::fromMarkedLocation(&value), attrs);
}

JS_PUBLIC_API bool JS_DefineProperty(JSContext* cx, HandleObject obj,
                                     const char* name, double valueArg,
                                     unsigned attrs) {
  Value value = NumberValue(valueArg);
  return DefineDataProperty(cx, obj, name,
                            HandleValue::fromMarkedLocation(&value), attrs);
}

#define AUTO_NAMELEN(s, n) (((n) == (size_t)-1) ? js_strlen(s) : (n))

JS_PUBLIC_API bool JS_DefineUCProperty(JSContext* cx, HandleObject obj,
                                       const char16_t* name, size_t namelen,
                                       Handle<PropertyDescriptor> desc,
                                       ObjectOpResult& result) {
  JSAtom* atom = AtomizeChars(cx, name, AUTO_NAMELEN(name, namelen));
  if (!atom) {
    return false;
  }
  RootedId id(cx, AtomToId(atom));
  return DefinePropertyByDescriptor(cx, obj, id, desc, result);
}

JS_PUBLIC_API bool JS_DefineUCProperty(JSContext* cx, HandleObject obj,
                                       const char16_t* name, size_t namelen,
                                       Handle<PropertyDescriptor> desc) {
  JSAtom* atom = AtomizeChars(cx, name, AUTO_NAMELEN(name, namelen));
  if (!atom) {
    return false;
  }
  RootedId id(cx, AtomToId(atom));
  ObjectOpResult result;
  return DefinePropertyByDescriptor(cx, obj, id, desc, result) &&
         result.checkStrict(cx, obj, id);
}

static bool DefineUCDataProperty(JSContext* cx, HandleObject obj,
                                 const char16_t* name, size_t namelen,
                                 HandleValue value, unsigned attrs) {
  JSAtom* atom = AtomizeChars(cx, name, AUTO_NAMELEN(name, namelen));
  if (!atom) {
    return false;
  }
  RootedId id(cx, AtomToId(atom));
  return DefineDataPropertyById(cx, obj, id, value, attrs);
}

JS_PUBLIC_API bool JS_DefineUCProperty(JSContext* cx, HandleObject obj,
                                       const char16_t* name, size_t namelen,
                                       HandleValue value, unsigned attrs) {
  return DefineUCDataProperty(cx, obj, name, namelen, value, attrs);
}

JS_PUBLIC_API bool JS_DefineUCProperty(JSContext* cx, HandleObject obj,
                                       const char16_t* name, size_t namelen,
                                       HandleObject getter, HandleObject setter,
                                       unsigned attrs) {
  JSAtom* atom = AtomizeChars(cx, name, AUTO_NAMELEN(name, namelen));
  if (!atom) {
    return false;
  }
  RootedId id(cx, AtomToId(atom));
  return DefineAccessorPropertyById(cx, obj, id, getter, setter, attrs);
}

JS_PUBLIC_API bool JS_DefineUCProperty(JSContext* cx, HandleObject obj,
                                       const char16_t* name, size_t namelen,
                                       HandleObject valueArg, unsigned attrs) {
  RootedValue value(cx, ObjectValue(*valueArg));
  return DefineUCDataProperty(cx, obj, name, namelen, value, attrs);
}

JS_PUBLIC_API bool JS_DefineUCProperty(JSContext* cx, HandleObject obj,
                                       const char16_t* name, size_t namelen,
                                       HandleString valueArg, unsigned attrs) {
  RootedValue value(cx, StringValue(valueArg));
  return DefineUCDataProperty(cx, obj, name, namelen, value, attrs);
}

JS_PUBLIC_API bool JS_DefineUCProperty(JSContext* cx, HandleObject obj,
                                       const char16_t* name, size_t namelen,
                                       int32_t valueArg, unsigned attrs) {
  Value value = Int32Value(valueArg);
  return DefineUCDataProperty(cx, obj, name, namelen,
                              HandleValue::fromMarkedLocation(&value), attrs);
}

JS_PUBLIC_API bool JS_DefineUCProperty(JSContext* cx, HandleObject obj,
                                       const char16_t* name, size_t namelen,
                                       uint32_t valueArg, unsigned attrs) {
  Value value = NumberValue(valueArg);
  return DefineUCDataProperty(cx, obj, name, namelen,
                              HandleValue::fromMarkedLocation(&value), attrs);
}

JS_PUBLIC_API bool JS_DefineUCProperty(JSContext* cx, HandleObject obj,
                                       const char16_t* name, size_t namelen,
                                       double valueArg, unsigned attrs) {
  Value value = NumberValue(valueArg);
  return DefineUCDataProperty(cx, obj, name, namelen,
                              HandleValue::fromMarkedLocation(&value), attrs);
}

static bool DefineDataElement(JSContext* cx, HandleObject obj, uint32_t index,
                              HandleValue value, unsigned attrs) {
  cx->check(obj, value);
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  RootedId id(cx);
  if (!IndexToId(cx, index, &id)) {
    return false;
  }
  return DefineDataPropertyById(cx, obj, id, value, attrs);
}

JS_PUBLIC_API bool JS_DefineElement(JSContext* cx, HandleObject obj,
                                    uint32_t index, HandleValue value,
                                    unsigned attrs) {
  return ::DefineDataElement(cx, obj, index, value, attrs);
}

JS_PUBLIC_API bool JS_DefineElement(JSContext* cx, HandleObject obj,
                                    uint32_t index, HandleObject getter,
                                    HandleObject setter, unsigned attrs) {
  RootedId id(cx);
  if (!IndexToId(cx, index, &id)) {
    return false;
  }
  return DefineAccessorPropertyById(cx, obj, id, getter, setter, attrs);
}

JS_PUBLIC_API bool JS_DefineElement(JSContext* cx, HandleObject obj,
                                    uint32_t index, HandleObject valueArg,
                                    unsigned attrs) {
  RootedValue value(cx, ObjectValue(*valueArg));
  return ::DefineDataElement(cx, obj, index, value, attrs);
}

JS_PUBLIC_API bool JS_DefineElement(JSContext* cx, HandleObject obj,
                                    uint32_t index, HandleString valueArg,
                                    unsigned attrs) {
  RootedValue value(cx, StringValue(valueArg));
  return ::DefineDataElement(cx, obj, index, value, attrs);
}

JS_PUBLIC_API bool JS_DefineElement(JSContext* cx, HandleObject obj,
                                    uint32_t index, int32_t valueArg,
                                    unsigned attrs) {
  Value value = Int32Value(valueArg);
  return ::DefineDataElement(cx, obj, index,
                             HandleValue::fromMarkedLocation(&value), attrs);
}

JS_PUBLIC_API bool JS_DefineElement(JSContext* cx, HandleObject obj,
                                    uint32_t index, uint32_t valueArg,
                                    unsigned attrs) {
  Value value = NumberValue(valueArg);
  return ::DefineDataElement(cx, obj, index,
                             HandleValue::fromMarkedLocation(&value), attrs);
}

JS_PUBLIC_API bool JS_DefineElement(JSContext* cx, HandleObject obj,
                                    uint32_t index, double valueArg,
                                    unsigned attrs) {
  Value value = NumberValue(valueArg);
  return ::DefineDataElement(cx, obj, index,
                             HandleValue::fromMarkedLocation(&value), attrs);
}

JS_PUBLIC_API bool JS_HasPropertyById(JSContext* cx, HandleObject obj,
                                      HandleId id, bool* foundp) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(obj, id);

  return HasProperty(cx, obj, id, foundp);
}

JS_PUBLIC_API bool JS_HasProperty(JSContext* cx, HandleObject obj,
                                  const char* name, bool* foundp) {
  JSAtom* atom = Atomize(cx, name, strlen(name));
  if (!atom) {
    return false;
  }
  RootedId id(cx, AtomToId(atom));
  return JS_HasPropertyById(cx, obj, id, foundp);
}

JS_PUBLIC_API bool JS_HasUCProperty(JSContext* cx, HandleObject obj,
                                    const char16_t* name, size_t namelen,
                                    bool* foundp) {
  JSAtom* atom = AtomizeChars(cx, name, AUTO_NAMELEN(name, namelen));
  if (!atom) {
    return false;
  }
  RootedId id(cx, AtomToId(atom));
  return JS_HasPropertyById(cx, obj, id, foundp);
}

JS_PUBLIC_API bool JS_HasElement(JSContext* cx, HandleObject obj,
                                 uint32_t index, bool* foundp) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  RootedId id(cx);
  if (!IndexToId(cx, index, &id)) {
    return false;
  }
  return JS_HasPropertyById(cx, obj, id, foundp);
}

JS_PUBLIC_API bool JS_HasOwnPropertyById(JSContext* cx, HandleObject obj,
                                         HandleId id, bool* foundp) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(obj, id);

  return HasOwnProperty(cx, obj, id, foundp);
}

JS_PUBLIC_API bool JS_HasOwnProperty(JSContext* cx, HandleObject obj,
                                     const char* name, bool* foundp) {
  JSAtom* atom = Atomize(cx, name, strlen(name));
  if (!atom) {
    return false;
  }
  RootedId id(cx, AtomToId(atom));
  return JS_HasOwnPropertyById(cx, obj, id, foundp);
}

JS_PUBLIC_API bool JS_ForwardGetPropertyTo(JSContext* cx, HandleObject obj,
                                           HandleId id, HandleValue receiver,
                                           MutableHandleValue vp) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(obj, id, receiver);

  return GetProperty(cx, obj, receiver, id, vp);
}

JS_PUBLIC_API bool JS_ForwardGetElementTo(JSContext* cx, HandleObject obj,
                                          uint32_t index, HandleObject receiver,
                                          MutableHandleValue vp) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(obj);

  return GetElement(cx, obj, receiver, index, vp);
}

JS_PUBLIC_API bool JS_GetPropertyById(JSContext* cx, HandleObject obj,
                                      HandleId id, MutableHandleValue vp) {
  RootedValue receiver(cx, ObjectValue(*obj));
  return JS_ForwardGetPropertyTo(cx, obj, id, receiver, vp);
}

JS_PUBLIC_API bool JS_GetProperty(JSContext* cx, HandleObject obj,
                                  const char* name, MutableHandleValue vp) {
  JSAtom* atom = Atomize(cx, name, strlen(name));
  if (!atom) {
    return false;
  }
  RootedId id(cx, AtomToId(atom));
  return JS_GetPropertyById(cx, obj, id, vp);
}

JS_PUBLIC_API bool JS_GetUCProperty(JSContext* cx, HandleObject obj,
                                    const char16_t* name, size_t namelen,
                                    MutableHandleValue vp) {
  JSAtom* atom = AtomizeChars(cx, name, AUTO_NAMELEN(name, namelen));
  if (!atom) {
    return false;
  }
  RootedId id(cx, AtomToId(atom));
  return JS_GetPropertyById(cx, obj, id, vp);
}

JS_PUBLIC_API bool JS_GetElement(JSContext* cx, HandleObject objArg,
                                 uint32_t index, MutableHandleValue vp) {
  return JS_ForwardGetElementTo(cx, objArg, index, objArg, vp);
}

JS_PUBLIC_API bool JS_ForwardSetPropertyTo(JSContext* cx, HandleObject obj,
                                           HandleId id, HandleValue v,
                                           HandleValue receiver,
                                           ObjectOpResult& result) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(obj, id, v, receiver);

  return SetProperty(cx, obj, id, v, receiver, result);
}

JS_PUBLIC_API bool JS_SetPropertyById(JSContext* cx, HandleObject obj,
                                      HandleId id, HandleValue v) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(obj, id, v);

  RootedValue receiver(cx, ObjectValue(*obj));
  ObjectOpResult ignored;
  return SetProperty(cx, obj, id, v, receiver, ignored);
}

JS_PUBLIC_API bool JS_SetProperty(JSContext* cx, HandleObject obj,
                                  const char* name, HandleValue v) {
  JSAtom* atom = Atomize(cx, name, strlen(name));
  if (!atom) {
    return false;
  }
  RootedId id(cx, AtomToId(atom));
  return JS_SetPropertyById(cx, obj, id, v);
}

JS_PUBLIC_API bool JS_SetUCProperty(JSContext* cx, HandleObject obj,
                                    const char16_t* name, size_t namelen,
                                    HandleValue v) {
  JSAtom* atom = AtomizeChars(cx, name, AUTO_NAMELEN(name, namelen));
  if (!atom) {
    return false;
  }
  RootedId id(cx, AtomToId(atom));
  return JS_SetPropertyById(cx, obj, id, v);
}

static bool SetElement(JSContext* cx, HandleObject obj, uint32_t index,
                       HandleValue v) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(obj, v);

  RootedValue receiver(cx, ObjectValue(*obj));
  ObjectOpResult ignored;
  return SetElement(cx, obj, index, v, receiver, ignored);
}

JS_PUBLIC_API bool JS_SetElement(JSContext* cx, HandleObject obj,
                                 uint32_t index, HandleValue v) {
  return SetElement(cx, obj, index, v);
}

JS_PUBLIC_API bool JS_SetElement(JSContext* cx, HandleObject obj,
                                 uint32_t index, HandleObject v) {
  RootedValue value(cx, ObjectOrNullValue(v));
  return SetElement(cx, obj, index, value);
}

JS_PUBLIC_API bool JS_SetElement(JSContext* cx, HandleObject obj,
                                 uint32_t index, HandleString v) {
  RootedValue value(cx, StringValue(v));
  return SetElement(cx, obj, index, value);
}

JS_PUBLIC_API bool JS_SetElement(JSContext* cx, HandleObject obj,
                                 uint32_t index, int32_t v) {
  RootedValue value(cx, NumberValue(v));
  return SetElement(cx, obj, index, value);
}

JS_PUBLIC_API bool JS_SetElement(JSContext* cx, HandleObject obj,
                                 uint32_t index, uint32_t v) {
  RootedValue value(cx, NumberValue(v));
  return SetElement(cx, obj, index, value);
}

JS_PUBLIC_API bool JS_SetElement(JSContext* cx, HandleObject obj,
                                 uint32_t index, double v) {
  RootedValue value(cx, NumberValue(v));
  return SetElement(cx, obj, index, value);
}

JS_PUBLIC_API bool JS_DeletePropertyById(JSContext* cx, HandleObject obj,
                                         HandleId id, ObjectOpResult& result) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(obj, id);

  return DeleteProperty(cx, obj, id, result);
}

JS_PUBLIC_API bool JS_DeleteProperty(JSContext* cx, HandleObject obj,
                                     const char* name, ObjectOpResult& result) {
  CHECK_THREAD(cx);
  cx->check(obj);

  JSAtom* atom = Atomize(cx, name, strlen(name));
  if (!atom) {
    return false;
  }
  RootedId id(cx, AtomToId(atom));
  return DeleteProperty(cx, obj, id, result);
}

JS_PUBLIC_API bool JS_DeleteUCProperty(JSContext* cx, HandleObject obj,
                                       const char16_t* name, size_t namelen,
                                       ObjectOpResult& result) {
  CHECK_THREAD(cx);
  cx->check(obj);

  JSAtom* atom = AtomizeChars(cx, name, AUTO_NAMELEN(name, namelen));
  if (!atom) {
    return false;
  }
  RootedId id(cx, AtomToId(atom));
  return DeleteProperty(cx, obj, id, result);
}

JS_PUBLIC_API bool JS_DeleteElement(JSContext* cx, HandleObject obj,
                                    uint32_t index, ObjectOpResult& result) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(obj);

  return DeleteElement(cx, obj, index, result);
}

JS_PUBLIC_API bool JS_DeletePropertyById(JSContext* cx, HandleObject obj,
                                         HandleId id) {
  ObjectOpResult ignored;
  return JS_DeletePropertyById(cx, obj, id, ignored);
}

JS_PUBLIC_API bool JS_DeleteProperty(JSContext* cx, HandleObject obj,
                                     const char* name) {
  ObjectOpResult ignored;
  return JS_DeleteProperty(cx, obj, name, ignored);
}

JS_PUBLIC_API bool JS_DeleteElement(JSContext* cx, HandleObject obj,
                                    uint32_t index) {
  ObjectOpResult ignored;
  return JS_DeleteElement(cx, obj, index, ignored);
}

JS_PUBLIC_API bool JS_Enumerate(JSContext* cx, HandleObject obj,
                                JS::MutableHandle<IdVector> props) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(obj, props);
  MOZ_ASSERT(props.empty());

  RootedIdVector ids(cx);
  if (!GetPropertyKeys(cx, obj, JSITER_OWNONLY, &ids)) {
    return false;
  }

  return props.append(ids.begin(), ids.end());
}

JS_PUBLIC_API bool JS::IsCallable(JSObject* obj) { return obj->isCallable(); }

JS_PUBLIC_API bool JS::IsConstructor(JSObject* obj) {
  return obj->isConstructor();
}

JS_PUBLIC_API bool JS_CallFunctionValue(JSContext* cx, HandleObject obj,
                                        HandleValue fval,
                                        const HandleValueArray& args,
                                        MutableHandleValue rval) {
  MOZ_ASSERT(!cx->zone()->isAtomsZone());
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(obj, fval, args);

  InvokeArgs iargs(cx);
  if (!FillArgumentsFromArraylike(cx, iargs, args)) {
    return false;
  }

  RootedValue thisv(cx, ObjectOrNullValue(obj));
  return Call(cx, fval, thisv, iargs, rval);
}

JS_PUBLIC_API bool JS_CallFunction(JSContext* cx, HandleObject obj,
                                   HandleFunction fun,
                                   const HandleValueArray& args,
                                   MutableHandleValue rval) {
  MOZ_ASSERT(!cx->zone()->isAtomsZone());
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(obj, fun, args);

  InvokeArgs iargs(cx);
  if (!FillArgumentsFromArraylike(cx, iargs, args)) {
    return false;
  }

  RootedValue fval(cx, ObjectValue(*fun));
  RootedValue thisv(cx, ObjectOrNullValue(obj));
  return Call(cx, fval, thisv, iargs, rval);
}

JS_PUBLIC_API bool JS_CallFunctionName(JSContext* cx, HandleObject obj,
                                       const char* name,
                                       const HandleValueArray& args,
                                       MutableHandleValue rval) {
  MOZ_ASSERT(!cx->zone()->isAtomsZone());
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(obj, args);

  JSAtom* atom = Atomize(cx, name, strlen(name));
  if (!atom) {
    return false;
  }

  RootedValue fval(cx);
  RootedId id(cx, AtomToId(atom));
  if (!GetProperty(cx, obj, obj, id, &fval)) {
    return false;
  }

  InvokeArgs iargs(cx);
  if (!FillArgumentsFromArraylike(cx, iargs, args)) {
    return false;
  }

  RootedValue thisv(cx, ObjectOrNullValue(obj));
  return Call(cx, fval, thisv, iargs, rval);
}

JS_PUBLIC_API bool JS::Call(JSContext* cx, HandleValue thisv, HandleValue fval,
                            const JS::HandleValueArray& args,
                            MutableHandleValue rval) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(thisv, fval, args);

  InvokeArgs iargs(cx);
  if (!FillArgumentsFromArraylike(cx, iargs, args)) {
    return false;
  }

  return Call(cx, fval, thisv, iargs, rval);
}

JS_PUBLIC_API bool JS::Construct(JSContext* cx, HandleValue fval,
                                 HandleObject newTarget,
                                 const JS::HandleValueArray& args,
                                 MutableHandleObject objp) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(fval, newTarget, args);

  if (!IsConstructor(fval)) {
    ReportValueError(cx, JSMSG_NOT_CONSTRUCTOR, JSDVG_IGNORE_STACK, fval,
                     nullptr);
    return false;
  }

  RootedValue newTargetVal(cx, ObjectValue(*newTarget));
  if (!IsConstructor(newTargetVal)) {
    ReportValueError(cx, JSMSG_NOT_CONSTRUCTOR, JSDVG_IGNORE_STACK,
                     newTargetVal, nullptr);
    return false;
  }

  ConstructArgs cargs(cx);
  if (!FillArgumentsFromArraylike(cx, cargs, args)) {
    return false;
  }

  return js::Construct(cx, fval, cargs, newTargetVal, objp);
}

JS_PUBLIC_API bool JS::Construct(JSContext* cx, HandleValue fval,
                                 const JS::HandleValueArray& args,
                                 MutableHandleObject objp) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(fval, args);

  if (!IsConstructor(fval)) {
    ReportValueError(cx, JSMSG_NOT_CONSTRUCTOR, JSDVG_IGNORE_STACK, fval,
                     nullptr);
    return false;
  }

  ConstructArgs cargs(cx);
  if (!FillArgumentsFromArraylike(cx, cargs, args)) {
    return false;
  }

  return js::Construct(cx, fval, cargs, fval, objp);
}

/* * */

JS_PUBLIC_API bool JS_AlreadyHasOwnPropertyById(JSContext* cx, HandleObject obj,
                                                HandleId id, bool* foundp) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(obj, id);

  if (!obj->is<NativeObject>()) {
    return js::HasOwnProperty(cx, obj, id, foundp);
  }

  PropertyResult prop;
  if (!NativeLookupOwnPropertyNoResolve(cx, &obj->as<NativeObject>(), id,
                                        &prop)) {
    return false;
  }
  *foundp = prop.isFound();
  return true;
}

JS_PUBLIC_API bool JS_AlreadyHasOwnProperty(JSContext* cx, HandleObject obj,
                                            const char* name, bool* foundp) {
  JSAtom* atom = Atomize(cx, name, strlen(name));
  if (!atom) {
    return false;
  }
  RootedId id(cx, AtomToId(atom));
  return JS_AlreadyHasOwnPropertyById(cx, obj, id, foundp);
}

JS_PUBLIC_API bool JS_AlreadyHasOwnUCProperty(JSContext* cx, HandleObject obj,
                                              const char16_t* name,
                                              size_t namelen, bool* foundp) {
  JSAtom* atom = AtomizeChars(cx, name, AUTO_NAMELEN(name, namelen));
  if (!atom) {
    return false;
  }
  RootedId id(cx, AtomToId(atom));
  return JS_AlreadyHasOwnPropertyById(cx, obj, id, foundp);
}

JS_PUBLIC_API bool JS_AlreadyHasOwnElement(JSContext* cx, HandleObject obj,
                                           uint32_t index, bool* foundp) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  RootedId id(cx);
  if (!IndexToId(cx, index, &id)) {
    return false;
  }
  return JS_AlreadyHasOwnPropertyById(cx, obj, id, foundp);
}

JS_PUBLIC_API bool JS_FreezeObject(JSContext* cx, HandleObject obj) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(obj);
  return FreezeObject(cx, obj);
}

static bool DeepFreezeSlot(JSContext* cx, const Value& v) {
  if (v.isPrimitive()) {
    return true;
  }
  RootedObject obj(cx, &v.toObject());
  return JS_DeepFreezeObject(cx, obj);
}

JS_PUBLIC_API bool JS_DeepFreezeObject(JSContext* cx, HandleObject obj) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(obj);

  // Assume that non-extensible objects are already deep-frozen, to avoid
  // divergence.
  bool extensible;
  if (!IsExtensible(cx, obj, &extensible)) {
    return false;
  }
  if (!extensible) {
    return true;
  }

  if (!FreezeObject(cx, obj)) {
    return false;
  }

  // Walk slots in obj and if any value is a non-null object, seal it.
  if (obj->is<NativeObject>()) {
    RootedNativeObject nobj(cx, &obj->as<NativeObject>());
    for (uint32_t i = 0, n = nobj->slotSpan(); i < n; ++i) {
      if (!DeepFreezeSlot(cx, nobj->getSlot(i))) {
        return false;
      }
    }
    for (uint32_t i = 0, n = nobj->getDenseInitializedLength(); i < n; ++i) {
      if (!DeepFreezeSlot(cx, nobj->getDenseElement(i))) {
        return false;
      }
    }
  }

  return true;
}

static bool DefineSelfHostedProperty(JSContext* cx, HandleObject obj,
                                     HandleId id, const char* getterName,
                                     const char* setterName, unsigned attrs) {
  JSAtom* getterNameAtom = Atomize(cx, getterName, strlen(getterName));
  if (!getterNameAtom) {
    return false;
  }
  RootedPropertyName getterNameName(cx, getterNameAtom->asPropertyName());

  RootedAtom name(cx, IdToFunctionName(cx, id));
  if (!name) {
    return false;
  }

  RootedValue getterValue(cx);
  if (!GlobalObject::getSelfHostedFunction(cx, cx->global(), getterNameName,
                                           name, 0, &getterValue)) {
    return false;
  }
  MOZ_ASSERT(getterValue.isObject() && getterValue.toObject().is<JSFunction>());
  RootedFunction getterFunc(cx, &getterValue.toObject().as<JSFunction>());

  RootedFunction setterFunc(cx);
  if (setterName) {
    JSAtom* setterNameAtom = Atomize(cx, setterName, strlen(setterName));
    if (!setterNameAtom) {
      return false;
    }
    RootedPropertyName setterNameName(cx, setterNameAtom->asPropertyName());

    RootedValue setterValue(cx);
    if (!GlobalObject::getSelfHostedFunction(cx, cx->global(), setterNameName,
                                             name, 1, &setterValue)) {
      return false;
    }
    MOZ_ASSERT(setterValue.isObject() &&
               setterValue.toObject().is<JSFunction>());
    setterFunc = &setterValue.toObject().as<JSFunction>();
  }

  return DefineAccessorPropertyById(cx, obj, id, getterFunc, setterFunc, attrs);
}

JS_PUBLIC_API JSObject* JS_DefineObject(JSContext* cx, HandleObject obj,
                                        const char* name, const JSClass* clasp,
                                        unsigned attrs) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(obj);

  if (!clasp) {
    clasp = &PlainObject::class_; /* default class is Object */
  }

  RootedObject nobj(cx, NewBuiltinClassInstance(cx, clasp));
  if (!nobj) {
    return nullptr;
  }

  RootedValue nobjValue(cx, ObjectValue(*nobj));
  if (!DefineDataProperty(cx, obj, name, nobjValue, attrs)) {
    return nullptr;
  }

  return nobj;
}

JS_PUBLIC_API bool JSPropertySpec::getValue(JSContext* cx,
                                            MutableHandleValue vp) const {
  MOZ_ASSERT(!isAccessor());

  switch (u.value.type) {
    case ValueWrapper::Type::String: {
      RootedAtom atom(cx, Atomize(cx, u.value.string, strlen(u.value.string)));
      if (!atom) {
        return false;
      }
      vp.setString(atom);
      return true;
    }

    case ValueWrapper::Type::Int32:
      vp.setInt32(u.value.int32);
      return true;

    case ValueWrapper::Type::Double:
      vp.setDouble(u.value.double_);
      return true;
  }

  MOZ_CRASH("Unexpected type");
}

bool PropertySpecNameToId(JSContext* cx, JSPropertySpec::Name name,
                          MutableHandleId id,
                          js::PinningBehavior pin = js::DoNotPinAtom) {
  if (name.isSymbol()) {
    id.set(SYMBOL_TO_JSID(cx->wellKnownSymbols().get(name.symbol())));
  } else {
    JSAtom* atom = Atomize(cx, name.string(), strlen(name.string()), pin);
    if (!atom) {
      return false;
    }
    id.set(AtomToId(atom));
  }
  return true;
}

JS_PUBLIC_API bool JS::PropertySpecNameToPermanentId(JSContext* cx,
                                                     JSPropertySpec::Name name,
                                                     jsid* idp) {
  // We are calling fromMarkedLocation(idp) even though idp points to a
  // location that will never be marked. This is OK because the whole point
  // of this API is to populate *idp with a jsid that does not need to be
  // marked.
  return PropertySpecNameToId(
      cx, name, MutableHandleId::fromMarkedLocation(idp), js::PinAtom);
}

JS_PUBLIC_API bool JS_DefineProperties(JSContext* cx, HandleObject obj,
                                       const JSPropertySpec* ps) {
  RootedId id(cx);

  for (; ps->name; ps++) {
    if (!PropertySpecNameToId(cx, ps->name, &id)) {
      return false;
    }

    if (ps->isAccessor()) {
      if (ps->isSelfHosted()) {
        if (!DefineSelfHostedProperty(
                cx, obj, id, ps->u.accessors.getter.selfHosted.funname,
                ps->u.accessors.setter.selfHosted.funname, ps->attributes())) {
          return false;
        }
      } else {
        if (!DefineAccessorPropertyById(
                cx, obj, id, ps->u.accessors.getter.native,
                ps->u.accessors.setter.native, ps->attributes())) {
          return false;
        }
      }
    } else {
      RootedValue v(cx);
      if (!ps->getValue(cx, &v)) {
        return false;
      }

      if (!DefineDataPropertyById(cx, obj, id, v, ps->attributes())) {
        return false;
      }
    }
  }
  return true;
}

JS_PUBLIC_API bool JS::ObjectToCompletePropertyDescriptor(
    JSContext* cx, HandleObject obj, HandleValue descObj,
    MutableHandle<PropertyDescriptor> desc) {
  // |obj| can be in a different compartment here. The caller is responsible
  // for wrapping it (see JS_WrapPropertyDescriptor).
  cx->check(descObj);
  if (!ToPropertyDescriptor(cx, descObj, true, desc)) {
    return false;
  }
  CompletePropertyDescriptor(desc);
  return true;
}

JS_PUBLIC_API void JS_SetAllNonReservedSlotsToUndefined(JS::HandleObject obj) {
  if (!obj->is<NativeObject>()) {
    return;
  }

  const JSClass* clasp = obj->getClass();
  unsigned numReserved = JSCLASS_RESERVED_SLOTS(clasp);
  unsigned numSlots = obj->as<NativeObject>().slotSpan();
  for (unsigned i = numReserved; i < numSlots; i++) {
    obj->as<NativeObject>().setSlot(i, UndefinedValue());
  }
}

JS_PUBLIC_API void JS_SetReservedSlot(JSObject* obj, uint32_t index,
                                      const Value& value) {
  obj->as<NativeObject>().setReservedSlot(index, value);
}

JS_PUBLIC_API void JS_InitReservedSlot(JSObject* obj, uint32_t index, void* ptr,
                                       size_t nbytes, JS::MemoryUse use) {
  InitReservedSlot(&obj->as<NativeObject>(), index, ptr, nbytes,
                   js::MemoryUse(use));
}

JS_PUBLIC_API void JS_InitPrivate(JSObject* obj, void* data, size_t nbytes,
                                  JS::MemoryUse use) {
  InitObjectPrivate(&obj->as<NativeObject>(), data, nbytes, js::MemoryUse(use));
}

JS_PUBLIC_API bool JS::IsMapObject(JSContext* cx, JS::HandleObject obj,
                                   bool* isMap) {
  return IsGivenTypeObject(cx, obj, ESClass::Map, isMap);
}

JS_PUBLIC_API bool JS::IsSetObject(JSContext* cx, JS::HandleObject obj,
                                   bool* isSet) {
  return IsGivenTypeObject(cx, obj, ESClass::Set, isSet);
}

JS_PUBLIC_API void JS_HoldPrincipals(JSPrincipals* principals) {
  ++principals->refcount;
}

JS_PUBLIC_API void JS_DropPrincipals(JSContext* cx, JSPrincipals* principals) {
  int rc = --principals->refcount;
  if (rc == 0) {
    JS::AutoSuppressGCAnalysis nogc;
    cx->runtime()->destroyPrincipals(principals);
  }
}

JS_PUBLIC_API void JS_SetSecurityCallbacks(JSContext* cx,
                                           const JSSecurityCallbacks* scb) {
  MOZ_ASSERT(scb != &NullSecurityCallbacks);
  cx->runtime()->securityCallbacks = scb ? scb : &NullSecurityCallbacks;
}

JS_PUBLIC_API const JSSecurityCallbacks* JS_GetSecurityCallbacks(
    JSContext* cx) {
  return (cx->runtime()->securityCallbacks != &NullSecurityCallbacks)
             ? cx->runtime()->securityCallbacks.ref()
             : nullptr;
}

JS_PUBLIC_API void JS_SetTrustedPrincipals(JSContext* cx, JSPrincipals* prin) {
  cx->runtime()->setTrustedPrincipals(prin);
}

extern JS_PUBLIC_API void JS_InitDestroyPrincipalsCallback(
    JSContext* cx, JSDestroyPrincipalsOp destroyPrincipals) {
  MOZ_ASSERT(destroyPrincipals);
  MOZ_ASSERT(!cx->runtime()->destroyPrincipals);
  cx->runtime()->destroyPrincipals = destroyPrincipals;
}

extern JS_PUBLIC_API void JS_InitReadPrincipalsCallback(
    JSContext* cx, JSReadPrincipalsOp read) {
  MOZ_ASSERT(read);
  MOZ_ASSERT(!cx->runtime()->readPrincipals);
  cx->runtime()->readPrincipals = read;
}

JS_PUBLIC_API JSFunction* JS_NewFunction(JSContext* cx, JSNative native,
                                         unsigned nargs, unsigned flags,
                                         const char* name) {
  MOZ_ASSERT(!cx->zone()->isAtomsZone());

  AssertHeapIsIdle();
  CHECK_THREAD(cx);

  RootedAtom atom(cx);
  if (name) {
    atom = Atomize(cx, name, strlen(name));
    if (!atom) {
      return nullptr;
    }
  }

  return (flags & JSFUN_CONSTRUCTOR)
             ? NewNativeConstructor(cx, native, nargs, atom)
             : NewNativeFunction(cx, native, nargs, atom);
}

JS_PUBLIC_API JSFunction* JS::GetSelfHostedFunction(JSContext* cx,
                                                    const char* selfHostedName,
                                                    HandleId id,
                                                    unsigned nargs) {
  MOZ_ASSERT(!cx->zone()->isAtomsZone());
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(id);

  RootedAtom name(cx, IdToFunctionName(cx, id));
  if (!name) {
    return nullptr;
  }

  JSAtom* shAtom = Atomize(cx, selfHostedName, strlen(selfHostedName));
  if (!shAtom) {
    return nullptr;
  }
  RootedPropertyName shName(cx, shAtom->asPropertyName());
  RootedValue funVal(cx);
  if (!GlobalObject::getSelfHostedFunction(cx, cx->global(), shName, name,
                                           nargs, &funVal)) {
    return nullptr;
  }
  return &funVal.toObject().as<JSFunction>();
}

JS_PUBLIC_API JSFunction* JS::NewFunctionFromSpec(JSContext* cx,
                                                  const JSFunctionSpec* fs,
                                                  HandleId id) {
  cx->check(id);

#ifdef DEBUG
  if (fs->name.isSymbol()) {
    MOZ_ASSERT(SYMBOL_TO_JSID(cx->wellKnownSymbols().get(fs->name.symbol())) ==
               id);
  } else {
    MOZ_ASSERT(JSID_IS_STRING(id) &&
               StringEqualsAscii(JSID_TO_LINEAR_STRING(id), fs->name.string()));
  }
#endif

  // Delay cloning self-hosted functions until they are called. This is
  // achieved by passing DefineFunction a nullptr JSNative which produces an
  // interpreted JSFunction where !hasScript. Interpreted call paths then
  // call InitializeLazyFunctionScript if !hasScript.
  if (fs->selfHostedName) {
    MOZ_ASSERT(!fs->call.op);
    MOZ_ASSERT(!fs->call.info);

    JSAtom* shAtom =
        Atomize(cx, fs->selfHostedName, strlen(fs->selfHostedName));
    if (!shAtom) {
      return nullptr;
    }
    RootedPropertyName shName(cx, shAtom->asPropertyName());
    RootedAtom name(cx, IdToFunctionName(cx, id));
    if (!name) {
      return nullptr;
    }
    RootedValue funVal(cx);
    if (!GlobalObject::getSelfHostedFunction(cx, cx->global(), shName, name,
                                             fs->nargs, &funVal)) {
      return nullptr;
    }
    return &funVal.toObject().as<JSFunction>();
  }

  RootedAtom atom(cx, IdToFunctionName(cx, id));
  if (!atom) {
    return nullptr;
  }

  MOZ_ASSERT(fs->call.op);

  JSFunction* fun;
  if (fs->flags & JSFUN_CONSTRUCTOR) {
    fun = NewNativeConstructor(cx, fs->call.op, fs->nargs, atom);
  } else {
    fun = NewNativeFunction(cx, fs->call.op, fs->nargs, atom);
  }
  if (!fun) {
    return nullptr;
  }

  if (fs->call.info) {
    fun->setJitInfo(fs->call.info);
  }
  return fun;
}

JS_PUBLIC_API JSFunction* JS::NewFunctionFromSpec(JSContext* cx,
                                                  const JSFunctionSpec* fs) {
  RootedId id(cx);
  if (!PropertySpecNameToId(cx, fs->name, &id)) {
    return nullptr;
  }

  return NewFunctionFromSpec(cx, fs, id);
}

JS_PUBLIC_API JSObject* JS_GetFunctionObject(JSFunction* fun) { return fun; }

JS_PUBLIC_API JSString* JS_GetFunctionId(JSFunction* fun) {
  return fun->explicitName();
}

JS_PUBLIC_API JSString* JS_GetFunctionDisplayId(JSFunction* fun) {
  return fun->displayAtom();
}

JS_PUBLIC_API uint16_t JS_GetFunctionArity(JSFunction* fun) {
  return fun->nargs();
}

JS_PUBLIC_API bool JS_GetFunctionLength(JSContext* cx, HandleFunction fun,
                                        uint16_t* length) {
  cx->check(fun);
  return JSFunction::getLength(cx, fun, length);
}

JS_PUBLIC_API bool JS_ObjectIsFunction(JSObject* obj) {
  return obj->is<JSFunction>();
}

JS_PUBLIC_API bool JS_IsNativeFunction(JSObject* funobj, JSNative call) {
  if (!funobj->is<JSFunction>()) {
    return false;
  }
  JSFunction* fun = &funobj->as<JSFunction>();
  return fun->isNativeFun() && fun->native() == call;
}

extern JS_PUBLIC_API bool JS_IsConstructor(JSFunction* fun) {
  return fun->isConstructor();
}

JS_PUBLIC_API bool JS_DefineFunctions(JSContext* cx, HandleObject obj,
                                      const JSFunctionSpec* fs) {
  MOZ_ASSERT(!cx->zone()->isAtomsZone());
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(obj);

  return DefineFunctions(cx, obj, fs, NotIntrinsic);
}

JS_PUBLIC_API JSFunction* JS_DefineFunction(JSContext* cx, HandleObject obj,
                                            const char* name, JSNative call,
                                            unsigned nargs, unsigned attrs) {
  MOZ_ASSERT(!cx->zone()->isAtomsZone());
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(obj);
  JSAtom* atom = Atomize(cx, name, strlen(name));
  if (!atom) {
    return nullptr;
  }
  Rooted<jsid> id(cx, AtomToId(atom));
  return DefineFunction(cx, obj, id, call, nargs, attrs);
}

JS_PUBLIC_API JSFunction* JS_DefineUCFunction(JSContext* cx, HandleObject obj,
                                              const char16_t* name,
                                              size_t namelen, JSNative call,
                                              unsigned nargs, unsigned attrs) {
  MOZ_ASSERT(!cx->zone()->isAtomsZone());
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(obj);
  JSAtom* atom = AtomizeChars(cx, name, AUTO_NAMELEN(name, namelen));
  if (!atom) {
    return nullptr;
  }
  Rooted<jsid> id(cx, AtomToId(atom));
  return DefineFunction(cx, obj, id, call, nargs, attrs);
}

extern JS_PUBLIC_API JSFunction* JS_DefineFunctionById(
    JSContext* cx, HandleObject obj, HandleId id, JSNative call, unsigned nargs,
    unsigned attrs) {
  MOZ_ASSERT(!cx->zone()->isAtomsZone());
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(obj, id);
  return DefineFunction(cx, obj, id, call, nargs, attrs);
}

void JS::TransitiveCompileOptions::copyPODTransitiveOptions(
    const TransitiveCompileOptions& rhs) {
  mutedErrors_ = rhs.mutedErrors_;
  forceFullParse_ = rhs.forceFullParse_;
  forceStrictMode_ = rhs.forceStrictMode_;
  skipFilenameValidation_ = rhs.skipFilenameValidation_;
  sourcePragmas_ = rhs.sourcePragmas_;
  selfHostingMode = rhs.selfHostingMode;
  asmJSOption = rhs.asmJSOption;
  throwOnAsmJSValidationFailureOption = rhs.throwOnAsmJSValidationFailureOption;
  forceAsync = rhs.forceAsync;
  discardSource = rhs.discardSource;
  sourceIsLazy = rhs.sourceIsLazy;
  introductionType = rhs.introductionType;
  introductionLineno = rhs.introductionLineno;
  introductionOffset = rhs.introductionOffset;
  hasIntroductionInfo = rhs.hasIntroductionInfo;
  hideScriptFromDebugger = rhs.hideScriptFromDebugger;
  deferDebugMetadata = rhs.deferDebugMetadata;
  nonSyntacticScope = rhs.nonSyntacticScope;
  privateClassFields = rhs.privateClassFields;
  privateClassMethods = rhs.privateClassMethods;
  topLevelAwait = rhs.topLevelAwait;
  classStaticBlocks = rhs.classStaticBlocks;
  useStencilXDR = rhs.useStencilXDR;
  useOffThreadParseGlobal = rhs.useOffThreadParseGlobal;
  useFdlibmForSinCosTan = rhs.useFdlibmForSinCosTan;
};

void JS::ReadOnlyCompileOptions::copyPODNonTransitiveOptions(
    const ReadOnlyCompileOptions& rhs) {
  lineno = rhs.lineno;
  column = rhs.column;
  scriptSourceOffset = rhs.scriptSourceOffset;
  isRunOnce = rhs.isRunOnce;
  noScriptRval = rhs.noScriptRval;
}

JS::OwningCompileOptions::OwningCompileOptions(JSContext* cx)
    : ReadOnlyCompileOptions() {}

void JS::OwningCompileOptions::release() {
  // OwningCompileOptions always owns these, so these casts are okay.
  js_free(const_cast<char*>(filename_));
  js_free(const_cast<char16_t*>(sourceMapURL_));
  js_free(const_cast<char*>(introducerFilename_));

  filename_ = nullptr;
  sourceMapURL_ = nullptr;
  introducerFilename_ = nullptr;
}

JS::OwningCompileOptions::~OwningCompileOptions() { release(); }

size_t JS::OwningCompileOptions::sizeOfExcludingThis(
    mozilla::MallocSizeOf mallocSizeOf) const {
  return mallocSizeOf(filename_) + mallocSizeOf(sourceMapURL_) +
         mallocSizeOf(introducerFilename_);
}

bool JS::OwningCompileOptions::copy(JSContext* cx,
                                    const ReadOnlyCompileOptions& rhs) {
  // Release existing string allocations.
  release();

  copyPODNonTransitiveOptions(rhs);
  copyPODTransitiveOptions(rhs);

  if (rhs.filename()) {
    filename_ = DuplicateString(cx, rhs.filename()).release();
    if (!filename_) {
      return false;
    }
  }

  if (rhs.sourceMapURL()) {
    sourceMapURL_ = DuplicateString(cx, rhs.sourceMapURL()).release();
    if (!sourceMapURL_) {
      return false;
    }
  }

  if (rhs.introducerFilename()) {
    introducerFilename_ =
        DuplicateString(cx, rhs.introducerFilename()).release();
    if (!introducerFilename_) {
      return false;
    }
  }

  return true;
}

JS::CompileOptions::CompileOptions(JSContext* cx) : ReadOnlyCompileOptions() {
  if (!js::IsAsmJSCompilationAvailable(cx)) {
    // Distinguishing the cases is just for error reporting.
    asmJSOption = !cx->options().asmJS()
                      ? AsmJSOption::DisabledByAsmJSPref
                      : AsmJSOption::DisabledByNoWasmCompiler;
  } else if (cx->realm() && cx->realm()->debuggerObservesAsmJS()) {
    asmJSOption = AsmJSOption::DisabledByDebugger;
  } else {
    asmJSOption = AsmJSOption::Enabled;
  }
  throwOnAsmJSValidationFailureOption =
      cx->options().throwOnAsmJSValidationFailure();
  privateClassFields = cx->options().privateClassFields();
  privateClassMethods = cx->options().privateClassMethods();
  topLevelAwait = cx->options().topLevelAwait();

  classStaticBlocks = cx->options().classStaticBlocks();

  useStencilXDR = !UseOffThreadParseGlobal();
  useOffThreadParseGlobal = UseOffThreadParseGlobal();

  useFdlibmForSinCosTan = math_use_fdlibm_for_sin_cos_tan();

  sourcePragmas_ = cx->options().sourcePragmas();

  // Certain modes of operation force strict-mode in general.
  forceStrictMode_ = cx->options().strictMode();

  // Certain modes of operation disallow syntax parsing in general.
  forceFullParse_ = coverage::IsLCovEnabled();

  // Note: If we parse outside of a specific realm, we do not inherit any realm
  // behaviours. These can still be set manually on the options though.
  if (cx->realm()) {
    discardSource = cx->realm()->behaviors().discardSource();
  }
}

CompileOptions& CompileOptions::setIntroductionInfoToCaller(
    JSContext* cx, const char* introductionType,
    MutableHandle<JSScript*> introductionScript) {
  RootedScript maybeScript(cx);
  const char* filename;
  unsigned lineno;
  uint32_t pcOffset;
  bool mutedErrors;
  DescribeScriptedCallerForCompilation(cx, &maybeScript, &filename, &lineno,
                                       &pcOffset, &mutedErrors);
  if (filename) {
    introductionScript.set(maybeScript);
    return setIntroductionInfo(filename, introductionType, lineno, pcOffset);
  }
  return setIntroductionType(introductionType);
}

JS_PUBLIC_API JSObject* JS_GetGlobalFromScript(JSScript* script) {
  return &script->global();
}

JS_PUBLIC_API const char* JS_GetScriptFilename(JSScript* script) {
  // This is called from ThreadStackHelper which can be called from another
  // thread or inside a signal hander, so we need to be careful in case a
  // copmacting GC is currently moving things around.
  return script->maybeForwardedFilename();
}

JS_PUBLIC_API unsigned JS_GetScriptBaseLineNumber(JSContext* cx,
                                                  JSScript* script) {
  return script->lineno();
}

JS_PUBLIC_API JSScript* JS_GetFunctionScript(JSContext* cx,
                                             HandleFunction fun) {
  if (fun->isNativeFun()) {
    return nullptr;
  }

  if (fun->hasBytecode()) {
    return fun->nonLazyScript();
  }

  AutoRealm ar(cx, fun);
  JSScript* script = JSFunction::getOrCreateScript(cx, fun);
  if (!script) {
    MOZ_CRASH();
  }
  return script;
}

JS_PUBLIC_API JSString* JS_DecompileScript(JSContext* cx, HandleScript script) {
  MOZ_ASSERT(!cx->zone()->isAtomsZone());

  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  RootedFunction fun(cx, script->function());
  if (fun) {
    return JS_DecompileFunction(cx, fun);
  }
  bool haveSource;
  if (!ScriptSource::loadSource(cx, script->scriptSource(), &haveSource)) {
    return nullptr;
  }
  return haveSource ? JSScript::sourceData(cx, script)
                    : NewStringCopyZ<CanGC>(cx, "[no source]");
}

JS_PUBLIC_API JSString* JS_DecompileFunction(JSContext* cx,
                                             HandleFunction fun) {
  MOZ_ASSERT(!cx->zone()->isAtomsZone());
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(fun);
  return FunctionToString(cx, fun, /* isToSource = */ false);
}

JS_PUBLIC_API void JS::SetScriptPrivate(JSScript* script,
                                        const JS::Value& value) {
  JSRuntime* rt = script->zone()->runtimeFromMainThread();
  script->sourceObject()->setPrivate(rt, value);
}

JS_PUBLIC_API JS::Value JS::GetScriptPrivate(JSScript* script) {
  return script->sourceObject()->canonicalPrivate();
}

JS_PUBLIC_API JS::Value JS::GetScriptedCallerPrivate(JSContext* cx) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);

  NonBuiltinFrameIter iter(cx, cx->realm()->principals());
  if (iter.done() || !iter.hasScript()) {
    return UndefinedValue();
  }

  return iter.script()->sourceObject()->canonicalPrivate();
}

JS_PUBLIC_API void JS::SetScriptPrivateReferenceHooks(
    JSRuntime* rt, JS::ScriptPrivateReferenceHook addRefHook,
    JS::ScriptPrivateReferenceHook releaseHook) {
  AssertHeapIsIdle();
  rt->scriptPrivateAddRefHook = addRefHook;
  rt->scriptPrivateReleaseHook = releaseHook;
}

JS_PUBLIC_API void JS::SetWaitCallback(JSRuntime* rt,
                                       BeforeWaitCallback beforeWait,
                                       AfterWaitCallback afterWait,
                                       size_t requiredMemory) {
  MOZ_RELEASE_ASSERT(requiredMemory <= WAIT_CALLBACK_CLIENT_MAXMEM);
  MOZ_RELEASE_ASSERT((beforeWait == nullptr) == (afterWait == nullptr));
  rt->beforeWaitCallback = beforeWait;
  rt->afterWaitCallback = afterWait;
}

JS_PUBLIC_API bool JS_CheckForInterrupt(JSContext* cx) {
  return js::CheckForInterrupt(cx);
}

JS_PUBLIC_API bool JS_AddInterruptCallback(JSContext* cx,
                                           JSInterruptCallback callback) {
  return cx->interruptCallbacks().append(callback);
}

JS_PUBLIC_API bool JS_DisableInterruptCallback(JSContext* cx) {
  bool result = cx->interruptCallbackDisabled;
  cx->interruptCallbackDisabled = true;
  return result;
}

JS_PUBLIC_API void JS_ResetInterruptCallback(JSContext* cx, bool enable) {
  cx->interruptCallbackDisabled = enable;
}

/************************************************************************/

/*
 * Promises.
 */
JS_PUBLIC_API void JS::SetJobQueue(JSContext* cx, JobQueue* queue) {
  cx->jobQueue = queue;
}

extern JS_PUBLIC_API void JS::SetPromiseRejectionTrackerCallback(
    JSContext* cx, PromiseRejectionTrackerCallback callback,
    void* data /* = nullptr */) {
  cx->promiseRejectionTrackerCallback = callback;
  cx->promiseRejectionTrackerCallbackData = data;
}

extern JS_PUBLIC_API void JS::JobQueueIsEmpty(JSContext* cx) {
  cx->canSkipEnqueuingJobs = true;
}

extern JS_PUBLIC_API void JS::JobQueueMayNotBeEmpty(JSContext* cx) {
  cx->canSkipEnqueuingJobs = false;
}

JS_PUBLIC_API JSObject* JS::NewPromiseObject(JSContext* cx,
                                             HandleObject executor) {
  MOZ_ASSERT(!cx->zone()->isAtomsZone());
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(executor);

  if (!executor) {
    return PromiseObject::createSkippingExecutor(cx);
  }

  MOZ_ASSERT(IsCallable(executor));
  return PromiseObject::create(cx, executor);
}

JS_PUBLIC_API bool JS::IsPromiseObject(JS::HandleObject obj) {
  return obj->is<PromiseObject>();
}

JS_PUBLIC_API JSObject* JS::GetPromiseConstructor(JSContext* cx) {
  CHECK_THREAD(cx);
  Rooted<GlobalObject*> global(cx, cx->global());
  return GlobalObject::getOrCreatePromiseConstructor(cx, global);
}

JS_PUBLIC_API JSObject* JS::GetPromisePrototype(JSContext* cx) {
  CHECK_THREAD(cx);
  Rooted<GlobalObject*> global(cx, cx->global());
  return GlobalObject::getOrCreatePromisePrototype(cx, global);
}

JS_PUBLIC_API JS::PromiseState JS::GetPromiseState(
    JS::HandleObject promiseObj_) {
  PromiseObject* promiseObj = promiseObj_->maybeUnwrapIf<PromiseObject>();
  if (!promiseObj) {
    return JS::PromiseState::Pending;
  }

  return promiseObj->state();
}

JS_PUBLIC_API uint64_t JS::GetPromiseID(JS::HandleObject promise) {
  return promise->as<PromiseObject>().getID();
}

JS_PUBLIC_API JS::Value JS::GetPromiseResult(JS::HandleObject promiseObj) {
  PromiseObject* promise = &promiseObj->as<PromiseObject>();
  MOZ_ASSERT(promise->state() != JS::PromiseState::Pending);
  return promise->state() == JS::PromiseState::Fulfilled ? promise->value()
                                                         : promise->reason();
}

JS_PUBLIC_API bool JS::GetPromiseIsHandled(JS::HandleObject promiseObj) {
  PromiseObject* promise = &promiseObj->as<PromiseObject>();
  return !promise->isUnhandled();
}

JS_PUBLIC_API void JS::SetSettledPromiseIsHandled(JSContext* cx,
                                                  JS::HandleObject promise) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(promise);

  mozilla::Maybe<AutoRealm> ar;
  Rooted<PromiseObject*> promiseObj(cx);
  if (IsWrapper(promise)) {
    promiseObj = promise->maybeUnwrapAs<PromiseObject>();
    if (!promiseObj) {
      ReportAccessDenied(cx);
      return;
    }
    ar.emplace(cx, promiseObj);
  } else {
    promiseObj = promise.as<PromiseObject>();
  }

  js::SetSettledPromiseIsHandled(cx, promiseObj);
}

JS_PUBLIC_API JSObject* JS::GetPromiseAllocationSite(JS::HandleObject promise) {
  return promise->as<PromiseObject>().allocationSite();
}

JS_PUBLIC_API JSObject* JS::GetPromiseResolutionSite(JS::HandleObject promise) {
  return promise->as<PromiseObject>().resolutionSite();
}

#ifdef DEBUG
JS_PUBLIC_API void JS::DumpPromiseAllocationSite(JSContext* cx,
                                                 JS::HandleObject promise) {
  RootedObject stack(cx, promise->as<PromiseObject>().allocationSite());
  JSPrincipals* principals = cx->realm()->principals();
  UniqueChars stackStr = BuildUTF8StackString(cx, principals, stack);
  if (stackStr) {
    fputs(stackStr.get(), stderr);
  }
}

JS_PUBLIC_API void JS::DumpPromiseResolutionSite(JSContext* cx,
                                                 JS::HandleObject promise) {
  RootedObject stack(cx, promise->as<PromiseObject>().resolutionSite());
  JSPrincipals* principals = cx->realm()->principals();
  UniqueChars stackStr = BuildUTF8StackString(cx, principals, stack);
  if (stackStr) {
    fputs(stackStr.get(), stderr);
  }
}
#endif

JS_PUBLIC_API JSObject* JS::CallOriginalPromiseResolve(
    JSContext* cx, JS::HandleValue resolutionValue) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(resolutionValue);

  RootedObject promise(cx,
                       PromiseObject::unforgeableResolve(cx, resolutionValue));
  MOZ_ASSERT_IF(promise, promise->canUnwrapAs<PromiseObject>());
  return promise;
}

JS_PUBLIC_API JSObject* JS::CallOriginalPromiseReject(
    JSContext* cx, JS::HandleValue rejectionValue) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(rejectionValue);

  RootedObject promise(cx,
                       PromiseObject::unforgeableReject(cx, rejectionValue));
  MOZ_ASSERT_IF(promise, promise->canUnwrapAs<PromiseObject>());
  return promise;
}

static bool ResolveOrRejectPromise(JSContext* cx, JS::HandleObject promiseObj,
                                   JS::HandleValue resultOrReason_,
                                   bool reject) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(promiseObj, resultOrReason_);

  mozilla::Maybe<AutoRealm> ar;
  Rooted<PromiseObject*> promise(cx);
  RootedValue resultOrReason(cx, resultOrReason_);
  if (IsWrapper(promiseObj)) {
    promise = promiseObj->maybeUnwrapAs<PromiseObject>();
    if (!promise) {
      ReportAccessDenied(cx);
      return false;
    }
    ar.emplace(cx, promise);
    if (!cx->compartment()->wrap(cx, &resultOrReason)) {
      return false;
    }
  } else {
    promise = promiseObj.as<PromiseObject>();
  }

  return reject ? PromiseObject::reject(cx, promise, resultOrReason)
                : PromiseObject::resolve(cx, promise, resultOrReason);
}

JS_PUBLIC_API bool JS::ResolvePromise(JSContext* cx,
                                      JS::HandleObject promiseObj,
                                      JS::HandleValue resolutionValue) {
  return ResolveOrRejectPromise(cx, promiseObj, resolutionValue, false);
}

JS_PUBLIC_API bool JS::RejectPromise(JSContext* cx, JS::HandleObject promiseObj,
                                     JS::HandleValue rejectionValue) {
  return ResolveOrRejectPromise(cx, promiseObj, rejectionValue, true);
}

JS_PUBLIC_API JSObject* JS::CallOriginalPromiseThen(
    JSContext* cx, JS::HandleObject promiseObj, JS::HandleObject onFulfilled,
    JS::HandleObject onRejected) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(promiseObj, onFulfilled, onRejected);

  MOZ_ASSERT_IF(onFulfilled, IsCallable(onFulfilled));
  MOZ_ASSERT_IF(onRejected, IsCallable(onRejected));

  return OriginalPromiseThen(cx, promiseObj, onFulfilled, onRejected);
}

[[nodiscard]] static bool ReactToPromise(JSContext* cx,
                                         JS::Handle<JSObject*> promiseObj,
                                         JS::Handle<JSObject*> onFulfilled,
                                         JS::Handle<JSObject*> onRejected,
                                         UnhandledRejectionBehavior behavior) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(promiseObj, onFulfilled, onRejected);

  MOZ_ASSERT_IF(onFulfilled, IsCallable(onFulfilled));
  MOZ_ASSERT_IF(onRejected, IsCallable(onRejected));

  Rooted<PromiseObject*> unwrappedPromise(cx);
  {
    RootedValue promiseVal(cx, ObjectValue(*promiseObj));
    unwrappedPromise = UnwrapAndTypeCheckValue<PromiseObject>(
        cx, promiseVal, [cx, promiseObj] {
          JS_ReportErrorNumberLatin1(cx, GetErrorMessage, nullptr,
                                     JSMSG_INCOMPATIBLE_PROTO, "Promise",
                                     "then", promiseObj->getClass()->name);
        });
    if (!unwrappedPromise) {
      return false;
    }
  }

  return ReactToUnwrappedPromise(cx, unwrappedPromise, onFulfilled, onRejected,
                                 behavior);
}

JS_PUBLIC_API bool JS::AddPromiseReactions(JSContext* cx,
                                           JS::HandleObject promiseObj,
                                           JS::HandleObject onFulfilled,
                                           JS::HandleObject onRejected) {
  return ReactToPromise(cx, promiseObj, onFulfilled, onRejected,
                        UnhandledRejectionBehavior::Report);
}

JS_PUBLIC_API bool JS::AddPromiseReactionsIgnoringUnhandledRejection(
    JSContext* cx, JS::HandleObject promiseObj, JS::HandleObject onFulfilled,
    JS::HandleObject onRejected) {
  return ReactToPromise(cx, promiseObj, onFulfilled, onRejected,
                        UnhandledRejectionBehavior::Ignore);
}

JS_PUBLIC_API JS::PromiseUserInputEventHandlingState
JS::GetPromiseUserInputEventHandlingState(JS::HandleObject promiseObj_) {
  PromiseObject* promise = promiseObj_->maybeUnwrapIf<PromiseObject>();
  if (!promise) {
    return JS::PromiseUserInputEventHandlingState::DontCare;
  }

  if (!promise->requiresUserInteractionHandling()) {
    return JS::PromiseUserInputEventHandlingState::DontCare;
  }
  if (promise->hadUserInteractionUponCreation()) {
    return JS::PromiseUserInputEventHandlingState::HadUserInteractionAtCreation;
  }
  return JS::PromiseUserInputEventHandlingState::
      DidntHaveUserInteractionAtCreation;
}

JS_PUBLIC_API bool JS::SetPromiseUserInputEventHandlingState(
    JS::HandleObject promiseObj_,
    JS::PromiseUserInputEventHandlingState state) {
  PromiseObject* promise = promiseObj_->maybeUnwrapIf<PromiseObject>();
  if (!promise) {
    return false;
  }

  switch (state) {
    case JS::PromiseUserInputEventHandlingState::DontCare:
      promise->setRequiresUserInteractionHandling(false);
      break;
    case JS::PromiseUserInputEventHandlingState::HadUserInteractionAtCreation:
      promise->setRequiresUserInteractionHandling(true);
      promise->setHadUserInteractionUponCreation(true);
      break;
    case JS::PromiseUserInputEventHandlingState::
        DidntHaveUserInteractionAtCreation:
      promise->setRequiresUserInteractionHandling(true);
      promise->setHadUserInteractionUponCreation(false);
      break;
    default:
      MOZ_ASSERT_UNREACHABLE(
          "Invalid PromiseUserInputEventHandlingState enum value");
      return false;
  }
  return true;
}

/**
 * Unforgeable version of Promise.all for internal use.
 *
 * Takes a dense array of Promise objects and returns a promise that's
 * resolved with an array of resolution values when all those promises ahve
 * been resolved, or rejected with the rejection value of the first rejected
 * promise.
 *
 * Asserts that the array is dense and all entries are Promise objects.
 */
JS_PUBLIC_API JSObject* JS::GetWaitForAllPromise(
    JSContext* cx, JS::HandleObjectVector promises) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);

  return js::GetWaitForAllPromise(cx, promises);
}

JS_PUBLIC_API void JS::InitDispatchToEventLoop(
    JSContext* cx, JS::DispatchToEventLoopCallback callback, void* closure) {
  cx->runtime()->offThreadPromiseState.ref().init(callback, closure);
}

JS_PUBLIC_API void JS::ShutdownAsyncTasks(JSContext* cx) {
  cx->runtime()->offThreadPromiseState.ref().shutdown(cx);
}

JS_PUBLIC_API void JS::InitConsumeStreamCallback(
    JSContext* cx, ConsumeStreamCallback consume,
    ReportStreamErrorCallback report) {
  cx->runtime()->consumeStreamCallback = consume;
  cx->runtime()->reportStreamErrorCallback = report;
}

JS_PUBLIC_API void JS_RequestInterruptCallback(JSContext* cx) {
  cx->requestInterrupt(InterruptReason::CallbackUrgent);
}

JS_PUBLIC_API void JS_RequestInterruptCallbackCanWait(JSContext* cx) {
  cx->requestInterrupt(InterruptReason::CallbackCanWait);
}

JS::AutoSetAsyncStackForNewCalls::AutoSetAsyncStackForNewCalls(
    JSContext* cx, HandleObject stack, const char* asyncCause,
    JS::AutoSetAsyncStackForNewCalls::AsyncCallKind kind)
    : cx(cx),
      oldAsyncStack(cx, cx->asyncStackForNewActivations()),
      oldAsyncCause(cx->asyncCauseForNewActivations),
      oldAsyncCallIsExplicit(cx->asyncCallIsExplicit) {
  CHECK_THREAD(cx);

  // The option determines whether we actually use the new values at this
  // point. It will not affect restoring the previous values when the object
  // is destroyed, so if the option changes it won't cause consistency issues.
  if (!cx->options().asyncStack()) {
    return;
  }

  SavedFrame* asyncStack = &stack->as<SavedFrame>();

  cx->asyncStackForNewActivations() = asyncStack;
  cx->asyncCauseForNewActivations = asyncCause;
  cx->asyncCallIsExplicit = kind == AsyncCallKind::EXPLICIT;
}

JS::AutoSetAsyncStackForNewCalls::~AutoSetAsyncStackForNewCalls() {
  cx->asyncCauseForNewActivations = oldAsyncCause;
  cx->asyncStackForNewActivations() =
      oldAsyncStack ? &oldAsyncStack->as<SavedFrame>() : nullptr;
  cx->asyncCallIsExplicit = oldAsyncCallIsExplicit;
}

/************************************************************************/
JS_PUBLIC_API JSString* JS_NewStringCopyN(JSContext* cx, const char* s,
                                          size_t n) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  return NewStringCopyN<CanGC>(cx, s, n);
}

JS_PUBLIC_API JSString* JS_NewStringCopyZ(JSContext* cx, const char* s) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  if (!s) {
    return cx->runtime()->emptyString;
  }
  return NewStringCopyZ<CanGC>(cx, s);
}

JS_PUBLIC_API JSString* JS_NewStringCopyUTF8Z(JSContext* cx,
                                              const JS::ConstUTF8CharsZ s) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  return NewStringCopyUTF8Z<CanGC>(cx, s);
}

JS_PUBLIC_API JSString* JS_NewStringCopyUTF8N(JSContext* cx,
                                              const JS::UTF8Chars s) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  return NewStringCopyUTF8N<CanGC>(cx, s);
}

JS_PUBLIC_API bool JS_StringHasBeenPinned(JSContext* cx, JSString* str) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);

  if (!str->isAtom()) {
    return false;
  }

  return AtomIsPinned(cx, &str->asAtom());
}

JS_PUBLIC_API JSString* JS_AtomizeAndPinJSString(JSContext* cx,
                                                 HandleString str) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  JSAtom* atom = AtomizeString(cx, str, PinAtom);
  MOZ_ASSERT_IF(atom, JS_StringHasBeenPinned(cx, atom));
  return atom;
}

JS_PUBLIC_API JSString* JS_AtomizeString(JSContext* cx, const char* s) {
  return JS_AtomizeStringN(cx, s, strlen(s));
}

JS_PUBLIC_API JSString* JS_AtomizeStringN(JSContext* cx, const char* s,
                                          size_t length) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  return Atomize(cx, s, length, DoNotPinAtom);
}

JS_PUBLIC_API JSString* JS_AtomizeAndPinString(JSContext* cx, const char* s) {
  return JS_AtomizeAndPinStringN(cx, s, strlen(s));
}

JS_PUBLIC_API JSString* JS_AtomizeAndPinStringN(JSContext* cx, const char* s,
                                                size_t length) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  JSAtom* atom = Atomize(cx, s, length, PinAtom);
  MOZ_ASSERT_IF(atom, JS_StringHasBeenPinned(cx, atom));
  return atom;
}

JS_PUBLIC_API JSString* JS_NewLatin1String(
    JSContext* cx, js::UniquePtr<JS::Latin1Char[], JS::FreePolicy> chars,
    size_t length) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  return NewString<CanGC>(cx, std::move(chars), length);
}

JS_PUBLIC_API JSString* JS_NewUCString(JSContext* cx,
                                       JS::UniqueTwoByteChars chars,
                                       size_t length) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  return NewString<CanGC>(cx, std::move(chars), length);
}

JS_PUBLIC_API JSString* JS_NewUCStringDontDeflate(JSContext* cx,
                                                  JS::UniqueTwoByteChars chars,
                                                  size_t length) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  return NewStringDontDeflate<CanGC>(cx, std::move(chars), length);
}

JS_PUBLIC_API JSString* JS_NewUCStringCopyN(JSContext* cx, const char16_t* s,
                                            size_t n) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  if (!n) {
    return cx->names().empty;
  }
  return NewStringCopyN<CanGC>(cx, s, n);
}

JS_PUBLIC_API JSString* JS_NewUCStringCopyZ(JSContext* cx, const char16_t* s) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  if (!s) {
    return cx->runtime()->emptyString;
  }
  return NewStringCopyZ<CanGC>(cx, s);
}

JS_PUBLIC_API JSString* JS_AtomizeUCString(JSContext* cx, const char16_t* s) {
  return JS_AtomizeUCStringN(cx, s, js_strlen(s));
}

JS_PUBLIC_API JSString* JS_AtomizeUCStringN(JSContext* cx, const char16_t* s,
                                            size_t length) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  return AtomizeChars(cx, s, length, DoNotPinAtom);
}

JS_PUBLIC_API JSString* JS_AtomizeAndPinUCStringN(JSContext* cx,
                                                  const char16_t* s,
                                                  size_t length) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  JSAtom* atom = AtomizeChars(cx, s, length, PinAtom);
  MOZ_ASSERT_IF(atom, JS_StringHasBeenPinned(cx, atom));
  return atom;
}

JS_PUBLIC_API JSString* JS_AtomizeAndPinUCString(JSContext* cx,
                                                 const char16_t* s) {
  return JS_AtomizeAndPinUCStringN(cx, s, js_strlen(s));
}

JS_PUBLIC_API size_t JS_GetStringLength(JSString* str) { return str->length(); }

JS_PUBLIC_API bool JS_StringIsLinear(JSString* str) { return str->isLinear(); }

JS_PUBLIC_API bool JS_DeprecatedStringHasLatin1Chars(JSString* str) {
  return str->hasLatin1Chars();
}

JS_PUBLIC_API const JS::Latin1Char* JS_GetLatin1StringCharsAndLength(
    JSContext* cx, const JS::AutoRequireNoGC& nogc, JSString* str,
    size_t* plength) {
  MOZ_ASSERT(plength);
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(str);
  JSLinearString* linear = str->ensureLinear(cx);
  if (!linear) {
    return nullptr;
  }
  *plength = linear->length();
  return linear->latin1Chars(nogc);
}

JS_PUBLIC_API const char16_t* JS_GetTwoByteStringCharsAndLength(
    JSContext* cx, const JS::AutoRequireNoGC& nogc, JSString* str,
    size_t* plength) {
  MOZ_ASSERT(plength);
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(str);
  JSLinearString* linear = str->ensureLinear(cx);
  if (!linear) {
    return nullptr;
  }
  *plength = linear->length();
  return linear->twoByteChars(nogc);
}

JS_PUBLIC_API const char16_t* JS_GetTwoByteExternalStringChars(JSString* str) {
  return str->asExternal().twoByteChars();
}

JS_PUBLIC_API bool JS_GetStringCharAt(JSContext* cx, JSString* str,
                                      size_t index, char16_t* res) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(str);

  JSLinearString* linear = str->ensureLinear(cx);
  if (!linear) {
    return false;
  }

  *res = linear->latin1OrTwoByteChar(index);
  return true;
}

JS_PUBLIC_API bool JS_CopyStringChars(JSContext* cx,
                                      mozilla::Range<char16_t> dest,
                                      JSString* str) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(str);

  JSLinearString* linear = str->ensureLinear(cx);
  if (!linear) {
    return false;
  }

  MOZ_ASSERT(linear->length() <= dest.length());
  CopyChars(dest.begin().get(), *linear);
  return true;
}

extern JS_PUBLIC_API JS::UniqueTwoByteChars JS_CopyStringCharsZ(JSContext* cx,
                                                                JSString* str) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);

  JSLinearString* linear = str->ensureLinear(cx);
  if (!linear) {
    return nullptr;
  }

  size_t len = linear->length();

  static_assert(JS::MaxStringLength < UINT32_MAX,
                "len + 1 must not overflow on 32-bit platforms");

  UniqueTwoByteChars chars(cx->pod_malloc<char16_t>(len + 1));
  if (!chars) {
    return nullptr;
  }

  CopyChars(chars.get(), *linear);
  chars[len] = '\0';

  return chars;
}

extern JS_PUBLIC_API JSLinearString* JS_EnsureLinearString(JSContext* cx,
                                                           JSString* str) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(str);
  return str->ensureLinear(cx);
}

JS_PUBLIC_API bool JS_CompareStrings(JSContext* cx, JSString* str1,
                                     JSString* str2, int32_t* result) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);

  return CompareStrings(cx, str1, str2, result);
}

JS_PUBLIC_API bool JS_StringEqualsAscii(JSContext* cx, JSString* str,
                                        const char* asciiBytes, bool* match) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);

  JSLinearString* linearStr = str->ensureLinear(cx);
  if (!linearStr) {
    return false;
  }
  *match = StringEqualsAscii(linearStr, asciiBytes);
  return true;
}

JS_PUBLIC_API bool JS_StringEqualsAscii(JSContext* cx, JSString* str,
                                        const char* asciiBytes, size_t length,
                                        bool* match) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);

  JSLinearString* linearStr = str->ensureLinear(cx);
  if (!linearStr) {
    return false;
  }
  *match = StringEqualsAscii(linearStr, asciiBytes, length);
  return true;
}

JS_PUBLIC_API bool JS_LinearStringEqualsAscii(JSLinearString* str,
                                              const char* asciiBytes) {
  return StringEqualsAscii(str, asciiBytes);
}

JS_PUBLIC_API bool JS_LinearStringEqualsAscii(JSLinearString* str,
                                              const char* asciiBytes,
                                              size_t length) {
  return StringEqualsAscii(str, asciiBytes, length);
}

JS_PUBLIC_API size_t JS_PutEscapedLinearString(char* buffer, size_t size,
                                               JSLinearString* str,
                                               char quote) {
  return PutEscapedString(buffer, size, str, quote);
}

JS_PUBLIC_API size_t JS_PutEscapedString(JSContext* cx, char* buffer,
                                         size_t size, JSString* str,
                                         char quote) {
  AssertHeapIsIdle();
  JSLinearString* linearStr = str->ensureLinear(cx);
  if (!linearStr) {
    return size_t(-1);
  }
  return PutEscapedString(buffer, size, linearStr, quote);
}

JS_PUBLIC_API JSString* JS_NewDependentString(JSContext* cx, HandleString str,
                                              size_t start, size_t length) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  return NewDependentString(cx, str, start, length);
}

JS_PUBLIC_API JSString* JS_ConcatStrings(JSContext* cx, HandleString left,
                                         HandleString right) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  return ConcatStrings<CanGC>(cx, left, right);
}

JS_PUBLIC_API bool JS_DecodeBytes(JSContext* cx, const char* src, size_t srclen,
                                  char16_t* dst, size_t* dstlenp) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);

  if (!dst) {
    *dstlenp = srclen;
    return true;
  }

  size_t dstlen = *dstlenp;

  if (srclen > dstlen) {
    CopyAndInflateChars(dst, src, dstlen);

    gc::AutoSuppressGC suppress(cx);
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_BUFFER_TOO_SMALL);
    return false;
  }

  CopyAndInflateChars(dst, src, srclen);
  *dstlenp = srclen;
  return true;
}

JS_PUBLIC_API JS::UniqueChars JS_EncodeStringToASCII(JSContext* cx,
                                                     JSString* str) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);

  return js::EncodeAscii(cx, str);
}

JS_PUBLIC_API JS::UniqueChars JS_EncodeStringToLatin1(JSContext* cx,
                                                      JSString* str) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);

  return js::EncodeLatin1(cx, str);
}

JS_PUBLIC_API JS::UniqueChars JS_EncodeStringToUTF8(JSContext* cx,
                                                    HandleString str) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);

  return StringToNewUTF8CharsZ(cx, *str);
}

JS_PUBLIC_API size_t JS_GetStringEncodingLength(JSContext* cx, JSString* str) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);

  if (!str->ensureLinear(cx)) {
    return size_t(-1);
  }
  return str->length();
}

JS_PUBLIC_API bool JS_EncodeStringToBuffer(JSContext* cx, JSString* str,
                                           char* buffer, size_t length) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);

  JSLinearString* linear = str->ensureLinear(cx);
  if (!linear) {
    return false;
  }

  JS::AutoCheckCannotGC nogc;
  size_t writeLength = std::min(linear->length(), length);
  if (linear->hasLatin1Chars()) {
    mozilla::PodCopy(reinterpret_cast<Latin1Char*>(buffer),
                     linear->latin1Chars(nogc), writeLength);
  } else {
    const char16_t* src = linear->twoByteChars(nogc);
    for (size_t i = 0; i < writeLength; i++) {
      buffer[i] = char(src[i]);
    }
  }
  return true;
}

JS_PUBLIC_API mozilla::Maybe<mozilla::Tuple<size_t, size_t>>
JS_EncodeStringToUTF8BufferPartial(JSContext* cx, JSString* str,
                                   mozilla::Span<char> buffer) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  JS::AutoCheckCannotGC nogc;
  return str->encodeUTF8Partial(nogc, buffer);
}

JS_PUBLIC_API JS::Symbol* JS::NewSymbol(JSContext* cx,
                                        HandleString description) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  if (description) {
    cx->check(description);
  }

  return Symbol::new_(cx, SymbolCode::UniqueSymbol, description);
}

JS_PUBLIC_API JS::Symbol* JS::GetSymbolFor(JSContext* cx, HandleString key) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(key);

  return Symbol::for_(cx, key);
}

JS_PUBLIC_API JSString* JS::GetSymbolDescription(HandleSymbol symbol) {
  return symbol->description();
}

JS_PUBLIC_API JS::SymbolCode JS::GetSymbolCode(Handle<Symbol*> symbol) {
  return symbol->code();
}

JS_PUBLIC_API JS::Symbol* JS::GetWellKnownSymbol(JSContext* cx,
                                                 JS::SymbolCode which) {
  return cx->wellKnownSymbols().get(which);
}

#ifdef DEBUG
static bool PropertySpecNameIsDigits(JSPropertySpec::Name name) {
  if (name.isSymbol()) {
    return false;
  }
  const char* s = name.string();
  if (!*s) {
    return false;
  }
  for (; *s; s++) {
    if (*s < '0' || *s > '9') {
      return false;
    }
  }
  return true;
}
#endif  // DEBUG

JS_PUBLIC_API bool JS::PropertySpecNameEqualsId(JSPropertySpec::Name name,
                                                HandleId id) {
  if (name.isSymbol()) {
    return id.isWellKnownSymbol(name.symbol());
  }

  MOZ_ASSERT(!PropertySpecNameIsDigits(name));
  return id.isAtom() && JS_LinearStringEqualsAscii(id.toAtom(), name.string());
}

JS_PUBLIC_API bool JS_Stringify(JSContext* cx, MutableHandleValue vp,
                                HandleObject replacer, HandleValue space,
                                JSONWriteCallback callback, void* data) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(replacer, space);
  StringBuffer sb(cx);
  if (!sb.ensureTwoByteChars()) {
    return false;
  }
  if (!Stringify(cx, vp, replacer, space, sb, StringifyBehavior::Normal)) {
    return false;
  }
  if (sb.empty() && !sb.append(cx->names().null)) {
    return false;
  }
  return callback(sb.rawTwoByteBegin(), sb.length(), data);
}

JS_PUBLIC_API bool JS::ToJSONMaybeSafely(JSContext* cx, JS::HandleObject input,
                                         JSONWriteCallback callback,
                                         void* data) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(input);

  StringBuffer sb(cx);
  if (!sb.ensureTwoByteChars()) {
    return false;
  }

  RootedValue inputValue(cx, ObjectValue(*input));
  if (!Stringify(cx, &inputValue, nullptr, NullHandleValue, sb,
                 StringifyBehavior::RestrictedSafe))
    return false;

  if (sb.empty() && !sb.append(cx->names().null)) {
    return false;
  }

  return callback(sb.rawTwoByteBegin(), sb.length(), data);
}

JS_PUBLIC_API bool JS_ParseJSON(JSContext* cx, const char16_t* chars,
                                uint32_t len, MutableHandleValue vp) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  return ParseJSONWithReviver(cx, mozilla::Range<const char16_t>(chars, len),
                              NullHandleValue, vp);
}

JS_PUBLIC_API bool JS_ParseJSON(JSContext* cx, HandleString str,
                                MutableHandleValue vp) {
  return JS_ParseJSONWithReviver(cx, str, NullHandleValue, vp);
}

JS_PUBLIC_API bool JS_ParseJSONWithReviver(JSContext* cx, const char16_t* chars,
                                           uint32_t len, HandleValue reviver,
                                           MutableHandleValue vp) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  return ParseJSONWithReviver(cx, mozilla::Range<const char16_t>(chars, len),
                              reviver, vp);
}

JS_PUBLIC_API bool JS_ParseJSONWithReviver(JSContext* cx, HandleString str,
                                           HandleValue reviver,
                                           MutableHandleValue vp) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(str);

  AutoStableStringChars stableChars(cx);
  if (!stableChars.init(cx, str)) {
    return false;
  }

  return stableChars.isLatin1()
             ? ParseJSONWithReviver(cx, stableChars.latin1Range(), reviver, vp)
             : ParseJSONWithReviver(cx, stableChars.twoByteRange(), reviver,
                                    vp);
}

/************************************************************************/

JS_PUBLIC_API void JS_ReportErrorASCII(JSContext* cx, const char* format, ...) {
  va_list ap;

  AssertHeapIsIdle();
  va_start(ap, format);
  ReportErrorVA(cx, IsWarning::No, format, ArgumentsAreASCII, ap);
  va_end(ap);
}

JS_PUBLIC_API void JS_ReportErrorLatin1(JSContext* cx, const char* format,
                                        ...) {
  va_list ap;

  AssertHeapIsIdle();
  va_start(ap, format);
  ReportErrorVA(cx, IsWarning::No, format, ArgumentsAreLatin1, ap);
  va_end(ap);
}

JS_PUBLIC_API void JS_ReportErrorUTF8(JSContext* cx, const char* format, ...) {
  va_list ap;

  AssertHeapIsIdle();
  va_start(ap, format);
  ReportErrorVA(cx, IsWarning::No, format, ArgumentsAreUTF8, ap);
  va_end(ap);
}

JS_PUBLIC_API void JS_ReportErrorNumberASCII(JSContext* cx,
                                             JSErrorCallback errorCallback,
                                             void* userRef,
                                             const unsigned errorNumber, ...) {
  va_list ap;
  va_start(ap, errorNumber);
  JS_ReportErrorNumberASCIIVA(cx, errorCallback, userRef, errorNumber, ap);
  va_end(ap);
}

JS_PUBLIC_API void JS_ReportErrorNumberASCIIVA(JSContext* cx,
                                               JSErrorCallback errorCallback,
                                               void* userRef,
                                               const unsigned errorNumber,
                                               va_list ap) {
  AssertHeapIsIdle();
  ReportErrorNumberVA(cx, IsWarning::No, errorCallback, userRef, errorNumber,
                      ArgumentsAreASCII, ap);
}

JS_PUBLIC_API void JS_ReportErrorNumberLatin1(JSContext* cx,
                                              JSErrorCallback errorCallback,
                                              void* userRef,
                                              const unsigned errorNumber, ...) {
  va_list ap;
  va_start(ap, errorNumber);
  JS_ReportErrorNumberLatin1VA(cx, errorCallback, userRef, errorNumber, ap);
  va_end(ap);
}

JS_PUBLIC_API void JS_ReportErrorNumberLatin1VA(JSContext* cx,
                                                JSErrorCallback errorCallback,
                                                void* userRef,
                                                const unsigned errorNumber,
                                                va_list ap) {
  AssertHeapIsIdle();
  ReportErrorNumberVA(cx, IsWarning::No, errorCallback, userRef, errorNumber,
                      ArgumentsAreLatin1, ap);
}

JS_PUBLIC_API void JS_ReportErrorNumberUTF8(JSContext* cx,
                                            JSErrorCallback errorCallback,
                                            void* userRef,
                                            const unsigned errorNumber, ...) {
  va_list ap;
  va_start(ap, errorNumber);
  JS_ReportErrorNumberUTF8VA(cx, errorCallback, userRef, errorNumber, ap);
  va_end(ap);
}

JS_PUBLIC_API void JS_ReportErrorNumberUTF8VA(JSContext* cx,
                                              JSErrorCallback errorCallback,
                                              void* userRef,
                                              const unsigned errorNumber,
                                              va_list ap) {
  AssertHeapIsIdle();
  ReportErrorNumberVA(cx, IsWarning::No, errorCallback, userRef, errorNumber,
                      ArgumentsAreUTF8, ap);
}

JS_PUBLIC_API void JS_ReportErrorNumberUTF8Array(JSContext* cx,
                                                 JSErrorCallback errorCallback,
                                                 void* userRef,
                                                 const unsigned errorNumber,
                                                 const char** args) {
  AssertHeapIsIdle();
  ReportErrorNumberUTF8Array(cx, IsWarning::No, errorCallback, userRef,
                             errorNumber, args);
}

JS_PUBLIC_API void JS_ReportErrorNumberUC(JSContext* cx,
                                          JSErrorCallback errorCallback,
                                          void* userRef,
                                          const unsigned errorNumber, ...) {
  va_list ap;

  AssertHeapIsIdle();
  va_start(ap, errorNumber);
  ReportErrorNumberVA(cx, IsWarning::No, errorCallback, userRef, errorNumber,
                      ArgumentsAreUnicode, ap);
  va_end(ap);
}

JS_PUBLIC_API void JS_ReportErrorNumberUCArray(JSContext* cx,
                                               JSErrorCallback errorCallback,
                                               void* userRef,
                                               const unsigned errorNumber,
                                               const char16_t** args) {
  AssertHeapIsIdle();
  ReportErrorNumberUCArray(cx, IsWarning::No, errorCallback, userRef,
                           errorNumber, args);
}

JS_PUBLIC_API void JS_ReportOutOfMemory(JSContext* cx) {
  ReportOutOfMemory(cx);
}

JS_PUBLIC_API void JS_ReportAllocationOverflow(JSContext* cx) {
  ReportAllocationOverflow(cx);
}

JS_PUBLIC_API bool JS_ExpandErrorArgumentsASCII(JSContext* cx,
                                                JSErrorCallback errorCallback,
                                                const unsigned errorNumber,
                                                JSErrorReport* reportp, ...) {
  va_list ap;
  bool ok;

  AssertHeapIsIdle();
  va_start(ap, reportp);
  ok = ExpandErrorArgumentsVA(cx, errorCallback, nullptr, errorNumber,
                              ArgumentsAreASCII, reportp, ap);
  va_end(ap);
  return ok;
}
/************************************************************************/

JS_PUBLIC_API bool JS_SetDefaultLocale(JSRuntime* rt, const char* locale) {
  AssertHeapIsIdle();
  return rt->setDefaultLocale(locale);
}

JS_PUBLIC_API UniqueChars JS_GetDefaultLocale(JSContext* cx) {
  AssertHeapIsIdle();
  if (const char* locale = cx->runtime()->getDefaultLocale()) {
    return DuplicateString(cx, locale);
  }

  return nullptr;
}

JS_PUBLIC_API void JS_ResetDefaultLocale(JSRuntime* rt) {
  AssertHeapIsIdle();
  rt->resetDefaultLocale();
}

JS_PUBLIC_API void JS_SetLocaleCallbacks(JSRuntime* rt,
                                         const JSLocaleCallbacks* callbacks) {
  AssertHeapIsIdle();
  rt->localeCallbacks = callbacks;
}

JS_PUBLIC_API const JSLocaleCallbacks* JS_GetLocaleCallbacks(JSRuntime* rt) {
  /* This function can be called by a finalizer. */
  return rt->localeCallbacks;
}

/************************************************************************/

JS_PUBLIC_API bool JS_IsExceptionPending(JSContext* cx) {
  /* This function can be called by a finalizer. */
  return (bool)cx->isExceptionPending();
}

JS_PUBLIC_API bool JS_IsThrowingOutOfMemory(JSContext* cx) {
  return cx->isThrowingOutOfMemory();
}

JS_PUBLIC_API bool JS_GetPendingException(JSContext* cx,
                                          MutableHandleValue vp) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  if (!cx->isExceptionPending()) {
    return false;
  }
  return cx->getPendingException(vp);
}

JS_PUBLIC_API void JS_SetPendingException(JSContext* cx, HandleValue value,
                                          JS::ExceptionStackBehavior behavior) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  // We don't check the compartment of `value` here, because we're not
  // doing anything with it other than storing it, and stored
  // exception values can be in an abitrary compartment.

  if (behavior == JS::ExceptionStackBehavior::Capture) {
    cx->setPendingExceptionAndCaptureStack(value);
  } else {
    cx->setPendingException(value, nullptr);
  }
}

JS_PUBLIC_API void JS_ClearPendingException(JSContext* cx) {
  AssertHeapIsIdle();
  cx->clearPendingException();
}

JS::AutoSaveExceptionState::AutoSaveExceptionState(JSContext* cx)
    : context(cx),
      wasPropagatingForcedReturn(cx->propagatingForcedReturn_),
      wasOverRecursed(cx->overRecursed_),
      wasThrowing(cx->throwing),
      exceptionValue(cx),
      exceptionStack(cx) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  if (wasPropagatingForcedReturn) {
    cx->clearPropagatingForcedReturn();
  }
  if (wasOverRecursed) {
    cx->overRecursed_ = false;
  }
  if (wasThrowing) {
    exceptionValue = cx->unwrappedException();
    exceptionStack = cx->unwrappedExceptionStack();
    cx->clearPendingException();
  }
}

void JS::AutoSaveExceptionState::drop() {
  wasPropagatingForcedReturn = false;
  wasOverRecursed = false;
  wasThrowing = false;
  exceptionValue.setUndefined();
  exceptionStack = nullptr;
}

void JS::AutoSaveExceptionState::restore() {
  context->propagatingForcedReturn_ = wasPropagatingForcedReturn;
  context->overRecursed_ = wasOverRecursed;
  context->throwing = wasThrowing;
  context->unwrappedException() = exceptionValue;
  if (exceptionStack) {
    context->unwrappedExceptionStack() = &exceptionStack->as<SavedFrame>();
  }
  drop();
}

JS::AutoSaveExceptionState::~AutoSaveExceptionState() {
  if (!context->isExceptionPending()) {
    if (wasPropagatingForcedReturn) {
      context->setPropagatingForcedReturn();
    }
    if (wasThrowing) {
      context->overRecursed_ = wasOverRecursed;
      context->throwing = true;
      context->unwrappedException() = exceptionValue;
      if (exceptionStack) {
        context->unwrappedExceptionStack() = &exceptionStack->as<SavedFrame>();
      }
    }
  }
}

JS_PUBLIC_API JSErrorReport* JS_ErrorFromException(JSContext* cx,
                                                   HandleObject obj) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(obj);
  return ErrorFromException(cx, obj);
}

void JSErrorReport::initBorrowedLinebuf(const char16_t* linebufArg,
                                        size_t linebufLengthArg,
                                        size_t tokenOffsetArg) {
  MOZ_ASSERT(linebufArg);
  MOZ_ASSERT(tokenOffsetArg <= linebufLengthArg);
  MOZ_ASSERT(linebufArg[linebufLengthArg] == '\0');

  linebuf_ = linebufArg;
  linebufLength_ = linebufLengthArg;
  tokenOffset_ = tokenOffsetArg;
}

void JSErrorReport::freeLinebuf() {
  if (ownsLinebuf_ && linebuf_) {
    js_free((void*)linebuf_);
    ownsLinebuf_ = false;
  }
  linebuf_ = nullptr;
}

JSString* JSErrorBase::newMessageString(JSContext* cx) {
  if (!message_) {
    return cx->runtime()->emptyString;
  }

  return JS_NewStringCopyUTF8Z(cx, message_);
}

void JSErrorBase::freeMessage() {
  if (ownsMessage_) {
    js_free((void*)message_.get());
    ownsMessage_ = false;
  }
  message_ = JS::ConstUTF8CharsZ();
}

JSErrorNotes::JSErrorNotes() : notes_() {}

JSErrorNotes::~JSErrorNotes() = default;

static UniquePtr<JSErrorNotes::Note> CreateErrorNoteVA(
    JSContext* cx, const char* filename, unsigned sourceId, unsigned lineno,
    unsigned column, JSErrorCallback errorCallback, void* userRef,
    const unsigned errorNumber, ErrorArgumentsType argumentsType, va_list ap) {
  auto note = MakeUnique<JSErrorNotes::Note>();
  if (!note) {
    ReportOutOfMemory(cx);
    return nullptr;
  }

  note->errorNumber = errorNumber;
  note->filename = filename;
  note->sourceId = sourceId;
  note->lineno = lineno;
  note->column = column;

  if (!ExpandErrorArgumentsVA(cx, errorCallback, userRef, errorNumber, nullptr,
                              argumentsType, note.get(), ap)) {
    return nullptr;
  }

  return note;
}

bool JSErrorNotes::addNoteASCII(JSContext* cx, const char* filename,
                                unsigned sourceId, unsigned lineno,
                                unsigned column, JSErrorCallback errorCallback,
                                void* userRef, const unsigned errorNumber,
                                ...) {
  va_list ap;
  va_start(ap, errorNumber);
  auto note =
      CreateErrorNoteVA(cx, filename, sourceId, lineno, column, errorCallback,
                        userRef, errorNumber, ArgumentsAreASCII, ap);
  va_end(ap);

  if (!note) {
    return false;
  }
  if (!notes_.append(std::move(note))) {
    ReportOutOfMemory(cx);
    return false;
  }
  return true;
}

bool JSErrorNotes::addNoteLatin1(JSContext* cx, const char* filename,
                                 unsigned sourceId, unsigned lineno,
                                 unsigned column, JSErrorCallback errorCallback,
                                 void* userRef, const unsigned errorNumber,
                                 ...) {
  va_list ap;
  va_start(ap, errorNumber);
  auto note =
      CreateErrorNoteVA(cx, filename, sourceId, lineno, column, errorCallback,
                        userRef, errorNumber, ArgumentsAreLatin1, ap);
  va_end(ap);

  if (!note) {
    return false;
  }
  if (!notes_.append(std::move(note))) {
    ReportOutOfMemory(cx);
    return false;
  }
  return true;
}

bool JSErrorNotes::addNoteUTF8(JSContext* cx, const char* filename,
                               unsigned sourceId, unsigned lineno,
                               unsigned column, JSErrorCallback errorCallback,
                               void* userRef, const unsigned errorNumber, ...) {
  va_list ap;
  va_start(ap, errorNumber);
  auto note =
      CreateErrorNoteVA(cx, filename, sourceId, lineno, column, errorCallback,
                        userRef, errorNumber, ArgumentsAreUTF8, ap);
  va_end(ap);

  if (!note) {
    return false;
  }
  if (!notes_.append(std::move(note))) {
    ReportOutOfMemory(cx);
    return false;
  }
  return true;
}

JS_PUBLIC_API size_t JSErrorNotes::length() { return notes_.length(); }

UniquePtr<JSErrorNotes> JSErrorNotes::copy(JSContext* cx) {
  auto copiedNotes = MakeUnique<JSErrorNotes>();
  if (!copiedNotes) {
    ReportOutOfMemory(cx);
    return nullptr;
  }

  for (auto&& note : *this) {
    UniquePtr<JSErrorNotes::Note> copied = CopyErrorNote(cx, note.get());
    if (!copied) {
      return nullptr;
    }

    if (!copiedNotes->notes_.append(std::move(copied))) {
      return nullptr;
    }
  }

  return copiedNotes;
}

JS_PUBLIC_API JSErrorNotes::iterator JSErrorNotes::begin() {
  return iterator(notes_.begin());
}

JS_PUBLIC_API JSErrorNotes::iterator JSErrorNotes::end() {
  return iterator(notes_.end());
}

extern MOZ_NEVER_INLINE JS_PUBLIC_API void JS_AbortIfWrongThread(
    JSContext* cx) {
  if (!CurrentThreadCanAccessRuntime(cx->runtime())) {
    MOZ_CRASH();
  }
  if (TlsContext.get() != cx) {
    MOZ_CRASH();
  }
}

#ifdef JS_GC_ZEAL
JS_PUBLIC_API void JS_GetGCZealBits(JSContext* cx, uint32_t* zealBits,
                                    uint32_t* frequency,
                                    uint32_t* nextScheduled) {
  cx->runtime()->gc.getZealBits(zealBits, frequency, nextScheduled);
}

JS_PUBLIC_API void JS_SetGCZeal(JSContext* cx, uint8_t zeal,
                                uint32_t frequency) {
  cx->runtime()->gc.setZeal(zeal, frequency);
}

JS_PUBLIC_API void JS_UnsetGCZeal(JSContext* cx, uint8_t zeal) {
  cx->runtime()->gc.unsetZeal(zeal);
}

JS_PUBLIC_API void JS_ScheduleGC(JSContext* cx, uint32_t count) {
  cx->runtime()->gc.setNextScheduled(count);
}
#endif

JS_PUBLIC_API void JS_SetParallelParsingEnabled(JSContext* cx, bool enabled) {
  cx->runtime()->setParallelParsingEnabled(enabled);
}

JS_PUBLIC_API void JS_SetOffthreadIonCompilationEnabled(JSContext* cx,
                                                        bool enabled) {
  cx->runtime()->setOffthreadIonCompilationEnabled(enabled);
}

JS_PUBLIC_API void JS_SetGlobalJitCompilerOption(JSContext* cx,
                                                 JSJitCompilerOption opt,
                                                 uint32_t value) {
  JSRuntime* rt = cx->runtime();
  switch (opt) {
    case JSJITCOMPILER_BASELINE_INTERPRETER_WARMUP_TRIGGER:
      if (value == uint32_t(-1)) {
        jit::DefaultJitOptions defaultValues;
        value = defaultValues.baselineInterpreterWarmUpThreshold;
      }
      jit::JitOptions.baselineInterpreterWarmUpThreshold = value;
      break;
    case JSJITCOMPILER_BASELINE_WARMUP_TRIGGER:
      if (value == uint32_t(-1)) {
        jit::DefaultJitOptions defaultValues;
        value = defaultValues.baselineJitWarmUpThreshold;
      }
      jit::JitOptions.baselineJitWarmUpThreshold = value;
      break;
    case JSJITCOMPILER_IC_FORCE_MEGAMORPHIC:
      jit::JitOptions.forceMegamorphicICs = !!value;
      break;
    case JSJITCOMPILER_ION_NORMAL_WARMUP_TRIGGER:
      if (value == uint32_t(-1)) {
        jit::JitOptions.resetNormalIonWarmUpThreshold();
        break;
      }
      jit::JitOptions.setNormalIonWarmUpThreshold(value);
      break;
    case JSJITCOMPILER_ION_GVN_ENABLE:
      if (value == 0) {
        jit::JitOptions.enableGvn(false);
        JitSpew(js::jit::JitSpew_IonScripts, "Disable ion's GVN");
      } else {
        jit::JitOptions.enableGvn(true);
        JitSpew(js::jit::JitSpew_IonScripts, "Enable ion's GVN");
      }
      break;
    case JSJITCOMPILER_ION_FORCE_IC:
      if (value == 0) {
        jit::JitOptions.forceInlineCaches = false;
        JitSpew(js::jit::JitSpew_IonScripts,
                "Ion: Enable non-IC optimizations.");
      } else {
        jit::JitOptions.forceInlineCaches = true;
        JitSpew(js::jit::JitSpew_IonScripts,
                "Ion: Disable non-IC optimizations.");
      }
      break;
    case JSJITCOMPILER_ION_CHECK_RANGE_ANALYSIS:
      if (value == 0) {
        jit::JitOptions.checkRangeAnalysis = false;
        JitSpew(js::jit::JitSpew_IonScripts,
                "Ion: Enable range analysis checks.");
      } else {
        jit::JitOptions.checkRangeAnalysis = true;
        JitSpew(js::jit::JitSpew_IonScripts,
                "Ion: Disable range analysis checks.");
      }
      break;
    case JSJITCOMPILER_ION_ENABLE:
      if (value == 1) {
        jit::JitOptions.ion = true;
        JitSpew(js::jit::JitSpew_IonScripts, "Enable ion");
      } else if (value == 0) {
        jit::JitOptions.ion = false;
        JitSpew(js::jit::JitSpew_IonScripts, "Disable ion");
      }
      break;
    case JSJITCOMPILER_JIT_TRUSTEDPRINCIPALS_ENABLE:
      if (value == 1) {
        jit::JitOptions.jitForTrustedPrincipals = true;
        JitSpew(js::jit::JitSpew_IonScripts,
                "Enable ion and baselinejit for trusted principals");
      } else if (value == 0) {
        jit::JitOptions.jitForTrustedPrincipals = false;
        JitSpew(js::jit::JitSpew_IonScripts,
                "Disable ion and baselinejit for trusted principals");
      }
      break;
    case JSJITCOMPILER_ION_FREQUENT_BAILOUT_THRESHOLD:
      if (value == uint32_t(-1)) {
        jit::DefaultJitOptions defaultValues;
        value = defaultValues.frequentBailoutThreshold;
      }
      jit::JitOptions.frequentBailoutThreshold = value;
      break;
    case JSJITCOMPILER_BASELINE_INTERPRETER_ENABLE:
      if (value == 1) {
        jit::JitOptions.baselineInterpreter = true;
      } else if (value == 0) {
        ReleaseAllJITCode(rt->defaultFreeOp());
        jit::JitOptions.baselineInterpreter = false;
      }
      break;
    case JSJITCOMPILER_BASELINE_ENABLE:
      if (value == 1) {
        jit::JitOptions.baselineJit = true;
        ReleaseAllJITCode(rt->defaultFreeOp());
        JitSpew(js::jit::JitSpew_BaselineScripts, "Enable baseline");
      } else if (value == 0) {
        jit::JitOptions.baselineJit = false;
        ReleaseAllJITCode(rt->defaultFreeOp());
        JitSpew(js::jit::JitSpew_BaselineScripts, "Disable baseline");
      }
      break;
    case JSJITCOMPILER_NATIVE_REGEXP_ENABLE:
      jit::JitOptions.nativeRegExp = !!value;
      break;
    case JSJITCOMPILER_OFFTHREAD_COMPILATION_ENABLE:
      if (value == 1) {
        rt->setOffthreadIonCompilationEnabled(true);
        JitSpew(js::jit::JitSpew_IonScripts, "Enable offthread compilation");
      } else if (value == 0) {
        rt->setOffthreadIonCompilationEnabled(false);
        JitSpew(js::jit::JitSpew_IonScripts, "Disable offthread compilation");
      }
      break;
    case JSJITCOMPILER_INLINING_BYTECODE_MAX_LENGTH:
      if (value == uint32_t(-1)) {
        jit::DefaultJitOptions defaultValues;
        value = defaultValues.smallFunctionMaxBytecodeLength;
      }
      jit::JitOptions.smallFunctionMaxBytecodeLength = value;
      break;
    case JSJITCOMPILER_JUMP_THRESHOLD:
      if (value == uint32_t(-1)) {
        jit::DefaultJitOptions defaultValues;
        value = defaultValues.jumpThreshold;
      }
      jit::JitOptions.jumpThreshold = value;
      break;
    case JSJITCOMPILER_SPECTRE_INDEX_MASKING:
      jit::JitOptions.spectreIndexMasking = !!value;
      break;
    case JSJITCOMPILER_SPECTRE_OBJECT_MITIGATIONS:
      jit::JitOptions.spectreObjectMitigations = !!value;
      break;
    case JSJITCOMPILER_SPECTRE_STRING_MITIGATIONS:
      jit::JitOptions.spectreStringMitigations = !!value;
      break;
    case JSJITCOMPILER_SPECTRE_VALUE_MASKING:
      jit::JitOptions.spectreValueMasking = !!value;
      break;
    case JSJITCOMPILER_SPECTRE_JIT_TO_CXX_CALLS:
      jit::JitOptions.spectreJitToCxxCalls = !!value;
      break;
    case JSJITCOMPILER_WASM_FOLD_OFFSETS:
      jit::JitOptions.wasmFoldOffsets = !!value;
      break;
    case JSJITCOMPILER_WASM_DELAY_TIER2:
      jit::JitOptions.wasmDelayTier2 = !!value;
      break;
    case JSJITCOMPILER_WASM_JIT_BASELINE:
      JS::ContextOptionsRef(cx).setWasmBaseline(!!value);
      break;
    case JSJITCOMPILER_WASM_JIT_OPTIMIZING:
#ifdef ENABLE_WASM_CRANELIFT
      JS::ContextOptionsRef(cx).setWasmCranelift(!!value);
      JS::ContextOptionsRef(cx).setWasmIon(!value);
#else
      JS::ContextOptionsRef(cx).setWasmIon(!!value);
      JS::ContextOptionsRef(cx).setWasmCranelift(!value);
#endif
      break;
#ifdef DEBUG
    case JSJITCOMPILER_FULL_DEBUG_CHECKS:
      jit::JitOptions.fullDebugChecks = !!value;
      break;
#endif
    default:
      break;
  }
}

JS_PUBLIC_API bool JS_GetGlobalJitCompilerOption(JSContext* cx,
                                                 JSJitCompilerOption opt,
                                                 uint32_t* valueOut) {
  MOZ_ASSERT(valueOut);
#ifndef JS_CODEGEN_NONE
  JSRuntime* rt = cx->runtime();
  switch (opt) {
    case JSJITCOMPILER_BASELINE_INTERPRETER_WARMUP_TRIGGER:
      *valueOut = jit::JitOptions.baselineInterpreterWarmUpThreshold;
      break;
    case JSJITCOMPILER_BASELINE_WARMUP_TRIGGER:
      *valueOut = jit::JitOptions.baselineJitWarmUpThreshold;
      break;
    case JSJITCOMPILER_IC_FORCE_MEGAMORPHIC:
      *valueOut = jit::JitOptions.forceMegamorphicICs;
      break;
    case JSJITCOMPILER_ION_NORMAL_WARMUP_TRIGGER:
      *valueOut = jit::JitOptions.normalIonWarmUpThreshold;
      break;
    case JSJITCOMPILER_ION_FORCE_IC:
      *valueOut = jit::JitOptions.forceInlineCaches;
      break;
    case JSJITCOMPILER_ION_CHECK_RANGE_ANALYSIS:
      *valueOut = jit::JitOptions.checkRangeAnalysis;
      break;
    case JSJITCOMPILER_ION_ENABLE:
      *valueOut = jit::JitOptions.ion;
      break;
    case JSJITCOMPILER_ION_FREQUENT_BAILOUT_THRESHOLD:
      *valueOut = jit::JitOptions.frequentBailoutThreshold;
      break;
    case JSJITCOMPILER_INLINING_BYTECODE_MAX_LENGTH:
      *valueOut = jit::JitOptions.smallFunctionMaxBytecodeLength;
      break;
    case JSJITCOMPILER_BASELINE_INTERPRETER_ENABLE:
      *valueOut = jit::JitOptions.baselineInterpreter;
      break;
    case JSJITCOMPILER_BASELINE_ENABLE:
      *valueOut = jit::JitOptions.baselineJit;
      break;
    case JSJITCOMPILER_NATIVE_REGEXP_ENABLE:
      *valueOut = jit::JitOptions.nativeRegExp;
      break;
    case JSJITCOMPILER_OFFTHREAD_COMPILATION_ENABLE:
      *valueOut = rt->canUseOffthreadIonCompilation();
      break;
    case JSJITCOMPILER_WASM_FOLD_OFFSETS:
      *valueOut = jit::JitOptions.wasmFoldOffsets ? 1 : 0;
      break;
    case JSJITCOMPILER_WASM_JIT_BASELINE:
      *valueOut = JS::ContextOptionsRef(cx).wasmBaseline() ? 1 : 0;
      break;
    case JSJITCOMPILER_WASM_JIT_OPTIMIZING:
#  ifdef ENABLE_WASM_CRANELIFT
      *valueOut = JS::ContextOptionsRef(cx).wasmCranelift() ? 1 : 0;
#  else
      *valueOut = JS::ContextOptionsRef(cx).wasmIon() ? 1 : 0;
#  endif
      break;
#  ifdef DEBUG
    case JSJITCOMPILER_FULL_DEBUG_CHECKS:
      *valueOut = jit::JitOptions.fullDebugChecks ? 1 : 0;
      break;
#  endif
    default:
      return false;
  }
#else
  *valueOut = 0;
#endif
  return true;
}

/************************************************************************/

#if !defined(STATIC_EXPORTABLE_JS_API) && !defined(STATIC_JS_API) && \
    defined(XP_WIN)

#  include "util/Windows.h"

/*
 * Initialization routine for the JS DLL.
 */
BOOL WINAPI DllMain(HINSTANCE hDLL, DWORD dwReason, LPVOID lpReserved) {
  return TRUE;
}

#endif

JS_PUBLIC_API bool JS_IndexToId(JSContext* cx, uint32_t index,
                                MutableHandleId id) {
  return IndexToId(cx, index, id);
}

JS_PUBLIC_API bool JS_CharsToId(JSContext* cx, JS::TwoByteChars chars,
                                MutableHandleId idp) {
  RootedAtom atom(cx, AtomizeChars(cx, chars.begin().get(), chars.length()));
  if (!atom) {
    return false;
  }
#ifdef DEBUG
  MOZ_ASSERT(!atom->isIndex(), "API misuse: |chars| must not encode an index");
#endif
  idp.set(AtomToId(atom));
  return true;
}

JS_PUBLIC_API bool JS_IsIdentifier(JSContext* cx, HandleString str,
                                   bool* isIdentifier) {
  cx->check(str);

  JSLinearString* linearStr = str->ensureLinear(cx);
  if (!linearStr) {
    return false;
  }

  *isIdentifier = js::frontend::IsIdentifier(linearStr);
  return true;
}

JS_PUBLIC_API bool JS_IsIdentifier(const char16_t* chars, size_t length) {
  return js::frontend::IsIdentifier(chars, length);
}

namespace JS {

void AutoFilename::reset() {
  if (ss_) {
    ss_->Release();
    ss_ = nullptr;
  }
  if (filename_.is<const char*>()) {
    filename_.as<const char*>() = nullptr;
  } else {
    filename_.as<UniqueChars>().reset();
  }
}

void AutoFilename::setScriptSource(js::ScriptSource* p) {
  MOZ_ASSERT(!ss_);
  MOZ_ASSERT(!get());
  ss_ = p;
  if (p) {
    p->AddRef();
    setUnowned(p->filename());
  }
}

void AutoFilename::setUnowned(const char* filename) {
  MOZ_ASSERT(!get());
  filename_.as<const char*>() = filename ? filename : "";
}

void AutoFilename::setOwned(UniqueChars&& filename) {
  MOZ_ASSERT(!get());
  filename_ = AsVariant(std::move(filename));
}

const char* AutoFilename::get() const {
  if (filename_.is<const char*>()) {
    return filename_.as<const char*>();
  }
  return filename_.as<UniqueChars>().get();
}

JS_PUBLIC_API bool DescribeScriptedCaller(JSContext* cx, AutoFilename* filename,
                                          unsigned* lineno, unsigned* column) {
  if (filename) {
    filename->reset();
  }
  if (lineno) {
    *lineno = 0;
  }
  if (column) {
    *column = 0;
  }

  if (!cx->compartment()) {
    return false;
  }

  NonBuiltinFrameIter i(cx, cx->realm()->principals());
  if (i.done()) {
    return false;
  }

  // If the caller is hidden, the embedding wants us to return false here so
  // that it can check its own stack (see HideScriptedCaller).
  if (i.activation()->scriptedCallerIsHidden()) {
    return false;
  }

  if (filename) {
    if (i.isWasm()) {
      // For Wasm, copy out the filename, there is no script source.
      UniqueChars copy = DuplicateString(i.filename() ? i.filename() : "");
      if (!copy) {
        filename->setUnowned("out of memory");
      } else {
        filename->setOwned(std::move(copy));
      }
    } else {
      // All other frames have a script source to read the filename from.
      filename->setScriptSource(i.scriptSource());
    }
  }

  if (lineno) {
    *lineno = i.computeLine(column);
  } else if (column) {
    i.computeLine(column);
  }

  return true;
}

// Fast path to get the activation and realm to use for GetScriptedCallerGlobal.
// If this returns false, the fast path didn't work out and the caller has to
// use the (much slower) NonBuiltinFrameIter path.
//
// The optimization here is that we skip Ion-inlined frames and only look at
// 'outer' frames. That's fine because Ion doesn't inline cross-realm calls.
// However, GetScriptedCallerGlobal has to skip self-hosted frames and Ion
// can inline self-hosted scripts, so we have to be careful:
//
// * When we see a non-self-hosted outer script, it's possible we inlined
//   self-hosted scripts into it but that doesn't matter because these scripts
//   all have the same realm/global anyway.
//
// * When we see a self-hosted outer script, it's possible we inlined
//   non-self-hosted scripts into it, so we have to give up because in this
//   case, whether or not to skip the self-hosted frame (to the possibly
//   different-realm caller) requires the slow path to handle inlining. Baseline
//   and the interpreter don't inline so this only affects Ion.
static bool GetScriptedCallerActivationRealmFast(JSContext* cx,
                                                 Activation** activation,
                                                 Realm** realm) {
  ActivationIterator activationIter(cx);

  if (activationIter.done()) {
    *activation = nullptr;
    *realm = nullptr;
    return true;
  }

  if (activationIter->isJit()) {
    jit::JitActivation* act = activationIter->asJit();
    JitFrameIter iter(act);
    while (true) {
      iter.skipNonScriptedJSFrames();
      if (iter.done()) {
        break;
      }

      if (!iter.isSelfHostedIgnoringInlining()) {
        *activation = act;
        *realm = iter.realm();
        return true;
      }

      if (iter.isJSJit() && iter.asJSJit().isIonScripted()) {
        // Ion might have inlined non-self-hosted scripts in this
        // self-hosted script.
        return false;
      }

      ++iter;
    }
  } else if (activationIter->isInterpreter()) {
    InterpreterActivation* act = activationIter->asInterpreter();
    for (InterpreterFrameIterator iter(act); !iter.done(); ++iter) {
      if (!iter.frame()->script()->selfHosted()) {
        *activation = act;
        *realm = iter.frame()->script()->realm();
        return true;
      }
    }
  }

  return false;
}

JS_PUBLIC_API JSObject* GetScriptedCallerGlobal(JSContext* cx) {
  Activation* activation;
  Realm* realm;
  if (GetScriptedCallerActivationRealmFast(cx, &activation, &realm)) {
    if (!activation) {
      return nullptr;
    }
  } else {
    NonBuiltinFrameIter i(cx);
    if (i.done()) {
      return nullptr;
    }
    activation = i.activation();
    realm = i.realm();
  }

  MOZ_ASSERT(realm->compartment() == activation->compartment());

  // If the caller is hidden, the embedding wants us to return null here so
  // that it can check its own stack (see HideScriptedCaller).
  if (activation->scriptedCallerIsHidden()) {
    return nullptr;
  }

  GlobalObject* global = realm->maybeGlobal();

  // No one should be running code in a realm without any live objects, so
  // there should definitely be a live global.
  MOZ_ASSERT(global);

  return global;
}

JS_PUBLIC_API void HideScriptedCaller(JSContext* cx) {
  MOZ_ASSERT(cx);

  // If there's no accessible activation on the stack, we'll return null from
  // DescribeScriptedCaller anyway, so there's no need to annotate anything.
  Activation* act = cx->activation();
  if (!act) {
    return;
  }
  act->hideScriptedCaller();
}

JS_PUBLIC_API void UnhideScriptedCaller(JSContext* cx) {
  Activation* act = cx->activation();
  if (!act) {
    return;
  }
  act->unhideScriptedCaller();
}

} /* namespace JS */

#ifdef JS_DEBUG
JS_PUBLIC_API void JS::detail::AssertArgumentsAreSane(JSContext* cx,
                                                      HandleValue value) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  cx->check(value);
}
#endif /* JS_DEBUG */

JS_PUBLIC_API JS::TranscodeResult JS::EncodeScript(JSContext* cx,
                                                   TranscodeBuffer& buffer,
                                                   HandleScript scriptArg) {
  // Run-once top-level scripts may mutate singleton objects so do not allow
  // encoding them. It could be possible to encode early enough to avoid this,
  // but for consistency with `CopyScriptImpl` we always disallow.
  MOZ_ASSERT(!scriptArg->isFunction());
  if (scriptArg->treatAsRunOnce()) {
    return JS::TranscodeResult::Failure_RunOnceNotSupported;
  }

  XDREncoder encoder(cx, buffer, buffer.length());
  RootedScript script(cx, scriptArg);
  XDRResult res = encoder.codeScript(&script);
  if (res.isErr()) {
    buffer.clearAndFree();
    return res.unwrapErr();
  }
  MOZ_ASSERT(!buffer.empty());
  return JS::TranscodeResult::Ok;
}

JS_PUBLIC_API JS::TranscodeResult JS::DecodeScript(
    JSContext* cx, const ReadOnlyCompileOptions& options,
    TranscodeBuffer& buffer, JS::MutableHandleScript scriptp,
    size_t cursorIndex) {
  auto decoder = js::MakeUnique<XDRDecoder>(cx, &options, buffer, cursorIndex);
  if (!decoder) {
    ReportOutOfMemory(cx);
    return JS::TranscodeResult::Throw;
  }
  XDRResult res = decoder->codeScript(scriptp);
  MOZ_ASSERT(bool(scriptp) == res.isOk());
  if (res.isErr()) {
    return res.unwrapErr();
  }
  return JS::TranscodeResult::Ok;
}

static JS::TranscodeResult DecodeStencil(JSContext* cx,
                                         JS::TranscodeBuffer& buffer,
                                         frontend::CompilationInput& input,
                                         frontend::CompilationStencil& stencil,
                                         size_t cursorIndex) {
  XDRStencilDecoder decoder(cx, buffer, cursorIndex);

  if (!input.initForGlobal(cx)) {
    return JS::TranscodeResult::Throw;
  }

  XDRResult res = decoder.codeStencil(input, stencil);
  if (res.isErr()) {
    return res.unwrapErr();
  }

  return JS::TranscodeResult::Ok;
}

JS_PUBLIC_API JS::TranscodeResult JS::DecodeScriptMaybeStencil(
    JSContext* cx, const ReadOnlyCompileOptions& options,
    TranscodeBuffer& buffer, JS::MutableHandleScript scriptp,
    size_t cursorIndex) {
  if (!options.useStencilXDR) {
    // The buffer contains JSScript.
    return JS::DecodeScript(cx, options, buffer, scriptp, cursorIndex);
  }

  MOZ_ASSERT(JS::IsTranscodingBytecodeAligned(buffer.begin()));
  MOZ_ASSERT(JS::IsTranscodingBytecodeOffsetAligned(cursorIndex));

  // The buffer contains stencil.

  Rooted<frontend::CompilationInput> input(cx,
                                           frontend::CompilationInput(options));
  frontend::CompilationStencil stencil(nullptr);

  JS::TranscodeResult res =
      DecodeStencil(cx, buffer, input.get(), stencil, cursorIndex);
  if (res != JS::TranscodeResult::Ok) {
    return res;
  }

  Rooted<frontend::CompilationGCOutput> gcOutput(cx);
  if (!frontend::InstantiateStencils(cx, input.get(), stencil,
                                     gcOutput.get())) {
    return JS::TranscodeResult::Throw;
  }

  MOZ_ASSERT(gcOutput.get().script);
  scriptp.set(gcOutput.get().script);

  return JS::TranscodeResult::Ok;
}

JS_PUBLIC_API JS::TranscodeResult JS::DecodeScript(
    JSContext* cx, const ReadOnlyCompileOptions& options,
    const TranscodeRange& range, JS::MutableHandleScript scriptp) {
  auto decoder = js::MakeUnique<XDRDecoder>(cx, &options, range);
  if (!decoder) {
    ReportOutOfMemory(cx);
    return JS::TranscodeResult::Throw;
  }
  XDRResult res = decoder->codeScript(scriptp);
  MOZ_ASSERT(bool(scriptp) == res.isOk());
  if (res.isErr()) {
    return res.unwrapErr();
  }
  return JS::TranscodeResult::Ok;
}

JS_PUBLIC_API JS::TranscodeResult JS::DecodeScriptAndStartIncrementalEncoding(
    JSContext* cx, const ReadOnlyCompileOptions& options,
    TranscodeBuffer& buffer, JS::MutableHandleScript scriptp,
    size_t cursorIndex) {
  MOZ_DIAGNOSTIC_ASSERT(options.useStencilXDR);

  Rooted<frontend::CompilationInput> input(cx,
                                           frontend::CompilationInput(options));
  frontend::CompilationStencil stencil(nullptr);

  JS::TranscodeResult res =
      DecodeStencil(cx, buffer, input.get(), stencil, cursorIndex);
  if (res != JS::TranscodeResult::Ok) {
    return res;
  }

  Rooted<frontend::CompilationGCOutput> gcOutput(cx);
  if (!frontend::InstantiateStencils(cx, input.get(), stencil,
                                     gcOutput.get())) {
    return JS::TranscodeResult::Throw;
  }

  MOZ_ASSERT(gcOutput.get().script);

  auto initial =
      js::MakeUnique<frontend::ExtensibleCompilationStencil>(cx, input.get());
  if (!initial) {
    ReportOutOfMemory(cx);
    return JS::TranscodeResult::Throw;
  }
  if (!initial->steal(cx, std::move(stencil))) {
    return JS::TranscodeResult::Throw;
  }

  if (!gcOutput.get().script->scriptSource()->startIncrementalEncoding(
          cx, options, std::move(initial))) {
    return JS::TranscodeResult::Throw;
  }

  scriptp.set(gcOutput.get().script);

  return JS::TranscodeResult::Ok;
}

JS_PUBLIC_API bool JS::FinishIncrementalEncoding(JSContext* cx,
                                                 JS::HandleScript script,
                                                 TranscodeBuffer& buffer) {
  if (!script) {
    return false;
  }
  if (!script->scriptSource()->xdrFinalizeEncoder(cx, buffer)) {
    return false;
  }
  return true;
}

bool JS::IsWasmModuleObject(HandleObject obj) {
  return obj->canUnwrapAs<WasmModuleObject>();
}

JS_PUBLIC_API RefPtr<JS::WasmModule> JS::GetWasmModule(HandleObject obj) {
  MOZ_ASSERT(JS::IsWasmModuleObject(obj));
  WasmModuleObject& mobj = obj->unwrapAs<WasmModuleObject>();
  return const_cast<wasm::Module*>(&mobj.module());
}

bool JS::DisableWasmHugeMemory() { return wasm::DisableHugeMemory(); }

JS_PUBLIC_API void JS::SetProcessLargeAllocationFailureCallback(
    JS::LargeAllocationFailureCallback lafc) {
  MOZ_ASSERT(!OnLargeAllocationFailure);
  OnLargeAllocationFailure = lafc;
}

JS_PUBLIC_API void JS::SetOutOfMemoryCallback(JSContext* cx,
                                              OutOfMemoryCallback cb,
                                              void* data) {
  cx->runtime()->oomCallback = cb;
  cx->runtime()->oomCallbackData = data;
}

JS::FirstSubsumedFrame::FirstSubsumedFrame(
    JSContext* cx, bool ignoreSelfHostedFrames /* = true */)
    : JS::FirstSubsumedFrame(cx, cx->realm()->principals(),
                             ignoreSelfHostedFrames) {}

JS_PUBLIC_API bool JS::CaptureCurrentStack(
    JSContext* cx, JS::MutableHandleObject stackp,
    JS::StackCapture&& capture /* = JS::StackCapture(JS::AllFrames()) */) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  MOZ_RELEASE_ASSERT(cx->realm());

  Realm* realm = cx->realm();
  Rooted<SavedFrame*> frame(cx);
  if (!realm->savedStacks().saveCurrentStack(cx, &frame, std::move(capture))) {
    return false;
  }
  stackp.set(frame.get());
  return true;
}

JS_PUBLIC_API bool JS::IsAsyncStackCaptureEnabledForRealm(JSContext* cx) {
  if (!cx->options().asyncStack()) {
    return false;
  }

  return !cx->options().asyncStackCaptureDebuggeeOnly() ||
         cx->realm()->isDebuggee();
}

JS_PUBLIC_API bool JS::CopyAsyncStack(JSContext* cx,
                                      JS::HandleObject asyncStack,
                                      JS::HandleString asyncCause,
                                      JS::MutableHandleObject stackp,
                                      const Maybe<size_t>& maxFrameCount) {
  AssertHeapIsIdle();
  CHECK_THREAD(cx);
  MOZ_RELEASE_ASSERT(cx->realm());

  js::AssertObjectIsSavedFrameOrWrapper(cx, asyncStack);
  Realm* realm = cx->realm();
  Rooted<SavedFrame*> frame(cx);
  if (!realm->savedStacks().copyAsyncStack(cx, asyncStack, asyncCause, &frame,
                                           maxFrameCount)) {
    return false;
  }
  stackp.set(frame.get());
  return true;
}

JS_PUBLIC_API Zone* JS::GetObjectZone(JSObject* obj) { return obj->zone(); }

JS_PUBLIC_API Zone* JS::GetNurseryCellZone(gc::Cell* cell) {
  return cell->nurseryZone();
}

JS_PUBLIC_API JS::TraceKind JS::GCThingTraceKind(void* thing) {
  MOZ_ASSERT(thing);
  return static_cast<js::gc::Cell*>(thing)->getTraceKind();
}

JS_PUBLIC_API void js::SetStackFormat(JSContext* cx, js::StackFormat format) {
  cx->runtime()->setStackFormat(format);
}

JS_PUBLIC_API js::StackFormat js::GetStackFormat(JSContext* cx) {
  return cx->runtime()->stackFormat();
}

JS_PUBLIC_API JS::JSTimers JS::GetJSTimers(JSContext* cx) {
  return cx->realm()->timers;
}

namespace js {

JS_PUBLIC_API void NoteIntentionalCrash() {
#ifdef __linux__
  static bool* addr =
      reinterpret_cast<bool*>(dlsym(RTLD_DEFAULT, "gBreakpadInjectorEnabled"));
  if (addr) {
    *addr = false;
  }
#endif
}

#ifdef DEBUG
bool gSupportDifferentialTesting = false;
#endif  // DEBUG

}  // namespace js

#ifdef DEBUG

JS_PUBLIC_API void JS::SetSupportDifferentialTesting(bool value) {
  js::gSupportDifferentialTesting = value;
}

#endif  // DEBUG

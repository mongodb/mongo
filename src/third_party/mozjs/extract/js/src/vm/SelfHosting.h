/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_SelfHosting_h_
#define vm_SelfHosting_h_

#include "NamespaceImports.h"

#include "js/CallNonGenericMethod.h"
#include "js/RootingAPI.h"
#include "js/TypeDecls.h"

// [SMDOC] Self-hosted JS
//
// Self-hosted JS allows implementing a part of the JS engine using JavaScript.
//
// This allows implementing new feature easily, and also enables JIT
// compilation to achieve better performance, for example with higher order
// functions. Self-hosted functions can be inlined in, and optimized with, the
// JS caller functions.
//
// Self-hosted JS code is compiled into a stencil during the initialization of
// the engine, and each function is instantiated into each global on demand.
//
// Self-hosted JS has several differences between regular JavaScript code,
// for performance optimization, security, and some other reasons.
//
// # Always strict mode
//
// Unlike regular JavaScript, self-hosted JS code is always in strict mode.
//
// # Prohibited syntax
//
//   * Regular expression `/foo/` cannot be used
//   * `obj.method(...)` and `obj[method](...)` style call cannot be used.
//      See `callFunction` below
//   * Object literal cannot contain duplicate property names
//   * `yield*` cannot be used
//
// # No lazy parsing
//
// Self-hosted JS does not use lazy/syntax parsing: bytecode is generated
// eagerly for each function. However, we do instantiate the BaseScript lazily
// from the stencil for JSFunctions created for self-hosted built-ins. See
// `SelfHostedLazyScript` and `JSRuntime::selfHostedLazyScript`.
//
// # Extended function
//
// Functions with "$"-prefix in their name are allocated as extended function.
// See "SetCanonicalName" below.
//
// # Intrinsic helper functions
//
// Self-hosted JS has access to special functions that can interact with
// native code or internal representation of JS values and objects.
//
// See `intrinsic_functions` array in SelfHosting.cpp.
//
// # Stack Frame
//
// Stack frame inside self-hosted JS is hidden from Error.prototype.stack by
// default, to hide the internal from user code.
//
// During debugging self-hosted JS code, `MOZ_SHOW_ALL_JS_FRAMES` environment
// variable can be used to expose those frames
//
// # Debugger interaction
//
// Self-hosted JS is hidden from debugger, and no source notes or breakpoint
// is generated.
//
// Most function calls inside self-hosted JS are hidden from Debugger's
// `onNativeCall` hook, except for the following (see below for each):
//   * `callContentFunction`
//   * `constructContentFunction`
//   * `allowContentIter`
//   * `allowContentIterWith`
//
// # XDR cache
//
// Compiling self-hosted JS code takes some time.
// To improve the startup performance, the bytecode for self-hosted JS code
// can be saved as XDR, and used by other instance. This is used to speed up
// JS shell tests and Firefox content process startup.
//
// See `JSRuntime::initSelfHostingStencil` function.
//
// # Special functions
//
// Self-hosted JS code has special functions, to emit special bytecode
// sequence, or directly operate on internals:
//
//   callFunction(callee, thisV, arg0, ...)
//     Call `callee` function with `thisV` as "this" value, passing
//     arg0, ..., as arguments.
//     This is used when "this" value is not `undefined.
//
//     `obj.method(...)` syntax is forbidden in self-hosted JS, to avoid
//     accidentally exposing the internal, or allowing user code to modify the
//     behavior.
//
//     If the `callee` can be user-provided, `callContentFunction` must be
//     used instead.
//
//   callContentFunction(callee, thisV, arg0, ...)
//     Same as `callFunction`, but this must be used when calling possibly
//     user-provided functions, even if "this" value is `undefined`.
//
//     This exposes function calls to debuggers, using `JSOp::CallContent`
//     opcode.
//
//   constructContentFunction(callee, newTarget, arg0, ...)
//     Construct `callee` function using `newTarget` as `new.target`.
//     This must be used when constructing possibly user-provided functions.
//
//     This exposes constructs to debuggers, using `JSOp::NewContent` opcode.
//
//   allowContentIter(iterable)
//     Iteration such as for-of and spread on user-provided value is
//     prohibited inside self-hosted JS by default.
//
//     `allowContentIter` marks iteration allowed for given possibly
//     user-provided iterable.
//
//     This exposes implicit function calls around iteration to debuggers,
//     using `JSOp::CallContentIter` opcode.
//
//     Used in the following contexts:
//
//       for (var item of allowContentIter(iterable)) { ... }
//       [...allowContentIter(iterable)]
//
//   allowContentIterWith(iterable, iteratorFunc)
//     Special form of `allowContentIter`, where `iterable[Symbol.iterator]` is
//     already retrieved.
//
//     This directly uses `iteratorFunc` instead of accessing
//     `iterable[Symbol.iterator]` again inside for-of bytecode.
//
//     for (var item of allowContentIterWith(iterable, iteratorFunc)) { ... }
//
//   DefineDataProperty(obj, key, value)
//     Initialize `obj`'s `key` property with `value`, like
//    `Object.defineProperty(obj, key, {value})`, using `JSOp::InitElem`
//     opcode. This is almost always better than `obj[key] = value` because it
//     ignores setters and other properties on the prototype chain.
//
//   hasOwn(key, obj)
//     Return `true` if `obj` has an own `key` property, using `JSOp::HasOwn`
//     opcode.
//
//   getPropertySuper(obj, key, receiver)
//     Return `obj.[[Get]](key, receiver)`, using `JSOp::GetElemSuper` opcode.
//
//   ToNumeric(v)
//     Convert `v` to number, using `JSOp::ToNumeric` opcode
//
//   ToString(v)
//     Convert `v` to string, `JSOp::ToString` opcode
//
//   GetBuiltinConstructor(name)
//     Return built-in constructor for `name`, e.g. `"Array"`, using
//     `JSOp::BuiltinObject` opcode.
//
//   GetBuiltinPrototype(name)
//     Return built-in prototype for `name`, e.g. `"RegExp"`, using
//     `JSOp::BuiltinObject` opcode.
//
//   GetBuiltinSymbol(name)
//     Return built-in symbol `Symbol[name]`, using `JSOp::Symbol` opcode.
//
//   SetIsInlinableLargeFunction(fun)
//     Mark the large function `fun` inlineable.
//     `fun` must be the last function declaration before this call.
//
//   SetCanonicalName(fun)
//     Set canonical name for the function `fun`.
//     `fun` must be the last function declaration before this call, and also
//     its function name must be prefixed with "$", to make it extended
//     function and store the original function name in the extended slot.
//
//   UnsafeGetReservedSlot(obj, slot)
//   UnsafeGetObjectFromReservedSlot(obj, slot)
//   UnsafeGetInt32FromReservedSlot(obj, slot)
//   UnsafeGetStringFromReservedSlot(obj, slot)
//   UnsafeGetBooleanFromReservedSlot(obj, slot)
//     Get `obj`'s reserved slot specified by integer value `slot`.
//     They are intrinsic helper functions, and also optimized during JIT
//     compilation.
//
//   UnsafeSetReservedSlot(obj, slot, value)
//     Set `obj`'s reserved slot specified by integer value `slot` to `value`.
//     This is an intrinsic helper function, and also optimized during JIT
//     compilation.
//
//   resumeGenerator(gen, value, kind)
//     Resume generator `gen`, using `kind`, which is one of "next", "throw",
//     or "return", pasing `value` as parameter, using `JSOp::Resume` opcode.
//
//   forceInterpreter()
//     Force interpreter execution for this function, using
//     `JSOp::ForceInterpreter` opcode.
//     This must be the first statement inside the function.

namespace JS {
class JS_PUBLIC_API CompileOptions;
}

namespace js {

class AnyInvokeArgs;
class PropertyName;
class ScriptSourceObject;

ScriptSourceObject* SelfHostingScriptSourceObject(JSContext* cx);

/*
 * Check whether the given JSFunction or Value is a self-hosted function whose
 * self-hosted name is the given name.
 */
bool IsSelfHostedFunctionWithName(JSFunction* fun, JSAtom* name);
bool IsSelfHostedFunctionWithName(const Value& v, JSAtom* name);

/*
 * Returns the name of the cloned function's binding in the self-hosted global.
 *
 * This returns a non-null value only when this is a top level function
 * declaration in the self-hosted global.
 */
PropertyName* GetClonedSelfHostedFunctionName(const JSFunction* fun);
void SetClonedSelfHostedFunctionName(JSFunction* fun, PropertyName* name);

constexpr char ExtendedUnclonedSelfHostedFunctionNamePrefix = '$';

/*
 * Uncloned self-hosted functions with `$` prefix are allocated as
 * extended function, to store the original name in `_SetCanonicalName`.
 */
bool IsExtendedUnclonedSelfHostedFunctionName(JSAtom* name);

void SetUnclonedSelfHostedCanonicalName(JSFunction* fun, JSAtom* name);

bool IsCallSelfHostedNonGenericMethod(NativeImpl impl);

bool ReportIncompatibleSelfHostedMethod(JSContext* cx, Handle<Value> thisValue);

/* Get the compile options used when compiling self hosted code. */
void FillSelfHostingCompileOptions(JS::CompileOptions& options);

const JSFunctionSpec* FindIntrinsicSpec(PropertyName* name);

#ifdef DEBUG
/*
 * Calls a self-hosted function by name.
 *
 * This function is only available in debug mode, because it always atomizes
 * its |name| parameter. Use the alternative function below in non-debug code.
 */
bool CallSelfHostedFunction(JSContext* cx, char const* name, HandleValue thisv,
                            const AnyInvokeArgs& args, MutableHandleValue rval);
#endif

/*
 * Calls a self-hosted function by name.
 */
bool CallSelfHostedFunction(JSContext* cx, Handle<PropertyName*> name,
                            HandleValue thisv, const AnyInvokeArgs& args,
                            MutableHandleValue rval);

bool intrinsic_NewArrayIterator(JSContext* cx, unsigned argc, JS::Value* vp);

bool intrinsic_NewStringIterator(JSContext* cx, unsigned argc, JS::Value* vp);

bool intrinsic_NewRegExpStringIterator(JSContext* cx, unsigned argc,
                                       JS::Value* vp);

#ifdef ENABLE_RECORD_TUPLE
bool IsTupleUnchecked(JSContext* cx, const CallArgs& args);
bool intrinsic_IsTuple(JSContext* cx, unsigned argc, JS::Value* vp);
#endif

} /* namespace js */

#endif /* vm_SelfHosting_h_ */

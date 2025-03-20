/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_ABIFunctions_h
#define jit_ABIFunctions_h

#include "jstypes.h"  // JS_FUNC_TO_DATA_PTR

struct JS_PUBLIC_API JSContext;

namespace JS {
class JS_PUBLIC_API Value;
}

namespace js {
namespace jit {

// This class is used to ensure that all known targets of callWithABI are
// registered here. Otherwise, this would raise a static assertion at compile
// time.
template <typename Sig, Sig fun>
struct ABIFunctionData {
  static const bool registered = false;
};

template <typename Sig, Sig fun>
struct ABIFunction {
  void* address() const { return JS_FUNC_TO_DATA_PTR(void*, fun); }

  // If this assertion fails, you are likely in the context of a
  // `callWithABI<Sig, fn>()` call. This error indicates that ABIFunction has
  // not been specialized for `<Sig, fn>` by the time of this call.
  //
  // This can be fixed by adding the function signature to either
  // ABIFUNCTION_LIST or ABIFUNCTION_AND_TYPE_LIST (if overloaded) within
  // `ABIFunctionList-inl.h` and to add an `#include` statement of this header
  // in the file which is making the call to `callWithABI<Sig, fn>()`.
  static_assert(ABIFunctionData<Sig, fun>::registered,
                "ABI function is not registered.");
};

template <typename Sig>
struct ABIFunctionSignatureData {
  static const bool registered = false;
};

template <typename Sig>
struct ABIFunctionSignature {
  void* address(Sig fun) const { return JS_FUNC_TO_DATA_PTR(void*, fun); }

  // If this assertion fails, you are likely in the context of a
  // `DynamicFunction<Sig>(fn)` call. This error indicates that
  // ABIFunctionSignature has not been specialized for `Sig` by the time of this
  // call.
  //
  // This can be fixed by adding the function signature to ABIFUNCTIONSIG_LIST
  // within `ABIFunctionList-inl.h` and to add an `#include` statement of this
  // header in the file which is making the call to `DynamicFunction<Sig>(fn)`.
  static_assert(ABIFunctionSignatureData<Sig>::registered,
                "ABI function signature is not registered.");
};

// This is a structure created to ensure that the dynamically computed
// function pointer is well typed.
//
// It is meant to be created only through DynamicFunction function calls. In
// extremelly rare cases, such as VMFunctions, it might be produced as a result
// of GetVMFunctionTarget.
struct DynFn {
  void* address;
};

#ifdef JS_SIMULATOR
bool CallAnyNative(JSContext* cx, unsigned argc, JS::Value* vp);
const void* RedirectedCallAnyNative();
#endif

}  // namespace jit
}  // namespace js

#endif /* jit_VMFunctions_h */

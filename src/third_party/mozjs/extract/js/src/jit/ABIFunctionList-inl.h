/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_ABIFunctionList_inl_h
#define jit_ABIFunctionList_inl_h

#include "mozilla/MacroArgs.h"  // MOZ_CONCAT
#include "mozilla/SIMD.h"       // mozilla::SIMD::memchr{,2x}{8,16}

#include "jslibmath.h"  // js::NumberMod
#include "jsmath.h"     // js::ecmaPow, js::ecmaHypot, js::hypot3, js::hypot4,
                        // js::ecmaAtan2, js::UnaryMathFunctionType, js::powi
#include "jsnum.h"      // js::StringToNumberPure, js::Int32ToStringPure,
                        // js::NumberToStringPure

#include "builtin/Array.h"             // js::ArrayShiftMoveElements
#include "builtin/MapObject.h"         // js::MapIteratorObject::next,
                                       // js::SetIteratorObject::next
#include "builtin/Object.h"            // js::ObjectClassToString
#include "builtin/RegExp.h"            // js::RegExpPrototypeOptimizableRaw,
                                       // js::RegExpInstanceOptimizableRaw
#include "builtin/Sorting.h"           // js::ArraySortData
#include "builtin/TestingFunctions.h"  // js::FuzzilliHash*

#include "irregexp/RegExpAPI.h"
// js::irregexp::CaseInsensitiveCompareNonUnicode,
// js::irregexp::CaseInsensitiveCompareUnicode,
// js::irregexp::GrowBacktrackStack,
// js::irregexp::IsCharacterInRangeArray

#include "jit/ABIFunctions.h"
#include "jit/Bailouts.h"  // js::jit::FinishBailoutToBaseline, js::jit::Bailout,
                           // js::jit::InvalidationBailout

#include "jit/Ion.h"          // js::jit::LazyLinkTopActivation
#include "jit/JitFrames.h"    // HandleException
#include "jit/VMFunctions.h"  // Rest of js::jit::* functions.

#include "js/CallArgs.h"     // JSNative
#include "js/Conversions.h"  // JS::ToInt32
// JSJitGetterOp, JSJitSetterOp, JSJitMethodOp
#include "js/experimental/JitInfo.h"

#include "proxy/Proxy.h"  // js::ProxyGetProperty

#include "vm/ArgumentsObject.h"   // js::ArgumentsObject::finishForIonPure
#include "vm/Interpreter.h"       // js::TypeOfObject
#include "vm/NativeObject.h"      // js::NativeObject
#include "vm/RegExpShared.h"      // js::ExecuteRegExpAtomRaw
#include "vm/TypedArrayObject.h"  // js::TypedArraySortFromJit
#include "wasm/WasmBuiltins.h"    // js::wasm::*

#include "builtin/Boolean-inl.h"  // js::EmulatesUndefined

namespace js {

namespace wasm {

class AnyRef;

}  // namespace wasm

namespace jit {

// List of all ABI functions to be used with callWithABI. Each entry stores
// the fully qualified name of the C++ function. This list must be sorted.
#if JS_GC_PROBES
#  define ABIFUNCTION_JS_GC_PROBES_LIST(_) _(js::jit::TraceCreateObject)
#else
#  define ABIFUNCTION_JS_GC_PROBES_LIST(_)
#endif

#if defined(JS_CODEGEN_ARM)
#  define ABIFUNCTION_JS_CODEGEN_ARM_LIST(_) \
    _(__aeabi_idivmod)                       \
    _(__aeabi_uidivmod)
#else
#  define ABIFUNCTION_JS_CODEGEN_ARM_LIST(_)
#endif

#ifdef WASM_CODEGEN_DEBUG
#  define ABIFUNCTION_WASM_CODEGEN_DEBUG_LIST(_) \
    _(js::wasm::PrintF32)                        \
    _(js::wasm::PrintF64)                        \
    _(js::wasm::PrintI32)                        \
    _(js::wasm::PrintPtr)                        \
    _(js::wasm::PrintText)
#else
#  define ABIFUNCTION_WASM_CODEGEN_DEBUG_LIST(_)
#endif

#ifdef FUZZING_JS_FUZZILLI
#  define ABIFUNCTION_FUZZILLI_LIST(_) _(js::FuzzilliHashBigInt)
#else
#  define ABIFUNCTION_FUZZILLI_LIST(_)
#endif

#define ABIFUNCTION_LIST(_)                                                    \
  ABIFUNCTION_JS_GC_PROBES_LIST(_)                                             \
  ABIFUNCTION_JS_CODEGEN_ARM_LIST(_)                                           \
  ABIFUNCTION_WASM_CODEGEN_DEBUG_LIST(_)                                       \
  _(js::ArgumentsObject::finishForIonPure)                                     \
  _(js::ArgumentsObject::finishInlineForIonPure)                               \
  _(js::ArrayShiftMoveElements)                                                \
  _(js::ArraySortData::sortArrayWithComparator)                                \
  _(js::ArraySortData::sortTypedArrayWithComparator)                           \
  _(js::ArraySortFromJit)                                                      \
  _(js::ecmaAtan2)                                                             \
  _(js::ecmaHypot)                                                             \
  _(js::ecmaPow)                                                               \
  _(js::EmulatesUndefined)                                                     \
  _(js::EmulatesUndefinedCheckFuse)                                            \
  _(js::ExecuteRegExpAtomRaw)                                                  \
  _(js::hypot3)                                                                \
  _(js::hypot4)                                                                \
  _(js::Interpret)                                                             \
  _(js::Int32ToStringPure)                                                     \
  _(js::irregexp::CaseInsensitiveCompareNonUnicode)                            \
  _(js::irregexp::CaseInsensitiveCompareUnicode)                               \
  _(js::irregexp::GrowBacktrackStack)                                          \
  _(js::irregexp::IsCharacterInRangeArray)                                     \
  _(js::jit::AllocateAndInitTypedArrayBuffer)                                  \
  _(js::jit::AllocateBigIntNoGC)                                               \
  _(js::jit::AllocateFatInlineString)                                          \
  _(js::jit::AllocateDependentString)                                          \
  _(js::jit::AssertMapObjectHash)                                              \
  _(js::jit::AssertPropertyLookup)                                             \
  _(js::jit::AssertSetObjectHash)                                              \
  _(js::jit::AssertValidBigIntPtr)                                             \
  _(js::jit::AssertValidObjectPtr)                                             \
  _(js::jit::AssertValidStringPtr)                                             \
  _(js::jit::AssertValidSymbolPtr)                                             \
  _(js::jit::AssertValidValue)                                                 \
  _(js::jit::AssumeUnreachable)                                                \
  _(js::jit::AtomicsStore64)                                                   \
  _(js::jit::AtomizeStringNoGC)                                                \
  _(js::jit::Bailout)                                                          \
  _(js::jit::BaselineScript::OSREntryForFrame)                                 \
  _(js::jit::BigIntNumberEqual<js::jit::EqualityKind::Equal>)                  \
  _(js::jit::BigIntNumberEqual<js::jit::EqualityKind::NotEqual>)               \
  _(js::jit::BigIntNumberCompare<js::jit::ComparisonKind::LessThan>)           \
  _(js::jit::NumberBigIntCompare<js::jit::ComparisonKind::LessThan>)           \
  _(js::jit::NumberBigIntCompare<js::jit::ComparisonKind::GreaterThanOrEqual>) \
  _(js::jit::BigIntNumberCompare<js::jit::ComparisonKind::GreaterThanOrEqual>) \
  _(js::jit::DateFillLocalTimeSlots)                                           \
  _(js::jit::EqualStringsHelperPure)                                           \
  _(js::jit::FinishBailoutToBaseline)                                          \
  _(js::jit::Float16ToFloat32)                                                 \
  _(js::jit::Float32ToFloat16)                                                 \
  _(js::jit::FrameIsDebuggeeCheck)                                             \
  _(js::jit::GetContextSensitiveInterpreterStub)                               \
  _(js::jit::GetIndexFromString)                                               \
  _(js::jit::GetInt32FromStringPure)                                           \
  _(js::jit::GetNativeDataPropertyPure)                                        \
  _(js::jit::GetNativeDataPropertyPureWithCacheLookup)                         \
  _(js::jit::GetNativeDataPropertyByValuePure)                                 \
  _(js::jit::GlobalHasLiveOnDebuggerStatement)                                 \
  _(js::jit::HandleCodeCoverageAtPC)                                           \
  _(js::jit::HandleCodeCoverageAtPrologue)                                     \
  _(js::jit::HandleException)                                                  \
  _(js::jit::HasNativeDataPropertyPure<false>)                                 \
  _(js::jit::HasNativeDataPropertyPure<true>)                                  \
  _(js::jit::HasNativeElementPure)                                             \
  _(js::jit::InitBaselineFrameForOsr)                                          \
  _(js::jit::InvalidationBailout)                                              \
  _(js::jit::InvokeFromInterpreterStub)                                        \
  _(js::jit::LazyLinkTopActivation)                                            \
  _(js::jit::LinearizeForCharAccessPure)                                       \
  _(js::jit::ObjectHasGetterSetterPure)                                        \
  _(js::jit::ObjectIsCallable)                                                 \
  _(js::jit::ObjectIsConstructor)                                              \
  _(js::jit::PostGlobalWriteBarrier)                                           \
  _(js::jit::PostWriteBarrier)                                                 \
  _(js::jit::PostWriteElementBarrier)                                          \
  _(js::jit::Printf0)                                                          \
  _(js::jit::Printf1)                                                          \
  _(js::jit::StringFromCharCodeNoGC)                                           \
  _(js::jit::StringTrimEndIndex)                                               \
  _(js::jit::StringTrimStartIndex)                                             \
  _(js::jit::TypeOfNameObject)                                                 \
  _(js::jit::TypeOfEqObject)                                                   \
  _(js::jit::WrapObjectPure)                                                   \
  ABIFUNCTION_FUZZILLI_LIST(_)                                                 \
  _(js::MapIteratorObject::next)                                               \
  _(js::NativeObject::addDenseElementPure)                                     \
  _(js::NativeObject::growSlotsPure)                                           \
  _(js::NumberMod)                                                             \
  _(js::NumberToStringPure)                                                    \
  _(js::ObjectClassToString)                                                   \
  _(js::powi)                                                                  \
  _(js::ProxyGetProperty)                                                      \
  _(js::RoundFloat16)                                                          \
  _(js::SetIteratorObject::next)                                               \
  _(js::StringToNumberPure)                                                    \
  _(js::TypedArraySortFromJit)                                                 \
  _(js::TypeOfObject)                                                          \
  _(mozilla::SIMD::memchr16)                                                   \
  _(mozilla::SIMD::memchr2x16)                                                 \
  _(mozilla::SIMD::memchr2x8)                                                  \
  _(mozilla::SIMD::memchr8)

// List of all ABI functions to be used with callWithABI, which are
// overloaded. Each entry stores the fully qualified name of the C++ function,
// followed by the signature of the function to be called. When the function
// is not overloaded, you should prefer adding the function to
// ABIFUNCTION_LIST instead. This list must be sorted with the name of the C++
// function.
#define ABIFUNCTION_AND_TYPE_LIST(_)                    \
  _(JS::ToInt32, int32_t (*)(double))                   \
  _(js::jit::RoundFloat16ToFloat32, float (*)(int32_t)) \
  _(js::jit::RoundFloat16ToFloat32, float (*)(float))   \
  _(js::jit::RoundFloat16ToFloat32, float (*)(double))

// List of all ABI function signature which are using a computed function
// pointer instead of a statically known function pointer.
#define ABIFUNCTIONSIG_LIST(_)                      \
  _(AtomicsCompareExchangeFn)                       \
  _(AtomicsReadWriteModifyFn)                       \
  _(bool (*)(BigInt*, BigInt*))                     \
  _(bool (*)(BigInt*, double))                      \
  _(bool (*)(double, BigInt*))                      \
  _(float (*)(float))                               \
  _(JSJitGetterOp)                                  \
  _(JSJitMethodOp)                                  \
  _(JSJitSetterOp)                                  \
  _(JSNative)                                       \
  _(js::UnaryMathFunctionType)                      \
  _(void (*)(js::gc::StoreBuffer*, js::gc::Cell**)) \
  _(void (*)(JSRuntime * rt, JSObject * *objp))     \
  _(void (*)(JSRuntime * rt, JSString * *stringp))  \
  _(void (*)(JSRuntime * rt, Shape * *shapep))      \
  _(void (*)(JSRuntime * rt, wasm::AnyRef * refp))  \
  _(void (*)(JSRuntime * rt, Value * vp))

// GCC warns when the signature does not have matching attributes (for example
// [[nodiscard]]). Squelch this warning to avoid a GCC-only footgun.
#if MOZ_IS_GCC
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wignored-attributes"
#endif

#define DEF_TEMPLATE(fp)                      \
  template <>                                 \
  struct ABIFunctionData<decltype(&fp), fp> { \
    static constexpr bool registered = true;  \
  };
ABIFUNCTION_LIST(DEF_TEMPLATE)
#undef DEF_TEMPLATE

#define DEF_TEMPLATE(fp, ...)                \
  template <>                                \
  struct ABIFunctionData<__VA_ARGS__, fp> {  \
    static constexpr bool registered = true; \
  };
ABIFUNCTION_AND_TYPE_LIST(DEF_TEMPLATE)
#undef DEF_TEMPLATE

// Define a known list of function signatures.
#define DEF_TEMPLATE(...)                        \
  template <>                                    \
  struct ABIFunctionSignatureData<__VA_ARGS__> { \
    static constexpr bool registered = true;     \
  };
ABIFUNCTIONSIG_LIST(DEF_TEMPLATE)
#undef DEF_TEMPLATE

#if MOZ_IS_GCC
#  pragma GCC diagnostic pop
#endif

}  // namespace jit
}  // namespace js

// Make sure that all names are fully qualified (or at least, are resolvable
// within the toplevel namespace).
//
// Previously this was accomplished just by using `::fp` to force resolution
// within the toplevel namespace, but (1) that prevented using templated
// functions with more than one parameter (eg `void foo<T, U>`) because the
// macro split on the comma and wrapping it in parens doesn't work because
// `::(foo)` is invalid; and (2) that would only check the function name itself,
// not eg template parameters.
namespace check_fully_qualified {
#define CHECK_NS_VISIBILITY(fp)                               \
  [[maybe_unused]] static constexpr decltype(&fp) MOZ_CONCAT( \
      fp_, __COUNTER__) = nullptr;
ABIFUNCTION_LIST(CHECK_NS_VISIBILITY)
#undef CHECK_NS_VISIBILITY
}  // namespace check_fully_qualified

#endif  // jit_VMFunctionList_inl_h

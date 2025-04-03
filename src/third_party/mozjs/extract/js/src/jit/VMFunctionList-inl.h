/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_VMFunctionList_inl_h
#define jit_VMFunctionList_inl_h

#include "builtin/Eval.h"
#include "builtin/ModuleObject.h"  // js::GetOrCreateModuleMetaObject
#include "builtin/Object.h"        // js::ObjectCreateWithTemplate
#include "builtin/Promise.h"       // js::AsyncFunctionAwait
#include "builtin/RegExp.h"
#include "builtin/String.h"
#include "builtin/TestingFunctions.h"
#include "jit/BaselineIC.h"
#include "jit/Ion.h"
#include "jit/IonIC.h"
#include "jit/TrialInlining.h"
#include "jit/VMFunctions.h"
#include "vm/AsyncFunction.h"
#include "vm/AsyncIteration.h"
#include "vm/BigIntType.h"
#include "vm/BoundFunctionObject.h"
#include "vm/EqualityOperations.h"
#include "vm/Interpreter.h"
#include "vm/Iteration.h"
#include "vm/TypedArrayObject.h"

#include "jit/BaselineFrame-inl.h"
#include "vm/Interpreter-inl.h"

namespace js {

/*
 * Alternative name for the 'ToStringSlow' function. The VMFUNCTION_LIST in
 * VMFuncionList-inl.h cannot include any overloaded functions, so this name is
 * provided for use in that list. ('ToStringSlow' has an overload in
 * Conversions.h.)
 */
template <AllowGC allowGC>
inline JSString* ToStringSlowForVM(
    JSContext* cx, typename MaybeRooted<Value, allowGC>::HandleType arg) {
  return ToStringSlow<allowGC>(cx, arg);
}

namespace jit {

#ifdef FUZZING_JS_FUZZILLI
#  define VMFUNCTION_FUZZILLI_LIST(_)             \
    _(FuzzilliHashObject, js::FuzzilliHashObject) \
    _(FuzzilliHashObjectInl, js::FuzzilliHashObjectInl)
#else
#  define VMFUNCTION_FUZZILLI_LIST(_)
#endif

// List of all VM functions to be used with callVM. Each entry stores the name
// (must be unique, used for the VMFunctionId enum and profiling) and the C++
// function to be called. This list must be sorted on the name field.
#define VMFUNCTION_LIST(_)                                                     \
  _(AddOrUpdateSparseElementHelper, js::AddOrUpdateSparseElementHelper)        \
  _(AddSlotAndCallAddPropHook, js::AddSlotAndCallAddPropHook)                  \
  _(ArgumentsObjectCreateForInlinedIon,                                        \
    js::ArgumentsObject::createForInlinedIon)                                  \
  _(ArgumentsObjectCreateForIon, js::ArgumentsObject::createForIon)            \
  _(ArgumentsSliceDense, js::ArgumentsSliceDense)                              \
  _(ArrayConstructorOneArg, js::ArrayConstructorOneArg)                        \
  _(ArrayFromArgumentsObject, js::ArrayFromArgumentsObject)                    \
  _(ArrayJoin, js::jit::ArrayJoin)                                             \
  _(ArraySliceDense, js::ArraySliceDense)                                      \
  _(AsyncFunctionAwait, js::AsyncFunctionAwait)                                \
  _(AsyncFunctionResolve, js::AsyncFunctionResolve)                            \
  _(AtomicsAdd64, js::jit::AtomicsAdd64)                                       \
  _(AtomicsAnd64, js::jit::AtomicsAnd64)                                       \
  _(AtomicsCompareExchange64, js::jit::AtomicsCompareExchange64)               \
  _(AtomicsExchange64, js::jit::AtomicsExchange64)                             \
  _(AtomicsLoad64, js::jit::AtomicsLoad64)                                     \
  _(AtomicsOr64, js::jit::AtomicsOr64)                                         \
  _(AtomicsSub64, js::jit::AtomicsSub64)                                       \
  _(AtomicsXor64, js::jit::AtomicsXor64)                                       \
  _(AtomizeString, js::AtomizeString)                                          \
  _(BaselineCompileFromBaselineInterpreter,                                    \
    js::jit::BaselineCompileFromBaselineInterpreter)                           \
  _(BaselineDebugPrologue, js::jit::DebugPrologue)                             \
  _(BaselineGetFunctionThis, js::jit::BaselineGetFunctionThis)                 \
  _(BigIntAdd, JS::BigInt::add)                                                \
  _(BigIntAsIntN, js::jit::BigIntAsIntN)                                       \
  _(BigIntAsUintN, js::jit::BigIntAsUintN)                                     \
  _(BigIntBitAnd, JS::BigInt::bitAnd)                                          \
  _(BigIntBitNot, JS::BigInt::bitNot)                                          \
  _(BigIntBitOr, JS::BigInt::bitOr)                                            \
  _(BigIntBitXor, JS::BigInt::bitXor)                                          \
  _(BigIntDec, JS::BigInt::dec)                                                \
  _(BigIntDiv, JS::BigInt::div)                                                \
  _(BigIntInc, JS::BigInt::inc)                                                \
  _(BigIntLeftShift, JS::BigInt::lsh)                                          \
  _(BigIntMod, JS::BigInt::mod)                                                \
  _(BigIntMul, JS::BigInt::mul)                                                \
  _(BigIntNeg, JS::BigInt::neg)                                                \
  _(BigIntPow, JS::BigInt::pow)                                                \
  _(BigIntRightShift, JS::BigInt::rsh)                                         \
  _(BigIntStringEqual, js::jit::InstantiatedBigIntStringEqual)                 \
  _(BigIntStringGreaterThanOrEqual,                                            \
    js::jit::InstantiatedBigIntStringGreaterThanOrEqual)                       \
  _(BigIntStringLessThan, js::jit::InstantiatedBigIntStringLessThan)           \
  _(BigIntStringNotEqual, js::jit::InstantiatedBigIntStringNotEqual)           \
  _(BigIntSub, JS::BigInt::sub)                                                \
  _(BindVarOperation, js::BindVarOperation)                                    \
  _(BlockLexicalEnvironmentObjectCreateWithoutEnclosing,                       \
    js::BlockLexicalEnvironmentObject::createWithoutEnclosing)                 \
  _(BoxBoxableValue, js::wasm::BoxBoxableValue)                                \
  _(BoxNonStrictThis, js::BoxNonStrictThis)                                    \
  _(BuiltinObjectOperation, js::BuiltinObjectOperation)                        \
  _(CallDOMGetter, js::jit::CallDOMGetter)                                     \
  _(CallDOMSetter, js::jit::CallDOMSetter)                                     \
  _(CallNativeGetter, js::jit::CallNativeGetter)                               \
  _(CallNativeSetter, js::jit::CallNativeSetter)                               \
  _(CallObjectCreateWithShape, js::CallObject::createWithShape)                \
  _(CanSkipAwait, js::CanSkipAwait)                                            \
  _(CharCodeAt, js::jit::CharCodeAt)                                           \
  _(CheckClassHeritageOperation, js::CheckClassHeritageOperation)              \
  _(CheckOverRecursed, js::jit::CheckOverRecursed)                             \
  _(CheckOverRecursedBaseline, js::jit::CheckOverRecursedBaseline)             \
  _(CheckPrivateFieldOperation, js::CheckPrivateFieldOperation)                \
  _(ClassBodyLexicalEnvironmentObjectCreateWithoutEnclosing,                   \
    js::ClassBodyLexicalEnvironmentObject::createWithoutEnclosing)             \
  _(CloneRegExpObject, js::CloneRegExpObject)                                  \
  _(CloseIterOperation, js::CloseIterOperation)                                \
  _(ConcatStrings, js::ConcatStrings<CanGC>)                                   \
  _(CreateAsyncFromSyncIterator, js::CreateAsyncFromSyncIterator)              \
  _(CreateBigIntFromInt64, js::jit::CreateBigIntFromInt64)                     \
  _(CreateBigIntFromUint64, js::jit::CreateBigIntFromUint64)                   \
  _(CreateGenerator, js::jit::CreateGenerator)                                 \
  _(CreateGeneratorFromFrame, js::jit::CreateGeneratorFromFrame)               \
  _(CreateThisFromIC, js::jit::CreateThisFromIC)                               \
  _(CreateThisFromIon, js::jit::CreateThisFromIon)                             \
  _(DebugAfterYield, js::jit::DebugAfterYield)                                 \
  _(DebugEpilogueOnBaselineReturn, js::jit::DebugEpilogueOnBaselineReturn)     \
  _(DebugLeaveLexicalEnv, js::jit::DebugLeaveLexicalEnv)                       \
  _(DebugLeaveThenFreshenLexicalEnv, js::jit::DebugLeaveThenFreshenLexicalEnv) \
  _(DebugLeaveThenPopLexicalEnv, js::jit::DebugLeaveThenPopLexicalEnv)         \
  _(DebugLeaveThenRecreateLexicalEnv,                                          \
    js::jit::DebugLeaveThenRecreateLexicalEnv)                                 \
  _(Debug_CheckSelfHosted, js::Debug_CheckSelfHosted)                          \
  _(DelElemOperationNonStrict, js::DelElemOperation<false>)                    \
  _(DelElemOperationStrict, js::DelElemOperation<true>)                        \
  _(DelPropOperationNonStrict, js::DelPropOperation<false>)                    \
  _(DelPropOperationStrict, js::DelPropOperation<true>)                        \
  _(DeleteNameOperation, js::DeleteNameOperation)                              \
  _(DoCallFallback, js::jit::DoCallFallback)                                   \
  _(DoConcatStringObject, js::jit::DoConcatStringObject)                       \
  _(DoSpreadCallFallback, js::jit::DoSpreadCallFallback)                       \
  _(DoStringToInt64, js::jit::DoStringToInt64)                                 \
  _(DoTrialInlining, js::jit::DoTrialInlining)                                 \
  _(EnterWith, js::jit::EnterWith)                                             \
  _(ExtractAwaitValue, js::ExtractAwaitValue)                                  \
  _(FinalSuspend, js::jit::FinalSuspend)                                       \
  _(FreshenLexicalEnv, js::jit::FreshenLexicalEnv)                             \
  _(FunWithProtoOperation, js::FunWithProtoOperation)                          \
  _(FunctionBindCreate, js::BoundFunctionObject::createWithTemplate)           \
  _(FunctionBindImpl, js::BoundFunctionObject::functionBindImpl)               \
  _(FunctionBindSpecializedBaseline,                                           \
    js::BoundFunctionObject::functionBindSpecializedBaseline)                  \
  VMFUNCTION_FUZZILLI_LIST(_)                                                  \
  _(GeneratorThrowOrReturn, js::jit::GeneratorThrowOrReturn)                   \
  _(GetAndClearException, js::GetAndClearException)                            \
  _(GetFirstDollarIndexRaw, js::GetFirstDollarIndexRaw)                        \
  _(GetImportOperation, js::GetImportOperation)                                \
  _(GetIntrinsicValue, js::jit::GetIntrinsicValue)                             \
  _(GetIterator, js::GetIterator)                                              \
  _(GetIteratorWithIndices, js::GetIteratorWithIndices)                        \
  _(GetNonSyntacticGlobalThis, js::GetNonSyntacticGlobalThis)                  \
  _(GetOrCreateModuleMetaObject, js::GetOrCreateModuleMetaObject)              \
  _(GetPrototypeOf, js::jit::GetPrototypeOf)                                   \
  _(GetSparseElementHelper, js::GetSparseElementHelper)                        \
  _(GlobalDeclInstantiationFromIon, js::jit::GlobalDeclInstantiationFromIon)   \
  _(GlobalOrEvalDeclInstantiation, js::GlobalOrEvalDeclInstantiation)          \
  _(HandleDebugTrap, js::jit::HandleDebugTrap)                                 \
  _(ImplicitThisOperation, js::ImplicitThisOperation)                          \
  _(ImportMetaOperation, js::ImportMetaOperation)                              \
  _(InitElemGetterSetterOperation, js::InitElemGetterSetterOperation)          \
  _(InitFunctionEnvironmentObjects, js::jit::InitFunctionEnvironmentObjects)   \
  _(InitPropGetterSetterOperation, js::InitPropGetterSetterOperation)          \
  _(InitRestParameter, js::jit::InitRestParameter)                             \
  _(Int32ToString, js::Int32ToString<CanGC>)                                   \
  _(Int32ToStringWithBase, js::Int32ToStringWithBase)                          \
  _(InterpretResume, js::jit::InterpretResume)                                 \
  _(InterruptCheck, js::jit::InterruptCheck)                                   \
  _(InvokeFunction, js::jit::InvokeFunction)                                   \
  _(IonBinaryArithICUpdate, js::jit::IonBinaryArithIC::update)                 \
  _(IonBindNameICUpdate, js::jit::IonBindNameIC::update)                       \
  _(IonCheckPrivateFieldICUpdate, js::jit::IonCheckPrivateFieldIC::update)     \
  _(IonCloseIterICUpdate, js::jit::IonCloseIterIC::update)                     \
  _(IonCompareICUpdate, js::jit::IonCompareIC::update)                         \
  _(IonCompileScriptForBaselineAtEntry,                                        \
    js::jit::IonCompileScriptForBaselineAtEntry)                               \
  _(IonCompileScriptForBaselineOSR, js::jit::IonCompileScriptForBaselineOSR)   \
  _(IonGetIteratorICUpdate, js::jit::IonGetIteratorIC::update)                 \
  _(IonGetNameICUpdate, js::jit::IonGetNameIC::update)                         \
  _(IonGetPropSuperICUpdate, js::jit::IonGetPropSuperIC::update)               \
  _(IonGetPropertyICUpdate, js::jit::IonGetPropertyIC::update)                 \
  _(IonHasOwnICUpdate, js::jit::IonHasOwnIC::update)                           \
  _(IonInICUpdate, js::jit::IonInIC::update)                                   \
  _(IonInstanceOfICUpdate, js::jit::IonInstanceOfIC::update)                   \
  _(IonOptimizeSpreadCallICUpdate, js::jit::IonOptimizeSpreadCallIC::update)   \
  _(IonSetPropertyICUpdate, js::jit::IonSetPropertyIC::update)                 \
  _(IonToPropertyKeyICUpdate, js::jit::IonToPropertyKeyIC::update)             \
  _(IonUnaryArithICUpdate, js::jit::IonUnaryArithIC::update)                   \
  _(IsArrayFromJit, js::IsArrayFromJit)                                        \
  _(IsPossiblyWrappedTypedArray, js::jit::IsPossiblyWrappedTypedArray)         \
  _(IsPrototypeOf, js::IsPrototypeOf)                                          \
  _(Lambda, js::Lambda)                                                        \
  _(LeaveWith, js::jit::LeaveWith)                                             \
  _(LinearizeForCharAccess, js::jit::LinearizeForCharAccess)                   \
  _(LoadAliasedDebugVar, js::LoadAliasedDebugVar)                              \
  _(MapObjectGet, js::jit::MapObjectGet)                                       \
  _(MapObjectHas, js::jit::MapObjectHas)                                       \
  _(MutatePrototype, js::jit::MutatePrototype)                                 \
  _(NamedLambdaObjectCreateWithoutEnclosing,                                   \
    js::NamedLambdaObject::createWithoutEnclosing)                             \
  _(NativeGetElement, js::NativeGetElement)                                    \
  _(NewArgumentsObject, js::jit::NewArgumentsObject)                           \
  _(NewArrayIterator, js::NewArrayIterator)                                    \
  _(NewArrayObjectBaselineFallback, js::NewArrayObjectBaselineFallback)        \
  _(NewArrayObjectEnsureDenseInitLength,                                       \
    js::jit::NewArrayObjectEnsureDenseInitLength)                              \
  _(NewArrayObjectOptimzedFallback, js::NewArrayObjectOptimizedFallback)       \
  _(NewArrayOperation, js::NewArrayOperation)                                  \
  _(NewArrayWithShape, js::NewArrayWithShape)                                  \
  _(NewObjectOperation, js::NewObjectOperation)                                \
  _(NewPlainObjectBaselineFallback, js::NewPlainObjectBaselineFallback)        \
  _(NewPlainObjectOptimizedFallback, js::NewPlainObjectOptimizedFallback)      \
  _(NewPrivateName, js::NewPrivateName)                                        \
  _(NewRegExpStringIterator, js::NewRegExpStringIterator)                      \
  _(NewStringIterator, js::NewStringIterator)                                  \
  _(NewStringObject, js::jit::NewStringObject)                                 \
  _(NewTypedArrayWithTemplateAndArray, js::NewTypedArrayWithTemplateAndArray)  \
  _(NewTypedArrayWithTemplateAndBuffer,                                        \
    js::NewTypedArrayWithTemplateAndBuffer)                                    \
  _(NewTypedArrayWithTemplateAndLength,                                        \
    js::NewTypedArrayWithTemplateAndLength)                                    \
  _(NormalSuspend, js::jit::NormalSuspend)                                     \
  _(NumberParseInt, js::NumberParseInt)                                        \
  _(NumberToString, js::NumberToString<CanGC>)                                 \
  _(ObjectCreateWithTemplate, js::ObjectCreateWithTemplate)                    \
  _(ObjectWithProtoOperation, js::ObjectWithProtoOperation)                    \
  _(OnDebuggerStatement, js::jit::OnDebuggerStatement)                         \
  _(ProxyGetProperty, js::ProxyGetProperty)                                    \
  _(ProxyGetPropertyByValue, js::ProxyGetPropertyByValue)                      \
  _(ProxyHas, js::ProxyHas)                                                    \
  _(ProxyHasOwn, js::ProxyHasOwn)                                              \
  _(ProxySetProperty, js::ProxySetProperty)                                    \
  _(ProxySetPropertyByValue, js::ProxySetPropertyByValue)                      \
  _(PushClassBodyEnv, js::jit::PushClassBodyEnv)                               \
  _(PushLexicalEnv, js::jit::PushLexicalEnv)                                   \
  _(PushVarEnv, js::jit::PushVarEnv)                                           \
  _(RecreateLexicalEnv, js::jit::RecreateLexicalEnv)                           \
  _(RegExpBuiltinExecMatchFromJit, js::RegExpBuiltinExecMatchFromJit)          \
  _(RegExpBuiltinExecTestFromJit, js::RegExpBuiltinExecTestFromJit)            \
  _(RegExpMatcherRaw, js::RegExpMatcherRaw)                                    \
  _(RegExpSearcherRaw, js::RegExpSearcherRaw)                                  \
  _(SameValue, js::SameValue)                                                  \
  _(SetArrayLength, js::jit::SetArrayLength)                                   \
  _(SetElementMegamorphicNoCache, js::jit::SetElementMegamorphic<false>)       \
  _(SetElementMegamorphicYesCache, js::jit::SetElementMegamorphic<true>)       \
  _(SetElementSuper, js::SetElementSuper)                                      \
  _(SetFunctionName, js::SetFunctionName)                                      \
  _(SetIntrinsicOperation, js::SetIntrinsicOperation)                          \
  _(SetObjectHas, js::jit::SetObjectHas)                                       \
  _(SetPropertyMegamorphicNoCache, js::jit::SetPropertyMegamorphic<false>)     \
  _(SetPropertyMegamorphicYesCache, js::jit::SetPropertyMegamorphic<true>)     \
  _(SetPropertySuper, js::SetPropertySuper)                                    \
  _(StartDynamicModuleImport, js::StartDynamicModuleImport)                    \
  _(StringBigIntGreaterThanOrEqual,                                            \
    js::jit::InstantiatedStringBigIntGreaterThanOrEqual)                       \
    _(StringBigIntLessThan, js::jit::InstantiatedStringBigIntLessThan)           \
  _(StringEndsWith, js::StringEndsWith)                                        \
  _(StringFlatReplaceString, js::StringFlatReplaceString)                      \
  _(StringFromCharCode, js::jit::StringFromCharCode)                           \
  _(StringFromCodePoint, js::jit::StringFromCodePoint)                         \
  _(StringIndexOf, js::StringIndexOf)                                          \
  _(StringReplace, js::jit::StringReplace)                                     \
  _(StringSplitString, js::StringSplitString)                                  \
  _(StringStartsWith, js::StringStartsWith)                                    \
  _(StringToLowerCase, js::StringToLowerCase)                                  \
  _(StringToNumber, js::StringToNumber)                                        \
  _(StringToUpperCase, js::StringToUpperCase)                                  \
  _(StringsCompareGreaterThanOrEquals,                                         \
    js::jit::StringsCompare<ComparisonKind::GreaterThanOrEqual>)               \
  _(StringsCompareLessThan, js::jit::StringsCompare<ComparisonKind::LessThan>) \
  _(StringsEqual, js::jit::StringsEqual<js::jit::EqualityKind::Equal>)         \
  _(StringsNotEqual, js::jit::StringsEqual<js::jit::EqualityKind::NotEqual>)   \
  _(SubstringKernel, js::SubstringKernel)                                      \
  _(ThrowBadDerivedReturnOrUninitializedThis,                                  \
    js::jit::ThrowBadDerivedReturnOrUninitializedThis)                         \
  _(ThrowCheckIsObject, js::ThrowCheckIsObject)                                \
  _(ThrowInitializedThis, js::ThrowInitializedThis)                            \
  _(ThrowMsgOperation, js::ThrowMsgOperation)                                  \
  _(ThrowObjectCoercible, js::ThrowObjectCoercible)                            \
  _(ThrowOperation, js::ThrowOperation)                                        \
  _(ThrowRuntimeLexicalError, js::jit::ThrowRuntimeLexicalError)               \
  _(ThrowUninitializedThis, js::ThrowUninitializedThis)                        \
  _(ToBigInt, js::ToBigInt)                                                    \
  _(ToStringSlow, js::ToStringSlowForVM<CanGC>)                                     \
  _(ValueToIterator, js::ValueToIterator)                                      \
  _(VarEnvironmentObjectCreateWithoutEnclosing,                                \
    js::VarEnvironmentObject::createWithoutEnclosing)

// The list below is for tail calls. The third argument specifies the number of
// non-argument Values the VM wrapper should pop from the stack. This is used
// for Baseline ICs.
//
// This list is required to be alphabetized.
#define TAIL_CALL_VMFUNCTION_LIST(_)                                        \
  _(DoBinaryArithFallback, js::jit::DoBinaryArithFallback, 2)               \
  _(DoBindNameFallback, js::jit::DoBindNameFallback, 0)                     \
  _(DoCheckPrivateFieldFallback, js::jit::DoCheckPrivateFieldFallback, 2)   \
  _(DoCloseIterFallback, js::jit::DoCloseIterFallback, 0)                   \
  _(DoCompareFallback, js::jit::DoCompareFallback, 2)                       \
  _(DoGetElemFallback, js::jit::DoGetElemFallback, 2)                       \
  _(DoGetElemSuperFallback, js::jit::DoGetElemSuperFallback, 3)             \
  _(DoGetIntrinsicFallback, js::jit::DoGetIntrinsicFallback, 0)             \
  _(DoGetIteratorFallback, js::jit::DoGetIteratorFallback, 1)               \
  _(DoGetNameFallback, js::jit::DoGetNameFallback, 0)                       \
  _(DoGetPropFallback, js::jit::DoGetPropFallback, 1)                       \
  _(DoGetPropSuperFallback, js::jit::DoGetPropSuperFallback, 0)             \
  _(DoHasOwnFallback, js::jit::DoHasOwnFallback, 2)                         \
  _(DoInFallback, js::jit::DoInFallback, 2)                                 \
  _(DoInstanceOfFallback, js::jit::DoInstanceOfFallback, 2)                 \
  _(DoNewArrayFallback, js::jit::DoNewArrayFallback, 0)                     \
  _(DoNewObjectFallback, js::jit::DoNewObjectFallback, 0)                   \
  _(DoOptimizeSpreadCallFallback, js::jit::DoOptimizeSpreadCallFallback, 0) \
  _(DoRestFallback, js::jit::DoRestFallback, 0)                             \
  _(DoSetElemFallback, js::jit::DoSetElemFallback, 2)                       \
  _(DoSetPropFallback, js::jit::DoSetPropFallback, 1)                       \
  _(DoToBoolFallback, js::jit::DoToBoolFallback, 0)                         \
  _(DoToPropertyKeyFallback, js::jit::DoToPropertyKeyFallback, 0)           \
  _(DoTypeOfFallback, js::jit::DoTypeOfFallback, 0)                         \
  _(DoUnaryArithFallback, js::jit::DoUnaryArithFallback, 1)

#define DEF_ID(name, ...) name,
enum class VMFunctionId { VMFUNCTION_LIST(DEF_ID) Count };
enum class TailCallVMFunctionId { TAIL_CALL_VMFUNCTION_LIST(DEF_ID) Count };
#undef DEF_ID

// Define the VMFunctionToId template to map from signature + function to
// the VMFunctionId. This lets us verify the consumer/codegen code matches
// the C++ signature.
template <typename Function, Function fun>
struct VMFunctionToId;  // Error here? Update VMFUNCTION_LIST?

template <typename Function, Function fun>
struct TailCallVMFunctionToId;  // Error here? Update TAIL_CALL_VMFUNCTION_LIST?

// GCC warns when the signature does not have matching attributes (for example
// [[nodiscard]]). Squelch this warning to avoid a GCC-only footgun.
#if MOZ_IS_GCC
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wignored-attributes"
#endif

// Note: the use of ::fp instead of fp is intentional to enforce use of
// fully-qualified names in the list above.
#define DEF_TEMPLATE(name, fp)                             \
  template <>                                              \
  struct VMFunctionToId<decltype(&(::fp)), ::fp> {         \
    static constexpr VMFunctionId id = VMFunctionId::name; \
  };
VMFUNCTION_LIST(DEF_TEMPLATE)
#undef DEF_TEMPLATE

#define DEF_TEMPLATE(name, fp, valuesToPop)                                \
  template <>                                                              \
  struct TailCallVMFunctionToId<decltype(&(::fp)), ::fp> {                 \
    static constexpr TailCallVMFunctionId id = TailCallVMFunctionId::name; \
  };
TAIL_CALL_VMFUNCTION_LIST(DEF_TEMPLATE)
#undef DEF_TEMPLATE

#if MOZ_IS_GCC
#  pragma GCC diagnostic pop
#endif

}  // namespace jit
}  // namespace js

#endif  // jit_VMFunctionList_inl_h

/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_MOpcodes_h
#define jit_MOpcodes_h

namespace js {
namespace jit {

#define MIR_OPCODE_LIST(_)                                                  \
    _(Constant)                                                             \
    _(SimdBox)                                                              \
    _(SimdUnbox)                                                            \
    _(SimdValueX4)                                                          \
    _(SimdSplat)                                                            \
    _(SimdConstant)                                                         \
    _(SimdConvert)                                                          \
    _(SimdReinterpretCast)                                                  \
    _(SimdExtractElement)                                                   \
    _(SimdInsertElement)                                                    \
    _(SimdSwizzle)                                                          \
    _(SimdGeneralShuffle)                                                   \
    _(SimdShuffle)                                                          \
    _(SimdUnaryArith)                                                       \
    _(SimdBinaryComp)                                                       \
    _(SimdBinaryArith)                                                      \
    _(SimdBinarySaturating)                                                 \
    _(SimdBinaryBitwise)                                                    \
    _(SimdShift)                                                            \
    _(SimdSelect)                                                           \
    _(SimdAllTrue)                                                          \
    _(SimdAnyTrue)                                                          \
    _(CloneLiteral)                                                         \
    _(Parameter)                                                            \
    _(Callee)                                                               \
    _(IsConstructing)                                                       \
    _(TableSwitch)                                                          \
    _(Goto)                                                                 \
    _(Test)                                                                 \
    _(GotoWithFake)                                                         \
    _(ObjectGroupDispatch)                                                  \
    _(FunctionDispatch)                                                     \
    _(Compare)                                                              \
    _(SameValue)                                                            \
    _(Phi)                                                                  \
    _(Beta)                                                                 \
    _(NaNToZero)                                                            \
    _(OsrValue)                                                             \
    _(OsrEnvironmentChain)                                                  \
    _(OsrReturnValue)                                                       \
    _(OsrArgumentsObject)                                                   \
    _(ReturnFromCtor)                                                       \
    _(BinarySharedStub)                                                     \
    _(UnarySharedStub)                                                      \
    _(NullarySharedStub)                                                    \
    _(CheckOverRecursed)                                                    \
    _(DefVar)                                                               \
    _(DefLexical)                                                           \
    _(DefFun)                                                               \
    _(CreateThis)                                                           \
    _(CreateThisWithProto)                                                  \
    _(CreateThisWithTemplate)                                               \
    _(CreateArgumentsObject)                                                \
    _(GetArgumentsObjectArg)                                                \
    _(SetArgumentsObjectArg)                                                \
    _(ComputeThis)                                                          \
    _(ImplicitThis)                                                         \
    _(Call)                                                                 \
    _(ApplyArgs)                                                            \
    _(ApplyArray)                                                           \
    _(Bail)                                                                 \
    _(Unreachable)                                                          \
    _(EncodeSnapshot)                                                       \
    _(AssertFloat32)                                                        \
    _(AssertRecoveredOnBailout)                                             \
    _(GetDynamicName)                                                       \
    _(CallDirectEval)                                                       \
    _(BitNot)                                                               \
    _(TypeOf)                                                               \
    _(ToAsync)                                                              \
    _(ToAsyncGen)                                                           \
    _(ToAsyncIter)                                                          \
    _(ToId)                                                                 \
    _(BitAnd)                                                               \
    _(BitOr)                                                                \
    _(BitXor)                                                               \
    _(Lsh)                                                                  \
    _(Rsh)                                                                  \
    _(Ursh)                                                                 \
    _(SignExtendInt32)                                                      \
    _(SignExtendInt64)                                                      \
    _(MinMax)                                                               \
    _(Abs)                                                                  \
    _(Clz)                                                                  \
    _(Ctz)                                                                  \
    _(Popcnt)                                                               \
    _(Sqrt)                                                                 \
    _(Atan2)                                                                \
    _(Hypot)                                                                \
    _(Pow)                                                                  \
    _(PowHalf)                                                              \
    _(Random)                                                               \
    _(MathFunction)                                                         \
    _(Add)                                                                  \
    _(Sub)                                                                  \
    _(Mul)                                                                  \
    _(Div)                                                                  \
    _(Mod)                                                                  \
    _(Concat)                                                               \
    _(CharCodeAt)                                                           \
    _(FromCharCode)                                                         \
    _(FromCodePoint)                                                        \
    _(StringConvertCase)                                                    \
    _(SinCos)                                                               \
    _(StringSplit)                                                          \
    _(Substr)                                                               \
    _(Return)                                                               \
    _(Throw)                                                                \
    _(Box)                                                                  \
    _(Unbox)                                                                \
    _(GuardObject)                                                          \
    _(GuardString)                                                          \
    _(PolyInlineGuard)                                                      \
    _(AssertRange)                                                          \
    _(ToDouble)                                                             \
    _(ToFloat32)                                                            \
    _(ToNumberInt32)                                                        \
    _(TruncateToInt32)                                                      \
    _(WrapInt64ToInt32)                                                     \
    _(ExtendInt32ToInt64)                                                   \
    _(Int64ToFloatingPoint)                                                 \
    _(ToString)                                                             \
    _(ToObject)                                                             \
    _(ToObjectOrNull)                                                       \
    _(NewArray)                                                             \
    _(NewArrayCopyOnWrite)                                                  \
    _(NewArrayDynamicLength)                                                \
    _(NewIterator)                                                          \
    _(NewTypedArray)                                                        \
    _(NewTypedArrayDynamicLength)                                           \
    _(NewObject)                                                            \
    _(NewTypedObject)                                                       \
    _(NewNamedLambdaObject)                                                 \
    _(NewCallObject)                                                        \
    _(NewSingletonCallObject)                                               \
    _(NewStringObject)                                                      \
    _(ObjectState)                                                          \
    _(ArrayState)                                                           \
    _(ArgumentState)                                                        \
    _(InitElem)                                                             \
    _(InitElemGetterSetter)                                                 \
    _(MutateProto)                                                          \
    _(InitPropGetterSetter)                                                 \
    _(Start)                                                                \
    _(OsrEntry)                                                             \
    _(Nop)                                                                  \
    _(LimitedTruncate)                                                      \
    _(RegExp)                                                               \
    _(RegExpMatcher)                                                        \
    _(RegExpSearcher)                                                       \
    _(RegExpTester)                                                         \
    _(RegExpPrototypeOptimizable)                                           \
    _(RegExpInstanceOptimizable)                                            \
    _(GetFirstDollarIndex)                                                  \
    _(StringReplace)                                                        \
    _(ClassConstructor)                                                     \
    _(Lambda)                                                               \
    _(LambdaArrow)                                                          \
    _(SetFunName)                                                           \
    _(KeepAliveObject)                                                      \
    _(Slots)                                                                \
    _(Elements)                                                             \
    _(ConstantElements)                                                     \
    _(ConvertElementsToDoubles)                                             \
    _(MaybeToDoubleElement)                                                 \
    _(MaybeCopyElementsForWrite)                                            \
    _(LoadSlot)                                                             \
    _(StoreSlot)                                                            \
    _(FunctionEnvironment)                                                  \
    _(NewLexicalEnvironmentObject)                                          \
    _(CopyLexicalEnvironmentObject)                                         \
    _(HomeObject)                                                           \
    _(HomeObjectSuperBase)                                                  \
    _(FilterTypeSet)                                                        \
    _(TypeBarrier)                                                          \
    _(PostWriteBarrier)                                                     \
    _(PostWriteElementBarrier)                                              \
    _(GetPropSuperCache)                                                    \
    _(GetPropertyCache)                                                     \
    _(GetPropertyPolymorphic)                                               \
    _(SetPropertyPolymorphic)                                               \
    _(BindNameCache)                                                        \
    _(CallBindVar)                                                          \
    _(GuardShape)                                                           \
    _(GuardReceiverPolymorphic)                                             \
    _(GuardObjectGroup)                                                     \
    _(GuardObjectIdentity)                                                  \
    _(GuardUnboxedExpando)                                                  \
    _(LoadUnboxedExpando)                                                   \
    _(ArrayLength)                                                          \
    _(SetArrayLength)                                                       \
    _(GetNextEntryForIterator)                                              \
    _(TypedArrayLength)                                                     \
    _(TypedArrayElements)                                                   \
    _(SetDisjointTypedElements)                                             \
    _(TypedObjectDescr)                                                     \
    _(TypedObjectElements)                                                  \
    _(SetTypedObjectOffset)                                                 \
    _(InitializedLength)                                                    \
    _(SetInitializedLength)                                                 \
    _(Not)                                                                  \
    _(BoundsCheck)                                                          \
    _(BoundsCheckLower)                                                     \
    _(SpectreMaskIndex)                                                     \
    _(InArray)                                                              \
    _(LoadElement)                                                          \
    _(LoadElementHole)                                                      \
    _(LoadUnboxedScalar)                                                    \
    _(LoadUnboxedObjectOrNull)                                              \
    _(LoadUnboxedString)                                                    \
    _(LoadElementFromState)                                                 \
    _(StoreElement)                                                         \
    _(StoreElementHole)                                                     \
    _(FallibleStoreElement)                                                 \
    _(StoreUnboxedScalar)                                                   \
    _(StoreUnboxedObjectOrNull)                                             \
    _(StoreUnboxedString)                                                   \
    _(ConvertUnboxedObjectToNative)                                         \
    _(ArrayPopShift)                                                        \
    _(ArrayPush)                                                            \
    _(ArraySlice)                                                           \
    _(ArrayJoin)                                                            \
    _(LoadTypedArrayElementHole)                                            \
    _(StoreTypedArrayElementHole)                                           \
    _(AtomicIsLockFree)                                                     \
    _(GuardSharedTypedArray)                                                \
    _(CompareExchangeTypedArrayElement)                                     \
    _(AtomicExchangeTypedArrayElement)                                      \
    _(AtomicTypedArrayElementBinop)                                         \
    _(EffectiveAddress)                                                     \
    _(ClampToUint8)                                                         \
    _(LoadFixedSlot)                                                        \
    _(LoadFixedSlotAndUnbox)                                                \
    _(StoreFixedSlot)                                                       \
    _(CallGetProperty)                                                      \
    _(GetNameCache)                                                         \
    _(CallGetIntrinsicValue)                                                \
    _(CallGetElement)                                                       \
    _(CallSetElement)                                                       \
    _(CallSetProperty)                                                      \
    _(CallInitElementArray)                                                 \
    _(DeleteProperty)                                                       \
    _(DeleteElement)                                                        \
    _(SetPropertyCache)                                                     \
    _(GetIteratorCache)                                                     \
    _(IteratorMore)                                                         \
    _(IsNoIter)                                                             \
    _(IteratorEnd)                                                          \
    _(StringLength)                                                         \
    _(ArgumentsLength)                                                      \
    _(GetFrameArgument)                                                     \
    _(SetFrameArgument)                                                     \
    _(RunOncePrologue)                                                      \
    _(Rest)                                                                 \
    _(Floor)                                                                \
    _(Ceil)                                                                 \
    _(Round)                                                                \
    _(NearbyInt)                                                            \
    _(InCache)                                                              \
    _(HasOwnCache)                                                          \
    _(InstanceOf)                                                           \
    _(InstanceOfCache)                                                      \
    _(InterruptCheck)                                                       \
    _(GetDOMProperty)                                                       \
    _(GetDOMMember)                                                         \
    _(SetDOMProperty)                                                       \
    _(IsConstructor)                                                        \
    _(IsCallable)                                                           \
    _(IsArray)                                                              \
    _(IsTypedArray)                                                         \
    _(IsObject)                                                             \
    _(HasClass)                                                             \
    _(GuardToClass)                                                         \
    _(ObjectClassToString)                                                  \
    _(CopySign)                                                             \
    _(Rotate)                                                               \
    _(NewDerivedTypedObject)                                                \
    _(RecompileCheck)                                                       \
    _(UnknownValue)                                                         \
    _(LexicalCheck)                                                         \
    _(ThrowRuntimeLexicalError)                                             \
    _(GlobalNameConflictsCheck)                                             \
    _(Debugger)                                                             \
    _(NewTarget)                                                            \
    _(ArrowNewTarget)                                                       \
    _(CheckReturn)                                                          \
    _(CheckIsObj)                                                           \
    _(CheckIsCallable)                                                      \
    _(CheckObjCoercible)                                                    \
    _(DebugCheckSelfHosted)                                                 \
    _(FinishBoundFunctionInit)                                              \
    _(IsPackedArray)                                                        \
    _(GetPrototypeOf)                                                       \
    _(AsmJSLoadHeap)                                                        \
    _(AsmJSStoreHeap)                                                       \
    _(WasmCompareExchangeHeap)                                              \
    _(WasmAtomicExchangeHeap)                                               \
    _(WasmAtomicBinopHeap)                                                  \
    _(WasmNeg)                                                              \
    _(WasmBoundsCheck)                                                      \
    _(WasmAlignmentCheck)                                                   \
    _(WasmLoadTls)                                                          \
    _(WasmAddOffset)                                                        \
    _(WasmLoad)                                                             \
    _(WasmStore)                                                            \
    _(WasmTrap)                                                             \
    _(WasmTruncateToInt32)                                                  \
    _(WasmUnsignedToDouble)                                                 \
    _(WasmUnsignedToFloat32)                                                \
    _(WasmLoadGlobalVar)                                                    \
    _(WasmStoreGlobalVar)                                                   \
    _(WasmReturn)                                                           \
    _(WasmReturnVoid)                                                       \
    _(WasmParameter)                                                        \
    _(WasmStackArg)                                                         \
    _(WasmCall)                                                             \
    _(WasmSelect)                                                           \
    _(WasmReinterpret)                                                      \
    _(WasmFloatConstant)                                                    \
    _(WasmTruncateToInt64)

// Forward declarations of MIR types.
#define FORWARD_DECLARE(op) class M##op;
 MIR_OPCODE_LIST(FORWARD_DECLARE)
#undef FORWARD_DECLARE

class MDefinitionVisitor // interface i.e. pure abstract class
{
  public:
#define VISIT_INS(op) virtual void visit##op(M##op*) = 0;
    MIR_OPCODE_LIST(VISIT_INS)
#undef VISIT_INS
};

// MDefinition visitor which raises a Not Yet Implemented error for
// non-overloaded visit functions.
class MDefinitionVisitorDefaultNYI : public MDefinitionVisitor
{
  public:
#define VISIT_INS(op) virtual void visit##op(M##op*) override { MOZ_CRASH("NYI: " #op); }
    MIR_OPCODE_LIST(VISIT_INS)
#undef VISIT_INS
};

// MDefinition visitor which ignores non-overloaded visit functions.
class MDefinitionVisitorDefaultNoop : public MDefinitionVisitor
{
  public:
#define VISIT_INS(op) virtual void visit##op(M##op*) override { }
    MIR_OPCODE_LIST(VISIT_INS)
#undef VISIT_INS
};

} // namespace jit
} // namespace js

#endif /* jit_MOpcodes_h */

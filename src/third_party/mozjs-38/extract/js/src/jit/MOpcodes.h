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
    _(NurseryObject)                                                        \
    _(SimdBox)                                                              \
    _(SimdUnbox)                                                            \
    _(SimdValueX4)                                                          \
    _(SimdSplatX4)                                                          \
    _(SimdConstant)                                                         \
    _(SimdConvert)                                                          \
    _(SimdReinterpretCast)                                                  \
    _(SimdExtractElement)                                                   \
    _(SimdInsertElement)                                                    \
    _(SimdSignMask)                                                         \
    _(SimdSwizzle)                                                          \
    _(SimdShuffle)                                                          \
    _(SimdUnaryArith)                                                       \
    _(SimdBinaryComp)                                                       \
    _(SimdBinaryArith)                                                      \
    _(SimdBinaryBitwise)                                                    \
    _(SimdShift)                                                            \
    _(SimdSelect)                                                           \
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
    _(Phi)                                                                  \
    _(Beta)                                                                 \
    _(OsrValue)                                                             \
    _(OsrScopeChain)                                                        \
    _(OsrReturnValue)                                                       \
    _(OsrArgumentsObject)                                                   \
    _(ReturnFromCtor)                                                       \
    _(CheckOverRecursed)                                                    \
    _(DefVar)                                                               \
    _(DefFun)                                                               \
    _(CreateThis)                                                           \
    _(CreateThisWithProto)                                                  \
    _(CreateThisWithTemplate)                                               \
    _(CreateArgumentsObject)                                                \
    _(GetArgumentsObjectArg)                                                \
    _(SetArgumentsObjectArg)                                                \
    _(ComputeThis)                                                          \
    _(LoadArrowThis)                                                        \
    _(Call)                                                                 \
    _(ApplyArgs)                                                            \
    _(ArraySplice)                                                          \
    _(Bail)                                                                 \
    _(Unreachable)                                                          \
    _(AssertFloat32)                                                        \
    _(GetDynamicName)                                                       \
    _(FilterArgumentsOrEval)                                                \
    _(CallDirectEval)                                                       \
    _(BitNot)                                                               \
    _(TypeOf)                                                               \
    _(ToId)                                                                 \
    _(BitAnd)                                                               \
    _(BitOr)                                                                \
    _(BitXor)                                                               \
    _(Lsh)                                                                  \
    _(Rsh)                                                                  \
    _(Ursh)                                                                 \
    _(MinMax)                                                               \
    _(Abs)                                                                  \
    _(Clz)                                                                  \
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
    _(ToInt32)                                                              \
    _(TruncateToInt32)                                                      \
    _(ToString)                                                             \
    _(ToObjectOrNull)                                                       \
    _(NewArray)                                                             \
    _(NewArrayCopyOnWrite)                                                  \
    _(NewArrayDynamicLength)                                                \
    _(NewObject)                                                            \
    _(NewTypedObject)                                                       \
    _(NewDeclEnvObject)                                                     \
    _(NewCallObject)                                                        \
    _(NewRunOnceCallObject)                                                 \
    _(NewStringObject)                                                      \
    _(ObjectState)                                                          \
    _(ArrayState)                                                           \
    _(InitElem)                                                             \
    _(InitElemGetterSetter)                                                 \
    _(MutateProto)                                                          \
    _(InitProp)                                                             \
    _(InitPropGetterSetter)                                                 \
    _(Start)                                                                \
    _(OsrEntry)                                                             \
    _(Nop)                                                                  \
    _(LimitedTruncate)                                                      \
    _(RegExp)                                                               \
    _(RegExpExec)                                                           \
    _(RegExpTest)                                                           \
    _(RegExpReplace)                                                        \
    _(StringReplace)                                                        \
    _(Lambda)                                                               \
    _(LambdaArrow)                                                          \
    _(Slots)                                                                \
    _(Elements)                                                             \
    _(ConstantElements)                                                     \
    _(ConvertElementsToDoubles)                                             \
    _(MaybeToDoubleElement)                                                 \
    _(MaybeCopyElementsForWrite)                                            \
    _(LoadSlot)                                                             \
    _(StoreSlot)                                                            \
    _(FunctionEnvironment)                                                  \
    _(FilterTypeSet)                                                        \
    _(TypeBarrier)                                                          \
    _(MonitorTypes)                                                         \
    _(PostWriteBarrier)                                                     \
    _(GetPropertyCache)                                                     \
    _(GetPropertyPolymorphic)                                               \
    _(SetPropertyPolymorphic)                                               \
    _(GetElementCache)                                                      \
    _(SetElementCache)                                                      \
    _(BindNameCache)                                                        \
    _(GuardShape)                                                           \
    _(GuardShapePolymorphic)                                                \
    _(GuardObjectGroup)                                                     \
    _(GuardObjectIdentity)                                                  \
    _(GuardClass)                                                           \
    _(ArrayLength)                                                          \
    _(SetArrayLength)                                                       \
    _(TypedArrayLength)                                                     \
    _(TypedArrayElements)                                                   \
    _(TypedObjectDescr)                                                     \
    _(TypedObjectElements)                                                  \
    _(SetTypedObjectOffset)                                                 \
    _(InitializedLength)                                                    \
    _(SetInitializedLength)                                                 \
    _(Not)                                                                  \
    _(BoundsCheck)                                                          \
    _(BoundsCheckLower)                                                     \
    _(InArray)                                                              \
    _(LoadElement)                                                          \
    _(LoadElementHole)                                                      \
    _(LoadUnboxedObjectOrNull)                                              \
    _(LoadUnboxedString)                                                    \
    _(StoreElement)                                                         \
    _(StoreElementHole)                                                     \
    _(StoreUnboxedObjectOrNull)                                             \
    _(StoreUnboxedString)                                                   \
    _(ConvertUnboxedObjectToNative)                                         \
    _(ArrayPopShift)                                                        \
    _(ArrayPush)                                                            \
    _(ArrayConcat)                                                          \
    _(ArrayJoin)                                                            \
    _(LoadTypedArrayElement)                                                \
    _(LoadTypedArrayElementHole)                                            \
    _(LoadTypedArrayElementStatic)                                          \
    _(StoreTypedArrayElement)                                               \
    _(StoreTypedArrayElementHole)                                           \
    _(StoreTypedArrayElementStatic)                                         \
    _(CompareExchangeTypedArrayElement)                                     \
    _(AtomicTypedArrayElementBinop)                                         \
    _(EffectiveAddress)                                                     \
    _(ClampToUint8)                                                         \
    _(LoadFixedSlot)                                                        \
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
    _(IteratorStart)                                                        \
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
    _(In)                                                                   \
    _(InstanceOf)                                                           \
    _(CallInstanceOf)                                                       \
    _(InterruptCheck)                                                       \
    _(AsmJSInterruptCheck)                                                  \
    _(GetDOMProperty)                                                       \
    _(GetDOMMember)                                                         \
    _(SetDOMProperty)                                                       \
    _(IsCallable)                                                           \
    _(IsObject)                                                             \
    _(HasClass)                                                             \
    _(AsmJSNeg)                                                             \
    _(AsmJSUnsignedToDouble)                                                \
    _(AsmJSUnsignedToFloat32)                                               \
    _(AsmJSLoadHeap)                                                        \
    _(AsmJSStoreHeap)                                                       \
    _(AsmJSLoadGlobalVar)                                                   \
    _(AsmJSStoreGlobalVar)                                                  \
    _(AsmJSLoadFuncPtr)                                                     \
    _(AsmJSLoadFFIFunc)                                                     \
    _(AsmJSReturn)                                                          \
    _(AsmJSParameter)                                                       \
    _(AsmJSVoidReturn)                                                      \
    _(AsmJSPassStackArg)                                                    \
    _(AsmJSCall)                                                            \
    _(NewDerivedTypedObject)                                                \
    _(RecompileCheck)                                                       \
    _(MemoryBarrier)                                                        \
    _(AsmJSCompareExchangeHeap)                                             \
    _(AsmJSAtomicBinopHeap)                                                 \
    _(UnknownValue)                                                         \
    _(LexicalCheck)                                                         \
    _(ThrowUninitializedLexical)                                            \
    _(Debugger)

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
#define VISIT_INS(op) virtual void visit##op(M##op*) { MOZ_CRASH("NYI: " #op); }
    MIR_OPCODE_LIST(VISIT_INS)
#undef VISIT_INS
};

// MDefinition visitor which ignores non-overloaded visit functions.
class MDefinitionVisitorDefaultNoop : public MDefinitionVisitor
{
  public:
#define VISIT_INS(op) virtual void visit##op(M##op*) { }
    MIR_OPCODE_LIST(VISIT_INS)
#undef VISIT_INS
};

} // namespace jit
} // namespace js

#endif /* jit_MOpcodes_h */

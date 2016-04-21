/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/Lowering.h"

#include "mozilla/DebugOnly.h"

#include "jit/JitSpewer.h"
#include "jit/LIR.h"
#include "jit/MIR.h"
#include "jit/MIRGraph.h"

#include "jsobjinlines.h"
#include "jsopcodeinlines.h"

#include "jit/shared/Lowering-shared-inl.h"

using namespace js;
using namespace jit;

using mozilla::DebugOnly;
using JS::GenericNaN;

void
LIRGenerator::useBoxAtStart(LInstruction* lir, size_t n, MDefinition* mir, LUse::Policy policy)
{
    return useBox(lir, n, mir, policy, true);
}

void
LIRGenerator::useBoxFixedAtStart(LInstruction* lir, size_t n, MDefinition* mir, ValueOperand op)
{
#if defined(JS_NUNBOX32)
    return useBoxFixed(lir, n, mir, op.typeReg(), op.payloadReg(), true);
#elif defined(JS_PUNBOX64)
    return useBoxFixed(lir, n, mir, op.valueReg(), op.scratchReg(), true);
#endif
}

void
LIRGenerator::visitCloneLiteral(MCloneLiteral* ins)
{
    MOZ_ASSERT(ins->type() == MIRType_Object);
    MOZ_ASSERT(ins->input()->type() == MIRType_Object);

    LCloneLiteral* lir = new(alloc()) LCloneLiteral(useRegisterAtStart(ins->input()));
    defineReturn(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitParameter(MParameter* param)
{
    ptrdiff_t offset;
    if (param->index() == MParameter::THIS_SLOT)
        offset = THIS_FRAME_ARGSLOT;
    else
        offset = 1 + param->index();

    LParameter* ins = new(alloc()) LParameter;
    defineBox(ins, param, LDefinition::FIXED);

    offset *= sizeof(Value);
#if defined(JS_NUNBOX32)
# if defined(IS_BIG_ENDIAN)
    ins->getDef(0)->setOutput(LArgument(offset));
    ins->getDef(1)->setOutput(LArgument(offset + 4));
# else
    ins->getDef(0)->setOutput(LArgument(offset + 4));
    ins->getDef(1)->setOutput(LArgument(offset));
# endif
#elif defined(JS_PUNBOX64)
    ins->getDef(0)->setOutput(LArgument(offset));
#endif
}

void
LIRGenerator::visitCallee(MCallee* ins)
{
    define(new(alloc()) LCallee(), ins);
}

void
LIRGenerator::visitIsConstructing(MIsConstructing* ins)
{
    define(new(alloc()) LIsConstructing(), ins);
}

void
LIRGenerator::visitGoto(MGoto* ins)
{
    add(new(alloc()) LGoto(ins->target()));
}

void
LIRGenerator::visitTableSwitch(MTableSwitch* tableswitch)
{
    MDefinition* opd = tableswitch->getOperand(0);

    // There should be at least 1 successor. The default case!
    MOZ_ASSERT(tableswitch->numSuccessors() > 0);

    // If there are no cases, the default case is always taken.
    if (tableswitch->numSuccessors() == 1) {
        add(new(alloc()) LGoto(tableswitch->getDefault()));
        return;
    }

    // If we don't know the type.
    if (opd->type() == MIRType_Value) {
        LTableSwitchV* lir = newLTableSwitchV(tableswitch);
        useBox(lir, LTableSwitchV::InputValue, opd);
        add(lir);
        return;
    }

    // Case indices are numeric, so other types will always go to the default case.
    if (opd->type() != MIRType_Int32 && opd->type() != MIRType_Double) {
        add(new(alloc()) LGoto(tableswitch->getDefault()));
        return;
    }

    // Return an LTableSwitch, capable of handling either an integer or
    // floating-point index.
    LAllocation index;
    LDefinition tempInt;
    if (opd->type() == MIRType_Int32) {
        index = useRegisterAtStart(opd);
        tempInt = tempCopy(opd, 0);
    } else {
        index = useRegister(opd);
        tempInt = temp(LDefinition::GENERAL);
    }
    add(newLTableSwitch(index, tempInt, tableswitch));
}

void
LIRGenerator::visitCheckOverRecursed(MCheckOverRecursed* ins)
{
    LCheckOverRecursed* lir = new(alloc()) LCheckOverRecursed();
    add(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitDefVar(MDefVar* ins)
{
    LDefVar* lir = new(alloc()) LDefVar(useRegisterAtStart(ins->scopeChain()));
    add(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitDefLexical(MDefLexical* ins)
{
    LDefLexical* lir = new(alloc()) LDefLexical();
    add(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitDefFun(MDefFun* ins)
{
    LDefFun* lir = new(alloc()) LDefFun(useRegisterAtStart(ins->scopeChain()));
    add(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitNewArray(MNewArray* ins)
{
    LNewArray* lir = new(alloc()) LNewArray(temp());
    define(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitNewArrayCopyOnWrite(MNewArrayCopyOnWrite* ins)
{
    LNewArrayCopyOnWrite* lir = new(alloc()) LNewArrayCopyOnWrite(temp());
    define(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitNewArrayDynamicLength(MNewArrayDynamicLength* ins)
{
    MDefinition* length = ins->length();
    MOZ_ASSERT(length->type() == MIRType_Int32);

    LNewArrayDynamicLength* lir = new(alloc()) LNewArrayDynamicLength(useRegister(length), temp());
    define(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitNewObject(MNewObject* ins)
{
    LNewObject* lir = new(alloc()) LNewObject(temp());
    define(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitNewTypedObject(MNewTypedObject* ins)
{
    LNewTypedObject* lir = new(alloc()) LNewTypedObject(temp());
    define(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitNewDeclEnvObject(MNewDeclEnvObject* ins)
{
    LNewDeclEnvObject* lir = new(alloc()) LNewDeclEnvObject(temp());
    define(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitNewCallObject(MNewCallObject* ins)
{
    LInstruction* lir;
    if (ins->templateObject()->isSingleton()) {
        LNewSingletonCallObject* singletonLir = new(alloc()) LNewSingletonCallObject(temp());
        define(singletonLir, ins);
        lir = singletonLir;
    } else {
        LNewCallObject* normalLir = new(alloc()) LNewCallObject(temp());
        define(normalLir, ins);
        lir = normalLir;
    }

    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitNewRunOnceCallObject(MNewRunOnceCallObject* ins)
{
    LNewSingletonCallObject* lir = new(alloc()) LNewSingletonCallObject(temp());
    define(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitNewDerivedTypedObject(MNewDerivedTypedObject* ins)
{
    LNewDerivedTypedObject* lir =
        new(alloc()) LNewDerivedTypedObject(useRegisterAtStart(ins->type()),
                                            useRegisterAtStart(ins->owner()),
                                            useRegisterAtStart(ins->offset()));
    defineReturn(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitNewStringObject(MNewStringObject* ins)
{
    MOZ_ASSERT(ins->input()->type() == MIRType_String);

    LNewStringObject* lir = new(alloc()) LNewStringObject(useRegister(ins->input()), temp());
    define(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitInitElem(MInitElem* ins)
{
    LInitElem* lir = new(alloc()) LInitElem(useRegisterAtStart(ins->getObject()));
    useBoxAtStart(lir, LInitElem::IdIndex, ins->getId());
    useBoxAtStart(lir, LInitElem::ValueIndex, ins->getValue());
    add(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitInitElemGetterSetter(MInitElemGetterSetter* ins)
{
    LInitElemGetterSetter* lir =
        new(alloc()) LInitElemGetterSetter(useRegisterAtStart(ins->object()),
                                           useRegisterAtStart(ins->value()));
    useBoxAtStart(lir, LInitElemGetterSetter::IdIndex, ins->idValue());
    add(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitMutateProto(MMutateProto* ins)
{
    LMutateProto* lir = new(alloc()) LMutateProto(useRegisterAtStart(ins->getObject()));
    useBoxAtStart(lir, LMutateProto::ValueIndex, ins->getValue());
    add(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitInitProp(MInitProp* ins)
{
    LInitProp* lir = new(alloc()) LInitProp(useRegisterAtStart(ins->getObject()));
    useBoxAtStart(lir, LInitProp::ValueIndex, ins->getValue());
    add(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitInitPropGetterSetter(MInitPropGetterSetter* ins)
{
    LInitPropGetterSetter* lir =
        new(alloc()) LInitPropGetterSetter(useRegisterAtStart(ins->object()),
                                           useRegisterAtStart(ins->value()));
    add(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitCreateThisWithTemplate(MCreateThisWithTemplate* ins)
{
    LCreateThisWithTemplate* lir = new(alloc()) LCreateThisWithTemplate(temp());
    define(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitCreateThisWithProto(MCreateThisWithProto* ins)
{
    LCreateThisWithProto* lir =
        new(alloc()) LCreateThisWithProto(useRegisterOrConstantAtStart(ins->getCallee()),
                                          useRegisterOrConstantAtStart(ins->getNewTarget()),
                                          useRegisterOrConstantAtStart(ins->getPrototype()));
    defineReturn(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitCreateThis(MCreateThis* ins)
{
    LCreateThis* lir = new(alloc()) LCreateThis(useRegisterOrConstantAtStart(ins->getCallee()),
                                                useRegisterOrConstantAtStart(ins->getNewTarget()));
    defineReturn(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitCreateArgumentsObject(MCreateArgumentsObject* ins)
{
    // LAllocation callObj = useRegisterAtStart(ins->getCallObject());
    LAllocation callObj = useFixed(ins->getCallObject(), CallTempReg0);
    LCreateArgumentsObject* lir = new(alloc()) LCreateArgumentsObject(callObj, tempFixed(CallTempReg1));
    defineReturn(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitGetArgumentsObjectArg(MGetArgumentsObjectArg* ins)
{
    LAllocation argsObj = useRegister(ins->getArgsObject());
    LGetArgumentsObjectArg* lir = new(alloc()) LGetArgumentsObjectArg(argsObj, temp());
    defineBox(lir, ins);
}

void
LIRGenerator::visitSetArgumentsObjectArg(MSetArgumentsObjectArg* ins)
{
    LAllocation argsObj = useRegister(ins->getArgsObject());
    LSetArgumentsObjectArg* lir = new(alloc()) LSetArgumentsObjectArg(argsObj, temp());
    useBox(lir, LSetArgumentsObjectArg::ValueIndex, ins->getValue());
    add(lir, ins);
}

void
LIRGenerator::visitReturnFromCtor(MReturnFromCtor* ins)
{
    LReturnFromCtor* lir = new(alloc()) LReturnFromCtor(useRegister(ins->getObject()));
    useBox(lir, LReturnFromCtor::ValueIndex, ins->getValue());
    define(lir, ins);
}

void
LIRGenerator::visitComputeThis(MComputeThis* ins)
{
    MOZ_ASSERT(ins->type() == MIRType_Value);
    MOZ_ASSERT(ins->input()->type() == MIRType_Value);

    LComputeThis* lir = new(alloc()) LComputeThis();

    // Don't use useBoxAtStart because ComputeThis has a safepoint and needs to
    // have its inputs in different registers than its return value so that
    // they aren't clobbered.
    useBox(lir, LComputeThis::ValueIndex, ins->input());

    defineBox(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitArrowNewTarget(MArrowNewTarget* ins)
{
    MOZ_ASSERT(ins->type() == MIRType_Value);
    MOZ_ASSERT(ins->callee()->type() == MIRType_Object);

    LArrowNewTarget* lir = new(alloc()) LArrowNewTarget(useRegister(ins->callee()));
    defineBox(lir, ins);
}

void
LIRGenerator::lowerCallArguments(MCall* call)
{
    uint32_t argc = call->numStackArgs();

    // Align the arguments of a call such that the callee would keep the same
    // alignment as the caller.
    uint32_t baseSlot = 0;
    if (JitStackValueAlignment > 1)
        baseSlot = AlignBytes(argc, JitStackValueAlignment);
    else
        baseSlot = argc;

    // Save the maximum number of argument, such that we can have one unique
    // frame size.
    if (baseSlot > maxargslots_)
        maxargslots_ = baseSlot;

    for (size_t i = 0; i < argc; i++) {
        MDefinition* arg = call->getArg(i);
        uint32_t argslot = baseSlot - i;

        // Values take a slow path.
        if (arg->type() == MIRType_Value) {
            LStackArgV* stack = new(alloc()) LStackArgV(argslot);
            useBox(stack, 0, arg);
            add(stack);
        } else {
            // Known types can move constant types and/or payloads.
            LStackArgT* stack = new(alloc()) LStackArgT(argslot, arg->type(), useRegisterOrConstant(arg));
            add(stack);
        }
    }
}

void
LIRGenerator::visitCall(MCall* call)
{
    MOZ_ASSERT(CallTempReg0 != CallTempReg1);
    MOZ_ASSERT(CallTempReg0 != ArgumentsRectifierReg);
    MOZ_ASSERT(CallTempReg1 != ArgumentsRectifierReg);
    MOZ_ASSERT(call->getFunction()->type() == MIRType_Object);

    lowerCallArguments(call);

    // Height of the current argument vector.
    JSFunction* target = call->getSingleTarget();

    LInstruction* lir;

    if (call->isCallDOMNative()) {
        // Call DOM functions.
        MOZ_ASSERT(target && target->isNative());
        Register cxReg, objReg, privReg, argsReg;
        GetTempRegForIntArg(0, 0, &cxReg);
        GetTempRegForIntArg(1, 0, &objReg);
        GetTempRegForIntArg(2, 0, &privReg);
        mozilla::DebugOnly<bool> ok = GetTempRegForIntArg(3, 0, &argsReg);
        MOZ_ASSERT(ok, "How can we not have four temp registers?");
        lir = new(alloc()) LCallDOMNative(tempFixed(cxReg), tempFixed(objReg),
                                          tempFixed(privReg), tempFixed(argsReg));
    } else if (target) {
        // Call known functions.
        if (target->isNative()) {
            Register cxReg, numReg, vpReg, tmpReg;
            GetTempRegForIntArg(0, 0, &cxReg);
            GetTempRegForIntArg(1, 0, &numReg);
            GetTempRegForIntArg(2, 0, &vpReg);

            // Even though this is just a temp reg, use the same API to avoid
            // register collisions.
            mozilla::DebugOnly<bool> ok = GetTempRegForIntArg(3, 0, &tmpReg);
            MOZ_ASSERT(ok, "How can we not have four temp registers?");

            lir = new(alloc()) LCallNative(tempFixed(cxReg), tempFixed(numReg),
                                           tempFixed(vpReg), tempFixed(tmpReg));
        } else {
            lir = new(alloc()) LCallKnown(useFixed(call->getFunction(), CallTempReg0),
                                          tempFixed(CallTempReg2));
        }
    } else {
        // Call anything, using the most generic code.
        lir = new(alloc()) LCallGeneric(useFixed(call->getFunction(), CallTempReg0),
                                        tempFixed(ArgumentsRectifierReg),
                                        tempFixed(CallTempReg2));
    }
    defineReturn(lir, call);
    assignSafepoint(lir, call);
}

void
LIRGenerator::visitApplyArgs(MApplyArgs* apply)
{
    MOZ_ASSERT(apply->getFunction()->type() == MIRType_Object);

    // Assert if we cannot build a rectifier frame.
    MOZ_ASSERT(CallTempReg0 != ArgumentsRectifierReg);
    MOZ_ASSERT(CallTempReg1 != ArgumentsRectifierReg);

    // Assert if the return value is already erased.
    MOZ_ASSERT(CallTempReg2 != JSReturnReg_Type);
    MOZ_ASSERT(CallTempReg2 != JSReturnReg_Data);

    LApplyArgsGeneric* lir = new(alloc()) LApplyArgsGeneric(
        useFixed(apply->getFunction(), CallTempReg3),
        useFixed(apply->getArgc(), CallTempReg0),
        tempFixed(CallTempReg1),  // object register
        tempFixed(CallTempReg2)); // stack counter register

    MDefinition* self = apply->getThis();
    useBoxFixed(lir, LApplyArgsGeneric::ThisIndex, self, CallTempReg4, CallTempReg5);

    // Bailout is only needed in the case of possible non-JSFunction callee.
    if (!apply->getSingleTarget())
        assignSnapshot(lir, Bailout_NonJSFunctionCallee);

    defineReturn(lir, apply);
    assignSafepoint(lir, apply);
}

void
LIRGenerator::visitApplyArray(MApplyArray* apply)
{
    MOZ_ASSERT(apply->getFunction()->type() == MIRType_Object);

    // Assert if we cannot build a rectifier frame.
    MOZ_ASSERT(CallTempReg0 != ArgumentsRectifierReg);
    MOZ_ASSERT(CallTempReg1 != ArgumentsRectifierReg);

    // Assert if the return value is already erased.
    MOZ_ASSERT(CallTempReg2 != JSReturnReg_Type);
    MOZ_ASSERT(CallTempReg2 != JSReturnReg_Data);

    LApplyArrayGeneric* lir = new(alloc()) LApplyArrayGeneric(
        useFixedAtStart(apply->getFunction(), CallTempReg3),
        useFixedAtStart(apply->getElements(), CallTempReg0),
        tempFixed(CallTempReg1),  // object register
        tempFixed(CallTempReg2)); // stack counter register

    MDefinition* self = apply->getThis();
    useBoxFixed(lir, LApplyArrayGeneric::ThisIndex, self, CallTempReg4, CallTempReg5);

    // Bailout is needed in the case of possible non-JSFunction callee,
    // too many values in the array, or empty space at the end of the
    // array.  I'm going to use NonJSFunctionCallee for the code even
    // if that is not an adequate description.
    assignSnapshot(lir, Bailout_NonJSFunctionCallee);

    defineReturn(lir, apply);
    assignSafepoint(lir, apply);
}

void
LIRGenerator::visitBail(MBail* bail)
{
    LBail* lir = new(alloc()) LBail();
    assignSnapshot(lir, bail->bailoutKind());
    add(lir, bail);
}

void
LIRGenerator::visitUnreachable(MUnreachable* unreachable)
{
    LUnreachable* lir = new(alloc()) LUnreachable();
    add(lir, unreachable);
}

void
LIRGenerator::visitEncodeSnapshot(MEncodeSnapshot* mir)
{
    LEncodeSnapshot* lir = new(alloc()) LEncodeSnapshot();
    assignSnapshot(lir, Bailout_Inevitable);
    add(lir, mir);
}

void
LIRGenerator::visitAssertFloat32(MAssertFloat32* assertion)
{
    MIRType type = assertion->input()->type();
    DebugOnly<bool> checkIsFloat32 = assertion->mustBeFloat32();

    if (type != MIRType_Value && !JitOptions.eagerCompilation) {
        MOZ_ASSERT_IF(checkIsFloat32, type == MIRType_Float32);
        MOZ_ASSERT_IF(!checkIsFloat32, type != MIRType_Float32);
    }
}

void
LIRGenerator::visitAssertRecoveredOnBailout(MAssertRecoveredOnBailout* assertion)
{
    MOZ_CRASH("AssertRecoveredOnBailout nodes are always recovered on bailouts.");
}

void
LIRGenerator::visitArraySplice(MArraySplice* ins)
{
    LArraySplice* lir = new(alloc()) LArraySplice(useRegisterAtStart(ins->object()),
                                                  useRegisterAtStart(ins->start()),
                                                  useRegisterAtStart(ins->deleteCount()));
    add(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitGetDynamicName(MGetDynamicName* ins)
{
    MDefinition* scopeChain = ins->getScopeChain();
    MOZ_ASSERT(scopeChain->type() == MIRType_Object);

    MDefinition* name = ins->getName();
    MOZ_ASSERT(name->type() == MIRType_String);

    LGetDynamicName* lir = new(alloc()) LGetDynamicName(useFixed(scopeChain, CallTempReg0),
                                                        useFixed(name, CallTempReg1),
                                                        tempFixed(CallTempReg2),
                                                        tempFixed(CallTempReg3),
                                                        tempFixed(CallTempReg4));

    assignSnapshot(lir, Bailout_DynamicNameNotFound);
    defineReturn(lir, ins);
}

void
LIRGenerator::visitCallDirectEval(MCallDirectEval* ins)
{
    MDefinition* scopeChain = ins->getScopeChain();
    MOZ_ASSERT(scopeChain->type() == MIRType_Object);

    MDefinition* string = ins->getString();
    MOZ_ASSERT(string->type() == MIRType_String);

    MDefinition* newTargetValue = ins->getNewTargetValue();

    LInstruction* lir = new(alloc()) LCallDirectEval(useRegisterAtStart(scopeChain),
                                                     useRegisterAtStart(string));
    useBoxAtStart(lir, LCallDirectEval::NewTarget, newTargetValue);

    defineReturn(lir, ins);
    assignSafepoint(lir, ins);
}

static JSOp
ReorderComparison(JSOp op, MDefinition** lhsp, MDefinition** rhsp)
{
    MDefinition* lhs = *lhsp;
    MDefinition* rhs = *rhsp;

    if (lhs->isConstantValue()) {
        *rhsp = lhs;
        *lhsp = rhs;
        return ReverseCompareOp(op);
    }
    return op;
}

void
LIRGenerator::visitTest(MTest* test)
{
    MDefinition* opd = test->getOperand(0);
    MBasicBlock* ifTrue = test->ifTrue();
    MBasicBlock* ifFalse = test->ifFalse();

    // String is converted to length of string in the type analysis phase (see
    // TestPolicy).
    MOZ_ASSERT(opd->type() != MIRType_String);

    // Testing a constant.
    if (opd->isConstantValue() && !opd->constantValue().isMagic()) {
        bool result = opd->constantToBoolean();
        add(new(alloc()) LGoto(result ? ifTrue : ifFalse));
        return;
    }

    if (opd->type() == MIRType_Value) {
        LDefinition temp0, temp1;
        if (test->operandMightEmulateUndefined()) {
            temp0 = temp();
            temp1 = temp();
        } else {
            temp0 = LDefinition::BogusTemp();
            temp1 = LDefinition::BogusTemp();
        }
        LTestVAndBranch* lir =
            new(alloc()) LTestVAndBranch(ifTrue, ifFalse, tempDouble(), temp0, temp1);
        useBox(lir, LTestVAndBranch::Input, opd);
        add(lir, test);
        return;
    }

    if (opd->type() == MIRType_ObjectOrNull) {
        LDefinition temp0 = test->operandMightEmulateUndefined() ? temp() : LDefinition::BogusTemp();
        add(new(alloc()) LTestOAndBranch(useRegister(opd), ifTrue, ifFalse, temp0), test);
        return;
    }

    // Objects are truthy, except if it might emulate undefined.
    if (opd->type() == MIRType_Object) {
        if (test->operandMightEmulateUndefined())
            add(new(alloc()) LTestOAndBranch(useRegister(opd), ifTrue, ifFalse, temp()), test);
        else
            add(new(alloc()) LGoto(ifTrue));
        return;
    }

    // These must be explicitly sniffed out since they are constants and have
    // no payload.
    if (opd->type() == MIRType_Undefined || opd->type() == MIRType_Null) {
        add(new(alloc()) LGoto(ifFalse));
        return;
    }

    // All symbols are truthy.
    if (opd->type() == MIRType_Symbol) {
        add(new(alloc()) LGoto(ifTrue));
        return;
    }

    // Check if the operand for this test is a compare operation. If it is, we want
    // to emit an LCompare*AndBranch rather than an LTest*AndBranch, to fuse the
    // compare and jump instructions.
    if (opd->isCompare() && opd->isEmittedAtUses()) {
        MCompare* comp = opd->toCompare();
        MDefinition* left = comp->lhs();
        MDefinition* right = comp->rhs();

        // Try to fold the comparison so that we don't have to handle all cases.
        bool result;
        if (comp->tryFold(&result)) {
            add(new(alloc()) LGoto(result ? ifTrue : ifFalse));
            return;
        }

        // Emit LCompare*AndBranch.

        // Compare and branch null/undefined.
        // The second operand has known null/undefined type,
        // so just test the first operand.
        if (comp->compareType() == MCompare::Compare_Null ||
            comp->compareType() == MCompare::Compare_Undefined)
        {
            if (left->type() == MIRType_Object || left->type() == MIRType_ObjectOrNull) {
                MOZ_ASSERT(left->type() == MIRType_ObjectOrNull ||
                           comp->operandMightEmulateUndefined(),
                           "MCompare::tryFold should handle the never-emulates-undefined case");

                LDefinition tmp =
                    comp->operandMightEmulateUndefined() ? temp() : LDefinition::BogusTemp();
                LIsNullOrLikeUndefinedAndBranchT* lir =
                    new(alloc()) LIsNullOrLikeUndefinedAndBranchT(comp, useRegister(left),
                                                                  ifTrue, ifFalse, tmp);
                add(lir, test);
                return;
            }

            LDefinition tmp, tmpToUnbox;
            if (comp->operandMightEmulateUndefined()) {
                tmp = temp();
                tmpToUnbox = tempToUnbox();
            } else {
                tmp = LDefinition::BogusTemp();
                tmpToUnbox = LDefinition::BogusTemp();
            }

            LIsNullOrLikeUndefinedAndBranchV* lir =
                new(alloc()) LIsNullOrLikeUndefinedAndBranchV(comp, ifTrue, ifFalse,
                                                              tmp, tmpToUnbox);
            useBox(lir, LIsNullOrLikeUndefinedAndBranchV::Value, left);
            add(lir, test);
            return;
        }

        // Compare and branch booleans.
        if (comp->compareType() == MCompare::Compare_Boolean) {
            MOZ_ASSERT(left->type() == MIRType_Value);
            MOZ_ASSERT(right->type() == MIRType_Boolean);

            LAllocation rhs = useRegisterOrConstant(right);
            LCompareBAndBranch* lir = new(alloc()) LCompareBAndBranch(comp, rhs, ifTrue, ifFalse);
            useBox(lir, LCompareBAndBranch::Lhs, left);
            add(lir, test);
            return;
        }

        // Compare and branch Int32 or Object pointers.
        if (comp->isInt32Comparison() ||
            comp->compareType() == MCompare::Compare_UInt32 ||
            comp->compareType() == MCompare::Compare_Object)
        {
            JSOp op = ReorderComparison(comp->jsop(), &left, &right);
            LAllocation lhs = useRegister(left);
            LAllocation rhs;
            if (comp->isInt32Comparison() || comp->compareType() == MCompare::Compare_UInt32)
                rhs = useAnyOrConstant(right);
            else
                rhs = useRegister(right);
            LCompareAndBranch* lir = new(alloc()) LCompareAndBranch(comp, op, lhs, rhs,
                                                                    ifTrue, ifFalse);
            add(lir, test);
            return;
        }

        // Compare and branch doubles.
        if (comp->isDoubleComparison()) {
            LAllocation lhs = useRegister(left);
            LAllocation rhs = useRegister(right);
            LCompareDAndBranch* lir = new(alloc()) LCompareDAndBranch(comp, lhs, rhs,
                                                                      ifTrue, ifFalse);
            add(lir, test);
            return;
        }

        // Compare and branch floats.
        if (comp->isFloat32Comparison()) {
            LAllocation lhs = useRegister(left);
            LAllocation rhs = useRegister(right);
            LCompareFAndBranch* lir = new(alloc()) LCompareFAndBranch(comp, lhs, rhs,
                                                                      ifTrue, ifFalse);
            add(lir, test);
            return;
        }

        // Compare values.
        if (comp->compareType() == MCompare::Compare_Bitwise) {
            LCompareBitwiseAndBranch* lir =
                new(alloc()) LCompareBitwiseAndBranch(comp, ifTrue, ifFalse);
            useBoxAtStart(lir, LCompareBitwiseAndBranch::LhsInput, left);
            useBoxAtStart(lir, LCompareBitwiseAndBranch::RhsInput, right);
            add(lir, test);
            return;
        }
    }

    // Check if the operand for this test is a bitand operation. If it is, we want
    // to emit an LBitAndAndBranch rather than an LTest*AndBranch.
    if (opd->isBitAnd() && opd->isEmittedAtUses()) {
        MDefinition* lhs = opd->getOperand(0);
        MDefinition* rhs = opd->getOperand(1);
        if (lhs->type() == MIRType_Int32 && rhs->type() == MIRType_Int32) {
            ReorderCommutative(&lhs, &rhs, test);
            lowerForBitAndAndBranch(new(alloc()) LBitAndAndBranch(ifTrue, ifFalse), test, lhs, rhs);
            return;
        }
    }

    if (opd->isIsObject() && opd->isEmittedAtUses()) {
        MDefinition* input = opd->toIsObject()->input();
        MOZ_ASSERT(input->type() == MIRType_Value);

        LIsObjectAndBranch* lir = new(alloc()) LIsObjectAndBranch(ifTrue, ifFalse);
        useBoxAtStart(lir, LIsObjectAndBranch::Input, input);
        add(lir, test);
        return;
    }

    if (opd->isIsNoIter()) {
        MOZ_ASSERT(opd->isEmittedAtUses());

        MDefinition* input = opd->toIsNoIter()->input();
        MOZ_ASSERT(input->type() == MIRType_Value);

        LIsNoIterAndBranch* lir = new(alloc()) LIsNoIterAndBranch(ifTrue, ifFalse);
        useBox(lir, LIsNoIterAndBranch::Input, input);
        add(lir, test);
        return;
    }

    switch (opd->type()) {
      case MIRType_Double:
        add(new(alloc()) LTestDAndBranch(useRegister(opd), ifTrue, ifFalse));
        break;
      case MIRType_Float32:
        add(new(alloc()) LTestFAndBranch(useRegister(opd), ifTrue, ifFalse));
        break;
      case MIRType_Int32:
      case MIRType_Boolean:
        add(new(alloc()) LTestIAndBranch(useRegister(opd), ifTrue, ifFalse));
        break;
      default:
        MOZ_CRASH("Bad type");
    }
}

void
LIRGenerator::visitGotoWithFake(MGotoWithFake* gotoWithFake)
{
    add(new(alloc()) LGoto(gotoWithFake->target()));
}

void
LIRGenerator::visitFunctionDispatch(MFunctionDispatch* ins)
{
    LFunctionDispatch* lir = new(alloc()) LFunctionDispatch(useRegister(ins->input()));
    add(lir, ins);
}

void
LIRGenerator::visitObjectGroupDispatch(MObjectGroupDispatch* ins)
{
    LObjectGroupDispatch* lir = new(alloc()) LObjectGroupDispatch(useRegister(ins->input()), temp());
    add(lir, ins);
}

static inline bool
CanEmitCompareAtUses(MInstruction* ins)
{
    if (!ins->canEmitAtUses())
        return false;

    bool foundTest = false;
    for (MUseIterator iter(ins->usesBegin()); iter != ins->usesEnd(); iter++) {
        MNode* node = iter->consumer();
        if (!node->isDefinition())
            return false;
        if (!node->toDefinition()->isTest())
            return false;
        if (foundTest)
            return false;
        foundTest = true;
    }
    return true;
}

void
LIRGenerator::visitCompare(MCompare* comp)
{
    MDefinition* left = comp->lhs();
    MDefinition* right = comp->rhs();

    // Try to fold the comparison so that we don't have to handle all cases.
    bool result;
    if (comp->tryFold(&result)) {
        define(new(alloc()) LInteger(result), comp);
        return;
    }

    // Move below the emitAtUses call if we ever implement
    // LCompareSAndBranch. Doing this now wouldn't be wrong, but doesn't
    // make sense and avoids confusion.
    if (comp->compareType() == MCompare::Compare_String) {
        LCompareS* lir = new(alloc()) LCompareS(useRegister(left), useRegister(right));
        define(lir, comp);
        assignSafepoint(lir, comp);
        return;
    }

    // Strict compare between value and string
    if (comp->compareType() == MCompare::Compare_StrictString) {
        MOZ_ASSERT(left->type() == MIRType_Value);
        MOZ_ASSERT(right->type() == MIRType_String);

        LCompareStrictS* lir = new(alloc()) LCompareStrictS(useRegister(right), tempToUnbox());
        useBox(lir, LCompareStrictS::Lhs, left);
        define(lir, comp);
        assignSafepoint(lir, comp);
        return;
    }

    // Unknown/unspecialized compare use a VM call.
    if (comp->compareType() == MCompare::Compare_Unknown) {
        LCompareVM* lir = new(alloc()) LCompareVM();
        useBoxAtStart(lir, LCompareVM::LhsInput, left);
        useBoxAtStart(lir, LCompareVM::RhsInput, right);
        defineReturn(lir, comp);
        assignSafepoint(lir, comp);
        return;
    }

    // Sniff out if the output of this compare is used only for a branching.
    // If it is, then we will emit an LCompare*AndBranch instruction in place
    // of this compare and any test that uses this compare. Thus, we can
    // ignore this Compare.
    if (CanEmitCompareAtUses(comp)) {
        emitAtUses(comp);
        return;
    }

    // Compare Null and Undefined.
    if (comp->compareType() == MCompare::Compare_Null ||
        comp->compareType() == MCompare::Compare_Undefined)
    {
        if (left->type() == MIRType_Object || left->type() == MIRType_ObjectOrNull) {
            MOZ_ASSERT(left->type() == MIRType_ObjectOrNull ||
                       comp->operandMightEmulateUndefined(),
                       "MCompare::tryFold should have folded this away");

            define(new(alloc()) LIsNullOrLikeUndefinedT(useRegister(left)), comp);
            return;
        }

        LDefinition tmp, tmpToUnbox;
        if (comp->operandMightEmulateUndefined()) {
            tmp = temp();
            tmpToUnbox = tempToUnbox();
        } else {
            tmp = LDefinition::BogusTemp();
            tmpToUnbox = LDefinition::BogusTemp();
        }

        LIsNullOrLikeUndefinedV* lir = new(alloc()) LIsNullOrLikeUndefinedV(tmp, tmpToUnbox);
        useBox(lir, LIsNullOrLikeUndefinedV::Value, left);
        define(lir, comp);
        return;
    }

    // Compare booleans.
    if (comp->compareType() == MCompare::Compare_Boolean) {
        MOZ_ASSERT(left->type() == MIRType_Value);
        MOZ_ASSERT(right->type() == MIRType_Boolean);

        LCompareB* lir = new(alloc()) LCompareB(useRegisterOrConstant(right));
        useBox(lir, LCompareB::Lhs, left);
        define(lir, comp);
        return;
    }

    // Compare Int32 or Object pointers.
    if (comp->isInt32Comparison() ||
        comp->compareType() == MCompare::Compare_UInt32 ||
        comp->compareType() == MCompare::Compare_Object)
    {
        JSOp op = ReorderComparison(comp->jsop(), &left, &right);
        LAllocation lhs = useRegister(left);
        LAllocation rhs;
        if (comp->isInt32Comparison() ||
            comp->compareType() == MCompare::Compare_UInt32)
        {
            rhs = useAnyOrConstant(right);
        } else {
            rhs = useRegister(right);
        }
        define(new(alloc()) LCompare(op, lhs, rhs), comp);
        return;
    }

    // Compare doubles.
    if (comp->isDoubleComparison()) {
        define(new(alloc()) LCompareD(useRegister(left), useRegister(right)), comp);
        return;
    }

    // Compare float32.
    if (comp->isFloat32Comparison()) {
        define(new(alloc()) LCompareF(useRegister(left), useRegister(right)), comp);
        return;
    }

    // Compare values.
    if (comp->compareType() == MCompare::Compare_Bitwise) {
        LCompareBitwise* lir = new(alloc()) LCompareBitwise();
        useBoxAtStart(lir, LCompareBitwise::LhsInput, left);
        useBoxAtStart(lir, LCompareBitwise::RhsInput, right);
        define(lir, comp);
        return;
    }

    MOZ_CRASH("Unrecognized compare type.");
}

void
LIRGenerator::lowerBitOp(JSOp op, MInstruction* ins)
{
    MDefinition* lhs = ins->getOperand(0);
    MDefinition* rhs = ins->getOperand(1);

    if (lhs->type() == MIRType_Int32 && rhs->type() == MIRType_Int32) {
        ReorderCommutative(&lhs, &rhs, ins);
        lowerForALU(new(alloc()) LBitOpI(op), ins, lhs, rhs);
        return;
    }

    LBitOpV* lir = new(alloc()) LBitOpV(op);
    useBoxAtStart(lir, LBitOpV::LhsInput, lhs);
    useBoxAtStart(lir, LBitOpV::RhsInput, rhs);
    defineReturn(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitTypeOf(MTypeOf* ins)
{
    MDefinition* opd = ins->input();
    MOZ_ASSERT(opd->type() == MIRType_Value);

    LTypeOfV* lir = new(alloc()) LTypeOfV(tempToUnbox());
    useBox(lir, LTypeOfV::Input, opd);
    define(lir, ins);
}

void
LIRGenerator::visitToId(MToId* ins)
{
    LToIdV* lir = new(alloc()) LToIdV(tempDouble());
    useBox(lir, LToIdV::Index, ins->input());
    defineBox(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitBitNot(MBitNot* ins)
{
    MDefinition* input = ins->getOperand(0);

    if (input->type() == MIRType_Int32) {
        lowerForALU(new(alloc()) LBitNotI(), ins, input);
        return;
    }

    LBitNotV* lir = new(alloc()) LBitNotV;
    useBoxAtStart(lir, LBitNotV::Input, input);
    defineReturn(lir, ins);
    assignSafepoint(lir, ins);
}

static bool
CanEmitBitAndAtUses(MInstruction* ins)
{
    if (!ins->canEmitAtUses())
        return false;

    if (ins->getOperand(0)->type() != MIRType_Int32 || ins->getOperand(1)->type() != MIRType_Int32)
        return false;

    MUseIterator iter(ins->usesBegin());
    if (iter == ins->usesEnd())
        return false;

    MNode* node = iter->consumer();
    if (!node->isDefinition())
        return false;

    if (!node->toDefinition()->isTest())
        return false;

    iter++;
    return iter == ins->usesEnd();
}

void
LIRGenerator::visitBitAnd(MBitAnd* ins)
{
    // Sniff out if the output of this bitand is used only for a branching.
    // If it is, then we will emit an LBitAndAndBranch instruction in place
    // of this bitand and any test that uses this bitand. Thus, we can
    // ignore this BitAnd.
    if (CanEmitBitAndAtUses(ins))
        emitAtUses(ins);
    else
        lowerBitOp(JSOP_BITAND, ins);
}

void
LIRGenerator::visitBitOr(MBitOr* ins)
{
    lowerBitOp(JSOP_BITOR, ins);
}

void
LIRGenerator::visitBitXor(MBitXor* ins)
{
    lowerBitOp(JSOP_BITXOR, ins);
}

void
LIRGenerator::lowerShiftOp(JSOp op, MShiftInstruction* ins)
{
    MDefinition* lhs = ins->getOperand(0);
    MDefinition* rhs = ins->getOperand(1);

    if (lhs->type() == MIRType_Int32 && rhs->type() == MIRType_Int32) {
        if (ins->type() == MIRType_Double) {
            MOZ_ASSERT(op == JSOP_URSH);
            lowerUrshD(ins->toUrsh());
            return;
        }

        LShiftI* lir = new(alloc()) LShiftI(op);
        if (op == JSOP_URSH) {
            if (ins->toUrsh()->fallible())
                assignSnapshot(lir, Bailout_OverflowInvalidate);
        }
        lowerForShift(lir, ins, lhs, rhs);
        return;
    }

    MOZ_ASSERT(ins->specialization() == MIRType_None);

    if (op == JSOP_URSH) {
        // Result is either int32 or double so we have to use BinaryV.
        lowerBinaryV(JSOP_URSH, ins);
        return;
    }

    LBitOpV* lir = new(alloc()) LBitOpV(op);
    useBoxAtStart(lir, LBitOpV::LhsInput, lhs);
    useBoxAtStart(lir, LBitOpV::RhsInput, rhs);
    defineReturn(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitLsh(MLsh* ins)
{
    lowerShiftOp(JSOP_LSH, ins);
}

void
LIRGenerator::visitRsh(MRsh* ins)
{
    lowerShiftOp(JSOP_RSH, ins);
}

void
LIRGenerator::visitUrsh(MUrsh* ins)
{
    lowerShiftOp(JSOP_URSH, ins);
}

void
LIRGenerator::visitFloor(MFloor* ins)
{
    MIRType type = ins->input()->type();
    MOZ_ASSERT(IsFloatingPointType(type));

    LInstructionHelper<1, 1, 0>* lir;
    if (type == MIRType_Double)
        lir = new(alloc()) LFloor(useRegister(ins->input()));
    else
        lir = new(alloc()) LFloorF(useRegister(ins->input()));

    assignSnapshot(lir, Bailout_Round);
    define(lir, ins);
}

void
LIRGenerator::visitCeil(MCeil* ins)
{
    MIRType type = ins->input()->type();
    MOZ_ASSERT(IsFloatingPointType(type));

    LInstructionHelper<1, 1, 0>* lir;
    if (type == MIRType_Double)
        lir = new(alloc()) LCeil(useRegister(ins->input()));
    else
        lir = new(alloc()) LCeilF(useRegister(ins->input()));

    assignSnapshot(lir, Bailout_Round);
    define(lir, ins);
}

void
LIRGenerator::visitRound(MRound* ins)
{
    MIRType type = ins->input()->type();
    MOZ_ASSERT(IsFloatingPointType(type));

    LInstructionHelper<1, 1, 1>* lir;
    if (type == MIRType_Double)
        lir = new (alloc()) LRound(useRegister(ins->input()), tempDouble());
    else
        lir = new (alloc()) LRoundF(useRegister(ins->input()), tempFloat32());

    assignSnapshot(lir, Bailout_Round);
    define(lir, ins);
}

void
LIRGenerator::visitMinMax(MMinMax* ins)
{
    MDefinition* first = ins->getOperand(0);
    MDefinition* second = ins->getOperand(1);

    ReorderCommutative(&first, &second, ins);

    LMinMaxBase* lir;
    switch (ins->specialization()) {
      case MIRType_Int32:
        lir = new(alloc()) LMinMaxI(useRegisterAtStart(first), useRegisterOrConstant(second));
        break;
      case MIRType_Float32:
        lir = new(alloc()) LMinMaxF(useRegisterAtStart(first), useRegister(second));
        break;
      case MIRType_Double:
        lir = new(alloc()) LMinMaxD(useRegisterAtStart(first), useRegister(second));
        break;
      default:
        MOZ_CRASH();
    }

    defineReuseInput(lir, ins, 0);
}

void
LIRGenerator::visitAbs(MAbs* ins)
{
    MDefinition* num = ins->input();
    MOZ_ASSERT(IsNumberType(num->type()));

    LInstructionHelper<1, 1, 0>* lir;
    switch (num->type()) {
      case MIRType_Int32:
        lir = new(alloc()) LAbsI(useRegisterAtStart(num));
        // needed to handle abs(INT32_MIN)
        if (ins->fallible())
            assignSnapshot(lir, Bailout_Overflow);
        break;
      case MIRType_Float32:
        lir = new(alloc()) LAbsF(useRegisterAtStart(num));
        break;
      case MIRType_Double:
        lir = new(alloc()) LAbsD(useRegisterAtStart(num));
        break;
      default:
        MOZ_CRASH();
    }

    defineReuseInput(lir, ins, 0);
}

void
LIRGenerator::visitClz(MClz* ins)
{
    MDefinition* num = ins->num();

    LClzI* lir = new(alloc()) LClzI(useRegisterAtStart(num));
    define(lir, ins);
}

void
LIRGenerator::visitSqrt(MSqrt* ins)
{
    MDefinition* num = ins->input();
    MOZ_ASSERT(IsFloatingPointType(num->type()));

    LInstructionHelper<1, 1, 0>* lir;
    if (num->type() == MIRType_Double)
        lir = new(alloc()) LSqrtD(useRegisterAtStart(num));
    else
        lir = new(alloc()) LSqrtF(useRegisterAtStart(num));
    define(lir, ins);
}

void
LIRGenerator::visitAtan2(MAtan2* ins)
{
    MDefinition* y = ins->y();
    MOZ_ASSERT(y->type() == MIRType_Double);

    MDefinition* x = ins->x();
    MOZ_ASSERT(x->type() == MIRType_Double);

    LAtan2D* lir = new(alloc()) LAtan2D(useRegisterAtStart(y), useRegisterAtStart(x),
                                        tempFixed(CallTempReg0));
    defineReturn(lir, ins);
}

void
LIRGenerator::visitHypot(MHypot* ins)
{
    LHypot* lir = nullptr;
    uint32_t length = ins->numOperands();
    for (uint32_t i = 0; i < length; ++i)
        MOZ_ASSERT(ins->getOperand(i)->type() == MIRType_Double);

    switch(length) {
      case 2:
        lir = new(alloc()) LHypot(useRegisterAtStart(ins->getOperand(0)),
                                  useRegisterAtStart(ins->getOperand(1)),
                                  tempFixed(CallTempReg0));
        break;
      case 3:
        lir = new(alloc()) LHypot(useRegisterAtStart(ins->getOperand(0)),
                                  useRegisterAtStart(ins->getOperand(1)),
                                  useRegisterAtStart(ins->getOperand(2)),
                                  tempFixed(CallTempReg0));
        break;
      case 4:
        lir = new(alloc()) LHypot(useRegisterAtStart(ins->getOperand(0)),
                                  useRegisterAtStart(ins->getOperand(1)),
                                  useRegisterAtStart(ins->getOperand(2)),
                                  useRegisterAtStart(ins->getOperand(3)),
                                  tempFixed(CallTempReg0));
        break;
      default:
        MOZ_CRASH("Unexpected number of arguments to LHypot.");
    }

    defineReturn(lir, ins);
}

void
LIRGenerator::visitPow(MPow* ins)
{
    MDefinition* input = ins->input();
    MOZ_ASSERT(input->type() == MIRType_Double);

    MDefinition* power = ins->power();
    MOZ_ASSERT(power->type() == MIRType_Int32 || power->type() == MIRType_Double);

    LInstruction* lir;
    if (power->type() == MIRType_Int32) {
        // Note: useRegisterAtStart here is safe, the temp is a GP register so
        // it will never get the same register.
        lir = new(alloc()) LPowI(useRegisterAtStart(input), useFixed(power, CallTempReg1),
                                 tempFixed(CallTempReg0));
    } else {
        lir = new(alloc()) LPowD(useRegisterAtStart(input), useRegisterAtStart(power),
                                 tempFixed(CallTempReg0));
    }
    defineReturn(lir, ins);
}

void
LIRGenerator::visitMathFunction(MMathFunction* ins)
{
    MOZ_ASSERT(IsFloatingPointType(ins->type()));
    MOZ_ASSERT_IF(ins->input()->type() != MIRType_SinCosDouble,
                  ins->type() == ins->input()->type());

    if (ins->input()->type() == MIRType_SinCosDouble) {
        MOZ_ASSERT(ins->type() == MIRType_Double);
        redefine(ins, ins->input(), ins->function());
        return;
    }

    LInstruction* lir;
    if (ins->type() == MIRType_Double) {
        // Note: useRegisterAtStart is safe here, the temp is not a FP register.
        lir = new(alloc()) LMathFunctionD(useRegisterAtStart(ins->input()),
                                          tempFixed(CallTempReg0));
    } else {
        lir = new(alloc()) LMathFunctionF(useRegisterAtStart(ins->input()),
                                          tempFixed(CallTempReg0));
    }
    defineReturn(lir, ins);
}

// Try to mark an add or sub instruction as able to recover its input when
// bailing out.
template <typename S, typename T>
static void
MaybeSetRecoversInput(S* mir, T* lir)
{
    MOZ_ASSERT(lir->mirRaw() == mir);
    if (!mir->fallible() || !lir->snapshot())
        return;

    if (lir->output()->policy() != LDefinition::MUST_REUSE_INPUT)
        return;

    // The original operands to an add or sub can't be recovered if they both
    // use the same register.
    if (lir->lhs()->isUse() && lir->rhs()->isUse() &&
        lir->lhs()->toUse()->virtualRegister() == lir->rhs()->toUse()->virtualRegister())
    {
        return;
    }

    // Add instructions that are on two different values can recover
    // the input they clobbered via MUST_REUSE_INPUT. Thus, a copy
    // of that input does not need to be kept alive in the snapshot
    // for the instruction.

    lir->setRecoversInput();

    const LUse* input = lir->getOperand(lir->output()->getReusedInput())->toUse();
    lir->snapshot()->rewriteRecoveredInput(*input);
}

void
LIRGenerator::visitAdd(MAdd* ins)
{
    MDefinition* lhs = ins->getOperand(0);
    MDefinition* rhs = ins->getOperand(1);

    MOZ_ASSERT(lhs->type() == rhs->type());

    if (ins->specialization() == MIRType_Int32) {
        MOZ_ASSERT(lhs->type() == MIRType_Int32);
        ReorderCommutative(&lhs, &rhs, ins);
        LAddI* lir = new(alloc()) LAddI;

        if (ins->fallible())
            assignSnapshot(lir, Bailout_OverflowInvalidate);

        lowerForALU(lir, ins, lhs, rhs);
        MaybeSetRecoversInput(ins, lir);
        return;
    }

    if (ins->specialization() == MIRType_Double) {
        MOZ_ASSERT(lhs->type() == MIRType_Double);
        ReorderCommutative(&lhs, &rhs, ins);
        lowerForFPU(new(alloc()) LMathD(JSOP_ADD), ins, lhs, rhs);
    } else if (ins->specialization() == MIRType_Float32) {
        MOZ_ASSERT(lhs->type() == MIRType_Float32);
        ReorderCommutative(&lhs, &rhs, ins);
        lowerForFPU(new(alloc()) LMathF(JSOP_ADD), ins, lhs, rhs);
    } else {
        lowerBinaryV(JSOP_ADD, ins);
    }
}

void
LIRGenerator::visitSub(MSub* ins)
{
    MDefinition* lhs = ins->lhs();
    MDefinition* rhs = ins->rhs();

    MOZ_ASSERT(lhs->type() == rhs->type());

    if (ins->specialization() == MIRType_Int32) {
        MOZ_ASSERT(lhs->type() == MIRType_Int32);

        LSubI* lir = new(alloc()) LSubI;
        if (ins->fallible())
            assignSnapshot(lir, Bailout_Overflow);

        lowerForALU(lir, ins, lhs, rhs);
        MaybeSetRecoversInput(ins, lir);
        return;
    }

    if (ins->specialization() == MIRType_Double) {
        MOZ_ASSERT(lhs->type() == MIRType_Double);
        lowerForFPU(new(alloc()) LMathD(JSOP_SUB), ins, lhs, rhs);
    } else if (ins->specialization() == MIRType_Float32) {
        MOZ_ASSERT(lhs->type() == MIRType_Float32);
        lowerForFPU(new(alloc()) LMathF(JSOP_SUB), ins, lhs, rhs);
    } else {
        lowerBinaryV(JSOP_SUB, ins);
    }
}

void
LIRGenerator::visitMul(MMul* ins)
{
    MDefinition* lhs = ins->lhs();
    MDefinition* rhs = ins->rhs();
    MOZ_ASSERT(lhs->type() == rhs->type());

    if (ins->specialization() == MIRType_Int32) {
        MOZ_ASSERT(lhs->type() == MIRType_Int32);
        ReorderCommutative(&lhs, &rhs, ins);

        // If our RHS is a constant -1 and we don't have to worry about
        // overflow, we can optimize to an LNegI.
        if (!ins->fallible() && rhs->isConstantValue() && rhs->constantValue() == Int32Value(-1))
            defineReuseInput(new(alloc()) LNegI(useRegisterAtStart(lhs)), ins, 0);
        else
            lowerMulI(ins, lhs, rhs);
    } else if (ins->specialization() == MIRType_Double) {
        MOZ_ASSERT(lhs->type() == MIRType_Double);
        ReorderCommutative(&lhs, &rhs, ins);

        // If our RHS is a constant -1.0, we can optimize to an LNegD.
        if (rhs->isConstantValue() && rhs->constantValue() == DoubleValue(-1.0))
            defineReuseInput(new(alloc()) LNegD(useRegisterAtStart(lhs)), ins, 0);
        else
            lowerForFPU(new(alloc()) LMathD(JSOP_MUL), ins, lhs, rhs);
    } else if (ins->specialization() == MIRType_Float32) {
        MOZ_ASSERT(lhs->type() == MIRType_Float32);
        ReorderCommutative(&lhs, &rhs, ins);

        // We apply the same optimizations as for doubles
        if (rhs->isConstantValue() && rhs->constantValue() == Float32Value(-1.0f))
            defineReuseInput(new(alloc()) LNegF(useRegisterAtStart(lhs)), ins, 0);
        else
            lowerForFPU(new(alloc()) LMathF(JSOP_MUL), ins, lhs, rhs);
    } else {
        lowerBinaryV(JSOP_MUL, ins);
    }
}

void
LIRGenerator::visitDiv(MDiv* ins)
{
    MDefinition* lhs = ins->lhs();
    MDefinition* rhs = ins->rhs();
    MOZ_ASSERT(lhs->type() == rhs->type());

    if (ins->specialization() == MIRType_Int32) {
        MOZ_ASSERT(lhs->type() == MIRType_Int32);
        lowerDivI(ins);
    } else if (ins->specialization() == MIRType_Double) {
        MOZ_ASSERT(lhs->type() == MIRType_Double);
        lowerForFPU(new(alloc()) LMathD(JSOP_DIV), ins, lhs, rhs);
    } else if (ins->specialization() == MIRType_Float32) {
        MOZ_ASSERT(lhs->type() == MIRType_Float32);
        lowerForFPU(new(alloc()) LMathF(JSOP_DIV), ins, lhs, rhs);
    } else {
        lowerBinaryV(JSOP_DIV, ins);
    }
}

void
LIRGenerator::visitMod(MMod* ins)
{
    MOZ_ASSERT(ins->lhs()->type() == ins->rhs()->type());

    if (ins->specialization() == MIRType_Int32) {
        MOZ_ASSERT(ins->type() == MIRType_Int32);
        MOZ_ASSERT(ins->lhs()->type() == MIRType_Int32);
        lowerModI(ins);
    } else if (ins->specialization() == MIRType_Double) {
        MOZ_ASSERT(ins->type() == MIRType_Double);
        MOZ_ASSERT(ins->lhs()->type() == MIRType_Double);
        MOZ_ASSERT(ins->rhs()->type() == MIRType_Double);

        // Note: useRegisterAtStart is safe here, the temp is not a FP register.
        LModD* lir = new(alloc()) LModD(useRegisterAtStart(ins->lhs()), useRegisterAtStart(ins->rhs()),
                                        tempFixed(CallTempReg0));
        defineReturn(lir, ins);
    } else {
        lowerBinaryV(JSOP_MOD, ins);
    }
}

void
LIRGenerator::lowerBinaryV(JSOp op, MBinaryInstruction* ins)
{
    MDefinition* lhs = ins->getOperand(0);
    MDefinition* rhs = ins->getOperand(1);

    MOZ_ASSERT(lhs->type() == MIRType_Value);
    MOZ_ASSERT(rhs->type() == MIRType_Value);

    LBinaryV* lir = new(alloc()) LBinaryV(op);
    useBoxAtStart(lir, LBinaryV::LhsInput, lhs);
    useBoxAtStart(lir, LBinaryV::RhsInput, rhs);
    defineReturn(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitConcat(MConcat* ins)
{
    MDefinition* lhs = ins->getOperand(0);
    MDefinition* rhs = ins->getOperand(1);

    MOZ_ASSERT(lhs->type() == MIRType_String);
    MOZ_ASSERT(rhs->type() == MIRType_String);
    MOZ_ASSERT(ins->type() == MIRType_String);

    LConcat* lir = new(alloc()) LConcat(useFixedAtStart(lhs, CallTempReg0),
                                        useFixedAtStart(rhs, CallTempReg1),
                                        tempFixed(CallTempReg0),
                                        tempFixed(CallTempReg1),
                                        tempFixed(CallTempReg2),
                                        tempFixed(CallTempReg3),
                                        tempFixed(CallTempReg4));
    defineFixed(lir, ins, LAllocation(AnyRegister(CallTempReg5)));
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitCharCodeAt(MCharCodeAt* ins)
{
    MDefinition* str = ins->getOperand(0);
    MDefinition* idx = ins->getOperand(1);

    MOZ_ASSERT(str->type() == MIRType_String);
    MOZ_ASSERT(idx->type() == MIRType_Int32);

    LCharCodeAt* lir = new(alloc()) LCharCodeAt(useRegister(str), useRegister(idx));
    define(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitFromCharCode(MFromCharCode* ins)
{
    MDefinition* code = ins->getOperand(0);

    MOZ_ASSERT(code->type() == MIRType_Int32);

    LFromCharCode* lir = new(alloc()) LFromCharCode(useRegister(code));
    define(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitStart(MStart* start)
{
    // Create a snapshot that captures the initial state of the function.
    LStart* lir = new(alloc()) LStart;
    assignSnapshot(lir, Bailout_InitialState);

    if (start->startType() == MStart::StartType_Default && lir->snapshot())
        lirGraph_.setEntrySnapshot(lir->snapshot());
    add(lir);
}

void
LIRGenerator::visitNop(MNop* nop)
{
}

void
LIRGenerator::visitLimitedTruncate(MLimitedTruncate* nop)
{
    redefine(nop, nop->input());
}

void
LIRGenerator::visitOsrEntry(MOsrEntry* entry)
{
    LOsrEntry* lir = new(alloc()) LOsrEntry(temp());
    defineFixed(lir, entry, LAllocation(AnyRegister(OsrFrameReg)));
}

void
LIRGenerator::visitOsrValue(MOsrValue* value)
{
    LOsrValue* lir = new(alloc()) LOsrValue(useRegister(value->entry()));
    defineBox(lir, value);
}

void
LIRGenerator::visitOsrReturnValue(MOsrReturnValue* value)
{
    LOsrReturnValue* lir = new(alloc()) LOsrReturnValue(useRegister(value->entry()));
    defineBox(lir, value);
}

void
LIRGenerator::visitOsrScopeChain(MOsrScopeChain* object)
{
    LOsrScopeChain* lir = new(alloc()) LOsrScopeChain(useRegister(object->entry()));
    define(lir, object);
}

void
LIRGenerator::visitOsrArgumentsObject(MOsrArgumentsObject* object)
{
    LOsrArgumentsObject* lir = new(alloc()) LOsrArgumentsObject(useRegister(object->entry()));
    define(lir, object);
}

void
LIRGenerator::visitToDouble(MToDouble* convert)
{
    MDefinition* opd = convert->input();
    mozilla::DebugOnly<MToFPInstruction::ConversionKind> conversion = convert->conversion();

    switch (opd->type()) {
      case MIRType_Value:
      {
        LValueToDouble* lir = new(alloc()) LValueToDouble();
        useBox(lir, LValueToDouble::Input, opd);
        assignSnapshot(lir, Bailout_NonPrimitiveInput);
        define(lir, convert);
        break;
      }

      case MIRType_Null:
        MOZ_ASSERT(conversion != MToFPInstruction::NumbersOnly &&
                   conversion != MToFPInstruction::NonNullNonStringPrimitives);
        lowerConstantDouble(0, convert);
        break;

      case MIRType_Undefined:
        MOZ_ASSERT(conversion != MToFPInstruction::NumbersOnly);
        lowerConstantDouble(GenericNaN(), convert);
        break;

      case MIRType_Boolean:
        MOZ_ASSERT(conversion != MToFPInstruction::NumbersOnly);
        /* FALLTHROUGH */

      case MIRType_Int32:
      {
        LInt32ToDouble* lir = new(alloc()) LInt32ToDouble(useRegisterAtStart(opd));
        define(lir, convert);
        break;
      }

      case MIRType_Float32:
      {
        LFloat32ToDouble* lir = new (alloc()) LFloat32ToDouble(useRegisterAtStart(opd));
        define(lir, convert);
        break;
      }

      case MIRType_Double:
        redefine(convert, opd);
        break;

      default:
        // Objects might be effectful. Symbols will throw.
        // Strings are complicated - we don't handle them yet.
        MOZ_CRASH("unexpected type");
    }
}

void
LIRGenerator::visitToFloat32(MToFloat32* convert)
{
    MDefinition* opd = convert->input();
    mozilla::DebugOnly<MToFloat32::ConversionKind> conversion = convert->conversion();

    switch (opd->type()) {
      case MIRType_Value:
      {
        LValueToFloat32* lir = new(alloc()) LValueToFloat32();
        useBox(lir, LValueToFloat32::Input, opd);
        assignSnapshot(lir, Bailout_NonPrimitiveInput);
        define(lir, convert);
        break;
      }

      case MIRType_Null:
        MOZ_ASSERT(conversion != MToFPInstruction::NumbersOnly &&
                   conversion != MToFPInstruction::NonNullNonStringPrimitives);
        lowerConstantFloat32(0, convert);
        break;

      case MIRType_Undefined:
        MOZ_ASSERT(conversion != MToFPInstruction::NumbersOnly);
        lowerConstantFloat32(GenericNaN(), convert);
        break;

      case MIRType_Boolean:
        MOZ_ASSERT(conversion != MToFPInstruction::NumbersOnly);
        /* FALLTHROUGH */

      case MIRType_Int32:
      {
        LInt32ToFloat32* lir = new(alloc()) LInt32ToFloat32(useRegisterAtStart(opd));
        define(lir, convert);
        break;
      }

      case MIRType_Double:
      {
        LDoubleToFloat32* lir = new(alloc()) LDoubleToFloat32(useRegisterAtStart(opd));
        define(lir, convert);
        break;
      }

      case MIRType_Float32:
        redefine(convert, opd);
        break;

      default:
        // Objects might be effectful. Symbols will throw.
        // Strings are complicated - we don't handle them yet.
        MOZ_CRASH("unexpected type");
    }
}

void
LIRGenerator::visitToInt32(MToInt32* convert)
{
    MDefinition* opd = convert->input();

    switch (opd->type()) {
      case MIRType_Value:
      {
        LValueToInt32* lir =
            new(alloc()) LValueToInt32(tempDouble(), temp(), LValueToInt32::NORMAL);
        useBox(lir, LValueToInt32::Input, opd);
        assignSnapshot(lir, Bailout_NonPrimitiveInput);
        define(lir, convert);
        assignSafepoint(lir, convert);
        break;
      }

      case MIRType_Null:
        MOZ_ASSERT(convert->conversion() == MacroAssembler::IntConversion_Any);
        define(new(alloc()) LInteger(0), convert);
        break;

      case MIRType_Boolean:
        MOZ_ASSERT(convert->conversion() == MacroAssembler::IntConversion_Any ||
                   convert->conversion() == MacroAssembler::IntConversion_NumbersOrBoolsOnly);
        redefine(convert, opd);
        break;

      case MIRType_Int32:
        redefine(convert, opd);
        break;

      case MIRType_Float32:
      {
        LFloat32ToInt32* lir = new(alloc()) LFloat32ToInt32(useRegister(opd));
        assignSnapshot(lir, Bailout_PrecisionLoss);
        define(lir, convert);
        break;
      }

      case MIRType_Double:
      {
        LDoubleToInt32* lir = new(alloc()) LDoubleToInt32(useRegister(opd));
        assignSnapshot(lir, Bailout_PrecisionLoss);
        define(lir, convert);
        break;
      }

      case MIRType_String:
      case MIRType_Symbol:
      case MIRType_Object:
      case MIRType_Undefined:
        // Objects might be effectful. Symbols throw. Undefined coerces to NaN, not int32.
        MOZ_CRASH("ToInt32 invalid input type");

      default:
        MOZ_CRASH("unexpected type");
    }
}

void
LIRGenerator::visitTruncateToInt32(MTruncateToInt32* truncate)
{
    MDefinition* opd = truncate->input();

    switch (opd->type()) {
      case MIRType_Value:
      {
        LValueToInt32* lir = new(alloc()) LValueToInt32(tempDouble(), temp(),
                                                        LValueToInt32::TRUNCATE);
        useBox(lir, LValueToInt32::Input, opd);
        assignSnapshot(lir, Bailout_NonPrimitiveInput);
        define(lir, truncate);
        assignSafepoint(lir, truncate);
        break;
      }

      case MIRType_Null:
      case MIRType_Undefined:
        define(new(alloc()) LInteger(0), truncate);
        break;

      case MIRType_Int32:
      case MIRType_Boolean:
        redefine(truncate, opd);
        break;

      case MIRType_Double:
        lowerTruncateDToInt32(truncate);
        break;

      case MIRType_Float32:
        lowerTruncateFToInt32(truncate);
        break;

      default:
        // Objects might be effectful. Symbols throw.
        // Strings are complicated - we don't handle them yet.
        MOZ_CRASH("unexpected type");
    }
}

void
LIRGenerator::visitToString(MToString* ins)
{
    MDefinition* opd = ins->input();

    switch (opd->type()) {
      case MIRType_Null: {
        const JSAtomState& names = GetJitContext()->runtime->names();
        LPointer* lir = new(alloc()) LPointer(names.null);
        define(lir, ins);
        break;
      }

      case MIRType_Undefined: {
        const JSAtomState& names = GetJitContext()->runtime->names();
        LPointer* lir = new(alloc()) LPointer(names.undefined);
        define(lir, ins);
        break;
      }

      case MIRType_Boolean: {
        LBooleanToString* lir = new(alloc()) LBooleanToString(useRegister(opd));
        define(lir, ins);
        break;
      }

      case MIRType_Double: {
        LDoubleToString* lir = new(alloc()) LDoubleToString(useRegister(opd), temp());

        define(lir, ins);
        assignSafepoint(lir, ins);
        break;
      }

      case MIRType_Int32: {
        LIntToString* lir = new(alloc()) LIntToString(useRegister(opd));

        define(lir, ins);
        assignSafepoint(lir, ins);
        break;
      }

      case MIRType_String:
        redefine(ins, ins->input());
        break;

      case MIRType_Value: {
        LValueToString* lir = new(alloc()) LValueToString(tempToUnbox());
        useBox(lir, LValueToString::Input, opd);
        if (ins->fallible())
            assignSnapshot(lir, Bailout_NonPrimitiveInput);
        define(lir, ins);
        assignSafepoint(lir, ins);
        break;
      }

      default:
        // Float32, symbols, and objects are not supported.
        MOZ_CRASH("unexpected type");
    }
}

void
LIRGenerator::visitToObjectOrNull(MToObjectOrNull* ins)
{
    MOZ_ASSERT(ins->input()->type() == MIRType_Value);

    LValueToObjectOrNull* lir = new(alloc()) LValueToObjectOrNull();
    useBox(lir, LValueToString::Input, ins->input());
    define(lir, ins);
    assignSafepoint(lir, ins);
}

static bool
MustCloneRegExpForCall(MCall* call, uint32_t useIndex)
{
    // We have a regex literal flowing into a call. Return |false| iff
    // this is a native call that does not let the regex escape.

    JSFunction* target = call->getSingleTarget();
    if (!target || !target->isNative())
        return true;

    if (useIndex == MCall::IndexOfThis() &&
        (target->native() == regexp_exec || target->native() == regexp_test))
    {
        return false;
    }

    if (useIndex == MCall::IndexOfArgument(0) &&
        (target->native() == str_split ||
         target->native() == str_replace ||
         target->native() == str_match ||
         target->native() == str_search))
    {
        return false;
    }

    return true;
}


static bool
MustCloneRegExp(MRegExp* regexp)
{
    if (regexp->mustClone())
        return true;

    // If this regex literal only flows into known natives that don't let
    // it escape, we don't have to clone it.

    for (MUseIterator iter(regexp->usesBegin()); iter != regexp->usesEnd(); iter++) {
        MNode* node = iter->consumer();
        if (!node->isDefinition())
            return true;

        MDefinition* def = node->toDefinition();
        if (def->isRegExpTest()) {
            MRegExpTest* test = def->toRegExpTest();
            if (test->indexOf(*iter) == 1) {
                // Optimized RegExp.prototype.test.
                MOZ_ASSERT(test->regexp() == regexp);
                continue;
            }
        } else if (def->isCall()) {
            MCall* call = def->toCall();
            if (!MustCloneRegExpForCall(call, call->indexOf(*iter)))
                continue;
        }

        return true;
    }
    return false;
}

void
LIRGenerator::visitRegExp(MRegExp* ins)
{
    if (!MustCloneRegExp(ins)) {
        RegExpObject* source = ins->source();
        define(new(alloc()) LPointer(source), ins);
    } else {
        LRegExp* lir = new(alloc()) LRegExp();
        defineReturn(lir, ins);
        assignSafepoint(lir, ins);
    }
}

void
LIRGenerator::visitRegExpExec(MRegExpExec* ins)
{
    MOZ_ASSERT(ins->regexp()->type() == MIRType_Object);
    MOZ_ASSERT(ins->string()->type() == MIRType_String);

    LRegExpExec* lir = new(alloc()) LRegExpExec(useFixedAtStart(ins->regexp(), CallTempReg0),
                                                useFixedAtStart(ins->string(), CallTempReg1));
    defineReturn(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitRegExpTest(MRegExpTest* ins)
{
    MOZ_ASSERT(ins->regexp()->type() == MIRType_Object);
    MOZ_ASSERT(ins->string()->type() == MIRType_String);

    LRegExpTest* lir = new(alloc()) LRegExpTest(useFixedAtStart(ins->regexp(), CallTempReg2),
                                                useFixedAtStart(ins->string(), CallTempReg3));
    defineReturn(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitRegExpReplace(MRegExpReplace* ins)
{
    MOZ_ASSERT(ins->pattern()->type() == MIRType_Object);
    MOZ_ASSERT(ins->string()->type() == MIRType_String);
    MOZ_ASSERT(ins->replacement()->type() == MIRType_String);

    LRegExpReplace* lir = new(alloc()) LRegExpReplace(useRegisterOrConstantAtStart(ins->string()),
                                                      useRegisterAtStart(ins->pattern()),
                                                      useRegisterOrConstantAtStart(ins->replacement()));
    defineReturn(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitStringReplace(MStringReplace* ins)
{
    MOZ_ASSERT(ins->pattern()->type() == MIRType_String);
    MOZ_ASSERT(ins->string()->type() == MIRType_String);
    MOZ_ASSERT(ins->replacement()->type() == MIRType_String);

    LStringReplace* lir = new(alloc()) LStringReplace(useRegisterOrConstantAtStart(ins->string()),
                                                      useRegisterAtStart(ins->pattern()),
                                                      useRegisterOrConstantAtStart(ins->replacement()));
    defineReturn(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitBinarySharedStub(MBinarySharedStub* ins)
{
    MDefinition* lhs = ins->getOperand(0);
    MDefinition* rhs = ins->getOperand(1);

    MOZ_ASSERT(ins->type() == MIRType_Value);
    MOZ_ASSERT(ins->type() == MIRType_Value);

    LBinarySharedStub* lir = new(alloc()) LBinarySharedStub();

    useBoxFixedAtStart(lir, LBinarySharedStub::LhsInput, lhs, R0);
    useBoxFixedAtStart(lir, LBinarySharedStub::RhsInput, rhs, R1);

    defineSharedStubReturn(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitUnarySharedStub(MUnarySharedStub* ins)
{
    MDefinition* input = ins->getOperand(0);
    MOZ_ASSERT(ins->type() == MIRType_Value);

    LUnarySharedStub* lir = new(alloc()) LUnarySharedStub();

    useBoxFixedAtStart(lir, LUnarySharedStub::Input, input, R0);

    defineSharedStubReturn(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitLambda(MLambda* ins)
{
    if (ins->info().singletonType || ins->info().useSingletonForClone) {
        // If the function has a singleton type, this instruction will only be
        // executed once so we don't bother inlining it.
        //
        // If UseSingletonForClone is true, we will assign a singleton type to
        // the clone and we have to clone the script, we can't do that inline.
        LLambdaForSingleton* lir = new(alloc()) LLambdaForSingleton(useRegisterAtStart(ins->scopeChain()));
        defineReturn(lir, ins);
        assignSafepoint(lir, ins);
    } else {
        LLambda* lir = new(alloc()) LLambda(useRegister(ins->scopeChain()), temp());
        define(lir, ins);
        assignSafepoint(lir, ins);
    }
}

void
LIRGenerator::visitLambdaArrow(MLambdaArrow* ins)
{
    MOZ_ASSERT(ins->scopeChain()->type() == MIRType_Object);
    MOZ_ASSERT(ins->newTargetDef()->type() == MIRType_Value);

    LLambdaArrow* lir = new(alloc()) LLambdaArrow(useRegister(ins->scopeChain()));
    useBox(lir, LLambdaArrow::NewTargetValue, ins->newTargetDef());
    define(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitKeepAliveObject(MKeepAliveObject* ins)
{
    MDefinition* obj = ins->object();
    MOZ_ASSERT(obj->type() == MIRType_Object);

    add(new(alloc()) LKeepAliveObject(useKeepalive(obj)), ins);
}

void
LIRGenerator::visitSlots(MSlots* ins)
{
    define(new(alloc()) LSlots(useRegisterAtStart(ins->object())), ins);
}

void
LIRGenerator::visitElements(MElements* ins)
{
    define(new(alloc()) LElements(useRegisterAtStart(ins->object())), ins);
}

void
LIRGenerator::visitConstantElements(MConstantElements* ins)
{
    define(new(alloc()) LPointer(ins->value().unwrap(/*safe - pointer does not flow back to C++*/),
                                 LPointer::NON_GC_THING),
           ins);
}

void
LIRGenerator::visitConvertElementsToDoubles(MConvertElementsToDoubles* ins)
{
    LInstruction* check = new(alloc()) LConvertElementsToDoubles(useRegister(ins->elements()));
    add(check, ins);
    assignSafepoint(check, ins);
}

void
LIRGenerator::visitMaybeToDoubleElement(MMaybeToDoubleElement* ins)
{
    MOZ_ASSERT(ins->elements()->type() == MIRType_Elements);
    MOZ_ASSERT(ins->value()->type() == MIRType_Int32);

    LMaybeToDoubleElement* lir = new(alloc()) LMaybeToDoubleElement(useRegisterAtStart(ins->elements()),
                                                                    useRegisterAtStart(ins->value()),
                                                                    tempDouble());
    defineBox(lir, ins);
}

void
LIRGenerator::visitMaybeCopyElementsForWrite(MMaybeCopyElementsForWrite* ins)
{
    LInstruction* check = new(alloc()) LMaybeCopyElementsForWrite(useRegister(ins->object()), temp());
    add(check, ins);
    assignSafepoint(check, ins);
}

void
LIRGenerator::visitLoadSlot(MLoadSlot* ins)
{
    switch (ins->type()) {
      case MIRType_Value:
        defineBox(new(alloc()) LLoadSlotV(useRegisterAtStart(ins->slots())), ins);
        break;

      case MIRType_Undefined:
      case MIRType_Null:
        MOZ_CRASH("typed load must have a payload");

      default:
        define(new(alloc()) LLoadSlotT(useRegisterForTypedLoad(ins->slots(), ins->type())), ins);
        break;
    }
}

void
LIRGenerator::visitFunctionEnvironment(MFunctionEnvironment* ins)
{
    define(new(alloc()) LFunctionEnvironment(useRegisterAtStart(ins->function())), ins);
}

void
LIRGenerator::visitInterruptCheck(MInterruptCheck* ins)
{
    // Implicit interrupt checks require asm.js signal handlers to be installed.
    // They also require writable JIT code: reprotecting in patchIonBackedges
    // would be expensive and using AutoWritableJitCode in the signal handler
    // is complicated because there could be another AutoWritableJitCode on the
    // stack.
    LInstructionHelper<0, 0, 0>* lir;
    if (GetJitContext()->runtime->canUseSignalHandlers() &&
        !ExecutableAllocator::nonWritableJitCode)
    {
        lir = new(alloc()) LInterruptCheckImplicit();
    } else {
        lir = new(alloc()) LInterruptCheck();
    }
    add(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitAsmJSInterruptCheck(MAsmJSInterruptCheck* ins)
{
    gen->setPerformsCall();

    LAsmJSInterruptCheck* lir = new(alloc()) LAsmJSInterruptCheck(ins->interruptExit(),
                                                                  ins->funcDesc());
    add(lir, ins);
}

void
LIRGenerator::visitStoreSlot(MStoreSlot* ins)
{
    LInstruction* lir;

    switch (ins->value()->type()) {
      case MIRType_Value:
        lir = new(alloc()) LStoreSlotV(useRegister(ins->slots()));
        useBox(lir, LStoreSlotV::Value, ins->value());
        add(lir, ins);
        break;

      case MIRType_Double:
        add(new(alloc()) LStoreSlotT(useRegister(ins->slots()), useRegister(ins->value())), ins);
        break;

      case MIRType_Float32:
        MOZ_CRASH("Float32 shouldn't be stored in a slot.");

      default:
        add(new(alloc()) LStoreSlotT(useRegister(ins->slots()),
                                     useRegisterOrConstant(ins->value())), ins);
        break;
    }
}

void
LIRGenerator::visitFilterTypeSet(MFilterTypeSet* ins)
{
    redefine(ins, ins->input());
}

void
LIRGenerator::visitTypeBarrier(MTypeBarrier* ins)
{
    // Requesting a non-GC pointer is safe here since we never re-enter C++
    // from inside a type barrier test.

    const TemporaryTypeSet* types = ins->resultTypeSet();
    bool needTemp = !types->unknownObject() && types->getObjectCount() > 0;

    MIRType inputType = ins->getOperand(0)->type();
    DebugOnly<MIRType> outputType = ins->type();

    MOZ_ASSERT(inputType == outputType);

    // Handle typebarrier that will always bail.
    // (Emit LBail for visibility).
    if (ins->alwaysBails()) {
        LBail* bail = new(alloc()) LBail();
        assignSnapshot(bail, Bailout_Inevitable);
        add(bail, ins);
        redefine(ins, ins->input());
        return;
    }

    // Handle typebarrier with Value as input.
    if (inputType == MIRType_Value) {
        LDefinition tmp = needTemp ? temp() : tempToUnbox();
        LTypeBarrierV* barrier = new(alloc()) LTypeBarrierV(tmp);
        useBox(barrier, LTypeBarrierV::Input, ins->input());
        assignSnapshot(barrier, Bailout_TypeBarrierV);
        add(barrier, ins);
        redefine(ins, ins->input());
        return;
    }

    // The payload needs to be tested if it either might be null or might have
    // an object that should be excluded from the barrier.
    bool needsObjectBarrier = false;
    if (inputType == MIRType_ObjectOrNull)
        needsObjectBarrier = true;
    if (inputType == MIRType_Object && !types->hasType(TypeSet::AnyObjectType()) &&
        ins->barrierKind() != BarrierKind::TypeTagOnly)
    {
        needsObjectBarrier = true;
    }

    if (needsObjectBarrier) {
        LDefinition tmp = needTemp ? temp() : LDefinition::BogusTemp();
        LTypeBarrierO* barrier = new(alloc()) LTypeBarrierO(useRegister(ins->getOperand(0)), tmp);
        assignSnapshot(barrier, Bailout_TypeBarrierO);
        add(barrier, ins);
        redefine(ins, ins->getOperand(0));
        return;
    }

    // Handle remaining cases: No-op, unbox did everything.
    redefine(ins, ins->getOperand(0));
}

void
LIRGenerator::visitMonitorTypes(MMonitorTypes* ins)
{
    // Requesting a non-GC pointer is safe here since we never re-enter C++
    // from inside a type check.

    const TemporaryTypeSet* types = ins->typeSet();
    bool needTemp = !types->unknownObject() && types->getObjectCount() > 0;
    LDefinition tmp = needTemp ? temp() : tempToUnbox();

    LMonitorTypes* lir = new(alloc()) LMonitorTypes(tmp);
    useBox(lir, LMonitorTypes::Input, ins->input());
    assignSnapshot(lir, Bailout_MonitorTypes);
    add(lir, ins);
}

// Returns true iff |def| is a constant that's either not a GC thing or is not
// allocated in the nursery.
static bool
IsNonNurseryConstant(MDefinition* def)
{
    if (!def->isConstant())
        return false;
    Value v = def->toConstant()->value();
    return !v.isMarkable() || !IsInsideNursery(v.toGCThing());
}

void
LIRGenerator::visitPostWriteBarrier(MPostWriteBarrier* ins)
{
    MOZ_ASSERT(ins->object()->type() == MIRType_Object);

    // LPostWriteBarrier assumes that if it has a constant object then that
    // object is tenured, and does not need to be tested for being in the
    // nursery. Ensure that assumption holds by lowering constant nursery
    // objects to a register.
    bool useConstantObject = IsNonNurseryConstant(ins->object());

    switch (ins->value()->type()) {
      case MIRType_Object:
      case MIRType_ObjectOrNull: {
        LDefinition tmp = needTempForPostBarrier() ? temp() : LDefinition::BogusTemp();
        LPostWriteBarrierO* lir =
            new(alloc()) LPostWriteBarrierO(useConstantObject
                                            ? useOrConstant(ins->object())
                                            : useRegister(ins->object()),
                                            useRegister(ins->value()), tmp);
        add(lir, ins);
        assignSafepoint(lir, ins);
        break;
      }
      case MIRType_Value: {
        LDefinition tmp = needTempForPostBarrier() ? temp() : LDefinition::BogusTemp();
        LPostWriteBarrierV* lir =
            new(alloc()) LPostWriteBarrierV(useConstantObject
                                            ? useOrConstant(ins->object())
                                            : useRegister(ins->object()),
                                            tmp);
        useBox(lir, LPostWriteBarrierV::Input, ins->value());
        add(lir, ins);
        assignSafepoint(lir, ins);
        break;
      }
      default:
        // Currently, only objects can be in the nursery. Other instruction
        // types cannot hold nursery pointers.
        break;
    }
}

void
LIRGenerator::visitArrayLength(MArrayLength* ins)
{
    MOZ_ASSERT(ins->elements()->type() == MIRType_Elements);
    define(new(alloc()) LArrayLength(useRegisterAtStart(ins->elements())), ins);
}

void
LIRGenerator::visitSetArrayLength(MSetArrayLength* ins)
{
    MOZ_ASSERT(ins->elements()->type() == MIRType_Elements);
    MOZ_ASSERT(ins->index()->type() == MIRType_Int32);

    MOZ_ASSERT(ins->index()->isConstant());
    add(new(alloc()) LSetArrayLength(useRegister(ins->elements()),
                                     useRegisterOrConstant(ins->index())), ins);
}

void
LIRGenerator::visitTypedArrayLength(MTypedArrayLength* ins)
{
    MOZ_ASSERT(ins->object()->type() == MIRType_Object);
    define(new(alloc()) LTypedArrayLength(useRegisterAtStart(ins->object())), ins);
}

void
LIRGenerator::visitTypedArrayElements(MTypedArrayElements* ins)
{
    MOZ_ASSERT(ins->type() == MIRType_Elements);
    define(new(alloc()) LTypedArrayElements(useRegisterAtStart(ins->object())), ins);
}

void
LIRGenerator::visitSetDisjointTypedElements(MSetDisjointTypedElements* ins)
{
    MOZ_ASSERT(ins->type() == MIRType_None);

    MDefinition* target = ins->target();
    MOZ_ASSERT(target->type() == MIRType_Object);

    MDefinition* targetOffset = ins->targetOffset();
    MOZ_ASSERT(targetOffset->type() == MIRType_Int32);

    MDefinition* source = ins->source();
    MOZ_ASSERT(source->type() == MIRType_Object);

    auto lir = new(alloc()) LSetDisjointTypedElements(useRegister(target),
                                                      useRegister(targetOffset),
                                                      useRegister(source),
                                                      temp());
    add(lir, ins);
}

void
LIRGenerator::visitTypedObjectDescr(MTypedObjectDescr* ins)
{
    MOZ_ASSERT(ins->type() == MIRType_Object);
    define(new(alloc()) LTypedObjectDescr(useRegisterAtStart(ins->object())), ins);
}

void
LIRGenerator::visitTypedObjectElements(MTypedObjectElements* ins)
{
    MOZ_ASSERT(ins->type() == MIRType_Elements);
    define(new(alloc()) LTypedObjectElements(useRegister(ins->object())), ins);
}

void
LIRGenerator::visitSetTypedObjectOffset(MSetTypedObjectOffset* ins)
{
    add(new(alloc()) LSetTypedObjectOffset(useRegister(ins->object()),
                                           useRegister(ins->offset()),
                                           temp(), temp()),
        ins);
}

void
LIRGenerator::visitInitializedLength(MInitializedLength* ins)
{
    MOZ_ASSERT(ins->elements()->type() == MIRType_Elements);
    define(new(alloc()) LInitializedLength(useRegisterAtStart(ins->elements())), ins);
}

void
LIRGenerator::visitSetInitializedLength(MSetInitializedLength* ins)
{
    MOZ_ASSERT(ins->elements()->type() == MIRType_Elements);
    MOZ_ASSERT(ins->index()->type() == MIRType_Int32);

    MOZ_ASSERT(ins->index()->isConstant());
    add(new(alloc()) LSetInitializedLength(useRegister(ins->elements()),
                                           useRegisterOrConstant(ins->index())), ins);
}

void
LIRGenerator::visitUnboxedArrayLength(MUnboxedArrayLength* ins)
{
    define(new(alloc()) LUnboxedArrayLength(useRegisterAtStart(ins->object())), ins);
}

void
LIRGenerator::visitUnboxedArrayInitializedLength(MUnboxedArrayInitializedLength* ins)
{
    define(new(alloc()) LUnboxedArrayInitializedLength(useRegisterAtStart(ins->object())), ins);
}

void
LIRGenerator::visitIncrementUnboxedArrayInitializedLength(MIncrementUnboxedArrayInitializedLength* ins)
{
    add(new(alloc()) LIncrementUnboxedArrayInitializedLength(useRegister(ins->object())), ins);
}

void
LIRGenerator::visitSetUnboxedArrayInitializedLength(MSetUnboxedArrayInitializedLength* ins)
{
    add(new(alloc()) LSetUnboxedArrayInitializedLength(useRegister(ins->object()),
                                                       useRegisterOrConstant(ins->length()),
                                                       temp()), ins);
}

void
LIRGenerator::visitNot(MNot* ins)
{
    MDefinition* op = ins->input();

    // String is converted to length of string in the type analysis phase (see
    // TestPolicy).
    MOZ_ASSERT(op->type() != MIRType_String);

    // - boolean: x xor 1
    // - int32: LCompare(x, 0)
    // - double: LCompare(x, 0)
    // - null or undefined: true
    // - object: false if it never emulates undefined, else LNotO(x)
    switch (op->type()) {
      case MIRType_Boolean: {
        MConstant* cons = MConstant::New(alloc(), Int32Value(1));
        ins->block()->insertBefore(ins, cons);
        lowerForALU(new(alloc()) LBitOpI(JSOP_BITXOR), ins, op, cons);
        break;
      }
      case MIRType_Int32:
        define(new(alloc()) LNotI(useRegisterAtStart(op)), ins);
        break;
      case MIRType_Double:
        define(new(alloc()) LNotD(useRegister(op)), ins);
        break;
      case MIRType_Float32:
        define(new(alloc()) LNotF(useRegister(op)), ins);
        break;
      case MIRType_Undefined:
      case MIRType_Null:
        define(new(alloc()) LInteger(1), ins);
        break;
      case MIRType_Symbol:
        define(new(alloc()) LInteger(0), ins);
        break;
      case MIRType_Object:
        if (!ins->operandMightEmulateUndefined()) {
            // Objects that don't emulate undefined can be constant-folded.
            define(new(alloc()) LInteger(0), ins);
        } else {
            // All others require further work.
            define(new(alloc()) LNotO(useRegister(op)), ins);
        }
        break;
      case MIRType_Value: {
        LDefinition temp0, temp1;
        if (ins->operandMightEmulateUndefined()) {
            temp0 = temp();
            temp1 = temp();
        } else {
            temp0 = LDefinition::BogusTemp();
            temp1 = LDefinition::BogusTemp();
        }

        LNotV* lir = new(alloc()) LNotV(tempDouble(), temp0, temp1);
        useBox(lir, LNotV::Input, op);
        define(lir, ins);
        break;
      }

      default:
        MOZ_CRASH("Unexpected MIRType.");
    }
}

void
LIRGenerator::visitBoundsCheck(MBoundsCheck* ins)
{
     if (!ins->fallible())
         return;

    LInstruction* check;
    if (ins->minimum() || ins->maximum()) {
        check = new(alloc()) LBoundsCheckRange(useRegisterOrConstant(ins->index()),
                                               useAny(ins->length()),
                                               temp());
    } else {
        check = new(alloc()) LBoundsCheck(useRegisterOrConstant(ins->index()),
                                          useAnyOrConstant(ins->length()));
    }
    assignSnapshot(check, Bailout_BoundsCheck);
    add(check, ins);
}

void
LIRGenerator::visitBoundsCheckLower(MBoundsCheckLower* ins)
{
    if (!ins->fallible())
        return;

    LInstruction* check = new(alloc()) LBoundsCheckLower(useRegister(ins->index()));
    assignSnapshot(check, Bailout_BoundsCheck);
    add(check, ins);
}

void
LIRGenerator::visitInArray(MInArray* ins)
{
    MOZ_ASSERT(ins->elements()->type() == MIRType_Elements);
    MOZ_ASSERT(ins->index()->type() == MIRType_Int32);
    MOZ_ASSERT(ins->initLength()->type() == MIRType_Int32);
    MOZ_ASSERT(ins->object()->type() == MIRType_Object);
    MOZ_ASSERT(ins->type() == MIRType_Boolean);

    LAllocation object;
    if (ins->needsNegativeIntCheck())
        object = useRegister(ins->object());

    LInArray* lir = new(alloc()) LInArray(useRegister(ins->elements()),
                                          useRegisterOrConstant(ins->index()),
                                          useRegister(ins->initLength()),
                                          object);
    define(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitLoadElement(MLoadElement* ins)
{
    MOZ_ASSERT(IsValidElementsType(ins->elements(), ins->offsetAdjustment()));
    MOZ_ASSERT(ins->index()->type() == MIRType_Int32);

    switch (ins->type()) {
      case MIRType_Value:
      {
        LLoadElementV* lir = new(alloc()) LLoadElementV(useRegister(ins->elements()),
                                                        useRegisterOrConstant(ins->index()));
        if (ins->fallible())
            assignSnapshot(lir, Bailout_Hole);
        defineBox(lir, ins);
        break;
      }
      case MIRType_Undefined:
      case MIRType_Null:
        MOZ_CRASH("typed load must have a payload");

      default:
      {
        LLoadElementT* lir = new(alloc()) LLoadElementT(useRegister(ins->elements()),
                                                        useRegisterOrConstant(ins->index()));
        if (ins->fallible())
            assignSnapshot(lir, Bailout_Hole);
        define(lir, ins);
        break;
      }
    }
}

void
LIRGenerator::visitLoadElementHole(MLoadElementHole* ins)
{
    MOZ_ASSERT(ins->elements()->type() == MIRType_Elements);
    MOZ_ASSERT(ins->index()->type() == MIRType_Int32);
    MOZ_ASSERT(ins->initLength()->type() == MIRType_Int32);
    MOZ_ASSERT(ins->type() == MIRType_Value);

    LLoadElementHole* lir = new(alloc()) LLoadElementHole(useRegister(ins->elements()),
                                                          useRegisterOrConstant(ins->index()),
                                                          useRegister(ins->initLength()));
    if (ins->needsNegativeIntCheck())
        assignSnapshot(lir, Bailout_NegativeIndex);
    defineBox(lir, ins);
}

void
LIRGenerator::visitLoadUnboxedObjectOrNull(MLoadUnboxedObjectOrNull* ins)
{
    MOZ_ASSERT(IsValidElementsType(ins->elements(), ins->offsetAdjustment()));
    MOZ_ASSERT(ins->index()->type() == MIRType_Int32);

    if (ins->type() == MIRType_Object || ins->type() == MIRType_ObjectOrNull) {
        LLoadUnboxedPointerT* lir = new(alloc()) LLoadUnboxedPointerT(useRegister(ins->elements()),
                                                                      useRegisterOrConstant(ins->index()));
        if (ins->nullBehavior() == MLoadUnboxedObjectOrNull::BailOnNull)
            assignSnapshot(lir, Bailout_TypeBarrierO);
        define(lir, ins);
    } else {
        MOZ_ASSERT(ins->type() == MIRType_Value);
        MOZ_ASSERT(ins->nullBehavior() != MLoadUnboxedObjectOrNull::BailOnNull);

        LLoadUnboxedPointerV* lir = new(alloc()) LLoadUnboxedPointerV(useRegister(ins->elements()),
                                                                      useRegisterOrConstant(ins->index()));
        defineBox(lir, ins);
    }
}

void
LIRGenerator::visitLoadUnboxedString(MLoadUnboxedString* ins)
{
    MOZ_ASSERT(IsValidElementsType(ins->elements(), ins->offsetAdjustment()));
    MOZ_ASSERT(ins->index()->type() == MIRType_Int32);
    MOZ_ASSERT(ins->type() == MIRType_String);

    LLoadUnboxedPointerT* lir = new(alloc()) LLoadUnboxedPointerT(useRegister(ins->elements()),
                                                                  useRegisterOrConstant(ins->index()));
    define(lir, ins);
}

void
LIRGenerator::visitStoreElement(MStoreElement* ins)
{
    MOZ_ASSERT(IsValidElementsType(ins->elements(), ins->offsetAdjustment()));
    MOZ_ASSERT(ins->index()->type() == MIRType_Int32);

    const LUse elements = useRegister(ins->elements());
    const LAllocation index = useRegisterOrConstant(ins->index());

    switch (ins->value()->type()) {
      case MIRType_Value:
      {
        LInstruction* lir = new(alloc()) LStoreElementV(elements, index);
        if (ins->fallible())
            assignSnapshot(lir, Bailout_Hole);
        useBox(lir, LStoreElementV::Value, ins->value());
        add(lir, ins);
        break;
      }

      default:
      {
        const LAllocation value = useRegisterOrNonDoubleConstant(ins->value());
        LInstruction* lir = new(alloc()) LStoreElementT(elements, index, value);
        if (ins->fallible())
            assignSnapshot(lir, Bailout_Hole);
        add(lir, ins);
        break;
      }
    }
}

void
LIRGenerator::visitStoreElementHole(MStoreElementHole* ins)
{
    MOZ_ASSERT(ins->elements()->type() == MIRType_Elements);
    MOZ_ASSERT(ins->index()->type() == MIRType_Int32);

    const LUse object = useRegister(ins->object());
    const LUse elements = useRegister(ins->elements());
    const LAllocation index = useRegisterOrConstant(ins->index());

    // Use a temp register when adding new elements to unboxed arrays.
    LDefinition tempDef = LDefinition::BogusTemp();
    if (ins->unboxedType() != JSVAL_TYPE_MAGIC)
        tempDef = temp();

    LInstruction* lir;
    switch (ins->value()->type()) {
      case MIRType_Value:
        lir = new(alloc()) LStoreElementHoleV(object, elements, index, tempDef);
        useBox(lir, LStoreElementHoleV::Value, ins->value());
        break;

      default:
      {
        const LAllocation value = useRegisterOrNonDoubleConstant(ins->value());
        lir = new(alloc()) LStoreElementHoleT(object, elements, index, value, tempDef);
        break;
      }
    }

    add(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitStoreUnboxedObjectOrNull(MStoreUnboxedObjectOrNull* ins)
{
    MOZ_ASSERT(IsValidElementsType(ins->elements(), ins->offsetAdjustment()));
    MOZ_ASSERT(ins->index()->type() == MIRType_Int32);
    MOZ_ASSERT(ins->value()->type() == MIRType_Object ||
               ins->value()->type() == MIRType_Null ||
               ins->value()->type() == MIRType_ObjectOrNull);

    const LUse elements = useRegister(ins->elements());
    const LAllocation index = useRegisterOrNonDoubleConstant(ins->index());
    const LAllocation value = useRegisterOrNonDoubleConstant(ins->value());

    LInstruction* lir = new(alloc()) LStoreUnboxedPointer(elements, index, value);
    add(lir, ins);
}

void
LIRGenerator::visitStoreUnboxedString(MStoreUnboxedString* ins)
{
    MOZ_ASSERT(IsValidElementsType(ins->elements(), ins->offsetAdjustment()));
    MOZ_ASSERT(ins->index()->type() == MIRType_Int32);
    MOZ_ASSERT(ins->value()->type() == MIRType_String);

    const LUse elements = useRegister(ins->elements());
    const LAllocation index = useRegisterOrConstant(ins->index());
    const LAllocation value = useRegisterOrNonDoubleConstant(ins->value());

    LInstruction* lir = new(alloc()) LStoreUnboxedPointer(elements, index, value);
    add(lir, ins);
}

void
LIRGenerator::visitConvertUnboxedObjectToNative(MConvertUnboxedObjectToNative* ins)
{
    LInstruction* check = new(alloc()) LConvertUnboxedObjectToNative(useRegister(ins->object()));
    add(check, ins);
    assignSafepoint(check, ins);
}

void
LIRGenerator::visitEffectiveAddress(MEffectiveAddress* ins)
{
    define(new(alloc()) LEffectiveAddress(useRegister(ins->base()), useRegister(ins->index())), ins);
}

void
LIRGenerator::visitArrayPopShift(MArrayPopShift* ins)
{
    LUse object = useRegister(ins->object());

    switch (ins->type()) {
      case MIRType_Value:
      {
        LArrayPopShiftV* lir = new(alloc()) LArrayPopShiftV(object, temp(), temp());
        defineBox(lir, ins);
        assignSafepoint(lir, ins);
        break;
      }
      case MIRType_Undefined:
      case MIRType_Null:
        MOZ_CRASH("typed load must have a payload");

      default:
      {
        LArrayPopShiftT* lir = new(alloc()) LArrayPopShiftT(object, temp(), temp());
        define(lir, ins);
        assignSafepoint(lir, ins);
        break;
      }
    }
}

void
LIRGenerator::visitArrayPush(MArrayPush* ins)
{
    MOZ_ASSERT(ins->type() == MIRType_Int32);

    LUse object = useRegister(ins->object());

    switch (ins->value()->type()) {
      case MIRType_Value:
      {
        LArrayPushV* lir = new(alloc()) LArrayPushV(object, temp());
        useBox(lir, LArrayPushV::Value, ins->value());
        define(lir, ins);
        assignSafepoint(lir, ins);
        break;
      }

      default:
      {
        const LAllocation value = useRegisterOrNonDoubleConstant(ins->value());
        LArrayPushT* lir = new(alloc()) LArrayPushT(object, value, temp());
        define(lir, ins);
        assignSafepoint(lir, ins);
        break;
      }
    }
}

void
LIRGenerator::visitArrayConcat(MArrayConcat* ins)
{
    MOZ_ASSERT(ins->type() == MIRType_Object);
    MOZ_ASSERT(ins->lhs()->type() == MIRType_Object);
    MOZ_ASSERT(ins->rhs()->type() == MIRType_Object);

    LArrayConcat* lir = new(alloc()) LArrayConcat(useFixed(ins->lhs(), CallTempReg1),
                                                  useFixed(ins->rhs(), CallTempReg2),
                                                  tempFixed(CallTempReg3),
                                                  tempFixed(CallTempReg4));
    defineReturn(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitArraySlice(MArraySlice* ins)
{
    MOZ_ASSERT(ins->type() == MIRType_Object);
    MOZ_ASSERT(ins->object()->type() == MIRType_Object);
    MOZ_ASSERT(ins->begin()->type() == MIRType_Int32);
    MOZ_ASSERT(ins->end()->type() == MIRType_Int32);

    LArraySlice* lir = new(alloc()) LArraySlice(useFixed(ins->object(), CallTempReg0),
                                                useFixed(ins->begin(), CallTempReg1),
                                                useFixed(ins->end(), CallTempReg2),
                                                tempFixed(CallTempReg3),
                                                tempFixed(CallTempReg4));
    defineReturn(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitArrayJoin(MArrayJoin* ins)
{
    MOZ_ASSERT(ins->type() == MIRType_String);
    MOZ_ASSERT(ins->array()->type() == MIRType_Object);
    MOZ_ASSERT(ins->sep()->type() == MIRType_String);

    LArrayJoin* lir = new(alloc()) LArrayJoin(useRegisterAtStart(ins->array()),
                                              useRegisterAtStart(ins->sep()));
    defineReturn(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitSinCos(MSinCos *ins)
{
    MOZ_ASSERT(ins->type() == MIRType_SinCosDouble);
    MOZ_ASSERT(ins->input()->type() == MIRType_Double  ||
               ins->input()->type() == MIRType_Float32 ||
               ins->input()->type() == MIRType_Int32);

    LSinCos *lir = new (alloc()) LSinCos(useRegisterAtStart(ins->input()),
                                         tempFixed(CallTempReg0),
                                         temp());
    defineSinCos(lir, ins);
}

void
LIRGenerator::visitStringSplit(MStringSplit* ins)
{
    MOZ_ASSERT(ins->type() == MIRType_Object);
    MOZ_ASSERT(ins->string()->type() == MIRType_String);
    MOZ_ASSERT(ins->separator()->type() == MIRType_String);

    LStringSplit* lir = new(alloc()) LStringSplit(useRegisterAtStart(ins->string()),
                                                  useRegisterAtStart(ins->separator()));
    defineReturn(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitLoadUnboxedScalar(MLoadUnboxedScalar* ins)
{
    MOZ_ASSERT(IsValidElementsType(ins->elements(), ins->offsetAdjustment()));
    MOZ_ASSERT(ins->index()->type() == MIRType_Int32);

    const LUse elements = useRegister(ins->elements());
    const LAllocation index = useRegisterOrConstant(ins->index());

    MOZ_ASSERT(IsNumberType(ins->type()) || IsSimdType(ins->type()) ||
               ins->type() == MIRType_Boolean);

    // We need a temp register for Uint32Array with known double result.
    LDefinition tempDef = LDefinition::BogusTemp();
    if (ins->readType() == Scalar::Uint32 && IsFloatingPointType(ins->type()))
        tempDef = temp();

    if (ins->requiresMemoryBarrier()) {
        LMemoryBarrier* fence = new(alloc()) LMemoryBarrier(MembarBeforeLoad);
        add(fence, ins);
    }
    LLoadUnboxedScalar* lir = new(alloc()) LLoadUnboxedScalar(elements, index, tempDef);
    if (ins->fallible())
        assignSnapshot(lir, Bailout_Overflow);
    define(lir, ins);
    if (ins->requiresMemoryBarrier()) {
        LMemoryBarrier* fence = new(alloc()) LMemoryBarrier(MembarAfterLoad);
        add(fence, ins);
    }
}

void
LIRGenerator::visitClampToUint8(MClampToUint8* ins)
{
    MDefinition* in = ins->input();

    switch (in->type()) {
      case MIRType_Boolean:
        redefine(ins, in);
        break;

      case MIRType_Int32:
        defineReuseInput(new(alloc()) LClampIToUint8(useRegisterAtStart(in)), ins, 0);
        break;

      case MIRType_Double:
        // LClampDToUint8 clobbers its input register. Making it available as
        // a temp copy describes this behavior to the register allocator.
        define(new(alloc()) LClampDToUint8(useRegisterAtStart(in), tempCopy(in, 0)), ins);
        break;

      case MIRType_Value:
      {
        LClampVToUint8* lir = new(alloc()) LClampVToUint8(tempDouble());
        useBox(lir, LClampVToUint8::Input, in);
        assignSnapshot(lir, Bailout_NonPrimitiveInput);
        define(lir, ins);
        assignSafepoint(lir, ins);
        break;
      }

      default:
        MOZ_CRASH("unexpected type");
    }
}

void
LIRGenerator::visitLoadTypedArrayElementHole(MLoadTypedArrayElementHole* ins)
{
    MOZ_ASSERT(ins->object()->type() == MIRType_Object);
    MOZ_ASSERT(ins->index()->type() == MIRType_Int32);

    MOZ_ASSERT(ins->type() == MIRType_Value);

    const LUse object = useRegister(ins->object());
    const LAllocation index = useRegisterOrConstant(ins->index());

    LLoadTypedArrayElementHole* lir = new(alloc()) LLoadTypedArrayElementHole(object, index);
    if (ins->fallible())
        assignSnapshot(lir, Bailout_Overflow);
    defineBox(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitLoadTypedArrayElementStatic(MLoadTypedArrayElementStatic* ins)
{
    LLoadTypedArrayElementStatic* lir =
        new(alloc()) LLoadTypedArrayElementStatic(useRegisterAtStart(ins->ptr()));

    // In case of out of bounds, may bail out, or may jump to ool code.
    if (ins->fallible())
        assignSnapshot(lir, Bailout_BoundsCheck);
    define(lir, ins);
}

void
LIRGenerator::visitStoreUnboxedScalar(MStoreUnboxedScalar* ins)
{
    MOZ_ASSERT(IsValidElementsType(ins->elements(), ins->offsetAdjustment()));
    MOZ_ASSERT(ins->index()->type() == MIRType_Int32);

    if (ins->isSimdWrite()) {
        MOZ_ASSERT_IF(ins->writeType() == Scalar::Float32x4, ins->value()->type() == MIRType_Float32x4);
        MOZ_ASSERT_IF(ins->writeType() == Scalar::Int32x4, ins->value()->type() == MIRType_Int32x4);
    } else if (ins->isFloatWrite()) {
        MOZ_ASSERT_IF(ins->writeType() == Scalar::Float32, ins->value()->type() == MIRType_Float32);
        MOZ_ASSERT_IF(ins->writeType() == Scalar::Float64, ins->value()->type() == MIRType_Double);
    } else {
        MOZ_ASSERT(ins->value()->type() == MIRType_Int32);
    }

    LUse elements = useRegister(ins->elements());
    LAllocation index = useRegisterOrConstant(ins->index());
    LAllocation value;

    // For byte arrays, the value has to be in a byte register on x86.
    if (ins->isByteWrite())
        value = useByteOpRegisterOrNonDoubleConstant(ins->value());
    else
        value = useRegisterOrNonDoubleConstant(ins->value());

    // Optimization opportunity for atomics: on some platforms there
    // is a store instruction that incorporates the necessary
    // barriers, and we could use that instead of separate barrier and
    // store instructions.  See bug #1077027.
    if (ins->requiresMemoryBarrier()) {
        LMemoryBarrier* fence = new(alloc()) LMemoryBarrier(MembarBeforeStore);
        add(fence, ins);
    }
    add(new(alloc()) LStoreUnboxedScalar(elements, index, value), ins);
    if (ins->requiresMemoryBarrier()) {
        LMemoryBarrier* fence = new(alloc()) LMemoryBarrier(MembarAfterStore);
        add(fence, ins);
    }
}

void
LIRGenerator::visitStoreTypedArrayElementHole(MStoreTypedArrayElementHole* ins)
{
    MOZ_ASSERT(ins->elements()->type() == MIRType_Elements);
    MOZ_ASSERT(ins->index()->type() == MIRType_Int32);
    MOZ_ASSERT(ins->length()->type() == MIRType_Int32);

    if (ins->isFloatWrite()) {
        MOZ_ASSERT_IF(ins->arrayType() == Scalar::Float32, ins->value()->type() == MIRType_Float32);
        MOZ_ASSERT_IF(ins->arrayType() == Scalar::Float64, ins->value()->type() == MIRType_Double);
    } else {
        MOZ_ASSERT(ins->value()->type() == MIRType_Int32);
    }

    LUse elements = useRegister(ins->elements());
    LAllocation length = useAnyOrConstant(ins->length());
    LAllocation index = useRegisterOrConstant(ins->index());
    LAllocation value;

    // For byte arrays, the value has to be in a byte register on x86.
    if (ins->isByteWrite())
        value = useByteOpRegisterOrNonDoubleConstant(ins->value());
    else
        value = useRegisterOrNonDoubleConstant(ins->value());
    add(new(alloc()) LStoreTypedArrayElementHole(elements, length, index, value), ins);
}

void
LIRGenerator::visitLoadFixedSlot(MLoadFixedSlot* ins)
{
    MDefinition* obj = ins->object();
    MOZ_ASSERT(obj->type() == MIRType_Object);

    MIRType type = ins->type();

    if (type == MIRType_Value) {
        LLoadFixedSlotV* lir = new(alloc()) LLoadFixedSlotV(useRegisterAtStart(obj));
        defineBox(lir, ins);
    } else {
        LLoadFixedSlotT* lir = new(alloc()) LLoadFixedSlotT(useRegisterForTypedLoad(obj, type));
        define(lir, ins);
    }
}

void
LIRGenerator::visitLoadFixedSlotAndUnbox(MLoadFixedSlotAndUnbox* ins)
{
    MDefinition* obj = ins->object();
    MOZ_ASSERT(obj->type() == MIRType_Object);

    LLoadFixedSlotAndUnbox* lir = new(alloc()) LLoadFixedSlotAndUnbox(useRegisterAtStart(obj));
    if (ins->fallible())
        assignSnapshot(lir, ins->bailoutKind());

    define(lir, ins);
}

void
LIRGenerator::visitStoreFixedSlot(MStoreFixedSlot* ins)
{
    MOZ_ASSERT(ins->object()->type() == MIRType_Object);

    if (ins->value()->type() == MIRType_Value) {
        LStoreFixedSlotV* lir = new(alloc()) LStoreFixedSlotV(useRegister(ins->object()));
        useBox(lir, LStoreFixedSlotV::Value, ins->value());
        add(lir, ins);
    } else {
        LStoreFixedSlotT* lir = new(alloc()) LStoreFixedSlotT(useRegister(ins->object()),
                                                              useRegisterOrConstant(ins->value()));
        add(lir, ins);
    }
}

void
LIRGenerator::visitGetNameCache(MGetNameCache* ins)
{
    MOZ_ASSERT(ins->scopeObj()->type() == MIRType_Object);

    // Set the performs-call flag so that we don't omit the overrecursed check.
    // This is necessary because the cache can attach a scripted getter stub
    // that calls this script recursively.
    gen->setPerformsCall();

    LGetNameCache* lir = new(alloc()) LGetNameCache(useRegister(ins->scopeObj()));
    defineBox(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitCallGetIntrinsicValue(MCallGetIntrinsicValue* ins)
{
    LCallGetIntrinsicValue* lir = new(alloc()) LCallGetIntrinsicValue();
    defineReturn(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitGetPropertyCache(MGetPropertyCache* ins)
{
    MOZ_ASSERT(ins->object()->type() == MIRType_Object);

    MDefinition* id = ins->idval();
    MOZ_ASSERT(id->type() == MIRType_String ||
               id->type() == MIRType_Symbol ||
               id->type() == MIRType_Int32 ||
               id->type() == MIRType_Value);

    if (ins->monitoredResult()) {
        // Set the performs-call flag so that we don't omit the overrecursed
        // check. This is necessary because the cache can attach a scripted
        // getter stub that calls this script recursively.
        gen->setPerformsCall();
    }

    // If this is a GETPROP, the id is a constant string. Allow passing it as a
    // constant to reduce register allocation pressure.
    bool useConstId = id->type() == MIRType_String || id->type() == MIRType_Symbol;

    if (ins->type() == MIRType_Value) {
        LGetPropertyCacheV* lir = new(alloc()) LGetPropertyCacheV(useRegister(ins->object()));
        useBoxOrTypedOrConstant(lir, LGetPropertyCacheV::Id, id, useConstId);
        defineBox(lir, ins);
        assignSafepoint(lir, ins);
    } else {
        LGetPropertyCacheT* lir = new(alloc()) LGetPropertyCacheT(useRegister(ins->object()));
        useBoxOrTypedOrConstant(lir, LGetPropertyCacheT::Id, id, useConstId);
        define(lir, ins);
        assignSafepoint(lir, ins);
    }
}

void
LIRGenerator::visitGetPropertyPolymorphic(MGetPropertyPolymorphic* ins)
{
    MOZ_ASSERT(ins->obj()->type() == MIRType_Object);

    if (ins->type() == MIRType_Value) {
        LGetPropertyPolymorphicV* lir =
            new(alloc()) LGetPropertyPolymorphicV(useRegister(ins->obj()));
        assignSnapshot(lir, Bailout_ShapeGuard);
        defineBox(lir, ins);
    } else {
        LDefinition maybeTemp = (ins->type() == MIRType_Double) ? temp() : LDefinition::BogusTemp();
        LGetPropertyPolymorphicT* lir =
            new(alloc()) LGetPropertyPolymorphicT(useRegister(ins->obj()), maybeTemp);
        assignSnapshot(lir, Bailout_ShapeGuard);
        define(lir, ins);
    }
}

void
LIRGenerator::visitSetPropertyPolymorphic(MSetPropertyPolymorphic* ins)
{
    MOZ_ASSERT(ins->obj()->type() == MIRType_Object);

    if (ins->value()->type() == MIRType_Value) {
        LSetPropertyPolymorphicV* lir =
            new(alloc()) LSetPropertyPolymorphicV(useRegister(ins->obj()), temp());
        useBox(lir, LSetPropertyPolymorphicV::Value, ins->value());
        assignSnapshot(lir, Bailout_ShapeGuard);
        add(lir, ins);
    } else {
        LAllocation value = useRegisterOrConstant(ins->value());
        LSetPropertyPolymorphicT* lir =
            new(alloc()) LSetPropertyPolymorphicT(useRegister(ins->obj()), value,
                                                  ins->value()->type(), temp());
        assignSnapshot(lir, Bailout_ShapeGuard);
        add(lir, ins);
    }
}

void
LIRGenerator::visitBindNameCache(MBindNameCache* ins)
{
    MOZ_ASSERT(ins->scopeChain()->type() == MIRType_Object);
    MOZ_ASSERT(ins->type() == MIRType_Object);

    LBindNameCache* lir = new(alloc()) LBindNameCache(useRegister(ins->scopeChain()));
    define(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitGuardObjectIdentity(MGuardObjectIdentity* ins)
{
    LGuardObjectIdentity* guard = new(alloc()) LGuardObjectIdentity(useRegister(ins->obj()),
                                                                    useRegister(ins->expected()));
    assignSnapshot(guard, Bailout_ObjectIdentityOrTypeGuard);
    add(guard, ins);
    redefine(ins, ins->obj());
}

void
LIRGenerator::visitGuardClass(MGuardClass* ins)
{
    LDefinition t = temp();
    LGuardClass* guard = new(alloc()) LGuardClass(useRegister(ins->obj()), t);
    assignSnapshot(guard, Bailout_ObjectIdentityOrTypeGuard);
    add(guard, ins);
}

void
LIRGenerator::visitGuardObject(MGuardObject* ins)
{
    // The type policy does all the work, so at this point the input
    // is guaranteed to be an object.
    MOZ_ASSERT(ins->input()->type() == MIRType_Object);
    redefine(ins, ins->input());
}

void
LIRGenerator::visitGuardString(MGuardString* ins)
{
    // The type policy does all the work, so at this point the input
    // is guaranteed to be a string.
    MOZ_ASSERT(ins->input()->type() == MIRType_String);
    redefine(ins, ins->input());
}

void
LIRGenerator::visitGuardSharedTypedArray(MGuardSharedTypedArray* ins)
{
    MOZ_ASSERT(ins->input()->type() == MIRType_Object);
    LGuardSharedTypedArray* guard =
        new(alloc()) LGuardSharedTypedArray(useRegister(ins->obj()), temp());
    assignSnapshot(guard, Bailout_NonSharedTypedArrayInput);
    add(guard, ins);
}

void
LIRGenerator::visitPolyInlineGuard(MPolyInlineGuard* ins)
{
    MOZ_ASSERT(ins->input()->type() == MIRType_Object);
    redefine(ins, ins->input());
}

void
LIRGenerator::visitGuardReceiverPolymorphic(MGuardReceiverPolymorphic* ins)
{
    MOZ_ASSERT(ins->obj()->type() == MIRType_Object);
    MOZ_ASSERT(ins->type() == MIRType_Object);

    LGuardReceiverPolymorphic* guard =
        new(alloc()) LGuardReceiverPolymorphic(useRegister(ins->obj()), temp());
    assignSnapshot(guard, Bailout_ShapeGuard);
    add(guard, ins);
    redefine(ins, ins->obj());
}

void
LIRGenerator::visitGuardUnboxedExpando(MGuardUnboxedExpando* ins)
{
    LGuardUnboxedExpando* guard =
        new(alloc()) LGuardUnboxedExpando(useRegister(ins->obj()));
    assignSnapshot(guard, ins->bailoutKind());
    add(guard, ins);
    redefine(ins, ins->obj());
}

void
LIRGenerator::visitLoadUnboxedExpando(MLoadUnboxedExpando* ins)
{
    LLoadUnboxedExpando* lir =
        new(alloc()) LLoadUnboxedExpando(useRegisterAtStart(ins->object()));
    define(lir, ins);
}

void
LIRGenerator::visitAssertRange(MAssertRange* ins)
{
    MDefinition* input = ins->input();
    LInstruction* lir = nullptr;

    switch (input->type()) {
      case MIRType_Boolean:
      case MIRType_Int32:
        lir = new(alloc()) LAssertRangeI(useRegisterAtStart(input));
        break;

      case MIRType_Double:
        lir = new(alloc()) LAssertRangeD(useRegister(input), tempDouble());
        break;

      case MIRType_Float32: {
        LDefinition armtemp = hasMultiAlias() ? tempDouble() : LDefinition::BogusTemp();
        lir = new(alloc()) LAssertRangeF(useRegister(input), tempDouble(), armtemp);
        break;
      }
      case MIRType_Value:
        lir = new(alloc()) LAssertRangeV(tempToUnbox(), tempDouble(), tempDouble());
        useBox(lir, LAssertRangeV::Input, input);
        break;

      default:
        MOZ_CRASH("Unexpected Range for MIRType");
        break;
    }

    lir->setMir(ins);
    add(lir);
}

void
LIRGenerator::visitCallGetProperty(MCallGetProperty* ins)
{
    LCallGetProperty* lir = new(alloc()) LCallGetProperty();
    useBoxAtStart(lir, LCallGetProperty::Value, ins->value());
    defineReturn(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitCallGetElement(MCallGetElement* ins)
{
    MOZ_ASSERT(ins->lhs()->type() == MIRType_Value);
    MOZ_ASSERT(ins->rhs()->type() == MIRType_Value);

    LCallGetElement* lir = new(alloc()) LCallGetElement();
    useBoxAtStart(lir, LCallGetElement::LhsInput, ins->lhs());
    useBoxAtStart(lir, LCallGetElement::RhsInput, ins->rhs());
    defineReturn(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitCallSetProperty(MCallSetProperty* ins)
{
    LInstruction* lir = new(alloc()) LCallSetProperty(useRegisterAtStart(ins->object()));
    useBoxAtStart(lir, LCallSetProperty::Value, ins->value());
    add(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitDeleteProperty(MDeleteProperty* ins)
{
    LCallDeleteProperty* lir = new(alloc()) LCallDeleteProperty();
    useBoxAtStart(lir, LCallDeleteProperty::Value, ins->value());
    defineReturn(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitDeleteElement(MDeleteElement* ins)
{
    LCallDeleteElement* lir = new(alloc()) LCallDeleteElement();
    useBoxAtStart(lir, LCallDeleteElement::Value, ins->value());
    useBoxAtStart(lir, LCallDeleteElement::Index, ins->index());
    defineReturn(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitSetPropertyCache(MSetPropertyCache* ins)
{
    MOZ_ASSERT(ins->object()->type() == MIRType_Object);

    MDefinition* id = ins->idval();
    MOZ_ASSERT(id->type() == MIRType_String ||
               id->type() == MIRType_Symbol ||
               id->type() == MIRType_Int32 ||
               id->type() == MIRType_Value);

    // If this is a SETPROP, the id is a constant string. Allow passing it as a
    // constant to reduce register allocation pressure.
    bool useConstId = id->type() == MIRType_String || id->type() == MIRType_Symbol;
    bool useConstValue = IsNonNurseryConstant(ins->value());

    // Set the performs-call flag so that we don't omit the overrecursed check.
    // This is necessary because the cache can attach a scripted setter stub
    // that calls this script recursively.
    gen->setPerformsCall();

    // If the index might be an integer, we need some extra temp registers for
    // the dense and typed array element stubs.
    LDefinition tempToUnboxIndex = LDefinition::BogusTemp();
    LDefinition tempD = LDefinition::BogusTemp();
    LDefinition tempF32 = LDefinition::BogusTemp();

    if (id->mightBeType(MIRType_Int32)) {
        if (id->type() != MIRType_Int32)
            tempToUnboxIndex = tempToUnbox();
        tempD = tempDouble();
        tempF32 = hasUnaliasedDouble() ? tempFloat32() : LDefinition::BogusTemp();
    }

    LInstruction* lir = new(alloc()) LSetPropertyCache(useRegister(ins->object()), temp(),
                                                       tempToUnboxIndex, tempD, tempF32);
    useBoxOrTypedOrConstant(lir, LSetPropertyCache::Id, id, useConstId);
    useBoxOrTypedOrConstant(lir, LSetPropertyCache::Value, ins->value(), useConstValue);

    add(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitCallSetElement(MCallSetElement* ins)
{
    MOZ_ASSERT(ins->object()->type() == MIRType_Object);
    MOZ_ASSERT(ins->index()->type() == MIRType_Value);
    MOZ_ASSERT(ins->value()->type() == MIRType_Value);

    LCallSetElement* lir = new(alloc()) LCallSetElement();
    lir->setOperand(0, useRegisterAtStart(ins->object()));
    useBoxAtStart(lir, LCallSetElement::Index, ins->index());
    useBoxAtStart(lir, LCallSetElement::Value, ins->value());
    add(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitCallInitElementArray(MCallInitElementArray* ins)
{
    LCallInitElementArray* lir = new(alloc()) LCallInitElementArray();
    lir->setOperand(0, useRegisterAtStart(ins->object()));
    useBoxAtStart(lir, LCallInitElementArray::Value, ins->value());
    add(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitIteratorStart(MIteratorStart* ins)
{
    // Call a stub if this is not a simple for-in loop.
    if (ins->flags() != JSITER_ENUMERATE) {
        LCallIteratorStart* lir = new(alloc()) LCallIteratorStart(useRegisterAtStart(ins->object()));
        defineReturn(lir, ins);
        assignSafepoint(lir, ins);
    } else {
        LIteratorStart* lir = new(alloc()) LIteratorStart(useRegister(ins->object()), temp(), temp(), temp());
        define(lir, ins);
        assignSafepoint(lir, ins);
    }
}

void
LIRGenerator::visitIteratorMore(MIteratorMore* ins)
{
    LIteratorMore* lir = new(alloc()) LIteratorMore(useRegister(ins->iterator()), temp());
    defineBox(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitIsNoIter(MIsNoIter* ins)
{
    MOZ_ASSERT(ins->hasOneUse());
    emitAtUses(ins);
}

void
LIRGenerator::visitIteratorEnd(MIteratorEnd* ins)
{
    LIteratorEnd* lir = new(alloc()) LIteratorEnd(useRegister(ins->iterator()), temp(), temp(), temp());
    add(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitStringLength(MStringLength* ins)
{
    MOZ_ASSERT(ins->string()->type() == MIRType_String);
    define(new(alloc()) LStringLength(useRegisterAtStart(ins->string())), ins);
}

void
LIRGenerator::visitArgumentsLength(MArgumentsLength* ins)
{
    define(new(alloc()) LArgumentsLength(), ins);
}

void
LIRGenerator::visitGetFrameArgument(MGetFrameArgument* ins)
{
    LGetFrameArgument* lir = new(alloc()) LGetFrameArgument(useRegisterOrConstant(ins->index()));
    defineBox(lir, ins);
}

void
LIRGenerator::visitNewTarget(MNewTarget* ins)
{
    LNewTarget* lir = new(alloc()) LNewTarget();
    defineBox(lir, ins);
}

void
LIRGenerator::visitSetFrameArgument(MSetFrameArgument* ins)
{
    MDefinition* input = ins->input();

    if (input->type() == MIRType_Value) {
        LSetFrameArgumentV* lir = new(alloc()) LSetFrameArgumentV();
        useBox(lir, LSetFrameArgumentV::Input, input);
        add(lir, ins);
    } else if (input->type() == MIRType_Undefined || input->type() == MIRType_Null) {
        Value val = input->type() == MIRType_Undefined ? UndefinedValue() : NullValue();
        LSetFrameArgumentC* lir = new(alloc()) LSetFrameArgumentC(val);
        add(lir, ins);
    } else {
        LSetFrameArgumentT* lir = new(alloc()) LSetFrameArgumentT(useRegister(input));
        add(lir, ins);
    }
}

void
LIRGenerator::visitRunOncePrologue(MRunOncePrologue* ins)
{
    LRunOncePrologue* lir = new(alloc()) LRunOncePrologue;
    add(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitRest(MRest* ins)
{
    MOZ_ASSERT(ins->numActuals()->type() == MIRType_Int32);

    LRest* lir = new(alloc()) LRest(useFixed(ins->numActuals(), CallTempReg0),
                                    tempFixed(CallTempReg1),
                                    tempFixed(CallTempReg2),
                                    tempFixed(CallTempReg3));
    defineReturn(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitThrow(MThrow* ins)
{
    MDefinition* value = ins->getOperand(0);
    MOZ_ASSERT(value->type() == MIRType_Value);

    LThrow* lir = new(alloc()) LThrow;
    useBoxAtStart(lir, LThrow::Value, value);
    add(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitIn(MIn* ins)
{
    MDefinition* lhs = ins->lhs();
    MDefinition* rhs = ins->rhs();

    MOZ_ASSERT(lhs->type() == MIRType_Value);
    MOZ_ASSERT(rhs->type() == MIRType_Object);

    LIn* lir = new(alloc()) LIn(useRegisterAtStart(rhs));
    useBoxAtStart(lir, LIn::LHS, lhs);
    defineReturn(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitInstanceOf(MInstanceOf* ins)
{
    MDefinition* lhs = ins->getOperand(0);

    MOZ_ASSERT(lhs->type() == MIRType_Value || lhs->type() == MIRType_Object);

    if (lhs->type() == MIRType_Object) {
        LInstanceOfO* lir = new(alloc()) LInstanceOfO(useRegister(lhs));
        define(lir, ins);
        assignSafepoint(lir, ins);
    } else {
        LInstanceOfV* lir = new(alloc()) LInstanceOfV();
        useBox(lir, LInstanceOfV::LHS, lhs);
        define(lir, ins);
        assignSafepoint(lir, ins);
    }
}

void
LIRGenerator::visitCallInstanceOf(MCallInstanceOf* ins)
{
    MDefinition* lhs = ins->lhs();
    MDefinition* rhs = ins->rhs();

    MOZ_ASSERT(lhs->type() == MIRType_Value);
    MOZ_ASSERT(rhs->type() == MIRType_Object);

    LCallInstanceOf* lir = new(alloc()) LCallInstanceOf(useRegisterAtStart(rhs));
    useBoxAtStart(lir, LCallInstanceOf::LHS, lhs);
    defineReturn(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitIsCallable(MIsCallable* ins)
{
    MOZ_ASSERT(ins->object()->type() == MIRType_Object);
    MOZ_ASSERT(ins->type() == MIRType_Boolean);
    define(new(alloc()) LIsCallable(useRegister(ins->object())), ins);
}

static bool
CanEmitIsObjectAtUses(MInstruction* ins)
{
    if (!ins->canEmitAtUses())
        return false;

    MUseIterator iter(ins->usesBegin());
    if (iter == ins->usesEnd())
        return false;

    MNode* node = iter->consumer();
    if (!node->isDefinition())
        return false;

    if (!node->toDefinition()->isTest())
        return false;

    iter++;
    return iter == ins->usesEnd();
}

void
LIRGenerator::visitIsObject(MIsObject* ins)
{
    if (CanEmitIsObjectAtUses(ins)) {
        emitAtUses(ins);
        return;
    }

    MDefinition* opd = ins->input();
    MOZ_ASSERT(opd->type() == MIRType_Value);
    LIsObject* lir = new(alloc()) LIsObject();
    useBoxAtStart(lir, LIsObject::Input, opd);
    define(lir, ins);
}

void
LIRGenerator::visitHasClass(MHasClass* ins)
{
    MOZ_ASSERT(ins->object()->type() == MIRType_Object);
    MOZ_ASSERT(ins->type() == MIRType_Boolean);
    define(new(alloc()) LHasClass(useRegister(ins->object())), ins);
}

void
LIRGenerator::visitAsmJSLoadGlobalVar(MAsmJSLoadGlobalVar* ins)
{
    define(new(alloc()) LAsmJSLoadGlobalVar, ins);
}

void
LIRGenerator::visitAsmJSStoreGlobalVar(MAsmJSStoreGlobalVar* ins)
{
    add(new(alloc()) LAsmJSStoreGlobalVar(useRegisterAtStart(ins->value())), ins);
}

void
LIRGenerator::visitAsmJSLoadFFIFunc(MAsmJSLoadFFIFunc* ins)
{
    define(new(alloc()) LAsmJSLoadFFIFunc, ins);
}

void
LIRGenerator::visitAsmJSParameter(MAsmJSParameter* ins)
{
    ABIArg abi = ins->abi();
    if (abi.argInRegister()) {
        defineFixed(new(alloc()) LAsmJSParameter, ins, LAllocation(abi.reg()));
    } else {
        MOZ_ASSERT(IsNumberType(ins->type()) || IsSimdType(ins->type()));
        defineFixed(new(alloc()) LAsmJSParameter, ins, LArgument(abi.offsetFromArgBase()));
    }
}

void
LIRGenerator::visitAsmJSReturn(MAsmJSReturn* ins)
{
    MDefinition* rval = ins->getOperand(0);
    LAsmJSReturn* lir = new(alloc()) LAsmJSReturn;
    if (rval->type() == MIRType_Float32)
        lir->setOperand(0, useFixed(rval, ReturnFloat32Reg));
    else if (rval->type() == MIRType_Double)
        lir->setOperand(0, useFixed(rval, ReturnDoubleReg));
    else if (IsSimdType(rval->type()))
        lir->setOperand(0, useFixed(rval, ReturnSimd128Reg));
    else if (rval->type() == MIRType_Int32)
        lir->setOperand(0, useFixed(rval, ReturnReg));
    else
        MOZ_CRASH("Unexpected asm.js return type");
    add(lir);
}

void
LIRGenerator::visitAsmJSVoidReturn(MAsmJSVoidReturn* ins)
{
    add(new(alloc()) LAsmJSVoidReturn);
}

void
LIRGenerator::visitAsmJSPassStackArg(MAsmJSPassStackArg* ins)
{
    if (IsFloatingPointType(ins->arg()->type()) || IsSimdType(ins->arg()->type())) {
        MOZ_ASSERT(!ins->arg()->isEmittedAtUses());
        add(new(alloc()) LAsmJSPassStackArg(useRegisterAtStart(ins->arg())), ins);
    } else {
        add(new(alloc()) LAsmJSPassStackArg(useRegisterOrConstantAtStart(ins->arg())), ins);
    }
}

void
LIRGenerator::visitAsmJSCall(MAsmJSCall* ins)
{
    gen->setPerformsCall();

    LAllocation* args = gen->allocate<LAllocation>(ins->numOperands());
    if (!args) {
        gen->abort("Couldn't allocate for MAsmJSCall");
        return;
    }

    for (unsigned i = 0; i < ins->numArgs(); i++)
        args[i] = useFixed(ins->getOperand(i), ins->registerForArg(i));

    if (ins->callee().which() == MAsmJSCall::Callee::Dynamic)
        args[ins->dynamicCalleeOperandIndex()] = useFixed(ins->callee().dynamic(), CallTempReg0);

    LInstruction* lir = new(alloc()) LAsmJSCall(args, ins->numOperands());
    if (ins->type() == MIRType_None)
        add(lir, ins);
    else
        defineReturn(lir, ins);
}

void
LIRGenerator::visitSetDOMProperty(MSetDOMProperty* ins)
{
    MDefinition* val = ins->value();

    Register cxReg, objReg, privReg, valueReg;
    GetTempRegForIntArg(0, 0, &cxReg);
    GetTempRegForIntArg(1, 0, &objReg);
    GetTempRegForIntArg(2, 0, &privReg);
    GetTempRegForIntArg(3, 0, &valueReg);
    LSetDOMProperty* lir = new(alloc()) LSetDOMProperty(tempFixed(cxReg),
                                                        useFixed(ins->object(), objReg),
                                                        tempFixed(privReg),
                                                        tempFixed(valueReg));

    // Keep using GetTempRegForIntArg, since we want to make sure we
    // don't clobber registers we're already using.
    Register tempReg1, tempReg2;
    GetTempRegForIntArg(4, 0, &tempReg1);
    mozilla::DebugOnly<bool> ok = GetTempRegForIntArg(5, 0, &tempReg2);
    MOZ_ASSERT(ok, "How can we not have six temp registers?");
    useBoxFixed(lir, LSetDOMProperty::Value, val, tempReg1, tempReg2);
    add(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitGetDOMProperty(MGetDOMProperty* ins)
{
    Register cxReg, objReg, privReg, valueReg;
    GetTempRegForIntArg(0, 0, &cxReg);
    GetTempRegForIntArg(1, 0, &objReg);
    GetTempRegForIntArg(2, 0, &privReg);
    mozilla::DebugOnly<bool> ok = GetTempRegForIntArg(3, 0, &valueReg);
    MOZ_ASSERT(ok, "How can we not have four temp registers?");
    LGetDOMProperty* lir = new(alloc()) LGetDOMProperty(tempFixed(cxReg),
                                                        useFixed(ins->object(), objReg),
                                                        tempFixed(privReg),
                                                        tempFixed(valueReg));

    defineReturn(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitGetDOMMember(MGetDOMMember* ins)
{
    MOZ_ASSERT(ins->isDomMovable(), "Members had better be movable");
    // We wish we could assert that ins->domAliasSet() == JSJitInfo::AliasNone,
    // but some MGetDOMMembers are for [Pure], not [Constant] properties, whose
    // value can in fact change as a result of DOM setters and method calls.
    MOZ_ASSERT(ins->domAliasSet() != JSJitInfo::AliasEverything,
               "Member gets had better not alias the world");

    MDefinition* obj = ins->object();
    MOZ_ASSERT(obj->type() == MIRType_Object);

    MIRType type = ins->type();

    if (type == MIRType_Value) {
        LGetDOMMemberV* lir = new(alloc()) LGetDOMMemberV(useRegisterAtStart(obj));
        defineBox(lir, ins);
    } else {
        LGetDOMMemberT* lir = new(alloc()) LGetDOMMemberT(useRegisterForTypedLoad(obj, type));
        define(lir, ins);
    }
}

void
LIRGenerator::visitRecompileCheck(MRecompileCheck* ins)
{
    LRecompileCheck* lir = new(alloc()) LRecompileCheck(temp());
    add(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitMemoryBarrier(MMemoryBarrier* ins)
{
    LMemoryBarrier* lir = new(alloc()) LMemoryBarrier(ins->type());
    add(lir, ins);
}

void
LIRGenerator::visitSimdBox(MSimdBox* ins)
{
    MOZ_ASSERT(IsSimdType(ins->input()->type()));
    LUse in = useRegister(ins->input());
    LSimdBox* lir = new(alloc()) LSimdBox(in, temp());
    define(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitSimdUnbox(MSimdUnbox* ins)
{
    MOZ_ASSERT(ins->input()->type() == MIRType_Object);
    MOZ_ASSERT(IsSimdType(ins->type()));
    LUse in = useRegister(ins->input());

    BailoutKind kind;
    switch (ins->type()) {
      case MIRType_Int32x4:
        kind = Bailout_NonSimdInt32x4Input;
        break;
      case MIRType_Float32x4:
        kind = Bailout_NonSimdFloat32x4Input;
        break;
      default:
        MOZ_CRASH("Unexpected SIMD Type.");
    };

    LSimdUnbox* lir = new(alloc()) LSimdUnbox(in, temp());
    assignSnapshot(lir, kind);
    define(lir, ins);
}

void
LIRGenerator::visitSimdConstant(MSimdConstant* ins)
{
    MOZ_ASSERT(IsSimdType(ins->type()));

    if (ins->type() == MIRType_Int32x4)
        define(new(alloc()) LInt32x4(), ins);
    else if (ins->type() == MIRType_Float32x4)
        define(new(alloc()) LFloat32x4(), ins);
    else
        MOZ_CRASH("Unknown SIMD kind when generating constant");
}

void
LIRGenerator::visitSimdConvert(MSimdConvert* ins)
{
    MOZ_ASSERT(IsSimdType(ins->type()));
    MDefinition* input = ins->input();
    LUse use = useRegister(input);
    if (ins->type() == MIRType_Int32x4) {
        MOZ_ASSERT(input->type() == MIRType_Float32x4);
        LFloat32x4ToInt32x4* lir = new(alloc()) LFloat32x4ToInt32x4(use, temp());
        if (!gen->compilingAsmJS())
            assignSnapshot(lir, Bailout_BoundsCheck);
        define(lir, ins);
    } else if (ins->type() == MIRType_Float32x4) {
        MOZ_ASSERT(input->type() == MIRType_Int32x4);
        define(new(alloc()) LInt32x4ToFloat32x4(use), ins);
    } else {
        MOZ_CRASH("Unknown SIMD kind when generating constant");
    }
}

void
LIRGenerator::visitSimdReinterpretCast(MSimdReinterpretCast* ins)
{
    MOZ_ASSERT(IsSimdType(ins->type()) && IsSimdType(ins->input()->type()));
    MDefinition* input = ins->input();
    LUse use = useRegisterAtStart(input);
    // :TODO: (Bug 1132894) We have to allocate a different register as redefine
    // and/or defineReuseInput are not yet capable of reusing the same register
    // with a different register type.
    define(new(alloc()) LSimdReinterpretCast(use), ins);
}

void
LIRGenerator::visitSimdExtractElement(MSimdExtractElement* ins)
{
    MOZ_ASSERT(IsSimdType(ins->input()->type()));
    MOZ_ASSERT(!IsSimdType(ins->type()));

    if (ins->input()->type() == MIRType_Int32x4) {
        // Note: there could be int16x8 in the future, which doesn't use the
        // same instruction. We either need to pass the arity or create new LIns.
        LUse use = useRegisterAtStart(ins->input());
        define(new(alloc()) LSimdExtractElementI(use), ins);
    } else if (ins->input()->type() == MIRType_Float32x4) {
        LUse use = useRegisterAtStart(ins->input());
        define(new(alloc()) LSimdExtractElementF(use), ins);
    } else {
        MOZ_CRASH("Unknown SIMD kind when extracting element");
    }
}

void
LIRGenerator::visitSimdInsertElement(MSimdInsertElement* ins)
{
    MOZ_ASSERT(IsSimdType(ins->type()));

    LUse vec = useRegisterAtStart(ins->vector());
    LUse val = useRegister(ins->value());
    if (ins->type() == MIRType_Int32x4)
        defineReuseInput(new(alloc()) LSimdInsertElementI(vec, val), ins, 0);
    else if (ins->type() == MIRType_Float32x4)
        defineReuseInput(new(alloc()) LSimdInsertElementF(vec, val), ins, 0);
    else
        MOZ_CRASH("Unknown SIMD kind when generating constant");
}

void
LIRGenerator::visitSimdSignMask(MSimdSignMask* ins)
{
    MDefinition* input = ins->input();
    MOZ_ASSERT(IsSimdType(input->type()));
    MOZ_ASSERT(ins->type() == MIRType_Int32);

    LUse use = useRegisterAtStart(input);

    switch (input->type()) {
      case MIRType_Int32x4:
      case MIRType_Float32x4:
        define(new(alloc()) LSimdSignMaskX4(use), ins);
        break;
      default:
        MOZ_CRASH("Unexpected SIMD type extracting sign bits.");
    }
}

void
LIRGenerator::visitSimdSwizzle(MSimdSwizzle* ins)
{
    MOZ_ASSERT(IsSimdType(ins->input()->type()));
    MOZ_ASSERT(IsSimdType(ins->type()));

    if (ins->input()->type() == MIRType_Int32x4) {
        LUse use = useRegisterAtStart(ins->input());
        LSimdSwizzleI* lir = new (alloc()) LSimdSwizzleI(use);
        define(lir, ins);
    } else if (ins->input()->type() == MIRType_Float32x4) {
        LUse use = useRegisterAtStart(ins->input());
        LSimdSwizzleF* lir = new (alloc()) LSimdSwizzleF(use);
        define(lir, ins);
    } else {
        MOZ_CRASH("Unknown SIMD kind when getting lane");
    }
}

void
LIRGenerator::visitSimdGeneralShuffle(MSimdGeneralShuffle*ins)
{
    MOZ_ASSERT(IsSimdType(ins->type()));

    LSimdGeneralShuffleBase* lir;
    if (ins->type() == MIRType_Int32x4)
        lir = new (alloc()) LSimdGeneralShuffleI(temp());
    else if (ins->type() == MIRType_Float32x4)
        lir = new (alloc()) LSimdGeneralShuffleF(temp());
    else
        MOZ_CRASH("Unknown SIMD kind when doing a shuffle");

    if (!lir->init(alloc(), ins->numVectors() + ins->numLanes()))
        return;

    for (unsigned i = 0; i < ins->numVectors(); i++) {
        MOZ_ASSERT(IsSimdType(ins->vector(i)->type()));
        lir->setOperand(i, useRegister(ins->vector(i)));
    }

    for (unsigned i = 0; i < ins->numLanes(); i++) {
        MOZ_ASSERT(ins->lane(i)->type() == MIRType_Int32);
        lir->setOperand(i + ins->numVectors(), useRegister(ins->lane(i)));
    }

    assignSnapshot(lir, Bailout_BoundsCheck);
    define(lir, ins);
}

void
LIRGenerator::visitSimdShuffle(MSimdShuffle* ins)
{
    MOZ_ASSERT(IsSimdType(ins->lhs()->type()));
    MOZ_ASSERT(IsSimdType(ins->rhs()->type()));
    MOZ_ASSERT(IsSimdType(ins->type()));
    MOZ_ASSERT(ins->type() == MIRType_Int32x4 || ins->type() == MIRType_Float32x4);

    bool zFromLHS = ins->laneZ() < 4;
    bool wFromLHS = ins->laneW() < 4;
    uint32_t lanesFromLHS = (ins->laneX() < 4) + (ins->laneY() < 4) + zFromLHS + wFromLHS;

    LSimdShuffle* lir = new (alloc()) LSimdShuffle();
    lowerForFPU(lir, ins, ins->lhs(), ins->rhs());

    // See codegen for requirements details.
    LDefinition temp = (lanesFromLHS == 3) ? tempCopy(ins->rhs(), 1) : LDefinition::BogusTemp();
    lir->setTemp(0, temp);
}

void
LIRGenerator::visitSimdUnaryArith(MSimdUnaryArith* ins)
{
    MOZ_ASSERT(IsSimdType(ins->input()->type()));
    MOZ_ASSERT(IsSimdType(ins->type()));

    // Cannot be at start, as the ouput is used as a temporary to store values.
    LUse in = use(ins->input());

    if (ins->type() == MIRType_Int32x4) {
        LSimdUnaryArithIx4* lir = new(alloc()) LSimdUnaryArithIx4(in);
        define(lir, ins);
    } else if (ins->type() == MIRType_Float32x4) {
        LSimdUnaryArithFx4* lir = new(alloc()) LSimdUnaryArithFx4(in);
        define(lir, ins);
    } else {
        MOZ_CRASH("Unknown SIMD kind for unary operation");
    }
}

void
LIRGenerator::visitSimdBinaryComp(MSimdBinaryComp* ins)
{
    MOZ_ASSERT(IsSimdType(ins->lhs()->type()));
    MOZ_ASSERT(IsSimdType(ins->rhs()->type()));
    MOZ_ASSERT(ins->type() == MIRType_Int32x4);

    if (ShouldReorderCommutative(ins->lhs(), ins->rhs(), ins))
        ins->reverse();

    if (ins->specialization() == MIRType_Int32x4) {
        LSimdBinaryCompIx4* add = new(alloc()) LSimdBinaryCompIx4();
        lowerForCompIx4(add, ins, ins->lhs(), ins->rhs());
    } else if (ins->specialization() == MIRType_Float32x4) {
        LSimdBinaryCompFx4* add = new(alloc()) LSimdBinaryCompFx4();
        lowerForCompFx4(add, ins, ins->lhs(), ins->rhs());
    } else {
        MOZ_CRASH("Unknown compare type when comparing values");
    }
}

void
LIRGenerator::visitSimdBinaryBitwise(MSimdBinaryBitwise* ins)
{
    MOZ_ASSERT(IsSimdType(ins->lhs()->type()));
    MOZ_ASSERT(IsSimdType(ins->rhs()->type()));
    MOZ_ASSERT(IsSimdType(ins->type()));

    MDefinition* lhs = ins->lhs();
    MDefinition* rhs = ins->rhs();
    ReorderCommutative(&lhs, &rhs, ins);

    if (ins->type() == MIRType_Int32x4 || ins->type() == MIRType_Float32x4) {
        LSimdBinaryBitwiseX4* lir = new(alloc()) LSimdBinaryBitwiseX4;
        lowerForFPU(lir, ins, lhs, rhs);
    } else {
        MOZ_CRASH("Unknown SIMD kind when doing bitwise operations");
    }
}

void
LIRGenerator::visitSimdShift(MSimdShift* ins)
{
    MOZ_ASSERT(ins->type() == MIRType_Int32x4);
    MOZ_ASSERT(ins->lhs()->type() == MIRType_Int32x4);
    MOZ_ASSERT(ins->rhs()->type() == MIRType_Int32);

    LUse vector = useRegisterAtStart(ins->lhs());
    LAllocation value = useRegisterOrConstant(ins->rhs());
    LSimdShift* lir = new(alloc()) LSimdShift(vector, value);
    defineReuseInput(lir, ins, 0);
}

void
LIRGenerator::visitLexicalCheck(MLexicalCheck* ins)
{
    MDefinition* input = ins->input();
    MOZ_ASSERT(input->type() == MIRType_Value);
    LLexicalCheck* lir = new(alloc()) LLexicalCheck();
    useBox(lir, LLexicalCheck::Input, input);
    assignSnapshot(lir, ins->bailoutKind());
    add(lir, ins);
    redefine(ins, input);
}

void
LIRGenerator::visitThrowRuntimeLexicalError(MThrowRuntimeLexicalError* ins)
{
    LThrowRuntimeLexicalError* lir = new(alloc()) LThrowRuntimeLexicalError();
    add(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitGlobalNameConflictsCheck(MGlobalNameConflictsCheck* ins)
{
    LGlobalNameConflictsCheck* lir = new(alloc()) LGlobalNameConflictsCheck();
    add(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitDebugger(MDebugger* ins)
{
    LDebugger* lir = new(alloc()) LDebugger(tempFixed(CallTempReg0), tempFixed(CallTempReg1));
    assignSnapshot(lir, Bailout_Debugger);
    add(lir, ins);
}

void
LIRGenerator::visitAtomicIsLockFree(MAtomicIsLockFree* ins)
{
    define(new(alloc()) LAtomicIsLockFree(useRegister(ins->input())), ins);
}

void
LIRGenerator::visitCheckReturn(MCheckReturn* ins)
{
    MDefinition* retVal = ins->returnValue();
    MDefinition* thisVal = ins->thisValue();
    MOZ_ASSERT(retVal->type() == MIRType_Value);
    MOZ_ASSERT(thisVal->type() == MIRType_Value);

    LCheckReturn* lir = new(alloc()) LCheckReturn();
    useBoxAtStart(lir, LCheckReturn::ReturnValue, retVal);
    useBoxAtStart(lir, LCheckReturn::ThisValue, thisVal);
    assignSnapshot(lir, Bailout_BadDerivedConstructorReturn);
    add(lir, ins);
    redefine(ins, retVal);
}

void
LIRGenerator::visitCheckObjCoercible(MCheckObjCoercible* ins)
{
    MDefinition* checkVal = ins->checkValue();
    MOZ_ASSERT(checkVal->type() == MIRType_Value);

    LCheckObjCoercible* lir = new(alloc()) LCheckObjCoercible();
    useBoxAtStart(lir, LCheckObjCoercible::CheckValue, checkVal);
    redefine(ins, checkVal);
    add(lir, ins);
    assignSafepoint(lir, ins);
}

static void
SpewResumePoint(MBasicBlock* block, MInstruction* ins, MResumePoint* resumePoint)
{
    Fprinter& out = JitSpewPrinter();
    out.printf("Current resume point %p details:\n", (void*)resumePoint);
    out.printf("    frame count: %u\n", resumePoint->frameCount());

    if (ins) {
        out.printf("    taken after: ");
        ins->printName(out);
    } else {
        out.printf("    taken at block %d entry", block->id());
    }
    out.printf("\n");

    out.printf("    pc: %p (script: %p, offset: %d)\n",
            (void*)resumePoint->pc(),
            (void*)resumePoint->block()->info().script(),
            int(resumePoint->block()->info().script()->pcToOffset(resumePoint->pc())));

    for (size_t i = 0, e = resumePoint->numOperands(); i < e; i++) {
        MDefinition* in = resumePoint->getOperand(i);
        out.printf("    slot%u: ", (unsigned)i);
        in->printName(out);
        out.printf("\n");
    }
}

bool
LIRGenerator::visitInstruction(MInstruction* ins)
{
    if (ins->isRecoveredOnBailout())
        return true;

    if (!gen->ensureBallast())
        return false;
    ins->accept(this);

    if (ins->possiblyCalls())
        gen->setPerformsCall();

    if (ins->resumePoint())
        updateResumeState(ins);

#ifdef DEBUG
    ins->setInWorklistUnchecked();
#endif

    // If no safepoint was created, there's no need for an OSI point.
    if (LOsiPoint* osiPoint = popOsiPoint())
        add(osiPoint);

    return !gen->errored();
}

void
LIRGenerator::definePhis()
{
    size_t lirIndex = 0;
    MBasicBlock* block = current->mir();
    for (MPhiIterator phi(block->phisBegin()); phi != block->phisEnd(); phi++) {
        if (phi->type() == MIRType_Value) {
            defineUntypedPhi(*phi, lirIndex);
            lirIndex += BOX_PIECES;
        } else {
            defineTypedPhi(*phi, lirIndex);
            lirIndex += 1;
        }
    }
}

void
LIRGenerator::updateResumeState(MInstruction* ins)
{
    lastResumePoint_ = ins->resumePoint();
    if (JitSpewEnabled(JitSpew_IonSnapshots) && lastResumePoint_)
        SpewResumePoint(nullptr, ins, lastResumePoint_);
}

void
LIRGenerator::updateResumeState(MBasicBlock* block)
{
    lastResumePoint_ = block->entryResumePoint();
    if (JitSpewEnabled(JitSpew_IonSnapshots) && lastResumePoint_)
        SpewResumePoint(block, nullptr, lastResumePoint_);
}

bool
LIRGenerator::visitBlock(MBasicBlock* block)
{
    current = block->lir();
    updateResumeState(block);

    definePhis();

    for (MInstructionIterator iter = block->begin(); *iter != block->lastIns(); iter++) {
        if (!visitInstruction(*iter))
            return false;
    }

    if (block->successorWithPhis()) {
        // If we have a successor with phis, lower the phi input now that we
        // are approaching the join point.
        MBasicBlock* successor = block->successorWithPhis();
        uint32_t position = block->positionInPhiSuccessor();
        size_t lirIndex = 0;
        for (MPhiIterator phi(successor->phisBegin()); phi != successor->phisEnd(); phi++) {
            MDefinition* opd = phi->getOperand(position);
            ensureDefined(opd);

            MOZ_ASSERT(opd->type() == phi->type());

            if (phi->type() == MIRType_Value) {
                lowerUntypedPhiInput(*phi, position, successor->lir(), lirIndex);
                lirIndex += BOX_PIECES;
            } else {
                lowerTypedPhiInput(*phi, position, successor->lir(), lirIndex);
                lirIndex += 1;
            }
        }
    }

    // Now emit the last instruction, which is some form of branch.
    if (!visitInstruction(block->lastIns()))
        return false;

    // If we have a resume point check that all the following blocks have one,
    // otherwise reuse the last resume point as the entry resume point of the
    // basic block.  This is used to handle fallible code which is moved/added
    // into split edge blocks, which do not have resume points.  See
    // SplitCriticalEdgesForBlock.
    //
    // When folding conditions, we might create split-edge blocks which have
    // multiple predecessors, in such case it is invalid to have any instruction
    // in these blocks, as these blocks have no associated pc, thus we cannot
    // safely bailout from such block.
    if (lastResumePoint_) {
        for (size_t s = 0; s < block->numSuccessors(); s++) {
            MBasicBlock* succ = block->getSuccessor(s);
            if (!succ->entryResumePoint() && succ->numPredecessors() == 1) {
                MOZ_ASSERT(succ->isSplitEdge());
                MOZ_ASSERT(succ->phisBegin() == succ->phisEnd());
                succ->setEntryResumePoint(lastResumePoint_);
            }
        }
    }

    return true;
}

bool
LIRGenerator::generate()
{
    // Create all blocks and prep all phis beforehand.
    for (ReversePostorderIterator block(graph.rpoBegin()); block != graph.rpoEnd(); block++) {
        if (gen->shouldCancel("Lowering (preparation loop)"))
            return false;

        if (!lirGraph_.initBlock(*block))
            return false;
    }

    for (ReversePostorderIterator block(graph.rpoBegin()); block != graph.rpoEnd(); block++) {
        if (gen->shouldCancel("Lowering (main loop)"))
            return false;

        if (!visitBlock(*block))
            return false;
    }

    lirGraph_.setArgumentSlotCount(maxargslots_);
    return true;
}

void
LIRGenerator::visitPhi(MPhi* phi)
{
    // Phi nodes are not lowered because they are only meaningful for the register allocator.
    MOZ_CRASH("Unexpected Phi node during Lowering.");
}

void
LIRGenerator::visitBeta(MBeta* beta)
{
    // Beta nodes are supposed to be removed before because they are
    // only used to carry the range information for Range analysis
    MOZ_CRASH("Unexpected Beta node during Lowering.");
}

void
LIRGenerator::visitObjectState(MObjectState* objState)
{
    // ObjectState nodes are always recovered on bailouts
    MOZ_CRASH("Unexpected ObjectState node during Lowering.");
}

void
LIRGenerator::visitArrayState(MArrayState* objState)
{
    // ArrayState nodes are always recovered on bailouts
    MOZ_CRASH("Unexpected ArrayState node during Lowering.");
}

void
LIRGenerator::visitUnknownValue(MUnknownValue* ins)
{
    MOZ_CRASH("Can not lower unknown value.");
}

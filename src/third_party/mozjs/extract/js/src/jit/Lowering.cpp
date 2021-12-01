/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/Lowering.h"

#include "mozilla/DebugOnly.h"
#include "mozilla/EndianUtils.h"

#include "jit/JitSpewer.h"
#include "jit/LIR.h"
#include "jit/MIR.h"
#include "jit/MIRGraph.h"
#include "wasm/WasmSignalHandlers.h"

#include "jit/shared/Lowering-shared-inl.h"
#include "vm/BytecodeUtil-inl.h"
#include "vm/JSObject-inl.h"

using namespace js;
using namespace jit;

using mozilla::DebugOnly;
using JS::GenericNaN;

LBoxAllocation
LIRGenerator::useBoxFixedAtStart(MDefinition* mir, ValueOperand op)
{
#if defined(JS_NUNBOX32)
    return useBoxFixed(mir, op.typeReg(), op.payloadReg(), true);
#elif defined(JS_PUNBOX64)
    return useBoxFixed(mir, op.valueReg(), op.scratchReg(), true);
#endif
}

LBoxAllocation
LIRGenerator::useBoxAtStart(MDefinition* mir, LUse::Policy policy)
{
    return useBox(mir, policy, /* useAtStart = */ true);
}

void
LIRGenerator::visitCloneLiteral(MCloneLiteral* ins)
{
    MOZ_ASSERT(ins->type() == MIRType::Object);
    MOZ_ASSERT(ins->input()->type() == MIRType::Object);

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
# if MOZ_BIG_ENDIAN
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

static void
TryToUseImplicitInterruptCheck(MIRGraph& graph, MBasicBlock* backedge)
{
    // Implicit interrupt checks require wasm signal handlers to be installed.
    if (!wasm::HaveSignalHandlers() || JitOptions.ionInterruptWithoutSignals)
        return;

    // To avoid triggering expensive interrupts (backedge patching) in
    // requestMajorGC and requestMinorGC, use an implicit interrupt check only
    // if the loop body can not trigger GC or affect GC state like the store
    // buffer. We do this by checking there are no safepoints attached to LIR
    // instructions inside the loop.

    MBasicBlockIterator block = graph.begin(backedge->loopHeaderOfBackedge());
    LInterruptCheck* check = nullptr;
    while (true) {
        LBlock* lir = block->lir();
        for (LInstructionIterator iter = lir->begin(); iter != lir->end(); iter++) {
            if (iter->isInterruptCheck()) {
                if (!check) {
                    MOZ_ASSERT(*block == backedge->loopHeaderOfBackedge());
                    check = iter->toInterruptCheck();
                }
                continue;
            }

            MOZ_ASSERT_IF(iter->isPostWriteBarrierO() || iter->isPostWriteBarrierV() || iter->isPostWriteBarrierS(),
                          iter->safepoint());

            if (iter->safepoint())
                return;
        }
        if (*block == backedge)
            break;
        block++;
    }

    check->setImplicit();
}

void
LIRGenerator::visitGoto(MGoto* ins)
{
    if (!gen->compilingWasm() && ins->block()->isLoopBackedge())
        TryToUseImplicitInterruptCheck(graph, ins->block());

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
    if (opd->type() == MIRType::Value) {
        LTableSwitchV* lir = newLTableSwitchV(tableswitch);
        add(lir);
        return;
    }

    // Case indices are numeric, so other types will always go to the default case.
    if (opd->type() != MIRType::Int32 && opd->type() != MIRType::Double) {
        add(new(alloc()) LGoto(tableswitch->getDefault()));
        return;
    }

    // Return an LTableSwitch, capable of handling either an integer or
    // floating-point index.
    LAllocation index;
    LDefinition tempInt;
    if (opd->type() == MIRType::Int32) {
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
    LCheckOverRecursed* lir = new(alloc()) LCheckOverRecursed(temp());
    add(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitDefVar(MDefVar* ins)
{
    LDefVar* lir = new(alloc()) LDefVar(useRegisterAtStart(ins->environmentChain()));
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
    MDefinition* fun = ins->fun();
    MOZ_ASSERT(fun->type() == MIRType::Object);

    LDefFun* lir = new(alloc()) LDefFun(useRegisterAtStart(fun),
                                        useRegisterAtStart(ins->environmentChain()));
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
    MOZ_ASSERT(length->type() == MIRType::Int32);

    LNewArrayDynamicLength* lir = new(alloc()) LNewArrayDynamicLength(useRegister(length), temp());
    define(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitNewIterator(MNewIterator* ins)
{
    LNewIterator* lir = new(alloc()) LNewIterator(temp());
    define(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitNewTypedArray(MNewTypedArray* ins)
{
    LNewTypedArray* lir = new(alloc()) LNewTypedArray(temp(), temp());
    define(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitNewTypedArrayDynamicLength(MNewTypedArrayDynamicLength* ins)
{
    MDefinition* length = ins->length();
    MOZ_ASSERT(length->type() == MIRType::Int32);

    LNewTypedArrayDynamicLength* lir = new(alloc()) LNewTypedArrayDynamicLength(useRegister(length), temp());
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
LIRGenerator::visitNewNamedLambdaObject(MNewNamedLambdaObject* ins)
{
    LNewNamedLambdaObject* lir = new(alloc()) LNewNamedLambdaObject(temp());
    define(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitNewCallObject(MNewCallObject* ins)
{
    LNewCallObject* lir = new(alloc()) LNewCallObject(temp());
    define(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitNewSingletonCallObject(MNewSingletonCallObject* ins)
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
    MOZ_ASSERT(ins->input()->type() == MIRType::String);

    LNewStringObject* lir = new(alloc()) LNewStringObject(useRegister(ins->input()), temp());
    define(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitInitElem(MInitElem* ins)
{
    LInitElem* lir = new(alloc()) LInitElem(useRegisterAtStart(ins->getObject()),
                                            useBoxAtStart(ins->getId()),
                                            useBoxAtStart(ins->getValue()));
    add(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitInitElemGetterSetter(MInitElemGetterSetter* ins)
{
    LInitElemGetterSetter* lir =
        new(alloc()) LInitElemGetterSetter(useRegisterAtStart(ins->object()),
                                           useBoxAtStart(ins->idValue()),
                                           useRegisterAtStart(ins->value()));
    add(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitMutateProto(MMutateProto* ins)
{
    LMutateProto* lir = new(alloc()) LMutateProto(useRegisterAtStart(ins->getObject()),
                                                  useBoxAtStart(ins->getValue()));
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
    LAllocation callObj = useFixedAtStart(ins->getCallObject(), CallTempReg0);
    LCreateArgumentsObject* lir = new(alloc()) LCreateArgumentsObject(callObj, tempFixed(CallTempReg1),
                                                                      tempFixed(CallTempReg2),
                                                                      tempFixed(CallTempReg3));
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
    LSetArgumentsObjectArg* lir =
        new(alloc()) LSetArgumentsObjectArg(argsObj, useBox(ins->getValue()), temp());
    add(lir, ins);
}

void
LIRGenerator::visitReturnFromCtor(MReturnFromCtor* ins)
{
    LReturnFromCtor* lir = new(alloc()) LReturnFromCtor(useBox(ins->getValue()),
                                                        useRegister(ins->getObject()));
    define(lir, ins);
}

void
LIRGenerator::visitComputeThis(MComputeThis* ins)
{
    MOZ_ASSERT(ins->type() == MIRType::Value);
    MOZ_ASSERT(ins->input()->type() == MIRType::Value);

    // Don't use useBoxAtStart because ComputeThis has a safepoint and needs to
    // have its inputs in different registers than its return value so that
    // they aren't clobbered.
    LComputeThis* lir = new(alloc()) LComputeThis(useBox(ins->input()));
    defineBox(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitImplicitThis(MImplicitThis* ins)
{
    MDefinition* env = ins->envChain();
    MOZ_ASSERT(env->type() == MIRType::Object);

    LImplicitThis* lir = new(alloc()) LImplicitThis(useRegisterAtStart(env));
    defineReturn(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitArrowNewTarget(MArrowNewTarget* ins)
{
    MOZ_ASSERT(ins->type() == MIRType::Value);
    MOZ_ASSERT(ins->callee()->type() == MIRType::Object);

    LArrowNewTarget* lir = new(alloc()) LArrowNewTarget(useRegister(ins->callee()));
    defineBox(lir, ins);
}

bool
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
        if (arg->type() == MIRType::Value) {
            LStackArgV* stack = new(alloc()) LStackArgV(argslot, useBox(arg));
            add(stack);
        } else {
            // Known types can move constant types and/or payloads.
            LStackArgT* stack = new(alloc()) LStackArgT(argslot, arg->type(), useRegisterOrConstant(arg));
            add(stack);
        }

        if (!alloc().ensureBallast())
            return false;
    }
    return true;
}

void
LIRGenerator::visitCall(MCall* call)
{
    MOZ_ASSERT(call->getFunction()->type() == MIRType::Object);

    // In case of oom, skip the rest of the allocations.
    if (!lowerCallArguments(call)) {
        abort(AbortReason::Alloc, "OOM: LIRGenerator::visitCall");
        return;
    }

    WrappedFunction* target = call->getSingleTarget();

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
        if (target->isNativeWithCppEntry()) {
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
            lir = new(alloc()) LCallKnown(useFixedAtStart(call->getFunction(), CallTempReg0),
                                          tempFixed(CallTempReg2));
        }
    } else {
        // Call anything, using the most generic code.
        lir = new(alloc()) LCallGeneric(useFixedAtStart(call->getFunction(), CallTempReg0),
                                        tempFixed(CallTempReg1),
                                        tempFixed(CallTempReg2));
    }
    defineReturn(lir, call);
    assignSafepoint(lir, call);
}

void
LIRGenerator::visitApplyArgs(MApplyArgs* apply)
{
    MOZ_ASSERT(apply->getFunction()->type() == MIRType::Object);

    // Assert if the return value is already erased.
    MOZ_ASSERT(CallTempReg2 != JSReturnReg_Type);
    MOZ_ASSERT(CallTempReg2 != JSReturnReg_Data);

    LApplyArgsGeneric* lir = new(alloc()) LApplyArgsGeneric(
        useFixedAtStart(apply->getFunction(), CallTempReg3),
        useFixedAtStart(apply->getArgc(), CallTempReg0),
        useBoxFixedAtStart(apply->getThis(), CallTempReg4, CallTempReg5),
        tempFixed(CallTempReg1),  // object register
        tempFixed(CallTempReg2)); // stack counter register

    // Bailout is needed in the case of possible non-JSFunction callee or too
    // many values in the arguments array.  I'm going to use NonJSFunctionCallee
    // for the code even if that is not an adequate description.
    assignSnapshot(lir, Bailout_NonJSFunctionCallee);

    defineReturn(lir, apply);
    assignSafepoint(lir, apply);
}

void
LIRGenerator::visitApplyArray(MApplyArray* apply)
{
    MOZ_ASSERT(apply->getFunction()->type() == MIRType::Object);

    // Assert if the return value is already erased.
    MOZ_ASSERT(CallTempReg2 != JSReturnReg_Type);
    MOZ_ASSERT(CallTempReg2 != JSReturnReg_Data);

    LApplyArrayGeneric* lir = new(alloc()) LApplyArrayGeneric(
        useFixedAtStart(apply->getFunction(), CallTempReg3),
        useFixedAtStart(apply->getElements(), CallTempReg0),
        useBoxFixedAtStart(apply->getThis(), CallTempReg4, CallTempReg5),
        tempFixed(CallTempReg1),  // object register
        tempFixed(CallTempReg2)); // stack counter register

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

    if (type != MIRType::Value && !JitOptions.eagerCompilation) {
        MOZ_ASSERT_IF(checkIsFloat32, type == MIRType::Float32);
        MOZ_ASSERT_IF(!checkIsFloat32, type != MIRType::Float32);
    }
}

void
LIRGenerator::visitAssertRecoveredOnBailout(MAssertRecoveredOnBailout* assertion)
{
    MOZ_CRASH("AssertRecoveredOnBailout nodes are always recovered on bailouts.");
}

void
LIRGenerator::visitGetDynamicName(MGetDynamicName* ins)
{
    MDefinition* envChain = ins->getEnvironmentChain();
    MOZ_ASSERT(envChain->type() == MIRType::Object);

    MDefinition* name = ins->getName();
    MOZ_ASSERT(name->type() == MIRType::String);

    LGetDynamicName* lir = new(alloc()) LGetDynamicName(useFixedAtStart(envChain, CallTempReg0),
                                                        useFixedAtStart(name, CallTempReg1),
                                                        tempFixed(CallTempReg2),
                                                        tempFixed(CallTempReg3),
                                                        tempFixed(CallTempReg4));

    assignSnapshot(lir, Bailout_DynamicNameNotFound);
    defineReturn(lir, ins);
}

void
LIRGenerator::visitCallDirectEval(MCallDirectEval* ins)
{
    MDefinition* envChain = ins->getEnvironmentChain();
    MOZ_ASSERT(envChain->type() == MIRType::Object);

    MDefinition* string = ins->getString();
    MOZ_ASSERT(string->type() == MIRType::String);

    MDefinition* newTargetValue = ins->getNewTargetValue();

    LInstruction* lir = new(alloc()) LCallDirectEval(useRegisterAtStart(envChain),
                                                     useRegisterAtStart(string),
                                                     useBoxAtStart(newTargetValue));
    defineReturn(lir, ins);
    assignSafepoint(lir, ins);
}

static JSOp
ReorderComparison(JSOp op, MDefinition** lhsp, MDefinition** rhsp)
{
    MDefinition* lhs = *lhsp;
    MDefinition* rhs = *rhsp;

    if (lhs->maybeConstantValue()) {
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
    MOZ_ASSERT(opd->type() != MIRType::String);

    // Testing a constant.
    if (MConstant* constant = opd->maybeConstantValue()) {
        bool b;
        if (constant->valueToBoolean(&b)) {
            add(new(alloc()) LGoto(b ? ifTrue : ifFalse));
            return;
        }
    }

    if (opd->type() == MIRType::Value) {
        LDefinition temp0, temp1;
        if (test->operandMightEmulateUndefined()) {
            temp0 = temp();
            temp1 = temp();
        } else {
            temp0 = LDefinition::BogusTemp();
            temp1 = LDefinition::BogusTemp();
        }
        LTestVAndBranch* lir =
            new(alloc()) LTestVAndBranch(ifTrue, ifFalse, useBox(opd), tempDouble(), temp0, temp1);
        add(lir, test);
        return;
    }

    if (opd->type() == MIRType::ObjectOrNull) {
        LDefinition temp0 = test->operandMightEmulateUndefined() ? temp() : LDefinition::BogusTemp();
        add(new(alloc()) LTestOAndBranch(useRegister(opd), ifTrue, ifFalse, temp0), test);
        return;
    }

    // Objects are truthy, except if it might emulate undefined.
    if (opd->type() == MIRType::Object) {
        if (test->operandMightEmulateUndefined())
            add(new(alloc()) LTestOAndBranch(useRegister(opd), ifTrue, ifFalse, temp()), test);
        else
            add(new(alloc()) LGoto(ifTrue));
        return;
    }

    // These must be explicitly sniffed out since they are constants and have
    // no payload.
    if (opd->type() == MIRType::Undefined || opd->type() == MIRType::Null) {
        add(new(alloc()) LGoto(ifFalse));
        return;
    }

    // All symbols are truthy.
    if (opd->type() == MIRType::Symbol) {
        add(new(alloc()) LGoto(ifTrue));
        return;
    }

    if (opd->isCompare() && opd->isEmittedAtUses()) {
        // Emit LBitAndBranch for cases like |if ((x & y) === 0)|.
        MCompare* comp = opd->toCompare();
        if ((comp->isInt32Comparison() ||
             comp->compareType() == MCompare::Compare_UInt32) &&
            comp->getOperand(1)->isConstant() &&
            comp->getOperand(1)->toConstant()->isInt32(0) &&
            comp->getOperand(0)->isBitAnd() &&
            comp->getOperand(0)->isEmittedAtUses())
        {
            MDefinition* bitAnd = opd->getOperand(0);
            MDefinition* lhs = bitAnd->getOperand(0);
            MDefinition* rhs = bitAnd->getOperand(1);

            Assembler::Condition cond = JSOpToCondition(comp->compareType(),
                                                        comp->jsop());

            if (lhs->type() == MIRType::Int32 && rhs->type() == MIRType::Int32 &&
                (cond == Assembler::Equal || cond == Assembler::NotEqual))
            {
                ReorderCommutative(&lhs, &rhs, test);
                if (cond == Assembler::Equal)
                    cond = Assembler::Zero;
                else if(cond == Assembler::NotEqual)
                    cond = Assembler::NonZero;
                else
                    MOZ_ASSERT_UNREACHABLE("inequality operators cannot be folded");
                lowerForBitAndAndBranch(new(alloc()) LBitAndAndBranch(ifTrue, ifFalse, cond),
                                        test, lhs, rhs);
                return;
            }
        }
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
            if (left->type() == MIRType::Object || left->type() == MIRType::ObjectOrNull) {
                MOZ_ASSERT(left->type() == MIRType::ObjectOrNull ||
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
                new(alloc()) LIsNullOrLikeUndefinedAndBranchV(comp, ifTrue, ifFalse, useBox(left),
                                                              tmp, tmpToUnbox);
            add(lir, test);
            return;
        }

        // Compare and branch booleans.
        if (comp->compareType() == MCompare::Compare_Boolean) {
            MOZ_ASSERT(left->type() == MIRType::Value);
            MOZ_ASSERT(right->type() == MIRType::Boolean);

            LCompareBAndBranch* lir = new(alloc()) LCompareBAndBranch(comp, useBox(left),
                                                                      useRegisterOrConstant(right),
                                                                      ifTrue, ifFalse);
            add(lir, test);
            return;
        }

        // Compare and branch Int32, Symbol or Object pointers.
        if (comp->isInt32Comparison() ||
            comp->compareType() == MCompare::Compare_UInt32 ||
            comp->compareType() == MCompare::Compare_Object ||
            comp->compareType() == MCompare::Compare_Symbol)
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

        // Compare and branch Int64.
        if (comp->compareType() == MCompare::Compare_Int64 ||
            comp->compareType() == MCompare::Compare_UInt64)
        {
            JSOp op = ReorderComparison(comp->jsop(), &left, &right);
            LCompareI64AndBranch* lir = new(alloc()) LCompareI64AndBranch(comp, op,
                                                                          useInt64Register(left),
                                                                          useInt64OrConstant(right),
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
                new(alloc()) LCompareBitwiseAndBranch(comp, ifTrue, ifFalse,
                                                      useBoxAtStart(left),
                                                      useBoxAtStart(right));
            add(lir, test);
            return;
        }
    }

    // Check if the operand for this test is a bitand operation. If it is, we want
    // to emit an LBitAndAndBranch rather than an LTest*AndBranch.
    if (opd->isBitAnd() && opd->isEmittedAtUses()) {
        MDefinition* lhs = opd->getOperand(0);
        MDefinition* rhs = opd->getOperand(1);
        if (lhs->type() == MIRType::Int32 && rhs->type() == MIRType::Int32) {
            ReorderCommutative(&lhs, &rhs, test);
            lowerForBitAndAndBranch(new(alloc()) LBitAndAndBranch(ifTrue, ifFalse), test, lhs, rhs);
            return;
        }
    }

    if (opd->isIsObject() && opd->isEmittedAtUses()) {
        MDefinition* input = opd->toIsObject()->input();
        MOZ_ASSERT(input->type() == MIRType::Value);

        LIsObjectAndBranch* lir = new(alloc()) LIsObjectAndBranch(ifTrue, ifFalse,
                                                                  useBoxAtStart(input));
        add(lir, test);
        return;
    }

    if (opd->isIsNoIter()) {
        MOZ_ASSERT(opd->isEmittedAtUses());

        MDefinition* input = opd->toIsNoIter()->input();
        MOZ_ASSERT(input->type() == MIRType::Value);

        LIsNoIterAndBranch* lir = new(alloc()) LIsNoIterAndBranch(ifTrue, ifFalse,
                                                                  useBox(input));
        add(lir, test);
        return;
    }

    switch (opd->type()) {
      case MIRType::Double:
        add(new(alloc()) LTestDAndBranch(useRegister(opd), ifTrue, ifFalse));
        break;
      case MIRType::Float32:
        add(new(alloc()) LTestFAndBranch(useRegister(opd), ifTrue, ifFalse));
        break;
      case MIRType::Int32:
      case MIRType::Boolean:
        add(new(alloc()) LTestIAndBranch(useRegister(opd), ifTrue, ifFalse));
        break;
      case MIRType::Int64:
        add(new(alloc()) LTestI64AndBranch(useInt64Register(opd), ifTrue, ifFalse));
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
        MOZ_ASSERT(left->type() == MIRType::Value);
        MOZ_ASSERT(right->type() == MIRType::String);

        LCompareStrictS* lir = new(alloc()) LCompareStrictS(useBox(left), useRegister(right),
                                                            tempToUnbox());
        define(lir, comp);
        assignSafepoint(lir, comp);
        return;
    }

    // Unknown/unspecialized compare use a VM call.
    if (comp->compareType() == MCompare::Compare_Unknown) {
        LCompareVM* lir = new(alloc()) LCompareVM(useBoxAtStart(left), useBoxAtStart(right));
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
        if (left->type() == MIRType::Object || left->type() == MIRType::ObjectOrNull) {
            MOZ_ASSERT(left->type() == MIRType::ObjectOrNull ||
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

        LIsNullOrLikeUndefinedV* lir = new(alloc()) LIsNullOrLikeUndefinedV(useBox(left),
                                                                            tmp, tmpToUnbox);
        define(lir, comp);
        return;
    }

    // Compare booleans.
    if (comp->compareType() == MCompare::Compare_Boolean) {
        MOZ_ASSERT(left->type() == MIRType::Value);
        MOZ_ASSERT(right->type() == MIRType::Boolean);

        LCompareB* lir = new(alloc()) LCompareB(useBox(left), useRegisterOrConstant(right));
        define(lir, comp);
        return;
    }

    // Compare Int32, Symbol or Object pointers.
    if (comp->isInt32Comparison() ||
        comp->compareType() == MCompare::Compare_UInt32 ||
        comp->compareType() == MCompare::Compare_Object ||
        comp->compareType() == MCompare::Compare_Symbol)
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

    // Compare Int64.
    if (comp->compareType() == MCompare::Compare_Int64 ||
        comp->compareType() == MCompare::Compare_UInt64)
    {
        JSOp op = ReorderComparison(comp->jsop(), &left, &right);
        define(new(alloc()) LCompareI64(op, useInt64Register(left), useInt64OrConstant(right)),
               comp);
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
        LCompareBitwise* lir = new(alloc()) LCompareBitwise(useBoxAtStart(left),
                                                            useBoxAtStart(right));
        define(lir, comp);
        return;
    }

    MOZ_CRASH("Unrecognized compare type.");
}

void
LIRGenerator::visitSameValue(MSameValue* ins)
{
    MDefinition* lhs = ins->lhs();
    MDefinition* rhs = ins->rhs();

    if (lhs->type() == MIRType::Double && rhs->type() == MIRType::Double) {
        auto* lir = new(alloc()) LSameValueD(useRegister(lhs), useRegister(rhs), tempDouble());
        define(lir, ins);
        return;
    }

    if (lhs->type() == MIRType::Value && rhs->type() == MIRType::Double) {
        auto* lir = new(alloc()) LSameValueV(useBox(lhs), useRegister(rhs), tempDouble(),
                                             tempDouble());
        define(lir, ins);
        return;
    }

    MOZ_ASSERT(lhs->type() == MIRType::Value);
    MOZ_ASSERT(rhs->type() == MIRType::Value);

    auto* lir = new(alloc()) LSameValueVM(useBoxAtStart(lhs), useBoxAtStart(rhs));
    defineReturn(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::lowerBitOp(JSOp op, MInstruction* ins)
{
    MDefinition* lhs = ins->getOperand(0);
    MDefinition* rhs = ins->getOperand(1);

    if (lhs->type() == MIRType::Int32) {
        MOZ_ASSERT(rhs->type() == MIRType::Int32);
        ReorderCommutative(&lhs, &rhs, ins);
        lowerForALU(new(alloc()) LBitOpI(op), ins, lhs, rhs);
        return;
    }

    if (lhs->type() == MIRType::Int64) {
        MOZ_ASSERT(rhs->type() == MIRType::Int64);
        ReorderCommutative(&lhs, &rhs, ins);
        lowerForALUInt64(new(alloc()) LBitOpI64(op), ins, lhs, rhs);
        return;
    }

    LBitOpV* lir = new(alloc()) LBitOpV(op, useBoxAtStart(lhs), useBoxAtStart(rhs));
    defineReturn(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitTypeOf(MTypeOf* ins)
{
    MDefinition* opd = ins->input();
    MOZ_ASSERT(opd->type() == MIRType::Value);

    LTypeOfV* lir = new(alloc()) LTypeOfV(useBox(opd), tempToUnbox());
    define(lir, ins);
}

void
LIRGenerator::visitToAsync(MToAsync* ins)
{
    LToAsync* lir = new(alloc()) LToAsync(useRegisterAtStart(ins->input()));
    defineReturn(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitToAsyncGen(MToAsyncGen* ins)
{
    LToAsyncGen* lir = new(alloc()) LToAsyncGen(useRegisterAtStart(ins->input()));
    defineReturn(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitToAsyncIter(MToAsyncIter* ins)
{
    LToAsyncIter* lir = new(alloc()) LToAsyncIter(useRegisterAtStart(ins->getIterator()),
                                                  useBoxAtStart(ins->getNextMethod()));
    defineReturn(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitToId(MToId* ins)
{
    LToIdV* lir = new(alloc()) LToIdV(useBox(ins->input()), tempDouble());
    defineBox(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitBitNot(MBitNot* ins)
{
    MDefinition* input = ins->getOperand(0);

    if (input->type() == MIRType::Int32) {
        lowerForALU(new(alloc()) LBitNotI(), ins, input);
        return;
    }

    LBitNotV* lir = new(alloc()) LBitNotV(useBoxAtStart(input));
    defineReturn(lir, ins);
    assignSafepoint(lir, ins);
}

static bool
CanEmitBitAndAtUses(MInstruction* ins)
{
    if (!ins->canEmitAtUses())
        return false;

    if (ins->getOperand(0)->type() != MIRType::Int32 || ins->getOperand(1)->type() != MIRType::Int32)
        return false;

    MUseIterator iter(ins->usesBegin());
    if (iter == ins->usesEnd())
        return false;

    MNode* node = iter->consumer();
    if (!node->isDefinition() || !node->toDefinition()->isInstruction())
        return false;

    MInstruction* use = node->toDefinition()->toInstruction();
    if (!use->isTest() && !(use->isCompare() && CanEmitCompareAtUses(use)))
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

    if (lhs->type() == MIRType::Int32) {
        MOZ_ASSERT(rhs->type() == MIRType::Int32);

        if (ins->type() == MIRType::Double) {
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

    if (lhs->type() == MIRType::Int64) {
        MOZ_ASSERT(rhs->type() == MIRType::Int64);
        lowerForShiftInt64(new(alloc()) LShiftI64(op), ins, lhs, rhs);
        return;
    }

    MOZ_ASSERT(ins->specialization() == MIRType::None);

    if (op == JSOP_URSH) {
        // Result is either int32 or double so we have to use BinaryV.
        lowerBinaryV(JSOP_URSH, ins);
        return;
    }

    LBitOpV* lir = new(alloc()) LBitOpV(op, useBoxAtStart(lhs), useBoxAtStart(rhs));
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
LIRGenerator::visitSignExtendInt32(MSignExtendInt32* ins)
{
    LInstructionHelper<1, 1, 0>* lir;

    if (ins->mode() == MSignExtendInt32::Byte)
        lir = new(alloc()) LSignExtendInt32(useByteOpRegisterAtStart(ins->input()), ins->mode());
    else
        lir = new(alloc()) LSignExtendInt32(useRegisterAtStart(ins->input()), ins->mode());

    define(lir, ins);
}

void
LIRGenerator::visitRotate(MRotate* ins)
{
    MDefinition* input = ins->input();
    MDefinition* count = ins->count();

    if (ins->type() == MIRType::Int32) {
        auto* lir = new(alloc()) LRotate();
        lowerForShift(lir, ins, input, count);
    } else if (ins->type() == MIRType::Int64) {
        auto* lir = new(alloc()) LRotateI64();
        lowerForShiftInt64(lir, ins, input, count);
    } else {
        MOZ_CRASH("unexpected type in visitRotate");
    }
}

void
LIRGenerator::visitFloor(MFloor* ins)
{
    MIRType type = ins->input()->type();
    MOZ_ASSERT(IsFloatingPointType(type));

    LInstructionHelper<1, 1, 0>* lir;
    if (type == MIRType::Double)
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
    if (type == MIRType::Double)
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
    if (type == MIRType::Double)
        lir = new (alloc()) LRound(useRegister(ins->input()), tempDouble());
    else
        lir = new (alloc()) LRoundF(useRegister(ins->input()), tempFloat32());

    assignSnapshot(lir, Bailout_Round);
    define(lir, ins);
}

void
LIRGenerator::visitNearbyInt(MNearbyInt* ins)
{
    MIRType inputType = ins->input()->type();
    MOZ_ASSERT(IsFloatingPointType(inputType));
    MOZ_ASSERT(ins->type() == inputType);

    LInstructionHelper<1, 1, 0>* lir;
    if (inputType == MIRType::Double)
        lir = new(alloc()) LNearbyInt(useRegisterAtStart(ins->input()));
    else
        lir = new(alloc()) LNearbyIntF(useRegisterAtStart(ins->input()));

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
      case MIRType::Int32:
        lir = new(alloc()) LMinMaxI(useRegisterAtStart(first), useRegisterOrConstant(second));
        break;
      case MIRType::Float32:
        lir = new(alloc()) LMinMaxF(useRegisterAtStart(first), useRegister(second));
        break;
      case MIRType::Double:
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
      case MIRType::Int32:
        lir = new(alloc()) LAbsI(useRegisterAtStart(num));
        // needed to handle abs(INT32_MIN)
        if (ins->fallible())
            assignSnapshot(lir, Bailout_Overflow);
        break;
      case MIRType::Float32:
        lir = new(alloc()) LAbsF(useRegisterAtStart(num));
        break;
      case MIRType::Double:
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

    MOZ_ASSERT(IsIntType(ins->type()));

    if (ins->type() == MIRType::Int32) {
        LClzI* lir = new(alloc()) LClzI(useRegisterAtStart(num));
        define(lir, ins);
        return;
    }

    auto* lir = new(alloc()) LClzI64(useInt64RegisterAtStart(num));
    defineInt64(lir, ins);
}

void
LIRGenerator::visitCtz(MCtz* ins)
{
    MDefinition* num = ins->num();

    MOZ_ASSERT(IsIntType(ins->type()));

    if (ins->type() == MIRType::Int32) {
        LCtzI* lir = new(alloc()) LCtzI(useRegisterAtStart(num));
        define(lir, ins);
        return;
    }

    auto* lir = new(alloc()) LCtzI64(useInt64RegisterAtStart(num));
    defineInt64(lir, ins);
}

void
LIRGenerator::visitPopcnt(MPopcnt* ins)
{
    MDefinition* num = ins->num();

    MOZ_ASSERT(IsIntType(ins->type()));

    if (ins->type() == MIRType::Int32) {
        LPopcntI* lir = new(alloc()) LPopcntI(useRegisterAtStart(num), temp());
        define(lir, ins);
        return;
    }

    auto* lir = new(alloc()) LPopcntI64(useInt64RegisterAtStart(num), temp());
    defineInt64(lir, ins);
}

void
LIRGenerator::visitSqrt(MSqrt* ins)
{
    MDefinition* num = ins->input();
    MOZ_ASSERT(IsFloatingPointType(num->type()));

    LInstructionHelper<1, 1, 0>* lir;
    if (num->type() == MIRType::Double)
        lir = new(alloc()) LSqrtD(useRegisterAtStart(num));
    else
        lir = new(alloc()) LSqrtF(useRegisterAtStart(num));
    define(lir, ins);
}

void
LIRGenerator::visitAtan2(MAtan2* ins)
{
    MDefinition* y = ins->y();
    MOZ_ASSERT(y->type() == MIRType::Double);

    MDefinition* x = ins->x();
    MOZ_ASSERT(x->type() == MIRType::Double);

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
        MOZ_ASSERT(ins->getOperand(i)->type() == MIRType::Double);

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
    MDefinition* power = ins->power();
    LInstruction* lir;

    if (ins->specialization() == MIRType::None) {
        MOZ_ASSERT(input->type() == MIRType::Value);
        MOZ_ASSERT(power->type() == MIRType::Value);

        lir = new(alloc()) LPowV(useBoxAtStart(input), useBoxAtStart(power));
        defineReturn(lir, ins);
        assignSafepoint(lir, ins);
        return;
    }

    MOZ_ASSERT(input->type() == MIRType::Double);
    MOZ_ASSERT(power->type() == MIRType::Int32 || power->type() == MIRType::Double);

    if (power->type() == MIRType::Int32) {
        // Note: useRegisterAtStart here is safe, the temp is a GP register so
        // it will never get the same register.
        lir = new(alloc()) LPowI(useRegisterAtStart(input), useFixedAtStart(power, CallTempReg1),
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
    MOZ_ASSERT_IF(ins->input()->type() != MIRType::SinCosDouble,
                  ins->type() == ins->input()->type());

    if (ins->input()->type() == MIRType::SinCosDouble) {
        MOZ_ASSERT(ins->type() == MIRType::Double);
        redefine(ins, ins->input(), ins->function());
        return;
    }

    LInstruction* lir;
    if (ins->type() == MIRType::Double) {
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

    if (ins->specialization() == MIRType::Int32) {
        MOZ_ASSERT(lhs->type() == MIRType::Int32);
        ReorderCommutative(&lhs, &rhs, ins);
        LAddI* lir = new(alloc()) LAddI;

        if (ins->fallible())
            assignSnapshot(lir, Bailout_OverflowInvalidate);

        lowerForALU(lir, ins, lhs, rhs);
        MaybeSetRecoversInput(ins, lir);
        return;
    }

    if (ins->specialization() == MIRType::Int64) {
        MOZ_ASSERT(lhs->type() == MIRType::Int64);
        ReorderCommutative(&lhs, &rhs, ins);
        LAddI64* lir = new(alloc()) LAddI64;
        lowerForALUInt64(lir, ins, lhs, rhs);
        return;
    }

    if (ins->specialization() == MIRType::Double) {
        MOZ_ASSERT(lhs->type() == MIRType::Double);
        ReorderCommutative(&lhs, &rhs, ins);
        lowerForFPU(new(alloc()) LMathD(JSOP_ADD), ins, lhs, rhs);
        return;
    }

    if (ins->specialization() == MIRType::Float32) {
        MOZ_ASSERT(lhs->type() == MIRType::Float32);
        ReorderCommutative(&lhs, &rhs, ins);
        lowerForFPU(new(alloc()) LMathF(JSOP_ADD), ins, lhs, rhs);
        return;
    }

    lowerBinaryV(JSOP_ADD, ins);
}

void
LIRGenerator::visitSub(MSub* ins)
{
    MDefinition* lhs = ins->lhs();
    MDefinition* rhs = ins->rhs();

    MOZ_ASSERT(lhs->type() == rhs->type());

    if (ins->specialization() == MIRType::Int32) {
        MOZ_ASSERT(lhs->type() == MIRType::Int32);

        LSubI* lir = new(alloc()) LSubI;
        if (ins->fallible())
            assignSnapshot(lir, Bailout_Overflow);

        lowerForALU(lir, ins, lhs, rhs);
        MaybeSetRecoversInput(ins, lir);
        return;
    }

    if (ins->specialization() == MIRType::Int64) {
        MOZ_ASSERT(lhs->type() == MIRType::Int64);
        LSubI64* lir = new(alloc()) LSubI64;
        lowerForALUInt64(lir, ins, lhs, rhs);
        return;
    }

    if (ins->specialization() == MIRType::Double) {
        MOZ_ASSERT(lhs->type() == MIRType::Double);
        lowerForFPU(new(alloc()) LMathD(JSOP_SUB), ins, lhs, rhs);
        return;
    }

    if (ins->specialization() == MIRType::Float32) {
        MOZ_ASSERT(lhs->type() == MIRType::Float32);
        lowerForFPU(new(alloc()) LMathF(JSOP_SUB), ins, lhs, rhs);
        return;
    }

    lowerBinaryV(JSOP_SUB, ins);
}

void
LIRGenerator::visitMul(MMul* ins)
{
    MDefinition* lhs = ins->lhs();
    MDefinition* rhs = ins->rhs();
    MOZ_ASSERT(lhs->type() == rhs->type());

    if (ins->specialization() == MIRType::Int32) {
        MOZ_ASSERT(lhs->type() == MIRType::Int32);
        ReorderCommutative(&lhs, &rhs, ins);

        // If our RHS is a constant -1 and we don't have to worry about
        // overflow, we can optimize to an LNegI.
        if (!ins->fallible() && rhs->isConstant() && rhs->toConstant()->toInt32() == -1)
            defineReuseInput(new(alloc()) LNegI(useRegisterAtStart(lhs)), ins, 0);
        else
            lowerMulI(ins, lhs, rhs);
        return;
    }

    if (ins->specialization() == MIRType::Int64) {
        MOZ_ASSERT(lhs->type() == MIRType::Int64);
        ReorderCommutative(&lhs, &rhs, ins);
        LMulI64* lir = new(alloc()) LMulI64;
        lowerForMulInt64(lir, ins, lhs, rhs);
        return;
    }

    if (ins->specialization() == MIRType::Double) {
        MOZ_ASSERT(lhs->type() == MIRType::Double);
        ReorderCommutative(&lhs, &rhs, ins);

        // If our RHS is a constant -1.0, we can optimize to an LNegD.
        if (!ins->mustPreserveNaN() && rhs->isConstant() && rhs->toConstant()->toDouble() == -1.0)
            defineReuseInput(new(alloc()) LNegD(useRegisterAtStart(lhs)), ins, 0);
        else
            lowerForFPU(new(alloc()) LMathD(JSOP_MUL), ins, lhs, rhs);
        return;
    }

    if (ins->specialization() == MIRType::Float32) {
        MOZ_ASSERT(lhs->type() == MIRType::Float32);
        ReorderCommutative(&lhs, &rhs, ins);

        // We apply the same optimizations as for doubles
        if (!ins->mustPreserveNaN() && rhs->isConstant() && rhs->toConstant()->toFloat32() == -1.0f)
            defineReuseInput(new(alloc()) LNegF(useRegisterAtStart(lhs)), ins, 0);
        else
            lowerForFPU(new(alloc()) LMathF(JSOP_MUL), ins, lhs, rhs);
        return;
    }

    lowerBinaryV(JSOP_MUL, ins);
}

void
LIRGenerator::visitDiv(MDiv* ins)
{
    MDefinition* lhs = ins->lhs();
    MDefinition* rhs = ins->rhs();
    MOZ_ASSERT(lhs->type() == rhs->type());

    if (ins->specialization() == MIRType::Int32) {
        MOZ_ASSERT(lhs->type() == MIRType::Int32);
        lowerDivI(ins);
        return;
    }

    if (ins->specialization() == MIRType::Int64) {
        MOZ_ASSERT(lhs->type() == MIRType::Int64);
        lowerDivI64(ins);
        return;
    }

    if (ins->specialization() == MIRType::Double) {
        MOZ_ASSERT(lhs->type() == MIRType::Double);
        lowerForFPU(new(alloc()) LMathD(JSOP_DIV), ins, lhs, rhs);
        return;
    }

    if (ins->specialization() == MIRType::Float32) {
        MOZ_ASSERT(lhs->type() == MIRType::Float32);
        lowerForFPU(new(alloc()) LMathF(JSOP_DIV), ins, lhs, rhs);
        return;
    }

    lowerBinaryV(JSOP_DIV, ins);
}

void
LIRGenerator::visitMod(MMod* ins)
{
    MOZ_ASSERT(ins->lhs()->type() == ins->rhs()->type());

    if (ins->specialization() == MIRType::Int32) {
        MOZ_ASSERT(ins->type() == MIRType::Int32);
        MOZ_ASSERT(ins->lhs()->type() == MIRType::Int32);
        lowerModI(ins);
        return;
    }

    if (ins->specialization() == MIRType::Int64) {
        MOZ_ASSERT(ins->type() == MIRType::Int64);
        MOZ_ASSERT(ins->lhs()->type() == MIRType::Int64);
        lowerModI64(ins);
        return;
    }

    if (ins->specialization() == MIRType::Double) {
        MOZ_ASSERT(ins->type() == MIRType::Double);
        MOZ_ASSERT(ins->lhs()->type() == MIRType::Double);
        MOZ_ASSERT(ins->rhs()->type() == MIRType::Double);

        // Ion does an unaligned ABI call and thus needs a temp register. Wasm
        // doesn't.
        LDefinition maybeTemp = gen->compilingWasm()
                                ? LDefinition::BogusTemp()
                                : tempFixed(CallTempReg0);

        // Note: useRegisterAtStart is safe here, the temp is not a FP register.
        LModD* lir = new(alloc()) LModD(useRegisterAtStart(ins->lhs()),
                                        useRegisterAtStart(ins->rhs()),
                                        maybeTemp);
        defineReturn(lir, ins);
        return;
    }

    lowerBinaryV(JSOP_MOD, ins);
}

void
LIRGenerator::lowerBinaryV(JSOp op, MBinaryInstruction* ins)
{
    MDefinition* lhs = ins->getOperand(0);
    MDefinition* rhs = ins->getOperand(1);

    MOZ_ASSERT(lhs->type() == MIRType::Value);
    MOZ_ASSERT(rhs->type() == MIRType::Value);

    LBinaryV* lir = new(alloc()) LBinaryV(op, useBoxAtStart(lhs), useBoxAtStart(rhs));
    defineReturn(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitConcat(MConcat* ins)
{
    MDefinition* lhs = ins->getOperand(0);
    MDefinition* rhs = ins->getOperand(1);

    MOZ_ASSERT(lhs->type() == MIRType::String);
    MOZ_ASSERT(rhs->type() == MIRType::String);
    MOZ_ASSERT(ins->type() == MIRType::String);

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

    MOZ_ASSERT(str->type() == MIRType::String);
    MOZ_ASSERT(idx->type() == MIRType::Int32);

    LCharCodeAt* lir = new(alloc()) LCharCodeAt(useRegister(str), useRegister(idx), temp());
    define(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitFromCharCode(MFromCharCode* ins)
{
    MDefinition* code = ins->getOperand(0);

    MOZ_ASSERT(code->type() == MIRType::Int32);

    LFromCharCode* lir = new(alloc()) LFromCharCode(useRegister(code));
    define(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitFromCodePoint(MFromCodePoint* ins)
{
    MDefinition* codePoint = ins->getOperand(0);

    MOZ_ASSERT(codePoint->type() == MIRType::Int32);

    LFromCodePoint* lir = new(alloc()) LFromCodePoint(useRegister(codePoint), temp(), temp());
    assignSnapshot(lir, Bailout_BoundsCheck);
    define(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitStringConvertCase(MStringConvertCase* ins)
{
    MOZ_ASSERT(ins->string()->type() == MIRType::String);

    auto* lir = new(alloc()) LStringConvertCase(useRegisterAtStart(ins->string()));
    defineReturn(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitStart(MStart* start)
{
    LStart* lir = new(alloc()) LStart;

    // Create a snapshot that captures the initial state of the function.
    assignSnapshot(lir, Bailout_ArgumentCheck);
    if (start->block()->graph().entryBlock() == start->block())
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
LIRGenerator::visitOsrEnvironmentChain(MOsrEnvironmentChain* object)
{
    LOsrEnvironmentChain* lir = new(alloc()) LOsrEnvironmentChain(useRegister(object->entry()));
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
      case MIRType::Value:
      {
        LValueToDouble* lir = new(alloc()) LValueToDouble(useBox(opd));
        assignSnapshot(lir, Bailout_NonPrimitiveInput);
        define(lir, convert);
        break;
      }

      case MIRType::Null:
        MOZ_ASSERT(conversion != MToFPInstruction::NumbersOnly &&
                   conversion != MToFPInstruction::NonNullNonStringPrimitives);
        lowerConstantDouble(0, convert);
        break;

      case MIRType::Undefined:
        MOZ_ASSERT(conversion != MToFPInstruction::NumbersOnly);
        lowerConstantDouble(GenericNaN(), convert);
        break;

      case MIRType::Boolean:
        MOZ_ASSERT(conversion != MToFPInstruction::NumbersOnly);
        MOZ_FALLTHROUGH;

      case MIRType::Int32:
      {
        LInt32ToDouble* lir = new(alloc()) LInt32ToDouble(useRegisterAtStart(opd));
        define(lir, convert);
        break;
      }

      case MIRType::Float32:
      {
        LFloat32ToDouble* lir = new (alloc()) LFloat32ToDouble(useRegisterAtStart(opd));
        define(lir, convert);
        break;
      }

      case MIRType::Double:
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
      case MIRType::Value:
      {
        LValueToFloat32* lir = new(alloc()) LValueToFloat32(useBox(opd));
        assignSnapshot(lir, Bailout_NonPrimitiveInput);
        define(lir, convert);
        break;
      }

      case MIRType::Null:
        MOZ_ASSERT(conversion != MToFPInstruction::NumbersOnly &&
                   conversion != MToFPInstruction::NonNullNonStringPrimitives);
        lowerConstantFloat32(0, convert);
        break;

      case MIRType::Undefined:
        MOZ_ASSERT(conversion != MToFPInstruction::NumbersOnly);
        lowerConstantFloat32(GenericNaN(), convert);
        break;

      case MIRType::Boolean:
        MOZ_ASSERT(conversion != MToFPInstruction::NumbersOnly);
        MOZ_FALLTHROUGH;

      case MIRType::Int32:
      {
        LInt32ToFloat32* lir = new(alloc()) LInt32ToFloat32(useRegisterAtStart(opd));
        define(lir, convert);
        break;
      }

      case MIRType::Double:
      {
        LDoubleToFloat32* lir = new(alloc()) LDoubleToFloat32(useRegisterAtStart(opd));
        define(lir, convert);
        break;
      }

      case MIRType::Float32:
        redefine(convert, opd);
        break;

      default:
        // Objects might be effectful. Symbols will throw.
        // Strings are complicated - we don't handle them yet.
        MOZ_CRASH("unexpected type");
    }
}

void
LIRGenerator::visitToNumberInt32(MToNumberInt32* convert)
{
    MDefinition* opd = convert->input();

    switch (opd->type()) {
      case MIRType::Value:
      {
        auto* lir =
            new(alloc()) LValueToInt32(useBox(opd), tempDouble(), temp(), LValueToInt32::NORMAL);
        assignSnapshot(lir, Bailout_NonPrimitiveInput);
        define(lir, convert);
        assignSafepoint(lir, convert);
        break;
      }

      case MIRType::Null:
        MOZ_ASSERT(convert->conversion() == IntConversionInputKind::Any);
        define(new(alloc()) LInteger(0), convert);
        break;

      case MIRType::Boolean:
        MOZ_ASSERT(convert->conversion() == IntConversionInputKind::Any ||
                   convert->conversion() == IntConversionInputKind::NumbersOrBoolsOnly);
        redefine(convert, opd);
        break;

      case MIRType::Int32:
        redefine(convert, opd);
        break;

      case MIRType::Float32:
      {
        LFloat32ToInt32* lir = new(alloc()) LFloat32ToInt32(useRegister(opd));
        assignSnapshot(lir, Bailout_PrecisionLoss);
        define(lir, convert);
        break;
      }

      case MIRType::Double:
      {
        LDoubleToInt32* lir = new(alloc()) LDoubleToInt32(useRegister(opd));
        assignSnapshot(lir, Bailout_PrecisionLoss);
        define(lir, convert);
        break;
      }

      case MIRType::String:
      case MIRType::Symbol:
      case MIRType::Object:
      case MIRType::Undefined:
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
      case MIRType::Value: {
        LValueToInt32* lir = new(alloc()) LValueToInt32(useBox(opd), tempDouble(), temp(),
                                                        LValueToInt32::TRUNCATE);
        assignSnapshot(lir, Bailout_NonPrimitiveInput);
        define(lir, truncate);
        assignSafepoint(lir, truncate);
        break;
      }

      case MIRType::Null:
      case MIRType::Undefined:
        define(new(alloc()) LInteger(0), truncate);
        break;

      case MIRType::Int32:
      case MIRType::Boolean:
        redefine(truncate, opd);
        break;

      case MIRType::Double:
        // May call into JS::ToInt32() on the slow OOL path.
        gen->setNeedsStaticStackAlignment();
        lowerTruncateDToInt32(truncate);
        break;

      case MIRType::Float32:
        // May call into JS::ToInt32() on the slow OOL path.
        gen->setNeedsStaticStackAlignment();
        lowerTruncateFToInt32(truncate);
        break;

      default:
        // Objects might be effectful. Symbols throw.
        // Strings are complicated - we don't handle them yet.
        MOZ_CRASH("unexpected type");
    }
}

void
LIRGenerator::visitWasmTruncateToInt32(MWasmTruncateToInt32* ins)
{
    MDefinition* input = ins->input();
    switch (input->type()) {
      case MIRType::Double:
      case MIRType::Float32: {
        auto* lir = new(alloc()) LWasmTruncateToInt32(useRegisterAtStart(input));
        define(lir, ins);
        break;
      }
      default:
        MOZ_CRASH("unexpected type in WasmTruncateToInt32");
    }
}

void
LIRGenerator::visitWrapInt64ToInt32(MWrapInt64ToInt32* ins)
{
    define(new(alloc()) LWrapInt64ToInt32(useInt64AtStart(ins->input())), ins);
}

void
LIRGenerator::visitToString(MToString* ins)
{
    MDefinition* opd = ins->input();

    switch (opd->type()) {
      case MIRType::Null: {
        const JSAtomState& names = gen->runtime->names();
        LPointer* lir = new(alloc()) LPointer(names.null);
        define(lir, ins);
        break;
      }

      case MIRType::Undefined: {
        const JSAtomState& names = gen->runtime->names();
        LPointer* lir = new(alloc()) LPointer(names.undefined);
        define(lir, ins);
        break;
      }

      case MIRType::Boolean: {
        LBooleanToString* lir = new(alloc()) LBooleanToString(useRegister(opd));
        define(lir, ins);
        break;
      }

      case MIRType::Double: {
        LDoubleToString* lir = new(alloc()) LDoubleToString(useRegister(opd), temp());

        define(lir, ins);
        assignSafepoint(lir, ins);
        break;
      }

      case MIRType::Int32: {
        LIntToString* lir = new(alloc()) LIntToString(useRegister(opd));

        define(lir, ins);
        assignSafepoint(lir, ins);
        break;
      }

      case MIRType::String:
        redefine(ins, ins->input());
        break;

      case MIRType::Value: {
        LValueToString* lir = new(alloc()) LValueToString(useBox(opd), tempToUnbox());
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
LIRGenerator::visitToObject(MToObject* ins)
{
    MOZ_ASSERT(ins->input()->type() == MIRType::Value);

    LValueToObject* lir = new(alloc()) LValueToObject(useBox(ins->input()));
    define(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitToObjectOrNull(MToObjectOrNull* ins)
{
    MOZ_ASSERT(ins->input()->type() == MIRType::Value);

    LValueToObjectOrNull* lir = new(alloc()) LValueToObjectOrNull(useBox(ins->input()));
    define(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitRegExp(MRegExp* ins)
{
    if (ins->mustClone()) {
        LRegExp* lir = new(alloc()) LRegExp(temp());
        define(lir, ins);
        assignSafepoint(lir, ins);
    } else {
        RegExpObject* source = ins->source();
        define(new(alloc()) LPointer(source), ins);
    }
}

void
LIRGenerator::visitRegExpMatcher(MRegExpMatcher* ins)
{
    MOZ_ASSERT(ins->regexp()->type() == MIRType::Object);
    MOZ_ASSERT(ins->string()->type() == MIRType::String);
    MOZ_ASSERT(ins->lastIndex()->type() == MIRType::Int32);

    LRegExpMatcher* lir = new(alloc()) LRegExpMatcher(useFixedAtStart(ins->regexp(), RegExpMatcherRegExpReg),
                                                      useFixedAtStart(ins->string(), RegExpMatcherStringReg),
                                                      useFixedAtStart(ins->lastIndex(), RegExpMatcherLastIndexReg));
    defineReturn(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitRegExpSearcher(MRegExpSearcher* ins)
{
    MOZ_ASSERT(ins->regexp()->type() == MIRType::Object);
    MOZ_ASSERT(ins->string()->type() == MIRType::String);
    MOZ_ASSERT(ins->lastIndex()->type() == MIRType::Int32);

    LRegExpSearcher* lir = new(alloc()) LRegExpSearcher(useFixedAtStart(ins->regexp(), RegExpTesterRegExpReg),
                                                        useFixedAtStart(ins->string(), RegExpTesterStringReg),
                                                        useFixedAtStart(ins->lastIndex(), RegExpTesterLastIndexReg));
    defineReturn(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitRegExpTester(MRegExpTester* ins)
{
    MOZ_ASSERT(ins->regexp()->type() == MIRType::Object);
    MOZ_ASSERT(ins->string()->type() == MIRType::String);
    MOZ_ASSERT(ins->lastIndex()->type() == MIRType::Int32);

    LRegExpTester* lir = new(alloc()) LRegExpTester(useFixedAtStart(ins->regexp(), RegExpTesterRegExpReg),
                                                    useFixedAtStart(ins->string(), RegExpTesterStringReg),
                                                    useFixedAtStart(ins->lastIndex(), RegExpTesterLastIndexReg));
    defineReturn(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitRegExpPrototypeOptimizable(MRegExpPrototypeOptimizable* ins)
{
    MOZ_ASSERT(ins->object()->type() == MIRType::Object);
    MOZ_ASSERT(ins->type() == MIRType::Boolean);
    LRegExpPrototypeOptimizable* lir = new(alloc()) LRegExpPrototypeOptimizable(useRegister(ins->object()),
                                                                                temp());
    define(lir, ins);
}

void
LIRGenerator::visitRegExpInstanceOptimizable(MRegExpInstanceOptimizable* ins)
{
    MOZ_ASSERT(ins->object()->type() == MIRType::Object);
    MOZ_ASSERT(ins->proto()->type() == MIRType::Object);
    MOZ_ASSERT(ins->type() == MIRType::Boolean);
    LRegExpInstanceOptimizable* lir = new(alloc()) LRegExpInstanceOptimizable(useRegister(ins->object()),
                                                                              useRegister(ins->proto()),
                                                                              temp());
    define(lir, ins);
}

void
LIRGenerator::visitGetFirstDollarIndex(MGetFirstDollarIndex* ins)
{
    MOZ_ASSERT(ins->str()->type() == MIRType::String);
    MOZ_ASSERT(ins->type() == MIRType::Int32);
    LGetFirstDollarIndex* lir = new(alloc()) LGetFirstDollarIndex(useRegister(ins->str()),
                                                                  temp(), temp(), temp());
    define(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitStringReplace(MStringReplace* ins)
{
    MOZ_ASSERT(ins->pattern()->type() == MIRType::String);
    MOZ_ASSERT(ins->string()->type() == MIRType::String);
    MOZ_ASSERT(ins->replacement()->type() == MIRType::String);

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

    MOZ_ASSERT(ins->type() == MIRType::Value);
    MOZ_ASSERT(ins->type() == MIRType::Value);

    LBinarySharedStub* lir = new(alloc()) LBinarySharedStub(useBoxFixedAtStart(lhs, R0),
                                                            useBoxFixedAtStart(rhs, R1));
    defineSharedStubReturn(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitUnarySharedStub(MUnarySharedStub* ins)
{
    MDefinition* input = ins->getOperand(0);
    MOZ_ASSERT(ins->type() == MIRType::Value);

    LUnarySharedStub* lir = new(alloc()) LUnarySharedStub(useBoxFixedAtStart(input, R0));
    defineSharedStubReturn(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitNullarySharedStub(MNullarySharedStub* ins)
{
    MOZ_ASSERT(ins->type() == MIRType::Value);

    LNullarySharedStub* lir = new(alloc()) LNullarySharedStub();

    defineSharedStubReturn(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitClassConstructor(MClassConstructor* ins)
{
    LClassConstructor* lir = new(alloc()) LClassConstructor();
    defineReturn(lir, ins);
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
        LLambdaForSingleton* lir = new(alloc())
            LLambdaForSingleton(useRegisterAtStart(ins->environmentChain()));
        defineReturn(lir, ins);
        assignSafepoint(lir, ins);
    } else {
        LLambda* lir = new(alloc()) LLambda(useRegister(ins->environmentChain()), temp());
        define(lir, ins);
        assignSafepoint(lir, ins);
    }
}

void
LIRGenerator::visitLambdaArrow(MLambdaArrow* ins)
{
    MOZ_ASSERT(ins->environmentChain()->type() == MIRType::Object);
    MOZ_ASSERT(ins->newTargetDef()->type() == MIRType::Value);

    LLambdaArrow* lir = new(alloc()) LLambdaArrow(useRegister(ins->environmentChain()),
                                                  useBox(ins->newTargetDef()));
    define(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitSetFunName(MSetFunName* ins)
{
    MOZ_ASSERT(ins->fun()->type() == MIRType::Object);
    MOZ_ASSERT(ins->name()->type() == MIRType::Value);

    LSetFunName* lir = new(alloc()) LSetFunName(useRegisterAtStart(ins->fun()),
                                                useBoxAtStart(ins->name()));
    add(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitNewLexicalEnvironmentObject(MNewLexicalEnvironmentObject* ins)
{
    MDefinition* enclosing = ins->enclosing();
    MOZ_ASSERT(enclosing->type() == MIRType::Object);

    LNewLexicalEnvironmentObject* lir =
        new(alloc()) LNewLexicalEnvironmentObject(useRegisterAtStart(enclosing));

    defineReturn(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitCopyLexicalEnvironmentObject(MCopyLexicalEnvironmentObject* ins)
{
    MDefinition* env = ins->env();
    MOZ_ASSERT(env->type() == MIRType::Object);

    LCopyLexicalEnvironmentObject* lir =
        new(alloc()) LCopyLexicalEnvironmentObject(useRegisterAtStart(env));

    defineReturn(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitKeepAliveObject(MKeepAliveObject* ins)
{
    MDefinition* obj = ins->object();
    MOZ_ASSERT(obj->type() == MIRType::Object);

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
    MOZ_ASSERT(ins->elements()->type() == MIRType::Elements);
    MOZ_ASSERT(ins->value()->type() == MIRType::Int32);

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
      case MIRType::Value:
        defineBox(new(alloc()) LLoadSlotV(useRegisterAtStart(ins->slots())), ins);
        break;

      case MIRType::Undefined:
      case MIRType::Null:
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
LIRGenerator::visitHomeObject(MHomeObject* ins)
{
    define(new(alloc()) LHomeObject(useRegisterAtStart(ins->function())), ins);
}

void
LIRGenerator::visitHomeObjectSuperBase(MHomeObjectSuperBase* ins)
{
    MOZ_ASSERT(ins->homeObject()->type() == MIRType::Object);
    MOZ_ASSERT(ins->type() == MIRType::Object);

    auto lir = new(alloc()) LHomeObjectSuperBase(useRegister(ins->homeObject()));
    define(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitInterruptCheck(MInterruptCheck* ins)
{
    LInstruction* lir = new(alloc()) LInterruptCheck(temp());
    add(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitWasmTrap(MWasmTrap* ins)
{
    add(new(alloc()) LWasmTrap, ins);
}

void
LIRGenerator::visitWasmReinterpret(MWasmReinterpret* ins)
{
    if (ins->type() == MIRType::Int64)
        defineInt64(new(alloc()) LWasmReinterpretToI64(useRegisterAtStart(ins->input())), ins);
    else if (ins->input()->type() == MIRType::Int64)
        define(new(alloc()) LWasmReinterpretFromI64(useInt64RegisterAtStart(ins->input())), ins);
    else
        define(new(alloc()) LWasmReinterpret(useRegisterAtStart(ins->input())), ins);
}

void
LIRGenerator::visitStoreSlot(MStoreSlot* ins)
{
    LInstruction* lir;

    switch (ins->value()->type()) {
      case MIRType::Value:
        lir = new(alloc()) LStoreSlotV(useRegister(ins->slots()), useBox(ins->value()));
        add(lir, ins);
        break;

      case MIRType::Double:
        add(new(alloc()) LStoreSlotT(useRegister(ins->slots()), useRegister(ins->value())), ins);
        break;

      case MIRType::Float32:
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

    MIRType inputType = ins->getOperand(0)->type();
    MOZ_ASSERT(inputType == ins->type());

    // Handle typebarrier that will always bail.
    // (Emit LBail for visibility).
    if (ins->alwaysBails()) {
        LBail* bail = new(alloc()) LBail();
        assignSnapshot(bail, Bailout_Inevitable);
        add(bail, ins);
        redefine(ins, ins->input());
        return;
    }

    bool hasSpecificObjects = !types->unknownObject() && types->getObjectCount() > 0;

    // Handle typebarrier with Value as input.
    if (inputType == MIRType::Value) {
        LDefinition objTemp = hasSpecificObjects ? temp() : LDefinition::BogusTemp();
        if (ins->canRedefineInput()) {
            LTypeBarrierV* barrier =
                new(alloc()) LTypeBarrierV(useBox(ins->input()), tempToUnbox(), objTemp);
            assignSnapshot(barrier, Bailout_TypeBarrierV);
            add(barrier, ins);
            redefine(ins, ins->input());
        } else {
            LTypeBarrierV* barrier =
                new(alloc()) LTypeBarrierV(useBoxAtStart(ins->input()), tempToUnbox(), objTemp);
            assignSnapshot(barrier, Bailout_TypeBarrierV);
            defineBoxReuseInput(barrier, ins, 0);
        }
        return;
    }

    // The payload needs to be tested if it either might be null or might have
    // an object that should be excluded from the barrier.
    bool needsObjectBarrier = false;
    if (inputType == MIRType::ObjectOrNull)
        needsObjectBarrier = true;
    if (inputType == MIRType::Object && !types->hasType(TypeSet::AnyObjectType()) &&
        ins->barrierKind() != BarrierKind::TypeTagOnly)
    {
        needsObjectBarrier = true;
    }

    if (needsObjectBarrier) {
        LDefinition tmp = hasSpecificObjects ? temp() : LDefinition::BogusTemp();
        if (ins->canRedefineInput()) {
            LTypeBarrierO* barrier =
                new(alloc()) LTypeBarrierO(useRegister(ins->input()), tmp);
            assignSnapshot(barrier, Bailout_TypeBarrierO);
            add(barrier, ins);
            redefine(ins, ins->getOperand(0));
        } else {
            LTypeBarrierO* barrier =
                new(alloc()) LTypeBarrierO(useRegisterAtStart(ins->input()), tmp);
            assignSnapshot(barrier, Bailout_TypeBarrierO);
            defineReuseInput(barrier, ins, 0);
        }
        return;
    }

    // Handle remaining cases: No-op, unbox did everything.
    redefine(ins, ins->getOperand(0));
}

// Returns true iff |def| is a constant that's either not a GC thing or is not
// allocated in the nursery.
static bool
IsNonNurseryConstant(MDefinition* def)
{
    if (!def->isConstant())
        return false;
    Value v = def->toConstant()->toJSValue();
    return !v.isGCThing() || !IsInsideNursery(v.toGCThing());
}

void
LIRGenerator::visitPostWriteBarrier(MPostWriteBarrier* ins)
{
    MOZ_ASSERT(ins->object()->type() == MIRType::Object);

    // LPostWriteBarrier assumes that if it has a constant object then that
    // object is tenured, and does not need to be tested for being in the
    // nursery. Ensure that assumption holds by lowering constant nursery
    // objects to a register.
    bool useConstantObject = IsNonNurseryConstant(ins->object());

    switch (ins->value()->type()) {
      case MIRType::Object:
      case MIRType::ObjectOrNull: {
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
      case MIRType::String: {
          LDefinition tmp = needTempForPostBarrier() ? temp() : LDefinition::BogusTemp();
          LPostWriteBarrierS* lir =
            new(alloc()) LPostWriteBarrierS(useConstantObject
                                            ? useOrConstant(ins->object())
                                            : useRegister(ins->object()),
                                            useRegister(ins->value()), tmp);
        add(lir, ins);
        assignSafepoint(lir, ins);
        break;
      }
      case MIRType::Value: {
        LDefinition tmp = needTempForPostBarrier() ? temp() : LDefinition::BogusTemp();
        LPostWriteBarrierV* lir =
            new(alloc()) LPostWriteBarrierV(useConstantObject
                                            ? useOrConstant(ins->object())
                                            : useRegister(ins->object()),
                                            useBox(ins->value()),
                                            tmp);
        add(lir, ins);
        assignSafepoint(lir, ins);
        break;
      }
      default:
        // Currently, only objects and strings can be in the nursery. Other
        // instruction types cannot hold nursery pointers.
        break;
    }
}

void
LIRGenerator::visitPostWriteElementBarrier(MPostWriteElementBarrier* ins)
{
    MOZ_ASSERT(ins->object()->type() == MIRType::Object);
    MOZ_ASSERT(ins->index()->type() == MIRType::Int32);

    // LPostWriteElementBarrier assumes that if it has a constant object then that
    // object is tenured, and does not need to be tested for being in the
    // nursery. Ensure that assumption holds by lowering constant nursery
    // objects to a register.
    bool useConstantObject =
        ins->object()->isConstant() &&
        !IsInsideNursery(&ins->object()->toConstant()->toObject());

    switch (ins->value()->type()) {
      case MIRType::Object:
      case MIRType::ObjectOrNull: {
        LDefinition tmp = needTempForPostBarrier() ? temp() : LDefinition::BogusTemp();
        LPostWriteElementBarrierO* lir =
            new(alloc()) LPostWriteElementBarrierO(useConstantObject
                                                   ? useOrConstant(ins->object())
                                                   : useRegister(ins->object()),
                                                   useRegister(ins->value()),
                                                   useRegister(ins->index()),
                                                   tmp);
        add(lir, ins);
        assignSafepoint(lir, ins);
        break;
      }
      case MIRType::String: {
        LDefinition tmp = needTempForPostBarrier() ? temp() : LDefinition::BogusTemp();
        LPostWriteElementBarrierS* lir =
            new(alloc()) LPostWriteElementBarrierS(useConstantObject
                                                   ? useOrConstant(ins->object())
                                                   : useRegister(ins->object()),
                                                   useRegister(ins->value()),
                                                   useRegister(ins->index()),
                                                   tmp);
        add(lir, ins);
        assignSafepoint(lir, ins);
        break;
      }
      case MIRType::Value: {
        LDefinition tmp = needTempForPostBarrier() ? temp() : LDefinition::BogusTemp();
        LPostWriteElementBarrierV* lir =
            new(alloc()) LPostWriteElementBarrierV(useConstantObject
                                                   ? useOrConstant(ins->object())
                                                   : useRegister(ins->object()),
                                                   useRegister(ins->index()),
                                                   useBox(ins->value()),
                                                   tmp);
        add(lir, ins);
        assignSafepoint(lir, ins);
        break;
      }
      default:
        // Currently, only objects and strings can be in the nursery. Other
        // instruction types cannot hold nursery pointers.
        break;
    }
}

void
LIRGenerator::visitArrayLength(MArrayLength* ins)
{
    MOZ_ASSERT(ins->elements()->type() == MIRType::Elements);
    define(new(alloc()) LArrayLength(useRegisterAtStart(ins->elements())), ins);
}

void
LIRGenerator::visitSetArrayLength(MSetArrayLength* ins)
{
    MOZ_ASSERT(ins->elements()->type() == MIRType::Elements);
    MOZ_ASSERT(ins->index()->type() == MIRType::Int32);

    MOZ_ASSERT(ins->index()->isConstant());
    add(new(alloc()) LSetArrayLength(useRegister(ins->elements()),
                                     useRegisterOrConstant(ins->index())), ins);
}

void
LIRGenerator::visitGetNextEntryForIterator(MGetNextEntryForIterator* ins)
{
    MOZ_ASSERT(ins->iter()->type() == MIRType::Object);
    MOZ_ASSERT(ins->result()->type() == MIRType::Object);
    auto lir = new(alloc()) LGetNextEntryForIterator(useRegister(ins->iter()),
                                                     useRegister(ins->result()),
                                                     temp(), temp(), temp());
    define(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitTypedArrayLength(MTypedArrayLength* ins)
{
    MOZ_ASSERT(ins->object()->type() == MIRType::Object);
    define(new(alloc()) LTypedArrayLength(useRegisterAtStart(ins->object())), ins);
}

void
LIRGenerator::visitTypedArrayElements(MTypedArrayElements* ins)
{
    MOZ_ASSERT(ins->type() == MIRType::Elements);
    define(new(alloc()) LTypedArrayElements(useRegisterAtStart(ins->object())), ins);
}

void
LIRGenerator::visitSetDisjointTypedElements(MSetDisjointTypedElements* ins)
{
    MOZ_ASSERT(ins->type() == MIRType::None);

    MDefinition* target = ins->target();
    MOZ_ASSERT(target->type() == MIRType::Object);

    MDefinition* targetOffset = ins->targetOffset();
    MOZ_ASSERT(targetOffset->type() == MIRType::Int32);

    MDefinition* source = ins->source();
    MOZ_ASSERT(source->type() == MIRType::Object);

    auto lir = new(alloc()) LSetDisjointTypedElements(useRegister(target),
                                                      useRegister(targetOffset),
                                                      useRegister(source),
                                                      temp());
    add(lir, ins);
}

void
LIRGenerator::visitTypedObjectDescr(MTypedObjectDescr* ins)
{
    MOZ_ASSERT(ins->type() == MIRType::Object);
    define(new(alloc()) LTypedObjectDescr(useRegisterAtStart(ins->object())), ins);
}

void
LIRGenerator::visitTypedObjectElements(MTypedObjectElements* ins)
{
    MOZ_ASSERT(ins->type() == MIRType::Elements);
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
    MOZ_ASSERT(ins->elements()->type() == MIRType::Elements);
    define(new(alloc()) LInitializedLength(useRegisterAtStart(ins->elements())), ins);
}

void
LIRGenerator::visitSetInitializedLength(MSetInitializedLength* ins)
{
    MOZ_ASSERT(ins->elements()->type() == MIRType::Elements);
    MOZ_ASSERT(ins->index()->type() == MIRType::Int32);

    MOZ_ASSERT(ins->index()->isConstant());
    add(new(alloc()) LSetInitializedLength(useRegister(ins->elements()),
                                           useRegisterOrConstant(ins->index())), ins);
}

void
LIRGenerator::visitNot(MNot* ins)
{
    MDefinition* op = ins->input();

    // String is converted to length of string in the type analysis phase (see
    // TestPolicy).
    MOZ_ASSERT(op->type() != MIRType::String);

    // - boolean: x xor 1
    // - int32: LCompare(x, 0)
    // - double: LCompare(x, 0)
    // - null or undefined: true
    // - object: false if it never emulates undefined, else LNotO(x)
    switch (op->type()) {
      case MIRType::Boolean: {
        MConstant* cons = MConstant::New(alloc(), Int32Value(1));
        ins->block()->insertBefore(ins, cons);
        lowerForALU(new(alloc()) LBitOpI(JSOP_BITXOR), ins, op, cons);
        break;
      }
      case MIRType::Int32:
        define(new(alloc()) LNotI(useRegisterAtStart(op)), ins);
        break;
      case MIRType::Int64:
        define(new(alloc()) LNotI64(useInt64RegisterAtStart(op)), ins);
        break;
      case MIRType::Double:
        define(new(alloc()) LNotD(useRegister(op)), ins);
        break;
      case MIRType::Float32:
        define(new(alloc()) LNotF(useRegister(op)), ins);
        break;
      case MIRType::Undefined:
      case MIRType::Null:
        define(new(alloc()) LInteger(1), ins);
        break;
      case MIRType::Symbol:
        define(new(alloc()) LInteger(0), ins);
        break;
      case MIRType::Object:
        if (!ins->operandMightEmulateUndefined()) {
            // Objects that don't emulate undefined can be constant-folded.
            define(new(alloc()) LInteger(0), ins);
        } else {
            // All others require further work.
            define(new(alloc()) LNotO(useRegister(op)), ins);
        }
        break;
      case MIRType::Value: {
        LDefinition temp0, temp1;
        if (ins->operandMightEmulateUndefined()) {
            temp0 = temp();
            temp1 = temp();
        } else {
            temp0 = LDefinition::BogusTemp();
            temp1 = LDefinition::BogusTemp();
        }

        LNotV* lir = new(alloc()) LNotV(useBox(op), tempDouble(), temp0, temp1);
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
    MOZ_ASSERT(ins->index()->type() == MIRType::Int32);
    MOZ_ASSERT(ins->length()->type() == MIRType::Int32);
    MOZ_ASSERT(ins->type() == MIRType::Int32);

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
LIRGenerator::visitSpectreMaskIndex(MSpectreMaskIndex* ins)
{
    MOZ_ASSERT(ins->index()->type() == MIRType::Int32);
    MOZ_ASSERT(ins->length()->type() == MIRType::Int32);
    MOZ_ASSERT(ins->type() == MIRType::Int32);

    auto* lir = new(alloc()) LSpectreMaskIndex(useRegister(ins->index()), useAny(ins->length()));
    define(lir, ins);
}

void
LIRGenerator::visitBoundsCheckLower(MBoundsCheckLower* ins)
{
    MOZ_ASSERT(ins->index()->type() == MIRType::Int32);

    if (!ins->fallible())
        return;

    LInstruction* check = new(alloc()) LBoundsCheckLower(useRegister(ins->index()));
    assignSnapshot(check, Bailout_BoundsCheck);
    add(check, ins);
}

void
LIRGenerator::visitInArray(MInArray* ins)
{
    MOZ_ASSERT(ins->elements()->type() == MIRType::Elements);
    MOZ_ASSERT(ins->index()->type() == MIRType::Int32);
    MOZ_ASSERT(ins->initLength()->type() == MIRType::Int32);
    MOZ_ASSERT(ins->object()->type() == MIRType::Object);
    MOZ_ASSERT(ins->type() == MIRType::Boolean);

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
    MOZ_ASSERT(ins->index()->type() == MIRType::Int32);

    switch (ins->type()) {
      case MIRType::Value:
      {
        LLoadElementV* lir = new(alloc()) LLoadElementV(useRegister(ins->elements()),
                                                        useRegisterOrConstant(ins->index()));
        if (ins->fallible())
            assignSnapshot(lir, Bailout_Hole);
        defineBox(lir, ins);
        break;
      }
      case MIRType::Undefined:
      case MIRType::Null:
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
    MOZ_ASSERT(ins->elements()->type() == MIRType::Elements);
    MOZ_ASSERT(ins->index()->type() == MIRType::Int32);
    MOZ_ASSERT(ins->initLength()->type() == MIRType::Int32);
    MOZ_ASSERT(ins->type() == MIRType::Value);

    LLoadElementHole* lir = new(alloc()) LLoadElementHole(useRegister(ins->elements()),
                                                          useRegister(ins->index()),
                                                          useRegister(ins->initLength()));
    if (ins->needsNegativeIntCheck())
        assignSnapshot(lir, Bailout_NegativeIndex);
    defineBox(lir, ins);
}

void
LIRGenerator::visitLoadUnboxedObjectOrNull(MLoadUnboxedObjectOrNull* ins)
{
    MOZ_ASSERT(IsValidElementsType(ins->elements(), ins->offsetAdjustment()));
    MOZ_ASSERT(ins->index()->type() == MIRType::Int32);

    if (ins->type() == MIRType::Object || ins->type() == MIRType::ObjectOrNull) {
        LLoadUnboxedPointerT* lir = new(alloc()) LLoadUnboxedPointerT(useRegister(ins->elements()),
                                                                      useRegisterOrConstant(ins->index()));
        if (ins->nullBehavior() == MLoadUnboxedObjectOrNull::BailOnNull)
            assignSnapshot(lir, Bailout_TypeBarrierO);
        define(lir, ins);
    } else {
        MOZ_ASSERT(ins->type() == MIRType::Value);
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
    MOZ_ASSERT(ins->index()->type() == MIRType::Int32);
    MOZ_ASSERT(ins->type() == MIRType::String);

    LLoadUnboxedPointerT* lir = new(alloc()) LLoadUnboxedPointerT(useRegister(ins->elements()),
                                                                  useRegisterOrConstant(ins->index()));
    define(lir, ins);
}

void
LIRGenerator::visitLoadElementFromState(MLoadElementFromState* ins)
{
    MOZ_ASSERT(ins->index()->type() == MIRType::Int32);

    LDefinition temp1 = LDefinition::BogusTemp();
#ifdef JS_NUNBOX32
    temp1 = temp();
#endif
    MOZ_ASSERT(ins->array()->isArgumentState(),
               "LIRGenerator::visitLoadElementFromState: Unsupported state object");
    MArgumentState* array = ins->array()->toArgumentState();

    //   1                                 -- for the index as a register
    //   BOX_PIECES * array->numElements() -- for using as operand all the
    //                                        elements of the inlined array.
    size_t numOperands = 1 + BOX_PIECES * array->numElements();

    auto* lir = allocateVariadic<LLoadElementFromStateV>(numOperands, temp(), temp1, tempDouble());
    if (!lir) {
        abort(AbortReason::Alloc, "OOM: LIRGenerator::visitLoadElementFromState");
        return;
    }

    lir->setOperand(0, useRegister(ins->index())); // index

    for (size_t i = 0, e = array->numElements(); i < e; i++) {
        MDefinition* elem = array->getElement(i);
        if (elem->isConstant() && elem->isEmittedAtUses()) {
            lir->setOperand(1 + BOX_PIECES * i, LAllocation());
#ifdef JS_NUNBOX32
            lir->setOperand(1 + BOX_PIECES * i + 1, LAllocation());
#endif
            continue;
        }

        switch (array->getElement(i)->type()) {
          case MIRType::Value:
            lir->setBoxOperand(1 + BOX_PIECES * i, useBox(elem, LUse::ANY));
            break;
          // Anything which can be boxed:
          case MIRType::Boolean:
          case MIRType::Int32:
          case MIRType::Double:
          case MIRType::Object:
          case MIRType::String:
          case MIRType::Symbol:
            lir->setOperand(1 + BOX_PIECES * i, use(elem));
#ifdef JS_NUNBOX32
            // Bogus second operand.
            lir->setOperand(1 + BOX_PIECES * i + 1, LAllocation());
#endif
            break;
          case MIRType::Null:
          case MIRType::Undefined:
            // Bogus operand, as these can be inlined.
            lir->setOperand(1 + BOX_PIECES * i, LAllocation());
#ifdef JS_NUNBOX32
            lir->setOperand(1 + BOX_PIECES * i + 1, LAllocation());
#endif
            break;
          default:
            MOZ_CRASH("LIRGenerator::visitLoadElementFromState: Unsupported element type.");
            return;
        }
    }

    defineBox(lir, ins);
}

void
LIRGenerator::visitStoreElement(MStoreElement* ins)
{
    MOZ_ASSERT(IsValidElementsType(ins->elements(), ins->offsetAdjustment()));
    MOZ_ASSERT(ins->index()->type() == MIRType::Int32);

    const LUse elements = useRegister(ins->elements());
    const LAllocation index = useRegisterOrConstant(ins->index());

    switch (ins->value()->type()) {
      case MIRType::Value:
      {
        LInstruction* lir = new(alloc()) LStoreElementV(elements, index, useBox(ins->value()));
        if (ins->fallible())
            assignSnapshot(lir, Bailout_Hole);
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

static bool
BoundsCheckNeedsSpectreTemp()
{
    // On x86, spectreBoundsCheck32 can emit better code if it has a scratch
    // register and index masking is enabled.
#ifdef JS_CODEGEN_X86
    return JitOptions.spectreIndexMasking;
#else
    return false;
#endif
}

void
LIRGenerator::visitStoreElementHole(MStoreElementHole* ins)
{
    MOZ_ASSERT(ins->elements()->type() == MIRType::Elements);
    MOZ_ASSERT(ins->index()->type() == MIRType::Int32);

    const LUse object = useRegister(ins->object());
    const LUse elements = useRegister(ins->elements());
    const LAllocation index = useRegister(ins->index());

    LDefinition spectreTemp = BoundsCheckNeedsSpectreTemp() ? temp() : LDefinition::BogusTemp();

    LInstruction* lir;
    switch (ins->value()->type()) {
      case MIRType::Value:
        lir = new(alloc()) LStoreElementHoleV(object, elements, index, useBox(ins->value()),
                                              spectreTemp);
        break;

      default:
      {
        const LAllocation value = useRegisterOrNonDoubleConstant(ins->value());
        lir = new(alloc()) LStoreElementHoleT(object, elements, index, value, spectreTemp);
        break;
      }
    }

    add(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitFallibleStoreElement(MFallibleStoreElement* ins)
{
    MOZ_ASSERT(ins->elements()->type() == MIRType::Elements);
    MOZ_ASSERT(ins->index()->type() == MIRType::Int32);

    const LUse object = useRegister(ins->object());
    const LUse elements = useRegister(ins->elements());
    const LAllocation index = useRegister(ins->index());

    LDefinition spectreTemp = BoundsCheckNeedsSpectreTemp() ? temp() : LDefinition::BogusTemp();

    LInstruction* lir;
    switch (ins->value()->type()) {
      case MIRType::Value:
        lir = new(alloc()) LFallibleStoreElementV(object, elements, index, useBox(ins->value()),
                                                  spectreTemp);
        break;
      default:
        const LAllocation value = useRegisterOrNonDoubleConstant(ins->value());
        lir = new(alloc()) LFallibleStoreElementT(object, elements, index, value,
                                                  spectreTemp);
        break;
    }

    add(lir, ins);
    assignSafepoint(lir, ins);
}


void
LIRGenerator::visitStoreUnboxedObjectOrNull(MStoreUnboxedObjectOrNull* ins)
{
    MOZ_ASSERT(IsValidElementsType(ins->elements(), ins->offsetAdjustment()));
    MOZ_ASSERT(ins->index()->type() == MIRType::Int32);
    MOZ_ASSERT(ins->value()->type() == MIRType::Object ||
               ins->value()->type() == MIRType::Null ||
               ins->value()->type() == MIRType::ObjectOrNull);

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
    MOZ_ASSERT(ins->index()->type() == MIRType::Int32);
    MOZ_ASSERT(ins->value()->type() == MIRType::String);

    const LUse elements = useRegister(ins->elements());
    const LAllocation index = useRegisterOrConstant(ins->index());
    const LAllocation value = useRegisterOrNonDoubleConstant(ins->value());

    LInstruction* lir = new(alloc()) LStoreUnboxedPointer(elements, index, value);
    add(lir, ins);
}

void
LIRGenerator::visitConvertUnboxedObjectToNative(MConvertUnboxedObjectToNative* ins)
{
    MOZ_ASSERT(ins->object()->type() == MIRType::Object);

    if (JitOptions.spectreObjectMitigationsMisc) {
        auto* lir = new(alloc()) LConvertUnboxedObjectToNative(useRegisterAtStart(ins->object()),
                                                               temp());
        defineReuseInput(lir, ins, 0);
        assignSafepoint(lir, ins);
    } else {
        auto* lir = new(alloc()) LConvertUnboxedObjectToNative(useRegister(ins->object()),
                                                               LDefinition::BogusTemp());
        add(lir, ins);
        assignSafepoint(lir, ins);
    }
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
      case MIRType::Value:
      {
        LArrayPopShiftV* lir = new(alloc()) LArrayPopShiftV(object, temp(), temp());
        defineBox(lir, ins);
        assignSafepoint(lir, ins);
        break;
      }
      case MIRType::Undefined:
      case MIRType::Null:
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
    MOZ_ASSERT(ins->type() == MIRType::Int32);

    LUse object = useRegister(ins->object());

    LDefinition spectreTemp = BoundsCheckNeedsSpectreTemp() ? temp() : LDefinition::BogusTemp();

    switch (ins->value()->type()) {
      case MIRType::Value:
      {
        LArrayPushV* lir = new(alloc()) LArrayPushV(object, useBox(ins->value()), temp(),
                                                    spectreTemp);
        define(lir, ins);
        assignSafepoint(lir, ins);
        break;
      }

      default:
      {
        const LAllocation value = useRegisterOrNonDoubleConstant(ins->value());
        LArrayPushT* lir = new(alloc()) LArrayPushT(object, value, temp(), spectreTemp);
        define(lir, ins);
        assignSafepoint(lir, ins);
        break;
      }
    }
}

void
LIRGenerator::visitArraySlice(MArraySlice* ins)
{
    MOZ_ASSERT(ins->type() == MIRType::Object);
    MOZ_ASSERT(ins->object()->type() == MIRType::Object);
    MOZ_ASSERT(ins->begin()->type() == MIRType::Int32);
    MOZ_ASSERT(ins->end()->type() == MIRType::Int32);

    LArraySlice* lir = new(alloc()) LArraySlice(useFixedAtStart(ins->object(), CallTempReg0),
                                                useFixedAtStart(ins->begin(), CallTempReg1),
                                                useFixedAtStart(ins->end(), CallTempReg2),
                                                tempFixed(CallTempReg3),
                                                tempFixed(CallTempReg4));
    defineReturn(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitArrayJoin(MArrayJoin* ins)
{
    MOZ_ASSERT(ins->type() == MIRType::String);
    MOZ_ASSERT(ins->array()->type() == MIRType::Object);
    MOZ_ASSERT(ins->sep()->type() == MIRType::String);

    LDefinition tempDef = LDefinition::BogusTemp();
    if (ins->optimizeForArray())
        tempDef = temp();

    LArrayJoin* lir = new(alloc()) LArrayJoin(useRegisterAtStart(ins->array()),
                                              useRegisterAtStart(ins->sep()),
                                              tempDef);
    defineReturn(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitSinCos(MSinCos *ins)
{
    MOZ_ASSERT(ins->type() == MIRType::SinCosDouble);
    MOZ_ASSERT(ins->input()->type() == MIRType::Double  ||
               ins->input()->type() == MIRType::Float32 ||
               ins->input()->type() == MIRType::Int32);

    LSinCos *lir = new (alloc()) LSinCos(useRegisterAtStart(ins->input()),
                                         tempFixed(CallTempReg0),
                                         temp());
    defineSinCos(lir, ins);
}

void
LIRGenerator::visitStringSplit(MStringSplit* ins)
{
    MOZ_ASSERT(ins->type() == MIRType::Object);
    MOZ_ASSERT(ins->string()->type() == MIRType::String);
    MOZ_ASSERT(ins->separator()->type() == MIRType::String);

    LStringSplit* lir = new(alloc()) LStringSplit(useRegisterAtStart(ins->string()),
                                                  useRegisterAtStart(ins->separator()));
    defineReturn(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitLoadUnboxedScalar(MLoadUnboxedScalar* ins)
{
    MOZ_ASSERT(IsValidElementsType(ins->elements(), ins->offsetAdjustment()));
    MOZ_ASSERT(ins->index()->type() == MIRType::Int32);

    const LUse elements = useRegister(ins->elements());
    const LAllocation index = useRegisterOrConstant(ins->index());

    MOZ_ASSERT(IsNumberType(ins->type()) || IsSimdType(ins->type()) ||
               ins->type() == MIRType::Boolean);

    // We need a temp register for Uint32Array with known double result.
    LDefinition tempDef = LDefinition::BogusTemp();
    if (ins->readType() == Scalar::Uint32 && IsFloatingPointType(ins->type()))
        tempDef = temp();

    Synchronization sync = Synchronization::Load();
    if (ins->requiresMemoryBarrier()) {
        LMemoryBarrier* fence = new(alloc()) LMemoryBarrier(sync.barrierBefore);
        add(fence, ins);
    }
    LLoadUnboxedScalar* lir = new(alloc()) LLoadUnboxedScalar(elements, index, tempDef);
    if (ins->fallible())
        assignSnapshot(lir, Bailout_Overflow);
    define(lir, ins);
    if (ins->requiresMemoryBarrier()) {
        LMemoryBarrier* fence = new(alloc()) LMemoryBarrier(sync.barrierAfter);
        add(fence, ins);
    }
}

void
LIRGenerator::visitClampToUint8(MClampToUint8* ins)
{
    MDefinition* in = ins->input();

    switch (in->type()) {
      case MIRType::Boolean:
        redefine(ins, in);
        break;

      case MIRType::Int32:
        defineReuseInput(new(alloc()) LClampIToUint8(useRegisterAtStart(in)), ins, 0);
        break;

      case MIRType::Double:
        // LClampDToUint8 clobbers its input register. Making it available as
        // a temp copy describes this behavior to the register allocator.
        define(new(alloc()) LClampDToUint8(useRegisterAtStart(in), tempCopy(in, 0)), ins);
        break;

      case MIRType::Value:
      {
        LClampVToUint8* lir = new(alloc()) LClampVToUint8(useBox(in), tempDouble());
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
    MOZ_ASSERT(ins->object()->type() == MIRType::Object);
    MOZ_ASSERT(ins->index()->type() == MIRType::Int32);

    MOZ_ASSERT(ins->type() == MIRType::Value);

    const LUse object = useRegister(ins->object());
    const LAllocation index = useRegister(ins->index());

    LLoadTypedArrayElementHole* lir = new(alloc()) LLoadTypedArrayElementHole(object, index,
                                                                              temp());
    if (ins->fallible())
        assignSnapshot(lir, Bailout_Overflow);
    defineBox(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitStoreUnboxedScalar(MStoreUnboxedScalar* ins)
{
    MOZ_ASSERT(IsValidElementsType(ins->elements(), ins->offsetAdjustment()));
    MOZ_ASSERT(ins->index()->type() == MIRType::Int32);

    if (ins->isSimdWrite()) {
        MOZ_ASSERT_IF(ins->writeType() == Scalar::Float32x4, ins->value()->type() == MIRType::Float32x4);
        MOZ_ASSERT_IF(ins->writeType() == Scalar::Int8x16, ins->value()->type() == MIRType::Int8x16);
        MOZ_ASSERT_IF(ins->writeType() == Scalar::Int16x8, ins->value()->type() == MIRType::Int16x8);
        MOZ_ASSERT_IF(ins->writeType() == Scalar::Int32x4, ins->value()->type() == MIRType::Int32x4);
    } else if (ins->isFloatWrite()) {
        MOZ_ASSERT_IF(ins->writeType() == Scalar::Float32, ins->value()->type() == MIRType::Float32);
        MOZ_ASSERT_IF(ins->writeType() == Scalar::Float64, ins->value()->type() == MIRType::Double);
    } else {
        MOZ_ASSERT(ins->value()->type() == MIRType::Int32);
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
    Synchronization sync = Synchronization::Store();
    if (ins->requiresMemoryBarrier()) {
        LMemoryBarrier* fence = new(alloc()) LMemoryBarrier(sync.barrierBefore);
        add(fence, ins);
    }
    add(new(alloc()) LStoreUnboxedScalar(elements, index, value), ins);
    if (ins->requiresMemoryBarrier()) {
        LMemoryBarrier* fence = new(alloc()) LMemoryBarrier(sync.barrierAfter);
        add(fence, ins);
    }
}

void
LIRGenerator::visitStoreTypedArrayElementHole(MStoreTypedArrayElementHole* ins)
{
    MOZ_ASSERT(ins->elements()->type() == MIRType::Elements);
    MOZ_ASSERT(ins->index()->type() == MIRType::Int32);
    MOZ_ASSERT(ins->length()->type() == MIRType::Int32);

    if (ins->isFloatWrite()) {
        MOZ_ASSERT_IF(ins->arrayType() == Scalar::Float32, ins->value()->type() == MIRType::Float32);
        MOZ_ASSERT_IF(ins->arrayType() == Scalar::Float64, ins->value()->type() == MIRType::Double);
    } else {
        MOZ_ASSERT(ins->value()->type() == MIRType::Int32);
    }

    LUse elements = useRegister(ins->elements());
    LAllocation length = useAny(ins->length());
    LAllocation index = useRegister(ins->index());
    // For byte arrays, the value has to be in a byte register on x86.
    LAllocation value;
    if (ins->isByteWrite())
        value = useByteOpRegisterOrNonDoubleConstant(ins->value());
    else
        value = useRegisterOrNonDoubleConstant(ins->value());

    LDefinition spectreTemp = BoundsCheckNeedsSpectreTemp() ? temp() : LDefinition::BogusTemp();
    auto* lir =
        new(alloc()) LStoreTypedArrayElementHole(elements, length, index, value, spectreTemp);
    add(lir, ins);
}

void
LIRGenerator::visitLoadFixedSlot(MLoadFixedSlot* ins)
{
    MDefinition* obj = ins->object();
    MOZ_ASSERT(obj->type() == MIRType::Object);

    MIRType type = ins->type();

    if (type == MIRType::Value) {
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
    MOZ_ASSERT(obj->type() == MIRType::Object);

    LLoadFixedSlotAndUnbox* lir = new(alloc()) LLoadFixedSlotAndUnbox(useRegisterAtStart(obj));
    if (ins->fallible())
        assignSnapshot(lir, ins->bailoutKind());

    define(lir, ins);
}

void
LIRGenerator::visitStoreFixedSlot(MStoreFixedSlot* ins)
{
    MOZ_ASSERT(ins->object()->type() == MIRType::Object);

    if (ins->value()->type() == MIRType::Value) {
        LStoreFixedSlotV* lir = new(alloc()) LStoreFixedSlotV(useRegister(ins->object()),
                                                              useBox(ins->value()));
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
    MOZ_ASSERT(ins->envObj()->type() == MIRType::Object);

    // Emit an overrecursed check: this is necessary because the cache can
    // attach a scripted getter stub that calls this script recursively.
    gen->setNeedsOverrecursedCheck();

    LGetNameCache* lir = new(alloc()) LGetNameCache(useRegister(ins->envObj()), temp());
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
LIRGenerator::visitGetPropSuperCache(MGetPropSuperCache* ins)
{
    MDefinition* obj = ins->object();
    MDefinition* receiver = ins->receiver();
    MDefinition* id = ins->idval();

    gen->setNeedsOverrecursedCheck();

    bool useConstId = id->type() == MIRType::String || id->type() == MIRType::Symbol;

    auto* lir = new(alloc()) LGetPropSuperCacheV(useRegister(obj),
                                                 useBoxOrTyped(receiver),
                                                 useBoxOrTypedOrConstant(id, useConstId));
    defineBox(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitGetPropertyCache(MGetPropertyCache* ins)
{
    MDefinition* value = ins->value();
    MOZ_ASSERT(value->type() == MIRType::Object || value->type() == MIRType::Value);

    MDefinition* id = ins->idval();
    MOZ_ASSERT(id->type() == MIRType::String ||
               id->type() == MIRType::Symbol ||
               id->type() == MIRType::Int32 ||
               id->type() == MIRType::Value);

    if (ins->monitoredResult()) {
        // Emit an overrecursed check: this is necessary because the cache can
        // attach a scripted getter stub that calls this script recursively.
        gen->setNeedsOverrecursedCheck();
    }

    // If this is a GETPROP, the id is a constant string. Allow passing it as a
    // constant to reduce register allocation pressure.
    bool useConstId = id->type() == MIRType::String || id->type() == MIRType::Symbol;

    // We need a temp register if we can't use the output register as scratch.
    // See IonIC::scratchRegisterForEntryJump.
    LDefinition maybeTemp = LDefinition::BogusTemp();
    if (ins->type() == MIRType::Double)
        maybeTemp = temp();

    if (ins->type() == MIRType::Value) {
        LGetPropertyCacheV* lir =
            new(alloc()) LGetPropertyCacheV(useBoxOrTyped(value),
                                            useBoxOrTypedOrConstant(id, useConstId),
                                            maybeTemp);
        defineBox(lir, ins);
        assignSafepoint(lir, ins);
    } else {
        LGetPropertyCacheT* lir =
            new(alloc()) LGetPropertyCacheT(useBoxOrTyped(value),
                                            useBoxOrTypedOrConstant(id, useConstId),
                                            maybeTemp);
        define(lir, ins);
        assignSafepoint(lir, ins);
    }
}

void
LIRGenerator::visitGetPropertyPolymorphic(MGetPropertyPolymorphic* ins)
{
    MOZ_ASSERT(ins->object()->type() == MIRType::Object);

    if (ins->type() == MIRType::Value) {
        LGetPropertyPolymorphicV* lir =
            new(alloc()) LGetPropertyPolymorphicV(useRegister(ins->object()), temp());
        assignSnapshot(lir, Bailout_ShapeGuard);
        defineBox(lir, ins);
    } else {
        LDefinition maybeTemp2 =
            (ins->type() == MIRType::Double) ? temp() : LDefinition::BogusTemp();
        LGetPropertyPolymorphicT* lir =
            new(alloc()) LGetPropertyPolymorphicT(useRegister(ins->object()), temp(), maybeTemp2);
        assignSnapshot(lir, Bailout_ShapeGuard);
        define(lir, ins);
    }
}

void
LIRGenerator::visitSetPropertyPolymorphic(MSetPropertyPolymorphic* ins)
{
    MOZ_ASSERT(ins->object()->type() == MIRType::Object);

    if (ins->value()->type() == MIRType::Value) {
        LSetPropertyPolymorphicV* lir =
            new(alloc()) LSetPropertyPolymorphicV(useRegister(ins->object()),
                                                  useBox(ins->value()),
                                                  temp(), temp());
        assignSnapshot(lir, Bailout_ShapeGuard);
        add(lir, ins);
    } else {
        LAllocation value = useRegisterOrConstant(ins->value());
        LSetPropertyPolymorphicT* lir =
            new(alloc()) LSetPropertyPolymorphicT(useRegister(ins->object()), value,
                                                  ins->value()->type(), temp(), temp());
        assignSnapshot(lir, Bailout_ShapeGuard);
        add(lir, ins);
    }
}

void
LIRGenerator::visitBindNameCache(MBindNameCache* ins)
{
    MOZ_ASSERT(ins->environmentChain()->type() == MIRType::Object);
    MOZ_ASSERT(ins->type() == MIRType::Object);

    LBindNameCache* lir = new(alloc()) LBindNameCache(useRegister(ins->environmentChain()), temp());
    define(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitCallBindVar(MCallBindVar* ins)
{
    MOZ_ASSERT(ins->environmentChain()->type() == MIRType::Object);
    MOZ_ASSERT(ins->type() == MIRType::Object);

    LCallBindVar* lir = new(alloc()) LCallBindVar(useRegister(ins->environmentChain()));
    define(lir, ins);
}

void
LIRGenerator::visitGuardObjectIdentity(MGuardObjectIdentity* ins)
{
    LGuardObjectIdentity* guard = new(alloc()) LGuardObjectIdentity(useRegister(ins->object()),
                                                                    useRegister(ins->expected()));
    assignSnapshot(guard, Bailout_ObjectIdentityOrTypeGuard);
    add(guard, ins);
    redefine(ins, ins->object());
}

void
LIRGenerator::visitGuardShape(MGuardShape* ins)
{
    MOZ_ASSERT(ins->object()->type() == MIRType::Object);

    if (JitOptions.spectreObjectMitigationsMisc) {
        auto* lir = new(alloc()) LGuardShape(useRegisterAtStart(ins->object()), temp());
        assignSnapshot(lir, ins->bailoutKind());
        defineReuseInput(lir, ins, 0);
    } else {
        auto* lir = new(alloc()) LGuardShape(useRegister(ins->object()),
                                             LDefinition::BogusTemp());
        assignSnapshot(lir, ins->bailoutKind());
        add(lir, ins);
        redefine(ins, ins->object());
    }
}

void
LIRGenerator::visitGuardObjectGroup(MGuardObjectGroup* ins)
{
    MOZ_ASSERT(ins->object()->type() == MIRType::Object);

    if (JitOptions.spectreObjectMitigationsMisc) {
        auto* lir = new(alloc()) LGuardObjectGroup(useRegisterAtStart(ins->object()), temp());
        assignSnapshot(lir, ins->bailoutKind());
        defineReuseInput(lir, ins, 0);
    } else {
        auto* lir = new(alloc()) LGuardObjectGroup(useRegister(ins->object()),
                                                   LDefinition::BogusTemp());
        assignSnapshot(lir, ins->bailoutKind());
        add(lir, ins);
        redefine(ins, ins->object());
    }
}

void
LIRGenerator::visitGuardObject(MGuardObject* ins)
{
    // The type policy does all the work, so at this point the input
    // is guaranteed to be an object.
    MOZ_ASSERT(ins->input()->type() == MIRType::Object);
    redefine(ins, ins->input());
}

void
LIRGenerator::visitGuardString(MGuardString* ins)
{
    // The type policy does all the work, so at this point the input
    // is guaranteed to be a string.
    MOZ_ASSERT(ins->input()->type() == MIRType::String);
    redefine(ins, ins->input());
}

void
LIRGenerator::visitGuardSharedTypedArray(MGuardSharedTypedArray* ins)
{
    MOZ_ASSERT(ins->input()->type() == MIRType::Object);
    LGuardSharedTypedArray* guard =
        new(alloc()) LGuardSharedTypedArray(useRegister(ins->object()), temp());
    assignSnapshot(guard, Bailout_NonSharedTypedArrayInput);
    add(guard, ins);
}

void
LIRGenerator::visitPolyInlineGuard(MPolyInlineGuard* ins)
{
    MOZ_ASSERT(ins->input()->type() == MIRType::Object);
    redefine(ins, ins->input());
}

void
LIRGenerator::visitGuardReceiverPolymorphic(MGuardReceiverPolymorphic* ins)
{
    MOZ_ASSERT(ins->object()->type() == MIRType::Object);
    MOZ_ASSERT(ins->type() == MIRType::Object);

    if (JitOptions.spectreObjectMitigationsMisc) {
        auto* lir = new(alloc()) LGuardReceiverPolymorphic(useRegisterAtStart(ins->object()),
                                                           temp(), temp());
        assignSnapshot(lir, Bailout_ShapeGuard);
        defineReuseInput(lir, ins, 0);
    } else {
        auto* lir = new(alloc()) LGuardReceiverPolymorphic(useRegister(ins->object()),
                                                           temp(), temp());
        assignSnapshot(lir, Bailout_ShapeGuard);
        add(lir, ins);
        redefine(ins, ins->object());
    }
}

void
LIRGenerator::visitGuardUnboxedExpando(MGuardUnboxedExpando* ins)
{
    LGuardUnboxedExpando* guard =
        new(alloc()) LGuardUnboxedExpando(useRegister(ins->object()));
    assignSnapshot(guard, ins->bailoutKind());
    add(guard, ins);
    redefine(ins, ins->object());
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
      case MIRType::Boolean:
      case MIRType::Int32:
        lir = new(alloc()) LAssertRangeI(useRegisterAtStart(input));
        break;

      case MIRType::Double:
        lir = new(alloc()) LAssertRangeD(useRegister(input), tempDouble());
        break;

      case MIRType::Float32:
        lir = new(alloc()) LAssertRangeF(useRegister(input), tempDouble(), tempDouble());
        break;

      case MIRType::Value:
        lir = new(alloc()) LAssertRangeV(useBox(input), tempToUnbox(), tempDouble(), tempDouble());
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
    LCallGetProperty* lir = new(alloc()) LCallGetProperty(useBoxAtStart(ins->value()));
    defineReturn(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitCallGetElement(MCallGetElement* ins)
{
    MOZ_ASSERT(ins->lhs()->type() == MIRType::Value);
    MOZ_ASSERT(ins->rhs()->type() == MIRType::Value);

    LCallGetElement* lir = new(alloc()) LCallGetElement(useBoxAtStart(ins->lhs()),
                                                        useBoxAtStart(ins->rhs()));
    defineReturn(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitCallSetProperty(MCallSetProperty* ins)
{
    LInstruction* lir = new(alloc()) LCallSetProperty(useRegisterAtStart(ins->object()),
                                                      useBoxAtStart(ins->value()));
    add(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitDeleteProperty(MDeleteProperty* ins)
{
    LCallDeleteProperty* lir = new(alloc()) LCallDeleteProperty(useBoxAtStart(ins->value()));
    defineReturn(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitDeleteElement(MDeleteElement* ins)
{
    LCallDeleteElement* lir = new(alloc()) LCallDeleteElement(useBoxAtStart(ins->value()),
                                                              useBoxAtStart(ins->index()));
    defineReturn(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitSetPropertyCache(MSetPropertyCache* ins)
{
    MOZ_ASSERT(ins->object()->type() == MIRType::Object);

    MDefinition* id = ins->idval();
    MOZ_ASSERT(id->type() == MIRType::String ||
               id->type() == MIRType::Symbol ||
               id->type() == MIRType::Int32 ||
               id->type() == MIRType::Value);

    // If this is a SETPROP, the id is a constant string. Allow passing it as a
    // constant to reduce register allocation pressure.
    bool useConstId = id->type() == MIRType::String || id->type() == MIRType::Symbol;
    bool useConstValue = IsNonNurseryConstant(ins->value());

    // Emit an overrecursed check: this is necessary because the cache can
    // attach a scripted setter stub that calls this script recursively.
    gen->setNeedsOverrecursedCheck();

    // We need a double/float32 temp register for typed array stubs if this is
    // a SETELEM or INITELEM op.
    LDefinition tempD = LDefinition::BogusTemp();
    LDefinition tempF32 = LDefinition::BogusTemp();
    if (IsElemPC(ins->resumePoint()->pc())) {
        tempD = tempDouble();
        tempF32 = hasUnaliasedDouble() ? tempFloat32() : LDefinition::BogusTemp();
    }

    LInstruction* lir =
        new(alloc()) LSetPropertyCache(useRegister(ins->object()),
                                       useBoxOrTypedOrConstant(id, useConstId),
                                       useBoxOrTypedOrConstant(ins->value(), useConstValue),
                                       temp(), tempD, tempF32);
    add(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitCallSetElement(MCallSetElement* ins)
{
    MOZ_ASSERT(ins->object()->type() == MIRType::Object);
    MOZ_ASSERT(ins->index()->type() == MIRType::Value);
    MOZ_ASSERT(ins->value()->type() == MIRType::Value);

    LCallSetElement* lir = new(alloc()) LCallSetElement(useRegisterAtStart(ins->object()),
                                                        useBoxAtStart(ins->index()),
                                                        useBoxAtStart(ins->value()));
    add(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitCallInitElementArray(MCallInitElementArray* ins)
{
    LCallInitElementArray* lir =
        new(alloc()) LCallInitElementArray(useRegisterAtStart(ins->object()),
                                           useRegisterOrConstantAtStart(ins->index()),
                                           useBoxAtStart(ins->value()));
    add(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitGetIteratorCache(MGetIteratorCache* ins)
{
    MDefinition* value = ins->value();
    MOZ_ASSERT(value->type() == MIRType::Object || value->type() == MIRType::Value);

    LGetIteratorCache* lir = new(alloc()) LGetIteratorCache(useBoxOrTyped(value), temp(), temp());
    define(lir, ins);
    assignSafepoint(lir, ins);
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
    MOZ_ASSERT(ins->string()->type() == MIRType::String);
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

    if (input->type() == MIRType::Value) {
        LSetFrameArgumentV* lir = new(alloc()) LSetFrameArgumentV(useBox(input));
        add(lir, ins);
    } else if (input->type() == MIRType::Undefined || input->type() == MIRType::Null) {
        Value val = input->type() == MIRType::Undefined ? UndefinedValue() : NullValue();
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
    MOZ_ASSERT(ins->numActuals()->type() == MIRType::Int32);

    LRest* lir = new(alloc()) LRest(useFixedAtStart(ins->numActuals(), CallTempReg0),
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
    MOZ_ASSERT(value->type() == MIRType::Value);

    LThrow* lir = new(alloc()) LThrow(useBoxAtStart(value));
    add(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitInCache(MInCache* ins)
{
    MDefinition* lhs = ins->lhs();
    MDefinition* rhs = ins->rhs();

    MOZ_ASSERT(lhs->type() == MIRType::String ||
               lhs->type() == MIRType::Symbol ||
               lhs->type() == MIRType::Int32 ||
               lhs->type() == MIRType::Value);
    MOZ_ASSERT(rhs->type() == MIRType::Object);

    LInCache* lir = new(alloc()) LInCache(useBoxOrTyped(lhs), useRegister(rhs), temp());
    define(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitHasOwnCache(MHasOwnCache* ins)
{
    MDefinition* value = ins->value();
    MOZ_ASSERT(value->type() == MIRType::Object || value->type() == MIRType::Value);

    MDefinition* id = ins->idval();
    MOZ_ASSERT(id->type() == MIRType::String ||
               id->type() == MIRType::Symbol ||
               id->type() == MIRType::Int32 ||
               id->type() == MIRType::Value);

    // Emit an overrecursed check: this is necessary because the cache can
    // attach a scripted getter stub that calls this script recursively.
    gen->setNeedsOverrecursedCheck();

    LHasOwnCache* lir = new(alloc()) LHasOwnCache(useBoxOrTyped(value), useBoxOrTyped(id));
    define(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitInstanceOf(MInstanceOf* ins)
{
    MDefinition* lhs = ins->getOperand(0);

    MOZ_ASSERT(lhs->type() == MIRType::Value || lhs->type() == MIRType::Object);

    if (lhs->type() == MIRType::Object) {
        LInstanceOfO* lir = new(alloc()) LInstanceOfO(useRegister(lhs));
        define(lir, ins);
        assignSafepoint(lir, ins);
    } else {
        LInstanceOfV* lir = new(alloc()) LInstanceOfV(useBox(lhs));
        define(lir, ins);
        assignSafepoint(lir, ins);
    }
}

void
LIRGenerator::visitInstanceOfCache(MInstanceOfCache* ins)
{
    MDefinition* lhs = ins->lhs();
    MDefinition* rhs = ins->rhs();

    MOZ_ASSERT(lhs->type() == MIRType::Value);
    MOZ_ASSERT(rhs->type() == MIRType::Object);

    LInstanceOfCache* lir = new(alloc()) LInstanceOfCache(useBox(lhs),
                                                          useRegister(rhs));
    define(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitIsArray(MIsArray* ins)
{
    MOZ_ASSERT(ins->type() == MIRType::Boolean);

    if (ins->value()->type() == MIRType::Object) {
        LIsArrayO* lir = new(alloc()) LIsArrayO(useRegister(ins->value()));
        define(lir, ins);
        assignSafepoint(lir, ins);
    } else {
        MOZ_ASSERT(ins->value()->type() == MIRType::Value);
        LIsArrayV* lir = new(alloc()) LIsArrayV(useBox(ins->value()), temp());
        define(lir, ins);
        assignSafepoint(lir, ins);
    }
}

void
LIRGenerator::visitIsTypedArray(MIsTypedArray* ins)
{
    MOZ_ASSERT(ins->value()->type() == MIRType::Object);
    MOZ_ASSERT(ins->type() == MIRType::Boolean);

    auto* lir = new(alloc()) LIsTypedArray(useRegister(ins->value()));
    define(lir, ins);
}

void
LIRGenerator::visitIsCallable(MIsCallable* ins)
{
    MOZ_ASSERT(ins->type() == MIRType::Boolean);

    if (ins->object()->type() == MIRType::Object) {
        define(new(alloc()) LIsCallableO(useRegister(ins->object())), ins);
    } else {
        MOZ_ASSERT(ins->object()->type() == MIRType::Value);
        define(new(alloc()) LIsCallableV(useBox(ins->object()), temp()), ins);
    }
}

void
LIRGenerator::visitIsConstructor(MIsConstructor* ins)
{
    MOZ_ASSERT(ins->object()->type() == MIRType::Object);
    MOZ_ASSERT(ins->type() == MIRType::Boolean);
    define(new(alloc()) LIsConstructor(useRegister(ins->object())), ins);
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
    MOZ_ASSERT(opd->type() == MIRType::Value);
    LIsObject* lir = new(alloc()) LIsObject(useBoxAtStart(opd));
    define(lir, ins);
}

void
LIRGenerator::visitHasClass(MHasClass* ins)
{
    MOZ_ASSERT(ins->object()->type() == MIRType::Object);
    MOZ_ASSERT(ins->type() == MIRType::Boolean);
    define(new(alloc()) LHasClass(useRegister(ins->object())), ins);
}

void
LIRGenerator::visitGuardToClass(MGuardToClass* ins)
{
    MOZ_ASSERT(ins->object()->type() == MIRType::Object);
    MOZ_ASSERT(ins->type() == MIRType::ObjectOrNull|| ins->type() == MIRType::Object);
    LGuardToClass* lir = new(alloc()) LGuardToClass(useRegister(ins->object()), temp());
    assignSnapshot(lir, Bailout_TypeBarrierO);
    define(lir, ins);
}

void
LIRGenerator::visitObjectClassToString(MObjectClassToString* ins)
{
    MOZ_ASSERT(ins->object()->type() == MIRType::Object);
    MOZ_ASSERT(ins->type() == MIRType::String);
    auto lir = new(alloc()) LObjectClassToString(useRegisterAtStart(ins->object()));
    defineReturn(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitWasmAddOffset(MWasmAddOffset* ins)
{
    MOZ_ASSERT(ins->base()->type() == MIRType::Int32);
    MOZ_ASSERT(ins->type() == MIRType::Int32);
    MOZ_ASSERT(ins->offset());
    define(new(alloc()) LWasmAddOffset(useRegisterAtStart(ins->base())), ins);
}

void
LIRGenerator::visitWasmLoadTls(MWasmLoadTls* ins)
{
#ifdef WASM_HUGE_MEMORY
    // This will disappear once we remove HeapReg and replace it with a load
    // from Tls, but in the mean time it keeps us sane.
    MOZ_CRASH("No WasmLoadTls here at the moment");
#endif
    auto* lir = new(alloc()) LWasmLoadTls(useRegisterAtStart(ins->tlsPtr()));
    define(lir, ins);
}

void
LIRGenerator::visitWasmBoundsCheck(MWasmBoundsCheck* ins)
{
#ifdef WASM_HUGE_MEMORY
    MOZ_CRASH("No bounds checking on huge memory");
#else
    MOZ_ASSERT(!ins->isRedundant());

    MDefinition* index = ins->index();
    MOZ_ASSERT(index->type() == MIRType::Int32);

    MDefinition* boundsCheckLimit = ins->boundsCheckLimit();
    MOZ_ASSERT(boundsCheckLimit->type() == MIRType::Int32);


    if (JitOptions.spectreIndexMasking) {
        auto* lir = new(alloc()) LWasmBoundsCheck(useRegisterAtStart(index),
                                                  useRegister(boundsCheckLimit));
        defineReuseInput(lir, ins, 0);
    } else {
        auto* lir = new(alloc()) LWasmBoundsCheck(useRegisterAtStart(index),
                                                  useRegisterAtStart(boundsCheckLimit));
        add(lir, ins);
    }
#endif
}

void
LIRGenerator::visitWasmAlignmentCheck(MWasmAlignmentCheck* ins)
{
    MDefinition* index = ins->index();
    MOZ_ASSERT(index->type() == MIRType::Int32);

    auto* lir = new(alloc()) LWasmAlignmentCheck(useRegisterAtStart(index));
    add(lir, ins);
}

void
LIRGenerator::visitWasmLoadGlobalVar(MWasmLoadGlobalVar* ins)
{
    if (ins->type() == MIRType::Int64) {
#ifdef JS_PUNBOX64
        LAllocation tlsPtr = useRegisterAtStart(ins->tlsPtr());
#else
        LAllocation tlsPtr = useRegister(ins->tlsPtr());
#endif
        defineInt64(new(alloc()) LWasmLoadGlobalVarI64(tlsPtr), ins);
    } else {
        LAllocation tlsPtr = useRegisterAtStart(ins->tlsPtr());
        define(new(alloc()) LWasmLoadGlobalVar(tlsPtr), ins);
    }
}

void
LIRGenerator::visitWasmStoreGlobalVar(MWasmStoreGlobalVar* ins)
{
    MDefinition* value = ins->value();
    if (value->type() == MIRType::Int64) {
#ifdef JS_PUNBOX64
        LAllocation tlsPtr = useRegisterAtStart(ins->tlsPtr());
        LInt64Allocation valueAlloc = useInt64RegisterAtStart(value);
#else
        LAllocation tlsPtr = useRegister(ins->tlsPtr());
        LInt64Allocation valueAlloc = useInt64Register(value);
#endif
        add(new(alloc()) LWasmStoreGlobalVarI64(valueAlloc, tlsPtr), ins);
    } else {
        LAllocation tlsPtr = useRegisterAtStart(ins->tlsPtr());
        LAllocation valueAlloc = useRegisterAtStart(value);
        add(new(alloc()) LWasmStoreGlobalVar(valueAlloc, tlsPtr), ins);
    }
}

void
LIRGenerator::visitWasmParameter(MWasmParameter* ins)
{
    ABIArg abi = ins->abi();
    if (abi.argInRegister()) {
#if defined(JS_NUNBOX32)
        if (abi.isGeneralRegPair()) {
            defineInt64Fixed(new(alloc()) LWasmParameterI64, ins,
                LInt64Allocation(LAllocation(AnyRegister(abi.gpr64().high)),
                                 LAllocation(AnyRegister(abi.gpr64().low))));
            return;
        }
#endif
        defineFixed(new(alloc()) LWasmParameter, ins, LAllocation(abi.reg()));
        return;
    }
    if (ins->type() == MIRType::Int64) {
        MOZ_ASSERT(!abi.argInRegister());
        defineInt64Fixed(new(alloc()) LWasmParameterI64, ins,
#if defined(JS_NUNBOX32)
            LInt64Allocation(LArgument(abi.offsetFromArgBase() + INT64HIGH_OFFSET),
                             LArgument(abi.offsetFromArgBase() + INT64LOW_OFFSET))
#else
            LInt64Allocation(LArgument(abi.offsetFromArgBase()))
#endif
        );
    } else {
        MOZ_ASSERT(IsNumberType(ins->type()) || IsSimdType(ins->type()));
        defineFixed(new(alloc()) LWasmParameter, ins, LArgument(abi.offsetFromArgBase()));
    }
}

void
LIRGenerator::visitWasmReturn(MWasmReturn* ins)
{
    MDefinition* rval = ins->getOperand(0);

    if (rval->type() == MIRType::Int64) {
        add(new(alloc()) LWasmReturnI64(useInt64Fixed(rval, ReturnReg64)));
        return;
    }

    LWasmReturn* lir = new(alloc()) LWasmReturn;
    if (rval->type() == MIRType::Float32)
        lir->setOperand(0, useFixed(rval, ReturnFloat32Reg));
    else if (rval->type() == MIRType::Double)
        lir->setOperand(0, useFixed(rval, ReturnDoubleReg));
    else if (IsSimdType(rval->type()))
        lir->setOperand(0, useFixed(rval, ReturnSimd128Reg));
    else if (rval->type() == MIRType::Int32)
        lir->setOperand(0, useFixed(rval, ReturnReg));
    else
        MOZ_CRASH("Unexpected wasm return type");

    add(lir);
}

void
LIRGenerator::visitWasmReturnVoid(MWasmReturnVoid* ins)
{
    add(new(alloc()) LWasmReturnVoid);
}

void
LIRGenerator::visitWasmStackArg(MWasmStackArg* ins)
{
    if (ins->arg()->type() == MIRType::Int64) {
        add(new(alloc()) LWasmStackArgI64(useInt64RegisterOrConstantAtStart(ins->arg())), ins);
    } else if (IsFloatingPointType(ins->arg()->type()) || IsSimdType(ins->arg()->type())) {
        MOZ_ASSERT(!ins->arg()->isEmittedAtUses());
        add(new(alloc()) LWasmStackArg(useRegisterAtStart(ins->arg())), ins);
    } else {
        add(new(alloc()) LWasmStackArg(useRegisterOrConstantAtStart(ins->arg())), ins);
    }
}

template <typename LClass>
LInstruction*
LIRGenerator::lowerWasmCall(MWasmCall* ins, bool needsBoundsCheck)
{
    auto* lir = allocateVariadic<LClass>(ins->numOperands(), needsBoundsCheck);
    if (!lir) {
        abort(AbortReason::Alloc, "Couldn't allocate for MWasmCall");
        return nullptr;
    }

    for (unsigned i = 0; i < ins->numArgs(); i++)
        lir->setOperand(i, useFixedAtStart(ins->getOperand(i), ins->registerForArg(i)));

    if (ins->callee().isTable()) {
        MDefinition* index = ins->getOperand(ins->numArgs());
        lir->setOperand(ins->numArgs(), useFixedAtStart(index, WasmTableCallIndexReg));
    }

    return lir;
}

void
LIRGenerator::visitWasmCall(MWasmCall* ins)
{
    bool needsBoundsCheck = true;
    if (ins->callee().isTable()) {
        MDefinition* index = ins->getOperand(ins->numArgs());

        if (ins->callee().which() == wasm::CalleeDesc::WasmTable && index->isConstant()) {
            if (uint32_t(index->toConstant()->toInt32()) < ins->callee().wasmTableMinLength())
                needsBoundsCheck = false;
        }
    }

    LInstruction* lir;
    if (ins->type() == MIRType::Int64)
        lir = lowerWasmCall<LWasmCallI64>(ins, needsBoundsCheck);
    else if (ins->type() == MIRType::None)
        lir = lowerWasmCall<LWasmCallVoid>(ins, needsBoundsCheck);
    else
        lir = lowerWasmCall<LWasmCall>(ins, needsBoundsCheck);
    if (!lir)
        return;

    if (ins->type() == MIRType::None)
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

    // Keep using GetTempRegForIntArg, since we want to make sure we
    // don't clobber registers we're already using.
    Register tempReg1, tempReg2;
    GetTempRegForIntArg(4, 0, &tempReg1);
    mozilla::DebugOnly<bool> ok = GetTempRegForIntArg(5, 0, &tempReg2);
    MOZ_ASSERT(ok, "How can we not have six temp registers?");

    LSetDOMProperty* lir = new(alloc()) LSetDOMProperty(tempFixed(cxReg),
                                                        useFixedAtStart(ins->object(), objReg),
                                                        useBoxFixedAtStart(val, tempReg1, tempReg2),
                                                        tempFixed(privReg),
                                                        tempFixed(valueReg));
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
                                                        useFixedAtStart(ins->object(), objReg),
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
    MOZ_ASSERT(obj->type() == MIRType::Object);

    MIRType type = ins->type();

    if (type == MIRType::Value) {
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
    MOZ_ASSERT(ins->input()->type() == MIRType::Object);
    MOZ_ASSERT(IsSimdType(ins->type()));
    LUse in = useRegister(ins->input());
    LSimdUnbox* lir = new(alloc()) LSimdUnbox(in, temp());
    assignSnapshot(lir, Bailout_UnexpectedSimdInput);
    define(lir, ins);
}

void
LIRGenerator::visitSimdConstant(MSimdConstant* ins)
{
    MOZ_ASSERT(IsSimdType(ins->type()));

    switch (ins->type()) {
      case MIRType::Int8x16:
      case MIRType::Int16x8:
      case MIRType::Int32x4:
      case MIRType::Bool8x16:
      case MIRType::Bool16x8:
      case MIRType::Bool32x4:
        define(new(alloc()) LSimd128Int(), ins);
        break;
      case MIRType::Float32x4:
        define(new(alloc()) LSimd128Float(), ins);
        break;
      default:
        MOZ_CRASH("Unknown SIMD kind when generating constant");
    }
}

void
LIRGenerator::visitSimdConvert(MSimdConvert* ins)
{
    MOZ_ASSERT(IsSimdType(ins->type()));
    MDefinition* input = ins->input();
    LUse use = useRegister(input);
    if (ins->type() == MIRType::Int32x4) {
        MOZ_ASSERT(input->type() == MIRType::Float32x4);
        switch (ins->signedness()) {
          case SimdSign::Signed: {
              LFloat32x4ToInt32x4* lir = new(alloc()) LFloat32x4ToInt32x4(use, temp());
              if (!gen->compilingWasm())
                  assignSnapshot(lir, Bailout_BoundsCheck);
              define(lir, ins);
              break;
          }
          case SimdSign::Unsigned: {
              LFloat32x4ToUint32x4* lir =
                new (alloc()) LFloat32x4ToUint32x4(use, temp(), temp(LDefinition::SIMD128INT));
              if (!gen->compilingWasm())
                  assignSnapshot(lir, Bailout_BoundsCheck);
              define(lir, ins);
              break;
          }
          default:
            MOZ_CRASH("Unexpected SimdConvert sign");
        }
    } else if (ins->type() == MIRType::Float32x4) {
        MOZ_ASSERT(input->type() == MIRType::Int32x4);
        MOZ_ASSERT(ins->signedness() == SimdSign::Signed, "Unexpected SimdConvert sign");
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
LIRGenerator::visitSimdAllTrue(MSimdAllTrue* ins)
{
    MDefinition* input = ins->input();
    MOZ_ASSERT(IsBooleanSimdType(input->type()));

    LUse use = useRegisterAtStart(input);
    define(new(alloc()) LSimdAllTrue(use), ins);
}

void
LIRGenerator::visitSimdAnyTrue(MSimdAnyTrue* ins)
{
    MDefinition* input = ins->input();
    MOZ_ASSERT(IsBooleanSimdType(input->type()));

    LUse use = useRegisterAtStart(input);
    define(new(alloc()) LSimdAnyTrue(use), ins);
}

void
LIRGenerator::visitSimdUnaryArith(MSimdUnaryArith* ins)
{
    MOZ_ASSERT(IsSimdType(ins->input()->type()));
    MOZ_ASSERT(IsSimdType(ins->type()));

    // Cannot be at start, as the ouput is used as a temporary to store values.
    LUse in = use(ins->input());

    switch (ins->type()) {
      case MIRType::Int8x16:
      case MIRType::Bool8x16:
        define(new (alloc()) LSimdUnaryArithIx16(in), ins);
        break;
      case MIRType::Int16x8:
      case MIRType::Bool16x8:
        define(new (alloc()) LSimdUnaryArithIx8(in), ins);
        break;
      case MIRType::Int32x4:
      case MIRType::Bool32x4:
        define(new (alloc()) LSimdUnaryArithIx4(in), ins);
        break;
      case MIRType::Float32x4:
        define(new (alloc()) LSimdUnaryArithFx4(in), ins);
        break;
      default:
        MOZ_CRASH("Unknown SIMD kind for unary operation");
    }
}

void
LIRGenerator::visitSimdBinaryComp(MSimdBinaryComp* ins)
{
    MOZ_ASSERT(IsSimdType(ins->lhs()->type()));
    MOZ_ASSERT(IsSimdType(ins->rhs()->type()));
    MOZ_ASSERT(IsBooleanSimdType(ins->type()));

    if (ShouldReorderCommutative(ins->lhs(), ins->rhs(), ins))
        ins->reverse();

    switch (ins->specialization()) {
      case MIRType::Int8x16: {
          MOZ_ASSERT(ins->signedness() == SimdSign::Signed);
          LSimdBinaryCompIx16* add = new (alloc()) LSimdBinaryCompIx16();
          lowerForFPU(add, ins, ins->lhs(), ins->rhs());
          return;
      }
      case MIRType::Int16x8: {
          MOZ_ASSERT(ins->signedness() == SimdSign::Signed);
          LSimdBinaryCompIx8* add = new (alloc()) LSimdBinaryCompIx8();
          lowerForFPU(add, ins, ins->lhs(), ins->rhs());
          return;
      }
      case MIRType::Int32x4: {
          MOZ_ASSERT(ins->signedness() == SimdSign::Signed);
          LSimdBinaryCompIx4* add = new (alloc()) LSimdBinaryCompIx4();
          lowerForCompIx4(add, ins, ins->lhs(), ins->rhs());
          return;
      }
      case MIRType::Float32x4: {
          MOZ_ASSERT(ins->signedness() == SimdSign::NotApplicable);
          LSimdBinaryCompFx4* add = new (alloc()) LSimdBinaryCompFx4();
          lowerForCompFx4(add, ins, ins->lhs(), ins->rhs());
          return;
      }
      default:
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
    LSimdBinaryBitwise* lir = new(alloc()) LSimdBinaryBitwise;
    lowerForFPU(lir, ins, lhs, rhs);
}

void
LIRGenerator::visitSimdShift(MSimdShift* ins)
{
    MOZ_ASSERT(IsIntegerSimdType(ins->type()));
    MOZ_ASSERT(ins->lhs()->type() == ins->type());
    MOZ_ASSERT(ins->rhs()->type() == MIRType::Int32);

    LUse vector = useRegisterAtStart(ins->lhs());
    LAllocation value = useRegisterOrConstant(ins->rhs());
    // We need a temp register to mask the shift amount, but not if the shift
    // amount is a constant.
    LDefinition tempReg = value.isConstant() ? LDefinition::BogusTemp() : temp();
    LSimdShift* lir = new(alloc()) LSimdShift(vector, value, tempReg);
    defineReuseInput(lir, ins, 0);
}

void
LIRGenerator::visitLexicalCheck(MLexicalCheck* ins)
{
    MDefinition* input = ins->input();
    MOZ_ASSERT(input->type() == MIRType::Value);
    LLexicalCheck* lir = new(alloc()) LLexicalCheck(useBox(input));
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
    MOZ_ASSERT(retVal->type() == MIRType::Value);
    MOZ_ASSERT(thisVal->type() == MIRType::Value);

    LCheckReturn* lir = new(alloc()) LCheckReturn(useBoxAtStart(retVal), useBoxAtStart(thisVal));
    assignSnapshot(lir, Bailout_BadDerivedConstructorReturn);
    add(lir, ins);
    redefine(ins, retVal);
}

void
LIRGenerator::visitCheckIsObj(MCheckIsObj* ins)
{
    MDefinition* checkVal = ins->checkValue();
    MOZ_ASSERT(checkVal->type() == MIRType::Value);

    LCheckIsObj* lir = new(alloc()) LCheckIsObj(useBoxAtStart(checkVal));
    redefine(ins, checkVal);
    add(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitCheckIsCallable(MCheckIsCallable* ins)
{
    MDefinition* checkVal = ins->checkValue();
    MOZ_ASSERT(checkVal->type() == MIRType::Value);

    LCheckIsCallable* lir = new(alloc()) LCheckIsCallable(useBox(checkVal),
                                                          temp());
    redefine(ins, checkVal);
    add(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitCheckObjCoercible(MCheckObjCoercible* ins)
{
    MDefinition* checkVal = ins->checkValue();
    MOZ_ASSERT(checkVal->type() == MIRType::Value);

    LCheckObjCoercible* lir = new(alloc()) LCheckObjCoercible(useBoxAtStart(checkVal));
    redefine(ins, checkVal);
    add(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitDebugCheckSelfHosted(MDebugCheckSelfHosted* ins)
{
    MDefinition* checkVal = ins->checkValue();
    MOZ_ASSERT(checkVal->type() == MIRType::Value);

    LDebugCheckSelfHosted* lir = new (alloc()) LDebugCheckSelfHosted(useBoxAtStart(checkVal));
    redefine(ins, checkVal);
    add(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitFinishBoundFunctionInit(MFinishBoundFunctionInit* ins)
{
    auto lir = new(alloc()) LFinishBoundFunctionInit(useRegister(ins->bound()),
                                                     useRegister(ins->target()),
                                                     useRegister(ins->argCount()),
                                                     temp(), temp());
    add(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGenerator::visitIsPackedArray(MIsPackedArray* ins)
{
    MOZ_ASSERT(ins->array()->type() == MIRType::Object);
    MOZ_ASSERT(ins->type() == MIRType::Boolean);

    auto lir = new(alloc()) LIsPackedArray(useRegister(ins->array()), temp());
    define(lir, ins);
}

void
LIRGenerator::visitGetPrototypeOf(MGetPrototypeOf* ins)
{
    MOZ_ASSERT(ins->target()->type() == MIRType::Object);
    MOZ_ASSERT(ins->type() == MIRType::Value);

    auto lir = new(alloc()) LGetPrototypeOf(useRegister(ins->target()));
    defineBox(lir, ins);
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
    if (ins->isRecoveredOnBailout()) {
        MOZ_ASSERT(!JitOptions.disableRecoverIns);
        return true;
    }

    if (!gen->ensureBallast())
        return false;
    ins->accept(this);

    if (ins->possiblyCalls()) {
        gen->setNeedsStaticStackAlignment();
        gen->setNeedsOverrecursedCheck();
    }

    if (ins->resumePoint())
        updateResumeState(ins);

#ifdef DEBUG
    ins->setInWorklistUnchecked();
#endif

    // If no safepoint was created, there's no need for an OSI point.
    if (LOsiPoint* osiPoint = popOsiPoint())
        add(osiPoint);

    return !errored();
}

void
LIRGenerator::definePhis()
{
    size_t lirIndex = 0;
    MBasicBlock* block = current->mir();
    for (MPhiIterator phi(block->phisBegin()); phi != block->phisEnd(); phi++) {
        if (phi->type() == MIRType::Value) {
            defineUntypedPhi(*phi, lirIndex);
            lirIndex += BOX_PIECES;
        } else if (phi->type() == MIRType::Int64) {
            defineInt64Phi(*phi, lirIndex);
            lirIndex += INT64_PIECES;
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
    // As Value Numbering phase can remove edges from the entry basic block to a
    // code paths reachable from the OSR entry point, we have to add fixup
    // blocks to keep the dominator tree organized the same way. These fixup
    // blocks are flaged as unreachable, and should only exist iff the graph has
    // an OSR block.
    //
    // Note: RangeAnalysis can flag blocks as unreachable, but they are only
    // removed iff GVN (including UCE) is enabled.
    MOZ_ASSERT_IF(!mir()->compilingWasm() && !block->unreachable(), block->entryResumePoint());
    MOZ_ASSERT_IF(block->unreachable(), block->graph().osrBlock() ||
                  !mir()->optimizationInfo().gvnEnabled());
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

    // See fixup blocks added by Value Numbering, to keep the dominator relation
    // modified by the presence of the OSR block.
    MOZ_ASSERT_IF(block->unreachable(), *block->begin() == block->lastIns() ||
                  !mir()->optimizationInfo().gvnEnabled());
    MOZ_ASSERT_IF(block->unreachable(), block->graph().osrBlock() ||
                  !mir()->optimizationInfo().gvnEnabled());
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
            if (!gen->ensureBallast())
                return false;

            MDefinition* opd = phi->getOperand(position);
            ensureDefined(opd);

            MOZ_ASSERT(opd->type() == phi->type());

            if (phi->type() == MIRType::Value) {
                lowerUntypedPhiInput(*phi, position, successor->lir(), lirIndex);
                lirIndex += BOX_PIECES;
            } else if (phi->type() == MIRType::Int64) {
                lowerInt64PhiInput(*phi, position, successor->lir(), lirIndex);
                lirIndex += INT64_PIECES;
            } else {
                lowerTypedPhiInput(*phi, position, successor->lir(), lirIndex);
                lirIndex += 1;
            }
        }
    }

    // Now emit the last instruction, which is some form of branch.
    if (!visitInstruction(block->lastIns()))
        return false;

    return true;
}

void
LIRGenerator::visitNaNToZero(MNaNToZero *ins)
{
    MDefinition* input = ins->input();

    if (ins->operandIsNeverNaN() && ins->operandIsNeverNegativeZero()) {
        redefine(ins, input);
        return;
    }
    LNaNToZero* lir = new(alloc()) LNaNToZero(useRegisterAtStart(input), tempDouble());
    defineReuseInput(lir, ins, 0);
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
LIRGenerator::visitArgumentState(MArgumentState* objState)
{
    // ArgumentState nodes are always inlined at their uses.
}

void
LIRGenerator::visitUnknownValue(MUnknownValue* ins)
{
    MOZ_CRASH("Can not lower unknown value.");
}

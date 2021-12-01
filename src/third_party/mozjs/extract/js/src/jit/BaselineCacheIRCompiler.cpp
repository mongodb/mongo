/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/BaselineCacheIRCompiler.h"

#include "jit/CacheIR.h"
#include "jit/Linker.h"
#include "jit/SharedICHelpers.h"
#include "proxy/DeadObjectProxy.h"
#include "proxy/Proxy.h"

#include "jit/MacroAssembler-inl.h"
#include "jit/SharedICHelpers-inl.h"
#include "vm/JSCompartment-inl.h"
#include "vm/JSContext-inl.h"

using namespace js;
using namespace js::jit;

using mozilla::Maybe;

class AutoStubFrame;

Address
CacheRegisterAllocator::addressOf(MacroAssembler& masm, BaselineFrameSlot slot) const
{
    uint32_t offset = stackPushed_ + ICStackValueOffset + slot.slot() * sizeof(JS::Value);
    return Address(masm.getStackPointer(), offset);
}

// BaselineCacheIRCompiler compiles CacheIR to BaselineIC native code.
class MOZ_RAII BaselineCacheIRCompiler : public CacheIRCompiler
{
#ifdef DEBUG
    // Some Baseline IC stubs can be used in IonMonkey through SharedStubs.
    // Those stubs have different machine code, so we need to track whether
    // we're compiling for Baseline or Ion.
    ICStubEngine engine_;
#endif

    uint32_t stubDataOffset_;
    bool inStubFrame_;
    bool makesGCCalls_;

    MOZ_MUST_USE bool callVM(MacroAssembler& masm, const VMFunction& fun);

    MOZ_MUST_USE bool callTypeUpdateIC(Register obj, ValueOperand val, Register scratch,
                                       LiveGeneralRegisterSet saveRegs);

    MOZ_MUST_USE bool emitStoreSlotShared(bool isFixed);
    MOZ_MUST_USE bool emitAddAndStoreSlotShared(CacheOp op);

  public:
    friend class AutoStubFrame;

    BaselineCacheIRCompiler(JSContext* cx, const CacheIRWriter& writer, ICStubEngine engine,
                            uint32_t stubDataOffset)
      : CacheIRCompiler(cx, writer, Mode::Baseline),
#ifdef DEBUG
        engine_(engine),
#endif
        stubDataOffset_(stubDataOffset),
        inStubFrame_(false),
        makesGCCalls_(false)
    {}

    MOZ_MUST_USE bool init(CacheKind kind);

    JitCode* compile();

    bool makesGCCalls() const { return makesGCCalls_; }

  private:
#define DEFINE_OP(op) MOZ_MUST_USE bool emit##op();
    CACHE_IR_OPS(DEFINE_OP)
#undef DEFINE_OP

    Address stubAddress(uint32_t offset) const {
        return Address(ICStubReg, stubDataOffset_ + offset);
    }
};

#define DEFINE_SHARED_OP(op) \
    bool BaselineCacheIRCompiler::emit##op() { return CacheIRCompiler::emit##op(); }
    CACHE_IR_SHARED_OPS(DEFINE_SHARED_OP)
#undef DEFINE_SHARED_OP

enum class CallCanGC { CanGC, CanNotGC };

// Instructions that have to perform a callVM require a stub frame. Call its
// enter() and leave() methods to enter/leave the stub frame.
class MOZ_RAII AutoStubFrame
{
    BaselineCacheIRCompiler& compiler;
#ifdef DEBUG
    uint32_t framePushedAtEnterStubFrame_;
#endif

    AutoStubFrame(const AutoStubFrame&) = delete;
    void operator=(const AutoStubFrame&) = delete;

  public:
    explicit AutoStubFrame(BaselineCacheIRCompiler& compiler)
      : compiler(compiler)
#ifdef DEBUG
        , framePushedAtEnterStubFrame_(0)
#endif
    { }

    void enter(MacroAssembler& masm, Register scratch, CallCanGC canGC = CallCanGC::CanGC) {
        MOZ_ASSERT(compiler.allocator.stackPushed() == 0);
        MOZ_ASSERT(compiler.engine_ == ICStubEngine::Baseline);

        EmitBaselineEnterStubFrame(masm, scratch);

#ifdef DEBUG
        framePushedAtEnterStubFrame_ = masm.framePushed();
#endif

        MOZ_ASSERT(!compiler.inStubFrame_);
        compiler.inStubFrame_ = true;
        if (canGC == CallCanGC::CanGC)
            compiler.makesGCCalls_ = true;
    }
    void leave(MacroAssembler& masm, bool calledIntoIon = false) {
        MOZ_ASSERT(compiler.inStubFrame_);
        compiler.inStubFrame_ = false;

#ifdef DEBUG
        masm.setFramePushed(framePushedAtEnterStubFrame_);
        if (calledIntoIon)
            masm.adjustFrame(sizeof(intptr_t)); // Calls into ion have this extra.
#endif

        EmitBaselineLeaveStubFrame(masm, calledIntoIon);
    }

#ifdef DEBUG
    ~AutoStubFrame() {
        MOZ_ASSERT(!compiler.inStubFrame_);
    }
#endif
};

bool
BaselineCacheIRCompiler::callVM(MacroAssembler& masm, const VMFunction& fun)
{
    MOZ_ASSERT(inStubFrame_);

    TrampolinePtr code = cx_->runtime()->jitRuntime()->getVMWrapper(fun);
    MOZ_ASSERT(fun.expectTailCall == NonTailCall);
    MOZ_ASSERT(engine_ == ICStubEngine::Baseline);

    EmitBaselineCallVM(code, masm);
    return true;
}

JitCode*
BaselineCacheIRCompiler::compile()
{
#ifndef JS_USE_LINK_REGISTER
    // The first value contains the return addres,
    // which we pull into ICTailCallReg for tail calls.
    masm.adjustFrame(sizeof(intptr_t));
#endif
#ifdef JS_CODEGEN_ARM
    masm.setSecondScratchReg(BaselineSecondScratchReg);
#endif

    do {
        switch (reader.readOp()) {
#define DEFINE_OP(op)                   \
          case CacheOp::op:             \
            if (!emit##op())            \
                return nullptr;         \
            break;
    CACHE_IR_OPS(DEFINE_OP)
#undef DEFINE_OP

          default:
            MOZ_CRASH("Invalid op");
        }

        allocator.nextOp();
    } while (reader.more());

    MOZ_ASSERT(!inStubFrame_);
    masm.assumeUnreachable("Should have returned from IC");

    // Done emitting the main IC code. Now emit the failure paths.
    for (size_t i = 0; i < failurePaths.length(); i++) {
        if (!emitFailurePath(i))
            return nullptr;
        EmitStubGuardFailure(masm);
    }

    Linker linker(masm);
    AutoFlushICache afc("getStubCode");
    Rooted<JitCode*> newStubCode(cx_, linker.newCode(cx_, CodeKind::Baseline));
    if (!newStubCode) {
        cx_->recoverFromOutOfMemory();
        return nullptr;
    }

    return newStubCode;
}

bool
BaselineCacheIRCompiler::emitGuardShape()
{
    ObjOperandId objId = reader.objOperandId();
    Register obj = allocator.useRegister(masm, objId);
    AutoScratchRegister scratch1(allocator, masm);

    bool needSpectreMitigations = objectGuardNeedsSpectreMitigations(objId);

    Maybe<AutoScratchRegister> maybeScratch2;
    if (needSpectreMitigations)
        maybeScratch2.emplace(allocator, masm);

    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;

    Address addr(stubAddress(reader.stubOffset()));
    masm.loadPtr(addr, scratch1);
    if (needSpectreMitigations) {
        masm.branchTestObjShape(Assembler::NotEqual, obj, scratch1, *maybeScratch2, obj,
                                failure->label());
    } else {
        masm.branchTestObjShapeNoSpectreMitigations(Assembler::NotEqual, obj, scratch1,
                                                    failure->label());
    }

    return true;
}

bool
BaselineCacheIRCompiler::emitGuardGroup()
{
    ObjOperandId objId = reader.objOperandId();
    Register obj = allocator.useRegister(masm, objId);
    AutoScratchRegister scratch1(allocator, masm);

    bool needSpectreMitigations = objectGuardNeedsSpectreMitigations(objId);

    Maybe<AutoScratchRegister> maybeScratch2;
    if (needSpectreMitigations)
        maybeScratch2.emplace(allocator, masm);

    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;

    Address addr(stubAddress(reader.stubOffset()));
    masm.loadPtr(addr, scratch1);
    if (needSpectreMitigations) {
        masm.branchTestObjGroup(Assembler::NotEqual, obj, scratch1, *maybeScratch2, obj,
                                failure->label());
    } else {
        masm.branchTestObjGroupNoSpectreMitigations(Assembler::NotEqual, obj, scratch1,
                                                    failure->label());
    }

    return true;
}

bool
BaselineCacheIRCompiler::emitGuardGroupHasUnanalyzedNewScript()
{
    Address addr(stubAddress(reader.stubOffset()));
    AutoScratchRegister scratch1(allocator, masm);
    AutoScratchRegister scratch2(allocator, masm);

    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;

    masm.loadPtr(addr, scratch1);
    masm.guardGroupHasUnanalyzedNewScript(scratch1, scratch2, failure->label());
    return true;
}

bool
BaselineCacheIRCompiler::emitGuardProto()
{
    Register obj = allocator.useRegister(masm, reader.objOperandId());
    AutoScratchRegister scratch(allocator, masm);

    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;

    Address addr(stubAddress(reader.stubOffset()));
    masm.loadObjProto(obj, scratch);
    masm.branchPtr(Assembler::NotEqual, addr, scratch, failure->label());
    return true;
}

bool
BaselineCacheIRCompiler::emitGuardCompartment()
{
    Register obj = allocator.useRegister(masm, reader.objOperandId());
    AutoScratchRegister scratch(allocator, masm);

    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;

    // Verify that the global wrapper is still valid, as
    // it is pre-requisite for doing the compartment check.
    Address globalWrapper(stubAddress(reader.stubOffset()));
    masm.loadPtr(globalWrapper, scratch);
    Address handlerAddr(scratch, ProxyObject::offsetOfHandler());
    masm.branchPtr(Assembler::Equal, handlerAddr, ImmPtr(&DeadObjectProxy::singleton), failure->label());

    Address addr(stubAddress(reader.stubOffset()));
    masm.branchTestObjCompartment(Assembler::NotEqual, obj, addr, scratch, failure->label());
    return true;
}

bool
BaselineCacheIRCompiler::emitGuardAnyClass()
{
    ObjOperandId objId = reader.objOperandId();
    Register obj = allocator.useRegister(masm, objId);
    AutoScratchRegister scratch(allocator, masm);

    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;

    Address testAddr(stubAddress(reader.stubOffset()));
    if (objectGuardNeedsSpectreMitigations(objId)) {
        masm.branchTestObjClass(Assembler::NotEqual, obj, testAddr, scratch, obj,
                                failure->label());
    } else {
        masm.branchTestObjClassNoSpectreMitigations(Assembler::NotEqual, obj, testAddr, scratch,
                                                    failure->label());
    }

    return true;
}

bool
BaselineCacheIRCompiler::emitGuardHasProxyHandler()
{
    Register obj = allocator.useRegister(masm, reader.objOperandId());
    AutoScratchRegister scratch(allocator, masm);

    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;

    Address testAddr(stubAddress(reader.stubOffset()));
    masm.loadPtr(testAddr, scratch);

    Address handlerAddr(obj, ProxyObject::offsetOfHandler());
    masm.branchPtr(Assembler::NotEqual, handlerAddr, scratch, failure->label());
    return true;
}

bool
BaselineCacheIRCompiler::emitGuardSpecificObject()
{
    Register obj = allocator.useRegister(masm, reader.objOperandId());

    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;

    Address addr(stubAddress(reader.stubOffset()));
    masm.branchPtr(Assembler::NotEqual, addr, obj, failure->label());
    return true;
}

bool
BaselineCacheIRCompiler::emitGuardSpecificAtom()
{
    Register str = allocator.useRegister(masm, reader.stringOperandId());
    AutoScratchRegister scratch(allocator, masm);

    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;

    Address atomAddr(stubAddress(reader.stubOffset()));

    Label done;
    masm.branchPtr(Assembler::Equal, atomAddr, str, &done);

    // The pointers are not equal, so if the input string is also an atom it
    // must be a different string.
    masm.branchTest32(Assembler::Zero, Address(str, JSString::offsetOfFlags()),
                      Imm32(JSString::NON_ATOM_BIT), failure->label());

    // Check the length.
    masm.loadPtr(atomAddr, scratch);
    masm.loadStringLength(scratch, scratch);
    masm.branch32(Assembler::NotEqual, Address(str, JSString::offsetOfLength()),
                  scratch, failure->label());

    // We have a non-atomized string with the same length. Call a helper
    // function to do the comparison.
    LiveRegisterSet volatileRegs(GeneralRegisterSet::Volatile(), liveVolatileFloatRegs());
    masm.PushRegsInMask(volatileRegs);

    masm.setupUnalignedABICall(scratch);
    masm.loadPtr(atomAddr, scratch);
    masm.passABIArg(scratch);
    masm.passABIArg(str);
    masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, EqualStringsHelper));
    masm.mov(ReturnReg, scratch);

    LiveRegisterSet ignore;
    ignore.add(scratch);
    masm.PopRegsInMaskIgnore(volatileRegs, ignore);
    masm.branchIfFalseBool(scratch, failure->label());

    masm.bind(&done);
    return true;
}

bool
BaselineCacheIRCompiler::emitGuardSpecificSymbol()
{
    Register sym = allocator.useRegister(masm, reader.symbolOperandId());

    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;

    Address addr(stubAddress(reader.stubOffset()));
    masm.branchPtr(Assembler::NotEqual, addr, sym, failure->label());
    return true;
}

bool
BaselineCacheIRCompiler::emitGuardXrayExpandoShapeAndDefaultProto()
{
    Register obj = allocator.useRegister(masm, reader.objOperandId());
    bool hasExpando = reader.readBool();
    Address shapeWrapperAddress(stubAddress(reader.stubOffset()));

    AutoScratchRegister scratch(allocator, masm);
    Maybe<AutoScratchRegister> scratch2, scratch3;
    if (hasExpando) {
        scratch2.emplace(allocator, masm);
        scratch3.emplace(allocator, masm);
    }

    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;

    masm.loadPtr(Address(obj, ProxyObject::offsetOfReservedSlots()), scratch);
    Address holderAddress(scratch, sizeof(Value) * GetXrayJitInfo()->xrayHolderSlot);
    Address expandoAddress(scratch, NativeObject::getFixedSlotOffset(GetXrayJitInfo()->holderExpandoSlot));

    if (hasExpando) {
        masm.branchTestObject(Assembler::NotEqual, holderAddress, failure->label());
        masm.unboxObject(holderAddress, scratch);
        masm.branchTestObject(Assembler::NotEqual, expandoAddress, failure->label());
        masm.unboxObject(expandoAddress, scratch);

        // Unwrap the expando before checking its shape.
        masm.loadPtr(Address(scratch, ProxyObject::offsetOfReservedSlots()), scratch);
        masm.unboxObject(Address(scratch, detail::ProxyReservedSlots::offsetOfPrivateSlot()), scratch);

        masm.loadPtr(shapeWrapperAddress, scratch2.ref());
        LoadShapeWrapperContents(masm, scratch2.ref(), scratch2.ref(), failure->label());
        masm.branchTestObjShape(Assembler::NotEqual, scratch, *scratch2, *scratch3, scratch,
                                failure->label());

        // The reserved slots on the expando should all be in fixed slots.
        Address protoAddress(scratch, NativeObject::getFixedSlotOffset(GetXrayJitInfo()->expandoProtoSlot));
        masm.branchTestUndefined(Assembler::NotEqual, protoAddress, failure->label());
    } else {
        Label done;
        masm.branchTestObject(Assembler::NotEqual, holderAddress, &done);
        masm.unboxObject(holderAddress, scratch);
        masm.branchTestObject(Assembler::Equal, expandoAddress, failure->label());
        masm.bind(&done);
    }

    return true;
}

bool
BaselineCacheIRCompiler::emitGuardFunctionPrototype()
{
    Register obj = allocator.useRegister(masm, reader.objOperandId());
    Register prototypeObject = allocator.useRegister(masm, reader.objOperandId());

    // Allocate registers before the failure path to make sure they're registered
    // by addFailurePath.
    AutoScratchRegister scratch1(allocator, masm);
    AutoScratchRegister scratch2(allocator, masm);

    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;

     // Guard on the .prototype object.
    masm.loadPtr(Address(obj, NativeObject::offsetOfSlots()), scratch1);
    masm.load32(Address(stubAddress(reader.stubOffset())), scratch2);
    BaseValueIndex prototypeSlot(scratch1, scratch2);
    masm.branchTestObject(Assembler::NotEqual, prototypeSlot, failure->label());
    masm.unboxObject(prototypeSlot, scratch1);
    masm.branchPtr(Assembler::NotEqual,
                   prototypeObject,
                   scratch1, failure->label());

    return true;
}

bool
BaselineCacheIRCompiler::emitLoadValueResult()
{
    AutoOutputRegister output(*this);
    masm.loadValue(stubAddress(reader.stubOffset()), output.valueReg());
    return true;
}

bool
BaselineCacheIRCompiler::emitLoadFixedSlotResult()
{
    AutoOutputRegister output(*this);
    Register obj = allocator.useRegister(masm, reader.objOperandId());
    AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

    masm.load32(stubAddress(reader.stubOffset()), scratch);
    masm.loadValue(BaseIndex(obj, scratch, TimesOne), output.valueReg());
    return true;
}

bool
BaselineCacheIRCompiler::emitLoadDynamicSlotResult()
{
    AutoOutputRegister output(*this);
    Register obj = allocator.useRegister(masm, reader.objOperandId());
    AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);
    AutoScratchRegister scratch2(allocator, masm);

    masm.load32(stubAddress(reader.stubOffset()), scratch);
    masm.loadPtr(Address(obj, NativeObject::offsetOfSlots()), scratch2);
    masm.loadValue(BaseIndex(scratch2, scratch, TimesOne), output.valueReg());
    return true;
}

bool
BaselineCacheIRCompiler::emitMegamorphicLoadSlotResult()
{
    AutoOutputRegister output(*this);

    Register obj = allocator.useRegister(masm, reader.objOperandId());
    Address nameAddr = stubAddress(reader.stubOffset());
    bool handleMissing = reader.readBool();

    AutoScratchRegisterMaybeOutput scratch1(allocator, masm, output);
    AutoScratchRegister scratch2(allocator, masm);
    AutoScratchRegister scratch3(allocator, masm);

    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;

    // The object must be Native.
    masm.branchIfNonNativeObj(obj, scratch3, failure->label());

    masm.Push(UndefinedValue());
    masm.moveStackPtrTo(scratch3.get());

    LiveRegisterSet volatileRegs(GeneralRegisterSet::Volatile(), liveVolatileFloatRegs());
    volatileRegs.takeUnchecked(scratch1);
    volatileRegs.takeUnchecked(scratch2);
    volatileRegs.takeUnchecked(scratch3);
    masm.PushRegsInMask(volatileRegs);

    masm.setupUnalignedABICall(scratch1);
    masm.loadJSContext(scratch1);
    masm.passABIArg(scratch1);
    masm.passABIArg(obj);
    masm.loadPtr(nameAddr, scratch2);
    masm.passABIArg(scratch2);
    masm.passABIArg(scratch3);
    if (handleMissing)
        masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, (GetNativeDataProperty<true>)));
    else
        masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, (GetNativeDataProperty<false>)));
    masm.mov(ReturnReg, scratch2);
    masm.PopRegsInMask(volatileRegs);

    masm.loadTypedOrValue(Address(masm.getStackPointer(), 0), output);
    masm.adjustStack(sizeof(Value));

    masm.branchIfFalseBool(scratch2, failure->label());
    if (JitOptions.spectreJitToCxxCalls)
        masm.speculationBarrier();
    return true;
}

bool
BaselineCacheIRCompiler::emitMegamorphicStoreSlot()
{
    Register obj = allocator.useRegister(masm, reader.objOperandId());
    Address nameAddr = stubAddress(reader.stubOffset());
    ValueOperand val = allocator.useValueRegister(masm, reader.valOperandId());
    bool needsTypeBarrier = reader.readBool();

    AutoScratchRegister scratch1(allocator, masm);
    AutoScratchRegister scratch2(allocator, masm);

    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;

    masm.Push(val);
    masm.moveStackPtrTo(val.scratchReg());

    LiveRegisterSet volatileRegs(GeneralRegisterSet::Volatile(), liveVolatileFloatRegs());
    volatileRegs.takeUnchecked(scratch1);
    volatileRegs.takeUnchecked(scratch2);
    volatileRegs.takeUnchecked(val);
    masm.PushRegsInMask(volatileRegs);

    masm.setupUnalignedABICall(scratch1);
    masm.loadJSContext(scratch1);
    masm.passABIArg(scratch1);
    masm.passABIArg(obj);
    masm.loadPtr(nameAddr, scratch2);
    masm.passABIArg(scratch2);
    masm.passABIArg(val.scratchReg());
    if (needsTypeBarrier)
        masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, (SetNativeDataProperty<true>)));
    else
        masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, (SetNativeDataProperty<false>)));
    masm.mov(ReturnReg, scratch1);
    masm.PopRegsInMask(volatileRegs);

    masm.loadValue(Address(masm.getStackPointer(), 0), val);
    masm.adjustStack(sizeof(Value));

    masm.branchIfFalseBool(scratch1, failure->label());
    return true;
}

bool
BaselineCacheIRCompiler::emitGuardHasGetterSetter()
{
    Register obj = allocator.useRegister(masm, reader.objOperandId());
    Address shapeAddr = stubAddress(reader.stubOffset());

    AutoScratchRegister scratch1(allocator, masm);
    AutoScratchRegister scratch2(allocator, masm);

    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;

    LiveRegisterSet volatileRegs(GeneralRegisterSet::Volatile(), liveVolatileFloatRegs());
    volatileRegs.takeUnchecked(scratch1);
    volatileRegs.takeUnchecked(scratch2);
    masm.PushRegsInMask(volatileRegs);

    masm.setupUnalignedABICall(scratch1);
    masm.loadJSContext(scratch1);
    masm.passABIArg(scratch1);
    masm.passABIArg(obj);
    masm.loadPtr(shapeAddr, scratch2);
    masm.passABIArg(scratch2);
    masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, ObjectHasGetterSetter));
    masm.mov(ReturnReg, scratch1);
    masm.PopRegsInMask(volatileRegs);

    masm.branchIfFalseBool(scratch1, failure->label());
    return true;
}

bool
BaselineCacheIRCompiler::emitCallScriptedGetterResult()
{
    MOZ_ASSERT(engine_ == ICStubEngine::Baseline);

    Register obj = allocator.useRegister(masm, reader.objOperandId());
    Address getterAddr(stubAddress(reader.stubOffset()));

    AutoScratchRegister code(allocator, masm);
    AutoScratchRegister callee(allocator, masm);
    AutoScratchRegister scratch(allocator, masm);

    // First, ensure our getter is non-lazy.
    {
        FailurePath* failure;
        if (!addFailurePath(&failure))
            return false;

        masm.loadPtr(getterAddr, callee);
        masm.branchIfFunctionHasNoJitEntry(callee, /* constructing */ false, failure->label());
        masm.loadJitCodeRaw(callee, code);
    }

    allocator.discardStack(masm);

    AutoStubFrame stubFrame(*this);
    stubFrame.enter(masm, scratch);

    // Align the stack such that the JitFrameLayout is aligned on
    // JitStackAlignment.
    masm.alignJitStackBasedOnNArgs(0);

    // Getter is called with 0 arguments, just |obj| as thisv.
    // Note that we use Push, not push, so that callJit will align the stack
    // properly on ARM.
    masm.Push(TypedOrValueRegister(MIRType::Object, AnyRegister(obj)));

    EmitBaselineCreateStubFrameDescriptor(masm, scratch, JitFrameLayout::Size());
    masm.Push(Imm32(0));  // ActualArgc is 0
    masm.Push(callee);
    masm.Push(scratch);

    // Handle arguments underflow.
    Label noUnderflow;
    masm.load16ZeroExtend(Address(callee, JSFunction::offsetOfNargs()), callee);
    masm.branch32(Assembler::Equal, callee, Imm32(0), &noUnderflow);
    {
        // Call the arguments rectifier.
        TrampolinePtr argumentsRectifier = cx_->runtime()->jitRuntime()->getArgumentsRectifier();
        masm.movePtr(argumentsRectifier, code);
    }

    masm.bind(&noUnderflow);
    masm.callJit(code);

    stubFrame.leave(masm, true);
    return true;
}

typedef bool (*CallNativeGetterFn)(JSContext*, HandleFunction, HandleObject, MutableHandleValue);
static const VMFunction CallNativeGetterInfo =
    FunctionInfo<CallNativeGetterFn>(CallNativeGetter, "CallNativeGetter");

bool
BaselineCacheIRCompiler::emitCallNativeGetterResult()
{
    Register obj = allocator.useRegister(masm, reader.objOperandId());
    Address getterAddr(stubAddress(reader.stubOffset()));

    AutoScratchRegister scratch(allocator, masm);

    allocator.discardStack(masm);

    AutoStubFrame stubFrame(*this);
    stubFrame.enter(masm, scratch);

    // Load the callee in the scratch register.
    masm.loadPtr(getterAddr, scratch);

    masm.Push(obj);
    masm.Push(scratch);

    if (!callVM(masm, CallNativeGetterInfo))
        return false;

    stubFrame.leave(masm);
    return true;
}

typedef bool (*ProxyGetPropertyFn)(JSContext*, HandleObject, HandleId, MutableHandleValue);
static const VMFunction ProxyGetPropertyInfo =
    FunctionInfo<ProxyGetPropertyFn>(ProxyGetProperty, "ProxyGetProperty");

bool
BaselineCacheIRCompiler::emitCallProxyGetResult()
{
    Register obj = allocator.useRegister(masm, reader.objOperandId());
    Address idAddr(stubAddress(reader.stubOffset()));

    AutoScratchRegister scratch(allocator, masm);

    allocator.discardStack(masm);

    AutoStubFrame stubFrame(*this);
    stubFrame.enter(masm, scratch);

    // Load the jsid in the scratch register.
    masm.loadPtr(idAddr, scratch);

    masm.Push(scratch);
    masm.Push(obj);

    if (!callVM(masm, ProxyGetPropertyInfo))
        return false;

    stubFrame.leave(masm);
    return true;
}

typedef bool (*ProxyGetPropertyByValueFn)(JSContext*, HandleObject, HandleValue, MutableHandleValue);
static const VMFunction ProxyGetPropertyByValueInfo =
    FunctionInfo<ProxyGetPropertyByValueFn>(ProxyGetPropertyByValue, "ProxyGetPropertyByValue");

bool
BaselineCacheIRCompiler::emitCallProxyGetByValueResult()
{
    Register obj = allocator.useRegister(masm, reader.objOperandId());
    ValueOperand idVal = allocator.useValueRegister(masm, reader.valOperandId());

    AutoScratchRegister scratch(allocator, masm);

    allocator.discardStack(masm);

    AutoStubFrame stubFrame(*this);
    stubFrame.enter(masm, scratch);

    masm.Push(idVal);
    masm.Push(obj);

    if (!callVM(masm, ProxyGetPropertyByValueInfo))
        return false;

    stubFrame.leave(masm);
    return true;
}

typedef bool (*ProxyHasFn)(JSContext*, HandleObject, HandleValue, MutableHandleValue);
static const VMFunction ProxyHasInfo = FunctionInfo<ProxyHasFn>(ProxyHas, "ProxyHas");

typedef bool (*ProxyHasOwnFn)(JSContext*, HandleObject, HandleValue, MutableHandleValue);
static const VMFunction ProxyHasOwnInfo = FunctionInfo<ProxyHasOwnFn>(ProxyHasOwn, "ProxyHasOwn");

bool
BaselineCacheIRCompiler::emitCallProxyHasPropResult()
{
    Register obj = allocator.useRegister(masm, reader.objOperandId());
    ValueOperand idVal = allocator.useValueRegister(masm, reader.valOperandId());
    bool hasOwn = reader.readBool();

    AutoScratchRegister scratch(allocator, masm);

    allocator.discardStack(masm);

    AutoStubFrame stubFrame(*this);
    stubFrame.enter(masm, scratch);

    masm.Push(idVal);
    masm.Push(obj);

    if (hasOwn) {
        if (!callVM(masm, ProxyHasOwnInfo))
            return false;
    } else {
        if (!callVM(masm, ProxyHasInfo))
            return false;
    }

    stubFrame.leave(masm);
    return true;
}

bool
BaselineCacheIRCompiler::emitLoadUnboxedPropertyResult()
{
    AutoOutputRegister output(*this);
    Register obj = allocator.useRegister(masm, reader.objOperandId());
    AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

    JSValueType fieldType = reader.valueType();
    Address fieldOffset(stubAddress(reader.stubOffset()));
    masm.load32(fieldOffset, scratch);
    masm.loadUnboxedProperty(BaseIndex(obj, scratch, TimesOne), fieldType, output);
    return true;
}

bool
BaselineCacheIRCompiler::emitGuardFrameHasNoArgumentsObject()
{
    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;

    masm.branchTest32(Assembler::NonZero,
                      Address(BaselineFrameReg, BaselineFrame::reverseOffsetOfFlags()),
                      Imm32(BaselineFrame::HAS_ARGS_OBJ),
                      failure->label());
    return true;
}

bool
BaselineCacheIRCompiler::emitLoadFrameCalleeResult()
{
    AutoOutputRegister output(*this);
    AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

    Address callee(BaselineFrameReg, BaselineFrame::offsetOfCalleeToken());
    masm.loadFunctionFromCalleeToken(callee, scratch);
    masm.tagValue(JSVAL_TYPE_OBJECT, scratch, output.valueReg());
    return true;
}

bool
BaselineCacheIRCompiler::emitLoadFrameNumActualArgsResult()
{
    AutoOutputRegister output(*this);
    AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

    Address actualArgs(BaselineFrameReg, BaselineFrame::offsetOfNumActualArgs());
    masm.loadPtr(actualArgs, scratch);
    masm.tagValue(JSVAL_TYPE_INT32, scratch, output.valueReg());
    return true;
}

bool
BaselineCacheIRCompiler::emitLoadTypedObjectResult()
{
    AutoOutputRegister output(*this);
    Register obj = allocator.useRegister(masm, reader.objOperandId());
    AutoScratchRegister scratch1(allocator, masm);
    AutoScratchRegister scratch2(allocator, masm);

    TypedThingLayout layout = reader.typedThingLayout();
    uint32_t typeDescr = reader.typeDescrKey();
    Address fieldOffset(stubAddress(reader.stubOffset()));

    // Get the object's data pointer.
    LoadTypedThingData(masm, layout, obj, scratch1);

    // Get the address being written to.
    masm.load32(fieldOffset, scratch2);
    masm.addPtr(scratch2, scratch1);

    Address fieldAddr(scratch1, 0);
    emitLoadTypedObjectResultShared(fieldAddr, scratch2, typeDescr, output);
    return true;
}

bool
BaselineCacheIRCompiler::emitLoadFrameArgumentResult()
{
    AutoOutputRegister output(*this);
    Register index = allocator.useRegister(masm, reader.int32OperandId());
    AutoScratchRegister scratch1(allocator, masm);
    AutoScratchRegisterMaybeOutput scratch2(allocator, masm, output);

    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;

    // Bounds check.
    masm.loadPtr(Address(BaselineFrameReg, BaselineFrame::offsetOfNumActualArgs()), scratch1);
    masm.spectreBoundsCheck32(index, scratch1, scratch2, failure->label());

    // Load the argument.
    masm.loadValue(BaseValueIndex(BaselineFrameReg, index, BaselineFrame::offsetOfArg(0)),
                   output.valueReg());
    return true;
}

bool
BaselineCacheIRCompiler::emitLoadEnvironmentFixedSlotResult()
{
    AutoOutputRegister output(*this);
    Register obj = allocator.useRegister(masm, reader.objOperandId());
    AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;

    masm.load32(stubAddress(reader.stubOffset()), scratch);
    BaseIndex slot(obj, scratch, TimesOne);

    // Check for uninitialized lexicals.
    masm.branchTestMagic(Assembler::Equal, slot, failure->label());

    // Load the value.
    masm.loadValue(slot, output.valueReg());
    return true;
}

bool
BaselineCacheIRCompiler::emitLoadEnvironmentDynamicSlotResult()
{
    AutoOutputRegister output(*this);
    Register obj = allocator.useRegister(masm, reader.objOperandId());
    AutoScratchRegister scratch(allocator, masm);
    AutoScratchRegisterMaybeOutput scratch2(allocator, masm, output);

    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;

    masm.load32(stubAddress(reader.stubOffset()), scratch);
    masm.loadPtr(Address(obj, NativeObject::offsetOfSlots()), scratch2);

    // Check for uninitialized lexicals.
    BaseIndex slot(scratch2, scratch, TimesOne);
    masm.branchTestMagic(Assembler::Equal, slot, failure->label());

    // Load the value.
    masm.loadValue(slot, output.valueReg());
    return true;
}

bool
BaselineCacheIRCompiler::emitLoadStringResult()
{
    AutoOutputRegister output(*this);
    AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

    masm.loadPtr(stubAddress(reader.stubOffset()), scratch);
    masm.tagValue(JSVAL_TYPE_STRING, scratch, output.valueReg());
    return true;
}

typedef bool (*StringSplitHelperFn)(JSContext*, HandleString, HandleString, HandleObjectGroup,
                              uint32_t limit, MutableHandleValue);
static const VMFunction StringSplitHelperInfo =
    FunctionInfo<StringSplitHelperFn>(StringSplitHelper, "StringSplitHelper");

bool
BaselineCacheIRCompiler::emitCallStringSplitResult()
{
    Register str = allocator.useRegister(masm, reader.stringOperandId());
    Register sep = allocator.useRegister(masm, reader.stringOperandId());
    Address groupAddr(stubAddress(reader.stubOffset()));

    AutoScratchRegister scratch(allocator, masm);
    allocator.discardStack(masm);

    AutoStubFrame stubFrame(*this);
    stubFrame.enter(masm, scratch);

    // Load the group in the scratch register.
    masm.loadPtr(groupAddr, scratch);

    masm.Push(Imm32(INT32_MAX));
    masm.Push(scratch);
    masm.Push(sep);
    masm.Push(str);

    if (!callVM(masm, StringSplitHelperInfo))
        return false;

    stubFrame.leave(masm);
    return true;
}

bool
BaselineCacheIRCompiler::callTypeUpdateIC(Register obj, ValueOperand val, Register scratch,
                                          LiveGeneralRegisterSet saveRegs)
{
    // Ensure the stack is empty for the VM call below.
    allocator.discardStack(masm);

    // R0 contains the value that needs to be typechecked.
    MOZ_ASSERT(val == R0);
    MOZ_ASSERT(scratch == R1.scratchReg());

#if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
    static const bool CallClobbersTailReg = false;
#else
    static const bool CallClobbersTailReg = true;
#endif

    // Call the first type update stub.
    if (CallClobbersTailReg)
        masm.push(ICTailCallReg);
    masm.push(ICStubReg);
    masm.loadPtr(Address(ICStubReg, ICUpdatedStub::offsetOfFirstUpdateStub()),
                 ICStubReg);
    masm.call(Address(ICStubReg, ICStub::offsetOfStubCode()));
    masm.pop(ICStubReg);
    if (CallClobbersTailReg)
        masm.pop(ICTailCallReg);

    // The update IC will store 0 or 1 in |scratch|, R1.scratchReg(), reflecting
    // if the value in R0 type-checked properly or not.
    Label done;
    masm.branch32(Assembler::Equal, scratch, Imm32(1), &done);

    AutoStubFrame stubFrame(*this);
    stubFrame.enter(masm, scratch, CallCanGC::CanNotGC);

    masm.PushRegsInMask(saveRegs);

    masm.Push(val);
    masm.Push(TypedOrValueRegister(MIRType::Object, AnyRegister(obj)));
    masm.Push(ICStubReg);

    // Load previous frame pointer, push BaselineFrame*.
    masm.loadPtr(Address(BaselineFrameReg, 0), scratch);
    masm.pushBaselineFramePtr(scratch, scratch);

    if (!callVM(masm, DoTypeUpdateFallbackInfo))
        return false;

    masm.PopRegsInMask(saveRegs);

    stubFrame.leave(masm);

    masm.bind(&done);
    return true;
}

bool
BaselineCacheIRCompiler::emitStoreSlotShared(bool isFixed)
{
    ObjOperandId objId = reader.objOperandId();
    Address offsetAddr = stubAddress(reader.stubOffset());

    // Allocate the fixed registers first. These need to be fixed for
    // callTypeUpdateIC.
    AutoScratchRegister scratch1(allocator, masm, R1.scratchReg());
    ValueOperand val = allocator.useFixedValueRegister(masm, reader.valOperandId(), R0);

    Register obj = allocator.useRegister(masm, objId);
    Maybe<AutoScratchRegister> scratch2;
    if (!isFixed)
        scratch2.emplace(allocator, masm);

    LiveGeneralRegisterSet saveRegs;
    saveRegs.add(obj);
    saveRegs.add(val);
    if (!callTypeUpdateIC(obj, val, scratch1, saveRegs))
        return false;

    masm.load32(offsetAddr, scratch1);

    if (isFixed) {
        BaseIndex slot(obj, scratch1, TimesOne);
        EmitPreBarrier(masm, slot, MIRType::Value);
        masm.storeValue(val, slot);
    } else {
        masm.loadPtr(Address(obj, NativeObject::offsetOfSlots()), scratch2.ref());
        BaseIndex slot(scratch2.ref(), scratch1, TimesOne);
        EmitPreBarrier(masm, slot, MIRType::Value);
        masm.storeValue(val, slot);
    }

    emitPostBarrierSlot(obj, val, scratch1);
    return true;
}

bool
BaselineCacheIRCompiler::emitStoreFixedSlot()
{
    return emitStoreSlotShared(true);
}

bool
BaselineCacheIRCompiler::emitStoreDynamicSlot()
{
    return emitStoreSlotShared(false);
}

bool
BaselineCacheIRCompiler::emitAddAndStoreSlotShared(CacheOp op)
{
    ObjOperandId objId = reader.objOperandId();
    Address offsetAddr = stubAddress(reader.stubOffset());

    // Allocate the fixed registers first. These need to be fixed for
    // callTypeUpdateIC.
    AutoScratchRegister scratch1(allocator, masm, R1.scratchReg());
    ValueOperand val = allocator.useFixedValueRegister(masm, reader.valOperandId(), R0);

    Register obj = allocator.useRegister(masm, objId);
    AutoScratchRegister scratch2(allocator, masm);

    bool changeGroup = reader.readBool();
    Address newGroupAddr = stubAddress(reader.stubOffset());
    Address newShapeAddr = stubAddress(reader.stubOffset());

    if (op == CacheOp::AllocateAndStoreDynamicSlot) {
        // We have to (re)allocate dynamic slots. Do this first, as it's the
        // only fallible operation here. This simplifies the callTypeUpdateIC
        // call below: it does not have to worry about saving registers used by
        // failure paths. Note that growSlotsDontReportOOM is fallible but does
        // not GC.
        Address numNewSlotsAddr = stubAddress(reader.stubOffset());

        FailurePath* failure;
        if (!addFailurePath(&failure))
            return false;

        LiveRegisterSet save(GeneralRegisterSet::Volatile(), liveVolatileFloatRegs());
        masm.PushRegsInMask(save);

        masm.setupUnalignedABICall(scratch1);
        masm.loadJSContext(scratch1);
        masm.passABIArg(scratch1);
        masm.passABIArg(obj);
        masm.load32(numNewSlotsAddr, scratch2);
        masm.passABIArg(scratch2);
        masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, NativeObject::growSlotsDontReportOOM));
        masm.mov(ReturnReg, scratch1);

        LiveRegisterSet ignore;
        ignore.add(scratch1);
        masm.PopRegsInMaskIgnore(save, ignore);

        masm.branchIfFalseBool(scratch1, failure->label());
    }

    LiveGeneralRegisterSet saveRegs;
    saveRegs.add(obj);
    saveRegs.add(val);
    if (!callTypeUpdateIC(obj, val, scratch1, saveRegs))
        return false;

    if (changeGroup) {
        // Changing object's group from a partially to fully initialized group,
        // per the acquired properties analysis. Only change the group if the
        // old group still has a newScript. This only applies to PlainObjects.
        Label noGroupChange;
        masm.branchIfObjGroupHasNoAddendum(obj, scratch1, &noGroupChange);

        // Update the object's group.
        masm.loadPtr(newGroupAddr, scratch1);
        masm.storeObjGroup(scratch1, obj, [](MacroAssembler& masm, const Address& addr) {
            EmitPreBarrier(masm, addr, MIRType::ObjectGroup);
        });

        masm.bind(&noGroupChange);
    }

    // Update the object's shape.
    masm.loadPtr(newShapeAddr, scratch1);
    masm.storeObjShape(scratch1, obj, [](MacroAssembler& masm, const Address& addr) {
        EmitPreBarrier(masm, addr, MIRType::Shape);
    });

    // Perform the store. No pre-barrier required since this is a new
    // initialization.
    masm.load32(offsetAddr, scratch1);
    if (op == CacheOp::AddAndStoreFixedSlot) {
        BaseIndex slot(obj, scratch1, TimesOne);
        masm.storeValue(val, slot);
    } else {
        MOZ_ASSERT(op == CacheOp::AddAndStoreDynamicSlot ||
                   op == CacheOp::AllocateAndStoreDynamicSlot);
        masm.loadPtr(Address(obj, NativeObject::offsetOfSlots()), scratch2);
        BaseIndex slot(scratch2, scratch1, TimesOne);
        masm.storeValue(val, slot);
    }

    emitPostBarrierSlot(obj, val, scratch1);
    return true;
}

bool
BaselineCacheIRCompiler::emitAddAndStoreFixedSlot()
{
    return emitAddAndStoreSlotShared(CacheOp::AddAndStoreFixedSlot);
}

bool
BaselineCacheIRCompiler::emitAddAndStoreDynamicSlot()
{
    return emitAddAndStoreSlotShared(CacheOp::AddAndStoreDynamicSlot);
}

bool
BaselineCacheIRCompiler::emitAllocateAndStoreDynamicSlot()
{
    return emitAddAndStoreSlotShared(CacheOp::AllocateAndStoreDynamicSlot);
}

bool
BaselineCacheIRCompiler::emitStoreUnboxedProperty()
{
    ObjOperandId objId = reader.objOperandId();
    JSValueType fieldType = reader.valueType();
    Address offsetAddr = stubAddress(reader.stubOffset());

    // Allocate the fixed registers first. These need to be fixed for
    // callTypeUpdateIC.
    AutoScratchRegister scratch(allocator, masm, R1.scratchReg());
    ValueOperand val = allocator.useFixedValueRegister(masm, reader.valOperandId(), R0);

    Register obj = allocator.useRegister(masm, objId);

    // We only need the type update IC if we are storing an object.
    if (fieldType == JSVAL_TYPE_OBJECT) {
        LiveGeneralRegisterSet saveRegs;
        saveRegs.add(obj);
        saveRegs.add(val);
        if (!callTypeUpdateIC(obj, val, scratch, saveRegs))
            return false;
    }

    masm.load32(offsetAddr, scratch);
    BaseIndex fieldAddr(obj, scratch, TimesOne);

    // Note that the storeUnboxedProperty call here is infallible, as the
    // IR emitter is responsible for guarding on |val|'s type.
    EmitICUnboxedPreBarrier(masm, fieldAddr, fieldType);
    masm.storeUnboxedProperty(fieldAddr, fieldType,
                              ConstantOrRegister(TypedOrValueRegister(val)),
                              /* failure = */ nullptr);

    if (UnboxedTypeNeedsPostBarrier(fieldType))
        emitPostBarrierSlot(obj, val, scratch);
    return true;
}

bool
BaselineCacheIRCompiler::emitStoreTypedObjectReferenceProperty()
{
    ObjOperandId objId = reader.objOperandId();
    Address offsetAddr = stubAddress(reader.stubOffset());
    TypedThingLayout layout = reader.typedThingLayout();
    ReferenceTypeDescr::Type type = reader.referenceTypeDescrType();

    // Allocate the fixed registers first. These need to be fixed for
    // callTypeUpdateIC.
    AutoScratchRegister scratch1(allocator, masm, R1.scratchReg());
    ValueOperand val = allocator.useFixedValueRegister(masm, reader.valOperandId(), R0);

    Register obj = allocator.useRegister(masm, objId);
    AutoScratchRegister scratch2(allocator, masm);

    // We don't need a type update IC if the property is always a string.
    if (type != ReferenceTypeDescr::TYPE_STRING) {
        LiveGeneralRegisterSet saveRegs;
        saveRegs.add(obj);
        saveRegs.add(val);
        if (!callTypeUpdateIC(obj, val, scratch1, saveRegs))
            return false;
    }

    // Compute the address being written to.
    LoadTypedThingData(masm, layout, obj, scratch1);
    masm.addPtr(offsetAddr, scratch1);
    Address dest(scratch1, 0);

    emitStoreTypedObjectReferenceProp(val, type, dest, scratch2);
    emitPostBarrierSlot(obj, val, scratch1);

    return true;
}

bool
BaselineCacheIRCompiler::emitStoreTypedObjectScalarProperty()
{
    Register obj = allocator.useRegister(masm, reader.objOperandId());
    Address offsetAddr = stubAddress(reader.stubOffset());
    TypedThingLayout layout = reader.typedThingLayout();
    Scalar::Type type = reader.scalarType();
    ValueOperand val = allocator.useValueRegister(masm, reader.valOperandId());
    AutoScratchRegister scratch1(allocator, masm);
    AutoScratchRegister scratch2(allocator, masm);

    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;

    // Compute the address being written to.
    LoadTypedThingData(masm, layout, obj, scratch1);
    masm.addPtr(offsetAddr, scratch1);
    Address dest(scratch1, 0);

    StoreToTypedArray(cx_, masm, type, val, dest, scratch2, failure->label());
    return true;
}

bool
BaselineCacheIRCompiler::emitStoreDenseElement()
{
    ObjOperandId objId = reader.objOperandId();
    Int32OperandId indexId = reader.int32OperandId();

    // Allocate the fixed registers first. These need to be fixed for
    // callTypeUpdateIC.
    AutoScratchRegister scratch(allocator, masm, R1.scratchReg());
    ValueOperand val = allocator.useFixedValueRegister(masm, reader.valOperandId(), R0);

    Register obj = allocator.useRegister(masm, objId);
    Register index = allocator.useRegister(masm, indexId);

    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;

    // Load obj->elements in scratch.
    masm.loadPtr(Address(obj, NativeObject::offsetOfElements()), scratch);

    // Bounds check. Unfortunately we don't have more registers available on
    // x86, so use InvalidReg and emit slightly slower code on x86.
    Register spectreTemp = InvalidReg;
    Address initLength(scratch, ObjectElements::offsetOfInitializedLength());
    masm.spectreBoundsCheck32(index, initLength, spectreTemp, failure->label());

    // Hole check.
    BaseObjectElementIndex element(scratch, index);
    masm.branchTestMagic(Assembler::Equal, element, failure->label());

    // Perform a single test to see if we either need to convert double
    // elements, clone the copy on write elements in the object or fail
    // due to a frozen element.
    Label noSpecialHandling;
    Address elementsFlags(scratch, ObjectElements::offsetOfFlags());
    masm.branchTest32(Assembler::Zero, elementsFlags,
                      Imm32(ObjectElements::CONVERT_DOUBLE_ELEMENTS |
                            ObjectElements::COPY_ON_WRITE |
                            ObjectElements::FROZEN),
                      &noSpecialHandling);

    // Fail if we need to clone copy on write elements or to throw due
    // to a frozen element.
    masm.branchTest32(Assembler::NonZero, elementsFlags,
                      Imm32(ObjectElements::COPY_ON_WRITE |
                            ObjectElements::FROZEN),
                      failure->label());

    // We need to convert int32 values being stored into doubles. Note that
    // double arrays are only created by IonMonkey, so if we have no FP support
    // Ion is disabled and there should be no double arrays.
    if (cx_->runtime()->jitSupportsFloatingPoint) {
        // It's fine to convert the value in place in Baseline. We can't do
        // this in Ion.
        masm.convertInt32ValueToDouble(val);
    } else {
        masm.assumeUnreachable("There shouldn't be double arrays when there is no FP support.");
    }

    masm.bind(&noSpecialHandling);

    // Call the type update IC. After this everything must be infallible as we
    // don't save all registers here.
    LiveGeneralRegisterSet saveRegs;
    saveRegs.add(obj);
    saveRegs.add(index);
    saveRegs.add(val);
    if (!callTypeUpdateIC(obj, val, scratch, saveRegs))
        return false;

    // Perform the store. Reload obj->elements because callTypeUpdateIC
    // used the scratch register.
    masm.loadPtr(Address(obj, NativeObject::offsetOfElements()), scratch);
    EmitPreBarrier(masm, element, MIRType::Value);
    masm.storeValue(val, element);

    emitPostBarrierElement(obj, val, scratch, index);
    return true;
}

bool
BaselineCacheIRCompiler::emitStoreDenseElementHole()
{
    ObjOperandId objId = reader.objOperandId();
    Int32OperandId indexId = reader.int32OperandId();

    // Allocate the fixed registers first. These need to be fixed for
    // callTypeUpdateIC.
    AutoScratchRegister scratch(allocator, masm, R1.scratchReg());
    ValueOperand val = allocator.useFixedValueRegister(masm, reader.valOperandId(), R0);

    Register obj = allocator.useRegister(masm, objId);
    Register index = allocator.useRegister(masm, indexId);

    bool handleAdd = reader.readBool();

    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;

    // Load obj->elements in scratch.
    masm.loadPtr(Address(obj, NativeObject::offsetOfElements()), scratch);

    BaseObjectElementIndex element(scratch, index);
    Address initLength(scratch, ObjectElements::offsetOfInitializedLength());
    Address elementsFlags(scratch, ObjectElements::offsetOfFlags());

    // Check for copy-on-write or frozen elements.
    masm.branchTest32(Assembler::NonZero, elementsFlags,
                      Imm32(ObjectElements::COPY_ON_WRITE |
                            ObjectElements::FROZEN),
                      failure->label());

    // We don't have enough registers on x86 so use InvalidReg. This will emit
    // slightly less efficient code on x86.
    Register spectreTemp = InvalidReg;

    if (handleAdd) {
        // Bounds check.
        Label capacityOk, outOfBounds;
        masm.spectreBoundsCheck32(index, initLength, spectreTemp, &outOfBounds);
        masm.jump(&capacityOk);

        // If we're out-of-bounds, only handle the index == initLength case.
        masm.bind(&outOfBounds);
        masm.branch32(Assembler::NotEqual, initLength, index, failure->label());

        // If index < capacity, we can add a dense element inline. If not we
        // need to allocate more elements.
        Label allocElement;
        Address capacity(scratch, ObjectElements::offsetOfCapacity());
        masm.spectreBoundsCheck32(index, capacity, spectreTemp, &allocElement);
        masm.jump(&capacityOk);

        // Check for non-writable array length. We only have to do this if
        // index >= capacity.
        masm.bind(&allocElement);
        masm.branchTest32(Assembler::NonZero, elementsFlags,
                          Imm32(ObjectElements::NONWRITABLE_ARRAY_LENGTH),
                          failure->label());

        LiveRegisterSet save(GeneralRegisterSet::Volatile(), liveVolatileFloatRegs());
        save.takeUnchecked(scratch);
        masm.PushRegsInMask(save);

        masm.setupUnalignedABICall(scratch);
        masm.loadJSContext(scratch);
        masm.passABIArg(scratch);
        masm.passABIArg(obj);
        masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, NativeObject::addDenseElementDontReportOOM));
        masm.mov(ReturnReg, scratch);

        masm.PopRegsInMask(save);
        masm.branchIfFalseBool(scratch, failure->label());

        // Load the reallocated elements pointer.
        masm.loadPtr(Address(obj, NativeObject::offsetOfElements()), scratch);

        masm.bind(&capacityOk);

        // We increment initLength after the callTypeUpdateIC call, to ensure
        // the type update code doesn't read uninitialized memory.
    } else {
        // Fail if index >= initLength.
        masm.spectreBoundsCheck32(index, initLength, spectreTemp, failure->label());
    }

    // Check if we have to convert a double element.
    Label noConversion;
    masm.branchTest32(Assembler::Zero, elementsFlags,
                      Imm32(ObjectElements::CONVERT_DOUBLE_ELEMENTS),
                      &noConversion);

    // We need to convert int32 values being stored into doubles. Note that
    // double arrays are only created by IonMonkey, so if we have no FP support
    // Ion is disabled and there should be no double arrays.
    if (cx_->runtime()->jitSupportsFloatingPoint) {
        // It's fine to convert the value in place in Baseline. We can't do
        // this in Ion.
        masm.convertInt32ValueToDouble(val);
    } else {
        masm.assumeUnreachable("There shouldn't be double arrays when there is no FP support.");
    }

    masm.bind(&noConversion);

    // Call the type update IC. After this everything must be infallible as we
    // don't save all registers here.
    LiveGeneralRegisterSet saveRegs;
    saveRegs.add(obj);
    saveRegs.add(index);
    saveRegs.add(val);
    if (!callTypeUpdateIC(obj, val, scratch, saveRegs))
        return false;

    // Reload obj->elements as callTypeUpdateIC used the scratch register.
    masm.loadPtr(Address(obj, NativeObject::offsetOfElements()), scratch);

    Label doStore;
    if (handleAdd) {
        // If index == initLength, increment initLength.
        Label inBounds;
        masm.branch32(Assembler::NotEqual, initLength, index, &inBounds);

        // Increment initLength.
        masm.add32(Imm32(1), initLength);

        // If length is now <= index, increment length too.
        Label skipIncrementLength;
        Address length(scratch, ObjectElements::offsetOfLength());
        masm.branch32(Assembler::Above, length, index, &skipIncrementLength);
        masm.add32(Imm32(1), length);
        masm.bind(&skipIncrementLength);

        // Skip EmitPreBarrier as the memory is uninitialized.
        masm.jump(&doStore);

        masm.bind(&inBounds);
    }

    EmitPreBarrier(masm, element, MIRType::Value);

    masm.bind(&doStore);
    masm.storeValue(val, element);

    emitPostBarrierElement(obj, val, scratch, index);
    return true;
}

bool
BaselineCacheIRCompiler::emitArrayPush()
{
    ObjOperandId objId = reader.objOperandId();
    ValOperandId rhsId = reader.valOperandId();

    // Allocate the fixed registers first. These need to be fixed for
    // callTypeUpdateIC.
    AutoScratchRegister scratch(allocator, masm, R1.scratchReg());
    ValueOperand val = allocator.useFixedValueRegister(masm, rhsId, R0);

    Register obj = allocator.useRegister(masm, objId);
    AutoScratchRegister scratchLength(allocator, masm);

    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;

    // Load obj->elements in scratch.
    masm.loadPtr(Address(obj, NativeObject::offsetOfElements()), scratch);

    Address elementsInitLength(scratch, ObjectElements::offsetOfInitializedLength());
    Address elementsLength(scratch, ObjectElements::offsetOfLength());
    Address elementsFlags(scratch, ObjectElements::offsetOfFlags());

    // Check for copy-on-write or frozen elements.
    masm.branchTest32(Assembler::NonZero, elementsFlags,
                      Imm32(ObjectElements::COPY_ON_WRITE |
                            ObjectElements::FROZEN),
                      failure->label());

    // Fail if length != initLength.
    masm.load32(elementsInitLength, scratchLength);
    masm.branch32(Assembler::NotEqual, elementsLength, scratchLength, failure->label());

    // If scratchLength < capacity, we can add a dense element inline. If not we
    // need to allocate more elements.
    Label capacityOk, allocElement;
    Address capacity(scratch, ObjectElements::offsetOfCapacity());
    masm.spectreBoundsCheck32(scratchLength, capacity, InvalidReg, &allocElement);
    masm.jump(&capacityOk);

    // Check for non-writable array length. We only have to do this if
    // index >= capacity.
    masm.bind(&allocElement);
    masm.branchTest32(Assembler::NonZero, elementsFlags,
                      Imm32(ObjectElements::NONWRITABLE_ARRAY_LENGTH),
                      failure->label());

    LiveRegisterSet save(GeneralRegisterSet::Volatile(), liveVolatileFloatRegs());
    save.takeUnchecked(scratch);
    masm.PushRegsInMask(save);

    masm.setupUnalignedABICall(scratch);
    masm.loadJSContext(scratch);
    masm.passABIArg(scratch);
    masm.passABIArg(obj);
    masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, NativeObject::addDenseElementDontReportOOM));
    masm.mov(ReturnReg, scratch);

    masm.PopRegsInMask(save);
    masm.branchIfFalseBool(scratch, failure->label());

    // Load the reallocated elements pointer.
    masm.loadPtr(Address(obj, NativeObject::offsetOfElements()), scratch);

    masm.bind(&capacityOk);

    // Check if we have to convert a double element.
    Label noConversion;
    masm.branchTest32(Assembler::Zero, elementsFlags,
                      Imm32(ObjectElements::CONVERT_DOUBLE_ELEMENTS),
                      &noConversion);

    // We need to convert int32 values being stored into doubles. Note that
    // double arrays are only created by IonMonkey, so if we have no FP support
    // Ion is disabled and there should be no double arrays.
    if (cx_->runtime()->jitSupportsFloatingPoint) {
        // It's fine to convert the value in place in Baseline. We can't do
        // this in Ion.
        masm.convertInt32ValueToDouble(val);
    } else {
        masm.assumeUnreachable("There shouldn't be double arrays when there is no FP support.");
    }

    masm.bind(&noConversion);

    // Call the type update IC. After this everything must be infallible as we
    // don't save all registers here.
    LiveGeneralRegisterSet saveRegs;
    saveRegs.add(obj);
    saveRegs.add(val);
    if (!callTypeUpdateIC(obj, val, scratch, saveRegs))
        return false;

    // Reload obj->elements as callTypeUpdateIC used the scratch register.
    masm.loadPtr(Address(obj, NativeObject::offsetOfElements()), scratch);

    // Increment initLength and length.
    masm.add32(Imm32(1), elementsInitLength);
    masm.load32(elementsLength, scratchLength);
    masm.add32(Imm32(1), elementsLength);

    // Store the value.
    BaseObjectElementIndex element(scratch, scratchLength);
    masm.storeValue(val, element);
    emitPostBarrierElement(obj, val, scratch, scratchLength);

    // Return value is new length.
    masm.add32(Imm32(1), scratchLength);
    masm.tagValue(JSVAL_TYPE_INT32, scratchLength, val);

    return true;
}

bool
BaselineCacheIRCompiler::emitStoreTypedElement()
{
    Register obj = allocator.useRegister(masm, reader.objOperandId());
    Register index = allocator.useRegister(masm, reader.int32OperandId());
    ValueOperand val = allocator.useValueRegister(masm, reader.valOperandId());

    TypedThingLayout layout = reader.typedThingLayout();
    Scalar::Type type = reader.scalarType();
    bool handleOOB = reader.readBool();

    AutoScratchRegister scratch1(allocator, masm);

    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;

    // Bounds check.
    Label done;
    LoadTypedThingLength(masm, layout, obj, scratch1);

    // Unfortunately we don't have more registers available on x86, so use
    // InvalidReg and emit slightly slower code on x86.
    Register spectreTemp = InvalidReg;
    masm.spectreBoundsCheck32(index, scratch1, spectreTemp, handleOOB ? &done : failure->label());

    // Load the elements vector.
    LoadTypedThingData(masm, layout, obj, scratch1);

    BaseIndex dest(scratch1, index, ScaleFromElemWidth(Scalar::byteSize(type)));

    // Use ICStubReg as second scratch register. TODO: consider doing the RHS
    // type check/conversion as a separate IR instruction so we can simplify
    // this.
    Register scratch2 = ICStubReg;
    masm.push(scratch2);

    Label fail;
    StoreToTypedArray(cx_, masm, type, val, dest, scratch2, &fail);
    masm.pop(scratch2);
    masm.jump(&done);

    masm.bind(&fail);
    masm.pop(scratch2);
    masm.jump(failure->label());

    masm.bind(&done);
    return true;
}

typedef bool (*CallNativeSetterFn)(JSContext*, HandleFunction, HandleObject, HandleValue);
static const VMFunction CallNativeSetterInfo =
    FunctionInfo<CallNativeSetterFn>(CallNativeSetter, "CallNativeSetter");

bool
BaselineCacheIRCompiler::emitCallNativeSetter()
{
    Register obj = allocator.useRegister(masm, reader.objOperandId());
    Address setterAddr(stubAddress(reader.stubOffset()));
    ValueOperand val = allocator.useValueRegister(masm, reader.valOperandId());

    AutoScratchRegister scratch(allocator, masm);

    allocator.discardStack(masm);

    AutoStubFrame stubFrame(*this);
    stubFrame.enter(masm, scratch);

    // Load the callee in the scratch register.
    masm.loadPtr(setterAddr, scratch);

    masm.Push(val);
    masm.Push(obj);
    masm.Push(scratch);

    if (!callVM(masm, CallNativeSetterInfo))
        return false;

    stubFrame.leave(masm);
    return true;
}

bool
BaselineCacheIRCompiler::emitCallScriptedSetter()
{
    AutoScratchRegister scratch1(allocator, masm);
    AutoScratchRegister scratch2(allocator, masm);

    Register obj = allocator.useRegister(masm, reader.objOperandId());
    Address setterAddr(stubAddress(reader.stubOffset()));
    ValueOperand val = allocator.useValueRegister(masm, reader.valOperandId());

    // First, ensure our setter is non-lazy. This also loads the callee in
    // scratch1.
    {
        FailurePath* failure;
        if (!addFailurePath(&failure))
            return false;

        masm.loadPtr(setterAddr, scratch1);
        masm.branchIfFunctionHasNoJitEntry(scratch1, /* constructing */ false, failure->label());
    }

    allocator.discardStack(masm);

    AutoStubFrame stubFrame(*this);
    stubFrame.enter(masm, scratch2);

    // Align the stack such that the JitFrameLayout is aligned on
    // JitStackAlignment.
    masm.alignJitStackBasedOnNArgs(1);

    // Setter is called with 1 argument, and |obj| as thisv. Note that we use
    // Push, not push, so that callJit will align the stack properly on ARM.
    masm.Push(val);
    masm.Push(TypedOrValueRegister(MIRType::Object, AnyRegister(obj)));

    // Now that the object register is no longer needed, use it as second
    // scratch.
    EmitBaselineCreateStubFrameDescriptor(masm, scratch2, JitFrameLayout::Size());
    masm.Push(Imm32(1));  // ActualArgc

    // Push callee.
    masm.Push(scratch1);

    // Push frame descriptor.
    masm.Push(scratch2);

    // Load callee->nargs in scratch2 and the JIT code in scratch.
    Label noUnderflow;
    masm.load16ZeroExtend(Address(scratch1, JSFunction::offsetOfNargs()), scratch2);
    masm.loadJitCodeRaw(scratch1, scratch1);

    // Handle arguments underflow.
    masm.branch32(Assembler::BelowOrEqual, scratch2, Imm32(1), &noUnderflow);
    {
        // Call the arguments rectifier.
        TrampolinePtr argumentsRectifier = cx_->runtime()->jitRuntime()->getArgumentsRectifier();
        masm.movePtr(argumentsRectifier, scratch1);
    }

    masm.bind(&noUnderflow);
    masm.callJit(scratch1);

    stubFrame.leave(masm, true);
    return true;
}

typedef bool (*SetArrayLengthFn)(JSContext*, HandleObject, HandleValue, bool);
static const VMFunction SetArrayLengthInfo =
    FunctionInfo<SetArrayLengthFn>(SetArrayLength, "SetArrayLength");

bool
BaselineCacheIRCompiler::emitCallSetArrayLength()
{
    Register obj = allocator.useRegister(masm, reader.objOperandId());
    bool strict = reader.readBool();
    ValueOperand val = allocator.useValueRegister(masm, reader.valOperandId());

    AutoScratchRegister scratch(allocator, masm);

    allocator.discardStack(masm);

    AutoStubFrame stubFrame(*this);
    stubFrame.enter(masm, scratch);

    masm.Push(Imm32(strict));
    masm.Push(val);
    masm.Push(obj);

    if (!callVM(masm, SetArrayLengthInfo))
        return false;

    stubFrame.leave(masm);
    return true;
}

typedef bool (*ProxySetPropertyFn)(JSContext*, HandleObject, HandleId, HandleValue, bool);
static const VMFunction ProxySetPropertyInfo =
    FunctionInfo<ProxySetPropertyFn>(ProxySetProperty, "ProxySetProperty");

bool
BaselineCacheIRCompiler::emitCallProxySet()
{
    Register obj = allocator.useRegister(masm, reader.objOperandId());
    ValueOperand val = allocator.useValueRegister(masm, reader.valOperandId());
    Address idAddr(stubAddress(reader.stubOffset()));
    bool strict = reader.readBool();

    AutoScratchRegister scratch(allocator, masm);

    allocator.discardStack(masm);

    AutoStubFrame stubFrame(*this);
    stubFrame.enter(masm, scratch);

    // Load the jsid in the scratch register.
    masm.loadPtr(idAddr, scratch);

    masm.Push(Imm32(strict));
    masm.Push(val);
    masm.Push(scratch);
    masm.Push(obj);

    if (!callVM(masm, ProxySetPropertyInfo))
        return false;

    stubFrame.leave(masm);
    return true;
}

typedef bool (*ProxySetPropertyByValueFn)(JSContext*, HandleObject, HandleValue, HandleValue, bool);
static const VMFunction ProxySetPropertyByValueInfo =
    FunctionInfo<ProxySetPropertyByValueFn>(ProxySetPropertyByValue, "ProxySetPropertyByValue");

bool
BaselineCacheIRCompiler::emitCallProxySetByValue()
{
    Register obj = allocator.useRegister(masm, reader.objOperandId());
    ValueOperand idVal = allocator.useValueRegister(masm, reader.valOperandId());
    ValueOperand val = allocator.useValueRegister(masm, reader.valOperandId());
    bool strict = reader.readBool();

    allocator.discardStack(masm);

    // We need a scratch register but we don't have any registers available on
    // x86, so temporarily store |obj| in the frame's scratch slot.
    int scratchOffset = BaselineFrame::reverseOffsetOfScratchValue();
    masm.storePtr(obj, Address(BaselineFrameReg, scratchOffset));

    AutoStubFrame stubFrame(*this);
    stubFrame.enter(masm, obj);

    // Restore |obj|. Because we entered a stub frame we first have to load
    // the original frame pointer.
    masm.loadPtr(Address(BaselineFrameReg, 0), obj);
    masm.loadPtr(Address(obj, scratchOffset), obj);

    masm.Push(Imm32(strict));
    masm.Push(val);
    masm.Push(idVal);
    masm.Push(obj);

    if (!callVM(masm, ProxySetPropertyByValueInfo))
        return false;

    stubFrame.leave(masm);
    return true;
}

bool
BaselineCacheIRCompiler::emitMegamorphicSetElement()
{
    Register obj = allocator.useRegister(masm, reader.objOperandId());
    ValueOperand idVal = allocator.useValueRegister(masm, reader.valOperandId());
    ValueOperand val = allocator.useValueRegister(masm, reader.valOperandId());
    bool strict = reader.readBool();

    allocator.discardStack(masm);

    // We need a scratch register but we don't have any registers available on
    // x86, so temporarily store |obj| in the frame's scratch slot.
    int scratchOffset = BaselineFrame::reverseOffsetOfScratchValue();
    masm.storePtr(obj, Address(BaselineFrameReg, scratchOffset));

    AutoStubFrame stubFrame(*this);
    stubFrame.enter(masm, obj);

    // Restore |obj|. Because we entered a stub frame we first have to load
    // the original frame pointer.
    masm.loadPtr(Address(BaselineFrameReg, 0), obj);
    masm.loadPtr(Address(obj, scratchOffset), obj);

    masm.Push(Imm32(strict));
    masm.Push(TypedOrValueRegister(MIRType::Object, AnyRegister(obj)));
    masm.Push(val);
    masm.Push(idVal);
    masm.Push(obj);

    if (!callVM(masm, SetObjectElementInfo))
        return false;

    stubFrame.leave(masm);
    return true;
}

bool
BaselineCacheIRCompiler::emitTypeMonitorResult()
{
    allocator.discardStack(masm);
    EmitEnterTypeMonitorIC(masm);
    return true;
}

bool
BaselineCacheIRCompiler::emitReturnFromIC()
{
    allocator.discardStack(masm);
    EmitReturnFromIC(masm);
    return true;
}

bool
BaselineCacheIRCompiler::emitLoadObject()
{
    Register reg = allocator.defineRegister(masm, reader.objOperandId());
    masm.loadPtr(stubAddress(reader.stubOffset()), reg);
    return true;
}

bool
BaselineCacheIRCompiler::emitLoadStackValue()
{
    ValueOperand val = allocator.defineValueRegister(masm, reader.valOperandId());
    Address addr = allocator.addressOf(masm, BaselineFrameSlot(reader.uint32Immediate()));
    masm.loadValue(addr, val);
    return true;
}

bool
BaselineCacheIRCompiler::emitGuardAndGetIterator()
{
    Register obj = allocator.useRegister(masm, reader.objOperandId());

    AutoScratchRegister scratch1(allocator, masm);
    AutoScratchRegister scratch2(allocator, masm);
    AutoScratchRegister niScratch(allocator, masm);

    Address iterAddr(stubAddress(reader.stubOffset()));
    Address enumeratorsAddr(stubAddress(reader.stubOffset()));

    Register output = allocator.defineRegister(masm, reader.objOperandId());

    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;

    // Load our PropertyIteratorObject* and its NativeIterator.
    masm.loadPtr(iterAddr, output);
    masm.loadObjPrivate(output, JSObject::ITER_CLASS_NFIXED_SLOTS, niScratch);

    // Ensure the |active| and |unreusable| bits are not set.
    masm.branchTest32(Assembler::NonZero, Address(niScratch, offsetof(NativeIterator, flags)),
                      Imm32(JSITER_ACTIVE|JSITER_UNREUSABLE), failure->label());

    // Pre-write barrier for store to 'obj'.
    Address iterObjAddr(niScratch, offsetof(NativeIterator, obj));
    EmitPreBarrier(masm, iterObjAddr, MIRType::Object);

    // Mark iterator as active.
    Address iterFlagsAddr(niScratch, offsetof(NativeIterator, flags));
    masm.storePtr(obj, iterObjAddr);
    masm.or32(Imm32(JSITER_ACTIVE), iterFlagsAddr);

    // Post-write barrier for stores to 'obj'.
    emitPostBarrierSlot(output, TypedOrValueRegister(MIRType::Object, AnyRegister(obj)), scratch1);

    // Chain onto the active iterator stack. Note that Baseline CacheIR stub
    // code is shared across compartments within a Zone, so we can't bake in
    // compartment->enumerators here.
    masm.loadPtr(enumeratorsAddr, scratch1);
    masm.loadPtr(Address(scratch1, 0), scratch1);
    emitRegisterEnumerator(scratch1, niScratch, scratch2);

    return true;
}

bool
BaselineCacheIRCompiler::emitGuardDOMExpandoMissingOrGuardShape()
{
    ValueOperand val = allocator.useValueRegister(masm, reader.valOperandId());
    AutoScratchRegister shapeScratch(allocator, masm);
    AutoScratchRegister objScratch(allocator, masm);
    Address shapeAddr(stubAddress(reader.stubOffset()));

    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;

    Label done;
    masm.branchTestUndefined(Assembler::Equal, val, &done);

    masm.debugAssertIsObject(val);
    masm.loadPtr(shapeAddr, shapeScratch);
    masm.unboxObject(val, objScratch);
    // The expando object is not used in this case, so we don't need Spectre
    // mitigations.
    masm.branchTestObjShapeNoSpectreMitigations(Assembler::NotEqual, objScratch, shapeScratch,
                                                failure->label());

    masm.bind(&done);
    return true;
}

bool
BaselineCacheIRCompiler::emitLoadDOMExpandoValueGuardGeneration()
{
    Register obj = allocator.useRegister(masm, reader.objOperandId());
    Address expandoAndGenerationAddr(stubAddress(reader.stubOffset()));
    Address generationAddr(stubAddress(reader.stubOffset()));

    AutoScratchRegister scratch(allocator, masm);
    ValueOperand output = allocator.defineValueRegister(masm, reader.valOperandId());

    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;

    masm.loadPtr(Address(obj, ProxyObject::offsetOfReservedSlots()), scratch);
    Address expandoAddr(scratch, detail::ProxyReservedSlots::offsetOfPrivateSlot());

    // Load the ExpandoAndGeneration* in the output scratch register and guard
    // it matches the proxy's ExpandoAndGeneration.
    masm.loadPtr(expandoAndGenerationAddr, output.scratchReg());
    masm.branchPrivatePtr(Assembler::NotEqual, expandoAddr, output.scratchReg(), failure->label());

    // Guard expandoAndGeneration->generation matches the expected generation.
    masm.branch64(Assembler::NotEqual,
                  Address(output.scratchReg(), ExpandoAndGeneration::offsetOfGeneration()),
                  generationAddr,
                  scratch, failure->label());

    // Load expandoAndGeneration->expando into the output Value register.
    masm.loadValue(Address(output.scratchReg(), ExpandoAndGeneration::offsetOfExpando()), output);
    return true;
}

bool
BaselineCacheIRCompiler::init(CacheKind kind)
{
    if (!allocator.init())
        return false;

    // Baseline ICs monitor values when needed, so returning doubles is fine.
    allowDoubleResult_.emplace(true);

    size_t numInputs = writer_.numInputOperands();

    // Baseline passes the first 2 inputs in R0/R1, other Values are stored on
    // the stack.
    size_t numInputsInRegs = std::min(numInputs, size_t(2));
    AllocatableGeneralRegisterSet available(ICStubCompiler::availableGeneralRegs(numInputsInRegs));

    switch (kind) {
      case CacheKind::GetIntrinsic:
        MOZ_ASSERT(numInputs == 0);
        break;
      case CacheKind::GetProp:
      case CacheKind::TypeOf:
      case CacheKind::GetIterator:
      case CacheKind::ToBool:
        MOZ_ASSERT(numInputs == 1);
        allocator.initInputLocation(0, R0);
        break;
      case CacheKind::Compare:
      case CacheKind::GetElem:
      case CacheKind::GetPropSuper:
      case CacheKind::SetProp:
      case CacheKind::In:
      case CacheKind::HasOwn:
      case CacheKind::InstanceOf:
        MOZ_ASSERT(numInputs == 2);
        allocator.initInputLocation(0, R0);
        allocator.initInputLocation(1, R1);
        break;
      case CacheKind::GetElemSuper:
        MOZ_ASSERT(numInputs == 3);
        allocator.initInputLocation(0, BaselineFrameSlot(0));
        allocator.initInputLocation(1, R0);
        allocator.initInputLocation(2, R1);
        break;
      case CacheKind::SetElem:
        MOZ_ASSERT(numInputs == 3);
        allocator.initInputLocation(0, R0);
        allocator.initInputLocation(1, R1);
        allocator.initInputLocation(2, BaselineFrameSlot(0));
        break;
      case CacheKind::GetName:
      case CacheKind::BindName:
        MOZ_ASSERT(numInputs == 1);
        allocator.initInputLocation(0, R0.scratchReg(), JSVAL_TYPE_OBJECT);
#if defined(JS_NUNBOX32)
        // availableGeneralRegs can't know that GetName/BindName is only using
        // the payloadReg and not typeReg on x86.
        available.add(R0.typeReg());
#endif
        break;
      case CacheKind::Call:
        MOZ_ASSERT(numInputs == 1);
        allocator.initInputLocation(0, R0.scratchReg(), JSVAL_TYPE_INT32);
#if defined(JS_NUNBOX32)
        // availableGeneralRegs can't know that Call is only using
        // the payloadReg and not typeReg on x86.
        available.add(R0.typeReg());
#endif
        break;
    }

    // Baseline doesn't allocate float registers so none of them are live.
    liveFloatRegs_ = LiveFloatRegisterSet(FloatRegisterSet());

    allocator.initAvailableRegs(available);
    outputUnchecked_.emplace(R0);
    return true;
}

static const size_t MaxOptimizedCacheIRStubs = 16;

ICStub*
js::jit::AttachBaselineCacheIRStub(JSContext* cx, const CacheIRWriter& writer,
                                   CacheKind kind, BaselineCacheIRStubKind stubKind,
                                   ICStubEngine engine, JSScript* outerScript,
                                   ICFallbackStub* stub, bool* attached)
{
    // We shouldn't GC or report OOM (or any other exception) here.
    AutoAssertNoPendingException aanpe(cx);
    JS::AutoCheckCannotGC nogc;

    MOZ_ASSERT(!*attached);

    if (writer.failed())
        return nullptr;

    // Just a sanity check: the caller should ensure we don't attach an
    // unlimited number of stubs.
    MOZ_ASSERT(stub->numOptimizedStubs() < MaxOptimizedCacheIRStubs);

    uint32_t stubDataOffset = 0;
    switch (stubKind) {
      case BaselineCacheIRStubKind::Monitored:
        stubDataOffset = sizeof(ICCacheIR_Monitored);
        break;
      case BaselineCacheIRStubKind::Regular:
        stubDataOffset = sizeof(ICCacheIR_Regular);
        break;
      case BaselineCacheIRStubKind::Updated:
        stubDataOffset = sizeof(ICCacheIR_Updated);
        break;
    }

    JitZone* jitZone = cx->zone()->jitZone();

    // Check if we already have JitCode for this stub.
    CacheIRStubInfo* stubInfo;
    CacheIRStubKey::Lookup lookup(kind, engine, writer.codeStart(), writer.codeLength());
    JitCode* code = jitZone->getBaselineCacheIRStubCode(lookup, &stubInfo);
    if (!code) {
        // We have to generate stub code.
        JitContext jctx(cx, nullptr);
        BaselineCacheIRCompiler comp(cx, writer, engine, stubDataOffset);
        if (!comp.init(kind))
            return nullptr;

        code = comp.compile();
        if (!code)
            return nullptr;

        // Allocate the shared CacheIRStubInfo. Note that the
        // putBaselineCacheIRStubCode call below will transfer ownership
        // to the stub code HashMap, so we don't have to worry about freeing
        // it below.
        MOZ_ASSERT(!stubInfo);
        stubInfo = CacheIRStubInfo::New(kind, engine, comp.makesGCCalls(), stubDataOffset, writer);
        if (!stubInfo)
            return nullptr;

        CacheIRStubKey key(stubInfo);
        if (!jitZone->putBaselineCacheIRStubCode(lookup, key, code))
            return nullptr;
    }

    MOZ_ASSERT(code);
    MOZ_ASSERT(stubInfo);
    MOZ_ASSERT(stubInfo->stubDataSize() == writer.stubDataSize());

    // Ensure we don't attach duplicate stubs. This can happen if a stub failed
    // for some reason and the IR generator doesn't check for exactly the same
    // conditions.
    for (ICStubConstIterator iter = stub->beginChainConst(); !iter.atEnd(); iter++) {
        bool updated = false;
        switch (stubKind) {
          case BaselineCacheIRStubKind::Regular: {
            if (!iter->isCacheIR_Regular())
                continue;
            auto otherStub = iter->toCacheIR_Regular();
            if (otherStub->stubInfo() != stubInfo)
                continue;
            if (!writer.stubDataEqualsMaybeUpdate(otherStub->stubDataStart(), &updated))
                continue;
            break;
          }
          case BaselineCacheIRStubKind::Monitored: {
            if (!iter->isCacheIR_Monitored())
                continue;
            auto otherStub = iter->toCacheIR_Monitored();
            if (otherStub->stubInfo() != stubInfo)
                continue;
            if (!writer.stubDataEqualsMaybeUpdate(otherStub->stubDataStart(), &updated))
                continue;
            break;
          }
          case BaselineCacheIRStubKind::Updated: {
            if (!iter->isCacheIR_Updated())
                continue;
            auto otherStub = iter->toCacheIR_Updated();
            if (otherStub->stubInfo() != stubInfo)
                continue;
            if (!writer.stubDataEqualsMaybeUpdate(otherStub->stubDataStart(), &updated))
                continue;
            break;
          }
        }

        // We found a stub that's exactly the same as the stub we're about to
        // attach. Just return nullptr, the caller should do nothing in this
        // case.
        if (updated)
            *attached = true;
        return nullptr;
    }

    // Time to allocate and attach a new stub.

    size_t bytesNeeded = stubInfo->stubDataOffset() + stubInfo->stubDataSize();

    ICStubSpace* stubSpace = ICStubCompiler::StubSpaceForStub(stubInfo->makesGCCalls(),
                                                              outerScript, engine);
    void* newStubMem = stubSpace->alloc(bytesNeeded);
    if (!newStubMem)
        return nullptr;

    switch (stubKind) {
      case BaselineCacheIRStubKind::Regular: {
        auto newStub = new(newStubMem) ICCacheIR_Regular(code, stubInfo);
        writer.copyStubData(newStub->stubDataStart());
        stub->addNewStub(newStub);
        *attached = true;
        return newStub;
      }
      case BaselineCacheIRStubKind::Monitored: {
        ICTypeMonitor_Fallback* typeMonitorFallback =
            stub->toMonitoredFallbackStub()->getFallbackMonitorStub(cx, outerScript);
        if (!typeMonitorFallback) {
            cx->recoverFromOutOfMemory();
            return nullptr;
        }
        ICStub* monitorStub = typeMonitorFallback->firstMonitorStub();
        auto newStub = new(newStubMem) ICCacheIR_Monitored(code, monitorStub, stubInfo);
        writer.copyStubData(newStub->stubDataStart());
        stub->addNewStub(newStub);
        *attached = true;
        return newStub;
      }
      case BaselineCacheIRStubKind::Updated: {
        auto newStub = new(newStubMem) ICCacheIR_Updated(code, stubInfo);
        if (!newStub->initUpdatingChain(cx, stubSpace)) {
            cx->recoverFromOutOfMemory();
            return nullptr;
        }
        writer.copyStubData(newStub->stubDataStart());
        stub->addNewStub(newStub);
        *attached = true;
        return newStub;
      }
    }

    MOZ_CRASH("Invalid kind");
}

uint8_t*
ICCacheIR_Regular::stubDataStart()
{
    return reinterpret_cast<uint8_t*>(this) + stubInfo_->stubDataOffset();
}

uint8_t*
ICCacheIR_Monitored::stubDataStart()
{
    return reinterpret_cast<uint8_t*>(this) + stubInfo_->stubDataOffset();
}

uint8_t*
ICCacheIR_Updated::stubDataStart()
{
    return reinterpret_cast<uint8_t*>(this) + stubInfo_->stubDataOffset();
}

/* static */ ICCacheIR_Regular*
ICCacheIR_Regular::Clone(JSContext* cx, ICStubSpace* space, ICStub* firstMonitorStub,
                         ICCacheIR_Regular& other)
{
    const CacheIRStubInfo* stubInfo = other.stubInfo();
    MOZ_ASSERT(stubInfo->makesGCCalls());

    size_t bytesNeeded = stubInfo->stubDataOffset() + stubInfo->stubDataSize();
    void* newStub = space->alloc(bytesNeeded);
    if (!newStub)
        return nullptr;

    ICCacheIR_Regular* res = new(newStub) ICCacheIR_Regular(other.jitCode(), stubInfo);
    stubInfo->copyStubData(&other, res);
    return res;
}


/* static */ ICCacheIR_Monitored*
ICCacheIR_Monitored::Clone(JSContext* cx, ICStubSpace* space, ICStub* firstMonitorStub,
                           ICCacheIR_Monitored& other)
{
    const CacheIRStubInfo* stubInfo = other.stubInfo();
    MOZ_ASSERT(stubInfo->makesGCCalls());

    size_t bytesNeeded = stubInfo->stubDataOffset() + stubInfo->stubDataSize();
    void* newStub = space->alloc(bytesNeeded);
    if (!newStub)
        return nullptr;

    ICCacheIR_Monitored* res = new(newStub) ICCacheIR_Monitored(other.jitCode(), firstMonitorStub,
                                                                stubInfo);
    stubInfo->copyStubData(&other, res);
    return res;
}

/* static */ ICCacheIR_Updated*
ICCacheIR_Updated::Clone(JSContext* cx, ICStubSpace* space, ICStub* firstMonitorStub,
                         ICCacheIR_Updated& other)
{
    const CacheIRStubInfo* stubInfo = other.stubInfo();
    MOZ_ASSERT(stubInfo->makesGCCalls());

    size_t bytesNeeded = stubInfo->stubDataOffset() + stubInfo->stubDataSize();
    void* newStub = space->alloc(bytesNeeded);
    if (!newStub)
        return nullptr;

    ICCacheIR_Updated* res = new(newStub) ICCacheIR_Updated(other.jitCode(), stubInfo);
    res->updateStubGroup() = other.updateStubGroup();
    res->updateStubId() = other.updateStubId();

    stubInfo->copyStubData(&other, res);
    return res;
}

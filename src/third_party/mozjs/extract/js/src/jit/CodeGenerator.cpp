/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/CodeGenerator.h"

#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"
#include "mozilla/Casting.h"
#include "mozilla/DebugOnly.h"
#include "mozilla/EnumeratedArray.h"
#include "mozilla/EnumeratedRange.h"
#include "mozilla/MathAlgorithms.h"
#include "mozilla/ScopeExit.h"
#include "mozilla/Unused.h"

#include <type_traits>

#include "jslibmath.h"
#include "jsmath.h"
#include "jsnum.h"

#include "builtin/Eval.h"
#include "builtin/RegExp.h"
#include "builtin/SelfHostingDefines.h"
#include "builtin/String.h"
#include "builtin/TypedObject.h"
#include "gc/Nursery.h"
#include "irregexp/NativeRegExpMacroAssembler.h"
#include "jit/AtomicOperations.h"
#include "jit/BaselineCompiler.h"
#include "jit/IonBuilder.h"
#include "jit/IonIC.h"
#include "jit/IonOptimizationLevels.h"
#include "jit/JitcodeMap.h"
#include "jit/JitSpewer.h"
#include "jit/Linker.h"
#include "jit/Lowering.h"
#include "jit/MIRGenerator.h"
#include "jit/MoveEmitter.h"
#include "jit/RangeAnalysis.h"
#include "jit/SharedICHelpers.h"
#include "jit/StackSlotAllocator.h"
#include "util/Unicode.h"
#include "vm/AsyncFunction.h"
#include "vm/AsyncIteration.h"
#include "vm/MatchPairs.h"
#include "vm/RegExpObject.h"
#include "vm/RegExpStatics.h"
#include "vm/StringType.h"
#include "vm/TraceLogging.h"
#include "vm/TypedArrayObject.h"
#include "vtune/VTuneWrapper.h"

#include "jsboolinlines.h"

#include "jit/MacroAssembler-inl.h"
#include "jit/shared/CodeGenerator-shared-inl.h"
#include "jit/shared/Lowering-shared-inl.h"
#include "vm/Interpreter-inl.h"

using namespace js;
using namespace js::jit;

using mozilla::AssertedCast;
using mozilla::DebugOnly;
using mozilla::FloatingPoint;
using mozilla::Maybe;
using mozilla::NegativeInfinity;
using mozilla::PositiveInfinity;
using JS::GenericNaN;

namespace js {
namespace jit {

class OutOfLineICFallback : public OutOfLineCodeBase<CodeGenerator>
{
  private:
    LInstruction* lir_;
    size_t cacheIndex_;
    size_t cacheInfoIndex_;

  public:
    OutOfLineICFallback(LInstruction* lir, size_t cacheIndex, size_t cacheInfoIndex)
      : lir_(lir),
        cacheIndex_(cacheIndex),
        cacheInfoIndex_(cacheInfoIndex)
    { }

    void bind(MacroAssembler* masm) override {
        // The binding of the initial jump is done in
        // CodeGenerator::visitOutOfLineICFallback.
    }

    size_t cacheIndex() const {
        return cacheIndex_;
    }
    size_t cacheInfoIndex() const {
        return cacheInfoIndex_;
    }
    LInstruction* lir() const {
        return lir_;
    }

    void accept(CodeGenerator* codegen) override {
        codegen->visitOutOfLineICFallback(this);
    }
};

void
CodeGeneratorShared::addIC(LInstruction* lir, size_t cacheIndex)
{
    if (cacheIndex == SIZE_MAX) {
        masm.setOOM();
        return;
    }

    DataPtr<IonIC> cache(this, cacheIndex);
    MInstruction* mir = lir->mirRaw()->toInstruction();
    if (mir->resumePoint()) {
        cache->setScriptedLocation(mir->block()->info().script(),
                                   mir->resumePoint()->pc());
    } else {
        cache->setIdempotent();
    }

    Register temp = cache->scratchRegisterForEntryJump();
    icInfo_.back().icOffsetForJump = masm.movWithPatch(ImmWord(-1), temp);
    masm.jump(Address(temp, 0));

    MOZ_ASSERT(!icInfo_.empty());

    OutOfLineICFallback* ool = new(alloc()) OutOfLineICFallback(lir, cacheIndex, icInfo_.length() - 1);
    addOutOfLineCode(ool, mir);

    masm.bind(ool->rejoin());
    cache->setRejoinLabel(CodeOffset(ool->rejoin()->offset()));
}

typedef bool (*IonGetPropertyICFn)(JSContext*, HandleScript, IonGetPropertyIC*, HandleValue, HandleValue,
                                   MutableHandleValue);
static const VMFunction IonGetPropertyICInfo =
    FunctionInfo<IonGetPropertyICFn>(IonGetPropertyIC::update, "IonGetPropertyIC::update");

typedef bool (*IonSetPropertyICFn)(JSContext*, HandleScript, IonSetPropertyIC*, HandleObject,
                                   HandleValue, HandleValue);
static const VMFunction IonSetPropertyICInfo =
    FunctionInfo<IonSetPropertyICFn>(IonSetPropertyIC::update, "IonSetPropertyIC::update");

typedef bool (*IonGetPropSuperICFn)(JSContext*, HandleScript, IonGetPropSuperIC*, HandleObject, HandleValue,
                                    HandleValue, MutableHandleValue);
static const VMFunction IonGetPropSuperICInfo =
    FunctionInfo<IonGetPropSuperICFn>(IonGetPropSuperIC::update, "IonGetPropSuperIC::update");

typedef bool (*IonGetNameICFn)(JSContext*, HandleScript, IonGetNameIC*, HandleObject,
                               MutableHandleValue);
static const VMFunction IonGetNameICInfo =
    FunctionInfo<IonGetNameICFn>(IonGetNameIC::update, "IonGetNameIC::update");

typedef bool (*IonHasOwnICFn)(JSContext*, HandleScript, IonHasOwnIC*, HandleValue, HandleValue,
                              int32_t*);
static const VMFunction IonHasOwnICInfo =
    FunctionInfo<IonHasOwnICFn>(IonHasOwnIC::update, "IonHasOwnIC::update");

typedef JSObject* (*IonBindNameICFn)(JSContext*, HandleScript, IonBindNameIC*, HandleObject);
static const VMFunction IonBindNameICInfo =
    FunctionInfo<IonBindNameICFn>(IonBindNameIC::update, "IonBindNameIC::update");

typedef JSObject* (*IonGetIteratorICFn)(JSContext*, HandleScript, IonGetIteratorIC*, HandleValue);
static const VMFunction IonGetIteratorICInfo =
    FunctionInfo<IonGetIteratorICFn>(IonGetIteratorIC::update, "IonGetIteratorIC::update");

typedef bool (*IonInICFn)(JSContext*, HandleScript, IonInIC*, HandleValue, HandleObject, bool*);
static const VMFunction IonInICInfo =
    FunctionInfo<IonInICFn>(IonInIC::update, "IonInIC::update");


typedef bool (*IonInstanceOfICFn)(JSContext*, HandleScript, IonInstanceOfIC*,
                         HandleValue lhs, HandleObject rhs, bool* res);
static const VMFunction IonInstanceOfInfo =
    FunctionInfo<IonInstanceOfICFn>(IonInstanceOfIC::update, "IonInstanceOfIC::update");

void
CodeGenerator::visitOutOfLineICFallback(OutOfLineICFallback* ool)
{
    LInstruction* lir = ool->lir();
    size_t cacheIndex = ool->cacheIndex();
    size_t cacheInfoIndex = ool->cacheInfoIndex();

    DataPtr<IonIC> ic(this, cacheIndex);

    // Register the location of the OOL path in the IC.
    ic->setFallbackLabel(masm.labelForPatch());

    switch (ic->kind()) {
      case CacheKind::GetProp:
      case CacheKind::GetElem: {
        IonGetPropertyIC* getPropIC = ic->asGetPropertyIC();

        saveLive(lir);

        pushArg(getPropIC->id());
        pushArg(getPropIC->value());
        icInfo_[cacheInfoIndex].icOffsetForPush = pushArgWithPatch(ImmWord(-1));
        pushArg(ImmGCPtr(gen->info().script()));

        callVM(IonGetPropertyICInfo, lir);

        StoreValueTo(getPropIC->output()).generate(this);
        restoreLiveIgnore(lir, StoreValueTo(getPropIC->output()).clobbered());

        masm.jump(ool->rejoin());
        return;
      }
      case CacheKind::GetPropSuper:
      case CacheKind::GetElemSuper: {
        IonGetPropSuperIC* getPropSuperIC = ic->asGetPropSuperIC();

        saveLive(lir);

        pushArg(getPropSuperIC->id());
        pushArg(getPropSuperIC->receiver());
        pushArg(getPropSuperIC->object());
        icInfo_[cacheInfoIndex].icOffsetForPush = pushArgWithPatch(ImmWord(-1));
        pushArg(ImmGCPtr(gen->info().script()));

        callVM(IonGetPropSuperICInfo, lir);

        StoreValueTo(getPropSuperIC->output()).generate(this);
        restoreLiveIgnore(lir, StoreValueTo(getPropSuperIC->output()).clobbered());

        masm.jump(ool->rejoin());
        return;
      }
      case CacheKind::SetProp:
      case CacheKind::SetElem: {
        IonSetPropertyIC* setPropIC = ic->asSetPropertyIC();

        saveLive(lir);

        pushArg(setPropIC->rhs());
        pushArg(setPropIC->id());
        pushArg(setPropIC->object());
        icInfo_[cacheInfoIndex].icOffsetForPush = pushArgWithPatch(ImmWord(-1));
        pushArg(ImmGCPtr(gen->info().script()));

        callVM(IonSetPropertyICInfo, lir);

        restoreLive(lir);

        masm.jump(ool->rejoin());
        return;
      }
      case CacheKind::GetName: {
        IonGetNameIC* getNameIC = ic->asGetNameIC();

        saveLive(lir);

        pushArg(getNameIC->environment());
        icInfo_[cacheInfoIndex].icOffsetForPush = pushArgWithPatch(ImmWord(-1));
        pushArg(ImmGCPtr(gen->info().script()));

        callVM(IonGetNameICInfo, lir);

        StoreValueTo(getNameIC->output()).generate(this);
        restoreLiveIgnore(lir, StoreValueTo(getNameIC->output()).clobbered());

        masm.jump(ool->rejoin());
        return;
      }
      case CacheKind::BindName: {
        IonBindNameIC* bindNameIC = ic->asBindNameIC();

        saveLive(lir);

        pushArg(bindNameIC->environment());
        icInfo_[cacheInfoIndex].icOffsetForPush = pushArgWithPatch(ImmWord(-1));
        pushArg(ImmGCPtr(gen->info().script()));

        callVM(IonBindNameICInfo, lir);

        StoreRegisterTo(bindNameIC->output()).generate(this);
        restoreLiveIgnore(lir, StoreRegisterTo(bindNameIC->output()).clobbered());

        masm.jump(ool->rejoin());
        return;
      }
      case CacheKind::GetIterator: {
        IonGetIteratorIC* getIteratorIC = ic->asGetIteratorIC();

        saveLive(lir);

        pushArg(getIteratorIC->value());
        icInfo_[cacheInfoIndex].icOffsetForPush = pushArgWithPatch(ImmWord(-1));
        pushArg(ImmGCPtr(gen->info().script()));

        callVM(IonGetIteratorICInfo, lir);

        StoreRegisterTo(getIteratorIC->output()).generate(this);
        restoreLiveIgnore(lir, StoreRegisterTo(getIteratorIC->output()).clobbered());

        masm.jump(ool->rejoin());
        return;
      }
      case CacheKind::In: {
        IonInIC* inIC = ic->asInIC();

        saveLive(lir);

        pushArg(inIC->object());
        pushArg(inIC->key());
        icInfo_[cacheInfoIndex].icOffsetForPush = pushArgWithPatch(ImmWord(-1));
        pushArg(ImmGCPtr(gen->info().script()));

        callVM(IonInICInfo, lir);

        StoreRegisterTo(inIC->output()).generate(this);
        restoreLiveIgnore(lir, StoreRegisterTo(inIC->output()).clobbered());

        masm.jump(ool->rejoin());
        return;
      }
      case CacheKind::HasOwn: {
        IonHasOwnIC* hasOwnIC = ic->asHasOwnIC();

        saveLive(lir);

        pushArg(hasOwnIC->id());
        pushArg(hasOwnIC->value());
        icInfo_[cacheInfoIndex].icOffsetForPush = pushArgWithPatch(ImmWord(-1));
        pushArg(ImmGCPtr(gen->info().script()));

        callVM(IonHasOwnICInfo, lir);

        StoreRegisterTo(hasOwnIC->output()).generate(this);
        restoreLiveIgnore(lir, StoreRegisterTo(hasOwnIC->output()).clobbered());

        masm.jump(ool->rejoin());
        return;
      }
      case CacheKind::InstanceOf: {
        IonInstanceOfIC* hasInstanceOfIC = ic->asInstanceOfIC();

        saveLive(lir);

        pushArg(hasInstanceOfIC->rhs());
        pushArg(hasInstanceOfIC->lhs());
        icInfo_[cacheInfoIndex].icOffsetForPush = pushArgWithPatch(ImmWord(-1));
        pushArg(ImmGCPtr(gen->info().script()));

        callVM(IonInstanceOfInfo, lir);

        StoreRegisterTo(hasInstanceOfIC->output()).generate(this);
        restoreLiveIgnore(lir, StoreRegisterTo(hasInstanceOfIC->output()).clobbered());

        masm.jump(ool->rejoin());
        return;
      }
      case CacheKind::Call:
      case CacheKind::Compare:
      case CacheKind::TypeOf:
      case CacheKind::ToBool:
      case CacheKind::GetIntrinsic:
        MOZ_CRASH("Unsupported IC");
    }
    MOZ_CRASH();
}

StringObject*
MNewStringObject::templateObj() const
{
    return &templateObj_->as<StringObject>();
}

CodeGenerator::CodeGenerator(MIRGenerator* gen, LIRGraph* graph, MacroAssembler* masm)
  : CodeGeneratorSpecific(gen, graph, masm)
  , ionScriptLabels_(gen->alloc())
  , scriptCounts_(nullptr)
  , simdTemplatesToReadBarrier_(0)
  , compartmentStubsToReadBarrier_(0)
{
}

CodeGenerator::~CodeGenerator()
{
    js_delete(scriptCounts_);
}

typedef bool (*StringToNumberFn)(JSContext*, JSString*, double*);
static const VMFunction StringToNumberInfo =
    FunctionInfo<StringToNumberFn>(StringToNumber, "StringToNumber");

void
CodeGenerator::visitValueToInt32(LValueToInt32* lir)
{
    ValueOperand operand = ToValue(lir, LValueToInt32::Input);
    Register output = ToRegister(lir->output());
    FloatRegister temp = ToFloatRegister(lir->tempFloat());

    MDefinition* input;
    if (lir->mode() == LValueToInt32::NORMAL)
        input = lir->mirNormal()->input();
    else
        input = lir->mirTruncate()->input();

    Label fails;
    if (lir->mode() == LValueToInt32::TRUNCATE) {
        OutOfLineCode* oolDouble = oolTruncateDouble(temp, output, lir->mir());

        // We can only handle strings in truncation contexts, like bitwise
        // operations.
        Label* stringEntry;
        Label* stringRejoin;
        Register stringReg;
        if (input->mightBeType(MIRType::String)) {
            stringReg = ToRegister(lir->temp());
            OutOfLineCode* oolString = oolCallVM(StringToNumberInfo, lir, ArgList(stringReg),
                                                 StoreFloatRegisterTo(temp));
            stringEntry = oolString->entry();
            stringRejoin = oolString->rejoin();
        } else {
            stringReg = InvalidReg;
            stringEntry = nullptr;
            stringRejoin = nullptr;
        }

        masm.truncateValueToInt32(operand, input, stringEntry, stringRejoin, oolDouble->entry(),
                                  stringReg, temp, output, &fails);
        masm.bind(oolDouble->rejoin());
    } else {
        masm.convertValueToInt32(operand, input, temp, output, &fails,
                                 lir->mirNormal()->canBeNegativeZero(),
                                 lir->mirNormal()->conversion());
    }

    bailoutFrom(&fails, lir->snapshot());
}

void
CodeGenerator::visitValueToDouble(LValueToDouble* lir)
{
    MToDouble* mir = lir->mir();
    ValueOperand operand = ToValue(lir, LValueToDouble::Input);
    FloatRegister output = ToFloatRegister(lir->output());

    Label isDouble, isInt32, isBool, isNull, isUndefined, done;
    bool hasBoolean = false, hasNull = false, hasUndefined = false;

    {
        ScratchTagScope tag(masm, operand);
        masm.splitTagForTest(operand, tag);

        masm.branchTestDouble(Assembler::Equal, tag, &isDouble);
        masm.branchTestInt32(Assembler::Equal, tag, &isInt32);

        if (mir->conversion() != MToFPInstruction::NumbersOnly) {
            masm.branchTestBoolean(Assembler::Equal, tag, &isBool);
            masm.branchTestUndefined(Assembler::Equal, tag, &isUndefined);
            hasBoolean = true;
            hasUndefined = true;
            if (mir->conversion() != MToFPInstruction::NonNullNonStringPrimitives) {
                masm.branchTestNull(Assembler::Equal, tag, &isNull);
                hasNull = true;
            }
        }
    }

    bailout(lir->snapshot());

    if (hasNull) {
        masm.bind(&isNull);
        masm.loadConstantDouble(0.0, output);
        masm.jump(&done);
    }

    if (hasUndefined) {
        masm.bind(&isUndefined);
        masm.loadConstantDouble(GenericNaN(), output);
        masm.jump(&done);
    }

    if (hasBoolean) {
        masm.bind(&isBool);
        masm.boolValueToDouble(operand, output);
        masm.jump(&done);
    }

    masm.bind(&isInt32);
    masm.int32ValueToDouble(operand, output);
    masm.jump(&done);

    masm.bind(&isDouble);
    masm.unboxDouble(operand, output);
    masm.bind(&done);
}

void
CodeGenerator::visitValueToFloat32(LValueToFloat32* lir)
{
    MToFloat32* mir = lir->mir();
    ValueOperand operand = ToValue(lir, LValueToFloat32::Input);
    FloatRegister output = ToFloatRegister(lir->output());

    Label isDouble, isInt32, isBool, isNull, isUndefined, done;
    bool hasBoolean = false, hasNull = false, hasUndefined = false;

    {
        ScratchTagScope tag(masm, operand);
        masm.splitTagForTest(operand, tag);

        masm.branchTestDouble(Assembler::Equal, tag, &isDouble);
        masm.branchTestInt32(Assembler::Equal, tag, &isInt32);

        if (mir->conversion() != MToFPInstruction::NumbersOnly) {
            masm.branchTestBoolean(Assembler::Equal, tag, &isBool);
            masm.branchTestUndefined(Assembler::Equal, tag, &isUndefined);
            hasBoolean = true;
            hasUndefined = true;
            if (mir->conversion() != MToFPInstruction::NonNullNonStringPrimitives) {
                masm.branchTestNull(Assembler::Equal, tag, &isNull);
                hasNull = true;
            }
        }
    }

    bailout(lir->snapshot());

    if (hasNull) {
        masm.bind(&isNull);
        masm.loadConstantFloat32(0.0f, output);
        masm.jump(&done);
    }

    if (hasUndefined) {
        masm.bind(&isUndefined);
        masm.loadConstantFloat32(float(GenericNaN()), output);
        masm.jump(&done);
    }

    if (hasBoolean) {
        masm.bind(&isBool);
        masm.boolValueToFloat32(operand, output);
        masm.jump(&done);
    }

    masm.bind(&isInt32);
    masm.int32ValueToFloat32(operand, output);
    masm.jump(&done);

    masm.bind(&isDouble);
    // ARM and MIPS may not have a double register available if we've
    // allocated output as a float32.
#if defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_MIPS32)
    masm.unboxDouble(operand, ScratchDoubleReg);
    masm.convertDoubleToFloat32(ScratchDoubleReg, output);
#else
    masm.unboxDouble(operand, output);
    masm.convertDoubleToFloat32(output, output);
#endif
    masm.bind(&done);
}

void
CodeGenerator::visitInt32ToDouble(LInt32ToDouble* lir)
{
    masm.convertInt32ToDouble(ToRegister(lir->input()), ToFloatRegister(lir->output()));
}

void
CodeGenerator::visitFloat32ToDouble(LFloat32ToDouble* lir)
{
    masm.convertFloat32ToDouble(ToFloatRegister(lir->input()), ToFloatRegister(lir->output()));
}

void
CodeGenerator::visitDoubleToFloat32(LDoubleToFloat32* lir)
{
    masm.convertDoubleToFloat32(ToFloatRegister(lir->input()), ToFloatRegister(lir->output()));
}

void
CodeGenerator::visitInt32ToFloat32(LInt32ToFloat32* lir)
{
    masm.convertInt32ToFloat32(ToRegister(lir->input()), ToFloatRegister(lir->output()));
}

void
CodeGenerator::visitDoubleToInt32(LDoubleToInt32* lir)
{
    Label fail;
    FloatRegister input = ToFloatRegister(lir->input());
    Register output = ToRegister(lir->output());
    masm.convertDoubleToInt32(input, output, &fail, lir->mir()->canBeNegativeZero());
    bailoutFrom(&fail, lir->snapshot());
}

void
CodeGenerator::visitFloat32ToInt32(LFloat32ToInt32* lir)
{
    Label fail;
    FloatRegister input = ToFloatRegister(lir->input());
    Register output = ToRegister(lir->output());
    masm.convertFloat32ToInt32(input, output, &fail, lir->mir()->canBeNegativeZero());
    bailoutFrom(&fail, lir->snapshot());
}

void
CodeGenerator::emitOOLTestObject(Register objreg,
                                 Label* ifEmulatesUndefined,
                                 Label* ifDoesntEmulateUndefined,
                                 Register scratch)
{
    saveVolatile(scratch);
    masm.setupUnalignedABICall(scratch);
    masm.passABIArg(objreg);
    masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, js::EmulatesUndefined));
    masm.storeCallBoolResult(scratch);
    restoreVolatile(scratch);

    masm.branchIfTrueBool(scratch, ifEmulatesUndefined);
    masm.jump(ifDoesntEmulateUndefined);
}

// Base out-of-line code generator for all tests of the truthiness of an
// object, where the object might not be truthy.  (Recall that per spec all
// objects are truthy, but we implement the JSCLASS_EMULATES_UNDEFINED class
// flag to permit objects to look like |undefined| in certain contexts,
// including in object truthiness testing.)  We check truthiness inline except
// when we're testing it on a proxy (or if TI guarantees us that the specified
// object will never emulate |undefined|), in which case out-of-line code will
// call EmulatesUndefined for a conclusive answer.
class OutOfLineTestObject : public OutOfLineCodeBase<CodeGenerator>
{
    Register objreg_;
    Register scratch_;

    Label* ifEmulatesUndefined_;
    Label* ifDoesntEmulateUndefined_;

#ifdef DEBUG
    bool initialized() { return ifEmulatesUndefined_ != nullptr; }
#endif

  public:
    OutOfLineTestObject()
#ifdef DEBUG
      : ifEmulatesUndefined_(nullptr), ifDoesntEmulateUndefined_(nullptr)
#endif
    { }

    void accept(CodeGenerator* codegen) final {
        MOZ_ASSERT(initialized());
        codegen->emitOOLTestObject(objreg_, ifEmulatesUndefined_, ifDoesntEmulateUndefined_,
                                   scratch_);
    }

    // Specify the register where the object to be tested is found, labels to
    // jump to if the object is truthy or falsy, and a scratch register for
    // use in the out-of-line path.
    void setInputAndTargets(Register objreg, Label* ifEmulatesUndefined, Label* ifDoesntEmulateUndefined,
                            Register scratch)
    {
        MOZ_ASSERT(!initialized());
        MOZ_ASSERT(ifEmulatesUndefined);
        objreg_ = objreg;
        scratch_ = scratch;
        ifEmulatesUndefined_ = ifEmulatesUndefined;
        ifDoesntEmulateUndefined_ = ifDoesntEmulateUndefined;
    }
};

// A subclass of OutOfLineTestObject containing two extra labels, for use when
// the ifTruthy/ifFalsy labels are needed in inline code as well as out-of-line
// code.  The user should bind these labels in inline code, and specify them as
// targets via setInputAndTargets, as appropriate.
class OutOfLineTestObjectWithLabels : public OutOfLineTestObject
{
    Label label1_;
    Label label2_;

  public:
    OutOfLineTestObjectWithLabels() { }

    Label* label1() { return &label1_; }
    Label* label2() { return &label2_; }
};

void
CodeGenerator::testObjectEmulatesUndefinedKernel(Register objreg,
                                                 Label* ifEmulatesUndefined,
                                                 Label* ifDoesntEmulateUndefined,
                                                 Register scratch, OutOfLineTestObject* ool)
{
    ool->setInputAndTargets(objreg, ifEmulatesUndefined, ifDoesntEmulateUndefined, scratch);

    // Perform a fast-path check of the object's class flags if the object's
    // not a proxy.  Let out-of-line code handle the slow cases that require
    // saving registers, making a function call, and restoring registers.
    masm.branchIfObjectEmulatesUndefined(objreg, scratch, ool->entry(), ifEmulatesUndefined);
}

void
CodeGenerator::branchTestObjectEmulatesUndefined(Register objreg,
                                                 Label* ifEmulatesUndefined,
                                                 Label* ifDoesntEmulateUndefined,
                                                 Register scratch, OutOfLineTestObject* ool)
{
    MOZ_ASSERT(!ifDoesntEmulateUndefined->bound(),
               "ifDoesntEmulateUndefined will be bound to the fallthrough path");

    testObjectEmulatesUndefinedKernel(objreg, ifEmulatesUndefined, ifDoesntEmulateUndefined,
                                      scratch, ool);
    masm.bind(ifDoesntEmulateUndefined);
}

void
CodeGenerator::testObjectEmulatesUndefined(Register objreg,
                                           Label* ifEmulatesUndefined,
                                           Label* ifDoesntEmulateUndefined,
                                           Register scratch, OutOfLineTestObject* ool)
{
    testObjectEmulatesUndefinedKernel(objreg, ifEmulatesUndefined, ifDoesntEmulateUndefined,
                                      scratch, ool);
    masm.jump(ifDoesntEmulateUndefined);
}

void
CodeGenerator::testValueTruthyKernel(const ValueOperand& value,
                                     const LDefinition* scratch1, const LDefinition* scratch2,
                                     FloatRegister fr,
                                     Label* ifTruthy, Label* ifFalsy,
                                     OutOfLineTestObject* ool,
                                     MDefinition* valueMIR)
{
    // Count the number of possible type tags we might have, so we'll know when
    // we've checked them all and hence can avoid emitting a tag check for the
    // last one.  In particular, whenever tagCount is 1 that means we've tried
    // all but one of them already so we know exactly what's left based on the
    // mightBe* booleans.
    bool mightBeUndefined = valueMIR->mightBeType(MIRType::Undefined);
    bool mightBeNull = valueMIR->mightBeType(MIRType::Null);
    bool mightBeBoolean = valueMIR->mightBeType(MIRType::Boolean);
    bool mightBeInt32 = valueMIR->mightBeType(MIRType::Int32);
    bool mightBeObject = valueMIR->mightBeType(MIRType::Object);
    bool mightBeString = valueMIR->mightBeType(MIRType::String);
    bool mightBeSymbol = valueMIR->mightBeType(MIRType::Symbol);
    bool mightBeDouble = valueMIR->mightBeType(MIRType::Double);
    int tagCount = int(mightBeUndefined) + int(mightBeNull) +
        int(mightBeBoolean) + int(mightBeInt32) + int(mightBeObject) +
        int(mightBeString) + int(mightBeSymbol) + int(mightBeDouble);

    MOZ_ASSERT_IF(!valueMIR->emptyResultTypeSet(), tagCount > 0);

    // If we know we're null or undefined, we're definitely falsy, no
    // need to even check the tag.
    if (int(mightBeNull) + int(mightBeUndefined) == tagCount) {
        masm.jump(ifFalsy);
        return;
    }

    ScratchTagScope tag(masm, value);
    masm.splitTagForTest(value, tag);

    if (mightBeUndefined) {
        MOZ_ASSERT(tagCount > 1);
        masm.branchTestUndefined(Assembler::Equal, tag, ifFalsy);
        --tagCount;
    }

    if (mightBeNull) {
        MOZ_ASSERT(tagCount > 1);
        masm.branchTestNull(Assembler::Equal, tag, ifFalsy);
        --tagCount;
    }

    if (mightBeBoolean) {
        MOZ_ASSERT(tagCount != 0);
        Label notBoolean;
        if (tagCount != 1)
            masm.branchTestBoolean(Assembler::NotEqual, tag, &notBoolean);
        {
            ScratchTagScopeRelease _(&tag);
            masm.branchTestBooleanTruthy(false, value, ifFalsy);
        }
        if (tagCount != 1)
            masm.jump(ifTruthy);
        // Else just fall through to truthiness.
        masm.bind(&notBoolean);
        --tagCount;
    }

    if (mightBeInt32) {
        MOZ_ASSERT(tagCount != 0);
        Label notInt32;
        if (tagCount != 1)
            masm.branchTestInt32(Assembler::NotEqual, tag, &notInt32);
        {
            ScratchTagScopeRelease _(&tag);
            masm.branchTestInt32Truthy(false, value, ifFalsy);
        }
        if (tagCount != 1)
            masm.jump(ifTruthy);
        // Else just fall through to truthiness.
        masm.bind(&notInt32);
        --tagCount;
    }

    if (mightBeObject) {
        MOZ_ASSERT(tagCount != 0);
        if (ool) {
            Label notObject;

            if (tagCount != 1)
                masm.branchTestObject(Assembler::NotEqual, tag, &notObject);

            {
                ScratchTagScopeRelease _(&tag);
                Register objreg = masm.extractObject(value, ToRegister(scratch1));
                testObjectEmulatesUndefined(objreg, ifFalsy, ifTruthy, ToRegister(scratch2), ool);
            }

            masm.bind(&notObject);
        } else {
            if (tagCount != 1)
                masm.branchTestObject(Assembler::Equal, tag, ifTruthy);
            // Else just fall through to truthiness.
        }
        --tagCount;
    } else {
        MOZ_ASSERT(!ool,
                   "We better not have an unused OOL path, since the code generator will try to "
                   "generate code for it but we never set up its labels, which will cause null "
                   "derefs of those labels.");
    }

    if (mightBeString) {
        // Test if a string is non-empty.
        MOZ_ASSERT(tagCount != 0);
        Label notString;
        if (tagCount != 1)
            masm.branchTestString(Assembler::NotEqual, tag, &notString);
        {
            ScratchTagScopeRelease _(&tag);
            masm.branchTestStringTruthy(false, value, ifFalsy);
        }
        if (tagCount != 1)
            masm.jump(ifTruthy);
        // Else just fall through to truthiness.
        masm.bind(&notString);
        --tagCount;
    }

    if (mightBeSymbol) {
        // All symbols are truthy.
        MOZ_ASSERT(tagCount != 0);
        if (tagCount != 1)
            masm.branchTestSymbol(Assembler::Equal, tag, ifTruthy);
        // Else fall through to ifTruthy.
        --tagCount;
    }

    if (mightBeDouble) {
        MOZ_ASSERT(tagCount == 1);
        // If we reach here the value is a double.
        {
            ScratchTagScopeRelease _(&tag);
            masm.unboxDouble(value, fr);
            masm.branchTestDoubleTruthy(false, fr, ifFalsy);
        }
        --tagCount;
    }

    MOZ_ASSERT(tagCount == 0);

    // Fall through for truthy.
}

void
CodeGenerator::testValueTruthy(const ValueOperand& value,
                               const LDefinition* scratch1, const LDefinition* scratch2,
                               FloatRegister fr,
                               Label* ifTruthy, Label* ifFalsy,
                               OutOfLineTestObject* ool,
                               MDefinition* valueMIR)
{
    testValueTruthyKernel(value, scratch1, scratch2, fr, ifTruthy, ifFalsy, ool, valueMIR);
    masm.jump(ifTruthy);
}

void
CodeGenerator::visitTestOAndBranch(LTestOAndBranch* lir)
{
    MIRType inputType = lir->mir()->input()->type();
    MOZ_ASSERT(inputType == MIRType::ObjectOrNull || lir->mir()->operandMightEmulateUndefined(),
               "If the object couldn't emulate undefined, this should have been folded.");

    Label* truthy = getJumpLabelForBranch(lir->ifTruthy());
    Label* falsy = getJumpLabelForBranch(lir->ifFalsy());
    Register input = ToRegister(lir->input());

    if (lir->mir()->operandMightEmulateUndefined()) {
        if (inputType == MIRType::ObjectOrNull)
            masm.branchTestPtr(Assembler::Zero, input, input, falsy);

        OutOfLineTestObject* ool = new(alloc()) OutOfLineTestObject();
        addOutOfLineCode(ool, lir->mir());

        testObjectEmulatesUndefined(input, falsy, truthy, ToRegister(lir->temp()), ool);
    } else {
        MOZ_ASSERT(inputType == MIRType::ObjectOrNull);
        testZeroEmitBranch(Assembler::NotEqual, input, lir->ifTruthy(), lir->ifFalsy());
    }
}

void
CodeGenerator::visitTestVAndBranch(LTestVAndBranch* lir)
{
    OutOfLineTestObject* ool = nullptr;
    MDefinition* input = lir->mir()->input();
    // Unfortunately, it's possible that someone (e.g. phi elimination) switched
    // out our input after we did cacheOperandMightEmulateUndefined.  So we
    // might think it can emulate undefined _and_ know that it can't be an
    // object.
    if (lir->mir()->operandMightEmulateUndefined() && input->mightBeType(MIRType::Object)) {
        ool = new(alloc()) OutOfLineTestObject();
        addOutOfLineCode(ool, lir->mir());
    }

    Label* truthy = getJumpLabelForBranch(lir->ifTruthy());
    Label* falsy = getJumpLabelForBranch(lir->ifFalsy());

    testValueTruthy(ToValue(lir, LTestVAndBranch::Input),
                    lir->temp1(), lir->temp2(),
                    ToFloatRegister(lir->tempFloat()),
                    truthy, falsy, ool, input);
}

void
CodeGenerator::visitFunctionDispatch(LFunctionDispatch* lir)
{
    MFunctionDispatch* mir = lir->mir();
    Register input = ToRegister(lir->input());
    Label* lastLabel;
    size_t casesWithFallback;

    // Determine if the last case is fallback or an ordinary case.
    if (!mir->hasFallback()) {
        MOZ_ASSERT(mir->numCases() > 0);
        casesWithFallback = mir->numCases();
        lastLabel = skipTrivialBlocks(mir->getCaseBlock(mir->numCases() - 1))->lir()->label();
    } else {
        casesWithFallback = mir->numCases() + 1;
        lastLabel = skipTrivialBlocks(mir->getFallback())->lir()->label();
    }

    // Compare function pointers, except for the last case.
    for (size_t i = 0; i < casesWithFallback - 1; i++) {
        MOZ_ASSERT(i < mir->numCases());
        LBlock* target = skipTrivialBlocks(mir->getCaseBlock(i))->lir();
        if (ObjectGroup* funcGroup = mir->getCaseObjectGroup(i)) {
            masm.branchTestObjGroupUnsafe(Assembler::Equal, input, funcGroup, target->label());
        } else {
            JSFunction* func = mir->getCase(i);
            masm.branchPtr(Assembler::Equal, input, ImmGCPtr(func), target->label());
        }
    }

    // Jump to the last case.
    masm.jump(lastLabel);
}

void
CodeGenerator::visitObjectGroupDispatch(LObjectGroupDispatch* lir)
{
    MObjectGroupDispatch* mir = lir->mir();
    Register input = ToRegister(lir->input());
    Register temp = ToRegister(lir->temp());

    // Load the incoming ObjectGroup in temp.
    masm.loadObjGroupUnsafe(input, temp);

    // Compare ObjectGroups.
    MacroAssembler::BranchGCPtr lastBranch;
    LBlock* lastBlock = nullptr;
    InlinePropertyTable* propTable = mir->propTable();
    for (size_t i = 0; i < mir->numCases(); i++) {
        JSFunction* func = mir->getCase(i);
        LBlock* target = skipTrivialBlocks(mir->getCaseBlock(i))->lir();

        DebugOnly<bool> found = false;
        for (size_t j = 0; j < propTable->numEntries(); j++) {
            if (propTable->getFunction(j) != func)
                continue;

            if (lastBranch.isInitialized())
                lastBranch.emit(masm);

            ObjectGroup* group = propTable->getObjectGroup(j);
            lastBranch = MacroAssembler::BranchGCPtr(Assembler::Equal, temp, ImmGCPtr(group),
                                                     target->label());
            lastBlock = target;
            found = true;
        }
        MOZ_ASSERT(found);
    }

    // Jump to fallback block if we have an unknown ObjectGroup. If there's no
    // fallback block, we should have handled all cases.

    if (!mir->hasFallback()) {
        MOZ_ASSERT(lastBranch.isInitialized());
#ifdef DEBUG
        Label ok;
        lastBranch.relink(&ok);
        lastBranch.emit(masm);
        masm.assumeUnreachable("Unexpected ObjectGroup");
        masm.bind(&ok);
#endif
        if (!isNextBlock(lastBlock))
            masm.jump(lastBlock->label());
        return;
    }

    LBlock* fallback = skipTrivialBlocks(mir->getFallback())->lir();
    if (!lastBranch.isInitialized()) {
        if (!isNextBlock(fallback))
            masm.jump(fallback->label());
        return;
    }

    lastBranch.invertCondition();
    lastBranch.relink(fallback->label());
    lastBranch.emit(masm);

    if (!isNextBlock(lastBlock))
        masm.jump(lastBlock->label());
}

void
CodeGenerator::visitBooleanToString(LBooleanToString* lir)
{
    Register input = ToRegister(lir->input());
    Register output = ToRegister(lir->output());
    const JSAtomState& names = gen->runtime->names();
    Label true_, done;

    masm.branchTest32(Assembler::NonZero, input, input, &true_);
    masm.movePtr(ImmGCPtr(names.false_), output);
    masm.jump(&done);

    masm.bind(&true_);
    masm.movePtr(ImmGCPtr(names.true_), output);

    masm.bind(&done);
}

void
CodeGenerator::emitIntToString(Register input, Register output, Label* ool)
{
    masm.boundsCheck32PowerOfTwo(input, StaticStrings::INT_STATIC_LIMIT, ool);

    // Fast path for small integers.
    masm.movePtr(ImmPtr(&gen->runtime->staticStrings().intStaticTable), output);
    masm.loadPtr(BaseIndex(output, input, ScalePointer), output);
}

typedef JSFlatString* (*IntToStringFn)(JSContext*, int);
static const VMFunction IntToStringInfo =
    FunctionInfo<IntToStringFn>(Int32ToString<CanGC>, "Int32ToString");

void
CodeGenerator::visitIntToString(LIntToString* lir)
{
    Register input = ToRegister(lir->input());
    Register output = ToRegister(lir->output());

    OutOfLineCode* ool = oolCallVM(IntToStringInfo, lir, ArgList(input),
                                   StoreRegisterTo(output));

    emitIntToString(input, output, ool->entry());

    masm.bind(ool->rejoin());
}

typedef JSString* (*DoubleToStringFn)(JSContext*, double);
static const VMFunction DoubleToStringInfo =
    FunctionInfo<DoubleToStringFn>(NumberToString<CanGC>, "NumberToString");

void
CodeGenerator::visitDoubleToString(LDoubleToString* lir)
{
    FloatRegister input = ToFloatRegister(lir->input());
    Register temp = ToRegister(lir->tempInt());
    Register output = ToRegister(lir->output());

    OutOfLineCode* ool = oolCallVM(DoubleToStringInfo, lir, ArgList(input),
                                   StoreRegisterTo(output));

    // Try double to integer conversion and run integer to string code.
    masm.convertDoubleToInt32(input, temp, ool->entry(), true);
    emitIntToString(temp, output, ool->entry());

    masm.bind(ool->rejoin());
}

typedef JSString* (*PrimitiveToStringFn)(JSContext*, HandleValue);
static const VMFunction PrimitiveToStringInfo =
    FunctionInfo<PrimitiveToStringFn>(ToStringSlow, "ToStringSlow");

void
CodeGenerator::visitValueToString(LValueToString* lir)
{
    ValueOperand input = ToValue(lir, LValueToString::Input);
    Register output = ToRegister(lir->output());

    OutOfLineCode* ool = oolCallVM(PrimitiveToStringInfo, lir, ArgList(input),
                                   StoreRegisterTo(output));

    Label done;
    Register tag = masm.extractTag(input, output);
    const JSAtomState& names = gen->runtime->names();

    // String
    if (lir->mir()->input()->mightBeType(MIRType::String)) {
        Label notString;
        masm.branchTestString(Assembler::NotEqual, tag, &notString);
        masm.unboxString(input, output);
        masm.jump(&done);
        masm.bind(&notString);
    }

    // Integer
    if (lir->mir()->input()->mightBeType(MIRType::Int32)) {
        Label notInteger;
        masm.branchTestInt32(Assembler::NotEqual, tag, &notInteger);
        Register unboxed = ToTempUnboxRegister(lir->tempToUnbox());
        unboxed = masm.extractInt32(input, unboxed);
        emitIntToString(unboxed, output, ool->entry());
        masm.jump(&done);
        masm.bind(&notInteger);
    }

    // Double
    if (lir->mir()->input()->mightBeType(MIRType::Double)) {
        // Note: no fastpath. Need two extra registers and can only convert doubles
        // that fit integers and are smaller than StaticStrings::INT_STATIC_LIMIT.
        masm.branchTestDouble(Assembler::Equal, tag, ool->entry());
    }

    // Undefined
    if (lir->mir()->input()->mightBeType(MIRType::Undefined)) {
        Label notUndefined;
        masm.branchTestUndefined(Assembler::NotEqual, tag, &notUndefined);
        masm.movePtr(ImmGCPtr(names.undefined), output);
        masm.jump(&done);
        masm.bind(&notUndefined);
    }

    // Null
    if (lir->mir()->input()->mightBeType(MIRType::Null)) {
        Label notNull;
        masm.branchTestNull(Assembler::NotEqual, tag, &notNull);
        masm.movePtr(ImmGCPtr(names.null), output);
        masm.jump(&done);
        masm.bind(&notNull);
    }

    // Boolean
    if (lir->mir()->input()->mightBeType(MIRType::Boolean)) {
        Label notBoolean, true_;
        masm.branchTestBoolean(Assembler::NotEqual, tag, &notBoolean);
        masm.branchTestBooleanTruthy(true, input, &true_);
        masm.movePtr(ImmGCPtr(names.false_), output);
        masm.jump(&done);
        masm.bind(&true_);
        masm.movePtr(ImmGCPtr(names.true_), output);
        masm.jump(&done);
        masm.bind(&notBoolean);
    }

    // Object
    if (lir->mir()->input()->mightBeType(MIRType::Object)) {
        // Bail.
        MOZ_ASSERT(lir->mir()->fallible());
        Label bail;
        masm.branchTestObject(Assembler::Equal, tag, &bail);
        bailoutFrom(&bail, lir->snapshot());
    }

    // Symbol
    if (lir->mir()->input()->mightBeType(MIRType::Symbol)) {
        // Bail.
        MOZ_ASSERT(lir->mir()->fallible());
        Label bail;
        masm.branchTestSymbol(Assembler::Equal, tag, &bail);
        bailoutFrom(&bail, lir->snapshot());
    }

#ifdef DEBUG
    masm.assumeUnreachable("Unexpected type for MValueToString.");
#endif

    masm.bind(&done);
    masm.bind(ool->rejoin());
}

typedef JSObject* (*ToObjectFn)(JSContext*, HandleValue, bool);
static const VMFunction ToObjectInfo =
    FunctionInfo<ToObjectFn>(ToObjectSlow, "ToObjectSlow");

void
CodeGenerator::visitValueToObject(LValueToObject* lir)
{
    ValueOperand input = ToValue(lir, LValueToObject::Input);
    Register output = ToRegister(lir->output());

    OutOfLineCode* ool = oolCallVM(ToObjectInfo, lir, ArgList(input, Imm32(0)),
                                   StoreRegisterTo(output));

    masm.branchTestObject(Assembler::NotEqual, input, ool->entry());
    masm.unboxObject(input, output);

    masm.bind(ool->rejoin());
}

void
CodeGenerator::visitValueToObjectOrNull(LValueToObjectOrNull* lir)
{
    ValueOperand input = ToValue(lir, LValueToObjectOrNull::Input);
    Register output = ToRegister(lir->output());

    OutOfLineCode* ool = oolCallVM(ToObjectInfo, lir, ArgList(input, Imm32(0)),
                                   StoreRegisterTo(output));

    Label isObject;
    masm.branchTestObject(Assembler::Equal, input, &isObject);
    masm.branchTestNull(Assembler::NotEqual, input, ool->entry());

    masm.movePtr(ImmWord(0), output);
    masm.jump(ool->rejoin());

    masm.bind(&isObject);
    masm.unboxObject(input, output);

    masm.bind(ool->rejoin());
}

enum class FieldToBarrier {
    REGEXP_PENDING_INPUT,
    REGEXP_MATCHES_INPUT,
    DEPENDENT_STRING_BASE
};

static void
EmitStoreBufferMutation(MacroAssembler& masm, Register holder, FieldToBarrier field,
                        Register buffer,
                        LiveGeneralRegisterSet& liveVolatiles,
                        void (*fun)(js::gc::StoreBuffer*, js::gc::Cell**))
{
    Label callVM;
    Label exit;

    // Call into the VM to barrier the write. The only registers that need to
    // be preserved are those in liveVolatiles, so once they are saved on the
    // stack all volatile registers are available for use.
    masm.bind(&callVM);
    masm.PushRegsInMask(liveVolatiles);

    AllocatableGeneralRegisterSet regs(GeneralRegisterSet::Volatile());
    regs.takeUnchecked(buffer);
    regs.takeUnchecked(holder);
    Register addrReg = regs.takeAny();

    switch (field) {
      case FieldToBarrier::REGEXP_PENDING_INPUT:
        masm.computeEffectiveAddress(Address(holder, RegExpStatics::offsetOfPendingInput()), addrReg);
        break;

      case FieldToBarrier::REGEXP_MATCHES_INPUT:
        masm.computeEffectiveAddress(Address(holder, RegExpStatics::offsetOfMatchesInput()), addrReg);
        break;

      case FieldToBarrier::DEPENDENT_STRING_BASE:
        masm.leaNewDependentStringBase(holder, addrReg);
        break;
    }

    bool needExtraReg = !regs.hasAny<GeneralRegisterSet::DefaultType>();
    if (needExtraReg) {
        masm.push(holder);
        masm.setupUnalignedABICall(holder);
    } else {
        masm.setupUnalignedABICall(regs.takeAny());
    }
    masm.passABIArg(buffer);
    masm.passABIArg(addrReg);
    masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, fun), MoveOp::GENERAL,
                     CheckUnsafeCallWithABI::DontCheckOther);

    if (needExtraReg)
        masm.pop(holder);
    masm.PopRegsInMask(liveVolatiles);
    masm.bind(&exit);
}

// Warning: this function modifies prev and next.
static void
EmitPostWriteBarrierS(MacroAssembler& masm,
                      Register string, FieldToBarrier field,
                      Register prev, Register next,
                      LiveGeneralRegisterSet& liveVolatiles)
{
    Label exit;
    Label checkRemove, putCell;

    // if (next && (buffer = next->storeBuffer()))
    // but we never pass in nullptr for next.
    Register storebuffer = next;
    masm.loadStoreBuffer(next, storebuffer);
    masm.branchPtr(Assembler::Equal, storebuffer, ImmWord(0), &checkRemove);

    // if (prev && prev->storeBuffer())
    masm.branchPtr(Assembler::Equal, prev, ImmWord(0), &putCell);
    masm.loadStoreBuffer(prev, prev);
    masm.branchPtr(Assembler::NotEqual, prev, ImmWord(0), &exit);

    // buffer->putCell(cellp)
    masm.bind(&putCell);
    EmitStoreBufferMutation(masm, string, field, storebuffer, liveVolatiles,
                            JSString::addCellAddressToStoreBuffer);
    masm.jump(&exit);

    // if (prev && (buffer = prev->storeBuffer()))
    masm.bind(&checkRemove);
    masm.branchPtr(Assembler::Equal, prev, ImmWord(0), &exit);
    masm.loadStoreBuffer(prev, storebuffer);
    masm.branchPtr(Assembler::Equal, storebuffer, ImmWord(0), &exit);
    EmitStoreBufferMutation(masm, string, field, storebuffer, liveVolatiles,
                            JSString::removeCellAddressFromStoreBuffer);

    masm.bind(&exit);
}

typedef JSObject* (*CloneRegExpObjectFn)(JSContext*, Handle<RegExpObject*>);
static const VMFunction CloneRegExpObjectInfo =
    FunctionInfo<CloneRegExpObjectFn>(CloneRegExpObject, "CloneRegExpObject");

void
CodeGenerator::visitRegExp(LRegExp* lir)
{
    Register output = ToRegister(lir->output());
    Register temp = ToRegister(lir->temp());
    JSObject* templateObject = lir->mir()->source();

    OutOfLineCode *ool = oolCallVM(CloneRegExpObjectInfo, lir, ArgList(ImmGCPtr(lir->mir()->source())),
                                   StoreRegisterTo(output));
    if (lir->mir()->hasShared())
        masm.createGCObject(output, temp, templateObject, gc::DefaultHeap, ool->entry());
    else
        masm.jump(ool->entry());
    masm.bind(ool->rejoin());
}

// Amount of space to reserve on the stack when executing RegExps inline.
static const size_t RegExpReservedStack = sizeof(irregexp::InputOutputData)
                                        + sizeof(MatchPairs)
                                        + RegExpObject::MaxPairCount * sizeof(MatchPair);

static size_t
RegExpPairsVectorStartOffset(size_t inputOutputDataStartOffset)
{
    return inputOutputDataStartOffset + sizeof(irregexp::InputOutputData) + sizeof(MatchPairs);
}

static Address
RegExpPairCountAddress(MacroAssembler& masm, size_t inputOutputDataStartOffset)
{
    return Address(masm.getStackPointer(), inputOutputDataStartOffset
                                           + sizeof(irregexp::InputOutputData)
                                           + MatchPairs::offsetOfPairCount());
}

// Prepare an InputOutputData and optional MatchPairs which space has been
// allocated for on the stack, and try to execute a RegExp on a string input.
// If the RegExp was successfully executed and matched the input, fallthrough,
// otherwise jump to notFound or failure.
static bool
PrepareAndExecuteRegExp(JSContext* cx, MacroAssembler& masm, Register regexp, Register input,
                        Register lastIndex,
                        Register temp1, Register temp2, Register temp3,
                        size_t inputOutputDataStartOffset,
                        RegExpShared::CompilationMode mode,
                        Label* notFound, Label* failure)
{
    size_t matchPairsStartOffset = inputOutputDataStartOffset + sizeof(irregexp::InputOutputData);
    size_t pairsVectorStartOffset = RegExpPairsVectorStartOffset(inputOutputDataStartOffset);

    Address inputStartAddress(masm.getStackPointer(),
        inputOutputDataStartOffset + offsetof(irregexp::InputOutputData, inputStart));
    Address inputEndAddress(masm.getStackPointer(),
        inputOutputDataStartOffset + offsetof(irregexp::InputOutputData, inputEnd));
    Address matchesPointerAddress(masm.getStackPointer(),
        inputOutputDataStartOffset + offsetof(irregexp::InputOutputData, matches));
    Address startIndexAddress(masm.getStackPointer(),
        inputOutputDataStartOffset + offsetof(irregexp::InputOutputData, startIndex));
    Address endIndexAddress(masm.getStackPointer(),
        inputOutputDataStartOffset + offsetof(irregexp::InputOutputData, endIndex));
    Address matchResultAddress(masm.getStackPointer(),
        inputOutputDataStartOffset + offsetof(irregexp::InputOutputData, result));

    Address pairCountAddress = RegExpPairCountAddress(masm, inputOutputDataStartOffset);
    Address pairsPointerAddress(masm.getStackPointer(),
        matchPairsStartOffset + MatchPairs::offsetOfPairs());

    Address pairsVectorAddress(masm.getStackPointer(), pairsVectorStartOffset);

    RegExpStatics* res = GlobalObject::getRegExpStatics(cx, cx->global());
    if (!res)
        return false;
#ifdef JS_USE_LINK_REGISTER
    if (mode != RegExpShared::MatchOnly)
        masm.pushReturnAddress();
#endif
    if (mode == RegExpShared::Normal) {
        // First, fill in a skeletal MatchPairs instance on the stack. This will be
        // passed to the OOL stub in the caller if we aren't able to execute the
        // RegExp inline, and that stub needs to be able to determine whether the
        // execution finished successfully.
        masm.store32(Imm32(1), pairCountAddress);
        masm.store32(Imm32(-1), pairsVectorAddress);
        masm.computeEffectiveAddress(pairsVectorAddress, temp1);
        masm.storePtr(temp1, pairsPointerAddress);
    }

    // Check for a linear input string.
    masm.branchIfRopeOrExternal(input, temp1, failure);

    // Get the RegExpShared for the RegExp.
    masm.loadPtr(Address(regexp, NativeObject::getFixedSlotOffset(RegExpObject::PRIVATE_SLOT)), temp1);
    masm.branchPtr(Assembler::Equal, temp1, ImmWord(0), failure);

    // ES6 21.2.2.2 step 2.
    // See RegExp.cpp ExecuteRegExp for more detail.
    {
        Label done;

        masm.branchTest32(Assembler::Zero, Address(temp1, RegExpShared::offsetOfFlags()),
                          Imm32(UnicodeFlag), &done);

        // If input is latin1, there should not be surrogate pair.
        masm.branchLatin1String(input, &done);

        // Check if |lastIndex > 0 && lastIndex < input->length()|.
        // lastIndex should already have no sign here.
        masm.branchTest32(Assembler::Zero, lastIndex, lastIndex, &done);
        masm.loadStringLength(input, temp2);
        masm.branch32(Assembler::AboveOrEqual, lastIndex, temp2, &done);

        // Check if input[lastIndex] is trail surrogate.
        masm.loadStringChars(input, temp2, CharEncoding::TwoByte);
        masm.computeEffectiveAddress(BaseIndex(temp2, lastIndex, TimesTwo), temp3);
        masm.load16ZeroExtend(Address(temp3, 0), temp3);

        masm.branch32(Assembler::Below, temp3, Imm32(unicode::TrailSurrogateMin), &done);
        masm.branch32(Assembler::Above, temp3, Imm32(unicode::TrailSurrogateMax), &done);

        // Check if input[lastIndex-1] is lead surrogate.
        masm.move32(lastIndex, temp3);
        masm.sub32(Imm32(1), temp3);
        masm.computeEffectiveAddress(BaseIndex(temp2, temp3, TimesTwo), temp3);
        masm.load16ZeroExtend(Address(temp3, 0), temp3);

        masm.branch32(Assembler::Below, temp3, Imm32(unicode::LeadSurrogateMin), &done);
        masm.branch32(Assembler::Above, temp3, Imm32(unicode::LeadSurrogateMax), &done);

        // Move lastIndex to lead surrogate.
        masm.subPtr(Imm32(1), lastIndex);

        masm.bind(&done);
    }

    if (mode == RegExpShared::Normal) {
        // Don't handle RegExps with excessive parens.
        masm.load32(Address(temp1, RegExpShared::offsetOfParenCount()), temp2);
        masm.branch32(Assembler::AboveOrEqual, temp2, Imm32(RegExpObject::MaxPairCount), failure);

        // Fill in the paren count in the MatchPairs on the stack.
        masm.add32(Imm32(1), temp2);
        masm.store32(temp2, pairCountAddress);
    }

    // Load the code pointer for the type of input string we have, and compute
    // the input start/end pointers in the InputOutputData.
    Register codePointer = temp1;
    {
        masm.loadStringLength(input, temp3);

        Label isLatin1, done;
        masm.branchLatin1String(input, &isLatin1);
        {
            masm.loadStringChars(input, temp2, CharEncoding::TwoByte);
            masm.storePtr(temp2, inputStartAddress);
            masm.lshiftPtr(Imm32(1), temp3);
            masm.loadPtr(Address(temp1, RegExpShared::offsetOfTwoByteJitCode(mode)),
                         codePointer);
            masm.jump(&done);
        }
        masm.bind(&isLatin1);
        {
            masm.loadStringChars(input, temp2, CharEncoding::Latin1);
            masm.storePtr(temp2, inputStartAddress);
            masm.loadPtr(Address(temp1, RegExpShared::offsetOfLatin1JitCode(mode)),
                         codePointer);
        }
        masm.bind(&done);

        masm.addPtr(temp3, temp2);
        masm.storePtr(temp2, inputEndAddress);
    }

    // Check the RegExpShared has been compiled for this type of input.
    masm.branchPtr(Assembler::Equal, codePointer, ImmWord(0), failure);
    masm.loadPtr(Address(codePointer, JitCode::offsetOfCode()), codePointer);

    // Finish filling in the InputOutputData instance on the stack.
    if (mode == RegExpShared::Normal) {
        masm.computeEffectiveAddress(Address(masm.getStackPointer(), matchPairsStartOffset), temp2);
        masm.storePtr(temp2, matchesPointerAddress);
    } else {
        // Use InputOutputData.endIndex itself for output.
        masm.computeEffectiveAddress(endIndexAddress, temp2);
        masm.storePtr(temp2, endIndexAddress);
    }
    masm.storePtr(lastIndex, startIndexAddress);
    masm.store32(Imm32(0), matchResultAddress);

    // Save any volatile inputs.
    LiveGeneralRegisterSet volatileRegs;
    if (lastIndex.volatile_())
        volatileRegs.add(lastIndex);
    if (input.volatile_())
        volatileRegs.add(input);
    if (regexp.volatile_())
        volatileRegs.add(regexp);

#ifdef JS_TRACE_LOGGING
    if (TraceLogTextIdEnabled(TraceLogger_IrregexpExecute)) {
        masm.push(temp1);
        masm.loadTraceLogger(temp1);
        masm.tracelogStartId(temp1, TraceLogger_IrregexpExecute);
        masm.pop(temp1);
    }
#endif

    // Execute the RegExp.
    masm.computeEffectiveAddress(Address(masm.getStackPointer(), inputOutputDataStartOffset), temp2);
    masm.PushRegsInMask(volatileRegs);
    masm.setupUnalignedABICall(temp3);
    masm.passABIArg(temp2);
    masm.callWithABI(codePointer);
    masm.PopRegsInMask(volatileRegs);

#ifdef JS_TRACE_LOGGING
    if (TraceLogTextIdEnabled(TraceLogger_IrregexpExecute)) {
        masm.loadTraceLogger(temp1);
        masm.tracelogStopId(temp1, TraceLogger_IrregexpExecute);
    }
#endif

    Label success;
    masm.branch32(Assembler::Equal, matchResultAddress,
                  Imm32(RegExpRunStatus_Success_NotFound), notFound);
    masm.branch32(Assembler::Equal, matchResultAddress,
                  Imm32(RegExpRunStatus_Error), failure);

    // Lazily update the RegExpStatics.
    masm.movePtr(ImmPtr(res), temp1);

    Address pendingInputAddress(temp1, RegExpStatics::offsetOfPendingInput());
    Address matchesInputAddress(temp1, RegExpStatics::offsetOfMatchesInput());
    Address lazySourceAddress(temp1, RegExpStatics::offsetOfLazySource());
    Address lazyIndexAddress(temp1, RegExpStatics::offsetOfLazyIndex());

    masm.guardedCallPreBarrier(pendingInputAddress, MIRType::String);
    masm.guardedCallPreBarrier(matchesInputAddress, MIRType::String);
    masm.guardedCallPreBarrier(lazySourceAddress, MIRType::String);

    if (temp1.volatile_())
        volatileRegs.add(temp1);

    // Writing into RegExpStatics tenured memory; must post-barrier.
    masm.loadPtr(pendingInputAddress, temp2);
    masm.storePtr(input, pendingInputAddress);
    masm.movePtr(input, temp3);
    EmitPostWriteBarrierS(masm, temp1, FieldToBarrier::REGEXP_PENDING_INPUT,
                          temp2 /* prev */, temp3 /* next */, volatileRegs);

    masm.loadPtr(matchesInputAddress, temp2);
    masm.storePtr(input, matchesInputAddress);
    masm.movePtr(input, temp3);
    EmitPostWriteBarrierS(masm, temp1, FieldToBarrier::REGEXP_MATCHES_INPUT,
                          temp2 /* prev */, temp3 /* next */, volatileRegs);

    masm.storePtr(lastIndex, Address(temp1, RegExpStatics::offsetOfLazyIndex()));
    masm.store32(Imm32(1), Address(temp1, RegExpStatics::offsetOfPendingLazyEvaluation()));

    masm.loadPtr(Address(regexp, NativeObject::getFixedSlotOffset(RegExpObject::PRIVATE_SLOT)), temp2);
    masm.loadPtr(Address(temp2, RegExpShared::offsetOfSource()), temp3);
    masm.storePtr(temp3, lazySourceAddress);
    masm.load32(Address(temp2, RegExpShared::offsetOfFlags()), temp3);
    masm.store32(temp3, Address(temp1, RegExpStatics::offsetOfLazyFlags()));

    if (mode == RegExpShared::MatchOnly) {
        // endIndex is passed via temp3.
        masm.load32(endIndexAddress, temp3);
    }

    return true;
}

static void
CopyStringChars(MacroAssembler& masm, Register to, Register from, Register len,
                Register byteOpScratch, size_t fromWidth, size_t toWidth);

class CreateDependentString
{
    Register string_;
    Register temp_;
    Label* failure_;
    enum class FallbackKind : uint8_t {
        InlineString,
        FatInlineString,
        NotInlineString,
        Count
    };
    mozilla::EnumeratedArray<FallbackKind, FallbackKind::Count, Label> fallbacks_, joins_;

public:
    // Generate code that creates DependentString.
    // Caller should call generateFallback after masm.ret(), to generate
    // fallback path.
    void generate(MacroAssembler& masm, const JSAtomState& names,
                  bool latin1, Register string,
                  Register base, Register temp1, Register temp2,
                  BaseIndex startIndexAddress, BaseIndex limitIndexAddress,
                  bool stringsCanBeInNursery,
                  Label* failure);

    // Generate fallback path for creating DependentString.
    void generateFallback(MacroAssembler& masm, LiveRegisterSet regsToSave);
};

void
CreateDependentString::generate(MacroAssembler& masm, const JSAtomState& names,
                                bool latin1, Register string,
                                Register base, Register temp1, Register temp2,
                                BaseIndex startIndexAddress, BaseIndex limitIndexAddress,
                                bool stringsCanBeInNursery,
                                Label* failure)
{
    string_ = string;
    temp_ = temp2;
    failure_ = failure;

    // Compute the string length.
    masm.load32(startIndexAddress, temp2);
    masm.load32(limitIndexAddress, temp1);
    masm.sub32(temp2, temp1);

    Label done, nonEmpty;

    // Zero length matches use the empty string.
    masm.branchTest32(Assembler::NonZero, temp1, temp1, &nonEmpty);
    masm.movePtr(ImmGCPtr(names.empty), string);
    masm.jump(&done);

    masm.bind(&nonEmpty);

    Label notInline;

    int32_t maxInlineLength = latin1
                              ? (int32_t) JSFatInlineString::MAX_LENGTH_LATIN1
                              : (int32_t) JSFatInlineString::MAX_LENGTH_TWO_BYTE;
    masm.branch32(Assembler::Above, temp1, Imm32(maxInlineLength), &notInline);

    {
        // Make a thin or fat inline string.
        Label stringAllocated, fatInline;

        int32_t maxThinInlineLength = latin1
                                      ? (int32_t) JSThinInlineString::MAX_LENGTH_LATIN1
                                      : (int32_t) JSThinInlineString::MAX_LENGTH_TWO_BYTE;
        masm.branch32(Assembler::Above, temp1, Imm32(maxThinInlineLength), &fatInline);

        int32_t thinFlags = (latin1 ? JSString::LATIN1_CHARS_BIT : 0) | JSString::INIT_THIN_INLINE_FLAGS;
        masm.newGCString(string, temp2, &fallbacks_[FallbackKind::InlineString], stringsCanBeInNursery);
        masm.bind(&joins_[FallbackKind::InlineString]);
        masm.store32(Imm32(thinFlags), Address(string, JSString::offsetOfFlags()));
        masm.jump(&stringAllocated);

        masm.bind(&fatInline);

        int32_t fatFlags = (latin1 ? JSString::LATIN1_CHARS_BIT : 0) | JSString::INIT_FAT_INLINE_FLAGS;
        masm.newGCFatInlineString(string, temp2, &fallbacks_[FallbackKind::FatInlineString], stringsCanBeInNursery);
        masm.bind(&joins_[FallbackKind::FatInlineString]);
        masm.store32(Imm32(fatFlags), Address(string, JSString::offsetOfFlags()));

        masm.bind(&stringAllocated);
        masm.store32(temp1, Address(string, JSString::offsetOfLength()));

        masm.push(string);
        masm.push(base);

        // Adjust the start index address for the above pushes.
        MOZ_ASSERT(startIndexAddress.base == masm.getStackPointer());
        BaseIndex newStartIndexAddress = startIndexAddress;
        newStartIndexAddress.offset += 2 * sizeof(void*);

        // Load chars pointer for the new string.
        masm.loadInlineStringCharsForStore(string, string);

        // Load the source characters pointer.
        masm.loadStringChars(base, temp2,
                             latin1 ? CharEncoding::Latin1 : CharEncoding::TwoByte);
        masm.load32(newStartIndexAddress, base);
        if (latin1)
            masm.addPtr(temp2, base);
        else
            masm.computeEffectiveAddress(BaseIndex(temp2, base, TimesTwo), base);

        CopyStringChars(masm, string, base, temp1, temp2, latin1 ? 1 : 2, latin1 ? 1 : 2);

        // Null-terminate.
        if (latin1)
            masm.store8(Imm32(0), Address(string, 0));
        else
            masm.store16(Imm32(0), Address(string, 0));

        masm.pop(base);
        masm.pop(string);
    }

    masm.jump(&done);
    masm.bind(&notInline);

    {
        // Make a dependent string.
        int32_t flags = (latin1 ? JSString::LATIN1_CHARS_BIT : 0) | JSString::DEPENDENT_FLAGS;

        masm.newGCString(string, temp2, &fallbacks_[FallbackKind::NotInlineString], stringsCanBeInNursery);
        // Warning: string may be tenured (if the fallback case is hit), so
        // stores into it must be post barriered.
        masm.bind(&joins_[FallbackKind::NotInlineString]);
        masm.store32(Imm32(flags), Address(string, JSString::offsetOfFlags()));
        masm.store32(temp1, Address(string, JSString::offsetOfLength()));

        masm.loadNonInlineStringChars(base, temp1,
                                      latin1 ? CharEncoding::Latin1 : CharEncoding::TwoByte);
        masm.load32(startIndexAddress, temp2);
        if (latin1)
            masm.addPtr(temp2, temp1);
        else
            masm.computeEffectiveAddress(BaseIndex(temp1, temp2, TimesTwo), temp1);
        masm.storeNonInlineStringChars(temp1, string);
        masm.storeDependentStringBase(base, string);
        masm.movePtr(base, temp1);

        // Follow any base pointer if the input is itself a dependent string.
        // Watch for undepended strings, which have a base pointer but don't
        // actually share their characters with it.
        Label noBase;
        masm.load32(Address(base, JSString::offsetOfFlags()), temp2);
        masm.and32(Imm32(JSString::TYPE_FLAGS_MASK), temp2);
        masm.branch32(Assembler::NotEqual, temp2, Imm32(JSString::DEPENDENT_FLAGS), &noBase);
        masm.loadDependentStringBase(base, temp1);
        masm.storeDependentStringBase(temp1, string);
        masm.bind(&noBase);

        // Post-barrier the base store, whether it was the direct or indirect
        // base (both will end up in temp1 here).
        masm.movePtr(ImmWord(0), temp2);
        LiveGeneralRegisterSet saveRegs(GeneralRegisterSet::Volatile());
        if (temp1.volatile_())
            saveRegs.takeUnchecked(temp1);
        if (temp2.volatile_())
            saveRegs.takeUnchecked(temp2);
        EmitPostWriteBarrierS(masm, string, FieldToBarrier::DEPENDENT_STRING_BASE,
                              temp2 /* prev */, temp1 /* next */, saveRegs);
    }

    masm.bind(&done);
}

static void*
AllocateString(JSContext* cx)
{
    AutoUnsafeCallWithABI unsafe;
    return js::Allocate<JSString, NoGC>(cx, js::gc::TenuredHeap);
}

static void*
AllocateFatInlineString(JSContext* cx)
{
    AutoUnsafeCallWithABI unsafe;
    return js::Allocate<JSFatInlineString, NoGC>(cx, js::gc::TenuredHeap);
}

void
CreateDependentString::generateFallback(MacroAssembler& masm, LiveRegisterSet regsToSave)
{
    regsToSave.take(string_);
    regsToSave.take(temp_);
    for (FallbackKind kind : mozilla::MakeEnumeratedRange(FallbackKind::Count)) {
        masm.bind(&fallbacks_[kind]);

        masm.PushRegsInMask(regsToSave);

        masm.setupUnalignedABICall(string_);
        masm.loadJSContext(string_);
        masm.passABIArg(string_);
        masm.callWithABI(kind == FallbackKind::FatInlineString
                         ? JS_FUNC_TO_DATA_PTR(void*, AllocateFatInlineString)
                         : JS_FUNC_TO_DATA_PTR(void*, AllocateString));
        masm.storeCallPointerResult(string_);

        masm.PopRegsInMask(regsToSave);

        masm.branchPtr(Assembler::Equal, string_, ImmWord(0), failure_);

        masm.jump(&joins_[kind]);
    }
}

static void*
CreateMatchResultFallbackFunc(JSContext* cx, gc::AllocKind kind, size_t nDynamicSlots)
{
    AutoUnsafeCallWithABI unsafe;
    return js::Allocate<JSObject, NoGC>(cx, kind, nDynamicSlots, gc::DefaultHeap,
                                        &ArrayObject::class_);
}

static void
CreateMatchResultFallback(MacroAssembler& masm, LiveRegisterSet regsToSave,
                          Register object, Register temp2, Register temp5,
                          ArrayObject* templateObj, Label* fail)
{
    MOZ_ASSERT(templateObj->group()->clasp() == &ArrayObject::class_);

    regsToSave.take(object);
    regsToSave.take(temp2);
    regsToSave.take(temp5);
    masm.PushRegsInMask(regsToSave);

    masm.setupUnalignedABICall(object);

    masm.loadJSContext(object);
    masm.passABIArg(object);
    masm.move32(Imm32(int32_t(templateObj->asTenured().getAllocKind())), temp2);
    masm.passABIArg(temp2);
    masm.move32(Imm32(int32_t(templateObj->as<NativeObject>().numDynamicSlots())), temp5);
    masm.passABIArg(temp5);
    masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, CreateMatchResultFallbackFunc));
    masm.storeCallPointerResult(object);

    masm.PopRegsInMask(regsToSave);

    masm.branchPtr(Assembler::Equal, object, ImmWord(0), fail);

    masm.initGCThing(object, temp2, templateObj, true, false);
}

JitCode*
JitCompartment::generateRegExpMatcherStub(JSContext* cx)
{
    Register regexp = RegExpMatcherRegExpReg;
    Register input = RegExpMatcherStringReg;
    Register lastIndex = RegExpMatcherLastIndexReg;
    ValueOperand result = JSReturnOperand;

    // We are free to clobber all registers, as LRegExpMatcher is a call instruction.
    AllocatableGeneralRegisterSet regs(GeneralRegisterSet::All());
    regs.take(input);
    regs.take(regexp);
    regs.take(lastIndex);

    // temp5 is used in single byte instructions when creating dependent
    // strings, and has restrictions on which register it can be on some
    // platforms.
    Register temp5;
    {
        AllocatableGeneralRegisterSet oregs = regs;
        do {
            temp5 = oregs.takeAny();
        } while (!MacroAssembler::canUseInSingleByteInstruction(temp5));
        regs.take(temp5);
    }

    Register temp1 = regs.takeAny();
    Register temp2 = regs.takeAny();
    Register temp3 = regs.takeAny();

    Register maybeTemp4 = InvalidReg;
    if (!regs.empty()) {
        // There are not enough registers on x86.
        maybeTemp4 = regs.takeAny();
    }

    ArrayObject* templateObject = cx->compartment()->regExps.getOrCreateMatchResultTemplateObject(cx);
    if (!templateObject)
        return nullptr;

    // The template object should have enough space for the maximum number of
    // pairs this stub can handle.
    MOZ_ASSERT(ObjectElements::VALUES_PER_HEADER + RegExpObject::MaxPairCount ==
               gc::GetGCKindSlots(templateObject->asTenured().getAllocKind()));

    MacroAssembler masm(cx);

    // The InputOutputData is placed above the return address on the stack.
    size_t inputOutputDataStartOffset = sizeof(void*);

    Label notFound, oolEntry;
    if (!PrepareAndExecuteRegExp(cx, masm, regexp, input, lastIndex,
                                 temp1, temp2, temp5, inputOutputDataStartOffset,
                                 RegExpShared::Normal, &notFound, &oolEntry))
    {
        return nullptr;
    }

    // Construct the result.
    Register object = temp1;
    Label matchResultFallback, matchResultJoin;
    masm.createGCObject(object, temp2, templateObject, gc::DefaultHeap, &matchResultFallback);
    masm.bind(&matchResultJoin);

    // Initialize slots of result object.
    masm.loadPtr(Address(object, NativeObject::offsetOfSlots()), temp2);
    masm.storeValue(templateObject->getSlot(0), Address(temp2, 0));
    masm.storeValue(templateObject->getSlot(1), Address(temp2, sizeof(Value)));

    size_t elementsOffset = NativeObject::offsetOfFixedElements();

#ifdef DEBUG
    // Assert the initial value of initializedLength and length to make sure
    // restoration on failure case works.
    {
        Label initLengthOK, lengthOK;
        masm.branch32(Assembler::Equal,
                      Address(object, elementsOffset + ObjectElements::offsetOfInitializedLength()),
                      Imm32(templateObject->getDenseInitializedLength()),
                      &initLengthOK);
        masm.assumeUnreachable("Initial value of the match object's initializedLength does not match to restoration.");
        masm.bind(&initLengthOK);

        masm.branch32(Assembler::Equal,
                      Address(object, elementsOffset + ObjectElements::offsetOfLength()),
                      Imm32(templateObject->length()),
                      &lengthOK);
        masm.assumeUnreachable("Initial value of The match object's length does not match to restoration.");
        masm.bind(&lengthOK);
    }
#endif

    Register matchIndex = temp2;
    masm.move32(Imm32(0), matchIndex);

    size_t pairsVectorStartOffset = RegExpPairsVectorStartOffset(inputOutputDataStartOffset);
    Address pairsVectorAddress(masm.getStackPointer(), pairsVectorStartOffset);
    Address pairCountAddress = RegExpPairCountAddress(masm, inputOutputDataStartOffset);

    BaseIndex stringAddress(object, matchIndex, TimesEight, elementsOffset);

    JS_STATIC_ASSERT(sizeof(MatchPair) == 8);
    BaseIndex stringIndexAddress(masm.getStackPointer(), matchIndex, TimesEight,
                                 pairsVectorStartOffset + offsetof(MatchPair, start));
    BaseIndex stringLimitAddress(masm.getStackPointer(), matchIndex, TimesEight,
                                 pairsVectorStartOffset + offsetof(MatchPair, limit));

    // Loop to construct the match strings. There are two different loops,
    // depending on whether the input is latin1.
    CreateDependentString depStr[2];

    // depStr may refer to failureRestore during generateFallback below,
    // so this variable must live outside of the block.
    Label failureRestore;
    {
        Label isLatin1, done;
        masm.branchLatin1String(input, &isLatin1);

        Label* failure = &oolEntry;
        Register temp4 = (maybeTemp4 == InvalidReg) ? lastIndex : maybeTemp4;

        if (maybeTemp4 == InvalidReg) {
            failure = &failureRestore;

            // Save lastIndex value to temporary space.
            masm.store32(lastIndex, Address(object, elementsOffset + ObjectElements::offsetOfLength()));
        }

        for (int isLatin = 0; isLatin <= 1; isLatin++) {
            if (isLatin)
                masm.bind(&isLatin1);

            Label matchLoop;
            masm.bind(&matchLoop);

            Label isUndefined, storeDone;
            masm.branch32(Assembler::LessThan, stringIndexAddress, Imm32(0), &isUndefined);

            depStr[isLatin].generate(masm, cx->names(), isLatin, temp3, input, temp4, temp5,
                                     stringIndexAddress, stringLimitAddress,
                                     stringsCanBeInNursery,
                                     failure);

            masm.storeValue(JSVAL_TYPE_STRING, temp3, stringAddress);
            // Storing into nursery-allocated results object's elements; no post barrier.
            masm.jump(&storeDone);
            masm.bind(&isUndefined);

            masm.storeValue(UndefinedValue(), stringAddress);
            masm.bind(&storeDone);

            masm.add32(Imm32(1), matchIndex);
            masm.branch32(Assembler::LessThanOrEqual, pairCountAddress, matchIndex, &done);
            masm.jump(&matchLoop);
        }

        if (maybeTemp4 == InvalidReg) {
            // Restore lastIndex value from temporary space, both for success
            // and failure cases.

            masm.load32(Address(object, elementsOffset + ObjectElements::offsetOfLength()), lastIndex);
            masm.jump(&done);

            masm.bind(&failureRestore);
            masm.load32(Address(object, elementsOffset + ObjectElements::offsetOfLength()), lastIndex);

            // Restore the match object for failure case.
            masm.store32(Imm32(templateObject->getDenseInitializedLength()),
                         Address(object, elementsOffset + ObjectElements::offsetOfInitializedLength()));
            masm.store32(Imm32(templateObject->length()),
                         Address(object, elementsOffset + ObjectElements::offsetOfLength()));
            masm.jump(&oolEntry);
        }

        masm.bind(&done);
    }

    // Fill in the rest of the output object.
    masm.store32(matchIndex, Address(object, elementsOffset + ObjectElements::offsetOfInitializedLength()));
    masm.store32(matchIndex, Address(object, elementsOffset + ObjectElements::offsetOfLength()));

    masm.loadPtr(Address(object, NativeObject::offsetOfSlots()), temp2);

    MOZ_ASSERT(templateObject->numFixedSlots() == 0);
    MOZ_ASSERT(templateObject->lookupPure(cx->names().index)->slot() == 0);
    MOZ_ASSERT(templateObject->lookupPure(cx->names().input)->slot() == 1);

    masm.load32(pairsVectorAddress, temp3);
    masm.storeValue(JSVAL_TYPE_INT32, temp3, Address(temp2, 0));
    Address inputSlotAddress(temp2, sizeof(Value));
    masm.storeValue(JSVAL_TYPE_STRING, input, inputSlotAddress);
    // No post barrier needed (inputSlotAddress is within nursery object.)

    // All done!
    masm.tagValue(JSVAL_TYPE_OBJECT, object, result);
    masm.ret();

    masm.bind(&notFound);
    masm.moveValue(NullValue(), result);
    masm.ret();

    // Fallback paths for CreateDependentString and createGCObject.
    // Need to save all registers in use when they were called.
    LiveRegisterSet regsToSave(RegisterSet::Volatile());
    regsToSave.addUnchecked(regexp);
    regsToSave.addUnchecked(input);
    regsToSave.addUnchecked(lastIndex);
    regsToSave.addUnchecked(temp1);
    regsToSave.addUnchecked(temp2);
    regsToSave.addUnchecked(temp3);
    if (maybeTemp4 != InvalidReg)
        regsToSave.addUnchecked(maybeTemp4);
    regsToSave.addUnchecked(temp5);

    for (int isLatin = 0; isLatin <= 1; isLatin++)
        depStr[isLatin].generateFallback(masm, regsToSave);

    masm.bind(&matchResultFallback);
    CreateMatchResultFallback(masm, regsToSave, object, temp2, temp5, templateObject, &oolEntry);
    masm.jump(&matchResultJoin);

    // Use an undefined value to signal to the caller that the OOL stub needs to be called.
    masm.bind(&oolEntry);
    masm.moveValue(UndefinedValue(), result);
    masm.ret();

    Linker linker(masm);
    AutoFlushICache afc("RegExpMatcherStub");
    JitCode* code = linker.newCode(cx, CodeKind::Other);
    if (!code)
        return nullptr;

#ifdef JS_ION_PERF
    writePerfSpewerJitCodeProfile(code, "RegExpMatcherStub");
#endif
#ifdef MOZ_VTUNE
    vtune::MarkStub(code, "RegExpMatcherStub");
#endif

    return code;
}

class OutOfLineRegExpMatcher : public OutOfLineCodeBase<CodeGenerator>
{
    LRegExpMatcher* lir_;

  public:
    explicit OutOfLineRegExpMatcher(LRegExpMatcher* lir)
      : lir_(lir)
    { }

    void accept(CodeGenerator* codegen) override {
        codegen->visitOutOfLineRegExpMatcher(this);
    }

    LRegExpMatcher* lir() const {
        return lir_;
    }
};

typedef bool (*RegExpMatcherRawFn)(JSContext* cx, HandleObject regexp, HandleString input,
                                   int32_t lastIndex,
                                   MatchPairs* pairs, MutableHandleValue output);
static const VMFunction RegExpMatcherRawInfo =
    FunctionInfo<RegExpMatcherRawFn>(RegExpMatcherRaw, "RegExpMatcherRaw");

void
CodeGenerator::visitOutOfLineRegExpMatcher(OutOfLineRegExpMatcher* ool)
{
    LRegExpMatcher* lir = ool->lir();
    Register lastIndex = ToRegister(lir->lastIndex());
    Register input = ToRegister(lir->string());
    Register regexp = ToRegister(lir->regexp());

    AllocatableGeneralRegisterSet regs(GeneralRegisterSet::All());
    regs.take(lastIndex);
    regs.take(input);
    regs.take(regexp);
    Register temp = regs.takeAny();

    masm.computeEffectiveAddress(Address(masm.getStackPointer(),
        sizeof(irregexp::InputOutputData)), temp);

    pushArg(temp);
    pushArg(lastIndex);
    pushArg(input);
    pushArg(regexp);

    // We are not using oolCallVM because we are in a Call, and that live
    // registers are already saved by the the register allocator.
    callVM(RegExpMatcherRawInfo, lir);

    masm.jump(ool->rejoin());
}

void
CodeGenerator::visitRegExpMatcher(LRegExpMatcher* lir)
{
    MOZ_ASSERT(ToRegister(lir->regexp()) == RegExpMatcherRegExpReg);
    MOZ_ASSERT(ToRegister(lir->string()) == RegExpMatcherStringReg);
    MOZ_ASSERT(ToRegister(lir->lastIndex()) == RegExpMatcherLastIndexReg);
    MOZ_ASSERT(ToOutValue(lir) == JSReturnOperand);

#if defined(JS_NUNBOX32)
    MOZ_ASSERT(RegExpMatcherRegExpReg != JSReturnReg_Type);
    MOZ_ASSERT(RegExpMatcherRegExpReg != JSReturnReg_Data);
    MOZ_ASSERT(RegExpMatcherStringReg != JSReturnReg_Type);
    MOZ_ASSERT(RegExpMatcherStringReg != JSReturnReg_Data);
    MOZ_ASSERT(RegExpMatcherLastIndexReg != JSReturnReg_Type);
    MOZ_ASSERT(RegExpMatcherLastIndexReg != JSReturnReg_Data);
#elif defined(JS_PUNBOX64)
    MOZ_ASSERT(RegExpMatcherRegExpReg != JSReturnReg);
    MOZ_ASSERT(RegExpMatcherStringReg != JSReturnReg);
    MOZ_ASSERT(RegExpMatcherLastIndexReg != JSReturnReg);
#endif

    masm.reserveStack(RegExpReservedStack);

    OutOfLineRegExpMatcher* ool = new(alloc()) OutOfLineRegExpMatcher(lir);
    addOutOfLineCode(ool, lir->mir());

    const JitCompartment* jitCompartment = gen->compartment->jitCompartment();
    JitCode* regExpMatcherStub = jitCompartment->regExpMatcherStubNoBarrier(&compartmentStubsToReadBarrier_);
    masm.call(regExpMatcherStub);
    masm.branchTestUndefined(Assembler::Equal, JSReturnOperand, ool->entry());
    masm.bind(ool->rejoin());

    masm.freeStack(RegExpReservedStack);
}

static const int32_t RegExpSearcherResultNotFound = -1;
static const int32_t RegExpSearcherResultFailed = -2;

JitCode*
JitCompartment::generateRegExpSearcherStub(JSContext* cx)
{
    Register regexp = RegExpTesterRegExpReg;
    Register input = RegExpTesterStringReg;
    Register lastIndex = RegExpTesterLastIndexReg;
    Register result = ReturnReg;

    // We are free to clobber all registers, as LRegExpSearcher is a call instruction.
    AllocatableGeneralRegisterSet regs(GeneralRegisterSet::All());
    regs.take(input);
    regs.take(regexp);
    regs.take(lastIndex);

    Register temp1 = regs.takeAny();
    Register temp2 = regs.takeAny();
    Register temp3 = regs.takeAny();

    MacroAssembler masm(cx);

    // The InputOutputData is placed above the return address on the stack.
    size_t inputOutputDataStartOffset = sizeof(void*);

    Label notFound, oolEntry;
    if (!PrepareAndExecuteRegExp(cx, masm, regexp, input, lastIndex,
                                 temp1, temp2, temp3, inputOutputDataStartOffset,
                                 RegExpShared::Normal, &notFound, &oolEntry))
    {
        return nullptr;
    }

    size_t pairsVectorStartOffset = RegExpPairsVectorStartOffset(inputOutputDataStartOffset);
    Address stringIndexAddress(masm.getStackPointer(),
                               pairsVectorStartOffset + offsetof(MatchPair, start));
    Address stringLimitAddress(masm.getStackPointer(),
                               pairsVectorStartOffset + offsetof(MatchPair, limit));

    masm.load32(stringIndexAddress, result);
    masm.load32(stringLimitAddress, input);
    masm.lshiftPtr(Imm32(15), input);
    masm.or32(input, result);
    masm.ret();

    masm.bind(&notFound);
    masm.move32(Imm32(RegExpSearcherResultNotFound), result);
    masm.ret();

    masm.bind(&oolEntry);
    masm.move32(Imm32(RegExpSearcherResultFailed), result);
    masm.ret();

    Linker linker(masm);
    AutoFlushICache afc("RegExpSearcherStub");
    JitCode* code = linker.newCode(cx, CodeKind::Other);
    if (!code)
        return nullptr;

#ifdef JS_ION_PERF
    writePerfSpewerJitCodeProfile(code, "RegExpSearcherStub");
#endif
#ifdef MOZ_VTUNE
    vtune::MarkStub(code, "RegExpSearcherStub");
#endif

    return code;
}

class OutOfLineRegExpSearcher : public OutOfLineCodeBase<CodeGenerator>
{
    LRegExpSearcher* lir_;

  public:
    explicit OutOfLineRegExpSearcher(LRegExpSearcher* lir)
      : lir_(lir)
    { }

    void accept(CodeGenerator* codegen) override {
        codegen->visitOutOfLineRegExpSearcher(this);
    }

    LRegExpSearcher* lir() const {
        return lir_;
    }
};

typedef bool (*RegExpSearcherRawFn)(JSContext* cx, HandleObject regexp, HandleString input,
                                    int32_t lastIndex,
                                    MatchPairs* pairs, int32_t* result);
static const VMFunction RegExpSearcherRawInfo =
    FunctionInfo<RegExpSearcherRawFn>(RegExpSearcherRaw, "RegExpSearcherRaw");

void
CodeGenerator::visitOutOfLineRegExpSearcher(OutOfLineRegExpSearcher* ool)
{
    LRegExpSearcher* lir = ool->lir();
    Register lastIndex = ToRegister(lir->lastIndex());
    Register input = ToRegister(lir->string());
    Register regexp = ToRegister(lir->regexp());

    AllocatableGeneralRegisterSet regs(GeneralRegisterSet::All());
    regs.take(lastIndex);
    regs.take(input);
    regs.take(regexp);
    Register temp = regs.takeAny();

    masm.computeEffectiveAddress(Address(masm.getStackPointer(),
        sizeof(irregexp::InputOutputData)), temp);

    pushArg(temp);
    pushArg(lastIndex);
    pushArg(input);
    pushArg(regexp);

    // We are not using oolCallVM because we are in a Call, and that live
    // registers are already saved by the the register allocator.
    callVM(RegExpSearcherRawInfo, lir);

    masm.jump(ool->rejoin());
}

void
CodeGenerator::visitRegExpSearcher(LRegExpSearcher* lir)
{
    MOZ_ASSERT(ToRegister(lir->regexp()) == RegExpTesterRegExpReg);
    MOZ_ASSERT(ToRegister(lir->string()) == RegExpTesterStringReg);
    MOZ_ASSERT(ToRegister(lir->lastIndex()) == RegExpTesterLastIndexReg);
    MOZ_ASSERT(ToRegister(lir->output()) == ReturnReg);

    MOZ_ASSERT(RegExpTesterRegExpReg != ReturnReg);
    MOZ_ASSERT(RegExpTesterStringReg != ReturnReg);
    MOZ_ASSERT(RegExpTesterLastIndexReg != ReturnReg);

    masm.reserveStack(RegExpReservedStack);

    OutOfLineRegExpSearcher* ool = new(alloc()) OutOfLineRegExpSearcher(lir);
    addOutOfLineCode(ool, lir->mir());

    const JitCompartment* jitCompartment = gen->compartment->jitCompartment();
    JitCode* regExpSearcherStub = jitCompartment->regExpSearcherStubNoBarrier(&compartmentStubsToReadBarrier_);
    masm.call(regExpSearcherStub);
    masm.branch32(Assembler::Equal, ReturnReg, Imm32(RegExpSearcherResultFailed), ool->entry());
    masm.bind(ool->rejoin());

    masm.freeStack(RegExpReservedStack);
}

static const int32_t RegExpTesterResultNotFound = -1;
static const int32_t RegExpTesterResultFailed = -2;

JitCode*
JitCompartment::generateRegExpTesterStub(JSContext* cx)
{
    Register regexp = RegExpTesterRegExpReg;
    Register input = RegExpTesterStringReg;
    Register lastIndex = RegExpTesterLastIndexReg;
    Register result = ReturnReg;

    MacroAssembler masm(cx);

#ifdef JS_USE_LINK_REGISTER
    masm.pushReturnAddress();
#endif

    // We are free to clobber all registers, as LRegExpTester is a call instruction.
    AllocatableGeneralRegisterSet regs(GeneralRegisterSet::All());
    regs.take(input);
    regs.take(regexp);
    regs.take(lastIndex);

    Register temp1 = regs.takeAny();
    Register temp2 = regs.takeAny();
    Register temp3 = regs.takeAny();

    masm.reserveStack(sizeof(irregexp::InputOutputData));

    Label notFound, oolEntry;
    if (!PrepareAndExecuteRegExp(cx, masm, regexp, input, lastIndex,
                                 temp1, temp2, temp3, 0,
                                 RegExpShared::MatchOnly, &notFound, &oolEntry))
    {
        return nullptr;
    }

    Label done;

    // temp3 contains endIndex.
    masm.move32(temp3, result);
    masm.jump(&done);

    masm.bind(&notFound);
    masm.move32(Imm32(RegExpTesterResultNotFound), result);
    masm.jump(&done);

    masm.bind(&oolEntry);
    masm.move32(Imm32(RegExpTesterResultFailed), result);

    masm.bind(&done);
    masm.freeStack(sizeof(irregexp::InputOutputData));
    masm.ret();

    Linker linker(masm);
    AutoFlushICache afc("RegExpTesterStub");
    JitCode* code = linker.newCode(cx, CodeKind::Other);
    if (!code)
        return nullptr;

#ifdef JS_ION_PERF
    writePerfSpewerJitCodeProfile(code, "RegExpTesterStub");
#endif
#ifdef MOZ_VTUNE
    vtune::MarkStub(code, "RegExpTesterStub");
#endif

    return code;
}

class OutOfLineRegExpTester : public OutOfLineCodeBase<CodeGenerator>
{
    LRegExpTester* lir_;

  public:
    explicit OutOfLineRegExpTester(LRegExpTester* lir)
      : lir_(lir)
    { }

    void accept(CodeGenerator* codegen) override {
        codegen->visitOutOfLineRegExpTester(this);
    }

    LRegExpTester* lir() const {
        return lir_;
    }
};

typedef bool (*RegExpTesterRawFn)(JSContext* cx, HandleObject regexp, HandleString input,
                                  int32_t lastIndex, int32_t* result);
static const VMFunction RegExpTesterRawInfo =
    FunctionInfo<RegExpTesterRawFn>(RegExpTesterRaw, "RegExpTesterRaw");

void
CodeGenerator::visitOutOfLineRegExpTester(OutOfLineRegExpTester* ool)
{
    LRegExpTester* lir = ool->lir();
    Register lastIndex = ToRegister(lir->lastIndex());
    Register input = ToRegister(lir->string());
    Register regexp = ToRegister(lir->regexp());

    pushArg(lastIndex);
    pushArg(input);
    pushArg(regexp);

    // We are not using oolCallVM because we are in a Call, and that live
    // registers are already saved by the the register allocator.
    callVM(RegExpTesterRawInfo, lir);

    masm.jump(ool->rejoin());
}

void
CodeGenerator::visitRegExpTester(LRegExpTester* lir)
{
    MOZ_ASSERT(ToRegister(lir->regexp()) == RegExpTesterRegExpReg);
    MOZ_ASSERT(ToRegister(lir->string()) == RegExpTesterStringReg);
    MOZ_ASSERT(ToRegister(lir->lastIndex()) == RegExpTesterLastIndexReg);
    MOZ_ASSERT(ToRegister(lir->output()) == ReturnReg);

    MOZ_ASSERT(RegExpTesterRegExpReg != ReturnReg);
    MOZ_ASSERT(RegExpTesterStringReg != ReturnReg);
    MOZ_ASSERT(RegExpTesterLastIndexReg != ReturnReg);

    OutOfLineRegExpTester* ool = new(alloc()) OutOfLineRegExpTester(lir);
    addOutOfLineCode(ool, lir->mir());

    const JitCompartment* jitCompartment = gen->compartment->jitCompartment();
    JitCode* regExpTesterStub = jitCompartment->regExpTesterStubNoBarrier(&compartmentStubsToReadBarrier_);
    masm.call(regExpTesterStub);

    masm.branch32(Assembler::Equal, ReturnReg, Imm32(RegExpTesterResultFailed), ool->entry());
    masm.bind(ool->rejoin());
}

class OutOfLineRegExpPrototypeOptimizable : public OutOfLineCodeBase<CodeGenerator>
{
    LRegExpPrototypeOptimizable* ins_;

  public:
    explicit OutOfLineRegExpPrototypeOptimizable(LRegExpPrototypeOptimizable* ins)
      : ins_(ins)
    { }

    void accept(CodeGenerator* codegen) override {
        codegen->visitOutOfLineRegExpPrototypeOptimizable(this);
    }
    LRegExpPrototypeOptimizable* ins() const {
        return ins_;
    }
};

void
CodeGenerator::visitRegExpPrototypeOptimizable(LRegExpPrototypeOptimizable* ins)
{
    Register object = ToRegister(ins->object());
    Register output = ToRegister(ins->output());
    Register temp = ToRegister(ins->temp());

    OutOfLineRegExpPrototypeOptimizable* ool = new(alloc()) OutOfLineRegExpPrototypeOptimizable(ins);
    addOutOfLineCode(ool, ins->mir());

    masm.loadJSContext(temp);
    masm.loadPtr(Address(temp, JSContext::offsetOfCompartment()), temp);
    size_t offset = JSCompartment::offsetOfRegExps() +
                    RegExpCompartment::offsetOfOptimizableRegExpPrototypeShape();
    masm.loadPtr(Address(temp, offset), temp);

    masm.branchTestObjShapeUnsafe(Assembler::NotEqual, object, temp, ool->entry());
    masm.move32(Imm32(0x1), output);

    masm.bind(ool->rejoin());
}

void
CodeGenerator::visitOutOfLineRegExpPrototypeOptimizable(OutOfLineRegExpPrototypeOptimizable* ool)
{
    LRegExpPrototypeOptimizable* ins = ool->ins();
    Register object = ToRegister(ins->object());
    Register output = ToRegister(ins->output());

    saveVolatile(output);

    masm.setupUnalignedABICall(output);
    masm.loadJSContext(output);
    masm.passABIArg(output);
    masm.passABIArg(object);
    masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, RegExpPrototypeOptimizableRaw));
    masm.storeCallBoolResult(output);

    restoreVolatile(output);

    masm.jump(ool->rejoin());
}

class OutOfLineRegExpInstanceOptimizable : public OutOfLineCodeBase<CodeGenerator>
{
    LRegExpInstanceOptimizable* ins_;

  public:
    explicit OutOfLineRegExpInstanceOptimizable(LRegExpInstanceOptimizable* ins)
      : ins_(ins)
    { }

    void accept(CodeGenerator* codegen) override {
        codegen->visitOutOfLineRegExpInstanceOptimizable(this);
    }
    LRegExpInstanceOptimizable* ins() const {
        return ins_;
    }
};

void
CodeGenerator::visitRegExpInstanceOptimizable(LRegExpInstanceOptimizable* ins)
{
    Register object = ToRegister(ins->object());
    Register output = ToRegister(ins->output());
    Register temp = ToRegister(ins->temp());

    OutOfLineRegExpInstanceOptimizable* ool = new(alloc()) OutOfLineRegExpInstanceOptimizable(ins);
    addOutOfLineCode(ool, ins->mir());

    masm.loadJSContext(temp);
    masm.loadPtr(Address(temp, JSContext::offsetOfCompartment()), temp);
    size_t offset = JSCompartment::offsetOfRegExps() +
                    RegExpCompartment::offsetOfOptimizableRegExpInstanceShape();
    masm.loadPtr(Address(temp, offset), temp);

    masm.branchTestObjShapeUnsafe(Assembler::NotEqual, object, temp, ool->entry());
    masm.move32(Imm32(0x1), output);

    masm.bind(ool->rejoin());
}

void
CodeGenerator::visitOutOfLineRegExpInstanceOptimizable(OutOfLineRegExpInstanceOptimizable* ool)
{
    LRegExpInstanceOptimizable* ins = ool->ins();
    Register object = ToRegister(ins->object());
    Register proto = ToRegister(ins->proto());
    Register output = ToRegister(ins->output());

    saveVolatile(output);

    masm.setupUnalignedABICall(output);
    masm.loadJSContext(output);
    masm.passABIArg(output);
    masm.passABIArg(object);
    masm.passABIArg(proto);
    masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, RegExpInstanceOptimizableRaw));
    masm.storeCallBoolResult(output);

    restoreVolatile(output);

    masm.jump(ool->rejoin());
}

static void
FindFirstDollarIndex(MacroAssembler& masm, Register len, Register chars,
                     Register temp, Register output, bool isLatin1)
{
    masm.move32(Imm32(0), output);

    Label start, done;
    masm.bind(&start);
    if (isLatin1)
        masm.load8ZeroExtend(BaseIndex(chars, output, TimesOne), temp);
    else
        masm.load16ZeroExtend(BaseIndex(chars, output, TimesTwo), temp);

    masm.branch32(Assembler::Equal, temp, Imm32('$'), &done);

    masm.add32(Imm32(1), output);
    masm.branch32(Assembler::NotEqual, output, len, &start);

    masm.move32(Imm32(-1), output);

    masm.bind(&done);
}

typedef bool (*GetFirstDollarIndexRawFn)(JSContext*, JSString*, int32_t*);
static const VMFunction GetFirstDollarIndexRawInfo =
    FunctionInfo<GetFirstDollarIndexRawFn>(GetFirstDollarIndexRaw, "GetFirstDollarIndexRaw");

void
CodeGenerator::visitGetFirstDollarIndex(LGetFirstDollarIndex* ins)
{
    Register str = ToRegister(ins->str());
    Register output = ToRegister(ins->output());
    Register temp0 = ToRegister(ins->temp0());
    Register temp1 = ToRegister(ins->temp1());
    Register len = ToRegister(ins->temp2());

    OutOfLineCode* ool = oolCallVM(GetFirstDollarIndexRawInfo, ins, ArgList(str),
                                   StoreRegisterTo(output));

    masm.branchIfRope(str, ool->entry());
    masm.loadStringLength(str, len);

    Label isLatin1, done;
    masm.branchLatin1String(str, &isLatin1);
    {
        masm.loadStringChars(str, temp0, CharEncoding::TwoByte);
        FindFirstDollarIndex(masm, len, temp0, temp1, output, /* isLatin1 = */ false);
        masm.jump(&done);
    }
    masm.bind(&isLatin1);
    {
        masm.loadStringChars(str, temp0, CharEncoding::Latin1);
        FindFirstDollarIndex(masm, len, temp0, temp1, output, /* isLatin1 = */ true);
    }
    masm.bind(&done);
    masm.bind(ool->rejoin());
}

typedef JSString* (*StringReplaceFn)(JSContext*, HandleString, HandleString, HandleString);
static const VMFunction StringFlatReplaceInfo =
    FunctionInfo<StringReplaceFn>(js::str_flat_replace_string, "str_flat_replace_string");
static const VMFunction StringReplaceInfo =
    FunctionInfo<StringReplaceFn>(StringReplace, "StringReplace");

void
CodeGenerator::visitStringReplace(LStringReplace* lir)
{
    if (lir->replacement()->isConstant())
        pushArg(ImmGCPtr(lir->replacement()->toConstant()->toString()));
    else
        pushArg(ToRegister(lir->replacement()));

    if (lir->pattern()->isConstant())
        pushArg(ImmGCPtr(lir->pattern()->toConstant()->toString()));
    else
        pushArg(ToRegister(lir->pattern()));

    if (lir->string()->isConstant())
        pushArg(ImmGCPtr(lir->string()->toConstant()->toString()));
    else
        pushArg(ToRegister(lir->string()));

    if (lir->mir()->isFlatReplacement())
        callVM(StringFlatReplaceInfo, lir);
    else
        callVM(StringReplaceInfo, lir);
}

void
CodeGenerator::emitSharedStub(ICStub::Kind kind, LInstruction* lir)
{
    JSScript* script = lir->mirRaw()->block()->info().script();
    jsbytecode* pc = lir->mirRaw()->toInstruction()->resumePoint()->pc();

#ifdef JS_USE_LINK_REGISTER
    // Some architectures don't push the return address on the stack but
    // use the link register. In that case the stack isn't aligned. Push
    // to make sure we are aligned.
    masm.Push(Imm32(0));
#endif

    // Create descriptor signifying end of Ion frame.
    uint32_t descriptor = MakeFrameDescriptor(masm.framePushed(), JitFrame_IonJS,
                                              JitStubFrameLayout::Size());
    masm.Push(Imm32(descriptor));

    // Call into the stubcode.
    CodeOffset patchOffset;
    IonICEntry entry(script->pcToOffset(pc), ICEntry::Kind_Op, script);
    EmitCallIC(&patchOffset, masm);
    entry.setReturnOffset(CodeOffset(masm.currentOffset()));

    SharedStub sharedStub(kind, entry, patchOffset);
    masm.propagateOOM(sharedStubs_.append(sharedStub));

    // Fix up upon return.
    uint32_t callOffset = masm.currentOffset();
#ifdef JS_USE_LINK_REGISTER
    masm.freeStack(sizeof(intptr_t) * 2);
#else
    masm.freeStack(sizeof(intptr_t));
#endif
    markSafepointAt(callOffset, lir);
}

void
CodeGenerator::visitBinarySharedStub(LBinarySharedStub* lir)
{
    JSOp jsop = JSOp(*lir->mirRaw()->toInstruction()->resumePoint()->pc());
    switch (jsop) {
      case JSOP_ADD:
      case JSOP_SUB:
      case JSOP_MUL:
      case JSOP_DIV:
      case JSOP_MOD:
      case JSOP_POW:
        emitSharedStub(ICStub::Kind::BinaryArith_Fallback, lir);
        break;
      case JSOP_LT:
      case JSOP_LE:
      case JSOP_GT:
      case JSOP_GE:
      case JSOP_EQ:
      case JSOP_NE:
      case JSOP_STRICTEQ:
      case JSOP_STRICTNE:
        emitSharedStub(ICStub::Kind::Compare_Fallback, lir);
        break;
      default:
        MOZ_CRASH("Unsupported jsop in shared stubs.");
    }
}

void
CodeGenerator::visitUnarySharedStub(LUnarySharedStub* lir)
{
    JSOp jsop = JSOp(*lir->mir()->resumePoint()->pc());
    switch (jsop) {
      case JSOP_BITNOT:
      case JSOP_NEG:
        emitSharedStub(ICStub::Kind::UnaryArith_Fallback, lir);
        break;
      case JSOP_CALLPROP:
      case JSOP_GETPROP:
      case JSOP_LENGTH:
        emitSharedStub(ICStub::Kind::GetProp_Fallback, lir);
        break;
      default:
        MOZ_CRASH("Unsupported jsop in shared stubs.");
    }
}

void
CodeGenerator::visitNullarySharedStub(LNullarySharedStub* lir)
{
    jsbytecode* pc = lir->mir()->resumePoint()->pc();
    JSOp jsop = JSOp(*pc);
    switch (jsop) {
      case JSOP_NEWARRAY: {
        uint32_t length = GET_UINT32(pc);
        MOZ_ASSERT(length <= INT32_MAX,
                   "the bytecode emitter must fail to compile code that would "
                   "produce JSOP_NEWARRAY with a length exceeding int32_t range");

        // Pass length in R0.
        masm.move32(Imm32(AssertedCast<int32_t>(length)), R0.scratchReg());
        emitSharedStub(ICStub::Kind::NewArray_Fallback, lir);
        break;
      }
      case JSOP_NEWOBJECT:
        emitSharedStub(ICStub::Kind::NewObject_Fallback, lir);
        break;
      case JSOP_NEWINIT: {
        JSProtoKey key = JSProtoKey(GET_UINT8(pc));
        if (key == JSProto_Array) {
            masm.move32(Imm32(0), R0.scratchReg());
            emitSharedStub(ICStub::Kind::NewArray_Fallback, lir);
        } else {
            emitSharedStub(ICStub::Kind::NewObject_Fallback, lir);
        }
        break;
      }
      default:
        MOZ_CRASH("Unsupported jsop in shared stubs.");
    }
}

typedef JSFunction* (*MakeDefaultConstructorFn)(JSContext*, HandleScript,
                                                jsbytecode*, HandleObject);
static const VMFunction MakeDefaultConstructorInfo =
    FunctionInfo<MakeDefaultConstructorFn>(js::MakeDefaultConstructor,
                                           "MakeDefaultConstructor");

void
CodeGenerator::visitClassConstructor(LClassConstructor* lir)
{
    pushArg(ImmPtr(nullptr));
    pushArg(ImmPtr(lir->mir()->pc()));
    pushArg(ImmGCPtr(current->mir()->info().script()));
    callVM(MakeDefaultConstructorInfo, lir);
}

typedef JSObject* (*LambdaFn)(JSContext*, HandleFunction, HandleObject);
static const VMFunction LambdaInfo = FunctionInfo<LambdaFn>(js::Lambda, "Lambda");

void
CodeGenerator::visitLambdaForSingleton(LLambdaForSingleton* lir)
{
    pushArg(ToRegister(lir->environmentChain()));
    pushArg(ImmGCPtr(lir->mir()->info().funUnsafe()));
    callVM(LambdaInfo, lir);
}

void
CodeGenerator::visitLambda(LLambda* lir)
{
    Register envChain = ToRegister(lir->environmentChain());
    Register output = ToRegister(lir->output());
    Register tempReg = ToRegister(lir->temp());
    const LambdaFunctionInfo& info = lir->mir()->info();

    OutOfLineCode* ool = oolCallVM(LambdaInfo, lir, ArgList(ImmGCPtr(info.funUnsafe()), envChain),
                                   StoreRegisterTo(output));

    MOZ_ASSERT(!info.singletonType);

    masm.createGCObject(output, tempReg, info.funUnsafe(), gc::DefaultHeap, ool->entry());

    emitLambdaInit(output, envChain, info);

    if (info.flags & JSFunction::EXTENDED) {
        static_assert(FunctionExtended::NUM_EXTENDED_SLOTS == 2, "All slots must be initialized");
        masm.storeValue(UndefinedValue(), Address(output, FunctionExtended::offsetOfExtendedSlot(0)));
        masm.storeValue(UndefinedValue(), Address(output, FunctionExtended::offsetOfExtendedSlot(1)));
    }

    masm.bind(ool->rejoin());
}

class OutOfLineLambdaArrow : public OutOfLineCodeBase<CodeGenerator>
{
  public:
    LLambdaArrow* lir;
    Label entryNoPop_;

    explicit OutOfLineLambdaArrow(LLambdaArrow* lir)
      : lir(lir)
    { }

    void accept(CodeGenerator* codegen) override {
        codegen->visitOutOfLineLambdaArrow(this);
    }

    Label* entryNoPop() {
        return &entryNoPop_;
    }
};

typedef JSObject* (*LambdaArrowFn)(JSContext*, HandleFunction, HandleObject, HandleValue);
static const VMFunction LambdaArrowInfo =
    FunctionInfo<LambdaArrowFn>(js::LambdaArrow, "LambdaArrow");

void
CodeGenerator::visitOutOfLineLambdaArrow(OutOfLineLambdaArrow* ool)
{
    Register envChain = ToRegister(ool->lir->environmentChain());
    ValueOperand newTarget = ToValue(ool->lir, LLambdaArrow::NewTargetValue);
    Register output = ToRegister(ool->lir->output());
    const LambdaFunctionInfo& info = ool->lir->mir()->info();

    // When we get here, we may need to restore part of the newTarget,
    // which has been conscripted into service as a temp register.
    masm.pop(newTarget.scratchReg());

    masm.bind(ool->entryNoPop());

    saveLive(ool->lir);

    pushArg(newTarget);
    pushArg(envChain);
    pushArg(ImmGCPtr(info.funUnsafe()));

    callVM(LambdaArrowInfo, ool->lir);
    StoreRegisterTo(output).generate(this);

    restoreLiveIgnore(ool->lir, StoreRegisterTo(output).clobbered());

    masm.jump(ool->rejoin());
}

void
CodeGenerator::visitLambdaArrow(LLambdaArrow* lir)
{
    Register envChain = ToRegister(lir->environmentChain());
    ValueOperand newTarget = ToValue(lir, LLambdaArrow::NewTargetValue);
    Register output = ToRegister(lir->output());
    const LambdaFunctionInfo& info = lir->mir()->info();

    OutOfLineLambdaArrow* ool = new (alloc()) OutOfLineLambdaArrow(lir);
    addOutOfLineCode(ool, lir->mir());

    MOZ_ASSERT(!info.useSingletonForClone);

    if (info.singletonType) {
        // If the function has a singleton type, this instruction will only be
        // executed once so we don't bother inlining it.
        masm.jump(ool->entryNoPop());
        masm.bind(ool->rejoin());
        return;
    }

    // There's not enough registers on x86 with the profiler enabled to request
    // a temp. Instead, spill part of one of the values, being prepared to
    // restore it if necessary on the out of line path.
    Register tempReg = newTarget.scratchReg();
    masm.push(newTarget.scratchReg());

    masm.createGCObject(output, tempReg, info.funUnsafe(), gc::DefaultHeap, ool->entry());

    masm.pop(newTarget.scratchReg());

    emitLambdaInit(output, envChain, info);

    // Initialize extended slots. Lexical |this| is stored in the first one.
    MOZ_ASSERT(info.flags & JSFunction::EXTENDED);
    static_assert(FunctionExtended::NUM_EXTENDED_SLOTS == 2, "All slots must be initialized");
    static_assert(FunctionExtended::ARROW_NEWTARGET_SLOT == 0,
                  "|new.target| must be stored in first slot");
    masm.storeValue(newTarget, Address(output, FunctionExtended::offsetOfExtendedSlot(0)));
    masm.storeValue(UndefinedValue(), Address(output, FunctionExtended::offsetOfExtendedSlot(1)));

    masm.bind(ool->rejoin());
}

void
CodeGenerator::emitLambdaInit(Register output, Register envChain,
                              const LambdaFunctionInfo& info)
{
    // Initialize nargs and flags. We do this with a single uint32 to avoid
    // 16-bit writes.
    union {
        struct S {
            uint16_t nargs;
            uint16_t flags;
        } s;
        uint32_t word;
    } u;
    u.s.nargs = info.nargs;
    u.s.flags = info.flags;

    static_assert(JSFunction::offsetOfFlags() == JSFunction::offsetOfNargs() + 2,
                  "the code below needs to be adapted");
    masm.store32(Imm32(u.word), Address(output, JSFunction::offsetOfNargs()));
    masm.storePtr(ImmGCPtr(info.scriptOrLazyScript),
                  Address(output, JSFunction::offsetOfScriptOrLazyScript()));
    masm.storePtr(envChain, Address(output, JSFunction::offsetOfEnvironment()));
    // No post barrier needed because output is guaranteed to be allocated in
    // the nursery.
    masm.storePtr(ImmGCPtr(info.funUnsafe()->displayAtom()),
                  Address(output, JSFunction::offsetOfAtom()));
}

typedef bool (*SetFunNameFn)(JSContext*, HandleFunction, HandleValue, FunctionPrefixKind);
static const VMFunction SetFunNameInfo =
    FunctionInfo<SetFunNameFn>(js::SetFunctionNameIfNoOwnName, "SetFunName");

void
CodeGenerator::visitSetFunName(LSetFunName* lir)
{
    pushArg(Imm32(lir->mir()->prefixKind()));
    pushArg(ToValue(lir, LSetFunName::NameValue));
    pushArg(ToRegister(lir->fun()));

    callVM(SetFunNameInfo, lir);
}

void
CodeGenerator::visitOsiPoint(LOsiPoint* lir)
{
    // Note: markOsiPoint ensures enough space exists between the last
    // LOsiPoint and this one to patch adjacent call instructions.

    MOZ_ASSERT(masm.framePushed() == frameSize());

    uint32_t osiCallPointOffset = markOsiPoint(lir);

    LSafepoint* safepoint = lir->associatedSafepoint();
    MOZ_ASSERT(!safepoint->osiCallPointOffset());
    safepoint->setOsiCallPointOffset(osiCallPointOffset);

#ifdef DEBUG
    // There should be no movegroups or other instructions between
    // an instruction and its OsiPoint. This is necessary because
    // we use the OsiPoint's snapshot from within VM calls.
    for (LInstructionReverseIterator iter(current->rbegin(lir)); iter != current->rend(); iter++) {
        if (*iter == lir)
            continue;
        MOZ_ASSERT(!iter->isMoveGroup());
        MOZ_ASSERT(iter->safepoint() == safepoint);
        break;
    }
#endif

#ifdef CHECK_OSIPOINT_REGISTERS
    if (shouldVerifyOsiPointRegs(safepoint))
        verifyOsiPointRegs(safepoint);
#endif
}

void
CodeGenerator::visitGoto(LGoto* lir)
{
    jumpToBlock(lir->target());
}

// Out-of-line path to execute any move groups between the start of a loop
// header and its interrupt check, then invoke the interrupt handler.
class OutOfLineInterruptCheckImplicit : public OutOfLineCodeBase<CodeGenerator>
{
  public:
    LBlock* block;
    LInterruptCheck* lir;

    OutOfLineInterruptCheckImplicit(LBlock* block, LInterruptCheck* lir)
      : block(block), lir(lir)
    { }

    void accept(CodeGenerator* codegen) override {
        codegen->visitOutOfLineInterruptCheckImplicit(this);
    }
};

typedef bool (*InterruptCheckFn)(JSContext*);
static const VMFunction InterruptCheckInfo =
    FunctionInfo<InterruptCheckFn>(InterruptCheck, "InterruptCheck");

void
CodeGenerator::visitOutOfLineInterruptCheckImplicit(OutOfLineInterruptCheckImplicit* ool)
{
#ifdef CHECK_OSIPOINT_REGISTERS
    // This is path is entered from the patched back-edge of the loop. This
    // means that the JitAtivation flags used for checking the validity of the
    // OSI points are not reseted by the path generated by generateBody, so we
    // have to reset it here.
    resetOsiPointRegs(ool->lir->safepoint());
#endif

    LInstructionIterator iter = ool->block->begin();
    for (; iter != ool->block->end(); iter++) {
        if (iter->isMoveGroup()) {
            // Replay this move group that preceds the interrupt check at the
            // start of the loop header. Any incoming jumps here will be from
            // the backedge and will skip over the move group emitted inline.
            visitMoveGroup(iter->toMoveGroup());
        } else {
            break;
        }
    }
    MOZ_ASSERT(*iter == ool->lir);

    saveLive(ool->lir);
    callVM(InterruptCheckInfo, ool->lir);
    restoreLive(ool->lir);
    masm.jump(ool->rejoin());
}

void
CodeGenerator::visitTableSwitch(LTableSwitch* ins)
{
    MTableSwitch* mir = ins->mir();
    Label* defaultcase = skipTrivialBlocks(mir->getDefault())->lir()->label();
    const LAllocation* temp;

    if (mir->getOperand(0)->type() != MIRType::Int32) {
        temp = ins->tempInt()->output();

        // The input is a double, so try and convert it to an integer.
        // If it does not fit in an integer, take the default case.
        masm.convertDoubleToInt32(ToFloatRegister(ins->index()), ToRegister(temp), defaultcase, false);
    } else {
        temp = ins->index();
    }

    emitTableSwitchDispatch(mir, ToRegister(temp), ToRegisterOrInvalid(ins->tempPointer()));
}

void
CodeGenerator::visitTableSwitchV(LTableSwitchV* ins)
{
    MTableSwitch* mir = ins->mir();
    Label* defaultcase = skipTrivialBlocks(mir->getDefault())->lir()->label();

    Register index = ToRegister(ins->tempInt());
    ValueOperand value = ToValue(ins, LTableSwitchV::InputValue);
    Register tag = masm.extractTag(value, index);
    masm.branchTestNumber(Assembler::NotEqual, tag, defaultcase);

    Label unboxInt, isInt;
    masm.branchTestInt32(Assembler::Equal, tag, &unboxInt);
    {
        FloatRegister floatIndex = ToFloatRegister(ins->tempFloat());
        masm.unboxDouble(value, floatIndex);
        masm.convertDoubleToInt32(floatIndex, index, defaultcase, false);
        masm.jump(&isInt);
    }

    masm.bind(&unboxInt);
    masm.unboxInt32(value, index);

    masm.bind(&isInt);

    emitTableSwitchDispatch(mir, index, ToRegisterOrInvalid(ins->tempPointer()));
}

typedef JSObject* (*DeepCloneObjectLiteralFn)(JSContext*, HandleObject, NewObjectKind);
static const VMFunction DeepCloneObjectLiteralInfo =
    FunctionInfo<DeepCloneObjectLiteralFn>(DeepCloneObjectLiteral, "DeepCloneObjectLiteral");

void
CodeGenerator::visitCloneLiteral(LCloneLiteral* lir)
{
    pushArg(ImmWord(TenuredObject));
    pushArg(ToRegister(lir->getObjectLiteral()));
    callVM(DeepCloneObjectLiteralInfo, lir);
}

void
CodeGenerator::visitParameter(LParameter* lir)
{
}

void
CodeGenerator::visitCallee(LCallee* lir)
{
    Register callee = ToRegister(lir->output());
    Address ptr(masm.getStackPointer(), frameSize() + JitFrameLayout::offsetOfCalleeToken());

    masm.loadFunctionFromCalleeToken(ptr, callee);
}

void
CodeGenerator::visitIsConstructing(LIsConstructing* lir)
{
    Register output = ToRegister(lir->output());
    Address calleeToken(masm.getStackPointer(), frameSize() + JitFrameLayout::offsetOfCalleeToken());
    masm.loadPtr(calleeToken, output);

    // We must be inside a function.
    MOZ_ASSERT(current->mir()->info().script()->functionNonDelazifying());

    // The low bit indicates whether this call is constructing, just clear the
    // other bits.
    static_assert(CalleeToken_Function == 0x0, "CalleeTokenTag value should match");
    static_assert(CalleeToken_FunctionConstructing == 0x1, "CalleeTokenTag value should match");
    masm.andPtr(Imm32(0x1), output);
}

void
CodeGenerator::visitStart(LStart* lir)
{
}

void
CodeGenerator::visitReturn(LReturn* lir)
{
#if defined(JS_NUNBOX32)
    DebugOnly<LAllocation*> type    = lir->getOperand(TYPE_INDEX);
    DebugOnly<LAllocation*> payload = lir->getOperand(PAYLOAD_INDEX);
    MOZ_ASSERT(ToRegister(type)    == JSReturnReg_Type);
    MOZ_ASSERT(ToRegister(payload) == JSReturnReg_Data);
#elif defined(JS_PUNBOX64)
    DebugOnly<LAllocation*> result = lir->getOperand(0);
    MOZ_ASSERT(ToRegister(result) == JSReturnReg);
#endif
    // Don't emit a jump to the return label if this is the last block.
    if (current->mir() != *gen->graph().poBegin())
        masm.jump(&returnLabel_);
}

void
CodeGenerator::visitOsrEntry(LOsrEntry* lir)
{
    Register temp = ToRegister(lir->temp());

    // Remember the OSR entry offset into the code buffer.
    masm.flushBuffer();
    setOsrEntryOffset(masm.size());

#ifdef JS_TRACE_LOGGING
    emitTracelogStopEvent(TraceLogger_Baseline);
    emitTracelogStartEvent(TraceLogger_IonMonkey);
#endif

    // If profiling, save the current frame pointer to a per-thread global field.
    if (isProfilerInstrumentationEnabled())
        masm.profilerEnterFrame(masm.getStackPointer(), temp);

    // Allocate the full frame for this function
    // Note we have a new entry here. So we reset MacroAssembler::framePushed()
    // to 0, before reserving the stack.
    MOZ_ASSERT(masm.framePushed() == frameSize());
    masm.setFramePushed(0);

    // Ensure that the Ion frames is properly aligned.
    masm.assertStackAlignment(JitStackAlignment, 0);

    masm.reserveStack(frameSize());
}

void
CodeGenerator::visitOsrEnvironmentChain(LOsrEnvironmentChain* lir)
{
    const LAllocation* frame   = lir->getOperand(0);
    const LDefinition* object  = lir->getDef(0);

    const ptrdiff_t frameOffset = BaselineFrame::reverseOffsetOfEnvironmentChain();

    masm.loadPtr(Address(ToRegister(frame), frameOffset), ToRegister(object));
}

void
CodeGenerator::visitOsrArgumentsObject(LOsrArgumentsObject* lir)
{
    const LAllocation* frame   = lir->getOperand(0);
    const LDefinition* object  = lir->getDef(0);

    const ptrdiff_t frameOffset = BaselineFrame::reverseOffsetOfArgsObj();

    masm.loadPtr(Address(ToRegister(frame), frameOffset), ToRegister(object));
}

void
CodeGenerator::visitOsrValue(LOsrValue* value)
{
    const LAllocation* frame   = value->getOperand(0);
    const ValueOperand out     = ToOutValue(value);

    const ptrdiff_t frameOffset = value->mir()->frameOffset();

    masm.loadValue(Address(ToRegister(frame), frameOffset), out);
}

void
CodeGenerator::visitOsrReturnValue(LOsrReturnValue* lir)
{
    const LAllocation* frame   = lir->getOperand(0);
    const ValueOperand out     = ToOutValue(lir);

    Address flags = Address(ToRegister(frame), BaselineFrame::reverseOffsetOfFlags());
    Address retval = Address(ToRegister(frame), BaselineFrame::reverseOffsetOfReturnValue());

    masm.moveValue(UndefinedValue(), out);

    Label done;
    masm.branchTest32(Assembler::Zero, flags, Imm32(BaselineFrame::HAS_RVAL), &done);
    masm.loadValue(retval, out);
    masm.bind(&done);
}

void
CodeGenerator::visitStackArgT(LStackArgT* lir)
{
    const LAllocation* arg = lir->getArgument();
    MIRType argType = lir->type();
    uint32_t argslot = lir->argslot();
    MOZ_ASSERT(argslot - 1u < graph.argumentSlotCount());

    int32_t stack_offset = StackOffsetOfPassedArg(argslot);
    Address dest(masm.getStackPointer(), stack_offset);

    if (arg->isFloatReg())
        masm.storeDouble(ToFloatRegister(arg), dest);
    else if (arg->isRegister())
        masm.storeValue(ValueTypeFromMIRType(argType), ToRegister(arg), dest);
    else
        masm.storeValue(arg->toConstant()->toJSValue(), dest);
}

void
CodeGenerator::visitStackArgV(LStackArgV* lir)
{
    ValueOperand val = ToValue(lir, 0);
    uint32_t argslot = lir->argslot();
    MOZ_ASSERT(argslot - 1u < graph.argumentSlotCount());

    int32_t stack_offset = StackOffsetOfPassedArg(argslot);

    masm.storeValue(val, Address(masm.getStackPointer(), stack_offset));
}

void
CodeGenerator::visitMoveGroup(LMoveGroup* group)
{
    if (!group->numMoves())
        return;

    MoveResolver& resolver = masm.moveResolver();

    for (size_t i = 0; i < group->numMoves(); i++) {
        const LMove& move = group->getMove(i);

        LAllocation from = move.from();
        LAllocation to = move.to();
        LDefinition::Type type = move.type();

        // No bogus moves.
        MOZ_ASSERT(from != to);
        MOZ_ASSERT(!from.isConstant());
        MoveOp::Type moveType;
        switch (type) {
          case LDefinition::OBJECT:
          case LDefinition::SLOTS:
#ifdef JS_NUNBOX32
          case LDefinition::TYPE:
          case LDefinition::PAYLOAD:
#else
          case LDefinition::BOX:
#endif
          case LDefinition::GENERAL:      moveType = MoveOp::GENERAL;      break;
          case LDefinition::INT32:        moveType = MoveOp::INT32;        break;
          case LDefinition::FLOAT32:      moveType = MoveOp::FLOAT32;      break;
          case LDefinition::DOUBLE:       moveType = MoveOp::DOUBLE;       break;
          case LDefinition::SIMD128INT:   moveType = MoveOp::SIMD128INT;   break;
          case LDefinition::SIMD128FLOAT: moveType = MoveOp::SIMD128FLOAT; break;
          default: MOZ_CRASH("Unexpected move type");
        }

        masm.propagateOOM(resolver.addMove(toMoveOperand(from), toMoveOperand(to), moveType));
    }

    masm.propagateOOM(resolver.resolve());
    if (masm.oom())
        return;

    MoveEmitter emitter(masm);

#ifdef JS_CODEGEN_X86
    if (group->maybeScratchRegister().isGeneralReg())
        emitter.setScratchRegister(group->maybeScratchRegister().toGeneralReg()->reg());
    else
        resolver.sortMemoryToMemoryMoves();
#endif

    emitter.emit(resolver);
    emitter.finish();
}

void
CodeGenerator::visitInteger(LInteger* lir)
{
    masm.move32(Imm32(lir->getValue()), ToRegister(lir->output()));
}

void
CodeGenerator::visitInteger64(LInteger64* lir)
{
    masm.move64(Imm64(lir->getValue()), ToOutRegister64(lir));
}

void
CodeGenerator::visitPointer(LPointer* lir)
{
    if (lir->kind() == LPointer::GC_THING)
        masm.movePtr(ImmGCPtr(lir->gcptr()), ToRegister(lir->output()));
    else
        masm.movePtr(ImmPtr(lir->ptr()), ToRegister(lir->output()));
}

void
CodeGenerator::visitKeepAliveObject(LKeepAliveObject* lir)
{
    // No-op.
}

void
CodeGenerator::visitSlots(LSlots* lir)
{
    Address slots(ToRegister(lir->object()), NativeObject::offsetOfSlots());
    masm.loadPtr(slots, ToRegister(lir->output()));
}

void
CodeGenerator::visitLoadSlotT(LLoadSlotT* lir)
{
    Register base = ToRegister(lir->slots());
    int32_t offset = lir->mir()->slot() * sizeof(js::Value);
    AnyRegister result = ToAnyRegister(lir->output());

    masm.loadUnboxedValue(Address(base, offset), lir->mir()->type(), result);
}

void
CodeGenerator::visitLoadSlotV(LLoadSlotV* lir)
{
    ValueOperand dest = ToOutValue(lir);
    Register base = ToRegister(lir->input());
    int32_t offset = lir->mir()->slot() * sizeof(js::Value);

    masm.loadValue(Address(base, offset), dest);
}

void
CodeGenerator::visitStoreSlotT(LStoreSlotT* lir)
{
    Register base = ToRegister(lir->slots());
    int32_t offset = lir->mir()->slot() * sizeof(js::Value);
    Address dest(base, offset);

    if (lir->mir()->needsBarrier())
        emitPreBarrier(dest);

    MIRType valueType = lir->mir()->value()->type();

    if (valueType == MIRType::ObjectOrNull) {
        masm.storeObjectOrNull(ToRegister(lir->value()), dest);
    } else {
        ConstantOrRegister value;
        if (lir->value()->isConstant())
            value = ConstantOrRegister(lir->value()->toConstant()->toJSValue());
        else
            value = TypedOrValueRegister(valueType, ToAnyRegister(lir->value()));
        masm.storeUnboxedValue(value, valueType, dest, lir->mir()->slotType());
    }
}

void
CodeGenerator::visitStoreSlotV(LStoreSlotV* lir)
{
    Register base = ToRegister(lir->slots());
    int32_t offset = lir->mir()->slot() * sizeof(Value);

    const ValueOperand value = ToValue(lir, LStoreSlotV::Value);

    if (lir->mir()->needsBarrier())
       emitPreBarrier(Address(base, offset));

    masm.storeValue(value, Address(base, offset));
}

static void
GuardReceiver(MacroAssembler& masm, const ReceiverGuard& guard,
              Register obj, Register expandoScratch, Register scratch, Label* miss,
              bool checkNullExpando)
{
    if (guard.group) {
        masm.branchTestObjGroup(Assembler::NotEqual, obj, guard.group, scratch, obj, miss);

        Address expandoAddress(obj, UnboxedPlainObject::offsetOfExpando());
        if (guard.shape) {
            masm.loadPtr(expandoAddress, expandoScratch);
            masm.branchPtr(Assembler::Equal, expandoScratch, ImmWord(0), miss);
            masm.branchTestObjShape(Assembler::NotEqual, expandoScratch, guard.shape, scratch,
                                    expandoScratch, miss);
        } else if (checkNullExpando) {
            masm.branchPtr(Assembler::NotEqual, expandoAddress, ImmWord(0), miss);
        }
    } else {
        masm.branchTestObjShape(Assembler::NotEqual, obj, guard.shape, scratch, obj, miss);
    }
}

void
CodeGenerator::emitGetPropertyPolymorphic(LInstruction* ins, Register obj, Register expandoScratch,
                                          Register scratch,
                                          const TypedOrValueRegister& output)
{
    MGetPropertyPolymorphic* mir = ins->mirRaw()->toGetPropertyPolymorphic();

    Label done;

    for (size_t i = 0; i < mir->numReceivers(); i++) {
        ReceiverGuard receiver = mir->receiver(i);

        Label next;
        masm.comment("GuardReceiver");
        GuardReceiver(masm, receiver, obj, expandoScratch, scratch, &next,
                      /* checkNullExpando = */ false);

        if (receiver.shape) {
            masm.comment("loadTypedOrValue");
            // If this is an unboxed expando access, GuardReceiver loaded the
            // expando object into expandoScratch.
            Register target = receiver.group ? expandoScratch : obj;

            Shape* shape = mir->shape(i);
            if (shape->slot() < shape->numFixedSlots()) {
                // Fixed slot.
                masm.loadTypedOrValue(Address(target, NativeObject::getFixedSlotOffset(shape->slot())),
                                      output);
            } else {
                // Dynamic slot.
                uint32_t offset = (shape->slot() - shape->numFixedSlots()) * sizeof(js::Value);
                masm.loadPtr(Address(target, NativeObject::offsetOfSlots()), scratch);
                masm.loadTypedOrValue(Address(scratch, offset), output);
            }
        } else {
            masm.comment("loadUnboxedProperty");
            const UnboxedLayout::Property* property =
                receiver.group->unboxedLayout().lookup(mir->name());
            Address propertyAddr(obj, UnboxedPlainObject::offsetOfData() + property->offset);

            masm.loadUnboxedProperty(propertyAddr, property->type, output);
        }

        if (i == mir->numReceivers() - 1) {
            bailoutFrom(&next, ins->snapshot());
        } else {
            masm.jump(&done);
            masm.bind(&next);
        }
    }

    masm.bind(&done);
}

void
CodeGenerator::visitGetPropertyPolymorphicV(LGetPropertyPolymorphicV* ins)
{
    Register obj = ToRegister(ins->obj());
    ValueOperand output = ToOutValue(ins);
    Register temp = ToRegister(ins->temp());
    emitGetPropertyPolymorphic(ins, obj, output.scratchReg(), temp, output);
}

void
CodeGenerator::visitGetPropertyPolymorphicT(LGetPropertyPolymorphicT* ins)
{
    Register obj = ToRegister(ins->obj());
    TypedOrValueRegister output(ins->mir()->type(), ToAnyRegister(ins->output()));
    Register temp1 = ToRegister(ins->temp1());
    Register temp2 = (output.type() == MIRType::Double)
                     ? ToRegister(ins->temp2())
                     : output.typedReg().gpr();
    emitGetPropertyPolymorphic(ins, obj, temp1, temp2, output);
}

template <typename T>
static void
EmitUnboxedPreBarrier(MacroAssembler &masm, T address, JSValueType type)
{
    if (type == JSVAL_TYPE_OBJECT)
        masm.guardedCallPreBarrier(address, MIRType::Object);
    else if (type == JSVAL_TYPE_STRING)
        masm.guardedCallPreBarrier(address, MIRType::String);
    else
        MOZ_ASSERT(!UnboxedTypeNeedsPreBarrier(type));
}

void
CodeGenerator::emitSetPropertyPolymorphic(LInstruction* ins, Register obj, Register expandoScratch,
                                          Register scratch, const ConstantOrRegister& value)
{
    MSetPropertyPolymorphic* mir = ins->mirRaw()->toSetPropertyPolymorphic();

    Label done;
    for (size_t i = 0; i < mir->numReceivers(); i++) {
        ReceiverGuard receiver = mir->receiver(i);

        Label next;
        GuardReceiver(masm, receiver, obj, expandoScratch, scratch, &next,
                      /* checkNullExpando = */ false);

        if (receiver.shape) {
            // If this is an unboxed expando access, GuardReceiver loaded the
            // expando object into expandoScratch.
            Register target = receiver.group ? expandoScratch : obj;

            Shape* shape = mir->shape(i);
            if (shape->slot() < shape->numFixedSlots()) {
                // Fixed slot.
                Address addr(target, NativeObject::getFixedSlotOffset(shape->slot()));
                if (mir->needsBarrier())
                    emitPreBarrier(addr);
                masm.storeConstantOrRegister(value, addr);
            } else {
                // Dynamic slot.
                masm.loadPtr(Address(target, NativeObject::offsetOfSlots()), scratch);
                Address addr(scratch, (shape->slot() - shape->numFixedSlots()) * sizeof(js::Value));
                if (mir->needsBarrier())
                    emitPreBarrier(addr);
                masm.storeConstantOrRegister(value, addr);
            }
        } else {
            const UnboxedLayout::Property* property =
                receiver.group->unboxedLayout().lookup(mir->name());
            Address propertyAddr(obj, UnboxedPlainObject::offsetOfData() + property->offset);

            EmitUnboxedPreBarrier(masm, propertyAddr, property->type);
            masm.storeUnboxedProperty(propertyAddr, property->type, value, nullptr);
        }

        if (i == mir->numReceivers() - 1) {
            bailoutFrom(&next, ins->snapshot());
        } else {
            masm.jump(&done);
            masm.bind(&next);
        }
    }

    masm.bind(&done);
}

void
CodeGenerator::visitSetPropertyPolymorphicV(LSetPropertyPolymorphicV* ins)
{
    Register obj = ToRegister(ins->obj());
    Register temp1 = ToRegister(ins->temp1());
    Register temp2 = ToRegister(ins->temp2());
    ValueOperand value = ToValue(ins, LSetPropertyPolymorphicV::Value);
    emitSetPropertyPolymorphic(ins, obj, temp1, temp2, TypedOrValueRegister(value));
}

void
CodeGenerator::visitSetPropertyPolymorphicT(LSetPropertyPolymorphicT* ins)
{
    Register obj = ToRegister(ins->obj());
    Register temp1 = ToRegister(ins->temp1());
    Register temp2 = ToRegister(ins->temp2());

    ConstantOrRegister value;
    if (ins->mir()->value()->isConstant())
        value = ConstantOrRegister(ins->mir()->value()->toConstant()->toJSValue());
    else
        value = TypedOrValueRegister(ins->mir()->value()->type(), ToAnyRegister(ins->value()));

    emitSetPropertyPolymorphic(ins, obj, temp1, temp2, value);
}

void
CodeGenerator::visitElements(LElements* lir)
{
    Address elements(ToRegister(lir->object()), NativeObject::offsetOfElements());
    masm.loadPtr(elements, ToRegister(lir->output()));
}

typedef bool (*ConvertElementsToDoublesFn)(JSContext*, uintptr_t);
static const VMFunction ConvertElementsToDoublesInfo =
    FunctionInfo<ConvertElementsToDoublesFn>(ObjectElements::ConvertElementsToDoubles,
                                             "ObjectElements::ConvertElementsToDoubles");

void
CodeGenerator::visitConvertElementsToDoubles(LConvertElementsToDoubles* lir)
{
    Register elements = ToRegister(lir->elements());

    OutOfLineCode* ool = oolCallVM(ConvertElementsToDoublesInfo, lir,
                                   ArgList(elements), StoreNothing());

    Address convertedAddress(elements, ObjectElements::offsetOfFlags());
    Imm32 bit(ObjectElements::CONVERT_DOUBLE_ELEMENTS);
    masm.branchTest32(Assembler::Zero, convertedAddress, bit, ool->entry());
    masm.bind(ool->rejoin());
}

void
CodeGenerator::visitMaybeToDoubleElement(LMaybeToDoubleElement* lir)
{
    Register elements = ToRegister(lir->elements());
    Register value = ToRegister(lir->value());
    ValueOperand out = ToOutValue(lir);

    FloatRegister temp = ToFloatRegister(lir->tempFloat());
    Label convert, done;

    // If the CONVERT_DOUBLE_ELEMENTS flag is set, convert the int32
    // value to double. Else, just box it.
    masm.branchTest32(Assembler::NonZero,
                      Address(elements, ObjectElements::offsetOfFlags()),
                      Imm32(ObjectElements::CONVERT_DOUBLE_ELEMENTS),
                      &convert);

    masm.tagValue(JSVAL_TYPE_INT32, value, out);
    masm.jump(&done);

    masm.bind(&convert);
    masm.convertInt32ToDouble(value, temp);
    masm.boxDouble(temp, out, temp);

    masm.bind(&done);
}

typedef bool (*CopyElementsForWriteFn)(JSContext*, NativeObject*);
static const VMFunction CopyElementsForWriteInfo =
    FunctionInfo<CopyElementsForWriteFn>(NativeObject::CopyElementsForWrite,
                                         "NativeObject::CopyElementsForWrite");

void
CodeGenerator::visitMaybeCopyElementsForWrite(LMaybeCopyElementsForWrite* lir)
{
    Register object = ToRegister(lir->object());
    Register temp = ToRegister(lir->temp());

    OutOfLineCode* ool = oolCallVM(CopyElementsForWriteInfo, lir,
                                   ArgList(object), StoreNothing());

    if (lir->mir()->checkNative())
        masm.branchIfNonNativeObj(object, temp, ool->rejoin());

    masm.loadPtr(Address(object, NativeObject::offsetOfElements()), temp);
    masm.branchTest32(Assembler::NonZero,
                      Address(temp, ObjectElements::offsetOfFlags()),
                      Imm32(ObjectElements::COPY_ON_WRITE),
                      ool->entry());
    masm.bind(ool->rejoin());
}

void
CodeGenerator::visitFunctionEnvironment(LFunctionEnvironment* lir)
{
    Address environment(ToRegister(lir->function()), JSFunction::offsetOfEnvironment());
    masm.loadPtr(environment, ToRegister(lir->output()));
}

void
CodeGenerator::visitHomeObject(LHomeObject* lir)
{
    Address homeObject(ToRegister(lir->function()), FunctionExtended::offsetOfMethodHomeObjectSlot());
#ifdef DEBUG
    Label isObject;
    masm.branchTestObject(Assembler::Equal, homeObject, &isObject);
    masm.assumeUnreachable("[[HomeObject]] must be Object");
    masm.bind(&isObject);
#endif
    masm.unboxObject(homeObject, ToRegister(lir->output()));
}

typedef JSObject* (*HomeObjectSuperBaseFn)(JSContext*, HandleObject);
static const VMFunction HomeObjectSuperBaseInfo =
    FunctionInfo<HomeObjectSuperBaseFn>(HomeObjectSuperBase, "HomeObjectSuperBase");

void
CodeGenerator::visitHomeObjectSuperBase(LHomeObjectSuperBase* lir)
{
    Register homeObject = ToRegister(lir->homeObject());
    Register output = ToRegister(lir->output());

    OutOfLineCode* ool = oolCallVM(HomeObjectSuperBaseInfo, lir, ArgList(homeObject),
                                   StoreRegisterTo(output));

    masm.loadObjProto(homeObject, output);
    masm.branchPtr(Assembler::BelowOrEqual, output, ImmWord(1), ool->entry());
    masm.bind(ool->rejoin());
}

typedef LexicalEnvironmentObject* (*NewLexicalEnvironmentObjectFn)(JSContext*,
                                                                   Handle<LexicalScope*>,
                                                                   HandleObject, gc::InitialHeap);
static const VMFunction NewLexicalEnvironmentObjectInfo =
    FunctionInfo<NewLexicalEnvironmentObjectFn>(LexicalEnvironmentObject::create,
                                                "LexicalEnvironmentObject::create");

void
CodeGenerator::visitNewLexicalEnvironmentObject(LNewLexicalEnvironmentObject* lir)
{
    pushArg(Imm32(gc::DefaultHeap));
    pushArg(ToRegister(lir->enclosing()));
    pushArg(ImmGCPtr(lir->mir()->scope()));
    callVM(NewLexicalEnvironmentObjectInfo, lir);
}

typedef JSObject* (*CopyLexicalEnvironmentObjectFn)(JSContext*, HandleObject, bool);
static const VMFunction CopyLexicalEnvironmentObjectInfo =
    FunctionInfo<CopyLexicalEnvironmentObjectFn>(js::jit::CopyLexicalEnvironmentObject,
                                                "js::jit::CopyLexicalEnvironmentObject");

void
CodeGenerator::visitCopyLexicalEnvironmentObject(LCopyLexicalEnvironmentObject* lir)
{
    pushArg(Imm32(lir->mir()->copySlots()));
    pushArg(ToRegister(lir->env()));
    callVM(CopyLexicalEnvironmentObjectInfo, lir);
}

void
CodeGenerator::visitGuardShape(LGuardShape* guard)
{
    Register obj = ToRegister(guard->input());
    Register temp = ToTempRegisterOrInvalid(guard->temp());
    Label bail;
    masm.branchTestObjShape(Assembler::NotEqual, obj, guard->mir()->shape(), temp, obj, &bail);
    bailoutFrom(&bail, guard->snapshot());
}

void
CodeGenerator::visitGuardObjectGroup(LGuardObjectGroup* guard)
{
    Register obj = ToRegister(guard->input());
    Register temp = ToTempRegisterOrInvalid(guard->temp());
    Assembler::Condition cond =
        guard->mir()->bailOnEquality() ? Assembler::Equal : Assembler::NotEqual;
    Label bail;
    masm.branchTestObjGroup(cond, obj, guard->mir()->group(), temp, obj, &bail);
    bailoutFrom(&bail, guard->snapshot());
}

void
CodeGenerator::visitGuardObjectIdentity(LGuardObjectIdentity* guard)
{
    Register input = ToRegister(guard->input());
    Register expected = ToRegister(guard->expected());

    Assembler::Condition cond =
        guard->mir()->bailOnEquality() ? Assembler::Equal : Assembler::NotEqual;
    bailoutCmpPtr(cond, input, expected, guard->snapshot());
}

void
CodeGenerator::visitGuardReceiverPolymorphic(LGuardReceiverPolymorphic* lir)
{
    const MGuardReceiverPolymorphic* mir = lir->mir();
    Register obj = ToRegister(lir->object());
    Register temp1 = ToRegister(lir->temp1());
    Register temp2 = ToRegister(lir->temp2());

    Label done;

    for (size_t i = 0; i < mir->numReceivers(); i++) {
        const ReceiverGuard& receiver = mir->receiver(i);

        Label next;
        GuardReceiver(masm, receiver, obj, temp1, temp2, &next, /* checkNullExpando = */ true);

        if (i == mir->numReceivers() - 1) {
            bailoutFrom(&next, lir->snapshot());
        } else {
            masm.jump(&done);
            masm.bind(&next);
        }
    }

    masm.bind(&done);
}

void
CodeGenerator::visitGuardUnboxedExpando(LGuardUnboxedExpando* lir)
{
    Label miss;

    Register obj = ToRegister(lir->object());
    masm.branchPtr(lir->mir()->requireExpando() ? Assembler::Equal : Assembler::NotEqual,
                   Address(obj, UnboxedPlainObject::offsetOfExpando()), ImmWord(0), &miss);

    bailoutFrom(&miss, lir->snapshot());
}

void
CodeGenerator::visitLoadUnboxedExpando(LLoadUnboxedExpando* lir)
{
    Register obj = ToRegister(lir->object());
    Register result = ToRegister(lir->getDef(0));

    masm.loadPtr(Address(obj, UnboxedPlainObject::offsetOfExpando()), result);
}

void
CodeGenerator::visitTypeBarrierV(LTypeBarrierV* lir)
{
    ValueOperand operand = ToValue(lir, LTypeBarrierV::Input);
    Register unboxScratch = ToTempRegisterOrInvalid(lir->unboxTemp());
    Register objScratch = ToTempRegisterOrInvalid(lir->objTemp());

    // guardObjectType may zero the payload/Value register on speculative paths
    // (we should have a defineReuseInput allocation in this case).
    Register spectreRegToZero = operand.payloadOrValueReg();

    Label miss;
    masm.guardTypeSet(operand, lir->mir()->resultTypeSet(), lir->mir()->barrierKind(),
                      unboxScratch, objScratch, spectreRegToZero, &miss);
    bailoutFrom(&miss, lir->snapshot());
}

void
CodeGenerator::visitTypeBarrierO(LTypeBarrierO* lir)
{
    Register obj = ToRegister(lir->object());
    Register scratch = ToTempRegisterOrInvalid(lir->temp());
    Label miss, ok;

    if (lir->mir()->type() == MIRType::ObjectOrNull) {
        masm.comment("Object or Null");
        Label* nullTarget = lir->mir()->resultTypeSet()->mightBeMIRType(MIRType::Null) ? &ok : &miss;
        masm.branchTestPtr(Assembler::Zero, obj, obj, nullTarget);
    } else {
        MOZ_ASSERT(lir->mir()->type() == MIRType::Object);
        MOZ_ASSERT(lir->mir()->barrierKind() != BarrierKind::TypeTagOnly);
    }

    if (lir->mir()->barrierKind() != BarrierKind::TypeTagOnly) {
        masm.comment("Type tag only");
        // guardObjectType may zero the object register on speculative paths
        // (we should have a defineReuseInput allocation in this case).
        Register spectreRegToZero = obj;
        masm.guardObjectType(obj, lir->mir()->resultTypeSet(), scratch, spectreRegToZero, &miss);
    }

    bailoutFrom(&miss, lir->snapshot());
    masm.bind(&ok);
}

// Out-of-line path to update the store buffer.
class OutOfLineCallPostWriteBarrier : public OutOfLineCodeBase<CodeGenerator>
{
    LInstruction* lir_;
    const LAllocation* object_;

  public:
    OutOfLineCallPostWriteBarrier(LInstruction* lir, const LAllocation* object)
      : lir_(lir), object_(object)
    { }

    void accept(CodeGenerator* codegen) override {
        codegen->visitOutOfLineCallPostWriteBarrier(this);
    }

    LInstruction* lir() const {
        return lir_;
    }
    const LAllocation* object() const {
        return object_;
    }
};

static void
EmitStoreBufferCheckForConstant(MacroAssembler& masm, const gc::TenuredCell* cell,
                                AllocatableGeneralRegisterSet& regs, Label* exit, Label* callVM)
{
    Register temp = regs.takeAny();

    gc::Arena* arena = cell->arena();

    Register cells = temp;
    masm.loadPtr(AbsoluteAddress(&arena->bufferedCells()), cells);

    size_t index = gc::ArenaCellSet::getCellIndex(cell);
    size_t word;
    uint32_t mask;
    gc::ArenaCellSet::getWordIndexAndMask(index, &word, &mask);
    size_t offset = gc::ArenaCellSet::offsetOfBits() + word * sizeof(uint32_t);

    masm.branchTest32(Assembler::NonZero, Address(cells, offset), Imm32(mask), exit);

    // Check whether this is the sentinel set and if so call the VM to allocate
    // one for this arena.
    masm.branchPtr(Assembler::Equal, Address(cells, gc::ArenaCellSet::offsetOfArena()),
                   ImmPtr(nullptr), callVM);

    // Add the cell to the set.
    masm.or32(Imm32(mask), Address(cells, offset));
    masm.jump(exit);

    regs.add(temp);
}

static void
EmitPostWriteBarrier(MacroAssembler& masm, CompileRuntime* runtime, Register objreg,
                     JSObject* maybeConstant, bool isGlobal, AllocatableGeneralRegisterSet& regs)
{
    MOZ_ASSERT_IF(isGlobal, maybeConstant);

    Label callVM;
    Label exit;

    // We already have a fast path to check whether a global is in the store
    // buffer.
    if (!isGlobal && maybeConstant)
        EmitStoreBufferCheckForConstant(masm, &maybeConstant->asTenured(), regs, &exit, &callVM);

    // Call into the VM to barrier the write.
    masm.bind(&callVM);

    Register runtimereg = regs.takeAny();
    masm.mov(ImmPtr(runtime), runtimereg);

    void (*fun)(JSRuntime*, JSObject*) = isGlobal ? PostGlobalWriteBarrier : PostWriteBarrier;
    masm.setupUnalignedABICall(regs.takeAny());
    masm.passABIArg(runtimereg);
    masm.passABIArg(objreg);
    masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, fun));

    masm.bind(&exit);
}

void
CodeGenerator::emitPostWriteBarrier(const LAllocation* obj)
{
    AllocatableGeneralRegisterSet regs(GeneralRegisterSet::Volatile());

    Register objreg;
    JSObject* object = nullptr;
    bool isGlobal = false;
    if (obj->isConstant()) {
        object = &obj->toConstant()->toObject();
        isGlobal = isGlobalObject(object);
        objreg = regs.takeAny();
        masm.movePtr(ImmGCPtr(object), objreg);
    } else {
        objreg = ToRegister(obj);
        regs.takeUnchecked(objreg);
    }

    EmitPostWriteBarrier(masm, gen->runtime, objreg, object, isGlobal, regs);
}

void
CodeGenerator::emitPostWriteBarrier(Register objreg)
{
    AllocatableGeneralRegisterSet regs(GeneralRegisterSet::Volatile());
    regs.takeUnchecked(objreg);
    EmitPostWriteBarrier(masm, gen->runtime, objreg, nullptr, false, regs);
}

void
CodeGenerator::visitOutOfLineCallPostWriteBarrier(OutOfLineCallPostWriteBarrier* ool)
{
    saveLiveVolatile(ool->lir());
    const LAllocation* obj = ool->object();
    emitPostWriteBarrier(obj);
    restoreLiveVolatile(ool->lir());

    masm.jump(ool->rejoin());
}

void
CodeGenerator::maybeEmitGlobalBarrierCheck(const LAllocation* maybeGlobal, OutOfLineCode* ool)
{
    // Check whether an object is a global that we have already barriered before
    // calling into the VM.

    if (!maybeGlobal->isConstant())
        return;

    JSObject* obj = &maybeGlobal->toConstant()->toObject();
    if (!isGlobalObject(obj))
        return;

    JSCompartment* comp = obj->compartment();
    auto addr = AbsoluteAddress(&comp->globalWriteBarriered);
    masm.branch32(Assembler::NotEqual, addr, Imm32(0), ool->rejoin());
}

template <class LPostBarrierType, MIRType nurseryType>
void
CodeGenerator::visitPostWriteBarrierCommon(LPostBarrierType* lir, OutOfLineCode* ool)
{
    addOutOfLineCode(ool, lir->mir());

    Register temp = ToTempRegisterOrInvalid(lir->temp());

    if (lir->object()->isConstant()) {
        // Constant nursery objects cannot appear here, see
        // LIRGenerator::visitPostWriteElementBarrier.
        MOZ_ASSERT(!IsInsideNursery(&lir->object()->toConstant()->toObject()));
    } else {
        masm.branchPtrInNurseryChunk(Assembler::Equal, ToRegister(lir->object()), temp,
                                     ool->rejoin());
    }

    maybeEmitGlobalBarrierCheck(lir->object(), ool);

    Register value = ToRegister(lir->value());
    if (nurseryType == MIRType::Object) {
        if (lir->mir()->value()->type() == MIRType::ObjectOrNull)
            masm.branchTestPtr(Assembler::Zero, value, value, ool->rejoin());
        else
            MOZ_ASSERT(lir->mir()->value()->type() == MIRType::Object);
    } else {
        MOZ_ASSERT(nurseryType == MIRType::String);
        MOZ_ASSERT(lir->mir()->value()->type() == MIRType::String);
    }
    masm.branchPtrInNurseryChunk(Assembler::Equal, value, temp, ool->entry());

    masm.bind(ool->rejoin());
}

template <class LPostBarrierType>
void
CodeGenerator::visitPostWriteBarrierCommonV(LPostBarrierType* lir, OutOfLineCode* ool)
{
    addOutOfLineCode(ool, lir->mir());

    Register temp = ToTempRegisterOrInvalid(lir->temp());

    if (lir->object()->isConstant()) {
        // Constant nursery objects cannot appear here, see LIRGenerator::visitPostWriteElementBarrier.
        MOZ_ASSERT(!IsInsideNursery(&lir->object()->toConstant()->toObject()));
    } else {
        masm.branchPtrInNurseryChunk(Assembler::Equal, ToRegister(lir->object()), temp,
                                     ool->rejoin());
    }

    maybeEmitGlobalBarrierCheck(lir->object(), ool);

    ValueOperand value = ToValue(lir, LPostBarrierType::Input);
    // Bug 1386094 - most callers only need to check for object or string, not
    // both.
    masm.branchValueIsNurseryCell(Assembler::Equal, value, temp, ool->entry());

    masm.bind(ool->rejoin());
}

void
CodeGenerator::visitPostWriteBarrierO(LPostWriteBarrierO* lir)
{
    auto ool = new(alloc()) OutOfLineCallPostWriteBarrier(lir, lir->object());
    visitPostWriteBarrierCommon<LPostWriteBarrierO, MIRType::Object>(lir, ool);
}

void
CodeGenerator::visitPostWriteBarrierS(LPostWriteBarrierS* lir)
{
    auto ool = new(alloc()) OutOfLineCallPostWriteBarrier(lir, lir->object());
    visitPostWriteBarrierCommon<LPostWriteBarrierS, MIRType::String>(lir, ool);
}

void
CodeGenerator::visitPostWriteBarrierV(LPostWriteBarrierV* lir)
{
    auto ool = new(alloc()) OutOfLineCallPostWriteBarrier(lir, lir->object());
    visitPostWriteBarrierCommonV(lir, ool);
}

// Out-of-line path to update the store buffer.
class OutOfLineCallPostWriteElementBarrier : public OutOfLineCodeBase<CodeGenerator>
{
    LInstruction* lir_;
    const LAllocation* object_;
    const LAllocation* index_;

  public:
    OutOfLineCallPostWriteElementBarrier(LInstruction* lir, const LAllocation* object,
                                         const LAllocation* index)
      : lir_(lir),
        object_(object),
        index_(index)
    { }

    void accept(CodeGenerator* codegen) override {
        codegen->visitOutOfLineCallPostWriteElementBarrier(this);
    }

    LInstruction* lir() const {
        return lir_;
    }

    const LAllocation* object() const {
        return object_;
    }

    const LAllocation* index() const {
        return index_;
    }
};

void
CodeGenerator::visitOutOfLineCallPostWriteElementBarrier(OutOfLineCallPostWriteElementBarrier* ool)
{
    saveLiveVolatile(ool->lir());

    const LAllocation* obj = ool->object();
    const LAllocation* index = ool->index();

    Register objreg = obj->isConstant() ? InvalidReg : ToRegister(obj);
    Register indexreg = ToRegister(index);

    AllocatableGeneralRegisterSet regs(GeneralRegisterSet::Volatile());
    regs.takeUnchecked(indexreg);

    if (obj->isConstant()) {
        objreg = regs.takeAny();
        masm.movePtr(ImmGCPtr(&obj->toConstant()->toObject()), objreg);
    } else {
        regs.takeUnchecked(objreg);
    }

    Register runtimereg = regs.takeAny();
    masm.setupUnalignedABICall(runtimereg);
    masm.mov(ImmPtr(gen->runtime), runtimereg);
    masm.passABIArg(runtimereg);
    masm.passABIArg(objreg);
    masm.passABIArg(indexreg);
    masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, (PostWriteElementBarrier<IndexInBounds::Maybe>)));

    restoreLiveVolatile(ool->lir());

    masm.jump(ool->rejoin());
}

void
CodeGenerator::visitPostWriteElementBarrierO(LPostWriteElementBarrierO* lir)
{
    auto ool = new(alloc()) OutOfLineCallPostWriteElementBarrier(lir, lir->object(), lir->index());
    visitPostWriteBarrierCommon<LPostWriteElementBarrierO, MIRType::Object>(lir, ool);
}

void
CodeGenerator::visitPostWriteElementBarrierS(LPostWriteElementBarrierS* lir)
{
    auto ool = new(alloc()) OutOfLineCallPostWriteElementBarrier(lir, lir->object(), lir->index());
    visitPostWriteBarrierCommon<LPostWriteElementBarrierS, MIRType::String>(lir, ool);
}

void
CodeGenerator::visitPostWriteElementBarrierV(LPostWriteElementBarrierV* lir)
{
    auto ool = new(alloc()) OutOfLineCallPostWriteElementBarrier(lir, lir->object(), lir->index());
    visitPostWriteBarrierCommonV(lir, ool);
}

void
CodeGenerator::visitCallNative(LCallNative* call)
{
    WrappedFunction* target = call->getSingleTarget();
    MOZ_ASSERT(target);
    MOZ_ASSERT(target->isNativeWithCppEntry());

    int callargslot = call->argslot();
    int unusedStack = StackOffsetOfPassedArg(callargslot);

    // Registers used for callWithABI() argument-passing.
    const Register argContextReg   = ToRegister(call->getArgContextReg());
    const Register argUintNReg     = ToRegister(call->getArgUintNReg());
    const Register argVpReg        = ToRegister(call->getArgVpReg());

    // Misc. temporary registers.
    const Register tempReg = ToRegister(call->getTempReg());

    DebugOnly<uint32_t> initialStack = masm.framePushed();

    masm.checkStackAlignment();

    // Native functions have the signature:
    //  bool (*)(JSContext*, unsigned, Value* vp)
    // Where vp[0] is space for an outparam, vp[1] is |this|, and vp[2] onward
    // are the function arguments.

    // Allocate space for the outparam, moving the StackPointer to what will be &vp[1].
    masm.adjustStack(unusedStack);

    // Push a Value containing the callee object: natives are allowed to access
    // their callee before setting the return value. The StackPointer is moved
    // to &vp[0].
    masm.Push(ObjectValue(*target->rawJSFunction()));

    // Preload arguments into registers.
    masm.loadJSContext(argContextReg);
    masm.move32(Imm32(call->numActualArgs()), argUintNReg);
    masm.moveStackPtrTo(argVpReg);

    masm.Push(argUintNReg);

    // Construct native exit frame.
    uint32_t safepointOffset = masm.buildFakeExitFrame(tempReg);
    masm.enterFakeExitFrameForNative(argContextReg, tempReg, call->mir()->isConstructing());

    markSafepointAt(safepointOffset, call);

    emitTracelogStartEvent(TraceLogger_Call);

    // Construct and execute call.
    masm.setupUnalignedABICall(tempReg);
    masm.passABIArg(argContextReg);
    masm.passABIArg(argUintNReg);
    masm.passABIArg(argVpReg);
    JSNative native = target->native();
    if (call->ignoresReturnValue() && target->hasJitInfo()) {
        const JSJitInfo* jitInfo = target->jitInfo();
        if (jitInfo->type() == JSJitInfo::IgnoresReturnValueNative)
            native = jitInfo->ignoresReturnValueMethod;
    }
    masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, native), MoveOp::GENERAL,
                     CheckUnsafeCallWithABI::DontCheckHasExitFrame);

    emitTracelogStopEvent(TraceLogger_Call);

    // Test for failure.
    masm.branchIfFalseBool(ReturnReg, masm.failureLabel());

    // Load the outparam vp[0] into output register(s).
    masm.loadValue(Address(masm.getStackPointer(), NativeExitFrameLayout::offsetOfResult()), JSReturnOperand);

    // Until C++ code is instrumented against Spectre, prevent speculative
    // execution from returning any private data.
    if (JitOptions.spectreJitToCxxCalls && !call->mir()->ignoresReturnValue() &&
        call->mir()->hasLiveDefUses())
    {
        masm.speculationBarrier();
    }

    // The next instruction is removing the footer of the exit frame, so there
    // is no need for leaveFakeExitFrame.

    // Move the StackPointer back to its original location, unwinding the native exit frame.
    masm.adjustStack(NativeExitFrameLayout::Size() - unusedStack);
    MOZ_ASSERT(masm.framePushed() == initialStack);
}

static void
LoadDOMPrivate(MacroAssembler& masm, Register obj, Register priv, DOMObjectKind kind)
{
    // Load the value in DOM_OBJECT_SLOT for a native or proxy DOM object. This
    // will be in the first slot but may be fixed or non-fixed.
    MOZ_ASSERT(obj != priv);

    // Check if it's a proxy.
    Label isProxy, done;
    if (kind == DOMObjectKind::Unknown)
        masm.branchTestObjectIsProxy(true, obj, priv, &isProxy);

    if (kind != DOMObjectKind::Proxy) {
        // If it's a native object, the value must be in a fixed slot.
        masm.debugAssertObjHasFixedSlots(obj, priv);
        masm.loadPrivate(Address(obj, NativeObject::getFixedSlotOffset(0)), priv);
        if (kind == DOMObjectKind::Unknown)
            masm.jump(&done);
    }

    if (kind != DOMObjectKind::Native) {
        masm.bind(&isProxy);
#ifdef DEBUG
        // Sanity check: it must be a DOM proxy.
        Label isDOMProxy;
        masm.branchTestProxyHandlerFamily(Assembler::Equal, obj, priv,
                                          GetDOMProxyHandlerFamily(), &isDOMProxy);
        masm.assumeUnreachable("Expected a DOM proxy");
        masm.bind(&isDOMProxy);
#endif
        masm.loadPtr(Address(obj, ProxyObject::offsetOfReservedSlots()), priv);
        masm.loadPrivate(Address(priv, detail::ProxyReservedSlots::offsetOfSlot(0)), priv);
    }

    masm.bind(&done);
}

void
CodeGenerator::visitCallDOMNative(LCallDOMNative* call)
{
    WrappedFunction* target = call->getSingleTarget();
    MOZ_ASSERT(target);
    MOZ_ASSERT(target->isNative());
    MOZ_ASSERT(target->hasJitInfo());
    MOZ_ASSERT(call->mir()->isCallDOMNative());

    int callargslot = call->argslot();
    int unusedStack = StackOffsetOfPassedArg(callargslot);

    // Registers used for callWithABI() argument-passing.
    const Register argJSContext = ToRegister(call->getArgJSContext());
    const Register argObj       = ToRegister(call->getArgObj());
    const Register argPrivate   = ToRegister(call->getArgPrivate());
    const Register argArgs      = ToRegister(call->getArgArgs());

    DebugOnly<uint32_t> initialStack = masm.framePushed();

    masm.checkStackAlignment();

    // DOM methods have the signature:
    //  bool (*)(JSContext*, HandleObject, void* private, const JSJitMethodCallArgs& args)
    // Where args is initialized from an argc and a vp, vp[0] is space for an
    // outparam and the callee, vp[1] is |this|, and vp[2] onward are the
    // function arguments.  Note that args stores the argv, not the vp, and
    // argv == vp + 2.

    // Nestle the stack up against the pushed arguments, leaving StackPointer at
    // &vp[1]
    masm.adjustStack(unusedStack);
    // argObj is filled with the extracted object, then returned.
    Register obj = masm.extractObject(Address(masm.getStackPointer(), 0), argObj);
    MOZ_ASSERT(obj == argObj);

    // Push a Value containing the callee object: natives are allowed to access their callee before
    // setitng the return value. After this the StackPointer points to &vp[0].
    masm.Push(ObjectValue(*target->rawJSFunction()));

    // Now compute the argv value.  Since StackPointer is pointing to &vp[0] and
    // argv is &vp[2] we just need to add 2*sizeof(Value) to the current
    // StackPointer.
    JS_STATIC_ASSERT(JSJitMethodCallArgsTraits::offsetOfArgv == 0);
    JS_STATIC_ASSERT(JSJitMethodCallArgsTraits::offsetOfArgc ==
                     IonDOMMethodExitFrameLayoutTraits::offsetOfArgcFromArgv);
    masm.computeEffectiveAddress(Address(masm.getStackPointer(), 2 * sizeof(Value)), argArgs);

    LoadDOMPrivate(masm, obj, argPrivate, static_cast<MCallDOMNative*>(call->mir())->objectKind());

    // Push argc from the call instruction into what will become the IonExitFrame
    masm.Push(Imm32(call->numActualArgs()));

    // Push our argv onto the stack
    masm.Push(argArgs);
    // And store our JSJitMethodCallArgs* in argArgs.
    masm.moveStackPtrTo(argArgs);

    // Push |this| object for passing HandleObject. We push after argc to
    // maintain the same sp-relative location of the object pointer with other
    // DOMExitFrames.
    masm.Push(argObj);
    masm.moveStackPtrTo(argObj);

    // Construct native exit frame.
    uint32_t safepointOffset = masm.buildFakeExitFrame(argJSContext);
    masm.loadJSContext(argJSContext);
    masm.enterFakeExitFrame(argJSContext, argJSContext, ExitFrameType::IonDOMMethod);

    markSafepointAt(safepointOffset, call);

    // Construct and execute call.
    masm.setupUnalignedABICall(argJSContext);
    masm.loadJSContext(argJSContext);
    masm.passABIArg(argJSContext);
    masm.passABIArg(argObj);
    masm.passABIArg(argPrivate);
    masm.passABIArg(argArgs);
    masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, target->jitInfo()->method), MoveOp::GENERAL,
                     CheckUnsafeCallWithABI::DontCheckHasExitFrame);

    if (target->jitInfo()->isInfallible) {
        masm.loadValue(Address(masm.getStackPointer(), IonDOMMethodExitFrameLayout::offsetOfResult()),
                       JSReturnOperand);
    } else {
        // Test for failure.
        masm.branchIfFalseBool(ReturnReg, masm.exceptionLabel());

        // Load the outparam vp[0] into output register(s).
        masm.loadValue(Address(masm.getStackPointer(), IonDOMMethodExitFrameLayout::offsetOfResult()),
                       JSReturnOperand);
    }

    // Until C++ code is instrumented against Spectre, prevent speculative
    // execution from returning any private data.
    if (JitOptions.spectreJitToCxxCalls && call->mir()->hasLiveDefUses())
        masm.speculationBarrier();

    // The next instruction is removing the footer of the exit frame, so there
    // is no need for leaveFakeExitFrame.

    // Move the StackPointer back to its original location, unwinding the native exit frame.
    masm.adjustStack(IonDOMMethodExitFrameLayout::Size() - unusedStack);
    MOZ_ASSERT(masm.framePushed() == initialStack);
}

typedef bool (*GetIntrinsicValueFn)(JSContext* cx, HandlePropertyName, MutableHandleValue);
static const VMFunction GetIntrinsicValueInfo =
    FunctionInfo<GetIntrinsicValueFn>(GetIntrinsicValue, "GetIntrinsicValue");

void
CodeGenerator::visitCallGetIntrinsicValue(LCallGetIntrinsicValue* lir)
{
    pushArg(ImmGCPtr(lir->mir()->name()));
    callVM(GetIntrinsicValueInfo, lir);
}

typedef bool (*InvokeFunctionFn)(JSContext*, HandleObject, bool, bool, uint32_t, Value*,
                                 MutableHandleValue);
static const VMFunction InvokeFunctionInfo =
    FunctionInfo<InvokeFunctionFn>(InvokeFunction, "InvokeFunction");

void
CodeGenerator::emitCallInvokeFunction(LInstruction* call, Register calleereg,
                                      bool constructing, bool ignoresReturnValue,
                                      uint32_t argc, uint32_t unusedStack)
{
    // Nestle %esp up to the argument vector.
    // Each path must account for framePushed_ separately, for callVM to be valid.
    masm.freeStack(unusedStack);

    pushArg(masm.getStackPointer()); // argv.
    pushArg(Imm32(argc));            // argc.
    pushArg(Imm32(ignoresReturnValue));
    pushArg(Imm32(constructing));    // constructing.
    pushArg(calleereg);              // JSFunction*.

    callVM(InvokeFunctionInfo, call);

    // Un-nestle %esp from the argument vector. No prefix was pushed.
    masm.reserveStack(unusedStack);
}

void
CodeGenerator::visitCallGeneric(LCallGeneric* call)
{
    Register calleereg = ToRegister(call->getFunction());
    Register objreg    = ToRegister(call->getTempObject());
    Register nargsreg  = ToRegister(call->getNargsReg());
    uint32_t unusedStack = StackOffsetOfPassedArg(call->argslot());
    Label invoke, thunk, makeCall, end;

    // Known-target case is handled by LCallKnown.
    MOZ_ASSERT(!call->hasSingleTarget());

    masm.checkStackAlignment();

    // Guard that calleereg is actually a function object.
    if (call->mir()->needsClassCheck()) {
        masm.branchTestObjClass(Assembler::NotEqual, calleereg, &JSFunction::class_, nargsreg,
                                calleereg, &invoke);
    }

    // Guard that calleereg is an interpreted function with a JSScript or a
    // wasm function.
    // If we are constructing, also ensure the callee is a constructor.
    if (call->mir()->isConstructing()) {
        masm.branchIfNotInterpretedConstructor(calleereg, nargsreg, &invoke);
    } else {
        masm.branchIfFunctionHasNoJitEntry(calleereg, /* isConstructing */ false, &invoke);
        masm.branchFunctionKind(Assembler::Equal, JSFunction::ClassConstructor, calleereg, objreg,
                                &invoke);
    }

    if (call->mir()->needsArgCheck())
        masm.loadJitCodeRaw(calleereg, objreg);
    else
        masm.loadJitCodeNoArgCheck(calleereg, objreg);

    // Nestle the StackPointer up to the argument vector.
    masm.freeStack(unusedStack);

    // Construct the IonFramePrefix.
    uint32_t descriptor = MakeFrameDescriptor(masm.framePushed(), JitFrame_IonJS,
                                              JitFrameLayout::Size());
    masm.Push(Imm32(call->numActualArgs()));
    masm.PushCalleeToken(calleereg, call->mir()->isConstructing());
    masm.Push(Imm32(descriptor));

    // Check whether the provided arguments satisfy target argc.
    // We cannot have lowered to LCallGeneric with a known target. Assert that we didn't
    // add any undefineds in IonBuilder. NB: MCall::numStackArgs includes |this|.
    DebugOnly<unsigned> numNonArgsOnStack = 1 + call->isConstructing();
    MOZ_ASSERT(call->numActualArgs() == call->mir()->numStackArgs() - numNonArgsOnStack);
    masm.load16ZeroExtend(Address(calleereg, JSFunction::offsetOfNargs()), nargsreg);
    masm.branch32(Assembler::Above, nargsreg, Imm32(call->numActualArgs()), &thunk);
    masm.jump(&makeCall);

    // Argument fixup needed. Load the ArgumentsRectifier.
    masm.bind(&thunk);
    {
        TrampolinePtr argumentsRectifier = gen->jitRuntime()->getArgumentsRectifier();
        masm.movePtr(argumentsRectifier, objreg);
    }

    // Finally call the function in objreg.
    masm.bind(&makeCall);
    uint32_t callOffset = masm.callJit(objreg);
    markSafepointAt(callOffset, call);

    // Increment to remove IonFramePrefix; decrement to fill FrameSizeClass.
    // The return address has already been removed from the Ion frame.
    int prefixGarbage = sizeof(JitFrameLayout) - sizeof(void*);
    masm.adjustStack(prefixGarbage - unusedStack);
    masm.jump(&end);

    // Handle uncompiled or native functions.
    masm.bind(&invoke);
    emitCallInvokeFunction(call, calleereg, call->isConstructing(), call->ignoresReturnValue(),
                           call->numActualArgs(), unusedStack);

    masm.bind(&end);

    // If the return value of the constructing function is Primitive,
    // replace the return value with the Object from CreateThis.
    if (call->mir()->isConstructing()) {
        Label notPrimitive;
        masm.branchTestPrimitive(Assembler::NotEqual, JSReturnOperand, &notPrimitive);
        masm.loadValue(Address(masm.getStackPointer(), unusedStack), JSReturnOperand);
        masm.bind(&notPrimitive);
    }
}

typedef bool (*InvokeFunctionShuffleFn)(JSContext*, HandleObject, uint32_t, uint32_t, Value*,
                                        MutableHandleValue);
static const VMFunction InvokeFunctionShuffleInfo =
    FunctionInfo<InvokeFunctionShuffleFn>(InvokeFunctionShuffleNewTarget,
                                          "InvokeFunctionShuffleNewTarget");
void
CodeGenerator::emitCallInvokeFunctionShuffleNewTarget(LCallKnown* call, Register calleeReg,
                                                      uint32_t numFormals, uint32_t unusedStack)
{
    masm.freeStack(unusedStack);

    pushArg(masm.getStackPointer());
    pushArg(Imm32(numFormals));
    pushArg(Imm32(call->numActualArgs()));
    pushArg(calleeReg);

    callVM(InvokeFunctionShuffleInfo, call);

    masm.reserveStack(unusedStack);
}

void
CodeGenerator::visitCallKnown(LCallKnown* call)
{
    Register calleereg = ToRegister(call->getFunction());
    Register objreg    = ToRegister(call->getTempObject());
    uint32_t unusedStack = StackOffsetOfPassedArg(call->argslot());
    WrappedFunction* target = call->getSingleTarget();

    // Native single targets (except wasm) are handled by LCallNative.
    MOZ_ASSERT(!target->isNativeWithCppEntry());
    // Missing arguments must have been explicitly appended by the IonBuilder.
    DebugOnly<unsigned> numNonArgsOnStack = 1 + call->isConstructing();
    MOZ_ASSERT(target->nargs() <= call->mir()->numStackArgs() - numNonArgsOnStack);

    MOZ_ASSERT_IF(call->isConstructing(), target->isConstructor());

    masm.checkStackAlignment();

    if (target->isClassConstructor() && !call->isConstructing()) {
        emitCallInvokeFunction(call, calleereg, call->isConstructing(), call->ignoresReturnValue(),
                               call->numActualArgs(), unusedStack);
        return;
    }

    MOZ_ASSERT_IF(target->isClassConstructor(), call->isConstructing());

    Label uncompiled;
    if (!target->isNativeWithJitEntry()) {
        // The calleereg is known to be a non-native function, but might point
        // to a LazyScript instead of a JSScript.
        masm.branchIfFunctionHasNoJitEntry(calleereg, call->isConstructing(), &uncompiled);
    }

    if (call->mir()->needsArgCheck())
        masm.loadJitCodeRaw(calleereg, objreg);
    else
        masm.loadJitCodeNoArgCheck(calleereg, objreg);

    // Nestle the StackPointer up to the argument vector.
    masm.freeStack(unusedStack);

    // Construct the IonFramePrefix.
    uint32_t descriptor = MakeFrameDescriptor(masm.framePushed(), JitFrame_IonJS,
                                              JitFrameLayout::Size());
    masm.Push(Imm32(call->numActualArgs()));
    masm.PushCalleeToken(calleereg, call->mir()->isConstructing());
    masm.Push(Imm32(descriptor));

    // Finally call the function in objreg.
    uint32_t callOffset = masm.callJit(objreg);
    markSafepointAt(callOffset, call);

    // Increment to remove IonFramePrefix; decrement to fill FrameSizeClass.
    // The return address has already been removed from the Ion frame.
    int prefixGarbage = sizeof(JitFrameLayout) - sizeof(void*);
    masm.adjustStack(prefixGarbage - unusedStack);

    if (uncompiled.used()) {
        Label end;
        masm.jump(&end);

        // Handle uncompiled functions.
        masm.bind(&uncompiled);
        if (call->isConstructing() && target->nargs() > call->numActualArgs()) {
            emitCallInvokeFunctionShuffleNewTarget(call, calleereg, target->nargs(), unusedStack);
        } else {
            emitCallInvokeFunction(call, calleereg, call->isConstructing(),
                                   call->ignoresReturnValue(), call->numActualArgs(), unusedStack);
        }

        masm.bind(&end);
    }

    // If the return value of the constructing function is Primitive,
    // replace the return value with the Object from CreateThis.
    if (call->mir()->isConstructing()) {
        Label notPrimitive;
        masm.branchTestPrimitive(Assembler::NotEqual, JSReturnOperand, &notPrimitive);
        masm.loadValue(Address(masm.getStackPointer(), unusedStack), JSReturnOperand);
        masm.bind(&notPrimitive);
    }
}

template<typename T>
void
CodeGenerator::emitCallInvokeFunction(T* apply, Register extraStackSize)
{
    Register objreg = ToRegister(apply->getTempObject());
    MOZ_ASSERT(objreg != extraStackSize);

    // Push the space used by the arguments.
    masm.moveStackPtrTo(objreg);
    masm.Push(extraStackSize);

    pushArg(objreg);                           // argv.
    pushArg(ToRegister(apply->getArgc()));     // argc.
    pushArg(Imm32(false));                     // ignoresReturnValue.
    pushArg(Imm32(false));                     // isConstrucing.
    pushArg(ToRegister(apply->getFunction())); // JSFunction*.

    // This specialization og callVM restore the extraStackSize after the call.
    callVM(InvokeFunctionInfo, apply, &extraStackSize);

    masm.Pop(extraStackSize);
}

// Do not bailout after the execution of this function since the stack no longer
// correspond to what is expected by the snapshots.
void
CodeGenerator::emitAllocateSpaceForApply(Register argcreg, Register extraStackSpace, Label* end)
{
    // Initialize the loop counter AND Compute the stack usage (if == 0)
    masm.movePtr(argcreg, extraStackSpace);

    // Align the JitFrameLayout on the JitStackAlignment.
    if (JitStackValueAlignment > 1) {
        MOZ_ASSERT(frameSize() % JitStackAlignment == 0,
            "Stack padding assumes that the frameSize is correct");
        MOZ_ASSERT(JitStackValueAlignment == 2);
        Label noPaddingNeeded;
        // if the number of arguments is odd, then we do not need any padding.
        masm.branchTestPtr(Assembler::NonZero, argcreg, Imm32(1), &noPaddingNeeded);
        masm.addPtr(Imm32(1), extraStackSpace);
        masm.bind(&noPaddingNeeded);
    }

    // Reserve space for copying the arguments.
    NativeObject::elementsSizeMustNotOverflow();
    masm.lshiftPtr(Imm32(ValueShift), extraStackSpace);
    masm.subFromStackPtr(extraStackSpace);

#ifdef DEBUG
    // Put a magic value in the space reserved for padding. Note, this code
    // cannot be merged with the previous test, as not all architectures can
    // write below their stack pointers.
    if (JitStackValueAlignment > 1) {
        MOZ_ASSERT(JitStackValueAlignment == 2);
        Label noPaddingNeeded;
        // if the number of arguments is odd, then we do not need any padding.
        masm.branchTestPtr(Assembler::NonZero, argcreg, Imm32(1), &noPaddingNeeded);
        BaseValueIndex dstPtr(masm.getStackPointer(), argcreg);
        masm.storeValue(MagicValue(JS_ARG_POISON), dstPtr);
        masm.bind(&noPaddingNeeded);
    }
#endif

    // Skip the copy of arguments if there are none.
    masm.branchTestPtr(Assembler::Zero, argcreg, argcreg, end);
}

// Destroys argvIndex and copyreg.
void
CodeGenerator::emitCopyValuesForApply(Register argvSrcBase, Register argvIndex, Register copyreg,
                                      size_t argvSrcOffset, size_t argvDstOffset)
{
    Label loop;
    masm.bind(&loop);

    // As argvIndex is off by 1, and we use the decBranchPtr instruction
    // to loop back, we have to substract the size of the word which are
    // copied.
    BaseValueIndex srcPtr(argvSrcBase, argvIndex, argvSrcOffset - sizeof(void*));
    BaseValueIndex dstPtr(masm.getStackPointer(), argvIndex, argvDstOffset - sizeof(void*));
    masm.loadPtr(srcPtr, copyreg);
    masm.storePtr(copyreg, dstPtr);

    // Handle 32 bits architectures.
    if (sizeof(Value) == 2 * sizeof(void*)) {
        BaseValueIndex srcPtrLow(argvSrcBase, argvIndex, argvSrcOffset - 2 * sizeof(void*));
        BaseValueIndex dstPtrLow(masm.getStackPointer(), argvIndex, argvDstOffset - 2 * sizeof(void*));
        masm.loadPtr(srcPtrLow, copyreg);
        masm.storePtr(copyreg, dstPtrLow);
    }

    masm.decBranchPtr(Assembler::NonZero, argvIndex, Imm32(1), &loop);
}

void
CodeGenerator::emitPopArguments(Register extraStackSpace)
{
    // Pop |this| and Arguments.
    masm.freeStack(extraStackSpace);
}

void
CodeGenerator::emitPushArguments(LApplyArgsGeneric* apply, Register extraStackSpace)
{
    // Holds the function nargs. Initially the number of args to the caller.
    Register argcreg = ToRegister(apply->getArgc());
    Register copyreg = ToRegister(apply->getTempObject());

    Label end;
    emitAllocateSpaceForApply(argcreg, extraStackSpace, &end);

    // We are making a copy of the arguments which are above the JitFrameLayout
    // of the current Ion frame.
    //
    // [arg1] [arg0] <- src [this] [JitFrameLayout] [.. frameSize ..] [pad] [arg1] [arg0] <- dst

    // Compute the source and destination offsets into the stack.
    size_t argvSrcOffset = frameSize() + JitFrameLayout::offsetOfActualArgs();
    size_t argvDstOffset = 0;

    // Save the extra stack space, and re-use the register as a base.
    masm.push(extraStackSpace);
    Register argvSrcBase = extraStackSpace;
    argvSrcOffset += sizeof(void*);
    argvDstOffset += sizeof(void*);

    // Save the actual number of register, and re-use the register as an index register.
    masm.push(argcreg);
    Register argvIndex = argcreg;
    argvSrcOffset += sizeof(void*);
    argvDstOffset += sizeof(void*);

    // srcPtr = (StackPointer + extraStackSpace) + argvSrcOffset
    // dstPtr = (StackPointer                  ) + argvDstOffset
    masm.addStackPtrTo(argvSrcBase);

    // Copy arguments.
    emitCopyValuesForApply(argvSrcBase, argvIndex, copyreg, argvSrcOffset, argvDstOffset);

    // Restore argcreg and the extra stack space counter.
    masm.pop(argcreg);
    masm.pop(extraStackSpace);

    // Join with all arguments copied and the extra stack usage computed.
    masm.bind(&end);

    // Push |this|.
    masm.addPtr(Imm32(sizeof(Value)), extraStackSpace);
    masm.pushValue(ToValue(apply, LApplyArgsGeneric::ThisIndex));
}

void
CodeGenerator::emitPushArguments(LApplyArrayGeneric* apply, Register extraStackSpace)
{
    Label noCopy, epilogue;
    Register tmpArgc = ToRegister(apply->getTempObject());
    Register elementsAndArgc = ToRegister(apply->getElements());

    // Invariants guarded in the caller:
    //  - the array is not too long
    //  - the array length equals its initialized length

    // The array length is our argc for the purposes of allocating space.
    Address length(ToRegister(apply->getElements()), ObjectElements::offsetOfLength());
    masm.load32(length, tmpArgc);

    // Allocate space for the values.
    emitAllocateSpaceForApply(tmpArgc, extraStackSpace, &noCopy);

    // Copy the values.  This code is skipped entirely if there are
    // no values.
    size_t argvDstOffset = 0;

    Register argvSrcBase = elementsAndArgc; // Elements value

    masm.push(extraStackSpace);
    Register copyreg = extraStackSpace;
    argvDstOffset += sizeof(void*);

    masm.push(tmpArgc);
    Register argvIndex = tmpArgc;
    argvDstOffset += sizeof(void*);

    // Copy
    emitCopyValuesForApply(argvSrcBase, argvIndex, copyreg, 0, argvDstOffset);

    // Restore.
    masm.pop(elementsAndArgc);
    masm.pop(extraStackSpace);
    masm.jump(&epilogue);

    // Clear argc if we skipped the copy step.
    masm.bind(&noCopy);
    masm.movePtr(ImmPtr(0), elementsAndArgc);

    // Join with all arguments copied and the extra stack usage computed.
    // Note, "elements" has become "argc".
    masm.bind(&epilogue);

    // Push |this|.
    masm.addPtr(Imm32(sizeof(Value)), extraStackSpace);
    masm.pushValue(ToValue(apply, LApplyArgsGeneric::ThisIndex));
}

template<typename T>
void
CodeGenerator::emitApplyGeneric(T* apply)
{
    // Holds the function object.
    Register calleereg = ToRegister(apply->getFunction());

    // Temporary register for modifying the function object.
    Register objreg = ToRegister(apply->getTempObject());
    Register extraStackSpace = ToRegister(apply->getTempStackCounter());

    // Holds the function nargs, computed in the invoker or (for
    // ApplyArray) in the argument pusher.
    Register argcreg = ToRegister(apply->getArgc());

    // Unless already known, guard that calleereg is actually a function object.
    if (!apply->hasSingleTarget()) {
        Label bail;
        masm.branchTestObjClass(Assembler::NotEqual, calleereg, &JSFunction::class_, objreg,
                                calleereg, &bail);
        bailoutFrom(&bail, apply->snapshot());
    }

    // Copy the arguments of the current function.
    //
    // In the case of ApplyArray, also compute argc: the argc register
    // and the elements register are the same; argc must not be
    // referenced before the call to emitPushArguments() and elements
    // must not be referenced after it returns.
    //
    // objreg is dead across this call.
    //
    // extraStackSpace is garbage on entry and defined on exit.
    emitPushArguments(apply, extraStackSpace);

    masm.checkStackAlignment();

    // If the function is native, only emit the call to InvokeFunction.
    if (apply->hasSingleTarget() && apply->getSingleTarget()->isNativeWithCppEntry()) {
        emitCallInvokeFunction(apply, extraStackSpace);
        emitPopArguments(extraStackSpace);
        return;
    }

    Label end, invoke;

    // Guard that calleereg is an interpreted function with a JSScript.
    masm.branchIfFunctionHasNoJitEntry(calleereg, /* constructing */ false, &invoke);

    // Guard that calleereg is not a class constrcuctor
    masm.branchFunctionKind(Assembler::Equal, JSFunction::ClassConstructor,
                            calleereg, objreg, &invoke);

    // Knowing that calleereg is a non-native function, load jitcode.
    masm.loadJitCodeRaw(calleereg, objreg);

    // Call with an Ion frame or a rectifier frame.
    {
        // Create the frame descriptor.
        unsigned pushed = masm.framePushed();
        Register stackSpace = extraStackSpace;
        masm.addPtr(Imm32(pushed), stackSpace);
        masm.makeFrameDescriptor(stackSpace, JitFrame_IonJS, JitFrameLayout::Size());

        masm.Push(argcreg);
        masm.Push(calleereg);
        masm.Push(stackSpace); // descriptor

        Label underflow, rejoin;

        // Check whether the provided arguments satisfy target argc.
        if (!apply->hasSingleTarget()) {
            Register nformals = extraStackSpace;
            masm.load16ZeroExtend(Address(calleereg, JSFunction::offsetOfNargs()), nformals);
            masm.branch32(Assembler::Below, argcreg, nformals, &underflow);
        } else {
            masm.branch32(Assembler::Below, argcreg, Imm32(apply->getSingleTarget()->nargs()),
                          &underflow);
        }

        // Skip the construction of the rectifier frame because we have no
        // underflow.
        masm.jump(&rejoin);

        // Argument fixup needed. Get ready to call the argumentsRectifier.
        {
            masm.bind(&underflow);

            // Hardcode the address of the argumentsRectifier code.
            TrampolinePtr argumentsRectifier = gen->jitRuntime()->getArgumentsRectifier();
            masm.movePtr(argumentsRectifier, objreg);
        }

        masm.bind(&rejoin);

        // Finally call the function in objreg, as assigned by one of the paths above.
        uint32_t callOffset = masm.callJit(objreg);
        markSafepointAt(callOffset, apply);

        // Recover the number of arguments from the frame descriptor.
        masm.loadPtr(Address(masm.getStackPointer(), 0), stackSpace);
        masm.rshiftPtr(Imm32(FRAMESIZE_SHIFT), stackSpace);
        masm.subPtr(Imm32(pushed), stackSpace);

        // Increment to remove IonFramePrefix; decrement to fill FrameSizeClass.
        // The return address has already been removed from the Ion frame.
        int prefixGarbage = sizeof(JitFrameLayout) - sizeof(void*);
        masm.adjustStack(prefixGarbage);
        masm.jump(&end);
    }

    // Handle uncompiled or native functions.
    {
        masm.bind(&invoke);
        emitCallInvokeFunction(apply, extraStackSpace);
    }

    // Pop arguments and continue.
    masm.bind(&end);
    emitPopArguments(extraStackSpace);
}

void
CodeGenerator::visitApplyArgsGeneric(LApplyArgsGeneric* apply)
{
    // Limit the number of parameters we can handle to a number that does not risk
    // us allocating too much stack, notably on Windows where there is a 4K guard page
    // that has to be touched to extend the stack.  See bug 1351278.  The value "3000"
    // is the size of the guard page minus an arbitrary, but large, safety margin.

    LSnapshot* snapshot = apply->snapshot();
    Register argcreg = ToRegister(apply->getArgc());

    uint32_t limit = 3000 / sizeof(Value);
    bailoutCmp32(Assembler::Above, argcreg, Imm32(limit), snapshot);

    emitApplyGeneric(apply);
}

void
CodeGenerator::visitApplyArrayGeneric(LApplyArrayGeneric* apply)
{
    LSnapshot* snapshot = apply->snapshot();
    Register tmp = ToRegister(apply->getTempObject());

    Address length(ToRegister(apply->getElements()), ObjectElements::offsetOfLength());
    masm.load32(length, tmp);

    // See comment in visitApplyArgsGeneric, above.

    uint32_t limit = 3000 / sizeof(Value);
    bailoutCmp32(Assembler::Above, tmp, Imm32(limit), snapshot);

    // Ensure that the array does not contain an uninitialized tail.

    Address initializedLength(ToRegister(apply->getElements()),
                              ObjectElements::offsetOfInitializedLength());
    masm.sub32(initializedLength, tmp);
    bailoutCmp32(Assembler::NotEqual, tmp, Imm32(0), snapshot);

    emitApplyGeneric(apply);
}

void
CodeGenerator::visitBail(LBail* lir)
{
    bailout(lir->snapshot());
}

void
CodeGenerator::visitUnreachable(LUnreachable* lir)
{
    masm.assumeUnreachable("end-of-block assumed unreachable");
}

void
CodeGenerator::visitEncodeSnapshot(LEncodeSnapshot* lir)
{
    encode(lir->snapshot());
}

void
CodeGenerator::visitGetDynamicName(LGetDynamicName* lir)
{
    Register envChain = ToRegister(lir->getEnvironmentChain());
    Register name = ToRegister(lir->getName());
    Register temp1 = ToRegister(lir->temp1());
    Register temp2 = ToRegister(lir->temp2());
    Register temp3 = ToRegister(lir->temp3());

    masm.loadJSContext(temp3);

    /* Make space for the outparam. */
    masm.adjustStack(-int32_t(sizeof(Value)));
    masm.moveStackPtrTo(temp2);

    masm.setupUnalignedABICall(temp1);
    masm.passABIArg(temp3);
    masm.passABIArg(envChain);
    masm.passABIArg(name);
    masm.passABIArg(temp2);
    masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, GetDynamicName));

    const ValueOperand out = ToOutValue(lir);

    masm.loadValue(Address(masm.getStackPointer(), 0), out);
    masm.adjustStack(sizeof(Value));

    Label undefined;
    masm.branchTestUndefined(Assembler::Equal, out, &undefined);
    bailoutFrom(&undefined, lir->snapshot());
}

typedef bool (*DirectEvalSFn)(JSContext*, HandleObject, HandleScript, HandleValue,
                              HandleString, jsbytecode*, MutableHandleValue);
static const VMFunction DirectEvalStringInfo =
    FunctionInfo<DirectEvalSFn>(DirectEvalStringFromIon, "DirectEvalStringFromIon");

void
CodeGenerator::visitCallDirectEval(LCallDirectEval* lir)
{
    Register envChain = ToRegister(lir->getEnvironmentChain());
    Register string = ToRegister(lir->getString());

    pushArg(ImmPtr(lir->mir()->pc()));
    pushArg(string);
    pushArg(ToValue(lir, LCallDirectEval::NewTarget));
    pushArg(ImmGCPtr(current->mir()->info().script()));
    pushArg(envChain);

    callVM(DirectEvalStringInfo, lir);
}

void
CodeGenerator::generateArgumentsChecks(bool assert)
{
    // This function can be used the normal way to check the argument types,
    // before entering the function and bailout when arguments don't match.
    // For debug purpose, this is can also be used to force/check that the
    // arguments are correct. Upon fail it will hit a breakpoint.

    MIRGraph& mir = gen->graph();
    MResumePoint* rp = mir.entryResumePoint();

    // No registers are allocated yet, so it's safe to grab anything.
    AllocatableGeneralRegisterSet temps(GeneralRegisterSet::All());
    Register temp1 = temps.takeAny();
    Register temp2 = temps.takeAny();

    const CompileInfo& info = gen->info();

    Label miss;
    for (uint32_t i = info.startArgSlot(); i < info.endArgSlot(); i++) {
        // All initial parameters are guaranteed to be MParameters.
        MParameter* param = rp->getOperand(i)->toParameter();
        const TypeSet* types = param->resultTypeSet();
        if (!types || types->unknown())
            continue;

#ifndef JS_CODEGEN_ARM64
        // Calculate the offset on the stack of the argument.
        // (i - info.startArgSlot())    - Compute index of arg within arg vector.
        // ... * sizeof(Value)          - Scale by value size.
        // ArgToStackOffset(...)        - Compute displacement within arg vector.
        int32_t offset = ArgToStackOffset((i - info.startArgSlot()) * sizeof(Value));
        Address argAddr(masm.getStackPointer(), offset);

        // guardObjectType will zero the stack pointer register on speculative
        // paths.
        Register spectreRegToZero = masm.getStackPointer();
        masm.guardTypeSet(argAddr, types, BarrierKind::TypeSet, temp1, temp2,
                          spectreRegToZero, &miss);
#else
        // On ARM64, the stack pointer situation is more complicated. When we
        // enable Ion, we should figure out how to mitigate Spectre there.
        mozilla::Unused << temp1;
        mozilla::Unused << temp2;
        MOZ_CRASH("NYI");
#endif
    }

    if (miss.used()) {
        if (assert) {
#ifdef DEBUG
            Label success;
            masm.jump(&success);
            masm.bind(&miss);

            // Check for cases where the type set guard might have missed due to
            // changing object groups.
            for (uint32_t i = info.startArgSlot(); i < info.endArgSlot(); i++) {
                MParameter* param = rp->getOperand(i)->toParameter();
                const TemporaryTypeSet* types = param->resultTypeSet();
                if (!types || types->unknown())
                    continue;

                Label skip;
                Address addr(masm.getStackPointer(), ArgToStackOffset((i - info.startArgSlot()) * sizeof(Value)));
                masm.branchTestObject(Assembler::NotEqual, addr, &skip);
                Register obj = masm.extractObject(addr, temp1);
                masm.guardTypeSetMightBeIncomplete(types, obj, temp1, &success);
                masm.bind(&skip);
            }

            masm.assumeUnreachable("Argument check fail.");
            masm.bind(&success);
#else
            MOZ_CRASH("Shouldn't get here in opt builds");
#endif
        } else {
            bailoutFrom(&miss, graph.entrySnapshot());
        }
    }
}

// Out-of-line path to report over-recursed error and fail.
class CheckOverRecursedFailure : public OutOfLineCodeBase<CodeGenerator>
{
    LInstruction* lir_;

  public:
    explicit CheckOverRecursedFailure(LInstruction* lir)
      : lir_(lir)
    { }

    void accept(CodeGenerator* codegen) override {
        codegen->visitCheckOverRecursedFailure(this);
    }

    LInstruction* lir() const {
        return lir_;
    }
};

void
CodeGenerator::visitCheckOverRecursed(LCheckOverRecursed* lir)
{
    // If we don't push anything on the stack, skip the check.
    if (omitOverRecursedCheck())
        return;

    // Ensure that this frame will not cross the stack limit.
    // This is a weak check, justified by Ion using the C stack: we must always
    // be some distance away from the actual limit, since if the limit is
    // crossed, an error must be thrown, which requires more frames.
    //
    // It must always be possible to trespass past the stack limit.
    // Ion may legally place frames very close to the limit. Calling additional
    // C functions may then violate the limit without any checking.
    //
    // Since Ion frames exist on the C stack, the stack limit may be
    // dynamically set by JS_SetThreadStackLimit() and JS_SetNativeStackQuota().

    CheckOverRecursedFailure* ool = new(alloc()) CheckOverRecursedFailure(lir);
    addOutOfLineCode(ool, lir->mir());

    Register temp = ToRegister(lir->temp());

    // Conditional forward (unlikely) branch to failure.
    const void* contextAddr = gen->compartment->zone()->addressOfJSContext();
    masm.loadPtr(AbsoluteAddress(contextAddr), temp);
    masm.branchStackPtrRhs(Assembler::AboveOrEqual,
                           Address(temp, offsetof(JSContext, jitStackLimit)), ool->entry());
    masm.bind(ool->rejoin());
}

typedef bool (*DefVarFn)(JSContext*, HandlePropertyName, unsigned, HandleObject);
static const VMFunction DefVarInfo = FunctionInfo<DefVarFn>(DefVar, "DefVar");

void
CodeGenerator::visitDefVar(LDefVar* lir)
{
    Register envChain = ToRegister(lir->environmentChain());

    pushArg(envChain); // JSObject*
    pushArg(Imm32(lir->mir()->attrs())); // unsigned
    pushArg(ImmGCPtr(lir->mir()->name())); // PropertyName*

    callVM(DefVarInfo, lir);
}

typedef bool (*DefLexicalFn)(JSContext*, HandlePropertyName, unsigned);
static const VMFunction DefLexicalInfo =
    FunctionInfo<DefLexicalFn>(DefGlobalLexical, "DefGlobalLexical");

void
CodeGenerator::visitDefLexical(LDefLexical* lir)
{
    pushArg(Imm32(lir->mir()->attrs())); // unsigned
    pushArg(ImmGCPtr(lir->mir()->name())); // PropertyName*

    callVM(DefLexicalInfo, lir);
}

typedef bool (*DefFunOperationFn)(JSContext*, HandleScript, HandleObject, HandleFunction);
static const VMFunction DefFunOperationInfo =
    FunctionInfo<DefFunOperationFn>(DefFunOperation, "DefFunOperation");

void
CodeGenerator::visitDefFun(LDefFun* lir)
{
    Register envChain = ToRegister(lir->environmentChain());

    Register fun = ToRegister(lir->fun());
    pushArg(fun);
    pushArg(envChain);
    pushArg(ImmGCPtr(current->mir()->info().script()));

    callVM(DefFunOperationInfo, lir);
}

typedef bool (*CheckOverRecursedFn)(JSContext*);
static const VMFunction CheckOverRecursedInfo =
    FunctionInfo<CheckOverRecursedFn>(CheckOverRecursed, "CheckOverRecursed");

void
CodeGenerator::visitCheckOverRecursedFailure(CheckOverRecursedFailure* ool)
{
    // The OOL path is hit if the recursion depth has been exceeded.
    // Throw an InternalError for over-recursion.

    // LFunctionEnvironment can appear before LCheckOverRecursed, so we have
    // to save all live registers to avoid crashes if CheckOverRecursed triggers
    // a GC.
    saveLive(ool->lir());

    callVM(CheckOverRecursedInfo, ool->lir());

    restoreLive(ool->lir());
    masm.jump(ool->rejoin());
}

IonScriptCounts*
CodeGenerator::maybeCreateScriptCounts()
{
    // If scripts are being profiled, create a new IonScriptCounts for the
    // profiling data, which will be attached to the associated JSScript or
    // wasm module after code generation finishes.
    if (!gen->hasProfilingScripts())
        return nullptr;

    // This test inhibits IonScriptCount creation for wasm code which is
    // currently incompatible with wasm codegen for two reasons: (1) wasm code
    // must be serializable and script count codegen bakes in absolute
    // addresses, (2) wasm code does not have a JSScript with which to associate
    // code coverage data.
    JSScript* script = gen->info().script();
    if (!script)
        return nullptr;

    UniquePtr<IonScriptCounts> counts(js_new<IonScriptCounts>());
    if (!counts || !counts->init(graph.numBlocks()))
        return nullptr;

    for (size_t i = 0; i < graph.numBlocks(); i++) {
        MBasicBlock* block = graph.getBlock(i)->mir();

        uint32_t offset = 0;
        char* description = nullptr;
        if (MResumePoint* resume = block->entryResumePoint()) {
            // Find a PC offset in the outermost script to use. If this
            // block is from an inlined script, find a location in the
            // outer script to associate information about the inlining
            // with.
            while (resume->caller())
                resume = resume->caller();
            offset = script->pcToOffset(resume->pc());

            if (block->entryResumePoint()->caller()) {
                // Get the filename and line number of the inner script.
                JSScript* innerScript = block->info().script();
                description = (char*) js_calloc(200);
                if (description) {
                    snprintf(description, 200, "%s:%zu",
                             innerScript->filename(), innerScript->lineno());
                }
            }
        }

        if (!counts->block(i).init(block->id(), offset, description, block->numSuccessors()))
            return nullptr;

        for (size_t j = 0; j < block->numSuccessors(); j++)
            counts->block(i).setSuccessor(j, skipTrivialBlocks(block->getSuccessor(j))->id());
    }

    scriptCounts_ = counts.release();
    return scriptCounts_;
}

// Structure for managing the state tracked for a block by script counters.
struct ScriptCountBlockState
{
    IonBlockCounts& block;
    MacroAssembler& masm;

    Sprinter printer;

  public:
    ScriptCountBlockState(IonBlockCounts* block, MacroAssembler* masm)
      : block(*block), masm(*masm), printer(GetJitContext()->cx, false)
    {
    }

    bool init()
    {
        if (!printer.init())
            return false;

        // Bump the hit count for the block at the start. This code is not
        // included in either the text for the block or the instruction byte
        // counts.
        masm.inc64(AbsoluteAddress(block.addressOfHitCount()));

        // Collect human readable assembly for the code generated in the block.
        masm.setPrinter(&printer);

        return true;
    }

    void visitInstruction(LInstruction* ins)
    {
        // Prefix stream of assembly instructions with their LIR instruction
        // name and any associated high level info.
        if (const char* extra = ins->getExtraName())
            printer.printf("[%s:%s]\n", ins->opName(), extra);
        else
            printer.printf("[%s]\n", ins->opName());
    }

    ~ScriptCountBlockState()
    {
        masm.setPrinter(nullptr);

        if (!printer.hadOutOfMemory())
            block.setCode(printer.string());
    }
};

void
CodeGenerator::branchIfInvalidated(Register temp, Label* invalidated)
{
    CodeOffset label = masm.movWithPatch(ImmWord(uintptr_t(-1)), temp);
    masm.propagateOOM(ionScriptLabels_.append(label));

    // If IonScript::invalidationCount_ != 0, the script has been invalidated.
    masm.branch32(Assembler::NotEqual,
                  Address(temp, IonScript::offsetOfInvalidationCount()),
                  Imm32(0),
                  invalidated);
}

#ifdef DEBUG
void
CodeGenerator::emitAssertObjectOrStringResult(Register input, MIRType type, const TemporaryTypeSet* typeset)
{
    MOZ_ASSERT(type == MIRType::Object || type == MIRType::ObjectOrNull ||
               type == MIRType::String || type == MIRType::Symbol);

    AllocatableGeneralRegisterSet regs(GeneralRegisterSet::All());
    regs.take(input);

    Register temp = regs.takeAny();
    masm.push(temp);

    // Don't check if the script has been invalidated. In that case invalid
    // types are expected (until we reach the OsiPoint and bailout).
    Label done;
    branchIfInvalidated(temp, &done);

    if ((type == MIRType::Object || type == MIRType::ObjectOrNull) &&
        typeset && !typeset->unknownObject())
    {
        // We have a result TypeSet, assert this object is in it.
        Label miss, ok;
        if (type == MIRType::ObjectOrNull)
            masm.branchPtr(Assembler::Equal, input, ImmWord(0), &ok);
        if (typeset->getObjectCount() > 0)
            masm.guardObjectType(input, typeset, temp, input, &miss);
        else
            masm.jump(&miss);
        masm.jump(&ok);

        masm.bind(&miss);
        masm.guardTypeSetMightBeIncomplete(typeset, input, temp, &ok);

        masm.assumeUnreachable("MIR instruction returned object with unexpected type");

        masm.bind(&ok);
    }

    // Check that we have a valid GC pointer.
    if (JitOptions.fullDebugChecks) {
        saveVolatile();
        masm.setupUnalignedABICall(temp);
        masm.loadJSContext(temp);
        masm.passABIArg(temp);
        masm.passABIArg(input);

        void* callee;
        switch (type) {
          case MIRType::Object:
            callee = JS_FUNC_TO_DATA_PTR(void*, AssertValidObjectPtr);
            break;
          case MIRType::ObjectOrNull:
            callee = JS_FUNC_TO_DATA_PTR(void*, AssertValidObjectOrNullPtr);
            break;
          case MIRType::String:
            callee = JS_FUNC_TO_DATA_PTR(void*, AssertValidStringPtr);
            break;
          case MIRType::Symbol:
            callee = JS_FUNC_TO_DATA_PTR(void*, AssertValidSymbolPtr);
            break;
          default:
            MOZ_CRASH();
        }

        masm.callWithABI(callee);
        restoreVolatile();
    }

    masm.bind(&done);
    masm.pop(temp);
}

void
CodeGenerator::emitAssertResultV(const ValueOperand input, const TemporaryTypeSet* typeset)
{
    AllocatableGeneralRegisterSet regs(GeneralRegisterSet::All());
    regs.take(input);

    Register temp1 = regs.takeAny();
    Register temp2 = regs.takeAny();
    masm.push(temp1);
    masm.push(temp2);

    // Don't check if the script has been invalidated. In that case invalid
    // types are expected (until we reach the OsiPoint and bailout).
    Label done;
    branchIfInvalidated(temp1, &done);

    if (typeset && !typeset->unknown()) {
        // We have a result TypeSet, assert this value is in it.
        Label miss, ok;
        masm.guardTypeSet(input, typeset, BarrierKind::TypeSet, temp1, temp2,
                          input.payloadOrValueReg(), &miss);
        masm.jump(&ok);

        masm.bind(&miss);

        // Check for cases where the type set guard might have missed due to
        // changing object groups.
        Label realMiss;
        masm.branchTestObject(Assembler::NotEqual, input, &realMiss);
        Register payload = masm.extractObject(input, temp1);
        masm.guardTypeSetMightBeIncomplete(typeset, payload, temp1, &ok);
        masm.bind(&realMiss);

        masm.assumeUnreachable("MIR instruction returned value with unexpected type");

        masm.bind(&ok);
    }

    // Check that we have a valid GC pointer.
    if (JitOptions.fullDebugChecks) {
        saveVolatile();

        masm.pushValue(input);
        masm.moveStackPtrTo(temp1);

        masm.setupUnalignedABICall(temp2);
        masm.loadJSContext(temp2);
        masm.passABIArg(temp2);
        masm.passABIArg(temp1);
        masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, AssertValidValue));
        masm.popValue(input);
        restoreVolatile();
    }

    masm.bind(&done);
    masm.pop(temp2);
    masm.pop(temp1);
}

void
CodeGenerator::emitObjectOrStringResultChecks(LInstruction* lir, MDefinition* mir)
{
    if (lir->numDefs() == 0)
        return;

    MOZ_ASSERT(lir->numDefs() == 1);
    if (lir->getDef(0)->isBogusTemp())
        return;

    Register output = ToRegister(lir->getDef(0));
    emitAssertObjectOrStringResult(output, mir->type(), mir->resultTypeSet());
}

void
CodeGenerator::emitValueResultChecks(LInstruction* lir, MDefinition* mir)
{
    if (lir->numDefs() == 0)
        return;

    MOZ_ASSERT(lir->numDefs() == BOX_PIECES);
    if (!lir->getDef(0)->output()->isRegister())
        return;

    ValueOperand output = ToOutValue(lir);

    emitAssertResultV(output, mir->resultTypeSet());
}

void
CodeGenerator::emitDebugResultChecks(LInstruction* ins)
{
    // In debug builds, check that LIR instructions return valid values.

    MDefinition* mir = ins->mirRaw();
    if (!mir)
        return;

    switch (mir->type()) {
      case MIRType::Object:
      case MIRType::ObjectOrNull:
      case MIRType::String:
      case MIRType::Symbol:
        emitObjectOrStringResultChecks(ins, mir);
        break;
      case MIRType::Value:
        emitValueResultChecks(ins, mir);
        break;
      default:
        break;
    }
}

void
CodeGenerator::emitDebugForceBailing(LInstruction* lir)
{
    if (!lir->snapshot())
        return;
    if (lir->isStart())
        return;
    if (lir->isOsiPoint())
        return;

    masm.comment("emitDebugForceBailing");
    const void* bailAfterAddr = gen->compartment->zone()->addressOfIonBailAfter();

    AllocatableGeneralRegisterSet regs(GeneralRegisterSet::All());

    Label done, notBail, bail;
    masm.branch32(Assembler::Equal, AbsoluteAddress(bailAfterAddr), Imm32(0), &done);
    {
        Register temp = regs.takeAny();

        masm.push(temp);
        masm.load32(AbsoluteAddress(bailAfterAddr), temp);
        masm.sub32(Imm32(1), temp);
        masm.store32(temp, AbsoluteAddress(bailAfterAddr));

        masm.branch32(Assembler::NotEqual, temp, Imm32(0), &notBail);
        {
            masm.pop(temp);
            masm.jump(&bail);
            bailoutFrom(&bail, lir->snapshot());
        }
        masm.bind(&notBail);
        masm.pop(temp);
    }
    masm.bind(&done);
}
#endif

static void
DumpTrackedSite(const BytecodeSite* site)
{
    if (!JitSpewEnabled(JitSpew_OptimizationTracking))
        return;

#ifdef JS_JITSPEW
    unsigned column = 0;
    unsigned lineNumber = PCToLineNumber(site->script(), site->pc(), &column);
    JitSpew(JitSpew_OptimizationTracking, "Types for %s at %s:%u:%u",
            CodeName[JSOp(*site->pc())],
            site->script()->filename(),
            lineNumber,
            column);
#endif
}

static void
DumpTrackedOptimizations(TrackedOptimizations* optimizations)
{
    if (!JitSpewEnabled(JitSpew_OptimizationTracking))
        return;

    optimizations->spew(JitSpew_OptimizationTracking);
}

bool
CodeGenerator::generateBody()
{
    IonScriptCounts* counts = maybeCreateScriptCounts();

#if defined(JS_ION_PERF)
    PerfSpewer* perfSpewer = &perfSpewer_;
    if (gen->compilingWasm())
        perfSpewer = &gen->perfSpewer();
#endif

    for (size_t i = 0; i < graph.numBlocks(); i++) {
        current = graph.getBlock(i);

        // Don't emit any code for trivial blocks, containing just a goto. Such
        // blocks are created to split critical edges, and if we didn't end up
        // putting any instructions in them, we can skip them.
        if (current->isTrivial())
            continue;

#ifdef JS_JITSPEW
        const char* filename = nullptr;
        size_t lineNumber = 0;
        unsigned columnNumber = 0;
        if (current->mir()->info().script()) {
            filename = current->mir()->info().script()->filename();
            if (current->mir()->pc())
                lineNumber = PCToLineNumber(current->mir()->info().script(), current->mir()->pc(),
                                            &columnNumber);
        } else {
#ifdef DEBUG
            lineNumber = current->mir()->lineno();
            columnNumber = current->mir()->columnIndex();
#endif
        }
        JitSpew(JitSpew_Codegen, "# block%zu %s:%zu:%u%s:",
                i, filename ? filename : "?", lineNumber, columnNumber,
                current->mir()->isLoopHeader() ? " (loop header)" : "");
#endif

        masm.bind(current->label());

        mozilla::Maybe<ScriptCountBlockState> blockCounts;
        if (counts) {
            blockCounts.emplace(&counts->block(i), &masm);
            if (!blockCounts->init())
                return false;
        }
        TrackedOptimizations* last = nullptr;

#if defined(JS_ION_PERF)
        if (!perfSpewer->startBasicBlock(current->mir(), masm))
            return false;
#endif

        for (LInstructionIterator iter = current->begin(); iter != current->end(); iter++) {
            if (!alloc().ensureBallast())
                return false;

#ifdef JS_JITSPEW
            JitSpewStart(JitSpew_Codegen, "instruction %s", iter->opName());
            if (const char* extra = iter->getExtraName())
                JitSpewCont(JitSpew_Codegen, ":%s", extra);
            JitSpewFin(JitSpew_Codegen);
#endif

            if (counts)
                blockCounts->visitInstruction(*iter);

#ifdef CHECK_OSIPOINT_REGISTERS
            if (iter->safepoint())
                resetOsiPointRegs(iter->safepoint());
#endif

            if (iter->mirRaw()) {
                // Only add instructions that have a tracked inline script tree.
                if (iter->mirRaw()->trackedTree()) {
                    if (!addNativeToBytecodeEntry(iter->mirRaw()->trackedSite()))
                        return false;
                }

                // Track the start native offset of optimizations.
                if (iter->mirRaw()->trackedOptimizations()) {
                    if (last != iter->mirRaw()->trackedOptimizations()) {
                        DumpTrackedSite(iter->mirRaw()->trackedSite());
                        DumpTrackedOptimizations(iter->mirRaw()->trackedOptimizations());
                        last = iter->mirRaw()->trackedOptimizations();
                    }
                    if (!addTrackedOptimizationsEntry(iter->mirRaw()->trackedOptimizations()))
                        return false;
                }
            }

            setElement(*iter); // needed to encode correct snapshot location.

#ifdef DEBUG
            emitDebugForceBailing(*iter);
#endif

            switch (iter->op()) {
#define LIROP(op) case LNode::LOp_##op: visit##op(iter->to##op()); break;
    LIR_OPCODE_LIST(LIROP)
#undef LIROP
              case LNode::LOp_Invalid:
              default:
                MOZ_CRASH("Invalid LIR op");
            }

            // Track the end native offset of optimizations.
            if (iter->mirRaw() && iter->mirRaw()->trackedOptimizations())
                extendTrackedOptimizationsEntry(iter->mirRaw()->trackedOptimizations());

#ifdef DEBUG
            if (!counts)
                emitDebugResultChecks(*iter);
#endif
        }
        if (masm.oom())
            return false;

#if defined(JS_ION_PERF)
        perfSpewer->endBasicBlock(masm);
#endif
    }

    return true;
}

// Out-of-line object allocation for LNewArray.
class OutOfLineNewArray : public OutOfLineCodeBase<CodeGenerator>
{
    LNewArray* lir_;

  public:
    explicit OutOfLineNewArray(LNewArray* lir)
      : lir_(lir)
    { }

    void accept(CodeGenerator* codegen) override {
        codegen->visitOutOfLineNewArray(this);
    }

    LNewArray* lir() const {
        return lir_;
    }
};

typedef JSObject* (*NewArrayOperationFn)(JSContext*, HandleScript, jsbytecode*, uint32_t,
                                         NewObjectKind);
static const VMFunction NewArrayOperationInfo =
    FunctionInfo<NewArrayOperationFn>(NewArrayOperation, "NewArrayOperation");

static JSObject*
NewArrayWithGroup(JSContext* cx, uint32_t length, HandleObjectGroup group,
                  bool convertDoubleElements)
{
    ArrayObject* res = NewFullyAllocatedArrayTryUseGroup(cx, group, length);
    if (!res)
        return nullptr;
    if (convertDoubleElements)
        res->setShouldConvertDoubleElements();
    return res;
}

typedef JSObject* (*NewArrayWithGroupFn)(JSContext*, uint32_t, HandleObjectGroup, bool);
static const VMFunction NewArrayWithGroupInfo =
    FunctionInfo<NewArrayWithGroupFn>(NewArrayWithGroup, "NewArrayWithGroup");

void
CodeGenerator::visitNewArrayCallVM(LNewArray* lir)
{
    Register objReg = ToRegister(lir->output());

    MOZ_ASSERT(!lir->isCall());
    saveLive(lir);

    JSObject* templateObject = lir->mir()->templateObject();

    if (templateObject) {
        pushArg(Imm32(lir->mir()->convertDoubleElements()));
        pushArg(ImmGCPtr(templateObject->group()));
        pushArg(Imm32(lir->mir()->length()));

        callVM(NewArrayWithGroupInfo, lir);
    } else {
        pushArg(Imm32(GenericObject));
        pushArg(Imm32(lir->mir()->length()));
        pushArg(ImmPtr(lir->mir()->pc()));
        pushArg(ImmGCPtr(lir->mir()->block()->info().script()));

        callVM(NewArrayOperationInfo, lir);
    }

    if (ReturnReg != objReg)
        masm.movePtr(ReturnReg, objReg);

    restoreLive(lir);
}

typedef JSObject* (*NewDerivedTypedObjectFn)(JSContext*,
                                             HandleObject type,
                                             HandleObject owner,
                                             int32_t offset);
static const VMFunction CreateDerivedTypedObjInfo =
    FunctionInfo<NewDerivedTypedObjectFn>(CreateDerivedTypedObj, "CreateDerivedTypedObj");

void
CodeGenerator::visitNewDerivedTypedObject(LNewDerivedTypedObject* lir)
{
    pushArg(ToRegister(lir->offset()));
    pushArg(ToRegister(lir->owner()));
    pushArg(ToRegister(lir->type()));
    callVM(CreateDerivedTypedObjInfo, lir);
}

void
CodeGenerator::visitAtan2D(LAtan2D* lir)
{
    Register temp = ToRegister(lir->temp());
    FloatRegister y = ToFloatRegister(lir->y());
    FloatRegister x = ToFloatRegister(lir->x());

    masm.setupUnalignedABICall(temp);
    masm.passABIArg(y, MoveOp::DOUBLE);
    masm.passABIArg(x, MoveOp::DOUBLE);
    masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, ecmaAtan2), MoveOp::DOUBLE);

    MOZ_ASSERT(ToFloatRegister(lir->output()) == ReturnDoubleReg);
}

void
CodeGenerator::visitHypot(LHypot* lir)
{
    Register temp = ToRegister(lir->temp());
    uint32_t numArgs = lir->numArgs();
    masm.setupUnalignedABICall(temp);

    for (uint32_t i = 0 ; i < numArgs; ++i)
        masm.passABIArg(ToFloatRegister(lir->getOperand(i)), MoveOp::DOUBLE);

    switch(numArgs) {
      case 2:
        masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, ecmaHypot), MoveOp::DOUBLE);
        break;
      case 3:
        masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, hypot3), MoveOp::DOUBLE);
        break;
      case 4:
        masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, hypot4), MoveOp::DOUBLE);
        break;
      default:
        MOZ_CRASH("Unexpected number of arguments to hypot function.");
    }
    MOZ_ASSERT(ToFloatRegister(lir->output()) == ReturnDoubleReg);
}

void
CodeGenerator::visitNewArray(LNewArray* lir)
{
    Register objReg = ToRegister(lir->output());
    Register tempReg = ToRegister(lir->temp());
    JSObject* templateObject = lir->mir()->templateObject();
    DebugOnly<uint32_t> length = lir->mir()->length();

    MOZ_ASSERT(length <= NativeObject::MAX_DENSE_ELEMENTS_COUNT);

    if (lir->mir()->isVMCall()) {
        visitNewArrayCallVM(lir);
        return;
    }

    OutOfLineNewArray* ool = new(alloc()) OutOfLineNewArray(lir);
    addOutOfLineCode(ool, lir->mir());

    masm.createGCObject(objReg, tempReg, templateObject, lir->mir()->initialHeap(),
                        ool->entry(), /* initContents = */ true,
                        lir->mir()->convertDoubleElements());

    masm.bind(ool->rejoin());
}

void
CodeGenerator::visitOutOfLineNewArray(OutOfLineNewArray* ool)
{
    visitNewArrayCallVM(ool->lir());
    masm.jump(ool->rejoin());
}

void
CodeGenerator::visitNewArrayCopyOnWrite(LNewArrayCopyOnWrite* lir)
{
    Register objReg = ToRegister(lir->output());
    Register tempReg = ToRegister(lir->temp());
    ArrayObject* templateObject = lir->mir()->templateObject();
    gc::InitialHeap initialHeap = lir->mir()->initialHeap();

    // If we have a template object, we can inline call object creation.
    OutOfLineCode* ool = oolCallVM(NewArrayCopyOnWriteInfo, lir,
                                   ArgList(ImmGCPtr(templateObject), Imm32(initialHeap)),
                                   StoreRegisterTo(objReg));

    masm.createGCObject(objReg, tempReg, templateObject, initialHeap, ool->entry());

    masm.bind(ool->rejoin());
}

typedef ArrayObject* (*ArrayConstructorOneArgFn)(JSContext*, HandleObjectGroup, int32_t length);
static const VMFunction ArrayConstructorOneArgInfo =
    FunctionInfo<ArrayConstructorOneArgFn>(ArrayConstructorOneArg, "ArrayConstructorOneArg");

void
CodeGenerator::visitNewArrayDynamicLength(LNewArrayDynamicLength* lir)
{
    Register lengthReg = ToRegister(lir->length());
    Register objReg = ToRegister(lir->output());
    Register tempReg = ToRegister(lir->temp());

    JSObject* templateObject = lir->mir()->templateObject();
    gc::InitialHeap initialHeap = lir->mir()->initialHeap();

    OutOfLineCode* ool = oolCallVM(ArrayConstructorOneArgInfo, lir,
                                   ArgList(ImmGCPtr(templateObject->group()), lengthReg),
                                   StoreRegisterTo(objReg));

    bool canInline = true;
    size_t inlineLength = 0;
    if (templateObject->as<ArrayObject>().hasFixedElements()) {
        size_t numSlots = gc::GetGCKindSlots(templateObject->asTenured().getAllocKind());
        inlineLength = numSlots - ObjectElements::VALUES_PER_HEADER;
    } else {
        canInline = false;
    }

    if (canInline) {
        // Try to do the allocation inline if the template object is big enough
        // for the length in lengthReg. If the length is bigger we could still
        // use the template object and not allocate the elements, but it's more
        // efficient to do a single big allocation than (repeatedly) reallocating
        // the array later on when filling it.
        masm.branch32(Assembler::Above, lengthReg, Imm32(inlineLength), ool->entry());

        masm.createGCObject(objReg, tempReg, templateObject, initialHeap, ool->entry());

        size_t lengthOffset = NativeObject::offsetOfFixedElements() + ObjectElements::offsetOfLength();
        masm.store32(lengthReg, Address(objReg, lengthOffset));
    } else {
        masm.jump(ool->entry());
    }

    masm.bind(ool->rejoin());
}

typedef ArrayIteratorObject* (*NewArrayIteratorObjectFn)(JSContext*, NewObjectKind);
static const VMFunction NewArrayIteratorObjectInfo =
    FunctionInfo<NewArrayIteratorObjectFn>(NewArrayIteratorObject, "NewArrayIteratorObject");

typedef StringIteratorObject* (*NewStringIteratorObjectFn)(JSContext*, NewObjectKind);
static const VMFunction NewStringIteratorObjectInfo =
    FunctionInfo<NewStringIteratorObjectFn>(NewStringIteratorObject, "NewStringIteratorObject");

void
CodeGenerator::visitNewIterator(LNewIterator* lir)
{
    Register objReg = ToRegister(lir->output());
    Register tempReg = ToRegister(lir->temp());
    JSObject* templateObject = lir->mir()->templateObject();

    OutOfLineCode* ool;
    switch (lir->mir()->type()) {
      case MNewIterator::ArrayIterator:
        ool = oolCallVM(NewArrayIteratorObjectInfo, lir,
                        ArgList(Imm32(GenericObject)),
                        StoreRegisterTo(objReg));
        break;
      case MNewIterator::StringIterator:
        ool = oolCallVM(NewStringIteratorObjectInfo, lir,
                        ArgList(Imm32(GenericObject)),
                        StoreRegisterTo(objReg));
        break;
      default:
          MOZ_CRASH("unexpected iterator type");
    }

    masm.createGCObject(objReg, tempReg, templateObject, gc::DefaultHeap, ool->entry());

    masm.bind(ool->rejoin());
}

typedef TypedArrayObject* (*TypedArrayConstructorOneArgFn)(JSContext*, HandleObject, int32_t length);
static const VMFunction TypedArrayConstructorOneArgInfo =
    FunctionInfo<TypedArrayConstructorOneArgFn>(TypedArrayCreateWithTemplate,
                                                "TypedArrayCreateWithTemplate");

void
CodeGenerator::visitNewTypedArray(LNewTypedArray* lir)
{
    Register objReg = ToRegister(lir->output());
    Register tempReg = ToRegister(lir->temp1());
    Register lengthReg = ToRegister(lir->temp2());
    LiveRegisterSet liveRegs = lir->safepoint()->liveRegs();

    JSObject* templateObject = lir->mir()->templateObject();
    gc::InitialHeap initialHeap = lir->mir()->initialHeap();

    TypedArrayObject* ttemplate = &templateObject->as<TypedArrayObject>();
    uint32_t n = ttemplate->length();

    OutOfLineCode* ool = oolCallVM(TypedArrayConstructorOneArgInfo, lir,
                                   ArgList(ImmGCPtr(templateObject), Imm32(n)),
                                   StoreRegisterTo(objReg));

    masm.createGCObject(objReg, tempReg, templateObject, initialHeap,
                        ool->entry(), /*initContents*/true, /*convertDoubleElements*/false);

    masm.initTypedArraySlots(objReg, tempReg, lengthReg, liveRegs, ool->entry(),
                             ttemplate, TypedArrayLength::Fixed);

    masm.bind(ool->rejoin());
}

void
CodeGenerator::visitNewTypedArrayDynamicLength(LNewTypedArrayDynamicLength* lir)
{
    Register lengthReg = ToRegister(lir->length());
    Register objReg = ToRegister(lir->output());
    Register tempReg = ToRegister(lir->temp());
    LiveRegisterSet liveRegs = lir->safepoint()->liveRegs();

    JSObject* templateObject = lir->mir()->templateObject();
    gc::InitialHeap initialHeap = lir->mir()->initialHeap();

    TypedArrayObject* ttemplate = &templateObject->as<TypedArrayObject>();

    OutOfLineCode* ool = oolCallVM(TypedArrayConstructorOneArgInfo, lir,
                                   ArgList(ImmGCPtr(templateObject), lengthReg),
                                   StoreRegisterTo(objReg));

    masm.createGCObject(objReg, tempReg, templateObject, initialHeap,
                        ool->entry(), /*initContents*/true, /*convertDoubleElements*/false);

    masm.initTypedArraySlots(objReg, tempReg, lengthReg, liveRegs, ool->entry(),
                             ttemplate, TypedArrayLength::Dynamic);

    masm.bind(ool->rejoin());
}

// Out-of-line object allocation for JSOP_NEWOBJECT.
class OutOfLineNewObject : public OutOfLineCodeBase<CodeGenerator>
{
    LNewObject* lir_;

  public:
    explicit OutOfLineNewObject(LNewObject* lir)
      : lir_(lir)
    { }

    void accept(CodeGenerator* codegen) override {
        codegen->visitOutOfLineNewObject(this);
    }

    LNewObject* lir() const {
        return lir_;
    }
};

typedef JSObject* (*NewInitObjectWithTemplateFn)(JSContext*, HandleObject);
static const VMFunction NewInitObjectWithTemplateInfo =
    FunctionInfo<NewInitObjectWithTemplateFn>(NewObjectOperationWithTemplate,
                                              "NewObjectOperationWithTemplate");

typedef JSObject* (*NewInitObjectFn)(JSContext*, HandleScript, jsbytecode* pc, NewObjectKind);
static const VMFunction NewInitObjectInfo =
    FunctionInfo<NewInitObjectFn>(NewObjectOperation, "NewObjectOperation");

typedef PlainObject* (*ObjectCreateWithTemplateFn)(JSContext*, HandlePlainObject);
static const VMFunction ObjectCreateWithTemplateInfo =
    FunctionInfo<ObjectCreateWithTemplateFn>(ObjectCreateWithTemplate, "ObjectCreateWithTemplate");

void
CodeGenerator::visitNewObjectVMCall(LNewObject* lir)
{
    Register objReg = ToRegister(lir->output());

    MOZ_ASSERT(!lir->isCall());
    saveLive(lir);

    JSObject* templateObject = lir->mir()->templateObject();

    // If we're making a new object with a class prototype (that is, an object
    // that derives its class from its prototype instead of being
    // PlainObject::class_'d) from self-hosted code, we need a different init
    // function.
    switch (lir->mir()->mode()) {
      case MNewObject::ObjectLiteral:
        if (templateObject) {
            pushArg(ImmGCPtr(templateObject));
            callVM(NewInitObjectWithTemplateInfo, lir);
        } else {
            pushArg(Imm32(GenericObject));
            pushArg(ImmPtr(lir->mir()->resumePoint()->pc()));
            pushArg(ImmGCPtr(lir->mir()->block()->info().script()));
            callVM(NewInitObjectInfo, lir);
        }
        break;
      case MNewObject::ObjectCreate:
        pushArg(ImmGCPtr(templateObject));
        callVM(ObjectCreateWithTemplateInfo, lir);
        break;
    }

    if (ReturnReg != objReg)
        masm.movePtr(ReturnReg, objReg);

    restoreLive(lir);
}

static bool
ShouldInitFixedSlots(LInstruction* lir, JSObject* obj)
{
    if (!obj->isNative())
        return true;
    NativeObject* templateObj = &obj->as<NativeObject>();

    // Look for StoreFixedSlot instructions following an object allocation
    // that write to this object before a GC is triggered or this object is
    // passed to a VM call. If all fixed slots will be initialized, the
    // allocation code doesn't need to set the slots to |undefined|.

    uint32_t nfixed = templateObj->numUsedFixedSlots();
    if (nfixed == 0)
        return false;

    // Only optimize if all fixed slots are initially |undefined|, so that we
    // can assume incremental pre-barriers are not necessary. See also the
    // comment below.
    for (uint32_t slot = 0; slot < nfixed; slot++) {
        if (!templateObj->getSlot(slot).isUndefined())
            return true;
    }

    // Keep track of the fixed slots that are initialized. initializedSlots is
    // a bit mask with a bit for each slot.
    MOZ_ASSERT(nfixed <= NativeObject::MAX_FIXED_SLOTS);
    static_assert(NativeObject::MAX_FIXED_SLOTS <= 32, "Slot bits must fit in 32 bits");
    uint32_t initializedSlots = 0;
    uint32_t numInitialized = 0;

    MInstruction* allocMir = lir->mirRaw()->toInstruction();
    MBasicBlock* block = allocMir->block();

    // Skip the allocation instruction.
    MInstructionIterator iter = block->begin(allocMir);
    MOZ_ASSERT(*iter == allocMir);
    iter++;

    while (true) {
        for (; iter != block->end(); iter++) {
            if (iter->isNop() || iter->isConstant() || iter->isPostWriteBarrier()) {
                // These instructions won't trigger a GC or read object slots.
                continue;
            }

            if (iter->isStoreFixedSlot()) {
                MStoreFixedSlot* store = iter->toStoreFixedSlot();
                if (store->object() != allocMir)
                    return true;

                // We may not initialize this object slot on allocation, so the
                // pre-barrier could read uninitialized memory. Simply disable
                // the barrier for this store: the object was just initialized
                // so the barrier is not necessary.
                store->setNeedsBarrier(false);

                uint32_t slot = store->slot();
                MOZ_ASSERT(slot < nfixed);
                if ((initializedSlots & (1 << slot)) == 0) {
                    numInitialized++;
                    initializedSlots |= (1 << slot);

                    if (numInitialized == nfixed) {
                        // All fixed slots will be initialized.
                        MOZ_ASSERT(mozilla::CountPopulation32(initializedSlots) == nfixed);
                        return false;
                    }
                }
                continue;
            }

            if (iter->isGoto()) {
                block = iter->toGoto()->target();
                if (block->numPredecessors() != 1)
                    return true;
                break;
            }

            // Unhandled instruction, assume it bails or reads object slots.
            return true;
        }
        iter = block->begin();
    }

    MOZ_CRASH("Shouldn't get here");
}

void
CodeGenerator::visitNewObject(LNewObject* lir)
{
    Register objReg = ToRegister(lir->output());
    Register tempReg = ToRegister(lir->temp());
    JSObject* templateObject = lir->mir()->templateObject();

    if (lir->mir()->isVMCall()) {
        visitNewObjectVMCall(lir);
        return;
    }

    OutOfLineNewObject* ool = new(alloc()) OutOfLineNewObject(lir);
    addOutOfLineCode(ool, lir->mir());

    bool initContents = ShouldInitFixedSlots(lir, templateObject);
    masm.createGCObject(objReg, tempReg, templateObject, lir->mir()->initialHeap(), ool->entry(),
                        initContents);

    masm.bind(ool->rejoin());
}

void
CodeGenerator::visitOutOfLineNewObject(OutOfLineNewObject* ool)
{
    visitNewObjectVMCall(ool->lir());
    masm.jump(ool->rejoin());
}

typedef InlineTypedObject* (*NewTypedObjectFn)(JSContext*, Handle<InlineTypedObject*>, gc::InitialHeap);
static const VMFunction NewTypedObjectInfo =
    FunctionInfo<NewTypedObjectFn>(InlineTypedObject::createCopy, "InlineTypedObject::createCopy");

void
CodeGenerator::visitNewTypedObject(LNewTypedObject* lir)
{
    Register object = ToRegister(lir->output());
    Register temp = ToRegister(lir->temp());
    InlineTypedObject* templateObject = lir->mir()->templateObject();
    gc::InitialHeap initialHeap = lir->mir()->initialHeap();

    OutOfLineCode* ool = oolCallVM(NewTypedObjectInfo, lir,
                                   ArgList(ImmGCPtr(templateObject), Imm32(initialHeap)),
                                   StoreRegisterTo(object));

    masm.createGCObject(object, temp, templateObject, initialHeap, ool->entry());

    masm.bind(ool->rejoin());
}

void
CodeGenerator::visitSimdBox(LSimdBox* lir)
{
    FloatRegister in = ToFloatRegister(lir->input());
    Register object = ToRegister(lir->output());
    Register temp = ToRegister(lir->temp());
    InlineTypedObject* templateObject = lir->mir()->templateObject();
    gc::InitialHeap initialHeap = lir->mir()->initialHeap();
    MIRType type = lir->mir()->input()->type();

    addSimdTemplateToReadBarrier(lir->mir()->simdType());

    MOZ_ASSERT(lir->safepoint()->liveRegs().has(in), "Save the input register across oolCallVM");
    OutOfLineCode* ool = oolCallVM(NewTypedObjectInfo, lir,
                                   ArgList(ImmGCPtr(templateObject), Imm32(initialHeap)),
                                   StoreRegisterTo(object));

    masm.createGCObject(object, temp, templateObject, initialHeap, ool->entry());
    masm.bind(ool->rejoin());

    Address objectData(object, InlineTypedObject::offsetOfDataStart());
    switch (type) {
      case MIRType::Int8x16:
      case MIRType::Int16x8:
      case MIRType::Int32x4:
      case MIRType::Bool8x16:
      case MIRType::Bool16x8:
      case MIRType::Bool32x4:
        masm.storeUnalignedSimd128Int(in, objectData);
        break;
      case MIRType::Float32x4:
        masm.storeUnalignedSimd128Float(in, objectData);
        break;
      default:
        MOZ_CRASH("Unknown SIMD kind when generating code for SimdBox.");
    }
}

void
CodeGenerator::addSimdTemplateToReadBarrier(SimdType simdType)
{
    simdTemplatesToReadBarrier_ |= 1 << uint32_t(simdType);
}

void
CodeGenerator::visitSimdUnbox(LSimdUnbox* lir)
{
    Register object = ToRegister(lir->input());
    FloatRegister simd = ToFloatRegister(lir->output());
    Register temp = ToRegister(lir->temp());
    Label bail;

    masm.branchIfNotSimdObject(object, temp, lir->mir()->simdType(), &bail);

    // Load the value from the data of the InlineTypedObject.
    Address objectData(object, InlineTypedObject::offsetOfDataStart());
    switch (lir->mir()->type()) {
      case MIRType::Int8x16:
      case MIRType::Int16x8:
      case MIRType::Int32x4:
      case MIRType::Bool8x16:
      case MIRType::Bool16x8:
      case MIRType::Bool32x4:
        masm.loadUnalignedSimd128Int(objectData, simd);
        break;
      case MIRType::Float32x4:
        masm.loadUnalignedSimd128Float(objectData, simd);
        break;
      default:
        MOZ_CRASH("The impossible happened!");
    }

    bailoutFrom(&bail, lir->snapshot());
}

typedef js::NamedLambdaObject* (*NewNamedLambdaObjectFn)(JSContext*, HandleFunction, gc::InitialHeap);
static const VMFunction NewNamedLambdaObjectInfo =
    FunctionInfo<NewNamedLambdaObjectFn>(NamedLambdaObject::createTemplateObject,
                                         "NamedLambdaObject::createTemplateObject");

void
CodeGenerator::visitNewNamedLambdaObject(LNewNamedLambdaObject* lir)
{
    Register objReg = ToRegister(lir->output());
    Register tempReg = ToRegister(lir->temp());
    EnvironmentObject* templateObj = lir->mir()->templateObj();
    const CompileInfo& info = lir->mir()->block()->info();

    // If we have a template object, we can inline call object creation.
    OutOfLineCode* ool = oolCallVM(NewNamedLambdaObjectInfo, lir,
                                   ArgList(ImmGCPtr(info.funMaybeLazy()), Imm32(gc::DefaultHeap)),
                                   StoreRegisterTo(objReg));

    bool initContents = ShouldInitFixedSlots(lir, templateObj);
    masm.createGCObject(objReg, tempReg, templateObj, gc::DefaultHeap, ool->entry(),
                        initContents);

    masm.bind(ool->rejoin());
}

typedef JSObject* (*NewCallObjectFn)(JSContext*, HandleShape, HandleObjectGroup);
static const VMFunction NewCallObjectInfo =
    FunctionInfo<NewCallObjectFn>(NewCallObject, "NewCallObject");

void
CodeGenerator::visitNewCallObject(LNewCallObject* lir)
{
    Register objReg = ToRegister(lir->output());
    Register tempReg = ToRegister(lir->temp());

    CallObject* templateObj = lir->mir()->templateObject();

    OutOfLineCode* ool = oolCallVM(NewCallObjectInfo, lir,
                                   ArgList(ImmGCPtr(templateObj->lastProperty()),
                                           ImmGCPtr(templateObj->group())),
                                   StoreRegisterTo(objReg));

    // Inline call object creation, using the OOL path only for tricky cases.
    bool initContents = ShouldInitFixedSlots(lir, templateObj);
    masm.createGCObject(objReg, tempReg, templateObj, gc::DefaultHeap, ool->entry(),
                        initContents);

    masm.bind(ool->rejoin());
}

typedef JSObject* (*NewSingletonCallObjectFn)(JSContext*, HandleShape);
static const VMFunction NewSingletonCallObjectInfo =
    FunctionInfo<NewSingletonCallObjectFn>(NewSingletonCallObject, "NewSingletonCallObject");

void
CodeGenerator::visitNewSingletonCallObject(LNewSingletonCallObject* lir)
{
    Register objReg = ToRegister(lir->output());

    JSObject* templateObj = lir->mir()->templateObject();

    OutOfLineCode* ool;
    ool = oolCallVM(NewSingletonCallObjectInfo, lir,
                    ArgList(ImmGCPtr(templateObj->as<CallObject>().lastProperty())),
                    StoreRegisterTo(objReg));

    // Objects can only be given singleton types in VM calls.  We make the call
    // out of line to not bloat inline code, even if (naively) this seems like
    // extra work.
    masm.jump(ool->entry());
    masm.bind(ool->rejoin());
}

typedef JSObject* (*NewStringObjectFn)(JSContext*, HandleString);
static const VMFunction NewStringObjectInfo =
    FunctionInfo<NewStringObjectFn>(NewStringObject, "NewStringObject");

void
CodeGenerator::visitNewStringObject(LNewStringObject* lir)
{
    Register input = ToRegister(lir->input());
    Register output = ToRegister(lir->output());
    Register temp = ToRegister(lir->temp());

    StringObject* templateObj = lir->mir()->templateObj();

    OutOfLineCode* ool = oolCallVM(NewStringObjectInfo, lir, ArgList(input),
                                   StoreRegisterTo(output));

    masm.createGCObject(output, temp, templateObj, gc::DefaultHeap, ool->entry());

    masm.loadStringLength(input, temp);

    masm.storeValue(JSVAL_TYPE_STRING, input, Address(output, StringObject::offsetOfPrimitiveValue()));
    masm.storeValue(JSVAL_TYPE_INT32, temp, Address(output, StringObject::offsetOfLength()));

    masm.bind(ool->rejoin());
}

typedef bool(*InitElemFn)(JSContext* cx, jsbytecode* pc, HandleObject obj,
                          HandleValue id, HandleValue value);
static const VMFunction InitElemInfo =
    FunctionInfo<InitElemFn>(InitElemOperation, "InitElemOperation");

void
CodeGenerator::visitInitElem(LInitElem* lir)
{
    Register objReg = ToRegister(lir->getObject());

    pushArg(ToValue(lir, LInitElem::ValueIndex));
    pushArg(ToValue(lir, LInitElem::IdIndex));
    pushArg(objReg);
    pushArg(ImmPtr(lir->mir()->resumePoint()->pc()));

    callVM(InitElemInfo, lir);
}

typedef bool (*InitElemGetterSetterFn)(JSContext*, jsbytecode*, HandleObject, HandleValue,
                                       HandleObject);
static const VMFunction InitElemGetterSetterInfo =
    FunctionInfo<InitElemGetterSetterFn>(InitGetterSetterOperation, "InitElemGetterSetterOperation");

void
CodeGenerator::visitInitElemGetterSetter(LInitElemGetterSetter* lir)
{
    Register obj = ToRegister(lir->object());
    Register value = ToRegister(lir->value());

    pushArg(value);
    pushArg(ToValue(lir, LInitElemGetterSetter::IdIndex));
    pushArg(obj);
    pushArg(ImmPtr(lir->mir()->resumePoint()->pc()));

    callVM(InitElemGetterSetterInfo, lir);
}

typedef bool(*MutatePrototypeFn)(JSContext* cx, HandlePlainObject obj, HandleValue value);
static const VMFunction MutatePrototypeInfo =
    FunctionInfo<MutatePrototypeFn>(MutatePrototype, "MutatePrototype");

void
CodeGenerator::visitMutateProto(LMutateProto* lir)
{
    Register objReg = ToRegister(lir->getObject());

    pushArg(ToValue(lir, LMutateProto::ValueIndex));
    pushArg(objReg);

    callVM(MutatePrototypeInfo, lir);
}

typedef bool(*InitPropGetterSetterFn)(JSContext*, jsbytecode*, HandleObject, HandlePropertyName,
                                      HandleObject);
static const VMFunction InitPropGetterSetterInfo =
    FunctionInfo<InitPropGetterSetterFn>(InitGetterSetterOperation, "InitPropGetterSetterOperation");

void
CodeGenerator::visitInitPropGetterSetter(LInitPropGetterSetter* lir)
{
    Register obj = ToRegister(lir->object());
    Register value = ToRegister(lir->value());

    pushArg(value);
    pushArg(ImmGCPtr(lir->mir()->name()));
    pushArg(obj);
    pushArg(ImmPtr(lir->mir()->resumePoint()->pc()));

    callVM(InitPropGetterSetterInfo, lir);
}

typedef bool (*CreateThisFn)(JSContext* cx, HandleObject callee, HandleObject newTarget, MutableHandleValue rval);
static const VMFunction CreateThisInfoCodeGen = FunctionInfo<CreateThisFn>(CreateThis, "CreateThis");

void
CodeGenerator::visitCreateThis(LCreateThis* lir)
{
    const LAllocation* callee = lir->getCallee();
    const LAllocation* newTarget = lir->getNewTarget();

    if (newTarget->isConstant())
        pushArg(ImmGCPtr(&newTarget->toConstant()->toObject()));
    else
        pushArg(ToRegister(newTarget));

    if (callee->isConstant())
        pushArg(ImmGCPtr(&callee->toConstant()->toObject()));
    else
        pushArg(ToRegister(callee));

    callVM(CreateThisInfoCodeGen, lir);
}

static JSObject*
CreateThisForFunctionWithProtoWrapper(JSContext* cx, HandleObject callee, HandleObject newTarget,
                                      HandleObject proto)
{
    return CreateThisForFunctionWithProto(cx, callee, newTarget, proto);
}

typedef JSObject* (*CreateThisWithProtoFn)(JSContext* cx, HandleObject callee,
                                           HandleObject newTarget, HandleObject proto);
static const VMFunction CreateThisWithProtoInfo =
    FunctionInfo<CreateThisWithProtoFn>(CreateThisForFunctionWithProtoWrapper,
                                        "CreateThisForFunctionWithProtoWrapper");

void
CodeGenerator::visitCreateThisWithProto(LCreateThisWithProto* lir)
{
    const LAllocation* callee = lir->getCallee();
    const LAllocation* newTarget = lir->getNewTarget();
    const LAllocation* proto = lir->getPrototype();

    if (proto->isConstant())
        pushArg(ImmGCPtr(&proto->toConstant()->toObject()));
    else
        pushArg(ToRegister(proto));

    if (newTarget->isConstant())
        pushArg(ImmGCPtr(&newTarget->toConstant()->toObject()));
    else
        pushArg(ToRegister(newTarget));

    if (callee->isConstant())
        pushArg(ImmGCPtr(&callee->toConstant()->toObject()));
    else
        pushArg(ToRegister(callee));

    callVM(CreateThisWithProtoInfo, lir);
}

void
CodeGenerator::visitCreateThisWithTemplate(LCreateThisWithTemplate* lir)
{
    JSObject* templateObject = lir->mir()->templateObject();
    Register objReg = ToRegister(lir->output());
    Register tempReg = ToRegister(lir->temp());

    OutOfLineCode* ool = oolCallVM(NewInitObjectWithTemplateInfo, lir,
                                   ArgList(ImmGCPtr(templateObject)),
                                   StoreRegisterTo(objReg));

    // Allocate. If the FreeList is empty, call to VM, which may GC.
    bool initContents = !templateObject->is<PlainObject>() ||
                        ShouldInitFixedSlots(lir, &templateObject->as<PlainObject>());
    masm.createGCObject(objReg, tempReg, templateObject, lir->mir()->initialHeap(), ool->entry(),
                        initContents);

    masm.bind(ool->rejoin());
}

typedef JSObject* (*NewIonArgumentsObjectFn)(JSContext* cx, JitFrameLayout* frame, HandleObject);
static const VMFunction NewIonArgumentsObjectInfo =
    FunctionInfo<NewIonArgumentsObjectFn>((NewIonArgumentsObjectFn) ArgumentsObject::createForIon,
                                          "ArgumentsObject::createForIon");

void
CodeGenerator::visitCreateArgumentsObject(LCreateArgumentsObject* lir)
{
    // This should be getting constructed in the first block only, and not any OSR entry blocks.
    MOZ_ASSERT(lir->mir()->block()->id() == 0);

    Register callObj = ToRegister(lir->getCallObject());
    Register temp = ToRegister(lir->temp0());
    Label done;

    if (ArgumentsObject* templateObj = lir->mir()->templateObject()) {
        Register objTemp = ToRegister(lir->temp1());
        Register cxTemp = ToRegister(lir->temp2());

        masm.Push(callObj);

        // Try to allocate an arguments object. This will leave the reserved
        // slots uninitialized, so it's important we don't GC until we
        // initialize these slots in ArgumentsObject::finishForIon.
        Label failure;
        masm.createGCObject(objTemp, temp, templateObj, gc::DefaultHeap, &failure,
                            /* initContents = */ false);

        masm.moveStackPtrTo(temp);
        masm.addPtr(Imm32(masm.framePushed()), temp);

        masm.setupUnalignedABICall(cxTemp);
        masm.loadJSContext(cxTemp);
        masm.passABIArg(cxTemp);
        masm.passABIArg(temp);
        masm.passABIArg(callObj);
        masm.passABIArg(objTemp);

        masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, ArgumentsObject::finishForIon));
        masm.branchTestPtr(Assembler::Zero, ReturnReg, ReturnReg, &failure);

        // Discard saved callObj on the stack.
        masm.addToStackPtr(Imm32(sizeof(uintptr_t)));
        masm.jump(&done);

        masm.bind(&failure);
        masm.Pop(callObj);
    }

    masm.moveStackPtrTo(temp);
    masm.addPtr(Imm32(frameSize()), temp);

    pushArg(callObj);
    pushArg(temp);
    callVM(NewIonArgumentsObjectInfo, lir);

    masm.bind(&done);
}

void
CodeGenerator::visitGetArgumentsObjectArg(LGetArgumentsObjectArg* lir)
{
    Register temp = ToRegister(lir->getTemp(0));
    Register argsObj = ToRegister(lir->getArgsObject());
    ValueOperand out = ToOutValue(lir);

    masm.loadPrivate(Address(argsObj, ArgumentsObject::getDataSlotOffset()), temp);
    Address argAddr(temp, ArgumentsData::offsetOfArgs() + lir->mir()->argno() * sizeof(Value));
    masm.loadValue(argAddr, out);
#ifdef DEBUG
    Label success;
    masm.branchTestMagic(Assembler::NotEqual, out, &success);
    masm.assumeUnreachable("Result from ArgumentObject shouldn't be JSVAL_TYPE_MAGIC.");
    masm.bind(&success);
#endif
}

void
CodeGenerator::visitSetArgumentsObjectArg(LSetArgumentsObjectArg* lir)
{
    Register temp = ToRegister(lir->getTemp(0));
    Register argsObj = ToRegister(lir->getArgsObject());
    ValueOperand value = ToValue(lir, LSetArgumentsObjectArg::ValueIndex);

    masm.loadPrivate(Address(argsObj, ArgumentsObject::getDataSlotOffset()), temp);
    Address argAddr(temp, ArgumentsData::offsetOfArgs() + lir->mir()->argno() * sizeof(Value));
    emitPreBarrier(argAddr);
#ifdef DEBUG
    Label success;
    masm.branchTestMagic(Assembler::NotEqual, argAddr, &success);
    masm.assumeUnreachable("Result in ArgumentObject shouldn't be JSVAL_TYPE_MAGIC.");
    masm.bind(&success);
#endif
    masm.storeValue(value, argAddr);
}

void
CodeGenerator::visitReturnFromCtor(LReturnFromCtor* lir)
{
    ValueOperand value = ToValue(lir, LReturnFromCtor::ValueIndex);
    Register obj = ToRegister(lir->getObject());
    Register output = ToRegister(lir->output());

    Label valueIsObject, end;

    masm.branchTestObject(Assembler::Equal, value, &valueIsObject);

    // Value is not an object. Return that other object.
    masm.movePtr(obj, output);
    masm.jump(&end);

    // Value is an object. Return unbox(Value).
    masm.bind(&valueIsObject);
    Register payload = masm.extractObject(value, output);
    if (payload != output)
        masm.movePtr(payload, output);

    masm.bind(&end);
}

typedef bool (*BoxNonStrictThisFn)(JSContext*, HandleValue, MutableHandleValue);
static const VMFunction BoxNonStrictThisInfo =
    FunctionInfo<BoxNonStrictThisFn>(BoxNonStrictThis, "BoxNonStrictThis");

void
CodeGenerator::visitComputeThis(LComputeThis* lir)
{
    ValueOperand value = ToValue(lir, LComputeThis::ValueIndex);
    ValueOperand output = ToOutValue(lir);

    OutOfLineCode* ool = oolCallVM(BoxNonStrictThisInfo, lir, ArgList(value), StoreValueTo(output));

    masm.branchTestObject(Assembler::NotEqual, value, ool->entry());
    masm.moveValue(value, output);
    masm.bind(ool->rejoin());
}

void
CodeGenerator::visitImplicitThis(LImplicitThis* lir)
{
    pushArg(ImmGCPtr(lir->mir()->name()));
    pushArg(ToRegister(lir->env()));
    callVM(ImplicitThisInfo, lir);
}

void
CodeGenerator::visitArrowNewTarget(LArrowNewTarget* lir)
{
    Register callee = ToRegister(lir->callee());
    ValueOperand output = ToOutValue(lir);
    masm.loadValue(Address(callee, FunctionExtended::offsetOfArrowNewTargetSlot()), output);
}

void
CodeGenerator::visitArrayLength(LArrayLength* lir)
{
    Address length(ToRegister(lir->elements()), ObjectElements::offsetOfLength());
    masm.load32(length, ToRegister(lir->output()));
}

static void
SetLengthFromIndex(MacroAssembler& masm, const LAllocation* index, const Address& length)
{
    if (index->isConstant()) {
        masm.store32(Imm32(ToInt32(index) + 1), length);
    } else {
        Register newLength = ToRegister(index);
        masm.add32(Imm32(1), newLength);
        masm.store32(newLength, length);
        masm.sub32(Imm32(1), newLength);
    }
}

void
CodeGenerator::visitSetArrayLength(LSetArrayLength* lir)
{
    Address length(ToRegister(lir->elements()), ObjectElements::offsetOfLength());
    SetLengthFromIndex(masm, lir->index(), length);
}

template <class OrderedHashTable>
static void
RangeFront(MacroAssembler&, Register, Register, Register);

template <>
void
RangeFront<ValueMap>(MacroAssembler& masm, Register range, Register i, Register front)
{
    masm.loadPtr(Address(range, ValueMap::Range::offsetOfHashTable()), front);
    masm.loadPtr(Address(front, ValueMap::offsetOfImplData()), front);

    MOZ_ASSERT(ValueMap::offsetOfImplDataElement() == 0, "offsetof(Data, element) is 0");
    static_assert(ValueMap::sizeofImplData() == 24, "sizeof(Data) is 24");
    masm.mulBy3(i, i);
    masm.lshiftPtr(Imm32(3), i);
    masm.addPtr(i, front);
}

template <>
void
RangeFront<ValueSet>(MacroAssembler& masm, Register range, Register i, Register front)
{
    masm.loadPtr(Address(range, ValueSet::Range::offsetOfHashTable()), front);
    masm.loadPtr(Address(front, ValueSet::offsetOfImplData()), front);

    MOZ_ASSERT(ValueSet::offsetOfImplDataElement() == 0, "offsetof(Data, element) is 0");
    static_assert(ValueSet::sizeofImplData() == 16, "sizeof(Data) is 16");
    masm.lshiftPtr(Imm32(4), i);
    masm.addPtr(i, front);
}

template <class OrderedHashTable>
static void
RangePopFront(MacroAssembler& masm, Register range, Register front, Register dataLength,
              Register temp)
{
    Register i = temp;

    masm.add32(Imm32(1), Address(range, OrderedHashTable::Range::offsetOfCount()));

    masm.load32(Address(range, OrderedHashTable::Range::offsetOfI()), i);

    Label done, seek;
    masm.bind(&seek);
    masm.add32(Imm32(1), i);
    masm.branch32(Assembler::AboveOrEqual, i, dataLength, &done);

    // We can add sizeof(Data) to |front| to select the next element, because
    // |front| and |range.ht.data[i]| point to the same location.
    MOZ_ASSERT(OrderedHashTable::offsetOfImplDataElement() == 0, "offsetof(Data, element) is 0");
    masm.addPtr(Imm32(OrderedHashTable::sizeofImplData()), front);

    masm.branchTestMagic(Assembler::Equal, Address(front, OrderedHashTable::offsetOfEntryKey()),
                         JS_HASH_KEY_EMPTY, &seek);

    masm.bind(&done);
    masm.store32(i, Address(range, OrderedHashTable::Range::offsetOfI()));
}

template <class OrderedHashTable>
static inline void
RangeDestruct(MacroAssembler& masm, Register iter, Register range, Register temp0, Register temp1)
{
    Register next = temp0;
    Register prevp = temp1;

    masm.loadPtr(Address(range, OrderedHashTable::Range::offsetOfNext()), next);
    masm.loadPtr(Address(range, OrderedHashTable::Range::offsetOfPrevP()), prevp);
    masm.storePtr(next, Address(prevp, 0));

    Label hasNoNext;
    masm.branchTestPtr(Assembler::Zero, next, next, &hasNoNext);

    masm.storePtr(prevp, Address(next, OrderedHashTable::Range::offsetOfPrevP()));

    masm.bind(&hasNoNext);

    Label nurseryAllocated;
    masm.branchPtrInNurseryChunk(Assembler::Equal, iter, temp0, &nurseryAllocated);

    masm.callFreeStub(range);

    masm.bind(&nurseryAllocated);
}

template <>
void
CodeGenerator::emitLoadIteratorValues<ValueMap>(Register result, Register temp, Register front)
{
    size_t elementsOffset = NativeObject::offsetOfFixedElements();

    Address keyAddress(front, ValueMap::Entry::offsetOfKey());
    Address valueAddress(front, ValueMap::Entry::offsetOfValue());
    Address keyElemAddress(result, elementsOffset);
    Address valueElemAddress(result, elementsOffset + sizeof(Value));
    masm.guardedCallPreBarrier(keyElemAddress, MIRType::Value);
    masm.guardedCallPreBarrier(valueElemAddress, MIRType::Value);
    masm.storeValue(keyAddress, keyElemAddress, temp);
    masm.storeValue(valueAddress, valueElemAddress, temp);

    Label emitBarrier, skipBarrier;
    masm.branchValueIsNurseryCell(Assembler::Equal, keyAddress, temp, &emitBarrier);
    masm.branchValueIsNurseryCell(Assembler::NotEqual, valueAddress, temp, &skipBarrier);
    {
        masm.bind(&emitBarrier);
        saveVolatile(temp);
        emitPostWriteBarrier(result);
        restoreVolatile(temp);
    }
    masm.bind(&skipBarrier);
}

template <>
void
CodeGenerator::emitLoadIteratorValues<ValueSet>(Register result, Register temp, Register front)
{
    size_t elementsOffset = NativeObject::offsetOfFixedElements();

    Address keyAddress(front, ValueSet::offsetOfEntryKey());
    Address keyElemAddress(result, elementsOffset);
    masm.guardedCallPreBarrier(keyElemAddress, MIRType::Value);
    masm.storeValue(keyAddress, keyElemAddress, temp);

    Label skipBarrier;
    masm.branchValueIsNurseryCell(Assembler::NotEqual, keyAddress, temp, &skipBarrier);
    {
        saveVolatile(temp);
        emitPostWriteBarrier(result);
        restoreVolatile(temp);
    }
    masm.bind(&skipBarrier);
}

template <class IteratorObject, class OrderedHashTable>
void
CodeGenerator::emitGetNextEntryForIterator(LGetNextEntryForIterator* lir)
{
    Register iter = ToRegister(lir->iter());
    Register result = ToRegister(lir->result());
    Register temp = ToRegister(lir->temp0());
    Register dataLength = ToRegister(lir->temp1());
    Register range = ToRegister(lir->temp2());
    Register output = ToRegister(lir->output());

#ifdef DEBUG
    // Self-hosted code is responsible for ensuring GetNextEntryForIterator is
    // only called with the correct iterator class. Assert here all self-
    // hosted callers of GetNextEntryForIterator perform this class check.
    // No Spectre mitigations are needed because this is DEBUG-only code.
    Label success;
    masm.branchTestObjClassNoSpectreMitigations(Assembler::Equal, iter, &IteratorObject::class_,
                                                temp, &success);
    masm.assumeUnreachable("Iterator object should have the correct class.");
    masm.bind(&success);
#endif

    masm.loadPrivate(Address(iter, NativeObject::getFixedSlotOffset(IteratorObject::RangeSlot)),
                     range);

    Label iterAlreadyDone, iterDone, done;
    masm.branchTestPtr(Assembler::Zero, range, range, &iterAlreadyDone);

    masm.load32(Address(range, OrderedHashTable::Range::offsetOfI()), temp);
    masm.loadPtr(Address(range, OrderedHashTable::Range::offsetOfHashTable()), dataLength);
    masm.load32(Address(dataLength, OrderedHashTable::offsetOfImplDataLength()), dataLength);
    masm.branch32(Assembler::AboveOrEqual, temp, dataLength, &iterDone);
    {
        masm.push(iter);

        Register front = iter;
        RangeFront<OrderedHashTable>(masm, range, temp, front);

        emitLoadIteratorValues<OrderedHashTable>(result, temp, front);

        RangePopFront<OrderedHashTable>(masm, range, front, dataLength, temp);

        masm.pop(iter);
        masm.move32(Imm32(0), output);
    }
    masm.jump(&done);
    {
        masm.bind(&iterDone);

        RangeDestruct<OrderedHashTable>(masm, iter, range, temp, dataLength);

        masm.storeValue(PrivateValue(nullptr),
                        Address(iter, NativeObject::getFixedSlotOffset(IteratorObject::RangeSlot)));

        masm.bind(&iterAlreadyDone);

        masm.move32(Imm32(1), output);
    }
    masm.bind(&done);
}

void
CodeGenerator::visitGetNextEntryForIterator(LGetNextEntryForIterator* lir)
{
    if (lir->mir()->mode() == MGetNextEntryForIterator::Map) {
        emitGetNextEntryForIterator<MapIteratorObject, ValueMap>(lir);
    } else {
        MOZ_ASSERT(lir->mir()->mode() == MGetNextEntryForIterator::Set);
        emitGetNextEntryForIterator<SetIteratorObject, ValueSet>(lir);
    }
}

void
CodeGenerator::visitTypedArrayLength(LTypedArrayLength* lir)
{
    Register obj = ToRegister(lir->object());
    Register out = ToRegister(lir->output());
    masm.unboxInt32(Address(obj, TypedArrayObject::lengthOffset()), out);
}

void
CodeGenerator::visitTypedArrayElements(LTypedArrayElements* lir)
{
    Register obj = ToRegister(lir->object());
    Register out = ToRegister(lir->output());
    masm.loadPtr(Address(obj, TypedArrayObject::dataOffset()), out);
}

void
CodeGenerator::visitSetDisjointTypedElements(LSetDisjointTypedElements* lir)
{
    Register target = ToRegister(lir->target());
    Register targetOffset = ToRegister(lir->targetOffset());
    Register source = ToRegister(lir->source());

    Register temp = ToRegister(lir->temp());

    masm.setupUnalignedABICall(temp);
    masm.passABIArg(target);
    masm.passABIArg(targetOffset);
    masm.passABIArg(source);
    masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, js::SetDisjointTypedElements));
}

void
CodeGenerator::visitTypedObjectDescr(LTypedObjectDescr* lir)
{
    Register obj = ToRegister(lir->object());
    Register out = ToRegister(lir->output());
    masm.loadTypedObjectDescr(obj, out);
}

void
CodeGenerator::visitTypedObjectElements(LTypedObjectElements* lir)
{
    Register obj = ToRegister(lir->object());
    Register out = ToRegister(lir->output());

    if (lir->mir()->definitelyOutline()) {
        masm.loadPtr(Address(obj, OutlineTypedObject::offsetOfData()), out);
    } else {
        Label inlineObject, done;
        masm.branchIfInlineTypedObject(obj, out, &inlineObject);

        masm.loadPtr(Address(obj, OutlineTypedObject::offsetOfData()), out);
        masm.jump(&done);

        masm.bind(&inlineObject);
        masm.computeEffectiveAddress(Address(obj, InlineTypedObject::offsetOfDataStart()), out);
        masm.bind(&done);
    }
}

void
CodeGenerator::visitSetTypedObjectOffset(LSetTypedObjectOffset* lir)
{
    Register object = ToRegister(lir->object());
    Register offset = ToRegister(lir->offset());
    Register temp0 = ToRegister(lir->temp0());
    Register temp1 = ToRegister(lir->temp1());

    // Compute the base pointer for the typed object's owner.
    masm.loadPtr(Address(object, OutlineTypedObject::offsetOfOwner()), temp0);

    Label inlineObject, done;
    masm.branchIfInlineTypedObject(temp0, temp1, &inlineObject);

    masm.loadPrivate(Address(temp0, ArrayBufferObject::offsetOfDataSlot()), temp0);
    masm.jump(&done);

    masm.bind(&inlineObject);
    masm.addPtr(ImmWord(InlineTypedObject::offsetOfDataStart()), temp0);

    masm.bind(&done);

    // Compute the new data pointer and set it in the object.
    masm.addPtr(offset, temp0);
    masm.storePtr(temp0, Address(object, OutlineTypedObject::offsetOfData()));
}

void
CodeGenerator::visitStringLength(LStringLength* lir)
{
    Register input = ToRegister(lir->string());
    Register output = ToRegister(lir->output());

    masm.loadStringLength(input, output);
}

void
CodeGenerator::visitMinMaxI(LMinMaxI* ins)
{
    Register first = ToRegister(ins->first());
    Register output = ToRegister(ins->output());

    MOZ_ASSERT(first == output);

    Label done;
    Assembler::Condition cond = ins->mir()->isMax()
                                ? Assembler::GreaterThan
                                : Assembler::LessThan;

    if (ins->second()->isConstant()) {
        masm.branch32(cond, first, Imm32(ToInt32(ins->second())), &done);
        masm.move32(Imm32(ToInt32(ins->second())), output);
    } else {
        masm.branch32(cond, first, ToRegister(ins->second()), &done);
        masm.move32(ToRegister(ins->second()), output);
    }

    masm.bind(&done);
}

void
CodeGenerator::visitAbsI(LAbsI* ins)
{
    Register input = ToRegister(ins->input());
    Label positive;

    MOZ_ASSERT(input == ToRegister(ins->output()));
    masm.branchTest32(Assembler::NotSigned, input, input, &positive);
    masm.neg32(input);
    LSnapshot* snapshot = ins->snapshot();
#if defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
    if (snapshot)
        bailoutCmp32(Assembler::Equal, input, Imm32(INT32_MIN), snapshot);
#else
    if (snapshot)
        bailoutIf(Assembler::Overflow, snapshot);
#endif
    masm.bind(&positive);
}

void
CodeGenerator::visitPowI(LPowI* ins)
{
    FloatRegister value = ToFloatRegister(ins->value());
    Register power = ToRegister(ins->power());
    Register temp = ToRegister(ins->temp());

    MOZ_ASSERT(power != temp);

    masm.setupUnalignedABICall(temp);
    masm.passABIArg(value, MoveOp::DOUBLE);
    masm.passABIArg(power);

    masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, js::powi), MoveOp::DOUBLE);
    MOZ_ASSERT(ToFloatRegister(ins->output()) == ReturnDoubleReg);
}

void
CodeGenerator::visitPowD(LPowD* ins)
{
    FloatRegister value = ToFloatRegister(ins->value());
    FloatRegister power = ToFloatRegister(ins->power());
    Register temp = ToRegister(ins->temp());

    masm.setupUnalignedABICall(temp);
    masm.passABIArg(value, MoveOp::DOUBLE);
    masm.passABIArg(power, MoveOp::DOUBLE);
    masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, ecmaPow), MoveOp::DOUBLE);

    MOZ_ASSERT(ToFloatRegister(ins->output()) == ReturnDoubleReg);
}

using PowFn = bool (*)(JSContext*, HandleValue, HandleValue, MutableHandleValue);
static const VMFunction PowInfo =
    FunctionInfo<PowFn>(js::math_pow_handle, "math_pow_handle");

void
CodeGenerator::visitPowV(LPowV* ins)
{
    pushArg(ToValue(ins, LPowV::PowerInput));
    pushArg(ToValue(ins, LPowV::ValueInput));
    callVM(PowInfo, ins);
}

void
CodeGenerator::visitMathFunctionD(LMathFunctionD* ins)
{
    Register temp = ToRegister(ins->temp());
    FloatRegister input = ToFloatRegister(ins->input());
    MOZ_ASSERT(ToFloatRegister(ins->output()) == ReturnDoubleReg);

    masm.setupUnalignedABICall(temp);

    const MathCache* mathCache = ins->mir()->cache();
    if (mathCache) {
        masm.movePtr(ImmPtr(mathCache), temp);
        masm.passABIArg(temp);
    }
    masm.passABIArg(input, MoveOp::DOUBLE);

#   define MAYBE_CACHED(fcn) (mathCache ? (void*)fcn ## _impl : (void*)fcn ## _uncached)

    void* funptr = nullptr;
    switch (ins->mir()->function()) {
      case MMathFunction::Log:
        funptr = JS_FUNC_TO_DATA_PTR(void*, MAYBE_CACHED(js::math_log));
        break;
      case MMathFunction::Sin:
        funptr = JS_FUNC_TO_DATA_PTR(void*, MAYBE_CACHED(js::math_sin));
        break;
      case MMathFunction::Cos:
        funptr = JS_FUNC_TO_DATA_PTR(void*, MAYBE_CACHED(js::math_cos));
        break;
      case MMathFunction::Exp:
        funptr = JS_FUNC_TO_DATA_PTR(void*, MAYBE_CACHED(js::math_exp));
        break;
      case MMathFunction::Tan:
        funptr = JS_FUNC_TO_DATA_PTR(void*, MAYBE_CACHED(js::math_tan));
        break;
      case MMathFunction::ATan:
        funptr = JS_FUNC_TO_DATA_PTR(void*, MAYBE_CACHED(js::math_atan));
        break;
      case MMathFunction::ASin:
        funptr = JS_FUNC_TO_DATA_PTR(void*, MAYBE_CACHED(js::math_asin));
        break;
      case MMathFunction::ACos:
        funptr = JS_FUNC_TO_DATA_PTR(void*, MAYBE_CACHED(js::math_acos));
        break;
      case MMathFunction::Log10:
        funptr = JS_FUNC_TO_DATA_PTR(void*, MAYBE_CACHED(js::math_log10));
        break;
      case MMathFunction::Log2:
        funptr = JS_FUNC_TO_DATA_PTR(void*, MAYBE_CACHED(js::math_log2));
        break;
      case MMathFunction::Log1P:
        funptr = JS_FUNC_TO_DATA_PTR(void*, MAYBE_CACHED(js::math_log1p));
        break;
      case MMathFunction::ExpM1:
        funptr = JS_FUNC_TO_DATA_PTR(void*, MAYBE_CACHED(js::math_expm1));
        break;
      case MMathFunction::CosH:
        funptr = JS_FUNC_TO_DATA_PTR(void*, MAYBE_CACHED(js::math_cosh));
        break;
      case MMathFunction::SinH:
        funptr = JS_FUNC_TO_DATA_PTR(void*, MAYBE_CACHED(js::math_sinh));
        break;
      case MMathFunction::TanH:
        funptr = JS_FUNC_TO_DATA_PTR(void*, MAYBE_CACHED(js::math_tanh));
        break;
      case MMathFunction::ACosH:
        funptr = JS_FUNC_TO_DATA_PTR(void*, MAYBE_CACHED(js::math_acosh));
        break;
      case MMathFunction::ASinH:
        funptr = JS_FUNC_TO_DATA_PTR(void*, MAYBE_CACHED(js::math_asinh));
        break;
      case MMathFunction::ATanH:
        funptr = JS_FUNC_TO_DATA_PTR(void*, MAYBE_CACHED(js::math_atanh));
        break;
      case MMathFunction::Sign:
        funptr = JS_FUNC_TO_DATA_PTR(void*, MAYBE_CACHED(js::math_sign));
        break;
      case MMathFunction::Trunc:
        funptr = JS_FUNC_TO_DATA_PTR(void*, MAYBE_CACHED(js::math_trunc));
        break;
      case MMathFunction::Cbrt:
        funptr = JS_FUNC_TO_DATA_PTR(void*, MAYBE_CACHED(js::math_cbrt));
        break;
      case MMathFunction::Floor:
        funptr = JS_FUNC_TO_DATA_PTR(void*, js::math_floor_impl);
        break;
      case MMathFunction::Ceil:
        funptr = JS_FUNC_TO_DATA_PTR(void*, js::math_ceil_impl);
        break;
      case MMathFunction::Round:
        funptr = JS_FUNC_TO_DATA_PTR(void*, js::math_round_impl);
        break;
      default:
        MOZ_CRASH("Unknown math function");
    }

#   undef MAYBE_CACHED

    masm.callWithABI(funptr, MoveOp::DOUBLE);
}

void
CodeGenerator::visitMathFunctionF(LMathFunctionF* ins)
{
    Register temp = ToRegister(ins->temp());
    FloatRegister input = ToFloatRegister(ins->input());
    MOZ_ASSERT(ToFloatRegister(ins->output()) == ReturnFloat32Reg);

    masm.setupUnalignedABICall(temp);
    masm.passABIArg(input, MoveOp::FLOAT32);

    void* funptr = nullptr;
    CheckUnsafeCallWithABI check = CheckUnsafeCallWithABI::Check;
    switch (ins->mir()->function()) {
      case MMathFunction::Floor:
        funptr = JS_FUNC_TO_DATA_PTR(void*, floorf);
        check = CheckUnsafeCallWithABI::DontCheckOther;
        break;
      case MMathFunction::Round:
        funptr = JS_FUNC_TO_DATA_PTR(void*, math_roundf_impl);
        break;
      case MMathFunction::Ceil:
        funptr = JS_FUNC_TO_DATA_PTR(void*, ceilf);
        check = CheckUnsafeCallWithABI::DontCheckOther;
        break;
      default:
        MOZ_CRASH("Unknown or unsupported float32 math function");
    }

    masm.callWithABI(funptr, MoveOp::FLOAT32, check);
}

void
CodeGenerator::visitModD(LModD* ins)
{
    FloatRegister lhs = ToFloatRegister(ins->lhs());
    FloatRegister rhs = ToFloatRegister(ins->rhs());

    MOZ_ASSERT(ToFloatRegister(ins->output()) == ReturnDoubleReg);
    MOZ_ASSERT(ins->temp()->isBogusTemp() == gen->compilingWasm());

    if (gen->compilingWasm()) {
        masm.setupWasmABICall();
        masm.passABIArg(lhs, MoveOp::DOUBLE);
        masm.passABIArg(rhs, MoveOp::DOUBLE);
        masm.callWithABI(ins->mir()->bytecodeOffset(), wasm::SymbolicAddress::ModD, MoveOp::DOUBLE);
    } else {
        masm.setupUnalignedABICall(ToRegister(ins->temp()));
        masm.passABIArg(lhs, MoveOp::DOUBLE);
        masm.passABIArg(rhs, MoveOp::DOUBLE);
        masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, NumberMod), MoveOp::DOUBLE);
    }
}

typedef bool (*BinaryFn)(JSContext*, MutableHandleValue, MutableHandleValue, MutableHandleValue);

static const VMFunction AddInfo = FunctionInfo<BinaryFn>(js::AddValues, "AddValues");
static const VMFunction SubInfo = FunctionInfo<BinaryFn>(js::SubValues, "SubValues");
static const VMFunction MulInfo = FunctionInfo<BinaryFn>(js::MulValues, "MulValues");
static const VMFunction DivInfo = FunctionInfo<BinaryFn>(js::DivValues, "DivValues");
static const VMFunction ModInfo = FunctionInfo<BinaryFn>(js::ModValues, "ModValues");
static const VMFunction UrshInfo = FunctionInfo<BinaryFn>(js::UrshValues, "UrshValues");

void
CodeGenerator::visitBinaryV(LBinaryV* lir)
{
    pushArg(ToValue(lir, LBinaryV::RhsInput));
    pushArg(ToValue(lir, LBinaryV::LhsInput));

    switch (lir->jsop()) {
      case JSOP_ADD:
        callVM(AddInfo, lir);
        break;

      case JSOP_SUB:
        callVM(SubInfo, lir);
        break;

      case JSOP_MUL:
        callVM(MulInfo, lir);
        break;

      case JSOP_DIV:
        callVM(DivInfo, lir);
        break;

      case JSOP_MOD:
        callVM(ModInfo, lir);
        break;

      case JSOP_URSH:
        callVM(UrshInfo, lir);
        break;

      default:
        MOZ_CRASH("Unexpected binary op");
    }
}

typedef bool (*StringCompareFn)(JSContext*, HandleString, HandleString, bool*);
static const VMFunction StringsEqualInfo =
    FunctionInfo<StringCompareFn>(jit::StringsEqual<true>, "StringsEqual");
static const VMFunction StringsNotEqualInfo =
    FunctionInfo<StringCompareFn>(jit::StringsEqual<false>, "StringsEqual");

void
CodeGenerator::emitCompareS(LInstruction* lir, JSOp op, Register left, Register right,
                            Register output)
{
    MOZ_ASSERT(lir->isCompareS() || lir->isCompareStrictS());

    OutOfLineCode* ool = nullptr;

    if (op == JSOP_EQ || op == JSOP_STRICTEQ) {
        ool = oolCallVM(StringsEqualInfo, lir, ArgList(left, right),  StoreRegisterTo(output));
    } else {
        MOZ_ASSERT(op == JSOP_NE || op == JSOP_STRICTNE);
        ool = oolCallVM(StringsNotEqualInfo, lir, ArgList(left, right), StoreRegisterTo(output));
    }

    masm.compareStrings(op, left, right, output, ool->entry());

    masm.bind(ool->rejoin());
}

void
CodeGenerator::visitCompareStrictS(LCompareStrictS* lir)
{
    JSOp op = lir->mir()->jsop();
    MOZ_ASSERT(op == JSOP_STRICTEQ || op == JSOP_STRICTNE);

    const ValueOperand leftV = ToValue(lir, LCompareStrictS::Lhs);
    Register right = ToRegister(lir->right());
    Register output = ToRegister(lir->output());
    Register tempToUnbox = ToTempUnboxRegister(lir->tempToUnbox());

    Label string, done;

    masm.branchTestString(Assembler::Equal, leftV, &string);
    masm.move32(Imm32(op == JSOP_STRICTNE), output);
    masm.jump(&done);

    masm.bind(&string);
    Register left = masm.extractString(leftV, tempToUnbox);
    emitCompareS(lir, op, left, right, output);

    masm.bind(&done);
}

void
CodeGenerator::visitCompareS(LCompareS* lir)
{
    JSOp op = lir->mir()->jsop();
    Register left = ToRegister(lir->left());
    Register right = ToRegister(lir->right());
    Register output = ToRegister(lir->output());

    emitCompareS(lir, op, left, right, output);
}

typedef bool (*CompareFn)(JSContext*, MutableHandleValue, MutableHandleValue, bool*);
static const VMFunction EqInfo =
    FunctionInfo<CompareFn>(jit::LooselyEqual<true>, "LooselyEqual");
static const VMFunction NeInfo =
    FunctionInfo<CompareFn>(jit::LooselyEqual<false>, "LooselyEqual");
static const VMFunction StrictEqInfo =
    FunctionInfo<CompareFn>(jit::StrictlyEqual<true>, "StrictlyEqual");
static const VMFunction StrictNeInfo =
    FunctionInfo<CompareFn>(jit::StrictlyEqual<false>, "StrictlyEqual");
static const VMFunction LtInfo =
    FunctionInfo<CompareFn>(jit::LessThan, "LessThan");
static const VMFunction LeInfo =
    FunctionInfo<CompareFn>(jit::LessThanOrEqual, "LessThanOrEqual");
static const VMFunction GtInfo =
    FunctionInfo<CompareFn>(jit::GreaterThan, "GreaterThan");
static const VMFunction GeInfo =
    FunctionInfo<CompareFn>(jit::GreaterThanOrEqual, "GreaterThanOrEqual");

void
CodeGenerator::visitCompareVM(LCompareVM* lir)
{
    pushArg(ToValue(lir, LBinaryV::RhsInput));
    pushArg(ToValue(lir, LBinaryV::LhsInput));

    switch (lir->mir()->jsop()) {
      case JSOP_EQ:
        callVM(EqInfo, lir);
        break;

      case JSOP_NE:
        callVM(NeInfo, lir);
        break;

      case JSOP_STRICTEQ:
        callVM(StrictEqInfo, lir);
        break;

      case JSOP_STRICTNE:
        callVM(StrictNeInfo, lir);
        break;

      case JSOP_LT:
        callVM(LtInfo, lir);
        break;

      case JSOP_LE:
        callVM(LeInfo, lir);
        break;

      case JSOP_GT:
        callVM(GtInfo, lir);
        break;

      case JSOP_GE:
        callVM(GeInfo, lir);
        break;

      default:
        MOZ_CRASH("Unexpected compare op");
    }
}

void
CodeGenerator::visitIsNullOrLikeUndefinedV(LIsNullOrLikeUndefinedV* lir)
{
    JSOp op = lir->mir()->jsop();
    MCompare::CompareType compareType = lir->mir()->compareType();
    MOZ_ASSERT(compareType == MCompare::Compare_Undefined ||
               compareType == MCompare::Compare_Null);

    const ValueOperand value = ToValue(lir, LIsNullOrLikeUndefinedV::Value);
    Register output = ToRegister(lir->output());

    if (op == JSOP_EQ || op == JSOP_NE) {
        MOZ_ASSERT(lir->mir()->lhs()->type() != MIRType::Object ||
                   lir->mir()->operandMightEmulateUndefined(),
                   "Operands which can't emulate undefined should have been folded");

        OutOfLineTestObjectWithLabels* ool = nullptr;
        Maybe<Label> label1, label2;
        Label* nullOrLikeUndefined;
        Label* notNullOrLikeUndefined;
        if (lir->mir()->operandMightEmulateUndefined()) {
            ool = new(alloc()) OutOfLineTestObjectWithLabels();
            addOutOfLineCode(ool, lir->mir());
            nullOrLikeUndefined = ool->label1();
            notNullOrLikeUndefined = ool->label2();
        } else {
            label1.emplace();
            label2.emplace();
            nullOrLikeUndefined = label1.ptr();
            notNullOrLikeUndefined = label2.ptr();
        }

        {
            ScratchTagScope tag(masm, value);
            masm.splitTagForTest(value, tag);

            MDefinition* input = lir->mir()->lhs();
            if (input->mightBeType(MIRType::Null))
                masm.branchTestNull(Assembler::Equal, tag, nullOrLikeUndefined);
            if (input->mightBeType(MIRType::Undefined))
                masm.branchTestUndefined(Assembler::Equal, tag, nullOrLikeUndefined);

            if (ool) {
                // Check whether it's a truthy object or a falsy object that emulates
                // undefined.
                masm.branchTestObject(Assembler::NotEqual, tag, notNullOrLikeUndefined);

                ScratchTagScopeRelease _(&tag);

                Register objreg = masm.extractObject(value, ToTempUnboxRegister(lir->tempToUnbox()));
                branchTestObjectEmulatesUndefined(objreg, nullOrLikeUndefined, notNullOrLikeUndefined,
                                                  ToRegister(lir->temp()), ool);
                // fall through
            }
        }

        Label done;

        // It's not null or undefined, and if it's an object it doesn't
        // emulate undefined, so it's not like undefined.
        masm.move32(Imm32(op == JSOP_NE), output);
        masm.jump(&done);

        masm.bind(nullOrLikeUndefined);
        masm.move32(Imm32(op == JSOP_EQ), output);

        // Both branches meet here.
        masm.bind(&done);
        return;
    }

    MOZ_ASSERT(op == JSOP_STRICTEQ || op == JSOP_STRICTNE);

    Assembler::Condition cond = JSOpToCondition(compareType, op);
    if (compareType == MCompare::Compare_Null)
        masm.testNullSet(cond, value, output);
    else
        masm.testUndefinedSet(cond, value, output);
}

void
CodeGenerator::visitIsNullOrLikeUndefinedAndBranchV(LIsNullOrLikeUndefinedAndBranchV* lir)
{
    JSOp op = lir->cmpMir()->jsop();
    MCompare::CompareType compareType = lir->cmpMir()->compareType();
    MOZ_ASSERT(compareType == MCompare::Compare_Undefined ||
               compareType == MCompare::Compare_Null);

    const ValueOperand value = ToValue(lir, LIsNullOrLikeUndefinedAndBranchV::Value);

    if (op == JSOP_EQ || op == JSOP_NE) {
        MBasicBlock* ifTrue;
        MBasicBlock* ifFalse;

        if (op == JSOP_EQ) {
            ifTrue = lir->ifTrue();
            ifFalse = lir->ifFalse();
        } else {
            // Swap branches.
            ifTrue = lir->ifFalse();
            ifFalse = lir->ifTrue();
            op = JSOP_EQ;
        }

        MOZ_ASSERT(lir->cmpMir()->lhs()->type() != MIRType::Object ||
                   lir->cmpMir()->operandMightEmulateUndefined(),
                   "Operands which can't emulate undefined should have been folded");

        OutOfLineTestObject* ool = nullptr;
        if (lir->cmpMir()->operandMightEmulateUndefined()) {
            ool = new(alloc()) OutOfLineTestObject();
            addOutOfLineCode(ool, lir->cmpMir());
        }

        {
            ScratchTagScope tag(masm, value);
            masm.splitTagForTest(value, tag);

            Label* ifTrueLabel = getJumpLabelForBranch(ifTrue);
            Label* ifFalseLabel = getJumpLabelForBranch(ifFalse);

            MDefinition* input = lir->cmpMir()->lhs();
            if (input->mightBeType(MIRType::Null))
                masm.branchTestNull(Assembler::Equal, tag, ifTrueLabel);
            if (input->mightBeType(MIRType::Undefined))
                masm.branchTestUndefined(Assembler::Equal, tag, ifTrueLabel);

            if (ool) {
                masm.branchTestObject(Assembler::NotEqual, tag, ifFalseLabel);

                ScratchTagScopeRelease _(&tag);

                // Objects that emulate undefined are loosely equal to null/undefined.
                Register objreg = masm.extractObject(value, ToTempUnboxRegister(lir->tempToUnbox()));
                Register scratch = ToRegister(lir->temp());
                testObjectEmulatesUndefined(objreg, ifTrueLabel, ifFalseLabel, scratch, ool);
            } else {
                masm.jump(ifFalseLabel);
            }
            return;
        }
    }

    MOZ_ASSERT(op == JSOP_STRICTEQ || op == JSOP_STRICTNE);

    Assembler::Condition cond = JSOpToCondition(compareType, op);
    if (compareType == MCompare::Compare_Null)
        testNullEmitBranch(cond, value, lir->ifTrue(), lir->ifFalse());
    else
        testUndefinedEmitBranch(cond, value, lir->ifTrue(), lir->ifFalse());
}

void
CodeGenerator::visitIsNullOrLikeUndefinedT(LIsNullOrLikeUndefinedT * lir)
{
    MOZ_ASSERT(lir->mir()->compareType() == MCompare::Compare_Undefined ||
               lir->mir()->compareType() == MCompare::Compare_Null);

    MIRType lhsType = lir->mir()->lhs()->type();
    MOZ_ASSERT(lhsType == MIRType::Object || lhsType == MIRType::ObjectOrNull);

    JSOp op = lir->mir()->jsop();
    MOZ_ASSERT(lhsType == MIRType::ObjectOrNull || op == JSOP_EQ || op == JSOP_NE,
               "Strict equality should have been folded");

    MOZ_ASSERT(lhsType == MIRType::ObjectOrNull || lir->mir()->operandMightEmulateUndefined(),
               "If the object couldn't emulate undefined, this should have been folded.");

    Register objreg = ToRegister(lir->input());
    Register output = ToRegister(lir->output());

    if ((op == JSOP_EQ || op == JSOP_NE) && lir->mir()->operandMightEmulateUndefined()) {
        OutOfLineTestObjectWithLabels* ool = new(alloc()) OutOfLineTestObjectWithLabels();
        addOutOfLineCode(ool, lir->mir());

        Label* emulatesUndefined = ool->label1();
        Label* doesntEmulateUndefined = ool->label2();

        if (lhsType == MIRType::ObjectOrNull)
            masm.branchTestPtr(Assembler::Zero, objreg, objreg, emulatesUndefined);

        branchTestObjectEmulatesUndefined(objreg, emulatesUndefined, doesntEmulateUndefined,
                                          output, ool);

        Label done;

        masm.move32(Imm32(op == JSOP_NE), output);
        masm.jump(&done);

        masm.bind(emulatesUndefined);
        masm.move32(Imm32(op == JSOP_EQ), output);
        masm.bind(&done);
    } else {
        MOZ_ASSERT(lhsType == MIRType::ObjectOrNull);

        Label isNull, done;

        masm.branchTestPtr(Assembler::Zero, objreg, objreg, &isNull);

        masm.move32(Imm32(op == JSOP_NE || op == JSOP_STRICTNE), output);
        masm.jump(&done);

        masm.bind(&isNull);
        masm.move32(Imm32(op == JSOP_EQ || op == JSOP_STRICTEQ), output);

        masm.bind(&done);
    }
}

void
CodeGenerator::visitIsNullOrLikeUndefinedAndBranchT(LIsNullOrLikeUndefinedAndBranchT* lir)
{
    DebugOnly<MCompare::CompareType> compareType = lir->cmpMir()->compareType();
    MOZ_ASSERT(compareType == MCompare::Compare_Undefined ||
               compareType == MCompare::Compare_Null);

    MIRType lhsType = lir->cmpMir()->lhs()->type();
    MOZ_ASSERT(lhsType == MIRType::Object || lhsType == MIRType::ObjectOrNull);

    JSOp op = lir->cmpMir()->jsop();
    MOZ_ASSERT(lhsType == MIRType::ObjectOrNull || op == JSOP_EQ || op == JSOP_NE,
               "Strict equality should have been folded");

    MOZ_ASSERT(lhsType == MIRType::ObjectOrNull || lir->cmpMir()->operandMightEmulateUndefined(),
               "If the object couldn't emulate undefined, this should have been folded.");

    MBasicBlock* ifTrue;
    MBasicBlock* ifFalse;

    if (op == JSOP_EQ || op == JSOP_STRICTEQ) {
        ifTrue = lir->ifTrue();
        ifFalse = lir->ifFalse();
    } else {
        // Swap branches.
        ifTrue = lir->ifFalse();
        ifFalse = lir->ifTrue();
    }

    Register input = ToRegister(lir->getOperand(0));

    if ((op == JSOP_EQ || op == JSOP_NE) && lir->cmpMir()->operandMightEmulateUndefined()) {
        OutOfLineTestObject* ool = new(alloc()) OutOfLineTestObject();
        addOutOfLineCode(ool, lir->cmpMir());

        Label* ifTrueLabel = getJumpLabelForBranch(ifTrue);
        Label* ifFalseLabel = getJumpLabelForBranch(ifFalse);

        if (lhsType == MIRType::ObjectOrNull)
            masm.branchTestPtr(Assembler::Zero, input, input, ifTrueLabel);

        // Objects that emulate undefined are loosely equal to null/undefined.
        Register scratch = ToRegister(lir->temp());
        testObjectEmulatesUndefined(input, ifTrueLabel, ifFalseLabel, scratch, ool);
    } else {
        MOZ_ASSERT(lhsType == MIRType::ObjectOrNull);
        testZeroEmitBranch(Assembler::Equal, input, ifTrue, ifFalse);
    }
}

void
CodeGenerator::emitSameValue(FloatRegister left, FloatRegister right, FloatRegister temp,
                             Register output)
{
    Label nonEqual, isSameValue, isNotSameValue;
    masm.branchDouble(Assembler::DoubleNotEqualOrUnordered, left, right, &nonEqual);
    {
        // First, test for being equal to 0.0, which also includes -0.0.
        masm.loadConstantDouble(0.0, temp);
        masm.branchDouble(Assembler::DoubleNotEqual, left, temp, &isSameValue);

        // The easiest way to distinguish -0.0 from 0.0 is that 1.0/-0.0
        // is -Infinity instead of Infinity.
        Label isNegInf;
        masm.loadConstantDouble(1.0, temp);
        masm.divDouble(left, temp);
        masm.branchDouble(Assembler::DoubleLessThan, temp, left, &isNegInf);
        {
            masm.loadConstantDouble(1.0, temp);
            masm.divDouble(right, temp);
            masm.branchDouble(Assembler::DoubleGreaterThan, temp, right, &isSameValue);
            masm.jump(&isNotSameValue);
        }
        masm.bind(&isNegInf);
        {
            masm.loadConstantDouble(1.0, temp);
            masm.divDouble(right, temp);
            masm.branchDouble(Assembler::DoubleLessThan, temp, right, &isSameValue);
            masm.jump(&isNotSameValue);
        }
    }
    masm.bind(&nonEqual);
    {
        // Test if both values are NaN.
        masm.branchDouble(Assembler::DoubleOrdered, left, left, &isNotSameValue);
        masm.branchDouble(Assembler::DoubleOrdered, right, right, &isNotSameValue);
    }

    Label done;
    masm.bind(&isSameValue);
    masm.move32(Imm32(1), output);
    masm.jump(&done);

    masm.bind(&isNotSameValue);
    masm.move32(Imm32(0), output);

    masm.bind(&done);
}

void
CodeGenerator::visitSameValueD(LSameValueD* lir)
{
    FloatRegister left = ToFloatRegister(lir->left());
    FloatRegister right = ToFloatRegister(lir->right());
    FloatRegister temp = ToFloatRegister(lir->tempFloat());
    Register output = ToRegister(lir->output());

    emitSameValue(left, right, temp, output);
}

void
CodeGenerator::visitSameValueV(LSameValueV* lir)
{
    ValueOperand left = ToValue(lir, LSameValueV::LhsInput);
    FloatRegister right = ToFloatRegister(lir->right());
    FloatRegister temp1 = ToFloatRegister(lir->tempFloat1());
    FloatRegister temp2 = ToFloatRegister(lir->tempFloat2());
    Register output = ToRegister(lir->output());

    Label nonDouble;
    masm.move32(Imm32(0), output);
    masm.ensureDouble(left, temp1, &nonDouble);
    emitSameValue(temp1, right, temp2, output);
    masm.bind(&nonDouble);
}

typedef bool (*SameValueFn)(JSContext*, HandleValue, HandleValue, bool*);
static const VMFunction SameValueInfo = FunctionInfo<SameValueFn>(js::SameValue, "SameValue");

void
CodeGenerator::visitSameValueVM(LSameValueVM* lir)
{
    pushArg(ToValue(lir, LSameValueVM::RhsInput));
    pushArg(ToValue(lir, LSameValueVM::LhsInput));
    callVM(SameValueInfo, lir);
}

typedef JSString* (*ConcatStringsFn)(JSContext*, HandleString, HandleString);
static const VMFunction ConcatStringsInfo =
    FunctionInfo<ConcatStringsFn>(ConcatStrings<CanGC>, "ConcatStrings");

void
CodeGenerator::emitConcat(LInstruction* lir, Register lhs, Register rhs, Register output)
{
    OutOfLineCode* ool = oolCallVM(ConcatStringsInfo, lir, ArgList(lhs, rhs),
                                   StoreRegisterTo(output));

    const JitCompartment* jitCompartment = gen->compartment->jitCompartment();
    JitCode* stringConcatStub = jitCompartment->stringConcatStubNoBarrier(&compartmentStubsToReadBarrier_);
    masm.call(stringConcatStub);
    masm.branchTestPtr(Assembler::Zero, output, output, ool->entry());

    masm.bind(ool->rejoin());
}

void
CodeGenerator::visitConcat(LConcat* lir)
{
    Register lhs = ToRegister(lir->lhs());
    Register rhs = ToRegister(lir->rhs());

    Register output = ToRegister(lir->output());

    MOZ_ASSERT(lhs == CallTempReg0);
    MOZ_ASSERT(rhs == CallTempReg1);
    MOZ_ASSERT(ToRegister(lir->temp1()) == CallTempReg0);
    MOZ_ASSERT(ToRegister(lir->temp2()) == CallTempReg1);
    MOZ_ASSERT(ToRegister(lir->temp3()) == CallTempReg2);
    MOZ_ASSERT(ToRegister(lir->temp4()) == CallTempReg3);
    MOZ_ASSERT(ToRegister(lir->temp5()) == CallTempReg4);
    MOZ_ASSERT(output == CallTempReg5);

    emitConcat(lir, lhs, rhs, output);
}

static void
CopyStringChars(MacroAssembler& masm, Register to, Register from, Register len,
                Register byteOpScratch, size_t fromWidth, size_t toWidth)
{
    // Copy |len| char16_t code units from |from| to |to|. Assumes len > 0
    // (checked below in debug builds), and when done |to| must point to the
    // next available char.

#ifdef DEBUG
    Label ok;
    masm.branch32(Assembler::GreaterThan, len, Imm32(0), &ok);
    masm.assumeUnreachable("Length should be greater than 0.");
    masm.bind(&ok);
#endif

    MOZ_ASSERT(fromWidth == 1 || fromWidth == 2);
    MOZ_ASSERT(toWidth == 1 || toWidth == 2);
    MOZ_ASSERT_IF(toWidth == 1, fromWidth == 1);

    Label start;
    masm.bind(&start);
    if (fromWidth == 2)
        masm.load16ZeroExtend(Address(from, 0), byteOpScratch);
    else
        masm.load8ZeroExtend(Address(from, 0), byteOpScratch);
    if (toWidth == 2)
        masm.store16(byteOpScratch, Address(to, 0));
    else
        masm.store8(byteOpScratch, Address(to, 0));
    masm.addPtr(Imm32(fromWidth), from);
    masm.addPtr(Imm32(toWidth), to);
    masm.branchSub32(Assembler::NonZero, Imm32(1), len, &start);
}

static void
CopyStringCharsMaybeInflate(MacroAssembler& masm, Register input, Register destChars,
                            Register temp1, Register temp2)
{
    // destChars is TwoByte and input is a Latin1 or TwoByte string, so we may
    // have to inflate.

    Label isLatin1, done;
    masm.loadStringLength(input, temp1);
    masm.branchLatin1String(input, &isLatin1);
    {
        masm.loadStringChars(input, temp2, CharEncoding::TwoByte);
        masm.movePtr(temp2, input);
        CopyStringChars(masm, destChars, input, temp1, temp2, sizeof(char16_t), sizeof(char16_t));
        masm.jump(&done);
    }
    masm.bind(&isLatin1);
    {
        masm.loadStringChars(input, temp2, CharEncoding::Latin1);
        masm.movePtr(temp2, input);
        CopyStringChars(masm, destChars, input, temp1, temp2, sizeof(char), sizeof(char16_t));
    }
    masm.bind(&done);
}

static void
ConcatInlineString(MacroAssembler& masm, Register lhs, Register rhs, Register output,
                   Register temp1, Register temp2, Register temp3,
                   bool stringsCanBeInNursery,
                   Label* failure, bool isTwoByte)
{
    // State: result length in temp2.

    // Ensure both strings are linear.
    masm.branchIfRope(lhs, failure);
    masm.branchIfRope(rhs, failure);

    // Allocate a JSThinInlineString or JSFatInlineString.
    size_t maxThinInlineLength;
    if (isTwoByte)
        maxThinInlineLength = JSThinInlineString::MAX_LENGTH_TWO_BYTE;
    else
        maxThinInlineLength = JSThinInlineString::MAX_LENGTH_LATIN1;

    Label isFat, allocDone;
    masm.branch32(Assembler::Above, temp2, Imm32(maxThinInlineLength), &isFat);
    {
        uint32_t flags = JSString::INIT_THIN_INLINE_FLAGS;
        if (!isTwoByte)
            flags |= JSString::LATIN1_CHARS_BIT;
        masm.newGCString(output, temp1, failure, stringsCanBeInNursery);
        masm.store32(Imm32(flags), Address(output, JSString::offsetOfFlags()));
        masm.jump(&allocDone);
    }
    masm.bind(&isFat);
    {
        uint32_t flags = JSString::INIT_FAT_INLINE_FLAGS;
        if (!isTwoByte)
            flags |= JSString::LATIN1_CHARS_BIT;
        masm.newGCFatInlineString(output, temp1, failure, stringsCanBeInNursery);
        masm.store32(Imm32(flags), Address(output, JSString::offsetOfFlags()));
    }
    masm.bind(&allocDone);

    // Store length.
    masm.store32(temp2, Address(output, JSString::offsetOfLength()));

    // Load chars pointer in temp2.
    masm.loadInlineStringCharsForStore(output, temp2);

    {
        // Copy lhs chars. Note that this advances temp2 to point to the next
        // char. This also clobbers the lhs register.
        if (isTwoByte) {
            CopyStringCharsMaybeInflate(masm, lhs, temp2, temp1, temp3);
        } else {
            masm.loadStringLength(lhs, temp3);
            masm.loadStringChars(lhs, temp1, CharEncoding::Latin1);
            masm.movePtr(temp1, lhs);
            CopyStringChars(masm, temp2, lhs, temp3, temp1, sizeof(char), sizeof(char));
        }

        // Copy rhs chars. Clobbers the rhs register.
        if (isTwoByte) {
            CopyStringCharsMaybeInflate(masm, rhs, temp2, temp1, temp3);
        } else {
            masm.loadStringLength(rhs, temp3);
            masm.loadStringChars(rhs, temp1, CharEncoding::Latin1);
            masm.movePtr(temp1, rhs);
            CopyStringChars(masm, temp2, rhs, temp3, temp1, sizeof(char), sizeof(char));
        }

        // Null-terminate.
        if (isTwoByte)
            masm.store16(Imm32(0), Address(temp2, 0));
        else
            masm.store8(Imm32(0), Address(temp2, 0));
    }

    masm.ret();
}

typedef JSString* (*SubstringKernelFn)(JSContext* cx, HandleString str, int32_t begin, int32_t len);
static const VMFunction SubstringKernelInfo =
    FunctionInfo<SubstringKernelFn>(SubstringKernel, "SubstringKernel");

void
CodeGenerator::visitSubstr(LSubstr* lir)
{
    Register string = ToRegister(lir->string());
    Register begin = ToRegister(lir->begin());
    Register length = ToRegister(lir->length());
    Register output = ToRegister(lir->output());
    Register temp = ToRegister(lir->temp());
    Register temp3 = ToRegister(lir->temp3());

    // On x86 there are not enough registers. In that case reuse the string
    // register as temporary.
    Register temp2 = lir->temp2()->isBogusTemp() ? string : ToRegister(lir->temp2());

    Address stringFlags(string, JSString::offsetOfFlags());

    Label isLatin1, notInline, nonZero, isInlinedLatin1;

    // For every edge case use the C++ variant.
    // Note: we also use this upon allocation failure in newGCString and
    // newGCFatInlineString. To squeeze out even more performance those failures
    // can be handled by allocate in ool code and returning to jit code to fill
    // in all data.
    OutOfLineCode* ool = oolCallVM(SubstringKernelInfo, lir,
                                   ArgList(string, begin, length),
                                   StoreRegisterTo(output));
    Label* slowPath = ool->entry();
    Label* done = ool->rejoin();

    // Zero length, return emptystring.
    masm.branchTest32(Assembler::NonZero, length, length, &nonZero);
    const JSAtomState& names = gen->runtime->names();
    masm.movePtr(ImmGCPtr(names.empty), output);
    masm.jump(done);

    // Use slow path for ropes.
    masm.bind(&nonZero);
    masm.branchIfRopeOrExternal(string, temp, slowPath);

    // Handle inlined strings by creating a FatInlineString.
    masm.branchTest32(Assembler::Zero, stringFlags, Imm32(JSString::INLINE_CHARS_BIT), &notInline);
    masm.newGCFatInlineString(output, temp, slowPath, stringsCanBeInNursery());
    masm.store32(length, Address(output, JSString::offsetOfLength()));

    masm.branchLatin1String(string, &isInlinedLatin1);
    {
        masm.store32(Imm32(JSString::INIT_FAT_INLINE_FLAGS),
                     Address(output, JSString::offsetOfFlags()));
        masm.loadInlineStringChars(string, temp, CharEncoding::TwoByte);
        if (temp2 == string)
            masm.push(string);
        BaseIndex chars(temp, begin, ScaleFromElemWidth(sizeof(char16_t)));
        masm.computeEffectiveAddress(chars, temp2);
        masm.loadInlineStringCharsForStore(output, temp);
        CopyStringChars(masm, temp, temp2, length, temp3, sizeof(char16_t), sizeof(char16_t));
        masm.load32(Address(output, JSString::offsetOfLength()), length);
        masm.store16(Imm32(0), Address(temp, 0));
        if (temp2 == string)
            masm.pop(string);
        masm.jump(done);
    }
    masm.bind(&isInlinedLatin1);
    {
        masm.store32(Imm32(JSString::INIT_FAT_INLINE_FLAGS | JSString::LATIN1_CHARS_BIT),
                     Address(output, JSString::offsetOfFlags()));
        if (temp2 == string) {
            masm.push(string);
            masm.loadInlineStringChars(string, temp, CharEncoding::Latin1);
            masm.movePtr(temp, temp2);
        } else {
            masm.loadInlineStringChars(string, temp2, CharEncoding::Latin1);
        }
        static_assert(sizeof(char) == 1, "begin index shouldn't need scaling");
        masm.addPtr(begin, temp2);
        masm.loadInlineStringCharsForStore(output, temp);
        CopyStringChars(masm, temp, temp2, length, temp3, sizeof(char), sizeof(char));
        masm.load32(Address(output, JSString::offsetOfLength()), length);
        masm.store8(Imm32(0), Address(temp, 0));
        if (temp2 == string)
            masm.pop(string);
        masm.jump(done);
    }

    // Handle other cases with a DependentString.
    masm.bind(&notInline);
    masm.newGCString(output, temp, slowPath, gen->stringsCanBeInNursery());
    masm.store32(length, Address(output, JSString::offsetOfLength()));
    masm.storeDependentStringBase(string, output);

    masm.branchLatin1String(string, &isLatin1);
    {
        masm.store32(Imm32(JSString::DEPENDENT_FLAGS), Address(output, JSString::offsetOfFlags()));
        masm.loadNonInlineStringChars(string, temp, CharEncoding::TwoByte);
        BaseIndex chars(temp, begin, ScaleFromElemWidth(sizeof(char16_t)));
        masm.computeEffectiveAddress(chars, temp);
        masm.storeNonInlineStringChars(temp, output);
        masm.jump(done);
    }
    masm.bind(&isLatin1);
    {
        masm.store32(Imm32(JSString::DEPENDENT_FLAGS | JSString::LATIN1_CHARS_BIT),
                     Address(output, JSString::offsetOfFlags()));
        masm.loadNonInlineStringChars(string, temp, CharEncoding::Latin1);
        static_assert(sizeof(char) == 1, "begin index shouldn't need scaling");
        masm.addPtr(begin, temp);
        masm.storeNonInlineStringChars(temp, output);
        masm.jump(done);
    }

    masm.bind(done);
}

JitCode*
JitCompartment::generateStringConcatStub(JSContext* cx)
{
    MacroAssembler masm(cx);

    Register lhs = CallTempReg0;
    Register rhs = CallTempReg1;
    Register temp1 = CallTempReg2;
    Register temp2 = CallTempReg3;
    Register temp3 = CallTempReg4;
    Register output = CallTempReg5;

    Label failure;
#ifdef JS_USE_LINK_REGISTER
    masm.pushReturnAddress();
#endif
    // If lhs is empty, return rhs.
    Label leftEmpty;
    masm.loadStringLength(lhs, temp1);
    masm.branchTest32(Assembler::Zero, temp1, temp1, &leftEmpty);

    // If rhs is empty, return lhs.
    Label rightEmpty;
    masm.loadStringLength(rhs, temp2);
    masm.branchTest32(Assembler::Zero, temp2, temp2, &rightEmpty);

    masm.add32(temp1, temp2);

    // Check if we can use a JSFatInlineString. The result is a Latin1 string if
    // lhs and rhs are both Latin1, so we AND the flags.
    Label isFatInlineTwoByte, isFatInlineLatin1;
    masm.load32(Address(lhs, JSString::offsetOfFlags()), temp1);
    masm.and32(Address(rhs, JSString::offsetOfFlags()), temp1);

    Label isLatin1, notInline;
    masm.branchTest32(Assembler::NonZero, temp1, Imm32(JSString::LATIN1_CHARS_BIT), &isLatin1);
    {
        masm.branch32(Assembler::BelowOrEqual, temp2, Imm32(JSFatInlineString::MAX_LENGTH_TWO_BYTE),
                      &isFatInlineTwoByte);
        masm.jump(&notInline);
    }
    masm.bind(&isLatin1);
    {
        masm.branch32(Assembler::BelowOrEqual, temp2, Imm32(JSFatInlineString::MAX_LENGTH_LATIN1),
                      &isFatInlineLatin1);
    }
    masm.bind(&notInline);

    // Keep AND'ed flags in temp1.

    // Ensure result length <= JSString::MAX_LENGTH.
    masm.branch32(Assembler::Above, temp2, Imm32(JSString::MAX_LENGTH), &failure);

    // Allocate a new rope, guaranteed to be in the nursery.
    masm.newGCString(output, temp3, &failure, stringsCanBeInNursery);

    // Store rope length and flags. temp1 still holds the result of AND'ing the
    // lhs and rhs flags, so we just have to clear the other flags and set
    // NON_ATOM_BIT to get our rope flags (Latin1 if both lhs and rhs are
    // Latin1).
    static_assert(JSString::INIT_ROPE_FLAGS == JSString::NON_ATOM_BIT,
                  "Rope type flags must be NON_ATOM_BIT only");
    masm.and32(Imm32(JSString::LATIN1_CHARS_BIT), temp1);
    masm.or32(Imm32(JSString::NON_ATOM_BIT), temp1);
    masm.store32(temp1, Address(output, JSString::offsetOfFlags()));
    masm.store32(temp2, Address(output, JSString::offsetOfLength()));

    // Store left and right nodes.
    masm.storeRopeChildren(lhs, rhs, output);
    masm.ret();

    masm.bind(&leftEmpty);
    masm.mov(rhs, output);
    masm.ret();

    masm.bind(&rightEmpty);
    masm.mov(lhs, output);
    masm.ret();

    masm.bind(&isFatInlineTwoByte);
    ConcatInlineString(masm, lhs, rhs, output, temp1, temp2, temp3,
                       stringsCanBeInNursery, &failure, true);

    masm.bind(&isFatInlineLatin1);
    ConcatInlineString(masm, lhs, rhs, output, temp1, temp2, temp3,
                       stringsCanBeInNursery, &failure, false);

    masm.pop(temp2);
    masm.pop(temp1);

    masm.bind(&failure);
    masm.movePtr(ImmPtr(nullptr), output);
    masm.ret();

    Linker linker(masm);
    AutoFlushICache afc("StringConcatStub");
    JitCode* code = linker.newCode(cx, CodeKind::Other);

#ifdef JS_ION_PERF
    writePerfSpewerJitCodeProfile(code, "StringConcatStub");
#endif
#ifdef MOZ_VTUNE
    vtune::MarkStub(code, "StringConcatStub");
#endif

    return code;
}

void
JitRuntime::generateMallocStub(MacroAssembler& masm)
{
    const Register regReturn = CallTempReg0;
    const Register regZone = CallTempReg0;
    const Register regNBytes = CallTempReg1;

    mallocStubOffset_ = startTrampolineCode(masm);

    AllocatableRegisterSet regs(RegisterSet::Volatile());
#ifdef JS_USE_LINK_REGISTER
    masm.pushReturnAddress();
#endif
    regs.takeUnchecked(regZone);
    regs.takeUnchecked(regNBytes);
    LiveRegisterSet save(regs.asLiveSet());
    masm.PushRegsInMask(save);

    const Register regTemp = regs.takeAnyGeneral();
    MOZ_ASSERT(regTemp != regNBytes && regTemp != regZone);

    masm.setupUnalignedABICall(regTemp);
    masm.passABIArg(regZone);
    masm.passABIArg(regNBytes);
    masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, MallocWrapper));
    masm.storeCallPointerResult(regReturn);

    masm.PopRegsInMask(save);
    masm.ret();
}

void
JitRuntime::generateFreeStub(MacroAssembler& masm)
{
    const Register regSlots = CallTempReg0;

    freeStubOffset_ = startTrampolineCode(masm);

#ifdef JS_USE_LINK_REGISTER
    masm.pushReturnAddress();
#endif
    AllocatableRegisterSet regs(RegisterSet::Volatile());
    regs.takeUnchecked(regSlots);
    LiveRegisterSet save(regs.asLiveSet());
    masm.PushRegsInMask(save);

    const Register regTemp = regs.takeAnyGeneral();
    MOZ_ASSERT(regTemp != regSlots);

    masm.setupUnalignedABICall(regTemp);
    masm.passABIArg(regSlots);
    masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, js_free), MoveOp::GENERAL,
                     CheckUnsafeCallWithABI::DontCheckOther);

    masm.PopRegsInMask(save);

    masm.ret();
}

void
JitRuntime::generateLazyLinkStub(MacroAssembler& masm)
{
    lazyLinkStubOffset_ = startTrampolineCode(masm);

#ifdef JS_USE_LINK_REGISTER
    masm.pushReturnAddress();
#endif

    AllocatableGeneralRegisterSet regs(GeneralRegisterSet::Volatile());
    Register temp0 = regs.takeAny();
    Register temp1 = regs.takeAny();
    Register temp2 = regs.takeAny();

    masm.loadJSContext(temp0);
    masm.enterFakeExitFrame(temp0, temp2, ExitFrameType::LazyLink);
    masm.moveStackPtrTo(temp1);

    masm.setupUnalignedABICall(temp2);
    masm.passABIArg(temp0);
    masm.passABIArg(temp1);
    masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, LazyLinkTopActivation), MoveOp::GENERAL,
                     CheckUnsafeCallWithABI::DontCheckHasExitFrame);

    masm.leaveExitFrame();

#ifdef JS_USE_LINK_REGISTER
    // Restore the return address such that the emitPrologue function of the
    // CodeGenerator can push it back on the stack with pushReturnAddress.
    masm.popReturnAddress();
#endif
    masm.jump(ReturnReg);
}

void
JitRuntime::generateInterpreterStub(MacroAssembler& masm)
{
    interpreterStubOffset_ = startTrampolineCode(masm);

#ifdef JS_USE_LINK_REGISTER
    masm.pushReturnAddress();
#endif

    AllocatableGeneralRegisterSet regs(GeneralRegisterSet::Volatile());
    Register temp0 = regs.takeAny();
    Register temp1 = regs.takeAny();
    Register temp2 = regs.takeAny();

    masm.loadJSContext(temp0);
    masm.enterFakeExitFrame(temp0, temp2, ExitFrameType::InterpreterStub);
    masm.moveStackPtrTo(temp1);

    masm.setupUnalignedABICall(temp2);
    masm.passABIArg(temp0);
    masm.passABIArg(temp1);
    masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, InvokeFromInterpreterStub), MoveOp::GENERAL,
                     CheckUnsafeCallWithABI::DontCheckHasExitFrame);

    masm.branchIfFalseBool(ReturnReg, masm.failureLabel());
    masm.leaveExitFrame();

    // InvokeFromInterpreterStub stores the return value in argv[0], where the
    // caller stored |this|.
    masm.loadValue(Address(masm.getStackPointer(), JitFrameLayout::offsetOfThis()),
                   JSReturnOperand);
    masm.ret();
}

bool
JitRuntime::generateTLEventVM(MacroAssembler& masm, const VMFunction& f, bool enter)
{
#ifdef JS_TRACE_LOGGING
    bool vmEventEnabled = TraceLogTextIdEnabled(TraceLogger_VM);
    bool vmSpecificEventEnabled = TraceLogTextIdEnabled(TraceLogger_VMSpecific);

    if (vmEventEnabled || vmSpecificEventEnabled) {
        AllocatableRegisterSet regs(RegisterSet::Volatile());
        Register loggerReg = regs.takeAnyGeneral();
        masm.Push(loggerReg);
        masm.loadTraceLogger(loggerReg);

        if (vmEventEnabled) {
            if (enter)
                masm.tracelogStartId(loggerReg, TraceLogger_VM, /* force = */ true);
            else
                masm.tracelogStopId(loggerReg, TraceLogger_VM, /* force = */ true);
        }
        if (vmSpecificEventEnabled) {
            TraceLoggerEvent event(f.name());
            if (!event.hasTextId())
                return false;

            if (enter)
                masm.tracelogStartId(loggerReg, event.textId(), /* force = */ true);
            else
                masm.tracelogStopId(loggerReg, event.textId(), /* force = */ true);
        }

        masm.Pop(loggerReg);
    }
#endif

    return true;
}

typedef bool (*CharCodeAtFn)(JSContext*, HandleString, int32_t, uint32_t*);
static const VMFunction CharCodeAtInfo =
    FunctionInfo<CharCodeAtFn>(jit::CharCodeAt, "CharCodeAt");

void
CodeGenerator::visitCharCodeAt(LCharCodeAt* lir)
{
    Register str = ToRegister(lir->str());
    Register index = ToRegister(lir->index());
    Register output = ToRegister(lir->output());
    Register temp = ToRegister(lir->temp());

    OutOfLineCode* ool = oolCallVM(CharCodeAtInfo, lir, ArgList(str, index), StoreRegisterTo(output));
    masm.loadStringChar(str, index, output, temp, ool->entry());
    masm.bind(ool->rejoin());
}

typedef JSFlatString* (*StringFromCharCodeFn)(JSContext*, int32_t);
static const VMFunction StringFromCharCodeInfo =
    FunctionInfo<StringFromCharCodeFn>(jit::StringFromCharCode, "StringFromCharCode");

void
CodeGenerator::visitFromCharCode(LFromCharCode* lir)
{
    Register code = ToRegister(lir->code());
    Register output = ToRegister(lir->output());

    OutOfLineCode* ool = oolCallVM(StringFromCharCodeInfo, lir, ArgList(code), StoreRegisterTo(output));

    // OOL path if code >= UNIT_STATIC_LIMIT.
    masm.boundsCheck32PowerOfTwo(code, StaticStrings::UNIT_STATIC_LIMIT, ool->entry());

    masm.movePtr(ImmPtr(&gen->runtime->staticStrings().unitStaticTable), output);
    masm.loadPtr(BaseIndex(output, code, ScalePointer), output);

    masm.bind(ool->rejoin());
}

typedef JSString* (*StringFromCodePointFn)(JSContext*, int32_t);
static const VMFunction StringFromCodePointInfo =
    FunctionInfo<StringFromCodePointFn>(jit::StringFromCodePoint, "StringFromCodePoint");

void
CodeGenerator::visitFromCodePoint(LFromCodePoint* lir)
{
    Register codePoint = ToRegister(lir->codePoint());
    Register output = ToRegister(lir->output());
    Register temp1 = ToRegister(lir->temp1());
    Register temp2 = ToRegister(lir->temp2());
    LSnapshot* snapshot = lir->snapshot();

    // The OOL path is only taken when we can't allocate the inline string.
    OutOfLineCode* ool = oolCallVM(StringFromCodePointInfo, lir, ArgList(codePoint),
                                   StoreRegisterTo(output));

    Label isTwoByte;
    Label* done = ool->rejoin();

    static_assert(StaticStrings::UNIT_STATIC_LIMIT -1 == JSString::MAX_LATIN1_CHAR,
                  "Latin-1 strings can be loaded from static strings");
    masm.boundsCheck32PowerOfTwo(codePoint, StaticStrings::UNIT_STATIC_LIMIT, &isTwoByte);
    {
        masm.movePtr(ImmPtr(&gen->runtime->staticStrings().unitStaticTable), output);
        masm.loadPtr(BaseIndex(output, codePoint, ScalePointer), output);
        masm.jump(done);
    }
    masm.bind(&isTwoByte);
    {
        // Use a bailout if the input is not a valid code point, because
        // MFromCodePoint is movable and it'd be observable when a moved
        // fromCodePoint throws an exception before its actual call site.
        bailoutCmp32(Assembler::Above, codePoint, Imm32(unicode::NonBMPMax), snapshot);

        // Allocate a JSThinInlineString.
        {
            static_assert(JSThinInlineString::MAX_LENGTH_TWO_BYTE >= 2,
                          "JSThinInlineString can hold a supplementary code point");

            uint32_t flags = JSString::INIT_THIN_INLINE_FLAGS;
            masm.newGCString(output, temp1, ool->entry(), gen->stringsCanBeInNursery());
            masm.store32(Imm32(flags), Address(output, JSString::offsetOfFlags()));
        }

        Label isSupplementary;
        masm.branch32(Assembler::AboveOrEqual, codePoint, Imm32(unicode::NonBMPMin),
                      &isSupplementary);
        {
            // Store length.
            masm.store32(Imm32(1), Address(output, JSString::offsetOfLength()));

            // Load chars pointer in temp1.
            masm.loadInlineStringCharsForStore(output, temp1);

            masm.store16(codePoint, Address(temp1, 0));

            // Null-terminate.
            masm.store16(Imm32(0), Address(temp1, sizeof(char16_t)));

            masm.jump(done);
        }
        masm.bind(&isSupplementary);
        {
            // Store length.
            masm.store32(Imm32(2), Address(output, JSString::offsetOfLength()));

            // Load chars pointer in temp1.
            masm.loadInlineStringCharsForStore(output, temp1);

            // Inlined unicode::LeadSurrogate(uint32_t).
            masm.move32(codePoint, temp2);
            masm.rshift32(Imm32(10), temp2);
            masm.add32(Imm32(unicode::LeadSurrogateMin - (unicode::NonBMPMin >> 10)), temp2);

            masm.store16(temp2, Address(temp1, 0));

            // Inlined unicode::TrailSurrogate(uint32_t).
            masm.move32(codePoint, temp2);
            masm.and32(Imm32(0x3FF), temp2);
            masm.or32(Imm32(unicode::TrailSurrogateMin), temp2);

            masm.store16(temp2, Address(temp1, sizeof(char16_t)));

            // Null-terminate.
            masm.store16(Imm32(0), Address(temp1, 2 * sizeof(char16_t)));
        }
    }

    masm.bind(done);
}

typedef JSString* (*StringToLowerCaseFn)(JSContext*, HandleString);
static const VMFunction StringToLowerCaseInfo =
    FunctionInfo<StringToLowerCaseFn>(js::StringToLowerCase, "StringToLowerCase");

typedef JSString* (*StringToUpperCaseFn)(JSContext*, HandleString);
static const VMFunction StringToUpperCaseInfo =
    FunctionInfo<StringToUpperCaseFn>(js::StringToUpperCase, "StringToUpperCase");

void
CodeGenerator::visitStringConvertCase(LStringConvertCase* lir)
{
    pushArg(ToRegister(lir->string()));

    if (lir->mir()->mode() == MStringConvertCase::LowerCase)
        callVM(StringToLowerCaseInfo, lir);
    else
        callVM(StringToUpperCaseInfo, lir);
}

void
CodeGenerator::visitSinCos(LSinCos *lir)
{
    Register temp = ToRegister(lir->temp());
    Register params = ToRegister(lir->temp2());
    FloatRegister input = ToFloatRegister(lir->input());
    FloatRegister outputSin = ToFloatRegister(lir->outputSin());
    FloatRegister outputCos = ToFloatRegister(lir->outputCos());

    masm.reserveStack(sizeof(double) * 2);
    masm.moveStackPtrTo(params);

    const MathCache* mathCache = lir->mir()->cache();

    masm.setupUnalignedABICall(temp);
    if (mathCache) {
        masm.movePtr(ImmPtr(mathCache), temp);
        masm.passABIArg(temp);
    }

#define MAYBE_CACHED_(fcn) (mathCache ? (void*)fcn ## _impl : (void*)fcn ## _uncached)

    masm.passABIArg(input, MoveOp::DOUBLE);
    masm.passABIArg(MoveOperand(params, sizeof(double), MoveOperand::EFFECTIVE_ADDRESS),
                                MoveOp::GENERAL);
    masm.passABIArg(MoveOperand(params, 0, MoveOperand::EFFECTIVE_ADDRESS),
                                MoveOp::GENERAL);

    masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, MAYBE_CACHED_(js::math_sincos)));
#undef MAYBE_CACHED_

    masm.loadDouble(Address(masm.getStackPointer(), 0), outputCos);
    masm.loadDouble(Address(masm.getStackPointer(), sizeof(double)), outputSin);
    masm.freeStack(sizeof(double) * 2);
}

typedef ArrayObject* (*StringSplitFn)(JSContext*, HandleObjectGroup, HandleString, HandleString, uint32_t);
static const VMFunction StringSplitInfo =
    FunctionInfo<StringSplitFn>(js::str_split_string, "str_split_string");

void
CodeGenerator::visitStringSplit(LStringSplit* lir)
{
    pushArg(Imm32(INT32_MAX));
    pushArg(ToRegister(lir->separator()));
    pushArg(ToRegister(lir->string()));
    pushArg(ImmGCPtr(lir->mir()->group()));

    callVM(StringSplitInfo, lir);
}

void
CodeGenerator::visitInitializedLength(LInitializedLength* lir)
{
    Address initLength(ToRegister(lir->elements()), ObjectElements::offsetOfInitializedLength());
    masm.load32(initLength, ToRegister(lir->output()));
}

void
CodeGenerator::visitSetInitializedLength(LSetInitializedLength* lir)
{
    Address initLength(ToRegister(lir->elements()), ObjectElements::offsetOfInitializedLength());
    SetLengthFromIndex(masm, lir->index(), initLength);
}

void
CodeGenerator::visitNotO(LNotO* lir)
{
    MOZ_ASSERT(lir->mir()->operandMightEmulateUndefined(),
               "This should be constant-folded if the object can't emulate undefined.");

    OutOfLineTestObjectWithLabels* ool = new(alloc()) OutOfLineTestObjectWithLabels();
    addOutOfLineCode(ool, lir->mir());

    Label* ifEmulatesUndefined = ool->label1();
    Label* ifDoesntEmulateUndefined = ool->label2();

    Register objreg = ToRegister(lir->input());
    Register output = ToRegister(lir->output());
    branchTestObjectEmulatesUndefined(objreg, ifEmulatesUndefined, ifDoesntEmulateUndefined,
                                      output, ool);
    // fall through

    Label join;

    masm.move32(Imm32(0), output);
    masm.jump(&join);

    masm.bind(ifEmulatesUndefined);
    masm.move32(Imm32(1), output);

    masm.bind(&join);
}

void
CodeGenerator::visitNotV(LNotV* lir)
{
    Maybe<Label> ifTruthyLabel, ifFalsyLabel;
    Label* ifTruthy;
    Label* ifFalsy;

    OutOfLineTestObjectWithLabels* ool = nullptr;
    MDefinition* operand = lir->mir()->input();
    // Unfortunately, it's possible that someone (e.g. phi elimination) switched
    // out our operand after we did cacheOperandMightEmulateUndefined.  So we
    // might think it can emulate undefined _and_ know that it can't be an
    // object.
    if (lir->mir()->operandMightEmulateUndefined() && operand->mightBeType(MIRType::Object)) {
        ool = new(alloc()) OutOfLineTestObjectWithLabels();
        addOutOfLineCode(ool, lir->mir());
        ifTruthy = ool->label1();
        ifFalsy = ool->label2();
    } else {
        ifTruthyLabel.emplace();
        ifFalsyLabel.emplace();
        ifTruthy = ifTruthyLabel.ptr();
        ifFalsy = ifFalsyLabel.ptr();
    }

    testValueTruthyKernel(ToValue(lir, LNotV::Input), lir->temp1(), lir->temp2(),
                          ToFloatRegister(lir->tempFloat()),
                          ifTruthy, ifFalsy, ool, operand);

    Label join;
    Register output = ToRegister(lir->output());

    // Note that the testValueTruthyKernel call above may choose to fall through
    // to ifTruthy instead of branching there.
    masm.bind(ifTruthy);
    masm.move32(Imm32(0), output);
    masm.jump(&join);

    masm.bind(ifFalsy);
    masm.move32(Imm32(1), output);

    // both branches meet here.
    masm.bind(&join);
}

void
CodeGenerator::visitBoundsCheck(LBoundsCheck* lir)
{
    const LAllocation* index = lir->index();
    const LAllocation* length = lir->length();
    LSnapshot* snapshot = lir->snapshot();

    if (index->isConstant()) {
        // Use uint32 so that the comparison is unsigned.
        uint32_t idx = ToInt32(index);
        if (length->isConstant()) {
            uint32_t len = ToInt32(lir->length());
            if (idx < len)
                return;
            bailout(snapshot);
            return;
        }

        if (length->isRegister())
            bailoutCmp32(Assembler::BelowOrEqual, ToRegister(length), Imm32(idx), snapshot);
        else
            bailoutCmp32(Assembler::BelowOrEqual, ToAddress(length), Imm32(idx), snapshot);
        return;
    }

    Register indexReg = ToRegister(index);
    if (length->isConstant())
        bailoutCmp32(Assembler::AboveOrEqual, indexReg, Imm32(ToInt32(length)), snapshot);
    else if (length->isRegister())
        bailoutCmp32(Assembler::BelowOrEqual, ToRegister(length), indexReg, snapshot);
    else
        bailoutCmp32(Assembler::BelowOrEqual, ToAddress(length), indexReg, snapshot);
}

void
CodeGenerator::visitBoundsCheckRange(LBoundsCheckRange* lir)
{
    int32_t min = lir->mir()->minimum();
    int32_t max = lir->mir()->maximum();
    MOZ_ASSERT(max >= min);

    const LAllocation* length = lir->length();
    LSnapshot* snapshot = lir->snapshot();
    Register temp = ToRegister(lir->getTemp(0));
    if (lir->index()->isConstant()) {
        int32_t nmin, nmax;
        int32_t index = ToInt32(lir->index());
        if (SafeAdd(index, min, &nmin) && SafeAdd(index, max, &nmax) && nmin >= 0) {
            if (length->isRegister())
                bailoutCmp32(Assembler::BelowOrEqual, ToRegister(length), Imm32(nmax), snapshot);
            else
                bailoutCmp32(Assembler::BelowOrEqual, ToAddress(length), Imm32(nmax), snapshot);
            return;
        }
        masm.mov(ImmWord(index), temp);
    } else {
        masm.mov(ToRegister(lir->index()), temp);
    }

    // If the minimum and maximum differ then do an underflow check first.
    // If the two are the same then doing an unsigned comparison on the
    // length will also catch a negative index.
    if (min != max) {
        if (min != 0) {
            Label bail;
            masm.branchAdd32(Assembler::Overflow, Imm32(min), temp, &bail);
            bailoutFrom(&bail, snapshot);
        }

        bailoutCmp32(Assembler::LessThan, temp, Imm32(0), snapshot);

        if (min != 0) {
            int32_t diff;
            if (SafeSub(max, min, &diff))
                max = diff;
            else
                masm.sub32(Imm32(min), temp);
        }
    }

    // Compute the maximum possible index. No overflow check is needed when
    // max > 0. We can only wraparound to a negative number, which will test as
    // larger than all nonnegative numbers in the unsigned comparison, and the
    // length is required to be nonnegative (else testing a negative length
    // would succeed on any nonnegative index).
    if (max != 0) {
        if (max < 0) {
            Label bail;
            masm.branchAdd32(Assembler::Overflow, Imm32(max), temp, &bail);
            bailoutFrom(&bail, snapshot);
        } else {
            masm.add32(Imm32(max), temp);
        }
    }

    if (length->isRegister())
        bailoutCmp32(Assembler::BelowOrEqual, ToRegister(length), temp, snapshot);
    else
        bailoutCmp32(Assembler::BelowOrEqual, ToAddress(length), temp, snapshot);
}

void
CodeGenerator::visitBoundsCheckLower(LBoundsCheckLower* lir)
{
    int32_t min = lir->mir()->minimum();
    bailoutCmp32(Assembler::LessThan, ToRegister(lir->index()), Imm32(min),
                 lir->snapshot());
}

void
CodeGenerator::visitSpectreMaskIndex(LSpectreMaskIndex* lir)
{
    MOZ_ASSERT(JitOptions.spectreIndexMasking);

    const LAllocation* length = lir->length();
    Register index = ToRegister(lir->index());
    Register output = ToRegister(lir->output());

    if (length->isRegister())
        masm.spectreMaskIndex(index, ToRegister(length), output);
    else
        masm.spectreMaskIndex(index, ToAddress(length), output);
}

class OutOfLineStoreElementHole : public OutOfLineCodeBase<CodeGenerator>
{
    LInstruction* ins_;
    Label rejoinStore_;
    bool strict_;

  public:
    explicit OutOfLineStoreElementHole(LInstruction* ins, bool strict)
      : ins_(ins), strict_(strict)
    {
        MOZ_ASSERT(ins->isStoreElementHoleV() || ins->isStoreElementHoleT() ||
                   ins->isFallibleStoreElementV() || ins->isFallibleStoreElementT());
    }

    void accept(CodeGenerator* codegen) override {
        codegen->visitOutOfLineStoreElementHole(this);
    }
    LInstruction* ins() const {
        return ins_;
    }
    Label* rejoinStore() {
        return &rejoinStore_;
    }
    bool strict() const {
        return strict_;
    }
};

void
CodeGenerator::emitStoreHoleCheck(Register elements, const LAllocation* index,
                                  int32_t offsetAdjustment, LSnapshot* snapshot)
{
    Label bail;
    if (index->isConstant()) {
        Address dest(elements, ToInt32(index) * sizeof(js::Value) + offsetAdjustment);
        masm.branchTestMagic(Assembler::Equal, dest, &bail);
    } else {
        BaseIndex dest(elements, ToRegister(index), TimesEight, offsetAdjustment);
        masm.branchTestMagic(Assembler::Equal, dest, &bail);
    }
    bailoutFrom(&bail, snapshot);
}

static ConstantOrRegister
ToConstantOrRegister(const LAllocation* value, MIRType valueType)
{
    if (value->isConstant())
        return ConstantOrRegister(value->toConstant()->toJSValue());
    return TypedOrValueRegister(valueType, ToAnyRegister(value));
}

void
CodeGenerator::emitStoreElementTyped(const LAllocation* value,
                                     MIRType valueType, MIRType elementType,
                                     Register elements, const LAllocation* index,
                                     int32_t offsetAdjustment)
{
    ConstantOrRegister v = ToConstantOrRegister(value, valueType);
    if (index->isConstant()) {
        Address dest(elements, ToInt32(index) * sizeof(js::Value) + offsetAdjustment);
        masm.storeUnboxedValue(v, valueType, dest, elementType);
    } else {
        BaseIndex dest(elements, ToRegister(index), TimesEight, offsetAdjustment);
        masm.storeUnboxedValue(v, valueType, dest, elementType);
    }
}

void
CodeGenerator::visitStoreElementT(LStoreElementT* store)
{
    Register elements = ToRegister(store->elements());
    const LAllocation* index = store->index();

    if (store->mir()->needsBarrier())
        emitPreBarrier(elements, index, store->mir()->offsetAdjustment());

    if (store->mir()->needsHoleCheck())
        emitStoreHoleCheck(elements, index, store->mir()->offsetAdjustment(), store->snapshot());

    emitStoreElementTyped(store->value(),
                          store->mir()->value()->type(), store->mir()->elementType(),
                          elements, index, store->mir()->offsetAdjustment());
}

void
CodeGenerator::visitStoreElementV(LStoreElementV* lir)
{
    const ValueOperand value = ToValue(lir, LStoreElementV::Value);
    Register elements = ToRegister(lir->elements());
    const LAllocation* index = lir->index();

    if (lir->mir()->needsBarrier())
        emitPreBarrier(elements, index, lir->mir()->offsetAdjustment());

    if (lir->mir()->needsHoleCheck())
        emitStoreHoleCheck(elements, index, lir->mir()->offsetAdjustment(), lir->snapshot());

    if (lir->index()->isConstant()) {
        Address dest(elements,
                     ToInt32(lir->index()) * sizeof(js::Value) + lir->mir()->offsetAdjustment());
        masm.storeValue(value, dest);
    } else {
        BaseIndex dest(elements, ToRegister(lir->index()), TimesEight,
                       lir->mir()->offsetAdjustment());
        masm.storeValue(value, dest);
    }
}

template <typename T> void
CodeGenerator::emitStoreElementHoleT(T* lir)
{
    static_assert(std::is_same<T, LStoreElementHoleT>::value || std::is_same<T, LFallibleStoreElementT>::value,
                  "emitStoreElementHoleT called with unexpected argument type");

    OutOfLineStoreElementHole* ool =
        new(alloc()) OutOfLineStoreElementHole(lir, current->mir()->strict());
    addOutOfLineCode(ool, lir->mir());

    Register elements = ToRegister(lir->elements());
    Register index = ToRegister(lir->index());
    Register spectreTemp = ToTempRegisterOrInvalid(lir->spectreTemp());

    Address initLength(elements, ObjectElements::offsetOfInitializedLength());
    masm.spectreBoundsCheck32(index, initLength, spectreTemp, ool->entry());

    if (lir->mir()->needsBarrier())
        emitPreBarrier(elements, lir->index(), 0);

    masm.bind(ool->rejoinStore());
    emitStoreElementTyped(lir->value(), lir->mir()->value()->type(), lir->mir()->elementType(),
                          elements, lir->index(), 0);

    masm.bind(ool->rejoin());
}

void
CodeGenerator::visitStoreElementHoleT(LStoreElementHoleT* lir)
{
    emitStoreElementHoleT(lir);
}

template <typename T> void
CodeGenerator::emitStoreElementHoleV(T* lir)
{
    static_assert(std::is_same<T, LStoreElementHoleV>::value || std::is_same<T, LFallibleStoreElementV>::value,
                  "emitStoreElementHoleV called with unexpected parameter type");

    OutOfLineStoreElementHole* ool =
        new(alloc()) OutOfLineStoreElementHole(lir, current->mir()->strict());
    addOutOfLineCode(ool, lir->mir());

    Register elements = ToRegister(lir->elements());
    Register index = ToRegister(lir->index());
    const ValueOperand value = ToValue(lir, T::Value);
    Register spectreTemp = ToTempRegisterOrInvalid(lir->spectreTemp());

    Address initLength(elements, ObjectElements::offsetOfInitializedLength());
    masm.spectreBoundsCheck32(index, initLength, spectreTemp, ool->entry());

    if (lir->mir()->needsBarrier())
        emitPreBarrier(elements, lir->index(), 0);

    masm.bind(ool->rejoinStore());
    masm.storeValue(value, BaseIndex(elements, index, TimesEight));

    masm.bind(ool->rejoin());
}

void
CodeGenerator::visitStoreElementHoleV(LStoreElementHoleV* lir)
{
    emitStoreElementHoleV(lir);
}

typedef bool (*ThrowReadOnlyFn)(JSContext*, HandleObject, int32_t);
static const VMFunction ThrowReadOnlyInfo =
    FunctionInfo<ThrowReadOnlyFn>(ThrowReadOnlyError, "ThrowReadOnlyError");

void
CodeGenerator::visitFallibleStoreElementT(LFallibleStoreElementT* lir)
{
    Register elements = ToRegister(lir->elements());

    // Handle frozen objects
    Label isFrozen;
    Address flags(elements, ObjectElements::offsetOfFlags());
    if (!lir->mir()->strict()) {
        masm.branchTest32(Assembler::NonZero, flags, Imm32(ObjectElements::FROZEN), &isFrozen);
    } else {
        Register object = ToRegister(lir->object());
        const LAllocation* index = lir->index();
        OutOfLineCode* ool;
        if (index->isConstant()) {
            ool = oolCallVM(ThrowReadOnlyInfo, lir,
                            ArgList(object, Imm32(ToInt32(index))), StoreNothing());
        } else {
            ool = oolCallVM(ThrowReadOnlyInfo, lir,
                            ArgList(object, ToRegister(index)), StoreNothing());
        }
        masm.branchTest32(Assembler::NonZero, flags, Imm32(ObjectElements::FROZEN), ool->entry());
        // This OOL code should have thrown an exception, so will never return.
        // So, do not bind ool->rejoin() anywhere, so that it implicitly (and without the cost
        // of a jump) does a masm.assumeUnreachable().
    }

    emitStoreElementHoleT(lir);

    masm.bind(&isFrozen);
}

void
CodeGenerator::visitFallibleStoreElementV(LFallibleStoreElementV* lir)
{
    Register elements = ToRegister(lir->elements());

    // Handle frozen objects
    Label isFrozen;
    Address flags(elements, ObjectElements::offsetOfFlags());
    if (!lir->mir()->strict()) {
        masm.branchTest32(Assembler::NonZero, flags, Imm32(ObjectElements::FROZEN), &isFrozen);
    } else {
        Register object = ToRegister(lir->object());
        const LAllocation* index = lir->index();
        OutOfLineCode* ool;
        if (index->isConstant()) {
            ool = oolCallVM(ThrowReadOnlyInfo, lir,
                            ArgList(object, Imm32(ToInt32(index))), StoreNothing());
        } else {
            ool = oolCallVM(ThrowReadOnlyInfo, lir,
                            ArgList(object, ToRegister(index)), StoreNothing());
        }
        masm.branchTest32(Assembler::NonZero, flags, Imm32(ObjectElements::FROZEN), ool->entry());
        // This OOL code should have thrown an exception, so will never return.
        // So, do not bind ool->rejoin() anywhere, so that it implicitly (and without the cost
        // of a jump) does a masm.assumeUnreachable().
    }

    emitStoreElementHoleV(lir);

    masm.bind(&isFrozen);
}

typedef bool (*SetDenseElementFn)(JSContext*, HandleNativeObject, int32_t, HandleValue,
                                  bool strict);
static const VMFunction SetDenseElementInfo =
    FunctionInfo<SetDenseElementFn>(jit::SetDenseElement, "SetDenseElement");

void
CodeGenerator::visitOutOfLineStoreElementHole(OutOfLineStoreElementHole* ool)
{
    Register object, elements;
    LInstruction* ins = ool->ins();
    const LAllocation* index;
    MIRType valueType;
    ConstantOrRegister value;
    Register spectreTemp;

    if (ins->isStoreElementHoleV()) {
        LStoreElementHoleV* store = ins->toStoreElementHoleV();
        object = ToRegister(store->object());
        elements = ToRegister(store->elements());
        index = store->index();
        valueType = store->mir()->value()->type();
        value = TypedOrValueRegister(ToValue(store, LStoreElementHoleV::Value));
        spectreTemp = ToTempRegisterOrInvalid(store->spectreTemp());
    } else if (ins->isFallibleStoreElementV()) {
        LFallibleStoreElementV* store = ins->toFallibleStoreElementV();
        object = ToRegister(store->object());
        elements = ToRegister(store->elements());
        index = store->index();
        valueType = store->mir()->value()->type();
        value = TypedOrValueRegister(ToValue(store, LFallibleStoreElementV::Value));
        spectreTemp = ToTempRegisterOrInvalid(store->spectreTemp());
    } else if (ins->isStoreElementHoleT()) {
        LStoreElementHoleT* store = ins->toStoreElementHoleT();
        object = ToRegister(store->object());
        elements = ToRegister(store->elements());
        index = store->index();
        valueType = store->mir()->value()->type();
        if (store->value()->isConstant())
            value = ConstantOrRegister(store->value()->toConstant()->toJSValue());
        else
            value = TypedOrValueRegister(valueType, ToAnyRegister(store->value()));
        spectreTemp = ToTempRegisterOrInvalid(store->spectreTemp());
    } else { // ins->isFallibleStoreElementT()
        LFallibleStoreElementT* store = ins->toFallibleStoreElementT();
        object = ToRegister(store->object());
        elements = ToRegister(store->elements());
        index = store->index();
        valueType = store->mir()->value()->type();
        if (store->value()->isConstant())
            value = ConstantOrRegister(store->value()->toConstant()->toJSValue());
        else
            value = TypedOrValueRegister(valueType, ToAnyRegister(store->value()));
        spectreTemp = ToTempRegisterOrInvalid(store->spectreTemp());
    }

    Register indexReg = ToRegister(index);

    // If index == initializedLength, try to bump the initialized length inline.
    // If index > initializedLength, call a stub. Note that this relies on the
    // condition flags sticking from the incoming branch.
    // Also note: this branch does not need Spectre mitigations, doing that for
    // the capacity check below is sufficient.
    Label callStub;
#if defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
    // Had to reimplement for MIPS because there are no flags.
    Address initLength(elements, ObjectElements::offsetOfInitializedLength());
    masm.branch32(Assembler::NotEqual, initLength, indexReg, &callStub);
#else
    masm.j(Assembler::NotEqual, &callStub);
#endif

    // Check array capacity.
    masm.spectreBoundsCheck32(indexReg, Address(elements, ObjectElements::offsetOfCapacity()),
                              spectreTemp, &callStub);

    // Update initialized length. The capacity guard above ensures this won't overflow,
    // due to MAX_DENSE_ELEMENTS_COUNT.
    masm.add32(Imm32(1), indexReg);
    masm.store32(indexReg, Address(elements, ObjectElements::offsetOfInitializedLength()));

    // Update length if length < initializedLength.
    Label dontUpdate;
    masm.branch32(Assembler::AboveOrEqual, Address(elements, ObjectElements::offsetOfLength()),
                  indexReg, &dontUpdate);
    masm.store32(indexReg, Address(elements, ObjectElements::offsetOfLength()));
    masm.bind(&dontUpdate);

    masm.sub32(Imm32(1), indexReg);

    if ((ins->isStoreElementHoleT() || ins->isFallibleStoreElementT()) &&
        valueType != MIRType::Double)
    {
        // The inline path for StoreElementHoleT and FallibleStoreElementT does not always store
        // the type tag, so we do the store on the OOL path. We use MIRType::None for the element
        // type so that storeElementTyped will always store the type tag.
        if (ins->isStoreElementHoleT()) {
            emitStoreElementTyped(ins->toStoreElementHoleT()->value(), valueType, MIRType::None,
                                  elements, index, 0);
            masm.jump(ool->rejoin());
        } else if (ins->isFallibleStoreElementT()) {
            emitStoreElementTyped(ins->toFallibleStoreElementT()->value(), valueType,
                                  MIRType::None, elements, index, 0);
            masm.jump(ool->rejoin());
        }
    } else {
        // Jump to the inline path where we will store the value.
        masm.jump(ool->rejoinStore());
    }

    masm.bind(&callStub);
    saveLive(ins);

    pushArg(Imm32(ool->strict()));
    pushArg(value);
    if (index->isConstant())
        pushArg(Imm32(ToInt32(index)));
    else
        pushArg(ToRegister(index));
    pushArg(object);
    callVM(SetDenseElementInfo, ins);

    restoreLive(ins);
    masm.jump(ool->rejoin());
}

template <typename T>
static void
StoreUnboxedPointer(MacroAssembler& masm, T address, MIRType type, const LAllocation* value,
                    bool preBarrier)
{
    if (preBarrier)
        masm.guardedCallPreBarrier(address, type);
    if (value->isConstant()) {
        Value v = value->toConstant()->toJSValue();
        if (v.isGCThing()) {
            masm.storePtr(ImmGCPtr(v.toGCThing()), address);
        } else {
            MOZ_ASSERT(v.isNull());
            masm.storePtr(ImmWord(0), address);
        }
    } else {
        masm.storePtr(ToRegister(value), address);
    }
}

void
CodeGenerator::visitStoreUnboxedPointer(LStoreUnboxedPointer* lir)
{
    MIRType type;
    int32_t offsetAdjustment;
    bool preBarrier;
    if (lir->mir()->isStoreUnboxedObjectOrNull()) {
        type = MIRType::Object;
        offsetAdjustment = lir->mir()->toStoreUnboxedObjectOrNull()->offsetAdjustment();
        preBarrier = lir->mir()->toStoreUnboxedObjectOrNull()->preBarrier();
    } else if (lir->mir()->isStoreUnboxedString()) {
        type = MIRType::String;
        offsetAdjustment = lir->mir()->toStoreUnboxedString()->offsetAdjustment();
        preBarrier = lir->mir()->toStoreUnboxedString()->preBarrier();
    } else {
        MOZ_CRASH();
    }

    Register elements = ToRegister(lir->elements());
    const LAllocation* index = lir->index();
    const LAllocation* value = lir->value();

    if (index->isConstant()) {
        Address address(elements, ToInt32(index) * sizeof(uintptr_t) + offsetAdjustment);
        StoreUnboxedPointer(masm, address, type, value, preBarrier);
    } else {
        BaseIndex address(elements, ToRegister(index), ScalePointer, offsetAdjustment);
        StoreUnboxedPointer(masm, address, type, value, preBarrier);
    }
}

typedef NativeObject* (*ConvertUnboxedObjectToNativeFn)(JSContext*, JSObject*);
static const VMFunction ConvertUnboxedPlainObjectToNativeInfo =
    FunctionInfo<ConvertUnboxedObjectToNativeFn>(UnboxedPlainObject::convertToNative,
                                                 "UnboxedPlainObject::convertToNative");

void
CodeGenerator::visitConvertUnboxedObjectToNative(LConvertUnboxedObjectToNative* lir)
{
    Register object = ToRegister(lir->getOperand(0));
    Register temp = ToTempRegisterOrInvalid(lir->temp());

    // The call will return the same object so StoreRegisterTo(object) is safe.
    OutOfLineCode* ool = oolCallVM(ConvertUnboxedPlainObjectToNativeInfo,
                                   lir, ArgList(object), StoreRegisterTo(object));

    masm.branchTestObjGroup(Assembler::Equal, object, lir->mir()->group(), temp, object,
                            ool->entry());
    masm.bind(ool->rejoin());
}

typedef bool (*ArrayPopShiftFn)(JSContext*, HandleObject, MutableHandleValue);
static const VMFunction ArrayPopDenseInfo =
    FunctionInfo<ArrayPopShiftFn>(jit::ArrayPopDense, "ArrayPopDense");
static const VMFunction ArrayShiftDenseInfo =
    FunctionInfo<ArrayPopShiftFn>(jit::ArrayShiftDense, "ArrayShiftDense");

void
CodeGenerator::emitArrayPopShift(LInstruction* lir, const MArrayPopShift* mir, Register obj,
                                 Register elementsTemp, Register lengthTemp, TypedOrValueRegister out)
{
    OutOfLineCode* ool;

    if (mir->mode() == MArrayPopShift::Pop) {
        ool = oolCallVM(ArrayPopDenseInfo, lir, ArgList(obj), StoreValueTo(out));
    } else {
        MOZ_ASSERT(mir->mode() == MArrayPopShift::Shift);
        ool = oolCallVM(ArrayShiftDenseInfo, lir, ArgList(obj), StoreValueTo(out));
    }

    // VM call if a write barrier is necessary.
    masm.branchTestNeedsIncrementalBarrier(Assembler::NonZero, ool->entry());

    // Load elements and initializedLength, and VM call if
    // length != initializedLength.
    masm.loadPtr(Address(obj, NativeObject::offsetOfElements()), elementsTemp);
    masm.load32(Address(elementsTemp, ObjectElements::offsetOfInitializedLength()), lengthTemp);

    Address lengthAddr(elementsTemp, ObjectElements::offsetOfLength());
    masm.branch32(Assembler::NotEqual, lengthAddr, lengthTemp, ool->entry());

    // Test for length != 0. On zero length either take a VM call or generate
    // an undefined value, depending on whether the call is known to produce
    // undefined.
    Label done;
    if (mir->maybeUndefined()) {
        Label notEmpty;
        masm.branchTest32(Assembler::NonZero, lengthTemp, lengthTemp, &notEmpty);

        // According to the spec we need to set the length 0 (which is already 0).
        // This is observable when the array length is made non-writable.
        // Handle this case in the OOL.
        Address elementFlags(elementsTemp, ObjectElements::offsetOfFlags());
        Imm32 bit(ObjectElements::NONWRITABLE_ARRAY_LENGTH);
        masm.branchTest32(Assembler::NonZero, elementFlags, bit, ool->entry());

        masm.moveValue(UndefinedValue(), out.valueReg());
        masm.jump(&done);
        masm.bind(&notEmpty);
    } else {
        masm.branchTest32(Assembler::Zero, lengthTemp, lengthTemp, ool->entry());
    }

    masm.sub32(Imm32(1), lengthTemp);

    if (mir->mode() == MArrayPopShift::Pop) {
        BaseIndex addr(elementsTemp, lengthTemp, TimesEight);
        masm.loadElementTypedOrValue(addr, out, mir->needsHoleCheck(), ool->entry());
    } else {
        MOZ_ASSERT(mir->mode() == MArrayPopShift::Shift);
        Address addr(elementsTemp, 0);
        masm.loadElementTypedOrValue(addr, out, mir->needsHoleCheck(), ool->entry());
    }

    // Handle the failure case when the array length is non-writable in the
    // OOL path.  (Unlike in the adding-an-element cases, we can't rely on the
    // capacity <= length invariant for such arrays to avoid an explicit
    // check.)
    Address elementFlags(elementsTemp, ObjectElements::offsetOfFlags());
    Imm32 bit(ObjectElements::NONWRITABLE_ARRAY_LENGTH);
    masm.branchTest32(Assembler::NonZero, elementFlags, bit, ool->entry());

    if (mir->mode() == MArrayPopShift::Shift) {
        // Don't save the elementsTemp register.
        LiveRegisterSet temps;
        temps.add(elementsTemp);

        saveVolatile(temps);
        masm.setupUnalignedABICall(elementsTemp);
        masm.passABIArg(obj);
        masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, js::ArrayShiftMoveElements));
        restoreVolatile(temps);

        // Reload elementsTemp as ArrayShiftMoveElements may have moved it.
        masm.loadPtr(Address(obj, NativeObject::offsetOfElements()), elementsTemp);
    }

    // Now adjust length and initializedLength.
    masm.store32(lengthTemp, Address(elementsTemp, ObjectElements::offsetOfLength()));
    masm.store32(lengthTemp, Address(elementsTemp, ObjectElements::offsetOfInitializedLength()));

    masm.bind(&done);
    masm.bind(ool->rejoin());
}

void
CodeGenerator::visitArrayPopShiftV(LArrayPopShiftV* lir)
{
    Register obj = ToRegister(lir->object());
    Register elements = ToRegister(lir->temp0());
    Register length = ToRegister(lir->temp1());
    TypedOrValueRegister out(ToOutValue(lir));
    emitArrayPopShift(lir, lir->mir(), obj, elements, length, out);
}

void
CodeGenerator::visitArrayPopShiftT(LArrayPopShiftT* lir)
{
    Register obj = ToRegister(lir->object());
    Register elements = ToRegister(lir->temp0());
    Register length = ToRegister(lir->temp1());
    TypedOrValueRegister out(lir->mir()->type(), ToAnyRegister(lir->output()));
    emitArrayPopShift(lir, lir->mir(), obj, elements, length, out);
}

typedef bool (*ArrayPushDenseFn)(JSContext*, HandleArrayObject, HandleValue, uint32_t*);
static const VMFunction ArrayPushDenseInfo =
    FunctionInfo<ArrayPushDenseFn>(jit::ArrayPushDense, "ArrayPushDense");

void
CodeGenerator::emitArrayPush(LInstruction* lir, Register obj,
                             const ConstantOrRegister& value, Register elementsTemp, Register length,
                             Register spectreTemp)
{
    OutOfLineCode* ool = oolCallVM(ArrayPushDenseInfo, lir, ArgList(obj, value), StoreRegisterTo(length));

    // Load elements and length.
    masm.loadPtr(Address(obj, NativeObject::offsetOfElements()), elementsTemp);
    masm.load32(Address(elementsTemp, ObjectElements::offsetOfLength()), length);

    // Guard length == initializedLength.
    Address initLength(elementsTemp, ObjectElements::offsetOfInitializedLength());
    masm.branch32(Assembler::NotEqual, initLength, length, ool->entry());

    // Guard length < capacity.
    Address capacity(elementsTemp, ObjectElements::offsetOfCapacity());
    masm.spectreBoundsCheck32(length, capacity, spectreTemp, ool->entry());

    // Do the store.
    masm.storeConstantOrRegister(value, BaseIndex(elementsTemp, length, TimesEight));

    masm.add32(Imm32(1), length);

    // Update length and initialized length.
    masm.store32(length, Address(elementsTemp, ObjectElements::offsetOfLength()));
    masm.store32(length, Address(elementsTemp, ObjectElements::offsetOfInitializedLength()));

    masm.bind(ool->rejoin());
}

void
CodeGenerator::visitArrayPushV(LArrayPushV* lir)
{
    Register obj = ToRegister(lir->object());
    Register elementsTemp = ToRegister(lir->temp());
    Register length = ToRegister(lir->output());
    ConstantOrRegister value = TypedOrValueRegister(ToValue(lir, LArrayPushV::Value));
    Register spectreTemp = ToTempRegisterOrInvalid(lir->spectreTemp());
    emitArrayPush(lir, obj, value, elementsTemp, length, spectreTemp);
}

void
CodeGenerator::visitArrayPushT(LArrayPushT* lir)
{
    Register obj = ToRegister(lir->object());
    Register elementsTemp = ToRegister(lir->temp());
    Register length = ToRegister(lir->output());
    ConstantOrRegister value;
    if (lir->value()->isConstant())
        value = ConstantOrRegister(lir->value()->toConstant()->toJSValue());
    else
        value = TypedOrValueRegister(lir->mir()->value()->type(), ToAnyRegister(lir->value()));
    Register spectreTemp = ToTempRegisterOrInvalid(lir->spectreTemp());
    emitArrayPush(lir, obj, value, elementsTemp, length, spectreTemp);
}

typedef JSObject* (*ArraySliceDenseFn)(JSContext*, HandleObject, int32_t, int32_t, HandleObject);
static const VMFunction ArraySliceDenseInfo =
    FunctionInfo<ArraySliceDenseFn>(array_slice_dense, "array_slice_dense");

void
CodeGenerator::visitArraySlice(LArraySlice* lir)
{
    Register object = ToRegister(lir->object());
    Register begin = ToRegister(lir->begin());
    Register end = ToRegister(lir->end());
    Register temp1 = ToRegister(lir->temp1());
    Register temp2 = ToRegister(lir->temp2());

    Label call, fail;

    // Try to allocate an object.
    masm.createGCObject(temp1, temp2, lir->mir()->templateObj(), lir->mir()->initialHeap(), &fail);

    // Fixup the group of the result in case it doesn't match the template object.
    masm.copyObjGroupNoPreBarrier(object, temp1, temp2);

    masm.jump(&call);
    {
        masm.bind(&fail);
        masm.movePtr(ImmPtr(nullptr), temp1);
    }
    masm.bind(&call);

    pushArg(temp1);
    pushArg(end);
    pushArg(begin);
    pushArg(object);
    callVM(ArraySliceDenseInfo, lir);
}

typedef JSString* (*ArrayJoinFn)(JSContext*, HandleObject, HandleString);
static const VMFunction ArrayJoinInfo = FunctionInfo<ArrayJoinFn>(jit::ArrayJoin, "ArrayJoin");

void
CodeGenerator::visitArrayJoin(LArrayJoin* lir)
{
    Label skipCall;

    Register output = ToRegister(lir->output());
    Register sep = ToRegister(lir->separator());
    Register array = ToRegister(lir->array());
    if (lir->mir()->optimizeForArray()) {
        Register temp = ToRegister(lir->temp());

        masm.loadPtr(Address(array, NativeObject::offsetOfElements()), temp);
        Address length(temp, ObjectElements::offsetOfLength());
        Address initLength(temp, ObjectElements::offsetOfInitializedLength());

        // Check for length == 0
        Label notEmpty;
        masm.branch32(Assembler::NotEqual, length, Imm32(0), &notEmpty);
        const JSAtomState& names = GetJitContext()->runtime->names();
        masm.movePtr(ImmGCPtr(names.empty), output);
        masm.jump(&skipCall);

        masm.bind(&notEmpty);
        Label notSingleString;
        // Check for length == 1, initializedLength >= 1, arr[0].isString()
        masm.branch32(Assembler::NotEqual, length, Imm32(1), &notSingleString);
        masm.branch32(Assembler::LessThan, initLength, Imm32(1), &notSingleString);

        Address elem0(temp, 0);
        masm.branchTestString(Assembler::NotEqual, elem0, &notSingleString);

        // At this point, 'output' can be used as a scratch register, since we're
        // guaranteed to succeed.
        masm.unboxString(elem0, output);
        masm.jump(&skipCall);
        masm.bind(&notSingleString);
    }

    pushArg(sep);
    pushArg(array);
    callVM(ArrayJoinInfo, lir);
    masm.bind(&skipCall);
}

void
CodeGenerator::visitGetIteratorCache(LGetIteratorCache* lir)
{
    LiveRegisterSet liveRegs = lir->safepoint()->liveRegs();
    TypedOrValueRegister val =
        toConstantOrRegister(lir, LGetIteratorCache::Value, lir->mir()->value()->type()).reg();
    Register output = ToRegister(lir->output());
    Register temp1 = ToRegister(lir->temp1());
    Register temp2 = ToRegister(lir->temp2());

    IonGetIteratorIC ic(liveRegs, val, output, temp1, temp2);
    addIC(lir, allocateIC(ic));
}

static void
LoadNativeIterator(MacroAssembler& masm, Register obj, Register dest, Label* failures)
{
    MOZ_ASSERT(obj != dest);

    // Test class.
    masm.branchTestObjClass(Assembler::NotEqual, obj, &PropertyIteratorObject::class_, dest,
                            obj, failures);

    // Load NativeIterator object.
    masm.loadObjPrivate(obj, JSObject::ITER_CLASS_NFIXED_SLOTS, dest);
}

typedef bool (*IteratorMoreFn)(JSContext*, HandleObject, MutableHandleValue);
static const VMFunction IteratorMoreInfo =
    FunctionInfo<IteratorMoreFn>(IteratorMore, "IteratorMore");

void
CodeGenerator::visitIteratorMore(LIteratorMore* lir)
{
    const Register obj = ToRegister(lir->object());
    const ValueOperand output = ToOutValue(lir);
    const Register temp = ToRegister(lir->temp());

    OutOfLineCode* ool = oolCallVM(IteratorMoreInfo, lir, ArgList(obj), StoreValueTo(output));

    Register outputScratch = output.scratchReg();
    LoadNativeIterator(masm, obj, outputScratch, ool->entry());

    // If props_cursor < props_end, load the next string and advance the cursor.
    // Else, return MagicValue(JS_NO_ITER_VALUE).
    Label iterDone;
    Address cursorAddr(outputScratch, offsetof(NativeIterator, props_cursor));
    Address cursorEndAddr(outputScratch, offsetof(NativeIterator, props_end));
    masm.loadPtr(cursorAddr, temp);
    masm.branchPtr(Assembler::BelowOrEqual, cursorEndAddr, temp, &iterDone);

    // Get next string.
    masm.loadPtr(Address(temp, 0), temp);

    // Increase the cursor.
    masm.addPtr(Imm32(sizeof(JSString*)), cursorAddr);

    masm.tagValue(JSVAL_TYPE_STRING, temp, output);
    masm.jump(ool->rejoin());

    masm.bind(&iterDone);
    masm.moveValue(MagicValue(JS_NO_ITER_VALUE), output);

    masm.bind(ool->rejoin());
}

void
CodeGenerator::visitIsNoIterAndBranch(LIsNoIterAndBranch* lir)
{
    ValueOperand input = ToValue(lir, LIsNoIterAndBranch::Input);
    Label* ifTrue = getJumpLabelForBranch(lir->ifTrue());
    Label* ifFalse = getJumpLabelForBranch(lir->ifFalse());

    masm.branchTestMagic(Assembler::Equal, input, ifTrue);

    if (!isNextBlock(lir->ifFalse()->lir()))
        masm.jump(ifFalse);
}

typedef void (*CloseIteratorFromIonFn)(JSContext*, JSObject*);
static const VMFunction CloseIteratorFromIonInfo =
    FunctionInfo<CloseIteratorFromIonFn>(CloseIteratorFromIon, "CloseIteratorFromIon");

void
CodeGenerator::visitIteratorEnd(LIteratorEnd* lir)
{
    const Register obj = ToRegister(lir->object());
    const Register temp1 = ToRegister(lir->temp1());
    const Register temp2 = ToRegister(lir->temp2());
    const Register temp3 = ToRegister(lir->temp3());

    OutOfLineCode* ool = oolCallVM(CloseIteratorFromIonInfo, lir, ArgList(obj), StoreNothing());

    LoadNativeIterator(masm, obj, temp1, ool->entry());

    // Clear active bit.
    masm.and32(Imm32(~JSITER_ACTIVE), Address(temp1, offsetof(NativeIterator, flags)));

    // Reset property cursor.
    masm.loadPtr(Address(temp1, offsetof(NativeIterator, props_array)), temp2);
    masm.storePtr(temp2, Address(temp1, offsetof(NativeIterator, props_cursor)));

    // Unlink from the iterator list.
    const Register next = temp2;
    const Register prev = temp3;
    masm.loadPtr(Address(temp1, NativeIterator::offsetOfNext()), next);
    masm.loadPtr(Address(temp1, NativeIterator::offsetOfPrev()), prev);
    masm.storePtr(prev, Address(next, NativeIterator::offsetOfPrev()));
    masm.storePtr(next, Address(prev, NativeIterator::offsetOfNext()));
#ifdef DEBUG
    masm.storePtr(ImmPtr(nullptr), Address(temp1, NativeIterator::offsetOfNext()));
    masm.storePtr(ImmPtr(nullptr), Address(temp1, NativeIterator::offsetOfPrev()));
#endif

    masm.bind(ool->rejoin());
}

void
CodeGenerator::visitArgumentsLength(LArgumentsLength* lir)
{
    // read number of actual arguments from the JS frame.
    Register argc = ToRegister(lir->output());
    Address ptr(masm.getStackPointer(), frameSize() + JitFrameLayout::offsetOfNumActualArgs());

    masm.loadPtr(ptr, argc);
}

void
CodeGenerator::visitGetFrameArgument(LGetFrameArgument* lir)
{
    ValueOperand result = ToOutValue(lir);
    const LAllocation* index = lir->index();
    size_t argvOffset = frameSize() + JitFrameLayout::offsetOfActualArgs();

    if (index->isConstant()) {
        int32_t i = index->toConstant()->toInt32();
        Address argPtr(masm.getStackPointer(), sizeof(Value) * i + argvOffset);
        masm.loadValue(argPtr, result);
    } else {
        Register i = ToRegister(index);
        BaseValueIndex argPtr(masm.getStackPointer(), i, argvOffset);
        masm.loadValue(argPtr, result);
    }
}

void
CodeGenerator::visitSetFrameArgumentT(LSetFrameArgumentT* lir)
{
    size_t argOffset = frameSize() + JitFrameLayout::offsetOfActualArgs() +
                       (sizeof(Value) * lir->mir()->argno());

    MIRType type = lir->mir()->value()->type();

    if (type == MIRType::Double) {
        // Store doubles directly.
        FloatRegister input = ToFloatRegister(lir->input());
        masm.storeDouble(input, Address(masm.getStackPointer(), argOffset));

    } else {
        Register input = ToRegister(lir->input());
        masm.storeValue(ValueTypeFromMIRType(type), input, Address(masm.getStackPointer(), argOffset));
    }
}

void
CodeGenerator:: visitSetFrameArgumentC(LSetFrameArgumentC* lir)
{
    size_t argOffset = frameSize() + JitFrameLayout::offsetOfActualArgs() +
                       (sizeof(Value) * lir->mir()->argno());
    masm.storeValue(lir->val(), Address(masm.getStackPointer(), argOffset));
}

void
CodeGenerator:: visitSetFrameArgumentV(LSetFrameArgumentV* lir)
{
    const ValueOperand val = ToValue(lir, LSetFrameArgumentV::Input);
    size_t argOffset = frameSize() + JitFrameLayout::offsetOfActualArgs() +
                       (sizeof(Value) * lir->mir()->argno());
    masm.storeValue(val, Address(masm.getStackPointer(), argOffset));
}

typedef bool (*RunOnceScriptPrologueFn)(JSContext*, HandleScript);
static const VMFunction RunOnceScriptPrologueInfo =
    FunctionInfo<RunOnceScriptPrologueFn>(js::RunOnceScriptPrologue, "RunOnceScriptPrologue");

void
CodeGenerator::visitRunOncePrologue(LRunOncePrologue* lir)
{
    pushArg(ImmGCPtr(lir->mir()->block()->info().script()));
    callVM(RunOnceScriptPrologueInfo, lir);
}

typedef JSObject* (*InitRestParameterFn)(JSContext*, uint32_t, Value*, HandleObject,
                                         HandleObject);
static const VMFunction InitRestParameterInfo =
    FunctionInfo<InitRestParameterFn>(InitRestParameter, "InitRestParameter");

void
CodeGenerator::emitRest(LInstruction* lir, Register array, Register numActuals,
                        Register temp0, Register temp1, unsigned numFormals,
                        JSObject* templateObject, bool saveAndRestore, Register resultreg)
{
    // Compute actuals() + numFormals.
    size_t actualsOffset = frameSize() + JitFrameLayout::offsetOfActualArgs();
    masm.moveStackPtrTo(temp1);
    masm.addPtr(Imm32(sizeof(Value) * numFormals + actualsOffset), temp1);

    // Compute numActuals - numFormals.
    Label emptyLength, joinLength;
    masm.movePtr(numActuals, temp0);
    masm.branch32(Assembler::LessThanOrEqual, temp0, Imm32(numFormals), &emptyLength);
    masm.sub32(Imm32(numFormals), temp0);
    masm.jump(&joinLength);
    {
        masm.bind(&emptyLength);
        masm.move32(Imm32(0), temp0);
    }
    masm.bind(&joinLength);

    if (saveAndRestore)
        saveLive(lir);

    pushArg(array);
    pushArg(ImmGCPtr(templateObject));
    pushArg(temp1);
    pushArg(temp0);

    callVM(InitRestParameterInfo, lir);

    if (saveAndRestore) {
        storePointerResultTo(resultreg);
        restoreLive(lir);
    }
}

void
CodeGenerator::visitRest(LRest* lir)
{
    Register numActuals = ToRegister(lir->numActuals());
    Register temp0 = ToRegister(lir->getTemp(0));
    Register temp1 = ToRegister(lir->getTemp(1));
    Register temp2 = ToRegister(lir->getTemp(2));
    unsigned numFormals = lir->mir()->numFormals();
    ArrayObject* templateObject = lir->mir()->templateObject();

    Label joinAlloc, failAlloc;
    masm.createGCObject(temp2, temp0, templateObject, gc::DefaultHeap, &failAlloc);
    masm.jump(&joinAlloc);
    {
        masm.bind(&failAlloc);
        masm.movePtr(ImmPtr(nullptr), temp2);
    }
    masm.bind(&joinAlloc);

    emitRest(lir, temp2, numActuals, temp0, temp1, numFormals, templateObject, false, ToRegister(lir->output()));
}

bool
CodeGenerator::generateWasm(wasm::SigIdDesc sigId, wasm::BytecodeOffset trapOffset,
                            wasm::FuncOffsets* offsets)
{
    JitSpew(JitSpew_Codegen, "# Emitting wasm code");

    wasm::IsLeaf isLeaf = !gen->needsOverrecursedCheck();

    wasm::GenerateFunctionPrologue(masm, frameSize(), isLeaf, sigId, trapOffset, offsets);

    if (!generateBody())
        return false;

    masm.bind(&returnLabel_);
    wasm::GenerateFunctionEpilogue(masm, frameSize(), offsets);

#if defined(JS_ION_PERF)
    // Note the end of the inline code and start of the OOL code.
    gen->perfSpewer().noteEndInlineCode(masm);
#endif

    if (!generateOutOfLineCode())
        return false;

    masm.wasmEmitOldTrapOutOfLineCode();

    masm.flush();
    if (masm.oom())
        return false;

    offsets->end = masm.currentOffset();

    MOZ_ASSERT(!masm.failureLabel()->used());
    MOZ_ASSERT(snapshots_.listSize() == 0);
    MOZ_ASSERT(snapshots_.RVATableSize() == 0);
    MOZ_ASSERT(recovers_.size() == 0);
    MOZ_ASSERT(bailouts_.empty());
    MOZ_ASSERT(graph.numConstants() == 0);
    MOZ_ASSERT(safepointIndices_.empty());
    MOZ_ASSERT(osiIndices_.empty());
    MOZ_ASSERT(icList_.empty());
    MOZ_ASSERT(safepoints_.size() == 0);
    MOZ_ASSERT(!scriptCounts_);
    return true;
}

bool
CodeGenerator::generate()
{
    JitSpew(JitSpew_Codegen, "# Emitting code for script %s:%zu",
            gen->info().script()->filename(),
            gen->info().script()->lineno());

    // Initialize native code table with an entry to the start of
    // top-level script.
    InlineScriptTree* tree = gen->info().inlineScriptTree();
    jsbytecode* startPC = tree->script()->code();
    BytecodeSite* startSite = new(gen->alloc()) BytecodeSite(tree, startPC);
    if (!addNativeToBytecodeEntry(startSite))
        return false;

    if (!snapshots_.init())
        return false;

    if (!safepoints_.init(gen->alloc()))
        return false;

    if (!generatePrologue())
        return false;

    // Before generating any code, we generate type checks for all parameters.
    // This comes before deoptTable_, because we can't use deopt tables without
    // creating the actual frame.
    generateArgumentsChecks();

    if (frameClass_ != FrameSizeClass::None())
        deoptTable_.emplace(gen->jitRuntime()->getBailoutTable(frameClass_));

    // Skip over the alternative entry to IonScript code.
    Label skipPrologue;
    masm.jump(&skipPrologue);

    // An alternative entry to the IonScript code, which doesn't test the
    // arguments.
    masm.flushBuffer();
    setSkipArgCheckEntryOffset(masm.size());
    masm.setFramePushed(0);
    if (!generatePrologue())
        return false;

    masm.bind(&skipPrologue);

#ifdef DEBUG
    // Assert that the argument types are correct.
    generateArgumentsChecks(/* assert = */ true);
#endif

    // Reset native => bytecode map table with top-level script and startPc.
    if (!addNativeToBytecodeEntry(startSite))
        return false;

    if (!generateBody())
        return false;

    // Reset native => bytecode map table with top-level script and startPc.
    if (!addNativeToBytecodeEntry(startSite))
        return false;

    if (!generateEpilogue())
        return false;

    // Reset native => bytecode map table with top-level script and startPc.
    if (!addNativeToBytecodeEntry(startSite))
        return false;

    generateInvalidateEpilogue();
#if defined(JS_ION_PERF)
    // Note the end of the inline code and start of the OOL code.
    perfSpewer_.noteEndInlineCode(masm);
#endif

    // native => bytecode entries for OOL code will be added
    // by CodeGeneratorShared::generateOutOfLineCode
    if (!generateOutOfLineCode())
        return false;

    // Add terminal entry.
    if (!addNativeToBytecodeEntry(startSite))
        return false;

    // Dump Native to bytecode entries to spew.
    dumpNativeToBytecodeEntries();

    return !masm.oom();
}

bool
CodeGenerator::linkSharedStubs(JSContext* cx)
{
    for (uint32_t i = 0; i < sharedStubs_.length(); i++) {
        ICStub *stub = nullptr;

        switch (sharedStubs_[i].kind) {
          case ICStub::Kind::BinaryArith_Fallback: {
            ICBinaryArith_Fallback::Compiler stubCompiler(cx, ICStubCompiler::Engine::IonSharedIC);
            stub = stubCompiler.getStub(&stubSpace_);
            break;
          }
          case ICStub::Kind::UnaryArith_Fallback: {
            ICUnaryArith_Fallback::Compiler stubCompiler(cx, ICStubCompiler::Engine::IonSharedIC);
            stub = stubCompiler.getStub(&stubSpace_);
            break;
          }
          case ICStub::Kind::Compare_Fallback: {
            ICCompare_Fallback::Compiler stubCompiler(cx, ICStubCompiler::Engine::IonSharedIC);
            stub = stubCompiler.getStub(&stubSpace_);
            break;
          }
          case ICStub::Kind::GetProp_Fallback: {
            ICGetProp_Fallback::Compiler stubCompiler(cx, ICStubCompiler::Engine::IonSharedIC);
            stub = stubCompiler.getStub(&stubSpace_);
            break;
          }
          case ICStub::Kind::NewArray_Fallback: {
            JSScript* script = sharedStubs_[i].entry.script();
            jsbytecode* pc = sharedStubs_[i].entry.pc(script);
            ObjectGroup* group = ObjectGroup::allocationSiteGroup(cx, script, pc, JSProto_Array);
            if (!group)
                return false;

            ICNewArray_Fallback::Compiler stubCompiler(cx, group, ICStubCompiler::Engine::IonSharedIC);
            stub = stubCompiler.getStub(&stubSpace_);
            break;
          }
          case ICStub::Kind::NewObject_Fallback: {
            ICNewObject_Fallback::Compiler stubCompiler(cx, ICStubCompiler::Engine::IonSharedIC);
            stub = stubCompiler.getStub(&stubSpace_);
            break;
          }
          default:
            MOZ_CRASH("Unsupported shared stub.");
        }

        if (!stub)
            return false;

        sharedStubs_[i].entry.setFirstStub(stub);
    }
    return true;
}

bool
CodeGenerator::link(JSContext* cx, CompilerConstraintList* constraints)
{
    // We cancel off-thread Ion compilations in a few places during GC, but if
    // this compilation was performed off-thread it will already have been
    // removed from the relevant lists by this point. Don't allow GC here.
    JS::AutoAssertNoGC nogc(cx);

    RootedScript script(cx, gen->info().script());
    OptimizationLevel optimizationLevel = gen->optimizationInfo().level();

    // Perform any read barriers which were skipped while compiling the
    // script, which may have happened off-thread.
    const JitCompartment* jc = gen->compartment->jitCompartment();
    jc->performStubReadBarriers(compartmentStubsToReadBarrier_);
    jc->performSIMDTemplateReadBarriers(simdTemplatesToReadBarrier_);

    // We finished the new IonScript. Invalidate the current active IonScript,
    // so we can replace it with this new (probably higher optimized) version.
    if (script->hasIonScript()) {
        MOZ_ASSERT(script->ionScript()->isRecompiling());
        // Do a normal invalidate, except don't cancel offThread compilations,
        // since that will cancel this compilation too.
        Invalidate(cx, script, /* resetUses */ false, /* cancelOffThread*/ false);
    }

    if (scriptCounts_ && !script->hasScriptCounts() && !script->initScriptCounts(cx))
        return false;

    if (!linkSharedStubs(cx))
        return false;

    // Check to make sure we didn't have a mid-build invalidation. If so, we
    // will trickle to jit::Compile() and return Method_Skipped.
    uint32_t warmUpCount = script->getWarmUpCount();

    // Record constraints. If an error occured, returns false and potentially
    // prevent future compilations. Otherwise, if an invalidation occured, then
    // skip the current compilation.
    RecompileInfo recompileInfo;
    bool validRecompiledInfo = false;
    if (!FinishCompilation(cx, script, constraints, &recompileInfo, &validRecompiledInfo))
        return false;
    if (!validRecompiledInfo)
        return true;
    auto guardRecordedConstraints = mozilla::MakeScopeExit([&] {
        // In case of error, invalidate the current recompileInfo.
        recompileInfo.compilerOutput(cx->zone()->types)->invalidate();
    });

    // IonMonkey could have inferred better type information during
    // compilation. Since adding the new information to the actual type
    // information can reset the usecount, increase it back to what it was
    // before.
    if (warmUpCount > script->getWarmUpCount())
        script->incWarmUpCounter(warmUpCount - script->getWarmUpCount());

    uint32_t argumentSlots = (gen->info().nargs() + 1) * sizeof(Value);
    uint32_t scriptFrameSize = frameClass_ == FrameSizeClass::None()
                           ? frameDepth_
                           : FrameSizeClass::FromDepth(frameDepth_).frameSize();

    // We encode safepoints after the OSI-point offsets have been determined.
    if (!encodeSafepoints())
        return false;

    IonScript* ionScript =
        IonScript::New(cx, recompileInfo,
                       graph.totalSlotCount(), argumentSlots, scriptFrameSize,
                       snapshots_.listSize(), snapshots_.RVATableSize(),
                       recovers_.size(), bailouts_.length(), graph.numConstants(),
                       safepointIndices_.length(), osiIndices_.length(),
                       icList_.length(), runtimeData_.length(),
                       safepoints_.size(), patchableBackedges_.length(),
                       sharedStubs_.length(), optimizationLevel);
    if (!ionScript)
        return false;
    auto guardIonScript = mozilla::MakeScopeExit([&ionScript] {
        // Use js_free instead of IonScript::Destroy: the cache list and
        // backedge list are still uninitialized.
        js_free(ionScript);
    });

    Linker linker(masm, nogc);
    AutoFlushICache afc("IonLink");
    JitCode* code = linker.newCode(cx, CodeKind::Ion, !patchableBackedges_.empty());
    if (!code)
        return false;

    // Encode native to bytecode map if profiling is enabled.
    if (isProfilerInstrumentationEnabled()) {
        // Generate native-to-bytecode main table.
        if (!generateCompactNativeToBytecodeMap(cx, code))
            return false;

        uint8_t* ionTableAddr = ((uint8_t*) nativeToBytecodeMap_) + nativeToBytecodeTableOffset_;
        JitcodeIonTable* ionTable = (JitcodeIonTable*) ionTableAddr;

        // Construct the IonEntry that will go into the global table.
        JitcodeGlobalEntry::IonEntry entry;
        if (!ionTable->makeIonEntry(cx, code, nativeToBytecodeScriptListLength_,
                                    nativeToBytecodeScriptList_, entry))
        {
            js_free(nativeToBytecodeScriptList_);
            js_free(nativeToBytecodeMap_);
            return false;
        }

        // nativeToBytecodeScriptList_ is no longer needed.
        js_free(nativeToBytecodeScriptList_);

        // Generate the tracked optimizations map.
        if (isOptimizationTrackingEnabled()) {
            // Treat OOMs and failures as if optimization tracking were turned off.
            IonTrackedTypeVector* allTypes = cx->new_<IonTrackedTypeVector>();
            if (allTypes && generateCompactTrackedOptimizationsMap(cx, code, allTypes)) {
                const uint8_t* optsRegionTableAddr = trackedOptimizationsMap_ +
                                                     trackedOptimizationsRegionTableOffset_;
                const IonTrackedOptimizationsRegionTable* optsRegionTable =
                    (const IonTrackedOptimizationsRegionTable*) optsRegionTableAddr;
                const uint8_t* optsTypesTableAddr = trackedOptimizationsMap_ +
                                                    trackedOptimizationsTypesTableOffset_;
                const IonTrackedOptimizationsTypesTable* optsTypesTable =
                    (const IonTrackedOptimizationsTypesTable*) optsTypesTableAddr;
                const uint8_t* optsAttemptsTableAddr = trackedOptimizationsMap_ +
                                                       trackedOptimizationsAttemptsTableOffset_;
                const IonTrackedOptimizationsAttemptsTable* optsAttemptsTable =
                    (const IonTrackedOptimizationsAttemptsTable*) optsAttemptsTableAddr;
                entry.initTrackedOptimizations(optsRegionTable, optsTypesTable, optsAttemptsTable,
                                               allTypes);
            } else {
                cx->recoverFromOutOfMemory();
            }
        }

        // Add entry to the global table.
        JitcodeGlobalTable* globalTable = cx->runtime()->jitRuntime()->getJitcodeGlobalTable();
        if (!globalTable->addEntry(entry)) {
            // Memory may have been allocated for the entry.
            entry.destroy();
            return false;
        }

        // Mark the jitcode as having a bytecode map.
        code->setHasBytecodeMap();
    } else {
        // Add a dumy jitcodeGlobalTable entry.
        JitcodeGlobalEntry::DummyEntry entry;
        entry.init(code, code->raw(), code->rawEnd());

        // Add entry to the global table.
        JitcodeGlobalTable* globalTable = cx->runtime()->jitRuntime()->getJitcodeGlobalTable();
        if (!globalTable->addEntry(entry)) {
            // Memory may have been allocated for the entry.
            entry.destroy();
            return false;
        }

        // Mark the jitcode as having a bytecode map.
        code->setHasBytecodeMap();
    }

    ionScript->setMethod(code);
    ionScript->setSkipArgCheckEntryOffset(getSkipArgCheckEntryOffset());

    // If the Gecko Profiler is enabled, mark IonScript as having been
    // instrumented accordingly.
    if (isProfilerInstrumentationEnabled())
        ionScript->setHasProfilingInstrumentation();

    script->setIonScript(cx->runtime(), ionScript);

    // Adopt fallback shared stubs from the compiler into the ion script.
    ionScript->adoptFallbackStubs(&stubSpace_);

    Assembler::PatchDataWithValueCheck(CodeLocationLabel(code, invalidateEpilogueData_),
                                       ImmPtr(ionScript),
                                       ImmPtr((void*)-1));

    for (size_t i = 0; i < ionScriptLabels_.length(); i++) {
        Assembler::PatchDataWithValueCheck(CodeLocationLabel(code, ionScriptLabels_[i]),
                                           ImmPtr(ionScript),
                                           ImmPtr((void*)-1));
    }

#ifdef JS_TRACE_LOGGING
    bool TLFailed = false;

    for (uint32_t i = 0; i < patchableTLEvents_.length(); i++) {
        TraceLoggerEvent event(patchableTLEvents_[i].event);
        if (!event.hasTextId() || !ionScript->addTraceLoggerEvent(event)) {
            TLFailed = true;
            break;
        }
        Assembler::PatchDataWithValueCheck(CodeLocationLabel(code, patchableTLEvents_[i].offset),
                ImmPtr((void*) uintptr_t(event.textId())),
                ImmPtr((void*)0));
    }

    if (!TLFailed && patchableTLScripts_.length() > 0) {
        MOZ_ASSERT(TraceLogTextIdEnabled(TraceLogger_Scripts));
        TraceLoggerEvent event(TraceLogger_Scripts, script);
        if (!event.hasTextId() || !ionScript->addTraceLoggerEvent(event))
            TLFailed = true;
        if (!TLFailed) {
            uint32_t textId = event.textId();
            for (uint32_t i = 0; i < patchableTLScripts_.length(); i++) {
                Assembler::PatchDataWithValueCheck(CodeLocationLabel(code, patchableTLScripts_[i]),
                                                   ImmPtr((void*) uintptr_t(textId)),
                                                   ImmPtr((void*)0));
            }
        }
    }
#endif

    // Patch shared stub IC loads using IC entries
    for (size_t i = 0; i < sharedStubs_.length(); i++) {
        CodeOffset label = sharedStubs_[i].label;

        IonICEntry& entry = ionScript->sharedStubList()[i];
        entry = sharedStubs_[i].entry;
        Assembler::PatchDataWithValueCheck(CodeLocationLabel(code, label),
                                           ImmPtr(&entry),
                                           ImmPtr((void*)-1));

        MOZ_ASSERT(entry.hasStub());
        MOZ_ASSERT(entry.firstStub()->isFallback());

        entry.firstStub()->toFallbackStub()->fixupICEntry(&entry);
    }

    // for generating inline caches during the execution.
    if (runtimeData_.length())
        ionScript->copyRuntimeData(&runtimeData_[0]);
    if (icList_.length())
        ionScript->copyICEntries(&icList_[0], masm);

    for (size_t i = 0; i < icInfo_.length(); i++) {
        IonIC& ic = ionScript->getICFromIndex(i);
        Assembler::PatchDataWithValueCheck(CodeLocationLabel(code, icInfo_[i].icOffsetForJump),
                                           ImmPtr(ic.codeRawPtr()),
                                           ImmPtr((void*)-1));
        Assembler::PatchDataWithValueCheck(CodeLocationLabel(code, icInfo_[i].icOffsetForPush),
                                           ImmPtr(&ic),
                                           ImmPtr((void*)-1));
    }

    JitSpew(JitSpew_Codegen, "Created IonScript %p (raw %p)",
            (void*) ionScript, (void*) code->raw());

    ionScript->setInvalidationEpilogueDataOffset(invalidateEpilogueData_.offset());
    ionScript->setOsrPc(gen->info().osrPc());
    ionScript->setOsrEntryOffset(getOsrEntryOffset());
    ionScript->setInvalidationEpilogueOffset(invalidate_.offset());

#if defined(JS_ION_PERF)
    if (PerfEnabled())
        perfSpewer_.writeProfile(script, code, masm);
#endif

#ifdef MOZ_VTUNE
    vtune::MarkScript(code, script, "ion");
#endif

    // for marking during GC.
    if (safepointIndices_.length())
        ionScript->copySafepointIndices(&safepointIndices_[0]);
    if (safepoints_.size())
        ionScript->copySafepoints(&safepoints_);

    // for reconvering from an Ion Frame.
    if (bailouts_.length())
        ionScript->copyBailoutTable(&bailouts_[0]);
    if (osiIndices_.length())
        ionScript->copyOsiIndices(&osiIndices_[0]);
    if (snapshots_.listSize())
        ionScript->copySnapshots(&snapshots_);
    MOZ_ASSERT_IF(snapshots_.listSize(), recovers_.size());
    if (recovers_.size())
        ionScript->copyRecovers(&recovers_);
    if (graph.numConstants()) {
        const Value* vp = graph.constantPool();
        ionScript->copyConstants(vp);
        for (size_t i = 0; i < graph.numConstants(); i++) {
            const Value& v = vp[i];
            if ((v.isObject() || v.isString()) && IsInsideNursery(v.toGCThing())) {
                cx->zone()->group()->storeBuffer().putWholeCell(script);
                break;
            }
        }
    }
    if (patchableBackedges_.length() > 0)
        ionScript->copyPatchableBackedges(cx, code, patchableBackedges_.begin(), masm);

    // Attach any generated script counts to the script.
    if (IonScriptCounts* counts = extractScriptCounts())
        script->addIonCounts(counts);

    guardIonScript.release();
    guardRecordedConstraints.release();
    return true;
}

// An out-of-line path to convert a boxed int32 to either a float or double.
class OutOfLineUnboxFloatingPoint : public OutOfLineCodeBase<CodeGenerator>
{
    LUnboxFloatingPoint* unboxFloatingPoint_;

  public:
    explicit OutOfLineUnboxFloatingPoint(LUnboxFloatingPoint* unboxFloatingPoint)
      : unboxFloatingPoint_(unboxFloatingPoint)
    { }

    void accept(CodeGenerator* codegen) override {
        codegen->visitOutOfLineUnboxFloatingPoint(this);
    }

    LUnboxFloatingPoint* unboxFloatingPoint() const {
        return unboxFloatingPoint_;
    }
};

void
CodeGenerator::visitUnboxFloatingPoint(LUnboxFloatingPoint* lir)
{
    const ValueOperand box = ToValue(lir, LUnboxFloatingPoint::Input);
    const LDefinition* result = lir->output();

    // Out-of-line path to convert int32 to double or bailout
    // if this instruction is fallible.
    OutOfLineUnboxFloatingPoint* ool = new(alloc()) OutOfLineUnboxFloatingPoint(lir);
    addOutOfLineCode(ool, lir->mir());

    FloatRegister resultReg = ToFloatRegister(result);
    masm.branchTestDouble(Assembler::NotEqual, box, ool->entry());
    masm.unboxDouble(box, resultReg);
    if (lir->type() == MIRType::Float32)
        masm.convertDoubleToFloat32(resultReg, resultReg);
    masm.bind(ool->rejoin());
}

void
CodeGenerator::visitOutOfLineUnboxFloatingPoint(OutOfLineUnboxFloatingPoint* ool)
{
    LUnboxFloatingPoint* ins = ool->unboxFloatingPoint();
    const ValueOperand value = ToValue(ins, LUnboxFloatingPoint::Input);

    if (ins->mir()->fallible()) {
        Label bail;
        masm.branchTestInt32(Assembler::NotEqual, value, &bail);
        bailoutFrom(&bail, ins->snapshot());
    }
    masm.int32ValueToFloatingPoint(value, ToFloatRegister(ins->output()), ins->type());
    masm.jump(ool->rejoin());
}

typedef JSObject* (*BindVarFn)(JSContext*, HandleObject);
static const VMFunction BindVarInfo = FunctionInfo<BindVarFn>(jit::BindVar, "BindVar");

void
CodeGenerator::visitCallBindVar(LCallBindVar* lir)
{
    pushArg(ToRegister(lir->environmentChain()));
    callVM(BindVarInfo, lir);
}

typedef bool (*GetPropertyFn)(JSContext*, HandleValue, HandlePropertyName, MutableHandleValue);
static const VMFunction GetPropertyInfo = FunctionInfo<GetPropertyFn>(GetProperty, "GetProperty");

void
CodeGenerator::visitCallGetProperty(LCallGetProperty* lir)
{
    pushArg(ImmGCPtr(lir->mir()->name()));
    pushArg(ToValue(lir, LCallGetProperty::Value));

    callVM(GetPropertyInfo, lir);
}

typedef bool (*GetOrCallElementFn)(JSContext*, MutableHandleValue, HandleValue, MutableHandleValue);
static const VMFunction GetElementInfo =
    FunctionInfo<GetOrCallElementFn>(js::GetElement, "GetElement");
static const VMFunction CallElementInfo =
    FunctionInfo<GetOrCallElementFn>(js::CallElement, "CallElement");

void
CodeGenerator::visitCallGetElement(LCallGetElement* lir)
{
    pushArg(ToValue(lir, LCallGetElement::RhsInput));
    pushArg(ToValue(lir, LCallGetElement::LhsInput));

    JSOp op = JSOp(*lir->mir()->resumePoint()->pc());

    if (op == JSOP_GETELEM) {
        callVM(GetElementInfo, lir);
    } else {
        MOZ_ASSERT(op == JSOP_CALLELEM);
        callVM(CallElementInfo, lir);
    }
}

void
CodeGenerator::visitCallSetElement(LCallSetElement* lir)
{
    Register obj = ToRegister(lir->getOperand(0));
    pushArg(Imm32(lir->mir()->strict()));
    pushArg(TypedOrValueRegister(MIRType::Object, AnyRegister(obj)));
    pushArg(ToValue(lir, LCallSetElement::Value));
    pushArg(ToValue(lir, LCallSetElement::Index));
    pushArg(obj);
    callVM(SetObjectElementInfo, lir);
}

typedef bool (*InitElementArrayFn)(JSContext*, jsbytecode*, HandleObject, uint32_t, HandleValue);
static const VMFunction InitElementArrayInfo =
    FunctionInfo<InitElementArrayFn>(js::InitElementArray, "InitElementArray");

void
CodeGenerator::visitCallInitElementArray(LCallInitElementArray* lir)
{
    pushArg(ToValue(lir, LCallInitElementArray::Value));
    if (lir->index()->isConstant())
        pushArg(Imm32(ToInt32(lir->index())));
    else
        pushArg(ToRegister(lir->index()));
    pushArg(ToRegister(lir->object()));
    pushArg(ImmPtr(lir->mir()->resumePoint()->pc()));
    callVM(InitElementArrayInfo, lir);
}

void
CodeGenerator::visitLoadFixedSlotV(LLoadFixedSlotV* ins)
{
    const Register obj = ToRegister(ins->getOperand(0));
    size_t slot = ins->mir()->slot();
    ValueOperand result = ToOutValue(ins);

    masm.loadValue(Address(obj, NativeObject::getFixedSlotOffset(slot)), result);
}

void
CodeGenerator::visitLoadFixedSlotT(LLoadFixedSlotT* ins)
{
    const Register obj = ToRegister(ins->getOperand(0));
    size_t slot = ins->mir()->slot();
    AnyRegister result = ToAnyRegister(ins->getDef(0));
    MIRType type = ins->mir()->type();

    masm.loadUnboxedValue(Address(obj, NativeObject::getFixedSlotOffset(slot)), type, result);
}

void
CodeGenerator::visitLoadFixedSlotAndUnbox(LLoadFixedSlotAndUnbox* ins)
{
    const MLoadFixedSlotAndUnbox* mir = ins->mir();
    MIRType type = mir->type();
    const Register input = ToRegister(ins->getOperand(0));
    AnyRegister result = ToAnyRegister(ins->output());
    size_t slot = mir->slot();

    Address address(input, NativeObject::getFixedSlotOffset(slot));
    Label bail;
    if (type == MIRType::Double) {
        MOZ_ASSERT(result.isFloat());
        masm.ensureDouble(address, result.fpu(), &bail);
        if (mir->fallible())
            bailoutFrom(&bail, ins->snapshot());
        return;
    }
    if (mir->fallible()) {
        switch (type) {
          case MIRType::Int32:
            masm.branchTestInt32(Assembler::NotEqual, address, &bail);
            break;
          case MIRType::Boolean:
            masm.branchTestBoolean(Assembler::NotEqual, address, &bail);
            break;
          default:
            MOZ_CRASH("Given MIRType cannot be unboxed.");
        }
        bailoutFrom(&bail, ins->snapshot());
    }
    masm.loadUnboxedValue(address, type, result);
}

void
CodeGenerator::visitStoreFixedSlotV(LStoreFixedSlotV* ins)
{
    const Register obj = ToRegister(ins->getOperand(0));
    size_t slot = ins->mir()->slot();

    const ValueOperand value = ToValue(ins, LStoreFixedSlotV::Value);

    Address address(obj, NativeObject::getFixedSlotOffset(slot));
    if (ins->mir()->needsBarrier())
        emitPreBarrier(address);

    masm.storeValue(value, address);
}

void
CodeGenerator::visitStoreFixedSlotT(LStoreFixedSlotT* ins)
{
    const Register obj = ToRegister(ins->getOperand(0));
    size_t slot = ins->mir()->slot();

    const LAllocation* value = ins->value();
    MIRType valueType = ins->mir()->value()->type();

    Address address(obj, NativeObject::getFixedSlotOffset(slot));
    if (ins->mir()->needsBarrier())
        emitPreBarrier(address);

    if (valueType == MIRType::ObjectOrNull) {
        Register nvalue = ToRegister(value);
        masm.storeObjectOrNull(nvalue, address);
    } else {
        ConstantOrRegister nvalue = value->isConstant()
                                    ? ConstantOrRegister(value->toConstant()->toJSValue())
                                    : TypedOrValueRegister(valueType, ToAnyRegister(value));
        masm.storeConstantOrRegister(nvalue, address);
    }
}

void
CodeGenerator::visitGetNameCache(LGetNameCache* ins)
{
    LiveRegisterSet liveRegs = ins->safepoint()->liveRegs();
    Register envChain = ToRegister(ins->envObj());
    ValueOperand output = ToOutValue(ins);
    Register temp = ToRegister(ins->temp());

    IonGetNameIC ic(liveRegs, envChain, output, temp);
    addIC(ins, allocateIC(ic));
}

void
CodeGenerator::addGetPropertyCache(LInstruction* ins, LiveRegisterSet liveRegs,
                                   TypedOrValueRegister value, const ConstantOrRegister& id,
                                   TypedOrValueRegister output, Register maybeTemp,
                                   GetPropertyResultFlags resultFlags)
{
    CacheKind kind = CacheKind::GetElem;
    if (id.constant() && id.value().isString()) {
        JSString* idString = id.value().toString();
        uint32_t dummy;
        if (idString->isAtom() && !idString->asAtom().isIndex(&dummy))
            kind = CacheKind::GetProp;
    }
    IonGetPropertyIC cache(kind, liveRegs, value, id, output, maybeTemp, resultFlags);
    addIC(ins, allocateIC(cache));
}

void
CodeGenerator::addSetPropertyCache(LInstruction* ins, LiveRegisterSet liveRegs, Register objReg,
                                   Register temp, FloatRegister tempDouble,
                                   FloatRegister tempF32, const ConstantOrRegister& id,
                                   const ConstantOrRegister& value,
                                   bool strict, bool needsPostBarrier, bool needsTypeBarrier,
                                   bool guardHoles)
{
    CacheKind kind = CacheKind::SetElem;
    if (id.constant() && id.value().isString()) {
        JSString* idString = id.value().toString();
        uint32_t dummy;
        if (idString->isAtom() && !idString->asAtom().isIndex(&dummy))
            kind = CacheKind::SetProp;
    }
    IonSetPropertyIC cache(kind, liveRegs, objReg, temp, tempDouble, tempF32,
                           id, value, strict, needsPostBarrier, needsTypeBarrier, guardHoles);
    addIC(ins, allocateIC(cache));
}

ConstantOrRegister
CodeGenerator::toConstantOrRegister(LInstruction* lir, size_t n, MIRType type)
{
    if (type == MIRType::Value)
        return TypedOrValueRegister(ToValue(lir, n));

    const LAllocation* value = lir->getOperand(n);
    if (value->isConstant())
        return ConstantOrRegister(value->toConstant()->toJSValue());

    return TypedOrValueRegister(type, ToAnyRegister(value));
}

static GetPropertyResultFlags
IonGetPropertyICFlags(const MGetPropertyCache* mir)
{
    GetPropertyResultFlags flags = GetPropertyResultFlags::None;
    if (mir->monitoredResult())
        flags |= GetPropertyResultFlags::Monitored;

    if (mir->type() == MIRType::Value) {
        if (TemporaryTypeSet* types = mir->resultTypeSet()) {
            if (types->hasType(TypeSet::UndefinedType()))
                flags |= GetPropertyResultFlags::AllowUndefined;
            if (types->hasType(TypeSet::Int32Type()))
                flags |= GetPropertyResultFlags::AllowInt32;
            if (types->hasType(TypeSet::DoubleType()))
                flags |= GetPropertyResultFlags::AllowDouble;
        } else {
            flags |= GetPropertyResultFlags::AllowUndefined
                   | GetPropertyResultFlags::AllowInt32
                   | GetPropertyResultFlags::AllowDouble;
        }
    } else if (mir->type() == MIRType::Int32) {
        flags |= GetPropertyResultFlags::AllowInt32;
    } else if (mir->type() == MIRType::Double) {
        flags |= GetPropertyResultFlags::AllowInt32 | GetPropertyResultFlags::AllowDouble;
    }

    return flags;
}

void
CodeGenerator::visitGetPropertyCacheV(LGetPropertyCacheV* ins)
{
    LiveRegisterSet liveRegs = ins->safepoint()->liveRegs();
    TypedOrValueRegister value =
        toConstantOrRegister(ins, LGetPropertyCacheV::Value, ins->mir()->value()->type()).reg();
    ConstantOrRegister id = toConstantOrRegister(ins, LGetPropertyCacheV::Id, ins->mir()->idval()->type());
    TypedOrValueRegister output(ToOutValue(ins));
    Register maybeTemp = ins->temp()->isBogusTemp() ? InvalidReg : ToRegister(ins->temp());

    addGetPropertyCache(ins, liveRegs, value, id, output, maybeTemp,
                        IonGetPropertyICFlags(ins->mir()));
}

void
CodeGenerator::visitGetPropertyCacheT(LGetPropertyCacheT* ins)
{
    LiveRegisterSet liveRegs = ins->safepoint()->liveRegs();
    TypedOrValueRegister value =
        toConstantOrRegister(ins, LGetPropertyCacheV::Value, ins->mir()->value()->type()).reg();
    ConstantOrRegister id = toConstantOrRegister(ins, LGetPropertyCacheT::Id, ins->mir()->idval()->type());
    TypedOrValueRegister output(ins->mir()->type(), ToAnyRegister(ins->getDef(0)));
    Register maybeTemp = ins->temp()->isBogusTemp() ? InvalidReg : ToRegister(ins->temp());

    addGetPropertyCache(ins, liveRegs, value, id, output, maybeTemp,
                        IonGetPropertyICFlags(ins->mir()));
}

void
CodeGenerator::visitGetPropSuperCacheV(LGetPropSuperCacheV* ins)
{
    LiveRegisterSet liveRegs = ins->safepoint()->liveRegs();
    Register obj = ToRegister(ins->obj());
    TypedOrValueRegister receiver =
        toConstantOrRegister(ins, LGetPropSuperCacheV::Receiver, ins->mir()->receiver()->type()).reg();
    ConstantOrRegister id = toConstantOrRegister(ins, LGetPropSuperCacheV::Id, ins->mir()->idval()->type());
    TypedOrValueRegister output(ToOutValue(ins));

    CacheKind kind = CacheKind::GetElemSuper;
    if (id.constant() && id.value().isString()) {
        JSString* idString = id.value().toString();
        uint32_t dummy;
        if (idString->isAtom() && !idString->asAtom().isIndex(&dummy))
            kind = CacheKind::GetPropSuper;
    }

    IonGetPropSuperIC cache(kind, liveRegs, obj, receiver, id, output);
    addIC(ins, allocateIC(cache));
}

void
CodeGenerator::visitBindNameCache(LBindNameCache* ins)
{
    LiveRegisterSet liveRegs = ins->safepoint()->liveRegs();
    Register envChain = ToRegister(ins->environmentChain());
    Register output = ToRegister(ins->output());
    Register temp = ToRegister(ins->temp());

    IonBindNameIC ic(liveRegs, envChain, output, temp);
    addIC(ins, allocateIC(ic));
}

void
CodeGenerator::visitHasOwnCache(LHasOwnCache* ins)
{
    LiveRegisterSet liveRegs = ins->safepoint()->liveRegs();
    TypedOrValueRegister value =
        toConstantOrRegister(ins, LHasOwnCache::Value, ins->mir()->value()->type()).reg();
    TypedOrValueRegister id =
        toConstantOrRegister(ins, LHasOwnCache::Id, ins->mir()->idval()->type()).reg();
    Register output = ToRegister(ins->output());

    IonHasOwnIC cache(liveRegs, value, id, output);
    addIC(ins, allocateIC(cache));
}

typedef bool (*SetPropertyFn)(JSContext*, HandleObject,
                              HandlePropertyName, const HandleValue, bool, jsbytecode*);
static const VMFunction SetPropertyInfo = FunctionInfo<SetPropertyFn>(SetProperty, "SetProperty");

void
CodeGenerator::visitCallSetProperty(LCallSetProperty* ins)
{
    ConstantOrRegister value = TypedOrValueRegister(ToValue(ins, LCallSetProperty::Value));

    const Register objReg = ToRegister(ins->getOperand(0));

    pushArg(ImmPtr(ins->mir()->resumePoint()->pc()));
    pushArg(Imm32(ins->mir()->strict()));

    pushArg(value);
    pushArg(ImmGCPtr(ins->mir()->name()));
    pushArg(objReg);

    callVM(SetPropertyInfo, ins);
}

typedef bool (*DeletePropertyFn)(JSContext*, HandleValue, HandlePropertyName, bool*);
static const VMFunction DeletePropertyStrictInfo =
    FunctionInfo<DeletePropertyFn>(DeletePropertyJit<true>, "DeletePropertyStrict");
static const VMFunction DeletePropertyNonStrictInfo =
    FunctionInfo<DeletePropertyFn>(DeletePropertyJit<false>, "DeletePropertyNonStrict");

void
CodeGenerator::visitCallDeleteProperty(LCallDeleteProperty* lir)
{
    pushArg(ImmGCPtr(lir->mir()->name()));
    pushArg(ToValue(lir, LCallDeleteProperty::Value));

    if (lir->mir()->strict())
        callVM(DeletePropertyStrictInfo, lir);
    else
        callVM(DeletePropertyNonStrictInfo, lir);
}

typedef bool (*DeleteElementFn)(JSContext*, HandleValue, HandleValue, bool*);
static const VMFunction DeleteElementStrictInfo =
    FunctionInfo<DeleteElementFn>(DeleteElementJit<true>, "DeleteElementStrict");
static const VMFunction DeleteElementNonStrictInfo =
    FunctionInfo<DeleteElementFn>(DeleteElementJit<false>, "DeleteElementNonStrict");

void
CodeGenerator::visitCallDeleteElement(LCallDeleteElement* lir)
{
    pushArg(ToValue(lir, LCallDeleteElement::Index));
    pushArg(ToValue(lir, LCallDeleteElement::Value));

    if (lir->mir()->strict())
        callVM(DeleteElementStrictInfo, lir);
    else
        callVM(DeleteElementNonStrictInfo, lir);
}

void
CodeGenerator::visitSetPropertyCache(LSetPropertyCache* ins)
{
    LiveRegisterSet liveRegs = ins->safepoint()->liveRegs();
    Register objReg = ToRegister(ins->getOperand(0));
    Register temp = ToRegister(ins->temp());
    FloatRegister tempDouble = ToTempFloatRegisterOrInvalid(ins->tempDouble());
    FloatRegister tempF32 = ToTempFloatRegisterOrInvalid(ins->tempFloat32());

    ConstantOrRegister id =
        toConstantOrRegister(ins, LSetPropertyCache::Id, ins->mir()->idval()->type());
    ConstantOrRegister value =
        toConstantOrRegister(ins, LSetPropertyCache::Value, ins->mir()->value()->type());

    addSetPropertyCache(ins, liveRegs, objReg, temp, tempDouble, tempF32,
                        id, value, ins->mir()->strict(), ins->mir()->needsPostBarrier(),
                        ins->mir()->needsTypeBarrier(), ins->mir()->guardHoles());
}

typedef bool (*ThrowFn)(JSContext*, HandleValue);
static const VMFunction ThrowInfoCodeGen = FunctionInfo<ThrowFn>(js::Throw, "Throw");

void
CodeGenerator::visitThrow(LThrow* lir)
{
    pushArg(ToValue(lir, LThrow::Value));
    callVM(ThrowInfoCodeGen, lir);
}

typedef bool (*BitNotFn)(JSContext*, HandleValue, int* p);
static const VMFunction BitNotInfo = FunctionInfo<BitNotFn>(BitNot, "BitNot");

void
CodeGenerator::visitBitNotV(LBitNotV* lir)
{
    pushArg(ToValue(lir, LBitNotV::Input));
    callVM(BitNotInfo, lir);
}

typedef bool (*BitopFn)(JSContext*, HandleValue, HandleValue, int* p);
static const VMFunction BitAndInfo = FunctionInfo<BitopFn>(BitAnd, "BitAnd");
static const VMFunction BitOrInfo = FunctionInfo<BitopFn>(BitOr, "BitOr");
static const VMFunction BitXorInfo = FunctionInfo<BitopFn>(BitXor, "BitXor");
static const VMFunction BitLhsInfo = FunctionInfo<BitopFn>(BitLsh, "BitLsh");
static const VMFunction BitRhsInfo = FunctionInfo<BitopFn>(BitRsh, "BitRsh");

void
CodeGenerator::visitBitOpV(LBitOpV* lir)
{
    pushArg(ToValue(lir, LBitOpV::RhsInput));
    pushArg(ToValue(lir, LBitOpV::LhsInput));

    switch (lir->jsop()) {
      case JSOP_BITAND:
        callVM(BitAndInfo, lir);
        break;
      case JSOP_BITOR:
        callVM(BitOrInfo, lir);
        break;
      case JSOP_BITXOR:
        callVM(BitXorInfo, lir);
        break;
      case JSOP_LSH:
        callVM(BitLhsInfo, lir);
        break;
      case JSOP_RSH:
        callVM(BitRhsInfo, lir);
        break;
      default:
        MOZ_CRASH("unexpected bitop");
    }
}

class OutOfLineTypeOfV : public OutOfLineCodeBase<CodeGenerator>
{
    LTypeOfV* ins_;

  public:
    explicit OutOfLineTypeOfV(LTypeOfV* ins)
      : ins_(ins)
    { }

    void accept(CodeGenerator* codegen) override {
        codegen->visitOutOfLineTypeOfV(this);
    }
    LTypeOfV* ins() const {
        return ins_;
    }
};

void
CodeGenerator::visitTypeOfV(LTypeOfV* lir)
{
    const ValueOperand value = ToValue(lir, LTypeOfV::Input);
    Register output = ToRegister(lir->output());
    Register tag = masm.extractTag(value, output);

    const JSAtomState& names = gen->runtime->names();
    Label done;

    MDefinition* input = lir->mir()->input();

    bool testObject = input->mightBeType(MIRType::Object);
    bool testNumber = input->mightBeType(MIRType::Int32) || input->mightBeType(MIRType::Double);
    bool testBoolean = input->mightBeType(MIRType::Boolean);
    bool testUndefined = input->mightBeType(MIRType::Undefined);
    bool testNull = input->mightBeType(MIRType::Null);
    bool testString = input->mightBeType(MIRType::String);
    bool testSymbol = input->mightBeType(MIRType::Symbol);

    unsigned numTests = unsigned(testObject) + unsigned(testNumber) + unsigned(testBoolean) +
        unsigned(testUndefined) + unsigned(testNull) + unsigned(testString) + unsigned(testSymbol);

    MOZ_ASSERT_IF(!input->emptyResultTypeSet(), numTests > 0);

    OutOfLineTypeOfV* ool = nullptr;
    if (testObject) {
        if (lir->mir()->inputMaybeCallableOrEmulatesUndefined()) {
            // The input may be a callable object (result is "function") or may
            // emulate undefined (result is "undefined"). Use an OOL path.
            ool = new(alloc()) OutOfLineTypeOfV(lir);
            addOutOfLineCode(ool, lir->mir());

            if (numTests > 1)
                masm.branchTestObject(Assembler::Equal, tag, ool->entry());
            else
                masm.jump(ool->entry());
        } else {
            // Input is not callable and does not emulate undefined, so if
            // it's an object the result is always "object".
            Label notObject;
            if (numTests > 1)
                masm.branchTestObject(Assembler::NotEqual, tag, &notObject);
            masm.movePtr(ImmGCPtr(names.object), output);
            if (numTests > 1)
                masm.jump(&done);
            masm.bind(&notObject);
        }
        numTests--;
    }

    if (testNumber) {
        Label notNumber;
        if (numTests > 1)
            masm.branchTestNumber(Assembler::NotEqual, tag, &notNumber);
        masm.movePtr(ImmGCPtr(names.number), output);
        if (numTests > 1)
            masm.jump(&done);
        masm.bind(&notNumber);
        numTests--;
    }

    if (testUndefined) {
        Label notUndefined;
        if (numTests > 1)
            masm.branchTestUndefined(Assembler::NotEqual, tag, &notUndefined);
        masm.movePtr(ImmGCPtr(names.undefined), output);
        if (numTests > 1)
            masm.jump(&done);
        masm.bind(&notUndefined);
        numTests--;
    }

    if (testNull) {
        Label notNull;
        if (numTests > 1)
            masm.branchTestNull(Assembler::NotEqual, tag, &notNull);
        masm.movePtr(ImmGCPtr(names.object), output);
        if (numTests > 1)
            masm.jump(&done);
        masm.bind(&notNull);
        numTests--;
    }

    if (testBoolean) {
        Label notBoolean;
        if (numTests > 1)
            masm.branchTestBoolean(Assembler::NotEqual, tag, &notBoolean);
        masm.movePtr(ImmGCPtr(names.boolean), output);
        if (numTests > 1)
            masm.jump(&done);
        masm.bind(&notBoolean);
        numTests--;
    }

    if (testString) {
        Label notString;
        if (numTests > 1)
            masm.branchTestString(Assembler::NotEqual, tag, &notString);
        masm.movePtr(ImmGCPtr(names.string), output);
        if (numTests > 1)
            masm.jump(&done);
        masm.bind(&notString);
        numTests--;
    }

    if (testSymbol) {
        Label notSymbol;
        if (numTests > 1)
            masm.branchTestSymbol(Assembler::NotEqual, tag, &notSymbol);
        masm.movePtr(ImmGCPtr(names.symbol), output);
        if (numTests > 1)
            masm.jump(&done);
        masm.bind(&notSymbol);
        numTests--;
    }

    MOZ_ASSERT(numTests == 0);

    masm.bind(&done);
    if (ool)
        masm.bind(ool->rejoin());
}

void
CodeGenerator::visitOutOfLineTypeOfV(OutOfLineTypeOfV* ool)
{
    LTypeOfV* ins = ool->ins();
    const JSAtomState& names = gen->runtime->names();

    ValueOperand input = ToValue(ins, LTypeOfV::Input);
    Register temp = ToTempUnboxRegister(ins->tempToUnbox());
    Register output = ToRegister(ins->output());

    Register obj = masm.extractObject(input, temp);

    Label slowCheck, isObject, isCallable, isUndefined, done;
    masm.typeOfObject(obj, output, &slowCheck, &isObject, &isCallable, &isUndefined);

    masm.bind(&isCallable);
    masm.movePtr(ImmGCPtr(names.function), output);
    masm.jump(ool->rejoin());

    masm.bind(&isUndefined);
    masm.movePtr(ImmGCPtr(names.undefined), output);
    masm.jump(ool->rejoin());

    masm.bind(&isObject);
    masm.movePtr(ImmGCPtr(names.object), output);
    masm.jump(ool->rejoin());

    masm.bind(&slowCheck);

    saveVolatile(output);
    masm.setupUnalignedABICall(output);
    masm.passABIArg(obj);
    masm.movePtr(ImmPtr(gen->runtime), output);
    masm.passABIArg(output);
    masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, jit::TypeOfObject));
    masm.storeCallPointerResult(output);
    restoreVolatile(output);

    masm.jump(ool->rejoin());
}

typedef JSObject* (*ToAsyncFn)(JSContext*, HandleFunction);
static const VMFunction ToAsyncInfo = FunctionInfo<ToAsyncFn>(js::WrapAsyncFunction, "ToAsync");

void
CodeGenerator::visitToAsync(LToAsync* lir)
{
    pushArg(ToRegister(lir->unwrapped()));
    callVM(ToAsyncInfo, lir);
}

typedef JSObject* (*ToAsyncGenFn)(JSContext*, HandleFunction);
static const VMFunction ToAsyncGenInfo =
    FunctionInfo<ToAsyncGenFn>(js::WrapAsyncGenerator, "ToAsyncGen");

void
CodeGenerator::visitToAsyncGen(LToAsyncGen* lir)
{
    pushArg(ToRegister(lir->unwrapped()));
    callVM(ToAsyncGenInfo, lir);
}

typedef JSObject* (*ToAsyncIterFn)(JSContext*, HandleObject, HandleValue);
static const VMFunction ToAsyncIterInfo =
    FunctionInfo<ToAsyncIterFn>(js::CreateAsyncFromSyncIterator, "ToAsyncIter");

void
CodeGenerator::visitToAsyncIter(LToAsyncIter* lir)
{
    pushArg(ToValue(lir, LToAsyncIter::NextMethodIndex));
    pushArg(ToRegister(lir->iterator()));
    callVM(ToAsyncIterInfo, lir);
}

typedef bool (*ToIdFn)(JSContext*, HandleValue, MutableHandleValue);
static const VMFunction ToIdInfo = FunctionInfo<ToIdFn>(ToIdOperation, "ToIdOperation");

void
CodeGenerator::visitToIdV(LToIdV* lir)
{
    Label notInt32;
    FloatRegister temp = ToFloatRegister(lir->tempFloat());
    const ValueOperand out = ToOutValue(lir);
    ValueOperand input = ToValue(lir, LToIdV::Input);

    OutOfLineCode* ool = oolCallVM(ToIdInfo, lir,
                                   ArgList(ToValue(lir, LToIdV::Input)),
                                   StoreValueTo(out));

    Register tag = masm.extractTag(input, out.scratchReg());

    masm.branchTestInt32(Assembler::NotEqual, tag, &notInt32);
    masm.moveValue(input, out);
    masm.jump(ool->rejoin());

    masm.bind(&notInt32);
    masm.branchTestDouble(Assembler::NotEqual, tag, ool->entry());
    masm.unboxDouble(input, temp);
    masm.convertDoubleToInt32(temp, out.scratchReg(), ool->entry(), true);
    masm.tagValue(JSVAL_TYPE_INT32, out.scratchReg(), out);

    masm.bind(ool->rejoin());
}

template<typename T>
void
CodeGenerator::emitLoadElementT(LLoadElementT* lir, const T& source)
{
    if (LIRGenerator::allowTypedElementHoleCheck()) {
        if (lir->mir()->needsHoleCheck()) {
            Label bail;
            masm.branchTestMagic(Assembler::Equal, source, &bail);
            bailoutFrom(&bail, lir->snapshot());
        }
    } else {
        MOZ_ASSERT(!lir->mir()->needsHoleCheck());
    }

    AnyRegister output = ToAnyRegister(lir->output());
    if (lir->mir()->loadDoubles())
        masm.loadDouble(source, output.fpu());
    else
        masm.loadUnboxedValue(source, lir->mir()->type(), output);
}

void
CodeGenerator::visitLoadElementT(LLoadElementT* lir)
{
    Register elements = ToRegister(lir->elements());
    const LAllocation* index = lir->index();
    if (index->isConstant()) {
        int32_t offset = ToInt32(index) * sizeof(js::Value) + lir->mir()->offsetAdjustment();
        emitLoadElementT(lir, Address(elements, offset));
    } else {
        emitLoadElementT(lir, BaseIndex(elements, ToRegister(index), TimesEight,
                                        lir->mir()->offsetAdjustment()));
    }
}

void
CodeGenerator::visitLoadElementV(LLoadElementV* load)
{
    Register elements = ToRegister(load->elements());
    const ValueOperand out = ToOutValue(load);

    if (load->index()->isConstant()) {
        NativeObject::elementsSizeMustNotOverflow();
        int32_t offset = ToInt32(load->index()) * sizeof(Value) + load->mir()->offsetAdjustment();
        masm.loadValue(Address(elements, offset), out);
    } else {
        masm.loadValue(BaseObjectElementIndex(elements, ToRegister(load->index()),
                                              load->mir()->offsetAdjustment()), out);
    }

    if (load->mir()->needsHoleCheck()) {
        Label testMagic;
        masm.branchTestMagic(Assembler::Equal, out, &testMagic);
        bailoutFrom(&testMagic, load->snapshot());
    }
}

void
CodeGenerator::visitLoadElementHole(LLoadElementHole* lir)
{
    Register elements = ToRegister(lir->elements());
    Register index = ToRegister(lir->index());
    Register initLength = ToRegister(lir->initLength());
    const ValueOperand out = ToOutValue(lir);

    const MLoadElementHole* mir = lir->mir();

    // If the index is out of bounds, load |undefined|. Otherwise, load the
    // value.
    Label outOfBounds, done;
    masm.spectreBoundsCheck32(index, initLength, out.scratchReg(), &outOfBounds);

    masm.loadValue(BaseObjectElementIndex(elements, index), out);

    // If a hole check is needed, and the value wasn't a hole, we're done.
    // Otherwise, we'll load undefined.
    if (lir->mir()->needsHoleCheck()) {
        masm.branchTestMagic(Assembler::NotEqual, out, &done);
        masm.moveValue(UndefinedValue(), out);
    }
    masm.jump(&done);

    masm.bind(&outOfBounds);
    if (mir->needsNegativeIntCheck()) {
        Label negative;
        masm.branch32(Assembler::LessThan, index, Imm32(0), &negative);
        bailoutFrom(&negative, lir->snapshot());
    }
    masm.moveValue(UndefinedValue(), out);

    masm.bind(&done);
}

void
CodeGenerator::visitLoadUnboxedPointerV(LLoadUnboxedPointerV* lir)
{
    Register elements = ToRegister(lir->elements());
    const ValueOperand out = ToOutValue(lir);

    if (lir->index()->isConstant()) {
        int32_t offset = ToInt32(lir->index()) * sizeof(uintptr_t) + lir->mir()->offsetAdjustment();
        masm.loadPtr(Address(elements, offset), out.scratchReg());
    } else {
        masm.loadPtr(BaseIndex(elements, ToRegister(lir->index()), ScalePointer,
                               lir->mir()->offsetAdjustment()), out.scratchReg());
    }

    Label notNull, done;
    masm.branchPtr(Assembler::NotEqual, out.scratchReg(), ImmWord(0), &notNull);

    masm.moveValue(NullValue(), out);
    masm.jump(&done);

    masm.bind(&notNull);
    masm.tagValue(JSVAL_TYPE_OBJECT, out.scratchReg(), out);

    masm.bind(&done);
}

void
CodeGenerator::visitLoadUnboxedPointerT(LLoadUnboxedPointerT* lir)
{
    Register elements = ToRegister(lir->elements());
    const LAllocation* index = lir->index();
    Register out = ToRegister(lir->output());

    bool bailOnNull;
    int32_t offsetAdjustment;
    if (lir->mir()->isLoadUnboxedObjectOrNull()) {
        bailOnNull = lir->mir()->toLoadUnboxedObjectOrNull()->nullBehavior() ==
                     MLoadUnboxedObjectOrNull::BailOnNull;
        offsetAdjustment = lir->mir()->toLoadUnboxedObjectOrNull()->offsetAdjustment();
    } else if (lir->mir()->isLoadUnboxedString()) {
        bailOnNull = false;
        offsetAdjustment = lir->mir()->toLoadUnboxedString()->offsetAdjustment();
    } else {
        MOZ_CRASH();
    }

    if (index->isConstant()) {
        Address source(elements, ToInt32(index) * sizeof(uintptr_t) + offsetAdjustment);
        masm.loadPtr(source, out);
    } else {
        BaseIndex source(elements, ToRegister(index), ScalePointer, offsetAdjustment);
        masm.loadPtr(source, out);
    }

    if (bailOnNull) {
        Label bail;
        masm.branchTestPtr(Assembler::Zero, out, out, &bail);
        bailoutFrom(&bail, lir->snapshot());
    }
}

void
CodeGenerator::visitUnboxObjectOrNull(LUnboxObjectOrNull* lir)
{
    Register obj = ToRegister(lir->input());

    if (lir->mir()->fallible()) {
        Label bail;
        masm.branchTestPtr(Assembler::Zero, obj, obj, &bail);
        bailoutFrom(&bail, lir->snapshot());
    }
}

void
CodeGenerator::visitLoadUnboxedScalar(LLoadUnboxedScalar* lir)
{
    Register elements = ToRegister(lir->elements());
    Register temp = lir->temp()->isBogusTemp() ? InvalidReg : ToRegister(lir->temp());
    AnyRegister out = ToAnyRegister(lir->output());

    const MLoadUnboxedScalar* mir = lir->mir();

    Scalar::Type readType = mir->readType();
    unsigned numElems = mir->numElems();

    int width = Scalar::byteSize(mir->storageType());
    bool canonicalizeDouble = mir->canonicalizeDoubles();

    Label fail;
    if (lir->index()->isConstant()) {
        Address source(elements, ToInt32(lir->index()) * width + mir->offsetAdjustment());
        masm.loadFromTypedArray(readType, source, out, temp, &fail, canonicalizeDouble, numElems);
    } else {
        BaseIndex source(elements, ToRegister(lir->index()), ScaleFromElemWidth(width),
                         mir->offsetAdjustment());
        masm.loadFromTypedArray(readType, source, out, temp, &fail, canonicalizeDouble, numElems);
    }

    if (fail.used())
        bailoutFrom(&fail, lir->snapshot());
}

void
CodeGenerator::visitLoadTypedArrayElementHole(LLoadTypedArrayElementHole* lir)
{
    Register object = ToRegister(lir->object());
    const ValueOperand out = ToOutValue(lir);

    // Load the length.
    Register scratch = out.scratchReg();
    Register scratch2 = ToRegister(lir->temp());
    Register index = ToRegister(lir->index());
    masm.unboxInt32(Address(object, TypedArrayObject::lengthOffset()), scratch);

    // Load undefined if index >= length.
    Label outOfBounds, done;
    masm.spectreBoundsCheck32(index, scratch, scratch2, &outOfBounds);

    // Load the elements vector.
    masm.loadPtr(Address(object, TypedArrayObject::dataOffset()), scratch);

    Scalar::Type arrayType = lir->mir()->arrayType();
    int width = Scalar::byteSize(arrayType);
    Label fail;
    BaseIndex source(scratch, index, ScaleFromElemWidth(width));
    masm.loadFromTypedArray(arrayType, source, out, lir->mir()->allowDouble(),
                            out.scratchReg(), &fail);
    masm.jump(&done);

    masm.bind(&outOfBounds);
    masm.moveValue(UndefinedValue(), out);

    if (fail.used())
        bailoutFrom(&fail, lir->snapshot());

    masm.bind(&done);
}

template <SwitchTableType tableType>
class OutOfLineSwitch : public OutOfLineCodeBase<CodeGenerator>
{
    using LabelsVector = Vector<Label, 0, JitAllocPolicy>;
    using CodeLabelsVector = Vector<CodeLabel, 0, JitAllocPolicy>;
    LabelsVector labels_;
    CodeLabelsVector codeLabels_;
    CodeLabel start_;
    bool isOutOfLine_;

    void accept(CodeGenerator* codegen) override {
        codegen->visitOutOfLineSwitch(this);
    }

  public:
    explicit OutOfLineSwitch(TempAllocator& alloc)
      : labels_(alloc),
        codeLabels_(alloc),
        isOutOfLine_(false)
    {}

    CodeLabel* start() {
        return &start_;
    }

    CodeLabelsVector& codeLabels() {
        return codeLabels_;
    }
    LabelsVector& labels() {
        return labels_;
    }

    void jumpToCodeEntries(MacroAssembler& masm, Register index, Register temp) {
        Register base;
        if (tableType == SwitchTableType::Inline) {
#if defined(JS_CODEGEN_ARM)
            base = ::js::jit::pc;
#else
            MOZ_CRASH("NYI: SwitchTableType::Inline");
#endif
        } else {
#if defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_ARM64)
            MOZ_CRASH("NYI: SwitchTableType::OutOfLine");
#else
            masm.mov(start(), temp);
            base = temp;
#endif
        }
        BaseIndex jumpTarget(base, index, ScalePointer);
        masm.branchToComputedAddress(jumpTarget);
    }

    // Register an entry in the switch table.
    void addTableEntry(MacroAssembler& masm) {
        if ((!isOutOfLine_ && tableType == SwitchTableType::Inline) ||
            (isOutOfLine_ && tableType == SwitchTableType::OutOfLine))
        {
            CodeLabel cl;
            masm.writeCodePointer(&cl);
            masm.propagateOOM(codeLabels_.append(mozilla::Move(cl)));
        }
    }
    // Register the code, to which the table will jump to.
    void addCodeEntry(MacroAssembler& masm) {
        Label entry;
        masm.bind(&entry);
        masm.propagateOOM(labels_.append(mozilla::Move(entry)));
    }

    void setOutOfLine() {
        isOutOfLine_ = true;
    }
};

template <SwitchTableType tableType>
void
CodeGenerator::visitOutOfLineSwitch(OutOfLineSwitch<tableType>* jumpTable)
{
    jumpTable->setOutOfLine();
    if (tableType == SwitchTableType::OutOfLine) {
#if defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_ARM64)
        MOZ_CRASH("NYI: SwitchTableType::OutOfLine");
#elif defined(JS_CODEGEN_NONE)
        MOZ_CRASH();
#else
        masm.haltingAlign(sizeof(void*));
        masm.bind(jumpTable->start());
        masm.addCodeLabel(*jumpTable->start());
#endif
    }

    // Add table entries if the table is inlined.
    auto& labels = jumpTable->labels();
    for (size_t i = 0, e = labels.length(); i < e; i++)
        jumpTable->addTableEntry(masm);

    auto& codeLabels = jumpTable->codeLabels();
    for (size_t i = 0, e = codeLabels.length(); i < e; i++) {
        // The entries of the jump table need to be absolute addresses and thus
        // must be patched after codegen is finished.
        auto& cl = codeLabels[i];
        cl.target()->bind(labels[i].offset());
        masm.addCodeLabel(cl);
    }
}

template void CodeGenerator::visitOutOfLineSwitch(OutOfLineSwitch<SwitchTableType::Inline>* jumpTable);
template void CodeGenerator::visitOutOfLineSwitch(OutOfLineSwitch<SwitchTableType::OutOfLine>* jumpTable);

void
CodeGenerator::visitLoadElementFromStateV(LLoadElementFromStateV* lir)
{
    Register index = ToRegister(lir->index());
    Register temp0 = ToRegister(lir->temp0());
#ifdef JS_NUNBOX32
    Register temp1 = ToRegister(lir->temp1());
#endif
    FloatRegister tempD = ToFloatRegister(lir->tempD());
    ValueOperand out = ToOutValue(lir);

    // For each element, load it and box it.
    MArgumentState* array = lir->array()->toArgumentState();
    Label join;

    // Jump to the code which is loading the element, based on its index.
#if defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_ARM64)
    auto* jumpTable = new (alloc()) OutOfLineSwitch<SwitchTableType::Inline>(alloc());
#else
    auto* jumpTable = new (alloc()) OutOfLineSwitch<SwitchTableType::OutOfLine>(alloc());
#endif

    {
#if defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_ARM64)
        // Inhibit pools within the following sequence because we are indexing into
        // a pc relative table. The region will have one instruction for ma_ldr, one
        // for breakpoint, and each table case takes one word.
        AutoForbidPools afp(&masm, 1 + 1 + array->numElements());
#endif
        jumpTable->jumpToCodeEntries(masm, index, temp0);

        // Add table entries if the table is inlined.
        for (size_t i = 0, e = array->numElements(); i < e; i++)
            jumpTable->addTableEntry(masm);
    }

    // Add inlined code for loading arguments from where they are allocated.
    for (size_t i = 0, e = array->numElements(); i < e; i++) {
        MDefinition* elem = array->getElement(i);
        ConstantOrRegister input;

        jumpTable->addCodeEntry(masm);
        Register typeReg = Register::Invalid();
        const LAllocation* a = lir->getOperand(1 + BOX_PIECES * i);
        if (a->isBogus()) {
            if (elem->type() == MIRType::Null) {
                input = NullValue();
            } else if (elem->type() == MIRType::Undefined) {
                input = UndefinedValue();
            } else if (elem->isConstant() && elem->isEmittedAtUses()) {
                input = elem->toConstant()->toJSValue();
            } else {
                MOZ_CRASH("Unsupported element constant allocation.");
            }
        } else if (a->isMemory()) {
            if (elem->type() == MIRType::Double) {
                masm.loadDouble(ToAddress(a), tempD);
                input = TypedOrValueRegister(elem->type(), AnyRegister(tempD));
            } else if (elem->type() == MIRType::Value) {
                typeReg = temp0;
                masm.loadPtr(ToAddress(a), temp0);
#ifdef JS_PUNBOX64
                input = TypedOrValueRegister(ValueOperand(temp0));
#endif
            } else {
                typeReg = temp0;
                size_t width = StackSlotAllocator::width(LDefinition::TypeFrom(elem->type()));
                if (width == 4)
                    masm.load32(ToAddress(a), temp0);
                else if (width == 8)
                    masm.loadPtr(ToAddress(a), temp0);
                else
                    MOZ_CRASH("Unsupported load size");
                input = TypedOrValueRegister(elem->type(), AnyRegister(typeReg));
            }
        } else if (a->isGeneralReg()) {
            typeReg = ToRegister(a);
            input = TypedOrValueRegister(elem->type(), AnyRegister(typeReg));
#ifdef JS_PUNBOX64
            if (elem->type() != MIRType::Value)
                input = TypedOrValueRegister(elem->type(), AnyRegister(typeReg));
            else
                input = TypedOrValueRegister(ValueOperand(typeReg));
#else
            if (elem->type() != MIRType::Value)
                input = TypedOrValueRegister(elem->type(), AnyRegister(typeReg));
#endif
        } else if (a->isFloatReg()) {
            input = TypedOrValueRegister(elem->type(), AnyRegister(ToFloatRegister(a)));
        } else if (a->isConstantValue()) {
            input = a->toConstant()->toJSValue();
        } else {
            MOZ_CRASH("Unsupported element allocation.");
        }

#ifdef JS_NUNBOX32
        if (elem->type() == MIRType::Value) {
            static_assert(TYPE_INDEX == 0, "Unexpected type allocation index");
            static_assert(PAYLOAD_INDEX == 1, "Unexpected payload allocation index");
            const LAllocation* a1 = lir->getOperand(1 + BOX_PIECES * i + 1);
            MOZ_ASSERT(!a1->isBogus());
            MOZ_ASSERT(typeReg != Register::Invalid());
            if (a1->isMemory()) {
                masm.loadPtr(ToAddress(a1), temp1);
                input = TypedOrValueRegister(ValueOperand(typeReg, temp1));
            } else if (a1->isGeneralReg()) {
                input = TypedOrValueRegister(ValueOperand(typeReg, ToRegister(a1)));
            } else {
                MOZ_CRASH("Unsupported Value allocation.");
            }
        } else {
            MOZ_ASSERT(lir->getOperand(1 + BOX_PIECES * i + 1)->isBogus());
        }
#endif
        masm.moveValue(input, out);

        // For the last entry, fall-through.
        if (i + 1 < e)
            masm.jump(&join);
    }

    addOutOfLineCode(jumpTable, lir->mir());
    masm.bind(&join);
}

template <typename T>
static inline void
StoreToTypedArray(MacroAssembler& masm, Scalar::Type writeType, const LAllocation* value,
                  const T& dest, unsigned numElems = 0)
{
    if (Scalar::isSimdType(writeType) ||
        writeType == Scalar::Float32 ||
        writeType == Scalar::Float64)
    {
        masm.storeToTypedFloatArray(writeType, ToFloatRegister(value), dest, numElems);
    } else {
        if (value->isConstant())
            masm.storeToTypedIntArray(writeType, Imm32(ToInt32(value)), dest);
        else
            masm.storeToTypedIntArray(writeType, ToRegister(value), dest);
    }
}

void
CodeGenerator::visitStoreUnboxedScalar(LStoreUnboxedScalar* lir)
{
    Register elements = ToRegister(lir->elements());
    const LAllocation* value = lir->value();

    const MStoreUnboxedScalar* mir = lir->mir();

    Scalar::Type writeType = mir->writeType();
    unsigned numElems = mir->numElems();

    int width = Scalar::byteSize(mir->storageType());

    if (lir->index()->isConstant()) {
        Address dest(elements, ToInt32(lir->index()) * width + mir->offsetAdjustment());
        StoreToTypedArray(masm, writeType, value, dest, numElems);
    } else {
        BaseIndex dest(elements, ToRegister(lir->index()), ScaleFromElemWidth(width),
                       mir->offsetAdjustment());
        StoreToTypedArray(masm, writeType, value, dest, numElems);
    }
}

void
CodeGenerator::visitStoreTypedArrayElementHole(LStoreTypedArrayElementHole* lir)
{
    Register elements = ToRegister(lir->elements());
    const LAllocation* value = lir->value();

    Scalar::Type arrayType = lir->mir()->arrayType();
    int width = Scalar::byteSize(arrayType);

    Register index = ToRegister(lir->index());
    const LAllocation* length = lir->length();
    Register spectreTemp = ToTempRegisterOrInvalid(lir->spectreTemp());

    Label skip;
    if (length->isRegister())
        masm.spectreBoundsCheck32(index, ToRegister(length), spectreTemp, &skip);
    else
        masm.spectreBoundsCheck32(index, ToAddress(length), spectreTemp, &skip);

    BaseIndex dest(elements, index, ScaleFromElemWidth(width));
    StoreToTypedArray(masm, arrayType, value, dest);

    masm.bind(&skip);
}

void
CodeGenerator::visitAtomicIsLockFree(LAtomicIsLockFree* lir)
{
    Register value = ToRegister(lir->value());
    Register output = ToRegister(lir->output());

    // Keep this in sync with isLockfreeJS() in jit/AtomicOperations.h.
    MOZ_ASSERT(AtomicOperations::isLockfreeJS(1));  // Implementation artifact
    MOZ_ASSERT(AtomicOperations::isLockfreeJS(2));  // Implementation artifact
    MOZ_ASSERT(AtomicOperations::isLockfreeJS(4));  // Spec requirement
    MOZ_ASSERT(!AtomicOperations::isLockfreeJS(8)); // Implementation invariant, for now

    Label Ldone, Lfailed;
    masm.move32(Imm32(1), output);
    masm.branch32(Assembler::Equal, value, Imm32(4), &Ldone);
    masm.branch32(Assembler::Equal, value, Imm32(2), &Ldone);
    masm.branch32(Assembler::Equal, value, Imm32(1), &Ldone);
    masm.move32(Imm32(0), output);
    masm.bind(&Ldone);
}

void
CodeGenerator::visitGuardSharedTypedArray(LGuardSharedTypedArray* guard)
{
    Register obj = ToRegister(guard->input());
    Register tmp = ToRegister(guard->tempInt());

    // The shared-memory flag is a bit in the ObjectElements header
    // that is set if the TypedArray is mapping a SharedArrayBuffer.
    // The flag is set at construction and does not change subsequently.
    masm.loadPtr(Address(obj, TypedArrayObject::offsetOfElements()), tmp);
    masm.load32(Address(tmp, ObjectElements::offsetOfFlags()), tmp);
    bailoutTest32(Assembler::Zero, tmp, Imm32(ObjectElements::SHARED_MEMORY), guard->snapshot());
}

void
CodeGenerator::visitClampIToUint8(LClampIToUint8* lir)
{
    Register output = ToRegister(lir->output());
    MOZ_ASSERT(output == ToRegister(lir->input()));
    masm.clampIntToUint8(output);
}

void
CodeGenerator::visitClampDToUint8(LClampDToUint8* lir)
{
    FloatRegister input = ToFloatRegister(lir->input());
    Register output = ToRegister(lir->output());
    masm.clampDoubleToUint8(input, output);
}

void
CodeGenerator::visitClampVToUint8(LClampVToUint8* lir)
{
    ValueOperand operand = ToValue(lir, LClampVToUint8::Input);
    FloatRegister tempFloat = ToFloatRegister(lir->tempFloat());
    Register output = ToRegister(lir->output());
    MDefinition* input = lir->mir()->input();

    Label* stringEntry;
    Label* stringRejoin;
    if (input->mightBeType(MIRType::String)) {
        OutOfLineCode* oolString = oolCallVM(StringToNumberInfo, lir, ArgList(output),
                                             StoreFloatRegisterTo(tempFloat));
        stringEntry = oolString->entry();
        stringRejoin = oolString->rejoin();
    } else {
        stringEntry = nullptr;
        stringRejoin = nullptr;
    }

    Label fails;
    masm.clampValueToUint8(operand, input,
                           stringEntry, stringRejoin,
                           output, tempFloat, output, &fails);

    bailoutFrom(&fails, lir->snapshot());
}

void
CodeGenerator::visitInCache(LInCache* ins)
{
    LiveRegisterSet liveRegs = ins->safepoint()->liveRegs();

    ConstantOrRegister key = toConstantOrRegister(ins, LInCache::LHS, ins->mir()->key()->type());
    Register object = ToRegister(ins->rhs());
    Register output = ToRegister(ins->output());
    Register temp = ToRegister(ins->temp());

    IonInIC cache(liveRegs, key, object, output, temp);
    addIC(ins, allocateIC(cache));
}

typedef bool (*OperatorInIFn)(JSContext*, uint32_t, HandleObject, bool*);
static const VMFunction OperatorInIInfo = FunctionInfo<OperatorInIFn>(OperatorInI, "OperatorInI");

void
CodeGenerator::visitInArray(LInArray* lir)
{
    const MInArray* mir = lir->mir();
    Register elements = ToRegister(lir->elements());
    Register initLength = ToRegister(lir->initLength());
    Register output = ToRegister(lir->output());

    // When the array is not packed we need to do a hole check in addition to the bounds check.
    Label falseBranch, done, trueBranch;

    OutOfLineCode* ool = nullptr;
    Label* failedInitLength = &falseBranch;

    if (lir->index()->isConstant()) {
        int32_t index = ToInt32(lir->index());

        MOZ_ASSERT_IF(index < 0, mir->needsNegativeIntCheck());
        if (mir->needsNegativeIntCheck()) {
            ool = oolCallVM(OperatorInIInfo, lir,
                            ArgList(Imm32(index), ToRegister(lir->object())),
                            StoreRegisterTo(output));
            failedInitLength = ool->entry();
        }

        masm.branch32(Assembler::BelowOrEqual, initLength, Imm32(index), failedInitLength);
        if (mir->needsHoleCheck()) {
            NativeObject::elementsSizeMustNotOverflow();
            Address address = Address(elements, index * sizeof(Value));
            masm.branchTestMagic(Assembler::Equal, address, &falseBranch);
        }
    } else {
        Label negativeIntCheck;
        Register index = ToRegister(lir->index());

        if (mir->needsNegativeIntCheck())
            failedInitLength = &negativeIntCheck;

        masm.branch32(Assembler::BelowOrEqual, initLength, index, failedInitLength);
        if (mir->needsHoleCheck()) {
            BaseIndex address = BaseIndex(elements, ToRegister(lir->index()), TimesEight);
            masm.branchTestMagic(Assembler::Equal, address, &falseBranch);
        }
        masm.jump(&trueBranch);

        if (mir->needsNegativeIntCheck()) {
            masm.bind(&negativeIntCheck);
            ool = oolCallVM(OperatorInIInfo, lir,
                            ArgList(index, ToRegister(lir->object())),
                            StoreRegisterTo(output));

            masm.branch32(Assembler::LessThan, index, Imm32(0), ool->entry());
            masm.jump(&falseBranch);
        }
    }

    masm.bind(&trueBranch);
    masm.move32(Imm32(1), output);
    masm.jump(&done);

    masm.bind(&falseBranch);
    masm.move32(Imm32(0), output);
    masm.bind(&done);

    if (ool)
        masm.bind(ool->rejoin());
}

void
CodeGenerator::visitInstanceOfO(LInstanceOfO* ins)
{
    emitInstanceOf(ins, ins->mir()->prototypeObject());
}

void
CodeGenerator::visitInstanceOfV(LInstanceOfV* ins)
{
    emitInstanceOf(ins, ins->mir()->prototypeObject());
}

// Wrap IsDelegateOfObject, which takes a JSObject*, not a HandleObject
static bool
IsDelegateObject(JSContext* cx, HandleObject protoObj, HandleObject obj, bool* res)
{
    return IsDelegateOfObject(cx, protoObj, obj, res);
}

typedef bool (*IsDelegateObjectFn)(JSContext*, HandleObject, HandleObject, bool*);
static const VMFunction IsDelegateObjectInfo =
    FunctionInfo<IsDelegateObjectFn>(IsDelegateObject, "IsDelegateObject");

void
CodeGenerator::emitInstanceOf(LInstruction* ins, JSObject* prototypeObject)
{
    // This path implements fun_hasInstance when the function's prototype is
    // known to be prototypeObject.

    Label done;
    Register output = ToRegister(ins->getDef(0));

    // If the lhs is a primitive, the result is false.
    Register objReg;
    if (ins->isInstanceOfV()) {
        Label isObject;
        ValueOperand lhsValue = ToValue(ins, LInstanceOfV::LHS);
        masm.branchTestObject(Assembler::Equal, lhsValue, &isObject);
        masm.mov(ImmWord(0), output);
        masm.jump(&done);
        masm.bind(&isObject);
        objReg = masm.extractObject(lhsValue, output);
    } else {
        objReg = ToRegister(ins->toInstanceOfO()->lhs());
    }

    // Crawl the lhs's prototype chain in a loop to search for prototypeObject.
    // This follows the main loop of js::IsDelegate, though additionally breaks
    // out of the loop on Proxy::LazyProto.

    // Load the lhs's prototype.
    masm.loadObjProto(objReg, output);

    Label testLazy;
    {
        Label loopPrototypeChain;
        masm.bind(&loopPrototypeChain);

        // Test for the target prototype object.
        Label notPrototypeObject;
        masm.branchPtr(Assembler::NotEqual, output, ImmGCPtr(prototypeObject), &notPrototypeObject);
        masm.mov(ImmWord(1), output);
        masm.jump(&done);
        masm.bind(&notPrototypeObject);

        MOZ_ASSERT(uintptr_t(TaggedProto::LazyProto) == 1);

        // Test for nullptr or Proxy::LazyProto
        masm.branchPtr(Assembler::BelowOrEqual, output, ImmWord(1), &testLazy);

        // Load the current object's prototype.
        masm.loadObjProto(output, output);

        masm.jump(&loopPrototypeChain);
    }

    // Make a VM call if an object with a lazy proto was found on the prototype
    // chain. This currently occurs only for cross compartment wrappers, which
    // we do not expect to be compared with non-wrapper functions from this
    // compartment. Otherwise, we stopped on a nullptr prototype and the output
    // register is already correct.

    OutOfLineCode* ool = oolCallVM(IsDelegateObjectInfo, ins,
                                   ArgList(ImmGCPtr(prototypeObject), objReg),
                                   StoreRegisterTo(output));

    // Regenerate the original lhs object for the VM call.
    Label regenerate, *lazyEntry;
    if (objReg != output) {
        lazyEntry = ool->entry();
    } else {
        masm.bind(&regenerate);
        lazyEntry = &regenerate;
        if (ins->isInstanceOfV()) {
            ValueOperand lhsValue = ToValue(ins, LInstanceOfV::LHS);
            objReg = masm.extractObject(lhsValue, output);
        } else {
            objReg = ToRegister(ins->toInstanceOfO()->lhs());
        }
        MOZ_ASSERT(objReg == output);
        masm.jump(ool->entry());
    }

    masm.bind(&testLazy);
    masm.branchPtr(Assembler::Equal, output, ImmWord(1), lazyEntry);

    masm.bind(&done);
    masm.bind(ool->rejoin());
}

typedef bool (*HasInstanceFn)(JSContext*, HandleObject, HandleValue, bool*);
static const VMFunction HasInstanceInfo = FunctionInfo<HasInstanceFn>(js::HasInstance, "HasInstance");

void
CodeGenerator::visitInstanceOfCache(LInstanceOfCache* ins)
{
    // The Lowering ensures that RHS is an object, and that LHS is a value.
    LiveRegisterSet liveRegs = ins->safepoint()->liveRegs();
    TypedOrValueRegister lhs = TypedOrValueRegister(ToValue(ins, LInstanceOfCache::LHS));
    Register rhs = ToRegister(ins->rhs());
    Register output = ToRegister(ins->output());

    IonInstanceOfIC ic(liveRegs, lhs, rhs, output);
    addIC(ins, allocateIC(ic));
}

void
CodeGenerator::visitGetDOMProperty(LGetDOMProperty* ins)
{
    const Register JSContextReg = ToRegister(ins->getJSContextReg());
    const Register ObjectReg = ToRegister(ins->getObjectReg());
    const Register PrivateReg = ToRegister(ins->getPrivReg());
    const Register ValueReg = ToRegister(ins->getValueReg());

    Label haveValue;
    if (ins->mir()->valueMayBeInSlot()) {
        size_t slot = ins->mir()->domMemberSlotIndex();
        // It's a bit annoying to redo these slot calculations, which duplcate
        // LSlots and a few other things like that, but I'm not sure there's a
        // way to reuse those here.
        //
        // If this ever gets fixed to work with proxies (by not assuming that
        // reserved slot indices, which is what domMemberSlotIndex() returns,
        // match fixed slot indices), we can reenable MGetDOMProperty for
        // proxies in IonBuilder.
        if (slot < NativeObject::MAX_FIXED_SLOTS) {
            masm.loadValue(Address(ObjectReg, NativeObject::getFixedSlotOffset(slot)),
                           JSReturnOperand);
        } else {
            // It's a dynamic slot.
            slot -= NativeObject::MAX_FIXED_SLOTS;
            // Use PrivateReg as a scratch register for the slots pointer.
            masm.loadPtr(Address(ObjectReg, NativeObject::offsetOfSlots()),
                         PrivateReg);
            masm.loadValue(Address(PrivateReg, slot*sizeof(js::Value)),
                           JSReturnOperand);
        }
        masm.branchTestUndefined(Assembler::NotEqual, JSReturnOperand, &haveValue);
    }

    DebugOnly<uint32_t> initialStack = masm.framePushed();

    masm.checkStackAlignment();

    // Make space for the outparam.  Pre-initialize it to UndefinedValue so we
    // can trace it at GC time.
    masm.Push(UndefinedValue());
    // We pass the pointer to our out param as an instance of
    // JSJitGetterCallArgs, since on the binary level it's the same thing.
    JS_STATIC_ASSERT(sizeof(JSJitGetterCallArgs) == sizeof(Value*));
    masm.moveStackPtrTo(ValueReg);

    masm.Push(ObjectReg);

    LoadDOMPrivate(masm, ObjectReg, PrivateReg, ins->mir()->objectKind());

    // Rooting will happen at GC time.
    masm.moveStackPtrTo(ObjectReg);

    uint32_t safepointOffset = masm.buildFakeExitFrame(JSContextReg);
    masm.loadJSContext(JSContextReg);
    masm.enterFakeExitFrame(JSContextReg, JSContextReg, ExitFrameType::IonDOMGetter);

    markSafepointAt(safepointOffset, ins);

    masm.setupUnalignedABICall(JSContextReg);
    masm.loadJSContext(JSContextReg);
    masm.passABIArg(JSContextReg);
    masm.passABIArg(ObjectReg);
    masm.passABIArg(PrivateReg);
    masm.passABIArg(ValueReg);
    masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, ins->mir()->fun()), MoveOp::GENERAL,
                     CheckUnsafeCallWithABI::DontCheckHasExitFrame);

    if (ins->mir()->isInfallible()) {
        masm.loadValue(Address(masm.getStackPointer(), IonDOMExitFrameLayout::offsetOfResult()),
                       JSReturnOperand);
    } else {
        masm.branchIfFalseBool(ReturnReg, masm.exceptionLabel());

        masm.loadValue(Address(masm.getStackPointer(), IonDOMExitFrameLayout::offsetOfResult()),
                       JSReturnOperand);
    }

    // Until C++ code is instrumented against Spectre, prevent speculative
    // execution from returning any private data.
    if (JitOptions.spectreJitToCxxCalls && ins->mir()->hasLiveDefUses())
        masm.speculationBarrier();

    masm.adjustStack(IonDOMExitFrameLayout::Size());

    masm.bind(&haveValue);

    MOZ_ASSERT(masm.framePushed() == initialStack);
}

void
CodeGenerator::visitGetDOMMemberV(LGetDOMMemberV* ins)
{
    // It's simpler to duplicate visitLoadFixedSlotV here than it is to try to
    // use an LLoadFixedSlotV or some subclass of it for this case: that would
    // require us to have MGetDOMMember inherit from MLoadFixedSlot, and then
    // we'd have to duplicate a bunch of stuff we now get for free from
    // MGetDOMProperty.
    //
    // If this ever gets fixed to work with proxies (by not assuming that
    // reserved slot indices, which is what domMemberSlotIndex() returns,
    // match fixed slot indices), we can reenable MGetDOMMember for
    // proxies in IonBuilder.
    Register object = ToRegister(ins->object());
    size_t slot = ins->mir()->domMemberSlotIndex();
    ValueOperand result = ToOutValue(ins);

    masm.loadValue(Address(object, NativeObject::getFixedSlotOffset(slot)), result);
}

void
CodeGenerator::visitGetDOMMemberT(LGetDOMMemberT* ins)
{
    // It's simpler to duplicate visitLoadFixedSlotT here than it is to try to
    // use an LLoadFixedSlotT or some subclass of it for this case: that would
    // require us to have MGetDOMMember inherit from MLoadFixedSlot, and then
    // we'd have to duplicate a bunch of stuff we now get for free from
    // MGetDOMProperty.
    //
    // If this ever gets fixed to work with proxies (by not assuming that
    // reserved slot indices, which is what domMemberSlotIndex() returns,
    // match fixed slot indices), we can reenable MGetDOMMember for
    // proxies in IonBuilder.
    Register object = ToRegister(ins->object());
    size_t slot = ins->mir()->domMemberSlotIndex();
    AnyRegister result = ToAnyRegister(ins->getDef(0));
    MIRType type = ins->mir()->type();

    masm.loadUnboxedValue(Address(object, NativeObject::getFixedSlotOffset(slot)), type, result);
}

void
CodeGenerator::visitSetDOMProperty(LSetDOMProperty* ins)
{
    const Register JSContextReg = ToRegister(ins->getJSContextReg());
    const Register ObjectReg = ToRegister(ins->getObjectReg());
    const Register PrivateReg = ToRegister(ins->getPrivReg());
    const Register ValueReg = ToRegister(ins->getValueReg());

    DebugOnly<uint32_t> initialStack = masm.framePushed();

    masm.checkStackAlignment();

    // Push the argument. Rooting will happen at GC time.
    ValueOperand argVal = ToValue(ins, LSetDOMProperty::Value);
    masm.Push(argVal);
    // We pass the pointer to our out param as an instance of
    // JSJitGetterCallArgs, since on the binary level it's the same thing.
    JS_STATIC_ASSERT(sizeof(JSJitSetterCallArgs) == sizeof(Value*));
    masm.moveStackPtrTo(ValueReg);

    masm.Push(ObjectReg);

    LoadDOMPrivate(masm, ObjectReg, PrivateReg, ins->mir()->objectKind());

    // Rooting will happen at GC time.
    masm.moveStackPtrTo(ObjectReg);

    uint32_t safepointOffset = masm.buildFakeExitFrame(JSContextReg);
    masm.loadJSContext(JSContextReg);
    masm.enterFakeExitFrame(JSContextReg, JSContextReg, ExitFrameType::IonDOMSetter);

    markSafepointAt(safepointOffset, ins);

    masm.setupUnalignedABICall(JSContextReg);
    masm.loadJSContext(JSContextReg);
    masm.passABIArg(JSContextReg);
    masm.passABIArg(ObjectReg);
    masm.passABIArg(PrivateReg);
    masm.passABIArg(ValueReg);
    masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, ins->mir()->fun()), MoveOp::GENERAL,
                     CheckUnsafeCallWithABI::DontCheckHasExitFrame);

    masm.branchIfFalseBool(ReturnReg, masm.exceptionLabel());

    masm.adjustStack(IonDOMExitFrameLayout::Size());

    MOZ_ASSERT(masm.framePushed() == initialStack);
}

class OutOfLineIsCallable : public OutOfLineCodeBase<CodeGenerator>
{
    Register object_;
    Register output_;

  public:
    OutOfLineIsCallable(Register object, Register output)
      : object_(object), output_(output)
    { }

    void accept(CodeGenerator* codegen) override {
        codegen->visitOutOfLineIsCallable(this);
    }
    Register object() const {
        return object_;
    }
    Register output() const {
        return output_;
    }
};

template <CodeGenerator::CallableOrConstructor mode>
void
CodeGenerator::emitIsCallableOrConstructor(Register object, Register output, Label* failure)
{
    Label notFunction, hasCOps, done;
    masm.loadObjClassUnsafe(object, output);

    // Just skim proxies off. Their notion of isCallable()/isConstructor() is
    // more complicated.
    masm.branchTestClassIsProxy(true, output, failure);

    // An object is callable iff:
    //   is<JSFunction>() || (getClass()->cOps && getClass()->cOps->call).
    // An object is constructor iff:
    //  ((is<JSFunction>() && as<JSFunction>().isConstructor) ||
    //   (getClass()->cOps && getClass()->cOps->construct)).
    masm.branchPtr(Assembler::NotEqual, output, ImmPtr(&JSFunction::class_), &notFunction);
    if (mode == Callable) {
        masm.move32(Imm32(1), output);
    } else {
        Label notConstructor;
        masm.load16ZeroExtend(Address(object, JSFunction::offsetOfFlags()), output);
        masm.and32(Imm32(JSFunction::CONSTRUCTOR), output);
        masm.branchTest32(Assembler::Zero, output, output, &notConstructor);
        masm.move32(Imm32(1), output);
        masm.jump(&done);
        masm.bind(&notConstructor);
        masm.move32(Imm32(0), output);
    }
    masm.jump(&done);

    masm.bind(&notFunction);
    masm.branchPtr(Assembler::NonZero, Address(output, offsetof(js::Class, cOps)),
                   ImmPtr(nullptr), &hasCOps);
    masm.move32(Imm32(0), output);
    masm.jump(&done);

    masm.bind(&hasCOps);
    masm.loadPtr(Address(output, offsetof(js::Class, cOps)), output);
    size_t opsOffset = mode == Callable
                       ? offsetof(js::ClassOps, call)
                       : offsetof(js::ClassOps, construct);
    masm.cmpPtrSet(Assembler::NonZero, Address(output, opsOffset),
                   ImmPtr(nullptr), output);

    masm.bind(&done);
}

void
CodeGenerator::visitIsCallableO(LIsCallableO* ins)
{
    Register object = ToRegister(ins->object());
    Register output = ToRegister(ins->output());

    OutOfLineIsCallable* ool = new(alloc()) OutOfLineIsCallable(object, output);
    addOutOfLineCode(ool, ins->mir());

    emitIsCallableOrConstructor<Callable>(object, output, ool->entry());

    masm.bind(ool->rejoin());
}

void
CodeGenerator::visitIsCallableV(LIsCallableV* ins)
{
    ValueOperand val = ToValue(ins, LIsCallableV::Value);
    Register output = ToRegister(ins->output());
    Register temp = ToRegister(ins->temp());

    Label notObject;
    masm.branchTestObject(Assembler::NotEqual, val, &notObject);
    masm.unboxObject(val, temp);

    OutOfLineIsCallable* ool = new(alloc()) OutOfLineIsCallable(temp, output);
    addOutOfLineCode(ool, ins->mir());

    emitIsCallableOrConstructor<Callable>(temp, output, ool->entry());
    masm.jump(ool->rejoin());

    masm.bind(&notObject);
    masm.move32(Imm32(0), output);

    masm.bind(ool->rejoin());
}

void
CodeGenerator::visitOutOfLineIsCallable(OutOfLineIsCallable* ool)
{
    Register object = ool->object();
    Register output = ool->output();

    saveVolatile(output);
    masm.setupUnalignedABICall(output);
    masm.passABIArg(object);
    masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, ObjectIsCallable));
    masm.storeCallBoolResult(output);
    restoreVolatile(output);
    masm.jump(ool->rejoin());
}

typedef bool (*CheckIsCallableFn)(JSContext*, HandleValue, CheckIsCallableKind);
static const VMFunction CheckIsCallableInfo =
    FunctionInfo<CheckIsCallableFn>(CheckIsCallable, "CheckIsCallable");

void
CodeGenerator::visitCheckIsCallable(LCheckIsCallable* ins)
{
    ValueOperand checkValue = ToValue(ins, LCheckIsCallable::CheckValue);
    Register temp = ToRegister(ins->temp());

    // OOL code is used in the following 2 cases:
    //   * checkValue is not callable
    //   * checkValue is proxy and it's unknown whether it's callable or not
    // CheckIsCallable checks if passed value is callable, regardless of the
    // cases above.  IsCallable operation is not observable and checking it
    // again doesn't matter.
    OutOfLineCode* ool = oolCallVM(CheckIsCallableInfo, ins,
                                   ArgList(checkValue, Imm32(ins->mir()->checkKind())),
                                   StoreNothing());

    masm.branchTestObject(Assembler::NotEqual, checkValue, ool->entry());

    Register object = masm.extractObject(checkValue, temp);
    emitIsCallableOrConstructor<Callable>(object, temp, ool->entry());

    masm.branchTest32(Assembler::Zero, temp, temp, ool->entry());

    masm.bind(ool->rejoin());
}

class OutOfLineIsConstructor : public OutOfLineCodeBase<CodeGenerator>
{
    LIsConstructor* ins_;

  public:
    explicit OutOfLineIsConstructor(LIsConstructor* ins)
      : ins_(ins)
    { }

    void accept(CodeGenerator* codegen) override {
        codegen->visitOutOfLineIsConstructor(this);
    }
    LIsConstructor* ins() const {
        return ins_;
    }
};

void
CodeGenerator::visitIsConstructor(LIsConstructor* ins)
{
    Register object = ToRegister(ins->object());
    Register output = ToRegister(ins->output());

    OutOfLineIsConstructor* ool = new(alloc()) OutOfLineIsConstructor(ins);
    addOutOfLineCode(ool, ins->mir());

    emitIsCallableOrConstructor<Constructor>(object, output, ool->entry());

    masm.bind(ool->rejoin());
}

void
CodeGenerator::visitOutOfLineIsConstructor(OutOfLineIsConstructor* ool)
{
    LIsConstructor* ins = ool->ins();
    Register object = ToRegister(ins->object());
    Register output = ToRegister(ins->output());

    saveVolatile(output);
    masm.setupUnalignedABICall(output);
    masm.passABIArg(object);
    masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, ObjectIsConstructor));
    masm.storeCallBoolResult(output);
    restoreVolatile(output);
    masm.jump(ool->rejoin());
}

typedef bool (*IsArrayFn)(JSContext*, HandleObject, bool*);
static const VMFunction IsArrayInfo = FunctionInfo<IsArrayFn>(JS::IsArray, "IsArray");

static void
EmitObjectIsArray(MacroAssembler& masm, OutOfLineCode* ool, Register obj, Register output,
                  Label* notArray = nullptr)
{
    masm.loadObjClassUnsafe(obj, output);

    Label isArray;
    masm.branchPtr(Assembler::Equal, output, ImmPtr(&ArrayObject::class_), &isArray);

    // Branch to OOL path if it's a proxy.
    masm.branchTestClassIsProxy(true, output, ool->entry());

    if (notArray)
        masm.bind(notArray);
    masm.move32(Imm32(0), output);
    masm.jump(ool->rejoin());

    masm.bind(&isArray);
    masm.move32(Imm32(1), output);

    masm.bind(ool->rejoin());
}

void
CodeGenerator::visitIsArrayO(LIsArrayO* lir)
{
    Register object = ToRegister(lir->object());
    Register output = ToRegister(lir->output());

    OutOfLineCode* ool = oolCallVM(IsArrayInfo, lir, ArgList(object),
                                   StoreRegisterTo(output));
    EmitObjectIsArray(masm, ool, object, output);
}

void
CodeGenerator::visitIsArrayV(LIsArrayV* lir)
{
    ValueOperand val = ToValue(lir, LIsArrayV::Value);
    Register output = ToRegister(lir->output());
    Register temp = ToRegister(lir->temp());

    Label notArray;
    masm.branchTestObject(Assembler::NotEqual, val, &notArray);
    masm.unboxObject(val, temp);

    OutOfLineCode* ool = oolCallVM(IsArrayInfo, lir, ArgList(temp),
                                   StoreRegisterTo(output));
    EmitObjectIsArray(masm, ool, temp, output, &notArray);
}

void
CodeGenerator::visitIsTypedArray(LIsTypedArray* lir)
{
    Register object = ToRegister(lir->object());
    Register output = ToRegister(lir->output());

    Label notTypedArray;
    Label done;

    static_assert(Scalar::Int8 == 0, "Int8 is the first typed array class");
    static_assert((Scalar::Uint8Clamped - Scalar::Int8) == Scalar::MaxTypedArrayViewType - 1,
                  "Uint8Clamped is the last typed array class");
    const Class* firstTypedArrayClass = TypedArrayObject::classForType(Scalar::Int8);
    const Class* lastTypedArrayClass = TypedArrayObject::classForType(Scalar::Uint8Clamped);

    masm.loadObjClassUnsafe(object, output);
    masm.branchPtr(Assembler::Below, output, ImmPtr(firstTypedArrayClass), &notTypedArray);
    masm.branchPtr(Assembler::Above, output, ImmPtr(lastTypedArrayClass), &notTypedArray);

    masm.move32(Imm32(1), output);
    masm.jump(&done);
    masm.bind(&notTypedArray);
    masm.move32(Imm32(0), output);
    masm.bind(&done);
}

void
CodeGenerator::visitIsObject(LIsObject* ins)
{
    Register output = ToRegister(ins->output());
    ValueOperand value = ToValue(ins, LIsObject::Input);
    masm.testObjectSet(Assembler::Equal, value, output);
}

void
CodeGenerator::visitIsObjectAndBranch(LIsObjectAndBranch* ins)
{
    ValueOperand value = ToValue(ins, LIsObjectAndBranch::Input);
    testObjectEmitBranch(Assembler::Equal, value, ins->ifTrue(), ins->ifFalse());
}

void
CodeGenerator::loadOutermostJSScript(Register reg)
{
    // The "outermost" JSScript means the script that we are compiling
    // basically; this is not always the script associated with the
    // current basic block, which might be an inlined script.

    MIRGraph& graph = current->mir()->graph();
    MBasicBlock* entryBlock = graph.entryBlock();
    masm.movePtr(ImmGCPtr(entryBlock->info().script()), reg);
}

void
CodeGenerator::loadJSScriptForBlock(MBasicBlock* block, Register reg)
{
    // The current JSScript means the script for the current
    // basic block. This may be an inlined script.

    JSScript* script = block->info().script();
    masm.movePtr(ImmGCPtr(script), reg);
}

void
CodeGenerator::visitHasClass(LHasClass* ins)
{
    Register lhs = ToRegister(ins->lhs());
    Register output = ToRegister(ins->output());

    masm.loadObjClassUnsafe(lhs, output);
    masm.cmpPtrSet(Assembler::Equal, output, ImmPtr(ins->mir()->getClass()), output);
}

void
CodeGenerator::visitGuardToClass(LGuardToClass* ins)
{
    Register lhs = ToRegister(ins->lhs());
    Register output = ToRegister(ins->output());
    Register temp = ToRegister(ins->temp());

    Label notEqual;

    masm.branchTestObjClass(Assembler::NotEqual, lhs, ins->mir()->getClass(), temp,
                            output, &notEqual);
    masm.mov(lhs, output);

    if (ins->mir()->type() == MIRType::Object) {
        // Can't return null-return here, so bail
        bailoutFrom(&notEqual, ins->snapshot());
    } else {
        Label done;
        masm.jump(&done);

        masm.bind(&notEqual);
        masm.mov(ImmPtr(0), output);

        masm.bind(&done);
    }
}

typedef JSString* (*ObjectClassToStringFn)(JSContext*, HandleObject);
static const VMFunction ObjectClassToStringInfo =
    FunctionInfo<ObjectClassToStringFn>(js::ObjectClassToString, "ObjectClassToString");

void
CodeGenerator::visitObjectClassToString(LObjectClassToString* lir)
{
    pushArg(ToRegister(lir->object()));
    callVM(ObjectClassToStringInfo, lir);
}

void
CodeGenerator::visitWasmParameter(LWasmParameter* lir)
{
}

void
CodeGenerator::visitWasmParameterI64(LWasmParameterI64* lir)
{
}

void
CodeGenerator::visitWasmReturn(LWasmReturn* lir)
{
    // Don't emit a jump to the return label if this is the last block.
    if (current->mir() != *gen->graph().poBegin())
        masm.jump(&returnLabel_);
}

void
CodeGenerator::visitWasmReturnI64(LWasmReturnI64* lir)
{
    // Don't emit a jump to the return label if this is the last block.
    if (current->mir() != *gen->graph().poBegin())
        masm.jump(&returnLabel_);
}

void
CodeGenerator::visitWasmReturnVoid(LWasmReturnVoid* lir)
{
    // Don't emit a jump to the return label if this is the last block.
    if (current->mir() != *gen->graph().poBegin())
        masm.jump(&returnLabel_);
}

void
CodeGenerator::emitAssertRangeI(const Range* r, Register input)
{
    // Check the lower bound.
    if (r->hasInt32LowerBound() && r->lower() > INT32_MIN) {
        Label success;
        masm.branch32(Assembler::GreaterThanOrEqual, input, Imm32(r->lower()), &success);
        masm.assumeUnreachable("Integer input should be equal or higher than Lowerbound.");
        masm.bind(&success);
    }

    // Check the upper bound.
    if (r->hasInt32UpperBound() && r->upper() < INT32_MAX) {
        Label success;
        masm.branch32(Assembler::LessThanOrEqual, input, Imm32(r->upper()), &success);
        masm.assumeUnreachable("Integer input should be lower or equal than Upperbound.");
        masm.bind(&success);
    }

    // For r->canHaveFractionalPart(), r->canBeNegativeZero(), and
    // r->exponent(), there's nothing to check, because if we ended up in the
    // integer range checking code, the value is already in an integer register
    // in the integer range.
}

void
CodeGenerator::emitAssertRangeD(const Range* r, FloatRegister input, FloatRegister temp)
{
    // Check the lower bound.
    if (r->hasInt32LowerBound()) {
        Label success;
        masm.loadConstantDouble(r->lower(), temp);
        if (r->canBeNaN())
            masm.branchDouble(Assembler::DoubleUnordered, input, input, &success);
        masm.branchDouble(Assembler::DoubleGreaterThanOrEqual, input, temp, &success);
        masm.assumeUnreachable("Double input should be equal or higher than Lowerbound.");
        masm.bind(&success);
    }
    // Check the upper bound.
    if (r->hasInt32UpperBound()) {
        Label success;
        masm.loadConstantDouble(r->upper(), temp);
        if (r->canBeNaN())
            masm.branchDouble(Assembler::DoubleUnordered, input, input, &success);
        masm.branchDouble(Assembler::DoubleLessThanOrEqual, input, temp, &success);
        masm.assumeUnreachable("Double input should be lower or equal than Upperbound.");
        masm.bind(&success);
    }

    // This code does not yet check r->canHaveFractionalPart(). This would require new
    // assembler interfaces to make rounding instructions available.

    if (!r->canBeNegativeZero()) {
        Label success;

        // First, test for being equal to 0.0, which also includes -0.0.
        masm.loadConstantDouble(0.0, temp);
        masm.branchDouble(Assembler::DoubleNotEqualOrUnordered, input, temp, &success);

        // The easiest way to distinguish -0.0 from 0.0 is that 1.0/-0.0 is
        // -Infinity instead of Infinity.
        masm.loadConstantDouble(1.0, temp);
        masm.divDouble(input, temp);
        masm.branchDouble(Assembler::DoubleGreaterThan, temp, input, &success);

        masm.assumeUnreachable("Input shouldn't be negative zero.");

        masm.bind(&success);
    }

    if (!r->hasInt32Bounds() && !r->canBeInfiniteOrNaN() &&
        r->exponent() < FloatingPoint<double>::kExponentBias)
    {
        // Check the bounds implied by the maximum exponent.
        Label exponentLoOk;
        masm.loadConstantDouble(pow(2.0, r->exponent() + 1), temp);
        masm.branchDouble(Assembler::DoubleUnordered, input, input, &exponentLoOk);
        masm.branchDouble(Assembler::DoubleLessThanOrEqual, input, temp, &exponentLoOk);
        masm.assumeUnreachable("Check for exponent failed.");
        masm.bind(&exponentLoOk);

        Label exponentHiOk;
        masm.loadConstantDouble(-pow(2.0, r->exponent() + 1), temp);
        masm.branchDouble(Assembler::DoubleUnordered, input, input, &exponentHiOk);
        masm.branchDouble(Assembler::DoubleGreaterThanOrEqual, input, temp, &exponentHiOk);
        masm.assumeUnreachable("Check for exponent failed.");
        masm.bind(&exponentHiOk);
    } else if (!r->hasInt32Bounds() && !r->canBeNaN()) {
        // If we think the value can't be NaN, check that it isn't.
        Label notnan;
        masm.branchDouble(Assembler::DoubleOrdered, input, input, &notnan);
        masm.assumeUnreachable("Input shouldn't be NaN.");
        masm.bind(&notnan);

        // If we think the value also can't be an infinity, check that it isn't.
        if (!r->canBeInfiniteOrNaN()) {
            Label notposinf;
            masm.loadConstantDouble(PositiveInfinity<double>(), temp);
            masm.branchDouble(Assembler::DoubleLessThan, input, temp, &notposinf);
            masm.assumeUnreachable("Input shouldn't be +Inf.");
            masm.bind(&notposinf);

            Label notneginf;
            masm.loadConstantDouble(NegativeInfinity<double>(), temp);
            masm.branchDouble(Assembler::DoubleGreaterThan, input, temp, &notneginf);
            masm.assumeUnreachable("Input shouldn't be -Inf.");
            masm.bind(&notneginf);
        }
    }
}

void
CodeGenerator::visitAssertResultV(LAssertResultV* ins)
{
#ifdef DEBUG
    const ValueOperand value = ToValue(ins, LAssertResultV::Input);
    emitAssertResultV(value, ins->mirRaw()->resultTypeSet());
#else
    MOZ_CRASH("LAssertResultV is debug only");
#endif
}

void
CodeGenerator::visitAssertResultT(LAssertResultT* ins)
{
#ifdef DEBUG
    Register input = ToRegister(ins->input());
    MDefinition* mir = ins->mirRaw();
    emitAssertObjectOrStringResult(input, mir->type(), mir->resultTypeSet());
#else
    MOZ_CRASH("LAssertResultT is debug only");
#endif
}

void
CodeGenerator::visitAssertRangeI(LAssertRangeI* ins)
{
    Register input = ToRegister(ins->input());
    const Range* r = ins->range();

    emitAssertRangeI(r, input);
}

void
CodeGenerator::visitAssertRangeD(LAssertRangeD* ins)
{
    FloatRegister input = ToFloatRegister(ins->input());
    FloatRegister temp = ToFloatRegister(ins->temp());
    const Range* r = ins->range();

    emitAssertRangeD(r, input, temp);
}

void
CodeGenerator::visitAssertRangeF(LAssertRangeF* ins)
{
    FloatRegister input = ToFloatRegister(ins->input());
    FloatRegister temp = ToFloatRegister(ins->temp());
    FloatRegister temp2 = ToFloatRegister(ins->temp2());

    const Range* r = ins->range();

    masm.convertFloat32ToDouble(input, temp);
    emitAssertRangeD(r, temp, temp2);
}

void
CodeGenerator::visitAssertRangeV(LAssertRangeV* ins)
{
    const Range* r = ins->range();
    const ValueOperand value = ToValue(ins, LAssertRangeV::Input);
    Label done;

    {
        ScratchTagScope tag(masm, value);
        masm.splitTagForTest(value, tag);

        {
            Label isNotInt32;
            masm.branchTestInt32(Assembler::NotEqual, tag, &isNotInt32);
            {
                ScratchTagScopeRelease _(&tag);
                Register unboxInt32 = ToTempUnboxRegister(ins->temp());
                Register input = masm.extractInt32(value, unboxInt32);
                emitAssertRangeI(r, input);
                masm.jump(&done);
            }
            masm.bind(&isNotInt32);
        }

        {
            Label isNotDouble;
            masm.branchTestDouble(Assembler::NotEqual, tag, &isNotDouble);
            {
                ScratchTagScopeRelease _(&tag);
                FloatRegister input = ToFloatRegister(ins->floatTemp1());
                FloatRegister temp = ToFloatRegister(ins->floatTemp2());
                masm.unboxDouble(value, input);
                emitAssertRangeD(r, input, temp);
                masm.jump(&done);
            }
            masm.bind(&isNotDouble);
        }
    }

    masm.assumeUnreachable("Incorrect range for Value.");
    masm.bind(&done);
}

void
CodeGenerator::visitInterruptCheck(LInterruptCheck* lir)
{
    if (lir->implicit()) {
        OutOfLineInterruptCheckImplicit* ool = new(alloc()) OutOfLineInterruptCheckImplicit(current, lir);
        addOutOfLineCode(ool, lir->mir());

        lir->setOolEntry(ool->entry());
        masm.bind(ool->rejoin());
        return;
    }

    OutOfLineCode* ool = oolCallVM(InterruptCheckInfo, lir, ArgList(), StoreNothing());

    Register temp = ToRegister(lir->temp());

    const void* contextAddr = gen->compartment->zone()->addressOfJSContext();
    masm.loadPtr(AbsoluteAddress(contextAddr), temp);
    masm.branch32(Assembler::NotEqual, Address(temp, offsetof(JSContext, interrupt_)),
                  Imm32(0), ool->entry());
    masm.bind(ool->rejoin());
}

void
CodeGenerator::visitWasmTrap(LWasmTrap* lir)
{
    MOZ_ASSERT(gen->compilingWasm());
    const MWasmTrap* mir = lir->mir();

    masm.wasmTrap(mir->trap(), mir->bytecodeOffset());
}

void
CodeGenerator::visitWasmBoundsCheck(LWasmBoundsCheck* ins)
{
#ifdef WASM_HUGE_MEMORY
    MOZ_CRASH("No wasm bounds check for huge memory");
#else
    const MWasmBoundsCheck* mir = ins->mir();
    Register ptr = ToRegister(ins->ptr());
    Register boundsCheckLimit = ToRegister(ins->boundsCheckLimit());
    masm.wasmBoundsCheck(Assembler::AboveOrEqual, ptr, boundsCheckLimit,
                         oldTrap(mir, wasm::Trap::OutOfBounds));
#endif
}

void
CodeGenerator::visitWasmAlignmentCheck(LWasmAlignmentCheck* ins)
{
    const MWasmAlignmentCheck* mir = ins->mir();
    Register ptr = ToRegister(ins->ptr());
    masm.branchTest32(Assembler::NonZero, ptr, Imm32(mir->byteSize() - 1),
                      oldTrap(mir, wasm::Trap::UnalignedAccess));
}

void
CodeGenerator::visitWasmLoadTls(LWasmLoadTls* ins)
{
    switch (ins->mir()->type()) {
      case MIRType::Pointer:
        masm.loadPtr(Address(ToRegister(ins->tlsPtr()), ins->mir()->offset()),
                     ToRegister(ins->output()));
        break;
      case MIRType::Int32:
        masm.load32(Address(ToRegister(ins->tlsPtr()), ins->mir()->offset()),
                    ToRegister(ins->output()));
        break;
      default:
        MOZ_CRASH("MIRType not supported in WasmLoadTls");
    }
}

typedef bool (*RecompileFn)(JSContext*);
static const VMFunction RecompileFnInfo = FunctionInfo<RecompileFn>(Recompile, "Recompile");

typedef bool (*ForcedRecompileFn)(JSContext*);
static const VMFunction ForcedRecompileFnInfo =
    FunctionInfo<ForcedRecompileFn>(ForcedRecompile, "ForcedRecompile");

void
CodeGenerator::visitRecompileCheck(LRecompileCheck* ins)
{
    Label done;
    Register tmp = ToRegister(ins->scratch());
    OutOfLineCode* ool;
    if (ins->mir()->forceRecompilation())
        ool = oolCallVM(ForcedRecompileFnInfo, ins, ArgList(), StoreRegisterTo(tmp));
    else
        ool = oolCallVM(RecompileFnInfo, ins, ArgList(), StoreRegisterTo(tmp));

    // Check if warm-up counter is high enough.
    AbsoluteAddress warmUpCount = AbsoluteAddress(ins->mir()->script()->addressOfWarmUpCounter());
    if (ins->mir()->increaseWarmUpCounter()) {
        masm.load32(warmUpCount, tmp);
        masm.add32(Imm32(1), tmp);
        masm.store32(tmp, warmUpCount);
        masm.branch32(Assembler::BelowOrEqual, tmp, Imm32(ins->mir()->recompileThreshold()), &done);
    } else {
        masm.branch32(Assembler::BelowOrEqual, warmUpCount, Imm32(ins->mir()->recompileThreshold()),
                      &done);
    }

    // Check if not yet recompiling.
    CodeOffset label = masm.movWithPatch(ImmWord(uintptr_t(-1)), tmp);
    masm.propagateOOM(ionScriptLabels_.append(label));
    masm.branch32(Assembler::Equal,
                  Address(tmp, IonScript::offsetOfRecompiling()),
                  Imm32(0),
                  ool->entry());
    masm.bind(ool->rejoin());
    masm.bind(&done);
}

void
CodeGenerator::visitLexicalCheck(LLexicalCheck* ins)
{
    ValueOperand inputValue = ToValue(ins, LLexicalCheck::Input);
    Label bail;
    masm.branchTestMagicValue(Assembler::Equal, inputValue, JS_UNINITIALIZED_LEXICAL, &bail);
    bailoutFrom(&bail, ins->snapshot());
}

typedef bool (*ThrowRuntimeLexicalErrorFn)(JSContext*, unsigned);
static const VMFunction ThrowRuntimeLexicalErrorInfo =
    FunctionInfo<ThrowRuntimeLexicalErrorFn>(ThrowRuntimeLexicalError, "ThrowRuntimeLexicalError");

void
CodeGenerator::visitThrowRuntimeLexicalError(LThrowRuntimeLexicalError* ins)
{
    pushArg(Imm32(ins->mir()->errorNumber()));
    callVM(ThrowRuntimeLexicalErrorInfo, ins);
}

typedef bool (*GlobalNameConflictsCheckFromIonFn)(JSContext*, HandleScript);
static const VMFunction GlobalNameConflictsCheckFromIonInfo =
    FunctionInfo<GlobalNameConflictsCheckFromIonFn>(GlobalNameConflictsCheckFromIon,
                                                    "GlobalNameConflictsCheckFromIon");

void
CodeGenerator::visitGlobalNameConflictsCheck(LGlobalNameConflictsCheck* ins)
{
    pushArg(ImmGCPtr(ins->mirRaw()->block()->info().script()));
    callVM(GlobalNameConflictsCheckFromIonInfo, ins);
}

void
CodeGenerator::visitDebugger(LDebugger* ins)
{
    Register cx = ToRegister(ins->getTemp(0));
    Register temp = ToRegister(ins->getTemp(1));

    masm.loadJSContext(cx);
    masm.setupUnalignedABICall(temp);
    masm.passABIArg(cx);
    masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, GlobalHasLiveOnDebuggerStatement));

    Label bail;
    masm.branchIfTrueBool(ReturnReg, &bail);
    bailoutFrom(&bail, ins->snapshot());
}

void
CodeGenerator::visitNewTarget(LNewTarget *ins)
{
    ValueOperand output = ToOutValue(ins);

    // if (isConstructing) output = argv[Max(numActualArgs, numFormalArgs)]
    Label notConstructing, done;
    Address calleeToken(masm.getStackPointer(), frameSize() + JitFrameLayout::offsetOfCalleeToken());
    masm.branchTestPtr(Assembler::Zero, calleeToken,
                       Imm32(CalleeToken_FunctionConstructing), &notConstructing);

    Register argvLen = output.scratchReg();

    Address actualArgsPtr(masm.getStackPointer(), frameSize() + JitFrameLayout::offsetOfNumActualArgs());
    masm.loadPtr(actualArgsPtr, argvLen);

    Label useNFormals;

    size_t numFormalArgs = ins->mirRaw()->block()->info().funMaybeLazy()->nargs();
    masm.branchPtr(Assembler::Below, argvLen, Imm32(numFormalArgs),
                   &useNFormals);

    size_t argsOffset = frameSize() + JitFrameLayout::offsetOfActualArgs();
    {
        BaseValueIndex newTarget(masm.getStackPointer(), argvLen, argsOffset);
        masm.loadValue(newTarget, output);
        masm.jump(&done);
    }

    masm.bind(&useNFormals);

    {
        Address newTarget(masm.getStackPointer(), argsOffset + (numFormalArgs * sizeof(Value)));
        masm.loadValue(newTarget, output);
        masm.jump(&done);
    }

    // else output = undefined
    masm.bind(&notConstructing);
    masm.moveValue(UndefinedValue(), output);
    masm.bind(&done);
}

void
CodeGenerator::visitCheckReturn(LCheckReturn* ins)
{
    ValueOperand returnValue = ToValue(ins, LCheckReturn::ReturnValue);
    ValueOperand thisValue = ToValue(ins, LCheckReturn::ThisValue);
    Label bail, noChecks;
    masm.branchTestObject(Assembler::Equal, returnValue, &noChecks);
    masm.branchTestUndefined(Assembler::NotEqual, returnValue, &bail);
    masm.branchTestMagicValue(Assembler::Equal, thisValue, JS_UNINITIALIZED_LEXICAL, &bail);
    bailoutFrom(&bail, ins->snapshot());
    masm.bind(&noChecks);
}

typedef bool (*ThrowCheckIsObjectFn)(JSContext*, CheckIsObjectKind);
static const VMFunction ThrowCheckIsObjectInfo =
    FunctionInfo<ThrowCheckIsObjectFn>(ThrowCheckIsObject, "ThrowCheckIsObject");

void
CodeGenerator::visitCheckIsObj(LCheckIsObj* ins)
{
    ValueOperand checkValue = ToValue(ins, LCheckIsObj::CheckValue);

    OutOfLineCode* ool = oolCallVM(ThrowCheckIsObjectInfo, ins,
                                   ArgList(Imm32(ins->mir()->checkKind())),
                                   StoreNothing());
    masm.branchTestObject(Assembler::NotEqual, checkValue, ool->entry());
    masm.bind(ool->rejoin());
}

typedef bool (*ThrowObjCoercibleFn)(JSContext*, HandleValue);
static const VMFunction ThrowObjectCoercibleInfo =
    FunctionInfo<ThrowObjCoercibleFn>(ThrowObjectCoercible, "ThrowObjectCoercible");

void
CodeGenerator::visitCheckObjCoercible(LCheckObjCoercible* ins)
{
    ValueOperand checkValue = ToValue(ins, LCheckObjCoercible::CheckValue);
    Label fail, done;
    masm.branchTestNull(Assembler::Equal, checkValue, &fail);
    masm.branchTestUndefined(Assembler::NotEqual, checkValue, &done);
    masm.bind(&fail);
    pushArg(checkValue);
    callVM(ThrowObjectCoercibleInfo, ins);
    masm.bind(&done);
}

typedef bool (*CheckSelfHostedFn)(JSContext*, HandleValue);
static const VMFunction CheckSelfHostedInfo =
    FunctionInfo<CheckSelfHostedFn>(js::Debug_CheckSelfHosted, "Debug_CheckSelfHosted");

void
CodeGenerator::visitDebugCheckSelfHosted(LDebugCheckSelfHosted* ins)
{
    ValueOperand checkValue = ToValue(ins, LDebugCheckSelfHosted::CheckValue);
    pushArg(checkValue);
    callVM(CheckSelfHostedInfo, ins);
}

void
CodeGenerator::visitRandom(LRandom* ins)
{
    using mozilla::non_crypto::XorShift128PlusRNG;

    FloatRegister output = ToFloatRegister(ins->output());
    Register tempReg = ToRegister(ins->temp0());

#ifdef JS_PUNBOX64
    Register64 s0Reg(ToRegister(ins->temp1()));
    Register64 s1Reg(ToRegister(ins->temp2()));
#else
    Register64 s0Reg(ToRegister(ins->temp1()), ToRegister(ins->temp2()));
    Register64 s1Reg(ToRegister(ins->temp3()), ToRegister(ins->temp4()));
#endif

    const void* rng = gen->compartment->addressOfRandomNumberGenerator();
    masm.movePtr(ImmPtr(rng), tempReg);

    static_assert(sizeof(XorShift128PlusRNG) == 2 * sizeof(uint64_t),
                  "Code below assumes XorShift128PlusRNG contains two uint64_t values");

    Address state0Addr(tempReg, XorShift128PlusRNG::offsetOfState0());
    Address state1Addr(tempReg, XorShift128PlusRNG::offsetOfState1());

    // uint64_t s1 = mState[0];
    masm.load64(state0Addr, s1Reg);

    // s1 ^= s1 << 23;
    masm.move64(s1Reg, s0Reg);
    masm.lshift64(Imm32(23), s1Reg);
    masm.xor64(s0Reg, s1Reg);

    // s1 ^= s1 >> 17
    masm.move64(s1Reg, s0Reg);
    masm.rshift64(Imm32(17), s1Reg);
    masm.xor64(s0Reg, s1Reg);

    // const uint64_t s0 = mState[1];
    masm.load64(state1Addr, s0Reg);

    // mState[0] = s0;
    masm.store64(s0Reg, state0Addr);

    // s1 ^= s0
    masm.xor64(s0Reg, s1Reg);

    // s1 ^= s0 >> 26
    masm.rshift64(Imm32(26), s0Reg);
    masm.xor64(s0Reg, s1Reg);

    // mState[1] = s1
    masm.store64(s1Reg, state1Addr);

    // s1 += mState[0]
    masm.load64(state0Addr, s0Reg);
    masm.add64(s0Reg, s1Reg);

    // See comment in XorShift128PlusRNG::nextDouble().
    static const int MantissaBits = FloatingPoint<double>::kExponentShift + 1;
    static const double ScaleInv = double(1) / (1ULL << MantissaBits);

    masm.and64(Imm64((1ULL << MantissaBits) - 1), s1Reg);

    if (masm.convertUInt64ToDoubleNeedsTemp())
        masm.convertUInt64ToDouble(s1Reg, output, tempReg);
    else
        masm.convertUInt64ToDouble(s1Reg, output, Register::Invalid());

    // output *= ScaleInv
    masm.mulDoublePtr(ImmPtr(&ScaleInv), tempReg, output);
}

void
CodeGenerator::visitSignExtendInt32(LSignExtendInt32* ins)
{
    Register input = ToRegister(ins->input());
    Register output = ToRegister(ins->output());

    switch (ins->mode()) {
      case MSignExtendInt32::Byte:
        masm.move8SignExtend(input, output);
        break;
      case MSignExtendInt32::Half:
        masm.move16SignExtend(input, output);
        break;
    }
}

void
CodeGenerator::visitRotate(LRotate* ins)
{
    MRotate* mir = ins->mir();
    Register input = ToRegister(ins->input());
    Register dest = ToRegister(ins->output());

    const LAllocation* count = ins->count();
    if (count->isConstant()) {
        int32_t c = ToInt32(count) & 0x1F;
        if (mir->isLeftRotate())
            masm.rotateLeft(Imm32(c), input, dest);
        else
            masm.rotateRight(Imm32(c), input, dest);
    } else {
        Register creg = ToRegister(count);
        if (mir->isLeftRotate())
            masm.rotateLeft(creg, input, dest);
        else
            masm.rotateRight(creg, input, dest);
    }
}

class OutOfLineNaNToZero : public OutOfLineCodeBase<CodeGenerator>
{
    LNaNToZero* lir_;

  public:
    explicit OutOfLineNaNToZero(LNaNToZero* lir)
      : lir_(lir)
    {}

    void accept(CodeGenerator* codegen) override {
        codegen->visitOutOfLineNaNToZero(this);
    }
    LNaNToZero* lir() const {
        return lir_;
    }
};

void
CodeGenerator::visitOutOfLineNaNToZero(OutOfLineNaNToZero* ool)
{
    FloatRegister output = ToFloatRegister(ool->lir()->output());
    masm.loadConstantDouble(0.0, output);
    masm.jump(ool->rejoin());
}

void
CodeGenerator::visitNaNToZero(LNaNToZero* lir)
{
    FloatRegister input = ToFloatRegister(lir->input());

    OutOfLineNaNToZero* ool = new(alloc()) OutOfLineNaNToZero(lir);
    addOutOfLineCode(ool, lir->mir());

    if (lir->mir()->operandIsNeverNegativeZero()){
        masm.branchDouble(Assembler::DoubleUnordered, input, input, ool->entry());
    } else {
        FloatRegister scratch = ToFloatRegister(lir->tempDouble());
        masm.loadConstantDouble(0.0, scratch);
        masm.branchDouble(Assembler::DoubleEqualOrUnordered, input, scratch, ool->entry());
    }
    masm.bind(ool->rejoin());
}

typedef bool (*FinishBoundFunctionInitFn)(JSContext* cx, HandleFunction bound,
                                          HandleObject target, int32_t argCount);
static const VMFunction FinishBoundFunctionInitInfo =
    FunctionInfo<FinishBoundFunctionInitFn>(JSFunction::finishBoundFunctionInit,
                                            "JSFunction::finishBoundFunctionInit");

void
CodeGenerator::visitFinishBoundFunctionInit(LFinishBoundFunctionInit* lir)
{
    Register bound = ToRegister(lir->bound());
    Register target = ToRegister(lir->target());
    Register argCount = ToRegister(lir->argCount());
    Register temp1 = ToRegister(lir->temp1());
    Register temp2 = ToRegister(lir->temp2());

    OutOfLineCode* ool = oolCallVM(FinishBoundFunctionInitInfo, lir,
                                   ArgList(bound, target, argCount), StoreNothing());
    Label* slowPath = ool->entry();

    const size_t boundLengthOffset = FunctionExtended::offsetOfExtendedSlot(BOUND_FUN_LENGTH_SLOT);

    // Take the slow path if the target is not a JSFunction.
    masm.branchTestObjClass(Assembler::NotEqual, target, &JSFunction::class_, temp1, target,
                            slowPath);

    // Take the slow path if we'd need to adjust the [[Prototype]].
    masm.loadObjProto(bound, temp1);
    masm.loadObjProto(target, temp2);
    masm.branchPtr(Assembler::NotEqual, temp1, temp2, slowPath);

    // Get the function flags.
    masm.load16ZeroExtend(Address(target, JSFunction::offsetOfFlags()), temp1);

    // Functions with lazy scripts don't store their length.
    // If the length or name property is resolved, it might be shadowed.
    masm.branchTest32(Assembler::NonZero,
                      temp1,
                      Imm32(JSFunction::INTERPRETED_LAZY |
                            JSFunction::RESOLVED_NAME |
                            JSFunction::RESOLVED_LENGTH),
                      slowPath);

    Label notBoundTarget, loadName;
    masm.branchTest32(Assembler::Zero, temp1, Imm32(JSFunction::BOUND_FUN), &notBoundTarget);
    {
        // Target's name atom doesn't contain the bound function prefix, so we
        // need to call into the VM.
        masm.branchTest32(Assembler::Zero, temp1,
                          Imm32(JSFunction::HAS_BOUND_FUNCTION_NAME_PREFIX), slowPath);

        // We also take the slow path when target's length isn't an int32.
        masm.branchTestInt32(Assembler::NotEqual, Address(target, boundLengthOffset), slowPath);

        // Bound functions reuse HAS_GUESSED_ATOM for HAS_BOUND_FUNCTION_NAME_PREFIX,
        // so skip the guessed atom check below.
        static_assert(JSFunction::HAS_BOUND_FUNCTION_NAME_PREFIX == JSFunction::HAS_GUESSED_ATOM,
                      "HAS_BOUND_FUNCTION_NAME_PREFIX is shared with HAS_GUESSED_ATOM");
        masm.jump(&loadName);
    }
    masm.bind(&notBoundTarget);

    Label guessed, hasName;
    masm.branchTest32(Assembler::NonZero, temp1, Imm32(JSFunction::HAS_GUESSED_ATOM), &guessed);
    masm.bind(&loadName);
    masm.loadPtr(Address(target, JSFunction::offsetOfAtom()), temp2);
    masm.branchTestPtr(Assembler::NonZero, temp2, temp2, &hasName);
    {
        masm.bind(&guessed);

        // Unnamed class expression don't have a name property. To avoid
        // looking it up from the prototype chain, we take the slow path here.
        masm.branchFunctionKind(Assembler::Equal, JSFunction::ClassConstructor, target, temp2,
                                slowPath);

        // An absent name property defaults to the empty string.
        const JSAtomState& names = gen->runtime->names();
        masm.movePtr(ImmGCPtr(names.empty), temp2);
    }
    masm.bind(&hasName);

    // Store the target's name atom in the bound function as is.
    masm.storePtr(temp2, Address(bound, JSFunction::offsetOfAtom()));

    // Set the BOUND_FN flag and, if the target is a constructor, the
    // CONSTRUCTOR flag.
    Label isConstructor, boundFlagsComputed;
    masm.load16ZeroExtend(Address(bound, JSFunction::offsetOfFlags()), temp2);
    masm.branchTest32(Assembler::NonZero, temp1, Imm32(JSFunction::CONSTRUCTOR), &isConstructor);
    {
        masm.or32(Imm32(JSFunction::BOUND_FUN), temp2);
        masm.jump(&boundFlagsComputed);
    }
    masm.bind(&isConstructor);
    {
        masm.or32(Imm32(JSFunction::BOUND_FUN | JSFunction::CONSTRUCTOR), temp2);
    }
    masm.bind(&boundFlagsComputed);
    masm.store16(temp2, Address(bound, JSFunction::offsetOfFlags()));

    // Load the target function's length.
    Label isInterpreted, isBound, lengthLoaded;
    masm.branchTest32(Assembler::NonZero, temp1, Imm32(JSFunction::BOUND_FUN), &isBound);
    masm.branchTest32(Assembler::NonZero, temp1, Imm32(JSFunction::INTERPRETED), &isInterpreted);
    {
        // Load the length property of a native function.
        masm.load16ZeroExtend(Address(target, JSFunction::offsetOfNargs()), temp1);
        masm.jump(&lengthLoaded);
    }
    masm.bind(&isBound);
    {
        // Load the length property of a bound function.
        masm.unboxInt32(Address(target, boundLengthOffset), temp1);
        masm.jump(&lengthLoaded);
    }
    masm.bind(&isInterpreted);
    {
        // Load the length property of an interpreted function.
        masm.loadPtr(Address(target, JSFunction::offsetOfScript()), temp1);
        masm.load16ZeroExtend(Address(temp1, JSScript::offsetOfFunLength()), temp1);
    }
    masm.bind(&lengthLoaded);

    // Compute the bound function length: Max(0, target.length - argCount).
    Label nonNegative;
    masm.sub32(argCount, temp1);
    masm.branch32(Assembler::GreaterThanOrEqual, temp1, Imm32(0), &nonNegative);
    masm.move32(Imm32(0), temp1);
    masm.bind(&nonNegative);

    // Store the bound function's length into the extended slot.
    masm.storeValue(JSVAL_TYPE_INT32, temp1, Address(bound, boundLengthOffset));

    masm.bind(ool->rejoin());
}

void
CodeGenerator::visitIsPackedArray(LIsPackedArray* lir)
{
    Register array = ToRegister(lir->array());
    Register output = ToRegister(lir->output());
    Register elementsTemp = ToRegister(lir->temp());

    Label notPacked, done;

    // Load elements and length.
    masm.loadPtr(Address(array, NativeObject::offsetOfElements()), elementsTemp);
    masm.load32(Address(elementsTemp, ObjectElements::offsetOfLength()), output);

    // Test length == initializedLength.
    Address initLength(elementsTemp, ObjectElements::offsetOfInitializedLength());
    masm.branch32(Assembler::NotEqual, initLength, output, &notPacked);

    masm.move32(Imm32(1), output);
    masm.jump(&done);
    masm.bind(&notPacked);
    masm.move32(Imm32(0), output);

    masm.bind(&done);
}

typedef bool (*GetPrototypeOfFn)(JSContext*, HandleObject, MutableHandleValue);
static const VMFunction GetPrototypeOfInfo =
    FunctionInfo<GetPrototypeOfFn>(jit::GetPrototypeOf, "GetPrototypeOf");

void
CodeGenerator::visitGetPrototypeOf(LGetPrototypeOf* lir)
{
    Register target = ToRegister(lir->target());
    ValueOperand out = ToOutValue(lir);
    Register scratch = out.scratchReg();

    OutOfLineCode* ool = oolCallVM(GetPrototypeOfInfo, lir, ArgList(target), StoreValueTo(out));

    MOZ_ASSERT(uintptr_t(TaggedProto::LazyProto) == 1);

    masm.loadObjProto(target, scratch);

    Label hasProto;
    masm.branchPtr(Assembler::Above, scratch, ImmWord(1), &hasProto);

    // Call into the VM for lazy prototypes.
    masm.branchPtr(Assembler::Equal, scratch, ImmWord(1), ool->entry());

    masm.moveValue(NullValue(), out);
    masm.jump(ool->rejoin());

    masm.bind(&hasProto);
    masm.tagValue(JSVAL_TYPE_OBJECT, scratch, out);

    masm.bind(ool->rejoin());
}

static_assert(!std::is_polymorphic<CodeGenerator>::value,
              "CodeGenerator should not have any virtual methods");

} // namespace jit
} // namespace js

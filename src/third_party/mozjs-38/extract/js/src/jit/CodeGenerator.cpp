/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/CodeGenerator.h"

#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"
#include "mozilla/DebugOnly.h"
#include "mozilla/MathAlgorithms.h"

#include "jslibmath.h"
#include "jsmath.h"
#include "jsnum.h"
#include "jsprf.h"

#include "asmjs/AsmJSModule.h"
#include "builtin/Eval.h"
#include "builtin/TypedObject.h"
#include "gc/Nursery.h"
#include "irregexp/NativeRegExpMacroAssembler.h"
#include "jit/BaselineCompiler.h"
#include "jit/IonBuilder.h"
#include "jit/IonCaches.h"
#include "jit/IonOptimizationLevels.h"
#include "jit/JitcodeMap.h"
#include "jit/JitSpewer.h"
#include "jit/Linker.h"
#include "jit/Lowering.h"
#include "jit/MIRGenerator.h"
#include "jit/MoveEmitter.h"
#include "jit/RangeAnalysis.h"
#include "vm/MatchPairs.h"
#include "vm/RegExpStatics.h"
#include "vm/TraceLogging.h"

#include "jsboolinlines.h"

#include "jit/shared/CodeGenerator-shared-inl.h"
#include "vm/Interpreter-inl.h"

using namespace js;
using namespace js::jit;

using mozilla::DebugOnly;
using mozilla::FloatingPoint;
using mozilla::Maybe;
using mozilla::NegativeInfinity;
using mozilla::PositiveInfinity;
using JS::GenericNaN;

namespace js {
namespace jit {

// This out-of-line cache is used to do a double dispatch including it-self and
// the wrapped IonCache.
class OutOfLineUpdateCache :
  public OutOfLineCodeBase<CodeGenerator>,
  public IonCacheVisitor
{
  private:
    LInstruction* lir_;
    size_t cacheIndex_;
    AddCacheState state_;

  public:
    OutOfLineUpdateCache(LInstruction* lir, size_t cacheIndex)
      : lir_(lir),
        cacheIndex_(cacheIndex)
    { }

    void bind(MacroAssembler* masm) {
        // The binding of the initial jump is done in
        // CodeGenerator::visitOutOfLineCache.
    }

    size_t getCacheIndex() const {
        return cacheIndex_;
    }
    LInstruction* lir() const {
        return lir_;
    }
    AddCacheState& state() {
        return state_;
    }

    void accept(CodeGenerator* codegen) {
        codegen->visitOutOfLineCache(this);
    }

    // ICs' visit functions delegating the work to the CodeGen visit funtions.
#define VISIT_CACHE_FUNCTION(op)                                        \
    void visit##op##IC(CodeGenerator* codegen) {                        \
        CodeGenerator::DataPtr<op##IC> ic(codegen, getCacheIndex());    \
        codegen->visit##op##IC(this, ic);                        \
    }

    IONCACHE_KIND_LIST(VISIT_CACHE_FUNCTION)
#undef VISIT_CACHE_FUNCTION
};

// This function is declared here because it needs to instantiate an
// OutOfLineUpdateCache, but we want to keep it visible inside the
// CodeGeneratorShared such as we can specialize inline caches in function of
// the architecture.
void
CodeGeneratorShared::addCache(LInstruction* lir, size_t cacheIndex)
{
    if (cacheIndex == SIZE_MAX) {
        masm.setOOM();
        return;
    }

    DataPtr<IonCache> cache(this, cacheIndex);
    MInstruction* mir = lir->mirRaw()->toInstruction();
    if (mir->resumePoint())
        cache->setScriptedLocation(mir->block()->info().script(),
                                   mir->resumePoint()->pc());
    else
        cache->setIdempotent();

    OutOfLineUpdateCache* ool = new(alloc()) OutOfLineUpdateCache(lir, cacheIndex);
    addOutOfLineCode(ool, mir);

    // OOL-specific state depends on the type of cache.
    cache->initializeAddCacheState(lir, &ool->state());

    cache->emitInitialJump(masm, ool->state());
    masm.bind(ool->rejoin());
}

void
CodeGenerator::visitOutOfLineCache(OutOfLineUpdateCache* ool)
{
    DataPtr<IonCache> cache(this, ool->getCacheIndex());

    // Register the location of the OOL path in the IC.
    cache->setFallbackLabel(masm.labelForPatch());
    cache->bindInitialJump(masm, ool->state());

    // Dispatch to ICs' accept functions.
    cache->accept(this, ool);
}

StringObject*
MNewStringObject::templateObj() const {
    return &templateObj_->as<StringObject>();
}

CodeGenerator::CodeGenerator(MIRGenerator* gen, LIRGraph* graph, MacroAssembler* masm)
  : CodeGeneratorSpecific(gen, graph, masm)
  , ionScriptLabels_(gen->alloc())
  , scriptCounts_(nullptr)
{
}

CodeGenerator::~CodeGenerator()
{
    MOZ_ASSERT_IF(!gen->compilingAsmJS(), masm.numAsmJSAbsoluteLinks() == 0);
    js_delete(scriptCounts_);
}

typedef bool (*StringToNumberFn)(ExclusiveContext*, JSString*, double*);
static const VMFunction StringToNumberInfo = FunctionInfo<StringToNumberFn>(StringToNumber);

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
        Label* stringEntry, *stringRejoin;
        Register stringReg;
        if (input->mightBeType(MIRType_String)) {
            stringReg = ToRegister(lir->temp());
            OutOfLineCode* oolString = oolCallVM(StringToNumberInfo, lir, (ArgList(), stringReg),
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

    Register tag = masm.splitTagForTest(operand);

    Label isDouble, isInt32, isBool, isNull, isUndefined, done;
    bool hasBoolean = false, hasNull = false, hasUndefined = false;

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

    Register tag = masm.splitTagForTest(operand);

    Label isDouble, isInt32, isBool, isNull, isUndefined, done;
    bool hasBoolean = false, hasNull = false, hasUndefined = false;

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
#if defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_MIPS)
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
    masm.setupUnalignedABICall(1, scratch);
    masm.passABIArg(objreg);
    masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, js::EmulatesUndefined));
    masm.storeCallResult(scratch);
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

    void accept(CodeGenerator* codegen) final override {
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
    masm.branchTestObjectTruthy(false, objreg, scratch, ool->entry(), ifEmulatesUndefined);
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
    bool mightBeUndefined = valueMIR->mightBeType(MIRType_Undefined);
    bool mightBeNull = valueMIR->mightBeType(MIRType_Null);
    bool mightBeBoolean = valueMIR->mightBeType(MIRType_Boolean);
    bool mightBeInt32 = valueMIR->mightBeType(MIRType_Int32);
    bool mightBeObject = valueMIR->mightBeType(MIRType_Object);
    bool mightBeString = valueMIR->mightBeType(MIRType_String);
    bool mightBeSymbol = valueMIR->mightBeType(MIRType_Symbol);
    bool mightBeDouble = valueMIR->mightBeType(MIRType_Double);
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

    Register tag = masm.splitTagForTest(value);

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
        masm.branchTestBooleanTruthy(false, value, ifFalsy);
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
        masm.branchTestInt32Truthy(false, value, ifFalsy);
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

            Register objreg = masm.extractObject(value, ToRegister(scratch1));
            testObjectEmulatesUndefined(objreg, ifFalsy, ifTruthy, ToRegister(scratch2), ool);

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
        masm.branchTestStringTruthy(false, value, ifFalsy);
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
        masm.unboxDouble(value, fr);
        masm.branchTestDoubleTruthy(false, fr, ifFalsy);
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

Label*
CodeGenerator::getJumpLabelForBranch(MBasicBlock* block)
{
    // Skip past trivial blocks.
    block = skipTrivialBlocks(block);

    if (!labelForBackedgeWithImplicitCheck(block))
        return block->lir()->label();

    // We need to use a patchable jump for this backedge, but want to treat
    // this as a normal label target to simplify codegen. Efficiency isn't so
    // important here as these tests are extremely unlikely to be used in loop
    // backedges, so emit inline code for the patchable jump. Heap allocating
    // the label allows it to be used by out of line blocks.
    Label* res = GetJitContext()->temp->lifoAlloc()->new_<Label>();
    Label after;
    masm.jump(&after);
    masm.bind(res);
    jumpToBlock(block);
    masm.bind(&after);
    return res;
}

void
CodeGenerator::visitTestOAndBranch(LTestOAndBranch* lir)
{
    MIRType inputType = lir->mir()->input()->type();
    MOZ_ASSERT(inputType == MIRType_ObjectOrNull || lir->mir()->operandMightEmulateUndefined(),
               "If the object couldn't emulate undefined, this should have been folded.");

    Label* truthy = getJumpLabelForBranch(lir->ifTruthy());
    Label* falsy = getJumpLabelForBranch(lir->ifFalsy());
    Register input = ToRegister(lir->input());

    if (lir->mir()->operandMightEmulateUndefined()) {
        if (inputType == MIRType_ObjectOrNull)
            masm.branchTestPtr(Assembler::Zero, input, input, falsy);

        OutOfLineTestObject* ool = new(alloc()) OutOfLineTestObject();
        addOutOfLineCode(ool, lir->mir());

        testObjectEmulatesUndefined(input, falsy, truthy, ToRegister(lir->temp()), ool);
    } else {
        MOZ_ASSERT(inputType == MIRType_ObjectOrNull);
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
    if (lir->mir()->operandMightEmulateUndefined() && input->mightBeType(MIRType_Object)) {
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
            masm.branchPtr(Assembler::Equal, Address(input, JSObject::offsetOfGroup()),
                           ImmGCPtr(funcGroup), target->label());
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

    // Hold the incoming ObjectGroup.

    masm.loadPtr(Address(input, JSObject::offsetOfGroup()), temp);

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

    // Unknown function: jump to fallback block.

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
    const JSAtomState& names = GetJitContext()->runtime->names();
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
    masm.branch32(Assembler::AboveOrEqual, input, Imm32(StaticStrings::INT_STATIC_LIMIT), ool);

    // Fast path for small integers.
    masm.movePtr(ImmPtr(&GetJitContext()->runtime->staticStrings().intStaticTable), output);
    masm.loadPtr(BaseIndex(output, input, ScalePointer), output);
}

typedef JSFlatString* (*IntToStringFn)(ExclusiveContext*, int);
static const VMFunction IntToStringInfo = FunctionInfo<IntToStringFn>(Int32ToString<CanGC>);

void
CodeGenerator::visitIntToString(LIntToString* lir)
{
    Register input = ToRegister(lir->input());
    Register output = ToRegister(lir->output());

    OutOfLineCode* ool = oolCallVM(IntToStringInfo, lir, (ArgList(), input),
                                   StoreRegisterTo(output));

    emitIntToString(input, output, ool->entry());

    masm.bind(ool->rejoin());
}

typedef JSString* (*DoubleToStringFn)(ExclusiveContext*, double);
static const VMFunction DoubleToStringInfo = FunctionInfo<DoubleToStringFn>(NumberToString<CanGC>);

void
CodeGenerator::visitDoubleToString(LDoubleToString* lir)
{
    FloatRegister input = ToFloatRegister(lir->input());
    Register temp = ToRegister(lir->tempInt());
    Register output = ToRegister(lir->output());

    OutOfLineCode* ool = oolCallVM(DoubleToStringInfo, lir, (ArgList(), input),
                                   StoreRegisterTo(output));

    // Try double to integer conversion and run integer to string code.
    masm.convertDoubleToInt32(input, temp, ool->entry(), true);
    emitIntToString(temp, output, ool->entry());

    masm.bind(ool->rejoin());
}

typedef JSString* (*PrimitiveToStringFn)(JSContext*, HandleValue);
static const VMFunction PrimitiveToStringInfo = FunctionInfo<PrimitiveToStringFn>(ToStringSlow);

void
CodeGenerator::visitValueToString(LValueToString* lir)
{
    ValueOperand input = ToValue(lir, LValueToString::Input);
    Register output = ToRegister(lir->output());

    OutOfLineCode* ool = oolCallVM(PrimitiveToStringInfo, lir, (ArgList(), input),
                                   StoreRegisterTo(output));

    Label done;
    Register tag = masm.splitTagForTest(input);
    const JSAtomState& names = GetJitContext()->runtime->names();

    // String
    if (lir->mir()->input()->mightBeType(MIRType_String)) {
        Label notString;
        masm.branchTestString(Assembler::NotEqual, tag, &notString);
        masm.unboxString(input, output);
        masm.jump(&done);
        masm.bind(&notString);
    }

    // Integer
    if (lir->mir()->input()->mightBeType(MIRType_Int32)) {
        Label notInteger;
        masm.branchTestInt32(Assembler::NotEqual, tag, &notInteger);
        Register unboxed = ToTempUnboxRegister(lir->tempToUnbox());
        unboxed = masm.extractInt32(input, unboxed);
        emitIntToString(unboxed, output, ool->entry());
        masm.jump(&done);
        masm.bind(&notInteger);
    }

    // Double
    if (lir->mir()->input()->mightBeType(MIRType_Double)) {
        // Note: no fastpath. Need two extra registers and can only convert doubles
        // that fit integers and are smaller than StaticStrings::INT_STATIC_LIMIT.
        masm.branchTestDouble(Assembler::Equal, tag, ool->entry());
    }

    // Undefined
    if (lir->mir()->input()->mightBeType(MIRType_Undefined)) {
        Label notUndefined;
        masm.branchTestUndefined(Assembler::NotEqual, tag, &notUndefined);
        masm.movePtr(ImmGCPtr(names.undefined), output);
        masm.jump(&done);
        masm.bind(&notUndefined);
    }

    // Null
    if (lir->mir()->input()->mightBeType(MIRType_Null)) {
        Label notNull;
        masm.branchTestNull(Assembler::NotEqual, tag, &notNull);
        masm.movePtr(ImmGCPtr(names.null), output);
        masm.jump(&done);
        masm.bind(&notNull);
    }

    // Boolean
    if (lir->mir()->input()->mightBeType(MIRType_Boolean)) {
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
    if (lir->mir()->input()->mightBeType(MIRType_Object)) {
        // Bail.
        MOZ_ASSERT(lir->mir()->fallible());
        Label bail;
        masm.branchTestObject(Assembler::Equal, tag, &bail);
        bailoutFrom(&bail, lir->snapshot());
    }

    // Symbol
    if (lir->mir()->input()->mightBeType(MIRType_Symbol))
        masm.branchTestSymbol(Assembler::Equal, tag, ool->entry());

#ifdef DEBUG
    masm.assumeUnreachable("Unexpected type for MValueToString.");
#endif

    masm.bind(&done);
    masm.bind(ool->rejoin());
}

typedef JSObject* (*ToObjectFn)(JSContext*, HandleValue, bool);
static const VMFunction ToObjectInfo = FunctionInfo<ToObjectFn>(ToObjectSlow);

void
CodeGenerator::visitValueToObjectOrNull(LValueToObjectOrNull* lir)
{
    ValueOperand input = ToValue(lir, LValueToObjectOrNull::Input);
    Register output = ToRegister(lir->output());

    OutOfLineCode* ool = oolCallVM(ToObjectInfo, lir, (ArgList(), input, Imm32(0)),
                                   StoreRegisterTo(output));

    Label done;
    masm.branchTestObject(Assembler::Equal, input, &done);
    masm.branchTestNull(Assembler::NotEqual, input, ool->entry());

    masm.bind(&done);
    masm.unboxNonDouble(input, output);

    masm.bind(ool->rejoin());
}

typedef JSObject* (*CloneRegExpObjectFn)(JSContext*, JSObject*);
static const VMFunction CloneRegExpObjectInfo =
    FunctionInfo<CloneRegExpObjectFn>(CloneRegExpObject);

void
CodeGenerator::visitRegExp(LRegExp* lir)
{
    pushArg(ImmGCPtr(lir->mir()->source()));
    callVM(CloneRegExpObjectInfo, lir);
}

// The maximum number of pairs we can handle when executing RegExps inline.
static const size_t RegExpMaxPairCount = 6;

// Amount of space to reserve on the stack when executing RegExps inline.
static const size_t RegExpReservedStack = sizeof(irregexp::InputOutputData)
                                        + sizeof(MatchPairs)
                                        + RegExpMaxPairCount * sizeof(MatchPair);

static size_t
RegExpPairsVectorStartOffset(size_t inputOutputDataStartOffset)
{
    return inputOutputDataStartOffset + sizeof(irregexp::InputOutputData) + sizeof(MatchPairs);
}

static Address
RegExpPairCountAddress(size_t inputOutputDataStartOffset)
{
    return Address(StackPointer, inputOutputDataStartOffset
                                 + sizeof(irregexp::InputOutputData)
                                 + MatchPairs::offsetOfPairCount());
}

// Prepare an InputOutputData and optional MatchPairs which space has been
// allocated for on the stack, and try to execute a RegExp on a string input.
// If the RegExp was successfully executed and matched the input, fallthrough,
// otherwise jump to notFound or failure.
static bool
PrepareAndExecuteRegExp(JSContext* cx, MacroAssembler& masm, Register regexp, Register input,
                        Register temp1, Register temp2, Register temp3,
                        size_t inputOutputDataStartOffset,
                        RegExpShared::CompilationMode mode,
                        Label* notFound, Label* failure)
{
    size_t matchPairsStartOffset = inputOutputDataStartOffset + sizeof(irregexp::InputOutputData);
    size_t pairsVectorStartOffset = RegExpPairsVectorStartOffset(inputOutputDataStartOffset);

    Address inputStartAddress(StackPointer,
        inputOutputDataStartOffset + offsetof(irregexp::InputOutputData, inputStart));
    Address inputEndAddress(StackPointer,
        inputOutputDataStartOffset + offsetof(irregexp::InputOutputData, inputEnd));
    Address matchesPointerAddress(StackPointer,
        inputOutputDataStartOffset + offsetof(irregexp::InputOutputData, matches));
    Address startIndexAddress(StackPointer,
        inputOutputDataStartOffset + offsetof(irregexp::InputOutputData, startIndex));
    Address matchResultAddress(StackPointer,
        inputOutputDataStartOffset + offsetof(irregexp::InputOutputData, result));

    Address pairCountAddress = RegExpPairCountAddress(inputOutputDataStartOffset);
    Address pairsPointerAddress(StackPointer, matchPairsStartOffset + MatchPairs::offsetOfPairs());

    Address pairsVectorAddress(StackPointer, pairsVectorStartOffset);

    RegExpStatics* res = cx->global()->getRegExpStatics(cx);
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
    masm.branchIfRope(input, failure);

    // Get the RegExpShared for the RegExp.
    masm.loadPtr(Address(regexp, NativeObject::getFixedSlotOffset(RegExpObject::PRIVATE_SLOT)), temp1);
    masm.branchPtr(Assembler::Equal, temp1, ImmWord(0), failure);

    // Don't handle RegExps which read and write to lastIndex.
    masm.branchTest32(Assembler::NonZero, Address(temp1, RegExpShared::offsetOfFlags()),
                      Imm32(StickyFlag | GlobalFlag), failure);

    if (mode == RegExpShared::Normal) {
        // Don't handle RegExps with excessive parens.
        masm.load32(Address(temp1, RegExpShared::offsetOfParenCount()), temp2);
        masm.branch32(Assembler::AboveOrEqual, temp2, Imm32(RegExpMaxPairCount), failure);

        // Fill in the paren count in the MatchPairs on the stack.
        masm.add32(Imm32(1), temp2);
        masm.store32(temp2, pairCountAddress);
    }

    // Load the code pointer for the type of input string we have, and compute
    // the input start/end pointers in the InputOutputData.
    Register codePointer = temp1;
    {
        masm.loadStringChars(input, temp2);
        masm.storePtr(temp2, inputStartAddress);
        masm.loadStringLength(input, temp3);
        Label isLatin1, done;
        masm.branchTest32(Assembler::NonZero, Address(input, JSString::offsetOfFlags()),
                          Imm32(JSString::LATIN1_CHARS_BIT), &isLatin1);
        {
            masm.lshiftPtr(Imm32(1), temp3);
            masm.loadPtr(Address(temp1, RegExpShared::offsetOfJitCode(mode, false)), codePointer);
        }
        masm.jump(&done);
        {
            masm.bind(&isLatin1);
            masm.loadPtr(Address(temp1, RegExpShared::offsetOfJitCode(mode, true)), codePointer);
        }
        masm.bind(&done);
        masm.addPtr(temp3, temp2);
        masm.storePtr(temp2, inputEndAddress);
    }

    // Check the RegExpShared has been compiled for this type of input.
    masm.branchPtr(Assembler::Equal, codePointer, ImmWord(0), failure);
    masm.loadPtr(Address(codePointer, JitCode::offsetOfCode()), codePointer);

    // Don't handle execution inside a PreserveRegExpStatics instance.
    masm.branchPtr(Assembler::NotEqual, AbsoluteAddress(res->addressOfBufferLink()), ImmWord(0), failure);

    // Finish filling in the InputOutputData instance on the stack.
    if (mode == RegExpShared::Normal) {
        masm.computeEffectiveAddress(Address(StackPointer, matchPairsStartOffset), temp2);
        masm.storePtr(temp2, matchesPointerAddress);
    }
    masm.storePtr(ImmWord(0), startIndexAddress);
    masm.store32(Imm32(0), matchResultAddress);

    // Save any volatile inputs.
    GeneralRegisterSet volatileRegs;
    if (input.volatile_())
        volatileRegs.add(input);
    if (regexp.volatile_())
        volatileRegs.add(regexp);

    // Execute the RegExp.
    masm.computeEffectiveAddress(Address(StackPointer, inputOutputDataStartOffset), temp2);
    masm.PushRegsInMask(volatileRegs);
    masm.setupUnalignedABICall(1, temp3);
    masm.passABIArg(temp2);
    masm.callWithABI(codePointer);
    masm.PopRegsInMask(volatileRegs);

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

    masm.patchableCallPreBarrier(pendingInputAddress, MIRType_String);
    masm.patchableCallPreBarrier(matchesInputAddress, MIRType_String);
    masm.patchableCallPreBarrier(lazySourceAddress, MIRType_String);

    masm.storePtr(input, pendingInputAddress);
    masm.storePtr(input, matchesInputAddress);
    masm.storePtr(ImmWord(0), Address(temp1, RegExpStatics::offsetOfLazyIndex()));
    masm.store32(Imm32(1), Address(temp1, RegExpStatics::offsetOfPendingLazyEvaluation()));

    masm.loadPtr(Address(regexp, NativeObject::getFixedSlotOffset(RegExpObject::PRIVATE_SLOT)), temp2);
    masm.loadPtr(Address(temp2, RegExpShared::offsetOfSource()), temp3);
    masm.storePtr(temp3, lazySourceAddress);
    masm.load32(Address(temp2, RegExpShared::offsetOfFlags()), temp3);
    masm.store32(temp3, Address(temp1, RegExpStatics::offsetOfLazyFlags()));

    return true;
}

static void
CopyStringChars(MacroAssembler& masm, Register to, Register from, Register len,
                Register byteOpScratch, size_t fromWidth, size_t toWidth);

static void
CreateDependentString(MacroAssembler& masm, const JSAtomState& names,
                      bool latin1, Register string,
                      Register base, Register temp1, Register temp2,
                      BaseIndex startIndexAddress, BaseIndex limitIndexAddress,
                      Label* failure)
{
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
        masm.newGCString(string, temp2, failure);
        masm.store32(Imm32(thinFlags), Address(string, JSString::offsetOfFlags()));
        masm.jump(&stringAllocated);

        masm.bind(&fatInline);

        int32_t fatFlags = (latin1 ? JSString::LATIN1_CHARS_BIT : 0) | JSString::INIT_FAT_INLINE_FLAGS;
        masm.newGCFatInlineString(string, temp2, failure);
        masm.store32(Imm32(fatFlags), Address(string, JSString::offsetOfFlags()));

        masm.bind(&stringAllocated);
        masm.store32(temp1, Address(string, JSString::offsetOfLength()));

        masm.push(string);
        masm.push(base);

        // Adjust the start index address for the above pushes.
        MOZ_ASSERT(startIndexAddress.base == StackPointer);
        BaseIndex newStartIndexAddress = startIndexAddress;
        newStartIndexAddress.offset += 2 * sizeof(void*);

        // Load chars pointer for the new string.
        masm.addPtr(ImmWord(JSInlineString::offsetOfInlineStorage()), string);

        // Load the source characters pointer.
        masm.loadStringChars(base, base);
        masm.load32(newStartIndexAddress, temp2);
        if (latin1)
            masm.addPtr(temp2, base);
        else
            masm.computeEffectiveAddress(BaseIndex(base, temp2, TimesTwo), base);

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

        masm.newGCString(string, temp2, failure);
        masm.store32(Imm32(flags), Address(string, JSString::offsetOfFlags()));
        masm.store32(temp1, Address(string, JSString::offsetOfLength()));

        masm.loadPtr(Address(base, JSString::offsetOfNonInlineChars()), temp1);
        masm.load32(startIndexAddress, temp2);
        if (latin1)
            masm.addPtr(temp2, temp1);
        else
            masm.computeEffectiveAddress(BaseIndex(temp1, temp2, TimesTwo), temp1);
        masm.storePtr(temp1, Address(string, JSString::offsetOfNonInlineChars()));
        masm.storePtr(base, Address(string, JSDependentString::offsetOfBase()));

        // Follow any base pointer if the input is itself a dependent string.
        // Watch for undepended strings, which have a base pointer but don't
        // actually share their characters with it.
        Label noBase;
        masm.branchTest32(Assembler::Zero, Address(base, JSString::offsetOfFlags()),
                          Imm32(JSString::HAS_BASE_BIT), &noBase);
        masm.branchTest32(Assembler::NonZero, Address(base, JSString::offsetOfFlags()),
                          Imm32(JSString::FLAT_BIT), &noBase);
        masm.loadPtr(Address(base, JSDependentString::offsetOfBase()), temp1);
        masm.storePtr(temp1, Address(string, JSDependentString::offsetOfBase()));
        masm.bind(&noBase);
    }

    masm.bind(&done);
}

JitCode*
JitCompartment::generateRegExpExecStub(JSContext* cx)
{
    Register regexp = CallTempReg0;
    Register input = CallTempReg1;
    ValueOperand result = JSReturnOperand;

    // We are free to clobber all registers, as LRegExpExec is a call instruction.
    GeneralRegisterSet regs = GeneralRegisterSet::All();
    regs.take(input);
    regs.take(regexp);

    // temp5 is used in single byte instructions when creating dependent
    // strings, and has restrictions on which register it can be on some
    // platforms.
    Register temp5;
    {
        GeneralRegisterSet oregs = regs;
        do {
            temp5 = oregs.takeAny();
        } while (!MacroAssembler::canUseInSingleByteInstruction(temp5));
        regs.take(temp5);
    }

    Register temp1 = regs.takeAny();
    Register temp2 = regs.takeAny();
    Register temp3 = regs.takeAny();
    Register temp4 = regs.takeAny();

    ArrayObject* templateObject = cx->compartment()->regExps.getOrCreateMatchResultTemplateObject(cx);
    if (!templateObject)
        return nullptr;

    // The template object should have enough space for the maximum number of
    // pairs this stub can handle.
    MOZ_ASSERT(ObjectElements::VALUES_PER_HEADER + RegExpMaxPairCount ==
               gc::GetGCKindSlots(templateObject->asTenured().getAllocKind()));

    MacroAssembler masm(cx);

    // The InputOutputData is placed above the return address on the stack.
    size_t inputOutputDataStartOffset = sizeof(void*);

    Label notFound, oolEntry;
    if (!PrepareAndExecuteRegExp(cx, masm, regexp, input, temp1, temp2, temp3,
                                 inputOutputDataStartOffset, RegExpShared::Normal,
                                 &notFound, &oolEntry))
    {
        return nullptr;
    }

    // Construct the result.
    Register object = temp1;
    masm.createGCObject(object, temp2, templateObject, gc::DefaultHeap, &oolEntry);

    Register matchIndex = temp2;
    masm.move32(Imm32(0), matchIndex);

    size_t pairsVectorStartOffset = RegExpPairsVectorStartOffset(inputOutputDataStartOffset);
    Address pairsVectorAddress(StackPointer, pairsVectorStartOffset);
    Address pairCountAddress = RegExpPairCountAddress(inputOutputDataStartOffset);

    size_t elementsOffset = NativeObject::offsetOfFixedElements();
    BaseIndex stringAddress(object, matchIndex, TimesEight, elementsOffset);

    JS_STATIC_ASSERT(sizeof(MatchPair) == 8);
    BaseIndex stringIndexAddress(StackPointer, matchIndex, TimesEight,
                                 pairsVectorStartOffset + offsetof(MatchPair, start));
    BaseIndex stringLimitAddress(StackPointer, matchIndex, TimesEight,
                                 pairsVectorStartOffset + offsetof(MatchPair, limit));

    // Loop to construct the match strings. There are two different loops,
    // depending on whether the input is latin1.
    {
        Label isLatin1, done;
        masm.branchTest32(Assembler::NonZero, Address(input, JSString::offsetOfFlags()),
                          Imm32(JSString::LATIN1_CHARS_BIT), &isLatin1);

        for (int isLatin = 0; isLatin <= 1; isLatin++) {
            if (isLatin)
                masm.bind(&isLatin1);

            Label matchLoop;
            masm.bind(&matchLoop);

            Label isUndefined, storeDone;
            masm.branch32(Assembler::LessThan, stringIndexAddress, Imm32(0), &isUndefined);

            CreateDependentString(masm, cx->names(), isLatin, temp3, input, temp4, temp5,
                                  stringIndexAddress, stringLimitAddress, &oolEntry);
            masm.storeValue(JSVAL_TYPE_STRING, temp3, stringAddress);

            masm.jump(&storeDone);
            masm.bind(&isUndefined);

            masm.storeValue(UndefinedValue(), stringAddress);
            masm.bind(&storeDone);

            masm.add32(Imm32(1), matchIndex);
            masm.branch32(Assembler::LessThanOrEqual, pairCountAddress, matchIndex, &done);
            masm.jump(&matchLoop);
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
    masm.storeValue(JSVAL_TYPE_STRING, input, Address(temp2, sizeof(Value)));

    // All done!
    masm.tagValue(JSVAL_TYPE_OBJECT, object, result);
    masm.ret();

    masm.bind(&notFound);
    masm.moveValue(NullValue(), result);
    masm.ret();

    // Use an undefined value to signal to the caller that the OOL stub needs to be called.
    masm.bind(&oolEntry);
    masm.moveValue(UndefinedValue(), result);
    masm.ret();

    Linker linker(masm);
    AutoFlushICache afc("RegExpExecStub");
    JitCode* code = linker.newCode<CanGC>(cx, OTHER_CODE);

#ifdef JS_ION_PERF
    writePerfSpewerJitCodeProfile(code, "RegExpExecStub");
#endif

    if (cx->zone()->needsIncrementalBarrier())
        code->togglePreBarriers(true);

    return code;
}

class OutOfLineRegExpExec : public OutOfLineCodeBase<CodeGenerator>
{
    LRegExpExec* lir_;

  public:
    explicit OutOfLineRegExpExec(LRegExpExec* lir)
      : lir_(lir)
    { }

    void accept(CodeGenerator* codegen) {
        codegen->visitOutOfLineRegExpExec(this);
    }

    LRegExpExec* lir() const {
        return lir_;
    }
};

typedef bool (*RegExpExecRawFn)(JSContext* cx, HandleObject regexp,
                                HandleString input, MatchPairs* pairs, MutableHandleValue output);
static const VMFunction RegExpExecRawInfo = FunctionInfo<RegExpExecRawFn>(regexp_exec_raw);

void
CodeGenerator::visitOutOfLineRegExpExec(OutOfLineRegExpExec* ool)
{
    LRegExpExec* lir = ool->lir();
    Register input = ToRegister(lir->string());
    Register regexp = ToRegister(lir->regexp());

    GeneralRegisterSet regs = GeneralRegisterSet::All();
    regs.take(input);
    regs.take(regexp);
    Register temp = regs.takeAny();

    masm.computeEffectiveAddress(Address(StackPointer, sizeof(irregexp::InputOutputData)), temp);

    pushArg(temp);
    pushArg(input);
    pushArg(regexp);

    callVM(RegExpExecRawInfo, lir);

    masm.jump(ool->rejoin());
}

void
CodeGenerator::visitRegExpExec(LRegExpExec* lir)
{
    MOZ_ASSERT(ToRegister(lir->regexp()) == CallTempReg0);
    MOZ_ASSERT(ToRegister(lir->string()) == CallTempReg1);
    MOZ_ASSERT(GetValueOutput(lir) == JSReturnOperand);

    masm.reserveStack(RegExpReservedStack);

    OutOfLineRegExpExec* ool = new(alloc()) OutOfLineRegExpExec(lir);
    addOutOfLineCode(ool, lir->mir());

    JitCode* regExpExecStub = gen->compartment->jitCompartment()->regExpExecStubNoBarrier();
    masm.call(regExpExecStub);
    masm.branchTestUndefined(Assembler::Equal, JSReturnOperand, ool->entry());
    masm.bind(ool->rejoin());

    masm.freeStack(RegExpReservedStack);
}

// The value returned by the RegExp test stub if inline execution failed.
static const int32_t RegExpTestFailedValue = 2;

JitCode*
JitCompartment::generateRegExpTestStub(JSContext* cx)
{
    Register regexp = CallTempReg2;
    Register input = CallTempReg3;
    Register result = ReturnReg;

    MOZ_ASSERT(regexp != result && input != result);

    // We are free to clobber all registers, as LRegExpTest is a call instruction.
    GeneralRegisterSet regs = GeneralRegisterSet::All();
    regs.take(input);
    regs.take(regexp);
    Register temp1 = regs.takeAny();
    Register temp2 = regs.takeAny();
    Register temp3 = regs.takeAny();

    MacroAssembler masm(cx);

#ifdef JS_USE_LINK_REGISTER
    masm.pushReturnAddress();
#endif

    masm.reserveStack(sizeof(irregexp::InputOutputData));

    Label notFound, oolEntry;
    if (!PrepareAndExecuteRegExp(cx, masm, regexp, input, temp1, temp2, temp3, 0,
                                 RegExpShared::MatchOnly, &notFound, &oolEntry))
    {
        return nullptr;
    }

    Label done;

    masm.move32(Imm32(1), result);
    masm.jump(&done);

    masm.bind(&notFound);
    masm.move32(Imm32(0), result);
    masm.jump(&done);

    masm.bind(&oolEntry);
    masm.move32(Imm32(RegExpTestFailedValue), result);

    masm.bind(&done);
    masm.freeStack(sizeof(irregexp::InputOutputData));
    masm.ret();

    Linker linker(masm);
    AutoFlushICache afc("RegExpTestStub");
    JitCode* code = linker.newCode<CanGC>(cx, OTHER_CODE);

#ifdef JS_ION_PERF
    writePerfSpewerJitCodeProfile(code, "RegExpTestStub");
#endif

    if (cx->zone()->needsIncrementalBarrier())
        code->togglePreBarriers(true);

    return code;
}

class OutOfLineRegExpTest : public OutOfLineCodeBase<CodeGenerator>
{
    LRegExpTest* lir_;

  public:
    explicit OutOfLineRegExpTest(LRegExpTest* lir)
      : lir_(lir)
    { }

    void accept(CodeGenerator* codegen) {
        codegen->visitOutOfLineRegExpTest(this);
    }

    LRegExpTest* lir() const {
        return lir_;
    }
};

typedef bool (*RegExpTestRawFn)(JSContext* cx, HandleObject regexp,
                                HandleString input, bool* result);
static const VMFunction RegExpTestRawInfo = FunctionInfo<RegExpTestRawFn>(regexp_test_raw);

void
CodeGenerator::visitOutOfLineRegExpTest(OutOfLineRegExpTest* ool)
{
    LRegExpTest* lir = ool->lir();
    Register input = ToRegister(lir->string());
    Register regexp = ToRegister(lir->regexp());

    pushArg(input);
    pushArg(regexp);

    callVM(RegExpTestRawInfo, lir);

    masm.jump(ool->rejoin());
}

void
CodeGenerator::visitRegExpTest(LRegExpTest* lir)
{
    MOZ_ASSERT(ToRegister(lir->regexp()) == CallTempReg2);
    MOZ_ASSERT(ToRegister(lir->string()) == CallTempReg3);
    MOZ_ASSERT(ToRegister(lir->output()) == ReturnReg);

    OutOfLineRegExpTest* ool = new(alloc()) OutOfLineRegExpTest(lir);
    addOutOfLineCode(ool, lir->mir());

    JitCode* regExpTestStub = gen->compartment->jitCompartment()->regExpTestStubNoBarrier();
    masm.call(regExpTestStub);

    masm.branch32(Assembler::Equal, ReturnReg, Imm32(RegExpTestFailedValue), ool->entry());
    masm.bind(ool->rejoin());
}

typedef JSString* (*RegExpReplaceFn)(JSContext*, HandleString, HandleObject, HandleString);
static const VMFunction RegExpReplaceInfo = FunctionInfo<RegExpReplaceFn>(RegExpReplace);

void
CodeGenerator::visitRegExpReplace(LRegExpReplace* lir)
{
    if (lir->replacement()->isConstant())
        pushArg(ImmGCPtr(lir->replacement()->toConstant()->toString()));
    else
        pushArg(ToRegister(lir->replacement()));

    pushArg(ToRegister(lir->pattern()));

    if (lir->string()->isConstant())
        pushArg(ImmGCPtr(lir->string()->toConstant()->toString()));
    else
        pushArg(ToRegister(lir->string()));

    callVM(RegExpReplaceInfo, lir);
}

typedef JSString* (*StringReplaceFn)(JSContext*, HandleString, HandleString, HandleString);
static const VMFunction StringReplaceInfo = FunctionInfo<StringReplaceFn>(StringReplace);

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

    callVM(StringReplaceInfo, lir);
}

typedef JSObject* (*LambdaFn)(JSContext*, HandleFunction, HandleObject);
static const VMFunction LambdaInfo = FunctionInfo<LambdaFn>(js::Lambda);

void
CodeGenerator::visitLambdaForSingleton(LLambdaForSingleton* lir)
{
    pushArg(ToRegister(lir->scopeChain()));
    pushArg(ImmGCPtr(lir->mir()->info().fun));
    callVM(LambdaInfo, lir);
}

void
CodeGenerator::visitLambda(LLambda* lir)
{
    Register scopeChain = ToRegister(lir->scopeChain());
    Register output = ToRegister(lir->output());
    Register tempReg = ToRegister(lir->temp());
    const LambdaFunctionInfo& info = lir->mir()->info();

    OutOfLineCode* ool = oolCallVM(LambdaInfo, lir, (ArgList(), ImmGCPtr(info.fun), scopeChain),
                                   StoreRegisterTo(output));

    MOZ_ASSERT(!info.singletonType);

    masm.createGCObject(output, tempReg, info.fun, gc::DefaultHeap, ool->entry());

    emitLambdaInit(output, scopeChain, info);

    masm.bind(ool->rejoin());
}

typedef JSObject* (*LambdaArrowFn)(JSContext*, HandleFunction, HandleObject, HandleValue);
static const VMFunction LambdaArrowInfo = FunctionInfo<LambdaArrowFn>(js::LambdaArrow);

void
CodeGenerator::visitLambdaArrow(LLambdaArrow* lir)
{
    Register scopeChain = ToRegister(lir->scopeChain());
    ValueOperand thisv = ToValue(lir, LLambdaArrow::ThisValue);
    Register output = ToRegister(lir->output());
    Register tempReg = ToRegister(lir->temp());
    const LambdaFunctionInfo& info = lir->mir()->info();

    OutOfLineCode* ool = oolCallVM(LambdaArrowInfo, lir,
                                   (ArgList(), ImmGCPtr(info.fun), scopeChain, thisv),
                                   StoreRegisterTo(output));

    MOZ_ASSERT(!info.useSingletonForClone);

    if (info.singletonType) {
        // If the function has a singleton type, this instruction will only be
        // executed once so we don't bother inlining it.
        masm.jump(ool->entry());
        masm.bind(ool->rejoin());
        return;
    }

    masm.createGCObject(output, tempReg, info.fun, gc::DefaultHeap, ool->entry());

    emitLambdaInit(output, scopeChain, info);

    // Initialize extended slots. Lexical |this| is stored in the first one.
    MOZ_ASSERT(info.flags & JSFunction::EXTENDED);
    static_assert(FunctionExtended::NUM_EXTENDED_SLOTS == 2, "All slots must be initialized");
    static_assert(FunctionExtended::ARROW_THIS_SLOT == 0, "|this| must be stored in first slot");
    masm.storeValue(thisv, Address(output, FunctionExtended::offsetOfExtendedSlot(0)));
    masm.storeValue(UndefinedValue(), Address(output, FunctionExtended::offsetOfExtendedSlot(1)));

    masm.bind(ool->rejoin());
}

void
CodeGenerator::emitLambdaInit(Register output, Register scopeChain,
                              const LambdaFunctionInfo& info)
{
    MOZ_ASSERT(!!(info.flags & JSFunction::ARROW) == !!(info.flags & JSFunction::EXTENDED));

    // Initialize nargs and flags. We do this with a single uint32 to avoid
    // 16-bit writes.
    union {
        struct S {
            uint16_t nargs;
            uint16_t flags;
        } s;
        uint32_t word;
    } u;
    u.s.nargs = info.fun->nargs();
    u.s.flags = info.flags;

    MOZ_ASSERT(JSFunction::offsetOfFlags() == JSFunction::offsetOfNargs() + 2);
    masm.store32(Imm32(u.word), Address(output, JSFunction::offsetOfNargs()));
    masm.storePtr(ImmGCPtr(info.scriptOrLazyScript),
                  Address(output, JSFunction::offsetOfNativeOrScript()));
    masm.storePtr(scopeChain, Address(output, JSFunction::offsetOfEnvironment()));
    masm.storePtr(ImmGCPtr(info.fun->displayAtom()), Address(output, JSFunction::offsetOfAtom()));
}

void
CodeGenerator::visitLabel(LLabel* lir)
{
}

void
CodeGenerator::visitNop(LNop* lir)
{
}

void
CodeGenerator::visitMop(LMop* lir)
{
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
        if (*iter == lir || iter->isNop())
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
    LInterruptCheckImplicit* lir;

    OutOfLineInterruptCheckImplicit(LBlock* block, LInterruptCheckImplicit* lir)
      : block(block), lir(lir)
    { }

    void accept(CodeGenerator* codegen) {
        codegen->visitOutOfLineInterruptCheckImplicit(this);
    }
};

typedef bool (*InterruptCheckFn)(JSContext*);
static const VMFunction InterruptCheckInfo = FunctionInfo<InterruptCheckFn>(InterruptCheck);

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
        if (iter->isLabel()) {
            // Nothing to do.
        } else if (iter->isMoveGroup()) {
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
CodeGenerator::visitInterruptCheckImplicit(LInterruptCheckImplicit* lir)
{
    OutOfLineInterruptCheckImplicit* ool = new(alloc()) OutOfLineInterruptCheckImplicit(current, lir);
    addOutOfLineCode(ool, lir->mir());

    lir->setOolEntry(ool->entry());
    masm.bind(ool->rejoin());
}

void
CodeGenerator::visitTableSwitch(LTableSwitch* ins)
{
    MTableSwitch* mir = ins->mir();
    Label* defaultcase = skipTrivialBlocks(mir->getDefault())->lir()->label();
    const LAllocation* temp;

    if (mir->getOperand(0)->type() != MIRType_Int32) {
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

typedef NativeObject* (*DeepCloneObjectLiteralFn)(JSContext*, HandleNativeObject, NewObjectKind);
static const VMFunction DeepCloneObjectLiteralInfo =
    FunctionInfo<DeepCloneObjectLiteralFn>(DeepCloneObjectLiteral);

void
CodeGenerator::visitCloneLiteral(LCloneLiteral* lir)
{
    pushArg(ImmWord(js::MaybeSingletonObject));
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
    Address ptr(StackPointer, frameSize() + JitFrameLayout::offsetOfCalleeToken());

    masm.loadFunctionFromCalleeToken(ptr, callee);
}

void
CodeGenerator::visitIsConstructing(LIsConstructing* lir)
{
    Register output = ToRegister(lir->output());
    Address calleeToken(StackPointer, frameSize() + JitFrameLayout::offsetOfCalleeToken());
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
        masm.profilerEnterFrame(StackPointer, temp);

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
CodeGenerator::visitOsrScopeChain(LOsrScopeChain* lir)
{
    const LAllocation* frame   = lir->getOperand(0);
    const LDefinition* object  = lir->getDef(0);

    const ptrdiff_t frameOffset = BaselineFrame::reverseOffsetOfScopeChain();

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
    Address dest(StackPointer, stack_offset);

    if (arg->isFloatReg())
        masm.storeDouble(ToFloatRegister(arg), dest);
    else if (arg->isRegister())
        masm.storeValue(ValueTypeFromMIRType(argType), ToRegister(arg), dest);
    else
        masm.storeValue(*(arg->toConstant()), dest);
}

void
CodeGenerator::visitStackArgV(LStackArgV* lir)
{
    ValueOperand val = ToValue(lir, 0);
    uint32_t argslot = lir->argslot();
    MOZ_ASSERT(argslot - 1u < graph.argumentSlotCount());

    int32_t stack_offset = StackOffsetOfPassedArg(argslot);

    masm.storeValue(val, Address(StackPointer, stack_offset));
}

void
CodeGenerator::visitMoveGroup(LMoveGroup* group)
{
    if (!group->numMoves())
        return;

    MoveResolver& resolver = masm.moveResolver();

    for (size_t i = 0; i < group->numMoves(); i++) {
        const LMove& move = group->getMove(i);

        const LAllocation* from = move.from();
        const LAllocation* to = move.to();
        LDefinition::Type type = move.type();

        // No bogus moves.
        MOZ_ASSERT(*from != *to);
        MOZ_ASSERT(!from->isConstant());
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
          case LDefinition::GENERAL:    moveType = MoveOp::GENERAL;   break;
          case LDefinition::INT32:      moveType = MoveOp::INT32;     break;
          case LDefinition::FLOAT32:    moveType = MoveOp::FLOAT32;   break;
          case LDefinition::DOUBLE:     moveType = MoveOp::DOUBLE;    break;
          case LDefinition::INT32X4:    moveType = MoveOp::INT32X4;   break;
          case LDefinition::FLOAT32X4:  moveType = MoveOp::FLOAT32X4; break;
          default: MOZ_CRASH("Unexpected move type");
        }

        masm.propagateOOM(resolver.addMove(toMoveOperand(from), toMoveOperand(to), moveType));
    }

    masm.propagateOOM(resolver.resolve());

    MoveEmitter emitter(masm);
    if (group->maybeScratchRegister().isGeneralReg())
        emitter.setScratchRegister(group->maybeScratchRegister().toGeneralReg()->reg());
    emitter.emit(resolver);
    emitter.finish();
}

void
CodeGenerator::visitInteger(LInteger* lir)
{
    masm.move32(Imm32(lir->getValue()), ToRegister(lir->output()));
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
CodeGenerator::visitNurseryObject(LNurseryObject* lir)
{
    Register output = ToRegister(lir->output());
    uint32_t index = lir->mir()->index();

    // Store a dummy JSObject pointer. We will fix it up on the main thread,
    // in JitCode::fixupNurseryObjects. The low bit is set to distinguish
    // it from a real JSObject pointer.
    JSObject* ptr = reinterpret_cast<JSObject*>((uintptr_t(index) << 1) | 1);
    masm.movePtr(ImmGCPtr(IonNurseryPtr(ptr)), output);
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

    if (valueType == MIRType_ObjectOrNull) {
        masm.storeObjectOrNull(ToRegister(lir->value()), dest);
    } else {
        ConstantOrRegister value;
        if (lir->value()->isConstant())
            value = ConstantOrRegister(*lir->value()->toConstant());
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

void
CodeGenerator::emitGetPropertyPolymorphic(LInstruction* ins, Register obj, Register scratch,
                                          const TypedOrValueRegister& output)
{
    MGetPropertyPolymorphic* mir = ins->mirRaw()->toGetPropertyPolymorphic();

    size_t total = mir->numUnboxedGroups() + mir->numShapes();
    MOZ_ASSERT(total > 1);

    bool groupInScratch = mir->numUnboxedGroups() > 1;
    bool shapeInScratch = mir->numShapes() > 1;

    Label done;

    for (size_t i = 0; i < total; i++) {
        bool unboxedGroup = i < mir->numUnboxedGroups();

        ImmGCPtr comparePtr = unboxedGroup
                              ? ImmGCPtr(mir->unboxedGroup(i))
                              : ImmGCPtr(mir->objShape(i - mir->numUnboxedGroups()));
        Address addr(obj, unboxedGroup ? JSObject::offsetOfGroup() : JSObject::offsetOfShape());

        if ((i == 0 && groupInScratch) || (i == mir->numUnboxedGroups() && shapeInScratch))
            masm.loadPtr(addr, scratch);

        bool inScratch = unboxedGroup ? groupInScratch : shapeInScratch;

        Label next;
        if (i == total - 1) {
            if (inScratch)
                bailoutCmpPtr(Assembler::NotEqual, scratch, comparePtr, ins->snapshot());
            else
                bailoutCmpPtr(Assembler::NotEqual, addr, comparePtr, ins->snapshot());
        } else {
            if (inScratch)
                masm.branchPtr(Assembler::NotEqual, scratch, comparePtr, &next);
            else
                masm.branchPtr(Assembler::NotEqual, addr, comparePtr, &next);
        }

        if (unboxedGroup) {
            const UnboxedLayout::Property* property =
                mir->unboxedGroup(i)->unboxedLayout().lookup(mir->name());
            Address propertyAddr(obj, UnboxedPlainObject::offsetOfData() + property->offset);

            masm.loadUnboxedProperty(propertyAddr, property->type, output);
        } else {
            Shape* shape = mir->shape(i - mir->numUnboxedGroups());
            if (shape->slot() < shape->numFixedSlots()) {
                // Fixed slot.
                masm.loadTypedOrValue(Address(obj, NativeObject::getFixedSlotOffset(shape->slot())),
                                      output);
            } else {
                // Dynamic slot.
                uint32_t offset = (shape->slot() - shape->numFixedSlots()) * sizeof(js::Value);
                masm.loadPtr(Address(obj, NativeObject::offsetOfSlots()), scratch);
                masm.loadTypedOrValue(Address(scratch, offset), output);
            }
        }

        if (i != total - 1)
            masm.jump(&done);
        masm.bind(&next);
    }

    masm.bind(&done);
}

void
CodeGenerator::visitGetPropertyPolymorphicV(LGetPropertyPolymorphicV* ins)
{
    Register obj = ToRegister(ins->obj());
    ValueOperand output = GetValueOutput(ins);
    emitGetPropertyPolymorphic(ins, obj, output.scratchReg(), output);
}

void
CodeGenerator::visitGetPropertyPolymorphicT(LGetPropertyPolymorphicT* ins)
{
    Register obj = ToRegister(ins->obj());
    TypedOrValueRegister output(ins->mir()->type(), ToAnyRegister(ins->output()));
    Register temp = (output.type() == MIRType_Double)
                    ? ToRegister(ins->temp())
                    : output.typedReg().gpr();
    emitGetPropertyPolymorphic(ins, obj, temp, output);
}

void
CodeGenerator::emitSetPropertyPolymorphic(LInstruction* ins, Register obj, Register scratch,
                                          const ConstantOrRegister& value)
{
    MSetPropertyPolymorphic* mir = ins->mirRaw()->toSetPropertyPolymorphic();

    size_t total = mir->numUnboxedGroups() + mir->numShapes();
    MOZ_ASSERT(total > 1);

    bool groupInScratch = mir->numUnboxedGroups() > 1;
    bool shapeInScratch = mir->numShapes() > 1;

    Label done;
    for (size_t i = 0; i < total; i++) {
        bool unboxedGroup = i < mir->numUnboxedGroups();

        ImmGCPtr comparePtr = unboxedGroup
                              ? ImmGCPtr(mir->unboxedGroup(i))
                              : ImmGCPtr(mir->objShape(i - mir->numUnboxedGroups()));
        Address addr(obj, unboxedGroup ? JSObject::offsetOfGroup() : JSObject::offsetOfShape());

        if ((i == 0 && groupInScratch) || (i == mir->numUnboxedGroups() && shapeInScratch))
            masm.loadPtr(addr, scratch);

        bool inScratch = unboxedGroup ? groupInScratch : shapeInScratch;

        Label next;
        if (i == total - 1) {
            if (inScratch)
                bailoutCmpPtr(Assembler::NotEqual, scratch, comparePtr, ins->snapshot());
            else
                bailoutCmpPtr(Assembler::NotEqual, addr, comparePtr, ins->snapshot());
        } else {
            if (inScratch)
                masm.branchPtr(Assembler::NotEqual, scratch, comparePtr, &next);
            else
                masm.branchPtr(Assembler::NotEqual, addr, comparePtr, &next);
        }

        if (unboxedGroup) {
            const UnboxedLayout::Property* property =
                mir->unboxedGroup(i)->unboxedLayout().lookup(mir->name());
            Address propertyAddr(obj, UnboxedPlainObject::offsetOfData() + property->offset);

            if (property->type == JSVAL_TYPE_OBJECT)
                masm.patchableCallPreBarrier(propertyAddr, MIRType_Object);
            else if (property->type == JSVAL_TYPE_STRING)
                masm.patchableCallPreBarrier(propertyAddr, MIRType_String);
            else
                MOZ_ASSERT(!UnboxedTypeNeedsPreBarrier(property->type));

            masm.storeUnboxedProperty(propertyAddr, property->type, value, nullptr);
        } else {
            Shape* shape = mir->shape(i - mir->numUnboxedGroups());
            if (shape->slot() < shape->numFixedSlots()) {
                // Fixed slot.
                Address addr(obj, NativeObject::getFixedSlotOffset(shape->slot()));
                if (mir->needsBarrier())
                    emitPreBarrier(addr);
                masm.storeConstantOrRegister(value, addr);
            } else {
                // Dynamic slot.
                masm.loadPtr(Address(obj, NativeObject::offsetOfSlots()), scratch);
                Address addr(scratch, (shape->slot() - shape->numFixedSlots()) * sizeof(js::Value));
                if (mir->needsBarrier())
                    emitPreBarrier(addr);
                masm.storeConstantOrRegister(value, addr);
            }
        }

        if (i != total - 1)
            masm.jump(&done);
        masm.bind(&next);
    }

    masm.bind(&done);
}

void
CodeGenerator::visitSetPropertyPolymorphicV(LSetPropertyPolymorphicV* ins)
{
    Register obj = ToRegister(ins->obj());
    Register temp = ToRegister(ins->temp());
    ValueOperand value = ToValue(ins, LSetPropertyPolymorphicV::Value);
    emitSetPropertyPolymorphic(ins, obj, temp, TypedOrValueRegister(value));
}

void
CodeGenerator::visitSetPropertyPolymorphicT(LSetPropertyPolymorphicT* ins)
{
    Register obj = ToRegister(ins->obj());
    Register temp = ToRegister(ins->temp());

    ConstantOrRegister value;
    if (ins->mir()->value()->isConstant())
        value = ConstantOrRegister(ins->mir()->value()->toConstant()->value());
    else
        value = TypedOrValueRegister(ins->mir()->value()->type(), ToAnyRegister(ins->value()));

    emitSetPropertyPolymorphic(ins, obj, temp, value);
}

void
CodeGenerator::visitElements(LElements* lir)
{
    Address elements(ToRegister(lir->object()), NativeObject::offsetOfElements());
    masm.loadPtr(elements, ToRegister(lir->output()));
}

typedef bool (*ConvertElementsToDoublesFn)(JSContext*, uintptr_t);
static const VMFunction ConvertElementsToDoublesInfo =
    FunctionInfo<ConvertElementsToDoublesFn>(ObjectElements::ConvertElementsToDoubles);

void
CodeGenerator::visitConvertElementsToDoubles(LConvertElementsToDoubles* lir)
{
    Register elements = ToRegister(lir->elements());

    OutOfLineCode* ool = oolCallVM(ConvertElementsToDoublesInfo, lir,
                                   (ArgList(), elements), StoreNothing());

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
    masm.boxDouble(temp, out);

    masm.bind(&done);
}

typedef bool (*CopyElementsForWriteFn)(ExclusiveContext*, NativeObject*);
static const VMFunction CopyElementsForWriteInfo =
    FunctionInfo<CopyElementsForWriteFn>(NativeObject::CopyElementsForWrite);

void
CodeGenerator::visitMaybeCopyElementsForWrite(LMaybeCopyElementsForWrite* lir)
{
    Register object = ToRegister(lir->object());
    Register temp = ToRegister(lir->temp());

    OutOfLineCode* ool = oolCallVM(CopyElementsForWriteInfo, lir,
                                   (ArgList(), object), StoreNothing());

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
CodeGenerator::visitGuardObjectIdentity(LGuardObjectIdentity* guard)
{
    Register input = ToRegister(guard->input());
    Register expected = ToRegister(guard->expected());

    Assembler::Condition cond =
        guard->mir()->bailOnEquality() ? Assembler::Equal : Assembler::NotEqual;
    bailoutCmpPtr(cond, input, expected, guard->snapshot());
}

void
CodeGenerator::visitGuardShapePolymorphic(LGuardShapePolymorphic* lir)
{
    const MGuardShapePolymorphic* mir = lir->mir();
    Register obj = ToRegister(lir->object());
    Register temp = ToRegister(lir->temp());

    MOZ_ASSERT(mir->numShapes() > 1);

    Label done;
    masm.loadObjShape(obj, temp);

    for (size_t i = 0; i < mir->numShapes(); i++) {
        Shape* shape = mir->getShape(i);
        if (i == mir->numShapes() - 1)
            bailoutCmpPtr(Assembler::NotEqual, temp, ImmGCPtr(shape), lir->snapshot());
        else
            masm.branchPtr(Assembler::Equal, temp, ImmGCPtr(shape), &done);
    }

    masm.bind(&done);
}

void
CodeGenerator::visitTypeBarrierV(LTypeBarrierV* lir)
{
    ValueOperand operand = ToValue(lir, LTypeBarrierV::Input);
    Register scratch = ToTempRegisterOrInvalid(lir->temp());

    Label miss;
    masm.guardTypeSet(operand, lir->mir()->resultTypeSet(), lir->mir()->barrierKind(), scratch, &miss);
    bailoutFrom(&miss, lir->snapshot());
}

void
CodeGenerator::visitTypeBarrierO(LTypeBarrierO* lir)
{
    Register obj = ToRegister(lir->object());
    Register scratch = ToTempRegisterOrInvalid(lir->temp());
    Label miss, ok;

    if (lir->mir()->type() == MIRType_ObjectOrNull) {
        Label* nullTarget = lir->mir()->resultTypeSet()->mightBeMIRType(MIRType_Null) ? &ok : &miss;
        masm.branchTestPtr(Assembler::Zero, obj, obj, nullTarget);
    } else {
        MOZ_ASSERT(lir->mir()->type() == MIRType_Object);
        MOZ_ASSERT(lir->mir()->barrierKind() != BarrierKind::TypeTagOnly);
    }

    if (lir->mir()->barrierKind() != BarrierKind::TypeTagOnly)
        masm.guardObjectType(obj, lir->mir()->resultTypeSet(), scratch, &miss);

    bailoutFrom(&miss, lir->snapshot());
    masm.bind(&ok);
}

void
CodeGenerator::visitMonitorTypes(LMonitorTypes* lir)
{
    ValueOperand operand = ToValue(lir, LMonitorTypes::Input);
    Register scratch = ToTempUnboxRegister(lir->temp());

    Label matched, miss;
    masm.guardTypeSet(operand, lir->mir()->typeSet(), lir->mir()->barrierKind(), scratch, &miss);
    bailoutFrom(&miss, lir->snapshot());
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

    void accept(CodeGenerator* codegen) {
        codegen->visitOutOfLineCallPostWriteBarrier(this);
    }

    LInstruction* lir() const {
        return lir_;
    }
    const LAllocation* object() const {
        return object_;
    }
};

void
CodeGenerator::visitOutOfLineCallPostWriteBarrier(OutOfLineCallPostWriteBarrier* ool)
{
    saveLiveVolatile(ool->lir());

    const LAllocation* obj = ool->object();

    GeneralRegisterSet regs = GeneralRegisterSet::Volatile();

    Register objreg;
    bool isGlobal = false;
    if (obj->isConstant()) {
        JSObject* object = &obj->toConstant()->toObject();
        isGlobal = object->is<GlobalObject>();
        objreg = regs.takeAny();
        masm.movePtr(ImmGCPtr(object), objreg);
    } else {
        objreg = ToRegister(obj);
        regs.takeUnchecked(objreg);
    }

    Register runtimereg = regs.takeAny();
    masm.mov(ImmPtr(GetJitContext()->runtime), runtimereg);

    void (*fun)(JSRuntime*, JSObject*) = isGlobal ? PostGlobalWriteBarrier : PostWriteBarrier;
    masm.setupUnalignedABICall(2, regs.takeAny());
    masm.passABIArg(runtimereg);
    masm.passABIArg(objreg);
    masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, fun));

    restoreLiveVolatile(ool->lir());

    masm.jump(ool->rejoin());
}

void
CodeGenerator::visitPostWriteBarrierO(LPostWriteBarrierO* lir)
{
    OutOfLineCallPostWriteBarrier* ool = new(alloc()) OutOfLineCallPostWriteBarrier(lir, lir->object());
    addOutOfLineCode(ool, lir->mir());

    Register temp = ToTempRegisterOrInvalid(lir->temp());

    if (lir->object()->isConstant()) {
#ifdef DEBUG
        MOZ_ASSERT(!IsInsideNursery(&lir->object()->toConstant()->toObject()));
#endif
    } else {
        masm.branchPtrInNurseryRange(Assembler::Equal, ToRegister(lir->object()), temp,
                                     ool->rejoin());
    }

    masm.branchPtrInNurseryRange(Assembler::Equal, ToRegister(lir->value()), temp, ool->entry());

    masm.bind(ool->rejoin());
}

void
CodeGenerator::visitPostWriteBarrierV(LPostWriteBarrierV* lir)
{
    OutOfLineCallPostWriteBarrier* ool = new(alloc()) OutOfLineCallPostWriteBarrier(lir, lir->object());
    addOutOfLineCode(ool, lir->mir());

    Register temp = ToTempRegisterOrInvalid(lir->temp());

    if (lir->object()->isConstant()) {
#ifdef DEBUG
        MOZ_ASSERT(!IsInsideNursery(&lir->object()->toConstant()->toObject()));
#endif
    } else {
        masm.branchPtrInNurseryRange(Assembler::Equal, ToRegister(lir->object()), temp,
                                     ool->rejoin());
    }

    ValueOperand value = ToValue(lir, LPostWriteBarrierV::Input);
    masm.branchValueIsNurseryObject(Assembler::Equal, value, temp, ool->entry());

    masm.bind(ool->rejoin());
}

void
CodeGenerator::visitCallNative(LCallNative* call)
{
    JSFunction* target = call->getSingleTarget();
    MOZ_ASSERT(target);
    MOZ_ASSERT(target->isNative());

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

    // Push a Value containing the callee object: natives are allowed to access their callee before
    // setitng the return value. The StackPointer is moved to &vp[0].
    masm.Push(ObjectValue(*target));

    // Preload arguments into registers.
    masm.loadJSContext(argContextReg);
    masm.move32(Imm32(call->numStackArgs()), argUintNReg);
    masm.movePtr(StackPointer, argVpReg);

    masm.Push(argUintNReg);

    // Construct native exit frame.
    uint32_t safepointOffset;
    masm.buildFakeExitFrame(tempReg, &safepointOffset);
    masm.enterFakeExitFrame(NativeExitFrameLayout::Token());

    markSafepointAt(safepointOffset, call);

    // Construct and execute call.
    masm.setupUnalignedABICall(3, tempReg);
    masm.passABIArg(argContextReg);
    masm.passABIArg(argUintNReg);
    masm.passABIArg(argVpReg);
    masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, target->native()));

    // Test for failure.
    masm.branchIfFalseBool(ReturnReg, masm.failureLabel());

    // Load the outparam vp[0] into output register(s).
    masm.loadValue(Address(StackPointer, NativeExitFrameLayout::offsetOfResult()), JSReturnOperand);

    // The next instruction is removing the footer of the exit frame, so there
    // is no need for leaveFakeExitFrame.

    // Move the StackPointer back to its original location, unwinding the native exit frame.
    masm.adjustStack(NativeExitFrameLayout::Size() - unusedStack);
    MOZ_ASSERT(masm.framePushed() == initialStack);
}

static void
LoadDOMPrivate(MacroAssembler& masm, Register obj, Register priv)
{
    // Load the value in DOM_OBJECT_SLOT for a native or proxy DOM object. This
    // will be in the first slot but may be fixed or non-fixed.
    MOZ_ASSERT(obj != priv);

    // Check shape->numFixedSlots != 0.
    masm.loadPtr(Address(obj, JSObject::offsetOfShape()), priv);

    Label hasFixedSlots, done;
    masm.branchTest32(Assembler::NonZero,
                      Address(priv, Shape::offsetOfSlotInfo()),
                      Imm32(Shape::fixedSlotsMask()),
                      &hasFixedSlots);

    masm.loadPtr(Address(obj, NativeObject::offsetOfSlots()), priv);
    masm.loadPrivate(Address(priv, 0), priv);

    masm.jump(&done);
    masm.bind(&hasFixedSlots);

    masm.loadPrivate(Address(obj, NativeObject::getFixedSlotOffset(0)), priv);

    masm.bind(&done);
}

void
CodeGenerator::visitCallDOMNative(LCallDOMNative* call)
{
    JSFunction* target = call->getSingleTarget();
    MOZ_ASSERT(target);
    MOZ_ASSERT(target->isNative());
    MOZ_ASSERT(target->jitInfo());
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
    Register obj = masm.extractObject(Address(StackPointer, 0), argObj);
    MOZ_ASSERT(obj == argObj);

    // Push a Value containing the callee object: natives are allowed to access their callee before
    // setitng the return value. After this the StackPointer points to &vp[0].
    masm.Push(ObjectValue(*target));

    // Now compute the argv value.  Since StackPointer is pointing to &vp[0] and
    // argv is &vp[2] we just need to add 2*sizeof(Value) to the current
    // StackPointer.
    JS_STATIC_ASSERT(JSJitMethodCallArgsTraits::offsetOfArgv == 0);
    JS_STATIC_ASSERT(JSJitMethodCallArgsTraits::offsetOfArgc ==
                     IonDOMMethodExitFrameLayoutTraits::offsetOfArgcFromArgv);
    masm.computeEffectiveAddress(Address(StackPointer, 2 * sizeof(Value)), argArgs);

    LoadDOMPrivate(masm, obj, argPrivate);

    // Push argc from the call instruction into what will become the IonExitFrame
    masm.Push(Imm32(call->numStackArgs()));

    // Push our argv onto the stack
    masm.Push(argArgs);
    // And store our JSJitMethodCallArgs* in argArgs.
    masm.movePtr(StackPointer, argArgs);

    // Push |this| object for passing HandleObject. We push after argc to
    // maintain the same sp-relative location of the object pointer with other
    // DOMExitFrames.
    masm.Push(argObj);
    masm.movePtr(StackPointer, argObj);

    // Construct native exit frame.
    uint32_t safepointOffset;
    masm.buildFakeExitFrame(argJSContext, &safepointOffset);
    masm.enterFakeExitFrame(IonDOMMethodExitFrameLayout::Token());

    markSafepointAt(safepointOffset, call);

    // Construct and execute call.
    masm.setupUnalignedABICall(4, argJSContext);

    masm.loadJSContext(argJSContext);

    masm.passABIArg(argJSContext);
    masm.passABIArg(argObj);
    masm.passABIArg(argPrivate);
    masm.passABIArg(argArgs);
    masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, target->jitInfo()->method));

    if (target->jitInfo()->isInfallible) {
        masm.loadValue(Address(StackPointer, IonDOMMethodExitFrameLayout::offsetOfResult()),
                       JSReturnOperand);
    } else {
        // Test for failure.
        masm.branchIfFalseBool(ReturnReg, masm.exceptionLabel());

        // Load the outparam vp[0] into output register(s).
        masm.loadValue(Address(StackPointer, IonDOMMethodExitFrameLayout::offsetOfResult()),
                       JSReturnOperand);
    }

    // The next instruction is removing the footer of the exit frame, so there
    // is no need for leaveFakeExitFrame.

    // Move the StackPointer back to its original location, unwinding the native exit frame.
    masm.adjustStack(IonDOMMethodExitFrameLayout::Size() - unusedStack);
    MOZ_ASSERT(masm.framePushed() == initialStack);
}

typedef bool (*GetIntrinsicValueFn)(JSContext* cx, HandlePropertyName, MutableHandleValue);
static const VMFunction GetIntrinsicValueInfo =
    FunctionInfo<GetIntrinsicValueFn>(GetIntrinsicValue);

void
CodeGenerator::visitCallGetIntrinsicValue(LCallGetIntrinsicValue* lir)
{
    pushArg(ImmGCPtr(lir->mir()->name()));
    callVM(GetIntrinsicValueInfo, lir);
}

typedef bool (*InvokeFunctionFn)(JSContext*, HandleObject, uint32_t, Value*, Value*);
static const VMFunction InvokeFunctionInfo = FunctionInfo<InvokeFunctionFn>(InvokeFunction);

void
CodeGenerator::emitCallInvokeFunction(LInstruction* call, Register calleereg,
                                      uint32_t argc, uint32_t unusedStack)
{
    // Nestle %esp up to the argument vector.
    // Each path must account for framePushed_ separately, for callVM to be valid.
    masm.freeStack(unusedStack);

    pushArg(StackPointer); // argv.
    pushArg(Imm32(argc));  // argc.
    pushArg(calleereg);    // JSFunction*.

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

    // Generate an ArgumentsRectifier.
    JitCode* argumentsRectifier = gen->jitRuntime()->getArgumentsRectifier();

    masm.checkStackAlignment();

    // Guard that calleereg is actually a function object.
    masm.loadObjClass(calleereg, nargsreg);
    masm.branchPtr(Assembler::NotEqual, nargsreg, ImmPtr(&JSFunction::class_), &invoke);

    // Guard that calleereg is an interpreted function with a JSScript.
    // If we are constructing, also ensure the callee is a constructor.
    if (call->mir()->isConstructing())
        masm.branchIfNotInterpretedConstructor(calleereg, nargsreg, &invoke);
    else
        masm.branchIfFunctionHasNoScript(calleereg, &invoke);

    // Knowing that calleereg is a non-native function, load the JSScript.
    masm.loadPtr(Address(calleereg, JSFunction::offsetOfNativeOrScript()), objreg);

    // Load script jitcode.
    masm.loadBaselineOrIonRaw(objreg, objreg, &invoke);

    // Nestle the StackPointer up to the argument vector.
    masm.freeStack(unusedStack);

    // Construct the IonFramePrefix.
    uint32_t descriptor = MakeFrameDescriptor(masm.framePushed(), JitFrame_IonJS);
    masm.Push(Imm32(call->numActualArgs()));
    masm.PushCalleeToken(calleereg, call->mir()->isConstructing());
    masm.Push(Imm32(descriptor));

    // Check whether the provided arguments satisfy target argc.
    masm.load16ZeroExtend(Address(calleereg, JSFunction::offsetOfNargs()), nargsreg);
    masm.branch32(Assembler::Above, nargsreg, Imm32(call->numStackArgs()), &thunk);
    masm.jump(&makeCall);

    // Argument fixed needed. Load the ArgumentsRectifier.
    masm.bind(&thunk);
    {
        MOZ_ASSERT(ArgumentsRectifierReg != objreg);
        masm.movePtr(ImmGCPtr(argumentsRectifier), objreg); // Necessary for GC marking.
        masm.loadPtr(Address(objreg, JitCode::offsetOfCode()), objreg);
        masm.move32(Imm32(call->numStackArgs()), ArgumentsRectifierReg);
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
    emitCallInvokeFunction(call, calleereg, call->numActualArgs(), unusedStack);

    masm.bind(&end);

    // If the return value of the constructing function is Primitive,
    // replace the return value with the Object from CreateThis.
    if (call->mir()->isConstructing()) {
        Label notPrimitive;
        masm.branchTestPrimitive(Assembler::NotEqual, JSReturnOperand, &notPrimitive);
        masm.loadValue(Address(StackPointer, unusedStack), JSReturnOperand);
        masm.bind(&notPrimitive);
    }
}

void
CodeGenerator::visitCallKnown(LCallKnown* call)
{
    Register calleereg = ToRegister(call->getFunction());
    Register objreg    = ToRegister(call->getTempObject());
    uint32_t unusedStack = StackOffsetOfPassedArg(call->argslot());
    DebugOnly<JSFunction*> target = call->getSingleTarget();
    Label end, uncompiled;

    // Native single targets are handled by LCallNative.
    MOZ_ASSERT(!target->isNative());
    // Missing arguments must have been explicitly appended by the IonBuilder.
    MOZ_ASSERT(target->nargs() <= call->numStackArgs());

    MOZ_ASSERT_IF(call->mir()->isConstructing(), target->isInterpretedConstructor());

    masm.checkStackAlignment();

    // The calleereg is known to be a non-native function, but might point to
    // a LazyScript instead of a JSScript.
    masm.branchIfFunctionHasNoScript(calleereg, &uncompiled);

    // Knowing that calleereg is a non-native function, load the JSScript.
    masm.loadPtr(Address(calleereg, JSFunction::offsetOfNativeOrScript()), objreg);

    // Load script jitcode.
    if (call->mir()->needsArgCheck())
        masm.loadBaselineOrIonRaw(objreg, objreg, &uncompiled);
    else
        masm.loadBaselineOrIonNoArgCheck(objreg, objreg, &uncompiled);

    // Nestle the StackPointer up to the argument vector.
    masm.freeStack(unusedStack);

    // Construct the IonFramePrefix.
    uint32_t descriptor = MakeFrameDescriptor(masm.framePushed(), JitFrame_IonJS);
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
    masm.jump(&end);

    // Handle uncompiled functions.
    masm.bind(&uncompiled);
    emitCallInvokeFunction(call, calleereg, call->numActualArgs(), unusedStack);

    masm.bind(&end);

    // If the return value of the constructing function is Primitive,
    // replace the return value with the Object from CreateThis.
    if (call->mir()->isConstructing()) {
        Label notPrimitive;
        masm.branchTestPrimitive(Assembler::NotEqual, JSReturnOperand, &notPrimitive);
        masm.loadValue(Address(StackPointer, unusedStack), JSReturnOperand);
        masm.bind(&notPrimitive);
    }
}

void
CodeGenerator::emitCallInvokeFunction(LApplyArgsGeneric* apply, Register extraStackSize)
{
    Register objreg = ToRegister(apply->getTempObject());
    MOZ_ASSERT(objreg != extraStackSize);

    // Push the space used by the arguments.
    masm.movePtr(StackPointer, objreg);
    masm.Push(extraStackSize);

    pushArg(objreg);                           // argv.
    pushArg(ToRegister(apply->getArgc()));     // argc.
    pushArg(ToRegister(apply->getFunction())); // JSFunction*.

    // This specialization og callVM restore the extraStackSize after the call.
    callVM(InvokeFunctionInfo, apply, &extraStackSize);

    masm.Pop(extraStackSize);
}

// Do not bailout after the execution of this function since the stack no longer
// correspond to what is expected by the snapshots.
void
CodeGenerator::emitPushArguments(LApplyArgsGeneric* apply, Register extraStackSpace)
{
    // Holds the function nargs. Initially undefined.
    Register argcreg = ToRegister(apply->getArgc());
    Register copyreg = ToRegister(apply->getTempObject());

    // Initialize the loop counter AND Compute the stack usage (if == 0)
    masm.movePtr(argcreg, extraStackSpace);

    // Align the JitFrameLayout on the JitStackAlignment.
    const uint32_t alignment = JitStackAlignment / sizeof(Value);
    if (alignment > 1) {
        MOZ_ASSERT(frameSize() % JitStackAlignment == 0,
            "Stack padding assumes that the frameSize is correct");
        MOZ_ASSERT(alignment == 2);
        Label noPaddingNeeded;
        // if the number of arguments is odd, then we do not need any padding.
        masm.branchTestPtr(Assembler::NonZero, argcreg, Imm32(1), &noPaddingNeeded);
        masm.addPtr(Imm32(1), extraStackSpace);
        masm.bind(&noPaddingNeeded);
    }

    // Reserve space for copying the arguments.
    NativeObject::elementsSizeMustNotOverflow();
    masm.lshiftPtr(Imm32(ValueShift), extraStackSpace);
    masm.subPtr(extraStackSpace, StackPointer);

#ifdef DEBUG
    // Put a magic value in the space reserved for padding. Note, this code
    // cannot be merged with the previous test, as not all architectures can
    // write below their stack pointers.
    if (alignment > 1) {
        MOZ_ASSERT(alignment == 2);
        Label noPaddingNeeded;
        // if the number of arguments is odd, then we do not need any padding.
        masm.branchTestPtr(Assembler::NonZero, argcreg, Imm32(1), &noPaddingNeeded);
        BaseValueIndex dstPtr(StackPointer, argcreg);
        masm.storeValue(MagicValue(JS_ARG_POISON), dstPtr);
        masm.bind(&noPaddingNeeded);
    }
#endif

    // Skip the copy of arguments.
    Label end;
    masm.branchTestPtr(Assembler::Zero, argcreg, argcreg, &end);

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
    masm.addPtr(StackPointer, argvSrcBase);

    // Copy arguments.
    {
        Label loop;
        masm.bind(&loop);

        // As argvIndex is off by 1, and we use the decBranchPtr instruction
        // to loop back, we have to substract the size of the word which are
        // copied.
        BaseValueIndex srcPtr(argvSrcBase, argvIndex, argvSrcOffset - sizeof(void*));
        BaseValueIndex dstPtr(StackPointer, argvIndex, argvDstOffset - sizeof(void*));
        masm.loadPtr(srcPtr, copyreg);
        masm.storePtr(copyreg, dstPtr);

        // Handle 32 bits architectures.
        if (sizeof(Value) == 2 * sizeof(void*)) {
            BaseValueIndex srcPtrLow(argvSrcBase, argvIndex, argvSrcOffset - 2 * sizeof(void*));
            BaseValueIndex dstPtrLow(StackPointer, argvIndex, argvDstOffset - 2 * sizeof(void*));
            masm.loadPtr(srcPtrLow, copyreg);
            masm.storePtr(copyreg, dstPtrLow);
        }

        masm.decBranchPtr(Assembler::NonZero, argvIndex, Imm32(1), &loop);
    }

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
CodeGenerator::emitPopArguments(LApplyArgsGeneric* apply, Register extraStackSpace)
{
    // Pop |this| and Arguments.
    masm.freeStack(extraStackSpace);
}

void
CodeGenerator::visitApplyArgsGeneric(LApplyArgsGeneric* apply)
{
    // Holds the function object.
    Register calleereg = ToRegister(apply->getFunction());

    // Temporary register for modifying the function object.
    Register objreg = ToRegister(apply->getTempObject());
    Register extraStackSpace = ToRegister(apply->getTempStackCounter());

    // Holds the function nargs. Initially undefined.
    Register argcreg = ToRegister(apply->getArgc());

    // Unless already known, guard that calleereg is actually a function object.
    if (!apply->hasSingleTarget()) {
        masm.loadObjClass(calleereg, objreg);

        ImmPtr ptr = ImmPtr(&JSFunction::class_);
        bailoutCmpPtr(Assembler::NotEqual, objreg, ptr, apply->snapshot());
    }

    // Copy the arguments of the current function.
    emitPushArguments(apply, extraStackSpace);

    masm.checkStackAlignment();

    // If the function is native, only emit the call to InvokeFunction.
    if (apply->hasSingleTarget() && apply->getSingleTarget()->isNative()) {
        emitCallInvokeFunction(apply, extraStackSpace);
        emitPopArguments(apply, extraStackSpace);
        return;
    }

    Label end, invoke;

    // Guard that calleereg is an interpreted function with a JSScript.
    masm.branchIfFunctionHasNoScript(calleereg, &invoke);

    // Knowing that calleereg is a non-native function, load the JSScript.
    masm.loadPtr(Address(calleereg, JSFunction::offsetOfNativeOrScript()), objreg);

    // Load script jitcode.
    masm.loadBaselineOrIonRaw(objreg, objreg, &invoke);

    // Call with an Ion frame or a rectifier frame.
    {
        // Create the frame descriptor.
        unsigned pushed = masm.framePushed();
        Register stackSpace = extraStackSpace;
        masm.addPtr(Imm32(pushed), stackSpace);
        masm.makeFrameDescriptor(stackSpace, JitFrame_IonJS);

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
            JitCode* argumentsRectifier = gen->jitRuntime()->getArgumentsRectifier();

            MOZ_ASSERT(ArgumentsRectifierReg != objreg);
            masm.movePtr(ImmGCPtr(argumentsRectifier), objreg); // Necessary for GC marking.
            masm.loadPtr(Address(objreg, JitCode::offsetOfCode()), objreg);
            masm.movePtr(argcreg, ArgumentsRectifierReg);
        }

        masm.bind(&rejoin);

        // Finally call the function in objreg, as assigned by one of the paths above.
        uint32_t callOffset = masm.callJit(objreg);
        markSafepointAt(callOffset, apply);

        // Recover the number of arguments from the frame descriptor.
        masm.loadPtr(Address(StackPointer, 0), stackSpace);
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
    emitPopArguments(apply, extraStackSpace);
}

typedef bool (*ArraySpliceDenseFn)(JSContext*, HandleObject, uint32_t, uint32_t);
static const VMFunction ArraySpliceDenseInfo = FunctionInfo<ArraySpliceDenseFn>(ArraySpliceDense);

void
CodeGenerator::visitArraySplice(LArraySplice* lir)
{
    pushArg(ToRegister(lir->getDeleteCount()));
    pushArg(ToRegister(lir->getStart()));
    pushArg(ToRegister(lir->getObject()));
    callVM(ArraySpliceDenseInfo, lir);
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
CodeGenerator::visitGetDynamicName(LGetDynamicName* lir)
{
    Register scopeChain = ToRegister(lir->getScopeChain());
    Register name = ToRegister(lir->getName());
    Register temp1 = ToRegister(lir->temp1());
    Register temp2 = ToRegister(lir->temp2());
    Register temp3 = ToRegister(lir->temp3());

    masm.loadJSContext(temp3);

    /* Make space for the outparam. */
    masm.adjustStack(-int32_t(sizeof(Value)));
    masm.movePtr(StackPointer, temp2);

    masm.setupUnalignedABICall(4, temp1);
    masm.passABIArg(temp3);
    masm.passABIArg(scopeChain);
    masm.passABIArg(name);
    masm.passABIArg(temp2);
    masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, GetDynamicName));

    const ValueOperand out = ToOutValue(lir);

    masm.loadValue(Address(StackPointer, 0), out);
    masm.adjustStack(sizeof(Value));

    Label undefined;
    masm.branchTestUndefined(Assembler::Equal, out, &undefined);
    bailoutFrom(&undefined, lir->snapshot());
}

void
CodeGenerator::emitFilterArgumentsOrEval(LInstruction* lir, Register string,
                                         Register temp1, Register temp2)
{
    masm.loadJSContext(temp2);

    masm.setupUnalignedABICall(2, temp1);
    masm.passABIArg(temp2);
    masm.passABIArg(string);
    masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, FilterArgumentsOrEval));

    Label bail;
    masm.branchIfFalseBool(ReturnReg, &bail);
    bailoutFrom(&bail, lir->snapshot());
}

void
CodeGenerator::visitFilterArgumentsOrEvalS(LFilterArgumentsOrEvalS* lir)
{
    emitFilterArgumentsOrEval(lir, ToRegister(lir->getString()),
                              ToRegister(lir->temp1()),
                              ToRegister(lir->temp2()));
}

void
CodeGenerator::visitFilterArgumentsOrEvalV(LFilterArgumentsOrEvalV* lir)
{
    ValueOperand input = ToValue(lir, LFilterArgumentsOrEvalV::Input);

    // Act as nop on non-strings.
    Label done;
    masm.branchTestString(Assembler::NotEqual, input, &done);

    emitFilterArgumentsOrEval(lir, masm.extractString(input, ToRegister(lir->temp3())),
                              ToRegister(lir->temp1()), ToRegister(lir->temp2()));

    masm.bind(&done);
}

typedef bool (*DirectEvalSFn)(JSContext*, HandleObject, HandleScript, HandleValue, HandleString,
                              jsbytecode*, MutableHandleValue);
static const VMFunction DirectEvalStringInfo = FunctionInfo<DirectEvalSFn>(DirectEvalStringFromIon);

void
CodeGenerator::visitCallDirectEvalS(LCallDirectEvalS* lir)
{
    Register scopeChain = ToRegister(lir->getScopeChain());
    Register string = ToRegister(lir->getString());

    pushArg(ImmPtr(lir->mir()->pc()));
    pushArg(string);
    pushArg(ToValue(lir, LCallDirectEvalS::ThisValue));
    pushArg(ImmGCPtr(gen->info().script()));
    pushArg(scopeChain);

    callVM(DirectEvalStringInfo, lir);
}

typedef bool (*DirectEvalVFn)(JSContext*, HandleObject, HandleScript, HandleValue, HandleValue,
                              jsbytecode*, MutableHandleValue);
static const VMFunction DirectEvalValueInfo = FunctionInfo<DirectEvalVFn>(DirectEvalValueFromIon);

void
CodeGenerator::visitCallDirectEvalV(LCallDirectEvalV* lir)
{
    Register scopeChain = ToRegister(lir->getScopeChain());

    pushArg(ImmPtr(lir->mir()->pc()));
    pushArg(ToValue(lir, LCallDirectEvalV::Argument));
    pushArg(ToValue(lir, LCallDirectEvalV::ThisValue));
    pushArg(ImmGCPtr(gen->info().script()));
    pushArg(scopeChain);

    callVM(DirectEvalValueInfo, lir);
}

// Registers safe for use before generatePrologue().
static const uint32_t EntryTempMask = Registers::TempMask & ~(1 << OsrFrameReg.code());

void
CodeGenerator::generateArgumentsChecks(bool bailout)
{
    // This function can be used the normal way to check the argument types,
    // before entering the function and bailout when arguments don't match.
    // For debug purpose, this is can also be used to force/check that the
    // arguments are correct. Upon fail it will hit a breakpoint.

    MIRGraph& mir = gen->graph();
    MResumePoint* rp = mir.entryResumePoint();

    // No registers are allocated yet, so it's safe to grab anything.
    Register temp = GeneralRegisterSet(EntryTempMask).getAny();

    CompileInfo& info = gen->info();

    Label miss;
    for (uint32_t i = info.startArgSlot(); i < info.endArgSlot(); i++) {
        // All initial parameters are guaranteed to be MParameters.
        MParameter* param = rp->getOperand(i)->toParameter();
        const TypeSet* types = param->resultTypeSet();
        if (!types || types->unknown())
            continue;

        // Calculate the offset on the stack of the argument.
        // (i - info.startArgSlot())    - Compute index of arg within arg vector.
        // ... * sizeof(Value)          - Scale by value size.
        // ArgToStackOffset(...)        - Compute displacement within arg vector.
        int32_t offset = ArgToStackOffset((i - info.startArgSlot()) * sizeof(Value));
        masm.guardTypeSet(Address(StackPointer, offset), types, BarrierKind::TypeSet, temp, &miss);
    }

    if (miss.used()) {
        if (bailout) {
            bailoutFrom(&miss, graph.entrySnapshot());
        } else {
            Label success;
            masm.jump(&success);
            masm.bind(&miss);
            masm.assumeUnreachable("Argument check fail.");
            masm.bind(&success);
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

    void accept(CodeGenerator* codegen) {
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

    // Since Ion frames exist on the C stack, the stack limit may be
    // dynamically set by JS_SetThreadStackLimit() and JS_SetNativeStackQuota().
    const void* limitAddr = GetJitContext()->runtime->addressOfJitStackLimit();

    CheckOverRecursedFailure* ool = new(alloc()) CheckOverRecursedFailure(lir);
    addOutOfLineCode(ool, lir->mir());

    // Conditional forward (unlikely) branch to failure.
    masm.branchPtr(Assembler::AboveOrEqual, AbsoluteAddress(limitAddr), StackPointer, ool->entry());
    masm.bind(ool->rejoin());
}

typedef bool (*DefVarOrConstFn)(JSContext*, HandlePropertyName, unsigned, HandleObject);
static const VMFunction DefVarOrConstInfo =
    FunctionInfo<DefVarOrConstFn>(DefVarOrConst);

void
CodeGenerator::visitDefVar(LDefVar* lir)
{
    Register scopeChain = ToRegister(lir->scopeChain());

    pushArg(scopeChain); // JSObject*
    pushArg(Imm32(lir->mir()->attrs())); // unsigned
    pushArg(ImmGCPtr(lir->mir()->name())); // PropertyName*

    callVM(DefVarOrConstInfo, lir);
}

typedef bool (*DefFunOperationFn)(JSContext*, HandleScript, HandleObject, HandleFunction);
static const VMFunction DefFunOperationInfo = FunctionInfo<DefFunOperationFn>(DefFunOperation);

void
CodeGenerator::visitDefFun(LDefFun* lir)
{
    Register scopeChain = ToRegister(lir->scopeChain());

    pushArg(ImmGCPtr(lir->mir()->fun()));
    pushArg(scopeChain);
    pushArg(ImmGCPtr(current->mir()->info().script()));

    callVM(DefFunOperationInfo, lir);
}

typedef bool (*CheckOverRecursedFn)(JSContext*);
static const VMFunction CheckOverRecursedInfo =
    FunctionInfo<CheckOverRecursedFn>(CheckOverRecursed);

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
    // AsmJS module after code generation finishes.
    if (!GetJitContext()->runtime->profilingScripts())
        return nullptr;

    IonScriptCounts* counts = nullptr;

    CompileInfo* outerInfo = &gen->info();
    JSScript* script = outerInfo->script();

    counts = js_new<IonScriptCounts>();
    if (!counts || !counts->init(graph.numBlocks())) {
        js_delete(counts);
        return nullptr;
    }

    for (size_t i = 0; i < graph.numBlocks(); i++) {
        MBasicBlock* block = graph.getBlock(i)->mir();

        uint32_t offset = 0;
        char* description = nullptr;
        if (script) {
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
                        JS_snprintf(description, 200, "%s:%d",
                                    innerScript->filename(), innerScript->lineno());
                    }
                }
            }
        }

        if (!counts->block(i).init(block->id(), offset, description, block->numSuccessors())) {
            js_delete(counts);
            return nullptr;
        }
        for (size_t j = 0; j < block->numSuccessors(); j++)
            counts->block(i).setSuccessor(j, skipTrivialBlocks(block->getSuccessor(j))->id());
    }

    scriptCounts_ = counts;
    return counts;
}

// Structure for managing the state tracked for a block by script counters.
struct ScriptCountBlockState
{
    IonBlockCounts& block;
    MacroAssembler& masm;

    Sprinter printer;

  public:
    ScriptCountBlockState(IonBlockCounts* block, MacroAssembler* masm)
      : block(*block), masm(*masm), printer(GetJitContext()->cx)
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
        if (const char* extra = ins->extraName())
            printer.printf("[%s:%s]\n", ins->opName(), extra);
        else
            printer.printf("[%s]\n", ins->opName());
    }

    ~ScriptCountBlockState()
    {
        masm.setPrinter(nullptr);

        block.setCode(printer.string());
    }
};

#ifdef DEBUG
void
CodeGenerator::branchIfInvalidated(Register temp, Label* invalidated)
{
    CodeOffsetLabel label = masm.movWithPatch(ImmWord(uintptr_t(-1)), temp);
    masm.propagateOOM(ionScriptLabels_.append(label));

    // If IonScript::invalidationCount_ != 0, the script has been invalidated.
    masm.branch32(Assembler::NotEqual,
                  Address(temp, IonScript::offsetOfInvalidationCount()),
                  Imm32(0),
                  invalidated);
}

void
CodeGenerator::emitObjectOrStringResultChecks(LInstruction* lir, MDefinition* mir)
{
    if (lir->numDefs() == 0)
        return;

    MOZ_ASSERT(lir->numDefs() == 1);
    Register output = ToRegister(lir->getDef(0));

    GeneralRegisterSet regs(GeneralRegisterSet::All());
    regs.take(output);

    Register temp = regs.takeAny();
    masm.push(temp);

    // Don't check if the script has been invalidated. In that case invalid
    // types are expected (until we reach the OsiPoint and bailout).
    Label done;
    branchIfInvalidated(temp, &done);

    if ((mir->type() == MIRType_Object || mir->type() == MIRType_ObjectOrNull) &&
        mir->resultTypeSet() &&
        !mir->resultTypeSet()->unknownObject())
    {
        // We have a result TypeSet, assert this object is in it.
        Label miss, ok;
        if (mir->type() == MIRType_ObjectOrNull)
            masm.branchPtr(Assembler::Equal, output, ImmWord(0), &ok);
        if (mir->resultTypeSet()->getObjectCount() > 0)
            masm.guardObjectType(output, mir->resultTypeSet(), temp, &miss);
        else
            masm.jump(&miss);
        masm.jump(&ok);

        masm.bind(&miss);

        // Type set guards might miss when an object's group changes and its
        // properties become unknown, so check for this case.
        masm.loadPtr(Address(output, JSObject::offsetOfGroup()), temp);
        masm.branchTestPtr(Assembler::NonZero,
                           Address(temp, ObjectGroup::offsetOfFlags()),
                           Imm32(OBJECT_FLAG_UNKNOWN_PROPERTIES), &ok);

        masm.assumeUnreachable("MIR instruction returned object with unexpected type");

        masm.bind(&ok);
    }

    // Check that we have a valid GC pointer.
    saveVolatile();
    masm.setupUnalignedABICall(2, temp);
    masm.loadJSContext(temp);
    masm.passABIArg(temp);
    masm.passABIArg(output);

    void* callee;
    switch (mir->type()) {
      case MIRType_Object:
        callee = JS_FUNC_TO_DATA_PTR(void*, AssertValidObjectPtr);
        break;
      case MIRType_ObjectOrNull:
        callee = JS_FUNC_TO_DATA_PTR(void*, AssertValidObjectOrNullPtr);
        break;
      case MIRType_String:
        callee = JS_FUNC_TO_DATA_PTR(void*, AssertValidStringPtr);
        break;
      case MIRType_Symbol:
        callee = JS_FUNC_TO_DATA_PTR(void*, AssertValidSymbolPtr);
        break;
      default:
        MOZ_CRASH();
    }

    masm.callWithABI(callee);
    restoreVolatile();

    masm.bind(&done);
    masm.pop(temp);
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

    GeneralRegisterSet regs(GeneralRegisterSet::All());
    regs.take(output);

    Register temp1 = regs.takeAny();
    Register temp2 = regs.takeAny();
    masm.push(temp1);
    masm.push(temp2);

    // Don't check if the script has been invalidated. In that case invalid
    // types are expected (until we reach the OsiPoint and bailout).
    Label done;
    branchIfInvalidated(temp1, &done);

    if (mir->resultTypeSet() && !mir->resultTypeSet()->unknown()) {
        // We have a result TypeSet, assert this value is in it.
        Label miss, ok;
        masm.guardTypeSet(output, mir->resultTypeSet(), BarrierKind::TypeSet, temp1, &miss);
        masm.jump(&ok);

        masm.bind(&miss);

        // Type set guards might miss when an object's group changes and its
        // properties become unknown, so check for this case.
        Label realMiss;
        masm.branchTestObject(Assembler::NotEqual, output, &realMiss);
        Register payload = masm.extractObject(output, temp1);
        masm.loadPtr(Address(payload, JSObject::offsetOfGroup()), temp1);
        masm.branchTestPtr(Assembler::NonZero,
                           Address(temp1, ObjectGroup::offsetOfFlags()),
                           Imm32(OBJECT_FLAG_UNKNOWN_PROPERTIES), &ok);
        masm.bind(&realMiss);

        masm.assumeUnreachable("MIR instruction returned value with unexpected type");

        masm.bind(&ok);
    }

    // Check that we have a valid GC pointer.
    saveVolatile();

    masm.pushValue(output);
    masm.movePtr(StackPointer, temp1);

    masm.setupUnalignedABICall(2, temp2);
    masm.loadJSContext(temp2);
    masm.passABIArg(temp2);
    masm.passABIArg(temp1);
    masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, AssertValidValue));
    masm.popValue(output);
    restoreVolatile();

    masm.bind(&done);
    masm.pop(temp2);
    masm.pop(temp1);
}

void
CodeGenerator::emitDebugResultChecks(LInstruction* ins)
{
    // In debug builds, check that LIR instructions return valid values.

    MDefinition* mir = ins->mirRaw();
    if (!mir)
        return;

    switch (mir->type()) {
      case MIRType_Object:
      case MIRType_ObjectOrNull:
      case MIRType_String:
      case MIRType_Symbol:
        emitObjectOrStringResultChecks(ins, mir);
        break;
      case MIRType_Value:
        emitValueResultChecks(ins, mir);
        break;
      default:
        break;
    }
}
#endif

bool
CodeGenerator::generateBody()
{
    IonScriptCounts* counts = maybeCreateScriptCounts();

#if defined(JS_ION_PERF)
    PerfSpewer* perfSpewer = &perfSpewer_;
    if (gen->compilingAsmJS())
        perfSpewer = &gen->perfSpewer();
#endif

    for (size_t i = 0; i < graph.numBlocks(); i++) {
        current = graph.getBlock(i);

        // Don't emit any code for trivial blocks, containing just a goto. Such
        // blocks are created to split critical edges, and if we didn't end up
        // putting any instructions in them, we can skip them.
        if (current->isTrivial())
            continue;

#ifdef DEBUG
        const char* filename = nullptr;
        unsigned lineNumber = 0, columnNumber = 0;
        if (current->mir()->info().script()) {
            filename = current->mir()->info().script()->filename();
            if (current->mir()->pc())
                lineNumber = PCToLineNumber(current->mir()->info().script(), current->mir()->pc(),
                                            &columnNumber);
        } else {
            lineNumber = current->mir()->lineno();
            columnNumber = current->mir()->columnIndex();
        }
        JitSpew(JitSpew_Codegen, "# block%lu %s:%u:%u%s:", i,
                filename ? filename : "?", lineNumber, columnNumber,
                current->mir()->isLoopHeader() ? " (loop header)" : "");
#endif

        masm.bind(current->label());

        mozilla::Maybe<ScriptCountBlockState> blockCounts;
        if (counts) {
            blockCounts.emplace(&counts->block(i), &masm);
            if (!blockCounts->init())
                return false;
        }

#if defined(JS_ION_PERF)
        perfSpewer->startBasicBlock(current->mir(), masm);
#endif

        for (LInstructionIterator iter = current->begin(); iter != current->end(); iter++) {
#ifdef DEBUG
            JitSpewStart(JitSpew_Codegen, "instruction %s", iter->opName());
            if (const char* extra = iter->extraName())
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
                    if (!addTrackedOptimizationsEntry(iter->mirRaw()->trackedOptimizations()))
                        return false;
                }
            }

            iter->accept(this);

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

    void accept(CodeGenerator* codegen) {
        codegen->visitOutOfLineNewArray(this);
    }

    LNewArray* lir() const {
        return lir_;
    }
};

typedef ArrayObject* (*NewDenseArrayFn)(ExclusiveContext*, uint32_t, HandleObjectGroup,
                                        AllocatingBehaviour);
static const VMFunction NewDenseArrayInfo = FunctionInfo<NewDenseArrayFn>(NewDenseArray);

void
CodeGenerator::visitNewArrayCallVM(LNewArray* lir)
{
    Register objReg = ToRegister(lir->output());

    MOZ_ASSERT(!lir->isCall());
    saveLive(lir);

    JSObject* templateObject = lir->mir()->templateObject();
    ObjectGroup* group =
        templateObject->isSingleton() ? nullptr : templateObject->group();

    pushArg(Imm32(lir->mir()->allocatingBehaviour()));
    pushArg(ImmGCPtr(group));
    pushArg(Imm32(lir->mir()->count()));

    callVM(NewDenseArrayInfo, lir);

    if (ReturnReg != objReg)
        masm.movePtr(ReturnReg, objReg);

    restoreLive(lir);
}

typedef JSObject* (*NewDerivedTypedObjectFn)(JSContext*,
                                             HandleObject type,
                                             HandleObject owner,
                                             int32_t offset);
static const VMFunction CreateDerivedTypedObjInfo =
    FunctionInfo<NewDerivedTypedObjectFn>(CreateDerivedTypedObj);

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

    masm.setupUnalignedABICall(2, temp);
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
    masm.setupUnalignedABICall(numArgs, temp);

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
    ArrayObject* templateObject = lir->mir()->templateObject();
    DebugOnly<uint32_t> count = lir->mir()->count();

    MOZ_ASSERT(count < NativeObject::NELEMENTS_LIMIT);

    if (lir->mir()->shouldUseVM()) {
        visitNewArrayCallVM(lir);
        return;
    }

    OutOfLineNewArray* ool = new(alloc()) OutOfLineNewArray(lir);
    addOutOfLineCode(ool, lir->mir());

    masm.createGCObject(objReg, tempReg, templateObject, lir->mir()->initialHeap(), ool->entry());

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
                                   (ArgList(), ImmGCPtr(templateObject), Imm32(initialHeap)),
                                   StoreRegisterTo(objReg));

    masm.createGCObject(objReg, tempReg, templateObject, initialHeap, ool->entry());

    masm.bind(ool->rejoin());
}

typedef ArrayObject* (*ArrayConstructorOneArgFn)(JSContext*, HandleObjectGroup, int32_t length);
static const VMFunction ArrayConstructorOneArgInfo =
    FunctionInfo<ArrayConstructorOneArgFn>(ArrayConstructorOneArg);

void
CodeGenerator::visitNewArrayDynamicLength(LNewArrayDynamicLength* lir)
{
    Register lengthReg = ToRegister(lir->length());
    Register objReg = ToRegister(lir->output());
    Register tempReg = ToRegister(lir->temp());

    ArrayObject* templateObject = lir->mir()->templateObject();
    gc::InitialHeap initialHeap = lir->mir()->initialHeap();

    OutOfLineCode* ool = oolCallVM(ArrayConstructorOneArgInfo, lir,
                                   (ArgList(), ImmGCPtr(templateObject->group()), lengthReg),
                                   StoreRegisterTo(objReg));

    size_t numSlots = gc::GetGCKindSlots(templateObject->asTenured().getAllocKind());
    size_t inlineLength = numSlots >= ObjectElements::VALUES_PER_HEADER
                        ? numSlots - ObjectElements::VALUES_PER_HEADER
                        : 0;

    // Try to do the allocation inline if the template object is big enough
    // for the length in lengthReg. If the length is bigger we could still
    // use the template object and not allocate the elements, but it's more
    // efficient to do a single big allocation than (repeatedly) reallocating
    // the array later on when filling it.
    if (!templateObject->isSingleton() && templateObject->length() <= inlineLength)
        masm.branch32(Assembler::Above, lengthReg, Imm32(templateObject->length()), ool->entry());
    else
        masm.jump(ool->entry());

    masm.createGCObject(objReg, tempReg, templateObject, initialHeap, ool->entry());

    size_t lengthOffset = NativeObject::offsetOfFixedElements() + ObjectElements::offsetOfLength();
    masm.store32(lengthReg, Address(objReg, lengthOffset));

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

    void accept(CodeGenerator* codegen) {
        codegen->visitOutOfLineNewObject(this);
    }

    LNewObject* lir() const {
        return lir_;
    }
};

typedef JSObject* (*NewInitObjectFn)(JSContext*, HandlePlainObject);
static const VMFunction NewInitObjectInfo = FunctionInfo<NewInitObjectFn>(NewInitObject);

typedef PlainObject* (*ObjectCreateWithTemplateFn)(JSContext*, HandlePlainObject);
static const VMFunction ObjectCreateWithTemplateInfo =
    FunctionInfo<ObjectCreateWithTemplateFn>(ObjectCreateWithTemplate);

void
CodeGenerator::visitNewObjectVMCall(LNewObject* lir)
{
    Register objReg = ToRegister(lir->output());

    MOZ_ASSERT(!lir->isCall());
    saveLive(lir);

    pushArg(ImmGCPtr(lir->mir()->templateObject()));

    // If we're making a new object with a class prototype (that is, an object
    // that derives its class from its prototype instead of being
    // JSObject::class_'d) from self-hosted code, we need a different init
    // function.
    if (lir->mir()->mode() == MNewObject::ObjectLiteral) {
        callVM(NewInitObjectInfo, lir);
    } else {
        MOZ_ASSERT(lir->mir()->mode() == MNewObject::ObjectCreate);
        callVM(ObjectCreateWithTemplateInfo, lir);
    }

    if (ReturnReg != objReg)
        masm.movePtr(ReturnReg, objReg);

    restoreLive(lir);
}

static bool
ShouldInitFixedSlots(LInstruction* lir, NativeObject* templateObj)
{
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
    PlainObject* templateObject = lir->mir()->templateObject();

    if (lir->mir()->shouldUseVM()) {
        visitNewObjectVMCall(lir);
        return;
    }

    OutOfLineNewObject* ool = new(alloc()) OutOfLineNewObject(lir);
    addOutOfLineCode(ool, lir->mir());

    bool initFixedSlots = ShouldInitFixedSlots(lir, templateObject);
    masm.createGCObject(objReg, tempReg, templateObject, lir->mir()->initialHeap(), ool->entry(),
                        initFixedSlots);

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
    FunctionInfo<NewTypedObjectFn>(InlineTypedObject::createCopy);

void
CodeGenerator::visitNewTypedObject(LNewTypedObject* lir)
{
    Register object = ToRegister(lir->output());
    Register temp = ToRegister(lir->temp());
    InlineTypedObject* templateObject = lir->mir()->templateObject();
    gc::InitialHeap initialHeap = lir->mir()->initialHeap();

    OutOfLineCode* ool = oolCallVM(NewTypedObjectInfo, lir,
                                   (ArgList(), ImmGCPtr(templateObject), Imm32(initialHeap)),
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

    // :TODO: We cannot spill SIMD registers (Bug 1112164) from safepoints, thus
    // we cannot use the same oolCallVM as visitNewTypedObject for allocating
    // SIMD Typed Objects if we are at the end of the nursery. (Bug 1119303)
    Label bail;
    masm.createGCObject(object, temp, templateObject, initialHeap, &bail);
    bailoutFrom(&bail, lir->snapshot());

    Address objectData(object, InlineTypedObject::offsetOfDataStart());
    switch (type) {
      case MIRType_Int32x4:
        masm.storeUnalignedInt32x4(in, objectData);
        break;
      case MIRType_Float32x4:
        masm.storeUnalignedFloat32x4(in, objectData);
        break;
      default:
        MOZ_CRASH("Unknown SIMD kind when generating code for SimdBox.");
    }
}

void
CodeGenerator::visitSimdUnbox(LSimdUnbox* lir)
{
    Register object = ToRegister(lir->input());
    FloatRegister simd = ToFloatRegister(lir->output());
    Register temp = ToRegister(lir->temp());
    Label bail;

    // obj->group()
    masm.loadPtr(Address(object, JSObject::offsetOfGroup()), temp);

    // Guard that the object has the same representation as the one produced for
    // SIMD value-type.
    Address clasp(temp, ObjectGroup::offsetOfClasp());
    static_assert(!SimdTypeDescr::Opaque, "SIMD objects are transparent");
    masm.branchPtr(Assembler::NotEqual, clasp, ImmPtr(&InlineTransparentTypedObject::class_),
                   &bail);

    // obj->type()->typeDescr()
    // The previous class pointer comparison implies that the addendumKind is
    // Addendum_TypeDescr.
    masm.loadPtr(Address(temp, ObjectGroup::offsetOfAddendum()), temp);

    // Check for the /Kind/ reserved slot of the TypeDescr.  This is an Int32
    // Value which is equivalent to the object class check.
    static_assert(JS_DESCR_SLOT_KIND < NativeObject::MAX_FIXED_SLOTS, "Load from fixed slots");
    Address typeDescrKind(temp, NativeObject::getFixedSlotOffset(JS_DESCR_SLOT_KIND));
    masm.assertTestInt32(Assembler::Equal, typeDescrKind,
      "MOZ_ASSERT(obj->type()->typeDescr()->getReservedSlot(JS_DESCR_SLOT_KIND).isInt32())");
    masm.branch32(Assembler::NotEqual, masm.ToPayload(typeDescrKind), Imm32(js::type::Simd), &bail);

    // Convert the SIMD MIRType to a SimdTypeDescr::Type.
    js::SimdTypeDescr::Type type;
    switch (lir->mir()->type()) {
      case MIRType_Int32x4:
        type = js::SimdTypeDescr::TYPE_INT32;
        break;
      case MIRType_Float32x4:
        type = js::SimdTypeDescr::TYPE_FLOAT32;
        break;
      default:
        MOZ_CRASH("Unexpected SIMD Type.");
    }

    // Check if the SimdTypeDescr /Type/ match the specialization of this
    // MSimdUnbox instruction.
    static_assert(JS_DESCR_SLOT_TYPE < NativeObject::MAX_FIXED_SLOTS, "Load from fixed slots");
    Address typeDescrType(temp, NativeObject::getFixedSlotOffset(JS_DESCR_SLOT_TYPE));
    masm.assertTestInt32(Assembler::Equal, typeDescrType,
      "MOZ_ASSERT(obj->type()->typeDescr()->getReservedSlot(JS_DESCR_SLOT_TYPE).isInt32())");
    masm.branch32(Assembler::NotEqual, masm.ToPayload(typeDescrType), Imm32(type), &bail);

    // Load the value from the data of the InlineTypedObject.
    Address objectData(object, InlineTypedObject::offsetOfDataStart());
    switch (lir->mir()->type()) {
      case MIRType_Int32x4:
        masm.loadUnalignedInt32x4(objectData, simd);
        break;
      case MIRType_Float32x4:
        masm.loadUnalignedFloat32x4(objectData, simd);
        break;
      default:
        MOZ_CRASH("The impossible happened!");
    }

    bailoutFrom(&bail, lir->snapshot());
}

typedef js::DeclEnvObject* (*NewDeclEnvObjectFn)(JSContext*, HandleFunction, gc::InitialHeap);
static const VMFunction NewDeclEnvObjectInfo =
    FunctionInfo<NewDeclEnvObjectFn>(DeclEnvObject::createTemplateObject);

void
CodeGenerator::visitNewDeclEnvObject(LNewDeclEnvObject* lir)
{
    Register objReg = ToRegister(lir->output());
    Register tempReg = ToRegister(lir->temp());
    DeclEnvObject* templateObj = lir->mir()->templateObj();
    CompileInfo& info = lir->mir()->block()->info();

    // If we have a template object, we can inline call object creation.
    OutOfLineCode* ool = oolCallVM(NewDeclEnvObjectInfo, lir,
                                   (ArgList(), ImmGCPtr(info.funMaybeLazy()),
                                    Imm32(gc::DefaultHeap)),
                                   StoreRegisterTo(objReg));

    bool initFixedSlots = ShouldInitFixedSlots(lir, templateObj);
    masm.createGCObject(objReg, tempReg, templateObj, gc::DefaultHeap, ool->entry(),
                        initFixedSlots);

    masm.bind(ool->rejoin());
}

typedef JSObject* (*NewCallObjectFn)(JSContext*, HandleShape, HandleObjectGroup, uint32_t);
static const VMFunction NewCallObjectInfo =
    FunctionInfo<NewCallObjectFn>(NewCallObject);

void
CodeGenerator::visitNewCallObject(LNewCallObject* lir)
{
    Register objReg = ToRegister(lir->output());
    Register tempReg = ToRegister(lir->temp());

    CallObject* templateObj = lir->mir()->templateObject();

    JSScript* script = lir->mir()->block()->info().script();
    uint32_t lexicalBegin = script->bindings.aliasedBodyLevelLexicalBegin();
    OutOfLineCode* ool = oolCallVM(NewCallObjectInfo, lir,
                                   (ArgList(), ImmGCPtr(templateObj->lastProperty()),
                                               ImmGCPtr(templateObj->group()),
                                               Imm32(lexicalBegin)),
                                   StoreRegisterTo(objReg));

    // Inline call object creation, using the OOL path only for tricky cases.
    bool initFixedSlots = ShouldInitFixedSlots(lir, templateObj);
    masm.createGCObject(objReg, tempReg, templateObj, gc::DefaultHeap, ool->entry(),
                        initFixedSlots);

    masm.bind(ool->rejoin());
}

typedef JSObject* (*NewSingletonCallObjectFn)(JSContext*, HandleShape, uint32_t);
static const VMFunction NewSingletonCallObjectInfo =
    FunctionInfo<NewSingletonCallObjectFn>(NewSingletonCallObject);

void
CodeGenerator::visitNewSingletonCallObject(LNewSingletonCallObject* lir)
{
    Register objReg = ToRegister(lir->output());

    JSObject* templateObj = lir->mir()->templateObject();

    JSScript* script = lir->mir()->block()->info().script();
    uint32_t lexicalBegin = script->bindings.aliasedBodyLevelLexicalBegin();
    OutOfLineCode* ool;
    ool = oolCallVM(NewSingletonCallObjectInfo, lir,
                    (ArgList(), ImmGCPtr(templateObj->lastProperty()),
                                Imm32(lexicalBegin)),
                    StoreRegisterTo(objReg));

    // Objects can only be given singleton types in VM calls.  We make the call
    // out of line to not bloat inline code, even if (naively) this seems like
    // extra work.
    masm.jump(ool->entry());
    masm.bind(ool->rejoin());
}

typedef JSObject* (*NewStringObjectFn)(JSContext*, HandleString);
static const VMFunction NewStringObjectInfo = FunctionInfo<NewStringObjectFn>(NewStringObject);

void
CodeGenerator::visitNewStringObject(LNewStringObject* lir)
{
    Register input = ToRegister(lir->input());
    Register output = ToRegister(lir->output());
    Register temp = ToRegister(lir->temp());

    StringObject* templateObj = lir->mir()->templateObj();

    OutOfLineCode* ool = oolCallVM(NewStringObjectInfo, lir, (ArgList(), input),
                                   StoreRegisterTo(output));

    masm.createGCObject(output, temp, templateObj, gc::DefaultHeap, ool->entry());

    masm.loadStringLength(input, temp);

    masm.storeValue(JSVAL_TYPE_STRING, input, Address(output, StringObject::offsetOfPrimitiveValue()));
    masm.storeValue(JSVAL_TYPE_INT32, temp, Address(output, StringObject::offsetOfLength()));

    masm.bind(ool->rejoin());
}

typedef bool(*InitElemFn)(JSContext* cx, HandleObject obj,
                          HandleValue id, HandleValue value);
static const VMFunction InitElemInfo =
    FunctionInfo<InitElemFn>(InitElemOperation);

void
CodeGenerator::visitInitElem(LInitElem* lir)
{
    Register objReg = ToRegister(lir->getObject());

    pushArg(ToValue(lir, LInitElem::ValueIndex));
    pushArg(ToValue(lir, LInitElem::IdIndex));
    pushArg(objReg);

    callVM(InitElemInfo, lir);
}

typedef bool (*InitElemGetterSetterFn)(JSContext*, jsbytecode*, HandleObject, HandleValue,
                                       HandleObject);
static const VMFunction InitElemGetterSetterInfo =
    FunctionInfo<InitElemGetterSetterFn>(InitGetterSetterOperation);

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
    FunctionInfo<MutatePrototypeFn>(MutatePrototype);

void
CodeGenerator::visitMutateProto(LMutateProto* lir)
{
    Register objReg = ToRegister(lir->getObject());

    pushArg(ToValue(lir, LMutateProto::ValueIndex));
    pushArg(objReg);

    callVM(MutatePrototypeInfo, lir);
}

typedef bool(*InitPropFn)(JSContext* cx, HandleNativeObject obj,
                          HandlePropertyName name, HandleValue value);
static const VMFunction InitPropInfo =
    FunctionInfo<InitPropFn>(InitProp);

void
CodeGenerator::visitInitProp(LInitProp* lir)
{
    Register objReg = ToRegister(lir->getObject());

    pushArg(ToValue(lir, LInitProp::ValueIndex));
    pushArg(ImmGCPtr(lir->mir()->propertyName()));
    pushArg(objReg);

    callVM(InitPropInfo, lir);
}

typedef bool(*InitPropGetterSetterFn)(JSContext*, jsbytecode*, HandleObject, HandlePropertyName,
                                      HandleObject);
static const VMFunction InitPropGetterSetterInfo =
    FunctionInfo<InitPropGetterSetterFn>(InitGetterSetterOperation);

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

typedef bool (*CreateThisFn)(JSContext* cx, HandleObject callee, MutableHandleValue rval);
static const VMFunction CreateThisInfoCodeGen = FunctionInfo<CreateThisFn>(CreateThis);

void
CodeGenerator::visitCreateThis(LCreateThis* lir)
{
    const LAllocation* callee = lir->getCallee();

    if (callee->isConstant())
        pushArg(ImmGCPtr(&callee->toConstant()->toObject()));
    else
        pushArg(ToRegister(callee));

    callVM(CreateThisInfoCodeGen, lir);
}

static JSObject*
CreateThisForFunctionWithProtoWrapper(JSContext* cx, js::HandleObject callee, HandleObject proto)
{
    return CreateThisForFunctionWithProto(cx, callee, proto);
}

typedef JSObject* (*CreateThisWithProtoFn)(JSContext* cx, HandleObject callee, HandleObject proto);
static const VMFunction CreateThisWithProtoInfo =
FunctionInfo<CreateThisWithProtoFn>(CreateThisForFunctionWithProtoWrapper);

void
CodeGenerator::visitCreateThisWithProto(LCreateThisWithProto* lir)
{
    const LAllocation* callee = lir->getCallee();
    const LAllocation* proto = lir->getPrototype();

    if (proto->isConstant())
        pushArg(ImmGCPtr(&proto->toConstant()->toObject()));
    else
        pushArg(ToRegister(proto));

    if (callee->isConstant())
        pushArg(ImmGCPtr(&callee->toConstant()->toObject()));
    else
        pushArg(ToRegister(callee));

    callVM(CreateThisWithProtoInfo, lir);
}

typedef JSObject* (*NewGCObjectFn)(JSContext* cx, gc::AllocKind allocKind,
                                   gc::InitialHeap initialHeap, const js::Class* clasp);
static const VMFunction NewGCObjectInfo =
    FunctionInfo<NewGCObjectFn>(js::jit::NewGCObject);

void
CodeGenerator::visitCreateThisWithTemplate(LCreateThisWithTemplate* lir)
{
    JSObject* templateObject = lir->mir()->templateObject();
    gc::AllocKind allocKind = templateObject->asTenured().getAllocKind();
    gc::InitialHeap initialHeap = lir->mir()->initialHeap();
    const js::Class* clasp = templateObject->getClass();
    Register objReg = ToRegister(lir->output());
    Register tempReg = ToRegister(lir->temp());

    OutOfLineCode* ool = oolCallVM(NewGCObjectInfo, lir,
                                   (ArgList(), Imm32(allocKind), Imm32(initialHeap),
                                    ImmPtr(clasp)),
                                   StoreRegisterTo(objReg));

    // Allocate. If the FreeList is empty, call to VM, which may GC.
    masm.newGCThing(objReg, tempReg, templateObject, lir->mir()->initialHeap(), ool->entry());

    // Initialize based on the templateObject.
    masm.bind(ool->rejoin());

    bool initFixedSlots = !templateObject->is<PlainObject>() ||
                          ShouldInitFixedSlots(lir, &templateObject->as<PlainObject>());
    masm.initGCThing(objReg, tempReg, templateObject, initFixedSlots);
}

typedef JSObject* (*NewIonArgumentsObjectFn)(JSContext* cx, JitFrameLayout* frame, HandleObject);
static const VMFunction NewIonArgumentsObjectInfo =
    FunctionInfo<NewIonArgumentsObjectFn>((NewIonArgumentsObjectFn) ArgumentsObject::createForIon);

void
CodeGenerator::visitCreateArgumentsObject(LCreateArgumentsObject* lir)
{
    // This should be getting constructed in the first block only, and not any OSR entry blocks.
    MOZ_ASSERT(lir->mir()->block()->id() == 0);

    const LAllocation* callObj = lir->getCallObject();
    Register temp = ToRegister(lir->getTemp(0));

    masm.movePtr(StackPointer, temp);
    masm.addPtr(Imm32(frameSize()), temp);

    pushArg(ToRegister(callObj));
    pushArg(temp);
    callVM(NewIonArgumentsObjectInfo, lir);
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

typedef JSObject* (*BoxNonStrictThisFn)(JSContext*, HandleValue);
static const VMFunction BoxNonStrictThisInfo = FunctionInfo<BoxNonStrictThisFn>(BoxNonStrictThis);

void
CodeGenerator::visitComputeThis(LComputeThis* lir)
{
    ValueOperand value = ToValue(lir, LComputeThis::ValueIndex);
    Register output = ToRegister(lir->output());

    OutOfLineCode* ool = oolCallVM(BoxNonStrictThisInfo, lir, (ArgList(), value),
                                   StoreRegisterTo(output));

    masm.branchTestObject(Assembler::NotEqual, value, ool->entry());
    masm.unboxObject(value, output);

    masm.bind(ool->rejoin());
}

void
CodeGenerator::visitLoadArrowThis(LLoadArrowThis* lir)
{
    Register callee = ToRegister(lir->callee());
    ValueOperand output = ToOutValue(lir);
    masm.loadValue(Address(callee, FunctionExtended::offsetOfArrowThisSlot()), output);
}

void
CodeGenerator::visitArrayLength(LArrayLength* lir)
{
    Address length(ToRegister(lir->elements()), ObjectElements::offsetOfLength());
    masm.load32(length, ToRegister(lir->output()));
}

void
CodeGenerator::visitSetArrayLength(LSetArrayLength* lir)
{
    Address length(ToRegister(lir->elements()), ObjectElements::offsetOfLength());
    Int32Key newLength = ToInt32Key(lir->index());

    masm.bumpKey(&newLength, 1);
    masm.storeKey(newLength, length);
    // Restore register value if it is used/captured after.
    masm.bumpKey(&newLength, -1);
}

void
CodeGenerator::visitTypedArrayLength(LTypedArrayLength* lir)
{
    Register obj = ToRegister(lir->object());
    Register out = ToRegister(lir->output());
    masm.unboxInt32(Address(obj, TypedArrayLayout::lengthOffset()), out);
}

void
CodeGenerator::visitTypedArrayElements(LTypedArrayElements* lir)
{
    Register obj = ToRegister(lir->object());
    Register out = ToRegister(lir->output());
    masm.loadPtr(Address(obj, TypedArrayLayout::dataOffset()), out);
}

void
CodeGenerator::visitTypedObjectDescr(LTypedObjectDescr* lir)
{
    Register obj = ToRegister(lir->object());
    Register out = ToRegister(lir->output());

    masm.loadPtr(Address(obj, JSObject::offsetOfGroup()), out);
    masm.loadPtr(Address(out, ObjectGroup::offsetOfAddendum()), out);
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
        masm.loadObjClass(obj, out);
        masm.branchPtr(Assembler::Equal, out, ImmPtr(&InlineOpaqueTypedObject::class_), &inlineObject);
        masm.branchPtr(Assembler::Equal, out, ImmPtr(&InlineTransparentTypedObject::class_), &inlineObject);

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
    masm.loadObjClass(temp0, temp1);
    masm.branchPtr(Assembler::Equal, temp1, ImmPtr(&InlineOpaqueTypedObject::class_), &inlineObject);
    masm.branchPtr(Assembler::Equal, temp1, ImmPtr(&InlineTransparentTypedObject::class_), &inlineObject);

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
#ifdef JS_CODEGEN_MIPS
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

    // In all implementations, setupUnalignedABICall() relinquishes use of
    // its scratch register. We can therefore save an input register by
    // reusing the scratch register to pass constants to callWithABI.
    masm.setupUnalignedABICall(2, temp);
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

    masm.setupUnalignedABICall(2, temp);
    masm.passABIArg(value, MoveOp::DOUBLE);
    masm.passABIArg(power, MoveOp::DOUBLE);
    masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, ecmaPow), MoveOp::DOUBLE);

    MOZ_ASSERT(ToFloatRegister(ins->output()) == ReturnDoubleReg);
}

void
CodeGenerator::visitRandom(LRandom* ins)
{
    Register temp = ToRegister(ins->temp());
    Register temp2 = ToRegister(ins->temp2());

    masm.loadJSContext(temp);

    masm.setupUnalignedABICall(1, temp2);
    masm.passABIArg(temp);
    masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, math_random_no_outparam), MoveOp::DOUBLE);

    MOZ_ASSERT(ToFloatRegister(ins->output()) == ReturnDoubleReg);
}

void
CodeGenerator::visitMathFunctionD(LMathFunctionD* ins)
{
    Register temp = ToRegister(ins->temp());
    FloatRegister input = ToFloatRegister(ins->input());
    MOZ_ASSERT(ToFloatRegister(ins->output()) == ReturnDoubleReg);

    const MathCache* mathCache = ins->mir()->cache();

    masm.setupUnalignedABICall(mathCache ? 2 : 1, temp);
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

    masm.setupUnalignedABICall(1, temp);
    masm.passABIArg(input, MoveOp::FLOAT32);

    void* funptr = nullptr;
    switch (ins->mir()->function()) {
      case MMathFunction::Floor: funptr = JS_FUNC_TO_DATA_PTR(void*, floorf);           break;
      case MMathFunction::Round: funptr = JS_FUNC_TO_DATA_PTR(void*, math_roundf_impl); break;
      case MMathFunction::Ceil:  funptr = JS_FUNC_TO_DATA_PTR(void*, ceilf);            break;
      default:
        MOZ_CRASH("Unknown or unsupported float32 math function");
    }

    masm.callWithABI(funptr, MoveOp::FLOAT32);
}

void
CodeGenerator::visitModD(LModD* ins)
{
    FloatRegister lhs = ToFloatRegister(ins->lhs());
    FloatRegister rhs = ToFloatRegister(ins->rhs());
    Register temp = ToRegister(ins->temp());

    MOZ_ASSERT(ToFloatRegister(ins->output()) == ReturnDoubleReg);

    masm.setupUnalignedABICall(2, temp);
    masm.passABIArg(lhs, MoveOp::DOUBLE);
    masm.passABIArg(rhs, MoveOp::DOUBLE);

    if (gen->compilingAsmJS())
        masm.callWithABI(AsmJSImm_ModD, MoveOp::DOUBLE);
    else
        masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, NumberMod), MoveOp::DOUBLE);
}

typedef bool (*BinaryFn)(JSContext*, MutableHandleValue, MutableHandleValue, MutableHandleValue);

static const VMFunction AddInfo = FunctionInfo<BinaryFn>(js::AddValues);
static const VMFunction SubInfo = FunctionInfo<BinaryFn>(js::SubValues);
static const VMFunction MulInfo = FunctionInfo<BinaryFn>(js::MulValues);
static const VMFunction DivInfo = FunctionInfo<BinaryFn>(js::DivValues);
static const VMFunction ModInfo = FunctionInfo<BinaryFn>(js::ModValues);
static const VMFunction UrshInfo = FunctionInfo<BinaryFn>(js::UrshValues);

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
    FunctionInfo<StringCompareFn>(jit::StringsEqual<true>);
static const VMFunction StringsNotEqualInfo =
    FunctionInfo<StringCompareFn>(jit::StringsEqual<false>);

void
CodeGenerator::emitCompareS(LInstruction* lir, JSOp op, Register left, Register right,
                            Register output)
{
    MOZ_ASSERT(lir->isCompareS() || lir->isCompareStrictS());

    OutOfLineCode* ool = nullptr;

    if (op == JSOP_EQ || op == JSOP_STRICTEQ) {
        ool = oolCallVM(StringsEqualInfo, lir, (ArgList(), left, right),  StoreRegisterTo(output));
    } else {
        MOZ_ASSERT(op == JSOP_NE || op == JSOP_STRICTNE);
        ool = oolCallVM(StringsNotEqualInfo, lir, (ArgList(), left, right), StoreRegisterTo(output));
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
static const VMFunction EqInfo = FunctionInfo<CompareFn>(jit::LooselyEqual<true>);
static const VMFunction NeInfo = FunctionInfo<CompareFn>(jit::LooselyEqual<false>);
static const VMFunction StrictEqInfo = FunctionInfo<CompareFn>(jit::StrictlyEqual<true>);
static const VMFunction StrictNeInfo = FunctionInfo<CompareFn>(jit::StrictlyEqual<false>);
static const VMFunction LtInfo = FunctionInfo<CompareFn>(jit::LessThan);
static const VMFunction LeInfo = FunctionInfo<CompareFn>(jit::LessThanOrEqual);
static const VMFunction GtInfo = FunctionInfo<CompareFn>(jit::GreaterThan);
static const VMFunction GeInfo = FunctionInfo<CompareFn>(jit::GreaterThanOrEqual);

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
        MOZ_ASSERT(lir->mir()->lhs()->type() != MIRType_Object ||
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

        Register tag = masm.splitTagForTest(value);
        MDefinition* input = lir->mir()->lhs();
        if (input->mightBeType(MIRType_Null))
            masm.branchTestNull(Assembler::Equal, tag, nullOrLikeUndefined);
        if (input->mightBeType(MIRType_Undefined))
            masm.branchTestUndefined(Assembler::Equal, tag, nullOrLikeUndefined);

        if (ool) {
            // Check whether it's a truthy object or a falsy object that emulates
            // undefined.
            masm.branchTestObject(Assembler::NotEqual, tag, notNullOrLikeUndefined);

            Register objreg = masm.extractObject(value, ToTempUnboxRegister(lir->tempToUnbox()));
            branchTestObjectEmulatesUndefined(objreg, nullOrLikeUndefined, notNullOrLikeUndefined,
                                              ToRegister(lir->temp()), ool);
            // fall through
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

        MOZ_ASSERT(lir->cmpMir()->lhs()->type() != MIRType_Object ||
                   lir->cmpMir()->operandMightEmulateUndefined(),
                   "Operands which can't emulate undefined should have been folded");

        OutOfLineTestObject* ool = nullptr;
        if (lir->cmpMir()->operandMightEmulateUndefined()) {
            ool = new(alloc()) OutOfLineTestObject();
            addOutOfLineCode(ool, lir->cmpMir());
        }

        Register tag = masm.splitTagForTest(value);

        Label* ifTrueLabel = getJumpLabelForBranch(ifTrue);
        Label* ifFalseLabel = getJumpLabelForBranch(ifFalse);

        MDefinition* input = lir->cmpMir()->lhs();
        if (input->mightBeType(MIRType_Null))
            masm.branchTestNull(Assembler::Equal, tag, ifTrueLabel);
        if (input->mightBeType(MIRType_Undefined))
            masm.branchTestUndefined(Assembler::Equal, tag, ifTrueLabel);

        if (ool) {
            masm.branchTestObject(Assembler::NotEqual, tag, ifFalseLabel);

            // Objects that emulate undefined are loosely equal to null/undefined.
            Register objreg = masm.extractObject(value, ToTempUnboxRegister(lir->tempToUnbox()));
            Register scratch = ToRegister(lir->temp());
            testObjectEmulatesUndefined(objreg, ifTrueLabel, ifFalseLabel, scratch, ool);
        } else {
            masm.jump(ifFalseLabel);
        }
        return;
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
    MOZ_ASSERT(lhsType == MIRType_Object || lhsType == MIRType_ObjectOrNull);

    JSOp op = lir->mir()->jsop();
    MOZ_ASSERT(lhsType == MIRType_ObjectOrNull || op == JSOP_EQ || op == JSOP_NE,
               "Strict equality should have been folded");

    MOZ_ASSERT(lhsType == MIRType_ObjectOrNull || lir->mir()->operandMightEmulateUndefined(),
               "If the object couldn't emulate undefined, this should have been folded.");

    Register objreg = ToRegister(lir->input());
    Register output = ToRegister(lir->output());

    if ((op == JSOP_EQ || op == JSOP_NE) && lir->mir()->operandMightEmulateUndefined()) {
        OutOfLineTestObjectWithLabels* ool = new(alloc()) OutOfLineTestObjectWithLabels();
        addOutOfLineCode(ool, lir->mir());

        Label* emulatesUndefined = ool->label1();
        Label* doesntEmulateUndefined = ool->label2();

        if (lhsType == MIRType_ObjectOrNull)
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
        MOZ_ASSERT(lhsType == MIRType_ObjectOrNull);

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
    MOZ_ASSERT(lhsType == MIRType_Object || lhsType == MIRType_ObjectOrNull);

    JSOp op = lir->cmpMir()->jsop();
    MOZ_ASSERT(lhsType == MIRType_ObjectOrNull || op == JSOP_EQ || op == JSOP_NE,
               "Strict equality should have been folded");

    MOZ_ASSERT(lhsType == MIRType_ObjectOrNull || lir->cmpMir()->operandMightEmulateUndefined(),
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

        if (lhsType == MIRType_ObjectOrNull)
            masm.branchTestPtr(Assembler::Zero, input, input, ifTrueLabel);

        // Objects that emulate undefined are loosely equal to null/undefined.
        Register scratch = ToRegister(lir->temp());
        testObjectEmulatesUndefined(input, ifTrueLabel, ifFalseLabel, scratch, ool);
    } else {
        MOZ_ASSERT(lhsType == MIRType_ObjectOrNull);
        testZeroEmitBranch(Assembler::Equal, input, ifTrue, ifFalse);
    }
}

typedef JSString* (*ConcatStringsFn)(ExclusiveContext*, HandleString, HandleString);
static const VMFunction ConcatStringsInfo = FunctionInfo<ConcatStringsFn>(ConcatStrings<CanGC>);

void
CodeGenerator::emitConcat(LInstruction* lir, Register lhs, Register rhs, Register output)
{
    OutOfLineCode* ool = oolCallVM(ConcatStringsInfo, lir, (ArgList(), lhs, rhs),
                                   StoreRegisterTo(output));

    JitCode* stringConcatStub = gen->compartment->jitCompartment()->stringConcatStubNoBarrier();
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
    masm.branchTest32(Assembler::NonZero, Address(input, JSString::offsetOfFlags()),
                      Imm32(JSString::LATIN1_CHARS_BIT), &isLatin1);
    {
        masm.loadStringChars(input, input);
        CopyStringChars(masm, destChars, input, temp1, temp2, sizeof(char16_t), sizeof(char16_t));
        masm.jump(&done);
    }
    masm.bind(&isLatin1);
    {
        masm.loadStringChars(input, input);
        CopyStringChars(masm, destChars, input, temp1, temp2, sizeof(char), sizeof(char16_t));
    }
    masm.bind(&done);
}

static void
ConcatInlineString(MacroAssembler& masm, Register lhs, Register rhs, Register output,
                   Register temp1, Register temp2, Register temp3,
                   Label* failure, Label* failurePopTemps, bool isTwoByte)
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
        masm.newGCString(output, temp1, failure);
        masm.store32(Imm32(flags), Address(output, JSString::offsetOfFlags()));
        masm.jump(&allocDone);
    }
    masm.bind(&isFat);
    {
        uint32_t flags = JSString::INIT_FAT_INLINE_FLAGS;
        if (!isTwoByte)
            flags |= JSString::LATIN1_CHARS_BIT;
        masm.newGCFatInlineString(output, temp1, failure);
        masm.store32(Imm32(flags), Address(output, JSString::offsetOfFlags()));
    }
    masm.bind(&allocDone);

    // Store length.
    masm.store32(temp2, Address(output, JSString::offsetOfLength()));

    // Load chars pointer in temp2.
    masm.computeEffectiveAddress(Address(output, JSInlineString::offsetOfInlineStorage()), temp2);

    {
        // Copy lhs chars. Note that this advances temp2 to point to the next
        // char. This also clobbers the lhs register.
        if (isTwoByte) {
            CopyStringCharsMaybeInflate(masm, lhs, temp2, temp1, temp3);
        } else {
            masm.loadStringLength(lhs, temp3);
            masm.loadStringChars(lhs, lhs);
            CopyStringChars(masm, temp2, lhs, temp3, temp1, sizeof(char), sizeof(char));
        }

        // Copy rhs chars. Clobbers the rhs register.
        if (isTwoByte) {
            CopyStringCharsMaybeInflate(masm, rhs, temp2, temp1, temp3);
        } else {
            masm.loadStringLength(rhs, temp3);
            masm.loadStringChars(rhs, rhs);
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
    FunctionInfo<SubstringKernelFn>(SubstringKernel);

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
                                   (ArgList(), string, begin, length),
                                    StoreRegisterTo(output));
    Label* slowPath = ool->entry();
    Label* done = ool->rejoin();

    // Zero length, return emptystring.
    masm.branchTest32(Assembler::NonZero, length, length, &nonZero);
    const JSAtomState& names = GetJitContext()->runtime->names();
    masm.movePtr(ImmGCPtr(names.empty), output);
    masm.jump(done);

    // Use slow path for ropes.
    masm.bind(&nonZero);
    static_assert(JSString::ROPE_FLAGS == 0,
                  "rope flags must be zero for (flags & TYPE_FLAGS_MASK) == 0 "
                  "to be a valid is-rope check");
    masm.branchTest32(Assembler::Zero, stringFlags, Imm32(JSString::TYPE_FLAGS_MASK), slowPath);

    // Handle inlined strings by creating a FatInlineString.
    masm.branchTest32(Assembler::Zero, stringFlags, Imm32(JSString::INLINE_CHARS_BIT), &notInline);
    masm.newGCFatInlineString(output, temp, slowPath);
    masm.store32(length, Address(output, JSString::offsetOfLength()));
    Address stringStorage(string, JSInlineString::offsetOfInlineStorage());
    Address outputStorage(output, JSInlineString::offsetOfInlineStorage());

    masm.branchTest32(Assembler::NonZero, stringFlags, Imm32(JSString::LATIN1_CHARS_BIT),
                      &isInlinedLatin1);
    {
        masm.store32(Imm32(JSString::INIT_FAT_INLINE_FLAGS),
                     Address(output, JSString::offsetOfFlags()));
        masm.computeEffectiveAddress(stringStorage, temp);
        if (temp2 == string)
            masm.push(string);
        BaseIndex chars(temp, begin, ScaleFromElemWidth(sizeof(char16_t)));
        masm.computeEffectiveAddress(chars, temp2);
        masm.computeEffectiveAddress(outputStorage, temp);
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
        if (temp2 == string)
            masm.push(string);
        masm.computeEffectiveAddress(stringStorage, temp2);
        static_assert(sizeof(char) == 1, "begin index shouldn't need scaling");
        masm.addPtr(begin, temp2);
        masm.computeEffectiveAddress(outputStorage, temp);
        CopyStringChars(masm, temp, temp2, length, temp3, sizeof(char), sizeof(char));
        masm.load32(Address(output, JSString::offsetOfLength()), length);
        masm.store8(Imm32(0), Address(temp, 0));
        if (temp2 == string)
            masm.pop(string);
        masm.jump(done);
    }

    // Handle other cases with a DependentString.
    masm.bind(&notInline);
    masm.newGCString(output, temp, slowPath);
    masm.store32(length, Address(output, JSString::offsetOfLength()));
    masm.storePtr(string, Address(output, JSDependentString::offsetOfBase()));

    masm.branchTest32(Assembler::NonZero, stringFlags, Imm32(JSString::LATIN1_CHARS_BIT), &isLatin1);
    {
        masm.store32(Imm32(JSString::DEPENDENT_FLAGS), Address(output, JSString::offsetOfFlags()));
        masm.loadPtr(Address(string, JSString::offsetOfNonInlineChars()), temp);
        BaseIndex chars(temp, begin, ScaleFromElemWidth(sizeof(char16_t)));
        masm.computeEffectiveAddress(chars, temp);
        masm.storePtr(temp, Address(output, JSString::offsetOfNonInlineChars()));
        masm.jump(done);
    }
    masm.bind(&isLatin1);
    {
        masm.store32(Imm32(JSString::DEPENDENT_FLAGS | JSString::LATIN1_CHARS_BIT),
                     Address(output, JSString::offsetOfFlags()));
        masm.loadPtr(Address(string, JSString::offsetOfNonInlineChars()), temp);
        static_assert(sizeof(char) == 1, "begin index shouldn't need scaling");
        masm.addPtr(begin, temp);
        masm.storePtr(temp, Address(output, JSString::offsetOfNonInlineChars()));
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

    Label failure, failurePopTemps;
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

    // Allocate a new rope.
    masm.newGCString(output, temp3, &failure);

    // Store rope length and flags. temp1 still holds the result of AND'ing the
    // lhs and rhs flags, so we just have to clear the other flags to get our
    // rope flags (Latin1 if both lhs and rhs are Latin1).
    static_assert(JSString::ROPE_FLAGS == 0, "Rope flags must be 0");
    masm.and32(Imm32(JSString::LATIN1_CHARS_BIT), temp1);
    masm.store32(temp1, Address(output, JSString::offsetOfFlags()));
    masm.store32(temp2, Address(output, JSString::offsetOfLength()));

    // Store left and right nodes.
    masm.storePtr(lhs, Address(output, JSRope::offsetOfLeft()));
    masm.storePtr(rhs, Address(output, JSRope::offsetOfRight()));
    masm.ret();

    masm.bind(&leftEmpty);
    masm.mov(rhs, output);
    masm.ret();

    masm.bind(&rightEmpty);
    masm.mov(lhs, output);
    masm.ret();

    masm.bind(&isFatInlineTwoByte);
    ConcatInlineString(masm, lhs, rhs, output, temp1, temp2, temp3,
                       &failure, &failurePopTemps, true);

    masm.bind(&isFatInlineLatin1);
    ConcatInlineString(masm, lhs, rhs, output, temp1, temp2, temp3,
                       &failure, &failurePopTemps, false);

    masm.bind(&failurePopTemps);
    masm.pop(temp2);
    masm.pop(temp1);

    masm.bind(&failure);
    masm.movePtr(ImmPtr(nullptr), output);
    masm.ret();

    Linker linker(masm);
    AutoFlushICache afc("StringConcatStub");
    JitCode* code = linker.newCode<CanGC>(cx, OTHER_CODE);

#ifdef JS_ION_PERF
    writePerfSpewerJitCodeProfile(code, "StringConcatStub");
#endif

    return code;
}

JitCode*
JitRuntime::generateMallocStub(JSContext* cx)
{
    const Register regReturn = CallTempReg0;
    const Register regNBytes = CallTempReg0;

    MacroAssembler masm(cx);

    RegisterSet regs = RegisterSet::Volatile();
#ifdef JS_USE_LINK_REGISTER
    masm.pushReturnAddress();
#endif
    regs.takeUnchecked(regNBytes);
    masm.PushRegsInMask(regs);

    const Register regTemp = regs.takeGeneral();
    const Register regRuntime = regTemp;
    regs.add(regTemp);
    MOZ_ASSERT(regTemp != regNBytes);

    masm.setupUnalignedABICall(2, regTemp);
    masm.movePtr(ImmPtr(cx->runtime()), regRuntime);
    masm.passABIArg(regRuntime);
    masm.passABIArg(regNBytes);
    masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, MallocWrapper));
    masm.storeCallResult(regReturn);

    masm.PopRegsInMask(regs);
    masm.ret();

    Linker linker(masm);
    AutoFlushICache afc("MallocStub");
    JitCode* code = linker.newCode<NoGC>(cx, OTHER_CODE);

#ifdef JS_ION_PERF
    writePerfSpewerJitCodeProfile(code, "MallocStub");
#endif

    return code;
}

JitCode*
JitRuntime::generateFreeStub(JSContext* cx)
{
    const Register regSlots = CallTempReg0;

    MacroAssembler masm(cx);
#ifdef JS_USE_LINK_REGISTER
    masm.pushReturnAddress();
#endif
    RegisterSet regs = RegisterSet::Volatile();
    regs.takeUnchecked(regSlots);
    masm.PushRegsInMask(regs);

    const Register regTemp = regs.takeGeneral();
    regs.add(regTemp);
    MOZ_ASSERT(regTemp != regSlots);

    masm.setupUnalignedABICall(1, regTemp);
    masm.passABIArg(regSlots);
    masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, js_free));

    masm.PopRegsInMask(regs);

    masm.ret();

    Linker linker(masm);
    AutoFlushICache afc("FreeStub");
    JitCode* code = linker.newCode<NoGC>(cx, OTHER_CODE);

#ifdef JS_ION_PERF
    writePerfSpewerJitCodeProfile(code, "FreeStub");
#endif

    return code;
}


JitCode*
JitRuntime::generateLazyLinkStub(JSContext* cx)
{
    MacroAssembler masm(cx);
#ifdef JS_USE_LINK_REGISTER
    masm.pushReturnAddress();
#endif

    GeneralRegisterSet regs = GeneralRegisterSet::Volatile();
    Register temp0 = regs.takeAny();

    // The caller did not push an exit frame on the stack, it pushed a
    // JitFrameLayout.  We modify the descriptor to be a valid exit frame and
    // restore it once the lazy link is complete.
    Address descriptor(StackPointer, CommonFrameLayout::offsetOfDescriptor());
    size_t convertToExitFrame = JitFrameLayout::Size() - ExitFrameLayout::Size();
    masm.addPtr(Imm32(convertToExitFrame << FRAMESIZE_SHIFT), descriptor);

    masm.enterFakeExitFrame(LazyLinkExitFrameLayout::Token());
    masm.PushStubCode();

    masm.setupUnalignedABICall(1, temp0);
    masm.loadJSContext(temp0);
    masm.passABIArg(temp0);
    masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, LazyLinkTopActivation));

    masm.leaveExitFrame(/* stub code */ sizeof(JitCode*));

    masm.addPtr(Imm32(- (convertToExitFrame << FRAMESIZE_SHIFT)), descriptor);

#ifdef JS_USE_LINK_REGISTER
    // Restore the return address such that the emitPrologue function of the
    // CodeGenerator can push it back on the stack with pushReturnAddress.
    masm.pop(lr);
#endif
    masm.jump(ReturnReg);

    Linker linker(masm);
    AutoFlushICache afc("LazyLinkStub");
    JitCode* code = linker.newCode<NoGC>(cx, OTHER_CODE);

#ifdef JS_ION_PERF
    writePerfSpewerJitCodeProfile(code, "LazyLinkStub");
#endif
    return code;
}

typedef bool (*CharCodeAtFn)(JSContext*, HandleString, int32_t, uint32_t*);
static const VMFunction CharCodeAtInfo = FunctionInfo<CharCodeAtFn>(jit::CharCodeAt);

void
CodeGenerator::visitCharCodeAt(LCharCodeAt* lir)
{
    Register str = ToRegister(lir->str());
    Register index = ToRegister(lir->index());
    Register output = ToRegister(lir->output());

    OutOfLineCode* ool = oolCallVM(CharCodeAtInfo, lir, (ArgList(), str, index), StoreRegisterTo(output));

    masm.branchIfRope(str, ool->entry());
    masm.loadStringChar(str, index, output);

    masm.bind(ool->rejoin());
}

typedef JSFlatString* (*StringFromCharCodeFn)(JSContext*, int32_t);
static const VMFunction StringFromCharCodeInfo = FunctionInfo<StringFromCharCodeFn>(jit::StringFromCharCode);

void
CodeGenerator::visitFromCharCode(LFromCharCode* lir)
{
    Register code = ToRegister(lir->code());
    Register output = ToRegister(lir->output());

    OutOfLineCode* ool = oolCallVM(StringFromCharCodeInfo, lir, (ArgList(), code), StoreRegisterTo(output));

    // OOL path if code >= UNIT_STATIC_LIMIT.
    masm.branch32(Assembler::AboveOrEqual, code, Imm32(StaticStrings::UNIT_STATIC_LIMIT),
                  ool->entry());

    masm.movePtr(ImmPtr(&GetJitContext()->runtime->staticStrings().unitStaticTable), output);
    masm.loadPtr(BaseIndex(output, code, ScalePointer), output);

    masm.bind(ool->rejoin());
}

typedef JSObject* (*StringSplitFn)(JSContext*, HandleObjectGroup, HandleString, HandleString);
static const VMFunction StringSplitInfo = FunctionInfo<StringSplitFn>(js::str_split_string);

void
CodeGenerator::visitStringSplit(LStringSplit* lir)
{
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
    Int32Key index = ToInt32Key(lir->index());

    masm.bumpKey(&index, 1);
    masm.storeKey(index, initLength);
    // Restore register value if it is used/captured after.
    masm.bumpKey(&index, -1);
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
    if (lir->mir()->operandMightEmulateUndefined() && operand->mightBeType(MIRType_Object)) {
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
    if (lir->index()->isConstant()) {
        // Use uint32 so that the comparison is unsigned.
        uint32_t index = ToInt32(lir->index());
        if (lir->length()->isConstant()) {
            uint32_t length = ToInt32(lir->length());
            if (index < length)
                return;
            bailout(lir->snapshot());
        } else {
            bailoutCmp32(Assembler::BelowOrEqual, ToOperand(lir->length()), Imm32(index),
                         lir->snapshot());
        }
    } else if (lir->length()->isConstant()) {
        bailoutCmp32(Assembler::AboveOrEqual, ToRegister(lir->index()),
                     Imm32(ToInt32(lir->length())), lir->snapshot());
    } else {
        bailoutCmp32(Assembler::BelowOrEqual, ToOperand(lir->length()),
                     ToRegister(lir->index()), lir->snapshot());
    }
}

void
CodeGenerator::visitBoundsCheckRange(LBoundsCheckRange* lir)
{
    int32_t min = lir->mir()->minimum();
    int32_t max = lir->mir()->maximum();
    MOZ_ASSERT(max >= min);

    Register temp = ToRegister(lir->getTemp(0));
    if (lir->index()->isConstant()) {
        int32_t nmin, nmax;
        int32_t index = ToInt32(lir->index());
        if (SafeAdd(index, min, &nmin) && SafeAdd(index, max, &nmax) && nmin >= 0) {
            bailoutCmp32(Assembler::BelowOrEqual, ToOperand(lir->length()), Imm32(nmax),
                         lir->snapshot());
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
            bailoutFrom(&bail, lir->snapshot());
        }

        bailoutCmp32(Assembler::LessThan, temp, Imm32(0), lir->snapshot());

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
            bailoutFrom(&bail, lir->snapshot());
        } else {
            masm.add32(Imm32(max), temp);
        }
    }

    bailoutCmp32(Assembler::BelowOrEqual, ToOperand(lir->length()), temp, lir->snapshot());
}

void
CodeGenerator::visitBoundsCheckLower(LBoundsCheckLower* lir)
{
    int32_t min = lir->mir()->minimum();
    bailoutCmp32(Assembler::LessThan, ToRegister(lir->index()), Imm32(min),
                 lir->snapshot());
}

class OutOfLineStoreElementHole : public OutOfLineCodeBase<CodeGenerator>
{
    LInstruction* ins_;
    Label rejoinStore_;

  public:
    explicit OutOfLineStoreElementHole(LInstruction* ins)
      : ins_(ins)
    {
        MOZ_ASSERT(ins->isStoreElementHoleV() || ins->isStoreElementHoleT());
    }

    void accept(CodeGenerator* codegen) {
        codegen->visitOutOfLineStoreElementHole(this);
    }
    LInstruction* ins() const {
        return ins_;
    }
    Label* rejoinStore() {
        return &rejoinStore_;
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

void
CodeGenerator::emitStoreElementTyped(const LAllocation* value,
                                     MIRType valueType, MIRType elementType,
                                     Register elements, const LAllocation* index,
                                     int32_t offsetAdjustment)
{
    ConstantOrRegister v;
    if (value->isConstant())
        v = ConstantOrRegister(*value->toConstant());
    else
        v = TypedOrValueRegister(valueType, ToAnyRegister(value));

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
        emitPreBarrier(elements, index);

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
        emitPreBarrier(elements, index);

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

void
CodeGenerator::visitStoreElementHoleT(LStoreElementHoleT* lir)
{
    OutOfLineStoreElementHole* ool = new(alloc()) OutOfLineStoreElementHole(lir);
    addOutOfLineCode(ool, lir->mir());

    Register elements = ToRegister(lir->elements());
    const LAllocation* index = lir->index();

    // OOL path if index >= initializedLength.
    Address initLength(elements, ObjectElements::offsetOfInitializedLength());
    masm.branchKey(Assembler::BelowOrEqual, initLength, ToInt32Key(index), ool->entry());

    if (lir->mir()->needsBarrier())
        emitPreBarrier(elements, index);

    masm.bind(ool->rejoinStore());
    emitStoreElementTyped(lir->value(), lir->mir()->value()->type(), lir->mir()->elementType(),
                          elements, index, 0);

    masm.bind(ool->rejoin());
}

void
CodeGenerator::visitStoreElementHoleV(LStoreElementHoleV* lir)
{
    OutOfLineStoreElementHole* ool = new(alloc()) OutOfLineStoreElementHole(lir);
    addOutOfLineCode(ool, lir->mir());

    Register elements = ToRegister(lir->elements());
    const LAllocation* index = lir->index();
    const ValueOperand value = ToValue(lir, LStoreElementHoleV::Value);

    // OOL path if index >= initializedLength.
    Address initLength(elements, ObjectElements::offsetOfInitializedLength());
    masm.branchKey(Assembler::BelowOrEqual, initLength, ToInt32Key(index), ool->entry());

    if (lir->mir()->needsBarrier())
        emitPreBarrier(elements, index);

    masm.bind(ool->rejoinStore());
    if (lir->index()->isConstant())
        masm.storeValue(value, Address(elements, ToInt32(lir->index()) * sizeof(js::Value)));
    else
        masm.storeValue(value, BaseIndex(elements, ToRegister(lir->index()), TimesEight));

    masm.bind(ool->rejoin());
}

typedef bool (*SetDenseElementFn)(JSContext*, HandleNativeObject, int32_t, HandleValue,
                                  bool strict);
static const VMFunction SetDenseElementInfo = FunctionInfo<SetDenseElementFn>(SetDenseElement);

void
CodeGenerator::visitOutOfLineStoreElementHole(OutOfLineStoreElementHole* ool)
{
    Register object, elements;
    LInstruction* ins = ool->ins();
    const LAllocation* index;
    MIRType valueType;
    ConstantOrRegister value;

    if (ins->isStoreElementHoleV()) {
        LStoreElementHoleV* store = ins->toStoreElementHoleV();
        object = ToRegister(store->object());
        elements = ToRegister(store->elements());
        index = store->index();
        valueType = store->mir()->value()->type();
        value = TypedOrValueRegister(ToValue(store, LStoreElementHoleV::Value));
    } else {
        LStoreElementHoleT* store = ins->toStoreElementHoleT();
        object = ToRegister(store->object());
        elements = ToRegister(store->elements());
        index = store->index();
        valueType = store->mir()->value()->type();
        if (store->value()->isConstant())
            value = ConstantOrRegister(*store->value()->toConstant());
        else
            value = TypedOrValueRegister(valueType, ToAnyRegister(store->value()));
    }

    // If index == initializedLength, try to bump the initialized length inline.
    // If index > initializedLength, call a stub. Note that this relies on the
    // condition flags sticking from the incoming branch.
    Label callStub;
#ifdef JS_CODEGEN_MIPS
    // Had to reimplement for MIPS because there are no flags.
    Address initLength(elements, ObjectElements::offsetOfInitializedLength());
    masm.branchKey(Assembler::NotEqual, initLength, ToInt32Key(index), &callStub);
#else
    masm.j(Assembler::NotEqual, &callStub);
#endif

    Int32Key key = ToInt32Key(index);

    // Check array capacity.
    masm.branchKey(Assembler::BelowOrEqual, Address(elements, ObjectElements::offsetOfCapacity()),
                   key, &callStub);

    // Update initialized length. The capacity guard above ensures this won't overflow,
    // due to NELEMENTS_LIMIT.
    masm.bumpKey(&key, 1);
    masm.storeKey(key, Address(elements, ObjectElements::offsetOfInitializedLength()));

    // Update length if length < initializedLength.
    Label dontUpdate;
    masm.branchKey(Assembler::AboveOrEqual, Address(elements, ObjectElements::offsetOfLength()),
                   key, &dontUpdate);
    masm.storeKey(key, Address(elements, ObjectElements::offsetOfLength()));
    masm.bind(&dontUpdate);

    masm.bumpKey(&key, -1);

    if (ins->isStoreElementHoleT() && valueType != MIRType_Double) {
        // The inline path for StoreElementHoleT does not always store the type tag,
        // so we do the store on the OOL path. We use MIRType_None for the element type
        // so that storeElementTyped will always store the type tag.
        emitStoreElementTyped(ins->toStoreElementHoleT()->value(), valueType, MIRType_None,
                              elements, index, 0);
        masm.jump(ool->rejoin());
    } else {
        // Jump to the inline path where we will store the value.
        masm.jump(ool->rejoinStore());
    }

    masm.bind(&callStub);
    saveLive(ins);

    pushArg(Imm32(current->mir()->strict()));
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
StoreUnboxedPointer(MacroAssembler& masm, T address, MIRType type, const LAllocation* value)
{
    masm.patchableCallPreBarrier(address, type);
    if (value->isConstant()) {
        Value v = *value->toConstant();
        if (v.isMarkable()) {
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
    if (lir->mir()->isStoreUnboxedObjectOrNull()) {
        type = MIRType_Object;
        offsetAdjustment = lir->mir()->toStoreUnboxedObjectOrNull()->offsetAdjustment();
    } else if (lir->mir()->isStoreUnboxedString()) {
        type = MIRType_String;
        offsetAdjustment = lir->mir()->toStoreUnboxedString()->offsetAdjustment();
    } else {
        MOZ_CRASH();
    }

    Register elements = ToRegister(lir->elements());
    const LAllocation* index = lir->index();
    const LAllocation* value = lir->value();

    if (index->isConstant()) {
        Address address(elements, ToInt32(index) * sizeof(uintptr_t) + offsetAdjustment);
        StoreUnboxedPointer(masm, address, type, value);
    } else {
        BaseIndex address(elements, ToRegister(index), ScalePointer, offsetAdjustment);
        StoreUnboxedPointer(masm, address, type, value);
    }
}

typedef bool (*ConvertUnboxedObjectToNativeFn)(JSContext*, JSObject*);
static const VMFunction ConvertUnboxedObjectToNativeInfo =
    FunctionInfo<ConvertUnboxedObjectToNativeFn>(UnboxedPlainObject::convertToNative);

void
CodeGenerator::visitConvertUnboxedObjectToNative(LConvertUnboxedObjectToNative* lir)
{
    Register object = ToRegister(lir->getOperand(0));

    OutOfLineCode* ool = oolCallVM(ConvertUnboxedObjectToNativeInfo, lir,
                                   (ArgList(), object), StoreNothing());

    masm.branchPtr(Assembler::Equal, Address(object, JSObject::offsetOfGroup()),
                   ImmGCPtr(lir->mir()->group()), ool->entry());
    masm.bind(ool->rejoin());
}

typedef bool (*ArrayPopShiftFn)(JSContext*, HandleObject, MutableHandleValue);
static const VMFunction ArrayPopDenseInfo = FunctionInfo<ArrayPopShiftFn>(jit::ArrayPopDense);
static const VMFunction ArrayShiftDenseInfo = FunctionInfo<ArrayPopShiftFn>(jit::ArrayShiftDense);

void
CodeGenerator::emitArrayPopShift(LInstruction* lir, const MArrayPopShift* mir, Register obj,
                                 Register elementsTemp, Register lengthTemp, TypedOrValueRegister out)
{
    OutOfLineCode* ool;

    if (mir->mode() == MArrayPopShift::Pop) {
        ool = oolCallVM(ArrayPopDenseInfo, lir, (ArgList(), obj), StoreValueTo(out));
    } else {
        MOZ_ASSERT(mir->mode() == MArrayPopShift::Shift);
        ool = oolCallVM(ArrayShiftDenseInfo, lir, (ArgList(), obj), StoreValueTo(out));
    }

    // VM call if a write barrier is necessary.
    masm.branchTestNeedsIncrementalBarrier(Assembler::NonZero, ool->entry());

    // Load elements and length.
    masm.loadPtr(Address(obj, NativeObject::offsetOfElements()), elementsTemp);
    masm.load32(Address(elementsTemp, ObjectElements::offsetOfLength()), lengthTemp);

    // VM call if length != initializedLength.
    Int32Key key = Int32Key(lengthTemp);
    Address initLength(elementsTemp, ObjectElements::offsetOfInitializedLength());
    masm.branchKey(Assembler::NotEqual, initLength, key, ool->entry());

    // Test for length != 0. On zero length either take a VM call or generate
    // an undefined value, depending on whether the call is known to produce
    // undefined.
    Label done;
    if (mir->maybeUndefined()) {
        Label notEmpty;
        masm.branchTest32(Assembler::NonZero, lengthTemp, lengthTemp, &notEmpty);
        masm.moveValue(UndefinedValue(), out.valueReg());
        masm.jump(&done);
        masm.bind(&notEmpty);
    } else {
        masm.branchTest32(Assembler::Zero, lengthTemp, lengthTemp, ool->entry());
    }

    masm.bumpKey(&key, -1);

    if (mir->mode() == MArrayPopShift::Pop) {
        masm.loadElementTypedOrValue(BaseIndex(elementsTemp, lengthTemp, TimesEight), out,
                                     mir->needsHoleCheck(), ool->entry());
    } else {
        MOZ_ASSERT(mir->mode() == MArrayPopShift::Shift);
        masm.loadElementTypedOrValue(Address(elementsTemp, 0), out, mir->needsHoleCheck(),
                                     ool->entry());
    }

    // Handle the failure case when the array length is non-writable in the
    // OOL path.  (Unlike in the adding-an-element cases, we can't rely on the
    // capacity <= length invariant for such arrays to avoid an explicit
    // check.)
    Address elementFlags(elementsTemp, ObjectElements::offsetOfFlags());
    Imm32 bit(ObjectElements::NONWRITABLE_ARRAY_LENGTH);
    masm.branchTest32(Assembler::NonZero, elementFlags, bit, ool->entry());

    // Now adjust length and initializedLength.
    masm.store32(lengthTemp, Address(elementsTemp, ObjectElements::offsetOfLength()));
    masm.store32(lengthTemp, Address(elementsTemp, ObjectElements::offsetOfInitializedLength()));

    if (mir->mode() == MArrayPopShift::Shift) {
        // Don't save the temp registers.
        RegisterSet temps;
        temps.add(elementsTemp);
        temps.add(lengthTemp);

        saveVolatile(temps);
        masm.setupUnalignedABICall(1, lengthTemp);
        masm.passABIArg(obj);
        masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, js::ArrayShiftMoveElements));
        restoreVolatile(temps);
    }

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
    FunctionInfo<ArrayPushDenseFn>(jit::ArrayPushDense);

void
CodeGenerator::emitArrayPush(LInstruction* lir, const MArrayPush* mir, Register obj,
                             ConstantOrRegister value, Register elementsTemp, Register length)
{
    OutOfLineCode* ool = oolCallVM(ArrayPushDenseInfo, lir, (ArgList(), obj, value), StoreRegisterTo(length));

    // Load elements and length.
    masm.loadPtr(Address(obj, NativeObject::offsetOfElements()), elementsTemp);
    masm.load32(Address(elementsTemp, ObjectElements::offsetOfLength()), length);

    Int32Key key = Int32Key(length);
    Address initLength(elementsTemp, ObjectElements::offsetOfInitializedLength());
    Address capacity(elementsTemp, ObjectElements::offsetOfCapacity());

    // Guard length == initializedLength.
    masm.branchKey(Assembler::NotEqual, initLength, key, ool->entry());

    // Guard length < capacity.
    masm.branchKey(Assembler::BelowOrEqual, capacity, key, ool->entry());

    masm.storeConstantOrRegister(value, BaseIndex(elementsTemp, length, TimesEight));

    masm.bumpKey(&key, 1);
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
    emitArrayPush(lir, lir->mir(), obj, value, elementsTemp, length);
}

void
CodeGenerator::visitArrayPushT(LArrayPushT* lir)
{
    Register obj = ToRegister(lir->object());
    Register elementsTemp = ToRegister(lir->temp());
    Register length = ToRegister(lir->output());
    ConstantOrRegister value;
    if (lir->value()->isConstant())
        value = ConstantOrRegister(*lir->value()->toConstant());
    else
        value = TypedOrValueRegister(lir->mir()->value()->type(), ToAnyRegister(lir->value()));
    emitArrayPush(lir, lir->mir(), obj, value, elementsTemp, length);
}

typedef JSObject* (*ArrayConcatDenseFn)(JSContext*, HandleObject, HandleObject, HandleObject);
static const VMFunction ArrayConcatDenseInfo = FunctionInfo<ArrayConcatDenseFn>(ArrayConcatDense);

void
CodeGenerator::visitArrayConcat(LArrayConcat* lir)
{
    Register lhs = ToRegister(lir->lhs());
    Register rhs = ToRegister(lir->rhs());
    Register temp1 = ToRegister(lir->temp1());
    Register temp2 = ToRegister(lir->temp2());

    // If 'length == initializedLength' for both arrays we try to allocate an object
    // inline and pass it to the stub. Else, we just pass nullptr and the stub falls
    // back to a slow path.
    Label fail, call;
    masm.loadPtr(Address(lhs, NativeObject::offsetOfElements()), temp1);
    masm.load32(Address(temp1, ObjectElements::offsetOfInitializedLength()), temp2);
    masm.branch32(Assembler::NotEqual, Address(temp1, ObjectElements::offsetOfLength()), temp2, &fail);

    masm.loadPtr(Address(rhs, NativeObject::offsetOfElements()), temp1);
    masm.load32(Address(temp1, ObjectElements::offsetOfInitializedLength()), temp2);
    masm.branch32(Assembler::NotEqual, Address(temp1, ObjectElements::offsetOfLength()), temp2, &fail);

    // Try to allocate an object.
    masm.createGCObject(temp1, temp2, lir->mir()->templateObj(), lir->mir()->initialHeap(), &fail);
    masm.jump(&call);
    {
        masm.bind(&fail);
        masm.movePtr(ImmPtr(nullptr), temp1);
    }
    masm.bind(&call);

    pushArg(temp1);
    pushArg(ToRegister(lir->rhs()));
    pushArg(ToRegister(lir->lhs()));
    callVM(ArrayConcatDenseInfo, lir);
}

typedef JSString* (*ArrayJoinFn)(JSContext*, HandleObject, HandleString);
static const VMFunction ArrayJoinInfo = FunctionInfo<ArrayJoinFn>(jit::ArrayJoin);

void
CodeGenerator::visitArrayJoin(LArrayJoin* lir)
{
    pushArg(ToRegister(lir->separator()));
    pushArg(ToRegister(lir->array()));

    callVM(ArrayJoinInfo, lir);
}

typedef JSObject* (*GetIteratorObjectFn)(JSContext*, HandleObject, uint32_t);
static const VMFunction GetIteratorObjectInfo = FunctionInfo<GetIteratorObjectFn>(GetIteratorObject);

void
CodeGenerator::visitCallIteratorStart(LCallIteratorStart* lir)
{
    pushArg(Imm32(lir->mir()->flags()));
    pushArg(ToRegister(lir->object()));
    callVM(GetIteratorObjectInfo, lir);
}

void
CodeGenerator::visitIteratorStart(LIteratorStart* lir)
{
    const Register obj = ToRegister(lir->object());
    const Register output = ToRegister(lir->output());

    uint32_t flags = lir->mir()->flags();

    OutOfLineCode* ool = oolCallVM(GetIteratorObjectInfo, lir,
                                   (ArgList(), obj, Imm32(flags)), StoreRegisterTo(output));

    const Register temp1 = ToRegister(lir->temp1());
    const Register temp2 = ToRegister(lir->temp2());
    const Register niTemp = ToRegister(lir->temp3()); // Holds the NativeIterator object.

    // Iterators other than for-in should use LCallIteratorStart.
    MOZ_ASSERT(flags == JSITER_ENUMERATE);

    // Fetch the most recent iterator and ensure it's not nullptr.
    masm.loadPtr(AbsoluteAddress(GetJitContext()->runtime->addressOfLastCachedNativeIterator()), output);
    masm.branchTestPtr(Assembler::Zero, output, output, ool->entry());

    // Load NativeIterator.
    masm.loadObjPrivate(output, JSObject::ITER_CLASS_NFIXED_SLOTS, niTemp);

    // Ensure the |active| and |unreusable| bits are not set.
    masm.branchTest32(Assembler::NonZero, Address(niTemp, offsetof(NativeIterator, flags)),
                      Imm32(JSITER_ACTIVE|JSITER_UNREUSABLE), ool->entry());

    // Load the iterator's shape array.
    masm.loadPtr(Address(niTemp, offsetof(NativeIterator, shapes_array)), temp2);

    // Compare shape of object with the first shape.
    masm.loadObjShape(obj, temp1);
    masm.branchPtr(Assembler::NotEqual, Address(temp2, 0), temp1, ool->entry());

    // Compare shape of object's prototype with the second shape.
    masm.loadObjProto(obj, temp1);
    masm.loadObjShape(temp1, temp1);
    masm.branchPtr(Assembler::NotEqual, Address(temp2, sizeof(Shape*)), temp1, ool->entry());

    // Ensure the object's prototype's prototype is nullptr. The last native
    // iterator will always have a prototype chain length of one (i.e. it must
    // be a plain ), so we do not need to generate a loop here.
    masm.loadObjProto(obj, temp1);
    masm.loadObjProto(temp1, temp1);
    masm.branchTestPtr(Assembler::NonZero, temp1, temp1, ool->entry());

    // Ensure the object does not have any elements. The presence of dense
    // elements is not captured by the shape tests above.
    masm.branchPtr(Assembler::NotEqual,
                   Address(obj, NativeObject::offsetOfElements()),
                   ImmPtr(js::emptyObjectElements),
                   ool->entry());

    // Write barrier for stores to the iterator. We only need to take a write
    // barrier if NativeIterator::obj is actually going to change.
    {
        // Bug 867815: Unconditionally take this out- of-line so that we do not
        // have to post-barrier the store to NativeIter::obj. This just needs
        // JIT support for the Cell* buffer.
        Address objAddr(niTemp, offsetof(NativeIterator, obj));
        masm.branchPtr(Assembler::NotEqual, objAddr, obj, ool->entry());
    }

    // Mark iterator as active.
    masm.storePtr(obj, Address(niTemp, offsetof(NativeIterator, obj)));
    masm.or32(Imm32(JSITER_ACTIVE), Address(niTemp, offsetof(NativeIterator, flags)));

    // Chain onto the active iterator stack.
    masm.loadPtr(AbsoluteAddress(gen->compartment->addressOfEnumerators()), temp1);

    // ni->next = list
    masm.storePtr(temp1, Address(niTemp, NativeIterator::offsetOfNext()));

    // ni->prev = list->prev
    masm.loadPtr(Address(temp1, NativeIterator::offsetOfPrev()), temp2);
    masm.storePtr(temp2, Address(niTemp, NativeIterator::offsetOfPrev()));

    // list->prev->next = ni
    masm.storePtr(niTemp, Address(temp2, NativeIterator::offsetOfNext()));

    // list->prev = ni
    masm.storePtr(niTemp, Address(temp1, NativeIterator::offsetOfPrev()));

    masm.bind(ool->rejoin());
}

static void
LoadNativeIterator(MacroAssembler& masm, Register obj, Register dest, Label* failures)
{
    MOZ_ASSERT(obj != dest);

    // Test class.
    masm.branchTestObjClass(Assembler::NotEqual, obj, dest, &PropertyIteratorObject::class_, failures);

    // Load NativeIterator object.
    masm.loadObjPrivate(obj, JSObject::ITER_CLASS_NFIXED_SLOTS, dest);
}

typedef bool (*IteratorMoreFn)(JSContext*, HandleObject, MutableHandleValue);
static const VMFunction IteratorMoreInfo = FunctionInfo<IteratorMoreFn>(IteratorMore);

void
CodeGenerator::visitIteratorMore(LIteratorMore* lir)
{
    const Register obj = ToRegister(lir->object());
    const ValueOperand output = ToOutValue(lir);
    const Register temp = ToRegister(lir->temp());

    OutOfLineCode* ool = oolCallVM(IteratorMoreInfo, lir, (ArgList(), obj), StoreValueTo(output));

    Register outputScratch = output.scratchReg();
    LoadNativeIterator(masm, obj, outputScratch, ool->entry());

    masm.branchTest32(Assembler::NonZero, Address(outputScratch, offsetof(NativeIterator, flags)),
                      Imm32(JSITER_FOREACH), ool->entry());

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

typedef bool (*CloseIteratorFn)(JSContext*, HandleObject);
static const VMFunction CloseIteratorInfo = FunctionInfo<CloseIteratorFn>(CloseIterator);

void
CodeGenerator::visitIteratorEnd(LIteratorEnd* lir)
{
    const Register obj = ToRegister(lir->object());
    const Register temp1 = ToRegister(lir->temp1());
    const Register temp2 = ToRegister(lir->temp2());
    const Register temp3 = ToRegister(lir->temp3());

    OutOfLineCode* ool = oolCallVM(CloseIteratorInfo, lir, (ArgList(), obj), StoreNothing());

    LoadNativeIterator(masm, obj, temp1, ool->entry());

    masm.branchTest32(Assembler::Zero, Address(temp1, offsetof(NativeIterator, flags)),
                      Imm32(JSITER_ENUMERATE), ool->entry());

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
    Address ptr(StackPointer, frameSize() + JitFrameLayout::offsetOfNumActualArgs());

    masm.loadPtr(ptr, argc);
}

void
CodeGenerator::visitGetFrameArgument(LGetFrameArgument* lir)
{
    ValueOperand result = GetValueOutput(lir);
    const LAllocation* index = lir->index();
    size_t argvOffset = frameSize() + JitFrameLayout::offsetOfActualArgs();

    if (index->isConstant()) {
        int32_t i = index->toConstant()->toInt32();
        Address argPtr(StackPointer, sizeof(Value) * i + argvOffset);
        masm.loadValue(argPtr, result);
    } else {
        Register i = ToRegister(index);
        BaseValueIndex argPtr(StackPointer, i, argvOffset);
        masm.loadValue(argPtr, result);
    }
}

void
CodeGenerator::visitSetFrameArgumentT(LSetFrameArgumentT* lir)
{
    size_t argOffset = frameSize() + JitFrameLayout::offsetOfActualArgs() +
                       (sizeof(Value) * lir->mir()->argno());

    MIRType type = lir->mir()->value()->type();

    if (type == MIRType_Double) {
        // Store doubles directly.
        FloatRegister input = ToFloatRegister(lir->input());
        masm.storeDouble(input, Address(StackPointer, argOffset));

    } else {
        Register input = ToRegister(lir->input());
        masm.storeValue(ValueTypeFromMIRType(type), input, Address(StackPointer, argOffset));
    }
}

void
CodeGenerator:: visitSetFrameArgumentC(LSetFrameArgumentC* lir)
{
    size_t argOffset = frameSize() + JitFrameLayout::offsetOfActualArgs() +
                       (sizeof(Value) * lir->mir()->argno());
    masm.storeValue(lir->val(), Address(StackPointer, argOffset));
}

void
CodeGenerator:: visitSetFrameArgumentV(LSetFrameArgumentV* lir)
{
    const ValueOperand val = ToValue(lir, LSetFrameArgumentV::Input);
    size_t argOffset = frameSize() + JitFrameLayout::offsetOfActualArgs() +
                       (sizeof(Value) * lir->mir()->argno());
    masm.storeValue(val, Address(StackPointer, argOffset));
}

typedef bool (*RunOnceScriptPrologueFn)(JSContext*, HandleScript);
static const VMFunction RunOnceScriptPrologueInfo =
    FunctionInfo<RunOnceScriptPrologueFn>(js::RunOnceScriptPrologue);

void
CodeGenerator::visitRunOncePrologue(LRunOncePrologue* lir)
{
    pushArg(ImmGCPtr(lir->mir()->block()->info().script()));
    callVM(RunOnceScriptPrologueInfo, lir);
}

typedef JSObject* (*InitRestParameterFn)(JSContext*, uint32_t, Value*, HandleObject,
                                         HandleObject);
static const VMFunction InitRestParameterInfo =
    FunctionInfo<InitRestParameterFn>(InitRestParameter);

void
CodeGenerator::emitRest(LInstruction* lir, Register array, Register numActuals,
                        Register temp0, Register temp1, unsigned numFormals,
                        JSObject* templateObject, bool saveAndRestore, Register resultreg)
{
    // Compute actuals() + numFormals.
    size_t actualsOffset = frameSize() + JitFrameLayout::offsetOfActualArgs();
    masm.movePtr(StackPointer, temp1);
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
        storeResultTo(resultreg);
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
CodeGenerator::generateAsmJS(AsmJSFunctionLabels* labels)
{
    JitSpew(JitSpew_Codegen, "# Emitting asm.js code");

    if (!omitOverRecursedCheck())
        labels->overflowThunk.emplace();

    GenerateAsmJSFunctionPrologue(masm, frameSize(), labels);

    if (!generateBody())
        return false;

    masm.bind(&returnLabel_);
    GenerateAsmJSFunctionEpilogue(masm, frameSize(), labels);

#if defined(JS_ION_PERF)
    // Note the end of the inline code and start of the OOL code.
    gen->perfSpewer().noteEndInlineCode(masm);
#endif

    if (!generateOutOfLineCode())
        return false;

    masm.bind(&labels->end);

    // The only remaining work needed to compile this function is to patch the
    // switch-statement jump tables (the entries of the table need the absolute
    // address of the cases). These table entries are accmulated as CodeLabels
    // in the MacroAssembler's codeLabels_ list and processed all at once at in
    // the "static-link" phase of module compilation. It is critical that there
    // is nothing else to do after this point since the LifoAlloc memory
    // holding the MIR graph is about to be popped and reused. In particular,
    // every step in CodeGenerator::link must be a nop, as asserted here:
    MOZ_ASSERT(snapshots_.listSize() == 0);
    MOZ_ASSERT(snapshots_.RVATableSize() == 0);
    MOZ_ASSERT(recovers_.size() == 0);
    MOZ_ASSERT(bailouts_.empty());
    MOZ_ASSERT(graph.numConstants() == 0);
    MOZ_ASSERT(safepointIndices_.empty());
    MOZ_ASSERT(osiIndices_.empty());
    MOZ_ASSERT(cacheList_.empty());
    MOZ_ASSERT(safepoints_.size() == 0);
    return true;
}

bool
CodeGenerator::generate()
{
    JitSpew(JitSpew_Codegen, "# Emitting code for script %s:%d",
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

    if (frameClass_ != FrameSizeClass::None()) {
        deoptTable_ = gen->jitRuntime()->getBailoutTable(frameClass_);
        if (!deoptTable_)
            return false;
    }

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
    generateArgumentsChecks(/* bailout = */ false);
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

struct AutoDiscardIonCode
{
    JSContext* cx;
    RecompileInfo* recompileInfo;
    IonScript* ionScript;
    bool keep;

    AutoDiscardIonCode(JSContext* cx, RecompileInfo* recompileInfo)
      : cx(cx), recompileInfo(recompileInfo), ionScript(nullptr), keep(false) {}

    ~AutoDiscardIonCode() {
        if (keep)
            return;

        // Use js_free instead of IonScript::Destroy: the cache list and
        // backedge list are still uninitialized.
        if (ionScript)
            js_free(ionScript);

        recompileInfo->compilerOutput(cx->zone()->types)->invalidate();
    }

    void keepIonCode() {
        keep = true;
    }
};

bool
CodeGenerator::link(JSContext* cx, CompilerConstraintList* constraints)
{
    RootedScript script(cx, gen->info().script());
    OptimizationLevel optimizationLevel = gen->optimizationInfo().level();

    // We finished the new IonScript. Invalidate the current active IonScript,
    // so we can replace it with this new (probably higher optimized) version.
    if (script->hasIonScript()) {
        MOZ_ASSERT(script->ionScript()->isRecompiling());
        // Do a normal invalidate, except don't cancel offThread compilations,
        // since that will cancel this compilation too.
        if (!Invalidate(cx, script, /* resetUses */ false, /* cancelOffThread*/ false))
            return false;
    }

    if (scriptCounts_ && !script->hasScriptCounts() && !script->initScriptCounts(cx))
        return false;

    // Check to make sure we didn't have a mid-build invalidation. If so, we
    // will trickle to jit::Compile() and return Method_Skipped.
    uint32_t warmUpCount = script->getWarmUpCount();
    RecompileInfo recompileInfo;
    if (!FinishCompilation(cx, script, constraints, &recompileInfo))
        return true;

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
    encodeSafepoints();

    AutoDiscardIonCode discardIonCode(cx, &recompileInfo);

    IonScript* ionScript =
      IonScript::New(cx, recompileInfo,
                     graph.totalSlotCount(), argumentSlots, scriptFrameSize,
                     snapshots_.listSize(), snapshots_.RVATableSize(),
                     recovers_.size(), bailouts_.length(), graph.numConstants(),
                     safepointIndices_.length(), osiIndices_.length(),
                     cacheList_.length(), runtimeData_.length(),
                     safepoints_.size(), patchableBackedges_.length(), optimizationLevel);
    if (!ionScript)
        return false;
    discardIonCode.ionScript = ionScript;

    // Also, note that creating the code here during an incremental GC will
    // trace the code and mark all GC things it refers to. This captures any
    // read barriers which were skipped while compiling the script off thread.
    Linker linker(masm);
    AutoFlushICache afc("IonLink");
    JitCode* code = linker.newCode<CanGC>(cx, ION_CODE);
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
            }
        }

        // Add entry to the global table.
        JitcodeGlobalTable* globalTable = cx->runtime()->jitRuntime()->getJitcodeGlobalTable();
        if (!globalTable->addEntry(entry, cx->runtime())) {
            // Memory may have been allocated for the entry.
            entry.destroy();
            return false;
        }

        // Mark the jitcode as having a bytecode map.
        code->setHasBytecodeMap();
    } else {
        // Add a dumy jitcodeGlobalTable entry.
        JitcodeGlobalEntry::DummyEntry entry;
        entry.init(code->raw(), code->rawEnd());

        // Add entry to the global table.
        JitcodeGlobalTable* globalTable = cx->runtime()->jitRuntime()->getJitcodeGlobalTable();
        if (!globalTable->addEntry(entry, cx->runtime())) {
            // Memory may have been allocated for the entry.
            entry.destroy();
            return false;
        }

        // Mark the jitcode as having a bytecode map.
        code->setHasBytecodeMap();
    }

    ionScript->setMethod(code);
    ionScript->setSkipArgCheckEntryOffset(getSkipArgCheckEntryOffset());

    // If SPS is enabled, mark IonScript as having been instrumented with SPS
    if (isProfilerInstrumentationEnabled())
        ionScript->setHasProfilingInstrumentation();

    script->setIonScript(cx, ionScript);

    invalidateEpilogueData_.fixup(&masm);
    Assembler::PatchDataWithValueCheck(CodeLocationLabel(code, invalidateEpilogueData_),
                                       ImmPtr(ionScript),
                                       ImmPtr((void*)-1));

    JitSpew(JitSpew_Codegen, "Created IonScript %p (raw %p)",
            (void*) ionScript, (void*) code->raw());

    ionScript->setInvalidationEpilogueDataOffset(invalidateEpilogueData_.offset());
    ionScript->setOsrPc(gen->info().osrPc());
    ionScript->setOsrEntryOffset(getOsrEntryOffset());
    ptrdiff_t real_invalidate = masm.actualOffset(invalidate_.offset());
    ionScript->setInvalidationEpilogueOffset(real_invalidate);

    ionScript->setDeoptTable(deoptTable_);

#if defined(JS_ION_PERF)
    if (PerfEnabled())
        perfSpewer_.writeProfile(script, code, masm);
#endif

    for (size_t i = 0; i < ionScriptLabels_.length(); i++) {
        ionScriptLabels_[i].fixup(&masm);
        Assembler::PatchDataWithValueCheck(CodeLocationLabel(code, ionScriptLabels_[i]),
                                           ImmPtr(ionScript),
                                           ImmPtr((void*)-1));
    }

    // for generating inline caches during the execution.
    if (runtimeData_.length())
        ionScript->copyRuntimeData(&runtimeData_[0]);
    if (cacheList_.length())
        ionScript->copyCacheEntries(&cacheList_[0], masm);

    // for marking during GC.
    if (safepointIndices_.length())
        ionScript->copySafepointIndices(&safepointIndices_[0], masm);
    if (safepoints_.size())
        ionScript->copySafepoints(&safepoints_);

    // for reconvering from an Ion Frame.
    if (bailouts_.length())
        ionScript->copyBailoutTable(&bailouts_[0]);
    if (osiIndices_.length())
        ionScript->copyOsiIndices(&osiIndices_[0], masm);
    if (snapshots_.listSize())
        ionScript->copySnapshots(&snapshots_);
    MOZ_ASSERT_IF(snapshots_.listSize(), recovers_.size());
    if (recovers_.size())
        ionScript->copyRecovers(&recovers_);
    if (graph.numConstants())
        ionScript->copyConstants(graph.constantPool());
    if (patchableBackedges_.length() > 0)
        ionScript->copyPatchableBackedges(cx, code, patchableBackedges_.begin(), masm);

#ifdef JS_TRACE_LOGGING
    TraceLoggerThread* logger = TraceLoggerForMainThread(cx->runtime());
    for (uint32_t i = 0; i < patchableTraceLoggers_.length(); i++) {
        patchableTraceLoggers_[i].fixup(&masm);
        Assembler::PatchDataWithValueCheck(CodeLocationLabel(code, patchableTraceLoggers_[i]),
                                           ImmPtr(logger),
                                           ImmPtr(nullptr));
    }

    if (patchableTLScripts_.length() > 0) {
        MOZ_ASSERT(TraceLogTextIdEnabled(TraceLogger_Scripts));
        TraceLoggerEvent event(logger, TraceLogger_Scripts, script);
        ionScript->setTraceLoggerEvent(event);
        uint32_t textId = event.payload()->textId();
        for (uint32_t i = 0; i < patchableTLScripts_.length(); i++) {
            patchableTLScripts_[i].fixup(&masm);
            Assembler::PatchDataWithValueCheck(CodeLocationLabel(code, patchableTLScripts_[i]),
                                               ImmPtr((void*) uintptr_t(textId)),
                                               ImmPtr((void*)0));
        }
    }
#endif

    // Replace dummy JSObject pointers embedded by LNurseryObject.
    code->fixupNurseryObjects(cx, gen->nurseryObjects());

    // The correct state for prebarriers is unknown until the end of compilation,
    // since a GC can occur during code generation. All barriers are emitted
    // off-by-default, and are toggled on here if necessary.
    if (cx->zone()->needsIncrementalBarrier())
        ionScript->toggleBarriers(true);

    // Attach any generated script counts to the script.
    if (IonScriptCounts* counts = extractScriptCounts())
        script->addIonCounts(counts);

    // Make sure that AutoDiscardIonCode does not free the relevant info.
    discardIonCode.keepIonCode();

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

    void accept(CodeGenerator* codegen) {
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
    if (lir->type() == MIRType_Float32)
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

typedef bool (*GetPropertyFn)(JSContext*, HandleValue, HandlePropertyName, MutableHandleValue);
static const VMFunction GetPropertyInfo = FunctionInfo<GetPropertyFn>(GetProperty);
static const VMFunction CallPropertyInfo = FunctionInfo<GetPropertyFn>(CallProperty);

void
CodeGenerator::visitCallGetProperty(LCallGetProperty* lir)
{
    pushArg(ImmGCPtr(lir->mir()->name()));
    pushArg(ToValue(lir, LCallGetProperty::Value));

    if (lir->mir()->callprop())
        callVM(CallPropertyInfo, lir);
    else
        callVM(GetPropertyInfo, lir);
}

typedef bool (*GetOrCallElementFn)(JSContext*, MutableHandleValue, HandleValue, MutableHandleValue);
static const VMFunction GetElementInfo = FunctionInfo<GetOrCallElementFn>(js::GetElement);
static const VMFunction CallElementInfo = FunctionInfo<GetOrCallElementFn>(js::CallElement);

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

typedef bool (*SetObjectElementFn)(JSContext*, HandleObject, HandleValue, HandleValue,
                                   bool strict);
static const VMFunction SetObjectElementInfo = FunctionInfo<SetObjectElementFn>(SetObjectElement);

void
CodeGenerator::visitCallSetElement(LCallSetElement* lir)
{
    pushArg(Imm32(lir->mir()->strict()));
    pushArg(ToValue(lir, LCallSetElement::Value));
    pushArg(ToValue(lir, LCallSetElement::Index));
    pushArg(ToRegister(lir->getOperand(0)));
    callVM(SetObjectElementInfo, lir);
}

typedef bool (*InitElementArrayFn)(JSContext*, jsbytecode*, HandleObject, uint32_t, HandleValue);
static const VMFunction InitElementArrayInfo = FunctionInfo<InitElementArrayFn>(js::InitElementArray);

void
CodeGenerator::visitCallInitElementArray(LCallInitElementArray* lir)
{
    pushArg(ToValue(lir, LCallInitElementArray::Value));
    pushArg(Imm32(lir->mir()->index()));
    pushArg(ToRegister(lir->getOperand(0)));
    pushArg(ImmPtr(lir->mir()->resumePoint()->pc()));
    callVM(InitElementArrayInfo, lir);
}

void
CodeGenerator::visitLoadFixedSlotV(LLoadFixedSlotV* ins)
{
    const Register obj = ToRegister(ins->getOperand(0));
    size_t slot = ins->mir()->slot();
    ValueOperand result = GetValueOutput(ins);

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

    if (valueType == MIRType_ObjectOrNull) {
        Register nvalue = ToRegister(value);
        masm.storeObjectOrNull(nvalue, address);
    } else {
        ConstantOrRegister nvalue = value->isConstant()
                                    ? ConstantOrRegister(*value->toConstant())
                                    : TypedOrValueRegister(valueType, ToAnyRegister(value));
        masm.storeConstantOrRegister(nvalue, address);
    }
}

void
CodeGenerator::visitGetNameCache(LGetNameCache* ins)
{
    RegisterSet liveRegs = ins->safepoint()->liveRegs();
    Register scopeChain = ToRegister(ins->scopeObj());
    TypedOrValueRegister output(GetValueOutput(ins));
    bool isTypeOf = ins->mir()->accessKind() != MGetNameCache::NAME;

    NameIC cache(liveRegs, isTypeOf, scopeChain, ins->mir()->name(), output);
    cache.setProfilerLeavePC(ins->mir()->profilerLeavePc());
    addCache(ins, allocateCache(cache));
}

typedef bool (*NameICFn)(JSContext*, HandleScript, size_t, HandleObject, MutableHandleValue);
const VMFunction NameIC::UpdateInfo = FunctionInfo<NameICFn>(NameIC::update);

void
CodeGenerator::visitNameIC(OutOfLineUpdateCache* ool, DataPtr<NameIC>& ic)
{
    LInstruction* lir = ool->lir();
    saveLive(lir);

    pushArg(ic->scopeChainReg());
    pushArg(Imm32(ool->getCacheIndex()));
    pushArg(ImmGCPtr(gen->info().script()));
    callVM(NameIC::UpdateInfo, lir);
    StoreValueTo(ic->outputReg()).generate(this);
    restoreLiveIgnore(lir, StoreValueTo(ic->outputReg()).clobbered());

    masm.jump(ool->rejoin());
}

void
CodeGenerator::addGetPropertyCache(LInstruction* ins, RegisterSet liveRegs, Register objReg,
                                   PropertyName* name, TypedOrValueRegister output,
                                   bool monitoredResult, jsbytecode* profilerLeavePc)
{
    GetPropertyIC cache(liveRegs, objReg, name, output, monitoredResult);
    cache.setProfilerLeavePC(profilerLeavePc);
    addCache(ins, allocateCache(cache));
}

void
CodeGenerator::addSetPropertyCache(LInstruction* ins, RegisterSet liveRegs, Register objReg,
                                   PropertyName* name, ConstantOrRegister value, bool strict,
                                   bool needsTypeBarrier, jsbytecode* profilerLeavePc)
{
    SetPropertyIC cache(liveRegs, objReg, name, value, strict, needsTypeBarrier);
    cache.setProfilerLeavePC(profilerLeavePc);
    addCache(ins, allocateCache(cache));
}

void
CodeGenerator::addSetElementCache(LInstruction* ins, Register obj, Register unboxIndex,
                                  Register temp, FloatRegister tempDouble,
                                  FloatRegister tempFloat32, ValueOperand index,
                                  ConstantOrRegister value, bool strict, bool guardHoles,
                                  jsbytecode* profilerLeavePc)
{
    SetElementIC cache(obj, unboxIndex, temp, tempDouble, tempFloat32, index, value, strict,
                       guardHoles);
    cache.setProfilerLeavePC(profilerLeavePc);
    addCache(ins, allocateCache(cache));
}

void
CodeGenerator::visitGetPropertyCacheV(LGetPropertyCacheV* ins)
{
    RegisterSet liveRegs = ins->safepoint()->liveRegs();
    Register objReg = ToRegister(ins->getOperand(0));
    PropertyName* name = ins->mir()->name();
    bool monitoredResult = ins->mir()->monitoredResult();
    TypedOrValueRegister output = TypedOrValueRegister(GetValueOutput(ins));

    addGetPropertyCache(ins, liveRegs, objReg, name, output, monitoredResult,
                        ins->mir()->profilerLeavePc());
}

void
CodeGenerator::visitGetPropertyCacheT(LGetPropertyCacheT* ins)
{
    RegisterSet liveRegs = ins->safepoint()->liveRegs();
    Register objReg = ToRegister(ins->getOperand(0));
    PropertyName* name = ins->mir()->name();
    bool monitoredResult = ins->mir()->monitoredResult();
    TypedOrValueRegister output(ins->mir()->type(), ToAnyRegister(ins->getDef(0)));

    addGetPropertyCache(ins, liveRegs, objReg, name, output, monitoredResult,
                        ins->mir()->profilerLeavePc());
}

typedef bool (*GetPropertyICFn)(JSContext*, HandleScript, size_t, HandleObject,
                                MutableHandleValue);
const VMFunction GetPropertyIC::UpdateInfo = FunctionInfo<GetPropertyICFn>(GetPropertyIC::update);

void
CodeGenerator::visitGetPropertyIC(OutOfLineUpdateCache* ool, DataPtr<GetPropertyIC>& ic)
{
    LInstruction* lir = ool->lir();

    if (ic->idempotent()) {
        size_t numLocs;
        CacheLocationList& cacheLocs = lir->mirRaw()->toGetPropertyCache()->location();
        size_t locationBase = addCacheLocations(cacheLocs, &numLocs);
        ic->setLocationInfo(locationBase, numLocs);
    }

    saveLive(lir);

    pushArg(ic->object());
    pushArg(Imm32(ool->getCacheIndex()));
    pushArg(ImmGCPtr(gen->info().script()));
    callVM(GetPropertyIC::UpdateInfo, lir);
    StoreValueTo(ic->output()).generate(this);
    restoreLiveIgnore(lir, StoreValueTo(ic->output()).clobbered());

    masm.jump(ool->rejoin());
}

void
CodeGenerator::addGetElementCache(LInstruction* ins, Register obj, ConstantOrRegister index,
                                  TypedOrValueRegister output, bool monitoredResult,
                                  bool allowDoubleResult, jsbytecode* profilerLeavePc)
{
    RegisterSet liveRegs = ins->safepoint()->liveRegs();
    GetElementIC cache(liveRegs, obj, index, output, monitoredResult, allowDoubleResult);
    cache.setProfilerLeavePC(profilerLeavePc);
    addCache(ins, allocateCache(cache));
}

void
CodeGenerator::visitGetElementCacheV(LGetElementCacheV* ins)
{
    Register obj = ToRegister(ins->object());
    ConstantOrRegister index = TypedOrValueRegister(ToValue(ins, LGetElementCacheV::Index));
    TypedOrValueRegister output = TypedOrValueRegister(GetValueOutput(ins));
    const MGetElementCache* mir = ins->mir();

    addGetElementCache(ins, obj, index, output, mir->monitoredResult(),
                       mir->allowDoubleResult(), mir->profilerLeavePc());
}

void
CodeGenerator::visitGetElementCacheT(LGetElementCacheT* ins)
{
    Register obj = ToRegister(ins->object());
    ConstantOrRegister index = TypedOrValueRegister(MIRType_Int32, ToAnyRegister(ins->index()));
    TypedOrValueRegister output(ins->mir()->type(), ToAnyRegister(ins->output()));
    const MGetElementCache* mir = ins->mir();

    addGetElementCache(ins, obj, index, output, mir->monitoredResult(),
                       mir->allowDoubleResult(), mir->profilerLeavePc());
}

typedef bool (*GetElementICFn)(JSContext*, HandleScript, size_t, HandleObject, HandleValue,
                               MutableHandleValue);
const VMFunction GetElementIC::UpdateInfo = FunctionInfo<GetElementICFn>(GetElementIC::update);

void
CodeGenerator::visitGetElementIC(OutOfLineUpdateCache* ool, DataPtr<GetElementIC>& ic)
{
    LInstruction* lir = ool->lir();
    saveLive(lir);

    pushArg(ic->index());
    pushArg(ic->object());
    pushArg(Imm32(ool->getCacheIndex()));
    pushArg(ImmGCPtr(gen->info().script()));
    callVM(GetElementIC::UpdateInfo, lir);
    StoreValueTo(ic->output()).generate(this);
    restoreLiveIgnore(lir, StoreValueTo(ic->output()).clobbered());

    masm.jump(ool->rejoin());
}

void
CodeGenerator::visitSetElementCacheV(LSetElementCacheV* ins)
{
    Register obj = ToRegister(ins->object());
    Register unboxIndex = ToTempUnboxRegister(ins->tempToUnboxIndex());
    Register temp = ToRegister(ins->temp());
    FloatRegister tempDouble = ToFloatRegister(ins->tempDouble());
    FloatRegister tempFloat32 = ToFloatRegister(ins->tempFloat32());
    ValueOperand index = ToValue(ins, LSetElementCacheV::Index);
    ConstantOrRegister value = TypedOrValueRegister(ToValue(ins, LSetElementCacheV::Value));

    addSetElementCache(ins, obj, unboxIndex, temp, tempDouble, tempFloat32, index, value,
                       ins->mir()->strict(), ins->mir()->guardHoles(),
                       ins->mir()->profilerLeavePc());
}

void
CodeGenerator::visitSetElementCacheT(LSetElementCacheT* ins)
{
    Register obj = ToRegister(ins->object());
    Register unboxIndex = ToTempUnboxRegister(ins->tempToUnboxIndex());
    Register temp = ToRegister(ins->temp());
    FloatRegister tempDouble = ToFloatRegister(ins->tempDouble());
    FloatRegister tempFloat32 = ToFloatRegister(ins->tempFloat32());
    ValueOperand index = ToValue(ins, LSetElementCacheT::Index);
    ConstantOrRegister value;
    const LAllocation* tmp = ins->value();
    if (tmp->isConstant())
        value = *tmp->toConstant();
    else
        value = TypedOrValueRegister(ins->mir()->value()->type(), ToAnyRegister(tmp));

    addSetElementCache(ins, obj, unboxIndex, temp, tempDouble, tempFloat32, index, value,
                       ins->mir()->strict(), ins->mir()->guardHoles(),
                       ins->mir()->profilerLeavePc());
}

typedef bool (*SetElementICFn)(JSContext*, HandleScript, size_t, HandleObject, HandleValue,
                               HandleValue);
const VMFunction SetElementIC::UpdateInfo = FunctionInfo<SetElementICFn>(SetElementIC::update);

void
CodeGenerator::visitSetElementIC(OutOfLineUpdateCache* ool, DataPtr<SetElementIC>& ic)
{
    LInstruction* lir = ool->lir();
    saveLive(lir);

    pushArg(ic->value());
    pushArg(ic->index());
    pushArg(ic->object());
    pushArg(Imm32(ool->getCacheIndex()));
    pushArg(ImmGCPtr(gen->info().script()));
    callVM(SetElementIC::UpdateInfo, lir);
    restoreLive(lir);

    masm.jump(ool->rejoin());
}

void
CodeGenerator::visitBindNameCache(LBindNameCache* ins)
{
    Register scopeChain = ToRegister(ins->scopeChain());
    Register output = ToRegister(ins->output());
    BindNameIC cache(scopeChain, ins->mir()->name(), output);
    cache.setProfilerLeavePC(ins->mir()->profilerLeavePc());

    addCache(ins, allocateCache(cache));
}

typedef JSObject* (*BindNameICFn)(JSContext*, HandleScript, size_t, HandleObject);
const VMFunction BindNameIC::UpdateInfo = FunctionInfo<BindNameICFn>(BindNameIC::update);

void
CodeGenerator::visitBindNameIC(OutOfLineUpdateCache* ool, DataPtr<BindNameIC>& ic)
{
    LInstruction* lir = ool->lir();
    saveLive(lir);

    pushArg(ic->scopeChainReg());
    pushArg(Imm32(ool->getCacheIndex()));
    pushArg(ImmGCPtr(gen->info().script()));
    callVM(BindNameIC::UpdateInfo, lir);
    StoreRegisterTo(ic->outputReg()).generate(this);
    restoreLiveIgnore(lir, StoreRegisterTo(ic->outputReg()).clobbered());

    masm.jump(ool->rejoin());
}

typedef bool (*SetPropertyFn)(JSContext*, HandleObject,
                              HandlePropertyName, const HandleValue, bool, jsbytecode*);
static const VMFunction SetPropertyInfo = FunctionInfo<SetPropertyFn>(SetProperty);

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
    FunctionInfo<DeletePropertyFn>(DeleteProperty<true>);
static const VMFunction DeletePropertyNonStrictInfo =
    FunctionInfo<DeletePropertyFn>(DeleteProperty<false>);

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
    FunctionInfo<DeleteElementFn>(DeleteElement<true>);
static const VMFunction DeleteElementNonStrictInfo =
    FunctionInfo<DeleteElementFn>(DeleteElement<false>);

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
CodeGenerator::visitSetPropertyCacheV(LSetPropertyCacheV* ins)
{
    RegisterSet liveRegs = ins->safepoint()->liveRegs();
    Register objReg = ToRegister(ins->getOperand(0));
    ConstantOrRegister value = TypedOrValueRegister(ToValue(ins, LSetPropertyCacheV::Value));

    addSetPropertyCache(ins, liveRegs, objReg, ins->mir()->name(), value,
                        ins->mir()->strict(), ins->mir()->needsTypeBarrier(),
                        ins->mir()->profilerLeavePc());
}

void
CodeGenerator::visitSetPropertyCacheT(LSetPropertyCacheT* ins)
{
    RegisterSet liveRegs = ins->safepoint()->liveRegs();
    Register objReg = ToRegister(ins->getOperand(0));
    ConstantOrRegister value;

    if (ins->getOperand(1)->isConstant())
        value = ConstantOrRegister(*ins->getOperand(1)->toConstant());
    else
        value = TypedOrValueRegister(ins->valueType(), ToAnyRegister(ins->getOperand(1)));

    addSetPropertyCache(ins, liveRegs, objReg, ins->mir()->name(), value,
                        ins->mir()->strict(), ins->mir()->needsTypeBarrier(),
                        ins->mir()->profilerLeavePc());
}

typedef bool (*SetPropertyICFn)(JSContext*, HandleScript, size_t, HandleObject, HandleValue);
const VMFunction SetPropertyIC::UpdateInfo = FunctionInfo<SetPropertyICFn>(SetPropertyIC::update);

void
CodeGenerator::visitSetPropertyIC(OutOfLineUpdateCache* ool, DataPtr<SetPropertyIC>& ic)
{
    LInstruction* lir = ool->lir();
    saveLive(lir);

    pushArg(ic->value());
    pushArg(ic->object());
    pushArg(Imm32(ool->getCacheIndex()));
    pushArg(ImmGCPtr(gen->info().script()));
    callVM(SetPropertyIC::UpdateInfo, lir);
    restoreLive(lir);

    masm.jump(ool->rejoin());
}

typedef bool (*ThrowFn)(JSContext*, HandleValue);
static const VMFunction ThrowInfoCodeGen = FunctionInfo<ThrowFn>(js::Throw);

void
CodeGenerator::visitThrow(LThrow* lir)
{
    pushArg(ToValue(lir, LThrow::Value));
    callVM(ThrowInfoCodeGen, lir);
}

typedef bool (*BitNotFn)(JSContext*, HandleValue, int* p);
static const VMFunction BitNotInfo = FunctionInfo<BitNotFn>(BitNot);

void
CodeGenerator::visitBitNotV(LBitNotV* lir)
{
    pushArg(ToValue(lir, LBitNotV::Input));
    callVM(BitNotInfo, lir);
}

typedef bool (*BitopFn)(JSContext*, HandleValue, HandleValue, int* p);
static const VMFunction BitAndInfo = FunctionInfo<BitopFn>(BitAnd);
static const VMFunction BitOrInfo = FunctionInfo<BitopFn>(BitOr);
static const VMFunction BitXorInfo = FunctionInfo<BitopFn>(BitXor);
static const VMFunction BitLhsInfo = FunctionInfo<BitopFn>(BitLsh);
static const VMFunction BitRhsInfo = FunctionInfo<BitopFn>(BitRsh);

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

    void accept(CodeGenerator* codegen) {
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
    Register tag = masm.splitTagForTest(value);

    const JSAtomState& names = GetJitContext()->runtime->names();
    Label done;

    MDefinition* input = lir->mir()->input();

    bool testObject = input->mightBeType(MIRType_Object);
    bool testNumber = input->mightBeType(MIRType_Int32) || input->mightBeType(MIRType_Double);
    bool testBoolean = input->mightBeType(MIRType_Boolean);
    bool testUndefined = input->mightBeType(MIRType_Undefined);
    bool testNull = input->mightBeType(MIRType_Null);
    bool testString = input->mightBeType(MIRType_String);
    bool testSymbol = input->mightBeType(MIRType_Symbol);

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

    ValueOperand input = ToValue(ins, LTypeOfV::Input);
    Register temp = ToTempUnboxRegister(ins->tempToUnbox());
    Register output = ToRegister(ins->output());

    Register obj = masm.extractObject(input, temp);

    saveVolatile(output);
    masm.setupUnalignedABICall(2, output);
    masm.passABIArg(obj);
    masm.movePtr(ImmPtr(GetJitContext()->runtime), output);
    masm.passABIArg(output);
    masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, js::TypeOfObjectOperation));
    masm.storeCallResult(output);
    restoreVolatile(output);

    masm.jump(ool->rejoin());
}

typedef bool (*ToIdFn)(JSContext*, HandleScript, jsbytecode*, HandleValue, HandleValue,
                       MutableHandleValue);
static const VMFunction ToIdInfo = FunctionInfo<ToIdFn>(ToIdOperation);

void
CodeGenerator::visitToIdV(LToIdV* lir)
{
    Label notInt32;
    FloatRegister temp = ToFloatRegister(lir->tempFloat());
    const ValueOperand out = ToOutValue(lir);
    ValueOperand index = ToValue(lir, LToIdV::Index);

    OutOfLineCode* ool = oolCallVM(ToIdInfo, lir,
                                   (ArgList(),
                                   ImmGCPtr(current->mir()->info().script()),
                                   ImmPtr(lir->mir()->resumePoint()->pc()),
                                   ToValue(lir, LToIdV::Object),
                                   ToValue(lir, LToIdV::Index)),
                                   StoreValueTo(out));

    Register tag = masm.splitTagForTest(index);

    masm.branchTestInt32(Assembler::NotEqual, tag, &notInt32);
    masm.moveValue(index, out);
    masm.jump(ool->rejoin());

    masm.bind(&notInt32);
    masm.branchTestDouble(Assembler::NotEqual, tag, ool->entry());
    masm.unboxDouble(index, temp);
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
    Register initLength = ToRegister(lir->initLength());
    const ValueOperand out = ToOutValue(lir);

    const MLoadElementHole* mir = lir->mir();

    // If the index is out of bounds, load |undefined|. Otherwise, load the
    // value.
    Label undefined, done;
    if (lir->index()->isConstant()) {
        masm.branch32(Assembler::BelowOrEqual, initLength, Imm32(ToInt32(lir->index())), &undefined);
        NativeObject::elementsSizeMustNotOverflow();
        masm.loadValue(Address(elements, ToInt32(lir->index()) * sizeof(Value)), out);
    } else {
        masm.branch32(Assembler::BelowOrEqual, initLength, ToRegister(lir->index()), &undefined);
        masm.loadValue(BaseObjectElementIndex(elements, ToRegister(lir->index())), out);
    }

    // If a hole check is needed, and the value wasn't a hole, we're done.
    // Otherwise, we'll load undefined.
    if (lir->mir()->needsHoleCheck())
        masm.branchTestMagic(Assembler::NotEqual, out, &done);
    else
        masm.jump(&done);

    masm.bind(&undefined);

    if (mir->needsNegativeIntCheck()) {
        if (lir->index()->isConstant()) {
            if (ToInt32(lir->index()) < 0)
                bailout(lir->snapshot());
        } else {
            Label negative;
            masm.branch32(Assembler::LessThan, ToRegister(lir->index()), Imm32(0), &negative);
            bailoutFrom(&negative, lir->snapshot());
        }
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
CodeGenerator::visitLoadTypedArrayElement(LLoadTypedArrayElement* lir)
{
    Register elements = ToRegister(lir->elements());
    Register temp = lir->temp()->isBogusTemp() ? InvalidReg : ToRegister(lir->temp());
    AnyRegister out = ToAnyRegister(lir->output());

    Scalar::Type arrayType = lir->mir()->arrayType();
    int width = Scalar::byteSize(arrayType);

    Label fail;
    if (lir->index()->isConstant()) {
        Address source(elements, ToInt32(lir->index()) * width + lir->mir()->offsetAdjustment());
        masm.loadFromTypedArray(arrayType, source, out, temp, &fail,
                                lir->mir()->canonicalizeDoubles());
    } else {
        BaseIndex source(elements, ToRegister(lir->index()), ScaleFromElemWidth(width),
                         lir->mir()->offsetAdjustment());
        masm.loadFromTypedArray(arrayType, source, out, temp, &fail,
                                lir->mir()->canonicalizeDoubles());
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
    Int32Key key = ToInt32Key(lir->index());
    masm.unboxInt32(Address(object, TypedArrayLayout::lengthOffset()), scratch);

    // Load undefined unless length > key.
    Label inbounds, done;
    masm.branchKey(Assembler::Above, scratch, key, &inbounds);
    masm.moveValue(UndefinedValue(), out);
    masm.jump(&done);

    // Load the elements vector.
    masm.bind(&inbounds);
    masm.loadPtr(Address(object, TypedArrayLayout::dataOffset()), scratch);

    Scalar::Type arrayType = lir->mir()->arrayType();
    int width = Scalar::byteSize(arrayType);

    Label fail;
    if (key.isConstant()) {
        Address source(scratch, key.constant() * width);
        masm.loadFromTypedArray(arrayType, source, out, lir->mir()->allowDouble(),
                                out.scratchReg(), &fail);
    } else {
        BaseIndex source(scratch, key.reg(), ScaleFromElemWidth(width));
        masm.loadFromTypedArray(arrayType, source, out, lir->mir()->allowDouble(),
                                out.scratchReg(), &fail);
    }

    if (fail.used())
        bailoutFrom(&fail, lir->snapshot());

    masm.bind(&done);
}

template <typename T>
static inline void
StoreToTypedArray(MacroAssembler& masm, Scalar::Type arrayType, const LAllocation* value, const T& dest)
{
    if (arrayType == Scalar::Float32 || arrayType == Scalar::Float64) {
        masm.storeToTypedFloatArray(arrayType, ToFloatRegister(value), dest);
    } else {
        if (value->isConstant())
            masm.storeToTypedIntArray(arrayType, Imm32(ToInt32(value)), dest);
        else
            masm.storeToTypedIntArray(arrayType, ToRegister(value), dest);
    }
}

void
CodeGenerator::visitStoreTypedArrayElement(LStoreTypedArrayElement* lir)
{
    Register elements = ToRegister(lir->elements());
    const LAllocation* value = lir->value();

    Scalar::Type arrayType = lir->mir()->arrayType();
    int width = Scalar::byteSize(arrayType);

    if (lir->index()->isConstant()) {
        Address dest(elements, ToInt32(lir->index()) * width + lir->mir()->offsetAdjustment());
        StoreToTypedArray(masm, arrayType, value, dest);
    } else {
        BaseIndex dest(elements, ToRegister(lir->index()), ScaleFromElemWidth(width),
                       lir->mir()->offsetAdjustment());
        StoreToTypedArray(masm, arrayType, value, dest);
    }
}

void
CodeGenerator::visitStoreTypedArrayElementHole(LStoreTypedArrayElementHole* lir)
{
    Register elements = ToRegister(lir->elements());
    const LAllocation* value = lir->value();

    Scalar::Type arrayType = lir->mir()->arrayType();
    int width = Scalar::byteSize(arrayType);

    bool guardLength = true;
    if (lir->index()->isConstant() && lir->length()->isConstant()) {
        uint32_t idx = ToInt32(lir->index());
        uint32_t len = ToInt32(lir->length());
        if (idx >= len)
            return;
        guardLength = false;
    }
    Label skip;
    if (lir->index()->isConstant()) {
        uint32_t idx = ToInt32(lir->index());
        if (guardLength)
            masm.branch32(Assembler::BelowOrEqual, ToOperand(lir->length()), Imm32(idx), &skip);
        Address dest(elements, idx * width);
        StoreToTypedArray(masm, arrayType, value, dest);
    } else {
        Register idxReg = ToRegister(lir->index());
        MOZ_ASSERT(guardLength);
        if (lir->length()->isConstant())
            masm.branch32(Assembler::AboveOrEqual, idxReg, Imm32(ToInt32(lir->length())), &skip);
        else
            masm.branch32(Assembler::BelowOrEqual, ToOperand(lir->length()), idxReg, &skip);
        BaseIndex dest(elements, ToRegister(lir->index()), ScaleFromElemWidth(width));
        StoreToTypedArray(masm, arrayType, value, dest);
    }
    if (guardLength)
        masm.bind(&skip);
}

void
CodeGenerator::visitCompareExchangeTypedArrayElement(LCompareExchangeTypedArrayElement* lir)
{
    Register elements = ToRegister(lir->elements());
    AnyRegister output = ToAnyRegister(lir->output());
    Register temp = lir->temp()->isBogusTemp() ? InvalidReg : ToRegister(lir->temp());

    MOZ_ASSERT(lir->oldval()->isRegister());
    MOZ_ASSERT(lir->newval()->isRegister());

    Register oldval = ToRegister(lir->oldval());
    Register newval = ToRegister(lir->newval());

    Scalar::Type arrayType = lir->mir()->arrayType();
    int width = Scalar::byteSize(arrayType);

    if (lir->index()->isConstant()) {
        Address dest(elements, ToInt32(lir->index()) * width);
        masm.compareExchangeToTypedIntArray(arrayType, dest, oldval, newval, temp, output);
    } else {
        BaseIndex dest(elements, ToRegister(lir->index()), ScaleFromElemWidth(width));
        masm.compareExchangeToTypedIntArray(arrayType, dest, oldval, newval, temp, output);
    }
}

template <typename T>
static inline void
AtomicBinopToTypedArray(MacroAssembler& masm, AtomicOp op,
                        Scalar::Type arrayType, const LAllocation* value, const T& mem,
                        Register temp1, Register temp2, AnyRegister output)
{
    if (value->isConstant())
        masm.atomicBinopToTypedIntArray(op, arrayType, Imm32(ToInt32(value)), mem, temp1, temp2, output);
    else
        masm.atomicBinopToTypedIntArray(op, arrayType, ToRegister(value), mem, temp1, temp2, output);
}

void
CodeGenerator::visitAtomicTypedArrayElementBinop(LAtomicTypedArrayElementBinop* lir)
{
    AnyRegister output = ToAnyRegister(lir->output());
    Register elements = ToRegister(lir->elements());
    Register temp1 = lir->temp1()->isBogusTemp() ? InvalidReg : ToRegister(lir->temp1());
    Register temp2 = lir->temp2()->isBogusTemp() ? InvalidReg : ToRegister(lir->temp2());
    const LAllocation* value = lir->value();

    Scalar::Type arrayType = lir->mir()->arrayType();
    int width = Scalar::byteSize(arrayType);

    if (lir->index()->isConstant()) {
        Address mem(elements, ToInt32(lir->index()) * width);
        AtomicBinopToTypedArray(masm, lir->mir()->operation(), arrayType, value, mem, temp1, temp2, output);
    } else {
        BaseIndex mem(elements, ToRegister(lir->index()), ScaleFromElemWidth(width));
        AtomicBinopToTypedArray(masm, lir->mir()->operation(), arrayType, value, mem, temp1, temp2, output);
    }
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

    Label* stringEntry, *stringRejoin;
    if (input->mightBeType(MIRType_String)) {
        OutOfLineCode* oolString = oolCallVM(StringToNumberInfo, lir, (ArgList(), output),
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

typedef bool (*OperatorInFn)(JSContext*, HandleValue, HandleObject, bool*);
static const VMFunction OperatorInInfo = FunctionInfo<OperatorInFn>(OperatorIn);

void
CodeGenerator::visitIn(LIn* ins)
{
    pushArg(ToRegister(ins->rhs()));
    pushArg(ToValue(ins, LIn::LHS));

    callVM(OperatorInInfo, ins);
}

typedef bool (*OperatorInIFn)(JSContext*, uint32_t, HandleObject, bool*);
static const VMFunction OperatorInIInfo = FunctionInfo<OperatorInIFn>(OperatorInI);

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
                            (ArgList(), Imm32(index), ToRegister(lir->object())),
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
                            (ArgList(), index, ToRegister(lir->object())),
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
static const VMFunction IsDelegateObjectInfo = FunctionInfo<IsDelegateObjectFn>(IsDelegateObject);

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
                                   (ArgList(), ImmGCPtr(prototypeObject), objReg),
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
static const VMFunction HasInstanceInfo = FunctionInfo<HasInstanceFn>(js::HasInstance);

void
CodeGenerator::visitCallInstanceOf(LCallInstanceOf* ins)
{
    ValueOperand lhs = ToValue(ins, LCallInstanceOf::LHS);
    Register rhs = ToRegister(ins->rhs());
    MOZ_ASSERT(ToRegister(ins->output()) == ReturnReg);

    pushArg(lhs);
    pushArg(rhs);
    callVM(HasInstanceInfo, ins);
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
    masm.movePtr(StackPointer, ValueReg);

    masm.Push(ObjectReg);

    LoadDOMPrivate(masm, ObjectReg, PrivateReg);

    // Rooting will happen at GC time.
    masm.movePtr(StackPointer, ObjectReg);

    uint32_t safepointOffset;
    masm.buildFakeExitFrame(JSContextReg, &safepointOffset);
    masm.enterFakeExitFrame(IonDOMExitFrameLayout::GetterToken());

    markSafepointAt(safepointOffset, ins);

    masm.setupUnalignedABICall(4, JSContextReg);

    masm.loadJSContext(JSContextReg);

    masm.passABIArg(JSContextReg);
    masm.passABIArg(ObjectReg);
    masm.passABIArg(PrivateReg);
    masm.passABIArg(ValueReg);
    masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, ins->mir()->fun()));

    if (ins->mir()->isInfallible()) {
        masm.loadValue(Address(StackPointer, IonDOMExitFrameLayout::offsetOfResult()),
                       JSReturnOperand);
    } else {
        masm.branchIfFalseBool(ReturnReg, masm.exceptionLabel());

        masm.loadValue(Address(StackPointer, IonDOMExitFrameLayout::offsetOfResult()),
                       JSReturnOperand);
    }
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
    Register object = ToRegister(ins->object());
    size_t slot = ins->mir()->domMemberSlotIndex();
    ValueOperand result = GetValueOutput(ins);

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
    masm.movePtr(StackPointer, ValueReg);

    masm.Push(ObjectReg);

    LoadDOMPrivate(masm, ObjectReg, PrivateReg);

    // Rooting will happen at GC time.
    masm.movePtr(StackPointer, ObjectReg);

    uint32_t safepointOffset;
    masm.buildFakeExitFrame(JSContextReg, &safepointOffset);
    masm.enterFakeExitFrame(IonDOMExitFrameLayout::SetterToken());

    markSafepointAt(safepointOffset, ins);

    masm.setupUnalignedABICall(4, JSContextReg);

    masm.loadJSContext(JSContextReg);

    masm.passABIArg(JSContextReg);
    masm.passABIArg(ObjectReg);
    masm.passABIArg(PrivateReg);
    masm.passABIArg(ValueReg);
    masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, ins->mir()->fun()));

    masm.branchIfFalseBool(ReturnReg, masm.exceptionLabel());

    masm.adjustStack(IonDOMExitFrameLayout::Size());

    MOZ_ASSERT(masm.framePushed() == initialStack);
}

class OutOfLineIsCallable : public OutOfLineCodeBase<CodeGenerator>
{
    LIsCallable* ins_;

  public:
    explicit OutOfLineIsCallable(LIsCallable* ins)
      : ins_(ins)
    { }

    void accept(CodeGenerator* codegen) {
        codegen->visitOutOfLineIsCallable(this);
    }
    LIsCallable* ins() const {
        return ins_;
    }
};

void
CodeGenerator::visitIsCallable(LIsCallable* ins)
{
    Register object = ToRegister(ins->object());
    Register output = ToRegister(ins->output());

    OutOfLineIsCallable* ool = new(alloc()) OutOfLineIsCallable(ins);
    addOutOfLineCode(ool, ins->mir());

    Label notFunction, done;
    masm.loadObjClass(object, output);

    // Just skim proxies off. Their notion of isCallable() is more complicated.
    masm.branchTestClassIsProxy(true, output, ool->entry());

    // An object is callable iff (is<JSFunction>() || getClass()->call.
    masm.branchPtr(Assembler::NotEqual, output, ImmPtr(&JSFunction::class_), &notFunction);
    masm.move32(Imm32(1), output);
    masm.jump(&done);

    masm.bind(&notFunction);
    masm.cmpPtrSet(Assembler::NonZero, Address(output, offsetof(js::Class, call)), ImmPtr(nullptr), output);
    masm.bind(&done);
    masm.bind(ool->rejoin());
}

void
CodeGenerator::visitOutOfLineIsCallable(OutOfLineIsCallable* ool)
{
    LIsCallable* ins = ool->ins();
    Register object = ToRegister(ins->object());
    Register output = ToRegister(ins->output());

    saveVolatile(output);
    masm.setupUnalignedABICall(1, output);
    masm.passABIArg(object);
    masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, ObjectIsCallable));
    masm.storeCallResult(output);
    // C++ compilers like to only use the bottom byte for bools, but we need to maintain the entire
    // register.
    masm.and32(Imm32(0xFF), output);
    restoreVolatile(output);
    masm.jump(ool->rejoin());
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

    masm.loadObjClass(lhs, output);
    masm.cmpPtrSet(Assembler::Equal, output, ImmPtr(ins->mir()->getClass()), output);
}

void
CodeGenerator::visitAsmJSParameter(LAsmJSParameter* lir)
{
}

void
CodeGenerator::visitAsmJSReturn(LAsmJSReturn* lir)
{
    // Don't emit a jump to the return label if this is the last block.
    if (current->mir() != *gen->graph().poBegin())
        masm.jump(&returnLabel_);
}

void
CodeGenerator::visitAsmJSVoidReturn(LAsmJSVoidReturn* lir)
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
    FloatRegister dest = input;
    if (hasMultiAlias())
        dest = ToFloatRegister(ins->armtemp());

    const Range* r = ins->range();

    masm.convertFloat32ToDouble(input, dest);
    emitAssertRangeD(r, dest, temp);
    if (dest == input)
        masm.convertDoubleToFloat32(input, input);
}

void
CodeGenerator::visitAssertRangeV(LAssertRangeV* ins)
{
    const Range* r = ins->range();
    const ValueOperand value = ToValue(ins, LAssertRangeV::Input);
    Register tag = masm.splitTagForTest(value);
    Label done;

    {
        Label isNotInt32;
        masm.branchTestInt32(Assembler::NotEqual, tag, &isNotInt32);
        Register unboxInt32 = ToTempUnboxRegister(ins->temp());
        Register input = masm.extractInt32(value, unboxInt32);
        emitAssertRangeI(r, input);
        masm.jump(&done);
        masm.bind(&isNotInt32);
    }

    {
        Label isNotDouble;
        masm.branchTestDouble(Assembler::NotEqual, tag, &isNotDouble);
        FloatRegister input = ToFloatRegister(ins->floatTemp1());
        FloatRegister temp = ToFloatRegister(ins->floatTemp2());
        masm.unboxDouble(value, input);
        emitAssertRangeD(r, input, temp);
        masm.jump(&done);
        masm.bind(&isNotDouble);
    }

    masm.assumeUnreachable("Incorrect range for Value.");
    masm.bind(&done);
}

void
CodeGenerator::visitInterruptCheck(LInterruptCheck* lir)
{
    OutOfLineCode* ool = oolCallVM(InterruptCheckInfo, lir, (ArgList()), StoreNothing());

    AbsoluteAddress interruptAddr(GetJitContext()->runtime->addressOfInterruptUint32());
    masm.branch32(Assembler::NotEqual, interruptAddr, Imm32(0), ool->entry());
    masm.bind(ool->rejoin());
}

void
CodeGenerator::visitAsmJSInterruptCheck(LAsmJSInterruptCheck* lir)
{
    Label rejoin;
    masm.branch32(Assembler::Equal, AsmJSAbsoluteAddress(AsmJSImm_RuntimeInterruptUint32),
                  Imm32(0), &rejoin);
    {
        uint32_t stackFixup = ComputeByteAlignment(masm.framePushed() + sizeof(AsmJSFrame),
                                                   ABIStackAlignment);
        masm.reserveStack(stackFixup);
        masm.call(lir->funcDesc(), lir->interruptExit());
        masm.freeStack(stackFixup);
    }
    masm.bind(&rejoin);
}

typedef bool (*RecompileFn)(JSContext*);
static const VMFunction RecompileFnInfo = FunctionInfo<RecompileFn>(Recompile);

typedef bool (*ForcedRecompileFn)(JSContext*);
static const VMFunction ForcedRecompileFnInfo = FunctionInfo<ForcedRecompileFn>(ForcedRecompile);

void
CodeGenerator::visitRecompileCheck(LRecompileCheck* ins)
{
    Label done;
    Register tmp = ToRegister(ins->scratch());
    OutOfLineCode* ool;
    if (ins->mir()->forceRecompilation())
        ool = oolCallVM(ForcedRecompileFnInfo, ins, (ArgList()), StoreRegisterTo(tmp));
    else
        ool = oolCallVM(RecompileFnInfo, ins, (ArgList()), StoreRegisterTo(tmp));

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
    CodeOffsetLabel label = masm.movWithPatch(ImmWord(uintptr_t(-1)), tmp);
    masm.propagateOOM(ionScriptLabels_.append(label));
    masm.branch32(Assembler::Equal,
                  Address(tmp, IonScript::offsetOfRecompiling()),
                  Imm32(0),
                  ool->entry());
    masm.bind(ool->rejoin());
    masm.bind(&done);
}

typedef bool (*ThrowUninitializedLexicalFn)(JSContext*);
static const VMFunction ThrowUninitializedLexicalInfo =
    FunctionInfo<ThrowUninitializedLexicalFn>(ThrowUninitializedLexical);

void
CodeGenerator::visitLexicalCheck(LLexicalCheck* ins)
{
    OutOfLineCode* ool = oolCallVM(ThrowUninitializedLexicalInfo, ins, (ArgList()),
                                   StoreNothing());
    ValueOperand inputValue = ToValue(ins, LLexicalCheck::Input);
    masm.branchTestMagicValue(Assembler::Equal, inputValue, JS_UNINITIALIZED_LEXICAL,
                              ool->entry());
    masm.bind(ool->rejoin());
}

void
CodeGenerator::visitThrowUninitializedLexical(LThrowUninitializedLexical* ins)
{
    callVM(ThrowUninitializedLexicalInfo, ins);
}

void
CodeGenerator::visitDebugger(LDebugger* ins)
{
    Register cx = ToRegister(ins->getTemp(0));
    Register temp = ToRegister(ins->getTemp(1));

    masm.loadJSContext(cx);
    masm.setupUnalignedABICall(1, temp);
    masm.passABIArg(cx);
    masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, GlobalHasLiveOnDebuggerStatement));

    Label bail;
    masm.branchIfTrueBool(ReturnReg, &bail);
    bailoutFrom(&bail, ins->snapshot());
}

} // namespace jit
} // namespace js

/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_none_MacroAssembler_none_h
#define jit_none_MacroAssembler_none_h

#include "jit/JitCompartment.h"
#include "jit/MoveResolver.h"
#include "jit/shared/Assembler-shared.h"


namespace js {
namespace jit {

class MDefinition;

static MOZ_CONSTEXPR_VAR Register StackPointer = { Registers::invalid_reg };
static MOZ_CONSTEXPR_VAR Register FramePointer = { Registers::invalid_reg };
static MOZ_CONSTEXPR_VAR Register ReturnReg = { Registers::invalid_reg };
static MOZ_CONSTEXPR_VAR FloatRegister ReturnFloat32Reg = { FloatRegisters::invalid_reg };
static MOZ_CONSTEXPR_VAR FloatRegister ReturnDoubleReg = { FloatRegisters::invalid_reg };
static MOZ_CONSTEXPR_VAR FloatRegister ReturnSimdReg = { FloatRegisters::invalid_reg };
static MOZ_CONSTEXPR_VAR FloatRegister ScratchFloat32Reg = { FloatRegisters::invalid_reg };
static MOZ_CONSTEXPR_VAR FloatRegister ScratchDoubleReg = { FloatRegisters::invalid_reg };
static MOZ_CONSTEXPR_VAR FloatRegister ScratchSimdReg = { FloatRegisters::invalid_reg };
static MOZ_CONSTEXPR_VAR FloatRegister InvalidFloatReg = { FloatRegisters::invalid_reg };

static MOZ_CONSTEXPR_VAR Register OsrFrameReg = { Registers::invalid_reg };
static MOZ_CONSTEXPR_VAR Register ArgumentsRectifierReg = { Registers::invalid_reg };
static MOZ_CONSTEXPR_VAR Register PreBarrierReg = { Registers::invalid_reg };
static MOZ_CONSTEXPR_VAR Register CallTempReg0 = { Registers::invalid_reg };
static MOZ_CONSTEXPR_VAR Register CallTempReg1 = { Registers::invalid_reg };
static MOZ_CONSTEXPR_VAR Register CallTempReg2 = { Registers::invalid_reg };
static MOZ_CONSTEXPR_VAR Register CallTempReg3 = { Registers::invalid_reg };
static MOZ_CONSTEXPR_VAR Register CallTempReg4 = { Registers::invalid_reg };
static MOZ_CONSTEXPR_VAR Register CallTempReg5 = { Registers::invalid_reg };
static MOZ_CONSTEXPR_VAR Register InvalidReg = { Registers::invalid_reg };

static MOZ_CONSTEXPR_VAR Register IntArgReg0 = { Registers::invalid_reg };
static MOZ_CONSTEXPR_VAR Register IntArgReg1 = { Registers::invalid_reg };
static MOZ_CONSTEXPR_VAR Register IntArgReg2 = { Registers::invalid_reg };
static MOZ_CONSTEXPR_VAR Register IntArgReg3 = { Registers::invalid_reg };
static MOZ_CONSTEXPR_VAR Register GlobalReg = { Registers::invalid_reg };
static MOZ_CONSTEXPR_VAR Register HeapReg = { Registers::invalid_reg };

static MOZ_CONSTEXPR_VAR Register AsmJSIonExitRegCallee = { Registers::invalid_reg };
static MOZ_CONSTEXPR_VAR Register AsmJSIonExitRegE0 = { Registers::invalid_reg };
static MOZ_CONSTEXPR_VAR Register AsmJSIonExitRegE1 = { Registers::invalid_reg };
static MOZ_CONSTEXPR_VAR Register AsmJSIonExitRegE2 = { Registers::invalid_reg };
static MOZ_CONSTEXPR_VAR Register AsmJSIonExitRegE3 = { Registers::invalid_reg };

static MOZ_CONSTEXPR_VAR Register AsmJSIonExitRegReturnData = { Registers::invalid_reg };
static MOZ_CONSTEXPR_VAR Register AsmJSIonExitRegReturnType = { Registers::invalid_reg };
static MOZ_CONSTEXPR_VAR Register AsmJSIonExitRegD0 = { Registers::invalid_reg };
static MOZ_CONSTEXPR_VAR Register AsmJSIonExitRegD1 = { Registers::invalid_reg };
static MOZ_CONSTEXPR_VAR Register AsmJSIonExitRegD2 = { Registers::invalid_reg };

static MOZ_CONSTEXPR_VAR Register JSReturnReg_Type = { Registers::invalid_reg };
static MOZ_CONSTEXPR_VAR Register JSReturnReg_Data = { Registers::invalid_reg };
static MOZ_CONSTEXPR_VAR Register JSReturnReg = { Registers::invalid_reg };

#if defined(JS_NUNBOX32)
static MOZ_CONSTEXPR_VAR ValueOperand JSReturnOperand(InvalidReg, InvalidReg);
#elif defined(JS_PUNBOX64)
static MOZ_CONSTEXPR_VAR ValueOperand JSReturnOperand(InvalidReg);
#else
#error "Bad architecture"
#endif

static const uint32_t ABIStackAlignment = 4;
static const uint32_t CodeAlignment = 4;
static const uint32_t JitStackAlignment = 4;

static const Scale ScalePointer = TimesOne;

class Assembler : public AssemblerShared
{
  public:
    enum Condition {
        Equal,
        NotEqual,
        Above,
        AboveOrEqual,
        Below,
        BelowOrEqual,
        GreaterThan,
        GreaterThanOrEqual,
        LessThan,
        LessThanOrEqual,
        Overflow,
        Signed,
        NotSigned,
        Zero,
        NonZero,
        Always,
    };

    enum DoubleCondition {
        DoubleOrdered,
        DoubleEqual,
        DoubleNotEqual,
        DoubleGreaterThan,
        DoubleGreaterThanOrEqual,
        DoubleLessThan,
        DoubleLessThanOrEqual,
        DoubleUnordered,
        DoubleEqualOrUnordered,
        DoubleNotEqualOrUnordered,
        DoubleGreaterThanOrUnordered,
        DoubleGreaterThanOrEqualOrUnordered,
        DoubleLessThanOrUnordered,
        DoubleLessThanOrEqualOrUnordered
    };

    static Condition InvertCondition(Condition) { MOZ_CRASH(); }

    template <typename T, typename S>
    static void PatchDataWithValueCheck(CodeLocationLabel, T, S) { MOZ_CRASH(); }
    static void PatchWrite_Imm32(CodeLocationLabel, Imm32) { MOZ_CRASH(); }

    static void PatchWrite_NearCall(CodeLocationLabel, CodeLocationLabel) { MOZ_CRASH(); }
    static uint32_t PatchWrite_NearCallSize() { MOZ_CRASH(); }
    static void PatchInstructionImmediate(uint8_t*, PatchedImmPtr) { MOZ_CRASH(); }
    static int32_t ExtractCodeLabelOffset(uint8_t*) { MOZ_CRASH(); }

    static void ToggleToJmp(CodeLocationLabel) { MOZ_CRASH(); }
    static void ToggleToCmp(CodeLocationLabel) { MOZ_CRASH(); }
    static void ToggleCall(CodeLocationLabel, bool) { MOZ_CRASH(); }

    static uintptr_t GetPointer(uint8_t*) { MOZ_CRASH(); }
};

class Operand
{
  public:
    Operand (const Address&) { MOZ_CRASH();}

};

class MacroAssemblerNone : public Assembler
{
  public:
    MacroAssemblerNone() { MOZ_CRASH(); }

    MoveResolver moveResolver_;
    size_t framePushed_;

    uint32_t framePushed() const { MOZ_CRASH(); }
    void setFramePushed(uint32_t) { MOZ_CRASH(); }

    size_t size() const { MOZ_CRASH(); }
    size_t bytesNeeded() const { MOZ_CRASH(); }
    size_t jumpRelocationTableBytes() const { MOZ_CRASH(); }
    size_t dataRelocationTableBytes() const { MOZ_CRASH(); }
    size_t preBarrierTableBytes() const { MOZ_CRASH(); }

    size_t numCodeLabels() const { MOZ_CRASH(); }
    CodeLabel codeLabel(size_t) { MOZ_CRASH(); }

    void trace(JSTracer*) { MOZ_CRASH(); }
    static void TraceJumpRelocations(JSTracer*, JitCode*, CompactBufferReader&) { MOZ_CRASH(); }
    static void TraceDataRelocations(JSTracer*, JitCode*, CompactBufferReader&) { MOZ_CRASH(); }

    static void FixupNurseryObjects(JSContext*, JitCode*, CompactBufferReader&,
                                    const ObjectVector&)
    {
        MOZ_CRASH();
    }

    static bool SupportsFloatingPoint() { return false; }
    static bool SupportsSimd() { return false; }

    void executableCopy(void*) { MOZ_CRASH(); }
    void copyJumpRelocationTable(uint8_t*) { MOZ_CRASH(); }
    void copyDataRelocationTable(uint8_t*) { MOZ_CRASH(); }
    void copyPreBarrierTable(uint8_t*) { MOZ_CRASH(); }
    void processCodeLabels(uint8_t*) { MOZ_CRASH(); }

    void flushBuffer() { MOZ_CRASH(); }

    template <typename T> void bind(T) { MOZ_CRASH(); }
    template <typename T> void j(Condition, T) { MOZ_CRASH(); }
    template <typename T> void jump(T) { MOZ_CRASH(); }
    void align(size_t) { MOZ_CRASH(); }
    void checkStackAlignment() { MOZ_CRASH(); }
    uint32_t currentOffset() { MOZ_CRASH(); }
    uint32_t actualOffset(uint32_t) { MOZ_CRASH(); }
    uint32_t labelOffsetToPatchOffset(uint32_t) { MOZ_CRASH(); }
    CodeOffsetLabel labelForPatch() { MOZ_CRASH(); }

    template <typename T> void call(T) { MOZ_CRASH(); }
    template <typename T, typename S> void call(T, S) { MOZ_CRASH(); }
    template <typename T> void callWithABI(T, MoveOp::Type v = MoveOp::GENERAL) { MOZ_CRASH(); }
    void callAndPushReturnAddress(Label* label) { MOZ_CRASH(); }

    void setupAlignedABICall(uint32_t) { MOZ_CRASH(); }
    void setupUnalignedABICall(uint32_t, Register) { MOZ_CRASH(); }
    template <typename T> void passABIArg(T, MoveOp::Type v = MoveOp::GENERAL) { MOZ_CRASH(); }

    void callWithExitFrame(Label*) { MOZ_CRASH(); }
    void callWithExitFrame(JitCode*) { MOZ_CRASH(); }
    void callWithExitFrame(JitCode*, Register) { MOZ_CRASH(); }

    void callJit(Register callee) { MOZ_CRASH(); }
    void callJitFromAsmJS(Register callee) { MOZ_CRASH(); }

    void nop() { MOZ_CRASH(); }
    void breakpoint() { MOZ_CRASH(); }
    void abiret() { MOZ_CRASH(); }
    void ret() { MOZ_CRASH(); }

    CodeOffsetLabel toggledJump(Label*) { MOZ_CRASH(); }
    CodeOffsetLabel toggledCall(JitCode*, bool) { MOZ_CRASH(); }
    static size_t ToggledCallSize(uint8_t*) { MOZ_CRASH(); }

    void writePrebarrierOffset(CodeOffsetLabel) { MOZ_CRASH(); }

    void finish() { MOZ_CRASH(); }

    template <typename T, typename S> void moveValue(T, S) { MOZ_CRASH(); }
    template <typename T, typename S, typename U> void moveValue(T, S, U) { MOZ_CRASH(); }
    template <typename T, typename S> void storeValue(T, S) { MOZ_CRASH(); }
    template <typename T, typename S, typename U> void storeValue(T, S, U) { MOZ_CRASH(); }
    template <typename T, typename S> void loadValue(T, S) { MOZ_CRASH(); }
    template <typename T> void pushValue(T) { MOZ_CRASH(); }
    template <typename T, typename S> void pushValue(T, S) { MOZ_CRASH(); }
    void popValue(ValueOperand) { MOZ_CRASH(); }
    void tagValue(JSValueType, Register, ValueOperand) { MOZ_CRASH(); }
    void retn(Imm32 n) { MOZ_CRASH(); }
    template <typename T> void push(T) { MOZ_CRASH(); }
    template <typename T> void Push(T) { MOZ_CRASH(); }
    template <typename T> void pop(T) { MOZ_CRASH(); }
    template <typename T> void Pop(T) { MOZ_CRASH(); }
    template <typename T> CodeOffsetLabel PushWithPatch(T) { MOZ_CRASH(); }
    void implicitPop(uint32_t) { MOZ_CRASH(); }

    CodeOffsetJump jumpWithPatch(RepatchLabel*) { MOZ_CRASH(); }
    CodeOffsetJump jumpWithPatch(RepatchLabel*, Condition) { MOZ_CRASH(); }
    CodeOffsetJump backedgeJump(RepatchLabel* label) { MOZ_CRASH(); }
    template <typename T, typename S>
    CodeOffsetJump branchPtrWithPatch(Condition, T, S, RepatchLabel*) { MOZ_CRASH(); }

    template <typename T, typename S> void branchTestValue(Condition, T, S, Label*) { MOZ_CRASH(); }
    void testNullSet(Condition, ValueOperand, Register) { MOZ_CRASH(); }
    void testObjectSet(Condition, ValueOperand, Register) { MOZ_CRASH(); }
    void testUndefinedSet(Condition, ValueOperand, Register) { MOZ_CRASH(); }

    template <typename T, typename S> void cmpPtrSet(Condition, T, S, Register) { MOZ_CRASH(); }
    template <typename T, typename S> void cmp32Set(Condition, T, S, Register) { MOZ_CRASH(); }

    void reserveStack(uint32_t) { MOZ_CRASH(); }
    template <typename T> void freeStack(T) { MOZ_CRASH(); }
    template <typename T, typename S> void add32(T, S) { MOZ_CRASH(); }
    template <typename T, typename S> void addPtr(T, S) { MOZ_CRASH(); }
    template <typename T, typename S> void sub32(T, S) { MOZ_CRASH(); }
    template <typename T, typename S> void subPtr(T, S) { MOZ_CRASH(); }
    void neg32(Register) { MOZ_CRASH(); }
    void mulBy3(Register, Register) { MOZ_CRASH(); }

    void negateDouble(FloatRegister) { MOZ_CRASH(); }
    void addDouble(FloatRegister, FloatRegister) { MOZ_CRASH(); }
    void subDouble(FloatRegister, FloatRegister) { MOZ_CRASH(); }
    void mulDouble(FloatRegister, FloatRegister) { MOZ_CRASH(); }
    void divDouble(FloatRegister, FloatRegister) { MOZ_CRASH(); }

    template <typename T, typename S> void branch32(Condition, T, S, Label*) { MOZ_CRASH(); }
    template <typename T, typename S> void branchTest32(Condition, T, S, Label*) { MOZ_CRASH(); }
    template <typename T, typename S> void branchAdd32(Condition, T, S, Label*) { MOZ_CRASH(); }
    template <typename T, typename S> void branchSub32(Condition, T, S, Label*) { MOZ_CRASH(); }
    template <typename T, typename S> void branchPtr(Condition, T, S, Label*) { MOZ_CRASH(); }
    template <typename T, typename S> void branchTestPtr(Condition, T, S, Label*) { MOZ_CRASH(); }
    template <typename T, typename S> void branchDouble(DoubleCondition, T, S, Label*) { MOZ_CRASH(); }
    template <typename T, typename S> void branchFloat(DoubleCondition, T, S, Label*) { MOZ_CRASH(); }
    template <typename T, typename S> void branchPrivatePtr(Condition, T, S, Label*) { MOZ_CRASH(); }
    template <typename T, typename S> void decBranchPtr(Condition, T, S, Label*) { MOZ_CRASH(); }
    template <typename T, typename S> void mov(T, S) { MOZ_CRASH(); }
    template <typename T, typename S> void movq(T, S) { MOZ_CRASH(); }
    template <typename T, typename S> void movePtr(T, S) { MOZ_CRASH(); }
    template <typename T, typename S> void move32(T, S) { MOZ_CRASH(); }
    template <typename T, typename S> void moveFloat32(T, S) { MOZ_CRASH(); }
    template <typename T, typename S> void moveDouble(T, S) { MOZ_CRASH(); }
    template <typename T> CodeOffsetLabel movWithPatch(T, Register) { MOZ_CRASH(); }

    template <typename T> void loadPtr(T, Register) { MOZ_CRASH(); }
    template <typename T> void load32(T, Register) { MOZ_CRASH(); }
    template <typename T> void loadFloat32(T, FloatRegister) { MOZ_CRASH(); }
    template <typename T> void loadDouble(T, FloatRegister) { MOZ_CRASH(); }
    template <typename T> void loadAlignedInt32x4(T, FloatRegister) { MOZ_CRASH(); }
    template <typename T> void loadUnalignedInt32x4(T, FloatRegister) { MOZ_CRASH(); }
    template <typename T> void loadAlignedFloat32x4(T, FloatRegister) { MOZ_CRASH(); }
    template <typename T> void loadUnalignedFloat32x4(T, FloatRegister) { MOZ_CRASH(); }
    template <typename T> void loadPrivate(T, Register) { MOZ_CRASH(); }
    template <typename T> void load8SignExtend(T, Register) { MOZ_CRASH(); }
    template <typename T> void load8ZeroExtend(T, Register) { MOZ_CRASH(); }
    template <typename T> void load16SignExtend(T, Register) { MOZ_CRASH(); }
    template <typename T> void load16ZeroExtend(T, Register) { MOZ_CRASH(); }

    template <typename T, typename S> void storePtr(T, S) { MOZ_CRASH(); }
    template <typename T, typename S> void store32(T, S) { MOZ_CRASH(); }
    template <typename T, typename S> void store32_NoSecondScratch(T, S) { MOZ_CRASH(); }
    template <typename T, typename S> void storeFloat32(T, S) { MOZ_CRASH(); }
    template <typename T, typename S> void storeDouble(T, S) { MOZ_CRASH(); }
    template <typename T, typename S> void storeAlignedInt32x4(T, S) { MOZ_CRASH(); }
    template <typename T, typename S> void storeUnalignedInt32x4(T, S) { MOZ_CRASH(); }
    template <typename T, typename S> void storeAlignedFloat32x4(T, S) { MOZ_CRASH(); }
    template <typename T, typename S> void storeUnalignedFloat32x4(T, S) { MOZ_CRASH(); }
    template <typename T, typename S> void store8(T, S) { MOZ_CRASH(); }
    template <typename T, typename S> void store16(T, S) { MOZ_CRASH(); }

    template <typename T> void computeEffectiveAddress(T, Register) { MOZ_CRASH(); }

    template <typename T> void compareExchange8SignExtend(const T& mem, Register oldval, Register newval, Register output) { MOZ_CRASH(); }
    template <typename T> void compareExchange8ZeroExtend(const T& mem, Register oldval, Register newval, Register output) { MOZ_CRASH(); }
    template <typename T> void compareExchange16SignExtend(const T& mem, Register oldval, Register newval, Register output) { MOZ_CRASH(); }
    template <typename T> void compareExchange16ZeroExtend(const T& mem, Register oldval, Register newval, Register output) { MOZ_CRASH(); }
    template <typename T> void compareExchange32(const T& mem, Register oldval, Register newval, Register output) { MOZ_CRASH(); }
    template <typename T, typename S> void atomicFetchAdd8SignExtend(const T& value, const S& mem, Register temp, Register output) { MOZ_CRASH(); }
    template <typename T, typename S> void atomicFetchAdd8ZeroExtend(const T& value, const S& mem, Register temp, Register output) { MOZ_CRASH(); }
    template <typename T, typename S> void atomicFetchAdd16SignExtend(const T& value, const S& mem, Register temp, Register output) { MOZ_CRASH(); }
    template <typename T, typename S> void atomicFetchAdd16ZeroExtend(const T& value, const S& mem, Register temp, Register output) { MOZ_CRASH(); }
    template <typename T, typename S> void atomicFetchAdd32(const T& value, const S& mem, Register temp, Register output) { MOZ_CRASH(); }
    template <typename T, typename S> void atomicFetchSub8SignExtend(const T& value, const S& mem, Register temp, Register output) { MOZ_CRASH(); }
    template <typename T, typename S> void atomicFetchSub8ZeroExtend(const T& value, const S& mem, Register temp, Register output) { MOZ_CRASH(); }
    template <typename T, typename S> void atomicFetchSub16SignExtend(const T& value, const S& mem, Register temp, Register output) { MOZ_CRASH(); }
    template <typename T, typename S> void atomicFetchSub16ZeroExtend(const T& value, const S& mem, Register temp, Register output) { MOZ_CRASH(); }
    template <typename T, typename S> void atomicFetchSub32(const T& value, const S& mem, Register temp, Register output) { MOZ_CRASH(); }
    template <typename T, typename S> void atomicFetchAnd8SignExtend(const T& value, const S& mem, Register temp, Register output) { MOZ_CRASH(); }
    template <typename T, typename S> void atomicFetchAnd8ZeroExtend(const T& value, const S& mem, Register temp, Register output) { MOZ_CRASH(); }
    template <typename T, typename S> void atomicFetchAnd16SignExtend(const T& value, const S& mem, Register temp, Register output) { MOZ_CRASH(); }
    template <typename T, typename S> void atomicFetchAnd16ZeroExtend(const T& value, const S& mem, Register temp, Register output) { MOZ_CRASH(); }
    template <typename T, typename S> void atomicFetchAnd32(const T& value, const S& mem, Register temp, Register output) { MOZ_CRASH(); }
    template <typename T, typename S> void atomicFetchOr8SignExtend(const T& value, const S& mem, Register temp, Register output) { MOZ_CRASH(); }
    template <typename T, typename S> void atomicFetchOr8ZeroExtend(const T& value, const S& mem, Register temp, Register output) { MOZ_CRASH(); }
    template <typename T, typename S> void atomicFetchOr16SignExtend(const T& value, const S& mem, Register temp, Register output) { MOZ_CRASH(); }
    template <typename T, typename S> void atomicFetchOr16ZeroExtend(const T& value, const S& mem, Register temp, Register output) { MOZ_CRASH(); }
    template <typename T, typename S> void atomicFetchOr32(const T& value, const S& mem, Register temp, Register output) { MOZ_CRASH(); }
    template <typename T, typename S> void atomicFetchXor8SignExtend(const T& value, const S& mem, Register temp, Register output) { MOZ_CRASH(); }
    template <typename T, typename S> void atomicFetchXor8ZeroExtend(const T& value, const S& mem, Register temp, Register output) { MOZ_CRASH(); }
    template <typename T, typename S> void atomicFetchXor16SignExtend(const T& value, const S& mem, Register temp, Register output) { MOZ_CRASH(); }
    template <typename T, typename S> void atomicFetchXor16ZeroExtend(const T& value, const S& mem, Register temp, Register output) { MOZ_CRASH(); }
    template <typename T, typename S> void atomicFetchXor32(const T& value, const S& mem, Register temp, Register output) { MOZ_CRASH(); }

    void clampIntToUint8(Register) { MOZ_CRASH(); }

    Register splitTagForTest(ValueOperand) { MOZ_CRASH(); }

    template <typename T> void branchTestUndefined(Condition, T, Label*) { MOZ_CRASH(); }
    template <typename T> void branchTestInt32(Condition, T, Label*) { MOZ_CRASH(); }
    template <typename T> void branchTestBoolean(Condition, T, Label*) { MOZ_CRASH(); }
    template <typename T> void branchTestDouble(Condition, T, Label*) { MOZ_CRASH(); }
    template <typename T> void branchTestNull(Condition, T, Label*) { MOZ_CRASH(); }
    template <typename T> void branchTestString(Condition, T, Label*) { MOZ_CRASH(); }
    template <typename T> void branchTestSymbol(Condition, T, Label*) { MOZ_CRASH(); }
    template <typename T> void branchTestObject(Condition, T, Label*) { MOZ_CRASH(); }
    template <typename T> void branchTestNumber(Condition, T, Label*) { MOZ_CRASH(); }
    template <typename T> void branchTestGCThing(Condition, T, Label*) { MOZ_CRASH(); }
    template <typename T> void branchTestPrimitive(Condition, T, Label*) { MOZ_CRASH(); }
    template <typename T> void branchTestMagic(Condition, T, Label*) { MOZ_CRASH(); }
    template <typename T> void branchTestMagicValue(Condition, T, JSWhyMagic, Label*) { MOZ_CRASH(); }
    void boxDouble(FloatRegister, ValueOperand) { MOZ_CRASH(); }
    void boxNonDouble(JSValueType, Register, ValueOperand) { MOZ_CRASH(); }
    template <typename T> void unboxInt32(T, Register) { MOZ_CRASH(); }
    template <typename T> void unboxBoolean(T, Register) { MOZ_CRASH(); }
    template <typename T> void unboxString(T, Register) { MOZ_CRASH(); }
    template <typename T> void unboxSymbol(T, Register) { MOZ_CRASH(); }
    template <typename T> void unboxObject(T, Register) { MOZ_CRASH(); }
    template <typename T> void unboxDouble(T, FloatRegister) { MOZ_CRASH(); }
    void unboxValue(const ValueOperand&, AnyRegister) { MOZ_CRASH(); }
    void unboxNonDouble(const ValueOperand&, Register ) { MOZ_CRASH();}
    void notBoolean(ValueOperand) { MOZ_CRASH(); }
    Register extractObject(Address, Register) { MOZ_CRASH(); }
    Register extractObject(ValueOperand, Register) { MOZ_CRASH(); }
    Register extractInt32(ValueOperand, Register) { MOZ_CRASH(); }
    Register extractBoolean(ValueOperand, Register) { MOZ_CRASH(); }
    template <typename T> Register extractTag(T, Register) { MOZ_CRASH(); }

    void convertFloat32ToInt32(FloatRegister, Register, Label*, bool v = true) { MOZ_CRASH(); }
    void convertDoubleToInt32(FloatRegister, Register, Label*, bool v = true) { MOZ_CRASH(); }
    void convertBoolToInt32(Register, Register) { MOZ_CRASH(); }

    void convertDoubleToFloat32(FloatRegister, FloatRegister) { MOZ_CRASH(); }
    void convertInt32ToFloat32(Register, FloatRegister) { MOZ_CRASH(); }

    template <typename T> void convertInt32ToDouble(T, FloatRegister) { MOZ_CRASH(); }
    void convertFloat32ToDouble(FloatRegister, FloatRegister) { MOZ_CRASH(); }

    void branchTruncateDouble(FloatRegister, Register, Label*) { MOZ_CRASH(); }
    void branchTruncateFloat32(FloatRegister, Register, Label*) { MOZ_CRASH(); }

    void boolValueToDouble(ValueOperand, FloatRegister) { MOZ_CRASH(); }
    void boolValueToFloat32(ValueOperand, FloatRegister) { MOZ_CRASH(); }
    void int32ValueToDouble(ValueOperand, FloatRegister) { MOZ_CRASH(); }
    void int32ValueToFloat32(ValueOperand, FloatRegister) { MOZ_CRASH(); }

    void loadConstantDouble(double, FloatRegister) { MOZ_CRASH(); }
    void addConstantDouble(double, FloatRegister) { MOZ_CRASH(); }
    void loadConstantFloat32(float, FloatRegister) { MOZ_CRASH(); }
    void addConstantFloat32(float, FloatRegister) { MOZ_CRASH(); }
    Condition testInt32Truthy(bool, ValueOperand) { MOZ_CRASH(); }
    Condition testStringTruthy(bool, ValueOperand) { MOZ_CRASH(); }
    void branchTestInt32Truthy(bool, ValueOperand, Label*) { MOZ_CRASH(); }
    void branchTestBooleanTruthy(bool, ValueOperand, Label*) { MOZ_CRASH(); }
    void branchTestStringTruthy(bool, ValueOperand, Label*) { MOZ_CRASH(); }
    void branchTestDoubleTruthy(bool, FloatRegister, Label*) { MOZ_CRASH(); }

    template <typename T> void loadUnboxedValue(T, MIRType, AnyRegister) { MOZ_CRASH(); }
    template <typename T> void storeUnboxedValue(ConstantOrRegister, MIRType, T, MIRType) { MOZ_CRASH(); }
    template <typename T> void storeUnboxedPayload(ValueOperand value, T, size_t) { MOZ_CRASH(); }

    void rshiftPtr(Imm32, Register) { MOZ_CRASH(); }
    void rshiftPtrArithmetic(Imm32, Register) { MOZ_CRASH(); }
    void lshiftPtr(Imm32, Register) { MOZ_CRASH(); }
    template <typename T, typename S> void xorPtr(T, S) { MOZ_CRASH(); }
    template <typename T, typename S> void xor32(T, S) { MOZ_CRASH(); }
    template <typename T, typename S> void orPtr(T, S) { MOZ_CRASH(); }
    template <typename T, typename S> void or32(T, S) { MOZ_CRASH(); }
    template <typename T, typename S> void andPtr(T, S) { MOZ_CRASH(); }
    template <typename T, typename S> void and32(T, S) { MOZ_CRASH(); }
    template <typename T> void not32(T) { MOZ_CRASH(); }
    void convertUInt32ToDouble(Register, FloatRegister) { MOZ_CRASH(); }
    void convertUInt32ToFloat32(Register, FloatRegister) { MOZ_CRASH(); }
    void inc64(AbsoluteAddress) { MOZ_CRASH(); }
    void incrementInt32Value(Address) { MOZ_CRASH(); }
    void ensureDouble(ValueOperand, FloatRegister, Label*) { MOZ_CRASH(); }
    void handleFailureWithHandlerTail(void*) { MOZ_CRASH(); }
    void makeFrameDescriptor(Register, FrameType) { MOZ_CRASH(); }

    void branchPtrInNurseryRange(Condition, Register, Register, Label*) { MOZ_CRASH(); }
    void branchValueIsNurseryObject(Condition, ValueOperand, Register, Label*) { MOZ_CRASH(); }

    void buildFakeExitFrame(Register, uint32_t*) { MOZ_CRASH(); }
    bool buildOOLFakeExitFrame(void*) { MOZ_CRASH(); }
    void loadAsmJSActivation(Register) { MOZ_CRASH(); }
    void loadAsmJSHeapRegisterFromGlobalData() { MOZ_CRASH(); }
    void memIntToValue(Address, Address) { MOZ_CRASH(); }

    void setPrinter(Sprinter*) { MOZ_CRASH(); }
    Operand ToPayload(Operand base) { MOZ_CRASH(); }

    // Instrumentation for entering and leaving the profiler.
    void profilerEnterFrame(Register , Register ) { MOZ_CRASH(); }
    void profilerExitFrame() { MOZ_CRASH(); }

#ifdef JS_NUNBOX32
    Address ToPayload(Address) { MOZ_CRASH(); }
    Address ToType(Address) { MOZ_CRASH(); }
#endif
};

typedef MacroAssemblerNone MacroAssemblerSpecific;

class ABIArgGenerator
{
  public:
    ABIArgGenerator() { MOZ_CRASH(); }
    ABIArg next(MIRType) { MOZ_CRASH(); }
    ABIArg& current() { MOZ_CRASH(); }
    uint32_t stackBytesConsumedSoFar() const { MOZ_CRASH(); }

    static const Register NonArgReturnReg0;
    static const Register NonArgReturnReg1;
    static const Register NonArg_VolatileReg;
    static const Register NonReturn_VolatileReg0;
    static const Register NonReturn_VolatileReg1;
};

static inline void PatchJump(CodeLocationJump&, CodeLocationLabel) { MOZ_CRASH(); }
static inline bool GetTempRegForIntArg(uint32_t, uint32_t, Register*) { MOZ_CRASH(); }
static inline
void PatchBackedge(CodeLocationJump& jump_, CodeLocationLabel label, JitRuntime::BackedgeTarget target)
{
    MOZ_CRASH();
}

} // namespace jit
} // namespace js

#endif /* jit_none_MacroAssembler_none_h */

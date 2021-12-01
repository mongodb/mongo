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

static constexpr Register StackPointer { Registers::invalid_reg };
static constexpr Register FramePointer { Registers::invalid_reg };
static constexpr Register ReturnReg { Registers::invalid_reg };
static constexpr FloatRegister ReturnFloat32Reg = { FloatRegisters::invalid_reg };
static constexpr FloatRegister ReturnDoubleReg = { FloatRegisters::invalid_reg };
static constexpr FloatRegister ReturnSimd128Reg = { FloatRegisters::invalid_reg };
static constexpr FloatRegister ScratchFloat32Reg = { FloatRegisters::invalid_reg };
static constexpr FloatRegister ScratchDoubleReg = { FloatRegisters::invalid_reg };
static constexpr FloatRegister ScratchSimd128Reg = { FloatRegisters::invalid_reg };
static constexpr FloatRegister InvalidFloatReg = { FloatRegisters::invalid_reg };

static constexpr Register OsrFrameReg { Registers::invalid_reg };
static constexpr Register PreBarrierReg { Registers::invalid_reg };
static constexpr Register CallTempReg0 { Registers::invalid_reg };
static constexpr Register CallTempReg1 { Registers::invalid_reg };
static constexpr Register CallTempReg2 { Registers::invalid_reg };
static constexpr Register CallTempReg3 { Registers::invalid_reg };
static constexpr Register CallTempReg4 { Registers::invalid_reg };
static constexpr Register CallTempReg5 { Registers::invalid_reg };
static constexpr Register InvalidReg { Registers::invalid_reg };

static constexpr Register IntArgReg0 { Registers::invalid_reg };
static constexpr Register IntArgReg1 { Registers::invalid_reg };
static constexpr Register IntArgReg2 { Registers::invalid_reg };
static constexpr Register IntArgReg3 { Registers::invalid_reg };
static constexpr Register HeapReg { Registers::invalid_reg };

static constexpr Register WasmIonExitRegCallee { Registers::invalid_reg };
static constexpr Register WasmIonExitRegE0 { Registers::invalid_reg };
static constexpr Register WasmIonExitRegE1 { Registers::invalid_reg };

static constexpr Register WasmIonExitRegReturnData { Registers::invalid_reg };
static constexpr Register WasmIonExitRegReturnType { Registers::invalid_reg };
static constexpr Register WasmIonExitTlsReg { Registers::invalid_reg };
static constexpr Register WasmIonExitRegD0 { Registers::invalid_reg };
static constexpr Register WasmIonExitRegD1 { Registers::invalid_reg };
static constexpr Register WasmIonExitRegD2 { Registers::invalid_reg };

static constexpr Register RegExpTesterRegExpReg { Registers::invalid_reg };
static constexpr Register RegExpTesterStringReg { Registers::invalid_reg };
static constexpr Register RegExpTesterLastIndexReg { Registers::invalid_reg };
static constexpr Register RegExpTesterStickyReg { Registers::invalid_reg };

static constexpr Register RegExpMatcherRegExpReg { Registers::invalid_reg };
static constexpr Register RegExpMatcherStringReg { Registers::invalid_reg };
static constexpr Register RegExpMatcherLastIndexReg { Registers::invalid_reg };
static constexpr Register RegExpMatcherStickyReg { Registers::invalid_reg };

static constexpr Register JSReturnReg_Type { Registers::invalid_reg };
static constexpr Register JSReturnReg_Data { Registers::invalid_reg };
static constexpr Register JSReturnReg { Registers::invalid_reg };

#if defined(JS_NUNBOX32)
static constexpr ValueOperand JSReturnOperand(InvalidReg, InvalidReg);
static constexpr Register64 ReturnReg64(InvalidReg, InvalidReg);
#elif defined(JS_PUNBOX64)
static constexpr ValueOperand JSReturnOperand(InvalidReg);
static constexpr Register64 ReturnReg64(InvalidReg);
#else
#error "Bad architecture"
#endif

static constexpr Register ABINonArgReg0 { Registers::invalid_reg };
static constexpr Register ABINonArgReg1 { Registers::invalid_reg };
static constexpr Register ABINonArgReg2 { Registers::invalid_reg };
static constexpr Register ABINonArgReturnReg0 { Registers::invalid_reg };
static constexpr Register ABINonArgReturnReg1 { Registers::invalid_reg };
static constexpr Register ABINonArgReturnVolatileReg { Registers::invalid_reg };

static constexpr FloatRegister ABINonArgDoubleReg = { FloatRegisters::invalid_reg };

static constexpr Register WasmTableCallScratchReg { Registers::invalid_reg };
static constexpr Register WasmTableCallSigReg { Registers::invalid_reg };
static constexpr Register WasmTableCallIndexReg { Registers::invalid_reg };
static constexpr Register WasmTlsReg { Registers::invalid_reg };

static constexpr uint32_t ABIStackAlignment = 4;
static constexpr uint32_t CodeAlignment = sizeof(void*);
static constexpr uint32_t JitStackAlignment = 8;
static constexpr uint32_t JitStackValueAlignment = JitStackAlignment / sizeof(Value);

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
        CarrySet,
        CarryClear,
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

    static DoubleCondition InvertCondition(DoubleCondition) { MOZ_CRASH(); }

    template <typename T, typename S>
    static void PatchDataWithValueCheck(CodeLocationLabel, T, S) { MOZ_CRASH(); }
    static void PatchWrite_Imm32(CodeLocationLabel, Imm32) { MOZ_CRASH(); }

    static void PatchWrite_NearCall(CodeLocationLabel, CodeLocationLabel) { MOZ_CRASH(); }
    static uint32_t PatchWrite_NearCallSize() { MOZ_CRASH(); }

    static void ToggleToJmp(CodeLocationLabel) { MOZ_CRASH(); }
    static void ToggleToCmp(CodeLocationLabel) { MOZ_CRASH(); }
    static void ToggleCall(CodeLocationLabel, bool) { MOZ_CRASH(); }

    static void Bind(uint8_t*, const CodeLabel&) { MOZ_CRASH(); }

    static uintptr_t GetPointer(uint8_t*) { MOZ_CRASH(); }

    static bool HasRoundInstruction(RoundingMode) { return false; }

    void verifyHeapAccessDisassembly(uint32_t begin, uint32_t end,
                                     const Disassembler::HeapAccess& heapAccess)
    {
        MOZ_CRASH();
    }
};

class Operand
{
  public:
    Operand (const Address&) { MOZ_CRASH();}
    Operand (const Register) { MOZ_CRASH();}
    Operand (const FloatRegister) { MOZ_CRASH();}
    Operand (Register, Imm32 ) { MOZ_CRASH(); }
    Operand (Register, int32_t ) { MOZ_CRASH(); }
};

class ScratchTagScope
{
  public:
    ScratchTagScope(MacroAssembler&, const ValueOperand) {}
    operator Register() { MOZ_CRASH(); }
    void release() { MOZ_CRASH(); }
    void reacquire() { MOZ_CRASH(); }
};

class ScratchTagScopeRelease
{
  public:
    explicit ScratchTagScopeRelease(ScratchTagScope*) {}
};

class MacroAssemblerNone : public Assembler
{
  public:
    MacroAssemblerNone() { MOZ_CRASH(); }

    MoveResolver moveResolver_;

    size_t size() const { MOZ_CRASH(); }
    size_t bytesNeeded() const { MOZ_CRASH(); }
    size_t jumpRelocationTableBytes() const { MOZ_CRASH(); }
    size_t dataRelocationTableBytes() const { MOZ_CRASH(); }
    size_t preBarrierTableBytes() const { MOZ_CRASH(); }

    size_t numCodeLabels() const { MOZ_CRASH(); }
    CodeLabel codeLabel(size_t) { MOZ_CRASH(); }

    bool reserve(size_t size) { MOZ_CRASH(); }
    bool appendRawCode(const uint8_t* code, size_t numBytes) { MOZ_CRASH(); }
    bool swapBuffer(wasm::Bytes& bytes) { MOZ_CRASH(); }

    void trace(JSTracer*) { MOZ_CRASH(); }
    static void TraceJumpRelocations(JSTracer*, JitCode*, CompactBufferReader&) { MOZ_CRASH(); }
    static void TraceDataRelocations(JSTracer*, JitCode*, CompactBufferReader&) { MOZ_CRASH(); }

    static bool SupportsFloatingPoint() { return false; }
    static bool SupportsSimd() { return false; }
    static bool SupportsUnalignedAccesses() { return false; }

    void executableCopy(void*, bool = true) { MOZ_CRASH(); }
    void copyJumpRelocationTable(uint8_t*) { MOZ_CRASH(); }
    void copyDataRelocationTable(uint8_t*) { MOZ_CRASH(); }
    void copyPreBarrierTable(uint8_t*) { MOZ_CRASH(); }
    void processCodeLabels(uint8_t*) { MOZ_CRASH(); }

    void flushBuffer() { MOZ_CRASH(); }

    template <typename T> void bind(T) { MOZ_CRASH(); }
    void bindLater(Label*, wasm::OldTrapDesc) { MOZ_CRASH(); }
    template <typename T> void j(Condition, T) { MOZ_CRASH(); }
    template <typename T> void jump(T) { MOZ_CRASH(); }
    void writeCodePointer(CodeLabel* label) { MOZ_CRASH(); }
    void haltingAlign(size_t) { MOZ_CRASH(); }
    void nopAlign(size_t) { MOZ_CRASH(); }
    void checkStackAlignment() { MOZ_CRASH(); }
    uint32_t currentOffset() { MOZ_CRASH(); }
    CodeOffset labelForPatch() { MOZ_CRASH(); }

    void nop() { MOZ_CRASH(); }
    void breakpoint() { MOZ_CRASH(); }
    void abiret() { MOZ_CRASH(); }
    void ret() { MOZ_CRASH(); }

    CodeOffset toggledJump(Label*) { MOZ_CRASH(); }
    CodeOffset toggledCall(JitCode*, bool) { MOZ_CRASH(); }
    static size_t ToggledCallSize(uint8_t*) { MOZ_CRASH(); }

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
    template <typename T> CodeOffset pushWithPatch(T) { MOZ_CRASH(); }

    CodeOffsetJump jumpWithPatch(RepatchLabel*, Label* doc = nullptr) { MOZ_CRASH(); }
    CodeOffsetJump jumpWithPatch(RepatchLabel*, Condition, Label* doc = nullptr) { MOZ_CRASH(); }
    CodeOffsetJump backedgeJump(RepatchLabel* label, Label* doc = nullptr) { MOZ_CRASH(); }

    void testNullSet(Condition, ValueOperand, Register) { MOZ_CRASH(); }
    void testObjectSet(Condition, ValueOperand, Register) { MOZ_CRASH(); }
    void testUndefinedSet(Condition, ValueOperand, Register) { MOZ_CRASH(); }

    template <typename T, typename S> void cmpPtrSet(Condition, T, S, Register) { MOZ_CRASH(); }
    template <typename T, typename S> void cmp32Set(Condition, T, S, Register) { MOZ_CRASH(); }

    template <typename T> void mov(T, Register) { MOZ_CRASH(); }
    template <typename T> void movePtr(T, Register) { MOZ_CRASH(); }
    template <typename T> void move32(T, Register) { MOZ_CRASH(); }
    template <typename T, typename S> void movq(T, S) { MOZ_CRASH(); }
    template <typename T, typename S> void moveFloat32(T, S) { MOZ_CRASH(); }
    template <typename T, typename S> void moveDouble(T, S) { MOZ_CRASH(); }
    template <typename T, typename S> void move64(T, S) { MOZ_CRASH(); }
    template <typename T> CodeOffset movWithPatch(T, Register) { MOZ_CRASH(); }

    template <typename T> void loadInt32x1(T, FloatRegister dest) { MOZ_CRASH(); }
    template <typename T> void loadInt32x2(T, FloatRegister dest) { MOZ_CRASH(); }
    template <typename T> void loadInt32x3(T, FloatRegister dest) { MOZ_CRASH(); }
    template <typename T> void loadInt32x4(T, FloatRegister dest) { MOZ_CRASH(); }
    template <typename T> void loadFloat32x3(T, FloatRegister dest) { MOZ_CRASH(); }
    template <typename T> void loadFloat32x4(T, FloatRegister dest) { MOZ_CRASH(); }

    template <typename T> void loadPtr(T, Register) { MOZ_CRASH(); }
    template <typename T> void load32(T, Register) { MOZ_CRASH(); }
    template <typename T> void loadFloat32(T, FloatRegister) { MOZ_CRASH(); }
    template <typename T> void loadDouble(T, FloatRegister) { MOZ_CRASH(); }
    template <typename T> void loadAlignedSimd128Int(T, FloatRegister) { MOZ_CRASH(); }
    template <typename T> void loadUnalignedSimd128Int(T, FloatRegister) { MOZ_CRASH(); }
    template <typename T> void loadAlignedSimd128Float(T, FloatRegister) { MOZ_CRASH(); }
    template <typename T> void loadUnalignedSimd128Float(T, FloatRegister) { MOZ_CRASH(); }
    template <typename T> void loadPrivate(T, Register) { MOZ_CRASH(); }
    template <typename T> void load8SignExtend(T, Register) { MOZ_CRASH(); }
    template <typename T> void load8ZeroExtend(T, Register) { MOZ_CRASH(); }
    template <typename T> void load16SignExtend(T, Register) { MOZ_CRASH(); }
    template <typename T> void load16ZeroExtend(T, Register) { MOZ_CRASH(); }
    template <typename T> void load64(T, Register64 ) { MOZ_CRASH(); }

    template <typename T, typename S> void storePtr(const T&, S) { MOZ_CRASH(); }
    template <typename T, typename S> void store32(T, S) { MOZ_CRASH(); }
    template <typename T, typename S> void store32_NoSecondScratch(T, S) { MOZ_CRASH(); }
    template <typename T, typename S> void storeFloat32(T, S) { MOZ_CRASH(); }
    template <typename T, typename S> void storeDouble(T, S) { MOZ_CRASH(); }
    template <typename T, typename S> void storeAlignedSimd128Int(T, S) { MOZ_CRASH(); }
    template <typename T, typename S> void storeUnalignedSimd128Int(T, S) { MOZ_CRASH(); }
    template <typename T, typename S> void storeAlignedSimd128Float(T, S) { MOZ_CRASH(); }
    template <typename T, typename S> void storeUnalignedSimd128Float(T, S) { MOZ_CRASH(); }
    template <typename T, typename S> void store8(T, S) { MOZ_CRASH(); }
    template <typename T, typename S> void store16(T, S) { MOZ_CRASH(); }
    template <typename T, typename S> void storeInt32x1(T, S) { MOZ_CRASH(); }
    template <typename T, typename S> void storeInt32x2(T, S) { MOZ_CRASH(); }
    template <typename T, typename S> void storeInt32x3(T, S) { MOZ_CRASH(); }
    template <typename T, typename S> void storeInt32x4(T, S) { MOZ_CRASH(); }
    template <typename T, typename S> void storeFloat32x3(T, S) { MOZ_CRASH(); }
    template <typename T, typename S> void storeFloat32x4(T, S) { MOZ_CRASH(); }
    template <typename T, typename S> void store64(T, S) { MOZ_CRASH(); }

    template <typename T> void computeEffectiveAddress(T, Register) { MOZ_CRASH(); }

    void splitTagForTest(ValueOperand, ScratchTagScope&) { MOZ_CRASH(); }

    void boxDouble(FloatRegister, ValueOperand, FloatRegister) { MOZ_CRASH(); }
    void boxNonDouble(JSValueType, Register, ValueOperand) { MOZ_CRASH(); }
    template <typename T> void unboxInt32(T, Register) { MOZ_CRASH(); }
    template <typename T> void unboxBoolean(T, Register) { MOZ_CRASH(); }
    template <typename T> void unboxString(T, Register) { MOZ_CRASH(); }
    template <typename T> void unboxSymbol(T, Register) { MOZ_CRASH(); }
    template <typename T> void unboxObject(T, Register) { MOZ_CRASH(); }
    template <typename T> void unboxDouble(T, FloatRegister) { MOZ_CRASH(); }
    template <typename T> void unboxPrivate(T, Register) { MOZ_CRASH(); }
    void unboxValue(const ValueOperand&, AnyRegister, JSValueType) { MOZ_CRASH(); }
    void unboxNonDouble(const ValueOperand&, Register, JSValueType) { MOZ_CRASH();}
    void unboxNonDouble(const Address&, Register, JSValueType) { MOZ_CRASH();}
    void unboxGCThingForPreBarrierTrampoline(const Address&, Register) { MOZ_CRASH(); }
    void notBoolean(ValueOperand) { MOZ_CRASH(); }
    Register extractObject(Address, Register) { MOZ_CRASH(); }
    Register extractObject(ValueOperand, Register) { MOZ_CRASH(); }
    Register extractString(ValueOperand, Register) { MOZ_CRASH(); }
    Register extractSymbol(ValueOperand, Register) { MOZ_CRASH(); }
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

    void boolValueToDouble(ValueOperand, FloatRegister) { MOZ_CRASH(); }
    void boolValueToFloat32(ValueOperand, FloatRegister) { MOZ_CRASH(); }
    void int32ValueToDouble(ValueOperand, FloatRegister) { MOZ_CRASH(); }
    void int32ValueToFloat32(ValueOperand, FloatRegister) { MOZ_CRASH(); }

    void loadConstantDouble(double, FloatRegister) { MOZ_CRASH(); }
    void loadConstantFloat32(float, FloatRegister) { MOZ_CRASH(); }
    Condition testInt32Truthy(bool, ValueOperand) { MOZ_CRASH(); }
    Condition testStringTruthy(bool, ValueOperand) { MOZ_CRASH(); }

    template <typename T> void loadUnboxedValue(T, MIRType, AnyRegister) { MOZ_CRASH(); }
    template <typename T> void storeUnboxedValue(const ConstantOrRegister&, MIRType, T, MIRType) { MOZ_CRASH(); }
    template <typename T> void storeUnboxedPayload(ValueOperand value, T, size_t, JSValueType) { MOZ_CRASH(); }

    void convertUInt32ToDouble(Register, FloatRegister) { MOZ_CRASH(); }
    void convertUInt32ToFloat32(Register, FloatRegister) { MOZ_CRASH(); }
    void incrementInt32Value(Address) { MOZ_CRASH(); }
    void ensureDouble(ValueOperand, FloatRegister, Label*) { MOZ_CRASH(); }
    void handleFailureWithHandlerTail(void*) { MOZ_CRASH(); }

    void buildFakeExitFrame(Register, uint32_t*) { MOZ_CRASH(); }
    bool buildOOLFakeExitFrame(void*) { MOZ_CRASH(); }
    void loadWasmGlobalPtr(uint32_t, Register) { MOZ_CRASH(); }
    void loadWasmPinnedRegsFromTls() { MOZ_CRASH(); }

    void setPrinter(Sprinter*) { MOZ_CRASH(); }
    Operand ToPayload(Operand base) { MOZ_CRASH(); }

    static const Register getStackPointer() { MOZ_CRASH(); }

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
};

static inline void
PatchJump(CodeLocationJump&, CodeLocationLabel, ReprotectCode reprotect = DontReprotect)
{
    MOZ_CRASH();
}

static inline bool GetTempRegForIntArg(uint32_t, uint32_t, Register*) { MOZ_CRASH(); }

static inline
void PatchBackedge(CodeLocationJump& jump_, CodeLocationLabel label, JitZoneGroup::BackedgeTarget target)
{
    MOZ_CRASH();
}

} // namespace jit
} // namespace js

#endif /* jit_none_MacroAssembler_none_h */

/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_arm_Assembler_arm_h
#define jit_arm_Assembler_arm_h

#include "mozilla/ArrayUtils.h"
#include "mozilla/Attributes.h"
#include "mozilla/MathAlgorithms.h"

#include "jit/arm/Architecture-arm.h"
#include "jit/arm/disasm/Disasm-arm.h"
#include "jit/CompactBuffer.h"
#include "jit/IonCode.h"
#include "jit/JitCompartment.h"
#include "jit/shared/Assembler-shared.h"
#include "jit/shared/Disassembler-shared.h"
#include "jit/shared/IonAssemblerBufferWithConstantPools.h"

union PoolHintPun;

namespace js {
namespace jit {

using LiteralDoc = DisassemblerSpew::LiteralDoc;
using LabelDoc = DisassemblerSpew::LabelDoc;

// NOTE: there are duplicates in this list! Sometimes we want to specifically
// refer to the link register as a link register (bl lr is much clearer than bl
// r14). HOWEVER, this register can easily be a gpr when it is not busy holding
// the return address.
static constexpr Register r0  { Registers::r0 };
static constexpr Register r1  { Registers::r1 };
static constexpr Register r2  { Registers::r2 };
static constexpr Register r3  { Registers::r3 };
static constexpr Register r4  { Registers::r4 };
static constexpr Register r5  { Registers::r5 };
static constexpr Register r6  { Registers::r6 };
static constexpr Register r7  { Registers::r7 };
static constexpr Register r8  { Registers::r8 };
static constexpr Register r9  { Registers::r9 };
static constexpr Register r10 { Registers::r10 };
static constexpr Register r11 { Registers::r11 };
static constexpr Register r12 { Registers::ip };
static constexpr Register ip  { Registers::ip };
static constexpr Register sp  { Registers::sp };
static constexpr Register r14 { Registers::lr };
static constexpr Register lr  { Registers::lr };
static constexpr Register pc  { Registers::pc };

static constexpr Register ScratchRegister {Registers::ip};

// Helper class for ScratchRegister usage. Asserts that only one piece
// of code thinks it has exclusive ownership of the scratch register.
struct ScratchRegisterScope : public AutoRegisterScope
{
    explicit ScratchRegisterScope(MacroAssembler& masm)
      : AutoRegisterScope(masm, ScratchRegister)
    { }
};

struct SecondScratchRegisterScope : public AutoRegisterScope
{
    explicit SecondScratchRegisterScope(MacroAssembler& masm);
};

static constexpr Register OsrFrameReg = r3;
static constexpr Register CallTempReg0 = r5;
static constexpr Register CallTempReg1 = r6;
static constexpr Register CallTempReg2 = r7;
static constexpr Register CallTempReg3 = r8;
static constexpr Register CallTempReg4 = r0;
static constexpr Register CallTempReg5 = r1;

static constexpr Register IntArgReg0 = r0;
static constexpr Register IntArgReg1 = r1;
static constexpr Register IntArgReg2 = r2;
static constexpr Register IntArgReg3 = r3;
static constexpr Register HeapReg = r10;
static constexpr Register CallTempNonArgRegs[] = { r5, r6, r7, r8 };
static const uint32_t NumCallTempNonArgRegs =
    mozilla::ArrayLength(CallTempNonArgRegs);

class ABIArgGenerator
{
    unsigned intRegIndex_;
    unsigned floatRegIndex_;
    uint32_t stackOffset_;
    ABIArg current_;

    // ARM can either use HardFp (use float registers for float arguments), or
    // SoftFp (use general registers for float arguments) ABI.  We keep this
    // switch as a runtime switch because wasm always use the HardFp back-end
    // while the calls to native functions have to use the one provided by the
    // system.
    bool useHardFp_;

    ABIArg softNext(MIRType argType);
    ABIArg hardNext(MIRType argType);

  public:
    ABIArgGenerator();

    void setUseHardFp(bool useHardFp) {
        MOZ_ASSERT(intRegIndex_ == 0 && floatRegIndex_ == 0);
        useHardFp_ = useHardFp;
    }
    ABIArg next(MIRType argType);
    ABIArg& current() { return current_; }
    uint32_t stackBytesConsumedSoFar() const { return stackOffset_; }
};

bool IsUnaligned(const wasm::MemoryAccessDesc& access);

// These registers may be volatile or nonvolatile.
static constexpr Register ABINonArgReg0 = r4;
static constexpr Register ABINonArgReg1 = r5;
static constexpr Register ABINonArgReg2 = r6;

// This register may be volatile or nonvolatile. Avoid d15 which is the
// ScratchDoubleReg.
static constexpr FloatRegister ABINonArgDoubleReg { FloatRegisters::d8, VFPRegister::Double };

// These registers may be volatile or nonvolatile.
// Note: these three registers are all guaranteed to be different
static constexpr Register ABINonArgReturnReg0 = r4;
static constexpr Register ABINonArgReturnReg1 = r5;

// This register is guaranteed to be clobberable during the prologue and
// epilogue of an ABI call which must preserve both ABI argument, return
// and non-volatile registers.
static constexpr Register ABINonArgReturnVolatileReg = lr;

// TLS pointer argument register for WebAssembly functions. This must not alias
// any other register used for passing function arguments or return values.
// Preserved by WebAssembly functions.
static constexpr Register WasmTlsReg = r9;

// Registers used for wasm table calls. These registers must be disjoint
// from the ABI argument registers, WasmTlsReg and each other.
static constexpr Register WasmTableCallScratchReg = ABINonArgReg0;
static constexpr Register WasmTableCallSigReg = ABINonArgReg1;
static constexpr Register WasmTableCallIndexReg = ABINonArgReg2;

static constexpr Register PreBarrierReg = r1;

static constexpr Register InvalidReg { Registers::invalid_reg };
static constexpr FloatRegister InvalidFloatReg;

static constexpr Register JSReturnReg_Type = r3;
static constexpr Register JSReturnReg_Data = r2;
static constexpr Register StackPointer = sp;
static constexpr Register FramePointer = r11;
static constexpr Register ReturnReg = r0;
static constexpr Register64 ReturnReg64(r1, r0);
static constexpr FloatRegister ReturnFloat32Reg = { FloatRegisters::d0, VFPRegister::Single };
static constexpr FloatRegister ReturnDoubleReg = { FloatRegisters::d0, VFPRegister::Double};
static constexpr FloatRegister ReturnSimd128Reg = InvalidFloatReg;
static constexpr FloatRegister ScratchFloat32Reg = { FloatRegisters::s30, VFPRegister::Single };
static constexpr FloatRegister ScratchDoubleReg = { FloatRegisters::d15, VFPRegister::Double };
static constexpr FloatRegister ScratchSimd128Reg = InvalidFloatReg;
static constexpr FloatRegister ScratchUIntReg = { FloatRegisters::d15, VFPRegister::UInt };
static constexpr FloatRegister ScratchIntReg = { FloatRegisters::d15, VFPRegister::Int };

struct ScratchFloat32Scope : public AutoFloatRegisterScope
{
    explicit ScratchFloat32Scope(MacroAssembler& masm)
      : AutoFloatRegisterScope(masm, ScratchFloat32Reg)
    { }
};
struct ScratchDoubleScope : public AutoFloatRegisterScope
{
    explicit ScratchDoubleScope(MacroAssembler& masm)
      : AutoFloatRegisterScope(masm, ScratchDoubleReg)
    { }
};

// Registers used in the GenerateFFIIonExit Enable Activation block.
static constexpr Register WasmIonExitRegCallee = r4;
static constexpr Register WasmIonExitRegE0 = r0;
static constexpr Register WasmIonExitRegE1 = r1;

// Registers used in the GenerateFFIIonExit Disable Activation block.
// None of these may be the second scratch register (lr).
static constexpr Register WasmIonExitRegReturnData = r2;
static constexpr Register WasmIonExitRegReturnType = r3;
static constexpr Register WasmIonExitTlsReg = r9;
static constexpr Register WasmIonExitRegD0 = r0;
static constexpr Register WasmIonExitRegD1 = r1;
static constexpr Register WasmIonExitRegD2 = r4;

// Registerd used in RegExpMatcher instruction (do not use JSReturnOperand).
static constexpr Register RegExpMatcherRegExpReg = CallTempReg0;
static constexpr Register RegExpMatcherStringReg = CallTempReg1;
static constexpr Register RegExpMatcherLastIndexReg = CallTempReg2;

// Registerd used in RegExpTester instruction (do not use ReturnReg).
static constexpr Register RegExpTesterRegExpReg = CallTempReg0;
static constexpr Register RegExpTesterStringReg = CallTempReg1;
static constexpr Register RegExpTesterLastIndexReg = CallTempReg2;

static constexpr FloatRegister d0  = {FloatRegisters::d0, VFPRegister::Double};
static constexpr FloatRegister d1  = {FloatRegisters::d1, VFPRegister::Double};
static constexpr FloatRegister d2  = {FloatRegisters::d2, VFPRegister::Double};
static constexpr FloatRegister d3  = {FloatRegisters::d3, VFPRegister::Double};
static constexpr FloatRegister d4  = {FloatRegisters::d4, VFPRegister::Double};
static constexpr FloatRegister d5  = {FloatRegisters::d5, VFPRegister::Double};
static constexpr FloatRegister d6  = {FloatRegisters::d6, VFPRegister::Double};
static constexpr FloatRegister d7  = {FloatRegisters::d7, VFPRegister::Double};
static constexpr FloatRegister d8  = {FloatRegisters::d8, VFPRegister::Double};
static constexpr FloatRegister d9  = {FloatRegisters::d9, VFPRegister::Double};
static constexpr FloatRegister d10 = {FloatRegisters::d10, VFPRegister::Double};
static constexpr FloatRegister d11 = {FloatRegisters::d11, VFPRegister::Double};
static constexpr FloatRegister d12 = {FloatRegisters::d12, VFPRegister::Double};
static constexpr FloatRegister d13 = {FloatRegisters::d13, VFPRegister::Double};
static constexpr FloatRegister d14 = {FloatRegisters::d14, VFPRegister::Double};
static constexpr FloatRegister d15 = {FloatRegisters::d15, VFPRegister::Double};


// For maximal awesomeness, 8 should be sufficent. ldrd/strd (dual-register
// load/store) operate in a single cycle when the address they are dealing with
// is 8 byte aligned. Also, the ARM abi wants the stack to be 8 byte aligned at
// function boundaries. I'm trying to make sure this is always true.
static constexpr uint32_t ABIStackAlignment = 8;
static constexpr uint32_t CodeAlignment = 8;
static constexpr uint32_t JitStackAlignment = 8;

static constexpr uint32_t JitStackValueAlignment = JitStackAlignment / sizeof(Value);
static_assert(JitStackAlignment % sizeof(Value) == 0 && JitStackValueAlignment >= 1,
  "Stack alignment should be a non-zero multiple of sizeof(Value)");

// This boolean indicates whether we support SIMD instructions flavoured for
// this architecture or not. Rather than a method in the LIRGenerator, it is
// here such that it is accessible from the entire codebase. Once full support
// for SIMD is reached on all tier-1 platforms, this constant can be deleted.
static constexpr bool SupportsSimd = false;
static constexpr uint32_t SimdMemoryAlignment = 8;

static_assert(CodeAlignment % SimdMemoryAlignment == 0,
  "Code alignment should be larger than any of the alignments which are used for "
  "the constant sections of the code buffer.  Thus it should be larger than the "
  "alignment for SIMD constants.");

static_assert(JitStackAlignment % SimdMemoryAlignment == 0,
  "Stack alignment should be larger than any of the alignments which are used for "
  "spilled values.  Thus it should be larger than the alignment for SIMD accesses.");

static const uint32_t WasmStackAlignment = SimdMemoryAlignment;

// Does this architecture support SIMD conversions between Uint32x4 and Float32x4?
static constexpr bool SupportsUint32x4FloatConversions = false;

// Does this architecture support comparisons of unsigned integer vectors?
static constexpr bool SupportsUint8x16Compares = false;
static constexpr bool SupportsUint16x8Compares = false;
static constexpr bool SupportsUint32x4Compares = false;

static const Scale ScalePointer = TimesFour;

class Instruction;
class InstBranchImm;
uint32_t RM(Register r);
uint32_t RS(Register r);
uint32_t RD(Register r);
uint32_t RT(Register r);
uint32_t RN(Register r);

uint32_t maybeRD(Register r);
uint32_t maybeRT(Register r);
uint32_t maybeRN(Register r);

Register toRN(Instruction i);
Register toRM(Instruction i);
Register toRD(Instruction i);
Register toR(Instruction i);

class VFPRegister;
uint32_t VD(VFPRegister vr);
uint32_t VN(VFPRegister vr);
uint32_t VM(VFPRegister vr);

// For being passed into the generic vfp instruction generator when there is an
// instruction that only takes two registers.
static constexpr VFPRegister NoVFPRegister(VFPRegister::Double, 0, false, true);

struct ImmTag : public Imm32
{
    explicit ImmTag(JSValueTag mask)
      : Imm32(int32_t(mask))
    { }
};

struct ImmType : public ImmTag
{
    explicit ImmType(JSValueType type)
      : ImmTag(JSVAL_TYPE_TO_TAG(type))
    { }
};

enum Index {
    Offset = 0 << 21 | 1<<24,
    PreIndex = 1 << 21 | 1 << 24,
    PostIndex = 0 << 21 | 0 << 24
    // The docs were rather unclear on this. It sounds like
    // 1 << 21 | 0 << 24 encodes dtrt.
};

enum IsImmOp2_ {
    IsImmOp2    = 1 << 25,
    IsNotImmOp2 = 0 << 25
};
enum IsImmDTR_ {
    IsImmDTR    = 0 << 25,
    IsNotImmDTR = 1 << 25
};
// For the extra memory operations, ldrd, ldrsb, ldrh.
enum IsImmEDTR_ {
    IsImmEDTR    = 1 << 22,
    IsNotImmEDTR = 0 << 22
};

enum ShiftType {
    LSL = 0, // << 5
    LSR = 1, // << 5
    ASR = 2, // << 5
    ROR = 3, // << 5
    RRX = ROR // RRX is encoded as ROR with a 0 offset.
};

// Modes for STM/LDM. Names are the suffixes applied to the instruction.
enum DTMMode {
    A = 0 << 24, // empty / after
    B = 1 << 24, // full / before
    D = 0 << 23, // decrement
    I = 1 << 23, // increment
    DA = D | A,
    DB = D | B,
    IA = I | A,
    IB = I | B
};

enum DTMWriteBack {
    WriteBack   = 1 << 21,
    NoWriteBack = 0 << 21
};

// Condition code updating mode.
enum SBit {
    SetCC   = 1 << 20,  // Set condition code.
    LeaveCC = 0 << 20   // Leave condition code unchanged.
};

enum LoadStore {
    IsLoad  = 1 << 20,
    IsStore = 0 << 20
};

// You almost never want to use this directly. Instead, you wantto pass in a
// signed constant, and let this bit be implicitly set for you. This is however,
// necessary if we want a negative index.
enum IsUp_ {
    IsUp   = 1 << 23,
    IsDown = 0 << 23
};
enum ALUOp {
    OpMov = 0xd << 21,
    OpMvn = 0xf << 21,
    OpAnd = 0x0 << 21,
    OpBic = 0xe << 21,
    OpEor = 0x1 << 21,
    OpOrr = 0xc << 21,
    OpAdc = 0x5 << 21,
    OpAdd = 0x4 << 21,
    OpSbc = 0x6 << 21,
    OpSub = 0x2 << 21,
    OpRsb = 0x3 << 21,
    OpRsc = 0x7 << 21,
    OpCmn = 0xb << 21,
    OpCmp = 0xa << 21,
    OpTeq = 0x9 << 21,
    OpTst = 0x8 << 21,
    OpInvalid = -1
};


enum MULOp {
    OpmMul   = 0 << 21,
    OpmMla   = 1 << 21,
    OpmUmaal = 2 << 21,
    OpmMls   = 3 << 21,
    OpmUmull = 4 << 21,
    OpmUmlal = 5 << 21,
    OpmSmull = 6 << 21,
    OpmSmlal = 7 << 21
};
enum BranchTag {
    OpB = 0x0a000000,
    OpBMask = 0x0f000000,
    OpBDestMask = 0x00ffffff,
    OpBl = 0x0b000000,
    OpBlx = 0x012fff30,
    OpBx  = 0x012fff10
};

// Just like ALUOp, but for the vfp instruction set.
enum VFPOp {
    OpvMul  = 0x2 << 20,
    OpvAdd  = 0x3 << 20,
    OpvSub  = 0x3 << 20 | 0x1 << 6,
    OpvDiv  = 0x8 << 20,
    OpvMov  = 0xB << 20 | 0x1 << 6,
    OpvAbs  = 0xB << 20 | 0x3 << 6,
    OpvNeg  = 0xB << 20 | 0x1 << 6 | 0x1 << 16,
    OpvSqrt = 0xB << 20 | 0x3 << 6 | 0x1 << 16,
    OpvCmp  = 0xB << 20 | 0x1 << 6 | 0x4 << 16,
    OpvCmpz  = 0xB << 20 | 0x1 << 6 | 0x5 << 16
};

// Negate the operation, AND negate the immediate that we were passed in.
ALUOp ALUNeg(ALUOp op, Register dest, Register scratch, Imm32* imm, Register* negDest);
bool can_dbl(ALUOp op);
bool condsAreSafe(ALUOp op);

// If there is a variant of op that has a dest (think cmp/sub) return that
// variant of it.
ALUOp getDestVariant(ALUOp op);

static constexpr ValueOperand JSReturnOperand{JSReturnReg_Type, JSReturnReg_Data};
static const ValueOperand softfpReturnOperand = ValueOperand(r1, r0);

// All of these classes exist solely to shuffle data into the various operands.
// For example Operand2 can be an imm8, a register-shifted-by-a-constant or a
// register-shifted-by-a-register. We represent this in C++ by having a base
// class Operand2, which just stores the 32 bits of data as they will be encoded
// in the instruction. You cannot directly create an Operand2 since it is
// tricky, and not entirely sane to do so. Instead, you create one of its child
// classes, e.g. Imm8. Imm8's constructor takes a single integer argument. Imm8
// will verify that its argument can be encoded as an ARM 12 bit imm8, encode it
// using an Imm8data, and finally call its parent's (Operand2) constructor with
// the Imm8data. The Operand2 constructor will then call the Imm8data's encode()
// function to extract the raw bits from it.
//
// In the future, we should be able to extract data from the Operand2 by asking
// it for its component Imm8data structures. The reason this is so horribly
// round-about is we wanted to have Imm8 and RegisterShiftedRegister inherit
// directly from Operand2 but have all of them take up only a single word of
// storage. We also wanted to avoid passing around raw integers at all since
// they are error prone.
class Op2Reg;
class O2RegImmShift;
class O2RegRegShift;

namespace datastore {

class Reg
{
    // The "second register".
    uint32_t rm_ : 4;
    // Do we get another register for shifting.
    bool rrs_ : 1;
    ShiftType type_ : 2;
    // We'd like this to be a more sensible encoding, but that would need to be
    // a struct and that would not pack :(
    uint32_t shiftAmount_ : 5;
    uint32_t pad_ : 20;

  public:
    Reg(uint32_t rm, ShiftType type, uint32_t rsr, uint32_t shiftAmount)
      : rm_(rm), rrs_(rsr), type_(type), shiftAmount_(shiftAmount), pad_(0)
    { }
    explicit Reg(const Op2Reg& op) {
        memcpy(this, &op, sizeof(*this));
    }

    uint32_t shiftAmount() const {
        return shiftAmount_;
    }

    uint32_t encode() const {
        return rm_ | (rrs_ << 4) | (type_ << 5) | (shiftAmount_ << 7);
    }
};

// Op2 has a mode labelled "<imm8m>", which is arm's magical immediate encoding.
// Some instructions actually get 8 bits of data, which is called Imm8Data
// below. These should have edit distance > 1, but this is how it is for now.
class Imm8mData
{
    uint32_t data_ : 8;
    uint32_t rot_ : 4;
    uint32_t buff_ : 19;

    // Throw in an extra bit that will be 1 if we can't encode this properly.
    // if we can encode it properly, a simple "|" will still suffice to meld it
    // into the instruction.
    bool invalid_ : 1;

  public:
    // Default constructor makes an invalid immediate.
    Imm8mData()
      : data_(0xff), rot_(0xf), buff_(0), invalid_(true)
    { }

    Imm8mData(uint32_t data, uint32_t rot)
      : data_(data), rot_(rot), buff_(0), invalid_(false)
    {
        MOZ_ASSERT(data == data_);
        MOZ_ASSERT(rot == rot_);
    }

    bool invalid() const { return invalid_; }

    uint32_t encode() const {
        MOZ_ASSERT(!invalid_);
        return data_ | (rot_ << 8);
    };
};

class Imm8Data
{
    uint32_t imm4L_ : 4;
    uint32_t pad_ : 4;
    uint32_t imm4H_ : 4;

  public:
    explicit Imm8Data(uint32_t imm)
      : imm4L_(imm & 0xf), imm4H_(imm >> 4)
    {
        MOZ_ASSERT(imm <= 0xff);
    }

    uint32_t encode() const {
        return imm4L_ | (imm4H_ << 8);
    };
};

// VLDR/VSTR take an 8 bit offset, which is implicitly left shifted by 2.
class Imm8VFPOffData
{
    uint32_t data_;

  public:
    explicit Imm8VFPOffData(uint32_t imm)
      : data_(imm)
    {
        MOZ_ASSERT((imm & ~(0xff)) == 0);
    }
    uint32_t encode() const {
        return data_;
    };
};

// ARM can magically encode 256 very special immediates to be moved into a
// register.
struct Imm8VFPImmData
{
    // This structure's members are public and it has no constructor to
    // initialize them, for a very special reason. Were this structure to
    // have a constructor, the initialization for DoubleEncoder's internal
    // table (see below) would require a rather large static constructor on
    // some of our supported compilers. The known solution to this is to mark
    // the constructor constexpr, but, again, some of our supported
    // compilers don't support constexpr! So we are reduced to public
    // members and eschewing a constructor in hopes that the initialization
    // of DoubleEncoder's table is correct.
    uint32_t imm4L : 4;
    uint32_t imm4H : 4;
    int32_t isInvalid : 24;

    uint32_t encode() const {
        // This assert is an attempting at ensuring that we don't create random
        // instances of this structure and then asking to encode() it.
        MOZ_ASSERT(isInvalid == 0);
        return imm4L | (imm4H << 16);
    };
};

class Imm12Data
{
    uint32_t data_ : 12;

  public:
    explicit Imm12Data(uint32_t imm)
      : data_(imm)
    {
        MOZ_ASSERT(data_ == imm);
    }

    uint32_t encode() const {
        return data_;
    }
};

class RIS
{
    uint32_t shiftAmount_ : 5;

  public:
    explicit RIS(uint32_t imm)
      : shiftAmount_(imm)
    {
        MOZ_ASSERT(shiftAmount_ == imm);
    }

    explicit RIS(Reg r)
      : shiftAmount_(r.shiftAmount())
    { }

    uint32_t encode() const {
        return shiftAmount_;
    }
};

class RRS
{
    bool mustZero_ : 1;
    // The register that holds the shift amount.
    uint32_t rs_ : 4;

  public:
    explicit RRS(uint32_t rs)
      : rs_(rs)
    {
        MOZ_ASSERT(rs_ == rs);
    }

    uint32_t encode() const {
        return rs_ << 1;
    }
};

} // namespace datastore

class MacroAssemblerARM;
class Operand;

class Operand2
{
    friend class Operand;
    friend class MacroAssemblerARM;
    friend class InstALU;

    uint32_t oper_ : 31;
    bool invalid_ : 1;

  protected:
    explicit Operand2(datastore::Imm8mData base)
      : oper_(base.invalid() ? -1 : (base.encode() | uint32_t(IsImmOp2))),
        invalid_(base.invalid())
    { }

    explicit Operand2(datastore::Reg base)
      : oper_(base.encode() | uint32_t(IsNotImmOp2)),
        invalid_(false)
    { }

  private:
    explicit Operand2(uint32_t blob)
      : oper_(blob),
        invalid_(false)
    { }

  public:
    bool isO2Reg() const {
        return !(oper_ & IsImmOp2);
    }

    Op2Reg toOp2Reg() const;

    bool isImm8() const {
        return oper_ & IsImmOp2;
    }

    bool invalid() const {
        return invalid_;
    }

    uint32_t encode() const {
        return oper_;
    }
};

class Imm8 : public Operand2
{
  public:
    explicit Imm8(uint32_t imm)
      : Operand2(EncodeImm(imm))
    { }

    static datastore::Imm8mData EncodeImm(uint32_t imm) {
        // RotateLeft below may not be called with a shift of zero.
        if (imm <= 0xFF)
            return datastore::Imm8mData(imm, 0);

        // An encodable integer has a maximum of 8 contiguous set bits,
        // with an optional wrapped left rotation to even bit positions.
        for (int rot = 1; rot < 16; rot++) {
            uint32_t rotimm = mozilla::RotateLeft(imm, rot * 2);
            if (rotimm <= 0xFF)
                return datastore::Imm8mData(rotimm, rot);
        }
        return datastore::Imm8mData();
    }

    // Pair template?
    struct TwoImm8mData
    {
        datastore::Imm8mData fst_, snd_;

        TwoImm8mData() = default;

        TwoImm8mData(datastore::Imm8mData fst, datastore::Imm8mData snd)
          : fst_(fst), snd_(snd)
        { }

        datastore::Imm8mData fst() const { return fst_; }
        datastore::Imm8mData snd() const { return snd_; }
    };

    static TwoImm8mData EncodeTwoImms(uint32_t);
};

class Op2Reg : public Operand2
{
  public:
    explicit Op2Reg(Register rm, ShiftType type, datastore::RIS shiftImm)
      : Operand2(datastore::Reg(rm.code(), type, 0, shiftImm.encode()))
    { }

    explicit Op2Reg(Register rm, ShiftType type, datastore::RRS shiftReg)
      : Operand2(datastore::Reg(rm.code(), type, 1, shiftReg.encode()))
    { }
};

static_assert(sizeof(Op2Reg) == sizeof(datastore::Reg),
              "datastore::Reg(const Op2Reg&) constructor relies on Reg/Op2Reg having same size");

class O2RegImmShift : public Op2Reg
{
  public:
    explicit O2RegImmShift(Register rn, ShiftType type, uint32_t shift)
      : Op2Reg(rn, type, datastore::RIS(shift))
    { }
};

class O2RegRegShift : public Op2Reg
{
  public:
    explicit O2RegRegShift(Register rn, ShiftType type, Register rs)
      : Op2Reg(rn, type, datastore::RRS(rs.code()))
    { }
};

O2RegImmShift O2Reg(Register r);
O2RegImmShift lsl(Register r, int amt);
O2RegImmShift lsr(Register r, int amt);
O2RegImmShift asr(Register r, int amt);
O2RegImmShift rol(Register r, int amt);
O2RegImmShift ror(Register r, int amt);

O2RegRegShift lsl(Register r, Register amt);
O2RegRegShift lsr(Register r, Register amt);
O2RegRegShift asr(Register r, Register amt);
O2RegRegShift ror(Register r, Register amt);

// An offset from a register to be used for ldr/str. This should include the
// sign bit, since ARM has "signed-magnitude" offsets. That is it encodes an
// unsigned offset, then the instruction specifies if the offset is positive or
// negative. The +/- bit is necessary if the instruction set wants to be able to
// have a negative register offset e.g. ldr pc, [r1,-r2];
class DtrOff
{
    uint32_t data_;

  protected:
    explicit DtrOff(datastore::Imm12Data immdata, IsUp_ iu)
      : data_(immdata.encode() | uint32_t(IsImmDTR) | uint32_t(iu))
    { }

    explicit DtrOff(datastore::Reg reg, IsUp_ iu = IsUp)
      : data_(reg.encode() | uint32_t(IsNotImmDTR) | iu)
    { }

  public:
    uint32_t encode() const { return data_; }
};

class DtrOffImm : public DtrOff
{
  public:
    explicit DtrOffImm(int32_t imm)
      : DtrOff(datastore::Imm12Data(mozilla::Abs(imm)), imm >= 0 ? IsUp : IsDown)
    {
        MOZ_ASSERT(mozilla::Abs(imm) < 4096);
    }
};

class DtrOffReg : public DtrOff
{
    // These are designed to be called by a constructor of a subclass.
    // Constructing the necessary RIS/RRS structures is annoying.

  protected:
    explicit DtrOffReg(Register rn, ShiftType type, datastore::RIS shiftImm, IsUp_ iu = IsUp)
      : DtrOff(datastore::Reg(rn.code(), type, 0, shiftImm.encode()), iu)
    { }

    explicit DtrOffReg(Register rn, ShiftType type, datastore::RRS shiftReg, IsUp_ iu = IsUp)
      : DtrOff(datastore::Reg(rn.code(), type, 1, shiftReg.encode()), iu)
    { }
};

class DtrRegImmShift : public DtrOffReg
{
  public:
    explicit DtrRegImmShift(Register rn, ShiftType type, uint32_t shift, IsUp_ iu = IsUp)
      : DtrOffReg(rn, type, datastore::RIS(shift), iu)
    { }
};

class DtrRegRegShift : public DtrOffReg
{
  public:
    explicit DtrRegRegShift(Register rn, ShiftType type, Register rs, IsUp_ iu = IsUp)
      : DtrOffReg(rn, type, datastore::RRS(rs.code()), iu)
    { }
};

// We will frequently want to bundle a register with its offset so that we have
// an "operand" to a load instruction.
class DTRAddr
{
    friend class Operand;

    uint32_t data_;

  public:
    explicit DTRAddr(Register reg, DtrOff dtr)
      : data_(dtr.encode() | (reg.code() << 16))
    { }

    uint32_t encode() const {
        return data_;
    }

    Register getBase() const {
        return Register::FromCode((data_ >> 16) & 0xf);
    }
};

// Offsets for the extended data transfer instructions:
// ldrsh, ldrd, ldrsb, etc.
class EDtrOff
{
    uint32_t data_;

  protected:
    explicit EDtrOff(datastore::Imm8Data imm8, IsUp_ iu = IsUp)
      : data_(imm8.encode() | IsImmEDTR | uint32_t(iu))
    { }

    explicit EDtrOff(Register rm, IsUp_ iu = IsUp)
      : data_(rm.code() | IsNotImmEDTR | iu)
    { }

  public:
    uint32_t encode() const {
        return data_;
    }
};

class EDtrOffImm : public EDtrOff
{
  public:
    explicit EDtrOffImm(int32_t imm)
      : EDtrOff(datastore::Imm8Data(mozilla::Abs(imm)), (imm >= 0) ? IsUp : IsDown)
    {
        MOZ_ASSERT(mozilla::Abs(imm) < 256);
    }
};

// This is the most-derived class, since the extended data transfer instructions
// don't support any sort of modifying the "index" operand.
class EDtrOffReg : public EDtrOff
{
  public:
    explicit EDtrOffReg(Register rm)
      : EDtrOff(rm)
    { }
};

class EDtrAddr
{
    uint32_t data_;

  public:
    explicit EDtrAddr(Register r, EDtrOff off)
      : data_(RN(r) | off.encode())
    { }

    uint32_t encode() const {
        return data_;
    }
#ifdef DEBUG
    Register maybeOffsetRegister() const {
        if (data_ & IsImmEDTR)
            return InvalidReg;
        return Register::FromCode(data_ & 0xf);
    }
#endif
};

class VFPOff
{
    uint32_t data_;

  protected:
    explicit VFPOff(datastore::Imm8VFPOffData imm, IsUp_ isup)
      : data_(imm.encode() | uint32_t(isup))
    { }

  public:
    uint32_t encode() const {
        return data_;
    }
};

class VFPOffImm : public VFPOff
{
  public:
    explicit VFPOffImm(int32_t imm)
      : VFPOff(datastore::Imm8VFPOffData(mozilla::Abs(imm) / 4), imm < 0 ? IsDown : IsUp)
    {
        MOZ_ASSERT(mozilla::Abs(imm) <= 255 * 4);
    }
};

class VFPAddr
{
    friend class Operand;

    uint32_t data_;

  public:
    explicit VFPAddr(Register base, VFPOff off)
      : data_(RN(base) | off.encode())
    { }

    uint32_t encode() const {
        return data_;
    }
};

class VFPImm
{
    uint32_t data_;

  public:
    explicit VFPImm(uint32_t topWordOfDouble);

    static const VFPImm One;

    uint32_t encode() const {
        return data_;
    }
    bool isValid() const {
        return data_ != -1U;
    }
};

// A BOffImm is an immediate that is used for branches. Namely, it is the offset
// that will be encoded in the branch instruction. This is the only sane way of
// constructing a branch.
class BOffImm
{
    friend class InstBranchImm;

    uint32_t data_;

  public:
    explicit BOffImm(int offset)
      : data_((offset - 8) >> 2 & 0x00ffffff)
    {
        MOZ_ASSERT((offset & 0x3) == 0);
        if (!IsInRange(offset))
            MOZ_CRASH("BOffImm offset out of range");
    }

    explicit BOffImm()
      : data_(INVALID)
    { }

  private:
    explicit BOffImm(const Instruction& inst);

  public:
    static const uint32_t INVALID = 0x00800000;

    uint32_t encode() const {
        return data_;
    }
    int32_t decode() const {
        return ((int32_t(data_) << 8) >> 6) + 8;
    }

    static bool IsInRange(int offset) {
        if ((offset - 8) < -33554432)
            return false;
        if ((offset - 8) > 33554428)
            return false;
        return true;
    }

    bool isInvalid() const {
        return data_ == INVALID;
    }
    Instruction* getDest(Instruction* src) const;
};

class Imm16
{
    uint32_t lower_ : 12;
    uint32_t pad_ : 4;
    uint32_t upper_ : 4;
    uint32_t invalid_ : 12;

  public:
    explicit Imm16();
    explicit Imm16(uint32_t imm);
    explicit Imm16(Instruction& inst);

    uint32_t encode() const {
        return lower_ | (upper_ << 16);
    }
    uint32_t decode() const {
        return lower_ | (upper_ << 12);
    }

    bool isInvalid() const {
        return invalid_;
    }
};

// I would preffer that these do not exist, since there are essentially no
// instructions that would ever take more than one of these, however, the MIR
// wants to only have one type of arguments to functions, so bugger.
class Operand
{
    // The encoding of registers is the same for OP2, DTR and EDTR yet the type
    // system doesn't let us express this, so choices must be made.
  public:
    enum class Tag : uint8_t {
        OP2,
        MEM,
        FOP
    };

  private:
    Tag tag_ : 8;
    uint32_t reg_ : 5;
    int32_t offset_;

  public:
    explicit Operand(Register reg)
      : tag_(Tag::OP2), reg_(reg.code())
    { }

    explicit Operand(FloatRegister freg)
      : tag_(Tag::FOP), reg_(freg.code())
    { }

    explicit Operand(Register base, Imm32 off)
      : tag_(Tag::MEM), reg_(base.code()), offset_(off.value)
    { }

    explicit Operand(Register base, int32_t off)
      : tag_(Tag::MEM), reg_(base.code()), offset_(off)
    { }

    explicit Operand(const Address& addr)
      : tag_(Tag::MEM), reg_(addr.base.code()), offset_(addr.offset)
    { }

  public:
    Tag tag() const {
        return tag_;
    }

    Operand2 toOp2() const {
        MOZ_ASSERT(tag_ == Tag::OP2);
        return O2Reg(Register::FromCode(reg_));
    }

    Register toReg() const {
        MOZ_ASSERT(tag_ == Tag::OP2);
        return Register::FromCode(reg_);
    }

    Address toAddress() const {
        MOZ_ASSERT(tag_ == Tag::MEM);
        return Address(Register::FromCode(reg_), offset_);
    }
    int32_t disp() const {
        MOZ_ASSERT(tag_ == Tag::MEM);
        return offset_;
    }

    int32_t base() const {
        MOZ_ASSERT(tag_ == Tag::MEM);
        return reg_;
    }
    Register baseReg() const {
        MOZ_ASSERT(tag_ == Tag::MEM);
        return Register::FromCode(reg_);
    }
    DTRAddr toDTRAddr() const {
        MOZ_ASSERT(tag_ == Tag::MEM);
        return DTRAddr(baseReg(), DtrOffImm(offset_));
    }
    VFPAddr toVFPAddr() const {
        MOZ_ASSERT(tag_ == Tag::MEM);
        return VFPAddr(baseReg(), VFPOffImm(offset_));
    }
};

inline Imm32
Imm64::firstHalf() const
{
    return low();
}

inline Imm32
Imm64::secondHalf() const
{
    return hi();
}

void
PatchJump(CodeLocationJump& jump_, CodeLocationLabel label,
          ReprotectCode reprotect = DontReprotect);

static inline void
PatchBackedge(CodeLocationJump& jump_, CodeLocationLabel label, JitZoneGroup::BackedgeTarget target)
{
    PatchJump(jump_, label);
}

class Assembler;
typedef js::jit::AssemblerBufferWithConstantPools<1024, 4, Instruction, Assembler> ARMBuffer;

class Assembler : public AssemblerShared
{
  public:
    // ARM conditional constants:
    enum ARMCondition {
        EQ = 0x00000000, // Zero
        NE = 0x10000000, // Non-zero
        CS = 0x20000000,
        CC = 0x30000000,
        MI = 0x40000000,
        PL = 0x50000000,
        VS = 0x60000000,
        VC = 0x70000000,
        HI = 0x80000000,
        LS = 0x90000000,
        GE = 0xa0000000,
        LT = 0xb0000000,
        GT = 0xc0000000,
        LE = 0xd0000000,
        AL = 0xe0000000
    };

    enum Condition {
        Equal = EQ,
        NotEqual = NE,
        Above = HI,
        AboveOrEqual = CS,
        Below = CC,
        BelowOrEqual = LS,
        GreaterThan = GT,
        GreaterThanOrEqual = GE,
        LessThan = LT,
        LessThanOrEqual = LE,
        Overflow = VS,
        CarrySet = CS,
        CarryClear = CC,
        Signed = MI,
        NotSigned = PL,
        Zero = EQ,
        NonZero = NE,
        Always  = AL,

        VFP_NotEqualOrUnordered = NE,
        VFP_Equal = EQ,
        VFP_Unordered = VS,
        VFP_NotUnordered = VC,
        VFP_GreaterThanOrEqualOrUnordered = CS,
        VFP_GreaterThanOrEqual = GE,
        VFP_GreaterThanOrUnordered = HI,
        VFP_GreaterThan = GT,
        VFP_LessThanOrEqualOrUnordered = LE,
        VFP_LessThanOrEqual = LS,
        VFP_LessThanOrUnordered = LT,
        VFP_LessThan = CC // MI is valid too.
    };

    // Bit set when a DoubleCondition does not map to a single ARM condition.
    // The macro assembler has to special-case these conditions, or else
    // ConditionFromDoubleCondition will complain.
    static const int DoubleConditionBitSpecial = 0x1;

    enum DoubleCondition {
        // These conditions will only evaluate to true if the comparison is
        // ordered - i.e. neither operand is NaN.
        DoubleOrdered = VFP_NotUnordered,
        DoubleEqual = VFP_Equal,
        DoubleNotEqual = VFP_NotEqualOrUnordered | DoubleConditionBitSpecial,
        DoubleGreaterThan = VFP_GreaterThan,
        DoubleGreaterThanOrEqual = VFP_GreaterThanOrEqual,
        DoubleLessThan = VFP_LessThan,
        DoubleLessThanOrEqual = VFP_LessThanOrEqual,
        // If either operand is NaN, these conditions always evaluate to true.
        DoubleUnordered = VFP_Unordered,
        DoubleEqualOrUnordered = VFP_Equal | DoubleConditionBitSpecial,
        DoubleNotEqualOrUnordered = VFP_NotEqualOrUnordered,
        DoubleGreaterThanOrUnordered = VFP_GreaterThanOrUnordered,
        DoubleGreaterThanOrEqualOrUnordered = VFP_GreaterThanOrEqualOrUnordered,
        DoubleLessThanOrUnordered = VFP_LessThanOrUnordered,
        DoubleLessThanOrEqualOrUnordered = VFP_LessThanOrEqualOrUnordered
    };

    Condition getCondition(uint32_t inst) {
        return (Condition) (0xf0000000 & inst);
    }
    static inline Condition ConditionFromDoubleCondition(DoubleCondition cond) {
        MOZ_ASSERT(!(cond & DoubleConditionBitSpecial));
        return static_cast<Condition>(cond);
    }

    enum BarrierOption {
        BarrierSY = 15,         // Full system barrier
        BarrierST = 14          // StoreStore barrier
    };

    // This should be protected, but since CodeGenerator wants to use it, it
    // needs to go out here :(

    BufferOffset nextOffset() {
        return m_buffer.nextOffset();
    }

  protected:
    // Shim around AssemblerBufferWithConstantPools::allocEntry.
    BufferOffset allocLiteralLoadEntry(size_t numInst, unsigned numPoolEntries,
                                       PoolHintPun& php, uint8_t* data,
                                       const LiteralDoc& doc = LiteralDoc(),
                                       ARMBuffer::PoolEntry* pe = nullptr,
                                       bool loadToPC = false);

    Instruction* editSrc(BufferOffset bo) {
        return m_buffer.getInst(bo);
    }

#ifdef JS_DISASM_ARM
    typedef disasm::EmbeddedVector<char, disasm::ReasonableBufferSize> DisasmBuffer;

    static void disassembleInstruction(const Instruction* i, DisasmBuffer& buffer);

    void initDisassembler();
    void finishDisassembler();
    void spew(Instruction* i);
    void spewBranch(Instruction* i, const LabelDoc& target);
    void spewLiteralLoad(PoolHintPun& php, bool loadToPC, const Instruction* offs,
                         const LiteralDoc& doc);
#endif

  public:
    void resetCounter();
    uint32_t actualIndex(uint32_t) const;
    static uint8_t* PatchableJumpAddress(JitCode* code, uint32_t index);
    static uint32_t NopFill;
    static uint32_t GetNopFill();
    static uint32_t AsmPoolMaxOffset;
    static uint32_t GetPoolMaxOffset();

  protected:
    // Structure for fixing up pc-relative loads/jumps when a the machine code
    // gets moved (executable copy, gc, etc.).
    struct RelativePatch
    {
        void* target_;
        Relocation::Kind kind_;

      public:
        RelativePatch(void* target, Relocation::Kind kind)
          : target_(target), kind_(kind)
        { }
        void* target() const { return target_; }
        Relocation::Kind kind() const { return kind_; }
    };

    // TODO: this should actually be a pool-like object. It is currently a big
    // hack, and probably shouldn't exist.
    js::Vector<RelativePatch, 8, SystemAllocPolicy> jumps_;

    CompactBufferWriter jumpRelocations_;
    CompactBufferWriter dataRelocations_;

    ARMBuffer m_buffer;

#ifdef JS_DISASM_ARM
    DisassemblerSpew spew_;
#endif

  public:
    // For the alignment fill use NOP: 0x0320f000 or (Always | InstNOP::NopInst).
    // For the nopFill use a branch to the next instruction: 0xeaffffff.
    Assembler()
      : m_buffer(1, 1, 8, GetPoolMaxOffset(), 8, 0xe320f000, 0xeaffffff, GetNopFill()),
        isFinished(false),
        dtmActive(false),
        dtmCond(Always)
    {
#ifdef JS_DISASM_ARM
        initDisassembler();
#endif
    }

    ~Assembler() {
#ifdef JS_DISASM_ARM
        finishDisassembler();
#endif
    }

    // We need to wait until an AutoJitContextAlloc is created by the
    // MacroAssembler, before allocating any space.
    void initWithAllocator() {
        m_buffer.initWithAllocator();
    }

    static Condition InvertCondition(Condition cond);
    static Condition UnsignedCondition(Condition cond);
    static Condition ConditionWithoutEqual(Condition cond);

    static DoubleCondition InvertCondition(DoubleCondition cond);

    // MacroAssemblers hold onto gcthings, so they are traced by the GC.
    void trace(JSTracer* trc);
    void writeRelocation(BufferOffset src) {
        jumpRelocations_.writeUnsigned(src.getOffset());
    }

    void writeDataRelocation(BufferOffset offset, ImmGCPtr ptr) {
        if (ptr.value) {
            if (gc::IsInsideNursery(ptr.value))
                embedsNurseryPointers_ = true;
            dataRelocations_.writeUnsigned(offset.getOffset());
        }
    }

    enum RelocBranchStyle {
        B_MOVWT,
        B_LDR_BX,
        B_LDR,
        B_MOVW_ADD
    };

    enum RelocStyle {
        L_MOVWT,
        L_LDR
    };

  public:
    // Given the start of a Control Flow sequence, grab the value that is
    // finally branched to given the start of a function that loads an address
    // into a register get the address that ends up in the register.
    template <class Iter>
    static const uint32_t* GetCF32Target(Iter* iter);

    static uintptr_t GetPointer(uint8_t*);
    template <class Iter>
    static const uint32_t* GetPtr32Target(Iter iter, Register* dest = nullptr, RelocStyle* rs = nullptr);

    bool oom() const;

    void setPrinter(Sprinter* sp) {
#ifdef JS_DISASM_ARM
        spew_.setPrinter(sp);
#endif
    }

    static const Register getStackPointer() {
        return StackPointer;
    }

  private:
    bool isFinished;

  protected:
    LabelDoc refLabel(const Label* label) {
#ifdef JS_DISASM_ARM
        return spew_.refLabel(label);
#else
        return LabelDoc();
#endif
    }

  public:
    void finish();
    bool appendRawCode(const uint8_t* code, size_t numBytes);
    bool reserve(size_t size);
    bool swapBuffer(wasm::Bytes& bytes);
    void copyJumpRelocationTable(uint8_t* dest);
    void copyDataRelocationTable(uint8_t* dest);

    // Size of the instruction stream, in bytes, after pools are flushed.
    size_t size() const;
    // Size of the jump relocation table, in bytes.
    size_t jumpRelocationTableBytes() const;
    size_t dataRelocationTableBytes() const;

    // Size of the data table, in bytes.
    size_t bytesNeeded() const;

    // Write a single instruction into the instruction stream.  Very hot,
    // inlined for performance
    MOZ_ALWAYS_INLINE BufferOffset writeInst(uint32_t x) {
        BufferOffset offs = m_buffer.putInt(x);
#ifdef JS_DISASM_ARM
        spew(m_buffer.getInstOrNull(offs));
#endif
        return offs;
    }

    // As above, but also mark the instruction as a branch.  Very hot, inlined
    // for performance
    MOZ_ALWAYS_INLINE BufferOffset
    writeBranchInst(uint32_t x, const LabelDoc& documentation) {
        BufferOffset offs = m_buffer.putInt(x);
#ifdef JS_DISASM_ARM
        spewBranch(m_buffer.getInstOrNull(offs), documentation);
#endif
        return offs;
    }

    // Write a placeholder NOP for a branch into the instruction stream
    // (in order to adjust assembler addresses and mark it as a branch), it will
    // be overwritten subsequently.
    BufferOffset allocBranchInst();

    // A static variant for the cases where we don't want to have an assembler
    // object.
    static void WriteInstStatic(uint32_t x, uint32_t* dest);

  public:
    void writeCodePointer(CodeLabel* label);

    void haltingAlign(int alignment);
    void nopAlign(int alignment);
    BufferOffset as_nop();
    BufferOffset as_alu(Register dest, Register src1, Operand2 op2,
                        ALUOp op, SBit s = LeaveCC, Condition c = Always);
    BufferOffset as_mov(Register dest,
                        Operand2 op2, SBit s = LeaveCC, Condition c = Always);
    BufferOffset as_mvn(Register dest, Operand2 op2,
                        SBit s = LeaveCC, Condition c = Always);

    static void as_alu_patch(Register dest, Register src1, Operand2 op2,
                             ALUOp op, SBit s, Condition c, uint32_t* pos);
    static void as_mov_patch(Register dest,
                             Operand2 op2, SBit s, Condition c, uint32_t* pos);

    // Logical operations:
    BufferOffset as_and(Register dest, Register src1,
                Operand2 op2, SBit s = LeaveCC, Condition c = Always);
    BufferOffset as_bic(Register dest, Register src1,
                Operand2 op2, SBit s = LeaveCC, Condition c = Always);
    BufferOffset as_eor(Register dest, Register src1,
                Operand2 op2, SBit s = LeaveCC, Condition c = Always);
    BufferOffset as_orr(Register dest, Register src1,
                Operand2 op2, SBit s = LeaveCC, Condition c = Always);
    // Mathematical operations:
    BufferOffset as_adc(Register dest, Register src1,
                Operand2 op2, SBit s = LeaveCC, Condition c = Always);
    BufferOffset as_add(Register dest, Register src1,
                Operand2 op2, SBit s = LeaveCC, Condition c = Always);
    BufferOffset as_sbc(Register dest, Register src1,
                Operand2 op2, SBit s = LeaveCC, Condition c = Always);
    BufferOffset as_sub(Register dest, Register src1,
                Operand2 op2, SBit s = LeaveCC, Condition c = Always);
    BufferOffset as_rsb(Register dest, Register src1,
                Operand2 op2, SBit s = LeaveCC, Condition c = Always);
    BufferOffset as_rsc(Register dest, Register src1,
                Operand2 op2, SBit s = LeaveCC, Condition c = Always);
    // Test operations:
    BufferOffset as_cmn(Register src1, Operand2 op2, Condition c = Always);
    BufferOffset as_cmp(Register src1, Operand2 op2, Condition c = Always);
    BufferOffset as_teq(Register src1, Operand2 op2, Condition c = Always);
    BufferOffset as_tst(Register src1, Operand2 op2, Condition c = Always);

    // Sign extension operations:
    BufferOffset as_sxtb(Register dest, Register src, int rotate, Condition c = Always);
    BufferOffset as_sxth(Register dest, Register src, int rotate, Condition c = Always);
    BufferOffset as_uxtb(Register dest, Register src, int rotate, Condition c = Always);
    BufferOffset as_uxth(Register dest, Register src, int rotate, Condition c = Always);

    // Not quite ALU worthy, but useful none the less: These also have the issue
    // of these being formatted completly differently from the standard ALU operations.
    BufferOffset as_movw(Register dest, Imm16 imm, Condition c = Always);
    BufferOffset as_movt(Register dest, Imm16 imm, Condition c = Always);

    static void as_movw_patch(Register dest, Imm16 imm, Condition c, Instruction* pos);
    static void as_movt_patch(Register dest, Imm16 imm, Condition c, Instruction* pos);

    BufferOffset as_genmul(Register d1, Register d2, Register rm, Register rn,
                   MULOp op, SBit s, Condition c = Always);
    BufferOffset as_mul(Register dest, Register src1, Register src2,
                SBit s = LeaveCC, Condition c = Always);
    BufferOffset as_mla(Register dest, Register acc, Register src1, Register src2,
                SBit s = LeaveCC, Condition c = Always);
    BufferOffset as_umaal(Register dest1, Register dest2, Register src1, Register src2,
                  Condition c = Always);
    BufferOffset as_mls(Register dest, Register acc, Register src1, Register src2,
                Condition c = Always);
    BufferOffset as_umull(Register dest1, Register dest2, Register src1, Register src2,
                SBit s = LeaveCC, Condition c = Always);
    BufferOffset as_umlal(Register dest1, Register dest2, Register src1, Register src2,
                SBit s = LeaveCC, Condition c = Always);
    BufferOffset as_smull(Register dest1, Register dest2, Register src1, Register src2,
                SBit s = LeaveCC, Condition c = Always);
    BufferOffset as_smlal(Register dest1, Register dest2, Register src1, Register src2,
                SBit s = LeaveCC, Condition c = Always);

    BufferOffset as_sdiv(Register dest, Register num, Register div, Condition c = Always);
    BufferOffset as_udiv(Register dest, Register num, Register div, Condition c = Always);
    BufferOffset as_clz(Register dest, Register src, Condition c = Always);

    // Data transfer instructions: ldr, str, ldrb, strb.
    // Using an int to differentiate between 8 bits and 32 bits is overkill.
    BufferOffset as_dtr(LoadStore ls, int size, Index mode,
                        Register rt, DTRAddr addr, Condition c = Always);

    static void as_dtr_patch(LoadStore ls, int size, Index mode,
                             Register rt, DTRAddr addr, Condition c, uint32_t* dest);

    // Handles all of the other integral data transferring functions:
    // ldrsb, ldrsh, ldrd, etc. The size is given in bits.
    BufferOffset as_extdtr(LoadStore ls, int size, bool IsSigned, Index mode,
                           Register rt, EDtrAddr addr, Condition c = Always);

    BufferOffset as_dtm(LoadStore ls, Register rn, uint32_t mask,
                DTMMode mode, DTMWriteBack wb, Condition c = Always);

    // Overwrite a pool entry with new data.
    static void WritePoolEntry(Instruction* addr, Condition c, uint32_t data);

    // Load a 32 bit immediate from a pool into a register.
    BufferOffset as_Imm32Pool(Register dest, uint32_t value, Condition c = Always);
    // Make a patchable jump that can target the entire 32 bit address space.
    BufferOffset as_BranchPool(uint32_t value, RepatchLabel* label,
                               const LabelDoc& documentation,
                               ARMBuffer::PoolEntry* pe = nullptr, Condition c = Always);

    // Load a 64 bit floating point immediate from a pool into a register.
    BufferOffset as_FImm64Pool(VFPRegister dest, double value, Condition c = Always);
    // Load a 32 bit floating point immediate from a pool into a register.
    BufferOffset as_FImm32Pool(VFPRegister dest, float value, Condition c = Always);

    // Atomic instructions: ldrexd, ldrex, ldrexh, ldrexb, strexd, strex, strexh, strexb.
    //
    // The doubleword, halfword, and byte versions are available from ARMv6K forward.
    //
    // The word versions are available from ARMv6 forward and can be used to
    // implement the halfword and byte versions on older systems.

    // LDREXD rt, rt2, [rn].  Constraint: rt even register, rt2=rt+1.
    BufferOffset as_ldrexd(Register rt, Register rt2, Register rn, Condition c = Always);

    // LDREX rt, [rn]
    BufferOffset as_ldrex(Register rt, Register rn, Condition c = Always);
    BufferOffset as_ldrexh(Register rt, Register rn, Condition c = Always);
    BufferOffset as_ldrexb(Register rt, Register rn, Condition c = Always);

    // STREXD rd, rt, rt2, [rn].  Constraint: rt even register, rt2=rt+1.
    BufferOffset as_strexd(Register rd, Register rt, Register rt2, Register rn, Condition c = Always);

    // STREX rd, rt, [rn].  Constraint: rd != rn, rd != rt.
    BufferOffset as_strex(Register rd, Register rt, Register rn, Condition c = Always);
    BufferOffset as_strexh(Register rd, Register rt, Register rn, Condition c = Always);
    BufferOffset as_strexb(Register rd, Register rt, Register rn, Condition c = Always);

    // CLREX
    BufferOffset as_clrex();

    // Memory synchronization.
    // These are available from ARMv7 forward.
    BufferOffset as_dmb(BarrierOption option = BarrierSY);
    BufferOffset as_dsb(BarrierOption option = BarrierSY);
    BufferOffset as_isb();

    // Memory synchronization for architectures before ARMv7.
    BufferOffset as_dsb_trap();
    BufferOffset as_dmb_trap();
    BufferOffset as_isb_trap();

    // Speculation barrier
    BufferOffset as_csdb();

    // Control flow stuff:

    // bx can *only* branch to a register never to an immediate.
    BufferOffset as_bx(Register r, Condition c = Always);

    // Branch can branch to an immediate *or* to a register. Branches to
    // immediates are pc relative, branches to registers are absolute.
    BufferOffset as_b(BOffImm off, Condition c, Label* documentation = nullptr);

    BufferOffset as_b(Label* l, Condition c = Always);
    BufferOffset as_b(wasm::OldTrapDesc target, Condition c = Always);
    BufferOffset as_b(BOffImm off, Condition c, BufferOffset inst);

    // blx can go to either an immediate or a register. When blx'ing to a
    // register, we change processor mode depending on the low bit of the
    // register when blx'ing to an immediate, we *always* change processor
    // state.
    BufferOffset as_blx(Label* l);

    BufferOffset as_blx(Register r, Condition c = Always);
    BufferOffset as_bl(BOffImm off, Condition c, Label* documentation = nullptr);
    // bl can only branch+link to an immediate, never to a register it never
    // changes processor state.
    BufferOffset as_bl();
    // bl #imm can have a condition code, blx #imm cannot.
    // blx reg can be conditional.
    BufferOffset as_bl(Label* l, Condition c);
    BufferOffset as_bl(BOffImm off, Condition c, BufferOffset inst);

    BufferOffset as_mrs(Register r, Condition c = Always);
    BufferOffset as_msr(Register r, Condition c = Always);

    // VFP instructions!
  private:
    enum vfp_size {
        IsDouble = 1 << 8,
        IsSingle = 0 << 8
    };

    BufferOffset writeVFPInst(vfp_size sz, uint32_t blob);

    static void WriteVFPInstStatic(vfp_size sz, uint32_t blob, uint32_t* dest);

    // Unityped variants: all registers hold the same (ieee754 single/double)
    // notably not included are vcvt; vmov vd, #imm; vmov rt, vn.
    BufferOffset as_vfp_float(VFPRegister vd, VFPRegister vn, VFPRegister vm,
                              VFPOp op, Condition c = Always);

  public:
    BufferOffset as_vadd(VFPRegister vd, VFPRegister vn, VFPRegister vm, Condition c = Always);
    BufferOffset as_vdiv(VFPRegister vd, VFPRegister vn, VFPRegister vm, Condition c = Always);
    BufferOffset as_vmul(VFPRegister vd, VFPRegister vn, VFPRegister vm, Condition c = Always);
    BufferOffset as_vnmul(VFPRegister vd, VFPRegister vn, VFPRegister vm, Condition c = Always);
    BufferOffset as_vnmla(VFPRegister vd, VFPRegister vn, VFPRegister vm, Condition c = Always);
    BufferOffset as_vnmls(VFPRegister vd, VFPRegister vn, VFPRegister vm, Condition c = Always);
    BufferOffset as_vneg(VFPRegister vd, VFPRegister vm, Condition c = Always);
    BufferOffset as_vsqrt(VFPRegister vd, VFPRegister vm, Condition c = Always);
    BufferOffset as_vabs(VFPRegister vd, VFPRegister vm, Condition c = Always);
    BufferOffset as_vsub(VFPRegister vd, VFPRegister vn, VFPRegister vm, Condition c = Always);
    BufferOffset as_vcmp(VFPRegister vd, VFPRegister vm, Condition c = Always);
    BufferOffset as_vcmpz(VFPRegister vd,  Condition c = Always);

    // Specifically, a move between two same sized-registers.
    BufferOffset as_vmov(VFPRegister vd, VFPRegister vsrc, Condition c = Always);

    // Transfer between Core and VFP.
    enum FloatToCore_ {
        FloatToCore = 1 << 20,
        CoreToFloat = 0 << 20
    };

  private:
    enum VFPXferSize {
        WordTransfer   = 0x02000010,
        DoubleTransfer = 0x00400010
    };

  public:
    // Unlike the next function, moving between the core registers and vfp
    // registers can't be *that* properly typed. Namely, since I don't want to
    // munge the type VFPRegister to also include core registers. Thus, the core
    // and vfp registers are passed in based on their type, and src/dest is
    // determined by the float2core.

    BufferOffset as_vxfer(Register vt1, Register vt2, VFPRegister vm, FloatToCore_ f2c,
                  Condition c = Always, int idx = 0);

    // Our encoding actually allows just the src and the dest (and their types)
    // to uniquely specify the encoding that we are going to use.
    BufferOffset as_vcvt(VFPRegister vd, VFPRegister vm, bool useFPSCR = false,
                         Condition c = Always);

    // Hard coded to a 32 bit fixed width result for now.
    BufferOffset as_vcvtFixed(VFPRegister vd, bool isSigned, uint32_t fixedPoint,
                              bool toFixed, Condition c = Always);

    // Transfer between VFP and memory.
    BufferOffset as_vdtr(LoadStore ls, VFPRegister vd, VFPAddr addr,
                         Condition c = Always /* vfp doesn't have a wb option*/);

    static void as_vdtr_patch(LoadStore ls, VFPRegister vd, VFPAddr addr,
                              Condition c /* vfp doesn't have a wb option */, uint32_t* dest);

    // VFP's ldm/stm work differently from the standard arm ones. You can only
    // transfer a range.

    BufferOffset as_vdtm(LoadStore st, Register rn, VFPRegister vd, int length,
                 /* also has update conditions */ Condition c = Always);

    BufferOffset as_vimm(VFPRegister vd, VFPImm imm, Condition c = Always);

    BufferOffset as_vmrs(Register r, Condition c = Always);
    BufferOffset as_vmsr(Register r, Condition c = Always);

    // Label operations.
    bool nextLink(BufferOffset b, BufferOffset* next);
    void bind(Label* label, BufferOffset boff = BufferOffset());
    void bind(RepatchLabel* label);
    void bindLater(Label* label, wasm::OldTrapDesc target);
    uint32_t currentOffset() {
        return nextOffset().getOffset();
    }
    void retarget(Label* label, Label* target);
    // I'm going to pretend this doesn't exist for now.
    void retarget(Label* label, void* target, Relocation::Kind reloc);

    static void Bind(uint8_t* rawCode, const CodeLabel& label);

    void as_bkpt();
    BufferOffset as_illegal_trap();

  public:
    static void TraceJumpRelocations(JSTracer* trc, JitCode* code, CompactBufferReader& reader);
    static void TraceDataRelocations(JSTracer* trc, JitCode* code, CompactBufferReader& reader);

    static bool SupportsFloatingPoint() {
        return HasVFP();
    }
    static bool SupportsUnalignedAccesses() {
        return HasARMv7();
    }
    static bool SupportsSimd() {
        return js::jit::SupportsSimd;
    }

    static bool HasRoundInstruction(RoundingMode mode) { return false; }

  protected:
    void addPendingJump(BufferOffset src, ImmPtr target, Relocation::Kind kind) {
        enoughMemory_ &= jumps_.append(RelativePatch(target.value, kind));
        if (kind == Relocation::JITCODE)
            writeRelocation(src);
    }

  public:
    // The buffer is about to be linked, make sure any constant pools or excess
    // bookkeeping has been flushed to the instruction stream.
    void flush() {
        MOZ_ASSERT(!isFinished);
        m_buffer.flushPool();
        return;
    }

    void comment(const char* msg) {
#ifdef JS_DISASM_ARM
        spew_.spew("; %s", msg);
#endif
    }

    // Copy the assembly code to the given buffer, and perform any pending
    // relocations relying on the target address.
    void executableCopy(uint8_t* buffer, bool flushICache = true);

    // Actual assembly emitting functions.

    // Since I can't think of a reasonable default for the mode, I'm going to
    // leave it as a required argument.
    void startDataTransferM(LoadStore ls, Register rm,
                            DTMMode mode, DTMWriteBack update = NoWriteBack,
                            Condition c = Always)
    {
        MOZ_ASSERT(!dtmActive);
        dtmUpdate = update;
        dtmBase = rm;
        dtmLoadStore = ls;
        dtmLastReg = -1;
        dtmRegBitField = 0;
        dtmActive = 1;
        dtmCond = c;
        dtmMode = mode;
    }

    void transferReg(Register rn) {
        MOZ_ASSERT(dtmActive);
        MOZ_ASSERT(rn.code() > dtmLastReg);
        dtmRegBitField |= 1 << rn.code();
        if (dtmLoadStore == IsLoad && rn.code() == 13 && dtmBase.code() == 13) {
            MOZ_CRASH("ARM Spec says this is invalid");
        }
    }
    void finishDataTransfer() {
        dtmActive = false;
        as_dtm(dtmLoadStore, dtmBase, dtmRegBitField, dtmMode, dtmUpdate, dtmCond);
    }

    void startFloatTransferM(LoadStore ls, Register rm,
                             DTMMode mode, DTMWriteBack update = NoWriteBack,
                             Condition c = Always)
    {
        MOZ_ASSERT(!dtmActive);
        dtmActive = true;
        dtmUpdate = update;
        dtmLoadStore = ls;
        dtmBase = rm;
        dtmCond = c;
        dtmLastReg = -1;
        dtmMode = mode;
        dtmDelta = 0;
    }
    void transferFloatReg(VFPRegister rn)
    {
        if (dtmLastReg == -1) {
            vdtmFirstReg = rn.code();
        } else {
            if (dtmDelta == 0) {
                dtmDelta = rn.code() - dtmLastReg;
                MOZ_ASSERT(dtmDelta == 1 || dtmDelta == -1);
            }
            MOZ_ASSERT(dtmLastReg >= 0);
            MOZ_ASSERT(rn.code() == unsigned(dtmLastReg) + dtmDelta);
        }

        dtmLastReg = rn.code();
    }
    void finishFloatTransfer() {
        MOZ_ASSERT(dtmActive);
        dtmActive = false;
        MOZ_ASSERT(dtmLastReg != -1);
        dtmDelta = dtmDelta ? dtmDelta : 1;
        // The operand for the vstr/vldr instruction is the lowest register in the range.
        int low = Min(dtmLastReg, vdtmFirstReg);
        int high = Max(dtmLastReg, vdtmFirstReg);
        // Fencepost problem.
        int len = high - low + 1;
        // vdtm can only transfer 16 registers at once.  If we need to transfer more,
        // then either hoops are necessary, or we need to be updating the register.
        MOZ_ASSERT_IF(len > 16, dtmUpdate == WriteBack);

        int adjustLow = dtmLoadStore == IsStore ? 0 : 1;
        int adjustHigh = dtmLoadStore == IsStore ? -1 : 0;
        while (len > 0) {
            // Limit the instruction to 16 registers.
            int curLen = Min(len, 16);
            // If it is a store, we want to start at the high end and move down
            // (e.g. vpush d16-d31; vpush d0-d15).
            int curStart = (dtmLoadStore == IsStore) ? high - curLen + 1 : low;
            as_vdtm(dtmLoadStore, dtmBase,
                    VFPRegister(FloatRegister::FromCode(curStart)),
                    curLen, dtmCond);
            // Update the bounds.
            low += adjustLow * curLen;
            high += adjustHigh * curLen;
            // Update the length parameter.
            len -= curLen;
        }
    }

  private:
    int dtmRegBitField;
    int vdtmFirstReg;
    int dtmLastReg;
    int dtmDelta;
    Register dtmBase;
    DTMWriteBack dtmUpdate;
    DTMMode dtmMode;
    LoadStore dtmLoadStore;
    bool dtmActive;
    Condition dtmCond;

  public:
    enum {
        PadForAlign8  = (int)0x00,
        PadForAlign16 = (int)0x0000,
        PadForAlign32 = (int)0xe12fff7f  // 'bkpt 0xffff'
    };

    // API for speaking with the IonAssemblerBufferWithConstantPools generate an
    // initial placeholder instruction that we want to later fix up.
    static void InsertIndexIntoTag(uint8_t* load, uint32_t index);

    // Take the stub value that was written in before, and write in an actual
    // load using the index we'd computed previously as well as the address of
    // the pool start.
    static void PatchConstantPoolLoad(void* loadAddr, void* constPoolAddr);

    // We're not tracking short-range branches for ARM for now.
    static void PatchShortRangeBranchToVeneer(ARMBuffer*, unsigned rangeIdx, BufferOffset deadline,
                                              BufferOffset veneer)
    {
        MOZ_CRASH();
    }
    // END API

    // Move our entire pool into the instruction stream. This is to force an
    // opportunistic dump of the pool, prefferably when it is more convenient to
    // do a dump.
    void flushBuffer();
    void enterNoPool(size_t maxInst);
    void leaveNoPool();
    // This should return a BOffImm, but we didn't want to require everyplace
    // that used the AssemblerBuffer to make that class.
    static ptrdiff_t GetBranchOffset(const Instruction* i);
    static void RetargetNearBranch(Instruction* i, int offset, Condition cond, bool final = true);
    static void RetargetNearBranch(Instruction* i, int offset, bool final = true);
    static void RetargetFarBranch(Instruction* i, uint8_t** slot, uint8_t* dest, Condition cond);

    static void WritePoolHeader(uint8_t* start, Pool* p, bool isNatural);
    static void WritePoolGuard(BufferOffset branch, Instruction* inst, BufferOffset dest);


    static uint32_t PatchWrite_NearCallSize();
    static uint32_t NopSize() { return 4; }
    static void PatchWrite_NearCall(CodeLocationLabel start, CodeLocationLabel toCall);
    static void PatchDataWithValueCheck(CodeLocationLabel label, PatchedImmPtr newValue,
                                        PatchedImmPtr expectedValue);
    static void PatchDataWithValueCheck(CodeLocationLabel label, ImmPtr newValue,
                                        ImmPtr expectedValue);
    static void PatchWrite_Imm32(CodeLocationLabel label, Imm32 imm);

    static uint32_t AlignDoubleArg(uint32_t offset) {
        return (offset + 1) & ~1;
    }
    static uint8_t* NextInstruction(uint8_t* instruction, uint32_t* count = nullptr);

    // Toggle a jmp or cmp emitted by toggledJump().
    static void ToggleToJmp(CodeLocationLabel inst_);
    static void ToggleToCmp(CodeLocationLabel inst_);

    static uint8_t* BailoutTableStart(uint8_t* code);

    static size_t ToggledCallSize(uint8_t* code);
    static void ToggleCall(CodeLocationLabel inst_, bool enabled);

    void processCodeLabels(uint8_t* rawCode);

    bool bailed() {
        return m_buffer.bail();
    }

    void verifyHeapAccessDisassembly(uint32_t begin, uint32_t end,
                                     const Disassembler::HeapAccess& heapAccess)
    {
        // Implement this if we implement a disassembler.
    }
}; // Assembler

// An Instruction is a structure for both encoding and decoding any and all ARM
// instructions. Many classes have not been implemented thus far.
class Instruction
{
    uint32_t data;

  protected:
    // This is not for defaulting to always, this is for instructions that
    // cannot be made conditional, and have the usually invalid 4b1111 cond
    // field.
    explicit Instruction(uint32_t data_, bool fake = false)
      : data(data_ | 0xf0000000)
    {
        MOZ_ASSERT(fake || ((data_ & 0xf0000000) == 0));
    }
    // Standard constructor.
    Instruction(uint32_t data_, Assembler::Condition c)
      : data(data_ | (uint32_t) c)
    {
        MOZ_ASSERT((data_ & 0xf0000000) == 0);
    }
    // You should never create an instruction directly. You should create a more
    // specific instruction which will eventually call one of these constructors
    // for you.
  public:
    uint32_t encode() const {
        return data;
    }
    // Check if this instruction is really a particular case.
    template <class C>
    bool is() const { return C::IsTHIS(*this); }

    // Safely get a more specific variant of this pointer.
    template <class C>
    C* as() const { return C::AsTHIS(*this); }

    const Instruction& operator=(Instruction src) {
        data = src.data;
        return *this;
    }
    // Since almost all instructions have condition codes, the condition code
    // extractor resides in the base class.
    Assembler::Condition extractCond() const {
        MOZ_ASSERT(data >> 28 != 0xf, "The instruction does not have condition code");
        return (Assembler::Condition)(data & 0xf0000000);
    }

    // Sometimes, an api wants a uint32_t (or a pointer to it) rather than an
    // instruction. raw() just coerces this into a pointer to a uint32_t.
    const uint32_t* raw() const { return &data; }
    uint32_t size() const { return 4; }
}; // Instruction

// Make sure that it is the right size.
JS_STATIC_ASSERT(sizeof(Instruction) == 4);

// Data Transfer Instructions.
class InstDTR : public Instruction
{
  public:
    enum IsByte_ {
        IsByte = 0x00400000,
        IsWord = 0x00000000
    };
    static const int IsDTR     = 0x04000000;
    static const int IsDTRMask = 0x0c000000;

    // TODO: Replace the initialization with something that is safer.
    InstDTR(LoadStore ls, IsByte_ ib, Index mode, Register rt, DTRAddr addr, Assembler::Condition c)
      : Instruction(ls | ib | mode | RT(rt) | addr.encode() | IsDTR, c)
    { }

    static bool IsTHIS(const Instruction& i);
    static InstDTR* AsTHIS(const Instruction& i);

};
JS_STATIC_ASSERT(sizeof(InstDTR) == sizeof(Instruction));

class InstLDR : public InstDTR
{
  public:
    InstLDR(Index mode, Register rt, DTRAddr addr, Assembler::Condition c)
      : InstDTR(IsLoad, IsWord, mode, rt, addr, c)
    { }

    static bool IsTHIS(const Instruction& i);
    static InstLDR* AsTHIS(const Instruction& i);

    int32_t signedOffset() const {
        int32_t offset = encode() & 0xfff;
        if (IsUp_(encode() & IsUp) != IsUp)
            return -offset;
        return offset;
    }
    uint32_t* dest() const {
        int32_t offset = signedOffset();
        // When patching the load in PatchConstantPoolLoad, we ensure that the
        // offset is a multiple of 4, offset by 8 bytes from the actual
        // location.  Indeed, when the base register is PC, ARM's 3 stages
        // pipeline design makes it that PC is off by 8 bytes (= 2 *
        // sizeof(uint32*)) when we actually executed it.
        MOZ_ASSERT(offset % 4 == 0);
        offset >>= 2;
        return (uint32_t*)raw() + offset + 2;
    }
};
JS_STATIC_ASSERT(sizeof(InstDTR) == sizeof(InstLDR));

class InstNOP : public Instruction
{
  public:
    static const uint32_t NopInst = 0x0320f000;

    InstNOP()
      : Instruction(NopInst, Assembler::Always)
    { }

    static bool IsTHIS(const Instruction& i);
    static InstNOP* AsTHIS(Instruction& i);
};

// Branching to a register, or calling a register
class InstBranchReg : public Instruction
{
  protected:
    // Don't use BranchTag yourself, use a derived instruction.
    enum BranchTag {
        IsBX  = 0x012fff10,
        IsBLX = 0x012fff30
    };

    static const uint32_t IsBRegMask = 0x0ffffff0;

    InstBranchReg(BranchTag tag, Register rm, Assembler::Condition c)
      : Instruction(tag | rm.code(), c)
    { }

  public:
    static bool IsTHIS (const Instruction& i);
    static InstBranchReg* AsTHIS (const Instruction& i);

    // Get the register that is being branched to
    void extractDest(Register* dest);
    // Make sure we are branching to a pre-known register
    bool checkDest(Register dest);
};
JS_STATIC_ASSERT(sizeof(InstBranchReg) == sizeof(Instruction));

// Branching to an immediate offset, or calling an immediate offset
class InstBranchImm : public Instruction
{
  protected:
    enum BranchTag {
        IsB   = 0x0a000000,
        IsBL  = 0x0b000000
    };

    static const uint32_t IsBImmMask = 0x0f000000;

    InstBranchImm(BranchTag tag, BOffImm off, Assembler::Condition c)
      : Instruction(tag | off.encode(), c)
    { }

  public:
    static bool IsTHIS (const Instruction& i);
    static InstBranchImm* AsTHIS (const Instruction& i);

    void extractImm(BOffImm* dest);
};
JS_STATIC_ASSERT(sizeof(InstBranchImm) == sizeof(Instruction));

// Very specific branching instructions.
class InstBXReg : public InstBranchReg
{
  public:
    static bool IsTHIS (const Instruction& i);
    static InstBXReg* AsTHIS (const Instruction& i);
};

class InstBLXReg : public InstBranchReg
{
  public:
    InstBLXReg(Register reg, Assembler::Condition c)
      : InstBranchReg(IsBLX, reg, c)
    { }

    static bool IsTHIS (const Instruction& i);
    static InstBLXReg* AsTHIS (const Instruction& i);
};

class InstBImm : public InstBranchImm
{
  public:
    InstBImm(BOffImm off, Assembler::Condition c)
      : InstBranchImm(IsB, off, c)
    { }

    static bool IsTHIS (const Instruction& i);
    static InstBImm* AsTHIS (const Instruction& i);
};

class InstBLImm : public InstBranchImm
{
  public:
    InstBLImm(BOffImm off, Assembler::Condition c)
      : InstBranchImm(IsBL, off, c)
    { }

    static bool IsTHIS (const Instruction& i);
    static InstBLImm* AsTHIS (const Instruction& i);
};

// Both movw and movt. The layout of both the immediate and the destination
// register is the same so the code is being shared.
class InstMovWT : public Instruction
{
  protected:
    enum WT {
        IsW = 0x03000000,
        IsT = 0x03400000
    };
    static const uint32_t IsWTMask = 0x0ff00000;

    InstMovWT(Register rd, Imm16 imm, WT wt, Assembler::Condition c)
      : Instruction (RD(rd) | imm.encode() | wt, c)
    { }

  public:
    void extractImm(Imm16* dest);
    void extractDest(Register* dest);
    bool checkImm(Imm16 dest);
    bool checkDest(Register dest);

    static bool IsTHIS (Instruction& i);
    static InstMovWT* AsTHIS (Instruction& i);

};
JS_STATIC_ASSERT(sizeof(InstMovWT) == sizeof(Instruction));

class InstMovW : public InstMovWT
{
  public:
    InstMovW (Register rd, Imm16 imm, Assembler::Condition c)
      : InstMovWT(rd, imm, IsW, c)
    { }

    static bool IsTHIS (const Instruction& i);
    static InstMovW* AsTHIS (const Instruction& i);
};

class InstMovT : public InstMovWT
{
  public:
    InstMovT (Register rd, Imm16 imm, Assembler::Condition c)
      : InstMovWT(rd, imm, IsT, c)
    { }

    static bool IsTHIS (const Instruction& i);
    static InstMovT* AsTHIS (const Instruction& i);
};

class InstALU : public Instruction
{
    static const int32_t ALUMask = 0xc << 24;

  public:
    InstALU(Register rd, Register rn, Operand2 op2, ALUOp op, SBit s, Assembler::Condition c)
      : Instruction(maybeRD(rd) | maybeRN(rn) | op2.encode() | op | s, c)
    { }

    static bool IsTHIS (const Instruction& i);
    static InstALU* AsTHIS (const Instruction& i);

    void extractOp(ALUOp* ret);
    bool checkOp(ALUOp op);
    void extractDest(Register* ret);
    bool checkDest(Register rd);
    void extractOp1(Register* ret);
    bool checkOp1(Register rn);
    Operand2 extractOp2();
};

class InstCMP : public InstALU
{
  public:
    static bool IsTHIS (const Instruction& i);
    static InstCMP* AsTHIS (const Instruction& i);
};

class InstMOV : public InstALU
{
  public:
    static bool IsTHIS (const Instruction& i);
    static InstMOV* AsTHIS (const Instruction& i);
};

class InstructionIterator
{
  private:
    Instruction* inst_;

  public:
    explicit InstructionIterator(Instruction* inst)
      : inst_(inst)
    {
        maybeSkipAutomaticInstructions();
    }

    // Advances to the next intentionally-inserted instruction.
    Instruction* next();

    // Advances past any automatically-inserted instructions.
    Instruction* maybeSkipAutomaticInstructions();

    Instruction* cur() const {
        return inst_;
    }

  protected:
    // Advances past the given number of instruction-length bytes.
    void advanceRaw(ptrdiff_t instructions = 1) {
        inst_ = inst_ + instructions;
    }

    // Look ahead, including automatically-inserted instructions
    // and PoolHeaders.
    Instruction* peekRaw(ptrdiff_t instructions = 1) const {
        return inst_ + instructions;
    }
};

// Compile-time iterator over instructions, with a safe interface that
// references not-necessarily-linear Instructions by linear BufferOffset.
class BufferInstructionIterator : public ARMBuffer::AssemblerBufferInstIterator
{
  public:
    BufferInstructionIterator(BufferOffset bo, ARMBuffer* buffer)
      : ARMBuffer::AssemblerBufferInstIterator(bo, buffer)
    {}

    // Advances the buffer to the next intentionally-inserted instruction.
    Instruction* next() {
        advance(cur()->size());
        maybeSkipAutomaticInstructions();
        return cur();
    }

    // Advances the BufferOffset past any automatically-inserted instructions.
    Instruction* maybeSkipAutomaticInstructions();
};

static const uint32_t NumIntArgRegs = 4;

// There are 16 *float* registers available for arguments
// If doubles are used, only half the number of registers are available.
static const uint32_t NumFloatArgRegs = 16;

static inline bool
GetIntArgReg(uint32_t usedIntArgs, uint32_t usedFloatArgs, Register* out)
{
    if (usedIntArgs >= NumIntArgRegs)
        return false;

    *out = Register::FromCode(usedIntArgs);
    return true;
}

// Get a register in which we plan to put a quantity that will be used as an
// integer argument. This differs from GetIntArgReg in that if we have no more
// actual argument registers to use we will fall back on using whatever
// CallTempReg* don't overlap the argument registers, and only fail once those
// run out too.
static inline bool
GetTempRegForIntArg(uint32_t usedIntArgs, uint32_t usedFloatArgs, Register* out)
{
    if (GetIntArgReg(usedIntArgs, usedFloatArgs, out))
        return true;

    // Unfortunately, we have to assume things about the point at which
    // GetIntArgReg returns false, because we need to know how many registers it
    // can allocate.
    usedIntArgs -= NumIntArgRegs;
    if (usedIntArgs >= NumCallTempNonArgRegs)
        return false;

    *out = CallTempNonArgRegs[usedIntArgs];
    return true;
}


#if !defined(JS_CODEGEN_ARM_HARDFP) || defined(JS_SIMULATOR_ARM)

static inline uint32_t
GetArgStackDisp(uint32_t arg)
{
    MOZ_ASSERT(!UseHardFpABI());
    MOZ_ASSERT(arg >= NumIntArgRegs);
    return (arg - NumIntArgRegs) * sizeof(intptr_t);
}

#endif


#if defined(JS_CODEGEN_ARM_HARDFP) || defined(JS_SIMULATOR_ARM)

static inline bool
GetFloat32ArgReg(uint32_t usedIntArgs, uint32_t usedFloatArgs, FloatRegister* out)
{
    MOZ_ASSERT(UseHardFpABI());
    if (usedFloatArgs >= NumFloatArgRegs)
        return false;
    *out = VFPRegister(usedFloatArgs, VFPRegister::Single);
    return true;
}
static inline bool
GetDoubleArgReg(uint32_t usedIntArgs, uint32_t usedFloatArgs, FloatRegister* out)
{
    MOZ_ASSERT(UseHardFpABI());
    MOZ_ASSERT((usedFloatArgs % 2) == 0);
    if (usedFloatArgs >= NumFloatArgRegs)
        return false;
    *out = VFPRegister(usedFloatArgs>>1, VFPRegister::Double);
    return true;
}

static inline uint32_t
GetIntArgStackDisp(uint32_t usedIntArgs, uint32_t usedFloatArgs, uint32_t* padding)
{
    MOZ_ASSERT(UseHardFpABI());
    MOZ_ASSERT(usedIntArgs >= NumIntArgRegs);
    uint32_t doubleSlots = Max(0, (int32_t)usedFloatArgs - (int32_t)NumFloatArgRegs);
    doubleSlots *= 2;
    int intSlots = usedIntArgs - NumIntArgRegs;
    return (intSlots + doubleSlots + *padding) * sizeof(intptr_t);
}

static inline uint32_t
GetFloat32ArgStackDisp(uint32_t usedIntArgs, uint32_t usedFloatArgs, uint32_t* padding)
{
    MOZ_ASSERT(UseHardFpABI());
    MOZ_ASSERT(usedFloatArgs >= NumFloatArgRegs);
    uint32_t intSlots = 0;
    if (usedIntArgs > NumIntArgRegs)
        intSlots = usedIntArgs - NumIntArgRegs;
    uint32_t float32Slots = usedFloatArgs - NumFloatArgRegs;
    return (intSlots + float32Slots + *padding) * sizeof(intptr_t);
}

static inline uint32_t
GetDoubleArgStackDisp(uint32_t usedIntArgs, uint32_t usedFloatArgs, uint32_t* padding)
{
    MOZ_ASSERT(UseHardFpABI());
    MOZ_ASSERT(usedFloatArgs >= NumFloatArgRegs);
    uint32_t intSlots = 0;
    if (usedIntArgs > NumIntArgRegs) {
        intSlots = usedIntArgs - NumIntArgRegs;
        // Update the amount of padding required.
        *padding += (*padding + usedIntArgs) % 2;
    }
    uint32_t doubleSlots = usedFloatArgs - NumFloatArgRegs;
    doubleSlots *= 2;
    return (intSlots + doubleSlots + *padding) * sizeof(intptr_t);
}

#endif

class DoubleEncoder
{
    struct DoubleEntry
    {
        uint32_t dblTop;
        datastore::Imm8VFPImmData data;
    };

    static const DoubleEntry table[256];

  public:
    bool lookup(uint32_t top, datastore::Imm8VFPImmData* ret) const {
        for (int i = 0; i < 256; i++) {
            if (table[i].dblTop == top) {
                *ret = table[i].data;
                return true;
            }
        }
        return false;
    }
};

class AutoForbidPools
{
    Assembler* masm_;

  public:
    // The maxInst argument is the maximum number of word sized instructions
    // that will be allocated within this context. It is used to determine if
    // the pool needs to be dumped before entering this content. The debug code
    // checks that no more than maxInst instructions are actually allocated.
    //
    // Allocation of pool entries is not supported within this content so the
    // code can not use large integers or float constants etc.
    AutoForbidPools(Assembler* masm, size_t maxInst)
      : masm_(masm)
    {
        masm_->enterNoPool(maxInst);
    }

    ~AutoForbidPools() {
        masm_->leaveNoPool();
    }
};

} // namespace jit
} // namespace js

#endif /* jit_arm_Assembler_arm_h */

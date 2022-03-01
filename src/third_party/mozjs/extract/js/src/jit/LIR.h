/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_LIR_h
#define jit_LIR_h

// This file declares the core data structures for LIR: storage allocations for
// inputs and outputs, as well as the interface instructions must conform to.

#include "mozilla/Array.h"
#include "mozilla/Casting.h"

#include "jit/Bailouts.h"
#include "jit/FixedList.h"
#include "jit/InlineList.h"
#include "jit/JitAllocPolicy.h"
#include "jit/LOpcodesGenerated.h"
#include "jit/MIR.h"
#include "jit/MIRGraph.h"
#include "jit/Registers.h"
#include "jit/Safepoints.h"
#include "util/Memory.h"

namespace js {
namespace jit {

class LUse;
class LGeneralReg;
class LFloatReg;
class LStackSlot;
class LStackArea;
class LArgument;
class LConstantIndex;
class LInstruction;
class LDefinition;
class MBasicBlock;
class MIRGenerator;

static const uint32_t VREG_INCREMENT = 1;

static const uint32_t THIS_FRAME_ARGSLOT = 0;

#if defined(JS_NUNBOX32)
#  define BOX_PIECES 2
static const uint32_t VREG_TYPE_OFFSET = 0;
static const uint32_t VREG_DATA_OFFSET = 1;
static const uint32_t TYPE_INDEX = 0;
static const uint32_t PAYLOAD_INDEX = 1;
static const uint32_t INT64LOW_INDEX = 0;
static const uint32_t INT64HIGH_INDEX = 1;
#elif defined(JS_PUNBOX64)
#  define BOX_PIECES 1
#else
#  error "Unknown!"
#endif

static const uint32_t INT64_PIECES = sizeof(int64_t) / sizeof(uintptr_t);

// Represents storage for an operand. For constants, the pointer is tagged
// with a single bit, and the untagged pointer is a pointer to a Value.
class LAllocation : public TempObject {
  uintptr_t bits_;

  // 3 bits gives us enough for an interesting set of Kinds and also fits
  // within the alignment bits of pointers to Value, which are always
  // 8-byte aligned.
  static const uintptr_t KIND_BITS = 3;
  static const uintptr_t KIND_SHIFT = 0;
  static const uintptr_t KIND_MASK = (1 << KIND_BITS) - 1;

 protected:
  static const uintptr_t DATA_BITS = (sizeof(uint32_t) * 8) - KIND_BITS;
  static const uintptr_t DATA_SHIFT = KIND_SHIFT + KIND_BITS;

 public:
  enum Kind {
    CONSTANT_VALUE,  // MConstant*.
    CONSTANT_INDEX,  // Constant arbitrary index.
    USE,         // Use of a virtual register, with physical allocation policy.
    GPR,         // General purpose register.
    FPU,         // Floating-point register.
    STACK_SLOT,  // Stack slot.
    STACK_AREA,  // Stack area.
    ARGUMENT_SLOT  // Argument slot.
  };

  static const uintptr_t DATA_MASK = (1 << DATA_BITS) - 1;

 protected:
  uint32_t data() const {
    MOZ_ASSERT(!hasIns());
    return mozilla::AssertedCast<uint32_t>(bits_ >> DATA_SHIFT);
  }
  void setData(uintptr_t data) {
    MOZ_ASSERT(!hasIns());
    MOZ_ASSERT(data <= DATA_MASK);
    bits_ &= ~(DATA_MASK << DATA_SHIFT);
    bits_ |= (data << DATA_SHIFT);
  }
  void setKindAndData(Kind kind, uintptr_t data) {
    MOZ_ASSERT(data <= DATA_MASK);
    bits_ = (uintptr_t(kind) << KIND_SHIFT) | data << DATA_SHIFT;
    MOZ_ASSERT(!hasIns());
  }

  bool hasIns() const { return isStackArea(); }
  const LInstruction* ins() const {
    MOZ_ASSERT(hasIns());
    return reinterpret_cast<const LInstruction*>(bits_ &
                                                 ~(KIND_MASK << KIND_SHIFT));
  }
  LInstruction* ins() {
    MOZ_ASSERT(hasIns());
    return reinterpret_cast<LInstruction*>(bits_ & ~(KIND_MASK << KIND_SHIFT));
  }
  void setKindAndIns(Kind kind, LInstruction* ins) {
    uintptr_t data = reinterpret_cast<uintptr_t>(ins);
    MOZ_ASSERT((data & (KIND_MASK << KIND_SHIFT)) == 0);
    bits_ = data | (uintptr_t(kind) << KIND_SHIFT);
    MOZ_ASSERT(hasIns());
  }

  LAllocation(Kind kind, uintptr_t data) { setKindAndData(kind, data); }
  LAllocation(Kind kind, LInstruction* ins) { setKindAndIns(kind, ins); }
  explicit LAllocation(Kind kind) { setKindAndData(kind, 0); }

 public:
  LAllocation() : bits_(0) { MOZ_ASSERT(isBogus()); }

  // The MConstant pointer must have its low bits cleared.
  explicit LAllocation(const MConstant* c) {
    MOZ_ASSERT(c);
    bits_ = uintptr_t(c);
    MOZ_ASSERT((bits_ & (KIND_MASK << KIND_SHIFT)) == 0);
    bits_ |= CONSTANT_VALUE << KIND_SHIFT;
  }
  inline explicit LAllocation(AnyRegister reg);

  Kind kind() const { return (Kind)((bits_ >> KIND_SHIFT) & KIND_MASK); }

  bool isBogus() const { return bits_ == 0; }
  bool isUse() const { return kind() == USE; }
  bool isConstant() const { return isConstantValue() || isConstantIndex(); }
  bool isConstantValue() const { return kind() == CONSTANT_VALUE; }
  bool isConstantIndex() const { return kind() == CONSTANT_INDEX; }
  bool isGeneralReg() const { return kind() == GPR; }
  bool isFloatReg() const { return kind() == FPU; }
  bool isStackSlot() const { return kind() == STACK_SLOT; }
  bool isStackArea() const { return kind() == STACK_AREA; }
  bool isArgument() const { return kind() == ARGUMENT_SLOT; }
  bool isRegister() const { return isGeneralReg() || isFloatReg(); }
  bool isRegister(bool needFloat) const {
    return needFloat ? isFloatReg() : isGeneralReg();
  }
  bool isMemory() const { return isStackSlot() || isArgument(); }
  inline uint32_t memorySlot() const;
  inline LUse* toUse();
  inline const LUse* toUse() const;
  inline const LGeneralReg* toGeneralReg() const;
  inline const LFloatReg* toFloatReg() const;
  inline const LStackSlot* toStackSlot() const;
  inline LStackArea* toStackArea();
  inline const LStackArea* toStackArea() const;
  inline const LArgument* toArgument() const;
  inline const LConstantIndex* toConstantIndex() const;
  inline AnyRegister toRegister() const;

  const MConstant* toConstant() const {
    MOZ_ASSERT(isConstantValue());
    return reinterpret_cast<const MConstant*>(bits_ &
                                              ~(KIND_MASK << KIND_SHIFT));
  }

  bool operator==(const LAllocation& other) const {
    return bits_ == other.bits_;
  }

  bool operator!=(const LAllocation& other) const {
    return bits_ != other.bits_;
  }

  HashNumber hash() const { return bits_; }

  bool aliases(const LAllocation& other) const;

#ifdef JS_JITSPEW
  UniqueChars toString() const;
  void dump() const;
#endif
};

class LUse : public LAllocation {
  static const uint32_t POLICY_BITS = 3;
  static const uint32_t POLICY_SHIFT = 0;
  static const uint32_t POLICY_MASK = (1 << POLICY_BITS) - 1;
#ifdef JS_CODEGEN_ARM64
  static const uint32_t REG_BITS = 7;
#else
  static const uint32_t REG_BITS = 6;
#endif
  static const uint32_t REG_SHIFT = POLICY_SHIFT + POLICY_BITS;
  static const uint32_t REG_MASK = (1 << REG_BITS) - 1;

  // Whether the physical register for this operand may be reused for a def.
  static const uint32_t USED_AT_START_BITS = 1;
  static const uint32_t USED_AT_START_SHIFT = REG_SHIFT + REG_BITS;
  static const uint32_t USED_AT_START_MASK = (1 << USED_AT_START_BITS) - 1;

  // The REG field will hold the register code for any Register or
  // FloatRegister, though not for an AnyRegister.
  static_assert(std::max(Registers::Total, FloatRegisters::Total) <=
                    REG_MASK + 1,
                "The field must be able to represent any register code");

 public:
  // Virtual registers get the remaining bits.
  static const uint32_t VREG_BITS =
      DATA_BITS - (USED_AT_START_SHIFT + USED_AT_START_BITS);
  static const uint32_t VREG_SHIFT = USED_AT_START_SHIFT + USED_AT_START_BITS;
  static const uint32_t VREG_MASK = (1 << VREG_BITS) - 1;

  enum Policy {
    // Input should be in a read-only register or stack slot.
    ANY,

    // Input must be in a read-only register.
    REGISTER,

    // Input must be in a specific, read-only register.
    FIXED,

    // Keep the used virtual register alive, and use whatever allocation is
    // available. This is similar to ANY but hints to the register allocator
    // that it is never useful to optimize this site.
    KEEPALIVE,

    // Input must be allocated on the stack.  Only used when extracting stack
    // results from stack result areas.
    STACK,

    // For snapshot inputs, indicates that the associated instruction will
    // write this input to its output register before bailing out.
    // The register allocator may thus allocate that output register, and
    // does not need to keep the virtual register alive (alternatively,
    // this may be treated as KEEPALIVE).
    RECOVERED_INPUT
  };

  void set(Policy policy, uint32_t reg, bool usedAtStart) {
    MOZ_ASSERT(reg <= REG_MASK, "Register code must fit in field");
    setKindAndData(USE, (policy << POLICY_SHIFT) | (reg << REG_SHIFT) |
                            ((usedAtStart ? 1 : 0) << USED_AT_START_SHIFT));
  }

 public:
  LUse(uint32_t vreg, Policy policy, bool usedAtStart = false) {
    set(policy, 0, usedAtStart);
    setVirtualRegister(vreg);
  }
  explicit LUse(Policy policy, bool usedAtStart = false) {
    set(policy, 0, usedAtStart);
  }
  explicit LUse(Register reg, bool usedAtStart = false) {
    set(FIXED, reg.code(), usedAtStart);
  }
  explicit LUse(FloatRegister reg, bool usedAtStart = false) {
    set(FIXED, reg.code(), usedAtStart);
  }
  LUse(Register reg, uint32_t virtualRegister, bool usedAtStart = false) {
    set(FIXED, reg.code(), usedAtStart);
    setVirtualRegister(virtualRegister);
  }
  LUse(FloatRegister reg, uint32_t virtualRegister, bool usedAtStart = false) {
    set(FIXED, reg.code(), usedAtStart);
    setVirtualRegister(virtualRegister);
  }

  void setVirtualRegister(uint32_t index) {
    MOZ_ASSERT(index < VREG_MASK);

    uint32_t old = data() & ~(VREG_MASK << VREG_SHIFT);
    setData(old | (index << VREG_SHIFT));
  }

  Policy policy() const {
    Policy policy = (Policy)((data() >> POLICY_SHIFT) & POLICY_MASK);
    return policy;
  }
  uint32_t virtualRegister() const {
    uint32_t index = (data() >> VREG_SHIFT) & VREG_MASK;
    MOZ_ASSERT(index != 0);
    return index;
  }
  uint32_t registerCode() const {
    MOZ_ASSERT(policy() == FIXED);
    return (data() >> REG_SHIFT) & REG_MASK;
  }
  bool isFixedRegister() const { return policy() == FIXED; }
  bool usedAtStart() const {
    return !!((data() >> USED_AT_START_SHIFT) & USED_AT_START_MASK);
  }
};

static const uint32_t MAX_VIRTUAL_REGISTERS = LUse::VREG_MASK;

class LBoxAllocation {
#ifdef JS_NUNBOX32
  LAllocation type_;
  LAllocation payload_;
#else
  LAllocation value_;
#endif

 public:
#ifdef JS_NUNBOX32
  LBoxAllocation(LAllocation type, LAllocation payload)
      : type_(type), payload_(payload) {}

  LAllocation type() const { return type_; }
  LAllocation payload() const { return payload_; }
#else
  explicit LBoxAllocation(LAllocation value) : value_(value) {}

  LAllocation value() const { return value_; }
#endif
};

template <class ValT>
class LInt64Value {
#if JS_BITS_PER_WORD == 32
  ValT high_;
  ValT low_;
#else
  ValT value_;
#endif

 public:
  LInt64Value() = default;

#if JS_BITS_PER_WORD == 32
  LInt64Value(ValT high, ValT low) : high_(high), low_(low) {}

  ValT high() const { return high_; }
  ValT low() const { return low_; }

  const ValT* pointerHigh() const { return &high_; }
  const ValT* pointerLow() const { return &low_; }
#else
  explicit LInt64Value(ValT value) : value_(value) {}

  ValT value() const { return value_; }
  const ValT* pointer() const { return &value_; }
#endif
};

using LInt64Allocation = LInt64Value<LAllocation>;

class LGeneralReg : public LAllocation {
 public:
  explicit LGeneralReg(Register reg) : LAllocation(GPR, reg.code()) {}

  Register reg() const { return Register::FromCode(data()); }
};

class LFloatReg : public LAllocation {
 public:
  explicit LFloatReg(FloatRegister reg) : LAllocation(FPU, reg.code()) {}

  FloatRegister reg() const { return FloatRegister::FromCode(data()); }
};

// Arbitrary constant index.
class LConstantIndex : public LAllocation {
  explicit LConstantIndex(uint32_t index)
      : LAllocation(CONSTANT_INDEX, index) {}

 public:
  static LConstantIndex FromIndex(uint32_t index) {
    return LConstantIndex(index);
  }

  uint32_t index() const { return data(); }
};

// Stack slots are indices into the stack. The indices are byte indices.
class LStackSlot : public LAllocation {
 public:
  explicit LStackSlot(uint32_t slot) : LAllocation(STACK_SLOT, slot) {}

  uint32_t slot() const { return data(); }
};

// Stack area indicates a contiguous stack allocation meant to receive call
// results that don't fit in registers.
class LStackArea : public LAllocation {
 public:
  explicit LStackArea(LInstruction* stackArea)
      : LAllocation(STACK_AREA, stackArea) {}

  // Byte index of base of stack area, in the same coordinate space as
  // LStackSlot::slot().
  inline uint32_t base() const;
  inline void setBase(uint32_t base);

  // Size in bytes of the stack area.
  inline uint32_t size() const;
  inline uint32_t alignment() const { return 8; }

  class ResultIterator {
    const LStackArea& alloc_;
    uint32_t idx_;

   public:
    explicit ResultIterator(const LStackArea& alloc) : alloc_(alloc), idx_(0) {}

    inline bool done() const;
    inline void next();
    inline LAllocation alloc() const;
    inline bool isGcPointer() const;

    explicit operator bool() const { return !done(); }
  };

  ResultIterator results() const { return ResultIterator(*this); }

  inline LStackSlot resultAlloc(LInstruction* lir, LDefinition* def) const;
};

// Arguments are reverse indices into the stack. The indices are byte indices.
class LArgument : public LAllocation {
 public:
  explicit LArgument(uint32_t index) : LAllocation(ARGUMENT_SLOT, index) {}

  uint32_t index() const { return data(); }
};

inline uint32_t LAllocation::memorySlot() const {
  MOZ_ASSERT(isMemory());
  return isStackSlot() ? toStackSlot()->slot() : toArgument()->index();
}

// Represents storage for a definition.
class LDefinition {
  // Bits containing policy, type, and virtual register.
  uint32_t bits_;

  // Before register allocation, this optionally contains a fixed policy.
  // Register allocation assigns this field to a physical policy if none is
  // fixed.
  //
  // Right now, pre-allocated outputs are limited to the following:
  //   * Physical argument stack slots.
  //   * Physical registers.
  LAllocation output_;

  static const uint32_t TYPE_BITS = 4;
  static const uint32_t TYPE_SHIFT = 0;
  static const uint32_t TYPE_MASK = (1 << TYPE_BITS) - 1;
  static const uint32_t POLICY_BITS = 2;
  static const uint32_t POLICY_SHIFT = TYPE_SHIFT + TYPE_BITS;
  static const uint32_t POLICY_MASK = (1 << POLICY_BITS) - 1;

  static const uint32_t VREG_BITS =
      (sizeof(uint32_t) * 8) - (POLICY_BITS + TYPE_BITS);
  static const uint32_t VREG_SHIFT = POLICY_SHIFT + POLICY_BITS;
  static const uint32_t VREG_MASK = (1 << VREG_BITS) - 1;

 public:
  // Note that definitions, by default, are always allocated a register,
  // unless the policy specifies that an input can be re-used and that input
  // is a stack slot.
  enum Policy {
    // The policy is predetermined by the LAllocation attached to this
    // definition. The allocation may be:
    //   * A register, which may not appear as any fixed temporary.
    //   * A stack slot or argument.
    //
    // Register allocation will not modify a fixed allocation.
    FIXED,

    // A random register of an appropriate class will be assigned.
    REGISTER,

    // An area on the stack must be assigned.  Used when defining stack results
    // and stack result areas.
    STACK,

    // One definition per instruction must re-use the first input
    // allocation, which (for now) must be a register.
    MUST_REUSE_INPUT
  };

  enum Type {
    GENERAL,  // Generic, integer or pointer-width data (GPR).
    INT32,    // int32 data (GPR).
    OBJECT,   // Pointer that may be collected as garbage (GPR).
    SLOTS,    // Slots/elements pointer that may be moved by minor GCs (GPR).
    FLOAT32,  // 32-bit floating-point value (FPU).
    DOUBLE,   // 64-bit floating-point value (FPU).
    SIMD128,  // 128-bit SIMD vector (FPU).
    STACKRESULTS,  // A variable-size stack allocation that may contain objects.
#ifdef JS_NUNBOX32
    // A type virtual register must be followed by a payload virtual
    // register, as both will be tracked as a single gcthing.
    TYPE,
    PAYLOAD
#else
    BOX  // Joined box, for punbox systems. (GPR, gcthing)
#endif
  };

  void set(uint32_t index, Type type, Policy policy) {
    static_assert(MAX_VIRTUAL_REGISTERS <= VREG_MASK);
    bits_ =
        (index << VREG_SHIFT) | (policy << POLICY_SHIFT) | (type << TYPE_SHIFT);
#ifndef ENABLE_WASM_SIMD
    MOZ_ASSERT(this->type() != SIMD128);
#endif
  }

 public:
  LDefinition(uint32_t index, Type type, Policy policy = REGISTER) {
    set(index, type, policy);
  }

  explicit LDefinition(Type type, Policy policy = REGISTER) {
    set(0, type, policy);
  }

  LDefinition(Type type, const LAllocation& a) : output_(a) {
    set(0, type, FIXED);
  }

  LDefinition(uint32_t index, Type type, const LAllocation& a) : output_(a) {
    set(index, type, FIXED);
  }

  LDefinition() : bits_(0) { MOZ_ASSERT(isBogusTemp()); }

  static LDefinition BogusTemp() { return LDefinition(); }

  Policy policy() const {
    return (Policy)((bits_ >> POLICY_SHIFT) & POLICY_MASK);
  }
  Type type() const { return (Type)((bits_ >> TYPE_SHIFT) & TYPE_MASK); }
  bool isCompatibleReg(const AnyRegister& r) const {
    if (isFloatReg() && r.isFloat()) {
      if (type() == FLOAT32) {
        return r.fpu().isSingle();
      }
      if (type() == DOUBLE) {
        return r.fpu().isDouble();
      }
      if (type() == SIMD128) {
        return r.fpu().isSimd128();
      }
      MOZ_CRASH("Unexpected MDefinition type");
    }
    return !isFloatReg() && !r.isFloat();
  }
  bool isCompatibleDef(const LDefinition& other) const {
#if defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_MIPS32)
    if (isFloatReg() && other.isFloatReg()) {
      return type() == other.type();
    }
    return !isFloatReg() && !other.isFloatReg();
#else
    return isFloatReg() == other.isFloatReg();
#endif
  }

  bool isFloatReg() const {
    return type() == FLOAT32 || type() == DOUBLE || type() == SIMD128;
  }
  uint32_t virtualRegister() const {
    uint32_t index = (bits_ >> VREG_SHIFT) & VREG_MASK;
    // MOZ_ASSERT(index != 0);
    return index;
  }
  LAllocation* output() { return &output_; }
  const LAllocation* output() const { return &output_; }
  bool isFixed() const { return policy() == FIXED; }
  bool isBogusTemp() const { return isFixed() && output()->isBogus(); }
  void setVirtualRegister(uint32_t index) {
    MOZ_ASSERT(index < VREG_MASK);
    bits_ &= ~(VREG_MASK << VREG_SHIFT);
    bits_ |= index << VREG_SHIFT;
  }
  void setOutput(const LAllocation& a) {
    output_ = a;
    if (!a.isUse()) {
      bits_ &= ~(POLICY_MASK << POLICY_SHIFT);
      bits_ |= FIXED << POLICY_SHIFT;
    }
  }
  void setReusedInput(uint32_t operand) {
    output_ = LConstantIndex::FromIndex(operand);
  }
  uint32_t getReusedInput() const {
    MOZ_ASSERT(policy() == LDefinition::MUST_REUSE_INPUT);
    return output_.toConstantIndex()->index();
  }

  static inline Type TypeFrom(MIRType type) {
    switch (type) {
      case MIRType::Boolean:
      case MIRType::Int32:
        // The stack slot allocator doesn't currently support allocating
        // 1-byte slots, so for now we lower MIRType::Boolean into INT32.
        static_assert(sizeof(bool) <= sizeof(int32_t),
                      "bool doesn't fit in an int32 slot");
        return LDefinition::INT32;
      case MIRType::String:
      case MIRType::Symbol:
      case MIRType::BigInt:
      case MIRType::Object:
      case MIRType::RefOrNull:
        return LDefinition::OBJECT;
      case MIRType::Double:
        return LDefinition::DOUBLE;
      case MIRType::Float32:
        return LDefinition::FLOAT32;
#if defined(JS_PUNBOX64)
      case MIRType::Value:
        return LDefinition::BOX;
#endif
      case MIRType::Slots:
      case MIRType::Elements:
        return LDefinition::SLOTS;
      case MIRType::Pointer:
      case MIRType::IntPtr:
        return LDefinition::GENERAL;
#if defined(JS_PUNBOX64)
      case MIRType::Int64:
        return LDefinition::GENERAL;
#endif
      case MIRType::StackResults:
        return LDefinition::STACKRESULTS;
      case MIRType::Simd128:
        return LDefinition::SIMD128;
      default:
        MOZ_CRASH("unexpected type");
    }
  }

  UniqueChars toString() const;

#ifdef JS_JITSPEW
  void dump() const;
#endif
};

class LInt64Definition : public LInt64Value<LDefinition> {
 public:
  using LInt64Value<LDefinition>::LInt64Value;

  static LInt64Definition BogusTemp() { return LInt64Definition(); }

  bool isBogusTemp() const {
#if JS_BITS_PER_WORD == 32
    MOZ_ASSERT(high().isBogusTemp() == low().isBogusTemp());
    return high().isBogusTemp();
#else
    return value().isBogusTemp();
#endif
  }
};

// Forward declarations of LIR types.
#define LIROP(op) class L##op;
LIR_OPCODE_LIST(LIROP)
#undef LIROP

class LSnapshot;
class LSafepoint;
class LElementVisitor;

constexpr size_t MaxNumLInstructionOperands = 63;

// The common base class for LPhi and LInstruction.
class LNode {
 protected:
  MDefinition* mir_;

 private:
  LBlock* block_;
  uint32_t id_;

 protected:
  // Bitfields below are all uint32_t to make sure MSVC packs them correctly.
  uint32_t op_ : 10;
  uint32_t isCall_ : 1;

  // LPhi::numOperands() may not fit in this bitfield, so we only use this
  // field for LInstruction.
  uint32_t nonPhiNumOperands_ : 6;
  static_assert((1 << 6) - 1 == MaxNumLInstructionOperands,
                "packing constraints");

  // For LInstruction, the first operand is stored at offset
  // sizeof(LInstruction) + nonPhiOperandsOffset_ * sizeof(uintptr_t).
  uint32_t nonPhiOperandsOffset_ : 5;
  uint32_t numDefs_ : 4;
  uint32_t numTemps_ : 4;

 public:
  enum class Opcode {
#define LIROP(name) name,
    LIR_OPCODE_LIST(LIROP)
#undef LIROP
        Invalid
  };

  LNode(Opcode op, uint32_t nonPhiNumOperands, uint32_t numDefs,
        uint32_t numTemps)
      : mir_(nullptr),
        block_(nullptr),
        id_(0),
        op_(uint32_t(op)),
        isCall_(false),
        nonPhiNumOperands_(nonPhiNumOperands),
        nonPhiOperandsOffset_(0),
        numDefs_(numDefs),
        numTemps_(numTemps) {
    MOZ_ASSERT(op < Opcode::Invalid);
    MOZ_ASSERT(op_ == uint32_t(op), "opcode must fit in bitfield");
    MOZ_ASSERT(nonPhiNumOperands_ == nonPhiNumOperands,
               "nonPhiNumOperands must fit in bitfield");
    MOZ_ASSERT(numDefs_ == numDefs, "numDefs must fit in bitfield");
    MOZ_ASSERT(numTemps_ == numTemps, "numTemps must fit in bitfield");
  }

  const char* opName() {
    switch (op()) {
#define LIR_NAME_INS(name) \
  case Opcode::name:       \
    return #name;
      LIR_OPCODE_LIST(LIR_NAME_INS)
#undef LIR_NAME_INS
      default:
        MOZ_CRASH("Invalid op");
    }
  }

  // Hook for opcodes to add extra high level detail about what code will be
  // emitted for the op.
 private:
  const char* extraName() const { return nullptr; }

 public:
#ifdef JS_JITSPEW
  const char* getExtraName() const;
#endif

  Opcode op() const { return Opcode(op_); }

  bool isInstruction() const { return op() != Opcode::Phi; }
  inline LInstruction* toInstruction();
  inline const LInstruction* toInstruction() const;

  // Returns the number of outputs of this instruction. If an output is
  // unallocated, it is an LDefinition, defining a virtual register.
  size_t numDefs() const { return numDefs_; }

  bool isCall() const { return isCall_; }

  // Does this call preserve the given register?
  // By default, it is assumed that all registers are clobbered by a call.
  inline bool isCallPreserved(AnyRegister reg) const;

  uint32_t id() const { return id_; }
  void setId(uint32_t id) {
    MOZ_ASSERT(!id_);
    MOZ_ASSERT(id);
    id_ = id;
  }
  void setMir(MDefinition* mir) { mir_ = mir; }
  MDefinition* mirRaw() const {
    /* Untyped MIR for this op. Prefer mir() methods in subclasses. */
    return mir_;
  }
  LBlock* block() const { return block_; }
  void setBlock(LBlock* block) { block_ = block; }

  // For an instruction which has a MUST_REUSE_INPUT output, whether that
  // output register will be restored to its original value when bailing out.
  inline bool recoversInput() const;

#ifdef JS_JITSPEW
  void dump(GenericPrinter& out);
  void dump();
  static void printName(GenericPrinter& out, Opcode op);
  void printName(GenericPrinter& out);
  void printOperands(GenericPrinter& out);
#endif

 public:
  // Opcode testing and casts.
#define LIROP(name)                                      \
  bool is##name() const { return op() == Opcode::name; } \
  inline L##name* to##name();                            \
  inline const L##name* to##name() const;
  LIR_OPCODE_LIST(LIROP)
#undef LIROP

// Note: GenerateOpcodeFiles.py generates LOpcodesGenerated.h based on this
// macro.
#define LIR_HEADER(opcode) \
  static constexpr LNode::Opcode classOpcode = LNode::Opcode::opcode;
};

class LInstruction : public LNode,
                     public TempObject,
                     public InlineListNode<LInstruction> {
  // This snapshot could be set after a ResumePoint.  It is used to restart
  // from the resume point pc.
  LSnapshot* snapshot_;

  // Structure capturing the set of stack slots and registers which are known
  // to hold either gcthings or Values.
  LSafepoint* safepoint_;

  LMoveGroup* inputMoves_;
  LMoveGroup* fixReuseMoves_;
  LMoveGroup* movesAfter_;

 protected:
  LInstruction(Opcode opcode, uint32_t numOperands, uint32_t numDefs,
               uint32_t numTemps)
      : LNode(opcode, numOperands, numDefs, numTemps),
        snapshot_(nullptr),
        safepoint_(nullptr),
        inputMoves_(nullptr),
        fixReuseMoves_(nullptr),
        movesAfter_(nullptr) {}

  void setIsCall() { isCall_ = true; }

 public:
  inline LDefinition* getDef(size_t index);

  void setDef(size_t index, const LDefinition& def) { *getDef(index) = def; }

  LAllocation* getOperand(size_t index) const {
    MOZ_ASSERT(index < numOperands());
    MOZ_ASSERT(nonPhiOperandsOffset_ > 0);
    uintptr_t p = reinterpret_cast<uintptr_t>(this + 1) +
                  nonPhiOperandsOffset_ * sizeof(uintptr_t);
    return reinterpret_cast<LAllocation*>(p) + index;
  }
  void setOperand(size_t index, const LAllocation& a) {
    *getOperand(index) = a;
  }

  void initOperandsOffset(size_t offset) {
    MOZ_ASSERT(nonPhiOperandsOffset_ == 0);
    MOZ_ASSERT(offset >= sizeof(LInstruction));
    MOZ_ASSERT(((offset - sizeof(LInstruction)) % sizeof(uintptr_t)) == 0);
    offset = (offset - sizeof(LInstruction)) / sizeof(uintptr_t);
    nonPhiOperandsOffset_ = offset;
    MOZ_ASSERT(nonPhiOperandsOffset_ == offset, "offset must fit in bitfield");
  }

  // Returns information about temporary registers needed. Each temporary
  // register is an LDefinition with a fixed or virtual register and
  // either GENERAL, FLOAT32, or DOUBLE type.
  size_t numTemps() const { return numTemps_; }
  inline LDefinition* getTemp(size_t index);

  LSnapshot* snapshot() const { return snapshot_; }
  LSafepoint* safepoint() const { return safepoint_; }
  LMoveGroup* inputMoves() const { return inputMoves_; }
  void setInputMoves(LMoveGroup* moves) { inputMoves_ = moves; }
  LMoveGroup* fixReuseMoves() const { return fixReuseMoves_; }
  void setFixReuseMoves(LMoveGroup* moves) { fixReuseMoves_ = moves; }
  LMoveGroup* movesAfter() const { return movesAfter_; }
  void setMovesAfter(LMoveGroup* moves) { movesAfter_ = moves; }
  uint32_t numOperands() const { return nonPhiNumOperands_; }
  void assignSnapshot(LSnapshot* snapshot);
  void initSafepoint(TempAllocator& alloc);

  class InputIterator;
};

LInstruction* LNode::toInstruction() {
  MOZ_ASSERT(isInstruction());
  return static_cast<LInstruction*>(this);
}

const LInstruction* LNode::toInstruction() const {
  MOZ_ASSERT(isInstruction());
  return static_cast<const LInstruction*>(this);
}

class LElementVisitor {
#ifdef TRACK_SNAPSHOTS
  LInstruction* ins_ = nullptr;
#endif

 protected:
#ifdef TRACK_SNAPSHOTS
  LInstruction* instruction() { return ins_; }

  void setElement(LInstruction* ins) { ins_ = ins; }
#else
  void setElement(LInstruction* ins) {}
#endif
};

using LInstructionIterator = InlineList<LInstruction>::iterator;
using LInstructionReverseIterator = InlineList<LInstruction>::reverse_iterator;

class MPhi;

// Phi is a pseudo-instruction that emits no code, and is an annotation for the
// register allocator. Like its equivalent in MIR, phis are collected at the
// top of blocks and are meant to be executed in parallel, choosing the input
// corresponding to the predecessor taken in the control flow graph.
class LPhi final : public LNode {
  LAllocation* const inputs_;
  LDefinition def_;

 public:
  LIR_HEADER(Phi)

  LPhi(MPhi* ins, LAllocation* inputs)
      : LNode(classOpcode,
              /* nonPhiNumOperands = */ 0,
              /* numDefs = */ 1,
              /* numTemps = */ 0),
        inputs_(inputs) {
    setMir(ins);
  }

  LDefinition* getDef(size_t index) {
    MOZ_ASSERT(index == 0);
    return &def_;
  }
  void setDef(size_t index, const LDefinition& def) {
    MOZ_ASSERT(index == 0);
    def_ = def;
  }
  size_t numOperands() const { return mir_->toPhi()->numOperands(); }
  LAllocation* getOperand(size_t index) {
    MOZ_ASSERT(index < numOperands());
    return &inputs_[index];
  }
  void setOperand(size_t index, const LAllocation& a) {
    MOZ_ASSERT(index < numOperands());
    inputs_[index] = a;
  }

  // Phis don't have temps, so calling numTemps/getTemp is pointless.
  size_t numTemps() const = delete;
  LDefinition* getTemp(size_t index) = delete;
};

class LMoveGroup;
class LBlock {
  MBasicBlock* block_;
  FixedList<LPhi> phis_;
  InlineList<LInstruction> instructions_;
  LMoveGroup* entryMoveGroup_;
  LMoveGroup* exitMoveGroup_;
  Label label_;

 public:
  explicit LBlock(MBasicBlock* block);
  [[nodiscard]] bool init(TempAllocator& alloc);

  void add(LInstruction* ins) {
    ins->setBlock(this);
    instructions_.pushBack(ins);
  }
  size_t numPhis() const { return phis_.length(); }
  LPhi* getPhi(size_t index) { return &phis_[index]; }
  const LPhi* getPhi(size_t index) const { return &phis_[index]; }
  MBasicBlock* mir() const { return block_; }
  LInstructionIterator begin() { return instructions_.begin(); }
  LInstructionIterator begin(LInstruction* at) {
    return instructions_.begin(at);
  }
  LInstructionIterator end() { return instructions_.end(); }
  LInstructionReverseIterator rbegin() { return instructions_.rbegin(); }
  LInstructionReverseIterator rbegin(LInstruction* at) {
    return instructions_.rbegin(at);
  }
  LInstructionReverseIterator rend() { return instructions_.rend(); }
  InlineList<LInstruction>& instructions() { return instructions_; }
  void insertAfter(LInstruction* at, LInstruction* ins) {
    instructions_.insertAfter(at, ins);
  }
  void insertBefore(LInstruction* at, LInstruction* ins) {
    instructions_.insertBefore(at, ins);
  }
  const LNode* firstElementWithId() const {
    return !phis_.empty() ? static_cast<const LNode*>(getPhi(0))
                          : firstInstructionWithId();
  }
  uint32_t firstId() const { return firstElementWithId()->id(); }
  uint32_t lastId() const { return lastInstructionWithId()->id(); }
  const LInstruction* firstInstructionWithId() const;
  const LInstruction* lastInstructionWithId() const {
    const LInstruction* last = *instructions_.rbegin();
    MOZ_ASSERT(last->id());
    // The last instruction is a control flow instruction which does not have
    // any output.
    MOZ_ASSERT(last->numDefs() == 0);
    return last;
  }

  // Return the label to branch to when branching to this block.
  Label* label() {
    MOZ_ASSERT(!isTrivial());
    return &label_;
  }

  LMoveGroup* getEntryMoveGroup(TempAllocator& alloc);
  LMoveGroup* getExitMoveGroup(TempAllocator& alloc);

  // Test whether this basic block is empty except for a simple goto, and
  // which is not forming a loop. No code will be emitted for such blocks.
  bool isTrivial() { return begin()->isGoto() && !mir()->isLoopHeader(); }

#ifdef JS_JITSPEW
  void dump(GenericPrinter& out);
  void dump();
#endif
};

namespace details {
template <size_t Defs, size_t Temps>
class LInstructionFixedDefsTempsHelper : public LInstruction {
  mozilla::Array<LDefinition, Defs + Temps> defsAndTemps_;

 protected:
  LInstructionFixedDefsTempsHelper(Opcode opcode, uint32_t numOperands)
      : LInstruction(opcode, numOperands, Defs, Temps) {}

 public:
  // Override the methods in LInstruction with more optimized versions
  // for when we know the exact instruction type.
  LDefinition* getDef(size_t index) {
    MOZ_ASSERT(index < Defs);
    return &defsAndTemps_[index];
  }
  LDefinition* getTemp(size_t index) {
    MOZ_ASSERT(index < Temps);
    return &defsAndTemps_[Defs + index];
  }
  LInt64Definition getInt64Temp(size_t index) {
    MOZ_ASSERT(index + INT64_PIECES <= Temps);
#if JS_BITS_PER_WORD == 32
    return LInt64Definition(defsAndTemps_[Defs + index + INT64HIGH_INDEX],
                            defsAndTemps_[Defs + index + INT64LOW_INDEX]);
#else
    return LInt64Definition(defsAndTemps_[Defs + index]);
#endif
  }

  void setDef(size_t index, const LDefinition& def) {
    MOZ_ASSERT(index < Defs);
    defsAndTemps_[index] = def;
  }
  void setTemp(size_t index, const LDefinition& a) {
    MOZ_ASSERT(index < Temps);
    defsAndTemps_[Defs + index] = a;
  }
  void setInt64Temp(size_t index, const LInt64Definition& a) {
#if JS_BITS_PER_WORD == 32
    setTemp(index, a.low());
    setTemp(index + 1, a.high());
#else
    setTemp(index, a.value());
#endif
  }

  // Default accessors, assuming a single input and output, respectively.
  const LAllocation* input() {
    MOZ_ASSERT(numOperands() == 1);
    return getOperand(0);
  }
  const LDefinition* output() {
    MOZ_ASSERT(numDefs() == 1);
    return getDef(0);
  }
  static size_t offsetOfDef(size_t index) {
    using T = LInstructionFixedDefsTempsHelper<0, 0>;
    return offsetof(T, defsAndTemps_) + index * sizeof(LDefinition);
  }
  static size_t offsetOfTemp(uint32_t numDefs, uint32_t index) {
    using T = LInstructionFixedDefsTempsHelper<0, 0>;
    return offsetof(T, defsAndTemps_) + (numDefs + index) * sizeof(LDefinition);
  }
};
}  // namespace details

inline LDefinition* LInstruction::getDef(size_t index) {
  MOZ_ASSERT(index < numDefs());
  using T = details::LInstructionFixedDefsTempsHelper<0, 0>;
  uint8_t* p = reinterpret_cast<uint8_t*>(this) + T::offsetOfDef(index);
  return reinterpret_cast<LDefinition*>(p);
}

inline LDefinition* LInstruction::getTemp(size_t index) {
  MOZ_ASSERT(index < numTemps());
  using T = details::LInstructionFixedDefsTempsHelper<0, 0>;
  uint8_t* p =
      reinterpret_cast<uint8_t*>(this) + T::offsetOfTemp(numDefs(), index);
  return reinterpret_cast<LDefinition*>(p);
}

template <size_t Defs, size_t Operands, size_t Temps>
class LInstructionHelper
    : public details::LInstructionFixedDefsTempsHelper<Defs, Temps> {
  mozilla::Array<LAllocation, Operands> operands_;

 protected:
  explicit LInstructionHelper(LNode::Opcode opcode)
      : details::LInstructionFixedDefsTempsHelper<Defs, Temps>(opcode,
                                                               Operands) {
    static_assert(
        Operands == 0 || sizeof(operands_) == Operands * sizeof(LAllocation),
        "mozilla::Array should not contain other fields");
    if (Operands > 0) {
      using T = LInstructionHelper<Defs, Operands, Temps>;
      this->initOperandsOffset(offsetof(T, operands_));
    }
  }

 public:
  // Override the methods in LInstruction with more optimized versions
  // for when we know the exact instruction type.
  LAllocation* getOperand(size_t index) { return &operands_[index]; }
  void setOperand(size_t index, const LAllocation& a) { operands_[index] = a; }
  void setBoxOperand(size_t index, const LBoxAllocation& alloc) {
#ifdef JS_NUNBOX32
    operands_[index + TYPE_INDEX] = alloc.type();
    operands_[index + PAYLOAD_INDEX] = alloc.payload();
#else
    operands_[index] = alloc.value();
#endif
  }
  void setInt64Operand(size_t index, const LInt64Allocation& alloc) {
#if JS_BITS_PER_WORD == 32
    operands_[index + INT64LOW_INDEX] = alloc.low();
    operands_[index + INT64HIGH_INDEX] = alloc.high();
#else
    operands_[index] = alloc.value();
#endif
  }
  const LInt64Allocation getInt64Operand(size_t offset) {
#if JS_BITS_PER_WORD == 32
    return LInt64Allocation(operands_[offset + INT64HIGH_INDEX],
                            operands_[offset + INT64LOW_INDEX]);
#else
    return LInt64Allocation(operands_[offset]);
#endif
  }
};

template <size_t Defs, size_t Temps>
class LVariadicInstruction
    : public details::LInstructionFixedDefsTempsHelper<Defs, Temps> {
 protected:
  LVariadicInstruction(LNode::Opcode opcode, size_t numOperands)
      : details::LInstructionFixedDefsTempsHelper<Defs, Temps>(opcode,
                                                               numOperands) {}

 public:
  void setBoxOperand(size_t index, const LBoxAllocation& a) {
#ifdef JS_NUNBOX32
    this->setOperand(index + TYPE_INDEX, a.type());
    this->setOperand(index + PAYLOAD_INDEX, a.payload());
#else
    this->setOperand(index, a.value());
#endif
  }
};

template <size_t Defs, size_t Operands, size_t Temps>
class LCallInstructionHelper
    : public LInstructionHelper<Defs, Operands, Temps> {
 protected:
  explicit LCallInstructionHelper(LNode::Opcode opcode)
      : LInstructionHelper<Defs, Operands, Temps>(opcode) {
    this->setIsCall();
  }
};

template <size_t Defs, size_t Temps>
class LBinaryCallInstructionHelper
    : public LCallInstructionHelper<Defs, 2, Temps> {
 protected:
  explicit LBinaryCallInstructionHelper(LNode::Opcode opcode)
      : LCallInstructionHelper<Defs, 2, Temps>(opcode) {}

 public:
  const LAllocation* lhs() { return this->getOperand(0); }
  const LAllocation* rhs() { return this->getOperand(1); }
};

class LRecoverInfo : public TempObject {
 public:
  typedef Vector<MNode*, 2, JitAllocPolicy> Instructions;

 private:
  // List of instructions needed to recover the stack frames.
  // Outer frames are stored before inner frames.
  Instructions instructions_;

  // Cached offset where this resume point is encoded.
  RecoverOffset recoverOffset_;

  explicit LRecoverInfo(TempAllocator& alloc);
  [[nodiscard]] bool init(MResumePoint* mir);

  // Fill the instruction vector such as all instructions needed for the
  // recovery are pushed before the current instruction.
  template <typename Node>
  [[nodiscard]] bool appendOperands(Node* ins);
  [[nodiscard]] bool appendDefinition(MDefinition* def);
  [[nodiscard]] bool appendResumePoint(MResumePoint* rp);

 public:
  static LRecoverInfo* New(MIRGenerator* gen, MResumePoint* mir);

  // Resume point of the inner most function.
  MResumePoint* mir() const { return instructions_.back()->toResumePoint(); }
  RecoverOffset recoverOffset() const { return recoverOffset_; }
  void setRecoverOffset(RecoverOffset offset) {
    MOZ_ASSERT(recoverOffset_ == INVALID_RECOVER_OFFSET);
    recoverOffset_ = offset;
  }

  MNode** begin() { return instructions_.begin(); }
  MNode** end() { return instructions_.end(); }
  size_t numInstructions() const { return instructions_.length(); }

  class OperandIter {
   private:
    MNode** it_;
    MNode** end_;
    size_t op_;
    size_t opEnd_;
    MResumePoint* rp_;
    MNode* node_;

   public:
    explicit OperandIter(LRecoverInfo* recoverInfo)
        : it_(recoverInfo->begin()),
          end_(recoverInfo->end()),
          op_(0),
          opEnd_(0),
          rp_(nullptr),
          node_(nullptr) {
      settle();
    }

    void settle() {
      opEnd_ = (*it_)->numOperands();
      while (opEnd_ == 0) {
        ++it_;
        op_ = 0;
        opEnd_ = (*it_)->numOperands();
      }
      node_ = *it_;
      if (node_->isResumePoint()) {
        rp_ = node_->toResumePoint();
      }
    }

    MDefinition* operator*() {
      if (rp_) {  // de-virtualize MResumePoint::getOperand calls.
        return rp_->getOperand(op_);
      }
      return node_->getOperand(op_);
    }
    MDefinition* operator->() {
      if (rp_) {  // de-virtualize MResumePoint::getOperand calls.
        return rp_->getOperand(op_);
      }
      return node_->getOperand(op_);
    }

    OperandIter& operator++() {
      ++op_;
      if (op_ != opEnd_) {
        return *this;
      }
      op_ = 0;
      ++it_;
      node_ = rp_ = nullptr;
      if (!*this) {
        settle();
      }
      return *this;
    }

    explicit operator bool() const { return it_ == end_; }

#ifdef DEBUG
    bool canOptimizeOutIfUnused();
#endif
  };
};

// An LSnapshot is the reflection of an MResumePoint in LIR. Unlike
// MResumePoints, they cannot be shared, as they are filled in by the register
// allocator in order to capture the precise low-level stack state in between an
// instruction's input and output. During code generation, LSnapshots are
// compressed and saved in the compiled script.
class LSnapshot : public TempObject {
 private:
  LAllocation* slots_;
  LRecoverInfo* recoverInfo_;
  SnapshotOffset snapshotOffset_;
  uint32_t numSlots_;
  BailoutId bailoutId_;
  BailoutKind bailoutKind_;

  LSnapshot(LRecoverInfo* recover, BailoutKind kind);
  [[nodiscard]] bool init(MIRGenerator* gen);

 public:
  static LSnapshot* New(MIRGenerator* gen, LRecoverInfo* recover,
                        BailoutKind kind);

  size_t numEntries() const { return numSlots_; }
  size_t numSlots() const { return numSlots_ / BOX_PIECES; }
  LAllocation* payloadOfSlot(size_t i) {
    MOZ_ASSERT(i < numSlots());
    size_t entryIndex = (i * BOX_PIECES) + (BOX_PIECES - 1);
    return getEntry(entryIndex);
  }
#ifdef JS_NUNBOX32
  LAllocation* typeOfSlot(size_t i) {
    MOZ_ASSERT(i < numSlots());
    size_t entryIndex = (i * BOX_PIECES) + (BOX_PIECES - 2);
    return getEntry(entryIndex);
  }
#endif
  LAllocation* getEntry(size_t i) {
    MOZ_ASSERT(i < numSlots_);
    return &slots_[i];
  }
  void setEntry(size_t i, const LAllocation& alloc) {
    MOZ_ASSERT(i < numSlots_);
    slots_[i] = alloc;
  }
  LRecoverInfo* recoverInfo() const { return recoverInfo_; }
  MResumePoint* mir() const { return recoverInfo()->mir(); }
  SnapshotOffset snapshotOffset() const { return snapshotOffset_; }
  BailoutId bailoutId() const { return bailoutId_; }
  void setSnapshotOffset(SnapshotOffset offset) {
    MOZ_ASSERT(snapshotOffset_ == INVALID_SNAPSHOT_OFFSET);
    snapshotOffset_ = offset;
  }
  void setBailoutId(BailoutId id) {
    MOZ_ASSERT(bailoutId_ == INVALID_BAILOUT_ID);
    bailoutId_ = id;
  }
  BailoutKind bailoutKind() const { return bailoutKind_; }
  void rewriteRecoveredInput(LUse input);
};

struct SafepointSlotEntry {
  // Flag indicating whether this is a slot in the stack or argument space.
  uint32_t stack : 1;

  // Byte offset of the slot, as in LStackSlot or LArgument.
  uint32_t slot : 31;

  SafepointSlotEntry() : stack(0), slot(0) {}
  SafepointSlotEntry(bool stack, uint32_t slot) : stack(stack), slot(slot) {}
  explicit SafepointSlotEntry(const LAllocation* a)
      : stack(a->isStackSlot()), slot(a->memorySlot()) {}
};

struct SafepointNunboxEntry {
  uint32_t typeVreg;
  LAllocation type;
  LAllocation payload;

  SafepointNunboxEntry() : typeVreg(0) {}
  SafepointNunboxEntry(uint32_t typeVreg, LAllocation type, LAllocation payload)
      : typeVreg(typeVreg), type(type), payload(payload) {}
};

class LSafepoint : public TempObject {
  using SlotEntry = SafepointSlotEntry;
  using NunboxEntry = SafepointNunboxEntry;

 public:
  typedef Vector<SlotEntry, 0, JitAllocPolicy> SlotList;
  typedef Vector<NunboxEntry, 0, JitAllocPolicy> NunboxList;

 private:
  // The information in a safepoint describes the registers and gc related
  // values that are live at the start of the associated instruction.

  // The set of registers which are live at an OOL call made within the
  // instruction. This includes any registers for inputs which are not
  // use-at-start, any registers for temps, and any registers live after the
  // call except outputs of the instruction.
  //
  // For call instructions, the live regs are empty. Call instructions may
  // have register inputs or temporaries, which will *not* be in the live
  // registers: if passed to the call, the values passed will be marked via
  // MarkJitExitFrame, and no registers can be live after the instruction
  // except its outputs.
  LiveRegisterSet liveRegs_;

  // The subset of liveRegs which contains gcthing pointers.
  LiveGeneralRegisterSet gcRegs_;

#ifdef CHECK_OSIPOINT_REGISTERS
  // Clobbered regs of the current instruction. This set is never written to
  // the safepoint; it's only used by assertions during compilation.
  LiveRegisterSet clobberedRegs_;
#endif

  // Offset to a position in the safepoint stream, or
  // INVALID_SAFEPOINT_OFFSET.
  uint32_t safepointOffset_;

  // Assembler buffer displacement to OSI point's call location.
  uint32_t osiCallPointOffset_;

  // List of slots which have gcthing pointers.
  SlotList gcSlots_;

#ifdef JS_NUNBOX32
  // List of registers (in liveRegs) and slots which contain pieces of Values.
  NunboxList nunboxParts_;
#elif JS_PUNBOX64
  // List of slots which have Values.
  SlotList valueSlots_;

  // The subset of liveRegs which have Values.
  LiveGeneralRegisterSet valueRegs_;
#endif

  // The subset of liveRegs which contains pointers to slots/elements.
  LiveGeneralRegisterSet slotsOrElementsRegs_;

  // List of slots which have slots/elements pointers.
  SlotList slotsOrElementsSlots_;

  // Wasm only: with what kind of instruction is this LSafepoint associated?
  // true => wasm trap, false => wasm call.
  bool isWasmTrap_;

  // Wasm only: what is the value of masm.framePushed() that corresponds to
  // the lowest-addressed word covered by the StackMap that we will generate
  // from this LSafepoint?  This depends on the instruction:
  //
  // if isWasmTrap_ == true:
  //    masm.framePushed() unmodified.  Note that when constructing the
  //    StackMap we will add entries below this point to take account of
  //    registers dumped on the stack as a result of the trap.
  //
  // if isWasmTrap_ == false:
  //    masm.framePushed() - StackArgAreaSizeUnaligned(arg types for the call),
  //    because the map does not include the outgoing args themselves, but
  //    it does cover any and all alignment space above them.
  uint32_t framePushedAtStackMapBase_;

 public:
  void assertInvariants() {
    // Every register in valueRegs and gcRegs should also be in liveRegs.
#ifndef JS_NUNBOX32
    MOZ_ASSERT((valueRegs().bits() & ~liveRegs().gprs().bits()) == 0);
#endif
    MOZ_ASSERT((gcRegs().bits() & ~liveRegs().gprs().bits()) == 0);
  }

  explicit LSafepoint(TempAllocator& alloc)
      : safepointOffset_(INVALID_SAFEPOINT_OFFSET),
        osiCallPointOffset_(0),
        gcSlots_(alloc),
#ifdef JS_NUNBOX32
        nunboxParts_(alloc),
#else
        valueSlots_(alloc),
#endif
        slotsOrElementsSlots_(alloc),
        isWasmTrap_(false),
        framePushedAtStackMapBase_(0) {
    assertInvariants();
  }
  void addLiveRegister(AnyRegister reg) {
    liveRegs_.addUnchecked(reg);
    assertInvariants();
  }
  const LiveRegisterSet& liveRegs() const { return liveRegs_; }
#ifdef CHECK_OSIPOINT_REGISTERS
  void addClobberedRegister(AnyRegister reg) {
    clobberedRegs_.addUnchecked(reg);
    assertInvariants();
  }
  const LiveRegisterSet& clobberedRegs() const { return clobberedRegs_; }
#endif
  void addGcRegister(Register reg) {
    gcRegs_.addUnchecked(reg);
    assertInvariants();
  }
  LiveGeneralRegisterSet gcRegs() const { return gcRegs_; }
  [[nodiscard]] bool addGcSlot(bool stack, uint32_t slot) {
    bool result = gcSlots_.append(SlotEntry(stack, slot));
    if (result) {
      assertInvariants();
    }
    return result;
  }
  SlotList& gcSlots() { return gcSlots_; }

  SlotList& slotsOrElementsSlots() { return slotsOrElementsSlots_; }
  LiveGeneralRegisterSet slotsOrElementsRegs() const {
    return slotsOrElementsRegs_;
  }
  void addSlotsOrElementsRegister(Register reg) {
    slotsOrElementsRegs_.addUnchecked(reg);
    assertInvariants();
  }
  [[nodiscard]] bool addSlotsOrElementsSlot(bool stack, uint32_t slot) {
    bool result = slotsOrElementsSlots_.append(SlotEntry(stack, slot));
    if (result) {
      assertInvariants();
    }
    return result;
  }
  [[nodiscard]] bool addSlotsOrElementsPointer(LAllocation alloc) {
    if (alloc.isMemory()) {
      return addSlotsOrElementsSlot(alloc.isStackSlot(), alloc.memorySlot());
    }
    MOZ_ASSERT(alloc.isRegister());
    addSlotsOrElementsRegister(alloc.toRegister().gpr());
    assertInvariants();
    return true;
  }
  bool hasSlotsOrElementsPointer(LAllocation alloc) const {
    if (alloc.isRegister()) {
      return slotsOrElementsRegs().has(alloc.toRegister().gpr());
    }
    for (size_t i = 0; i < slotsOrElementsSlots_.length(); i++) {
      const SlotEntry& entry = slotsOrElementsSlots_[i];
      if (entry.stack == alloc.isStackSlot() &&
          entry.slot == alloc.memorySlot()) {
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] bool addGcPointer(LAllocation alloc) {
    if (alloc.isMemory()) {
      return addGcSlot(alloc.isStackSlot(), alloc.memorySlot());
    }
    if (alloc.isRegister()) {
      addGcRegister(alloc.toRegister().gpr());
    }
    assertInvariants();
    return true;
  }

  bool hasGcPointer(LAllocation alloc) const {
    if (alloc.isRegister()) {
      return gcRegs().has(alloc.toRegister().gpr());
    }
    MOZ_ASSERT(alloc.isMemory());
    for (size_t i = 0; i < gcSlots_.length(); i++) {
      if (gcSlots_[i].stack == alloc.isStackSlot() &&
          gcSlots_[i].slot == alloc.memorySlot()) {
        return true;
      }
    }
    return false;
  }

  // Return true if all GC-managed pointers from `alloc` are recorded in this
  // safepoint.
  bool hasAllGcPointersFromStackArea(LAllocation alloc) const {
    for (LStackArea::ResultIterator iter = alloc.toStackArea()->results(); iter;
         iter.next()) {
      if (iter.isGcPointer() && !hasGcPointer(iter.alloc())) {
        return false;
      }
    }
    return true;
  }

#ifdef JS_NUNBOX32
  [[nodiscard]] bool addNunboxParts(uint32_t typeVreg, LAllocation type,
                                    LAllocation payload) {
    bool result = nunboxParts_.append(NunboxEntry(typeVreg, type, payload));
    if (result) {
      assertInvariants();
    }
    return result;
  }

  [[nodiscard]] bool addNunboxType(uint32_t typeVreg, LAllocation type) {
    for (size_t i = 0; i < nunboxParts_.length(); i++) {
      if (nunboxParts_[i].type == type) {
        return true;
      }
      if (nunboxParts_[i].type == LUse(typeVreg, LUse::ANY)) {
        nunboxParts_[i].type = type;
        return true;
      }
    }

    // vregs for nunbox pairs are adjacent, with the type coming first.
    uint32_t payloadVreg = typeVreg + 1;
    bool result = nunboxParts_.append(
        NunboxEntry(typeVreg, type, LUse(payloadVreg, LUse::ANY)));
    if (result) {
      assertInvariants();
    }
    return result;
  }

  [[nodiscard]] bool addNunboxPayload(uint32_t payloadVreg,
                                      LAllocation payload) {
    for (size_t i = 0; i < nunboxParts_.length(); i++) {
      if (nunboxParts_[i].payload == payload) {
        return true;
      }
      if (nunboxParts_[i].payload == LUse(payloadVreg, LUse::ANY)) {
        nunboxParts_[i].payload = payload;
        return true;
      }
    }

    // vregs for nunbox pairs are adjacent, with the type coming first.
    uint32_t typeVreg = payloadVreg - 1;
    bool result = nunboxParts_.append(
        NunboxEntry(typeVreg, LUse(typeVreg, LUse::ANY), payload));
    if (result) {
      assertInvariants();
    }
    return result;
  }

  LAllocation findTypeAllocation(uint32_t typeVreg) {
    // Look for some allocation for the specified type vreg, to go with a
    // partial nunbox entry for the payload. Note that we don't need to
    // look at the value slots in the safepoint, as these aren't used by
    // register allocators which add partial nunbox entries.
    for (size_t i = 0; i < nunboxParts_.length(); i++) {
      if (nunboxParts_[i].typeVreg == typeVreg &&
          !nunboxParts_[i].type.isUse()) {
        return nunboxParts_[i].type;
      }
    }
    return LUse(typeVreg, LUse::ANY);
  }

#  ifdef DEBUG
  bool hasNunboxPayload(LAllocation payload) const {
    for (size_t i = 0; i < nunboxParts_.length(); i++) {
      if (nunboxParts_[i].payload == payload) {
        return true;
      }
    }
    return false;
  }
#  endif

  NunboxList& nunboxParts() { return nunboxParts_; }

#elif JS_PUNBOX64
  [[nodiscard]] bool addValueSlot(bool stack, uint32_t slot) {
    bool result = valueSlots_.append(SlotEntry(stack, slot));
    if (result) {
      assertInvariants();
    }
    return result;
  }
  SlotList& valueSlots() { return valueSlots_; }

  bool hasValueSlot(bool stack, uint32_t slot) const {
    for (size_t i = 0; i < valueSlots_.length(); i++) {
      if (valueSlots_[i].stack == stack && valueSlots_[i].slot == slot) {
        return true;
      }
    }
    return false;
  }

  void addValueRegister(Register reg) {
    valueRegs_.add(reg);
    assertInvariants();
  }
  LiveGeneralRegisterSet valueRegs() const { return valueRegs_; }

  [[nodiscard]] bool addBoxedValue(LAllocation alloc) {
    if (alloc.isRegister()) {
      Register reg = alloc.toRegister().gpr();
      if (!valueRegs().has(reg)) {
        addValueRegister(reg);
      }
      return true;
    }
    if (hasValueSlot(alloc.isStackSlot(), alloc.memorySlot())) {
      return true;
    }
    return addValueSlot(alloc.isStackSlot(), alloc.memorySlot());
  }

  bool hasBoxedValue(LAllocation alloc) const {
    if (alloc.isRegister()) {
      return valueRegs().has(alloc.toRegister().gpr());
    }
    return hasValueSlot(alloc.isStackSlot(), alloc.memorySlot());
  }

#endif  // JS_PUNBOX64

  bool encoded() const { return safepointOffset_ != INVALID_SAFEPOINT_OFFSET; }
  uint32_t offset() const {
    MOZ_ASSERT(encoded());
    return safepointOffset_;
  }
  void setOffset(uint32_t offset) { safepointOffset_ = offset; }
  uint32_t osiReturnPointOffset() const {
    // In general, pointer arithmetic on code is bad, but in this case,
    // getting the return address from a call instruction, stepping over pools
    // would be wrong.
    return osiCallPointOffset_ + Assembler::PatchWrite_NearCallSize();
  }
  uint32_t osiCallPointOffset() const { return osiCallPointOffset_; }
  void setOsiCallPointOffset(uint32_t osiCallPointOffset) {
    MOZ_ASSERT(!osiCallPointOffset_);
    osiCallPointOffset_ = osiCallPointOffset;
  }

  bool isWasmTrap() const { return isWasmTrap_; }
  void setIsWasmTrap() { isWasmTrap_ = true; }

  uint32_t framePushedAtStackMapBase() const {
    return framePushedAtStackMapBase_;
  }
  void setFramePushedAtStackMapBase(uint32_t n) {
    MOZ_ASSERT(framePushedAtStackMapBase_ == 0);
    framePushedAtStackMapBase_ = n;
  }
};

class LInstruction::InputIterator {
 private:
  LInstruction& ins_;
  size_t idx_;
  bool snapshot_;

  void handleOperandsEnd() {
    // Iterate on the snapshot when iteration over all operands is done.
    if (!snapshot_ && idx_ == ins_.numOperands() && ins_.snapshot()) {
      idx_ = 0;
      snapshot_ = true;
    }
  }

 public:
  explicit InputIterator(LInstruction& ins)
      : ins_(ins), idx_(0), snapshot_(false) {
    handleOperandsEnd();
  }

  bool more() const {
    if (snapshot_) {
      return idx_ < ins_.snapshot()->numEntries();
    }
    if (idx_ < ins_.numOperands()) {
      return true;
    }
    if (ins_.snapshot() && ins_.snapshot()->numEntries()) {
      return true;
    }
    return false;
  }

  bool isSnapshotInput() const { return snapshot_; }

  void next() {
    MOZ_ASSERT(more());
    idx_++;
    handleOperandsEnd();
  }

  void replace(const LAllocation& alloc) {
    if (snapshot_) {
      ins_.snapshot()->setEntry(idx_, alloc);
    } else {
      ins_.setOperand(idx_, alloc);
    }
  }

  LAllocation* operator*() const {
    if (snapshot_) {
      return ins_.snapshot()->getEntry(idx_);
    }
    return ins_.getOperand(idx_);
  }

  LAllocation* operator->() const { return **this; }
};

class LIRGraph {
  struct ValueHasher {
    using Lookup = Value;
    static HashNumber hash(const Value& v) { return HashNumber(v.asRawBits()); }
    static bool match(const Value& lhs, const Value& rhs) { return lhs == rhs; }
  };

  FixedList<LBlock> blocks_;

  // constantPool_ is a mozilla::Vector, not a js::Vector, because
  // js::Vector<Value> is prohibited as unsafe. This particular Vector of
  // Values is safe because it is only used within the scope of an
  // AutoSuppressGC (in IonCompile), which inhibits GC.
  mozilla::Vector<Value, 0, JitAllocPolicy> constantPool_;
  typedef HashMap<Value, uint32_t, ValueHasher, JitAllocPolicy> ConstantPoolMap;
  ConstantPoolMap constantPoolMap_;
  Vector<LInstruction*, 0, JitAllocPolicy> safepoints_;
  Vector<LInstruction*, 0, JitAllocPolicy> nonCallSafepoints_;
  uint32_t numVirtualRegisters_;
  uint32_t numInstructions_;

  // Number of stack slots needed for local spills.
  uint32_t localSlotCount_;
  // Number of stack slots needed for argument construction for calls.
  uint32_t argumentSlotCount_;

  MIRGraph& mir_;

 public:
  explicit LIRGraph(MIRGraph* mir);

  [[nodiscard]] bool init() {
    return blocks_.init(mir_.alloc(), mir_.numBlocks());
  }
  MIRGraph& mir() const { return mir_; }
  size_t numBlocks() const { return blocks_.length(); }
  LBlock* getBlock(size_t i) { return &blocks_[i]; }
  uint32_t numBlockIds() const { return mir_.numBlockIds(); }
  [[nodiscard]] bool initBlock(MBasicBlock* mir) {
    auto* block = &blocks_[mir->id()];
    auto* lir = new (block) LBlock(mir);
    return lir->init(mir_.alloc());
  }
  uint32_t getVirtualRegister() {
    numVirtualRegisters_ += VREG_INCREMENT;
    return numVirtualRegisters_;
  }
  uint32_t numVirtualRegisters() const {
    // Virtual registers are 1-based, not 0-based, so add one as a
    // convenience for 0-based arrays.
    return numVirtualRegisters_ + 1;
  }
  uint32_t getInstructionId() { return numInstructions_++; }
  uint32_t numInstructions() const { return numInstructions_; }
  void setLocalSlotCount(uint32_t localSlotCount) {
    localSlotCount_ = localSlotCount;
  }
  uint32_t localSlotCount() const { return localSlotCount_; }
  // Return the localSlotCount() value rounded up so that it satisfies the
  // platform stack alignment requirement, and so that it's a multiple of
  // the number of slots per Value.
  uint32_t paddedLocalSlotCount() const {
    // Round to JitStackAlignment, and implicitly to sizeof(Value) as
    // JitStackAlignment is a multiple of sizeof(Value). These alignments
    // are needed for spilling SIMD registers properly, and for
    // StackOffsetOfPassedArg which rounds argument slots to 8-byte
    // boundaries.
    return AlignBytes(localSlotCount(), JitStackAlignment);
  }
  size_t paddedLocalSlotsSize() const { return paddedLocalSlotCount(); }
  void setArgumentSlotCount(uint32_t argumentSlotCount) {
    argumentSlotCount_ = argumentSlotCount;
  }
  uint32_t argumentSlotCount() const { return argumentSlotCount_; }
  size_t argumentsSize() const { return argumentSlotCount() * sizeof(Value); }
  uint32_t totalSlotCount() const {
    return paddedLocalSlotCount() + argumentsSize();
  }
  [[nodiscard]] bool addConstantToPool(const Value& v, uint32_t* index);
  size_t numConstants() const { return constantPool_.length(); }
  Value* constantPool() { return &constantPool_[0]; }

  bool noteNeedsSafepoint(LInstruction* ins);
  size_t numNonCallSafepoints() const { return nonCallSafepoints_.length(); }
  LInstruction* getNonCallSafepoint(size_t i) const {
    return nonCallSafepoints_[i];
  }
  size_t numSafepoints() const { return safepoints_.length(); }
  LInstruction* getSafepoint(size_t i) const { return safepoints_[i]; }

#ifdef JS_JITSPEW
  void dump(GenericPrinter& out);
  void dump();
#endif
};

LAllocation::LAllocation(AnyRegister reg) {
  if (reg.isFloat()) {
    *this = LFloatReg(reg.fpu());
  } else {
    *this = LGeneralReg(reg.gpr());
  }
}

AnyRegister LAllocation::toRegister() const {
  MOZ_ASSERT(isRegister());
  if (isFloatReg()) {
    return AnyRegister(toFloatReg()->reg());
  }
  return AnyRegister(toGeneralReg()->reg());
}

}  // namespace jit
}  // namespace js

#include "jit/shared/LIR-shared.h"
#if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
#  if defined(JS_CODEGEN_X86)
#    include "jit/x86/LIR-x86.h"
#  elif defined(JS_CODEGEN_X64)
#    include "jit/x64/LIR-x64.h"
#  endif
#  include "jit/x86-shared/LIR-x86-shared.h"
#elif defined(JS_CODEGEN_ARM)
#  include "jit/arm/LIR-arm.h"
#elif defined(JS_CODEGEN_ARM64)
#  include "jit/arm64/LIR-arm64.h"
#elif defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
#  if defined(JS_CODEGEN_MIPS32)
#    include "jit/mips32/LIR-mips32.h"
#  elif defined(JS_CODEGEN_MIPS64)
#    include "jit/mips64/LIR-mips64.h"
#  endif
#  include "jit/mips-shared/LIR-mips-shared.h"
#elif defined(JS_CODEGEN_NONE)
#  include "jit/none/LIR-none.h"
#else
#  error "Unknown architecture!"
#endif

#undef LIR_HEADER

namespace js {
namespace jit {

#define LIROP(name)                           \
  L##name* LNode::to##name() {                \
    MOZ_ASSERT(is##name());                   \
    return static_cast<L##name*>(this);       \
  }                                           \
  const L##name* LNode::to##name() const {    \
    MOZ_ASSERT(is##name());                   \
    return static_cast<const L##name*>(this); \
  }
LIR_OPCODE_LIST(LIROP)
#undef LIROP

#define LALLOC_CAST(type)               \
  L##type* LAllocation::to##type() {    \
    MOZ_ASSERT(is##type());             \
    return static_cast<L##type*>(this); \
  }
#define LALLOC_CONST_CAST(type)                  \
  const L##type* LAllocation::to##type() const { \
    MOZ_ASSERT(is##type());                      \
    return static_cast<const L##type*>(this);    \
  }

LALLOC_CAST(Use)
LALLOC_CONST_CAST(Use)
LALLOC_CONST_CAST(GeneralReg)
LALLOC_CONST_CAST(FloatReg)
LALLOC_CONST_CAST(StackSlot)
LALLOC_CAST(StackArea)
LALLOC_CONST_CAST(StackArea)
LALLOC_CONST_CAST(Argument)
LALLOC_CONST_CAST(ConstantIndex)

#undef LALLOC_CAST

}  // namespace jit
}  // namespace js

#endif /* jit_LIR_h */

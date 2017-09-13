/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_LIR_h
#define jit_LIR_h

// This file declares the core data structures for LIR: storage allocations for
// inputs and outputs, as well as the interface instructions must conform to.

#include "mozilla/Array.h"

#include "jit/Bailouts.h"
#include "jit/FixedList.h"
#include "jit/InlineList.h"
#include "jit/JitAllocPolicy.h"
#include "jit/LOpcodes.h"
#include "jit/MIR.h"
#include "jit/MIRGraph.h"
#include "jit/Registers.h"
#include "jit/Safepoints.h"

namespace js {
namespace jit {

class LUse;
class LGeneralReg;
class LFloatReg;
class LStackSlot;
class LArgument;
class LConstantIndex;
class MBasicBlock;
class MIRGenerator;

static const uint32_t VREG_INCREMENT = 1;

static const uint32_t THIS_FRAME_ARGSLOT = 0;

#if defined(JS_NUNBOX32)
# define BOX_PIECES         2
static const uint32_t VREG_TYPE_OFFSET = 0;
static const uint32_t VREG_DATA_OFFSET = 1;
static const uint32_t TYPE_INDEX = 0;
static const uint32_t PAYLOAD_INDEX = 1;
#elif defined(JS_PUNBOX64)
# define BOX_PIECES         1
#else
# error "Unknown!"
#endif

// Represents storage for an operand. For constants, the pointer is tagged
// with a single bit, and the untagged pointer is a pointer to a Value.
class LAllocation : public TempObject
{
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
        CONSTANT_VALUE, // Constant js::Value.
        CONSTANT_INDEX, // Constant arbitrary index.
        USE,            // Use of a virtual register, with physical allocation policy.
        GPR,            // General purpose register.
        FPU,            // Floating-point register.
        STACK_SLOT,     // Stack slot.
        ARGUMENT_SLOT   // Argument slot.
    };

    static const uintptr_t DATA_MASK = (1 << DATA_BITS) - 1;

  protected:
    uint32_t data() const {
        return uint32_t(bits_) >> DATA_SHIFT;
    }
    void setData(uint32_t data) {
        MOZ_ASSERT(data <= DATA_MASK);
        bits_ &= ~(DATA_MASK << DATA_SHIFT);
        bits_ |= (data << DATA_SHIFT);
    }
    void setKindAndData(Kind kind, uint32_t data) {
        MOZ_ASSERT(data <= DATA_MASK);
        bits_ = (uint32_t(kind) << KIND_SHIFT) | data << DATA_SHIFT;
    }

    LAllocation(Kind kind, uint32_t data) {
        setKindAndData(kind, data);
    }
    explicit LAllocation(Kind kind) {
        setKindAndData(kind, 0);
    }

  public:
    LAllocation() : bits_(0)
    {
        MOZ_ASSERT(isBogus());
    }

    // The value pointer must be rooted in MIR and have its low bits cleared.
    explicit LAllocation(const Value* vp) {
        MOZ_ASSERT(vp);
        bits_ = uintptr_t(vp);
        MOZ_ASSERT((bits_ & (KIND_MASK << KIND_SHIFT)) == 0);
        bits_ |= CONSTANT_VALUE << KIND_SHIFT;
    }
    inline explicit LAllocation(AnyRegister reg);

    Kind kind() const {
        return (Kind)((bits_ >> KIND_SHIFT) & KIND_MASK);
    }

    bool isBogus() const {
        return bits_ == 0;
    }
    bool isUse() const {
        return kind() == USE;
    }
    bool isConstant() const {
        return isConstantValue() || isConstantIndex();
    }
    bool isConstantValue() const {
        return kind() == CONSTANT_VALUE;
    }
    bool isConstantIndex() const {
        return kind() == CONSTANT_INDEX;
    }
    bool isGeneralReg() const {
        return kind() == GPR;
    }
    bool isFloatReg() const {
        return kind() == FPU;
    }
    bool isStackSlot() const {
        return kind() == STACK_SLOT;
    }
    bool isArgument() const {
        return kind() == ARGUMENT_SLOT;
    }
    bool isRegister() const {
        return isGeneralReg() || isFloatReg();
    }
    bool isRegister(bool needFloat) const {
        return needFloat ? isFloatReg() : isGeneralReg();
    }
    bool isMemory() const {
        return isStackSlot() || isArgument();
    }
    inline uint32_t memorySlot() const;
    inline LUse* toUse();
    inline const LUse* toUse() const;
    inline const LGeneralReg* toGeneralReg() const;
    inline const LFloatReg* toFloatReg() const;
    inline const LStackSlot* toStackSlot() const;
    inline const LArgument* toArgument() const;
    inline const LConstantIndex* toConstantIndex() const;
    inline AnyRegister toRegister() const;

    const Value* toConstant() const {
        MOZ_ASSERT(isConstantValue());
        return reinterpret_cast<const Value*>(bits_ & ~(KIND_MASK << KIND_SHIFT));
    }

    bool operator ==(const LAllocation& other) const {
        return bits_ == other.bits_;
    }

    bool operator !=(const LAllocation& other) const {
        return bits_ != other.bits_;
    }

    HashNumber hash() const {
        return bits_;
    }

    const char* toString() const;
    bool aliases(const LAllocation& other) const;
    void dump() const;

};

class LUse : public LAllocation
{
    static const uint32_t POLICY_BITS = 3;
    static const uint32_t POLICY_SHIFT = 0;
    static const uint32_t POLICY_MASK = (1 << POLICY_BITS) - 1;
    static const uint32_t REG_BITS = 6;
    static const uint32_t REG_SHIFT = POLICY_SHIFT + POLICY_BITS;
    static const uint32_t REG_MASK = (1 << REG_BITS) - 1;

    // Whether the physical register for this operand may be reused for a def.
    static const uint32_t USED_AT_START_BITS = 1;
    static const uint32_t USED_AT_START_SHIFT = REG_SHIFT + REG_BITS;
    static const uint32_t USED_AT_START_MASK = (1 << USED_AT_START_BITS) - 1;

  public:
    // Virtual registers get the remaining 19 bits.
    static const uint32_t VREG_BITS = DATA_BITS - (USED_AT_START_SHIFT + USED_AT_START_BITS);
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

        // For snapshot inputs, indicates that the associated instruction will
        // write this input to its output register before bailing out.
        // The register allocator may thus allocate that output register, and
        // does not need to keep the virtual register alive (alternatively,
        // this may be treated as KEEPALIVE).
        RECOVERED_INPUT
    };

    void set(Policy policy, uint32_t reg, bool usedAtStart) {
        setKindAndData(USE, (policy << POLICY_SHIFT) |
                            (reg << REG_SHIFT) |
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
    bool isFixedRegister() const {
        return policy() == FIXED;
    }
    bool usedAtStart() const {
        return !!((data() >> USED_AT_START_SHIFT) & USED_AT_START_MASK);
    }
};

static const uint32_t MAX_VIRTUAL_REGISTERS = LUse::VREG_MASK;

class LGeneralReg : public LAllocation
{
  public:
    explicit LGeneralReg(Register reg)
      : LAllocation(GPR, reg.code())
    { }

    Register reg() const {
        return Register::FromCode(data());
    }
};

class LFloatReg : public LAllocation
{
  public:
    explicit LFloatReg(FloatRegister reg)
      : LAllocation(FPU, reg.code())
    { }

    FloatRegister reg() const {
        return FloatRegister::FromCode(data());
    }
};

// Arbitrary constant index.
class LConstantIndex : public LAllocation
{
    explicit LConstantIndex(uint32_t index)
      : LAllocation(CONSTANT_INDEX, index)
    { }

  public:
    static LConstantIndex FromIndex(uint32_t index) {
        return LConstantIndex(index);
    }

    uint32_t index() const {
        return data();
    }
};

// Stack slots are indices into the stack. The indices are byte indices.
class LStackSlot : public LAllocation
{
  public:
    explicit LStackSlot(uint32_t slot)
      : LAllocation(STACK_SLOT, slot)
    { }

    uint32_t slot() const {
        return data();
    }
};

// Arguments are reverse indices into the stack. The indices are byte indices.
class LArgument : public LAllocation
{
  public:
    explicit LArgument(uint32_t index)
      : LAllocation(ARGUMENT_SLOT, index)
    { }

    uint32_t index() const {
        return data();
    }
};

inline uint32_t
LAllocation::memorySlot() const
{
    MOZ_ASSERT(isMemory());
    return isStackSlot() ? toStackSlot()->slot() : toArgument()->index();
}

// Represents storage for a definition.
class LDefinition
{
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

    static const uint32_t VREG_BITS = (sizeof(uint32_t) * 8) - (POLICY_BITS + TYPE_BITS);
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

        // One definition per instruction must re-use the first input
        // allocation, which (for now) must be a register.
        MUST_REUSE_INPUT
    };

    // This should be kept in sync with LIR.cpp's TypeChars.
    enum Type {
        GENERAL,    // Generic, integer or pointer-width data (GPR).
        INT32,      // int32 data (GPR).
        OBJECT,     // Pointer that may be collected as garbage (GPR).
        SLOTS,      // Slots/elements pointer that may be moved by minor GCs (GPR).
        FLOAT32,    // 32-bit floating-point value (FPU).
        DOUBLE,     // 64-bit floating-point value (FPU).
        INT32X4,    // SIMD data containing four 32-bit integers (FPU).
        FLOAT32X4,  // SIMD data containing four 32-bit floats (FPU).
        SINCOS,
#ifdef JS_NUNBOX32
        // A type virtual register must be followed by a payload virtual
        // register, as both will be tracked as a single gcthing.
        TYPE,
        PAYLOAD
#else
        BOX         // Joined box, for punbox systems. (GPR, gcthing)
#endif
    };

    void set(uint32_t index, Type type, Policy policy) {
        JS_STATIC_ASSERT(MAX_VIRTUAL_REGISTERS <= VREG_MASK);
        bits_ = (index << VREG_SHIFT) | (policy << POLICY_SHIFT) | (type << TYPE_SHIFT);
        MOZ_ASSERT_IF(!SupportsSimd, !isSimdType());
    }

  public:
    LDefinition(uint32_t index, Type type, Policy policy = REGISTER) {
        set(index, type, policy);
    }

    explicit LDefinition(Type type, Policy policy = REGISTER) {
        set(0, type, policy);
    }

    LDefinition(Type type, const LAllocation& a)
      : output_(a)
    {
        set(0, type, FIXED);
    }

    LDefinition(uint32_t index, Type type, const LAllocation& a)
      : output_(a)
    {
        set(index, type, FIXED);
    }

    LDefinition() : bits_(0)
    {
        MOZ_ASSERT(isBogusTemp());
    }

    static LDefinition BogusTemp() {
        return LDefinition();
    }

    Policy policy() const {
        return (Policy)((bits_ >> POLICY_SHIFT) & POLICY_MASK);
    }
    Type type() const {
        return (Type)((bits_ >> TYPE_SHIFT) & TYPE_MASK);
    }
    bool isSimdType() const {
        return type() == INT32X4 || type() == FLOAT32X4;
    }
    bool isCompatibleReg(const AnyRegister& r) const {
        if (isFloatReg() && r.isFloat()) {
            if (type() == FLOAT32)
                return r.fpu().isSingle();
            if (type() == DOUBLE)
                return r.fpu().isDouble();
            if (isSimdType())
                return r.fpu().isSimd128();
            MOZ_CRASH("Unexpected MDefinition type");
        }
        return !isFloatReg() && !r.isFloat();
    }
    bool isCompatibleDef(const LDefinition& other) const {
#if defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_MIPS32)
        if (isFloatReg() && other.isFloatReg())
            return type() == other.type();
        return !isFloatReg() && !other.isFloatReg();
#else
        return isFloatReg() == other.isFloatReg();
#endif
    }

    bool isFloatReg() const {
        return type() == FLOAT32 || type() == DOUBLE || isSimdType();
    }
    uint32_t virtualRegister() const {
        uint32_t index = (bits_ >> VREG_SHIFT) & VREG_MASK;
        //MOZ_ASSERT(index != 0);
        return index;
    }
    LAllocation* output() {
        return &output_;
    }
    const LAllocation* output() const {
        return &output_;
    }
    bool isFixed() const {
        return policy() == FIXED;
    }
    bool isBogusTemp() const {
        return isFixed() && output()->isBogus();
    }
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
          case MIRType_Boolean:
          case MIRType_Int32:
            // The stack slot allocator doesn't currently support allocating
            // 1-byte slots, so for now we lower MIRType_Boolean into INT32.
            static_assert(sizeof(bool) <= sizeof(int32_t), "bool doesn't fit in an int32 slot");
            return LDefinition::INT32;
          case MIRType_String:
          case MIRType_Symbol:
          case MIRType_Object:
          case MIRType_ObjectOrNull:
            return LDefinition::OBJECT;
          case MIRType_Double:
            return LDefinition::DOUBLE;
          case MIRType_Float32:
            return LDefinition::FLOAT32;
#if defined(JS_PUNBOX64)
          case MIRType_Value:
            return LDefinition::BOX;
#endif
          case MIRType_SinCosDouble:
            return LDefinition::SINCOS;
          case MIRType_Slots:
          case MIRType_Elements:
            return LDefinition::SLOTS;
          case MIRType_Pointer:
            return LDefinition::GENERAL;
          case MIRType_Int32x4:
            return LDefinition::INT32X4;
          case MIRType_Float32x4:
            return LDefinition::FLOAT32X4;
          default:
            MOZ_CRASH("unexpected type");
        }
    }

    const char* toString() const;

    void dump() const;
};

// Forward declarations of LIR types.
#define LIROP(op) class L##op;
    LIR_OPCODE_LIST(LIROP)
#undef LIROP

class LSnapshot;
class LSafepoint;
class LInstruction;
class LElementVisitor;

// The common base class for LPhi and LInstruction.
class LNode
{
    uint32_t id_;
    LBlock* block_;

  protected:
    MDefinition* mir_;

  public:
    LNode()
      : id_(0),
        block_(nullptr),
        mir_(nullptr)
    { }

    enum Opcode {
#   define LIROP(name) LOp_##name,
        LIR_OPCODE_LIST(LIROP)
#   undef LIROP
        LOp_Invalid
    };

    const char* opName() {
        switch (op()) {
#   define LIR_NAME_INS(name)                   \
            case LOp_##name: return #name;
            LIR_OPCODE_LIST(LIR_NAME_INS)
#   undef LIR_NAME_INS
          default:
            return "Invalid";
        }
    }

    // Hook for opcodes to add extra high level detail about what code will be
    // emitted for the op.
    virtual const char* extraName() const {
        return nullptr;
    }

    virtual Opcode op() const = 0;

    bool isInstruction() const {
        return op() != LOp_Phi;
    }
    inline LInstruction* toInstruction();
    inline const LInstruction* toInstruction() const;

    // Returns the number of outputs of this instruction. If an output is
    // unallocated, it is an LDefinition, defining a virtual register.
    virtual size_t numDefs() const = 0;
    virtual LDefinition* getDef(size_t index) = 0;
    virtual void setDef(size_t index, const LDefinition& def) = 0;

    // Returns information about operands.
    virtual size_t numOperands() const = 0;
    virtual LAllocation* getOperand(size_t index) = 0;
    virtual void setOperand(size_t index, const LAllocation& a) = 0;

    // Returns information about temporary registers needed. Each temporary
    // register is an LDefinition with a fixed or virtual register and
    // either GENERAL, FLOAT32, or DOUBLE type.
    virtual size_t numTemps() const = 0;
    virtual LDefinition* getTemp(size_t index) = 0;
    virtual void setTemp(size_t index, const LDefinition& a) = 0;

    // Returns the number of successors of this instruction, if it is a control
    // transfer instruction, or zero otherwise.
    virtual size_t numSuccessors() const = 0;
    virtual MBasicBlock* getSuccessor(size_t i) const = 0;
    virtual void setSuccessor(size_t i, MBasicBlock* successor) = 0;

    virtual bool isCall() const {
        return false;
    }
    uint32_t id() const {
        return id_;
    }
    void setId(uint32_t id) {
        MOZ_ASSERT(!id_);
        MOZ_ASSERT(id);
        id_ = id;
    }
    void setMir(MDefinition* mir) {
        mir_ = mir;
    }
    MDefinition* mirRaw() const {
        /* Untyped MIR for this op. Prefer mir() methods in subclasses. */
        return mir_;
    }
    LBlock* block() const {
        return block_;
    }
    void setBlock(LBlock* block) {
        block_ = block;
    }

    // For an instruction which has a MUST_REUSE_INPUT output, whether that
    // output register will be restored to its original value when bailing out.
    virtual bool recoversInput() const {
        return false;
    }

    virtual void dump(GenericPrinter& out);
    void dump();
    static void printName(GenericPrinter& out, Opcode op);
    virtual void printName(GenericPrinter& out);
    virtual void printOperands(GenericPrinter& out);

  public:
    // Opcode testing and casts.
#   define LIROP(name)                                                      \
    bool is##name() const {                                                 \
        return op() == LOp_##name;                                          \
    }                                                                       \
    inline L##name* to##name();                                             \
    inline const L##name* to##name() const;
    LIR_OPCODE_LIST(LIROP)
#   undef LIROP

    virtual void accept(LElementVisitor* visitor) = 0;

#define LIR_HEADER(opcode)                                                  \
    Opcode op() const {                                                     \
        return LInstruction::LOp_##opcode;                                  \
    }                                                                       \
    void accept(LElementVisitor* visitor) {                                 \
        visitor->setElement(this);                                          \
        visitor->visit##opcode(this);                                       \
    }
};

class LInstruction
  : public LNode
  , public TempObject
  , public InlineListNode<LInstruction>
{
    // This snapshot could be set after a ResumePoint.  It is used to restart
    // from the resume point pc.
    LSnapshot* snapshot_;

    // Structure capturing the set of stack slots and registers which are known
    // to hold either gcthings or Values.
    LSafepoint* safepoint_;

    LMoveGroup* inputMoves_;
    LMoveGroup* movesAfter_;

  protected:
    LInstruction()
      : snapshot_(nullptr),
        safepoint_(nullptr),
        inputMoves_(nullptr),
        movesAfter_(nullptr)
    { }

  public:
    LSnapshot* snapshot() const {
        return snapshot_;
    }
    LSafepoint* safepoint() const {
        return safepoint_;
    }
    LMoveGroup* inputMoves() const {
        return inputMoves_;
    }
    void setInputMoves(LMoveGroup* moves) {
        inputMoves_ = moves;
    }
    LMoveGroup* movesAfter() const {
        return movesAfter_;
    }
    void setMovesAfter(LMoveGroup* moves) {
        movesAfter_ = moves;
    }
    void assignSnapshot(LSnapshot* snapshot);
    void initSafepoint(TempAllocator& alloc);

    class InputIterator;
};

LInstruction*
LNode::toInstruction()
{
    MOZ_ASSERT(isInstruction());
    return static_cast<LInstruction*>(this);
}

const LInstruction*
LNode::toInstruction() const
{
    MOZ_ASSERT(isInstruction());
    return static_cast<const LInstruction*>(this);
}

class LElementVisitor
{
    LNode* ins_;

  protected:
    jsbytecode* lastPC_;
    jsbytecode* lastNotInlinedPC_;

    LNode* instruction() {
        return ins_;
    }

  public:
    void setElement(LNode* ins) {
        ins_ = ins;
        if (ins->mirRaw()) {
            lastPC_ = ins->mirRaw()->trackedPc();
            if (ins->mirRaw()->trackedTree())
                lastNotInlinedPC_ = ins->mirRaw()->profilerLeavePc();
        }
    }

    LElementVisitor()
      : ins_(nullptr),
        lastPC_(nullptr),
        lastNotInlinedPC_(nullptr)
    {}

  public:
#define VISIT_INS(op) virtual void visit##op(L##op*) { MOZ_CRASH("NYI: " #op); }
    LIR_OPCODE_LIST(VISIT_INS)
#undef VISIT_INS
};

typedef InlineList<LInstruction>::iterator LInstructionIterator;
typedef InlineList<LInstruction>::reverse_iterator LInstructionReverseIterator;

class MPhi;

// Phi is a pseudo-instruction that emits no code, and is an annotation for the
// register allocator. Like its equivalent in MIR, phis are collected at the
// top of blocks and are meant to be executed in parallel, choosing the input
// corresponding to the predecessor taken in the control flow graph.
class LPhi final : public LNode
{
    LAllocation* const inputs_;
    LDefinition def_;

  public:
    LIR_HEADER(Phi)

    LPhi(MPhi* ins, LAllocation* inputs)
        : inputs_(inputs)
    {
        setMir(ins);
    }

    size_t numDefs() const {
        return 1;
    }
    LDefinition* getDef(size_t index) {
        MOZ_ASSERT(index == 0);
        return &def_;
    }
    void setDef(size_t index, const LDefinition& def) {
        MOZ_ASSERT(index == 0);
        def_ = def;
    }
    size_t numOperands() const {
        return mir_->toPhi()->numOperands();
    }
    LAllocation* getOperand(size_t index) {
        MOZ_ASSERT(index < numOperands());
        return &inputs_[index];
    }
    void setOperand(size_t index, const LAllocation& a) {
        MOZ_ASSERT(index < numOperands());
        inputs_[index] = a;
    }
    size_t numTemps() const {
        return 0;
    }
    LDefinition* getTemp(size_t index) {
        MOZ_CRASH("no temps");
    }
    void setTemp(size_t index, const LDefinition& temp) {
        MOZ_CRASH("no temps");
    }
    size_t numSuccessors() const {
        return 0;
    }
    MBasicBlock* getSuccessor(size_t i) const {
        MOZ_CRASH("no successors");
    }
    void setSuccessor(size_t i, MBasicBlock*) {
        MOZ_CRASH("no successors");
    }
};

class LMoveGroup;
class LBlock
{
    MBasicBlock* block_;
    FixedList<LPhi> phis_;
    InlineList<LInstruction> instructions_;
    LMoveGroup* entryMoveGroup_;
    LMoveGroup* exitMoveGroup_;
    Label label_;

  public:
    explicit LBlock(MBasicBlock* block);
    bool init(TempAllocator& alloc);

    void add(LInstruction* ins) {
        ins->setBlock(this);
        instructions_.pushBack(ins);
    }
    size_t numPhis() const {
        return phis_.length();
    }
    LPhi* getPhi(size_t index) {
        return &phis_[index];
    }
    const LPhi* getPhi(size_t index) const {
        return &phis_[index];
    }
    MBasicBlock* mir() const {
        return block_;
    }
    LInstructionIterator begin() {
        return instructions_.begin();
    }
    LInstructionIterator begin(LInstruction* at) {
        return instructions_.begin(at);
    }
    LInstructionIterator end() {
        return instructions_.end();
    }
    LInstructionReverseIterator rbegin() {
        return instructions_.rbegin();
    }
    LInstructionReverseIterator rbegin(LInstruction* at) {
        return instructions_.rbegin(at);
    }
    LInstructionReverseIterator rend() {
        return instructions_.rend();
    }
    InlineList<LInstruction>& instructions() {
        return instructions_;
    }
    void insertAfter(LInstruction* at, LInstruction* ins) {
        instructions_.insertAfter(at, ins);
    }
    void insertBefore(LInstruction* at, LInstruction* ins) {
        instructions_.insertBefore(at, ins);
    }
    const LNode* firstElementWithId() const {
        return !phis_.empty()
               ? static_cast<const LNode*>(getPhi(0))
               : firstInstructionWithId();
    }
    uint32_t firstId() const {
        return firstElementWithId()->id();
    }
    uint32_t lastId() const {
        return lastInstructionWithId()->id();
    }
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
    bool isTrivial() {
        return begin()->isGoto() && !mir()->isLoopHeader();
    }

    void dump(GenericPrinter& out);
    void dump();
};

namespace details {
    template <size_t Defs, size_t Temps>
    class LInstructionFixedDefsTempsHelper : public LInstruction
    {
        mozilla::Array<LDefinition, Defs> defs_;
        mozilla::Array<LDefinition, Temps> temps_;

      public:
        size_t numDefs() const final override {
            return Defs;
        }
        LDefinition* getDef(size_t index) final override {
            return &defs_[index];
        }
        size_t numTemps() const final override {
            return Temps;
        }
        LDefinition* getTemp(size_t index) final override {
            return &temps_[index];
        }

        void setDef(size_t index, const LDefinition& def) final override {
            defs_[index] = def;
        }
        void setTemp(size_t index, const LDefinition& a) final override {
            temps_[index] = a;
        }

        size_t numSuccessors() const override {
            return 0;
        }
        MBasicBlock* getSuccessor(size_t i) const override {
            MOZ_ASSERT(false);
            return nullptr;
        }
        void setSuccessor(size_t i, MBasicBlock* successor) override {
            MOZ_ASSERT(false);
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
    };
} // namespace details

template <size_t Defs, size_t Operands, size_t Temps>
class LInstructionHelper : public details::LInstructionFixedDefsTempsHelper<Defs, Temps>
{
    mozilla::Array<LAllocation, Operands> operands_;

  public:
    size_t numOperands() const final override {
        return Operands;
    }
    LAllocation* getOperand(size_t index) final override {
        return &operands_[index];
    }
    void setOperand(size_t index, const LAllocation& a) final override {
        operands_[index] = a;
    }
};

template<size_t Defs, size_t Temps>
class LVariadicInstruction : public details::LInstructionFixedDefsTempsHelper<Defs, Temps>
{
    FixedList<LAllocation> operands_;

  public:
    bool init(TempAllocator& alloc, size_t length) {
        return operands_.init(alloc, length);
    }
    size_t numOperands() const final override {
        return operands_.length();
    }
    LAllocation* getOperand(size_t index) final override {
        return &operands_[index];
    }
    void setOperand(size_t index, const LAllocation& a) final override {
        operands_[index] = a;
    }
};

template <size_t Defs, size_t Operands, size_t Temps>
class LCallInstructionHelper : public LInstructionHelper<Defs, Operands, Temps>
{
  public:
    virtual bool isCall() const {
        return true;
    }
};

class LRecoverInfo : public TempObject
{
  public:
    typedef Vector<MNode*, 2, JitAllocPolicy> Instructions;

  private:
    // List of instructions needed to recover the stack frames.
    // Outer frames are stored before inner frames.
    Instructions instructions_;

    // Cached offset where this resume point is encoded.
    RecoverOffset recoverOffset_;

    explicit LRecoverInfo(TempAllocator& alloc);
    bool init(MResumePoint* mir);

    // Fill the instruction vector such as all instructions needed for the
    // recovery are pushed before the current instruction.
    bool appendOperands(MNode* ins);
    bool appendDefinition(MDefinition* def);
    bool appendResumePoint(MResumePoint* rp);
  public:
    static LRecoverInfo* New(MIRGenerator* gen, MResumePoint* mir);

    // Resume point of the inner most function.
    MResumePoint* mir() const {
        return instructions_.back()->toResumePoint();
    }
    RecoverOffset recoverOffset() const {
        return recoverOffset_;
    }
    void setRecoverOffset(RecoverOffset offset) {
        MOZ_ASSERT(recoverOffset_ == INVALID_RECOVER_OFFSET);
        recoverOffset_ = offset;
    }

    MNode** begin() {
        return instructions_.begin();
    }
    MNode** end() {
        return instructions_.end();
    }
    size_t numInstructions() const {
        return instructions_.length();
    }

    class OperandIter
    {
      private:
        MNode** it_;
        MNode** end_;
        size_t op_;

      public:
        explicit OperandIter(LRecoverInfo* recoverInfo)
          : it_(recoverInfo->begin()), end_(recoverInfo->end()), op_(0)
        {
            settle();
        }

        void settle() {
            while ((*it_)->numOperands() == 0) {
                ++it_;
                op_ = 0;
            }
        }

        MDefinition* operator*() {
            return (*it_)->getOperand(op_);
        }
        MDefinition* operator ->() {
            return (*it_)->getOperand(op_);
        }

        OperandIter& operator ++() {
            ++op_;
            if (op_ == (*it_)->numOperands()) {
                op_ = 0;
                ++it_;
            }
            if (!*this)
                settle();

            return *this;
        }

        explicit operator bool() const {
            return it_ == end_;
        }

#ifdef DEBUG
        bool canOptimizeOutIfUnused();
#endif
    };
};

// An LSnapshot is the reflection of an MResumePoint in LIR. Unlike MResumePoints,
// they cannot be shared, as they are filled in by the register allocator in
// order to capture the precise low-level stack state in between an
// instruction's input and output. During code generation, LSnapshots are
// compressed and saved in the compiled script.
class LSnapshot : public TempObject
{
  private:
    uint32_t numSlots_;
    LAllocation* slots_;
    LRecoverInfo* recoverInfo_;
    SnapshotOffset snapshotOffset_;
    BailoutId bailoutId_;
    BailoutKind bailoutKind_;

    LSnapshot(LRecoverInfo* recover, BailoutKind kind);
    bool init(MIRGenerator* gen);

  public:
    static LSnapshot* New(MIRGenerator* gen, LRecoverInfo* recover, BailoutKind kind);

    size_t numEntries() const {
        return numSlots_;
    }
    size_t numSlots() const {
        return numSlots_ / BOX_PIECES;
    }
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
    LRecoverInfo* recoverInfo() const {
        return recoverInfo_;
    }
    MResumePoint* mir() const {
        return recoverInfo()->mir();
    }
    SnapshotOffset snapshotOffset() const {
        return snapshotOffset_;
    }
    BailoutId bailoutId() const {
        return bailoutId_;
    }
    void setSnapshotOffset(SnapshotOffset offset) {
        MOZ_ASSERT(snapshotOffset_ == INVALID_SNAPSHOT_OFFSET);
        snapshotOffset_ = offset;
    }
    void setBailoutId(BailoutId id) {
        MOZ_ASSERT(bailoutId_ == INVALID_BAILOUT_ID);
        bailoutId_ = id;
    }
    BailoutKind bailoutKind() const {
        return bailoutKind_;
    }
    void setBailoutKind(BailoutKind kind) {
        bailoutKind_ = kind;
    }
    void rewriteRecoveredInput(LUse input);
};

struct SafepointSlotEntry {
    // Flag indicating whether this is a slot in the stack or argument space.
    uint32_t stack:1;

    // Byte offset of the slot, as in LStackSlot or LArgument.
    uint32_t slot:31;

    SafepointSlotEntry() { }
    SafepointSlotEntry(bool stack, uint32_t slot)
      : stack(stack), slot(slot)
    { }
    explicit SafepointSlotEntry(const LAllocation* a)
      : stack(a->isStackSlot()), slot(a->memorySlot())
    { }
};

struct SafepointNunboxEntry {
    uint32_t typeVreg;
    LAllocation type;
    LAllocation payload;

    SafepointNunboxEntry() { }
    SafepointNunboxEntry(uint32_t typeVreg, LAllocation type, LAllocation payload)
      : typeVreg(typeVreg), type(type), payload(payload)
    { }
};

class LSafepoint : public TempObject
{
    typedef SafepointSlotEntry SlotEntry;
    typedef SafepointNunboxEntry NunboxEntry;

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

    // List of slots which have Values.
    SlotList valueSlots_;

#ifdef JS_NUNBOX32
    // List of registers (in liveRegs) and slots which contain pieces of Values.
    NunboxList nunboxParts_;
#elif JS_PUNBOX64
    // The subset of liveRegs which have Values.
    LiveGeneralRegisterSet valueRegs_;
#endif

    // The subset of liveRegs which contains pointers to slots/elements.
    LiveGeneralRegisterSet slotsOrElementsRegs_;

    // List of slots which have slots/elements pointers.
    SlotList slotsOrElementsSlots_;

  public:
    void assertInvariants() {
        // Every register in valueRegs and gcRegs should also be in liveRegs.
#ifndef JS_NUNBOX32
        MOZ_ASSERT((valueRegs().bits() & ~liveRegs().gprs().bits()) == 0);
#endif
        MOZ_ASSERT((gcRegs().bits() & ~liveRegs().gprs().bits()) == 0);
    }

    explicit LSafepoint(TempAllocator& alloc)
      : safepointOffset_(INVALID_SAFEPOINT_OFFSET)
      , osiCallPointOffset_(0)
      , gcSlots_(alloc)
      , valueSlots_(alloc)
#ifdef JS_NUNBOX32
      , nunboxParts_(alloc)
#endif
      , slotsOrElementsSlots_(alloc)
    {
      assertInvariants();
    }
    void addLiveRegister(AnyRegister reg) {
        liveRegs_.addUnchecked(reg);
        assertInvariants();
    }
    const LiveRegisterSet& liveRegs() const {
        return liveRegs_;
    }
#ifdef CHECK_OSIPOINT_REGISTERS
    void addClobberedRegister(AnyRegister reg) {
        clobberedRegs_.addUnchecked(reg);
        assertInvariants();
    }
    const LiveRegisterSet& clobberedRegs() const {
        return clobberedRegs_;
    }
#endif
    void addGcRegister(Register reg) {
        gcRegs_.addUnchecked(reg);
        assertInvariants();
    }
    LiveGeneralRegisterSet gcRegs() const {
        return gcRegs_;
    }
    bool addGcSlot(bool stack, uint32_t slot) {
        bool result = gcSlots_.append(SlotEntry(stack, slot));
        if (result)
            assertInvariants();
        return result;
    }
    SlotList& gcSlots() {
        return gcSlots_;
    }

    SlotList& slotsOrElementsSlots() {
        return slotsOrElementsSlots_;
    }
    LiveGeneralRegisterSet slotsOrElementsRegs() const {
        return slotsOrElementsRegs_;
    }
    void addSlotsOrElementsRegister(Register reg) {
        slotsOrElementsRegs_.addUnchecked(reg);
        assertInvariants();
    }
    bool addSlotsOrElementsSlot(bool stack, uint32_t slot) {
        bool result = slotsOrElementsSlots_.append(SlotEntry(stack, slot));
        if (result)
            assertInvariants();
        return result;
    }
    bool addSlotsOrElementsPointer(LAllocation alloc) {
        if (alloc.isMemory())
            return addSlotsOrElementsSlot(alloc.isStackSlot(), alloc.memorySlot());
        MOZ_ASSERT(alloc.isRegister());
        addSlotsOrElementsRegister(alloc.toRegister().gpr());
        assertInvariants();
        return true;
    }
    bool hasSlotsOrElementsPointer(LAllocation alloc) const {
        if (alloc.isRegister())
            return slotsOrElementsRegs().has(alloc.toRegister().gpr());
        for (size_t i = 0; i < slotsOrElementsSlots_.length(); i++) {
            const SlotEntry& entry = slotsOrElementsSlots_[i];
            if (entry.stack == alloc.isStackSlot() && entry.slot == alloc.memorySlot())
                return true;
        }
        return false;
    }

    bool addGcPointer(LAllocation alloc) {
        if (alloc.isMemory())
            return addGcSlot(alloc.isStackSlot(), alloc.memorySlot());
        if (alloc.isRegister())
            addGcRegister(alloc.toRegister().gpr());
        assertInvariants();
        return true;
    }

    bool hasGcPointer(LAllocation alloc) const {
        if (alloc.isRegister())
            return gcRegs().has(alloc.toRegister().gpr());
        MOZ_ASSERT(alloc.isMemory());
        for (size_t i = 0; i < gcSlots_.length(); i++) {
            if (gcSlots_[i].stack == alloc.isStackSlot() && gcSlots_[i].slot == alloc.memorySlot())
                return true;
        }
        return false;
    }

    bool addValueSlot(bool stack, uint32_t slot) {
        bool result = valueSlots_.append(SlotEntry(stack, slot));
        if (result)
            assertInvariants();
        return result;
    }
    SlotList& valueSlots() {
        return valueSlots_;
    }

    bool hasValueSlot(bool stack, uint32_t slot) const {
        for (size_t i = 0; i < valueSlots_.length(); i++) {
            if (valueSlots_[i].stack == stack && valueSlots_[i].slot == slot)
                return true;
        }
        return false;
    }

#ifdef JS_NUNBOX32

    bool addNunboxParts(uint32_t typeVreg, LAllocation type, LAllocation payload) {
        bool result = nunboxParts_.append(NunboxEntry(typeVreg, type, payload));
        if (result)
            assertInvariants();
        return result;
    }

    bool addNunboxType(uint32_t typeVreg, LAllocation type) {
        for (size_t i = 0; i < nunboxParts_.length(); i++) {
            if (nunboxParts_[i].type == type)
                return true;
            if (nunboxParts_[i].type == LUse(typeVreg, LUse::ANY)) {
                nunboxParts_[i].type = type;
                return true;
            }
        }

        // vregs for nunbox pairs are adjacent, with the type coming first.
        uint32_t payloadVreg = typeVreg + 1;
        bool result = nunboxParts_.append(NunboxEntry(typeVreg, type, LUse(payloadVreg, LUse::ANY)));
        if (result)
            assertInvariants();
        return result;
    }

    bool addNunboxPayload(uint32_t payloadVreg, LAllocation payload) {
        for (size_t i = 0; i < nunboxParts_.length(); i++) {
            if (nunboxParts_[i].payload == payload)
                return true;
            if (nunboxParts_[i].payload == LUse(payloadVreg, LUse::ANY)) {
                nunboxParts_[i].payload = payload;
                return true;
            }
        }

        // vregs for nunbox pairs are adjacent, with the type coming first.
        uint32_t typeVreg = payloadVreg - 1;
        bool result = nunboxParts_.append(NunboxEntry(typeVreg, LUse(typeVreg, LUse::ANY), payload));
        if (result)
            assertInvariants();
        return result;
    }

    LAllocation findTypeAllocation(uint32_t typeVreg) {
        // Look for some allocation for the specified type vreg, to go with a
        // partial nunbox entry for the payload. Note that we don't need to
        // look at the value slots in the safepoint, as these aren't used by
        // register allocators which add partial nunbox entries.
        for (size_t i = 0; i < nunboxParts_.length(); i++) {
            if (nunboxParts_[i].typeVreg == typeVreg && !nunboxParts_[i].type.isUse())
                return nunboxParts_[i].type;
        }
        return LUse(typeVreg, LUse::ANY);
    }

#ifdef DEBUG
    bool hasNunboxPayload(LAllocation payload) const {
        if (payload.isMemory() && hasValueSlot(payload.isStackSlot(), payload.memorySlot()))
            return true;
        for (size_t i = 0; i < nunboxParts_.length(); i++) {
            if (nunboxParts_[i].payload == payload)
                return true;
        }
        return false;
    }
#endif

    NunboxList& nunboxParts() {
        return nunboxParts_;
    }

#elif JS_PUNBOX64

    void addValueRegister(Register reg) {
        valueRegs_.add(reg);
        assertInvariants();
    }
    LiveGeneralRegisterSet valueRegs() const {
        return valueRegs_;
    }

    bool addBoxedValue(LAllocation alloc) {
        if (alloc.isRegister()) {
            Register reg = alloc.toRegister().gpr();
            if (!valueRegs().has(reg))
                addValueRegister(reg);
            return true;
        }
        if (hasValueSlot(alloc.isStackSlot(), alloc.memorySlot()))
            return true;
        return addValueSlot(alloc.isStackSlot(), alloc.memorySlot());
    }

    bool hasBoxedValue(LAllocation alloc) const {
        if (alloc.isRegister())
            return valueRegs().has(alloc.toRegister().gpr());
        return hasValueSlot(alloc.isStackSlot(), alloc.memorySlot());
    }

#endif // JS_PUNBOX64

    bool encoded() const {
        return safepointOffset_ != INVALID_SAFEPOINT_OFFSET;
    }
    uint32_t offset() const {
        MOZ_ASSERT(encoded());
        return safepointOffset_;
    }
    void setOffset(uint32_t offset) {
        safepointOffset_ = offset;
    }
    uint32_t osiReturnPointOffset() const {
        // In general, pointer arithmetic on code is bad, but in this case,
        // getting the return address from a call instruction, stepping over pools
        // would be wrong.
        return osiCallPointOffset_ + Assembler::PatchWrite_NearCallSize();
    }
    uint32_t osiCallPointOffset() const {
        return osiCallPointOffset_;
    }
    void setOsiCallPointOffset(uint32_t osiCallPointOffset) {
        MOZ_ASSERT(!osiCallPointOffset_);
        osiCallPointOffset_ = osiCallPointOffset;
    }
};

class LInstruction::InputIterator
{
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
    explicit InputIterator(LInstruction& ins) :
      ins_(ins),
      idx_(0),
      snapshot_(false)
    {
        handleOperandsEnd();
    }

    bool more() const {
        if (snapshot_)
            return idx_ < ins_.snapshot()->numEntries();
        if (idx_ < ins_.numOperands())
            return true;
        if (ins_.snapshot() && ins_.snapshot()->numEntries())
            return true;
        return false;
    }

    bool isSnapshotInput() const {
        return snapshot_;
    }

    void next() {
        MOZ_ASSERT(more());
        idx_++;
        handleOperandsEnd();
    }

    void replace(const LAllocation& alloc) {
        if (snapshot_)
            ins_.snapshot()->setEntry(idx_, alloc);
        else
            ins_.setOperand(idx_, alloc);
    }

    LAllocation* operator*() const {
        if (snapshot_)
            return ins_.snapshot()->getEntry(idx_);
        return ins_.getOperand(idx_);
    }

    LAllocation* operator ->() const {
        return **this;
    }
};

class LIRGraph
{
    struct ValueHasher
    {
        typedef Value Lookup;
        static HashNumber hash(const Value& v) {
            return HashNumber(v.asRawBits());
        }
        static bool match(const Value& lhs, const Value& rhs) {
            return lhs == rhs;
        }
    };

    FixedList<LBlock> blocks_;
    Vector<Value, 0, JitAllocPolicy> constantPool_;
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

    // Snapshot taken before any LIR has been lowered.
    LSnapshot* entrySnapshot_;

    MIRGraph& mir_;

  public:
    explicit LIRGraph(MIRGraph* mir);

    bool init() {
        return constantPoolMap_.init() && blocks_.init(mir_.alloc(), mir_.numBlocks());
    }
    MIRGraph& mir() const {
        return mir_;
    }
    size_t numBlocks() const {
        return blocks_.length();
    }
    LBlock* getBlock(size_t i) {
        return &blocks_[i];
    }
    uint32_t numBlockIds() const {
        return mir_.numBlockIds();
    }
    bool initBlock(MBasicBlock* mir) {
        LBlock* lir = new (&blocks_[mir->id()]) LBlock(mir);
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
    uint32_t getInstructionId() {
        return numInstructions_++;
    }
    uint32_t numInstructions() const {
        return numInstructions_;
    }
    void setLocalSlotCount(uint32_t localSlotCount) {
        localSlotCount_ = localSlotCount;
    }
    uint32_t localSlotCount() const {
        return localSlotCount_;
    }
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
    size_t paddedLocalSlotsSize() const {
        return paddedLocalSlotCount();
    }
    void setArgumentSlotCount(uint32_t argumentSlotCount) {
        argumentSlotCount_ = argumentSlotCount;
    }
    uint32_t argumentSlotCount() const {
        return argumentSlotCount_;
    }
    size_t argumentsSize() const {
        return argumentSlotCount() * sizeof(Value);
    }
    uint32_t totalSlotCount() const {
        return paddedLocalSlotCount() + argumentsSize();
    }
    bool addConstantToPool(const Value& v, uint32_t* index);
    size_t numConstants() const {
        return constantPool_.length();
    }
    Value* constantPool() {
        return &constantPool_[0];
    }
    void setEntrySnapshot(LSnapshot* snapshot) {
        MOZ_ASSERT(!entrySnapshot_);
        MOZ_ASSERT(snapshot->bailoutKind() == Bailout_InitialState);
        snapshot->setBailoutKind(Bailout_ArgumentCheck);
        entrySnapshot_ = snapshot;
    }
    LSnapshot* entrySnapshot() const {
        MOZ_ASSERT(entrySnapshot_);
        return entrySnapshot_;
    }
    bool noteNeedsSafepoint(LInstruction* ins);
    size_t numNonCallSafepoints() const {
        return nonCallSafepoints_.length();
    }
    LInstruction* getNonCallSafepoint(size_t i) const {
        return nonCallSafepoints_[i];
    }
    size_t numSafepoints() const {
        return safepoints_.length();
    }
    LInstruction* getSafepoint(size_t i) const {
        return safepoints_[i];
    }

    void dump(GenericPrinter& out);
    void dump();
};

LAllocation::LAllocation(AnyRegister reg)
{
    if (reg.isFloat())
        *this = LFloatReg(reg.fpu());
    else
        *this = LGeneralReg(reg.gpr());
}

AnyRegister
LAllocation::toRegister() const
{
    MOZ_ASSERT(isRegister());
    if (isFloatReg())
        return AnyRegister(toFloatReg()->reg());
    return AnyRegister(toGeneralReg()->reg());
}

} // namespace jit
} // namespace js

#include "jit/shared/LIR-shared.h"
#if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
# if defined(JS_CODEGEN_X86)
#  include "jit/x86/LIR-x86.h"
# elif defined(JS_CODEGEN_X64)
#  include "jit/x64/LIR-x64.h"
# endif
# include "jit/x86-shared/LIR-x86-shared.h"
#elif defined(JS_CODEGEN_ARM)
# include "jit/arm/LIR-arm.h"
#elif defined(JS_CODEGEN_ARM64)
# include "jit/arm64/LIR-arm64.h"
#elif defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
# if defined(JS_CODEGEN_MIPS32)
#  include "jit/mips32/LIR-mips32.h"
# elif defined(JS_CODEGEN_MIPS64)
#  include "jit/mips64/LIR-mips64.h"
# endif
# include "jit/mips-shared/LIR-mips-shared.h"
#elif defined(JS_CODEGEN_NONE)
# include "jit/none/LIR-none.h"
#else
# error "Unknown architecture!"
#endif

#undef LIR_HEADER

namespace js {
namespace jit {

#define LIROP(name)                                                         \
    L##name* LNode::to##name()                                              \
    {                                                                       \
        MOZ_ASSERT(is##name());                                             \
        return static_cast<L##name*>(this);                                \
    }                                                                       \
    const L##name* LNode::to##name() const                                  \
    {                                                                       \
        MOZ_ASSERT(is##name());                                             \
        return static_cast<const L##name*>(this);                          \
    }
    LIR_OPCODE_LIST(LIROP)
#undef LIROP

#define LALLOC_CAST(type)                                                   \
    L##type* LAllocation::to##type() {                                      \
        MOZ_ASSERT(is##type());                                             \
        return static_cast<L##type*>(this);                                \
    }
#define LALLOC_CONST_CAST(type)                                             \
    const L##type* LAllocation::to##type() const {                          \
        MOZ_ASSERT(is##type());                                             \
        return static_cast<const L##type*>(this);                          \
    }

LALLOC_CAST(Use)
LALLOC_CONST_CAST(Use)
LALLOC_CONST_CAST(GeneralReg)
LALLOC_CONST_CAST(FloatReg)
LALLOC_CONST_CAST(StackSlot)
LALLOC_CONST_CAST(Argument)
LALLOC_CONST_CAST(ConstantIndex)

#undef LALLOC_CAST

#ifdef JS_NUNBOX32
static inline signed
OffsetToOtherHalfOfNunbox(LDefinition::Type type)
{
    MOZ_ASSERT(type == LDefinition::TYPE || type == LDefinition::PAYLOAD);
    signed offset = (type == LDefinition::TYPE)
                    ? PAYLOAD_INDEX - TYPE_INDEX
                    : TYPE_INDEX - PAYLOAD_INDEX;
    return offset;
}

static inline void
AssertTypesFormANunbox(LDefinition::Type type1, LDefinition::Type type2)
{
    MOZ_ASSERT((type1 == LDefinition::TYPE && type2 == LDefinition::PAYLOAD) ||
               (type2 == LDefinition::TYPE && type1 == LDefinition::PAYLOAD));
}

static inline unsigned
OffsetOfNunboxSlot(LDefinition::Type type)
{
    if (type == LDefinition::PAYLOAD)
        return NUNBOX32_PAYLOAD_OFFSET;
    return NUNBOX32_TYPE_OFFSET;
}

// Note that stack indexes for LStackSlot are modelled backwards, so a
// double-sized slot starting at 2 has its next word at 1, *not* 3.
static inline unsigned
BaseOfNunboxSlot(LDefinition::Type type, unsigned slot)
{
    if (type == LDefinition::PAYLOAD)
        return slot + NUNBOX32_PAYLOAD_OFFSET;
    return slot + NUNBOX32_TYPE_OFFSET;
}
#endif

} // namespace jit
} // namespace js

#endif /* jit_LIR_h */

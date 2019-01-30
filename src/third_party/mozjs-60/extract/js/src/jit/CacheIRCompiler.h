/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_CacheIRCompiler_h
#define jit_CacheIRCompiler_h

#include "jit/CacheIR.h"

namespace js {
namespace jit {

// The ops below are defined in CacheIRCompiler and codegen is shared between
// BaselineCacheIRCompiler and IonCacheIRCompiler.
#define CACHE_IR_SHARED_OPS(_)            \
    _(GuardIsObject)                      \
    _(GuardIsNullOrUndefined)             \
    _(GuardIsObjectOrNull)                \
    _(GuardIsString)                      \
    _(GuardIsSymbol)                      \
    _(GuardIsNumber)                      \
    _(GuardIsInt32Index)                  \
    _(GuardType)                          \
    _(GuardClass)                         \
    _(GuardIsNativeFunction)              \
    _(GuardIsNativeObject)                \
    _(GuardIsProxy)                       \
    _(GuardNotDOMProxy)                   \
    _(GuardSpecificInt32Immediate)        \
    _(GuardMagicValue)                    \
    _(GuardNoUnboxedExpando)              \
    _(GuardAndLoadUnboxedExpando)         \
    _(GuardNoDetachedTypedObjects)        \
    _(GuardNoDenseElements)               \
    _(GuardAndGetIndexFromString)         \
    _(GuardIndexIsNonNegative)            \
    _(GuardTagNotEqual)                   \
    _(LoadProto)                          \
    _(LoadEnclosingEnvironment)           \
    _(LoadWrapperTarget)                  \
    _(LoadValueTag)                       \
    _(LoadDOMExpandoValue)                \
    _(LoadDOMExpandoValueIgnoreGeneration)\
    _(LoadUndefinedResult)                \
    _(LoadBooleanResult)                  \
    _(LoadInt32ArrayLengthResult)         \
    _(LoadArgumentsObjectLengthResult)    \
    _(LoadFunctionLengthResult)           \
    _(LoadStringLengthResult)             \
    _(LoadStringCharResult)               \
    _(LoadArgumentsObjectArgResult)       \
    _(LoadInstanceOfObjectResult)         \
    _(LoadDenseElementResult)             \
    _(LoadDenseElementHoleResult)         \
    _(LoadDenseElementExistsResult)       \
    _(LoadDenseElementHoleExistsResult)   \
    _(LoadTypedElementExistsResult)       \
    _(LoadTypedElementResult)             \
    _(LoadObjectResult)                   \
    _(LoadTypeOfObjectResult)             \
    _(LoadInt32TruthyResult)              \
    _(LoadDoubleTruthyResult)             \
    _(LoadStringTruthyResult)             \
    _(LoadObjectTruthyResult)             \
    _(CompareStringResult)                \
    _(CompareObjectResult)                \
    _(CompareSymbolResult)                \
    _(ArrayJoinResult)                    \
    _(CallPrintString)                    \
    _(Breakpoint)                         \
    _(MegamorphicLoadSlotByValueResult)   \
    _(MegamorphicHasPropResult)           \
    _(CallObjectHasSparseElementResult)   \
    _(WrapResult)

// Represents a Value on the Baseline frame's expression stack. Slot 0 is the
// value on top of the stack (the most recently pushed value), slot 1 is the
// value pushed before that, etc.
class BaselineFrameSlot
{
    uint32_t slot_;

  public:
    explicit BaselineFrameSlot(uint32_t slot) : slot_(slot) {}
    uint32_t slot() const { return slot_; }

    bool operator==(const BaselineFrameSlot& other) const { return slot_ == other.slot_; }
    bool operator!=(const BaselineFrameSlot& other) const { return slot_ != other.slot_; }
};

// OperandLocation represents the location of an OperandId. The operand is
// either in a register or on the stack, and is either boxed or unboxed.
class OperandLocation
{
  public:
    enum Kind {
        Uninitialized = 0,
        PayloadReg,
        DoubleReg,
        ValueReg,
        PayloadStack,
        ValueStack,
        BaselineFrame,
        Constant,
    };

  private:
    Kind kind_;

    union Data {
        struct {
            Register reg;
            JSValueType type;
        } payloadReg;
        FloatRegister doubleReg;
        ValueOperand valueReg;
        struct {
            uint32_t stackPushed;
            JSValueType type;
        } payloadStack;
        uint32_t valueStackPushed;
        BaselineFrameSlot baselineFrameSlot;
        Value constant;

        Data() : valueStackPushed(0) {}
    };
    Data data_;

  public:
    OperandLocation() : kind_(Uninitialized) {}

    Kind kind() const { return kind_; }

    void setUninitialized() {
        kind_ = Uninitialized;
    }

    ValueOperand valueReg() const {
        MOZ_ASSERT(kind_ == ValueReg);
        return data_.valueReg;
    }
    Register payloadReg() const {
        MOZ_ASSERT(kind_ == PayloadReg);
        return data_.payloadReg.reg;
    }
    FloatRegister doubleReg() const {
        MOZ_ASSERT(kind_ == DoubleReg);
        return data_.doubleReg;
    }
    uint32_t payloadStack() const {
        MOZ_ASSERT(kind_ == PayloadStack);
        return data_.payloadStack.stackPushed;
    }
    uint32_t valueStack() const {
        MOZ_ASSERT(kind_ == ValueStack);
        return data_.valueStackPushed;
    }
    JSValueType payloadType() const {
        if (kind_ == PayloadReg)
            return data_.payloadReg.type;
        MOZ_ASSERT(kind_ == PayloadStack);
        return data_.payloadStack.type;
    }
    Value constant() const {
        MOZ_ASSERT(kind_ == Constant);
        return data_.constant;
    }
    BaselineFrameSlot baselineFrameSlot() const {
        MOZ_ASSERT(kind_ == BaselineFrame);
        return data_.baselineFrameSlot;
    }

    void setPayloadReg(Register reg, JSValueType type) {
        kind_ = PayloadReg;
        data_.payloadReg.reg = reg;
        data_.payloadReg.type = type;
    }
    void setDoubleReg(FloatRegister reg) {
        kind_ = DoubleReg;
        data_.doubleReg = reg;
    }
    void setValueReg(ValueOperand reg) {
        kind_ = ValueReg;
        data_.valueReg = reg;
    }
    void setPayloadStack(uint32_t stackPushed, JSValueType type) {
        kind_ = PayloadStack;
        data_.payloadStack.stackPushed = stackPushed;
        data_.payloadStack.type = type;
    }
    void setValueStack(uint32_t stackPushed) {
        kind_ = ValueStack;
        data_.valueStackPushed = stackPushed;
    }
    void setConstant(const Value& v) {
        kind_ = Constant;
        data_.constant = v;
    }
    void setBaselineFrame(BaselineFrameSlot slot) {
        kind_ = BaselineFrame;
        data_.baselineFrameSlot = slot;
    }

    bool isInRegister() const { return kind_ == PayloadReg || kind_ == ValueReg; }
    bool isOnStack() const { return kind_ == PayloadStack || kind_ == ValueStack; }

    size_t stackPushed() const {
        if (kind_ == PayloadStack)
            return data_.payloadStack.stackPushed;
        MOZ_ASSERT(kind_ == ValueStack);
        return data_.valueStackPushed;
    }
    size_t stackSizeInBytes() const {
        if (kind_ == PayloadStack)
            return sizeof(uintptr_t);
        MOZ_ASSERT(kind_ == ValueStack);
        return sizeof(js::Value);
    }
    void adjustStackPushed(int32_t diff) {
        if (kind_ == PayloadStack) {
            data_.payloadStack.stackPushed += diff;
            return;
        }
        MOZ_ASSERT(kind_ == ValueStack);
        data_.valueStackPushed += diff;
    }

    bool aliasesReg(Register reg) const {
        if (kind_ == PayloadReg)
            return payloadReg() == reg;
        if (kind_ == ValueReg)
            return valueReg().aliases(reg);
        return false;
    }
    bool aliasesReg(ValueOperand reg) const {
#if defined(JS_NUNBOX32)
        return aliasesReg(reg.typeReg()) || aliasesReg(reg.payloadReg());
#else
        return aliasesReg(reg.valueReg());
#endif
    }

    bool aliasesReg(const OperandLocation& other) const;

    bool operator==(const OperandLocation& other) const;
    bool operator!=(const OperandLocation& other) const { return !operator==(other); }
};

struct SpilledRegister
{
    Register reg;
    uint32_t stackPushed;

    SpilledRegister(Register reg, uint32_t stackPushed)
        : reg(reg), stackPushed(stackPushed)
    {}
    bool operator==(const SpilledRegister& other) const {
        return reg == other.reg && stackPushed == other.stackPushed;
    }
    bool operator!=(const SpilledRegister& other) const { return !(*this == other); }
};

using SpilledRegisterVector = Vector<SpilledRegister, 2, SystemAllocPolicy>;

// Class to track and allocate registers while emitting IC code.
class MOZ_RAII CacheRegisterAllocator
{
    // The original location of the inputs to the cache.
    Vector<OperandLocation, 4, SystemAllocPolicy> origInputLocations_;

    // The current location of each operand.
    Vector<OperandLocation, 8, SystemAllocPolicy> operandLocations_;

    // Free lists for value- and payload-slots on stack
    Vector<uint32_t, 2, SystemAllocPolicy> freeValueSlots_;
    Vector<uint32_t, 2, SystemAllocPolicy> freePayloadSlots_;

    // The registers allocated while emitting the current CacheIR op.
    // This prevents us from allocating a register and then immediately
    // clobbering it for something else, while we're still holding on to it.
    LiveGeneralRegisterSet currentOpRegs_;

    const AllocatableGeneralRegisterSet allocatableRegs_;

    // Registers that are currently unused and available.
    AllocatableGeneralRegisterSet availableRegs_;

    // Registers that are available, but before use they must be saved and
    // then restored when returning from the stub.
    AllocatableGeneralRegisterSet availableRegsAfterSpill_;

    // Registers we took from availableRegsAfterSpill_ and spilled to the stack.
    SpilledRegisterVector spilledRegs_;

    // The number of bytes pushed on the native stack.
    uint32_t stackPushed_;

    // The index of the CacheIR instruction we're currently emitting.
    uint32_t currentInstruction_;

    const CacheIRWriter& writer_;

    CacheRegisterAllocator(const CacheRegisterAllocator&) = delete;
    CacheRegisterAllocator& operator=(const CacheRegisterAllocator&) = delete;

    void freeDeadOperandLocations(MacroAssembler& masm);

    void spillOperandToStack(MacroAssembler& masm, OperandLocation* loc);
    void spillOperandToStackOrRegister(MacroAssembler& masm, OperandLocation* loc);

    void popPayload(MacroAssembler& masm, OperandLocation* loc, Register dest);
    void popValue(MacroAssembler& masm, OperandLocation* loc, ValueOperand dest);

  public:
    friend class AutoScratchRegister;
    friend class AutoScratchRegisterExcluding;

    explicit CacheRegisterAllocator(const CacheIRWriter& writer)
      : allocatableRegs_(GeneralRegisterSet::All()),
        stackPushed_(0),
        currentInstruction_(0),
        writer_(writer)
    {}

    MOZ_MUST_USE bool init();

    void initAvailableRegs(const AllocatableGeneralRegisterSet& available) {
        availableRegs_ = available;
    }
    void initAvailableRegsAfterSpill();

    void fixupAliasedInputs(MacroAssembler& masm);

    OperandLocation operandLocation(size_t i) const {
        return operandLocations_[i];
    }
    void setOperandLocation(size_t i, const OperandLocation& loc) {
        operandLocations_[i] = loc;
    }

    OperandLocation origInputLocation(size_t i) const {
        return origInputLocations_[i];
    }
    void initInputLocation(size_t i, ValueOperand reg) {
        origInputLocations_[i].setValueReg(reg);
        operandLocations_[i].setValueReg(reg);
    }
    void initInputLocation(size_t i, Register reg, JSValueType type) {
        origInputLocations_[i].setPayloadReg(reg, type);
        operandLocations_[i].setPayloadReg(reg, type);
    }
    void initInputLocation(size_t i, FloatRegister reg) {
        origInputLocations_[i].setDoubleReg(reg);
        operandLocations_[i].setDoubleReg(reg);
    }
    void initInputLocation(size_t i, const Value& v) {
        origInputLocations_[i].setConstant(v);
        operandLocations_[i].setConstant(v);
    }
    void initInputLocation(size_t i, BaselineFrameSlot slot) {
        origInputLocations_[i].setBaselineFrame(slot);
        operandLocations_[i].setBaselineFrame(slot);
    }

    void initInputLocation(size_t i, const TypedOrValueRegister& reg);
    void initInputLocation(size_t i, const ConstantOrRegister& value);

    const SpilledRegisterVector& spilledRegs() const { return spilledRegs_; }

    MOZ_MUST_USE bool setSpilledRegs(const SpilledRegisterVector& regs) {
        spilledRegs_.clear();
        return spilledRegs_.appendAll(regs);
    }

    void nextOp() {
        currentOpRegs_.clear();
        currentInstruction_++;
    }

    bool isDeadAfterInstruction(OperandId opId) const {
        return writer_.operandIsDead(opId.id(), currentInstruction_ + 1);
    }

    uint32_t stackPushed() const {
        return stackPushed_;
    }
    void setStackPushed(uint32_t pushed) {
        stackPushed_ = pushed;
    }

    bool isAllocatable(Register reg) const {
        return allocatableRegs_.has(reg);
    }

    // Allocates a new register.
    Register allocateRegister(MacroAssembler& masm);
    ValueOperand allocateValueRegister(MacroAssembler& masm);

    void allocateFixedRegister(MacroAssembler& masm, Register reg);
    void allocateFixedValueRegister(MacroAssembler& masm, ValueOperand reg);

    // Releases a register so it can be reused later.
    void releaseRegister(Register reg) {
        MOZ_ASSERT(currentOpRegs_.has(reg));
        availableRegs_.add(reg);
        currentOpRegs_.take(reg);
    }
    void releaseValueRegister(ValueOperand reg) {
#ifdef JS_NUNBOX32
        releaseRegister(reg.payloadReg());
        releaseRegister(reg.typeReg());
#else
        releaseRegister(reg.valueReg());
#endif
    }

    // Removes spilled values from the native stack. This should only be
    // called after all registers have been allocated.
    void discardStack(MacroAssembler& masm);

    Address addressOf(MacroAssembler& masm, BaselineFrameSlot slot) const;

    // Returns the register for the given operand. If the operand is currently
    // not in a register, it will load it into one.
    ValueOperand useValueRegister(MacroAssembler& masm, ValOperandId val);
    ValueOperand useFixedValueRegister(MacroAssembler& masm, ValOperandId valId, ValueOperand reg);
    Register useRegister(MacroAssembler& masm, TypedOperandId typedId);

    ConstantOrRegister useConstantOrRegister(MacroAssembler& masm, ValOperandId val);

    // Allocates an output register for the given operand.
    Register defineRegister(MacroAssembler& masm, TypedOperandId typedId);
    ValueOperand defineValueRegister(MacroAssembler& masm, ValOperandId val);

    // Returns |val|'s JSValueType or JSVAL_TYPE_UNKNOWN.
    JSValueType knownType(ValOperandId val) const;

    // Emits code to restore registers and stack to the state at the start of
    // the stub.
    void restoreInputState(MacroAssembler& masm, bool discardStack = true);

    // Returns the set of registers storing the IC input operands.
    GeneralRegisterSet inputRegisterSet() const;

    void saveIonLiveRegisters(MacroAssembler& masm, LiveRegisterSet liveRegs,
                              Register scratch, IonScript* ionScript);
    void restoreIonLiveRegisters(MacroAssembler& masm, LiveRegisterSet liveRegs);
};

// RAII class to allocate a scratch register and release it when we're done
// with it.
class MOZ_RAII AutoScratchRegister
{
    CacheRegisterAllocator& alloc_;
    Register reg_;

    AutoScratchRegister(const AutoScratchRegister&) = delete;
    void operator=(const AutoScratchRegister&) = delete;

  public:
    AutoScratchRegister(CacheRegisterAllocator& alloc, MacroAssembler& masm,
                        Register reg = InvalidReg)
      : alloc_(alloc)
    {
        if (reg != InvalidReg) {
            alloc.allocateFixedRegister(masm, reg);
            reg_ = reg;
        } else {
            reg_ = alloc.allocateRegister(masm);
        }
        MOZ_ASSERT(alloc_.currentOpRegs_.has(reg_));
    }
    ~AutoScratchRegister() {
        alloc_.releaseRegister(reg_);
    }

    Register get() const { return reg_; }
    operator Register() const { return reg_; }
};

// The FailurePath class stores everything we need to generate a failure path
// at the end of the IC code. The failure path restores the input registers, if
// needed, and jumps to the next stub.
class FailurePath
{
    Vector<OperandLocation, 4, SystemAllocPolicy> inputs_;
    SpilledRegisterVector spilledRegs_;
    NonAssertingLabel label_;
    uint32_t stackPushed_;

  public:
    FailurePath() = default;

    FailurePath(FailurePath&& other)
      : inputs_(Move(other.inputs_)),
        spilledRegs_(Move(other.spilledRegs_)),
        label_(other.label_),
        stackPushed_(other.stackPushed_)
    {}

    Label* label() { return &label_; }

    void setStackPushed(uint32_t i) { stackPushed_ = i; }
    uint32_t stackPushed() const { return stackPushed_; }

    MOZ_MUST_USE bool appendInput(const OperandLocation& loc) {
        return inputs_.append(loc);
    }
    OperandLocation input(size_t i) const {
        return inputs_[i];
    }

    const SpilledRegisterVector& spilledRegs() const { return spilledRegs_; }

    MOZ_MUST_USE bool setSpilledRegs(const SpilledRegisterVector& regs) {
        MOZ_ASSERT(spilledRegs_.empty());
        return spilledRegs_.appendAll(regs);
    }

    // If canShareFailurePath(other) returns true, the same machine code will
    // be emitted for two failure paths, so we can share them.
    bool canShareFailurePath(const FailurePath& other) const;
};

class AutoOutputRegister;

// Base class for BaselineCacheIRCompiler and IonCacheIRCompiler.
class MOZ_RAII CacheIRCompiler
{
  protected:
    friend class AutoOutputRegister;

    enum class Mode { Baseline, Ion };

    JSContext* cx_;
    CacheIRReader reader;
    const CacheIRWriter& writer_;
    MacroAssembler masm;

    CacheRegisterAllocator allocator;
    Vector<FailurePath, 4, SystemAllocPolicy> failurePaths;

    // Float registers that are live. Registers not in this set can be
    // clobbered and don't need to be saved before performing a VM call.
    // Doing this for non-float registers is a bit more complicated because
    // the IC register allocator allocates GPRs.
    LiveFloatRegisterSet liveFloatRegs_;

    Maybe<TypedOrValueRegister> outputUnchecked_;
    Mode mode_;

    // Whether this IC may read double values from uint32 arrays.
    Maybe<bool> allowDoubleResult_;

    CacheIRCompiler(JSContext* cx, const CacheIRWriter& writer, Mode mode)
      : cx_(cx),
        reader(writer),
        writer_(writer),
        allocator(writer_),
        liveFloatRegs_(FloatRegisterSet::All()),
        mode_(mode)
    {
        MOZ_ASSERT(!writer.failed());
    }

    MOZ_MUST_USE bool addFailurePath(FailurePath** failure);
    MOZ_MUST_USE bool emitFailurePath(size_t i);

    // Returns the set of volatile float registers that are live. These
    // registers need to be saved when making non-GC calls with callWithABI.
    FloatRegisterSet liveVolatileFloatRegs() const {
        return FloatRegisterSet::Intersect(liveFloatRegs_.set(), FloatRegisterSet::Volatile());
    }

    bool objectGuardNeedsSpectreMitigations(ObjOperandId objId) const {
        // Instructions like GuardShape need Spectre mitigations if
        // (1) mitigations are enabled and (2) the object is used by other
        // instructions (if the object is *not* used by other instructions,
        // zeroing its register is pointless).
        return JitOptions.spectreObjectMitigationsMisc && !allocator.isDeadAfterInstruction(objId);
    }

    void emitLoadTypedObjectResultShared(const Address& fieldAddr, Register scratch,
                                         uint32_t typeDescr,
                                         const AutoOutputRegister& output);

    void emitStoreTypedObjectReferenceProp(ValueOperand val, ReferenceTypeDescr::Type type,
                                           const Address& dest, Register scratch);

    void emitRegisterEnumerator(Register enumeratorsList, Register iter, Register scratch);

  private:
    void emitPostBarrierShared(Register obj, const ConstantOrRegister& val, Register scratch,
                               Register maybeIndex);

    void emitPostBarrierShared(Register obj, ValueOperand val, Register scratch,
                               Register maybeIndex) {
        emitPostBarrierShared(obj, ConstantOrRegister(val), scratch, maybeIndex);
    }

  protected:
    template <typename T>
    void emitPostBarrierSlot(Register obj, const T& val, Register scratch) {
        emitPostBarrierShared(obj, val, scratch, InvalidReg);
    }

    template <typename T>
    void emitPostBarrierElement(Register obj, const T& val, Register scratch, Register index) {
        MOZ_ASSERT(index != InvalidReg);
        emitPostBarrierShared(obj, val, scratch, index);
    }

    bool emitComparePointerResultShared(bool symbol);

#define DEFINE_SHARED_OP(op) MOZ_MUST_USE bool emit##op();
    CACHE_IR_SHARED_OPS(DEFINE_SHARED_OP)
#undef DEFINE_SHARED_OP
};

// Ensures the IC's output register is available for writing.
class MOZ_RAII AutoOutputRegister
{
    TypedOrValueRegister output_;
    CacheRegisterAllocator& alloc_;

    AutoOutputRegister(const AutoOutputRegister&) = delete;
    void operator=(const AutoOutputRegister&) = delete;

  public:
    explicit AutoOutputRegister(CacheIRCompiler& compiler);
    ~AutoOutputRegister();

    Register maybeReg() const {
        if (output_.hasValue())
            return output_.valueReg().scratchReg();
        if (!output_.typedReg().isFloat())
            return output_.typedReg().gpr();
        return InvalidReg;
    }

    bool hasValue() const { return output_.hasValue(); }
    ValueOperand valueReg() const { return output_.valueReg(); }
    AnyRegister typedReg() const { return output_.typedReg(); }

    JSValueType type() const {
        MOZ_ASSERT(!hasValue());
        return ValueTypeFromMIRType(output_.type());
    }

    operator TypedOrValueRegister() const { return output_; }
};

// Like AutoScratchRegister, but reuse a register of |output| if possible.
class MOZ_RAII AutoScratchRegisterMaybeOutput
{
    mozilla::Maybe<AutoScratchRegister> scratch_;
    Register scratchReg_;

    AutoScratchRegisterMaybeOutput(const AutoScratchRegisterMaybeOutput&) = delete;
    void operator=(const AutoScratchRegisterMaybeOutput&) = delete;

  public:
    AutoScratchRegisterMaybeOutput(CacheRegisterAllocator& alloc, MacroAssembler& masm,
                                   const AutoOutputRegister& output)
    {
        scratchReg_ = output.maybeReg();
        if (scratchReg_ == InvalidReg) {
            scratch_.emplace(alloc, masm);
            scratchReg_ = scratch_.ref();
        }
    }

    operator Register() const { return scratchReg_; }
};

// See the 'Sharing Baseline stub code' comment in CacheIR.h for a description
// of this class.
class CacheIRStubInfo
{
    // These fields don't require 8 bits, but GCC complains if these fields are
    // smaller than the size of the enums.
    CacheKind kind_ : 8;
    ICStubEngine engine_ : 8;
    bool makesGCCalls_ : 1;
    uint8_t stubDataOffset_;

    const uint8_t* code_;
    uint32_t length_;
    const uint8_t* fieldTypes_;

    CacheIRStubInfo(CacheKind kind, ICStubEngine engine, bool makesGCCalls,
                    uint32_t stubDataOffset, const uint8_t* code, uint32_t codeLength,
                    const uint8_t* fieldTypes)
      : kind_(kind),
        engine_(engine),
        makesGCCalls_(makesGCCalls),
        stubDataOffset_(stubDataOffset),
        code_(code),
        length_(codeLength),
        fieldTypes_(fieldTypes)
    {
        MOZ_ASSERT(kind_ == kind, "Kind must fit in bitfield");
        MOZ_ASSERT(engine_ == engine, "Engine must fit in bitfield");
        MOZ_ASSERT(stubDataOffset_ == stubDataOffset, "stubDataOffset must fit in uint8_t");
    }

    CacheIRStubInfo(const CacheIRStubInfo&) = delete;
    CacheIRStubInfo& operator=(const CacheIRStubInfo&) = delete;

  public:
    CacheKind kind() const { return kind_; }
    ICStubEngine engine() const { return engine_; }
    bool makesGCCalls() const { return makesGCCalls_; }

    const uint8_t* code() const { return code_; }
    uint32_t codeLength() const { return length_; }
    uint32_t stubDataOffset() const { return stubDataOffset_; }

    size_t stubDataSize() const;

    StubField::Type fieldType(uint32_t i) const { return (StubField::Type)fieldTypes_[i]; }

    static CacheIRStubInfo* New(CacheKind kind, ICStubEngine engine, bool canMakeCalls,
                                uint32_t stubDataOffset, const CacheIRWriter& writer);

    template <class Stub, class T>
    js::GCPtr<T>& getStubField(Stub* stub, uint32_t field) const;

    template <class T>
    js::GCPtr<T>& getStubField(ICStub* stub, uint32_t field) const {
        return getStubField<ICStub, T>(stub, field);
    }

    uintptr_t getStubRawWord(ICStub* stub, uint32_t field) const;

    void copyStubData(ICStub* src, ICStub* dest) const;
};

template <typename T>
void TraceCacheIRStub(JSTracer* trc, T* stub, const CacheIRStubInfo* stubInfo);

} // namespace jit
} // namespace js

#endif /* jit_CacheIRCompiler_h */

/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_RegisterSets_h
#define jit_RegisterSets_h

#include "mozilla/Alignment.h"
#include "mozilla/MathAlgorithms.h"

#include "jit/JitAllocPolicy.h"
#include "jit/Registers.h"

namespace js {
namespace jit {

struct AnyRegister {
    typedef uint32_t Code;

    static const uint32_t Total = Registers::Total + FloatRegisters::Total;
    static const uint32_t Invalid = UINT_MAX;

  private:
    Code code_;

  public:
    AnyRegister()
    { }
    explicit AnyRegister(Register gpr) {
        code_ = gpr.code();
    }
    explicit AnyRegister(FloatRegister fpu) {
        code_ = fpu.code() + Registers::Total;
    }
    static AnyRegister FromCode(uint32_t i) {
        MOZ_ASSERT(i < Total);
        AnyRegister r;
        r.code_ = i;
        return r;
    }
    bool isFloat() const {
        return code_ >= Registers::Total;
    }
    Register gpr() const {
        MOZ_ASSERT(!isFloat());
        return Register::FromCode(code_);
    }
    FloatRegister fpu() const {
        MOZ_ASSERT(isFloat());
        return FloatRegister::FromCode(code_ - Registers::Total);
    }
    bool operator ==(AnyRegister other) const {
        return code_ == other.code_;
    }
    bool operator !=(AnyRegister other) const {
        return code_ != other.code_;
    }
    const char* name() const {
        return isFloat() ? fpu().name() : gpr().name();
    }
    Code code() const {
        return code_;
    }
    bool volatile_() const {
        return isFloat() ? fpu().volatile_() : gpr().volatile_();
    }
    AnyRegister aliased(uint32_t aliasIdx) const {
        AnyRegister ret;
        if (isFloat()) {
            FloatRegister fret;
            fpu().aliased(aliasIdx, &fret);
            ret = AnyRegister(fret);
        } else {
            Register gret;
            gpr().aliased(aliasIdx, &gret);
            ret = AnyRegister(gret);
        }
        MOZ_ASSERT_IF(aliasIdx == 0, ret == *this);
        return ret;
    }
    uint32_t numAliased() const {
        if (isFloat())
            return fpu().numAliased();
        return gpr().numAliased();
    }
    bool aliases(const AnyRegister& other) const {
        if (isFloat() && other.isFloat())
            return fpu().aliases(other.fpu());
        if (!isFloat() && !other.isFloat())
            return gpr().aliases(other.gpr());
        return false;
    }
    // do the two registers hold the same type of data (e.g. both float32, both gpr)
    bool isCompatibleReg (const AnyRegister other) const {
        if (isFloat() && other.isFloat())
            return fpu().equiv(other.fpu());
        if (!isFloat() && !other.isFloat())
            return true;
        return false;
    }

};

// Registers to hold a boxed value. Uses one register on 64 bit
// platforms, two registers on 32 bit platforms.
class ValueOperand
{
#if defined(JS_NUNBOX32)
    Register type_;
    Register payload_;

  public:
    MOZ_CONSTEXPR ValueOperand(Register type, Register payload)
      : type_(type), payload_(payload)
    { }

    Register typeReg() const {
        return type_;
    }
    Register payloadReg() const {
        return payload_;
    }

    Register scratchReg() const {
        return payloadReg();
    }
    bool operator==(const ValueOperand& o) const {
        return type_ == o.type_ && payload_ == o.payload_;
    }
    bool operator!=(const ValueOperand& o) const {
        return !(*this == o);
    }

#elif defined(JS_PUNBOX64)
    Register value_;

  public:
    explicit MOZ_CONSTEXPR ValueOperand(Register value)
      : value_(value)
    { }

    Register valueReg() const {
        return value_;
    }

    Register scratchReg() const {
        return valueReg();
    }
    bool operator==(const ValueOperand& o) const {
        return value_ == o.value_;
    }
    bool operator!=(const ValueOperand& o) const {
        return !(*this == o);
    }
#endif

    ValueOperand() {}
};

// Registers to hold either either a typed or untyped value.
class TypedOrValueRegister
{
    // Type of value being stored.
    MIRType type_;

    // Space to hold either an AnyRegister or a ValueOperand.
    union U {
        mozilla::AlignedStorage2<AnyRegister> typed;
        mozilla::AlignedStorage2<ValueOperand> value;
    } data;

    AnyRegister& dataTyped() {
        MOZ_ASSERT(hasTyped());
        return *data.typed.addr();
    }
    ValueOperand& dataValue() {
        MOZ_ASSERT(hasValue());
        return *data.value.addr();
    }

    AnyRegister dataTyped() const {
        MOZ_ASSERT(hasTyped());
        return *data.typed.addr();
    }
    const ValueOperand& dataValue() const {
        MOZ_ASSERT(hasValue());
        return *data.value.addr();
    }

  public:

    TypedOrValueRegister()
      : type_(MIRType_None)
    {}

    TypedOrValueRegister(MIRType type, AnyRegister reg)
      : type_(type)
    {
        dataTyped() = reg;
    }

    MOZ_IMPLICIT TypedOrValueRegister(ValueOperand value)
      : type_(MIRType_Value)
    {
        dataValue() = value;
    }

    MIRType type() const {
        return type_;
    }

    bool hasTyped() const {
        return type() != MIRType_None && type() != MIRType_Value;
    }

    bool hasValue() const {
        return type() == MIRType_Value;
    }

    AnyRegister typedReg() const {
        return dataTyped();
    }

    ValueOperand valueReg() const {
        return dataValue();
    }

    AnyRegister scratchReg() {
        if (hasValue())
            return AnyRegister(valueReg().scratchReg());
        return typedReg();
    }
};

// A constant value, or registers to hold a typed/untyped value.
class ConstantOrRegister
{
    // Whether a constant value is being stored.
    bool constant_;

    // Space to hold either a Value or a TypedOrValueRegister.
    union U {
        mozilla::AlignedStorage2<Value> constant;
        mozilla::AlignedStorage2<TypedOrValueRegister> reg;
    } data;

    Value& dataValue() {
        MOZ_ASSERT(constant());
        return *data.constant.addr();
    }
    TypedOrValueRegister& dataReg() {
        MOZ_ASSERT(!constant());
        return *data.reg.addr();
    }

  public:

    ConstantOrRegister()
    {}

    MOZ_IMPLICIT ConstantOrRegister(Value value)
      : constant_(true)
    {
        dataValue() = value;
    }

    MOZ_IMPLICIT ConstantOrRegister(TypedOrValueRegister reg)
      : constant_(false)
    {
        dataReg() = reg;
    }

    bool constant() {
        return constant_;
    }

    Value value() {
        return dataValue();
    }

    TypedOrValueRegister reg() {
        return dataReg();
    }
};

struct Int32Key {
    bool isRegister_;
    union {
        Register reg_;
        int32_t constant_;
    };

    explicit Int32Key(Register reg)
      : isRegister_(true), reg_(reg)
    { }

    explicit Int32Key(int32_t index)
      : isRegister_(false), constant_(index)
    { }

    inline void bumpConstant(int diff) {
        MOZ_ASSERT(!isRegister_);
        constant_ += diff;
    }
    inline Register reg() const {
        MOZ_ASSERT(isRegister_);
        return reg_;
    }
    inline int32_t constant() const {
        MOZ_ASSERT(!isRegister_);
        return constant_;
    }
    inline bool isRegister() const {
        return isRegister_;
    }
    inline bool isConstant() const {
        return !isRegister_;
    }
};

template <typename T>
class TypedRegisterSet
{
    typedef typename T::SetType SetType;
    SetType bits_;


  public:
    explicit MOZ_CONSTEXPR TypedRegisterSet(SetType bits)
      : bits_(bits)
    { }

    MOZ_CONSTEXPR TypedRegisterSet() : bits_(0)
    { }
    MOZ_CONSTEXPR TypedRegisterSet(const TypedRegisterSet<T>& set) : bits_(set.bits_)
    { }

    static inline TypedRegisterSet All() {
        return TypedRegisterSet(T::Codes::AllocatableMask);
    }
    static inline TypedRegisterSet Intersect(const TypedRegisterSet& lhs,
                                             const TypedRegisterSet& rhs) {
        return TypedRegisterSet(lhs.bits_ & rhs.bits_);
    }
    static inline TypedRegisterSet Union(const TypedRegisterSet& lhs,
                                         const TypedRegisterSet& rhs) {
        return TypedRegisterSet(lhs.bits_ | rhs.bits_);
    }
    static inline TypedRegisterSet Not(const TypedRegisterSet& in) {
        return TypedRegisterSet(~in.bits_ & T::Codes::AllocatableMask);
    }
    static inline TypedRegisterSet Subtract(const TypedRegisterSet& lhs,
                                            const TypedRegisterSet& rhs)
    {
        return TypedRegisterSet(lhs.bits_ & ~rhs.bits_);
    }
    static inline TypedRegisterSet VolatileNot(const TypedRegisterSet& in) {
        const SetType allocatableVolatile =
            T::Codes::AllocatableMask & T::Codes::VolatileMask;
        return TypedRegisterSet(~in.bits_ & allocatableVolatile);
    }
    static inline TypedRegisterSet Volatile() {
        return TypedRegisterSet(T::Codes::AllocatableMask & T::Codes::VolatileMask);
    }
    static inline TypedRegisterSet NonVolatile() {
        return TypedRegisterSet(T::Codes::AllocatableMask & T::Codes::NonVolatileMask);
    }
    bool has(T reg) const {
        // When checking to see if a set has a register, we only want that exact
        // register, not worrying about aliasing.
        return !!(bits_ & (SetType(1) << reg.code()));
    }
    void addUnchecked(T reg) {
        bits_ |= (SetType(1) << reg.code());
    }
    void addAllAliasedUnchecked(T reg) {
        for (uint32_t a = 0; a < reg.numAliased(); a++) {
            T tmp;
            reg.aliased(a, &tmp);
            bits_ |= (SetType(1) << tmp.code());
        }
    }

    void add(T reg) {
        // Make sure we don't add two overlapping registers.
#ifdef DEBUG
        for (uint32_t a = 0; a < reg.numAliased(); a++) {
            T tmp;
            reg.aliased(a, &tmp);
            MOZ_ASSERT(!has(tmp));
        }
#endif
        addUnchecked(reg);
    }

    void add(ValueOperand value) {
#if defined(JS_NUNBOX32)
        add(value.payloadReg());
        add(value.typeReg());
#elif defined(JS_PUNBOX64)
        add(value.valueReg());
#else
#error "Bad architecture"
#endif
    }
    // Determine if some register are still allocated.  This function should
    // be used with the set of allocatable registers used for the initialization
    // of the current set.
    bool someAllocated(const TypedRegisterSet& allocatable) const {
        return allocatable.bits_ & ~bits_;
    }
    bool empty() const {
        return !bits_;
    }
    void take(T reg) {
        MOZ_ASSERT(has(reg));
        takeUnchecked(reg);
    }
    void takeUnchecked(T reg) {
        bits_ &= ~(SetType(1) << reg.code());
    }
    void takeAllAliasedUnchecked(T reg) {
        for (uint32_t a = 0; a < reg.numAliased(); a++) {
            T tmp;
            reg.aliased(a, &tmp);
            bits_ &= ~(SetType(1) << tmp.code());
        }
    }
    void take(ValueOperand value) {
#if defined(JS_NUNBOX32)
        take(value.payloadReg());
        take(value.typeReg());
#elif defined(JS_PUNBOX64)
        take(value.valueReg());
#else
#error "Bad architecture"
#endif
    }
    void takeUnchecked(ValueOperand value) {
#if defined(JS_NUNBOX32)
        takeUnchecked(value.payloadReg());
        takeUnchecked(value.typeReg());
#elif defined(JS_PUNBOX64)
        takeUnchecked(value.valueReg());
#else
#error "Bad architecture"
#endif
    }
    ValueOperand takeValueOperand() {
#if defined(JS_NUNBOX32)
        return ValueOperand(takeAny(), takeAny());
#elif defined(JS_PUNBOX64)
        return ValueOperand(takeAny());
#else
#error "Bad architecture"
#endif
    }
    T getAny() const {
        // The choice of first or last here is mostly arbitrary, as they are
        // about the same speed on popular architectures. We choose first, as
        // it has the advantage of using the "lower" registers more often. These
        // registers are sometimes more efficient (e.g. optimized encodings for
        // EAX on x86).
        return getFirst();
    }
    T getAnyExcluding(T preclude) {
        MOZ_ASSERT(!empty());
        if (!has(preclude))
            return getAny();

        take(preclude);
        MOZ_ASSERT(!empty());
        T result = getAny();
        add(preclude);
        return result;
    }
    T getFirst() const {
        MOZ_ASSERT(!empty());
        return T::FromCode(T::FirstBit(bits_));
    }
    T getLast() const {
        MOZ_ASSERT(!empty());
        int ireg = T::LastBit(bits_);
        return T::FromCode(ireg);
    }
    T takeAny() {
        MOZ_ASSERT(!empty());
        T reg = getAny();
        take(reg);
        return reg;
    }
    T takeAnyExcluding(T preclude) {
        T reg = getAnyExcluding(preclude);
        take(reg);
        return reg;
    }
    ValueOperand takeAnyValue() {
#if defined(JS_NUNBOX32)
        T type = takeAny();
        T payload = takeAny();
        return ValueOperand(type, payload);
#elif defined(JS_PUNBOX64)
        T reg = takeAny();
        return ValueOperand(reg);
#else
#error "Bad architecture"
#endif
    }
    T takeFirst() {
        MOZ_ASSERT(!empty());
        T reg = getFirst();
        take(reg);
        return reg;
    }
    T takeLast() {
        MOZ_ASSERT(!empty());
        T reg = getLast();
        take(reg);
        return reg;
    }
    void clear() {
        bits_ = 0;
    }
    SetType bits() const {
        return bits_;
    }
    uint32_t size() const {
        return mozilla::CountPopulation32(bits_);
    }
    bool operator ==(const TypedRegisterSet<T>& other) const {
        return other.bits_ == bits_;
    }
    TypedRegisterSet<T> reduceSetForPush() const {
        return T::ReduceSetForPush(*this);
    }
    uint32_t getSizeInBytes() const {
        return T::GetSizeInBytes(*this);
    }
    uint32_t getPushSizeInBytes() const {
        return T::GetPushSizeInBytes(*this);
    }
};

typedef TypedRegisterSet<Register> GeneralRegisterSet;
typedef TypedRegisterSet<FloatRegister> FloatRegisterSet;

class AnyRegisterIterator;

class RegisterSet {
    GeneralRegisterSet gpr_;
    FloatRegisterSet fpu_;

    friend class AnyRegisterIterator;

  public:
    RegisterSet()
    { }
    MOZ_CONSTEXPR RegisterSet(const GeneralRegisterSet& gpr, const FloatRegisterSet& fpu)
      : gpr_(gpr),
        fpu_(fpu)
    { }
    static inline RegisterSet All() {
        return RegisterSet(GeneralRegisterSet::All(), FloatRegisterSet::All());
    }
    static inline RegisterSet Intersect(const RegisterSet& lhs, const RegisterSet& rhs) {
        return RegisterSet(GeneralRegisterSet::Intersect(lhs.gpr_, rhs.gpr_),
                           FloatRegisterSet::Intersect(lhs.fpu_, rhs.fpu_));
    }
    static inline RegisterSet Union(const RegisterSet& lhs, const RegisterSet& rhs) {
        return RegisterSet(GeneralRegisterSet::Union(lhs.gpr_, rhs.gpr_),
                           FloatRegisterSet::Union(lhs.fpu_, rhs.fpu_));
    }
    static inline RegisterSet Not(const RegisterSet& in) {
        return RegisterSet(GeneralRegisterSet::Not(in.gpr_),
                           FloatRegisterSet::Not(in.fpu_));
    }
    static inline RegisterSet VolatileNot(const RegisterSet& in) {
        return RegisterSet(GeneralRegisterSet::VolatileNot(in.gpr_),
                           FloatRegisterSet::VolatileNot(in.fpu_));
    }
    static inline RegisterSet Volatile() {
        return RegisterSet(GeneralRegisterSet::Volatile(), FloatRegisterSet::Volatile());
    }
    bool has(Register reg) const {
        return gpr_.has(reg);
    }
    bool has(FloatRegister reg) const {
        return fpu_.has(reg);
    }
    bool has(AnyRegister reg) const {
        return reg.isFloat() ? has(reg.fpu()) : has(reg.gpr());
    }
    void add(Register reg) {
        gpr_.add(reg);
    }
    void add(FloatRegister reg) {
        fpu_.add(reg);
    }
    void add(AnyRegister any) {
        if (any.isFloat())
            add(any.fpu());
        else
            add(any.gpr());
    }
    void add(ValueOperand value) {
#if defined(JS_NUNBOX32)
        add(value.payloadReg());
        add(value.typeReg());
#elif defined(JS_PUNBOX64)
        add(value.valueReg());
#else
#error "Bad architecture"
#endif
    }
    void add(TypedOrValueRegister reg) {
        if (reg.hasValue())
            add(reg.valueReg());
        else if (reg.hasTyped())
            add(reg.typedReg());
    }
    void addUnchecked(Register reg) {
        gpr_.addUnchecked(reg);
    }
    void addUnchecked(FloatRegister reg) {
        fpu_.addUnchecked(reg);
    }
    void addUnchecked(AnyRegister any) {
        if (any.isFloat())
            addUnchecked(any.fpu());
        else
            addUnchecked(any.gpr());
    }
    void addAllAliasedUnchecked(const AnyRegister& reg) {
        if (reg.isFloat())
            fpu_.addAllAliasedUnchecked(reg.fpu());
        else
            gpr_.addAllAliasedUnchecked(reg.gpr());
    }


    bool empty(bool floats) const {
        return floats ? fpu_.empty() : gpr_.empty();
    }
    FloatRegister takeFloat() {
        return fpu_.takeAny();
    }
    Register takeGeneral() {
        return gpr_.takeAny();
    }
    ValueOperand takeValueOperand() {
#if defined(JS_NUNBOX32)
        return ValueOperand(takeGeneral(), takeGeneral());
#elif defined(JS_PUNBOX64)
        return ValueOperand(takeGeneral());
#else
#error "Bad architecture"
#endif
    }
    void take(AnyRegister reg) {
        if (reg.isFloat())
            fpu_.take(reg.fpu());
        else
            gpr_.take(reg.gpr());
    }
    void takeAllAliasedUnchecked(AnyRegister reg) {
        if (reg.isFloat())
            fpu_.takeAllAliasedUnchecked(reg.fpu());
        else
            gpr_.takeAllAliasedUnchecked(reg.gpr());
    }
    AnyRegister takeAny(bool isFloat) {
        if (isFloat)
            return AnyRegister(takeFloat());
        return AnyRegister(takeGeneral());
    }
    void clear() {
        gpr_.clear();
        fpu_.clear();
    }
    MOZ_CONSTEXPR GeneralRegisterSet gprs() const {
        return gpr_;
    }
    MOZ_CONSTEXPR FloatRegisterSet fpus() const {
        return fpu_;
    }
    bool operator ==(const RegisterSet& other) const {
        return other.gpr_ == gpr_ && other.fpu_ == fpu_;
    }

    void takeUnchecked(Register reg) {
        gpr_.takeUnchecked(reg);
    }
    void takeUnchecked(FloatRegister reg) {
        fpu_.takeUnchecked(reg);
    }
    void takeUnchecked(AnyRegister reg) {
        if (reg.isFloat())
            fpu_.takeUnchecked(reg.fpu());
        else
            gpr_.takeUnchecked(reg.gpr());
    }
    void takeUnchecked(ValueOperand value) {
        gpr_.takeUnchecked(value);
    }
    void takeUnchecked(TypedOrValueRegister reg) {
        if (reg.hasValue())
            takeUnchecked(reg.valueReg());
        else if (reg.hasTyped())
            takeUnchecked(reg.typedReg());
    }
};

// iterates in whatever order happens to be convenient.
// Use TypedRegisterBackwardIterator or TypedRegisterForwardIterator if a
// specific order is required.
template <typename T>
class TypedRegisterIterator
{
    TypedRegisterSet<T> regset_;

  public:
    explicit TypedRegisterIterator(TypedRegisterSet<T> regset) : regset_(regset)
    { }
    TypedRegisterIterator(const TypedRegisterIterator& other) : regset_(other.regset_)
    { }

    bool more() const {
        return !regset_.empty();
    }
    TypedRegisterIterator<T> operator ++(int) {
        TypedRegisterIterator<T> old(*this);
        regset_.takeAny();
        return old;
    }
    TypedRegisterIterator<T>& operator ++() {
        regset_.takeAny();
        return *this;
    }
    T operator*() const {
        return regset_.getAny();
    }
};

// iterates backwards, that is, rn to r0
template <typename T>
class TypedRegisterBackwardIterator
{
    TypedRegisterSet<T> regset_;

  public:
    explicit TypedRegisterBackwardIterator(TypedRegisterSet<T> regset) : regset_(regset)
    { }
    TypedRegisterBackwardIterator(const TypedRegisterBackwardIterator& other)
      : regset_(other.regset_)
    { }

    bool more() const {
        return !regset_.empty();
    }
    TypedRegisterBackwardIterator<T> operator ++(int) {
        TypedRegisterBackwardIterator<T> old(*this);
        regset_.takeLast();
        return old;
    }
    TypedRegisterBackwardIterator<T>& operator ++() {
        regset_.takeLast();
        return *this;
    }
    T operator*() const {
        return regset_.getLast();
    }
};

// iterates forwards, that is r0 to rn
template <typename T>
class TypedRegisterForwardIterator
{
    TypedRegisterSet<T> regset_;

  public:
    explicit TypedRegisterForwardIterator(TypedRegisterSet<T> regset) : regset_(regset)
    { }
    TypedRegisterForwardIterator(const TypedRegisterForwardIterator& other) : regset_(other.regset_)
    { }

    bool more() const {
        return !regset_.empty();
    }
    TypedRegisterForwardIterator<T> operator ++(int) {
        TypedRegisterForwardIterator<T> old(*this);
        regset_.takeFirst();
        return old;
    }
    TypedRegisterForwardIterator<T>& operator ++() {
        regset_.takeFirst();
        return *this;
    }
    T operator*() const {
        return regset_.getFirst();
    }
};

typedef TypedRegisterIterator<Register> GeneralRegisterIterator;
typedef TypedRegisterIterator<FloatRegister> FloatRegisterIterator;
typedef TypedRegisterBackwardIterator<Register> GeneralRegisterBackwardIterator;
typedef TypedRegisterBackwardIterator<FloatRegister> FloatRegisterBackwardIterator;
typedef TypedRegisterForwardIterator<Register> GeneralRegisterForwardIterator;
typedef TypedRegisterForwardIterator<FloatRegister> FloatRegisterForwardIterator;

class AnyRegisterIterator
{
    GeneralRegisterIterator geniter_;
    FloatRegisterIterator floatiter_;

  public:
    AnyRegisterIterator()
      : geniter_(GeneralRegisterSet::All()), floatiter_(FloatRegisterSet::All())
    { }
    AnyRegisterIterator(GeneralRegisterSet genset, FloatRegisterSet floatset)
      : geniter_(genset), floatiter_(floatset)
    { }
    explicit AnyRegisterIterator(const RegisterSet& set)
      : geniter_(set.gpr_), floatiter_(set.fpu_)
    { }
    AnyRegisterIterator(const AnyRegisterIterator& other)
      : geniter_(other.geniter_), floatiter_(other.floatiter_)
    { }
    bool more() const {
        return geniter_.more() || floatiter_.more();
    }
    AnyRegisterIterator operator ++(int) {
        AnyRegisterIterator old(*this);
        if (geniter_.more())
            geniter_++;
        else
            floatiter_++;
        return old;
    }
    AnyRegister operator*() const {
        if (geniter_.more())
            return AnyRegister(*geniter_);
        return AnyRegister(*floatiter_);
    }
};

class ABIArg
{
  public:
    enum Kind { GPR, FPU, Stack };

  private:
    Kind kind_;
    union {
        Registers::Code gpr_;
        FloatRegisters::Code fpu_;
        uint32_t offset_;
    } u;

  public:
    ABIArg() : kind_(Kind(-1)) { u.offset_ = -1; }
    explicit ABIArg(Register gpr) : kind_(GPR) { u.gpr_ = gpr.code(); }
    explicit ABIArg(FloatRegister fpu) : kind_(FPU) { u.fpu_ = fpu.code(); }
    explicit ABIArg(uint32_t offset) : kind_(Stack) { u.offset_ = offset; }

    Kind kind() const { return kind_; }
    Register gpr() const { MOZ_ASSERT(kind() == GPR); return Register::FromCode(u.gpr_); }
    FloatRegister fpu() const { MOZ_ASSERT(kind() == FPU); return FloatRegister::FromCode(u.fpu_); }
    uint32_t offsetFromArgBase() const { MOZ_ASSERT(kind() == Stack); return u.offset_; }

    bool argInRegister() const { return kind() != Stack; }
    AnyRegister reg() const { return kind_ == GPR ? AnyRegister(gpr()) : AnyRegister(fpu()); }
};

} // namespace jit
} // namespace js

#endif /* jit_RegisterSets_h */

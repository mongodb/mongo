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
  public:
    typedef T RegType;
    typedef typename T::SetType SetType;

  private:
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

    bool empty() const {
        return !bits_;
    }
    void clear() {
        bits_ = 0;
    }

    bool hasRegisterIndex(T reg) const {
        return !!(bits_ & (SetType(1) << reg.code()));
    }
    bool hasAllocatable(T reg) const {
        return !(~bits_ & reg.alignedOrDominatedAliasedSet());
    }

    void addRegisterIndex(T reg) {
        bits_ |= (SetType(1) << reg.code());
    }
    void addAllocatable(T reg) {
        bits_ |= reg.alignedOrDominatedAliasedSet();
    }


    void takeRegisterIndex(T reg) {
        bits_ &= ~(SetType(1) << reg.code());
    }
    void takeAllocatable(T reg) {
        bits_ &= ~reg.alignedOrDominatedAliasedSet();
    }

    T getAny() const {
        // The choice of first or last here is mostly arbitrary, as they are
        // about the same speed on popular architectures. We choose first, as
        // it has the advantage of using the "lower" registers more often. These
        // registers are sometimes more efficient (e.g. optimized encodings for
        // EAX on x86).
        return getFirst();
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

    SetType bits() const {
        return bits_;
    }
    uint32_t size() const {
        return T::SetSize(bits_);
    }
    bool operator ==(const TypedRegisterSet<T>& other) const {
        return other.bits_ == bits_;
    }
    TypedRegisterSet<T> reduceSetForPush() const {
        return T::ReduceSetForPush(*this);
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

    bool empty() const {
        return fpu_.empty() && gpr_.empty();
    }
    void clear() {
        fpu_.clear();
        gpr_.clear();
    }
    bool emptyGeneral() const {
        return gpr_.empty();
    }
    bool emptyFloat() const {
        return fpu_.empty();
    }
    MOZ_CONSTEXPR GeneralRegisterSet gprs() const {
        return gpr_;
    }
    GeneralRegisterSet& gprs() {
        return gpr_;
    }
    MOZ_CONSTEXPR FloatRegisterSet fpus() const {
        return fpu_;
    }
    FloatRegisterSet& fpus() {
        return fpu_;
    }
    bool operator ==(const RegisterSet& other) const {
        return other.gpr_ == gpr_ && other.fpu_ == fpu_;
    }

};

// There are 2 use cases for register sets:
//
//   1. To serve as a pool of allocatable register. This is useful for working
//      on the code produced by some stub where free registers are available, or
//      when we can release some registers.
//
//   2. To serve as a list of typed registers. This is useful for working with
//      live registers and to manipulate them with the proper instructions. This
//      is used by the register allocator to fill the Safepoints.
//
// These 2 uses cases can be used on top of 3 different backend representation
// of register sets, which are either GeneralRegisterSet, FloatRegisterSet, or
// RegisterSet (for both). These classes are used to store the bit sets to
// represent each register.
//
// Each use case defines an Accessor class, such as AllocatableSetAccessor or
// LiveSetAccessor, which is parameterized with the type of the register
// set. These accessors are in charge of manipulating the register set in a
// consistent way.
//
// The RegSetCommonInterface class is used to wrap the accessors with convenient
// shortcuts which are based on the accessors.
//
// Then, to avoid to many levels of complexity while using these interfaces,
// shortcut templates are created to make it easy to distinguish between a
// register set used for allocating registers, or a register set used for making
// a collection of allocated (live) registers.
//
// This separation exists to prevent mixing LiveSet and AllocatableSet
// manipulations of the same register set, and ensure safety while avoiding
// false positive.

template <typename RegisterSet>
class AllocatableSet;

template <typename RegisterSet>
class LiveSet;

// Base accessors classes have the minimal set of raw methods to manipulate the register set
// given as parameter in a consistent manner.  These methods are:
//
//    - has: Returns if all the bits needed to take a register are present.
//
//    - takeUnchecked: Subtracts the bits used to represent the register in the
//      register set.
//
//    - addUnchecked: Adds the bits used to represent the register in the
//      register set.

// The AllocatableSet accessors are used to make a pool of unused
// registers. Taking or adding registers should consider the aliasing rules of
// the architecture.  For example, on ARM, the following piece of code should
// work fine, knowing that the double register |d0| is composed of float
// registers |s0| and |s1|:
//
//     AllocatableFloatRegisterSet regs;
//     regs.add(s0);
//     regs.add(s1);
//     // d0 is now available.
//     regs.take(d0);
//
// These accessors are useful for allocating registers within the functions used
// to generate stubs, trampolines, and inline caches (BaselineIC, IonCache).
template <typename Set>
class AllocatableSetAccessors
{
  public:
    typedef Set RegSet;
    typedef typename RegSet::RegType RegType;
    typedef typename RegSet::SetType SetType;

  protected:
    RegSet set_;

  public:
    AllocatableSetAccessors() : set_() {}
    explicit MOZ_CONSTEXPR AllocatableSetAccessors(SetType set) : set_(set) {}
    explicit MOZ_CONSTEXPR AllocatableSetAccessors(RegSet set) : set_(set) {}

    bool has(RegType reg) const {
        return set_.hasAllocatable(reg);
    }

    void addUnchecked(RegType reg) {
        set_.addAllocatable(reg);
    }

    void takeUnchecked(RegType reg) {
        set_.takeAllocatable(reg);
    }
};

// Specialization of the AllocatableSet accessors for the RegisterSet aggregate.
template <>
class AllocatableSetAccessors<RegisterSet>
{
  public:
    typedef RegisterSet RegSet;
    typedef AnyRegister RegType;
    typedef char SetType;

  protected:
    RegisterSet set_;

  public:
    AllocatableSetAccessors() : set_() {}
    explicit MOZ_CONSTEXPR AllocatableSetAccessors(SetType) = delete;
    explicit MOZ_CONSTEXPR AllocatableSetAccessors(RegisterSet set) : set_(set) {}

    bool has(Register reg) const {
        return set_.gprs().hasAllocatable(reg);
    }
    bool has(FloatRegister reg) const {
        return set_.fpus().hasAllocatable(reg);
    }

    void addUnchecked(Register reg) {
        set_.gprs().addAllocatable(reg);
    }
    void addUnchecked(FloatRegister reg) {
        set_.fpus().addAllocatable(reg);
    }

    void takeUnchecked(Register reg) {
        set_.gprs().takeAllocatable(reg);
    }
    void takeUnchecked(FloatRegister reg) {
        set_.fpus().takeAllocatable(reg);
    }
};


// The LiveSet accessors are used to collect a list of allocated
// registers. Taking or adding a register should *not* consider the aliases, as
// we care about interpreting the registers with the correct type.  For example,
// on x64, where one float registers can be interpreted as an Simd128, a Double,
// or a Float, adding xmm0 as an Simd128, does not make the register available
// as a Double.
//
//     LiveFloatRegisterSet regs;
//     regs.add(xmm0.asSimd128());
//     regs.take(xmm0); // Assert!
//
// These accessors are useful for recording the result of a register allocator,
// such as what the Backtracking allocator do on the Safepoints.
template <typename Set>
class LiveSetAccessors
{
  public:
    typedef Set RegSet;
    typedef typename RegSet::RegType RegType;
    typedef typename RegSet::SetType SetType;

  protected:
    RegSet set_;

  public:
    LiveSetAccessors() : set_() {}
    explicit MOZ_CONSTEXPR LiveSetAccessors(SetType set) : set_(set) {}
    explicit MOZ_CONSTEXPR LiveSetAccessors(RegSet set) : set_(set) {}

    bool has(RegType reg) const {
        return set_.hasRegisterIndex(reg);
    }

    void addUnchecked(RegType reg) {
        set_.addRegisterIndex(reg);
    }

    void takeUnchecked(RegType reg) {
        set_.takeRegisterIndex(reg);
    }
};

// Specialization of the LiveSet accessors for the RegisterSet aggregate.
template <>
class LiveSetAccessors<RegisterSet>
{
  public:
    typedef RegisterSet RegSet;
    typedef AnyRegister RegType;
    typedef char SetType;

  protected:
    RegisterSet set_;

  public:
    LiveSetAccessors() : set_() {}
    explicit MOZ_CONSTEXPR LiveSetAccessors(SetType) = delete;
    explicit MOZ_CONSTEXPR LiveSetAccessors(RegisterSet set) : set_(set) {}

    bool has(Register reg) const {
        return set_.gprs().hasRegisterIndex(reg);
    }
    bool has(FloatRegister reg) const {
        return set_.fpus().hasRegisterIndex(reg);
    }

    void addUnchecked(Register reg) {
        set_.gprs().addRegisterIndex(reg);
    }
    void addUnchecked(FloatRegister reg) {
        set_.fpus().addRegisterIndex(reg);
    }

    void takeUnchecked(Register reg) {
        set_.gprs().takeRegisterIndex(reg);
    }
    void takeUnchecked(FloatRegister reg) {
        set_.fpus().takeRegisterIndex(reg);
    }
};

#define DEFINE_ACCESSOR_CONSTRUCTORS_(REGSET)                         \
    typedef typename Parent::RegSet  RegSet;                          \
    typedef typename Parent::RegType RegType;                         \
    typedef typename Parent::SetType SetType;                         \
                                                                      \
    MOZ_CONSTEXPR_TMPL REGSET() : Parent() {}                         \
    explicit MOZ_CONSTEXPR_TMPL REGSET(SetType set) : Parent(set) {}  \
    explicit MOZ_CONSTEXPR_TMPL REGSET(RegSet set) : Parent(set) {}

// This class adds checked accessors on top of the unchecked variants defined by
// AllocatableSet and LiveSet accessors. Also it defines interface which are
// specialized to the register set implementation, such as |getAny| and
// |takeAny| variants.
template <class Accessors, typename Set>
class SpecializedRegSet : public Accessors
{
    typedef Accessors Parent;

  public:
    DEFINE_ACCESSOR_CONSTRUCTORS_(SpecializedRegSet)

    SetType bits() const {
        return this->Parent::set_.bits();
    }

    using Parent::has;

    using Parent::addUnchecked;
    void add(RegType reg) {
        MOZ_ASSERT(!has(reg));
        addUnchecked(reg);
    }

    using Parent::takeUnchecked;
    void take(RegType reg) {
        MOZ_ASSERT(has(reg));
        takeUnchecked(reg);
    }

    RegType getAny() const {
        return this->Parent::set_.getAny();
    }
    RegType getFirst() const {
        return this->Parent::set_.getFirst();
    }
    RegType getLast() const {
        return this->Parent::set_.getLast();
    }

    RegType getAnyExcluding(RegType preclude) {
        if (!has(preclude))
            return getAny();

        take(preclude);
        RegType result = getAny();
        add(preclude);
        return result;
    }

    RegType takeAny() {
        RegType reg = getAny();
        take(reg);
        return reg;
    }
    RegType takeFirst() {
        RegType reg = getFirst();
        take(reg);
        return reg;
    }
    RegType takeLast() {
        RegType reg = getLast();
        take(reg);
        return reg;
    }

    ValueOperand takeAnyValue() {
#if defined(JS_NUNBOX32)
        return ValueOperand(takeAny(), takeAny());
#elif defined(JS_PUNBOX64)
        return ValueOperand(takeAny());
#else
#error "Bad architecture"
#endif
    }

    RegType takeAnyExcluding(RegType preclude) {
        RegType reg = getAnyExcluding(preclude);
        take(reg);
        return reg;
    }
};

// Specialization of the accessors for the RegisterSet aggregate.
template <class Accessors>
class SpecializedRegSet<Accessors, RegisterSet> : public Accessors
{
    typedef Accessors Parent;

  public:
    DEFINE_ACCESSOR_CONSTRUCTORS_(SpecializedRegSet)

    GeneralRegisterSet gprs() const {
        return this->Parent::set_.gprs();
    }
    GeneralRegisterSet& gprs() {
        return this->Parent::set_.gprs();
    }
    FloatRegisterSet fpus() const {
        return this->Parent::set_.fpus();
    }
    FloatRegisterSet& fpus() {
        return this->Parent::set_.fpus();
    }

    bool emptyGeneral() const {
        return this->Parent::set_.emptyGeneral();
    }
    bool emptyFloat() const {
        return this->Parent::set_.emptyFloat();
    }


    using Parent::has;
    bool has(AnyRegister reg) const {
        return reg.isFloat() ? has(reg.fpu()) : has(reg.gpr());
    }


    using Parent::addUnchecked;
    void addUnchecked(AnyRegister reg) {
        if (reg.isFloat())
            addUnchecked(reg.fpu());
        else
            addUnchecked(reg.gpr());
    }

    void add(Register reg) {
        MOZ_ASSERT(!has(reg));
        addUnchecked(reg);
    }
    void add(FloatRegister reg) {
        MOZ_ASSERT(!has(reg));
        addUnchecked(reg);
    }
    void add(AnyRegister reg) {
        if (reg.isFloat())
            add(reg.fpu());
        else
            add(reg.gpr());
    }

    using Parent::takeUnchecked;
    void takeUnchecked(AnyRegister reg) {
        if (reg.isFloat())
            takeUnchecked(reg.fpu());
        else
            takeUnchecked(reg.gpr());
    }

    void take(Register reg) {
        MOZ_ASSERT(has(reg));
        takeUnchecked(reg);
    }
    void take(FloatRegister reg) {
        MOZ_ASSERT(has(reg));
        takeUnchecked(reg);
    }
    void take(AnyRegister reg) {
        if (reg.isFloat())
            take(reg.fpu());
        else
            take(reg.gpr());
    }

    Register getAnyGeneral() const {
        return this->Parent::set_.gprs().getAny();
    }
    FloatRegister getAnyFloat() const {
        return this->Parent::set_.fpus().getAny();
    }

    Register takeAnyGeneral() {
        Register reg = getAnyGeneral();
        take(reg);
        return reg;
    }
    FloatRegister takeAnyFloat() {
        FloatRegister reg = getAnyFloat();
        take(reg);
        return reg;
    }
    ValueOperand takeAnyValue() {
#if defined(JS_NUNBOX32)
        return ValueOperand(takeAnyGeneral(), takeAnyGeneral());
#elif defined(JS_PUNBOX64)
        return ValueOperand(takeAnyGeneral());
#else
#error "Bad architecture"
#endif
    }
};


// Interface which is common to all register set implementations. It overloads
// |add|, |take| and |takeUnchecked| methods for types such as |ValueOperand|
// and |TypedOrValueRegister|.
template <class Accessors, typename Set>
class CommonRegSet : public SpecializedRegSet<Accessors, Set>
{
    typedef SpecializedRegSet<Accessors, Set> Parent;

  public:
    DEFINE_ACCESSOR_CONSTRUCTORS_(CommonRegSet)

    RegSet set() const {
        return this->Parent::set_;
    }
    RegSet& set() {
        return this->Parent::set_;
    }

    bool empty() const {
        return this->Parent::set_.empty();
    }
    void clear() {
        this->Parent::set_.clear();
    }

    using Parent::add;
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

    using Parent::take;
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
    void take(TypedOrValueRegister reg) {
        if (reg.hasValue())
            take(reg.valueReg());
        else if (reg.hasTyped())
            take(reg.typedReg());
    }

    using Parent::takeUnchecked;
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
    void takeUnchecked(TypedOrValueRegister reg) {
        if (reg.hasValue())
            takeUnchecked(reg.valueReg());
        else if (reg.hasTyped())
            takeUnchecked(reg.typedReg());
    }
};


// These classes do not provide any additional members, they only use their
// constructors to forward to the common interface for all register sets.  The
// only benefit of these classes is to provide user friendly names.
template <typename Set>
class LiveSet : public CommonRegSet<LiveSetAccessors<Set>, Set>
{
    typedef CommonRegSet<LiveSetAccessors<Set>, Set> Parent;

  public:
    DEFINE_ACCESSOR_CONSTRUCTORS_(LiveSet)
};

template <typename Set>
class AllocatableSet : public CommonRegSet<AllocatableSetAccessors<Set>, Set>
{
    typedef CommonRegSet<AllocatableSetAccessors<Set>, Set> Parent;

  public:
    DEFINE_ACCESSOR_CONSTRUCTORS_(AllocatableSet)

    LiveSet<Set> asLiveSet() const {
        return LiveSet<Set>(this->set());
    }
};

#define DEFINE_ACCESSOR_CONSTRUCTORS_FOR_REGISTERSET_(REGSET)               \
    typedef Parent::RegSet  RegSet;                                         \
    typedef Parent::RegType RegType;                                        \
    typedef Parent::SetType SetType;                                        \
                                                                            \
    MOZ_CONSTEXPR_TMPL REGSET() : Parent() {}                               \
    explicit MOZ_CONSTEXPR_TMPL REGSET(SetType) = delete;                   \
    explicit MOZ_CONSTEXPR_TMPL REGSET(RegSet set) : Parent(set) {}         \
    MOZ_CONSTEXPR_TMPL REGSET(GeneralRegisterSet gpr, FloatRegisterSet fpu) \
      : Parent(RegisterSet(gpr, fpu))                                       \
    {}                                                                      \
    REGSET(REGSET<GeneralRegisterSet> gpr, REGSET<FloatRegisterSet> fpu)    \
      : Parent(RegisterSet(gpr.set(), fpu.set()))                           \
    {}

template <>
class LiveSet<RegisterSet>
  : public CommonRegSet<LiveSetAccessors<RegisterSet>, RegisterSet>
{
    // Note: We have to provide a qualified name for LiveSetAccessors, as it is
    // interpreted as being the specialized class name inherited from the parent
    // class specialization.
    typedef CommonRegSet<jit::LiveSetAccessors<RegisterSet>, RegisterSet> Parent;

  public:
    DEFINE_ACCESSOR_CONSTRUCTORS_FOR_REGISTERSET_(LiveSet)
};

template <>
class AllocatableSet<RegisterSet>
  : public CommonRegSet<AllocatableSetAccessors<RegisterSet>, RegisterSet>
{
    // Note: We have to provide a qualified name for AllocatableSetAccessors, as
    // it is interpreted as being the specialized class name inherited from the
    // parent class specialization.
    typedef CommonRegSet<jit::AllocatableSetAccessors<RegisterSet>, RegisterSet> Parent;

  public:
    DEFINE_ACCESSOR_CONSTRUCTORS_FOR_REGISTERSET_(AllocatableSet)

    LiveSet<RegisterSet> asLiveSet() const {
        return LiveSet<RegisterSet>(this->set());
    }
};

#undef DEFINE_ACCESSOR_CONSTRUCTORS_FOR_REGISTERSET_
#undef DEFINE_ACCESSOR_CONSTRUCTORS_

typedef AllocatableSet<GeneralRegisterSet> AllocatableGeneralRegisterSet;
typedef AllocatableSet<FloatRegisterSet> AllocatableFloatRegisterSet;
typedef AllocatableSet<RegisterSet> AllocatableRegisterSet;

typedef LiveSet<GeneralRegisterSet> LiveGeneralRegisterSet;
typedef LiveSet<FloatRegisterSet> LiveFloatRegisterSet;
typedef LiveSet<RegisterSet> LiveRegisterSet;

// iterates in whatever order happens to be convenient.
// Use TypedRegisterBackwardIterator or TypedRegisterForwardIterator if a
// specific order is required.
template <typename T>
class TypedRegisterIterator
{
    LiveSet<TypedRegisterSet<T>> regset_;

  public:
    explicit TypedRegisterIterator(TypedRegisterSet<T> regset) : regset_(regset)
    { }
    explicit TypedRegisterIterator(LiveSet<TypedRegisterSet<T>> regset) : regset_(regset)
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
    LiveSet<TypedRegisterSet<T>> regset_;

  public:
    explicit TypedRegisterBackwardIterator(TypedRegisterSet<T> regset) : regset_(regset)
    { }
    explicit TypedRegisterBackwardIterator(LiveSet<TypedRegisterSet<T>> regset) : regset_(regset)
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
    LiveSet<TypedRegisterSet<T>> regset_;

  public:
    explicit TypedRegisterForwardIterator(TypedRegisterSet<T> regset) : regset_(regset)
    { }
    explicit TypedRegisterForwardIterator(LiveSet<TypedRegisterSet<T>> regset) : regset_(regset)
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
    explicit AnyRegisterIterator(const LiveSet<RegisterSet>& set)
      : geniter_(set.gprs()), floatiter_(set.fpus())
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
    enum Kind {
        GPR,
#ifdef JS_CODEGEN_REGISTER_PAIR
        GPR_PAIR,
#endif
        FPU,
        Stack
    };

  private:
    Kind kind_;
    union {
        Register::Code gpr_;
        FloatRegister::Code fpu_;
        uint32_t offset_;
    } u;

  public:
    ABIArg() : kind_(Kind(-1)) { u.offset_ = -1; }
    explicit ABIArg(Register gpr) : kind_(GPR) { u.gpr_ = gpr.code(); }
    explicit ABIArg(Register gprLow, Register gprHigh)
    {
#if defined(JS_CODEGEN_REGISTER_PAIR)
        kind_ = GPR_PAIR;
#else
        MOZ_CRASH("Unsupported type of ABI argument.");
#endif
        u.gpr_ = gprLow.code();
        MOZ_ASSERT(u.gpr_ % 2 == 0);
        MOZ_ASSERT(u.gpr_ + 1 == gprHigh.code());
    }
    explicit ABIArg(FloatRegister fpu) : kind_(FPU) { u.fpu_ = fpu.code(); }
    explicit ABIArg(uint32_t offset) : kind_(Stack) { u.offset_ = offset; }

    Kind kind() const { return kind_; }
#ifdef JS_CODEGEN_REGISTER_PAIR
    bool isGeneralRegPair() const { return kind_ == GPR_PAIR; }
#else
    bool isGeneralRegPair() const { return false; }
#endif

    Register gpr() const {
        MOZ_ASSERT(kind() == GPR);
        return Register::FromCode(u.gpr_);
    }
    Register evenGpr() const {
        MOZ_ASSERT(isGeneralRegPair());
        return Register::FromCode(u.gpr_);
    }
    Register oddGpr() const {
        MOZ_ASSERT(isGeneralRegPair());
        return Register::FromCode(u.gpr_ + 1);
    }
    FloatRegister fpu() const { MOZ_ASSERT(kind() == FPU); return FloatRegister::FromCode(u.fpu_); }
    uint32_t offsetFromArgBase() const { MOZ_ASSERT(kind() == Stack); return u.offset_; }

    bool argInRegister() const { return kind() != Stack; }
    AnyRegister reg() const { return kind_ == GPR ? AnyRegister(gpr()) : AnyRegister(fpu()); }
};

// Get the set of registers which should be saved by a block of code which
// clobbers all registers besides |unused|, but does not clobber floating point
// registers.
inline LiveGeneralRegisterSet
SavedNonVolatileRegisters(AllocatableGeneralRegisterSet unused)
{
    LiveGeneralRegisterSet result;

    for (GeneralRegisterIterator iter(GeneralRegisterSet::NonVolatile()); iter.more(); iter++) {
        Register reg = *iter;
        if (!unused.has(reg))
            result.add(reg);
    }

    // Some platforms require the link register to be saved, if calls can be made.
#if defined(JS_CODEGEN_ARM)
    result.add(Register::FromCode(Registers::lr));
#elif defined(JS_CODEGEN_ARM64)
    result.add(Register::FromCode(Registers::lr));
#elif defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
    result.add(Register::FromCode(Registers::ra));
#endif

    return result;
}

} // namespace jit
} // namespace js

#endif /* jit_RegisterSets_h */

/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_shared_Assembler_shared_h
#define jit_shared_Assembler_shared_h

#include "mozilla/PodOperations.h"

#include <limits.h>

#include "asmjs/AsmJSFrameIterator.h"
#include "jit/JitAllocPolicy.h"
#include "jit/Label.h"
#include "jit/Registers.h"
#include "jit/RegisterSets.h"
#include "vm/HelperThreads.h"

#if defined(JS_CODEGEN_ARM)
#define JS_USE_LINK_REGISTER
#endif

#if defined(JS_CODEGEN_X64) || defined(JS_CODEGEN_ARM)
// JS_SMALL_BRANCH means the range on a branch instruction
// is smaller than the whole address space
#    define JS_SMALL_BRANCH
#endif
namespace js {
namespace jit {

namespace Disassembler {
class HeapAccess;
};

static const uint32_t Simd128DataSize = 4 * sizeof(int32_t);
static_assert(Simd128DataSize == 4 * sizeof(int32_t), "SIMD data should be able to contain int32x4");
static_assert(Simd128DataSize == 4 * sizeof(float), "SIMD data should be able to contain float32x4");
static_assert(Simd128DataSize == 2 * sizeof(double), "SIMD data should be able to contain float64x2");

enum Scale {
    TimesOne = 0,
    TimesTwo = 1,
    TimesFour = 2,
    TimesEight = 3
};

static_assert(sizeof(JS::Value) == 8,
              "required for TimesEight and 3 below to be correct");
static const Scale ValueScale = TimesEight;
static const size_t ValueShift = 3;

static inline unsigned
ScaleToShift(Scale scale)
{
    return unsigned(scale);
}

static inline bool
IsShiftInScaleRange(int i)
{
    return i >= TimesOne && i <= TimesEight;
}

static inline Scale
ShiftToScale(int i)
{
    MOZ_ASSERT(IsShiftInScaleRange(i));
    return Scale(i);
}

static inline Scale
ScaleFromElemWidth(int shift)
{
    switch (shift) {
      case 1:
        return TimesOne;
      case 2:
        return TimesTwo;
      case 4:
        return TimesFour;
      case 8:
        return TimesEight;
    }

    MOZ_CRASH("Invalid scale");
}

// Used for 32-bit immediates which do not require relocation.
struct Imm32
{
    int32_t value;

    explicit Imm32(int32_t value) : value(value)
    { }

    static inline Imm32 ShiftOf(enum Scale s) {
        switch (s) {
          case TimesOne:
            return Imm32(0);
          case TimesTwo:
            return Imm32(1);
          case TimesFour:
            return Imm32(2);
          case TimesEight:
            return Imm32(3);
        };
        MOZ_CRASH("Invalid scale");
    }

    static inline Imm32 FactorOf(enum Scale s) {
        return Imm32(1 << ShiftOf(s).value);
    }
};

// Pointer-sized integer to be embedded as an immediate in an instruction.
struct ImmWord
{
    uintptr_t value;

    explicit ImmWord(uintptr_t value) : value(value)
    { }
};

#ifdef DEBUG
static inline bool
IsCompilingAsmJS()
{
    // asm.js compilation pushes a JitContext with a null JSCompartment.
    JitContext* jctx = MaybeGetJitContext();
    return jctx && jctx->compartment == nullptr;
}

static inline bool
CanUsePointerImmediates()
{
    if (!IsCompilingAsmJS())
        return true;

    // Pointer immediates can still be used with asm.js when the resulting code
    // is being profiled; the module will not be serialized in this case.
    JitContext* jctx = MaybeGetJitContext();
    if (jctx && jctx->runtime->profilingScripts())
        return true;

    return false;
}
#endif

// Pointer to be embedded as an immediate in an instruction.
struct ImmPtr
{
    void* value;

    explicit ImmPtr(const void* value) : value(const_cast<void*>(value))
    {
        // To make code serialization-safe, asm.js compilation should only
        // compile pointer immediates using AsmJSImmPtr.
        MOZ_ASSERT(CanUsePointerImmediates());
    }

    template <class R>
    explicit ImmPtr(R (*pf)())
      : value(JS_FUNC_TO_DATA_PTR(void*, pf))
    {
        MOZ_ASSERT(CanUsePointerImmediates());
    }

    template <class R, class A1>
    explicit ImmPtr(R (*pf)(A1))
      : value(JS_FUNC_TO_DATA_PTR(void*, pf))
    {
        MOZ_ASSERT(CanUsePointerImmediates());
    }

    template <class R, class A1, class A2>
    explicit ImmPtr(R (*pf)(A1, A2))
      : value(JS_FUNC_TO_DATA_PTR(void*, pf))
    {
        MOZ_ASSERT(CanUsePointerImmediates());
    }

    template <class R, class A1, class A2, class A3>
    explicit ImmPtr(R (*pf)(A1, A2, A3))
      : value(JS_FUNC_TO_DATA_PTR(void*, pf))
    {
        MOZ_ASSERT(CanUsePointerImmediates());
    }

    template <class R, class A1, class A2, class A3, class A4>
    explicit ImmPtr(R (*pf)(A1, A2, A3, A4))
      : value(JS_FUNC_TO_DATA_PTR(void*, pf))
    {
        MOZ_ASSERT(CanUsePointerImmediates());
    }

};

// The same as ImmPtr except that the intention is to patch this
// instruction. The initial value of the immediate is 'addr' and this value is
// either clobbered or used in the patching process.
struct PatchedImmPtr {
    void* value;

    explicit PatchedImmPtr()
      : value(nullptr)
    { }
    explicit PatchedImmPtr(const void* value)
      : value(const_cast<void*>(value))
    { }
};

class AssemblerShared;
class ImmGCPtr;

// Used for immediates which require relocation and may be traced during minor GC.
class ImmMaybeNurseryPtr
{
    friend class AssemblerShared;
    friend class ImmGCPtr;
    const gc::Cell* value;

    ImmMaybeNurseryPtr() : value(0) {}

  public:
    explicit ImmMaybeNurseryPtr(const gc::Cell* ptr) : value(ptr)
    {
        MOZ_ASSERT(!IsPoisonedPtr(ptr));

        // asm.js shouldn't be creating GC things
        MOZ_ASSERT(!IsCompilingAsmJS());
    }
};

// Dummy value used for nursery pointers during Ion compilation, see
// LNurseryObject.
class IonNurseryPtr
{
    const gc::Cell* ptr;

  public:
    friend class ImmGCPtr;

    explicit IonNurseryPtr(const gc::Cell* ptr) : ptr(ptr)
    {
        MOZ_ASSERT(ptr);
        MOZ_ASSERT(uintptr_t(ptr) & 0x1);
    }
};

// Used for immediates which require relocation.
class ImmGCPtr
{
  public:
    const gc::Cell* value;

    explicit ImmGCPtr(const gc::Cell* ptr) : value(ptr)
    {
        MOZ_ASSERT(!IsPoisonedPtr(ptr));
        MOZ_ASSERT_IF(ptr, ptr->isTenured());

        // asm.js shouldn't be creating GC things
        MOZ_ASSERT(!IsCompilingAsmJS());
    }

    explicit ImmGCPtr(IonNurseryPtr ptr) : value(ptr.ptr)
    {
        MOZ_ASSERT(!IsPoisonedPtr(value));
        MOZ_ASSERT(value);

        // asm.js shouldn't be creating GC things
        MOZ_ASSERT(!IsCompilingAsmJS());
    }

  private:
    ImmGCPtr() : value(0) {}

    friend class AssemblerShared;
    explicit ImmGCPtr(ImmMaybeNurseryPtr ptr) : value(ptr.value)
    {
        MOZ_ASSERT(!IsPoisonedPtr(ptr.value));

        // asm.js shouldn't be creating GC things
        MOZ_ASSERT(!IsCompilingAsmJS());
    }
};

// Pointer to be embedded as an immediate that is loaded/stored from by an
// instruction.
struct AbsoluteAddress
{
    void* addr;

    explicit AbsoluteAddress(const void* addr)
      : addr(const_cast<void*>(addr))
    {
        MOZ_ASSERT(CanUsePointerImmediates());
    }

    AbsoluteAddress offset(ptrdiff_t delta) {
        return AbsoluteAddress(((uint8_t*) addr) + delta);
    }
};

// The same as AbsoluteAddress except that the intention is to patch this
// instruction. The initial value of the immediate is 'addr' and this value is
// either clobbered or used in the patching process.
struct PatchedAbsoluteAddress
{
    void* addr;

    explicit PatchedAbsoluteAddress()
      : addr(nullptr)
    { }
    explicit PatchedAbsoluteAddress(const void* addr)
      : addr(const_cast<void*>(addr))
    { }
};

// Specifies an address computed in the form of a register base and a constant,
// 32-bit offset.
struct Address
{
    Register base;
    int32_t offset;

    Address(Register base, int32_t offset) : base(base), offset(offset)
    { }

    Address() { mozilla::PodZero(this); }
};

// Specifies an address computed in the form of a register base, a register
// index with a scale, and a constant, 32-bit offset.
struct BaseIndex
{
    Register base;
    Register index;
    Scale scale;
    int32_t offset;

    BaseIndex(Register base, Register index, Scale scale, int32_t offset = 0)
      : base(base), index(index), scale(scale), offset(offset)
    { }

    BaseIndex() { mozilla::PodZero(this); }
};

// A BaseIndex used to access Values.  Note that |offset| is *not* scaled by
// sizeof(Value).  Use this *only* if you're indexing into a series of Values
// that aren't object elements or object slots (for example, values on the
// stack, values in an arguments object, &c.).  If you're indexing into an
// object's elements or slots, don't use this directly!  Use
// BaseObject{Element,Slot}Index instead.
struct BaseValueIndex : BaseIndex
{
    BaseValueIndex(Register base, Register index, int32_t offset = 0)
      : BaseIndex(base, index, ValueScale, offset)
    { }
};

// Specifies the address of an indexed Value within object elements from a
// base.  The index must not already be scaled by sizeof(Value)!
struct BaseObjectElementIndex : BaseValueIndex
{
    BaseObjectElementIndex(Register base, Register index, int32_t offset = 0)
      : BaseValueIndex(base, index, offset)
    {
        NativeObject::elementsSizeMustNotOverflow();
    }
};

// Like BaseObjectElementIndex, except for object slots.
struct BaseObjectSlotIndex : BaseValueIndex
{
    BaseObjectSlotIndex(Register base, Register index)
      : BaseValueIndex(base, index)
    {
        NativeObject::slotsSizeMustNotOverflow();
    }
};

class Relocation {
  public:
    enum Kind {
        // The target is immovable, so patching is only needed if the source
        // buffer is relocated and the reference is relative.
        HARDCODED,

        // The target is the start of a JitCode buffer, which must be traced
        // during garbage collection. Relocations and patching may be needed.
        JITCODE
    };
};

class RepatchLabel
{
    static const int32_t INVALID_OFFSET = 0xC0000000;
    int32_t offset_ : 31;
    uint32_t bound_ : 1;
  public:

    RepatchLabel() : offset_(INVALID_OFFSET), bound_(0) {}

    void use(uint32_t newOffset) {
        MOZ_ASSERT(offset_ == INVALID_OFFSET);
        MOZ_ASSERT(newOffset != (uint32_t)INVALID_OFFSET);
        offset_ = newOffset;
    }
    bool bound() const {
        return bound_;
    }
    void bind(int32_t dest) {
        MOZ_ASSERT(!bound_);
        MOZ_ASSERT(dest != INVALID_OFFSET);
        offset_ = dest;
        bound_ = true;
    }
    int32_t target() {
        MOZ_ASSERT(bound());
        int32_t ret = offset_;
        offset_ = INVALID_OFFSET;
        return ret;
    }
    int32_t offset() {
        MOZ_ASSERT(!bound());
        return offset_;
    }
    bool used() const {
        return !bound() && offset_ != (INVALID_OFFSET);
    }

};
// An absolute label is like a Label, except it represents an absolute
// reference rather than a relative one. Thus, it cannot be patched until after
// linking.
struct AbsoluteLabel : public LabelBase
{
  public:
    AbsoluteLabel()
    { }
    AbsoluteLabel(const AbsoluteLabel& label) : LabelBase(label)
    { }
    int32_t prev() const {
        MOZ_ASSERT(!bound());
        if (!used())
            return INVALID_OFFSET;
        return offset();
    }
    void setPrev(int32_t offset) {
        use(offset);
    }
    void bind() {
        bound_ = true;

        // These labels cannot be used after being bound.
        offset_ = -1;
    }
};

// A code label contains an absolute reference to a point in the code
// Thus, it cannot be patched until after linking
class CodeLabel
{
    // The destination position, where the absolute reference should get patched into
    AbsoluteLabel dest_;

    // The source label (relative) in the code to where the
    // the destination should get patched to.
    Label src_;

  public:
    CodeLabel()
    { }
    explicit CodeLabel(const AbsoluteLabel& dest)
       : dest_(dest)
    { }
    AbsoluteLabel* dest() {
        return &dest_;
    }
    Label* src() {
        return &src_;
    }
};

// Location of a jump or label in a generated JitCode block, relative to the
// start of the block.

class CodeOffsetJump
{
    size_t offset_;

#ifdef JS_SMALL_BRANCH
    size_t jumpTableIndex_;
#endif

  public:

#ifdef JS_SMALL_BRANCH
    CodeOffsetJump(size_t offset, size_t jumpTableIndex)
        : offset_(offset), jumpTableIndex_(jumpTableIndex)
    {}
    size_t jumpTableIndex() const {
        return jumpTableIndex_;
    }
#else
    CodeOffsetJump(size_t offset) : offset_(offset) {}
#endif

    CodeOffsetJump() {
        mozilla::PodZero(this);
    }

    size_t offset() const {
        return offset_;
    }
    void fixup(MacroAssembler* masm);
};

class CodeOffsetLabel
{
    size_t offset_;

  public:
    explicit CodeOffsetLabel(size_t offset) : offset_(offset) {}
    CodeOffsetLabel() : offset_(0) {}

    size_t offset() const {
        return offset_;
    }
    void fixup(MacroAssembler* masm);

};

// Absolute location of a jump or a label in some generated JitCode block.
// Can also encode a CodeOffset{Jump,Label}, such that the offset is initially
// set and the absolute location later filled in after the final JitCode is
// allocated.

class CodeLocationJump
{
    uint8_t* raw_;
#ifdef DEBUG
    enum State { Uninitialized, Absolute, Relative };
    State state_;
    void setUninitialized() {
        state_ = Uninitialized;
    }
    void setAbsolute() {
        state_ = Absolute;
    }
    void setRelative() {
        state_ = Relative;
    }
#else
    void setUninitialized() const {
    }
    void setAbsolute() const {
    }
    void setRelative() const {
    }
#endif

#ifdef JS_SMALL_BRANCH
    uint8_t* jumpTableEntry_;
#endif

  public:
    CodeLocationJump() {
        raw_ = nullptr;
        setUninitialized();
#ifdef JS_SMALL_BRANCH
        jumpTableEntry_ = (uint8_t*) 0xdeadab1e;
#endif
    }
    CodeLocationJump(JitCode* code, CodeOffsetJump base) {
        *this = base;
        repoint(code);
    }

    void operator = (CodeOffsetJump base) {
        raw_ = (uint8_t*) base.offset();
        setRelative();
#ifdef JS_SMALL_BRANCH
        jumpTableEntry_ = (uint8_t*) base.jumpTableIndex();
#endif
    }

    void repoint(JitCode* code, MacroAssembler* masm = nullptr);

    uint8_t* raw() const {
        MOZ_ASSERT(state_ == Absolute);
        return raw_;
    }
    uint8_t* offset() const {
        MOZ_ASSERT(state_ == Relative);
        return raw_;
    }

#ifdef JS_SMALL_BRANCH
    uint8_t* jumpTableEntry() const {
        MOZ_ASSERT(state_ == Absolute);
        return jumpTableEntry_;
    }
#endif
};

class CodeLocationLabel
{
    uint8_t* raw_;
#ifdef DEBUG
    enum State { Uninitialized, Absolute, Relative };
    State state_;
    void setUninitialized() {
        state_ = Uninitialized;
    }
    void setAbsolute() {
        state_ = Absolute;
    }
    void setRelative() {
        state_ = Relative;
    }
#else
    void setUninitialized() const {
    }
    void setAbsolute() const {
    }
    void setRelative() const {
    }
#endif

  public:
    CodeLocationLabel() {
        raw_ = nullptr;
        setUninitialized();
    }
    CodeLocationLabel(JitCode* code, CodeOffsetLabel base) {
        *this = base;
        repoint(code);
    }
    explicit CodeLocationLabel(JitCode* code) {
        raw_ = code->raw();
        setAbsolute();
    }
    explicit CodeLocationLabel(uint8_t* raw) {
        raw_ = raw;
        setAbsolute();
    }

    void operator = (CodeOffsetLabel base) {
        raw_ = (uint8_t*)base.offset();
        setRelative();
    }
    ptrdiff_t operator - (const CodeLocationLabel& other) {
        return raw_ - other.raw_;
    }

    void repoint(JitCode* code, MacroAssembler* masm = nullptr);

#ifdef DEBUG
    bool isSet() const {
        return state_ != Uninitialized;
    }
#endif

    uint8_t* raw() const {
        MOZ_ASSERT(state_ == Absolute);
        return raw_;
    }
    uint8_t* offset() const {
        MOZ_ASSERT(state_ == Relative);
        return raw_;
    }
};

// While the frame-pointer chain allows the stack to be unwound without
// metadata, Error.stack still needs to know the line/column of every call in
// the chain. A CallSiteDesc describes the line/column of a single callsite.
// A CallSiteDesc is created by callers of MacroAssembler.
class CallSiteDesc
{
    uint32_t line_;
    uint32_t column_ : 31;
    uint32_t kind_ : 1;
  public:
    enum Kind {
        Relative,  // pc-relative call
        Register   // call *register
    };
    CallSiteDesc() {}
    explicit CallSiteDesc(Kind kind)
      : line_(0), column_(0), kind_(kind)
    {}
    CallSiteDesc(uint32_t line, uint32_t column, Kind kind)
      : line_(line), column_(column), kind_(kind)
    {
        MOZ_ASSERT(column <= INT32_MAX);
    }
    uint32_t line() const { return line_; }
    uint32_t column() const { return column_; }
    Kind kind() const { return Kind(kind_); }
};

// Adds to CallSiteDesc the metadata necessary to walk the stack given an
// initial stack-pointer.
class CallSite : public CallSiteDesc
{
    uint32_t returnAddressOffset_;
    uint32_t stackDepth_;

  public:
    CallSite() {}

    CallSite(CallSiteDesc desc, uint32_t returnAddressOffset, uint32_t stackDepth)
      : CallSiteDesc(desc),
        returnAddressOffset_(returnAddressOffset),
        stackDepth_(stackDepth)
    { }

    void setReturnAddressOffset(uint32_t r) { returnAddressOffset_ = r; }
    uint32_t returnAddressOffset() const { return returnAddressOffset_; }

    // The stackDepth measures the amount of stack space pushed since the
    // function was called. In particular, this includes the pushed return
    // address on all archs (whether or not the call instruction pushes the
    // return address (x86/x64) or the prologue does (ARM/MIPS)).
    uint32_t stackDepth() const { return stackDepth_; }
};

typedef Vector<CallSite, 0, SystemAllocPolicy> CallSiteVector;

// As an invariant across architectures, within asm.js code:
//   $sp % AsmJSStackAlignment = (sizeof(AsmJSFrame) + masm.framePushed) % AsmJSStackAlignment
// Thus, AsmJSFrame represents the bytes pushed after the call (which occurred
// with a AsmJSStackAlignment-aligned StackPointer) that are not included in
// masm.framePushed.
struct AsmJSFrame
{
    // The caller's saved frame pointer. In non-profiling mode, internal
    // asm.js-to-asm.js calls don't update fp and thus don't save the caller's
    // frame pointer; the space is reserved, however, so that profiling mode can
    // reuse the same function body without recompiling.
    uint8_t* callerFP;

    // The return address pushed by the call (in the case of ARM/MIPS the return
    // address is pushed by the first instruction of the prologue).
    void* returnAddress;
};
static_assert(sizeof(AsmJSFrame) == 2 * sizeof(void*), "?!");
static const uint32_t AsmJSFrameBytesAfterReturnAddress = sizeof(void*);

// A hoisting of constants that would otherwise require #including AsmJSModule.h
// everywhere. Values are asserted in AsmJSModule.h.
static const unsigned AsmJSActivationGlobalDataOffset = 0;
static const unsigned AsmJSHeapGlobalDataOffset = sizeof(void*);
static const unsigned AsmJSNaN64GlobalDataOffset = 2 * sizeof(void*);
static const unsigned AsmJSNaN32GlobalDataOffset = 2 * sizeof(void*) + sizeof(double);

// Summarizes a heap access made by asm.js code that needs to be patched later
// and/or looked up by the asm.js signal handlers. Different architectures need
// to know different things (x64: offset and length, ARM: where to patch in
// heap length, x86: where to patch in heap length and base) hence the massive
// #ifdefery.
class AsmJSHeapAccess
{
  private:
    uint32_t offset_;
#if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
    uint8_t cmpDelta_;  // the number of bytes from the cmp to the load/store instruction
    uint8_t opLength_;  // the length of the load/store instruction
    uint8_t numSimdElems_; // the number of SIMD lanes to load/store at once
    Scalar::Type type_;
    AnyRegister::Code loadedReg_ : 8;
#endif

    JS_STATIC_ASSERT(AnyRegister::Total < UINT8_MAX);

  public:
    AsmJSHeapAccess() {}
#if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
    static const uint32_t NoLengthCheck = UINT32_MAX;

    // If 'cmp' equals 'offset' or if it is not supplied then the
    // cmpDelta_ is zero indicating that there is no length to patch.
    AsmJSHeapAccess(uint32_t offset, uint32_t after, Scalar::Type type, AnyRegister loadedReg,
                    uint32_t cmp = NoLengthCheck)
      : offset_(offset),
        cmpDelta_(cmp == NoLengthCheck ? 0 : offset - cmp),
        opLength_(after - offset),
        numSimdElems_(UINT8_MAX),
        type_(type),
        loadedReg_(loadedReg.code())
    {
        MOZ_ASSERT(!Scalar::isSimdType(type));
    }
    AsmJSHeapAccess(uint32_t offset, uint8_t after, Scalar::Type type, uint32_t cmp = NoLengthCheck)
      : offset_(offset),
        cmpDelta_(cmp == NoLengthCheck ? 0 : offset - cmp),
        opLength_(after - offset),
        numSimdElems_(UINT8_MAX),
        type_(type),
        loadedReg_(UINT8_MAX)
    {
        MOZ_ASSERT(!Scalar::isSimdType(type));
    }
    // SIMD loads / stores
    AsmJSHeapAccess(uint32_t offset, uint32_t after, unsigned numSimdElems, Scalar::Type type,
                    uint32_t cmp = NoLengthCheck)
      : offset_(offset),
        cmpDelta_(cmp == NoLengthCheck ? 0 : offset - cmp),
        opLength_(after - offset),
        numSimdElems_(numSimdElems),
        type_(type),
        loadedReg_(UINT8_MAX)
    {
        MOZ_ASSERT(Scalar::isSimdType(type));
    }
#elif defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_MIPS)
    explicit AsmJSHeapAccess(uint32_t offset)
      : offset_(offset)
    {}
#endif

    uint32_t offset() const { return offset_; }
    void setOffset(uint32_t offset) { offset_ = offset; }
#if defined(JS_CODEGEN_X86)
    void* patchOffsetAt(uint8_t* code) const { return code + (offset_ + opLength_); }
#endif
#if defined(JS_CODEGEN_X64)
    unsigned opLength() const { MOZ_ASSERT(!Scalar::isSimdType(type_)); return opLength_; }
    bool isLoad() const { MOZ_ASSERT(!Scalar::isSimdType(type_)); return loadedReg_ != UINT8_MAX; }
#endif
#if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
    bool hasLengthCheck() const { return cmpDelta_ > 0; }
    void* patchLengthAt(uint8_t* code) const { return code + (offset_ - cmpDelta_); }
    unsigned numSimdElems() const { MOZ_ASSERT(Scalar::isSimdType(type_)); return numSimdElems_; }
    Scalar::Type type() const { return type_; }
    AnyRegister loadedReg() const { return AnyRegister::FromCode(loadedReg_); }
#endif
};

typedef Vector<AsmJSHeapAccess, 0, SystemAllocPolicy> AsmJSHeapAccessVector;

struct AsmJSGlobalAccess
{
    CodeOffsetLabel patchAt;
    unsigned globalDataOffset;

    AsmJSGlobalAccess(CodeOffsetLabel patchAt, unsigned globalDataOffset)
      : patchAt(patchAt), globalDataOffset(globalDataOffset)
    {}
};

// Describes the intended pointee of an immediate to be embedded in asm.js
// code. By representing the pointee as a symbolic enum, the pointee can be
// patched after deserialization when the address of global things has changed.
enum AsmJSImmKind
{
    AsmJSImm_ToInt32         = AsmJSExit::Builtin_ToInt32,
#if defined(JS_CODEGEN_ARM)
    AsmJSImm_aeabi_idivmod   = AsmJSExit::Builtin_IDivMod,
    AsmJSImm_aeabi_uidivmod  = AsmJSExit::Builtin_UDivMod,
#endif
    AsmJSImm_ModD            = AsmJSExit::Builtin_ModD,
    AsmJSImm_SinD            = AsmJSExit::Builtin_SinD,
    AsmJSImm_CosD            = AsmJSExit::Builtin_CosD,
    AsmJSImm_TanD            = AsmJSExit::Builtin_TanD,
    AsmJSImm_ASinD           = AsmJSExit::Builtin_ASinD,
    AsmJSImm_ACosD           = AsmJSExit::Builtin_ACosD,
    AsmJSImm_ATanD           = AsmJSExit::Builtin_ATanD,
    AsmJSImm_CeilD           = AsmJSExit::Builtin_CeilD,
    AsmJSImm_CeilF           = AsmJSExit::Builtin_CeilF,
    AsmJSImm_FloorD          = AsmJSExit::Builtin_FloorD,
    AsmJSImm_FloorF          = AsmJSExit::Builtin_FloorF,
    AsmJSImm_ExpD            = AsmJSExit::Builtin_ExpD,
    AsmJSImm_LogD            = AsmJSExit::Builtin_LogD,
    AsmJSImm_PowD            = AsmJSExit::Builtin_PowD,
    AsmJSImm_ATan2D          = AsmJSExit::Builtin_ATan2D,
    AsmJSImm_Runtime,
    AsmJSImm_RuntimeInterruptUint32,
    AsmJSImm_StackLimit,
    AsmJSImm_ReportOverRecursed,
    AsmJSImm_OnDetached,
    AsmJSImm_OnOutOfBounds,
    AsmJSImm_HandleExecutionInterrupt,
    AsmJSImm_InvokeFromAsmJS_Ignore,
    AsmJSImm_InvokeFromAsmJS_ToInt32,
    AsmJSImm_InvokeFromAsmJS_ToNumber,
    AsmJSImm_CoerceInPlace_ToInt32,
    AsmJSImm_CoerceInPlace_ToNumber,
    AsmJSImm_Limit
};

static inline AsmJSImmKind
BuiltinToImmKind(AsmJSExit::BuiltinKind builtin)
{
    return AsmJSImmKind(builtin);
}

static inline bool
ImmKindIsBuiltin(AsmJSImmKind imm, AsmJSExit::BuiltinKind* builtin)
{
    if (unsigned(imm) >= unsigned(AsmJSExit::Builtin_Limit))
        return false;
    *builtin = AsmJSExit::BuiltinKind(imm);
    return true;
}

// Pointer to be embedded as an immediate in asm.js code.
class AsmJSImmPtr
{
    AsmJSImmKind kind_;
  public:
    AsmJSImmKind kind() const { return kind_; }
    // This needs to be MOZ_IMPLICIT in order to make MacroAssember::CallWithABINoProfiling compile.
    MOZ_IMPLICIT AsmJSImmPtr(AsmJSImmKind kind) : kind_(kind) { MOZ_ASSERT(IsCompilingAsmJS()); }
    AsmJSImmPtr() {}
};

// Pointer to be embedded as an immediate that is loaded/stored from by an
// instruction in asm.js code.
class AsmJSAbsoluteAddress
{
    AsmJSImmKind kind_;
  public:
    AsmJSImmKind kind() const { return kind_; }
    explicit AsmJSAbsoluteAddress(AsmJSImmKind kind) : kind_(kind) { MOZ_ASSERT(IsCompilingAsmJS()); }
    AsmJSAbsoluteAddress() {}
};

// Represents an instruction to be patched and the intended pointee. These
// links are accumulated in the MacroAssembler, but patching is done outside
// the MacroAssembler (in AsmJSModule::staticallyLink).
struct AsmJSAbsoluteLink
{
    AsmJSAbsoluteLink(CodeOffsetLabel patchAt, AsmJSImmKind target)
      : patchAt(patchAt), target(target) {}
    CodeOffsetLabel patchAt;
    AsmJSImmKind target;
};

// The base class of all Assemblers for all archs.
class AssemblerShared
{
    Vector<CallSite, 0, SystemAllocPolicy> callsites_;
    Vector<AsmJSHeapAccess, 0, SystemAllocPolicy> asmJSHeapAccesses_;
    Vector<AsmJSGlobalAccess, 0, SystemAllocPolicy> asmJSGlobalAccesses_;
    Vector<AsmJSAbsoluteLink, 0, SystemAllocPolicy> asmJSAbsoluteLinks_;

  protected:
    Vector<CodeOffsetLabel, 0, SystemAllocPolicy> profilerCallSites_;
    bool enoughMemory_;
    bool embedsNurseryPointers_;

  public:
    AssemblerShared()
     : enoughMemory_(true),
       embedsNurseryPointers_(false)
    {}

    void propagateOOM(bool success) {
        enoughMemory_ &= success;
    }

    void setOOM() {
        enoughMemory_ = false;
    }

    bool oom() const {
        return !enoughMemory_;
    }

    void appendProfilerCallSite(CodeOffsetLabel label) {
        enoughMemory_ &= profilerCallSites_.append(label);
    }

    bool embedsNurseryPointers() const {
        return embedsNurseryPointers_;
    }

    ImmGCPtr noteMaybeNurseryPtr(ImmMaybeNurseryPtr ptr) {
        if (ptr.value && gc::IsInsideNursery(ptr.value)) {
            // noteMaybeNurseryPtr can be reached from off-thread compilation,
            // though not with an actual nursery pointer argument in that case.
            MOZ_ASSERT(GetJitContext()->runtime->onMainThread());
            // Do not be ion-compiling on the main thread.
            MOZ_ASSERT(!GetJitContext()->runtime->mainThread()->ionCompiling);
            embedsNurseryPointers_ = true;
        }
        return ImmGCPtr(ptr);
    }

    void append(const CallSiteDesc& desc, size_t currentOffset, size_t framePushed) {
        // framePushed does not include sizeof(AsmJSFrame), so add it in here (see
        // CallSite::stackDepth).
        CallSite callsite(desc, currentOffset, framePushed + sizeof(AsmJSFrame));
        enoughMemory_ &= callsites_.append(callsite);
    }
    CallSiteVector&& extractCallSites() { return Move(callsites_); }

    void append(AsmJSHeapAccess access) { enoughMemory_ &= asmJSHeapAccesses_.append(access); }
    AsmJSHeapAccessVector&& extractAsmJSHeapAccesses() { return Move(asmJSHeapAccesses_); }

    void append(AsmJSGlobalAccess access) { enoughMemory_ &= asmJSGlobalAccesses_.append(access); }
    size_t numAsmJSGlobalAccesses() const { return asmJSGlobalAccesses_.length(); }
    AsmJSGlobalAccess asmJSGlobalAccess(size_t i) const { return asmJSGlobalAccesses_[i]; }

    void append(AsmJSAbsoluteLink link) { enoughMemory_ &= asmJSAbsoluteLinks_.append(link); }
    size_t numAsmJSAbsoluteLinks() const { return asmJSAbsoluteLinks_.length(); }
    AsmJSAbsoluteLink asmJSAbsoluteLink(size_t i) const { return asmJSAbsoluteLinks_[i]; }

    static bool canUseInSingleByteInstruction(Register reg) { return true; }
};

} // namespace jit
} // namespace js

#endif /* jit_shared_Assembler_shared_h */

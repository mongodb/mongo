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

#if defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_ARM64)
// Push return addresses callee-side.
# define JS_USE_LINK_REGISTER
#endif

#if defined(JS_CODEGEN_X64) || defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_ARM64)
// JS_SMALL_BRANCH means the range on a branch instruction
// is smaller than the whole address space
# define JS_SMALL_BRANCH
#endif

namespace js {
namespace jit {

namespace Disassembler {
class HeapAccess;
} // namespace Disassembler

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

// Used for 64-bit immediates which do not require relocation.
struct Imm64
{
    uint64_t value;

    explicit Imm64(uint64_t value) : value(value)
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
#endif

// Pointer to be embedded as an immediate in an instruction.
struct ImmPtr
{
    void* value;

    explicit ImmPtr(const void* value) : value(const_cast<void*>(value))
    {
        // To make code serialization-safe, wasm compilation should only
        // compile pointer immediates using a SymbolicAddress.
        MOZ_ASSERT(!IsCompilingAsmJS());
    }

    template <class R>
    explicit ImmPtr(R (*pf)())
      : value(JS_FUNC_TO_DATA_PTR(void*, pf))
    {
        MOZ_ASSERT(!IsCompilingAsmJS());
    }

    template <class R, class A1>
    explicit ImmPtr(R (*pf)(A1))
      : value(JS_FUNC_TO_DATA_PTR(void*, pf))
    {
        MOZ_ASSERT(!IsCompilingAsmJS());
    }

    template <class R, class A1, class A2>
    explicit ImmPtr(R (*pf)(A1, A2))
      : value(JS_FUNC_TO_DATA_PTR(void*, pf))
    {
        MOZ_ASSERT(!IsCompilingAsmJS());
    }

    template <class R, class A1, class A2, class A3>
    explicit ImmPtr(R (*pf)(A1, A2, A3))
      : value(JS_FUNC_TO_DATA_PTR(void*, pf))
    {
        MOZ_ASSERT(!IsCompilingAsmJS());
    }

    template <class R, class A1, class A2, class A3, class A4>
    explicit ImmPtr(R (*pf)(A1, A2, A3, A4))
      : value(JS_FUNC_TO_DATA_PTR(void*, pf))
    {
        MOZ_ASSERT(!IsCompilingAsmJS());
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

// Used for immediates which require relocation.
class ImmGCPtr
{
  public:
    const gc::Cell* value;

    explicit ImmGCPtr(const gc::Cell* ptr) : value(ptr)
    {
        // Nursery pointers can't be used if the main thread might be currently
        // performing a minor GC.
        MOZ_ASSERT_IF(ptr && !ptr->isTenured(),
                      !CurrentThreadIsIonCompilingSafeForMinorGC());

        // asm.js shouldn't be creating GC things
        MOZ_ASSERT(!IsCompilingAsmJS());
    }

  private:
    ImmGCPtr() : value(0) {}
};

// Pointer to be embedded as an immediate that is loaded/stored from by an
// instruction.
struct AbsoluteAddress
{
    void* addr;

    explicit AbsoluteAddress(const void* addr)
      : addr(const_cast<void*>(addr))
    {
        MOZ_ASSERT(!IsCompilingAsmJS());
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
    explicit PatchedAbsoluteAddress(uintptr_t addr)
      : addr(reinterpret_cast<void*>(addr))
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

class CodeOffset
{
    size_t offset_;

    static const size_t NOT_BOUND = size_t(-1);

  public:
    explicit CodeOffset(size_t offset) : offset_(offset) {}
    CodeOffset() : offset_(NOT_BOUND) {}

    size_t offset() const {
        MOZ_ASSERT(bound());
        return offset_;
    }

    void bind(size_t offset) {
        MOZ_ASSERT(!bound());
        offset_ = offset;
        MOZ_ASSERT(bound());
    }
    bool bound() const {
        return offset_ != NOT_BOUND;
    }

    void offsetBy(size_t delta) {
        MOZ_ASSERT(bound());
        MOZ_ASSERT(offset_ + delta >= offset_, "no overflow");
        offset_ += delta;
    }
};

// A code label contains an absolute reference to a point in the code. Thus, it
// cannot be patched until after linking.
// When the source label is resolved into a memory address, this address is
// patched into the destination address.
class CodeLabel
{
    // The destination position, where the absolute reference should get
    // patched into.
    CodeOffset patchAt_;

    // The source label (relative) in the code to where the destination should
    // get patched to.
    CodeOffset target_;

  public:
    CodeLabel()
    { }
    explicit CodeLabel(const CodeOffset& patchAt)
      : patchAt_(patchAt)
    { }
    CodeLabel(const CodeOffset& patchAt, const CodeOffset& target)
      : patchAt_(patchAt),
        target_(target)
    { }
    CodeOffset* patchAt() {
        return &patchAt_;
    }
    CodeOffset* target() {
        return &target_;
    }
    void offsetBy(size_t delta) {
        patchAt_.offsetBy(delta);
        target_.offsetBy(delta);
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
    CodeLocationLabel(JitCode* code, CodeOffset base) {
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

    void operator = (CodeOffset base) {
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

struct AsmJSGlobalAccess
{
    CodeOffset patchAt;
    unsigned globalDataOffset;

    AsmJSGlobalAccess(CodeOffset patchAt, unsigned globalDataOffset)
      : patchAt(patchAt), globalDataOffset(globalDataOffset)
    {}
};

// Represents an instruction to be patched and the intended pointee. These
// links are accumulated in the MacroAssembler, but patching is done outside
// the MacroAssembler (in AsmJSModule::staticallyLink).
struct AsmJSAbsoluteLink
{
    AsmJSAbsoluteLink(CodeOffset patchAt, wasm::SymbolicAddress target)
      : patchAt(patchAt), target(target) {}

    CodeOffset patchAt;
    wasm::SymbolicAddress target;
};

// Represents a call from an asm.js function to another asm.js function,
// represented by the index of the callee in the Module Validator
struct AsmJSInternalCallee
{
    uint32_t index;

    // Provide a default constructor for embedding it in unions
    AsmJSInternalCallee() = default;

    explicit AsmJSInternalCallee(uint32_t calleeIndex)
      : index(calleeIndex)
    {}
};

// The base class of all Assemblers for all archs.
class AssemblerShared
{
    wasm::CallSiteAndTargetVector callsites_;
    wasm::HeapAccessVector heapAccesses_;
    Vector<AsmJSGlobalAccess, 0, SystemAllocPolicy> asmJSGlobalAccesses_;
    Vector<AsmJSAbsoluteLink, 0, SystemAllocPolicy> asmJSAbsoluteLinks_;

  protected:
    Vector<CodeLabel, 0, SystemAllocPolicy> codeLabels_;

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

    bool embedsNurseryPointers() const {
        return embedsNurseryPointers_;
    }

    void append(const wasm::CallSiteDesc& desc, CodeOffset label, size_t framePushed,
                uint32_t targetIndex = wasm::CallSiteAndTarget::NOT_INTERNAL)
    {
        // framePushed does not include sizeof(AsmJSFrame), so add it in here (see
        // CallSite::stackDepth).
        wasm::CallSite callsite(desc, label.offset(), framePushed + sizeof(AsmJSFrame));
        enoughMemory_ &= callsites_.append(wasm::CallSiteAndTarget(callsite, targetIndex));
    }
    wasm::CallSiteAndTargetVector& callSites() { return callsites_; }

    void append(wasm::HeapAccess access) { enoughMemory_ &= heapAccesses_.append(access); }
    wasm::HeapAccessVector&& extractHeapAccesses() { return Move(heapAccesses_); }

    void append(AsmJSGlobalAccess access) { enoughMemory_ &= asmJSGlobalAccesses_.append(access); }
    size_t numAsmJSGlobalAccesses() const { return asmJSGlobalAccesses_.length(); }
    AsmJSGlobalAccess asmJSGlobalAccess(size_t i) const { return asmJSGlobalAccesses_[i]; }

    void append(AsmJSAbsoluteLink link) { enoughMemory_ &= asmJSAbsoluteLinks_.append(link); }
    size_t numAsmJSAbsoluteLinks() const { return asmJSAbsoluteLinks_.length(); }
    AsmJSAbsoluteLink asmJSAbsoluteLink(size_t i) const { return asmJSAbsoluteLinks_[i]; }

    static bool canUseInSingleByteInstruction(Register reg) { return true; }

    void addCodeLabel(CodeLabel label) {
        propagateOOM(codeLabels_.append(label));
    }
    size_t numCodeLabels() const {
        return codeLabels_.length();
    }
    CodeLabel codeLabel(size_t i) {
        return codeLabels_[i];
    }

    // Merge this assembler with the other one, invalidating it, by shifting all
    // offsets by a delta.
    bool asmMergeWith(size_t delta, const AssemblerShared& other) {
        size_t i = callsites_.length();
        enoughMemory_ &= callsites_.appendAll(other.callsites_);
        for (; i < callsites_.length(); i++)
            callsites_[i].offsetReturnAddressBy(delta);

        i = heapAccesses_.length();
        enoughMemory_ &= heapAccesses_.appendAll(other.heapAccesses_);
        for (; i < heapAccesses_.length(); i++)
            heapAccesses_[i].offsetInsnOffsetBy(delta);

        i = asmJSGlobalAccesses_.length();
        enoughMemory_ &= asmJSGlobalAccesses_.appendAll(other.asmJSGlobalAccesses_);
        for (; i < asmJSGlobalAccesses_.length(); i++)
            asmJSGlobalAccesses_[i].patchAt.offsetBy(delta);

        i = asmJSAbsoluteLinks_.length();
        enoughMemory_ &= asmJSAbsoluteLinks_.appendAll(other.asmJSAbsoluteLinks_);
        for (; i < asmJSAbsoluteLinks_.length(); i++)
            asmJSAbsoluteLinks_[i].patchAt.offsetBy(delta);

        i = codeLabels_.length();
        enoughMemory_ &= codeLabels_.appendAll(other.codeLabels_);
        for (; i < codeLabels_.length(); i++)
            codeLabels_[i].offsetBy(delta);

        return !oom();
    }
};

} // namespace jit
} // namespace js

#endif /* jit_shared_Assembler_shared_h */

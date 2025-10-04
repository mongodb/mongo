/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_shared_Assembler_shared_h
#define jit_shared_Assembler_shared_h

#include "mozilla/CheckedInt.h"
#include "mozilla/DebugOnly.h"

#include <limits.h>
#include <utility>  // std::pair

#include "gc/Barrier.h"
#include "jit/AtomicOp.h"
#include "jit/JitAllocPolicy.h"
#include "jit/JitCode.h"
#include "jit/JitContext.h"
#include "jit/Label.h"
#include "jit/Registers.h"
#include "jit/RegisterSets.h"
#include "js/ScalarType.h"  // js::Scalar::Type
#include "vm/HelperThreads.h"
#include "wasm/WasmCodegenTypes.h"
#include "wasm/WasmConstants.h"

#if defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_ARM64) ||      \
    defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64) ||  \
    defined(JS_CODEGEN_LOONG64) || defined(JS_CODEGEN_WASM32) || \
    defined(JS_CODEGEN_RISCV64)
// Push return addresses callee-side.
#  define JS_USE_LINK_REGISTER
#endif

#if defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64) || \
    defined(JS_CODEGEN_ARM64) || defined(JS_CODEGEN_LOONG64) || \
    defined(JS_CODEGEN_RISCV64)
// JS_CODELABEL_LINKMODE gives labels additional metadata
// describing how Bind() should patch them.
#  define JS_CODELABEL_LINKMODE
#endif

using js::wasm::FaultingCodeOffset;

namespace js {
namespace jit {

enum class FrameType;
enum class ExceptionResumeKind : int32_t;

namespace Disassembler {
class HeapAccess;
}  // namespace Disassembler

static constexpr uint32_t Simd128DataSize = 4 * sizeof(int32_t);
static_assert(Simd128DataSize == 4 * sizeof(int32_t),
              "SIMD data should be able to contain int32x4");
static_assert(Simd128DataSize == 4 * sizeof(float),
              "SIMD data should be able to contain float32x4");
static_assert(Simd128DataSize == 2 * sizeof(double),
              "SIMD data should be able to contain float64x2");

enum Scale {
  TimesOne = 0,
  TimesTwo = 1,
  TimesFour = 2,
  TimesEight = 3,
  Invalid = -1
};

static_assert(sizeof(JS::Value) == 8,
              "required for TimesEight and 3 below to be correct");
static const Scale ValueScale = TimesEight;
static const size_t ValueShift = 3;

static inline unsigned ScaleToShift(Scale scale) { return unsigned(scale); }

static inline bool IsShiftInScaleRange(int i) {
  return i >= TimesOne && i <= TimesEight;
}

static inline Scale ShiftToScale(int i) {
  MOZ_ASSERT(IsShiftInScaleRange(i));
  return Scale(i);
}

static inline Scale ScaleFromElemWidth(int shift) {
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

static inline Scale ScaleFromScalarType(Scalar::Type type) {
  return ScaleFromElemWidth(Scalar::byteSize(type));
}

#ifdef JS_JITSPEW
static inline const char* StringFromScale(Scale scale) {
  switch (scale) {
    case TimesOne:
      return "TimesOne";
    case TimesTwo:
      return "TimesTwo";
    case TimesFour:
      return "TimesFour";
    case TimesEight:
      return "TimesEight";
    default:
      break;
  }
  MOZ_CRASH("Unknown Scale");
}
#endif

// Used for 32-bit immediates which do not require relocation.
struct Imm32 {
  int32_t value;

  explicit Imm32(int32_t value) : value(value) {}
  explicit Imm32(FrameType type) : Imm32(int32_t(type)) {}
  explicit Imm32(ExceptionResumeKind kind) : Imm32(int32_t(kind)) {}

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
      default:
        MOZ_CRASH("Invalid scale");
    };
  }

  static inline Imm32 FactorOf(enum Scale s) {
    return Imm32(1 << ShiftOf(s).value);
  }
};

// Pointer-sized integer to be embedded as an immediate in an instruction.
struct ImmWord {
  uintptr_t value;

  explicit ImmWord(uintptr_t value) : value(value) {}
};

// Used for 64-bit immediates which do not require relocation.
struct Imm64 {
  uint64_t value;

  explicit Imm64(int64_t value) : value(value) {}

  Imm32 low() const { return Imm32(int32_t(value)); }

  Imm32 hi() const { return Imm32(int32_t(value >> 32)); }

  inline Imm32 firstHalf() const;
  inline Imm32 secondHalf() const;
};

#ifdef DEBUG
static inline bool IsCompilingWasm() {
  return GetJitContext()->isCompilingWasm();
}
#endif

// Pointer to be embedded as an immediate in an instruction.
struct ImmPtr {
  void* value;

  struct NoCheckToken {};

  explicit constexpr ImmPtr(std::nullptr_t) : value(nullptr) {
    // Explicit constructor for nullptr. This ensures ImmPtr(0) can't be called.
    // Either use ImmPtr(nullptr) or ImmWord(0).
  }

  explicit ImmPtr(void* value, NoCheckToken) : value(value) {
    // A special unchecked variant for contexts where we know it is safe to
    // use an immptr. This is assuming the caller knows what they're doing.
  }

  explicit ImmPtr(const void* value) : value(const_cast<void*>(value)) {
    // To make code serialization-safe, wasm compilation should only
    // compile pointer immediates using a SymbolicAddress.
    MOZ_ASSERT(!IsCompilingWasm());
  }

  template <class R>
  explicit ImmPtr(R (*pf)()) : value(JS_FUNC_TO_DATA_PTR(void*, pf)) {
    MOZ_ASSERT(!IsCompilingWasm());
  }

  template <class R, class A1>
  explicit ImmPtr(R (*pf)(A1)) : value(JS_FUNC_TO_DATA_PTR(void*, pf)) {
    MOZ_ASSERT(!IsCompilingWasm());
  }

  template <class R, class A1, class A2>
  explicit ImmPtr(R (*pf)(A1, A2)) : value(JS_FUNC_TO_DATA_PTR(void*, pf)) {
    MOZ_ASSERT(!IsCompilingWasm());
  }

  template <class R, class A1, class A2, class A3>
  explicit ImmPtr(R (*pf)(A1, A2, A3)) : value(JS_FUNC_TO_DATA_PTR(void*, pf)) {
    MOZ_ASSERT(!IsCompilingWasm());
  }

  template <class R, class A1, class A2, class A3, class A4>
  explicit ImmPtr(R (*pf)(A1, A2, A3, A4))
      : value(JS_FUNC_TO_DATA_PTR(void*, pf)) {
    MOZ_ASSERT(!IsCompilingWasm());
  }
};

// The same as ImmPtr except that the intention is to patch this
// instruction. The initial value of the immediate is 'addr' and this value is
// either clobbered or used in the patching process.
struct PatchedImmPtr {
  void* value;

  explicit PatchedImmPtr() : value(nullptr) {}
  explicit PatchedImmPtr(const void* value) : value(const_cast<void*>(value)) {}
};

class AssemblerShared;
class ImmGCPtr;

// Used for immediates which require relocation.
class ImmGCPtr {
 public:
  const gc::Cell* value;

  explicit ImmGCPtr(const gc::Cell* ptr) : value(ptr) {
    // Nursery pointers can't be used if the main thread might be currently
    // performing a minor GC.
    MOZ_ASSERT_IF(ptr && !ptr->isTenured(), !CurrentThreadIsIonCompiling());

    // wasm shouldn't be creating GC things
    MOZ_ASSERT(!IsCompilingWasm());
  }

 private:
  ImmGCPtr() : value(0) {}
};

// Pointer to trampoline code. Trampoline code is kept alive until the runtime
// is destroyed, so does not need to be traced.
struct TrampolinePtr {
  uint8_t* value;

  TrampolinePtr() : value(nullptr) {}
  explicit TrampolinePtr(uint8_t* value) : value(value) { MOZ_ASSERT(value); }
};

// Pointer to be embedded as an immediate that is loaded/stored from by an
// instruction.
struct AbsoluteAddress {
  void* addr;

  explicit AbsoluteAddress(const void* addr) : addr(const_cast<void*>(addr)) {
    MOZ_ASSERT(!IsCompilingWasm());
  }

  AbsoluteAddress offset(ptrdiff_t delta) {
    return AbsoluteAddress(((uint8_t*)addr) + delta);
  }
};

// The same as AbsoluteAddress except that the intention is to patch this
// instruction. The initial value of the immediate is 'addr' and this value is
// either clobbered or used in the patching process.
struct PatchedAbsoluteAddress {
  void* addr;

  explicit PatchedAbsoluteAddress() : addr(nullptr) {}
  explicit PatchedAbsoluteAddress(const void* addr)
      : addr(const_cast<void*>(addr)) {}
  explicit PatchedAbsoluteAddress(uintptr_t addr)
      : addr(reinterpret_cast<void*>(addr)) {}
};

// Specifies an address computed in the form of a register base and a constant,
// 32-bit offset.
struct Address {
  RegisterOrSP base;
  int32_t offset;

  Address(Register base, int32_t offset)
      : base(RegisterOrSP(base)), offset(offset) {}

#ifdef JS_HAS_HIDDEN_SP
  Address(RegisterOrSP base, int32_t offset) : base(base), offset(offset) {}
#endif

  Address() = delete;
};

#if JS_BITS_PER_WORD == 32

static inline Address LowWord(const Address& address) {
  using mozilla::CheckedInt;

  CheckedInt<int32_t> offset =
      CheckedInt<int32_t>(address.offset) + INT64LOW_OFFSET;
  MOZ_ALWAYS_TRUE(offset.isValid());
  return Address(address.base, offset.value());
}

static inline Address HighWord(const Address& address) {
  using mozilla::CheckedInt;

  CheckedInt<int32_t> offset =
      CheckedInt<int32_t>(address.offset) + INT64HIGH_OFFSET;
  MOZ_ALWAYS_TRUE(offset.isValid());
  return Address(address.base, offset.value());
}

#endif

// Specifies an address computed in the form of a register base, a register
// index with a scale, and a constant, 32-bit offset.
struct BaseIndex {
  RegisterOrSP base;
  Register index;
  Scale scale;
  int32_t offset;

  BaseIndex(Register base, Register index, Scale scale, int32_t offset = 0)
      : base(RegisterOrSP(base)), index(index), scale(scale), offset(offset) {}

#ifdef JS_HAS_HIDDEN_SP
  BaseIndex(RegisterOrSP base, Register index, Scale scale, int32_t offset = 0)
      : base(base), index(index), scale(scale), offset(offset) {}
#endif

  BaseIndex() = delete;
};

#if JS_BITS_PER_WORD == 32

static inline BaseIndex LowWord(const BaseIndex& address) {
  using mozilla::CheckedInt;

  CheckedInt<int32_t> offset =
      CheckedInt<int32_t>(address.offset) + INT64LOW_OFFSET;
  MOZ_ALWAYS_TRUE(offset.isValid());
  return BaseIndex(address.base, address.index, address.scale, offset.value());
}

static inline BaseIndex HighWord(const BaseIndex& address) {
  using mozilla::CheckedInt;

  CheckedInt<int32_t> offset =
      CheckedInt<int32_t>(address.offset) + INT64HIGH_OFFSET;
  MOZ_ALWAYS_TRUE(offset.isValid());
  return BaseIndex(address.base, address.index, address.scale, offset.value());
}

#endif

// A BaseIndex used to access Values.  Note that |offset| is *not* scaled by
// sizeof(Value).  Use this *only* if you're indexing into a series of Values
// that aren't object elements or object slots (for example, values on the
// stack, values in an arguments object, &c.).  If you're indexing into an
// object's elements or slots, don't use this directly!  Use
// BaseObject{Element,Slot}Index instead.
struct BaseValueIndex : BaseIndex {
  BaseValueIndex(Register base, Register index, int32_t offset = 0)
      : BaseIndex(RegisterOrSP(base), index, ValueScale, offset) {}

#ifdef JS_HAS_HIDDEN_SP
  BaseValueIndex(RegisterOrSP base, Register index, int32_t offset = 0)
      : BaseIndex(base, index, ValueScale, offset) {}
#endif
};

// Specifies the address of an indexed Value within object elements from a
// base.  The index must not already be scaled by sizeof(Value)!
struct BaseObjectElementIndex : BaseValueIndex {
  BaseObjectElementIndex(Register base, Register index, int32_t offset = 0)
      : BaseValueIndex(base, index, offset) {}

#ifdef JS_HAS_HIDDEN_SP
  BaseObjectElementIndex(RegisterOrSP base, Register index, int32_t offset = 0)
      : BaseValueIndex(base, index, offset) {}
#endif

  static void staticAssertions();
};

// Like BaseObjectElementIndex, except for object slots.
struct BaseObjectSlotIndex : BaseValueIndex {
  BaseObjectSlotIndex(Register base, Register index)
      : BaseValueIndex(base, index) {}

#ifdef JS_HAS_HIDDEN_SP
  BaseObjectSlotIndex(RegisterOrSP base, Register index)
      : BaseValueIndex(base, index) {}
#endif

  static void staticAssertions();
};

enum class RelocationKind {
  // The target is immovable, so patching is only needed if the source
  // buffer is relocated and the reference is relative.
  HARDCODED,

  // The target is the start of a JitCode buffer, which must be traced
  // during garbage collection. Relocations and patching may be needed.
  JITCODE
};

class CodeOffset {
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
  bool bound() const { return offset_ != NOT_BOUND; }

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
// Some need to distinguish between multiple ways of patching that address.
// See JS_CODELABEL_LINKMODE.
class CodeLabel {
  // The destination position, where the absolute reference should get
  // patched into.
  CodeOffset patchAt_;

  // The source label (relative) in the code to where the destination should
  // get patched to.
  CodeOffset target_;

#ifdef JS_CODELABEL_LINKMODE
 public:
  enum LinkMode { Uninitialized = 0, RawPointer, MoveImmediate, JumpImmediate };

 private:
  LinkMode linkMode_ = Uninitialized;
#endif

 public:
  CodeLabel() = default;
  explicit CodeLabel(const CodeOffset& patchAt) : patchAt_(patchAt) {}
  CodeLabel(const CodeOffset& patchAt, const CodeOffset& target)
      : patchAt_(patchAt), target_(target) {}
  CodeOffset* patchAt() { return &patchAt_; }
  CodeOffset* target() { return &target_; }
  CodeOffset patchAt() const { return patchAt_; }
  CodeOffset target() const { return target_; }
#ifdef JS_CODELABEL_LINKMODE
  LinkMode linkMode() const { return linkMode_; }
  void setLinkMode(LinkMode value) { linkMode_ = value; }
#endif
};

typedef Vector<CodeLabel, 0, SystemAllocPolicy> CodeLabelVector;

class CodeLocationLabel {
  uint8_t* raw_ = nullptr;

 public:
  CodeLocationLabel(JitCode* code, CodeOffset base) {
    MOZ_ASSERT(base.offset() < code->instructionsSize());
    raw_ = code->raw() + base.offset();
  }
  explicit CodeLocationLabel(JitCode* code) { raw_ = code->raw(); }
  explicit CodeLocationLabel(uint8_t* raw) {
    MOZ_ASSERT(raw);
    raw_ = raw;
  }

  ptrdiff_t operator-(const CodeLocationLabel& other) const {
    return raw_ - other.raw_;
  }

  uint8_t* raw() const { return raw_; }
};

}  // namespace jit

namespace wasm {

// Represents an instruction to be patched and the intended pointee. These
// links are accumulated in the MacroAssembler, but patching is done outside
// the MacroAssembler (in Module::staticallyLink).

struct SymbolicAccess {
  SymbolicAccess(jit::CodeOffset patchAt, SymbolicAddress target)
      : patchAt(patchAt), target(target) {}

  jit::CodeOffset patchAt;
  SymbolicAddress target;
};

typedef Vector<SymbolicAccess, 0, SystemAllocPolicy> SymbolicAccessVector;

// Describes a single wasm or asm.js memory access for the purpose of generating
// code and metadata.

class MemoryAccessDesc {
  uint32_t memoryIndex_;
  uint64_t offset64_;
  uint32_t align_;
  Scalar::Type type_;
  jit::Synchronization sync_;
  wasm::BytecodeOffset trapOffset_;
  wasm::SimdOp widenOp_;
  enum { Plain, ZeroExtend, Splat, Widen } loadOp_;
  // Used for an assertion in MacroAssembler about offset length
  mozilla::DebugOnly<bool> hugeMemory_;

 public:
  explicit MemoryAccessDesc(
      uint32_t memoryIndex, Scalar::Type type, uint32_t align, uint64_t offset,
      BytecodeOffset trapOffset, mozilla::DebugOnly<bool> hugeMemory,
      jit::Synchronization sync = jit::Synchronization::None())
      : memoryIndex_(memoryIndex),
        offset64_(offset),
        align_(align),
        type_(type),
        sync_(sync),
        trapOffset_(trapOffset),
        widenOp_(wasm::SimdOp::Limit),
        loadOp_(Plain),
        hugeMemory_(hugeMemory) {
    MOZ_ASSERT(mozilla::IsPowerOfTwo(align));
  }

  uint32_t memoryIndex() const {
    MOZ_ASSERT(memoryIndex_ != UINT32_MAX);
    return memoryIndex_;
  }

  // The offset is a 64-bit value because of memory64.  Almost always, it will
  // fit in 32 bits, and hence offset() checks that it will, this method is used
  // almost everywhere in the engine.  The compiler front-ends must use
  // offset64() to bypass the check performed by offset(), and must resolve
  // offsets that don't fit in 32 bits early in the compilation pipeline so that
  // no large offsets are observed later.
  uint32_t offset() const {
    MOZ_ASSERT(offset64_ <= UINT32_MAX);
    return uint32_t(offset64_);
  }
  uint64_t offset64() const { return offset64_; }

  // The offset can be cleared without worrying about its magnitude.
  void clearOffset() { offset64_ = 0; }

  // The offset can be set (after compile-time evaluation) but only to values
  // that fit in 32 bits.
  void setOffset32(uint32_t offset) { offset64_ = offset; }

  uint32_t align() const { return align_; }
  Scalar::Type type() const { return type_; }
  unsigned byteSize() const { return Scalar::byteSize(type()); }
  jit::Synchronization sync() const { return sync_; }
  BytecodeOffset trapOffset() const { return trapOffset_; }
  wasm::SimdOp widenSimdOp() const {
    MOZ_ASSERT(isWidenSimd128Load());
    return widenOp_;
  }
  bool isAtomic() const { return !sync_.isNone(); }
  bool isZeroExtendSimd128Load() const { return loadOp_ == ZeroExtend; }
  bool isSplatSimd128Load() const { return loadOp_ == Splat; }
  bool isWidenSimd128Load() const { return loadOp_ == Widen; }

  mozilla::DebugOnly<bool> isHugeMemory() const { return hugeMemory_; }
#ifdef DEBUG
  void assertOffsetInGuardPages() const;
#else
  void assertOffsetInGuardPages() const {}
#endif

  void setZeroExtendSimd128Load() {
    MOZ_ASSERT(type() == Scalar::Float32 || type() == Scalar::Float64);
    MOZ_ASSERT(!isAtomic());
    MOZ_ASSERT(loadOp_ == Plain);
    loadOp_ = ZeroExtend;
  }

  void setSplatSimd128Load() {
    MOZ_ASSERT(type() == Scalar::Uint8 || type() == Scalar::Uint16 ||
               type() == Scalar::Float32 || type() == Scalar::Float64);
    MOZ_ASSERT(!isAtomic());
    MOZ_ASSERT(loadOp_ == Plain);
    loadOp_ = Splat;
  }

  void setWidenSimd128Load(wasm::SimdOp op) {
    MOZ_ASSERT(type() == Scalar::Float64);
    MOZ_ASSERT(!isAtomic());
    MOZ_ASSERT(loadOp_ == Plain);
    widenOp_ = op;
    loadOp_ = Widen;
  }
};

}  // namespace wasm

namespace jit {

// The base class of all Assemblers for all archs.
class AssemblerShared {
  wasm::CallSiteVector callSites_;
  wasm::CallSiteTargetVector callSiteTargets_;
  wasm::TrapSiteVectorArray trapSites_;
  wasm::SymbolicAccessVector symbolicAccesses_;
  wasm::TryNoteVector tryNotes_;
  wasm::CodeRangeUnwindInfoVector codeRangesUnwind_;

#ifdef DEBUG
  // To facilitate figuring out which part of SM created each instruction as
  // shown by IONFLAGS=codegen, this maintains a stack of (notionally)
  // code-creating routines, which is printed in the log output every time an
  // entry is pushed or popped.  Do not push/pop entries directly; instead use
  // `class AutoCreatedBy`.
  mozilla::Vector<const char*> creators_;
#endif

 protected:
  CodeLabelVector codeLabels_;

  bool enoughMemory_;
  bool embedsNurseryPointers_;

 public:
  AssemblerShared() : enoughMemory_(true), embedsNurseryPointers_(false) {}

  ~AssemblerShared();

#ifdef DEBUG
  // Do not use these directly; instead use `class AutoCreatedBy`.
  void pushCreator(const char*);
  void popCreator();
  // See comment on the implementation of `hasCreator` for guidance on what to
  // do if you get failures of the assertion `MOZ_ASSERT(hasCreator())`,
  bool hasCreator() const;
#endif

  void propagateOOM(bool success) { enoughMemory_ &= success; }

  void setOOM() { enoughMemory_ = false; }

  bool oom() const { return !enoughMemory_; }

  bool embedsNurseryPointers() const { return embedsNurseryPointers_; }

  void addCodeLabel(CodeLabel label) {
    propagateOOM(codeLabels_.append(label));
  }
  size_t numCodeLabels() const { return codeLabels_.length(); }
  CodeLabel codeLabel(size_t i) { return codeLabels_[i]; }
  CodeLabelVector& codeLabels() { return codeLabels_; }

  // WebAssembly metadata emitted by masm operations accumulated on the
  // MacroAssembler, and swapped into a wasm::CompiledCode after finish().

  template <typename... Args>
  void append(const wasm::CallSiteDesc& desc, CodeOffset retAddr,
              Args&&... args) {
    enoughMemory_ &= callSites_.emplaceBack(desc, retAddr.offset());
    enoughMemory_ &= callSiteTargets_.emplaceBack(std::forward<Args>(args)...);
  }
  void append(wasm::Trap trap, wasm::TrapSite site) {
    enoughMemory_ &= trapSites_[trap].append(site);
  }
  void append(const wasm::MemoryAccessDesc& access, wasm::TrapMachineInsn insn,
              FaultingCodeOffset assemblerOffsetOfFaultingMachineInsn) {
    append(wasm::Trap::OutOfBounds,
           wasm::TrapSite(insn, assemblerOffsetOfFaultingMachineInsn,
                          access.trapOffset()));
  }
  void append(wasm::SymbolicAccess access) {
    enoughMemory_ &= symbolicAccesses_.append(access);
  }
  // This one returns an index as the try note so that it can be looked up
  // later to add the end point and stack position of the try block.
  [[nodiscard]] bool append(wasm::TryNote tryNote, size_t* tryNoteIndex) {
    if (!tryNotes_.append(tryNote)) {
      enoughMemory_ = false;
      return false;
    }
    *tryNoteIndex = tryNotes_.length() - 1;
    return true;
  }

  void append(wasm::CodeRangeUnwindInfo::UnwindHow unwindHow,
              uint32_t pcOffset) {
    enoughMemory_ &= codeRangesUnwind_.emplaceBack(pcOffset, unwindHow);
  }

  wasm::CallSiteVector& callSites() { return callSites_; }
  wasm::CallSiteTargetVector& callSiteTargets() { return callSiteTargets_; }
  wasm::TrapSiteVectorArray& trapSites() { return trapSites_; }
  wasm::SymbolicAccessVector& symbolicAccesses() { return symbolicAccesses_; }
  wasm::TryNoteVector& tryNotes() { return tryNotes_; }
  wasm::CodeRangeUnwindInfoVector& codeRangeUnwindInfos() {
    return codeRangesUnwind_;
  }
};

// AutoCreatedBy pushes and later pops a who-created-these-insns? tag into the
// JitSpew_Codegen output.  These could be created fairly frequently, so a
// dummy inlineable-out version is provided for non-debug builds.  The tag
// text can be completely arbitrary -- it serves only to help readers of the
// output text to relate instructions back to the part(s) of SM that created
// them.
#ifdef DEBUG
class MOZ_RAII AutoCreatedBy {
 private:
  AssemblerShared& ash_;

 public:
  AutoCreatedBy(AssemblerShared& ash, const char* who) : ash_(ash) {
    ash_.pushCreator(who);
  }
  ~AutoCreatedBy() { ash_.popCreator(); }
};
#else
class MOZ_RAII AutoCreatedBy {
 public:
  inline AutoCreatedBy(AssemblerShared& ash, const char* who) {}
  // A user-defined constructor is necessary to stop some compilers from
  // complaining about unused variables.
  inline ~AutoCreatedBy() {}
};
#endif

}  // namespace jit
}  // namespace js

#endif /* jit_shared_Assembler_shared_h */

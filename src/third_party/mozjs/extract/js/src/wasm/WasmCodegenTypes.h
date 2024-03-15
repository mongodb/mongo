/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 *
 * Copyright 2021 Mozilla Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef wasm_codegen_types_h
#define wasm_codegen_types_h

#include "mozilla/EnumeratedArray.h"
#include "mozilla/PodOperations.h"

#include <stdint.h>

#include "jit/IonTypes.h"
#include "wasm/WasmBuiltins.h"
#include "wasm/WasmCodegenConstants.h"
#include "wasm/WasmConstants.h"
#include "wasm/WasmInstanceData.h"
#include "wasm/WasmSerialize.h"
#include "wasm/WasmTypeDef.h"
#include "wasm/WasmUtility.h"

namespace js {

namespace jit {
template <class VecT, class ABIArgGeneratorT>
class ABIArgIterBase;
}  // namespace jit

namespace wasm {

using mozilla::EnumeratedArray;

struct ModuleEnvironment;
struct TableDesc;
struct V128;

// ArgTypeVector type.
//
// Functions usually receive one ABI argument per WebAssembly argument.  However
// if a function has multiple results and some of those results go to the stack,
// then it additionally receives a synthetic ABI argument holding a pointer to
// the stack result area.
//
// Given the presence of synthetic arguments, sometimes we need a name for
// non-synthetic arguments.  We call those "natural" arguments.

enum class StackResults { HasStackResults, NoStackResults };

class ArgTypeVector {
  const ValTypeVector& args_;
  bool hasStackResults_;

  // To allow ABIArgIterBase<VecT, ABIArgGeneratorT>, we define a private
  // length() method.  To prevent accidental errors, other users need to be
  // explicit and call lengthWithStackResults() or
  // lengthWithoutStackResults().
  size_t length() const { return args_.length() + size_t(hasStackResults_); }
  template <class VecT, class ABIArgGeneratorT>
  friend class jit::ABIArgIterBase;

 public:
  ArgTypeVector(const ValTypeVector& args, StackResults stackResults)
      : args_(args),
        hasStackResults_(stackResults == StackResults::HasStackResults) {}
  explicit ArgTypeVector(const FuncType& funcType);

  bool hasSyntheticStackResultPointerArg() const { return hasStackResults_; }
  StackResults stackResults() const {
    return hasSyntheticStackResultPointerArg() ? StackResults::HasStackResults
                                               : StackResults::NoStackResults;
  }
  size_t lengthWithoutStackResults() const { return args_.length(); }
  bool isSyntheticStackResultPointerArg(size_t idx) const {
    // The pointer to stack results area, if present, is a synthetic argument
    // tacked on at the end.
    MOZ_ASSERT(idx < lengthWithStackResults());
    return idx == args_.length();
  }
  bool isNaturalArg(size_t idx) const {
    return !isSyntheticStackResultPointerArg(idx);
  }
  size_t naturalIndex(size_t idx) const {
    MOZ_ASSERT(isNaturalArg(idx));
    // Because the synthetic argument, if present, is tacked on the end, an
    // argument index that isn't synthetic is natural.
    return idx;
  }

  size_t lengthWithStackResults() const { return length(); }
  jit::MIRType operator[](size_t i) const {
    MOZ_ASSERT(i < lengthWithStackResults());
    if (isSyntheticStackResultPointerArg(i)) {
      return jit::MIRType::StackResults;
    }
    return args_[naturalIndex(i)].toMIRType();
  }
};

// A wrapper around the bytecode offset of a wasm instruction within a whole
// module, used for trap offsets or call offsets. These offsets should refer to
// the first byte of the instruction that triggered the trap / did the call and
// should ultimately derive from OpIter::bytecodeOffset.

class BytecodeOffset {
  static const uint32_t INVALID = -1;
  uint32_t offset_;

  WASM_CHECK_CACHEABLE_POD(offset_);

 public:
  BytecodeOffset() : offset_(INVALID) {}
  explicit BytecodeOffset(uint32_t offset) : offset_(offset) {}

  bool isValid() const { return offset_ != INVALID; }
  uint32_t offset() const {
    MOZ_ASSERT(isValid());
    return offset_;
  }
};

WASM_DECLARE_CACHEABLE_POD(BytecodeOffset);

// A TrapSite (in the TrapSiteVector for a given Trap code) represents a wasm
// instruction at a given bytecode offset that can fault at the given pc offset.
// When such a fault occurs, a signal/exception handler looks up the TrapSite to
// confirm the fault is intended/safe and redirects pc to the trap stub.

struct TrapSite {
  uint32_t pcOffset;
  BytecodeOffset bytecode;

  WASM_CHECK_CACHEABLE_POD(pcOffset, bytecode);

  TrapSite() : pcOffset(-1), bytecode() {}
  TrapSite(uint32_t pcOffset, BytecodeOffset bytecode)
      : pcOffset(pcOffset), bytecode(bytecode) {}

  void offsetBy(uint32_t offset) { pcOffset += offset; }
};

WASM_DECLARE_CACHEABLE_POD(TrapSite);
WASM_DECLARE_POD_VECTOR(TrapSite, TrapSiteVector)

struct TrapSiteVectorArray
    : EnumeratedArray<Trap, Trap::Limit, TrapSiteVector> {
  bool empty() const;
  void clear();
  void swap(TrapSiteVectorArray& rhs);
  void shrinkStorageToFit();

  size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const;
};

// On trap, the bytecode offset to be reported in callstacks is saved.

struct TrapData {
  // The resumePC indicates where, if the trap doesn't throw, the trap stub
  // should jump to after restoring all register state.
  void* resumePC;

  // The unwoundPC is the PC after adjustment by wasm::StartUnwinding(), which
  // basically unwinds partially-construted wasm::Frames when pc is in the
  // prologue/epilogue. Stack traces during a trap should use this PC since
  // it corresponds to the JitActivation::wasmExitFP.
  void* unwoundPC;

  Trap trap;
  uint32_t bytecodeOffset;
};

// The (,Callable,Func)Offsets classes are used to record the offsets of
// different key points in a CodeRange during compilation.

struct Offsets {
  explicit Offsets(uint32_t begin = 0, uint32_t end = 0)
      : begin(begin), end(end) {}

  // These define a [begin, end) contiguous range of instructions compiled
  // into a CodeRange.
  uint32_t begin;
  uint32_t end;

  WASM_CHECK_CACHEABLE_POD(begin, end);
};

WASM_DECLARE_CACHEABLE_POD(Offsets);

struct CallableOffsets : Offsets {
  MOZ_IMPLICIT CallableOffsets(uint32_t ret = 0) : Offsets(), ret(ret) {}

  // The offset of the return instruction precedes 'end' by a variable number
  // of instructions due to out-of-line codegen.
  uint32_t ret;

  WASM_CHECK_CACHEABLE_POD_WITH_PARENT(Offsets, ret);
};

WASM_DECLARE_CACHEABLE_POD(CallableOffsets);

struct FuncOffsets : CallableOffsets {
  MOZ_IMPLICIT FuncOffsets()
      : CallableOffsets(), uncheckedCallEntry(0), tierEntry(0) {}

  // Function CodeRanges have a checked call entry which takes an extra
  // signature argument which is checked against the callee's signature before
  // falling through to the normal prologue. The checked call entry is thus at
  // the beginning of the CodeRange and the unchecked call entry is at some
  // offset after the checked call entry.
  //
  // Note that there won't always be a checked call entry because not all
  // functions require them. See GenerateFunctionPrologue.
  uint32_t uncheckedCallEntry;

  // The tierEntry is the point within a function to which the patching code
  // within a Tier-1 function jumps.  It could be the instruction following
  // the jump in the Tier-1 function, or the point following the standard
  // prologue within a Tier-2 function.
  uint32_t tierEntry;

  WASM_CHECK_CACHEABLE_POD_WITH_PARENT(CallableOffsets, uncheckedCallEntry,
                                       tierEntry);
};

WASM_DECLARE_CACHEABLE_POD(FuncOffsets);

using FuncOffsetsVector = Vector<FuncOffsets, 0, SystemAllocPolicy>;

// A CodeRange describes a single contiguous range of code within a wasm
// module's code segment. A CodeRange describes what the code does and, for
// function bodies, the name and source coordinates of the function.

class CodeRange {
 public:
  enum Kind {
    Function,          // function definition
    InterpEntry,       // calls into wasm from C++
    JitEntry,          // calls into wasm from jit code
    ImportInterpExit,  // slow-path calling from wasm into C++ interp
    ImportJitExit,     // fast-path calling from wasm into jit code
    BuiltinThunk,      // fast-path calling from wasm into a C++ native
    TrapExit,          // calls C++ to report and jumps to throw stub
    DebugTrap,         // calls C++ to handle debug event
    FarJumpIsland,     // inserted to connect otherwise out-of-range insns
    Throw              // special stack-unwinding stub jumped to by other stubs
  };

 private:
  // All fields are treated as cacheable POD:
  uint32_t begin_;
  uint32_t ret_;
  uint32_t end_;
  union {
    struct {
      uint32_t funcIndex_;
      union {
        struct {
          uint32_t lineOrBytecode_;
          uint16_t beginToUncheckedCallEntry_;
          uint16_t beginToTierEntry_;
        } func;
      };
    };
    Trap trap_;
  } u;
  Kind kind_ : 8;

  WASM_CHECK_CACHEABLE_POD(begin_, ret_, end_, u.funcIndex_,
                           u.func.lineOrBytecode_,
                           u.func.beginToUncheckedCallEntry_,
                           u.func.beginToTierEntry_, u.trap_, kind_);

 public:
  CodeRange() = default;
  CodeRange(Kind kind, Offsets offsets);
  CodeRange(Kind kind, uint32_t funcIndex, Offsets offsets);
  CodeRange(Kind kind, CallableOffsets offsets);
  CodeRange(Kind kind, uint32_t funcIndex, CallableOffsets);
  CodeRange(uint32_t funcIndex, uint32_t lineOrBytecode, FuncOffsets offsets);

  void offsetBy(uint32_t offset) {
    begin_ += offset;
    end_ += offset;
    if (hasReturn()) {
      ret_ += offset;
    }
  }

  // All CodeRanges have a begin and end.

  uint32_t begin() const { return begin_; }
  uint32_t end() const { return end_; }

  // Other fields are only available for certain CodeRange::Kinds.

  Kind kind() const { return kind_; }

  bool isFunction() const { return kind() == Function; }
  bool isImportExit() const {
    return kind() == ImportJitExit || kind() == ImportInterpExit ||
           kind() == BuiltinThunk;
  }
  bool isImportInterpExit() const { return kind() == ImportInterpExit; }
  bool isImportJitExit() const { return kind() == ImportJitExit; }
  bool isTrapExit() const { return kind() == TrapExit; }
  bool isDebugTrap() const { return kind() == DebugTrap; }
  bool isThunk() const { return kind() == FarJumpIsland; }

  // Functions, import exits, trap exits and JitEntry stubs have standard
  // callable prologues and epilogues. Asynchronous frame iteration needs to
  // know the offset of the return instruction to calculate the frame pointer.

  bool hasReturn() const {
    return isFunction() || isImportExit() || isDebugTrap() || isJitEntry();
  }
  uint32_t ret() const {
    MOZ_ASSERT(hasReturn());
    return ret_;
  }

  // Functions, export stubs and import stubs all have an associated function
  // index.

  bool isJitEntry() const { return kind() == JitEntry; }
  bool isInterpEntry() const { return kind() == InterpEntry; }
  bool isEntry() const { return isInterpEntry() || isJitEntry(); }
  bool hasFuncIndex() const {
    return isFunction() || isImportExit() || isEntry();
  }
  uint32_t funcIndex() const {
    MOZ_ASSERT(hasFuncIndex());
    return u.funcIndex_;
  }

  // TrapExit CodeRanges have a Trap field.

  Trap trap() const {
    MOZ_ASSERT(isTrapExit());
    return u.trap_;
  }

  // Function CodeRanges have two entry points: one for normal calls (with a
  // known signature) and one for table calls (which involves dynamic
  // signature checking).

  uint32_t funcCheckedCallEntry() const {
    MOZ_ASSERT(isFunction());
    // not all functions have the checked call prologue;
    // see GenerateFunctionPrologue
    MOZ_ASSERT(u.func.beginToUncheckedCallEntry_ != 0);
    return begin_;
  }
  uint32_t funcUncheckedCallEntry() const {
    MOZ_ASSERT(isFunction());
    return begin_ + u.func.beginToUncheckedCallEntry_;
  }
  uint32_t funcTierEntry() const {
    MOZ_ASSERT(isFunction());
    return begin_ + u.func.beginToTierEntry_;
  }
  uint32_t funcLineOrBytecode() const {
    MOZ_ASSERT(isFunction());
    return u.func.lineOrBytecode_;
  }

  // A sorted array of CodeRanges can be looked up via BinarySearch and
  // OffsetInCode.

  struct OffsetInCode {
    size_t offset;
    explicit OffsetInCode(size_t offset) : offset(offset) {}
    bool operator==(const CodeRange& rhs) const {
      return offset >= rhs.begin() && offset < rhs.end();
    }
    bool operator<(const CodeRange& rhs) const { return offset < rhs.begin(); }
  };
};

WASM_DECLARE_CACHEABLE_POD(CodeRange);
WASM_DECLARE_POD_VECTOR(CodeRange, CodeRangeVector)

extern const CodeRange* LookupInSorted(const CodeRangeVector& codeRanges,
                                       CodeRange::OffsetInCode target);

// While the frame-pointer chain allows the stack to be unwound without
// metadata, Error.stack still needs to know the line/column of every call in
// the chain. A CallSiteDesc describes a single callsite to which CallSite adds
// the metadata necessary to walk up to the next frame. Lastly CallSiteAndTarget
// adds the function index of the callee.

class CallSiteDesc {
  static constexpr size_t LINE_OR_BYTECODE_BITS_SIZE = 28;
  uint32_t lineOrBytecode_ : LINE_OR_BYTECODE_BITS_SIZE;
  uint32_t kind_ : 4;

  WASM_CHECK_CACHEABLE_POD(lineOrBytecode_, kind_);

 public:
  static constexpr uint32_t MAX_LINE_OR_BYTECODE_VALUE =
      (1 << LINE_OR_BYTECODE_BITS_SIZE) - 1;

  enum Kind {
    Func,          // pc-relative call to a specific function
    Import,        // wasm import call
    Indirect,      // dynamic callee called via register, context on stack
    IndirectFast,  // dynamically determined to be same-instance
    FuncRef,       // call using direct function reference
    FuncRefFast,   // call using direct function reference within same-instance
    Symbolic,      // call to a single symbolic callee
    EnterFrame,    // call to a enter frame handler
    LeaveFrame,    // call to a leave frame handler
    Breakpoint     // call to instruction breakpoint
  };
  CallSiteDesc() : lineOrBytecode_(0), kind_(0) {}
  explicit CallSiteDesc(Kind kind) : lineOrBytecode_(0), kind_(kind) {
    MOZ_ASSERT(kind == Kind(kind_));
  }
  CallSiteDesc(uint32_t lineOrBytecode, Kind kind)
      : lineOrBytecode_(lineOrBytecode), kind_(kind) {
    MOZ_ASSERT(kind == Kind(kind_));
    MOZ_ASSERT(lineOrBytecode == lineOrBytecode_);
  }
  CallSiteDesc(BytecodeOffset bytecodeOffset, Kind kind)
      : lineOrBytecode_(bytecodeOffset.offset()), kind_(kind) {
    MOZ_ASSERT(kind == Kind(kind_));
    MOZ_ASSERT(bytecodeOffset.offset() == lineOrBytecode_);
  }
  uint32_t lineOrBytecode() const { return lineOrBytecode_; }
  Kind kind() const { return Kind(kind_); }
  bool isImportCall() const { return kind() == CallSiteDesc::Import; }
  bool isIndirectCall() const { return kind() == CallSiteDesc::Indirect; }
  bool isFuncRefCall() const { return kind() == CallSiteDesc::FuncRef; }
  bool mightBeCrossInstance() const {
    return isImportCall() || isIndirectCall() || isFuncRefCall();
  }
};

static_assert(js::wasm::MaxFunctionBytes <=
              CallSiteDesc::MAX_LINE_OR_BYTECODE_VALUE);

WASM_DECLARE_CACHEABLE_POD(CallSiteDesc);

class CallSite : public CallSiteDesc {
  uint32_t returnAddressOffset_;

  WASM_CHECK_CACHEABLE_POD_WITH_PARENT(CallSiteDesc, returnAddressOffset_);

 public:
  CallSite() : returnAddressOffset_(0) {}

  CallSite(CallSiteDesc desc, uint32_t returnAddressOffset)
      : CallSiteDesc(desc), returnAddressOffset_(returnAddressOffset) {}

  void offsetBy(uint32_t delta) { returnAddressOffset_ += delta; }
  uint32_t returnAddressOffset() const { return returnAddressOffset_; }
};

WASM_DECLARE_CACHEABLE_POD(CallSite);
WASM_DECLARE_POD_VECTOR(CallSite, CallSiteVector)

// A CallSiteTarget describes the callee of a CallSite, either a function or a
// trap exit. Although checked in debug builds, a CallSiteTarget doesn't
// officially know whether it targets a function or trap, relying on the Kind of
// the CallSite to discriminate.

class CallSiteTarget {
  uint32_t packed_;

  WASM_CHECK_CACHEABLE_POD(packed_);
#ifdef DEBUG
  enum Kind { None, FuncIndex, TrapExit } kind_;
  WASM_CHECK_CACHEABLE_POD(kind_);
#endif

 public:
  explicit CallSiteTarget()
      : packed_(UINT32_MAX)
#ifdef DEBUG
        ,
        kind_(None)
#endif
  {
  }

  explicit CallSiteTarget(uint32_t funcIndex)
      : packed_(funcIndex)
#ifdef DEBUG
        ,
        kind_(FuncIndex)
#endif
  {
  }

  explicit CallSiteTarget(Trap trap)
      : packed_(uint32_t(trap))
#ifdef DEBUG
        ,
        kind_(TrapExit)
#endif
  {
  }

  uint32_t funcIndex() const {
    MOZ_ASSERT(kind_ == FuncIndex);
    return packed_;
  }

  Trap trap() const {
    MOZ_ASSERT(kind_ == TrapExit);
    MOZ_ASSERT(packed_ < uint32_t(Trap::Limit));
    return Trap(packed_);
  }
};

WASM_DECLARE_CACHEABLE_POD(CallSiteTarget);

using CallSiteTargetVector = Vector<CallSiteTarget, 0, SystemAllocPolicy>;

// TryNotes are stored in a vector that acts as an exception table for
// wasm try-catch blocks. These represent the information needed to take
// exception handling actions after a throw is executed.
struct TryNote {
 private:
  // Sentinel value to detect a try note that has not been given a try body.
  static const uint32_t BEGIN_NONE = UINT32_MAX;

  // Begin code offset of the try body.
  uint32_t begin_;
  // Exclusive end code offset of the try body.
  uint32_t end_;
  // The code offset of the landing pad.
  uint32_t entryPoint_;
  // Track offset from frame of stack pointer.
  uint32_t framePushed_;

  WASM_CHECK_CACHEABLE_POD(begin_, end_, entryPoint_, framePushed_);

 public:
  explicit TryNote()
      : begin_(BEGIN_NONE), end_(0), entryPoint_(0), framePushed_(0) {}

  // Returns whether a try note has been assigned a range for the try body.
  bool hasTryBody() const { return begin_ != BEGIN_NONE; }

  // The code offset of the beginning of the try body.
  uint32_t tryBodyBegin() const { return begin_; }

  // The code offset of the exclusive end of the try body.
  uint32_t tryBodyEnd() const { return end_; }

  // Returns whether an offset is within this try note's body.
  bool offsetWithinTryBody(uint32_t offset) const {
    return offset > begin_ && offset <= end_;
  }

  // The code offset of the entry to the landing pad.
  uint32_t landingPadEntryPoint() const { return entryPoint_; }

  // The stack frame pushed amount at the entry to the landing pad.
  uint32_t landingPadFramePushed() const { return framePushed_; }

  // Set the beginning of the try body.
  void setTryBodyBegin(uint32_t begin) {
    // There must not be a begin to the try body yet
    MOZ_ASSERT(begin_ == BEGIN_NONE);
    begin_ = begin;
  }

  // Set the end of the try body.
  void setTryBodyEnd(uint32_t end) {
    // There must be a begin to the try body
    MOZ_ASSERT(begin_ != BEGIN_NONE);
    end_ = end;
    // We do not allow empty try bodies
    MOZ_ASSERT(end_ > begin_);
  }

  // Set the entry point and frame pushed of the landing pad.
  void setLandingPad(uint32_t entryPoint, uint32_t framePushed) {
    entryPoint_ = entryPoint;
    framePushed_ = framePushed;
  }

  // Adjust all code offsets in this try note by a delta.
  void offsetBy(uint32_t offset) {
    begin_ += offset;
    end_ += offset;
    entryPoint_ += offset;
  }

  bool operator<(const TryNote& other) const {
    // Special case comparison with self. This avoids triggering the assertion
    // about non-intersection below. This case can arise in std::sort.
    if (this == &other) {
      return false;
    }
    // Try notes must be properly nested without touching at begin and end
    MOZ_ASSERT(end_ <= other.begin_ || begin_ >= other.end_ ||
               (begin_ > other.begin_ && end_ < other.end_) ||
               (other.begin_ > begin_ && other.end_ < end_));
    // A total order is therefore given solely by comparing end points. This
    // order will be such that the first try note to intersect a point is the
    // innermost try note for that point.
    return end_ < other.end_;
  }
};

WASM_DECLARE_CACHEABLE_POD(TryNote);
WASM_DECLARE_POD_VECTOR(TryNote, TryNoteVector)

// CallIndirectId describes how to compile a call_indirect and matching
// signature check in the function prologue for a given function type.

enum class CallIndirectIdKind { AsmJS, Immediate, Global, None };

class CallIndirectId {
  CallIndirectIdKind kind_;
  size_t bits_;

  CallIndirectId(CallIndirectIdKind kind, size_t bits)
      : kind_(kind), bits_(bits) {}

 public:
  CallIndirectId() : kind_(CallIndirectIdKind::None), bits_(0) {}

  // Get a CallIndirectId for an asm.js function which will generate a no-op
  // checked call prologue.
  static CallIndirectId forAsmJSFunc();

  // Get the CallIndirectId for a function in a specific module.
  static CallIndirectId forFunc(const ModuleEnvironment& moduleEnv,
                                uint32_t funcIndex);

  // Get the CallIndirectId for a function type in a specific module.
  static CallIndirectId forFuncType(const ModuleEnvironment& moduleEnv,
                                    uint32_t funcTypeIndex);

  CallIndirectIdKind kind() const { return kind_; }
  bool isGlobal() const { return kind_ == CallIndirectIdKind::Global; }

  uint32_t immediate() const {
    MOZ_ASSERT(kind_ == CallIndirectIdKind::Immediate);
    return bits_;
  }
  uint32_t instanceDataOffset() const {
    MOZ_ASSERT(kind_ == CallIndirectIdKind::Global);
    return bits_;
  }
};

// CalleeDesc describes how to compile one of the variety of asm.js/wasm calls.
// This is hoisted into WasmCodegenTypes.h for sharing between Ion and Baseline.

class CalleeDesc {
 public:
  enum Which {
    // Calls a function defined in the same module by its index.
    Func,

    // Calls the import identified by the offset of its FuncImportInstanceData
    // in
    // thread-local data.
    Import,

    // Calls a WebAssembly table (heterogeneous, index must be bounds
    // checked, callee instance depends on TableDesc).
    WasmTable,

    // Calls an asm.js table (homogeneous, masked index, same-instance).
    AsmJSTable,

    // Call a C++ function identified by SymbolicAddress.
    Builtin,

    // Like Builtin, but automatically passes Instance* as first argument.
    BuiltinInstanceMethod,

    // Calls a function reference.
    FuncRef,
  };

 private:
  // which_ shall be initialized in the static constructors
  MOZ_INIT_OUTSIDE_CTOR Which which_;
  union U {
    U() : funcIndex_(0) {}
    uint32_t funcIndex_;
    struct {
      uint32_t instanceDataOffset_;
    } import;
    struct {
      uint32_t instanceDataOffset_;
      uint32_t minLength_;
      Maybe<uint32_t> maxLength_;
      CallIndirectId callIndirectId_;
    } table;
    SymbolicAddress builtin_;
  } u;

 public:
  CalleeDesc() = default;
  static CalleeDesc function(uint32_t funcIndex);
  static CalleeDesc import(uint32_t instanceDataOffset);
  static CalleeDesc wasmTable(const ModuleEnvironment& moduleEnv,
                              const TableDesc& desc, uint32_t tableIndex,
                              CallIndirectId callIndirectId);
  static CalleeDesc asmJSTable(const ModuleEnvironment& moduleEnv,
                               uint32_t tableIndex);
  static CalleeDesc builtin(SymbolicAddress callee);
  static CalleeDesc builtinInstanceMethod(SymbolicAddress callee);
  static CalleeDesc wasmFuncRef();
  Which which() const { return which_; }
  uint32_t funcIndex() const {
    MOZ_ASSERT(which_ == Func);
    return u.funcIndex_;
  }
  uint32_t importInstanceDataOffset() const {
    MOZ_ASSERT(which_ == Import);
    return u.import.instanceDataOffset_;
  }
  bool isTable() const { return which_ == WasmTable || which_ == AsmJSTable; }
  uint32_t tableLengthInstanceDataOffset() const {
    MOZ_ASSERT(isTable());
    return u.table.instanceDataOffset_ + offsetof(TableInstanceData, length);
  }
  uint32_t tableFunctionBaseInstanceDataOffset() const {
    MOZ_ASSERT(isTable());
    return u.table.instanceDataOffset_ + offsetof(TableInstanceData, elements);
  }
  CallIndirectId wasmTableSigId() const {
    MOZ_ASSERT(which_ == WasmTable);
    return u.table.callIndirectId_;
  }
  uint32_t wasmTableMinLength() const {
    MOZ_ASSERT(which_ == WasmTable);
    return u.table.minLength_;
  }
  Maybe<uint32_t> wasmTableMaxLength() const {
    MOZ_ASSERT(which_ == WasmTable);
    return u.table.maxLength_;
  }
  SymbolicAddress builtin() const {
    MOZ_ASSERT(which_ == Builtin || which_ == BuiltinInstanceMethod);
    return u.builtin_;
  }
  bool isFuncRef() const { return which_ == FuncRef; }
};

}  // namespace wasm
}  // namespace js

#endif  // wasm_codegen_types_h

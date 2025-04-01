/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 *
 * Copyright 2016 Mozilla Foundation
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

// This is an INTERNAL header for Wasm baseline compiler: the compiler object
// and its supporting types.

#ifndef wasm_wasm_baseline_object_h
#define wasm_wasm_baseline_object_h

#include "wasm/WasmBCDefs.h"
#include "wasm/WasmBCFrame.h"
#include "wasm/WasmBCRegDefs.h"
#include "wasm/WasmBCStk.h"
#include "wasm/WasmConstants.h"

namespace js {
namespace wasm {

// Container for a piece of out-of-line code, the slow path that supports an
// operation.
class OutOfLineCode;

// Part of the inter-bytecode state for the boolean-evaluation-for-control
// optimization.
struct BranchState;

// Representation of wasm local variables.
using Local = BaseStackFrame::Local;

// Bitset used for simple bounds check elimination.  Capping this at 64 locals
// makes sense; even 32 locals would probably be OK in practice.
//
// For more information about BCE, see the block comment in WasmBCMemory.cpp.
using BCESet = uint64_t;

// Information stored in the control node for generating exception handling
// landing pads.
struct CatchInfo {
  uint32_t tagIndex;        // Index for the associated exception.
  NonAssertingLabel label;  // The entry label for the handler.

  explicit CatchInfo(uint32_t tagIndex_) : tagIndex(tagIndex_) {}
};

using CatchInfoVector = Vector<CatchInfo, 1, SystemAllocPolicy>;

// Control node, representing labels and stack heights at join points.
struct Control {
  NonAssertingLabel label;       // The "exit" label
  NonAssertingLabel otherLabel;  // Used for the "else" branch of if-then-else
  // and to allow delegate to jump to catches.
  StackHeight stackHeight;     // From BaseStackFrame
  uint32_t stackSize;          // Value stack height
  BCESet bceSafeOnEntry;       // Bounds check info flowing into the item
  BCESet bceSafeOnExit;        // Bounds check info flowing out of the item
  bool deadOnArrival;          // deadCode_ was set on entry to the region
  bool deadThenBranch;         // deadCode_ was set on exit from "then"
  size_t tryNoteIndex;         // For tracking try branch code ranges.
  CatchInfoVector catchInfos;  // Used for try-catch handlers.

  Control()
      : stackHeight(StackHeight::Invalid()),
        stackSize(UINT32_MAX),
        bceSafeOnEntry(0),
        bceSafeOnExit(~BCESet(0)),
        deadOnArrival(false),
        deadThenBranch(false),
        tryNoteIndex(0) {}

  Control(Control&&) = default;
  Control(const Control&) = delete;
};

// A vector of Nothing values, used for reading opcodes.
class BaseNothingVector {
  Nothing unused_;

 public:
  bool reserve(size_t size) { return true; }
  bool resize(size_t length) { return true; }
  Nothing& operator[](size_t) { return unused_; }
  Nothing& back() { return unused_; }
  size_t length() const { return 0; }
  bool append(Nothing& nothing) { return true; }
  void infallibleAppend(Nothing& nothing) {}
};

// The baseline compiler tracks values on a stack of its own -- it needs to scan
// that stack for spilling -- and thus has no need for the values maintained by
// the iterator.
struct BaseCompilePolicy {
  using Value = Nothing;
  using ValueVector = BaseNothingVector;

  // The baseline compiler uses the iterator's control stack, attaching
  // its own control information.
  using ControlItem = Control;
};

using BaseOpIter = OpIter<BaseCompilePolicy>;

// Latent operation for boolean-evaluation-for-control optimization.
enum class LatentOp { None, Compare, Eqz };

// Encapsulate the checking needed for a memory access.
struct AccessCheck {
  AccessCheck()
      : omitBoundsCheck(false),
        omitAlignmentCheck(false),
        onlyPointerAlignment(false) {}

  // If `omitAlignmentCheck` is true then we need check neither the
  // pointer nor the offset.  Otherwise, if `onlyPointerAlignment` is true
  // then we need check only the pointer.  Otherwise, check the sum of
  // pointer and offset.

  bool omitBoundsCheck;
  bool omitAlignmentCheck;
  bool onlyPointerAlignment;
};

// Encapsulate all the information about a function call.
struct FunctionCall {
  FunctionCall()
      : restoreRegisterStateAndRealm(false),
        usesSystemAbi(false),
#ifdef JS_CODEGEN_ARM
        hardFP(true),
#endif
        frameAlignAdjustment(0),
        stackArgAreaSize(0) {
  }

  WasmABIArgGenerator abi;
  bool restoreRegisterStateAndRealm;
  bool usesSystemAbi;
#ifdef JS_CODEGEN_ARM
  bool hardFP;
#endif
  size_t frameAlignAdjustment;
  size_t stackArgAreaSize;
};

enum class PreBarrierKind {
  // No pre-write barrier is required because the previous value is undefined.
  None,
  // Perform a pre-write barrier to mark the previous value if an incremental
  // GC is underway.
  Normal,
};

enum class PostBarrierKind {
  // Remove an existing store buffer entry if the new value does not require
  // one. This is required to preserve invariants with HeapPtr when used for
  // movable storage.
  Precise,
  // Add a store buffer entry if the new value requires it, but do not attempt
  // to remove a pre-existing entry.
  Imprecise,
};

struct BranchIfRefSubtypeRegisters {
  RegPtr superSTV;
  RegI32 scratch1;
  RegI32 scratch2;
};

//////////////////////////////////////////////////////////////////////////////
//
// Wasm baseline compiler proper.
//
// This is a struct and not a class because there is no real benefit to hiding
// anything, and because many static functions that are wrappers for masm
// methods need to reach into it and would otherwise have to be declared as
// friends.
//
// (Members generally have a '_' suffix but some don't because they are
// referenced everywhere and it would be tedious to spell that out.)

struct BaseCompiler final {
  ///////////////////////////////////////////////////////////////////////////
  //
  // Private types

  using LabelVector = Vector<NonAssertingLabel, 8, SystemAllocPolicy>;

  ///////////////////////////////////////////////////////////////////////////
  //
  // Read-only and write-once members.

  // Static compilation environment.
  const ModuleEnvironment& moduleEnv_;
  const CompilerEnvironment& compilerEnv_;
  const FuncCompileInput& func_;
  const ValTypeVector& locals_;

  // Information about the locations of locals, this is set up during
  // initialization and read-only after that.
  BaseStackFrame::LocalVector localInfo_;

  // On specific platforms we sometimes need to use specific registers.
  const SpecificRegs specific_;

  // SigD and SigF are single-entry parameter lists for f64 and f32, these are
  // created during initialization.
  ValTypeVector SigD_;
  ValTypeVector SigF_;

  // Where to go to to return, bound as compilation ends.
  NonAssertingLabel returnLabel_;

  // Prologue and epilogue offsets, initialized during prologue and epilogue
  // generation and only used by the caller.
  FuncOffsets offsets_;

  // We call this address from the breakable point when the breakpoint handler
  // is not null.
  NonAssertingLabel debugTrapStub_;
  uint32_t previousBreakablePoint_;

  // BaselineCompileFunctions() "lends" us the StkVector to use in this
  // BaseCompiler object, and that is installed in |stk_| in our constructor.
  // This is so as to avoid having to malloc/free the vector's contents at
  // each creation/destruction of a BaseCompiler object.  It does however mean
  // that we need to hold on to a reference to BaselineCompileFunctions()'s
  // vector, so we can swap (give) its contents back when this BaseCompiler
  // object is destroyed.  This significantly reduces the heap turnover of the
  // baseline compiler.  See bug 1532592.
  StkVector& stkSource_;

  ///////////////////////////////////////////////////////////////////////////
  //
  // Output-only data structures.

  // Bump allocator for temporary memory, used for the value stack and
  // out-of-line code blobs.  Bump-allocated memory is not freed until the end
  // of the compilation.
  TempAllocator::Fallible alloc_;

  // Machine code emitter.
  MacroAssembler& masm;

  ///////////////////////////////////////////////////////////////////////////
  //
  // Compilation state.

  // Decoder for this function, used for misc error reporting.
  Decoder& decoder_;

  // Opcode reader.
  BaseOpIter iter_;

  // Register allocator.
  BaseRegAlloc ra;

  // Stack frame abstraction.
  BaseStackFrame fr;

  // Latent out of line support code for some operations, code for these will be
  // emitted at the end of compilation.
  Vector<OutOfLineCode*, 8, SystemAllocPolicy> outOfLine_;

  // Stack map state.  This keeps track of live pointer slots and allows precise
  // stack maps to be generated at safe points.
  StackMapGenerator stackMapGenerator_;

  // Wasm value stack.  This maps values on the wasm stack to values in the
  // running code and their locations.
  //
  // The value stack facilitates on-the-fly register allocation and the use of
  // immediates in instructions.  It tracks latent constants, latent references
  // to locals, register contents, and values that have been flushed to the CPU
  // stack.
  //
  // The stack can be flushed to the CPU stack using sync().
  //
  // The stack is a StkVector rather than a StkVector& since constantly
  // dereferencing a StkVector& has been shown to add 0.5% or more to the
  // compiler's dynamic instruction count.
  StkVector stk_;

  // Flag indicating that the compiler is currently in a dead code region.
  bool deadCode_;

  // Store previously finished note to know if we need to insert a nop in
  // finishTryNote.
  size_t mostRecentFinishedTryNoteIndex_;

  ///////////////////////////////////////////////////////////////////////////
  //
  // State for bounds check elimination.

  // Locals that have been bounds checked and not updated since
  BCESet bceSafe_;

  ///////////////////////////////////////////////////////////////////////////
  //
  // State for boolean-evaluation-for-control.

  // Latent operation for branch (seen next)
  LatentOp latentOp_;

  // Operand type, if latentOp_ is true
  ValType latentType_;

  // Comparison operator, if latentOp_ == Compare, int types
  Assembler::Condition latentIntCmp_;

  // Comparison operator, if latentOp_ == Compare, float types
  Assembler::DoubleCondition latentDoubleCmp_;

  ///////////////////////////////////////////////////////////////////////////
  //
  // Main compilation API.
  //
  // A client will create a compiler object, and then call init(),
  // emitFunction(), and finish() in that order.

  BaseCompiler(const ModuleEnvironment& moduleEnv,
               const CompilerEnvironment& compilerEnv,
               const FuncCompileInput& func, const ValTypeVector& locals,
               const RegisterOffsets& trapExitLayout,
               size_t trapExitLayoutNumWords, Decoder& decoder,
               StkVector& stkSource, TempAllocator* alloc, MacroAssembler* masm,
               StackMaps* stackMaps);
  ~BaseCompiler();

  [[nodiscard]] bool init();
  [[nodiscard]] bool emitFunction();
  [[nodiscard]] FuncOffsets finish();

  //////////////////////////////////////////////////////////////////////////////
  //
  // Sundry accessor abstractions and convenience predicates.
  //
  // WasmBaselineObject-inl.h.

  inline const FuncType& funcType() const;
  inline bool usesMemory() const;
  inline bool usesSharedMemory(uint32_t memoryIndex) const;
  inline bool isMem32(uint32_t memoryIndex) const;
  inline bool isMem64(uint32_t memoryIndex) const;
  inline bool hugeMemoryEnabled(uint32_t memoryIndex) const;
  inline uint32_t instanceOffsetOfMemoryBase(uint32_t memoryIndex) const;
  inline uint32_t instanceOffsetOfBoundsCheckLimit(uint32_t memoryIndex) const;

  // The casts are used by some of the ScratchRegister implementations.
  operator MacroAssembler&() const { return masm; }
  operator BaseRegAlloc&() { return ra; }

  //////////////////////////////////////////////////////////////////////////////
  //
  // Locals.
  //
  // WasmBaselineObject-inl.h.

  // Assert that the local at the given index has the given type, and return a
  // reference to the Local.
  inline const Local& localFromSlot(uint32_t slot, MIRType type);

  //////////////////////////////////////////////////////////////////////////////
  //
  // Out of line code management.

  [[nodiscard]] OutOfLineCode* addOutOfLineCode(OutOfLineCode* ool);
  [[nodiscard]] bool generateOutOfLineCode();

  /////////////////////////////////////////////////////////////////////////////
  //
  // Layering in the compiler (briefly).
  //
  // At the lowest layers are abstractions for registers (managed by the
  // BaseRegAlloc and the wrappers below) and the stack frame (managed by the
  // BaseStackFrame).
  //
  // The registers and frame are in turn used by the value abstraction, which is
  // implemented by the Stk type and backed by the value stack.  Values may be
  // stored in registers, in the frame, or may be latent constants, and the
  // value stack handles storage mostly transparently in its push and pop
  // routines.
  //
  // In turn, the pop routines bring values into registers so that we can
  // compute on them, and the push routines move values to the stack (where they
  // may still reside in registers until the registers are needed or the value
  // must be in memory).
  //
  // Routines for managing parameters and results (for blocks or calls) may also
  // manipulate the stack directly.
  //
  // At the top are the code generators: methods that use the poppers and
  // pushers and other utilities to move values into place, and that emit code
  // to compute on those values or change control flow.

  /////////////////////////////////////////////////////////////////////////////
  //
  // Register management.  These are simply strongly-typed wrappers that
  // delegate to the register allocator.

  inline bool isAvailableI32(RegI32 r);
  inline bool isAvailableI64(RegI64 r);
  inline bool isAvailableRef(RegRef r);
  inline bool isAvailablePtr(RegPtr r);
  inline bool isAvailableF32(RegF32 r);
  inline bool isAvailableF64(RegF64 r);
#ifdef ENABLE_WASM_SIMD
  inline bool isAvailableV128(RegV128 r);
#endif

  // Allocate any register
  [[nodiscard]] inline RegI32 needI32();
  [[nodiscard]] inline RegI64 needI64();
  [[nodiscard]] inline RegRef needRef();
  [[nodiscard]] inline RegPtr needPtr();
  [[nodiscard]] inline RegF32 needF32();
  [[nodiscard]] inline RegF64 needF64();
#ifdef ENABLE_WASM_SIMD
  [[nodiscard]] inline RegV128 needV128();
#endif

  // Allocate a specific register
  inline void needI32(RegI32 specific);
  inline void needI64(RegI64 specific);
  inline void needRef(RegRef specific);
  inline void needPtr(RegPtr specific);
  inline void needF32(RegF32 specific);
  inline void needF64(RegF64 specific);
#ifdef ENABLE_WASM_SIMD
  inline void needV128(RegV128 specific);
#endif

  template <typename RegType>
  inline RegType need();

  // Just a shorthand.
  inline void need2xI32(RegI32 r0, RegI32 r1);
  inline void need2xI64(RegI64 r0, RegI64 r1);

  // Get a register but do not sync the stack to free one up.  This will crash
  // if no register is available.
  inline void needI32NoSync(RegI32 r);

#if defined(JS_CODEGEN_ARM)
  // Allocate a specific register pair (even-odd register numbers).
  [[nodiscard]] inline RegI64 needI64Pair();
#endif

  inline void freeAny(AnyReg r);
  inline void freeI32(RegI32 r);
  inline void freeI64(RegI64 r);
  inline void freeRef(RegRef r);
  inline void freePtr(RegPtr r);
  inline void freeF32(RegF32 r);
  inline void freeF64(RegF64 r);
#ifdef ENABLE_WASM_SIMD
  inline void freeV128(RegV128 r);
#endif

  template <typename RegType>
  inline void free(RegType r);

  // Free r if it is not invalid.
  inline void maybeFree(RegI32 r);
  inline void maybeFree(RegI64 r);
  inline void maybeFree(RegF32 r);
  inline void maybeFree(RegF64 r);
  inline void maybeFree(RegRef r);
  inline void maybeFree(RegPtr r);
#ifdef ENABLE_WASM_SIMD
  inline void maybeFree(RegV128 r);
#endif

  // On 64-bit systems, `except` must equal r and this is a no-op.  On 32-bit
  // systems, `except` must equal the high or low part of a pair and the other
  // part of the pair is freed.
  inline void freeI64Except(RegI64 r, RegI32 except);

  // Return the 32-bit low part of the 64-bit register, do not free anything.
  inline RegI32 fromI64(RegI64 r);

  // If r is valid, return fromI64(r), otherwise an invalid RegI32.
  inline RegI32 maybeFromI64(RegI64 r);

#ifdef JS_PUNBOX64
  // On 64-bit systems, reinterpret r as 64-bit.
  inline RegI64 fromI32(RegI32 r);
#endif

  // Widen r to 64 bits; this may allocate another register to form a pair.
  // Note this does not generate code for sign/zero extension.
  inline RegI64 widenI32(RegI32 r);

  // Narrow r to 32 bits; this may free part of a pair.  Note this does not
  // generate code to canonicalize the value on 64-bit systems.
  inline RegI32 narrowI64(RegI64 r);
  inline RegI32 narrowRef(RegRef r);

  // Return the 32-bit low part of r.
  inline RegI32 lowPart(RegI64 r);

  // On 64-bit systems, return an invalid register.  On 32-bit systems, return
  // the low part of a pair.
  inline RegI32 maybeHighPart(RegI64 r);

  // On 64-bit systems, do nothing.  On 32-bit systems, clear the high register.
  inline void maybeClearHighPart(RegI64 r);

  //////////////////////////////////////////////////////////////////////////////
  //
  // Values and value stack: Low-level methods for moving Stk values of specific
  // kinds to registers.

  inline void loadConstI32(const Stk& src, RegI32 dest);
  inline void loadMemI32(const Stk& src, RegI32 dest);
  inline void loadLocalI32(const Stk& src, RegI32 dest);
  inline void loadRegisterI32(const Stk& src, RegI32 dest);
  inline void loadConstI64(const Stk& src, RegI64 dest);
  inline void loadMemI64(const Stk& src, RegI64 dest);
  inline void loadLocalI64(const Stk& src, RegI64 dest);
  inline void loadRegisterI64(const Stk& src, RegI64 dest);
  inline void loadConstRef(const Stk& src, RegRef dest);
  inline void loadMemRef(const Stk& src, RegRef dest);
  inline void loadLocalRef(const Stk& src, RegRef dest);
  inline void loadRegisterRef(const Stk& src, RegRef dest);
  inline void loadConstF64(const Stk& src, RegF64 dest);
  inline void loadMemF64(const Stk& src, RegF64 dest);
  inline void loadLocalF64(const Stk& src, RegF64 dest);
  inline void loadRegisterF64(const Stk& src, RegF64 dest);
  inline void loadConstF32(const Stk& src, RegF32 dest);
  inline void loadMemF32(const Stk& src, RegF32 dest);
  inline void loadLocalF32(const Stk& src, RegF32 dest);
  inline void loadRegisterF32(const Stk& src, RegF32 dest);
#ifdef ENABLE_WASM_SIMD
  inline void loadConstV128(const Stk& src, RegV128 dest);
  inline void loadMemV128(const Stk& src, RegV128 dest);
  inline void loadLocalV128(const Stk& src, RegV128 dest);
  inline void loadRegisterV128(const Stk& src, RegV128 dest);
#endif

  //////////////////////////////////////////////////////////////////////////
  //
  // Values and value stack: Mid-level routines for moving Stk values of any
  // kind to registers.

  inline void loadI32(const Stk& src, RegI32 dest);
  inline void loadI64(const Stk& src, RegI64 dest);
#if !defined(JS_PUNBOX64)
  inline void loadI64Low(const Stk& src, RegI32 dest);
  inline void loadI64High(const Stk& src, RegI32 dest);
#endif
  inline void loadF64(const Stk& src, RegF64 dest);
  inline void loadF32(const Stk& src, RegF32 dest);
#ifdef ENABLE_WASM_SIMD
  inline void loadV128(const Stk& src, RegV128 dest);
#endif
  inline void loadRef(const Stk& src, RegRef dest);

  //////////////////////////////////////////////////////////////////////
  //
  // Value stack: stack management.

  // Flush all local and register value stack elements to memory.
  inline void sync();

  // Save a register on the value stack temporarily.
  void saveTempPtr(const RegPtr& r);

  // Restore a temporarily saved register from the value stack.
  void restoreTempPtr(const RegPtr& r);

  // This is an optimization used to avoid calling sync for setLocal: if the
  // local does not exist unresolved on the value stack then we can skip the
  // sync.
  inline bool hasLocal(uint32_t slot);

  // Sync the local if necessary. (This currently syncs everything if a sync is
  // needed at all.)
  inline void syncLocal(uint32_t slot);

  // Return the amount of execution stack consumed by the top numval
  // values on the value stack.
  inline size_t stackConsumed(size_t numval);

  // Drop one value off the stack, possibly also moving the physical stack
  // pointer.
  inline void dropValue();

#ifdef DEBUG
  // Check that we're not leaking registers by comparing the
  // state of the stack + available registers with the set of
  // all available registers.

  // Call this between opcodes.
  void performRegisterLeakCheck();

  // This can be called at any point, really, but typically just after
  // performRegisterLeakCheck().
  void assertStackInvariants() const;

  // Count the number of memory references on the value stack.
  inline size_t countMemRefsOnStk();

  // Print the stack to stderr.
  void showStack(const char* who) const;
#endif

  //////////////////////////////////////////////////////////////////////
  //
  // Value stack: pushers of values.

  // Push a register onto the value stack.
  inline void pushAny(AnyReg r);
  inline void pushI32(RegI32 r);
  inline void pushI64(RegI64 r);
  inline void pushRef(RegRef r);
  inline void pushPtr(RegPtr r);
  inline void pushF64(RegF64 r);
  inline void pushF32(RegF32 r);
#ifdef ENABLE_WASM_SIMD
  inline void pushV128(RegV128 r);
#endif

  // Template variation of the foregoing, for use by templated emitters.
  template <typename RegType>
  inline void push(RegType item);

  // Push a constant value onto the stack.  pushI32 can also take uint32_t, and
  // pushI64 can take uint64_t; the semantics are the same.  Appropriate sign
  // extension for a 32-bit value on a 64-bit architecture happens when the
  // value is popped, see the definition of moveImm32.
  inline void pushI32(int32_t v);
  inline void pushI64(int64_t v);
  inline void pushRef(intptr_t v);
  inline void pushPtr(intptr_t v);
  inline void pushF64(double v);
  inline void pushF32(float v);
#ifdef ENABLE_WASM_SIMD
  inline void pushV128(V128 v);
#endif
  inline void pushConstRef(intptr_t v);

  // Push the local slot onto the stack.  The slot will not be read here; it
  // will be read when it is consumed, or when a side effect to the slot forces
  // its value to be saved.
  inline void pushLocalI32(uint32_t slot);
  inline void pushLocalI64(uint32_t slot);
  inline void pushLocalRef(uint32_t slot);
  inline void pushLocalF64(uint32_t slot);
  inline void pushLocalF32(uint32_t slot);
#ifdef ENABLE_WASM_SIMD
  inline void pushLocalV128(uint32_t slot);
#endif

  // Push an U32 as an I64, zero-extending it in the process
  inline void pushU32AsI64(RegI32 rs);

  //////////////////////////////////////////////////////////////////////
  //
  // Value stack: poppers and peekers of values.

  // Pop some value off the stack.
  inline AnyReg popAny();
  inline AnyReg popAny(AnyReg specific);

  // Call only from other popI32() variants.  v must be the stack top.  May pop
  // the CPU stack.
  inline void popI32(const Stk& v, RegI32 dest);

  [[nodiscard]] inline RegI32 popI32();
  inline RegI32 popI32(RegI32 specific);

#ifdef ENABLE_WASM_SIMD
  // Call only from other popV128() variants.  v must be the stack top.  May pop
  // the CPU stack.
  inline void popV128(const Stk& v, RegV128 dest);

  [[nodiscard]] inline RegV128 popV128();
  inline RegV128 popV128(RegV128 specific);
#endif

  // Call only from other popI64() variants.  v must be the stack top.  May pop
  // the CPU stack.
  inline void popI64(const Stk& v, RegI64 dest);

  [[nodiscard]] inline RegI64 popI64();
  inline RegI64 popI64(RegI64 specific);

  // Call only from other popRef() variants.  v must be the stack top.  May pop
  // the CPU stack.
  inline void popRef(const Stk& v, RegRef dest);

  inline RegRef popRef(RegRef specific);
  [[nodiscard]] inline RegRef popRef();

  // Call only from other popPtr() variants.  v must be the stack top.  May pop
  // the CPU stack.
  inline void popPtr(const Stk& v, RegPtr dest);

  inline RegPtr popPtr(RegPtr specific);
  [[nodiscard]] inline RegPtr popPtr();

  // Call only from other popF64() variants.  v must be the stack top.  May pop
  // the CPU stack.
  inline void popF64(const Stk& v, RegF64 dest);

  [[nodiscard]] inline RegF64 popF64();
  inline RegF64 popF64(RegF64 specific);

  // Call only from other popF32() variants.  v must be the stack top.  May pop
  // the CPU stack.
  inline void popF32(const Stk& v, RegF32 dest);

  [[nodiscard]] inline RegF32 popF32();
  inline RegF32 popF32(RegF32 specific);

  // Templated variation of the foregoing, for use by templated emitters.
  template <typename RegType>
  inline RegType pop();

  // Constant poppers will return true and pop the value if the stack top is a
  // constant of the appropriate type; otherwise pop nothing and return false.
  [[nodiscard]] inline bool hasConst() const;
  [[nodiscard]] inline bool popConst(int32_t* c);
  [[nodiscard]] inline bool popConst(int64_t* c);
  [[nodiscard]] inline bool peekConst(int32_t* c);
  [[nodiscard]] inline bool peekConst(int64_t* c);
  [[nodiscard]] inline bool peek2xConst(int32_t* c0, int32_t* c1);
  [[nodiscard]] inline bool popConstPositivePowerOfTwo(int32_t* c,
                                                       uint_fast8_t* power,
                                                       int32_t cutoff);
  [[nodiscard]] inline bool popConstPositivePowerOfTwo(int64_t* c,
                                                       uint_fast8_t* power,
                                                       int64_t cutoff);

  // Shorthand: Pop r1, then r0.
  inline void pop2xI32(RegI32* r0, RegI32* r1);
  inline void pop2xI64(RegI64* r0, RegI64* r1);
  inline void pop2xF32(RegF32* r0, RegF32* r1);
  inline void pop2xF64(RegF64* r0, RegF64* r1);
#ifdef ENABLE_WASM_SIMD
  inline void pop2xV128(RegV128* r0, RegV128* r1);
#endif
  inline void pop2xRef(RegRef* r0, RegRef* r1);

  // Pop to a specific register
  inline RegI32 popI32ToSpecific(RegI32 specific);
  inline RegI64 popI64ToSpecific(RegI64 specific);

#ifdef JS_CODEGEN_ARM
  // Pop an I64 as a valid register pair.
  inline RegI64 popI64Pair();
#endif

  // Pop an I64 but narrow it and return the narrowed part.
  inline RegI32 popI64ToI32();
  inline RegI32 popI64ToSpecificI32(RegI32 specific);

  // Pop an I32 or I64 as an I64. The value is zero extended out to 64-bits.
  inline RegI64 popIndexToInt64(IndexType indexType);

  // Pop the stack until it has the desired size, but do not move the physical
  // stack pointer.
  inline void popValueStackTo(uint32_t stackSize);

  // Pop the given number of elements off the value stack, but do not move
  // the physical stack pointer.
  inline void popValueStackBy(uint32_t items);

  // Peek into the stack at relativeDepth from the top.
  inline Stk& peek(uint32_t relativeDepth);

  // Peek the reference value at the specified depth and load it into a
  // register.
  inline void peekRefAt(uint32_t depth, RegRef dest);

  // Peek at the value on the top of the stack and return true if it is a Local
  // of any type.
  [[nodiscard]] inline bool peekLocal(uint32_t* local);

  ////////////////////////////////////////////////////////////////////////////
  //
  // Block parameters and results.
  //
  // Blocks may have multiple parameters and multiple results.  Blocks can also
  // be the target of branches: the entry for loops, and the exit for
  // non-loops.
  //
  // Passing multiple values to a non-branch target (i.e., the entry of a
  // "block") falls out naturally: any items on the value stack can flow
  // directly from one block to another.
  //
  // However, for branch targets, we need to allocate well-known locations for
  // the branch values.  The approach taken in the baseline compiler is to
  // allocate registers to the top N values (currently N=1), and then stack
  // locations for the rest.
  //

  // Types of result registers that interest us for result-manipulating
  // functions.
  enum class ResultRegKind {
    // General and floating result registers.
    All,

    // General result registers only.
    OnlyGPRs
  };

  // This is a flag ultimately intended for popBlockResults() that specifies how
  // the CPU stack should be handled after the result values have been
  // processed.
  enum class ContinuationKind {
    // Adjust the stack for a fallthrough: do nothing.
    Fallthrough,

    // Adjust the stack for a jump: make the stack conform to the
    // expected stack at the target
    Jump
  };

  // TODO: It's definitely disputable whether the result register management is
  // hot enough to warrant inlining at the outermost level.

  inline void needResultRegisters(ResultType type, ResultRegKind which);
#ifdef JS_64BIT
  inline void widenInt32ResultRegisters(ResultType type);
#endif
  inline void freeResultRegisters(ResultType type, ResultRegKind which);
  inline void needIntegerResultRegisters(ResultType type);
  inline void freeIntegerResultRegisters(ResultType type);
  inline void needResultRegisters(ResultType type);
  inline void freeResultRegisters(ResultType type);
  void assertResultRegistersAvailable(ResultType type);
  inline void captureResultRegisters(ResultType type);
  inline void captureCallResultRegisters(ResultType type);

  void popRegisterResults(ABIResultIter& iter);
  void popStackResults(ABIResultIter& iter, StackHeight stackBase);

  void popBlockResults(ResultType type, StackHeight stackBase,
                       ContinuationKind kind);

  // This function is similar to popBlockResults, but additionally handles the
  // implicit exception pointer that is pushed to the value stack on entry to
  // a catch handler by dropping it appropriately.
  void popCatchResults(ResultType type, StackHeight stackBase);

  Stk captureStackResult(const ABIResult& result, StackHeight resultsBase,
                         uint32_t stackResultBytes);

  [[nodiscard]] bool pushResults(ResultType type, StackHeight resultsBase);
  [[nodiscard]] bool pushBlockResults(ResultType type);

  // A combination of popBlockResults + pushBlockResults, used when entering a
  // block with a control-flow join (loops) or split (if) to shuffle the
  // fallthrough block parameters into the locations expected by the
  // continuation.
  //
  // This function should only be called when entering a block with a
  // control-flow join at the entry, where there are no live temporaries in
  // the current block.
  [[nodiscard]] bool topBlockParams(ResultType type);

  // A combination of popBlockResults + pushBlockResults, used before branches
  // where we don't know the target (br_if / br_table).  If and when the branch
  // is taken, the stack results will be shuffled down into place.  For br_if
  // that has fallthrough, the parameters for the untaken branch flow through to
  // the continuation.
  [[nodiscard]] bool topBranchParams(ResultType type, StackHeight* height);

  // Conditional branches with fallthrough are preceded by a topBranchParams, so
  // we know that there are no stack results that need to be materialized.  In
  // that case, we can just shuffle the whole block down before popping the
  // stack.
  void shuffleStackResultsBeforeBranch(StackHeight srcHeight,
                                       StackHeight destHeight, ResultType type);

  // If in debug mode, adds LeaveFrame breakpoint.
  bool insertDebugCollapseFrame();

  //////////////////////////////////////////////////////////////////////
  //
  // Stack maps

  // Various methods for creating a stackmap.  Stackmaps are indexed by the
  // lowest address of the instruction immediately *after* the instruction of
  // interest.  In practice that means either: the return point of a call, the
  // instruction immediately after a trap instruction (the "resume"
  // instruction), or the instruction immediately following a no-op (when
  // debugging is enabled).

  // Create a vanilla stackmap.
  [[nodiscard]] bool createStackMap(const char* who);

  // Create a stackmap as vanilla, but for a custom assembler offset.
  [[nodiscard]] bool createStackMap(const char* who,
                                    CodeOffset assemblerOffset);

  // Create a stack map as vanilla, and note the presence of a ref-typed
  // DebugFrame on the stack.
  [[nodiscard]] bool createStackMap(
      const char* who, HasDebugFrameWithLiveRefs debugFrameWithLiveRefs);

  // The most general stackmap construction.
  [[nodiscard]] bool createStackMap(
      const char* who, const ExitStubMapVector& extras,
      uint32_t assemblerOffset,
      HasDebugFrameWithLiveRefs debugFrameWithLiveRefs);

  ////////////////////////////////////////////////////////////
  //
  // Control stack

  inline void initControl(Control& item, ResultType params);
  inline Control& controlItem();
  inline Control& controlItem(uint32_t relativeDepth);
  inline Control& controlOutermost();
  inline LabelKind controlKind(uint32_t relativeDepth);

  ////////////////////////////////////////////////////////////
  //
  // Debugger API

  // Insert a breakpoint almost anywhere.  This will create a call, with all the
  // overhead that entails.
  void insertBreakablePoint(CallSiteDesc::Kind kind);

  // Insert code at the end of a function for breakpoint filtering.
  void insertBreakpointStub();

  // Debugger API used at the return point: shuffle register return values off
  // to memory for the debugger to see; and get them back again.
  void saveRegisterReturnValues(const ResultType& resultType);
  void restoreRegisterReturnValues(const ResultType& resultType);

  //////////////////////////////////////////////////////////////////////
  //
  // Function prologue and epilogue.

  // Set up and tear down frame, execute prologue and epilogue.
  [[nodiscard]] bool beginFunction();
  [[nodiscard]] bool endFunction();

  // Move return values to memory before returning, as appropriate
  void popStackReturnValues(const ResultType& resultType);

  //////////////////////////////////////////////////////////////////////
  //
  // Calls.

  void beginCall(FunctionCall& call, UseABI useABI,
                 RestoreRegisterStateAndRealm restoreRegisterStateAndRealm);
  void endCall(FunctionCall& call, size_t stackSpace);
  void startCallArgs(size_t stackArgAreaSizeUnaligned, FunctionCall* call);
  ABIArg reservePointerArgument(FunctionCall* call);
  void passArg(ValType type, const Stk& arg, FunctionCall* call);
  CodeOffset callDefinition(uint32_t funcIndex, const FunctionCall& call);
  CodeOffset callSymbolic(SymbolicAddress callee, const FunctionCall& call);

  // Precondition for the call*() methods: sync()

  bool callIndirect(uint32_t funcTypeIndex, uint32_t tableIndex,
                    const Stk& indexVal, const FunctionCall& call,
                    bool tailCall, CodeOffset* fastCallOffset,
                    CodeOffset* slowCallOffset);
  CodeOffset callImport(unsigned instanceDataOffset, const FunctionCall& call);
#ifdef ENABLE_WASM_GC
  void callRef(const Stk& calleeRef, const FunctionCall& call,
               CodeOffset* fastCallOffset, CodeOffset* slowCallOffset);
#  ifdef ENABLE_WASM_TAIL_CALLS
  void returnCallRef(const Stk& calleeRef, const FunctionCall& call,
                     const FuncType* funcType);
#  endif
#endif
  CodeOffset builtinCall(SymbolicAddress builtin, const FunctionCall& call);
  CodeOffset builtinInstanceMethodCall(const SymbolicAddressSignature& builtin,
                                       const ABIArg& instanceArg,
                                       const FunctionCall& call);
  [[nodiscard]] bool pushCallResults(const FunctionCall& call, ResultType type,
                                     const StackResultsLoc& loc);

  // Helpers to pick up the returned value from the return register.
  inline RegI32 captureReturnedI32();
  inline RegI64 captureReturnedI64();
  inline RegF32 captureReturnedF32(const FunctionCall& call);
  inline RegF64 captureReturnedF64(const FunctionCall& call);
#ifdef ENABLE_WASM_SIMD
  inline RegV128 captureReturnedV128(const FunctionCall& call);
#endif
  inline RegRef captureReturnedRef();

  //////////////////////////////////////////////////////////////////////
  //
  // Register-to-register moves.  These emit nothing if src == dest.

  inline void moveI32(RegI32 src, RegI32 dest);
  inline void moveI64(RegI64 src, RegI64 dest);
  inline void moveRef(RegRef src, RegRef dest);
  inline void movePtr(RegPtr src, RegPtr dest);
  inline void moveF64(RegF64 src, RegF64 dest);
  inline void moveF32(RegF32 src, RegF32 dest);
#ifdef ENABLE_WASM_SIMD
  inline void moveV128(RegV128 src, RegV128 dest);
#endif

  template <typename RegType>
  inline void move(RegType src, RegType dest);

  //////////////////////////////////////////////////////////////////////
  //
  // Immediate-to-register moves.
  //
  // The compiler depends on moveImm32() clearing the high bits of a 64-bit
  // register on 64-bit systems except MIPS64 And LoongArch64 where high bits
  // are sign extended from lower bits, see doc block "64-bit GPRs carrying
  // 32-bit values" in MacroAssembler.h.

  inline void moveImm32(int32_t v, RegI32 dest);
  inline void moveImm64(int64_t v, RegI64 dest);
  inline void moveImmRef(intptr_t v, RegRef dest);

  //////////////////////////////////////////////////////////////////////
  //
  // Sundry low-level code generators.

  // Check the interrupt flag, trap if it is set.
  [[nodiscard]] bool addInterruptCheck();

  // Check that the value is not zero, trap if it is.
  void checkDivideByZero(RegI32 rhs);
  void checkDivideByZero(RegI64 r);

  // Check that a signed division will not overflow, trap or flush-to-zero if it
  // will according to `zeroOnOverflow`.
  void checkDivideSignedOverflow(RegI32 rhs, RegI32 srcDest, Label* done,
                                 bool zeroOnOverflow);
  void checkDivideSignedOverflow(RegI64 rhs, RegI64 srcDest, Label* done,
                                 bool zeroOnOverflow);

  // Emit a jump table to be used by tableSwitch()
  void jumpTable(const LabelVector& labels, Label* theTable);

  // Emit a table switch, `theTable` is the jump table.
  void tableSwitch(Label* theTable, RegI32 switchValue, Label* dispatchCode);

  // Compare i64 and set an i32 boolean result according to the condition.
  inline void cmp64Set(Assembler::Condition cond, RegI64 lhs, RegI64 rhs,
                       RegI32 dest);

  // Round floating to integer.
  [[nodiscard]] inline bool supportsRoundInstruction(RoundingMode mode);
  inline void roundF32(RoundingMode roundingMode, RegF32 f0);
  inline void roundF64(RoundingMode roundingMode, RegF64 f0);

  // These are just wrappers around assembler functions, but without
  // type-specific names, and using our register abstractions for better type
  // discipline.
  inline void branchTo(Assembler::DoubleCondition c, RegF64 lhs, RegF64 rhs,
                       Label* l);
  inline void branchTo(Assembler::DoubleCondition c, RegF32 lhs, RegF32 rhs,
                       Label* l);
  inline void branchTo(Assembler::Condition c, RegI32 lhs, RegI32 rhs,
                       Label* l);
  inline void branchTo(Assembler::Condition c, RegI32 lhs, Imm32 rhs, Label* l);
  inline void branchTo(Assembler::Condition c, RegI64 lhs, RegI64 rhs,
                       Label* l);
  inline void branchTo(Assembler::Condition c, RegI64 lhs, Imm64 rhs, Label* l);
  inline void branchTo(Assembler::Condition c, RegRef lhs, ImmWord rhs,
                       Label* l);

  // Helpers for accessing Instance::baselineScratchWords_.  Note that Word
  // and I64 versions of these routines access the same area and it is up to
  // the caller to use it in some way which makes sense.

  // Store/load `r`, a machine word, to/from the `index`th scratch storage
  // slot in the current Instance.  `instancePtr` must point at the current
  // Instance; it will not be modified.  For ::stashWord, `r` must not be the
  // same as `instancePtr`.
  void stashWord(RegPtr instancePtr, size_t index, RegPtr r);
  void unstashWord(RegPtr instancePtr, size_t index, RegPtr r);

#ifdef JS_CODEGEN_X86
  // Store r in instance scratch storage after first loading the instance from
  // the frame into the regForInstance.  regForInstance must be neither of the
  // registers in r.
  void stashI64(RegPtr regForInstance, RegI64 r);

  // Load r from the instance scratch storage after first loading the instance
  // from the frame into the regForInstance.  regForInstance can be one of the
  // registers in r.
  void unstashI64(RegPtr regForInstance, RegI64 r);
#endif

  //////////////////////////////////////////////////////////////////////
  //
  // Code generators for actual operations.

  template <typename RegType, typename IntType>
  void quotientOrRemainder(RegType rs, RegType rsd, RegType reserved,
                           IsUnsigned isUnsigned, ZeroOnOverflow zeroOnOverflow,
                           bool isConst, IntType c,
                           void (*operate)(MacroAssembler&, RegType, RegType,
                                           RegType, IsUnsigned));

  [[nodiscard]] bool truncateF32ToI32(RegF32 src, RegI32 dest,
                                      TruncFlags flags);
  [[nodiscard]] bool truncateF64ToI32(RegF64 src, RegI32 dest,
                                      TruncFlags flags);

#ifndef RABALDR_FLOAT_TO_I64_CALLOUT
  [[nodiscard]] RegF64 needTempForFloatingToI64(TruncFlags flags);
  [[nodiscard]] bool truncateF32ToI64(RegF32 src, RegI64 dest, TruncFlags flags,
                                      RegF64 temp);
  [[nodiscard]] bool truncateF64ToI64(RegF64 src, RegI64 dest, TruncFlags flags,
                                      RegF64 temp);
#endif  // RABALDR_FLOAT_TO_I64_CALLOUT

#ifndef RABALDR_I64_TO_FLOAT_CALLOUT
  [[nodiscard]] RegI32 needConvertI64ToFloatTemp(ValType to, bool isUnsigned);
  void convertI64ToF32(RegI64 src, bool isUnsigned, RegF32 dest, RegI32 temp);
  void convertI64ToF64(RegI64 src, bool isUnsigned, RegF64 dest, RegI32 temp);
#endif  // RABALDR_I64_TO_FLOAT_CALLOUT

  //////////////////////////////////////////////////////////////////////
  //
  // Global variable access.

  Address addressOfGlobalVar(const GlobalDesc& global, RegPtr tmp);

  //////////////////////////////////////////////////////////////////////
  //
  // Table access.

  Address addressOfTableField(uint32_t tableIndex, uint32_t fieldOffset,
                              RegPtr instance);
  void loadTableLength(uint32_t tableIndex, RegPtr instance, RegI32 length);
  void loadTableElements(uint32_t tableIndex, RegPtr instance, RegPtr elements);

  //////////////////////////////////////////////////////////////////////
  //
  // Heap access.

  void bceCheckLocal(MemoryAccessDesc* access, AccessCheck* check,
                     uint32_t local);
  void bceLocalIsUpdated(uint32_t local);

  // Fold offsets into ptr and bounds check as necessary.  The instance will be
  // valid in cases where it's needed.
  template <typename RegIndexType>
  void prepareMemoryAccess(MemoryAccessDesc* access, AccessCheck* check,
                           RegPtr instance, RegIndexType ptr);

  void branchAddNoOverflow(uint64_t offset, RegI32 ptr, Label* ok);
  void branchTestLowZero(RegI32 ptr, Imm32 mask, Label* ok);
  void boundsCheck4GBOrLargerAccess(uint32_t memoryIndex, RegPtr instance,
                                    RegI32 ptr, Label* ok);
  void boundsCheckBelow4GBAccess(uint32_t memoryIndex, RegPtr instance,
                                 RegI32 ptr, Label* ok);

  void branchAddNoOverflow(uint64_t offset, RegI64 ptr, Label* ok);
  void branchTestLowZero(RegI64 ptr, Imm32 mask, Label* ok);
  void boundsCheck4GBOrLargerAccess(uint32_t memoryIndex, RegPtr instance,
                                    RegI64 ptr, Label* ok);
  void boundsCheckBelow4GBAccess(uint32_t memoryIndex, RegPtr instance,
                                 RegI64 ptr, Label* ok);

  // Some consumers depend on the returned Address not incorporating instance,
  // as instance may be the scratch register.
  template <typename RegIndexType>
  Address prepareAtomicMemoryAccess(MemoryAccessDesc* access,
                                    AccessCheck* check, RegPtr instance,
                                    RegIndexType ptr);

  template <typename RegIndexType>
  void computeEffectiveAddress(MemoryAccessDesc* access);

  [[nodiscard]] bool needInstanceForAccess(const MemoryAccessDesc* access,
                                           const AccessCheck& check);

  // ptr and dest may be the same iff dest is I32.
  // This may destroy ptr even if ptr and dest are not the same.
  void executeLoad(MemoryAccessDesc* access, AccessCheck* check,
                   RegPtr instance, RegPtr memoryBase, RegI32 ptr, AnyReg dest,
                   RegI32 temp);
  void load(MemoryAccessDesc* access, AccessCheck* check, RegPtr instance,
            RegPtr memoryBase, RegI32 ptr, AnyReg dest, RegI32 temp);
#ifdef ENABLE_WASM_MEMORY64
  void load(MemoryAccessDesc* access, AccessCheck* check, RegPtr instance,
            RegPtr memoryBase, RegI64 ptr, AnyReg dest, RegI64 temp);
#endif

  template <typename RegType>
  void doLoadCommon(MemoryAccessDesc* access, AccessCheck check, ValType type);

  void loadCommon(MemoryAccessDesc* access, AccessCheck check, ValType type);

  // ptr and src must not be the same register.
  // This may destroy ptr and src.
  void executeStore(MemoryAccessDesc* access, AccessCheck* check,
                    RegPtr instance, RegPtr memoryBase, RegI32 ptr, AnyReg src,
                    RegI32 temp);
  void store(MemoryAccessDesc* access, AccessCheck* check, RegPtr instance,
             RegPtr memoryBase, RegI32 ptr, AnyReg src, RegI32 temp);
#ifdef ENABLE_WASM_MEMORY64
  void store(MemoryAccessDesc* access, AccessCheck* check, RegPtr instance,
             RegPtr memoryBase, RegI64 ptr, AnyReg src, RegI64 temp);
#endif

  template <typename RegType>
  void doStoreCommon(MemoryAccessDesc* access, AccessCheck check,
                     ValType resultType);

  void storeCommon(MemoryAccessDesc* access, AccessCheck check,
                   ValType resultType);

  void atomicLoad(MemoryAccessDesc* access, ValType type);
#if !defined(JS_64BIT)
  template <typename RegIndexType>
  void atomicLoad64(MemoryAccessDesc* desc);
#endif

  void atomicStore(MemoryAccessDesc* access, ValType type);

  void atomicRMW(MemoryAccessDesc* access, ValType type, AtomicOp op);
  template <typename RegIndexType>
  void atomicRMW32(MemoryAccessDesc* access, ValType type, AtomicOp op);
  template <typename RegIndexType>
  void atomicRMW64(MemoryAccessDesc* access, ValType type, AtomicOp op);

  void atomicXchg(MemoryAccessDesc* access, ValType type);
  template <typename RegIndexType>
  void atomicXchg64(MemoryAccessDesc* access, WantResult wantResult);
  template <typename RegIndexType>
  void atomicXchg32(MemoryAccessDesc* access, ValType type);

  void atomicCmpXchg(MemoryAccessDesc* access, ValType type);
  template <typename RegIndexType>
  void atomicCmpXchg32(MemoryAccessDesc* access, ValType type);
  template <typename RegIndexType>
  void atomicCmpXchg64(MemoryAccessDesc* access, ValType type);

  template <typename RegType>
  RegType popConstMemoryAccess(MemoryAccessDesc* access, AccessCheck* check);
  template <typename RegType>
  RegType popMemoryAccess(MemoryAccessDesc* access, AccessCheck* check);

  void pushHeapBase(uint32_t memoryIndex);

  ////////////////////////////////////////////////////////////////////////////
  //
  // Platform-specific popping and register targeting.

  // The simple popping methods pop values into targeted registers; the caller
  // can free registers using standard functions.  These are always called
  // popXForY where X says something about types and Y something about the
  // operation being targeted.

  RegI32 needRotate64Temp();
  void popAndAllocateForDivAndRemI32(RegI32* r0, RegI32* r1, RegI32* reserved);
  void popAndAllocateForMulI64(RegI64* r0, RegI64* r1, RegI32* temp);
#ifndef RABALDR_INT_DIV_I64_CALLOUT
  void popAndAllocateForDivAndRemI64(RegI64* r0, RegI64* r1, RegI64* reserved,
                                     IsRemainder isRemainder);
#endif
  RegI32 popI32RhsForShift();
  RegI32 popI32RhsForShiftI64();
  RegI64 popI64RhsForShift();
  RegI32 popI32RhsForRotate();
  RegI64 popI64RhsForRotate();
  void popI32ForSignExtendI64(RegI64* r0);
  void popI64ForSignExtendI64(RegI64* r0);

  ////////////////////////////////////////////////////////////
  //
  // Sundry helpers.

  // Retrieve the current bytecodeOffset.
  inline BytecodeOffset bytecodeOffset() const;

  // Generate a trap instruction for the current bytecodeOffset.
  inline void trap(Trap t) const;

  // Abstracted helper for throwing, used for throw, rethrow, and rethrowing
  // at the end of a series of catch blocks (if none matched the exception).
  [[nodiscard]] bool throwFrom(RegRef exn);

  // Load the specified tag object from the Instance.
  void loadTag(RegPtr instance, uint32_t tagIndex, RegRef tagDst);

  // Load the pending exception state from the Instance and then reset it.
  void consumePendingException(RegPtr instance, RegRef* exnDst, RegRef* tagDst);

  [[nodiscard]] bool startTryNote(size_t* tryNoteIndex);
  void finishTryNote(size_t tryNoteIndex);

  ////////////////////////////////////////////////////////////
  //
  // Barriers support.

  // This emits a GC pre-write barrier.  The pre-barrier is needed when we
  // replace a member field with a new value, and the previous field value
  // might have no other referents, and incremental GC is ongoing. The field
  // might belong to an object or be a stack slot or a register or a heap
  // allocated value.
  //
  // let obj = { field: previousValue };
  // obj.field = newValue; // previousValue must be marked with a pre-barrier.
  //
  // The `valueAddr` is the address of the location that we are about to
  // update.  This function preserves that register.
  void emitPreBarrier(RegPtr valueAddr);

  // This emits a GC post-write barrier. The post-barrier is needed when we
  // replace a member field with a new value, the new value is in the nursery,
  // and the containing object is a tenured object. The field must then be
  // added to the store buffer so that the nursery can be correctly collected.
  // The field might belong to an object or be a stack slot or a register or a
  // heap allocated value.
  //
  // For the difference between 'precise' and 'imprecise', look at the
  // documentation on PostBarrierKind.
  //
  // `object` is a pointer to the object that contains the field. It is used, if
  // present, to skip adding a store buffer entry when the containing object is
  // in the nursery. This register is preserved by this function.
  // `valueAddr` is the address of the location that we are writing to. This
  // register is consumed by this function.
  // `prevValue` is the value that existed in the field before `value` was
  // stored. This register is consumed by this function.
  // `value` is the value that was stored in the field. This register is
  // preserved by this function.
  [[nodiscard]] bool emitPostBarrierImprecise(const Maybe<RegRef>& object,
                                              RegPtr valueAddr, RegRef value);
  [[nodiscard]] bool emitPostBarrierPrecise(const Maybe<RegRef>& object,
                                            RegPtr valueAddr, RegRef prevValue,
                                            RegRef value);

  // Emits a store to a JS object pointer at the address `valueAddr`, which is
  // inside the GC cell `object`.
  //
  // Preserves `object` and `value`. Consumes `valueAddr`.
  [[nodiscard]] bool emitBarrieredStore(const Maybe<RegRef>& object,
                                        RegPtr valueAddr, RegRef value,
                                        PreBarrierKind preBarrierKind,
                                        PostBarrierKind postBarrierKind);

  // Emits a store of nullptr to a JS object pointer at the address valueAddr.
  // Preserves `valueAddr`.
  void emitBarrieredClear(RegPtr valueAddr);

  ////////////////////////////////////////////////////////////
  //
  // Machinery for optimized conditional branches.  See comments in the
  // implementation.

  void setLatentCompare(Assembler::Condition compareOp, ValType operandType);
  void setLatentCompare(Assembler::DoubleCondition compareOp,
                        ValType operandType);
  void setLatentEqz(ValType operandType);
  bool hasLatentOp() const;
  void resetLatentOp();
  // Jump to the given branch, passing results, if the condition, `cond`
  // matches between `lhs` and `rhs.
  template <typename Cond, typename Lhs, typename Rhs>
  [[nodiscard]] bool jumpConditionalWithResults(BranchState* b, Cond cond,
                                                Lhs lhs, Rhs rhs);
#ifdef ENABLE_WASM_GC
  // Jump to the given branch, passing results, if the WasmGcObject, `object`,
  // is a subtype of `destType`.
  [[nodiscard]] bool jumpConditionalWithResults(BranchState* b, RegRef object,
                                                RefType sourceType,
                                                RefType destType,
                                                bool onSuccess);
#endif
  template <typename Cond>
  [[nodiscard]] bool sniffConditionalControlCmp(Cond compareOp,
                                                ValType operandType);
  [[nodiscard]] bool sniffConditionalControlEqz(ValType operandType);
  void emitBranchSetup(BranchState* b);
  [[nodiscard]] bool emitBranchPerform(BranchState* b);

  //////////////////////////////////////////////////////////////////////

  [[nodiscard]] bool emitBody();
  [[nodiscard]] bool emitBlock();
  [[nodiscard]] bool emitLoop();
  [[nodiscard]] bool emitIf();
  [[nodiscard]] bool emitElse();
  // Used for common setup for catch and catch_all.
  void emitCatchSetup(LabelKind kind, Control& tryCatch,
                      const ResultType& resultType);

  [[nodiscard]] bool emitTry();
  [[nodiscard]] bool emitTryTable();
  [[nodiscard]] bool emitCatch();
  [[nodiscard]] bool emitCatchAll();
  [[nodiscard]] bool emitDelegate();
  [[nodiscard]] bool emitThrow();
  [[nodiscard]] bool emitThrowRef();
  [[nodiscard]] bool emitRethrow();
  [[nodiscard]] bool emitEnd();
  [[nodiscard]] bool emitBr();
  [[nodiscard]] bool emitBrIf();
  [[nodiscard]] bool emitBrTable();
  [[nodiscard]] bool emitDrop();
  [[nodiscard]] bool emitReturn();

  // A flag passed to emitCallArgs, describing how the value stack is laid out.
  enum class CalleeOnStack {
    // After the arguments to the call, there is a callee pushed onto value
    // stack.  This is only the case for callIndirect.  To get the arguments to
    // the call, emitCallArgs has to reach one element deeper into the value
    // stack, to skip the callee.
    True,

    // No callee on the stack.
    False
  };

  // The typename T for emitCallArgs can be one of the following:
  // NormalCallResults, TailCallResults, or NoCallResults.
  template <typename T>
  [[nodiscard]] bool emitCallArgs(const ValTypeVector& argTypes, T results,
                                  FunctionCall* baselineCall,
                                  CalleeOnStack calleeOnStack);

  [[nodiscard]] bool emitCall();
  [[nodiscard]] bool emitReturnCall();
  [[nodiscard]] bool emitCallIndirect();
  [[nodiscard]] bool emitReturnCallIndirect();
  [[nodiscard]] bool emitUnaryMathBuiltinCall(SymbolicAddress callee,
                                              ValType operandType);
  [[nodiscard]] bool emitGetLocal();
  [[nodiscard]] bool emitSetLocal();
  [[nodiscard]] bool emitTeeLocal();
  [[nodiscard]] bool emitGetGlobal();
  [[nodiscard]] bool emitSetGlobal();
  [[nodiscard]] RegPtr maybeLoadMemoryBaseForAccess(
      RegPtr instance, const MemoryAccessDesc* access);
  [[nodiscard]] RegPtr maybeLoadInstanceForAccess(
      const MemoryAccessDesc* access, const AccessCheck& check);
  [[nodiscard]] RegPtr maybeLoadInstanceForAccess(
      const MemoryAccessDesc* access, const AccessCheck& check,
      RegPtr specific);
  [[nodiscard]] bool emitLoad(ValType type, Scalar::Type viewType);
  [[nodiscard]] bool emitStore(ValType resultType, Scalar::Type viewType);
  [[nodiscard]] bool emitSelect(bool typed);

  template <bool isSetLocal>
  [[nodiscard]] bool emitSetOrTeeLocal(uint32_t slot);

  [[nodiscard]] bool endBlock(ResultType type);
  [[nodiscard]] bool endIfThen(ResultType type);
  [[nodiscard]] bool endIfThenElse(ResultType type);
  [[nodiscard]] bool endTryCatch(ResultType type);
  [[nodiscard]] bool endTryTable(ResultType type);

  void doReturn(ContinuationKind kind);
  void pushReturnValueOfCall(const FunctionCall& call, MIRType type);

  [[nodiscard]] bool pushStackResultsForCall(const ResultType& type,
                                             RegPtr temp, StackResultsLoc* loc);
  void popStackResultsAfterCall(const StackResultsLoc& results,
                                uint32_t stackArgBytes);

  void emitCompareI32(Assembler::Condition compareOp, ValType compareType);
  void emitCompareI64(Assembler::Condition compareOp, ValType compareType);
  void emitCompareF32(Assembler::DoubleCondition compareOp,
                      ValType compareType);
  void emitCompareF64(Assembler::DoubleCondition compareOp,
                      ValType compareType);
  void emitCompareRef(Assembler::Condition compareOp, ValType compareType);

  template <typename CompilerType>
  inline CompilerType& selectCompiler();

  template <typename SourceType, typename DestType>
  inline void emitUnop(void (*op)(MacroAssembler& masm, SourceType rs,
                                  DestType rd));

  template <typename SourceType, typename DestType, typename TempType>
  inline void emitUnop(void (*op)(MacroAssembler& masm, SourceType rs,
                                  DestType rd, TempType temp));

  template <typename SourceType, typename DestType, typename ImmType>
  inline void emitUnop(ImmType immediate, void (*op)(MacroAssembler&, ImmType,
                                                     SourceType, DestType));

  template <typename CompilerType, typename RegType>
  inline void emitUnop(void (*op)(CompilerType& compiler, RegType rsd));

  template <typename RegType, typename TempType>
  inline void emitUnop(void (*op)(BaseCompiler& bc, RegType rsd, TempType rt),
                       TempType (*getSpecializedTemp)(BaseCompiler& bc));

  template <typename CompilerType, typename RhsType, typename LhsDestType>
  inline void emitBinop(void (*op)(CompilerType& masm, RhsType src,
                                   LhsDestType srcDest));

  template <typename RhsDestType, typename LhsType>
  inline void emitBinop(void (*op)(MacroAssembler& masm, RhsDestType src,
                                   LhsType srcDest, RhsDestOp));

  template <typename RhsType, typename LhsDestType, typename TempType>
  inline void emitBinop(void (*)(MacroAssembler& masm, RhsType rs,
                                 LhsDestType rsd, TempType temp));

  template <typename RhsType, typename LhsDestType, typename TempType1,
            typename TempType2>
  inline void emitBinop(void (*)(MacroAssembler& masm, RhsType rs,
                                 LhsDestType rsd, TempType1 temp1,
                                 TempType2 temp2));

  template <typename RhsType, typename LhsDestType, typename ImmType>
  inline void emitBinop(ImmType immediate, void (*op)(MacroAssembler&, ImmType,
                                                      RhsType, LhsDestType));

  template <typename RhsType, typename LhsDestType, typename ImmType,
            typename TempType1, typename TempType2>
  inline void emitBinop(ImmType immediate,
                        void (*op)(MacroAssembler&, ImmType, RhsType,
                                   LhsDestType, TempType1 temp1,
                                   TempType2 temp2));

  template <typename CompilerType1, typename CompilerType2, typename RegType,
            typename ImmType>
  inline void emitBinop(void (*op)(CompilerType1& compiler1, RegType rs,
                                   RegType rd),
                        void (*opConst)(CompilerType2& compiler2, ImmType c,
                                        RegType rd),
                        RegType (BaseCompiler::*rhsPopper)() = nullptr);

  template <typename CompilerType, typename ValType>
  inline void emitTernary(void (*op)(CompilerType&, ValType src0, ValType src1,
                                     ValType srcDest));

  template <typename CompilerType, typename ValType>
  inline void emitTernary(void (*op)(CompilerType&, ValType src0, ValType src1,
                                     ValType srcDest, ValType temp));

  template <typename CompilerType, typename ValType>
  inline void emitTernaryResultLast(void (*op)(CompilerType&, ValType src0,
                                               ValType src1, ValType srcDest));

  template <typename R>
  [[nodiscard]] inline bool emitInstanceCallOp(
      const SymbolicAddressSignature& fn, R reader);

  template <typename A1, typename R>
  [[nodiscard]] inline bool emitInstanceCallOp(
      const SymbolicAddressSignature& fn, R reader);

  template <typename A1, typename A2, typename R>
  [[nodiscard]] inline bool emitInstanceCallOp(
      const SymbolicAddressSignature& fn, R reader);

  void emitMultiplyI64();
  void emitQuotientI32();
  void emitQuotientU32();
  void emitRemainderI32();
  void emitRemainderU32();
#ifdef RABALDR_INT_DIV_I64_CALLOUT
  [[nodiscard]] bool emitDivOrModI64BuiltinCall(SymbolicAddress callee,
                                                ValType operandType);
#else
  void emitQuotientI64();
  void emitQuotientU64();
  void emitRemainderI64();
  void emitRemainderU64();
#endif
  void emitRotrI64();
  void emitRotlI64();
  void emitEqzI32();
  void emitEqzI64();
  template <TruncFlags flags>
  [[nodiscard]] bool emitTruncateF32ToI32();
  template <TruncFlags flags>
  [[nodiscard]] bool emitTruncateF64ToI32();
#ifdef RABALDR_FLOAT_TO_I64_CALLOUT
  [[nodiscard]] bool emitConvertFloatingToInt64Callout(SymbolicAddress callee,
                                                       ValType operandType,
                                                       ValType resultType);
#else
  template <TruncFlags flags>
  [[nodiscard]] bool emitTruncateF32ToI64();
  template <TruncFlags flags>
  [[nodiscard]] bool emitTruncateF64ToI64();
#endif
  void emitExtendI64_8();
  void emitExtendI64_16();
  void emitExtendI64_32();
  void emitExtendI32ToI64();
  void emitExtendU32ToI64();
#ifdef RABALDR_I64_TO_FLOAT_CALLOUT
  [[nodiscard]] bool emitConvertInt64ToFloatingCallout(SymbolicAddress callee,
                                                       ValType operandType,
                                                       ValType resultType);
#else
  void emitConvertU64ToF32();
  void emitConvertU64ToF64();
#endif
  void emitRound(RoundingMode roundingMode, ValType operandType);

  // Generate a call to the instance function denoted by `builtin`, passing as
  // args the top elements of the compiler's value stack and optionally an
  // Instance* too.  The relationship between the top of stack and arg
  // ordering is as follows.  If the value stack looks like this:
  //
  //   A  <- least recently pushed
  //   B
  //   C  <- most recently pushed
  //
  // then the called function is expected to have signature [if an Instance*
  // is also to be passed]:
  //
  //   static Instance::foo(Instance*, A, B, C)
  //
  // and the SymbolicAddressSignature::argTypes array will be
  //
  //   {_PTR, _A, _B, _C, _END}  // _PTR is for the Instance*
  //
  // (see WasmBuiltins.cpp).  In short, the most recently pushed value is the
  // rightmost argument to the function.
  [[nodiscard]] bool emitInstanceCall(const SymbolicAddressSignature& builtin);

  [[nodiscard]] bool emitMemoryGrow();
  [[nodiscard]] bool emitMemorySize();

  [[nodiscard]] bool emitRefFunc();
  [[nodiscard]] bool emitRefNull();
  [[nodiscard]] bool emitRefIsNull();
#ifdef ENABLE_WASM_GC
  [[nodiscard]] bool emitRefAsNonNull();
  [[nodiscard]] bool emitBrOnNull();
  [[nodiscard]] bool emitBrOnNonNull();
  [[nodiscard]] bool emitCallRef();
  [[nodiscard]] bool emitReturnCallRef();
#endif

  [[nodiscard]] bool emitAtomicCmpXchg(ValType type, Scalar::Type viewType);
  [[nodiscard]] bool emitAtomicLoad(ValType type, Scalar::Type viewType);
  [[nodiscard]] bool emitAtomicRMW(ValType type, Scalar::Type viewType,
                                   AtomicOp op);
  [[nodiscard]] bool emitAtomicStore(ValType type, Scalar::Type viewType);
  [[nodiscard]] bool emitWait(ValType type, uint32_t byteSize);
  [[nodiscard]] bool atomicWait(ValType type, MemoryAccessDesc* access);
  [[nodiscard]] bool emitWake();
  [[nodiscard]] bool atomicWake(MemoryAccessDesc* access);
  [[nodiscard]] bool emitFence();
  [[nodiscard]] bool emitAtomicXchg(ValType type, Scalar::Type viewType);
  [[nodiscard]] bool emitMemInit();
  [[nodiscard]] bool emitMemCopy();
  [[nodiscard]] bool memCopyCall(uint32_t dstMemIndex, uint32_t srcMemIndex);
  void memCopyInlineM32();
  [[nodiscard]] bool emitTableCopy();
  [[nodiscard]] bool emitDataOrElemDrop(bool isData);
  [[nodiscard]] bool emitMemFill();
  [[nodiscard]] bool memFillCall(uint32_t memoryIndex);
  void memFillInlineM32();
  [[nodiscard]] bool emitTableInit();
  [[nodiscard]] bool emitTableFill();
  [[nodiscard]] bool emitMemDiscard();
  [[nodiscard]] bool emitTableGet();
  [[nodiscard]] bool emitTableGrow();
  [[nodiscard]] bool emitTableSet();
  [[nodiscard]] bool emitTableSize();

  void emitTableBoundsCheck(uint32_t tableIndex, RegI32 index, RegPtr instance);
  [[nodiscard]] bool emitTableGetAnyRef(uint32_t tableIndex);
  [[nodiscard]] bool emitTableSetAnyRef(uint32_t tableIndex);

#ifdef ENABLE_WASM_GC
  [[nodiscard]] bool emitStructNew();
  [[nodiscard]] bool emitStructNewDefault();
  [[nodiscard]] bool emitStructGet(FieldWideningOp wideningOp);
  [[nodiscard]] bool emitStructSet();
  [[nodiscard]] bool emitArrayNew();
  [[nodiscard]] bool emitArrayNewFixed();
  [[nodiscard]] bool emitArrayNewDefault();
  [[nodiscard]] bool emitArrayNewData();
  [[nodiscard]] bool emitArrayNewElem();
  [[nodiscard]] bool emitArrayInitData();
  [[nodiscard]] bool emitArrayInitElem();
  [[nodiscard]] bool emitArrayGet(FieldWideningOp wideningOp);
  [[nodiscard]] bool emitArraySet();
  [[nodiscard]] bool emitArrayLen();
  [[nodiscard]] bool emitArrayCopy();
  [[nodiscard]] bool emitArrayFill();
  [[nodiscard]] bool emitRefI31();
  [[nodiscard]] bool emitI31Get(FieldWideningOp wideningOp);
  [[nodiscard]] bool emitRefTest(bool nullable);
  [[nodiscard]] bool emitRefCast(bool nullable);
  [[nodiscard]] bool emitBrOnCastCommon(bool onSuccess,
                                        uint32_t labelRelativeDepth,
                                        const ResultType& labelType,
                                        RefType sourceType, RefType destType);
  [[nodiscard]] bool emitBrOnCast(bool onSuccess);
  [[nodiscard]] bool emitAnyConvertExtern();
  [[nodiscard]] bool emitExternConvertAny();

  // Utility classes/methods to add trap information related to
  // null pointer dereferences/accesses.
  struct NoNullCheck {
    static void emitNullCheck(BaseCompiler* bc, RegRef rp) {}
    static void emitTrapSite(BaseCompiler* bc, FaultingCodeOffset fco,
                             TrapMachineInsn tmi) {}
  };
  struct SignalNullCheck {
    static void emitNullCheck(BaseCompiler* bc, RegRef rp);
    static void emitTrapSite(BaseCompiler* bc, FaultingCodeOffset fco,
                             TrapMachineInsn tmi);
  };

  // Load a pointer to the TypeDefInstanceData for a given type index
  RegPtr loadTypeDefInstanceData(uint32_t typeIndex);
  // Load a pointer to the SuperTypeVector for a given type index
  RegPtr loadSuperTypeVector(uint32_t typeIndex);

  // Emits allocation code for a GC struct. The struct may have an out-of-line
  // data area; if so, `isOutlineStruct` will be true and `outlineBase` will be
  // allocated and must be freed.
  template <bool ZeroFields>
  bool emitStructAlloc(uint32_t typeIndex, RegRef* object,
                       bool* isOutlineStruct, RegPtr* outlineBase);
  // Emits allocation code for a dynamically-sized GC array.
  template <bool ZeroFields>
  bool emitArrayAlloc(uint32_t typeIndex, RegRef object, RegI32 numElements,
                      uint32_t elemSize);
  // Emits allocation code for a fixed-size GC array.
  template <bool ZeroFields>
  bool emitArrayAllocFixed(uint32_t typeIndex, RegRef object,
                           uint32_t numElements, uint32_t elemSize);

  template <typename NullCheckPolicy>
  RegPtr emitGcArrayGetData(RegRef rp);
  template <typename NullCheckPolicy>
  RegI32 emitGcArrayGetNumElements(RegRef rp);
  void emitGcArrayBoundsCheck(RegI32 index, RegI32 numElements);
  template <typename T, typename NullCheckPolicy>
  void emitGcGet(StorageType type, FieldWideningOp wideningOp, const T& src);
  template <typename T, typename NullCheckPolicy>
  void emitGcSetScalar(const T& dst, StorageType type, AnyReg value);

  BranchIfRefSubtypeRegisters allocRegistersForBranchIfRefSubtype(
      RefType destType);
  void freeRegistersForBranchIfRefSubtype(
      const BranchIfRefSubtypeRegisters& regs);

  // Write `value` to wasm struct `object`, at `areaBase + areaOffset`.  The
  // caller must decide on the in- vs out-of-lineness before the call and set
  // the latter two accordingly; this routine does not take that into account.
  // The value in `object` is unmodified, but `areaBase` and `value` may get
  // trashed.
  template <typename NullCheckPolicy>
  [[nodiscard]] bool emitGcStructSet(RegRef object, RegPtr areaBase,
                                     uint32_t areaOffset, StorageType type,
                                     AnyReg value,
                                     PreBarrierKind preBarrierKind);

  [[nodiscard]] bool emitGcArraySet(RegRef object, RegPtr data, RegI32 index,
                                    const ArrayType& array, AnyReg value,
                                    PreBarrierKind preBarrierKind);
#endif  // ENABLE_WASM_GC

#ifdef ENABLE_WASM_SIMD
  void emitVectorAndNot();
#  ifdef ENABLE_WASM_RELAXED_SIMD
  void emitDotI8x16I7x16AddS();
#  endif

  void loadSplat(MemoryAccessDesc* access);
  void loadZero(MemoryAccessDesc* access);
  void loadExtend(MemoryAccessDesc* access, Scalar::Type viewType);
  void loadLane(MemoryAccessDesc* access, uint32_t laneIndex);
  void storeLane(MemoryAccessDesc* access, uint32_t laneIndex);

  [[nodiscard]] bool emitLoadSplat(Scalar::Type viewType);
  [[nodiscard]] bool emitLoadZero(Scalar::Type viewType);
  [[nodiscard]] bool emitLoadExtend(Scalar::Type viewType);
  [[nodiscard]] bool emitLoadLane(uint32_t laneSize);
  [[nodiscard]] bool emitStoreLane(uint32_t laneSize);
  [[nodiscard]] bool emitVectorShuffle();
  [[nodiscard]] bool emitVectorLaneSelect();
#  if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
  [[nodiscard]] bool emitVectorShiftRightI64x2();
#  endif
#endif
  [[nodiscard]] bool emitCallBuiltinModuleFunc();
};

}  // namespace wasm
}  // namespace js

#endif  // wasm_wasm_baseline_object_h

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

// This is an INTERNAL header for Wasm baseline compiler: CPU stack frame,
// stack maps, and associated logic.

#ifndef wasm_wasm_baseline_frame_h
#define wasm_wasm_baseline_frame_h

#include "wasm/WasmBaselineCompile.h"  // For BaseLocalIter
#include "wasm/WasmBCDefs.h"
#include "wasm/WasmBCRegDefs.h"
#include "wasm/WasmBCStk.h"
#include "wasm/WasmConstants.h"  // For MaxFrameSize

// [SMDOC] Wasm baseline compiler's stack frame.
//
// For background, see "Wasm's ABIs" in WasmFrame.h, the following should never
// be in conflict with that.
//
// The stack frame has four parts ("below" means at lower addresses):
//
//  - the Frame element;
//  - the Local area, including the DebugFrame element and possibly a spilled
//    pointer to stack results, if any; allocated below the header with various
//    forms of alignment;
//  - the Dynamic area, comprising the temporary storage the compiler uses for
//    register spilling, allocated below the Local area;
//  - the Arguments area, comprising memory allocated for outgoing calls,
//    allocated below the Dynamic area.
//
//                +==============================+
//                |    Incoming stack arg        |
//                |    ...                       |
// -------------  +==============================+
//                |    Frame (fixed size)        |
// -------------  +==============================+ <-------------------- FP
//         ^      |    DebugFrame (optional)     |    ^  ^             ^^
//   localSize    |    Register arg local        |    |  |             ||
//         |      |    ...                       |    |  |     framePushed
//         |      |    Register stack result ptr?|    |  |             ||
//         |      |    Non-arg local             |    |  |             ||
//         |      |    ...                       |    |  |             ||
//         |      |    (padding)                 |    |  |             ||
//         |      |    Instance pointer          |    |  |             ||
//         |      +------------------------------+    |  |             ||
//         v      |    (padding)                 |    |  v             ||
// -------------  +==============================+ currentStackHeight  ||
//         ^      |    Dynamic (variable size)   |    |                ||
//  dynamicSize   |    ...                       |    |                ||
//         v      |    ...                       |    v                ||
// -------------  |    (free space, sometimes)   | ---------           v|
//                +==============================+ <----- SP not-during calls
//                |    Arguments (sometimes)     |                      |
//                |    ...                       |                      v
//                +==============================+ <----- SP during calls
//
// The Frame is addressed off the stack pointer.  masm.framePushed() is always
// correct, and masm.getStackPointer() + masm.framePushed() always addresses the
// Frame, with the DebugFrame optionally below it.
//
// The Local area (including the DebugFrame and, if needed, the spilled value of
// the stack results area pointer) is laid out by BaseLocalIter and is allocated
// and deallocated by standard prologue and epilogue functions that manipulate
// the stack pointer, but it is accessed via BaseStackFrame.
//
// The Dynamic area is maintained by and accessed via BaseStackFrame.  On some
// systems (such as ARM64), the Dynamic memory may be allocated in chunks
// because the SP needs a specific alignment, and in this case there will
// normally be some free space directly above the SP.  The stack height does not
// include the free space, it reflects the logically used space only.
//
// The Dynamic area is where space for stack results is allocated when calling
// functions that return results on the stack.  If a function has stack results,
// a pointer to the low address of the stack result area is passed as an
// additional argument, according to the usual ABI.  See
// ABIResultIter::HasStackResults.
//
// The Arguments area is allocated and deallocated via BaseStackFrame (see
// comments later) but is accessed directly off the stack pointer.

namespace js {
namespace wasm {

using namespace js::jit;

// Abstraction of the height of the stack frame, to avoid type confusion.

class StackHeight {
  friend class BaseStackFrameAllocator;

  uint32_t height;

 public:
  explicit StackHeight(uint32_t h) : height(h) {}
  static StackHeight Invalid() { return StackHeight(UINT32_MAX); }
  bool isValid() const { return height != UINT32_MAX; }
  bool operator==(StackHeight rhs) const {
    MOZ_ASSERT(isValid() && rhs.isValid());
    return height == rhs.height;
  }
  bool operator!=(StackHeight rhs) const { return !(*this == rhs); }
};

// Abstraction for where multi-value results go on the machine stack.

class StackResultsLoc {
  uint32_t bytes_;
  size_t count_;
  Maybe<uint32_t> height_;

 public:
  StackResultsLoc() : bytes_(0), count_(0){};
  StackResultsLoc(uint32_t bytes, size_t count, uint32_t height)
      : bytes_(bytes), count_(count), height_(Some(height)) {
    MOZ_ASSERT(bytes != 0);
    MOZ_ASSERT(count != 0);
    MOZ_ASSERT(height != 0);
  }

  uint32_t bytes() const { return bytes_; }
  uint32_t count() const { return count_; }
  uint32_t height() const { return height_.value(); }

  bool hasStackResults() const { return bytes() != 0; }
  StackResults stackResults() const {
    return hasStackResults() ? StackResults::HasStackResults
                             : StackResults::NoStackResults;
  }
};

// Abstraction of the baseline compiler's stack frame (except for the Frame /
// DebugFrame parts).  See comments above for more.  Remember, "below" on the
// stack means at lower addresses.
//
// The abstraction is split into two parts: BaseStackFrameAllocator is
// responsible for allocating and deallocating space on the stack and for
// performing computations that are affected by how the allocation is performed;
// BaseStackFrame then provides a pleasant interface for stack frame management.

class BaseStackFrameAllocator {
  MacroAssembler& masm;

#ifdef RABALDR_CHUNKY_STACK
  // On platforms that require the stack pointer to be aligned on a boundary
  // greater than the typical stack item (eg, ARM64 requires 16-byte alignment
  // but items are 8 bytes), allocate stack memory in chunks, and use a
  // separate stack height variable to track the effective stack pointer
  // within the allocated area.  Effectively, there's a variable amount of
  // free space directly above the stack pointer.  See diagram above.

  // The following must be true in order for the stack height to be
  // predictable at control flow joins:
  //
  // - The Local area is always aligned according to WasmStackAlignment, ie,
  //   masm.framePushed() % WasmStackAlignment is zero after allocating
  //   locals.
  //
  // - ChunkSize is always a multiple of WasmStackAlignment.
  //
  // - Pushing and popping are always in units of ChunkSize (hence preserving
  //   alignment).
  //
  // - The free space on the stack (masm.framePushed() - currentStackHeight_)
  //   is a predictable (nonnegative) amount.

  // As an optimization, we pre-allocate some space on the stack, the size of
  // this allocation is InitialChunk and it must be a multiple of ChunkSize.
  // It is allocated as part of the function prologue and deallocated as part
  // of the epilogue, along with the locals.
  //
  // If ChunkSize is too large then we risk overflowing the stack on simple
  // recursions with few live values where stack overflow should not be a
  // risk; if it is too small we spend too much time adjusting the stack
  // pointer.
  //
  // Good values for ChunkSize are the subject of future empirical analysis;
  // eight words is just an educated guess.

  static constexpr uint32_t ChunkSize = 8 * sizeof(void*);
  static constexpr uint32_t InitialChunk = ChunkSize;

  // The current logical height of the frame is
  //   currentStackHeight_ = localSize_ + dynamicSize
  // where dynamicSize is not accounted for explicitly and localSize_ also
  // includes size for the DebugFrame.
  //
  // The allocated size of the frame, provided by masm.framePushed(), is usually
  // larger than currentStackHeight_, notably at the beginning of execution when
  // we've allocated InitialChunk extra space.

  uint32_t currentStackHeight_;
#endif

  // Size of the Local area in bytes (stable after BaseCompiler::init() has
  // called BaseStackFrame::setupLocals(), which in turn calls
  // BaseStackFrameAllocator::setLocalSize()), always rounded to the proper
  // stack alignment.  The Local area is then allocated in beginFunction(),
  // following the allocation of the Header.  See onFixedStackAllocated()
  // below.

  uint32_t localSize_;

 protected:
  ///////////////////////////////////////////////////////////////////////////
  //
  // Initialization

  explicit BaseStackFrameAllocator(MacroAssembler& masm)
      : masm(masm),
#ifdef RABALDR_CHUNKY_STACK
        currentStackHeight_(0),
#endif
        localSize_(UINT32_MAX) {
  }

 protected:
  //////////////////////////////////////////////////////////////////////
  //
  // The Local area - the static part of the frame.

  // Record the size of the Local area, once it is known.

  void setLocalSize(uint32_t localSize) {
    MOZ_ASSERT(localSize == AlignBytes(localSize, sizeof(void*)),
               "localSize_ should be aligned to at least a pointer");
    MOZ_ASSERT(localSize_ == UINT32_MAX);
    localSize_ = localSize;
  }

  // Record the current stack height, after it has become stable in
  // beginFunction().  See also BaseStackFrame::onFixedStackAllocated().

  void onFixedStackAllocated() {
    MOZ_ASSERT(localSize_ != UINT32_MAX);
#ifdef RABALDR_CHUNKY_STACK
    currentStackHeight_ = localSize_;
#endif
  }

 public:
  // The fixed amount of memory, in bytes, allocated on the stack below the
  // Header for purposes such as locals and other fixed values.  Includes all
  // necessary alignment, and on ARM64 also the initial chunk for the working
  // stack memory.

  uint32_t fixedAllocSize() const {
    MOZ_ASSERT(localSize_ != UINT32_MAX);
#ifdef RABALDR_CHUNKY_STACK
    return localSize_ + InitialChunk;
#else
    return localSize_;
#endif
  }

#ifdef RABALDR_CHUNKY_STACK
  // The allocated frame size is frequently larger than the logical stack
  // height; we round up to a chunk boundary, and special case the initial
  // chunk.
  uint32_t framePushedForHeight(uint32_t logicalHeight) {
    if (logicalHeight <= fixedAllocSize()) {
      return fixedAllocSize();
    }
    return fixedAllocSize() +
           AlignBytes(logicalHeight - fixedAllocSize(), ChunkSize);
  }
#endif

 protected:
  //////////////////////////////////////////////////////////////////////
  //
  // The Dynamic area - the dynamic part of the frame, for spilling and saving
  // intermediate values.

  // Offset off of sp_ for the slot at stack area location `offset`.

  int32_t stackOffset(int32_t offset) {
    MOZ_ASSERT(offset > 0);
    return masm.framePushed() - offset;
  }

  uint32_t computeHeightWithStackResults(StackHeight stackBase,
                                         uint32_t stackResultBytes) {
    MOZ_ASSERT(stackResultBytes);
    MOZ_ASSERT(currentStackHeight() >= stackBase.height);
    return stackBase.height + stackResultBytes;
  }

#ifdef RABALDR_CHUNKY_STACK
  void pushChunkyBytes(uint32_t bytes) {
    checkChunkyInvariants();
    uint32_t freeSpace = masm.framePushed() - currentStackHeight_;
    if (freeSpace < bytes) {
      uint32_t bytesToReserve = AlignBytes(bytes - freeSpace, ChunkSize);
      MOZ_ASSERT(bytesToReserve + freeSpace >= bytes);
      masm.reserveStack(bytesToReserve);
    }
    currentStackHeight_ += bytes;
    checkChunkyInvariants();
  }

  void popChunkyBytes(uint32_t bytes) {
    checkChunkyInvariants();
    currentStackHeight_ -= bytes;
    // Sometimes, popChunkyBytes() is used to pop a larger area, as when we drop
    // values consumed by a call, and we may need to drop several chunks.  But
    // never drop the initial chunk.  Crucially, the amount we drop is always an
    // integral number of chunks.
    uint32_t freeSpace = masm.framePushed() - currentStackHeight_;
    if (freeSpace >= ChunkSize) {
      uint32_t targetAllocSize = framePushedForHeight(currentStackHeight_);
      uint32_t amountToFree = masm.framePushed() - targetAllocSize;
      MOZ_ASSERT(amountToFree % ChunkSize == 0);
      if (amountToFree) {
        masm.freeStack(amountToFree);
      }
    }
    checkChunkyInvariants();
  }
#endif

  uint32_t currentStackHeight() const {
#ifdef RABALDR_CHUNKY_STACK
    return currentStackHeight_;
#else
    return masm.framePushed();
#endif
  }

 private:
#ifdef RABALDR_CHUNKY_STACK
  void checkChunkyInvariants() {
    MOZ_ASSERT(masm.framePushed() >= fixedAllocSize());
    MOZ_ASSERT(masm.framePushed() >= currentStackHeight_);
    MOZ_ASSERT(masm.framePushed() == fixedAllocSize() ||
               masm.framePushed() - currentStackHeight_ < ChunkSize);
    MOZ_ASSERT((masm.framePushed() - localSize_) % ChunkSize == 0);
  }
#endif

  // For a given stack height, return the appropriate size of the allocated
  // frame.

  uint32_t framePushedForHeight(StackHeight stackHeight) {
#ifdef RABALDR_CHUNKY_STACK
    // A more complicated adjustment is needed.
    return framePushedForHeight(stackHeight.height);
#else
    // The allocated frame size equals the stack height.
    return stackHeight.height;
#endif
  }

 public:
  // The current height of the stack area, not necessarily zero-based, in a
  // type-safe way.

  StackHeight stackHeight() const { return StackHeight(currentStackHeight()); }

  // Set the frame height to a previously recorded value.

  void setStackHeight(StackHeight amount) {
#ifdef RABALDR_CHUNKY_STACK
    currentStackHeight_ = amount.height;
    masm.setFramePushed(framePushedForHeight(amount));
    checkChunkyInvariants();
#else
    masm.setFramePushed(amount.height);
#endif
  }

  // The current height of the dynamic part of the stack area (ie, the backing
  // store for the evaluation stack), zero-based.

  uint32_t dynamicHeight() const { return currentStackHeight() - localSize_; }

  // Before branching to an outer control label, pop the execution stack to
  // the level expected by that region, but do not update masm.framePushed()
  // as that will happen as compilation leaves the block.
  //
  // Note these operate directly on the stack pointer register.

  void popStackBeforeBranch(StackHeight destStackHeight,
                            uint32_t stackResultBytes) {
    uint32_t framePushedHere = masm.framePushed();
    StackHeight heightThere =
        StackHeight(destStackHeight.height + stackResultBytes);
    uint32_t framePushedThere = framePushedForHeight(heightThere);
    if (framePushedHere > framePushedThere) {
      masm.addToStackPtr(Imm32(framePushedHere - framePushedThere));
    }
  }

  void popStackBeforeBranch(StackHeight destStackHeight, ResultType type) {
    popStackBeforeBranch(destStackHeight,
                         ABIResultIter::MeasureStackBytes(type));
  }

  // Given that there are |stackParamSize| bytes on the dynamic stack
  // corresponding to the stack results, return the stack height once these
  // parameters are popped.

  StackHeight stackResultsBase(uint32_t stackParamSize) {
    return StackHeight(currentStackHeight() - stackParamSize);
  }

  // For most of WebAssembly, adjacent instructions have fallthrough control
  // flow between them, which allows us to simply thread the current stack
  // height through the compiler.  There are two exceptions to this rule: when
  // leaving a block via dead code, and when entering the "else" arm of an "if".
  // In these cases, the stack height is the block entry height, plus any stack
  // values (results in the block exit case, parameters in the else entry case).

  void resetStackHeight(StackHeight destStackHeight, ResultType type) {
    uint32_t height = destStackHeight.height;
    height += ABIResultIter::MeasureStackBytes(type);
    setStackHeight(StackHeight(height));
  }

  // Return offset of stack result.

  uint32_t locateStackResult(const ABIResult& result, StackHeight stackBase,
                             uint32_t stackResultBytes) {
    MOZ_ASSERT(result.onStack());
    MOZ_ASSERT(result.stackOffset() + result.size() <= stackResultBytes);
    uint32_t end = computeHeightWithStackResults(stackBase, stackResultBytes);
    return end - result.stackOffset();
  }

 public:
  //////////////////////////////////////////////////////////////////////
  //
  // The Argument area - for outgoing calls.
  //
  // We abstract these operations as an optimization: we can merge the freeing
  // of the argument area and dropping values off the stack after a call.  But
  // they always amount to manipulating the real stack pointer by some amount.
  //
  // Note that we do not update currentStackHeight_ for this; the frame does
  // not know about outgoing arguments.  But we do update framePushed(), so we
  // can still index into the frame below the outgoing arguments area.

  // This is always equivalent to a masm.reserveStack() call.

  void allocArgArea(size_t argSize) {
    if (argSize) {
      masm.reserveStack(argSize);
    }
  }

  // This frees the argument area allocated by allocArgArea(), and `argSize`
  // must be equal to the `argSize` argument to allocArgArea().  In addition
  // we drop some values from the frame, corresponding to the values that were
  // consumed by the call.

  void freeArgAreaAndPopBytes(size_t argSize, size_t dropSize) {
#ifdef RABALDR_CHUNKY_STACK
    // Freeing the outgoing arguments and freeing the consumed values have
    // different semantics here, which is why the operation is split.
    if (argSize) {
      masm.freeStack(argSize);
    }
    popChunkyBytes(dropSize);
#else
    if (argSize + dropSize) {
      masm.freeStack(argSize + dropSize);
    }
#endif
  }
};

class BaseStackFrame final : public BaseStackFrameAllocator {
  MacroAssembler& masm;

  // The largest observed value of masm.framePushed(), ie, the size of the
  // stack frame.  Read this for its true value only when code generation is
  // finished.
  uint32_t maxFramePushed_;

  // Patch point where we check for stack overflow.
  CodeOffset stackAddOffset_;

  // Low byte offset of pointer to stack results, if any.
  Maybe<int32_t> stackResultsPtrOffset_;

  // The offset of instance pointer.
  uint32_t instancePointerOffset_;

  // Low byte offset of local area for true locals (not parameters).
  uint32_t varLow_;

  // High byte offset + 1 of local area for true locals.
  uint32_t varHigh_;

  // The stack pointer, cached for brevity.
  RegisterOrSP sp_;

 public:
  explicit BaseStackFrame(MacroAssembler& masm)
      : BaseStackFrameAllocator(masm),
        masm(masm),
        maxFramePushed_(0),
        stackAddOffset_(0),
        instancePointerOffset_(UINT32_MAX),
        varLow_(UINT32_MAX),
        varHigh_(UINT32_MAX),
        sp_(masm.getStackPointer()) {}

  ///////////////////////////////////////////////////////////////////////////
  //
  // Stack management and overflow checking

  // This must be called once beginFunction has allocated space for the Header
  // (the Frame and DebugFrame) and the Local area, and will record the current
  // frame size for internal use by the stack abstractions.

  void onFixedStackAllocated() {
    maxFramePushed_ = masm.framePushed();
    BaseStackFrameAllocator::onFixedStackAllocated();
  }

  // We won't know until after we've generated code how big the frame will be
  // (we may need arbitrary spill slots and outgoing param slots) so emit a
  // patchable add that is patched in endFunction().
  //
  // Note the platform scratch register may be used by branchPtr(), so
  // generally tmp must be something else.

  void checkStack(Register tmp, BytecodeOffset trapOffset) {
    stackAddOffset_ = masm.sub32FromStackPtrWithPatch(tmp);
    Label ok;
    masm.branchPtr(Assembler::Below,
                   Address(InstanceReg, wasm::Instance::offsetOfStackLimit()),
                   tmp, &ok);
    masm.wasmTrap(Trap::StackOverflow, trapOffset);
    masm.bind(&ok);
  }

  void patchCheckStack() {
    masm.patchSub32FromStackPtr(stackAddOffset_,
                                Imm32(int32_t(maxFramePushed_)));
  }

  // Very large frames are implausible, probably an attack.

  bool checkStackHeight() { return maxFramePushed_ <= MaxFrameSize; }

  ///////////////////////////////////////////////////////////////////////////
  //
  // Local area

  struct Local {
    // Type of the value.
    const MIRType type;

    // Byte offset from Frame "into" the locals, ie positive for true locals
    // and negative for incoming args that read directly from the arg area.
    // It assumes the stack is growing down and that locals are on the stack
    // at lower addresses than Frame, and is the offset from Frame of the
    // lowest-addressed byte of the local.
    const int32_t offs;

    Local(MIRType type, int32_t offs) : type(type), offs(offs) {}

    bool isStackArgument() const { return offs < 0; }
  };

  // Profiling shows that the number of parameters and locals frequently
  // touches or exceeds 8.  So 16 seems like a reasonable starting point.
  using LocalVector = Vector<Local, 16, SystemAllocPolicy>;

  // Initialize `localInfo` based on the types of `locals` and `args`.
  [[nodiscard]] bool setupLocals(const ValTypeVector& locals,
                                 const ArgTypeVector& args, bool debugEnabled,
                                 LocalVector* localInfo) {
    if (!localInfo->reserve(locals.length())) {
      return false;
    }

    DebugOnly<uint32_t> index = 0;
    BaseLocalIter i(locals, args, debugEnabled);
    for (; !i.done() && i.index() < args.lengthWithoutStackResults(); i++) {
      MOZ_ASSERT(i.isArg());
      MOZ_ASSERT(i.index() == index);
      localInfo->infallibleEmplaceBack(i.mirType(), i.frameOffset());
      index++;
    }

    varLow_ = i.frameSize();
    for (; !i.done(); i++) {
      MOZ_ASSERT(!i.isArg());
      MOZ_ASSERT(i.index() == index);
      localInfo->infallibleEmplaceBack(i.mirType(), i.frameOffset());
      index++;
    }
    varHigh_ = i.frameSize();

    // Reserve an additional stack slot for the instance pointer.
    const uint32_t pointerAlignedVarHigh = AlignBytes(varHigh_, sizeof(void*));
    const uint32_t localSize = pointerAlignedVarHigh + sizeof(void*);
    instancePointerOffset_ = localSize;

    setLocalSize(AlignBytes(localSize, WasmStackAlignment));

    if (args.hasSyntheticStackResultPointerArg()) {
      stackResultsPtrOffset_ = Some(i.stackResultPointerOffset());
    }

    return true;
  }

  void zeroLocals(BaseRegAlloc* ra);

  Address addressOfLocal(const Local& local, uint32_t additionalOffset = 0) {
    if (local.isStackArgument()) {
      return Address(FramePointer,
                     stackArgumentOffsetFromFp(local) + additionalOffset);
    }
    return Address(sp_, localOffsetFromSp(local) + additionalOffset);
  }

  void loadLocalI32(const Local& src, RegI32 dest) {
    masm.load32(addressOfLocal(src), dest);
  }

#ifndef JS_PUNBOX64
  void loadLocalI64Low(const Local& src, RegI32 dest) {
    masm.load32(addressOfLocal(src, INT64LOW_OFFSET), dest);
  }

  void loadLocalI64High(const Local& src, RegI32 dest) {
    masm.load32(addressOfLocal(src, INT64HIGH_OFFSET), dest);
  }
#endif

  void loadLocalI64(const Local& src, RegI64 dest) {
    masm.load64(addressOfLocal(src), dest);
  }

  void loadLocalRef(const Local& src, RegRef dest) {
    masm.loadPtr(addressOfLocal(src), dest);
  }

  void loadLocalF64(const Local& src, RegF64 dest) {
    masm.loadDouble(addressOfLocal(src), dest);
  }

  void loadLocalF32(const Local& src, RegF32 dest) {
    masm.loadFloat32(addressOfLocal(src), dest);
  }

#ifdef ENABLE_WASM_SIMD
  void loadLocalV128(const Local& src, RegV128 dest) {
    masm.loadUnalignedSimd128(addressOfLocal(src), dest);
  }
#endif

  void storeLocalI32(RegI32 src, const Local& dest) {
    masm.store32(src, addressOfLocal(dest));
  }

  void storeLocalI64(RegI64 src, const Local& dest) {
    masm.store64(src, addressOfLocal(dest));
  }

  void storeLocalRef(RegRef src, const Local& dest) {
    masm.storePtr(src, addressOfLocal(dest));
  }

  void storeLocalF64(RegF64 src, const Local& dest) {
    masm.storeDouble(src, addressOfLocal(dest));
  }

  void storeLocalF32(RegF32 src, const Local& dest) {
    masm.storeFloat32(src, addressOfLocal(dest));
  }

#ifdef ENABLE_WASM_SIMD
  void storeLocalV128(RegV128 src, const Local& dest) {
    masm.storeUnalignedSimd128(src, addressOfLocal(dest));
  }
#endif

  // Offset off of sp_ for `local`.
  int32_t localOffsetFromSp(const Local& local) {
    MOZ_ASSERT(!local.isStackArgument());
    return localOffset(local.offs);
  }

  // Offset off of frame pointer for `stack argument`.
  int32_t stackArgumentOffsetFromFp(const Local& local) {
    MOZ_ASSERT(local.isStackArgument());
    return -local.offs;
  }

  // The incoming stack result area pointer is for stack results of the function
  // being compiled.
  void loadIncomingStackResultAreaPtr(RegPtr reg) {
    const int32_t offset = stackResultsPtrOffset_.value();
    Address src = offset < 0 ? Address(FramePointer, -offset)
                             : Address(sp_, stackOffset(offset));
    masm.loadPtr(src, reg);
  }

  void storeIncomingStackResultAreaPtr(RegPtr reg) {
    // If we get here, that means the pointer to the stack results area was
    // passed in as a register, and therefore it will be spilled below the
    // frame, so the offset is a positive height.
    MOZ_ASSERT(stackResultsPtrOffset_.value() > 0);
    masm.storePtr(reg,
                  Address(sp_, stackOffset(stackResultsPtrOffset_.value())));
  }

  void loadInstancePtr(Register dst) {
    masm.loadPtr(Address(sp_, stackOffset(instancePointerOffset_)), dst);
  }

  void storeInstancePtr(Register instance) {
    masm.storePtr(instance, Address(sp_, stackOffset(instancePointerOffset_)));
  }

  int32_t getInstancePtrOffset() { return stackOffset(instancePointerOffset_); }

  // An outgoing stack result area pointer is for stack results of callees of
  // the function being compiled.
  void computeOutgoingStackResultAreaPtr(const StackResultsLoc& results,
                                         RegPtr dest) {
    MOZ_ASSERT(results.height() <= masm.framePushed());
    uint32_t offsetFromSP = masm.framePushed() - results.height();
    masm.moveStackPtrTo(dest);
    if (offsetFromSP) {
      masm.addPtr(Imm32(offsetFromSP), dest);
    }
  }

 private:
  // Offset off of sp_ for a local with offset `offset` from Frame.
  int32_t localOffset(int32_t offset) { return masm.framePushed() - offset; }

 public:
  ///////////////////////////////////////////////////////////////////////////
  //
  // Dynamic area

  static constexpr size_t StackSizeOfPtr = ABIResult::StackSizeOfPtr;
  static constexpr size_t StackSizeOfInt64 = ABIResult::StackSizeOfInt64;
  static constexpr size_t StackSizeOfFloat = ABIResult::StackSizeOfFloat;
  static constexpr size_t StackSizeOfDouble = ABIResult::StackSizeOfDouble;
#ifdef ENABLE_WASM_SIMD
  static constexpr size_t StackSizeOfV128 = ABIResult::StackSizeOfV128;
#endif

  // Pushes the register `r` to the stack. This pushes the full 64-bit width on
  // 64-bit systems, and 32-bits otherwise.
  uint32_t pushGPR(Register r) {
    DebugOnly<uint32_t> stackBefore = currentStackHeight();
#ifdef RABALDR_CHUNKY_STACK
    pushChunkyBytes(StackSizeOfPtr);
    masm.storePtr(r, Address(sp_, stackOffset(currentStackHeight())));
#else
    masm.Push(r);
#endif
    maxFramePushed_ = std::max(maxFramePushed_, masm.framePushed());
    MOZ_ASSERT(stackBefore + StackSizeOfPtr == currentStackHeight());
    return currentStackHeight();
  }

  uint32_t pushFloat32(FloatRegister r) {
    DebugOnly<uint32_t> stackBefore = currentStackHeight();
#ifdef RABALDR_CHUNKY_STACK
    pushChunkyBytes(StackSizeOfFloat);
    masm.storeFloat32(r, Address(sp_, stackOffset(currentStackHeight())));
#else
    masm.Push(r);
#endif
    maxFramePushed_ = std::max(maxFramePushed_, masm.framePushed());
    MOZ_ASSERT(stackBefore + StackSizeOfFloat == currentStackHeight());
    return currentStackHeight();
  }

#ifdef ENABLE_WASM_SIMD
  uint32_t pushV128(RegV128 r) {
    DebugOnly<uint32_t> stackBefore = currentStackHeight();
#  ifdef RABALDR_CHUNKY_STACK
    pushChunkyBytes(StackSizeOfV128);
#  else
    masm.adjustStack(-(int)StackSizeOfV128);
#  endif
    masm.storeUnalignedSimd128(r,
                               Address(sp_, stackOffset(currentStackHeight())));
    maxFramePushed_ = std::max(maxFramePushed_, masm.framePushed());
    MOZ_ASSERT(stackBefore + StackSizeOfV128 == currentStackHeight());
    return currentStackHeight();
  }
#endif

  uint32_t pushDouble(FloatRegister r) {
    DebugOnly<uint32_t> stackBefore = currentStackHeight();
#ifdef RABALDR_CHUNKY_STACK
    pushChunkyBytes(StackSizeOfDouble);
    masm.storeDouble(r, Address(sp_, stackOffset(currentStackHeight())));
#else
    masm.Push(r);
#endif
    maxFramePushed_ = std::max(maxFramePushed_, masm.framePushed());
    MOZ_ASSERT(stackBefore + StackSizeOfDouble == currentStackHeight());
    return currentStackHeight();
  }

  // Pops the stack into the register `r`. This pops the full 64-bit width on
  // 64-bit systems, and 32-bits otherwise.
  void popGPR(Register r) {
    DebugOnly<uint32_t> stackBefore = currentStackHeight();
#ifdef RABALDR_CHUNKY_STACK
    masm.loadPtr(Address(sp_, stackOffset(currentStackHeight())), r);
    popChunkyBytes(StackSizeOfPtr);
#else
    masm.Pop(r);
#endif
    MOZ_ASSERT(stackBefore - StackSizeOfPtr == currentStackHeight());
  }

  void popFloat32(FloatRegister r) {
    DebugOnly<uint32_t> stackBefore = currentStackHeight();
#ifdef RABALDR_CHUNKY_STACK
    masm.loadFloat32(Address(sp_, stackOffset(currentStackHeight())), r);
    popChunkyBytes(StackSizeOfFloat);
#else
    masm.Pop(r);
#endif
    MOZ_ASSERT(stackBefore - StackSizeOfFloat == currentStackHeight());
  }

  void popDouble(FloatRegister r) {
    DebugOnly<uint32_t> stackBefore = currentStackHeight();
#ifdef RABALDR_CHUNKY_STACK
    masm.loadDouble(Address(sp_, stackOffset(currentStackHeight())), r);
    popChunkyBytes(StackSizeOfDouble);
#else
    masm.Pop(r);
#endif
    MOZ_ASSERT(stackBefore - StackSizeOfDouble == currentStackHeight());
  }

#ifdef ENABLE_WASM_SIMD
  void popV128(RegV128 r) {
    DebugOnly<uint32_t> stackBefore = currentStackHeight();
    masm.loadUnalignedSimd128(Address(sp_, stackOffset(currentStackHeight())),
                              r);
#  ifdef RABALDR_CHUNKY_STACK
    popChunkyBytes(StackSizeOfV128);
#  else
    masm.adjustStack((int)StackSizeOfV128);
#  endif
    MOZ_ASSERT(stackBefore - StackSizeOfV128 == currentStackHeight());
  }
#endif

  void popBytes(size_t bytes) {
    if (bytes > 0) {
#ifdef RABALDR_CHUNKY_STACK
      popChunkyBytes(bytes);
#else
      masm.freeStack(bytes);
#endif
    }
  }

  void loadStackI32(int32_t offset, RegI32 dest) {
    masm.load32(Address(sp_, stackOffset(offset)), dest);
  }

  void loadStackI64(int32_t offset, RegI64 dest) {
    masm.load64(Address(sp_, stackOffset(offset)), dest);
  }

#ifndef JS_PUNBOX64
  void loadStackI64Low(int32_t offset, RegI32 dest) {
    masm.load32(Address(sp_, stackOffset(offset - INT64LOW_OFFSET)), dest);
  }

  void loadStackI64High(int32_t offset, RegI32 dest) {
    masm.load32(Address(sp_, stackOffset(offset - INT64HIGH_OFFSET)), dest);
  }
#endif

  void loadStackRef(int32_t offset, RegRef dest) {
    masm.loadPtr(Address(sp_, stackOffset(offset)), dest);
  }

  void loadStackF64(int32_t offset, RegF64 dest) {
    masm.loadDouble(Address(sp_, stackOffset(offset)), dest);
  }

  void loadStackF32(int32_t offset, RegF32 dest) {
    masm.loadFloat32(Address(sp_, stackOffset(offset)), dest);
  }

#ifdef ENABLE_WASM_SIMD
  void loadStackV128(int32_t offset, RegV128 dest) {
    masm.loadUnalignedSimd128(Address(sp_, stackOffset(offset)), dest);
  }
#endif

  uint32_t prepareStackResultArea(StackHeight stackBase,
                                  uint32_t stackResultBytes) {
    uint32_t end = computeHeightWithStackResults(stackBase, stackResultBytes);
    if (currentStackHeight() < end) {
      uint32_t bytes = end - currentStackHeight();
#ifdef RABALDR_CHUNKY_STACK
      pushChunkyBytes(bytes);
#else
      masm.reserveStack(bytes);
#endif
      maxFramePushed_ = std::max(maxFramePushed_, masm.framePushed());
    }
    return end;
  }

  void finishStackResultArea(StackHeight stackBase, uint32_t stackResultBytes) {
    uint32_t end = computeHeightWithStackResults(stackBase, stackResultBytes);
    MOZ_ASSERT(currentStackHeight() >= end);
    popBytes(currentStackHeight() - end);
  }

  // |srcHeight| and |destHeight| are stack heights *including* |bytes|.
  void shuffleStackResultsTowardFP(uint32_t srcHeight, uint32_t destHeight,
                                   uint32_t bytes, Register temp) {
    MOZ_ASSERT(destHeight < srcHeight);
    MOZ_ASSERT(bytes % sizeof(uint32_t) == 0);
    uint32_t destOffset = stackOffset(destHeight) + bytes;
    uint32_t srcOffset = stackOffset(srcHeight) + bytes;
    while (bytes >= sizeof(intptr_t)) {
      destOffset -= sizeof(intptr_t);
      srcOffset -= sizeof(intptr_t);
      bytes -= sizeof(intptr_t);
      masm.loadPtr(Address(sp_, srcOffset), temp);
      masm.storePtr(temp, Address(sp_, destOffset));
    }
    if (bytes) {
      MOZ_ASSERT(bytes == sizeof(uint32_t));
      destOffset -= sizeof(uint32_t);
      srcOffset -= sizeof(uint32_t);
      masm.load32(Address(sp_, srcOffset), temp);
      masm.store32(temp, Address(sp_, destOffset));
    }
  }

  // Unlike the overload that operates on raw heights, |srcHeight| and
  // |destHeight| are stack heights *not including* |bytes|.
  void shuffleStackResultsTowardFP(StackHeight srcHeight,
                                   StackHeight destHeight, uint32_t bytes,
                                   Register temp) {
    MOZ_ASSERT(srcHeight.isValid());
    MOZ_ASSERT(destHeight.isValid());
    uint32_t src = computeHeightWithStackResults(srcHeight, bytes);
    uint32_t dest = computeHeightWithStackResults(destHeight, bytes);
    MOZ_ASSERT(src <= currentStackHeight());
    MOZ_ASSERT(dest <= currentStackHeight());
    shuffleStackResultsTowardFP(src, dest, bytes, temp);
  }

  // |srcHeight| and |destHeight| are stack heights *including* |bytes|.
  void shuffleStackResultsTowardSP(uint32_t srcHeight, uint32_t destHeight,
                                   uint32_t bytes, Register temp) {
    MOZ_ASSERT(destHeight > srcHeight);
    MOZ_ASSERT(bytes % sizeof(uint32_t) == 0);
    uint32_t destOffset = stackOffset(destHeight);
    uint32_t srcOffset = stackOffset(srcHeight);
    while (bytes >= sizeof(intptr_t)) {
      masm.loadPtr(Address(sp_, srcOffset), temp);
      masm.storePtr(temp, Address(sp_, destOffset));
      destOffset += sizeof(intptr_t);
      srcOffset += sizeof(intptr_t);
      bytes -= sizeof(intptr_t);
    }
    if (bytes) {
      MOZ_ASSERT(bytes == sizeof(uint32_t));
      masm.load32(Address(sp_, srcOffset), temp);
      masm.store32(temp, Address(sp_, destOffset));
    }
  }

  // Copy results from the top of the current stack frame to an area of memory,
  // and pop the stack accordingly.  `dest` is the address of the low byte of
  // that memory.
  void popStackResultsToMemory(Register dest, uint32_t bytes, Register temp) {
    MOZ_ASSERT(bytes <= currentStackHeight());
    MOZ_ASSERT(bytes % sizeof(uint32_t) == 0);
    uint32_t bytesToPop = bytes;
    uint32_t srcOffset = stackOffset(currentStackHeight());
    uint32_t destOffset = 0;
    while (bytes >= sizeof(intptr_t)) {
      masm.loadPtr(Address(sp_, srcOffset), temp);
      masm.storePtr(temp, Address(dest, destOffset));
      destOffset += sizeof(intptr_t);
      srcOffset += sizeof(intptr_t);
      bytes -= sizeof(intptr_t);
    }
    if (bytes) {
      MOZ_ASSERT(bytes == sizeof(uint32_t));
      masm.load32(Address(sp_, srcOffset), temp);
      masm.store32(temp, Address(dest, destOffset));
    }
    popBytes(bytesToPop);
  }

  void allocArgArea(size_t argSize) {
    if (argSize) {
      BaseStackFrameAllocator::allocArgArea(argSize);
      maxFramePushed_ = std::max(maxFramePushed_, masm.framePushed());
    }
  }

 private:
  void store32BitsToStack(int32_t imm, uint32_t destHeight, Register temp) {
    masm.move32(Imm32(imm), temp);
    masm.store32(temp, Address(sp_, stackOffset(destHeight)));
  }

  void store64BitsToStack(int64_t imm, uint32_t destHeight, Register temp) {
#ifdef JS_PUNBOX64
    masm.move64(Imm64(imm), Register64(temp));
    masm.store64(Register64(temp), Address(sp_, stackOffset(destHeight)));
#else
    union {
      int64_t i64;
      int32_t i32[2];
    } bits = {.i64 = imm};
    static_assert(sizeof(bits) == 8);
    store32BitsToStack(bits.i32[0], destHeight, temp);
    store32BitsToStack(bits.i32[1], destHeight - sizeof(int32_t), temp);
#endif
  }

 public:
  void storeImmediatePtrToStack(intptr_t imm, uint32_t destHeight,
                                Register temp) {
#ifdef JS_PUNBOX64
    static_assert(StackSizeOfPtr == 8);
    store64BitsToStack(imm, destHeight, temp);
#else
    static_assert(StackSizeOfPtr == 4);
    store32BitsToStack(int32_t(imm), destHeight, temp);
#endif
  }

  void storeImmediateI64ToStack(int64_t imm, uint32_t destHeight,
                                Register temp) {
    store64BitsToStack(imm, destHeight, temp);
  }

  void storeImmediateF32ToStack(float imm, uint32_t destHeight, Register temp) {
    union {
      float f32;
      int32_t i32;
    } bits{imm};
    static_assert(sizeof(bits) == 4);
    // Do not store 4 bytes if StackSizeOfFloat == 8.  It's probably OK to do
    // so, but it costs little to store something predictable.
    if (StackSizeOfFloat == 4) {
      store32BitsToStack(bits.i32, destHeight, temp);
    } else {
      store64BitsToStack(uint32_t(bits.i32), destHeight, temp);
    }
  }

  void storeImmediateF64ToStack(double imm, uint32_t destHeight,
                                Register temp) {
    union {
      double f64;
      int64_t i64;
    } bits{imm};
    static_assert(sizeof(bits) == 8);
    store64BitsToStack(bits.i64, destHeight, temp);
  }

#ifdef ENABLE_WASM_SIMD
  void storeImmediateV128ToStack(V128 imm, uint32_t destHeight, Register temp) {
    union {
      int32_t i32[4];
      uint8_t bytes[16];
    } bits{};
    static_assert(sizeof(bits) == 16);
    memcpy(bits.bytes, imm.bytes, 16);
    for (unsigned i = 0; i < 4; i++) {
      store32BitsToStack(bits.i32[i], destHeight - i * sizeof(int32_t), temp);
    }
  }
#endif
};

//////////////////////////////////////////////////////////////////////////////
//
// MachineStackTracker, used for stack-slot pointerness tracking.

// An expensive operation in stack-map creation is copying of the
// MachineStackTracker (MST) into the final StackMap.  This is done in
// StackMapGenerator::createStackMap.  Given that this is basically a
// bit-array copy, it is reasonable to ask whether the two classes could have
// a more similar representation, so that the copy could then be done with
// `memcpy`.
//
// Although in principle feasible, the follow complications exist, and so for
// the moment, this has not been done.
//
// * StackMap is optimised for compact size (storage) since there will be
//   many, so it uses a true bitmap.  MST is intended to be fast and simple,
//   and only one exists at once (per compilation thread).  Doing this would
//   require MST to use a true bitmap, and hence ..
//
// * .. the copying can't be a straight memcpy, since StackMap has entries for
//   words not covered by MST.  Hence the copy would need to shift bits in
//   each byte left or right (statistically speaking, in 7 cases out of 8) in
//   order to ensure no "holes" in the resulting bitmap.
//
// * Furthermore the copying would need to logically invert the direction of
//   the stacks.  For MST, index zero in the vector corresponds to the highest
//   address in the stack. For StackMap, bit index zero corresponds to the
//   lowest address in the stack.
//
// * Finally, StackMap is a variable-length structure whose size must be known
//   at creation time.  The size of an MST by contrast isn't known at creation
//   time -- it grows as the baseline compiler pushes stuff on its value
//   stack. That's why it has to have vector entry 0 being the highest address.
//
// * Although not directly relevant, StackMaps are also created by the via-Ion
//   compilation routes, by translation from the pre-existing "JS-era"
//   LSafePoints (CreateStackMapFromLSafepoint).  So if we want to mash
//   StackMap around to suit baseline better, we also need to ensure it
//   doesn't break Ion somehow.

class MachineStackTracker {
  // Simulates the machine's stack, with one bool per word.  The booleans are
  // represented as `uint8_t`s so as to guarantee the element size is one
  // byte.  Index zero in this vector corresponds to the highest address in
  // the machine's stack.  The last entry corresponds to what SP currently
  // points at.  This all assumes a grow-down stack.
  //
  // numPtrs_ contains the number of "true" values in vec_, and is therefore
  // redundant.  But it serves as a constant-time way to detect the common
  // case where vec_ holds no "true" values.
  size_t numPtrs_;
  Vector<uint8_t, 64, SystemAllocPolicy> vec_;

 public:
  MachineStackTracker() : numPtrs_(0) {}

  ~MachineStackTracker() {
#ifdef DEBUG
    size_t n = 0;
    for (uint8_t b : vec_) {
      n += (b ? 1 : 0);
    }
    MOZ_ASSERT(n == numPtrs_);
#endif
  }

  // Clone this MachineStackTracker, writing the result at |dst|.
  [[nodiscard]] bool cloneTo(MachineStackTracker* dst);

  // Notionally push |n| non-pointers on the stack.
  [[nodiscard]] bool pushNonGCPointers(size_t n) {
    return vec_.appendN(uint8_t(false), n);
  }

  // Mark the stack slot |offsetFromSP| up from the bottom as holding a
  // pointer.
  void setGCPointer(size_t offsetFromSP) {
    // offsetFromSP == 0 denotes the most recently pushed item, == 1 the
    // second most recently pushed item, etc.
    MOZ_ASSERT(offsetFromSP < vec_.length());

    size_t offsetFromTop = vec_.length() - 1 - offsetFromSP;
    numPtrs_ = numPtrs_ + 1 - (vec_[offsetFromTop] ? 1 : 0);
    vec_[offsetFromTop] = uint8_t(true);
  }

  // Query the pointerness of the slot |offsetFromSP| up from the bottom.
  bool isGCPointer(size_t offsetFromSP) const {
    MOZ_ASSERT(offsetFromSP < vec_.length());

    size_t offsetFromTop = vec_.length() - 1 - offsetFromSP;
    return bool(vec_[offsetFromTop]);
  }

  // Return the number of words tracked by this MachineStackTracker.
  size_t length() const { return vec_.length(); }

  // Return the number of pointer-typed words tracked by this
  // MachineStackTracker.
  size_t numPtrs() const {
    MOZ_ASSERT(numPtrs_ <= length());
    return numPtrs_;
  }

  // Discard all contents, but (per mozilla::Vector::clear semantics) don't
  // free or reallocate any dynamic storage associated with |vec_|.
  void clear() {
    vec_.clear();
    numPtrs_ = 0;
  }

  // An iterator that produces indices of reftyped slots, starting at the
  // logical bottom of the (grow-down) stack.  Indices have the same meaning
  // as the arguments to `isGCPointer`.  That is, if this iterator produces a
  // value `i`, then it means that `isGCPointer(i) == true`; if the value `i`
  // is never produced then `isGCPointer(i) == false`.  The values are
  // produced in ascending order.
  //
  // Because most slots are non-reftyped, some effort has been put into
  // skipping over large groups of non-reftyped slots quickly.
  class Iter {
    // Both `bufU8_` and `bufU32_` are made to point to `vec_`s array of
    // `uint8_t`s, so we can scan (backwards) through it either in bytes or
    // 32-bit words.  Recall that the last element in `vec_` pertains to the
    // lowest-addressed word in the machine's grow-down stack, and we want to
    // iterate logically "up" this stack, so we need to iterate backwards
    // through `vec_`.
    //
    // This dual-pointer scheme assumes that the `vec_`s content array is at
    // least 32-bit aligned.
    const uint8_t* bufU8_;
    const uint32_t* bufU32_;
    // The number of elements in `bufU8_`.
    const size_t nElems_;
    // The index in `bufU8_` where the next search should start.
    size_t next_;

   public:
    explicit Iter(const MachineStackTracker& mst)
        : bufU8_((uint8_t*)mst.vec_.begin()),
          bufU32_((uint32_t*)mst.vec_.begin()),
          nElems_(mst.vec_.length()),
          next_(mst.vec_.length() - 1) {
      MOZ_ASSERT(uintptr_t(bufU8_) == uintptr_t(bufU32_));
      // Check minimum alignment constraint on the array.
      MOZ_ASSERT(0 == (uintptr_t(bufU8_) & 3));
    }

    ~Iter() { MOZ_ASSERT(uintptr_t(bufU8_) == uintptr_t(bufU32_)); }

    // It is important, for termination of the search loop in `next()`, that
    // this has the value obtained by subtracting 1 from size_t(0).
    static constexpr size_t FINISHED = ~size_t(0);
    static_assert(FINISHED == size_t(0) - 1);

    // Returns the next index `i` for which `isGCPointer(i) == true`.
    size_t get() {
      while (next_ != FINISHED) {
        if (bufU8_[next_]) {
          next_--;
          return nElems_ - 1 - (next_ + 1);
        }
        // Invariant: next_ != FINISHED (so it's still a valid index)
        //       and: bufU8_[next_] == 0
        //            (so we need to move backwards by at least 1)
        //
        // BEGIN optimization -- this could be removed without affecting
        // correctness.
        if ((next_ & 7) == 0) {
          // We're at the "bottom" of the current dual-4-element word.  Check
          // if we can jump backwards by 8.  This saves a conditional branch
          // and a few cycles by ORing two adjacent 32-bit words together,
          // whilst not requiring 64-bit alignment of `bufU32_`.
          while (next_ >= 8 &&
                 (bufU32_[(next_ - 4) >> 2] | bufU32_[(next_ - 8) >> 2]) == 0) {
            next_ -= 8;
          }
        }
        // END optimization
        next_--;
      }
      return FINISHED;
    }
  };
};

//////////////////////////////////////////////////////////////////////////////
//
// StackMapGenerator, which carries all state needed to create stackmaps.

enum class HasDebugFrameWithLiveRefs { No, Maybe };

struct StackMapGenerator {
 private:
  // --- These are constant for the life of the function's compilation ---

  // For generating stackmaps, we'll need to know the offsets of registers
  // as saved by the trap exit stub.
  const RegisterOffsets& trapExitLayout_;
  const size_t trapExitLayoutNumWords_;

  // Completed stackmaps are added here
  StackMaps* stackMaps_;

  // So as to be able to get current offset when creating stackmaps
  const MacroAssembler& masm_;

 public:
  // --- These are constant once we've completed beginFunction() ---

  // The number of words of arguments passed to this function in memory.
  size_t numStackArgWords;

  MachineStackTracker machineStackTracker;  // tracks machine stack pointerness

  // This holds masm.framePushed at entry to the function's body.  It is a
  // Maybe because createStackMap needs to know whether or not we're still
  // in the prologue.  It makes a Nothing-to-Some transition just once per
  // function.
  Maybe<uint32_t> framePushedAtEntryToBody;

  // --- These can change at any point ---

  // This holds masm.framePushed at it would be be for a function call
  // instruction, but excluding the stack area used to pass arguments in
  // memory.  That is, for an upcoming function call, this will hold
  //
  //   masm.framePushed() at the call instruction -
  //      StackArgAreaSizeUnaligned(argumentTypes)
  //
  // This value denotes the lowest-addressed stack word covered by the current
  // function's stackmap.  Words below this point form the highest-addressed
  // area of the callee's stackmap.  Note that all alignment padding above the
  // arguments-in-memory themselves belongs to the caller's stackmap, which
  // is why this is defined in terms of StackArgAreaSizeUnaligned() rather than
  // StackArgAreaSizeAligned().
  //
  // When not inside a function call setup/teardown sequence, it is Nothing.
  // It can make Nothing-to/from-Some transitions arbitrarily as we progress
  // through the function body.
  Maybe<uint32_t> framePushedExcludingOutboundCallArgs;

  // The number of memory-resident, ref-typed entries on the containing
  // BaseCompiler::stk_.
  size_t memRefsOnStk;

  // This is a copy of machineStackTracker that is used only within individual
  // calls to createStackMap. It is here only to avoid possible heap allocation
  // costs resulting from making it local to createStackMap().
  MachineStackTracker augmentedMst;

  StackMapGenerator(StackMaps* stackMaps, const RegisterOffsets& trapExitLayout,
                    const size_t trapExitLayoutNumWords,
                    const MacroAssembler& masm)
      : trapExitLayout_(trapExitLayout),
        trapExitLayoutNumWords_(trapExitLayoutNumWords),
        stackMaps_(stackMaps),
        masm_(masm),
        numStackArgWords(0),
        memRefsOnStk(0) {}

  // At the beginning of a function, we may have live roots in registers (as
  // arguments) at the point where we perform a stack overflow check.  This
  // method generates the "extra" stackmap entries to describe that, in the
  // case that the check fails and we wind up calling into the wasm exit
  // stub, as generated by GenerateTrapExit().
  //
  // The resulting map must correspond precisely with the stack layout
  // created for the integer registers as saved by (code generated by)
  // GenerateTrapExit().  To do that we use trapExitLayout_ and
  // trapExitLayoutNumWords_, which together comprise a description of the
  // layout and are created by GenerateTrapExitRegisterOffsets().
  [[nodiscard]] bool generateStackmapEntriesForTrapExit(
      const ArgTypeVector& args, ExitStubMapVector* extras);

  // Creates a stackmap associated with the instruction denoted by
  // |assemblerOffset|, incorporating pointers from the current operand
  // stack |stk|, incorporating possible extra pointers in |extra| at the
  // lower addressed end, and possibly with the associated frame having a
  // DebugFrame that must be traced, as indicated by |debugFrameWithLiveRefs|.
  [[nodiscard]] bool createStackMap(
      const char* who, const ExitStubMapVector& extras,
      uint32_t assemblerOffset,
      HasDebugFrameWithLiveRefs debugFrameWithLiveRefs, const StkVector& stk);
};

}  // namespace wasm
}  // namespace js

#endif  // wasm_wasm_baseline_frame_h

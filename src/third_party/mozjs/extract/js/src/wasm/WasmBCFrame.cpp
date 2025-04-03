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

#include "wasm/WasmBCFrame.h"

#include "wasm/WasmBaselineCompile.h"  // For BaseLocalIter
#include "wasm/WasmBCClass.h"

#include "jit/MacroAssembler-inl.h"
#include "wasm/WasmBCClass-inl.h"
#include "wasm/WasmBCCodegen-inl.h"
#include "wasm/WasmBCRegDefs-inl.h"
#include "wasm/WasmBCRegMgmt-inl.h"
#include "wasm/WasmBCStkMgmt-inl.h"

namespace js {
namespace wasm {

//////////////////////////////////////////////////////////////////////////////
//
// BaseLocalIter methods.

BaseLocalIter::BaseLocalIter(const ValTypeVector& locals,
                             const ArgTypeVector& args, bool debugEnabled)
    : locals_(locals),
      args_(args),
      argsIter_(args_),
      index_(0),
      frameSize_(0),
      nextFrameSize_(debugEnabled ? DebugFrame::offsetOfFrame() : 0),
      frameOffset_(INT32_MAX),
      stackResultPointerOffset_(INT32_MAX),
      mirType_(MIRType::Undefined),
      done_(false) {
  MOZ_ASSERT(args.lengthWithoutStackResults() <= locals.length());
  settle();
}

int32_t BaseLocalIter::pushLocal(size_t nbytes) {
  MOZ_ASSERT(nbytes % 4 == 0 && nbytes <= 16);
  nextFrameSize_ = AlignBytes(frameSize_, nbytes) + nbytes;
  return nextFrameSize_;  // Locals grow down so capture base address.
}

void BaseLocalIter::settle() {
  MOZ_ASSERT(!done_);
  frameSize_ = nextFrameSize_;

  if (!argsIter_.done()) {
    mirType_ = argsIter_.mirType();
    MIRType concreteType = mirType_;
    switch (mirType_) {
      case MIRType::StackResults:
        // The pointer to stack results is handled like any other argument:
        // either addressed in place if it is passed on the stack, or we spill
        // it in the frame if it's in a register.
        MOZ_ASSERT(args_.isSyntheticStackResultPointerArg(index_));
        concreteType = MIRType::Pointer;
        [[fallthrough]];
      case MIRType::Int32:
      case MIRType::Int64:
      case MIRType::Double:
      case MIRType::Float32:
      case MIRType::RefOrNull:
#ifdef ENABLE_WASM_SIMD
      case MIRType::Simd128:
#endif
        if (argsIter_->argInRegister()) {
          frameOffset_ = pushLocal(MIRTypeToSize(concreteType));
        } else {
          frameOffset_ = -(argsIter_->offsetFromArgBase() + sizeof(Frame));
        }
        break;
      default:
        MOZ_CRASH("Argument type");
    }
    if (mirType_ == MIRType::StackResults) {
      stackResultPointerOffset_ = frameOffset();
      // Advance past the synthetic stack result pointer argument and fall
      // through to the next case.
      argsIter_++;
      frameSize_ = nextFrameSize_;
      MOZ_ASSERT(argsIter_.done());
    } else {
      return;
    }
  }

  if (index_ < locals_.length()) {
    switch (locals_[index_].kind()) {
      case ValType::I32:
      case ValType::I64:
      case ValType::F32:
      case ValType::F64:
#ifdef ENABLE_WASM_SIMD
      case ValType::V128:
#endif
      case ValType::Ref:
        // TODO/AnyRef-boxing: With boxed immediates and strings, the
        // debugger must be made aware that AnyRef != Pointer.
        ASSERT_ANYREF_IS_JSOBJECT;
        mirType_ = locals_[index_].toMIRType();
        frameOffset_ = pushLocal(MIRTypeToSize(mirType_));
        break;
      default:
        MOZ_CRASH("Compiler bug: Unexpected local type");
    }
    return;
  }

  done_ = true;
}

void BaseLocalIter::operator++(int) {
  MOZ_ASSERT(!done_);
  index_++;
  if (!argsIter_.done()) {
    argsIter_++;
  }
  settle();
}

//////////////////////////////////////////////////////////////////////////////
//
// Stack map methods.

bool BaseCompiler::createStackMap(const char* who) {
  const ExitStubMapVector noExtras;
  return stackMapGenerator_.createStackMap(who, noExtras, masm.currentOffset(),
                                           HasDebugFrameWithLiveRefs::No, stk_);
}

bool BaseCompiler::createStackMap(const char* who, CodeOffset assemblerOffset) {
  const ExitStubMapVector noExtras;
  return stackMapGenerator_.createStackMap(who, noExtras,
                                           assemblerOffset.offset(),
                                           HasDebugFrameWithLiveRefs::No, stk_);
}

bool BaseCompiler::createStackMap(
    const char* who, HasDebugFrameWithLiveRefs debugFrameWithLiveRefs) {
  const ExitStubMapVector noExtras;
  return stackMapGenerator_.createStackMap(who, noExtras, masm.currentOffset(),
                                           debugFrameWithLiveRefs, stk_);
}

bool BaseCompiler::createStackMap(
    const char* who, const ExitStubMapVector& extras, uint32_t assemblerOffset,
    HasDebugFrameWithLiveRefs debugFrameWithLiveRefs) {
  return stackMapGenerator_.createStackMap(who, extras, assemblerOffset,
                                           debugFrameWithLiveRefs, stk_);
}

bool MachineStackTracker::cloneTo(MachineStackTracker* dst) {
  MOZ_ASSERT(dst->vec_.empty());
  if (!dst->vec_.appendAll(vec_)) {
    return false;
  }
  dst->numPtrs_ = numPtrs_;
  return true;
}

bool StackMapGenerator::generateStackmapEntriesForTrapExit(
    const ArgTypeVector& args, ExitStubMapVector* extras) {
  return GenerateStackmapEntriesForTrapExit(args, trapExitLayout_,
                                            trapExitLayoutNumWords_, extras);
}

bool StackMapGenerator::createStackMap(
    const char* who, const ExitStubMapVector& extras, uint32_t assemblerOffset,
    HasDebugFrameWithLiveRefs debugFrameWithLiveRefs, const StkVector& stk) {
  size_t countedPointers = machineStackTracker.numPtrs() + memRefsOnStk;
#ifndef DEBUG
  // An important optimization.  If there are obviously no pointers, as
  // we expect in the majority of cases, exit quickly.
  if (countedPointers == 0 &&
      debugFrameWithLiveRefs == HasDebugFrameWithLiveRefs::No) {
    // We can skip creating the map if there are no |true| elements in
    // |extras|.
    bool extrasHasRef = false;
    for (bool b : extras) {
      if (b) {
        extrasHasRef = true;
        break;
      }
    }
    if (!extrasHasRef) {
      return true;
    }
  }
#else
  // In the debug case, create the stackmap regardless, and cross-check
  // the pointer-counting below.  We expect the final map to have
  // |countedPointers| in total.  This doesn't include those in the
  // DebugFrame, but they do not appear in the map's bitmap.  Note that
  // |countedPointers| is debug-only from this point onwards.
  for (bool b : extras) {
    countedPointers += (b ? 1 : 0);
  }
#endif

  // Start with the frame-setup map, and add operand-stack information to
  // that.  augmentedMst holds live data only within individual calls to
  // createStackMap.
  augmentedMst.clear();
  if (!machineStackTracker.cloneTo(&augmentedMst)) {
    return false;
  }

  // At this point, augmentedMst only contains entries covering the
  // incoming argument area (if any) and for the area allocated by this
  // function's prologue.  We now need to calculate how far the machine's
  // stack pointer is below where it was at the start of the body.  But we
  // must take care not to include any words pushed as arguments to an
  // upcoming function call, since those words "belong" to the stackmap of
  // the callee, not to the stackmap of this function.  Note however that
  // any alignment padding pushed prior to pushing the args *does* belong to
  // this function.
  //
  // That padding is taken into account at the point where
  // framePushedExcludingOutboundCallArgs is set, viz, in startCallArgs(),
  // and comprises two components:
  //
  // * call->frameAlignAdjustment
  // * the padding applied to the stack arg area itself.  That is:
  //   StackArgAreaSize(argTys) - StackArgAreaSizeUnpadded(argTys)
  Maybe<uint32_t> framePushedExcludingArgs;
  if (framePushedAtEntryToBody.isNothing()) {
    // Still in the prologue.  framePushedExcludingArgs remains Nothing.
    MOZ_ASSERT(framePushedExcludingOutboundCallArgs.isNothing());
  } else {
    // In the body.
    MOZ_ASSERT(masm_.framePushed() >= framePushedAtEntryToBody.value());
    if (framePushedExcludingOutboundCallArgs.isSome()) {
      // In the body, and we've potentially pushed some args onto the stack.
      // We must ignore them when sizing the stackmap.
      MOZ_ASSERT(masm_.framePushed() >=
                 framePushedExcludingOutboundCallArgs.value());
      MOZ_ASSERT(framePushedExcludingOutboundCallArgs.value() >=
                 framePushedAtEntryToBody.value());
      framePushedExcludingArgs =
          Some(framePushedExcludingOutboundCallArgs.value());
    } else {
      // In the body, but not with call args on the stack.  The stackmap
      // must be sized so as to extend all the way "down" to
      // masm_.framePushed().
      framePushedExcludingArgs = Some(masm_.framePushed());
    }
  }

  if (framePushedExcludingArgs.isSome()) {
    uint32_t bodyPushedBytes =
        framePushedExcludingArgs.value() - framePushedAtEntryToBody.value();
    MOZ_ASSERT(0 == bodyPushedBytes % sizeof(void*));
    if (!augmentedMst.pushNonGCPointers(bodyPushedBytes / sizeof(void*))) {
      return false;
    }
  }

  // Scan the operand stack, marking pointers in the just-added new
  // section.
  MOZ_ASSERT_IF(framePushedAtEntryToBody.isNothing(), stk.empty());
  MOZ_ASSERT_IF(framePushedExcludingArgs.isNothing(), stk.empty());

  for (const Stk& v : stk) {
#ifndef DEBUG
    // We don't track roots in registers, per rationale below, so if this
    // doesn't hold, something is seriously wrong, and we're likely to get a
    // GC-related crash.
    MOZ_RELEASE_ASSERT(v.kind() != Stk::RegisterRef);
    if (v.kind() != Stk::MemRef) {
      continue;
    }
#else
    // Take the opportunity to check everything we reasonably can about
    // operand stack elements.
    switch (v.kind()) {
      case Stk::MemI32:
      case Stk::MemI64:
      case Stk::MemF32:
      case Stk::MemF64:
      case Stk::ConstI32:
      case Stk::ConstI64:
      case Stk::ConstF32:
      case Stk::ConstF64:
#  ifdef ENABLE_WASM_SIMD
      case Stk::MemV128:
      case Stk::ConstV128:
#  endif
        // All of these have uninteresting type.
        continue;
      case Stk::LocalI32:
      case Stk::LocalI64:
      case Stk::LocalF32:
      case Stk::LocalF64:
#  ifdef ENABLE_WASM_SIMD
      case Stk::LocalV128:
#  endif
        // These also have uninteresting type.  Check that they live in the
        // section of stack set up by beginFunction().  The unguarded use of
        // |value()| here is safe due to the assertion above this loop.
        MOZ_ASSERT(v.offs() <= framePushedAtEntryToBody.value());
        continue;
      case Stk::RegisterI32:
      case Stk::RegisterI64:
      case Stk::RegisterF32:
      case Stk::RegisterF64:
#  ifdef ENABLE_WASM_SIMD
      case Stk::RegisterV128:
#  endif
        // These also have uninteresting type, but more to the point: all
        // registers holding live values should have been flushed to the
        // machine stack immediately prior to the instruction to which this
        // stackmap pertains.  So these can't happen.
        MOZ_CRASH("createStackMap: operand stack has Register-non-Ref");
      case Stk::MemRef:
        // This is the only case we care about.  We'll handle it after the
        // switch.
        break;
      case Stk::LocalRef:
        // We need the stackmap to mention this pointer, but it should
        // already be in the machineStackTracker section created by
        // beginFunction().
        MOZ_ASSERT(v.offs() <= framePushedAtEntryToBody.value());
        continue;
      case Stk::ConstRef:
        // This can currently only be a null pointer.
        MOZ_ASSERT(v.refval() == 0);
        continue;
      case Stk::RegisterRef:
        // This can't happen, per rationale above.
        MOZ_CRASH("createStackMap: operand stack contains RegisterRef");
      default:
        MOZ_CRASH("createStackMap: unknown operand stack element");
    }
#endif
    // v.offs() holds masm.framePushed() at the point immediately after it
    // was pushed on the stack.  Since it's still on the stack,
    // masm.framePushed() can't be less.
    MOZ_ASSERT(v.offs() <= framePushedExcludingArgs.value());
    uint32_t offsFromMapLowest = framePushedExcludingArgs.value() - v.offs();
    MOZ_ASSERT(0 == offsFromMapLowest % sizeof(void*));
    augmentedMst.setGCPointer(offsFromMapLowest / sizeof(void*));
  }

  // Create the final StackMap.  The initial map is zeroed out, so there's
  // no need to write zero bits in it.
  const uint32_t extraWords = extras.length();
  const uint32_t augmentedMstWords = augmentedMst.length();
  const uint32_t numMappedWords = extraWords + augmentedMstWords;
  StackMap* stackMap = StackMap::create(numMappedWords);
  if (!stackMap) {
    return false;
  }

  {
    // First the exit stub extra words, if any.
    uint32_t i = 0;
    for (bool b : extras) {
      if (b) {
        stackMap->setBit(i);
      }
      i++;
    }
  }
  {
    // Followed by the "main" part of the map.
    //
    // This is really just a bit-array copy, so it is reasonable to ask
    // whether the representation of MachineStackTracker could be made more
    // similar to that of StackMap, so that the copy could be done with
    // `memcpy`.  Unfortunately it's not so simple; see comment on `class
    // MachineStackTracker` for details.
    MachineStackTracker::Iter iter(augmentedMst);
    while (true) {
      size_t i = iter.get();
      if (i == MachineStackTracker::Iter::FINISHED) {
        break;
      }
      stackMap->setBit(extraWords + i);
    }
  }

  stackMap->setExitStubWords(extraWords);

  // Record in the map, how far down from the highest address the Frame* is.
  // Take the opportunity to check that we haven't marked any part of the
  // Frame itself as a pointer.
  stackMap->setFrameOffsetFromTop(numStackArgWords +
                                  sizeof(Frame) / sizeof(void*));
#ifdef DEBUG
  for (uint32_t i = 0; i < sizeof(Frame) / sizeof(void*); i++) {
    MOZ_ASSERT(stackMap->getBit(stackMap->header.numMappedWords -
                                stackMap->header.frameOffsetFromTop + i) == 0);
  }
#endif

  // Note the presence of a DebugFrame with live pointers, if any.
  if (debugFrameWithLiveRefs != HasDebugFrameWithLiveRefs::No) {
    stackMap->setHasDebugFrameWithLiveRefs();
  }

  // Add the completed map to the running collection thereof.
  if (!stackMaps_->add((uint8_t*)(uintptr_t)assemblerOffset, stackMap)) {
    stackMap->destroy();
    return false;
  }

#ifdef DEBUG
  {
    // Crosscheck the map pointer counting.
    uint32_t nw = stackMap->header.numMappedWords;
    uint32_t np = 0;
    for (uint32_t i = 0; i < nw; i++) {
      np += stackMap->getBit(i);
    }
    MOZ_ASSERT(size_t(np) == countedPointers);
  }
#endif

  return true;
}

//////////////////////////////////////////////////////////////////////////////
//
// Stack frame methods.

void BaseStackFrame::zeroLocals(BaseRegAlloc* ra) {
  MOZ_ASSERT(varLow_ != UINT32_MAX);

  if (varLow_ == varHigh_) {
    return;
  }

  static const uint32_t wordSize = sizeof(void*);

  // The adjustments to 'low' by the size of the item being stored compensates
  // for the fact that locals offsets are the offsets from Frame to the bytes
  // directly "above" the locals in the locals area.  See comment at Local.

  // On 64-bit systems we may have 32-bit alignment for the local area as it
  // may be preceded by parameters and prologue/debug data.

  uint32_t low = varLow_;
  if (low % wordSize) {
    masm.store32(Imm32(0), Address(sp_, localOffset(low + 4)));
    low += 4;
  }
  MOZ_ASSERT(low % wordSize == 0);

  const uint32_t high = AlignBytes(varHigh_, wordSize);

  // An UNROLL_LIMIT of 16 is chosen so that we only need an 8-bit signed
  // immediate to represent the offset in the store instructions in the loop
  // on x64.

  const uint32_t UNROLL_LIMIT = 16;
  const uint32_t initWords = (high - low) / wordSize;
  const uint32_t tailWords = initWords % UNROLL_LIMIT;
  const uint32_t loopHigh = high - (tailWords * wordSize);

  // With only one word to initialize, just store an immediate zero.

  if (initWords == 1) {
    masm.storePtr(ImmWord(0), Address(sp_, localOffset(low + wordSize)));
    return;
  }

  // For other cases, it's best to have a zero in a register.
  //
  // One can do more here with SIMD registers (store 16 bytes at a time) or
  // with instructions like STRD on ARM (store 8 bytes at a time), but that's
  // for another day.

  RegI32 zero = ra->needI32();
  masm.mov(ImmWord(0), zero);

  // For the general case we want to have a loop body of UNROLL_LIMIT stores
  // and then a tail of less than UNROLL_LIMIT stores.  When initWords is less
  // than 2*UNROLL_LIMIT the loop trip count is at most 1 and there is no
  // benefit to having the pointer calculations and the compare-and-branch.
  // So we completely unroll when we have initWords < 2 * UNROLL_LIMIT.  (In
  // this case we'll end up using 32-bit offsets on x64 for up to half of the
  // stores, though.)

  // Fully-unrolled case.

  if (initWords < 2 * UNROLL_LIMIT) {
    for (uint32_t i = low; i < high; i += wordSize) {
      masm.storePtr(zero, Address(sp_, localOffset(i + wordSize)));
    }
    ra->freeI32(zero);
    return;
  }

  // Unrolled loop with a tail. Stores will use negative offsets. That's OK
  // for x86 and ARM, at least.

  // Compute pointer to the highest-addressed slot on the frame.
  RegI32 p = ra->needI32();
  masm.computeEffectiveAddress(Address(sp_, localOffset(low + wordSize)), p);

  // Compute pointer to the lowest-addressed slot on the frame that will be
  // initialized by the loop body.
  RegI32 lim = ra->needI32();
  masm.computeEffectiveAddress(Address(sp_, localOffset(loopHigh + wordSize)),
                               lim);

  // The loop body.  Eventually we'll have p == lim and exit the loop.
  Label again;
  masm.bind(&again);
  for (uint32_t i = 0; i < UNROLL_LIMIT; ++i) {
    masm.storePtr(zero, Address(p, -(wordSize * i)));
  }
  masm.subPtr(Imm32(UNROLL_LIMIT * wordSize), p);
  masm.branchPtr(Assembler::LessThan, lim, p, &again);

  // The tail.
  for (uint32_t i = 0; i < tailWords; ++i) {
    masm.storePtr(zero, Address(p, -(wordSize * i)));
  }

  ra->freeI32(p);
  ra->freeI32(lim);
  ra->freeI32(zero);
}

}  // namespace wasm
}  // namespace js

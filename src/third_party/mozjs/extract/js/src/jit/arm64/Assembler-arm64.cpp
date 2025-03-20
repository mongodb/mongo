/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/arm64/Assembler-arm64.h"

#include "mozilla/DebugOnly.h"
#include "mozilla/MathAlgorithms.h"
#include "mozilla/Maybe.h"

#include "gc/Marking.h"
#include "jit/arm64/Architecture-arm64.h"
#include "jit/arm64/MacroAssembler-arm64.h"
#include "jit/arm64/vixl/Disasm-vixl.h"
#include "jit/AutoWritableJitCode.h"
#include "jit/ExecutableAllocator.h"
#include "vm/Realm.h"

#include "gc/StoreBuffer-inl.h"

using namespace js;
using namespace js::jit;

using mozilla::CountLeadingZeroes32;
using mozilla::DebugOnly;

// Note this is used for inter-wasm calls and may pass arguments and results
// in floating point registers even if the system ABI does not.

ABIArg ABIArgGenerator::next(MIRType type) {
  switch (type) {
    case MIRType::Int32:
    case MIRType::Int64:
    case MIRType::Pointer:
    case MIRType::WasmAnyRef:
    case MIRType::StackResults:
      if (intRegIndex_ == NumIntArgRegs) {
        current_ = ABIArg(stackOffset_);
        stackOffset_ += sizeof(uintptr_t);
        break;
      }
      current_ = ABIArg(Register::FromCode(intRegIndex_));
      intRegIndex_++;
      break;

    case MIRType::Float32:
    case MIRType::Double:
      if (floatRegIndex_ == NumFloatArgRegs) {
        current_ = ABIArg(stackOffset_);
        stackOffset_ += sizeof(double);
        break;
      }
      current_ = ABIArg(FloatRegister(FloatRegisters::Encoding(floatRegIndex_),
                                      type == MIRType::Double
                                          ? FloatRegisters::Double
                                          : FloatRegisters::Single));
      floatRegIndex_++;
      break;

#ifdef ENABLE_WASM_SIMD
    case MIRType::Simd128:
      if (floatRegIndex_ == NumFloatArgRegs) {
        stackOffset_ = AlignBytes(stackOffset_, SimdMemoryAlignment);
        current_ = ABIArg(stackOffset_);
        stackOffset_ += FloatRegister::SizeOfSimd128;
        break;
      }
      current_ = ABIArg(FloatRegister(FloatRegisters::Encoding(floatRegIndex_),
                                      FloatRegisters::Simd128));
      floatRegIndex_++;
      break;
#endif

    default:
      // Note that in Assembler-x64.cpp there's a special case for Win64 which
      // does not allow passing SIMD by value.  Since there's Win64 on ARM64 we
      // may need to duplicate that logic here.
      MOZ_CRASH("Unexpected argument type");
  }
  return current_;
}

namespace js {
namespace jit {

void Assembler::finish() {
  armbuffer_.flushPool();

  // The extended jump table is part of the code buffer.
  ExtendedJumpTable_ = emitExtendedJumpTable();
  Assembler::FinalizeCode();
}

bool Assembler::appendRawCode(const uint8_t* code, size_t numBytes) {
  flush();
  return armbuffer_.appendRawCode(code, numBytes);
}

bool Assembler::reserve(size_t size) {
  // This buffer uses fixed-size chunks so there's no point in reserving
  // now vs. on-demand.
  return !oom();
}

bool Assembler::swapBuffer(wasm::Bytes& bytes) {
  // For now, specialize to the one use case. As long as wasm::Bytes is a
  // Vector, not a linked-list of chunks, there's not much we can do other
  // than copy.
  MOZ_ASSERT(bytes.empty());
  if (!bytes.resize(bytesNeeded())) {
    return false;
  }
  armbuffer_.executableCopy(bytes.begin());
  return true;
}

BufferOffset Assembler::emitExtendedJumpTable() {
  if (!pendingJumps_.length() || oom()) {
    return BufferOffset();
  }

  armbuffer_.flushPool();
  armbuffer_.align(SizeOfJumpTableEntry);

  BufferOffset tableOffset = armbuffer_.nextOffset();

  for (size_t i = 0; i < pendingJumps_.length(); i++) {
    // Each JumpTableEntry is of the form:
    //   LDR ip0 [PC, 8]
    //   BR ip0
    //   [Patchable 8-byte constant low bits]
    //   [Patchable 8-byte constant high bits]
    DebugOnly<size_t> preOffset = size_t(armbuffer_.nextOffset().getOffset());

    // The unguarded use of ScratchReg64 here is OK:
    //
    // - The present function is called from code that does not claim any
    //   scratch registers, we're done compiling user code and are emitting jump
    //   tables.  Hence the scratch registers are available when we enter.
    //
    // - The pendingJumps_ represent jumps to other code sections that are not
    //   known to this MacroAssembler instance, and we're generating code to
    //   jump there.  It is safe to assume that any code using such a generated
    //   branch to an unknown location did not store any valuable value in any
    //   scratch register.  Hence the scratch registers can definitely be
    //   clobbered here.
    //
    // - Scratch register usage is restricted to sequential control flow within
    //   MacroAssembler functions.  Hence the scratch registers will not be
    //   clobbered by ldr and br as they are Assembler primitives, not
    //   MacroAssembler functions.

    ldr(ScratchReg64, ptrdiff_t(8 / vixl::kInstructionSize));
    br(ScratchReg64);

    DebugOnly<size_t> prePointer = size_t(armbuffer_.nextOffset().getOffset());
    MOZ_ASSERT_IF(!oom(),
                  prePointer - preOffset == OffsetOfJumpTableEntryPointer);

    brk(0x0);
    brk(0x0);

    DebugOnly<size_t> postOffset = size_t(armbuffer_.nextOffset().getOffset());

    MOZ_ASSERT_IF(!oom(), postOffset - preOffset == SizeOfJumpTableEntry);
  }

  if (oom()) {
    return BufferOffset();
  }

  return tableOffset;
}

void Assembler::executableCopy(uint8_t* buffer) {
  // Copy the code and all constant pools into the output buffer.
  armbuffer_.executableCopy(buffer);

  // Patch any relative jumps that target code outside the buffer.
  // The extended jump table may be used for distant jumps.
  for (size_t i = 0; i < pendingJumps_.length(); i++) {
    RelativePatch& rp = pendingJumps_[i];
    MOZ_ASSERT(rp.target);

    Instruction* target = (Instruction*)rp.target;
    Instruction* branch = (Instruction*)(buffer + rp.offset.getOffset());
    JumpTableEntry* extendedJumpTable = reinterpret_cast<JumpTableEntry*>(
        buffer + ExtendedJumpTable_.getOffset());
    if (branch->BranchType() != vixl::UnknownBranchType) {
      if (branch->IsTargetReachable(target)) {
        branch->SetImmPCOffsetTarget(target);
      } else {
        JumpTableEntry* entry = &extendedJumpTable[i];
        branch->SetImmPCOffsetTarget(entry->getLdr());
        entry->data = target;
      }
    } else {
      // Currently a two-instruction call, it should be possible to optimize
      // this into a single instruction call + nop in some instances, but this
      // will work.
    }
  }
}

BufferOffset Assembler::immPool(ARMRegister dest, uint8_t* value,
                                vixl::LoadLiteralOp op, const LiteralDoc& doc,
                                ARMBuffer::PoolEntry* pe) {
  uint32_t inst = op | Rt(dest);
  const size_t numInst = 1;
  const unsigned sizeOfPoolEntryInBytes = 4;
  const unsigned numPoolEntries = sizeof(value) / sizeOfPoolEntryInBytes;
  return allocLiteralLoadEntry(numInst, numPoolEntries, (uint8_t*)&inst, value,
                               doc, pe);
}

BufferOffset Assembler::immPool64(ARMRegister dest, uint64_t value,
                                  ARMBuffer::PoolEntry* pe) {
  return immPool(dest, (uint8_t*)&value, vixl::LDR_x_lit, LiteralDoc(value),
                 pe);
}

BufferOffset Assembler::fImmPool(ARMFPRegister dest, uint8_t* value,
                                 vixl::LoadLiteralOp op,
                                 const LiteralDoc& doc) {
  uint32_t inst = op | Rt(dest);
  const size_t numInst = 1;
  const unsigned sizeOfPoolEntryInBits = 32;
  const unsigned numPoolEntries = dest.size() / sizeOfPoolEntryInBits;
  return allocLiteralLoadEntry(numInst, numPoolEntries, (uint8_t*)&inst, value,
                               doc);
}

BufferOffset Assembler::fImmPool64(ARMFPRegister dest, double value) {
  return fImmPool(dest, (uint8_t*)&value, vixl::LDR_d_lit, LiteralDoc(value));
}

BufferOffset Assembler::fImmPool32(ARMFPRegister dest, float value) {
  return fImmPool(dest, (uint8_t*)&value, vixl::LDR_s_lit, LiteralDoc(value));
}

void Assembler::bind(Label* label, BufferOffset targetOffset) {
#ifdef JS_DISASM_ARM64
  spew_.spewBind(label);
#endif
  // Nothing has seen the label yet: just mark the location.
  // If we've run out of memory, don't attempt to modify the buffer which may
  // not be there. Just mark the label as bound to the (possibly bogus)
  // targetOffset.
  if (!label->used() || oom()) {
    label->bind(targetOffset.getOffset());
    return;
  }

  // Get the most recent instruction that used the label, as stored in the
  // label. This instruction is the head of an implicit linked list of label
  // uses.
  BufferOffset branchOffset(label);

  while (branchOffset.assigned()) {
    // Before overwriting the offset in this instruction, get the offset of
    // the next link in the implicit branch list.
    BufferOffset nextOffset = NextLink(branchOffset);

    // Linking against the actual (Instruction*) would be invalid,
    // since that Instruction could be anywhere in memory.
    // Instead, just link against the correct relative offset, assuming
    // no constant pools, which will be taken into consideration
    // during finalization.
    ptrdiff_t relativeByteOffset =
        targetOffset.getOffset() - branchOffset.getOffset();
    Instruction* link = getInstructionAt(branchOffset);

    // This branch may still be registered for callbacks. Stop tracking it.
    vixl::ImmBranchType branchType = link->BranchType();
    vixl::ImmBranchRangeType branchRange =
        Instruction::ImmBranchTypeToRange(branchType);
    if (branchRange < vixl::NumShortBranchRangeTypes) {
      BufferOffset deadline(
          branchOffset.getOffset() +
          Instruction::ImmBranchMaxForwardOffset(branchRange));
      armbuffer_.unregisterBranchDeadline(branchRange, deadline);
    }

    // Is link able to reach the label?
    if (link->IsPCRelAddressing() ||
        link->IsTargetReachable(link + relativeByteOffset)) {
      // Write a new relative offset into the instruction.
      link->SetImmPCOffsetTarget(link + relativeByteOffset);
    } else {
      // This is a short-range branch, and it can't reach the label directly.
      // Verify that it branches to a veneer: an unconditional branch.
      MOZ_ASSERT(getInstructionAt(nextOffset)->BranchType() ==
                 vixl::UncondBranchType);
    }

    branchOffset = nextOffset;
  }

  // Bind the label, so that future uses may encode the offset immediately.
  label->bind(targetOffset.getOffset());
}

void Assembler::addPendingJump(BufferOffset src, ImmPtr target,
                               RelocationKind reloc) {
  MOZ_ASSERT(target.value != nullptr);

  if (reloc == RelocationKind::JITCODE) {
    jumpRelocations_.writeUnsigned(src.getOffset());
  }

  // This jump is not patchable at runtime. Extended jump table entry
  // requirements cannot be known until finalization, so to be safe, give each
  // jump and entry. This also causes GC tracing of the target.
  enoughMemory_ &=
      pendingJumps_.append(RelativePatch(src, target.value, reloc));
}

void Assembler::PatchWrite_NearCall(CodeLocationLabel start,
                                    CodeLocationLabel toCall) {
  Instruction* dest = (Instruction*)start.raw();
  ptrdiff_t relTarget = (Instruction*)toCall.raw() - dest;
  ptrdiff_t relTarget00 = relTarget >> 2;
  MOZ_RELEASE_ASSERT((relTarget & 0x3) == 0);
  MOZ_RELEASE_ASSERT(vixl::IsInt26(relTarget00));

  bl(dest, relTarget00);
}

void Assembler::PatchDataWithValueCheck(CodeLocationLabel label,
                                        PatchedImmPtr newValue,
                                        PatchedImmPtr expected) {
  Instruction* i = (Instruction*)label.raw();
  void** pValue = i->LiteralAddress<void**>();
  MOZ_ASSERT(*pValue == expected.value);
  *pValue = newValue.value;
}

void Assembler::PatchDataWithValueCheck(CodeLocationLabel label,
                                        ImmPtr newValue, ImmPtr expected) {
  PatchDataWithValueCheck(label, PatchedImmPtr(newValue.value),
                          PatchedImmPtr(expected.value));
}

void Assembler::ToggleToJmp(CodeLocationLabel inst_) {
  Instruction* i = (Instruction*)inst_.raw();
  MOZ_ASSERT(i->IsAddSubImmediate());

  // Refer to instruction layout in ToggleToCmp().
  int imm19 = (int)i->Bits(23, 5);
  MOZ_ASSERT(vixl::IsInt19(imm19));

  b(i, imm19, Always);
}

void Assembler::ToggleToCmp(CodeLocationLabel inst_) {
  Instruction* i = (Instruction*)inst_.raw();
  MOZ_ASSERT(i->IsCondB());

  int imm19 = i->ImmCondBranch();
  // bit 23 is reserved, and the simulator throws an assertion when this happens
  // It'll be messy to decode, but we can steal bit 30 or bit 31.
  MOZ_ASSERT(vixl::IsInt18(imm19));

  // 31 - 64-bit if set, 32-bit if unset. (OK!)
  // 30 - sub if set, add if unset. (OK!)
  // 29 - SetFlagsBit. Must be set.
  // 22:23 - ShiftAddSub. (OK!)
  // 10:21 - ImmAddSub. (OK!)
  // 5:9 - First source register (Rn). (OK!)
  // 0:4 - Destination Register. Must be xzr.

  // From the above, there is a safe 19-bit contiguous region from 5:23.
  Emit(i, vixl::ThirtyTwoBits | vixl::AddSubImmediateFixed | vixl::SUB |
              Flags(vixl::SetFlags) | Rd(vixl::xzr) |
              (imm19 << vixl::Rn_offset));
}

void Assembler::ToggleCall(CodeLocationLabel inst_, bool enabled) {
  const Instruction* first = reinterpret_cast<Instruction*>(inst_.raw());
  Instruction* load;
  Instruction* call;

  // There might be a constant pool at the very first instruction.
  first = first->skipPool();

  // Skip the stack pointer restore instruction.
  if (first->IsStackPtrSync()) {
    first = first->InstructionAtOffset(vixl::kInstructionSize)->skipPool();
  }

  load = const_cast<Instruction*>(first);

  // The call instruction follows the load, but there may be an injected
  // constant pool.
  call = const_cast<Instruction*>(
      load->InstructionAtOffset(vixl::kInstructionSize)->skipPool());

  if (call->IsBLR() == enabled) {
    return;
  }

  if (call->IsBLR()) {
    // If the second instruction is blr(), then we have:
    //   ldr x17, [pc, offset]
    //   blr x17
    MOZ_ASSERT(load->IsLDR());
    // We want to transform this to:
    //   adr xzr, [pc, offset]
    //   nop
    int32_t offset = load->ImmLLiteral();
    adr(load, xzr, int32_t(offset));
    nop(call);
  } else {
    // We have:
    //   adr xzr, [pc, offset] (or ldr x17, [pc, offset])
    //   nop
    MOZ_ASSERT(load->IsADR() || load->IsLDR());
    MOZ_ASSERT(call->IsNOP());
    // Transform this to:
    //   ldr x17, [pc, offset]
    //   blr x17
    int32_t offset = (int)load->ImmPCRawOffset();
    MOZ_ASSERT(vixl::IsInt19(offset));
    ldr(load, ScratchReg2_64, int32_t(offset));
    blr(call, ScratchReg2_64);
  }
}

// Patches loads generated by MacroAssemblerCompat::mov(CodeLabel*, Register).
// The loading code is implemented in movePatchablePtr().
void Assembler::UpdateLoad64Value(Instruction* inst0, uint64_t value) {
  MOZ_ASSERT(inst0->IsLDR());
  uint64_t* literal = inst0->LiteralAddress<uint64_t*>();
  *literal = value;
}

class RelocationIterator {
  CompactBufferReader reader_;
  uint32_t offset_ = 0;

 public:
  explicit RelocationIterator(CompactBufferReader& reader) : reader_(reader) {}

  bool read() {
    if (!reader_.more()) {
      return false;
    }
    offset_ = reader_.readUnsigned();
    return true;
  }

  uint32_t offset() const { return offset_; }
};

static JitCode* CodeFromJump(JitCode* code, uint8_t* jump) {
  const Instruction* inst = (const Instruction*)jump;
  uint8_t* target;

  // We're expecting a call created by MacroAssembler::call(JitCode*).
  // It looks like:
  //
  //   ldr scratch, [pc, offset]
  //   blr scratch
  //
  // If the call has been toggled by ToggleCall(), it looks like:
  //
  //   adr xzr, [pc, offset]
  //   nop
  //
  // There might be a constant pool at the very first instruction.
  // See also ToggleCall().
  inst = inst->skipPool();

  // Skip the stack pointer restore instruction.
  if (inst->IsStackPtrSync()) {
    inst = inst->InstructionAtOffset(vixl::kInstructionSize)->skipPool();
  }

  if (inst->BranchType() != vixl::UnknownBranchType) {
    // This is an immediate branch.
    target = (uint8_t*)inst->ImmPCOffsetTarget();
  } else if (inst->IsLDR()) {
    // This is an ldr+blr call that is enabled. See ToggleCall().
    mozilla::DebugOnly<const Instruction*> nextInst =
        inst->InstructionAtOffset(vixl::kInstructionSize)->skipPool();
    MOZ_ASSERT(nextInst->IsNOP() || nextInst->IsBLR());
    target = (uint8_t*)inst->Literal64();
  } else if (inst->IsADR()) {
    // This is a disabled call: adr+nop. See ToggleCall().
    mozilla::DebugOnly<const Instruction*> nextInst =
        inst->InstructionAtOffset(vixl::kInstructionSize)->skipPool();
    MOZ_ASSERT(nextInst->IsNOP());
    ptrdiff_t offset = inst->ImmPCRawOffset() << vixl::kLiteralEntrySizeLog2;
    // This is what Literal64 would do with the corresponding ldr.
    memcpy(&target, inst + offset, sizeof(target));
  } else {
    MOZ_CRASH("Unrecognized jump instruction.");
  }

  // If the jump is within the code buffer, it uses the extended jump table.
  if (target >= code->raw() &&
      target < code->raw() + code->instructionsSize()) {
    MOZ_ASSERT(target + Assembler::SizeOfJumpTableEntry <=
               code->raw() + code->instructionsSize());

    uint8_t** patchablePtr =
        (uint8_t**)(target + Assembler::OffsetOfJumpTableEntryPointer);
    target = *patchablePtr;
  }

  return JitCode::FromExecutable(target);
}

void Assembler::TraceJumpRelocations(JSTracer* trc, JitCode* code,
                                     CompactBufferReader& reader) {
  RelocationIterator iter(reader);
  while (iter.read()) {
    JitCode* child = CodeFromJump(code, code->raw() + iter.offset());
    TraceManuallyBarrieredEdge(trc, &child, "rel32");
    MOZ_ASSERT(child == CodeFromJump(code, code->raw() + iter.offset()));
  }
}

/* static */
void Assembler::TraceDataRelocations(JSTracer* trc, JitCode* code,
                                     CompactBufferReader& reader) {
  mozilla::Maybe<AutoWritableJitCode> awjc;

  uint8_t* buffer = code->raw();

  while (reader.more()) {
    size_t offset = reader.readUnsigned();
    Instruction* load = (Instruction*)&buffer[offset];

    // The only valid traceable operation is a 64-bit load to an ARMRegister.
    // Refer to movePatchablePtr() for generation.
    MOZ_ASSERT(load->Mask(vixl::LoadLiteralMask) == vixl::LDR_x_lit);

    uintptr_t* literalAddr = load->LiteralAddress<uintptr_t*>();
    uintptr_t literal = *literalAddr;

    // Data relocations can be for Values or for raw pointers. If a Value is
    // zero-tagged, we can trace it as if it were a raw pointer. If a Value
    // is not zero-tagged, we have to interpret it as a Value to ensure that the
    // tag bits are masked off to recover the actual pointer.

    if (literal >> JSVAL_TAG_SHIFT) {
      // This relocation is a Value with a non-zero tag.
      Value v = Value::fromRawBits(literal);
      TraceManuallyBarrieredEdge(trc, &v, "jit-masm-value");
      if (*literalAddr != v.asRawBits()) {
        if (awjc.isNothing()) {
          awjc.emplace(code);
        }
        *literalAddr = v.asRawBits();
      }
      continue;
    }

    // This relocation is a raw pointer or a Value with a zero tag.
    // No barriers needed since the pointers are constants.
    gc::Cell* cell = reinterpret_cast<gc::Cell*>(literal);
    MOZ_ASSERT(gc::IsCellPointerValid(cell));
    TraceManuallyBarrieredGenericPointerEdge(trc, &cell, "jit-masm-ptr");
    if (uintptr_t(cell) != literal) {
      if (awjc.isNothing()) {
        awjc.emplace(code);
      }
      *literalAddr = uintptr_t(cell);
    }
  }
}

void Assembler::retarget(Label* label, Label* target) {
#ifdef JS_DISASM_ARM64
  spew_.spewRetarget(label, target);
#endif
  if (label->used()) {
    if (target->bound()) {
      bind(label, BufferOffset(target));
    } else if (target->used()) {
      // The target is not bound but used. Prepend label's branch list
      // onto target's.
      BufferOffset labelBranchOffset(label);

      // Find the head of the use chain for label.
      BufferOffset next = NextLink(labelBranchOffset);
      while (next.assigned()) {
        labelBranchOffset = next;
        next = NextLink(next);
      }

      // Then patch the head of label's use chain to the tail of target's
      // use chain, prepending the entire use chain of target.
      SetNextLink(labelBranchOffset, BufferOffset(target));
      target->use(label->offset());
    } else {
      // The target is unbound and unused. We can just take the head of
      // the list hanging off of label, and dump that into target.
      target->use(label->offset());
    }
  }
  label->reset();
}

}  // namespace jit
}  // namespace js

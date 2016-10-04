/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/arm64/Assembler-arm64.h"

#include "mozilla/DebugOnly.h"
#include "mozilla/MathAlgorithms.h"

#include "jscompartment.h"
#include "jsutil.h"

#include "gc/Marking.h"

#include "jit/arm64/Architecture-arm64.h"
#include "jit/arm64/MacroAssembler-arm64.h"
#include "jit/ExecutableAllocator.h"
#include "jit/JitCompartment.h"

using namespace js;
using namespace js::jit;

using mozilla::CountLeadingZeroes32;
using mozilla::DebugOnly;

// Note this is used for inter-AsmJS calls and may pass arguments and results
// in floating point registers even if the system ABI does not.

ABIArg
ABIArgGenerator::next(MIRType type)
{
    switch (type) {
      case MIRType_Int32:
      case MIRType_Pointer:
        if (intRegIndex_ == NumIntArgRegs) {
            current_ = ABIArg(stackOffset_);
            stackOffset_ += sizeof(uintptr_t);
            break;
        }
        current_ = ABIArg(Register::FromCode(intRegIndex_));
        intRegIndex_++;
        break;

      case MIRType_Float32:
      case MIRType_Double:
        if (floatRegIndex_ == NumFloatArgRegs) {
            current_ = ABIArg(stackOffset_);
            stackOffset_ += sizeof(double);
            break;
        }
        current_ = ABIArg(FloatRegister(floatRegIndex_,
                                        type == MIRType_Double ? FloatRegisters::Double
                                                               : FloatRegisters::Single));
        floatRegIndex_++;
        break;

      default:
        MOZ_CRASH("Unexpected argument type");
    }
    return current_;
}

const Register ABIArgGenerator::NonArgReturnReg0 = r8;
const Register ABIArgGenerator::NonArgReturnReg1 = r9;
const Register ABIArgGenerator::NonVolatileReg = r1;
const Register ABIArgGenerator::NonArg_VolatileReg = r13;
const Register ABIArgGenerator::NonReturn_VolatileReg0 = r2;
const Register ABIArgGenerator::NonReturn_VolatileReg1 = r3;

namespace js {
namespace jit {

void
Assembler::finish()
{
    armbuffer_.flushPool();

    // The extended jump table is part of the code buffer.
    ExtendedJumpTable_ = emitExtendedJumpTable();
    Assembler::FinalizeCode();

    // The jump relocation table starts with a fixed-width integer pointing
    // to the start of the extended jump table.
    // Space for this integer is allocated by Assembler::addJumpRelocation()
    // before writing the first entry.
    // Don't touch memory if we saw an OOM error.
    if (jumpRelocations_.length() && !oom()) {
        MOZ_ASSERT(jumpRelocations_.length() >= sizeof(uint32_t));
        *(uint32_t*)jumpRelocations_.buffer() = ExtendedJumpTable_.getOffset();
    }
}

BufferOffset
Assembler::emitExtendedJumpTable()
{
    if (!pendingJumps_.length() || oom())
        return BufferOffset();

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

        ldr(vixl::ip0, ptrdiff_t(8 / vixl::kInstructionSize));
        br(vixl::ip0);

        DebugOnly<size_t> prePointer = size_t(armbuffer_.nextOffset().getOffset());
        MOZ_ASSERT_IF(!oom(), prePointer - preOffset == OffsetOfJumpTableEntryPointer);

        brk(0x0);
        brk(0x0);

        DebugOnly<size_t> postOffset = size_t(armbuffer_.nextOffset().getOffset());

        MOZ_ASSERT_IF(!oom(), postOffset - preOffset == SizeOfJumpTableEntry);
    }

    if (oom())
        return BufferOffset();

    return tableOffset;
}

void
Assembler::executableCopy(uint8_t* buffer)
{
    // Copy the code and all constant pools into the output buffer.
    armbuffer_.executableCopy(buffer);

    // Patch any relative jumps that target code outside the buffer.
    // The extended jump table may be used for distant jumps.
    for (size_t i = 0; i < pendingJumps_.length(); i++) {
        RelativePatch& rp = pendingJumps_[i];

        if (!rp.target) {
            // The patch target is nullptr for jumps that have been linked to
            // a label within the same code block, but may be repatched later
            // to jump to a different code block.
            continue;
        }

        Instruction* target = (Instruction*)rp.target;
        Instruction* branch = (Instruction*)(buffer + rp.offset.getOffset());
        JumpTableEntry* extendedJumpTable =
            reinterpret_cast<JumpTableEntry*>(buffer + ExtendedJumpTable_.getOffset());
        if (branch->BranchType() != vixl::UnknownBranchType) {
            if (branch->IsTargetReachable(target)) {
                branch->SetImmPCOffsetTarget(target);
            } else {
                JumpTableEntry* entry = &extendedJumpTable[i];
                branch->SetImmPCOffsetTarget(entry->getLdr());
                entry->data = target;
            }
        } else {
            // Currently a two-instruction call, it should be possible to optimize this
            // into a single instruction call + nop in some instances, but this will work.
        }
    }
}

BufferOffset
Assembler::immPool(ARMRegister dest, uint8_t* value, vixl::LoadLiteralOp op, ARMBuffer::PoolEntry* pe)
{
    uint32_t inst = op | Rt(dest);
    const size_t numInst = 1;
    const unsigned sizeOfPoolEntryInBytes = 4;
    const unsigned numPoolEntries = sizeof(value) / sizeOfPoolEntryInBytes;
    return allocEntry(numInst, numPoolEntries, (uint8_t*)&inst, value, pe);
}

BufferOffset
Assembler::immPool64(ARMRegister dest, uint64_t value, ARMBuffer::PoolEntry* pe)
{
    return immPool(dest, (uint8_t*)&value, vixl::LDR_x_lit, pe);
}

BufferOffset
Assembler::immPool64Branch(RepatchLabel* label, ARMBuffer::PoolEntry* pe, Condition c)
{
    MOZ_CRASH("immPool64Branch");
}

BufferOffset
Assembler::fImmPool(ARMFPRegister dest, uint8_t* value, vixl::LoadLiteralOp op)
{
    uint32_t inst = op | Rt(dest);
    const size_t numInst = 1;
    const unsigned sizeOfPoolEntryInBits = 32;
    const unsigned numPoolEntries = dest.size() / sizeOfPoolEntryInBits;
    return allocEntry(numInst, numPoolEntries, (uint8_t*)&inst, value);
}

BufferOffset
Assembler::fImmPool64(ARMFPRegister dest, double value)
{
    return fImmPool(dest, (uint8_t*)&value, vixl::LDR_d_lit);
}
BufferOffset
Assembler::fImmPool32(ARMFPRegister dest, float value)
{
    return fImmPool(dest, (uint8_t*)&value, vixl::LDR_s_lit);
}

void
Assembler::bind(Label* label, BufferOffset targetOffset)
{
    // Nothing has seen the label yet: just mark the location.
    // If we've run out of memory, don't attempt to modify the buffer which may
    // not be there. Just mark the label as bound to the (possibly bogus)
    // targetOffset.
    if (!label->used() || oom()) {
        label->bind(targetOffset.getOffset());
        return;
    }

    // Get the most recent instruction that used the label, as stored in the label.
    // This instruction is the head of an implicit linked list of label uses.
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
        ptrdiff_t relativeByteOffset = targetOffset.getOffset() - branchOffset.getOffset();
        Instruction* link = getInstructionAt(branchOffset);

        // This branch may still be registered for callbacks. Stop tracking it.
        vixl::ImmBranchType branchType = link->BranchType();
        vixl::ImmBranchRangeType branchRange = Instruction::ImmBranchTypeToRange(branchType);
        if (branchRange < vixl::NumShortBranchRangeTypes) {
            BufferOffset deadline(branchOffset.getOffset() +
                                  Instruction::ImmBranchMaxForwardOffset(branchRange));
            armbuffer_.unregisterBranchDeadline(branchRange, deadline);
        }

        // Is link able to reach the label?
        if (link->IsPCRelAddressing() || link->IsTargetReachable(link + relativeByteOffset)) {
            // Write a new relative offset into the instruction.
            link->SetImmPCOffsetTarget(link + relativeByteOffset);
        } else {
            // This is a short-range branch, and it can't reach the label directly.
            // Verify that it branches to a veneer: an unconditional branch.
            MOZ_ASSERT(getInstructionAt(nextOffset)->BranchType() == vixl::UncondBranchType);
        }

        branchOffset = nextOffset;
    }

    // Bind the label, so that future uses may encode the offset immediately.
    label->bind(targetOffset.getOffset());
}

void
Assembler::bind(RepatchLabel* label)
{
    // Nothing has seen the label yet: just mark the location.
    // If we've run out of memory, don't attempt to modify the buffer which may
    // not be there. Just mark the label as bound to nextOffset().
    if (!label->used() || oom()) {
        label->bind(nextOffset().getOffset());
        return;
    }
    int branchOffset = label->offset();
    Instruction* inst = getInstructionAt(BufferOffset(branchOffset));
    inst->SetImmPCOffsetTarget(inst + nextOffset().getOffset() - branchOffset);
}

void
Assembler::trace(JSTracer* trc)
{
    for (size_t i = 0; i < pendingJumps_.length(); i++) {
        RelativePatch& rp = pendingJumps_[i];
        if (rp.kind == Relocation::JITCODE) {
            JitCode* code = JitCode::FromExecutable((uint8_t*)rp.target);
            TraceManuallyBarrieredEdge(trc, &code, "masmrel32");
            MOZ_ASSERT(code == JitCode::FromExecutable((uint8_t*)rp.target));
        }
    }

    // TODO: Trace.
#if 0
    if (tmpDataRelocations_.length())
        ::TraceDataRelocations(trc, &armbuffer_, &tmpDataRelocations_);
#endif
}

void
Assembler::addJumpRelocation(BufferOffset src, Relocation::Kind reloc)
{
    // Only JITCODE relocations are patchable at runtime.
    MOZ_ASSERT(reloc == Relocation::JITCODE);

    // The jump relocation table starts with a fixed-width integer pointing
    // to the start of the extended jump table. But, we don't know the
    // actual extended jump table offset yet, so write a 0 which we'll
    // patch later in Assembler::finish().
    if (!jumpRelocations_.length())
        jumpRelocations_.writeFixedUint32_t(0);

    // Each entry in the table is an (offset, extendedTableIndex) pair.
    jumpRelocations_.writeUnsigned(src.getOffset());
    jumpRelocations_.writeUnsigned(pendingJumps_.length());
}

void
Assembler::addPendingJump(BufferOffset src, ImmPtr target, Relocation::Kind reloc)
{
    MOZ_ASSERT(target.value != nullptr);

    if (reloc == Relocation::JITCODE)
        addJumpRelocation(src, reloc);

    // This jump is not patchable at runtime. Extended jump table entry requirements
    // cannot be known until finalization, so to be safe, give each jump and entry.
    // This also causes GC tracing of the target.
    enoughMemory_ &= pendingJumps_.append(RelativePatch(src, target.value, reloc));
}

size_t
Assembler::addPatchableJump(BufferOffset src, Relocation::Kind reloc)
{
    MOZ_CRASH("TODO: This is currently unused (and untested)");
    if (reloc == Relocation::JITCODE)
        addJumpRelocation(src, reloc);

    size_t extendedTableIndex = pendingJumps_.length();
    enoughMemory_ &= pendingJumps_.append(RelativePatch(src, nullptr, reloc));
    return extendedTableIndex;
}

void
PatchJump(CodeLocationJump& jump_, CodeLocationLabel label, ReprotectCode reprotect)
{
    MOZ_CRASH("PatchJump");
}

void
Assembler::PatchDataWithValueCheck(CodeLocationLabel label, PatchedImmPtr newValue,
                                   PatchedImmPtr expected)
{
    Instruction* i = (Instruction*)label.raw();
    void** pValue = i->LiteralAddress<void**>();
    MOZ_ASSERT(*pValue == expected.value);
    *pValue = newValue.value;
}

void
Assembler::PatchDataWithValueCheck(CodeLocationLabel label, ImmPtr newValue, ImmPtr expected)
{
    PatchDataWithValueCheck(label, PatchedImmPtr(newValue.value), PatchedImmPtr(expected.value));
}

void
Assembler::ToggleToJmp(CodeLocationLabel inst_)
{
    Instruction* i = (Instruction*)inst_.raw();
    MOZ_ASSERT(i->IsAddSubImmediate());

    // Refer to instruction layout in ToggleToCmp().
    int imm19 = (int)i->Bits(23, 5);
    MOZ_ASSERT(vixl::is_int19(imm19));

    b(i, imm19, Always);
}

void
Assembler::ToggleToCmp(CodeLocationLabel inst_)
{
    Instruction* i = (Instruction*)inst_.raw();
    MOZ_ASSERT(i->IsCondB());

    int imm19 = i->ImmCondBranch();
    // bit 23 is reserved, and the simulator throws an assertion when this happens
    // It'll be messy to decode, but we can steal bit 30 or bit 31.
    MOZ_ASSERT(vixl::is_int18(imm19));

    // 31 - 64-bit if set, 32-bit if unset. (OK!)
    // 30 - sub if set, add if unset. (OK!)
    // 29 - SetFlagsBit. Must be set.
    // 22:23 - ShiftAddSub. (OK!)
    // 10:21 - ImmAddSub. (OK!)
    // 5:9 - First source register (Rn). (OK!)
    // 0:4 - Destination Register. Must be xzr.

    // From the above, there is a safe 19-bit contiguous region from 5:23.
    Emit(i, vixl::ThirtyTwoBits | vixl::AddSubImmediateFixed | vixl::SUB | Flags(vixl::SetFlags) |
            Rd(vixl::xzr) | (imm19 << vixl::Rn_offset));
}

void
Assembler::ToggleCall(CodeLocationLabel inst_, bool enabled)
{
    const Instruction* first = reinterpret_cast<Instruction*>(inst_.raw());
    Instruction* load;
    Instruction* call;

    // There might be a constant pool at the very first instruction.
    first = first->skipPool();

    // Skip the stack pointer restore instruction.
    if (first->IsStackPtrSync())
        first = first->InstructionAtOffset(vixl::kInstructionSize)->skipPool();

    load = const_cast<Instruction*>(first);

    // The call instruction follows the load, but there may be an injected
    // constant pool.
    call = const_cast<Instruction*>(load->InstructionAtOffset(vixl::kInstructionSize)->skipPool());

    if (call->IsBLR() == enabled)
        return;

    if (call->IsBLR()) {
        // If the second instruction is blr(), then wehave:
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
        MOZ_ASSERT(vixl::is_int19(offset));
        ldr(load, ScratchReg2_64, int32_t(offset));
        blr(call, ScratchReg2_64);
    }
}

class RelocationIterator
{
    CompactBufferReader reader_;
    uint32_t tableStart_;
    uint32_t offset_;
    uint32_t extOffset_;

  public:
    explicit RelocationIterator(CompactBufferReader& reader)
      : reader_(reader)
    {
        // The first uint32_t stores the extended table offset.
        tableStart_ = reader_.readFixedUint32_t();
    }

    bool read() {
        if (!reader_.more())
            return false;
        offset_ = reader_.readUnsigned();
        extOffset_ = reader_.readUnsigned();
        return true;
    }

    uint32_t offset() const {
        return offset_;
    }
    uint32_t extendedOffset() const {
        return extOffset_;
    }
};

static JitCode*
CodeFromJump(JitCode* code, uint8_t* jump)
{
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
    if (inst->IsStackPtrSync())
        inst = inst->InstructionAtOffset(vixl::kInstructionSize)->skipPool();

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
    if (target >= code->raw() && target < code->raw() + code->instructionsSize()) {
        MOZ_ASSERT(target + Assembler::SizeOfJumpTableEntry <= code->raw() + code->instructionsSize());

        uint8_t** patchablePtr = (uint8_t**)(target + Assembler::OffsetOfJumpTableEntryPointer);
        target = *patchablePtr;
    }

    return JitCode::FromExecutable(target);
}

void
Assembler::TraceJumpRelocations(JSTracer* trc, JitCode* code, CompactBufferReader& reader)
{
    RelocationIterator iter(reader);
    while (iter.read()) {
        JitCode* child = CodeFromJump(code, code->raw() + iter.offset());
        TraceManuallyBarrieredEdge(trc, &child, "rel32");
        MOZ_ASSERT(child == CodeFromJump(code, code->raw() + iter.offset()));
    }
}

static void
TraceDataRelocations(JSTracer* trc, uint8_t* buffer, CompactBufferReader& reader)
{
    while (reader.more()) {
        size_t offset = reader.readUnsigned();
        Instruction* load = (Instruction*)&buffer[offset];

        // The only valid traceable operation is a 64-bit load to an ARMRegister.
        // Refer to movePatchablePtr() for generation.
        MOZ_ASSERT(load->Mask(vixl::LoadLiteralMask) == vixl::LDR_x_lit);

        uintptr_t* literalAddr = load->LiteralAddress<uintptr_t*>();
        uintptr_t literal = *literalAddr;

        // All pointers on AArch64 will have the top bits cleared.
        // If those bits are not cleared, this must be a Value.
        if (literal >> JSVAL_TAG_SHIFT) {
            jsval_layout layout;
            layout.asBits = literal;
            Value v = IMPL_TO_JSVAL(layout);
            TraceManuallyBarrieredEdge(trc, &v, "ion-masm-value");
            *literalAddr = JSVAL_TO_IMPL(v).asBits;

            // TODO: When we can, flush caches here if a pointer was moved.
            continue;
        }

        // No barriers needed since the pointers are constants.
        TraceManuallyBarrieredGenericPointerEdge(trc, reinterpret_cast<gc::Cell**>(literalAddr),
                                                 "ion-masm-ptr");

        // TODO: Flush caches at end?
    }
}

void
Assembler::TraceDataRelocations(JSTracer* trc, JitCode* code, CompactBufferReader& reader)
{
    ::TraceDataRelocations(trc, code->raw(), reader);
}

void
Assembler::FixupNurseryObjects(JSContext* cx, JitCode* code, CompactBufferReader& reader,
                               const ObjectVector& nurseryObjects)
{

    MOZ_ASSERT(!nurseryObjects.empty());

    uint8_t* buffer = code->raw();
    bool hasNurseryPointers = false;

    while (reader.more()) {
        size_t offset = reader.readUnsigned();
        Instruction* ins = (Instruction*)&buffer[offset];

        uintptr_t* literalAddr = ins->LiteralAddress<uintptr_t*>();
        uintptr_t literal = *literalAddr;

        if (literal >> JSVAL_TAG_SHIFT)
            continue; // This is a Value.

        if (!(literal & 0x1))
            continue;

        uint32_t index = literal >> 1;
        JSObject* obj = nurseryObjects[index];
        *literalAddr = uintptr_t(obj);

        // Either all objects are still in the nursery, or all objects are tenured.
        MOZ_ASSERT_IF(hasNurseryPointers, IsInsideNursery(obj));

        if (!hasNurseryPointers && IsInsideNursery(obj))
            hasNurseryPointers = true;
    }

    if (hasNurseryPointers)
        cx->runtime()->gc.storeBuffer.putWholeCell(code);
}

void
Assembler::PatchInstructionImmediate(uint8_t* code, PatchedImmPtr imm)
{
    MOZ_CRASH("PatchInstructionImmediate()");
}

void
Assembler::UpdateBoundsCheck(uint32_t heapSize, Instruction* inst)
{
    int32_t mask = ~(heapSize - 1);
    unsigned n, imm_s, imm_r;
    if (!IsImmLogical(mask, 32, &n, &imm_s, &imm_r))
        MOZ_CRASH("Could not encode immediate!?");

    inst->SetImmR(imm_r);
    inst->SetImmS(imm_s);
    inst->SetBitN(n);
}

void
Assembler::retarget(Label* label, Label* target)
{
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
            DebugOnly<uint32_t> prev = target->use(label->offset());
            MOZ_ASSERT((int32_t)prev == Label::INVALID_OFFSET);
        }
    }
    label->reset();
}

} // namespace jit
} // namespace js

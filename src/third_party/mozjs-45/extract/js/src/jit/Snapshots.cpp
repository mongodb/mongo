/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/Snapshots.h"

#include "jsscript.h"

#include "jit/CompileInfo.h"
#include "jit/JitSpewer.h"
#ifdef TRACK_SNAPSHOTS
# include "jit/LIR.h"
#endif
#include "jit/MIR.h"
#include "jit/Recover.h"

#include "vm/Printer.h"

using namespace js;
using namespace js::jit;

// Encodings:
//   [ptr] A fixed-size pointer.
//   [vwu] A variable-width unsigned integer.
//   [vws] A variable-width signed integer.
//    [u8] An 8-bit unsigned integer.
//   [u8'] An 8-bit unsigned integer which is potentially extended with packed
//         data.
//   [u8"] Packed data which is stored and packed in the previous [u8'].
//  [vwu*] A list of variable-width unsigned integers.
//   [pld] Payload of Recover Value Allocation:
//         PAYLOAD_NONE:
//           There is no payload.
//
//         PAYLOAD_INDEX:
//           [vwu] Index, such as the constant pool index.
//
//         PAYLOAD_STACK_OFFSET:
//           [vws] Stack offset based on the base of the Ion frame.
//
//         PAYLOAD_GPR:
//            [u8] Code of the general register.
//
//         PAYLOAD_FPU:
//            [u8] Code of the FPU register.
//
//         PAYLOAD_PACKED_TAG:
//           [u8"] Bits 5-7: JSValueType is encoded on the low bits of the Mode
//                           of the RValueAllocation.
//
// Snapshot header:
//
//   [vwu] bits ((n+1)-31]: recover instruction offset
//         bits [0,n): bailout kind (n = SNAPSHOT_BAILOUTKIND_BITS)
//
// Snapshot body, repeated "frame count" times, from oldest frame to newest frame.
// Note that the first frame doesn't have the "parent PC" field.
//
//   [ptr] Debug only: JSScript*
//   [vwu] pc offset
//   [vwu] # of RVA's indexes, including nargs
//  [vwu*] List of indexes to R(ecover)ValueAllocation table. Contains
//         nargs + nfixed + stackDepth items.
//
// Recover value allocations are encoded at the end of the Snapshot buffer, and
// they are padded on ALLOCATION_TABLE_ALIGNMENT.  The encoding of each
// allocation is determined by the RValueAllocation::Layout, which can be
// obtained from the RValueAllocation::Mode with layoutFromMode function.  The
// layout structure list the type of payload which are used to serialized /
// deserialized / dumped the content of the allocations.
//
// R(ecover)ValueAllocation items:
//   [u8'] Mode, which defines the type of the payload as well as the
//         interpretation.
//   [pld] first payload (packed tag, index, stack offset, register, ...)
//   [pld] second payload (register, stack offset, none)
//
//       Modes:
//         CONSTANT [INDEX]
//           Index into the constant pool.
//
//         CST_UNDEFINED []
//           Constant value which correspond to the "undefined" JS value.
//
//         CST_NULL []
//           Constant value which correspond to the "null" JS value.
//
//         DOUBLE_REG [FPU_REG]
//           Double value stored in a FPU register.
//
//         ANY_FLOAT_REG [FPU_REG]
//           Any Float value (float32, simd) stored in a FPU register.
//
//         ANY_FLOAT_STACK [STACK_OFFSET]
//           Any Float value (float32, simd) stored on the stack.
//
//         UNTYPED_REG   [GPR_REG]
//         UNTYPED_STACK [STACK_OFFSET]
//         UNTYPED_REG_REG     [GPR_REG,      GPR_REG]
//         UNTYPED_REG_STACK   [GPR_REG,      STACK_OFFSET]
//         UNTYPED_STACK_REG   [STACK_OFFSET, GPR_REG]
//         UNTYPED_STACK_STACK [STACK_OFFSET, STACK_OFFSET]
//           Value with dynamically known type. On 32 bits architecture, the
//           first register/stack-offset correspond to the holder of the type,
//           and the second correspond to the payload of the JS Value.
//
//         RECOVER_INSTRUCTION [INDEX]
//           Index into the list of recovered instruction results.
//
//         RI_WITH_DEFAULT_CST [INDEX] [INDEX]
//           The first payload is the index into the list of recovered
//           instruction results.  The second payload is the index in the
//           constant pool.
//
//         TYPED_REG [PACKED_TAG, GPR_REG]:
//           Value with statically known type, which payload is stored in a
//           register.
//
//         TYPED_STACK [PACKED_TAG, STACK_OFFSET]:
//           Value with statically known type, which payload is stored at an
//           offset on the stack.
//

const RValueAllocation::Layout&
RValueAllocation::layoutFromMode(Mode mode)
{
    switch (mode) {
      case CONSTANT: {
        static const RValueAllocation::Layout layout = {
            PAYLOAD_INDEX,
            PAYLOAD_NONE,
            "constant"
        };
        return layout;
      }

      case CST_UNDEFINED: {
        static const RValueAllocation::Layout layout = {
            PAYLOAD_NONE,
            PAYLOAD_NONE,
            "undefined"
        };
        return layout;
      }

      case CST_NULL: {
        static const RValueAllocation::Layout layout = {
            PAYLOAD_NONE,
            PAYLOAD_NONE,
            "null"
        };
        return layout;
      }

      case DOUBLE_REG: {
        static const RValueAllocation::Layout layout = {
            PAYLOAD_FPU,
            PAYLOAD_NONE,
            "double"
        };
        return layout;
      }
      case ANY_FLOAT_REG: {
        static const RValueAllocation::Layout layout = {
            PAYLOAD_FPU,
            PAYLOAD_NONE,
            "float register content"
        };
        return layout;
      }
      case ANY_FLOAT_STACK: {
        static const RValueAllocation::Layout layout = {
            PAYLOAD_STACK_OFFSET,
            PAYLOAD_NONE,
            "float register content"
        };
        return layout;
      }
#if defined(JS_NUNBOX32)
      case UNTYPED_REG_REG: {
        static const RValueAllocation::Layout layout = {
            PAYLOAD_GPR,
            PAYLOAD_GPR,
            "value"
        };
        return layout;
      }
      case UNTYPED_REG_STACK: {
        static const RValueAllocation::Layout layout = {
            PAYLOAD_GPR,
            PAYLOAD_STACK_OFFSET,
            "value"
        };
        return layout;
      }
      case UNTYPED_STACK_REG: {
        static const RValueAllocation::Layout layout = {
            PAYLOAD_STACK_OFFSET,
            PAYLOAD_GPR,
            "value"
        };
        return layout;
      }
      case UNTYPED_STACK_STACK: {
        static const RValueAllocation::Layout layout = {
            PAYLOAD_STACK_OFFSET,
            PAYLOAD_STACK_OFFSET,
            "value"
        };
        return layout;
      }
#elif defined(JS_PUNBOX64)
      case UNTYPED_REG: {
        static const RValueAllocation::Layout layout = {
            PAYLOAD_GPR,
            PAYLOAD_NONE,
            "value"
        };
        return layout;
      }
      case UNTYPED_STACK: {
        static const RValueAllocation::Layout layout = {
            PAYLOAD_STACK_OFFSET,
            PAYLOAD_NONE,
            "value"
        };
        return layout;
      }
#endif
      case RECOVER_INSTRUCTION: {
        static const RValueAllocation::Layout layout = {
            PAYLOAD_INDEX,
            PAYLOAD_NONE,
            "instruction"
        };
        return layout;
      }
      case RI_WITH_DEFAULT_CST: {
        static const RValueAllocation::Layout layout = {
            PAYLOAD_INDEX,
            PAYLOAD_INDEX,
            "instruction with default"
        };
        return layout;
      }

      default: {
        static const RValueAllocation::Layout regLayout = {
            PAYLOAD_PACKED_TAG,
            PAYLOAD_GPR,
            "typed value"
        };

        static const RValueAllocation::Layout stackLayout = {
            PAYLOAD_PACKED_TAG,
            PAYLOAD_STACK_OFFSET,
            "typed value"
        };

        if (mode >= TYPED_REG_MIN && mode <= TYPED_REG_MAX)
            return regLayout;
        if (mode >= TYPED_STACK_MIN && mode <= TYPED_STACK_MAX)
            return stackLayout;
      }
    }

    MOZ_CRASH("Wrong mode type?");
}

// Pad serialized RValueAllocations by a multiple of X bytes in the allocation
// buffer.  By padding serialized value allocations, we are building an
// indexable table of elements of X bytes, and thus we can safely divide any
// offset within the buffer by X to obtain an index.
//
// By padding, we are loosing space within the allocation buffer, but we
// multiple by X the number of indexes that we can store on one byte in each
// snapshots.
//
// Some value allocations are taking more than X bytes to be encoded, in which
// case we will pad to a multiple of X, and we are wasting indexes. The choice
// of X should be balanced between the wasted padding of serialized value
// allocation, and the saving made in snapshot indexes.
static const size_t ALLOCATION_TABLE_ALIGNMENT = 2; /* bytes */

void
RValueAllocation::readPayload(CompactBufferReader& reader, PayloadType type,
                              uint8_t* mode, Payload* p)
{
    switch (type) {
      case PAYLOAD_NONE:
        break;
      case PAYLOAD_INDEX:
        p->index = reader.readUnsigned();
        break;
      case PAYLOAD_STACK_OFFSET:
        p->stackOffset = reader.readSigned();
        break;
      case PAYLOAD_GPR:
        p->gpr = Register::FromCode(reader.readByte());
        break;
      case PAYLOAD_FPU:
        p->fpu.data = reader.readByte();
        break;
      case PAYLOAD_PACKED_TAG:
        p->type = JSValueType(*mode & PACKED_TAG_MASK);
        *mode = *mode & ~PACKED_TAG_MASK;
        break;
    }
}

RValueAllocation
RValueAllocation::read(CompactBufferReader& reader)
{
    uint8_t mode = reader.readByte();
    const Layout& layout = layoutFromMode(Mode(mode & MODE_BITS_MASK));
    Payload arg1, arg2;

    readPayload(reader, layout.type1, &mode, &arg1);
    readPayload(reader, layout.type2, &mode, &arg2);
    return RValueAllocation(Mode(mode), arg1, arg2);
}

void
RValueAllocation::writePayload(CompactBufferWriter& writer, PayloadType type, Payload p)
{
    switch (type) {
      case PAYLOAD_NONE:
        break;
      case PAYLOAD_INDEX:
        writer.writeUnsigned(p.index);
        break;
      case PAYLOAD_STACK_OFFSET:
        writer.writeSigned(p.stackOffset);
        break;
      case PAYLOAD_GPR:
        static_assert(Registers::Total <= 0x100,
                      "Not enough bytes to encode all registers.");
        writer.writeByte(p.gpr.code());
        break;
      case PAYLOAD_FPU:
        static_assert(FloatRegisters::Total <= 0x100,
                      "Not enough bytes to encode all float registers.");
        writer.writeByte(p.fpu.code());
        break;
      case PAYLOAD_PACKED_TAG: {
        // This code assumes that the PACKED_TAG payload is following the
        // writeByte of the mode.
        if (!writer.oom()) {
            MOZ_ASSERT(writer.length());
            uint8_t* mode = writer.buffer() + (writer.length() - 1);
            MOZ_ASSERT((*mode & PACKED_TAG_MASK) == 0 && (p.type & ~PACKED_TAG_MASK) == 0);
            *mode = *mode | p.type;
        }
        break;
      }
    }
}

void
RValueAllocation::writePadding(CompactBufferWriter& writer)
{
    // Write 0x7f in all padding bytes.
    while (writer.length() % ALLOCATION_TABLE_ALIGNMENT)
        writer.writeByte(0x7f);
}

void
RValueAllocation::write(CompactBufferWriter& writer) const
{
    const Layout& layout = layoutFromMode(mode());
    MOZ_ASSERT(layout.type2 != PAYLOAD_PACKED_TAG);
    MOZ_ASSERT(writer.length() % ALLOCATION_TABLE_ALIGNMENT == 0);

    writer.writeByte(mode_);
    writePayload(writer, layout.type1, arg1_);
    writePayload(writer, layout.type2, arg2_);
    writePadding(writer);
}

HashNumber
RValueAllocation::hash() const {
    CompactBufferWriter writer;
    write(writer);

    // We should never oom because the compact buffer writer has 32 inlined
    // bytes, and in the worse case scenario, only encode 12 bytes
    // (12 == mode + signed + signed + pad).
    MOZ_ASSERT(!writer.oom());
    MOZ_ASSERT(writer.length() <= 12);

    HashNumber res = 0;
    for (size_t i = 0; i < writer.length(); i++) {
        res = ((res << 8) | (res >> (sizeof(res) - 1)));
        res ^= writer.buffer()[i];
    }
    return res;
}

static const char*
ValTypeToString(JSValueType type)
{
    switch (type) {
      case JSVAL_TYPE_INT32:
        return "int32_t";
      case JSVAL_TYPE_DOUBLE:
        return "double";
      case JSVAL_TYPE_STRING:
        return "string";
      case JSVAL_TYPE_SYMBOL:
        return "symbol";
      case JSVAL_TYPE_BOOLEAN:
        return "boolean";
      case JSVAL_TYPE_OBJECT:
        return "object";
      case JSVAL_TYPE_MAGIC:
        return "magic";
      default:
        MOZ_CRASH("no payload");
    }
}

void
RValueAllocation::dumpPayload(GenericPrinter& out, PayloadType type, Payload p)
{
    switch (type) {
      case PAYLOAD_NONE:
        break;
      case PAYLOAD_INDEX:
        out.printf("index %u", p.index);
        break;
      case PAYLOAD_STACK_OFFSET:
        out.printf("stack %d", p.stackOffset);
        break;
      case PAYLOAD_GPR:
        out.printf("reg %s", p.gpr.name());
        break;
      case PAYLOAD_FPU:
        out.printf("reg %s", p.fpu.name());
        break;
      case PAYLOAD_PACKED_TAG:
        out.printf("%s", ValTypeToString(p.type));
        break;
    }
}

void
RValueAllocation::dump(GenericPrinter& out) const
{
    const Layout& layout = layoutFromMode(mode());
    out.printf("%s", layout.name);

    if (layout.type1 != PAYLOAD_NONE)
        out.printf(" (");
    dumpPayload(out, layout.type1, arg1_);
    if (layout.type2 != PAYLOAD_NONE)
        out.printf(", ");
    dumpPayload(out, layout.type2, arg2_);
    if (layout.type1 != PAYLOAD_NONE)
        out.printf(")");
}

bool
RValueAllocation::equalPayloads(PayloadType type, Payload lhs, Payload rhs)
{
    switch (type) {
      case PAYLOAD_NONE:
        return true;
      case PAYLOAD_INDEX:
        return lhs.index == rhs.index;
      case PAYLOAD_STACK_OFFSET:
        return lhs.stackOffset == rhs.stackOffset;
      case PAYLOAD_GPR:
        return lhs.gpr == rhs.gpr;
      case PAYLOAD_FPU:
        return lhs.fpu == rhs.fpu;
      case PAYLOAD_PACKED_TAG:
        return lhs.type == rhs.type;
    }

    return false;
}

SnapshotReader::SnapshotReader(const uint8_t* snapshots, uint32_t offset,
                               uint32_t RVATableSize, uint32_t listSize)
  : reader_(snapshots + offset, snapshots + listSize),
    allocReader_(snapshots + listSize, snapshots + listSize + RVATableSize),
    allocTable_(snapshots + listSize),
    allocRead_(0)
{
    if (!snapshots)
        return;
    JitSpew(JitSpew_IonSnapshots, "Creating snapshot reader");
    readSnapshotHeader();
}

#define COMPUTE_SHIFT_AFTER_(name) (name ## _BITS + name ##_SHIFT)
#define COMPUTE_MASK_(name) ((uint32_t(1 << name ## _BITS) - 1) << name ##_SHIFT)

// Details of snapshot header packing.
static const uint32_t SNAPSHOT_BAILOUTKIND_SHIFT = 0;
static const uint32_t SNAPSHOT_BAILOUTKIND_BITS = 6;
static const uint32_t SNAPSHOT_BAILOUTKIND_MASK = COMPUTE_MASK_(SNAPSHOT_BAILOUTKIND);

static const uint32_t SNAPSHOT_ROFFSET_SHIFT = COMPUTE_SHIFT_AFTER_(SNAPSHOT_BAILOUTKIND);
static const uint32_t SNAPSHOT_ROFFSET_BITS = 32 - SNAPSHOT_ROFFSET_SHIFT;
static const uint32_t SNAPSHOT_ROFFSET_MASK = COMPUTE_MASK_(SNAPSHOT_ROFFSET);

// Details of recover header packing.
static const uint32_t RECOVER_RESUMEAFTER_SHIFT = 0;
static const uint32_t RECOVER_RESUMEAFTER_BITS = 1;
static const uint32_t RECOVER_RESUMEAFTER_MASK = COMPUTE_MASK_(RECOVER_RESUMEAFTER);

static const uint32_t RECOVER_RINSCOUNT_SHIFT = COMPUTE_SHIFT_AFTER_(RECOVER_RESUMEAFTER);
static const uint32_t RECOVER_RINSCOUNT_BITS = 32 - RECOVER_RINSCOUNT_SHIFT;
static const uint32_t RECOVER_RINSCOUNT_MASK = COMPUTE_MASK_(RECOVER_RINSCOUNT);

#undef COMPUTE_MASK_
#undef COMPUTE_SHIFT_AFTER_

void
SnapshotReader::readSnapshotHeader()
{
    uint32_t bits = reader_.readUnsigned();

    bailoutKind_ = BailoutKind((bits & SNAPSHOT_BAILOUTKIND_MASK) >> SNAPSHOT_BAILOUTKIND_SHIFT);
    recoverOffset_ = (bits & SNAPSHOT_ROFFSET_MASK) >> SNAPSHOT_ROFFSET_SHIFT;

    JitSpew(JitSpew_IonSnapshots, "Read snapshot header with bailout kind %u",
            bailoutKind_);

#ifdef TRACK_SNAPSHOTS
    readTrackSnapshot();
#endif
}

#ifdef TRACK_SNAPSHOTS
void
SnapshotReader::readTrackSnapshot()
{
    pcOpcode_  = reader_.readUnsigned();
    mirOpcode_ = reader_.readUnsigned();
    mirId_     = reader_.readUnsigned();
    lirOpcode_ = reader_.readUnsigned();
    lirId_     = reader_.readUnsigned();
}

void
SnapshotReader::spewBailingFrom() const
{
    if (JitSpewEnabled(JitSpew_IonBailouts)) {
        JitSpewHeader(JitSpew_IonBailouts);
        Fprinter& out = JitSpewPrinter();
        out.printf(" bailing from bytecode: %s, MIR: ", CodeName[pcOpcode_]);
        MDefinition::PrintOpcodeName(out, MDefinition::Opcode(mirOpcode_));
        out.printf(" [%u], LIR: ", mirId_);
        LInstruction::printName(out, LInstruction::Opcode(lirOpcode_));
        out.printf(" [%u]", lirId_);
        out.printf("\n");
    }
}
#endif

uint32_t
SnapshotReader::readAllocationIndex()
{
    allocRead_++;
    return reader_.readUnsigned();
}

RValueAllocation
SnapshotReader::readAllocation()
{
    JitSpew(JitSpew_IonSnapshots, "Reading slot %u", allocRead_);
    uint32_t offset = readAllocationIndex() * ALLOCATION_TABLE_ALIGNMENT;
    allocReader_.seek(allocTable_, offset);
    return RValueAllocation::read(allocReader_);
}

bool
SnapshotWriter::init()
{
    // Based on the measurements made in Bug 962555 comment 20, this should be
    // enough to prevent the reallocation of the hash table for at least half of
    // the compilations.
    return allocMap_.init(32);
}

RecoverReader::RecoverReader(SnapshotReader& snapshot, const uint8_t* recovers, uint32_t size)
  : reader_(nullptr, nullptr),
    numInstructions_(0),
    numInstructionsRead_(0)
{
    if (!recovers)
        return;
    reader_ = CompactBufferReader(recovers + snapshot.recoverOffset(), recovers + size);
    readRecoverHeader();
    readInstruction();
}

void
RecoverReader::readRecoverHeader()
{
    uint32_t bits = reader_.readUnsigned();

    numInstructions_ = (bits & RECOVER_RINSCOUNT_MASK) >> RECOVER_RINSCOUNT_SHIFT;
    resumeAfter_ = (bits & RECOVER_RESUMEAFTER_MASK) >> RECOVER_RESUMEAFTER_SHIFT;
    MOZ_ASSERT(numInstructions_);

    JitSpew(JitSpew_IonSnapshots, "Read recover header with instructionCount %u (ra: %d)",
            numInstructions_, resumeAfter_);
}

void
RecoverReader::readInstruction()
{
    MOZ_ASSERT(moreInstructions());
    RInstruction::readRecoverData(reader_, &rawData_);
    numInstructionsRead_++;
}

SnapshotOffset
SnapshotWriter::startSnapshot(RecoverOffset recoverOffset, BailoutKind kind)
{
    lastStart_ = writer_.length();
    allocWritten_ = 0;

    JitSpew(JitSpew_IonSnapshots, "starting snapshot with recover offset %u, bailout kind %u",
            recoverOffset, kind);

    MOZ_ASSERT(uint32_t(kind) < (1 << SNAPSHOT_BAILOUTKIND_BITS));
    MOZ_ASSERT(recoverOffset < (1 << SNAPSHOT_ROFFSET_BITS));
    uint32_t bits =
        (uint32_t(kind) << SNAPSHOT_BAILOUTKIND_SHIFT) |
        (recoverOffset << SNAPSHOT_ROFFSET_SHIFT);

    writer_.writeUnsigned(bits);
    return lastStart_;
}

#ifdef TRACK_SNAPSHOTS
void
SnapshotWriter::trackSnapshot(uint32_t pcOpcode, uint32_t mirOpcode, uint32_t mirId,
                              uint32_t lirOpcode, uint32_t lirId)
{
    writer_.writeUnsigned(pcOpcode);
    writer_.writeUnsigned(mirOpcode);
    writer_.writeUnsigned(mirId);
    writer_.writeUnsigned(lirOpcode);
    writer_.writeUnsigned(lirId);
}
#endif

bool
SnapshotWriter::add(const RValueAllocation& alloc)
{
    MOZ_ASSERT(allocMap_.initialized());

    uint32_t offset;
    RValueAllocMap::AddPtr p = allocMap_.lookupForAdd(alloc);
    if (!p) {
        offset = allocWriter_.length();
        alloc.write(allocWriter_);
        if (!allocMap_.add(p, alloc, offset)) {
            allocWriter_.setOOM();
            return false;
        }
    } else {
        offset = p->value();
    }

    if (JitSpewEnabled(JitSpew_IonSnapshots)) {
        JitSpewHeader(JitSpew_IonSnapshots);
        Fprinter& out = JitSpewPrinter();
        out.printf("    slot %u (%d): ", allocWritten_, offset);
        alloc.dump(out);
        out.printf("\n");
    }

    allocWritten_++;
    writer_.writeUnsigned(offset / ALLOCATION_TABLE_ALIGNMENT);
    return true;
}

void
SnapshotWriter::endSnapshot()
{
    // Place a sentinel for asserting on the other end.
#ifdef DEBUG
    writer_.writeSigned(-1);
#endif

    JitSpew(JitSpew_IonSnapshots, "ending snapshot total size: %u bytes (start %u)",
            uint32_t(writer_.length() - lastStart_), lastStart_);
}

RecoverOffset
RecoverWriter::startRecover(uint32_t instructionCount, bool resumeAfter)
{
    MOZ_ASSERT(instructionCount);
    instructionCount_ = instructionCount;
    instructionsWritten_ = 0;

    JitSpew(JitSpew_IonSnapshots, "starting recover with %u instruction(s)",
            instructionCount);

    MOZ_ASSERT(!(uint32_t(resumeAfter) &~ RECOVER_RESUMEAFTER_MASK));
    MOZ_ASSERT(instructionCount < uint32_t(1 << RECOVER_RINSCOUNT_BITS));
    uint32_t bits =
        (uint32_t(resumeAfter) << RECOVER_RESUMEAFTER_SHIFT) |
        (instructionCount << RECOVER_RINSCOUNT_SHIFT);

    RecoverOffset recoverOffset = writer_.length();
    writer_.writeUnsigned(bits);
    return recoverOffset;
}

void
RecoverWriter::writeInstruction(const MNode* rp)
{
    if (!rp->writeRecoverData(writer_))
        writer_.setOOM();
    instructionsWritten_++;
}

void
RecoverWriter::endRecover()
{
    MOZ_ASSERT(instructionCount_ == instructionsWritten_);
}

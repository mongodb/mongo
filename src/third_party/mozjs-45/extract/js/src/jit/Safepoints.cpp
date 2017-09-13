/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/Safepoints.h"

#include "mozilla/MathAlgorithms.h"

#include "jit/BitSet.h"
#include "jit/JitSpewer.h"
#include "jit/LIR.h"

using namespace js;
using namespace jit;

using mozilla::FloorLog2;

SafepointWriter::SafepointWriter(uint32_t slotCount, uint32_t argumentCount)
  : frameSlots_((slotCount / sizeof(intptr_t)) + 1), // Stack slot counts are inclusive.
    argumentSlots_(argumentCount / sizeof(intptr_t))
{ }

bool
SafepointWriter::init(TempAllocator& alloc)
{
    return frameSlots_.init(alloc) && argumentSlots_.init(alloc);
}

uint32_t
SafepointWriter::startEntry()
{
    JitSpew(JitSpew_Safepoints, "Encoding safepoint (position %d):", stream_.length());
    return uint32_t(stream_.length());
}

void
SafepointWriter::writeOsiCallPointOffset(uint32_t osiCallPointOffset)
{
    stream_.writeUnsigned(osiCallPointOffset);
}

static void
WriteRegisterMask(CompactBufferWriter& stream, uint32_t bits)
{
    if (sizeof(PackedRegisterMask) == 1)
        stream.writeByte(bits);
    else
        stream.writeUnsigned(bits);
}

static int32_t
ReadRegisterMask(CompactBufferReader& stream)
{
    if (sizeof(PackedRegisterMask) == 1)
        return stream.readByte();
    return stream.readUnsigned();
}

static void
WriteFloatRegisterMask(CompactBufferWriter& stream, uint64_t bits)
{
    if (sizeof(FloatRegisters::SetType) == 1) {
        stream.writeByte(bits);
    } else if (sizeof(FloatRegisters::SetType) == 4) {
        stream.writeUnsigned(bits);
    } else {
        MOZ_ASSERT(sizeof(FloatRegisters::SetType) == 8);
        stream.writeUnsigned(bits & 0xffffffff);
        stream.writeUnsigned(bits >> 32);
    }
}

static int64_t
ReadFloatRegisterMask(CompactBufferReader& stream)
{
    if (sizeof(FloatRegisters::SetType) == 1)
        return stream.readByte();
    if (sizeof(FloatRegisters::SetType) <= 4)
        return stream.readUnsigned();
    MOZ_ASSERT(sizeof(FloatRegisters::SetType) == 8);
    uint64_t ret = stream.readUnsigned();
    ret |= uint64_t(stream.readUnsigned()) << 32;
    return ret;
}

void
SafepointWriter::writeGcRegs(LSafepoint* safepoint)
{
    LiveGeneralRegisterSet gc(safepoint->gcRegs());
    LiveGeneralRegisterSet spilledGpr(safepoint->liveRegs().gprs());
    LiveFloatRegisterSet spilledFloat(safepoint->liveRegs().fpus());
    LiveGeneralRegisterSet slots(safepoint->slotsOrElementsRegs());
    LiveGeneralRegisterSet valueRegs;

    WriteRegisterMask(stream_, spilledGpr.bits());
    if (!spilledGpr.empty()) {
        WriteRegisterMask(stream_, gc.bits());
        WriteRegisterMask(stream_, slots.bits());

#ifdef JS_PUNBOX64
        valueRegs = safepoint->valueRegs();
        WriteRegisterMask(stream_, valueRegs.bits());
#endif
    }

    // GC registers are a subset of the spilled registers.
    MOZ_ASSERT((valueRegs.bits() & ~spilledGpr.bits()) == 0);
    MOZ_ASSERT((gc.bits() & ~spilledGpr.bits()) == 0);

    WriteFloatRegisterMask(stream_, spilledFloat.bits());

#ifdef JS_JITSPEW
    if (JitSpewEnabled(JitSpew_Safepoints)) {
        for (GeneralRegisterForwardIterator iter(spilledGpr); iter.more(); iter++) {
            const char* type = gc.has(*iter)
                               ? "gc"
                               : slots.has(*iter)
                                 ? "slots"
                                 : valueRegs.has(*iter)
                                   ? "value"
                                   : "any";
            JitSpew(JitSpew_Safepoints, "    %s reg: %s", type, (*iter).name());
        }
        for (FloatRegisterForwardIterator iter(spilledFloat); iter.more(); iter++)
            JitSpew(JitSpew_Safepoints, "    float reg: %s", (*iter).name());
    }
#endif
}

static void
WriteBitset(const BitSet& set, CompactBufferWriter& stream)
{
    size_t count = set.rawLength();
    const uint32_t* words = set.raw();
    for (size_t i = 0; i < count; i++)
        stream.writeUnsigned(words[i]);
}

static void
MapSlotsToBitset(BitSet& stackSet, BitSet& argumentSet,
                 CompactBufferWriter& stream, const LSafepoint::SlotList& slots)
{
    stackSet.clear();
    argumentSet.clear();

    for (uint32_t i = 0; i < slots.length(); i++) {
        // Slots are represented at a distance from |fp|. We divide by the
        // pointer size, since we only care about pointer-sized/aligned slots
        // here.
        MOZ_ASSERT(slots[i].slot % sizeof(intptr_t) == 0);
        size_t index = slots[i].slot / sizeof(intptr_t);
        (slots[i].stack ? stackSet : argumentSet).insert(index);
    }

    WriteBitset(stackSet, stream);
    WriteBitset(argumentSet, stream);
}

void
SafepointWriter::writeGcSlots(LSafepoint* safepoint)
{
    LSafepoint::SlotList& slots = safepoint->gcSlots();

#ifdef JS_JITSPEW
    for (uint32_t i = 0; i < slots.length(); i++)
        JitSpew(JitSpew_Safepoints, "    gc slot: %d", slots[i]);
#endif

    MapSlotsToBitset(frameSlots_, argumentSlots_, stream_, slots);
}

void
SafepointWriter::writeSlotsOrElementsSlots(LSafepoint* safepoint)
{
    LSafepoint::SlotList& slots = safepoint->slotsOrElementsSlots();

    stream_.writeUnsigned(slots.length());

    for (uint32_t i = 0; i < slots.length(); i++) {
        if (!slots[i].stack)
            MOZ_CRASH();
#ifdef JS_JITSPEW
        JitSpew(JitSpew_Safepoints, "    slots/elements slot: %d", slots[i].slot);
#endif
        stream_.writeUnsigned(slots[i].slot);
    }
}

void
SafepointWriter::writeValueSlots(LSafepoint* safepoint)
{
    LSafepoint::SlotList& slots = safepoint->valueSlots();

#ifdef JS_JITSPEW
    for (uint32_t i = 0; i < slots.length(); i++)
        JitSpew(JitSpew_Safepoints, "    gc value: %d", slots[i]);
#endif

    MapSlotsToBitset(frameSlots_, argumentSlots_, stream_, slots);
}

#if defined(JS_JITSPEW) && defined(JS_NUNBOX32)
static void
DumpNunboxPart(const LAllocation& a)
{
    Fprinter& out = JitSpewPrinter();
    if (a.isStackSlot()) {
        out.printf("stack %d", a.toStackSlot()->slot());
    } else if (a.isArgument()) {
        out.printf("arg %d", a.toArgument()->index());
    } else {
        out.printf("reg %s", a.toGeneralReg()->reg().name());
    }
}
#endif // DEBUG

// Nunbox part encoding:
//
// Reg = 000
// Stack = 001
// Arg = 010
//
// [vwu] nentries:
//    uint16_t:  tttp ppXX XXXY YYYY
//
//     If ttt = Reg, type is reg XXXXX
//     If ppp = Reg, payload is reg YYYYY
//
//     If ttt != Reg, type is:
//          XXXXX if not 11111, otherwise followed by [vwu]
//     If ppp != Reg, payload is:
//          YYYYY if not 11111, otherwise followed by [vwu]
//
enum NunboxPartKind {
    Part_Reg,
    Part_Stack,
    Part_Arg
};

static const uint32_t PART_KIND_BITS = 3;
static const uint32_t PART_KIND_MASK = (1 << PART_KIND_BITS) - 1;
static const uint32_t PART_INFO_BITS = 5;
static const uint32_t PART_INFO_MASK = (1 << PART_INFO_BITS) - 1;

static const uint32_t MAX_INFO_VALUE = (1 << PART_INFO_BITS) - 1;
static const uint32_t TYPE_KIND_SHIFT = 16 - PART_KIND_BITS;
static const uint32_t PAYLOAD_KIND_SHIFT = TYPE_KIND_SHIFT - PART_KIND_BITS;
static const uint32_t TYPE_INFO_SHIFT = PAYLOAD_KIND_SHIFT - PART_INFO_BITS;
static const uint32_t PAYLOAD_INFO_SHIFT = TYPE_INFO_SHIFT - PART_INFO_BITS;

JS_STATIC_ASSERT(PAYLOAD_INFO_SHIFT == 0);

#ifdef JS_NUNBOX32
static inline NunboxPartKind
AllocationToPartKind(const LAllocation& a)
{
    if (a.isRegister())
        return Part_Reg;
    if (a.isStackSlot())
        return Part_Stack;
    MOZ_ASSERT(a.isArgument());
    return Part_Arg;
}

// gcc 4.5 doesn't actually inline CanEncodeInfoInHeader when only
// using the "inline" keyword, and miscompiles the function as well
// when doing block reordering with branch prediction information.
// See bug 799295 comment 71.
static MOZ_ALWAYS_INLINE bool
CanEncodeInfoInHeader(const LAllocation& a, uint32_t* out)
{
    if (a.isGeneralReg()) {
        *out = a.toGeneralReg()->reg().code();
        return true;
    }

    if (a.isStackSlot())
        *out = a.toStackSlot()->slot();
    else
        *out = a.toArgument()->index();

    return *out < MAX_INFO_VALUE;
}

void
SafepointWriter::writeNunboxParts(LSafepoint* safepoint)
{
    LSafepoint::NunboxList& entries = safepoint->nunboxParts();

# ifdef JS_JITSPEW
    if (JitSpewEnabled(JitSpew_Safepoints)) {
        for (uint32_t i = 0; i < entries.length(); i++) {
            SafepointNunboxEntry& entry = entries[i];
            if (entry.type.isUse() || entry.payload.isUse())
                continue;
            JitSpewHeader(JitSpew_Safepoints);
            Fprinter& out = JitSpewPrinter();
            out.printf("    nunbox (type in ");
            DumpNunboxPart(entry.type);
            out.printf(", payload in ");
            DumpNunboxPart(entry.payload);
            out.printf(")\n");
        }
    }
# endif

    // Safepoints are permitted to have partially filled in entries for nunboxes,
    // provided that only the type is live and not the payload. Omit these from
    // the written safepoint.

    size_t pos = stream_.length();
    stream_.writeUnsigned(entries.length());

    size_t count = 0;
    for (size_t i = 0; i < entries.length(); i++) {
        SafepointNunboxEntry& entry = entries[i];

        if (entry.payload.isUse()) {
            // No allocation associated with the payload.
            continue;
        }

        if (entry.type.isUse()) {
            // No allocation associated with the type. Look for another
            // safepoint entry with an allocation for the type.
            entry.type = safepoint->findTypeAllocation(entry.typeVreg);
            if (entry.type.isUse())
                continue;
        }

        count++;

        uint16_t header = 0;

        header |= (AllocationToPartKind(entry.type) << TYPE_KIND_SHIFT);
        header |= (AllocationToPartKind(entry.payload) << PAYLOAD_KIND_SHIFT);

        uint32_t typeVal;
        bool typeExtra = !CanEncodeInfoInHeader(entry.type, &typeVal);
        if (!typeExtra)
            header |= (typeVal << TYPE_INFO_SHIFT);
        else
            header |= (MAX_INFO_VALUE << TYPE_INFO_SHIFT);

        uint32_t payloadVal;
        bool payloadExtra = !CanEncodeInfoInHeader(entry.payload, &payloadVal);
        if (!payloadExtra)
            header |= (payloadVal << PAYLOAD_INFO_SHIFT);
        else
            header |= (MAX_INFO_VALUE << PAYLOAD_INFO_SHIFT);

        stream_.writeFixedUint16_t(header);
        if (typeExtra)
            stream_.writeUnsigned(typeVal);
        if (payloadExtra)
            stream_.writeUnsigned(payloadVal);
    }

    // Update the stream with the actual number of safepoint entries written.
    stream_.writeUnsignedAt(pos, count, entries.length());
}
#endif

void
SafepointWriter::encode(LSafepoint* safepoint)
{
    uint32_t safepointOffset = startEntry();

    MOZ_ASSERT(safepoint->osiCallPointOffset());

    writeOsiCallPointOffset(safepoint->osiCallPointOffset());
    writeGcRegs(safepoint);
    writeGcSlots(safepoint);
    writeValueSlots(safepoint);

#ifdef JS_NUNBOX32
    writeNunboxParts(safepoint);
#endif

    writeSlotsOrElementsSlots(safepoint);

    endEntry();
    safepoint->setOffset(safepointOffset);
}

void
SafepointWriter::endEntry()
{
    JitSpew(JitSpew_Safepoints, "    -- entry ended at %d", uint32_t(stream_.length()));
}

SafepointReader::SafepointReader(IonScript* script, const SafepointIndex* si)
  : stream_(script->safepoints() + si->safepointOffset(),
            script->safepoints() + script->safepointsSize()),
    frameSlots_((script->frameSlots() / sizeof(intptr_t)) + 1), // Stack slot counts are inclusive.
    argumentSlots_(script->argumentSlots() / sizeof(intptr_t))
{
    osiCallPointOffset_ = stream_.readUnsigned();

    // gcSpills is a subset of allGprSpills.
    allGprSpills_ = GeneralRegisterSet(ReadRegisterMask(stream_));
    if (allGprSpills_.empty()) {
        gcSpills_ = allGprSpills_;
        valueSpills_ = allGprSpills_;
        slotsOrElementsSpills_ = allGprSpills_;
    } else {
        gcSpills_ = GeneralRegisterSet(ReadRegisterMask(stream_));
        slotsOrElementsSpills_ = GeneralRegisterSet(ReadRegisterMask(stream_));
#ifdef JS_PUNBOX64
        valueSpills_ = GeneralRegisterSet(ReadRegisterMask(stream_));
#endif
    }
    allFloatSpills_ = FloatRegisterSet(ReadFloatRegisterMask(stream_));

    advanceFromGcRegs();
}

uint32_t
SafepointReader::osiReturnPointOffset() const
{
    return osiCallPointOffset_ + Assembler::PatchWrite_NearCallSize();
}

CodeLocationLabel
SafepointReader::InvalidationPatchPoint(IonScript* script, const SafepointIndex* si)
{
    SafepointReader reader(script, si);

    return CodeLocationLabel(script->method(), CodeOffset(reader.osiCallPointOffset()));
}

void
SafepointReader::advanceFromGcRegs()
{
    currentSlotChunk_ = 0;
    nextSlotChunkNumber_ = 0;
    currentSlotsAreStack_ = true;
}

bool
SafepointReader::getSlotFromBitmap(SafepointSlotEntry* entry)
{
    while (currentSlotChunk_ == 0) {
        // Are there any more chunks to read?
        if (currentSlotsAreStack_) {
            if (nextSlotChunkNumber_ == BitSet::RawLengthForBits(frameSlots_)) {
                nextSlotChunkNumber_ = 0;
                currentSlotsAreStack_ = false;
                continue;
            }
        } else if (nextSlotChunkNumber_ == BitSet::RawLengthForBits(argumentSlots_)) {
            return false;
        }

        // Yes, read the next chunk.
        currentSlotChunk_ = stream_.readUnsigned();
        nextSlotChunkNumber_++;
    }

    // The current chunk still has bits in it, so get the next bit, then mask
    // it out of the slot chunk.
    uint32_t bit = FloorLog2(currentSlotChunk_);
    currentSlotChunk_ &= ~(1 << bit);

    // Return the slot, and re-scale it by the pointer size, reversing the
    // transformation in MapSlotsToBitset.
    entry->stack = currentSlotsAreStack_;
    entry->slot = (((nextSlotChunkNumber_ - 1) * BitSet::BitsPerWord) + bit) * sizeof(intptr_t);
    return true;
}

bool
SafepointReader::getGcSlot(SafepointSlotEntry* entry)
{
    if (getSlotFromBitmap(entry))
        return true;
    advanceFromGcSlots();
    return false;
}

void
SafepointReader::advanceFromGcSlots()
{
    // No, reset the counter.
    currentSlotChunk_ = 0;
    nextSlotChunkNumber_ = 0;
    currentSlotsAreStack_ = true;
}

bool
SafepointReader::getValueSlot(SafepointSlotEntry* entry)
{
    if (getSlotFromBitmap(entry))
        return true;
    advanceFromValueSlots();
    return false;
}

void
SafepointReader::advanceFromValueSlots()
{
#ifdef JS_NUNBOX32
    nunboxSlotsRemaining_ = stream_.readUnsigned();
#else
    nunboxSlotsRemaining_ = 0;
    advanceFromNunboxSlots();
#endif
}

static inline LAllocation
PartFromStream(CompactBufferReader& stream, NunboxPartKind kind, uint32_t info)
{
    if (kind == Part_Reg)
        return LGeneralReg(Register::FromCode(info));

    if (info == MAX_INFO_VALUE)
        info = stream.readUnsigned();

    if (kind == Part_Stack)
        return LStackSlot(info);

    MOZ_ASSERT(kind == Part_Arg);
    return LArgument(info);
}

bool
SafepointReader::getNunboxSlot(LAllocation* type, LAllocation* payload)
{
    if (!nunboxSlotsRemaining_--) {
        advanceFromNunboxSlots();
        return false;
    }

    uint16_t header = stream_.readFixedUint16_t();
    NunboxPartKind typeKind = (NunboxPartKind)((header >> TYPE_KIND_SHIFT) & PART_KIND_MASK);
    NunboxPartKind payloadKind = (NunboxPartKind)((header >> PAYLOAD_KIND_SHIFT) & PART_KIND_MASK);
    uint32_t typeInfo = (header >> TYPE_INFO_SHIFT) & PART_INFO_MASK;
    uint32_t payloadInfo = (header >> PAYLOAD_INFO_SHIFT) & PART_INFO_MASK;

    *type = PartFromStream(stream_, typeKind, typeInfo);
    *payload = PartFromStream(stream_, payloadKind, payloadInfo);
    return true;
}

void
SafepointReader::advanceFromNunboxSlots()
{
    slotsOrElementsSlotsRemaining_ = stream_.readUnsigned();
}

bool
SafepointReader::getSlotsOrElementsSlot(SafepointSlotEntry* entry)
{
    if (!slotsOrElementsSlotsRemaining_--)
        return false;
    entry->stack = true;
    entry->slot = stream_.readUnsigned();
    return true;
}

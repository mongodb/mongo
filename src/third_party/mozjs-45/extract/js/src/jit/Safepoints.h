/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_Safepoints_h
#define jit_Safepoints_h

#include "jit/BitSet.h"
#include "jit/CompactBuffer.h"
#include "jit/shared/Assembler-shared.h"

namespace js {
namespace jit {

struct SafepointSlotEntry;

class LAllocation;
class LSafepoint;

static const uint32_t INVALID_SAFEPOINT_OFFSET = uint32_t(-1);

class SafepointWriter
{
    CompactBufferWriter stream_;
    BitSet frameSlots_;
    BitSet argumentSlots_;

  public:
    explicit SafepointWriter(uint32_t slotCount, uint32_t argumentCount);
    bool init(TempAllocator& alloc);

  private:
    // A safepoint entry is written in the order these functions appear.
    uint32_t startEntry();

    void writeOsiCallPointOffset(uint32_t osiPointOffset);
    void writeGcRegs(LSafepoint* safepoint);
    void writeGcSlots(LSafepoint* safepoint);
    void writeValueSlots(LSafepoint* safepoint);

    void writeSlotsOrElementsSlots(LSafepoint* safepoint);

#ifdef JS_NUNBOX32
    void writeNunboxParts(LSafepoint* safepoint);
#endif

    void endEntry();

  public:
    void encode(LSafepoint* safepoint);

    size_t size() const {
        return stream_.length();
    }
    const uint8_t* buffer() const {
        return stream_.buffer();
    }
    bool oom() const {
        return stream_.oom();
    }
};

class SafepointReader
{
    CompactBufferReader stream_;
    uint32_t frameSlots_;
    uint32_t argumentSlots_;
    uint32_t currentSlotChunk_;
    bool currentSlotsAreStack_;
    uint32_t nextSlotChunkNumber_;
    uint32_t osiCallPointOffset_;
    GeneralRegisterSet gcSpills_;
    GeneralRegisterSet valueSpills_;
    GeneralRegisterSet slotsOrElementsSpills_;
    GeneralRegisterSet allGprSpills_;
    FloatRegisterSet allFloatSpills_;
    uint32_t nunboxSlotsRemaining_;
    uint32_t slotsOrElementsSlotsRemaining_;

  private:
    void advanceFromGcRegs();
    void advanceFromGcSlots();
    void advanceFromValueSlots();
    void advanceFromNunboxSlots();
    bool getSlotFromBitmap(SafepointSlotEntry* entry);

  public:
    SafepointReader(IonScript* script, const SafepointIndex* si);

    static CodeLocationLabel InvalidationPatchPoint(IonScript* script, const SafepointIndex* si);

    uint32_t osiCallPointOffset() const {
        return osiCallPointOffset_;
    }
    LiveGeneralRegisterSet gcSpills() const {
        return LiveGeneralRegisterSet(gcSpills_);
    }
    LiveGeneralRegisterSet slotsOrElementsSpills() const {
        return LiveGeneralRegisterSet(slotsOrElementsSpills_);
    }
    LiveGeneralRegisterSet valueSpills() const {
        return LiveGeneralRegisterSet(valueSpills_);
    }
    LiveGeneralRegisterSet allGprSpills() const {
        return LiveGeneralRegisterSet(allGprSpills_);
    }
    LiveFloatRegisterSet allFloatSpills() const {
        return LiveFloatRegisterSet(allFloatSpills_);
    }
    uint32_t osiReturnPointOffset() const;

    // Returns true if a slot was read, false if there are no more slots.
    bool getGcSlot(SafepointSlotEntry* entry);

    // Returns true if a slot was read, false if there are no more value slots.
    bool getValueSlot(SafepointSlotEntry* entry);

    // Returns true if a nunbox slot was read, false if there are no more
    // nunbox slots.
    bool getNunboxSlot(LAllocation* type, LAllocation* payload);

    // Returns true if a slot was read, false if there are no more slots.
    bool getSlotsOrElementsSlot(SafepointSlotEntry* entry);
};

} // namespace jit
} // namespace js

#endif /* jit_Safepoints_h */

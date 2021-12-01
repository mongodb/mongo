/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_StackSlotAllocator_h
#define jit_StackSlotAllocator_h

#include "mozilla/Unused.h"

#include "jit/Registers.h"

namespace js {
namespace jit {

class StackSlotAllocator
{
    js::Vector<uint32_t, 4, SystemAllocPolicy> normalSlots;
    js::Vector<uint32_t, 4, SystemAllocPolicy> doubleSlots;
    uint32_t height_;

    void addAvailableSlot(uint32_t index) {
        // Ignoring OOM here (and below) is fine; it just means the stack slot
        // will be unused.
        mozilla::Unused << normalSlots.append(index);
    }
    void addAvailableDoubleSlot(uint32_t index) {
        mozilla::Unused << doubleSlots.append(index);
    }

    uint32_t allocateQuadSlot() {
        MOZ_ASSERT(SupportsSimd);
        // This relies on the fact that any architecture specific
        // alignment of the stack pointer is done a priori.
        if (height_ % 8 != 0)
            addAvailableSlot(height_ += 4);
        if (height_ % 16 != 0)
            addAvailableDoubleSlot(height_ += 8);
        return height_ += 16;
    }
    uint32_t allocateDoubleSlot() {
        if (!doubleSlots.empty())
            return doubleSlots.popCopy();
        if (height_ % 8 != 0)
            addAvailableSlot(height_ += 4);
        return height_ += 8;
    }
    uint32_t allocateSlot() {
        if (!normalSlots.empty())
            return normalSlots.popCopy();
        if (!doubleSlots.empty()) {
            uint32_t index = doubleSlots.popCopy();
            addAvailableSlot(index - 4);
            return index;
        }
        return height_ += 4;
    }

  public:
    StackSlotAllocator() : height_(0)
    { }

    static uint32_t width(LDefinition::Type type) {
        switch (type) {
#if JS_BITS_PER_WORD == 32
          case LDefinition::GENERAL:
          case LDefinition::OBJECT:
          case LDefinition::SLOTS:
#endif
          case LDefinition::INT32:
          case LDefinition::FLOAT32:      return 4;
#if JS_BITS_PER_WORD == 64
          case LDefinition::GENERAL:
          case LDefinition::OBJECT:
          case LDefinition::SLOTS:
#endif
#ifdef JS_PUNBOX64
          case LDefinition::BOX:
#endif
#ifdef JS_NUNBOX32
          case LDefinition::TYPE:
          case LDefinition::PAYLOAD:
#endif
          case LDefinition::DOUBLE:       return 8;
          case LDefinition::SINCOS:
          case LDefinition::SIMD128INT:
          case LDefinition::SIMD128FLOAT: return 16;
        }
        MOZ_CRASH("Unknown slot type");
    }

    uint32_t allocateSlot(LDefinition::Type type) {
        switch (width(type)) {
          case 4:  return allocateSlot();
          case 8:  return allocateDoubleSlot();
          case 16: return allocateQuadSlot();
        }
        MOZ_CRASH("Unknown slot width");
    }

    uint32_t stackHeight() const {
        return height_;
    }
};

} // namespace jit
} // namespace js

#endif /* jit_StackSlotAllocator_h */

/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_StackSlotAllocator_h
#define jit_StackSlotAllocator_h

#include "jit/LIR.h"
#include "jit/Registers.h"

namespace js {
namespace jit {

class StackSlotAllocator {
  js::Vector<uint32_t, 4, SystemAllocPolicy> normalSlots;
  js::Vector<uint32_t, 4, SystemAllocPolicy> doubleSlots;
  js::Vector<uint32_t, 4, SystemAllocPolicy> quadSlots;
  uint32_t height_;

  void addAvailableSlot(uint32_t index) {
    // Ignoring OOM here (and below) is fine; it just means the stack slot
    // will be unused.
    (void)normalSlots.append(index);
  }
  void addAvailableDoubleSlot(uint32_t index) {
    (void)doubleSlots.append(index);
  }
  void addAvailableQuadSlot(uint32_t index) { (void)quadSlots.append(index); }

  uint32_t allocateQuadSlot() {
    // This relies on the fact that any architecture specific
    // alignment of the stack pointer is done a priori.
    if (!quadSlots.empty()) {
      return quadSlots.popCopy();
    }
    if (height_ % 8 != 0) {
      addAvailableSlot(height_ += 4);
    }
    if (height_ % 16 != 0) {
      addAvailableDoubleSlot(height_ += 8);
    }
    return height_ += 16;
  }
  uint32_t allocateDoubleSlot() {
    if (!doubleSlots.empty()) {
      return doubleSlots.popCopy();
    }
    if (height_ % 8 != 0) {
      addAvailableSlot(height_ += 4);
    }
    return height_ += 8;
  }
  uint32_t allocateSlot() {
    if (!normalSlots.empty()) {
      return normalSlots.popCopy();
    }
    if (!doubleSlots.empty()) {
      uint32_t index = doubleSlots.popCopy();
      addAvailableSlot(index - 4);
      return index;
    }
    return height_ += 4;
  }

 public:
  StackSlotAllocator() : height_(0) {}

  void allocateStackArea(LStackArea* alloc) {
    uint32_t size = alloc->size();

    MOZ_ASSERT(size % 4 == 0);
    switch (alloc->alignment()) {
      case 8:
        if ((height_ + size) % 8 != 0) {
          addAvailableSlot(height_ += 4);
        }
        break;
      default:
        MOZ_CRASH("unexpected stack results area alignment");
    }
    MOZ_ASSERT((height_ + size) % alloc->alignment() == 0);

    height_ += size;
    alloc->setBase(height_);
  }

  uint32_t allocateSlot(LStackSlot::Width width) {
    switch (width) {
      case LStackSlot::Word:
        return allocateSlot();
      case LStackSlot::DoubleWord:
        return allocateDoubleSlot();
      case LStackSlot::QuadWord:
        return allocateQuadSlot();
    }
    MOZ_CRASH("Unknown slot width");
  }

  // This method is used by the Simple allocator to free stack slots so that
  // they can be reused. The Backtracking allocator doesn't call this.
  void freeSlot(LStackSlot::Width width, uint32_t slot) {
    switch (width) {
      case LStackSlot::Word:
        addAvailableSlot(slot);
        return;
      case LStackSlot::DoubleWord:
        addAvailableDoubleSlot(slot);
        return;
      case LStackSlot::QuadWord:
        addAvailableQuadSlot(slot);
        return;
    }
    MOZ_CRASH("Unknown slot width");
  }

  uint32_t stackHeight() const { return height_; }
};

}  // namespace jit
}  // namespace js

#endif /* jit_StackSlotAllocator_h */

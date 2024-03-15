/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_shared_IonAssemblerBuffer_h
#define jit_shared_IonAssemblerBuffer_h

#include "mozilla/Assertions.h"
#include "mozilla/MathAlgorithms.h"

#include <algorithm>

#include "jit/ProcessExecutableMemory.h"
#include "jit/shared/Assembler-shared.h"

namespace js {
namespace jit {

// The offset into a buffer, in bytes.
class BufferOffset {
  int offset;

 public:
  friend BufferOffset nextOffset();

  BufferOffset() : offset(INT_MIN) {}

  explicit BufferOffset(int offset_) : offset(offset_) {
    MOZ_ASSERT(offset >= 0);
  }

  explicit BufferOffset(Label* l) : offset(l->offset()) {
    MOZ_ASSERT(offset >= 0);
  }

  int getOffset() const { return offset; }
  bool assigned() const { return offset != INT_MIN; }

  // A BOffImm is a Branch Offset Immediate. It is an architecture-specific
  // structure that holds the immediate for a pc relative branch. diffB takes
  // the label for the destination of the branch, and encodes the immediate
  // for the branch. This will need to be fixed up later, since A pool may be
  // inserted between the branch and its destination.
  template <class BOffImm>
  BOffImm diffB(BufferOffset other) const {
    if (!BOffImm::IsInRange(offset - other.offset)) {
      return BOffImm();
    }
    return BOffImm(offset - other.offset);
  }

  template <class BOffImm>
  BOffImm diffB(Label* other) const {
    MOZ_ASSERT(other->bound());
    if (!BOffImm::IsInRange(offset - other->offset())) {
      return BOffImm();
    }
    return BOffImm(offset - other->offset());
  }
};

inline bool operator<(BufferOffset a, BufferOffset b) {
  return a.getOffset() < b.getOffset();
}

inline bool operator>(BufferOffset a, BufferOffset b) {
  return a.getOffset() > b.getOffset();
}

inline bool operator<=(BufferOffset a, BufferOffset b) {
  return a.getOffset() <= b.getOffset();
}

inline bool operator>=(BufferOffset a, BufferOffset b) {
  return a.getOffset() >= b.getOffset();
}

inline bool operator==(BufferOffset a, BufferOffset b) {
  return a.getOffset() == b.getOffset();
}

inline bool operator!=(BufferOffset a, BufferOffset b) {
  return a.getOffset() != b.getOffset();
}

template <int SliceSize>
class BufferSlice {
 protected:
  BufferSlice<SliceSize>* prev_;
  BufferSlice<SliceSize>* next_;

  size_t bytelength_;

 public:
  mozilla::Array<uint8_t, SliceSize> instructions;

 public:
  explicit BufferSlice() : prev_(nullptr), next_(nullptr), bytelength_(0) {}

  size_t length() const { return bytelength_; }
  static inline size_t Capacity() { return SliceSize; }

  BufferSlice* getNext() const { return next_; }
  BufferSlice* getPrev() const { return prev_; }

  void setNext(BufferSlice<SliceSize>* next) {
    MOZ_ASSERT(next_ == nullptr);
    MOZ_ASSERT(next->prev_ == nullptr);
    next_ = next;
    next->prev_ = this;
  }

  void putBytes(size_t numBytes, const void* source) {
    MOZ_ASSERT(bytelength_ + numBytes <= SliceSize);
    if (source) {
      memcpy(&instructions[length()], source, numBytes);
    }
    bytelength_ += numBytes;
  }

  MOZ_ALWAYS_INLINE
  void putU32Aligned(uint32_t value) {
    MOZ_ASSERT(bytelength_ + 4 <= SliceSize);
    MOZ_ASSERT((bytelength_ & 3) == 0);
    MOZ_ASSERT((uintptr_t(&instructions[0]) & 3) == 0);
    *reinterpret_cast<uint32_t*>(&instructions[bytelength_]) = value;
    bytelength_ += 4;
  }
};

template <int SliceSize, class Inst>
class AssemblerBuffer {
 protected:
  typedef BufferSlice<SliceSize> Slice;

  // Doubly-linked list of BufferSlices, with the most recent in tail position.
  Slice* head;
  Slice* tail;

  bool m_oom;

  // How many bytes has been committed to the buffer thus far.
  // Does not include tail.
  uint32_t bufferSize;

  // How many bytes can be in the buffer.  Normally this is
  // MaxCodeBytesPerBuffer, but for pasteup buffers where we handle far jumps
  // explicitly it can be larger.
  uint32_t maxSize;

  // Finger for speeding up accesses.
  Slice* finger;
  int finger_offset;

  LifoAlloc lifoAlloc_;

 public:
  explicit AssemblerBuffer()
      : head(nullptr),
        tail(nullptr),
        m_oom(false),
        bufferSize(0),
        maxSize(MaxCodeBytesPerBuffer),
        finger(nullptr),
        finger_offset(0),
        lifoAlloc_(8192) {}

 public:
  bool isAligned(size_t alignment) const {
    MOZ_ASSERT(mozilla::IsPowerOfTwo(alignment));
    return !(size() & (alignment - 1));
  }

  void setUnlimited() { maxSize = MaxCodeBytesPerProcess; }

 private:
  Slice* newSlice(LifoAlloc& a) {
    if (size() > maxSize - sizeof(Slice)) {
      fail_oom();
      return nullptr;
    }
    Slice* tmp = static_cast<Slice*>(a.alloc(sizeof(Slice)));
    if (!tmp) {
      fail_oom();
      return nullptr;
    }
    return new (tmp) Slice;
  }

 public:
  bool ensureSpace(size_t size) {
    // Space can exist in the most recent Slice.
    if (tail && tail->length() + size <= tail->Capacity()) {
      // Simulate allocation failure even when we don't need a new slice.
      if (js::oom::ShouldFailWithOOM()) {
        return fail_oom();
      }

      return true;
    }

    // Otherwise, a new Slice must be added.
    Slice* slice = newSlice(lifoAlloc_);
    if (slice == nullptr) {
      return fail_oom();
    }

    // If this is the first Slice in the buffer, add to head position.
    if (!head) {
      head = slice;
      finger = slice;
      finger_offset = 0;
    }

    // Finish the last Slice and add the new Slice to the linked list.
    if (tail) {
      bufferSize += tail->length();
      tail->setNext(slice);
    }
    tail = slice;

    return true;
  }

  BufferOffset putByte(uint8_t value) {
    return putBytes(sizeof(value), &value);
  }

  BufferOffset putShort(uint16_t value) {
    return putBytes(sizeof(value), &value);
  }

  BufferOffset putInt(uint32_t value) {
    return putBytes(sizeof(value), &value);
  }

  MOZ_ALWAYS_INLINE
  BufferOffset putU32Aligned(uint32_t value) {
    if (!ensureSpace(sizeof(value))) {
      return BufferOffset();
    }

    BufferOffset ret = nextOffset();
    tail->putU32Aligned(value);
    return ret;
  }

  // Add numBytes bytes to this buffer.
  // The data must fit in a single slice.
  BufferOffset putBytes(size_t numBytes, const void* inst) {
    if (!ensureSpace(numBytes)) {
      return BufferOffset();
    }

    BufferOffset ret = nextOffset();
    tail->putBytes(numBytes, inst);
    return ret;
  }

  // Add a potentially large amount of data to this buffer.
  // The data may be distrubuted across multiple slices.
  // Return the buffer offset of the first added byte.
  BufferOffset putBytesLarge(size_t numBytes, const void* data) {
    BufferOffset ret = nextOffset();
    while (numBytes > 0) {
      if (!ensureSpace(1)) {
        return BufferOffset();
      }
      size_t avail = tail->Capacity() - tail->length();
      size_t xfer = numBytes < avail ? numBytes : avail;
      MOZ_ASSERT(xfer > 0, "ensureSpace should have allocated a slice");
      tail->putBytes(xfer, data);
      data = (const uint8_t*)data + xfer;
      numBytes -= xfer;
    }
    return ret;
  }

  unsigned int size() const {
    if (tail) {
      return bufferSize + tail->length();
    }
    return bufferSize;
  }
  BufferOffset nextOffset() const { return BufferOffset(size()); }

  bool oom() const { return m_oom; }

  bool fail_oom() {
    m_oom = true;
#ifdef DEBUG
    JitContext* context = MaybeGetJitContext();
    if (context) {
      context->setOOM();
    }
#endif
    return false;
  }

 private:
  void update_finger(Slice* finger_, int fingerOffset_) {
    finger = finger_;
    finger_offset = fingerOffset_;
  }

  static const unsigned SliceDistanceRequiringFingerUpdate = 3;

  Inst* getInstForwards(BufferOffset off, Slice* start, int startOffset,
                        bool updateFinger = false) {
    const int offset = off.getOffset();

    int cursor = startOffset;
    unsigned slicesSkipped = 0;

    MOZ_ASSERT(offset >= cursor);

    for (Slice* slice = start; slice != nullptr; slice = slice->getNext()) {
      const int slicelen = slice->length();

      // Is the offset within the bounds of this slice?
      if (offset < cursor + slicelen) {
        if (updateFinger ||
            slicesSkipped >= SliceDistanceRequiringFingerUpdate) {
          update_finger(slice, cursor);
        }

        MOZ_ASSERT(offset - cursor < (int)slice->length());
        return (Inst*)&slice->instructions[offset - cursor];
      }

      cursor += slicelen;
      slicesSkipped++;
    }

    MOZ_CRASH("Invalid instruction cursor.");
  }

  Inst* getInstBackwards(BufferOffset off, Slice* start, int startOffset,
                         bool updateFinger = false) {
    const int offset = off.getOffset();

    int cursor = startOffset;  // First (lowest) offset in the start Slice.
    unsigned slicesSkipped = 0;

    MOZ_ASSERT(offset < int(cursor + start->length()));

    for (Slice* slice = start; slice != nullptr;) {
      // Is the offset within the bounds of this slice?
      if (offset >= cursor) {
        if (updateFinger ||
            slicesSkipped >= SliceDistanceRequiringFingerUpdate) {
          update_finger(slice, cursor);
        }

        MOZ_ASSERT(offset - cursor < (int)slice->length());
        return (Inst*)&slice->instructions[offset - cursor];
      }

      // Move the cursor to the start of the previous slice.
      Slice* prev = slice->getPrev();
      cursor -= prev->length();

      slice = prev;
      slicesSkipped++;
    }

    MOZ_CRASH("Invalid instruction cursor.");
  }

 public:
  Inst* getInstOrNull(BufferOffset off) {
    if (!off.assigned()) {
      return nullptr;
    }
    return getInst(off);
  }

  // Get a pointer to the instruction at offset |off| which must be within the
  // bounds of the buffer. Use |getInstOrNull()| if |off| may be unassigned.
  Inst* getInst(BufferOffset off) {
    const int offset = off.getOffset();
    // This function is hot, do not make the next line a RELEASE_ASSERT.
    MOZ_ASSERT(off.assigned() && offset >= 0 && unsigned(offset) < size());

    // Is the instruction in the last slice?
    if (offset >= int(bufferSize)) {
      return (Inst*)&tail->instructions[offset - bufferSize];
    }

    // How close is this offset to the previous one we looked up?
    // If it is sufficiently far from the start and end of the buffer,
    // use the finger to start midway through the list.
    int finger_dist = abs(offset - finger_offset);
    if (finger_dist < std::min(offset, int(bufferSize - offset))) {
      if (finger_offset < offset) {
        return getInstForwards(off, finger, finger_offset, true);
      }
      return getInstBackwards(off, finger, finger_offset, true);
    }

    // Is the instruction closer to the start or to the end?
    if (offset < int(bufferSize - offset)) {
      return getInstForwards(off, head, 0);
    }

    // The last slice was already checked above, so start at the
    // second-to-last.
    Slice* prev = tail->getPrev();
    return getInstBackwards(off, prev, bufferSize - prev->length());
  }

  typedef AssemblerBuffer<SliceSize, Inst> ThisClass;

  class AssemblerBufferInstIterator {
    BufferOffset bo_;
    ThisClass* buffer_;

   public:
    explicit AssemblerBufferInstIterator(BufferOffset bo, ThisClass* buffer)
        : bo_(bo), buffer_(buffer) {}
    void advance(int offset) { bo_ = BufferOffset(bo_.getOffset() + offset); }
    Inst* next() {
      advance(cur()->size());
      return cur();
    }
    Inst* peek() {
      return buffer_->getInst(BufferOffset(bo_.getOffset() + cur()->size()));
    }
    Inst* cur() const { return buffer_->getInst(bo_); }
  };
};

}  // namespace jit
}  // namespace js

#endif  // jit_shared_IonAssemblerBuffer_h

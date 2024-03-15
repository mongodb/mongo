/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * GC-internal definition of relocation overlay used while moving cells.
 */

#ifndef gc_RelocationOverlay_h
#define gc_RelocationOverlay_h

#include "mozilla/Assertions.h"

#include <stdint.h>

#include "gc/Cell.h"

namespace js {
namespace gc {

/*
 * This structure overlays a Cell that has been moved and provides a way to find
 * its new location. It's used during generational and compacting GC.
 */
class RelocationOverlay : public Cell {
 public:
  /* The location the cell has been moved to, stored in the cell header. */
  Cell* forwardingAddress() const {
    MOZ_ASSERT(isForwarded());
    return reinterpret_cast<Cell*>(header_.getForwardingAddress());
  }

 protected:
  /* A list entry to track all relocated things. */
  RelocationOverlay* next_;

  explicit RelocationOverlay(Cell* dst);

 public:
  static const RelocationOverlay* fromCell(const Cell* cell) {
    return static_cast<const RelocationOverlay*>(cell);
  }

  static RelocationOverlay* fromCell(Cell* cell) {
    return static_cast<RelocationOverlay*>(cell);
  }

  static RelocationOverlay* forwardCell(Cell* src, Cell* dst);

  void setNext(RelocationOverlay* next) {
    MOZ_ASSERT(isForwarded());
    next_ = next;
  }

  RelocationOverlay* next() const {
    MOZ_ASSERT(isForwarded());
    return next_;
  }
};

}  // namespace gc
}  // namespace js

#endif /* gc_RelocationOverlay_h */

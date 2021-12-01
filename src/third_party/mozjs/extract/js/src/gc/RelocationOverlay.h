/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * GC-internal definition of relocation overlay used while moving cells.
 */

#ifndef gc_RelocationOverlay_h
#define gc_RelocationOverlay_h

#include "mozilla/Assertions.h"
#include "mozilla/EndianUtils.h"

#include <stdint.h>

#include "js/HeapAPI.h"
#include "vm/JSObject.h"
#include "vm/Shape.h"

namespace js {
namespace gc {

struct Cell;

/*
 * This structure overlays a Cell that has been moved and provides a way to find
 * its new location. It's used during generational and compacting GC.
 */
class RelocationOverlay
{
    /* See comment in js/public/HeapAPI.h. */
    static const uint32_t Relocated = js::gc::Relocated;


#if MOZ_LITTLE_ENDIAN
    /*
     * Keep the first 32 bits untouched. Use them to distinguish strings from
     * objects in the nursery.
     */
    uint32_t preserve_;

    /* Set to Relocated when moved. */
    uint32_t magic_;
#else
    /*
     * On big-endian, we need to reorder to keep preserve_ lined up with the
     * low 32 bits of the aligned group_ pointer in JSObject.
     */
    uint32_t magic_;
    uint32_t preserve_;
#endif



    /* The location |this| was moved to. */
    Cell* newLocation_;

    /* A list entry to track all relocated things. */
    RelocationOverlay* next_;

  public:
    static const RelocationOverlay* fromCell(const Cell* cell) {
        return reinterpret_cast<const RelocationOverlay*>(cell);
    }

    static RelocationOverlay* fromCell(Cell* cell) {
        return reinterpret_cast<RelocationOverlay*>(cell);
    }

    bool isForwarded() const {
        (void) preserve_; // Suppress warning
        return magic_ == Relocated;
    }

    Cell* forwardingAddress() const {
        MOZ_ASSERT(isForwarded());
        return newLocation_;
    }

    void forwardTo(Cell* cell);

    RelocationOverlay*& nextRef() {
        MOZ_ASSERT(isForwarded());
        return next_;
    }

    RelocationOverlay* next() const {
        MOZ_ASSERT(isForwarded());
        return next_;
    }

    static bool isCellForwarded(const Cell* cell) {
        return fromCell(cell)->isForwarded();
    }
};

} // namespace gc
} // namespace js

#endif /* gc_RelocationOverlay_h */

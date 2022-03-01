/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_SafepointIndex_h
#define jit_SafepointIndex_h

#include "mozilla/Assertions.h"

#include <stdint.h>

#include "jit/IonTypes.h"

namespace js {
namespace jit {

class LSafepoint;
class CodegenSafepointIndex;

// Two-tuple that lets you look up the safepoint entry given the
// displacement of a call instruction within the JIT code.
class SafepointIndex {
  // The displacement is the distance from the first byte of the JIT'd code
  // to the return address (of the call that the safepoint was generated for).
  uint32_t displacement_ = 0;

  // Offset within the safepoint buffer.
  uint32_t safepointOffset_ = 0;

 public:
  inline explicit SafepointIndex(const CodegenSafepointIndex& csi);

  uint32_t displacement() const { return displacement_; }
  uint32_t safepointOffset() const { return safepointOffset_; }
};

class CodegenSafepointIndex {
  uint32_t displacement_ = 0;

  LSafepoint* safepoint_ = nullptr;

 public:
  CodegenSafepointIndex(uint32_t displacement, LSafepoint* safepoint)
      : displacement_(displacement), safepoint_(safepoint) {}

  LSafepoint* safepoint() const { return safepoint_; }
  uint32_t displacement() const { return displacement_; }

  inline SnapshotOffset snapshotOffset() const;
  inline bool hasSnapshotOffset() const;
};

// The OSI point is patched to a call instruction. Therefore, the
// returnPoint for an OSI call is the address immediately following that
// call instruction. The displacement of that point within the assembly
// buffer is the |returnPointDisplacement|.
class OsiIndex {
  uint32_t callPointDisplacement_;
  uint32_t snapshotOffset_;

 public:
  OsiIndex(uint32_t callPointDisplacement, uint32_t snapshotOffset)
      : callPointDisplacement_(callPointDisplacement),
        snapshotOffset_(snapshotOffset) {}

  uint32_t returnPointDisplacement() const;
  uint32_t callPointDisplacement() const { return callPointDisplacement_; }
  uint32_t snapshotOffset() const { return snapshotOffset_; }
};

} /* namespace jit */
} /* namespace js */

#endif /* jit_SafepointIndex_h */

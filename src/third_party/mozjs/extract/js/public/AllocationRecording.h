/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_AllocationRecording_h
#define js_AllocationRecording_h

#include <stdint.h>
#include "jstypes.h"
#include "js/TypeDecls.h"

namespace JS {

/**
 * This struct holds the information needed to create a profiler marker payload
 * that can represent a JS allocation. It translates JS engine specific classes,
 * into something that can be used in the profiler.
 */
struct RecordAllocationInfo {
  RecordAllocationInfo(const char16_t* typeName, const char* className,
                       const char16_t* descriptiveTypeName,
                       const char* coarseType, uint64_t size, bool inNursery)
      : typeName(typeName),
        className(className),
        descriptiveTypeName(descriptiveTypeName),
        coarseType(coarseType),
        size(size),
        inNursery(inNursery) {}

  // These pointers are borrowed from the UbiNode, and can point to live data.
  // It is important for the consumers of this struct to correctly
  // duplicate the strings to take ownership of them.
  const char16_t* typeName;
  const char* className;
  const char16_t* descriptiveTypeName;

  // The coarseType points to a string literal, so does not need to be
  // duplicated.
  const char* coarseType;

  // The size in bytes of the allocation.
  uint64_t size;

  // Whether or not the allocation is in the nursery or not.
  bool inNursery;
};

typedef void (*RecordAllocationsCallback)(RecordAllocationInfo&& info);

/**
 * Enable recording JS allocations. This feature hooks into the object creation
 * in the JavaScript engine, and reports back the allocation info through the
 * callback. This allocation tracking is turned on for all encountered realms.
 * The JS Debugger API can also turn on allocation tracking with its own
 * probability. If both allocation tracking mechanisms are turned on at the same
 * time, the Debugger's probability defers to the EnableRecordingAllocations's
 * probability setting.
 */
JS_PUBLIC_API void EnableRecordingAllocations(
    JSContext* cx, RecordAllocationsCallback callback, double probability);

/**
 * Turn off JS allocation recording. If any JS Debuggers are also recording
 * allocations, then the probability will be reset to the Debugger's desired
 * setting.
 */
JS_PUBLIC_API void DisableRecordingAllocations(JSContext* cx);

}  // namespace JS

#endif /* js_AllocationRecording_h */

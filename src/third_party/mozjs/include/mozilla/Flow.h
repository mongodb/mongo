/*
 * Copyright (C) 2021 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef Flow_h
#define Flow_h

// We use a Flow for connecting markers over time. It's a
// global (cross-process) 64bit id that will connect any markers that have the
// same id until we see a terminating-flow with that id.
//
// With these semanatics we can derive a flow from a pointer by xoring it with
// gProcessUUID and using a terminating-flow when we're done with
// that pointer e.g. destructor. This doesn't ensure that the flow is globally
// unique but makes colluisions unlikely enough that it mostly works.

// The following code for Flow is derived from Perfetto

#include <stdint.h>
#include "mozilla/ProfileBufferEntrySerialization.h"

extern uint64_t MFBT_DATA gProcessUUID;

// This class is used as a marker field type and is used in the marker schema.
// To create a Flow, use Flow::FromPointer, Flow::ProcessScoped or Flow::Global.
class Flow {
 public:
  // |aFlow| which is local within a given process (e.g. atomic counter xor'ed
  // with feature-specific value). This value is xor'ed with processUUID
  // to attempt to ensure that it's globally-unique.
  static inline Flow ProcessScoped(uint64_t aFlowId) {
    return Global(aFlowId ^ gProcessUUID);
  }

  // Same as above, but construct an id from a pointer.
  // NOTE: After the object is destroyed, the value of |aPtr| can be reused for
  // a different object (in particular if the object is allocated on a stack)
  // but it needs to be emitted as terminating flow first.
  static inline Flow FromPointer(void* aPtr) {
    return ProcessScoped(reinterpret_cast<uintptr_t>(aPtr));
  }

  // The caller is responsible for ensuring that it's
  // globally-unique (e.g. by generating a random value). This should be used
  // only for flow events which cross the process boundary (e.g. IPCs).
  static inline Flow Global(uint64_t aFlowId) { return Flow(aFlowId); }

  uint64_t Id() const { return mFlowId; }

  static MFBT_API void Init();

 private:
  explicit Flow(uint64_t aFlowId) : mFlowId(aFlowId) {}
  const uint64_t mFlowId;
};

template <>
struct mozilla::ProfileBufferEntryWriter::Serializer<Flow> {
  static constexpr Length Bytes(const Flow& aFlow) { return sizeof(Flow); }

  static void Write(ProfileBufferEntryWriter& aEW, const Flow& aFlow) {
    aEW.WriteBytes(&aFlow, sizeof(Flow));
  }
};

template <>
struct mozilla::ProfileBufferEntryReader::Deserializer<Flow> {
  static void ReadInto(ProfileBufferEntryReader& aER, uint64_t& aFlow) {
    aER.ReadBytes(&aFlow, sizeof(Flow));
  }

  static Flow Read(ProfileBufferEntryReader& aER) {
    uint64_t flow;
    ReadInto(aER, flow);
    return Flow::Global(flow);
  }
};

#endif  // Flow_h

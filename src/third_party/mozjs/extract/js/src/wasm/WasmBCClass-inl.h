/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 *
 * Copyright 2016 Mozilla Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// This is an INTERNAL header for Wasm baseline compiler: inline BaseCompiler
// methods that don't fit in any other group in particular.

#ifndef wasm_wasm_baseline_object_inl_h
#define wasm_wasm_baseline_object_inl_h

namespace js {
namespace wasm {

const FuncType& BaseCompiler::funcType() const {
  return *moduleEnv_.funcs[func_.index].type;
}

bool BaseCompiler::usesMemory() const {
  return moduleEnv_.memories.length() > 0;
}

bool BaseCompiler::usesSharedMemory(uint32_t memoryIndex) const {
  return moduleEnv_.usesSharedMemory(memoryIndex);
}

const Local& BaseCompiler::localFromSlot(uint32_t slot, MIRType type) {
  MOZ_ASSERT(localInfo_[slot].type == type);
  return localInfo_[slot];
}

BytecodeOffset BaseCompiler::bytecodeOffset() const {
  return iter_.bytecodeOffset();
}

bool BaseCompiler::isMem32(uint32_t memoryIndex) const {
  return moduleEnv_.memories[memoryIndex].indexType() == IndexType::I32;
}

bool BaseCompiler::isMem64(uint32_t memoryIndex) const {
  return moduleEnv_.memories[memoryIndex].indexType() == IndexType::I64;
}

bool BaseCompiler::hugeMemoryEnabled(uint32_t memoryIndex) const {
  return moduleEnv_.hugeMemoryEnabled(memoryIndex);
}

uint32_t BaseCompiler::instanceOffsetOfMemoryBase(uint32_t memoryIndex) const {
  if (memoryIndex == 0) {
    return Instance::offsetOfMemory0Base();
  }
  return Instance::offsetInData(
      moduleEnv_.offsetOfMemoryInstanceData(memoryIndex) +
      offsetof(MemoryInstanceData, base));
}

uint32_t BaseCompiler::instanceOffsetOfBoundsCheckLimit(
    uint32_t memoryIndex) const {
  if (memoryIndex == 0) {
    return Instance::offsetOfMemory0BoundsCheckLimit();
  }
  return Instance::offsetInData(
      moduleEnv_.offsetOfMemoryInstanceData(memoryIndex) +
      offsetof(MemoryInstanceData, boundsCheckLimit));
}

// The results parameter for BaseCompiler::emitCallArgs is used for
// regular Wasm calls.
struct NormalCallResults final {
  const StackResultsLoc& results;
  explicit NormalCallResults(const StackResultsLoc& results)
      : results(results) {}
  inline uint32_t onStackCount() const { return results.count(); }
  inline StackResults stackResults() const { return results.stackResults(); }
  inline void getStackResultArea(BaseStackFrame fr, RegPtr dest) const {
    fr.computeOutgoingStackResultAreaPtr(results, dest);
  }
};

// The results parameter for BaseCompiler::emitCallArgs is used for
// Wasm return/tail calls.
struct TailCallResults final {
  bool hasStackResults;
  explicit TailCallResults(const FuncType& funcType) {
    hasStackResults =
        ABIResultIter::HasStackResults(ResultType::Vector(funcType.results()));
  }
  inline uint32_t onStackCount() const { return 0; }
  inline StackResults stackResults() const {
    return hasStackResults ? StackResults::HasStackResults
                           : StackResults::NoStackResults;
  }
  inline void getStackResultArea(BaseStackFrame fr, RegPtr dest) const {
    fr.loadIncomingStackResultAreaPtr(dest);
  }
};

// The results parameter for BaseCompiler::emitCallArgs is used when
// no result (area) is expected.
struct NoCallResults final {
  inline uint32_t onStackCount() const { return 0; }
  inline StackResults stackResults() const {
    return StackResults::NoStackResults;
  }
  inline void getStackResultArea(BaseStackFrame fr, RegPtr dest) const {
    MOZ_CRASH();
  }
};

}  // namespace wasm
}  // namespace js

#endif  // wasm_wasm_baseline_object_inl_h

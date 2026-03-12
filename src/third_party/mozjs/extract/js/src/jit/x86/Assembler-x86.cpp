/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/x86/Assembler-x86.h"

#include "gc/Marking.h"
#include "util/Memory.h"

using namespace js;
using namespace js::jit;

ABIArgGenerator::ABIArgGenerator() : stackOffset_(0), current_() {}

ABIArg ABIArgGenerator::next(MIRType type) {
  switch (type) {
    case MIRType::Int32:
    case MIRType::Float32:
    case MIRType::Pointer:
    case MIRType::WasmAnyRef:
    case MIRType::WasmArrayData:
    case MIRType::StackResults:
      current_ = ABIArg(stackOffset_);
      stackOffset_ += sizeof(uint32_t);
      break;
    case MIRType::Double:
    case MIRType::Int64:
      current_ = ABIArg(stackOffset_);
      stackOffset_ += sizeof(uint64_t);
      break;
    case MIRType::Simd128:
      // On Win64, >64 bit args need to be passed by reference.  However, wasm
      // doesn't allow passing SIMD values to JS, so the only way to reach this
      // is wasm to wasm calls.  Ergo we can break the native ABI here and use
      // the Wasm ABI instead.
      stackOffset_ = AlignBytes(stackOffset_, SimdMemoryAlignment);
      current_ = ABIArg(stackOffset_);
      stackOffset_ += Simd128DataSize;
      break;
    default:
      MOZ_CRASH("Unexpected argument type");
  }
  return current_;
}

void Assembler::executableCopy(uint8_t* buffer) {
  AssemblerX86Shared::executableCopy(buffer);
  for (RelativePatch& rp : jumps_) {
    X86Encoding::SetRel32(buffer + rp.offset, rp.target);
  }
}

class RelocationIterator {
  CompactBufferReader reader_;
  uint32_t offset_;

 public:
  explicit RelocationIterator(CompactBufferReader& reader) : reader_(reader) {}

  bool read() {
    if (!reader_.more()) {
      return false;
    }
    offset_ = reader_.readUnsigned();
    return true;
  }

  uint32_t offset() const { return offset_; }
};

static inline JitCode* CodeFromJump(uint8_t* jump) {
  uint8_t* target = (uint8_t*)X86Encoding::GetRel32Target(jump);
  return JitCode::FromExecutable(target);
}

void Assembler::TraceJumpRelocations(JSTracer* trc, JitCode* code,
                                     CompactBufferReader& reader) {
  RelocationIterator iter(reader);
  while (iter.read()) {
    JitCode* child = CodeFromJump(code->raw() + iter.offset());
    TraceManuallyBarrieredEdge(trc, &child, "rel32");
    MOZ_ASSERT(child == CodeFromJump(code->raw() + iter.offset()));
  }
}

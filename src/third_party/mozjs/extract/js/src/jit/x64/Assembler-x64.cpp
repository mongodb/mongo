/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/x64/Assembler-x64.h"

#include "gc/Tracer.h"
#include "util/Memory.h"

using namespace js;
using namespace js::jit;

ABIArgGenerator::ABIArgGenerator()
    :
#if defined(XP_WIN)
      regIndex_(0),
      stackOffset_(ShadowStackSpace)
#else
      intRegIndex_(0),
      floatRegIndex_(0),
      stackOffset_(0)
#endif
{
}

ABIArg ABIArgGenerator::next(MIRType type) {
#if defined(XP_WIN)
  static_assert(NumIntArgRegs == NumFloatArgRegs);
  if (regIndex_ == NumIntArgRegs) {
    if (type == MIRType::Simd128) {
      // On Win64, >64 bit args need to be passed by reference.  However, wasm
      // doesn't allow passing SIMD values to JS, so the only way to reach this
      // is wasm to wasm calls.  Ergo we can break the native ABI here and use
      // the Wasm ABI instead.
      stackOffset_ = AlignBytes(stackOffset_, SimdMemoryAlignment);
      current_ = ABIArg(stackOffset_);
      stackOffset_ += Simd128DataSize;
    } else {
      current_ = ABIArg(stackOffset_);
      stackOffset_ += sizeof(uint64_t);
    }
    return current_;
  }
  switch (type) {
    case MIRType::Int32:
    case MIRType::Int64:
    case MIRType::Pointer:
    case MIRType::WasmAnyRef:
    case MIRType::WasmArrayData:
    case MIRType::StackResults:
      current_ = ABIArg(IntArgRegs[regIndex_++]);
      break;
    case MIRType::Float32:
      current_ = ABIArg(FloatArgRegs[regIndex_++].asSingle());
      break;
    case MIRType::Double:
      current_ = ABIArg(FloatArgRegs[regIndex_++]);
      break;
    case MIRType::Simd128:
      // On Win64, >64 bit args need to be passed by reference, but wasm
      // doesn't allow passing SIMD values to FFIs. The only way to reach
      // here is asm to asm calls, so we can break the ABI here.
      current_ = ABIArg(FloatArgRegs[regIndex_++].asSimd128());
      break;
    default:
      MOZ_CRASH("Unexpected argument type");
  }
  return current_;
#else
  switch (type) {
    case MIRType::Int32:
    case MIRType::Int64:
    case MIRType::Pointer:
    case MIRType::WasmAnyRef:
    case MIRType::WasmArrayData:
    case MIRType::StackResults:
      if (intRegIndex_ == NumIntArgRegs) {
        current_ = ABIArg(stackOffset_);
        stackOffset_ += sizeof(uint64_t);
        break;
      }
      current_ = ABIArg(IntArgRegs[intRegIndex_++]);
      break;
    case MIRType::Double:
    case MIRType::Float32:
      if (floatRegIndex_ == NumFloatArgRegs) {
        current_ = ABIArg(stackOffset_);
        stackOffset_ += sizeof(uint64_t);
        break;
      }
      if (type == MIRType::Float32) {
        current_ = ABIArg(FloatArgRegs[floatRegIndex_++].asSingle());
      } else {
        current_ = ABIArg(FloatArgRegs[floatRegIndex_++]);
      }
      break;
    case MIRType::Simd128:
      if (floatRegIndex_ == NumFloatArgRegs) {
        stackOffset_ = AlignBytes(stackOffset_, SimdMemoryAlignment);
        current_ = ABIArg(stackOffset_);
        stackOffset_ += Simd128DataSize;
        break;
      }
      current_ = ABIArg(FloatArgRegs[floatRegIndex_++].asSimd128());
      break;
    default:
      MOZ_CRASH("Unexpected argument type");
  }
  return current_;
#endif
}

void Assembler::addPendingJump(JmpSrc src, ImmPtr target,
                               RelocationKind reloc) {
  MOZ_ASSERT(target.value != nullptr);

  // Emit reloc before modifying the jump table, since it computes a 0-based
  // index. This jump is not patchable at runtime.
  if (reloc == RelocationKind::JITCODE) {
    jumpRelocations_.writeUnsigned(src.offset());
  }

  static_assert(MaxCodeBytesPerProcess <= uint64_t(2) * 1024 * 1024 * 1024,
                "Code depends on using int32_t for cross-JitCode jump offsets");

  MOZ_ASSERT_IF(reloc == RelocationKind::JITCODE,
                AddressIsInExecutableMemory(target.value));

  RelativePatch patch(src.offset(), target.value, reloc);
  if (reloc == RelocationKind::JITCODE ||
      AddressIsInExecutableMemory(target.value)) {
    enoughMemory_ &= codeJumps_.append(patch);
  } else {
    enoughMemory_ &= extendedJumps_.append(patch);
  }
}

void Assembler::finish() {
  if (oom()) {
    return;
  }

  AutoCreatedBy acb(*this, "Assembler::finish");

  if (!extendedJumps_.length()) {
    // Since we may be folowed by non-executable data, eagerly insert an
    // undefined instruction byte to prevent processors from decoding
    // gibberish into their pipelines. See Intel performance guides.
    masm.ud2();
    return;
  }

  // Emit the jump table.
  masm.haltingAlign(SizeOfJumpTableEntry);
  extendedJumpTable_ = masm.size();

  // Zero the extended jumps table.
  for (size_t i = 0; i < extendedJumps_.length(); i++) {
#ifdef DEBUG
    size_t oldSize = masm.size();
#endif
    MOZ_ASSERT(hasCreator());
    masm.jmp_rip(2);
    MOZ_ASSERT_IF(!masm.oom(), masm.size() - oldSize == 6);
    // Following an indirect branch with ud2 hints to the hardware that
    // there's no fall-through. This also aligns the 64-bit immediate.
    masm.ud2();
    MOZ_ASSERT_IF(!masm.oom(), masm.size() - oldSize == 8);
    masm.immediate64(0);
    MOZ_ASSERT_IF(!masm.oom(), masm.size() - oldSize == SizeOfExtendedJump);
    MOZ_ASSERT_IF(!masm.oom(), masm.size() - oldSize == SizeOfJumpTableEntry);
  }
}

void Assembler::executableCopy(uint8_t* buffer) {
  AssemblerX86Shared::executableCopy(buffer);

  for (RelativePatch& rp : codeJumps_) {
    uint8_t* src = buffer + rp.offset;
    MOZ_ASSERT(rp.target);

    MOZ_RELEASE_ASSERT(X86Encoding::CanRelinkJump(src, rp.target));
    X86Encoding::SetRel32(src, rp.target);
  }

  for (size_t i = 0; i < extendedJumps_.length(); i++) {
    RelativePatch& rp = extendedJumps_[i];
    uint8_t* src = buffer + rp.offset;
    MOZ_ASSERT(rp.target);

    if (X86Encoding::CanRelinkJump(src, rp.target)) {
      X86Encoding::SetRel32(src, rp.target);
    } else {
      // An extended jump table must exist, and its offset must be in
      // range.
      MOZ_ASSERT(extendedJumpTable_);
      MOZ_ASSERT((extendedJumpTable_ + i * SizeOfJumpTableEntry) <=
                 size() - SizeOfJumpTableEntry);

      // Patch the jump to go to the extended jump entry.
      uint8_t* entry = buffer + extendedJumpTable_ + i * SizeOfJumpTableEntry;
      X86Encoding::SetRel32(src, entry);

      // Now patch the pointer, note that we need to align it to
      // *after* the extended jump, i.e. after the 64-bit immedate.
      X86Encoding::SetPointer(entry + SizeOfExtendedJump, rp.target);
    }
  }
}

class RelocationIterator {
  CompactBufferReader reader_;
  uint32_t offset_ = 0;

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

JitCode* Assembler::CodeFromJump(JitCode* code, uint8_t* jump) {
  uint8_t* target = (uint8_t*)X86Encoding::GetRel32Target(jump);

  MOZ_ASSERT(!code->containsNativePC(target),
             "Extended jump table not used for cross-JitCode jumps");

  return JitCode::FromExecutable(target);
}

void Assembler::TraceJumpRelocations(JSTracer* trc, JitCode* code,
                                     CompactBufferReader& reader) {
  RelocationIterator iter(reader);
  while (iter.read()) {
    JitCode* child = CodeFromJump(code, code->raw() + iter.offset());
    TraceManuallyBarrieredEdge(trc, &child, "rel32");
    MOZ_ASSERT(child == CodeFromJump(code, code->raw() + iter.offset()));
  }
}

/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_JitCode_h
#define jit_JitCode_h

#include "mozilla/MemoryReporting.h"  // MallocSizeOf

#include <stddef.h>  // size_t
#include <stdint.h>  // uint8_t, uint32_t

#include "jstypes.h"

#include "gc/Allocator.h"  // AllowGC
#include "gc/Cell.h"       // gc::TenuredCellWithNonGCPointer
#include "js/TraceKind.h"  // JS::TraceKind
#include "js/UbiNode.h"    // ubi::{TracerConcrete, Size, CourseType}

namespace js {
namespace jit {

class ExecutablePool;
class JitCode;
class MacroAssembler;

enum class CodeKind : uint8_t;

// Header at start of raw code buffer
struct JitCodeHeader {
  // Link back to corresponding gcthing
  JitCode* jitCode_;

  void init(JitCode* jitCode);

  static JitCodeHeader* FromExecutable(uint8_t* buffer) {
    return (JitCodeHeader*)(buffer - sizeof(JitCodeHeader));
  }
};

class JitCode : public gc::TenuredCellWithNonGCPointer<uint8_t> {
  friend class gc::CellAllocator;

 public:
  // Raw code pointer, stored in the cell header.
  uint8_t* raw() const { return headerPtr(); }

 protected:
  ExecutablePool* pool_;
  uint32_t bufferSize_;  // Total buffer size. Does not include headerSize_.
  uint32_t insnSize_;    // Instruction stream size.
  uint32_t dataSize_;    // Size of the read-only data area.
  uint32_t jumpRelocTableBytes_;  // Size of the jump relocation table.
  uint32_t dataRelocTableBytes_;  // Size of the data relocation table.
  uint8_t headerSize_ : 5;        // Number of bytes allocated before codeStart.
  uint8_t kind_ : 3;              // jit::CodeKind, for the memory reporters.
  bool invalidated_ : 1;     // Whether the code object has been invalidated.
                             // This is necessary to prevent GC tracing.
  bool hasBytecodeMap_ : 1;  // Whether the code object has been registered with
                             // native=>bytecode mapping tables.

  JitCode() = delete;
  JitCode(uint8_t* code, uint32_t bufferSize, uint32_t headerSize,
          ExecutablePool* pool, CodeKind kind)
      : TenuredCellWithNonGCPointer(code),
        pool_(pool),
        bufferSize_(bufferSize),
        insnSize_(0),
        dataSize_(0),
        jumpRelocTableBytes_(0),
        dataRelocTableBytes_(0),
        headerSize_(headerSize),
        kind_(uint8_t(kind)),
        invalidated_(false),
        hasBytecodeMap_(false) {
    MOZ_ASSERT(CodeKind(kind_) == kind);
    MOZ_ASSERT(headerSize_ == headerSize);
  }

  uint32_t dataOffset() const { return insnSize_; }
  uint32_t jumpRelocTableOffset() const { return dataOffset() + dataSize_; }
  uint32_t dataRelocTableOffset() const {
    return jumpRelocTableOffset() + jumpRelocTableBytes_;
  }

 public:
  uint8_t* rawEnd() const { return raw() + insnSize_; }
  bool containsNativePC(const void* addr) const {
    const uint8_t* addr_u8 = (const uint8_t*)addr;
    return raw() <= addr_u8 && addr_u8 < rawEnd();
  }
  size_t instructionsSize() const { return insnSize_; }
  size_t bufferSize() const { return bufferSize_; }
  size_t headerSize() const { return headerSize_; }

  void traceChildren(JSTracer* trc);
  void finalize(JS::GCContext* gcx);
  void setInvalidated() { invalidated_ = true; }

  void setHasBytecodeMap() { hasBytecodeMap_ = true; }

  // If this JitCode object has been, effectively, corrupted due to
  // invalidation patching, then we have to remember this so we don't try and
  // trace relocation entries that may now be corrupt.
  bool invalidated() const { return !!invalidated_; }

  template <typename T>
  T as() const {
    return JS_DATA_TO_FUNC_PTR(T, raw());
  }

  void copyFrom(MacroAssembler& masm);

  static JitCode* FromExecutable(uint8_t* buffer) {
    JitCode* code = JitCodeHeader::FromExecutable(buffer)->jitCode_;
    MOZ_ASSERT(code->raw() == buffer);
    return code;
  }

  static size_t offsetOfCode() { return offsetOfHeaderPtr(); }

  uint8_t* jumpRelocTable() { return raw() + jumpRelocTableOffset(); }

  // Allocates a new JitCode object which will be managed by the GC. If no
  // object can be allocated, nullptr is returned. On failure, |pool| is
  // automatically released, so the code may be freed.
  template <AllowGC allowGC>
  static JitCode* New(JSContext* cx, uint8_t* code, uint32_t totalSize,
                      uint32_t headerSize, ExecutablePool* pool, CodeKind kind);

 public:
  static const JS::TraceKind TraceKind = JS::TraceKind::JitCode;
};

}  // namespace jit
}  // namespace js

// JS::ubi::Nodes can point to js::jit::JitCode instances; they're js::gc::Cell
// instances with no associated compartment.
namespace JS {
namespace ubi {
template <>
class Concrete<js::jit::JitCode> : TracerConcrete<js::jit::JitCode> {
 protected:
  explicit Concrete(js::jit::JitCode* ptr)
      : TracerConcrete<js::jit::JitCode>(ptr) {}

 public:
  static void construct(void* storage, js::jit::JitCode* ptr) {
    new (storage) Concrete(ptr);
  }

  CoarseType coarseType() const final { return CoarseType::Script; }

  Size size(mozilla::MallocSizeOf mallocSizeOf) const override {
    Size size = js::gc::Arena::thingSize(get().asTenured().getAllocKind());
    size += get().bufferSize();
    size += get().headerSize();
    return size;
  }

  const char16_t* typeName() const override { return concreteTypeName; }
  static const char16_t concreteTypeName[];
};

}  // namespace ubi
}  // namespace JS

#endif /* jit_JitCode_h */

/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_CacheIRReader_h
#define jit_CacheIRReader_h

#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"

#include <stdint.h>
#include "NamespaceImports.h"

#include "jit/CacheIR.h"
#include "jit/CacheIRWriter.h"
#include "jit/CompactBuffer.h"
#include "js/ScalarType.h"
#include "js/Value.h"
#include "wasm/WasmValType.h"

enum class JSOp : uint8_t;

namespace js {

enum class UnaryMathFunction : uint8_t;

namespace gc {
enum class AllocKind : uint8_t;
}

namespace jit {

class CacheIRStubInfo;

// Helper class for reading CacheIR bytecode.
class MOZ_RAII CacheIRReader {
  CompactBufferReader buffer_;

  CacheIRReader(const CacheIRReader&) = delete;
  CacheIRReader& operator=(const CacheIRReader&) = delete;

 public:
  CacheIRReader(const uint8_t* start, const uint8_t* end)
      : buffer_(start, end) {}
  explicit CacheIRReader(const CacheIRWriter& writer)
      : CacheIRReader(writer.codeStart(), writer.codeEnd()) {}
  explicit CacheIRReader(const CacheIRStubInfo* stubInfo);

  bool more() const { return buffer_.more(); }

  CacheOp readOp() { return CacheOp(buffer_.readUnsigned15Bit()); }

  // Skip data not currently used.
  void skip() { buffer_.readByte(); }
  void skip(uint32_t skipLength) {
    if (skipLength > 0) {
      buffer_.seek(buffer_.currentPosition(), skipLength);
    }
  }

  ValOperandId valOperandId() { return ValOperandId(buffer_.readByte()); }
  ValueTagOperandId valueTagOperandId() {
    return ValueTagOperandId(buffer_.readByte());
  }

  IntPtrOperandId intPtrOperandId() {
    return IntPtrOperandId(buffer_.readByte());
  }

  ObjOperandId objOperandId() { return ObjOperandId(buffer_.readByte()); }
  NumberOperandId numberOperandId() {
    return NumberOperandId(buffer_.readByte());
  }
  StringOperandId stringOperandId() {
    return StringOperandId(buffer_.readByte());
  }

  SymbolOperandId symbolOperandId() {
    return SymbolOperandId(buffer_.readByte());
  }

  BigIntOperandId bigIntOperandId() {
    return BigIntOperandId(buffer_.readByte());
  }

  BooleanOperandId booleanOperandId() {
    return BooleanOperandId(buffer_.readByte());
  }

  Int32OperandId int32OperandId() { return Int32OperandId(buffer_.readByte()); }

  uint32_t rawOperandId() { return buffer_.readByte(); }

  uint32_t stubOffset() { return buffer_.readByte() * sizeof(uintptr_t); }
  GuardClassKind guardClassKind() { return GuardClassKind(buffer_.readByte()); }
  ValueType valueType() { return ValueType(buffer_.readByte()); }
  wasm::ValType::Kind wasmValType() {
    return wasm::ValType::Kind(buffer_.readByte());
  }
  gc::AllocKind allocKind() { return gc::AllocKind(buffer_.readByte()); }
  CompletionKind completionKind() { return CompletionKind(buffer_.readByte()); }

  Scalar::Type scalarType() { return Scalar::Type(buffer_.readByte()); }
  JSWhyMagic whyMagic() { return JSWhyMagic(buffer_.readByte()); }
  JSOp jsop() { return JSOp(buffer_.readByte()); }
  int32_t int32Immediate() { return int32_t(buffer_.readFixedUint32_t()); }
  uint32_t uint32Immediate() { return buffer_.readFixedUint32_t(); }
  void* pointer() { return buffer_.readRawPointer(); }

  UnaryMathFunction unaryMathFunction() {
    return UnaryMathFunction(buffer_.readByte());
  }

  CallFlags callFlags() {
    // See CacheIRWriter::writeCallFlagsImm()
    uint8_t encoded = buffer_.readByte();
    CallFlags::ArgFormat format =
        CallFlags::ArgFormat(encoded & CallFlags::ArgFormatMask);
    bool isConstructing = encoded & CallFlags::IsConstructing;
    bool isSameRealm = encoded & CallFlags::IsSameRealm;
    bool needsUninitializedThis = encoded & CallFlags::NeedsUninitializedThis;
    MOZ_ASSERT_IF(needsUninitializedThis, isConstructing);
    switch (format) {
      case CallFlags::Unknown:
        MOZ_CRASH("Unexpected call flags");
      case CallFlags::Standard:
        return CallFlags(isConstructing, /*isSpread =*/false, isSameRealm,
                         needsUninitializedThis);
      case CallFlags::Spread:
        return CallFlags(isConstructing, /*isSpread =*/true, isSameRealm,
                         needsUninitializedThis);
      default:
        // The existing non-standard argument formats (FunCall and FunApply)
        // can't be constructors.
        MOZ_ASSERT(!isConstructing);
        return CallFlags(format);
    }
  }

  uint8_t readByte() { return buffer_.readByte(); }
  bool readBool() {
    uint8_t b = buffer_.readByte();
    MOZ_ASSERT(b <= 1);
    return bool(b);
  }

  const uint8_t* currentPosition() const { return buffer_.currentPosition(); }
};

}  // namespace jit
}  // namespace js

#endif /* jit_CacheIRReader_h */

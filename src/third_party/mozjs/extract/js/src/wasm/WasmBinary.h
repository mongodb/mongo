/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 *
 * Copyright 2021 Mozilla Foundation
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

#ifndef wasm_binary_h
#define wasm_binary_h

#include "mozilla/DebugOnly.h"
#include "mozilla/Maybe.h"

#include <type_traits>

#include "js/WasmFeatures.h"

#include "wasm/WasmCompile.h"
#include "wasm/WasmCompileArgs.h"
#include "wasm/WasmConstants.h"
#include "wasm/WasmTypeDecls.h"
#include "wasm/WasmTypeDef.h"
#include "wasm/WasmValType.h"

namespace js {
namespace wasm {

using mozilla::DebugOnly;
using mozilla::Maybe;

struct ModuleEnvironment;

// The Opcode compactly and safely represents the primary opcode plus any
// extension, with convenient predicates and accessors.

class Opcode {
  uint32_t bits_;

 public:
  MOZ_IMPLICIT Opcode(Op op) : bits_(uint32_t(op)) {
    static_assert(size_t(Op::Limit) == 256, "fits");
    MOZ_ASSERT(size_t(op) < size_t(Op::Limit));
  }
  MOZ_IMPLICIT Opcode(MiscOp op)
      : bits_((uint32_t(op) << 8) | uint32_t(Op::MiscPrefix)) {
    static_assert(size_t(MiscOp::Limit) <= 0xFFFFFF, "fits");
    MOZ_ASSERT(size_t(op) < size_t(MiscOp::Limit));
  }
  MOZ_IMPLICIT Opcode(ThreadOp op)
      : bits_((uint32_t(op) << 8) | uint32_t(Op::ThreadPrefix)) {
    static_assert(size_t(ThreadOp::Limit) <= 0xFFFFFF, "fits");
    MOZ_ASSERT(size_t(op) < size_t(ThreadOp::Limit));
  }
  MOZ_IMPLICIT Opcode(MozOp op)
      : bits_((uint32_t(op) << 8) | uint32_t(Op::MozPrefix)) {
    static_assert(size_t(MozOp::Limit) <= 0xFFFFFF, "fits");
    MOZ_ASSERT(size_t(op) < size_t(MozOp::Limit));
  }
  MOZ_IMPLICIT Opcode(SimdOp op)
      : bits_((uint32_t(op) << 8) | uint32_t(Op::SimdPrefix)) {
    static_assert(size_t(SimdOp::Limit) <= 0xFFFFFF, "fits");
    MOZ_ASSERT(size_t(op) < size_t(SimdOp::Limit));
  }
  MOZ_IMPLICIT Opcode(GcOp op)
      : bits_((uint32_t(op) << 8) | uint32_t(Op::GcPrefix)) {
    static_assert(size_t(SimdOp::Limit) <= 0xFFFFFF, "fits");
    MOZ_ASSERT(size_t(op) < size_t(SimdOp::Limit));
  }

  bool isOp() const { return bits_ < uint32_t(Op::FirstPrefix); }
  bool isMisc() const { return (bits_ & 255) == uint32_t(Op::MiscPrefix); }
  bool isThread() const { return (bits_ & 255) == uint32_t(Op::ThreadPrefix); }
  bool isMoz() const { return (bits_ & 255) == uint32_t(Op::MozPrefix); }
  bool isSimd() const { return (bits_ & 255) == uint32_t(Op::SimdPrefix); }
  bool isGc() const { return (bits_ & 255) == uint32_t(Op::GcPrefix); }

  Op asOp() const {
    MOZ_ASSERT(isOp());
    return Op(bits_);
  }
  MiscOp asMisc() const {
    MOZ_ASSERT(isMisc());
    return MiscOp(bits_ >> 8);
  }
  ThreadOp asThread() const {
    MOZ_ASSERT(isThread());
    return ThreadOp(bits_ >> 8);
  }
  MozOp asMoz() const {
    MOZ_ASSERT(isMoz());
    return MozOp(bits_ >> 8);
  }
  SimdOp asSimd() const {
    MOZ_ASSERT(isSimd());
    return SimdOp(bits_ >> 8);
  }
  GcOp asGc() const {
    MOZ_ASSERT(isGc());
    return GcOp(bits_ >> 8);
  }

  uint32_t bits() const { return bits_; }

  bool operator==(const Opcode& that) const { return bits_ == that.bits_; }
  bool operator!=(const Opcode& that) const { return bits_ != that.bits_; }
};

// This struct captures the bytecode offset of a section's payload (so not
// including the header) and the size of the payload.

struct SectionRange {
  uint32_t start;
  uint32_t size;

  uint32_t end() const { return start + size; }
  bool operator==(const SectionRange& rhs) const {
    return start == rhs.start && size == rhs.size;
  }
};

using MaybeSectionRange = Maybe<SectionRange>;

// The Encoder class appends bytes to the Bytes object it is given during
// construction. The client is responsible for the Bytes's lifetime and must
// keep the Bytes alive as long as the Encoder is used.

class Encoder {
  Bytes& bytes_;
  const TypeContext* types_;

  template <class T>
  [[nodiscard]] bool write(const T& v) {
    return bytes_.append(reinterpret_cast<const uint8_t*>(&v), sizeof(T));
  }

  template <typename UInt>
  [[nodiscard]] bool writeVarU(UInt i) {
    do {
      uint8_t byte = i & 0x7f;
      i >>= 7;
      if (i != 0) {
        byte |= 0x80;
      }
      if (!bytes_.append(byte)) {
        return false;
      }
    } while (i != 0);
    return true;
  }

  template <typename SInt>
  [[nodiscard]] bool writeVarS(SInt i) {
    bool done;
    do {
      uint8_t byte = i & 0x7f;
      i >>= 7;
      done = ((i == 0) && !(byte & 0x40)) || ((i == -1) && (byte & 0x40));
      if (!done) {
        byte |= 0x80;
      }
      if (!bytes_.append(byte)) {
        return false;
      }
    } while (!done);
    return true;
  }

  void patchVarU32(size_t offset, uint32_t patchBits, uint32_t assertBits) {
    do {
      uint8_t assertByte = assertBits & 0x7f;
      uint8_t patchByte = patchBits & 0x7f;
      assertBits >>= 7;
      patchBits >>= 7;
      if (assertBits != 0) {
        assertByte |= 0x80;
        patchByte |= 0x80;
      }
      MOZ_ASSERT(assertByte == bytes_[offset]);
      (void)assertByte;
      bytes_[offset] = patchByte;
      offset++;
    } while (assertBits != 0);
  }

  void patchFixedU7(size_t offset, uint8_t patchBits, uint8_t assertBits) {
    MOZ_ASSERT(patchBits <= uint8_t(INT8_MAX));
    patchFixedU8(offset, patchBits, assertBits);
  }

  void patchFixedU8(size_t offset, uint8_t patchBits, uint8_t assertBits) {
    MOZ_ASSERT(bytes_[offset] == assertBits);
    bytes_[offset] = patchBits;
  }

  uint32_t varU32ByteLength(size_t offset) const {
    size_t start = offset;
    while (bytes_[offset] & 0x80) {
      offset++;
    }
    return offset - start + 1;
  }

 public:
  explicit Encoder(Bytes& bytes) : bytes_(bytes), types_(nullptr) {
    MOZ_ASSERT(empty());
  }
  explicit Encoder(Bytes& bytes, const TypeContext& types)
      : bytes_(bytes), types_(&types) {
    MOZ_ASSERT(empty());
  }

  size_t currentOffset() const { return bytes_.length(); }
  bool empty() const { return currentOffset() == 0; }

  // Fixed-size encoding operations simply copy the literal bytes (without
  // attempting to align).

  [[nodiscard]] bool writeFixedU7(uint8_t i) {
    MOZ_ASSERT(i <= uint8_t(INT8_MAX));
    return writeFixedU8(i);
  }
  [[nodiscard]] bool writeFixedU8(uint8_t i) { return write<uint8_t>(i); }
  [[nodiscard]] bool writeFixedU32(uint32_t i) { return write<uint32_t>(i); }
  [[nodiscard]] bool writeFixedF32(float f) { return write<float>(f); }
  [[nodiscard]] bool writeFixedF64(double d) { return write<double>(d); }

  // Variable-length encodings that all use LEB128.

  [[nodiscard]] bool writeVarU32(uint32_t i) { return writeVarU<uint32_t>(i); }
  [[nodiscard]] bool writeVarS32(int32_t i) { return writeVarS<int32_t>(i); }
  [[nodiscard]] bool writeVarU64(uint64_t i) { return writeVarU<uint64_t>(i); }
  [[nodiscard]] bool writeVarS64(int64_t i) { return writeVarS<int64_t>(i); }
  [[nodiscard]] bool writeValType(ValType type) {
    static_assert(size_t(TypeCode::Limit) <= UINT8_MAX, "fits");
    if (type.isTypeRef()) {
      MOZ_RELEASE_ASSERT(types_,
                         "writeValType is used, but types were not specified.");
      if (!writeFixedU8(uint8_t(type.isNullable() ? TypeCode::NullableRef
                                                  : TypeCode::Ref))) {
        return false;
      }
      uint32_t typeIndex = types_->indexOf(*type.typeDef());
      // Encode positive LEB S33 as S64.
      return writeVarS64(typeIndex);
    }
    TypeCode tc = type.packed().typeCode();
    MOZ_ASSERT(size_t(tc) < size_t(TypeCode::Limit));
    return writeFixedU8(uint8_t(tc));
  }
  [[nodiscard]] bool writeOp(Opcode opcode) {
    // The Opcode constructor has asserted that `opcode` is meaningful, so no
    // further correctness checking is necessary here.
    uint32_t bits = opcode.bits();
    if (!writeFixedU8(bits & 255)) {
      return false;
    }
    if (opcode.isOp()) {
      return true;
    }
    return writeVarU32(bits >> 8);
  }

  // Fixed-length encodings that allow back-patching.

  [[nodiscard]] bool writePatchableFixedU7(size_t* offset) {
    *offset = bytes_.length();
    return writeFixedU8(UINT8_MAX);
  }
  void patchFixedU7(size_t offset, uint8_t patchBits) {
    return patchFixedU7(offset, patchBits, UINT8_MAX);
  }

  // Variable-length encodings that allow back-patching.

  [[nodiscard]] bool writePatchableVarU32(size_t* offset) {
    *offset = bytes_.length();
    return writeVarU32(UINT32_MAX);
  }
  void patchVarU32(size_t offset, uint32_t patchBits) {
    return patchVarU32(offset, patchBits, UINT32_MAX);
  }

  // Byte ranges start with an LEB128 length followed by an arbitrary sequence
  // of bytes. When used for strings, bytes are to be interpreted as utf8.

  [[nodiscard]] bool writeBytes(const void* bytes, uint32_t numBytes) {
    return writeVarU32(numBytes) &&
           bytes_.append(reinterpret_cast<const uint8_t*>(bytes), numBytes);
  }

  // A "section" is a contiguous range of bytes that stores its own size so
  // that it may be trivially skipped without examining the payload. Sections
  // require backpatching since the size of the section is only known at the
  // end while the size's varU32 must be stored at the beginning. Immediately
  // after the section length is the string id of the section.

  [[nodiscard]] bool startSection(SectionId id, size_t* offset) {
    MOZ_ASSERT(uint32_t(id) < 128);
    return writeVarU32(uint32_t(id)) && writePatchableVarU32(offset);
  }
  void finishSection(size_t offset) {
    return patchVarU32(offset,
                       bytes_.length() - offset - varU32ByteLength(offset));
  }
};

// The Decoder class decodes the bytes in the range it is given during
// construction. The client is responsible for keeping the byte range alive as
// long as the Decoder is used.

class Decoder {
  const uint8_t* const beg_;
  const uint8_t* const end_;
  const uint8_t* cur_;
  const size_t offsetInModule_;
  UniqueChars* error_;
  UniqueCharsVector* warnings_;
  bool resilientMode_;

  template <class T>
  [[nodiscard]] bool read(T* out) {
    if (bytesRemain() < sizeof(T)) {
      return false;
    }
    memcpy((void*)out, cur_, sizeof(T));
    cur_ += sizeof(T);
    return true;
  }

  template <class T>
  T uncheckedRead() {
    MOZ_ASSERT(bytesRemain() >= sizeof(T));
    T ret;
    memcpy(&ret, cur_, sizeof(T));
    cur_ += sizeof(T);
    return ret;
  }

  template <class T>
  void uncheckedRead(T* ret) {
    MOZ_ASSERT(bytesRemain() >= sizeof(T));
    memcpy(ret, cur_, sizeof(T));
    cur_ += sizeof(T);
  }

  template <typename UInt>
  [[nodiscard]] bool readVarU(UInt* out) {
    DebugOnly<const uint8_t*> before = cur_;
    const unsigned numBits = sizeof(UInt) * CHAR_BIT;
    const unsigned remainderBits = numBits % 7;
    const unsigned numBitsInSevens = numBits - remainderBits;
    UInt u = 0;
    uint8_t byte;
    UInt shift = 0;
    do {
      if (!readFixedU8(&byte)) {
        return false;
      }
      if (!(byte & 0x80)) {
        *out = u | UInt(byte) << shift;
        return true;
      }
      u |= UInt(byte & 0x7F) << shift;
      shift += 7;
    } while (shift != numBitsInSevens);
    if (!readFixedU8(&byte) || (byte & (unsigned(-1) << remainderBits))) {
      return false;
    }
    *out = u | (UInt(byte) << numBitsInSevens);
    MOZ_ASSERT_IF(sizeof(UInt) == 4,
                  unsigned(cur_ - before) <= MaxVarU32DecodedBytes);
    return true;
  }

  template <typename SInt>
  [[nodiscard]] bool readVarS(SInt* out) {
    using UInt = std::make_unsigned_t<SInt>;
    const unsigned numBits = sizeof(SInt) * CHAR_BIT;
    const unsigned remainderBits = numBits % 7;
    const unsigned numBitsInSevens = numBits - remainderBits;
    SInt s = 0;
    uint8_t byte;
    unsigned shift = 0;
    do {
      if (!readFixedU8(&byte)) {
        return false;
      }
      s |= SInt(byte & 0x7f) << shift;
      shift += 7;
      if (!(byte & 0x80)) {
        if (byte & 0x40) {
          s |= UInt(-1) << shift;
        }
        *out = s;
        return true;
      }
    } while (shift < numBitsInSevens);
    if (!remainderBits || !readFixedU8(&byte) || (byte & 0x80)) {
      return false;
    }
    uint8_t mask = 0x7f & (uint8_t(-1) << remainderBits);
    if ((byte & mask) != ((byte & (1 << (remainderBits - 1))) ? mask : 0)) {
      return false;
    }
    *out = s | UInt(byte) << shift;
    return true;
  }

 public:
  Decoder(const uint8_t* begin, const uint8_t* end, size_t offsetInModule,
          UniqueChars* error, UniqueCharsVector* warnings = nullptr,
          bool resilientMode = false)
      : beg_(begin),
        end_(end),
        cur_(begin),
        offsetInModule_(offsetInModule),
        error_(error),
        warnings_(warnings),
        resilientMode_(resilientMode) {
    MOZ_ASSERT(begin <= end);
  }
  explicit Decoder(const Bytes& bytes, size_t offsetInModule = 0,
                   UniqueChars* error = nullptr,
                   UniqueCharsVector* warnings = nullptr)
      : beg_(bytes.begin()),
        end_(bytes.end()),
        cur_(bytes.begin()),
        offsetInModule_(offsetInModule),
        error_(error),
        warnings_(warnings),
        resilientMode_(false) {}

  // These convenience functions use currentOffset() as the errorOffset.
  bool fail(const char* msg) { return fail(currentOffset(), msg); }
  bool failf(const char* msg, ...) MOZ_FORMAT_PRINTF(2, 3);
  void warnf(const char* msg, ...) MOZ_FORMAT_PRINTF(2, 3);

  // Report an error at the given offset (relative to the whole module).
  bool fail(size_t errorOffset, const char* msg);

  UniqueChars* error() { return error_; }

  void clearError() {
    if (error_) {
      error_->reset();
    }
  }

  bool done() const {
    MOZ_ASSERT(cur_ <= end_);
    return cur_ == end_;
  }
  bool resilientMode() const { return resilientMode_; }

  size_t bytesRemain() const {
    MOZ_ASSERT(end_ >= cur_);
    return size_t(end_ - cur_);
  }
  // pos must be a value previously returned from currentPosition.
  void rollbackPosition(const uint8_t* pos) { cur_ = pos; }
  const uint8_t* currentPosition() const { return cur_; }
  size_t beginOffset() const { return offsetInModule_; }
  size_t currentOffset() const { return offsetInModule_ + (cur_ - beg_); }
  const uint8_t* begin() const { return beg_; }
  const uint8_t* end() const { return end_; }

  // Peek at the next byte, if it exists, without advancing the position.

  bool peekByte(uint8_t* byte) {
    if (done()) {
      return false;
    }
    *byte = *cur_;
    return true;
  }

  // Fixed-size encoding operations simply copy the literal bytes (without
  // attempting to align).

  [[nodiscard]] bool readFixedU8(uint8_t* i) { return read<uint8_t>(i); }
  [[nodiscard]] bool readFixedU32(uint32_t* u) { return read<uint32_t>(u); }
  [[nodiscard]] bool readFixedF32(float* f) { return read<float>(f); }
  [[nodiscard]] bool readFixedF64(double* d) { return read<double>(d); }
#ifdef ENABLE_WASM_SIMD
  [[nodiscard]] bool readFixedV128(V128* d) {
    for (unsigned i = 0; i < 16; i++) {
      if (!read<uint8_t>(d->bytes + i)) {
        return false;
      }
    }
    return true;
  }
#endif

  // Variable-length encodings that all use LEB128.

  [[nodiscard]] bool readVarU32(uint32_t* out) {
    return readVarU<uint32_t>(out);
  }
  [[nodiscard]] bool readVarS32(int32_t* out) { return readVarS<int32_t>(out); }
  [[nodiscard]] bool readVarU64(uint64_t* out) {
    return readVarU<uint64_t>(out);
  }
  [[nodiscard]] bool readVarS64(int64_t* out) { return readVarS<int64_t>(out); }

  // Value and reference types

  [[nodiscard]] ValType uncheckedReadValType(const TypeContext& types);

  template <class T>
  [[nodiscard]] bool readPackedType(const TypeContext& types,
                                    const FeatureArgs& features, T* type);

  [[nodiscard]] bool readValType(const TypeContext& types,
                                 const FeatureArgs& features, ValType* type);

  [[nodiscard]] bool readStorageType(const TypeContext& types,
                                     const FeatureArgs& features,
                                     StorageType* type);

  [[nodiscard]] bool readHeapType(const TypeContext& types,
                                  const FeatureArgs& features, bool nullable,
                                  RefType* type);

  [[nodiscard]] bool readRefType(const TypeContext& types,
                                 const FeatureArgs& features, RefType* type);

  // Instruction opcode

  [[nodiscard]] bool readOp(OpBytes* op);

  // Instruction immediates for constant instructions

  [[nodiscard]] bool readBinary() { return true; }
  [[nodiscard]] bool readTypeIndex(uint32_t* typeIndex);
  [[nodiscard]] bool readGlobalIndex(uint32_t* globalIndex);
  [[nodiscard]] bool readFuncIndex(uint32_t* funcIndex);
  [[nodiscard]] bool readI32Const(int32_t* i32);
  [[nodiscard]] bool readI64Const(int64_t* i64);
  [[nodiscard]] bool readF32Const(float* f32);
  [[nodiscard]] bool readF64Const(double* f64);
#ifdef ENABLE_WASM_SIMD
  [[nodiscard]] bool readV128Const(V128* value);
#endif
  [[nodiscard]] bool readRefNull(const TypeContext& types,
                                 const FeatureArgs& features, RefType* type);

  // See writeBytes comment.

  [[nodiscard]] bool readBytes(uint32_t numBytes,
                               const uint8_t** bytes = nullptr) {
    if (bytes) {
      *bytes = cur_;
    }
    if (bytesRemain() < numBytes) {
      return false;
    }
    cur_ += numBytes;
    return true;
  }

  // See "section" description in Encoder.

  [[nodiscard]] bool readSectionHeader(uint8_t* id, SectionRange* range);

  [[nodiscard]] bool startSection(SectionId id, ModuleEnvironment* env,
                                  MaybeSectionRange* range,
                                  const char* sectionName);
  [[nodiscard]] bool finishSection(const SectionRange& range,
                                   const char* sectionName);

  // Custom sections do not cause validation errors unless the error is in
  // the section header itself.

  [[nodiscard]] bool startCustomSection(const char* expected,
                                        size_t expectedLength,
                                        ModuleEnvironment* env,
                                        MaybeSectionRange* range);

  template <size_t NameSizeWith0>
  [[nodiscard]] bool startCustomSection(const char (&name)[NameSizeWith0],
                                        ModuleEnvironment* env,
                                        MaybeSectionRange* range) {
    MOZ_ASSERT(name[NameSizeWith0 - 1] == '\0');
    return startCustomSection(name, NameSizeWith0 - 1, env, range);
  }

  void finishCustomSection(const char* name, const SectionRange& range);
  void skipAndFinishCustomSection(const SectionRange& range);

  [[nodiscard]] bool skipCustomSection(ModuleEnvironment* env);

  // The Name section has its own optional subsections.

  [[nodiscard]] bool startNameSubsection(NameType nameType,
                                         Maybe<uint32_t>* endOffset);
  [[nodiscard]] bool finishNameSubsection(uint32_t endOffset);
  [[nodiscard]] bool skipNameSubsection();

  // The infallible "unchecked" decoding functions can be used when we are
  // sure that the bytes are well-formed (by construction or due to previous
  // validation).

  uint8_t uncheckedReadFixedU8() { return uncheckedRead<uint8_t>(); }
  uint32_t uncheckedReadFixedU32() { return uncheckedRead<uint32_t>(); }
  void uncheckedReadFixedF32(float* out) { uncheckedRead<float>(out); }
  void uncheckedReadFixedF64(double* out) { uncheckedRead<double>(out); }
  template <typename UInt>
  UInt uncheckedReadVarU() {
    static const unsigned numBits = sizeof(UInt) * CHAR_BIT;
    static const unsigned remainderBits = numBits % 7;
    static const unsigned numBitsInSevens = numBits - remainderBits;
    UInt decoded = 0;
    uint32_t shift = 0;
    do {
      uint8_t byte = *cur_++;
      if (!(byte & 0x80)) {
        return decoded | (UInt(byte) << shift);
      }
      decoded |= UInt(byte & 0x7f) << shift;
      shift += 7;
    } while (shift != numBitsInSevens);
    uint8_t byte = *cur_++;
    MOZ_ASSERT(!(byte & 0xf0));
    return decoded | (UInt(byte) << numBitsInSevens);
  }
  uint32_t uncheckedReadVarU32() { return uncheckedReadVarU<uint32_t>(); }
  int32_t uncheckedReadVarS32() {
    int32_t i32 = 0;
    MOZ_ALWAYS_TRUE(readVarS32(&i32));
    return i32;
  }
  uint64_t uncheckedReadVarU64() { return uncheckedReadVarU<uint64_t>(); }
  int64_t uncheckedReadVarS64() {
    int64_t i64 = 0;
    MOZ_ALWAYS_TRUE(readVarS64(&i64));
    return i64;
  }
  Op uncheckedReadOp() {
    static_assert(size_t(Op::Limit) == 256, "fits");
    uint8_t u8 = uncheckedReadFixedU8();
    return u8 != UINT8_MAX ? Op(u8) : Op(uncheckedReadFixedU8() + UINT8_MAX);
  }
};

// Value and reference types

inline ValType Decoder::uncheckedReadValType(const TypeContext& types) {
  uint8_t code = uncheckedReadFixedU8();
  switch (code) {
    case uint8_t(TypeCode::FuncRef):
    case uint8_t(TypeCode::ExternRef):
    case uint8_t(TypeCode::ExnRef):
      return RefType::fromTypeCode(TypeCode(code), true);
    case uint8_t(TypeCode::Ref):
    case uint8_t(TypeCode::NullableRef): {
      bool nullable = code == uint8_t(TypeCode::NullableRef);

      uint8_t nextByte;
      peekByte(&nextByte);

      if ((nextByte & SLEB128SignMask) == SLEB128SignBit) {
        uint8_t code = uncheckedReadFixedU8();
        return RefType::fromTypeCode(TypeCode(code), nullable);
      }

      int32_t x = uncheckedReadVarS32();
      const TypeDef* typeDef = &types.type(x);
      return RefType::fromTypeDef(typeDef, nullable);
    }
    default:
      return ValType::fromNonRefTypeCode(TypeCode(code));
  }
}

template <class T>
inline bool Decoder::readPackedType(const TypeContext& types,
                                    const FeatureArgs& features, T* type) {
  static_assert(uint8_t(TypeCode::Limit) <= UINT8_MAX, "fits");
  uint8_t code;
  if (!readFixedU8(&code)) {
    return fail("expected type code");
  }
  switch (code) {
    case uint8_t(TypeCode::V128): {
#ifdef ENABLE_WASM_SIMD
      if (!features.simd) {
        return fail("v128 not enabled");
      }
      *type = T::fromNonRefTypeCode(TypeCode(code));
      return true;
#else
      break;
#endif
    }
    case uint8_t(TypeCode::FuncRef):
    case uint8_t(TypeCode::ExternRef): {
      *type = RefType::fromTypeCode(TypeCode(code), true);
      return true;
    }
    case uint8_t(TypeCode::ExnRef):
    case uint8_t(TypeCode::NullExnRef): {
      if (!features.exnref) {
        return fail("exnref not enabled");
      }
      *type = RefType::fromTypeCode(TypeCode(code), true);
      return true;
    }
    case uint8_t(TypeCode::Ref):
    case uint8_t(TypeCode::NullableRef): {
#ifdef ENABLE_WASM_GC
      if (!features.gc) {
        return fail("gc not enabled");
      }
      bool nullable = code == uint8_t(TypeCode::NullableRef);
      RefType refType;
      if (!readHeapType(types, features, nullable, &refType)) {
        return false;
      }
      *type = refType;
      return true;
#else
      break;
#endif
    }
    case uint8_t(TypeCode::AnyRef):
    case uint8_t(TypeCode::I31Ref):
    case uint8_t(TypeCode::EqRef):
    case uint8_t(TypeCode::StructRef):
    case uint8_t(TypeCode::ArrayRef):
    case uint8_t(TypeCode::NullFuncRef):
    case uint8_t(TypeCode::NullExternRef):
    case uint8_t(TypeCode::NullAnyRef): {
#ifdef ENABLE_WASM_GC
      if (!features.gc) {
        return fail("gc not enabled");
      }
      *type = RefType::fromTypeCode(TypeCode(code), true);
      return true;
#else
      break;
#endif
    }
    default: {
      if (!T::isValidTypeCode(TypeCode(code))) {
        break;
      }
      *type = T::fromNonRefTypeCode(TypeCode(code));
      return true;
    }
  }
  return fail("bad type");
}

inline bool Decoder::readValType(const TypeContext& types,
                                 const FeatureArgs& features, ValType* type) {
  return readPackedType<ValType>(types, features, type);
}

inline bool Decoder::readStorageType(const TypeContext& types,
                                     const FeatureArgs& features,
                                     StorageType* type) {
  return readPackedType<StorageType>(types, features, type);
}

inline bool Decoder::readHeapType(const TypeContext& types,
                                  const FeatureArgs& features, bool nullable,
                                  RefType* type) {
  uint8_t nextByte;
  if (!peekByte(&nextByte)) {
    return fail("expected heap type code");
  }

  if ((nextByte & SLEB128SignMask) == SLEB128SignBit) {
    uint8_t code;
    if (!readFixedU8(&code)) {
      return false;
    }

    switch (code) {
      case uint8_t(TypeCode::FuncRef):
      case uint8_t(TypeCode::ExternRef):
        *type = RefType::fromTypeCode(TypeCode(code), nullable);
        return true;
      case uint8_t(TypeCode::ExnRef):
      case uint8_t(TypeCode::NullExnRef): {
        if (!features.exnref) {
          return fail("exnref not enabled");
        }
        *type = RefType::fromTypeCode(TypeCode(code), nullable);
        return true;
      }
#ifdef ENABLE_WASM_GC
      case uint8_t(TypeCode::AnyRef):
      case uint8_t(TypeCode::I31Ref):
      case uint8_t(TypeCode::EqRef):
      case uint8_t(TypeCode::StructRef):
      case uint8_t(TypeCode::ArrayRef):
      case uint8_t(TypeCode::NullFuncRef):
      case uint8_t(TypeCode::NullExternRef):
      case uint8_t(TypeCode::NullAnyRef):
        if (!features.gc) {
          return fail("gc not enabled");
        }
        *type = RefType::fromTypeCode(TypeCode(code), nullable);
        return true;
#endif
      default:
        return fail("invalid heap type");
    }
  }

#ifdef ENABLE_WASM_GC
  if (features.gc) {
    int32_t x;
    if (!readVarS32(&x) || x < 0 || uint32_t(x) >= types.length()) {
      return fail("invalid heap type index");
    }
    const TypeDef* typeDef = &types.type(x);
    *type = RefType::fromTypeDef(typeDef, nullable);
    return true;
  }
#endif
  return fail("invalid heap type");
}

inline bool Decoder::readRefType(const TypeContext& types,
                                 const FeatureArgs& features, RefType* type) {
  ValType valType;
  if (!readValType(types, features, &valType)) {
    return false;
  }
  if (!valType.isRefType()) {
    return fail("bad type");
  }
  *type = valType.refType();
  return true;
}

// Instruction opcode

inline bool Decoder::readOp(OpBytes* op) {
  static_assert(size_t(Op::Limit) == 256, "fits");
  uint8_t u8;
  if (!readFixedU8(&u8)) {
    return false;
  }
  op->b0 = u8;
  if (MOZ_LIKELY(!IsPrefixByte(u8))) {
    return true;
  }
  return readVarU32(&op->b1);
}

// Instruction immediates for constant instructions

inline bool Decoder::readTypeIndex(uint32_t* typeIndex) {
  if (!readVarU32(typeIndex)) {
    return fail("unable to read type index");
  }
  return true;
}

inline bool Decoder::readGlobalIndex(uint32_t* globalIndex) {
  if (!readVarU32(globalIndex)) {
    return fail("unable to read global index");
  }
  return true;
}

inline bool Decoder::readFuncIndex(uint32_t* funcIndex) {
  if (!readVarU32(funcIndex)) {
    return fail("unable to read function index");
  }
  return true;
}

inline bool Decoder::readI32Const(int32_t* i32) {
  if (!readVarS32(i32)) {
    return fail("failed to read I32 constant");
  }
  return true;
}

inline bool Decoder::readI64Const(int64_t* i64) {
  if (!readVarS64(i64)) {
    return fail("failed to read I64 constant");
  }
  return true;
}

inline bool Decoder::readF32Const(float* f32) {
  if (!readFixedF32(f32)) {
    return fail("failed to read F32 constant");
  }
  return true;
}

inline bool Decoder::readF64Const(double* f64) {
  if (!readFixedF64(f64)) {
    return fail("failed to read F64 constant");
  }
  return true;
}

#ifdef ENABLE_WASM_SIMD
inline bool Decoder::readV128Const(V128* value) {
  if (!readFixedV128(value)) {
    return fail("unable to read V128 constant");
  }
  return true;
}
#endif

inline bool Decoder::readRefNull(const TypeContext& types,
                                 const FeatureArgs& features, RefType* type) {
  return readHeapType(types, features, true, type);
}

}  // namespace wasm
}  // namespace js

#endif  // namespace wasm_binary_h

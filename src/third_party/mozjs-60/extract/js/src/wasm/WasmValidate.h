/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
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

#ifndef wasm_validate_h
#define wasm_validate_h

#include "mozilla/TypeTraits.h"

#include "wasm/WasmCode.h"
#include "wasm/WasmTypes.h"

namespace js {
namespace wasm {

// This struct captures the bytecode offset of a section's payload (so not
// including the header) and the size of the payload.

struct SectionRange
{
    uint32_t start;
    uint32_t size;

    uint32_t end() const {
        return start + size;
    }
    bool operator==(const SectionRange& rhs) const {
        return start == rhs.start && size == rhs.size;
    }
};

typedef Maybe<SectionRange> MaybeSectionRange;

// ModuleEnvironment contains all the state necessary to validate, process or
// render functions. It is created by decoding all the sections before the wasm
// code section and then used immutably during. When compiling a module using a
// ModuleGenerator, the ModuleEnvironment holds state shared between the
// ModuleGenerator thread and background compile threads. All the threads
// are given a read-only view of the ModuleEnvironment, thus preventing race
// conditions.

struct ModuleEnvironment
{
    // Constant parameters for the entire compilation:
    const DebugEnabled        debug;
    const ModuleKind          kind;
    const CompileMode         mode;
    const Shareable           sharedMemoryEnabled;
    const Tier                tier;

    // Module fields decoded from the module environment (or initialized while
    // validating an asm.js module) and immutable during compilation:
    MemoryUsage               memoryUsage;
    uint32_t                  minMemoryLength;
    Maybe<uint32_t>           maxMemoryLength;
    SigWithIdVector           sigs;
    SigWithIdPtrVector        funcSigs;
    Uint32Vector              funcImportGlobalDataOffsets;
    GlobalDescVector          globals;
    TableDescVector           tables;
    Uint32Vector              asmJSSigToTableIndex;
    ImportVector              imports;
    ExportVector              exports;
    Maybe<uint32_t>           startFuncIndex;
    MaybeSectionRange         codeSection;

    // Fields decoded as part of the wasm module tail:
    ElemSegmentVector         elemSegments;
    DataSegmentVector         dataSegments;
    NameInBytecodeVector      funcNames;
    CustomSectionVector       customSections;

    explicit ModuleEnvironment(CompileMode mode = CompileMode::Once,
                               Tier tier = Tier::Ion,
                               DebugEnabled debug = DebugEnabled::False,
                               Shareable sharedMemoryEnabled = Shareable::False,
                               ModuleKind kind = ModuleKind::Wasm)
      : debug(debug),
        kind(kind),
        mode(mode),
        sharedMemoryEnabled(sharedMemoryEnabled),
        tier(tier),
        memoryUsage(MemoryUsage::None),
        minMemoryLength(0)
    {}

    size_t numTables() const {
        return tables.length();
    }
    size_t numSigs() const {
        return sigs.length();
    }
    size_t numFuncs() const {
        return funcSigs.length();
    }
    size_t numFuncImports() const {
        return funcImportGlobalDataOffsets.length();
    }
    size_t numFuncDefs() const {
        return funcSigs.length() - funcImportGlobalDataOffsets.length();
    }
    bool usesMemory() const {
        return memoryUsage != MemoryUsage::None;
    }
    bool usesSharedMemory() const {
        return memoryUsage == MemoryUsage::Shared;
    }
    bool isAsmJS() const {
        return kind == ModuleKind::AsmJS;
    }
    bool debugEnabled() const {
        return debug == DebugEnabled::True;
    }
    bool funcIsImport(uint32_t funcIndex) const {
        return funcIndex < funcImportGlobalDataOffsets.length();
    }
    uint32_t funcIndexToSigIndex(uint32_t funcIndex) const {
        return funcSigs[funcIndex] - sigs.begin();
    }
};

// The Encoder class appends bytes to the Bytes object it is given during
// construction. The client is responsible for the Bytes's lifetime and must
// keep the Bytes alive as long as the Encoder is used.

class Encoder
{
    Bytes& bytes_;

    template <class T>
    MOZ_MUST_USE bool write(const T& v) {
        return bytes_.append(reinterpret_cast<const uint8_t*>(&v), sizeof(T));
    }

    template <typename UInt>
    MOZ_MUST_USE bool writeVarU(UInt i) {
        do {
            uint8_t byte = i & 0x7f;
            i >>= 7;
            if (i != 0)
                byte |= 0x80;
            if (!bytes_.append(byte))
                return false;
        } while (i != 0);
        return true;
    }

    template <typename SInt>
    MOZ_MUST_USE bool writeVarS(SInt i) {
        bool done;
        do {
            uint8_t byte = i & 0x7f;
            i >>= 7;
            done = ((i == 0) && !(byte & 0x40)) || ((i == -1) && (byte & 0x40));
            if (!done)
                byte |= 0x80;
            if (!bytes_.append(byte))
                return false;
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
            bytes_[offset] = patchByte;
            offset++;
        } while(assertBits != 0);
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
        while (bytes_[offset] & 0x80)
            offset++;
        return offset - start + 1;
    }

  public:
    explicit Encoder(Bytes& bytes)
      : bytes_(bytes)
    {
        MOZ_ASSERT(empty());
    }

    size_t currentOffset() const { return bytes_.length(); }
    bool empty() const { return currentOffset() == 0; }

    // Fixed-size encoding operations simply copy the literal bytes (without
    // attempting to align).

    MOZ_MUST_USE bool writeFixedU7(uint8_t i) {
        MOZ_ASSERT(i <= uint8_t(INT8_MAX));
        return writeFixedU8(i);
    }
    MOZ_MUST_USE bool writeFixedU8(uint8_t i) {
        return write<uint8_t>(i);
    }
    MOZ_MUST_USE bool writeFixedU32(uint32_t i) {
        return write<uint32_t>(i);
    }
    MOZ_MUST_USE bool writeFixedF32(float f) {
        return write<float>(f);
    }
    MOZ_MUST_USE bool writeFixedF64(double d) {
        return write<double>(d);
    }
    MOZ_MUST_USE bool writeFixedI8x16(const I8x16& i8x16) {
        return write<I8x16>(i8x16);
    }
    MOZ_MUST_USE bool writeFixedI16x8(const I16x8& i16x8) {
        return write<I16x8>(i16x8);
    }
    MOZ_MUST_USE bool writeFixedI32x4(const I32x4& i32x4) {
        return write<I32x4>(i32x4);
    }
    MOZ_MUST_USE bool writeFixedF32x4(const F32x4& f32x4) {
        return write<F32x4>(f32x4);
    }

    // Variable-length encodings that all use LEB128.

    MOZ_MUST_USE bool writeVarU32(uint32_t i) {
        return writeVarU<uint32_t>(i);
    }
    MOZ_MUST_USE bool writeVarS32(int32_t i) {
        return writeVarS<int32_t>(i);
    }
    MOZ_MUST_USE bool writeVarU64(uint64_t i) {
        return writeVarU<uint64_t>(i);
    }
    MOZ_MUST_USE bool writeVarS64(int64_t i) {
        return writeVarS<int64_t>(i);
    }
    MOZ_MUST_USE bool writeValType(ValType type) {
        static_assert(size_t(TypeCode::Limit) <= UINT8_MAX, "fits");
        MOZ_ASSERT(size_t(type) < size_t(TypeCode::Limit));
        return writeFixedU8(uint8_t(type));
    }
    MOZ_MUST_USE bool writeBlockType(ExprType type) {
        static_assert(size_t(TypeCode::Limit) <= UINT8_MAX, "fits");
        MOZ_ASSERT(size_t(type) < size_t(TypeCode::Limit));
        return writeFixedU8(uint8_t(type));
    }
    MOZ_MUST_USE bool writeOp(Op op) {
        static_assert(size_t(Op::Limit) == 256, "fits");
        MOZ_ASSERT(size_t(op) < size_t(Op::Limit));
        return writeFixedU8(uint8_t(op));
    }
    MOZ_MUST_USE bool writeOp(MozOp op) {
        static_assert(size_t(MozOp::Limit) <= 256, "fits");
        MOZ_ASSERT(size_t(op) < size_t(MozOp::Limit));
        return writeFixedU8(uint8_t(Op::MozPrefix)) &&
               writeFixedU8(uint8_t(op));
    }
    MOZ_MUST_USE bool writeOp(NumericOp op) {
        static_assert(size_t(NumericOp::Limit) <= 256, "fits");
        MOZ_ASSERT(size_t(op) < size_t(NumericOp::Limit));
        return writeFixedU8(uint8_t(Op::NumericPrefix)) &&
               writeFixedU8(uint8_t(op));
    }
    MOZ_MUST_USE bool writeOp(ThreadOp op) {
        static_assert(size_t(ThreadOp::Limit) <= 256, "fits");
        MOZ_ASSERT(size_t(op) < size_t(ThreadOp::Limit));
        return writeFixedU8(uint8_t(Op::ThreadPrefix)) &&
               writeFixedU8(uint8_t(op));
    }

    // Fixed-length encodings that allow back-patching.

    MOZ_MUST_USE bool writePatchableFixedU7(size_t* offset) {
        *offset = bytes_.length();
        return writeFixedU8(UINT8_MAX);
    }
    void patchFixedU7(size_t offset, uint8_t patchBits) {
        return patchFixedU7(offset, patchBits, UINT8_MAX);
    }

    // Variable-length encodings that allow back-patching.

    MOZ_MUST_USE bool writePatchableVarU32(size_t* offset) {
        *offset = bytes_.length();
        return writeVarU32(UINT32_MAX);
    }
    void patchVarU32(size_t offset, uint32_t patchBits) {
        return patchVarU32(offset, patchBits, UINT32_MAX);
    }

    // Byte ranges start with an LEB128 length followed by an arbitrary sequence
    // of bytes. When used for strings, bytes are to be interpreted as utf8.

    MOZ_MUST_USE bool writeBytes(const void* bytes, uint32_t numBytes) {
        return writeVarU32(numBytes) &&
               bytes_.append(reinterpret_cast<const uint8_t*>(bytes), numBytes);
    }

    // A "section" is a contiguous range of bytes that stores its own size so
    // that it may be trivially skipped without examining the contents. Sections
    // require backpatching since the size of the section is only known at the
    // end while the size's varU32 must be stored at the beginning. Immediately
    // after the section length is the string id of the section.

    MOZ_MUST_USE bool startSection(SectionId id, size_t* offset) {
        return writeVarU32(uint32_t(id)) &&
               writePatchableVarU32(offset);
    }
    void finishSection(size_t offset) {
        return patchVarU32(offset, bytes_.length() - offset - varU32ByteLength(offset));
    }
};

// The Decoder class decodes the bytes in the range it is given during
// construction. The client is responsible for keeping the byte range alive as
// long as the Decoder is used.

class Decoder
{
    const uint8_t* const beg_;
    const uint8_t* const end_;
    const uint8_t* cur_;
    const size_t offsetInModule_;
    UniqueChars* error_;
    bool resilientMode_;

    template <class T>
    MOZ_MUST_USE bool read(T* out) {
        if (bytesRemain() < sizeof(T))
            return false;
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
    MOZ_MUST_USE bool readVarU(UInt* out) {
        DebugOnly<const uint8_t*> before = cur_;
        const unsigned numBits = sizeof(UInt) * CHAR_BIT;
        const unsigned remainderBits = numBits % 7;
        const unsigned numBitsInSevens = numBits - remainderBits;
        UInt u = 0;
        uint8_t byte;
        UInt shift = 0;
        do {
            if (!readFixedU8(&byte))
                return false;
            if (!(byte & 0x80)) {
                *out = u | UInt(byte) << shift;
                return true;
            }
            u |= UInt(byte & 0x7F) << shift;
            shift += 7;
        } while (shift != numBitsInSevens);
        if (!readFixedU8(&byte) || (byte & (unsigned(-1) << remainderBits)))
            return false;
        *out = u | (UInt(byte) << numBitsInSevens);
        MOZ_ASSERT_IF(sizeof(UInt) == 4, unsigned(cur_ - before) <= MaxVarU32DecodedBytes);
        return true;
    }

    template <typename SInt>
    MOZ_MUST_USE bool readVarS(SInt* out) {
        using UInt = typename mozilla::MakeUnsigned<SInt>::Type;
        const unsigned numBits = sizeof(SInt) * CHAR_BIT;
        const unsigned remainderBits = numBits % 7;
        const unsigned numBitsInSevens = numBits - remainderBits;
        SInt s = 0;
        uint8_t byte;
        unsigned shift = 0;
        do {
            if (!readFixedU8(&byte))
                return false;
            s |= SInt(byte & 0x7f) << shift;
            shift += 7;
            if (!(byte & 0x80)) {
                if (byte & 0x40)
                    s |= UInt(-1) << shift;
                *out = s;
                return true;
            }
        } while (shift < numBitsInSevens);
        if (!remainderBits || !readFixedU8(&byte) || (byte & 0x80))
            return false;
        uint8_t mask = 0x7f & (uint8_t(-1) << remainderBits);
        if ((byte & mask) != ((byte & (1 << (remainderBits - 1))) ? mask : 0))
            return false;
        *out = s | UInt(byte) << shift;
        return true;
    }

  public:
    Decoder(const uint8_t* begin, const uint8_t* end, size_t offsetInModule, UniqueChars* error,
            bool resilientMode = false)
      : beg_(begin),
        end_(end),
        cur_(begin),
        offsetInModule_(offsetInModule),
        error_(error),
        resilientMode_(resilientMode)
    {
        MOZ_ASSERT(begin <= end);
    }
    explicit Decoder(const Bytes& bytes, size_t offsetInModule = 0, UniqueChars* error = nullptr)
      : beg_(bytes.begin()),
        end_(bytes.end()),
        cur_(bytes.begin()),
        offsetInModule_(offsetInModule),
        error_(error),
        resilientMode_(false)
    {}

    // These convenience functions use currentOffset() as the errorOffset.
    bool fail(const char* msg) { return fail(currentOffset(), msg); }
    bool failf(const char* msg, ...) MOZ_FORMAT_PRINTF(2, 3);

    // Report an error at the given offset (relative to the whole module).
    bool fail(size_t errorOffset, const char* msg);

    void clearError() {
        if (error_)
            error_->reset();
    }

    bool done() const {
        MOZ_ASSERT(cur_ <= end_);
        return cur_ == end_;
    }
    bool resilientMode() const {
        return resilientMode_;
    }

    size_t bytesRemain() const {
        MOZ_ASSERT(end_ >= cur_);
        return size_t(end_ - cur_);
    }
    // pos must be a value previously returned from currentPosition.
    void rollbackPosition(const uint8_t* pos) {
        cur_ = pos;
    }
    const uint8_t* currentPosition() const {
        return cur_;
    }
    size_t currentOffset() const {
        return offsetInModule_ + (cur_ - beg_);
    }
    const uint8_t* begin() const {
        return beg_;
    }
    const uint8_t* end() const {
        return end_;
    }

    // Fixed-size encoding operations simply copy the literal bytes (without
    // attempting to align).

    MOZ_MUST_USE bool readFixedU8(uint8_t* i) {
        return read<uint8_t>(i);
    }
    MOZ_MUST_USE bool readFixedU32(uint32_t* u) {
        return read<uint32_t>(u);
    }
    MOZ_MUST_USE bool readFixedF32(float* f) {
        return read<float>(f);
    }
    MOZ_MUST_USE bool readFixedF64(double* d) {
        return read<double>(d);
    }
    MOZ_MUST_USE bool readFixedI8x16(I8x16* i8x16) {
        return read<I8x16>(i8x16);
    }
    MOZ_MUST_USE bool readFixedI16x8(I16x8* i16x8) {
        return read<I16x8>(i16x8);
    }
    MOZ_MUST_USE bool readFixedI32x4(I32x4* i32x4) {
        return read<I32x4>(i32x4);
    }
    MOZ_MUST_USE bool readFixedF32x4(F32x4* f32x4) {
        return read<F32x4>(f32x4);
    }

    // Variable-length encodings that all use LEB128.

    MOZ_MUST_USE bool readVarU32(uint32_t* out) {
        return readVarU<uint32_t>(out);
    }
    MOZ_MUST_USE bool readVarS32(int32_t* out) {
        return readVarS<int32_t>(out);
    }
    MOZ_MUST_USE bool readVarU64(uint64_t* out) {
        return readVarU<uint64_t>(out);
    }
    MOZ_MUST_USE bool readVarS64(int64_t* out) {
        return readVarS<int64_t>(out);
    }
    MOZ_MUST_USE bool readValType(uint8_t* type) {
        static_assert(uint8_t(TypeCode::Limit) <= UINT8_MAX, "fits");
        return readFixedU8(type);
    }
    MOZ_MUST_USE bool readBlockType(uint8_t* type) {
        static_assert(size_t(TypeCode::Limit) <= UINT8_MAX, "fits");
        return readFixedU8(type);
    }
    MOZ_MUST_USE bool readOp(OpBytes* op) {
        static_assert(size_t(Op::Limit) == 256, "fits");
        uint8_t u8;
        if (!readFixedU8(&u8))
            return false;
        op->b0 = u8;
        if (MOZ_LIKELY(!IsPrefixByte(u8)))
            return true;
        if (!readFixedU8(&u8)) {
            op->b1 = 0;         // Make it sane
            return false;
        }
        op->b1 = u8;
        return true;
    }

    // See writeBytes comment.

    MOZ_MUST_USE bool readBytes(uint32_t numBytes, const uint8_t** bytes = nullptr) {
        if (bytes)
            *bytes = cur_;
        if (bytesRemain() < numBytes)
            return false;
        cur_ += numBytes;
        return true;
    }

    // See "section" description in Encoder.

    MOZ_MUST_USE bool readSectionHeader(uint8_t* id, SectionRange* range);

    MOZ_MUST_USE bool startSection(SectionId id,
                                   ModuleEnvironment* env,
                                   MaybeSectionRange* range,
                                   const char* sectionName);
    MOZ_MUST_USE bool finishSection(const SectionRange& range,
                                    const char* sectionName);

    // Custom sections do not cause validation errors unless the error is in
    // the section header itself.

    MOZ_MUST_USE bool startCustomSection(const char* expected,
                                         size_t expectedLength,
                                         ModuleEnvironment* env,
                                         MaybeSectionRange* range);
    template <size_t NameSizeWith0>
    MOZ_MUST_USE bool startCustomSection(const char (&name)[NameSizeWith0],
                                         ModuleEnvironment* env,
                                         MaybeSectionRange* range)
    {
        MOZ_ASSERT(name[NameSizeWith0 - 1] == '\0');
        return startCustomSection(name, NameSizeWith0 - 1, env, range);
    }
    void finishCustomSection(const SectionRange& range);
    MOZ_MUST_USE bool skipCustomSection(ModuleEnvironment* env);

    // The Name section has its own optional subsections.

    MOZ_MUST_USE bool startNameSubsection(NameType nameType, Maybe<uint32_t>* endOffset);
    MOZ_MUST_USE bool finishNameSubsection(uint32_t endOffset);

    // The infallible "unchecked" decoding functions can be used when we are
    // sure that the bytes are well-formed (by construction or due to previous
    // validation).

    uint8_t uncheckedReadFixedU8() {
        return uncheckedRead<uint8_t>();
    }
    uint32_t uncheckedReadFixedU32() {
        return uncheckedRead<uint32_t>();
    }
    void uncheckedReadFixedF32(float* out) {
        uncheckedRead<float>(out);
    }
    void uncheckedReadFixedF64(double* out) {
        uncheckedRead<double>(out);
    }
    template <typename UInt>
    UInt uncheckedReadVarU() {
        static const unsigned numBits = sizeof(UInt) * CHAR_BIT;
        static const unsigned remainderBits = numBits % 7;
        static const unsigned numBitsInSevens = numBits - remainderBits;
        UInt decoded = 0;
        uint32_t shift = 0;
        do {
            uint8_t byte = *cur_++;
            if (!(byte & 0x80))
                return decoded | (UInt(byte) << shift);
            decoded |= UInt(byte & 0x7f) << shift;
            shift += 7;
        } while (shift != numBitsInSevens);
        uint8_t byte = *cur_++;
        MOZ_ASSERT(!(byte & 0xf0));
        return decoded | (UInt(byte) << numBitsInSevens);
    }
    uint32_t uncheckedReadVarU32() {
        return uncheckedReadVarU<uint32_t>();
    }
    int32_t uncheckedReadVarS32() {
        int32_t i32 = 0;
        MOZ_ALWAYS_TRUE(readVarS32(&i32));
        return i32;
    }
    uint64_t uncheckedReadVarU64() {
        return uncheckedReadVarU<uint64_t>();
    }
    int64_t uncheckedReadVarS64() {
        int64_t i64 = 0;
        MOZ_ALWAYS_TRUE(readVarS64(&i64));
        return i64;
    }
    ValType uncheckedReadValType() {
        return (ValType)uncheckedReadFixedU8();
    }
    Op uncheckedReadOp() {
        static_assert(size_t(Op::Limit) == 256, "fits");
        uint8_t u8 = uncheckedReadFixedU8();
        return u8 != UINT8_MAX
               ? Op(u8)
               : Op(uncheckedReadFixedU8() + UINT8_MAX);
    }
    void uncheckedReadFixedI8x16(I8x16* i8x16) {
        struct T { I8x16 v; };
        T t = uncheckedRead<T>();
        memcpy(i8x16, &t, sizeof(t));
    }
    void uncheckedReadFixedI16x8(I16x8* i16x8) {
        struct T { I16x8 v; };
        T t = uncheckedRead<T>();
        memcpy(i16x8, &t, sizeof(t));
    }
    void uncheckedReadFixedI32x4(I32x4* i32x4) {
        struct T { I32x4 v; };
        T t = uncheckedRead<T>();
        memcpy(i32x4, &t, sizeof(t));
    }
    void uncheckedReadFixedF32x4(F32x4* f32x4) {
        struct T { F32x4 v; };
        T t = uncheckedRead<T>();
        memcpy(f32x4, &t, sizeof(t));
    }
};

// The local entries are part of function bodies and thus serialized by both
// wasm and asm.js and decoded as part of both validation and compilation.

MOZ_MUST_USE bool
EncodeLocalEntries(Encoder& d, const ValTypeVector& locals);

MOZ_MUST_USE bool
DecodeLocalEntries(Decoder& d, ModuleKind kind, ValTypeVector* locals);

// Returns whether the given [begin, end) prefix of a module's bytecode starts a
// code section and, if so, returns the SectionRange of that code section.
// Note that, even if this function returns 'false', [begin, end) may actually
// be a valid module in the special case when there are no function defs and the
// code section is not present. Such modules can be valid so the caller must
// handle this special case.

MOZ_MUST_USE bool
StartsCodeSection(const uint8_t* begin, const uint8_t* end, SectionRange* range);

// Calling DecodeModuleEnvironment decodes all sections up to the code section
// and performs full validation of all those sections. The client must then
// decode the code section itself, reusing ValidateFunctionBody if necessary,
// and finally call DecodeModuleTail to decode all remaining sections after the
// code section (again, performing full validation).

MOZ_MUST_USE bool
DecodeModuleEnvironment(Decoder& d, ModuleEnvironment* env);

MOZ_MUST_USE bool
ValidateFunctionBody(const ModuleEnvironment& env, uint32_t funcIndex, uint32_t bodySize,
                     Decoder& d);

MOZ_MUST_USE bool
DecodeModuleTail(Decoder& d, ModuleEnvironment* env);

// Validate an entire module, returning true if the module was validated
// successfully. If Validate returns false:
//  - if *error is null, the caller should report out-of-memory
//  - otherwise, there was a legitimate error described by *error

MOZ_MUST_USE bool
Validate(JSContext* cx, const ShareableBytes& bytecode, UniqueChars* error);

}  // namespace wasm
}  // namespace js

#endif // namespace wasm_validate_h

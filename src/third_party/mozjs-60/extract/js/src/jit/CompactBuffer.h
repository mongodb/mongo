/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_Compactbuffer_h
#define jit_Compactbuffer_h

#include "jit/IonTypes.h"
#include "js/AllocPolicy.h"
#include "js/Vector.h"

namespace js {
namespace jit {

class CompactBufferWriter;

// CompactBuffers are byte streams designed for compressable integers. It has
// helper functions for writing bytes, fixed-size integers, and variable-sized
// integers. Variable sized integers are encoded in 1-5 bytes, each byte
// containing 7 bits of the integer and a bit which specifies whether the next
// byte is also part of the integer.
//
// Fixed-width integers are also available, in case the actual value will not
// be known until later.

class CompactBufferReader
{
    const uint8_t* buffer_;
    const uint8_t* end_;

    uint32_t readVariableLength() {
        uint32_t val = 0;
        uint32_t shift = 0;
        uint8_t byte;
        while (true) {
            MOZ_ASSERT(shift < 32);
            byte = readByte();
            val |= (uint32_t(byte) >> 1) << shift;
            shift += 7;
            if (!(byte & 1))
                return val;
        }
    }

  public:
    CompactBufferReader(const uint8_t* start, const uint8_t* end)
      : buffer_(start),
        end_(end)
    { }
    inline explicit CompactBufferReader(const CompactBufferWriter& writer);
    uint8_t readByte() {
        MOZ_ASSERT(buffer_ < end_);
        return *buffer_++;
    }
    uint32_t readFixedUint32_t() {
        uint32_t b0 = readByte();
        uint32_t b1 = readByte();
        uint32_t b2 = readByte();
        uint32_t b3 = readByte();
        return b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
    }
    uint16_t readFixedUint16_t() {
        uint32_t b0 = readByte();
        uint32_t b1 = readByte();
        return b0 | (b1 << 8);
    }
    uint32_t readNativeEndianUint32_t() {
        // Must be at 4-byte boundary
        MOZ_ASSERT(uintptr_t(buffer_) % sizeof(uint32_t) == 0);
        return *reinterpret_cast<const uint32_t*>(buffer_);
    }
    uint32_t readUnsigned() {
        return readVariableLength();
    }
    int32_t readSigned() {
        uint8_t b = readByte();
        bool isNegative = !!(b & (1 << 0));
        bool more = !!(b & (1 << 1));
        int32_t result = b >> 2;
        if (more)
            result |= readUnsigned() << 6;
        if (isNegative)
            return -result;
        return result;
    }

    void* readRawPointer() {
        uintptr_t ptrWord = 0;
        for (unsigned i = 0; i < sizeof(uintptr_t); i++) {
            ptrWord |= static_cast<uintptr_t>(readByte()) << (i*8);
        }
        return reinterpret_cast<void*>(ptrWord);
    }

    bool more() const {
        MOZ_ASSERT(buffer_ <= end_);
        return buffer_ < end_;
    }

    void seek(const uint8_t* start, uint32_t offset) {
        buffer_ = start + offset;
        MOZ_ASSERT(start < end_);
        MOZ_ASSERT(buffer_ < end_);
    }

    const uint8_t* currentPosition() const {
        return buffer_;
    }
};

class CompactBufferWriter
{
    js::Vector<uint8_t, 32, SystemAllocPolicy> buffer_;
    bool enoughMemory_;

  public:
    CompactBufferWriter()
      : enoughMemory_(true)
    { }

    void setOOM() {
        enoughMemory_ = false;
    }

    // Note: writeByte() takes uint32 to catch implicit casts with a runtime
    // assert.
    void writeByte(uint32_t byte) {
        MOZ_ASSERT(byte <= 0xFF);
        enoughMemory_ &= buffer_.append(byte);
    }
    void writeByteAt(uint32_t pos, uint32_t byte) {
        MOZ_ASSERT(byte <= 0xFF);
        if (!oom())
            buffer_[pos] = byte;
    }
    void writeUnsigned(uint32_t value) {
        do {
            uint8_t byte = ((value & 0x7F) << 1) | (value > 0x7F);
            writeByte(byte);
            value >>= 7;
        } while (value);
    }
    void writeUnsignedAt(uint32_t pos, uint32_t value, uint32_t original) {
        MOZ_ASSERT(value <= original);
        do {
            uint8_t byte = ((value & 0x7F) << 1) | (original > 0x7F);
            writeByteAt(pos++, byte);
            value >>= 7;
            original >>= 7;
        } while (original);
    }
    void writeSigned(int32_t v) {
        bool isNegative = v < 0;
        uint32_t value = isNegative ? -v : v;
        uint8_t byte = ((value & 0x3F) << 2) | ((value > 0x3F) << 1) | uint32_t(isNegative);
        writeByte(byte);

        // Write out the rest of the bytes, if needed.
        value >>= 6;
        if (value == 0)
            return;
        writeUnsigned(value);
    }
    void writeFixedUint32_t(uint32_t value) {
        writeByte(value & 0xFF);
        writeByte((value >> 8) & 0xFF);
        writeByte((value >> 16) & 0xFF);
        writeByte((value >> 24) & 0xFF);
    }
    void writeFixedUint16_t(uint16_t value) {
        writeByte(value & 0xFF);
        writeByte(value >> 8);
    }
    void writeNativeEndianUint32_t(uint32_t value) {
        // Must be at 4-byte boundary
        MOZ_ASSERT_IF(!oom(), length() % sizeof(uint32_t) == 0);
        writeFixedUint32_t(0);
        if (oom())
            return;
        uint8_t* endPtr = buffer() + length();
        reinterpret_cast<uint32_t*>(endPtr)[-1] = value;
    }
    void writeRawPointer(void* ptr) {
        uintptr_t ptrWord = reinterpret_cast<uintptr_t>(ptr);
        for (unsigned i = 0; i < sizeof(uintptr_t); i++) {
            writeByte((ptrWord >> (i*8)) & 0xFF);
        }
    }
    size_t length() const {
        return buffer_.length();
    }
    uint8_t* buffer() {
        MOZ_ASSERT(!oom());
        return &buffer_[0];
    }
    const uint8_t* buffer() const {
        MOZ_ASSERT(!oom());
        return &buffer_[0];
    }
    bool oom() const {
        return !enoughMemory_;
    }
    void propagateOOM(bool success) {
        enoughMemory_ &= success;
    }
};

CompactBufferReader::CompactBufferReader(const CompactBufferWriter& writer)
  : buffer_(writer.buffer()),
    end_(writer.buffer() + writer.length())
{
}

} // namespace jit
} // namespace js

#endif /* jit_Compactbuffer_h */

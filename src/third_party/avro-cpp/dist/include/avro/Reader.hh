/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef avro_Reader_hh__
#define avro_Reader_hh__

#include <array>
#include <boost/noncopyable.hpp>
#include <cstdint>
#include <vector>

#include "Config.hh"
#include "Types.hh"
#include "Validator.hh"
#include "Zigzag.hh"
#include "buffer/BufferReader.hh"

namespace avro {

///
/// Parses from an avro encoding to the requested type.  Assumes the next item
/// in the avro binary data is the expected type.
///

template<class ValidatorType>
class ReaderImpl : private boost::noncopyable {

public:
    explicit ReaderImpl(const InputBuffer &buffer) : reader_(buffer) {}

    ReaderImpl(const ValidSchema &schema, const InputBuffer &buffer) : validator_(schema),
                                                                       reader_(buffer) {}

    void readValue(Null &) {
        validator_.checkTypeExpected(AVRO_NULL);
    }

    void readValue(bool &val) {
        validator_.checkTypeExpected(AVRO_BOOL);
        uint8_t intVal = 0;
        reader_.read(intVal);
        val = (intVal != 0);
    }

    void readValue(int32_t &val) {
        validator_.checkTypeExpected(AVRO_INT);
        auto encoded = static_cast<uint32_t>(readVarInt());
        val = decodeZigzag32(encoded);
    }

    void readValue(int64_t &val) {
        validator_.checkTypeExpected(AVRO_LONG);
        uint64_t encoded = readVarInt();
        val = decodeZigzag64(encoded);
    }

    void readValue(float &val) {
        validator_.checkTypeExpected(AVRO_FLOAT);
        union {
            float f;
            uint32_t i;
        } v;
        reader_.read(v.i);
        val = v.f;
    }

    void readValue(double &val) {
        validator_.checkTypeExpected(AVRO_DOUBLE);
        union {
            double d;
            uint64_t i;
        } v = {0};
        reader_.read(v.i);
        val = v.d;
    }

    void readValue(std::string &val) {
        validator_.checkTypeExpected(AVRO_STRING);
        auto size = static_cast<size_t>(readSize());
        reader_.read(val, size);
    }

    void readBytes(std::vector<uint8_t> &val) {
        validator_.checkTypeExpected(AVRO_BYTES);
        auto size = static_cast<size_t>(readSize());
        val.resize(size);
        reader_.read(reinterpret_cast<char *>(val.data()), size);
    }

    void readFixed(uint8_t *val, size_t size) {
        validator_.checkFixedSizeExpected(size);
        reader_.read(reinterpret_cast<char *>(val), size);
    }

    template<size_t N>
    void readFixed(uint8_t (&val)[N]) {
        this->readFixed(val, N);
    }

    template<size_t N>
    void readFixed(std::array<uint8_t, N> &val) {
        this->readFixed(val.data(), N);
    }

    void readRecord() {
        validator_.checkTypeExpected(AVRO_RECORD);
        validator_.checkTypeExpected(AVRO_LONG);
        validator_.setCount(1);
    }

    void readRecordEnd() {
        validator_.checkTypeExpected(AVRO_RECORD);
        validator_.checkTypeExpected(AVRO_LONG);
        validator_.setCount(0);
    }

    int64_t readArrayBlockSize() {
        validator_.checkTypeExpected(AVRO_ARRAY);
        return readCount();
    }

    int64_t readUnion() {
        validator_.checkTypeExpected(AVRO_UNION);
        return readCount();
    }

    int64_t readEnum() {
        validator_.checkTypeExpected(AVRO_ENUM);
        return readCount();
    }

    int64_t readMapBlockSize() {
        validator_.checkTypeExpected(AVRO_MAP);
        return readCount();
    }

    Type nextType() const {
        return validator_.nextTypeExpected();
    }

    bool currentRecordName(std::string &name) const {
        return validator_.getCurrentRecordName(name);
    }

    bool nextFieldName(std::string &name) const {
        return validator_.getNextFieldName(name);
    }

private:
    uint64_t readVarInt() {
        uint64_t encoded = 0;
        uint8_t val = 0;
        int shift = 0;
        do {
            reader_.read(val);
            uint64_t newBits = static_cast<uint64_t>(val & 0x7f) << shift;
            encoded |= newBits;
            shift += 7;
        } while (val & 0x80);

        return encoded;
    }

    size_t readSize() {
        uint64_t encoded = readVarInt();
        auto size = static_cast<size_t>(decodeZigzag64(encoded));
        return size;
    }

    size_t readCount() {
        validator_.checkTypeExpected(AVRO_LONG);
        size_t count = readSize();
        validator_.setCount(count);
        return count;
    }

    ValidatorType validator_;
    BufferReader reader_;
};

using Reader = ReaderImpl<NullValidator>;
using ValidatingReader = ReaderImpl<Validator>;

} // namespace avro

#endif

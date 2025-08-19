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

#ifndef avro_Writer_hh__
#define avro_Writer_hh__

#include <array>
#include <boost/noncopyable.hpp>

#include "Config.hh"
#include "Types.hh"
#include "Validator.hh"
#include "Zigzag.hh"
#include "buffer/Buffer.hh"

namespace avro {

/// Class for writing avro data to a stream.

template<class ValidatorType>
class WriterImpl : private boost::noncopyable {

public:
    WriterImpl() = default;

    explicit WriterImpl(const ValidSchema &schema) : validator_(schema) {}

    void writeValue(const Null &) {
        validator_.checkTypeExpected(AVRO_NULL);
    }

    void writeValue(bool val) {
        validator_.checkTypeExpected(AVRO_BOOL);
        int8_t byte = (val != 0);
        buffer_.writeTo(byte);
    }

    void writeValue(int32_t val) {
        validator_.checkTypeExpected(AVRO_INT);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
        std::array<uint8_t, 5> bytes;
        size_t size = encodeInt32(val, bytes);
        buffer_.writeTo(reinterpret_cast<const char *>(bytes.data()), size);
    }

    void writeValue(int64_t val) {
        validator_.checkTypeExpected(AVRO_LONG);
        putLong(val);
    }

    void writeValue(float val) {
        validator_.checkTypeExpected(AVRO_FLOAT);
        union {
            float f;
            int32_t i;
        } v;

        v.f = val;
        buffer_.writeTo(v.i);
    }

    void writeValue(double val) {
        validator_.checkTypeExpected(AVRO_DOUBLE);
        union {
            double d;
            int64_t i;
        } v;

        v.d = val;
        buffer_.writeTo(v.i);
    }

    void writeValue(const std::string &val) {
        validator_.checkTypeExpected(AVRO_STRING);
        putBytes(val.c_str(), val.size());
    }

    void writeBytes(const void *val, size_t size) {
        validator_.checkTypeExpected(AVRO_BYTES);
        putBytes(val, size);
    }

    template<size_t N>
    void writeFixed(const uint8_t (&val)[N]) {
        validator_.checkFixedSizeExpected(N);
        buffer_.writeTo(reinterpret_cast<const char *>(val), N);
    }

    template<size_t N>
    void writeFixed(const std::array<uint8_t, N> &val) {
        validator_.checkFixedSizeExpected(val.size());
        buffer_.writeTo(reinterpret_cast<const char *>(val.data()), val.size());
    }

    void writeRecord() {
        validator_.checkTypeExpected(AVRO_RECORD);
        validator_.checkTypeExpected(AVRO_LONG);
        validator_.setCount(1);
    }

    void writeRecordEnd() {
        validator_.checkTypeExpected(AVRO_RECORD);
        validator_.checkTypeExpected(AVRO_LONG);
        validator_.setCount(0);
    }

    void writeArrayBlock(int64_t size) {
        validator_.checkTypeExpected(AVRO_ARRAY);
        writeCount(size);
    }

    void writeArrayEnd() {
        writeArrayBlock(0);
    }

    void writeMapBlock(int64_t size) {
        validator_.checkTypeExpected(AVRO_MAP);
        writeCount(size);
    }

    void writeMapEnd() {
        writeMapBlock(0);
    }

    void writeUnion(int64_t choice) {
        validator_.checkTypeExpected(AVRO_UNION);
        writeCount(choice);
    }

    void writeEnum(int64_t choice) {
        validator_.checkTypeExpected(AVRO_ENUM);
        writeCount(choice);
    }

    InputBuffer buffer() const {
        return buffer_;
    }

private:
    void putLong(int64_t val) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
        std::array<uint8_t, 10> bytes;
        size_t size = encodeInt64(val, bytes);
        buffer_.writeTo(reinterpret_cast<const char *>(bytes.data()), size);
    }

    void putBytes(const void *val, size_t size) {
        putLong(size);
        buffer_.writeTo(reinterpret_cast<const char *>(val), size);
    }

    void writeCount(int64_t count) {
        validator_.checkTypeExpected(AVRO_LONG);
        validator_.setCount(count);
        putLong(count);
    }

    ValidatorType validator_;
    OutputBuffer buffer_;
};

using Writer = WriterImpl<NullValidator>;
using ValidatingWriter = WriterImpl<Validator>;

} // namespace avro

#endif

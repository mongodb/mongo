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

#ifndef avro_Serializer_hh__
#define avro_Serializer_hh__

#include <array>
#include <boost/noncopyable.hpp>

#include "Config.hh"
#include "Writer.hh"

namespace avro {

/// Class that wraps a Writer or ValidatingWriter with an interface that uses
/// explicit write* names instead of writeValue

template<class Writer>
class Serializer : private boost::noncopyable {

public:
    /// Constructor only works with Writer
    explicit Serializer() : writer_() {}

    /// Constructor only works with ValidatingWriter
    explicit Serializer(const ValidSchema &schema) : writer_(schema) {}

    void writeNull() {
        writer_.writeValue(Null());
    }

    void writeBool(bool val) {
        writer_.writeValue(val);
    }

    void writeInt(int32_t val) {
        writer_.writeValue(val);
    }

    void writeLong(int64_t val) {
        writer_.writeValue(val);
    }

    void writeFloat(float val) {
        writer_.writeValue(val);
    }

    void writeDouble(double val) {
        writer_.writeValue(val);
    }

    void writeBytes(const void *val, size_t size) {
        writer_.writeBytes(val, size);
    }

    template<size_t N>
    void writeFixed(const uint8_t (&val)[N]) {
        writer_.writeFixed(val);
    }

    template<size_t N>
    void writeFixed(const std::array<uint8_t, N> &val) {
        writer_.writeFixed(val);
    }

    void writeString(const std::string &val) {
        writer_.writeValue(val);
    }

    void writeRecord() {
        writer_.writeRecord();
    }

    void writeRecordEnd() {
        writer_.writeRecordEnd();
    }

    void writeArrayBlock(int64_t size) {
        writer_.writeArrayBlock(size);
    }

    void writeArrayEnd() {
        writer_.writeArrayEnd();
    }

    void writeMapBlock(int64_t size) {
        writer_.writeMapBlock(size);
    }

    void writeMapEnd() {
        writer_.writeMapEnd();
    }

    void writeUnion(int64_t choice) {
        writer_.writeUnion(choice);
    }

    void writeEnum(int64_t choice) {
        writer_.writeEnum(choice);
    }

    InputBuffer buffer() const {
        return writer_.buffer();
    }

private:
    Writer writer_;
};

} // namespace avro

#endif

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

#ifndef avro_Encoding_hh__
#define avro_Encoding_hh__

#include <array>
#include <cstddef>
#include <cstdint>

#include "Config.hh"
/// \file
/// Functions for encoding and decoding integers with zigzag compression

namespace avro {

AVRO_DECL constexpr uint64_t encodeZigzag64(int64_t input) noexcept {
    return ((static_cast<uint64_t>(input) << 1) ^ (input >> 63));
}
AVRO_DECL constexpr int64_t decodeZigzag64(uint64_t input) noexcept {
    return static_cast<int64_t>(((input >> 1) ^ -(static_cast<int64_t>(input) & 1)));
}

AVRO_DECL constexpr uint32_t encodeZigzag32(int32_t input) noexcept {
    return (static_cast<uint32_t>(input) << 1) ^ (input >> 31);
}
AVRO_DECL constexpr int32_t decodeZigzag32(uint32_t input) noexcept {
    return static_cast<int32_t>(((input >> 1) ^ -(static_cast<int64_t>(input) & 1)));
}

AVRO_DECL size_t encodeInt32(int32_t input, std::array<uint8_t, 5> &output) noexcept;
AVRO_DECL size_t encodeInt64(int64_t input, std::array<uint8_t, 10> &output) noexcept;

} // namespace avro

#endif

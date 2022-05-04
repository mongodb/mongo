/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <fmt/format.h>
#include <sstream>
#include <string>

#include "mongo/base/string_data.h"

namespace mongo {
namespace base64 {

std::string encode(StringData in);
std::string decode(StringData in);

void encode(std::stringstream& ss, StringData in);
void decode(std::stringstream& ss, StringData in);

void encode(fmt::memory_buffer& buffer, StringData in);
void decode(fmt::memory_buffer& buffer, StringData in);

inline std::string encode(const void* data, size_t len) {
    return encode(StringData(reinterpret_cast<const char*>(data), len));
}

bool validate(StringData s);

/**
 * Calculate how large a given input would expand to.
 * Effectively: ceil(inLen * 4 / 3)
 */
constexpr std::size_t encodedLength(std::size_t inLen) {
    return (inLen + 2) / 3 * 4;
}
}  // namespace base64

// base64url encoding is a "url safe" variant of base64.
// '+' is replaced with '-'
// '/' is replaced with '_'
// '=' at the end of the string are optional
namespace base64url {

std::string encode(StringData in);
std::string decode(StringData out);

void encode(std::stringstream& ss, StringData in);
void decode(std::stringstream& ss, StringData in);

void encode(fmt::memory_buffer& buffer, StringData in);
void decode(fmt::memory_buffer& buffer, StringData in);

inline std::string encode(const void* data, std::size_t len) {
    return encode(StringData(reinterpret_cast<const char*>(data), len));
}

bool validate(StringData s);

constexpr std::size_t encodedLength(std::size_t inLen) {
    return base64::encodedLength(inLen);
}
}  // namespace base64url
}  // namespace mongo

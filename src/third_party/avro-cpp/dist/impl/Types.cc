/**
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

#include "Types.hh"
#include <iostream>
#include <string>

namespace avro {
namespace strings {
const std::string typeToString[] = {
    "string",
    "bytes",
    "int",
    "long",
    "float",
    "double",
    "boolean",
    "null",
    "record",
    "enum",
    "array",
    "map",
    "union",
    "fixed",
    "symbolic"};

static_assert((sizeof(typeToString) / sizeof(std::string)) == (AVRO_NUM_TYPES + 1),
              "Incorrect Avro typeToString");

} // namespace strings

// this static assert exists because a 32 bit integer is used as a bit-flag for each type,
// and it would be a problem for this flag if we ever supported more than 32 types
static_assert(AVRO_NUM_TYPES < 32, "Too many Avro types");

const std::string &toString(Type type) noexcept {
    static std::string undefinedType = "Undefined type";
    if (isAvroTypeOrPseudoType(type)) {
        return strings::typeToString[type];
    } else {
        return undefinedType;
    }
}

std::ostream &operator<<(std::ostream &os, Type type) {
    if (isAvroTypeOrPseudoType(type)) {
        os << strings::typeToString[type];
    } else {
        os << static_cast<int>(type);
    }
    return os;
}

std::ostream &operator<<(std::ostream &os, const Null &) {
    os << "(null value)";
    return os;
}

} // namespace avro

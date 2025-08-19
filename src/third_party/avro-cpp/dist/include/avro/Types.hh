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

#ifndef avro_Types_hh__
#define avro_Types_hh__

#include <iostream>

#include "Config.hh"

#include <fmt/format.h>

namespace avro {

/**
 * The "type" for the schema.
 */
enum Type {

    AVRO_STRING, /*!< String */
    AVRO_BYTES,  /*!< Sequence of variable length bytes data */
    AVRO_INT,    /*!< 32-bit integer */
    AVRO_LONG,   /*!< 64-bit integer */
    AVRO_FLOAT,  /*!< Floating point number */
    AVRO_DOUBLE, /*!< Double precision floating point number */
    AVRO_BOOL,   /*!< Boolean value */
    AVRO_NULL,   /*!< Null */

    AVRO_RECORD, /*!< Record, a sequence of fields */
    AVRO_ENUM,   /*!< Enumeration */
    AVRO_ARRAY,  /*!< Homogeneous array of some specific type */
    AVRO_MAP,    /*!< Homogeneous map from string to some specific type */
    AVRO_UNION,  /*!< Union of one or more types */
    AVRO_FIXED,  /*!< Fixed number of bytes */

    AVRO_NUM_TYPES, /*!< Marker */

    // The following is a pseudo-type used in implementation

    AVRO_SYMBOLIC = AVRO_NUM_TYPES, /*!< User internally to avoid circular references. */
    AVRO_UNKNOWN = -1               /*!< Used internally. */
};

/**
 * Returns true if and only if the given type is a primitive.
 * Primitive types are: string, bytes, int, long, float, double, boolean
 * and null
 */
inline constexpr bool isPrimitive(Type t) noexcept {
    return (t >= AVRO_STRING) && (t < AVRO_RECORD);
}

/**
 * Returns true if and only if the given type is a non primitive valid type.
 * Primitive types are: string, bytes, int, long, float, double, boolean
 * and null
 */
inline constexpr bool isCompound(Type t) noexcept {
    return (t >= AVRO_RECORD) && (t < AVRO_NUM_TYPES);
}

/**
 * Returns true if and only if the given type is a valid avro type.
 */
inline constexpr bool isAvroType(Type t) noexcept {
    return (t >= AVRO_STRING) && (t < AVRO_NUM_TYPES);
}

/**
 * Returns true if and only if the given type is within the valid range
 * of enumeration.
 */
inline constexpr bool isAvroTypeOrPseudoType(Type t) noexcept {
    return (t >= AVRO_STRING) && (t <= AVRO_NUM_TYPES);
}

/**
 * Converts the given type into a string. Useful for generating messages.
 */
AVRO_DECL const std::string& toString(Type type) noexcept;

/**
 * Writes a string form of the given type into the given ostream.
 */
AVRO_DECL std::ostream& operator<<(std::ostream& os, avro::Type type);

/// define a type to represent Avro Null in template functions
struct AVRO_DECL Null{};

/**
 * Writes schema for null \p null type to \p os.
 * \param os The ostream to write to.
 * \param null The value to be written.
 */
std::ostream& operator<<(std::ostream& os, const Null& null);

}  // namespace avro

template <>
struct fmt::formatter<avro::Type> : fmt::formatter<std::string> {
    template <typename FormatContext>
    auto format(avro::Type t, FormatContext& ctx) const {
        return fmt::formatter<std::string>::format(avro::toString(t), ctx);
    }
};

#endif

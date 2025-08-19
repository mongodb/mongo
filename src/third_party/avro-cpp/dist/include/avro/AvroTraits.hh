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

#ifndef avro_AvroTraits_hh__
#define avro_AvroTraits_hh__

#include "Config.hh"
#include "Types.hh"
#include <cstdint>
#include <type_traits>

/** @file
 *
 * This header contains type traits and similar utilities used by the library.
 */
namespace avro {

/**
 * Define an is_serializable trait for types we can serialize natively.
 * New types will need to define the trait as well.
 */
template<typename T>
struct is_serializable : public std::false_type {};

template<typename T>
struct is_promotable : public std::false_type {};

template<typename T>
struct type_to_avro {
    static const Type type = AVRO_NUM_TYPES;
};

/**
 * Check if a \p T is a complete type i.e. it is defined as opposed to just
 * declared.
 *
 * is_defined<T>::value will be true or false depending on whether T is a
 * complete type or not respectively.
 */
template<class T>
struct is_defined {

    typedef char yes[1];

    typedef char no[2];

    template<class U>
    static yes &test(char (*)[sizeof(U)]) { throw 0; }

    template<class U>
    static no &test(...) { throw 0; }

    static const bool value = sizeof(test<T>(0)) == sizeof(yes);
};

/**
 * Similar to is_defined, but used to check if T is not defined.
 *
 * is_not_defined<T>::value will be true or false depending on whether T is an
 * incomplete type or not respectively.
 */
template<class T>
struct is_not_defined {

    typedef char yes[1];

    typedef char no[2];

    template<class U>
    static yes &test(char (*)[sizeof(U)]) { throw 0; }

    template<class U>
    static no &test(...) { throw 0; }

    static const bool value = sizeof(test<T>(0)) == sizeof(no);
};

#define DEFINE_PRIMITIVE(CTYPE, AVROTYPE)                     \
    template<>                                                \
    struct is_serializable<CTYPE> : public std::true_type {}; \
                                                              \
    template<>                                                \
    struct type_to_avro<CTYPE> {                              \
        static const Type type = AVROTYPE;                    \
    };

#define DEFINE_PROMOTABLE_PRIMITIVE(CTYPE, AVROTYPE)        \
    template<>                                              \
    struct is_promotable<CTYPE> : public std::true_type {}; \
                                                            \
    DEFINE_PRIMITIVE(CTYPE, AVROTYPE)

DEFINE_PROMOTABLE_PRIMITIVE(int32_t, AVRO_INT)
DEFINE_PROMOTABLE_PRIMITIVE(int64_t, AVRO_LONG)
DEFINE_PROMOTABLE_PRIMITIVE(float, AVRO_FLOAT)
DEFINE_PRIMITIVE(double, AVRO_DOUBLE)
DEFINE_PRIMITIVE(bool, AVRO_BOOL)
DEFINE_PRIMITIVE(Null, AVRO_NULL)
DEFINE_PRIMITIVE(std::string, AVRO_STRING)
DEFINE_PRIMITIVE(std::vector<uint8_t>, AVRO_BYTES)

} // namespace avro

#endif

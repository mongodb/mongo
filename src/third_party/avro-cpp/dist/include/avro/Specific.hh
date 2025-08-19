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

#ifndef avro_Codec_hh__
#define avro_Codec_hh__

#include "array"
#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "boost/blank.hpp"

#include "AvroTraits.hh"
#include "Config.hh"
#include "Decoder.hh"
#include "Encoder.hh"

/**
 * A bunch of templates and specializations for encoding and decoding
 * specific types.
 *
 * Primitive AVRO types BOOLEAN, INT, LONG, FLOAT, DOUBLE, STRING and BYTES
 * get decoded to and encoded from C++ types bool, int32_t, int64_t, float,
 * double, std::string and std::vector<uint8_t> respectively. In addition,
 * std::vector<T> for arbitrary type T gets encoded as an Avro array of T.
 * Similarly, std::map<std::string, T> for arbitrary type T gets encoded
 * as an Avro map with value type T.
 *
 * Users can have their custom types encoded/decoded by specializing
 * avro::codec_traits class for their types.
 */
namespace avro {

typedef boost::blank null;

template<typename T>
void encode(Encoder &e, const T &t);
template<typename T>
void decode(Decoder &d, T &t);

/**
 * Codec_traits tells avro how to encode and decode an object of given type.
 *
 * The class is expected to have two static methods:
 * \li static void encode(Encoder& e, const T& value);
 * \li static void decode(Decoder& e, T& value);
 * The default is empty.
 */
template<typename T>
struct codec_traits;

/**
 * codec_traits for Avro boolean.
 */
template<>
struct codec_traits<bool> {
    /**
     * Encodes a given value.
     */
    static void encode(Encoder &e, bool b) {
        e.encodeBool(b);
    }

    /**
     * Decodes into a given value.
     */
    static void decode(Decoder &d, bool &b) {
        b = d.decodeBool();
    }
};

/**
 * codec_traits for Avro int.
 */
template<>
struct codec_traits<int32_t> {
    /**
     * Encodes a given value.
     */
    static void encode(Encoder &e, int32_t i) {
        e.encodeInt(i);
    }

    /**
     * Decodes into a given value.
     */
    static void decode(Decoder &d, int32_t &i) {
        i = d.decodeInt();
    }
};

/**
 * codec_traits for Avro long.
 */
template<>
struct codec_traits<int64_t> {
    /**
     * Encodes a given value.
     */
    static void encode(Encoder &e, int64_t l) {
        e.encodeLong(l);
    }

    /**
     * Decodes into a given value.
     */
    static void decode(Decoder &d, int64_t &l) {
        l = d.decodeLong();
    }
};

/**
 * codec_traits for Avro float.
 */
template<>
struct codec_traits<float> {
    /**
     * Encodes a given value.
     */
    static void encode(Encoder &e, float f) {
        e.encodeFloat(f);
    }

    /**
     * Decodes into a given value.
     */
    static void decode(Decoder &d, float &f) {
        f = d.decodeFloat();
    }
};

/**
 * codec_traits for Avro double.
 */
template<>
struct codec_traits<double> {
    /**
     * Encodes a given value.
     */
    static void encode(Encoder &e, double d) {
        e.encodeDouble(d);
    }

    /**
     * Decodes into a given value.
     */
    static void decode(Decoder &d, double &dbl) {
        dbl = d.decodeDouble();
    }
};

/**
 * codec_traits for Avro string.
 */
template<>
struct codec_traits<std::string> {
    /**
     * Encodes a given value.
     */
    static void encode(Encoder &e, const std::string &s) {
        e.encodeString(s);
    }

    /**
     * Decodes into a given value.
     */
    static void decode(Decoder &d, std::string &s) {
        s = d.decodeString();
    }
};

/**
 * codec_traits for Avro bytes.
 */
template<>
struct codec_traits<std::vector<uint8_t>> {
    /**
     * Encodes a given value.
     */
    static void encode(Encoder &e, const std::vector<uint8_t> &b) {
        e.encodeBytes(b);
    }

    /**
     * Decodes into a given value.
     */
    static void decode(Decoder &d, std::vector<uint8_t> &s) {
        d.decodeBytes(s);
    }
};

/**
 * codec_traits for Avro fixed.
 */
template<size_t N>
struct codec_traits<std::array<uint8_t, N>> {
    /**
     * Encodes a given value.
     */
    static void encode(Encoder &e, const std::array<uint8_t, N> &b) {
        e.encodeFixed(b.data(), N);
    }

    /**
     * Decodes into a given value.
     */
    static void decode(Decoder &d, std::array<uint8_t, N> &s) {
        std::vector<uint8_t> v(N);
        d.decodeFixed(N, v);
        std::copy(v.data(), v.data() + N, s.data());
    }
};

/**
 * codec_traits for Avro arrays.
 */
template<typename T>
struct codec_traits<std::vector<T>> {
    /**
     * Encodes a given value.
     */
    static void encode(Encoder &e, const std::vector<T> &b) {
        e.arrayStart();
        if (!b.empty()) {
            e.setItemCount(b.size());
            for (typename std::vector<T>::const_iterator it = b.begin();
                 it != b.end(); ++it) {
                e.startItem();
                avro::encode(e, *it);
            }
        }
        e.arrayEnd();
    }

    /**
     * Decodes into a given value.
     */
    static void decode(Decoder &d, std::vector<T> &s) {
        s.clear();
        for (size_t n = d.arrayStart(); n != 0; n = d.arrayNext()) {
            for (size_t i = 0; i < n; ++i) {
                T t;
                avro::decode(d, t);
                s.push_back(std::move(t));
            }
        }
    }
};

typedef codec_traits<std::vector<bool>::const_reference> bool_codec_traits;

template<>
struct codec_traits<std::conditional<avro::is_not_defined<bool_codec_traits>::value,
                                     std::vector<bool>::const_reference, void>::type> {
    /**
     * Encodes a given value.
     */
    static void encode(Encoder &e, std::vector<bool>::const_reference b) {
        e.encodeBool(b);
    }
};

/**
 * codec_traits for Avro maps.
 */
template<typename T>
struct codec_traits<std::map<std::string, T>> {
    /**
     * Encodes a given value.
     */
    static void encode(Encoder &e, const std::map<std::string, T> &b) {
        e.mapStart();
        if (!b.empty()) {
            e.setItemCount(b.size());
            for (typename std::map<std::string, T>::const_iterator
                     it = b.begin();
                 it != b.end(); ++it) {
                e.startItem();
                avro::encode(e, it->first);
                avro::encode(e, it->second);
            }
        }
        e.mapEnd();
    }

    /**
     * Decodes into a given value.
     */
    static void decode(Decoder &d, std::map<std::string, T> &s) {
        s.clear();
        for (size_t n = d.mapStart(); n != 0; n = d.mapNext()) {
            for (size_t i = 0; i < n; ++i) {
                std::string k;
                avro::decode(d, k);
                T &t = s[std::move(k)];
                avro::decode(d, t);
            }
        }
    }
};

/**
* codec_traits for Avro null.
*/
template<>
struct codec_traits<avro::null> {
    /**
    * Encodes a given value.
    */
    static void encode(Encoder &e, const avro::null &) {
        e.encodeNull();
    }

    /**
    * Decodes into a given value.
    */
    static void decode(Decoder &d, avro::null &) {
        d.decodeNull();
    }
};

/**
 * Generic encoder function that makes use of the codec_traits.
 */
template<typename T>
void encode(Encoder &e, const T &t) {
    codec_traits<T>::encode(e, t);
}

/**
 * Generic decoder function that makes use of the codec_traits.
 */
template<typename T>
void decode(Decoder &d, T &t) {
    codec_traits<T>::decode(d, t);
}

} // namespace avro

#endif // avro_Codec_hh__

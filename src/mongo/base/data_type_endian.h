/*    Copyright 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include <cstring>
#include <type_traits>

#include "mongo/base/data_type.h"
#include "mongo/base/error_codes.h"
#include "mongo/platform/endian.h"

namespace mongo {

template <typename T>
struct BigEndian {
    BigEndian() {}
    BigEndian(T t) : value(t) {}
    T value;

    operator T() const {
        return value;
    }
};

template <typename T>
BigEndian<T> tagBigEndian(T t) {
    return t;
}

template <typename T>
struct LittleEndian {
    LittleEndian() {}
    LittleEndian(T t) : value(t) {}
    T value;

    operator T() const {
        return value;
    }
};

template <typename T>
LittleEndian<T> tagLittleEndian(T t) {
    return t;
}

template <typename T>
struct DataType::Handler<BigEndian<T>> {
    static void unsafeLoad(BigEndian<T>* t, const char* ptr, size_t* advanced) {
        if (t) {
            DataType::unsafeLoad(&t->value, ptr, advanced);

            t->value = endian::bigToNative(t->value);
        } else {
            DataType::unsafeLoad(decltype(&t->value){nullptr}, ptr, advanced);
        }
    }

    static Status load(BigEndian<T>* t,
                       const char* ptr,
                       size_t length,
                       size_t* advanced,
                       std::ptrdiff_t debug_offset) {
        if (t) {
            Status x = DataType::load(&t->value, ptr, length, advanced, debug_offset);

            if (x.isOK()) {
                t->value = endian::bigToNative(t->value);
            }

            return x;
        } else {
            return DataType::load(
                decltype(&t->value){nullptr}, ptr, length, advanced, debug_offset);
        }
    }

    static void unsafeStore(const BigEndian<T>& t, char* ptr, size_t* advanced) {
        DataType::unsafeStore(endian::nativeToBig(t.value), ptr, advanced);
    }

    static Status store(const BigEndian<T>& t,
                        char* ptr,
                        size_t length,
                        size_t* advanced,
                        std::ptrdiff_t debug_offset) {
        return DataType::store(endian::nativeToBig(t.value), ptr, length, advanced, debug_offset);
    }

    static BigEndian<T> defaultConstruct() {
        return DataType::defaultConstruct<T>();
    }
};

template <typename T>
struct DataType::Handler<LittleEndian<T>> {
    static void unsafeLoad(LittleEndian<T>* t, const char* ptr, size_t* advanced) {
        if (t) {
            DataType::unsafeLoad(&t->value, ptr, advanced);

            t->value = endian::littleToNative(t->value);
        } else {
            DataType::unsafeLoad(decltype(&t->value){nullptr}, ptr, advanced);
        }
    }

    static Status load(LittleEndian<T>* t,
                       const char* ptr,
                       size_t length,
                       size_t* advanced,
                       std::ptrdiff_t debug_offset) {
        if (t) {
            Status x = DataType::load(&t->value, ptr, length, advanced, debug_offset);

            if (x.isOK()) {
                t->value = endian::littleToNative(t->value);
            }

            return x;
        } else {
            return DataType::load(
                decltype(&t->value){nullptr}, ptr, length, advanced, debug_offset);
        }
    }

    static void unsafeStore(const LittleEndian<T>& t, char* ptr, size_t* advanced) {
        DataType::unsafeStore(endian::nativeToLittle(t.value), ptr, advanced);
    }

    static Status store(const LittleEndian<T>& t,
                        char* ptr,
                        size_t length,
                        size_t* advanced,
                        std::ptrdiff_t debug_offset) {
        return DataType::store(
            endian::nativeToLittle(t.value), ptr, length, advanced, debug_offset);
    }

    static LittleEndian<T> defaultConstruct() {
        return DataType::defaultConstruct<T>();
    }
};

}  // namespace mongo

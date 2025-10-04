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

#include "mongo/base/error_codes.h"
#include "mongo/base/static_assert.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/platform/compiler.h"

#include <cstring>
#include <iosfwd>
#include <type_traits>

namespace mongo {

struct DataType {
    // Second template parameter allows templatized SFINAE specialization.
    //
    // Something like:
    //   template <typename T, typename std::enable_if<std::is_CONDITION<T>::value>::type>
    //   struct Handler { ... };
    //
    // That would allow you to constrain your specialization to all T's
    // that std::is_CONDITION<T>
    //
    // Again, note that you probably don't ever want to use this second
    // parameter for anything.  If you're not interested in template meta
    // programming to duck type in a specialization, you can pretend that
    // this just says template <typename T>.

    template <typename T, typename = void>
    struct Handler {
        static void unsafeLoad(T* t, const char* ptr, size_t* advanced) {
            MONGO_STATIC_ASSERT_MSG(std::is_trivially_copyable_v<T>,
                                    "The generic DataType implementation requires values to be "
                                    "trivially copyable. You may specialize the template to use it "
                                    "with other types.");
            if (t) {
                std::memcpy(t, ptr, sizeof(T));
            }

            if (advanced) {
                *advanced = sizeof(T);
            }
        }

        static Status load(
            T* t, const char* ptr, size_t length, size_t* advanced, std::ptrdiff_t debug_offset) {
            if (sizeof(T) > length) {
                return DataType::makeTrivialLoadStatus(sizeof(T), length, debug_offset);
            }

            unsafeLoad(t, ptr, advanced);

            return Status::OK();
        }

        static void unsafeStore(const T& t, char* ptr, size_t* advanced) {
            MONGO_STATIC_ASSERT_MSG(std::is_trivially_copyable_v<T>,
                                    "The generic DataType implementation requires values to be "
                                    "trivially copyable. You may specialize the template to use it "
                                    "with other types.");
            if (ptr) {
                /** Silence spurious GCC stringop-overflow false negatives. */
                MONGO_COMPILER_DIAGNOSTIC_PUSH
                MONGO_COMPILER_IF_GCC(MONGO_COMPILER_DIAGNOSTIC_IGNORED("-Wstringop-overflow"))
                std::memcpy(ptr, &t, sizeof(T));
                MONGO_COMPILER_DIAGNOSTIC_POP
            }

            if (advanced) {
                *advanced = sizeof(T);
            }
        }

        static Status store(
            const T& t, char* ptr, size_t length, size_t* advanced, std::ptrdiff_t debug_offset) {
            if (sizeof(T) > length) {
                return DataType::makeTrivialStoreStatus(sizeof(T), length, debug_offset);
            }

            unsafeStore(t, ptr, advanced);

            return Status::OK();
        }

        // It may be useful to specialize this for types that aren't natively
        // default constructible. Otherwise there's no way for us to support
        // that body of types (other than wrapping them with another tagged
        // type). Also, this guarantees value/aggregate initialization, which
        // guarantees no uninitialized memory leaks from load's, which gcc
        // otherwise can't seem to see.
        static T defaultConstruct() {
            return T{};
        }
    };

    // The following dispatch functions don't just save typing, they also work
    // around what seems like template type deduction bugs (for template
    // specializations) in gcc.  I.e. for sufficiently complicated workflows (a
    // specialization for tuple), going through dispatch functions compiles on
    // gcc 4.9 and using DataType<T> does not.

    // We return a status and take an out pointer so that we can:
    //
    // 1. Run a load without returning a value (I.e. skip / validate)
    // 2. Load directly into a remote structure, rather than forcing moves of
    //    possibly large objects
    template <typename T>
    static Status load(
        T* t, const char* ptr, size_t length, size_t* advanced, std::ptrdiff_t debug_offset) {
        return Handler<T>::load(t, ptr, length, advanced, debug_offset);
    }

    template <typename T>
    static Status store(
        const T& t, char* ptr, size_t length, size_t* advanced, std::ptrdiff_t debug_offset) {
        return Handler<T>::store(t, ptr, length, advanced, debug_offset);
    }

    template <typename T>
    static void unsafeLoad(T* t, const char* ptr, size_t* advanced) {
        Handler<T>::unsafeLoad(t, ptr, advanced);
    }

    template <typename T>
    static void unsafeStore(const T& t, char* ptr, size_t* advanced) {
        Handler<T>::unsafeStore(t, ptr, advanced);
    }

    template <typename T>
    static T defaultConstruct() {
        return Handler<T>::defaultConstruct();
    }

    static Status makeTrivialStoreStatus(size_t sizeOfT, size_t length, size_t debug_offset);
    static Status makeTrivialLoadStatus(size_t sizeOfT, size_t length, size_t debug_offset);
};

template <>
struct DataType::Handler<StringData> {
    // Consumes all available data, producing
    // a `StringData(ptr,length)`.
    static Status load(StringData* sdata,
                       const char* ptr,
                       size_t length,
                       size_t* advanced,
                       std::ptrdiff_t debug_offset);

    // Copies `sdata` fully into the [ptr,ptr+length) range.
    // Does nothing and returns an Overflow status if
    // `sdata` doesn't fit.
    static Status store(
        StringData sdata, char* ptr, size_t length, size_t* advanced, std::ptrdiff_t debug_offset);

    static StringData defaultConstruct() {
        return StringData();
    }
};
}  // namespace mongo

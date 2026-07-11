// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/base/static_assert.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/platform/compiler.h"
#include "mongo/util/modules.h"

#include <cstring>
#include <iosfwd>
#include <string_view>
#include <type_traits>

[[MONGO_MOD_PUBLIC]];

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
struct DataType::Handler<std::string_view> {
    // Consumes all available data, producing
    // a `std::string_view(ptr,length)`.
    static Status load(std::string_view* sdata,
                       const char* ptr,
                       size_t length,
                       size_t* advanced,
                       std::ptrdiff_t debug_offset);

    // Copies `sdata` fully into the [ptr,ptr+length) range.
    // Does nothing and returns an Overflow status if
    // `sdata` doesn't fit.
    static Status store(std::string_view sdata,
                        char* ptr,
                        size_t length,
                        size_t* advanced,
                        std::ptrdiff_t debug_offset);

    static std::string_view defaultConstruct() {
        return std::string_view();
    }
};
}  // namespace mongo

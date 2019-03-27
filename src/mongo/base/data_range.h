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

#include <cstring>
#include <tuple>
#include <type_traits>

#include "mongo/base/data_type.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/platform/endian.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
class ConstDataRange {
protected:
    // These are helper types to make ConstDataRange's and friends constructable either from
    // a range of byte-like pointers or from a container of byte-like values.
    template <typename T>
    constexpr static auto isByteV = ((std::is_integral_v<T> && sizeof(T) == 1) ||
                                     std::is_same_v<T, std::byte>);

    template <typename T, typename = void>
    struct HasDataSize : std::false_type {};

    template <typename T>
    struct HasDataSize<
        T,
        std::enable_if_t<std::is_void_v<
            std::void_t<decltype(std::declval<T>().data()), decltype(std::declval<T>().size())>>>>
        : std::true_type {};

    template <typename T, typename = void>
    struct ContiguousContainerOfByteLike : std::false_type {};

    template <typename T>
    struct ContiguousContainerOfByteLike<
        T,
        std::void_t<decltype(std::declval<T>().data()),
                    std::enable_if_t<isByteV<typename T::value_type> && HasDataSize<T>::value>>>
        : std::true_type {};

public:
    using byte_type = char;

    // You can construct a ConstDataRange from any byte-like sequence. Byte-like means an
    // integral type with a size of one.
    //
    // begin and end should point to the first and one past last bytes in
    // the range you wish to view.
    //
    // debug_offset provides a way to indicate that the ConstDataRange is
    // located at an offset into some larger logical buffer. By setting it
    // to a non-zero value, you'll change the Status messages that are
    // returned on failure to be offset by the amount passed to this
    // constructor.
    template <typename ByteLike, typename std::enable_if_t<isByteV<ByteLike>, int> = 0>
    ConstDataRange(const ByteLike* begin, const ByteLike* end, std::ptrdiff_t debug_offset = 0)
        : _begin(reinterpret_cast<const byte_type*>(begin)),
          _end(reinterpret_cast<const byte_type*>(end)),
          _debug_offset(debug_offset) {
        invariant(end >= begin);
    }

    // Constructing from nullptr, nullptr initializes an empty ConstDataRange.
    ConstDataRange(std::nullptr_t, std::nullptr_t, std::ptrdiff_t debug_offset = 0)
        : _begin(nullptr), _end(nullptr), _debug_offset(debug_offset) {}

    // You can also construct from a pointer to a byte-like type and a size.
    template <typename ByteLike, typename std::enable_if_t<isByteV<ByteLike>, int> = 0>
    ConstDataRange(const ByteLike* begin, std::size_t length, std::ptrdiff_t debug_offset = 0)
        : _begin(reinterpret_cast<const byte_type*>(begin)),
          _end(reinterpret_cast<const byte_type*>(_begin + length)),
          _debug_offset(debug_offset) {}

    // ConstDataRange can also act as a view of a container of byte-like values, such as a
    // std::vector<uint8_t> or a std::array<char, size>. The requirements are that the
    // value_type of the container is byte-like and that the values be contiguous - the container
    // must have a data() function that returns a pointer to the front and a size() function
    // that returns the number of elements.
    template <typename Container,
              typename std::enable_if_t<ContiguousContainerOfByteLike<Container>::value, int> = 0>
    ConstDataRange(const Container& container, std::ptrdiff_t debug_offset = 0)
        : ConstDataRange(container.data(), container.size(), debug_offset) {}

    // You can also construct from a C-style array, including string literals.
    template <typename ByteLike, size_t N, typename std::enable_if_t<isByteV<ByteLike>, int> = 0>
    ConstDataRange(const ByteLike (&arr)[N], std::ptrdiff_t debug_offset = 0)
        : ConstDataRange(arr, N, debug_offset) {}

    const byte_type* data() const noexcept {
        return _begin;
    }

    size_t length() const noexcept {
        return _end - _begin;
    }

    bool empty() const noexcept {
        return length() == 0;
    }

    template <typename T>
    Status readIntoNoThrow(T* t, size_t offset = 0) const noexcept {
        if (offset > length()) {
            return makeOffsetStatus(offset);
        }

        return DataType::load(
            t, _begin + offset, length() - offset, nullptr, offset + _debug_offset);
    }

    template <typename T>
    void readInto(T* t, size_t offset = 0) const {
        uassertStatusOK(readIntoNoThrow(t, offset));
    }

    template <typename T>
    StatusWith<T> readNoThrow(std::size_t offset = 0) const noexcept {
        T t(DataType::defaultConstruct<T>());
        Status s = readIntoNoThrow(&t, offset);

        if (s.isOK()) {
            return StatusWith<T>(std::move(t));
        } else {
            return StatusWith<T>(std::move(s));
        }
    }

    template <typename T>
    T read(std::size_t offset = 0) const {
        return uassertStatusOK(readNoThrow<T>(offset));
    }

    friend bool operator==(const ConstDataRange& lhs, const ConstDataRange& rhs) {
        return std::tie(lhs._begin, lhs._end) == std::tie(rhs._begin, rhs._end);
    }

    friend bool operator!=(const ConstDataRange& lhs, const ConstDataRange& rhs) {
        return !(lhs == rhs);
    }


protected:
    const byte_type* _begin;
    const byte_type* _end;
    std::ptrdiff_t _debug_offset;

    Status makeOffsetStatus(size_t offset) const;
};

class DataRange : public ConstDataRange {
public:
    // You can construct a DataRange from all the same types as ConstDataRange, except that the
    // arguments may not be const (since this is the mutable version of ConstDataRange).
    template <typename ByteLike>
    DataRange(ByteLike* begin, ByteLike* end, std::ptrdiff_t debug_offset = 0)
        : ConstDataRange(begin, end, debug_offset) {}

    template <typename ByteLike>
    DataRange(const ByteLike*, const ByteLike* end, std::ptrdiff_t debug_offset) = delete;

    template <typename ByteLike>
    DataRange(const ByteLike*, const ByteLike* end) = delete;

    template <typename ByteLike>
    DataRange(ByteLike* begin, std::size_t length, std::ptrdiff_t debug_offset = 0)
        : ConstDataRange(begin, length, debug_offset) {}

    template <typename ByteLike>
    DataRange(const ByteLike*, size_t, std::ptrdiff_t debug_offset) = delete;

    template <typename ByteLike>
    DataRange(const ByteLike*, size_t) = delete;

    DataRange(std::nullptr_t, std::nullptr_t, std::ptrdiff_t debug_offset = 0)
        : ConstDataRange(nullptr, nullptr, debug_offset) {}

    template <typename Container,
              typename std::enable_if_t<ContiguousContainerOfByteLike<Container>::value, int> = 0>
    DataRange(Container& container, std::ptrdiff_t debug_offset = 0)
        : ConstDataRange(std::forward<Container>(container), debug_offset) {}

    template <typename Container,
              typename std::enable_if_t<ContiguousContainerOfByteLike<Container>::value, int> = 0>
    DataRange(const Container&, std::ptrdiff_t) = delete;

    template <typename Container,
              typename std::enable_if_t<ContiguousContainerOfByteLike<Container>::value, int> = 0>
    DataRange(const Container&) = delete;

    template <typename ByteLike, size_t N>
    DataRange(ByteLike (&arr)[N], std::ptrdiff_t debug_offset = 0)
        : ConstDataRange(arr, N, debug_offset) {}

    template <typename ByteLike, size_t N>
    DataRange(const ByteLike (&arr)[N], std::ptrdiff_t) = delete;

    template <typename ByteLike, size_t N>
    DataRange(const ByteLike (&arr)[N]) = delete;

    template <typename T>
    Status writeNoThrow(const T& value, std::size_t offset = 0) noexcept {
        if (offset > length()) {
            return makeOffsetStatus(offset);
        }

        return DataType::store(value,
                               const_cast<char*>(_begin + offset),
                               length() - offset,
                               nullptr,
                               offset + _debug_offset);
    }

    template <typename T>
    void write(const T& value, std::size_t offset = 0) {
        uassertStatusOK(writeNoThrow(value, offset));
    }
};

struct DataRangeTypeHelper {
    static Status makeStoreStatus(size_t t_length, size_t length, std::ptrdiff_t debug_offset);
};

// Enable for classes derived from ConstDataRange
template <typename T>
struct DataType::Handler<T,
                         typename std::enable_if<std::is_base_of<ConstDataRange, T>::value>::type> {
    static Status load(
        T* t, const char* ptr, size_t length, size_t* advanced, std::ptrdiff_t debug_offset) {
        if (t) {
            // Assuming you know what you're doing at the read above this
            // is fine.  Either you're reading into a readable buffer, so
            // ptr started off non-const, or the const_cast will feed back
            // to const char* taking Const variants.  So it'll get tossed
            // out again.
            *t = T(const_cast<char*>(ptr), const_cast<char*>(ptr) + length);
        }

        if (advanced) {
            *advanced = length;
        }

        return Status::OK();
    }

    static Status store(
        const T& t, char* ptr, size_t length, size_t* advanced, std::ptrdiff_t debug_offset) {
        if (t.length() > length) {
            return DataRangeTypeHelper::makeStoreStatus(t.length(), length, debug_offset);
        }

        if (ptr) {
            std::memcpy(ptr, t.data(), t.length());
        }

        if (advanced) {
            *advanced = t.length();
        }

        return Status::OK();
    }

    static T defaultConstruct() {
        return T(nullptr, nullptr);
    }
};

}  // namespace mongo

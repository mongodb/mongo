/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/logv2/constants.h"
#include "mongo/stdx/variant.h"

#include <functional>

namespace mongo {
namespace logv2 {

class TypeErasedAttributeStorage;

// Custom type, storage for how to call its formatters
struct CustomAttributeValue {
    std::function<void(BSONObjBuilder&)> BSONSerialize;
    std::function<BSONArray()> toBSONArray;
    std::function<void(BSONObjBuilder&, StringData)> BSONAppend;

    // Have both serialize and toString available, using toString() with a serialize interface is
    // inefficient.
    std::function<void(fmt::memory_buffer&)> stringSerialize;
    std::function<std::string()> toString;
};

namespace detail {
namespace {

// Helper traits to figure out capabilities on custom types
template <class T, class = void>
struct HasToBSON : std::false_type {};

template <class T>
struct HasToBSON<T, std::void_t<decltype(std::declval<T>().toBSON())>> : std::true_type {};

template <class T, class = void>
struct HasToBSONArray : std::false_type {};

template <class T>
struct HasToBSONArray<T, std::void_t<decltype(std::declval<T>().toBSONArray())>> : std::true_type {
};

template <class T, class = void>
struct HasBSONSerialize : std::false_type {};

template <class T>
struct HasBSONSerialize<
    T,
    std::void_t<decltype(std::declval<T>().serialize(std::declval<BSONObjBuilder*>()))>>
    : std::true_type {};

template <class T, class = void>
struct HasBSONBuilderAppend : std::false_type {};

template <class T>
struct HasBSONBuilderAppend<T,
                            std::void_t<decltype(std::declval<BSONObjBuilder>().append(
                                std::declval<StringData>(), std::declval<T>()))>> : std::true_type {
};

template <class T, class = void>
struct HasStringSerialize : std::false_type {};

template <class T>
struct HasStringSerialize<
    T,
    std::void_t<decltype(std::declval<T>().serialize(std::declval<fmt::memory_buffer&>()))>>
    : std::true_type {};

template <class T, class = void>
struct HasToString : std::false_type {};

template <class T>
struct HasToString<T, std::void_t<decltype(std::declval<T>().toString())>> : std::true_type {};

}  // namespace

// Named attribute, storage for a name-value attribute.
class NamedAttribute {
public:
    NamedAttribute() = default;
    NamedAttribute(StringData n, int val) : name(n), value(val) {}
    NamedAttribute(StringData n, unsigned int val) : name(n), value(val) {}
    // long is 32bit on Windows and 64bit on posix. To avoid ambiguity where different platforms we
    // treat long as 64bit always
    NamedAttribute(StringData n, long val) : name(n), value(static_cast<long long>(val)) {}
    NamedAttribute(StringData n, unsigned long val)
        : name(n), value(static_cast<unsigned long long>(val)) {}
    NamedAttribute(StringData n, long long val) : name(n), value(val) {}
    NamedAttribute(StringData n, unsigned long long val) : name(n), value(val) {}
    NamedAttribute(StringData n, double val) : name(n), value(val) {}
    NamedAttribute(StringData n, bool val) : name(n), value(val) {}
    NamedAttribute(StringData n, StringData val) : name(n), value(val) {}
    NamedAttribute(StringData n, BSONObj const& val) : name(n), value(&val) {}
    NamedAttribute(StringData n, BSONArray const& val) : name(n), value(&val) {}
    NamedAttribute(StringData n, const char* val) : NamedAttribute(n, StringData(val)) {}
    NamedAttribute(StringData n, char* val) : NamedAttribute(n, static_cast<const char*>(val)) {}
    NamedAttribute(StringData n, float val) : NamedAttribute(n, static_cast<double>(val)) {}
    NamedAttribute(StringData n, std::string const& val) : NamedAttribute(n, StringData(val)) {}
    NamedAttribute(StringData n, long double val) = delete;

    NamedAttribute(StringData n, BSONElement const& val) : name(n) {
        CustomAttributeValue custom;
        custom.BSONSerialize = [&val](BSONObjBuilder& builder) {
            builder.appendElements(val.wrap());
        };
        custom.toString = [&val]() { return val.toString(); };
        value = std::move(custom);
    }

    template <typename T>
    NamedAttribute(StringData n, const boost::optional<T>& val)
        : NamedAttribute(val ? NamedAttribute(n, *val) : NamedAttribute()) {
        if (!val) {
            CustomAttributeValue custom;
            // Use BSONAppend instead of toBSON because we just want the null value and not a whole
            // object with a field name
            custom.BSONAppend = [](BSONObjBuilder& builder, StringData fieldName) {
                builder.appendNull(fieldName);
            };
            custom.toString = [&val]() { return constants::kNullOptionalString.toString(); };
            name = n;
            value = std::move(custom);
        }
    }

    template <typename T,
              typename = std::enable_if_t<!std::is_integral_v<T> && !std::is_floating_point_v<T>>>
    NamedAttribute(StringData n, const T& val) : name(n) {
        static_assert(
            HasToString<T>::value || HasStringSerialize<T>::value,
            "custom type needs toString() or serialize(fmt::memory_buffer&) implementation");

        CustomAttributeValue custom;
        if constexpr (HasBSONBuilderAppend<T>::value) {
            custom.BSONAppend = [&val](BSONObjBuilder& builder, StringData fieldName) {
                builder.append(fieldName, val);
            };
        }
        if constexpr (HasBSONSerialize<T>::value) {
            custom.BSONSerialize = [&val](BSONObjBuilder& builder) { val.serialize(&builder); };
        } else if constexpr (HasToBSON<T>::value) {
            custom.BSONSerialize = [&val](BSONObjBuilder& builder) {
                builder.appendElements(val.toBSON());
            };
        } else if constexpr (HasToBSONArray<T>::value) {
            custom.toBSONArray = [&val]() { return val.toBSONArray(); };
        }
        if constexpr (HasStringSerialize<T>::value) {
            custom.stringSerialize = [&val](fmt::memory_buffer& buffer) {
                return val.serialize(buffer);
            };
        } else if constexpr (HasToString<T>::value) {
            custom.toString = [&val]() { return val.toString(); };
        }

        value = std::move(custom);
    }

    StringData name;
    stdx::variant<int,
                  unsigned int,
                  long long,
                  unsigned long long,
                  bool,
                  double,
                  StringData,
                  const BSONObj*,
                  const BSONArray*,
                  CustomAttributeValue>
        value;
};

// Attribute Storage stores an array of Named Attributes.
template <typename... Args>
class AttributeStorage {
public:
    AttributeStorage(const Args&... args)
        : _data{detail::NamedAttribute(StringData(args.name.data(), args.name.size()),
                                       args.value)...} {}

private:
    static const size_t kNumArgs = sizeof...(Args);

    // Arrays need a size of at least 1, add dummy element if needed (kNumArgs above is still 0)
    NamedAttribute _data[kNumArgs ? kNumArgs : 1];

    // This class is meant to be wrapped by TypeErasedAttributeStorage below that provides public
    // accessors. Let it access all our internals.
    friend class mongo::logv2::TypeErasedAttributeStorage;
};

template <typename... Args>
AttributeStorage<Args...> makeAttributeStorage(const Args&... args) {
    return {args...};
}

}  // namespace detail

// Wrapper around internal pointer of AttributeStorage so it does not need any template parameters
class TypeErasedAttributeStorage {
public:
    TypeErasedAttributeStorage() : _size(0) {}

    template <typename... Args>
    TypeErasedAttributeStorage(const detail::AttributeStorage<Args...>& store) {
        _data = store._data;
        _size = store.kNumArgs;
    }

    bool empty() const {
        return _size == 0;
    }

    std::size_t size() const {
        return _size;
    }

    // Applies a function to every stored named attribute in order they are captured
    template <typename Func>
    void apply(Func&& f) const {
        std::for_each(_data, _data + _size, [&f](const detail::NamedAttribute& attr) {
            StringData name = attr.name;
            stdx::visit([name, &f](auto&& val) { f(name, val); }, attr.value);
        });
    }

private:
    const detail::NamedAttribute* _data;
    size_t _size;
};

}  // namespace logv2
}  // namespace mongo

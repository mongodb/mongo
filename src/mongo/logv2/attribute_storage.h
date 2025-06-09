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
#include "mongo/logv2/log_attr.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"

#include <functional>
#include <string_view>  // NOLINT
#include <variant>

#include <boost/container/small_vector.hpp>

namespace mongo::logv2 {

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

template <typename T>
auto seqLog(const T& container);

template <typename It>
auto seqLog(It begin, It end);

template <typename T>
auto mapLog(const T& container);

template <typename It>
auto mapLog(It begin, It end);

namespace detail {

template <template <class...> class Template, typename... Args>
struct IsInstantiationOf : std::false_type {};
template <template <class...> class Template, typename... Args>
struct IsInstantiationOf<Template, Template<Args...>> : std::true_type {};
template <template <class...> class Template, typename... Args>
constexpr bool isInstantiationOf = IsInstantiationOf<Template, Args...>::value;

// Helper traits to figure out capabilities on custom types
template <class T>
constexpr bool isOptional = isInstantiationOf<boost::optional, T>;

template <class T>
constexpr bool isDuration = isInstantiationOf<Duration, T>;

template <typename T>
using HasMappedTypeOp = typename T::mapped_type;
template <typename T>
constexpr bool hasMappedType = stdx::is_detected_v<HasMappedTypeOp, T>;

// Trait to detect container, common interface for both std::array and std::forward_list
template <class T, typename = void>
struct IsContainer : std::false_type {};
template <typename T>
struct IsContainer<T,
                   std::void_t<typename T::value_type,
                               typename T::size_type,
                               typename T::iterator,
                               typename T::const_iterator,
                               decltype(std::declval<T>().empty()),
                               decltype(std::declval<T>().begin()),
                               decltype(std::declval<T>().end())>> : std::true_type {};
template <typename T>
constexpr bool isContainer = IsContainer<T>::value;

template <typename T>
using HasToStringForLoggingOp = decltype(toStringForLogging(std::declval<T>()));
template <typename T>
constexpr bool hasToStringForLogging = stdx::is_detected_v<HasToStringForLoggingOp, T>;

template <typename T>
using HasToBSONOp = decltype(std::declval<T>().toBSON());
template <typename T>
constexpr bool hasToBSON = stdx::is_detected_v<HasToBSONOp, T>;

template <typename T>
using HasToBSONArrayOp = decltype(std::declval<T>().toBSONArray());
template <typename T>
constexpr bool hasToBSONArray = stdx::is_detected_v<HasToBSONArrayOp, T>;

template <typename T>
using HasBSONSerializeOp = decltype(std::declval<T>().serialize(std::declval<BSONObjBuilder*>()));
template <typename T>
constexpr bool hasBSONSerialize = stdx::is_detected_v<HasBSONSerializeOp, T>;

template <typename T>
using HasBSONBuilderAppendOp =
    decltype(std::declval<BSONObjBuilder>().append(std::declval<StringData>(), std::declval<T>()));
template <typename T>
constexpr bool hasBSONBuilderAppend = stdx::is_detected_v<HasBSONBuilderAppendOp, T>;

template <typename T>
using HasStringSerializeOp =
    decltype(std::declval<T>().serialize(std::declval<fmt::memory_buffer&>()));
template <typename T>
constexpr bool hasStringSerialize = stdx::is_detected_v<HasStringSerializeOp, T>;

template <typename T>
using HasToStringOp = std::remove_cv_t<decltype(std::declval<T>().toString())>;
template <typename T>
constexpr bool hasToString = stdx::is_detected_exact_v<std::string, HasToStringOp, T>;
template <typename T>
constexpr bool hasToStringReturnStringData =
    stdx::is_detected_convertible_v<StringData, HasToStringOp, T>;

template <typename T>
using HasNonMemberToStringOp = decltype(toString(std::declval<T>()));
template <typename T>
constexpr bool hasNonMemberToString =
    stdx::is_detected_exact_v<std::string, HasNonMemberToStringOp, T>;
template <typename T>
constexpr bool hasNonMemberToStringReturnStringData =
    stdx::is_detected_convertible_v<StringData, HasNonMemberToStringOp, T>;

template <typename T>
using HasNonMemberToBSONOp = decltype(toBSON(std::declval<T>()));
template <typename T>
constexpr bool hasNonMemberToBSON = stdx::is_detected_v<HasNonMemberToBSONOp, T>;

template <typename T>
constexpr inline bool isCustomLoggable =
    // Requires any of these BSON serialization calls:
    hasToBSON<T> ||             //   x.toBSON()
    hasNonMemberToBSON<T> ||    //   toBSON(x)
    hasToBSONArray<T> ||        //   x.toBSONArray()
    hasBSONSerialize<T> ||      //   x.serialize(&bob)
    hasBSONBuilderAppend<T> ||  //   bob.append(key, x)
    // or any of these String serialization calls:
    hasToStringForLogging<T> ||               //   std::string toStringForLogging(x)
    hasStringSerialize<T> ||                  //   x.serialize(fmt::memory_buffer&)
    hasToString<T> ||                         //   std::string x.toString()
    hasToStringReturnStringData<T> ||         //   StringData x.toString()
    hasNonMemberToString<T> ||                //   std::string toString(x)
    hasNonMemberToStringReturnStringData<T>;  //   StringData toString(x)

template <typename T>
void requireCustomLoggable() {
    static_assert(isCustomLoggable<T>,
                  "Logging object 'x' of user-defined type requires one of:"
                  // BSON serialization
                  " x.toBSON(),"
                  " toBSON(x),"
                  " x.toBSONArray(),"
                  " x.serialize(BSONObjBuilder*),"
                  " bob.append(key, x),"
                  // string serialization
                  " toStringForLogging(x),"
                  " x.serialize(fmt::memory_buffer&),"
                  " x.toString(),"
                  " toString(x).");
}

// Mapping functions on how to map a logged value to how it is stored in variant (reused by
// container support)
inline bool mapValue(bool value) {
    return value;
}
inline int mapValue(int value) {
    return value;
}
inline unsigned int mapValue(unsigned int value) {
    return value;
}
inline long long mapValue(long value) {
    return value;
}
inline unsigned long long mapValue(unsigned long value) {
    return value;
}
inline long long mapValue(long long value) {
    return value;
}
inline unsigned long long mapValue(unsigned long long value) {
    return value;
}
inline double mapValue(float value) {
    return value;
}
inline double mapValue(double value) {
    return value;
}

inline StringData mapValue(StringData value) {
    return value;
}
inline StringData mapValue(std::string const& value) {
    return value;
}
inline StringData mapValue(std::string_view value) {  // NOLINT
    return toStringDataForInterop(value);
}
inline StringData mapValue(char* value) {
    return value;
}
inline StringData mapValue(const char* value) {
    return value;
}

inline BSONObj mapValue(BSONObj const& value) {
    return value;
}
inline BSONArray mapValue(BSONArray const& value) {
    return value;
}
inline CustomAttributeValue mapValue(BSONElement const& val) {
    CustomAttributeValue custom;
    if (val) {
        custom.BSONSerialize = [&val](BSONObjBuilder& builder) {
            builder.appendElements(val.wrap());
        };
    }
    custom.toString = [&val]() {
        return val.toString();
    };
    return custom;
}
inline CustomAttributeValue mapValue(boost::none_t val) {
    CustomAttributeValue custom;
    // Use BSONAppend instead of toBSON because we just want the null value and not a whole
    // object with a field name
    custom.BSONAppend = [](BSONObjBuilder& builder, StringData fieldName) {
        builder.appendNull(fieldName);
    };
    custom.toString = [] {
        return std::string{constants::kNullOptionalString};
    };
    return custom;
}

template <typename T, std::enable_if_t<isDuration<T>, int> = 0>
auto mapValue(T val) {
    return val;
}

template <typename T, std::enable_if_t<std::is_enum_v<T>, int> = 0>
auto mapValue(T val) {
    if constexpr (hasToStringForLogging<T>) {
        CustomAttributeValue custom;
        custom.toString = [val] {
            return toStringForLogging(val);
        };
        return custom;
    } else if constexpr (hasNonMemberToString<T>) {
        CustomAttributeValue custom;
        custom.toString = [val]() {
            return toString(val);
        };
        return custom;
    } else if constexpr (hasNonMemberToStringReturnStringData<T>) {
        CustomAttributeValue custom;
        custom.stringSerialize = [val](fmt::memory_buffer& buffer) {
            StringData sd = toString(val);
            buffer.append(sd.data(), sd.data() + sd.size());
        };
        return custom;
    } else {
        return mapValue(static_cast<std::underlying_type_t<T>>(val));
    }
}

template <typename T, std::enable_if_t<isContainer<T> && !hasMappedType<T>, int> = 0>
CustomAttributeValue mapValue(const T& val) {
    CustomAttributeValue custom;
    custom.toBSONArray = [&val]() {
        return seqLog(val).toBSONArray();
    };
    custom.stringSerialize = [&val](fmt::memory_buffer& buffer) {
        seqLog(val).serialize(buffer);
    };
    return custom;
}

template <typename T, std::enable_if_t<isContainer<T> && hasMappedType<T>, int> = 0>
CustomAttributeValue mapValue(const T& val) {
    CustomAttributeValue custom;
    custom.BSONSerialize = [&val](BSONObjBuilder& builder) {
        mapLog(val).serialize(&builder);
    };
    custom.stringSerialize = [&val](fmt::memory_buffer& buffer) {
        mapLog(val).serialize(buffer);
    };
    return custom;
}

template <typename T,
          std::enable_if_t<!std::is_integral_v<T> && !std::is_floating_point_v<T> &&
                               !std::is_enum_v<T> && !isDuration<T> && !isContainer<T>,
                           int> = 0>
CustomAttributeValue mapValue(const T& val) {
    requireCustomLoggable<T>();
    CustomAttributeValue custom;
    if constexpr (hasBSONBuilderAppend<T>) {
        custom.BSONAppend = [&val](BSONObjBuilder& builder, StringData fieldName) {
            builder.append(fieldName, val);
        };
    }

    if constexpr (hasBSONSerialize<T>) {
        custom.BSONSerialize = [&val](BSONObjBuilder& builder) {
            val.serialize(&builder);
        };
    } else if constexpr (hasToBSON<T>) {
        custom.BSONSerialize = [&val](BSONObjBuilder& builder) {
            builder.appendElements(val.toBSON());
        };
    } else if constexpr (hasToBSONArray<T>) {
        custom.toBSONArray = [&val]() {
            return val.toBSONArray();
        };
    } else if constexpr (hasNonMemberToBSON<T>) {
        custom.BSONSerialize = [&val](BSONObjBuilder& builder) {
            builder.appendElements(toBSON(val));
        };
    }

    if constexpr (hasToStringForLogging<T>) {
        custom.toString = [&val] {
            return toStringForLogging(val);
        };
    } else if constexpr (hasStringSerialize<T>) {
        custom.stringSerialize = [&val](fmt::memory_buffer& buffer) {
            val.serialize(buffer);
        };
    } else if constexpr (hasToString<T>) {
        custom.toString = [&val]() {
            return val.toString();
        };
    } else if constexpr (hasToStringReturnStringData<T>) {
        custom.stringSerialize = [&val](fmt::memory_buffer& buffer) {
            StringData sd = val.toString();
            buffer.append(sd.data(), sd.data() + sd.size());
        };
    } else if constexpr (hasNonMemberToString<T>) {
        custom.toString = [&val]() {
            return toString(val);
        };
    } else if constexpr (hasNonMemberToStringReturnStringData<T>) {
        custom.stringSerialize = [&val](fmt::memory_buffer& buffer) {
            StringData sd = toString(val);
            buffer.append(sd.data(), sd.data() + sd.size());
        };
    }

    return custom;
}

template <typename It>
class SequenceContainerLogger {
public:
    SequenceContainerLogger(It begin, It end) : _begin(begin), _end(end) {}

    // JSON Format: [elem1, elem2, ..., elemN]
    BSONArray toBSONArray() const {
        BSONArrayBuilder builder;
        for (auto it = _begin; it != _end; ++it) {
            const auto& item = *it;
            auto append = [&builder](auto&& val) {
                using V = decltype(val);
                if constexpr (std::is_same_v<V, CustomAttributeValue&&>) {
                    if (val.BSONAppend) {
                        BSONObjBuilder objBuilder;
                        val.BSONAppend(objBuilder, ""_sd);
                        builder.append(objBuilder.done().getField(""_sd));
                    } else if (val.BSONSerialize) {
                        BSONObjBuilder objBuilder;
                        val.BSONSerialize(objBuilder);
                        builder.append(objBuilder.done());
                    } else if (val.toBSONArray) {
                        builder.append(val.toBSONArray());
                    } else if (val.stringSerialize) {
                        fmt::memory_buffer buffer;
                        val.stringSerialize(buffer);
                        builder.append(fmt::to_string(buffer));
                    } else {
                        builder.append(val.toString());
                    }
                } else if constexpr (isDuration<std::decay_t<V>>) {
                    builder.append(val.toBSON());
                } else if constexpr (std::is_same_v<std::decay_t<V>, unsigned int>) {
                    builder.append(static_cast<long long>(val));
                } else {
                    builder.append(val);
                }
            };

            using item_t = std::decay_t<decltype(item)>;
            if constexpr (isOptional<item_t>) {
                if (item) {
                    append(mapValue(*item));
                } else {
                    append(mapValue(boost::none));
                }
            } else {
                append(mapValue(item));
            }
        }
        return builder.arr();
    }

    // Text Format: (elem1, elem2, ..., elemN)
    void serialize(fmt::memory_buffer& buffer) const {
        StringData separator;
        buffer.push_back('(');
        for (auto it = _begin; it != _end; ++it) {
            const auto& item = *it;
            buffer.append(separator.data(), separator.data() + separator.size());

            auto append = [&buffer](auto&& val) {
                if constexpr (std::is_same_v<decltype(val), CustomAttributeValue&&>) {
                    if (val.stringSerialize) {
                        val.stringSerialize(buffer);
                    } else if (val.toString) {
                        fmt::format_to(std::back_inserter(buffer), "{}", val.toString());
                    } else if (val.BSONSerialize) {
                        BSONObjBuilder objBuilder;
                        val.BSONSerialize(objBuilder);
                        objBuilder.done().jsonStringBuffer(
                            JsonStringFormat::ExtendedRelaxedV2_0_0, 0, false, buffer);
                    } else if (val.BSONAppend) {
                        BSONObjBuilder objBuilder;
                        val.BSONAppend(objBuilder, ""_sd);
                        objBuilder.done().getField(""_sd).jsonStringBuffer(
                            JsonStringFormat::ExtendedRelaxedV2_0_0, false, false, 0, buffer);
                    } else {
                        val.toBSONArray().jsonStringBuffer(
                            JsonStringFormat::ExtendedRelaxedV2_0_0, 0, true, buffer);
                    }

                } else if constexpr (isDuration<std::decay_t<decltype(val)>>) {
                    fmt::format_to(std::back_inserter(buffer), "{}", val.toString());
                } else if constexpr (std::is_same_v<std::decay_t<decltype(val)>, BSONObj>) {
                    val.jsonStringBuffer(JsonStringFormat::ExtendedRelaxedV2_0_0, 0, false, buffer);
                } else if constexpr (std::is_same_v<std::decay_t<decltype(val)>, BSONArray>) {
                    val.jsonStringBuffer(JsonStringFormat::ExtendedRelaxedV2_0_0, 0, true, buffer);
                } else {
                    fmt::format_to(std::back_inserter(buffer), "{}", val);
                }
            };

            using item_t = std::decay_t<decltype(item)>;
            if constexpr (isOptional<item_t>) {
                if (item) {
                    append(mapValue(*item));
                } else {
                    append(mapValue(boost::none));
                }
            } else {
                append(mapValue(item));
            }

            separator = ", "_sd;
        }
        buffer.push_back(')');
    }

private:
    It _begin;
    It _end;
};

template <typename It>
class AssociativeContainerLogger {
public:
    static_assert(std::is_same_v<decltype(mapValue(std::declval<It>()->first)), StringData>,
                  "key in associative container needs to be a string");

    AssociativeContainerLogger(It begin, It end) : _begin(begin), _end(end) {}

    // JSON Format: {"elem1": val1, "elem2": val2, ..., "elemN": valN}
    void serialize(BSONObjBuilder* builder) const {
        for (auto it = _begin; it != _end; ++it) {
            const auto& item = *it;
            auto append = [builder](StringData key, auto&& val) {
                using V = decltype(val);
                if constexpr (std::is_same_v<V, CustomAttributeValue&&>) {
                    if (val.BSONAppend) {
                        val.BSONAppend(*builder, key);
                    } else if (val.BSONSerialize) {
                        BSONObjBuilder subBuilder = builder->subobjStart(key);
                        val.BSONSerialize(subBuilder);
                        subBuilder.done();
                    } else if (val.toBSONArray) {
                        builder->append(key, val.toBSONArray());
                    } else if (val.stringSerialize) {
                        fmt::memory_buffer buffer;
                        val.stringSerialize(buffer);
                        builder->append(key, fmt::to_string(buffer));
                    } else {
                        builder->append(key, val.toString());
                    }
                } else if constexpr (isDuration<std::decay_t<V>>) {
                    builder->append(key, val.toBSON());
                } else if constexpr (std::is_same_v<std::decay_t<V>, unsigned int>) {
                    builder->append(key, static_cast<long long>(val));
                } else {
                    builder->append(key, val);
                }
            };
            auto key = mapValue(item.first);
            using value_t = std::decay_t<decltype(item.second)>;
            if constexpr (isOptional<value_t>) {
                if (item.second) {
                    append(key, mapValue(*item.second));
                } else {
                    append(key, mapValue(boost::none));
                }
            } else {
                append(key, mapValue(item.second));
            }
        }
    }

    // Text Format: (elem1: val1, elem2: val2, ..., elemN: valN)
    void serialize(fmt::memory_buffer& buffer) const {
        StringData separator = ""_sd;
        buffer.push_back('(');
        for (auto it = _begin; it != _end; ++it) {
            const auto& item = *it;
            buffer.append(separator.data(), separator.data() + separator.size());

            auto append = [&buffer](StringData key, auto&& val) {
                if constexpr (std::is_same_v<decltype(val), CustomAttributeValue&&>) {
                    if (val.stringSerialize) {
                        fmt::format_to(std::back_inserter(buffer), "{}: ", key);
                        val.stringSerialize(buffer);
                    } else if (val.toString) {
                        fmt::format_to(std::back_inserter(buffer), "{}: {}", key, val.toString());
                    } else if (val.BSONSerialize) {
                        BSONObjBuilder objBuilder;
                        val.BSONSerialize(objBuilder);
                        fmt::format_to(std::back_inserter(buffer), "{}: ", key);
                        objBuilder.done().jsonStringBuffer(
                            JsonStringFormat::ExtendedRelaxedV2_0_0, 0, false, buffer);
                    } else if (val.BSONAppend) {
                        BSONObjBuilder objBuilder;
                        val.BSONAppend(objBuilder, ""_sd);
                        fmt::format_to(std::back_inserter(buffer), "{}: ", key);
                        objBuilder.done().getField(""_sd).jsonStringBuffer(
                            JsonStringFormat::ExtendedRelaxedV2_0_0, false, false, 0, buffer);
                    } else {
                        fmt::format_to(std::back_inserter(buffer), "{}: ", key);
                        val.toBSONArray().jsonStringBuffer(
                            JsonStringFormat::ExtendedRelaxedV2_0_0, 0, true, buffer);
                    }
                } else if constexpr (isDuration<std::decay_t<decltype(val)>>) {
                    fmt::format_to(std::back_inserter(buffer), "{}: {}", key, val.toString());
                } else if constexpr (std::is_same_v<std::decay_t<decltype(val)>, BSONObj>) {
                    fmt::format_to(std::back_inserter(buffer), "{}: ", key);
                    val.jsonStringBuffer(JsonStringFormat::ExtendedRelaxedV2_0_0, 0, false, buffer);
                } else if constexpr (std::is_same_v<std::decay_t<decltype(val)>, BSONArray>) {
                    fmt::format_to(std::back_inserter(buffer), "{}: ", key);
                    val.jsonStringBuffer(JsonStringFormat::ExtendedRelaxedV2_0_0, 0, true, buffer);
                } else {
                    fmt::format_to(std::back_inserter(buffer), "{}: {}", key, val);
                }
            };

            auto key = mapValue(item.first);
            using value_t = std::decay_t<decltype(item.second)>;
            if constexpr (isOptional<value_t>) {
                if (item.second) {
                    append(key, mapValue(*item.second));
                } else {
                    append(key, mapValue(boost::none));
                }
            } else {
                append(key, mapValue(item.second));
            }

            separator = ", "_sd;
        }
        buffer.push_back(')');
    }

private:
    It _begin;
    It _end;
};

// Named attribute, storage for a name-value attribute.
class NamedAttribute {
public:
    NamedAttribute() = default;
    NamedAttribute(const char* n, long double val) = delete;

    template <typename T>
    NamedAttribute(const char* n, const boost::optional<T>& val)
        : NamedAttribute(val ? NamedAttribute(n, *val) : NamedAttribute()) {
        if (!val) {
            name = n;
            value = mapValue(boost::none);
        }
    }

    template <typename T>
    NamedAttribute(const char* n, const T& val) : name(n), value(mapValue(val)) {}

    const char* name = nullptr;
    std::variant<int,
                 unsigned int,
                 long long,
                 unsigned long long,
                 bool,
                 double,
                 StringData,
                 Nanoseconds,
                 Microseconds,
                 Milliseconds,
                 Seconds,
                 Minutes,
                 Hours,
                 Days,
                 BSONObj,
                 BSONArray,
                 CustomAttributeValue>
        value;
};

// Attribute Storage stores an array of Named Attributes.
template <typename... Args>
class AttributeStorage {
public:
    AttributeStorage(const Args&... args)
        : _data{detail::NamedAttribute(args.name, args.value)...} {}

private:
    static const size_t kNumArgs = sizeof...(Args);

    // Arrays need a size of at least 1, add dummy element if needed (kNumArgs above is still 0)
    NamedAttribute _data[kNumArgs ? kNumArgs : 1];

    // This class is meant to be wrapped by TypeErasedAttributeStorage below that provides public
    // accessors. Let it access all our internals.
    friend class mongo::logv2::TypeErasedAttributeStorage;
};

}  // namespace detail

class DynamicAttributes {
public:
    DynamicAttributes() = default;

    /**
     * This constructor allows users to construct DynamicAttributes in the same style as normal
     * LOGV2 calls. Example:
     *
     * DynamicAttributes(
     *    DynamicAttributes{}, // Something that can be returned by a function
     *    "attr1"_attr = val1,
     *    "attr2"_attr = val2
     * )
     *
     * This can be useful for classes that want to provide a set of basic attributes for sub-classes
     * to extend with their own attributes.
     */
    template <typename... Args>
    DynamicAttributes(DynamicAttributes&& other, Args&&... args)
    requires(detail::IsNamedArg<Args> && ...)
        : _attributes(std::move(other._attributes)),
          _copiedStrings(std::move(other._copiedStrings)) {
        (add(std::forward<Args>(args)), ...);
    }

    // Do not allow rvalue references and temporary objects to avoid lifetime problem issues
    template <size_t N,
              typename T,
              std::enable_if_t<std::is_arithmetic_v<T> || std::is_pointer_v<T> ||
                                   std::is_enum_v<T> || detail::isDuration<T>,
                               int> = 0>
    void add(const char (&name)[N], T value) {
        _attributes.emplace_back(name, value);
    }

    template <size_t N>
    void add(const char (&name)[N], BSONObj value) {
        BSONObj owned = value.getOwned();
        _attributes.emplace_back(name, owned);
    }

    template <size_t N>
    void add(const char (&name)[N], BSONArray value) {
        BSONArray owned = static_cast<BSONArray>(value.getOwned());
        _attributes.emplace_back(name, owned);
    }

    template <size_t N,
              typename T,
              std::enable_if_t<std::is_class_v<T> && !detail::isDuration<T>, int> = 0>
    void add(const char (&name)[N], const T& value) {
        _attributes.emplace_back(name, value);
    }

    template <size_t N,
              typename T,
              std::enable_if_t<std::is_class_v<T> && !detail::isDuration<T>, int> = 0>
    void add(const char (&name)[N], T&& value) = delete;

    template <size_t N>
    void add(const char (&name)[N], StringData value) {
        _attributes.emplace_back(name, value);
    }

    // Deep copies the string instead of taking it by reference
    template <size_t N>
    void addDeepCopy(const char (&name)[N], std::string value) {
        _copiedStrings.push_front(std::move(value));
        add(name, StringData(_copiedStrings.front()));
    }

    // Does not have the protections of add() above. Be careful about lifetime of value!
    template <size_t N, typename T>
    void addUnsafe(const char (&name)[N], const T& value) {
        _attributes.emplace_back(name, value);
    }

private:
    template <typename T>
    void add(const detail::NamedArg<T>& namedArg) {
        _attributes.emplace_back(namedArg.name, namedArg.value);
    }

    // This class is meant to be wrapped by TypeErasedAttributeStorage below that provides public
    // accessors. Let it access all our internals.
    friend class mongo::logv2::TypeErasedAttributeStorage;

    boost::container::small_vector<detail::NamedAttribute, constants::kNumStaticAttrs> _attributes;

    // Linked list of deep copies to std::string that we can take address-of.
    std::forward_list<std::string> _copiedStrings;
};

// Wrapper around internal pointer of AttributeStorage so it does not need any template parameters
class TypeErasedAttributeStorage {
public:
    using const_iterator = const detail::NamedAttribute*;

    TypeErasedAttributeStorage() : _size(0) {}

    template <typename... Args>
    TypeErasedAttributeStorage(const detail::AttributeStorage<Args...>& store)
        : _data(store._data), _size(store.kNumArgs) {}

    TypeErasedAttributeStorage(const DynamicAttributes& attrs)
        : _data(attrs._attributes.data()), _size(attrs._attributes.size()) {}

    bool empty() const {
        return _size == 0;
    }

    std::size_t size() const {
        return _size;
    }

    const_iterator begin() const {
        return _data;
    }
    const_iterator end() const {
        return _data + _size;
    }

    // Applies a function to every stored named attribute in order they are captured
    template <typename Func>
    void apply(Func&& f) const {
        std::for_each(_data, _data + _size, [&](const auto& attr) {
            visit([&](auto&& val) { f(attr.name, val); }, attr.value);
        });
    }

private:
    const detail::NamedAttribute* _data;
    size_t _size;
};

// Helpers for logging containers, optional to use. Allowes logging of ranges.
template <typename T>
auto seqLog(const T& container) {
    using std::begin;
    using std::end;
    return detail::SequenceContainerLogger(begin(container), end(container));
}

template <typename It>
auto seqLog(It begin, It end) {
    return detail::SequenceContainerLogger(begin, end);
}

template <typename T>
auto mapLog(const T& container) {
    using std::begin;
    using std::end;
    return detail::AssociativeContainerLogger(begin(container), end(container));
}

template <typename It>
auto mapLog(It begin, It end) {
    return detail::AssociativeContainerLogger(begin, end);
}

}  // namespace mongo::logv2

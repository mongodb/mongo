// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/client/sdam/sdam_datatypes.h"
#include "mongo/client/sdam/server_description.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/optional_util.h"

#include <array>
#include <iterator>
#include <map>
#include <optional>
#include <ostream>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <boost/optional.hpp>

/**
 * The following facilitates writing tests in the Server Discovery And Monitoring (sdam) namespace.
 */
namespace mongo::sdam {

namespace test_stream_extension {

template <typename T>
struct IsStdVector : std::false_type {};
template <typename... Ts>
struct IsStdVector<std::vector<Ts...>> : std::true_type {};

template <typename T>
struct IsStdSet : std::false_type {};
template <typename... Ts>
struct IsStdSet<std::set<Ts...>> : std::true_type {};

template <typename T>
struct IsStdMap : std::false_type {};
template <typename... Ts>
struct IsStdMap<std::map<Ts...>> : std::true_type {};

template <typename T>
struct IsStdPair : std::false_type {};
template <typename... Ts>
struct IsStdPair<std::pair<Ts...>> : std::true_type {};

template <typename T>
std::ostream& stream(std::ostream& os, const T& v);

template <typename Seq>
std::ostream& streamSequence(std::ostream& os,
                             std::array<std::string_view, 2> braces,
                             const Seq& seq) {
    bool sep = false;
    os << braces[0];
    for (const auto& item : seq) {
        if (sep)
            os << ", ";
        sep = true;
        stream(os, item);
    }
    os << braces[1];
    return os;
}

template <typename T>
struct Extension {
    const T& operator*() const {
        return v;
    }

    template <typename U>
    friend bool operator==(const Extension& a, const Extension<U>& b) {
        return *a == *b;
    }
    template <typename U>
    friend bool operator!=(const Extension& a, const Extension<U>& b) {
        return *a != *b;
    }
    template <typename U>
    friend bool operator<(const Extension& a, const Extension<U>& b) {
        return *a < *b;
    }
    template <typename U>
    friend bool operator>(const Extension& a, const Extension<U>& b) {
        return *a > *b;
    }
    template <typename U>
    friend bool operator<=(const Extension& a, const Extension<U>& b) {
        return *a <= *b;
    }
    template <typename U>
    friend bool operator>=(const Extension& a, const Extension<U>& b) {
        return *a >= *b;
    }

    friend std::ostream& operator<<(std::ostream& os, const Extension& ext) {
        return stream(os, *ext);
    }

    const T& v;
};

template <typename T>
std::ostream& stream(std::ostream& os, const T& v) {
    if constexpr (IsStdVector<T>{}) {
        return streamSequence(os, {"[", "]"}, v);
    } else if constexpr (IsStdSet<T>{}) {
        return streamSequence(os, {"{", "}"}, v);
    } else if constexpr (IsStdMap<T>{}) {
        return streamSequence(os, {"{", "}"}, v);
    } else if constexpr (IsStdPair<T>{}) {
        stream(os, v.first);
        os << ": ";
        stream(os, v.second);
        return os;
    } else if constexpr (std::is_same_v<T, std::nullopt_t> || std::is_same_v<T, boost::none_t>) {
        return stream(os, "--");
    } else if constexpr (isStdOptional<T> || isBoostOptional<T>) {
        if (!v)
            return stream(os, std::nullopt);
        return stream(os << " ", *v);
    } else {
        return os << v;
    }
}

}  // namespace test_stream_extension

/**
 * Facade for use in ASSERTions. Presents pass-through relational ops and custom streaming
 * behavior around arbitrary object `v`.
 */
template <typename T>
auto adaptForAssert(const T& v) {
    return test_stream_extension::Extension<T>{v};
}

class SdamTestFixture : public unittest::Test {
protected:
    template <typename T, typename F, typename U = std::invoke_result_t<F, T>>
    std::vector<U> map(const std::vector<T>& source, const F& f) {
        std::vector<U> result;
        std::transform(source.begin(), source.end(), std::back_inserter(result), f);
        return result;
    }

    template <typename T, typename F, typename U = std::invoke_result_t<F, T>>
    std::set<U> mapSet(const std::vector<T>& source, const F& f) {
        auto v = map(source, f);
        return std::set<U>(v.begin(), v.end());
    }
};

}  // namespace mongo::sdam

/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/util/optional_util.h"

#include <boost/optional.hpp>
#include <optional>
#include <sstream>
#include <string>

#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

// Confine the nolint escaping to one place.
template <typename T>
using StdOptional = std::optional<T>;  // NOLINT
auto&& StdNullopt = std::nullopt;      // NOLINT

template <template <typename> class O, typename T, typename... As>
O<T> mkOpt(As&&... as) {
    return O<T>(std::forward<As>(as)...);
}

/** Deduce `T`, and wrap `v` in an `O<T>`, and do this `N` times. */
template <template <typename> class O, size_t N, typename T>
auto optWrap(T v) {
    if constexpr (N == 0)
        return v;
    else
        return optWrap<O, N - 1>(O<T>{std::move(v)});
}

template <typename T>
std::string str(const T& v) {
    std::ostringstream os;
    os << optional_io::Extension(v);
    return os.str();
}

MONGO_STATIC_ASSERT(isBoostOptional<boost::optional<int>>);
MONGO_STATIC_ASSERT(!isBoostOptional<int>);

MONGO_STATIC_ASSERT(!optional_io::canStream<StdOptional<int>>);
MONGO_STATIC_ASSERT(optional_io::canStreamWithExtension<StdOptional<int>>);

// boost::optional provides a SFINAE-hostile operator<<
MONGO_STATIC_ASSERT(optional_io::canStream<boost::optional<int>>);
MONGO_STATIC_ASSERT(optional_io::canStreamWithExtension<boost::optional<int>>);

TEST(OptionalUtil, ExtendedFormat) {
    ASSERT_EQ(str(123), "123");

    ASSERT_EQ(str(boost::none), "--");
    ASSERT_EQ(str(mkOpt<boost::optional, int>(123)), " 123");
    ASSERT_EQ(str(mkOpt<boost::optional, int>()), "--");

    ASSERT_EQ(str(StdNullopt), "--");
    ASSERT_EQ(str(mkOpt<StdOptional, int>(123)), " 123");
    ASSERT_EQ(str(mkOpt<StdOptional, int>()), "--");

    ASSERT_EQ(str(optWrap<boost::optional, 0>(123)), "123");
    ASSERT_EQ(str(optWrap<boost::optional, 1>(123)), " 123");
    ASSERT_EQ(str(optWrap<boost::optional, 2>(123)), "  123");

    ASSERT_EQ(str(optWrap<StdOptional, 0>(123)), "123");
    ASSERT_EQ(str(optWrap<StdOptional, 1>(123)), " 123");
    ASSERT_EQ(str(optWrap<StdOptional, 2>(123)), "  123");
}

}  // namespace
}  // namespace mongo

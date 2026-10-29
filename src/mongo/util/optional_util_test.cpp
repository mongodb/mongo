// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/optional_util.h"

#include "mongo/base/static_assert.h"
#include "mongo/unittest/unittest.h"

#include <cstddef>
#include <optional>
#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

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

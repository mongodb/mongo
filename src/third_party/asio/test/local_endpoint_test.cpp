/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/unittest/unittest.h"

#include <cstddef>
#include <iterator>

#include "asio/detail/socket_types.hpp"
#include "asio/local/detail/endpoint.hpp"
#include "asio/system_error.hpp"

namespace mongo {
namespace {

// In general, `sockaddr_un_type` is a `struct` like this:
//
//     struct sockaddr_un_type {
//         ... possibly other fields ...
//         sa_family_t sun_family;
//         ... possibly other fields ...
//         char sun_path[... size unspecified ...];
//     };
//
// Given a pointer to a `sockaddr_un_type` and some non-constant size, you want to know how many
// of the characters in `sun_path` are included in the size. There are
// `offsetof(asio::detail::sockaddr_un_type, sun_path)` bytes between the beginning of
// `sockaddr_un_type` and the beginning of `sun_path`, so if we're given a `sockaddr_un_type*`
// and a `size` of, say, 10, we know that
// `10 - offsetof(asio::detail::sockaddr_un_type, sun_path)` of the bytes in `sun_path` are
// included in the size; i.e. that is the length of the path stored in `sun_path`.
// Most of the time, there are no "other fields," and so the offset is `sizeof(sa_family_t)`,
// which is usually 2. To support the general case, use `kOffset` as a shorthand for the
// offset.
constexpr std::size_t kOffset = offsetof(asio::detail::sockaddr_un_type, sun_path);

/*
 * The `size()` of the `asio::local::detail::endpoint` object, used when calling functions like
 * `connect` and `bind`, includes the null terminator when the path actually refers to a path.
 * Linux and Windows have an extension where if the "path" starts with a null character ('\0'),
 * then the resulting "abstract" address begins with the '\0' and is not considered
 * null-terminated.
 */

/**
 * Verify that a real path (doesn't start with a null byte) results in a `.size()` that includes
 * a trailing null terminator.
 */
TEST(AsioLocalEndpointTest, RealPathEndpointSize) {
    const std::string path = "foobar";
    asio::local::detail::endpoint endpoint(path);
    ASSERT_EQ(endpoint.path(), path);

    ASSERT_EQ(endpoint.size(), kOffset + path.size() + 1);
}

/**
 * Verify that an "abstract" address (starts with a null byte) results in a `.size()` that does
 * _not_ include a trailing null terminator.
 */
TEST(AsioLocalEndpointTest, AbstractAddressEndpointSize) {
    const char abstract[] = {'\0', 'f', 'o', 'o', 'b', 'a', 'r'};
    const std::string path(std::begin(abstract), std::end(abstract));
    asio::local::detail::endpoint endpoint(path);
    ASSERT_EQ(endpoint.path(), path);

    // Since our `path` begins with `\0`, we expect the endpoint's `size()` to
    // be exactly the length of what we passed into `endpoint`'s constructor.
    ASSERT_EQ(endpoint.size(), kOffset + path.size());
}

/**
 * Verify that a path that contains only a null byte results in a `.size()` and a `.path()` that
 * includes the null byte.
 */
TEST(AsioLocalEndpointTest, MinimalAbstractAddress) {
    const char abstract[] = {'\0'};
    const std::string path(std::begin(abstract), std::end(abstract));
    asio::local::detail::endpoint endpoint(path);
    ASSERT_EQ(endpoint.path(), path);

    // A path that's just '\0' could be interpreted as an empty null-terminated
    // c-string or as an "abstract" name without any characters after the
    // initial '\0'. Either way, the path contributes only 1 byte to the
    // `size()` of the endpoint.
    ASSERT_EQ(endpoint.size(), kOffset + path.size());

    // However, for the purposes of `.path()` we have to decide whether to interpret the path as
    // "null, and then nothing" or "nothing, terminated by null." I choose the former. I figure an
    // empty file system path makes less sense than a minimal "abstract" address.
    ASSERT_EQ(endpoint.path(), path);
}

/**
 * Verify that an empty path results in an endpoint whose size does not include any path.
 */
TEST(AsioLocalEndpointTest, EmptyPath) {
    const std::string path;
    asio::local::detail::endpoint endpoint(path);
    ASSERT_EQ(endpoint.path(), path);

    // The size is just `kOffset` (since `path.size()` is zero).
    ASSERT_EQ(endpoint.size(), kOffset + path.size());
}

/**
 * Verify that `resize()` honors its size parameter regardless of whether the resulting path is
 * null terminated. This is a a consequence of `endpoint` tracking its size rather than its path
 * length.
 */
TEST(AsioLocalEndpointTest, NoSpecialLogicInResizeWhenPathEndsInNull) {
    asio::local::detail::endpoint endpoint;
    auto* const raw = reinterpret_cast<asio::detail::sockaddr_un_type*>(endpoint.data());
    raw->sun_family = AF_UNIX;
    const char path[] = {'f', 'o', 'o', '\0'};
    std::memcpy(raw->sun_path, path, sizeof path);
    endpoint.resize(kOffset + sizeof path);

    // Unpatched versions of ASIO would reduce the size of the endpoint when resized with a
    // trailing null character. The patched version does not do that -- instead, reckoning about
    // null terminators happens when `.path()` is requested.
    ASSERT(endpoint.size() == kOffset + sizeof path);
}

/**
 * Verify that creating an endpoint with an oversized path causes an exception to be thrown.
 */
TEST(AsioLocalEndpointTest, OversizedPathThrows) {
    // There are `sizeof(asio::detail::sockaddr_un_type::sun_path) - 1` bytes available, because
    // one is reserved for a potential null terminator.
    constexpr std::size_t max = sizeof(asio::detail::sockaddr_un_type::sun_path) - 1;

    const auto rightsized = []() {
        std::string okPath(max, 'z');
        asio::local::detail::endpoint endpoint(okPath);
    };
    ASSERT_DOES_NOT_THROW(rightsized());

    const auto oversized = []() {
        std::string oversizedPath(max + 1, 'z');
        asio::local::detail::endpoint endpoint(oversizedPath);
    };
    ASSERT_THROWS(oversized(), asio::system_error);
}

/**
 * Verify that resizing an endpoint beyond its maximum size causes an exception to be thrown.
 */
TEST(AsioLocalEndpointTest, OverresizedPathThrows) {
    // `asio::local::detail::endpoint is a wrapper for `asio::detail::sockaddr_un_type`.
    constexpr std::size_t max = sizeof(asio::detail::sockaddr_un_type);

    const auto rightsized = []() {
        asio::local::detail::endpoint endpoint;
        endpoint.resize(max);
    };
    ASSERT_DOES_NOT_THROW(rightsized());

    const auto oversized = []() {
        asio::local::detail::endpoint endpoint;
        endpoint.resize(max + 1);
    };
    ASSERT_THROWS(oversized(), asio::system_error);
}

}  // namespace
}  // namespace mongo

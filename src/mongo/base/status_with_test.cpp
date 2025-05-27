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

#include "mongo/base/status_with.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/unittest/stringify.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>

namespace mongo {
namespace {

TEST(StatusWith, MakeCtad) {
    auto validate = [](auto&& arg) {
        auto sw = StatusWith(arg);
        ASSERT_TRUE(sw.isOK());
        ASSERT_TRUE(uassertStatusOK(sw) == arg);
        using Arg = std::decay_t<decltype(arg)>;
        return std::is_same_v<decltype(sw), StatusWith<Arg>>;
    };
    ASSERT_TRUE(validate(3));
    ASSERT_TRUE(validate(false));
    ASSERT_TRUE(validate(123.45));
    ASSERT_TRUE(validate(std::string("foo")));
    ASSERT_TRUE(validate(std::vector<int>()));
    ASSERT_TRUE(validate(std::vector<int>({1, 2, 3})));
}

/** Check uassertStatusOK with various reference types */
TEST(StatusWith, UassertStatusOKReferenceTypes) {
    auto sd = "barbaz"_sd;
    auto sw = StatusWith(sd);

    const StatusWith<StringData>& cref = sw;
    ASSERT_EQUALS(uassertStatusOK(cref), sd);

    StatusWith<StringData>& ncref = sw;
    ASSERT_EQUALS(uassertStatusOK(ncref), sd);

    StatusWith<StringData>&& rref = std::move(sw);
    ASSERT_EQUALS(uassertStatusOK(std::move(rref)), sd);
}

TEST(StatusWith, nonDefaultConstructible) {
    class NoDefault {
    public:
        NoDefault() = delete;
        NoDefault(int x) : x{x} {}
        int x;
    };

    auto swND = StatusWith(NoDefault(1));
    ASSERT_EQ(swND.getValue().x, 1);

    auto swNDerror = StatusWith<NoDefault>(ErrorCodes::BadValue, "foo");
    ASSERT_FALSE(swNDerror.isOK());
}

TEST(StatusWith, ignoreTest) {
    // A compile-only test
    [] {
        return StatusWith(false);
    }()
        .getStatus()
        .ignore();
}

TEST(StatusWith, AssertionFormat) {
    Status failed(ErrorCodes::CallbackCanceled, "foo");
    ASSERT_EQ(unittest::stringify::invoke(StatusWith<StringData>(failed)),
              unittest::stringify::invoke(failed));
    ASSERT_EQ(unittest::stringify::invoke(StatusWith<StringData>("foo")), "foo");
}

}  // namespace
}  // namespace mongo

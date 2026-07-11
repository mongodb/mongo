// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/status_with.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/unittest/stringify.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

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
    auto sd = "barbaz"sv;
    auto sw = StatusWith(sd);

    const StatusWith<std::string_view>& cref = sw;
    ASSERT_EQUALS(uassertStatusOK(cref), sd);

    StatusWith<std::string_view>& ncref = sw;
    ASSERT_EQUALS(uassertStatusOK(ncref), sd);

    StatusWith<std::string_view>&& rref = std::move(sw);
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
    ASSERT_EQ(unittest::stringify::invoke(StatusWith<std::string_view>(failed)),
              unittest::stringify::invoke(failed));
    ASSERT_EQ(unittest::stringify::invoke(StatusWith<std::string_view>("foo")), "foo");
}

}  // namespace
}  // namespace mongo

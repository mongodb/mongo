// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/overloaded_visitor.h"  // IWYU pragma: keep

#include "mongo/unittest/unittest.h"

#include <string>
#include <variant>

namespace mongo {
namespace {

TEST(OverloadedVisitorTest, StdxVisit) {
    auto doVisit = [&](const std::variant<int, std::string>& var) {
        return visit(OverloadedVisitor{
                         [](int v) { return 1; },
                         [](const std::string& v) { return 2; },
                     },
                     var);
    };
    ASSERT_EQ(doVisit(123), 1);
    ASSERT_EQ(doVisit(std::string("hi")), 2);
}

TEST(OverloadedVisitorTest, Fallback) {
    auto doVisit = [&](const std::variant<int, std::string>& var) {
        return visit(OverloadedVisitor{
                         [](int v) { return 1; },
                         [](auto&& v) { return 2; },
                     },
                     var);
    };
    ASSERT_EQ(doVisit(123), 1);
    ASSERT_EQ(doVisit(std::string("hi")), 2);
}

TEST(OverloadedVisitorTest, IntegerRank) {
    auto doVisit = [&](const std::variant<int, long, long long>& var) {
        return visit(OverloadedVisitor{
                         [](long long v) { return 1; },
                         [](long v) { return 2; },
                         [](int v) { return 3; },
                     },
                     var);
    };
    ASSERT_EQ(doVisit(123LL), 1);
    ASSERT_EQ(doVisit(123L), 2);
    ASSERT_EQ(doVisit(123), 3);
}

TEST(OverloadedVisitorTest, MultiVisit) {
    std::variant<int, std::string> var1;
    std::variant<int, std::string> var2;
    auto doVisit = [&](const std::variant<int, std::string>& a,
                       const std::variant<int, std::string, double>& b) {
        return visit(OverloadedVisitor{
                         [](int a, int b) { return 0; },
                         [](int a, const std::string& b) { return 1; },
                         [](int a, double b) { return 2; },
                         [](const std::string& a, int b) { return 3; },
                         [](const std::string& a, const std::string& b) { return 4; },
                         [](const std::string& a, double b) { return 5; },
                     },
                     a,
                     b);
    };
    ASSERT_EQ(doVisit(123, 123), 0);
    ASSERT_EQ(doVisit(123, "b"), 1);
    ASSERT_EQ(doVisit(123, 0.5), 2);
    ASSERT_EQ(doVisit("a", 123), 3);
    ASSERT_EQ(doVisit("a", "b"), 4);
    ASSERT_EQ(doVisit("a", 0.5), 5);
}

}  // namespace
}  // namespace mongo

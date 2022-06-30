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

#include "mongo/util/overloaded_visitor.h"

#include <string>
#include <utility>

#include "mongo/stdx/variant.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(OverloadedVisitorTest, StdxVisit) {
    auto doVisit = [&](const stdx::variant<int, std::string>& var) {
        return stdx::visit(
            OverloadedVisitor{
                [](int v) { return 1; },
                [](const std::string& v) { return 2; },
            },
            var);
    };
    ASSERT_EQ(doVisit(123), 1);
    ASSERT_EQ(doVisit(std::string("hi")), 2);
}

TEST(OverloadedVisitorTest, Fallback) {
    auto doVisit = [&](const stdx::variant<int, std::string>& var) {
        return stdx::visit(
            OverloadedVisitor{
                [](int v) { return 1; },
                [](auto&& v) { return 2; },
            },
            var);
    };
    ASSERT_EQ(doVisit(123), 1);
    ASSERT_EQ(doVisit(std::string("hi")), 2);
}

TEST(OverloadedVisitorTest, IntegerRank) {
    auto doVisit = [&](const stdx::variant<int, long, long long>& var) {
        return stdx::visit(
            OverloadedVisitor{
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
    stdx::variant<int, std::string> var1;
    stdx::variant<int, std::string> var2;
    auto doVisit = [&](const stdx::variant<int, std::string>& a,
                       const stdx::variant<int, std::string, double>& b) {
        return stdx::visit(
            OverloadedVisitor{
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

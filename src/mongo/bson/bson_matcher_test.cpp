/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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


#include "mongo/bson/bson_matcher.h"

#include "mongo/unittest/assert.h"
#include "mongo/unittest/unittest.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::logv2::LogComponent::kTest

namespace mongo {
namespace {

using namespace unittest::match;

TEST(BSONMatcherTest, BSONObjComparisonMatcher) {
    auto a = BSONObjBuilder{}.append("foo", "bar").obj();
    auto b = BSONObjBuilder{}.append("foo", "baz").obj();
    ASSERT_THAT(a, BSONObjEQ(a));
    ASSERT_THAT(a, BSONObjNE(b));
    ASSERT_THAT(a, BSONObjLT(b));
    ASSERT_THAT(a, BSONObjLE(b));
    ASSERT_THAT(b, BSONObjLE(b));
    ASSERT_THAT(b, BSONObjGT(a));
    ASSERT_THAT(b, BSONObjGE(a));
    ASSERT_THAT(a, BSONObjGE(a));
}

TEST(BSONMatcherTest, BSONObjComparisonMatcherDescription) {
    auto a = BSONObjBuilder{}.append("foo", "bar").obj();
    std::string aDesc = R"({ foo: "bar" })";
    auto testCmp = [&](auto&& m, std::string op) {
        ASSERT_EQ(testing::DescribeMatcher<BSONObj>(m),
                  fmt::format("is a BSONObj {} {}", op, aDesc));
    };
    testCmp(BSONObjEQ(a), "equal to");
    testCmp(BSONObjNE(a), "not equal to");
    testCmp(BSONObjLT(a), "<");
    testCmp(BSONObjGT(a), ">");
    testCmp(BSONObjLE(a), "<=");
    testCmp(BSONObjGE(a), ">=");
}

TEST(BSONMatcherTest, BSONObjComparisonUnorderedMatcher) {
    auto a = BSONObjBuilder{}.append("foo", "bar").append("hello", "world").obj();
    auto b = BSONObjBuilder{}.append("hello", "world").append("foo", "bar").obj();
    ASSERT_THAT(a, BSONObjNE(b));
    ASSERT_THAT(a, BSONObjUnorderedEQ(b));
    ASSERT_THAT(a, BSONObjUnorderedLE(b));
    ASSERT_THAT(b, BSONObjUnorderedLE(a));
    ASSERT_THAT(b, BSONObjUnorderedGE(a));
    ASSERT_THAT(a, BSONObjUnorderedGE(b));

    auto c = BSONObjBuilder{}.append("hello", "world").append("foo", "baz").obj();
    ASSERT_THAT(a, BSONObjUnorderedNE(c));
    ASSERT_THAT(b, BSONObjUnorderedNE(c));
    ASSERT_THAT(a, BSONObjUnorderedLT(c));
    ASSERT_THAT(b, BSONObjUnorderedLT(c));
    ASSERT_THAT(c, BSONObjUnorderedGT(b));
    ASSERT_THAT(c, BSONObjUnorderedGT(a));
}

TEST(BSONMatcherTest, BSONObjComparisonUnorderedMatcherDescription) {
    auto a = BSONObjBuilder{}.append("foo", "bar").append("hello", "world").obj();
    std::string aDesc = R"({ foo: "bar", hello: "world" })";
    auto testCmp = [&](auto&& m, std::string op) {
        ASSERT_EQ(testing::DescribeMatcher<BSONObj>(m),
                  fmt::format("is a BSONObj unordered {} {}", op, aDesc));
    };
    testCmp(BSONObjUnorderedEQ(a), "equal to");
    testCmp(BSONObjUnorderedNE(a), "not equal to");
    testCmp(BSONObjUnorderedLT(a), "<");
    testCmp(BSONObjUnorderedGT(a), ">");
    testCmp(BSONObjUnorderedLE(a), "<=");
    testCmp(BSONObjUnorderedGE(a), ">=");
}

TEST(BSONMatcherTest, BSONElementComparisonsMatcher) {
    auto ao = BSONObjBuilder{}.append("foo", "bar").obj();
    auto bo = BSONObjBuilder{}.append("foo", "baz").obj();
    auto a = ao.firstElement();
    auto b = bo.firstElement();

    ASSERT_THAT(a, BSONElementEQ(a));
    ASSERT_THAT(a, BSONElementNE(b));
    ASSERT_THAT(a, BSONElementLT(b));
    ASSERT_THAT(a, BSONElementLE(a));
    ASSERT_THAT(a, BSONElementLE(b));
    ASSERT_THAT(b, BSONElementGT(a));
    ASSERT_THAT(b, BSONElementGE(a));
    ASSERT_THAT(a, BSONElementGE(a));
}

TEST(BSONMatcherTest, BSONElementComparisonsMatcherDescription) {
    auto ao = BSONObjBuilder{}.append("foo", "bar").obj();
    auto a = ao.firstElement();
    std::string aDesc = R"(foo: "bar")";
    auto testCmp = [&](auto&& m, std::string op) {
        ASSERT_EQ(testing::DescribeMatcher<BSONElement>(m),
                  fmt::format("is a BSONElement {} {}", op, aDesc));
    };
    testCmp(BSONElementEQ(a), "equal to");
    testCmp(BSONElementNE(a), "not equal to");
    testCmp(BSONElementLT(a), "<");
    testCmp(BSONElementGT(a), ">");
    testCmp(BSONElementLE(a), "<=");
    testCmp(BSONElementGE(a), ">=");
}

TEST(BSONMatcherTest, BSONElementComparisonUnorderedMatcher) {
    auto oa = BSONObjBuilder{}.append("baz", BSON("foo" << "bar" << "baz" << "a")).obj();
    auto ob = BSONObjBuilder{}.append("baz", BSON("baz" << "a" << "foo" << "bar")).obj();
    auto a = oa["baz"];
    auto b = ob["baz"];

    ASSERT_THAT(a, BSONElementUnorderedEQ(b));
    ASSERT_THAT(a, Not(BSONElementUnorderedNE(b)));
    ASSERT_THAT(a, BSONElementUnorderedLE(b));
    ASSERT_THAT(b, BSONElementUnorderedLE(a));
    ASSERT_THAT(a, Not(BSONElementUnorderedLT(b)));
    ASSERT_THAT(b, BSONElementUnorderedGE(a));
    ASSERT_THAT(a, BSONElementUnorderedGE(b));
    ASSERT_THAT(a, Not(BSONElementUnorderedGT(b)));
}

TEST(BSONMatcherTest, BSONElementComparisonUnorderedMatcherDescription) {
    auto oa = BSONObjBuilder{}.append("baz", BSON("foo" << "bar" << "baz" << "a")).obj();
    auto a = oa["baz"];
    std::string aDesc = R"(baz: { foo: "bar", baz: "a" })";
    auto testCmp = [&](auto&& m, std::string op) {
        ASSERT_EQ(testing::DescribeMatcher<BSONElement>(m),
                  fmt::format("is a BSONElement unordered {} {}", op, aDesc));
    };
    testCmp(BSONElementUnorderedEQ(a), "equal to");
    testCmp(BSONElementUnorderedNE(a), "not equal to");
    testCmp(BSONElementUnorderedLT(a), "<");
    testCmp(BSONElementUnorderedGT(a), ">");
    testCmp(BSONElementUnorderedLE(a), "<=");
    testCmp(BSONElementUnorderedGE(a), ">=");
}

TEST(BSONMatcherTest, BSONObjElements) {
    auto a = BSONObjBuilder{}.append("foo", "bar").append("hello", "world").obj();
    auto ae1 = a.getField("foo");
    auto ae2 = a.getField("hello");
    auto b = BSONObjBuilder{}.append("foo", "bar").append("baz", "foo").obj();
    auto be1 = b.getField("foo");
    auto be2 = b.getField("baz");
    ASSERT_THAT(a, BSONObjElements(Contains(BSONElementEQ(ae1))));
    ASSERT_THAT(a, BSONObjElements(Contains(BSONElementEQ(ae2))));
    ASSERT_THAT(a, BSONObjElements(Contains(BSONElementEQ(be1))));
    ASSERT_THAT(a, Not(BSONObjElements(Contains(BSONElementEQ(be2)))));
}

TEST(BSONMatcherTest, BSONObjElementsNested) {
    auto obj = BSONObjBuilder{}.append("baz", BSON("foo" << "bar")).obj();
    auto el = obj.firstElement().Obj().firstElement();
    ASSERT_THAT(obj,
                BSONObjElements(Contains(IsBSONElement(
                    "baz",
                    BSONType::object,
                    Matcher<BSONObj>(BSONObjElements(Contains(BSONElementEQ(el))))))));
}

TEST(BSONMatcherTest, BSONObjElementsNestedDescription) {
    auto obj = BSONObjBuilder{}.append("foo", "bar").obj();
    auto el = obj.firstElement();
    ASSERT_EQ(testing::DescribeMatcher<BSONElement>(
                  IsBSONElement("baz",
                                BSONType::object,
                                Matcher<BSONObj>(BSONObjElements(Contains(BSONElementEQ(el)))))),
              "is a BSONElement which has name which is equal to \"baz\" and type which is equal "
              "to object and value which is a BSONObj whose elements contains at least one "
              "element that is a BSONElement equal to foo: \"bar\"");
}

TEST(BSONMatcherTest, IsBSONElement) {
    auto a = BSONObjBuilder{}.append("foo", "bar").obj();

    ASSERT_THAT(a,
                BSONObjElements(
                    Contains(IsBSONElement("foo", BSONType::string, Matcher<std::string>{"bar"}))));
    ASSERT_THAT(
        a,
        Not(BSONObjElements(Contains(IsBSONElement("foo", BSONType::string, Matcher<int>{123})))));
    ASSERT_THAT(a,
                BSONObjElements(Contains(
                    Not(IsBSONElement("foo", BSONType::string, Matcher<std::string>{"baz"})))));
}

TEST(BSONMatcherTest, IsBSONElementUnrepresentable) {
    auto a = BSONObjBuilder{}.obj();
    ASSERT_THAT(a["foo"], IsBSONElement("", BSONType::eoo, _));
}

TEST(BSONMatcherTest, IsBSONElementDescription) {
    ASSERT_EQ(
        DescribeMatcher<BSONObj>(BSONObjElements(
            Contains(IsBSONElement("foo", BSONType::string, Matcher<std::string>{"bar"})))),
        "is a BSONObj whose elements contains at least one element that is a BSONElement which has "
        "name which is equal to \"foo\" and type which is equal to string and value which is equal "
        "to \"bar\"");
}

TEST(BSONMatcherTest, IsBSONElementContainsDescription) {
    ASSERT_EQ(
        DescribeMatcher<BSONObj>(Not(
            BSONObjElements(Contains(IsBSONElement("foo", BSONType::string, Matcher<int>{123}))))),
        "is a BSONObj whose elements doesn't contain any element that is a BSONElement which "
        "has name which is equal to \"foo\" and type which is equal to string and value which is "
        "equal to 123");
}

TEST(BSONMatcherTest, IsBSONElementContainsNegationDescription) {
    ASSERT_EQ(
        DescribeMatcher<BSONObj>(BSONObjElements(
            Contains(Not(IsBSONElement("foo", BSONType::string, Matcher<std::string>{"baz"}))))),
        "is a BSONObj whose elements contains at least one element that is a BSONElement which not "
        "(has name which is equal to \"foo\" and type which is equal to string and value which is "
        "equal to \"baz\")");
}

}  // namespace
}  // namespace mongo

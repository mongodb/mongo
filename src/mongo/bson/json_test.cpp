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

#include "mongo/bson/json.h"

#include "mongo/bson/bsonobj_comparator.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/overloaded_visitor.h"

#include <stack>

namespace mongo {

namespace {
/**
 * Template to help make deeply-nested JSON objects. At each step, f is given a Builder and the
 * current step number. f may append fields to the Builder, and then must return another
 * Builder representing a subobject or subarray of the one it was given.
 */
template <typename Builder>
Builder makeNested(int depth, std::function<Builder(Builder&, int)> f) {
    Builder topBuilder;
    std::stack<Builder> builders;
    builders.push(f(topBuilder, 0));

    for (int i = 1; i < depth; i++) {
        auto& builder = builders.top();
        builders.push(f(builder, i));
    }

    while (!builders.empty()) {
        builders.pop();
    }

    return topBuilder;
}

/**
 * Returns a nested sequence of objects, represented in JSON like so:
 * { obj_depth_0: { obj_depth_1: { ... } } }
 */
BSONObj makeNestedObjects(int depth) {
    return makeNested<BSONObjBuilder>(
               depth,
               [](BSONObjBuilder& builder, int i) {
                   return BSONObjBuilder{builder.subobjStart("obj_depth_" + std::to_string(i))};
               })
        .obj();
}

/**
 * Returns a nested sequence of arrays, represented in JSON like so: [[[]]].
 */
BSONObj makeEmptyNestedArrays(int depth) {
    return makeNested<BSONArrayBuilder>(depth,
                                        [](BSONArrayBuilder& builder, int i) {
                                            return BSONArrayBuilder{builder.subarrayStart()};
                                        })
        .obj();
}

/**
 * Returns a nested sequence of arrays like those returned by makeEmptyNestedArrays, but with
 * additional elements, represented in JSON like so:
 *      [ {$numberInt: 0}, "string_value", ...,
 *          [ {$numberInt: 1}, "string_value", ...,
 *              [...]
 *          ]
 *      ]
 */
BSONObj makeNonEmptyNestedArrays(int depth) {
    return makeNested<BSONArrayBuilder>(depth,
                                        [&](BSONArrayBuilder& builder, int i) {
                                            BSONArrayBuilder arr = builder.subarrayStart();
                                            // We need to respect the caller's depth limit with
                                            // respect to the JSON this BSONObj will generate. If we
                                            // add an integer at the last level, it will serialize
                                            // to JSON as an object like {$numberInt: N}, exceeding
                                            // the requested depth by one.
                                            if (i < depth - 1) {
                                                arr.append(i);
                                            }
                                            arr.appendBool(true);
                                            arr.append("string_value");
                                            return arr;
                                        })
        .obj();
}

/**
 * Returns a sequence of nested arrays and objects that don't contain any extraneous fields,
 * represented in JSON like so:
 * { arr_depth_0: [ { arr_depth_2: [...] } ] }
 */
BSONObj makeMinimalNestedArraysAndObjects(int depth) {
    using VariantBuilder = std::variant<BSONObjBuilder, BSONArrayBuilder>;
    auto builder = makeNested<VariantBuilder>(depth, [](VariantBuilder& builder, int i) {
        return std::visit(OverloadedVisitor{[&](BSONObjBuilder& objBuilder) {
                                                BSONArrayBuilder arr(objBuilder.subarrayStart(
                                                    "arr_depth_" + std::to_string(i)));
                                                return VariantBuilder{std::move(arr)};
                                            },
                                            [&](BSONArrayBuilder& arrBuilder) {
                                                BSONObjBuilder obj(arrBuilder.subobjStart());
                                                return VariantBuilder{std::move(obj)};
                                            }},
                          builder);
    });

    return std::visit([](auto&& builder) { return builder.obj(); }, builder);
}

/**
 * Returns a nested sequence of objects and arrays similar to the ones returned by
 * makeMinimalNestedArraysAndObjects, but with additional fields and elements.
 */
BSONObj makeNonMinimalNestedArraysAndObjects(int depth) {
    using VariantBuilder = std::variant<BSONObjBuilder, BSONArrayBuilder>;
    auto builder = makeNested<VariantBuilder>(depth, [](VariantBuilder& builder, int i) {
        return std::visit(OverloadedVisitor{[&](BSONObjBuilder& objBuilder) {
                                                objBuilder.append("string_field", "string_value");
                                                BSONArrayBuilder arr(objBuilder.subarrayStart(
                                                    "arr_depth_" + std::to_string(i)));
                                                return VariantBuilder{std::move(arr)};
                                            },
                                            [&](BSONArrayBuilder& arrBuilder) {
                                                arrBuilder.append("string_value");
                                                BSONObjBuilder obj(arrBuilder.subobjStart());
                                                return VariantBuilder{std::move(obj)};
                                            }},
                          builder);
    });

    return std::visit([](auto&& builder) { return builder.obj(); }, builder);
}

std::string makeNestedJSONArray(int depth) {
    return tojson(makeEmptyNestedArrays(depth));
}

StatusWith<BSONObj> parse(const std::string& jsonString) {
    BSONObjBuilder builder;
    JParse jparse(jsonString);
    Status status = jparse.parse(builder);
    if (!status.isOK()) {
        return status;
    }
    return builder.obj();
}

void testRoundTrip(BSONObj obj) {
    auto parseResult = parse(tojson(obj));
    ASSERT_OK(parseResult);
    BSONObj roundTripped = parseResult.getValue();
    BSONObjComparator bsonCmp({}, BSONObjComparator::FieldNamesMode::kConsider, nullptr);
    ASSERT_EQ(bsonCmp.compare(obj, roundTripped), 0);
}

void testParseFailure(BSONObj obj) {
    auto parseResult = parse(tojson(obj));
    ASSERT_NOT_OK(parseResult);
    ASSERT_EQUALS(parseResult.getStatus().code(), ErrorCodes::FailedToParse);
}

}  // namespace

TEST(JParseTest, NestedObjectsRoundTrip) {
    testRoundTrip(makeNestedObjects(5));
    testRoundTrip(makeNestedObjects(JParse::kMaxDepth));
}

TEST(JParseTest, NestedArraysRoundTrip) {
    testRoundTrip(makeEmptyNestedArrays(5));
    testRoundTrip(makeEmptyNestedArrays(JParse::kMaxDepth));
    testRoundTrip(makeNonEmptyNestedArrays(5));
    testRoundTrip(makeNonEmptyNestedArrays(JParse::kMaxDepth));
}

TEST(JParseTest, NestedArraysAndObjectsRoundTrip) {
    testRoundTrip(makeMinimalNestedArraysAndObjects(5));
    testRoundTrip(makeMinimalNestedArraysAndObjects(JParse::kMaxDepth));
    testRoundTrip(makeNonMinimalNestedArraysAndObjects(5));
    testRoundTrip(makeNonMinimalNestedArraysAndObjects(JParse::kMaxDepth));
}

TEST(JParseTest, DeeplyNestedObjects) {
    testParseFailure(makeNestedObjects(JParse::kMaxDepth + 1));
}

TEST(JParseTest, DeeplyNestedEmptyArrays) {
    testParseFailure(makeEmptyNestedArrays(JParse::kMaxDepth + 1));
}

TEST(JParseTest, DeeplyNestedNonEmptyArrays) {
    testParseFailure(makeNonEmptyNestedArrays(JParse::kMaxDepth + 1));
}

TEST(JParseTest, DeeplyNestedMinimalArraysAndObjects) {
    testParseFailure(makeMinimalNestedArraysAndObjects(JParse::kMaxDepth + 1));
}

TEST(JParseTest, DeeplyNestedNonMinimalArraysAndObjects) {
    testParseFailure(makeNonMinimalNestedArraysAndObjects(JParse::kMaxDepth + 1));
}

TEST(JParseTest, InvalidDBRefErrors) {
    std::string prologue = R"({"obj": DBRef("ref", {"$id": )";
    std::string epilogue = R"(}, "db")})";
    std::string goodJsonString = prologue + makeNestedJSONArray(5) + epilogue;
    ASSERT_OK(parse(goodJsonString));

    std::string badJsonString = prologue + makeNestedJSONArray(JParse::kMaxDepth + 1) + epilogue;
    auto parseResult = parse(badJsonString);
    ASSERT_NOT_OK(parseResult);
    ASSERT_EQUALS(ErrorCodes::FailedToParse, parseResult.getStatus().code());
}

TEST(JParseTest, InvalidDBRefObjectErrors) {
    std::string prologue = R"({"obj": {"$ref": "ns", "$id": )";
    std::string epilogue = R"(, "$db": "db"}})";
    std::string goodJsonString = prologue + makeNestedJSONArray(5) + epilogue;
    ASSERT_OK(parse(goodJsonString));

    std::string badJsonString = prologue + makeNestedJSONArray(JParse::kMaxDepth + 1) + epilogue;
    auto parseResult = parse(badJsonString);
    ASSERT_NOT_OK(parseResult);
    ASSERT_EQUALS(ErrorCodes::FailedToParse, parseResult.getStatus().code());
}

TEST(JParseTest, FailEarlyOnMissingOpeningBrace) {
    auto testEarlyParseFailure = [](StringData jsonString) -> void {
        BSONObjBuilder builder;
        JParse jparse(jsonString);
        Status status = jparse.parse(builder);
        ASSERT(status == ErrorCodes::FailedToParse);
        ASSERT(status.reason().find("Expecting '{'") != status.reason().npos);

        // If the builder is nonempty, parsing succeeded on the inner object and only failed
        // subsequently while finishing the outer object. We want parsing to fail as soon as we know
        // that the whole object is invalid.
        BSONObjComparator bsonCmp({}, BSONObjComparator::FieldNamesMode::kConsider, nullptr);
        ASSERT_EQ(bsonCmp.compare(builder.obj(), BSONObj()), 0);
    };

    testEarlyParseFailure(R"({"obj": {"$timestamp": "t": 0, "i": 0})");
    testEarlyParseFailure(R"({"obj": {"$regularExpression": "pattern": "", "options": ""})");
}

TEST(JParseTest, FailEarlyOnMissingClosingBrace) {
    auto testEarlyParseFailure = [](StringData jsonString) -> void {
        BSONObjBuilder builder;
        JParse jparse(jsonString);
        Status status = jparse.parse(builder);
        ASSERT(status == ErrorCodes::FailedToParse);
        ASSERT(status.reason().find("Expecting '}'") != status.reason().npos);

        // If the builder is nonempty, parsing succeeded on the inner object and only failed
        // subsequently while finishing the outer object. We want parsing to fail as soon as we know
        // that the whole object is invalid.
        BSONObjComparator bsonCmp({}, BSONObjComparator::FieldNamesMode::kConsider, nullptr);
        ASSERT_EQ(bsonCmp.compare(builder.obj(), BSONObj()), 0);
    };

    testEarlyParseFailure(R"({"obj": {"$binary": {"base64": "", "subType": "1" badTok)");
    testEarlyParseFailure(R"({"obj": {"$date": {"$numberLong": "0" badTok)");
    testEarlyParseFailure(R"({"obj": {"$timestamp": {"t": 0, "i": 0 badTok)");
    testEarlyParseFailure(R"({"obj": {"$regularExpression": {"pattern": "", "options": "" badTok)");
}

}  // namespace mongo

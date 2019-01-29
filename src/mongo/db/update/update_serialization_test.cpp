/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include <boost/intrusive_ptr.hpp>
#include <iostream>
#include <map>
#include <memory>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/db/json.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/update/update_driver.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

// Unit tests for UpdateNode serialization. Note that these tests are, for simplicity, sensitive to
// the order operations are serialized in (which is currently lexicographically) even though the
// server is insensitive to this order.

auto updateRoundTrip(const char* json, const std::vector<std::string> filterNames = {}) {
    UpdateDriver driver(new ExpressionContextForTest);
    auto bson = mongo::fromjson(json);
    // Include some trivial array filters to allow parsing '$[<identifier>]'.
    std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>> filters;
    for (const auto& name : filterNames)
        filters[name] = nullptr;
    driver.parse(bson, filters);
    return mongo::tojson(driver.serialize());
}

TEST(UpdateSerialization, DocumentReplacementSerializesExactly) {
    ASSERT_IDENTITY(R"({})", updateRoundTrip);
    ASSERT_IDENTITY(R"({ "a" : 23 })", updateRoundTrip);
    ASSERT_IDENTITY(R"({ "a" : 23, "b" : false, "c" : "JSON!" })", updateRoundTrip);
    ASSERT_IDENTITY(R"({ "a" : [ 1, 2, { "three" : 3 } ] })", updateRoundTrip);
    ASSERT_IDENTITY(R"({ "a" : [], "b" : {}, "c" : null })", updateRoundTrip);
}

TEST(UpdateSerialization, CurrentDateSerializesWithAddedVerbosity) {
    ASSERT_IDENTITY(R"({ "$currentDate" : { "whattimeisit" : { "$type" : "timestamp" } } })",
                    updateRoundTrip);
    ASSERT_IDENTITY(R"({ "$currentDate" : { "whatyearisit" : { "$type" : "date" } } })",
                    updateRoundTrip);
    ASSERT_EQ(R"({ "$currentDate" : { "whattimeisit" : { "$type" : "timestamp" }, )"
              R"("whatyearisit" : { "$type" : "date" } } })",
              updateRoundTrip(R"({ "$currentDate" : { "whattimeisit" : { "$type" : "timestamp" }, )"
                              R"("whatyearisit" : true } })"));
}

TEST(UpdateSerialization, IncrementSerializesExactly) {
    ASSERT_IDENTITY(R"({ "$inc" : { "in.cor.per.ated" : 2147483647, "invisible" : -2 } })",
                    updateRoundTrip);
}

TEST(UpdateSerialization, MinimumSerializesExactly) {
    auto myTrip = [](std::string foo) { return updateRoundTrip(foo.c_str()); };
    ASSERT_IDENTITY(R"({ "$min" : { "e" : 2, "i" : -2 } })", myTrip);
}

TEST(UpdateSerialization, MaximumSerializesExactly) {
    ASSERT_IDENTITY(R"({ "$max" : { "slacks" : 782, "tracks" : -2147483648, "x.y.z" : 0 } })",
                    updateRoundTrip);
}

TEST(UpdateSerialization, MultiplySerializesExactly) {
    ASSERT_IDENTITY(R"({ "$mul" : { "e" : 2.71828, "pi" : 3.14 } })", updateRoundTrip);
}

TEST(UpdateSerialization, RenameSerializesExactly) {
    ASSERT_IDENTITY(R"({ "$rename" : { "name.first" : "name.fname" } })", updateRoundTrip);
}

TEST(UpdateSerialization, SetSerializesExactly) {
    ASSERT_IDENTITY(R"({ "$set" : { "a.ba.ba.45.foo" : [ null, false, NaN ] } })", updateRoundTrip);
    ASSERT_IDENTITY(R"({ "$set" : { "a.b" : 1, "a.c" : 2, "a.d.e" : 3, "a.d.f" : 4 } })",
                    updateRoundTrip);
}

TEST(UpdateSerialization, SetOnInsertSerializesExactly) {
    ASSERT_IDENTITY(R"({ "$setOnInsert" : { "a.b.c.24" : 1, "a.b.c.d.e.24" : 2 } })",
                    updateRoundTrip);
}

TEST(UpdateSerialization, UnsetSerializesWhileDiscardingMeaninglessPayload) {
    ASSERT_EQ(R"({ "$unset" : { "a.0.1.foo" : 1 } })",
              updateRoundTrip(R"({ "$unset" : { "a.0.1.foo": "don't forget me" } })"));
}

TEST(UpdateSerialization, DollarPathsSerializesExactly) {
    ASSERT_IDENTITY(R"({ "$set" : { "grades.$" : 82 } })", updateRoundTrip);
    ASSERT_IDENTITY(R"({ "$set" : { "grades.$.std" : 6 } })", updateRoundTrip);
}

TEST(UpdateSerialization, DollarBracketsSerializesExactly) {
    ASSERT_IDENTITY(R"({ "$set" : { "grades.$[]" : 82 } })", updateRoundTrip);
    ASSERT_IDENTITY(R"({ "$set" : { "grades.$[].std" : 6 } })", updateRoundTrip);
    ASSERT_IDENTITY(R"({ "$set" : { "grades.$[].questions.$[]" : 2 } })", updateRoundTrip);
    ASSERT_IDENTITY(R"({ "$set" : { "grades.$[].questions.$[].first" : 8 } })", updateRoundTrip);
}

TEST(UpdateSerialization, DollarBracketsArrayFilterSerializesExactly) {
    ASSERT_EQ(R"({ "$set" : { "grades.$[array]" : 82 } })",
              updateRoundTrip(R"({ "$set" : { "grades.$[array]" : 82 } })", {"array"}));
    ASSERT_EQ(R"({ "$set" : { "grades.$[array].std" : 6 } })",
              updateRoundTrip(R"({ "$set" : { "grades.$[array].std" : 6 } })", {"array"}));
    ASSERT_EQ(R"({ "$set" : { "grades.$[array].questions.$[filters]" : 2 } })",
              updateRoundTrip(R"({ "$set" : { "grades.$[array].questions.$[filters]" : 2 } })",
                              {"array", "filters"}));
    ASSERT_EQ(
        R"({ "$set" : { "grades.$[array].questions.$[filters].first" : 8 } })",
        updateRoundTrip(R"({ "$set" : { "grades.$[array].questions.$[filters].first" : 8 } })",
                        {"array", "filters"}));
}

TEST(UpdateSerialization, AddToSetSerializesWithReducedVerbosity) {
    ASSERT_IDENTITY(R"({ "$addToSet" : { "stitch.lib" : "cool" } })", updateRoundTrip);
    ASSERT_IDENTITY(R"({ "$addToSet" : { "stitch.lib" : { "$each" : [ "cool", "sweet" ] } } })",
                    updateRoundTrip);
    ASSERT_IDENTITY(R"({ "$addToSet" : { "stitch.lib" : { "$each" : [] } } })", updateRoundTrip);
    ASSERT_EQ(R"({ "$addToSet" : { "stitch.lib" : "cool" } })",
              updateRoundTrip(R"({ "$addToSet" : { "stitch.lib" : { "$each" : [ "cool" ] } } })"));
}

TEST(UpdateSerialization, PopSerializesExactly) {
    ASSERT_IDENTITY(R"({ "$pop" : { "p.0.p" : 1 } })", updateRoundTrip);
    ASSERT_IDENTITY(R"({ "$pop" : { "p.0.p" : -1 } })", updateRoundTrip);
}

TEST(UpdateSerialization, PullUpdateLanguageSerializesExactlyFindLanguageChanges) {
    // This exercises PullNode::ObjectMatcher.
    ASSERT_IDENTITY(
        R"({ "$pull" : { "up" : { "push" : "down", "lucky numbers" : [ 1, 4, 7, 82 ] } } })",
        updateRoundTrip);
    // These exercise PullNode::WrappedObjectMatcher.
    ASSERT_IDENTITY(R"({ "$pull" : { "up.num" : { "$gt" : 12 } } })", updateRoundTrip);
    ASSERT_IDENTITY(R"({ "$pull" : { "up.num" : { "$in" : [ 12, 13, 14 ] } } })", updateRoundTrip);
    ASSERT_IDENTITY(R"({ "$pull" : { "foo" : { "bar" : { "$gt" : 3 } } } })", updateRoundTrip);
    ASSERT_EQ(R"({ "$pull" : { "where.to.begin" : { "$regex" : "^thestart", "$options" : "" } } })",
              updateRoundTrip(R"({ "$pull" : { "where.to.begin" : /^thestart/ } })"));
    // These exercise PullNode::EqualityMatcher.
    ASSERT_IDENTITY(R"({ "$pull" : { "up.num" : 12 } })", updateRoundTrip);
    ASSERT_IDENTITY(R"({ "$pull" : { "up.num" : [ 12, 13, 14 ] } })", updateRoundTrip);
}

TEST(UpdateSerialization, PushSerializesWithAddedVerbosity) {
    ASSERT_EQ(R"({ "$push" : { "up.num" : { "$each" : [ 12 ] } } })",
              updateRoundTrip(R"({ "$push" : { "up.num" : 12 } })"));
    ASSERT_IDENTITY(R"({ "$push" : { "up.num" : { "$each" : [ 12 ] } } })", updateRoundTrip);
    ASSERT_IDENTITY(R"({ "$push" : { "up.num" : { "$each" : [ 12, 13, 14 ] } } })",
                    updateRoundTrip);

    ASSERT_EQ(
        R"({ "$push" : { "up.num" : { "$each" : [], "$slice" : { "$numberLong" : "3" } } } })",
        updateRoundTrip(R"({ "$push" : { "up.num" : { "$each" : [], "$slice" : 3 } } })"));

    ASSERT_EQ(
        R"({ "$push" : { "up.num" : { "$each" : [ 12, 13, 14 ], )"
        R"("$position" : { "$numberLong" : "3" } } } })",
        updateRoundTrip(
            R"({ "$push" : { "up.num" : { "$each" : [ 12, 13, 14 ] , "$position" : 3 } } })"));

    // This coveres cases where $each contains non-object elements.
    ASSERT_IDENTITY(R"({ "$push" : { "up.num" : { "$each" : [ 12, 13, 14 ], "$sort" : 1 } } })",
                    updateRoundTrip);
    // This coveres cases where $each contains object elements.
    ASSERT_IDENTITY(R"({ "$push" : { "up.num" : { )"
                    R"("$each" : [ { "field" : 12 }, { "field" : 13 } ], )"
                    R"("$sort" : { "field" : 1 } } } })",
                    updateRoundTrip);

    ASSERT_EQ(
        R"({ "$push" : { "up.num" : { "$each" : [ 12, 13, 14 ], )"
        R"("$slice" : { "$numberLong" : "22" }, "$position" : { "$numberLong" : "3" }, )"
        R"("$sort" : 1 } } })",
        updateRoundTrip(
            R"({ "$push" : { "up.num" : { )"
            R"("$each" : [ 12, 13, 14 ] , "$slice" : 22, "$position" : 3, "$sort" : 1 } } })"));
}

TEST(UpdateSerialization, PullAllSerializesExactly) {
    ASSERT_IDENTITY(R"({ "$pullAll" : { "no stuff" : [] } })", updateRoundTrip);
    ASSERT_IDENTITY(
        R"({ "$pullAll" : { "up" : [ { "push" : "down", "lucky numbers" : [ 1, 4, 7, 82 ] } ] } })",
        updateRoundTrip);
    ASSERT_IDENTITY(R"({ "$pullAll" : { "stuff" : [ 14, false, null ] } })", updateRoundTrip);
}

TEST(UpdateSerialization, BitSerializesExactly) {
    ASSERT_IDENTITY(R"({ "$bit" : { "bitwise" : { "and" : 7 } } })", updateRoundTrip);
    ASSERT_IDENTITY(
        R"({ "$bit" : { "bitwise" : { "and" : 7 }, "unwise" : { "or" : 63, "xor" : 255 } } })",
        updateRoundTrip);
}

TEST(UpdateSerialization, CompoundStatementsSerialize) {
    ASSERT_IDENTITY(R"({ "$inc" : { "in.cor.per.ated" : 2147483647, "invisible" : -2 }, )"
                    R"("$max" : { "pi" : 3.14 }, )"
                    R"("$mul" : { "e" : 2, "i" : -2 }, )"
                    R"("$rename" : { "name.first" : "name.fname" }, )"
                    R"("$set" : { "a.ba.ba.45.$" : [ null, false, NaN ] } })",
                    updateRoundTrip);

    ASSERT_IDENTITY(
        R"({ "$addToSet" : { "stitch.lib" : { "$each" : [ "cool", "sweet" ] } }, )"
        R"("$pop" : { "p.0.p" : 1 }, )"
        R"("$pull" : { "up" : { "push" : "down", "lucky numbers" : [ 1, 4, 7, 82 ] } }, )"
        R"("$pullAll" : { "no stuff" : [] }, )"
        R"("$set" : { "grades.$[].questions.$[]" : 2 } })",
        updateRoundTrip);

    ASSERT_IDENTITY(R"({ "$bit" : { "bitwise" : { "and" : 7 } }, )"
                    R"("$currentDate" : { "whattimeisit" : { "$type" : "timestamp" } }, )"
                    R"("$min" : { "slacks" : 782, "tracks" : -2147483648, "x.y.z" : 0 }, )"
                    R"("$setOnInsert" : { "a.b.c.24" : 1, "a.b.c.d.e.24" : 2 }, )"
                    R"("$unset" : { "a.0.1.foo" : 1 } })",
                    updateRoundTrip);
}

}  // namespace
}  // mongo

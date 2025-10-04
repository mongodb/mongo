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

#include "mongo/base/string_data.h"
#include "mongo/bson/json.h"
#include "mongo/bson/oid.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/matcher/expression_with_placeholder.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/write_ops/write_ops_parsers.h"
#include "mongo/db/update/update_driver.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/intrusive_counter.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

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
    driver.parse(write_ops::UpdateModification::parseFromClassicUpdate(bson), filters);
    return mongo::tojson(driver.serialize().getDocument().toBson(),
                         mongo::JsonStringFormat::LegacyStrict);
}

void assertRoundTrip(const char* expr, SourceLocation loc = MONGO_SOURCE_LOCATION()) {
    ASSERT_EQ(updateRoundTrip(expr), expr) << fmt::format("[From {}]", loc);
}

TEST(UpdateSerialization, DocumentReplacementSerializesExactly) {
    assertRoundTrip(R"({})");
    assertRoundTrip(R"({ "a" : 23 })");
    assertRoundTrip(R"({ "a" : 23, "b" : false, "c" : "JSON!" })");
    assertRoundTrip(R"({ "a" : [ 1, 2, { "three" : 3 } ] })");
    assertRoundTrip(R"({ "a" : [], "b" : {}, "c" : null })");
}

TEST(UpdateSerialization, CurrentDateSerializesWithAddedVerbosity) {
    assertRoundTrip(R"({ "$currentDate" : { "whattimeisit" : { "$type" : "timestamp" } } })");
    assertRoundTrip(R"({ "$currentDate" : { "whatyearisit" : { "$type" : "date" } } })");
    ASSERT_EQ(R"({ "$currentDate" : { "whattimeisit" : { "$type" : "timestamp" }, )"
              R"("whatyearisit" : { "$type" : "date" } } })",
              updateRoundTrip(R"({ "$currentDate" : { "whattimeisit" : { "$type" : "timestamp" }, )"
                              R"("whatyearisit" : true } })"));
}

TEST(UpdateSerialization, IncrementSerializesExactly) {
    assertRoundTrip(R"({ "$inc" : { "in.cor.per.ated" : 2147483647, "invisible" : -2 } })");
}

TEST(UpdateSerialization, MinimumSerializesExactly) {
    std::string input = R"({ "$min" : { "e" : 2, "i" : -2 } })";
    ASSERT_EQ(updateRoundTrip(input.c_str()), input);
}

TEST(UpdateSerialization, MaximumSerializesExactly) {
    assertRoundTrip(R"({ "$max" : { "slacks" : 782, "tracks" : -2147483648, "x.y.z" : 0 } })");
}

TEST(UpdateSerialization, MultiplySerializesExactly) {
    assertRoundTrip(R"({ "$mul" : { "e" : 2.71828, "pi" : 3.14 } })");
}

TEST(UpdateSerialization, RenameSerializesExactly) {
    assertRoundTrip(R"({ "$rename" : { "name.first" : "name.fname" } })");
}

TEST(UpdateSerialization, SetSerializesExactly) {
    assertRoundTrip(R"({ "$set" : { "a.ba.ba.45.foo" : [ null, false, NaN ] } })");
    assertRoundTrip(R"({ "$set" : { "a.b" : 1, "a.c" : 2, "a.d.e" : 3, "a.d.f" : 4 } })");
}

TEST(UpdateSerialization, SetOnInsertSerializesExactly) {
    assertRoundTrip(R"({ "$setOnInsert" : { "a.b.c.24" : 1, "a.b.c.d.e.24" : 2 } })");
}

TEST(UpdateSerialization, UnsetSerializesWhileDiscardingMeaninglessPayload) {
    ASSERT_EQ(R"({ "$unset" : { "a.0.1.foo" : 1 } })",
              updateRoundTrip(R"({ "$unset" : { "a.0.1.foo": "don't forget me" } })"));
}

TEST(UpdateSerialization, DollarPathsSerializesExactly) {
    assertRoundTrip(R"({ "$set" : { "grades.$" : 82 } })");
    assertRoundTrip(R"({ "$set" : { "grades.$.std" : 6 } })");
}

TEST(UpdateSerialization, DollarBracketsSerializesExactly) {
    assertRoundTrip(R"({ "$set" : { "grades.$[]" : 82 } })");
    assertRoundTrip(R"({ "$set" : { "grades.$[].std" : 6 } })");
    assertRoundTrip(R"({ "$set" : { "grades.$[].questions.$[]" : 2 } })");
    assertRoundTrip(R"({ "$set" : { "grades.$[].questions.$[].first" : 8 } })");
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
    assertRoundTrip(R"({ "$addToSet" : { "stitch.lib" : "cool" } })");
    assertRoundTrip(R"({ "$addToSet" : { "stitch.lib" : { "$each" : [ "cool", "sweet" ] } } })");
    assertRoundTrip(R"({ "$addToSet" : { "stitch.lib" : { "$each" : [] } } })");
    ASSERT_EQ(R"({ "$addToSet" : { "stitch.lib" : "cool" } })",
              updateRoundTrip(R"({ "$addToSet" : { "stitch.lib" : { "$each" : [ "cool" ] } } })"));
}

TEST(UpdateSerialization, PopSerializesExactly) {
    assertRoundTrip(R"({ "$pop" : { "p.0.p" : 1 } })");
    assertRoundTrip(R"({ "$pop" : { "p.0.p" : -1 } })");
}

TEST(UpdateSerialization, PullUpdateLanguageSerializesExactlyFindLanguageChanges) {
    // This exercises PullNode::ObjectMatcher.
    assertRoundTrip(
        R"({ "$pull" : { "up" : { "push" : "down", "lucky numbers" : [ 1, 4, 7, 82 ] } } })");
    // These exercise PullNode::WrappedObjectMatcher.
    assertRoundTrip(R"({ "$pull" : { "up.num" : { "$gt" : 12 } } })");
    assertRoundTrip(R"({ "$pull" : { "up.num" : { "$in" : [ 12, 13, 14 ] } } })");
    assertRoundTrip(R"({ "$pull" : { "foo" : { "bar" : { "$gt" : 3 } } } })");
    ASSERT_EQ(R"({ "$pull" : { "where.to.begin" : { "$regex" : "^thestart", "$options" : "" } } })",
              updateRoundTrip(R"({ "$pull" : { "where.to.begin" : /^thestart/ } })"));
    // These exercise PullNode::EqualityMatcher.
    assertRoundTrip(R"({ "$pull" : { "up.num" : 12 } })");
    assertRoundTrip(R"({ "$pull" : { "up.num" : [ 12, 13, 14 ] } })");
}

TEST(UpdateSerialization, PushSerializesWithAddedVerbosity) {
    ASSERT_EQ(R"({ "$push" : { "up.num" : { "$each" : [ 12 ] } } })",
              updateRoundTrip(R"({ "$push" : { "up.num" : 12 } })"));
    assertRoundTrip(R"({ "$push" : { "up.num" : { "$each" : [ 12 ] } } })");
    assertRoundTrip(R"({ "$push" : { "up.num" : { "$each" : [ 12, 13, 14 ] } } })");

    ASSERT_EQ(
        R"({ "$push" : { "up.num" : { "$each" : [], "$slice" : { "$numberLong" : "3" } } } })",
        updateRoundTrip(R"({ "$push" : { "up.num" : { "$each" : [], "$slice" : 3 } } })"));

    ASSERT_EQ(
        R"({ "$push" : { "up.num" : { "$each" : [ 12, 13, 14 ], )"
        R"("$position" : { "$numberLong" : "3" } } } })",
        updateRoundTrip(
            R"({ "$push" : { "up.num" : { "$each" : [ 12, 13, 14 ] , "$position" : 3 } } })"));

    // This coveres cases where $each contains non-object elements.
    assertRoundTrip(R"({ "$push" : { "up.num" : { "$each" : [ 12, 13, 14 ], "$sort" : 1 } } })");
    // This coveres cases where $each contains object elements.
    assertRoundTrip(R"({ "$push" : { "up.num" : { )"
                    R"("$each" : [ { "field" : 12 }, { "field" : 13 } ], )"
                    R"("$sort" : { "field" : 1 } } } })");

    ASSERT_EQ(
        R"({ "$push" : { "up.num" : { "$each" : [ 12, 13, 14 ], )"
        R"("$slice" : { "$numberLong" : "22" }, "$position" : { "$numberLong" : "3" }, )"
        R"("$sort" : 1 } } })",
        updateRoundTrip(
            R"({ "$push" : { "up.num" : { )"
            R"("$each" : [ 12, 13, 14 ] , "$slice" : 22, "$position" : 3, "$sort" : 1 } } })"));
}

TEST(UpdateSerialization, PullAllSerializesExactly) {
    assertRoundTrip(R"({ "$pullAll" : { "no stuff" : [] } })");
    assertRoundTrip(
        R"({ "$pullAll" : { "up" : [ { "push" : "down", "lucky numbers" : [ 1, 4, 7, 82 ] } ] } })");
    assertRoundTrip(R"({ "$pullAll" : { "stuff" : [ 14, false, null ] } })");
}

TEST(UpdateSerialization, BitSerializesExactly) {
    assertRoundTrip(R"({ "$bit" : { "bitwise" : { "and" : 7 } } })");
    assertRoundTrip(
        R"({ "$bit" : { "bitwise" : { "and" : 7 }, "unwise" : { "or" : 63, "xor" : 255 } } })");
}

TEST(UpdateSerialization, CompoundStatementsSerialize) {
    assertRoundTrip(R"({ "$inc" : { "in.cor.per.ated" : 2147483647, "invisible" : -2 }, )"
                    R"("$max" : { "pi" : 3.14 }, )"
                    R"("$mul" : { "e" : 2, "i" : -2 }, )"
                    R"("$rename" : { "name.first" : "name.fname" }, )"
                    R"("$set" : { "a.ba.ba.45.$" : [ null, false, NaN ] } })");

    assertRoundTrip(
        R"({ "$addToSet" : { "stitch.lib" : { "$each" : [ "cool", "sweet" ] } }, )"
        R"("$pop" : { "p.0.p" : 1 }, )"
        R"("$pull" : { "up" : { "push" : "down", "lucky numbers" : [ 1, 4, 7, 82 ] } }, )"
        R"("$pullAll" : { "no stuff" : [] }, )"
        R"("$set" : { "grades.$[].questions.$[]" : 2 } })");

    assertRoundTrip(R"({ "$bit" : { "bitwise" : { "and" : 7 } }, )"
                    R"("$currentDate" : { "whattimeisit" : { "$type" : "timestamp" } }, )"
                    R"("$min" : { "slacks" : 782, "tracks" : -2147483648, "x.y.z" : 0 }, )"
                    R"("$setOnInsert" : { "a.b.c.24" : 1, "a.b.c.d.e.24" : 2 }, )"
                    R"("$unset" : { "a.0.1.foo" : 1 } })");
}

}  // namespace
}  // namespace mongo

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

#include "mongo/db/query/canonical_query_encoder.h"

#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"


namespace mongo {
namespace {

using std::unique_ptr;

static const NamespaceString nss("testdb.testcoll");

/**
 * Utility functions to create a CanonicalQuery
 */
unique_ptr<CanonicalQuery> canonicalize(BSONObj query,
                                        BSONObj sort,
                                        BSONObj proj,
                                        BSONObj collation) {
    QueryTestServiceContext serviceContext;
    auto opCtx = serviceContext.makeOperationContext();

    auto qr = std::make_unique<QueryRequest>(nss);
    qr->setFilter(query);
    qr->setSort(sort);
    qr->setProj(proj);
    qr->setCollation(collation);
    const boost::intrusive_ptr<ExpressionContext> expCtx;
    auto statusWithCQ =
        CanonicalQuery::canonicalize(opCtx.get(),
                                     std::move(qr),
                                     expCtx,
                                     ExtensionsCallbackNoop(),
                                     MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_OK(statusWithCQ.getStatus());
    return std::move(statusWithCQ.getValue());
}

unique_ptr<CanonicalQuery> canonicalize(const char* queryStr) {
    BSONObj queryObj = fromjson(queryStr);
    return canonicalize(queryObj, {}, {}, {});
}


/**
 * Test functions for computeKey, when no indexes are present. Cache keys are intentionally
 * obfuscated and are meaningful only within the current lifetime of the server process. Users
 * should treat plan cache keys as opaque.
 */

void testComputeKey(const CanonicalQuery& cq, const char* expectedStr) {
    const auto key = cq.encodeKey();
    StringData expectedKey(expectedStr);
    if (key != expectedKey) {
        str::stream ss;
        ss << "Unexpected plan cache key. Expected: " << expectedKey << ". Actual: " << key
           << ". Query: " << cq.toString();
        FAIL(ss);
    }
}

void testComputeKey(BSONObj query, BSONObj sort, BSONObj proj, const char* expectedStr) {
    BSONObj collation;
    unique_ptr<CanonicalQuery> cq(canonicalize(query, sort, proj, collation));
    testComputeKey(*cq, expectedStr);
}

void testComputeKey(const char* queryStr,
                    const char* sortStr,
                    const char* projStr,
                    const char* expectedStr) {
    testComputeKey(fromjson(queryStr), fromjson(sortStr), fromjson(projStr), expectedStr);
}

TEST(CanonicalQueryEncoderTest, ComputeKey) {
    // Generated cache keys should be treated as opaque to the user.

    // No sorts
    testComputeKey("{}", "{}", "{}", "an");
    testComputeKey("{$or: [{a: 1}, {b: 2}]}", "{}", "{}", "or[eqa,eqb]");
    testComputeKey("{$or: [{a: 1}, {b: 1}, {c: 1}], d: 1}", "{}", "{}", "an[or[eqa,eqb,eqc],eqd]");
    testComputeKey("{$or: [{a: 1}, {b: 1}], c: 1, d: 1}", "{}", "{}", "an[or[eqa,eqb],eqc,eqd]");
    testComputeKey("{a: 1, b: 1, c: 1}", "{}", "{}", "an[eqa,eqb,eqc]");
    testComputeKey("{a: 1, beqc: 1}", "{}", "{}", "an[eqa,eqbeqc]");
    testComputeKey("{ap1a: 1}", "{}", "{}", "eqap1a");
    testComputeKey("{aab: 1}", "{}", "{}", "eqaab");

    // With sort
    testComputeKey("{}", "{a: 1}", "{}", "an~aa");
    testComputeKey("{}", "{a: -1}", "{}", "an~da");
    testComputeKey("{}", "{a: {$meta: 'textScore'}}", "{a: {$meta: 'textScore'}}", "an~ta");
    testComputeKey("{a: 1}", "{b: 1}", "{}", "eqa~ab");

    // With projection
    testComputeKey("{}", "{}", "{a: 1}", "an|_id-a");
    testComputeKey("{}", "{}", "{a: -1}", "an|_id-a");
    testComputeKey("{}", "{}", "{a: -1.0}", "an|_id-a");
    testComputeKey("{}", "{}", "{a: true}", "an|_id-a");
    testComputeKey("{}", "{}", "{a: 0}", "an");
    testComputeKey("{}", "{}", "{a: false}", "an");
    testComputeKey("{}", "{}", "{a: 99}", "an|_id-a");
    testComputeKey("{}", "{}", "{a: 'foo'}", "an|_id-a");
    // $slice defaults to exclusion.
    testComputeKey("{}", "{}", "{a: {$slice: [3, 5]}}", "an");
    testComputeKey("{}", "{}", "{a: {$slice: [3, 5]}, b: 0}", "an");

    // But even when using $slice in an inclusion, the entire document is needed.
    testComputeKey("{}", "{}", "{a: {$slice: [3, 5]}, b: 1}", "an");

    testComputeKey("{}", "{}", "{a: {$elemMatch: {x: 2}}}", "an");
    testComputeKey("{}", "{}", "{a: {$elemMatch: {x: 2}}, b: 0}", "an");
    testComputeKey("{}", "{}", "{a: {$elemMatch: {x: 2}}, b: 1}", "an");

    testComputeKey("{}", "{}", "{a: {$slice: [3, 5]}, b: {$elemMatch: {x: 2}}}", "an");

    testComputeKey("{}", "{}", "{a: ObjectId('507f191e810c19729de860ea')}", "an|_id-a");
    testComputeKey("{a: 1}", "{}", "{'a.$': 1}", "eqa");
    testComputeKey("{a: 1}", "{}", "{a: 1}", "eqa|_id-a");

    // Projection should be order-insensitive
    testComputeKey("{}", "{}", "{a: 1, b: 1}", "an|_id-a-b");
    testComputeKey("{}", "{}", "{b: 1, a: 1}", "an|_id-a-b");

    // And should escape the separation character.
    testComputeKey("{}", "{}", "{'b-1': 1, 'a-2': 1}", "an|_id-a\\-2-b\\-1");

    // And should exclude $-prefixed fields which can be added internally.
    testComputeKey("{}", "{x: 1}", "{$sortKey: {$meta: 'sortKey'}}", "an~ax");
    testComputeKey("{}", "{}", "{}", "an");

    testComputeKey("{}", "{x: 1}", "{a: 1, $sortKey: {$meta: 'sortKey'}}", "an~ax|_id-a");
    testComputeKey("{}", "{}", "{a: 1}", "an|_id-a");

    // With or-elimination and projection
    testComputeKey("{$or: [{a: 1}]}", "{}", "{_id: 0, a: 1}", "eqa|a");
    testComputeKey("{$or: [{a: 1}]}", "{}", "{'a.$': 1}", "eqa");
}

// Delimiters found in user field names or non-standard projection field values
// must be escaped.
TEST(CanonicalQueryEncoderTest, ComputeKeyEscaped) {
    // Field name in query.
    testComputeKey("{'a,[]~|-<>': 1}", "{}", "{}", "eqa\\,\\[\\]\\~\\|\\-<>");

    // Field name in sort.
    testComputeKey("{}", "{'a,[]~|-<>': 1}", "{}", "an~aa\\,\\[\\]\\~\\|\\-<>");

    // Field name in projection.
    testComputeKey("{}", "{}", "{'a,[]~|-<>': 1}", "an|_id-a\\,\\[\\]\\~\\|\\-<>");

    // Value in projection.
    testComputeKey("{}", "{}", "{a: 'foo,[]~|-<>'}", "an|_id-a");
}

// Cache keys for $geoWithin queries with legacy and GeoJSON coordinates should
// not be the same.
TEST(CanonicalQueryEncoderTest, ComputeKeyGeoWithin) {
    PlanCache planCache;

    // Legacy coordinates.
    unique_ptr<CanonicalQuery> cqLegacy(
        canonicalize("{a: {$geoWithin: "
                     "{$box: [[-180, -90], [180, 90]]}}}"));
    // GeoJSON coordinates.
    unique_ptr<CanonicalQuery> cqNew(
        canonicalize("{a: {$geoWithin: "
                     "{$geometry: {type: 'Polygon', coordinates: "
                     "[[[0, 0], [0, 90], [90, 0], [0, 0]]]}}}}"));
    ASSERT_NOT_EQUALS(planCache.computeKey(*cqLegacy), planCache.computeKey(*cqNew));
}

// GEO_NEAR cache keys should include information on geometry and CRS in addition
// to the match type and field name.
TEST(CanonicalQueryEncoderTest, ComputeKeyGeoNear) {
    testComputeKey("{a: {$near: [0,0], $maxDistance:0.3 }}", "{}", "{}", "gnanrfl");
    testComputeKey("{a: {$nearSphere: [0,0], $maxDistance: 0.31 }}", "{}", "{}", "gnanssp");
    testComputeKey(
        "{a: {$geoNear: {$geometry: {type: 'Point', coordinates: [0,0]},"
        "$maxDistance:100}}}",
        "{}",
        "{}",
        "gnanrsp");
}

TEST(CanonicalQueryEncoderTest, ComputeKeyRegexDependsOnFlags) {
    testComputeKey("{a: {$regex: \"sometext\"}}", "{}", "{}", "rea");
    testComputeKey("{a: {$regex: \"sometext\", $options: \"\"}}", "{}", "{}", "rea");

    testComputeKey("{a: {$regex: \"sometext\", $options: \"s\"}}", "{}", "{}", "rea/s/");
    testComputeKey("{a: {$regex: \"sometext\", $options: \"ms\"}}", "{}", "{}", "rea/ms/");

    // Test that the ordering of $options doesn't matter.
    testComputeKey("{a: {$regex: \"sometext\", $options: \"im\"}}", "{}", "{}", "rea/im/");
    testComputeKey("{a: {$regex: \"sometext\", $options: \"mi\"}}", "{}", "{}", "rea/im/");

    // Test that only the options affect the key. Two regex match expressions with the same options
    // but different $regex values should have the same shape.
    testComputeKey("{a: {$regex: \"abc\", $options: \"mi\"}}", "{}", "{}", "rea/im/");
    testComputeKey("{a: {$regex: \"efg\", $options: \"mi\"}}", "{}", "{}", "rea/im/");

    testComputeKey("{a: {$regex: \"\", $options: \"ms\"}}", "{}", "{}", "rea/ms/");
    testComputeKey("{a: {$regex: \"___\", $options: \"ms\"}}", "{}", "{}", "rea/ms/");

    // Test that only valid regex flags contribute to the plan cache key encoding.
    testComputeKey(BSON("a" << BSON("$regex"
                                    << "abc"
                                    << "$options"
                                    << "abcdefghijklmnopqrstuvwxyz")),
                   {},
                   {},
                   "rea/imsx/");
    testComputeKey("{a: /abc/gim}", "{}", "{}", "rea/im/");
}

TEST(CanonicalQueryEncoderTest, ComputeKeyMatchInDependsOnPresenceOfRegexAndFlags) {
    // Test that an $in containing a single regex is unwrapped to $regex.
    testComputeKey("{a: {$in: [/foo/]}}", "{}", "{}", "rea");
    testComputeKey("{a: {$in: [/foo/i]}}", "{}", "{}", "rea/i/");

    // Test that an $in with no regexes does not include any regex information.
    testComputeKey("{a: {$in: [1, 'foo']}}", "{}", "{}", "ina");

    // Test that an $in with a regex encodes the presence of the regex.
    testComputeKey("{a: {$in: [1, /foo/]}}", "{}", "{}", "ina_re");

    // Test that an $in with a regex encodes the presence of the regex and its flags.
    testComputeKey("{a: {$in: [1, /foo/is]}}", "{}", "{}", "ina_re/is/");

    // Test that the computed key is invariant to the order of the flags within each regex.
    testComputeKey("{a: {$in: [1, /foo/si]}}", "{}", "{}", "ina_re/is/");

    // Test that an $in with multiple regexes encodes all unique flags.
    testComputeKey("{a: {$in: [1, /foo/i, /bar/m, /baz/s]}}", "{}", "{}", "ina_re/ims/");

    // Test that an $in with multiple regexes deduplicates identical flags.
    testComputeKey(
        "{a: {$in: [1, /foo/i, /bar/m, /baz/s, /qux/i, /quux/s]}}", "{}", "{}", "ina_re/ims/");

    // Test that the computed key is invariant to the ordering of the flags across regexes.
    testComputeKey("{a: {$in: [1, /foo/ism, /bar/msi, /baz/im, /qux/si, /quux/im]}}",
                   "{}",
                   "{}",
                   "ina_re/ims/");
    testComputeKey("{a: {$in: [1, /foo/msi, /bar/ism, /baz/is, /qux/mi, /quux/im]}}",
                   "{}",
                   "{}",
                   "ina_re/ims/");

    // Test that $not-$in-$regex similarly records the presence and flags of any regexes.
    testComputeKey("{a: {$not: {$in: [1, 'foo']}}}", "{}", "{}", "nt[ina]");
    testComputeKey("{a: {$not: {$in: [1, /foo/]}}}", "{}", "{}", "nt[ina_re]");
    testComputeKey(
        "{a: {$not: {$in: [1, /foo/i, /bar/i, /baz/msi]}}}", "{}", "{}", "nt[ina_re/ims/]");

    // Test that a $not-$in containing a single regex is unwrapped to $not-$regex.
    testComputeKey("{a: {$not: {$in: [/foo/]}}}", "{}", "{}", "nt[rea]");
    testComputeKey("{a: {$not: {$in: [/foo/i]}}}", "{}", "{}", "nt[rea/i/]");
}

TEST(CanonicalQueryEncoderTest, CheckCollationIsEncoded) {

    unique_ptr<CanonicalQuery> cq(canonicalize(
        fromjson("{a: 1, b: 1}"), {}, {}, fromjson("{locale: 'mock_reverse_string'}")));

    testComputeKey(*cq, "an[eqa,eqb]#mock_reverse_string02300000");
}

}  // namespace
}  // namespace mongo

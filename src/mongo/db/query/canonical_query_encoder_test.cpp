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

#include "mongo/db/catalog/collection_mock.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/plan_cache_key_factory.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"


namespace mongo {
namespace {

using std::unique_ptr;

static const NamespaceString nss("testdb.testcoll");

PlanCacheKey makeKey(const CanonicalQuery& cq) {
    CollectionMock coll(nss);
    return plan_cache_key_factory::make<PlanCacheKey>(cq, &coll);
}

/**
 * Utility functions to create a CanonicalQuery
 */
unique_ptr<CanonicalQuery> canonicalize(BSONObj query,
                                        BSONObj sort,
                                        BSONObj proj,
                                        BSONObj collation,
                                        std::unique_ptr<FindCommandRequest> findCommand = nullptr) {
    QueryTestServiceContext serviceContext;
    auto opCtx = serviceContext.makeOperationContext();

    if (!findCommand) {
        findCommand = std::make_unique<FindCommandRequest>(nss);
    }
    findCommand->setFilter(query.getOwned());
    findCommand->setSort(sort.getOwned());
    findCommand->setProjection(proj.getOwned());
    findCommand->setCollation(collation.getOwned());
    const boost::intrusive_ptr<ExpressionContext> expCtx;
    auto statusWithCQ =
        CanonicalQuery::canonicalize(opCtx.get(),
                                     std::move(findCommand),
                                     false,
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

void testComputeSBEKey(BSONObj query,
                       BSONObj sort,
                       BSONObj proj,
                       std::string expectedStr,
                       std::unique_ptr<FindCommandRequest> findCommand = nullptr) {
    BSONObj collation;
    unique_ptr<CanonicalQuery> cq(
        canonicalize(query, sort, proj, collation, std::move(findCommand)));
    cq->setSbeCompatible(true);
    auto key = makeKey(*cq);
    ASSERT_EQUALS(key.toString(), expectedStr);
}

void testComputeKey(const char* queryStr,
                    const char* sortStr,
                    const char* projStr,
                    const char* expectedStr) {
    testComputeKey(fromjson(queryStr), fromjson(sortStr), fromjson(projStr), expectedStr);
}

void testComputeSBEKey(const char* queryStr,
                       const char* sortStr,
                       const char* projStr,
                       std::string expectedStr,
                       std::unique_ptr<FindCommandRequest> findCommand = nullptr) {
    testComputeSBEKey(fromjson(queryStr),
                      fromjson(sortStr),
                      fromjson(projStr),
                      expectedStr,
                      std::move(findCommand));
}

TEST(CanonicalQueryEncoderTest, ComputeKey) {
    // Generated cache keys should be treated as opaque to the user.

    // No sorts
    testComputeKey("{}", "{}", "{}", "an@t");
    testComputeKey("{$or: [{a: 1}, {b: 2}]}", "{}", "{}", "or[eqa,eqb]@t");
    testComputeKey(
        "{$or: [{a: 1}, {b: 1}, {c: 1}], d: 1}", "{}", "{}", "an[or[eqa,eqb,eqc],eqd]@t");
    testComputeKey("{$or: [{a: 1}, {b: 1}], c: 1, d: 1}", "{}", "{}", "an[or[eqa,eqb],eqc,eqd]@t");
    testComputeKey("{a: 1, b: 1, c: 1}", "{}", "{}", "an[eqa,eqb,eqc]@t");
    testComputeKey("{a: 1, beqc: 1}", "{}", "{}", "an[eqa,eqbeqc]@t");
    testComputeKey("{ap1a: 1}", "{}", "{}", "eqap1a@t");
    testComputeKey("{aab: 1}", "{}", "{}", "eqaab@t");

    // With sort
    testComputeKey("{}", "{a: 1}", "{}", "an~aa@t");
    testComputeKey("{}", "{a: -1}", "{}", "an~da@t");
    testComputeKey("{$text: {$search: 'search keywords'}}",
                   "{a: {$meta: 'textScore'}}",
                   "{a: {$meta: 'textScore'}}",
                   "te_fts~ta@t");
    testComputeKey("{a: 1}", "{b: 1}", "{}", "eqa~ab@t");

    // With projection
    testComputeKey("{}", "{}", "{a: 1}", "an|_id-a@t");
    testComputeKey("{}", "{}", "{a: -1}", "an|_id-a@t");
    testComputeKey("{}", "{}", "{a: -1.0}", "an|_id-a@t");
    testComputeKey("{}", "{}", "{a: true}", "an|_id-a@t");
    testComputeKey("{}", "{}", "{a: 0}", "an@t");
    testComputeKey("{}", "{}", "{a: false}", "an@t");
    testComputeKey("{}", "{}", "{a: 99}", "an|_id-a@t");
    testComputeKey("{}", "{}", "{a: 'foo'}", "an|_id@t");

    // $slice defaults to exclusion.
    testComputeKey("{}", "{}", "{a: {$slice: [3, 5]}}", "an@t");
    testComputeKey("{}", "{}", "{a: {$slice: [3, 5]}, b: 0}", "an@t");

    // But even when using $slice in an inclusion, the entire document is needed.
    testComputeKey("{}", "{}", "{a: {$slice: [3, 5]}, b: 1}", "an@t");

    testComputeKey("{}", "{}", "{a: {$elemMatch: {x: 2}}}", "an@t");
    testComputeKey("{}", "{}", "{a: {$elemMatch: {x: 2}}, b: 0}", "an@t");
    testComputeKey("{}", "{}", "{a: {$elemMatch: {x: 2}}, b: 1}", "an@t");

    testComputeKey("{}", "{}", "{a: {$slice: [3, 5]}, b: {$elemMatch: {x: 2}}}", "an@t");

    testComputeKey("{}", "{}", "{a: ObjectId('507f191e810c19729de860ea')}", "an|_id@t");
    // Since this projection overwrites the entire document, no fields are required.
    testComputeKey(
        "{}", "{}", "{_id: 0, a: ObjectId('507f191e810c19729de860ea'), b: 'foo'}", "an|@t");
    testComputeKey("{a: 1}", "{}", "{'a.$': 1}", "eqa@t");
    testComputeKey("{a: 1}", "{}", "{a: 1}", "eqa|_id-a@t");

    // Projection should be order-insensitive
    testComputeKey("{}", "{}", "{a: 1, b: 1}", "an|_id-a-b@t");
    testComputeKey("{}", "{}", "{b: 1, a: 1}", "an|_id-a-b@t");

    // And should escape the separation character.
    testComputeKey("{}", "{}", "{'b-1': 1, 'a-2': 1}", "an|_id-a\\-2-b\\-1@t");

    // And should exclude $-prefixed fields which can be added internally.
    testComputeKey("{}", "{x: 1}", "{$sortKey: {$meta: 'sortKey'}}", "an~ax@t");
    testComputeKey("{}", "{}", "{}", "an@t");

    testComputeKey("{}", "{x: 1}", "{a: 1, $sortKey: {$meta: 'sortKey'}}", "an~ax|_id-a@t");
    testComputeKey("{}", "{}", "{a: 1}", "an|_id-a@t");

    // With or-elimination and projection
    testComputeKey("{$or: [{a: 1}]}", "{}", "{_id: 0, a: 1}", "eqa|a@t");
    testComputeKey("{$or: [{a: 1}]}", "{}", "{'a.$': 1}", "eqa@t");
}

TEST(CanonicalQueryEncoderTest, EncodeNotEqualNullPredicates) {
    // With '$eq', '$gte', and '$lte' negation comparison to 'null'.
    testComputeKey("{a: {$not: {$eq: null}}}", "{}", "{_id: 0, a: 1}", "ntnot_eq_null[eqa]|a@t");
    testComputeKey(
        "{a: {$not: {$eq: null}}}", "{a: 1}", "{_id: 0, a: 1}", "ntnot_eq_null[eqa]~aa|a@t");
    testComputeKey(
        "{a: {$not: {$gte: null}}}", "{a: 1}", "{_id: 0, a: 1}", "ntnot_eq_null[gea]~aa|a@t");
    testComputeKey(
        "{a: {$not: {$lte: null}}}", "{a: 1}", "{_id: 0, a: 1}", "ntnot_eq_null[lea]~aa|a@t");

    // Same '$eq' negation query with non-'null' argument should have different key.
    testComputeKey("{a: {$not: {$eq: true}}}", "{a: 1}", "{_id: 0, a: 1}", "nt[eqa]~aa|a@t");
}

// Delimiters found in user field names or non-standard projection field values
// must be escaped.
TEST(CanonicalQueryEncoderTest, ComputeKeyEscaped) {
    // Field name in query.
    testComputeKey("{'a,[]~|-<>': 1}", "{}", "{}", "eqa\\,\\[\\]\\~\\|\\-<>@t");

    // Field name in sort.
    testComputeKey("{}", "{'a,[]~|-<>': 1}", "{}", "an~aa\\,\\[\\]\\~\\|\\-<>@t");

    // Field name in projection.
    testComputeKey("{}", "{}", "{'a,[]~|-<>': 1}", "an|_id-a\\,\\[\\]\\~\\|\\-<>@t");

    // String literal provided as value.
    testComputeKey("{}", "{}", "{a: 'foo,[]~|-<>'}", "an|_id@t");
}

// Cache keys for $geoWithin queries with legacy and GeoJSON coordinates should
// not be the same.
TEST(CanonicalQueryEncoderTest, ComputeKeyGeoWithin) {
    PlanCache planCache(5000);

    // Legacy coordinates.
    unique_ptr<CanonicalQuery> cqLegacy(
        canonicalize("{a: {$geoWithin: "
                     "{$box: [[-180, -90], [180, 90]]}}}"));
    // GeoJSON coordinates.
    unique_ptr<CanonicalQuery> cqNew(
        canonicalize("{a: {$geoWithin: "
                     "{$geometry: {type: 'Polygon', coordinates: "
                     "[[[0, 0], [0, 90], [90, 0], [0, 0]]]}}}}"));
    ASSERT_NOT_EQUALS(makeKey(*cqLegacy), makeKey(*cqNew));
}

// GEO_NEAR cache keys should include information on geometry and CRS in addition
// to the match type and field name.
TEST(CanonicalQueryEncoderTest, ComputeKeyGeoNear) {
    testComputeKey("{a: {$near: [0,0], $maxDistance:0.3 }}", "{}", "{}", "gnanrfl@t");
    testComputeKey("{a: {$nearSphere: [0,0], $maxDistance: 0.31 }}", "{}", "{}", "gnanssp@t");
    testComputeKey(
        "{a: {$geoNear: {$geometry: {type: 'Point', coordinates: [0,0]},"
        "$maxDistance:100}}}",
        "{}",
        "{}",
        "gnanrsp@t");
}

TEST(CanonicalQueryEncoderTest, ComputeKeyRegexDependsOnFlags) {
    testComputeKey("{a: {$regex: \"sometext\"}}", "{}", "{}", "rea@t");
    testComputeKey("{a: {$regex: \"sometext\", $options: \"\"}}", "{}", "{}", "rea@t");

    testComputeKey("{a: {$regex: \"sometext\", $options: \"s\"}}", "{}", "{}", "rea/s/@t");
    testComputeKey("{a: {$regex: \"sometext\", $options: \"ms\"}}", "{}", "{}", "rea/ms/@t");

    // Test that the ordering of $options doesn't matter.
    testComputeKey("{a: {$regex: \"sometext\", $options: \"im\"}}", "{}", "{}", "rea/im/@t");
    testComputeKey("{a: {$regex: \"sometext\", $options: \"mi\"}}", "{}", "{}", "rea/im/@t");

    // Test that only the options affect the key. Two regex match expressions with the same options
    // but different $regex values should have the same shape.
    testComputeKey("{a: {$regex: \"abc\", $options: \"mi\"}}", "{}", "{}", "rea/im/@t");
    testComputeKey("{a: {$regex: \"efg\", $options: \"mi\"}}", "{}", "{}", "rea/im/@t");

    testComputeKey("{a: {$regex: \"\", $options: \"ms\"}}", "{}", "{}", "rea/ms/@t");
    testComputeKey("{a: {$regex: \"___\", $options: \"ms\"}}", "{}", "{}", "rea/ms/@t");

    // Test that only valid regex flags contribute to the plan cache key encoding.
    testComputeKey(BSON("a" << BSON("$regex"
                                    << "abc"
                                    << "$options"
                                    << "imxsu")),
                   {},
                   {},
                   "rea/imsx/@t");
    testComputeKey("{a: /abc/im}", "{}", "{}", "rea/im/@t");
}

TEST(CanonicalQueryEncoderTest, ComputeKeyMatchInDependsOnPresenceOfRegexAndFlags) {
    // Test that an $in containing a single regex is unwrapped to $regex.
    testComputeKey("{a: {$in: [/foo/]}}", "{}", "{}", "rea@t");
    testComputeKey("{a: {$in: [/foo/i]}}", "{}", "{}", "rea/i/@t");

    // Test that an $in with no regexes does not include any regex information.
    testComputeKey("{a: {$in: [1, 'foo']}}", "{}", "{}", "ina@t");

    // Test that an $in with a regex encodes the presence of the regex.
    testComputeKey("{a: {$in: [1, /foo/]}}", "{}", "{}", "ina_re@t");

    // Test that an $in with a regex encodes the presence of the regex and its flags.
    testComputeKey("{a: {$in: [1, /foo/is]}}", "{}", "{}", "ina_re/is/@t");

    // Test that the computed key is invariant to the order of the flags within each regex.
    testComputeKey("{a: {$in: [1, /foo/si]}}", "{}", "{}", "ina_re/is/@t");

    // Test that an $in with multiple regexes encodes all unique flags.
    testComputeKey("{a: {$in: [1, /foo/i, /bar/m, /baz/s]}}", "{}", "{}", "ina_re/ims/@t");

    // Test that an $in with multiple regexes deduplicates identical flags.
    testComputeKey(
        "{a: {$in: [1, /foo/i, /bar/m, /baz/s, /qux/i, /quux/s]}}", "{}", "{}", "ina_re/ims/@t");

    // Test that the computed key is invariant to the ordering of the flags across regexes.
    testComputeKey("{a: {$in: [1, /foo/ism, /bar/msi, /baz/im, /qux/si, /quux/im]}}",
                   "{}",
                   "{}",
                   "ina_re/ims/@t");
    testComputeKey("{a: {$in: [1, /foo/msi, /bar/ism, /baz/is, /qux/mi, /quux/im]}}",
                   "{}",
                   "{}",
                   "ina_re/ims/@t");

    // Test that $not-$in-$regex similarly records the presence and flags of any regexes.
    testComputeKey("{a: {$not: {$in: [1, 'foo']}}}", "{}", "{}", "nt[ina]@t");
    testComputeKey("{a: {$not: {$in: [1, /foo/]}}}", "{}", "{}", "nt[ina_re]@t");
    testComputeKey(
        "{a: {$not: {$in: [1, /foo/i, /bar/i, /baz/msi]}}}", "{}", "{}", "nt[ina_re/ims/]@t");

    // Test that a $not-$in containing a single regex is unwrapped to $not-$regex.
    testComputeKey("{a: {$not: {$in: [/foo/]}}}", "{}", "{}", "nt[rea]@t");
    testComputeKey("{a: {$not: {$in: [/foo/i]}}}", "{}", "{}", "nt[rea/i/]@t");
}

TEST(CanonicalQueryEncoderTest, CheckCollationIsEncoded) {

    unique_ptr<CanonicalQuery> cq(canonicalize(
        fromjson("{a: 1, b: 1}"), {}, {}, fromjson("{locale: 'mock_reverse_string'}")));

    testComputeKey(*cq, "an[eqa,eqb]#mock_reverse_string02300000@t");
}

TEST(CanonicalQueryEncoderTest, ComputeKeySBE) {
    // Generated cache keys should be treated as opaque to the user.
    RAIIServerParameterControllerForTest controller("featureFlagSbePlanCache", true);

    testComputeSBEKey("{}",
                      "{}",
                      "{}",
                      "BQAAAAAFAAAAAAAAAAAAAAAAbm5ubgUAAAAABQAAAAAFAAAAAGZ0ZkAAAABmCgAAAAMAAAB0yAAA"
                      "AGYAAEAG6AMAAA==");
    testComputeSBEKey("{$or: [{a: 1}, {b: 2}]}",
                      "{}",
                      "{}",
                      "LQAAAAQkb3IAIwAAAAMwAAwAAAAQYQABAAAAAAMxAAwAAAAQYgACAAAAAAAABQAAAAAAAAAAAAAA"
                      "AG5ubm4FAAAAAAUAAAAABQAAAABmdGZAAAAAZgoAAAADAAAAdMgAAABmAABABugDAAA=");
    testComputeSBEKey("{a: 1}",
                      "{}",
                      "{}",
                      "DAAAABBhAAEAAAAABQAAAAAAAAAAAAAAAG5ubm4FAAAAAAUAAAAABQAAAABmdGZAAAAAZgoAAAAD"
                      "AAAAdMgAAABmAABABugDAAA=");
    testComputeSBEKey("{b: 1}",
                      "{}",
                      "{}",
                      "DAAAABBiAAEAAAAABQAAAAAAAAAAAAAAAG5ubm4FAAAAAAUAAAAABQAAAABmdGZAAAAAZgoAAAAD"
                      "AAAAdMgAAABmAABABugDAAA=");
    testComputeSBEKey("{a: 1, b: 1, c: 1}",
                      "{}",
                      "{}",
                      "GgAAABBhAAEAAAAQYgABAAAAEGMAAQAAAAAFAAAAAAAAAAAAAAAAbm5ubgUAAAAABQAAAAAFAAAA"
                      "AGZ0ZkAAAABmCgAAAAMAAAB0yAAAAGYAAEAG6AMAAA==");

    // With sort
    testComputeSBEKey("{}",
                      "{a: 1}",
                      "{}",
                      "BQAAAAAFAAAAAH5hYQAAAAAAAAAAbm5ubgUAAAAABQAAAAAFAAAAAGZ0ZkAAAABmCgAAAAMAAAB0"
                      "yAAAAGYAAEAG6AMAAA==");
    testComputeSBEKey("{}",
                      "{a: -1}",
                      "{}",
                      "BQAAAAAFAAAAAH5kYQAAAAAAAAAAbm5ubgUAAAAABQAAAAAFAAAAAGZ0ZkAAAABmCgAAAAMAAAB0"
                      "yAAAAGYAAEAG6AMAAA==");
    testComputeSBEKey(
        "{a: 1}",
        "{a: 1}",
        "{}",
        "DAAAABBhAAEAAAAABQAAAAB+"
        "YWEAAAAAAAAAAG5ubm4FAAAAAAUAAAAABQAAAABmdGZAAAAAZgoAAAADAAAAdMgAAABmAABABugDAAA=");

    // With projection
    testComputeSBEKey("{a: 1}",
                      "{a: 1}",
                      "{a: 1}",
                      "DAAAABBhAAEAAAAADAAAABBhAAEAAAAAfmFhAAAAAAAAAABubm5uBQAAAAAFAAAAAAUAAAAAZnRm"
                      "QAAAAGYKAAAAAwAAAHTIAAAAZgAAQAboAwAA");
    testComputeSBEKey(
        "{}",
        "{a: 1}",
        "{a: 1}",
        "BQAAAAAMAAAAEGEAAQAAAAB+"
        "YWEAAAAAAAAAAG5ubm4FAAAAAAUAAAAABQAAAABmdGZAAAAAZgoAAAADAAAAdMgAAABmAABABugDAAA=");
    testComputeSBEKey("{}",
                      "{a: 1}",
                      "{a: 1, b: [{$const: 1}]}",
                      "BQAAAAAoAAAAEGEAAQAAAARiABkAAAADMAARAAAAECRjb25zdAABAAAAAAAAfmFhAAAAAAAAAABu"
                      "bm5uBQAAAAAFAAAAAAUAAAAAZnRmQAAAAGYKAAAAAwAAAHTIAAAAZgAAQAboAwAA");
    testComputeSBEKey("{}",
                      "{}",
                      "{a: 1}",
                      "BQAAAAAMAAAAEGEAAQAAAAAAAAAAAAAAAG5ubm4FAAAAAAUAAAAABQAAAABmdGZAAAAAZgoAAAAD"
                      "AAAAdMgAAABmAABABugDAAA=");
    testComputeSBEKey("{}",
                      "{}",
                      "{a: true}",
                      "BQAAAAAJAAAACGEAAQAAAAAAAAAAAG5ubm4FAAAAAAUAAAAABQAAAABmdGZAAAAAZgoAAAADAAAA"
                      "dMgAAABmAABABugDAAA=");
    testComputeSBEKey("{}",
                      "{}",
                      "{a: false}",
                      "BQAAAAAJAAAACGEAAAAAAAAAAAAAAG5ubm4FAAAAAAUAAAAABQAAAABmdGZAAAAAZgoAAAADAAAA"
                      "dMgAAABmAABABugDAAA=");

    // With FindCommandRequest
    auto findCommand = std::make_unique<FindCommandRequest>(nss);
    testComputeSBEKey(
        "{a: 1}",
        "{a: 1}",
        "{}",
        "DAAAABBhAAEAAAAABQAAAAB+"
        "YWEAAAAAAAAAAG5ubm4FAAAAAAUAAAAABQAAAABmdGZAAAAAZgoAAAADAAAAdMgAAABmAABABugDAAA=",
        std::move(findCommand));
    findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setAllowDiskUse(true);
    testComputeSBEKey(
        "{a: 1}",
        "{a: 1}",
        "{}",
        "DAAAABBhAAEAAAAABQAAAAB+"
        "YWEAAAAAAAAAAHRubm4FAAAAAAUAAAAABQAAAABmdGZAAAAAZgoAAAADAAAAdMgAAABmAABABugDAAA=",
        std::move(findCommand));
    findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setAllowDiskUse(false);
    testComputeSBEKey(
        "{a: 1}",
        "{a: 1}",
        "{}",
        "DAAAABBhAAEAAAAABQAAAAB+"
        "YWEAAAAAAAAAAGZubm4FAAAAAAUAAAAABQAAAABmdGZAAAAAZgoAAAADAAAAdMgAAABmAABABugDAAA=",
        std::move(findCommand));
    findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setReturnKey(true);
    testComputeSBEKey(
        "{a: 1}",
        "{a: 1}",
        "{}",
        "DAAAABBhAAEAAAAABQAAAAB+"
        "YWEAAAAAAAAAAG50bm4FAAAAAAUAAAAABQAAAABmdGZAAAAAZgoAAAADAAAAdMgAAABmAABABugDAAA=",
        std::move(findCommand));
    findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setRequestResumeToken(false);
    testComputeSBEKey(
        "{a: 1}",
        "{a: 1}",
        "{}",
        "DAAAABBhAAEAAAAABQAAAAB+"
        "YWEAAAAAAAAAAG5uZm4FAAAAAAUAAAAABQAAAABmdGZAAAAAZgoAAAADAAAAdMgAAABmAABABugDAAA=",
        std::move(findCommand));

    findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setSkip(10);
    testComputeSBEKey(
        "{a: 1}",
        "{a: 1}",
        "{}",
        "DAAAABBhAAEAAAAABQAAAAB+"
        "YWEKAAAAAAAAAAAAAABubm5uBQAAAAAFAAAAAAUAAAAAZnRmQAAAAGYKAAAAAwAAAHTIAAAAZgAAQAboAwAA",
        std::move(findCommand));

    findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setLimit(10);
    testComputeSBEKey(
        "{a: 1}",
        "{a: 1}",
        "{}",
        "DAAAABBhAAEAAAAABQAAAAB+"
        "YWEAAAAACgAAAAAAAABubm5uBQAAAAAFAAAAAAUAAAAAZnRmQAAAAGYKAAAAAwAAAHTIAAAAZgAAQAboAwAA",
        std::move(findCommand));

    findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setMin(mongo::fromjson("{ a : 1 }"));
    testComputeSBEKey(
        "{a: 1}",
        "{a: 1}",
        "{}",
        "DAAAABBhAAEAAAAABQAAAAB+"
        "YWEAAAAAAAAAAG5ubm4FAAAAAAwAAAAQYQABAAAAAAUAAAAAZnRmQAAAAGYKAAAAAwAAAHTIAAAAZgAAQAboAwAA",
        std::move(findCommand));
    findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setMax(mongo::fromjson("{ a : 1 }"));
    testComputeSBEKey(
        "{a: 1}",
        "{a: 1}",
        "{}",
        "DAAAABBhAAEAAAAABQAAAAB+"
        "YWEAAAAAAAAAAG5ubm4FAAAAAAUAAAAADAAAABBhAAEAAAAAZnRmQAAAAGYKAAAAAwAAAHTIAAAAZgAAQAboAwAA",
        std::move(findCommand));
    findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setRequestResumeToken(true);
    // "hint" must be {$natural:1} if 'requestResumeToken' is enabled.
    findCommand->setHint(fromjson("{$natural: 1}"));
    findCommand->setResumeAfter(mongo::fromjson("{ $recordId: NumberLong(1) }"));
    testComputeSBEKey("{a: 1}",
                      "{}",
                      "{}",
                      "DAAAABBhAAEAAAAABQAAAAAAAAAAAAAAAG5udG4YAAAAEiRyZWNvcmRJZAABAAAAAAAAAAAFAAAA"
                      "AAUAAAAAZnRmQAAAAGYKAAAAAwAAAHTIAAAAZgAAQAboAwAA",
                      std::move(findCommand));
}

}  // namespace
}  // namespace mongo

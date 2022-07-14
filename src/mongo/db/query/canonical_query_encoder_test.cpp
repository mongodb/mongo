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
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/inner_pipeline_stage_impl.h"
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
static const NamespaceString foreignNss("testdb.foreigncoll");

std::vector<std::unique_ptr<InnerPipelineStageInterface>> parsePipeline(
    const boost::intrusive_ptr<ExpressionContext> expCtx, const std::vector<BSONObj>& rawPipeline) {
    auto pipeline = Pipeline::parse(rawPipeline, expCtx);

    std::vector<std::unique_ptr<InnerPipelineStageInterface>> stages;
    for (auto&& source : pipeline->getSources()) {
        stages.emplace_back(std::make_unique<InnerPipelineStageImpl>(source));
    }
    return stages;
}

/**
 * Utility functions to create a CanonicalQuery
 */
unique_ptr<CanonicalQuery> canonicalize(BSONObj query,
                                        BSONObj sort,
                                        BSONObj proj,
                                        BSONObj collation,
                                        std::unique_ptr<FindCommandRequest> findCommand = nullptr,
                                        std::vector<BSONObj> pipelineObj = {}) {
    QueryTestServiceContext serviceContext;
    auto opCtx = serviceContext.makeOperationContext();

    if (!findCommand) {
        findCommand = std::make_unique<FindCommandRequest>(nss);
    }
    findCommand->setFilter(query.getOwned());
    findCommand->setSort(sort.getOwned());
    findCommand->setProjection(proj.getOwned());
    findCommand->setCollation(collation.getOwned());

    const auto expCtx = make_intrusive<ExpressionContextForTest>(opCtx.get(), nss);
    expCtx->addResolvedNamespaces({foreignNss});
    if (!findCommand->getCollation().isEmpty()) {
        auto statusWithCollator = CollatorFactoryInterface::get(opCtx->getServiceContext())
                                      ->makeFromBSON(findCommand->getCollation());
        ASSERT_OK(statusWithCollator.getStatus());
        expCtx->setCollator(std::move(statusWithCollator.getValue()));
    }
    auto pipeline = parsePipeline(expCtx, pipelineObj);

    auto statusWithCQ =
        CanonicalQuery::canonicalize(opCtx.get(),
                                     std::move(findCommand),
                                     false,
                                     expCtx,
                                     ExtensionsCallbackNoop(),
                                     MatchExpressionParser::kAllowAllSpecialFeatures,
                                     ProjectionPolicies::findProjectionPolicies(),
                                     std::move(pipeline));
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
                       std::unique_ptr<FindCommandRequest> findCommand = nullptr,
                       std::vector<BSONObj> pipelineObj = {}) {
    BSONObj collation;
    unique_ptr<CanonicalQuery> cq(
        canonicalize(query, sort, proj, collation, std::move(findCommand), std::move(pipelineObj)));
    cq->setSbeCompatible(true);
    const auto key = canonical_query_encoder::encodeSBE(*cq);
    ASSERT_EQUALS(key, expectedStr);
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
                       std::unique_ptr<FindCommandRequest> findCommand = nullptr,
                       std::vector<BSONObj> pipelineObj = {}) {
    testComputeSBEKey(fromjson(queryStr),
                      fromjson(sortStr),
                      fromjson(projStr),
                      expectedStr,
                      std::move(findCommand),
                      std::move(pipelineObj));
}

TEST(CanonicalQueryEncoderTest, ComputeKey) {
    // Generated cache keys should be treated as opaque to the user.

    // The computed key depends on which execution engine is enabled. As such, we disable SBE for
    // this test so that the test doesn't break should the default value of
    // 'internalQueryForceClassicEngine' change in the future.
    RAIIServerParameterControllerForTest controllerSBE("internalQueryForceClassicEngine", true);

    // No sorts
    testComputeKey("{}", "{}", "{}", "an@f");
    testComputeKey("{$or: [{a: 1}, {b: 2}]}", "{}", "{}", "or[eqa,eqb]@f");
    testComputeKey(
        "{$or: [{a: 1}, {b: 1}, {c: 1}], d: 1}", "{}", "{}", "an[or[eqa,eqb,eqc],eqd]@f");
    testComputeKey("{$or: [{a: 1}, {b: 1}], c: 1, d: 1}", "{}", "{}", "an[or[eqa,eqb],eqc,eqd]@f");
    testComputeKey("{a: 1, b: 1, c: 1}", "{}", "{}", "an[eqa,eqb,eqc]@f");
    testComputeKey("{a: 1, beqc: 1}", "{}", "{}", "an[eqa,eqbeqc]@f");
    testComputeKey("{ap1a: 1}", "{}", "{}", "eqap1a@f");
    testComputeKey("{aab: 1}", "{}", "{}", "eqaab@f");

    // With sort
    testComputeKey("{}", "{a: 1}", "{}", "an~aa@f");
    testComputeKey("{}", "{a: -1}", "{}", "an~da@f");
    testComputeKey("{$text: {$search: 'search keywords'}}",
                   "{a: {$meta: 'textScore'}}",
                   "{a: {$meta: 'textScore'}}",
                   "te_fts~ta@f");
    testComputeKey("{a: 1}", "{b: 1}", "{}", "eqa~ab@f");

    // With projection
    testComputeKey("{}", "{}", "{a: 1}", "an|_id-a@f");
    testComputeKey("{}", "{}", "{a: -1}", "an|_id-a@f");
    testComputeKey("{}", "{}", "{a: -1.0}", "an|_id-a@f");
    testComputeKey("{}", "{}", "{a: true}", "an|_id-a@f");
    testComputeKey("{}", "{}", "{a: 0}", "an@f");
    testComputeKey("{}", "{}", "{a: false}", "an@f");
    testComputeKey("{}", "{}", "{a: 99}", "an|_id-a@f");
    testComputeKey("{}", "{}", "{a: 'foo'}", "an|_id@f");

    // $slice defaults to exclusion.
    testComputeKey("{}", "{}", "{a: {$slice: [3, 5]}}", "an@f");
    testComputeKey("{}", "{}", "{a: {$slice: [3, 5]}, b: 0}", "an@f");

    // But even when using $slice in an inclusion, the entire document is needed.
    testComputeKey("{}", "{}", "{a: {$slice: [3, 5]}, b: 1}", "an@f");

    testComputeKey("{}", "{}", "{a: {$elemMatch: {x: 2}}}", "an@f");
    testComputeKey("{}", "{}", "{a: {$elemMatch: {x: 2}}, b: 0}", "an@f");
    testComputeKey("{}", "{}", "{a: {$elemMatch: {x: 2}}, b: 1}", "an@f");

    testComputeKey("{}", "{}", "{a: {$slice: [3, 5]}, b: {$elemMatch: {x: 2}}}", "an@f");

    testComputeKey("{}", "{}", "{a: ObjectId('507f191e810c19729de860ea')}", "an|_id@f");
    // Since this projection overwrites the entire document, no fields are required.
    testComputeKey(
        "{}", "{}", "{_id: 0, a: ObjectId('507f191e810c19729de860ea'), b: 'foo'}", "an|@f");
    testComputeKey("{a: 1}", "{}", "{'a.$': 1}", "eqa@f");
    testComputeKey("{a: 1}", "{}", "{a: 1}", "eqa|_id-a@f");

    // Projection should be order-insensitive
    testComputeKey("{}", "{}", "{a: 1, b: 1}", "an|_id-a-b@f");
    testComputeKey("{}", "{}", "{b: 1, a: 1}", "an|_id-a-b@f");

    // And should escape the separation character.
    testComputeKey("{}", "{}", "{'b-1': 1, 'a-2': 1}", "an|_id-a\\-2-b\\-1@f");

    // And should exclude $-prefixed fields which can be added internally.
    testComputeKey("{}", "{x: 1}", "{$sortKey: {$meta: 'sortKey'}}", "an~ax@f");
    testComputeKey("{}", "{}", "{}", "an@f");

    testComputeKey("{}", "{x: 1}", "{a: 1, $sortKey: {$meta: 'sortKey'}}", "an~ax|_id-a@f");
    testComputeKey("{}", "{}", "{a: 1}", "an|_id-a@f");

    // With or-elimination and projection
    testComputeKey("{$or: [{a: 1}]}", "{}", "{_id: 0, a: 1}", "eqa|a@f");
    testComputeKey("{$or: [{a: 1}]}", "{}", "{'a.$': 1}", "eqa@f");
}

TEST(CanonicalQueryEncoderTest, EncodeNotEqualNullPredicates) {
    // The computed key depends on which execution engine is enabled. As such, we disable SBE for
    // this test so that the test doesn't break should the default value of
    // 'internalQueryForceClassicEngine' change in the future.
    RAIIServerParameterControllerForTest controllerSBE("internalQueryForceClassicEngine", true);

    // With '$eq', '$gte', and '$lte' negation comparison to 'null'.
    testComputeKey("{a: {$not: {$eq: null}}}", "{}", "{_id: 0, a: 1}", "ntnot_eq_null[eqa]|a@f");
    testComputeKey(
        "{a: {$not: {$eq: null}}}", "{a: 1}", "{_id: 0, a: 1}", "ntnot_eq_null[eqa]~aa|a@f");
    testComputeKey(
        "{a: {$not: {$gte: null}}}", "{a: 1}", "{_id: 0, a: 1}", "ntnot_eq_null[gea]~aa|a@f");
    testComputeKey(
        "{a: {$not: {$lte: null}}}", "{a: 1}", "{_id: 0, a: 1}", "ntnot_eq_null[lea]~aa|a@f");

    // Same '$eq' negation query with non-'null' argument should have different key.
    testComputeKey("{a: {$not: {$eq: true}}}", "{a: 1}", "{_id: 0, a: 1}", "nt[eqa]~aa|a@f");
}

// Delimiters found in user field names or non-standard projection field values
// must be escaped.
TEST(CanonicalQueryEncoderTest, ComputeKeyEscaped) {
    // The computed key depends on which execution engine is enabled. As such, we disable SBE for
    // this test so that the test doesn't break should the default value of
    // 'internalQueryForceClassicEngine' change in the future.
    RAIIServerParameterControllerForTest controllerSBE("internalQueryForceClassicEngine", true);
    // Field name in query.
    testComputeKey("{'a,[]~|-<>': 1}", "{}", "{}", "eqa\\,\\[\\]\\~\\|\\-<>@f");

    // Field name in sort.
    testComputeKey("{}", "{'a,[]~|-<>': 1}", "{}", "an~aa\\,\\[\\]\\~\\|\\-<>@f");

    // Field name in projection.
    testComputeKey("{}", "{}", "{'a,[]~|-<>': 1}", "an|_id-a\\,\\[\\]\\~\\|\\-<>@f");

    // String literal provided as value.
    testComputeKey("{}", "{}", "{a: 'foo,[]~|-<>'}", "an|_id@f");
}

// Cache keys for $geoWithin queries with legacy and GeoJSON coordinates should
// not be the same.
TEST(CanonicalQueryEncoderTest, ComputeKeyGeoWithin) {
    // Legacy coordinates.
    unique_ptr<CanonicalQuery> cqLegacy(
        canonicalize("{a: {$geoWithin: "
                     "{$box: [[-180, -90], [180, 90]]}}}"));
    // GeoJSON coordinates.
    unique_ptr<CanonicalQuery> cqNew(
        canonicalize("{a: {$geoWithin: "
                     "{$geometry: {type: 'Polygon', coordinates: "
                     "[[[0, 0], [0, 90], [90, 0], [0, 0]]]}}}}"));
    ASSERT_NOT_EQUALS(canonical_query_encoder::encode(*cqLegacy),
                      canonical_query_encoder::encode(*cqNew));
}

// GEO_NEAR cache keys should include information on geometry and CRS in addition
// to the match type and field name.
TEST(CanonicalQueryEncoderTest, ComputeKeyGeoNear) {
    // The computed key depends on which execution engine is enabled. As such, we disable SBE for
    // this test so that the test doesn't break should the default value of
    // 'internalQueryForceClassicEngine' change in the future.
    RAIIServerParameterControllerForTest controllerSBE("internalQueryForceClassicEngine", true);

    testComputeKey("{a: {$near: [0,0], $maxDistance:0.3 }}", "{}", "{}", "gnanrfl@f");
    testComputeKey("{a: {$nearSphere: [0,0], $maxDistance: 0.31 }}", "{}", "{}", "gnanssp@f");
    testComputeKey(
        "{a: {$geoNear: {$geometry: {type: 'Point', coordinates: [0,0]},"
        "$maxDistance:100}}}",
        "{}",
        "{}",
        "gnanrsp@f");
}

TEST(CanonicalQueryEncoderTest, ComputeKeyRegexDependsOnFlags) {
    // The computed key depends on which execution engine is enabled. As such, we enable SBE for
    // this test in order to ensure that we have coverage for both SBE and the classic engine.
    RAIIServerParameterControllerForTest controllerSBE("internalQueryForceClassicEngine", false);
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
    // The computed key depends on which execution engine is enabled. As such, we disable SBE for
    // this test so that the test doesn't break should the default value of
    // 'internalQueryForceClassicEngine' change in the future.
    RAIIServerParameterControllerForTest controllerSBE("internalQueryForceClassicEngine", true);

    // Test that an $in containing a single regex is unwrapped to $regex.
    testComputeKey("{a: {$in: [/foo/]}}", "{}", "{}", "rea@f");
    testComputeKey("{a: {$in: [/foo/i]}}", "{}", "{}", "rea/i/@f");

    // Test that an $in with no regexes does not include any regex information.
    testComputeKey("{a: {$in: [1, 'foo']}}", "{}", "{}", "ina@f");

    // Test that an $in with a regex encodes the presence of the regex.
    testComputeKey("{a: {$in: [1, /foo/]}}", "{}", "{}", "ina_re@f");

    // Test that an $in with a regex encodes the presence of the regex and its flags.
    testComputeKey("{a: {$in: [1, /foo/is]}}", "{}", "{}", "ina_re/is/@f");

    // Test that the computed key is invariant to the order of the flags within each regex.
    testComputeKey("{a: {$in: [1, /foo/si]}}", "{}", "{}", "ina_re/is/@f");

    // Test that an $in with multiple regexes encodes all unique flags.
    testComputeKey("{a: {$in: [1, /foo/i, /bar/m, /baz/s]}}", "{}", "{}", "ina_re/ims/@f");

    // Test that an $in with multiple regexes deduplicates identical flags.
    testComputeKey(
        "{a: {$in: [1, /foo/i, /bar/m, /baz/s, /qux/i, /quux/s]}}", "{}", "{}", "ina_re/ims/@f");

    // Test that the computed key is invariant to the ordering of the flags across regexes.
    testComputeKey("{a: {$in: [1, /foo/ism, /bar/msi, /baz/im, /qux/si, /quux/im]}}",
                   "{}",
                   "{}",
                   "ina_re/ims/@f");
    testComputeKey("{a: {$in: [1, /foo/msi, /bar/ism, /baz/is, /qux/mi, /quux/im]}}",
                   "{}",
                   "{}",
                   "ina_re/ims/@f");

    // Test that $not-$in-$regex similarly records the presence and flags of any regexes.
    testComputeKey("{a: {$not: {$in: [1, 'foo']}}}", "{}", "{}", "nt[ina]@f");
    testComputeKey("{a: {$not: {$in: [1, /foo/]}}}", "{}", "{}", "nt[ina_re]@f");
    testComputeKey(
        "{a: {$not: {$in: [1, /foo/i, /bar/i, /baz/msi]}}}", "{}", "{}", "nt[ina_re/ims/]@f");

    // Test that a $not-$in containing a single regex is unwrapped to $not-$regex.
    testComputeKey("{a: {$not: {$in: [/foo/]}}}", "{}", "{}", "nt[rea]@f");
    testComputeKey("{a: {$not: {$in: [/foo/i]}}}", "{}", "{}", "nt[rea/i/]@f");
}

TEST(CanonicalQueryEncoderTest, CheckCollationIsEncoded) {
    // The computed key depends on which execution engine is enabled. As such, we disable SBE for
    // this test so that the test doesn't break should the default value of
    // 'internalQueryForceClassicEngine' change in the future.
    RAIIServerParameterControllerForTest controllerSBE("internalQueryForceClassicEngine", true);

    unique_ptr<CanonicalQuery> cq(canonicalize(
        fromjson("{a: 1, b: 1}"), {}, {}, fromjson("{locale: 'mock_reverse_string'}")));

    testComputeKey(*cq, "an[eqa,eqb]#mock_reverse_string02300000@f");
}

TEST(CanonicalQueryEncoderTest, ComputeKeySBE) {
    // Generated cache keys should be treated as opaque to the user.

    // SBE must be enabled in order to generate SBE plan cache keys.
    RAIIServerParameterControllerForTest controllerSBE("internalQueryForceClassicEngine", false);

    RAIIServerParameterControllerForTest controllerSBEPlanCache("featureFlagSbeFull", true);

    testComputeSBEKey("{}", "{}", "{}", "YW4ABQAAAAAAAAAAAAAAAG5ubm4FAAAAAGZe");
    testComputeSBEKey("{$or: [{a: 1}, {b: 2}]}",
                      "{}",
                      "{}",
                      "b3IAW2VxAGE/AAAAACxlcQBiPwEAAABdBQAAAAAAAAAAAAAAAG5ubm4FAAAAAGZe");
    testComputeSBEKey("{a: 1}", "{}", "{}", "ZXEAYT8AAAAABQAAAAAAAAAAAAAAAG5ubm4FAAAAAGZe");
    testComputeSBEKey("{b: 1}", "{}", "{}", "ZXEAYj8AAAAABQAAAAAAAAAAAAAAAG5ubm4FAAAAAGZe");
    testComputeSBEKey(
        "{a: 1, b: 1, c: 1}",
        "{}",
        "{}",
        "YW4AW2VxAGE/AAAAACxlcQBiPwEAAAAsZXEAYz8CAAAAXQUAAAAAAAAAAAAAAABubm5uBQAAAABmXg==");

    // With sort
    testComputeSBEKey("{}", "{a: 1}", "{}", "YW4ABQAAAAB+YWEAAAAAAAAAAG5ubm4FAAAAAGZe");
    testComputeSBEKey("{}", "{a: -1}", "{}", "YW4ABQAAAAB+ZGEAAAAAAAAAAG5ubm4FAAAAAGZe");
    testComputeSBEKey("{a: 1}", "{a: 1}", "{}", "ZXEAYT8AAAAABQAAAAB+YWEAAAAAAAAAAG5ubm4FAAAAAGZe");

    // With projection
    testComputeSBEKey("{a: 1}",
                      "{a: 1}",
                      "{a: 1}",
                      "ZXEAYT8AAAAADAAAABBhAAEAAAAAfmFhAAAAAAAAAABubm5uBQAAAABmXg==");
    testComputeSBEKey(
        "{}", "{a: 1}", "{a: 1}", "YW4ADAAAABBhAAEAAAAAfmFhAAAAAAAAAABubm5uBQAAAABmXg==");
    testComputeSBEKey(
        "{}",
        "{a: 1}",
        "{a: 1, b: [{$const: 1}]}",
        "YW4AKAAAABBhAAEAAAAEYgAZAAAAAzAAEQAAABAkY29uc3QAAQAAAAAAAH5hYQAAAAAAAAAAbm5ubgUAAAAAZl4=");
    testComputeSBEKey("{}", "{}", "{a: 1}", "YW4ADAAAABBhAAEAAAAAAAAAAAAAAABubm5uBQAAAABmXg==");
    testComputeSBEKey("{}", "{}", "{a: true}", "YW4ACQAAAAhhAAEAAAAAAAAAAABubm5uBQAAAABmXg==");
    testComputeSBEKey("{}", "{}", "{a: false}", "YW4ACQAAAAhhAAAAAAAAAAAAAABubm5uBQAAAABmXg==");

    // With FindCommandRequest
    auto findCommand = std::make_unique<FindCommandRequest>(nss);
    testComputeSBEKey("{a: 1}",
                      "{a: 1}",
                      "{}",
                      "ZXEAYT8AAAAABQAAAAB+YWEAAAAAAAAAAG5ubm4FAAAAAGZe",
                      std::move(findCommand));
    findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setAllowDiskUse(true);
    testComputeSBEKey("{a: 1}",
                      "{a: 1}",
                      "{}",
                      "ZXEAYT8AAAAABQAAAAB+YWEAAAAAAAAAAHRubm4FAAAAAGZe",
                      std::move(findCommand));
    findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setAllowDiskUse(false);
    testComputeSBEKey("{a: 1}",
                      "{a: 1}",
                      "{}",
                      "ZXEAYT8AAAAABQAAAAB+YWEAAAAAAAAAAGZubm4FAAAAAGZe",
                      std::move(findCommand));
    findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setReturnKey(true);
    testComputeSBEKey("{a: 1}",
                      "{a: 1}",
                      "{}",
                      "ZXEAYT8AAAAABQAAAAB+YWEAAAAAAAAAAG50bm4FAAAAAGZe",
                      std::move(findCommand));
    findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setRequestResumeToken(false);
    testComputeSBEKey("{a: 1}",
                      "{a: 1}",
                      "{}",
                      "ZXEAYT8AAAAABQAAAAB+YWEAAAAAAAAAAG5uZm4FAAAAAGZe",
                      std::move(findCommand));

    findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setSkip(10);
    testComputeSBEKey("{a: 1}",
                      "{a: 1}",
                      "{}",
                      "ZXEAYT8AAAAABQAAAAB+YWEKAAAAAAAAAAAAAABubm5uBQAAAABmXg==",
                      std::move(findCommand));

    findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setLimit(10);
    testComputeSBEKey("{a: 1}",
                      "{a: 1}",
                      "{}",
                      "ZXEAYT8AAAAABQAAAAB+YWEAAAAACgAAAAAAAABubm5uBQAAAABmXg==",
                      std::move(findCommand));

    findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setMin(mongo::fromjson("{ a : 1 }"));
    testComputeSBEKey("{a: 1}",
                      "{a: 1}",
                      "{}",
                      "ZXEAYT8AAAAABQAAAAB+YWEAAAAAAAAAAG5ubm4FAAAAAGZe",
                      std::move(findCommand));
    findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setMax(mongo::fromjson("{ a : 1 }"));
    testComputeSBEKey("{a: 1}",
                      "{a: 1}",
                      "{}",
                      "ZXEAYT8AAAAABQAAAAB+YWEAAAAAAAAAAG5ubm4FAAAAAGZe",
                      std::move(findCommand));
    findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setRequestResumeToken(true);
    // "hint" must be {$natural:1} if 'requestResumeToken' is enabled.
    findCommand->setHint(fromjson("{$natural: 1}"));
    findCommand->setResumeAfter(mongo::fromjson("{ $recordId: NumberLong(1) }"));
    testComputeSBEKey("{a: 1}",
                      "{}",
                      "{}",
                      "ZXEAYT8AAAAABQAAAAAAAAAAAAAAAG5udG4YAAAAEiRyZWNvcmRJZAABAAAAAAAAAABmXg==",
                      std::move(findCommand));
}

TEST(CanonicalQueryEncoderTest, ComputeKeySBEWithPipeline) {
    // SBE must be enabled in order to generate SBE plan cache keys.
    RAIIServerParameterControllerForTest controllerSBE("internalQueryForceClassicEngine", false);

    RAIIServerParameterControllerForTest controllerSBEPlanCache("featureFlagSbeFull", true);

    auto getLookupBson = [](StringData localField, StringData foreignField, StringData asField) {
        return BSON("$lookup" << BSON("from" << foreignNss.coll() << "localField" << localField
                                             << "foreignField" << foreignField << "as" << asField));
    };

    // No pipeline stage.
    testComputeSBEKey(
        "{a: 1}", "{}", "{}", "ZXEAYT8AAAAABQAAAAAAAAAAAAAAAG5ubm4FAAAAAGZe", nullptr, {});

    // Different $lookup stage options.
    testComputeSBEKey(
        "{a: 1}",
        "{}",
        "{}",
        "ZXEAYT8AAAAABQAAAAAAAAAAAAAAAG5ubm4FAAAAAGZeWgAAAAMkbG9va3VwAEwAAAACZnJvbQAMAAAAZm9yZWlnbm"
        "NvbGwAAmFzAAMAAABhcwACbG9jYWxGaWVsZAACAAAAYQACZm9yZWlnbkZpZWxkAAIAAABiAAAA",
        nullptr,
        {getLookupBson("a", "b", "as")});
    testComputeSBEKey(
        "{a: 1}",
        "{}",
        "{}",
        "ZXEAYT8AAAAABQAAAAAAAAAAAAAAAG5ubm4FAAAAAGZeWwAAAAMkbG9va3VwAE0AAAACZnJvbQAMAAAAZm9yZWlnbm"
        "NvbGwAAmFzAAMAAABhcwACbG9jYWxGaWVsZAADAAAAYTEAAmZvcmVpZ25GaWVsZAACAAAAYgAAAA==",
        nullptr,
        {getLookupBson("a1", "b", "as")});
    testComputeSBEKey(
        "{a: 1}",
        "{}",
        "{}",
        "ZXEAYT8AAAAABQAAAAAAAAAAAAAAAG5ubm4FAAAAAGZeWwAAAAMkbG9va3VwAE0AAAACZnJvbQAMAAAAZm9yZWlnbm"
        "NvbGwAAmFzAAMAAABhcwACbG9jYWxGaWVsZAACAAAAYQACZm9yZWlnbkZpZWxkAAMAAABiMQAAAA==",
        nullptr,
        {getLookupBson("a", "b1", "as")});
    testComputeSBEKey(
        "{a: 1}",
        "{}",
        "{}",
        "ZXEAYT8AAAAABQAAAAAAAAAAAAAAAG5ubm4FAAAAAGZeWwAAAAMkbG9va3VwAE0AAAACZnJvbQAMAAAAZm9yZWlnbm"
        "NvbGwAAmFzAAQAAABhczEAAmxvY2FsRmllbGQAAgAAAGEAAmZvcmVpZ25GaWVsZAACAAAAYgAAAA==",
        nullptr,
        {getLookupBson("a", "b", "as1")});

    // Multiple $lookup stages.
    testComputeSBEKey("{a: 1}",
                      "{}",
                      "{}",
                      "ZXEAYT8AAAAABQAAAAAAAAAAAAAAAG5ubm4FAAAAAGZeWgAAAAMkbG9va3VwAEwAAAACZnJvbQAM"
                      "AAAAZm9yZWlnbmNvbGwAAmFzAAMAAABhcwACbG9jYWxGaWVsZAACAAAAYQACZm9yZWlnbkZpZWxk"
                      "AAIAAABiAAAAXQAAAAMkbG9va3VwAE8AAAACZnJvbQAMAAAAZm9yZWlnbmNvbGwAAmFzAAQAAABh"
                      "czEAAmxvY2FsRmllbGQAAwAAAGExAAJmb3JlaWduRmllbGQAAwAAAGIxAAAA",
                      nullptr,
                      {getLookupBson("a", "b", "as"), getLookupBson("a1", "b1", "as1")});
}

TEST(CanonicalQueryEncoderTest, ComputeKeySBEWithReadConcern) {
    // SBE must be enabled in order to generate SBE plan cache keys.
    RAIIServerParameterControllerForTest controllerSBE("internalQueryForceClassicEngine", false);
    RAIIServerParameterControllerForTest controllerSBEPlanCache("featureFlagSbeFull", true);

    const auto sbeEncodingWithoutReadConcernAvailable =
        "ZXEAYT8AAAAABQAAAAB+YWEAAAAAAAAAAG5ubm4FAAAAAGZe";
    const auto sbeEncodingWithReadConcernAvailable =
        "ZXEAYT8AAAAABQAAAAB+YWEAAAAAAAAAAG5ubm4FAAAAAHRe";

    // Find command without read concern.
    auto findCommand = std::make_unique<FindCommandRequest>(nss);
    testComputeSBEKey(
        "{a: 1}", "{a: 1}", "{}", sbeEncodingWithoutReadConcernAvailable, std::move(findCommand));

    // Find command with read concern "majority".
    findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setReadConcern(
        repl::ReadConcernArgs(repl::ReadConcernLevel::kMajorityReadConcern).toBSONInner());
    testComputeSBEKey(
        "{a: 1}", "{a: 1}", "{}", sbeEncodingWithoutReadConcernAvailable, std::move(findCommand));

    // Find command with read concern "available".
    findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setReadConcern(
        repl::ReadConcernArgs(repl::ReadConcernLevel::kAvailableReadConcern).toBSONInner());
    testComputeSBEKey(
        "{a: 1}", "{a: 1}", "{}", sbeEncodingWithReadConcernAvailable, std::move(findCommand));
    ASSERT_NOT_EQUALS(sbeEncodingWithoutReadConcernAvailable, sbeEncodingWithReadConcernAvailable);
}

}  // namespace
}  // namespace mongo

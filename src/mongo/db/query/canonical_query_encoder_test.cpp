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
#include "mongo/db/query/canonical_query_test_util.h"
#include "mongo/db/query/plan_cache_key_factory.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/unittest/golden_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {

using std::unique_ptr;

static const NamespaceString foreignNss =
    NamespaceString::createNamespaceString_forTest("testdb.foreigncoll");

unittest::GoldenTestConfig goldenTestConfig{"src/mongo/db/test_output/query"};

std::vector<std::unique_ptr<InnerPipelineStageInterface>> parsePipeline(
    const boost::intrusive_ptr<ExpressionContext> expCtx, const std::vector<BSONObj>& rawPipeline) {
    auto pipeline = Pipeline::parse(rawPipeline, expCtx);

    std::vector<std::unique_ptr<InnerPipelineStageInterface>> stages;
    for (auto&& source : pipeline->getSources()) {
        stages.emplace_back(std::make_unique<InnerPipelineStageImpl>(source));
    }
    return stages;
}

class CanonicalQueryEncoderTest : public CanonicalQueryTest {
protected:
    unique_ptr<CanonicalQuery> canonicalize(
        OperationContext* opCtx,
        BSONObj query,
        BSONObj sort,
        BSONObj proj,
        BSONObj collation,
        std::unique_ptr<FindCommandRequest> findCommand = nullptr,
        std::vector<BSONObj> pipelineObj = {},
        bool isCountLike = false,
        bool needsMerge = false) {
        if (!findCommand) {
            findCommand = std::make_unique<FindCommandRequest>(nss);
        }
        findCommand->setFilter(query.getOwned());
        findCommand->setSort(sort.getOwned());
        findCommand->setProjection(proj.getOwned());
        findCommand->setCollation(collation.getOwned());

        const auto expCtx = make_intrusive<ExpressionContextForTest>(opCtx, nss);
        expCtx->addResolvedNamespaces({foreignNss});
        expCtx->needsMerge = needsMerge;
        if (!findCommand->getCollation().isEmpty()) {
            auto statusWithCollator = CollatorFactoryInterface::get(opCtx->getServiceContext())
                                          ->makeFromBSON(findCommand->getCollation());
            ASSERT_OK(statusWithCollator.getStatus());
            expCtx->setCollator(std::move(statusWithCollator.getValue()));
        }
        auto pipeline = parsePipeline(expCtx, pipelineObj);

        auto statusWithCQ =
            CanonicalQuery::canonicalize(opCtx,
                                         std::move(findCommand),
                                         false,
                                         expCtx,
                                         ExtensionsCallbackNoop(),
                                         MatchExpressionParser::kAllowAllSpecialFeatures,
                                         ProjectionPolicies::findProjectionPolicies(),
                                         std::move(pipeline),
                                         isCountLike);
        ASSERT_OK(statusWithCQ.getStatus());
        return std::move(statusWithCQ.getValue());
    }

    unique_ptr<CanonicalQuery> canonicalize(OperationContext* opCtx, const char* queryStr) {
        BSONObj queryObj = fromjson(queryStr);
        return canonicalize(opCtx, queryObj, {}, {}, {});
    }

    /**
     * Test functions for computeKey, when no indexes are present. Cache keys are intentionally
     * obfuscated and are meaningful only within the current lifetime of the server process. Users
     * should treat plan cache keys as opaque.
     */
    void testComputeKey(unittest::GoldenTestContext& gctx, const CanonicalQuery& cq) {
        gctx.outStream() << "==== VARIATION: cq=" << cq.toString() << std::endl;
        const auto key = cq.encodeKey();
        gctx.outStream() << key << std::endl;
    }

    void testComputeKey(unittest::GoldenTestContext& gctx,
                        BSONObj query,
                        BSONObj sort,
                        BSONObj proj) {
        BSONObj collation;
        gctx.outStream() << "==== VARIATION: query=" << query << ", sort=" << sort
                         << ", proj=" << proj << std::endl;
        unique_ptr<CanonicalQuery> cq(canonicalize(opCtx(), query, sort, proj, collation));
        const auto key = cq->encodeKey();
        gctx.outStream() << key << std::endl;
    }

    void testComputeKey(unittest::GoldenTestContext& gctx,
                        const char* queryStr,
                        const char* sortStr,
                        const char* projStr) {
        testComputeKey(gctx, fromjson(queryStr), fromjson(sortStr), fromjson(projStr));
    }

    void testComputeSBEKey(unittest::GoldenTestContext& gctx,
                           const char* queryStr,
                           const char* sortStr,
                           const char* projStr,
                           std::unique_ptr<FindCommandRequest> findCommand = nullptr,
                           std::vector<BSONObj> pipelineObj = {},
                           bool isCountLike = false,
                           bool needsMerge = false) {
        auto& stream = gctx.outStream();
        stream << "==== VARIATION: sbe, query=" << queryStr << ", sort=" << sortStr
               << ", proj=" << projStr;
        if (findCommand) {
            stream << ", hint=" << findCommand->getHint().toString()
                   << ", allowDiskUse=" << findCommand->getAllowDiskUse()
                   << ", returnKey=" << findCommand->getReturnKey()
                   << ", requestResumeToken=" << findCommand->getRequestResumeToken();
        }
        if (isCountLike) {
            stream << ", isCountLike=true";
        }
        if (needsMerge) {
            stream << ", needsMerge=true";
        }
        stream << std::endl;
        BSONObj collation;
        unique_ptr<CanonicalQuery> cq(canonicalize(opCtx(),
                                                   fromjson(queryStr),
                                                   fromjson(sortStr),
                                                   fromjson(projStr),
                                                   collation,
                                                   std::move(findCommand),
                                                   std::move(pipelineObj),
                                                   isCountLike,
                                                   needsMerge));
        cq->setSbeCompatible(true);
        const auto key = canonical_query_encoder::encodeSBE(*cq);
        gctx.outStream() << key << std::endl;
    }
};

TEST_F(CanonicalQueryEncoderTest, ComputeKey) {
    unittest::GoldenTestContext gctx(&goldenTestConfig);
    // Generated cache keys should be treated as opaque to the user.

    // The computed key depends on which execution engine is enabled. As such, we disable SBE for
    // this test so that the test doesn't break should the default value of
    // 'internalQueryFrameworkControl' change in the future.
    RAIIServerParameterControllerForTest controllerSBE("internalQueryFrameworkControl",
                                                       "forceClassicEngine");

    // No sorts
    testComputeKey(gctx, "{}", "{}", "{}");
    testComputeKey(gctx, "{$or: [{a: 1}, {b: 2}]}", "{}", "{}");
    testComputeKey(gctx, "{$or: [{a: 1}, {b: 1}, {c: 1}], d: 1}", "{}", "{}");
    testComputeKey(gctx, "{$or: [{a: 1}, {b: 1}], c: 1, d: 1}", "{}", "{}");
    testComputeKey(gctx, "{a: 1, b: 1, c: 1}", "{}", "{}");
    testComputeKey(gctx, "{a: 1, beqc: 1}", "{}", "{}");
    testComputeKey(gctx, "{ap1a: 1}", "{}", "{}");
    testComputeKey(gctx, "{aab: 1}", "{}", "{}");

    // With sort
    testComputeKey(gctx, "{}", "{a: 1}", "{}");
    testComputeKey(gctx, "{}", "{a: -1}", "{}");
    testComputeKey(gctx,
                   "{$text: {$search: 'search keywords'}}",
                   "{a: {$meta: 'textScore'}}",
                   "{a: {$meta: 'textScore'}}");
    testComputeKey(gctx, "{a: 1}", "{b: 1}", "{}");

    // With projection
    testComputeKey(gctx, "{}", "{}", "{a: 1}");
    testComputeKey(gctx, "{}", "{}", "{a: -1}");
    testComputeKey(gctx, "{}", "{}", "{a: -1.0}");
    testComputeKey(gctx, "{}", "{}", "{a: true}");
    testComputeKey(gctx, "{}", "{}", "{a: 0}");
    testComputeKey(gctx, "{}", "{}", "{a: false}");
    testComputeKey(gctx, "{}", "{}", "{a: 99}");
    testComputeKey(gctx, "{}", "{}", "{a: 'foo'}");

    // $slice defaults to exclusion.
    testComputeKey(gctx, "{}", "{}", "{a: {$slice: [3, 5]}}");
    testComputeKey(gctx, "{}", "{}", "{a: {$slice: [3, 5]}, b: 0}");

    // But even when using $slice in an inclusion, the entire document is needed.
    testComputeKey(gctx, "{}", "{}", "{a: {$slice: [3, 5]}, b: 1}");

    testComputeKey(gctx, "{}", "{}", "{a: {$elemMatch: {x: 2}}}");
    testComputeKey(gctx, "{}", "{}", "{a: {$elemMatch: {x: 2}}, b: 0}");
    testComputeKey(gctx, "{}", "{}", "{a: {$elemMatch: {x: 2}}, b: 1}");

    testComputeKey(gctx, "{}", "{}", "{a: {$slice: [3, 5]}, b: {$elemMatch: {x: 2}}}");

    testComputeKey(gctx, "{}", "{}", "{a: ObjectId('507f191e810c19729de860ea')}");
    // Since this projection overwrites the entire document, no fields are required.
    testComputeKey(gctx, "{}", "{}", "{_id: 0, a: ObjectId('507f191e810c19729de860ea'), b: 'foo'}");
    testComputeKey(gctx, "{a: 1}", "{}", "{'a.$': 1}");
    testComputeKey(gctx, "{a: 1}", "{}", "{a: 1}");

    // Projection should be order-insensitive
    testComputeKey(gctx, "{}", "{}", "{a: 1, b: 1}");
    testComputeKey(gctx, "{}", "{}", "{b: 1, a: 1}");

    // And should escape the separation character.
    testComputeKey(gctx, "{}", "{}", "{'b-1': 1, 'a-2': 1}");

    // And should exclude $-prefixed fields which can be added internally.
    testComputeKey(gctx, "{}", "{x: 1}", "{$sortKey: {$meta: 'sortKey'}}");
    testComputeKey(gctx, "{}", "{}", "{}");

    testComputeKey(gctx, "{}", "{x: 1}", "{a: 1, $sortKey: {$meta: 'sortKey'}}");
    testComputeKey(gctx, "{}", "{}", "{a: 1}");

    // With or-elimination and projection
    testComputeKey(gctx, "{$or: [{a: 1}]}", "{}", "{_id: 0, a: 1}");
    testComputeKey(gctx, "{$or: [{a: 1}]}", "{}", "{'a.$': 1}");
}

TEST_F(CanonicalQueryEncoderTest, EncodeNotEqualNullPredicates) {
    unittest::GoldenTestContext gctx(&goldenTestConfig);
    // The computed key depends on which execution engine is enabled. As such, we disable SBE for
    // this test so that the test doesn't break should the default value of
    // 'internalQueryFrameworkControl' change in the future.
    RAIIServerParameterControllerForTest controllerSBE("internalQueryFrameworkControl",
                                                       "forceClassicEngine");

    // With '$eq', '$gte', and '$lte' negation comparison to 'null'.
    testComputeKey(gctx, "{a: {$not: {$eq: null}}}", "{}", "{_id: 0, a: 1}");
    testComputeKey(gctx, "{a: {$not: {$eq: null}}}", "{a: 1}", "{_id: 0, a: 1}");
    testComputeKey(gctx, "{a: {$not: {$gte: null}}}", "{a: 1}", "{_id: 0, a: 1}");
    testComputeKey(gctx, "{a: {$not: {$lte: null}}}", "{a: 1}", "{_id: 0, a: 1}");

    // Same '$eq' negation query with non-'null' argument should have different key.
    testComputeKey(gctx, "{a: {$not: {$eq: true}}}", "{a: 1}", "{_id: 0, a: 1}");
}

// Delimiters found in user field names or non-standard projection field values
// must be escaped.
TEST_F(CanonicalQueryEncoderTest, ComputeKeyEscaped) {
    unittest::GoldenTestContext gctx(&goldenTestConfig);
    // The computed key depends on which execution engine is enabled. As such, we disable SBE for
    // this test so that the test doesn't break should the default value of
    // 'internalQueryFrameworkControl' change in the future.
    RAIIServerParameterControllerForTest controllerSBE("internalQueryFrameworkControl",
                                                       "forceClassicEngine");
    // Field name in query.
    testComputeKey(gctx, "{'a,[]~|-<>': 1}", "{}", "{}");

    // Field name in sort.
    testComputeKey(gctx, "{}", "{'a,[]~|-<>': 1}", "{}");

    // Field name in projection.
    testComputeKey(gctx, "{}", "{}", "{'a,[]~|-<>': 1}");

    // String literal provided as value.
    testComputeKey(gctx, "{}", "{}", "{a: 'foo,[]~|-<>'}");
}

// Cache keys for $geoWithin queries with legacy and GeoJSON coordinates should
// not be the same.
TEST_F(CanonicalQueryEncoderTest, ComputeKeyGeoWithin) {
    // Legacy coordinates.
    unique_ptr<CanonicalQuery> cqLegacy(canonicalize(opCtx(),
                                                     "{a: {$geoWithin: "
                                                     "{$box: [[-180, -90], [180, 90]]}}}"));
    // GeoJSON coordinates.
    unique_ptr<CanonicalQuery> cqNew(canonicalize(opCtx(),
                                                  "{a: {$geoWithin: "
                                                  "{$geometry: {type: 'Polygon', coordinates: "
                                                  "[[[0, 0], [0, 90], [90, 0], [0, 0]]]}}}}"));
    ASSERT_NOT_EQUALS(canonical_query_encoder::encodeClassic(*cqLegacy),
                      canonical_query_encoder::encodeClassic(*cqNew));
}

// GEO_NEAR cache keys should include information on geometry and CRS in addition
// to the match type and field name.
TEST_F(CanonicalQueryEncoderTest, ComputeKeyGeoNear) {
    unittest::GoldenTestContext gctx(&goldenTestConfig);
    // The computed key depends on which execution engine is enabled. As such, we disable SBE for
    // this test so that the test doesn't break should the default value of
    // 'internalQueryFrameworkControl' change in the future.
    RAIIServerParameterControllerForTest controllerSBE("internalQueryFrameworkControl",
                                                       "forceClassicEngine");

    testComputeKey(gctx, "{a: {$near: [0,0], $maxDistance:0.3 }}", "{}", "{}");
    testComputeKey(gctx, "{a: {$nearSphere: [0,0], $maxDistance: 0.31 }}", "{}", "{}");
    testComputeKey(gctx,
                   "{a: {$geoNear: {$geometry: {type: 'Point', coordinates: [0,0]},"
                   "$maxDistance:100}}}",
                   "{}",
                   "{}");
}

TEST_F(CanonicalQueryEncoderTest, ComputeKeyRegexDependsOnFlags) {
    unittest::GoldenTestContext gctx(&goldenTestConfig);
    // The computed key depends on which execution engine is enabled. As such, we enable SBE for
    // this test in order to ensure that we have coverage for both SBE and the classic engine.
    RAIIServerParameterControllerForTest controllerSBE("internalQueryFrameworkControl",
                                                       "trySbeEngine");
    testComputeKey(gctx, "{a: {$regex: \"sometext\"}}", "{}", "{}");
    testComputeKey(gctx, "{a: {$regex: \"sometext\", $options: \"\"}}", "{}", "{}");

    testComputeKey(gctx, "{a: {$regex: \"sometext\", $options: \"s\"}}", "{}", "{}");
    testComputeKey(gctx, "{a: {$regex: \"sometext\", $options: \"ms\"}}", "{}", "{}");

    // Test that the ordering of $options doesn't matter.
    testComputeKey(gctx, "{a: {$regex: \"sometext\", $options: \"im\"}}", "{}", "{}");
    testComputeKey(gctx, "{a: {$regex: \"sometext\", $options: \"mi\"}}", "{}", "{}");

    // Test that only the options affect the key. Two regex match expressions with the same options
    // but different $regex values should have the same shape.
    testComputeKey(gctx, "{a: {$regex: \"abc\", $options: \"mi\"}}", "{}", "{}");
    testComputeKey(gctx, "{a: {$regex: \"efg\", $options: \"mi\"}}", "{}", "{}");

    testComputeKey(gctx, "{a: {$regex: \"\", $options: \"ms\"}}", "{}", "{}");
    testComputeKey(gctx, "{a: {$regex: \"___\", $options: \"ms\"}}", "{}", "{}");

    // Test that only valid regex flags contribute to the plan cache key encoding.
    testComputeKey(gctx,
                   BSON("a" << BSON("$regex"
                                    << "abc"
                                    << "$options"
                                    << "imxsu")),
                   {},
                   {});
    testComputeKey(gctx, "{a: /abc/im}", "{}", "{}");
}

TEST_F(CanonicalQueryEncoderTest, ComputeKeyMatchInDependsOnPresenceOfRegexAndFlags) {
    unittest::GoldenTestContext gctx(&goldenTestConfig);
    // The computed key depends on which execution engine is enabled. As such, we disable SBE for
    // this test so that the test doesn't break should the default value of
    // 'internalQueryFrameworkControl' change in the future.
    RAIIServerParameterControllerForTest controllerSBE("internalQueryFrameworkControl",
                                                       "forceClassicEngine");

    // Test that an $in containing a single regex is unwrapped to $regex.
    testComputeKey(gctx, "{a: {$in: [/foo/]}}", "{}", "{}");
    testComputeKey(gctx, "{a: {$in: [/foo/i]}}", "{}", "{}");

    // Test that an $in with no regexes does not include any regex information.
    testComputeKey(gctx, "{a: {$in: [1, 'foo']}}", "{}", "{}");

    // Test that an $in with a regex encodes the presence of the regex.
    testComputeKey(gctx, "{a: {$in: [1, /foo/]}}", "{}", "{}");

    // Test that an $in with a regex encodes the presence of the regex and its flags.
    testComputeKey(gctx, "{a: {$in: [1, /foo/is]}}", "{}", "{}");

    // Test that the computed key is invariant to the order of the flags within each regex.
    testComputeKey(gctx, "{a: {$in: [1, /foo/si]}}", "{}", "{}");

    // Test that an $in with multiple regexes encodes all unique flags.
    testComputeKey(gctx, "{a: {$in: [1, /foo/i, /bar/m, /baz/s]}}", "{}", "{}");

    // Test that an $in with multiple regexes deduplicates identical flags.
    testComputeKey(gctx, "{a: {$in: [1, /foo/i, /bar/m, /baz/s, /qux/i, /quux/s]}}", "{}", "{}");

    // Test that the computed key is invariant to the ordering of the flags across regexes.
    testComputeKey(
        gctx, "{a: {$in: [1, /foo/ism, /bar/msi, /baz/im, /qux/si, /quux/im]}}", "{}", "{}");
    testComputeKey(
        gctx, "{a: {$in: [1, /foo/msi, /bar/ism, /baz/is, /qux/mi, /quux/im]}}", "{}", "{}");

    // Test that $not-$in-$regex similarly records the presence and flags of any regexes.
    testComputeKey(gctx, "{a: {$not: {$in: [1, 'foo']}}}", "{}", "{}");
    testComputeKey(gctx, "{a: {$not: {$in: [1, /foo/]}}}", "{}", "{}");
    testComputeKey(gctx, "{a: {$not: {$in: [1, /foo/i, /bar/i, /baz/msi]}}}", "{}", "{}");

    // Test that a $not-$in containing a single regex is unwrapped to $not-$regex.
    testComputeKey(gctx, "{a: {$not: {$in: [/foo/]}}}", "{}", "{}");
    testComputeKey(gctx, "{a: {$not: {$in: [/foo/i]}}}", "{}", "{}");
}

TEST_F(CanonicalQueryEncoderTest, CheckCollationIsEncoded) {
    unittest::GoldenTestContext gctx(&goldenTestConfig);
    // The computed key depends on which execution engine is enabled. As such, we disable SBE for
    // this test so that the test doesn't break should the default value of
    // 'internalQueryFrameworkControl' change in the future.
    RAIIServerParameterControllerForTest controllerSBE("internalQueryFrameworkControl",
                                                       "forceClassicEngine");

    unique_ptr<CanonicalQuery> cq(canonicalize(
        opCtx(), fromjson("{a: 1, b: 1}"), {}, {}, fromjson("{locale: 'mock_reverse_string'}")));

    testComputeKey(gctx, *cq);
}

TEST_F(CanonicalQueryEncoderTest, ComputeKeySBE) {
    unittest::GoldenTestContext gctx(&goldenTestConfig);
    // Generated cache keys should be treated as opaque to the user.

    // SBE must be enabled in order to generate SBE plan cache keys.
    RAIIServerParameterControllerForTest controllerSBE("internalQueryFrameworkControl",
                                                       "trySbeEngine");
    testComputeSBEKey(gctx, "{}", "{}", "{}");
    testComputeSBEKey(gctx, "{$or: [{a: 1}, {b: 2}]}", "{}", "{}");
    testComputeSBEKey(gctx, "{a: 1}", "{}", "{}");
    testComputeSBEKey(gctx, "{b: 1}", "{}", "{}");
    testComputeSBEKey(gctx, "{a: 1, b: 1, c: 1}", "{}", "{}");

    // With sort
    testComputeSBEKey(gctx, "{}", "{a: 1}", "{}");
    testComputeSBEKey(gctx, "{}", "{a: -1}", "{}");
    testComputeSBEKey(gctx, "{a: 1}", "{a: 1}", "{}");

    // With projection
    testComputeSBEKey(gctx, "{a: 1}", "{a: 1}", "{a: 1}");
    testComputeSBEKey(gctx, "{}", "{a: 1}", "{a: 1}");
    testComputeSBEKey(gctx, "{}", "{a: 1}", "{a: 1}");
    testComputeSBEKey(gctx, "{}", "{}", "{a: 1}");
    testComputeSBEKey(gctx, "{}", "{}", "{a: true}");
    testComputeSBEKey(gctx, "{}", "{}", "{a: false}");

    // With count
    testComputeSBEKey(gctx,
                      "{}",
                      "{}",
                      "{}",
                      nullptr /* findCommand */,
                      {} /* pipeline */,
                      true /* isCountLike */);

    // With FindCommandRequest
    auto findCommand = std::make_unique<FindCommandRequest>(nss);
    testComputeSBEKey(gctx, "{a: 1}", "{a: 1}", "{}", std::move(findCommand));
    findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setAllowDiskUse(true);
    testComputeSBEKey(gctx, "{a: 1}", "{a: 1}", "{}", std::move(findCommand));
    findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setAllowDiskUse(false);
    testComputeSBEKey(gctx, "{a: 1}", "{a: 1}", "{}", std::move(findCommand));
    findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setReturnKey(true);
    testComputeSBEKey(gctx, "{a: 1}", "{a: 1}", "{}", std::move(findCommand));
    findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setRequestResumeToken(false);
    testComputeSBEKey(gctx, "{a: 1}", "{a: 1}", "{}", std::move(findCommand));

    findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setSkip(10);
    testComputeSBEKey(gctx, "{a: 1}", "{a: 1}", "{}", std::move(findCommand));

    findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setLimit(10);
    testComputeSBEKey(gctx, "{a: 1}", "{a: 1}", "{}", std::move(findCommand));

    findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setMin(mongo::fromjson("{ a : 1 }"));
    testComputeSBEKey(gctx, "{a: 1}", "{a: 1}", "{}", std::move(findCommand));
    findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setMax(mongo::fromjson("{ a : 1 }"));
    testComputeSBEKey(gctx, "{a: 1}", "{a: 1}", "{}", std::move(findCommand));
    findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setRequestResumeToken(true);
    // "hint" must be {$natural:1} if 'requestResumeToken' is enabled.
    findCommand->setHint(fromjson("{$natural: 1}"));
    findCommand->setResumeAfter(mongo::fromjson("{ $recordId: NumberLong(1) }"));
    testComputeSBEKey(gctx, "{a: 1}", "{}", "{}", std::move(findCommand));

    findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setHint(fromjson("{a: 1}"));
    testComputeSBEKey(gctx, "{a: 1}", "{}", "{}", std::move(findCommand));
    findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setHint(fromjson("{a: -1}"));
    testComputeSBEKey(gctx, "{a: 1}", "{}", "{}", std::move(findCommand));
}

TEST_F(CanonicalQueryEncoderTest, ComputeKeySBEWithPipeline) {
    unittest::GoldenTestContext gctx(&goldenTestConfig);
    // SBE must be enabled in order to generate SBE plan cache keys.
    RAIIServerParameterControllerForTest controllerSBE("internalQueryFrameworkControl",
                                                       "trySbeEngine");


    auto getLookupBson = [](StringData localField, StringData foreignField, StringData asField) {
        return BSON("$lookup" << BSON("from" << foreignNss.coll() << "localField" << localField
                                             << "foreignField" << foreignField << "as" << asField));
    };

    // No pipeline stage.
    testComputeSBEKey(gctx, "{a: 1}", "{}", "{}");

    // Different $lookup stage options.
    testComputeSBEKey(gctx, "{a: 1}", "{}", "{}", nullptr, {getLookupBson("a", "b", "as")});
    testComputeSBEKey(gctx, "{a: 1}", "{}", "{}", nullptr, {getLookupBson("a1", "b", "as")});
    testComputeSBEKey(gctx, "{a: 1}", "{}", "{}", nullptr, {getLookupBson("a", "b1", "as")});
    testComputeSBEKey(gctx, "{a: 1}", "{}", "{}", nullptr, {getLookupBson("a", "b", "as1")});

    // Multiple $lookup stages.
    testComputeSBEKey(gctx,
                      "{a: 1}",
                      "{}",
                      "{}",
                      nullptr,
                      {getLookupBson("a", "b", "as"), getLookupBson("a1", "b1", "as1")});
}

TEST_F(CanonicalQueryEncoderTest, ComputeKeySBEWithReadConcern) {
    unittest::GoldenTestContext gctx(&goldenTestConfig);
    // SBE must be enabled in order to generate SBE plan cache keys.
    RAIIServerParameterControllerForTest controllerSBE("internalQueryFrameworkControl",
                                                       "trySbeEngine");

    // Find command without read concern.
    auto findCommand = std::make_unique<FindCommandRequest>(nss);
    testComputeSBEKey(gctx, "{a: 1}", "{a: 1}", "{}", std::move(findCommand));

    // Find command with read concern "majority".
    findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setReadConcern(
        repl::ReadConcernArgs(repl::ReadConcernLevel::kMajorityReadConcern).toBSONInner());
    testComputeSBEKey(gctx, "{a: 1}", "{a: 1}", "{}", std::move(findCommand));

    // Find command with read concern "available".
    findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setReadConcern(
        repl::ReadConcernArgs(repl::ReadConcernLevel::kAvailableReadConcern).toBSONInner());
    testComputeSBEKey(gctx, "{a: 1}", "{a: 1}", "{}", std::move(findCommand));
}

TEST_F(CanonicalQueryEncoderTest, ComputeKeyWithApiStrict) {
    unittest::GoldenTestContext gctx(&goldenTestConfig);
    {
        RAIIServerParameterControllerForTest controllerSBE("internalQueryFrameworkControl",
                                                           "forceClassicEngine");
        APIParameters::get(opCtx()).setAPIStrict(false);
        testComputeKey(gctx, "{}", "{}", "{}");

        APIParameters::get(opCtx()).setAPIStrict(true);
        testComputeKey(gctx, "{}", "{}", "{}");
    }

    {
        RAIIServerParameterControllerForTest controllerSBE("internalQueryFrameworkControl",
                                                           "trySbeEngine");

        APIParameters::get(opCtx()).setAPIStrict(false);
        testComputeSBEKey(gctx, "{}", "{}", "{}");

        APIParameters::get(opCtx()).setAPIStrict(true);
        testComputeSBEKey(gctx, "{}", "{}", "{}");
    }
}

TEST_F(CanonicalQueryEncoderTest, ComputeKeyWithNeedsMerge) {
    unittest::GoldenTestContext gctx(&goldenTestConfig);
    RAIIServerParameterControllerForTest controllerSBE("internalQueryFrameworkControl",
                                                       "trySbeEngine");
    const auto groupStage = fromjson("{$group: {_id: '$a', out: {$sum: 1}}}");
    testComputeSBEKey(gctx,
                      "{}",
                      "{}",
                      "{}",
                      nullptr /* findCommand */,
                      {groupStage},
                      false /* isCountLike */,
                      false /* needsMerge */);

    testComputeSBEKey(gctx,
                      "{}",
                      "{}",
                      "{}",
                      nullptr /* findCommand */,
                      {groupStage},
                      false /* isCountLike */,
                      true /* needsMerge */);
}

}  // namespace
}  // namespace mongo

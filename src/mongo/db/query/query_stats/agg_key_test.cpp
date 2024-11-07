/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/bson/json.h"
#include "mongo/db/basic_types.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/query_shape/agg_cmd_shape.h"
#include "mongo/db/query/query_stats/agg_key.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/intrusive_counter.h"

namespace mongo::query_stats {

namespace {

static const NamespaceStringOrUUID kDefaultTestNss =
    NamespaceStringOrUUID{NamespaceString::createNamespaceString_forTest("testDB.testColl")};

static constexpr auto collectionType = query_shape::CollectionType::kCollection;

class AggKeyTest : public ServiceContextTest {
public:
    static std::unique_ptr<const Key> makeAggKeyFromRawPipeline(
        const std::vector<BSONObj>& rawPipeline) {
        auto expCtx = make_intrusive<ExpressionContextForTest>(kDefaultTestNss.nss());
        AggregateCommandRequest acr(kDefaultTestNss.nss());
        acr.setPipeline(rawPipeline);
        auto pipeline = Pipeline::parse(rawPipeline, expCtx);
        return std::make_unique<AggKey>(acr,
                                        *pipeline,
                                        expCtx,
                                        pipeline->getInvolvedCollections(),
                                        acr.getNamespace(),
                                        collectionType);
    }
    size_t namespaceSize(stdx::unordered_set<NamespaceString> involvedNamespaces) {
        return std::accumulate(involvedNamespaces.begin(),
                               involvedNamespaces.end(),
                               0,
                               [](int64_t total, const auto& nss) { return total + nss.size(); });
    }
};

TEST_F(AggKeyTest, SizeOfAggCmdComponents) {
    auto expCtx = make_intrusive<ExpressionContextForTest>(kDefaultTestNss.nss());
    auto rawPipeline = {fromjson(R"({
            $match: {
                foo: { $in: ["a", "b"] },
                bar: { $gte: { $date: "2022-01-01T00:00:00Z" } }
            }
        })")};

    AggregateCommandRequest acr(kDefaultTestNss.nss());
    acr.setPipeline(rawPipeline);
    auto pipeline = Pipeline::parse(rawPipeline, expCtx);
    auto namespaces = pipeline->getInvolvedCollections();
    auto aggComponents = std::make_unique<AggCmdComponents>(acr, namespaces);

    const auto minimumSize = sizeof(SpecificKeyComponents) +
        sizeof(stdx::unordered_set<NamespaceString>) + 2 /*size for bool and HasField*/ +
        sizeof(boost::optional<mongo::ExplainOptions::Verbosity>) + namespaceSize(namespaces);
    ASSERT_GTE(aggComponents->size(), minimumSize);
    ASSERT_LTE(aggComponents->size(), minimumSize + 8 /*padding*/);
}

TEST_F(AggKeyTest, EquivalentAggCmdComponentSizes) {
    auto expCtx = make_intrusive<ExpressionContextForTest>(kDefaultTestNss.nss());
    auto rawPipeline = {fromjson(R"({
            $match: {
                foo: { $in: ["a", "b"] },
                bar: { $gte: { $date: "2022-01-01T00:00:00Z" } }
            }
        })")};

    // Set different values in the command request.
    AggregateCommandRequest acrBypassTrue(kDefaultTestNss.nss());
    acrBypassTrue.setPipeline(rawPipeline);
    acrBypassTrue.setBypassDocumentValidation(true);
    auto pipeline = Pipeline::parse(rawPipeline, expCtx);
    auto namespaces = pipeline->getInvolvedCollections();
    auto aggComponentsBypassTrue = std::make_unique<AggCmdComponents>(acrBypassTrue, namespaces);


    AggregateCommandRequest acrBypassFalse(kDefaultTestNss.nss());
    acrBypassFalse.setPipeline(rawPipeline);
    acrBypassFalse.setBypassDocumentValidation(false);
    auto aggComponentsBypassFalse = std::make_unique<AggCmdComponents>(acrBypassFalse, namespaces);

    ASSERT_EQ(aggComponentsBypassTrue->size(), aggComponentsBypassFalse->size());
}

TEST_F(AggKeyTest, DifferentAggCmdComponentSizes) {
    auto expCtx = make_intrusive<ExpressionContextForTest>(kDefaultTestNss.nss());
    auto rawPipeline = {fromjson(R"({
            $match: {
                foo: { $in: ["a", "b"] },
                bar: { $gte: { $date: "2022-01-01T00:00:00Z" } }
            }
        })")};

    AggregateCommandRequest acr(kDefaultTestNss.nss());
    acr.setPipeline(rawPipeline);
    // Manually creating different namespaces for testing purposes.
    const auto namespaceStringOne =
        NamespaceString::createNamespaceString_forTest("testDB.testColl1");
    const auto namespaceStringTwo =
        NamespaceString::createNamespaceString_forTest("testDB.testColl2");

    stdx::unordered_set<NamespaceString> smallNamespaces;
    smallNamespaces.insert(namespaceStringOne);

    stdx::unordered_set<NamespaceString> largeNamespaces;
    largeNamespaces.insert(namespaceStringOne);
    largeNamespaces.insert(namespaceStringTwo);

    auto smallAggComponents = std::make_unique<AggCmdComponents>(acr, smallNamespaces);
    auto largeAggComponents = std::make_unique<AggCmdComponents>(acr, largeNamespaces);

    ASSERT_LT(namespaceSize(smallNamespaces), namespaceSize(largeNamespaces));
    ASSERT_LT(smallAggComponents->size(), largeAggComponents->size());
}

// Testing item in opCtx that should impact key size.
TEST_F(AggKeyTest, SizeOfAggKeyWithAndWithoutWriteConcern) {
    auto rawPipeline = {fromjson(R"({
            $match: {
                foo: { $in: ["a", "b"] },
                bar: { $gte: { $date: "2022-01-01T00:00:00Z" } }
            }
        })")};

    auto keyWithoutComment = makeAggKeyFromRawPipeline(rawPipeline);

    auto expCtx = make_intrusive<ExpressionContextForTest>(kDefaultTestNss.nss());
    AggregateCommandRequest acrWithComment(kDefaultTestNss.nss());
    acrWithComment.setPipeline(rawPipeline);
    expCtx->getOperationContext()->setComment(BSON("comment"
                                                   << " foo"));
    auto pipelineWithComment = Pipeline::parse(rawPipeline, expCtx);
    auto keyWithComment = std::make_unique<AggKey>(acrWithComment,
                                                   *pipelineWithComment,
                                                   expCtx,
                                                   pipelineWithComment->getInvolvedCollections(),
                                                   acrWithComment.getNamespace(),
                                                   collectionType);

    ASSERT_LT(keyWithoutComment->size(), keyWithComment->size());
}

// Testing item in command request that should impact key size.
TEST_F(AggKeyTest, SizeOfAggKeyWithAndWithoutReadConcern) {
    auto rawPipeline = {fromjson(R"({
            $match: {
                foo: { $in: ["a", "b"] },
                bar: { $gte: { $date: "2022-01-01T00:00:00Z" } }
            }
        })")};

    auto keyWithoutReadConcern = makeAggKeyFromRawPipeline(rawPipeline);

    auto expCtx = make_intrusive<ExpressionContextForTest>(kDefaultTestNss.nss());
    AggregateCommandRequest acrWithReadConcern(kDefaultTestNss.nss());
    acrWithReadConcern.setPipeline(rawPipeline);
    acrWithReadConcern.setReadConcern(repl::ReadConcernArgs::kLocal);
    auto pipelineWithReadConcern = Pipeline::parse(rawPipeline, expCtx);
    auto keyWithReadConcern =
        std::make_unique<AggKey>(acrWithReadConcern,
                                 *pipelineWithReadConcern,
                                 expCtx,
                                 pipelineWithReadConcern->getInvolvedCollections(),
                                 acrWithReadConcern.getNamespace(),
                                 collectionType);

    ASSERT_LT(keyWithoutReadConcern->size(), keyWithReadConcern->size());
}
}  // namespace
}  // namespace mongo::query_stats

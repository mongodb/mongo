/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include <vector>

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/hasher.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/s/resharding/resharding_collection_cloner.h"
#include "mongo/db/s/resharding/resharding_metrics.h"
#include "mongo/db/s/resharding/resharding_metrics_new.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/unittest.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace {

using Doc = Document;
using Arr = std::vector<Value>;
using V = Value;

/**
 * Mock interface to allow specifying mock results for the 'from' collection of the $lookup stage.
 */
class MockMongoInterface final : public StubMongoProcessInterface {
public:
    MockMongoInterface(std::deque<DocumentSource::GetNextResult> mockResults)
        : _mockResults(std::move(mockResults)) {}

    std::unique_ptr<Pipeline, PipelineDeleter> attachCursorSourceToPipeline(
        Pipeline* ownedPipeline,
        ShardTargetingPolicy shardTargetingPolicy = ShardTargetingPolicy::kAllowed,
        boost::optional<BSONObj> readConcern = boost::none) final {
        std::unique_ptr<Pipeline, PipelineDeleter> pipeline(
            ownedPipeline, PipelineDeleter(ownedPipeline->getContext()->opCtx));

        pipeline->addInitialSource(
            DocumentSourceMock::createForTest(_mockResults, pipeline->getContext()));
        return pipeline;
    }

private:
    std::deque<DocumentSource::GetNextResult> _mockResults;
};

class ReshardingCollectionClonerTest : public ServiceContextTest {
protected:
    std::unique_ptr<Pipeline, PipelineDeleter> makePipeline(
        ShardKeyPattern newShardKeyPattern,
        ShardId recipientShard,
        std::deque<DocumentSource::GetNextResult> sourceCollectionData,
        std::deque<DocumentSource::GetNextResult> configCacheChunksData) {
        auto tempNss = constructTemporaryReshardingNss(_sourceNss.db(), _sourceUUID);

        _metricsNew =
            ReshardingMetricsNew::makeInstance(_sourceUUID,
                                               newShardKeyPattern.toBSON(),
                                               _sourceNss,
                                               ReshardingMetricsNew::Role::kRecipient,
                                               getServiceContext()->getFastClockSource()->now(),
                                               getServiceContext());

        ReshardingCollectionCloner cloner(
            std::make_unique<ReshardingCollectionCloner::Env>(_metrics.get(), _metricsNew.get()),
            std::move(newShardKeyPattern),
            _sourceNss,
            _sourceUUID,
            std::move(recipientShard),
            Timestamp(1, 0), /* dummy value */
            std::move(tempNss));

        auto pipeline = cloner.makePipeline(
            _opCtx.get(), std::make_shared<MockMongoInterface>(std::move(configCacheChunksData)));

        pipeline->addInitialSource(DocumentSourceMock::createForTest(
            std::move(sourceCollectionData), pipeline->getContext()));

        return pipeline;
    }

    template <class T>
    auto getHashedElementValue(T value) {
        return BSONElementHasher::hash64(BSON("" << value).firstElement(),
                                         BSONElementHasher::DEFAULT_HASH_SEED);
    }

    void setUp() override {
        ServiceContextTest::setUp();
        _metrics = std::make_unique<ReshardingMetrics>(getServiceContext());
        _metrics->onStart(ReshardingMetrics::Role::kRecipient,
                          getServiceContext()->getFastClockSource()->now());
        _metrics->setRecipientState(RecipientStateEnum::kCloning);
    }

    void tearDown() override {
        _metrics = nullptr;
        ServiceContextTest::tearDown();
    }

private:
    const NamespaceString _sourceNss = NamespaceString("test"_sd, "collection_being_resharded"_sd);
    const UUID _sourceUUID = UUID::gen();

    ServiceContext::UniqueOperationContext _opCtx = makeOperationContext();
    std::unique_ptr<ReshardingMetrics> _metrics;
    std::unique_ptr<ReshardingMetricsNew> _metricsNew;
};

}  // namespace
}  // namespace mongo

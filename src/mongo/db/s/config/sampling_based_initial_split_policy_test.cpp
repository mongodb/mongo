// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/agg/pipeline_builder.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/global_catalog/shard_key_pattern.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/pipeline_factory.h"
#include "mongo/db/s/config/initial_split_policy.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/s/query/exec/sharded_agg_test_fixture.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"

#include <deque>
#include <memory>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

using SamplingBasedSplitPolicyTest = ShardedAggTestFixture;

const ShardId primaryShardId = ShardId("0");

TEST_F(SamplingBasedSplitPolicyTest, ShardKeyWithNonDottedFieldAndIdIsNotProjectedSucceeds) {
    const int numInitialChunks = 2;
    const int numSamplesPerChunk = 2;
    auto shardKeyPattern = ShardKeyPattern(BSON("a" << 1));

    auto pipeline =
        pipeline_factory::makePipeline(SamplingBasedSplitPolicy::createRawPipeline(
                                           shardKeyPattern, numInitialChunks, numSamplesPerChunk),
                                       expCtx(),
                                       pipeline_factory::kOptionsMinimal);
    auto mockSource =
        DocumentSourceMock::createForTest({"{_id: 10, a: 15}", "{_id: 3, a: 5}"}, expCtx());
    pipeline->addInitialSource(mockSource.get());
    auto execPipeline = exec::agg::buildPipeline(pipeline->freeze());

    // We sample all of the documents and the document source has 2 chunks. So we can assert on the
    // returned values.
    auto next = execPipeline->getNext();
    ASSERT_EQUALS(next.value().getField("a").getInt(), 5);
    ASSERT(next.value().getField("_id").missing());
    next = execPipeline->getNext();
    ASSERT_EQUALS(next.value().getField("a").getInt(), 15);
    ASSERT(next.value().getField("_id").missing());
    ASSERT(!execPipeline->getNext());
}

TEST_F(SamplingBasedSplitPolicyTest, ShardKeyWithIdFieldIsProjectedSucceeds) {
    const int numInitialChunks = 2;
    const int numSamplesPerChunk = 2;
    auto shardKeyPattern = ShardKeyPattern(BSON("_id" << 1));

    auto pipeline =
        pipeline_factory::makePipeline(SamplingBasedSplitPolicy::createRawPipeline(
                                           shardKeyPattern, numInitialChunks, numSamplesPerChunk),
                                       expCtx(),
                                       pipeline_factory::kOptionsMinimal);
    auto mockSource =
        DocumentSourceMock::createForTest({"{_id: 10, a: 15}", "{_id: 3, a: 5}"}, expCtx());
    pipeline->addInitialSource(mockSource.get());
    auto execPipeline = exec::agg::buildPipeline(pipeline->freeze());

    // We sample all of the documents and the document source has 2 chunks. So we can assert on the
    // returned values.
    auto next = execPipeline->getNext();
    ASSERT_EQUALS(next.value().getField("_id").getInt(), 3);
    ASSERT(next.value().getField("a").missing());
    next = execPipeline->getNext();
    ASSERT_EQUALS(next.value().getField("_id").getInt(), 10);
    ASSERT(next.value().getField("a").missing());
    ASSERT(!execPipeline->getNext());
}

TEST_F(SamplingBasedSplitPolicyTest, CompoundShardKeyWithNonDottedHashedFieldSucceeds) {
    const int numInitialChunks = 2;
    const int numSamplesPerChunk = 2;
    auto shardKeyPattern = ShardKeyPattern(BSON("a" << 1 << "b"
                                                    << "hashed"));

    auto pipeline =
        pipeline_factory::makePipeline(SamplingBasedSplitPolicy::createRawPipeline(
                                           shardKeyPattern, numInitialChunks, numSamplesPerChunk),
                                       expCtx(),
                                       pipeline_factory::kOptionsMinimal);
    auto mockSource = DocumentSourceMock::createForTest(
        {"{x: 1, b: 16, a: 15}", "{x: 2, b: 123, a: 5}"}, expCtx());
    pipeline->addInitialSource(mockSource.get());
    auto execPipeline = exec::agg::buildPipeline(pipeline->freeze());

    // We sample all of the documents and the document source has 2 chunks. So we can assert on
    // the returned values.
    auto next = execPipeline->getNext();
    ASSERT_EQUALS(next.value().getField("a").getInt(), 5);
    ASSERT_EQUALS(next.value().getField("b").getLong(), -6548868637522515075LL);
    ASSERT(next.value().getField("x").missing());
    next = execPipeline->getNext();
    ASSERT_EQUALS(next.value().getField("a").getInt(), 15);
    ASSERT_EQUALS(next.value().getField("b").getLong(), 2598032665634823220LL);
    ASSERT(next.value().getField("x").missing());
    ASSERT(!execPipeline->getNext());
}

TEST_F(SamplingBasedSplitPolicyTest, CompoundShardKeyWithDottedFieldSucceeds) {
    const int numInitialChunks = 2;
    const int numSamplesPerChunk = 2;
    auto shardKeyPattern = ShardKeyPattern(BSON("a.b" << 1 << "c" << 1));

    auto pipeline =
        pipeline_factory::makePipeline(SamplingBasedSplitPolicy::createRawPipeline(
                                           shardKeyPattern, numInitialChunks, numSamplesPerChunk),
                                       expCtx(),
                                       pipeline_factory::kOptionsMinimal);
    auto mockSource = DocumentSourceMock::createForTest(
        {"{x: 10, a: {b: 20}, c: 1}", "{x: 3, a: {b: 10}, c: 5}"}, expCtx());
    pipeline->addInitialSource(mockSource.get());
    auto execPipeline = exec::agg::buildPipeline(pipeline->freeze());

    // We sample all of the documents and the document source has 2 chunks. So we can assert on the
    // returned values.
    auto next = execPipeline->getNext();
    ASSERT_BSONOBJ_EQ(next.value().toBson(), BSON("a.b" << 10 << "c" << 5));
    next = execPipeline->getNext();
    ASSERT_BSONOBJ_EQ(next.value().toBson(), BSON("a.b" << 20 << "c" << 1));
    ASSERT(!execPipeline->getNext());
}

TEST_F(SamplingBasedSplitPolicyTest, CompoundShardKeyWithDottedHashedFieldSucceeds) {
    const int numInitialChunks = 2;
    const int numSamplesPerChunk = 2;
    auto shardKeyPattern = ShardKeyPattern(BSON("a.b" << 1 << "c" << 1 << "a.c"
                                                      << "hashed"));

    auto pipeline =
        pipeline_factory::makePipeline(SamplingBasedSplitPolicy::createRawPipeline(
                                           shardKeyPattern, numInitialChunks, numSamplesPerChunk),
                                       expCtx(),
                                       pipeline_factory::kOptionsMinimal);
    auto mockSource = DocumentSourceMock::createForTest(
        {"{x: 10, a: {b: 20, c: 16}, c: 1}", "{x: 3, a: {b: 10, c: 123}, c: 5}"}, expCtx());
    pipeline->addInitialSource(mockSource.get());
    auto execPipeline = exec::agg::buildPipeline(pipeline->freeze());

    // We sample all of the documents and the document source has 2 chunks. So we can assert on the
    // returned values.
    auto next = execPipeline->getNext();
    ASSERT_BSONOBJ_EQ(next.value().toBson(),
                      BSON("a.b" << 10 << "c" << 5 << "a.c" << -6548868637522515075LL));
    next = execPipeline->getNext();
    ASSERT_BSONOBJ_EQ(next.value().toBson(),
                      BSON("a.b" << 20 << "c" << 1 << "a.c" << 2598032665634823220LL));
    ASSERT(!execPipeline->getNext());
}

std::string dumpValues(const std::vector<int>& values) {
    std::stringstream ss;
    ss << "[";
    for (const auto& val : values) {
        ss << val << ", ";
    }
    ss << "]";
    return ss.str();
}

TEST_F(SamplingBasedSplitPolicyTest, SamplingSucceedsSufficientSamples) {
    const int numInitialChunks = 5;
    const int numSamplesPerChunk = 5;
    const int numSamples = 30;
    auto shardKeyPattern = ShardKeyPattern(BSON("a" << 1));

    std::deque<DocumentSourceMock::GetNextResult> docs;
    for (int a = 0; a < numSamples; a++) {
        docs.emplace_back(Document(BSON("a" << a)));
    }

    auto pipeline =
        pipeline_factory::makePipeline(SamplingBasedSplitPolicy::createRawPipeline(
                                           shardKeyPattern, numInitialChunks, numSamplesPerChunk),
                                       expCtx(),
                                       pipeline_factory::kOptionsMinimal);
    auto mockSource = DocumentSourceMock::createForTest(docs, expCtx());
    pipeline->addInitialSource(mockSource.get());

    SamplingBasedSplitPolicy::PipelineDocumentSource skippingSource(std::move(pipeline),
                                                                    numSamplesPerChunk - 1);

    std::vector<int> sampledValues;
    for (auto i = 0; i < numInitialChunks - 1; i++) {
        auto nextDoc = skippingSource.getNext();
        sampledValues.push_back((*nextDoc)["a"].numberInt());
    }

    int lastVal = -1 * numSamplesPerChunk;
    for (const auto& val : sampledValues) {
        ASSERT_GTE(val - lastVal, numSamplesPerChunk) << dumpValues(sampledValues);
        lastVal = val;
    }
}

TEST_F(SamplingBasedSplitPolicyTest, SamplingSucceedsInsufficientSamplesOneSplitPoint) {
    const int numInitialChunks = 2;
    const int numSamplesPerChunk = 5;
    const int numSamples = 3;
    auto shardKeyPattern = ShardKeyPattern(BSON("a" << 1));

    std::deque<DocumentSourceMock::GetNextResult> docs;
    for (int a = 0; a < numSamples; a++) {
        docs.emplace_back(Document(BSON("a" << a)));
    }

    auto pipeline =
        pipeline_factory::makePipeline(SamplingBasedSplitPolicy::createRawPipeline(
                                           shardKeyPattern, numInitialChunks, numSamplesPerChunk),
                                       expCtx(),
                                       pipeline_factory::kOptionsMinimal);
    auto mockSource = DocumentSourceMock::createForTest(docs, expCtx());
    pipeline->addInitialSource(mockSource.get());

    SamplingBasedSplitPolicy::PipelineDocumentSource skippingSource(std::move(pipeline),
                                                                    numSamplesPerChunk - 1);

    // Verify that the source selected a split point although there are not enough samples.
    ASSERT(skippingSource.getNext());
}

TEST_F(SamplingBasedSplitPolicyTest, SamplingSucceedsInsufficientSamplesMultipleSplitPoints) {
    const int numInitialChunks = 3;
    const int numSamplesPerChunk = 5;
    const int numSamples = 8;
    auto shardKeyPattern = ShardKeyPattern(BSON("a" << 1));

    std::deque<DocumentSourceMock::GetNextResult> docs;
    for (int a = 0; a < numSamples; a++) {
        docs.emplace_back(Document(BSON("a" << a)));
    }

    auto pipeline =
        pipeline_factory::makePipeline(SamplingBasedSplitPolicy::createRawPipeline(
                                           shardKeyPattern, numInitialChunks, numSamplesPerChunk),
                                       expCtx(),
                                       pipeline_factory::kOptionsMinimal);
    auto mockSource = DocumentSourceMock::createForTest(docs, expCtx());
    pipeline->addInitialSource(mockSource.get());

    SamplingBasedSplitPolicy::PipelineDocumentSource skippingSource(std::move(pipeline),
                                                                    numSamplesPerChunk - 1);

    // Verify that the source selected two split points although there were not enough samples
    // for the second chunk.
    ASSERT(skippingSource.getNext());
    ASSERT(skippingSource.getNext());
}

TEST_F(SamplingBasedSplitPolicyTest, ShardKeyWithDottedPathAndIdIsNotProjectedSucceeds) {
    const int numInitialChunks = 2;
    const int numSamplesPerChunk = 2;

    auto shardKeyPattern = ShardKeyPattern(BSON("b" << 1));
    auto pipeline =
        pipeline_factory::makePipeline(SamplingBasedSplitPolicy::createRawPipeline(
                                           shardKeyPattern, numInitialChunks, numSamplesPerChunk),
                                       expCtx(),
                                       pipeline_factory::kOptionsMinimal);
    auto mockSource = DocumentSourceMock::createForTest(
        {"{_id: {a: 15}, b: 10}", "{_id: {a: 5}, b:1}"}, expCtx());
    pipeline->addInitialSource(mockSource.get());
    auto execPipeline = exec::agg::buildPipeline(pipeline->freeze());
    auto next = execPipeline->getNext();
    ASSERT_BSONOBJ_EQ(next.value().toBson(), BSON("b" << 1));
    next = execPipeline->getNext();
    ASSERT_BSONOBJ_EQ(next.value().toBson(), BSON("b" << 10));
    ASSERT(!execPipeline->getNext());
}

TEST_F(SamplingBasedSplitPolicyTest, CompoundShardKeyWithDottedPathAndIdIsProjectedSucceeds) {
    const int numInitialChunks = 2;
    const int numSamplesPerChunk = 2;

    auto shardKeyPattern = ShardKeyPattern(BSON("_id.a" << 1 << "c" << 1));
    auto pipeline =
        pipeline_factory::makePipeline(SamplingBasedSplitPolicy::createRawPipeline(
                                           shardKeyPattern, numInitialChunks, numSamplesPerChunk),
                                       expCtx(),
                                       pipeline_factory::kOptionsMinimal);
    auto mockSource = DocumentSourceMock::createForTest(
        {"{_id: {a: 15}, c: 10}", "{_id: {a: 5}, c: 1}"}, expCtx());
    pipeline->addInitialSource(mockSource.get());
    auto execPipeline = exec::agg::buildPipeline(pipeline->freeze());
    auto next = execPipeline->getNext();
    ASSERT_BSONOBJ_EQ(next.value().toBson(), BSON("_id.a" << 5 << "c" << 1));
    next = execPipeline->getNext();
    ASSERT_BSONOBJ_EQ(next.value().toBson(), BSON("_id.a" << 15 << "c" << 10));
    ASSERT(!execPipeline->getNext());
}

TEST_F(SamplingBasedSplitPolicyTest, CompoundShardKeyWithDottedHashedPathSucceeds) {
    const int numInitialChunks = 2;
    const int numSamplesPerChunk = 2;

    auto shardKeyPattern = ShardKeyPattern(BSON("_id.a" << 1 << "b" << 1 << "_id.b"
                                                        << "hashed"));
    auto pipeline =
        pipeline_factory::makePipeline(SamplingBasedSplitPolicy::createRawPipeline(
                                           shardKeyPattern, numInitialChunks, numSamplesPerChunk),
                                       expCtx(),
                                       pipeline_factory::kOptionsMinimal);
    auto mockSource = DocumentSourceMock::createForTest(
        {"{x: 10, _id: {a: 20, b: 16}, b: 1}", "{x: 3, _id: {a: 10, b: 123}, b: 5}"}, expCtx());
    pipeline->addInitialSource(mockSource.get());
    auto execPipeline = exec::agg::buildPipeline(pipeline->freeze());

    auto next = execPipeline->getNext();
    ASSERT_BSONOBJ_EQ(next.value().toBson(),
                      BSON("_id.a" << 10 << "b" << 5 << "_id.b" << -6548868637522515075LL));
    next = execPipeline->getNext();
    ASSERT_BSONOBJ_EQ(next.value().toBson(),
                      BSON("_id.a" << 20 << "b" << 1 << "_id.b" << 2598032665634823220LL));
    ASSERT(!execPipeline->getNext());
}

TEST_F(SamplingBasedSplitPolicyTest, SamplingSucceedsWithLimitedMemoryForSortOperation) {
    unittest::ServerParameterGuard sortMaxMemory{"internalQueryMaxBlockingSortMemoryUsageBytes",
                                                 100};

    unittest::TempDir _tempDir{"SamplingSucceedsWithLimitedMemoryForSortOperation"};
    storageGlobalParams.dbpath = _tempDir.path();
    const int numInitialChunks = 3;
    const int numSamplesPerChunk = 2;

    auto shardKeyPattern = ShardKeyPattern(BSON("a" << 1));
    const NamespaceString ns = NamespaceString::createNamespaceString_forTest("foo", "bar");
    auto mockSource = DocumentSourceMock::createForTest(
        {"{_id: 20, a: 4}", "{_id: 30, a: 3}", "{_id: 40, a: 2}", "{_id: 50, a: 1}"}, expCtx());
    auto pipelineDocSource =
        SamplingBasedSplitPolicy::makePipelineDocumentSource_forTest(operationContext(),
                                                                     mockSource,
                                                                     kTestAggregateNss,
                                                                     shardKeyPattern,
                                                                     numInitialChunks,
                                                                     numSamplesPerChunk);
    auto next = pipelineDocSource->getNext();
    ASSERT_BSONOBJ_EQ(BSON("a" << 2), next.value());
    next = pipelineDocSource->getNext();
    ASSERT_BSONOBJ_EQ(BSON("a" << 4), next.value());
    ASSERT(!pipelineDocSource->getNext());
}

TEST_F(SamplingBasedSplitPolicyTest, SetNumSamplesPerChunkSucceedsOnOneChunk) {
    const int numInitialChunks = 1;
    const int numSamplesPerChunk = 10;

    auto shardKeyPattern = ShardKeyPattern(BSON("a" << 1));
    const NamespaceString ns = NamespaceString::createNamespaceString_forTest("foo", "bar");
    auto pipeline =
        pipeline_factory::makePipeline(SamplingBasedSplitPolicy::createRawPipeline(
                                           shardKeyPattern, numInitialChunks, numSamplesPerChunk),
                                       expCtx(),
                                       pipeline_factory::kOptionsMinimal);
    auto mockSource = DocumentSourceMock::createForTest(
        {
            "{_id: 10, a: 15}",
            "{_id: 3, a: 5}",
            "{_id: 1, a: 1}",
            "{_id: 2, a: 2}",
            "{_id: 3, a: 3}",
            "{_id: 50, a: 50}",
            "{_id: 9, a: 14}",
            "{_id: 11, a: 12}",
            "{_id: 18, a: 13}",
            "{_id: 6, a: 6}",
            "{_id: 8, a: 30}",
            "{_id: 17, a: 21}",
        },
        expCtx());
    pipeline->addInitialSource(mockSource.get());
    auto execPipeline = exec::agg::buildPipeline(pipeline->freeze());

    int numSamples = 0;
    auto next = execPipeline->getNext();
    while (next) {
        ++numSamples;
        next = execPipeline->getNext();
    }
    ASSERT_EQ(numSamplesPerChunk * numInitialChunks, numSamples);
}

TEST_F(SamplingBasedSplitPolicyTest, SetNumSamplesPerChunkSucceedsOnMultipleChunks) {
    const int numInitialChunks = 3;
    const int numSamplesPerChunk = 3;

    auto shardKeyPattern = ShardKeyPattern(BSON("a" << 1));
    const NamespaceString ns = NamespaceString::createNamespaceString_forTest("foo", "bar");
    auto pipeline =
        pipeline_factory::makePipeline(SamplingBasedSplitPolicy::createRawPipeline(
                                           shardKeyPattern, numInitialChunks, numSamplesPerChunk),
                                       expCtx(),
                                       pipeline_factory::kOptionsMinimal);
    auto mockSource = DocumentSourceMock::createForTest(
        {
            "{_id: 10, a: 15}",
            "{_id: 3, a: 5}",
            "{_id: 1, a: 1}",
            "{_id: 2, a: 2}",
            "{_id: 3, a: 3}",
            "{_id: 50, a: 50}",
            "{_id: 9, a: 14}",
            "{_id: 11, a: 12}",
            "{_id: 18, a: 13}",
            "{_id: 6, a: 6}",
        },
        expCtx());
    pipeline->addInitialSource(mockSource.get());
    auto execPipeline = exec::agg::buildPipeline(pipeline->freeze());

    int numSamples = 0;
    auto next = execPipeline->getNext();
    while (next) {
        ++numSamples;
        next = execPipeline->getNext();
    }
    ASSERT_EQ(numSamplesPerChunk * numInitialChunks, numSamples);
}

}  // namespace
}  // namespace mongo

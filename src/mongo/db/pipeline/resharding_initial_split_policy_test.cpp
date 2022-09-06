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

#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/sharded_agg_helpers.h"
#include "mongo/db/s/config/initial_split_policy.h"
#include "mongo/logv2/log.h"
#include "mongo/s/query/sharded_agg_test_fixture.h"
#include "mongo/unittest/unittest.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

using ReshardingSplitPolicyTest = ShardedAggTestFixture;

const ShardId primaryShardId = ShardId("0");

TEST_F(ReshardingSplitPolicyTest, ShardKeyWithNonDottedFieldAndIdIsNotProjectedSucceeds) {
    auto shardKeyPattern = ShardKeyPattern(BSON("a" << 1));
    auto pipeline =
        Pipeline::parse(ReshardingSplitPolicy::createRawPipeline(
                            shardKeyPattern, 2 /* samplingRatio */, 1 /* numSplitPoints */),
                        expCtx());
    auto mockSource =
        DocumentSourceMock::createForTest({"{_id: 10, a: 15}", "{_id: 3, a: 5}"}, expCtx());
    pipeline->addInitialSource(mockSource.get());
    // We sample all of the documents since numSplitPoints(1) * samplingRatio (2) = 2 and the
    // document source has 2 chunks. So we can assert on the returned values.
    auto next = pipeline->getNext();
    ASSERT_EQUALS(next.value().getField("a").getInt(), 5);
    ASSERT(next.value().getField("_id").missing());
    next = pipeline->getNext();
    ASSERT_EQUALS(next.value().getField("a").getInt(), 15);
    ASSERT(next.value().getField("_id").missing());
    ASSERT(!pipeline->getNext());
}

TEST_F(ReshardingSplitPolicyTest, ShardKeyWithIdFieldIsProjectedSucceeds) {
    auto shardKeyPattern = ShardKeyPattern(BSON("_id" << 1));

    auto pipeline =
        Pipeline::parse(ReshardingSplitPolicy::createRawPipeline(
                            shardKeyPattern, 2 /* samplingRatio */, 1 /* numSplitPoints */),
                        expCtx());
    auto mockSource =
        DocumentSourceMock::createForTest({"{_id: 10, a: 15}", "{_id: 3, a: 5}"}, expCtx());
    pipeline->addInitialSource(mockSource.get());

    // We sample all of the documents since numSplitPoints(1) * samplingRatio (2) = 2 and the
    // document source has 2 chunks. So we can assert on the returned values.
    auto next = pipeline->getNext();
    ASSERT_EQUALS(next.value().getField("_id").getInt(), 3);
    ASSERT(next.value().getField("a").missing());
    next = pipeline->getNext();
    ASSERT_EQUALS(next.value().getField("_id").getInt(), 10);
    ASSERT(next.value().getField("a").missing());
    ASSERT(!pipeline->getNext());
}

TEST_F(ReshardingSplitPolicyTest, CompoundShardKeyWithNonDottedHashedFieldSucceeds) {
    auto shardKeyPattern = ShardKeyPattern(BSON("a" << 1 << "b"
                                                    << "hashed"));
    auto pipeline =
        Pipeline::parse(ReshardingSplitPolicy::createRawPipeline(
                            shardKeyPattern, 2 /* samplingRatio */, 1 /* numSplitPoints */),
                        expCtx());
    auto mockSource = DocumentSourceMock::createForTest(
        {"{x: 1, b: 16, a: 15}", "{x: 2, b: 123, a: 5}"}, expCtx());
    pipeline->addInitialSource(mockSource.get());
    // We sample all of the documents since numSplitPoints(1) * samplingRatio (2) = 2 and the
    // document source has 2 chunks. So we can assert on the returned values.
    auto next = pipeline->getNext();
    ASSERT_EQUALS(next.value().getField("a").getInt(), 5);
    ASSERT_EQUALS(next.value().getField("b").getLong(), -6548868637522515075LL);
    ASSERT(next.value().getField("x").missing());
    next = pipeline->getNext();
    ASSERT_EQUALS(next.value().getField("a").getInt(), 15);
    ASSERT_EQUALS(next.value().getField("b").getLong(), 2598032665634823220LL);
    ASSERT(next.value().getField("x").missing());
    ASSERT(!pipeline->getNext());
}

TEST_F(ReshardingSplitPolicyTest, CompoundShardKeyWithDottedFieldSucceeds) {
    auto shardKeyPattern = ShardKeyPattern(BSON("a.b" << 1 << "c" << 1));

    auto pipeline =
        Pipeline::parse(ReshardingSplitPolicy::createRawPipeline(
                            shardKeyPattern, 2 /* samplingRatio */, 1 /* numSplitPoints */),
                        expCtx());
    auto mockSource = DocumentSourceMock::createForTest(
        {"{x: 10, a: {b: 20}, c: 1}", "{x: 3, a: {b: 10}, c: 5}"}, expCtx());
    pipeline->addInitialSource(mockSource.get());

    // We sample all of the documents since numSplitPoints(1) * samplingRatio (2) = 2 and the
    // document source has 2 chunks. So we can assert on the returned values.
    auto next = pipeline->getNext();
    ASSERT_BSONOBJ_EQ(next.value().toBson(), BSON("a.b" << 10 << "c" << 5));
    next = pipeline->getNext();
    ASSERT_BSONOBJ_EQ(next.value().toBson(), BSON("a.b" << 20 << "c" << 1));
    ASSERT(!pipeline->getNext());
}

TEST_F(ReshardingSplitPolicyTest, CompoundShardKeyWithDottedHashedFieldSucceeds) {
    auto shardKeyPattern = ShardKeyPattern(BSON("a.b" << 1 << "c" << 1 << "a.c"
                                                      << "hashed"));

    auto pipeline =
        Pipeline::parse(ReshardingSplitPolicy::createRawPipeline(
                            shardKeyPattern, 2 /* samplingRatio */, 1 /* numSplitPoints */),
                        expCtx());
    auto mockSource = DocumentSourceMock::createForTest(
        {"{x: 10, a: {b: 20, c: 16}, c: 1}", "{x: 3, a: {b: 10, c: 123}, c: 5}"}, expCtx());
    pipeline->addInitialSource(mockSource.get());

    // We sample all of the documents since numSplitPoints(1) * samplingRatio (2) = 2 and the
    // document source has 2 chunks. So we can assert on the returned values.
    auto next = pipeline->getNext();
    ASSERT_BSONOBJ_EQ(next.value().toBson(),
                      BSON("a.b" << 10 << "c" << 5 << "a.c" << -6548868637522515075LL));
    next = pipeline->getNext();
    ASSERT_BSONOBJ_EQ(next.value().toBson(),
                      BSON("a.b" << 20 << "c" << 1 << "a.c" << 2598032665634823220LL));
    ASSERT(!pipeline->getNext());
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

TEST_F(ReshardingSplitPolicyTest, SamplingSuceeds) {
    const int kNumSplitPoints = 4;
    const int kNumSamplesPerChunk = 5;
    auto shardKeyPattern = ShardKeyPattern(BSON("a" << 1));

    std::deque<DocumentSourceMock::GetNextResult> docs;
    for (int a = 0; a < 30; a++) {
        docs.emplace_back(Document(BSON("a" << a)));
    }

    auto pipeline = Pipeline::parse(ReshardingSplitPolicy::createRawPipeline(
                                        shardKeyPattern, kNumSamplesPerChunk, kNumSplitPoints),
                                    expCtx());
    auto mockSource = DocumentSourceMock::createForTest(docs, expCtx());
    pipeline->addInitialSource(mockSource.get());

    ReshardingSplitPolicy::PipelineDocumentSource skippingSource(std::move(pipeline),
                                                                 kNumSamplesPerChunk - 1);

    std::vector<int> sampledValues;
    while (auto nextDoc = skippingSource.getNext()) {
        sampledValues.push_back((*nextDoc)["a"].numberInt());
    }

    ASSERT_EQ(kNumSplitPoints, sampledValues.size()) << dumpValues(sampledValues);

    int lastVal = -1 * kNumSamplesPerChunk;
    for (const auto& val : sampledValues) {
        ASSERT_GTE(val - lastVal, kNumSamplesPerChunk) << dumpValues(sampledValues);
        lastVal = val;
    }
}

TEST_F(ReshardingSplitPolicyTest, ShardKeyWithDottedPathAndIdIsNotProjectedSucceeds) {
    auto shardKeyPattern = ShardKeyPattern(BSON("b" << 1));
    auto pipeline =
        Pipeline::parse(ReshardingSplitPolicy::createRawPipeline(
                            shardKeyPattern, 2 /* samplingRatio */, 1 /* numSplitPoints */),
                        expCtx());
    auto mockSource = DocumentSourceMock::createForTest(
        {"{_id: {a: 15}, b: 10}", "{_id: {a: 5}, b:1}"}, expCtx());
    pipeline->addInitialSource(mockSource.get());
    auto next = pipeline->getNext();
    ASSERT_BSONOBJ_EQ(next.value().toBson(), BSON("b" << 1));
    next = pipeline->getNext();
    ASSERT_BSONOBJ_EQ(next.value().toBson(), BSON("b" << 10));
    ASSERT(!pipeline->getNext());
}

TEST_F(ReshardingSplitPolicyTest, CompoundShardKeyWithDottedPathAndIdIsProjectedSucceeds) {
    auto shardKeyPattern = ShardKeyPattern(BSON("_id.a" << 1 << "c" << 1));
    auto pipeline =
        Pipeline::parse(ReshardingSplitPolicy::createRawPipeline(
                            shardKeyPattern, 2 /* samplingRatio */, 1 /* numSplitPoints */),
                        expCtx());
    auto mockSource = DocumentSourceMock::createForTest(
        {"{_id: {a: 15}, c: 10}", "{_id: {a: 5}, c: 1}"}, expCtx());
    pipeline->addInitialSource(mockSource.get());
    auto next = pipeline->getNext();
    ASSERT_BSONOBJ_EQ(next.value().toBson(), BSON("_id.a" << 5 << "c" << 1));
    next = pipeline->getNext();
    ASSERT_BSONOBJ_EQ(next.value().toBson(), BSON("_id.a" << 15 << "c" << 10));
    ASSERT(!pipeline->getNext());
}

TEST_F(ReshardingSplitPolicyTest, CompoundShardKeyWithDottedHashedPathSucceeds) {
    auto shardKeyPattern = ShardKeyPattern(BSON("_id.a" << 1 << "b" << 1 << "_id.b"
                                                        << "hashed"));

    auto pipeline =
        Pipeline::parse(ReshardingSplitPolicy::createRawPipeline(
                            shardKeyPattern, 2 /* samplingRatio */, 1 /* numSplitPoints */),
                        expCtx());
    auto mockSource = DocumentSourceMock::createForTest(
        {"{x: 10, _id: {a: 20, b: 16}, b: 1}", "{x: 3, _id: {a: 10, b: 123}, b: 5}"}, expCtx());
    pipeline->addInitialSource(mockSource.get());

    auto next = pipeline->getNext();
    ASSERT_BSONOBJ_EQ(next.value().toBson(),
                      BSON("_id.a" << 10 << "b" << 5 << "_id.b" << -6548868637522515075LL));
    next = pipeline->getNext();
    ASSERT_BSONOBJ_EQ(next.value().toBson(),
                      BSON("_id.a" << 20 << "b" << 1 << "_id.b" << 2598032665634823220LL));
    ASSERT(!pipeline->getNext());
}
}  // namespace
}  // namespace mongo

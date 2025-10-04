/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/util/make_data_structure.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <vector>

namespace mongo {
namespace {

using InternalUnpackBucketMatchFixedBucketTest = AggregationContextFixture;
}

auto unpackSpecObj = fromjson(R"({
            $_internalUnpackBucket: { 
                exclude: [], timeField: 'foo', 
                metaField: 'meta1', 
                bucketMaxSpanSeconds: 3600,
                fixedBuckets: true
            }
  })");
auto groupSpecObj = fromjson(
    "{$group: {_id: '$meta1.a.b', accmin: {$min: '$b'}, accmax: {$max: '$c'}, $willBeMerged: "
    "false}}");
auto limitSpecObj = fromjson("{$limit: 2}");

TEST_F(InternalUnpackBucketMatchFixedBucketTest, MatchWithGroupLimitRewrite) {
    // Since the buckets are fixed, and the predicate is on the timeField and aligns with the
    // bucket boundaries, the $match stage will be pushed up and there will be no _eventFilter.
    // Since the measurements do not need to be individually filtered in the $match stage, further
    // optimizations are allowed.
    Date_t date = dateFromISOString("2012-10-26T09:00:00+0000").getValue();
    auto matchSpecObj = BSON("$match" << BSON("foo" << BSON("$lt" << date)));

    // $match followed by a $group should remove the $unpack stage.
    {
        auto pipeline =
            Pipeline::parse(makeVector(unpackSpecObj, matchSpecObj, groupSpecObj), getExpCtx());
        pipeline->optimizePipeline();

        auto serialized = pipeline->serializeToBson();
        ASSERT_EQ(2, serialized.size());
        // The $match stage includes an ObjectId that we can't predict, so we will validate the
        // first stage is a $match stage.
        ASSERT_TRUE(serialized[0].hasField("$match"));
        auto groupOptimized = fromjson(
            "{$group: {_id: '$meta.a.b', accmin: {$min: '$control.min.b'}, accmax: {$max: "
            "'$control.max.c'}, $willBeMerged: false}}");
        ASSERT_BSONOBJ_EQ(groupOptimized, serialized[1]);
    }

    // $limit can be pushed up after a $match.
    {
        auto pipeline =
            Pipeline::parse(makeVector(unpackSpecObj, matchSpecObj, limitSpecObj), getExpCtx());
        pipeline->optimizePipeline();

        auto serialized = pipeline->serializeToBson();
        ASSERT_EQ(4, serialized.size());
        ASSERT_TRUE(serialized[0].hasField("$match"));
        ASSERT_BSONOBJ_EQ(limitSpecObj, serialized[1]);
        ASSERT_BSONOBJ_EQ(unpackSpecObj, serialized[2]);
        ASSERT_BSONOBJ_EQ(limitSpecObj, serialized[3]);
    }
}

TEST_F(InternalUnpackBucketMatchFixedBucketTest, MatchWithGroupLimitRewriteNegative) {
    // Even though the buckets are fixed, a $gt query must always contain an _eventFilter, even when
    // the predicate aligns with the bucket boundaries. Therefore, we expect an _eventFilter to
    // exist, which prevents further optimizations.
    Date_t date = dateFromISOString("2012-10-26T09:00:00+0000").getValue();
    auto matchSpecObj = BSON("$match" << BSON("foo" << BSON("$gt" << date)));
    auto serializedUnpackObj = fromjson(R"({
            $_internalUnpackBucket: { 
                include: ['b', 'c', 'foo', 'meta1'], timeField: 'foo', 
                metaField: 'meta1', 
                bucketMaxSpanSeconds: 3600,
                wholeBucketFilter: {'control.min.foo': { $gt: new Date(1351242000000) }},
                eventFilter: { 'foo': { $gt: new Date(1351242000000) } },
                fixedBuckets: true
            }
  })");
    // The $group stage should not replace the $unpack stage like in the previous example, because
    // the _eventFilter exists.
    {
        auto pipeline =
            Pipeline::parse(makeVector(unpackSpecObj, matchSpecObj, groupSpecObj), getExpCtx());
        pipeline->optimizePipeline();

        auto serialized = pipeline->serializeToBson();
        ASSERT_EQ(3, serialized.size());
        ASSERT_TRUE(serialized[0].hasField("$match"));
        ASSERT_BSONOBJ_EQ(serializedUnpackObj, serialized[1]);
        ASSERT_BSONOBJ_EQ(groupSpecObj, serialized[2]);
    }
    // The $limit stage cannot be pushed up after the $match, since the _eventFilter remains.
    {
        auto pipeline =
            Pipeline::parse(makeVector(unpackSpecObj, matchSpecObj, limitSpecObj), getExpCtx());
        pipeline->optimizePipeline();

        auto serialized = pipeline->serializeToBson();
        ASSERT_EQ(3, serialized.size());
        ASSERT_TRUE(serialized[0].hasField("$match"));
        auto serializedUnpackObj = fromjson(R"({
            $_internalUnpackBucket: { 
                exclude: [], timeField: 'foo', 
                metaField: 'meta1', 
                bucketMaxSpanSeconds: 3600,
                wholeBucketFilter: {'control.min.foo': { $gt: new Date(1351242000000) }},
                eventFilter: { 'foo': { $gt: new Date(1351242000000) } },
                fixedBuckets: true
            }
        })");
        ASSERT_BSONOBJ_EQ(serializedUnpackObj, serialized[1]);
        ASSERT_BSONOBJ_EQ(limitSpecObj, serialized[2]);
    }
}
}  // namespace mongo

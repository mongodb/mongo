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

#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/query/util/make_data_structure.h"

namespace mongo {
namespace {

using InternalUnpackBucketSortReorderTest = AggregationContextFixture;

TEST_F(InternalUnpackBucketSortReorderTest, OptimizeForMetaSort) {
    auto unpackSpecObj = fromjson(
        "{$_internalUnpackBucket: { exclude: [], timeField: 'foo', metaField: 'meta1', "
        "bucketMaxSpanSeconds: 3600}}");
    auto countSpecObj = fromjson("{$sort: {'meta1.a': 1, 'meta1.b': -1}}");

    auto pipeline = Pipeline::parse(makeVector(unpackSpecObj, countSpecObj), getExpCtx());
    pipeline->optimizePipeline();

    auto serialized = pipeline->serializeToBson();

    // $sort is now before unpack bucket.
    ASSERT_EQ(2, serialized.size());
    ASSERT_BSONOBJ_EQ(fromjson("{ $sort: {'meta.a': 1, 'meta.b': -1} }"), serialized[0]);
    ASSERT_BSONOBJ_EQ(unpackSpecObj, serialized[1]);
}

TEST_F(InternalUnpackBucketSortReorderTest, OptimizeForMetaSortNegative) {
    auto unpackSpecObj = fromjson(
        "{$_internalUnpackBucket: { exclude: [], timeField: 'foo', metaField: 'meta1', "
        "bucketMaxSpanSeconds: 3600}}");
    auto countSpecObj = fromjson("{$sort: {'meta1.a': 1, 'unrelated': -1}}");

    auto pipeline = Pipeline::parse(makeVector(unpackSpecObj, countSpecObj), getExpCtx());
    pipeline->optimizePipeline();

    auto serialized = pipeline->serializeToBson();

    // $sort is still before unpack bucket stage.
    ASSERT_EQ(2, serialized.size());
    ASSERT_BSONOBJ_EQ(unpackSpecObj, serialized[0]);
    ASSERT_BSONOBJ_EQ(fromjson("{$sort: {'meta1.a': 1, 'unrelated': -1}}"), serialized[1]);
}

}  // namespace
}  // namespace mongo

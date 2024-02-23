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

#include "mongo/db/timeseries/bucket_catalog/bucket_metadata.h"
#include "mongo/unittest/unittest.h"

namespace mongo::timeseries::bucket_catalog {
namespace {

TEST(BucketMetadataTest, TrackedEmpty) {
    TrackingContext trackingContext;
    BSONElement metaElem;
    BucketMetadata untrackedMetadata{trackingContext, metaElem, nullptr, StringData{"m"}};
    ASSERT_EQ(trackingContext.allocated(), 0);
}

TEST(BucketMetadataTest, CloneAsTracked) {
    auto metaObj = BSON("meta" << 1);
    auto metaElem = metaObj.firstElement();
    BucketMetadata untrackedMetadata{metaElem, nullptr, StringData{"m"}};

    TrackingContext trackingContext;
    auto trackedMetadata1 = boost::make_optional(untrackedMetadata.cloneAsTracked(trackingContext));
    auto allocated = trackingContext.allocated();
    ASSERT_GTE(allocated, metaElem.size());

    // Making another tracked clone means we'll double-count the allocated memory.
    auto trackedMetadata2 = boost::make_optional(untrackedMetadata.cloneAsTracked(trackingContext));
    ASSERT_EQ(trackingContext.allocated(), allocated * 2);

    // Making a tracked clone from a tracked clone has the same effect.
    auto trackedMetadata3 = boost::make_optional(trackedMetadata2->cloneAsTracked(trackingContext));
    ASSERT_EQ(trackingContext.allocated(), allocated * 3);

    trackedMetadata1.reset();
    ASSERT_EQ(trackingContext.allocated(), allocated * 2);

    trackedMetadata2.reset();
    ASSERT_EQ(trackingContext.allocated(), allocated);

    trackedMetadata3.reset();
    ASSERT_EQ(trackingContext.allocated(), 0);
}

TEST(BucketMetadataTest, CloneAsUntracked) {
    TrackingContext trackingContext;
    auto metaObj = BSON("meta" << 1);
    auto metaElem = metaObj.firstElement();
    auto trackedMetadata =
        boost::make_optional(BucketMetadata{trackingContext, metaElem, nullptr, StringData{"m"}});

    auto allocated = trackingContext.allocated();
    ASSERT_GTE(allocated, metaElem.size());

    // Making an untracked clone does not affect the memory usage tracking.
    auto untrackedMetadata = boost::make_optional(trackedMetadata->cloneAsUntracked());
    ASSERT_EQ(trackingContext.allocated(), allocated);

    trackedMetadata.reset();
    ASSERT_EQ(trackingContext.allocated(), 0);
}

TEST(BucketMetadataTest, MoveTracked) {
    TrackingContext trackingContext;
    auto metaObj = BSON("meta" << 1);
    auto metaElem = metaObj.firstElement();
    auto trackedMetadata1 =
        boost::make_optional(BucketMetadata{trackingContext, metaElem, nullptr, StringData{"m"}});

    auto allocated = trackingContext.allocated();
    ASSERT_GTE(allocated, metaElem.size());

    auto trackedMetadata2 = boost::make_optional(std::move(*trackedMetadata1));
    ASSERT_EQ(trackingContext.allocated(), allocated);

    trackedMetadata1.reset();
    ASSERT_EQ(trackingContext.allocated(), allocated);

    auto trackedMetadata3 = boost::make_optional(BucketMetadata{std::move(*trackedMetadata2)});
    ASSERT_EQ(trackingContext.allocated(), allocated);

    trackedMetadata2.reset();
    ASSERT_EQ(trackingContext.allocated(), allocated);

    trackedMetadata3.reset();
    ASSERT_EQ(trackingContext.allocated(), 0);
}

}  // namespace
}  // namespace mongo::timeseries::bucket_catalog

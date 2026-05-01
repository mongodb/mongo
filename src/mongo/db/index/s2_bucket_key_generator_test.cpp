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


#include <algorithm>
#include <s2cell.h>
#include <s2latlng.h>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/index/expression_keys_private.h"
#include "mongo/db/index/expression_params.h"
#include "mongo/db/index/s2_common.h"
#include "mongo/db/json.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/timeseries/bucket_compression.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


using namespace mongo;

namespace {

std::string dumpKeyset(const KeyStringSet& keyStrings) {
    std::stringstream ss;
    ss << "[ ";
    for (auto& keyString : keyStrings) {
        auto key = KeyString::toBson(keyString, Ordering::make(BSONObj()));
        ss << key.toString() << " ";
    }
    ss << "]";

    return ss.str();
}

std::string dumpMultikeyPaths(const MultikeyPaths& multikeyPaths) {
    std::stringstream ss;

    ss << "[ ";
    for (const auto& multikeyComponents : multikeyPaths) {
        ss << "[ ";
        for (const auto& multikeyComponent : multikeyComponents) {
            ss << multikeyComponent << " ";
        }
        ss << "] ";
    }
    ss << "]";

    return ss.str();
}

bool areKeysetsEqual(const KeyStringSet& expectedKeys, const KeyStringSet& actualKeys) {
    if (expectedKeys.size() != actualKeys.size()) {
        LOGV2(206931,
              "Expected: {dumpKeyset_expectedKeys}, Actual: {dumpKeyset_actualKeys}",
              "dumpKeyset_expectedKeys"_attr = dumpKeyset(expectedKeys),
              "dumpKeyset_actualKeys"_attr = dumpKeyset(actualKeys));
        return false;
    }

    if (!std::equal(expectedKeys.begin(), expectedKeys.end(), actualKeys.begin())) {
        LOGV2(206941,
              "Expected: {dumpKeyset_expectedKeys}, Actual: {dumpKeyset_actualKeys}",
              "dumpKeyset_expectedKeys"_attr = dumpKeyset(expectedKeys),
              "dumpKeyset_actualKeys"_attr = dumpKeyset(actualKeys));
        return false;
    }

    return true;
}

void assertMultikeyPathsEqual(const MultikeyPaths& expectedMultikeyPaths,
                              const MultikeyPaths& actualMultikeyPaths) {
    if (expectedMultikeyPaths != actualMultikeyPaths) {
        FAIL(str::stream() << "Expected: " << dumpMultikeyPaths(expectedMultikeyPaths)
                           << ", Actual: " << dumpMultikeyPaths(actualMultikeyPaths));
    }
}

struct S2BucketKeyGeneratorTest : public unittest::Test {
    using PointSet = std::set<std::pair<long, long>>;
    SharedBufferFragmentBuilder allocator{KeyString::HeapBuilder::kHeapAllocatorDefaultBytes};

    void verifySetIsCoveredByKeys(const KeyStringSet& keys, const PointSet& points) const {
        std::vector<S2Cell> cells;
        for (const auto& key : keys) {
            auto obj = KeyString::toBson(key, Ordering::make(BSONObj()));
            cells.emplace_back(S2CellId(obj.firstElement().Long()));
        }

        for (const auto& point : points) {
            auto ll = S2LatLng::FromDegrees(point.second, point.first);
            auto pt = ll.ToPoint();
            bool covered = false;
            for (const auto& cell : cells) {
                if (cell.Contains(pt)) {
                    covered = true;
                    break;
                }
            }
            ASSERT_TRUE(covered);
        }
    }
};

TEST_F(S2BucketKeyGeneratorTest, GetS2BucketKeys) {
    BSONObj keyPattern = fromjson("{'data.geo': '2dsphere_bucket'}");
    BSONObj genKeysFrom = fromjson(
        "{control: {version: 1}, data: {geo: {"
        "'0': {type: 'Point', coordinates: [0, 0]},"
        "'1': {type: 'Point', coordinates: [3, 3]}"
        "}}}");
    BSONObj infoObj = fromjson("{key: {'data.geo': '2dsphere_bucket'}, '2dsphereIndexVersion': 3}");
    S2IndexingParams params;
    CollatorInterfaceMock* collator = nullptr;
    ExpressionParams::initialize2dsphereParams(infoObj, collator, &params);

    KeyStringSet actualKeys;
    MultikeyPaths actualMultikeyPaths;
    ExpressionKeysPrivate::getS2Keys(allocator,
                                     genKeysFrom,
                                     keyPattern,
                                     params,
                                     &actualKeys,
                                     &actualMultikeyPaths,
                                     KeyString::Version::kLatestVersion,
                                     SortedDataIndexAccessMethod::GetKeysContext::kAddingKeys,
                                     Ordering::make(BSONObj()));

    PointSet set{{0, 0}, {3, 3}};
    verifySetIsCoveredByKeys(actualKeys, set);
}

TEST_F(S2BucketKeyGeneratorTest, GetS2BucketKeysSubField) {
    BSONObj keyPattern = fromjson("{'data.geo.sub': '2dsphere_bucket'}");
    BSONObj genKeysFrom = fromjson(
        "{control: {version: 1}, data: {geo: {"
        "'0': {sub: {type: 'Point', coordinates: [0, 0]}},"
        "'1': {sub: {type: 'Point', coordinates: [3, 3]}}"
        "}}}");
    BSONObj infoObj =
        fromjson("{key: {'data.geo.sub': '2dsphere_bucket'}, '2dsphereIndexVersion': 3}");
    S2IndexingParams params;
    CollatorInterfaceMock* collator = nullptr;
    ExpressionParams::initialize2dsphereParams(infoObj, collator, &params);

    KeyStringSet actualKeys;
    MultikeyPaths actualMultikeyPaths;
    ExpressionKeysPrivate::getS2Keys(allocator,
                                     genKeysFrom,
                                     keyPattern,
                                     params,
                                     &actualKeys,
                                     &actualMultikeyPaths,
                                     KeyString::Version::kLatestVersion,
                                     SortedDataIndexAccessMethod::GetKeysContext::kAddingKeys,
                                     Ordering::make(BSONObj()));

    PointSet set{{0, 0}, {3, 3}};
    verifySetIsCoveredByKeys(actualKeys, set);
}

TEST_F(S2BucketKeyGeneratorTest, GetS2BucketKeysDeepSubField) {
    BSONObj keyPattern = fromjson("{'data.geo.sub1.sub2.sub3': '2dsphere_bucket'}");
    BSONObj genKeysFrom = fromjson(
        "{control: {version: 1}, data: {geo: {"
        "'0': {sub1: {sub2: {sub3: {type: 'Point', coordinates: [0, 0]}}}},"
        "'1': {sub1: {sub2: {sub3: {type: 'Point', coordinates: [3, 3]}}}}"
        "}}}");
    BSONObj infoObj = fromjson(
        "{key: {'data.geo.sub1.sub2.sub3': '2dsphere_bucket'}, '2dsphereIndexVersion': 3}");
    S2IndexingParams params;
    CollatorInterfaceMock* collator = nullptr;
    ExpressionParams::initialize2dsphereParams(infoObj, collator, &params);

    KeyStringSet actualKeys;
    MultikeyPaths actualMultikeyPaths;
    ExpressionKeysPrivate::getS2Keys(allocator,
                                     genKeysFrom,
                                     keyPattern,
                                     params,
                                     &actualKeys,
                                     &actualMultikeyPaths,
                                     KeyString::Version::kLatestVersion,
                                     SortedDataIndexAccessMethod::GetKeysContext::kAddingKeys,
                                     Ordering::make(BSONObj()));

    PointSet set{{0, 0}, {3, 3}};
    verifySetIsCoveredByKeys(actualKeys, set);
}

TEST_F(S2BucketKeyGeneratorTest, GetS2BucketKeysSubFieldSomeMissing) {
    BSONObj keyPattern = fromjson("{'data.geo.sub': '2dsphere_bucket'}");
    BSONObj genKeysFrom = fromjson(
        "{control: {version: 1}, data: {geo: {"
        "'0': {sub: {type: 'Point', coordinates: [0, 0]}},"
        "'1': {sub: {}},"
        "'2': {sub: null},"
        "'3': {sub: {type: 'Point', coordinates: [3, 3]}},"
        "'4': {sub: []},"
        "'5': {sub: {type: 'Point', coordinates: [5, 5]}},"
        "'6': {foo: 'bar'}"
        "}}}");
    BSONObj infoObj =
        fromjson("{key: {'data.geo.sub': '2dsphere_bucket'}, '2dsphereIndexVersion': 3}");
    S2IndexingParams params;
    CollatorInterfaceMock* collator = nullptr;
    ExpressionParams::initialize2dsphereParams(infoObj, collator, &params);

    KeyStringSet actualKeys;
    MultikeyPaths actualMultikeyPaths;
    ExpressionKeysPrivate::getS2Keys(allocator,
                                     genKeysFrom,
                                     keyPattern,
                                     params,
                                     &actualKeys,
                                     &actualMultikeyPaths,
                                     KeyString::Version::kLatestVersion,
                                     SortedDataIndexAccessMethod::GetKeysContext::kAddingKeys,
                                     Ordering::make(BSONObj()));

    PointSet set{{0, 0}, {3, 3}, {5, 5}};
    verifySetIsCoveredByKeys(actualKeys, set);
}

TEST_F(S2BucketKeyGeneratorTest, LogsGeoKeysForCoherentAndMixedCompressionBuckets) {
    const BSONObj keyPattern = fromjson(R"({"data.location": "2dsphere_bucket"})");
    const BSONObj infoObj =
        fromjson(R"({key: {"data.location": "2dsphere_bucket"}, "2dsphereIndexVersion": 3})");
    S2IndexingParams params;
    CollatorInterfaceMock* collator = nullptr;
    ExpressionParams::initialize2dsphereParams(infoObj, collator, &params);

    const auto v1Bucket = fromjson(R"({
        control: {version: 1},
        data: {
            time: {
                "0": {"$date": "2024-01-01T00:00:00.000Z"},
                "1": {"$date": "2024-01-01T00:00:01.000Z"},
                "2": {"$date": "2024-01-01T00:00:02.000Z"},
                "3": {"$date": "2024-01-01T00:00:03.000Z"}
            },
            location: {
                "0": {type: "Point", coordinates: [0, 0]},
                "1": {type: "Point", coordinates: [3, 3]},
                "2": {type: "Point", coordinates: [3, 3]},
                "3": {type: "Point", coordinates: [5, 5]}
            },
            sensor: {
                "0": "alpha",
                "2": "beta",
                "3": "gamma"
            }
        }
    })");

    const auto compressionResult = timeseries::compressBucket(
        v1Bucket,
        "time"_sd,
        NamespaceString::createNamespaceString_forTest("test.system.buckets.geo"),
        true);
    ASSERT_TRUE(compressionResult.compressedBucket.has_value());
    ASSERT_FALSE(compressionResult.decompressionFailed);
    const auto v2Bucket = compressionResult.compressedBucket.value();

    const auto mixedV2Bucket = std::invoke([&] {
        BSONObjBuilder mixedV2Builder;
        mixedV2Builder.append("control", v2Bucket.getField("control").Obj());
        {
            BSONObjBuilder data(mixedV2Builder.subobjStart("data"));
            data.append(v2Bucket.getObjectField("data").getField("time"));
            data.append(v1Bucket.getObjectField("data").getField("location"));
            data.append(v2Bucket.getObjectField("data").getField("sensor"));
        }
        return mixedV2Builder.obj();
    });

    BSONObjBuilder mixedV1Builder;
    mixedV1Builder.append(
        "control",
        BSON("version" << 1 << "count" << v2Bucket.getObjectField("control").getIntField("count")));
    mixedV1Builder.append("data", v2Bucket.getField("data").Obj());
    const auto mixedV1Bucket = mixedV1Builder.obj();

    auto generateKeys = [&](const BSONObj& bucket) {
        KeyStringSet keys;
        MultikeyPaths multikeyPaths;
        ExpressionKeysPrivate::getS2Keys(allocator,
                                         bucket,
                                         keyPattern,
                                         params,
                                         &keys,
                                         &multikeyPaths,
                                         KeyString::Version::kLatestVersion,
                                         SortedDataIndexAccessMethod::GetKeysContext::kAddingKeys,
                                         Ordering::make(BSONObj()));
        return keys;
    };

    const auto v1Keys = generateKeys(v1Bucket);
    const auto v2Keys = generateKeys(v2Bucket);
    const auto mixedV2Keys = generateKeys(mixedV2Bucket);
    const auto mixedV1Keys = generateKeys(mixedV1Bucket);

    ASSERT_TRUE(areKeysetsEqual(v1Keys, v2Keys));
    ASSERT_TRUE(areKeysetsEqual(v1Keys, mixedV2Keys));
    ASSERT_TRUE(areKeysetsEqual(v1Keys, mixedV1Keys));

    PointSet set{{0, 0}, {3, 3}, {5, 5}};
    verifySetIsCoveredByKeys(v1Keys, set);
    verifySetIsCoveredByKeys(v2Keys, set);
    verifySetIsCoveredByKeys(mixedV2Keys, set);
    verifySetIsCoveredByKeys(mixedV1Keys, set);
}

}  // namespace

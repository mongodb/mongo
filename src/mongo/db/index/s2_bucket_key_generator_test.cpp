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


#include "mongo/platform/basic.h"

#include "mongo/db/index/expression_keys_private.h"

#include <algorithm>

#include "third_party/s2/s2cell.h"
#include "third_party/s2/s2latlng.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/index/expression_params.h"
#include "mongo/db/index/s2_common.h"
#include "mongo/db/json.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
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

}  // namespace

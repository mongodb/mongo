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


#include "mongo/db/index/2d_key_generator.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/bson/ordering.h"
#include "mongo/db/geo/hash.h"
#include "mongo/db/index/2d_common.h"
#include "mongo/db/index/expression_params.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/logv2/log.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/shared_buffer_fragment.h"

#include <algorithm>
#include <memory>
#include <ostream>
#include <string>

#include <boost/container/flat_set.hpp>
#include <boost/container/vector.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


using namespace mongo;

namespace {

std::string dumpKeyset(const KeyStringSet& keyStrings) {
    std::stringstream ss;
    ss << "[ ";
    for (auto& keyString : keyStrings) {
        auto key = key_string::toBson(keyString, Ordering::make(BSONObj()));
        ss << key.toString() << " ";
    }
    ss << "]";

    return ss.str();
}

bool assertKeysetsEqual(const KeyStringSet& expectedKeys, const KeyStringSet& actualKeys) {
    if (expectedKeys.size() != actualKeys.size()) {
        LOGV2(20645,
              "Expected: {dumpKeyset_expectedKeys}, Actual: {dumpKeyset_actualKeys}",
              "dumpKeyset_expectedKeys"_attr = dumpKeyset(expectedKeys),
              "dumpKeyset_actualKeys"_attr = dumpKeyset(actualKeys));
        return false;
    }

    if (!std::equal(expectedKeys.begin(), expectedKeys.end(), actualKeys.begin())) {
        LOGV2(20646,
              "Expected: {dumpKeyset_expectedKeys}, Actual: {dumpKeyset_actualKeys}",
              "dumpKeyset_expectedKeys"_attr = dumpKeyset(expectedKeys),
              "dumpKeyset_actualKeys"_attr = dumpKeyset(actualKeys));
        return false;
    }

    return true;
}

key_string::Value make2DKey(const TwoDIndexingParams& params,
                            int x,
                            int y,
                            BSONElement trailingFields) {
    BSONObjBuilder bob;
    BSONObj locObj = BSON_ARRAY(x << y);
    params.geoHashConverter->hash(locObj, nullptr).appendHashMin(&bob, "");
    bob.append(trailingFields);
    key_string::HeapBuilder keyString(
        key_string::Version::kLatestVersion, bob.obj(), Ordering::make(BSONObj()));
    return keyString.release();
}

struct TwoDKeyGeneratorTest : public unittest::Test {
    SharedBufferFragmentBuilder allocator{key_string::HeapBuilder::kHeapAllocatorDefaultBytes};
};

TEST_F(TwoDKeyGeneratorTest, TrailingField) {
    BSONObj obj = fromjson("{a: [0, 0], b: 5}");
    BSONObj infoObj = fromjson("{key: {a: '2d', b: 1}}");
    TwoDIndexingParams params;
    index2d::parse2dParams(infoObj, &params);
    KeyStringSet actualKeys;
    index2d::get2DKeys(allocator,
                       obj,
                       params,
                       &actualKeys,
                       key_string::Version::kLatestVersion,
                       Ordering::make(BSONObj()));

    KeyStringSet expectedKeys;
    BSONObj trailingFields = BSON("" << 5);
    expectedKeys.insert(make2DKey(params, 0, 0, trailingFields.firstElement()));

    ASSERT(assertKeysetsEqual(expectedKeys, actualKeys));
}

TEST_F(TwoDKeyGeneratorTest, ArrayTrailingField) {
    BSONObj obj = fromjson("{a: [0, 0], b: [5, 6]}");
    BSONObj infoObj = fromjson("{key: {a: '2d', b: 1}}");
    TwoDIndexingParams params;
    index2d::parse2dParams(infoObj, &params);
    KeyStringSet actualKeys;
    index2d::get2DKeys(allocator,
                       obj,
                       params,
                       &actualKeys,
                       key_string::Version::kLatestVersion,
                       Ordering::make(BSONObj()));

    KeyStringSet expectedKeys;
    BSONObj trailingFields = BSON("" << BSON_ARRAY(5 << 6));
    expectedKeys.insert(make2DKey(params, 0, 0, trailingFields.firstElement()));

    ASSERT(assertKeysetsEqual(expectedKeys, actualKeys));
}

TEST_F(TwoDKeyGeneratorTest, ArrayOfObjectsTrailingField) {
    BSONObj obj = fromjson("{a: [0, 0], b: [{c: 5}, {c: 6}]}");
    BSONObj infoObj = fromjson("{key: {a: '2d', 'b.c': 1}}");
    TwoDIndexingParams params;
    index2d::parse2dParams(infoObj, &params);
    KeyStringSet actualKeys;
    index2d::get2DKeys(allocator,
                       obj,
                       params,
                       &actualKeys,
                       key_string::Version::kLatestVersion,
                       Ordering::make(BSONObj()));

    KeyStringSet expectedKeys;
    BSONObj trailingFields = BSON("" << BSON_ARRAY(5 << 6));
    expectedKeys.insert(make2DKey(params, 0, 0, trailingFields.firstElement()));

    ASSERT(assertKeysetsEqual(expectedKeys, actualKeys));
}

}  // namespace

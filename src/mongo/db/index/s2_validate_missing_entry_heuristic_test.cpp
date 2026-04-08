/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/bson/ordering.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/s2_common.h"
#include "mongo/db/index/s2_key_generator.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/shared_buffer_fragment.h"

#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

KeyStringSet buildS2KeySetForSpecVersion(const BSONObj& infoObj,
                                         const BSONObj& keyPattern,
                                         const BSONObj& document,
                                         boost::optional<S2IndexVersion> forcedVersion) {
    S2IndexingParams params;
    const CollatorInterface* collator = nullptr;
    index2dsphere::initialize2dsphereParams(infoObj, collator, &params);
    if (forcedVersion) {
        params.indexVersion = *forcedVersion;
    }

    SharedBufferFragmentBuilder pool(key_string::HeapBuilder::kHeapAllocatorDefaultBytes);
    KeyStringSet keys;
    index2dsphere::getS2Keys(pool,
                             document,
                             keyPattern,
                             params,
                             &keys,
                             nullptr,
                             key_string::Version::kLatestVersion,
                             SortedDataIndexAccessMethod::GetKeysContext::kAddingKeys,
                             Ordering::make(BSONObj()),
                             boost::none);
    return keys;
}

TEST(S2ValidateMissingEntryHeuristic, V3AndV4KeySetsDifferForLegacyPlusGeoJSONPoint) {
    BSONObj doc =
        BSON("_id" << 1 << "location"
                   << BSON("latitude" << 38.7348661 << "longitude" << -9.1447487 << "coordinates"
                                      << BSON_ARRAY(-9.1447487 << 38.7348661) << "type"
                                      << "Point"));
    BSONObj keyPattern = BSON("location" << "2dsphere");
    BSONObj infoObj = BSON("key" << keyPattern << "2dsphereIndexVersion" << S2_INDEX_VERSION_3);

    KeyStringSet v3Keys = buildS2KeySetForSpecVersion(infoObj, keyPattern, doc, boost::none);
    KeyStringSet v4Keys = buildS2KeySetForSpecVersion(
        infoObj, keyPattern, doc, boost::optional<S2IndexVersion>(S2_INDEX_VERSION_4));

    ASSERT_FALSE(v3Keys.empty());
    ASSERT_FALSE(v4Keys.empty());
    ASSERT_NE(v3Keys, v4Keys);

    size_t v3OnlyCount = 0;
    for (const auto& k : v3Keys) {
        if (!v4Keys.contains(k)) {
            ++v3OnlyCount;
        }
    }
    ASSERT_GTE(v3OnlyCount, 1u);
}

TEST(S2ValidateMissingEntryHeuristic, V3AndV4KeySetsMatchForMultiPoint) {
    BSONObj doc = fromjson(
        R"({ _id: 1, geo: { type: "MultiPoint", coordinates: [[1.0, 2.0], [50.0, 51.0]] } })");
    BSONObj keyPattern = BSON("geo" << "2dsphere");
    BSONObj infoObj = BSON("key" << keyPattern << "2dsphereIndexVersion" << S2_INDEX_VERSION_3);

    KeyStringSet v3Keys = buildS2KeySetForSpecVersion(infoObj, keyPattern, doc, boost::none);
    KeyStringSet v4Keys = buildS2KeySetForSpecVersion(
        infoObj, keyPattern, doc, boost::optional<S2IndexVersion>(S2_INDEX_VERSION_4));

    ASSERT_GTE(v3Keys.size(), 2u);
    ASSERT_EQ(v3Keys, v4Keys);
}

}  // namespace
}  // namespace mongo

/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/ce/stats_gen.h"
#include "mongo/db/query/ce/stats_serialization_utils.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {

IDLParserContext ctx("StatsPath");


/**
 *  Validate round trip convertion for numeric SBE values using NumberDouble datatype.
 */
TEST(StatsPath, BasicValidStatsBucketDouble) {

    auto value =
        stats_serialization_utils::TypeValuePair(sbe::value::TypeTags::NumberDouble, double{1});
    auto serializedBucket = stats_serialization_utils::makeStatsBucket(value, 1, 2, 3, 4, 5);
    auto parsedBucket = StatsBucket::parse(ctx, serializedBucket);

    // Verify the upperBoundary field conversion.
    ASSERT(sbe::bson::convertFrom<1>(parsedBucket.getUpperBoundary().getElement()) == value);

    // roundtrip
    auto bucketToBSON = parsedBucket.toBSON();
    ASSERT_BSONOBJ_EQ(serializedBucket, bucketToBSON);
}

/**
 *  Validate round trip convertion for heap SBE values using StringBig datatype.
 */
TEST(StatsPath, BasicValidStatsBucketString) {
    auto aString = "abcdef"_sd;
    auto value = sbe::value::makeBigString(aString);
    auto serializedBucket = stats_serialization_utils::makeStatsBucket(value, 1, 2, 3, 4, 5);
    auto parsedBucket = StatsBucket::parse(ctx, serializedBucket);

    // Verify the upperBoundary field conversion.
    auto rtValue = sbe::bson::convertFrom<1>(parsedBucket.getUpperBoundary().getElement());
    ASSERT(sbe::value::getStringView(rtValue.first, rtValue.second) == aString);

    // roundtrip
    auto bucketToBSON = parsedBucket.toBSON();
    ASSERT_BSONOBJ_EQ(serializedBucket, bucketToBSON);
}

/**
 *  Validate round trip convertion for StatsPath datatype.
 */
TEST(StatsPath, BasicValidStatsPath) {

    std::list<BSONObj> buckets;
    for (long i = 1; i <= 3; i++) {
        auto typeValue = stats_serialization_utils::TypeValuePair(
            sbe::value::TypeTags::NumberDouble, double{i + 1.0});

        auto bucket = stats_serialization_utils::makeStatsBucket(typeValue, i, i, i, i, i);
        buckets.push_back(bucket);
    }
    stats_serialization_utils::TypeCount types;
    for (long i = 1; i <= 3; i++) {
        std::stringstream typeName;
        typeName << "type" << i;
        auto typeElem = std::pair<std::string, long>(typeName.str(), i);
        types.push_back(typeElem);
    }
    auto serializedPath = stats_serialization_utils::makeStatsPath(
        "somePath", 100, 10, 0.1, 10, std::make_pair(4L, 6L), types, buckets, boost::none);

    auto parsedPath = StatsPath::parse(ctx, serializedPath);
    auto pathToBSON = parsedPath.toBSON();

    ASSERT_BSONOBJ_EQ(serializedPath, pathToBSON);
}

}  // namespace
}  // namespace mongo

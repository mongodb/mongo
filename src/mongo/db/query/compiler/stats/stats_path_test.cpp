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

#include "mongo/base/string_data.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/compiler/stats/ce_histogram.h"
#include "mongo/db/query/compiler/stats/scalar_histogram.h"
#include "mongo/db/query/compiler/stats/stats_gen.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/unittest/unittest.h"

#include <utility>
#include <vector>

namespace mongo::stats {
namespace {

IDLParserContext ctx("StatsPath");

/**
 *  Validate round trip conversion for histogram bucket
 */
TEST(StatsPath, BasicValidStatsBucketDouble) {
    // Create & parse StatsBucket.
    auto serializedBucket = Bucket{3.0, 4.0, 15.0, 2.0, 6.0}.serialize();
    auto parsedBucket = StatsBucket::parse(serializedBucket, ctx);

    // Round-trip conversion.
    auto bucketToBSON = parsedBucket.toBSON();
    ASSERT_BSONOBJ_EQ(serializedBucket, bucketToBSON);
}

/**
 *  Validate round-trip conversion for StatsPath datatype.
 */
TEST(StatsPath, BasicValidStatsPath) {
    // Initialize histogram buckets.
    constexpr double doubleCount = 15.0;
    constexpr double trueCount = 12.0;
    constexpr double falseCount = 16.0;
    constexpr double numDocs = doubleCount + trueCount + falseCount;
    std::vector<Bucket> buckets{
        Bucket{1.0, 0.0, 1.0, 0.0, 1.0},
        Bucket{2.0, 5.0, 8.0, 1.0, 3.0},
        Bucket{3.0, 4.0, 15.0, 2.0, 6.0},
    };

    // Initialize histogram bounds.
    auto [boundsTag, boundsVal] = sbe::value::makeNewArray();
    sbe::value::ValueGuard boundsGuard{boundsTag, boundsVal};
    auto bounds = sbe::value::getArrayView(boundsVal);
    bounds->push_back(sbe::value::TypeTags::NumberDouble, 1.0);
    bounds->push_back(sbe::value::TypeTags::NumberDouble, 2.0);
    bounds->push_back(sbe::value::TypeTags::NumberDouble, 3.0);

    // Create a scalar histogram.
    TypeCounts tc{
        {sbe::value::TypeTags::NumberDouble, doubleCount},
        {sbe::value::TypeTags::Boolean, trueCount + falseCount},
    };
    const auto sh = ScalarHistogram::make(*bounds, buckets);
    auto cehist = CEHistogram::make(std::move(sh), tc, numDocs, trueCount, falseCount);

    // Serialize to BSON.
    constexpr double sampleRate = 1.0;
    auto serializedPath = stats::makeStatsPath("somePath", numDocs, sampleRate, cehist);

    // Parse StatsPath via IDL & serialize to BSON.
    auto parsedPath = StatsPath::parse(serializedPath, ctx);
    auto parsedPathToBSON = parsedPath.toBSON();

    // We should end up with the same serialized BSON in the end.
    ASSERT_BSONOBJ_EQ(serializedPath, parsedPathToBSON);
}

/**
 *  Validate round-trip conversion for StatsPath datatype.
 */
TEST(StatsPath, BasicValidEmptyStatsPath) {
    // Initialize histogram buckets.
    constexpr double numDocs = 0.0;
    std::vector<Bucket> buckets;

    // Create an empty scalar histogram.
    auto cehist = CEHistogram::make(ScalarHistogram::make(), TypeCounts{}, 0.0 /* sampleSize */);

    // Serialize to BSON.
    constexpr double sampleRate = 1.0;
    auto serializedPath = stats::makeStatsPath("someEmptyPath", numDocs, sampleRate, cehist);

    // Parse StatsPath via IDL & serialize to BSON.
    auto parsedPath = StatsPath::parse(serializedPath, ctx);
    auto parsedPathToBSON = parsedPath.toBSON();

    // We should end up with the same serialized BSON in the end.
    ASSERT_BSONOBJ_EQ(serializedPath, parsedPathToBSON);
}

}  // namespace
}  // namespace mongo::stats

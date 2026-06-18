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

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/global_catalog/shard_key_pattern.h"
#include "mongo/db/global_catalog/type_collection_common_types_gen.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/s/resharding/common_types_gen.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <string>
#include <string_view>

#include <boost/optional/optional.hpp>
#include <gtest/gtest.h>

namespace mongo {
namespace resharding {
namespace {

const ShardKeyPattern kSourceShardKey(BSON("a" << 1));
const BSONObj kUserShardKey = BSON("b" << 1);

TypeCollectionTimeseriesFields makeTimeseriesFields(std::string_view timeField,
                                                    boost::optional<std::string_view> metaField) {
    TimeseriesOptions options{std::string{timeField}};
    if (metaField) {
        options.setMetaField(*metaField);
    }
    TypeCollectionTimeseriesFields fields;
    fields.setTimeseriesOptions(std::move(options));
    return fields;
}

class ReshardingProvenanceTest : public ::testing::TestWithParam<ReshardingProvenanceEnum> {
protected:
    void validateCollection(bool isSharded,
                            const BSONObj& finalKey = kUserShardKey,
                            bool forceRedistribution = false) {
        validateReshardCollectionRequest(
            GetParam(), isSharded, kSourceShardKey, finalKey, forceRedistribution);
    }
};

INSTANTIATE_TEST_SUITE_P(Provenance,
                         ReshardingProvenanceTest,
                         ::testing::Values(ReshardingProvenanceEnum::kReshardCollection,
                                           ReshardingProvenanceEnum::kMoveCollection,
                                           ReshardingProvenanceEnum::kUnshardCollection,
                                           ReshardingProvenanceEnum::kRewriteCollection),
                         [](const ::testing::TestParamInfo<ReshardingProvenanceEnum>& info) {
                             return std::string(idl::serialize(info.param));
                         });

// =====================================================================================
// computeReshardingShardKey
// =====================================================================================

TEST_P(ReshardingProvenanceTest, ComputeShardKey) {
    auto result =
        computeReshardingShardKey(GetParam(), kSourceShardKey, boost::none, kUserShardKey);
    if (isRewriteCollection(GetParam()))
        ASSERT_BSONOBJ_EQ(result, kSourceShardKey.getKeyPattern().toBSON());
    else
        ASSERT_BSONOBJ_EQ(result, kUserShardKey);
}

TEST_P(ReshardingProvenanceTest, ComputeShardKeyMissingUserKey) {
    auto computeWithNoUserKey = [&] {
        return computeReshardingShardKey(GetParam(), kSourceShardKey, boost::none, boost::none);
    };

    if (isRewriteCollection(GetParam()))
        ASSERT_BSONOBJ_EQ(computeWithNoUserKey(), kSourceShardKey.getKeyPattern().toBSON());
    else
        ASSERT_THROWS_CODE(computeWithNoUserKey(), DBException, ErrorCodes::InvalidOptions);
}

TEST_P(ReshardingProvenanceTest, ComputeShardKeyTimeseries) {
    // reshardCollection must translate userKey from user-facing field to bucket-level, otherwise it
    // may incorrectly throws InvalidOptions.
    const ShardKeyPattern tsSourceKey(BSON("meta.x" << 1));
    const BSONObj tsUserKey = BSON("metadata.x" << 1);

    auto tsFields = makeTimeseriesFields("time", std::string_view("metadata"));
    auto result = computeReshardingShardKey(GetParam(), tsSourceKey, tsFields, tsUserKey);

    if (isRewriteCollection(GetParam()) || isOrdinaryReshardCollection(GetParam()))
        ASSERT_BSONOBJ_EQ(result, tsSourceKey.getKeyPattern().toBSON());
    else
        ASSERT_BSONOBJ_EQ(result, tsUserKey);
}

// =====================================================================================
// validateReshardCollectionRequest
// =====================================================================================

TEST_P(ReshardingProvenanceTest, ValidateAcceptsValidShardedness) {
    if (isMoveCollection(GetParam())) {
        validateCollection(/*isSharded=*/false);
    } else {
        validateCollection(/*isSharded=*/true);
    }
}

TEST_P(ReshardingProvenanceTest, ValidateRejectsInvalidShardedness) {
    if (isUnshardCollection(GetParam())) {
        // unshardCollection does not validate sharded at the reshardCollectionCoordinator level.
        return;
    }

    if (isMoveCollection(GetParam()))
        ASSERT_THROWS_CODE(
            validateCollection(/*isSharded=*/true), DBException, ErrorCodes::NamespaceNotFound);
    else
        ASSERT_THROWS_CODE(
            validateCollection(/*isSharded=*/false), DBException, ErrorCodes::NamespaceNotSharded);
}

TEST_P(ReshardingProvenanceTest, ForceRedistributionIdentityKeyAccepted) {
    bool isSharded = !isMoveCollection(GetParam());
    validateCollection(
        isSharded, kSourceShardKey.getKeyPattern().toBSON(), /*forceRedistribution=*/true);
}

TEST_P(ReshardingProvenanceTest, ForceRedistributionWithDifferentKey) {
    if (isMoveCollection(GetParam()) || isUnshardCollection(GetParam())) {
        // forceRedistribution is not applicable to moveCollection and unshardCollection.
        return;
    }

    ASSERT_THROWS_CODE(
        validateCollection(/*isSharded=*/true, kUserShardKey, /*forceRedistribution=*/true),
        DBException,
        ErrorCodes::InvalidOptions);
}

TEST(ValidateReshardCollectionRequestTest, NoProvenanceRequiresShardedSource) {
    // When no provenance is provided, it defaults to reshardCollection.
    ASSERT_THROWS_CODE(
        validateReshardCollectionRequest(boost::none, false, kSourceShardKey, kUserShardKey, false),
        DBException,
        ErrorCodes::NamespaceNotSharded);
}

}  // namespace
}  // namespace resharding
}  // namespace mongo

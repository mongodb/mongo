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

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/global_catalog/type_tags.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/s/balancer/balancer.h"
#include "mongo/db/sharding_environment/sharding_statistics.h"
#include "mongo/unittest/unittest.h"

#include <string>

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

const std::string kCollKeyField = "coll." + std::string{CollectionType::kKeyPatternFieldName};

const BSONObj kCompoundKey = BSON("a" << 1 << "b" << 1);
const BSONObj kTripleKey = BSON("a" << 1 << "b" << 1 << "c" << 1);

TEST(BalancerMaxKeyZoneScanTest, MaxKeyPrefixWithTrailingMinKeyIsBuggy) {
    ASSERT_TRUE(
        Balancer::isBuggyMinKeyZoneFingerprint(kCompoundKey, BSON("a" << MAXKEY << "b" << MINKEY)));
}

TEST(BalancerMaxKeyZoneScanTest, MaxKeyPrefixRunOfMaxKeyWithTrailingMinKeyIsBuggy) {
    ASSERT_TRUE(Balancer::isBuggyMinKeyZoneFingerprint(
        kTripleKey, BSON("a" << MAXKEY << "b" << MAXKEY << "c" << MINKEY)));
}

TEST(BalancerMaxKeyZoneScanTest, LegitimatePrefixZoneWithTrailingMinKeyIsNotBuggy) {
    ASSERT_FALSE(
        Balancer::isBuggyMinKeyZoneFingerprint(kCompoundKey, BSON("a" << 10 << "b" << MINKEY)));
}

TEST(BalancerMaxKeyZoneScanTest, NormalValueBeforeMaxKeyTailIsNotBuggy) {
    ASSERT_FALSE(Balancer::isBuggyMinKeyZoneFingerprint(
        kTripleKey, BSON("a" << 5 << "b" << MAXKEY << "c" << MINKEY)));
}

TEST(BalancerMaxKeyZoneScanTest, WellFormedCompoundUpperBoundIsNotBuggy) {
    ASSERT_FALSE(
        Balancer::isBuggyMinKeyZoneFingerprint(kCompoundKey, BSON("a" << 10 << "b" << MAXKEY)));
}

TEST(BalancerMaxKeyZoneScanTest, AllMaxKeyUpperBoundIsNotBuggy) {
    ASSERT_FALSE(
        Balancer::isBuggyMinKeyZoneFingerprint(kCompoundKey, BSON("a" << MAXKEY << "b" << MAXKEY)));
}

TEST(BalancerMaxKeyZoneScanTest, AllMinKeyUpperBoundIsNotBuggy) {
    ASSERT_FALSE(
        Balancer::isBuggyMinKeyZoneFingerprint(kCompoundKey, BSON("a" << MINKEY << "b" << MINKEY)));
}

TEST(BalancerMaxKeyZoneScanTest, MinKeyPrefixWithMaxKeyTailIsNotBuggy) {
    ASSERT_FALSE(
        Balancer::isBuggyMinKeyZoneFingerprint(kCompoundKey, BSON("a" << MINKEY << "b" << MAXKEY)));
}

TEST(BalancerMaxKeyZoneScanTest, MaxKeyAfterMinKeyIsNotBuggy) {
    ASSERT_FALSE(Balancer::isBuggyMinKeyZoneFingerprint(
        kTripleKey, BSON("a" << MAXKEY << "b" << MINKEY << "c" << MAXKEY)));
}

TEST(BalancerMaxKeyZoneScanTest, FieldCountMismatchIsNotClassified) {
    // Fewer fields in the upper bound than in the shard key pattern.
    ASSERT_FALSE(
        Balancer::isBuggyMinKeyZoneFingerprint(kTripleKey, BSON("a" << MAXKEY << "b" << MINKEY)));
    // More fields in the upper bound than in the shard key pattern.
    ASSERT_FALSE(Balancer::isBuggyMinKeyZoneFingerprint(
        kCompoundKey, BSON("a" << MAXKEY << "b" << MINKEY << "c" << MINKEY)));
}

TEST(BalancerMaxKeyZoneScanTest, PipelineHasExpectedStagesInOrder) {
    const auto pipeline = Balancer::buildMaxKeyZoneScanPipeline();
    ASSERT_EQ(pipeline.size(), 4u);
    ASSERT_TRUE(pipeline[0].hasField("$lookup"));
    ASSERT_TRUE(pipeline[1].hasField("$unwind"));
    ASSERT_TRUE(pipeline[2].hasField("$match"));
    ASSERT_TRUE(pipeline[3].hasField("$project"));
}

TEST(BalancerMaxKeyZoneScanTest, PipelineLookupJoinsCollectionsOnNamespace) {
    const auto pipeline = Balancer::buildMaxKeyZoneScanPipeline();
    const auto lookup = pipeline[0].getObjectField("$lookup");
    ASSERT_EQ(lookup.getStringField("from"),
              NamespaceString::kConfigsvrCollectionsNamespace.coll());
    ASSERT_EQ(lookup.getStringField("localField"), TagsType::ns());
    ASSERT_EQ(lookup.getStringField("foreignField"), CollectionType::kNssFieldName);
    ASSERT_EQ(lookup.getStringField("as"), "coll"sv);
    // $unwind drops tags whose collection did not join.
    ASSERT_EQ(pipeline[1].getStringField("$unwind"), "$coll"sv);
}

TEST(BalancerMaxKeyZoneScanTest, PipelineMatchKeepsOnlyCompoundShardKeys) {
    const auto pipeline = Balancer::buildMaxKeyZoneScanPipeline();
    const auto expected =
        BSON("$match" << BSON(
                 "$expr" << BSON(
                     "$gte" << BSON_ARRAY(
                         BSON("$size" << BSON("$objectToArray" << ("$" + kCollKeyField))) << 2))));
    ASSERT_BSONOBJ_EQ(pipeline[2], expected);
}

TEST(BalancerMaxKeyZoneScanTest, PipelineProjectsOnlyClassifiedFields) {
    const auto pipeline = Balancer::buildMaxKeyZoneScanPipeline();
    const auto project = pipeline[3].getObjectField("$project");
    ASSERT_EQ(project.nFields(), 4);
    ASSERT_TRUE(project.hasField(TagsType::ns()));
    ASSERT_TRUE(project.hasField(TagsType::tag()));
    ASSERT_TRUE(project.hasField(TagsType::max()));
    ASSERT_TRUE(project.hasField(kCollKeyField));
}

TEST(BalancerMaxKeyZoneScanTest, ShardingStatisticsReportIncludesZoneScanFields) {
    ShardingStatistics stats;
    stats.maxKeyZoneScanComplete.store(1);
    stats.maxKeyZoneScanFoundBuggyZone.store(1);
    stats.maxKeyZoneScanAlertEmitted.store(1);
    stats.maxKeyZoneScanErrors.store(2);

    BSONObjBuilder bob;
    stats.report(&bob);
    const BSONObj obj = bob.obj();

    ASSERT_EQ(1LL, obj["maxKeyZoneScanComplete"].Long());
    ASSERT_EQ(1LL, obj["maxKeyZoneScanFoundBuggyZone"].Long());
    ASSERT_EQ(1LL, obj["maxKeyZoneScanAlertEmitted"].Long());
    ASSERT_EQ(2LL, obj["maxKeyZoneScanErrors"].Long());
}

TEST(BalancerMaxKeyZoneScanTest, ShardingStatisticsZoneScanFieldsDefaultToZero) {
    ShardingStatistics stats;

    BSONObjBuilder bob;
    stats.report(&bob);
    const BSONObj obj = bob.obj();

    ASSERT_EQ(0LL, obj["maxKeyZoneScanComplete"].Long());
    ASSERT_EQ(0LL, obj["maxKeyZoneScanFoundBuggyZone"].Long());
    ASSERT_EQ(0LL, obj["maxKeyZoneScanAlertEmitted"].Long());
    ASSERT_EQ(0LL, obj["maxKeyZoneScanErrors"].Long());
}

}  // namespace
}  // namespace mongo

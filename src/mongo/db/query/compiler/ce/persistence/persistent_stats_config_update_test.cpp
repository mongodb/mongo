// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/ce/persistence/persistent_stats_config_update.h"

#include "mongo/bson/json.h"
#include "mongo/db/query/compiler/ce/persistence/persistent_stats_config_gen.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/unittest/unittest.h"

#include <cstdint>
#include <variant>

#include <boost/optional.hpp>

namespace mongo {
namespace {

PersistentStatsConfigUpdate parsePersistentStatsConfigUpdate(const BSONObj& update) {
    return PersistentStatsConfigUpdate::parse(update, IDLParserContext("persistentStatsConfig"));
}

boost::optional<PersistentStatsConfig> makeConfig(boost::optional<std::int32_t> sampleSize,
                                                  boost::optional<std::int32_t> joinSampleSize) {
    PersistentStatsSamplesConfig samples;
    samples.setSampleSize(sampleSize);
    samples.setJoinSampleSize(joinSampleSize);

    PersistentStatsConfig config;
    config.setSamples(samples);
    return config;
}

}  // namespace

TEST(PersistentStatsConfigUpdate, ParsesDelta) {
    const auto update = parsePersistentStatsConfigUpdate(fromjson("{samples: {sampleSize: 1000}}"));
    ASSERT_TRUE(update.getSamples());
    ASSERT_TRUE(update.getSamples()->getSampleSize());
    ASSERT_TRUE(std::holds_alternative<SetSampleSize>(*update.getSamples()->getSampleSize()));
    ASSERT_EQUALS(std::get<SetSampleSize>(*update.getSamples()->getSampleSize()).value, 1000);
    ASSERT_FALSE(update.getSamples()->getJoinSampleSize());

    // A null sample size means "remove it" and must round-trip so that the oplog and the sharded
    // DDL path apply the same update.
    const auto removal =
        parsePersistentStatsConfigUpdate(fromjson("{samples: {joinSampleSize: null}}"));
    ASSERT_TRUE(removal.getSamples());
    ASSERT_TRUE(removal.getSamples()->getJoinSampleSize());
    ASSERT_TRUE(
        std::holds_alternative<RemoveSampleSize>(*removal.getSamples()->getJoinSampleSize()));
    ASSERT_BSONOBJ_EQ(removal.toBSON(), fromjson("{samples: {joinSampleSize: null}}"));

    const auto mixed = parsePersistentStatsConfigUpdate(
        fromjson("{samples: {sampleSize: null, joinSampleSize: 2000}}"));
    ASSERT_TRUE(mixed.getSamples()->getSampleSize());
    ASSERT_TRUE(std::holds_alternative<RemoveSampleSize>(*mixed.getSamples()->getSampleSize()));
    ASSERT_EQUALS(std::get<SetSampleSize>(*mixed.getSamples()->getJoinSampleSize()).value, 2000);
}

TEST(PersistentStatsConfigUpdate, CoercesIntegralDoubleToSampleSize) {
    // Sample sizes are read with BSONElement::safeNumberInt(), so integral doubles are accepted
    // and truncated the same way knobs and server parameters accept them.
    const auto update =
        parsePersistentStatsConfigUpdate(BSON("samples" << BSON("sampleSize" << 1.0)));
    ASSERT_TRUE(update.getSamples()->getSampleSize());
    ASSERT_TRUE(std::holds_alternative<SetSampleSize>(*update.getSamples()->getSampleSize()));
    ASSERT_EQUALS(std::get<SetSampleSize>(*update.getSamples()->getSampleSize()).value, 1);
}

TEST(PersistentStatsConfig, CoercesIntegralDoubleToSampleSize) {
    // The stored configuration declares its sample sizes as safeInt, which is also parsed with
    // BSONElement::safeNumberInt().
    const auto config = PersistentStatsConfig::parse(BSON("samples" << BSON("sampleSize" << 1.0)),
                                                     IDLParserContext("persistentStatsConfig"));
    ASSERT_TRUE(config.getSamples());
    ASSERT_EQUALS(*config.getSamples()->getSampleSize(), 1);
}

TEST(PersistentStatsConfigUpdate, RejectsUnknownFields) {
    ASSERT_THROWS(parsePersistentStatsConfigUpdate(fromjson("{unknownField: 1}")), DBException);
    ASSERT_THROWS(parsePersistentStatsConfigUpdate(fromjson("{samples: {unknownField: 1}}")),
                  DBException);
    ASSERT_THROWS(parsePersistentStatsConfigUpdate(fromjson("{samples: 5}")), DBException);
}

TEST(PersistentStatsConfigUpdate, RejectsInvalidSampleSizes) {
    ASSERT_THROWS(parsePersistentStatsConfigUpdate(fromjson("{samples: {sampleSize: 0}}")),
                  DBException);
    ASSERT_THROWS(parsePersistentStatsConfigUpdate(fromjson("{samples: {joinSampleSize: -1}}")),
                  DBException);
    ASSERT_THROWS(parsePersistentStatsConfigUpdate(fromjson("{samples: {sampleSize: 'x'}}")),
                  DBException);
}

TEST(PersistentStatsConfigUpdate, AppliesDelta) {
    // Setting a sample size on a collection with no configuration creates one.
    auto result = applyPersistentStatsConfigUpdate(
        boost::none, parsePersistentStatsConfigUpdate(fromjson("{samples: {sampleSize: 1000}}")));
    ASSERT_EQUALS(*result->getSamples()->getSampleSize(), 1000);
    ASSERT_FALSE(result->getSamples()->getJoinSampleSize());

    result = applyPersistentStatsConfigUpdate(
        boost::none,
        parsePersistentStatsConfigUpdate(fromjson("{samples: {joinSampleSize: 2000}}")));
    ASSERT_FALSE(result->getSamples()->getSampleSize());
    ASSERT_EQUALS(*result->getSamples()->getJoinSampleSize(), 2000);

    // Sample sizes the update does not mention are preserved.
    result = applyPersistentStatsConfigUpdate(
        makeConfig(1000, 2000),
        parsePersistentStatsConfigUpdate(fromjson("{samples: {joinSampleSize: 500}}")));
    ASSERT_EQUALS(*result->getSamples()->getSampleSize(), 1000);
    ASSERT_EQUALS(*result->getSamples()->getJoinSampleSize(), 500);

    // Removing a sample size preserves the other one.
    result = applyPersistentStatsConfigUpdate(
        makeConfig(1000, 2000),
        parsePersistentStatsConfigUpdate(fromjson("{samples: {sampleSize: null}}")));
    ASSERT_FALSE(result->getSamples()->getSampleSize());
    ASSERT_EQUALS(*result->getSamples()->getJoinSampleSize(), 2000);

    // Removing a sample size that is not set is a no-op.
    result = applyPersistentStatsConfigUpdate(
        makeConfig(boost::none, 2000),
        parsePersistentStatsConfigUpdate(fromjson("{samples: {sampleSize: null}}")));
    ASSERT_FALSE(result->getSamples()->getSampleSize());
    ASSERT_EQUALS(*result->getSamples()->getJoinSampleSize(), 2000);

    // Removing the last sample size leaves nothing configured.
    result = applyPersistentStatsConfigUpdate(
        makeConfig(1000, 2000),
        parsePersistentStatsConfigUpdate(
            fromjson("{samples: {sampleSize: null, joinSampleSize: null}}")));
    ASSERT_FALSE(result);
}

}  // namespace mongo

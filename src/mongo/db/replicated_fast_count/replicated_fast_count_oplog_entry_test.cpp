// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/replicated_fast_count/init_replicated_fast_count_oplog_entry_gen.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"

#include <string_view>

namespace mongo {
namespace {

BSONObj makeInitReplicatedFastCountO2(std::string_view metadataIdent,
                                      int metadataKeyFormat,
                                      std::string_view timestampsIdent,
                                      int timestampsKeyFormat) {
    return BSON("fastCountMetadataStoreIdent"
                << metadataIdent << "fastCountMetadataStoreKeyFormat" << metadataKeyFormat
                << "fastCountMetadataStoreTimestampsIdent" << timestampsIdent
                << "fastCountMetadataStoreTimestampsKeyFormat" << timestampsKeyFormat);
}

TEST(InitReplicatedFastCountO2Test, ParseSuccess) {
    auto obj = makeInitReplicatedFastCountO2("metadata_ident", 1, "timestamps_ident", 2);
    auto parsed = InitReplicatedFastCountO2::parse(obj);

    EXPECT_EQ(parsed.getFastCountMetadataStoreIdent(), "metadata_ident");
    EXPECT_EQ(parsed.getFastCountMetadataStoreKeyFormat(), 1);
    EXPECT_EQ(parsed.getFastCountMetadataStoreTimestampsIdent(), "timestamps_ident");
    EXPECT_EQ(parsed.getFastCountMetadataStoreTimestampsKeyFormat(), 2);
}

TEST(InitReplicatedFastCountO2Test, ParseMissingMetadataIdent) {
    auto obj =
        BSON("fastCountMetadataStoreKeyFormat" << 1 << "fastCountMetadataStoreTimestampsIdent"
                                               << "ts_ident"
                                               << "fastCountMetadataStoreTimestampsKeyFormat" << 1);
    ASSERT_THROWS(InitReplicatedFastCountO2::parse(obj), DBException);
}

TEST(InitReplicatedFastCountO2Test, ParseMissingTimestampsIdent) {
    auto obj =
        BSON("fastCountMetadataStoreIdent" << "md_ident"
                                           << "fastCountMetadataStoreKeyFormat" << 1
                                           << "fastCountMetadataStoreTimestampsKeyFormat" << 1);
    ASSERT_THROWS(InitReplicatedFastCountO2::parse(obj), DBException);
}

TEST(InitReplicatedFastCountO2Test, ParseMissingMetadataKeyFormat) {
    auto obj =
        BSON("fastCountMetadataStoreIdent" << "md_ident"
                                           << "fastCountMetadataStoreTimestampsIdent"
                                           << "ts_ident"
                                           << "fastCountMetadataStoreTimestampsKeyFormat" << 1);
    ASSERT_THROWS(InitReplicatedFastCountO2::parse(obj), DBException);
}

TEST(InitReplicatedFastCountO2Test, ParseMissingTimestampsKeyFormat) {
    auto obj = BSON("fastCountMetadataStoreIdent" << "md_ident"
                                                  << "fastCountMetadataStoreKeyFormat" << 1
                                                  << "fastCountMetadataStoreTimestampsIdent"
                                                  << "ts_ident");
    ASSERT_THROWS(InitReplicatedFastCountO2::parse(obj), DBException);
}

TEST(InitReplicatedFastCountO2Test, RoundTrip) {
    auto obj = makeInitReplicatedFastCountO2("md_ident", 0, "ts_ident", 1);
    auto parsed = InitReplicatedFastCountO2::parse(obj);

    BSONObjBuilder bob;
    parsed.serialize(&bob);
    auto serialized = bob.obj();

    auto reparsed = InitReplicatedFastCountO2::parse(serialized);
    EXPECT_EQ(reparsed.getFastCountMetadataStoreIdent(), "md_ident");
    EXPECT_EQ(reparsed.getFastCountMetadataStoreKeyFormat(), 0);
    EXPECT_EQ(reparsed.getFastCountMetadataStoreTimestampsIdent(), "ts_ident");
    EXPECT_EQ(reparsed.getFastCountMetadataStoreTimestampsKeyFormat(), 1);
}

}  // namespace
}  // namespace mongo

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
#include "mongo/db/replicated_fast_count/init_replicated_fast_count_oplog_entry_gen.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"

namespace mongo {
namespace {

BSONObj makeInitReplicatedFastCountO2(StringData metadataIdent,
                                      int metadataKeyFormat,
                                      StringData timestampsIdent,
                                      int timestampsKeyFormat) {
    return BSON("fastCountMetadataStoreIdent"
                << metadataIdent << "fastCountMetadataStoreKeyFormat" << metadataKeyFormat
                << "fastCountMetadataStoreTimestampsIdent" << timestampsIdent
                << "fastCountMetadataStoreTimestampsKeyFormat" << timestampsKeyFormat);
}

TEST(InitReplicatedFastCountO2Test, ParseSuccess) {
    auto obj = makeInitReplicatedFastCountO2("metadata_ident", 1, "timestamps_ident", 2);
    auto parsed = InitReplicatedFastCountO2::parse(obj);

    ASSERT_EQ(parsed.getFastCountMetadataStoreIdent(), "metadata_ident");
    ASSERT_EQ(parsed.getFastCountMetadataStoreKeyFormat(), 1);
    ASSERT_EQ(parsed.getFastCountMetadataStoreTimestampsIdent(), "timestamps_ident");
    ASSERT_EQ(parsed.getFastCountMetadataStoreTimestampsKeyFormat(), 2);
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
    ASSERT_EQ(reparsed.getFastCountMetadataStoreIdent(), "md_ident");
    ASSERT_EQ(reparsed.getFastCountMetadataStoreKeyFormat(), 0);
    ASSERT_EQ(reparsed.getFastCountMetadataStoreTimestampsIdent(), "ts_ident");
    ASSERT_EQ(reparsed.getFastCountMetadataStoreTimestampsKeyFormat(), 1);
}

}  // namespace
}  // namespace mongo

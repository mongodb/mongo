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

#include <limits>

#include "mongo/s/chunk_version.h"
#include "mongo/s/chunk_version_gen.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(ChunkVersionTest, Parsing60Format) {
    const Timestamp majorMinor = Timestamp(Seconds(1), 2);
    const OID epoch = OID::gen();
    const Timestamp timestamp(42);

    ChunkVersion chunkVersion(1, 2, epoch, timestamp);
    // Check future format in fromBSONLegacyOrNewerFormat.
    ChunkVersion futureFormatChunkVersion = ChunkVersion::fromBSONLegacyOrNewerFormat(
        BSON("chunkVersion" << BSON(ChunkVersion60Format::kVersionFieldName
                                    << majorMinor << ChunkVersion60Format::kEpochFieldName << epoch
                                    << ChunkVersion60Format::kTimestampFieldName << timestamp)),
        "chunkVersion");

    ASSERT_EQ(1, futureFormatChunkVersion.majorVersion());
    ASSERT_EQ(2, futureFormatChunkVersion.minorVersion());
    ASSERT_EQ(epoch, futureFormatChunkVersion.epoch());
    ASSERT_EQ(timestamp, futureFormatChunkVersion.getTimestamp());

    // Check future format in fromBSONPositionalOrNewerFormat.
    ChunkVersion futureFormatChunkVersion2 = ChunkVersion::fromBSONPositionalOrNewerFormat(
        BSON("chunkVersion" << BSON(ChunkVersion60Format::kVersionFieldName
                                    << majorMinor << ChunkVersion60Format::kEpochFieldName << epoch
                                    << ChunkVersion60Format::kTimestampFieldName
                                    << timestamp))["chunkVersion"]);

    ASSERT_EQ(1, futureFormatChunkVersion2.majorVersion());
    ASSERT_EQ(2, futureFormatChunkVersion2.minorVersion());
    ASSERT_EQ(epoch, futureFormatChunkVersion2.epoch());
    ASSERT_EQ(timestamp, futureFormatChunkVersion2.getTimestamp());
}

TEST(ChunkVersionTest, ToFromBSONRoundtrip) {
    ChunkVersion version(1, 2, OID::gen(), Timestamp(42));
    const auto roundTripVersion = ChunkVersion::fromBSONPositionalOrNewerFormat([&] {
        BSONObjBuilder builder;
        version.serializeToBSON("testVersionField", &builder);
        return builder.obj();
    }()["testVersionField"]);

    ASSERT_EQ(version, roundTripVersion);
}

TEST(ChunkVersionTest, ToFromBSONLegacyRoundtrip) {
    ChunkVersion version(1, 2, OID::gen(), Timestamp(42));
    const auto roundTripVersion = ChunkVersion::fromBSONLegacyOrNewerFormat(
        [&] {
            BSONObjBuilder builder;
            version.appendLegacyWithField(&builder, "testVersionField");
            return builder.obj();
        }(),
        "testVersionField");

    ASSERT_EQ(version, roundTripVersion);
}

TEST(ChunkVersionTest, FromBSONMissingTimestamp) {
    ASSERT_THROWS_CODE(ChunkVersion::fromBSONPositionalOrNewerFormat(
                           BSON("testVersionField" << BSON_ARRAY(
                                    Timestamp(Seconds(2), 3) << OID::gen()))["testVersionField"]),
                       DBException,
                       ErrorCodes::TypeMismatch);
}

TEST(ChunkVersionTest, FromBSON) {
    const OID oid = OID::gen();
    const Timestamp timestamp(42);
    ChunkVersion chunkVersionComplete = ChunkVersion::fromBSONPositionalOrNewerFormat(
        BSON("testVersionField" << BSON_ARRAY(Timestamp(Seconds(2), 3)
                                              << oid << timestamp))["testVersionField"]);

    ASSERT(chunkVersionComplete.epoch().isSet());
    ASSERT_EQ(oid, chunkVersionComplete.epoch());
    ASSERT_EQ(2u, chunkVersionComplete.majorVersion());
    ASSERT_EQ(3u, chunkVersionComplete.minorVersion());
    ASSERT_EQ(timestamp, chunkVersionComplete.getTimestamp());
}

TEST(ChunkVersionTest, FromBSONMissingEpoch) {
    ASSERT_THROWS_CODE(
        ChunkVersion::fromBSONPositionalOrNewerFormat(
            BSON("testVersionField" << BSON_ARRAY(Timestamp(Seconds(2), 3)))["testVersionField"]),
        DBException,
        ErrorCodes::TypeMismatch);
}

TEST(ChunkVersionTest, FromBSONMissingMajorAndMinor) {
    ASSERT_THROWS_CODE(ChunkVersion::fromBSONPositionalOrNewerFormat(
                           BSON("testVersionField" << BSON_ARRAY(OID::gen()))["testVersionField"]),
                       DBException,
                       ErrorCodes::TypeMismatch);
}

TEST(ChunkVersionTest, FromBSONLegacy_WithTimestamp_WithEpoch) {
    const OID oid = OID::gen();
    ChunkVersion chunkVersionComplete = ChunkVersion::fromBSONLegacyOrNewerFormat(
        BSON("lastmod" << Timestamp(Seconds(2), 3) << "lastmodEpoch" << oid << "lastmodTimestamp"
                       << Timestamp(42)),
        "lastmod");
    ASSERT_EQ(Timestamp(42), chunkVersionComplete.getTimestamp());
    ASSERT_EQ(oid, chunkVersionComplete.epoch());
    ASSERT_EQ(2u, chunkVersionComplete.majorVersion());
    ASSERT_EQ(3u, chunkVersionComplete.minorVersion());
}

TEST(ChunkVersionTest, FromBSONLegacy_NoTimestamp_WithUnshardedEpoch) {
    ChunkVersion chunkVersion = ChunkVersion::fromBSONLegacyOrNewerFormat(
        BSON("lastmod" << Timestamp() << "lastmodEpoch" << ChunkVersion::UNSHARDED().epoch()),
        "lastmod");
    ASSERT_EQ(ChunkVersion::UNSHARDED().getTimestamp(), chunkVersion.getTimestamp());
    ASSERT_EQ(ChunkVersion::UNSHARDED().epoch(), chunkVersion.epoch());
    ASSERT_EQ(0u, chunkVersion.majorVersion());
    ASSERT_EQ(0u, chunkVersion.minorVersion());
}

TEST(ChunkVersionTest, FromBSONLegacy_NoTimestamp_WithIgnoredEpoch) {
    ChunkVersion chunkVersion = ChunkVersion::fromBSONLegacyOrNewerFormat(
        BSON("lastmod" << Timestamp() << "lastmodEpoch" << ChunkVersion::IGNORED().epoch()),
        "lastmod");
    ASSERT_EQ(ChunkVersion::IGNORED().getTimestamp(), chunkVersion.getTimestamp());
    ASSERT_EQ(ChunkVersion::IGNORED().epoch(), chunkVersion.epoch());
    ASSERT_EQ(0u, chunkVersion.majorVersion());
    ASSERT_EQ(0u, chunkVersion.minorVersion());
}

TEST(ChunkVersionTest, FromBSONLegacy_NoTimestamp_WithShardedEpoch_Throws) {
    ASSERT_THROWS(
        ChunkVersion::fromBSONLegacyOrNewerFormat(
            BSON("lastmod" << Timestamp(Seconds(3), 4) << "lastmodEpoch" << OID::gen()), "lastmod"),
        DBException);
}

TEST(ChunkVersionTest, FromBSONLegacy_WithTimestamp_NoEpoch_Throws) {
    ASSERT_THROWS(
        ChunkVersion::fromBSONLegacyOrNewerFormat(
            BSON("lastmod" << Timestamp(Seconds(3), 4) << "lastmodTimestamp" << Timestamp(42)),
            "lastmod"),
        DBException);
}

TEST(ChunkVersionTest, FromBSONLegacy_NoTimestamp_NoEpoch_Throws) {
    ChunkVersion chunkVersion = ChunkVersion::fromBSONLegacyOrNewerFormat(
        BSON("lastmod" << Timestamp(Seconds(3), 4)), "lastmod");
    ASSERT_EQ(Timestamp(), chunkVersion.getTimestamp());
    ASSERT(!chunkVersion.epoch().isSet());
    ASSERT_EQ(3u, chunkVersion.majorVersion());
    ASSERT_EQ(4u, chunkVersion.minorVersion());
}

TEST(ChunkVersionTest, EqualityOperators) {
    OID epoch = OID::gen();
    Timestamp timestamp = Timestamp(1);

    ASSERT_EQ(ChunkVersion(3, 1, epoch, Timestamp(1, 1)),
              ChunkVersion(3, 1, epoch, Timestamp(1, 1)));
    ASSERT_EQ(ChunkVersion(3, 1, OID(), timestamp), ChunkVersion(3, 1, OID(), timestamp));

    ASSERT_NE(ChunkVersion(3, 1, epoch, timestamp), ChunkVersion(3, 1, OID(), Timestamp(1, 1)));
    ASSERT_NE(ChunkVersion(3, 1, OID(), Timestamp(1, 1)), ChunkVersion(3, 1, epoch, timestamp));
    ASSERT_NE(ChunkVersion(4, 2, epoch, timestamp), ChunkVersion(4, 1, epoch, timestamp));
}

TEST(ChunkVersionTest, OlderThan) {
    OID epoch = OID::gen();
    Timestamp timestamp(1);
    Timestamp newerTimestamp(2);

    ASSERT(ChunkVersion(3, 1, epoch, timestamp).isOlderThan(ChunkVersion(4, 1, epoch, timestamp)));
    ASSERT(!ChunkVersion(4, 1, epoch, timestamp).isOlderThan(ChunkVersion(3, 1, epoch, timestamp)));

    ASSERT(ChunkVersion(3, 1, epoch, timestamp).isOlderThan(ChunkVersion(3, 2, epoch, timestamp)));
    ASSERT(!ChunkVersion(3, 2, epoch, timestamp).isOlderThan(ChunkVersion(3, 1, epoch, timestamp)));

    ASSERT(ChunkVersion(3, 1, epoch, timestamp)
               .isOlderThan(ChunkVersion(3, 1, OID::gen(), newerTimestamp)));
    ASSERT(!ChunkVersion(3, 1, epoch, newerTimestamp)
                .isOlderThan(ChunkVersion(3, 1, OID::gen(), timestamp)));

    ASSERT(!ChunkVersion::UNSHARDED().isOlderThan(ChunkVersion(3, 1, epoch, timestamp)));
    ASSERT(!ChunkVersion(3, 1, epoch, timestamp).isOlderThan(ChunkVersion::UNSHARDED()));
}

TEST(ChunkVersionTest, CreateWithLargeValues) {
    const uint32_t majorVersion = std::numeric_limits<uint32_t>::max();
    const uint32_t minorVersion = std::numeric_limits<uint32_t>::max();
    const auto epoch = OID::gen();

    ChunkVersion version(majorVersion, minorVersion, epoch, Timestamp(1, 1));
    ASSERT_EQ(majorVersion, version.majorVersion());
    ASSERT_EQ(minorVersion, version.minorVersion());
    ASSERT_EQ(epoch, version.epoch());
    ASSERT_EQ(Timestamp(1, 1), version.getTimestamp());
}

TEST(ChunkVersionTest, ThrowsErrorIfOverflowIsAttemptedForMajorVersion) {
    const uint32_t majorVersion = std::numeric_limits<uint32_t>::max();
    const uint32_t minorVersion = 0;
    const auto epoch = OID::gen();

    ChunkVersion version(majorVersion, minorVersion, epoch, Timestamp(1, 1));
    ASSERT_EQ(majorVersion, version.majorVersion());
    ASSERT_EQ(minorVersion, version.minorVersion());
    ASSERT_EQ(epoch, version.epoch());

    ASSERT_THROWS_CODE(version.incMajor(), DBException, 31180);
}

TEST(ChunkVersionTest, ThrowsErrorIfOverflowIsAttemptedForMinorVersion) {
    const uint32_t majorVersion = 0;
    const uint32_t minorVersion = std::numeric_limits<uint32_t>::max();
    const auto epoch = OID::gen();

    ChunkVersion version(majorVersion, minorVersion, epoch, Timestamp(1, 1));
    ASSERT_EQ(majorVersion, version.majorVersion());
    ASSERT_EQ(minorVersion, version.minorVersion());
    ASSERT_EQ(epoch, version.epoch());

    ASSERT_THROWS_CODE(version.incMinor(), DBException, 31181);
}

}  // namespace
}  // namespace mongo

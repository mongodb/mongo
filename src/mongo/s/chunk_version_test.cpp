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

#include "mongo/platform/basic.h"

#include <limits>

#include "mongo/s/chunk_version.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using unittest::assertGet;

TEST(ChunkVersionParsing, ToFromBSONRoundtrip) {
    ChunkVersion version(1, 2, OID::gen());
    const auto roundTripVersion = assertGet(ChunkVersion::parseWithField(
        [&] {
            BSONObjBuilder builder;
            version.appendWithField(&builder, "testVersionField");
            return builder.obj();
        }(),
        "testVersionField"));

    ASSERT_EQ(version, roundTripVersion);
}

TEST(ChunkVersionParsing, ToFromBSONLegacyRoundtrip) {
    ChunkVersion version(1, 2, OID::gen());
    const auto roundTripVersion = assertGet(ChunkVersion::parseLegacyWithField(
        [&] {
            BSONObjBuilder builder;
            version.appendLegacyWithField(&builder, "testVersionField");
            return builder.obj();
        }(),
        "testVersionField"));

    ASSERT_EQ(version, roundTripVersion);
}

TEST(ChunkVersionParsing, FromBSON) {
    const OID oid = OID::gen();
    ChunkVersion chunkVersionComplete = assertGet(ChunkVersion::parseWithField(
        BSON("testVersionField" << BSON_ARRAY(Timestamp(Seconds(2), 3) << oid)),
        "testVersionField"));

    ASSERT(chunkVersionComplete.epoch().isSet());
    ASSERT_EQ(oid, chunkVersionComplete.epoch());
    ASSERT_EQ(2u, chunkVersionComplete.majorVersion());
    ASSERT_EQ(3u, chunkVersionComplete.minorVersion());
}

TEST(ChunkVersionParsing, FromBSONMissingEpoch) {
    ASSERT_THROWS_CODE(
        uassertStatusOK(ChunkVersion::parseWithField(
            BSON("testVersionField" << BSON_ARRAY(Timestamp(Seconds(2), 3))), "testVersionField")),
        AssertionException,
        ErrorCodes::TypeMismatch);
}

TEST(ChunkVersionParsing, FromBSONMissingTimestamp) {
    const OID oid = OID::gen();
    ASSERT_THROWS_CODE(uassertStatusOK(ChunkVersion::parseWithField(BSON("testVersionField" << oid),
                                                                    "testVersionField")),
                       AssertionException,
                       ErrorCodes::TypeMismatch);
}

TEST(ChunkVersionParsing, FromBSONLegacy) {
    const OID oid = OID::gen();
    ChunkVersion chunkVersionComplete = assertGet(ChunkVersion::parseLegacyWithField(
        BSON("lastmod" << Timestamp(Seconds(2), 3) << "lastmodEpoch" << oid), "lastmod"));

    ASSERT(chunkVersionComplete.epoch().isSet());
    ASSERT_EQ(oid, chunkVersionComplete.epoch());
    ASSERT_EQ(2u, chunkVersionComplete.majorVersion());
    ASSERT_EQ(3u, chunkVersionComplete.minorVersion());
}

TEST(ChunkVersionParsing, FromBSONLegacyEpochIsOptional) {
    ChunkVersion chunkVersionNoEpoch = assertGet(
        ChunkVersion::parseLegacyWithField(BSON("lastmod" << Timestamp(Seconds(3), 4)), "lastmod"));

    ASSERT(!chunkVersionNoEpoch.epoch().isSet());
    ASSERT_EQ(3u, chunkVersionNoEpoch.majorVersion());
    ASSERT_EQ(4u, chunkVersionNoEpoch.minorVersion());
}

TEST(ChunkVersionComparison, EqualityOperators) {
    OID epoch = OID::gen();

    ASSERT_EQ(ChunkVersion(3, 1, epoch), ChunkVersion(3, 1, epoch));
    ASSERT_EQ(ChunkVersion(3, 1, OID()), ChunkVersion(3, 1, OID()));

    ASSERT_NE(ChunkVersion(3, 1, epoch), ChunkVersion(3, 1, OID()));
    ASSERT_NE(ChunkVersion(3, 1, OID()), ChunkVersion(3, 1, epoch));
    ASSERT_NE(ChunkVersion(4, 2, epoch), ChunkVersion(4, 1, epoch));
}

TEST(ChunkVersionComparison, OlderThan) {
    OID epoch = OID::gen();

    ASSERT(ChunkVersion(3, 1, epoch).isOlderThan(ChunkVersion(4, 1, epoch)));
    ASSERT(!ChunkVersion(4, 1, epoch).isOlderThan(ChunkVersion(3, 1, epoch)));

    ASSERT(ChunkVersion(3, 1, epoch).isOlderThan(ChunkVersion(3, 2, epoch)));
    ASSERT(!ChunkVersion(3, 2, epoch).isOlderThan(ChunkVersion(3, 1, epoch)));

    ASSERT(!ChunkVersion(3, 1, epoch).isOlderThan(ChunkVersion(4, 1, OID())));
    ASSERT(!ChunkVersion(4, 1, OID()).isOlderThan(ChunkVersion(3, 1, epoch)));

    ASSERT(ChunkVersion(3, 2, epoch).isOlderThan(ChunkVersion(4, 1, epoch)));

    ASSERT(!ChunkVersion(3, 1, epoch).isOlderThan(ChunkVersion(3, 1, epoch)));
}

TEST(ChunkVersionConstruction, CreateWithLargeValues) {
    const auto minorVersion = std::numeric_limits<uint32_t>::max();
    const uint32_t majorVersion = 1 << 24;
    const auto epoch = OID::gen();

    ChunkVersion version(majorVersion, minorVersion, epoch);
    ASSERT_EQ(majorVersion, version.majorVersion());
    ASSERT_EQ(minorVersion, version.minorVersion());
    ASSERT_EQ(epoch, version.epoch());
}

TEST(ChunkVersionManipulation, ThrowsErrorIfOverflowIsAttemptedForMajorVersion) {
    const uint32_t minorVersion = 0;
    const uint32_t majorVersion = std::numeric_limits<uint32_t>::max();
    const auto epoch = OID::gen();

    ChunkVersion version(majorVersion, minorVersion, epoch);
    ASSERT_EQ(majorVersion, version.majorVersion());
    ASSERT_EQ(minorVersion, version.minorVersion());
    ASSERT_EQ(epoch, version.epoch());

    ASSERT_THROWS_CODE(version.incMajor(), DBException, 31180);
}

TEST(ChunkVersionManipulation, ThrowsErrorIfOverflowIsAttemptedForMinorVersion) {
    const uint32_t minorVersion = std::numeric_limits<uint32_t>::max();
    const uint32_t majorVersion = 0;
    const auto epoch = OID::gen();

    ChunkVersion version(majorVersion, minorVersion, epoch);
    ASSERT_EQ(majorVersion, version.majorVersion());
    ASSERT_EQ(minorVersion, version.minorVersion());
    ASSERT_EQ(epoch, version.epoch());

    ASSERT_THROWS_CODE(version.incMinor(), DBException, 31181);
}
}  // namespace
}  // namespace mongo

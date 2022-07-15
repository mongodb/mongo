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

/** Unit tests for RecordId. */

#include "mongo/db/record_id.h"

#include "mongo/db/record_id_helpers.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/debug_util.h"

namespace mongo {
namespace {

TEST(RecordId, HashEqual) {
    RecordId locA(1, 2);
    RecordId locB;
    locB = locA;
    ASSERT_EQUALS(locA, locB);
    RecordId::Hasher hasher;
    ASSERT_EQUALS(hasher(locA), hasher(locB));
}

TEST(RecordId, HashEqualOid) {
    RecordId locA(record_id_helpers::keyForOID(OID::gen()));
    RecordId locB;
    locB = locA;
    ASSERT_EQUALS(locA, locB);
    RecordId::Hasher hasher;
    ASSERT_EQUALS(hasher(locA), hasher(locB));
}

TEST(RecordId, HashNotEqual) {
    RecordId original(1, 2);
    RecordId diffFile(10, 2);
    RecordId diffOfs(1, 20);
    RecordId diffBoth(10, 20);
    RecordId reversed(2, 1);
    ASSERT_NOT_EQUALS(original, diffFile);
    ASSERT_NOT_EQUALS(original, diffOfs);
    ASSERT_NOT_EQUALS(original, diffBoth);
    ASSERT_NOT_EQUALS(original, reversed);

    // Unequal DiskLocs need not produce unequal hashes.  But unequal hashes are likely, and
    // assumed here for sanity checking of the custom hash implementation.
    RecordId::Hasher hasher;
    ASSERT_NOT_EQUALS(hasher(original), hasher(diffFile));
    ASSERT_NOT_EQUALS(hasher(original), hasher(diffOfs));
    ASSERT_NOT_EQUALS(hasher(original), hasher(diffBoth));
    ASSERT_NOT_EQUALS(hasher(original), hasher(reversed));
}

TEST(RecordId, HashNotEqualOid) {
    RecordId loc1(record_id_helpers::keyForOID(OID::gen()));
    RecordId loc2(record_id_helpers::keyForOID(OID::gen()));
    RecordId loc3(record_id_helpers::keyForOID(OID::gen()));
    ASSERT_NOT_EQUALS(loc1, loc2);
    ASSERT_NOT_EQUALS(loc1, loc3);
    ASSERT_NOT_EQUALS(loc2, loc3);

    // Unequal DiskLocs need not produce unequal hashes.  But unequal hashes are likely, and
    // assumed here for sanity checking of the custom hash implementation.
    RecordId::Hasher hasher;
    ASSERT_NOT_EQUALS(hasher(loc1), hasher(loc2));
    ASSERT_NOT_EQUALS(hasher(loc1), hasher(loc3));
    ASSERT_NOT_EQUALS(hasher(loc2), hasher(loc3));
}

TEST(RecordId, KeyStringTest) {
    RecordId ridNull;
    ASSERT(ridNull.isNull());
    ASSERT(!ridNull.isValid());

    RecordId null2;
    ASSERT(null2 == ridNull);

    OID oid1 = OID::gen();
    RecordId rid1(record_id_helpers::keyForOID(oid1));
    ASSERT(rid1.isValid());
    auto obj = record_id_helpers::toBSONAs(rid1, "");
    ASSERT_EQ(oid1, obj.firstElement().OID());
    ASSERT_GT(rid1, ridNull);
    ASSERT_LT(ridNull, rid1);
}

TEST(RecordId, NullTest) {
    // The int64 format should be considered null if its value is 0. Likewise, the value should be
    // interpreted as int64_t(0) if it is null.
    RecordId rid0(0);
    ASSERT(rid0.isNull());

    RecordId nullRid;
    ASSERT(nullRid.isNull());
    ASSERT_EQ(0, nullRid.getLong());
    ASSERT_NE(rid0, nullRid);
}

TEST(RecordId, RidTestCompare) {
    RecordId ridNull;
    RecordId rid1(1);

    ASSERT_GT(rid1, ridNull);
    ASSERT_LT(ridNull, rid1);
    ASSERT_NE(ridNull, rid1);

    RecordId rid0(0);
    ASSERT_GT(rid0, ridNull);
    ASSERT_LT(ridNull, rid0);
    ASSERT_NE(ridNull, rid0);
}

TEST(RecordId, OidTestCompare) {
    RecordId ridNull;
    RecordId rid0 = record_id_helpers::keyForOID(OID::createFromString("000000000000000000000000"));
    ASSERT_GT(rid0, ridNull);

    RecordId rid1 = record_id_helpers::keyForOID(OID::createFromString("000000000000000000000001"));
    ASSERT_GT(rid1, rid0);
    RecordId oidMin = record_id_helpers::keyForOID(OID());
    ASSERT_EQ(oidMin, rid0);
    ASSERT_GT(oidMin, ridNull);

    RecordId rid2 = record_id_helpers::keyForOID(OID::createFromString("000000000000000000000002"));
    ASSERT_GT(rid2, rid1);
    RecordId rid3 = record_id_helpers::keyForOID(OID::createFromString("ffffffffffffffffffffffff"));
    ASSERT_GT(rid3, rid2);
    ASSERT_GT(rid3, rid0);

    RecordId oidMax = record_id_helpers::keyForOID(OID::max());
    ASSERT_EQ(oidMax, rid3);
    ASSERT_GT(oidMax, rid0);
}

TEST(RecordId, ReservationsLong) {
    // It's important that reserved IDs like this never change.
    RecordId ridReserved(RecordId::kMaxRepr - (1024 * 1024));
    ASSERT_EQ(ridReserved,
              record_id_helpers::reservedIdFor(
                  record_id_helpers::ReservationId::kWildcardMultikeyMetadataId, KeyFormat::Long));
    ASSERT(record_id_helpers::isReserved(ridReserved));
    ASSERT(ridReserved.isValid());

    // Create a new RecordId in the reserved range and ensure it is considered reserved and unique.
    RecordId inReservedRange(RecordId::kMaxRepr - 1);
    ASSERT(record_id_helpers::isReserved(inReservedRange));
    ASSERT(inReservedRange.isValid());
    ASSERT_NE(inReservedRange,
              record_id_helpers::reservedIdFor(
                  record_id_helpers::ReservationId::kWildcardMultikeyMetadataId, KeyFormat::Long));
}

TEST(RecordId, ReservationsStr) {
    // It's important that reserved IDs like this never change.
    constexpr char buf[] = {static_cast<char>(0xFF), 0};
    RecordId ridReserved(buf, sizeof(buf));
    ASSERT_EQ(
        ridReserved,
        record_id_helpers::reservedIdFor(
            record_id_helpers::ReservationId::kWildcardMultikeyMetadataId, KeyFormat::String));
    ASSERT(record_id_helpers::isReserved(ridReserved));
    ASSERT(ridReserved.isValid());

    // Create a new RecordId in the reserved range and ensure it is considered reserved and unique.
    constexpr char buf2[] = {static_cast<char>(0xFF), static_cast<char>(0xFF)};
    RecordId inReservedRange(buf2, sizeof(buf2));
    ASSERT(record_id_helpers::isReserved(inReservedRange));
    ASSERT(inReservedRange.isValid());
    ASSERT_NE(
        inReservedRange,
        record_id_helpers::reservedIdFor(
            record_id_helpers::ReservationId::kWildcardMultikeyMetadataId, KeyFormat::String));
}

TEST(RecordId, RoundTripSerialize) {
    {
        RecordId id(1);
        BSONObjBuilder builder;
        id.serializeToken("rid", &builder);
        BSONObj obj = builder.done();
        ASSERT_EQ(id, RecordId::deserializeToken(obj["rid"]));
    }

    {
        RecordId id(4611686018427387904);
        BSONObjBuilder builder;
        id.serializeToken("rid", &builder);
        BSONObj obj = builder.done();
        ASSERT_EQ(id, RecordId::deserializeToken(obj["rid"]));
    }

    {
        RecordId id;
        BSONObjBuilder builder;
        id.serializeToken("rid", &builder);
        BSONObj obj = builder.done();
        ASSERT_EQ(id, RecordId::deserializeToken(obj["rid"]));
    }

    {
        RecordId id(record_id_helpers::keyForOID(OID::gen()));
        BSONObjBuilder builder;
        id.serializeToken("rid", &builder);
        BSONObj obj = builder.done();
        ASSERT_EQ(id, RecordId::deserializeToken(obj["rid"]));
    }

    {
        char buf[1024] = {'x'};
        RecordId id(buf, sizeof(buf));
        BSONObjBuilder builder;
        id.serializeToken("rid", &builder);
        BSONObj obj = builder.done();
        ASSERT_EQ(id, RecordId::deserializeToken(obj["rid"]));
    }

    {
        BSONObjBuilder builder;
        builder.append("rid", OID::gen());
        BSONObj obj = builder.done();
        ASSERT_THROWS_CODE(
            RecordId::deserializeToken(obj["rid"]), DBException, ErrorCodes::BadValue);
    }
}

TEST(RecordId, RoundTripSerializeBinary) {
    {
        RecordId id(1);
        BufBuilder builder;
        id.serializeToken(builder);
        BufReader reader(builder.buf(), builder.len());
        ASSERT_EQ(id, RecordId::deserializeToken(reader));
    }

    {
        RecordId id(4611686018427387904);
        BufBuilder builder;
        id.serializeToken(builder);
        BufReader reader(builder.buf(), builder.len());
        ASSERT_EQ(id, RecordId::deserializeToken(reader));
    }

    {
        RecordId id;
        BufBuilder builder;
        id.serializeToken(builder);
        BufReader reader(builder.buf(), builder.len());
        ASSERT_EQ(id, RecordId::deserializeToken(reader));
    }

    {
        RecordId id(record_id_helpers::keyForOID(OID::gen()));
        BufBuilder builder;
        id.serializeToken(builder);
        BufReader reader(builder.buf(), builder.len());
        ASSERT_EQ(id, RecordId::deserializeToken(reader));
    }

    {
        char buf[1024] = {'x'};
        RecordId id(buf, sizeof(buf));
        BufBuilder builder;
        id.serializeToken(builder);
        BufReader reader(builder.buf(), builder.len());
        ASSERT_EQ(id, RecordId::deserializeToken(reader));
    }
}
TEST(RecordId, RecordIdBigStr) {
    char buf[1024] = {'x'};

    // This string should be just enough to qualify for the small string optimization.
    RecordId smallId(buf, RecordId::kSmallStrMaxSize);
    ASSERT_TRUE(smallId.isInlineAllocated_forTest());
    ASSERT_EQ(smallId.getStr().size(), RecordId::kSmallStrMaxSize);
    ASSERT_EQ(sizeof(RecordId), smallId.memUsage());

    // At a certain size RecordId strings should expand beyond the size of the struct and start
    // using a heap buffer.
    RecordId bigId(buf, RecordId::kSmallStrMaxSize + 1);
    ASSERT_FALSE(bigId.isInlineAllocated_forTest());
    ASSERT_EQ(bigId.getStr().size(), RecordId::kSmallStrMaxSize + 1);
    ASSERT_EQ(sizeof(RecordId) + bigId.getStr().size(), bigId.memUsage());
    ASSERT_GT(bigId, smallId);
    ASSERT_LT(smallId, bigId);

    // Once copied, this RecordId should not be sharing its contents.
    RecordId bigCopy = bigId;
    ASSERT_FALSE(bigId.isInlineAllocated_forTest());
    ASSERT_FALSE(bigCopy.isInlineAllocated_forTest());
    ASSERT_NE(bigId.getStr().rawData(), bigCopy.getStr().rawData());
    ASSERT_EQ(bigId.getStr().size(), bigCopy.getStr().size());
    ASSERT_EQ(sizeof(RecordId) + bigId.getStr().size(), bigCopy.memUsage());

    ASSERT_EQ(bigCopy, bigId);
    ASSERT_EQ(bigCopy.toString(), bigId.toString());
    ASSERT_EQ(bigCopy.isValid(), bigId.isValid());
    ASSERT_EQ(bigCopy.isStr(), bigId.isStr());

    // Ensure there is a limit and it is enforced.
    std::string huge(RecordId::kBigStrMaxSize + 1, 'x');
    ASSERT_THROWS_CODE(RecordId(huge.c_str(), huge.size()), AssertionException, 5894900);
}

// RecordIds of different formats may not be compared.
DEATH_TEST(RecordId, UnsafeComparison, "Invariant failure") {
    if (kDebugBuild) {
        RecordId rid1(1);
        RecordId rid2 =
            record_id_helpers::keyForOID(OID::createFromString("000000000000000000000001"));
        ASSERT_NOT_EQUALS(rid1, rid2);
    } else {
        // This test should not be run in release builds as the assertion won't be in there.
        invariant(false, "Deliberately crash here so the test doesn't fail on release builds");
    }
}

}  // namespace
}  // namespace mongo

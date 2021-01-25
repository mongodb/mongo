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

#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

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
    RecordId locA(OID::gen());
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
    RecordId loc1(OID::gen());
    RecordId loc2(OID::gen());
    RecordId loc3(OID::gen());
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

TEST(RecordId, OidTest) {
    RecordId ridNull;
    ASSERT(ridNull.isNull());
    ASSERT(!ridNull.isReserved());
    ASSERT(!ridNull.isValid());
    ASSERT(!ridNull.isNormal());

    RecordId null2;
    ASSERT(null2 == ridNull);

    OID oid1 = OID::gen();
    RecordId rid1(oid1);
    ASSERT(rid1.isNormal());
    ASSERT(!rid1.isReserved());
    ASSERT(rid1.isValid());
    ASSERT_EQ(rid1.as<OID>(), oid1);
    ASSERT_GT(rid1, ridNull);
    ASSERT_LT(ridNull, rid1);
}

TEST(RecordId, NullTest) {
    // The int64 format should be considered null if its value is 0. Likewise, the value should be
    // interpreted as int64_t(0) if it is null.
    RecordId nullRid(0);
    ASSERT(nullRid.isNull());

    RecordId rid0;
    ASSERT(rid0.isNull());
    ASSERT_EQ(0, rid0.as<int64_t>());
    ASSERT_EQ(nullRid, rid0);
}

TEST(RecordId, OidTestCompare) {
    RecordId ridNull;
    RecordId rid0(OID::createFromString("000000000000000000000000"));
    ASSERT_GT(rid0, ridNull);

    RecordId rid1(OID::createFromString("000000000000000000000001"));
    ASSERT_GT(rid1, rid0);
    ASSERT_EQ(RecordId::min<OID>(), rid0);
    ASSERT_GT(RecordId::min<OID>(), ridNull);

    RecordId rid2(OID::createFromString("000000000000000000000002"));
    ASSERT_GT(rid2, rid1);
    RecordId rid3(OID::createFromString("ffffffffffffffffffffffff"));
    ASSERT_GT(rid3, rid2);
    ASSERT_GT(rid3, rid0);

    ASSERT_EQ(RecordId::max<OID>(), rid3);
    ASSERT_GT(RecordId::max<OID>(), rid0);
}

TEST(RecordId, Reservations) {
    // It's important that reserved IDs like this never change.
    RecordId ridReserved(RecordId::kMaxRepr - (1024 * 1024));
    ASSERT_EQ(ridReserved,
              RecordId::reservedIdFor<int64_t>(RecordId::Reservation::kWildcardMultikeyMetadataId));
    ASSERT(ridReserved.isReserved());
    ASSERT(ridReserved.isValid());
    ASSERT(!ridReserved.isNormal());

    RecordId oidReserved(OID::createFromString("fffffffffffffffffff00000"));
    ASSERT_EQ(oidReserved,
              RecordId::reservedIdFor<OID>(RecordId::Reservation::kWildcardMultikeyMetadataId));
    ASSERT(oidReserved.isReserved());
    ASSERT(oidReserved.isValid());
    ASSERT(!oidReserved.isNormal());
}

// RecordIds of different formats may not be compared.
DEATH_TEST(RecordId, UnsafeComparison, "Invariant failure") {
    RecordId rid1(1);
    RecordId rid2(OID::createFromString("000000000000000000000001"));
    ASSERT_NOT_EQUALS(rid1, rid2);
}

}  // namespace
}  // namespace mongo

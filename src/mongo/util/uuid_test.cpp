/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace {

TEST(UUIDTest, UUIDCollisionTest) {
    // Generate some UUIDs and check that they do not collide.
    // NOTE: if this test fails, it is not necessarily a bug. However, if it
    // begins to fail often and on specific platforms, we should investigate
    // the quality of our entropy on those systems.
    stdx::unordered_set<UUID, UUID::Hash> uuids;
    for (int i = 0; i < 10000; i++) {
        ASSERT(uuids.emplace(UUID::gen()).second);
    }
}

TEST(UUIDTest, isUUIDStringTest) {
    // Several valid strings
    ASSERT(UUID::isUUIDString("00000000-0000-4000-8000-000000000000"));
    ASSERT(UUID::isUUIDString("01234567-9abc-4def-9012-3456789abcde"));
    ASSERT(UUID::isUUIDString("dddddddd-eeee-4fff-aaaa-bbbbbbbbbbbb"));
    ASSERT(UUID::isUUIDString("A9A9A9A9-BEDF-4DD9-B001-222345716283"));

    // No version or variant set
    ASSERT(UUID::isUUIDString("00000000-0000-0000-0000-000000000000"));

    // Mixed casing is weird, but technically legal
    ASSERT(UUID::isUUIDString("abcdefAB-CDEF-4000-AaAa-FDFfdffd9991"));

    // Wrong number of Hyphens
    ASSERT(!UUID::isUUIDString("00000000-0000-4000-8000-0000000000-00"));
    ASSERT(!UUID::isUUIDString("000000000000-4000-8000-000000000000"));
    ASSERT(!UUID::isUUIDString("00000000000040008000000000000000"));

    // Hyphens in the wrong places
    ASSERT(!UUID::isUUIDString("dddddd-ddeeee-4fff-aaaa-bbbbbbbbbbbb"));
    ASSERT(!UUID::isUUIDString("ddddddd-deeee-4fff-aaaa-bbbbbbbbbbbb"));
    ASSERT(!UUID::isUUIDString("d-d-d-dddddeeee4fffaaaa-bbbbbbbbbbbb"));

    // Illegal characters
    ASSERT(!UUID::isUUIDString("samsamsa-sams-4sam-8sam-samsamsamsam"));

    // Too short
    ASSERT(!UUID::isUUIDString("A9A9A9A9-BEDF-4DD9-B001"));
    ASSERT(!UUID::isUUIDString("dddddddd-eeee-4fff-aaaa-bbbbbbbbbbb"));

    // Too long
    ASSERT(!UUID::isUUIDString("01234567-9abc-4def-9012-3456789abcdef"));
    ASSERT(!UUID::isUUIDString("0123004567-9abc-4def-9012-3456789abcdef0000"));
}

TEST(UUIDTest, toAndFromString) {
    // String -> UUID -> string
    auto s1 = "00000000-0000-4000-8000-000000000000";
    auto uuid1Res = UUID::parse(s1);
    ASSERT_OK(uuid1Res);
    auto uuid1 = uuid1Res.getValue();
    ASSERT(UUID::isUUIDString(s1));
    ASSERT(UUID::isUUIDString(uuid1.toString()));
    ASSERT_EQUALS(uuid1.toString(), s1);

    // UUID -> string -> UUID
    auto uuid2 = UUID::gen();
    auto s2 = uuid2.toString();
    ASSERT(UUID::isUUIDString(s2));

    auto uuid2FromStringRes = UUID::parse(s2);
    ASSERT_OK(uuid2FromStringRes);
    auto uuid2FromString = uuid2FromStringRes.getValue();
    ASSERT_EQUALS(uuid2FromString, uuid2);
    ASSERT_EQUALS(uuid2FromString.toString(), s2);

    // Two UUIDs constructed from the same string are equal
    auto s3 = "01234567-9abc-4def-9012-3456789abcde";
    auto uuid3Res = UUID::parse(s3);
    auto uuid3AgainRes = UUID::parse(s3);
    ASSERT_OK(uuid3Res);
    ASSERT_OK(uuid3AgainRes);
    ASSERT_EQUALS(uuid3Res.getValue(), uuid3AgainRes.getValue());

    // Two UUIDs constructed from differently cased string are equal
    auto sLower = "00000000-aaaa-4000-8000-000000000000";
    auto sUpper = "00000000-AAAA-4000-8000-000000000000";
    auto uuidLowerRes = UUID::parse(sLower);
    auto uuidUpperRes = UUID::parse(sUpper);
    ASSERT_OK(uuidLowerRes);
    ASSERT_OK(uuidUpperRes);
    auto uuidLower = uuidLowerRes.getValue();
    auto uuidUpper = uuidUpperRes.getValue();
    ASSERT_EQUALS(uuidLower, uuidUpper);

    // Casing is not preserved on round trip, both become lowercase
    ASSERT_EQUALS(uuidLower.toString(), uuidUpper.toString());
    ASSERT_EQUALS(uuidLower.toString(), sLower);
    ASSERT_EQUALS(uuidUpper.toString(), sLower);
    ASSERT_NOT_EQUALS(uuidUpper.toString(), sUpper);

    // UUIDs constructed from different strings are not equal
    auto s4 = "01234567-9abc-4def-9012-3456789abcde";
    auto s5 = "01234567-0000-4def-9012-3456789abcde";
    ASSERT_NOT_EQUALS(UUID::parse(s4).getValue(), UUID::parse(s5).getValue());

    // UUIDs cannot be constructed from invalid strings
    ASSERT_NOT_OK(UUID::parse("00000000000040008000000000000000"));
    ASSERT_NOT_OK(UUID::parse("d-d-d-dddddeeee4fffaaaa-bbbbbbbbbbbb"));
    ASSERT_NOT_OK(UUID::parse("samsamsa-sams-4sam-8sam-samsamsamsam"));
}

TEST(UUIDTest, toAndFromBSONTest) {
    // UUID -> BSON -> UUID
    UUID uuid = UUID::gen();
    auto uuidBSON = uuid.toBSON();
    auto uuid2Res = UUID::parse(uuidBSON.getField("uuid"));
    ASSERT_OK(uuid2Res);
    auto uuid2 = uuid2Res.getValue();
    ASSERT_EQUALS(uuid, uuid2Res);

    // BSON -> UUID -> BSON
    uint8_t uuidBytes[] = {0, 0, 0, 0, 0, 0, 0x40, 0, 0x80, 0, 0, 0, 0, 0, 0, 0};
    auto bson = BSON("uuid" << BSONBinData(uuidBytes, 16, newUUID));
    auto uuidFromBSON = UUID::parse(bson.getField("uuid"));
    ASSERT_OK(uuidFromBSON);

    auto uuidBSON2 = uuid2.toBSON();
    ASSERT_EQUALS(uuidBSON.woCompare(uuidBSON2), 0);

    // UUIDs cannot be constructed from invalid BSON elements
    auto bson2 = BSON("uuid"
                      << "sam");
    ASSERT_NOT_OK(UUID::parse(bson2.getField("uuid")));
    auto bson3 = BSON("uuid"
                      << "dddddddd-eeee-4fff-aaaa-bbbbbbbbbbbb");
    ASSERT_NOT_OK(UUID::parse(bson3.getField("uuid")));
    auto bson4 = BSON("uuid" << 14);
    ASSERT_NOT_OK(UUID::parse(bson4.getField("uuid")));
}

}  // namespace
}  // namespace mongo

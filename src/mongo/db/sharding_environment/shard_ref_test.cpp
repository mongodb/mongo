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

#include "mongo/db/sharding_environment/shard_ref.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/sharding_environment/shard_handle.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

#include <string>

namespace mongo {
namespace {

TEST(ShardRef, StringVariant) {
    ShardRef ref{std::string{"myShard"}};
    ASSERT_TRUE(ref.isString());
    ASSERT_FALSE(ref.isUUID());
    ASSERT_EQUALS(ref.getString(), std::string{"myShard"});
    ASSERT_EQUALS(ref.toString(), std::string{"myShard"});
}

TEST(ShardRef, UUIDVariant) {
    UUID uuid = UUID::gen();
    ShardRef ref{uuid};
    ASSERT_FALSE(ref.isString());
    ASSERT_TRUE(ref.isUUID());
    ASSERT_EQUALS(ref.getUUID(), uuid);
}

TEST(ShardRef, ImplicitConversionFromShardId) {
    ShardId id{"someShardName"};
    ShardRef ref = id;
    ASSERT_TRUE(ref.isString());
    ASSERT_EQUALS(ref.getString(), std::string{"someShardName"});
}

TEST(ShardRef, ImplicitConversionToShardId) {
    ShardRef ref{std::string{"someShardName"}};
    ShardId id = ref;
    ASSERT_EQUALS(id.toString(), std::string{"someShardName"});
}

TEST(ShardRef, GetShardIdReturnsReferenceIntoShardRef) {
    ShardRef ref{std::string{"someShardName"}};
    const ShardId& id = ref.getShardId();
    ASSERT_EQUALS(id.toString(), std::string{"someShardName"});
    // The returned reference must alias the ShardId stored inside the ShardRef, not a temporary.
    ASSERT_EQUALS(&id, &ref.getShardId());
}

TEST(ShardRef, StreamingUsesToStringAndDoesNotInvariantOnUUID) {
    ShardRef strRef{std::string{"myShard"}};
    ASSERT_EQUALS(str::stream() << strRef, std::string{"myShard"});

    // Streaming a UUID-backed ShardRef must render via toString() rather than routing through the
    // implicit ShardId conversion, which would invariant.
    UUID uuid = UUID::gen();
    ShardRef uuidRef{uuid};
    ASSERT_EQUALS(str::stream() << uuidRef, uuid.toString());
}

TEST(ShardRef, EqualityStringVsString) {
    ShardRef a{std::string{"x"}};
    ShardRef b{std::string{"x"}};
    ShardRef c{std::string{"y"}};
    ASSERT_TRUE(a == b);
    ASSERT_FALSE(a == c);
    ASSERT_TRUE(a != c);
}

TEST(ShardRef, EqualityUUIDvsUUID) {
    UUID uuid = UUID::gen();
    ShardRef a{uuid};
    ShardRef b{uuid};
    UUID other = UUID::gen();
    ShardRef c{other};
    ASSERT_TRUE(a == b);
    ASSERT_FALSE(a == c);
}

TEST(ShardRef, ParseStringElement) {
    BSONObjBuilder bob;
    bob.append("primary", "myShard");
    BSONObj obj = bob.obj();

    ShardRef ref = ShardRef::parse(obj["primary"]);
    ASSERT_TRUE(ref.isString());
    ASSERT_FALSE(ref.isUUID());
    ASSERT_EQUALS(ref.getString(), std::string{"myShard"});
}

TEST(ShardRef, ParseUUIDElement) {
    UUID uuid = UUID::gen();
    BSONObjBuilder bob;
    uuid.appendToBuilder(&bob, "primary");
    BSONObj obj = bob.obj();

    ShardRef ref = ShardRef::parse(obj["primary"]);
    ASSERT_FALSE(ref.isString());
    ASSERT_TRUE(ref.isUUID());
    ASSERT_EQUALS(ref.getUUID(), uuid);
}

// A string whose value happens to look like a UUID must still resolve as a string ShardRef (i.e.
// a shard name), not a UUID, because the BSON type is string and not BinData/newUUID.
TEST(ShardRef, ParseStringThatLooksLikeUUID) {
    UUID uuid = UUID::gen();
    std::string uuidStr = uuid.toString();

    BSONObjBuilder bob;
    bob.append("primary", uuidStr);
    BSONObj obj = bob.obj();

    ShardRef ref = ShardRef::parse(obj["primary"]);
    ASSERT_TRUE(ref.isString());
    ASSERT_FALSE(ref.isUUID());
    ASSERT_EQUALS(ref.getString(), uuidStr);
}

TEST(ShardRef, EqualityWithShardId) {
    ShardId id{"shardA"};
    ShardRef strRef{std::string{"shardA"}};
    ShardRef otherRef{std::string{"shardB"}};
    ShardRef uuidRef{UUID::gen()};

    // ShardRef(string) == ShardId with same name
    ASSERT_TRUE(strRef == id);
    ASSERT_TRUE(id == strRef);

    // ShardRef(string) != ShardId with different name
    ASSERT_TRUE(otherRef != id);
    ASSERT_TRUE(id != otherRef);

    // ShardRef(UUID) is never equal to a ShardId
    ASSERT_FALSE(uuidRef == id);
    ASSERT_FALSE(id == uuidRef);
    ASSERT_TRUE(uuidRef != id);
    ASSERT_TRUE(id != uuidRef);
}

TEST(ShardRef, OrderingStringVsString) {
    ShardRef a{std::string{"aaa"}};
    ShardRef b{std::string{"bbb"}};

    ASSERT_TRUE(a < b);
    ASSERT_FALSE(b < a);
    ASSERT_FALSE(a < a);

    ASSERT_TRUE(b > a);
    ASSERT_FALSE(a > b);

    ASSERT_TRUE(a <= b);
    ASSERT_TRUE(a <= a);
    ASSERT_FALSE(b <= a);

    ASSERT_TRUE(b >= a);
    ASSERT_TRUE(a >= a);
    ASSERT_FALSE(a >= b);
}

TEST(ShardRef, OrderingUUIDvsUUID) {
    // Generate two distinct UUIDs and verify strict ordering is consistent.
    UUID uuid1 = UUID::gen();
    UUID uuid2 = UUID::gen();
    ShardRef a{uuid1};
    ShardRef b{uuid2};

    // Exactly one of a<b or b<a must hold (strict weak ordering), and neither a<a nor b<b.
    ASSERT_TRUE((a < b) != (b < a));
    ASSERT_FALSE(a < a);
    ASSERT_FALSE(b < b);
}

TEST(ShardRef, UsableInSort) {
    // Verify that ShardRef is sortable (required by IDL comparison operators on
    // NamespacePlacementType which has generate_comparison_operators: true).
    std::vector<ShardRef> refs{
        ShardRef{std::string{"zzz"}},
        ShardRef{std::string{"aaa"}},
        ShardRef{std::string{"mmm"}},
    };
    std::sort(refs.begin(), refs.end());
    ASSERT_EQUALS(refs[0].getString(), std::string{"aaa"});
    ASSERT_EQUALS(refs[1].getString(), std::string{"mmm"});
    ASSERT_EQUALS(refs[2].getString(), std::string{"zzz"});
}

TEST(ShardRef, EqualityWithShardHandleStringRef) {
    ShardHandle handle{ShardId{"myShard"}, boost::none};
    ShardRef matchingRef{std::string{"myShard"}};
    ShardRef mismatchRef{std::string{"otherShard"}};

    ASSERT_TRUE(handle == matchingRef);
    ASSERT_TRUE(matchingRef == handle);
    ASSERT_FALSE(handle != matchingRef);

    ASSERT_FALSE(handle == mismatchRef);
    ASSERT_FALSE(mismatchRef == handle);
    ASSERT_TRUE(handle != mismatchRef);
}

TEST(ShardRef, EqualityWithShardHandleUuidRef) {
    UUID uuid = UUID::gen();
    ShardHandle handle{ShardId{"myShard"}, uuid};
    ShardRef matchingRef{uuid};
    ShardRef mismatchRef{UUID::gen()};

    ASSERT_TRUE(handle == matchingRef);
    ASSERT_TRUE(matchingRef == handle);
    ASSERT_FALSE(handle != matchingRef);

    ASSERT_FALSE(handle == mismatchRef);
    ASSERT_FALSE(mismatchRef == handle);
}

TEST(ShardRef, UuidRefReturnsFalseWhenHandleHasNoUuid) {
    ShardHandle handle{ShardId{"myShard"}, boost::none};
    ShardRef uuidRef{UUID::gen()};

    ASSERT_FALSE(handle == uuidRef);
    ASSERT_FALSE(uuidRef == handle);
    ASSERT_TRUE(handle != uuidRef);
}

}  // namespace
}  // namespace mongo

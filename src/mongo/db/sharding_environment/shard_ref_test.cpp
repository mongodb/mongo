// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/sharding_environment/shard_ref.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/unittest/unittest.h"
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

}  // namespace
}  // namespace mongo

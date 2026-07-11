// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/global_catalog/type_shard.h"

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/uuid.h"

namespace {

using namespace mongo;

using std::string;

TEST(ShardType, MissingName) {
    BSONObj obj = BSON(ShardType::host("localhost:27017"));
    StatusWith<ShardType> shardRes = ShardType::fromBSON(obj);
    ASSERT_FALSE(shardRes.isOK());
}

TEST(ShardType, MissingHost) {
    BSONObj obj = BSON(ShardType::name("shard0000"));
    StatusWith<ShardType> shardRes = ShardType::fromBSON(obj);
    ASSERT_FALSE(shardRes.isOK());
}

TEST(ShardType, OnlyMandatory) {
    BSONObj obj = BSON(ShardType::name("shard0000") << ShardType::host("localhost:27017"));
    StatusWith<ShardType> shardRes = ShardType::fromBSON(obj);
    ASSERT(shardRes.isOK());
    ShardType shard = shardRes.getValue();
    ASSERT(shard.validate().isOK());
}

TEST(ShardType, AllOptionalsPresent) {
    BSONObj obj = BSON(ShardType::name("shard0000")
                       << ShardType::host("localhost:27017") << ShardType::draining(true));
    StatusWith<ShardType> shardRes = ShardType::fromBSON(obj);
    ASSERT(shardRes.isOK());
    ShardType shard = shardRes.getValue();
    ASSERT(shard.validate().isOK());
}

TEST(ShardType, ValidateFailsWhenHandleAbsent) {
    ShardType shard;
    shard.setHost("localhost:27017");
    Status status = shard.validate();
    ASSERT_FALSE(status.isOK());
    ASSERT_EQ(status.code(), ErrorCodes::NoSuchKey);
}

TEST(ShardType, ValidateFailsWhenNameEmpty) {
    ShardType shard("", "localhost:27017");
    Status status = shard.validate();
    ASSERT_FALSE(status.isOK());
    ASSERT_EQ(status.code(), ErrorCodes::NoSuchKey);
}

TEST(ShardType, BadType) {
    BSONObj obj = BSON(ShardType::name() << 0);
    StatusWith<ShardType> shardRes = ShardType::fromBSON(obj);
    ASSERT_FALSE(shardRes.isOK());
}

TEST(ShardType, UUIDAbsentParsedFromBSON) {
    BSONObj obj = BSON(ShardType::name("shard0000") << ShardType::host("localhost:27017"));
    StatusWith<ShardType> shardRes = ShardType::fromBSON(obj);
    ASSERT(shardRes.isOK());
    const ShardType& shard = shardRes.getValue();
    ASSERT_EQ(shard.getName(), "shard0000");
    ASSERT_FALSE(shard.getUuid().has_value());
}

TEST(ShardType, UUIDPresentParsedFromBSON) {
    UUID uuid = UUID::gen();
    BSONObjBuilder builder;
    builder.append(ShardType::name(), "shard0000");
    builder.append(ShardType::host(), "localhost:27017");
    uuid.appendToBuilder(&builder, ShardType::uuid.name());
    StatusWith<ShardType> shardRes = ShardType::fromBSON(builder.obj());
    ASSERT(shardRes.isOK());
    const ShardType& shard = shardRes.getValue();
    ASSERT_EQ(shard.getName(), "shard0000");
    ASSERT(shard.getUuid().has_value());
    ASSERT_EQ(shard.getUuid().value(), uuid);
}

TEST(ShardType, UUIDBadTypeNotBinData) {
    BSONObj obj = BSON(ShardType::name("shard0000")
                       << ShardType::host("localhost:27017") << ShardType::uuid.name() << 42);
    StatusWith<ShardType> shardRes = ShardType::fromBSON(obj);
    ASSERT_FALSE(shardRes.isOK());
}

TEST(ShardType, UUIDBadBinDataSubtype) {
    BSONObjBuilder builder;
    builder.append(ShardType::name(), "shard0000");
    builder.append(ShardType::host(), "localhost:27017");
    uint8_t bytes[16] = {};
    builder.appendBinData(
        ShardType::uuid.name(), sizeof(bytes), BinDataType::BinDataGeneral, bytes);
    StatusWith<ShardType> shardRes = ShardType::fromBSON(builder.obj());
    ASSERT_FALSE(shardRes.isOK());
}

TEST(ShardType, ToBSONRoundtripWithUUID) {
    UUID uuid = UUID::gen();
    BSONObjBuilder builder;
    builder.append(ShardType::name(), "shard0000");
    builder.append(ShardType::host(), "localhost:27017");
    uuid.appendToBuilder(&builder, ShardType::uuid.name());
    BSONObj original = builder.obj();
    const ShardType shard = ShardType::fromBSON(original).getValue();
    BSONObj serialized = shard.toBSON();
    const ShardType roundtripped = ShardType::fromBSON(serialized).getValue();
    ASSERT_EQ(roundtripped.getName(), "shard0000");
    ASSERT_EQ(roundtripped.getUuid(), uuid);
}

TEST(ShardType, ConstructorWithUUID) {
    UUID uuid = UUID::gen();
    ShardType shard("shard0000", uuid, "localhost:27017");
    ASSERT_EQ(shard.getName(), "shard0000");
    ASSERT(shard.getUuid().has_value());
    ASSERT_EQ(shard.getUuid().value(), uuid);
    ASSERT(shard.validate().isOK());
}

TEST(ShardType, ConstructorWithoutUUID) {
    ShardType shard("shard0000", "localhost:27017");
    ASSERT_EQ(shard.getName(), "shard0000");
    ASSERT(!shard.getUuid().has_value());
    ASSERT(shard.validate().isOK());
}

TEST(ShardType, ToBSONIncludesUUID) {
    UUID uuid = UUID::gen();
    BSONObjBuilder builder;
    builder.append(ShardType::name(), "shard0000");
    builder.append(ShardType::host(), "localhost:27017");
    uuid.appendToBuilder(&builder, ShardType::uuid.name());
    StatusWith<ShardType> shardRes = ShardType::fromBSON(builder.obj());
    ASSERT(shardRes.isOK());
    const ShardType& shard = shardRes.getValue();

    BSONObj serialized = shard.toBSON();
    ASSERT(serialized.hasField(ShardType::uuid.name()));
    ASSERT_EQ(serialized[ShardType::uuid.name()].binDataType(), BinDataType::newUUID);
    ASSERT_EQ(uassertStatusOK(UUID::parse(serialized[ShardType::uuid.name()])), uuid);
}

TEST(ShardType, ToBSONRoundTripWithoutUUID) {
    BSONObj original = BSON(ShardType::name("shard0001") << ShardType::host("localhost:27017"));
    StatusWith<ShardType> shardRes = ShardType::fromBSON(original);
    ASSERT(shardRes.isOK());
    const ShardType& shard = shardRes.getValue();

    ASSERT_FALSE(shard.getUuid().has_value());

    BSONObj serialized = shard.toBSON();
    ASSERT_FALSE(serialized.hasField(ShardType::uuid.name()));

    StatusWith<ShardType> roundTripped = ShardType::fromBSON(serialized);
    ASSERT(roundTripped.isOK());
    ASSERT_EQ(roundTripped.getValue().getName(), shard.getName());
    ASSERT_FALSE(roundTripped.getValue().getUuid().has_value());
}

}  // unnamed namespace

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

#include "mongo/db/global_catalog/type_shard.h"

#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
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

}  // unnamed namespace
